// SPDX-License-Identifier: GPL-2.0
/*
 * f_h4mic: USB Audio Class 1 microphone, 48 kHz stereo 16-bit, fed from
 * userspace through /dev/h4mic (write raw s16le interleaved PCM). No ALSA:
 * the HERO4 kernel has no sound subsystem.
 *
 * The Ambarella UDC names no endpoint "-iso", so usb_ep_autoconfig() can't
 * place an isochronous one. We take a bulk IN endpoint and enable it with the
 * isochronous descriptor; the controller takes the type from it. Needs the
 * patched UDC driver (src/ambarella_udc, h4_udc_preset_iso): the stock one
 * can't select the streaming alt setting or send full isochronous packets, and
 * the controller then stops responding (docs/findings.md).
 *
 * Each 1 ms packet carries 48 frames, on a synchronous endpoint (locked to
 * USB's frame clock). Declared asynchronous, macOS estimated a device clock
 * from the packet sizes and slipped ~34 samples at its 512-frame buffer
 * boundaries, tens of times a second. The camera's audio clock runs ~0.7%
 * fast against USB's, so h4uvc resamples to keep this buffer at its target,
 * reading its level (averaged over ~1 s) from /dev/h4mic. Packets never carry
 * 47 or 49 frames: on a synchronous endpoint macOS drops or pads the extra
 * sample, and those corrections, dozens of times a second, crackled.
 * One packet per request: the controller mangles chains of isochronous
 * descriptors. Underrun: silence.
 * tone=1 sends a 1 kHz sine instead of the buffer, tone=2 a ramp/counter
 * pattern (bring-up tests), tone=3 the ramp with a full packet counter and the
 * USB microframe each packet was filled in (lines up missed frames with what
 * the host recorded, sample-exact).
 */
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/usb/audio.h>

/* Exported by the patched UDC driver; looked up at bind, so h4cam still loads
 * on the stock driver (without the microphone). */
extern void h4_udc_preset_iso(struct usb_ep *ep, int intf, int alt, int maxpacket);

