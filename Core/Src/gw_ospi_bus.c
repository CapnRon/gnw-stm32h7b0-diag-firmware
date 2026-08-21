#include "gw_ospi_bus.h"
#include "main.h"

#include <stdbool.h>
#include <string.h>

#define PSRAM_CMD_READ_ID 0x9Fu

#define PSRAM_MFID 0x9Du
#define PSRAM_KGD  0x5Du

static OSPI_HandleTypeDef *s_hospi;
static ospi_bus_psram_cs_t s_cs_mode = OSPI_BUS_PSRAM_CS_UNKNOWN;

/* ------------------------------------------------------------------ */
/* Pin configuration helpers                                          */
/* ------------------------------------------------------------------ */

static void cs_pe11_af(void)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_11;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF11_OCTOSPIM_P1;
    HAL_GPIO_Init(GPIOE, &gpio);
}

static void cs_pe11_input_pullup(void)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_11;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOE, &gpio);
}

static void cs_pc11_af(void)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_11;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF9_OCTOSPIM_P1;
    HAL_GPIO_Init(GPIOC, &gpio);
}

static void cs_pc11_input_pullup(void)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_11;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &gpio);
}

static void cs_pe9_output(bool high)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOE, &gpio);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_9, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void cs_pe9_input_pullup(void)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOE, &gpio);
}

/* ------------------------------------------------------------------ */
/* Low-level Read-ID                                                  */
/* ------------------------------------------------------------------ */

/* Issue a Read-ID (9Fh, 1-1-1, 24-bit don't-care address, 0 wait cycles)
 * and capture MF + KGD. The caller must already have the pins in the
 * correct state for the CS candidate under test. */
static bool probe_read_id(uint8_t id[2])
{
    OSPI_RegularCmdTypeDef c;
    memset(&c, 0, sizeof(c));

    c.OperationType    = HAL_OSPI_OPTYPE_COMMON_CFG;
    c.FlashId           = 0;
    c.Instruction        = PSRAM_CMD_READ_ID;
    c.InstructionSize    = HAL_OSPI_INSTRUCTION_8_BITS;
    c.InstructionMode    = HAL_OSPI_INSTRUCTION_1_LINE;
    c.AddressMode        = HAL_OSPI_ADDRESS_1_LINE;
    c.AddressSize        = HAL_OSPI_ADDRESS_24_BITS;
    c.Address            = 0;
    c.AlternateBytesMode = HAL_OSPI_ALTERNATE_BYTES_NONE;
    c.DummyCycles        = 0;
    c.DataMode           = HAL_OSPI_DATA_1_LINE;
    c.NbData             = 2;
    c.DQSMode            = HAL_OSPI_DQS_DISABLE;
    c.SIOOMode           = HAL_OSPI_SIOO_INST_EVERY_CMD;
    c.InstructionDtrMode = HAL_OSPI_INSTRUCTION_DTR_DISABLE;

    wdog_refresh();

    if (HAL_OSPI_Command(s_hospi, &c, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        return false;
    }
    if (HAL_OSPI_Receive(s_hospi, id, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        return false;
    }

    return (id[0] == PSRAM_MFID) && (id[1] == PSRAM_KGD);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void OspiBus_Init(OSPI_HandleTypeDef *hospi)
{
    s_hospi = hospi;

    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
}

ospi_bus_psram_cs_t OspiBus_ProbePsram(void)
{
    uint8_t id[2];

    /* Candidate 1: PE11, hardware OSPI NCS (AF11). The default
     * HAL_OSPI_MspInit() state; the peripheral drives CS by itself. */
    cs_pe11_af();
    cs_pc11_input_pullup();
    cs_pe9_input_pullup();
    if (probe_read_id(id)) {
        s_cs_mode = OSPI_BUS_PSRAM_CS_PE11;
        return s_cs_mode;
    }

    /* Candidate 2: PC11, alternate hardware NCS location (AF9). PE11 is
     * parked as a pulled-up input so only PC11 carries the NCS signal. */
    cs_pe11_input_pullup();
    cs_pc11_af();
    if (probe_read_id(id)) {
        s_cs_mode = OSPI_BUS_PSRAM_CS_PC11;
        return s_cs_mode;
    }

    /* Candidate 3: PE9, plain GPIO CS (original piggyback wiring). */
    cs_pe11_input_pullup();
    cs_pc11_input_pullup();
    cs_pe9_output(true);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_9, GPIO_PIN_RESET);
    bool ok = probe_read_id(id);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_9, GPIO_PIN_SET);
    if (ok) {
        s_cs_mode = OSPI_BUS_PSRAM_CS_PE9;
        return s_cs_mode;
    }

    /* No PSRAM found on any candidate. Restore PE11 to the OSPI
     * peripheral's hardware NCS function (its default, stock state from
     * HAL_OSPI_MspInit()) before returning -- candidate 2/3 above parked
     * it as a plain input, and leaving it that way silently breaks every
     * other bus user that expects automatic hardware CS on PE11 (the
     * stock NOR flash, in gw_nor_test.c). PC11/PE9 are left parked; only
     * PE11 is a shared default other code relies on. */
    cs_pe11_af();
    cs_pe9_input_pullup();

    s_cs_mode = OSPI_BUS_PSRAM_CS_UNKNOWN;
    return s_cs_mode;
}

ospi_bus_psram_cs_t OspiBus_GetPsramCs(void)
{
    return s_cs_mode;
}

void OspiBus_SelectPsram(void)
{
    if (s_cs_mode == OSPI_BUS_PSRAM_CS_PE9) {
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_9, GPIO_PIN_RESET);
    }
    /* PE11 / PC11: the OSPI peripheral asserts NCS by itself. */
}

void OspiBus_DeselectPsram(void)
{
    if (s_cs_mode == OSPI_BUS_PSRAM_CS_PE9) {
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_9, GPIO_PIN_SET);
    }
}
