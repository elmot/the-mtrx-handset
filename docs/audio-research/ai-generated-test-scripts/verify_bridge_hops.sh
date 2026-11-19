#!/usr/bin/env bash
# Verify each individual audio hop for HFP bridge on nokia-argon:
# 1) MultiMedia5 (hw:1,5) capture from INT_BT_SCO_TX at 8000 Hz 1ch
# 2) MultiMedia1 (hw:1,0) 8000 Hz 1ch playback on SPK_OUT -> MultiMedia2 (hw:1,1) 8000 Hz 1ch capture (AMIC1 vs AMIC3)
set -euo pipefail

PHONE_MAC="02:00:34:40:12:1E"
PHONE_CARD="bluez_card.02_00_34_40_12_1E"
TARGET_IP="${TARGET_IP:-172.16.42.1}"
TARGET_USER="${TARGET_USER:-elmot}"
TARGET_PASS="${TARGET_PASS:-q}"
SSH_OPTS=(-o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new)

python3 - <<'PYEOF'
import math, struct, wave
def tone(path, hz, dur=3):
    rate = 8000
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(b"".join(struct.pack('<h', int(12000 * math.sin(2*math.pi*hz*i/rate))) for i in range(rate*dur)))
tone('/tmp/hop_440.wav', 440, 4)
tone('/tmp/hop_660.wav', 660, 2)
PYEOF

scp "${SSH_OPTS[@]}" /tmp/hop_660.wav "${TARGET_USER}@${TARGET_IP}:/tmp/hop_660.wav"

echo "=== [1/2] Testing 8kHz SPK_OUT -> AMIC1 and AMIC3 locally on phone ==="
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" '
set -eu
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
systemctl --user stop nokia-bt-hfp.service
pactl set-card-profile alsa_card.platform-7702000.sound off || true

for mic in amic1 amic3; do
    amixer -c msm8916 cset name="PRI_MI2S_RX Audio Mixer MultiMedia1" 1 >/dev/null
    amixer -c msm8916 cset name="MultiMedia2 Mixer TERT_MI2S_TX" 1 >/dev/null
    amixer -c msm8916 cset name="EAR_S" ZERO >/dev/null
    amixer -c msm8916 cset name="RX1 MIX1 INP1" ZERO >/dev/null
    amixer -c msm8916 cset name="RX3 MIX1 INP1" RX1 >/dev/null
    amixer -c msm8916 cset name="RX3 Mute Switch" 0 >/dev/null
    amixer -c msm8916 cset name="RX3 Digital Volume" 84 >/dev/null
    amixer -c msm8916 cset name="SPK DAC Switch" 1 >/dev/null
    amixer -c msm8916 cset name="CIC1 MUX" AMIC >/dev/null
    amixer -c msm8916 cset name="TX1 HPF Switch" 1 >/dev/null
    amixer -c msm8916 cset name="TX1 Digital Volume" 84 >/dev/null
    if [ "$mic" = "amic3" ]; then
        amixer -c msm8916 cset name="ADC2 MUX" INP3 >/dev/null
        amixer -c msm8916 cset name="DEC1 MUX" ADC2 >/dev/null
        amixer -c msm8916 cset name="ADC2 Volume" 6 >/dev/null
    else
        amixer -c msm8916 cset name="ADC2 MUX" ZERO >/dev/null
        amixer -c msm8916 cset name="DEC1 MUX" ADC1 >/dev/null
        amixer -c msm8916 cset name="ADC1 Volume" 6 >/dev/null
    fi
    arecord -D hw:msm8916,1 -f S16_LE -r 8000 -c 1 -d 2 "/tmp/hop_spk_${mic}.wav" &
    PID=$!
    aplay -D hw:msm8916,0 -f S16_LE -r 8000 -c 1 /tmp/hop_660.wav
    wait "$PID"
done

# Also verify MultiMedia5 (hw:msm8916,5) capture session works with TERT_MI2S_TX
amixer -c msm8916 cset name="MultiMedia2 Mixer TERT_MI2S_TX" 0 >/dev/null
amixer -c msm8916 cset name="MultiMedia5 Mixer TERT_MI2S_TX" 1 >/dev/null
arecord -D hw:msm8916,5 -f S16_LE -r 8000 -c 1 -d 2 /tmp/hop_mm5_amic3.wav &
PID=$!
aplay -D hw:msm8916,0 -f S16_LE -r 8000 -c 1 /tmp/hop_660.wav
wait "$PID"
amixer -c msm8916 cset name="MultiMedia5 Mixer TERT_MI2S_TX" 0 >/dev/null
amixer -c msm8916 cset name="MultiMedia2 Mixer TERT_MI2S_TX" 1 >/dev/null
amixer -c msm8916 cset name="SPK DAC Switch" 0 >/dev/null
'
scp "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}:/tmp/hop_spk_amic1.wav" /tmp/hop_spk_amic1.wav
scp "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}:/tmp/hop_spk_amic3.wav" /tmp/hop_spk_amic3.wav
scp "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}:/tmp/hop_mm5_amic3.wav" /tmp/hop_mm5_amic3.wav

echo "=== [2/2] Restoring nokia-bt-hfp and analyzing recordings ==="
ssh "${SSH_OPTS[@]}" "${TARGET_USER}@${TARGET_IP}" '
set -eu
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
pactl set-card-profile alsa_card.platform-7702000.sound "HiFi (Mic1, Speaker)" || true
systemctl --user start nokia-bt-hfp.service
'

python3 - <<'PYEOF'
import math, struct, wave
for label, p in [("8kHz SPK->MM2 AMIC1", "/tmp/hop_spk_amic1.wav"),
                 ("8kHz SPK->MM2 AMIC3", "/tmp/hop_spk_amic3.wav"),
                 ("8kHz SPK->MM5 AMIC3", "/tmp/hop_mm5_amic3.wav")]:
    with wave.open(p, 'rb') as w:
        raw = w.readframes(w.getnframes()); rate = w.getframerate()
    s = struct.unpack(f'<{len(raw)//2}h', raw)
    peak = max((abs(x) for x in s), default=0)
    rms = math.sqrt(sum(x*x for x in s) / max(1, len(s)))
    k = int(0.5 + len(s) * 660.0 / rate)
    w_ang = 2.0 * math.pi * k / len(s)
    coeff = 2.0 * math.cos(w_ang)
    q0 = q1 = q2 = 0.0
    for x in s:
        q0 = coeff * q1 - q2 + x; q2 = q1; q1 = q0
    mag = math.sqrt(q1*q1 + q2*q2 - q1*q2*coeff) / len(s)
    print(f"{label}: peak={peak}, rms={rms:.1f}, mag@660Hz={mag:.1f}")
PYEOF
