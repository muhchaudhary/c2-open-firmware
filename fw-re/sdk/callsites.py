#!/usr/bin/env python3
"""callsites.py: list the call sites of given stock functions with the
constant arguments set up before each call.

  callsites.py STOCK.dis [--raw] ADDR=NAME [ADDR=NAME ...]

For each `call`/`goto` (tail call) to ADDR, walks back up to 12 instructions
from the call and reports the last constant loaded into r0..r3 there
(`rN = imm`, `rN = rM`-copies of a known constant are followed too). A
register the walk can't resolve prints as `?`. Pin numbers are decoded as
IO_PORTx_nn (br23: A 0..15, B 16..31, C 32..47, D 48..55, USB DP/DM 61/62)
unless --raw is given.
"""
import re
import sys

RAMBLK, RAMLEN = 0x1E67C78, 0xB7D1   # boot-RAM block: listing address <-> RAM 0..RAMLEN


def branch_target(a, n, off):
    """Target of a PC-relative call/goto at listing address a (length n).
    Code in the boot-RAM block runs at RAM a - RAMBLK; a negative target
    (RAM code calling flash) wraps at 25 bits; a target in RAM 0..RAMLEN is
    given as its listing address, and so is a target in 0x2000000 + RAM
    (the "OS core" calls are this boot-RAM code through that alias)."""
    pc = a - RAMBLK if a >= RAMBLK else a
    t = pc + n + off
    if t < 0:
        t &= 0x1FFFFFF
    if 0x2000000 <= t < 0x2000000 + RAMLEN:
        t -= 0x2000000
    return t + RAMBLK if t < RAMLEN else t


INS = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t?(.*)$')
BR = re.compile(r'^(call|goto) (-?\d+)')
LOAD = re.compile(r'^r([0-3]) = (-?\d+)$')
COPY = re.compile(r'^r([0-3]) = r(\d+)$')
WRITES = re.compile(r'^(?:\{?)r(\d+)(?:_r\d+)? (?:[-+*/&|^]|<<|>>)?=')


def pin(v):
    if v is None:
        return '?'
    if 0 <= v < 56:
        return 'P%s%02d' % ('ABCD'[v // 16], v % 16)
    return {61: 'USB_DP', 62: 'USB_DM'}.get(v, str(v))


PINS = True


def main():
    global PINS
    targets = {}
    if '--raw' in sys.argv:
        PINS = False
        sys.argv.remove('--raw')
    for a in sys.argv[2:]:
        k, v = a.split('=')
        targets[int(k, 16)] = v
    ins = []
    for line in open(sys.argv[1], errors='replace'):
        m = INS.match(line)
        if m:
            ins.append((int(m[1], 16), len(m[2].split()), m[3].split('#')[0].strip()))
    for i, (a, n, t) in enumerate(ins):
        m = BR.match(t)
        if not m or branch_target(a, n, int(m[2])) not in targets:
            continue
        name = targets[branch_target(a, n, int(m[2]))]
        regs, known = {}, {}
        for j in range(i - 1, max(i - 13, -1), -1):
            tj = ins[j][2]
            if tj.startswith(('call', 'rts', 'pc =', '{pc')):
                break
            lm, cm, wm = LOAD.match(tj), COPY.match(tj), WRITES.match(tj)
            if lm and int(lm[1]) not in regs:
                regs[int(lm[1])] = int(lm[2])
            elif cm and int(cm[1]) not in regs:
                regs[int(cm[1])] = ('r', int(cm[2]))
            elif wm and int(wm[1]) < 4 and int(wm[1]) not in regs:
                regs[int(wm[1])] = None
            if lm and int(lm[1]) >= 4:
                known.setdefault(int(lm[1]), int(lm[2]))
            for k in range(4, 15):
                mm = re.match(r'^r%d = (-?\d+)$' % k, tj)
                if mm:
                    known.setdefault(k, int(mm[1]))
        def val(r):
            v = regs.get(r)
            if isinstance(v, tuple):
                return known.get(v[1])
            return v
        r0, r1, r2, r3 = val(0), val(1), val(2), val(3)
        q = lambda v: '?' if v is None else (hex(v) if v > 9 else str(v))
        r0s = pin(r0) if PINS else q(r0)
        print(f'{a:08x}  {m[1]:4} {name:24} r0={r0s:7} r1={q(r1):<6} r2={q(r2):<6} r3={q(r3)}')


if __name__ == '__main__':
    main()
