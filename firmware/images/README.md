# Firmware images

Release images are published as the repository's Releases, from 0.2.2 on.
Each release is first built and tested on the hardware, then committed
here with its `build.sh` sidecar (`C2_pad.flash.bin` + `C2_pad.json`,
`C2_receiver.flash.bin` + `C2_receiver.json`: the release, the commit it
was built from, its sha256). The release workflow rebuilds each image from
that commit and checks it byte for byte before packaging it
(`firmware/README.md`, "Releases"). The images hold only our code and the
JieLi SDK's files (`firmware/mkimage.py`, `LICENSING.md`); the receiver's
is its app area alone, written at SPI 0x11000, while its own loader and
header stay on the chip (`docs/dongle.md`).

| Image | Device | Release | Built from | sha256 |
|---|---|---|---|---|
| `C2_pad.flash.bin` | the Cyclone 2 | 0.3.1 | `caf9be132c` | `750b12f870f2b05a6f07b4d5c3fee9027ed68de2130aa2297f87f214332ef9c8` |
| `C2_receiver.flash.bin` | the C2 receiver: its app area only | 0.3.1 | `cd993b61ae` | `a2aa95957af961f42f5ba78f215ec2b82353278fb2d698082d56f1680acab439` |

Earlier releases (0.2.1 and before) were built before this repository was
published and aren't here. Building from source: `firmware/README.md`.
`firmware/build/` is the scratch output of `firmware/build.sh`.

## Flashing

```sh
sudo fw-re/jlflash.sh write C2_pad.flash.bin
sudo fw-re/jlflash.sh write C2_receiver.flash.bin
```

`jlflash.sh` enters the bootloader on the image's device through our
firmware's debug log and checks the image matches the device. From any
other firmware:

- **The pad**: hold Home + Options and plug in USB (or hold both for 2 s
  with USB in), then `write`. If no firmware runs at all:
  `tools/usbkey-mega2560/`.
- **The receiver**: from our firmware, hold its button while plugging it
  in. From stock firmware there is no way into its bootloader but the ROM
  USB key on its board's D+ / D− pads (the case has to be opened:
  `tools/usbkey-mega2560/`, `docs/dongle.md`).

## Changelog

What each image added, newest first; "tested" means on the pad or receiver.

### The pad (`C2_pad.flash.bin`)

- **0.3.1** (2026-11-24): on the receiver link the pad reconnects by
  itself: while not connected it pages its receiver every 2 s after a
  failed page, until it connects or powers off. (A reconnect the receiver
  started always failed with "PIN or Key Missing", and the pad then
  forgot its pairing.) Tested with two receivers and two pads:
  power-cycled several times each, and a receiver plugged in after its
  pad was on; the release image on both pads (INFO 0.3.1).
- **0.3.0** (2026-11-18): Switch mode (Home + Y held 3 s, Home red): a
  Switch Pro Controller `057E:2009` over Bluetooth ("Pro Controller", its
  own address and pairings) and USB ("Nintendo Co., Ltd. Pro Controller")
  (`docs/switch.md`). The USB handshake, subcommands, stock's SPI page,
  the 0x30 state with the IMU, HD rumble by band (low: the big motor,
  high: the small one), player lights. The mode combos and colours now
  match GameSir's manual: Home + Y Switch, Home + A Steam (Home yellow,
  was white), Home + X Xbox, Home + B DS4. In Switch mode the low-battery
  warning is a dark blink on Home. Tested on Linux over Bluetooth and
  USB (`hid-nintendo`, SDL `testcontroller`: inputs, low / high rumble,
  the gyro's directions); the release image tested on the pad (the new
  combos and colours, Switch mode over Bluetooth, USB and the receiver,
  INFO 0.3.0). Not yet on a Switch console.
- **0.2.2** (2026-11-18): the first public release, and the first built
  reproducibly (the release workflow rebuilds it byte for byte). With only
  the debug log on USB (charging mode) the pad is now `3537:01AB` "Cyclone 2
  SI debug log", one of its own GameSir IDs, not pid.codes' test ID
  `1209:0001`. The SDK's two build-date log lines print a fixed date. No
  other change from 0.2.1.
