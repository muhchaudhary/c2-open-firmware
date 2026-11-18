# SInput: the protocol as the pad speaks it

In Steam mode the pad is an **SInput** gamepad: `2E8A:10C6`, named
**"Cyclone 2 SI"**, driven by SDL's `SDL_hidapi_sinput.c` (SDL 3.4) and by
Steam through SDL. The same reports go over Bluetooth, USB and the C2
receiver (which rebuilds them from its link, `docs/dongle.md`). Encoder:
`firmware/src/core/gp_sinput_desc.c`; commands: `gp_sinput.c`.

SDL matches VID/PID only, sends the features request and drops the device
if no reply arrives within ~100 ms (§2.7).

## 1. The LED chain

From the stock code, confirmed on hardware: the 8 slots form one chain of
WS2812-style 24-bit pixels on SPI1 (PC05), sent as G R B. Slot 0 Home, 1..4
the player LEDs (top to bottom), 5 left grip, 6 right grip, 7 the charging
dock's LED (the chain runs on through the dock's contacts). The whole chain
is powered from PB09 ("extend power"): a frame sent right after PB09 goes
on is lost. The firmware drives it with the SDK's `led_spi.c` bit patterns
(8 MHz, `0x7C` / `0x60`, every bit starts low); stock's own (`0xFC` /
`0xC0`) showed Home one frame late. `firmware/src/drivers/ws2812_spi.c`.

## 2. Reports

| Report | Dir | Size incl. ID | Use |
|---|---|---|---|
| `0x01` | in | 64 B | state, every report tick (~4 ms) |
| `0x02` | in | 64 B | replies: features (`02 02 …`, §2.4), configuration (`02 C0 …`, `docs/config-protocol.md`) |
| `0x03` | out | 48 B | commands (§2.5) |
| `0x04` | in | 64 B | battery for Linux (§2.6) |

### 2.1 State (input `0x01`)

| Byte | Field |
|---|---|
| 1 | plug: 2 charging, 3 full, 4 on battery |
| 2 | battery % |
| 3 | bit 0 A, 1 B, 2 X, 3 Y, 4 Up, 5 Down, 6 Left, 7 Right |
| 4 | bit 0 L3, 1 R3, 2 LB, 3 RB, (4, 5 digital LT / RT: 0), 6 left paddle, 7 right paddle |
| 5 | bit 0 Start, 1 Back, 2 Guide (Home), 3 Capture |
| 6 | bit 0: charging (the descriptor marks it constant, §2.6) |
| 7..14 | LX LY RX RY, int16, −32767..32767, +Y = down |
| 15..18 | LT RT, int16, −32768 released .. 32767 full |
| 19..22 | IMU timestamp, u32 µs (§2.2) |
| 23..34 | accel X Y Z, gyro X Y Z, int16 (§2.2) |
| 35..63 | 0 |

The bits are `GP_BTN_*` in `core/gamepad.h`, bit for bit. Face buttons:
`SDL_hidapi_sinput.c` labels bit 0 East, but SDL's mapping assigns the
first four buttons South, East, West, North, so bit 0 = A … bit 3 = Y.

### 2.2 IMU and timestamp

Ranges: accel ±4 g (8192 LSB per g), gyro ±2048 °/s (16 LSB per °/s). On
the C2 (QMI8658): SInput X = −sensor Y, Y = −sensor X, Z = sensor Z for
both sensors (stock's DS4 frame and SDL's SInput driver agree), saturating
negation. SDL turns it into its frame as (−X, Z, −Y); DS4 mode sends it
that way (`docs/ds4.md`). The gyro has its bias removed (`core/gp_motion.c`); the
accelerometer is as measured.

SDL integrates the gyro on the timestamp, so it follows real time: the sum
of the system millisecond clock's steps, in µs, whether a report went out
or not. (The tick itself wanders with load.) 90° of rotation reads 90°.

### 2.3 Descriptor (158 B)

`sinput_report_desc` in `gp_sinput_desc.c`:

```
05 01 09 05 a1 01            Generic Desktop / Gamepad / Application
85 01                        -- input 0x01 (63 data bytes) --
06 00 ff 09 20 15 00 25 64 75 08 95 01 81 02        [1] plug (vendor)
05 06 09 20 81 03                                   [2] battery %: Battery Strength, constant (§2.6)
05 09 19 01 29 18 25 01 75 01 95 18 81 02           [3..5] 24 buttons
05 85 09 44 95 01 81 03                             [6] bit 0: Battery System / Charging, constant
95 07 81 03                                         [6] bits 1..7
05 01 09 30 09 31 09 33 09 34 09 32 09 35
16 00 80 26 ff 7f 75 10 95 06 81 02                 LX LY RX RY LT RT, int16
95 02 81 03                                         timestamp (constant field)
09 40 09 41 09 42 09 43 09 44 09 45 95 06 81 02     accel, gyro
75 08 95 1d 81 03                                   reserved (29 B)
85 02 95 3f 81 03            -- input 0x02 (63 B, constant) --
85 03 95 2f 91 03            -- output 0x03 (47 B, constant) --
85 04                        -- input 0x04 (63 B): Linux's battery --
05 06 09 20 15 00 25 64 75 08 95 01 81 02           [1] Battery Strength 0..100
05 85 09 44 25 01 75 01 95 01 81 02                 [2] bit 0: Battery System / Charging
95 07 81 03 75 08 95 3d 81 03                       [2] bits 1..7, [3..63]
c0                           End Collection
```

