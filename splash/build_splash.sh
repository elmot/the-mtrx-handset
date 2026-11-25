#!/usr/bin/env bash
set -euo pipefail
set +x

ffmpeg -version >/dev/null 2>&1 || { echo >&2 "Error: ffmpeg is required."; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INPUT_IMG="${1:-$SCRIPT_DIR/splash.png}"
HEADER_FILE="$SCRIPT_DIR/splash_header"
RAW_FILE="$SCRIPT_DIR/splash.raw"
OUT_IMG="$SCRIPT_DIR/splash.img"

WIDTH=240
HEIGHT=320
EXPECTED_RAW_SIZE=$((WIDTH * HEIGHT * 2))

if [ ! -f "$INPUT_IMG" ]; then
    echo "Error: input image not found: $INPUT_IMG" >&2
    exit 1
fi

if [ ! -f "$HEADER_FILE" ]; then
    echo "Error: splash header not found: $HEADER_FILE" >&2
    echo "Extract it first with: dd if=splash_backup.img of=splash_header bs=512 count=1" >&2
    exit 1
fi

HEADER_SIZE="$(wc -c < "$HEADER_FILE")"
if [ "$HEADER_SIZE" -ne 512 ]; then
    echo "Error: $HEADER_FILE must be 512 bytes (got $HEADER_SIZE)" >&2
    exit 1
fi

echo "Converting and resizing '$INPUT_IMG' to ${WIDTH}x${HEIGHT} rgb565le..."
ffmpeg -y -hide_banner -loglevel error \
    -i "$INPUT_IMG" \
    -vf "scale=${WIDTH}:${HEIGHT}:force_original_aspect_ratio=decrease,pad=${WIDTH}:${HEIGHT}:(ow-iw)/2:(oh-ih)/2:black" \
    -f rawvideo -vcodec rawvideo -pix_fmt rgb565le \
    "$RAW_FILE"

RAW_SIZE="$(wc -c < "$RAW_FILE")"
if [ "$RAW_SIZE" -ne "$EXPECTED_RAW_SIZE" ]; then
    echo "Error: expected $EXPECTED_RAW_SIZE bytes in $RAW_FILE, got $RAW_SIZE" >&2
    exit 1
fi

echo "Building $OUT_IMG..."
cat "$HEADER_FILE" "$RAW_FILE" > "$OUT_IMG"

echo "Done."
