#!/usr/bin/env python3
"""sfrscan.py: find every peripheral-register access in the stock app.

  sfrscan.py BR23_H STOCK.dis MATCHES.tsv OUT.tsv

BR23_H is the SDK's include_lib/driver/cpu/br23/asm/br23.h: its
`typedef struct {...} X_TypeDef;` blocks and `#define JL_Y_BASE (...)`
give an address -> `JL_Y->FIELD` map. In STOCK.dis, a `rN = <imm>` that
lands in a register block starts tracking rN until it is overwritten or the
block ends (a call, a return); each `[rN+off]` / `b[rN+off]` / `h[rN+off]`
then is an access to that block's field at off, and `rM = rN + imm` derives
a new tracked base. The containing function is the nearest preceding name
in MATCHES.tsv (sdkmatch.py) or the nearest prologue.

OUT.tsv: address, function, register, access (the instruction text).
"""
import bisect
import re
import sys


def parse_header(path):
    src = open(path, errors='replace').read()
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)
    structs = {}
    for m in re.finditer(r'typedef\s+struct\s*\{(.*?)\}\s*(\w+)\s*;', src, re.S):
        off, fields = 0, []
        for fm in re.finditer(r'(__u8|__u16|__u32|u8|u16|u32)\s+(\w+)\s*(?:\[(\w+)\])?\s*;', m[1]):
            size = {'8': 1, '16': 2, '32': 4}[re.sub(r'\D', '', fm[1])]
            n = int(fm[3], 0) if fm[3] else 1
            off = (off + size - 1) // size * size
            fields.append((off, fm[2], size * n))
            off += size * n
        structs[m[2]] = fields
    env = {'ls_base': 0x1E0000, 'hs_base': 0x1F0000,
           'map_adr': lambda g, a: (64 * g + a) * 4}
    bases = {}
    for m in re.finditer(r'#define\s+(\w+_BASE)\s+(\(.*)', src):
        expr = m[2].strip()
        try:
            env[m[1]] = eval(re.sub(r'(0x[0-9a-fA-F]+)[uUlL]*', r'\1', expr), {}, env)
            bases[m[1]] = env[m[1]]
        except Exception:
            pass
    blocks = []      # (base, size, name, fields)
    for m in re.finditer(r'#define\s+(JL_\w+)\s+\(\(\s*(\w+)\s*\*\s*\)\s*(\w+)\s*\)', src):
        name, typ, base = m[1], m[2], m[3]
        if base in bases and typ in structs:
            f = structs[typ]
            size = (f[-1][0] + f[-1][2]) if f else 4
            blocks.append((bases[base], size, name, f))
    blocks.sort()
    return blocks


def main():
    blocks = parse_header(sys.argv[1])
    starts = [b[0] for b in blocks]

    def block_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < blocks[i][0] + blocks[i][1]:
            return blocks[i]
        return None

    def regname(addr):
        b = block_of(addr)
        if not b:
            return None
        rel = addr - b[0]
        for off, f, size in b[3]:
            if off <= rel < off + size:
                return '%s->%s%s' % (b[2], f, '' if rel == off else '+%d' % (rel - off))
        return '%s+%d' % (b[2], rel)

    names = {}
    for line in open(sys.argv[3]):
        a, how, r, L, n = line.rstrip('\n').split('\t')
        names[int(a, 16)] = n
    ins = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t?(.*)$')
    load = re.compile(r'^r(\d+) = (-?\d+)$')
    derive = re.compile(r'^r(\d+) = r(\d+) \+ (-?(?:0x[0-9A-Fa-f]+|\d+))$')
    mem = re.compile(r'(?:[bh])?\[r(\d+)\+(-?\d+)\]')
    dest = re.compile(r'^\{?r(\d+)')
    track = {}
    func, fname = 0, '?'
    out = open(sys.argv[4], 'w')
    count = 0
    for line in open(sys.argv[2], errors='replace'):
        m = ins.match(line)
        if not m:
            continue
        a, t = int(m[1], 16), m[3].split('#')[0].split(' <')[0].strip()
        if a in names:
            func, fname = a, names[a]
        elif t.startswith('[--sp] = {rets'):
            func, fname = a, 'fn_%x' % a
        if t.startswith(('call', 'rts', 'pc = [sp++]', '{pc')) or t.startswith('goto'):
            for mm in mem.finditer(t):
                pass
            track = {} if not t.startswith('call') else {k: v for k, v in track.items() if k >= 4}
            continue
        for mm in mem.finditer(t):
            r, off = int(mm[1]), int(mm[2])
            if r in track:
                rn = regname(track[r] + off)
                if rn:
                    out.write(f'{a:08x}\t{fname}\t{rn}\t{t}\n')
                    count += 1
        lm, dm, wm = load.match(t), derive.match(t), dest.match(t)
        if lm:
            v = int(lm[2]) & 0xffffffff
            if block_of(v):
                track[int(lm[1])] = v
                out.write(f'{a:08x}\t{fname}\t{regname(v)}\t(base) {t}\n')
            else:
                track.pop(int(lm[1]), None)
        elif dm and int(dm[2]) in track:
            v = track[int(dm[2])] + int(dm[3], 0)
            if block_of(v):
                track[int(dm[1])] = v
            else:
                track.pop(int(dm[1]), None)
        elif wm and not t.startswith('[') and '] =' not in t.split('=')[0]:
            track.pop(int(wm[1]), None)
    print(f'{len(blocks)} register blocks; {count} accesses written to {sys.argv[4]}')


if __name__ == '__main__':
    main()
