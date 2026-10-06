#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Build the patched Ambarella UDC driver (src/ambarella_udc) and the module
# that registers its platform device (src/h4_udc_dev) against the kernel tree
# prepared by build_h4cam.sh:
#
#   docker run --rm -v hero4-linux:/work -v "$PWD":/repo hero4-build /repo/scripts/build_udc.sh
set -euo pipefail
cd /work/linux
rm -rf /tmp/udc && cp -r /repo/src/ambarella_udc /tmp/udc
for i in 1 2 3 4 5; do make -j2 M=/tmp/udc modules && break; done
cp -v /tmp/udc/ambarella_udc.ko /repo/build/modules/ambarella_udc_h4.ko
rm -rf /tmp/udc_dev && cp -r /repo/src/h4_udc_dev /tmp/udc_dev
for i in 1 2 3 4 5; do make -j2 M=/tmp/udc_dev modules && break; done
cp -v /tmp/udc_dev/h4_udc_dev.ko /repo/build/modules/
