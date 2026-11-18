# Firmware: design and findings

The firmware in `firmware/` replaces GameSir's whole app image with one
built on JieLi's public SDK, using the decompiled stock app as the
reference for what the hardware needs. It is ours: compatibility with
GameSir Connect is not a goal (stock's feature set is the reference list:
`docs/gamesir-connect.md`, `docs/stock-features.md`). This page is how it
works and what was learned building it; `firmware/README.md` is the source
layout and the build, `docs/plan.md` the work still open.

## Architecture

**Contexts** (`core/gp_core.c`):

- **Report tick**: the SDK's `usr_timer`, a hardware-timer interrupt. It
  asks for 2 ms (stock's base) and fires about every 4 ms (the timer runs
  late under load). Each tick reads the buttons and sensors, builds the
  report for the active protocol and posts it if the transport is free (at
  most one in flight: a busy tick is skipped, so the next report carries
  the newest state). Durations go by the clock (`gp_ms()`), never by
  counting ticks.
- **Poll**: the app task every 50 ms. Power-off timers, combos, LEDs,
  configuration, settings saves, the transport's safety nets, the status
  line. Anything that writes the store or sends Bluetooth commands runs
  here, never in the tick.
- **Log task**: the USB debug log (`core/gp_log.c`), 1 KB stack: commands
  that do real work hand over to the poll.

The CPU runs at 96 MHz (the SDK HID example's 24 MHz left the tick 2–3 ms
of CPU and starved the idle task, which alone clears the watchdog).

**Transports** (`core/gp_transport.h`: `connected`, `busy`, `post`):

- **Bluetooth Classic HID** (`core/gp_bt.c`): reports go straight to the
  controller as ACL packets, as stock does (below).
- **USB** (`core/gp_usb.c`): a composite of the gamepad (SInput HID, or the
  XInput interface) and the CDC debug log; the report goes onto the
  interrupt endpoint from the tick once the last one has gone.
- **The C2 receiver link** (`core/gp_link.c`): Bluetooth to the receiver,
  with our own compact messages instead of SInput reports
  (`docs/dongle.md`).

**Protocols**: SInput in Steam mode (`docs/sinput.md`), Xbox in Xbox mode:
an Xbox One S pad over Bluetooth (`core/gp_xboxbt.c`), a wired Xbox 360 pad
over USB (`core/gp_xinput*.c`); a DualShock 4 over both in DS4 mode
(`docs/ds4.md`, `core/gp_ds4*.c`); a Switch Pro Controller over both in
Switch mode (`docs/switch.md`, `core/gp_switch*.c`). The configuration
protocol rides on SInput or the log port (`docs/config-protocol.md`).

**Modes, links and identities.** The input mode (Steam / Xbox / DS4 / Switch) and
the link (Bluetooth / receiver) are saved settings. Each combination is its
own Bluetooth device to a host, as stock: Xbox mode flips bit 0 of the
address and keeps its bonds at VM ids 130..150, the receiver link flips bit
1 and keeps them at 160..180 with the name "C2 pad (receiver link)", DS4
mode flips bit 2 and keeps them at 190..210, Switch mode flips bit 3 and
keeps them at 220..240, so every host keeps its pairing. The identity (Device ID, HID record) is set at
start-up: `patch_sdk.py` makes the SDK's records writable, and the SDP
record buffer grew to 768 B (the Xbox descriptor, 334 B, overflowed 512 B
into a boot loop).

**Storage** (`core/gp_store.h`): records by id (settings, calibration). The
default medium is the SDK's syscfg; the C2 keeps them in its I2C EEPROM
(`boards/c2/c2_store.c`: two copies per record, sequence number and CRC, a
power cut mid-write keeps the previous one). On first boot it copies stock's
factory calibration into a record of its own and moves the old syscfg
records over. The EEPROM is ours to use; going back to stock means
restoring `backups/c2-pad-eeprom.bin` too (`docs/stock-features.md`).

**Settings and profiles** (`core/gp_settings.c`): one table, global
settings plus 4 profiles (tuning, remap, turbo), saved 2 s after the last
change; the configuration protocol lists the table, so clients need no
change for a new setting.

## Inputs

