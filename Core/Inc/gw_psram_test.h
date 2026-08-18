#ifndef _GW_PSRAM_TEST_H_
#define _GW_PSRAM_TEST_H_

#include "stm32h7xx_hal.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Driver for the IS66WVS4M8FALL/BLL 32Mbit (4MByte) Serial/QPI PSRAM now
 * populated in the external-flash OSPI slot. This is volatile self-refresh
 * PSRAM, not NOR flash: no erase, no status/WIP polling, no write-enable.
 * Talks SPI (1-1-1) only, via indirect (non-memory-mapped) OSPI commands --
 * memory-mapped XIP is intentionally not used here, since this chip always
 * wraps reads/writes at 1024-byte page boundaries (datasheet 4.2), which
 * would silently corrupt a linear memory-mapped burst that crosses a page. */

#define PSRAM_SIZE_BYTES  (4u * 1024u * 1024u)
#define PSRAM_PAGE_BYTES  1024u

void PSRAM_Init(OSPI_HandleTypeDef *hospi);

/* Reads the 8-bit manufacturer ID and Known-Good-Die byte (9Fh command).
 * Returns true if KGD bit0 == 1 (PASS per datasheet Table under 5.7). */
bool PSRAM_ReadID(uint8_t *mfid, uint8_t *kgd);

/* Both handle chunking/splitting so no single OSPI command ever crosses a
 * PSRAM_PAGE_BYTES boundary. */
void PSRAM_Write(uint32_t address, const uint8_t *data, size_t len);
void PSRAM_Read(uint32_t address, uint8_t *data, size_t len);

#endif
