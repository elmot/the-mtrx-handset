#!/usr/bin/env bash
set -euo pipefail

TARGET_IP="${TARGET_IP:-172.16.42.1}"
TARGET_USER="${TARGET_USER:-elmot}"
MODE="${1:-all}" # primary | secondary | headset | all
DURATION="${DURATION:-3}"

SSH_OPTS=(-o ConnectTimeout=5 -o ServerAliveInterval=3 -o ServerAliveCountMax=2 -o StrictHostKeyChecking=accept-new -o BatchMode=yes)

echo "=== Running Microphone Capture Tests on ${TARGET_USER}@${TARGET_IP} (mode: ${MODE}, duration: ${DURATION}s) ==="
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" "MODE='${MODE}' DURATION='${DURATION}' sh -s" <<'EOF'
set -u

export XDG_RUNTIME_DIR="/run/user/$(id -u)"

echo "--- Cleaning up any stale audio processes ---"
killall -9 speaker-test aplay arecord paplay parecord 2>/dev/null || true

# Suspend PulseAudio sources/sinks so direct ALSA capture has exclusive hardware access
HAVE_PA=0
if pgrep -x pulseaudio >/dev/null 2>&1 || pgrep -x pipewire-pulse >/dev/null 2>&1; then HAVE_PA=1; fi
if [ "$HAVE_PA" = 1 ] && command -v pactl >/dev/null 2>&1; then
    for src in $(timeout 2 pactl list short sources 2>/dev/null | awk '{print $1}'); do
        timeout 2 pactl suspend-source "$src" 1 2>/dev/null || true
    done
    for sink in $(timeout 2 pactl list short sinks 2>/dev/null | awk '{print $1}'); do
        timeout 2 pactl suspend-sink "$sink" 1 2>/dev/null || true
    done
fi

reset_mics() {
    timeout 2 amixer -c 0 cset name='MultiMedia2 Mixer TERT_MI2S_TX' 0 >/dev/null 2>&1 || true
    timeout 2 amixer -c 0 cset name='DEC1 MUX' 'ZERO' >/dev/null 2>&1 || true
    timeout 2 amixer -c 0 cset name='DEC2 MUX' 'ZERO' >/dev/null 2>&1 || true
    timeout 2 amixer -c 0 cset name='ADC1 Volume' 6 >/dev/null 2>&1 || true
    timeout 2 amixer -c 0 cset name='ADC2 Volume' 6 >/dev/null 2>&1 || true
    timeout 2 amixer -c 0 cset name='ADC3 Volume' 6 >/dev/null 2>&1 || true
}

record_and_verify() {
    local label="$1"
    local wav="$2"
    local max_wait=$((DURATION + 2))
    echo "  Recording ${DURATION}s from ${label} -> ${wav} (timeout: ${max_wait}s)..."
    sudo dmesg -C 2>/dev/null || true
    timeout "${max_wait}" arecord -D plughw:0,1 -r 48000 -c 2 -f S16_LE -d "${DURATION}" "$wav" &
    local rpid=$!
    sleep 0.5
    echo "  PCM1c status during capture:"
    cat /proc/asound/card0/pcm1c/sub0/status 2>/dev/null | grep -E "state|hw_ptr|appl_ptr" | sed 's/^/    /' || true
    if wait "$rpid"; then
        echo "  [PASS] ${label} capture completed cleanly ($(wc -c < "$wav") bytes)."
    else
        local rc=$?
        echo "  PCM holder(s):"; sudo fuser -v /dev/snd/pcmC0D1c 2>&1 | sed 's/^/    /' || true
        kill -9 "$rpid" 2>/dev/null || true
        killall -9 arecord 2>/dev/null || true
        echo "  [FAIL] ${label} arecord exited/timed out (rc=${rc})."
        echo "  Kernel dmesg during capture:"
        sudo dmesg | sed 's/^/    /' || true
        return 1
    fi
}

test_primary_mic() {
    echo ""
    echo "========================================================="
    echo " [1/3] Testing Internal Primary Handset Mic (AMIC1/Mic1) "
    echo "========================================================="
    reset_mics
    amixer -c 0 cset name='MultiMedia2 Mixer TERT_MI2S_TX' 1 >/dev/null
    amixer -c 0 cset name='CIC1 MUX' 'AMIC' >/dev/null
    amixer -c 0 cset name='DEC1 MUX' 'ADC1' >/dev/null
    amixer -c 0 cset name='ADC1 Volume' 6 >/dev/null
    amixer -c 0 cset name='TX1 Digital Volume' 84 >/dev/null 2>&1 || true
    amixer -c 0 cset name='TX1 HPF Switch' 1 >/dev/null 2>&1 || true
    record_and_verify "Primary Internal Mic (AMIC1)" /tmp/mic_primary.wav
    reset_mics
}

