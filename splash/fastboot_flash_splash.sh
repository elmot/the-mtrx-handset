#!/usr/bin/env bash
set -euo pipefail
set +x

fastboot --version >/dev/null 2>&1 || { echo >&2 "Error: fastboot is required."; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_IMG="$SCRIPT_DIR/splash.img"

echo "Checking for fastboot device..."
if ! fastboot devices | grep -q "fastboot"; then
    echo "Error: no device detected in fastboot mode." >&2
    exit 1
fi

echo "Flashing $OUT_IMG to splash partition..."
fastboot flash splash "$OUT_IMG"

echo "Done."