static int tone;
module_param(tone, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(tone, "1: send a 1 kHz test tone; 2: ramp/counter test pattern; 3: ramp, counter and microframe");

#define MIC_RATE	48000
#define MIC_CHANNELS	2
#define MIC_FRAME	(MIC_CHANNELS * 2)		/* bytes per sample frame */
#define MIC_PER_MS	(MIC_RATE / 1000)		/* 48 frames */
#define MIC_PACKET	(MIC_PER_MS * MIC_FRAME)	/* 192 bytes, nominal */
#define MIC_MAXPACKET	((MIC_PER_MS + 2) * MIC_FRAME)	/* 200: up to 50 frames */
#define MIC_NREQ	4
#define MIC_RING	(64 * 1024)			/* ~340 ms */
/* Buffer target, adaptive: the preview's audio reaches h4uvc with gaps between
 * AAC frames of usually ~80-100 ms, at times ~170 ms. A fixed 150 ms never ran
 * dry but cost latency (the mic was ~100 ms behind the video), so start low,
 * grow by MIC_STEP on every underrun, and come back down by MIC_DECAY after
 * MIC_CALM packets (ms) without one. */
#define MIC_MS(ms)	((ms) * MIC_PER_MS * MIC_FRAME)
#define MIC_TARGET_MIN	MIC_MS(90)	/* the AAC arrives with gaps of 80-140 ms */
#define MIC_TARGET_MAX	MIC_MS(250)
#define MIC_STEP	MIC_MS(30)
#define MIC_DECAY	MIC_MS(10)
#define MIC_CALM	20000

static struct usb_string h4mic_strings_dev[] = {
	[0].s = "GoPro HERO4",
	{ }
};
static struct usb_gadget_strings h4mic_stringtab = {
	.language = 0x0409,
	.strings = h4mic_strings_dev,
};
static struct usb_gadget_strings *h4mic_strings[] = {
	&h4mic_stringtab,
	NULL,
};

static struct usb_interface_assoc_descriptor mic_iad = {
	.bLength		= sizeof(mic_iad),
	.bDescriptorType	= USB_DT_INTERFACE_ASSOCIATION,
	.bInterfaceCount	= 2,
	.bFunctionClass		= USB_CLASS_AUDIO,
	.bFunctionSubClass	= USB_SUBCLASS_AUDIOSTREAMING,
	.bFunctionProtocol	= 0,
};

static struct usb_interface_descriptor mic_ac_intf = {
	.bLength		= USB_DT_INTERFACE_SIZE,
	.bDescriptorType	= USB_DT_INTERFACE,
	.bNumEndpoints		= 0,
	.bInterfaceClass	= USB_CLASS_AUDIO,
	.bInterfaceSubClass	= USB_SUBCLASS_AUDIOCONTROL,
};

DECLARE_UAC_AC_HEADER_DESCRIPTOR(1);

#define MIC_IT_ID	1
#define MIC_OT_ID	2
#define MIC_AC_TOTAL	(UAC_DT_AC_HEADER_SIZE(1) + UAC_DT_INPUT_TERMINAL_SIZE + UAC_DT_OUTPUT_TERMINAL_SIZE)

static struct uac1_ac_header_descriptor_1 mic_ac_header = {
	.bLength		= UAC_DT_AC_HEADER_SIZE(1),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubtype	= UAC_HEADER,
	.bcdADC			= cpu_to_le16(0x0100),
	.wTotalLength		= cpu_to_le16(MIC_AC_TOTAL),
	.bInCollection		= 1,
	/* .baInterfaceNr[0]: the AS interface, set at bind */
};

static struct uac_input_terminal_descriptor mic_it = {
	.bLength		= UAC_DT_INPUT_TERMINAL_SIZE,
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubtype	= UAC_INPUT_TERMINAL,
	.bTerminalID		= MIC_IT_ID,
	.wTerminalType		= cpu_to_le16(0x0201),	/* microphone */
	.bNrChannels		= MIC_CHANNELS,
	.wChannelConfig		= cpu_to_le16(0x0003),	/* left, right */
};

static struct uac1_output_terminal_descriptor mic_ot = {
	.bLength		= UAC_DT_OUTPUT_TERMINAL_SIZE,
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubtype	= UAC_OUTPUT_TERMINAL,
	.bTerminalID		= MIC_OT_ID,
	.wTerminalType		= cpu_to_le16(UAC_TERMINAL_STREAMING),
	.bSourceID		= MIC_IT_ID,
};

static struct usb_interface_descriptor mic_as_alt0 = {
	.bLength		= USB_DT_INTERFACE_SIZE,
	.bDescriptorType	= USB_DT_INTERFACE,
	.bAlternateSetting	= 0,
	.bNumEndpoints		= 0,
	.bInterfaceClass	= USB_CLASS_AUDIO,
	.bInterfaceSubClass	= USB_SUBCLASS_AUDIOSTREAMING,
};

static struct usb_interface_descriptor mic_as_alt1 = {
	.bLength		= USB_DT_INTERFACE_SIZE,
	.bDescriptorType	= USB_DT_INTERFACE,
	.bAlternateSetting	= 1,
	.bNumEndpoints		= 1,
	.bInterfaceClass	= USB_CLASS_AUDIO,
	.bInterfaceSubClass	= USB_SUBCLASS_AUDIOSTREAMING,
};

static struct uac1_as_header_descriptor mic_as_header = {
	.bLength		= UAC_DT_AS_HEADER_SIZE,
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubtype	= UAC_AS_GENERAL,
	.bTerminalLink		= MIC_OT_ID,
	.bDelay			= 1,
	.wFormatTag		= cpu_to_le16(UAC_FORMAT_TYPE_I_PCM),
};

DECLARE_UAC_FORMAT_TYPE_I_DISCRETE_DESC(1);

static struct uac_format_type_i_discrete_descriptor_1 mic_format = {
	.bLength		= UAC_FORMAT_TYPE_I_DISCRETE_DESC_SIZE(1),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubtype	= UAC_FORMAT_TYPE,
	.bFormatType		= UAC_FORMAT_TYPE_I,
	.bNrChannels		= MIC_CHANNELS,
	.bSubframeSize		= 2,
	.bBitResolution		= 16,
	.bSamFreqType		= 1,
	.tSamFreq[0]		= { MIC_RATE & 0xff, (MIC_RATE >> 8) & 0xff, MIC_RATE >> 16 },
};

/* One packet per millisecond: bInterval 1 at full speed, 4 (8 microframes) at high speed. */
static struct usb_endpoint_descriptor mic_fs_ep = {
	.bLength		= USB_DT_ENDPOINT_AUDIO_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_SYNC,
	.wMaxPacketSize		= cpu_to_le16(MIC_MAXPACKET),
	.bInterval		= 1,
};

static struct usb_endpoint_descriptor mic_hs_ep = {
	.bLength		= USB_DT_ENDPOINT_AUDIO_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_SYNC,
	.wMaxPacketSize		= cpu_to_le16(MIC_MAXPACKET),
	.bInterval		= 4,
};

static struct uac_iso_endpoint_descriptor mic_cs_ep = {
	.bLength		= UAC_ISO_ENDPOINT_DESC_SIZE,
	.bDescriptorType	= USB_DT_CS_ENDPOINT,
	.bDescriptorSubtype	= UAC_EP_GENERAL,
	.bmAttributes		= 0,
	.bLockDelayUnits	= 0,
	.wLockDelay		= 0,
};

#define MIC_DESCS(ep) {							\
	(struct usb_descriptor_header *)&mic_iad,			\
	(struct usb_descriptor_header *)&mic_ac_intf,			\
	(struct usb_descriptor_header *)&mic_ac_header,			\
	(struct usb_descriptor_header *)&mic_it,			\
	(struct usb_descriptor_header *)&mic_ot,			\
	(struct usb_descriptor_header *)&mic_as_alt0,			\
	(struct usb_descriptor_header *)&mic_as_alt1,			\
	(struct usb_descriptor_header *)&mic_as_header,			\
	(struct usb_descriptor_header *)&mic_format,			\
	(struct usb_descriptor_header *)&ep,				\
	(struct usb_descriptor_header *)&mic_cs_ep,			\
	NULL,								\
}

