// SPDX-License-Identifier: GPL-2.0
/*
 * h4cam: composite USB gadget for the HERO4 Black.
 *
 *   - CDC ECM (USB Ethernet), as g_ether, so the HTTP API and the USB root
 *     shell keep working;
 *   - UAC1 microphone (f_h4mic.c), PCM written to /dev/h4mic by h4uvc; only
 *     with the patched UDC driver (src/ambarella_udc);
 *   - UVC 1.1 camera with one frame-based H.264 format, 1280x720 at 29.97 fps,
 *     streamed over a bulk endpoint (the Ambarella UDC names no isochronous
 *     endpoints, and macOS accepts bulk). The userspace server (h4uvc) answers
 *     the UVC control requests and feeds it the camera's own H.264 stream.
 *
 * Based on drivers/usb/gadget/webcam.c and ether.c (Linux 3.8, GPL v2).
 * The UVC function sources in this directory are patched copies; see the
 * "h4cam:" comments.
 */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/usb/video.h>
#include <linux/vmalloc.h>

#include "u_ether.h"
#include "f_uvc.h"

USB_GADGET_COMPOSITE_OPTIONS();

#include "f_ecm.c"

#include "uvc_queue.c"
#include "uvc_video.c"
#include "uvc_v4l2.c"
#include "f_uvc.c"
#include "f_h4mic.c"

/* --------------------------------------------------------------------------
 * Device descriptor
 */
#define H4CAM_VENDOR_ID		0x1d6b	/* Linux Foundation */
#define H4CAM_PRODUCT_ID	0x0104	/* Multifunction composite gadget */
#define H4CAM_DEVICE_BCD	0x0001

static char h4cam_vendor_label[] = "GoPro";
static char h4cam_product_label[] = "HERO4 Black";
static char h4cam_config_label[] = "Camera + Ethernet";

#define STRING_DESCRIPTION_IDX	USB_GADGET_FIRST_AVAIL_IDX

static struct usb_string h4cam_strings[] = {
	[USB_GADGET_MANUFACTURER_IDX].s = h4cam_vendor_label,
	[USB_GADGET_PRODUCT_IDX].s = h4cam_product_label,
	[USB_GADGET_SERIAL_IDX].s = "",
	[STRING_DESCRIPTION_IDX].s = h4cam_config_label,
	{  }
};

static struct usb_gadget_strings h4cam_stringtab = {
	.language = 0x0409,
	.strings = h4cam_strings,
};

static struct usb_gadget_strings *h4cam_device_strings[] = {
	&h4cam_stringtab,
	NULL,
};

static struct usb_device_descriptor h4cam_device_descriptor = {
	.bLength		= USB_DT_DEVICE_SIZE,
	.bDescriptorType	= USB_DT_DEVICE,
	.bcdUSB			= cpu_to_le16(0x0200),
	.bDeviceClass		= USB_CLASS_MISC,	/* interface association */
	.bDeviceSubClass	= 0x02,
	.bDeviceProtocol	= 0x01,
	.idVendor		= cpu_to_le16(H4CAM_VENDOR_ID),
	.idProduct		= cpu_to_le16(H4CAM_PRODUCT_ID),
	.bcdDevice		= cpu_to_le16(H4CAM_DEVICE_BCD),
};

/* --------------------------------------------------------------------------
 * UVC descriptors
 */
#ifndef UVC_VS_FORMAT_FRAME_BASED
#define UVC_VS_FORMAT_FRAME_BASED	0x10
#define UVC_VS_FRAME_FRAME_BASED	0x11
#endif

/* UVC 1.1 frame-based payload format (no struct for it in this kernel). */
struct uvc_format_frame_based {
	__u8  bLength;
	__u8  bDescriptorType;
	__u8  bDescriptorSubType;
	__u8  bFormatIndex;
	__u8  bNumFrameDescriptors;
	__u8  guidFormat[16];
	__u8  bBitsPerPixel;
	__u8  bDefaultFrameIndex;
	__u8  bAspectRatioX;
	__u8  bAspectRatioY;
	__u8  bmInterlaceFlags;
	__u8  bCopyProtect;
	__u8  bVariableSize;
} __attribute__((__packed__));

struct uvc_frame_frame_based_1 {
	__u8  bLength;
	__u8  bDescriptorType;
	__u8  bDescriptorSubType;
	__u8  bFrameIndex;
	__u8  bmCapabilities;
	__le16 wWidth;
	__le16 wHeight;
	__le32 dwMinBitRate;
	__le32 dwMaxBitRate;
	__le32 dwDefaultFrameInterval;
	__u8  bFrameIntervalType;
	__le32 dwBytesPerLine;
	__le32 dwFrameInterval[1];
} __attribute__((__packed__));

