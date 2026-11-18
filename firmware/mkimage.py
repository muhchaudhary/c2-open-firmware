#!/usr/bin/env python3
"""mkimage.py: the flash image of a board, from our app and the JieLi SDK's files only.

  firmware/mkimage.py build  LAYOUT.json APP.bin APP_ADDR SDK_TOOLS OUT
  firmware/mkimage.py verify LAYOUT.json IMAGE [APP.bin]
  firmware/mkimage.py kind   IMAGE          -> "full" or "app" (exit 1: neither)

LAYOUT.json is the board's (firmware/boards/<board>/image.json). APP.bin is
.text + .data of sdk.elf, APP_ADDR its link address (0x1E00120 on the pad,
0x1E000C0 on the receiver: app.bin sits at APP_ADDR - 0x1E00000 in the app
area). SDK_TOOLS is the SDK's cpu/br23/tools (uboot.boot, cfg_tool.bin).

Two kinds (LAYOUT "kind"):
  full  the whole JLFS container at SPI 0x1000 (the pad): header, the SDK's
        uboot.boot, an isd_config entry made here (chip key 0xFFFF, the
        layout's settings), the slot A directory header, the app area.
  app   the app area alone (the receiver: its own loader and header stay on
        the chip; LAYOUT "flash_at" is where it goes).

The format (fw-re/jl.py: JieLi's ENC / SFC ciphers and CRC16):
  JLFS header, 32 B: CRC | 0x0220 | version (4 B, plain) | flash size - 4 KB
    | 4 flag bytes | name (16 B, plain); the rest ENC'd; the CRC covers
    bytes 2..31 as decoded.
  Entries, 32 B each, ENC'd one by one (in the app area: plain, the area is
    SFC'd as a whole): CRC | data CRC | offset | size | flags | resvd | idx
    (1: the last) | name (16 B); CRC over bytes 2..31.
  uboot.boot: the SDK's file, its 16-byte header and its body ENC'd apart.
  isd_config: a 32-byte chip-key block, its CRC16, then settings, each
    value-length, key NUL-terminated, value (strings, or u32 LE).
  Slot A's directory header, 32 B before the app area, ENC'd: data CRC =
    CRC16 of the decrypted area, offset = CRC16 of the encrypted area,
    size = the area's size.
  App area: app_area_head, then app.bin, cfg_tool.bin and the areas (VM,
    PRCT, BTIF: absolute flash addresses), app.bin at its link address'
    offset, cfg_tool.bin right after it; SFC'd with the chip key, the
    stream starting at the area.
"""
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fw-re'))
from jl import crc16, enc, sfc, chipkey_decode  # noqa: E402

CHIPKEY = 0xFFFF
# The chip-key block for key 0xFFFF, as JieLi's packer (isd_download) writes
# it into isd_config: a 32-byte encoding of the number 0xFFFF (jl.py's
# chipkey_decode), checked below, not code.
CHIPKEY_BLOCK = bytes.fromhex('203413d65997fa2e6f41768928f01eb81c4bb54aa815520a5af99457570b2e22')
# The SDK files that go into our images, as at firmware/sdk/SDK_COMMIT (the
# tested ones): JieLi's bytes in what we ship. Another hash is an error (the
# SDK checkout changed under us); SDK_ANY_COMMIT=1 makes it a warning, as in
# firmware/sdk/sync.sh. The licence: LICENSING.md.
SDK_FILES = {
    'uboot.boot':   'b319dbb4e8ad85c39a68e4aea6f078bcdc25bfde21af6fdc3a49e47073fb6d30',
    'cfg_tool.bin': '21334ccee5588a3e72bd871ce0cc3eb2ad3c333f79927842279855fed87f1ec8',
}
XIP_BASE = 0x1E00000          # the app area maps here
AREA_HEAD_ADDR = 0x1E000C0    # app_area_head's "offset": the same on both boards' images
APP_FLAGS = 0x82              # an inline blob (app.bin, cfg_tool.bin)
AREA_FLAGS = 0x83             # app_area_head: its size includes its own header
HDR_FORMAT = 0x0220


