#!/bin/bash
# Switch the default OS of the Redmi Pad SE (xun) (tablet in fastboot or Android/PBRP).
# Option 1 Dual-OS slot layout:
#   xun-boot.sh windows   -> slot a (boot_a = Mu UEFI -> Windows 11 ARM64)
#   xun-boot.sh android   -> slot b (boot_b = LineageOS 16, recovery_b = PBRP 4.0)
# Uses only ABL's own `fastboot set_active` (swaps partition type GUIDs + attributes together).
set -e
case "$1" in
  windows) SLOT=a ;;
  android) SLOT=b ;;
  *) echo "usage: $0 windows|android" >&2; exit 1 ;;
esac
if ! fastboot devices | grep -q fastboot; then
  adb reboot bootloader
  for i in $(seq 1 60); do fastboot devices | grep -q fastboot && break; sleep 1; done
fi
[ "$(fastboot getvar is-userspace 2>&1 | awk '/is-userspace/{print $2}')" = "no" ] || { echo "not in bootloader fastboot" >&2; exit 1; }
fastboot set_active $SLOT
fastboot reboot
