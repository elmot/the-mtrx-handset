#!/usr/bin/env bash
# End-to-end test of nokia-bt-hfp (HFP-HF + Q6AFE INT_BT_SCO full-duplex bridge)
# between Elias-DELL (acting as HFP-AG) and nokia-argon.
set -euo pipefail

PHONE_MAC="02:00:34:40:12:1E"
PHONE_CARD="bluez_card.02_00_34_40_12_1E"
TARGET_IP="${TARGET_IP:-172.16.42.1}"
TARGET_USER="${TARGET_USER:-elmot}"
TARGET_PASS="${TARGET_PASS:-q}"
SSH_OPTS=(-o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new)

python3 - <<'PYEOF'
import math, struct, wave
rate = 8000
dur = 8
with wave.open('/tmp/host_hfp_440hz.wav', 'wb') as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(rate)
    frames = bytearray()
    for i in range(rate * dur):
        val = int(12000 * math.sin(2.0 * math.pi * 660.0 * i / rate))
        frames += struct.pack('<h', val)
    w.writeframes(frames)
PYEOF

echo "=== [1/5] Checking nokia-argon MultiMedia5 + nokia-bt-hfp service ==="
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" '
set -eu
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
arecord -l | grep -E "card |MultiMedia"
systemctl --user is-active nokia-bt-hfp.service
/usr/local/bin/nokia-bt-hfp --ctl status
'
DMESG_BEFORE="$(ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "printf '%s\n' '${TARGET_PASS}' | sudo -S dmesg | wc -l")"

echo "=== [2/5] Connecting BlueZ from Elias-DELL to nokia-argon ==="
bluetoothctl connect "$PHONE_MAC" || true
sleep 2
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" 'XDG_RUNTIME_DIR="/run/user/$(id -u)" /usr/local/bin/nokia-bt-hfp --ctl status'

echo "=== [3/5] Switching host BlueZ card to HFP (headset-head-unit) ==="
pactl set-card-profile "$PHONE_CARD" headset-head-unit
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
echo "Host HFP sink=$SINK_NAME src=$SRC_NAME"

echo "=== [4/5] Streaming HFP call audio and testing acoustic SPK->MIC path (noaec vs aec) ==="
rm -f /tmp/host_from_phone_mic_noaec.wav /tmp/host_from_phone_mic_aec.wav
pw-play --target="$SINK_NAME" /tmp/host_hfp_440hz.wav &
HOST_PLAY_PID=$!

# Window A (2s): speaker ON, AEC/NS OFF -> proves downlink SPK_OUT + uplink AMIC1 pass 440 Hz
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" '
set -eu
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
/usr/local/bin/nokia-bt-hfp --ctl speaker
/usr/local/bin/nokia-bt-hfp --ctl vgs 15
/usr/local/bin/nokia-bt-hfp --ctl dsp noaec_nons
/usr/local/bin/nokia-bt-hfp --ctl status
'
sleep 0.3
timeout 2 pw-record --target="$SRC_NAME" --rate=8000 --channels=1 --format=s16 /tmp/host_from_phone_mic_noaec.wav || true

# Window B (2s): speaker ON, AEC+NS ON -> verifies SpeexDSP AEC+NS suppresses 440 Hz echo
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" '
set -eu
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
/usr/local/bin/nokia-bt-hfp --ctl dsp on
/usr/local/bin/nokia-bt-hfp --ctl mute >/dev/null
/usr/local/bin/nokia-bt-hfp --ctl unmute >/dev/null
'
sleep 0.3
timeout 2 pw-record --target="$SRC_NAME" --rate=8000 --channels=1 --format=s16 /tmp/host_from_phone_mic_aec.wav || true

ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" 'XDG_RUNTIME_DIR="/run/user/$(id -u)" /usr/local/bin/nokia-bt-hfp --ctl earpiece'
wait "$HOST_PLAY_PID" || true

echo "=== [5/5] Restoring host profile and checking acoustic 440 Hz + AEC + dmesg ==="
pactl set-card-profile "$PHONE_CARD" a2dp-sink 2>/dev/null || pactl set-card-profile "$PHONE_CARD" a2dp-sink-sbc 2>/dev/null || true
sleep 1

python3 - <<'PYEOF'
import math, struct, wave
def inspect(label, path):
    with wave.open(path, 'rb') as w:
        raw = w.readframes(w.getnframes())
        rate = w.getframerate()
    samples = struct.unpack(f'<{len(raw)//2}h', raw)
    peak = max((abs(s) for s in samples), default=0)
    rms = math.sqrt(sum(s*s for s in samples) / max(1, len(samples)))
    k = int(0.5 + len(samples) * 660.0 / rate)
    w_ang = 2.0 * math.pi * k / max(1, len(samples))
    coeff = 2.0 * math.cos(w_ang)
    q0 = q1 = q2 = 0.0
    for x in samples:
        q0 = coeff * q1 - q2 + x
        q2 = q1
        q1 = q0
    mag = math.sqrt(q1*q1 + q2*q2 - q1*q2*coeff) / max(1, len(samples))
    print(f"{label}: samples={len(samples)}, peak={peak}, rms={rms:.1f}, mag@660Hz={mag:.1f}")

inspect("Speaker->Mic (noaec, raw)", "/tmp/host_from_phone_mic_noaec.wav")
inspect("Speaker->Mic (aec+ns)", "/tmp/host_from_phone_mic_aec.wav")
PYEOF

ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "TARGET_PASS='${TARGET_PASS}' DMESG_BEFORE='${DMESG_BEFORE}' sh -s" <<'EOF'
set -eu
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
echo "--- post-call nokia-bt-hfp status ---"
/usr/local/bin/nokia-bt-hfp --ctl status
echo "--- post-call PipeWire Built-in Audio profile ---"
pactl list cards | awk '/Name: alsa_card\.platform-7702000\.sound/{f=1} f && /Active Profile:/{print; exit}'
echo "--- nokia-bt-hfp service logs (last 40) ---"
journalctl --user -u nokia-bt-hfp.service -n 40 --no-pager || true
echo "--- phone dmesg btqcom/Bluetooth errors (should be empty!) ---"
printf '%s\n' "$TARGET_PASS" | sudo -S dmesg | tail -n +"$((DMESG_BEFORE + 1))" | grep -iE "bluetooth|btqcom|sending frame|q6afe|q6asm|q6adm" || echo "(none)"
EOF
