#ifndef _GW_NOR_TEST_H_
#define _GW_NOR_TEST_H_

#include "stm32h7xx_hal.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Presence + read-function test for the original NOR flash chip, now
 * piggybacked on the same OSPI bus as the PSRAM (see gw_ospi_bus.h) but
 * kept on its original CE# (PE11 / hardware NCS, unchanged from stock).
 * The write benchmark (NorTest_Write, below) IS destructive, unlike
 * everything else in this file -- it is restricted to a small, fixed,
 * out-of-the-way region (NOR_TEST_WRITE_OFFSET/_SIZE) specifically so it
 * never touches data elsewhere on the chip. Everything else here is
 * read-only and safe on any NOR content. */

void NorTest_Init(OSPI_HandleTypeDef *hospi);

/* Region used by the write-throughput benchmark. Chosen to sit well
 * within even the smallest part in nor_id_table (1MB), and away from
 * address 0 in case anything ever cares about that region specifically. */
#define NOR_TEST_WRITE_OFFSET 0x00080000u
#define NOR_TEST_WRITE_SIZE   0x00010000u /* 64KB, 16 sectors */

/* Erases (4KB sectors) then programs (256B pages) `len` bytes at
 * `address` with a fixed test pattern, then reads it back and verifies.
 * DESTRUCTIVE -- overwrites whatever was stored there. `len` must be a
 * multiple of 4096. Returns true if erase+program+verify all succeeded.
 * Timed cycles cover erase+program together, since on real NOR there is
 * no way to write without erasing first -- that combined cost is the
 * honest "write speed" to compare against PSRAM/SRAM. */
bool NorTest_Write(uint32_t address, size_t len);

/* Reads the 3-byte JEDEC ID (9Fh, standard on essentially all SPI NOR
 * parts regardless of address width or quad-mode state). Looks it up
 * against a table of known parts (mirrors gw_flash.c's jedec_map) to
 * report a name and its real address width, and to know whether to use
 * 24-bit or 32-bit addressing for the read-consistency check below.
 * Returns true (and fills *name) if the ID matches a known part; false
 * (with *name set to NULL) if it doesn't -- an unrecognized-but-non-empty
 * ID still counts as "chip present", just unidentified. */
bool NorTest_ReadID(uint8_t id[3], const char **name);

/* Reads the same `len` bytes (<=256) starting at `address` twice and
 * compares them. This only proves the chip responds and the read path is
 * stable -- it is not a data-integrity test, since we don't have a known
 * expected value to compare against without writing (which we deliberately
 * don't do here). Returns true if both reads matched. */
bool NorTest_ReadConsistency(uint32_t address, uint8_t len);

/* Sequential read of `len` bytes starting at `address` into `data`, chunked
 * internally (unlike PSRAM this chip has no page-wrap restriction on plain
 * SPI reads, but chunking keeps each HAL_OSPI transaction and its timeout
 * bounded). Read-only, safe on any NOR content -- used by the benchmark for
 * throughput/latency measurement, not correctness (see NorTest_ReadConsistency
 * for that). */
void NorTest_Read(uint32_t address, uint8_t *data, size_t len);

#endif
