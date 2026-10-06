// SPDX-License-Identifier: GPL-2.0
/*
 * h4uvc: camera-side server for the h4cam UVC gadget (HERO4 Black, Linux 3.8).
 *
 * Takes gpStream's preview locally (MPEG-TS in UDP datagrams, see
 * docs/findings.md), keeps it alive, demuxes the H.264 stream and queues one
 * access unit per V4L2 output buffer on the UVC gadget's video device. Answers
 * the host's UVC probe/commit requests and starts streaming on commit (bulk
 * mode has no alternate setting to signal it).
 *
 *   h4uvc [/dev/video0]
 *
 * gpStream sends to the address that issued the HTTP restart (REMOTE_ADDR) on
 * port 8554, where it also listens for keep-alives on 0.0.0.0:8554. So the HTTP
 * requests go out from 127.0.0.2, and the stream to 127.0.0.2:8554 is picked up
 * with a raw UDP socket: binding 8554 ourselves would make every new gpStream
 * fail with "bind error".
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/video.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "sps.h"
#include "ts-demux.h"
#include <neaacdec.h>
#include <math.h>

/* --- UVC gadget userspace API (drivers/usb/gadget/uvc.h) ------------------ */
#define UVC_EVENT_CONNECT (V4L2_EVENT_PRIVATE_START + 0)
#define UVC_EVENT_DISCONNECT (V4L2_EVENT_PRIVATE_START + 1)
#define UVC_EVENT_STREAMON (V4L2_EVENT_PRIVATE_START + 2)
#define UVC_EVENT_STREAMOFF (V4L2_EVENT_PRIVATE_START + 3)
#define UVC_EVENT_SETUP (V4L2_EVENT_PRIVATE_START + 4)
#define UVC_EVENT_DATA (V4L2_EVENT_PRIVATE_START + 5)

struct uvc_request_data {
	int32_t length;
	uint8_t data[60];
};

struct uvc_event {
	union {
		enum usb_device_speed speed;
		struct usb_ctrlrequest req;
		struct uvc_request_data data;
	};
};

#define UVCIOC_SEND_RESPONSE _IOW('U', 1, struct uvc_request_data)

/* --- configuration ---------------------------------------------------------- */
#define CAMERA "127.0.0.1"
#define LOCAL "127.0.0.2"
#define STREAM_PORT 8554
#define KEEP_ALIVE "_GPHD_:0:0:2:0.000000\n"
#define WIDTH 1280
#define HEIGHT 720
#define FRAME_INTERVAL 333667 /* 100 ns units, 29.97 fps */
#define BULK_PAYLOAD 16384    /* h4cam bulk_payload */
#define MAX_FRAME (512 * 1024)
#define NBUFS 4
#define HDR_END_OF_FRAME 0x02

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* stderr may be a file on the SD card: flush it to the card on every line, so
 * the log survives if Linux hangs (a power cycle restores /tmp). */
static void say(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	fprintf(stderr, "[%9.3f] ", now_s());
	vfprintf(stderr, fmt, a);
	fputc('\n', stderr);
	va_end(a);
	fflush(stderr);
	fsync(2);
}

/* --- camera HTTP API ------------------------------------------------------- */

/* GET /gp/gpControl<path> from LOCAL; returns the HTTP status, 0 on failure. */
static int api(const char *path, char *body, size_t body_size)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0), status = 0;
	struct sockaddr_in src = {.sin_family = AF_INET}, dst = {.sin_family = AF_INET, .sin_port = htons(80)};
	struct timeval tv = {.tv_sec = 8};
	inet_pton(AF_INET, LOCAL, &src.sin_addr);
	inet_pton(AF_INET, CAMERA, &dst.sin_addr);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (bind(fd, (struct sockaddr *)&src, sizeof(src)) < 0 || connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0)
		goto out;
	char req[256], buf[8192];
	int n = snprintf(req, sizeof(req), "GET /gp/gpControl%s HTTP/1.0\r\nHost: " CAMERA "\r\n\r\n", path);
	if (send(fd, req, n, 0) != n)
		goto out;
	size_t got = 0;
	ssize_t r;
	while (got < sizeof(buf) - 1 && (r = recv(fd, buf + got, sizeof(buf) - 1 - got, 0)) > 0)
		got += r;
	buf[got] = 0;
	if (sscanf(buf, "HTTP/%*s %d", &status) != 1)
		status = 0;
	if (body) {
		/* The camera's server ends its last header with a bare \n, then \r\n. */
		char *b = buf;
		while ((b = strchr(b, '\n')) && b[1] != '\n' && !(b[1] == '\r' && b[2] == '\n'))
			b++;
		snprintf(body, body_size, "%s", b ? b + (b[1] == '\n' ? 2 : 3) : "");
	}
