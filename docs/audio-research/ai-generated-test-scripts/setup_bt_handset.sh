#!/usr/bin/env bash
# Configure Nokia 8110 4G (nokia-argon) over SSH as a Bluetooth A2DP + HFP-HF handset.
# Usage:
#   ./scripts/setup_bt_handset.sh           # Configure running phone
#   ./scripts/setup_bt_handset.sh --reboot  # Reboot phone first, wait for SSH, then configure
set -euo pipefail

TARGET_IP="${TARGET_IP:-172.16.42.1}"
TARGET_USER="${TARGET_USER:-elmot}"
TARGET_PASS="${TARGET_PASS:-q}"
BT_ALIAS="${BT_ALIAS:-nokia-argon}"
REBOOT_FIRST=0

if [ "${1:-}" = "--reboot" ]; then
    REBOOT_FIRST=1
fi

SSH_OPTS=(-o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new)

if [ "$REBOOT_FIRST" = 1 ]; then
    echo "=== Rebooting ${TARGET_USER}@${TARGET_IP} ==="
    ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "printf '%s\n' '${TARGET_PASS}' | sudo -S reboot" || true
    echo "Waiting for phone to come back online..."
    sleep 8
    for _ in $(seq 1 40); do
        if ssh "${SSH_OPTS[@]}" -o BatchMode=yes "${TARGET_USER}@${TARGET_IP}" "true" 2>/dev/null; then
            break
        fi
        sleep 2
    done
    sleep 3
fi

echo "=== Configuring Bluetooth Handset on ${TARGET_USER}@${TARGET_IP} ==="
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "BT_ALIAS='${BT_ALIAS}' TARGET_PASS='${TARGET_PASS}' sh -s" <<'EOF'
set -eu

export XDG_RUNTIME_DIR="/run/user/$(id -u)"

echo "--- [1/4] Refreshing sudo credentials ---"
printf '%s\n' "$TARGET_PASS" | sudo -S -v

echo "--- [2/4] Installing D-Bus BlueZ policy for PipeWire/WirePlumber ---"
sudo mkdir -p /etc/dbus-1/system.d
sudo tee /etc/dbus-1/system.d/bluetooth-pipewire.conf >/dev/null <<'DBUS_EOF'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <policy user="elmot">
    <allow send_destination="org.bluez"/>
  </policy>
</busconfig>
DBUS_EOF
sudo systemctl reload dbus

echo "--- [3/4] Configuring WirePlumber (no seat-monitoring, A2DP Sink + HFP HF) ---"
mkdir -p ~/.config/pipewire/pipewire.conf.d
cat <<'PW_EOF' > ~/.config/pipewire/pipewire.conf.d/50-bt-quantum.conf
context.properties = {
  default.clock.min-quantum = 1024
  default.clock.quantum = 2048
}
PW_EOF

mkdir -p ~/.config/wireplumber/wireplumber.conf.d
rm -f ~/.config/wireplumber/wireplumber.conf.d/50-bluez-no-seat.conf \
      ~/.config/wireplumber/wireplumber.conf.d/50-bluez-speaker.conf

cat <<'WP_EOF' > ~/.config/wireplumber/wireplumber.conf.d/50-bluez-handset.conf
wireplumber.profiles = {
  main = {
    monitor.bluez.seat-monitoring = disabled
  }
}

monitor.bluez.properties = {
  bluez5.roles = [ a2dp_sink hfp_hf ]
  bluez5.codecs = [ sbc ]
  bluez5.hfphsp-backend = "native"
}
WP_EOF

systemctl --user restart wireplumber pipewire pipewire-pulse
for _ in $(seq 1 15); do
    if pactl list short sources 2>/dev/null | grep -q "HiFi__Mic1__source"; then
        break
    fi
    sleep 1
done

pactl set-default-sink alsa_output.platform-7702000.sound.HiFi__Speaker__sink || true
pactl set-default-source alsa_input.platform-7702000.sound.HiFi__Mic1__source || true

echo "--- [4/4] Enabling Bluetooth controller (${BT_ALIAS}) ---"
sudo rfkill unblock bluetooth || true
sudo systemctl start bluetooth

bluetoothctl <<BT_EOF
system-alias ${BT_ALIAS}
power on
pairable on
discoverable-timeout 0
discoverable on
BT_EOF

echo ""
echo "=== Verification ==="
bluetoothctl show | grep -E "Controller|Name|Alias|Powered|Discoverable|Pairable|Audio Sink|Handsfree|0000110b|0000111e" || true
echo ""
wpctl status
EOF
