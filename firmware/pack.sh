#!/usr/bin/env bash
# pack.sh: turn an SDK build (sdk.elf) into the board's flash image.
#
#   firmware/pack.sh SDK_ELF OUT_FLASH BOARD SDK
#
# app.bin = .text + .data, as the SDK's own download.sh does (.data has no
# separate load address: the startup code copies it from right after .text;
# a HID build has no overlays). firmware/mkimage.py puts it into the board's
# image (firmware/boards/BOARD/image.json) with the SDK's uboot.boot and
# cfg_tool.bin (SDK/cpu/br23/tools): nothing else goes in.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TC=${JL_TOOLCHAIN:-$ROOT/fw-re/jl-toolchain}/pi32v2/bin
ELF=$1 OUT=$2 BOARD=$3 SDK=$4
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
"$TC/objcopy" -O binary -j .text "$ELF" "$W/text.bin"
"$TC/objcopy" -O binary -j .data "$ELF" "$W/data.bin"
cat "$W/text.bin" "$W/data.bin" > "$W/app.bin"
# the link address: app.bin's place in the app area (0x1E00120 pad, 0x1E000C0 receiver)
ADDR=0x$("$TC/objdump" -h "$ELF" | awk '$2 == ".text" {print $4; exit}')
echo "app.bin: $(stat -c%s "$W/text.bin") + $(stat -c%s "$W/data.bin") = $(stat -c%s "$W/app.bin") bytes at $ADDR"
python3 "$ROOT/firmware/mkimage.py" build "$ROOT/firmware/boards/$BOARD/image.json" \
  "$W/app.bin" "$ADDR" "$SDK/cpu/br23/tools" "$OUT" | tail -3
