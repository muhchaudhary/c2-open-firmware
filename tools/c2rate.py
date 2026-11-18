#!/usr/bin/env python3
"""c2rate.py: measure a gamepad's input report rate and jitter (any firmware).

  sudo tools/c2rate.py [--device /dev/hidrawN | --id VID:PID] [--seconds 20]
  sudo tools/c2rate.py --list

Reads raw input reports from hidraw and times their arrival on the host, so
it works the same for stock and our firmware, over Bluetooth or USB, in any
mode (SInput, Xbox): the numbers are directly comparable. Keep the pad
moving (sticks, gyro) while it runs, since a pad that only reports on change
looks slow when idle. Prints a summary every 5 s and a histogram at the end:
reports/s, interval mean / median / p95 / p99 / max, and how many gaps were
longer than 15, 30 and 100 ms (stutter).
"""
import argparse
import glob
import os
import statistics
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
KNOWN = {(0x2E8A, 0x10C6), (0x045E, 0x02FD)}   # SInput, Xbox One S BT


def pick(want):
    for dev, bus, vid, pid, name in hidraws():
        if want and (vid, pid) == want:
            return dev
        if not want and ((vid, pid) in KNOWN or 'gamesir' in name.lower()):
            return dev
    return None


def pct(sorted_v, p):
    return sorted_v[min(len(sorted_v) - 1, int(len(sorted_v) * p))]


def summary(iv, secs):
    if len(iv) < 2:
        return 'no reports'
    s = sorted(iv)
    return (f'{len(iv) / secs:6.1f}/s  mean {statistics.fmean(iv):5.2f}  med {pct(s, .5):5.2f}  '
            f'p95 {pct(s, .95):5.2f}  p99 {pct(s, .99):6.2f}  max {s[-1]:6.1f} ms  '
            f'gaps >15:{sum(v > 15 for v in iv)} >30:{sum(v > 30 for v in iv)} >100:{sum(v > 100 for v in iv)}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device', help='hidraw node')
    ap.add_argument('--id', help='VID:PID in hex (default: a known C2 identity)')
    ap.add_argument('--seconds', type=float, default=20)
    ap.add_argument('--report-id', type=lambda x: int(x, 0), help='count only this report ID (first byte)')
    ap.add_argument('--list', action='store_true', help='list hidraw devices')
    a = ap.parse_args()
    if a.list:
        for dev, bus, vid, pid, name in hidraws():
            print(f'{dev}  {BUS.get(bus, bus)}  {vid:04X}:{pid:04X}  {name}')
        return
    want = tuple(int(x, 16) for x in a.id.split(':')) if a.id else None
    dev = a.device or pick(want)
    if not dev:
        sys.exit('no gamepad found: try --list, then --device or --id')
    fd = os.open(dev, os.O_RDONLY)
    print(f'{dev}: move the sticks; {a.seconds:g} s')
    iv, window, last = [], [], None
    t0 = t_win = time.monotonic()
    while time.monotonic() - t0 < a.seconds:
        r = os.read(fd, 128)
        now = time.monotonic()
        if not r or (a.report_id is not None and r[0] != a.report_id):
            continue
        if last is not None:
            d = (now - last) * 1000
            iv.append(d)
            window.append(d)
        last = now
        if now - t_win >= 5:
            print('  5 s: ' + summary(window, now - t_win))
            window, t_win = [], now
    total = time.monotonic() - t0
    print('total: ' + summary(iv, total))
    if iv:
        edges = [2, 4, 6, 8, 10, 12, 15, 20, 30, 50, 100, 1e9]
        counts = [0] * len(edges)
        for v in iv:
            counts[next(i for i, e in enumerate(edges) if v <= e)] += 1
        lo = 0
        for e, c in zip(edges, counts):
            label = f'{lo:g}-{e:g} ms' if e < 1e9 else f'>{lo:g} ms'
            print(f'  {label:>12} {c:6d} {"#" * int(60 * c / len(iv))}')
            lo = e


if __name__ == '__main__':
    main()