- **Buttons**: 20, active low with pull-ups, plus the trigger end-stops on
  the always-on domain's PR pins. Remap and turbo are applied for the host
  only: the pad's own combos read the physical buttons.
- **Sticks** (`drivers/hjc_stick.c`, `drivers/stick_axis.c`): an external
  chip (stock's "hjc723", powered from PA08) streams 4-byte frames over
  UART2 RX (PA03): ID, 15-bit value, CRC-8 (poly 0x31). On the wire it runs
  at ~500000 baud (stock asks its driver for 941176, scaled by its own UART
  clock); the firmware searches a list of rates and locks. A 2 KB DMA ring
  with an interrupt every 128 B (a 64 B ring at 4 B lost bytes under
  Bluetooth load: 10–43 % CRC errors), a sliding-window frame parser, and a
  jump filter (a value > 4096 away needs a second frame). Each report takes
  the mean of every value since the last one (~12 per axis): 5.5 ms median
  from frame to report, against 12.1 ms for stock's mean of 6 reports
  (`docs/latency.md`). Centre learned 1.5 s after data starts (averaged 1 s,
  restarted if the stick moves), slow re-centring near the centre, spans
  learned per direction (a new extreme commits after 200 ms with no stick
  click: a click pushes the magnet past the travel). Stored calibration
  replaces the learning.
- **Triggers** (`drivers/hall_trigger.c`): hall sensors on the ADC (LT
  PB03, RT PB10; ~485 at rest, ~11 fully pressed), an 8-sample mean. The
  rest is learned over 0.5 s, then follows the trigger stoppers: released
  further → that is the rest; held a little in (up to 50 counts) and still
  for 3 s → that is the rest. Dead zone 12 counts (plugging in shifts the
  rest by up to ~7). At an end-stop the trigger is full.
- **IMU** (`drivers/qmi8658.c`): a QST QMI8658 at 0xD4 on hardware IIC
  (SCL PB04, SDA PB06, 400 kHz), stock's set-up (±4 g, ±2048 °/s, 448 Hz).
  The data is little-endian although stock sets the big-endian bit. Now and
  then a read returns all 0xFF (re-read); a jump > 0.7 g / 500 dps is kept
  only if a re-read agrees. Gyro bias (`core/gp_motion.c`): learned in 1.5 s
  windows where gyro and accelerometer are still (the pad resting, not
  held) and gravity doesn't drift; a new bias must be within 1.5 dps of the
  current one, or repeat for 6 s (a slow turn is not bias). This pad's
  bias: 52, −41, −4 raw.
- **Shaping** (`core/gp_tuning.c`), per profile: radial stick dead zone,
  outer zone, anti-dead zone, curve; trigger dead zone, outer zone, hair
  trigger.
- **Calibration** (`core/gp_cal.c`): from the configuration protocol, not
  button combos: rest (centres, rests, gyro bias), then the range.

## Outputs

- **Rumble** (`drivers/erm_rumble.c`, `core/gp_rumble.c`): two ERM motors
  on MCPWM at 25 kHz, big on PA09, small on PB00. As stock: a level 0..4
  picks per motor a duty range and a start kick (M + Up / Down, 1 s of
  rumble at the new level). Xbox Bluetooth commands carry a duration.
- **LEDs** (`drivers/ws2812_spi.c`, `core/gp_leds.c`): the chain in
  `docs/sinput.md` §1. Home: the host's colour when connected; breathing
  (reconnecting) or flashing (pairing) in the mode's colour (Steam yellow,
  Xbox green, DS4 blue, Switch red) when not; a short red flash every 2 s at 5 % battery
  (Switch mode: Home goes dark for it instead). Player
  LEDs: the player number in the link's colour (Bluetooth blue, USB white,
  receiver purple; all four purple while looking for the receiver), the
  profile number for a moment after a change.
