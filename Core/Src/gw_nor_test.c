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
#define NOR_CMD_READ  0x03u

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

    /* No CS arbitration needed here: PE11 (hardware NCS) is NOR's original,
     * unchanged CE#, and is only ever detached from the OSPI peripheral
     * transiently inside a PSRAM command (see gw_ospi_bus.c) -- by the time
     * control returns here it's already back under the peripheral's
     * automatic control. */
    if (HAL_OSPI_Command(s_hospi, &c, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        Error_Handler();
    }

    if (len > 0) {
        if (HAL_OSPI_Receive(s_hospi, data, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
            Error_Handler();
        }
    }
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

    nor_cmd(NOR_CMD_READ, address, true, 0, a, len);
    nor_cmd(NOR_CMD_READ, address, true, 0, b, len);

    return memcmp(a, b, len) == 0;
}
