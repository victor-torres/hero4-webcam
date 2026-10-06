# hero4-webcam

Turn a **GoPro HERO4 Black** into a plug-and-play USB webcam, with no HDMI capture card and no app on the computer.

After the patch the camera boots as a standard USB device: **webcam** (1280x720 at 29.97 fps), **microphone**
(48 kHz stereo, the camera's own mic) and **USB Ethernet**. On macOS it shows up as "GoPro HERO4" in Photo Booth,
QuickTime, OBS, Zoom, Chrome and other apps, about 45 s after power-on. Video latency is about 190 ms glass to glass.

> **Risk:** this flashes modified firmware to your camera. It worked on ours, but a bad flash can brick a camera,
> and modified firmware may void the warranty. You do this at your own risk. This project is not affiliated with
> or endorsed by GoPro. "GoPro" and "HERO" are trademarks of GoPro, Inc.

## Requirements

- **GoPro HERO4 Black** (model HD4.02). Not the Silver or the Session: their firmware is different.
- Official firmware **v05.00.00** on the camera. The patch is made from this exact version only.
- A microSD card (FAT32) and a USB cable.
- A computer with Docker (for the cross-compiler) and Python 3.9+. Tested on macOS (Apple Silicon, with
  [colima](https://github.com/abiosoft/colima) or Docker Desktop). The camera has only been tested with macOS
  as the host.

## How it works

The HERO4 Black runs two operating systems: an RTOS (camera, encoder, USB) and Linux 3.8 (Wi-Fi, HTTP API,
streaming). The firmware is only protected by CRCs, not signed.

1. **Firmware patch** (`scripts/patch_firmware.py`), data only, 346 bytes. Bootloaders, DSP, kernel and RTOS code
   stay byte-identical.
   - A Linux script that runs whenever the SD card is mounted now also runs `h4.sh` from the card.
   - One entry of the RTOS video mode table changes so the idle preview (1080 SuperView 30, not recording) is
     1280x720 instead of 848x480. Nothing is written to the card while streaming.
2. **Kernel modules**, loaded from the card by `h4.sh`. GoPro's kernel has no USB gadget support. We build the
   modules from the GPL source: a module that registers the USB controller, a patched controller driver
   (isochronous transfers for the microphone), and `h4cam`, a composite gadget with UVC, UAC1 and CDC ECM.
3. **`h4uvc`**, a small program on the camera. It takes the camera's own live preview stream (H.264 + AAC) and
   sends the H.264 to the webcam as-is; macOS decodes it. The AAC is decoded and resampled for the microphone.

The whole story, with the dead ends, is in [docs/findings.md](docs/findings.md).

## Step by step

### 1. Get the official firmware

Download GoPro's v05.00.00 update for the HERO4 Black:

```
https://device-firmware.gp-static.com/13/HD4.02/camera_fw/05.00.00/UPDATE.zip
```

Save it as `firmware/UPDATE.zip`, and keep a copy on another card. The scripts check its SHA-256
(`1d88f5dd…124ac7e`) and refuse anything else.

### 2. Unpack and patch

```bash
python3 -m venv .venv
```

```bash
.venv/bin/pip install -r requirements.txt
```

```bash
PATH=.venv/bin:$PATH .venv/bin/python scripts/prepare_firmware.py firmware/UPDATE.zip
```

```bash
.venv/bin/python scripts/patch_firmware.py firmware/v05.00.00/camera_firmware.bin build/camera_firmware.bin
```

The patcher only writes its output if the result is byte-for-byte the image we flashed and tested
(SHA-256 `32c9cca1…`).

### 3. Build the modules and tools

Unpack the [faad2 2.11.1](https://github.com/knik0/faad2/releases/tag/2.11.1) source (AAC decoder) into
`vendor/faad2`, then:

```bash
scripts/build_all.sh
```

This builds the cross-compiler image (gcc 4.7.3, the camera kernel's own compiler) and clones
[evilwombat/linux-hero4](https://github.com/evilwombat/linux-hero4), the HERO4 kernel source, into a Docker volume.
It then builds the modules, `h4csi` and `h4uvc`, and lays out the SD card in `build/sdcard/`. The first run takes a
while.

### 4. Flash

1. Make sure the camera is on v05.00.00 and the battery is charged.
2. Copy everything in `build/sdcard/` to the root of the card: `UPDATE/`, `h4.sh` and `h4/`.
3. Put the card in the camera and turn it on. It installs the update by itself (a few minutes; don't turn it off).
4. When the camera is back to normal, delete the `UPDATE/` folder from the card.

### 5. Use it

Connect the camera to the computer by USB and turn it on. About 45 s later "GoPro HERO4" appears as a camera and as
a microphone. Pick it in any app.

## Options

Create these empty files in `h4/` on the card:

| File | Effect |
|---|---|
| `ether_only` | USB Ethernet only, no webcam (camera at `169.254.77.1`, HTTP API at `/gp/gpControl/...`) |
| `shell` | Root shell over the USB link: `nc 169.254.77.1 2323`. **No password.** Only reachable over the USB cable, not Wi-Fi |
| `debug` | Logs to the card (`h4/uvc/`, `h4_usb.log`) instead of RAM |

With USB Ethernet up, `scripts/stream_test.py --camera 169.254.77.1 --iface en10` captures and analyzes the raw
stream. The interface is the "Ethernet Gadget" port in `networksetup -listallhardwareports`, not always `en10`.

## Undo

- **Without reflashing:** delete `h4.sh` from the card. The hook does nothing without it, and the camera behaves like
  stock.
- **Back to stock firmware:** put the official `UPDATE.zip` contents in `UPDATE/` on the card and update the same way.
  The camera accepted a same-version image for the patch, so this should work, but we haven't tested it.
- **Bricked camera:** [evilwombat/gopro-usb-tools](https://github.com/evilwombat/gopro-usb-tools) can boot a HERO4
  Black/Silver through the SoC's USB recovery mode (experimental).

## Known issues

- **Microphone crackle:** short clusters of clicks every 1–3 s. The cause is still being investigated
  ([docs/findings.md](docs/findings.md), "mic crackle").
- After the camera re-enumerates (power cycle, replug), some apps need the video device selected again. OBS's video
  capture source is one of them.
- If the computer goes to sleep, the camera powers off. Its power button brings it back.
- Only tested with macOS as the host.

## Layout

- `scripts/`: firmware unpacking and patching, build scripts, `stream_test.py`, `h4send.py` (file upload, needs the shell)
- `sdcard/`: `h4.sh` and the scripts that go in `h4/` on the card
- `src/ambarella_udc/`: patched Ambarella USB device controller driver
- `src/h4_udc_dev/`: registers the controller's platform device
- `src/h4cam/`: composite gadget: UVC (H.264 over bulk), UAC1 microphone, CDC ECM
- `src/h4uvc/`: camera-side server feeding the webcam and the microphone
- `src/h4csi/`: get/set the camera's CSI keys (used to keep the RTOS from suspending Linux)
- `docker/`: cross-build environment
- `firmware/`, `vendor/`: local only (GoPro firmware, faad2), git-ignored

## Credits

- [evilwombat](https://github.com/evilwombat): HERO4 kernel tree, firmware tools, USB recovery tools
- [hypoxic/hero4-session](https://github.com/hypoxic/hero4-session): HERO4 Session research
- [KonradIT/goprowifihack](https://github.com/KonradIT/goprowifihack): GoPro HTTP API documentation
- [faad2](https://github.com/knik0/faad2): AAC decoder

## License

GPL-2.0, see [LICENSE](LICENSE). The kernel modules are modified Linux kernel code and keep their original
headers. `h4uvc` links faad2 (GPL-2.0) statically.

Contributions are welcome. Pull requests that fix the microphone crackle are especially welcome.