def sdk_file(tools, name):
    """An SDK file's bytes, checked against SDK_FILES."""
    path = os.path.join(tools, name)
    data = open(path, 'rb').read()
    have = hashlib.sha256(data).hexdigest()
    want = SDK_FILES.get(name)
    if have != want:
        msg = '%s: sha256 %s, not the tested %s' % (path, have[:16], (want or 'none')[:16])
        if os.environ.get('SDK_ANY_COMMIT') != '1':
            raise ValueError(msg + ' (SDK_ANY_COMMIT=1 packs it anyway)')
        print('warning: ' + msg, file=sys.stderr)
    return data


def num(v):
    return int(v, 0) if isinstance(v, str) else v


def entry(name, dcrc, off, size, flags, resvd=0xff, idx=0, attr=b''):
    """A 32-byte entry, plain (its CRC over bytes 2..31). The name field: the
    name, NUL, 0xFF up to 16 bytes; an area's attribute in its last 4 bytes
    (attr: VM's size word, PRCT "CODE", BTIF "AUTO")."""
    raw = name.encode('latin1')
    field = raw if len(raw) == 16 else raw + b'\0'
    assert len(field) + len(attr) <= 16, name
    field = field + b'\xff' * (16 - len(field) - len(attr)) + attr
    body = struct.pack('<HIIBBH16s', dcrc, off, size, flags, resvd, idx, field)
    return struct.pack('<H', crc16(body)) + body


def enc_copy(b, key=0xffff):
    t = bytearray(b)
    enc(t, 0, len(t), key)
    return bytes(t)


def keystream(n):
    return enc_copy(bytes(n))


def jlfs_header(h):
    """The 32-byte JLFS header: version (4) and name (16) stay plain, the rest is ENC'd."""
    ver = h['version'].encode('latin1')
    name = h['name'].encode('latin1')
    assert len(ver) == 4 and len(name) <= 16
    name = name + b'\xff' * (16 - len(name))
    k = keystream(32)
    stored = bytearray(32)
    stored[4:8] = ver
    stored[16:32] = name
    plain = bytearray(32)                     # the header as the CRC sees it (all decoded)
    struct.pack_into('<H', plain, 2, HDR_FORMAT)
    struct.pack_into('<I', plain, 8, num(h['flash_size']))
    plain[12:16] = bytes.fromhex(h['flags'])
    for i in list(range(4, 8)) + list(range(16, 32)):
        plain[i] = stored[i] ^ k[i]           # the plain fields, decoded
    struct.pack_into('<H', plain, 0, crc16(bytes(plain[2:])))
    for i in list(range(0, 4)) + list(range(8, 16)):
        stored[i] = plain[i] ^ k[i]
    return bytes(stored)


def uboot_blob(tools, name):
    """The SDK's uboot.boot as the container holds it: header (16 B) and body ENC'd apart."""
    path = os.path.join(tools, name)
    u = sdk_file(tools, name)
    hdr = struct.unpack_from('<HHIII', u)
    if crc16(u[16:]) != hdr[4] & 0xffff or crc16(u[:14]) != hdr[4] >> 16:
        raise ValueError('%s: its own CRCs don\'t match: not a uboot.boot' % path)
    return enc_copy(u[:16]) + enc_copy(u[16:])


def isd_config(settings):
    if chipkey_decode(CHIPKEY_BLOCK) != CHIPKEY:
        raise AssertionError('chip-key block does not encode 0xFFFF')
    out = bytearray(CHIPKEY_BLOCK) + struct.pack('<H', crc16(CHIPKEY_BLOCK))
    for key, val in settings:
        v = val.encode('latin1') if isinstance(val, str) else struct.pack('<I', val)
        out += bytes([len(v)]) + key.encode('latin1') + b'\0' + v
    return bytes(out)


def app_area(layout, app, app_addr, cfg):
    """The app area, plain: app_area_head, sub-entries, app.bin, cfg_tool.bin."""
    app_off = app_addr - XIP_BASE
    areas = layout['areas']
    n_subs = 2 + len(areas)
    if app_off < 32 * (1 + n_subs):
        raise ValueError('app.bin at %#x would overlap the entries' % app_off)
    cfg_off = app_off + len(app)
    size = cfg_off + len(cfg)
    subs = [entry('app.bin', crc16(app), app_off, len(app), APP_FLAGS),
            entry('cfg_tool.bin', crc16(cfg), cfg_off, len(cfg), APP_FLAGS)]
    for i, a in enumerate(areas):
        subs.append(entry(a['name'], 0xffff, num(a['off']), num(a['size']), num(a['flags']), num(a['resvd']),
                          1 if i == len(areas) - 1 else 0, bytes.fromhex(a.get('attr', ''))))
    body = bytearray(b'\xff' * (size - 32))
    pos = 0
    for s in subs:
        body[pos:pos + 32] = s
        pos += 32
    body[app_off - 32:app_off - 32 + len(app)] = app
    body[cfg_off - 32:cfg_off - 32 + len(cfg)] = cfg
    head = entry('app_area_head', crc16(bytes(body)), AREA_HEAD_ADDR, size, AREA_FLAGS, 0xff, 1)
    return head + bytes(body)