SDL ignores the descriptor; it exists for the kernel (hid-generic sees a
gamepad, and the battery, §2.6). Reports 2 and 3 are constant fields:
hidraw reads and writes them by size. The host tests parse it and check
the report sizes (`firmware/test/test_sinput.c`).

### 2.4 Features reply (input `0x02`)

| Byte | Value | Meaning |
|---|---|---|
| 1 | `02` | command: features |
| 2, 3 | `01 00` | protocol version 1 |
| 4 | `0xFF` | rumble, player LEDs, accel, gyro, both sticks, both analog triggers |
| 5 | `0x02` | joystick RGB |
| 6 | `3` | `SDL_GAMEPAD_TYPE_XBOXONE` (Steam shows the battery only for type 1, standard) |
| 7 | `0x20` | face style ABXY, sub-product 0 |
| 8, 9 | `3000` | polling interval, µs (nominal: `GP_TICK_MS`) |
| 10, 11 | `4` | accel range, g |
| 12, 13 | `2048` | gyro range, °/s |
| 14..17 | `FF CF 0F 00` | button masks: ABXY + d-pad; L3 R3 LB RB + paddles; Start Back Guide Capture; none |
| 20..25 | | the Bluetooth address (SDL shows it as the serial) |

The values come from `gp_board_info.sinput` (`boards/c2/gamepad_c2.c`).

### 2.5 Commands (output `0x03`)

| Command | Effect |
|---|---|
| `03 01 02 La Lb Ra Rb` | haptics type 2: left amplitude → the big motor, right → the small one (other types ignored) |
| `03 02` | features request: the reply goes out on the next free tick |
| `03 03 n` | player number 1..4 (0: unassigned, shown as 1) on the player LEDs |
| `03 04 R G B` | one colour on Home and the grips |
| `03 80 R0 G0 B0 … R7 G7 B7 F` | ours: a colour per slot 0..7 (§1), then a flag. `F` ≠ 0: the player LEDs show the host's colours too; `F` = 0: they keep the player number |
| `03 C0 …` | the configuration protocol (`docs/config-protocol.md`) |
| `03 8E` | through the C2 receiver only: a timed ping (`tools/c2lat.py`) |

Commands are handled where the report arrives (any context); flash writes
and Bluetooth commands they cause run in the poll.

### 2.6 Battery for Linux (input `0x04`)

Linux calls `power_supply_changed()` on every report carrying the Charging
usage (`hidinput_update_battery`): in report 0x01 that was ~250 uevents a
second, more than udev handles; its queue grew without end and new devices
waited behind it (2026-11-13, through the receiver). So report 0x01's
Battery Strength and Charging are constant (`81 03`: Linux ignores them,
SDL reads the raw bytes 1 and 2 as before), and report 0x04 carries both,
sent when the percent or the 5 V state changes and once a minute: a few
uevents an hour, at `/sys/class/power_supply/hid-<mac>-battery-4`.
Charging there means "on 5 V", full or not: Linux's HID battery has no
Full. SDL and Steam read report 0x01 only.

### 2.7 Unasked features reply

SDL (`RetrieveSDLFeatures`, 3.4.18) writes the request once, then reads at
most 100 reports with `SDL_Delay(1)` between them, about 100 ms; with no
`02 02` among them it drops SInput and the pad falls back to the kernel's
generic device (guessed mapping, no d-pad, no gyro) until it reconnects.
Over Bluetooth the request can reach the pad late (the host busy, the link
slow), so the pad also sends the reply unasked: every 5 ticks for the
first 10 s of a connection (SDL opens the pad right at the connect, while
the transmit pool is often full), then every 40 ms. SDL ignores report 2
outside its init; the cost is one state report per 40 ms. A reply that
finds the pool full is retried. A pending reply (or configuration reply)
is never replaced by a state report.

## 3. Host behaviour (SDL, Steam)

- **Steam** (desktop) needed the client beta for SInput, and lists the pad
  as an Xbox One controller. It ignores SDL's serial. It opens the pad
  twice at start, ~7 s apart, each time SDL's init: `03 02` then `03 03 01`
  (player 1). Colours come in pairs a few µs apart, so only the second
  sticks. Reordering controllers sends no player command, nor does its
  "Player slot LED" setting: under Steam the player LEDs stay at 1. Steam
  Input also makes a virtual "Microsoft X-Box 360 pad 0" (28DE:11FF) from
  the pad, which browser gamepad testers show as a second controller.
- **SDL**: `SDL_SetGamepadPlayerIndex` 0..3 lights 1..4 player LEDs. While
  `03 80` with `F` ≠ 0 owns the player LEDs, a `03 03` is kept and shows
  once `03 04` or `03 80` with `F` = 0 hands them back.
- **BlueZ** caches the SDP record and descriptor: after a change to either,
  `bluetoothctl remove <mac>` and pair again.
- Watching the host's commands live (BlueZ 5.87 prefixes outgoing lines
  with the process):

  ```sh
  sudo stdbuf -oL btmon | awk '/< ACL:/ {tx=1; hdr=$0; next} /^[^ ]/ {tx=0} tx && /^        [0-9a-f][0-9a-f] / {print hdr; print; fflush(); tx=0}'
  ```
