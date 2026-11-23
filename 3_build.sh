#!/bin/sh
set +x
. "$(dirname "$0")"/activate.sh || exit 1
pmbootstrap --version
pmbootstrap install
