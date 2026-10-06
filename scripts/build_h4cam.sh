#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Build the webcam modules (videodev, h4cam) inside the
# hero4-build container, against the same kernel tree as build_modules.sh:
#
#   docker run --rm -v hero4-linux:/work -v "$PWD":/repo hero4-build /repo/scripts/build_h4cam.sh
#
# The stock kernel has no V4L2 core (CONFIG_MEDIA_SUPPORT is off), so videodev
# is built as a module too. Writes build/modules/{videodev,h4cam}.ko.
set -euo pipefail

cd /work/linux
cp /repo/firmware/v05.00.00/kernel.config .config
for opt in USB_GADGET USB_AMBARELLA USB_ETH MEDIA_SUPPORT VIDEO_DEV VIDEO_V4L2; do
    scripts/config --module "$opt"
done
scripts/config --enable MEDIA_CAMERA_SUPPORT
scripts/config --disable USB_ETH_RNDIS --disable USB_ETH_EEM
make olddefconfig >/dev/null
grep -E "^CONFIG_(MEDIA_SUPPORT|VIDEO_DEV|VIDEO_V4L2|MEDIA_CAMERA_SUPPORT)=" .config

for i in 1 2 3 4 5; do make -j"$(nproc)" modules_prepare && break; done
for i in 1 2 3 4 5; do make -j2 SUBDIRS=drivers/media/v4l2-core modules && break; done

rm -rf /tmp/h4cam && cp -r /repo/src/h4cam /tmp/h4cam
for i in 1 2 3 4 5; do make -j2 M=/tmp/h4cam modules && break; done

out=/repo/build/modules
mkdir -p "$out"
cp -v drivers/media/v4l2-core/videodev.ko /tmp/h4cam/h4cam.ko "$out"/
modinfo "$out"/h4cam.ko | grep -E "vermagic|depends"
modinfo "$out"/videodev.ko | grep -E "vermagic|depends"
