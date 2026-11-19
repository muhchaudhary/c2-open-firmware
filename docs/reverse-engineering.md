# Reverse engineering the stock firmware

How this project read GameSir's firmware, and how to do the same with the
copy from your own controller: get the image, decrypt it, disassemble it,
then find what you're after. The project's findings are already written up
elsewhere (`docs/stock-features.md`, `firmware/boards/c2/c2_pins.h`,
`docs/firmware-design.md`). Read those first. Use this page when you want to
check a finding, go further than they do, or look at a firmware version they
don't cover.

**Nothing from GameSir is in this repository.** Their firmware isn't ours to
redistribute, so you bring your own copy and keep it out of git. `backups/`,
`fw-re/orig/`, `fw-re/refs/` and `fw-re/app_v356.bin` are already in
`.gitignore` for that. Don't publish dumps, listings or decompiled code.
Publish findings: addresses, layouts, behaviour.

## Was the stock firmware decompiled?

Partly, and on purpose. The stock app (`app.bin`, about 470 KB of pi32v2
code) is mostly JieLi's SDK: the OS, the Bluetooth stack and the drivers.
JieLi publishes that SDK
([fw-AC63_BT_SDK](https://github.com/Jieli-Tech/fw-AC63_BT_SDK)), so those
parts can be named automatically instead of read. Only GameSir's own code
needed reading, and only the parts a feature depended on:

- **Disassembled whole.** Both the pad (v3.56) and the receiver (v1.21).
- **Named automatically.** Several hundred functions are matched to SDK
  functions by `sdkmatch.py`.
- **Decompiled to C with Ghidra.** The app's main task (`joypad_task`) and
  the 28 message handlers it dispatches to. That was for the hybrid
  firmware (History, below). The rewrite doesn't use that C.
- **Traced by hand in the listing.** Whatever a feature needed: the board
  init and every pin, power and wake-up, the stick chip's UART, the EEPROM
  layout, calibration, the DS4 report.

Most of GameSir's host-side code is still unread, the receiver's in
particular.

## What you need

- A Linux machine, and the dev shell (`nix develop`) or, without Nix,
  `fw-re/get-toolchain.sh` and `fw-re/get-uboot-tool.sh`.
- JieLi's toolchain. Its `objdump` reads pi32v2 code. Below, `$JL_TOOLCHAIN`
  is its directory: the dev shell sets it, and without Nix it's
  `fw-re/jl-toolchain`.
- For automatic naming, a build of JieLi's SDK. `firmware/build.sh` makes
  one: it leaves `cpu/br23/tools/sdk.elf` in the SDK checkout.
- Optional: Ghidra with a pi32v2 processor module (Ghidra, below).

## 1. Get the image

Pick one of two sources.

**Your controller's flash (the best source).** It's exactly what your
controller runs, and the read is also your backup. With stock firmware on
the pad and the pad plugged in over USB:

```sh
sudo fw-re/jlflash.sh enter pad          # GameSir's own HID command (0F 17 55 88)
sudo IMG_ADDR=0 LEN=0x100000 fw-re/jlflash.sh read backups/c2_full.bin   # the whole chip, 1 MB
sudo fw-re/jlflash.sh read backups/c2-pad-stock.flash.bin                # the image alone (0x1000, 0x77000 B)
sudo fw-re/jlflash.sh reset
```

Reading is safe: it doesn't change the chip. For the receiver, use
`enter receiver` and `IMG_ADDR=0 LEN=0x80000`. It only obeys the command
while a stock pad is linked to it (`docs/dongle.md`). The EEPROM (calibration,
profiles, IMU offsets) is separate: `tools/c2eeprom.py`.

**A GameSir Connect update package (`.ufw`).** GameSir Connect carries
firmware packages under `resources/dfu/packages/`
(`docs/gamesir-connect.md` says where its files are). List one and pull out
its flash image:

```sh
python3 fw-re/ufw.py list    GS_C2_ADC_DEVICE_V356_No_Key.ufw
python3 fw-re/ufw.py extract GS_C2_ADC_DEVICE_V356_No_Key.ufw pad.flash.bin
```

The receiver's package holds six copies of the same app, one for each chip
variant. `extract` takes record 0, and `--at OFFSET` takes another.

## 2. Decrypt it

JieLi images are encrypted with a CRC16-based XOR keystream: "ENC" for the
headers, "SFC" for the app area, keyed by the chip key in
`isd_config.ini`. `fw-re/jl.py` is our implementation, written from
[kagaimiq's jielie notes](https://github.com/kagaimiq/jielie). It doesn't protect
anything; it only obscures.

```sh
python3 fw-re/flash.py pad.flash.bin pad/
```

This prints every entry it finds, with its CRC check, and writes:

| File | What it is |
|---|---|
| `app.bin` | the application: the code you'll read |
| `cfg_tool.bin` | settings blob (BT name, etc.) |
| `isd_config.ini.bin` | the loader's settings: debug UART pin, flash offsets, chip key |
| `decrypted.bin` | the whole image, decrypted in place |
| `tone.blk` | receiver only: its prompt tones |

The container layout (JLFS header, `uboot.boot`, slot A and slot B, VM,
`key_mac`) is in `docs/firmware-design.md`, "The boot container and the SDK".

## 3. Look at the strings

Do this before any disassembly. The stock firmware logs a lot, so the
strings alone map out most of its features:

```sh
strings -a -t x -n 5 pad/app.bin | less
```

Some things the strings gave this project without any code reading: the
mode list, the PS4 authentication messages, the VM item names
(`read vm ok N,<name>`), the button names (`power on <name> press`), and
build paths that show GameSir built the pad from the SDK's `apps/dongle`.
`docs/stock-features.md` is mostly this step. Treat a string as a lead, not
proof: the code base is shared with other GameSir pads, so it has strings
for hardware the C2 doesn't have.

String offsets are offsets into `app.bin`. Add the XIP base to get the
address code uses: `0x1E00120` on the pad, `0x1E000C0` on the receiver.

## 4. Disassemble it

`app.bin` is a raw blob. Wrap it in an ELF with JieLi's machine number and
list it:

```sh
python3 fw-re/mkelf.py pad/app.bin pad/app.elf 0x1E00120      # receiver: 0x1E000C0
"$JL_TOOLCHAIN/common/bin/objdump" -d --triple=pi32v2 pad/app.elf > pad/app.dis 2> pad/dis.err
```

The pad's v3.56 listing is about 178,000 lines. The tools below all read
this `.dis` file. Things that cost time to learn:

- **The base matters.** Listing the receiver at `0x1E00120` instead of
  `0x1E000C0` puts every absolute address 0x60 too high, and it looks
  almost right.
- **The boot-RAM block.** Startup copies the end of flash to RAM 0: on the
  pad, listing `0x1E67C78..0x1E73449` (`0xB7D1` bytes); on the receiver,
  `0x1E41B14`, `0x4F40` bytes. Code there runs from RAM. `0x2000000 + x` in a
  call is an alias of RAM `x`. The `fw-re/sdk` tools resolve both, and
  `--ramblk ADDR LEN` sets the block for an image other than the pad's
  v3.56. To find it in a new version, look at the startup code's second
  copy loop (`r1 = src`, `r2 = len`, copying to RAM 0).
- **Paired instructions.** Some pi32v2 instructions issue together with the
  next one, which then reads the *old* value of a register. objdump marks
  them with `#`. A `r5 += r6` read without this in mind gives wrong
  arithmetic. This broke several early binary patches.
- **Functions** start with a `[--sp] = {rets, ...}` push. objdump has no
  symbols, so names come from the next step.

## 5. Name what you can

```sh
"$JL_TOOLCHAIN/common/bin/objdump" -d --triple=pi32v2 ../fw-AC63_BT_SDK/cpu/br23/tools/sdk.elf > sdk.dis
python3 fw-re/sdk/sdkmatch.py sdk.dis pad/app.dis pad/matches.tsv
# receiver: add --ramblk 0x1E41B14 0x4F40
```

`sdkmatch.py` compares instruction sequences with constants and branch
offsets masked out, then follows calls out of each matched function to name
what they call. How many it names depends on which SDK app you built: 560
to 660 on the pad, about 300 on the receiver. Matches with a ratio under
0.85 are hints, not facts. Anything still unnamed after this is probably
GameSir's own code, so that's where to read.

To see what an SDK library function looks like compiled (the SDK ships the
Bluetooth stack, the OS and the CPU code as LLVM bitcode, not source):

```sh
fw-re/sdk/sdklib.sh btstack.a hid.c.o /tmp/hid    # writes hid.c.o.dis and hid.c.o.relocs
```

## 6. Find the code for a feature

These tools take the listing (plus `matches.tsv` for function names) and
answer one question each:

| Question | Tool |
|---|---|
| Which code prints this log string? | `strxref.py app.dis app.bin "TEXT"` |
| Who calls this function, and with which constant arguments? | `callsites.py app.dis ADDR=name ...` |
| Which pins are set up, and how (direction, pull-ups)? | `pinscan.py app.dis app.bin matches.tsv` |
| Which peripheral registers does it touch? | `sfrscan.py SDK/include_lib/driver/cpu/br23/asm/br23.h app.dis matches.tsv out.tsv` |
| Who reads or writes this RAM range? | `addrscan.py app.dis LO HI` |

All of them are in `fw-re/sdk/`.

### Worked example: the EEPROM layout

The stock pad keeps calibration and profiles in a 24C64 EEPROM. This is how
its layout (`docs/stock-features.md`, "Stock's EEPROM code") was found:

1. **A string leads to the driver.** `strings` shows
   `hw iic read 24c64 over time`, the timeout message of a blocking EEPROM
   read. `strxref.py` gives the function that prints it. That function's
   callees include the function that starts a read, `0x1E34C8E`.
2. **The calls give the layout.** `callsites.py` on that function lists
   every EEPROM read and its constant arguments:

   ```
   $ python3 fw-re/sdk/callsites.py pad/app.dis 0x1E34C8E=eeprom_read_start
   01e34fa8  call eeprom_read_start   r0=160  r1=0x1900 r2=?      r3=0x7f
   01e352f8  call eeprom_read_start   r0=160  r1=0x16fe r2=0xe4e4 r3=2
   01e3538c  call eeprom_read_start   r0=160  r1=0x1600 r2=?      r3=1
   01e35cfc  call eeprom_read_start   r0=160  r1=0x1800 r2=?      r3=4
   ...
   ```

   `r0=160` is device `0xA0` (the 24C64's I2C address), `r1` the EEPROM
   address, `r2` the RAM buffer, `r3` the length. Every record and its size
   is in this list.
3. **The code around each call gives the meaning.** At `0x1E352F8`, 2 bytes
   are read from `0x16FE` and compared with `51 6A`. If they match, 4035
   bytes are read from 0, then 1 byte from `0x1600`, which is then printed
   as `User Profile Num Is : %d`. That makes `0x16FE` the "formatted" magic,
   0..4034 the profiles and `0x1600` the current profile.
4. **Compare with a real dump.** `tools/c2eeprom.py` reads your EEPROM.
   Values that make sense (stick centres near mid-scale, IMU offsets near
   zero) confirm the decoding.

The pin map (`c2_pins.h`) was built the same way: `pinscan.py` for GameSir's
inline pin set-up, then each pin checked against the debug strings or a
matching SDK driver, with the stock address and a confidence level on every
entry.

## 7. Other things to read besides the firmware

Not everything was in the firmware:

- **GameSir Connect** (an Electron app): unpack the installer, then
  `app-32.7z`, then `resources/app.asar`. `dist/electron/main.js` holds the
  pad's configuration protocol, the 680-byte profile format, and the
  bootloader command `0F 17 55 88` (`docs/gamesir-connect.md`). The first
  command found, `0F FC`, does nothing on the C2: only the C2's own handler
  had the right one.
- **What goes over the air or the cable.** `btmon` for Bluetooth,
  `usbmon` / Wireshark for USB, and `/sys/class/hidraw/*/device/report_descriptor`
  for descriptors. That's how the DS4 report bugs below were found and
  checked.
- **The running pad.** The stock firmware's debug UART pin is in
  `isd_config.ini`. Our firmware has a USB debug log instead (`tools/c2log.py`).

## Ghidra (optional)

Stock Ghidra doesn't support pi32v2: it refuses the ELF ("unsupported ELF
architecture"). The project used the community ghidra-jieli module (the
fork in
[quarkslab/ghidra-jieli#1](https://github.com/quarkslab/ghidra-jieli/pull/1),
which adds a complete pi32v2 SLEIGH spec), installed into Ghidra 12.1. With
it, import `app.bin` as a raw binary, language `pi32v2:LE:32:default`, base
`0x1E00120`. Ghidra found about 1,975 functions in the pad's v3.56.

Its C output is a starting point, not something to trust as is. Before
using any of it, check it against the objdump listing, paired instructions
especially. The hybrid's `joypad_task` needed several fixes against the
listing before it matched. The tools above work on the objdump listing
alone. Most of the findings came from there, not from Ghidra.

## History: how the project got here

The work went through four stages, and each one used the stock firmware
differently:

1. **Binary patches (Oct 2026).** In-place byte patches to the stock v3.56
   app. They kept every CRC: four dead-register bytes were chosen so
   `app.bin`'s CRC16 didn't change. Fixes included the DS4 Bluetooth battery
   level (the stock firmware never wrote it), a race that zeroed about 7% of
   DS4 status bytes, DS4 CRC failures, and a Bluetooth XInput mode. Lessons:
   don't patch inside the boot-RAM block (it becomes initial RAM data);
   the only free space for new code was an unreferenced `__FILE__` string at
   `0x1E638F4`; send one packet per tick, never an extra one from inside the
   Bluetooth send path.
2. **Mapping where the SDK ends (Phase 0).** The question was where the SDK
   ends and GameSir's code begins. The answer: the SDK never holds a
   pointer to the app. SDK tasks post 16-bit messages to the app's
   `joypadtask`, which dispatches them through a 43-entry jump table.
3. **The hybrid.** That task and its handlers were decompiled with Ghidra,
   rewritten in C against the listing, and built into the stock image's
   reclaimed space. That gave a working SInput mode, still on the stock
   base.
4. **The rewrite (Nov 2026, this repository).** A whole new app on JieLi's
   public SDK, with the stock firmware used as reference only. The first
   from-scratch attempt bricked a pad on one mistranscribed board-init line.
   It was recovered with the ROM's USB key (`tools/usbkey-mega2560/`), and
   since then every pin is checked against the stock listing.

The PS4 work also settled a feasibility question. The stock firmware
answers the PS4's challenge with a weak one-way scheme, not real DS4
RSA-2048 signing. It has a single SHA-256 table and no big-number code, so
genuine DS4 authentication was never possible by patching the stock image.

## Safety

- **Back up the whole chip and the EEPROM before flashing anything.**
  Commands are in step 1.
- Reading is always safe. Writing is safe as long as the bootloader stays
  reachable. `docs/firmware-design.md`, "Safety", lists the ways back into
  it, the last being the ROM's USB key.
- Never flash a stock image you've modified without `mkufw.py verify`.
  `jlflash.sh write` checks the device name and verifies before it resets.
