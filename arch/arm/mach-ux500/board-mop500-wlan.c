/*
 * Copyright (C) ST-Ericsson SA 2011
 *
 * Author: Dmitry Tarnyagin <dmitry.tarnyagin@stericsson.com>
 * License terms: GNU General Public License (GPL) version 2
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <asm/mach-types.h>
#include <mach/irqs.h>
#include <plat/pincfg.h>
#include <linux/clk.h>
#include <mach/cw1200_plat.h>

#include "pins.h"

static void cw1200_release(struct device *dev);
static int cw1200_power_ctrl(const struct cw1200_platform_data *pdata,
			     bool enable);
static int cw1200_clk_ctrl(const struct cw1200_platform_data *pdata,
		bool enable);

static struct resource cw1200_href_resources[] = {
	{
		.start = 215,
		.end = 215,
		.flags = IORESOURCE_IO,
		.name = "cw1200_reset",
	},
	{
		.start = NOMADIK_GPIO_TO_IRQ(216),
		.end = NOMADIK_GPIO_TO_IRQ(216),
		.flags = IORESOURCE_IRQ,
		.name = "cw1200_irq",
	},
};

static struct resource cw1200_href60_resources[] = {
	{
		.start = 85,
		.end = 85,
		.flags = IORESOURCE_IO,
		.name = "cw1200_reset",
	},
	{
		.start = NOMADIK_GPIO_TO_IRQ(4),
		.end = NOMADIK_GPIO_TO_IRQ(4),
		.flags = IORESOURCE_IRQ,
		.name = "cw1200_irq",
	},
};

static struct resource cw1200_u9500_resources[] = {
	{
		.start = 85,
		.end = 85,
		.flags = IORESOURCE_IO,
		.name = "cw1200_reset",
	},
	{
		.start = NOMADIK_GPIO_TO_IRQ(144),
		.end = NOMADIK_GPIO_TO_IRQ(144),
		.flags = IORESOURCE_IRQ,
		.name = "cw1200_irq",
	},
};

static struct cw1200_platform_data cw1200_platform_data = {
	.clk_ctrl = cw1200_clk_ctrl,
};

static struct platform_device cw1200_device = {
	.name = "cw1200_wlan",
	.dev = {
		.platform_data = &cw1200_platform_data,
		.release = cw1200_release,
		.init_name = "cw1200_wlan",
	},
};

const struct cw1200_platform_data *cw1200_get_platform_data(void)
{
	return &cw1200_platform_data;
}
EXPORT_SYMBOL_GPL(cw1200_get_platform_data);

static int cw1200_pins_enable(bool enable)
{
	struct ux500_pins *pins = NULL;
	int ret = 0;

	pins = ux500_pins_get("sdi1");

	if (!pins) {
		printk(KERN_ERR "cw1200: Pins are not found. "
				"Check platform data.\n");
		return -ENOENT;
	}

	if (enable)
		ret = ux500_pins_enable(pins);
	else
		ret = ux500_pins_disable(pins);

	if (ret)
		printk(KERN_ERR "cw1200: Pins can not be %s: %d.\n",
				enable ? "enabled" : "disabled",
				ret);

	ux500_pins_put(pins);

	return ret;
}

static int cw1200_power_ctrl(const struct cw1200_platform_data *pdata,
		bool enable)
{
	static const char *vdd_name = "vdd";
	struct regulator *vdd;
	int ret = 0;

	vdd = regulator_get(&cw1200_device.dev, vdd_name);
	if (IS_ERR(vdd)) {
		ret = PTR_ERR(vdd);
		dev_warn(&cw1200_device.dev,
				"%s: Failed to get regulator '%s': %d\n",
				__func__, vdd_name, ret);
	} else {
		if (enable)
			ret = regulator_enable(vdd);
		else
			ret = regulator_disable(vdd);

		if (ret) {
			dev_warn(&cw1200_device.dev,
					"%s: Failed to %s regulator '%s': %d\n",
					__func__, enable ? "enable" : "disable",
					vdd_name, ret);
		}
		regulator_put(vdd);
	}
	return  ret;
}

static int cw1200_clk_ctrl(const struct cw1200_platform_data *pdata,
		bool enable)
{
	static const char *clock_name = "sys_clk_out";
	struct clk *clk_dev;
	int ret = 0;

	clk_dev = clk_get(&cw1200_device.dev, clock_name);

	if (IS_ERR(clk_dev)) {
		ret = PTR_ERR(clk_dev);
		dev_warn(&cw1200_device.dev,
				"%s: Failed to get clk '%s': %d\n",
				__func__, clock_name, ret);

	} else {

		if (enable)
			ret = clk_enable(clk_dev);
		else
			clk_disable(clk_dev);

		if (ret) {
			dev_warn(&cw1200_device.dev,
					"%s: Failed to %s clk enable: %d\n",
					__func__, clock_name, ret);
		}
	}

	return ret;
}

int __init mop500_wlan_init(struct device *parent)
{
	int ret;

	if (pins_for_u9500()) {
		cw1200_device.num_resources = ARRAY_SIZE(cw1200_u9500_resources);
		cw1200_device.resource = cw1200_u9500_resources;
	} else if (machine_is_u8500() || machine_is_nomadik() || machine_is_snowball()) {
		cw1200_device.num_resources = ARRAY_SIZE(cw1200_href_resources);
		cw1200_device.resource = cw1200_href_resources;
	} else if (machine_is_hrefv60() || machine_is_u8520()
			 || machine_is_u9540()) {
		cw1200_device.num_resources =
				ARRAY_SIZE(cw1200_href60_resources);
		cw1200_device.resource = cw1200_href60_resources;
	} else {
		dev_err(&cw1200_device.dev,
				"Unsupported mach type %d "
				"(check mach-types.h)\n",
				__machine_arch_type);
		return -ENOTSUPP;
	}

	if (machine_is_snowball())
		cw1200_platform_data.mmc_id = "mmc2";
	else
		cw1200_platform_data.mmc_id = "mmc3";

	cw1200_platform_data.reset = &cw1200_device.resource[0];
	cw1200_platform_data.irq = &cw1200_device.resource[1];

	cw1200_device.dev.release = cw1200_release;

	if (machine_is_snowball())
		cw1200_platform_data.power_ctrl = cw1200_power_ctrl;

	ret = cw1200_pins_enable(true);
	if (WARN_ON(ret))
		return ret;

	cw1200_device.dev.parent = parent;
	ret = platform_device_register(&cw1200_device);
	if (ret)
		cw1200_pins_enable(false);

	return ret;
}

static void cw1200_release(struct device *dev)
{
	cw1200_pins_enable(false);
}
