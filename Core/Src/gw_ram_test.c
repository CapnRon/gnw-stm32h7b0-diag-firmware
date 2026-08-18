#include "gw_ram_test.h"
#include "gw_psram_test.h"
#include "gw_nor_test.h"
#include "gw_linker.h"
#include "gw_lcd.h"
#include "main.h"
#include "utils.h"
#include "odroid_overlay.h"

#include <string.h>
#include <stdio.h>

/* Free tail after the diag build's fixed AHBRAM occupants (.audio, .ahb --
 * unused lookup tables pulled in by the always-linked emulator cores). */
extern uint32_t __ahbram_end__;
extern uint32_t __RAM_EMU_END__;
extern uint32_t __ram_end__;

#define AHBRAM_END_ADDR (0x30000000u + 128u * 1024u)

#define RAM_TEST_MAX_REGIONS 8

static char s_nor_detail_buf[48];
static char s_nor_id_buf[24];

static ram_test_result_t s_results[RAM_TEST_MAX_REGIONS];
static int s_result_count;

static void report_progress(const char *label, unsigned pct)
{
    char buf[40];

    printf("RAM test: %s %u%%\n", label, pct);

    odroid_overlay_draw_fill_rect(0, GW_LCD_HEIGHT - 40, GW_LCD_WIDTH, 20, 0x0000);
    snprintf(buf, sizeof(buf), "%s %u%%", label, pct);
    odroid_overlay_draw_text(4, GW_LCD_HEIGHT - 38, GW_LCD_WIDTH - 8, buf, 0xFFFF, 0x0000);
    lcd_sync();
    lcd_swap();
}

/* Address-in-address pass (catches address-line/decode faults) followed by
 * an alternating 0x55555555/0xAAAAAAAA checkerboard pass (catches data-line
 * stuck-at and adjacent-bit coupling faults). Leaves the region zeroed. */
static bool test_word_region(uint32_t start, uint32_t size, ram_test_result_t *r)
{
    volatile uint32_t *p = (volatile uint32_t *)start;
    uint32_t words = size / 4;

    for (uint32_t i = 0; i < words; i++) {
        p[i] = start + i * 4;
        if ((i & 0xFFFu) == 0) wdog_refresh();
    }
    for (uint32_t i = 0; i < words; i++) {
        uint32_t expect = start + i * 4;
        uint32_t got = p[i];
        if (got != expect) {
            r->first_fail_addr = start + i * 4;
            r->first_fail_expected = expect;
            r->first_fail_actual = got;
            return false;
        }
        if ((i & 0xFFFu) == 0) wdog_refresh();
    }

    for (uint32_t i = 0; i < words; i++) {
        p[i] = (i & 1u) ? 0xAAAAAAAAu : 0x55555555u;
        if ((i & 0xFFFu) == 0) wdog_refresh();
    }
    for (uint32_t i = 0; i < words; i++) {
        uint32_t expect = (i & 1u) ? 0xAAAAAAAAu : 0x55555555u;
        uint32_t got = p[i];
        if (got != expect) {
            r->first_fail_addr = start + i * 4;
            r->first_fail_expected = expect;
            r->first_fail_actual = got;
            return false;
        }
        if ((i & 0xFFFu) == 0) wdog_refresh();
    }

    memset((void *)start, 0, size);
    return true;
}

static bool test_psram(ram_test_result_t *r)
{
    static uint8_t wbuf[PSRAM_PAGE_BYTES];
    static uint8_t rbuf[PSRAM_PAGE_BYTES];
    uint32_t chunks = PSRAM_SIZE_BYTES / PSRAM_PAGE_BYTES;

    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t c = 0; c < chunks; c++) {
            uint32_t addr = c * PSRAM_PAGE_BYTES;

            for (uint32_t i = 0; i < PSRAM_PAGE_BYTES; i += 4) {
                uint32_t val = (pass == 0)
                    ? (addr + i)
                    : (((( addr + i) / 4) & 1u) ? 0xAAAAAAAAu : 0x55555555u);
                memcpy(&wbuf[i], &val, 4);
            }

            PSRAM_Write(addr, wbuf, PSRAM_PAGE_BYTES);
            PSRAM_Read(addr, rbuf, PSRAM_PAGE_BYTES);

            if (memcmp(wbuf, rbuf, PSRAM_PAGE_BYTES) != 0) {
                for (uint32_t i = 0; i < PSRAM_PAGE_BYTES; i++) {
                    if (wbuf[i] != rbuf[i]) {
                        r->first_fail_addr = addr + i;
                        r->first_fail_expected = wbuf[i];
                        r->first_fail_actual = rbuf[i];
                        return false;
                    }
                }
            }

            wdog_refresh();
            if ((c & 0x1FFu) == 0) {
                report_progress(pass == 0 ? "PSRAM pass 1/2" : "PSRAM pass 2/2",
                                 (unsigned)(c * 100u / chunks));
            }
        }
    }

    memset(wbuf, 0, sizeof(wbuf));
    for (uint32_t c = 0; c < chunks; c++) {
        PSRAM_Write(c * PSRAM_PAGE_BYTES, wbuf, PSRAM_PAGE_BYTES);
        wdog_refresh();
    }

    return true;
}

