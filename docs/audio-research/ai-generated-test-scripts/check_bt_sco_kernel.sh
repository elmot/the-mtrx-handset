#!/usr/bin/env bash
# Inspect booted kernel, DTB, loaded sound modules, dmesg, and ALSA controls on nokia-argon.
set -euo pipefail

TARGET_IP="${TARGET_IP:-172.16.42.1}"
TARGET_USER="${TARGET_USER:-elmot}"
TARGET_PASS="${TARGET_PASS:-q}"
SSH_OPTS=(-o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new)

ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "TARGET_PASS='${TARGET_PASS}' sh -s" <<'EOF'
set -eu

echo "=== Kernel & DTB ==="
uname -a
cat /proc/version
if [ -d /proc/device-tree/bt-sco ]; then
    printf "DT bt-sco compatible: "
    tr '\0' ' ' </proc/device-tree/bt-sco/compatible || true
    printf "\n"
else
    echo "DT /proc/device-tree/bt-sco: NOT PRESENT"
fi

echo "=== Sound & BT Modules ==="
lsmod | grep -iE "bt|q6|apq|lpass|wcn|snd" || true

echo "=== ASoc / Q6AFE / BT-SCO dmesg ==="
printf '%s\n' "$TARGET_PASS" | sudo -S -v
sudo dmesg | grep -iE "asoc|qdsp6|q6afe|q6routing|bt[-_ ]sco|apq8016|wcn|btqcom|msm8916" | tail -n 80 || true

echo "=== ALSA Cards & Devices ==="
cat /proc/asound/cards || true
aplay -l || true
arecord -l || true

echo "=== ALSA bt_sco / MultiMedia Controls ==="
amixer -c msm8916 controls 2>/dev/null | grep -iE "bt|sco|multimedia" | head -n 80 || true
EOF
