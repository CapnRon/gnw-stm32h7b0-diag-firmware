#ifndef _GW_OSPI_BUS_H_
#define _GW_OSPI_BUS_H_

#include "stm32h7xx_hal.h"

/* PSRAM chip-select auto-detection.
 *
 * The PSRAM (ISSI IS66WVS4M8FALL) can be wired with its CE# on one of three
 * pins, depending on which hardware revision it is mounted on:
 *
 *   - PE11: the OSPI peripheral's own hardware NCS (AF11). The peripheral
 *     drives it automatically for every transaction; no GPIO work needed.
 *   - PC11: alternate location of the same hardware NCS (AF9). Same
 *     behaviour, different pin (PE11 is then left as a pulled-up input).
 *   - PE9:  a plain GPIO output (the original piggyback wiring, when the
 *     NOR was still sharing the bus).
 *
 * OspiBus_ProbePsram() tries each candidate in that order, issues a Read-ID
 * (9Fh) and checks for MF=0x9D / KGD=0x5D. The first match is latched and
 * used for every subsequent Select/Deselect bracket. */

typedef enum {
    OSPI_BUS_PSRAM_CS_UNKNOWN = 0,
    OSPI_BUS_PSRAM_CS_PE11,   /* hardware OSPI NCS (AF11) */
    OSPI_BUS_PSRAM_CS_PC11,   /* hardware OSPI NCS alternate (AF9) */
    OSPI_BUS_PSRAM_CS_PE9,    /* plain GPIO output */
} ospi_bus_psram_cs_t;

void OspiBus_Init(OSPI_HandleTypeDef *hospi);

/* Probe all three CS candidates and latch the first one that answers with a
 * valid PSRAM ID. Returns the latched mode (UNKNOWN if nothing matched). */
ospi_bus_psram_cs_t OspiBus_ProbePsram(void);

ospi_bus_psram_cs_t OspiBus_GetPsramCs(void);

/* Bracket every PSRAM command with these two calls. No-ops for the
 * hardware-NCS modes; GPIO toggle only for the PE9 mode. */
void OspiBus_SelectPsram(void);
void OspiBus_DeselectPsram(void);

#endif
