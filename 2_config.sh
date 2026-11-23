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
echo "Notice: These precompiled binaries are downloaded from GitHub Releases"
echo "and are NOT built on your computer."
echo "However, building them from source locally can take up to several hours,"
echo "so downloading is strongly recommended."
printf "Download precompiled APKs? [Y/n]: "
read -r _ans
case "$_ans" in
    ""|[Yy]|[Yy][Ee][Ss])
        BASE_URL="https://github.com/elmot/the-mtrx-handset/releases/download/precompiled-heavyweight-apks"
        PKG_DIR="$WORK_DIR/packages/v26.06/armv7"
        KEY_DIR="$WORK_DIR/config_apk_keys"
        mkdir -p "$PKG_DIR" "$KEY_DIR"

        for apk in \
            linux-postmarketos-qcom-msm8916-6.12.1-r8.apk \
            py3-slint-1.19.0-r0.apk
        do
            echo "Downloading $apk..."
            curl -fL --retry 3 -o "$PKG_DIR/$apk" "$BASE_URL/$apk" || exit 1
        done

        echo "Downloading build-pub-key.tgz..."
        curl -fL --retry 3 -o "$KEY_DIR/build-pub-key.tgz" "$BASE_URL/build-pub-key.tgz" || exit 1
        tar -xzf "$KEY_DIR/build-pub-key.tgz" -C "$KEY_DIR" || exit 1
        rm -f "$KEY_DIR/build-pub-key.tgz"

        echo "Rebuilding pmbootstrap APKINDEX..."
        pmbootstrap index
        ;;
    *)
        echo "Skipping precompiled APK download. They will be built from source during 3_build.sh."
        ;;
esac