out:
	close(fd);
	return status;
}

/* A field of the "status" object, or of "settings" with settings != 0. */
static int json_field(const char *json, const char *key, int settings)
{
	char k[16];
	const char *start = settings ? strstr(json, "\"settings\"") : json;
	if (!start)
		return -1;
	snprintf(k, sizeof(k), "\"%s\":", key);
	const char *p = strstr(start, k);
	return p ? atoi(p + strlen(k)) : -1;
}

/* 1280x720 idle preview: 1080 SuperView 30 on the --idle720 firmware, then a
 * photo → video rebuild (the preview only picks up settings on a rebuild). */
static void apply_preset_720(void)
{
	say("preset 1280x720 (1080 SuperView 30)");
	api("/setting/2/8", NULL, 0);
	api("/setting/3/8", NULL, 0);
	api("/setting/4/0", NULL, 0);
	api("/command/mode?p=1", NULL, 0);
	sleep(3);
	api("/command/mode?p=0", NULL, 0);
	sleep(3);
	api("/setting/64/0", NULL, 0);
	api("/setting/62/8000000", NULL, 0);
}

/* gpStream reads the preview from the RTOS over IPC. On the single Linux core,
 * with the UVC path busy too, it falls behind and the RTOS drops the tail of
 * frames (5-11 losses per 10 s). At nice -20 that drops to ~0.4 per 10 s, the
 * same as without webcam mode. It is a new process after every restart, so
 * check on every poll. */
static void boost_gpstream(void)
{
	DIR *proc = opendir("/proc");
	struct dirent *e;
	if (!proc)
		return;
	while ((e = readdir(proc))) {
		char path[64], comm[32] = "";
		int pid = atoi(e->d_name);
		if (pid <= 0)
			continue;
		snprintf(path, sizeof(path), "/proc/%d/comm", pid);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(comm, sizeof(comm), f) && !strncmp(comm, "gpStream", 8) &&
		    getpriority(PRIO_PROCESS, pid) != -20) {
			setpriority(PRIO_PROCESS, pid, -20);
			say("gpStream %d: nice -20", pid);
		}
		fclose(f);
	}
	closedir(proc);
}

static void restart_stream(void)
{
	int s = api("/execute?p1=gpStream&a1=proto_v2&c1=restart", NULL, 0);
	say("restart stream -> %d", s);
}

/* --- UVC ------------------------------------------------------------------- */
struct uvc {
	int fd;
	int streaming;
	void *mem[NBUFS];
	size_t len[NBUFS];
	int queued[NBUFS];
	struct uvc_streaming_control probe, commit;
	int control; /* 1 = probe, 2 = commit: the SET_CUR whose data comes next */
	int need_idr;
	unsigned long frames, dropped;
};

static void fill_control(struct uvc_streaming_control *c)
{
	memset(c, 0, sizeof(*c));
	c->bmHint = 1;
	c->bFormatIndex = 1;
	c->bFrameIndex = 1;
	c->dwFrameInterval = FRAME_INTERVAL;
	c->dwMaxVideoFrameSize = MAX_FRAME;
	c->dwMaxPayloadTransferSize = BULK_PAYLOAD;
	c->dwClockFrequency = 48000000;
	c->bmFramingInfo = 3; /* FID required, EOF present */
	c->bPreferedVersion = 1;
	c->bMinVersion = 1;
	c->bMaxVersion = 1;
}

