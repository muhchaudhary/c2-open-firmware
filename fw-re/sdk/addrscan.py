#!/usr/bin/env python3
"""addrscan.py: list accesses to a RAM range in the stock listing.

  addrscan.py STOCK.dis LO HI   (hex)

Tracks constant registers per function (rN = imm, rN = rM + imm, rN += imm)
and prints every [r+imm] / [r+r<<k] operand whose address is in LO..HI."""
import re,sys
lo,hi=int(sys.argv[2],16),int(sys.argv[3],16)
INS=re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t?(.*)$')
regs={}
for line in open(sys.argv[1],errors='replace'):
    m=INS.match(line)
    if not m: continue
    a,t=int(m[1],16),m[3].split('#')[0].split(' <')[0].strip()
    if t.startswith('[--sp] = {rets'): regs={}
    # memory operands
    for mm in re.finditer(r'([bhd]?)\[(r\d+)\+(-?\d+)\]',t):
        r=int(mm[2][1:])
        if r in regs:
            ea=regs[r]+int(mm[3])
            if lo<=ea<=hi: print('%x  %#x  %s'%(a,ea,t))
    for mm in re.finditer(r'([bhd]?)\[(r\d+)\+(r\d+)(?:<<(\d))?\]',t):
        r1,r2=int(mm[2][1:]),int(mm[3][1:])
        if r1 in regs and r2 in regs:
            ea=regs[r1]+(regs[r2]<<int(mm[4] or 0))
            if lo<=ea<=hi: print('%x  %#x  %s'%(a,ea,t))
    v=None
    x=re.match(r'^r(\d+) = (-?\d+)$',t)
    if x: d,v=int(x[1]),int(x[2])
    x2=re.match(r'^r(\d+) = r(\d+) \+ (-?\d+)$',t)
    if x2 and int(x2[2]) in regs: d,v=int(x2[1]),regs[int(x2[2])]+int(x2[3])
    x3=re.match(r'^r(\d+) \+= (-?\d+)$',t)
    if x3 and int(x3[1]) in regs: d,v=int(x3[1]),regs[int(x3[1])]+int(x3[2])
    if v is not None: regs[d]=v
    else:
        w=re.match(r'^r(\d+) (?:[-+*/&|^]|<<|>>)?= ',t)
        if w: regs.pop(int(w[1]),None)
    if t.startswith('call'): 
        for k in (0,1,2,3): regs.pop(k,None)
