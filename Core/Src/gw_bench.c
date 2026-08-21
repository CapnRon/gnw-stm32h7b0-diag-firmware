#include "gw_bench.h"
#include "gw_ram_test.h"
#include "gw_psram_test.h"
#include "gw_nor_test.h"
#include "gw_ospi_bus.h"
#include "gw_lcd.h"
#include "main.h"
#include "utils.h"
#include "odroid_overlay.h"

#include <string.h>
#include <stdio.h>

#define BENCH_MAX_SRAM_REGIONS 6

static bench_row_t s_sram[BENCH_MAX_SRAM_REGIONS][BENCH_CLK_LEVEL_COUNT];
static int         s_sram_region_count;
static bench_row_t s_psram[BENCH_CLK_LEVEL_COUNT][2][2]; /* [level][sample_shift][io_quad] */
static bench_row_t s_nor[BENCH_CLK_LEVEL_COUNT][2][2]; /* [level][sample_shift][io_quad] */
static bench_row_t s_psram_mmap[BENCH_CLK_LEVEL_COUNT][2]; /* [level][sample_shift], quad-only */
static bool        s_psram_mmap_available; /* CS is PE11/PC11, not PE9 */
static bool        s_psram_present;
static bool        s_nor_present;
static int         s_page;

static const char *s_level_name[BENCH_CLK_LEVEL_COUNT] = {
    "Stock (280/64)",
    "Intermediate (312/104)",
    "Maximum (340/97)",
    "Aggressive (354/101)",
};

/* ---- DWT cycle counter -------------------------------------------------- */

extern uint32_t SystemCoreClock;

static void dwt_enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55u; /* unlock; harmless no-op on parts without a lock register */
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t dwt_now(void)
{
    return DWT->CYCCNT;
}

static float cycles_to_mb_s(uint32_t cycles, uint32_t bytes)
{
    if (cycles == 0) return 0.0f;
    double seconds = (double)cycles / (double)SystemCoreClock;
    return (float)((double)bytes / seconds / (1024.0 * 1024.0));
}

static float cycles_to_ns(uint32_t cycles)
{
    return (float)((double)cycles * 1e9 / (double)SystemCoreClock);
}

/* ---- clock / OSPI reconfiguration --------------------------------------- */

static uint8_t s_cur_level = 0xFFu;
static bool    s_cur_ss;
static bool    s_clock_inited;

