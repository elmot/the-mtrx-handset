#!/bin/sh
set +x
. "$(dirname "$0")"/activate.sh || exit 1
pmbootstrap --version
WORK_DIR=$(realpath -m "$XDG_CONFIG_HOME/../.local/var/pmbootstrap")
APORTS_DIR=$(realpath -m "$XDG_CONFIG_HOME/../pmaports")
echo Adding fake git remote \(existing upstream may be reported\)...
git -C $XDG_CONFIG_HOME/../pmaports remote add upstream https://gitlab.postmarketos.org/postmarketOS/pmaports.git
echo Writing config...
mkdir $XDG_CONFIG_HOME
cat > $XDG_CONFIG_HOME/pmbootstrap_v3.cfg <<EOF
[pmbootstrap]
work = $WORK_DIR
aports = $APORTS_DIR
device = nokia-argon
user = morpheus
ui = nokia-slint
channel = 26.06
locale = en_US
hostname = mtrx-handset
jobs = 10
timezone = Europe/Helsinki
EOF
pmbootstrap init