static struct usb_descriptor_header *mic_fs_descs[] = MIC_DESCS(mic_fs_ep);
static struct usb_descriptor_header *mic_hs_descs[] = MIC_DESCS(mic_hs_ep);

struct h4mic {
	struct usb_function func;
	struct usb_ep *ep;
	struct usb_request *req[MIC_NREQ];
	int ac_intf, as_intf, alt;
	bool enabled;			/* endpoint enabled (kept across alt 0) */
	unsigned long busy;		/* bit i: req[i] is queued */

	spinlock_t lock;		/* ring */
	u8 *ring;
	unsigned head, fill;		/* write position, bytes buffered */
	bool primed;			/* filled up to the target since the last underrun */
	unsigned phase;			/* test tone */
	unsigned long underruns, overruns;
	unsigned long packets, errors;		/* completions */
	unsigned target;			/* bytes buffered to aim for */
	unsigned long calm;			/* packets since the last underrun */
	unsigned long avg;			/* buffer level, averaged over ~1 s, << 10 */
};

static struct h4mic *the_mic;	/* for the misc device */

static inline struct h4mic *to_mic(struct usb_function *f)
{
	return container_of(f, struct h4mic, func);
}

/* 1 kHz at 48 kHz: one period per millisecond packet. */
static const s16 sine48[48] = {
	0, 1071, 2124, 3141, 4106, 5000, 5811, 6523, 7124, 7604, 7955, 8170,
	8243, 8170, 7955, 7604, 7124, 6523, 5811, 5000, 4106, 3141, 2124, 1071,
	0, -1071, -2124, -3141, -4106, -5000, -5811, -6523, -7124, -7604, -7955, -8170,
	-8243, -8170, -7955, -7604, -7124, -6523, -5811, -5000, -4106, -3141, -2124, -1071,
};

/* Copy n bytes from the ring's oldest data. Called with mic->lock held. */
static void ring_read(struct h4mic *mic, u8 *dst, unsigned n)
{
	unsigned tail = (mic->head + MIC_RING - mic->fill) % MIC_RING, first = min(n, MIC_RING - tail);

	memcpy(dst, mic->ring + tail, first);
	memcpy(dst + first, mic->ring, n - first);
	mic->fill -= n;
}

