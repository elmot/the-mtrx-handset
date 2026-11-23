#!/bin/sh
cd "$(dirname "$0")" || exit 1
echo Configure Python venv...
python -m venv .venv
cat > activate.sh <<EOF
export XDG_CONFIG_HOME="$(pwd)"/.config
export PMB_CHANNELS_CFG="$(pwd)"/channels.cfg
. $(pwd)/.venv/bin/activate
EOF
. $(pwd)/activate.sh
echo Install pmbootstrap...
cd pmbootstrap
pip install .
pmbootstrap --version

read -rp "You need python-slint gateway for UI development. Do you want python-slint to be installed? [y/N] " answer

if [ "$answer" = "Y" ] || [ "$answer" = "y" ]; then
  echo Install slint...
  pip install slint
else
    echo "python-slint gateway is not installed. Nevertheless you can install it at any moment with 'pip install slint'"
fi
