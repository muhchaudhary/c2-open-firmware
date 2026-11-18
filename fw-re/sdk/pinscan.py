#!/usr/bin/env python3
"""pinscan.py: decode the stock app's inline pin setup.

  pinscan.py STOCK.dis APP.bin [MATCHES.tsv] [--gpio2reg ADDR] [--ramblk ADDR LEN]
                                          [--helpers ADDR=FIELD,...]

The stock code configures pins as `r0 = <pin>; call gpio2reg` (0x1E001BC,
returns the port's register block or 0), then `[r0+off] |= / &= bit` on the
fields OUT +0, DIR +8, DIE +12, PU +16, PD +20, HD0 +24, HD +28, DIEH +32.
Calls to the single-pin helpers (gpio_set_direction etc.) with a constant pin
are decoded too. Each line: function (its first log string, if any), pin,
then the settings in order. APP.bin is the stock app (XIP 0x1E00120), used
to read the log strings.
Defaults are the pad's v3.56 app. The receiver's v1.21 app: --gpio2reg
0x1E021E4 --ramblk 0x1E41B14 0x4F40 --helpers
0x1E021FC=DIR,0x1E02252=PU,0x1E022A4=PD,0x1E022F6=DIE (pin in r0, value in r1).
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


XIP = 0x1E00120
GPIO2REG = 0x1E001BC
HELPERS = {0x1E001F2: 'DIR', 0x1E0027E: 'DIE', 0x1E002FE: 'PU', 0x1E00356: 'PD',
           0x1E003AE: 'OUT+DIR=out', 0x1E00444: 'DIR=in'}
FIELDS = {0: 'OUT', 8: 'DIR', 12: 'DIE', 16: 'PU', 20: 'PD', 24: 'HD0', 28: 'HD', 32: 'DIEH'}
INS = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t?(.*)$')
LOAD = re.compile(r'^r(\d+) = (-?\d+)$')
BR = re.compile(r'^(call|goto) (-?\d+)')
OP = re.compile(r'^\[r(\d+)\+(\d+)\] (\|=|&=) (0x[0-9A-F]+)$')


def pin(v):
    if 0 <= v < 56:
        return 'P%s%02d' % ('ABCD'[v // 16], v % 16)
    return {61: 'USB_DP', 62: 'USB_DM'}.get(v, str(v))


def cstr(app, addr):
    o = addr - XIP
    if not 0 <= o < len(app):
        return None
    e = app.find(b'\0', o, o + 120)
    s = app[o:e] if e > o else b''
    if len(s) >= 4 and all(32 <= c < 127 or c in (9, 10, 13) for c in s):
        return s.decode().strip()
    return None


def main():
    global GPIO2REG, HELPERS, RAMBLK, RAMLEN
    args = sys.argv[1:]
    if '--gpio2reg' in args:
        i = args.index('--gpio2reg'); GPIO2REG = int(args[i + 1], 0); HELPERS = {}
        del args[i:i + 2]
    if '--ramblk' in args:
        i = args.index('--ramblk'); RAMBLK, RAMLEN = int(args[i + 1], 0), int(args[i + 2], 0)
        del args[i:i + 3]
    if '--helpers' in args:
        i = args.index('--helpers')
        HELPERS = {int(k, 0): v for k, v in (kv.split('=') for kv in args[i + 1].split(','))}
        del args[i:i + 2]
    sys.argv[1:] = args
    app = open(sys.argv[2], 'rb').read()
    names = {}
    if len(sys.argv) > 3:
        for line in open(sys.argv[3]):
            a, _, _, _, n = line.rstrip('\n').split('\t')
            names[int(a, 16)] = n
    ins = [(int(m[1], 16), len(m[2].split()), m[3].split('#')[0].split(' <')[0].strip())
           for m in map(INS.match, open(sys.argv[1], errors='replace')) if m]
    func, label = None, ''
    i = 0
    while i < len(ins):
        a, n, t = ins[i]
        if t.startswith('[--sp] = {rets') or a in names:
            func, label = a, names.get(a, '')
        lm = LOAD.match(t)
        if lm and 0 <= int(lm[2]) < 63 and i + 1 < len(ins):
            b, bn, bt = ins[i + 1]
            bm = BR.match(bt)
            if bm and branch_target(b, bn, int(bm[2])) == GPIO2REG and lm[1] == '0':
                p, ops, j = int(lm[2]), [], i + 2
                reg = 0
                while j < len(ins) and j < i + 40:
                    tj = ins[j][2]
                    if tj.startswith(('call', 'rts', 'pc =', '{pc', '[--sp]')):
                        break
                    if re.match(r'^r0 = ', tj) and 'r0 = [r' not in tj and 'r0 = r0' not in tj:
                        break
                    om = OP.match(tj)
                    if om and om[1] == '0' or (om and om[1] == str(reg)):
                        f = FIELDS.get(int(om[2]), '+%s' % om[2])
                        mask = int(om[4], 16)
                        bit = 1 << (p % 16)
                        if om[3] == '|=' and mask == bit:
                            ops.append(f + '=1')
                        elif om[3] == '&=' and mask == (~bit & 0xffffffff):
                            ops.append(f + '=0')
                        else:
                            ops.append('%s%s%s' % (f, om[3], om[4]))
                    mv = re.match(r'^r(\d+) = r0$', tj)
                    if mv:
                        reg = int(mv[1])
                    j += 1
                print('%08x  %-24s %-7s %s' % (a, label or ('fn_%x' % func if func else '?'),
                                              pin(p), ' '.join(ops)))
        bm = BR.match(t)
        if bm and branch_target(a, n, int(bm[2])) in HELPERS:
            # constant pin in r0 just before?
            for k in range(i - 1, max(i - 6, -1), -1):
                km = LOAD.match(ins[k][2])
                if km and km[1] == '0':
                    r1 = None
                    for q in range(i - 1, k, -1):
                        qm = LOAD.match(ins[q][2])
                        if qm and qm[1] == '1':
                            r1 = qm[2]
                            break
                    print('%08x  %-24s %-7s %s(%s)' % (a, label or ('fn_%x' % func if func else '?'),
                                                       pin(int(km[2])), HELPERS[branch_target(a, n, int(bm[2]))], r1))
                    break
        if lm and lm[1] == '0':
            s = cstr(app, int(lm[2]) & 0xffffffff)
            if s and func and not label.startswith('"'):
                label = '"%s"' % s[:22]
        i += 1


if __name__ == '__main__':
    main()
