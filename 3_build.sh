#!/bin/sh
set +x

PASSWORD=""
HAS_PASSWORD=0

while [ $# -gt 0 ]; do
    case "$1" in
        --password)
            if [ $# -lt 2 ]; then
                echo "Error: --password requires a value" >&2
                exit 1
            fi
            PASSWORD="$2"
            HAS_PASSWORD=1
            shift 2
            ;;
        --password=*)
            PASSWORD="${1#--password=}"
            HAS_PASSWORD=1
            shift
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

. "$(dirname "$0")"/activate.sh || exit 1
echo pmbootstrap version:
pmbootstrap --version
pmbootstrap build firmware-nokia-argon --arch=armv7
if [ "$HAS_PASSWORD" -eq 1 ]; then
    pmbootstrap install --password "$PASSWORD"
else
    pmbootstrap install
fi
