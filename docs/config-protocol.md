# Configuration protocol

The firmware's own protocol for settings, status and maintenance. Clients:
`tools/c2cfg.py` (the reference) and `tools/c2cfg.html` (in the browser:
WebHID in Steam mode, WebSerial over the log port in any mode; Chrome or
Edge, open the file or serve it on localhost:
`python3 -m http.server -d tools 8765`, then `localhost:8765/c2cfg.html`).
Firmware: `firmware/src/core/gp_config.c`.

## Transport

The protocol rides on the SInput HID reports the pad already has, so it
needs no driver and works over **Bluetooth and USB**, while SDL or Steam
has the pad open:

- **Requests**: output report `0x03` (48 B with the ID), first byte
  `0xC0`. SInput's own commands are `0x01`..`0x04`; `0x80` is our per-LED
  command (`docs/sinput.md` 2.5).
- **Replies**: input report `0x02` (64 B with the ID), first byte `0xC0`.
  SDL reads report 2 only during its init and looks for `02 02` (features)
  there; a `C0` reply in that window is skipped like any other report.

In Xbox, DS4 and Switch mode the pad has no SInput reports: use the log port
(below), or switch to Steam mode (Home + A held 3 s).

On Linux the client opens the hidraw node of `2E8A:10C6` (it has a uaccess
ACL, no root needed) and writes / reads it directly; on Windows and macOS
hidapi does the same.

### Serial: the debug log port (any mode, protocol 3)

The pad on its USB cable and the C2 receiver both have a USB serial port
for their debug log (`tools/c2log.py`), in every mode. The same requests
and replies travel there as text lines:

- **Request**: `cfg <hex>` and a newline: the request bytes below (from
  `0xC0`), as hex. Trailing zero bytes may be left out (not the first three).
- **Reply**: a log line containing `cfg> <hex>`: the reply bytes below
  (from `0xC0`), trailing zeros left out; pad them back to 63. Other log
  lines come in between; a line starts with the uptime.

The receiver passes the request to the pad over its link and prints the
pad's reply; with no pad connected it logs `cmd: cfg: no pad connected`.
Not covered: Xbox, DS4 or Switch mode on Bluetooth straight to the computer (no
USB at all). Only one program can read a serial port: a running `c2log.py` takes
the replies. The log drops its oldest lines when it fills, so resend after
~400 ms as over Bluetooth. Browsers: WebSerial (Chrome, Edge). Linux: the
user needs the `dialout` (or `uucp`) group.

## Framing

Request, after the report ID (47 B):

| Byte | |
|---|---|
| 0 | `0xC0` |
| 1 | seq: any value, echoed in the reply |
| 2 | op |
| 3.. | payload, zero-padded |

Reply, after the report ID (63 B):

| Byte | |
|---|---|
| 0 | `0xC0` |
| 1 | seq of the request |
| 2 | op of the request |
| 3 | status |
| 4.. | payload |

Values are little-endian. One request at a time: wait for the reply (by
seq) before sending the next. Over Bluetooth a request takes up to ~200 ms
to reach the pad (more with Bluetooth audio on the same PC), and a reply
can be lost when the radio is busy: resend after ~400 ms with the same
seq. Every operation is safe to repeat.

Status: `0` ok, `1` unknown op, `2` unknown setting / index, `3` value out
of range, `4` not possible now, `5` request too short, `6` done but not
saved (the store's write failed: in use until power-off; the reply's
payload is as for `0`).

## Operations

| Op | Name | Request payload | Reply payload |
|---|---|---|---|
| `0x00` | INFO | – | see below |
| `0x01` | LIST | index u8 (0 .. count − 1) | key u8, flags u8, min u16, max u16, default u16, value u16, name (24 B, NUL-padded) |
| `0x02` | GET | key u8 | key u8, value u16 |
| `0x03` | SET | key u8, value u16 | key u8, value u16 (as applied) |
| `0x04` | DEFAULTS | – | – |
| `0x05` | REBOOT | – | – (restarts ~0.3 s after the reply) |
| `0x06` | BOOTLOADER | – | – (status 4 without USB 5 V; else the JieLi USB bootloader ~0.3 s after the reply: `fw-re/jlflash.sh write`) |
| `0x10` | CAL_START | parts u8 | parts u8 (see Calibration) |
| `0x11` | CAL_STATUS | – | see Calibration |
| `0x12` | CAL_FINISH | – | short parts u8, stored parts u8 (status 3 when a part is short: keep moving, FINISH again) |
| `0x13` | CAL_CANCEL | – | – |
| `0x14` | CAL_CLEAR | parts u8 | stored parts u8 |
| `0x15` | CAL_GET | – | see Calibration |

INFO reply payload:

| Byte | |
|---|---|
| 0 | protocol version (3; 2 had no status 6, release or serial port; 1 no calibration) |
| 1..16 | firmware version: git commit, `+` = built with changes (NUL-padded) |
| 17..32 | board / Bluetooth name |
| 33 | input mode: 0 Steam, 1 Xbox, 2 DS4, 3 Switch |
| 34 | link: 0 Bluetooth, 1 USB |
| 35..36 | battery mV |
| 37 | battery % |
| 38 | power: 2 charging, 3 full, 4 on battery (as SInput) |
| 39 | number of settings (for LIST) |
| 40 | active profile, 0..3 (protocol 2) |
| 41 | the charger status pin (C2: PB02: 0 charging, 1 done or no 5 V), 0xFF none |
| 42 | bit 0: USB 5 V present; bit 1: on a powered dock |
| 43..45 | release: major, minor, patch (`firmware/VERSION`; protocol 3) |