static void stream_off(struct uvc *u)
{
	if (!u->streaming)
		return;
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ioctl(u->fd, VIDIOC_STREAMOFF, &type);
	for (int i = 0; i < NBUFS; i++)
		if (u->mem[i])
			munmap(u->mem[i], u->len[i]);
	memset(u->mem, 0, sizeof(u->mem));
	struct v4l2_requestbuffers rb = {.count = 0, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP};
	ioctl(u->fd, VIDIOC_REQBUFS, &rb);
	u->streaming = 0;
	say("stream off");
}

static int stream_on(struct uvc *u)
{
	stream_off(u);
	struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
	fmt.fmt.pix.width = WIDTH;
	fmt.fmt.pix.height = HEIGHT;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.sizeimage = MAX_FRAME;
	if (ioctl(u->fd, VIDIOC_S_FMT, &fmt) < 0) {
		say("S_FMT: %s", strerror(errno));
		return -1;
	}
	struct v4l2_requestbuffers rb = {.count = NBUFS, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP};
	if (ioctl(u->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < NBUFS) {
		say("REQBUFS: %s (%u)", strerror(errno), rb.count);
		return -1;
	}
	for (int i = 0; i < NBUFS; i++) {
		struct v4l2_buffer b = {.index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP};
		if (ioctl(u->fd, VIDIOC_QUERYBUF, &b) < 0) {
			say("QUERYBUF: %s", strerror(errno));
			return -1;
		}
		u->len[i] = b.length;
		u->mem[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, u->fd, b.m.offset);
		if (u->mem[i] == MAP_FAILED) {
			u->mem[i] = NULL;
			say("mmap: %s", strerror(errno));
			return -1;
		}
		u->queued[i] = 0;
	}
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(u->fd, VIDIOC_STREAMON, &type) < 0) {
		say("STREAMON: %s", strerror(errno));
		return -1;
	}
	u->streaming = 1;
	u->need_idr = 1;
	u->frames = u->dropped = 0;
	say("stream on: H.264 %dx%d, %d buffers of %zu bytes", WIDTH, HEIGHT, NBUFS, u->len[0]);
	return 0;
}

static void respond(struct uvc *u, const void *data, int len)
{
	/* data == NULL with len > 0 accepts a SET_CUR data phase. */
	struct uvc_request_data r = {.length = len};
	if (data && len > 0)
		memcpy(r.data, data, len > 60 ? 60 : len);
	if (ioctl(u->fd, UVCIOC_SEND_RESPONSE, &r) < 0)
		say("SEND_RESPONSE: %s", strerror(errno));
}

static void handle_setup(struct uvc *u, const struct usb_ctrlrequest *req)
{
	int cs = req->wValue >> 8, entity = req->wIndex >> 8, intf = req->wIndex & 0xff;
	int len = req->wLength;
	struct uvc_streaming_control c;

	say("setup %02x %02x value %04x index %04x length %d", req->bRequestType, req->bRequest, req->wValue,
	    req->wIndex, len);

	/* Probe/commit live on the streaming interface (the second UVC one; ECM
	 * takes the first two interface numbers). Anything else: stall. */
	if ((req->bRequestType & USB_TYPE_MASK) != USB_TYPE_CLASS || entity != 0 || intf != 3 ||
	    (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL)) {
		respond(u, NULL, -1);
		return;
	}
	int size = len < (int)sizeof(c) ? len : (int)sizeof(c);
	switch (req->bRequest) {
	case UVC_SET_CUR:
		u->control = cs;
		respond(u, NULL, len); /* accept the data phase; it arrives as UVC_EVENT_DATA */
		break;
	case UVC_GET_CUR:
		respond(u, cs == UVC_VS_PROBE_CONTROL ? &u->probe : &u->commit, size);
		break;
	case UVC_GET_MIN:
	case UVC_GET_MAX:
	case UVC_GET_DEF:
		fill_control(&c);
		respond(u, &c, size);
		break;
	case UVC_GET_RES:
		memset(&c, 0, sizeof(c));
		respond(u, &c, size);
		break;
	case UVC_GET_LEN: {
		uint8_t l[2] = {sizeof(c) & 0xff, sizeof(c) >> 8};
		respond(u, l, 2);
		break;
	}
	case UVC_GET_INFO: {
		uint8_t info = 3; /* GET and SET supported */
		respond(u, &info, 1);
		break;
	}
	default:
		respond(u, NULL, -1);
	}
}

