#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# One upload per connection: first line = destination (relative to the card),
# rest = file contents. Only UPDATE/<name> and h4.sh are accepted.
read f
case "$f" in
UPDATE/camera_firmware.bin|UPDATE/camera_loaders.bin|UPDATE/hd4_update.txt|h4.sh) ;;
*) echo "refused: $f"; exit 1 ;;
esac
cat > "/tmp/fuse_d/$f" && sync && echo "ok $f"
