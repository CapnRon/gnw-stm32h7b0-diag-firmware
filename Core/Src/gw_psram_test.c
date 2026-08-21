#include "gw_psram_test.h"
#include "gw_ospi_bus.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

/* IS66WVS4M8FALL/BLL command set (datasheet Table 4.1). PSRAM_CMD_READ is
 * unused (kept for reference -- normal 33MHz-max read, superseded by
 * Fast Read for anything above that). Quad opcodes/dummy counts match
 * game-and-watch-retro-go-sd's validated cmds_psram table exactly. */
#define PSRAM_CMD_READ        0x03u
#define PSRAM_CMD_FAST_READ   0x0Bu
#define PSRAM_CMD_QUAD_READ   0xEBu  /* 1-4-4, 6 dummy cycles */
#define PSRAM_CMD_WRITE       0x02u
#define PSRAM_CMD_QUAD_WRITE  0x38u  /* 1-4-4, 0 dummy cycles */
#define PSRAM_CMD_RESET_EN    0x66u
#define PSRAM_CMD_RESET       0x99u
#define PSRAM_CMD_READ_ID     0x9Fu

static OSPI_HandleTypeDef *s_hospi;
static psram_io_mode_t s_io_mode = PSRAM_IO_SPI;

void PSRAM_SetIoMode(psram_io_mode_t mode)
{
    s_io_mode = mode;
}

psram_io_mode_t PSRAM_GetIoMode(void)
{
    return s_io_mode;
}

