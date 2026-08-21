#include "gw_nor_test.h"
#include "main.h"
#include "utils.h"

#include <string.h>

/* Standard SPI NOR opcodes, common across essentially every vendor
 * (Macronix/Winbond/ISSI/Cypress-Infineon) -- these two specifically work
 * in plain 1-1-1 SPI mode regardless of whether the chip is currently in
 * quad mode or what its address width is, which is exactly why they're
 * safe to use here without first knowing which chip is installed. */
#define NOR_CMD_RDID  0x9Fu
#define NOR_CMD_WREN  0x06u
#define NOR_CMD_RDSR  0x05u
/* READ, Sector Erase, and Page Program are NOT opcode-generic across
 * address widths -- the legacy 3-byte-address opcodes (0x03 READ, 0x20
 * SE, 0x02 PP) only accept 3 address bytes regardless of what
 * AddressSize the host clocks out. Sending a 4th address byte after them
 * desyncs the command: the chip silently truncates to the first 3 bytes
 * clocked, so e.g. address 0x00080000 (4 bytes: 00 08 00 00) gets
 * misread as address 0x000800 instead -- confirmed on real hardware
 * against the detected MX25U51245G (erase/program appeared to silently
 * no-op; the read verifying them was actually reading a different,
 * wrong address the whole time). Parts needing 32-bit addressing
 * (s_addr32) use the dedicated single-line 4-byte opcodes instead --
 * 0x13 ("4READ", the direct analog of 0x03), 0x21 ("SE4B"), and 0x12
 * ("4PP", no quad) -- matching gw_flash.c's proven-working
 * cmds_quad_32b_mx table for SE/PP on this same chip (that table only
 * has a quad 4-byte read opcode, 0xEC, not a single-line one; 0x13 is
 * the standard single-line equivalent documented across Macronix/
 * Winbond/GigaDevice/ISSI). 24-bit parts use the plain legacy opcodes,
 * which are correct there. */
#define NOR_CMD_READ_3B 0x03u
#define NOR_CMD_READ_4B 0x13u
#define NOR_CMD_SE_3B 0x20u
#define NOR_CMD_SE_4B 0x21u
#define NOR_CMD_PP_3B 0x02u
#define NOR_CMD_PP_4B 0x12u
#define NOR_STATUS_WIP 0x01u
#define NOR_STATUS_WEL 0x02u

typedef struct {
    uint8_t     id[3];
    const char *name;
    bool        addr32;
} nor_id_entry_t;

/* Mirrors the known-part table in gw_flash.c's jedec_map, name + address
 * width only (this module doesn't need erase/program command layouts). */
static const nor_id_entry_t nor_id_table[] = {
    { {0xC2, 0x25, 0x34}, "MX25U8035F",       false },
    { {0xC2, 0x25, 0x36}, "MX25U3232F",       false },
    { {0xC2, 0x25, 0x37}, "MX25U6432F",       false },
    { {0xC2, 0x25, 0x38}, "MX25U1283xF",      false },
    { {0xC2, 0x25, 0x39}, "MX25U25635F",      true  },
    { {0xC2, 0x25, 0x3A}, "MX25U51245G",      true  },
    { {0xC2, 0x24, 0x3A}, "MX25U51245G",      true  },
    { {0xC2, 0x95, 0x3A}, "MX25U51245G-54",   true  },
    { {0xC2, 0x25, 0x3B}, "MX66U1G45G",       true  },
    { {0xC2, 0x25, 0x3C}, "MX66U2G45G",       true  },
    { {0x01, 0x02, 0x20}, "S25FS512S",        true  },
    { {0x34, 0x2B, 0x1A}, "S25FS512S",        true  },
    { {0x9D, 0x70, 0x18}, "IS25WP128F",       false },
    { {0xEF, 0x60, 0x18}, "W25Q128JW-Q/N",    false },
    { {0xEF, 0x80, 0x18}, "W25Q128JW-M",      false },
    { {0xEF, 0x60, 0x20}, "W25Q512NW-Q/N",    true  },
    { {0xEF, 0x80, 0x20}, "W25Q512NW-M",      true  },
};

static OSPI_HandleTypeDef *s_hospi;
static bool s_addr32; /* set once ReadID identifies the part; defaults to 24-bit */

