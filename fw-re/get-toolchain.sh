#!/usr/bin/env bash
# get-toolchain.sh — download the JieLi Linux toolchain 20260730.1 (clang 4.0.1,
# the version every committed image was built with), check its sha256 and
# unpack it into fw-re/jl-toolchain/ (gitignored), where firmware/build.sh looks.
# With Nix, the dev shell (flake.nix) has the same toolchain and this isn't needed.
#
# Source: the DimmKirr/setup-fm1-toolchain mirror (JieLi's own pkgman URL was
# unreachable and always serves the newest release). The mirror re-packs
# upstream's archive (top-level toolchain/ directory), so its sha256 differs
# from upstream's (a1545cddbc451a06bac43eb5cd96d831470c42a97c5c0b1f98879a20fc7647bd,
# per the mirror's release notes).
#
#   fw-re/get-toolchain.sh            install into fw-re/jl-toolchain
#   DEST=/some/dir fw-re/get-toolchain.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${DEST:-$HERE/jl-toolchain}
VER=20260730.1
URL=https://github.com/DimmKirr/setup-fm1-toolchain/releases/download/toolchain-$VER/toolchain-$VER.tar.xz
SHA=d3fc3be92cfc07ccb0ac682b3b2e41e4d14593426067e6c3c6e908db2f90c0e0

if [ -e "$DEST" ]; then
  echo "$DEST already exists; remove it first to reinstall" >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "downloading toolchain $VER"
curl -fL --progress-bar -o "$TMP/tc.tar.xz" "$URL"
echo "$SHA  $TMP/tc.tar.xz" | sha256sum -c --quiet - || {
  echo "error: sha256 mismatch for $URL" >&2
  exit 1
}
tar -xJf "$TMP/tc.tar.xz" -C "$TMP"
[ -f "$TMP/toolchain/common/bin/clang" ] && [ -f "$TMP/toolchain/pi32v2/bin/ld" ] || {
  echo "error: unexpected archive layout (no toolchain/common/bin/clang)" >&2
  exit 1
}
mkdir -p "$(dirname "$DEST")"
mv "$TMP/toolchain" "$DEST"
echo "installed: $DEST"
