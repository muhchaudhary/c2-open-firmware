#!/usr/bin/env bash
# c2addr.sh: name the functions containing addresses from the debug log
# (STALL lines' "pc ..."), using the ELF of the image that logged them.
#
#   tools/c2addr.sh ADDR [ADDR ...]            (hex, with or without 0x)
#   ELF=path/to/c2.elf tools/c2addr.sh ...    (default firmware/build/c2.elf)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${IN_NIX_SHELL:-}" ] && command -v nix >/dev/null; then exec nix develop "$ROOT" -c "$0" "$@"; fi
ELF=${ELF:-$ROOT/firmware/build/c2.elf}
"${JL_TOOLCHAIN:-$ROOT/fw-re/jl-toolchain}/pi32v2/bin/objdump" -t "$ELF" | python3 -c '
import sys, re
syms = []
for l in sys.stdin:
    m = re.match(r"([0-9a-f]{8}) .{7} (\S+)\s+([0-9a-f]{8}) (\S+)$", l.rstrip())
    if m and int(m.group(3), 16):
        syms.append((int(m.group(1), 16), int(m.group(3), 16), m.group(4), m.group(2)))
for a in sys.argv[1:]:
    x = int(a, 16)
    hit = [s for s in syms if s[0] <= x < s[0] + s[1]]
    print(f"{x:08x}  " + (", ".join(f"{n}+{x - b:#x} ({sec})" for b, _, n, sec in hit) or "?"))
' "$@"
