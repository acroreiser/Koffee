/*
 * Copyright (C) ST-Ericsson SA 2010
 *
 * ST-Ericsson MCDE Sony acx424akp DCS display driver
 *
 * Author: Marcus Lorentzon <marcus.xm.lorentzon@stericsson.com>
 * for ST-Ericsson.
 *
 * License terms: GNU General Public License (GPL), version 2.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/err.h>
#include <linux/slab.h>

#include <linux/regulator/consumer.h>

#include <video/mcde_display.h>
#include <video/mcde_display-sony_acx424akp_dsi.h>

#define RESET_DELAY_MS		11
#define RESET_LOW_DELAY_US	20
#define SLEEP_OUT_DELAY_MS	140
#define SLEEP_IN_DELAY_MS	85	/* Assume 60 Hz 5 frames */
#define IO_REGU			"vddi"
#define IO_REGU_MIN		1600000
#define IO_REGU_MAX		3300000

#define DSI_HS_FREQ_HZ		420160000
#define DSI_LP_FREQ_HZ		19200000

struct device_info {
	int reset_gpio;
	struct mcde_port port;
	struct regulator *regulator;
};

static inline struct device_info *get_drvdata(struct mcde_display_device *ddev)
{
	return (struct device_info *)dev_get_drvdata(&ddev->dev);
}

static int display_read_deviceid(struct mcde_display_device *dev, u16 *id)
{
	struct mcde_chnl_state *chnl;

	u8  id1, id2, id3;
	int len = 1;
	int ret = 0;
	int readret = 0;

	dev_dbg(&dev->dev, "%s: Read device id of the display\n", __func__);

	/* Acquire MCDE resources */
	chnl = mcde_chnl_get(dev->chnl_id, dev->fifo, dev->port);
	if (IS_ERR(chnl)) {
		ret = PTR_ERR(chnl);
		dev_warn(&dev->dev, "Failed to acquire MCDE channel\n");
		goto out;
	}

	/* plugnplay: use registers DA, DBh and DCh to detect display */
	readret = mcde_dsi_dcs_read(chnl, 0xDA, (u32 *)&id1, &len);
	if (!readret)
		readret = mcde_dsi_dcs_read(chnl, 0xDB, (u32 *)&id2, &len);
	if (!readret)
		readret = mcde_dsi_dcs_read(chnl, 0xDC, (u32 *)&id3, &len);

	if (readret) {
		dev_info(&dev->dev,
			"mcde_dsi_dcs_read failed to read display ID\n");
		goto read_fail;
	}

	*id = (id3 << 8) | id2;
read_fail:
	/* close  MCDE channel */
	mcde_chnl_put(chnl);
out:
	return 0;
}

static int power_on(struct mcde_display_device *dev)
{
	struct device_info *di = get_drvdata(dev);

	dev_dbg(&dev->dev, "%s: Reset & power on sony display\n", __func__);

	regulator_enable(di->regulator);
	gpio_set_value_cansleep(di->reset_gpio, 1);
	msleep(RESET_DELAY_MS);
	gpio_set_value_cansleep(di->reset_gpio, 0);
	udelay(RESET_LOW_DELAY_US);
	gpio_set_value_cansleep(di->reset_gpio, 1);
	msleep(RESET_DELAY_MS);

	return 0;
}

static int power_off(struct mcde_display_device *dev)
{
	struct device_info *di = get_drvdata(dev);

	dev_dbg(&dev->dev, "%s:Reset & power off sony display\n", __func__);

	gpio_set_value_cansleep(di->reset_gpio, 0);
	msleep(RESET_DELAY_MS);
	regulator_disable(di->regulator);

	return 0;
}

static int display_on(struct mcde_display_device *ddev)
{
	int ret;
	u8 val = 0;

	dev_dbg(&ddev->dev, "Display on sony display\n");

	ret = mcde_dsi_dcs_write(ddev->chnl_state,
						DCS_CMD_SET_TEAR_ON, &val, 1);
	if (ret)
		dev_warn(&ddev->dev,
			"%s:Failed to enable synchronized update\n", __func__);

	ret = mcde_dsi_dcs_write(ddev->chnl_state, DCS_CMD_EXIT_SLEEP_MODE,
								NULL, 0);
	if (ret)
		return ret;
	msleep(SLEEP_OUT_DELAY_MS);
	return mcde_dsi_dcs_write(ddev->chnl_state, DCS_CMD_SET_DISPLAY_ON,
								NULL, 0);
}

