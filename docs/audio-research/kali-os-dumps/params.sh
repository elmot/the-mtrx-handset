#!/bin/sh

OUT="/data/local/tmp/audio-$(date +%H%M%S).txt"

{
    echo "===== DATE ====="
    date

    echo "===== TINYMIX ====="
    tinymix

    echo "===== PCM DEVICES ====="
    cat /proc/asound/pcm

    echo "===== ASoC PCM STATES ====="
    for f in /sys/kernel/debug/asoc/msm8909-skub-snd-card/*/state; do
        echo "----- $f"
        cat "$f" 2>/dev/null
    done

    echo "===== CODEC DAPM STATES ====="
    D="/sys/kernel/debug/asoc/msm8909-skub-snd-card/msm8x16_wcd_codec-13/dapm"
    for w in \
        "I2S RX1" "I2S RX2" "I2S RX3" \
        "RX1 MIX1" "RX1 MIX2" "RX2 MIX1" "RX2 MIX2" "RX3 MIX1" \
        "RX1 CHAIN" "RX2 CHAIN" "RX3 CHAIN" \
        "RX1 MIX1 INP1" "RX1 MIX2 INP1" \
        "RX2 MIX1 INP1" "RX2 MIX2 INP1" \
        "RX3 MIX1 INP1" \
        "RDAC2 MUX" \
        "HPHL DAC" "HPHR DAC" "HPHL PA" "HPHR PA" \
        "HEADPHONE" "EAR PA" "EAR_S" "EAR" \
        "SPK DAC" "SPK PA" "SPK_OUT"; do
        echo "----- $w"
        cat "$D/$w/state" 2>/dev/null
    done

    echo "===== ROUTING DAPM ====="
    R="/sys/kernel/debug/asoc/msm8909-skub-snd-card/qcom,msm-pcm-routing.44/dapm"
    for w in \
        "PRI_MI2S_RX Audio Mixer" \
        "PRI_MI2S_RX Voice Mixer" \
        "MultiMedia1 Mixer" \
        "PRI_MI2S_RX" "MI2S_RX"; do
        echo "----- $w"
        cat "$R/$w/state" 2>/dev/null
    done

} > "$OUT" 2>&1

echo "Saved to $OUT"
