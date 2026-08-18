#include "gw_ospi_bus.h"

#define PSRAM_CS_PORT GPIOE
#define PSRAM_CS_PIN  GPIO_PIN_9

static OSPI_HandleTypeDef *s_hospi;

void OspiBus_Init(OSPI_HandleTypeDef *hospi)
{
    s_hospi = hospi;

    /* PSRAM CS: plain push-pull GPIO, idle HIGH (deselected). NOR's CE#
     * (PE11) is left exactly as HAL_OSPI_MspInit() configured it -- AF11,
     * under the OSPI peripheral's automatic control -- since that's the
     * correct state for NOR access and the default state at boot. */
    __HAL_RCC_GPIOE_CLK_ENABLE();

    HAL_GPIO_WritePin(PSRAM_CS_PORT, PSRAM_CS_PIN, GPIO_PIN_SET);

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = PSRAM_CS_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(PSRAM_CS_PORT, &gpio);
}

void OspiBus_SelectPsram(void)
{
    /* Detach PE11 from the OSPI peripheral so its physical net floats and
     * is pulled HIGH externally -- NOR stays deselected regardless of what
     * the OSPI peripheral's internal NCS state machine does during the
     * PSRAM command that follows. */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_11;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL; /* external pull-up does the work */
    HAL_GPIO_Init(GPIOE, &gpio);

    HAL_GPIO_WritePin(PSRAM_CS_PORT, PSRAM_CS_PIN, GPIO_PIN_RESET);
}

void OspiBus_DeselectPsram(void)
{
    HAL_GPIO_WritePin(PSRAM_CS_PORT, PSRAM_CS_PIN, GPIO_PIN_SET);

    /* Reattach PE11 to the OSPI peripheral (AF11 / OCTOSPIM_P1_NCS,
     * matching HAL_OSPI_MspInit()) so it resumes automatic NCS control for
     * NOR transactions. */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_11;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF11_OCTOSPIM_P1;
    HAL_GPIO_Init(GPIOE, &gpio);
}
