/*
 * Copyright (C) ST-Ericsson SA 2010
 *
 * AB8500 display driver
 *
 * Author: Marcel Tunnissen <marcel.tuennissen@stericsson.com>
 * for ST-Ericsson.
 *
 * License terms: GNU General Public License (GPL), version 2.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/mfd/ab8500/denc.h>
#include <video/mcde_display.h>
#include <video/mcde_display-ab8500.h>

#define AB8500_DISP_TRACE	dev_dbg(&ddev->dev, "%s\n", __func__)

#define SDTV_PIXCLOCK		37037

/*
 * PAL:
 * Total nr of active lines:   576
 * Total nr of blanking lines:  49
 *                      total: 625
 */
#define PAL_HBP			132
#define PAL_HFP			12
#define PAL_VBP_FIELD_1		22
#define PAL_VBP_FIELD_2		23
#define PAL_VFP_FIELD_1		2
#define PAL_VFP_FIELD_2		2

/*
 * NTSC (ITU-R BT.470-5):
 * Total nr of active lines:   486
 * Total nr of blanking lines:  39
 *                      total: 525
 */
#define NTSC_ORG_HBP		122
#define NTSC_ORG_HFP		16
#define NTSC_ORG_VBP_FIELD_1	16
#define NTSC_ORG_VBP_FIELD_2	17
#define NTSC_ORG_VFP_FIELD_1	3
#define NTSC_ORG_VFP_FIELD_2	3

/*
 * NTSC (DV variant):
 * Total nr of active lines:   480
 * Total nr of blanking lines:  45
 *                      total: 525
 */
#define NTSC_HBP		122
#define NTSC_HFP		16
#define NTSC_VBP_FIELD_1	19
#define NTSC_VBP_FIELD_2	20
#define NTSC_VFP_FIELD_1	3
#define NTSC_VFP_FIELD_2	3

struct display_driver_data {
	struct ab8500_denc_conf denc_conf;
	struct platform_device *denc_dev;
	int nr_regulators;
	struct regulator **regulator;
};

static int try_video_mode(struct mcde_display_device *ddev,
				struct mcde_video_mode *video_mode);
static int set_video_mode(struct mcde_display_device *ddev,
				struct mcde_video_mode *video_mode);
static int set_power_mode(struct mcde_display_device *ddev,
				enum mcde_display_power_mode power_mode);
static int on_first_update(struct mcde_display_device *ddev);
static int display_update(struct mcde_display_device *ddev,
							bool tripple_buffer);

static int __devinit ab8500_probe(struct mcde_display_device *ddev)
{
	int ret = 0;
	int i;
	struct ab8500_display_platform_data *pdata = ddev->dev.platform_data;
	struct display_driver_data *driver_data;

	AB8500_DISP_TRACE;

	if (pdata == NULL) {
		dev_err(&ddev->dev, "%s:Platform data missing\n", __func__);
		return -EINVAL;
	}
	if (ddev->port->type != MCDE_PORTTYPE_DPI) {
		dev_err(&ddev->dev, "%s:Invalid port type %d\n", __func__,
							ddev->port->type);
		return -EINVAL;
	}

	driver_data = (struct display_driver_data *)
		kzalloc(sizeof(struct display_driver_data), GFP_KERNEL);
	if (!driver_data) {
		dev_err(&ddev->dev, "Failed to allocate driver data\n");
		return -ENOMEM;
	}
	driver_data->denc_dev = ab8500_denc_get_device();
	if (!driver_data->denc_dev) {
		dev_err(&ddev->dev, "Failed to get DENC device\n");
		ret = -ENODEV;
		goto dev_get_failed;
	}

	driver_data->regulator = kzalloc(pdata->nr_regulators *
					sizeof(struct regulator *), GFP_KERNEL);
	if (!driver_data->regulator) {
		dev_err(&ddev->dev, "Failed to allocate regulator list\n");
		ret = -ENOMEM;
		goto reg_alloc_failed;
	}
	for (i = 0; i < pdata->nr_regulators; i++) {
		driver_data->regulator[i] = regulator_get(&ddev->dev,
						pdata->regulator_id[i]);
		if (IS_ERR(driver_data->regulator[i])) {
			ret = PTR_ERR(driver_data->regulator[i]);
			dev_warn(&ddev->dev, "%s:Failed to get regulator %s\n",
					__func__, pdata->regulator_id[i]);
			goto regulator_get_failed;
		}
	}
	driver_data->nr_regulators = pdata->nr_regulators;

	dev_set_drvdata(&ddev->dev, driver_data);

	ddev->try_video_mode = try_video_mode;
	ddev->set_video_mode = set_video_mode;
	ddev->set_power_mode = set_power_mode;
	ddev->on_first_update = on_first_update;
	ddev->update = display_update;

	return 0;

regulator_get_failed:
	for (i--; i >= 0; i--)
		regulator_put(driver_data->regulator[i]);
	kfree(driver_data->regulator);
	driver_data->regulator = NULL;
reg_alloc_failed:
	ab8500_denc_put_device(driver_data->denc_dev);
dev_get_failed:
	kfree(driver_data);
	return ret;
}

