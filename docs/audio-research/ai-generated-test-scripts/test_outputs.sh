#!/usr/bin/env bash
set -euo pipefail

TARGET_IP="${TARGET_IP:-172.16.42.1}"
TARGET_USER="${TARGET_USER:-elmot}"
MODE="${1:-all}" # earpiece | speaker | headphones | all

SSH_OPTS=(-o ConnectTimeout=5 -o ServerAliveInterval=3 -o ServerAliveCountMax=2 -o StrictHostKeyChecking=accept-new -o BatchMode=yes)

echo "=== Generating 48kHz 16-bit Stereo Test Tone WAVs on Host ==="
python3 - <<'PYEOF'
import math, struct, wave

def make_wav(path, freqs, duration=4.0, rate=48000, amp=0.5):
    n = int(rate * duration)
    with wave.open(path, "w") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(n):
            t = i / rate
            # Rhythmic double-beep: 0.8s on, 0.2s pause, 0.8s on, 0.4s pause, repeat
            cycle = t % 1.2
            env = 1.0 if (cycle < 0.45 or (0.55 <= cycle < 1.0)) else 0.0
            val_l = int(32767 * amp * env * math.sin(2 * math.pi * freqs[0] * t))
            val_r = int(32767 * amp * env * math.sin(2 * math.pi * freqs[1] * t))
            frames += struct.pack("<hh", val_l, val_r)
        w.writeframes(frames)

make_wav("/tmp/tone_earpiece.wav", (523.25, 523.25), duration=4.0, amp=0.6)   # C5 (523 Hz)
make_wav("/tmp/tone_speaker.wav", (659.25, 659.25), duration=4.0, amp=0.6)    # E5 (659 Hz)
make_wav("/tmp/tone_headphones.wav", (440.0, 880.0), duration=4.0, amp=0.5)   # L=440Hz, R=880Hz
PYEOF

echo "=== Copying Test WAVs to ${TARGET_USER}@${TARGET_IP}:/tmp/ ==="
scp "${SSH_OPTS[@]}" /tmp/tone_earpiece.wav /tmp/tone_speaker.wav /tmp/tone_headphones.wav "${TARGET_USER}@${TARGET_IP}:/tmp/"

echo "=== Running Output Tests on ${TARGET_USER}@${TARGET_IP} (mode: ${MODE}) ==="
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "MODE='${MODE}' sh -s" <<'EOF'
set -u

export XDG_RUNTIME_DIR="/run/user/$(id -u)"

echo "--- Cleaning up any stale audio processes ---"
killall -9 speaker-test aplay arecord paplay parecord 2>/dev/null || true

# Suspend PulseAudio sinks so direct ALSA/UCM testing has exclusive hardware access
# (only if a sound server is already running; pactl would otherwise autospawn one)
HAVE_PA=0
if pgrep -x pulseaudio >/dev/null 2>&1 || pgrep -x pipewire-pulse >/dev/null 2>&1; then HAVE_PA=1; fi
if [ "$HAVE_PA" = 1 ] && command -v pactl >/dev/null 2>&1; then
    timeout 2 pactl suspend-sink @DEFAULT_SINK@ 1 2>/dev/null || true
    for sink in $(timeout 2 pactl list short sinks 2>/dev/null | awk '{print $1}'); do
        timeout 2 pactl suspend-sink "$sink" 1 2>/dev/null || true
    done
fi

reset_outputs() {
    timeout 2 amixer -c 0 cset name='EAR_S' 0 >/dev/null 2>&1 || true
    timeout 2 amixer -c 0 cset name='SPK DAC Switch' 0 >/dev/null 2>&1 || true
    timeout 2 amixer -c 0 cset name='HPHL' 0 >/dev/null 2>&1 || true
    timeout 2 amixer -c 0 cset name='HPHR' 0 >/dev/null 2>&1 || true
    timeout 2 amixer -c 0 cset name='RX1 MIX1 INP1' 'ZERO' >/dev/null 2>&1 || true
    timeout 2 amixer -c 0 cset name='RX2 MIX1 INP1' 'ZERO' >/dev/null 2>&1 || true
    timeout 2 amixer -c 0 cset name='RX3 MIX1 INP1' 'ZERO' >/dev/null 2>&1 || true
}

