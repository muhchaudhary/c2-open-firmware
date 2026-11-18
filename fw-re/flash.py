import sys, struct, os
sys.path.insert(0, os.path.dirname(__file__))
from jl import *

def entry(buf, off):
    hcrc, hdr = struct.unpack_from('<H30s', buf, off)
    dcrc, eoff, size, flags, resvd, idx, name = struct.unpack('<HIIBBH16s', hdr)
    return dict(ok=crc16(hdr) == hcrc, hdr=off, dcrc=dcrc, off=eoff, size=size, flags=flags, resvd=resvd, idx=idx, name=nts(name))

fw = bytearray(open(sys.argv[1], 'rb').read())
out = sys.argv[2]; os.makedirs(out, exist_ok=True)
base = None
for off in (0, 0x1000, 0x10000):
    h = bytearray(fw[off:off+32]); enc(h, 0, 32)
    if crc16(h[2:]) == struct.unpack_from('<H', h)[0]:
        base = off; break
print('base', hex(base))
h = h[:4] + fw[base+4:base+8] + h[8:16] + fw[base+16:base+32]
print(struct.unpack('<HH4sIBBBB16s', h))
fw[base:base+32] = h

# top-level list
off = base + 32; chipkey = None; appbase = None
while True:
    enc(fw, off, 32)
    e = entry(fw, off); print('top', e)
    data = e['off'] + base
    if e['name'] == 'isd_config.ini':
        ck = fw[data:data+32]
        if crc16(ck) == struct.unpack_from('<H', fw, data+32)[0]:
            chipkey = chipkey_decode(ck); print('chipkey %04x' % chipkey)
        open(f'{out}/isd_config.ini.bin', 'wb').write(fw[data:data+e['size']])
    if e['name'] == 'app_dir_head' and appbase is None: appbase = data
    if e['idx']: break
    off += 32

# app area (sfc-encrypted with chipkey, header+data interleaved)
off = appbase; dec = appbase; first = True
while True:
    nxt = (off + 32 + 31) & ~31
    if dec < nxt: sfc(fw, dec, nxt-dec, appbase, chipkey); dec = nxt
    e = entry(fw, off); print('app', e)
    if not e['ok']: break
    end = off + e['size']
    end2 = end if e['idx'] else (end + 32 + 31) & ~31
    if dec < end2: sfc(fw, dec, end2-dec, appbase, chipkey); dec = end2
    if first:
        print('entry point %#x' % e['off'])
        sub = off + 32
        while True:
            a = entry(fw, sub); print('   ', a)
            d0 = off + a['off']
            if a['flags'] == 0x82:
                blob = fw[d0:d0+a['size']]
                print('     crc ok', crc16(blob) == a['dcrc'])
                open(f"{out}/{a['name']}", 'wb').write(blob)
            if a['idx'] or not a['ok']: break
            sub += 32
        first = False
    else:
        open(f"{out}/{e['name']}.blk", 'wb').write(fw[off:end])
    if e['idx']: break
    off = end
open(f'{out}/decrypted.bin', 'wb').write(fw)
