#ifndef _GW_BENCH_H_
#define _GW_BENCH_H_

#include <stdint.h>
#include <stdbool.h>

/* CPU/OSPI clock presets -- see the table above SystemClock_Config()'s
 * definition in main.c for exact PLL values and resulting frequencies. */
#define BENCH_CLK_LEVEL_COUNT 4

/* One measured result row (one device, at one clock level, at one
 * SampleShifting setting). */
typedef struct {
    const char *device_name;    /* "DTCM Heap", "PSRAM", "NOR flash", ... */
    uint8_t     clk_level;      /* 0-3, see BENCH_CLK_LEVEL_COUNT */
    bool        sample_shift;   /* false=NONE, true=HALFCYCLE (OSPI devices only) */
    bool        io_quad;        /* false=SPI (1-1-1), true=Quad (1-4-4) -- PSRAM only */
    bool        applicable;     /* false = this axis doesn't apply to this device
                                  * (e.g. internal SRAM under the sample-shift axis) */
    bool        ran;            /* false if skipped (device not detected) */
    bool        pass;           /* data-integrity check passed at this config */
    bool        writable;       /* false = read-only device (NOR); write_mb_s meaningless */
    float       write_mb_s;
    float       read_mb_s;
    float       latency_ns;     /* single-access/-transaction latency */
} bench_row_t;

/* Runs the full benchmark matrix and leaves the device on the stock clock
 * (level 0, SampleShifting NONE) when done:
 *
 *  - Every internal SRAM region (from RamTest_GetInternalRegions()), at
 *    every CPU clock level. SRAM throughput/latency depends only on core
 *    clock, so the SampleShifting axis is skipped for these rows
 *    (applicable=false).
 *  - PSRAM (if OspiBus_ProbePsram() found it): every clock level crossed
 *    with OSPI SampleShifting -- HALFCYCLE only above stock; NONE is
 *    skipped for every overclocked level (1-3), since it's already
 *    established to reliably fail there (the read-sampling-margin bug
 *    fixed by psram-only firmware commit eceeef5d) and re-proving that
 *    every run just costs sweep time for no new information. Both
 *    SampleShifting settings still run at stock (level 0), where it
 *    doesn't matter either way -- crossed with both IO widths (SPI 1-1-1
 *    vs Quad 1-4-4, opcodes 0x02/0x0B vs 0x38/0xEB -- the quad path
 *    matches the one already proven on real hardware in the psram-only
 *    firmware's gw_flash.c cmds_psram table, commit fdd3cd04).
 *  - Memory-mapped PSRAM (if the detected chip-select is PE11/PC11 --
 *    see PSRAM_EnableMemoryMapped()'s doc comment for why PE9 is
 *    excluded): same clock/SampleShifting matrix, real reads AND writes
 *    straight through the PSRAM_MMAP_BASE pointer. See
 *    bench_psram_mmap()'s doc comment (gw_bench.c) for how a raw store
 *    through that pointer went from a reliable CPU double fault/lockup
 *    to fully working: STM32H72x/73x errata 2.8.6 ("Memory-mapped write
 *    error response when DQS output is disabled") -- fixed by enabling
 *    DQS in the write config even though this PSRAM has no physical DQS
 *    pin and doesn't need one, purely as an internal SoC-side
 *    workaround. Confirmed at every scale from a single word up to the
 *    full 4MB.
 *  - NOR flash (if NorTest_ReadID() gets a non-empty response): same
 *    clock/SampleShifting matrix as PSRAM above. IO width is not swept
 *    for NOR -- gw_nor_test.c intentionally only issues universal
 *    single-line commands so it stays correct across whatever
 *    unidentified NOR part might be installed; enabling quad reads would
 *    need a per-vendor quad-enable status-register write first, which is
 *    out of scope for a read-only presence/health check.
 *
 *  Every PSRAM/NOR pass re-verifies data integrity at that exact config
 *  before trusting its throughput number -- a corrupted-but-fast read
 *  must never look like a good result.
 *
 * ChipSelectBoundary is fixed at 10 (2^10=1024, this PSRAM's page size)
 * for every config, not swept: it's required for memory-mapped
 * correctness (makes the peripheral auto-reissue command+address at
 * every page boundary within one continuous AXI burst, instead of
 * silently reading/writing the PSRAM chip's own page-wrap garbage across
 * one), and it's inert for every indirect-mode command in this file --
 * they're all pre-chunked in software to stay within one PSRAM page
 * (gw_psram_test.c chunk_len()), so the hardware auto-split never has an
 * opportunity to engage there either way. Safe to apply everywhere.
 *
 * Blocking (this is a multi-config sweep, expect several seconds to low
 * tens of seconds depending on what's detected). Prints full per-row
 * detail to logbuf/UART as it runs; use BenchTest_DrawReport() to see a
 * condensed on-screen table afterward. */
void BenchTest_RunAll(void);

/* Draws the current page of results at (x, y), wrapped to width. One page
 * = one clock level (SRAM rows + PSRAM/NOR rows for both SampleShifting
 * settings). Call BenchTest_NextPage()/PrevPage() from button handling. */
void BenchTest_DrawReport(int x, int y, int width);
void BenchTest_NextPage(void);
void BenchTest_PrevPage(void);
int  BenchTest_GetPage(void);   /* 0-based */
int  BenchTest_GetPageCount(void);

#endif