DECLARE_UVC_HEADER_DESCRIPTOR(1);

static const struct UVC_HEADER_DESCRIPTOR(1) uvc_control_header = {
	.bLength		= UVC_DT_HEADER_SIZE(1),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VC_HEADER,
	.bcdUVC			= cpu_to_le16(0x0110),	/* frame-based formats need 1.1 */
	.wTotalLength		= 0, /* dynamic */
	.dwClockFrequency	= cpu_to_le32(48000000),
	.bInCollection		= 0, /* dynamic */
	.baInterfaceNr[0]	= 0, /* dynamic */
};

/* No controls on the camera terminal or processing unit, so the host has
 * nothing to query beyond probe/commit. */
static const struct uvc_camera_terminal_descriptor uvc_camera_terminal = {
	.bLength		= UVC_DT_CAMERA_TERMINAL_SIZE(3),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VC_INPUT_TERMINAL,
	.bTerminalID		= 1,
	.wTerminalType		= cpu_to_le16(0x0201),
	.bAssocTerminal		= 0,
	.iTerminal		= 0,
	.wObjectiveFocalLengthMin	= cpu_to_le16(0),
	.wObjectiveFocalLengthMax	= cpu_to_le16(0),
	.wOcularFocalLength		= cpu_to_le16(0),
	.bControlSize		= 3,
	.bmControls		= { 0, 0, 0 },
};

static const struct uvc_processing_unit_descriptor uvc_processing = {
	.bLength		= UVC_DT_PROCESSING_UNIT_SIZE(2),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VC_PROCESSING_UNIT,
	.bUnitID		= 2,
	.bSourceID		= 1,
	.wMaxMultiplier		= cpu_to_le16(0),
	.bControlSize		= 2,
	.bmControls		= { 0, 0 },
	.iProcessing		= 0,
};

static const struct uvc_output_terminal_descriptor uvc_output_terminal = {
	.bLength		= UVC_DT_OUTPUT_TERMINAL_SIZE,
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VC_OUTPUT_TERMINAL,
	.bTerminalID		= 3,
	.wTerminalType		= cpu_to_le16(0x0101),
	.bAssocTerminal		= 0,
	.bSourceID		= 2,
	.iTerminal		= 0,
};

DECLARE_UVC_INPUT_HEADER_DESCRIPTOR(1, 1);

static const struct UVC_INPUT_HEADER_DESCRIPTOR(1, 1) uvc_input_header = {
	.bLength		= UVC_DT_INPUT_HEADER_SIZE(1, 1),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VS_INPUT_HEADER,
	.bNumFormats		= 1,
	.wTotalLength		= 0, /* dynamic */
	.bEndpointAddress	= 0, /* dynamic */
	.bmInfo			= 0,
	.bTerminalLink		= 3,
	.bStillCaptureMethod	= 0,
	.bTriggerSupport	= 0,
	.bTriggerUsage		= 0,
	.bControlSize		= 1,
	.bmaControls[0][0]	= 0,
};

static const struct uvc_format_frame_based uvc_format_h264 = {
	.bLength		= sizeof(struct uvc_format_frame_based),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VS_FORMAT_FRAME_BASED,
	.bFormatIndex		= 1,
	.bNumFrameDescriptors	= 1,
	.guidFormat		=
		{ 'H',  '2',  '6',  '4', 0x00, 0x00, 0x10, 0x00,
		 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71},
	.bBitsPerPixel		= 16,
	.bDefaultFrameIndex	= 1,
	.bAspectRatioX		= 0,
	.bAspectRatioY		= 0,
	.bmInterlaceFlags	= 0,
	.bCopyProtect		= 0,
	.bVariableSize		= 1,
};

static const struct uvc_frame_frame_based_1 uvc_frame_h264_720p = {
	.bLength		= sizeof(struct uvc_frame_frame_based_1),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VS_FRAME_FRAME_BASED,
	.bFrameIndex		= 1,
	.bmCapabilities		= 0,
	.wWidth			= cpu_to_le16(1280),
	.wHeight		= cpu_to_le16(720),
	.dwMinBitRate		= cpu_to_le32(1000000),
	.dwMaxBitRate		= cpu_to_le32(20000000),
	.dwDefaultFrameInterval	= cpu_to_le32(333667),	/* 29.97 fps, 100 ns units */
	.bFrameIntervalType	= 1,
	.dwBytesPerLine		= 0,
	.dwFrameInterval[0]	= cpu_to_le32(333667),
};

