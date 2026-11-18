#!/usr/bin/env bash
# sdklib.sh: read a function in the SDK's prebuilt libraries.
#
#   fw-re/sdk/sdklib.sh LIB.a MEMBER.o OUTDIR     e.g. btstack.a hid.c.o /tmp/hid
#
# The libraries in cpu/br23/liba are LLVM bitcode. This extracts MEMBER,
# compiles it to pi32v2 code with our toolchain (it doesn't go into any
# firmware), and writes OUTDIR/MEMBER.dis (disassembly) and
# OUTDIR/MEMBER.relocs (address, symbol: what each 0 / call 0 refers to;
# the toolchain's objdump prints those as "Unknown"). Without a path, LIB is
# looked up in the SDK's cpu/br23/liba.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[ -n "${IN_NIX_SHELL:-}" ] || ! command -v nix >/dev/null || exec nix develop "$ROOT" -c "$0" "$@"
LIB=$1 MEMBER=$2 OUT=$3
[ -f "$LIB" ] || LIB=$(cd "$ROOT/.." && pwd)/fw-AC63_BT_SDK/cpu/br23/liba/$LIB
TC=${JL_TOOLCHAIN:-$ROOT/fw-re/jl-toolchain}
mkdir -p "$OUT"
LIB=$(realpath "$LIB")
(cd "$OUT" && "$TC/pi32v2/bin/ar" x "$LIB" "$MEMBER")
"$TC/common/bin/clang" --target=pi32v2 -mcpu=r3 -Oz -c -x ir "$OUT/$MEMBER" -o "$OUT/$MEMBER.native"
"$TC/common/bin/objdump" -d --triple=pi32v2 "$OUT/$MEMBER.native" > "$OUT/$MEMBER.dis"
python3 "$ROOT/fw-re/sdk/elfrelocs.py" "$OUT/$MEMBER.native" > "$OUT/$MEMBER.relocs"
echo "$OUT/$MEMBER.dis, $OUT/$MEMBER.relocs"
