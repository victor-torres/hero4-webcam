#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Build the camera-side UVC server (src/h4uvc) in the hero4-build container:
#
#   docker run --rm -v "$PWD":/repo hero4-build /repo/scripts/build_h4uvc.sh
#
# Writes build/tools/h4uvc. The
# microphone path decodes AAC with faad2 (GPLv2), statically linked: unpack the
# 2.11.4 release into vendor/faad2 first (github.com/FreewareAdvancedAudio/faad2).
set -euo pipefail
out=/repo/build/tools
mkdir -p "$out"
faad=/repo/vendor/faad2
# Retries: gcc segfaults now and then under emulation.
for i in 1 2 3 4 5; do arm-linux-gnueabi-gcc -std=gnu99 -O2 -Wall -Wno-unused-parameter -mfpu=neon -mfloat-abi=softfp \
    -I/repo/src/h4uvc -I$faad/include -I$faad/libfaad \
    -DAPPLY_DRC -DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 -DHAVE_STRING_H=1 -DHAVE_STRINGS_H=1 \
    -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DSTDC_HEADERS=1 -DHAVE_LRINTF=1 -DPACKAGE_VERSION=\"2.11.4\" \
    -o "$out/h4uvc" /repo/src/h4uvc/h4uvc.c /repo/src/h4uvc/sps.c /repo/src/h4uvc/ts-demux.c $faad/libfaad/*.c -lm -lpthread && break
done
arm-linux-gnueabi-strip "$out/h4uvc"
file "$out/h4uvc"
