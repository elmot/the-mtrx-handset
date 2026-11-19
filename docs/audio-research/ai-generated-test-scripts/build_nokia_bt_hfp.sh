#!/usr/bin/env bash
# Build nokia-bt-hfp and device-nokia-argon apk packages via pmbootstrap.
set -euo pipefail

ROOT_DIR="/home/elmot/hacking-nokia"
PMB="${ROOT_DIR}/pmbootstrap/pmbootstrap.py"

"$PMB" checksum nokia-bt-hfp device-nokia-argon
"$PMB" build nokia-bt-hfp device-nokia-argon --arch=armv7 --force --lax
