#!/bin/sh
set +x
. "$(dirname "$0")"/activate.sh || exit 1
echo pmbootstrap version:
pmbootstrap --version
pmbootstrap build firmware-nokia-argon --arch=armv7
pmbootstrap install