static int __devexit ab8500_remove(struct mcde_display_device *ddev)
{
	struct display_driver_data *driver_data = dev_get_drvdata(&ddev->dev);
	AB8500_DISP_TRACE;

	ddev->set_power_mode(ddev, MCDE_DISPLAY_PM_OFF);

	if (driver_data->regulator) {
		int i;
		for (i = driver_data->nr_regulators - 1; i >= 0; i--)
			regulator_put(driver_data->regulator[i]);
		kfree(driver_data->regulator);
		driver_data->regulator = NULL;
		driver_data->nr_regulators = 0;
	}
	ab8500_denc_put_device(driver_data->denc_dev);
	kfree(driver_data);
	return 0;
}

static int ab8500_resume(struct mcde_display_device *ddev)
{
	int ret = 0;
	AB8500_DISP_TRACE;

	ret = ddev->set_power_mode(ddev, MCDE_DISPLAY_PM_STANDBY);
	if (ret < 0)
		dev_warn(&ddev->dev, "%s: Failed to resume display\n",
								__func__);

	return ret;
}

static int ab8500_suspend(struct mcde_display_device *ddev, pm_message_t state)
{
	int ret = 0;
	AB8500_DISP_TRACE;

	ret = ddev->set_power_mode(ddev, MCDE_DISPLAY_PM_OFF);
	if (ret < 0)
		dev_warn(&ddev->dev, "%s: Failed to suspend display\n",
								__func__);

	return ret;
}


static struct mcde_display_driver ab8500_driver = {
	.probe	= ab8500_probe,
	.remove = ab8500_remove,
	.suspend = ab8500_suspend,
	.resume = ab8500_resume,
	.driver = {
		.name	= "mcde_tv_ab8500",
	},
};

static void print_vmode(struct mcde_video_mode *vmode)
{
	pr_debug("resolution: %dx%d\n", vmode->xres, vmode->yres);
	pr_debug("  pixclock: %d\n",    vmode->pixclock);
	pr_debug("       hbp: %d\n",    vmode->hbp);
	pr_debug("       hfp: %d\n",    vmode->hfp);
	pr_debug("       vbp: %d\n",    vmode->vbp);
	pr_debug("       vfp: %d\n",    vmode->vfp);
	pr_debug("interlaced: %s\n", vmode->interlaced ? "true" : "false");
}

static int try_video_mode(
	struct mcde_display_device *ddev, struct mcde_video_mode *video_mode)
{
	AB8500_DISP_TRACE;

	if (ddev == NULL || video_mode == NULL) {
		dev_warn(&ddev->dev, "%s:ddev = NULL or video_mode = NULL\n",
			__func__);
		return -EINVAL;
	}

	if (video_mode->xres != 720) {
		dev_warn(&ddev->dev,
			"%s:Failed to find video mode x=%d, y=%d\n",
			__func__, video_mode->xres, video_mode->yres);
		return -EINVAL;
	}

	/* TODO: move this part to MCDE: mcde_dss_try_video_mode? */
	/* check for PAL */
	switch (video_mode->yres) {
	case 576:
		/* set including SAV/EAV: */
		video_mode->hbp = PAL_HBP;
		video_mode->hfp = PAL_HFP;
		video_mode->vbp = PAL_VBP_FIELD_1 + PAL_VBP_FIELD_2;
		video_mode->vfp = PAL_VFP_FIELD_1 + PAL_VFP_FIELD_2;
		video_mode->interlaced = true;
		video_mode->pixclock = SDTV_PIXCLOCK;
		break;
	case 480:
		/* set including SAV/EAV */
		video_mode->hbp = NTSC_HBP;
		video_mode->hfp = NTSC_HFP;
		video_mode->vbp = NTSC_VBP_FIELD_1 + NTSC_VBP_FIELD_2;
		video_mode->vfp = NTSC_VFP_FIELD_1 + NTSC_VFP_FIELD_2;
		video_mode->interlaced = true;
		video_mode->pixclock = SDTV_PIXCLOCK;
		break;
	case 486:
		/* set including SAV/EAV */
		video_mode->hbp = NTSC_ORG_HBP;
		video_mode->hfp = NTSC_ORG_HFP;
		video_mode->vbp = NTSC_ORG_VBP_FIELD_1 + NTSC_ORG_VBP_FIELD_2;
		video_mode->vfp = NTSC_ORG_VFP_FIELD_1 + NTSC_ORG_VFP_FIELD_2;
		video_mode->interlaced = true;
		video_mode->pixclock = SDTV_PIXCLOCK;
		break;
	default:
		dev_warn(&ddev->dev,
			"%s:Failed to find video mode x=%d, y=%d\n",
			__func__, video_mode->xres, video_mode->yres);
		return -EINVAL;
	}