static void handle_data(struct uvc *u, const struct uvc_request_data *d)
{
	struct uvc_streaming_control c;
	fill_control(&c);
	/* Only one format, frame and interval exist; take what the host sent for
	 * the rest but keep our limits. */
	memcpy(&c, d->data, d->length < (int)sizeof(c) ? d->length : (int)sizeof(c));
	c.bFormatIndex = 1;
	c.bFrameIndex = 1;
	c.dwFrameInterval = FRAME_INTERVAL;
	c.dwMaxVideoFrameSize = MAX_FRAME;
	c.dwMaxPayloadTransferSize = BULK_PAYLOAD;
	if (u->control == UVC_VS_PROBE_CONTROL) {
		u->probe = c;
		say("probe set");
	} else if (u->control == UVC_VS_COMMIT_CONTROL) {
		u->commit = c;
		say("commit: starting stream");
		stream_on(u);
	}
	u->control = 0;
}

static void handle_event(struct uvc *u)
{
	struct v4l2_event ev;
	if (ioctl(u->fd, VIDIOC_DQEVENT, &ev) < 0)
		return;
	struct uvc_event *e = (void *)&ev.u.data;
	switch (ev.type) {
	case UVC_EVENT_CONNECT:
		say("host connected (speed %d)", e->speed);
		break;
	case UVC_EVENT_DISCONNECT:
		say("host disconnected");
		stream_off(u);
		break;
	case UVC_EVENT_SETUP:
		handle_setup(u, &e->req);
		break;
	case UVC_EVENT_DATA:
		handle_data(u, &e->data);
		break;
	case UVC_EVENT_STREAMOFF:
		/* h4cam posts this when the host re-selects alt 0: the app closed the camera. */
		say("host stopped streaming");
		stream_off(u);
		break;
	case UVC_EVENT_STREAMON:
		break;
	}
}

/* Give finished buffers back to the free pool. */
static void reclaim(struct uvc *u)
{
	for (;;) {
		struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP};
		if (ioctl(u->fd, VIDIOC_DQBUF, &b) < 0)
			return;
		u->queued[b.index] = 0;
	}
}

/* Copy an Annex B access unit, rewriting its SPS for low delay (sps.c: the
 * camera's says one reorder frame, so decoders held every frame back by one).
 * Returns the bytes written, 0 if it doesn't fit. */
static size_t copy_au(uint8_t *dst, size_t cap, const uint8_t *es, size_t len)
{
	size_t o = 0, i = 0;
	while (i < len) {
		/* NAL starts after a 00 00 01; ends at the next start code. */
		size_t s = i;
		while (s + 2 < len && !(es[s] == 0 && es[s + 1] == 0 && es[s + 2] == 1))
			s++;
		if (s + 2 >= len)
			s = len;
		size_t h = s < len ? s + 3 : len, e = h;
		while (e + 2 < len && !(es[e] == 0 && es[e + 1] == 0 && es[e + 2] == 1))
			e++;
		if (e + 2 >= len)
			e = len;
		size_t end = e;
		while (end > h && end < len && es[end - 1] == 0)	/* zero byte of the next 4-byte start code */
			end--;
		uint8_t sps[300];
		size_t n = h < end && (es[h] & 0x1f) == 7 ? sps_low_delay(es + h, end - h, sps, sizeof(sps)) : 0;
		if (n) {
			if (o + (h - i) + n + (e - end) > cap)
				return 0;
			memcpy(dst + o, es + i, h - i);
			o += h - i;
			memcpy(dst + o, sps, n);
			o += n;
			memcpy(dst + o, es + end, e - end);
			o += e - end;
		} else {
			if (o + (e - i) > cap)
				return 0;
			memcpy(dst + o, es + i, e - i);
			o += e - i;
		}
		i = e;
	}
	return o;
}

