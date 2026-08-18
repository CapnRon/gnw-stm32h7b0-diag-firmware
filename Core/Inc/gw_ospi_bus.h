#ifndef _GW_OSPI_BUS_H_
#define _GW_OSPI_BUS_H_

#include "stm32h7xx_hal.h"

/* CS arbitration for two chips sharing one OSPI bus (CLK + 4 SIO lines) but
 * with two separate, non-bridged chip-selects:
 *
 *   - NOR flash: original position, original CE#, wired to the OSPI
 *     peripheral's own hardware NCS pin (PE11 / OCTOSPIM_P1_NCS, external
 *     pull-up). HAL_OSPI_MspInit() already configures PE11 as AF11 so the
 *     peripheral drives it automatically for every transaction.
 *   - PSRAM: piggybacked onto the same CLK/SIO lines, CS wired to PE9
 *     (plain GPIO, not connected to the OSPI peripheral at all).
 *
 * The OSPI peripheral asserts its hardware NCS pin (PE11) for the full
 * duration of *every* command it issues, with no per-call way to suppress
 * it. Left alone, a PSRAM-targeted command (address on PE9) would also
 * select the NOR flash via PE11 at the same time -- since several PSRAM
 * opcodes (0x02 write, 0x0B read) are also valid NOR opcodes, that's not
 * just contention on the shared SIO lines, it's a real risk of writing to
 * or reading back the wrong chip.
 *
 * Fix: for the duration of a PSRAM transaction, detach PE11 from the OSPI
 * peripheral (switch it to a plain GPIO input) so the physical net floats
 * and is held HIGH by its external pull-up -- deselecting NOR regardless
 * of what the peripheral's internal NCS state machine is doing -- then
 * drive PE9 low. Restore PE11 to AF11/OCTOSPIM_P1_NCS afterward so the
 * peripheral resumes automatic NCS control for NOR transactions. */

void OspiBus_Init(OSPI_HandleTypeDef *hospi);

/* Bracket every PSRAM command with these two calls. Safe to nest by
 * accident (not reentrant/nestable on purpose, but each call pair is a
 * complete detach/reattach cycle). */
void OspiBus_SelectPsram(void);
void OspiBus_DeselectPsram(void);

#endif