static void psram_cmd(uint8_t instr, uint32_t addr, bool has_addr, uint8_t dummy,
                      uint8_t *data, size_t len, bool is_write, bool quad)
{
    OSPI_RegularCmdTypeDef c;
    memset(&c, 0, sizeof(c));

    c.OperationType     = HAL_OSPI_OPTYPE_COMMON_CFG;
    c.FlashId            = 0;
    c.Instruction         = instr;
    c.InstructionSize     = HAL_OSPI_INSTRUCTION_8_BITS;
    c.InstructionMode     = HAL_OSPI_INSTRUCTION_1_LINE; /* command byte always single-line, even in quad mode */
    c.AddressMode         = has_addr
        ? (quad ? HAL_OSPI_ADDRESS_4_LINES : HAL_OSPI_ADDRESS_1_LINE)
        : HAL_OSPI_ADDRESS_NONE;
    c.AddressSize         = HAL_OSPI_ADDRESS_24_BITS;
    c.Address             = addr;
    c.AlternateBytesMode  = HAL_OSPI_ALTERNATE_BYTES_NONE;
    c.DummyCycles         = dummy;
    c.DataMode            = (len > 0)
        ? (quad ? HAL_OSPI_DATA_4_LINES : HAL_OSPI_DATA_1_LINE)
        : HAL_OSPI_DATA_NONE;
    c.NbData              = len;
    c.DQSMode             = HAL_OSPI_DQS_DISABLE;
    c.SIOOMode            = HAL_OSPI_SIOO_INST_EVERY_CMD;
    c.InstructionDtrMode  = HAL_OSPI_INSTRUCTION_DTR_DISABLE;

    wdog_refresh();

    /* CS bracket is mode-aware: no-op for the hardware-NCS pins (PE11/PC11),
     * GPIO toggle for PE9. */
    OspiBus_SelectPsram();

    if (HAL_OSPI_Command(s_hospi, &c, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        OspiBus_DeselectPsram();
        Error_Handler();
    }

    if (len > 0) {
        if (is_write) {
            if (HAL_OSPI_Transmit(s_hospi, data, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
                OspiBus_DeselectPsram();
                Error_Handler();
            }
        } else {
            if (HAL_OSPI_Receive(s_hospi, data, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
                OspiBus_DeselectPsram();
                Error_Handler();
            }
        }
    }

    OspiBus_DeselectPsram();
}

void PSRAM_Init(OSPI_HandleTypeDef *hospi)
{
    s_hospi = hospi;

    OspiBus_Init(hospi);

    /* Auto-detect which pin carries the PSRAM's CE#: PE11 (hardware OSPI
     * NCS), PC11 (alternate NCS location) or PE9 (plain GPIO). */
    ospi_bus_psram_cs_t cs = OspiBus_ProbePsram();
    switch (cs) {
    case OSPI_BUS_PSRAM_CS_PE11:
        printf("PSRAM CS: PE11 (OSPI NCS)\n");
        break;
    case OSPI_BUS_PSRAM_CS_PC11:
        printf("PSRAM CS: PC11 (OSPI NCS alt)\n");
        break;
    case OSPI_BUS_PSRAM_CS_PE9:
        printf("PSRAM CS: PE9 (GPIO)\n");
        break;
    default:
        printf("PSRAM CS: NOT FOUND (tried PE11, PC11, PE9)\n");
        return;
    }

    /* Software reset (RESET ENABLE then RESET, datasheet 5.8). Device also
     * powers up in SPI standby already, this just guarantees a known state
     * regardless of what mode a previous firmware left it in. */
    psram_cmd(PSRAM_CMD_RESET_EN, 0, false, 0, NULL, 0, true, false);
    HAL_Delay(1);
    psram_cmd(PSRAM_CMD_RESET, 0, false, 0, NULL, 0, true, false);
    HAL_Delay(1); /* real tPUmin is 150us (datasheet 3.); 1ms is a comfortable margin. */
}

bool PSRAM_ReadID(uint8_t *mfid, uint8_t *kgd)
{
    uint8_t id[2] = {0};

    /* Read ID (9Fh): 24-bit don't-care address, 0 wait cycles in SPI mode,
     * then MF ID followed by KGD stream out (datasheet 5.7 / Fig 5.11). */
    psram_cmd(PSRAM_CMD_READ_ID, 0, true, 0, id, sizeof(id), false, false);

    *mfid = id[0];
    *kgd = id[1];

    return (id[1] & 0x01u) != 0;
}

static uint32_t chunk_len(uint32_t addr, uint32_t remaining)
{
    uint32_t to_page_end = PSRAM_PAGE_BYTES - (addr % PSRAM_PAGE_BYTES);
    return (remaining < to_page_end) ? remaining : to_page_end;
}

void PSRAM_Write(uint32_t address, const uint8_t *data, size_t len)
{
    bool quad = (s_io_mode == PSRAM_IO_QUAD);
    uint8_t instr = quad ? PSRAM_CMD_QUAD_WRITE : PSRAM_CMD_WRITE;

    while (len > 0) {
        uint32_t n = chunk_len(address, (uint32_t)len);

        psram_cmd(instr, address, true, 0, (uint8_t *)data, n, true, quad);

        address += n;
        data += n;
        len -= n;
    }
}

void PSRAM_Read(uint32_t address, uint8_t *data, size_t len)
{
    bool quad = (s_io_mode == PSRAM_IO_QUAD);
    uint8_t instr = quad ? PSRAM_CMD_QUAD_READ : PSRAM_CMD_FAST_READ;
    uint8_t dummy = quad ? 6 : 8;

    while (len > 0) {
        uint32_t n = chunk_len(address, (uint32_t)len);

        psram_cmd(instr, address, true, dummy, data, n, false, quad);

        address += n;
        data += n;
        len -= n;
    }
}

static bool s_mmap_active;

bool PSRAM_EnableMemoryMapped(void)
{
    if (s_mmap_active) {
        return true;
    }

    if (OspiBus_GetPsramCs() == OSPI_BUS_PSRAM_CS_PE9) {
        /* GPIO CS never gets driven during a hardware-autonomous
         * memory-mapped burst -- see the doc comment in gw_psram_test.h. */
        return false;
    }

    OSPI_RegularCmdTypeDef c;
    OSPI_MemoryMappedTypeDef mm;
    memset(&c, 0, sizeof(c));

    c.FlashId             = 0;
    c.InstructionSize     = HAL_OSPI_INSTRUCTION_8_BITS;
    c.InstructionMode     = HAL_OSPI_INSTRUCTION_1_LINE;
    c.AddressSize         = HAL_OSPI_ADDRESS_24_BITS;
    c.AddressMode         = HAL_OSPI_ADDRESS_4_LINES;
    c.AlternateBytesMode  = HAL_OSPI_ALTERNATE_BYTES_NONE;
    c.DataMode            = HAL_OSPI_DATA_4_LINES;
    c.DQSMode             = HAL_OSPI_DQS_DISABLE;
    c.SIOOMode            = HAL_OSPI_SIOO_INST_EVERY_CMD;
    c.InstructionDtrMode  = HAL_OSPI_INSTRUCTION_DTR_DISABLE;

    /* Read config: quad read 0xEB, 6 dummy cycles. */
    c.OperationType = HAL_OSPI_OPTYPE_READ_CFG;
    c.Instruction = PSRAM_CMD_QUAD_READ;
    c.DummyCycles = 6;
    if (HAL_OSPI_Command(s_hospi, &c, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        Error_Handler();
    }

    /* Write config: quad write 0x38, 0 dummy cycles.
     *
     * DQSMode = ENABLE here, deliberately different from the read config
     * above, per STM32H72x/73x errata 2.8.6 "Memory-mapped write error
     * response when DQS output is disabled": on parts with the OCTOSPI
     * memory-mapped region on the AXI bus, writes are always done
     * internally in 64-bit chunks, with DQS used to mask down to the
     * actual access size. With DQS disabled that masking breaks and the
     * write comes back as an AXI bus error -- reproducibly, on this
     * hardware, for literally any memory-mapped write, down to a single
     * word (Cortex-M double fault/lockup; confirmed independent of CPU
     * memory attributes, and identical whether the store came from
     * compiled CPU code or a debug probe writing the address directly).
     * This PSRAM has no physical DQS pin and doesn't need one -- this is
     * purely a workaround for the SoC's internal AXI-write masking, not
     * anything the external chip cares about. */
    c.OperationType = HAL_OSPI_OPTYPE_WRITE_CFG;
    c.Instruction = PSRAM_CMD_QUAD_WRITE;
    c.DummyCycles = 0;
    c.DQSMode = HAL_OSPI_DQS_ENABLE;
    if (HAL_OSPI_Command(s_hospi, &c, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        Error_Handler();
    }

    mm.TimeOutActivation = HAL_OSPI_TIMEOUT_COUNTER_DISABLE;
    mm.TimeOutPeriod = 0;
    if (HAL_OSPI_MemoryMapped(s_hospi, &mm) != HAL_OK) {
        Error_Handler();
    }

    s_mmap_active = true;
    return true;
}

void PSRAM_DisableMemoryMapped(void)
{
    if (!s_mmap_active) {
        return;
    }
    HAL_OSPI_Abort(s_hospi);
    s_mmap_active = false;
}

bool PSRAM_IsMemoryMapped(void)
{
    return s_mmap_active;
}