static const struct uvc_color_matching_descriptor uvc_color_matching = {
	.bLength		= UVC_DT_COLOR_MATCHING_SIZE,
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VS_COLORFORMAT,
	.bColorPrimaries	= 1,	/* BT.709 */
	.bTransferCharacteristics	= 1,
	.bMatrixCoefficients	= 1,	/* BT.709 */
};

static const struct uvc_descriptor_header * const uvc_control_cls[] = {
	(const struct uvc_descriptor_header *) &uvc_control_header,
	(const struct uvc_descriptor_header *) &uvc_camera_terminal,
	(const struct uvc_descriptor_header *) &uvc_processing,
	(const struct uvc_descriptor_header *) &uvc_output_terminal,
	NULL,
};

static const struct uvc_descriptor_header * const uvc_streaming_cls[] = {
	(const struct uvc_descriptor_header *) &uvc_input_header,
	(const struct uvc_descriptor_header *) &uvc_format_h264,
	(const struct uvc_descriptor_header *) &uvc_frame_h264_720p,
	(const struct uvc_descriptor_header *) &uvc_color_matching,
	NULL,
};

/* --------------------------------------------------------------------------
 * Configuration
 */
static u8 hostaddr[ETH_ALEN];

static bool mic = true;
module_param(mic, bool, S_IRUGO);
MODULE_PARM_DESC(mic, "add the UAC1 microphone (needs the patched UDC driver)");

static int __init h4cam_config_bind(struct usb_configuration *c)
{
	int ret = ecm_bind_config(c, hostaddr);
	bool with_mic = false;

	if (ret < 0)
		return ret;
	/* The microphone needs the patched UDC driver (f_h4mic.c); without it, a
	 * camera with no microphone beats no camera at all. */
	if (mic) {
		void *preset = symbol_get(h4_udc_preset_iso);
		if (preset) {
			symbol_put(h4_udc_preset_iso);
			with_mic = true;
		} else
			INFO(c->cdev, "no microphone: the UDC driver isn't the patched one (ambarella_udc_h4.ko)\n");
	}
	uvc_status_ep = !with_mic;
	ret = uvc_bind_config(c, uvc_control_cls, uvc_control_cls, uvc_streaming_cls, uvc_streaming_cls,
			      uvc_streaming_cls);
	if (ret < 0)
		return ret;
	/* Last, so ECM keeps interfaces 0-1 and UVC 2-3 (h4uvc expects streaming on 3). */
	return with_mic ? h4mic_bind_config(c) : 0;
}

static struct usb_configuration h4cam_config_driver = {
	.label			= h4cam_config_label,
	.bConfigurationValue	= 1,
	.bmAttributes		= USB_CONFIG_ATT_SELFPOWER,
	.bMaxPower		= CONFIG_USB_GADGET_VBUS_DRAW / 2,
};

static int h4cam_unbind(struct usb_composite_dev *cdev)
{
	gether_cleanup();
	return 0;
}

static int __init h4cam_bind(struct usb_composite_dev *cdev)
{
	int ret = gether_setup(cdev->gadget, hostaddr);
	if (ret < 0)
		return ret;

	ret = usb_string_ids_tab(cdev, h4cam_strings);
	if (ret < 0)
		goto error;
	h4cam_device_descriptor.iManufacturer = h4cam_strings[USB_GADGET_MANUFACTURER_IDX].id;
	h4cam_device_descriptor.iProduct = h4cam_strings[USB_GADGET_PRODUCT_IDX].id;
	h4cam_config_driver.iConfiguration = h4cam_strings[STRING_DESCRIPTION_IDX].id;

	ret = usb_add_config(cdev, &h4cam_config_driver, h4cam_config_bind);
	if (ret < 0)
		goto error;
	usb_composite_overwrite_options(cdev, &coverwrite);
	INFO(cdev, "h4cam: UVC H.264 + CDC ECM\n");
	return 0;

error:
	gether_cleanup();
	return ret;
}

static __refdata struct usb_composite_driver h4cam_driver = {
	.name		= "h4cam",
	.dev		= &h4cam_device_descriptor,
	.strings	= h4cam_device_strings,
	.max_speed	= USB_SPEED_HIGH,
	.bind		= h4cam_bind,
	.unbind		= h4cam_unbind,
};

static int __init h4cam_init(void)
{
	return usb_composite_probe(&h4cam_driver);
}
module_init(h4cam_init);

static void __exit h4cam_cleanup(void)
{
	usb_composite_unregister(&h4cam_driver);
}
module_exit(h4cam_cleanup);

/* Last: u_ether.c redefines INFO()/DBG() for its own struct. */
#include "u_ether.c"

MODULE_DESCRIPTION("HERO4 Black: UVC H.264 camera + CDC ECM");
MODULE_LICENSE("GPL");
