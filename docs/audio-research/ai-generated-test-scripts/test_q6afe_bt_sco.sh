#!/usr/bin/env bash
# Test Q6AFE INT_BT_SCO_RX (MultiMedia1) and INT_BT_SCO_TX (MultiMedia2) ALSA paths on nokia-argon.
set -euo pipefail

TARGET_IP="${TARGET_IP:-172.16.42.1}"
TARGET_USER="${TARGET_USER:-elmot}"
TARGET_PASS="${TARGET_PASS:-q}"
SSH_OPTS=(-o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new)

# Generate a 2-second 1000 Hz S16_LE 8000 Hz mono WAV locally
python3 - <<'PYEOF'
import math, struct, wave
rate = 8000
dur = 2
freq = 1000.0
with wave.open('/tmp/tone_1khz_8k.wav', 'wb') as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(rate)
    frames = bytearray()
    for i in range(rate * dur):
        val = int(12000 * math.sin(2.0 * math.pi * freq * i / rate))
        frames += struct.pack('<h', val)
    w.writeframes(frames)
PYEOF

scp "${SSH_OPTS[@]}" /tmp/tone_1khz_8k.wav "${TARGET_USER}@${TARGET_IP}:/tmp/tone_1khz_8k.wav"

ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "TARGET_PASS='${TARGET_PASS}' sh -s" <<'EOF'
set -u
export XDG_RUNTIME_DIR="/run/user/$(id -u)"

echo "=== Available tools for BT testing ==="
for cmd in python3 bluetoothctl btmon hciconfig dbus-send; do
    command -v "$cmd" || echo "missing: $cmd"
done
python3 -c 'import socket; s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_SCO); print("BTPROTO_SCO ok:", s.fileno()); s.close()' 2>&1 || true
python3 -c 'import dbus; print("python3-dbus ok")' 2>&1 || true
python3 -c 'import gi; print("python3-gobject ok")' 2>&1 || true

echo "=== MultiMedia2 INT_BT_SCO mixer control ==="
amixer -c msm8916 controls | grep -i "INT_BT_SCO"

echo "=== Pausing PipeWire/WirePlumber temporarily ==="
systemctl --user stop wireplumber pipewire-pulse pipewire || true

restore_audio() {
    amixer -c msm8916 cset name='INT_BT_SCO_RX Audio Mixer MultiMedia1' 0 >/dev/null 2>&1 || true
    amixer -c msm8916 cset name='MultiMedia2 Mixer INT_BT_SCO_TX' 0 >/dev/null 2>&1 || true
    amixer -c msm8916 cset name='PRI_MI2S_RX Audio Mixer MultiMedia1' 1 >/dev/null 2>&1 || true
    amixer -c msm8916 cset name='MultiMedia2 Mixer TERT_MI2S_TX' 1 >/dev/null 2>&1 || true
    systemctl --user start pipewire pipewire-pulse wireplumber || true
}
trap restore_audio EXIT

# Route MultiMedia1 -> INT_BT_SCO_RX and INT_BT_SCO_TX -> MultiMedia2 exclusively for this check
amixer -c msm8916 cset name='PRI_MI2S_RX Audio Mixer MultiMedia1' 0
amixer -c msm8916 cset name='INT_BT_SCO_RX Audio Mixer MultiMedia1' 1
amixer -c msm8916 cset name='MultiMedia2 Mixer TERT_MI2S_TX' 0
amixer -c msm8916 cset name='MultiMedia2 Mixer INT_BT_SCO_TX' 1

DMESG_BEFORE="$(printf '%s\n' "$TARGET_PASS" | sudo -S dmesg | wc -l)"

echo "=== Testing aplay (MultiMedia1 -> INT_BT_SCO_RX) & arecord (INT_BT_SCO_TX -> MultiMedia2) ==="
arecord -D hw:msm8916,1 -f S16_LE -r 8000 -c 1 -d 2 /tmp/cap_bt_sco.wav &
CAP_PID=$!
aplay -D hw:msm8916,0 -f S16_LE -r 8000 -c 1 /tmp/tone_1khz_8k.wav
PLAY_RC=$?
wait "$CAP_PID"
CAP_RC=$?

echo "aplay exit=$PLAY_RC, arecord exit=$CAP_RC"
ls -lh /tmp/cap_bt_sco.wav || true

echo "=== New kernel dmesg during Q6AFE INT_BT_SCO start/stop ==="
printf '%s\n' "$TARGET_PASS" | sudo -S dmesg | tail -n +"$((DMESG_BEFORE + 1))" || true
EOF
