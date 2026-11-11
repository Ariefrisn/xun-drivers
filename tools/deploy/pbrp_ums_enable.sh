#!/system/bin/sh
# Expose a GPT partition on Redmi Pad SE (xun) as USB Mass Storage (UMS) in PBRP/TWRP.
# Note: Always launch detached so USB reset cannot SIGHUP the caller, OR use PBRP UI
# (Mount -> Disable MTP / Mount USB Storage) if running interactively on tablet screen.
#
# Usage via ADB in PBRP:
#   adb push pbrp_ums_enable.sh /tmp/
#   adb shell "nohup sh /tmp/pbrp_ums_enable.sh win >/tmp/ums.log 2>&1 </dev/null &"
#   adb shell "nohup sh /tmp/pbrp_ums_enable.sh off >/tmp/ums.log 2>&1 </dev/null &"

TARGET="${1:-win}"
GADGET="/config/usb_gadget/g1"
CFG="$GADGET/configs/b.1"
MS="$GADGET/functions/mass_storage.0"

clear_lun() {
  for f in "$MS"/lun*/file /config/usb_gadget/*/functions/mass_storage*/lun*/file /sys/class/android_usb/android0/f_mass_storage/lun*/file; do
    [ -e "$f" ] && echo "" > "$f" 2>/dev/null || true
  done
}

unbind_links() {
  echo "" > "$GADGET/UDC" 2>/dev/null || echo "none" > "$GADGET/UDC" 2>/dev/null || true
  sleep 1
  for l in "$CFG/f1" "$CFG/f2" "$CFG/f3"; do
    if [ -L "$l" ] || [ -e "$l" ]; then
      unlink "$l" 2>/dev/null || rm -f "$l" 2>/dev/null || true
    fi
  done
}

if [ "$TARGET" = "off" ]; then
  if [ -d "$GADGET" ] && [ -d "$GADGET/functions/ffs.mtp" ] && [ -d "$GADGET/functions/ffs.adb" ]; then
    UDC="$(cat "$GADGET/UDC" 2>/dev/null)"
    [ -n "$UDC" ] && [ "$UDC" != "none" ] && [ "$UDC" != "" ] || UDC="$(getprop sys.usb.controller)"
    [ -n "$UDC" ] || UDC="$(ls /sys/class/udc 2>/dev/null | head -n 1)"
    unbind_links
    clear_lun
    echo 0x2717 > "$GADGET/idVendor"
    echo 0xff48 > "$GADGET/idProduct"
    echo "mtp_adb" > "$CFG/strings/0x409/configuration" 2>/dev/null || true
    ln -s "$GADGET/functions/ffs.mtp" "$CFG/f1"
    ln -s "$GADGET/functions/ffs.adb" "$CFG/f2"
    echo "$UDC" > "$GADGET/UDC"
    command -v twrp >/dev/null 2>&1 && twrp set tw_mtp_enabled 1 2>/dev/null || true
    start adbd 2>/dev/null || true
  else
    clear_lun
  fi
  echo "UMS cleared (MTP+ADB restored)."
  exit 0
fi

BLK="/dev/block/by-name/$TARGET"
[ -e "$BLK" ] || BLK="/dev/block/bootdevice/by-name/$TARGET"
[ -e "$BLK" ] || { echo "Partition block node not found for '$TARGET'" >&2; exit 1; }
REAL="$(readlink -f "$BLK")"

for m in $(awk -v d="$REAL" '$1==d{print $2}' /proc/mounts); do
  umount -f "$m" 2>/dev/null || umount -l "$m" 2>/dev/null || true
done

