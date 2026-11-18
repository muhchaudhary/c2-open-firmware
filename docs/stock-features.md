# Stock firmware features (from strings)

What GameSir's stock firmware does beyond what our firmware does: first as
GameSir's manual documents it for the C2 (the next section, the feature
set to match), then as far as its text strings, USB descriptors and flash
contents show (shared firmware: more than the C2 ships with). The point is to
help implement new features (PS4/PS5, Switch / Switch 2, Xbox 360, headset
audio, EEPROM data, factory tools) in our firmware.

**Confidence.** Everything here comes from strings and data in the images
(2026-11-15). None of it has been traced to code or tested on hardware unless
marked. The stock pad firmware also supports board variants that are not ours
(it has an `lsm6ds3 sensor init start` path; our pad has a QMI8658), so a
string doesn't prove the hardware exists on our C2. Facts marked *(public)*
come from public reverse-engineering work, not from these images.

## What GameSir says the C2 does (its manual)

The firmware's strings are from GameSir's shared code base, so they cover
more than the C2 ships with. What GameSir documents for the Cyclone 2
([manual](https://gamesir.com/support/manuals/gamesir-cyclone2), read
2026-11-18) is the feature set to match. The manual contradicts its own FAQ
in places (noted); combos it doesn't give are marked "not given".

**Platforms.** Windows 10/11 (Bluetooth, the receiver, the cable), Android
8.0+ (Bluetooth), Switch and Switch 2 (Bluetooth; the cable and the receiver
once the Switch's *Pro Controller Wired Communication* is on), Steam. **Not
claimed: PS4 / PS5, Xbox consoles, iOS.** So the PlayStation and Xbox 360
authentication code below is GameSir's shared firmware, not a C2 feature.

| Stock (manual) | Ours | |
|---|---|---|
| **Modes**: XInput (green, Home + X), NS / Switch "Pro Controller" (red, Home + Y), HID / Android (yellow, Home + A, "GameSir-Cyclone 2"); Windows Bluetooth with Home + B (blue, "Wireless Controller") | Steam (yellow, Home + A), Xbox (green, Home + X), DS4 (blue, Home + B, "Wireless Controller"), Switch (red, Home + Y) | The same combos and colours since 0.3.0. Our Home + A is SInput, not stock's HID / Android pad: Android takes our Xbox mode (Xbox One S over Bluetooth). |
| Hold M: show the mode's colour; View + Menu: cycle the modes | Home's colour shows the mode when not connected | no "show mode" or cycle combo |
| **Automatic mode detection** on the cable and the receiver; a mode chosen by hand is kept until a power-off | The saved mode, always | missing |
| Pairing: Home + mode button held 2 s; forced pairing Logo + Share 2 s; Home + Share 2 s after a failed connection | Home + Capture held 3 s (the current mode) | equivalent |
| **Back buttons programmed on the pad**: M + L4 / R4 held, press the target (one button or a combo), L4 / R4 again; saved after 10 s | remap in the configuration app / protocol only | on-pad programming missing; macros missing (the app has them) |
| **Turbo on the pad**: M + button cycles 8 / 12 / 20 Hz / off, on A B X Y LB RB LT RT; double-tap M clears all; Home flashes while turbo is on | per-button turbo and one rate per profile, from the app only; no LT / RT turbo | on-pad combos, per-button rates, trigger turbo and the indication missing |
| **ABXY swap** (A/B and X/Y): M + A held 2 s | by position, no swap | missing (Switch: by label) |
| **Zero dead zone** per stick: Mode + stick click until it rumbles (Raw ↔ circular) | per-profile dead zone from the app | on-pad toggle missing |
| Hair trigger: a combo (not given); Home flashes on LT / RT while on | per-profile setting from the app | on-pad toggle missing |
| Trigger modes: Hall analog, hair trigger, "mouse click"; a long / short travel switch | analog (any position), hair trigger | the C2 has trigger stops; "mouse click" mode missing |
| Rumble: 5 levels, off / 25 / 50 / 75 (default) / 100 %; combo not given | 5 levels (0..4), M + Up / Down | have (default level to compare) |
| 4 presets, combo not given | 4 profiles, M + Left / Right | have |
| **Calibration on the pad**: sticks and triggers View + Menu + Home 2 s, A, turn and press, A; gyro M + Screenshot 3 s (FAQ: 2 s) | from the configuration app / protocol | on-pad combos missing |
| Function reset (defaults): Home + Menu until the logo blinks (or Logo + Menu 5 s) | the configuration protocol's defaults | on-pad combo missing |
| Power: Home 5 s off; auto-off 10 min idle connected, 1 min pairing, 3 min reconnecting | the same (pairing 60 s, reconnecting 180 s, idle 10 min by default, settable) | have |
| Low battery: red flashes twice every 10 s under 15 % | a short red flash every 2 s at 5 % | thresholds differ |
| Charging off: red / orange / yellow / green by charge, green flash then off when full | stock's own animation (`core/gp_charging.c`) | have |
| Dock: charges, its LED follows the pad's | yes | have |
| Lighting from the app (zones, animations) | the host's colour, the mode's colour | missing (plan 5.4) |
| Firmware update from GameSir Connect (Logo + Menu, plugged in) | `jlflash.sh` | missing (plan 3.4) |

## Sources and addresses

| Image | File | app.bin | XIP base |
|---|---|---|---|
| Pad, stock v3.56 | `backups/c2-pad-stock-v356.flash.bin` (from `fw-re/orig/GS_C2_ADC_DEVICE_V356_No_Key.ufw`) | 471,889 B | `0x1E00120` |
| Receiver, stock v1.21 | `backups/dongle-installed.flash.bin` (from the chip backup) | 289,172 B | `0x1E000C0` |

None of these is in git (GameSir's firmware: `fw-re/orig/`, `backups/` are
local copies). Extract with `python3 fw-re/flash.py IMAGE OUTDIR` (writes `app.bin`,
`cfg_tool.bin`, `isd_config.ini.bin`, and `tone.blk` for the receiver). The
string offsets below (`@0x…`) are offsets into that `app.bin`. Add the XIP base
to get the address the code uses, then `fw-re/sdk/callsites.py` (or a search
for the address in the disassembly) finds the code that prints it. Strings
list: `strings -a -t x -n 5 app.bin`.

Build facts: the pad is built from `.../GameSir_Cyclone2_Device_Main_SDK101_ADC_switch2/sdk/apps/dongle/...`
(GameSir builds the pad from the SDK's `apps/dongle`), compiled Jul 30 2026,
BT controller lib `BTCTRLER-@20250911`. The receiver is built from
`...\GameSir_Cyclone2_Dognle_V121\HCI_SDK_v0.0.5\...`, dated Sep 26 2025.
The receiver also contains `.../br22/SDK/lib/crypto_toolbox/ecdh/bigint.c`.
That's an assert path (`__FILE__`) from JieLi's ECDH library (Secure Simple
Pairing), next to `ASSERT-FAILD: ctx->tol_used <= ECDH_MEM_POOL_SIZE`. The
public SDK's `crypto_toolbox_Osize.a` doesn't carry it.

## Pad modes

The pad's mode names (wired: `wire pc start … mode`, `usb change … mode!`;
wireless: `bt … mode`, `sw:wireless …`):

| Link | Modes |
|---|---|
| USB | xinput, dinput, android, ps3, ps4, ps5 (`sw:wire ps5`), switch enc (`USB_SWITCH ENC`), test, charge |
| Bluetooth | xinput (`Xbox Bluetooth Gamepad`), dinput, steam (`HORIPAD STEAM`, see docs/sinput.md), ps4 (`PlayStation(R)4`), ps5, switch (`Pro Controller` / `Nintendo`), android, Switch 2 wake (below) |
| Receiver ("24G") | `bt 24g xinput mode`, `bt 24g dinput mode`, `pair 24G mode` |

Each Bluetooth mode keeps its own host address and link key, so the pad can
be paired with several hosts at once (VM map below).

## USB identities

Device descriptors found in the images (`12 01 00 02 …`):

| VID:PID | In | What |
|---|---|---|
| `054C:09CC` | pad, receiver | Sony DualShock 4 (2nd revision). The product string is "Wireless Controller" (the DS4's own) |
| `045E:028E` | receiver | Microsoft Xbox 360 Controller, class 0xFF (XInput / XUSB), "Xbox 360 Controller for Windows" |
| `3537:1053` | pad | GameSir C2 ADC, wired (docs/gamesir-connect.md) |
| `3537:015A`, `0157`, `5437`, `0163`, `0178`, `01AB`, `0141` (bcd 0x0160) | pad | GameSir; which mode each one is is not mapped yet. Stock v3.56 holds them in one table of 15 device descriptors (0x1E5CF82..0x1E5D07E, with Sony `054C:09CC`, Nintendo `057E:2009`, PowerA `20D6:A711`, ShanWan `2563:*`), chosen by code that doesn't name them directly. Our firmware uses `01AB` for the debug log alone (charging mode) |
| `3537:0575` | receiver | idle, no pad linked (docs/dongle.md) |
| `3537:100B`, `102A` | receiver | wireless, GameSir Connect's IDs (docs/gamesir-connect.md) |
| `3537:0280`, `015A`, `0177`, `0178`, `101F` | receiver | GameSir; not mapped yet |

The pad reports its firmware version as bcdDevice (0x0356 = v3.56), and the
receiver its own (0x0121).

USB strings (UTF-16) in both images:
- `Guangzhou Chicken Run Network Technology Co., Ltd.`: the manufacturer, GameSir's company.
- `MSFT100`: the Microsoft OS string descriptor, so Windows binds a driver
  without an install (WinUSB/XUSB). We need this for an XInput mode on Windows.
- `Xbox Security Method 3, Version 1.00, © 2005 Microsoft Corporation. All rights reserved.`:
  the security string a real Xbox 360 controller has. The 360 console reads
  it during its controller authentication.
- `00006F64096B22E7`, `2115DV_0058`: a fixed serial and a model or hardware code, probably the same in every unit.

## PlayStation 4 / 5

### Authentication

*(public)* A PS4 only accepts a controller that signs a challenge. Over USB,
the console sends the challenge in pages with feature report **0xF0**, polls
**0xF2** until the signature is ready, then reads it in pages with **0xF1**. A
genuine controller does the signing in a Sony security chip with a per-device
key and certificate. Third-party pads either carry a licensed chip or relay
the challenge to a real one. The PS5 uses a similar F0/F1/F2 exchange with sub-IDs.

Both images implement this:

- **Receiver** (relays between the console on USB and the pad over BT):
  `Get report id f0`, `ps4 F0 index`, `report_id_f2_idle` / `report_id_f2_busy`,
  `tx ps4 f0/f1/f2`, `tx ps5 f0 sub id 01..03`, `tx ps5 f1 sub id 01/03`,
  `rx f1 sub id 01 ok`, `rx f2 response crc fail`, `Upload_Authdata_index`,
  `PS5 SCC DEVICE ATUH2 DATA READY`, `PS5 SCC DEVICE ATUH3 VERIFY SUCCESS`,
  `set step PS5_SCC_TRAN_AUTH1_DATA, SUB ID 01` / `AUTH3_DATA, SUB ID 02`,
  `rx ps5 device auth status`. Every step also exists as `… with audio`,
  because auth packets share the link with headset audio.
- **Pad** (does the signing): `rx f0 data:`, `f0 index`, `rx f0 sub 01..03 fail`,
  `Set AuthData F0 SubId02 To Scc`, `ps5_scc_back_data_to_usb_fifo:`,
  `get report f2 = ready` / `no ready`, `ps5 f2 auth status`, `ps5 f2 crc_data`,
  `Authdata_index`, `SLS52OneWayAuthTestReady:` (pad @0x5A2C0).

**Reading:** the pad has a security chip ("SCC"; `SLS52` may be its part name
or its auth scheme) that GameSir feeds the console's challenge into. Over the
receiver, the challenge goes USB → receiver → BT → pad → chip and the
signature comes back the same way. **Open:** whether our board has the chip,
and which bus it's on. Next step is a read-only I2C address scan from the
rewrite (the QMI8658 bus, PB04/PB06), then a look at the PCB near the MCU.
Without a chip, PS4/PS5 modes can't pass authentication. A PS4 cuts a
controller that fails it after about 8 minutes *(public)*.

### Pairing a PS4 over Bluetooth

*(public)* A DS4 learns its PS4 over USB: the console writes its own BT
address and a link key to the controller with feature report **0x13**, and
reads the controller's address with **0x12**. After that the pad connects over
BT on its own.

Stock pad: `set ps4 bt addr`, `write bt ps4 host bt addr`, `write bt ps4 link key`,
`read vm ok 4,org_ps4_host_bt_addr`, `read vm ok 10,org_ps4_link_key`,
`re_connect_ps4_host` (VM 13), `get ps4 report 05` (*(public)* report 0x05 is
the DS4's output report: rumble and lightbar), `PlayStation(R)4` (BT name),
`brigness = %d` (lightbar), `ps4 hid control command`, `l2cap init BtPs4SdpConnect`.
On the receiver: `send BTPS4_HID_MOTOR_OUTPUT_ID_0X11_2` / `0X13_2` (*(public)*
0x11 is the DS4's BT output report), `rx ps5 motor`, `read flash Reconnect Ps4 host flag`.

Also: `ps4 change mode to custom!`, `ps4 wheel change mode to custom!`. The PS4
mode seems to have a racing-wheel identity too.

## Nintendo Switch and Switch 2

- **Switch over BT** (as a Pro Controller): `bt switch mode`, `Pro Controller`,
  `Nintendo`, `set switch bt addr`, `write bt switch link key`,
  `switch hid control command`, `switch l2cap connect success`,
  `switch mode send l2cap hid interrupt connect req`, `Switch profile! profile_num`.
  VM 2 = host address, VM 8 = link key.
- **Switch over USB:** `USB_SWITCH ENC`, `switch enc mode`, `get enc data`,
  `enc NULL`, `Switch ID:`, `switch load app`. "enc" suggests an encrypted
  wired handshake. Not decoded.
- **Wake-up:** `switch wakeup send l2cap hid connect req over time`,
  `RX HCI_SWITCH_WAKEUP_OVER_TIME_ENENT`, `SwitchWakeupConnectFlagBuf` (VM 19).
- **Switch 2.** The firmware folder is named `…_ADC_switch2`. About 30 strings,
  all about BLE advertising and scanning: `rx switch2 host adv report:report_type,company_id,len`,
  `JOYPAD_TASK_BLE_SWITCH2_ADV_REPORT_SAVE`, `write study switch2 wakeup adv info`,
  `switch2_wakeup_adv_info_write/read`, `StartSwitch2WakeupAdv`,
  `f_switch2_power_on_send_adv`, `switch2 pair connect success enable scan adv`,
  `switch2 wakeup send hci connect command`, `switch2_reset_count`,
  `ble connect switch2,set disconnect handle`, `switch2 study over led dis start`.

  **Reading:** in a "study" step, the pad scans for the console's BLE
  advertisement, filtered by company ID, and saves it. To wake a sleeping
  Switch 2, the pad replays that advertisement, then connects with
  `HCI_CREATE_CONNECTION` and retries. Input itself probably still goes over
  Classic as a Pro Controller. To implement this, the first job is to decode
  which company ID and payload get saved (the `report_type,company_id,len`
  print shows where they're parsed).

## Xbox

- **Over BT:** name `Xbox Bluetooth Gamepad`, `bt xinput mode`, `send bt xinput home key`,
  `bt xinput out motor data,large,small`, VM 1 = host address, VM 7 = link key.
- **Over USB:** `xinput hid_endpoint_init`, `xinput out endpoint init_ok`,
  `xinput expansion endpoint init_ok` (*(public)* the 360 pad has extra
  interfaces for headset audio and security).
- **Xbox 360 console authentication, on the receiver:** `rx x360 info!!!`,
  `rx x360 challenge data!!!`, `rx x360 verify data!!!`, each with a
  `… crc fail` variant, plus `X360 VID:` / `X360 PID:`. Where the receiver
  forwards these, and who answers, is not known yet.

## Headset audio

The pad has the chip's own DAC (headset out) and ADC (mic), a power-amp
enable (`set enable audio power amplifier`), and headset detect
(`headset inster` / `headset remove`, PA12 per `firmware/boards/c2/c2_pins.h`).

- **Wired:** USB Audio Class: `uac register usb_id`, `config spk endpoint`,
  `usb_audio_mic_close`, `set uac mic tx handler`, `mic_samplingfrequency`, `spkdc`, `micdc`.
- **Over the receiver:** SBC both ways: `bt open spk`, `bt open mic`,
  `init sbc audio buf control`, `init sbc mic buf control`, `sbc_enc_open`,
  `audio_sbc_dec_open`, `bt mic data cbuf full`, `bt audio vol set`.
- **Receiver side:** `Joypad Headset Is Insert Usb Change!!` / `… Remove …`
  (it re-enumerates with or without the audio interfaces), `start send mic`,
  `read mic sbc fail`, `SBCMicDataBuf`, `audio set play vol`, `ps4 open spk`,
  `ps4 set vol`, `switch open spk`. Audio also exists inside PS4 auth
  (`… with audio`): on PS4 the headset goes over the console's own audio reports.
- Android mode resets the pad on headset insert/remove (`Android Mode Headset Inster Reset!`).
- `msbc hw enc timeout`: the chip has a hardware mSBC encoder (wideband speech, 16 kHz).

The JieLi SDK has these blocks (`apps/hid` audio config, `media.a`, `cpu/br23`
audio drivers), so an audio feature is mostly configuration, USB descriptors
and an SBC stream over our link.

## External EEPROM (24C64, 8 KB)

`hw iic read 24c64 over time`, `hw iic write 24c64 over time`, `start eeprom read/write`,
`eeprom_page_num`, `f_eeprom_err`, `frist power on reset eeprom!`, `eeprom clean err!`,
`device user data eeprom_page_num`, `device user data write eeprom_page`,
`read user data ok/err`, `hw i2c op over time re init`.

**Reading:** stock keeps "user data" (probably profiles and remaps) in an
I2C EEPROM, apparently paged for wear, and wipes it on first power-on.

**On our pad (tested 2026-11-17).** The I2C scan (log command `i2c`,
`firmware/boards/c2/gamepad_c2.c`) finds exactly two devices on the IMU's bus
(PB04/PB06): **0xA0** and the QMI8658 at 0xD4. Nothing else answers anywhere
from 0x10 to 0xEE, so **no PlayStation auth chip on this bus**. The device at
0xA0 reads back 8 KB with 2-byte addresses, and its two 4 KB halves differ,
so it is at least a 24C64. Dumped read-only with `tools/c2eeprom.py` (log
command `eeprom [AAAA NNNN]`) to `backups/c2-pad-eeprom.bin` (gitignored:
device-specific). 4150 of 8192 bytes are not 0xFF.

### Stock's EEPROM code (v3.56, traced 2026-11-17)

Disassembly: `fw-re/app_v356.bin` wrapped in an ELF (e_machine 0xF1, one
section at 0x1E00120) and listed with `fw-re/jl-toolchain/common/bin/objdump -d
--triple=pi32v2`. The callers come from `fw-re/sdk/callsites.py` and the
strings from `strxref.py`.

- **Driver.** The I2C is interrupt-driven: a transfer is started, then
  the caller polls a done flag (RAM `60544+1692`) in 10 us steps.
  - `0x1E34C8E` = **read start** `(dev 0xA0, addr, buf, len)`. `0x1E42C0E` = **write start**, same arguments.
  - `0x1E4F9AC` / `0x1E4F966` are the blocking versions (20,000 waits, then
    `hw iic read/write 24c64 over time`).
  - Writes go in 32-byte pages, the 24C64's page size, with a wait after each.
  - Addresses are 2 bytes, big-endian.
- **Boot load** (`0x1E352E4`):
  1. Read 2 bytes at `0x16FE`.
  2. If they are `51 6A` ("Qj", the "formatted" magic): read **4035 bytes** from `0x0000` (15 × 256 + 195) into a RAM struct (`76960+7082`). Read 1 byte at `0x1600`, the **current profile**, printed as `User Profile Num Is : %d`.
  3. Otherwise (`frist power on reset eeprom!`): fill the struct with defaults. Write it as 127 pages × 32 bytes (0x0000..0x0FDF), reading each page back to compare (`eeprom clean err!` on a mismatch). Write `0x1600 = 1`. If all pages matched, write the magic `51 6A` at `0x16FE` last.
- **Other records.** Each is read once at boot and is only valid when its magic matches:

| EEPROM | Size | Magic | Contents (traced) | On this pad |
|---|---|---|---|---|
| `0x0000..0x0FC2` | 4035 | `0x16FE` = `51 6A` | **5 profiles × 680 B** (0x2A8, at 0x000/0x2A8/0x550/0x7F8/0xAA0), then globals from `0x0D48` (below) | profiles 1 and 4 differ from the defaults |
| `0x1600` | 1 | — | current profile (1-based; first-boot default 1) | `01` |
| `0x16FE` | 2 | — | the formatted magic above | `51 6A` |
| `0x1700..0x1741` | 66 | `[64..65]` = `A6 6A` | **stick and trigger calibration**, 32 big-endian u16, printed `read cal data:` (`0x1E37BE2`). See the next table | present |
| `0x1800..0x1803` | 4 | `[2..3]` = `65 66` | `[0]` flag, `[1]` saved platform mode 0..12 (`JoypadSwitchStatus`: `sw:wire pc/ps4/ps5`, `sw:wireless pc/ps4/ps5`, ...). Applied at power-on only when `[0]` = 0 (`0x1E35CF0`) | `01 02`: not applied |
| `0x1820..0x182D` | 14 | `[12..13]` = `68 67` | **IMU factory offsets**, 6 × little-endian s16: accel X/Y/Z, gyro X/Y/Z (`0x1E37A4C`). The sensor calibration (`0x1E42FA2`) averages 130 samples at rest into this RAM block, prints `acc factory = %d %d %d` / `gyro factory = %d %d %d`, and subtracts it from every reading | accel (209, −300, 1466), gyro (−49, −7, 84) |
| `0x1850..0x1852` | 3 | `[1..2]` = `55 89` | one setting byte, to RAM `60544+27` (meaning not traced) | not written (FF) |
| `0x1900..0x197E` | 127 | `[125..126]` = `56 89` | `[0]` = 0, then 124 bytes of "device user data" (RAM `60544+6838`): `read user data ok/err` (`0x1E34F9A`); the reset writes defaults (`0x1E3524E`). Same 124-byte format as the 5 globals blocks | present |

Stick/trigger calibration at `0x1700` (decoded at `0x1E37C0C`; `d[i]` = the
big-endian u16 at 0x1700 + 2i):

| u16 # | Meaning (from where it goes in RAM, and the values) | This pad |
|---|---|---|
| 0, 1 | left stick centre X, Y | 15723, 15679 |
| 2, 3 | right stick centre X, Y | 16670, 16112 |
| 4, 5 / 6, 7 | left X min, max / left Y min, max | 3702, 25362 / 3989, 25957 |
| 8, 9 / 10, 11 | right X min, max / right Y min, max | 5728, 27995 / 4923, 27232 |
| 12..19, 20..27 | two arrays of 8 (left / right?): pairs near (16400, 15300). Not identified (per-direction points?) | |
| 28, 29 / 30, 31 | trigger maxima LT, RT / minima LT, RT (RAM h[1879], h[1908], h[1880], h[1909]) | 1920, 1940 / 49, 54 |

The left/right assignment follows the order of the RAM fields and is not
fully traced. The spans either side of centre (9,600–12,300) match what
our firmware's own calibration measured on this pad (9,862–12,799).

Profile layout (offsets in a 680-byte profile; defaults set at `0x1E3508C..0x1E35226`):

| Offset | Default | Probably |
|---|---|---|
| `0x20`, `0x21` | 75, 75 (the "KK") | two percent settings (vibration?) |
| `0x2E` | 2 | |
| `0x41`.. (16 × 7 B) | `0A 00 00 00 00 00 00` | per-button entries (remap / turbo; first byte 10) |
| `0xB2`, `0xB3` / `0x151`, `0x152` | `01 11` / `01 12` | heads of two 159-byte blocks (copied as a whole at `0x1E34F6A`): maybe the two back buttons' macros |
| `0x1F0..0x1F4` / `0x20C..0x210` | `01 05 5F 00 64` | left / right stick: on, dead zone 5, outer 95, 0, 100 |
| `0x1FA..0x1FC` / `0x216..0x218` | `01 0A 5A` | left / right: on, 10, 90 |
| `0x226..0x22C` / `0x246..0x24C` | `01 00 01 0A 64 00 64` | the two triggers: 10..100 |
| `0x266..0x26D` / `0x287..0x28E` | `00 .. 01 0A 64 00 64` | |

Globals after the profiles, at RAM struct offset 3400 = `0x0D48`: one byte (a
mode index 0..3; 4 is reset to 0), then 5 × 124-byte blocks (`0x0D49..0x0FB4`),
defaults from flash `0x1E9F100`. Same format as `0x1900`'s 124 bytes; likely
lighting. `0x0FBA` = `01 0A` (set at `0x1E35228`).

### What our firmware uses

- **Factory stick/trigger calibration** (`0x1700`) and **IMU offsets** (`0x1820`)
  are real and valid on this pad. They could become the defaults when the
  firmware's own calibration record is empty. Check the magic before
  using a record.
- **In use since 2026-11-17** (`boards/c2/c2_store.c`): the settings and
  calibration records (`core/gp_store.h`) live in the EEPROM, in two copies
  each (A/B, a sequence number and a CRC-16). On first boot the firmware
  formats the chip: it copies stock's factory calibration into its own
  record (log command `factory` decodes it), moves the records over from
  syscfg, and writes a `C2EE` header. Layout: the comment at the top of
  `c2_store.c`. Stock's `0x1700` / `0x1820` stay in place.
- **The EEPROM is ours to use (decided 2026-11-17).** Stock compatibility is
  not a goal. Going back to stock means writing `backups/c2-pad-eeprom.bin`
  back as well as the stock image. So our firmware may reformat it for its own
  data, for example profiles, calibration, or anything too big or too
  frequently written for the chip's syscfg/VM area. Keep reading the factory
  calibration (`0x1700`, `0x1820`) before anything overwrites it, or copy it
  into our own format first.

## Factory, calibration and test modes

- **Calibration:** `>>>>Enter Cal Mode!!`, `Enter Sensor Cal Mode!`, `LStick/RStick cal start/complete/revovery`,
  `LTrigger/RTrigger cal start/complete`, `LT/RT Cal data`, `Trigger Cal Temp`,
  `acc factory`, `gyro factory`, `read cal data:`, `return data cal crc=%x,rx crc=%x`.
- **USB test mode:** `wire pc start test mode`, `USB_TEST_MODE`, `writer test mode flag`
  (VM 17), `test read addr:%x len:%x`, `test write addr:%x len:%x` (a factory
  memory peek and poke), `test charge!`, `usb test mode audio init`,
  `test mode open speaker`, `test mode mic cbuf full`, `autotest`.
- **RF test:** `Enter 24G Carrier Output Mode` (constant carrier for certification), `24G Carrier Mode enter sleep`.
- **Resets and boot keys:** `factory all reset enter auto usb mode!`,
  `power on Cross press`, `power on enter usb updata mode`,
  `have press ps key before reset` (VM 16), `wire pc check ps key press`.
- **Receiver:** `soft reset enter bt test mode` (`SoftResetEnterBtTestFlagBuf`),
  `resume test!!!`, `joypad task test rx`.

## Several receivers

The receiver has `[SEARCHDONGLE]this dongle is master` / `this dongle is slave`,
`other dongle confirm this is master`, `DONGLE_IS_MASTER_SLAVE_CONFIRM`,
`NO_OTHER_DONGLE`. Receivers near each other negotiate a master, maybe so two
receivers and two pads on one PC don't take each other's pads. Not understood yet.

## Stock pad VM item map

From the pad's `read vm ok N,<name>` strings (items 0–22). Useful for reading
a stock pad's VM, or for keeping our own items (syscfg 48/49 and up) clear of
stock data.

| Item | Name | Item | Name |
|---|---|---|---|
| 0 | self bt base addr | 12 | (not printed) |
| 1 | xinput_host_bt_addr | 13 | re_connect_ps4_host |
| 2 | switch_host_bt_addr | 14 | SoftResetFlagBuf |
| 3 | steam_host_bt_addr | 15 | SoftResetEnterSdpFlagBuf |
| 4 | org_ps4_host_bt_addr | 16 | PressPsKeyBeforeResetFlagBuf |
| 5 | dinput_host_bt_addr | 17 | TestModeFlagBuf |
| 6 | 24g_host_bt_addr | 18 | UsbModeFlagBuf |
| 7 | xinput_link_key | 19 | SwitchWakeupConnectFlagBuf |
| 8 | switch_link_key | 20 | SetSwitchModeFlagBuf |
| 9 | steam_link_key | 21 | bt_send_data_over_time_buf |
| 10 | org_ps4_link_key | 22 | bt_device_mode |
| 11 | dinput_link_key | | |

## Receiver flash contents

From `backups/c2-receiver-full.bin` (layout in docs/dongle.md):

- **VM (0x61000, 116 KB):** only 99 bytes are used (to 0x61063). It could
  shrink to a few KB, which would free a lot of app space. Today the receiver
  has about 102.5 KB free before VM.
- **BTIF (0x7E000):** the receiver's own Bluetooth address, its 6 bytes
  stored backwards (`ff ee dd cc bb aa` for AA:BB:CC:DD:EE:FF). It's outside
  the image, which is why reflashing keeps it.
- **Installer record (0x60FF0):** `"dll\0" 01 15 …`: version 0x0115 = v1.21.
- **`tone` area (34,672 B):** JieLi's stock voice prompts, byte-identical to
  the SDK's `cpu/br23/tools/AC695X_config_tool/conf/source/tone_file/`. It's a
  name/offset/size table (32-byte entries: CRC, offset, size, flags, name),
  then `index.idx` and 26 `.wtg` clips (digits 0–9, bt, bt_conn, bt_dconn,
  tws_conn, tws_dconn, low_power, power_off, power_on, ring, paired, linein,
  music, fm, record, rtc, pc). `.wtg` is JieLi's own codec, 10-byte frames
  every 10 ms at 8 kHz. It is **not** G.729, though the frames are the same
  size. The SDK's `conf/source/tone/wtg_decode.exe IN.wtg OUT.pcm` decodes it
  (s16le, 8 kHz mono; it runs under Wine). The prompts are Mandarin. Our
  receiver doesn't use them, so dropping the area would free 34.7 KB.

## Implementation notes

What each feature would need, roughly from least to most blocked:

1. **I2C scan:** done (log command `i2c`). The EEPROM is at 0xA0, and there is no auth chip on this bus.
2. **EEPROM read-out:** done (`tools/c2eeprom.py`), and the layout is traced in stock's code (above). Next: maybe use the
   factory calibration as defaults.
3. **PS4/PS5 over USB.** DS4 identity (`054C:09CC`, "Wireless Controller", the
   DS4 report descriptor) plus F0/F1/F2 relayed to the auth chip. Needs the chip.
4. **PS4 over BT.** Report 0x13 pairing over USB (address and link key saved
   in our syscfg), then BT HID as a DS4 (report 0x11). Also needs the chip.
5. **XInput over USB on Windows.** `045E:028E`, class 0xFF, the `MSFT100` OS
   descriptor. Phase 3 of the receiver link (docs/dongle.md) covers part of
   this. A real 360 console would also need the security handshake.
6. **Switch (Pro Controller) over BT.** The protocol is publicly documented and
   needs no chip. Switch 2 wake-up comes on top: decode the saved BLE
   advertisement first.
7. **Headset audio.** SDK blocks exist. Work = USB audio descriptors on the
   receiver, an SBC stream both ways over our link, DAC/ADC and amp on the pad.
   It costs flash on the receiver (see the receiver's free space above).