static void reinit_ospi(bool sample_shift)
{
    OSPIM_CfgTypeDef sOspiManagerCfg = {0};

    /* Safety net: the peripheral can't be reinitialized while a
     * memory-mapped session is open. bench_psram_mmap() already disables
     * it before returning, but this guards any future caller too. */
    PSRAM_DisableMemoryMapped();

    HAL_OSPI_DeInit(&hospi1);

    hospi1.Instance = OCTOSPI1;
    hospi1.Init.FifoThreshold = 4;
    hospi1.Init.DualQuad = HAL_OSPI_DUALQUAD_DISABLE;
    hospi1.Init.MemoryType = HAL_OSPI_MEMTYPE_MACRONIX;
    hospi1.Init.DeviceSize = 28;
    hospi1.Init.ChipSelectHighTime = 2;
    hospi1.Init.FreeRunningClock = HAL_OSPI_FREERUNCLK_DISABLE;
    hospi1.Init.ClockMode = HAL_OSPI_CLOCK_MODE_0;
    hospi1.Init.WrapSize = HAL_OSPI_WRAP_NOT_SUPPORTED;
    hospi1.Init.ClockPrescaler = 1;
    hospi1.Init.SampleShifting = sample_shift
        ? HAL_OSPI_SAMPLE_SHIFTING_HALFCYCLE : HAL_OSPI_SAMPLE_SHIFTING_NONE;
    hospi1.Init.DelayHoldQuarterCycle = HAL_OSPI_DHQC_DISABLE;
    /* 10 = 2^10 = 1024 bytes = this PSRAM's page size. Required for
     * correctness in memory-mapped mode (bench_psram_mmap()): it makes the
     * peripheral auto-reissue command+address at every page boundary
     * within one continuous AXI burst, matching the PSRAM chip's own
     * page-wrap behavior instead of silently reading/writing wrapped
     * garbage across a boundary -- this is the same CSBOUND mechanism
     * root-caused in the psram-only firmware's overclock investigation.
     * Inert for every indirect-mode command in this file: they're all
     * pre-chunked to stay within one page (gw_psram_test.c chunk_len()),
     * so the hardware auto-split never has an opportunity to engage --
     * safe to apply this value unconditionally. */
    hospi1.Init.ChipSelectBoundary = 10;
    hospi1.Init.ClkChipSelectHighTime = 0;
    hospi1.Init.DelayBlockBypass = HAL_OSPI_DELAY_BLOCK_BYPASSED;
    hospi1.Init.MaxTran = 0;
    hospi1.Init.Refresh = 0;
    if (HAL_OSPI_Init(&hospi1) != HAL_OK) {
        Error_Handler();
    }

    sOspiManagerCfg.ClkPort = 1;
    sOspiManagerCfg.NCSPort = 1;
    sOspiManagerCfg.IOLowPort = HAL_OSPIM_IOPORT_1_LOW;
    if (HAL_OSPIM_Config(&hospi1, &sOspiManagerCfg, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        Error_Handler();
    }
}

static void reconfigure(uint8_t level, bool sample_shift)
{
    if (s_clock_inited && level == s_cur_level && sample_shift == s_cur_ss) {
        return;
    }

    SystemClock_Config(level);
    reinit_ospi(sample_shift);

    s_cur_level = level;
    s_cur_ss = sample_shift;
    s_clock_inited = true;
}

/* ---- progress UI --------------------------------------------------------- */

static void report_progress(const char *label, unsigned pct)
{
    char buf[40];

    printf("BENCH: %s %u%%\n", label, pct);

    odroid_overlay_draw_fill_rect(0, GW_LCD_HEIGHT - 20, GW_LCD_WIDTH, 20, 0x0000);
    snprintf(buf, sizeof(buf), "%s %u%%", label, pct);
    odroid_overlay_draw_text(4, GW_LCD_HEIGHT - 18, GW_LCD_WIDTH - 8, buf, 0xFFFF, 0x0000);
    lcd_sync();
    lcd_swap();
}

/* ---- per-device measurement ---------------------------------------------- */

/* Isolated single-access latency: a data synchronization barrier before and
 * after each access drains the load/store queue, so consecutive reps can't
 * pipeline-overlap the way a streaming throughput loop does. This is a
 * different, and genuinely useful, number from write_mb_s/read_mb_s --
 * it's the cost of one access in isolation, not sustained bandwidth.
 *
 * Also used for memory-mapped PSRAM (bench_psram_mmap()): safe to reuse
 * as-is there now that MPU_Config() (main.c) covers PSRAM_MMAP_BASE with
 * a Strongly Ordered region, so there's no D-Cache to produce a
 * cache-hit-inflated result the way an earlier, PSRAM-specific version of
 * this measurement did before that MPU region existed. */
static float measure_sram_latency_ns(volatile uint32_t *p)
{
    const int reps = 128;
    uint32_t total = 0;

    for (int i = 0; i < reps; i++) {
        __DSB(); __ISB();
        uint32_t t0 = dwt_now();
        p[0] = (uint32_t)i;
        __DSB();
        uint32_t t1 = dwt_now();
        total += (t1 - t0);
    }
    for (int i = 0; i < reps; i++) {
        __DSB(); __ISB();
        uint32_t t0 = dwt_now();
        volatile uint32_t v = p[0];
        __DSB();
        uint32_t t1 = dwt_now();
        (void)v;
        total += (t1 - t0);
    }

    return cycles_to_ns(total / (uint32_t)(2 * reps));
}

static void bench_sram_region(const ram_region_t *reg, bench_row_t *out)
{
    volatile uint32_t *p = (volatile uint32_t *)reg->start;
    uint32_t words = (reg->end - reg->start) / 4u;

    if (words == 0) {
        out->ran = false;
        return;
    }

    dwt_enable();

    uint32_t t0 = dwt_now();
    for (uint32_t i = 0; i < words; i++) {
        p[i] = i;
        if ((i & 0xFFFu) == 0) wdog_refresh();
    }
    uint32_t t1 = dwt_now();

    uint32_t sink = 0;
    for (uint32_t i = 0; i < words; i++) {
        sink += p[i];
        if ((i & 0xFFFu) == 0) wdog_refresh();
    }
    uint32_t t2 = dwt_now();
    (void)sink;

    /* Correctness at this exact clock config -- an SRAM fault that only
     * shows up at a particular core clock is the same class of bug as the
     * OSPI sampling-margin issue, just on the CPU-to-SRAM bus instead. */
    bool pass = true;
    for (uint32_t i = 0; i < words; i++) {
        if (p[i] != i) { pass = false; break; }
        if ((i & 0xFFFu) == 0) wdog_refresh();
    }
    memset((void *)reg->start, 0, (size_t)words * 4u);

    out->write_mb_s = cycles_to_mb_s(t1 - t0, words * 4u);
    out->read_mb_s  = cycles_to_mb_s(t2 - t1, words * 4u);
    out->latency_ns = measure_sram_latency_ns(p);
    out->writable = true;
    out->pass = pass;
}

static bool bench_psram(bench_row_t *out, bool quad)
{
    static uint8_t wbuf[PSRAM_PAGE_BYTES];
    static uint8_t rbuf[PSRAM_PAGE_BYTES];
    uint32_t chunks = PSRAM_SIZE_BYTES / PSRAM_PAGE_BYTES;
    uint32_t write_cycles = 0, read_cycles = 0;
    bool pass = true;

    PSRAM_SetIoMode(quad ? PSRAM_IO_QUAD : PSRAM_IO_SPI);
    dwt_enable();

    for (uint32_t c = 0; c < chunks; c++) {
        uint32_t addr = c * PSRAM_PAGE_BYTES;

        for (uint32_t i = 0; i < PSRAM_PAGE_BYTES; i += 4) {
            uint32_t val = addr + i;
            memcpy(&wbuf[i], &val, 4);
        }

        uint32_t t0 = dwt_now();
        PSRAM_Write(addr, wbuf, PSRAM_PAGE_BYTES);
        uint32_t t1 = dwt_now();
        PSRAM_Read(addr, rbuf, PSRAM_PAGE_BYTES);
        uint32_t t2 = dwt_now();

        write_cycles += (t1 - t0);
        read_cycles  += (t2 - t1);

        if (pass && memcmp(wbuf, rbuf, PSRAM_PAGE_BYTES) != 0) {
            pass = false;
        }

        wdog_refresh();
        if ((c & 0x1FFu) == 0) {
            report_progress(quad ? "PSRAM(quad)" : "PSRAM(spi)", (unsigned)(c * 100u / chunks));
        }
    }

    /* Leave PSRAM zeroed, matching RamTest_RunAll()'s convention. */
    memset(wbuf, 0, sizeof(wbuf));
    for (uint32_t c = 0; c < chunks; c++) {
        PSRAM_Write(c * PSRAM_PAGE_BYTES, wbuf, PSRAM_PAGE_BYTES);
        wdog_refresh();
    }

    out->write_mb_s = cycles_to_mb_s(write_cycles, PSRAM_SIZE_BYTES);
    out->read_mb_s  = cycles_to_mb_s(read_cycles, PSRAM_SIZE_BYTES);
    out->writable = true;
    out->pass = pass;

    /* Single small transaction, repeated -- dominated by SPI command +
     * address + dummy-cycle overhead, not transfer time, so it's a
     * genuinely different number from read_mb_s. */
    {
        const int reps = 64;
        uint8_t tiny[4];
        uint32_t t0 = dwt_now();
        for (int i = 0; i < reps; i++) {
            PSRAM_Read(0, tiny, sizeof(tiny));
        }
        uint32_t t1 = dwt_now();
        out->latency_ns = cycles_to_ns(t1 - t0) / reps;
    }

    return pass;
}

static bool bench_nor(bench_row_t *out, bool quad)
{
    static uint8_t buf[4096];
    /* Bounded to stay well within even the smallest part in gw_nor_test.c's
     * jedec table (MX25U8035F, 8Mbit = 1MB) -- this is a throughput probe,
     * not a capacity test (NorTest_ReadConsistency() already covers
     * presence/stability without assuming a size). */
    const uint32_t total = 256u * 1024u;
    uint32_t read_cycles = 0;
    bool pass;

    dwt_enable();

    {
        uint8_t a[64], b[64];
        NorTest_Read(0, a, sizeof(a), quad);
        NorTest_Read(0, b, sizeof(b), quad);
        pass = (memcmp(a, b, sizeof(a)) == 0);
    }

    for (uint32_t off = 0; off < total; off += sizeof(buf)) {
        uint32_t t0 = dwt_now();
        NorTest_Read(off, buf, sizeof(buf), quad);
        uint32_t t1 = dwt_now();
        read_cycles += (t1 - t0);
        wdog_refresh();
    }

    out->read_mb_s = cycles_to_mb_s(read_cycles, total);
    out->writable = true;

    /* Write throughput: erase+program a small, fixed, out-of-the-way
     * region (see NOR_TEST_WRITE_OFFSET/_SIZE) -- destructive to that
     * region only. Timed cycles cover erase+program together, since
     * that combined cost is the real, honest price of a NOR "write";
     * measuring program time alone would understate it and wouldn't be
     * a fair comparison against PSRAM/SRAM. */
    {
        uint32_t t0 = dwt_now();
        bool write_ok = NorTest_Write(NOR_TEST_WRITE_OFFSET, NOR_TEST_WRITE_SIZE);
        uint32_t t1 = dwt_now();
        out->write_mb_s = cycles_to_mb_s(t1 - t0, NOR_TEST_WRITE_SIZE);
        pass = pass && write_ok;
    }
    out->pass = pass;

    {
        const int reps = 64;
        uint8_t tiny[4];
        uint32_t t0 = dwt_now();
        for (int i = 0; i < reps; i++) {
            NorTest_Read(0, tiny, sizeof(tiny), quad);
        }
        uint32_t t1 = dwt_now();
        out->latency_ns = cycles_to_ns(t1 - t0) / reps;
    }

    return pass;
}

/* Memory-mapped (XIP) PSRAM read+write benchmark. Real reads AND writes,
 * straight through the mapped pointer at PSRAM_MMAP_BASE. Getting here
 * took two wrong turns, both worth keeping on record:
 *
 *  1. First guess: a raw store faulted because no MPU region covered
 *     this window, so it fell under the CPU's default Normal/Cacheable/
 *     Write-Back attributes -- a store would write-allocate a D-Cache
 *     line and the deferred write-back would fault later, at an
 *     unrelated PC. Added an explicit Strongly Ordered MPU region
 *     (MPU_Config(), main.c) -- verified correct by reading the live
 *     MPU_RNR/RBAR/RASR registers over SWD. Made no difference: fault
 *     reproduced identically, so this wasn't the cause (though the
 *     region is still correct/good practice and stays).
 *  2. Bisected with a debugger, `call`ing PSRAM_TestMmapWrite() directly
 *     with shrinking word counts -- down to a single word, still faulted
 *     the same way, and even a raw debug-probe write (bypassing the CPU
 *     entirely, no instruction execution, no caching) faulted too. That
 *     ruled out every CPU-side explanation and pointed at the OCTOSPI
 *     peripheral itself.
 *
 * Real cause: STM32H72x/73x errata 2.8.6, "Memory-mapped write error
 * response when DQS output is disabled." On parts with the OCTOSPI
 * memory-mapped region on the AXI bus, writes are always done
 * internally in 64-bit chunks with DQS used to mask down to the actual
 * access size -- with DQS disabled (this PSRAM has no physical DQS pin,
 * so that seemed like the obviously-correct setting) that masking
 * breaks and the write comes back as an AXI bus error. Fix applied in
 * PSRAM_EnableMemoryMapped()'s write config: DQSMode = ENABLE anyway,
 * purely as an internal SoC-side workaround -- the external chip still
 * neither has nor needs a real DQS signal. Confirmed on real hardware:
 * a single word, one page, and the full 4MB (crossing every page
 * boundary in the chip) all write cleanly with this fix.
 *
 * The Strongly Ordered MPU region also means no D-Cache management calls
 * are needed around this benchmark -- this region is genuinely
 * non-cacheable in hardware, not just conventionally treated that way. */
static bool bench_psram_mmap(bench_row_t *out)
{
    volatile uint32_t *p = (volatile uint32_t *)PSRAM_MMAP_BASE;
    uint32_t words = PSRAM_SIZE_BYTES / 4u;
    bool pass = true;

    if (!PSRAM_EnableMemoryMapped()) {
        out->ran = false;
        return false;
    }

    dwt_enable();

    uint32_t t0 = dwt_now();
    for (uint32_t i = 0; i < words; i++) {
        p[i] = i;
        if ((i & 0xFFFu) == 0) wdog_refresh();
    }
    uint32_t t1 = dwt_now();

    uint32_t sink = 0;
    for (uint32_t i = 0; i < words; i++) {
        sink += p[i];
        if ((i & 0xFFFu) == 0) wdog_refresh();
    }
    uint32_t t2 = dwt_now();
    (void)sink;

    for (uint32_t i = 0; i < words; i++) {
        if (p[i] != i) { pass = false; break; }
        if ((i & 0xFFFu) == 0) wdog_refresh();
    }

    /* Leave PSRAM zeroed, matching every other PSRAM benchmark's
     * convention -- straight through the mapped pointer, same as above. */
    for (uint32_t i = 0; i < words; i++) {
        p[i] = 0;
        if ((i & 0xFFFu) == 0) wdog_refresh();
    }

    out->write_mb_s = cycles_to_mb_s(t1 - t0, PSRAM_SIZE_BYTES);
    out->read_mb_s  = cycles_to_mb_s(t2 - t1, PSRAM_SIZE_BYTES);
    out->writable = true;
    out->pass = pass;
    out->latency_ns = measure_sram_latency_ns(p);

    PSRAM_DisableMemoryMapped();

    return pass;
}

/* ---- sweep orchestration --------------------------------------------------- */

void BenchTest_RunAll(void)
{
    const ram_region_t *regions;
    int region_count = RamTest_GetInternalRegions(&regions);
    if (region_count > BENCH_MAX_SRAM_REGIONS) region_count = BENCH_MAX_SRAM_REGIONS;
    s_sram_region_count = region_count;

    s_psram_present = (OspiBus_GetPsramCs() != OSPI_BUS_PSRAM_CS_UNKNOWN);
    s_psram_mmap_available = s_psram_present && (OspiBus_GetPsramCs() != OSPI_BUS_PSRAM_CS_PE9);

    uint8_t nid[3];
    const char *nname;
    NorTest_ReadID(nid, &nname);
    s_nor_present = !((nid[0] == 0x00 && nid[1] == 0x00 && nid[2] == 0x00) ||
                       (nid[0] == 0xFF && nid[1] == 0xFF && nid[2] == 0xFF));

    printf("BENCH: PSRAM %s, NOR %s\n",
           s_psram_present ? "present" : "not detected",
           s_nor_present ? "present" : "not detected");

    for (uint8_t level = 0; level < BENCH_CLK_LEVEL_COUNT; level++) {
        /* SampleShifting is an OSPI-only knob -- SRAM benchmarking never
         * touches the OSPI bus, so it only needs one pass per clock level. */
        reconfigure(level, false);

        for (int r = 0; r < region_count; r++) {
            bench_row_t *out = &s_sram[r][level];
            memset(out, 0, sizeof(*out));
            out->device_name = regions[r].name;
            out->clk_level = level;
            out->applicable = true;

            report_progress(regions[r].name, (unsigned)(level * 100u / BENCH_CLK_LEVEL_COUNT));
            bench_sram_region(&regions[r], out);
            out->ran = true;

            printf("BENCH: %-14s lvl=%u(%s) W=%.2fMB/s R=%.2fMB/s Lat=%.0fns %s\n",
                   out->device_name, level, s_level_name[level],
                   out->write_mb_s, out->read_mb_s, out->latency_ns,
                   out->pass ? "PASS" : "FAIL");
        }

        for (int ss = 0; ss < 2; ss++) {
            /* SS:NONE reliably fails at every overclocked level (root
             * cause already established: eceeef5d's read-sampling-margin
             * fix). Re-proving that every run just burns sweep time for
             * no new information -- skip it above stock. */
            if (level != 0 && ss == 0) {
                if (s_psram_present) {
                    for (int io = 0; io < 2; io++) {
                        memset(&s_psram[level][ss][io], 0, sizeof(bench_row_t));
                    }
                }
                memset(&s_psram_mmap[level][ss], 0, sizeof(bench_row_t));
                for (int io = 0; io < 2; io++) {
                    memset(&s_nor[level][ss][io], 0, sizeof(bench_row_t));
                }
                continue;
            }

            reconfigure(level, ss != 0);

            if (s_psram_present) {
                for (int io = 0; io < 2; io++) {
                    bench_row_t *out = &s_psram[level][ss][io];
                    memset(out, 0, sizeof(*out));
                    out->device_name = "PSRAM";
                    out->clk_level = level;
                    out->sample_shift = (ss != 0);
                    out->io_quad = (io != 0);
                    out->applicable = true;

                    bench_psram(out, io != 0);
                    out->ran = true;

                    printf("BENCH: PSRAM          lvl=%u(%s) SS=%-9s IO=%-4s W=%.2fMB/s R=%.2fMB/s Lat=%.2fus %s\n",
                           level, s_level_name[level], ss ? "HALFCYCLE" : "NONE",
                           io ? "QUAD" : "SPI",
                           out->write_mb_s, out->read_mb_s, out->latency_ns / 1000.0f,
                           out->pass ? "PASS" : "FAIL");
                }
                /* Leave PSRAM on the always-safe SPI mode between sweep
                 * steps -- NOR's read below shares the OSPI bus/CS
                 * arbitration but not the IO-mode state, so this isn't
                 * strictly required for NOR, just keeps PSRAM itself in a
                 * known state if anything else touches it mid-sweep. */
                PSRAM_SetIoMode(PSRAM_IO_SPI);
            }

            if (s_psram_mmap_available) {
                bench_row_t *out = &s_psram_mmap[level][ss];
                memset(out, 0, sizeof(*out));
                out->device_name = "PSRAM(mm)";
                out->clk_level = level;
                out->sample_shift = (ss != 0);
                out->io_quad = true;
                out->applicable = true;

                bench_psram_mmap(out);
                out->ran = true;

                printf("BENCH: PSRAM(mmap)    lvl=%u(%s) SS=%-9s W=%.2fMB/s R=%.2fMB/s Lat=%.2fus %s\n",
                       level, s_level_name[level], ss ? "HALFCYCLE" : "NONE",
                       out->write_mb_s, out->read_mb_s, out->latency_ns / 1000.0f,
                       out->pass ? "PASS" : "FAIL");
            }

            if (s_nor_present) {
                for (int io = 0; io < 2; io++) {
                    bench_row_t *out = &s_nor[level][ss][io];
                    memset(out, 0, sizeof(*out));
                    out->device_name = "NOR flash";
                    out->clk_level = level;
                    out->sample_shift = (ss != 0);
                    out->io_quad = (io != 0);
                    out->applicable = true;

                    bench_nor(out, io != 0);
                    out->ran = true;

                    printf("BENCH: NOR flash      lvl=%u(%s) SS=%-9s IO=%-4s W=%.2fMB/s R=%.2fMB/s Lat=%.2fus %s\n",
                           level, s_level_name[level], ss ? "HALFCYCLE" : "NONE",
                           io ? "QUAD" : "SPI",
                           out->write_mb_s, out->read_mb_s, out->latency_ns / 1000.0f,
                           out->pass ? "PASS" : "FAIL");
                }
            }
        }
    }

    /* Leave the device on a known-good, previously-validated config. */
    PSRAM_SetIoMode(PSRAM_IO_SPI);
    reconfigure(0, false);

    s_page = 0;
}

/* ---- report / paging ------------------------------------------------------ */

static void fmt_latency(char *buf, size_t n, float ns)
{
    if (ns >= 1000.0f) {
        snprintf(buf, n, "%.2fus", ns / 1000.0f);
    } else {
        snprintf(buf, n, "%.0fns", ns);
    }
}

void BenchTest_DrawReport(int x, int y, int width)
{
    int line_y = y;
    char buf[64];
    char lat[16];

    snprintf(buf, sizeof(buf), "%s", s_level_name[s_page]);
    line_y += odroid_overlay_draw_text(x, line_y, width, buf, 0xFFFF, 0x0000);
    if (s_psram_present || s_nor_present) {
        line_y += odroid_overlay_draw_text(x, line_y, width,
            "SS:N/H=SampleShift IO:S/Q=SPI/Quad", 0x8410, 0x0000);
    }

    for (int r = 0; r < s_sram_region_count; r++) {
        bench_row_t *row = &s_sram[r][s_page];
        uint16_t color = row->ran ? (row->pass ? 0x07E0 : 0xF800) : 0x8410;

        if (!row->ran) {
            snprintf(buf, sizeof(buf), "%-14s --", row->device_name ? row->device_name : "?");
            line_y += odroid_overlay_draw_text(x, line_y, width, buf, color, 0x0000);
            continue;
        }

        fmt_latency(lat, sizeof(lat), row->latency_ns);
        snprintf(buf, sizeof(buf), "%-14s %s", row->device_name, row->pass ? "PASS" : "FAIL");
        line_y += odroid_overlay_draw_text(x, line_y, width, buf, color, 0x0000);
        snprintf(buf, sizeof(buf), "  W:%.1f R:%.1f MB/s Lat:%s", row->write_mb_s, row->read_mb_s, lat);
        line_y += odroid_overlay_draw_text(x, line_y, width, buf, 0xFFFF, 0x0000);
    }

    for (int ss = 0; ss < 2; ss++) {
        if (!s_psram_present) break;
        for (int io = 0; io < 2; io++) {
            bench_row_t *row = &s_psram[s_page][ss][io];
            uint16_t color = row->ran ? (row->pass ? 0x07E0 : 0xF800) : 0x8410;

            fmt_latency(lat, sizeof(lat), row->latency_ns);
            snprintf(buf, sizeof(buf), "PSRAM SS:%c IO:%c %-4s W:%.1f R:%.1f %s",
                     ss ? 'H' : 'N', io ? 'Q' : 'S',
                     row->ran ? (row->pass ? "PASS" : "FAIL") : "--",
                     row->write_mb_s, row->read_mb_s, lat);
            line_y += odroid_overlay_draw_text(x, line_y, width, buf, color, 0x0000);
        }
    }

    for (int ss = 0; ss < 2; ss++) {
        if (!s_psram_mmap_available) break;
        bench_row_t *row = &s_psram_mmap[s_page][ss];
        uint16_t color = row->ran ? (row->pass ? 0x07E0 : 0xF800) : 0x8410;

        fmt_latency(lat, sizeof(lat), row->latency_ns);
        snprintf(buf, sizeof(buf), "PSRAM(mm) SS:%c %-4s W:%.1f R:%.1f %s",
                 ss ? 'H' : 'N',
                 row->ran ? (row->pass ? "PASS" : "FAIL") : "--",
                 row->write_mb_s, row->read_mb_s, lat);
        line_y += odroid_overlay_draw_text(x, line_y, width, buf, color, 0x0000);
    }

    for (int ss = 0; ss < 2; ss++) {
        if (!s_nor_present) break;
        for (int io = 0; io < 2; io++) {
            bench_row_t *row = &s_nor[s_page][ss][io];
            uint16_t color = row->ran ? (row->pass ? 0x07E0 : 0xF800) : 0x8410;

            fmt_latency(lat, sizeof(lat), row->latency_ns);
            snprintf(buf, sizeof(buf), "NOR SS:%c IO:%c %-4s",
                     ss ? 'H' : 'N', io ? 'Q' : 'S',
                     row->ran ? (row->pass ? "PASS" : "FAIL") : "--");
            line_y += odroid_overlay_draw_text(x, line_y, width, buf, color, 0x0000);
            snprintf(buf, sizeof(buf), "  W:%.1f R:%.1f MB/s Lat:%s",
                     row->write_mb_s, row->read_mb_s, lat);
            line_y += odroid_overlay_draw_text(x, line_y, width, buf, 0xFFFF, 0x0000);
        }
    }

    snprintf(buf, sizeof(buf), "<LEFT/RIGHT> page %d/%d", s_page + 1, BENCH_CLK_LEVEL_COUNT);
    odroid_overlay_draw_text(x, line_y, width, buf, 0xFFE0, 0x0000);
}

void BenchTest_NextPage(void)
{
    if (s_page < BENCH_CLK_LEVEL_COUNT - 1) s_page++;
}

void BenchTest_PrevPage(void)
{
    if (s_page > 0) s_page--;
}

int BenchTest_GetPage(void)
{
    return s_page;
}

int BenchTest_GetPageCount(void)
{
    return BENCH_CLK_LEVEL_COUNT;
}
