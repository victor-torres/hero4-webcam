#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Capture the HERO4 Black preview stream and report what it contains.

Over Wi-Fi (join the camera's network first):

    python3 scripts/stream_test.py              # 720p, 4 Mbps, 20 s
    python3 scripts/stream_test.py --window 4   # 480p

Over the USB network link (gate 5), give the camera's address and the Mac's
network interface for it (link-local addresses need the interface pinned):

    python3 scripts/stream_test.py --camera 169.254.77.1 --iface en10

The stream is the secondary encoder of the active mode-table entry. Idle in
1080 SuperView 30 it is 848x480 with nothing written to the card (--window 0
keeps the entry's own size):

    python3 scripts/stream_test.py --camera 169.254.77.1 --iface en10 \\
        --mode 8/8/0 --window 0 --bitrate 8000000

In 1080 Wide 30 the idle preview is 432x240; --record gives 848x480.

Writes captures/<timestamp>/{camera.json,stream.ts,ffprobe.json}. Needs ffmpeg/ffprobe.
"""
import argparse
import http.client
import json
import pathlib
import shutil
import socket
import subprocess
import sys
import threading
import time

CAMERA = "10.5.5.9"
IFACE = None
IP_BOUND_IF = 25  # macOS: send through one interface regardless of the routing table
PORT = 8554
KEEP_ALIVE = b"_GPHD_:0:0:2:0.000000\n"


def pin(sock):
    if IFACE:
        sock.setsockopt(socket.IPPROTO_IP, IP_BOUND_IF, socket.if_nametoindex(IFACE))
    return sock


def get(path):
    conn = http.client.HTTPConnection(CAMERA, timeout=8)
    conn.sock = pin(socket.socket(socket.AF_INET, socket.SOCK_STREAM))
    conn.sock.settimeout(8)
    conn.sock.connect((CAMERA, 80))
    try:
        conn.request("GET", path)
        return conn.getresponse().read().decode()
    finally:
        conn.close()


def keep_alive(stop):
    sock = pin(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
    while not stop.is_set():
        sock.sendto(KEEP_ALIVE, (CAMERA, PORT))
        stop.wait(2.5)


def receive(sock, seconds, ts):
    """Write the TS payload of each datagram to ts.

    gpStream (proto_v2) sends 1328-byte datagrams: a 12-byte header whose
    bytes 10-11 give the payload length (a multiple of 188), the TS packets,
    then padding. Fed to ffmpeg as raw udp://, the header and padding reach
    the TS demuxer and corrupt frames.
    """
    good = bad = 0
    end = time.time() + seconds
    with open(ts, "wb") as fh:
        while time.time() < end:
            try:
                d = sock.recv(65536)
            except socket.timeout:
                continue
            n = int.from_bytes(d[10:12], "big") if len(d) >= 12 else 0
            if n and n % 188 == 0 and 12 + n <= len(d) and d[12] == 0x47:
                fh.write(d[12:12 + n])
                good += 1
            else:
                bad += 1
    return good, bad


def main():
    global CAMERA, IFACE
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--window", type=int, default=7, help="setting 64: 4=480p, 7=720p, 8=960x720")
    p.add_argument("--bitrate", type=int, default=4_000_000, help="setting 62, bits/s")
    p.add_argument("--seconds", type=int, default=20)
    p.add_argument("--mode", help="video resolution/fps/fov as setting values, e.g. 9/8/0 = 1080 30 Wide")
    p.add_argument("--record", action="store_true", help="record to the SD card while capturing")
    p.add_argument("--app-mode", type=int, default=0, help="camera mode: 0=video, 3=hidden broadcast mode")
    p.add_argument("--camera", default=CAMERA, help="camera address")
    p.add_argument("--iface", help="network interface to use, e.g. en10 for the USB link")
    args = p.parse_args()
    CAMERA, IFACE = args.camera, args.iface

    for tool in ("ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found (brew install ffmpeg)")

    out = pathlib.Path(__file__).resolve().parent.parent / "captures" / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True)

    camera = json.loads(get("/gp/gpControl"))
    (out / "camera.json").write_text(json.dumps(camera, indent=2))
    info = camera.get("info", {})
    print(f"camera: {info.get('model_name')} fw {info.get('firmware_version')}")

    if args.mode:
        for setting, value in zip((2, 3, 4), args.mode.split("/")):
            print(f"set {setting}={value}:", get(f"/gp/gpControl/setting/{setting}/{value}").strip())
        # The preview pipeline only picks up new video settings when it is rebuilt.
        print("rebuild via photo mode:", get("/gp/gpControl/command/mode?p=1").strip())
        time.sleep(3)
    print("camera mode:", get(f"/gp/gpControl/command/mode?p={args.app_mode}").strip())
    time.sleep(3)
    print("set window:", get(f"/gp/gpControl/setting/64/{args.window}").strip())
    print("set bitrate:", get(f"/gp/gpControl/setting/62/{args.bitrate}").strip())
    if args.record:
        print("shutter on:", get("/gp/gpControl/command/shutter?p=1").strip())
        time.sleep(2)
    # Bind before restarting the stream so the first datagrams are not lost.
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    sock.bind(("0.0.0.0", PORT))
    sock.settimeout(1)
    print("restart stream:", get("/gp/gpControl/execute?p1=gpStream&a1=proto_v2&c1=restart").strip())

    stop = threading.Event()
    threading.Thread(target=keep_alive, args=(stop,), daemon=True).start()
    ts = out / "stream.ts"
    try:
        good, bad = receive(sock, args.seconds, ts)
        print(f"datagrams: {good} payload, {bad} unrecognised")
    finally:
        sock.close()
        stop.set()
        if args.record:
            print("shutter off:", get("/gp/gpControl/command/shutter?p=0").strip())

    if not ts.exists() or ts.stat().st_size == 0:
        sys.exit("no stream data received")

    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(ts)],
        capture_output=True, text=True,
    ).stdout
    (out / "ffprobe.json").write_text(probe)
    for s in json.loads(probe).get("streams", []):
        if s.get("codec_type") == "video":
            print(f"video: {s.get('codec_name')} {s.get('profile')} {s.get('width')}x{s.get('height')} "
                  f"{s.get('avg_frame_rate')} fps")
    print(f"measured bitrate: {ts.stat().st_size * 8 / args.seconds / 1e6:.2f} Mbps")
    print(f"saved to {out}")


if __name__ == "__main__":
    main()
