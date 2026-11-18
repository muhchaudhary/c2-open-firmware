#!/usr/bin/env bash
# get-uboot-tool.sh: fetch the flasher's engine, kagaimiq's jl-uboot-tool
# (MIT), at the commit this repository is tested with, into
# fw-re/jl-uboot-tool/ (gitignored), where fw-re/jlflash.sh looks.
# With Nix, the dev shell links the same commit there and this isn't needed.
#
# Not kept in this repository: it carries JieLi's USB loader blob, which
# isn't ours to redistribute.
#
#   fw-re/get-uboot-tool.sh            (as yourself, not with sudo)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=$HERE/jl-uboot-tool
URL=https://github.com/kagaimiq/jl-uboot-tool.git
REV=adb3f18889e88ac512ce0a3c4d8cc3d3cb30696a   # flake.nix pins the same

if [ -e "$DEST" ]; then
  echo "$DEST already exists; remove it first to fetch again" >&2
  exit 1
fi
git init -q "$DEST"
git -C "$DEST" fetch -q --depth 1 "$URL" "$REV"
git -C "$DEST" checkout -q FETCH_HEAD
[ "$(git -C "$DEST" rev-parse HEAD)" = "$REV" ] || { echo "error: got $(git -C "$DEST" rev-parse HEAD), not $REV" >&2; exit 1; }
echo "jl-uboot-tool $REV in $DEST"
echo "Its Python needs: $(tr '\n' ' ' < "$DEST/requirements.txt")(fw-re/.jlpy: README.md, Flashing)"
