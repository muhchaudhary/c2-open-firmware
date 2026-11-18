#!/usr/bin/env python3
"""sdkmatch.py: name functions in the stock C2 app by matching them against a
JieLi SDK build (fw-AC63_BT_SDK, cpu/br23/tools/sdk.elf).

  sdkmatch.py SDK.dis STOCK.dis OUT.tsv [--ramblk ADDR LEN]

Both inputs are `objdump -d --triple=pi32v2` listings: sdk.elf with its
symbols, the stock app wrapped in an ELF at 0x1E00120. Instructions are
compared with every constant and branch offset masked.

1. Body match: a function's first K instructions anchor candidate spots in
   stock; the best spot where >= RATIO of its instructions match is taken
   (ties between spots are skipped, they say nothing).
2. Call propagation: in a body match, a `call` at the same index in both
   gives the stock callee the SDK callee's name. Repeated until nothing new
   is found. A callee named this way is checked against its SDK body too,
   and kept with that ratio (low ratios mean a changed function, or a call
   that only lines up by luck: treat them as hints).

OUT.tsv: stock address, how (body / call), ratio, SDK length, SDK name.
--ramblk: the stock image's boot-RAM block (listing address, length), from
its startup code's second copy loop (`r1 = src`, `r2 = len`, to RAM 0).
Default: the pad's v3.56 app, 0x1E67C78 0xB7D1; the dongle's v1.21 app is
0x1E41B14 0x4F40.
"""
import collections
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


K, RATIO = 8, 0.85
INS = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t?(.*)$')
FUNC = re.compile(r'^([A-Za-z_.$][\w.$]*):$')
CALL = re.compile(r'^call (-?\d+)')


def norm(t):
    t = t.split(' <')[0].split('#')[0].strip()
    t = re.sub(r'-?0x[0-9a-fA-F]+|-?\b\d+\b', 'N', t)
    return re.sub(r'\s+', ' ', t)


def parse(path, stock=False):
    """-> list of (addr, norm, call_target or None), {func name: index}"""
    ins, funcs = [], {}
    for line in open(path, errors='replace'):
        m = FUNC.match(line.strip())
        if m and not line.startswith(' '):
            funcs.setdefault(m[1], len(ins))
            continue
        m = INS.match(line)
        if not m:
            continue
        a, n, text = int(m[1], 16), len(m[2].split()), m[3].strip()
        c = CALL.match(text)
        ins.append((a, norm(text), (branch_target(a, n, int(c[1])) if stock else a + n + int(c[1])) if c else None))
    return ins, funcs


def main():
    global RAMBLK, RAMLEN
    if '--ramblk' in sys.argv:
        i = sys.argv.index('--ramblk')
        RAMBLK, RAMLEN = int(sys.argv[i + 1], 0), int(sys.argv[i + 2], 0)
    sdk, sfun = parse(sys.argv[1])
    stock, _ = parse(sys.argv[2], stock=True)
    s_at = {a: i for i, (a, _, _) in enumerate(stock)}
    sdk_at = {sdk[i][0]: name for name, i in sfun.items()}
    # function bodies: from a symbol to the next one
    starts = sorted(sfun.items(), key=lambda kv: kv[1])
    body = {}
    for (name, i), nxt in zip(starts, starts[1:] + [(None, len(sdk))]):
        if nxt[1] > i:
            body[name] = (i, nxt[1] - i)
    ops = [n for _, n, _ in stock]
    idx = collections.defaultdict(list)
    for i in range(len(ops) - K):
        idx[tuple(ops[i:i + K])].append(i)

    def ratio(name, si):
        i, L = body[name]
        if si + L > len(ops):
            return 0.0
        return sum(ops[si + j] == sdk[i + j][1] for j in range(L)) / L

    found = {}          # stock index -> (how, ratio, name)
    named = set()
    for name, (i, L) in body.items():
        if L < K:
            continue
        cands = idx.get(tuple(n for _, n, _ in sdk[i:i + K]), [])
        scored = sorted(((ratio(name, si), si) for si in cands), reverse=True)
        if scored and scored[0][0] >= RATIO and (len(scored) == 1 or scored[1][0] < scored[0][0]):
            r, si = scored[0]
            if si not in found:
                found[si] = ('body', r, name)
                named.add(name)

    work = [si for si, v in found.items() if v[0] == 'body']
    while work:
        si = work.pop()
        name = found[si][2]
        i, L = body[name]
        for j in range(L):
            tgt_sdk, tgt_stock = sdk[i + j][2], stock[si + j][2] if si + j < len(stock) else None
            if tgt_sdk is None or tgt_stock is None or ops[si + j] != sdk[i + j][1]:
                continue
            callee = sdk_at.get(tgt_sdk)
            ti = s_at.get(tgt_stock)
            if callee is None or ti is None or ti in found or callee in named or callee not in body:
                continue
            r = ratio(callee, ti)
            found[ti] = ('call', r, callee)
            named.add(callee)
            if r >= RATIO:
                work.append(ti)

    rows = sorted(found.items())
    with open(sys.argv[3], 'w') as f:
        for si, (how, r, name) in rows:
            f.write(f'{stock[si][0]:08x}\t{how}\t{r:.2f}\t{body[name][1]}\t{name}\n')
    good = [(si, v) for si, v in rows if v[1] >= RATIO]
    cov = sum(body[v[2]][1] for _, v in good)
    print(f'SDK functions: {len(body)}; named in stock: {len(rows)} '
          f'({sum(v[0] == "body" for _, v in rows)} by body, '
          f'{sum(v[0] == "call" for _, v in rows)} by call), {len(good)} with ratio >= {RATIO}')
    print(f'stock instructions covered by ratio >= {RATIO}: {cov} of {len(ops)} '
          f'({100 * cov / len(ops):.1f} %)')


if __name__ == '__main__':
    main()
