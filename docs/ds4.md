# DS4 mode

In DS4 mode the pad is a **DualShock 4** (2nd revision, `054C:09CC`, named
"Wireless Controller") over Bluetooth and USB, for PCs: Linux's
`hid-playstation`, SDL's PS4 driver (Steam), DS4Windows, and games that
only know PlayStation pads. Switch to it with **Home + B** held 3 s (the
pad restarts; Home breathes blue while it isn't connected). Encoder and
descriptors: `firmware/src/core/gp_ds4_desc.c`; host tests:
`firmware/test/test_ds4.c`.

**Not for a PS4 console.** A PS4 only accepts a pad that signs its
challenge (feature reports 0xF0..0xF2) with a key in Sony's security chip.
Stock does that with a chip; there is none on the C2's I2C bus
(`docs/stock-features.md`). The pad answers no 0xF0..0xF2 report.

Status (2026-11-17): tested on Linux over Bluetooth, USB and the receiver
(`hid-playstation`: inputs, rumble, lightbar, IMU, battery; SDL
`testcontroller` "PS4 Controller"; Steam), and on Windows 10 through the
receiver (DS4Windows: inputs, ~4 ms average response).

## What hosts get

| Pad | DS4 |
|---|---|
| A B X Y | Cross Circle Square Triangle (by position) |
| LB RB, LT RT | L1 R1, L2 R2 (analog, and the button bit off rest) |
| Back, Start, Home | Share, Options, PS |
| Capture | touchpad click (no touchpad: no touches reported) |
| Paddles | none: remap them (`tools/c2cfg.html`) |
| Gyro, accelerometer | the DS4's frame: (−X, Z, −Y) of SInput's (SDL reads SInput's that way and the DS4's as they are); SInput's units (`docs/sinput.md` 2.2) |
| Battery | tenths, the cable bit on 5 V, "full" from the charger |
| Rumble | the host's strong / weak motor → the big / small one |
| Lightbar | Home and the grips (Linux sets it from the player number: 1 blue, 2 red, 3 green, 4 pink) |

A button remapped to L2 / R2 pulls that trigger fully.

## Identity

- **Bluetooth**: its own device to hosts, as Xbox mode: the address with
  bit 2 of the low byte flipped, bonds at VM ids 190..210, Device ID
  `054C:09CC` v1.00, name "Wireless Controller". Pair it separately
  (Home + Capture held 3 s in DS4 mode).
- **USB** (Home + Capture tapped with the cable in): `054C:09CC`, product
  "Wireless Controller", manufacturer "Sony Interactive Entertainment" (a
  real DS4's: hosts and browsers name it by both), the HID interface plus
  the debug log. In DS4 mode Bluetooth goes down before USB comes up:
  `hid-playstation` refuses a second device with the same address
  ("Duplicate device found"). The HID
  interface has an interrupt OUT endpoint (32 B): `hid-playstation` sends
  its rumble and lightbar only there. Its DMA buffer needs 64 more bytes
  in the SDK's USB pool (`patch_sdk.py`).
- **The C2 receiver**: the same DualShock 4 on its USB (`boards/dongle/dg_usb.c`):
  the pad's encoder on the link's state (`ds4_encode_usb`), the feature
  reports with the pad's address from its hello, an interrupt OUT
  endpoint (64 B). The host's rumble and lightbar go to the pad as SInput
  commands (`01 02` haptics, `04` colour), each in its own slot so one
  report's two don't replace each other.
- **Configuration**: the log port only (`docs/config-protocol.md`), as in
  Xbox mode.

## Reports

| Report | Dir | Size with ID | |
|---|---|---|---|
| `0x01` | in, USB | 64 | state, every tick |
| `0x11` | in, Bluetooth | 78 | state, every tick: `C0 00`, the same 32 bytes, touch, CRC-32 |
| `0x05` | out, USB | 32 | rumble, lightbar: interrupt OUT endpoint 2 (hid-playstation), or SET_REPORT (hidraw, SDL, Windows) |
| `0x11` | out, Bluetooth | 78 | the same after two bytes (HID + CRC flags, audio) |
| `0x02` / `0x05` | feature | 37 / 41 | IMU calibration (USB / Bluetooth, the latter with CRC-32) |
| `0x12` | feature, USB | 16 | the pad's Bluetooth address |
| `0xA3` | feature | 49 | build date, hardware and firmware version |

The state's common 32 bytes (as `hid-playstation`'s
`dualshock4_input_report_common`): LX LY RX RY (u8, centre 128, +Y down),
hat and Square Cross Circle Triangle, L1 R1 L2 R2 Share Options L3 R3, PS
and touchpad click and a 6-bit counter, L2 R2 (u8), the IMU timestamp in
16/3 µs units, temperature, gyro X Y Z, accel X Y Z (s16), 5 reserved,
battery (bits 0..3: 0..10, 11 full; bit 4: cable), 2 reserved. Then one
touch report with both contacts up.

The Bluetooth CRC-32 (zlib's) runs over a seed byte and then the report
from its ID: `A1` input, `A2` output, `A3` feature.

**Calibration**: hosts scale the gyro by speed × 2 / (plus − minus) and
the accelerometer by 2 g / (plus − minus). The pad reports ±8640 at
540 °/s and ±8192 for 1 g, no bias: the report's raw values are then
16 LSB per °/s and 8192 per g, SInput's own units (the gyro bias is
removed in the pad, `core/gp_motion.c`).

**Feature reports at the connect**: `hid-playstation` reads 0x12 (USB),
the calibration and 0xA3, and gives up on the pad if one fails or has the
wrong size. Over USB they answer GET_REPORT on EP0 (`core/gp_usb.c`); over
Bluetooth GET_REPORT comes on the HID control channel, where the SDK's
`hid_ctrl_data_parse_hook` asks the firmware first (`patch_sdk.py`,
`gp_bt_get_report` in `core/gp_bt.c`). The library copies a hook's reply
into the connection's 16-byte control buffer and drops a longer one
without a word, so the firmware sends its replies itself
(`l2cap_send_internal` on the control channel).

## Testing it

- Linux: `dmesg` shows `playstation ... DualShock 4` with no "Failed to
  retrieve" errors; `evtest` lists the gamepad, its motion sensors and
  touchpad devices; `/sys/class/power_supply/ps-controller-battery-*`.
- SDL: `testcontroller` (gyro, rumble, lightbar); Steam lists a PS4
  controller.
- BlueZ caches the SDP record: after a firmware change to DS4 mode's
  Bluetooth identity, `bluetoothctl remove <mac>` and pair again.