play_and_verify() {
    local label="$1"
    local wav="$2"
    echo "  Playing ${wav} on ${label} (timeout: 6s)..."
    sudo dmesg -C 2>/dev/null || true
    timeout 6 aplay -D plughw:0,0 "$wav" &
    local apid=$!
    sleep 0.5
    echo "  PCM0p status during playback:"
    cat /proc/asound/card0/pcm0p/sub0/status 2>/dev/null | sed 's/^/    /' || true
    sleep 0.5
    cat /proc/asound/card0/pcm0p/sub0/status 2>/dev/null | grep -E "state|hw_ptr|appl_ptr" | sed 's/^/    /' || true
    if wait "$apid"; then
        echo "  [PASS] ${label} playback completed cleanly."
    else
        local rc=$?
        kill -9 "$apid" 2>/dev/null || true
        killall -9 aplay 2>/dev/null || true
        echo "  [FAIL] ${label} aplay exited/timed out (rc=${rc})."
        echo "  Kernel dmesg during playback:"
        sudo dmesg | sed 's/^/    /' || true
        return 1
    fi
}

test_earpiece() {
    echo ""
    echo "=========================================="
    echo " [1/3] Testing Internal Earpiece (EAR)    "
    echo "=========================================="
    reset_outputs
    amixer -c 0 cset name='PRI_MI2S_RX Audio Mixer MultiMedia1' 1 >/dev/null
    amixer -c 0 cset name='RX1 MIX1 INP1' 'RX1' >/dev/null
    amixer -c 0 cset name='RDAC2 MUX' 'RX1' >/dev/null
    amixer -c 0 cset name='RX1 Digital Volume' 84 >/dev/null
    amixer -c 0 cset name='EAR PA Gain' 'POS_1P5_DB' >/dev/null 2>&1 || true
    amixer -c 0 cset name='EAR_S' 1 >/dev/null
    play_and_verify "Earpiece (523 Hz double-beep)" /tmp/tone_earpiece.wav
    reset_outputs
}

test_speaker() {
    echo ""
    echo "=========================================="
    echo " [2/3] Testing Internal Loudspeaker (SPK) "
    echo "=========================================="
    reset_outputs
    amixer -c 0 cset name='PRI_MI2S_RX Audio Mixer MultiMedia1' 1 >/dev/null
    amixer -c 0 cset name='RX3 MIX1 INP1' 'RX1' >/dev/null
    amixer -c 0 cset name='RX3 Digital Volume' 84 >/dev/null
    amixer -c 0 cset name='Speaker Boost' 'ENABLE' >/dev/null 2>&1 || true
    amixer -c 0 cset name='SPK DAC Switch' 1 >/dev/null
    play_and_verify "Loudspeaker (659 Hz double-beep)" /tmp/tone_speaker.wav
    reset_outputs
}

test_headphones() {
    echo ""
    echo "=========================================="
    echo " [3/3] Testing External Headset (HPH L/R) "
    echo "=========================================="
    reset_outputs
    amixer -c 0 cset name='PRI_MI2S_RX Audio Mixer MultiMedia1' 1 >/dev/null
    amixer -c 0 cset name='RX1 MIX1 INP1' 'RX1' >/dev/null
    amixer -c 0 cset name='RX2 MIX1 INP1' 'RX2' >/dev/null
    amixer -c 0 cset name='RDAC2 MUX' 'RX2' >/dev/null
    amixer -c 0 cset name='RX1 Digital Volume' 84 >/dev/null
    amixer -c 0 cset name='RX2 Digital Volume' 84 >/dev/null
    amixer -c 0 cset name='HPHL' 1 >/dev/null
    amixer -c 0 cset name='HPHR' 1 >/dev/null
    play_and_verify "External Headset (L=440Hz, R=880Hz)" /tmp/tone_headphones.wav
    reset_outputs
}

case "$MODE" in
    earpiece)   test_earpiece ;;
    speaker)    test_speaker ;;
    headphones) test_headphones ;;
    all)
        test_earpiece
        test_speaker
        test_headphones
        ;;
    *)
        echo "Usage: $0 [earpiece|speaker|headphones|all]"
        exit 1
        ;;
esac

# Restore default Speaker route for PulseAudio
if command -v alsaucm >/dev/null 2>&1; then
    timeout 2 alsaucm -c msm8916 set _verb HiFi set _enadev Speaker 2>/dev/null || true
fi
if [ "$HAVE_PA" = 1 ] && command -v pactl >/dev/null 2>&1; then
    for sink in $(timeout 2 pactl list short sinks 2>/dev/null | awk '{print $1}'); do
        timeout 2 pactl suspend-sink "$sink" 0 2>/dev/null || true
    done
fi
EOF