/* Fill one packet; returns its length. */
static unsigned mic_fill_packet(struct h4mic *mic, u8 *buf)
{
	unsigned i;

	if (tone) {
		s16 *s = (s16 *)buf;
		/* Microframe counter (11 bits) when this packet is filled, i.e. when
		 * the packet MIC_NREQ before it completed. */
		u16 uframe = usb_gadget_frame_number(mic->func.config->cdev->gadget) & 0x7ff;
		for (i = 0; i < MIC_PER_MS; i++)
			if (tone == 3) {	/* left: ramp; right: packet counter, then uframe */
				s[2 * i] = i * 600;
				s[2 * i + 1] = i < MIC_PER_MS / 2 ? mic->phase & 0x7fff : uframe;
			} else if (tone == 2) {	/* left: ramp within the packet; right: packet counter */
				s[2 * i] = i * 600;
				s[2 * i + 1] = (mic->phase % 32) * 900;
			} else
				s[2 * i] = s[2 * i + 1] = sine48[i];
		mic->phase++;
		return MIC_PACKET;
	}

	{
		unsigned frames = MIC_PER_MS;
		if (!mic->primed || mic->fill < frames * MIC_FRAME) {
			if (mic->primed) {
				mic->underruns++;
				mic->target = min_t(unsigned, mic->target + MIC_STEP, MIC_TARGET_MAX);
				mic->calm = 0;
			}
			mic->primed = false;
			memset(buf, 0, MIC_PACKET);
			return MIC_PACKET;
		}
		if (++mic->calm >= MIC_CALM) {
			mic->calm = 0;
			mic->target = max_t(unsigned, mic->target - MIC_DECAY, MIC_TARGET_MIN);
		}
		ring_read(mic, buf, frames * MIC_FRAME);
		mic->avg += ((unsigned long)mic->fill << 10) / 1024 - mic->avg / 1024;
		return frames * MIC_FRAME;
	}
}

static void mic_fill(struct h4mic *mic, struct usb_request *req)
{
	unsigned long flags;

	spin_lock_irqsave(&mic->lock, flags);
	req->length = mic_fill_packet(mic, req->buf);
	spin_unlock_irqrestore(&mic->lock, flags);
}

static void mic_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct h4mic *mic = req->context;
	int i;

	for (i = 0; i < MIC_NREQ && mic->req[i] != req; i++)
		;
	if (req->status == -ESHUTDOWN || req->status == -ECONNRESET || mic->alt != 1) {
		clear_bit(i, &mic->busy);	/* stream off: the request stays ours */
		return;
	}
	if (req->status)
		mic->errors++;
	mic->packets++;
	mic_fill(mic, req);
	if (usb_ep_queue(ep, req, GFP_ATOMIC)) {
		clear_bit(i, &mic->busy);
		pr_info_ratelimited("h4mic: queue failed\n");
	}
}

/* --- /dev/h4mic --------------------------------------------------------- */

static ssize_t h4mic_write(struct file *file, const char __user *data, size_t count, loff_t *off)
{
	struct h4mic *mic = the_mic;
	static u8 tmp[4096];		/* writes come from h4uvc only */
	size_t done = 0;
	unsigned long flags;

	if (!mic)
		return -ENODEV;
	while (done < count) {
		size_t n = min(count - done, sizeof(tmp)), first;
		if (copy_from_user(tmp, data + done, n))
			return done ? done : -EFAULT;
		spin_lock_irqsave(&mic->lock, flags);
		first = min_t(size_t, n, MIC_RING - mic->head);
		memcpy(mic->ring + mic->head, tmp, first);
		memcpy(mic->ring, tmp + first, n - first);
		mic->head = (mic->head + n) % MIC_RING;
		mic->fill += n;
		if (mic->fill > MIC_RING) {	/* nobody reading: keep the newest */
			mic->fill = MIC_RING;
			mic->overruns++;
		}
		if (mic->fill >= mic->target && !mic->primed) {
			mic->primed = true;
			mic->avg = (unsigned long)mic->fill << 10;
		}
		spin_unlock_irqrestore(&mic->lock, flags);
		done += n;
	}
	return done;
}

/* Reading gives h4uvc's resampler three u32: the buffer level averaged over
 * ~1 s and the target, in frames, and whether the host is streaming. */
static ssize_t h4mic_read(struct file *file, char __user *data, size_t count, loff_t *off)
{
	struct h4mic *mic = the_mic;
	unsigned long flags;
	u32 st[3];

	if (!mic)
		return -ENODEV;
	spin_lock_irqsave(&mic->lock, flags);
	st[0] = (mic->avg >> 10) / MIC_FRAME;
	st[1] = mic->target / MIC_FRAME;
	st[2] = mic->alt == 1 && mic->primed;
	spin_unlock_irqrestore(&mic->lock, flags);
	count = min(count, sizeof(st));
	return copy_to_user(data, st, count) ? -EFAULT : count;
}

