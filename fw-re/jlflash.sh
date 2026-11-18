#!/usr/bin/env bash
# Linux-native JieLi flasher for the GameSir Cyclone 2 (BR23 / AC695X).
# Run with sudo (needs /dev/sg); have the controller plugged into the host.
#
#   sudo ./jlflash.sh scan                show JieLi USB + any /dev/sg for it
#   sudo ./jlflash.sh enter [pad|receiver] clear usb_storage quirk + enter the USB bootloader
#                                          (debug log "boot", else the GameSir vendor HID
#                                          command; without either it lists the other ways
#                                          in and waits). Pad and receiver both plugged in:
#                                          name the one (they share USB IDs; told apart by
#                                          the USB product string), else it refuses.
#   sudo ./jlflash.sh read  OUT.bin        read whole flash (LEN=0x77000) to OUT.bin  [SAFE]
#   sudo ./jlflash.sh write FLASH.bin      erase+write FLASH.bin, then reset          [RISK]
#                                          (raw flash image that passes mkufw.py verify;
#                                          not in the bootloader yet: enters it on the
#                                          device the image is for, pad or receiver)
#   sudo ./jlflash.sh verify FLASH.bin     read back and compare with FLASH.bin  [SAFE]
#   sudo ./jlflash.sh restore FULL.bin     write a whole-chip backup of the receiver at 0  [RISK]
#   sudo ./jlflash.sh reset                reset the chip (boot the firmware)
#   sudo ./jlflash.sh memread ADDR LEN     hex dump of chip memory (RAM / ROM)  [SAFE]
#   sudo ./jlflash.sh crumbs [ADDR]        read the receiver firmware's breadcrumbs from RAM
#                                          (dg_nv in firmware/build/dongle.elf; docs/dongle.md)  [SAFE]
#
# Two devices:
#   pad      a whole image, JLFS header GS_C2_ADC_DEVICE...: at 0x1000, 0x77000 B
#   receiver the app area alone (firmware/mkimage.py, firmware/boards/dongle/image.json):
#            at 0x11000, up to 0x60000. The receiver boots its own loader from 0x0
#            (isd_config EOFFSET = 0x10), which finds the app through the JLFS header at
#            0x10000; neither is written, nor the installer record's sector (0x60000),
#            VM 0x61000, BTIF 0x7E000, key_mac 0x7F000. (Older receiver images, a whole
#            installed image GS_C2_Dongle at 0x10000, 0x51000 B, are still taken.)
# write / verify / restore first read the chip's header (0x1000 pad, 0x10000 receiver) and
# require the image's device name; for an app area also the app at 0x11000. So an image
# never lands on the other device. FORCE=1 skips the name check (a chip whose header is
# gone). Whole receiver backup: IMG_ADDR=0 LEN=0x80000 ... read OUT.bin
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLDIR=$HERE/jl-uboot-tool
LEN=${LEN:-0x77000}
IMG_ADDR=${IMG_ADDR:-0x1000}   # firmware image offset in SPI flash (first 4KB reserved/erased)
DONGLE_NAME=GS_C2_Dongle DONGLE_ADDR=0x10000 DONGLE_LEN=0x51000 DONGLE_FULL=0x80000
DONGLE_APP_ADDR=0x11000 DONGLE_APP_END=0x60000   # the app area; 0x60000: the installer record's sector
MKIMAGE=$HERE/../firmware/mkimage.py DONGLE_LAYOUT=$HERE/../firmware/boards/dongle/image.json
CHK_ADDR= APP_IMG=0   # set by need_image: the chip header to check; an app-area image
jlt(){
  [ -f "$TOOLDIR/jluboottool.py" ] || { echo "no $TOOLDIR: the flasher's engine (jl-uboot-tool) is fetched, not in git." >&2
    echo "Enter the dev shell once (nix develop: it links it), or run fw-re/get-uboot-tool.sh as yourself (not sudo)." >&2; exit 1; }
  ( cd "$TOOLDIR" && "$HERE/.jlpy/bin/python3" jluboottool.py --chip br23 "$@" ); }
