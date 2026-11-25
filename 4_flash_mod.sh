#!/bin/sh
set +x
. "$(dirname "$0")"/activate.sh || exit 1
echo "Prepare your Nokia 8110:"
echo "- Flash lk2nd bootloader as described here: https://wiki.nura.eco/wiki/Nokia_8110_4G_(nokia-argon)#Installing_lk2nd"
echo "Reboot to fastboot mode: long press power(~10 sec) button, holding Down button. The phone shall enter lk2nd fastboot mode"
echo "Connect the phone to this computer with USB cable"
echo "Flashing kernel..."
pmbootstrap flasher flash_kernel
echo "Flashing rootfs. This process must take about 5 minutes..."
pmbootstrap flasher flash_rootfs
echo "Clean up..."
pmbootstrap shutdown
echo "Flashing completed. Rebooting..."
fastboot reboot