static uint8_t nor_read_opcode(void)
{
    return s_addr32 ? NOR_CMD_READ_4B : NOR_CMD_READ_3B;
}

static void nor_cmd(uint8_t instr, uint32_t addr, bool has_addr, uint8_t dummy,
                    uint8_t *data, size_t len)
{
    OSPI_RegularCmdTypeDef c;
    memset(&c, 0, sizeof(c));

    c.OperationType      = HAL_OSPI_OPTYPE_COMMON_CFG;
    c.FlashId             = 0;
    c.Instruction          = instr;
    c.InstructionSize      = HAL_OSPI_INSTRUCTION_8_BITS;
    c.InstructionMode      = HAL_OSPI_INSTRUCTION_1_LINE;
    c.AddressMode          = has_addr ? HAL_OSPI_ADDRESS_1_LINE : HAL_OSPI_ADDRESS_NONE;
    c.AddressSize          = s_addr32 ? HAL_OSPI_ADDRESS_32_BITS : HAL_OSPI_ADDRESS_24_BITS;
    c.Address              = addr;
    c.AlternateBytesMode   = HAL_OSPI_ALTERNATE_BYTES_NONE;
    c.DummyCycles          = dummy;
    c.DataMode             = (len > 0) ? HAL_OSPI_DATA_1_LINE : HAL_OSPI_DATA_NONE;
    c.NbData               = len;
    c.DQSMode              = HAL_OSPI_DQS_DISABLE;
    c.SIOOMode             = HAL_OSPI_SIOO_INST_EVERY_CMD;
    c.InstructionDtrMode   = HAL_OSPI_INSTRUCTION_DTR_DISABLE;

    wdog_refresh();

    /* NOR's CE# is PE11, the OSPI peripheral's own hardware NCS (AF11) --
     * the board's stock, unmodified pin. PE9 is the pin piggybacked on
     * for the PSRAM mod's plain-GPIO CS, not NOR's. The peripheral drives
     * PE11 automatically for every transaction; this only works correctly
     * as long as PE11 is actually left muxed to the OSPI AF function --
     * see OspiBus_ProbePsram()'s cleanup in gw_ospi_bus.c, which restores
     * that muxing after a failed PSRAM probe (it tries PE11 as one of its
     * own candidates and would otherwise leave the pin parked as a plain
     * input, silently breaking every NOR command issued afterward). */
    if (HAL_OSPI_Command(s_hospi, &c, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        Error_Handler();
    }

    if (len > 0) {
        if (HAL_OSPI_Receive(s_hospi, data, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
            Error_Handler();
        }
    }
}

/* Same command/address framing as nor_cmd(), but transmits `data` instead
 * of receiving it -- used for WREN (len=0) and Page Program (len>0). */
static void nor_cmd_write(uint8_t instr, uint32_t addr, bool has_addr,
                          const uint8_t *data, size_t len)
{
    OSPI_RegularCmdTypeDef c;
    memset(&c, 0, sizeof(c));

    c.OperationType      = HAL_OSPI_OPTYPE_COMMON_CFG;
    c.FlashId             = 0;
    c.Instruction          = instr;
    c.InstructionSize      = HAL_OSPI_INSTRUCTION_8_BITS;
    c.InstructionMode      = HAL_OSPI_INSTRUCTION_1_LINE;
    c.AddressMode          = has_addr ? HAL_OSPI_ADDRESS_1_LINE : HAL_OSPI_ADDRESS_NONE;
    c.AddressSize          = s_addr32 ? HAL_OSPI_ADDRESS_32_BITS : HAL_OSPI_ADDRESS_24_BITS;
    c.Address              = addr;
    c.AlternateBytesMode   = HAL_OSPI_ALTERNATE_BYTES_NONE;
    c.DummyCycles          = 0;
    c.DataMode             = (len > 0) ? HAL_OSPI_DATA_1_LINE : HAL_OSPI_DATA_NONE;
    c.NbData               = len;
    c.DQSMode              = HAL_OSPI_DQS_DISABLE;
    c.SIOOMode             = HAL_OSPI_SIOO_INST_EVERY_CMD;
    c.InstructionDtrMode   = HAL_OSPI_INSTRUCTION_DTR_DISABLE;

    wdog_refresh();

    if (HAL_OSPI_Command(s_hospi, &c, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        Error_Handler();
    }

    if (len > 0) {
        if (HAL_OSPI_Transmit(s_hospi, (uint8_t *)data, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
            Error_Handler();
        }
    }
}

static void nor_wait_ready(void)
{
    uint8_t sr;
    do {
        nor_cmd(NOR_CMD_RDSR, 0, false, 0, &sr, 1);
    } while (sr & NOR_STATUS_WIP);
}

/* WREN's effect (the Write Enable Latch bit) is not synchronous with the
 * command completing -- gw_flash.c's proven-working driver polls for WEL
 * to actually read back set before issuing the erase/program that
 * depends on it. Skipping this and issuing the next command immediately
 * lets the chip silently ignore it (WEL not yet set), which looks
 * exactly like erase/program doing nothing. */
static void nor_wait_wel(void)
{
    uint8_t sr;
    do {
        nor_cmd(NOR_CMD_RDSR, 0, false, 0, &sr, 1);
    } while (!(sr & NOR_STATUS_WEL));
}

void NorTest_Init(OSPI_HandleTypeDef *hospi)
{
    s_hospi = hospi;
    s_addr32 = false; /* corrected by NorTest_ReadID() once the part is known */
}

bool NorTest_ReadID(uint8_t id[3], const char **name)
{
    nor_cmd(NOR_CMD_RDID, 0, false, 0, id, 3);

    for (unsigned i = 0; i < ARRAY_SIZE(nor_id_table); i++) {
        if (memcmp(id, nor_id_table[i].id, 3) == 0) {
            s_addr32 = nor_id_table[i].addr32;
            *name = nor_id_table[i].name;
            return true;
        }
    }

    *name = NULL;
    return false;
}

bool NorTest_ReadConsistency(uint32_t address, uint8_t len)
{
    uint8_t a[256];
    uint8_t b[256];

    nor_cmd(nor_read_opcode(), address, true, 0, a, len);
    nor_cmd(nor_read_opcode(), address, true, 0, b, len);

    return memcmp(a, b, len) == 0;
}

#define NOR_READ_CHUNK 4096u

void NorTest_Read(uint32_t address, uint8_t *data, size_t len)
{
    while (len > 0) {
        uint32_t n = (len < NOR_READ_CHUNK) ? (uint32_t)len : NOR_READ_CHUNK;

        nor_cmd(nor_read_opcode(), address, true, 0, data, n);

        address += n;
        data += n;
        len -= n;
    }
}

#define NOR_SECTOR_SIZE 4096u
#define NOR_PAGE_SIZE   256u

bool NorTest_Write(uint32_t address, size_t len)
{
    static uint8_t pattern[NOR_PAGE_SIZE];
    static uint8_t readback[NOR_PAGE_SIZE];
    const uint8_t cmd_se = s_addr32 ? NOR_CMD_SE_4B : NOR_CMD_SE_3B;
    const uint8_t cmd_pp = s_addr32 ? NOR_CMD_PP_4B : NOR_CMD_PP_3B;

    for (size_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)(i * 37u + 0xA5u);
    }

    for (uint32_t off = 0; off < len; off += NOR_SECTOR_SIZE) {
        nor_cmd_write(NOR_CMD_WREN, 0, false, NULL, 0);
        nor_wait_wel();
        nor_cmd_write(cmd_se, address + off, true, NULL, 0);
        nor_wait_ready();
        wdog_refresh();
    }

    for (uint32_t off = 0; off < len; off += NOR_PAGE_SIZE) {
        nor_cmd_write(NOR_CMD_WREN, 0, false, NULL, 0);
        nor_wait_wel();
        nor_cmd_write(cmd_pp, address + off, true, pattern, sizeof(pattern));
        nor_wait_ready();
        wdog_refresh();
    }

    for (uint32_t off = 0; off < len; off += sizeof(readback)) {
        nor_cmd(nor_read_opcode(), address + off, true, 0, readback, sizeof(readback));
        if (memcmp(readback, pattern, sizeof(readback)) != 0) {
            return false;
        }
    }

    return true;
}