## Settings

Self-describing: a client lists them (LIST 0 .. count − 1) and shows name,
range and default, so a new setting needs no client change. Keys are
stable; names are for people. A SET is applied at once and saved
2 s after the last change (one write per adjustment, as the button
combos). Flag `0x01`: takes effect after a restart, and the pad restarts
itself after the reply (it disconnects and comes back).

Global settings, plus 4 **profiles** with their own tuning, remap and
turbo (LIST flag `0x02`: GET / SET act on the active profile's). The
active profile is setting 4; on the pad **M + Left / Right** steps through
them, and the player LEDs show the number (purple) for a second.

| Key | Name | Range | Default | |
|---|---|---|---|---|
| 1 | `rumble_level` | 0..4 | 4 | rumble strength, 0 off (also M + Up / Down) |
| 2 | `mode` | 0..3 | 0 | 0 Steam (SInput), 1 Xbox, 2 DS4, 3 Switch; restarts on the Bluetooth link and wired, not on the receiver link (also Home + A / Home + X / Home + B / Home + Y) |
| 3 | `idle_off_min` | 0..60 | 10 | connected and idle: off after this many minutes, 0 never |
| 4 | `profile` | 0..3 | 0 | the active profile |
| 5 | `link` | 0..1 | 0 | 0 Bluetooth, 1 the C2 receiver (its own Bluetooth identity and pairings); restarts (also Home + M held 3 s) |
| `0x10` | `stick_deadzone` | 0..50 % | 4 | per profile: inner dead zone, radial |
| `0x11` | `stick_outer` | 0..30 % | 0 | full output this far before the edge |
| `0x12` | `stick_anti_deadzone` | 0..50 % | 0 | output starts here past the dead zone (for games with their own) |
| `0x13` | `stick_curve` | 0..3 | 0 | 0 linear, 1 ^1.5, 2 ^2, 3 ^3 (finer near the centre) |
| `0x14` | `trigger_deadzone` | 0..50 % | 0 | of the travel (the driver's noise floor stays below it) |
| `0x15` | `trigger_outer` | 0..30 % | 0 | full this far before the end |
| `0x16` | `hair_trigger` | 0..1 | 0 | full once past the dead zone (10 % of the travel at least), else released |
| `0x17` | `turbo_rate` | 1..30 | 10 | presses per second for turbo buttons |
| `0x40` + i | `map_<button>` | 0..18 | i | the button the host gets for button i (18: none) |
| `0x60` + i | `turbo_<button>` | 0..1 | 0 | button i in turbo |

Buttons, by index: 0 a, 1 b, 2 x, 3 y, 4 up, 5 down, 6 left, 7 right,
8 l3, 9 r3, 10 lb, 11 rb, 12 paddle_l, 13 paddle_r, 14 start, 15 back,
16 guide, 17 capture. Remap and turbo change only what the host sees: the
pad's own combos (power, pairing, modes, rumble, profiles) read the
physical buttons. The shaping applies in every mode (SInput, Xbox) and
the defaults are the pad's behaviour before these settings.

Stored as settings record version 4 (`gp_settings.c`; on the C2 in its
EEPROM, `boards/c2/c2_store.c`): the
settings struct itself, so a setting appended later loads as its default
from an older record; records 1-3 still load.

## Calibration

Parts: bit 0 sticks, bit 1 triggers, bit 2 gyro. Without a stored
calibration the pad learns at runtime: stick centres over the first ~2.5 s
(so the sticks must be left alone at boot), trigger rests over 0.5 s, the
gyro bias whenever it lies still. With one, sticks and triggers start from
the stored centres, rests and full travel; the gyro starts from the stored
bias and still follows drift. Stored as the calibration record (`gp_cal.c`).

The procedure (`tools/c2cfg.py cal` guides it):

1. **CAL_START** (parts): phase *rest*. The sticks centred, the triggers
   released, the pad lying still for ~1 s (progress in CAL_STATUS; if
   anything moves, the second starts over and `restarts` counts it). Gives
   the stick centres, trigger rests and gyro bias.
2. Phase *range* (when sticks or triggers are included): roll each stick
   around its edge, press each trigger fully; the furthest travel each way
   is kept. Gyro alone goes straight to *done*.
3. **CAL_FINISH**: every stick direction needs at least 6000 raw counts of
   travel (the C2's full swing is ~10000-12500), each trigger 200 ADC
   counts (full travel ~470). If a part is short: status 3, payload byte 0
   names it, and the procedure goes on. Otherwise stored and used at once.

CAL_STATUS payload: phase u8 (0 idle, 1 rest, 2 range, 3 done), parts u8,
stored parts u8, rest progress % u8, restarts u8, short parts u8; then
per stick axis LX LY RX RY: now, min, centre, max (u16 each, raw); per
trigger LT RT: now, rest, pressed (u16, ADC counts; pressing lowers the
value); gyro bias X Y Z (s16).

CAL_GET payload: stored parts u8; per stick axis: centre, travel up,
travel down (u16); per trigger: rest, travel (u16); gyro bias X Y Z (s16).

## Not yet

Firmware update from a client: BOOTLOADER is the first step, the image
itself still goes through `fw-re/jlflash.sh` (browsers can't reach the
bootloader: it is a USB mass-storage device; `docs/plan.md` 3.4).
