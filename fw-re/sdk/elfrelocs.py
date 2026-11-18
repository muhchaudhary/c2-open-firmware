#!/usr/bin/env python3
"""elfrelocs.py: list the relocations of an ELF32 object as address + symbol.

  elfrelocs.py OBJ.o

For every REL/RELA section, prints the offset in the section it applies to,
the target symbol (or section) name, and the addend. Used by sdklib.sh: the
JieLi objdump shows relocation targets as "Unknown".
"""
import struct
import sys


def main():
    d = open(sys.argv[1], 'rb').read()
    shoff, = struct.unpack_from('<I', d, 0x20)
    shes, shn, shstr = struct.unpack_from('<HHH', d, 0x2e)
    sh = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shes) for i in range(shn)]

    def cstr(tab, off):
        return d[tab + off:d.index(b'\0', tab + off)].decode()

    secname = [cstr(sh[shstr][4], s[0]) for s in sh]
    symtab = [i for i, s in enumerate(sh) if s[1] == 2][0]
    st = sh[symtab]
    syms = []
    for i in range(st[5] // 16):
        n, v, sz, info, oth, shndx = struct.unpack_from('<IIIBBH', d, st[4] + i * 16)
        syms.append(cstr(sh[st[6]][4], n) if n else (secname[shndx] if shndx < len(secname) else ''))
    for s in sh:
        if s[1] not in (4, 9):
            continue
        ent = 12 if s[1] == 4 else 8
        print('== %s' % secname[s[7]])
        for k in range(s[5] // ent):
            off, info = struct.unpack_from('<II', d, s[4] + k * ent)
            add = struct.unpack_from('<i', d, s[4] + k * ent + 8)[0] if s[1] == 4 else 0
            print('%05x %-32s %+d' % (off, syms[info >> 8], add))


if __name__ == '__main__':
    main()
