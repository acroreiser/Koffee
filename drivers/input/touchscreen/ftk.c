/******************** (C) COPYRIGHT 2010 STMicroelectronics ********************
*
* File Name		: ftk.c
* Authors		: Sensor & MicroActuators BU - Application Team
*			      : Hui HU  (hui.hu@st.com)
* Version		: V 2.0
* Date			: 13/12/2011
* Description	: Capacitive touch screen controller (FingertipK)
*
********************************************************************************
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* THE PRESENT SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES
* OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, FOR THE SOLE
* PURPOSE TO SUPPORT YOUR APPLICATION DEVELOPMENT.
* AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY DIRECT,
* INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING FROM THE
* CONTENT OF SUCH SOFTWARE AND/OR THE USE MADE BY CUSTOMERS OF THE CODING
* INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
*
* THIS SOFTWARE IS SPECIFICALLY DESIGNED FOR EXCLUSIVE USE WITH ST PARTS.
*
********************************************************************************
* REVISON HISTORY
*
*VERSION | DATE 	  | AUTHORS	    | DESCRIPTION
* 1.0    |  29/06/2011| Bela Somaiah| First Release(link to ftk_patch.h)
* 2.0    |  13/12/2011| Hui HU      | the default setting is cut1.3 for 480*800 phone
* 3.0	 |  12/10/2012| Ramesh Chandrasekaran| Adapted for user space firmware loading
*******************************************************************************/
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/hrtimer.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/serio.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/ctype.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/firmware.h>
#include <linux/input/st-ftk.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#undef CONFIG_HAS_EARLYSUSPEND
/*
 * Definitions & global arrays.
 */
#define FTK_DRIVER_DESC         "FingerTipK MultiTouch I2C Driver"
#define FTK_DRIVER_NAME         "ftk"


#define P70_PATCH_LEN          	PatchDataSize
#define WRITE_CHUNK_SIZE        64
#define P70_PATCH_ADDR_START    0x00420000

