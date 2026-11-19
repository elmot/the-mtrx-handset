#!/usr/bin/env bash
# Pair/connect this Linux host to nokia-argon over Bluetooth, play a test tone,
# and inspect PipeWire/WirePlumber/BlueZ state on both sides.
set -euo pipefail

TARGET_IP="${TARGET_IP:-172.16.42.1}"
TARGET_USER="${TARGET_USER:-elmot}"
NOKIA_BT_MAC="${NOKIA_BT_MAC:-02:00:34:40:12:1E}"
SSH_OPTS=(-o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new)

echo "=== 1. Check Nokia state over SSH ==="
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" '
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
echo "--- Nokia bluetoothctl show ---"
bluetoothctl show | grep -E "Controller|Name|Alias|Powered|Discoverable|Pairable|Audio Sink|Handsfree|0000110b|0000111e" || true
echo "--- Nokia wpctl status ---"
wpctl status
echo "--- Nokia wireplumber recent logs ---"
journalctl --user -u wireplumber -u pipewire -n 15 --no-pager || true
'

echo ""
echo "=== 2. Check Host Bluetooth & Pair/Connect to ${NOKIA_BT_MAC} ==="
HOST_BT_MAC="$(bluetoothctl show | awk '/^Controller / { print $2; exit }')"
echo "Host BT Controller: ${HOST_BT_MAC:-NONE}"
if [ -z "${HOST_BT_MAC:-}" ]; then
    echo "ERROR: No Bluetooth controller found on host."
    exit 1
fi

bluetoothctl power on >/dev/null || true
bluetoothctl pairable on >/dev/null || true

if ! bluetoothctl info "${NOKIA_BT_MAC}" 2>/dev/null | grep -q "Paired: yes"; then
    echo "Cleaning any stale partial pairing and scanning for ${NOKIA_BT_MAC}..."
    bluetoothctl remove "${NOKIA_BT_MAC}" >/dev/null 2>&1 || true
    ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "bluetoothctl remove '${HOST_BT_MAC}' >/dev/null 2>&1 || true"
    timeout 8 bluetoothctl --timeout 6 scan on >/dev/null 2>&1 || true
    echo "Starting temporary NoInputNoOutput agent on Nokia..."
    ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "bluetoothctl pairable on >/dev/null && bluetoothctl discoverable on >/dev/null"
    (
        echo "default-agent"
        sleep 25
    ) | ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "bluetoothctl --agent NoInputNoOutput" >/tmp/nokia_bt_agent.log 2>&1 &
    NOKIA_AGENT_PID=$!
    sleep 2
    echo "Pairing ${NOKIA_BT_MAC}..."
    (
        echo "default-agent"
        echo "pair ${NOKIA_BT_MAC}"
        sleep 10
    ) | bluetoothctl --agent NoInputNoOutput || true
    kill "${NOKIA_AGENT_PID}" 2>/dev/null || true
    echo "--- Nokia agent log ---"
    cat /tmp/nokia_bt_agent.log || true
fi

bluetoothctl trust "${NOKIA_BT_MAC}" || true
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "bluetoothctl trust '${HOST_BT_MAC}' && bluetoothctl disconnect B0:D5:FB:A6:13:9A" >/dev/null 2>&1 || true

sleep 1
echo "Connecting to ${NOKIA_BT_MAC}..."
bluetoothctl connect "${NOKIA_BT_MAC}" || true
sleep 3

echo ""
echo "=== 3. Host PipeWire Sinks & Cards ==="
wpctl status | grep -A 15 "Sinks:" || true
pactl list cards | grep -A 30 "nokia-argon" || true

python3 - <<'PYEOF'
import math, struct, wave
def make_tone(path, freq):
    rate = 48000
    n = int(rate * 2.0)
    with wave.open(path, "w") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(n):
            v = int(32767 * 0.4 * math.sin(2 * math.pi * freq * i / rate))
            frames += struct.pack("<hh", v, v)
        w.writeframes(frames)
make_tone("/tmp/bt_host_a2dp.wav", 440.0)
make_tone("/tmp/bt_host_hfp.wav", 880.0)
PYEOF

echo ""
echo "=== 4. Play 440Hz Test Tone from Host to Nokia (A2DP) ==="
pactl list short sinks
HOST_NOKIA_SINK="$(pactl list short sinks | awk 'tolower($2) ~ /02[:_]00[:_]34[:_]40[:_]12[:_]1e/ { print $2; exit }')"
if [ -n "${HOST_NOKIA_SINK:-}" ]; then
    echo "Found host BT sink: ${HOST_NOKIA_SINK}"
    paplay --device="${HOST_NOKIA_SINK}" /tmp/bt_host_a2dp.wav || true
else
    echo "No bluez_output sink for nokia-argon found on host yet."
fi

sleep 1
echo ""
echo "=== 5. Test HFP Handset Mode (headset-head-unit) ==="
HOST_CARD="$(pactl list short cards | awk 'tolower($2) ~ /02[:_]00[:_]34[:_]40[:_]12[:_]1e/ { print $2; exit }')"
if [ -n "${HOST_CARD:-}" ]; then
    pactl set-card-profile "${HOST_CARD}" headset-head-unit || true
    sleep 2
    echo "--- Host sinks/sources in HFP ---"
    pactl list short sinks | grep -iE "02[:_]00[:_]34[:_]40[:_]12[:_]1e" || true
    pactl list short sources | grep -iE "02[:_]00[:_]34[:_]40[:_]12[:_]1e" || true
    HFP_SINK="$(pactl list short sinks | awk 'tolower($2) ~ /02[:_]00[:_]34[:_]40[:_]12[:_]1e/ { print $2; exit }')"
    HFP_SRC="$(pactl list short sources | awk 'tolower($2) ~ /02[:_]00[:_]34[:_]40[:_]12[:_]1e/ && tolower($2) !~ /\.monitor$/ { print $2; exit }')"
    if [ -n "${HFP_SINK:-}" ]; then
        echo "Playing 880Hz HFP tone to ${HFP_SINK} (with Nokia SCO check)..."
        paplay --device="${HFP_SINK}" /tmp/bt_host_hfp.wav &
        PA_PID=$!
        sleep 1
        ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" '
            echo "--- Nokia hcitool con ---"
            hcitool con 2>/dev/null || true
            echo "--- Nokia dmesg (last 10) ---"
            printf "q\n" | sudo -S dmesg | tail -n 10 || true
        ' || true
        wait "${PA_PID}" || true
    fi
    if [ -n "${HFP_SRC:-}" ]; then
        echo "Recording 2s from HFP mic ${HFP_SRC}..."
        timeout 3 parecord --device="${HFP_SRC}" --file-format=wav /tmp/hfp_mic.wav 2>/dev/null || true
        ls -la /tmp/hfp_mic.wav 2>/dev/null || true
    fi
    pactl set-card-profile "${HOST_CARD}" a2dp-sink || true
fi

echo ""
echo "=== 4. Nokia State After Host Connect ==="
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" '
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
wpctl status
echo "--- Nokia bluez_card ---"
pactl list cards | grep -A 35 bluez_card || true
echo "--- Nokia wireplumber logs ---"
journalctl --user -u wireplumber -u pipewire -n 15 --no-pager || true
'
