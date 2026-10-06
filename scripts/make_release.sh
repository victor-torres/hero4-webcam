#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Package a prebuilt release from build/ (run scripts/build_all.sh first):
#
#   scripts/make_release.sh v1.0.0
#
# Writes dist/hero4-webcam-<version>.zip: the card files (h4.sh, h4/) and the
# firmware patcher, so users need only Python, not Docker. No GoPro code: the
# patcher builds UPDATE/ from the user's own copy of GoPro's UPDATE.zip.
# Also writes dist/faad2-<version>.tar.gz, the source of the faad2 statically
# linked into h4uvc (GPL-2.0), to attach to the release next to it.
set -euo pipefail
cd "$(dirname "$0")/.."
ver=${1:?usage: $0 <version>}
faad_ver=$(sed -n 's/.*PACKAGE_VERSION=\\"\([0-9.]*\)\\".*/\1/p' scripts/build_h4uvc.sh)
[ -n "$faad_ver" ] || { echo "faad2 version not found in build_h4uvc.sh"; exit 1; }
[ -f build/sdcard/h4.sh ] || { echo "run scripts/build_all.sh first"; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "commit your changes first: the binaries must match a commit"; exit 1; }

name=hero4-webcam-$ver
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
d=$stage/$name
mkdir -p "$d"
cp build/sdcard/h4.sh "$d/"
cp -R build/sdcard/h4 "$d/"
cp scripts/patch_firmware.py scripts/fwunpack.py LICENSE "$d/"
commit=$(git rev-parse --short HEAD)
cat > "$d/README.txt" << EOF
hero4-webcam $ver: GoPro HERO4 Black as a USB webcam and microphone
https://github.com/victor-torres/hero4-webcam  (built from commit $commit)

Needs a HERO4 Black on official firmware v05.00.00, GoPro's v05.00.00
UPDATE.zip, a FAT32 microSD card and Python 3.9+. Flashing modified firmware
is at your own risk; see the project README.

1. Patch the firmware onto the card (writes UPDATE/ on it):

     python3 -m pip install lzallright
     python3 patch_firmware.py UPDATE.zip /path/to/sdcard

2. Copy h4.sh and the h4 folder to the root of the card.
3. Put the card in the camera and turn it on; it updates itself (a few
   minutes). Afterwards delete UPDATE/ from the card.
4. Connect the camera by USB and turn it on: "GoPro HERO4" shows up as a
   camera and a microphone after about 45 s.

Licenses: GPL-2.0 (LICENSE). The kernel modules are built from the project's
src/ against evilwombat/linux-hero4; h4uvc statically links faad2 $faad_ver
(GPL-2.0), whose source is attached to the release as faad2-$faad_ver.tar.gz.
Code from FAAD2 is copyright (c) Nero AG, www.nero.com.
No GoPro firmware is included.
EOF

mkdir -p dist
rm -f "dist/$name.zip"
(cd "$stage" && zip -qrX - "$name") > "dist/$name.zip"
tar -czf "dist/faad2-$faad_ver.tar.gz" -C vendor --exclude .git faad2
ls -l "dist/$name.zip" "dist/faad2-$faad_ver.tar.gz"
unzip -l "dist/$name.zip"