- **0.2.1** (2026-11-17): the same firmware as 0.2.0, packed by
  `firmware/mkimage.py` from the SDK's files instead of GameSir's v19
  image: byte-identical to it but for `uboot.boot`'s build time (dead code)
  and the slot A directory header (now this app's). Tested: boots,
  connects, bonds kept; DS4 over the receiver.
- **0.2.0** (2026-11-17): DS4 mode (Home + B held 3 s): a
  DualShock 4 `054C:09CC` over Bluetooth ("Wireless Controller") and USB
  ("Sony Interactive Entertainment Wireless Controller"), Home blue
  (`docs/ds4.md`). Bluetooth feature reports sent past the BT library's
  16-byte control buffer; in DS4 mode Home + Capture takes Bluetooth down
  before USB comes up (one address); the IMU in the DS4's frame. Tested on
  Linux (`hid-playstation`: inputs, rumble, lightbar, IMU, battery; SDL
  "PS4 Controller"; Steam) and Windows 10 (DS4Windows).
- **0.1.0** (2026-11-17): the first numbered release (INFO
  reports it, protocol 3). Durations by the clock, not by counting ticks
  (sticks, triggers, rumble, calibration ran ~30 % long). Gyro bias learned
  only while resting and only near the current bias (a slow turn is no
  longer learned). Sticks: the mean of the chip's stream per report, 12.1 →
  5.5 ms median from the stick to the report, same noise
  (`docs/latency.md`; log commands `stickprobe`, `stickfilter old|new`).
  Configuration over the USB log port in every mode (`docs/config-protocol.md`,
  `tools/c2cfg.html`). Low-battery warning (Home flashes red at 5 %;
  `lowbat test`). A failed calibration save reported (status 6). Full
  deflection stays 32767 with a response curve. Tested: config over HID and
  both log ports, Xbox mode and back, stick latency and noise, the
  low-battery warning, the gyro at rest and slow aiming, the I2C scan.
- **Before 0.1.0** (2026-11-17): settings and calibration in the pad's I2C
  EEPROM (first boot formats it: stock's factory calibration copied, the
  old syscfg records moved over); log commands `i2c`, `eeprom`
  (`tools/c2eeprom.py`), `eetest`, `factory`; battery percent from tables
  measured on this pad, "full" from the charger's status pin, Linux's
  battery in report 0x04.
- **Before 0.1.0** (2026-11-14): a stick clicked while held up or down no
  longer shortens that axis's travel.
- **Before 0.1.0**: the combos as now: Home + Y Steam mode, Home + M the link,
  Home + Capture USB input (tapped) and pairing (held).
- **Before 0.1.0** (2026-11-13): the XInput encoder shared with the receiver;
  wired Xbox mode re-tested.
- **Before 0.1.0**: the receiver link: its own identity and pairings, the link
  protocol (0 reports lost), all four player LEDs purple while searching.
- **Before 0.1.0**: both triggers no longer read released ~1 s apart; no
  Linux uevent per report; reconnects to the receiver by itself.
- **Before 0.1.0**: profiles (4, M + Left / Right), stick and trigger tuning,
  remap and turbo.
- Earlier: calibration (`tools/c2cfg.py cal`); the configuration protocol;
  SDL's features reply at and after the connect; the charging dock;
  Bluetooth at stock's rate (~290 reports/s); USB input on demand; Xbox
  mode over Bluetooth (Windows 10, SDL) and USB (Linux, Windows 10); wired
  SInput; SInput over Bluetooth (buttons, sticks, triggers with stoppers,
  gyro, rumble with levels, battery and charging, LEDs, pairing and
  reconnect, every power-off path, the charging animation).

### The receiver (`C2_receiver.flash.bin`)

- **0.3.1** (2026-11-24): pads reconnect after being switched off. The
  receiver no longer pages its pad (that always failed authentication and
  made the pad forget the pairing); it waits for the pad's own page. Each
  receiver now makes its own Bluetooth address on first start (every
  receiver had the same one, `01:64:7E:5B:00:B2`, so with two receivers a
  pad could reach the wrong one): **pads paired before must pair once
  more.** On Linux the battery shows within a second of the gamepad being
  opened (was up to a minute). Tested with two receivers and two pads;
  the release image on both receivers: each its own address kept across
  restarts, both pads back by themselves in ~9 s, and the battery within
  0.9 s of opening the gamepad in 26 of 26 tries.
- **0.3.0** (2026-11-18): with the pad in Switch mode, a Pro Controller on
  USB (`057E:2009`, bcdDevice 0x0210 to tell it from the pad's cable):
  the receiver answers the handshake and subcommands itself and builds
  the 0x30 reports from the pad's link state; rumble and player lights
  go to the pad. Only the Pro Controller's report descriptor (no vendor
  collection) in this mode. Tested on Linux (SDL `testcontroller`:
  inputs, low / high rumble).
- **0.2.2** (2026-11-18): the first public release, built reproducibly.
  With no pad linked it is now `3537:0575` "C2 receiver" (stock's ID in that
  state), not `1209:0001`; its Bluetooth Device ID too. No other change
  from 0.2.1.
- **0.2.1** (2026-11-17): the app area alone (192,512 B at SPI
  0x11000), no longer a copy of the chip's installed image: its loader at
  0x0, its header at 0x10000 and the installer record stay on the chip;
  the SDK's `cfg_tool.bin`, no `tone` area. Needs `jlflash.sh` from this
  version (older ones take whole images only). Tested: write verified,
  boots, the pad linked without re-pairing, DS4 mode (`hid-playstation`),
  INFO relayed over its log port.
- **0.2.0** (2026-11-17): with the pad in DS4 mode, a DualShock 4
  on USB (`054C:09CC` "Sony Interactive Entertainment Wireless
  Controller"): the pad's state as DS4 reports, the feature reports
  `hid-playstation` reads, an interrupt OUT endpoint (rumble, lightbar to
  the pad); `0F 17 55 88` also on that endpoint. The DS4 mode saved like
  the others. Tested on Linux (`hid-playstation`, SDL "PS4 Controller",
  inputs, rumble, lightbar, IMU, battery, Steam ↔ Xbox ↔ DS4 switching,
  the bootloader by `0F 17 55 88` in DS4 mode) and Windows 10 (DS4Windows,
  ~4 ms average response).
- **0.1.0** (2026-11-17): configuration over its log port,
  relayed to the pad and back, in Steam and Xbox mode (`cfg <hex>`). Tested
  with the pad on 0.1.0: INFO, LIST, SET / GET, the mode switched both
  ways over it, short requests padded.
- **Before 0.1.0** (2026-11-14): no gamepad on USB without a pad
  (`1209:0001` "C2 receiver"); recovery from a reconnect that stopped after
  a Connection Timeout; LED dark after 30 s without a pad.
- **Before 0.1.0** (2026-11-13): follows the pad's mode on USB (Steam SInput
  `2E8A:10C6`, Xbox XInput `3537:100B`), comes up in the saved mode.
  Windows not tested.
- **Before 0.1.0**: pairs only with a pad on its receiver link, decodes the
  link protocol into SInput, answers the features request itself, `stats`
  with lost reports, the `03 8E` ping.
- **Before 0.1.0**: forwards the pad's reports to USB and host commands back;
  reconnects at start-up and every 5 s; pairing on the button; the LED;
  the four bootloader paths; the debug log.
