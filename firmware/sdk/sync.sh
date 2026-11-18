#!/usr/bin/env bash
# sync.sh: install the gamepad firmware into a fw-AC63_BT_SDK checkout.
#
#   firmware/sdk/sync.sh [SDK_DIR]      (default: ../fw-AC63_BT_SDK next to this repo)
#   BOARD=c2 firmware/sdk/sync.sh ...   (the board, firmware/boards/<name>; default c2)
#
# Restores the SDK files the firmware patches to their git state, copies
# firmware/src and firmware/boards/$BOARD to SDK_DIR/apps/gamepad, then
# applies firmware/sdk/patch_sdk.py (board selection, the sources in the
# build, the SDK changes). The SDK checkout is treated as a build tree: don't
# keep your own edits in the files listed in $PATCHED. Safe to run again;
# `git -C SDK_DIR status` shows exactly what it changed.
#
# The SDK must be at the commit in firmware/sdk/SDK_COMMIT (the one the
# patches were written for); SDK_ANY_COMMIT=1 tries another one anyway
# (patch_sdk.py still stops at a missing anchor).
#
# Then build: firmware/build.sh does sync + make + pack.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=${1:-$(cd "$ROOT/.." && pwd)/fw-AC63_BT_SDK}
BOARD=${BOARD:-c2}
FW=$ROOT/firmware
[ -f "$SDK/apps/hid/board/br23/board_config.h" ] || { echo "error: $SDK is not a fw-AC63_BT_SDK checkout" >&2; exit 1; }
[ -d "$FW/boards/$BOARD" ] || { echo "error: no board firmware/boards/$BOARD" >&2; exit 1; }

WANT=$(tr -d ' \n' < "$FW/sdk/SDK_COMMIT")
HAVE=$(git -C "$SDK" rev-parse HEAD 2>/dev/null || echo none)
if [ "$HAVE" != "$WANT" ]; then
  if [ "${SDK_ANY_COMMIT:-0}" = 1 ]; then
    echo "warning: SDK at ${HAVE:0:10}, the patches are for ${WANT:0:10} (SDK_ANY_COMMIT=1: trying anyway)" >&2
  else
    cat >&2 <<MSG
error: the SDK checkout $SDK is at ${HAVE:0:10}; this firmware is patched
for ${WANT:0:10} (firmware/sdk/SDK_COMMIT). Get that commit:
  git -C "$SDK" fetch --depth 1 origin $WANT
  git -C "$SDK" checkout $WANT
(SDK_ANY_COMMIT=1 tries the checkout as it is.)
MSG
    exit 1
  fi
fi

PATCHED=(
  apps/hid/board/br23/Makefile
  apps/hid/board/br23/board_config.h
  apps/hid/examples/keyboard/app_keyboard.c
  apps/hid/config/lib_profile_config.c
  apps/hid/modules/bt/edr_hid_user.c
  apps/hid/modules/bt/app_comm_edr.c
  apps/hid/modules/power/app_power_manage.c
  apps/common/device/usb/device/descriptor.c
  apps/common/device/usb/device/user_setup.c
  apps/common/device/usb/device/cdc.c
  apps/common/device/usb/usb_config.c
  cpu/br23/uart_dev.c
  cpu/br23/sdk_ld.c
  cpu/br23/setup.c
  apps/common/update/update.c
  apps/hid/config/lib_btctrler_config.c
)
git -C "$SDK" checkout -q -- "${PATCHED[@]}"

# Anything else changed in the SDK goes into the build unseen: say so. (The
# SDK's Makefile regenerates cpu/br23/sdk_used_list.used, which JieLi track.)
OTHER=$(git -C "$SDK" status --porcelain --untracked-files=no -- . ':!cpu/br23/sdk_used_list.used')
if [ -n "$OTHER" ]; then
  echo "warning: SDK files changed outside the firmware's patches (they go into the build):" >&2
  echo "$OTHER" | sed 's/^/  /' >&2
fi

# The previous layout installed into apps/hid (examples/c2, board/br23/*c2*).
rm -rf "$SDK/apps/hid/examples/c2"
rm -f "$SDK"/apps/hid/board/br23/{board_c2.c,board_c2_cfg.h,board_c2_global_build_cfg.h,c2_board.h,c2_pins.h,c2_power.h}

rm -rf "$SDK/apps/gamepad"
mkdir -p "$SDK/apps/gamepad/boards"
cp -rp "$FW/src" "$SDK/apps/gamepad/src"
cp -rp "$FW/boards/$BOARD" "$SDK/apps/gamepad/boards/$BOARD"

python3 "$FW/sdk/patch_sdk.py" "$SDK" "$BOARD"
echo "installed the firmware for board $BOARD into $SDK/apps/gamepad"
