#!/usr/bin/env bash
# Diagnose live HFP eSCO connection + Q6AFE INT_BT_SCO audio between Elias-DELL and nokia-argon.
set -euo pipefail

PHONE_MAC="02:00:34:40:12:1E"
PHONE_CARD="bluez_card.02_00_34_40_12_1E"
TARGET_IP="${TARGET_IP:-172.16.42.1}"
TARGET_USER="${TARGET_USER:-elmot}"
TARGET_PASS="${TARGET_PASS:-q}"
SSH_OPTS=(-o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new)

echo "=== [1/5] Checking nokia-argon BT status and preparing 440 Hz host test tone ==="
python3 - <<'PYEOF'
import math, struct, wave
def make_tone(path, freq, dur=4, rate=8000):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(rate * dur):
            val = int(12000 * math.sin(2.0 * math.pi * freq * i / rate))
            frames += struct.pack('<h', val)
        w.writeframes(frames)

make_tone('/tmp/host_440hz_8k.wav', 440.0, dur=4, rate=8000)
make_tone('/tmp/phone_1khz_8k.wav', 1000.0, dur=5, rate=8000)
PYEOF

scp "${SSH_OPTS[@]}" /tmp/phone_1khz_8k.wav "${TARGET_USER}@${TARGET_IP}:/tmp/phone_1khz_8k.wav"

echo "=== [2/5] Connecting BlueZ from Elias-DELL to nokia-argon ($PHONE_MAC) ==="
bluetoothctl info "$PHONE_MAC" | grep -E "Name|Connected|Paired|Trusted|UUID" || true
if ! bluetoothctl info "$PHONE_MAC" | grep -q "Connected: yes"; then
    bluetoothctl connect "$PHONE_MAC" || true
    sleep 2
fi

echo "=== [3/5] Switching host BlueZ card to HFP (headset-head-unit) ==="
pactl list cards short
CARD_PROFILES="$(pactl list cards | awk -v card="$PHONE_CARD" '$0 ~ card {f=1} f && /Profiles:/ {p=1; next} f && p && /Active Profile:/ {exit} f && p {print}')"
echo "$CARD_PROFILES"

if echo "$CARD_PROFILES" | grep -q "headset-head-unit-cvsd"; then
    HFP_PROFILE="headset-head-unit-cvsd"
else
    HFP_PROFILE="headset-head-unit"
fi
echo "Selected host HFP profile: $HFP_PROFILE"
pactl set-card-profile "$PHONE_CARD" "$HFP_PROFILE"

SINK_NAME=""
SRC_NAME=""
for _ in $(seq 1 12); do
    SINK_NAME="$(pactl list sinks short | awk '/02[:_]00[:_]34[:_]40[:_]12[:_]1E/ {print $2; exit}')"
    SRC_NAME="$(pactl list sources short | awk '/02[:_]00[:_]34[:_]40[:_]12[:_]1E/ && $2 !~ /\.monitor$/ {print $2; exit}')"
    if [ -n "$SINK_NAME" ] && [ -n "$SRC_NAME" ]; then
        break
    fi
    sleep 0.5
done
pactl list sinks short
pactl list sources short
pactl list cards | awk -v card="$PHONE_CARD" '$0 ~ card {f=1} f && /Active Profile:/ {print; exit}'
echo "Host HFP sink=$SINK_NAME src=$SRC_NAME"

echo "=== [4/5] Starting phone INT_BT_SCO RX/TX and host HFP stream ==="
DMESG_BEFORE="$(ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "printf '%s\n' '${TARGET_PASS}' | sudo -S dmesg | wc -l")"

rm -f /tmp/host_hfp_cap.wav /tmp/phone_hfp_cap.wav

# Start phone INT_BT_SCO_RX (MultiMedia3) + INT_BT_SCO_TX capture (MultiMedia2)
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "TARGET_PASS='${TARGET_PASS}' sh -s" <<'EOF' &
set -u
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
amixer -c msm8916 cset name='INT_BT_SCO_RX Audio Mixer MultiMedia3' 1 >/dev/null
amixer -c msm8916 cset name='MultiMedia2 Mixer TERT_MI2S_TX' 0 >/dev/null
amixer -c msm8916 cset name='MultiMedia2 Mixer INT_BT_SCO_TX' 1 >/dev/null
arecord -D hw:msm8916,1 -f S16_LE -r 8000 -c 1 -d 5 /tmp/phone_hfp_cap.wav &
REC_PID=$!
(
    sleep 1.5
    echo "=== Phone hci0 conn_info mid-stream ==="
    printf '%s\n' "$TARGET_PASS" | sudo -S cat /sys/kernel/debug/bluetooth/hci0/conn_info 2>/dev/null || true
) &
aplay -D hw:msm8916,2 -f S16_LE -r 8000 -c 1 /tmp/phone_1khz_8k.wav || echo "phone aplay failed"
wait "$REC_PID" || echo "phone arecord failed"
wait || true
amixer -c msm8916 cset name='INT_BT_SCO_RX Audio Mixer MultiMedia3' 0 >/dev/null
amixer -c msm8916 cset name='MultiMedia2 Mixer INT_BT_SCO_TX' 0 >/dev/null
amixer -c msm8916 cset name='MultiMedia2 Mixer TERT_MI2S_TX' 1 >/dev/null
EOF
PHONE_PID=$!

sleep 0.5
if [ -n "${SINK_NAME:-}" ] && [ -n "${SRC_NAME:-}" ]; then
    timeout 4 pw-play --target="$SINK_NAME" /tmp/host_440hz_8k.wav &
    HOST_PLAY_PID=$!
    timeout 4 pw-record --target="$SRC_NAME" --rate=8000 --channels=1 --format=s16 /tmp/host_hfp_cap.wav || true
    wait "$HOST_PLAY_PID" || true
else
    echo "[WARN] Missing HFP sink/source on host!"
    sleep 5
fi

wait "$PHONE_PID" || true
scp "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}:/tmp/phone_hfp_cap.wav" /tmp/phone_hfp_cap.wav 2>/dev/null || true

echo "=== [5/5] Inspecting captured audio and phone dmesg ==="
python3 - <<'PYEOF'
import struct, wave, os
for label, p in [("Host captured HFP (from phone INT_BT_SCO_RX)", "/tmp/host_hfp_cap.wav"),
                 ("Phone captured HFP (from phone INT_BT_SCO_TX)", "/tmp/phone_hfp_cap.wav")]:
    if not os.path.exists(p):
        print(f"No {p} created")
        continue
    with wave.open(p, 'rb') as w:
        raw = w.readframes(w.getnframes())
        rate = w.getframerate()
    n = len(raw) // 2
    samples = struct.unpack(f'<{n}h', raw) if n else ()
    peak = max((abs(s) for s in samples), default=0)
    nonzero = sum(1 for s in samples if s != 0)
    print(f"{label}: samples={n}, rate={rate}, nonzero={nonzero}, peak={peak}")
PYEOF

echo "--- Phone new dmesg ---"
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "printf '%s\n' '${TARGET_PASS}' | sudo -S dmesg | tail -n +$((DMESG_BEFORE + 1))" || true

echo "--- Phone PipeWire/WirePlumber journal (last 20 lines) ---"
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "journalctl --user -u pipewire -u wireplumber -n 20 --no-pager" || true

# Restore A2DP profile on host cleanly
pactl set-card-profile "$PHONE_CARD" a2dp-sink-sbc 2>/dev/null || pactl set-card-profile "$PHONE_CARD" a2dp-sink 2>/dev/null || true