static const struct file_operations h4mic_fops = {
	.read = h4mic_read,
	.owner = THIS_MODULE,
	.write = h4mic_write,
	.llseek = noop_llseek,
};

static struct miscdevice h4mic_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "h4mic",
	.fops = &h4mic_fops,
};

/* --- function ------------------------------------------------------------ */

/* The host closing the microphone (alt 0) only stops refilling: the endpoint
 * stays enabled and the requests already queued wait for the next IN tokens.
 * Disabling the isochronous endpoint and enabling it again hung the whole
 * camera (RTOS included, no kernel message) within a second of the second
 * open, on either firmware. Only a real disconnect or reconfiguration
 * (mic_disable) disables it. */
static void mic_pause(struct h4mic *mic)
{
	if (mic->alt == 1) {
		mic->alt = 0;
		pr_info("h4mic: stopped; %lu packets, %lu errors, %lu underruns, %lu overruns so far; buffer %u ms\n",
			mic->packets, mic->errors, mic->underruns, mic->overruns, mic->target / MIC_MS(1));
	}
}

static void mic_stop(struct h4mic *mic)
{
	mic_pause(mic);
	if (mic->enabled) {
		mic->enabled = false;
		usb_ep_disable(mic->ep);
		mic->busy = 0;
	}
}

static int mic_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct h4mic *mic = to_mic(f);
	unsigned long flags;
	int i, ret;

	if (intf == mic->ac_intf)
		return alt ? -EINVAL : 0;
	if (intf != mic->as_intf || alt > 1)
		return -EINVAL;

	mic_pause(mic);
	if (alt == 0)
		return 0;

	if (!mic->enabled) {
		ret = config_ep_by_speed(f->config->cdev->gadget, f, mic->ep);
		if (ret) {
			pr_info("h4mic: no descriptor for %s (%d)\n", mic->ep->name, ret);
			return ret;
		}
	}
	/* Nobody read while the stream was off, so the ring may hold up to
	 * MIC_RING of old audio: keep only the newest target's worth (latency). */
	spin_lock_irqsave(&mic->lock, flags);
	if (mic->fill > mic->target)
		mic->fill = mic->target;
	mic->avg = (unsigned long)mic->fill << 10;
	spin_unlock_irqrestore(&mic->lock, flags);

	if (!mic->enabled) {
		ret = usb_ep_enable(mic->ep);
		if (ret)
			return ret;
		mic->enabled = true;
	}
	mic->alt = 1;
	for (i = 0; i < MIC_NREQ; i++) {
		if (test_and_set_bit(i, &mic->busy))
			continue;	/* still queued from before the pause */
		mic_fill(mic, mic->req[i]);
		if (usb_ep_queue(mic->ep, mic->req[i], GFP_ATOMIC))
			clear_bit(i, &mic->busy);
	}
	return 0;
}

static int mic_get_alt(struct usb_function *f, unsigned intf)
{
	struct h4mic *mic = to_mic(f);
	return intf == mic->as_intf ? mic->alt : 0;
}

static void mic_disable(struct usb_function *f)
{
	mic_stop(to_mic(f));
}

static int mic_setup(struct usb_function *f, const struct usb_ctrlrequest *ctrl)
{
	return -EOPNOTSUPP;	/* no controls: the host gets a stall */
}

