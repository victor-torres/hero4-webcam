#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Upload a file to the camera over the USB link.

    scripts/h4send.py <local file> <dest on card> [interface]    e.g. sdcard/h4.sh h4.sh en10

The interface is the camera's "Ethernet Gadget" port (networksetup
-listallhardwareports); default en10. Needs the receiver running on the camera,
started from the USB shell (h4/shell on the card):
    tcpsvd -c 1 169.254.77.1 2324 sh /tmp/fuse_d/h4/h4rx.sh &

The card silently ignores renames across folders, so the receiver writes
straight to the destination; it only accepts UPDATE/* and h4.sh.
"""
import socket, sys
IP_BOUND_IF = 25
src, dst = sys.argv[1], sys.argv[2]
iface = sys.argv[3] if len(sys.argv) > 3 else "en10"
s = socket.socket()
s.setsockopt(socket.IPPROTO_IP, IP_BOUND_IF, socket.if_nametoindex(iface))
s.settimeout(60)
s.connect(("169.254.77.1", 2324))
s.sendall(dst.encode() + b"\n")
with open(src, "rb") as fh:
    while chunk := fh.read(1 << 16):
        s.sendall(chunk)
s.shutdown(socket.SHUT_WR)
print(s.recv(256).decode().strip() or "(no reply)")
s.close()
