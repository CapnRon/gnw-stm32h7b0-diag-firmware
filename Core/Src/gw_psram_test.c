#include "gw_psram_test.h"
#include "main.h"

#include <string.h>

/* IS66WVS4M8FALL/BLL command set (datasheet Table 4.1), SPI mode (1-1-1) only. */
#define PSRAM_CMD_READ       0x03u
#define PSRAM_CMD_FAST_READ  0x0Bu
#define PSRAM_CMD_WRITE      0x02u
#define PSRAM_CMD_RESET_EN   0x66u
#define PSRAM_CMD_RESET      0x99u
#define PSRAM_CMD_READ_ID    0x9Fu

static OSPI_HandleTypeDef *s_hospi;

static void psram_cmd(uint8_t instr, uint32_t addr, bool has_addr, uint8_t dummy,
                      uint8_t *data, size_t len, bool is_write)
{
    OSPI_RegularCmdTypeDef c;
    memset(&c, 0, sizeof(c));

    c.OperationType     = HAL_OSPI_OPTYPE_COMMON_CFG;
    c.FlashId            = 0;
    c.Instruction         = instr;
    c.InstructionSize     = HAL_OSPI_INSTRUCTION_8_BITS;
    c.InstructionMode     = HAL_OSPI_INSTRUCTION_1_LINE;
    c.AddressMode         = has_addr ? HAL_OSPI_ADDRESS_1_LINE : HAL_OSPI_ADDRESS_NONE;
    c.AddressSize         = HAL_OSPI_ADDRESS_24_BITS;
    c.Address             = addr;
    c.AlternateBytesMode  = HAL_OSPI_ALTERNATE_BYTES_NONE;
    c.DummyCycles         = dummy;
    c.DataMode            = (len > 0) ? HAL_OSPI_DATA_1_LINE : HAL_OSPI_DATA_NONE;
    c.NbData              = len;
    c.DQSMode             = HAL_OSPI_DQS_DISABLE;
    c.SIOOMode            = HAL_OSPI_SIOO_INST_EVERY_CMD;
    c.InstructionDtrMode  = HAL_OSPI_INSTRUCTION_DTR_DISABLE;

    wdog_refresh();

    if (HAL_OSPI_Command(s_hospi, &c, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        Error_Handler();
    }

    if (len > 0) {
        if (is_write) {
            if (HAL_OSPI_Transmit(s_hospi, data, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
                Error_Handler();
            }
        } else {
            if (HAL_OSPI_Receive(s_hospi, data, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
                Error_Handler();
            }
        }
    }
}

void PSRAM_Init(OSPI_HandleTypeDef *hospi)
{
    s_hospi = hospi;

    /* Software reset (RESET ENABLE then RESET, datasheet 5.8). Device also
     * powers up in SPI standby already, this just guarantees a known state
     * regardless of what mode a previous firmware left it in. */
    psram_cmd(PSRAM_CMD_RESET_EN, 0, false, 0, NULL, 0, true);
    HAL_Delay(1);
    psram_cmd(PSRAM_CMD_RESET, 0, false, 0, NULL, 0, true);
    HAL_Delay(1); /* real tPUmin is 150us (datasheet 3.); 1ms is a comfortable margin. */
}

bool PSRAM_ReadID(uint8_t *mfid, uint8_t *kgd)
{
    uint8_t id[2] = {0};

    /* Read ID (9Fh): 24-bit don't-care address, 0 wait cycles in SPI mode,
     * then MF ID followed by KGD stream out (datasheet 5.7 / Fig 5.11). */
    psram_cmd(PSRAM_CMD_READ_ID, 0, true, 0, id, sizeof(id), false);

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
    while (len > 0) {
        uint32_t n = chunk_len(address, (uint32_t)len);

        /* SPI Write (02h, 1-1-1, 0 wait cycles). */
        psram_cmd(PSRAM_CMD_WRITE, address, true, 0, (uint8_t *)data, n, true);

        address += n;
        data += n;
        len -= n;
    }
}

void PSRAM_Read(uint32_t address, uint8_t *data, size_t len)
{
    while (len > 0) {
        uint32_t n = chunk_len(address, (uint32_t)len);

        /* SPI Fast Read (0Bh, 1-1-1, 8 wait cycles, up to 104MHz). */
        psram_cmd(PSRAM_CMD_FAST_READ, address, true, 8, data, n, false);

        address += n;
        data += n;
        len -= n;
    }
}