static void queue_frame(struct uvc *u, const uint8_t *es, size_t len, int damaged)
{
	if (!u->streaming)
		return;
	reclaim(u);

	/* A frame is an IDR if it holds a NAL unit of type 5. */
	int idr = 0;
	for (size_t i = 0; i + 3 < len && !idr; i++)
		if (!es[i] && !es[i + 1] && es[i + 2] == 1 && (es[i + 3] & 0x1f) == 5)
			idr = 1;

	/* After a loss, a full queue or a fresh start, skip to the next IDR so the
	 * host's decoder never sees a frame with a missing reference. */
	if (damaged)
		u->need_idr = 1;
	if (u->need_idr && (!idr || damaged)) {
		u->dropped++;
		return;
	}
	int slot = -1;
	for (int i = 0; i < NBUFS; i++)
		if (!u->queued[i] && u->mem[i]) {
			slot = i;
			break;
		}
	if (slot < 0 || !(len = copy_au(u->mem[slot], u->len[slot], es, len))) {
		u->need_idr = 1;
		u->dropped++;
		return;
	}
	struct v4l2_buffer b = {.index = slot, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP,
				.bytesused = len};
	if (ioctl(u->fd, VIDIOC_QBUF, &b) < 0) {
		say("QBUF: %s", strerror(errno));
		u->need_idr = 1;
		return;
	}
	u->queued[slot] = 1;
	u->need_idr = 0;
	u->frames++;
}

/* --- main loop ------------------------------------------------------------- */
static struct uvc g_uvc;

/* --- microphone ------------------------------------------------------------
 * The preview carries AAC-LC (ADTS) audio. With h4cam's microphone (mic=1), decode
 * it and write 48 kHz stereo s16 to /dev/h4mic; the gadget paces it out to the
 * host. Writes never block: the driver keeps the newest ~340 ms. */
static struct {
	int fd;
	NeAACDecHandle dec;
	unsigned long frames, errors, pes;
	/* Resampler: the camera's audio clock runs ~0.6% fast against USB's. Keep
	 * f_h4mic's buffer at its target by resampling (linear interpolation),
	 * ratio = input frames per output frame, steered by the buffer level. */
	double ratio, integ, next_ctl;
	int64_t pos;		/* 32.32 fixed point, relative to the next block's in[0] */
	int16_t prev[2];
	int16_t rs[2400 * 2];
	int peak;			/* largest decoded sample since the last stats line */
	double last_pes, max_gap;
	int16_t stereo[2048 * 2];
} mic = {.fd = -1, .ratio = 1.007, .integ = 0.007};	/* measured ~0.7% */

static void mic_control(void)
{
	uint32_t st[3];
	double t = now_s();
	if (t < mic.next_ctl)
		return;
	double dt = mic.next_ctl ? t - mic.next_ctl + 0.1 : 0.1;
	mic.next_ctl = t + 0.1;
	if (read(mic.fd, st, sizeof(st)) != sizeof(st) || !st[2])
		return;		/* old driver, or the host isn't listening: hold */
	double err = (double)st[0] - (double)st[1];	/* frames above target */
	/* The level is a 1 s average (lag), and the buffer absorbs the AAC
	 * bursts. 10 ms off target -> 0.1% now; the integral learns the clock
	 * offset over tens of seconds. The resampler is the only correction:
	 * every USB packet carries exactly 48 frames. */
	mic.integ += err * dt * 2e-7;
	if (mic.integ > 0.015)
		mic.integ = 0.015;
	if (mic.integ < -0.015)
		mic.integ = -0.015;
	mic.ratio = 1.0 + mic.integ + err * 2e-6;
	if (mic.ratio > 1.02)
		mic.ratio = 1.02;
	if (mic.ratio < 0.98)
		mic.ratio = 0.98;
}

