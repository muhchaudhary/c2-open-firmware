# Stick latency: measurements (2026-11-17)

How long a stick movement takes to reach the PC through the C2 receiver,
measured in stages, before and after the stick filter change (plan item
1.5). Firmware of 2026-11-17 (before 0.1.0), pad on the C2 receiver (its own link,
`docs/dongle.md`), one PC.

## Result

| | Old filter (stock's) | New filter |
|---|---|---|
| **In the pad**: stick chip frame → value in the report being built, median | 12.1 ms | **5.5 ms** |
| mean / p95 / max | 12.1 / 15.7 / 17.5 ms | 6.2 / 10.6 / 11.9 ms |
| flicks timed / missed | 161 / 47 | 392 / 22 |
| noise at rest, raw, sd (p-p) | 2 (10–12) | 1–2 (9–13) |
| **The link**: pad → PC, one way (estimate, below) | ~4.5 ms | ~4.5 ms |
| **Stick chip frame → PC**, median (sum) | ~16.5 ms | **~10 ms** |

The new filter takes about 6.5 ms off the median and 5 ms off the p95. It
is no noisier, and fewer quick taps get flattened below the threshold
(missed: 47 of 208, now 22 of 414). Both sticks measure the same. The raw
numbers, one row per flick, are in `data/stick-lag-2026-11-17.csv`.

## What changed

The stick chip streams all four axes over UART, ~3000 values a second per
axis (4-byte frames at 500000 baud). The UART interrupt fires every 128
bytes (2.6 ms) and handles one burst.

- **Old** (stock's way): keep only the latest value; each report takes
  it and averages it with the 5 reports before. At a ~4 ms report tick
  that is a 24 ms window, ~10 ms of lag on its own.
- **New**: the interrupt adds up every accepted value; each report takes
  the mean of all of them since the last report (~12 per axis), so the
  window is one report interval. Same smoothing (more values in it), a
  fraction of the lag. `hjc_stick_value()` in `firmware/src/drivers/hjc_stick.c`;
  `stick_axis` no longer averages (`avg = 1` in `boards/c2/gamepad_c2.c`).

Both are in the firmware: log command `stickfilter old` / `stickfilter new`
(new is the default after boot), to measure or feel them on one image.

## How each stage was measured

### In the pad: `stickprobe` (log command)

`tools/c2log.py --target pad` shows the log; send `stickprobe`, keep the
sticks still for 3 s, then flick them (quick pushes past ~40 % that snap
back to the centre, one direction at a time), then `stickprobe end`.

- **Noise** (first 3 s): spread of the value the stick scaling gets, after
  the filter and before the dead zone. The reports can't show it: they
  read 0 at rest inside the dead zone.
- **Lag** (each flick): from the stick chip frame that first passes 40 % of
  the travel to the report whose filtered value passes it on the same
  side. The frame's arrival time is the UART interrupt's time minus the
  bytes after it in the burst (20 µs each); the report's is when it is
  built. A flick the filter never brings past 40 % within 200 ms counts as
  missed.
- Clock: `gp_us()` (millisecond clock plus the system timer's count); a
  span taken in an interrupt can be 2 ms short at a wrap of the 2 ms step,
  which `gp_us_span()` corrects.

What this covers: the UART burst, waiting for the report tick, the filter.
Not covered: the stick chip's own sampling and filtering before it sends a
frame (the chip is a black box; only an external rig can see it).

Two probe bugs were found and fixed while measuring: a crossing on the
other side counted (a snap-back past the centre read as a 2–3 ms flick),
and on the right stick the side was taken in the chip's direction, which is
inverted (RX, RY: `0x7fff ^ v`). The numbers above are after both fixes.

### The link: `tools/c2lat.py`

With the pad still: `tools/c2lat.py --device /dev/hidrawN` (the receiver).

- **Round trip**, 300 pings PC → receiver → pad → receiver → PC: median
  11.0 ms, p95 13.7 ms, min 7.4 ms. The pad answers from its report tick,
  so the round trip includes one tick wait (~2 ms on average).
- **Report timing**, 15 s: 247 reports/s, median interval 4.0 ms; a report
  arrives a median 1.85 ms (p95 2.6 ms) later than the fastest ones.

One way, pad → PC: about (11.0 − 2) / 2 ≈ **4.5 ms**. An estimate: it
assumes both directions take the same time, which nothing here checks.

## Where the remaining 5.5 ms in the pad goes

Average waits, from the design (they add up to the measured median):

| Stage | Average |
|---|---|
| UART burst: a frame waits for the 128-byte interrupt (2.6 ms apart) | ~1.3 ms |
| The report tick: waits for the next one (~4 ms apart) | ~2 ms |
| The mean: crosses about half a window after the raw value | ~2 ms |
| **Sum** | **~5.3 ms** |

Next steps, cheapest first:

1. A smaller UART step (64 bytes, 1.3 ms; ~780 interrupts a second): about
   −0.6 ms. The step was raised from 4 bytes because 12500 interrupts a
   second lost bytes under Bluetooth load; check the stick error count.
2. A steadier, faster tick (2–3 ms, plan item 5.5): shortens both the
   tick wait and the mean's window, about −2 ms.
3. The link (about 4.5 ms one way): the receiver's poll interval and the
   pad's send scheduling (`docs/dongle.md`, "Pad latency: open work").

## Reproducing

1. Build, flash: `firmware/build.sh`, `sudo fw-re/jlflash.sh write firmware/build/c2.flash.bin`.
2. `tools/c2log.py --target pad --out log.txt` in one terminal.
3. Per filter: `stickfilter old` (or `new`), then `stickprobe`, 3 s hands
   off, ~20 flicks per stick, `stickprobe end`. Send commands with
   `python3 -c` and `c2log.command()`, or any serial terminal (one line
   each). Wait ~20 s after a boot first: the stick chip is noisy at power-up.
4. Pad still: `tools/c2lat.py --device /dev/hidrawN`.
5. Per-flick rows: `grep -oE "probe: [LR][XY] [0-9.]+" log.txt`.

Keep only one reader on the log port: two readers each get part of the
lines.