find_sg(){  # /dev/sgN whose USB ancestor has idVendor 4c4a (JieLi UBOOT)
  for s in /sys/class/scsi_generic/sg*; do
    [ -e "$s" ] || continue
    p=$(readlink -f "$s/device")
    while [ -n "$p" ] && [ "$p" != "/" ]; do
      if [ -f "$p/idVendor" ] && [ "$(cat "$p/idVendor" 2>/dev/null)" = "4c4a" ]; then
        echo "/dev/$(basename "$s")"; return 0; fi
      p=$(dirname "$p")
    done
  done; return 1
}
jl_usb_path(){  # sysfs path of the 4c4a USB device
  for d in /sys/bus/usb/devices/*; do
    [ -f "$d/idVendor" ] && [ "$(cat "$d/idVendor" 2>/dev/null)" = "4c4a" ] && { echo "$d"; return 0; }
  done; return 1; }
ensure_sg(){  # make /dev/sg appear
  modprobe sg 2>/dev/null || true
  find_sg && return 0
  echo > /sys/module/usb_storage/parameters/quirks   # ensure quirk is clear
  u=$(jl_usb_path) || { echo "no 4c4a device (not in bootloader?)"; return 1; }
  echo "re-enumerating $u to bind usb-storage ..."
  echo 0 > "$u/authorized"; sleep 1; echo 1 > "$u/authorized"; sleep 2
  for i in $(seq 1 40); do find_sg >/dev/null 2>&1 && break; sleep 0.3; done
  find_sg; }
reset_chip(){  # reset; the chip reboots before it completes the SCSI command,
  # so the tool reports "Transfer failed: info:01, host status:07" (DID_ERROR).
  # That is the expected result: check the bootloader left USB instead.
  local out
  out=$(jlt --device "$1" "reset" 2>&1) || true
  if [ -n "$out" ] && ! grep -q 'host status:07' <<<"$out"; then echo "$out"; fi
  for i in $(seq 1 20); do jl_usb_path >/dev/null || { echo "reset: chip rebooted into the firmware"; return 0; }; sleep 0.25; done
  echo "reset: sent, but the bootloader (4c4a) is still on USB"; [ -z "$out" ] || echo "$out"; return 1; }
# Python for the image checks (stdlib only): the tool's venv, else the system one.
PY=$HERE/.jlpy/bin/python3; [ -x "$PY" ] || PY=$(command -v python3 || true)
is_ufw(){  # by extension, or by the header CRC so a renamed .ufw is caught too
  # exit 0 = UFW, 1 = not UFW; any other result (no python, read error) aborts
  [[ "${1,,}" == *.ufw ]] && return 0
  [ -n "$PY" ] || { echo "need python3 to check the image"; exit 1; }
  local rc=0
  "$PY" -I -c 'import sys; sys.path.insert(0, sys.argv[1]); from jl import *
h = bytearray(open(sys.argv[2], "rb").read(0x40)); enc(h, 0, 0x40)
sys.exit(crc16(h[2:0x40]) != int.from_bytes(h[:2], "little"))' "$HERE" "$1" || rc=$?
  [ "$rc" -le 1 ] || { echo "could not check $1 for a UFW header (python exit $rc)"; exit 1; }
  return "$rc"; }
hdr_name(){  # FILE OFFSET -> the JLFS header's name at OFFSET (empty if no valid header there:
  # its CRC and the format word 0x0220; an app area's first entry passes the CRC alone)
  "$PY" -I -c 'import sys; sys.path.insert(0, sys.argv[1]); from jl import *
f = open(sys.argv[2], "rb"); f.seek(int(sys.argv[3], 0)); raw = f.read(32)
h = bytearray(raw); enc(h, 0, 32)
if len(raw) == 32 and crc16(h[2:]) == int.from_bytes(h[:2], "little") and h[2:4] == b"\x20\x02":
    print(raw[16:32].split(b"\xff")[0].split(b"\0")[0].decode("latin1"))' "$HERE" "$1" "$2"; }
check_target(){  # SG ADDR NAME: the chip's header at ADDR must name NAME (FORCE=1 skips);
  # for an app-area image (APP_IMG=1) its app_dir_head must also point at IMG_ADDR
  local tmp got; tmp=$(mktemp)
  jlt --device "$1" "read $2 0x1000 $tmp" >/dev/null
  got=$(hdr_name "$tmp" 0)
  if [ "$got" = "$3" ] && [ "$APP_IMG" = 1 ]; then
    local app; app=$("$PY" -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import mkufw
fw = open(sys.argv[2], "rb").read(); tops, ck, ab = mkufw.parse_top(fw, 0); print(hex(ab))' "$HERE" "$tmp" 2>/dev/null || true)
    rm -f "$tmp"
    if [ -z "$app" ] || [ $((app + $2)) -ne $((IMG_ADDR)) ]; then
      echo "!!! the chip's header at $2 puts the app at ${app:-?} (+$2), not $IMG_ADDR: not the layout"
      echo "    this image is for. Not writing (no FORCE for this: it would break the boot)."; exit 1
    fi
    echo "target: chip header at $2 is '$got', app area at $IMG_ADDR"; return 0
  fi
  rm -f "$tmp"
  if [ "$got" = "$3" ]; then echo "target: chip header at $2 is '$got'"; return 0; fi
  echo "!!! the chip's header at $2 is '${got:-none}', the image is '$3': wrong device?"
  if [ "${FORCE:-0}" = 1 ]; then echo "FORCE=1: writing anyway"; return 0; fi
  echo "Not writing. (FORCE=1 overrides, for a chip whose header is gone.)"; exit 1; }
need_image(){  # refuse anything but a valid raw flash image; sets IMG (absolute path), and
  # IMG_ADDR / LEN / IMG_NAME from the image's header name (pad or receiver)
  [ -f "$1" ] || { echo "no such file: '$1'"; exit 1; }
  IMG=$(realpath "$1")
  IMG_NAME=$(hdr_name "$IMG" 0)
  if [ -z "$IMG_NAME" ] && [ "$("$PY" -I "$MKIMAGE" kind "$IMG" 2>/dev/null)" = app ]; then
    # the receiver's app area: checked against its layout (areas where the chip has them)
    if ! "$PY" -I "$MKIMAGE" verify "$DONGLE_LAYOUT" "$IMG" >/dev/null 2>&1; then
      echo "$1 is an app area, but not the receiver's (python3 firmware/mkimage.py verify $DONGLE_LAYOUT $1)"
      exit 1
    fi
    IMG_NAME=$DONGLE_NAME IMG_ADDR=$DONGLE_APP_ADDR LEN=$(stat -c%s "$IMG") CHK_ADDR=$DONGLE_ADDR APP_IMG=1
    if [ $((IMG_ADDR + LEN)) -gt $((DONGLE_APP_END)) ]; then
      echo "$1 would run to $(printf %#x $((IMG_ADDR + LEN))), into the installer record's sector at $DONGLE_APP_END"
      exit 1
    fi
    return 0
  fi
  if [ "$IMG_NAME" = "$DONGLE_NAME" ]; then
    IMG_ADDR=$DONGLE_ADDR LEN=$DONGLE_LEN
    # only the installed layout (PRCT / VM at absolute addresses, as the chip has it); the
    # .ufw's image (PRCT 0, VM 0x51000) would put VM inside the image at 0x10000
    if ! "$PY" -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import mkufw; from jl import sfc
fw = bytearray(open(sys.argv[2], "rb").read()); tops, ck, ab = mkufw.parse_top(fw, 0)
A = mkufw.app_region_size(fw, ab, ck); sfc(fw, ab, A, ab, ck); _, subs, _ = mkufw.parse_app(fw, ab)
off = {a["name"]: a["off"] for _, a in subs}
sys.exit(not (off.get("PRCT") == int(sys.argv[3], 0) and off.get("VM") == int(sys.argv[3], 0) + int(sys.argv[4], 0)))' \
        "$HERE" "$IMG" "$DONGLE_ADDR" "$DONGLE_LEN" 2>/dev/null; then
      echo "$1 is a receiver image, but not in the installed layout (PRCT at $DONGLE_ADDR, VM right after)."
      echo "Receiver images now are the app area alone: BOARD=dongle firmware/build.sh"
      exit 1
    fi
  fi
  if is_ufw "$IMG"; then
    echo "$1 is a UFW container, not a flash image. Flashing it leaves the chip unbootable."
    echo "Pass a raw flash image, e.g. firmware/build/c2.flash.bin"
    exit 1
  fi
  local sz; sz=$(stat -c%s "$IMG")
  if [ "$sz" -ne $((LEN)) ]; then
    echo "$1 is $sz bytes; a flash image is exactly $((LEN)) ($LEN)."
    echo "(firmware/build/<board>.flash.bin is the image firmware/build.sh writes)"
    exit 1
  fi
  if ! "$PY" -I "$HERE/mkufw.py" verify "$IMG" >/dev/null 2>&1; then
    echo "$1 fails 'mkufw.py verify' (bad JLFS header or CRCs): not a flashable image."
    echo "Run: python3 fw-re/mkufw.py verify $1"
    exit 1
  fi; }

enter_bl(){  # [pad|receiver]: that device into the bootloader (no target: the only one there)
  local target=${1:-} tsel=()
  case "$target" in "") ;; pad|receiver) tsel=(--target "$target") ;;
    *) echo "enter: pad or receiver, not '$target'"; exit 1 ;; esac
  set +e
  echo "[1/3] clearing usb_storage quirk"; echo > /sys/module/usb_storage/parameters/quirks 2>/dev/null; echo "    quirk now: '$(cat /sys/module/usb_storage/parameters/quirks)'"
  if lsusb 2>/dev/null | grep -qi 4c4a; then
    echo "[2/3] already in the bootloader (write / verify check which chip it is)"
  elif python3 -I "$HERE/../tools/c2log.py" --boot "${tsel[@]}"; then
    echo "[2/3] sent \"boot\" to the ${target:-device}'s USB debug log"
  elif python3 -I "$HERE/enter_bootloader.py" "${tsel[@]}" --yes; then
    echo "[2/3] sent enter-bootloader (vendor HID)"
  else
    echo "    Nothing to send the command to${target:+ on the $target} (or more than one device: name it)."
    echo "    Other ways in:"
    echo "     - the pad, rewrite images: hold Home + Options and plug in USB"
    echo "       (or hold both for 2 s while it runs with USB connected)"
    echo "     - the receiver: docs/dongle.md, \"a way back to the bootloader\""
    echo "     - any firmware, or none: tools/usbkey-mega2560/ (the ROM's USB key)"
    echo "    Waiting 15 s for the bootloader to appear..."
    for i in $(seq 1 50); do lsusb 2>/dev/null | grep -qi 4c4a && break; sleep 0.3; done
  fi
  echo "[3/3] waiting for bootloader to enumerate"
  for i in $(seq 1 50); do lsusb 2>/dev/null | grep -qi 4c4a && break; sleep 0.3; done
  echo "JieLi USB: $(lsusb 2>/dev/null | grep -i 4c4a || echo '(none - still not in bootloader)')"
  ensure_sg >/dev/null 2>&1; echo "bootloader sg device: $(find_sg || echo 'NOT FOUND')"
  set -e; }
image_target(){  # the device an image (need_image ran) is for
  [ "$IMG_NAME" = "$DONGLE_NAME" ] && echo receiver || echo pad; }

case "${1:-}" in
  bind) ensure_sg && echo "sg ready: $(find_sg)" ;;
  scan)
    echo "JieLi USB:"; lsusb 2>/dev/null | grep -i 4c4a || echo "  (none - not in bootloader?)"
    echo "sg device: $(find_sg || echo 'NOT FOUND')" ;;
  enter) enter_bl "${2:-}" ;;
  read)  [ -n "${2:-}" ] || { echo "usage: read OUT.bin"; exit 1; }
    out=$(realpath "$2"); SG=$(ensure_sg) || { echo "no sg"; exit 1; }
    jlt --device "$SG" "read $IMG_ADDR $LEN $out" ;;
  write) need_image "${2:-}"
    if ! lsusb 2>/dev/null | grep -qi 4c4a; then
      echo "== $(image_target) image: entering the bootloader on the $(image_target)"; enter_bl "$(image_target)"
    fi
    SG=$(ensure_sg) || { echo "no sg"; exit 1; }
    check_target "$SG" "${CHK_ADDR:-$IMG_ADDR}" "$IMG_NAME"
    echo "writing $IMG ($LEN bytes) at $IMG_ADDR ..."; jlt --device "$SG" "write $IMG_ADDR $IMG"
    tmp=$(mktemp); jlt --device "$SG" "read $IMG_ADDR $LEN $tmp"
    if cmp -s "$tmp" "$IMG"; then echo "WRITE VERIFIED -> resetting"; reset_chip "$SG" || true; else
      echo "!!! VERIFY FAILED - NOT resetting. Re-run write (still in bootloader)."; cmp "$tmp" "$IMG" | head; fi
    rm -f "$tmp" ;;
  verify) # read back and diff against a flash.bin:  verify FLASH.bin
    need_image "${2:-}"; SG=$(ensure_sg) || { echo "no sg"; exit 1; }
    tmp=$(mktemp); jlt --device "$SG" "read $IMG_ADDR $LEN $tmp"
    if cmp -s "$tmp" "$IMG"; then echo "VERIFY OK: chip matches $IMG"; else echo "VERIFY DIFF vs $IMG"; cmp "$tmp" "$IMG" | head; fi; rm -f "$tmp" ;;
  restore) # whole-chip receiver backup (IMG_ADDR=0 LEN=0x80000 read) back to the chip
    [ -f "${2:-}" ] || { echo "usage: restore FULL.bin (a 512 KB receiver backup)"; exit 1; }
    IMG=$(realpath "$2")
    [ "$(stat -c%s "$IMG")" -eq $((DONGLE_FULL)) ] || { echo "$2 is not $((DONGLE_FULL)) bytes: not a whole receiver backup"; exit 1; }
    [ "$(hdr_name "$IMG" "$DONGLE_ADDR")" = "$DONGLE_NAME" ] || { echo "$2 has no $DONGLE_NAME header at $DONGLE_ADDR: not a receiver backup"; exit 1; }
    tmp=$(mktemp); dd if="$IMG" of="$tmp" bs=4096 skip=$((DONGLE_ADDR / 4096)) count=$((DONGLE_LEN / 4096)) status=none
    "$PY" -I "$HERE/mkufw.py" verify "$tmp" >/dev/null 2>&1 || { echo "the image in $2 fails mkufw.py verify"; rm -f "$tmp"; exit 1; }
    rm -f "$tmp"
    SG=$(ensure_sg) || { echo "no sg"; exit 1; }
    check_target "$SG" "$DONGLE_ADDR" "$DONGLE_NAME"
    echo "restoring $IMG ($DONGLE_FULL bytes) at 0 ..."; jlt --device "$SG" "write 0 $IMG"
    tmp=$(mktemp); jlt --device "$SG" "read 0 $DONGLE_FULL $tmp"
    if cmp -s "$tmp" "$IMG"; then echo "RESTORE VERIFIED -> resetting"; reset_chip "$SG" || true; else
      echo "!!! VERIFY FAILED - NOT resetting. Re-run restore (still in bootloader)."; cmp "$tmp" "$IMG" | head; fi
    rm -f "$tmp" ;;
  reset) SG=$(ensure_sg) || { echo "no sg"; exit 1; }; reset_chip "$SG" ;;
  memread) [ -n "${3:-}" ] || { echo "usage: memread ADDR LEN"; exit 1; }
    SG=$(ensure_sg) || { echo "no sg"; exit 1; }
    tmp=$(mktemp); jlt --device "$SG" "memread $2 $3 $tmp" >/dev/null
    "$PY" -I -c 'import sys
d = open(sys.argv[1], "rb").read(); a = int(sys.argv[2], 0)
for i in range(0, len(d), 16): print("%08x  %s" % (a + i, d[i:i+16].hex(" ")))' "$tmp" "$2"
    rm -f "$tmp" ;;
  crumbs) # the receiver build's no-init RAM block (.dg_noinit, 0x9A24): what the last boots reached
    addr=${2:-}
    if [ -z "$addr" ]; then   # the block's address in the receiver build that is flashed (the ELF of the last build)
      elf=$HERE/../firmware/build/dongle.elf
      addr=$("${JL_TOOLCHAIN:-$HERE/jl-toolchain}/common/bin/objdump" -t "$elf" 2>/dev/null | awk '$NF == "dg_nv" {print "0x" $1; exit}')
      [ -n "$addr" ] || { echo "no dg_nv in $elf: pass the address (crumbs ADDR)"; exit 1; }
      echo "breadcrumbs at $addr (dg_nv in $elf)"
    fi
    SG=$(ensure_sg) || { echo "no sg"; exit 1; }
    tmp=$(mktemp); jlt --device "$SG" "memread $addr 40 $tmp" >/dev/null
    "$PY" -I -c 'import sys, struct
d = open(sys.argv[1], "rb").read()
print("raw:", d.hex(" "))
if len(d) >= 40:
    early = d[32:40]
    steps = ["startup done (app_load_common_code)", "update_result_get start", "update_result_get end", "guard (setup_arch)"]
    if early[0] == 0xA5:
        for i, st in enumerate(steps):
            print("  before main: %-38s %s" % (st, "reached" if early[i] == 0xA5 else "NOT reached"))
    else:
        print("  before main: startup never reached app_load_common_code (or an older build)")
magic, run, boots, rst = struct.unpack_from("<IIII", d)
if magic != 0x6447A5C3:
    print("no breadcrumbs (magic %08x): the firmware never reached its guard, or this RAM was overwritten" % magic)
    sys.exit(0)
print("magic ok: the firmware reached its guard (first in setup_arch)")
print("guard run %u (boots in a row without 10 s up)" % run)
if boots > 0xffff:
    print("(an older build: only magic and the run count)"); sys.exit(0)
print("boots %u, latest reset source %02x" % (boots, rst & 0xff))
names = {1: "guard", 2: "clocks done", 3: "board_init", 4: "power init done", 5: "devices_init done",
         6: "app started", 7: "USB host seen", 8: "stable 10 s", 0xEE: "guard -> bootloader",
         0xB0: "button held at power-up -> bootloader", 9: "Bluetooth initialised"}
for k in range(max(1, boots - 15), boots + 1):
    c = d[16 + (k & 15)]
    print("  boot %3u: last checkpoint %3u  %s" % (k, c, names.get(c, "")))' "$tmp"
    rm -f "$tmp" ;;
  *) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
