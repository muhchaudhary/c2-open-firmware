# Firmware: work plan (2026-11-17)

From a review of `firmware/` on 2026-11-17, checked against the code. Each
item says where the problem is and what done looks like. Tick the box and add
the commit when an item lands. Stock's feature set, the reference for what
is still missing: `docs/stock-features.md` (first GameSir's manual for the
C2, "What GameSir says the C2 does", then the firmware's strings) and
`docs/gamesir-connect.md`.

Order: correctness first (1), then what makes later changes testable (2),
then features by value for effort (3–5), then housekeeping (6).

## 1. Correctness

- [x] **1.1 Durations by the clock, not by counting ticks.** (Done
  2026-11-17; tested on the pad.) `GP_TICK_MS` is
  3, but the tick fires about every 4 ms (`docs/firmware-design.md`,
  "Architecture"), and the stick and trigger code only runs when a report is
  built (`gp_read_state` comes after the `busy()` return in `gp_core.c`).
  So every `X / GP_TICK_MS` duration runs at least ~30 % long. Affected:
  Xbox Bluetooth rumble durations and the 1 s level feedback
  (`gp_rumble.c`), trigger rest learning and re-zero (`hall_trigger.c`),
  stick settle / learn / hold / commit (`stick_axis.c`), the stick chip's
  baud search (`hjc_stick.c`), the IMU's reset wait (`qmi8658.c`), the
  calibration rest (`gp_cal.c`), the boot I2C scan delay (`gamepad_c2.c`),
  the stall detector and the early features window (`gp_core.c`). Fix: a
  start time from `jiffies_msec()` and a compare. `GP_TICK_MS` stays only as
  the nominal rate reported to hosts.
  Not in scope: the poll (`GP_POLL_MS`, a `sys_timer`) counters. Check with
  one log line whether the poll drifts; convert those too if it does.
- [x] **1.2 Gyro bias learning eats slow turns.** (Done 2026-11-17:
  1.5 s resting window with the accelerometer steady,
  new bias near the current one or repeated 6 s; tested on the pad.) `gp_motion.c` learns a bias
  whenever the gyro varies by less than 3 dps over 64 samples (~256 ms), with
  no check that it is near zero, and overwrites a stored calibration bias.
  A slow steady turn becomes bias. Fix: also require the accelerometer to be
  steady, a longer window by time (~1–2 s), and with a calibrated bias only
  accept a learned one within a small distance of it.
- [x] **1.3 Calibration save result.** (Done 2026-11-17: config
  status 6.) `cal_save()` ignores `gp_store_write`'s
  result; `CAL_FINISH` should report the failure (as the settings save
  logs it). The FINISH / tick overlap is harmless (it can only widen the
  range): leave it, note it in a comment.
- [x] **1.4 Low-battery warning.** (Done 2026-11-17: at 5 % on
  battery, from the board's percent, not the SDK event; tested with `lowbat test`.) At 3.4 V the SDK only prints a log line
  (`app_power_manage.c`, `POWER_EVENT_POWER_WARNING`). Show it: a red Home
  blink until charging or power-off; hook the event in `gp_main.c`.
- [x] **1.5 Stick lag.** (Done 2026-11-17: median 12.1 → 5.5 ms in
  the pad, measured with `stickprobe`; write-up in `docs/latency.md`.) The 6-sample mean in `stick_axis.c` runs at the
  report rate: ~10 ms average lag at a 4 ms tick. Average the stick chip's
  raw stream in `hjc_stick.c` instead (as that file's comment suggests);
  measure before and after with `tools/c2lat.py`.

## 2. Testability

- [x] **2.1 Host unit tests.** (Done 2026-11-17: `firmware/test`, 9 programs,
  ~5400 checks, ASan + UBSan; found the curve rounding bug below 32767.) `gp_tuning.c`, `stick_axis.c`,
  `hall_trigger.c`, `gp_link.c`, the SInput / XInput encoders and the
  settings table don't need hardware. Build them on the PC with a small
  `typedef.h` / `jiffies_msec` / `adc_get_value` shim (`firmware/test/`),
  one `make test`. The time-based counters (1.1) get tests with a fake
  clock.
- [x] **2.2 CI.** (Done 2026-11-17: `.github/workflows/host-tests.yml`, gcc and
  clang, every push and PR.) GitHub Actions running 2.1.
- [x] **2.3 Pin the SDK.** (Done 2026-11-17: `firmware/sdk/SDK_COMMIT`, checked
  by `sync.sh`, which also warns about other SDK edits.) Record the fw-AC63_BT_SDK commit `patch_sdk.py`
  expects (15 patched files in `sync.sh`), fail early on a mismatch.
  The SDK files copied into the image as they are (`uboot.boot`,
  `cfg_tool.bin`) are pinned by sha256 in `mkimage.py` (2026-11-18).
- [x] **2.4 Firmware build in CI.** (Done 2026-11-18:
  `.github/workflows/firmware.yml`.) Both boards on every push and PR: the
  toolchain from the flake, the SDK sparse-fetched at `SDK_COMMIT`,
  `build.sh --wall` (`-Wall` on our sources, any warning fails; three in
  `board_c2.c` fixed), the image, ELF and JSON sidecar (release, commit,
  SDK commit, sizes, sha256) uploaded per run. Idea from
  github.com/glvno/matryoshka's workflows.

## 3. Configuration everywhere

- [x] **3.1 Config in every mode.** (Done 2026-11-17: over the USB log port,
  pad and receiver, any mode; tested in Steam and Xbox mode.) The protocol rides on SInput reports
  only (`gp_sinput.c`), so Xbox mode has none. Add a second path: the USB
  debug-log serial port (wired, any mode) and/or a vendor HID collection.
- [x] **3.2 Version info.** (Done 2026-11-17: `firmware/VERSION`, INFO bytes
  43..45, protocol 3.) INFO returns a bare git hash. Add a protocol /
  firmware version number (tagged releases) so tools can check
  compatibility.
- [x] **3.3 Browser config app.** (Done 2026-11-17: `tools/c2cfg.html`, WebHID +
  WebSerial; tested on the pad.) WebHID single page over the same protocol:
  settings, profiles, remaps, calibration. Replaces `tools/c2cfg.py` for
  users.
- [ ] **3.4 Firmware update from the app.** (Later.) Not from the browser
  alone: the bootloader is a USB mass-storage device (4c4a:2342, SCSI over
  `/dev/sg`), which WebUSB blocks and the kernel's storage driver holds.
  Options: document the one-command `jlflash.sh write` path in the app;
  an opt-in udev rule for sudo-less flashing; tagged releases with images
  and a "newer firmware" check in the app.

## 4. Mapping

- [ ] **4.1 Paddles in Xbox modes.** Neither Xbox encoder has a paddle bit,
  so they are dead unless remapped. Per-mode default maps (e.g. paddles →
  A / B or L3 / R3) or a "mode defaults" setting.
- [ ] **4.2 Triggers in the remap table.** L2 / R2 aren't in
  `gp_map_buttons`: no trigger → button, button → trigger, swap, turbo.
- [ ] **4.3 Stock mapping features**: toggle buttons; a Shift layer;
  remap to up to 3 targets; paddle macros (stock: up to 30 steps of key,
  hold, delay; loop; recorded on the pad); ABXY swap; d-pad SOCD / diagonal
  lock; a stick as d-pad or mouse.

- [ ] **4.4 On-pad configuration, as stock's manual has it** (2026-11-18,
  `docs/stock-features.md`): M + L4 / R4 programs a back button (a button
  or a combo); M + button cycles turbo 8 / 12 / 20 Hz / off, LT / RT
  included, double-tap M clears, Home flashes while on; M + A swaps ABXY;
  Mode + stick click toggles a zero dead zone; a hair-trigger toggle;
  calibration (sticks and triggers, gyro) and function reset from combos.
  Check each against our combos first (M + d-pad is rumble and profiles).
- [ ] **4.5 Modes as stock**: hold M to show the mode, automatic mode
  detection on the cable and the receiver. (Stock's mode combos and
  colours: done for 0.3.0, Home + A Steam yellow, Home + Y Switch red.)
- [ ] **4.6 Low battery as stock**: red twice every 10 s under 15 % (ours: a
  short flash every 2 s at 5 %).

## 5. Tuning, motion, LEDs, modes

- [ ] **5.1** Square gate and 5-point response curves (sticks, triggers).
- [ ] **5.2** Per-motor rumble strength.
- [ ] **5.3** Gyro to stick / mouse with an activation button (for Xbox modes,
  where the host gets no gyro); stock's "Aim" and "Tilt" modes with dead
  zones and a curve. After 1.2.
- [ ] **5.4** LED settings: brightness, idle dimming, saved colours /
  animation presets, per-zone colours (today colours come only from the
  host).
- [ ] **5.5** Report rate setting, and the open latency work: a steady 2–3 ms
  tick (`docs/latency.md`, "Where the remaining 5.5 ms in the pad goes").
- [ ] **5.6** Switch Pro and DS4 modes (stock has both). DS4 on the pad
  (Bluetooth, USB and the receiver, `docs/ds4.md`): tested on Linux
  2026-11-17 (`hid-playstation`, SDL, Steam), Windows 10 through the
  receiver (DS4Windows). Switch Pro on the pad (Bluetooth and USB,
  `docs/switch.md`, Home + Y): tested on Linux 2026-11-18 over Bluetooth,
  USB and the receiver (SDL: inputs, rumble by band, the gyro's
  directions). Still open: a Switch console; USB pairing (subcommand
  0x01) and Switch 2 wake-up.

## 6. Build and docs

- [x] **6.1 Builds from our code and the SDK only.** (Done 2026-11-17,
  `firmware/mkimage.py` + `boards/*/image.json`; both flashed and tested.)
  Stock firmware is GameSir's, not ours: no build needs it now, and it left
  git (`.gitignore`: kept locally for reverse engineering). The pad: the whole container from the SDK's `uboot.boot`
  and `cfg_tool.bin`; byte-identical to the image built on v19 but for
  `uboot.boot`'s build time (dead code, two builds of the same source) and
  the slot A directory header (now our app's values). The receiver: the
  app area alone at 0x11000, as its own loader at 0x0 (via the header at
  0x10000) expects; its loader, header and installer record are left as
  they are. Images 0.2.1 from it, tested. The history rewritten without
  them (2026-11-17): stock firmware, decompiled and disassembled stock,
  GameSir Connect's logs and DLLs, the images before 0.2.1.
- [x] **6.4 Reproducible builds.** (Done 2026-11-18, `patch_sdk.py`.) The
  only difference between two builds of one tree was the SDK's `__DATE__` /
  `__TIME__` (log lines in `apps/hid/app_main.c`, `cpu/br23/setup.c`); both
  now fixed (`Jan  1 1970`, `00:00:00`). Checked byte for byte, both
  boards: two clean builds a minute apart, an incremental rebuild,
  switching boards without a clean, another SDK checkout path, and CI
  (Ubuntu) against local (NixOS). The toolchain is
  pinned by the flake, the SDK by `SDK_COMMIT` and the embedded files'
  sha256. The committed 0.2.1 images predate this: the next release is the
  first that rebuilds exactly.
- [x] **6.5 Release workflow.** (Done 2026-11-18:
  `.github/workflows/release.yml`, the steps in `firmware/README.md`.) Run by
  hand on main: each committed image (`firmware/images/`, with its
  `build.sh` sidecar) rebuilt at the commit it was built from, required
  byte-identical to the hardware-tested one; then a release folder with
  the images, ELFs, sidecars, `manifest.json`, `SHA256SUMS` and the
  licences, hashes in the run summary. No tagging or publishing by itself.
  First real use: 0.2.2.
- [x] **6.6 Licence for our code.** (Done 2026-11-18: Apache-2.0, `LICENSE`.)
  `LICENSING.md` lists what the images embed (JieLi's SDK: Apache-2.0; the
  toolchain's newlib and compiler-rt).
- [x] **6.3 Image history.** (Done 2026-11-17: a changelog in
  `firmware/images/README.md`; the pad image renamed `C2_pad.flash.bin`.)