	print_vmode(video_mode);

	return 0;

}

static int set_video_mode(
	struct mcde_display_device *ddev, struct mcde_video_mode *video_mode)
{
	int res;
	struct ab8500_display_platform_data *pdata = ddev->dev.platform_data;
	struct display_driver_data *driver_data =
		(struct display_driver_data *)dev_get_drvdata(&ddev->dev);
	AB8500_DISP_TRACE;

	if (ddev == NULL || video_mode == NULL) {
		dev_warn(&ddev->dev, "%s:ddev = NULL or video_mode = NULL\n",
			__func__);
		return -EINVAL;
	}
	ddev->video_mode = *video_mode;

	if (video_mode->xres != 720) {
		dev_warn(&ddev->dev, "%s:Failed to set video mode x=%d, y=%d\n",
			__func__, video_mode->xres, video_mode->yres);
		return -EINVAL;
	}

	/* check for PAL BDGHI and N */
	switch (video_mode->yres) {
	case 576:
		driver_data->denc_conf.TV_std = TV_STD_PAL_BDGHI;
		/* TODO: how to choose LOW DEF FILTER */
		driver_data->denc_conf.cr_filter = TV_CR_PAL_HIGH_DEF_FILTER;
		/* TODO: PAL N (e.g. uses a setup of 7.5 IRE) */
		driver_data->denc_conf.black_level_setup = false;
		break;
	case 480: /* NTSC, PAL M DV variant */
	case 486: /* NTSC, PAL M original   */
		/* TODO: PAL M */
		driver_data->denc_conf.TV_std = TV_STD_NTSC_M;
		/* TODO: how to choose LOW DEF FILTER */
		driver_data->denc_conf.cr_filter = TV_CR_NTSC_HIGH_DEF_FILTER;
		driver_data->denc_conf.black_level_setup = true;
		break;
	default:
		dev_warn(&ddev->dev, "%s:Failed to set video mode x=%d, y=%d\n",
			__func__, video_mode->xres, video_mode->yres);
		return -EINVAL;
	}


	driver_data->denc_conf.progressive	= !video_mode->interlaced;
	driver_data->denc_conf.act_output	= true;
	driver_data->denc_conf.test_pattern	= false;
	driver_data->denc_conf.partial_blanking	= true;
	driver_data->denc_conf.blank_all	= false;
	driver_data->denc_conf.suppress_col	= false;
	driver_data->denc_conf.phase_reset_mode	= TV_PHASE_RST_MOD_DISABLE;
	driver_data->denc_conf.dac_enable	= false;
	driver_data->denc_conf.act_dc_output	= true;

	set_power_mode(ddev, MCDE_DISPLAY_PM_STANDBY);
	if (pdata->rgb_2_yCbCr_transform)
		mcde_chnl_set_col_convert(ddev->chnl_state,
						pdata->rgb_2_yCbCr_transform,
						MCDE_CONVERT_RGB_2_YCBCR);
	mcde_chnl_stop_flow(ddev->chnl_state);
	res = mcde_chnl_set_video_mode(ddev->chnl_state, &ddev->video_mode);
	if (res < 0) {
		dev_warn(&ddev->dev, "%s:Failed to set video mode on channel\n",
			__func__);

		return res;
	}
	ddev->update_flags |= UPDATE_FLAG_VIDEO_MODE;

	return 0;
}

static int set_power_mode(struct mcde_display_device *ddev,
	enum mcde_display_power_mode power_mode)
{
	int ret = 0;
	int i;
	struct display_driver_data *driver_data = dev_get_drvdata(&ddev->dev);
	AB8500_DISP_TRACE;

