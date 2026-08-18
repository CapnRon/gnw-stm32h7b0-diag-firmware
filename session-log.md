# Session log

Terse working notes for this repo, in normal technical style (not
Simplified Technical English -- that's reserved for upstream-facing PR
text only).

## 2026-08-18/19 -- PSRAM swap verification, then NOR+PSRAM piggyback

### Background

The board's external OSPI flash footprint was reworked to hold an ISSI
IS66WVS4M8FALL/BLL 32Mbit (4MB) Serial/QPI PSRAM instead of NOR flash, to
test whether a stronger/faster external memory chip helps with more
demanding emulation cores. Confirmed pin-compatible with the stock NOR
footprint (identical 8-pin SPI/QPI pinout: CE#, SO/SIO1, SIO2, VSS, SI/SIO0,
CLK, SIO3, VDD).

### RAM test firmware (branch `ram-test`)

Added a diagnostic build that tests all internal SRAM regions this build
doesn't otherwise use (DTCM heap, AXI RAM_CORE free tail, AXI RAM_EMU, AHB
SRAM1/2) plus a full read/write/verify pass over the PSRAM. Address-
uniqueness pass + 0x55/0xAA checkerboard pass per region. Results shown on
LCD and printed to `logbuf`.

New files: `Core/Inc/gw_psram_test.h`, `Core/Src/gw_psram_test.c` (driver:
indirect SPI commands only, chunked to never cross the chip's 1024-byte
page-wrap boundary -- deliberately never memory-mapped at 0x90000000, see
README for why), `Core/Inc/gw_ram_test.h`, `Core/Src/gw_ram_test.c` (the
test itself). Sound playback and the bouncing-image demo disabled (both
depended on the NOR-flash asset blob that no longer applies).

**Verified on real hardware**: all 4 internal regions + full 4MB PSRAM
(both pattern passes) PASS. PSRAM JEDEC read returned the real ISSI ID
(`MF=0x9D KGD=0x5D`, KGD PASS bit set) -- confirms genuine ISSI silicon,
not a misread.

### Debug connection troubleshooting (this session, most of the friction)

Spent a long stretch unable to get OpenOCD/gnwmanager to connect over SWD
at all (`STLINK_JTAG_GET_IDCODE_ERROR` / "unable to connect to the
target"). Ruled out via elimination, in order: probe speed, connect-under-
reset, physical rework redo, swapping to a second (stock, then confirmed-
unlocked) board, USB replug (confirmed via `dmesg` as a genuine
re-enumeration, not a stale session), pyocd as an alternate backend (same
`Get IDCODE error`). None of those were it.

**Actual fix**: `gnwmanager`'s own error message named it directly --
"Was able to connect to stlink probe, but unable to talk to the device.
Try releasing the Game & Watch power button at the same time as running
this command." Timing the command to fire the instant POWER is released
worked. This is a known G&W-specific quirk, not a probe/cable/board fault
-- all the elimination steps before finding this were wasted effort that a
faster read of gnwmanager's error text would have skipped. Worth
remembering for next time: prefer `gnwmanager info` over raw
`openocd ... init` when debugging G&W connection issues, since gnwmanager's
error messages are G&W-aware and openocd's are not.

Also recovered a real stock-firmware bank1 backup
(`C:\Users\Administrator\backups-2026-07-20-16-10-11\internal_flash_backup_zelda.bin`,
131072 bytes, hash `ab37ba03...`) and restored it to the stock/reference
board after testing on it, verified via SHA256 readback match.

### NOR+PSRAM piggyback (in progress, this session)

User physically piggybacked the PSRAM onto the same OSPI bus as the
original 64MB NOR flash (confirmed via `gnwmanager info` -> "External
Flash Size (MB): 64.0", read over the NOR's original hardware NCS *before*
any of today's firmware changes -- proves the piggyback wiring is
electrically sound independent of firmware). NOR keeps its original
position and CE# (PE11 / OCTOSPIM_P1_NCS, external pull-up, under the OSPI
peripheral's automatic control). PSRAM's CS was moved to PE9 (plain GPIO,
not wired to the OSPI peripheral at all, not bridged to PE11).

**The problem this creates**: the OSPI peripheral asserts its hardware NCS
pin (PE11) for the full duration of *every* command it issues, with no
per-call way to suppress it. Left alone, a PSRAM-targeted command (using
PE9) would *also* select the NOR flash via PE11 at the same time. Several
PSRAM opcodes (0x02 write, 0x0B read) are also valid NOR opcodes, so this
isn't just SIO-line contention -- it's a real risk of the NOR flash
executing the same command, potentially corrupting real stored NOR
content.

**Fix implemented**: new `gw_ospi_bus.h`/`.c`. Before every PSRAM
transaction, PE11 is detached from the OSPI peripheral (switched to a
plain GPIO input) so its physical net floats and is held HIGH by the
external pull-up -- deselecting NOR regardless of what the peripheral's
internal NCS state machine is doing -- then PE9 is driven low. Restored to
AF11/OCTOSPIM_P1_NCS afterward so the peripheral resumes automatic NCS
control for NOR access. Wired into `gw_psram_test.c`'s single command
chokepoint (`psram_cmd()`), so every PSRAM call is automatically bracketed
correctly.

Also added `gw_nor_test.h`/`.c`: a **read-only** presence + function check
for the piggybacked NOR (JEDEC ID read against a table mirroring
`gw_flash.c`'s known-parts list, for name + address-width; a 64-byte
read-twice-and-compare consistency check). Deliberately never
erases/writes -- this NOR may hold real stored ROM/asset/save data from
other firmware builds, unlike the PSRAM (volatile, always safe to fully
exercise).

`gw_ram_test.c` extended to run and report both PSRAM and NOR results
together, plus a free-text `detail` line for the NOR row (matched part
name or raw JEDEC bytes) since that check doesn't produce a meaningful
address/expected/actual triple the way the pattern tests do.

Build verified clean (51.4KB text / 128KB budget, no new warnings).
Flashed to the reworked board and verified via SHA256 readback match
(`bfdd3bfe...`). **Was mid-boot-and-log-capture of this build when this
log was requested** -- next step is to read the log buffer and confirm
both NOR (expect real 64MB part ID + consistent reads) and PSRAM (expect
same PASS as before) report correctly with the CS arbitration in place.

### Files changed (branch `ram-test`)

- `Core/Inc/gw_ospi_bus.h`, `Core/Src/gw_ospi_bus.c` -- new, CS arbitration
- `Core/Inc/gw_nor_test.h`, `Core/Src/gw_nor_test.c` -- new, NOR presence/read test
- `Core/Inc/gw_psram_test.h`, `Core/Src/gw_psram_test.c` -- wired into CS arbitration
- `Core/Inc/gw_ram_test.h`, `Core/Src/gw_ram_test.c` -- added NOR result + detail line
- `Core/Src/main.c` -- calls `NorTest_Init(&hospi1)` after `PSRAM_Init(&hospi1)`
- `Makefile` -- added the two new source files
- `README.md` -- documents the RAM test screen, the PSRAM addressing
  rationale (why never mapped at 0x90000000), and the gnwmanager
  "Check your soldering!" false alarm + OpenOCD-direct workaround
