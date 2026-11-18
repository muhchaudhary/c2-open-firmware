#!/usr/bin/env python3
"""c2si_dump.py: show the C2's SInput reports live (this project's firmware).

  sudo tools/c2si_dump.py [--device /dev/hidrawN] [--rate HZ]

Decodes input report 0x01 (docs/sinput.md 2): plug status [1], battery %
[2], the three button bytes [3..5], charging [6] bit 0, sticks LX LY RX RY
and triggers LT RT [7..18], the timestamp in us [19..22], accel and gyro X
Y Z [23..34]. Prints a line HZ times a second (10) and counts the reports
in between, so the rate shows too. Ctrl-C to stop.

The firmware's own diagnostics (stick errors, stalls, send failures) are in
its USB debug log: tools/c2log.py.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import c2led  # noqa: E402  (find_hidraw)

PLUG = {0: '?', 2: 'charging', 3: 'full', 4: 'battery'}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device', help='hidraw node (default: find 2E8A:10C6)')
    ap.add_argument('--rate', type=float, default=10, help='lines per second')
    a = ap.parse_args()
    dev = a.device or c2led.find_hidraw()
    if not dev:
        sys.exit('C2 not found: is it connected (SInput, 2E8A:10C6)?')
    fd = os.open(dev, os.O_RDONLY)
    print(f'{dev}: buttons(3) LX LY RX RY LT RT | plug bat% chg | aX aY aZ gX gY gZ | ts_ms | reports/s')
    n, last = 0, time.time()
    while True:
        r = os.read(fd, 64)
        if not r or r[0] != 0x01 or len(r) < 35:
            continue
        n += 1
        now = time.time()
        if now - last < 1 / a.rate:
            continue
        lx, ly, rx, ry, lt, rt = struct.unpack_from('<6h', r, 7)
        ts = struct.unpack_from('<I', r, 19)[0]
        imu = struct.unpack_from('<6h', r, 23)
        print(f'{r[3]:08b} {r[4]:08b} {r[5]:08b} {lx:6} {ly:6} {rx:6} {ry:6} {lt:6} {rt:6} '
              f'| {PLUG.get(r[1], r[1])} {r[2]:3}% {r[6] & 1} '
              f'| {" ".join(f"{v:6}" for v in imu)} '
              f'| {ts // 1000:8} '
              f'| {n / (now - last):4.0f}', flush=True)
        n, last = 0, now


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
