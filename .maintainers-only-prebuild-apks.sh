#!/bin/sh
# Maintainers only: refresh prebuilt-apks from a local pmbootstrap work directory.
# Usage: ./.maintainers-only-prebuild-apks.sh [path/to/.local/var/pmbootstrap]
set -eu

SRC_DIR="${1:-.local/var/pmbootstrap}"
DST_DIR="$(cd "$(dirname "$0")" && pwd)/prebuilt-apks"
PKG_SRC="$SRC_DIR/packages/v26.06/armv7"
PKG_DST="$DST_DIR/packages/v26.06/armv7"
KEY_SRC="$SRC_DIR/config_apk_keys"
KEY_DST="$DST_DIR/config_apk_keys"

if [ ! -d "$SRC_DIR" ]; then
    echo "Source directory not found: $SRC_DIR" >&2
    exit 1
fi

mkdir -p "$PKG_DST" "$KEY_DST"

echo "Copying APKs and keys from $SRC_DIR"
cp -f "$PKG_SRC"/linux-postmarketos-qcom-msm8916-6.12.1-r8.apk "$PKG_SRC"/py3-slint-1.19.0-r1.apk "$PKG_DST"/
rm -f "$KEY_DST"/*.rsa.pub
cp -f "$KEY_SRC"/pmos@local-*.rsa.pub "$KEY_DST"/

echo "Done."
