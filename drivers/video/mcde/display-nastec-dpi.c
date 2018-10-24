/*
 * Copyright (C) ST-Ericsson SA 2010
 *
 * ST-Ericsson MCDE DPI display driver
 * The VUIB500 is an user interface board the can be attached to an HREF. It
 * supports the DPI pixel interface and converts this to an analog VGA signal,
 * which can be connected to a monitor using a DSUB connector. The VUIB board
 * uses an external power supply of 5V.
 *
 * Author: Marcel Tunnissen <marcel.tuennissen@stericsson.com>
 * for ST-Ericsson.
 *
 * License terms: GNU General Public License (GPL), version 2.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio.h>

#include <video/mcde_display.h>
#include <video/mcde_display-nastec-dpi.h>

#define DPI_DISP_TRACE	dev_dbg(&ddev->dev, "%s\n", __func__)

static int try_video_mode(struct mcde_display_device *ddev,
				struct mcde_video_mode *video_mode);
static int set_video_mode(struct mcde_display_device *ddev,
				struct mcde_video_mode *video_mode);
static int dpi_request_regulators(struct mcde_display_device *ddev);
static int dpi_enable_regulators(struct mcde_display_device *ddev);
static int dpi_disable_regulators(struct mcde_display_device *ddev);

static int __devinit dpi_display_probe(struct mcde_display_device *ddev)
{
	int ret = 0;
	struct mcde_display_dpi_platform_data *pdata = ddev->dev.platform_data;
	DPI_DISP_TRACE;

	if (pdata == NULL) {
		dev_err(&ddev->dev, "%s:Platform data missing\n", __func__);
		ret = -EINVAL;
		goto no_pdata;
	}

	if (ddev->port->type != MCDE_PORTTYPE_DPI) {
		dev_err(&ddev->dev,
			"%s:Invalid port type %d\n",
			__func__, ddev->port->type);
		ret = -EINVAL;
		goto invalid_port_type;
	}

	ret = dpi_request_regulators(ddev);
	if (ret)
		goto out;

	ret = dpi_enable_regulators(ddev);
	if (ret)
		goto out;

	ddev->try_video_mode = try_video_mode;
	ddev->set_video_mode = set_video_mode;
	dev_info(&ddev->dev, "DPI display probed\n");

	goto out;
invalid_port_type:
no_pdata:
out:
	return ret;
}

/* Dpi lcd has, 4 power sources, namely: 3.3V, 1.8V,
 * VLED boost & VLED, in designs where these are not
 * always-on we need to request regulators to be
 * turned on, to get the lcd backlight and lcd working
 */
static int dpi_request_regulators(struct mcde_display_device *ddev)
{
	int ret = 0;
	struct mcde_display_dpi_platform_data *pdata = ddev->dev.platform_data;
	/* Request regulators */
	pdata->supplies[0].supply = "lcd3v3";
	pdata->supplies[1].supply = "lcd1v8";
	pdata->supplies[2].supply = "lcdvledboost";
	pdata->supplies[3].supply = "lcdvled";

	ret = regulator_bulk_get(&ddev->dev,
					ARRAY_SIZE(pdata->supplies),
					pdata->supplies);
	if (ret) {
			dev_err(&ddev->dev, "%s:couldn't get regulators %d\n",
					__func__, ret);
			return ret;
	}
	return ret;
}

static void dpi_free_regulators(struct mcde_display_device *ddev)
{
	struct mcde_display_dpi_platform_data *pdata = ddev->dev.platform_data;
	/* Free regulators */
	regulator_bulk_free(ARRAY_SIZE(pdata->supplies),
			pdata->supplies);
}

static int dpi_enable_regulators(struct mcde_display_device *ddev)
{
	int ret = 0;
	struct mcde_display_dpi_platform_data *pdata = ddev->dev.platform_data;
	ret = regulator_bulk_enable(ARRAY_SIZE(pdata->supplies),
					pdata->supplies);
	if (ret) {
			dev_err(&ddev->dev, "%s:failed to enable regulators %d\n",
					__func__, ret);
			return ret;
	}

	return ret;
}

static int dpi_disable_regulators(struct mcde_display_device *ddev)
{
	int ret = 0;
	struct mcde_display_dpi_platform_data *pdata = ddev->dev.platform_data;
	ret = regulator_bulk_disable(ARRAY_SIZE(pdata->supplies),
					pdata->supplies);
	if (ret) {
			dev_err(&ddev->dev, "%s:failed to enable regulators %d\n",
					__func__, ret);
			return ret;
	}

	return ret;
}

static int __devexit dpi_display_remove(struct mcde_display_device *ddev)
{
	DPI_DISP_TRACE;

	ddev->set_power_mode(ddev, MCDE_DISPLAY_PM_OFF);
	return 0;
}

