#ifndef _GW_RAM_TEST_H_
#define _GW_RAM_TEST_H_

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *name;
    uint32_t    start;
    uint32_t    size;
    bool        pass;
    uint32_t    first_fail_addr;
    uint32_t    first_fail_expected;
    uint32_t    first_fail_actual;
    /* Extra free-text line shown under this result, or NULL for none.
     * Used by the NOR flash entry (matched part name / raw JEDEC bytes)
     * since that check is a presence+consistency check, not a byte-range
     * pattern test with a meaningful address/expected/actual to show. */
    const char *detail;
} ram_test_result_t;

typedef struct {
    const char *name;
    uint32_t    start;
    uint32_t    end;
} ram_region_t;

/* Returns the internal-SRAM region table (same regions RamTest_RunAll()
 * verifies: DTCM heap, free tail of AXI RAM_CORE, all of AXI RAM_EMU, free
 * tail of AHB SRAM1/2) via *out, and its length. Shared with gw_bench.c so
 * the benchmark exercises exactly the same boundaries the correctness test
 * does, from one source of truth. */
int RamTest_GetInternalRegions(const ram_region_t **out);

/* Runs a full address+checkerboard pattern test over every internal SRAM
 * region this diagnostic build never otherwise touches (DTCM heap, the free
 * tail of AXI RAM_CORE, all of AXI RAM_EMU, and the free tail of AHB
 * SRAM1/2), plus a full read/write/verify pass over the entire external
 * OSPI PSRAM (CS on PE9), plus a non-destructive presence + read-function
 * check of the original NOR flash (CS on PE11/hardware NCS, unchanged --
 * see gw_nor_test.h for why this one is read-only). Results are stashed
 * for RamTest_DrawReport(). Blocking; prints progress to the LCD and to
 * logbuf/UART as it goes. */
void RamTest_RunAll(void);

/* Draws the most recent RamTest_RunAll() results as text, one line per
 * region (two lines if it failed), starting at (x, y), wrapped to width. */
void RamTest_DrawReport(int x, int y, int width);

#endif
