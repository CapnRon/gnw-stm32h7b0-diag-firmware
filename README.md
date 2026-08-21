# Game & Watch STM32H7B0 Diagnostic Firmware

This is a diagnostic test firmware for the Nintendo Game & Watch, built and
tested on the Zelda edition (STM32H7B0VBTx). The button handling keeps the
Mario edition's shared TIME/SELECT and GAME/START inputs on purpose, so the
same firmware should also work on the Mario edition (STM32H7A3). Use it to
check hardware health after a repair. It is a fork of
[kbeckmann/game-and-watch-retro-go](https://github.com/kbeckmann/game-and-watch-retro-go).

## PSRAM chip-select auto-detection (`ram-test` branch)

The `ram-test` branch adds a PSRAM/NOR diagnostic suite. The PSRAM
(ISSI IS66WVS4M8FALL, 4MB) can be wired with its CE# on one of three pins
depending on the hardware revision, and the firmware detects it
automatically at boot:

1. **PE11** - the OCTOSPI hardware NCS pin (AF11). No GPIO work needed.
2. **PC11** - alternate location of the same NCS signal (AF9).
3. **PE9**  - plain GPIO output (the original NOR-sharing piggyback wiring).

`OspiBus_ProbePsram()` issues a Read-ID (9Fh) on each candidate and latches
the first one that answers MF=0x9D / KGD=0x5D. The test log prints the
detected pin, e.g. `PSRAM CS: PE11 (OSPI NCS)`. The RAM test then runs the
full read/write verification (two pattern passes) over the 4MB PSRAM, plus
all four internal SRAM regions, and reports the NOR JEDEC ID (absent when
the NOR has been removed). If no PSRAM answers on any candidate, PE11 is
restored to the OSPI peripheral's hardware NCS function before the probe
returns -- the last candidate leaves it parked as a plain input, and NOR
(which is hardware-NCS on PE11, the board's stock pin) would otherwise
silently stop responding to every command afterward.

## Benchmark suite (A/B: BENCH)

Alongside the pass/fail RAM test, pressing A or B switches to a clock/OSPI
benchmark sweep (`Core/Src/gw_bench.c`). It measures real read+write
throughput and latency for every internal SRAM region, the PSRAM (indirect
and memory-mapped, when present), and the NOR flash, at four CPU/OSPI clock
levels: Stock (280/64 MHz), Intermediate (312/104), Maximum (340/97), and
Aggressive (354/101). Use LEFT/RIGHT to page through clock levels.

Results below are from a real board with the NOR flash reinstalled
(PSRAM removed), MX25U51245G detected:

| Device | Clock level | Write | Read | Latency |
|---|---|---|---|---|
| DTCM Heap | Stock (280/64) | 266.5 MB/s | 266.3 MB/s | 29ns |
| DTCM Heap | Intermediate (312/104) | 296.2 MB/s | 296.9 MB/s | 26ns |
| DTCM Heap | Maximum (340/97) | 323.6 MB/s | 323.4 MB/s | 24ns |
| DTCM Heap | Aggressive (354/101) | 336.4 MB/s | 336.2 MB/s | 23ns |
| AXI RAM_CORE | Stock | 266.5 MB/s | 230.1 MB/s | 39ns |
| AXI RAM_CORE | Intermediate | 297.0 MB/s | 256.6 MB/s | 35ns |
| AXI RAM_CORE | Maximum | 323.2 MB/s | 279.1 MB/s | 32ns |
| AXI RAM_CORE | Aggressive | 336.4 MB/s | 295.6 MB/s | 31ns |
| AXI RAM_EMU | Stock | 266.7 MB/s | 230.6 MB/s | 39ns |
| AXI RAM_EMU | Intermediate | 297.1 MB/s | 257.1 MB/s | 35ns |
| AXI RAM_EMU | Maximum | 323.8 MB/s | 280.0 MB/s | 32ns |
| AXI RAM_EMU | Aggressive | 336.9 MB/s | 291.2 MB/s | 31ns |
| AHB SRAM1/2 | Stock | 194.2 MB/s | 92.8 MB/s | 50ns |
| AHB SRAM1/2 | Intermediate | 216.3 MB/s | 103.4 MB/s | 45ns |
| AHB SRAM1/2 | Maximum | 235.8 MB/s | 112.7 MB/s | 41ns |
| AHB SRAM1/2 | Aggressive | 245.3 MB/s | ~115-117 MB/s | 40ns |
| NOR flash, IO:SPI (indirect) | Stock | 0.20 MB/s | 7.55 MB/s | 3.46us |
| NOR flash, IO:QUAD (indirect) | Stock | 0.20 MB/s | 8.81 MB/s | 5.11us |
| NOR flash, IO:SPI (indirect) | Intermediate | 0.21 MB/s | 9.85 MB/s | 2.70us |
| NOR flash, IO:QUAD (indirect) | Intermediate | 0.20 MB/s | 9.82 MB/s | 4.23us |
| NOR flash, IO:SPI (indirect) | Maximum | 0.21 MB/s | 10.73 MB/s | 2.59us |
| NOR flash, IO:QUAD (indirect) | Maximum | 0.21 MB/s | 10.70 MB/s | 4.00us |
| NOR flash, **memory-mapped** | Stock | -- | 30.08 MB/s | 0.04us |
| NOR flash, **memory-mapped** | Intermediate | -- | 48.89 MB/s | 0.03us |
| NOR flash, **memory-mapped** | Maximum | -- | 45.66 MB/s | 0.03us |

All rows PASS. PSRAM rows are absent here since it's physically removed on
this board; when present, it reports its own indirect and memory-mapped
read/write numbers per clock level and sample-shift setting.

### NOR: real reads are memory-mapped-only, real writes are indirect-only

This is the actual, confirmed access pattern for NOR on this board's real
game firmware (`gw_flash.c`, checked directly -- not inferred): **reads
and writes never use the same mechanism.**

- **Read is memory-mapped-only.** `OSPI_Init()` ends every boot by calling
  `OSPI_EnableMemoryMappedMode()` and leaves it as the resting state.
  Checked directly against the real driver's source: `OSPI_Read()`/
  `OSPI_ReadBytes(CMD(READ), ...)`, the indirect bulk-read path, has
  **zero callers anywhere in the codebase**. Every real data read --
  ROM, assets, anything -- goes through the memory-mapped window. The
  only indirect reads that exist at all are small control-plane ones:
  RDID, RDSR, RDCR, SFDP (status/ID/config bytes, never bulk data).
- **Write is indirect-only, always.** `OSPI_EnableMemoryMappedMode()`'s
  write config deliberately reuses the *read* instruction, specifically
  so a stray store through the mapped pointer re-reads instead of
  programming the chip -- see the code comment, "in order to not alter
  the flash by accident." There is no real memory-mapped NOR write path
  in the stock driver at all. Every real write (`OSPI_WriteBytes()`,
  `OSPI_PageProgram()`, `_OSPI_Erase()`) is an indirect command.

This diag firmware's benchmark rows above reflect that split faithfully:
the indirect NOR rows (SPI/QUAD) exist for a controlled apples-to-apples
comparison against PSRAM's own indirect numbers, and the memory-mapped
NOR row measures the access path real gameplay actually uses for reads.
There is no memory-mapped NOR write row, and there will not be one --
this isn't a missing feature, it reflects the same real limitation the
stock driver works around. A memory-mapped write attempt was built and
tested here (real hardware, both the DQS errata fix and the
Strongly-Ordered MPU region that make PSRAM's memory-mapped writes work
correctly, applied to NOR's write config the same way): every write was
silently a complete no-op -- confirmed by reading back through the exact
same mapped session immediately after the write, before any mode switch,
still showing the pre-write erased value. Ruled out address/opcode
mistakes (write config matched `gw_flash.c`'s `cmds_quad_32b_mx` CMD_PP
exactly) and write-completion timing (added explicit WIP polling between
every page write; made no difference). NOR's write config not working
the same way PSRAM's does is not obviously wrong given how the two chips
differ (PSRAM is genuine RAM with no internal write-completion latency
to race against; NOR's real per-page program time has no way to
interleave with an automatic hardware burst the way indirect mode's
explicit WIP poll between commands provides) -- but the specific
mechanism was not conclusively identified, and this was not pursued
further once it became clear the reference driver never attempts this
either.

Beyond the write-side split, NOR's read speed itself has two very
different profiles depending on how it's accessed: indirect reads are
throttled by fixed per-command overhead regardless of SPI mode (SPI vs
QUAD barely differ -- 8.81 vs 7.55 MB/s at Stock, QUAD actually *slower*
at higher clock levels once its extra dummy cycles stop being hidden by
a larger transfer), while memory-mapped reads have no such overhead and
reach 30-49 MB/s, matching PSRAM's own throughput almost exactly since
both chips share the same OCTOSPI1 peripheral. Real gameplay only ever
sees the memory-mapped number -- the indirect rows exist purely for a
controlled, apples-to-apples SPI-vs-QUAD comparison, not because
gameplay experiences indirect-mode NOR reads at all.

### Flashing (patched OpenOCD)

The G&W's STM32H7B0 exposes an undocumented 256KB bank; flashing requires
the patched OpenOCD build. See the `psram-only` branch of
[CapnRon/game-and-watch-retro-go-sd](https://github.com/CapnRon/game-and-watch-retro-go-sd)
for the full procedure; this repo's `scripts/interface_stlink.cfg` uses
`set DUAL_BANK 0` (256KB bank at 0x08000000).

### Companion repos

- **Firmware**: [CapnRon/game-and-watch-retro-go-sd](https://github.com/CapnRon/game-and-watch-retro-go-sd)
  branch `psram-only` - Retro-Go SD running fully on the PSRAM (no NOR).
- **Flasher**: [CapnRon/gnwmanager](https://github.com/CapnRon/gnwmanager)
  branch `psram-only` - PSRAM-tolerant gnwmanager bootloader.

## Why this firmware exists

A previous tool corrupted the STM32H7B0 option bytes on this board. The
corruption locked the flash and force-enabled the hardware watchdog. This
firmware includes a fix. On every boot, it checks the option bytes. If they do
not match a known-good reference value, it corrects them.

Use this firmware to check these things after a repair:

- The LCD panel and backlight work.
- The watchdog and option bytes stay correct across reboots.
- The battery and charge-status sensor work.
- STANDBY mode and the power-button wakeup path work.
- Every internal SRAM region and the full external OSPI chip hold data
  correctly.

## `ram-test` branch

This branch swaps the usual color-bar/sound/bouncing-image demo for a RAM
test screen. It exists because the external OSPI flash footprint on this
board was reworked to hold an
[ISSI IS66WVS4M8FALL/BLL](https://www.issi.com) 32Mbit (4MB) Serial/QPI
PSRAM instead of NOR flash, and this branch verifies that swap.

On boot it runs an address-uniqueness pass and a 0x55/0xAA checkerboard
pass over every internal SRAM region this build doesn't otherwise use
(DTCM heap, the free tail of AXI RAM_CORE, all of AXI RAM_EMU, and the
free tail of AHB SRAM1/2), plus a full read/write/verify pass over the
entire external PSRAM. Results are shown on screen and printed to the
`logbuf` UART/log ring (see [`scripts/dump_logs.sh`](scripts/dump_logs.sh)),
one PASS/FAIL line per region plus the exact failing address/expected/
actual value if a region fails.

Sound playback and the bouncing-image demo are disabled on this branch --
both depended on the asset blob that used to live in that external flash
slot, which no longer applies now that the slot holds PSRAM. The button
panel and volume readout are also removed; POWER-hold-to-shutdown still
works. See [`Core/Src/gw_psram_test.c`](Core/Src/gw_psram_test.c) for the
PSRAM driver (indirect SPI commands only -- this chip wraps reads/writes
at 1024-byte page boundaries, so it is deliberately never memory-mapped)
and [`Core/Src/gw_ram_test.c`](Core/Src/gw_ram_test.c) for the test itself.

This branch needs only the internal-flash image; there is no asset blob to
build or flash (see [Flashing tools](#flashing-tools) below).

### Why the PSRAM is never mapped at 0x90000000

`0x90000000` is the STM32H7's OCTOSPI1 memory-mapped window. Once the OSPI
peripheral is configured for it, the external chip appears as flat,
directly-readable memory at that address -- `*(uint8_t*)(0x90000000 +
offset)` works, and the OSPI controller transparently issues the right SPI
command underneath. This is how the *original* NOR-flash firmware works:
the linker script sets `__EXTFLASH_BASE__ = 0x90000000`, and game ROM data,
the emulator overlay code, and save states are all addressed as if they
were just sitting in memory at `0x90000000 + offset`. That's safe for NOR
flash, which reads linearly with no surprises.

It is not safe for this PSRAM. The IS66WVS4M8 datasheet is explicit that
reads and writes are "always wrapped within page": every transaction
silently wraps back to the start of its current 1024-byte page instead of
continuing into the next one. If memory-mapped mode were enabled the usual
way, the CPU's cache and AXI bus would do what they always do -- burst-read
chunks larger than 1024 bytes, or reads that straddle a 1024-byte boundary,
whenever it feels like it -- and the PSRAM would silently hand back
wrapped-around garbage for any burst crossing a page boundary. No error,
just wrong data.

So [`gw_psram_test.c`](Core/Src/gw_psram_test.c) never calls
`HAL_OSPI_MemoryMapped()`. Every access goes through `PSRAM_Read()` /
`PSRAM_Write()`, which issue individual, explicit SPI commands and manually
chunk every transfer so no single command ever crosses a 1024-byte
boundary. The addresses passed to those functions (`0x000000` through
`0x3FFFFF`, the chip's 4MB range) aren't CPU pointers -- they're the 24-bit
address field baked into each SPI command, telling the PSRAM chip which of
its internal bytes to read or write. There is no `*(ptr)` access to PSRAM
anywhere in this firmware; it's all indirect, command-by-command.

## Power off

Hold **POWER** for 5 seconds to power off the device. This checks that
STANDBY mode and the power-button wakeup path both work. Press **POWER**
again to turn the device back on.

## Known hardware note

Some buttons share one logical input. `TIME` and `SELECT` share one bit.
`GAME` and `START` share one bit. This is normal. The original firmware
design uses this so the same code works on both the Mario and Zelda models.
This branch doesn't display per-button state on screen (see
[`ram-test` branch](#ram-test-branch) above), but the shared-bit behavior
still applies if you read `buttons_get()` yourself.

## Building

Same build process as the upstream project. See
[Prerequisites](#prerequisites) below for the ARM GCC toolchain and Python
dependencies. From the repo root:

```bash
make -j8 GNW_TARGET=zelda EXTFLASH_SIZE_MB=64
```

The sound and image assets are not stored in this repo. Build your own
asset blob from your own audio and image files with the script in
[`assets/`](assets/README.md).

## Flashing tools

This firmware is flashed with [gnwmanager](https://github.com/BrianPugh/gnwmanager),
not the `make flash` target the upstream project's own instructions use
below. Install it with:

```bash
pip install gnwmanager
```

`gnwmanager` needs a debug-probe backend. Its default backend is
[OpenOCD](https://openocd.org/), which most package managers ship
(`apt-get install openocd` on Debian/Ubuntu).

On this branch, only the internal-flash image needs flashing -- there is no
asset blob:

```bash
gnwmanager flash --location bank1 --file build/gw_retro_go_intflash.bin
```

**On a board with the PSRAM swap**, `gnwmanager` will refuse to run any
command, including the one above, with `Failed to communicate with external
flash chip. Check your soldering!` -- this is a false alarm. `gnwmanager`
unconditionally probes the external chip's JEDEC ID as part of connecting,
and PSRAM returns a different ID format than the NOR flash it expects, which
it doesn't recognize. It's not a soldering problem. Flash straight through
OpenOCD instead, which only touches internal flash and never probes the
external chip:

```bash
make flash_intflash
```

See [`assets/README.md`](assets/README.md) for full asset-build and flashing
details (not applicable to this branch, but relevant on `main`).

## Credits

This firmware is a fork of
[kbeckmann/game-and-watch-retro-go](https://github.com/kbeckmann/game-and-watch-retro-go),
which is itself based on [ducalex/retro-go](https://github.com/ducalex/retro-go)
and [sylverb/game-and-watch-retro-go](https://github.com/sylverb/game-and-watch-retro-go).
See the original project for the full emulator feature set, controls, and
build instructions. This fork strips out the emulator and ROM-loading code
and replaces it with the diagnostic tests described above.

This firmware also builds against two upstream git submodules, unmodified:

- [kbeckmann/retro-go-stm32](https://github.com/kbeckmann/retro-go-stm32)
- [bzhxx/LCD-Game-Emulator](https://github.com/bzhxx/LCD-Game-Emulator)

Flashing this firmware to real hardware relies on
[BrianPugh/gnwmanager](https://github.com/BrianPugh/gnwmanager) and
[OpenOCD](https://openocd.org/). See [Flashing tools](#flashing-tools) above.

This project is licensed under the GNU GPL v2. See [LICENSE](LICENSE).