static int dpi_display_resume(struct mcde_display_device *ddev)
{
	int ret;
	DPI_DISP_TRACE;

	/* set_power_mode will handle call platform_enable */
	ret = ddev->set_power_mode(ddev, MCDE_DISPLAY_PM_STANDBY);
	if (ret < 0)
		dev_warn(&ddev->dev, "%s:Failed to resume display\n"
			, __func__);
	return ret;
}

static int dpi_display_suspend(struct mcde_display_device *ddev,
							pm_message_t state)
{
	int ret;
	DPI_DISP_TRACE;

	/* set_power_mode will handle call platform_disable */
	ret = ddev->set_power_mode(ddev, MCDE_DISPLAY_PM_OFF);
	if (ret < 0)
		dev_warn(&ddev->dev, "%s:Failed to suspend display\n"
			, __func__);
	return ret;
}

static void print_vmode(struct mcde_video_mode *vmode)
{
	pr_debug("resolution: %dx%d\n", vmode->xres, vmode->yres);
	pr_debug("  pixclock: %d\n",    vmode->pixclock);
	pr_debug("       hbp: %d\n",    vmode->hbp);
	pr_debug("       hfp: %d\n",    vmode->hfp);
	pr_debug("       hsw: %d\n",    vmode->hsw);
	pr_debug("       vbp: %d\n",    vmode->vbp);
	pr_debug("       vfp: %d\n",    vmode->vfp);
	pr_debug("       vsw: %d\n",    vmode->vsw);
	pr_debug("interlaced: %s\n", vmode->interlaced ? "true" : "false");
}

/* Taken from the programmed value of the LCD clock in PRCMU */
#define PIX_CLK_FREQ		66560000
#define VMODE_XRES		1280
#define VMODE_YRES		800

static int try_video_mode(
	struct mcde_display_device *ddev, struct mcde_video_mode *video_mode)
{
	int res = -EINVAL;
	DPI_DISP_TRACE;

	if (ddev == NULL || video_mode == NULL) {
		dev_warn(&ddev->dev, "%s:ddev = NULL or video_mode = NULL\n",
			__func__);
		return res;
	}

	print_vmode(video_mode);

	if (video_mode->xres == VMODE_XRES && video_mode->yres == VMODE_YRES) {
		video_mode->hbp = 48;
		video_mode->hfp = 8;
		video_mode->hsw = 104;	/* hbp+hfp+hsw = 160 */
		video_mode->vbp = 19;
		video_mode->vfp = 2;
		video_mode->vsw = 2;	/* vbp+vfp+vsw = 23 */
		/*
		 * The pixclock setting is not used within MCDE. The clock is
		 * setup elsewhere. But the pixclock value is visible in user
		 * space.
		 */
		video_mode->pixclock =	(int) (1e+12 * (1.0 / PIX_CLK_FREQ));
		res = 0;
	} /* TODO: add more supported resolutions here */
	video_mode->interlaced = false;

	if (res == 0)
		print_vmode(video_mode);
	else
		dev_warn(&ddev->dev,
			"%s:Failed to find video mode x=%d, y=%d\n",
			__func__, video_mode->xres, video_mode->yres);

	return res;

}

static int set_video_mode(
	struct mcde_display_device *ddev, struct mcde_video_mode *video_mode)
{
	int res;
	DPI_DISP_TRACE;

	if (ddev == NULL || video_mode == NULL) {
		dev_warn(&ddev->dev, "%s:ddev = NULL or video_mode = NULL\n",
			__func__);
		return -EINVAL;
	}
	if (video_mode->xres != VMODE_XRES || video_mode->yres != VMODE_YRES) {
		dev_warn(&ddev->dev, "%s:Failed to set video mode x=%d, y=%d\n",
			__func__, video_mode->xres, video_mode->yres);
		return -EINVAL;
	}
	ddev->video_mode = *video_mode;
	print_vmode(video_mode);

	res = mcde_chnl_set_video_mode(ddev->chnl_state, &ddev->video_mode);
	if (res < 0) {
		dev_warn(&ddev->dev, "%s:Failed to set video mode on channel\n",
			__func__);

	}
	/* notify mcde display driver about updated video mode */
	ddev->update_flags |= UPDATE_FLAG_VIDEO_MODE;
	return res;
}

static struct mcde_display_driver dpi_display_driver = {
	.probe	= dpi_display_probe,
	.remove = dpi_display_remove,
	.suspend = dpi_display_suspend,
	.resume = dpi_display_resume,
	.driver = {
		.name	= "mcde_display_dpi",
	},
};

/* Module init */
static int __init mcde_dpi_display_init(void)
{
	pr_info("%s\n", __func__);

	return mcde_display_driver_register(&dpi_display_driver);
}
module_init(mcde_dpi_display_init);

static void __exit mcde_dpi_display_exit(void)
{
	pr_info("%s\n", __func__);

	mcde_display_driver_unregister(&dpi_display_driver);
}
module_exit(mcde_dpi_display_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Gregory Hermant <gregory.hermant@calao-systems.com>");
MODULE_DESCRIPTION("ST-Ericsson MCDE DPI display driver for NASTECH display");
