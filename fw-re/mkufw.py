#!/usr/bin/env python3
"""mkufw — build a flashable C2 (BR23/AC695X) flash image from a stock image + a custom app.

This is the *builder* inverse of flash.py (the decrypter). It:
  * reuses the stock JLFS header, top-level entry list, uboot.boot and isd_config.ini verbatim
    (the only thing we change is the app image in slot A),
  * swaps the app.bin blob inside the SFC-encrypted app_area_head region,
  * recomputes the affected CRCs (app.bin dcrc/hcrc, app_area_head dcrc/hcrc),
  * re-SFC-encrypts the region with the chip key (0xFFFF for No_Key images),
  * self-verifies the result (and can round-trip: build-with-stock-app == byte-identical input).

A raw flash image is what jlflash.sh reads/writes: byte 0 == SPI 0x1000 (the JLFS header),
LEN 0x77000 bytes. Slot A (app_dir_head) is fully inside that window; slot B (app_dir_head2)
starts at SPI 0x7D000, outside the 0x77000 window, so only slot A is (re)written here.

Usage:
  mkufw.py roundtrip STOCK_FLASH [OUT]   # self-test: build using the stock app; OUT must equal STOCK
  mkufw.py build    STOCK_FLASH CUSTOM_APP OUT_FLASH [--resize]
  mkufw.py verify   FLASH [EXPECTED_APP] # decrypt + check every CRC; optionally app.bin == EXPECTED_APP

By default the custom app must be exactly the size of the stock app.bin. With
`build --resize` it may differ: the region is [app_area_head + sub-entries][app.bin at
+0x120][cfg_tool.bin], all inline and contiguous, so cfg_tool.bin moves to just after the
new app.bin, the app.bin and app_area_head sizes change, and the bytes the old region
used past the new end become 0xFF.

Areas chained after the app region (the C2 dongle's `tone`, flags 0x83: a 32-byte header
whose size counts itself, data CRC'd after the header) are part of the same SFC stream from
appbase; a resize moves them to follow the new region. Everything must end before the
limit: slot B (app_dir_head2, the pad's 0x7C020), the VM area (the dongle's 0x51000) or
the end of the image, whichever comes first.

The same works for the C2 dongle (GS_C2_Dongle_*.ufw, also AC695X): one app slot, 512 KB
flash; `ufw.py` extracts its flash image from the .ufw.

Today's images come from firmware/mkimage.py; jlflash.sh uses this file's checks (verify).
"""
import sys, struct, os

sys.path.insert(0, os.path.dirname(__file__))
from jl import crc16, enc, sfc, chipkey_decode, nts

APP_NAME = 'app.bin'
APP_FLAGS = 0x82          # inline data blob
EXPECTED_FLASH_SIZE = 0x77000


def entry(buf, off):
    """Parse one 32-byte JLFS entry (already-decrypted bytes) at buf[off]."""
    hcrc, hdr = struct.unpack_from('<H30s', buf, off)
    dcrc, eoff, size, flags, resvd, idx, name = struct.unpack('<HIIBBH16s', hdr)
    return dict(ok=crc16(hdr) == hcrc, dcrc=dcrc, off=eoff, size=size,
                flags=flags, resvd=resvd, idx=idx, name=nts(name))


def set_entry_dcrc(buf, off, dcrc):
    """Write dcrc into the entry header and refresh its hcrc (over bytes [2:32])."""
    struct.pack_into('<H', buf, off + 2, dcrc)
    hdr = buf[off + 2:off + 32]
    struct.pack_into('<H', buf, off, crc16(hdr))


def find_header_base(fw):
    for off in (0, 0x1000, 0x10000):
        h = bytearray(fw[off:off + 32]); enc(h, 0, 32)
        if crc16(h[2:]) == struct.unpack_from('<H', h)[0]:
            return off
    raise ValueError('no valid JLFS header found at 0x0/0x1000/0x10000')


def parse_top(fw, base):
    """Walk the top-level entry list (each 32 B is ENC'd with a fresh key 0xFFFF).
    Returns (tops, chipkey, appbase)."""
    off = base + 32; chipkey = None; appbase = None; tops = []
    while True:
        g = bytearray(fw[off:off + 32]); enc(g, 0, 32)
        e = entry(g, 0)
        tops.append(e)
        data = e['off'] + base
        if e['name'] == 'isd_config.ini':
            ck = fw[data:data + 32]
            if crc16(ck) == struct.unpack_from('<H', fw, data + 32)[0]:
                chipkey = chipkey_decode(ck)
        if e['name'] == 'app_dir_head' and appbase is None:
            appbase = data
        if e['idx']:
            break
        off += 32
    if chipkey is None:
        raise ValueError('could not decode chip key from isd_config.ini')
    if appbase is None:
        raise ValueError('no app_dir_head (slot A) in top list')
    return tops, chipkey, appbase


