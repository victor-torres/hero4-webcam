#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Build the USB gadget modules the stock HERO4 Black kernel lacks.
# Runs inside the hero4-build container (kernel tree lives in the hero4-linux
# volume because the macOS filesystem is case-insensitive):
#
#   docker build -t hero4-build docker
#   docker volume create hero4-linux
#   docker run --rm -v hero4-linux:/work hero4-build \
#       git clone --depth 1 https://github.com/evilwombat/linux-hero4 linux
#   docker run --rm -v hero4-linux:/work -v "$PWD":/repo hero4-build /repo/scripts/build_modules.sh
#
# Uses the config extracted from the camera's kernel image and writes the
# .ko files to build/modules.
set -euo pipefail

cd /work/linux
cp /repo/firmware/v05.00.00/kernel.config .config
for opt in USB_GADGET USB_AMBARELLA USB_ETH USB_G_NCM USB_G_SERIAL; do
    scripts/config --module "$opt"
done
# macOS has no RNDIS driver; plain CDC ECM / NCM work without one.
scripts/config --disable USB_ETH_RNDIS --disable USB_ETH_EEM
make olddefconfig >/dev/null

# Retries: gcc segfaults now and then under emulation.
for i in 1 2 3 4 5; do make -j"$(nproc)" modules_prepare && break; done
for i in 1 2 3 4 5; do make -j2 SUBDIRS=drivers/usb modules && break; done

out=/repo/build/modules
mkdir -p "$out"
cp -v drivers/usb/usb-common.ko drivers/usb/gadget/*.ko "$out"/