def sfc_copy(b, key=CHIPKEY):
    t = bytearray(b)
    sfc(t, 0, len(t), 0, key)
    return bytes(t)


def build(layout, app, app_addr, tools):
    cfg = sdk_file(tools, 'cfg_tool.bin')
    area = app_area(layout, app, app_addr, cfg)
    area_enc = sfc_copy(area)
    if layout['kind'] == 'app':
        size = (len(area_enc) + 0xfff) & ~0xfff
        if size > num(layout['max_size']):
            raise ValueError('app area %#x B > %#x: it would reach the installer record'
                             % (size, num(layout['max_size'])))
        return area_enc + b'\xff' * (size - len(area_enc))
    if layout['kind'] != 'full':
        raise ValueError('layout kind %r' % layout['kind'])
    img = bytearray(b'\xff' * num(layout['image_size']))
    img[0:32] = jlfs_header(layout['header'])
    ub = uboot_blob(tools, layout['uboot'])
    ub_at = num(layout['uboot_at'])
    isd = isd_config(layout['isd_config'])
    isd_at = ub_at + len(ub)
    tops = [entry('uboot.boot', crc16(ub), ub_at, len(ub), 0x00, 0x00, 0),
            entry('isd_config.ini', crc16(isd), isd_at, len(isd), 0x02, 0x80, 0)]
    for d in layout['dirs']:
        tops.append(entry(d['name'], num(d['dcrc']), num(d['off']), num(d['size']), num(d['flags']),
                          num(d['resvd']), num(d.get('idx', 0))))
    if 32 + 32 * len(tops) > ub_at:
        raise ValueError('entry list runs into uboot.boot')
    for i, t in enumerate(tops):
        img[32 + 32 * i:64 + 32 * i] = enc_copy(t)
    img[ub_at:ub_at + len(ub)] = ub
    img[isd_at:isd_at + len(isd)] = isd
    appbase = num(layout['dirs'][0]['off'])
    if isd_at + len(isd) > appbase - 32:
        raise ValueError('isd_config runs into the app area')
    if appbase + len(area_enc) > len(img):
        raise ValueError('app area %#x..%#x past the image end %#x'
                         % (appbase, appbase + len(area_enc), len(img)))
    dirhead = entry('\xff' * 16, crc16(area), crc16(area_enc), len(area), 0xff, 0xff, 0xffff)   # an all-0xFF name
    img[appbase - 32:appbase] = enc_copy(dirhead)
    img[appbase:appbase + len(area_enc)] = area_enc
    return bytes(img)


# ---- checks ------------------------------------------------------------------
def parse_entry(b, off=0):
    hcrc, dcrc, eoff, size, flags, resvd, idx, name = struct.unpack_from('<HHIIBBH16s', b, off)
    return dict(ok=crc16(b[off + 2:off + 32]) == hcrc, dcrc=dcrc, off=eoff, size=size, flags=flags,
                resvd=resvd, idx=idx, name=name.split(b'\0')[0].split(b'\xff')[0].decode('latin1'))


def check_area(area_enc, app=None):
    """Decrypt an app area and check its CRCs; returns (ok, lines, entries by name)."""
    head = parse_entry(sfc_copy(area_enc[:32]))
    lines = []
    if not head['ok'] or head['name'] != 'app_area_head' or head['size'] > len(area_enc):
        return False, ['no valid app_area_head'], {}
    area = sfc_copy(area_enc[:head['size']])
    ok = crc16(area[32:]) == head['dcrc']
    lines.append('app_area_head size %#x crc ok=%s' % (head['size'], ok))
    subs, off = {}, 32
    while off + 32 <= len(area):
        e = parse_entry(area, off)
        subs[e['name']] = e
        good = e['ok']
        if e['flags'] == APP_FLAGS:
            good = good and crc16(area[e['off']:e['off'] + e['size']]) == e['dcrc']
        lines.append('  %-12s off %#x size %#x ok=%s' % (e['name'], e['off'], e['size'], good))
        ok = ok and good
        if e['idx'] or not e['ok']:
            break
        off += 32
    if app is not None and 'app.bin' in subs:
        same = area[subs['app.bin']['off']:subs['app.bin']['off'] + subs['app.bin']['size']] == app
        lines.append('  app.bin == expected app: %s' % same)
        ok = ok and same
    return ok, lines, subs


