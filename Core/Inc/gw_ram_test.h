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
} ram_test_result_t;

/* Runs a full address+checkerboard pattern test over every internal SRAM
 * region this diagnostic build never otherwise touches (DTCM heap, the free
 * tail of AXI RAM_CORE, all of AXI RAM_EMU, and the free tail of AHB
 * SRAM1/2), plus a full read/write/verify pass over the entire external
 * OSPI PSRAM. Results are stashed for RamTest_DrawReport(). Blocking; prints
 * progress to the LCD and to logbuf/UART as it goes. */
void RamTest_RunAll(void);

/* Draws the most recent RamTest_RunAll() results as text, one line per
 * region (two lines if it failed), starting at (x, y), wrapped to width. */
void RamTest_DrawReport(int x, int y, int width);

#endif