test_secondary_mic() {
    echo ""
    echo "========================================================="
    echo " [2/3] Testing Internal Secondary Mic (AMIC3/Mic2)       "
    echo "========================================================="
    reset_mics
    amixer -c 0 cset name='MultiMedia2 Mixer TERT_MI2S_TX' 1 >/dev/null
    amixer -c 0 cset name='CIC1 MUX' 'AMIC' >/dev/null
    amixer -c 0 cset name='ADC2 MUX' 'INP3' >/dev/null
    amixer -c 0 cset name='DEC1 MUX' 'ADC2' >/dev/null
    amixer -c 0 cset name='ADC2 Volume' 6 >/dev/null
    amixer -c 0 cset name='TX1 Digital Volume' 84 >/dev/null 2>&1 || true
    amixer -c 0 cset name='TX1 HPF Switch' 1 >/dev/null 2>&1 || true
    record_and_verify "Secondary Internal Mic (AMIC3)" /tmp/mic_secondary.wav
    reset_mics
}

test_headset_mic() {
    echo ""
    echo "========================================================="
    echo " [3/3] Testing External Headset Mic (AMIC2/Headset)      "
    echo "========================================================="
    reset_mics
    amixer -c 0 cset name='MultiMedia2 Mixer TERT_MI2S_TX' 1 >/dev/null
    amixer -c 0 cset name='CIC1 MUX' 'AMIC' >/dev/null
    amixer -c 0 cset name='ADC2 MUX' 'INP2' >/dev/null
    amixer -c 0 cset name='DEC1 MUX' 'ADC2' >/dev/null
    amixer -c 0 cset name='ADC2 Volume' 6 >/dev/null
    amixer -c 0 cset name='TX1 Digital Volume' 84 >/dev/null 2>&1 || true
    amixer -c 0 cset name='TX1 HPF Switch' 1 >/dev/null 2>&1 || true
    record_and_verify "External Headset Mic (AMIC2)" /tmp/mic_headset.wav
    reset_mics
}

case "$MODE" in
    primary)   test_primary_mic ;;
    secondary) test_secondary_mic ;;
    headset)   test_headset_mic ;;
    all)
        test_primary_mic
        test_secondary_mic
        test_headset_mic
        ;;
    *)
        echo "Usage: $0 [primary|secondary|headset|all]"
        exit 1
        ;;
esac

if [ "$HAVE_PA" = 1 ] && command -v pactl >/dev/null 2>&1; then
    for src in $(timeout 2 pactl list short sources 2>/dev/null | awk '{print $1}'); do
        timeout 2 pactl suspend-source "$src" 0 2>/dev/null || true
    done
    for sink in $(timeout 2 pactl list short sinks 2>/dev/null | awk '{print $1}'); do
        timeout 2 pactl suspend-sink "$sink" 0 2>/dev/null || true
    done
fi
EOF

echo ""
echo "=== Fetching Recorded WAVs & Analyzing Signal Levels (Peak & RMS) ==="
for m in primary secondary headset; do
    if [[ "$MODE" == "all" || "$MODE" == "$m" ]]; then
        scp "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}:/tmp/mic_${m}.wav" "/tmp/mic_${m}.wav" 2>/dev/null || true
    fi
done

python3 - "$MODE" <<'PYEOF'
import math, os, struct, sys, wave

mode = sys.argv[1]
targets = ["primary", "secondary", "headset"] if mode == "all" else [mode]

for name in targets:
    path = f"/tmp/mic_{name}.wav"
    if not os.path.exists(path):
        print(f"  [MISSING] {path}")
        continue
    with wave.open(path, "rb") as w:
        nch = w.getnchannels()
        rate = w.getframerate()
        nframes = w.getnframes()
        raw = w.readframes(nframes)
    samples = struct.unpack(f"<{nframes * nch}h", raw)
    # Skip first 100ms (pop/settling) if long enough
    skip = min(len(samples) // 4, int(0.1 * rate * nch))
    trimmed = samples[skip:] if skip < len(samples) else samples
    peak = max((abs(s) for s in trimmed), default=0)
    rms = math.sqrt(sum(s * s for s in trimmed) / len(trimmed)) if trimmed else 0.0
    db_peak = 20 * math.log10(peak / 32768.0) if peak > 0 else -999.0
    db_rms = 20 * math.log10(rms / 32768.0) if rms > 0 else -999.0
    status = "NON-ZERO SIGNAL OK" if peak > 0 else "SILENT (0)"
    print(f"  [{status}] mic_{name}.wav: {nframes/rate:.2f}s @ {rate}Hz ({nch}ch) | Peak={peak}/32767 ({db_peak:.1f} dBFS) | RMS={rms:.1f} ({db_rms:.1f} dBFS)")
PYEOF