def verify(layout, img, app=None):
    lines = []
    if layout['kind'] == 'app':
        ok, l2, subs = check_area(img, app)
        lines += l2
        for a in layout['areas']:
            s = subs.get(a['name'])
            good = s is not None and s['off'] == num(a['off']) and s['size'] == num(a['size'])
            ok = ok and good
            if not good:
                lines.append('  area %s not where the layout has it' % a['name'])
        ok = ok and len(img) <= num(layout['max_size'])
        return ok, lines
    ok = is_jlfs_header(img)
    lines.append('header ok=%s name %s' % (ok, img[16:32].split(b'\xff')[0].decode('latin1')))
    off = 32
    tops = {}
    while True:
        e = parse_entry(enc_copy(img[off:off + 32]))
        tops[e['name']] = e
        data_ok = True
        if e['name'] in ('uboot.boot', 'isd_config.ini'):
            data_ok = crc16(img[e['off']:e['off'] + e['size']]) == e['dcrc']
        lines.append('top %-14s ok=%s data ok=%s' % (e['name'], e['ok'], data_ok))
        ok = ok and e['ok'] and data_ok
        if e['idx'] or not e['ok']:
            break
        off += 32
    isd = tops.get('isd_config.ini')
    if isd:
        ok = ok and chipkey_decode(img[isd['off']:isd['off'] + 32]) == CHIPKEY
    appbase = tops['app_dir_head']['off']
    a_ok, l2, _ = check_area(img[appbase:], app)
    d = parse_entry(enc_copy(img[appbase - 32:appbase]))
    size = d['size']
    d_ok = d['ok'] and crc16(img[appbase:appbase + size]) == d['off'] and \
        crc16(sfc_copy(img[appbase:appbase + size])) == d['dcrc']
    lines.append('slot A directory header ok=%s' % d_ok)
    return ok and a_ok and d_ok, lines + l2


def is_jlfs_header(img):
    """A JLFS header at img[0]: its CRC and the format word. (The CRC alone isn't
    enough: an app area's first entry is SFC'd with key 0xFFFF at offset 0, the
    same keystream as the header's ENC, and its CRC holds too.)"""
    k = keystream(32)
    h = bytes(img[i] ^ k[i] for i in range(32))
    return crc16(h[2:]) == struct.unpack_from('<H', h)[0] and struct.unpack_from('<H', h, 2)[0] == HDR_FORMAT


def kind(img):
    if is_jlfs_header(img):
        return 'full'
    if check_area(img)[0]:
        return 'app'
    return None


def main(argv):
    if len(argv) >= 7 and argv[1] == 'build':
        layout = json.load(open(argv[2]))
        app = open(argv[3], 'rb').read()
        try:
            img = build(layout, app, int(argv[4], 0), argv[5])
        except ValueError as e:
            sys.exit('error: %s' % e)
        ok, lines = verify(layout, img, app)
        print('\n'.join(lines))
        if not ok:
            sys.exit('VERIFY FAILED')
        open(argv[6], 'wb').write(img)
        print('VERIFY OK: %s, %d bytes (%s)' % (argv[6], len(img), layout['kind']))
    elif len(argv) >= 4 and argv[1] == 'verify':
        layout = json.load(open(argv[2]))
        app = open(argv[4], 'rb').read() if len(argv) > 4 else None
        ok, lines = verify(layout, open(argv[3], 'rb').read(), app)
        print('\n'.join(lines))
        print('VERIFY OK' if ok else 'VERIFY FAILED')
        sys.exit(0 if ok else 1)
    elif len(argv) == 3 and argv[1] == 'kind':
        k = kind(open(argv[2], 'rb').read())
        if not k:
            sys.exit(1)
        print(k)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
