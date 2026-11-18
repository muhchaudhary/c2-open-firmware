#!/usr/bin/env python3
"""c2batlog.py: log the C2's battery to a CSV, for the voltage curve.

  tools/c2batlog.py [--every S] [--out FILE] [--device /dev/hidrawN]

Asks the pad for its INFO (tools/c2cfg.py's config protocol) every S
seconds (default 10) over whatever link it is on: Bluetooth, the receiver
or USB, in Steam mode. One row each: PC time, seconds since the start,
battery mV (the pad's running average), percent, plug (2 charging, 3 full,
4 on battery), PB02 (the possible charger status pin), USB 5 V, docked.
Default file: firmware/build/batlog-<date>.csv. Ctrl-C to stop; a pad that
stops answering (off, out of range) is waited for.

For a discharge curve turn the idle power-off off first
(`tools/c2cfg.py set idle_off_min 0`, check the name with `list`), charge
to full, unplug, and let it run to the low-battery power-off. A pad off on
the cable or the dock (charging mode) has no gamepad to ask: read its
"charging: bat .." lines with tools/c2log.py instead.
"""
import argparse
import datetime
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import c2cfg  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--every', type=float, default=10)
    ap.add_argument('--out')
    ap.add_argument('--device')
    a = ap.parse_args()
    out = a.out or os.path.join(ROOT, 'firmware', 'build',
                                'batlog-' + datetime.datetime.now().strftime('%Y%m%d-%H%M') + '.csv')
    new = not os.path.exists(out)
    f = open(out, 'a')
    if new:
        f.write('time,t_s,mv,percent,plug,pb02,usb5v,docked\n')
    print(f'logging to {out}', file=sys.stderr)
    t0 = time.monotonic()
    pad = None
    while True:
        try:
            if pad is None:
                dev = a.device or c2cfg.find_device()
                if not dev:
                    raise SystemExit('no pad')
                pad = c2cfg.Pad(dev)
            p = pad.request(c2cfg.OP_INFO)
        except (SystemExit, OSError) as e:
            print(f'{datetime.datetime.now():%H:%M:%S} no answer ({e}), waiting', file=sys.stderr)
            pad = None
            time.sleep(a.every)
            continue
        mv = p[35] | p[36] << 8
        pb02 = p[41] if len(p) > 42 else 255
        pw = p[42] if len(p) > 42 else 0
        row = (f'{datetime.datetime.now():%Y-%m-%d %H:%M:%S},{time.monotonic() - t0:.0f},{mv},{p[37]},'
               f'{p[38]},{"" if pb02 == 255 else pb02},{pw & 1},{pw >> 1 & 1}')
        f.write(row + '\n')
        f.flush()
        print(row)
        time.sleep(a.every)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