def app_region_size(fw, appbase, chipkey):
    """Size of the SFC-encrypted app image, from the app_area_head entry (first block)."""
    h = bytearray(fw[appbase:appbase + 32])
    sfc(h, 0, 32, 0, chipkey)          # first 32 B of the region == key^0 block
    _, _, size, _, _, _, _ = struct.unpack_from('<HIIBBH16s', h, 2)
    return size


def parse_app(fw, appbase):
    """Parse app_area_head + its sub-entries from an already-decrypted region.
    Returns (ahead, subs, appbin)."""
    ahead = entry(fw, appbase)
    subs = []
    sub = appbase + 32
    while True:
        a = entry(fw, sub)
        subs.append((sub, a))
        if a['idx'] or not a['ok']:
            break
        sub += 32
    appbin = None
    for s, a in subs:
        if a['name'] == APP_NAME and a['flags'] == APP_FLAGS:
            appbin = (s, a)
    if appbin is None:
        raise ValueError('app.bin sub-entry not found in app_area_head')
    return ahead, subs, appbin


CFG_NAME = 'cfg_tool.bin'
AREA_FLAGS = 0x83         # an area entry (app_area_head, tone): size includes the header


def region_limit(fw, base, tops, subs, used_end=None):
    """Where slot A's region (and what follows it) must end: slot B, the VM area, the
    image end, or the first non-0xFF byte after used_end (the stock region's end; the
    dongle's installed image keeps a 16-byte record at 0x50FF0), whichever comes first."""
    ends = [len(fw)]
    ends += [base + t['off'] for t in tops if t['name'] == 'app_dir_head2']
    ends += [base + a['off'] for _, a in subs if a['name'] == 'VM']
    if used_end is not None:
        top = min(ends)
        nxt = next((i for i in range(used_end, top) if fw[i] != 0xff), None)
        if nxt is not None:
            ends.append(nxt & ~0xf)
    return min(ends)


def tail_size(fw, appbase, A, chipkey, limit):
    """Bytes of the area entries chained right after the app region (0 if none), from
    the still-encrypted image. Raises if the chain is not a run of area entries."""
    g = bytearray(fw[appbase:limit])
    sfc(g, 0, len(g), 0, chipkey)               # same stream as the region, base appbase
    t = A
    while t + 32 <= len(g):
        e = entry(g, t)
        if not e['ok']:
            break
        if e['flags'] != AREA_FLAGS or e['off'] != 32 or e['size'] < 32 or t + e['size'] > len(g):
            raise ValueError('unexpected entry %r after the app region' % e['name'])
        if crc16(g[t + 32:t + e['size']]) != e['dcrc']:
            raise ValueError('area %r after the app region: data CRC mismatch' % e['name'])
        t += e['size']
        if e['idx']:
            break
    return t - A


def tail_entries(g, at, size):
    """The (offset, entry) list of the area chain at g[at:at+size] (decrypted)."""
    out, t = [], at
    while t < at + size:
        e = entry(g, t); out.append((t, e)); t += e['size']
    return out


