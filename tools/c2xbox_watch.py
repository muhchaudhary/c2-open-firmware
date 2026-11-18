#!/usr/bin/env python3
"""c2xbox_watch.py: timestamp when each input starts to change, Xbox Bluetooth mode.

  tools/c2xbox_watch.py [--device /dev/hidrawN] [--seconds 30]

Decodes report 1 of the Xbox One S Bluetooth layout (045E:02FD, what the
rewrite and stock send in Xbox mode; src/core/gp_xboxbt.c): sticks u16,
triggers 10 bits, hat, buttons, View. Prints a line, with the host time in
ms, whenever an input starts moving (sticks / triggers past a small
threshold from rest) or a button / d-pad changes, and report 2 (the Xbox
button).

Latency test: buttons are read raw every tick, so they are the reference.
Press a button and an analog input at the same moment, e.g. A together
with RT, or slap LB and LT down with one finger: the "+N ms after <button>"
on the analog line is how much later that input showed up. Repeat ~10 times.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import c2rate  # noqa: E402  (hidraws)

BTN = {0: 'A', 1: 'B', 3: 'X', 4: 'Y', 6: 'LB', 7: 'RB', 11: 'Menu', 13: 'LS', 14: 'RS'}
HAT = ['-', 'Up', 'UpRight', 'Right', 'DownRight', 'Down', 'DownLeft', 'Left', 'UpLeft']
STICK_MOVE = 4000      # of 32768 from centre
TRIG_MOVE = 40         # of 1023


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device')
    ap.add_argument('--seconds', type=float, default=30)
    a = ap.parse_args()
    dev = a.device
    if not dev:
        dev = next((d for d, bus, vid, pid, n in c2rate.hidraws() if (vid, pid) == (0x045E, 0x02FD)), None)
    if not dev:
        sys.exit('no 045E:02FD (Xbox mode) hidraw found: c2rate.py --list')
    fd = os.open(dev, os.O_RDONLY)
    print(f'{dev}: press a button together with a trigger or stick; {a.seconds:g} s')
    t0 = time.monotonic()
    last_btn_t, last_btn = None, ''
    prev = None
    moving = {}
    while time.monotonic() - t0 < a.seconds:
        r = os.read(fd, 64)
        now = (time.monotonic() - t0) * 1000
        if r[0] == 2 and len(r) >= 2:
            print(f'{now:9.1f}  Xbox button {"down" if r[1] & 1 else "up"}')
            if r[1] & 1:
                last_btn_t, last_btn = now, 'Xbox'
            continue
        if r[0] != 1 or len(r) < 17:
            continue
        lx, ly, rx, ry, lt, rt = struct.unpack_from('<4H2H', r, 1)
        lt &= 0x3FF
        rt &= 0x3FF
        hat = r[13] & 0x0F
        btn, = struct.unpack_from('<H', r, 14)
        view = r[16] & 1
        cur = {'LX': abs(lx - 32768) > STICK_MOVE, 'LY': abs(ly - 32768) > STICK_MOVE,
               'RX': abs(rx - 32768) > STICK_MOVE, 'RY': abs(ry - 32768) > STICK_MOVE,
               'LT': lt > TRIG_MOVE, 'RT': rt > TRIG_MOVE}
        if prev is not None:
            pbtn, phat, pview = prev
            for bit, name in BTN.items():
                if (btn ^ pbtn) >> bit & 1:
                    down = btn >> bit & 1
                    print(f'{now:9.1f}  {name} {"down" if down else "up"}')
                    if down:
                        last_btn_t, last_btn = now, name
            if hat != phat:
                print(f'{now:9.1f}  d-pad {HAT[hat] if hat < 9 else hat}')
                if hat:
                    last_btn_t, last_btn = now, 'd-pad'
            if view != pview:
                print(f'{now:9.1f}  View {"down" if view else "up"}')
            for k, v in cur.items():
                if v and not moving.get(k):
                    rel = f'   +{now - last_btn_t:.1f} ms after {last_btn}' if last_btn_t is not None and now - last_btn_t < 300 else ''
                    print(f'{now:9.1f}  {k} moving{rel}')
                elif not v and moving.get(k):
                    print(f'{now:9.1f}  {k} back')
        moving = cur
        prev = (btn, hat, view)


if __name__ == '__main__':
    main()
