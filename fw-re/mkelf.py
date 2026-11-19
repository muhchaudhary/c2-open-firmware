#!/usr/bin/env python3
"""mkelf.py: wrap a raw pi32v2 blob (a stock app.bin) in a minimal ELF32 so
JieLi's objdump can disassemble it.

  mkelf.py APP.bin OUT.elf BASE

BASE is the address the app runs at (XIP): 0x1E00120 for the pad's app,
0x1E000C0 for the receiver's. e_machine is 0xF1, JieLi's value for pi32v2.
Then:

  $JL_TOOLCHAIN/common/bin/objdump -d --triple=pi32v2 OUT.elf > app.dis

(docs/reverse-engineering.md).
"""
import struct
import sys

if len(sys.argv) != 4:
    sys.exit(__doc__)
blob = open(sys.argv[1], 'rb').read()
addr = int(sys.argv[3], 0)
shstr = b'\0.text\0.shstrtab\0'
ehsz, phsz, shsz = 52, 32, 40
text_off = 0x100
shstr_off = text_off + len(blob)
sh_off = (shstr_off + len(shstr) + 3) & ~3

eh = b'\x7fELF' + bytes([1, 1, 1]) + bytes(9)
eh += struct.pack('<HHIIIIIHHHHHH', 2, 0xF1, 1, addr, ehsz, sh_off, 0, ehsz, phsz, 1, shsz, 3, 2)
ph = struct.pack('<IIIIIIII', 1, text_off, addr, addr, len(blob), len(blob), 5, 4)
sh = bytes(shsz)
sh += struct.pack('<IIIIIIIIII', 1, 1, 6, addr, text_off, len(blob), 0, 0, 2, 0)
sh += struct.pack('<IIIIIIIIII', 7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)

out = bytearray(eh + ph)
out += bytes(text_off - len(out)) + blob + shstr
out += bytes(sh_off - len(out)) + sh
open(sys.argv[2], 'wb').write(out)
