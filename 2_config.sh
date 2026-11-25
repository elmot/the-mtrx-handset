#!/bin/sh
set +x
. "$(dirname "$0")"/activate.sh || exit 1
pmbootstrap --version
WORK_DIR=$(realpath -m "$XDG_CONFIG_HOME/../.local/var/pmbootstrap")
APORTS_DIR=$(realpath -m "$XDG_CONFIG_HOME/../pmaports")
echo
echo Adding fake git remote \(existing upstream may be reported\)...
git -C $XDG_CONFIG_HOME/../pmaports remote add upstream https://gitlab.postmarketos.org/postmarketOS/pmaports.git
echo Writing config...
mkdir -p $XDG_CONFIG_HOME
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

[providers]
postmarketos-base-ui-audio-backend = postmarketos-base-ui-audio-backend-pipewire
EOF
echo Execution 'pmbootstrap init'...
pmbootstrap init

echo
echo "=== Precompiled Heavyweight APKs (kernel & py3-slint) ==="
echo "Notice: These precompiled binaries are taken from the 'prebuilt-apks' directory"
echo "of this repository and are NOT built on your computer."
echo "However, building them from source locally can take up to several hours,"
echo "so using them is strongly recommended."
echo "Use precompiled APKs? [Y/n]:"
read -r _ans
case "$_ans" in
    ""|[Yy]|[Yy][Ee][Ss])
        PREBUILT_DIR=$(realpath -m "$(dirname "$0")/prebuilt-apks")
        echo "Copying precompiled APKs and signing keys..."
        # pmbootstrap owns the work dir as root; the chroot user (uid 12345) must write the packages index
        sudo mkdir -p "$WORK_DIR"/config_apk_keys || exit 1
        sudo cp -rf "$PREBUILT_DIR"/packages "$WORK_DIR"/ || exit 1
        sudo cp -f "$PREBUILT_DIR"/config_apk_keys/pmos@local-*.rsa.pub "$WORK_DIR"/config_apk_keys/ || exit 1
        sudo chown -R 12345:12345 "$WORK_DIR"/packages || exit 1

        echo "Rebuilding pmbootstrap APKINDEX..."
        pmbootstrap index
        ;;
    *)
        echo "Skipping precompiled APK download. They will be built from source during 3_build.sh."
        ;;
esac