if [ -d "$MS" ]; then
  LUN=""
  for f in "$MS/lun.0/file" "$MS/lun0/file"; do
    [ -e "$f" ] && LUN="$f" && break
  done
  [ -n "$LUN" ] || { echo "No lun file in $MS" >&2; exit 1; }

  UDC="$(cat "$GADGET/UDC" 2>/dev/null)"
  [ -n "$UDC" ] && [ "$UDC" != "none" ] && [ "$UDC" != "" ] || UDC="$(getprop sys.usb.controller)"
  [ -n "$UDC" ] || UDC="$(ls /sys/class/udc 2>/dev/null | head -n 1)"

  # Disable PBRP MTP auto-restart and reset sys.usb.config=none so init does NOT race on 0x4E11
  command -v twrp >/dev/null 2>&1 && twrp set tw_mtp_enabled 0 2>/dev/null || true
  setprop sys.usb.config none 2>/dev/null || true
  sleep 2
  unbind_links

  DIR="$(dirname "$LUN")"
  [ -e "$DIR/ro" ] && echo 0 > "$DIR/ro" 2>/dev/null || true
  [ -e "$DIR/removable" ] && echo 1 > "$DIR/removable" 2>/dev/null || true
  [ -e "$DIR/cdrom" ] && echo 0 > "$DIR/cdrom" 2>/dev/null || true
  echo "" > "$LUN" 2>/dev/null || true
  echo "$REAL" > "$LUN"

  # Start adbd without touching sys.usb.config; wait briefly for FunctionFS endpoints
  start adbd 2>/dev/null || true
  for _w in 1 2 3 4; do
    [ "$(getprop sys.usb.ffs.ready 2>/dev/null)" = "1" ] && break
    sleep 1
  done

  echo 0x18D1 > "$GADGET/idVendor"
  echo 0x9025 > "$GADGET/idProduct"
  echo "ums_adb" > "$CFG/strings/0x409/configuration" 2>/dev/null || true
  ln -s "$MS" "$CFG/f1" 2>/dev/null || true
  [ -d "$GADGET/functions/ffs.adb" ] && ln -s "$GADGET/functions/ffs.adb" "$CFG/f2" 2>/dev/null || true

  bound=0
  if echo "$UDC" > "$GADGET/UDC" 2>/dev/null; then
    sleep 1
    cur_udc="$(cat "$GADGET/UDC" 2>/dev/null)"
    cur_pid="$(cat "$GADGET/idProduct" 2>/dev/null)"
    cur_f1="$(readlink "$CFG/f1" 2>/dev/null)"
    if [ "$cur_udc" = "$UDC" ] && echo "$cur_pid" | grep -qi "9025" && echo "$cur_f1" | grep -q "mass_storage"; then
      bound=1
    fi
  fi
  if [ "$bound" = "1" ]; then
    echo "UMS active (ConfigFS 18D1:9025 + ADB): $TARGET ($REAL) -> $LUN"
    exit 0
  fi

  # Fallback to pure UMS (18D1:9024) if composite ADB FunctionFS bind fails
  unbind_links
  echo "" > "$LUN" 2>/dev/null || true
  echo "$REAL" > "$LUN"
  ln -s "$MS" "$CFG/f1" 2>/dev/null || true
  echo 0x18D1 > "$GADGET/idVendor"
  echo 0x9024 > "$GADGET/idProduct"
  echo "ums" > "$CFG/strings/0x409/configuration" 2>/dev/null || true
  echo "$UDC" > "$GADGET/UDC" 2>/dev/null || true
  echo "UMS active (pure UMS fallback, ConfigFS 18D1:9024): $TARGET ($REAL) -> $LUN"
  exit 0
fi

LUN=""
for f in /sys/class/android_usb/android0/f_mass_storage/lun0/file /sys/class/android_usb/android0/f_mass_storage/lun/file; do
  [ -e "$f" ] && LUN="$f" && break
done
[ -n "$LUN" ] || { echo "No USB mass_storage lun file found." >&2; exit 1; }
echo "" > "$LUN" 2>/dev/null || true
echo "$REAL" > "$LUN"
echo "UMS active: $TARGET ($REAL) -> $LUN"
