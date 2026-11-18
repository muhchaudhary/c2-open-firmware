#!/usr/bin/env bash
# build.sh: build the firmware and pack it into a flash image.
#
#   firmware/build.sh            sync into the SDK, build, pack
#   firmware/build.sh --flash    ... then enter the bootloader and write it (sudo)
#   firmware/build.sh --wall     ... and check our sources with -Wall (firmware/sdk/wall.mk;
#                                any warning fails the build; CI runs this)
#   BOARD=c2 firmware/build.sh   the board (firmware/boards/<name>; default c2)
#
# Steps: firmware/sdk/sync.sh (installs the firmware into the SDK checkout),
# the SDK's make for apps/hid (its post-build download.sh is skipped: it
# needs JieLi tools we don't have and isn't needed), a check that sdk.elf was
# actually relinked, then firmware/pack.sh. Output:
# firmware/build/<board>.flash.bin, <board>.elf (symbols for
# tools/c2addr.sh) and <board>.json (release, commit, SDK commit, sizes,
# the image's sha256).
#
# SDK checkout: ../fw-AC63_BT_SDK next to this repo, or set SDK=/path.
# For --flash, the debug log port's "boot" command enters the bootloader;
# otherwise hold Home + Options with USB connected, or use
# tools/usbkey-mega2560/.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# Run inside the repo's dev shell (flake.nix: the toolchain, python3) when
# Nix is there; without it, as is (fw-re/get-toolchain.sh's toolchain).
if [ -z "${IN_NIX_SHELL:-}" ] && command -v nix >/dev/null; then
  exec nix develop "$ROOT" -c "$0" "$@"
fi

FLASH=0 WALL=0
for a in "$@"; do
  case "$a" in
    --flash) FLASH=1 ;;
    --wall) WALL=1 ;;
    -h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

# Not as root: the SDK checkout and firmware/build/ would be left with root's
# files, and the next build fails on them. --flash asks sudo itself.
[ "$(id -u)" != 0 ] || { echo "build as yourself, not root (--flash runs sudo for the flash step only)" >&2; exit 1; }

SDK=${SDK:-$(cd "$ROOT/.." && pwd)/fw-AC63_BT_SDK}
export BOARD=${BOARD:-c2}
MKDIR=$SDK/apps/hid/board/br23
ELF=$SDK/cpu/br23/tools/sdk.elf
TC=${JL_TOOLCHAIN:-$ROOT/fw-re/jl-toolchain}/pi32v2/bin   # JL_TOOLCHAIN: the dev shell's (flake.nix)
OUT=$ROOT/firmware/build/$BOARD.flash.bin
LOG=$ROOT/firmware/build/make.log
mkdir -p "$ROOT/firmware/build"

echo "== sync board $BOARD into $SDK"
"$ROOT/firmware/sdk/sync.sh" "$SDK" | sed 's/^/   /'

# The firmware version for the configuration protocol's INFO reply
# (core/gp_config.c): the git commit, "+" if the tree has changes.
VER=$(git -C "$ROOT" rev-parse --short=10 HEAD 2>/dev/null || echo unknown)
git -C "$ROOT" diff --quiet HEAD -- firmware 2>/dev/null || VER="$VER+"
# The release number (firmware/VERSION, major.minor.patch): INFO bytes 43..45.
REL=$(tr -d ' \n' < "$ROOT/firmware/VERSION")
[[ "$REL" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || { echo "error: firmware/VERSION is '$REL', not major.minor.patch" >&2; exit 1; }
VERH=$SDK/apps/gamepad/src/core/gp_version.h
{
  printf '#define GP_FW_VERSION "%s"\n' "$VER"
  printf '#define GP_FW_MAJOR %s\n#define GP_FW_MINOR %s\n#define GP_FW_PATCH %s\n' \
    "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}"
} > "$VERH"
echo "   version $REL ($VER)"

echo "== build (log: ${LOG#$ROOT/})"
stamp=$(mktemp); trap 'rm -f "$stamp"' EXIT
sleep 1   # sdk.elf must end up strictly newer than this
mk() { make -C "$MKDIR" TOOL_DIR="$TC" RUN_POST_SCRIPT=: -j"$(nproc)" >"$LOG" 2>&1; }
if ! mk && grep -q "No rule to make target" "$LOG"; then
  echo "   stale dependencies (a file moved): clean build"
  rm -rf "$MKDIR/objs"
  mk || true
fi
if grep -qE "^make: \*\*\*|Error [0-9]+$" "$LOG"; then
  echo "!! make failed. Errors:"
  grep -E "error|undefined reference|No rule|\*\*\*" "$LOG" | grep -v "warning:" | head -30 | sed 's/^/   /'
  exit 1
fi
{ grep -E '^\+(CC|LINK)' "$LOG" || true; } | sed 's/^/   /' | tail -5
if [ ! -f "$ELF" ] || [ ! "$ELF" -nt "$stamp" ]; then
  if grep -q '^+LINK' "$LOG"; then
    echo "!! the link ran but $ELF was not written:"
    grep -iE "error|undefined" "$LOG" | head -20 | sed 's/^/   /'
    exit 1
  fi
  echo "   (nothing changed since the last build: reusing $ELF)"
fi

if [ "$WALL" = 1 ]; then
  echo "== -Wall (our sources)"
  if ! make -s -C "$MKDIR" -f Makefile -f "$ROOT/firmware/sdk/wall.mk" wall TOOL_DIR="$TC" 2>&1 | sed 's/^/   /'; then
    echo "!! warnings in our sources (above)"; exit 1
  fi
fi

echo "== pack"
"$ROOT/firmware/pack.sh" "$ELF" "$OUT" "$BOARD" "$SDK" | sed 's/^/   /'
cp -p "$ELF" "$ROOT/firmware/build/$BOARD.elf"   # symbols for addresses in the debug log (tools/c2addr.sh)
SHA=$(sha256sum "$OUT" | cut -d' ' -f1)
APPSZ=0   # .text + .data: app.bin (pack.sh)
for sz in $(LC_ALL=C "$TC/objdump" -h "$ELF" | awk '$2 == ".text" || $2 == ".data" {print $3}'); do APPSZ=$((APPSZ + 0x$sz)); done
cat > "${OUT%.flash.bin}.json" <<JSON
{
  "board": "$BOARD",
  "release": "$REL",
  "commit": "$VER",
  "sdk_commit": "$(tr -d ' \n' < "$ROOT/firmware/sdk/SDK_COMMIT")",
  "toolchain": "$(basename "$(readlink -f "$TC/../..")")",
  "image": "$(basename "$OUT")",
  "image_bytes": $(stat -c%s "$OUT"),
  "app_bytes": $APPSZ,
  "sha256": "$SHA"
}
JSON
echo "== $OUT"
echo "   sha256 ${SHA:0:16}"

if [ "$FLASH" = 1 ]; then
  echo "== flash"
  sudo "$ROOT/fw-re/jlflash.sh" write "$OUT"           # enters the bootloader on the image's device
else
  echo "   flash: sudo fw-re/jlflash.sh write ${OUT#$ROOT/}"
  echo "   or:    firmware/build.sh --flash"
fi