static int __init mic_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_composite_dev *cdev = c->cdev;
	struct h4mic *mic = to_mic(f);
	struct usb_endpoint_descriptor claim = {
		.bLength = USB_DT_ENDPOINT_SIZE,
		.bDescriptorType = USB_DT_ENDPOINT,
		.bEndpointAddress = USB_DIR_IN,
		.bmAttributes = USB_ENDPOINT_XFER_BULK,
	};
	int ret, i;

	void (*preset)(struct usb_ep *, int, int, int) = symbol_get(h4_udc_preset_iso);

	if (!preset)	/* h4cam checks before adding the function */
		return -ENODEV;
	ret = usb_string_id(cdev);
	if (ret < 0)
		return ret;
	h4mic_strings_dev[0].id = ret;
	mic_ac_intf.iInterface = ret;
	mic_iad.iFunction = ret;

	if ((ret = usb_interface_id(c, f)) < 0)
		return ret;
	mic->ac_intf = ret;
	mic_iad.bFirstInterface = ret;
	mic_ac_intf.bInterfaceNumber = ret;
	if ((ret = usb_interface_id(c, f)) < 0)
		return ret;
	mic->as_intf = ret;
	mic_as_alt0.bInterfaceNumber = ret;
	mic_as_alt1.bInterfaceNumber = ret;
	mic_ac_header.baInterfaceNr[0] = ret;

	/* No "-iso" endpoints on this UDC: take a bulk one, use it as isochronous.
	 * Only ep1in..ep5in have their own endpoint register: the driver maps epNin
	 * to register N and epNout to N + 5, so ep6in would overwrite ep1out's (USB
	 * Ethernet's). usb_ep_autoconfig() doesn't know, so pick a free one here. */
	mic->ep = NULL;
	for (i = 5; i >= 1 && !mic->ep; i--) {
		struct usb_ep *ep;
		char name[16];
		snprintf(name, sizeof(name), "ep%din-bulk", i);
		list_for_each_entry(ep, &cdev->gadget->ep_list, ep_list)
			if (!ep->driver_data && !strcmp(ep->name, name)) {
				mic->ep = ep;
				claim.bEndpointAddress = USB_DIR_IN | i;
				break;
			}
	}
	if (!mic->ep)
		return -ENODEV;
	mic->ep->driver_data = mic;
	mic->ep->address = claim.bEndpointAddress;	/* autoconfig would have set it */
	preset(mic->ep, mic->as_intf, 1, MIC_MAXPACKET);
	symbol_put(h4_udc_preset_iso);
	mic_fs_ep.bEndpointAddress = claim.bEndpointAddress;
	mic_hs_ep.bEndpointAddress = claim.bEndpointAddress;

	for (i = 0; i < MIC_NREQ; i++) {
		mic->req[i] = usb_ep_alloc_request(mic->ep, GFP_KERNEL);
		if (!mic->req[i])
			return -ENOMEM;
		mic->req[i]->buf = kzalloc(MIC_MAXPACKET, GFP_KERNEL);
		if (!mic->req[i]->buf)
			return -ENOMEM;
		mic->req[i]->complete = mic_complete;
		mic->req[i]->context = mic;
	}

	ret = usb_assign_descriptors(f, mic_fs_descs, mic_hs_descs, NULL);
	if (ret)
		return ret;
	INFO(cdev, "h4mic: UAC1 microphone on %s, interfaces %d/%d%s\n", mic->ep->name, mic->ac_intf,
	     mic->as_intf, tone ? ", test tone" : "");
	return 0;
}

static void mic_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct h4mic *mic = to_mic(f);
	int i;

	misc_deregister(&h4mic_misc);
	the_mic = NULL;
	for (i = 0; i < MIC_NREQ; i++)
		if (mic->req[i]) {
			kfree(mic->req[i]->buf);
			usb_ep_free_request(mic->ep, mic->req[i]);
		}
	usb_free_all_descriptors(f);
	vfree(mic->ring);
	kfree(mic);
}

static int __init h4mic_bind_config(struct usb_configuration *c)
{
	struct h4mic *mic = kzalloc(sizeof(*mic), GFP_KERNEL);
	int ret;

	if (!mic)
		return -ENOMEM;
	mic->ring = vmalloc(MIC_RING);
	if (!mic->ring) {
		kfree(mic);
		return -ENOMEM;
	}
	spin_lock_init(&mic->lock);
	mic->target = MIC_TARGET_MIN;
	mic->func.name = "h4mic";
	mic->func.strings = h4mic_strings;
	mic->func.bind = mic_bind;
	mic->func.unbind = mic_unbind;
	mic->func.set_alt = mic_set_alt;
	mic->func.get_alt = mic_get_alt;
	mic->func.disable = mic_disable;
	mic->func.setup = mic_setup;

	ret = usb_add_function(c, &mic->func);
	if (ret) {
		vfree(mic->ring);
		kfree(mic);
		return ret;
	}
	the_mic = mic;
	ret = misc_register(&h4mic_misc);
	if (ret)
		pr_info("h4mic: misc_register failed (%d)\n", ret);
	return 0;
}
