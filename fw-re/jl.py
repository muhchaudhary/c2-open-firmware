"""Own re-implementation of JieLi ENC cipher / CRC16 (from kagaimiq/jielie docs)."""
import struct

def crc16(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc

def enc(buf, off, size, key=0xffff):
    for i in range(size):
        buf[off+i] ^= key & 0xff
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xffff
    return key

def sfc(buf, off, size, base, key, bs=32):
    for i in range(0, size, bs):
        enc(buf, off+i, min(size-i, bs), key ^ (((off+i-base) >> 2) & 0xffff))

def chipkey_decode(d):
    s = sum(d[:16]) & 0xff
    s = 0xaa if s >= 0xe0 else 0x55 if s <= 0x10 else s
    return sum(1 << i for i in range(16) if (d[16+i] ^ d[15-i]) < s)

def nts(b): return b.split(b'\0')[0].decode('latin1')