- **Battery** (`drivers/battery_adc.c`): PB01 through a 1:2 divider (the
  chip's own VBAT is not the battery). Percent from two tables measured on
  this pad with `tools/c2batlog.py` (an idle discharge of 19 h 35 min and a
  charge of 1 h 53 min; `boards/c2/gamepad_c2.c`); on battery it only falls, on 5 V it only
  rises, so plugging in doesn't jump it. Full = the charger IC's status pin
  PB02 (stock's "4000 mV for 30 s" fired ~65 min early).

## Power

- **On**: Home (PB11). USB 5 V (PC03) or the dock wakes the chip too:
  without Home held that is **charging mode** (stock's animation on the
  player LEDs, colour by charge; Home restarts into normal use; unplugged:
  off).
- **Off** (`core/gp_power.c`): Home held 5 s alone (the LEDs go dark, off
  on release); 60 s in pairing mode or 180 s reconnecting without a host;
  connected and idle for the `idle_off_min` setting (default 10 min; never
  while wired); on the dock; the SDK's low-battery check at 3.3 V. The
  sequence: the link is closed first (a disconnect sent alone and re-sent
  until it is down, else the host kept the link until its timeout), then
  scanning off, the wake-up flags cleared, soft-off. Going off with the link
  up woke the chip straight back.
- **Charging dock** (stock's "pedestal"): PA01 senses the dock's 5 V
  (above 4000 mV after 20 readings), PB05 switches its power. Stepping onto
  the dock turns the pad off into charging mode; a pad started on the dock
  stays on.
- **Low battery**: Home flashes red at 5 % (about an hour idle before the
  SDK's 3.3 V power-off).

## Bluetooth: what it took

- **Reports straight to the controller.** The library's path
  (`edr_hid_data_send` → command ring → btstack task → L2CAP) held each
  report until its send-done 10–12 ms later: ~110 reports/s. Stock builds
  the ACL packet itself; so does the firmware now
  (`hci_get_outgoing_acl_packet_buffer`, `bredr_hci_send_acl_packet`, with
  the interrupt channel's handle and remote CID): ~290 reports/s.
- **The command ring has no lock.** `user_send_cmd_prepare` stores tail + 1
  before the command and wraps it after; btstack preempting the app task in
  between spun forever (stalls of 14 s, 19 s, or a watchdog reset). Sends
  run with the scheduler suspended. Kept as safety nets: a stall detector
  in the tick that feeds the watchdog for up to 30 s, a send stuck 250 ms
  clears the busy flag, sends failing for 1 s drop the link.
- **The HID send buffer**: the library refuses reports longer than its DIY
  buffer (64 B; an SInput report is 65, DS4's 79 with the header):
  `hid_diy_set_sendbuf_size(2 + 77)`.
- **GET_REPORT** on the control channel (DS4's feature reports): the
  library asks `hid_ctrl_data_parse_hook` first; `patch_sdk.py` has it ask
  the firmware.
- **No device-initiated sniff** (`SNIFF_ENABLE 0`): one report per 10 ms
  filled the queue.
- **Reconnect**: the library opens the HID control channel before the link
  is encrypted, BlueZ refuses it ("security block") and the library never
  retries. The firmware tracks the ACL and asks for the HID connection
  again after 1.5 s (3 times, then drops the link).
- **The USB log mustn't block**: with the cable in and nothing reading the
  port, `cdc_write_data` waited forever and starved the rest (~60
  reports/s). The log writes only into a free endpoint and drops its oldest
  lines when full.

## The boot container and the SDK

[Jieli-Tech/fw-AC63_BT_SDK](https://github.com/Jieli-Tech/fw-AC63_BT_SDK)
(Apache-2.0), pinned at `firmware/sdk/SDK_COMMIT`. It has a `cpu/br23` port
(the C2's chip; GameSir built on its `board_ac695x_demo.c`). Drivers and
boards are C; the Bluetooth stack, the OS and low-level CPU code are
prebuilt LLVM bitcode (`cpu/br23/liba/*.a`, linked with LTO). The SDK's
post-build `download.sh` is skipped: it hands the files to JieLi's
internal download tool, and its packer `cpu/br23/tools/isd_download` needs
OpenSSL 1.0. `firmware/pack.sh` packs instead, with `firmware/mkimage.py`,
from our app and the SDK's own `uboot.boot` and `cfg_tool.bin`; no stock
image goes in. The layouts: `firmware/boards/<board>/image.json`.

**The pad** (`kind: full`): the whole container, written at SPI 0x1000.

| Entry | Offset | Size | Notes |
|---|---|---|---|
| JLFS header | `0` | 32 B | name `GS_C2_ADC_DEVICE`, version `3.56` (the pad's stock fields, kept), flash size − 4 KB |
| `uboot.boot` | `0xC0` | 8168 B | the SDK's file: its 16-byte header and its body ENC'd apart |
| `isd_config.ini` | `0x20A8` | 0x85 | the chip-key block (key `0xFFFF`), then the settings |
| slot A directory header | `0x3000` | 32 B | CRC16 of the app area decrypted and encrypted, its size |
| `app_dir_head` (slot A) | `0x3020` | to 0x7C000 | the app area: `app_area_head`, `app.bin` at +0x120 (= XIP `0x1E00120`), `cfg_tool.bin`, VM / PRCT / BTIF |
| `app_dir_head2` (slot B) | `0x7C020` | 0x7C000 | second bank, outside the 0x77000 image `jlflash.sh` writes |
| `key_mac` | `0xFF000` | 0x1000 | |

- The pad's stock v19 image differed from this only in `uboot.boot`'s
  build time (a constant its first instructions load and overwrite: two
  builds of the same source, 2021-12-14, 11 h apart; both its CRCs follow)
  and in its slot A directory header (its own app's values). Checked byte
  for byte, 2026-11-17.
- `isd_config.ini`: debug UART on PB04 (the IMU's SCL: unused), `EOFFSET =
  1`, VLVD 4. Each setting: value length, key, NUL, value (strings, or u32).
- Slot A can grow up to slot B: 495 KB for `app.bin` and `cfg_tool.bin`.
- The area entries' name fields carry an attribute in their last 4 bytes
  (VM its size word, PRCT `CODE`, BTIF `AUTO`).
- A JLFS header is told from an app area by its CRC *and* its format word
  `0x0220`: an app area's first entry is SFC'd with the keystream the
  header's ENC uses, so its CRC holds as well.

**The receiver** (`kind: app`): the app area alone, written at SPI 0x11000
(`docs/dongle.md`, "Building and flashing").

## Reverse-engineering tools

- `fw-re/sdk/sdkmatch.py`: names stock functions by matching them against
  an SDK build (constants and branch offsets masked, then along calls): 662
  stock functions named in v3.56. Low coverage is
  expected: only GameSir's own part needs decompiling.
- `fw-re/sdk/sfrscan.py` (every peripheral-register access),
  `pinscan.py` (GameSir's inline pin set-up), `callsites.py` (calls with
  constant arguments). `0x2000000 + x` in stock is an alias of RAM `x`
  (the boot-RAM block's own code), which the tools resolve.
- The pin map with the stock address and a confidence level for every
  entry: `firmware/boards/c2/c2_pins.h`; power and wake-up:
  `firmware/boards/c2/c2_power.h`.

## Stock's calibration (for reference)

Started on the pad: Home + Share + Options held 4 s (sticks and triggers)
or Capture + M held 4 s (gyro); GameSir Connect sends `0F FD m mask` for
the same. A sticks pass counts 4 full turns near full deflection and keeps
8 radii per stick (each ≥ 8192, else "Stick Err"); a trigger range under
385 counts fails. Stored as a 66-byte record ending `A6 6A` (flash store,
`0x1E37BFC`), and as factory data in the EEPROM (`docs/stock-features.md`).
The firmware copies the factory record at first boot and has its own
calibration over the configuration protocol.

## Safety

- **Back up first**: the whole chip
  (`sudo IMG_ADDR=0 LEN=0x100000 fw-re/jlflash.sh read c2_full.bin`) and
  the EEPROM (`tools/c2eeprom.py`).
- **Ways back to the bootloader**, in order: the log's `boot` (what
  `jlflash.sh` uses), Home + Options with USB (at power-on, or held 2 s),
  the config protocol's BOOTLOADER, and with no working firmware the ROM's
  USB key (`tools/usbkey-mega2560/`). The bootloader's own write is
  verified before the reset, so a bad transfer can't brick; a valid image
  that crashes before USB can.
- **Lessons that cost a recovery**: the first from-scratch attempt (before
  the SDK) bricked the pad on a mistranscribed board init: every pin is
  cross-checked against the stock disassembly now. A tick on JL_TIMER3 hung
  the pad at boot with no watchdog reset (JL_TIMER PRD is 16 bits): don't
  retry timer experiments casually. Never flash the SDK demo's board
  unchanged: its pins are JieLi's demo board's.
