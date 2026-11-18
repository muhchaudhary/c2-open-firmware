# Firmware

Open firmware for GameSir pads on JieLi's BR23 chips (AC695X / AC632N
family), built on JieLi's public SDK
([fw-AC63_BT_SDK](https://github.com/Jieli-Tech/fw-AC63_BT_SDK)). Boards:
the **Cyclone 2** (`boards/c2`) and its **USB receiver** (`boards/dongle`).
What it does and how to use it: the [top-level README](../README.md); how it
works and what was learned: [`docs/firmware-design.md`](../docs/firmware-design.md).

## Layout

```
firmware/
├── src/
│   ├── gp_main.c              the SDK application: boot, Bluetooth start-up, event dispatch
│   ├── core/                  the gamepad, independent of the hardware
│   │   ├── gamepad.h              gp_state, button bits, the clock, the board interface
│   │   ├── gp_core.c              report tick, app-task poll, combos, stall detector
│   │   ├── gp_transport.h         how reports reach the host
│   │   ├── gp_bt.c                Bluetooth Classic HID: identities, sending, link safety nets
│   │   ├── gp_usb.c               USB: wired gamepad (SInput or XInput) + debug log (CDC)
│   │   ├── gp_link.c              the receiver link protocol (shared with the receiver)
│   │   ├── gp_sinput.c            SInput host commands
│   │   ├── gp_sinput_desc.c       SInput descriptor and encoders (shared with the receiver)
│   │   ├── gp_xinput.c            wired Xbox 360: commands
│   │   ├── gp_xinput_desc.c       wired Xbox 360: encoder, USB descriptors (shared)
│   │   ├── gp_xboxbt.c            Xbox One S over Bluetooth: descriptor, reports, rumble
│   │   ├── gp_ds4_desc.c          DS4: descriptors, reports, feature reports, output parsing (testable)
│   │   ├── gp_ds4.c               DS4: the host's rumble and lightbar applied
│   │   ├── gp_switch_desc.c       Switch Pro Controller: descriptors, subcommands, SPI page, reports (testable)
│   │   ├── gp_switch.c            Switch: the host's rumble and player lights applied
│   │   ├── gp_config.c            the configuration protocol (docs/config-protocol.md)
│   │   ├── gp_settings.c          settings table, profiles, records
│   │   ├── gp_store.c             record store (default: syscfg; boards may replace it)
│   │   ├── gp_cal.c               calibration
│   │   ├── gp_tuning.c            stick and trigger shaping per profile
│   │   ├── gp_motion.c            gyro bias learning
│   │   ├── gp_rumble.c            host rumble, level and its hotkeys
│   │   ├── gp_leds.c              host colours, player LEDs, Home animations, low battery
│   │   ├── gp_power.c             power-off paths
│   │   ├── gp_charging.c          charging mode and its animation
│   │   └── gp_log.c               USB debug log, log commands, config over the log
│   └── drivers/               reusable parts, configured by a board
│       ├── stick_axis             stick axis: centre learning, scaling, radial dead zone
│       ├── hjc_stick              GameSir's stick chip (UART stream), stream mean, latency probe
│       ├── hall_trigger           hall-sensor triggers on the ADC, stoppers, end-stops
│       ├── qmi8658                QMI8658 IMU
│       ├── erm_rumble             ERM motors on MCPWM, GameSir's levels and kick
│       ├── battery_adc            battery on an ADC divider: measured tables, external charger
│       ├── ws2812_spi             WS2812 LED chain on SPI1
│       ├── jl_iic                 hardware IIC with bounded waits
│       └── eeprom24               24Cxx I2C EEPROM
├── boards/
│   ├── c2/                    GameSir Cyclone 2
│   │   ├── gamepad_c2.c           the board interface: buttons, drivers, battery tables, log commands
│   │   ├── c2_store.c             records in the I2C EEPROM (layout in its header)
│   │   ├── board_c2.c             SDK board: power, wake-up, soft-off, bootloader combo
│   │   ├── board_c2_cfg.h         SDK configuration + identity (name, VID / PID)
│   │   ├── c2_pins.h              pin map, decompiled from stock with addresses
│   │   ├── c2_power.h             power and wake-up settings from stock
│   │   └── image.json             the flash image's layout (firmware/mkimage.py)
│   └── dongle/                the C2 receiver (docs/dongle.md)
│       ├── dg_main.c              the application, LED, button, log commands
│       ├── dg_usb.c               USB: idle / SInput / XInput + debug log
│       ├── dg_bt.c                Bluetooth: pairing, reconnect, config relay
│       ├── dg_link.c              the link protocol → SInput
│       ├── board_dongle.c         SDK board, crash-loop guard, breadcrumbs
│       ├── sources                the files from src/ it builds
│       └── image.json             its image: the app area alone (firmware/mkimage.py)
├── sdk/
│   ├── SDK_COMMIT             the fw-AC63_BT_SDK commit the patches are for
│   ├── sync.sh                installs src/ and boards/<name>/ into the SDK as apps/gamepad
│   └── patch_sdk.py           the SDK changes (build list, board selection, HID, link address, …)
├── test/                      host unit tests (make -C firmware/test)
├── images/                    tested images and their changelog
├── VERSION                    the release number (major.minor.patch), in the INFO reply
├── build.sh                   sync + make + pack (+ --flash)
├── pack.sh                    sdk.elf → app.bin → mkimage.py
└── mkimage.py                 the board's flash image from app.bin + the SDK's uboot.boot / cfg_tool.bin
```

The SDK checkout is a build tree: `sdk/sync.sh` restores the SDK files it
patches from git, copies this directory in, and patches them again. Nothing
here is edited inside the SDK.

## Building and flashing

Needs the SDK next to this repository (`../fw-AC63_BT_SDK`, or
`SDK=/path`) at the commit in `sdk/SDK_COMMIT` (`sync.sh` stops on another;
`SDK_ANY_COMMIT=1` tries it anyway), and the dev shell (`flake.nix`; any
Linux with [Nix](https://nixos.org/download), flakes on): `nix develop`, or
direnv with the repo's `.envrc`. It brings the JieLi toolchain (the
hash-checked archive `fw-re/get-toolchain.sh` fetches, patched to run
without a `/lib64` loader: `$JL_TOOLCHAIN`), the flasher's Python
(`fw-re/.jlpy`), make, gcc and the USB tools. `build.sh` enters it by
itself.

```sh
git clone --depth 1 https://github.com/Jieli-Tech/fw-AC63_BT_SDK ../fw-AC63_BT_SDK
git -C ../fw-AC63_BT_SDK fetch --depth 1 origin $(cat firmware/sdk/SDK_COMMIT)
git -C ../fw-AC63_BT_SDK checkout $(cat firmware/sdk/SDK_COMMIT)
```

Without Nix: `fw-re/get-toolchain.sh` (into `fw-re/jl-toolchain`, where
the scripts look when `$JL_TOOLCHAIN` isn't set), Python 3 with `pyyaml
crcmod pycryptodomex tqdm` at `fw-re/.jlpy`, and GNU make; `build.sh`
then runs as is.

```sh
firmware/build.sh                    # → firmware/build/c2.flash.bin, c2.elf
BOARD=dongle firmware/build.sh       # the receiver → firmware/build/dongle.flash.bin (docs/dongle.md)
sudo fw-re/jlflash.sh write firmware/build/c2.flash.bin
```

Build as yourself, not root (`build.sh` refuses root: root's files in the
SDK checkout break the next build); `--flash` asks sudo for the flash step
only. `sync.sh` warns about SDK files changed outside its patches. The SDK
compiles with `-w`; `build.sh --wall` also checks our sources with `-Wall`
(`firmware/sdk/wall.mk`, the SDK's headers as system headers) and fails on
any warning, as CI does (`.github/workflows/firmware.yml`, both boards on
every push and PR, the images as run artifacts). `mkimage.py` refuses an SDK
`uboot.boot` or `cfg_tool.bin` other than the tested ones (their sha256;
licences: `LICENSING.md`). Builds are reproducible: the same commit gives
the same image byte for byte, here or in CI (the SDK's `__DATE__` /
`__TIME__` are fixed by `patch_sdk.py`; the toolchain is the flake's). `jlflash.sh write` gets into the bootloader through the debug log
port and picks the image's device (controller or receiver) by its USB
product string.

**Releases**: INFO carries the release next to the git commit.

1. Raise `VERSION` (patch for fixes, minor for features, major when the
   configuration protocol breaks clients) and commit: the images must be
   built from a commit, not from a tree with changes (`"commit"` without `+`).
2. Build both boards and test them on the hardware.
3. Copy each into `firmware/images/` with its sidecar: `build/c2.flash.bin`
   and `build/c2.json` → `C2_pad.flash.bin`, `C2_pad.json`; `dongle.*` →
   `C2_receiver.*`. Add the changelog entry, commit, and merge with a merge
   commit (not squash or rebase: each sidecar's commit must stay in main's
   history).
4. Run the **release** workflow (Actions, on main). It rebuilds every image
   at the commit in its sidecar, fails unless each is byte-identical to the
   committed one, and uploads the release folder (images, ELFs, sidecars,
   `manifest.json`, `SHA256SUMS`, the licences).
5. Tag the commit it names (`git tag vX.Y.Z`) and publish the folder.

## Host tests

The hardware-independent code runs on the PC: the SInput / XInput / DS4 / Switch /
receiver-link encoders, the settings table and its records, stick and
trigger shaping, the stick axis and hall trigger drivers, gyro bias
learning and the battery percent. No SDK or toolchain needed:

```sh
make -C firmware/test             # build and run all (gcc or clang, ASan + UBSan)
make -C firmware/test test_motion # one; TEST_VERBOSE=1 shows the firmware's log lines
```

Each `test_<name>.c` is built with `fake.c` (a clock, ADC, store and log the
test controls) and the firmware files it tests (`src_<name>` in the
Makefile), with `shim/` standing in for the SDK headers. Time-based code
runs on `fake_ms`, so a test checks "after 1.5 s" without waiting. GitHub
Actions runs them on every push and pull request, with gcc and clang
(`.github/workflows/host-tests.yml`).

## How it fits together

- **Report tick** (`gp_core.c`, a timer interrupt; asks for 2 ms, fires
  about every 4 ms): the board's buttons and `gp_board_read()` fill a
  `struct gp_state`; the core applies the profile (remap, turbo, shaping),
  encodes the active protocol's report and posts it. At most one report is
  in flight. Durations go by `gp_ms()`, never by counting ticks.
- **Poll** (`gp_core.c`, every 50 ms, app task): combos, power-off timers,
  LEDs, configuration, settings saves (2 s after the last change), the
  transport's safety nets, the status line. Anything that writes the store
  or sends Bluetooth commands runs here.
- **Transport** (`gp_transport.h`): `connected`, `busy`, `post`. Bluetooth
  sends straight to the controller with the scheduler held (the SDK's
  command ring has no lock; `gp_bt.c`); USB writes the interrupt endpoint
  from the tick; the receiver link sends `gp_link` messages. Host commands
  go to `sinput_command()` (or the XInput / Xbox / DS4 handlers).
- **Modes and links**: Steam (SInput), Xbox, DS4 or Switch; Bluetooth or the receiver.
  Each combination is its own Bluetooth identity, so every host keeps its
  pairing. A cable gives the debug log and charging; Home + Capture tapped
  makes it the gamepad. The combos: the top-level README.
- **Storage**: settings and calibration are records (`gp_store.h`); the C2
  keeps them in its I2C EEPROM (`boards/c2/c2_store.c`).
- **Configuration**: SInput reports or the debug log port, any mode
  (`docs/config-protocol.md`); clients `tools/c2cfg.html` and
  `tools/c2cfg.py`.

## Adding a pad

1. Map the stock firmware's pins and power settings (the `fw-re/sdk/`
   tools; `boards/c2/c2_pins.h` shows the level of detail that kept the C2
   from bricking).
2. Make `boards/<name>/`: copy `boards/c2`, rename the `board_c2*` files
   (the SDK selects `CONFIG_BOARD_<NAME>` and includes `board_<name>_cfg.h`),
   and set the identity at the top of the config header.
3. Write `gamepad_<name>.c`: `gp_board_info` (name, SInput capabilities, LED
   layout, motors) and the functions declared at the end of
   `core/gamepad.h`, built from the drivers. A pad with ADC sticks feeds
   `stick_axis` from the ADC instead of `hjc_stick`.
4. `BOARD=<name> firmware/build.sh`.