static int display_off(struct mcde_display_device *ddev)
{
	int ret;

	dev_dbg(&ddev->dev, "Display off sony display\n");

	ret = mcde_dsi_dcs_write(ddev->chnl_state, DCS_CMD_SET_DISPLAY_OFF,
								NULL, 0);
	if (ret)
		return ret;

	ret = mcde_dsi_dcs_write(ddev->chnl_state, DCS_CMD_ENTER_SLEEP_MODE,
								NULL, 0);
	/* Wait for 4 frames or more */
	msleep(SLEEP_IN_DELAY_MS);

	return ret;
}

static int sony_acx424akp_set_scan_mode(struct mcde_display_device *ddev,
		enum mcde_display_power_mode power_mode)
{
	int ret = 0;
	u8 param[MCDE_MAX_DSI_DIRECT_CMD_WRITE];

	dev_dbg(&ddev->dev, "%s:Set Power mode\n", __func__);

	/* 180 rotation for SONY ACX424AKP display */
	if (ddev->power_mode == MCDE_DISPLAY_PM_STANDBY) {
		param[0] = 0xAA;
		ret = mcde_dsi_dcs_write(ddev->chnl_state, 0xf3, param, 1);
		if (ret)
			return ret;

		param[0] = 0x00;
		param[1] = 0x00;
		ret = mcde_dsi_generic_write(ddev->chnl_state, param, 3);
		if (ret)
			return ret;

		param[0] = 0xC9;
		param[1] = 0x01;
		ret = mcde_dsi_generic_write(ddev->chnl_state, param, 3);
		if (ret)
			return ret;

		param[0] = 0xA2;
		param[1] = 0x00;
		ret = mcde_dsi_generic_write(ddev->chnl_state, param, 3);
		if (ret)
			return ret;

		param[0] = 0xFF;
		param[1] = 0xAA;
		ret = mcde_dsi_generic_write(ddev->chnl_state, param, 3);
		if (ret)
			return ret;
	}
	return ret;
}

static int sony_acx424akp_set_power_mode(struct mcde_display_device *ddev,
	enum mcde_display_power_mode power_mode)
{
	int ret = 0;

	dev_dbg(&ddev->dev, "%s:Set Power mode\n", __func__);

	/* OFF -> STANDBY */
	if (ddev->power_mode == MCDE_DISPLAY_PM_OFF &&
					power_mode != MCDE_DISPLAY_PM_OFF) {
		ret = power_on(ddev);
		if (ret)
			return ret;
		ddev->power_mode = MCDE_DISPLAY_PM_STANDBY;
	}

	/* STANDBY -> ON */
	if (ddev->power_mode == MCDE_DISPLAY_PM_STANDBY &&
					power_mode == MCDE_DISPLAY_PM_ON) {

		ret = display_on(ddev);
		if (ret)
			return ret;
		ddev->power_mode = MCDE_DISPLAY_PM_ON;
	}
	/* ON -> STANDBY */
	else if (ddev->power_mode == MCDE_DISPLAY_PM_ON &&
					power_mode <= MCDE_DISPLAY_PM_STANDBY) {

		ret = display_off(ddev);
		if (ret)
			return ret;
		ddev->power_mode = MCDE_DISPLAY_PM_STANDBY;
	}

	/* STANDBY -> OFF */
	if (ddev->power_mode == MCDE_DISPLAY_PM_STANDBY &&
					power_mode == MCDE_DISPLAY_PM_OFF) {
		ret = power_off(ddev);
		if (ret)
			return ret;
		ddev->power_mode = MCDE_DISPLAY_PM_OFF;
	}

	return mcde_chnl_set_power_mode(ddev->chnl_state, ddev->power_mode);
}