/* Resample n stereo frames; returns the output frame count (in mic.rs).
 * Integer only: this CPU has no fast floating point for userspace (softfp
 * ABI), and per-sample double math stole time from gpStream. */
static size_t mic_resample(const int16_t *in, size_t n)
{
	size_t o = 0;
	int64_t p = mic.pos, step = (int64_t)(mic.ratio * 4294967296.0);
	while (p <= ((int64_t)(n - 1) << 32) && o < sizeof(mic.rs) / 4) {
		int64_t j = p >> 32;	/* -1 means mic.prev */
		int32_t f = (int32_t)((p & 0xffffffff) >> 17);	/* 0..32767: (b - a) * f fits */
		for (int c = 0; c < 2; c++) {
			int32_t a = j < 0 ? mic.prev[c] : in[2 * j + c];
			int32_t b = in[2 * (j + 1) + c];
			mic.rs[2 * o + c] = (int16_t)(a + (((b - a) * f) >> 15));
		}
		o++;
		p += step;
	}
	mic.pos = p - ((int64_t)n << 32);
	mic.prev[0] = in[2 * (n - 1)];
	mic.prev[1] = in[2 * (n - 1) + 1];
	return o;
}

static void mic_audio(const uint8_t *es, size_t len)
{
	size_t off = 0;
	double t = now_s();
	if (mic.fd < 0)
		return;
	if (mic.last_pes && t - mic.last_pes > mic.max_gap)
		mic.max_gap = t - mic.last_pes;
	mic.last_pes = t;
	mic.pes++;
	if (!mic.dec) {
		unsigned long rate;
		unsigned char ch;
		mic.dec = NeAACDecOpen();
		NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(mic.dec);
		c->outputFormat = FAAD_FMT_16BIT;
		c->defSampleRate = 48000;
		NeAACDecSetConfiguration(mic.dec, c);
		if (NeAACDecInit(mic.dec, (unsigned char *)es, len, &rate, &ch) < 0) {
			NeAACDecClose(mic.dec);
			mic.dec = NULL;
			return;
		}
		say("mic: AAC %lu Hz, %u ch", rate, ch);
		if (rate != 48000)
			say("mic: not 48 kHz, the pitch will be off");
	}
	while (off + 7 < len) {
		NeAACDecFrameInfo fi;
		int16_t *pcm = NeAACDecDecode(mic.dec, &fi, (unsigned char *)es + off, len - off);
		if (fi.error || !fi.bytesconsumed) {
			mic.errors++;
			break;
		}
		off += fi.bytesconsumed;
		if (!pcm || !fi.samples)
			continue;
		size_t frames = fi.samples / fi.channels, bytes = frames * 4;
		const void *out = pcm;
		if (fi.channels == 1 && frames <= 2048) {
			for (size_t i = 0; i < frames; i++)
				mic.stereo[2 * i] = mic.stereo[2 * i + 1] = pcm[i];
			out = mic.stereo;
		} else if (fi.channels != 2)
			continue;
		/* Test: while /tmp/h4uvc.sine exists, send a continuous 700 Hz sine
		 * instead of the camera's audio (isolates the decoder from the
		 * ring/USB path when hunting glitches). */
		static unsigned long sine_n;
		static int sine_on, sine_check;
		static int16_t sine_tab[48000];	/* exactly 700 periods */
		if (++sine_check >= 47) {
			sine_check = 0;
			sine_on = access("/tmp/h4uvc.sine", F_OK) == 0;
		}
		if (sine_on && frames <= 2048) {
			if (!sine_tab[1])
				for (int i = 0; i < 48000; i++)
					sine_tab[i] = (int16_t)(8000 * sin(2 * M_PI * 700 * i / 48000.0));
			for (size_t i = 0; i < frames; i++, sine_n++)
				mic.stereo[2 * i] = mic.stereo[2 * i + 1] = sine_tab[sine_n % 48000];
			out = mic.stereo;
		}
		mic_control();
		if (frames) {
			frames = mic_resample((const int16_t *)out, frames);
			out = mic.rs;
			bytes = frames * 4;
		}
		for (size_t i = 0; i < frames * 2; i++) {
			int v = ((const int16_t *)out)[i];
			if (v < 0)
				v = -v;
			if (v > mic.peak)
				mic.peak = v;
		}
		if (write(mic.fd, out, bytes) == (ssize_t)bytes)
			mic.frames += frames;
	}
}

