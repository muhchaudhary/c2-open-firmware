# usbkey-mega2560 — USB_KEY sender for an Arduino Mega 2560

For a controller whose firmware hangs before USB (no LEDs, nothing in
`dmesg`), so `jlflash.sh enter` can't reach it. The BR23 boot ROM checks the
USB data lines for a key (`0x16EF`, sent bit-serially: one line the clock,
the other the data) at every boot; if it sees it, it acknowledges by holding
both lines low for 1–2 ms, skips the firmware and comes up as the JieLi
bootloader (`4c4a:2342`). The Mega sends that key. Background:
`fw-re/jl-uboot-tool/docs/how-to-enter-uboot.md` (it contradicts itself on
which line is the clock and gives no bit rate, so the sketch rotates through
both line orders and several speeds).

How it works:

- **The PC stays wired to the controller the whole time.** After the ACK the
  ROM calibrates its clock from the PC's SOFs and only tries a few times.
  Swapping cables or moving wires by hand was too slow every time (on every
  port): the ROM then enumerated with a wrong clock, `error -71`. So the Mega
  takes itself off the bus electronically, within microseconds of the ACK.
- The Mega's pins are 5 V, so the bus pins are open-drain (pulled low or
  released, never driven high), and each line's pull-up is a divider between
  two pins (5 V → 1 kΩ → node → 2 kΩ → 0 V = 3.3 V). Both pins go
  high-impedance on the ACK, which removes the pull-up completely. The node
  voltage is checked at every start; above 3.6 V nothing is driven.
- The ACK is read with the ADC (a 5 V AVR needs 3.0 V for a guaranteed digital
  high) and only counts if the line is low for 0.5–5 ms; a host port reset
  (10 ms or more) does not.
- Both ACKs seen so far were clock on D+, data on D-, so those modes go first.

## What worked (2026-11-02)

Recovered a C2 bricked by an image whose board init crashed. Notes from that run:

- The ACK only comes right after pressing the reset button on the back of the
  controller (the ROM checks for the key once per boot). Every ACK was
  `clock D+, data D-`, 3 or 30 µs; low for ~2 ms.
- Without the key, the bricked firmware shows nothing on USB at all. After an
  ACK, a late connection shows as `error -71` / `not accepting address`.
- What got `4c4a:2342`: a **very fast** plug swap, from the Mega's cable to a
  plain cable to the laptop, right at the ACK. Swaps of about a second, and
  the always-connected divider wiring below, both gave `-71` every time.
- `jlflash.sh write` of the v19 image (`fw-re/refs/`, a local copy) then
  verified and the controller booted v3.56 (`3537:1053`).

Compiles with `arduino:avr` 1.8.8.

## Wiring

A cut USB-A to USB-C cable, both halves joined straight through. The Mega
connects to the joined data wires.

```
  USB-A half (laptop)                                USB-C half (controller)
  red   VBUS ──────────────────────────────────────── red   VBUS
  black GND  ─────────────────────┬────────────────── black GND
  white D-   ───────────────────┬─│────────────────── white D-
  green D+   ─────────────────┬─│─│────────────────── green D+
                              │ │ │
                          220 Ω 220 Ω  └── Mega GND
                              │ │
            D+ node ──────────┘ └────────── D- node

  D- node:  Mega A0,   1 kΩ to pin 22,   2 kΩ to pin 23
  D+ node:  Mega A1,   1 kΩ to pin 24,   2 kΩ to pin 25
```

2 kΩ = two 1 kΩ in series. Nothing goes to the Mega's 3V3 or 5V pins.

## Procedure

```sh
arduino-cli compile --upload --fqbn arduino:avr:mega -p /dev/ttyACM0 .
picocom -b 115200 /dev/ttyACM0
```

1. With the cable **not** plugged into the laptop or the controller, check the
   first line: `pull-up check: D- node 3.3 V, D+ node 3.3 V`. Above 3.6 V the
   sketch refuses to key: fix the wiring before connecting anything.
2. Plug the USB-A half into the laptop, then the USB-C half into the
   controller. Watch `journalctl -kf | grep --line-buffered -E 'usb|4c4a'`.
3. Press the controller's reset button. On `ACK received` nothing needs to be
   touched: the log should show `BR23UBOOT1.00` (`4c4a:2342`).
4. `sudo fw-re/jlflash.sh write firmware/images/C2_pad.flash.bin`.

**One mode only.** Send `1`–`6` to lock the sketch to one mode, `a` to rotate
again. All ACKs so far were mode `1` or `2` (clock D+, data D−; 3 or 30 µs).
Lock one for a device that checks the key only once per power-on, such as
the C2's USB receiver (no battery, no reset button: switch its VBUS on last,
while keying; docs/dongle.md).

**Listen mode.** `l` stops keying, releases both lines (no pull-ups) and
prints each D+ / D- level change with how long the old level held; `r` keys
again. For a device that signals on its USB pins: the C2 receiver's
bring-up builds pulse D+ / D- n times at boot checkpoint n (docs/dongle.md).

`line low … not an ACK` lines while keying are the laptop resetting the port;
they are ignored. Send `r` to key again.
