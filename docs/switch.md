# Switch mode

In Switch mode the pad is a **Nintendo Switch Pro Controller** (`057E:2009`,
named "Pro Controller") over Bluetooth and USB: for the Switch itself
(Switch 1 and 2 take a Pro Controller), Linux's `hid-nintendo`, SDL's
Switch driver (Steam). Switch to it with **Home + Y** held 3 s, stock's combo (the pad
restarts; Home is **red** in this mode). Protocol, descriptors and SPI page:
`firmware/src/core/gp_switch_desc.c`; host tests:
`firmware/test/test_switch.c`.

A Pro Controller signs nothing, so unlike a PS4 there is no chip to
emulate: the console takes any pad that answers its subcommands and SPI
reads as a real one does.

Status (2026-11-18): tested on Linux over Bluetooth, USB and the receiver
(SDL `testcontroller`: inputs, low / high rumble on the big / small motor;
the gyro and accelerometer's directions as SDL sees them, checked by
tilting the pad). Not yet: a Switch console.

## What hosts get

| Pad | Pro Controller |
|---|---|
| A B X Y | B A Y X **by position** (the C2's bottom button is the Switch's bottom one, B), as DS4 mode |
| LB RB | L R |
| LT RT | ZL ZR (digital: past a quarter of the travel, after the profile's shaping) |
| Back, Start, Home, Capture | Minus, Plus, Home, Capture |
| L3 R3 | the stick clicks |
| Paddles | none: remap them (`tools/c2cfg.html`) |
| Sticks | 12 bits, centre 0x800, ±0x7FE (the SPI page's calibration says so: full deflection is full) |
| Gyro, accelerometer | the Switch's frame (x forward, y left, z up): SInput's y, x, z. Accel 4096 LSB/g (SInput's halved), gyro 16 LSB/°/s (SInput's own: the SPI page's gyro coefficient is 14976 = 936 × 16) |
| Battery | the level nibble (8 / 6 / 4 / 2 / 0), the charging bit, "powered" on 5 V |
| HD rumble | by band, not side: the left actuator's low band → the big motor, the right actuator's high band → the small one; an actuator alone (the other silent) → its own motor, at its stronger band. So SDL's low / high (sent as the bands, the same on both sides) are the big / small motor, as in the other modes, and `hid-nintendo`'s strong / weak (left / right) too. Amplitudes by SDL's table; frequencies dropped: ERMs have none. (Mapping by side first: SDL's high ran the big motor too, 2026-11-18) |
| Player lights | the player LEDs (patterns 1, 3, 7, F → players 1..4; one LED alone too) |

## Identity

- **Bluetooth**: its own device to hosts, as the other modes: the address
  with bit 3 of the low byte flipped, bonds at VM ids 220..240, Device ID
  `057E:2009` v2.00, name "Pro Controller", class of device 0x002508 (the
  SDK's gamepad class, a real one's). Pair it separately (Home + Capture
  held 3 s in Switch mode). On a Switch: *Controllers → Change Grip/Order*,
  then pairing mode on the pad.
- **USB** (Home + Capture tapped with the cable in): `057E:2009`,
  "Nintendo Co., Ltd." "Pro Controller", the HID interface (a real one's
  203-byte report descriptor, interrupt IN and a 64-byte interrupt OUT on
  endpoint 2) plus the debug log. Bluetooth goes down first, as in DS4
  mode (one address on both). On a Switch, *Pro Controller Wired
  Communication* must be on.
- **The C2 receiver**: the same Pro Controller on its USB (`057E:2009`,
  bcdDevice 0x0210 so `tools/c2log.py` tells it from the pad's cable;
  `boards/dongle/dg_usb.c`). The protocol runs on the receiver: it answers
  the `0x80` handshake and the subcommands itself (device info with the
  pad's address from its hello, the SPI page, ...), and builds the 0x30
  reports from the pad's link state, IMU included, every 8 ms. Rumble and
  player lights go to the pad as SInput commands (`01 02` haptics, `03`
  player). Only the Pro Controller's report descriptor, no vendor
  collection (the bootloader: the log's `boot`, the receiver's button).
- **Configuration**: the log port only (`docs/config-protocol.md`).

## The protocol

Reports, IDs first:

| Report | Dir | Length after the ID | |
|---|---|---|---|
| `0x3F` | in, Bluetooth | 11 | plain HID (16 buttons, hat, 4 × u16 axes), until the host picks 0x30; on a change and every 500 ms |
| `0x30` | in | 49 (BT) / 63 (USB) | timer, battery, buttons, sticks, vibrator byte, 3 IMU samples (newest first): every 15 ms on Bluetooth, 8 ms on USB, as a real one |
| `0x21` | in | 49 / 63 | the same 12 bytes, then a subcommand's ack, its ID, 35 bytes of reply |
| `0x81` | in, USB | 63 | a `0x80` command's reply |
| `0x01` | out | | rumble (8 B), then a subcommand and its arguments |
| `0x10` | out | | rumble alone |
| `0x80` | out, USB | | `01` status (type, address LSB first), `02` handshake, `03` 3 Mbit, `04` reports start, `05` / `06` stop; `92`: a UART-wrapped `0x01` |

Subcommands answered: `01` pairing (acked; the USB pairing's address and
key exchange isn't done), `02` device info (firmware 4.33, Pro Controller,
the address, colours from SPI), `03` report mode, `04` trigger times
(zeros), `10` SPI read, `11` / `12` SPI write / erase (acked, not kept), `21`
NFC/IR MCU configuration (stock's reply, `01 00 FF 00 03 00 05 01`, CRC-8
0x5C), `30` player lights, `40` IMU on / off (off: the samples are zeros),
`50` voltage; anything else (`06`, `08`, `22`, `38`, `41`, `48`, ...) a bare
ack. A reply goes out on the next tick, ahead of the state; one waiting on
Bluetooth is never replaced by a state report.

**The SPI page** (0x6000..0x60A9) is stock v3.56's emulated one
(`fw-re/app_v356.bin` at 0x640D0): no serial, type 3, colours in SPI;
stick calibration centre 0x800 ± 0x7FE; body black, buttons white; stick
parameters with a **dead zone of 0** (the pad applies its own,
`core/gp_tuning.c`). Changed: the IMU calibration (above). Everything else,
including the user calibration at 0x8010 / 0x8026, reads 0xFF: hosts take
the factory one. Stock's Bluetooth and USB report descriptors are byte for
byte a real one's, and the firmware's are the same.

## Future work

Roughly in order: what a console needs first, then what stock has, then
polish. Stock's strings for each: `docs/stock-features.md`, "Nintendo
Switch and Switch 2".

**On a console (not tried yet)**

- **A Switch 1 and a Switch 2.** Pairing (*Change Grip/Order*),
  reconnecting after the console sleeps, *Test Input Devices*, *Calibrate
  Control Sticks*, motion, rumble, each over Bluetooth, the cable and the
  receiver. Everything so far is tested on Linux only. Things a console
  might mind and Linux doesn't:
  - the 15 ms report rate on Bluetooth;
  - the composite USB device: the debug log's CDC interface next to the
    gamepad. If the console refuses it, drop the log in this mode;
  - the receiver's bcdDevice 0x0210 (a real one: 0x0200);
  - the made-up firmware version (4.33) in device info;
  - SPI reads past stock's page answering FF.
- **USB pairing** (subcommand `01`): plugged in, the console sends its
  Bluetooth address and a link key so the pad can reconnect wirelessly
  after. Ours only acks it. Stock stores both ("set switch bt addr",
  "write bt switch link key"; VM 2 and 8). Needs: the steps' reply formats,
  then the address and key written into Switch mode's bonds (VM 220..240).
  It decides whether a pad first plugged into a console reconnects to it
  over Bluetooth.
- **Waking a sleeping Switch 1** with Home: the pad pages the console as on
  any reconnect. Stock has its own path ("switch wakeup send l2cap hid
  connect req", VM 19 `SwitchWakeupConnectFlagBuf`). To be checked on a
  console first.
- **Switch 2 wake-up.** Stock "studies" the console's BLE advertisement
  (scanned, filtered by company ID, saved), then replays it to wake a
  sleeping Switch 2 and connects over Classic. Needs BLE, which this
  build has off (`apps/hid`), the advertisement's format (stock's
  `rx switch2 host adv report:report_type,company_id,len`), and room for
  it in the store.

**Stock features still missing**

- **ABXY by label**: stock's "ABXY swap in Switch", M + A held 2 s in its
  manual. Ours is by position only; a setting (per profile, or per mode)
  and the combo (`docs/plan.md` 4.4).
- **Keeping the console's stick calibration**: SPI writes (subcommand
  `11`) are acked and dropped, so *Calibrate Control Sticks* doesn't last.
  Keep the user calibration (sticks 0x8010..0x8025, IMU 0x8026..0x8039) in the pad's store and answer
  reads from it.
- **The USB "enc" mode** stock has (`USB_SWITCH ENC`, `switch enc mode`):
  an encrypted wired handshake, not decoded. Possibly for the console's
  wired licensed-pad path; to look at only if the plain Pro Controller on
  the cable isn't enough on a console.

**Polish**

- **HD rumble** loses its frequencies (the ERMs have none) and the
  per-side split when both sides rumble. Encodings other than the one SDL
  and `hid-nintendo` send are untested.
- **Digital ZL / ZR** at a quarter of the travel, fixed: a setting.
- **Home's LED** pattern (subcommand `38`) is acked and ignored: Home
  stays red. **IMU sensitivity** (`41`) is ignored: the ranges are fixed.
- **A serial number** at SPI 0x6000 (FF now) and **the pad's real
  colours** at 0x6050 (black body, white buttons, from stock).
- **Paddles**: no Pro Controller button, so dead unless remapped; a
  per-mode default map (`docs/plan.md` 4.1).
- **NFC / IR** (amiibo): none on the C2. Subcommands `21` / `22` are
  answered so the console doesn't stall, nothing more.

## Testing it

- Linux: `dmesg` shows `nintendo ... controller type = Pro Controller`;
  `evtest` lists the pad and its IMU device; the player LEDs light.
- SDL: `testcontroller` (gyro, rumble); Steam lists a Switch Pro
  Controller.
- A Switch: *Change Grip/Order*, pairing mode on the pad; then *Test
  Input Devices* and *Calibrate Control Sticks* (the sticks' centre and
  range), a game with motion controls and rumble.
- BlueZ caches the SDP record: after a firmware change to Switch mode's
  Bluetooth identity, `bluetoothctl remove <mac>` and pair again.
