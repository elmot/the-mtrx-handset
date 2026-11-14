#!/usr/bin/env bash
set -euo pipefail

TARGET_IP="172.16.42.1"
TARGET_USER="elmot"
DTB_LOCAL="/home/elmot/hacking-nokia/src/linux-msm8916/arch/arm/boot/dts/qcom/qcom-msm8905-nokia-argon.dtb"
SSH_OPTS=(-o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new -o BatchMode=yes)

if [[ ! -f "$DTB_LOCAL" ]]; then
    echo "[ERROR] Compiled DTB not found at $DTB_LOCAL" >&2
    exit 1
fi

echo "[1/4] Copying compiled DTB to ${TARGET_USER}@${TARGET_IP}:/tmp/qcom-msm8905-nokia-argon.dtb..."
scp "${SSH_OPTS[@]}" "$DTB_LOCAL" "${TARGET_USER}@${TARGET_IP}:/tmp/qcom-msm8905-nokia-argon.dtb"

echo "[2/4] Backing up original DTB and installing new DTB into /boot..."
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" '
    set -e
    if [ ! -f /boot/qcom-msm8905-nokia-argon.dtb.orig ]; then
        sudo cp /boot/qcom-msm8905-nokia-argon.dtb /boot/qcom-msm8905-nokia-argon.dtb.orig
    fi
    if [ ! -f /boot/boot.img.orig ]; then
        sudo cp /boot/boot.img /boot/boot.img.orig
    fi
    sudo cp /tmp/qcom-msm8905-nokia-argon.dtb /boot/dtbs/qcom-msm8905-nokia-argon.dtb
    sudo cp /tmp/qcom-msm8905-nokia-argon.dtb /boot/qcom-msm8905-nokia-argon.dtb
    sudo mkinitfs
    sudo sync
    ls -la /boot/qcom-msm8905-nokia-argon.dtb* /boot/dtbs/qcom-msm8905-nokia-argon.dtb
'

echo "[3/4] Rebooting phone..."
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" 'sudo reboot' || true

echo "[4/4] Waiting for phone to reboot and reconnect over SSH..."
for i in $(seq 1 30); do
    sleep 2
    if ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "true" 2>/dev/null; then
        echo "  [OK] Phone is back online!"
        exit 0
    fi
done

echo "[WARN] Phone did not reconnect within 60s." >&2
exit 1

