#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Build everything and lay out the SD card in build/sdcard/:
#
#   scripts/build_all.sh
#
# Needs Docker, firmware/v05.00.00 from scripts/prepare_firmware.py, and the
# faad2 2.11.4 release unpacked into vendor/faad2. The kernel tree lives in a
# Docker volume (HERO4_VOLUME, default hero4-linux) because the macOS
# filesystem is case-insensitive.
set -euo pipefail
cd "$(dirname "$0")/.."
vol=${HERO4_VOLUME:-hero4-linux}

[ -f firmware/v05.00.00/kernel.config ] || { echo "run scripts/prepare_firmware.py first"; exit 1; }
[ -d vendor/faad2/libfaad ] || { echo "unpack faad2 2.11.4 into vendor/faad2 first"; exit 1; }

docker build -t hero4-build docker
docker volume create "$vol" > /dev/null
docker run --rm --platform linux/amd64 -v "$vol":/work hero4-build sh -c \
    '[ -d linux ] || git clone --depth 1 https://github.com/evilwombat/linux-hero4 linux'

# Each step is retried: the old toolchain crashes now and then under x86
# emulation (Rosetta/qemu on Apple Silicon), even in shell scripts.
run() {
    for i in 1 2 3; do
        docker run --rm --platform linux/amd64 -v "$vol":/work -v "$PWD":/repo hero4-build "/repo/scripts/$1" && return
        echo "$1 failed (attempt $i), retrying"
    done
    return 1
}
run build_modules.sh   # usb-common, udc-core, ambarella_udc, libcomposite, g_ether
run build_h4cam.sh     # videodev, h4cam (reconfigures the tree with V4L2)
run build_udc.sh       # ambarella_udc_h4, h4_udc_dev
run build_tools.sh     # h4csi
run build_h4uvc.sh     # h4uvc

card=build/sdcard
rm -rf "$card"
mkdir -p "$card/UPDATE" "$card/h4"
cp build/camera_firmware.bin "$card/UPDATE/" 2> /dev/null ||
    echo "note: no build/camera_firmware.bin (scripts/patch_firmware.py), UPDATE/ left empty"
cp firmware/v05.00.00/camera_loaders.bin firmware/v05.00.00/hd4_update.txt "$card/UPDATE/"
cp sdcard/h4.sh "$card/"
cp sdcard/h4/*.sh "$card/h4/"
for m in usb-common udc-core ambarella_udc ambarella_udc_h4 libcomposite h4_udc_dev videodev h4cam g_ether; do
    cp "build/modules/$m.ko" "$card/h4/"
done
cp build/tools/h4csi build/tools/h4uvc "$card/h4/"
echo
echo "SD card layout in $card:"
find "$card" -type f | sort
