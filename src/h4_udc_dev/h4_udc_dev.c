/*
 * Registers the "ambarella-udc" platform device on the HERO4 Black.
 *
 * GoPro's kernel is built with PLAT_AMBARELLA_SUPPORT_UDC but its board file
 * never registers the device, so ambarella_udc.ko has nothing to bind to.
 * The resources and callbacks below follow
 * arch/arm/plat-ambarella/generic/udc.c and uport.c (S2, non-HAL paths).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/delay.h>

#include <mach/hardware.h>
#include <plat/udc.h>

static struct resource h4_udc_resources[] = {
	[0] = {
		.start	= USBDC_BASE,
		.end	= USBDC_BASE + 0x1FFF,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start	= USBC_IRQ,
		.end	= USBC_IRQ,
		.flags	= IORESOURCE_IRQ,
	},
};

static void h4_enable_phy(void)
{
	amba_setbitsl(ANA_PWR_REG, 0x6);	/* always on */
}

/* The RTOS shares this PHY; never power it down behind its back. */
static void h4_disable_phy(void)
{
}

static void h4_reset_usb(void)
{
	u32 val;

	val = amba_rct_readl(ANA_PWR_REG);
	/* force usbphy on */
	amba_rct_writel(ANA_PWR_REG, val | 0x4);
	udelay(1);
	/* UDC soft reset */
	amba_rct_setbitsl(USB_REFCLK_REG, 0x20000000);
	udelay(1);
	amba_rct_clrbitsl(USB_REFCLK_REG, 0x20000000);
	udelay(1);
	/* restore ana_pwr_reg */
	amba_rct_writel(ANA_PWR_REG, val);
	udelay(1);
}

static int h4_flush_rxfifo(void)
{
	int retry_count = 1000;
	int rval = 0;

	amba_setbitsl(USB_DEV_CTRL_REG, USB_DEV_NAK);
	amba_setbitsl(USB_DEV_CTRL_REG, USB_DEV_FLUSH_RXFIFO);
	while (!(amba_readl(USB_DEV_STS_REG) & USB_DEV_RXFIFO_EMPTY_STS)) {
		if (retry_count-- < 0) {
			pr_err("%s: failed", __func__);
			rval = -1;
			break;
		}
		udelay(5);
	}
	amba_clrbitsl(USB_DEV_CTRL_REG, USB_DEV_NAK);
	return rval;
}

static struct ambarella_udc_controller h4_udc_controller = {
	.vbus_polled	= 1,
	.irqflags	= IRQF_DISABLED | IRQF_TRIGGER_HIGH,
#if defined(CONFIG_PLAT_AMBARELLA_S2_CORTEX)
	.dma_fix	= 0xC0000000,
#else
	.dma_fix	= 0,
#endif
	.enable_phy	= h4_enable_phy,
	.disable_phy	= h4_disable_phy,
	.reset_usb	= h4_reset_usb,
	.flush_rxfifo	= h4_flush_rxfifo,
};

static u64 h4_udc_dmamask = DMA_BIT_MASK(32);

static void h4_udc_release(struct device *dev)
{
}

static struct platform_device h4_udc_device = {
	.name		= "ambarella-udc",
	.id		= -1,
	.resource	= h4_udc_resources,
	.num_resources	= ARRAY_SIZE(h4_udc_resources),
	.dev		= {
		.platform_data		= &h4_udc_controller,
		.dma_mask		= &h4_udc_dmamask,
		.coherent_dma_mask	= DMA_BIT_MASK(32),
		.release		= h4_udc_release,
	}
};

static int __init h4_udc_dev_init(void)
{
	pr_info("h4_udc_dev: ANA_PWR=0x%08x USB_REFCLK=0x%08x DEV_CTRL=0x%08x DEV_STS=0x%08x\n",
		amba_rct_readl(ANA_PWR_REG), amba_rct_readl(USB_REFCLK_REG),
		amba_readl(USB_DEV_CTRL_REG), amba_readl(USB_DEV_STS_REG));
	return platform_device_register(&h4_udc_device);
}

static void __exit h4_udc_dev_exit(void)
{
	platform_device_unregister(&h4_udc_device);
}

module_init(h4_udc_dev_init);
module_exit(h4_udc_dev_exit);

MODULE_DESCRIPTION("ambarella-udc platform device for GoPro HERO4 Black");
MODULE_LICENSE("GPL");
