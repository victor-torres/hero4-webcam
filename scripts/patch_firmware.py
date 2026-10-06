#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Build the patched HERO4 Black v05.00.00 firmware.

    python3 scripts/patch_firmware.py firmware/v05.00.00/camera_firmware.bin build/camera_firmware.bin

Two changes, both data only; bootloaders, DSP, kernel and RTOS code stay byte-identical:

1. SD card hook. The RTOS asks Linux to run /usr/local/share/script/sd_script.sh
   every time the SD card is mounted. We replace that script with an equivalent
   one that also starts <sdcard>/h4.sh when it exists. The UBIFS image is patched
   in place instead of being rebuilt: the script's single data node is rewritten
   uncompressed with exactly the same node length, and the file size in its inode
   node is adjusted.

2. 720p idle preview. RTOS video mode table entry 62 is the idle (not recording)
   1080 SuperView 30 preview: its DSP and encoder secondary sizes go from 848x480
   to 1280x720, so the live stream is 720p with nothing written to the card. Main
   size, bitrate and frame rate stay stock.

Only the official v05.00.00 image is accepted, and the result must match the
tested image byte for byte (both checked by SHA-256).
"""
import argparse
import hashlib
import pathlib
import struct
import sys
import zlib

from lzallright import LZOCompressor

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from fwunpack import GLOBAL_HEADER_SIZE, SECTION_HEADER_SIZE, sections  # noqa: E402

UBIFS_MAGIC = struct.pack("<I", 0x06101831)
CH_SIZE = 24          # common header
DATA_HDR = 48         # common header + key + size + compr_type + pad
INO_NODE, DATA_NODE = 0, 1
COMPR_NONE, COMPR_LZO = 0, 1
INO_SIZE_OFF = CH_SIZE + 16 + 8   # after key and creat_sqnum

# camera_firmware.bin from the official HD4.02 v05.00.00 UPDATE.zip
STOCK_SHA256 = "f1be2cce699691cd1cd7754cad51b82fdb0c99c036a913bc11024454ec5e29c1"
# Result of this script on it: the image flashed and tested by the authors
PATCHED_SHA256 = "32c9cca1b066ebc23ff6387b810cbdff7d13f42a1b00a71bce89a30f6048c47d"

ORIGINAL_MARK = b"#This script is used to mount/umount sd card in ambafs."
HOOK = b"h4.sh"

NEW_SCRIPT = b"""#!/bin/sh
if [ "$1" = "mount" ]; then
mount | grep -q "$3 on $2 type $4" || mount -t $4 $3 $2
[ -f $2/h4.sh ] && sh $2/h4.sh &
fi
if [ "$1" = "umount" ]; then
umount $2
fi
"""


def node_crc(node):
    return zlib.crc32(node[8:]) ^ 0xFFFFFFFF


def nodes(img):
    pos = 0
    while (pos := img.find(UBIFS_MAGIC, pos)) != -1:
        crc, _sqnum, length, ntype = struct.unpack_from("<IQIB", img, pos + 4)
        if CH_SIZE <= length <= 8192 and node_crc(img[pos:pos + length]) == crc:
            yield pos, length, ntype
            pos += length
        else:
            pos += 4


def patch_ubifs(img):
    img = bytearray(img)
    data_hits, inodes = [], {}
    for pos, length, ntype in nodes(bytes(img)):
        inum = struct.unpack_from("<I", img, pos + CH_SIZE)[0]
        if ntype == INO_NODE:
            inodes.setdefault(inum, []).append(pos)
        elif ntype == DATA_NODE:
            size, compr = struct.unpack_from("<IH", img, pos + CH_SIZE + 16)
            payload = bytes(img[pos + DATA_HDR:pos + length])
            if compr == COMPR_LZO:
                payload = bytes(LZOCompressor.decompress(payload, size))
            elif compr != COMPR_NONE:
                continue
            if ORIGINAL_MARK in payload:
                data_hits.append((pos, length, inum, payload))

    if len(data_hits) != 1:
        sys.exit(f"expected exactly one sd_script.sh data node, found {len(data_hits)}")
    pos, length, inum, original = data_hits[0]
    if len(inodes.get(inum, [])) != 1:
        sys.exit(f"expected exactly one inode node for inode {inum}")
    ipos = inodes[inum][0]
    isize = struct.unpack_from("<Q", img, ipos + INO_SIZE_OFF)[0]
    if isize != len(original):
        sys.exit(f"inode size {isize} != data size {len(original)}: file spans several nodes")

    room = length - DATA_HDR
    if len(NEW_SCRIPT) > room:
        sys.exit(f"new script is {len(NEW_SCRIPT)} bytes, only {room} fit")
    new = NEW_SCRIPT + b"\n" * (room - len(NEW_SCRIPT))

    struct.pack_into("<IH", img, pos + CH_SIZE + 16, room, COMPR_NONE)
    img[pos + DATA_HDR:pos + length] = new
    struct.pack_into("<I", img, pos + 4, node_crc(img[pos:pos + length]))

    ilen = struct.unpack_from("<I", img, ipos + 16)[0]
    struct.pack_into("<Q", img, ipos + INO_SIZE_OFF, room)
    struct.pack_into("<I", img, ipos + 4, node_crc(img[ipos:ipos + ilen]))

    print(f"inode {inum}: data node @{pos:#x} ({len(original)} -> {room} bytes, stored raw), "
          f"inode node @{ipos:#x}")
    return bytes(img)


# RTOS (section 1) video mode table of v05.00.00: 229 pointers to 0x13c-byte entries.
RTOS_BASE = 0x03300000
MODE_TABLE, MODE_COUNT = 0x04054158, 229
MAIN_SIZE = 0xC4               # main encoder width, height
MAIN_BITRATE = 0xCC
MAIN_RATE = 0xD4               # time scale, ticks per frame
DSP_SEC_SIZE = 0x70            # secondary buffer width, height
ENC_SEC_SIZE = 0x100           # secondary encoder width, height
SEC_FROM, SEC_TO = (848, 480), (1280, 720)
IDLE720_ENTRY = 62             # idle (not recording) SuperView 30 preview
IDLE720_ADDR = 0x03EF1118
IDLE720_BITRATE = 500_000
IDLE720_RATE = (90000, 3003)   # 29.97 fps


def patch_rtos_idle720(img):
    img = bytearray(img)
    u32 = lambda addr: struct.unpack_from("<I", img, addr - RTOS_BASE)[0]
    e = u32(MODE_TABLE + 4 * IDLE720_ENTRY)
    main = (u32(e + MAIN_SIZE), u32(e + MAIN_SIZE + 4))
    bitrate = u32(e + MAIN_BITRATE)
    rate = (u32(e + MAIN_RATE), u32(e + MAIN_RATE + 4))
    dsp = (u32(e + DSP_SEC_SIZE), u32(e + DSP_SEC_SIZE + 4))
    enc = (u32(e + ENC_SEC_SIZE), u32(e + ENC_SEC_SIZE + 4))
    if not (e == IDLE720_ADDR and main == (1920, 1080) and bitrate == IDLE720_BITRATE
            and rate == IDLE720_RATE and dsp == SEC_FROM and enc == SEC_FROM):
        sys.exit(f"entry {IDLE720_ENTRY} is not the expected idle 1080p30 preview "
                 f"(addr {e:#x}, main {main}, bitrate {bitrate}, rate {rate}, dsp {dsp}, enc {enc})")
    for off in (DSP_SEC_SIZE, ENC_SEC_SIZE):
        struct.pack_into("<II", img, e + off - RTOS_BASE, *SEC_TO)
    print(f"rtos: idle secondary 1280x720 in mode {IDLE720_ENTRY}@{rate[0] / rate[1]:.2f}")
    return bytes(img)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("src", type=pathlib.Path)
    p.add_argument("dst", type=pathlib.Path)
    args = p.parse_args()
    src, dst = args.src, args.dst
    fw = bytearray(src.read_bytes())
    digest = hashlib.sha256(fw).hexdigest()
    if digest != STOCK_SHA256:
        sys.exit(f"{src} is not the official v05.00.00 camera_firmware.bin "
                 f"(sha256 {digest}, expected {STOCK_SHA256})")

    rtos = [s for s in sections(bytes(fw)) if s["addr"] == RTOS_BASE]
    if len(rtos) != 1:
        sys.exit("could not find the RTOS section")
    s = rtos[0]
    start, end = s["start"], s["start"] + s["length"]
    fw[start:end] = patch_rtos_idle720(bytes(fw[start:end]))
    struct.pack_into("<I", fw, start - SECTION_HEADER_SIZE, zlib.crc32(fw[start:end]))

    ubi = [s for s in sections(bytes(fw)) if fw[s["start"]:s["start"] + 4] == b"UBI#"]
    if len(ubi) != 1:
        sys.exit("could not find the UBI section")
    s = ubi[0]
    start, end = s["start"], s["start"] + s["length"]
    if HOOK in fw[start:end] and ORIGINAL_MARK not in fw[start:end]:
        sys.exit("image looks already patched")

    fw[start:end] = patch_ubifs(bytes(fw[start:end]))
    struct.pack_into("<I", fw, start - SECTION_HEADER_SIZE, zlib.crc32(fw[start:end]))
    struct.pack_into("<I", fw, 0, zlib.crc32(fw[GLOBAL_HEADER_SIZE:]))
    if hashlib.sha256(fw).hexdigest() != PATCHED_SHA256:
        sys.exit("patched image differs from the tested one; not writing it")

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(fw)
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