void RamTest_RunAll(void)
{
    s_result_count = 0;

    struct { const char *name; uint32_t start; uint32_t end; } internal[] = {
        { "DTCM Heap",    (uint32_t)&_heap_start,     (uint32_t)&_heap_end },
        { "AXI RAM_CORE", (uint32_t)&__ram_end__,     (uint32_t)__RAM_EMU_START__ },
        { "AXI RAM_EMU",  (uint32_t)__RAM_EMU_START__,(uint32_t)&__RAM_EMU_END__ },
        { "AHB SRAM1/2",  (uint32_t)&__ahbram_end__,  AHBRAM_END_ADDR },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(internal); i++) {
        ram_test_result_t *r = &s_results[s_result_count++];
        memset(r, 0, sizeof(*r));
        r->name = internal[i].name;
        r->start = internal[i].start;
        r->size = internal[i].end - internal[i].start;

        printf("RAM test: %s @0x%08lx size=%lu bytes\n",
               r->name, (unsigned long)r->start, (unsigned long)r->size);
        report_progress(r->name, 0);

        r->pass = test_word_region(r->start, r->size & ~3u, r);

        printf("  -> %s\n", r->pass ? "PASS" : "FAIL");
    }

    {
        ram_test_result_t *r = &s_results[s_result_count++];
        memset(r, 0, sizeof(*r));
        r->name = "OSPI PSRAM (IS66WVS4M8, CS=PE9)";
        r->start = 0;
        r->size = PSRAM_SIZE_BYTES;

        uint8_t mfid, kgd;
        bool kgd_ok = PSRAM_ReadID(&mfid, &kgd);
        printf("PSRAM ID: MF=0x%02x KGD=0x%02x (%s)\n", mfid, kgd, kgd_ok ? "PASS" : "FAIL");

        if (!kgd_ok) {
            r->pass = false;
            r->first_fail_addr = 0;
            r->first_fail_expected = 1; /* KGD bit0 expected set */
            r->first_fail_actual = kgd;
        } else {
            r->pass = test_psram(r);
        }

        printf("  -> %s\n", r->pass ? "PASS" : "FAIL");
    }

    {
        /* Piggybacked NOR flash, original CE# (PE11 / hardware NCS,
         * unchanged from stock). Deliberately read-only -- see
         * gw_nor_test.h. size stays 0 so this entry isn't counted into the
         * "Total tested" byte count below: presence + a small read-
         * consistency check verify the chip and bus wiring, not its full
         * capacity. */
        ram_test_result_t *r = &s_results[s_result_count++];
        memset(r, 0, sizeof(*r));
        r->name = "NOR flash (piggybacked, CS=PE11)";
        r->start = 0;
        r->size = 0;

        uint8_t id[3];
        const char *part_name;
        bool known = NorTest_ReadID(id, &part_name);
        bool present = !((id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00) ||
                          (id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF));

        printf("NOR ID: %02x %02x %02x (%s)\n", id[0], id[1], id[2],
               known ? part_name : (present ? "unknown part" : "no response"));

        if (!present) {
            r->pass = false;
            snprintf(s_nor_detail_buf, sizeof(s_nor_detail_buf), "no response (00/FF)");
        } else {
            bool consistent = NorTest_ReadConsistency(0, 64);
            r->pass = consistent;

            if (known) {
                snprintf(s_nor_id_buf, sizeof(s_nor_id_buf), "%s", part_name);
            } else {
                snprintf(s_nor_id_buf, sizeof(s_nor_id_buf), "ID %02X %02X %02X (unknown)",
                         id[0], id[1], id[2]);
            }
            snprintf(s_nor_detail_buf, sizeof(s_nor_detail_buf), "%s%s",
                     s_nor_id_buf, consistent ? "" : " - read unstable");
        }
        r->detail = s_nor_detail_buf;

        printf("  -> %s\n", r->pass ? "PASS" : "FAIL");
    }
}

void RamTest_DrawReport(int x, int y, int width)
{
    int line_y = y;
    uint32_t total_bytes = 0;
    char buf[64];

    for (int i = 0; i < s_result_count; i++) {
        ram_test_result_t *r = &s_results[i];
        uint16_t color = r->pass ? 0x07E0 : 0xF800;

        if (r->size > 0) {
            total_bytes += r->size;
            snprintf(buf, sizeof(buf), "%-22s %6lu KB %s",
                     r->name, (unsigned long)(r->size / 1024u), r->pass ? "PASS" : "FAIL");
        } else {
            snprintf(buf, sizeof(buf), "%-22s %s",
                     r->name, r->pass ? "PASS" : "FAIL");
        }
        line_y += odroid_overlay_draw_text(x, line_y, width, buf, color, 0x0000);

        if (r->detail) {
            snprintf(buf, sizeof(buf), "  %s", r->detail);
            line_y += odroid_overlay_draw_text(x, line_y, width, buf, 0xFFE0, 0x0000);
        } else if (!r->pass) {
            snprintf(buf, sizeof(buf), "  @0x%08lx exp=0x%08lx got=0x%08lx",
                     (unsigned long)r->first_fail_addr,
                     (unsigned long)r->first_fail_expected,
                     (unsigned long)r->first_fail_actual);
            line_y += odroid_overlay_draw_text(x, line_y, width, buf, 0xFFE0, 0x0000);
        }
    }

    snprintf(buf, sizeof(buf), "Total tested: %lu KB", (unsigned long)(total_bytes / 1024u));
    line_y += odroid_overlay_draw_text(x, line_y, width, buf, 0xFFFF, 0x0000);
}