def build(stock, custom_app, resize=False):
    """Return a new flash image with the slot-A app.bin replaced by custom_app."""
    fw = bytearray(stock)
    base = find_header_base(fw)
    if base != 0:
        print('note: header base %#x (expected 0)' % base)
    tops, chipkey, appbase = parse_top(fw, base)
    print('chipkey %04x  appbase %#x' % (chipkey, appbase))

    A = app_region_size(fw, appbase, chipkey)
    if appbase + A > len(fw):
        raise ValueError('app region %#x..%#x exceeds image size %#x' %
                         (appbase, appbase + A, len(fw)))
    T = tail_size(fw, appbase, A, chipkey, len(fw))
    # decrypt the whole app image region (and the areas chained after it) in place
    sfc(fw, appbase, A + T, appbase, chipkey)

    ahead, subs, (appbin_off, appbin) = parse_app(fw, appbase)
    limit = region_limit(fw, base, tops, subs, appbase + A + T)
    if appbase + A + T > limit:
        raise ValueError('stock region %#x..%#x already runs past the limit %#x'
                         % (appbase, appbase + A + T, limit))
    if T:
        print('areas after the app region: %s (%d bytes)'
              % (', '.join(e['name'] for _, e in tail_entries(fw, appbase + A, T)), T))
    if not (ahead['ok'] and appbin['ok']):
        raise ValueError('app_area_head / app.bin entry CRCs invalid in stock image')
    if crc16(fw[appbase + 32:appbase + A]) != ahead['dcrc']:
        raise ValueError('stock app_area_head dcrc does not match region (corrupt input?)')

    blob_off = appbase + appbin['off']
    blob = fw[blob_off:blob_off + appbin['size']]
    if crc16(blob) != appbin['dcrc']:
        raise ValueError('stock app.bin dcrc mismatch (corrupt input?)')
    print('stock app.bin  %#x..%#x  %d bytes  crc16 %04x' %
          (blob_off, blob_off + appbin['size'], len(blob), appbin['dcrc']))

    if len(custom_app) != appbin['size']:
        if not resize:
            raise ValueError('custom app is %d bytes; slot app.bin is %d bytes (must match, '
                             'or pass --resize)' % (len(custom_app), appbin['size']))
        A = relayout(fw, appbase, A, T, subs, appbin_off, appbin, len(custom_app), limit)
        appbin = entry(fw, appbin_off)

    # swap + recompute CRCs (app.bin first, then app_area_head over the updated region)
    fw[blob_off:blob_off + appbin['size']] = custom_app
    set_entry_dcrc(fw, appbin_off, crc16(custom_app))
    print('new app.bin dcrc %04x' % crc16(custom_app))
    set_entry_dcrc(fw, appbase, crc16(fw[appbase + 32:appbase + A]))

    # re-encrypt the region and the areas after it (one stream from appbase)
    sfc(fw, appbase, A + T, appbase, chipkey)
    return bytes(fw)


def relayout(fw, appbase, A, T, subs, appbin_off, appbin, new_size, limit):
    """Resize app.bin in the decrypted region fw[appbase:appbase+A]: move cfg_tool.bin
    to follow it, update both entries and app_area_head's size, and move the T bytes of
    areas chained after the region to follow the new one. Returns the new A."""
    inline = sorted((a['off'], s, a) for s, a in subs if a['flags'] == APP_FLAGS)
    names = [a['name'] for _, _, a in inline]
    if names != [APP_NAME, CFG_NAME]:
        raise ValueError('unexpected inline sub-entries %s (want app.bin, cfg_tool.bin)' % names)
    _, cfg_off, cfg = inline[1]
    if appbin['off'] + appbin['size'] != cfg['off'] or cfg['off'] + cfg['size'] != A:
        raise ValueError('app.bin / cfg_tool.bin are not contiguous to the region end')
    cfg_blob = bytes(fw[appbase + cfg['off']:appbase + cfg['off'] + cfg['size']])
    tail = bytes(fw[appbase + A:appbase + A + T])
    new_cfg = appbin['off'] + new_size
    new_A = new_cfg + cfg['size']
    if appbase + new_A + T > limit:
        raise ValueError('resized region %#x..%#x runs past the limit %#x (slot B, VM or '
                         'the image end)' % (appbase, appbase + new_A + T, limit))
    end = max(A, new_A) + T
    fw[appbase + appbin['off']:appbase + end] = b'\xff' * (end - appbin['off'])
    fw[appbase + new_cfg:appbase + new_A] = cfg_blob
    fw[appbase + new_A:appbase + new_A + T] = tail
    struct.pack_into('<I', fw, appbin_off + 8, new_size)      # entry: hcrc dcrc off size ...
    set_entry_dcrc(fw, appbin_off, 0)                          # placeholder, set by caller
    struct.pack_into('<I', fw, cfg_off + 4, new_cfg)
    set_entry_dcrc(fw, cfg_off, crc16(cfg_blob))
    struct.pack_into('<I', fw, appbase + 8, new_A)
    print('resized app.bin %d -> %d bytes; cfg_tool.bin at +%#x; region %#x -> %#x bytes'
          % (appbin['size'], new_size, new_cfg, A, new_A))
    return new_A


