#!/usr/bin/env python3
"""ufw — list a JieLi .ufw update package and extract the flash images in it.

A .ufw (GameSir Connect's `resources/dfu/packages/*.ufw`) starts with a 0x40-byte header,
ENC'd with key 0xFFFF like a JLFS entry: hcrc, crc, total file size, a count, the chip
name (`AC695X` for the C2 pad and dongle). Records follow at 0x40, 0x180, 0x2C0, ... (every
0x140; only the first 0x40 bytes of each are understood): type, CRC16 of the data, data
offset and length. Record 0 is the flash image `fw-re/flash.py` and `mkufw.py` take (JLFS
header at byte 0, SFC-encrypted app area).

Some packages hold more flash images than records describe: the dongle's has six (the same
app.bin, three per key_mac layout, 0x7F000 / 0x7FF00, i.e. per chip variant). `list` finds
them by their JLFS header; `extract --at OFF` cuts one from OFF to the next.

Usage:
  ufw.py list    PACKAGE.ufw
  ufw.py extract PACKAGE.ufw OUT_FLASH [--record N | --at OFFSET]   # default: record 0
"""
import sys, struct, os

sys.path.insert(0, os.path.dirname(__file__))
from jl import crc16, enc, nts

REC0, REC_STEP = 0x40, 0x140


def header(f):
    h = bytearray(f[:0x40]); enc(h, 0, 0x40)
    if crc16(h[2:]) != struct.unpack_from('<H', h)[0]:
        raise ValueError('not a .ufw: header CRC mismatch')
    _, dcrc, size, count = struct.unpack_from('<HHIH', h)
    return dict(dcrc=dcrc, size=size, count=count, chip=nts(bytes(h[16:32])))


def records(f):
    out = []
    for o in range(REC0, len(f) - 0x40, REC_STEP):
        r = bytearray(f[o:o + 0x40]); enc(r, 0, 0x40)
        a, typ, crc, off, ln, ln2 = struct.unpack_from('<HHIIII', r)
        if off == 0 or ln == 0 or off + ln > len(f) or crc > 0xffff:
            break
        out.append(dict(at=o, a=a, type=typ, crc=crc, off=off, len=ln, len2=ln2,
                        crc_ok=crc16(f[off:off + ln]) == crc))
    return out


def jlfs_images(f):
    """Offsets of flash images (a JLFS header whose first entry is uboot.boot)."""
    out = []
    for o in range(0, len(f) - 64, 0x20):
        h = bytearray(f[o:o + 64]); enc(h, 0, 32); enc(h, 32, 32)
        if crc16(h[2:32]) != struct.unpack_from('<H', h)[0]:
            continue
        if crc16(h[34:64]) == struct.unpack_from('<H', h, 32)[0] and nts(bytes(h[48:64])) == 'uboot.boot':
            # bytes 4..8 (version) and 16..32 (name) of a JLFS header are stored plain
            out.append((o, nts(f[o + 16:o + 32].split(b'\xff')[0]), f[o + 4:o + 8].decode('latin1')))
    return out


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('list', 'extract'):
        print(__doc__); return 2
    f = open(sys.argv[2], 'rb').read()
    hd = header(f)
    recs = records(f)
    if sys.argv[1] == 'list':
        print('chip %s  size %#x (file %#x)  count %d' % (hd['chip'], hd['size'], len(f), hd['count']))
        for i, r in enumerate(recs):
            print('  record %d at %#05x  type %04x/%04x  data %#08x..%#08x  crc %04x %s'
                  % (i, r['at'], r['a'], r['type'], r['off'], r['off'] + r['len'], r['crc'],
                     'ok' if r['crc_ok'] else '(not a CRC of the data: record not understood)'))
        for o, name, ver in jlfs_images(f):
            print('  flash image at %#08x  %s v%s' % (o, name, ver))
        return 0
    out = sys.argv[3]
    args = sys.argv[4:]
    if '--at' in args:
        at = int(args[args.index('--at') + 1], 0)
        starts = sorted(o for o, _, _ in jlfs_images(f))
        if at not in starts:
            raise SystemExit('no flash image at %#x (see `list`)' % at)
        nxt = [o for o in starts if o > at]
        data = f[at:nxt[0] if nxt else len(f)]
    else:
        n = int(args[args.index('--record') + 1]) if '--record' in args else 0
        r = recs[n]
        if not r['crc_ok']:
            raise SystemExit('record %d: data CRC mismatch' % n)
        data = f[r['off']:r['off'] + r['len']]
    open(out, 'wb').write(data)
    print('wrote %s (%#x bytes)' % (out, len(data)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