static int __devinit sony_acx424akp_probe(struct mcde_display_device *dev)
{
	int ret = 0;
	u16 id = 0;
	struct device_info *di;
	struct mcde_port *port;
	struct mcde_display_sony_acx424akp_platform_data *pdata =
		dev->dev.platform_data;

	if (pdata == NULL || !pdata->reset_gpio) {
		dev_err(&dev->dev, "Invalid platform data\n");
		return -EINVAL;
	}

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	port = dev->port;
	di->reset_gpio = pdata->reset_gpio;
	di->port.type = MCDE_PORTTYPE_DSI;
	di->port.mode = MCDE_PORTMODE_CMD;
	di->port.pixel_format = MCDE_PORTPIXFMT_DSI_24BPP;
	di->port.sync_src = dev->port->sync_src;
	di->port.frame_trig = dev->port->frame_trig;
	di->port.phy.dsi.num_data_lanes = 2;
	di->port.link = port->link;
	di->port.phy.dsi.host_eot_gen = true;
	/* TODO: Move UI to mcde_hw.c when clk_get_rate(dsi) is done */
	di->port.phy.dsi.ui = 9;
	di->port.phy.dsi.hs_freq = DSI_HS_FREQ_HZ;
	di->port.phy.dsi.lp_freq = DSI_LP_FREQ_HZ;

	ret = gpio_request(di->reset_gpio, NULL);
	if (WARN_ON(ret))
		goto gpio_request_failed;

	gpio_direction_output(di->reset_gpio, 1);
	di->regulator = regulator_get(&dev->dev, IO_REGU);
	if (IS_ERR(di->regulator)) {
		ret = PTR_ERR(di->regulator);
		di->regulator = NULL;
		goto regulator_get_failed;
	}
	ret = regulator_set_voltage(di->regulator, IO_REGU_MIN, IO_REGU_MAX);
	if (WARN_ON(ret))
		goto regulator_voltage_failed;

	dev->set_power_mode = sony_acx424akp_set_power_mode;

	dev->port = &di->port;
	dev->native_x_res = 480;
	dev->native_y_res = 854;
	dev_set_drvdata(&dev->dev, di);

	/*
	* When u-boot has display a startup screen.
	* U-boot has turned on display power however the
	* regulator framework does not know about that
	* This is the case here, the display driver has to
	* enable the regulator for the display.
	*/
	if (dev->power_mode != MCDE_DISPLAY_PM_OFF) {
		(void) regulator_enable(di->regulator);
	} else {
		power_on(dev);
		dev->power_mode = MCDE_DISPLAY_PM_STANDBY;
	}

	ret = display_read_deviceid(dev, &id);
	if (ret)
		goto read_id_failed;

	switch (id) {
	case DISPLAY_SONY_ACX424AKP:
	case DISPLAY_SONY_ACX424AKP_ID2:
		pdata->disp_panel = id;
		dev_info(&dev->dev,
			"Sony ACX424AKP display (ID 0x%.4X) probed\n", id);
		break;
	default:
		pdata->disp_panel = DISPLAY_NONE;
		dev_info(&dev->dev,
			"Display not recognized (ID 0x%.4X) probed\n", id);
		goto read_id_failed;
	}

	return 0;

read_id_failed:
regulator_voltage_failed:
	regulator_put(di->regulator);
regulator_get_failed:
	gpio_free(di->reset_gpio);
gpio_request_failed:
	kfree(di);
	return ret;
}

static int __devexit sony_acx424akp_remove(struct mcde_display_device *dev)
{
	struct device_info *di = get_drvdata(dev);

	dev->set_power_mode(dev, MCDE_DISPLAY_PM_OFF);

	regulator_put(di->regulator);
	gpio_direction_input(di->reset_gpio);
	gpio_free(di->reset_gpio);

	kfree(di);

	return 0;
}

#if !defined(CONFIG_HAS_EARLYSUSPEND) && defined(CONFIG_PM)
static int sony_acx424akp_resume(struct mcde_display_device *ddev)
{
	int ret;

	/* set_power_mode will handle call platform_enable */
	ret = ddev->set_power_mode(ddev, MCDE_DISPLAY_PM_STANDBY);
	if (ret < 0)
		dev_warn(&ddev->dev, "%s:Failed to resume display\n"
			, __func__);
	return ret;
}

static int sony_acx424akp_suspend(struct mcde_display_device *ddev, \
							pm_message_t state)
{
	int ret;

	/* set_power_mode will handle call platform_disable */
	ret = ddev->set_power_mode(ddev, MCDE_DISPLAY_PM_OFF);
	if (ret < 0)
		dev_warn(&ddev->dev, "%s:Failed to suspend display\n"
			, __func__);
	return ret;
}
#endif

static struct mcde_display_driver sony_acx424akp_driver = {
	.probe	= sony_acx424akp_probe,
	.remove = sony_acx424akp_remove,
#if !defined(CONFIG_HAS_EARLYSUSPEND) && defined(CONFIG_PM)
	.suspend = sony_acx424akp_suspend,
	.resume = sony_acx424akp_resume,
#else
	.suspend = NULL,
	.resume = NULL,
#endif
	.driver = {
		.name	= "mcde_disp_sony_acx424akp",
	},
};

/* Module init */
static int __init mcde_display_sony_acx424akp_init(void)
{
	pr_info("%s\n", __func__);

	return mcde_display_driver_register(&sony_acx424akp_driver);
}
module_init(mcde_display_sony_acx424akp_init);

static void __exit mcde_display_sony_acx424akp_exit(void)
{
	pr_info("%s\n", __func__);

	mcde_display_driver_unregister(&sony_acx424akp_driver);
}
module_exit(mcde_display_sony_acx424akp_exit);

MODULE_AUTHOR("Marcus Lorentzon <marcus.xm.lorentzon@stericsson.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ST-Ericsson MCDE Sony ACX424AKP DCS display driver");
