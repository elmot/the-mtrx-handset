#!/bin/sh
cd "$(dirname "$0")" || exit 1
echo Configure Python ven
python -m venv .venv
cat > activate.sh <<EOF
export XDG_CONFIG_HOME="$(pwd)"/.config
export PMB_CHANNELS_CFG="$(pwd)"/channels.cfg
. $(pwd)/.venv/bin/activate >> activate.sh
EOF
. $(pwd)/activate.sh
echo Install pmbootstrap
cd pmbootstrap
pip install .
pmbootstrap --version
