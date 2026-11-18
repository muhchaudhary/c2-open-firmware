#!/usr/bin/env python3
"""c2eeprom.py: read the C2 pad's I2C EEPROM (0xA0, 8 KB) through its USB
debug log, and save it (read-only: nothing is written to the EEPROM).

  tools/c2eeprom.py [--device /dev/ttyACMn] [--out FILE]

Sends the log command "eeprom" (firmware/boards/c2/gamepad_c2.c), collects
the "ee: AAAA: <hex>" lines, checks the firmware's byte sum, asks again for
any range that went missing, and writes the 8192 bytes to --out (default
backups/c2-pad-eeprom.bin; backups/ is gitignored: device-specific data).

The pad must be connected to a host (Bluetooth, receiver or USB gamepad): the
dump runs in the report tick, which runs only then. Stop tools/c2log.py
first: two readers on one port each get part of the lines.

Finds the pad's port by its USB product name ("Cyclone"), not by ID: the
receiver uses the same 2E8A:10C6.
"""
import argparse
import glob
import os
import re
import select
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from c2log import open_port, ROOT  # noqa: E402

SIZE = 0x2000
LINE = re.compile(r'ee: ([0-9A-F]{4}): ([0-9A-F]+)\s*$')
FAIL = re.compile(r'ee: ([0-9A-F]{4}): read failed')
DONE = re.compile(r'ee: done ([0-9A-F]{4})\+([0-9A-F]{4}) sum ([0-9A-F]+) failed (\d+)')


def find_pad_port():
    for tty in sorted(glob.glob('/sys/class/tty/ttyACM*')):
        p = os.path.realpath(os.path.join(tty, 'device'))
        while p != '/':
            try:
                product = open(os.path.join(p, 'product')).read().strip()
            except OSError:
                p = os.path.dirname(p)
                continue
            if 'Cyclone' in product:
                return '/dev/' + os.path.basename(tty)
            break
    return None


def dump(fd, start, length, timeout):
    """Ask for start+length; return ({addr: bytes}, done-line or None)."""
    os.write(fd, f'\neeprom {start:04X} {length:04X}\n'.encode())
    got, done, buf = {}, None, b''
    end = time.time() + timeout
    while time.time() < end and not done:
        r, _, _ = select.select([fd], [], [], 0.5)
        if not r:
            continue
        buf += os.read(fd, 4096)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            s = line.decode('utf-8', 'replace').rstrip('\r')
            m = LINE.search(s)
            if m:
                got[int(m.group(1), 16)] = bytes.fromhex(m.group(2))
                n = len(got)
                if n % 32 == 0:
                    print(f'  {n * 32} bytes...', flush=True)
            elif FAIL.search(s):
                print('  ' + s.strip())
            elif DONE.search(s):
                done = DONE.search(s)
            elif 'cmd: eeprom' in s:
                print('  ' + s.strip())
    return got, done


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device', help='tty (default: the one whose USB product says "Cyclone")')
    ap.add_argument('--out', default=os.path.join(ROOT, 'backups/c2-pad-eeprom.bin'))
    a = ap.parse_args()

    dev = a.device or find_pad_port()
    if not dev:
        sys.exit('pad debug log port not found: is the pad on USB with this project firmware?')
    print(f'{dev}: dumping the EEPROM (~10 s)')
    fd = open_port(dev)
    time.sleep(0.5)
    while select.select([fd], [], [], 0.2)[0]:          # drop what was queued before
        os.read(fd, 4096)

    data = bytearray(b'\xff' * SIZE)
    have = bytearray(SIZE)                              # 1 where the byte was received
    got, done = dump(fd, 0, SIZE, 40)
    if not done:
        sys.exit('no "ee: done" line: is the pad connected to a host? (the dump runs only then)')
    for addr, b in got.items():
        data[addr:addr + len(b)] = b
        have[addr:addr + len(b)] = b'\x01' * len(b)
    sum_ok = sum(b for chunks in got.values() for b in chunks) == int(done.group(3), 16)

    for attempt in range(3):                            # ask again for whatever is missing
        gaps, i = [], 0
        while i < SIZE:
            if have[i]:
                i += 1
                continue
            j = i
            while j < SIZE and not have[j]:
                j += 1
            gaps.append((i, j - i))
            i = j
        if not gaps:
            break
        print(f'  missing {sum(n for _, n in gaps)} bytes in {len(gaps)} ranges: asking again')
        for start, n in gaps:
            g, _ = dump(fd, start, n, 10 + n // 100)
            for addr, b in g.items():
                data[addr:addr + len(b)] = b
                have[addr:addr + len(b)] = b'\x01' * len(b)
    os.close(fd)

    missing = SIZE - sum(have)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    open(a.out, 'wb').write(data)
    used = sum(1 for b in data if b != 0xff)
    print(f'saved {a.out}: {SIZE} bytes, {used} not 0xFF, {missing} missing (left 0xFF), '
          f'first pass sum {"ok" if sum_ok else "MISMATCH"}, read failures {done.group(4)}')
    half = SIZE // 2
    if data[:half] == data[half:]:
        print('note: the two 4 KB halves are identical: the chip may be a 24C32 (it wraps at 4 KB)')
    if missing:
        sys.exit(1)


if __name__ == '__main__':
    main()