	/* OFF -> STANDBY */
	if (ddev->power_mode == MCDE_DISPLAY_PM_OFF &&
					power_mode != MCDE_DISPLAY_PM_OFF) {
		dev_dbg(&ddev->dev, "off -> standby\n");
		if (ddev->platform_enable) {
			ret = ddev->platform_enable(ddev);
			if (ret)
				goto error;
		}
		if (driver_data->regulator) {
			for (i = 0; i < driver_data->nr_regulators; i++) {
				ret = regulator_enable(
						driver_data->regulator[i]);
				if (ret)
					goto off_to_standby_failed;
				dev_dbg(&ddev->dev, "regulator %d on\n", i);
			}
		}
		ab8500_denc_power_up(driver_data->denc_dev);
		ab8500_denc_reset(driver_data->denc_dev, true);
		ddev->power_mode = MCDE_DISPLAY_PM_STANDBY;
	}
	/* STANDBY -> ON */
	if (ddev->power_mode == MCDE_DISPLAY_PM_STANDBY &&
					power_mode == MCDE_DISPLAY_PM_ON) {
		dev_dbg(&ddev->dev, "standby -> on\n");
		ddev->power_mode = MCDE_DISPLAY_PM_ON;
	}
	/* ON -> STANDBY */
	if (ddev->power_mode == MCDE_DISPLAY_PM_ON &&
					power_mode <= MCDE_DISPLAY_PM_STANDBY) {
		dev_dbg(&ddev->dev, "on -> standby\n");
		ab8500_denc_reset(driver_data->denc_dev, false);
		ddev->power_mode = MCDE_DISPLAY_PM_STANDBY;
	}
	/* STANDBY -> OFF */
	if (ddev->power_mode == MCDE_DISPLAY_PM_STANDBY &&
					power_mode == MCDE_DISPLAY_PM_OFF) {
		bool error = false;
		dev_dbg(&ddev->dev, "standby -> off\n");
		if (driver_data->regulator) {
			for (i = 0; i < driver_data->nr_regulators; i++) {
				ret = regulator_disable(
						driver_data->regulator[i]);
				/* continue in case of an error */
				error |= (ret != 0);
				dev_dbg(&ddev->dev, "regulator %d off\n", i);
			}
		}
		if (ddev->platform_disable) {
			ret = ddev->platform_disable(ddev);
			error |= (ret != 0);
		}
		if (error) {
			/* the latest error code is returned */
			goto error;
		}
		memset(&(ddev->video_mode), 0, sizeof(struct mcde_video_mode));
		ab8500_denc_power_down(driver_data->denc_dev);
		ddev->power_mode = MCDE_DISPLAY_PM_OFF;
	}

	return 0;

	/* In case of an error, try to leave in off-state */
off_to_standby_failed:
	for (i--; i >= 0; i--)
		regulator_disable(driver_data->regulator[i]);
	ddev->platform_disable(ddev);

error:
	dev_err(&ddev->dev, "Failed to set power mode");
	return ret;
}

static int on_first_update(struct mcde_display_device *ddev)
{
	struct display_driver_data *driver_data = dev_get_drvdata(&ddev->dev);

	ab8500_denc_conf(driver_data->denc_dev, &driver_data->denc_conf);
	ab8500_denc_conf_plug_detect(driver_data->denc_dev, true, false,
							TV_PLUG_TIME_2S);
	ab8500_denc_mask_int_plug_det(driver_data->denc_dev, false, false);
	ddev->first_update = false;
	return 0;
}

static int display_update(struct mcde_display_device *ddev, bool tripple_buffer)
{
	int ret;

	if (ddev->first_update)
		on_first_update(ddev);
	if (ddev->power_mode != MCDE_DISPLAY_PM_ON && ddev->set_power_mode) {
		ret = set_power_mode(ddev, MCDE_DISPLAY_PM_ON);
		if (ret < 0)
			goto error;
	}
	ret = mcde_chnl_update(ddev->chnl_state, &ddev->update_area,
								tripple_buffer);
	if (ret < 0)
		goto error;
out:
	return ret;
error:
	dev_warn(&ddev->dev, "%s:Failed to set power mode to on\n", __func__);
	goto out;
}

/* Module init */
static int __init mcde_display_tvout_ab8500_init(void)
{
	pr_debug("%s\n", __func__);

	return mcde_display_driver_register(&ab8500_driver);
}
late_initcall(mcde_display_tvout_ab8500_init);

static void __exit mcde_display_tvout_ab8500_exit(void)
{
	pr_debug("%s\n", __func__);

	mcde_display_driver_unregister(&ab8500_driver);
}
module_exit(mcde_display_tvout_ab8500_exit);

MODULE_AUTHOR("Marcel Tunnissen <marcel.tuennissen@stericsson.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ST-Ericsson MCDE TVout through AB8500 display driver");
