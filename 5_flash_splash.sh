#!/bin/sh
set +x
. "$(dirname "$0")"/activate.sh || exit 1
printf "Update splash screen from /splash/splash.png? [y/N]: "
read -r _ans
case "$_ans" in
    [Yy])
        ./splash/build_splash.sh
        ;;
    *)
        echo "Pre-composed splash screen is going to be flashed"
        ;;
esac

echo "Flashing splash screen..."
./splash/fastboot_flash_splash.sh
echo "Flashing completed. Rebooting..."
fastboot reboot
