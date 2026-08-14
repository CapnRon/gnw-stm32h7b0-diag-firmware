# Game & Watch STM32H7B0 Diagnostic Firmware

This is a diagnostic test firmware for the Nintendo Game & Watch, built and
tested on the Zelda edition (STM32H7B0VBTx). The button handling keeps the
Mario edition's shared TIME/SELECT and GAME/START inputs on purpose, so the
same firmware should also work on the Mario edition (STM32H7A3). Use it to
check hardware health after a repair. It is a fork of
[kbeckmann/game-and-watch-retro-go](https://github.com/kbeckmann/game-and-watch-retro-go).

## Why this firmware exists

A previous tool corrupted the STM32H7B0 option bytes on this board. The
corruption locked the flash and force-enabled the hardware watchdog. This
firmware includes a fix. On every boot, it checks the option bytes. If they do
not match a known-good reference value, it corrects them.

Use this firmware to check these things after a repair:

- The LCD panel and backlight work.
- Every button reports its correct state.
- The watchdog and option bytes stay correct across reboots.
- The speaker and volume control work.
- The battery and charge-status sensor work.

## What the firmware shows on screen

- A smooth scrolling color-bar pattern fills most of the screen.
- A status panel at the bottom shows all ten buttons. A button lights up
  green when you press it.
- A status readout in the top-right corner shows:
  - Volume level (0-9). Press **PAUSE** to cycle through volume levels.
  - Battery percentage and charge state.
  - Option-byte status. `OPT:OK` means the option bytes were already correct.
    `OPT:FIXED` means the firmware corrected them on this boot.
- A bouncing image moves around the color-bar area, like an old Amiga demo.
  Use this to check the LCD refresh and framebuffer at a glance.

## Sound test

Press any button to play a short sound effect. The firmware picks one at
random from a small built-in set. This checks the SAI audio peripheral and
speaker.

## Known hardware note

Some buttons share one logical input. `TIME` and `SELECT` share one bit.
`GAME` and `START` share one bit. This is normal. The original firmware
design uses this so the same code works on both the Mario and Zelda models.

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

Flash the firmware and the asset blob as two separate steps:

```bash
gnwmanager flash --location bank1 --file build/gw_retro_go_intflash.bin
gnwmanager flash --location ext --file assets/assets.bin
```

See [`assets/README.md`](assets/README.md) for full asset-build and flashing
details.

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
