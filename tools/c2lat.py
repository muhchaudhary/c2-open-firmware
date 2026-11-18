#!/usr/bin/env python3
"""c2lat.py: measure an SInput gamepad's latency from the PC (wired pad or receiver).

  sudo tools/c2lat.py [--device /dev/hidrawN | --name SUBSTR] [--count 300]
  sudo tools/c2lat.py --list

Two measurements, both with nothing but the PC's own clock:

1. Round trip. Sends SInput's features request (output report 03 02) and
   times until the pad's features reply (input report 0x02, first byte 02)
   arrives: PC -> pad -> PC. Through the receiver on its link (the pad's
   own protocol, docs/dongle.md) the receiver answers the features request
   itself, so there the tool sends the receiver's ping (03 8E): it asks the
   pad for a hello and answers 02 8E when it comes (--method). Through the receiver that is USB, Bluetooth out,
   the pad's tick, Bluetooth back, USB. The same on the wired pad gives the
   baseline, so the difference is what the wireless link adds (both ways).
   The receiver times its own part of each one (debug log, "lat: echo").

2. Report age spread. Each state report carries the pad's timestamp (bytes
   19..22, us, stepped in ms): arrival time minus timestamp is the delivery
   delay plus an unknown constant offset (and slow clock drift, fitted out).
   Its spread above the best case shows how much a report waits on top of
   the fastest path (queuing in the pad, the link, the receiver, USB).

Run 1 with the pad still (fewer state reports to sift); move the sticks for 2.
"""
import argparse
import glob
import os
import select
import statistics
import struct
import sys
import time


def hidraws():
    for d in sorted(glob.glob('/sys/class/hidraw/hidraw*')):
        try:
            with open(os.path.join(d, 'device', 'uevent')) as f:
                ev = dict(l.strip().split('=', 1) for l in f if '=' in l)
        except OSError:
            continue
        bus, vid, pid = ev.get('HID_ID', '0:0:0').split(':')
        yield '/dev/' + os.path.basename(d), int(bus, 16), int(vid, 16), int(pid, 16), ev.get('HID_NAME', '')


BUS = {3: 'usb', 5: 'bt'}


def sinput_devices():
    return [h for h in hidraws() if (h[2], h[3]) == (0x2E8A, 0x10C6)]


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p / 100 * len(xs)))]


def summary(name, xs):
    print(f'  {name}: n {len(xs)}  min {min(xs):.2f}  median {statistics.median(xs):.2f}  '
          f'mean {statistics.mean(xs):.2f}  p95 {pct(xs, 95):.2f}  max {max(xs):.2f}  ms')


def round_trips(fd, count, cmd):
    req = bytes([0x03, cmd]) + bytes(46)             # output report 0x03: 47 bytes after the ID
    rtts, lost = [], 0
    for i in range(count):
        while select.select([fd], [], [], 0)[0]:     # drain
            os.read(fd, 64)
        t0 = time.perf_counter_ns()
        os.write(fd, req)
        got = None
        while True:
            left = 0.25 - (time.perf_counter_ns() - t0) / 1e9
            if left <= 0 or not select.select([fd], [], [], left)[0]:
                break
            d = os.read(fd, 64)
            if len(d) >= 2 and d[0] == 0x02 and d[1] == cmd:
                got = time.perf_counter_ns()
                break
        if got is None:
            lost += 1
        else:
            rtts.append((got - t0) / 1e6)
        time.sleep(0.02 + (i % 7) * 0.0013)          # don't phase-lock to the pad's tick
    if rtts:
        summary('round trip', rtts)
    if lost:
        print(f'  no reply within 250 ms: {lost} of {count}')
    return rtts


def age_spread(fd, seconds):
    pts = []                                         # (arrival us, pad ts us)
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if not select.select([fd], [], [], 0.5)[0]:
            continue
        d = os.read(fd, 64)
        now = time.perf_counter_ns() / 1000
        if len(d) >= 23 and d[0] == 0x01:
            pts.append((now, struct.unpack_from('<I', d, 19)[0]))
    if len(pts) < 100:
        print('  too few state reports (move the sticks)')
        return
    # unwrap the pad's u32 us timestamp
    ts, off, prev = [], 0, pts[0][1]
    for _, t in pts:
        if t < prev - 2**31:
            off += 2**32
        prev = t
        ts.append(t + off)
    delay = [a - t for (a, _), t in zip(pts, ts)]
    # drift: a line through the per-second minima (the fastest reports)
    t0 = ts[0]
    buckets = {}
    for t, dl in zip(ts, delay):
        k = int((t - t0) // 1e6)
        if k not in buckets or dl < buckets[k][1]:
            buckets[k] = (t, dl)
    xs = [v[0] - t0 for v in buckets.values()]
    ys = [v[1] for v in buckets.values()]
    if len(xs) >= 2:
        mx, my = statistics.mean(xs), statistics.mean(ys)
        sxx = sum((x - mx) ** 2 for x in xs)
        slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else 0
    else:
        slope, mx, my = 0, 0, ys[0]
    resid = [dl - (my + slope * (t - t0 - mx)) for t, dl in zip(ts, delay)]
    base = pct(resid, 1)
    extra = [(r - base) / 1000 for r in resid]
    iv = [(b[0] - a[0]) / 1000 for a, b in zip(pts, pts[1:])]
    print(f'  {len(pts)} state reports in {seconds} s ({len(pts) / seconds:.0f}/s), '
          f'clock drift {slope * 1e6:.0f} ppm')
    summary('interval', iv)
    summary('age above the fastest 1%', extra)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--device')
    ap.add_argument('--name', help='part of the HID name ("C2 receiver" for the receiver)')
    ap.add_argument('--count', type=int, default=300)
    ap.add_argument('--seconds', type=int, default=15, help='for the report age spread')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--method', choices=('auto', 'features', 'ping'), default='auto',
                    help='round trip: features request, or the receiver ping (auto: ping on the receiver)')
    a = ap.parse_args()
    devs = sinput_devices()
    if a.list or not (a.device or devs):
        for dev, bus, vid, pid, name in devs:
            print(f'{dev}  {BUS.get(bus, bus)}  {vid:04x}:{pid:04x}  {name}')
        if not devs:
            print('no SInput device (2E8A:10C6)')
        return
    dev = a.device
    if not dev:
        m = [d for d in devs if not a.name or a.name.lower() in d[4].lower()]
        if len(m) != 1:
            print('pick one with --device or --name:', file=sys.stderr)
            for d, bus, vid, pid, name in devs:
                print(f'  {d}  {BUS.get(bus, bus)}  {name}', file=sys.stderr)
            sys.exit(1)
        dev = m[0][0]
    name = next((d[4] for d in devs if d[0] == dev), '?')
    print(f'{dev}: {name}')
    fd = os.open(dev, os.O_RDWR)
    method = a.method if a.method != 'auto' else ('ping' if 'receiver' in name.lower() else 'features')
    print(f'round trip, {a.count} x {method} (keep the pad still):')
    rtts = round_trips(fd, a.count, 0x8E if method == 'ping' else 0x02)
    if not rtts and method == 'ping':
        print('  (no ping answers: a pad without the link? try --method features)')
    print(f'report age, {a.seconds} s (move the sticks now):')
    age_spread(fd, a.seconds)


if __name__ == '__main__':
    main()
