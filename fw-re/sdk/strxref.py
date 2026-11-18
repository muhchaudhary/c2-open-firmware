#!/usr/bin/env python3
"""strxref.py: find the code that loads given strings in the stock app.

  strxref.py STOCK.dis APP.bin TEXT [TEXT ...]

Strings are often reached as base + offset (`r6 = 0x1E5BF70` once, then
`r0 = r6 + 1427`, or `r0 = 8424; r0 += r6`), so this tracks constant
registers through a function (reset at each prologue) and reports every
instruction whose result is the address of one of the strings.
"""
import re
import sys

XIP = 0x1E00120
INS = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t?(.*)$')


def main():
    app = open(sys.argv[2], 'rb').read()
    want = {}
    for text in sys.argv[3:]:
        for m in re.finditer(re.escape(text.encode()), app):
            want[m.start() + XIP] = text
    regs = {}
    for line in open(sys.argv[1], errors='replace'):
        m = INS.match(line)
        if not m:
            continue
        a, t = int(m[1], 16), m[3].split('#')[0].split(' <')[0].strip()
        if t.startswith('[--sp] = {rets'):
            regs = {}
        v = None
        mm = re.match(r'^r(\d+) = (-?\d+)$', t)
        if mm:
            d, v = int(mm[1]), int(mm[2]) & 0xffffffff
        mm = mm or None
        m2 = re.match(r'^r(\d+) = r(\d+) \+ (-?\d+)$', t)
        if m2 and int(m2[2]) in regs:
            d, v = int(m2[1]), (regs[int(m2[2])] + int(m2[3])) & 0xffffffff
        m3 = re.match(r'^r(\d+) \+= r(\d+)$', t)
        if m3 and int(m3[1]) in regs and int(m3[2]) in regs:
            d, v = int(m3[1]), (regs[int(m3[1])] + regs[int(m3[2])]) & 0xffffffff
        if v is not None:
            regs[d] = v
            if v in want:
                print(f'{a:08x}  {want[v]!r}')
        else:
            md = re.match(r'^r(\d+) ', t)
            if md and '=' in t.split(' ', 2)[1:2][0] if len(t.split(' ')) > 1 else False:
                regs.pop(int(md[1]), None)


if __name__ == '__main__':
    main()
