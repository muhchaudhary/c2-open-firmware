# GameSir Cyclone 2: open firmware

Open firmware for the **GameSir Cyclone 2** (C2) controller and its USB
receiver, built on JieLi's public SDK for the controller's chip (AC695X /
BR23). It replaces GameSir's firmware entirely; the source is in
[`firmware/`](firmware/).

This is an independent project, not affiliated with or endorsed by GameSir.
Flashing replaces the controller's firmware: back it up first, and know the
way back ([Flashing](#flashing)). It works on our controllers, but you flash
at your own risk.

## What it does

- **Steam mode**: an [SInput](docs/sinput.md) gamepad (`2E8A:10C6`), which SDL
  and Steam drive natively: every button including both back paddles,
  analog triggers, gyro and accelerometer, both rumble motors, battery and
  charging (also as a Linux battery), the host's LED colours and the player
  number.
- **Xbox mode**: an Xbox One S pad over Bluetooth, a wired Xbox 360 pad over
  USB.
- **DS4 mode**: a DualShock 4 over Bluetooth and USB, for PCs
  ([`docs/ds4.md`](docs/ds4.md); not a PS4 console, which needs Sony's
  authentication chip). Not tested on hardware yet.
- **Switch mode**: a Switch Pro Controller over Bluetooth and USB, for the
  Switch itself and PCs ([`docs/switch.md`](docs/switch.md); no chip
  needed). Tested on Linux over Bluetooth, USB and the receiver; not yet on
  a Switch console.
- **Three ways to connect**, in any mode: Bluetooth to the computer, a
  USB cable, or the [C2 receiver](docs/dongle.md) running this project's
  receiver firmware.
- **Configuration**, saved on the controller: 4 profiles with stick and
  trigger tuning (dead zones, curves, hair trigger), button remapping and
  turbo; rumble strength; idle power-off; calibration. From the browser
  ([`tools/c2cfg.html`](tools/README.md#configuration)) or the command line
  (`tools/c2cfg.py`), in every mode.
- Low stick latency (5.5 ms from the stick chip to the report,
  [`docs/latency.md`](docs/latency.md)), a measured battery curve, the
  charging dock, a low-battery warning, a USB debug log.

## Getting started

1. **Back up** the controller first (it keeps GameSir's firmware, pairings
   and calibration): see [Flashing](#flashing).
2. **Flash** an image: the repository's Releases have tested builds (from
   0.2.2; [`firmware/images/`](firmware/images/) has the changelog), or
   [build](firmware/README.md#building-and-flashing) the latest from source.
3. **Pair** it: power on with Home, hold Home + Capture 3 s for pairing
   mode, and pair "Cyclone 2 SI" from the computer.
4. **Configure** it: open [`tools/c2cfg.html`](tools/c2cfg.html) in Chrome
   or Edge.

## Controls

| Combo | Action |
|---|---|
| Home | power on |
| Home held 5 s | power off (the LEDs go dark; off on release) |
| Home + Capture held 3 s | pairing mode |
| Home + Capture tapped, USB cable in | USB input on / off (the cable alone only charges and gives the debug log) |
| Home + A held 3 s | Steam mode (SInput; stock's HID / Android combo) |
| Home + X held 3 s | Xbox mode |
| Home + B held 3 s | DS4 mode |
| Home + Y held 3 s | Switch mode |
| Home + M held 3 s | link: Bluetooth ↔ the C2 receiver |
| M + Up / Down | rumble strength (1 s of rumble at the new level) |
| M + Left / Right | profile (shown on the player LEDs) |
| Home + Options, USB in, at power-on or held 2 s | USB bootloader (for flashing) |

**LEDs**: Home breathes while reconnecting and flashes in pairing mode, in
the mode's colour (Steam yellow, Xbox green, DS4 blue, Switch red: stock's combos and colours); connected, it takes the host's
colour (Switch mode: red). The player LEDs show the player number in the link's colour
(Bluetooth blue, USB white, receiver purple). A short red flash on Home
every 2 s (Switch mode: a short dark blink): battery at 5 %. With USB in and the controller off: the charging
animation.

## Docs

The firmware:

- [`firmware/README.md`](firmware/README.md): source layout, building,
  host tests, adding a pad
- [`docs/firmware-design.md`](docs/firmware-design.md): how it works, the
  hardware, what was learned
- [`docs/config-protocol.md`](docs/config-protocol.md): the configuration
  protocol
- [`docs/sinput.md`](docs/sinput.md): SInput as the controller speaks it
- [`docs/ds4.md`](docs/ds4.md): DS4 mode
- [`docs/switch.md`](docs/switch.md): Switch mode
- [`docs/dongle.md`](docs/dongle.md): the C2 receiver
- [`docs/latency.md`](docs/latency.md): latency measurements
- [`docs/plan.md`](docs/plan.md): the work plan
- [`tools/README.md`](tools/README.md): the PC tools

The stock firmware, for reference:

- [`docs/stock-features.md`](docs/stock-features.md): stock features from
  their strings (PS4/PS5, Switch, audio, EEPROM, test modes, VM map)
- [`docs/gamesir-connect.md`](docs/gamesir-connect.md): what GameSir's app
  does with a C2

## Layout

```
.
├── firmware/               the firmware: source, boards, build, tests, images
├── tools/                  PC tools: configuration, debug log, latency, battery
├── docs/                   design, protocols, receiver, measurements, stock reference
├── fw-re/                  JieLi image tools, the flasher, reverse-engineering tools
│   ├── jlflash.sh              Linux flasher (sudo, /dev/sg)
│   ├── enter_bootloader.py     GameSir's HID command → USB bootloader
│   ├── mkufw.py  jl.py  flash.py  ufw.py    image tools (the builder is firmware/mkimage.py)
│   ├── get-toolchain.sh        the JieLi toolchain, hash-checked (without Nix; the dev shell has it)
│   ├── sdk/                    reverse-engineering tools (sdkmatch, pin and register scans)
│   └── get-uboot-tool.sh       the flasher's engine, kagaimiq's jl-uboot-tool (MIT), pinned (without Nix)
└── tools/usbkey-mega2560/  the ROM's USB key, for a controller whose firmware doesn't boot
```

GameSir's firmware is not in this repository and no build needs it.

## Flashing

`fw-re/jlflash.sh` drives the chip's USB bootloader natively on Linux:

```sh
sudo fw-re/jlflash.sh write <flash.bin>     # into the bootloader on the image's device (controller or
                                            # receiver), write, read back, verify, restart
sudo fw-re/jlflash.sh enter [pad|receiver]  # only enter the bootloader (4c4a:2342)
sudo IMG_ADDR=0 LEN=0x100000 fw-re/jlflash.sh read c2_full_backup.bin   # whole-chip backup
```

It needs two things, both gitignored: kagaimiq's
[jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool) (MIT, the
flasher's engine) at `fw-re/jl-uboot-tool`, and a Python with `pyyaml
crcmod pycryptodomex tqdm` at `fw-re/.jlpy`. The dev shell links both
there (`nix develop` once, or direnv), so `sudo`, which drops the shell's
PATH, still finds them. Without Nix: `fw-re/get-uboot-tool.sh` (the same
pinned commit) and a virtualenv with those four packages at `fw-re/.jlpy`.

From this firmware, `write` gets into the bootloader through the debug log;
from GameSir's, hold Home + Options and plug in USB. `write` only takes a
raw flash image that passes `mkufw.py verify` (not a `.ufw`), checks the
image is for the device it finds (`FORCE=1` overrides), and only restarts
the chip if the read-back matches, so a bad transfer can't brick it. A
valid image that crashes before USB can: then the ROM's USB key
([`tools/usbkey-mega2560/`](tools/usbkey-mega2560/)) is the way back.

The firmware keeps its settings in the controller's EEPROM: its first boot
formats the start of it (stock's factory calibration is copied into the
new format, and stock's own copy is left in place). Going back to stock
fully means restoring that area too; `tools/c2eeprom.py` reads the EEPROM
through the debug log. After changing firmware, remove the controller from
the computer's Bluetooth (`bluetoothctl remove <mac>`) and pair again.

## Licence

Apache-2.0 (`LICENSE`). The images also contain JieLi's SDK (Apache-2.0)
and the toolchain's newlib and compiler-rt: `LICENSING.md`.
