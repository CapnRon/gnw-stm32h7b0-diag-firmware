#ifndef _GW_PSRAM_TEST_H_
#define _GW_PSRAM_TEST_H_

#include "stm32h7xx_hal.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Driver for an IS66WVS4M8FALL/BLL 32Mbit (4MByte) Serial/QPI PSRAM
 * piggybacked onto the same OSPI CLK/SIO lines as the board's original NOR
 * flash, which stays in its original position on its original CE#. The
 * PSRAM's own CS is wired separately to PE9 (see gw_ospi_bus.h for how the
 * two chip-selects are arbitrated so a PSRAM command can't also select the
 * NOR flash). This is volatile self-refresh PSRAM, not NOR flash: no
 * erase, no status/WIP polling, no write-enable. Talks SPI (1-1-1) only,
 * via indirect (non-memory-mapped) OSPI commands -- memory-mapped XIP is
 * intentionally not used here, since this chip always wraps reads/writes
 * at 1024-byte page boundaries (datasheet 4.2), which would silently
 * corrupt a linear memory-mapped burst that crosses a page. */

#define PSRAM_SIZE_BYTES  (4u * 1024u * 1024u)
#define PSRAM_PAGE_BYTES  1024u

/* Data-bus width for PSRAM_Read()/PSRAM_Write(). PSRAM_IO_SPI (default) is
 * plain 1-1-1 (command/address/data all single-line) -- always correct,
 * lowest bandwidth. PSRAM_IO_QUAD is 1-4-4 (command single-line, address
 * and data quad-line) -- command opcodes 0x38 (write) / 0xEB (read),
 * matching the ones already proven on real hardware in the psram-only
 * firmware (game-and-watch-retro-go-sd, commit fdd3cd04's write path and
 * its gw_flash.c cmds_psram read path). Needs all four SIOx lines wired,
 * which this board's OSPI pinout already provides (see the OCTOSPI1
 * GPIO config in HAL_OSPI_MspInit()) -- this is a firmware-only switch,
 * no rewiring required. Affects PSRAM_Write()/PSRAM_Read() only; RESET
 * and Read-ID always stay 1-1-1 (matches the validated command table). */
typedef enum {
    PSRAM_IO_SPI = 0,
    PSRAM_IO_QUAD,
} psram_io_mode_t;

void PSRAM_Init(OSPI_HandleTypeDef *hospi);

void PSRAM_SetIoMode(psram_io_mode_t mode);
psram_io_mode_t PSRAM_GetIoMode(void);

/* Reads the 8-bit manufacturer ID and Known-Good-Die byte (9Fh command).
 * Returns true if KGD bit0 == 1 (PASS per datasheet Table under 5.7). */
bool PSRAM_ReadID(uint8_t *mfid, uint8_t *kgd);

/* Both handle chunking/splitting so no single OSPI command ever crosses a
 * PSRAM_PAGE_BYTES boundary. Command opcodes/dummy-cycle count depend on
 * the current PSRAM_SetIoMode() setting. */
void PSRAM_Write(uint32_t address, const uint8_t *data, size_t len);
void PSRAM_Read(uint32_t address, uint8_t *data, size_t len);

#define PSRAM_MMAP_BASE 0x90000000u

/* Memory-mapped (XIP) access at PSRAM_MMAP_BASE + offset, matching the
 * config already proven on real hardware in the psram-only firmware
 * (game-and-watch-retro-go-sd's gw_flash.c OSPI_EnableMemoryMappedMode()):
 * quad read (0xEB, 6 dummy) for the read config, quad write (0x38, 0
 * dummy) for the write config, memory-mapped timeout counter disabled.
 * ChipSelectBoundary must already be set to 10 (2^10=1024, this PSRAM's
 * page size) via the OSPI Init that's active when this is called -- that
 * makes the peripheral auto-reissue command+address at every page
 * boundary within one continuous AXI burst, which is required for
 * correctness here: without it, a burst that crosses a page comes back
 * as the PSRAM chip's own silent page-wrap garbage, not an error (this
 * is the same CSBOUND mechanism root-caused in the psram-only firmware's
 * overclock investigation).
 *
 * Only safe when the detected chip-select is PE11 or PC11 (the OSPI
 * peripheral's own hardware NCS) -- returns false and does nothing on
 * the PE9 (plain GPIO) variant, since a hardware-driven memory-mapped
 * burst never calls OspiBus_SelectPsram()/DeselectPsram(), so nothing
 * would ever assert that GPIO.
 *
 * Must call PSRAM_DisableMemoryMapped() before any indirect-mode
 * PSRAM_Read()/PSRAM_Write()/PSRAM_ReadID() call, and before
 * reconfiguring the OSPI peripheral (clock/SampleShifting/CSBoundary
 * changes) -- the peripheral cannot be reinitialized while a
 * memory-mapped session is open. */
bool PSRAM_EnableMemoryMapped(void);
void PSRAM_DisableMemoryMapped(void);
bool PSRAM_IsMemoryMapped(void);

#endif
