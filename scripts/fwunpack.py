#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Split an Ambarella camera_firmware.bin into its sections and verify CRCs.

    python3 scripts/fwunpack.py firmware/v05.00.00/camera_firmware.bin [outdir]

Format (after evilwombat/gopro-fw-tools): 224-byte global header whose first
word is the CRC32 of everything after it, then sections, each preceded by a
0x100-byte header: crc, version, build date, length, mem addr, flags, magic.
"""
import pathlib
import struct
import sys
import zlib

GLOBAL_HEADER_SIZE = 224
SECTION_HEADER_SIZE = 0x100
MAGIC = bytes.fromhex("90eb24a3")  # 0xA324EB90 little-endian


def sections(buf):
    pos = 0
    while (pos := buf.find(MAGIC, pos)) != -1:
        hdr = pos - 24
        crc, version, date, length, addr, flags = struct.unpack_from("<6I", buf, hdr)
        start = hdr + SECTION_HEADER_SIZE
        if length > len(buf) - start:
            pos += 4
            continue
        yield dict(hdr=hdr, start=start, length=length, crc=crc, version=version,
                   date=date, addr=addr, flags=flags)
        pos = start + length


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    path = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2]) if len(sys.argv) == 3 else None
    buf = path.read_bytes()

    want = struct.unpack_from("<I", buf, 0)[0]
    got = zlib.crc32(buf[GLOBAL_HEADER_SIZE:])
    print(f"global crc {want:08x} {'OK' if want == got else f'MISMATCH (actual {got:08x})'}")

    if out:
        out.mkdir(parents=True, exist_ok=True)
    print(" #    offset    length       crc     ver       date   memaddr     flags  head")
    for i, s in enumerate(sections(buf)):
        data = buf[s["start"]:s["start"] + s["length"]]
        ok = "OK" if zlib.crc32(data) == s["crc"] else "BAD"
        print(f"{i:2d} {s['start']:9d} {s['length']:9d}  {s['crc']:08x} {ok:3s} {s['version']:08x} "
              f"{s['date']:08x}  {s['addr']:08x}  {s['flags']:08x}  {data[:8].hex()}")
        if out:
            (out / f"section_{i}").write_bytes(data)


if __name__ == "__main__":
    main()
