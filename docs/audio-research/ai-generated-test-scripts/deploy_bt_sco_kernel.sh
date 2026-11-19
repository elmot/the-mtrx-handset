#!/usr/bin/env bash
# Deploy the patched INT_BT_SCO kernel + DTB packages to nokia-argon over SSH
# without rebooting (user triggers reboot/power cycle when ready).
set -euo pipefail

TARGET_IP="${TARGET_IP:-172.16.42.1}"
TARGET_USER="${TARGET_USER:-elmot}"
TARGET_PASS="${TARGET_PASS:-q}"

PKG_DIR="${PKG_DIR:-/home/elmot/.local/var/pmbootstrap/packages/v26.06/armv7}"
KERNEL_APK="${PKG_DIR}/linux-postmarketos-qcom-msm8916-6.12.1_p20261119154522-r6.apk"
DEVICE_APK="${PKG_DIR}/device-nokia-argon-10-r3.apk"
FW_APK="${PKG_DIR}/device-nokia-argon-nonfree-firmware-10-r3.apk"

SSH_OPTS=(-o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new)

for f in "$KERNEL_APK" "$DEVICE_APK" "$FW_APK"; do
    if [ ! -f "$f" ]; then
        echo "[ERROR] Missing package: $f" >&2
        exit 1
    fi
done

echo "=== [1/4] Checking /boot free space and backing up current /boot to /home/${TARGET_USER}/backup-boot-r5 ==="
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "TARGET_PASS='${TARGET_PASS}' sh -s" <<'EOF'
set -eu
printf '%s\n' "$TARGET_PASS" | sudo -S -v

df -h /boot
avail_kb="$(df -Pk /boot | awk 'NR==2 {print $4}')"
echo "Available /boot KB: ${avail_kb}"

mkdir -p "$HOME/backup-boot-r5"
if [ ! -f "$HOME/backup-boot-r5/boot.img" ] && [ -f /boot/boot.img ]; then
    sudo cp -a /boot/boot.img /boot/vmlinuz /boot/initramfs /boot/qcom-msm8905-nokia-argon.dtb "$HOME/backup-boot-r5/"
    sudo chown -R "$(id -u):$(id -g)" "$HOME/backup-boot-r5"
    echo "Backed up /boot artifacts to $HOME/backup-boot-r5"
fi

if [ "$avail_kb" -lt 20480 ]; then
    echo "[WARN] Less than 20 MB free on /boot (${avail_kb} KB); checking old backups in /boot..."
    ls -la /boot
    exit 2
fi
EOF

echo "=== [2/4] Copying built .apk packages to ${TARGET_USER}@${TARGET_IP}:/tmp/ ==="
scp "${SSH_OPTS[@]}" "$KERNEL_APK" "$DEVICE_APK" "$FW_APK" "${TARGET_USER}@${TARGET_IP}:/tmp/"

echo "=== [3/4] Installing packages offline with apk add --no-network --allow-untrusted ==="
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "TARGET_PASS='${TARGET_PASS}' sh -s" <<'EOF'
set -eu
printf '%s\n' "$TARGET_PASS" | sudo -S -v

sudo apk add --no-network --allow-untrusted \
    /tmp/linux-postmarketos-qcom-msm8916-6.12.1_p20261119154522-r6.apk \
    /tmp/device-nokia-argon-10-r3.apk \
    /tmp/device-nokia-argon-nonfree-firmware-10-r3.apk

rm -f /tmp/linux-postmarketos-qcom-msm8916-*.apk /tmp/device-nokia-argon-*.apk

# Ensure /boot DTB matches the updated /usr/share/nokia-argon/ DTB even on upgrade
if [ -f /usr/share/nokia-argon/qcom-msm8905-nokia-argon.dtb ]; then
    need_mkinitfs=0
    if ! cmp -s /usr/share/nokia-argon/qcom-msm8905-nokia-argon.dtb /boot/qcom-msm8905-nokia-argon.dtb 2>/dev/null; then
        sudo cp /usr/share/nokia-argon/qcom-msm8905-nokia-argon.dtb /boot/qcom-msm8905-nokia-argon.dtb
        need_mkinitfs=1
    fi
    if ! cmp -s /usr/share/nokia-argon/qcom-msm8905-nokia-argon.dtb /boot/dtbs/qcom-msm8905-nokia-argon.dtb 2>/dev/null; then
        sudo cp /usr/share/nokia-argon/qcom-msm8905-nokia-argon.dtb /boot/dtbs/qcom-msm8905-nokia-argon.dtb
        need_mkinitfs=1
    fi
    if [ "$need_mkinitfs" -eq 1 ]; then
        echo "DTB differed in /boot; re-running mkinitfs..."
        sudo mkinitfs
    fi
fi

sudo sync
EOF

echo "=== [4/4] Verifying installed kernel, modules, and DTB on phone ==="
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" '
set -eu
ls -lh /boot/vmlinuz /boot/initramfs /boot/boot.img /boot/qcom-msm8905-nokia-argon.dtb /boot/dtbs/qcom-msm8905-nokia-argon.dtb /usr/share/nokia-argon/qcom-msm8905-nokia-argon.dtb
find /lib/modules -name "snd-soc-bt-sco*" -o -name "q6afe*" -o -name "q6routing*" -o -name "snd-soc-apq8016-sbc*" | sort
strings /boot/dtbs/qcom-msm8905-nokia-argon.dtb | grep -E "linux,bt-sco|dai@13[67]|backend[23]-dai-link" || true
cat /boot/extlinux/extlinux.conf 2>/dev/null || true
'
echo "=== DONE: Packages deployed. Please reboot nokia-argon to boot the new kernel. ==="