static void on_pes(void *opaque, enum ts_stream st, const uint8_t *es, size_t len, uint64_t t, int64_t pts,
		   bool damaged)
{
	if (st == TS_VIDEO)
		queue_frame(opaque, es, len, damaged);
	else if (st == TS_AUDIO)
		mic_audio(es, len);
}

/* Camera status poll, on its own thread: an HTTP request takes up to ~200 ms,
 * and in the receive loop it held up the stream for that long every 3 s (the
 * microphone's buffer ran dry). The RTOS stops feeding the preview ~50 s after
 * the last HTTP request, so keep asking. */
static int g_preset;
static volatile double g_last_restart;

static void *poll_thread(void *arg)
{
	char status[4096];
	int usb_mode_seen = 0;
	for (;;) {
		boost_gpstream();
		if (api("/status", status, sizeof(status)) == 200 && json_field(status, "43", 0) == 7 && !usb_mode_seen) {
			say("camera in USB mode: no preview until it leaves it (h4uvc -p, or the HTTP API)");
			usb_mode_seen = 1;
			if (g_preset) {
				apply_preset_720();
				restart_stream();
				g_last_restart = now_s();
			}
		}
		sleep(3);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	/* -p: apply the 720p preset (photo → video rebuild) before opening the device.
	 * Off by default: right after a mode switch the RTOS may suspend Linux. */
	int preset = argc > 1 && !strcmp(argv[1], "-p");
	const char *dev = argc > 1 + preset ? argv[1 + preset] : "/dev/video0";
	struct uvc *u = &g_uvc;

	/* The preset blocks for ~6 s, so do it before opening the device. */
	char status[4096];
	if (api("/status", status, sizeof(status)) == 200)
		say("camera mode %d, video %d/%d/%d", json_field(status, "43", 0), json_field(status, "2", 1),
		    json_field(status, "3", 1), json_field(status, "4", 1));
	if (preset)
		apply_preset_720();

	/* Linux 3.8's gadget core connects right after binding, overriding the UVC
	 * function's "stay off the bus until the device is opened": the host
	 * enumerated the camera ~7 s before we could answer it, and after a cold
	 * boot OBS got a black picture and a stalled microphone. Opening and
	 * closing the device turns the pullup on and off (uvc_function_connect /
	 * _disconnect), so reconnect now that we're ready. 0.5 s off the bus was
	 * too short: macOS kept its stale audio device and the mic stayed silent. */
	u->fd = open(dev, O_RDWR | O_NONBLOCK);
	if (u->fd >= 0) {
		close(u->fd);
		sleep(3);
		u->fd = open(dev, O_RDWR | O_NONBLOCK);
	}
	if (u->fd < 0) {
		say("open %s: %s", dev, strerror(errno));
		return 1;
	}
	fill_control(&u->probe);
	fill_control(&u->commit);
	unsigned types[] = {UVC_EVENT_CONNECT, UVC_EVENT_DISCONNECT, UVC_EVENT_SETUP,
			    UVC_EVENT_DATA,    UVC_EVENT_STREAMON,   UVC_EVENT_STREAMOFF};
	for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		struct v4l2_event_subscription sub = {.type = types[i]};
		if (ioctl(u->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0)
			say("SUBSCRIBE_EVENT %u: %s", i, strerror(errno));
	}
	say("%s open, waiting for the host", dev);
	mic.fd = open("/dev/h4mic", O_RDWR | O_NONBLOCK);
	if (mic.fd >= 0)
		say("microphone: /dev/h4mic");

	/* A raw socket sees a copy of every incoming UDP datagram, IP header included. */
	int rx = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
	int rcvbuf = 2 << 20;
	if (rx < 0) {
		say("raw socket: %s", strerror(errno));
		return 1;
	}
	setsockopt(rx, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	int ka = socket(AF_INET, SOCK_DGRAM, 0);
	struct in_addr local;
	inet_pton(AF_INET, LOCAL, &local);
	struct sockaddr_in cam = {.sin_family = AF_INET, .sin_port = htons(STREAM_PORT)};
	inet_pton(AF_INET, CAMERA, &cam.sin_addr);

	struct ts_demux demux;
	ts_demux_init(&demux, on_pes, u);

	restart_stream();
	g_last_restart = now_s();
	g_preset = preset;
	pthread_t poller;
	pthread_create(&poller, NULL, poll_thread, NULL);

	double next_keep_alive = 0, next_stats = now_s() + 10, last_rx = now_s();
	unsigned long datagrams = 0;
	static uint8_t d[65536];

	for (;;) {
		double t = now_s();
		if (t >= next_keep_alive) {
			sendto(ka, KEEP_ALIVE, sizeof(KEEP_ALIVE) - 1, 0, (struct sockaddr *)&cam, sizeof(cam));
			next_keep_alive = t + 2.5;
		}
		if (t - last_rx > 5 && t - g_last_restart > 10) {
			say("no stream for %.0f s", t - last_rx);
			g_last_restart = now_s();
			restart_stream();
			ts_demux_reset(&demux);
		}
		if (t >= next_stats) {
			if (u->streaming || datagrams == 0)
				say("%lu datagrams, %lu frames queued, %lu dropped, %llu video losses%s", datagrams, u->frames,
				    u->dropped, (unsigned long long)demux.s[TS_VIDEO].lost, u->streaming ? "" : " (not streaming)");
			if (mic.fd >= 0)
				say("mic: %lu frames in %lu PES (longest gap %.0f ms), %lu decode errors, peak %.0f dBFS, "
				    "resample %+.3f%%", mic.frames, mic.pes, mic.max_gap * 1000, mic.errors,
				    mic.peak ? 20 * log10(mic.peak / 32768.0) : -99.0, (mic.ratio - 1) * 100);
			mic.frames = mic.errors = mic.pes = 0;
			mic.peak = 0;
			mic.max_gap = 0;
			datagrams = 0;
			u->frames = u->dropped = 0;
			demux.s[TS_VIDEO].lost = 0;
			next_stats = t + 10;
		}

		struct pollfd p[2] = {{.fd = u->fd, .events = POLLPRI}, {.fd = rx, .events = POLLIN}};
		if (poll(p, 2, 200) <= 0)
			continue;
		if (p[0].revents & POLLPRI)
			handle_event(u);
		if (!(p[1].revents & POLLIN))
			continue;
		ssize_t got = recv(rx, d, sizeof(d), 0);
		/* IP header (to LOCAL), UDP header (to STREAM_PORT), then gpStream's datagram. */
		if (got < 28)
			continue;
		size_t ihl = (size_t)(d[0] & 0x0f) * 4;
		if ((size_t)got < ihl + 8 || memcmp(d + 16, &local, 4) || (d[ihl + 2] << 8 | d[ihl + 3]) != STREAM_PORT)
			continue;
		uint8_t *g = d + ihl + 8;
		ssize_t len = got - (ssize_t)(ihl + 8);
		size_t n = len >= 12 ? ((size_t)g[10] << 8 | g[11]) : 0;
		if (!(n && n % 188 == 0 && 12 + n <= (size_t)len && g[12] == 0x47))
			continue;
		datagrams++;
		last_rx = now_s();
		bool in[TS_STREAMS] = {false};
		for (size_t i = 0; i < n; i += 188) {
			int st = ts_demux_packet(&demux, g + 12 + i, 0);
			if (st >= 0)
				in[st] = true;
		}
		if (g[0] & HDR_END_OF_FRAME)
			for (int st = 0; st < TS_STREAMS; st++)
				if (in[st])
					ts_demux_flush(&demux, st);
	}
}
