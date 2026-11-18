# GameSir Connect 1.14.2: what it does with a Cyclone 2

Reference only: GameSir Connect compatibility is not a goal of our firmware
(what is still missing: `docs/plan.md`). Read from the Electron app (`GameSir Connect Setup 1.14.2.exe` →
`app-32.7z` → `resources/app.asar` → `dist/electron/main.js`, the C2's
protocol class `gs` and its codec `qa`, command builders `vo`, constants
`wa`). Only what concerns the C2. Nothing here is implemented in the
rewrite yet; it is the list of stock features the app exposes, and the wire
format, for when we do.

## Device entry

`Model:"C2"`, `ProductName:"CYCLONE 2"`, bundled firmware `GS_C2_DEVICE`
v1.2.3 (`packages/GS_C2_DEVICE_V123_Key.ufw`) and `GS_C2_ADC_DEVICE` v3.5.2
(`packages/GS_C2_ADC_DEVICE_V352_Key.ufw`, the hall/ADC variant). Config
interface: vendor usage page `0xFFF0`, VID `0x3537`, PIDs `0x101D` (wired),
`0x102A` (wireless), `0x1053` (ADC, wired), `0x100B` (wireless). A C2 dongle
is a separate device (`GS_C2_Dongle`).

The app offers **no sleep / auto-off setting for the C2** (its other pads:
Never / 1 / 5 / 10 / 20 min, default 10; our firmware defaults to 10).

## Commands (host → pad, report starts `0x0F`)

| Bytes | Meaning |
|---|---|
| `0F 01` / `0F 02 x` | enter / exit profile config |
| `0F 03 p hi lo n data…` | write profile `p` at offset `hi:lo`, `n` ≤ 58 bytes |
| `0F 04 p hi lo n` | read profile `p` (chunks of 58) |
| `0F 07 p` | switch to profile `p` (1–4, Shift = 5) |
| `0F 09` | read firmware version (reply: bytes 4..8 pad, 12..16 dongle) |
| `0F 0B` | read the current profile number |
| `0F 0D [on mode]` | read RGB state / set light status |
| `0F 10 p hi lo n` | refresh a profile range (after the pad reports a change) |
| `0F 16 k s` | macro recording on the back buttons (k: 1 FL1, 2 FR1) |
| `0F 17 55 88` | "quick update" |
| `0F 20 66 55 L R` | rumble test, L / R motor |
| `0F F2 x` | heartbeat |
| `0F FC` | enter firmware upgrade mode |
| `0F FD m` | calibration: the reply reports mode and which of LT, RT, LS, RS, sensor are done |

Profile numbers: 1–4 gamepad profiles, 5 the Shift layer, 32 the light
profile, 48 "current profile number" (in change notices). The pad also
sends input report `0x12` to the app: buttons, sticks, triggers, and from
byte 35 charge state, battery level, light config index, 5 RGB values; byte
53 bits 0/1 macro recording, bits 4/5 FL1/FR1.

## Gamepad profile (680 bytes)

| Offset | Size | Block |
|---|---|---|
| 0 | 32 | name (UTF-8) |
| 32 | 32 | Fun_Data: motor L, R, LT, RT; audio enable, volume, mixer, mic mute, mic sensitivity; Shift enable + key; d-pad diagonal lock; ABXY swap in XInput; ABXY swap in Switch; report-rate gear; reserved |
| 64 | 16 × 7 | buttons Up, Down, Left, Right, L1, R1, L3, R3, A, B, X, Y, Home, Select, Start, Capture: turbo on, turbo speed, remap on, up to 3 target keys, toggle |
| 176 | 2 × 159 | back buttons FL1, FR1: as a button, plus a macro (enable, key-held, loop, loop time, up to 30 steps of key + hold + delay) |
| 494 | 2 × 28 | triggers L2, R2: turbo, dead zones (start, end, anti-start, anti-end), remap, toggle, "quick trigger" (hair trigger: on, start, end), response curve (5 points) |
| 550 | 2 × 32 | sticks L, R: on, square/circle, dead zones (as triggers), response curve (5 points), remap (to d-pad / mouse: flips, axis ratio, DPI, direction targets) |
| 614 | 2 × 33 | motion "Aim" and "Tilt": on, quick key, active axis, dead zones, curve, mapping (as sticks) |

The light profile (635 bytes) holds 5 animations × 8 key-frames (124 bytes
each), 5 RGB zones.

Profiles can be exported as `.profile` files: the JSON of the byte array,
AES-256-CBC with the passphrase `HJC2021`.
