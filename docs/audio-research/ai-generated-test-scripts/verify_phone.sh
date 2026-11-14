#!/usr/bin/env bash
set -uo pipefail

export PATH="$HOME/platform-tools:$HOME/hack-nokia/pmbootstrap:$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

echo "=== 1. Host Utilities & Environment Check ==="
for tool in ssh scp ip lsusb dtc git curl python3 pmbootstrap.py fastboot adb; do
    if command -v "$tool" >/dev/null 2>&1; then
        echo "  [OK]      $tool -> $(command -v "$tool")"
    else
        echo "  [MISSING] $tool"
    fi
done

echo ""
echo "=== 2. Check Home Directory for pmbootstrap / pmaports / Nura Workdirs ==="
find "$HOME" -maxdepth 3 \( -name "pmbootstrap*" -o -name "pmaports*" -o -name "*nura*" \) 2>/dev/null || true

echo ""
echo "=== 3. USB & Host Network Interface Check ==="
ip -br addr show enxeaaffb8e7230 2>/dev/null || ip -br addr show
TARGET_IP="172.16.42.1"
TARGET_USER="elmot"

echo ""
echo "=== 4. SSH Target Verification (${TARGET_USER}@${TARGET_IP}) ==="
SSH_OPTS=(-o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new -o BatchMode=yes)
if ssh "${SSH_OPTS[@]}" "root@${TARGET_IP}" "true" 2>/dev/null; then
    echo "  [OK] Passwordless root SSH connected to root@${TARGET_IP}"
fi
if ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "true" 2>/dev/null; then
    echo "  [OK] Passwordless SSH connected to ${TARGET_USER}@${TARGET_IP}"
    DTB_LOCAL="/home/elmot/hacking-nokia/src/linux-msm8916/arch/arm/boot/dts/qcom/qcom-msm8905-nokia-argon.dtb"
    if [ -f "$DTB_LOCAL" ]; then
        scp "${SSH_OPTS[@]}" "$DTB_LOCAL" "${TARGET_USER}@${TARGET_IP}:/tmp/qcom-msm8905-nokia-argon.dtb" && \
            echo "  [OK] Staged compiled DTB to ${TARGET_USER}@${TARGET_IP}:/tmp/qcom-msm8905-nokia-argon.dtb"
    fi

    NEEDS_REBOOT="$(ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" '
        if sudo -n true 2>/dev/null && [ -f /tmp/qcom-msm8905-nokia-argon.dtb ]; then
            if ! cmp -s /tmp/qcom-msm8905-nokia-argon.dtb /boot/qcom-msm8905-nokia-argon.dtb; then
                echo "INSTALLING"
                for f in $(sudo find /boot /usr /lib -name "qcom-msm8905-nokia-argon.dtb" 2>/dev/null); do
                    if [ ! -f "${f}.orig" ]; then
                        sudo cp "$f" "${f}.orig"
                    fi
                    sudo cp /tmp/qcom-msm8905-nokia-argon.dtb "$f"
                done
                if [ ! -f /boot/boot.img.orig ] && [ -f /boot/boot.img ]; then
                    sudo cp /boot/boot.img /boot/boot.img.orig
                fi
                sudo mkinitfs >&2
                sudo cp /tmp/qcom-msm8905-nokia-argon.dtb /boot/qcom-msm8905-nokia-argon.dtb
                sudo cp /tmp/qcom-msm8905-nokia-argon.dtb /boot/dtbs/qcom-msm8905-nokia-argon.dtb
                sudo sync
            fi
        fi
    ')"

    if [[ "$NEEDS_REBOOT" == *"INSTALLING"* ]]; then
        echo "  [OK] Installed updated DTB and regenerated boot files; rebooting phone..."
        ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "sudo reboot" 2>/dev/null || true
        sleep 5
        for i in $(seq 1 35); do
            sleep 2
            if ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "true" 2>/dev/null; then
                echo "  [OK] Phone reconnected after reboot! Waiting 5s for ASoC/QDSP6 deferred probes..."
                sleep 5
                break
            fi
        done
    fi

    ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" '
        echo "--- Gate 0: Root Privilege & Bootloader Check ---"
        id
        sudo -n true 2>/dev/null && echo "SUDO: nopasswd OK" || echo "SUDO: password required"
        doas -n true 2>/dev/null && echo "DOAS: nopasswd OK" || echo "DOAS: password required / not configured"
        ls -la /etc/sudoers.d /etc/doas.d 2>/dev/null || true
        tr "\0" " " < /sys/firmware/devicetree/base/chosen/lk2nd,project 2>/dev/null; echo ""
        tr "\0" " " < /sys/firmware/devicetree/base/chosen/lk2nd,version 2>/dev/null; echo ""

        echo "--- Gate 1: OS, Kernel, Boot & Mounts ---"
        cat /etc/os-release
        uname -a
        grep -E " / | /boot " /proc/mounts
        ls -la /boot
        apk list -I 2>/dev/null | grep -E "^(linux-|device-|firmware-)" || true

        echo "--- Gate 2: Wi-Fi ---"
        ip -o link show wlan0
        iw dev wlan0 info 2>/dev/null || true

        echo "--- Gate 3: Bluetooth ---"
        ls -la /sys/class/bluetooth/
        timeout 3 bluetoothctl list 2>/dev/null || true

        echo "--- Gate 4: Device Tree (/sys/firmware/devicetree/base) ---"
        echo "Top-level DT model & compatible:"
        tr "\0" " " < /sys/firmware/devicetree/base/model; echo ""
        tr "\0" " " < /sys/firmware/devicetree/base/compatible; echo ""
        echo "Root DT nodes:"
        ls /sys/firmware/devicetree/base/
        echo "/soc@0 nodes:"
        ls /sys/firmware/devicetree/base/soc@0/ 2>/dev/null || true
        echo "SPMI PMIC nodes:"
        ls /sys/firmware/devicetree/base/soc@0/spmi@*/pmic@*/ 2>/dev/null || true
        echo "Audio / Codec / LPASS DT nodes:"
        find /sys/firmware/devicetree/base -maxdepth 4 \( -name "sound*" -o -name "*codec*" -o -name "apr*" \) 2>/dev/null || true

        echo "--- Gate 5: ALSA / ASoC Sound & Display Subsystem ---"
        ls -la /dev/fb* /dev/dri* /lib/firmware/nokia* /sys/bus/spi/devices/ 2>/dev/null || true
        lsmod | grep -E "panel|mipi|drm|spi|snd|wcd|q6|apr" || true
        cat /proc/asound/cards 2>/dev/null || true
        cat /proc/asound/pcm 2>/dev/null || true
        ls -la /dev/snd /sys/class/sound 2>/dev/null || true
        echo "Deferred devices (/sys/kernel/debug/devices_deferred):"
        sudo -n cat /sys/kernel/debug/devices_deferred 2>/dev/null || true
        echo "Audio / QDSP6 / WCD / ASoC dmesg logs:"
        sudo -n dmesg | grep -iE "snd|sound|asoc|wcd|codec|qdsp|q6|apr|lpass|8916|8909|7702000|771c000|f000" | tail -n 50 || true
    '
else
    echo "  [FAIL] Cannot connect to ${TARGET_USER}@${TARGET_IP}"
    ping -c 1 -W 2 "${TARGET_IP}" || true
    ssh -o ConnectTimeout=3 -o BatchMode=yes "${TARGET_USER}@${TARGET_IP}" "true" || true
    adb devices -l || true
    fastboot devices || true
    lsusb || true
    exit 1
fi
