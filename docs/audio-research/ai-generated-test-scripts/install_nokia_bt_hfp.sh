#!/usr/bin/env bash
# Deploy the built nokia-bt-hfp.apk package directly to a running nokia-argon over SSH.
# Note: For new rootfs builds (`pmbootstrap install`), nokia-bt-hfp is pre-installed automatically via device-nokia-argon.
set -euo pipefail

ROOT_DIR="/home/elmot/hacking-nokia"
APK_LOCAL=$(ls -1t /home/elmot/.local/var/pmbootstrap/packages/v26.06/armv7/nokia-bt-hfp-*.apk 2>/dev/null | head -n1 || true)

TARGET_IP="${TARGET_IP:-172.16.42.1}"
TARGET_USER="${TARGET_USER:-elmot}"
TARGET_PASS="${TARGET_PASS:-q}"
SSH_OPTS=(-o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new)

if [ -z "$APK_LOCAL" ] || [ ! -f "$APK_LOCAL" ]; then
    echo "[ERROR] Missing built nokia-bt-hfp apk (run scripts/build_nokia_bt_hfp.sh first)" >&2
    exit 1
fi

echo "=== [1/2] Copying $(basename "$APK_LOCAL") to ${TARGET_USER}@${TARGET_IP}:/tmp/ ==="
scp "${SSH_OPTS[@]}" "$APK_LOCAL" "${TARGET_USER}@${TARGET_IP}:/tmp/nokia-bt-hfp.apk"

echo "=== [2/2] Installing nokia-bt-hfp.apk and restarting user service ==="
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "TARGET_PASS='${TARGET_PASS}' sh -s" <<'EOF'
set -eu
export XDG_RUNTIME_DIR="/run/user/$(id -u)"

printf '%s\n' "$TARGET_PASS" | sudo -S -v
sudo apk add --allow-untrusted --force-overwrite /tmp/nokia-bt-hfp.apk
rm -f /tmp/nokia-bt-hfp.apk

# Clean up any legacy manual overrides in /usr/local/bin or ~/.config
sudo rm -f /usr/local/bin/nokia-bt-hfp
rm -f "$HOME/.config/systemd/user/nokia-bt-hfp.service" \
      "$HOME/.config/wireplumber/wireplumber.conf.d/50-bluez-handset.conf" \
      "$HOME/.config/wireplumber/wireplumber.conf.d/50-disable-aloop.conf" \
      "$HOME/.config/pipewire/pipewire.conf.d/50-bt-quantum.conf"

systemctl --user daemon-reload
systemctl --user restart pipewire wireplumber nokia-bt-hfp.service
sleep 2
systemctl --user --no-pager status nokia-bt-hfp.service || true
/usr/bin/nokia-bt-hfp --ctl status || true
EOF
