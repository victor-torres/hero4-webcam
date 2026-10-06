# Findings

The project's lab notebook, kept in the order things were found. "Verified" means checked on a real camera;
everything else is from third-party docs or static analysis.

Some tools mentioned here were dropped from this repository once the camera worked as a plug-and-play webcam:
an OBS source plugin, a web viewer, the broadcast-mode script (`bcast.sh`), the runtime gadget switch (`uvc_on.sh`),
an RTOS disassembly helper, and the patcher's `--sec720` option (720p while recording). The root shell on port 2323
is now off unless `h4/shell` exists on the card.

## 2026-10-02 — desk research

### Stock firmware already has a 720p stream setting
- `http://10.5.5.9/gp/gpControl/setting/64/7` → stream window 1280x720 (also `8` = 960x720, `4` = 480p).
- `http://10.5.5.9/gp/gpControl/setting/62/<bps>` → stream bitrate; presets 250k–2.4M, reportedly accepts any number (e.g. 7000000) but Wi-Fi drops packets.
- Start: `http://10.5.5.9/gp/gpControl/execute?p1=gpStream&a1=proto_v2&c1=restart`, MPEG-TS on UDP 8554, keep-alive `_GPHD_:0:0:2:0.000000\n` to 10.5.5.9:8554 every ~2.5 s.
- Source: [goprowifihack HERO4 WifiCommands](https://github.com/KonradIT/goprowifihack/blob/master/HERO4/WifiCommands.md). Not yet verified on our camera → gate 0.

Implication: the encoder side of "HD streaming" may need no firmware work at all; the bottleneck is the Wi-Fi transport.

### Architecture
- Ambarella A9, dual OS: RTOS (camera, encoder, USB by default) + Linux 3.8 (Wi-Fi, HTTP API, streamer). They talk over RPMSG (`CONFIG_RPMSG=y`, `CONFIG_MACH_BOSS=y` in the Session config).
- Update file `camera_firmware.bin` is a standard Ambarella image; `fwunpacker` from [evilwombat/gopro-fw-tools](https://github.com/evilwombat/gopro-fw-tools) splits it into sections, one of which is the Linux rootfs (UBIFS).
- Partitions seen on the Session: `bst bld hal pri sec bak rom dsp lnx swp add adc`.

### Linux can own the USB port (seen on HERO4 Session)
- RTOS shell: `t app usb [mtp/msc/rs232/linux/linux_once]`.
- Session kernel config: `CONFIG_USB_GADGET_AMBARELLA=y`, `CONFIG_USB_G_SERIAL=y`; `USB_ETH`, `USB_G_NCM`, `FUNCTIONFS`, UVC not built.
- After `t app usb linux_once` the host sees a serial console (login `default`, no password).
- Source: [hypoxic/hero4-session](https://github.com/hypoxic/hero4-session). The Black shares the codebase ("Banzai") but this is unverified there → gates 1 and 4.

### Running our own code on the Black
- Stock HERO4 Black/Silver does not execute `autoexec.ash`.
- Known route: patched firmware (Alti Force, based on v3.00) that runs `override.sh` from the SD card on the Linux side — [KonradIT/overridehack](https://github.com/konradit/overridehack). CamDo sells a similar serial-locked patch.
- Plan: don't flash a third-party binary. Diff it against stock (or just read the stock init scripts) and make the equivalent change to v05.00 ourselves → gate 3. Open question: whether the update image is only CRC-checked or signed.

### Why not UVC
- UVC gadget needs V4L2 in the kernel (likely absent) plus a userspace feeder.
- macOS apps expect MJPEG/YUY2 from UVC devices; UVC-H.264 support is poor. The camera hands Linux H.264 only, and transcoding 720p30 to MJPEG on a dual 1 GHz Cortex-A9 in software is unlikely to be real-time.
- Getting raw YUV out of the RTOS side would mean patching the closed RTOS image.

### Recovery
- [evilwombat/gopro-usb-tools](https://github.com/evilwombat/gopro-usb-tools) can boot a hard-bricked HERO4 Black/Silver through the SoC's USB command mode. Experimental.

### Reference downloads
- Official firmware v05.00.00: `https://device-firmware.gp-static.com/13/HD4.02/camera_fw/05.00.00/UPDATE.zip` (28,448,855 bytes; HTTP 200 on 2026-10-02).
- Mirror list: [KonradIT/gopro-firmware-archive](https://github.com/KonradIT/gopro-firmware-archive/blob/main/data/HERO4_Black.md).
- GPL kernel source: GoPro open-source page (`gopro.com/support/open-source`), `linux.tar`; community fork [evilwombat/linux-hero4](https://github.com/evilwombat/linux-hero4).

## 2026-10-02 — gate 1: static analysis of v05.00.00 (verified by us)

`UPDATE.zip` sha256 `1d88f5dd7a4fdaa6c1664841996efaf68302b2fe0be89d29eb7bec514124ac7e`. Unpack with `scripts/fwunpack.py`.

### Image format: CRC only
- `camera_firmware.bin`: 224-byte global header (word 0 = zlib CRC32 of the rest), then 5 sections with 0x100-byte headers, each with its own CRC32. All verify with plain `zlib.crc32`. No signature block found.
- evilwombat's `h4-section-patch.c` replaces a section and fixes both CRCs — consistent with unsigned images. Whether the updater checks anything else is only proven by flashing.

| # | Content | Size |
|---|---------|------|
| 0 | DSP firmware | 6.8 MB |
| 1 | RTOS (BANZAI / "Hawaii-Phase5") | 14.9 MB |
| 2 | ROMFS | 11.4 MB |
| 3 | Linux 3.8.0 kernel, uncompressed, embedded config (`firmware/v05.00.00/kernel.config`) | 5.1 MB |
| 4 | UBI: Linux rootfs (`linux` volume) + empty `pref` | 29 MB |

### Kernel (differs from the Session)
- `CONFIG_USB_GADGET` **not set**, no USB host either. No gadget modules in the rootfs.
- But: `CONFIG_MODULES=y`, no `MODVERSIONS`, no `MODULE_SIG`, non-SMP, `PLAT_AMBARELLA_SUPPORT_UDC=y` and `SUPPORT_UPORT=y` (kernel has the `ambarella-udc` platform device and `/proc/ambarella/uport`).
- Leftover Ambarella scripts in the rootfs (`usr/local/share/script/usb_*.sh`) show the intended sequence: `echo device > /proc/ambarella/uport`, then `modprobe udc-core ambarella_udc libcomposite g_*`.
- `vendor/linux-hero4` (3.8.0) contains `ambarella_udc.c`, `ether.c`, `ncm.c`, `f_rndis.c`, `webcam.c`, and an `ambarella_a9_ambalink_usb_defconfig`. → gate 2 is: build `udc-core`, `ambarella_udc`, `libcomposite`, `g_ether`/`g_ncm` as modules against `kernel.config`.

### Streamer
- `usr/local/gopro/bin/gpStream` ("gpStreamA9") gets frames over `libambaipc` and sends UDP to the HTTP client's address (`REMOTE_ADDR`). If the Mac calls the HTTP API over a USB network interface, the stream should follow it there with no streamer changes.
- RTOS has `appc_stream_window_size_set`, `mft_set_sec_enc_size` and "override sec" window / bitrate / GOP / IDR knobs for the secondary encoder.

### Ways to run our code
- Linux init: `etc/init.d/S50service` mounts the SD card at `/tmp/fuse_d` and has a commented-out `telnetd`. Adding a hook here = repack UBIFS section 4 (flash).
- RTOS: strings `C:\cal.txt` / `calib_script_sd_callback` are present — the same calibration-script hook hypoxic used on the Session, possibly usable on **stock** firmware. `c:\autoexec.ash` is also referenced. Untested on the Black.
- RTOS USB: `t frw usb set-usb-class 2` = CDC-ACM (RTOS console over USB). No `t app usb linux` string on the Black, so the Session's USB hand-over command likely doesn't exist here; the RTOS may have to be told to drop USB some other way before Linux loads its driver. **This is now the main open risk.**
- Linux has `util_svc` (`AmbaRpcProg_Util_Exec1/2_Svc`): the RTOS can execute Linux commands over RPC.
- SD-card `gpauto` file is read by `gpNet` (settings only, not code).

## 2026-10-02 — gate 2: USB gadget modules build (passed, statically)

- Toolchain: `docker/Dockerfile` (Ubuntu 14.04 amd64 under colima + Rosetta, `arm-linux-gnueabi-gcc` 4.7.3 — the camera's kernel was built with CodeSourcery 4.7.3).
- `vendor/linux-hero4` + the camera's embedded config: `make oldconfig` adds a single symbol (`CONFIG_AMBPTB_PARTITION_HERO4`), so the tree matches the shipped kernel closely.
- `scripts/build_modules.sh` produces `usb-common`, `udc-core`, `ambarella_udc`, `libcomposite`, `g_ether` (CDC ECM), `g_ncm`, `g_serial` in `build/modules/`.
- vermagic `3.8.0 preempt mod_unload ARMv7` — identical to the stock `ambafs.ko`.
- All 158 undefined symbols across the modules are present by name in the camera's kernel image or provided by our own modules. (Name check only; struct layout mismatches would only show at runtime.)
- Load order on the camera: `usb-common udc-core ambarella_udc libcomposite g_ether`, after `echo device > /proc/ambarella/uport`.

## 2026-10-02 — code-execution route

- `cal.txt` on the Black: the parser's command list is calibration-only (`_calib_*`, `_nand_*`, `_buzz`, `_delay`, `_tcal` …). The Session's `_tapp` passthrough is absent, and several commands write calibration data to NAND. **Not using it.**
- `c:\autoexec.ash` is only referenced from a data pointer next to `csc_storage_internal_event_handler`; not traced further.
- Chosen route for gate 3: repack the Linux rootfs (section 4) with a hook in `etc/init.d/S50service` that runs a script from the SD card, fix CRCs, flash through the normal `UPDATE/` folder mechanism. RTOS, DSP, kernel and bootloaders stay byte-identical.
- UBI parameters of the stock image (from `ubireader_utils_info`): PEB 131072, LEB 126976 (`-e`), min I/O 2048, sub-page 2048, VID header offset 2048, LZO, fanout 8, r5 hash, image_seq 634535477; volumes `linux` (id 0, 281 reserved PEBs, 205 used) and `pref` (id 1, autoresize). Section 4 is 28,966,912 bytes = 221 PEBs.

## 2026-10-02 — gate 3: patched firmware built (not yet flashed)

- Hook point: the RTOS runs `/usr/local/share/script/sd_script.sh mount /tmp/fuse_d c: ambafs` on the Linux side at every SD mount (string in the RTOS image). The stock `tcp_script.sh` → `MISC/tcp_tuner.sh` hook is dead code (nothing calls it). Busybox has no `telnetd`; `tcpsvd`, `inetd`, `ftpd`, `httpd` exist.
- `scripts/patch_firmware.py` rewrites that script in place inside the UBIFS image: the file is one uncompressed 351-byte data node, so the new script is padded to 351 bytes and the node CRC fixed. No repack, inode untouched.
- Diff vs stock `camera_firmware.bin`: 334 bytes in 4 runs — global CRC, section 4 CRC, node CRC, script body. Re-extracted rootfs differs from stock only in `sd_script.sh`.
- New behaviour: after mounting, runs `sh <sdcard>/h4.sh &` if the file exists. `sdcard/h4.sh` (first version) only appends to `h4.log` and writes `h4_info.txt` on the card.
- Card layout: `UPDATE/{camera_firmware.bin,camera_loaders.bin,hd4_update.txt}`, `h4.sh`.

## 2026-10-02 — gate 3 passed: patched firmware flashed, hook runs

- Camera accepted the patched v05.00.00 image through the normal `UPDATE/` procedure (`MISC/version.txt`: `HD4.02.05.00.00`). CRC-only confirmed.
- `h4.sh` ran as root on every boot, about 12 s after power-on, i.e. after the hibernation resume (snapshot is taken ~4 s into the first boot; same PID 494 on later boots).
- Runtime facts: Linux 3.8.0, 41 MB RAM for Linux (33 MB free), only `ambafs` loaded, no network interface until Wi-Fi is switched on, `/proc/ambarella/uport` reports `port is DEVICE: controller is DEVICE`. No USB IRQ or iomem claim on the Linux side yet.
- Source check: `ambarella_udc0` is registered unconditionally for the ginkgo board; the driver's probe enables the PHY, resets the controller, requests `USBC_IRQ` and polls VBUS.
- Gate 4 test on the card: `sdcard/h4.sh` loads `usb-common udc-core ambarella_udc libcomposite g_ether` 15 s after the hook starts, brings `usb0` up as 169.254.77.1/16 and logs to `h4_usb.log` for 3 minutes.

## 2026-10-02 — gate 4 passed, gate 5 partly: video over USB works, but at 240p

- Run 1: all modules loaded, but GoPro's kernel never registers the `ambarella-udc` platform device (not in `/sys/bus/platform/devices`), so `g_ether` failed with ENODEV and the RTOS enumerated as MTP as usual.
- Fix: `src/h4_udc_dev/` — a module that registers the device with the S2 non-HAL PHY/reset callbacks copied from `plat-ambarella/generic/{udc,uport}.c`.
- Run 2: Mac enumerates `Linux 3.8.0 with ambarella_udc` / `Ethernet Gadget` (05c5:a4a1, CDC ECM), interface `en10`, host MAC 02:48:34:00:00:02. Camera is 169.254.77.1; the Mac self-assigns a link-local address. No freeze; the RTOS does not fight for the controller.
- HTTP API answers over USB (`curl --interface en10 http://169.254.77.1/gp/gpControl/status`). Ping is dropped by the stock iptables rules.
- The RTOS still notices the cable and puts the camera app in USB mode (status 43 = 7, USB icon on screen, SD card taken from Linux). `GET /gp/gpControl/command/mode?p=0` switches it back to video mode, after which `gpStream` delivers MPEG-TS over the USB link.
- Measured: H.264 Main, 29.97 fps, bitrate follows setting 62 well past the presets (8.4 Mbps measured at 8000000). No Wi-Fi involved.
- **Resolution is stuck at 432x240** whatever setting 64 says (tried 4 and 7, main mode 1080p60/1080p30/720p30/WVGA, with a mode switch, with and without `proto_v2`). The camera's own `/gp/gpControl` schema lists setting 64 options only up to 6 ("480 1:2 Subsample"); 7–9 (720p) from the public docs are not in this firmware's table, and even 480 does not take effect. Skipping gate 0 hid this.
- Leads for higher resolution: RTOS strings `appc_stream_window_size_set`, `mft_set_sec_enc_size`, "new override sec %ld x %ld" / "using override window %ldx%ld" — an override mechanism exists in the RTOS; how it is triggered is not yet known.
- `scripts/stream_test.py --camera 169.254.77.1 --iface en10` reproduces the capture (pins sockets to the interface with `IP_BOUND_IF`).

## 2026-10-02 — alternatives to the 240p preview (measured over the USB link)

- **Still photos as frames:** 12 MP single photo = 0.80 s capture+save, ~0.1 s media list, 0.70 s download of 4.5 MB (≈6 MB/s over the ECM link) → 1.64 s per frame, 0.6 fps. Even at a smaller photo size the capture step alone caps it near 1 fps; burst/continuous modes are short bursts, not sustained. Not viable for 10 fps.
- **Tailing the recording:** while the camera records 1080p30 (~26 Mbps), `GET :8080/videos/DCIM/100GOPRO/<next file>.MP4` with `Range: bytes=N-` returns HTTP 206 with the bytes written so far; the visible size grows in steps every ~2 s (3.7 → 10.0 → 17.8 → 27.3 MB over 9 s). `/gp/gpMediaList` returns nothing during recording, so the file name has to be predicted (last number + 1). The `moov` atom is only written at stop, so a live reader must parse `mdat` itself (AVCC length-prefixed NAL units, SPS/PPS taken from a finished clip of the same mode).

## 2026-10-03 — why the stream is 240p: RTOS video mode table (static analysis)

- The stream is the **secondary encoder** (the same one that writes `.LRV` files). Its size comes from a static table in the RTOS image: 229 pointers at `0x04054158` (RTOS loads at `0x03300000`), each to a 0x13c-byte mode entry. Relevant fields: `+0x60` sensor input size, `+0x70` secondary DSP buffer size, `+0xc4` main encoder size, `+0xcc` main bitrate, `+0xd4` frame rate (scale/ticks), `+0x100` secondary encoder size, `+0x108` secondary default bitrate.
- `drv_video_enc_setup` (`0x03a641a0`) takes the secondary size from `+0x100` unless an override is set.
- Setting 64 → `appc_stream_window_size_set` accepts 0–9 and stores it. `mft_set_sec_enc_size` (`0x03a64ec0`) maps the value to a size: 1–3 → 432/320 × 240, 4–6 → 848/640 × 480, **7–9 → 1280/960 × 720**, with the 3:4 and 1:2 variants scaling the width. It **rejects any height above the current mode's secondary height** ("height %ld exceeding max allowed height"), so on a 240 mode, 480 and 720 fail silently. That explains the gate 5 result.
- Secondary sizes in the stock table:
  - 432x240: most 4K/2.7K modes, 1080p48–60, and the idle-preview entries (main encoder itself at 432x240 or 320x240, ~0 bitrate). The idle preview doesn't use the recording entry: stock 1080p30 has an 848x480 secondary, but the idle stream was still 240.
  - **848x480 @ 2.5 Mbps**: 1080p30/25/24 (all FOVs), 720p60/50/30/25.
  - **1280x720 @ 2.5 Mbps**: entries 59/60 (1080p30/25 from 4000x3000 input). So the DSP already runs 1080p main + 720p secondary in stock firmware. Which user setting selects them is unknown: tested, it's not 1080 SuperView 30 and not 1080 Linear 30.
- Patch (`--sec720`, an option since removed from `patch_firmware.py`) sets `+0x70` and `+0x100` to 1280x720 in the 13 recording entries with 1080p main, ≤30 fps and an 848x480 secondary. Same change as stock entries 59/60, whose other fields differ only by sensor mode. Compared with the flashed hook image, it changes only the global CRC, the section 1 CRC and 52 size words; section 4 (hook) is identical. Side effect: `.LRV` files in those modes become 720p.
- Not covered: the idle (not recording) preview stays 240. Raising it would mean editing the preview-mode entries, whose sensor input is only 640x360–1280x720, and those are selected by code not yet traced. 1080p over the secondary encoder isn't attempted, since no stock mode runs a 1080p secondary. For a 1080p feed, tailing the recording (see 2026-10-02) is still the route.

## 2026-10-03 — gate 6: stream while recording (verified, stock RTOS)

All runs over USB with `stream_test.py --record --window 0 --bitrate 8000000`:

| Mode (2/3/4) | Stream | LRV of the same clip |
|---|---|---|
| 1080 SuperView 30 Wide (8/8/0) | 432x240 | 432x240, 7.7 Mbps |
| 1080 30 Wide (9/8/0) | **848x480**, 8.3 Mbps | 848x480 (earlier clips GOPR0572/0573 at 4 Mbps) |
| 1080 30 Linear (9/8/4) | **848x480** | — |
| 1080 30 Wide, window 7 (720) | 848x480 (override rejected: above the mode's max) | — |

- Confirmed: while recording, the stream is the secondary encoder (LRV bitrate follows setting 62) at the mode table's size, and setting 64 can't exceed it.
- **848x480 over USB works on the currently flashed firmware**: record in 1080 Wide/Linear 30, window 0.
- 1080 Wide 30 is one of the 848x480 entries `--sec720` changes, so after flashing that image the same command should give 1280x720. Untested.
- The shutter status (`status 8`) lags about 2 s after `shutter?p=0`.

## 2026-10-03 — streaming without filling the card / without a card

- **Looping sub-mode works (verified):** `command/sub_mode?mode=0&sub_mode=3`, 1080 Wide 30, shutter on → stream is 848x480 at 8.3 Mbps and the camera writes rotating chapter files (`G001xxxx.MP4`) instead of one growing clip. Interval is setting 6 (was 1). This bounds card usage, but the card still takes the full 1080p write load.
- **Hidden broadcast mode (partly verified):** `command/mode?p=3` is accepted (status 43 = 3). It is `APPC_MODE_BROADCAST` in the RTOS, with its own pipeline (`av_graph_create_broadcast`). Optional recording is `broadcast_record`, so by default broadcast doesn't write to the card. The two 1080p entries with a 500 kbps main encoder and an 848x480 / 2.5 Mbps secondary (mode table entries 62/63) look like its encoder config. In mode 3 the idle stream is still 432x240 (plus an extra data PID), and the shutter does nothing.
- Starting a broadcast is `appc_broadcast_start` (`0x03981610`, requires app mode 3). It is reached only from the RTOS "remote proxy" command table (REMOTE_WIFI / REMOTE_BACPAC, index at `0x04055f20`, handler `0x03b81dac` next to `broadcast res/fps/fov set` and `broadcast window size set`) and from the RTOS `t api` shell. The HTTP API has no URL for it. On Linux, `gpCamApi` forwards TLV messages from the `/to_camera` POSIX queue to the RTOS over IPC, and `gpsend ca XX` sends legacy two-letter commands. (Decoded below.)
- No card at all: the idle preview doesn't need a card, but our hook (`h4.sh` + modules) lives on the card. Without a card it would have to move into the flashed rootfs, which means a real UBIFS rebuild instead of the current one-node in-place patch.

## 2026-10-03 — broadcast start path decoded (static analysis; tool written, not yet run)

- **RTOS remote-proxy API.** `remote_proxy_task_entry` (`0x03a88128`) dispatches messages whose word 0 is `[key][module][api][status]` and whose halfword at +4 is the argument length. The module index table is at `0x04055f20`: 15 × `{ptr, count}`, each pointing at `{nargs, handler}` entries; the length must equal `nargs` unless `nargs` is -1. Handlers read their arguments from `data[5…]`.
- **Module 5 = broadcast.** api 3 = res/fps/fov set (3 args), 5 = record res/fps/fov set (3), **6 = start (1 arg: privacy 0–2)**, 7 = stop, then window size / GOP / IDR get/set pairs.
- **Linux → RTOS.** The TLV command tag is `0x41` with a two-letter code. `CAsetYY` (`0x03a7a1e8`) copies its payload `[key][module][api][len_hi][len_lo][args…]`, replaces `key` with the privileged channel's, and calls `remote_proxy_command_execute` (`0x03a88e4c`; logs `[BPGEN] key mod api length`). The RTOS shell equivalent is `t api remote <key> <module> <api id> <len 15:8> <len 7:0> <bytes>`.
- **Preconditions in `appc_broadcast_start` / `broadcast_start`:** app mode 3; privacy < 3; if a settings flag (`0x43b6d84+0x1c`) is 0, SD card status and free space are checked; and **broadcast status must be 1 ("ready")**. That status comes only from the CSI callback for `CSI_BROADCAST_STATUS`, which maps Linux values 1→1, 2→0, 3→2, 4→3, 5→5.
- **CSI** is a key-value store kept by `gpNet`; the RTOS gets change callbacks. `/usr/lib/libcsi.so` exports `int csi_set(uint8_t key, const void *val, uint32_t len)` and `int csi_get(uint8_t key, uint32_t max, uint32_t *len, void *val)`. Key numbers from `csi_key_list` match the RTOS callback: 6 `BROADCAST_PRIVATE`, 7 `NUMBER_OF_VIEWERS`, 8 `ERROR_CODE`, **9 `BROADCAST_STATUS`**, 0x34 `BROADCAST_ID`, 0x24 `JAKARTA_LIVE_BROADCAST`.
- **Tooling:**
  - `src/h4csi` is a get/set CLI linked against the camera's `libcsi.so`; build it with `scripts/build_tools.sh` in the `hero4-build` container.
  - `sdcard/h4/bcast.sh start|stop|status` runs `h4csi set 9 1`, then `gpsend ca "YY%00%05%06%00%01%0<privacy>"`.
  - `sdcard/h4.sh` now also starts `tcpsvd` on `169.254.77.1:2323` (root shell over the USB link only: `nc 169.254.77.1 2323`).
- **Open questions:**
  - Does `gpsend` decode `%xx` into raw bytes, including `%00`? Its usage line suggests it does.
  - Does `gpNet` NAK `YY` ("YY NAK instead of forwarding", "YY (or SY) not active")? `gpsend ca` goes to `/to_camera` → `gpCamApi`, which may bypass `gpNet`'s check.
  - Where does the broadcast pipeline's output go: to `gpStream`'s preview path or to `/var/www/live` segments?

### First run on the camera (2026-10-03)

- The USB root shell works once `h4.sh` accepts traffic on `usb0` (the stock iptables rules dropped port 2323) and starts `sh` instead of `sh -i` (no output with `-i`). `nc 169.254.77.1 2323`, no prompt.
- `h4csi` works: key 9 read 0 at boot, and `h4csi set 9 1` → rc=0, then reads 1. HTTP status 16 (`broadcast_bstatus`) is then 1, so the RTOS saw the change.
- `gpsend` decodes `%xx`, including `%00`, and `YY` reaches the RTOS: `YY%00%05%06%00%01%00 --> <ack>(%00%05%06%ff%00%00)`. The reply echoes module 5 / api 6; the status byte is **0xff** (failure).
- At the time of the second attempt: status 43 = 3 (broadcast mode), 8 = 0 (not busy), 16 = 1, 33 = 0 (SD OK), 55 = 1; privacy 0. `/var/www/live` empty, no broadcast output.
- `0x03b7e804` maps any nonzero handler return to 0xff, so the status byte carries no reason. The internal `broadcast_start` (`0x03a7a6c0`) fails only on the ready check, and on success sets the RTOS broadcast status to 2. Status 16 staying 1 means the refusal happens earlier, in `appc_broadcast_start`. A likely candidate: the pipeline-state fields (`0x43b6d84+0xc` / `+0x3c`); with `+0x3c` = 0 it logs "Shouldn't be here" and returns an error. Not confirmed.
- Retried with the mode-3 preview stream running (status 32 = 1): still 0xff. The stream stayed 432x240 for the full 40 s, so the start attempt had no visible effect.
- Earlier list of things to check: how `0x03b7e804` maps handler return codes to the status byte (whether 0xff is just "any error"); the busy check at `0x039bb258`; the state check at `0x039bad00`; and whether `remote_proxy_command_execute` waits for the proxy task (0xff could be a timeout).

## 2026-10-03 — 848x480 live preview with no recording (verified)

- How the mode-table index is chosen (static analysis):
  - The only store to the index byte `0x047acb31` is at `0x039d99dc`.
  - The idle preview comes from `av_set_pipeline_config` (`0x0395f810`) → `av_get_low_res_config`; recording from `video_record_start` (`0x03a5ad74`).
  - Both look up the Black video table at `0x04031c64`. Each (res, fps, fov) record holds byte +3 = record index, +4 = idle index (NTSC), +5 = idle (PAL), +6/+7 = "high" (NTSC/PAL); +6/+7 are used only when the display handle is 9.
  - Records: 1080p30 Wide `09 08 00 34 52 …` → record 52 / idle 82 (432x240); 1080 SuperView 30 `08 08 00 3b 3e …` → record 59 (1280x720 secondary) / idle 62 (848x480); 720p30 Wide `0c 08 00 45 52 …` → record 69 / idle 82.
- **Verified:** 1080 SuperView 30, video mode, not recording → stream **848x480** at 8.46 Mbps; no new file on the card, status 8 and 10 = 0.
- The earlier 432x240 SuperView results were stale pipelines. Video settings changed after the mode switch only take effect at the next rebuild (mode change). `stream_test.py` now applies `--mode`, then rebuilds via photo mode.
- Linux suspend: with `CSI_PREVENT_RTOS_DEEP_SLEEP` (key 0x43) = 1, Linux stayed up through a 5-minute poll in mode 3. It still dropped once right after a photo → video switch. The key resets to 0 at boot; the `h4.sh` that sets it is in the repo but not on the card yet.
- Next for 720p with no writes: entry 62 (idle SuperView 30, input 2000x1500, main 1920x1080 @ 500 kbps) → set `+0x70/+0x74` and `+0x100/+0x104` to 1280x720, mirroring record entry 59 (SuperView 30, 1280x720 secondary). Flash with the camera set to 1080 Wide 30, so boot uses the untouched entry 82, and switch to SuperView 30 only at runtime.

## 2026-10-03 — 720p live preview with no card writes (verified, patched firmware)

- Flashed the `patch_firmware.py --idle720` image (sha256 `32c9cca1…`, what `patch_firmware.py` produces now): the SD hook plus entry 62 secondary 848x480 → 1280x720 (`+0x70/+0x74`, `+0x100/+0x104`). Diff against the previous image: global CRC, section 1 CRC and those 4 words. The update went in through `UPDATE/` (folder removed afterwards, fresh Linux boot). The version string is unchanged (`HD4.02.05.00.00`).
- **Result:** 1080 SuperView 30, video mode, idle → stream **1280x720 @ 29.97 fps, 8.41 Mbps**; not recording, not encoding, no new file on the card (451 frames in 15 s). The picture is correct.
- Decode errors (lower-frame smears) occurred at the same rate as stock 848x480 captures (27/451 vs 28/361 and 39/445), so not the patch. Cause found below: the receiver, not the network.
- Boot safety: the patch only affects SuperView 30 idle. Other settings (e.g. 1080 Wide 30 → idle entry 82) are stock.
- Linux "drop-outs" explained: when the camera turns off, Linux is suspended and later resumed (uptime keeps growing, same PIDs); auto power-off was already Never. The resume restores the stock firewall, and the hook's lock made it skip re-setup. `h4.sh` now re-applies the firewall rules, key 0x43 and the shell listener on repeat runs (deployed with this flash; re-arm path not yet observed).
- Uploads over USB: `tcpsvd` on 2324 running a receiver that takes `dest\n` + data (renaming across folders on the card silently does nothing, so files are written directly to their destination). Tested with the update files (sha256 matched).

## 2026-10-03 — stream framing: the "packet loss" was the receiver

- `gpStream` (`proto_v2`) sends 1328-byte UDP datagrams. They carry a 12-byte GoPro header (not RTP), with the payload length (a multiple of 188) in bytes 10–11, then 2–7 TS packets, then padding.
- `stream_test.py` passed them to `ffmpeg` as raw `udp://` MPEG-TS, so the header and padding reached the TS demuxer as garbage. That produced the "Packet corrupt" warnings and the lower-frame smears in every capture so far.
- During a capture, the Mac showed no "dropped due to full socket buffers" and the camera's `usb0` showed no TX drops or errors.
- `stream_test.py` now receives the datagrams itself (8 MB `SO_RCVBUF`, bound before the stream restart) and writes only `d[12:12+len]`. 720p idle SuperView 30: 12453 datagrams, all well-formed; **445 frames, 0 decode errors, 0 warnings**; picture clean.
- Any live receiver (OBS, an ffmpeg pipe) needs the same unwrapping: strip the 12-byte header and the padding before the TS demuxer.

## 2026-10-05 — the preview needs HTTP activity, not just UDP keep-alives

- Symptom (OBS plugin): the stream stopped about every 66 s and came back only when the plugin restarted `gpStream`.
- `gpStream` (`/usr/local/gopro/bin/gpStream`, 7 KB of ARM code) streams to `REMOTE_ADDR`, the address of whoever sent the HTTP restart. Its keep-alive thread listens on 8554 (`.data`: port `0x216a`, timeout `0x61a8` = 25 000 ms) and accepts `_GPHD_:%u:%u:%d:%lf` when the third field is 2, from any source address. Without keep-alives it logs "Suspending due to no keepalive" and resumes on the next one. It logs through `/dev/kmsg` as `gpStreamA9`, so `dmesg` on the camera shows it.
- The stops are not that path. During them `gpStream` logs nothing, and its "received 500 packets" lines (frames arriving from the RTOS over IPC) stop. The RTOS stops feeding the preview.
- Measured over USB with keep-alives every 2.5 s: **no HTTP requests → data stopped about 50 s after the last request** (`[8430, 8455, 8317, 8423, 6515, 0, …]` datagrams per 10 s). **`GET /gp/gpControl/status` every 2 s → 150 s without a gap.** Once stopped, a `gpStream` restart alone sometimes did not bring it back; a photo → video mode rebuild did.
- The web viewer never hit this because its page polls `/api/status` (→ camera status) every couple of seconds. The OBS plugin now polls status every 3 s.
- The 12-byte gpStream datagram header (60 s recording, 50 136 datagrams, 1 792 video frames):
  - byte 0 bit `0x02` marks the **last datagram of a frame** (audio datagrams have it too). All 1 788 complete video frames ended on a flagged datagram; 210 of them on a full 7-packet one, so "short datagram = end of frame" misses about 12%.
  - byte 9 counts datagrams and resets at frame-group boundaries; it was continuous across every loss below.
- **The occasional "packet loss" is on the camera**, not USB: 2 video frames in 60 s had their tail missing (TS continuity jump of 10–13 packets) while the datagram counter stayed continuous, and their last datagram had no end-of-frame flag. `gpStream`'s "Data overflow, capping LTP payloadSize" message suggests it truncates oversized frames. Receivers should flush frames on the end-of-frame flag and treat a continuity jump as damage.

## 2026-10-05 — Mac sleep powers the camera off

- With the camera streaming over USB, the Mac went into idle sleep. On wake `en10` was `status: inactive`, the camera showed the charging screen (powered off), and the gadget was still enumerated. Pressing the camera's power button brought the link back after about 90 s; the OBS plugin then left USB mode and resumed on its own.
- The OBS plugin now holds a `PreventUserIdleSystemSleep` assertion while the stream runs.

## 2026-10-05 — UVC experiment: the camera works as a USB webcam on macOS (runtime only)

- **Result:** with `h4cam` (UVC + ECM gadget) and `h4uvc` loaded at runtime, macOS lists the camera as a regular webcam ("Video Control", `system_profiler SPCameraDataType`). AVFoundation exposes it as **`2vuy` 1280x720 @ 29.97**: Apple's UVC driver accepts the UVC 1.1 frame-based H.264 format and decodes it, so apps get ordinary decoded frames. OBS's stock "Video Capture Device" (`macos-avcapture`) showed a clean picture with no plugin. ~29.2 fps delivered; the camera's Linux core was ~50% idle (gpStream 29%, h4uvc 12%).
- **Why not isochronous:** the Ambarella UDC names its endpoints `epNin-bulk` / one `ep4in-int`, no `-iso`, so `usb_ep_autoconfig` can't place an isochronous endpoint. The UVC function streams over a bulk endpoint instead (UVC allows it; macOS handles it). The 3.8 `f_uvc` had a half-finished bulk path (`uvc_video_encode_bulk`); `src/h4cam/` completes it: alt 0 carries the bulk endpoint, it is enabled at `set_alt(0)`, one request = one 16 KB payload, transfers also end at the end of a frame.
- **Kernel pieces:** the stock kernel has no V4L2 (`CONFIG_MEDIA_SUPPORT` off); `videodev.ko` builds as a module against it and resolves entirely against the running kernel. `h4cam.ko` includes ECM (same MACs as `g_ether`, so `en10` and the USB shell keep working) and the patched UVC function. Build: `scripts/build_h4cam.sh`, `scripts/build_h4uvc.sh`.
- **h4uvc** answers probe/commit (only one format/frame/interval) and starts streaming on commit, since bulk mode has no alternate setting to signal it. It gets the preview locally: HTTP requests go out from 127.0.0.2, so gpStream streams to `REMOTE_ADDR` 127.0.0.2:8554, and a raw UDP socket picks the datagrams up. Binding 8554 itself makes every new gpStream exit with "bind error" (it binds 0.0.0.0:8554 for keep-alives without `SO_REUSEADDR`).
- **Gotchas found on the way:**
  - The UVC function keeps the whole composite gadget off the bus until the video device is opened, and closing it (h4uvc exiting) drops USB Ethernet too.
  - A power cycle (camera off/on) restores Linux from its hibernation image: our modules and `/tmp` are gone, `g_ether` is back. A safe way to undo any runtime experiment. Logs must go to the SD card (`/tmp/fuse_d/h4/uvc/`).
  - BusyBox `sh` on the camera has no `$((...))`.
  - After a resume, `/tmp/h4csi` is missing, so `h4.sh`'s re-arm path couldn't set key 0x43; fixed to copy it from the card.
  - A command-line process without camera permission gets a running AVCaptureSession that delivers no frames (`authorizationStatus` 0); test with an app that has permission (OBS).
- **Not done yet:** loading it at boot instead of `g_ether`, audio (would need a UAC function), stopping the stream when the host closes the camera, setting the bitrate and anti-suspend key from h4uvc. The OBS plugin and webcam mode can't run at the same time: the plugin's stream restarts redirect gpStream to the Mac.

### Webcam mode at power-on (verified)

- `h4.sh` now loads `videodev` + `h4cam` instead of `g_ether` (falls back to `g_ether` if they fail, or if `h4/ether_only` exists on the card) and starts `h4/uvc_run.sh`, which keeps `h4uvc -p` running and logs to `h4/uvc/h4uvc.log` on the card.
- Cold power-on: the camera left USB and came back as "GoPro HERO4" ~20 s later; h4uvc found the camera in USB mode, applied the 720p preset (22 s → 29 s after its start), opened the device, and OBS's capture source negotiated and streamed at 30 fps right away.
- h4cam now posts STREAMOFF when the host re-selects alt 0 (an app closing the camera) and resets the bulk endpoint; the UVC strings name the camera "GoPro HERO4".

## 2026-10-05 — the "quarter-second freezes" were gpStream starving

- In webcam mode the video lost 5–11 frame tails per 10 s (vs. ~0.4 with the OBS plugin path). The camera's datagram counter stays continuous across each loss, so data goes missing between the RTOS and gpStream: on the single Linux core (`CONFIG_SMP` off), with the UVC path busy too, gpStream reads the IPC queue too late.
- `renice -20` on gpStream: 0 losses in 5 of 6 ten-second windows, ~0.4 per 10 s overall, 30 fps steady. `SCHED_FIFO` 50 was no better. Camera CPU ~50% idle throughout.
- gpStream is a new process after each stream restart, so h4uvc re-applies nice -20 on every 3 s poll (`boost_gpstream`).

## 2026-10-05 — USB microphone (UAC1): blocked by the UDC

- The stream's audio (AAC-LC 48 kHz stereo) is fine; the web viewer and the OBS plugin decode it on the Mac. The goal here was a standard USB microphone device, which needs USB Audio Class streaming over an **isochronous** endpoint (no bulk option in UAC).
- `src/h4cam/f_h4mic.c`: UAC1 mic, 48 kHz stereo s16, PCM from `/dev/h4mic`, 47/48/49 frames per 1 ms packet for clock drift, `tone=1` test mode. It claims an IN endpoint as bulk (the UDC names none `-iso`) and enables it with the isochronous descriptor. Built into h4cam behind `mic=1` (default off).
- macOS lists it ("GoPro HERO4", 2 ch, 48 kHz). As soon as an app opens it: silence, then the UDC stops responding on every endpoint (USB Ethernet too). Linux keeps running (a logger kept writing to the card for 3 minutes); a power cycle recovers.
- The last USB event logged is the UDC reporting **alt 0** for the mic's streaming interface when the host selects it. The controller decodes SET_INTERFACE in hardware; the host then sends isochronous IN tokens to an endpoint that was never enabled, and the controller wedges.
- Tried in a patched UDC driver (`src/ambarella_udc/`, `scripts/build_udc.sh`): setting the isochronous frame number in DMA descriptors (`USB_DMA_FRM_NUM`, left 0 by the stock driver), not restarting a running isochronous DMA on each IN token, no `BUG()` on a missing descriptor, an interrupt-storm guard. None of it changed the outcome; the guard never tripped, so it is not an interrupt storm.
- Open: how the UDC20 core learns valid alternate settings (it may need every endpoint's config/interface/alt programmed into the UDC endpoint registers up front, not at `ep_enable`), and whether its DMA supports isochronous IN at all.

## 2026-10-05 — USB microphone works (plug and play, verified from a cold power-on)

The camera now boots as webcam + microphone + USB Ethernet. macOS lists "GoPro HERO4" as an audio input (2 ch, 48 kHz) next to the camera; OBS's stock audio input picked it up after a power cycle with nothing run by hand. A direct 13 s CoreAudio capture had no gaps; video stayed at 30 fps with no drops or losses while both streamed.

What it took, in the order found (patched UDC driver `src/ambarella_udc` → `ambarella_udc_h4.ko`, now loaded at boot with the stock driver as fallback):

- **Alternate settings:** the UDC answers SET_INTERFACE in hardware and only accepts an alt setting that some endpoint register (`USB_UDC_REG`, interface/alt fields) already names; otherwise it reports alt 0. That was the "reports alt 0 and wedges" above. f_h4mic registers its endpoint with `h4_udc_preset_iso()` and the driver pre-programs that register with (interface, alt 1) after every SET_CONFIGURATION / SET_INTERFACE. The register dump then showed alt 1.
- **Endpoint registers:** only ep1in–ep5in have their own. The driver maps epNin to register N and epNout to N + 5, so the mic's ep6in overwrote ep1out's (USB Ethernet's receive endpoint). With the mic on, the UVC VideoControl status endpoint (optional, never enabled) is left out to free an IN endpoint, and f_h4mic picks a free one of ep1–5.
- **Frame numbers:** an isochronous descriptor's frame field is in microframes (low 8 bits of the frame number + 3 bits of microframe). The plain frame number written there matched only briefly every 256 ms: audio arrived in ~25 ms bursts every 250 ms.
- **FIFO:** isochronous IN is double-buffered. A one-packet FIFO (49 words) sent only the first 96 of 192 bytes per packet (a ramp test pattern showed it). Two packets fixed it.
- **Rate:** arming the next packet on the next IN token sent it a frame late, so 500 packets/s. The driver now chains the next request from the DMA-completion interrupt: ~1000/s.
- **Chains don't work:** several 1 ms packets per request (one descriptor each, own frame number) came out mangled (completion errors, broken pattern), with the FIFO at 2 or 4 packets. One packet per request.
- **The webcam broke with the mic on:** the UVC bulk endpoint advertised wMaxPacketSize 1024, a leftover of the isochronous code in f_uvc (high-speed bulk is 512). It went unnoticed until isochronous traffic shared the bus, then every video request failed with a host error (`HE error in ep3in-bulk`, ~70/s, black picture). Each of those messages also cost ~3 ms with interrupts off, printed to the serial console: the single core was 94% busy. Bulk descriptors are now 512/64 and the messages are rate-limited. Also: the shared "dummy" descriptor the driver points an endpoint at after completion is skipped for isochronous endpoints.
- **Audio source:** h4uvc decodes the preview's AAC-LC (ADTS, one frame per PES) with faad2 (statically linked, ~8% CPU) and writes PCM to `/dev/h4mic`. Its HTTP status poll moved to a thread: in the receive loop each request held up the stream for ~200 ms. Audio still arrives with gaps of up to ~170 ms, so f_h4mic buffers 150 ms (trimmed to that when the host starts the stream) and skips or repeats one frame per packet to follow the camera's clock. Over 2 minutes with video: 0 underruns, 0 errors.
- CPU with both streaming: ~27% idle (8% before the host-error fix).

Notes:
- macOS toggles the mic's alt setting a dozen times in the first ~5 s, then settles.
- OBS recordings showed a few 13–70 ms silences per 20 s that a direct CoreAudio capture of the same mic didn't have: on the OBS side.
- After the camera re-enumerates, OBS's video capture source doesn't reopen it by itself (the mic source does); re-select the device. Seen with and without the mic.
- `sched_clock()` here is fine (ambarella-cs-timer, 1 ns), but the PMU cycle counter (`mrc p15, 0, r, c9, c13, 0`) was handier for per-branch interrupt profiling.

### After a cold boot: black picture, then a frozen mic (fixed)

- **Black picture / "noise" mic right after power-on:** Linux 3.8's gadget core calls `usb_gadget_connect()` right after binding, which overrides the UVC function's "stay off the bus until /dev/video0 is opened". The camera enumerated at ~22 s, h4uvc answered only at ~29 s (after its 6 s preset), and whatever the host opened in between got nothing. With no mic traffic, CoreAudio kept replaying its last ~220 ms of input (a steady, AAC-shaped noise floor). The camera itself was fine: h4uvc's decoded peak (now in its 10 s stats line) reached −6 dBFS while the Mac spoke, and the raw AAC decoded the same on the Mac. h4uvc now closes and reopens /dev/video0 once it's ready, which drops the gadget off the bus for 3 s (0.5 s was too short: macOS kept the stale audio device) and lets the host enumerate a camera that answers.
- **Mic freezing a few seconds in:** when the video started, the mic endpoint got "DMA failed" / BNA (a descriptor not marked ready), then an interrupt storm (status 0x440, IN token + DMA complete). The storm guard from the earlier investigation masked the endpoint for good, so the mic stayed dead. Fixes in the patched UDC driver: a re-armed isochronous descriptor is reset to "host ready"; the isochronous endpoint rests on its own dummy descriptor (not the one the other endpoints share); on BNA / host error the packet is re-armed instead of failed; a completion that arrives while idle re-arms too; a packet whose frame passed while the DMA waited is re-armed on the next IN token; the storm guard unmasks after 1 s.
- Verified from a cold power-on with nothing run by hand: video in Photo Booth at 30 fps (0 dropped, 0 losses), and the mic picked up the Mac's speech (−20 dB over a −48 dB floor) in 8 checks over 3 minutes. A storm still happened once at stream start (188 s) and recovered within the second.

## 2026-10-05 — latency, the "storms", and logging off the card

- **Latency (Chrome test page: screen flips + speaker clicks, measured on the GoPro's video and mic):** video ~194 ms glass to glass, mic ~254 ms (the page counts the speaker's ~35 ms output latency). Most of the video's is the camera's encoder.
- **Mic buffer, adaptive (f_h4mic):** starts at 60 ms, +30 ms on each underrun (max 250), −10 ms after every 20 s without one. The preview's AAC arrives with gaps of ~80–110 ms (up to ~170 ms under load), so a fixed buffer had to be 150 ms; it now sits at 60 ms with ~1 underrun per session. The audio's PTS run ~73 ms ahead of the video's at arrival, so the camera doesn't mux audio late: the ~60 ms audio lag is the buffer. No A/V alignment on the camera for now (within what people notice for audio-after-video).
- **SPS rewrite (h4uvc, `sps.c`):** the camera's SPS says max_num_reorder_frames = 1 with I/P slices only; h4uvc rewrites it to 0 (decoded frames bit-identical). No measurable change in Chrome/macOS; kept for decoders that do hold a reorder frame.
- **The ~16 s "interrupt storms" were the guard's fault:** it counted all endpoints together (trip at 20000/s) and masked whichever endpoint was pending, often the mic, during short bursts on the video endpoint; a trace of the mic's last 32 events before a trip showed it calm (one completion and re-arm per ms). Each trip was a second of silence and grew the mic buffer. Now per endpoint, at 50000/s: a real one on the mic is rare (~1 per 90 s) and inaudible.
- **Wedges from runtime module swaps:** swapping the UDC driver with a running isochronous stream wedged the controller twice (USB Ethernet gone, power cycle or battery pull). Test drivers go on the card and boot instead.
- **Logging off the card:** h4uvc and h4.sh log to RAM (`/tmp/h4uvc.d`, `/tmp/h4_usb.log`); only `h4.log` gets one line per boot. An empty `h4/debug` file on the card brings back card logging (previous run kept, kernel log every 5 s) for problems that need evidence across a power cycle. The card's filesystem won't replace a file that's open (a write or rename "succeeds" but the old file stays), so h4.sh runs uvc_run.sh from a RAM copy.

## 2026-10-05 — mic crackle: partly fixed, cause of the rest still open

Symptom: recordings over USB (QuickTime) had a short crackle every couple of seconds. Measured with a synthetic test tone (`touch /tmp/h4uvc.sine` on the camera: h4uvc sends a continuous 700 Hz sine instead of the camera's audio) recorded on the Mac, and a sample-exact discontinuity check (for a pure sine, x[n+1] − 2cos(ω)x[n] + x[n−1] ≈ 0).

Fixed:
- **Clock drift handled by dropping samples.** The camera's audio clock runs ~0.7% fast against USB's (the resampler settles at +0.6–0.7%). f_h4mic kept its buffer level by dropping a sample whenever the level left a narrow band; with the bursty input that happened ~170 times a second. Variable packets (47/49/50 frames) instead didn't help: macOS slipped samples itself. Now h4uvc resamples (linear interpolation, integer math: double math per sample on this softfp CPU starved gpStream and lost AAC frames), steered by the buffer level f_h4mic reports on read(), and every packet carries exactly 48 frames.
- **Endpoint declared asynchronous.** macOS then estimates a device clock from the packets; now synchronous.
- **Underruns at a 60 ms buffer.** The AAC arrives with gaps of 80–140 ms; minimum now 90 ms (~30 ms more latency).

Still open (fixed 2026-10-06, see below: the serial console): clusters of ~140 ms every ~1–3 s, in which the Mac's recording jumps by tens of samples about every 10 ms (its 512-frame I/O buffer: it re-times the stream). Seen with AVFoundation (ffmpeg, QuickTime) and with an AudioQueue recorder alike, so it's in the stream. Not clipping (glitches in silence and at −12 dBFS), not the decoder (its output is clean), not h4uvc's keep-alive (2.5 s) or status poll (3 s): making them 4–5× rarer made it no better. Best lead: the camera still misses ~1.6 USB frames a second (UDC driver counters, printed when the mic stream stops: the next packet is normally armed more than a frame ahead, but now and then the CPU is held up for over 1 ms). Unproven that these line up with the clusters. Ideas: correlate missed frames with glitch times; find what blocks interrupts (the RTOS IPC?); queue more than one isochronous packet in hardware (descriptor chains came out mangled earlier); or carry the audio over USB Ethernet to a virtual mic on the Mac instead.

### 2026-10-05 (later) — what the glitch is, sample-exact

- **In a QuickTime recording of the real mic** (AAC, 44.1 kHz) each click is a short rectangular pulse, not a gap or a repeat. Clicks within a cluster are spaced by whole multiples of 10.67 ms (512 frames at 48 kHz). Pulse widths are 8, 24, 40 or 56 frames at 48 kHz.
- **With `tone=2`** (left: ramp within the packet; right: packet counter mod 32), recorded losslessly with `ffmpeg -f avfoundation`, every sample maps to a stream position. The camera's data arrives bit-exact. In a cluster, at a 512-frame boundary of the Mac's buffer, the position jumps **+288 frames (exactly 6 packets) ahead** for 16, 32 or 48 frames, then jumps back by −288. So each click is 16–48 frames copied in from 6 ms later, and the timing comes from the host. 73 clusters in 53 s. (Wrong: the counter was mod 32, and −250 packets ≡ +6. See below: it is audio from 250 ms earlier.)
- The same recording also had +512 jumps: whole 512-frame buffers that ffmpeg's AVFoundation capture dropped (6.5 s of a 60 s capture). That's a capture artifact; ignore it.
- The driver counted 100 late arms (frames that went out empty) and 0 re-arms over that 60 s stream: a similar rate to the clusters (1.7/s vs 1.4/s), but not yet shown to coincide.
- **Next:** `tone=3` (new) writes a full packet counter in the first 24 frames of each packet's right channel and the USB microframe counter (11 bits, read when the packet is filled, i.e. when the packet 4 before it completed) in the last 24. A lossless recording then shows missed frames (a jump in microframe − 8 × packet) and glitches on one timeline. `tone` can be written at runtime (`/sys/module/h4cam/parameters/tone`), but tone=3 needs the new h4cam.ko.

## 2026-10-06 — mic crackle fixed: the serial console

- **`tone=3`, 53 s lossless:** the inserted frames are from **249 packets (≈250 ms) earlier**, not 6 packets later (the `tone=2` counter was mod 32). macOS reads its input ring buffer ahead of what has been written and gets the previous lap. 1119 such inserts in 74 clusters.
- **The camera's misses** (microframe jumps between consecutive packets) are 24–36 microframes, i.e. the CPU held up **3–4.5 ms**, 98 of them, **every ~0.6 s**. Each one leaves the host's ring 3–4 packets short. A third of the clusters start within ±5 ms of a miss; the rest within ~280 ms after one.
- **Cause:** GoPro's gpStream driver prints `[E]gpStreamA9: received 500 packets` (KERN_ERR) every ~0.6 s, and the console (`console=ttyS2`, 115200 baud, loglevel 6) prints it synchronously: ~50 characters ≈ 4.3 ms with interrupts off.
- **Fix:** `echo 1 > /proc/sys/kernel/printk` (messages still go to dmesg). Same test: **98 misses → 1, 74 clusters → 1**. Real path (h4uvc's 700 Hz sine through the buffer and USB): 1 discontinuity in 30 s and one cluster in 60 s.
- **Where it's set:** not in `h4.sh`: GoPro's `/etc/init.d/S65gopro2` runs `dmesg -n6` after the card hook, so after a boot the level was back at 6. h4uvc now checks it on every 3 s poll (`quiet_console`) and logs `console loglevel 6 -> 1`. Verified after a power cycle: 0 glitches and 1 miss in 45 s of `tone=3`.
- **Capture artifact, confirmed:** ffmpeg's AVFoundation input drops whole 512-frame buffers (~12/s here, `-thread_queue_size` doesn't help). In a 30 s capture the missing samples were exactly 319 × 512, one per 512-aligned discontinuity. Ignore 512-aligned events.
- **faad2 2.11.1 → 2.11.4** (the project moved to github.com/FreewareAdvancedAudio/faad2; 2.11.2–2.11.4 are mostly robustness/security fixes, many in SBR/PS/RVLC/LTP which the preview's AAC-LC doesn't use). Same sources and defines; on the camera: 0 decode errors, h4uvc's CPU about the same (~13% vs ~16% in 10 s samples).
