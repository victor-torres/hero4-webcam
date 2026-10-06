#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Build the camera-side userland tools (src/h4csi) against the camera's own
# libraries (soft-float EABI, glibc 2.17), inside the hero4-build container:
#
#   docker run --rm -v "$PWD":/repo hero4-build /repo/scripts/build_tools.sh
#
# Writes build/tools/h4csi.
set -euo pipefail

root=/repo/firmware/v05.00.00/rootfs/634535477/linux
out=/repo/build/tools
mkdir -p "$out"

arm-linux-gnueabi-gcc -O2 -Wall -o "$out/h4csi" /repo/src/h4csi/h4csi.c \
    -L"$root/usr/lib" -lcsi \
    -Wl,-rpath-link,"$root/usr/lib" -Wl,-rpath-link,"$root/lib" \
    -Wl,--allow-shlib-undefined
arm-linux-gnueabi-strip "$out/h4csi"
file "$out/h4csi"
