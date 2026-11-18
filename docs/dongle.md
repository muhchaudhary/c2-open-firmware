# The C2 receiver

GameSir's "2.4G" USB receiver for the Cyclone 2 is a Bluetooth Classic
device on the same chip as the pad (AC695X, BR23), with 512 KB of flash.
Our firmware for it is `firmware/boards/dongle/` (it builds the pad's
shared encoders from `firmware/src/core` but not its core). The pad needs
our firmware too: it talks to the receiver over its own link protocol.

## Using it

- **Pair**: on the pad, switch the link to the receiver (**Home + M** held
  3 s; the pad restarts on its receiver identity), then pairing mode
  (Home + Capture held 3 s). On the receiver, a short press of its button
  (or the log's `bt pair`): it searches ~10 s and takes the first pad named
  "C2 pad (…", replacing its old pairing. A pad on its normal Bluetooth
  identity is never taken, so a PC and the receiver each keep their
  pairing.
- **Reconnect**: the receiver pages its paired pad at start-up and every
  5 s while not connected, and stays connectable for the pad's own page.
- **LED** (PA09): slow blink idle, solid connected, fast blink pairing; dark
  after 30 s with no pad (still reconnecting).
- **On USB**, by what is connected:

  | | USB ID | Interfaces |
  |---|---|---|
  | no pad | `1209:0001` "C2 receiver" | debug log + a HID interface with only the vendor collection: not a controller |
  | pad in Steam mode | `2E8A:10C6` "GameSir C2 receiver" | SInput gamepad (+ the vendor collection) + debug log |
  | pad in Xbox mode | `3537:100B` "Xbox 360 Controller for Windows" | XInput (stock's receiver ID and Microsoft OS descriptors) + debug log |
  | pad in DS4 mode | `054C:09CC` "Sony Interactive Entertainment Wireless Controller" | a DualShock 4 as the pad is on its cable (`docs/ds4.md`; + the vendor collection, its `0F 17 55 88` also on the OUT endpoint) + debug log |

  The mode is saved, so the receiver comes up as the pad's last mode before
  the pad connects. A mode change re-enumerates.
- **Configuration** goes through it in every mode: SInput reports in Steam
  mode, the log port in any mode (`docs/config-protocol.md`).

## Design

**USB** (`dg_usb.c`). The pad's reports go onto the interrupt endpoint as
they arrive (with interrupts off, so there is one writer; a 2 ms timer
sends leftovers). A packet uncollected for 50 ms is flushed. The host's
SInput commands (SET_REPORT on EP0) go back to the pad; in Xbox mode the
host's rumble (`00 08`) and LED ring (`01 03`) become SInput haptics and the
player number. Linux's battery report 0x04 is built from the pad's status
message. Nothing is written to the endpoint from inside a USB interrupt (a
features reply written there once left the endpoint dead).

**Bluetooth** (`dg_bt.c`). The public SDK's library works as the pad's host
without changes: to the library the receiver is a HID "device" that pages
its host, and the pad, taking the receiver for its host, opens the HID
channels. The receiver is connectable but not discoverable. `patch_sdk.py`
turns inquiry on (apps/hid builds the library without it). Failed
connections don't go to the SDK's handler: after a Connection Timeout it
paged once more without page scan on and then ignored retries, so the pad
could never reconnect. Now a page with no result in 15 s is cancelled and
the receiver made connectable again; three in a row restart it.

**The link protocol** (`core/gp_link.h`, `dg_link.c`). On the HID interrupt
channel, pad → receiver:

- **STATE** (0x20), every tick, 30 B: sequence, buttons, sticks and
  triggers (16-bit, lossless), IMU, the pad's ms clock. 36 B on the air:
  one 2-DH1 packet, where SInput's report needed 3-DH1.
- **STATUS** (0x21): battery and charging, on a change and once a second.
- **HELLO** (0x22): version, mode, SInput capabilities, serial, tick; at the
  connect, after a mode change, and when the receiver asks (0x30).
- Configuration replies stay SInput 0x02; host commands stay SInput 0x03.

The receiver builds SInput's state report with the pad's own encoder,
answers SDL's features request from the hello, counts lost reports by
sequence (the pad stamps it at send time), and answers a timed ping (host
`03 8E` → hello request → `02 8E`) for `tools/c2lat.py`. An SInput pad
without the link still passes straight through.

**The board.** The button is **PA00** (to ground, pull-up). The LED is PA09
(PA06 held high, as stock). **PA12 is wired to USB D−**: a pull-up there
makes the PC see a low-speed device that never answers (two builds needed
the ROM key). The app runs at **0x1E000C0**: the receiver's container puts
`app.bin` at +0xC0 of its app area (the pad's at +0x120), so
`patch_sdk.py` links it there; linked at the SDK's 0x1E00120 the first
builds crashed before USB (relative branches worked, absolute ones landed
0x60 off).

## A way back to the bootloader, always

The receiver can't be opened without breaking its housing, and the ROM's
USB key only works on its board's D+ / D− pads, not through the plug. So
every receiver build keeps all of these, and they are tested:

1. **The button held at power-up** → bootloader. Checked first thing in
   `setup_arch()`, before the watchdog and clock set-up; needs nothing from
   the firmware past its first instructions.
2. The debug log's **`boot`** command (`jlflash.sh` uses it).
3. **`0F 17 55 88`** on the vendor HID collection, at any time (not in Xbox
   mode, which has no HID interface).
4. **A crash-loop guard**: a counter in no-init RAM (`.dg_noinit`, a NOLOAD
   section `patch_sdk.py` adds; the SDK's `.non_volatile_ram` is zeroed with
   `.bss`), counted at the top of `setup_arch()` and cleared after 10 s of
   uptime: the third boot in a row without that enters the bootloader.
   Breadcrumbs in the same RAM: `sudo fw-re/jlflash.sh crumbs` shows the
   last boots' checkpoints and reset source.

## Building and flashing

`BOARD=dongle firmware/build.sh` → `firmware/build/dongle.flash.bin`: the
**app area alone** (`firmware/mkimage.py`, layout
`firmware/boards/dongle/image.json`), from our app and the SDK's
`cfg_tool.bin`. The receiver boots its own loader from SPI 0x0, whose
`isd_config` (`EOFFSET` 0x10) sends it to the JLFS header at 0x10000, which
puts the app area at 0x11000 (the first 4 KB at 0x0 and at 0x10000 are the
same bytes). Neither is written: the image goes to 0x11000 and must end
before 0x60000, the sector of the installer's record (0x60FF0), so VM (the
pairing), BTIF and the MAC stay too. No `tone` area (JieLi's voice
prompts: unused).

`jlflash.sh write` tells the app area from a whole image by its content,
checks it against the receiver's layout, and before writing reads the
chip's header at 0x10000: it must be `GS_C2_Dongle` and put the app at
0x11000. Older receiver images (a whole installed image at 0x10000, built
on a backup of the chip) are still taken. `jlflash.sh restore FULL.bin`
writes a whole 512 KB backup back
(`sudo IMG_ADDR=0 LEN=0x80000 fw-re/jlflash.sh read backups/c2-receiver-full.bin`
makes one).

**Log commands** (`tools/c2log.py --target receiver`): `stats` (once), `stats
on` / `stats off` (every second: reports in and out, replaced, flushed,
lost, latency), `bt pair`, `bt connect AA:BB:…`, `bt disc`, `bt hid`, `usb`
(the USB state), `clock`, `crash` (tests the guard), `boot`, `reset`,
`cfg <hex>` (configuration, passed to the pad).

## Measurements (2026-11-13)

Through the receiver, same pad and PC (`tools/c2lat.py`, and the
receiver's own µs timing):

| | SInput on the link | our link protocol |
|---|---|---|
| round trip median, p95 | 10.7, 14.8 ms | 10.2, 12.9 ms |
| state reports at the PC | 244/s | 261–263/s |
| report age above the fastest 1 %, median / p95 | 2.0 / 3.4 ms | 1.7 / 3.0 ms |
| bytes on the air | ~16.4 kB/s | ~8.0 kB/s |

A state report goes from Bluetooth onto the USB endpoint in ~0.02 ms; a
host command reaches Bluetooth in 1.6 ms on average. Nothing is lost on the
air. The pad is the main delay (`docs/latency.md`).

## The stock receiver (reference)

- **USB**: `3537:0575` "GameSir-Cyclone 2" (bcdDevice 1.21) with no pad,
  `3537:100B` with one. One HID interface: Consumer volume keys (`0xDD`),
  vendor `0xFF32` (`0x41`), and GameSir Connect's vendor page `0xFFF0`
  (in `0x10` / `0x12`, out `0x0F`). It sends `0x12` 1000 times a second.
  `0F 17 55 88` enters the bootloader only while a stock pad is linked.
- **Firmware**: GameSir Connect ships it (`GS_C2_Dongle_V121_No_Key.ufw`;
  not in git: kept in `fw-re/orig/` locally; `fw-re/ufw.py list|extract`, `fw-re/flash.py`,
  `mkufw.py verify`). Built on JieLi's "HCI_SDK" (not public). The `.ufw`
  holds six flash images with the same app, for chip variants.
- **On the chip**: `isd_config` `EOFFSET` 0x10, so the live image sits at
  0x10000 (0x51000 B; the installer relocated its entries); VM at 0x61000
  (`55 AA AA 55`, settings and the pairing), BTIF 0x7E000, key_mac
  0x7F000. A stale copy of the image's start sits at 0x1000.
- **What it does** (strings): Bluetooth Classic ("2.4G" is a name, not a
  radio): inquiry for the pad, pairing in VM, L2CAP HID channels; USB audio
  (speaker, mic, SBC) for the pad's headset; `auto ps4 mode`, `Game Pair
  Mode`. `fw-re/sdk/sdkmatch.py` names 307 of its functions as SDK ones
  (controller, OS, flash); the host side is GameSir's own.
- Its listing must be disassembled at the real base, 0x1E000C0: wrapped at
  0x1E00120, every absolute target reads 0x60 high.
