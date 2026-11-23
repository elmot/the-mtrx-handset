#!/usr/bin/env bash
set -euo pipefail

# Offline DTB Validator for Nokia 8110 4G (nokia-argon) Audio Bring-up
# Usage: ./validate_dtb.sh /path/to/qcom-msm8905-nokia-argon.dtb

DTB_PATH="${1:-}"
if [[ -z "$DTB_PATH" || ! -f "$DTB_PATH" ]]; then
    echo "Usage: $0 <path-to-compiled.dtb>" >&2
    exit 1
fi

TMP_DTS="$(mktemp /tmp/argon-decompiled.XXXXXX.dts)"
trap 'rm -f "$TMP_DTS"' EXIT

echo "[1/3] Decompiling $DTB_PATH with dtc..."
dtc -I dtb -O dts -o "$TMP_DTS" "$DTB_PATH"

echo "[2/3] Checking required audio nodes in decompiled DTB..."
REQUIRED_PATTERNS=(
    "qcom,msm8916-wcd-digital-codec"
    "qcom,pm8916-wcd-analog-codec"
    "sound"
    "vdd-cdc-io-supply"
    "vdd-cdc-tx-rx-cx-supply"
    "vdd-micbias-supply"
)

MISSING=0
for pat in "${REQUIRED_PATTERNS[@]}"; do
    if grep -q "$pat" "$TMP_DTS"; then
        echo "  [OK] Found '$pat'"
    else
        echo "  [MISSING] '$pat' not found in compiled DTB!" >&2
        MISSING=1
    fi
done

echo "[3/3] Checking for disabled status on audio nodes..."
for node in "audio-codec@771c000" "audio-codec@f000" "sound@7702000"; do
    if grep -A 25 "${node} {" "$TMP_DTS" | grep -q 'status = "disabled"'; then
        echo "  [FAIL] Node '$node' is still marked status = \"disabled\"!" >&2
        MISSING=1
    fi
done

echo "[4/4] Checking for unresolved phandles (0xffffffff)..."
if grep -v "cooling-device" "$TMP_DTS" | grep -q "0xffffffff"; then
    echo "  [FAIL] Found unresolved phandle (0xffffffff) in compiled DTB!" >&2
    MISSING=1
else
    echo "  [OK] Zero unresolved phandles"
fi

if [[ "$MISSING" -ne 0 ]]; then
    echo "DTB validation FAILED." >&2
    exit 2
fi

echo "DTB validation PASSED."