static struct i2c_driver ftk_driver;
static struct workqueue_struct *stmtouch_wq;
static int cor_xyz[11][3];
static unsigned char ID_Indx[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static int IDj;

struct hrtimer test_timer;

struct ftk_ts {
	struct device *dev;
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct ftk_platform_data *pdata;
	struct hrtimer timer;
	struct work_struct  work;
	int irq;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void ftk_early_suspend(struct early_suspend *h);
static void ftk_late_resume(struct early_suspend *h);
#endif

static int init_ftk(struct ftk_ts *ftk_ts);

static int ftk_write_reg(struct ftk_ts *ftk_ts, unsigned char * reg, u16 num_com)
{
	struct i2c_msg xfer_msg[2];

	xfer_msg[0].addr = ftk_ts->client->addr;
	xfer_msg[0].len = num_com;
	xfer_msg[0].flags = 0;
	xfer_msg[0].buf = reg;

	return i2c_transfer(ftk_ts->client->adapter, xfer_msg, 1);
}

static int ftk_read_reg(struct ftk_ts *ftk_ts, unsigned char *reg, int cnum, u8 *buf, int num)
{
	struct i2c_msg xfer_msg[2];

	xfer_msg[0].addr = ftk_ts->client->addr;
	xfer_msg[0].len = cnum;
	xfer_msg[0].flags = 0;
	xfer_msg[0].buf = reg;

	xfer_msg[1].addr = ftk_ts->client->addr;
	xfer_msg[1].len = num;
	xfer_msg[1].flags = I2C_M_RD;
	xfer_msg[1].buf = buf;

	return i2c_transfer(ftk_ts->client->adapter, xfer_msg, 2);
}

static u8 load_config(struct ftk_ts *ftk_ts, const u8 *data, size_t size)
{
	u8 val[8];
	u8 regAdd[7];
	u16 patch_length;
	u16 config_length;
	u16 b0_i;

	patch_length = (data[0] << 8) + data[1];
	config_length =	(data[patch_length + 2] << 8) +
						data[patch_length + 3];

	regAdd[0] = 0xB0;	/* All writes below are 1 addr, 1 value */

	for (b0_i = (patch_length + 4); b0_i < size; b0_i += 2) {
		regAdd[1] = data[b0_i];
		regAdd[2] = data[b0_i + 1];
		ftk_write_reg(ftk_ts, &regAdd[0], 3);
		mdelay(5);
	}

	regAdd[0] = 0xB3;	/* Set Upper byte Address */
	regAdd[1] = 0xFF;
	regAdd[2] = 0xFF;
	ftk_write_reg(ftk_ts, &regAdd[0], 3);
	mdelay(5);

	regAdd[0] = 0xB1;	/* Set lower byte Address */
	regAdd[1] = 0xFC;
	regAdd[2] = 0x2F;
	regAdd[3] = 0xC0;
	ftk_write_reg(ftk_ts, &regAdd[0], 4);
	mdelay(5);

	regAdd[0] = 0x81;
	regAdd[1] = 0x00;
	ftk_write_reg(ftk_ts, &regAdd[0], 1);

	regAdd[0] = 0xB0;	/* Warm-Boot */
	regAdd[1] = 0x04;
	regAdd[2] = 0xE0;
	ftk_write_reg(ftk_ts, &regAdd[0], 3);
	mdelay(50);

	regAdd[0] = 0x8A;	/* Set Area Location */
	regAdd[1] = 0x00;	/* Touch screen size */
	regAdd[2] = 0x00;
	regAdd[3] = 0x00;
	regAdd[4] = 0xFF;	/* Full screen is used */
	regAdd[5] = 0xFF;
	regAdd[6] = 0xFF;
	ftk_write_reg(ftk_ts, &regAdd[0], 7);
	mdelay(5);

	regAdd[0] = 0x8B;	/* Set Area layer */
	regAdd[1] = 0x00;
	regAdd[2] = 0x00;
	ftk_write_reg(ftk_ts, &regAdd[0], 3);
	mdelay(5);

	regAdd[0] = 0x8C;	/* Set Area Event */
	regAdd[1] = 0x00;
	regAdd[2] = 0x00;
	regAdd[3] = 0x00;
	regAdd[4] = 0x00;
	regAdd[5] = 0x0F;
	regAdd[6] = 0xFF;
	ftk_write_reg(ftk_ts, &regAdd[0], 7);
	mdelay(5);

	regAdd[0] = 0x8D;	/* Set Area Touches */
	regAdd[1] = 0x00;
	regAdd[2] = 0xFF;
	ftk_write_reg(ftk_ts, &regAdd[0], 3);
	mdelay(5);

	regAdd[0] = 0x8F;	/* Touch orientation */
	regAdd[1] = 0xC0;
	ftk_write_reg(ftk_ts, &regAdd[0], 2);
	mdelay(5);

	regAdd[0] = 0x83;	/* TS Sense on */
	regAdd[1] = 0x00;
	ftk_write_reg(ftk_ts, &regAdd[0], 1);
	mdelay(5);

	regAdd[0] = 0x80;
	regAdd[1] = 0x00;
	ftk_write_reg(ftk_ts, &regAdd[0], 1);
	mdelay(5);

	regAdd[0] = 0x81;
	regAdd[1] = 0x00;
	ftk_write_reg(ftk_ts, &regAdd[0], 1);
	mdelay(5);

	regAdd[0] = 0xB0;
	regAdd[1] = 0x05;	/* Set Interrupt Polarity */
	regAdd[2] = 0x02;	/* '00' - level interrupt	'02' - edge interrupt */
	ftk_write_reg(ftk_ts, &regAdd[0], 3);
	mdelay(5);

	regAdd[0] = 0xB0;
	regAdd[1] = 0x06;	/* Enable Touch Detect Interrupt */
	regAdd[2] = 0x40;
	ftk_write_reg(ftk_ts, &regAdd[0], 3);
	mdelay(5);

	regAdd[0] = 0xB0;
	regAdd[1] = 0x07;	/* Read 0x07 to clear ISR */
	ftk_read_reg(ftk_ts, &regAdd[0], 2, &val[0], 1);
	mdelay(5);

	regAdd[0] = 0x85;	/* Read 0x85 to clear the buffer */
	ftk_read_reg(ftk_ts, &regAdd[0], 1, &val[0], 8);
	mdelay(20);

	regAdd[0] = 0x85;	/* Read 0x85 to clear the buffer */
	ftk_read_reg(ftk_ts, &regAdd[0], 1, &val[0], 8);
	mdelay(20);

	regAdd[0] = 0xB0;
	regAdd[1] = 0x03;
	ftk_read_reg(ftk_ts, regAdd, 2, val, 1);
	mdelay(5);

	regAdd[0] = 0x83;	/* TS Sense on */
	regAdd[1] = 0x00;
	ftk_write_reg(ftk_ts, &regAdd[0], 1);
	mdelay(5);

	return 0;
}

static u8 load_patch(struct ftk_ts *ftk_ts, const u8 *data, size_t size)
{
	u32 writeAddr, j = 0, patch_length = 0;
	u8 i, byteWork0[2], byteWork1[67];
	u8 regAdd[3] = {0xB3, 0xB1, 0};

	patch_length = (data[0] << 8) + data[1];

	while (j < patch_length) {
		writeAddr = P70_PATCH_ADDR_START + j;

		byteWork0[0] = (writeAddr >> 24) & 0xFF;
		byteWork0[1] = (writeAddr >> 16) & 0xFF;
		regAdd[0] = 0xB3;
		regAdd[1] = byteWork0[0];
		regAdd[2] = byteWork0[1];
		ftk_write_reg(ftk_ts, &regAdd[0], 3);
		mdelay(1);

		byteWork1[0] = 0xB1;
		byteWork1[1] = (writeAddr >> 8) & 0xFF;
		byteWork1[2] = writeAddr & 0xFF;

		i = 0;
		while ((j < patch_length) && (i < WRITE_CHUNK_SIZE)) {
			byteWork1[i+3] = data[j + 2];
			i++;
			j++;
		}
		ftk_write_reg(ftk_ts, &byteWork1[0], 67);
		mdelay(1);
	}

	return 0;
}

static int verify_firmware(struct ftk_ts *ftk, const u8 *data, size_t size)
{
	u16 firmware_length;
	u16 patch_length;
	u16 config_length;

	firmware_length = size;

	patch_length = (data[0] << 8) + data[1];
	config_length = (data[patch_length + 2] << 8) +
						data[patch_length + 3];

	if (firmware_length != patch_length + config_length + 4) {
		dev_info(&ftk->client->dev, "firmware file corrupted (%d: %d %d)\n",
			firmware_length, patch_length, config_length);
		return -EINVAL;
	}
	dev_info(&ftk->client->dev, "Firmware file okay\n");
	return 0;
}

static int init_ftk(struct ftk_ts *ftk_ts)
{
	const struct firmware *firmware;
	u8 val[8];
	u8 regAdd[7];
	int ret;

	regAdd[0] = 0xB0;
	regAdd[1] = 0x00;
	ret = ftk_read_reg(ftk_ts, regAdd, 2, val, 3);

	if (ret < 0)
		dev_err(&ftk_ts->client->dev, " i2c_transfer failed\n");

	mdelay(1);
	dev_info(&ftk_ts->client->dev, "Chip ID = %x %x %x\n" ,
					val[0], val[1], val[2]);

	regAdd[0] = 0x9E;	/* TS Soft Reset */
	ret = ftk_write_reg(ftk_ts, &regAdd[0], 1);
	mdelay(1);

	/* Load firmware from file */
	dev_info(&ftk_ts->client->dev, "loading firmware from file: %s\n",
			ftk_ts->pdata->patch_file);

	ret = request_firmware(&firmware, ftk_ts->pdata->patch_file,
					&ftk_ts->client->dev);
	if (ret < 0) {
		dev_err(&ftk_ts->client->dev,
			"request patch firmware failed (%d)\n", ret);
	} else {
		ret = verify_firmware(ftk_ts, firmware->data,
					firmware->size);
		if (ret == 0) {
			ret = load_patch(ftk_ts, firmware->data,
					firmware->size);
			if (ret == 0) {
				dev_info(&ftk_ts->client->dev, "Patch loaded\n");
				load_config(ftk_ts, firmware->data,
					firmware->size);
				dev_info(&ftk_ts->client->dev, "config loaded\n");
			}
		}
	}
	release_firmware(firmware);

    regAdd[0] = 0xB3;
	regAdd[1] = 0xFF;
	regAdd[2] = 0xFF;
	ftk_write_reg(ftk_ts, &regAdd[0], 3);
	mdelay(5);

	if (ret < 0)
		dev_err(&ftk_ts->client->dev, "Not Initialised\n");
	else
		dev_info(&ftk_ts->client->dev, "Initialised\n");

	return 0;
}

static enum hrtimer_restart st_ts_timer_func(struct hrtimer *timer)
{
	struct ftk_ts *ftkts  = container_of(timer, struct ftk_ts, timer);
	queue_work(stmtouch_wq, &ftkts->work);
	return HRTIMER_NORESTART;
}

static irqreturn_t ts_interrupt(int irq, void *handle)
{
	struct ftk_ts *ftk_ts = handle;

	disable_irq_nosync(ftk_ts->client->irq);
	queue_work(stmtouch_wq, &ftk_ts->work);
	return IRQ_HANDLED;
}

static u8 decode_data_packet(struct ftk_ts *ftkts, unsigned char data[], unsigned char LeftEvent)
{
	u8 EventNum = 0;
	u8 NumTouches = 0;
	u8 TouchID, EventID;
	u8 LastLeftEvent = 0;
	int i, num_touch;

	for (EventNum = 0; EventNum <= LeftEvent; EventNum++) {
		LastLeftEvent = data[7+EventNum*8] & 0x0F;
		NumTouches = (data[1+EventNum*8] & 0xF0) >> 4;
		TouchID = data[1+EventNum*8] & 0x0F;
		EventID = data[EventNum*8] & 0xFF;

		if ((EventID == 0x03) || (EventID == 0x05)) {
			ID_Indx[TouchID] = 1;
			cor_xyz[TouchID][0] = ((data[4+EventNum*8]&0xF0)>>4)|((data[2+EventNum*8])<<4);
			cor_xyz[TouchID][1] = ((data[4+EventNum*8]&0x0F)|((data[3+EventNum*8])<<4));
			cor_xyz[TouchID][2] = data[5+EventNum*8];
		} else if (EventID == 0x04) {
			ID_Indx[TouchID] = 0;
		}

		if (EventID == 0x10) {	/* for esd reset */
			IDj = 0;	/* clear the data buffer  of  x,y,z */
			for (i = 0; i < 10; i++) {
				ID_Indx[i] = 0;
			}
			input_mt_sync(ftkts->input_dev);
			input_sync(ftkts->input_dev);

			init_ftk(ftkts);
			return 0;
		}

	}
	num_touch = 0;
	for (i = 0; i < 11; i++) {
		if (ID_Indx[i]) {
			num_touch++;
			input_report_abs(ftkts->input_dev, ABS_MT_TRACKING_ID, i);
			input_report_abs(ftkts->input_dev, ABS_MT_POSITION_X, cor_xyz[i][0]);
			input_report_abs(ftkts->input_dev, ABS_MT_POSITION_Y, cor_xyz[i][1]);
			input_report_abs(ftkts->input_dev, ABS_MT_TOUCH_MAJOR, cor_xyz[i][2]);
			input_report_key(ftkts->input_dev, BTN_TOUCH, 1);
			input_mt_sync(ftkts->input_dev);
		}
	}

	if (num_touch == 0) {
		input_report_key(ftkts->input_dev, BTN_TOUCH, 0);
		input_mt_sync(ftkts->input_dev);
	}
	input_sync(ftkts->input_dev);

	return LastLeftEvent;
}

/* FTK Touch data reading and processing */
static void ts_tasklet_proc(struct work_struct *work)
{
	struct ftk_ts *ftkts = container_of(work, struct ftk_ts, work);

	unsigned char data[256+1];
	int rc;
	u8 temp_data[2];
	u8 status;
	u8 regAdd;
	u8 FirstLeftEvent = 0;
	u8 repeat_flag = 0;

	data[0] = 0xB0;
	data[1] = 0x07;
	rc = ftk_read_reg(ftkts, &data[0], 2, &status, 1);

	if (status & 0x40) {
		do {
			data[0] = 0xB1;
			data[1] = 0xFC;
			data[2] = 0x21;

			ftk_read_reg(ftkts, &data[0], 3, &temp_data[0], 2);
			FirstLeftEvent = temp_data[1];
			if (FirstLeftEvent > 0) {
				memset(data, 0x0, 257);
				regAdd = 0x86;
				data[0] = 0;
				rc = ftk_read_reg(ftkts, &regAdd, 1, data, (FirstLeftEvent << 1));

				decode_data_packet(ftkts, data, (FirstLeftEvent >> 2));
			}

			data[0] = 0xB0;
			data[1] = 0x07;
			rc = ftk_read_reg(ftkts, &data[0], 2, &status, 1);

			if (status & 0x40)
				repeat_flag = 1;
			else
				repeat_flag = 0;

		} while (repeat_flag);
	}

	mdelay(1);

	if (!ftkts->irq) {
		hrtimer_start(&ftkts->timer, ktime_set(0, 10000000), HRTIMER_MODE_REL);
	} else {
		enable_irq(ftkts->client->irq);
	}
}


static int ftk_probe(struct i2c_client *client, const struct i2c_device_id *idp)
{
	struct ftk_ts *ftk_ts = NULL;
	struct ftk_platform_data *pdata;
	int err = 0;

	dev_info(&client->dev, "probe\n");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, " err = EIO! \n");
		err = EIO;
		goto fail;
	}

	ftk_ts = kzalloc(sizeof(struct ftk_ts), GFP_KERNEL);
	if (!ftk_ts) {
		dev_err(&client->dev, " err = ENOMEM! \n");
		err = ENOMEM;
		goto fail;
	}

	INIT_WORK(&ftk_ts->work, ts_tasklet_proc);

	ftk_ts->client = client;
	i2c_set_clientdata(client, ftk_ts);

	pdata = client->dev.platform_data;

	ftk_ts->pdata = pdata;
	ftk_ts->dev = &ftk_ts->client->dev;
	ftk_ts->input_dev = input_allocate_device();
	ftk_ts->input_dev->dev.parent = &client->dev;
	if (!ftk_ts->input_dev) {
		dev_err(&client->dev, " err = ENOMEM! \n");
		err = ENOMEM;
		goto fail;
	}
	ftk_ts->input_dev->name = FTK_DRIVER_NAME;
	ftk_ts->input_dev->phys = "ftk/input0";
	ftk_ts->input_dev->id.bustype = BUS_I2C;
	ftk_ts->input_dev->id.vendor = 0x0001;
	ftk_ts->input_dev->id.product = 0x0002;
	ftk_ts->input_dev->id.version = 0x0100;

	ftk_ts->input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	ftk_ts->input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	set_bit(EV_SYN, ftk_ts->input_dev->evbit);
	set_bit(EV_KEY, ftk_ts->input_dev->evbit);
	set_bit(BTN_TOUCH, ftk_ts->input_dev->keybit);
	set_bit(BTN_2, ftk_ts->input_dev->keybit);
	set_bit(EV_ABS, ftk_ts->input_dev->evbit);

	input_set_abs_params(ftk_ts->input_dev, ABS_X,
					ftk_ts->pdata->x_min, ftk_ts->pdata->x_max, 0, 0);
	input_set_abs_params(ftk_ts->input_dev, ABS_Y,
					ftk_ts->pdata->y_min, ftk_ts->pdata->y_max, 0, 0);
	input_set_abs_params(ftk_ts->input_dev, ABS_PRESSURE,
					ftk_ts->pdata->p_min, ftk_ts->pdata->p_max, 0, 0);
	input_set_abs_params(ftk_ts->input_dev, ABS_MT_TRACKING_ID, 0, 10, 0, 0);
	input_set_abs_params(ftk_ts->input_dev, ABS_MT_TOUCH_MAJOR,
					ftk_ts->pdata->p_min, ftk_ts->pdata->p_max, 0, 0);
	input_set_abs_params(ftk_ts->input_dev, ABS_MT_WIDTH_MAJOR,
					ftk_ts->pdata->p_min, ftk_ts->pdata->p_max, 0, 0);
	input_set_abs_params(ftk_ts->input_dev, ABS_MT_POSITION_X,
					ftk_ts->pdata->x_min, ftk_ts->pdata->x_max, 0, 0);
	input_set_abs_params(ftk_ts->input_dev, ABS_MT_POSITION_Y,
					ftk_ts->pdata->y_min, ftk_ts->pdata->y_max, 0, 0);

	err = input_register_device(ftk_ts->input_dev);
	if (err) {
		dev_err(&client->dev, "input_register_device fail! \n");
		goto fail;
	}

	err = init_ftk(ftk_ts);
	if (err) {
		dev_err(&client->dev, "init_ftk  fail! \n");
		goto fail;
	}

	ftk_ts->irq = client->irq;

	if (!ftk_ts->irq) {
		hrtimer_init(&ftk_ts->timer, CLOCK_BOOTTIME, HRTIMER_MODE_REL);
		ftk_ts->timer.function = st_ts_timer_func;
		hrtimer_start(&ftk_ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
	} else {
		if (request_irq (ftk_ts->irq, ts_interrupt, IRQF_TRIGGER_FALLING, client->name, ftk_ts)) {
			dev_err(&client->dev, "request_irq  fail! \n");
			err = -EBUSY;
			goto fail;
		}
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
		ftk_ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
		ftk_ts->early_suspend.suspend = ftk_early_suspend;
		ftk_ts->early_suspend.resume = ftk_late_resume;
		register_early_suspend(&ftk_ts->early_suspend);
#endif

	return 0;
fail:
	if (ftk_ts) {
		if (ftk_ts->input_dev)
			input_free_device(ftk_ts->input_dev);
		kfree(ftk_ts);
	}
	dev_err(&client->dev, "probe err=%d\n", err);
	return err;
}

static int ftk_remove(struct i2c_client *client)
{
	struct ftk_ts *ts = i2c_get_clientdata(client);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&ts->early_suspend);
#endif

	if (ts->irq)
		free_irq(client->irq, ts);
	else
		hrtimer_cancel(&ts->timer);

	input_unregister_device(ts->input_dev);
	kfree(ts);
	return 0;
}

static int ftk_suspend(struct i2c_client *client, pm_message_t mesg)
{
	int ret, i;
	u8 regAdd[3];

	struct ftk_ts  *ts = i2c_get_clientdata(client);
	dev_info(&client->dev, "suspend\n");
	if (ts->irq) {
		disable_irq(client->irq);
	} else {
		hrtimer_cancel(&ts->timer);
	}

	ret = cancel_work_sync(&ts->work);

	regAdd[0] = 0x80;	/* TS Sleep in */
	regAdd[1] = 0x00;
	ret = ftk_write_reg(ts, &regAdd[0], 1);
	mdelay(5);

	regAdd[0] = 0x88;	/*flush buffer */
	regAdd[1] = 0x00;
	ret = ftk_write_reg(ts, &regAdd[0], 1);
	mdelay(5);

	regAdd[0] = 0xB0;	/*disable interrupt */
	regAdd[1] = 0x06;
	regAdd[2] = 0x00;
	ret = ftk_write_reg(ts, &regAdd[0], 3);
	mdelay(5);

	IDj = 0;	/* clear the data buffer  of  x,y,z */

	for (i = 0; i < 10; i++) {
		ID_Indx[i] = 0;
	}

	input_mt_sync(ts->input_dev);
	input_sync(ts->input_dev);

	if (ret < 0)
		dev_err(&client->dev,
			"suspend: i2c_smbus_write_byte_data failed\n");

	return 0;
}

static int ftk_resume(struct i2c_client *client)
{
	int ret;
	u8 regAdd[3];
	struct ftk_ts  *ts = i2c_get_clientdata(client);
	dev_info(&client->dev, "resume\n");
	if (ts->irq)
		enable_irq(client->irq);
	else
		hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);

	regAdd[0] = 0x81;	/* TS Sleep out */
	regAdd[1] = 0x00;
	ret = ftk_write_reg(ts, &regAdd[0], 1);
	mdelay(5);

	regAdd[0] = 0xB0;	/* enable interrupt */
	regAdd[1] = 0x06;
	regAdd[2] = 0x40;
	ret = ftk_write_reg(ts, &regAdd[0], 3);
	mdelay(5);

	if (ret < 0)
		dev_err(&client->dev,
			"resume: i2c_smbus_write_byte_data failed\n");

	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void ftk_early_suspend(struct early_suspend *h)
{
	struct ftk_ts *ts;
	ts = container_of(h, struct ftk_ts, early_suspend);
	ftk_suspend(ts->client, PMSG_SUSPEND);
}

static void ftk_late_resume(struct early_suspend *h)
{
	struct ftk_ts *ts;
	ts = container_of(h, struct ftk_ts, early_suspend);
	ftk_resume(ts->client);
}
#endif

static const struct i2c_device_id ftk_id[] = {
	{FTK_DRIVER_NAME, 0},
	{ }
};

static struct i2c_driver ftk_driver = {
	.driver = {
		.name = "ftk",
		.owner = THIS_MODULE,
	},
	.probe = ftk_probe,
	.remove = ftk_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend = ftk_suspend,
	.resume = ftk_resume,
#endif
	.id_table = ftk_id,
};

static int __init ftk_init(void)
{
	struct i2c_adapter *adap;
	struct i2c_client *client;
	const struct i2c_board_info *info;
	struct ftk_platform_data *pdata;

	info = snowball_touch_get_plat_data();
	pdata = info->platform_data;

	adap = i2c_get_adapter(pdata->busnum);

	if (!adap) {
		pr_err("Failed to get adapter i2c%d\n", pdata->busnum);
		return -ENODEV;;
	}
	client = i2c_new_device(adap, info);
	if (!client)
		pr_err("Failed to register %s to i2c%d\n",
				info->type, pdata->busnum);

	i2c_put_adapter(adap);

	stmtouch_wq = create_singlethread_workqueue("stmtouch_wq");
	if (!stmtouch_wq)
		return -ENOMEM;

	return i2c_add_driver(&ftk_driver);
}

static void __exit ftk_exit(void)
{

	i2c_del_driver(&ftk_driver);
	if (stmtouch_wq)
		destroy_workqueue(stmtouch_wq);
}

MODULE_DESCRIPTION(FTK_DRIVER_DESC);
MODULE_AUTHOR("Hui HU");
MODULE_LICENSE("GPL");

module_init(ftk_init);
module_exit(ftk_exit);
