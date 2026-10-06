#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Unpack the official HERO4 Black v05.00.00 update for the build.

    python3 scripts/prepare_firmware.py firmware/UPDATE.zip

Download UPDATE.zip from GoPro first (see README). Writes firmware/v05.00.00/:
the three update files, sections/section_N, kernel.config (embedded in the
kernel image) and rootfs/ (the Linux root filesystem, for the camera's
libcsi.so). Needs ubi_reader (pip install ubi_reader).
"""
import hashlib
import pathlib
import subprocess
import sys
import zipfile
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from fwunpack import sections  # noqa: E402

UPDATE_SHA256 = "1d88f5dd7a4fdaa6c1664841996efaf68302b2fe0be89d29eb7bec514124ac7e"
OUT = pathlib.Path(__file__).resolve().parent.parent / "firmware" / "v05.00.00"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    zpath = pathlib.Path(sys.argv[1])
    digest = hashlib.sha256(zpath.read_bytes()).hexdigest()
    if digest != UPDATE_SHA256:
        sys.exit(f"{zpath} is not the official v05.00.00 UPDATE.zip (sha256 {digest})")

    OUT.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zpath) as z:
        z.extractall(OUT)
    fw = (OUT / "camera_firmware.bin").read_bytes()

    secdir = OUT / "sections"
    secdir.mkdir(exist_ok=True)
    for i, s in enumerate(sections(fw)):
        (secdir / f"section_{i}").write_bytes(fw[s["start"]:s["start"] + s["length"]])

    kernel = (secdir / "section_3").read_bytes()
    pos = kernel.find(b"IKCFG_ST")
    if pos < 0:
        sys.exit("no embedded kernel config in section 3")
    (OUT / "kernel.config").write_bytes(zlib.decompressobj(31).decompress(kernel[pos + 8:]))

    subprocess.run(["ubireader_extract_files", "-k", "-o", str(OUT / "rootfs"),
                    str(secdir / "section_4")], check=True)
    print(f"done: {OUT}")


if __name__ == "__main__":
    main()