def verify(fw, expected_app=None):
    """Decrypt and check every CRC in the app region. Returns (ok, report_lines)."""
    lines = []
    base = find_header_base(fw)
    tops, chipkey, appbase = parse_top(fw, base)
    lines.append('header base %#x  chipkey %04x  appbase %#x' % (base, chipkey, appbase))
    for t in tops:
        lines.append('  top %-14s ok=%s dcrc=%04x size=%#x' % (t['name'], t['ok'], t['dcrc'], t['size']))
    A = app_region_size(fw, appbase, chipkey)
    try:
        T = tail_size(fw, appbase, A, chipkey, len(fw))
    except ValueError as err:
        lines.append('  after the app region: %s' % err)
        return False, lines
    g = bytearray(fw)
    sfc(g, appbase, A + T, appbase, chipkey)
    ahead, subs, (appbin_off, appbin) = parse_app(g, appbase)
    limit = region_limit(fw, base, tops, subs, appbase + A + T)
    ok = ahead['ok'] and appbase + A + T <= limit
    lines.append('  region %#x..%#x  limit %#x' % (appbase, appbase + A + T, limit))
    for t, e in tail_entries(g, appbase + A, T):
        lines.append('  area %-13s ok=%s dcrc=%04x size=%#x at %#x'
                     % (e['name'], e['ok'], e['dcrc'], e['size'], t))
    lines.append('  app_area_head ok=%s dcrc=%04x' % (ahead['ok'], ahead['dcrc']))
    ok = ok and (crc16(g[appbase + 32:appbase + A]) == ahead['dcrc'])
    lines.append('    region dcrc match=%s' % (crc16(g[appbase + 32:appbase + A]) == ahead['dcrc']))
    for s, a in subs:
        lines.append('  sub %-14s ok=%s dcrc=%04x' % (a['name'], a['ok'], a['dcrc']))
        ok = ok and a['ok']
    blob_off = appbase + appbin['off']
    blob = g[blob_off:blob_off + appbin['size']]
    dc = crc16(blob) == appbin['dcrc']
    lines.append('  app.bin dcrc match=%s (%d bytes)' % (dc, len(blob)))
    ok = ok and dc
    if expected_app is not None:
        match = bytes(blob) == expected_app
        lines.append('  app.bin == expected app: %s' % match)
        ok = ok and match
    return ok, lines


def roundtrip(stock, out_path=None):
    """Build using the stock's own app; the output must be byte-identical to the input."""
    base = find_header_base(stock)
    tops, chipkey, appbase = parse_top(stock, base)
    A = app_region_size(stock, appbase, chipkey)
    g = bytearray(stock)
    sfc(g, appbase, A, appbase, chipkey)
    ahead, subs, (appbin_off, appbin) = parse_app(g, appbase)
    blob_off = appbase + appbin['off']
    stock_app = bytes(g[blob_off:blob_off + appbin['size']])
    out = build(stock, stock_app)
    identical = (out == stock)
    print('roundtrip: built %d bytes, input %d bytes, byte-identical=%s'
          % (len(out), len(stock), identical))
    if out_path:
        open(out_path, 'wb').write(out)
        print('wrote %s' % out_path)
    return identical


def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    cmd = sys.argv[1]
    if cmd == 'roundtrip':
        stock = open(sys.argv[2], 'rb').read()
        out = sys.argv[3] if len(sys.argv) > 3 else None
        ok = roundtrip(stock, out)
        return 0 if ok else 1
    if cmd == 'build':
        stock = open(sys.argv[2], 'rb').read()
        custom = open(sys.argv[3], 'rb').read()
        out_path = sys.argv[4]
        if len(stock) != EXPECTED_FLASH_SIZE:
            print('warning: stock flash is %#x bytes (expected %#x)' % (len(stock), EXPECTED_FLASH_SIZE))
        out = build(stock, custom, resize='--resize' in sys.argv[5:])
        open(out_path, 'wb').write(out)
        print('wrote %s (%d bytes)' % (out_path, len(out)))
        ok, lines = verify(out, expected_app=custom)
        print('--- self-verify ---')
        for l in lines: print(l)
        print('VERIFY %s' % ('OK' if ok else 'FAILED'))
        return 0 if ok else 1
    if cmd == 'verify':
        fw = open(sys.argv[2], 'rb').read()
        exp = open(sys.argv[3], 'rb').read() if len(sys.argv) > 3 else None
        ok, lines = verify(fw, expected_app=exp)
        for l in lines: print(l)
        print('VERIFY %s' % ('OK' if ok else 'FAILED'))
        return 0 if ok else 1
    print('unknown command: %s' % cmd); return 2


if __name__ == '__main__':
    sys.exit(main())
