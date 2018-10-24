/*
 * Copyright (C) ST-Ericsson SA 2012
 * License Terms: GNU General Public License, version 2
 *
 * Mostly this accelerometer device is a copy of magnetometer
 * driver lsm303dlh or viceversa, so the code is mostly based
 * on lsm303dlh driver.
 *
 * Author: Naga Radhesh Y <naga.radheshy@stericsson.com>
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/lsm303dlh.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include "../iio.h"
#include "../sysfs.h"

/* Idenitification register */
#define LSM303DLH_A_CHIP_ID		0x0F
/* control register1  */
#define LSM303DLH_A_CTRL_REG1_A	0x20
/* control register2 */
#define LSM303DLH_A_CTRL_REG2_A	0x21
/* control register3 */
#define LSM303DLH_A_CTRL_REG3_A	0x22
/* control register4 */
#define LSM303DLH_A_CTRL_REG4_A	0x23
/* control register5 */
#define LSM303DLH_A_CTRL_REG5_A	0x24
/* data output X register */
#define LSM303DLH_A_OUT_X_L_A	0x28
/* data output Y register */
#define LSM303DLH_A_OUT_Y_L_A	0x2A
/* data output Z register */
#define LSM303DLH_A_OUT_Z_L_A	0x2C
/* status register */
#define LSM303DLH_A_STATUS_REG_A	0x27

/* sensitivity adjustment */
#define LSM303DLH_A_SHIFT_ADJ_2G 4 /*    1/16*/
#define LSM303DLH_A_SHIFT_ADJ_4G 3 /*    2/16*/
#define LSM303DLH_A_SHIFT_ADJ_8G 2 /* ~3.9/16*/

/* control register 1, Mode selection */
#define LSM303DLH_A_CR1_PM_BIT 5
#define LSM303DLH_A_CR1_PM_MASK (0x7 << LSM303DLH_A_CR1_PM_BIT)
/* control register 1, Data Rate */
#define LSM303DLH_A_CR1_DR_BIT 3
#define LSM303DLH_A_CR1_DR_MASK (0x3 << LSM303DLH_A_CR1_DR_BIT)
/* control register 1, x,y,z enable bits */
#define LSM303DLH_A_CR1_EN_BIT 0
#define LSM303DLH_A_CR1_EN_MASK (0x7 << LSM303DLH_A_CR1_EN_BIT)
#define LSM303DLH_A_CR1_AXIS_ENABLE 7

/* control register 2, Re-Boot Memory */
#define LSM303DLH_A_CR2_BOOT_ENABLE 0x80

/* control register 4, self test */
#define LSM303DLH_A_CR4_ST_BIT 1
#define LSM303DLH_A_CR4_ST_MASK (0x1 << LSM303DLH_A_CR4_ST_BIT)
/* control register 4, self test sign */
#define LSM303DLH_A_CR4_STS_BIT 3
#define LSM303DLH_A_CR4_STS_MASK (0x1 << LSM303DLH_A_CR4_STS_BIT)
/* control register 4, full scale */
#define LSM303DLH_A_CR4_FS_BIT 4
#define LSM303DLH_A_CR4_FS_MASK (0x3 << LSM303DLH_A_CR4_FS_BIT)
/* control register 4, endianness */
#define LSM303DLH_A_CR4_BLE_BIT 6
#define LSM303DLH_A_CR4_BLE_MASK (0x1 << LSM303DLH_A_CR4_BLE_BIT)
/* control register 4, Block data update */
#define LSM303DLH_A_CR4_BDU_BIT 7
#define LSM303DLH_A_CR4_BDU_MASK (0x1 << LSM303DLH_A_CR4_BDU_BIT)

/* Accelerometer operating mode */
#define LSM303DLH_A_MODE_OFF	0x00
#define LSM303DLH_A_MODE_NORMAL	0x01
#define LSM303DLH_A_MODE_LP_HALF	0x02
#define LSM303DLH_A_MODE_LP_1	0x03
#define LSM303DLH_A_MODE_LP_2	0x04
#define LSM303DLH_A_MODE_LP_5	0x05
#define LSM303DLH_A_MODE_LP_10	0x06

/*
 * CTRL_REG1_A register rate settings
 *
 * DR1 DR0      Output data rate[Hz]
 * 0   0           50
 * 0   1           100
 * 1   0           400
 * 1   1           1000
 */
#define LSM303DLH_A_RATE_50		0x00
#define LSM303DLH_A_RATE_100	0x01
#define LSM303DLH_A_RATE_400	0x02
#define LSM303DLH_A_RATE_1000	0x03

/*
 * CTRL_REG4_A register range settings
 *
 * FS1 FS0	FUll scale range
 * 0   0           2g
 * 0   1           4g
 * 1   0           Not used
 * 1   1           8g
 */
#define LSM303DLH_A_RANGE_2G	0x00
#define LSM303DLH_A_RANGE_4G	0x01
#define LSM303DLH_A_RANGE_8G	0x03

/* device status defines */
#define LSM303DLH_A_DEVICE_OFF 0
#define LSM303DLH_A_DEVICE_ON 1
#define LSM303DLH_A_DEVICE_SUSPENDED 2

/* status register */
#define LSM303DLH_A_SR_REG_A	0x27
/* status register, ready  */
#define LSM303DLH_A_XYZ_DATA_RDY	0x08
#define LSM303DLH_A_XYZ_DATA_RDY_BIT	3
#define LSM303DLH_A_XYZ_DATA_RDY_MASK	(0x1 << LSM303DLH_A_XYZ_DATA_RDY_BIT)

/* Multiple byte transfer enable */
#define MULTIPLE_I2C_TR		0x80

/*
 * struct lsm303dlh_a_data - data structure used by lsm303dlh_a driver
 * @client: i2c client
 * @indio_dev: iio device structure
 * @attr: device attributes
 * @lock: mutex lock for sysfs operations
 * @regulator: regulator
 * @early_suspend: early suspend structure
 * @pdata: lsm303dlh platform data
 * @mode: current mode of operation
 * @rate: current sampling rate
 * @range: current range value of accelerometer
 * @shift_adjust: output bit shift
 * @device_status: device is ON, OFF or SUSPENDED
 */

struct lsm303dlh_a_data {
	struct i2c_client	*client;
	struct mutex		lock;
	struct regulator	*regulator;
	struct lsm303dlh_platform_data *pdata;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
	short			data[3];
	u8			mode;
	u8			rate;
	u8			range;
	int			shift_adjust;
	int			device_status;
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void lsm303dlh_a_early_suspend(struct early_suspend *data);
static void lsm303dlh_a_late_resume(struct early_suspend *data);
#endif

static inline int is_device_on(struct lsm303dlh_a_data *data)
{
	struct i2c_client *client = data->client;
	/* * Perform read/write operation only when device is active */
	if (data->device_status != LSM303DLH_A_DEVICE_ON) {
		dev_err(&client->dev,
			"device is switched off, make it on using mode");
		return -EINVAL;
	}

	return 0;
}

/* To disable regulator and status */
static int lsm303dlh_a_disable(struct lsm303dlh_a_data *data)
{
	data->device_status = LSM303DLH_A_DEVICE_OFF;

	regulator_disable(data->regulator);

	return 0;
}

/* To enable regulator and status */
static int lsm303dlh_a_enable(struct lsm303dlh_a_data *data)
{
	data->device_status = LSM303DLH_A_DEVICE_ON;

	regulator_enable(data->regulator);

	return 0;
}

static s32 lsm303dlh_a_setbootbit(struct i2c_client *client, u8 reg_val)
{
	 /* write to the boot bit to reboot memory content */
	return i2c_smbus_write_byte_data(client,
					LSM303DLH_A_CTRL_REG2_A, reg_val);

}

static s32 lsm303dlh_a_set_mode(struct i2c_client *client, u8 mode)
{
	int reg_val;

	if (mode > LSM303DLH_A_MODE_LP_10) {
		dev_err(&client->dev, "given mode not supported\n");
		return -EINVAL;
	}

	reg_val = i2c_smbus_read_byte_data(client, LSM303DLH_A_CTRL_REG1_A);

	reg_val |= LSM303DLH_A_CR1_AXIS_ENABLE;
	reg_val &= ~LSM303DLH_A_CR1_PM_MASK;

	reg_val |= ((mode << LSM303DLH_A_CR1_PM_BIT) & LSM303DLH_A_CR1_PM_MASK);

	/* the upper three bits indicates the accelerometer sensor mode */
	return i2c_smbus_write_byte_data(client,
					LSM303DLH_A_CTRL_REG1_A, reg_val);
}

static s32 lsm303dlh_a_set_rate(struct i2c_client *client, u8 rate)
{
	int reg_val;

	if (rate > LSM303DLH_A_RATE_1000) {
		dev_err(&client->dev, "given rate not supported\n");
		return -EINVAL;
	}

	reg_val = i2c_smbus_read_byte_data(client, LSM303DLH_A_CTRL_REG1_A);

	reg_val &= ~LSM303DLH_A_CR1_DR_MASK;

	reg_val |= ((rate << LSM303DLH_A_CR1_DR_BIT) & LSM303DLH_A_CR1_DR_MASK);

	/* 3rd and 4th bits indicate rate of accelerometer */
	return i2c_smbus_write_byte_data(client,
					LSM303DLH_A_CTRL_REG1_A, reg_val);
}

static s32 lsm303dlh_a_set_range(struct i2c_client *client, u8 range)
{
	int reg_val;

	if (range > LSM303DLH_A_RANGE_8G) {
		dev_err(&client->dev, "given range not supported\n");
		return -EINVAL;
	}

	reg_val = (range << LSM303DLH_A_CR4_FS_BIT);

	/*
	 * Block mode update is recommended for not
	 * ending up reading different values
	 */
	reg_val |= LSM303DLH_A_CR4_BDU_MASK;

	/* 4th and 5th bits indicate range of accelerometer */
	return i2c_smbus_write_byte_data(client,
					LSM303DLH_A_CTRL_REG4_A, reg_val);
}

static s32 lsm303dlh_a_set_shift(struct lsm303dlh_a_data *data, u8 range)
{
	int ret = 0;
	struct i2c_client *client = data->client;

	switch (range) {
	case LSM303DLH_A_RANGE_2G:
		data->shift_adjust = LSM303DLH_A_SHIFT_ADJ_2G;
		break;
	case LSM303DLH_A_RANGE_4G:
		data->shift_adjust = LSM303DLH_A_SHIFT_ADJ_4G;
		break;
	case LSM303DLH_A_RANGE_8G:
		data->shift_adjust = LSM303DLH_A_SHIFT_ADJ_8G;
		break;
	default:
		dev_err(&client->dev, "Invalid range %d\n", range);
		ret = -EINVAL;
	}
	return ret;
}

/*
 * To read output x/y/z data register,
 * in this case x,y and z are not
 * mapped w.r.t board orientation.
 * Reading just raw data from device
 */
static ssize_t lsm303dlh_a_xyz_read(struct iio_dev *indio_dev,
					int address,
					int *buf)
{

	struct lsm303dlh_a_data *data = iio_priv(indio_dev);
	int lsb , msb;
	int ret;
	s16 val;

	/* Perform read/write operation, only when device is active */
	ret = is_device_on(data);
	if (ret)
		return -EINVAL;

	mutex_lock(&data->lock);

	ret = i2c_smbus_read_byte_data(data->client, LSM303DLH_A_SR_REG_A);

	/* wait till data is written to all six registers */
	while (!(ret & LSM303DLH_A_XYZ_DATA_RDY_MASK))
		ret = i2c_smbus_read_byte_data(data->client,
						LSM303DLH_A_SR_REG_A);

	lsb = i2c_smbus_read_byte_data(data->client, address);
	if (lsb < 0) {
		dev_err(&data->client->dev, "reading xyz failed\n");
		mutex_unlock(&data->lock);
		return -EINVAL;
	}
	msb = i2c_smbus_read_byte_data(data->client, (address + 1));
	if (msb < 0) {
		dev_err(&data->client->dev, "reading xyz failed\n");
		mutex_unlock(&data->lock);
		return -EINVAL;
	}
	val = ((msb << 8) | lsb);


	val >>= data->shift_adjust;
	*buf = (s16)val;
	mutex_unlock(&data->lock);

	return IIO_VAL_INT;
}

/*
 * To read output x,y,z data register. After reading change x,y and z values
 * w.r.t the orientation of the device.
 */
static ssize_t lsm303dlh_a_readdata(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct lsm303dlh_a_data *data = iio_priv(dev_get_drvdata(dev));
	struct lsm303dlh_platform_data *pdata = data->pdata;
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	u8 map_x = pdata->axis_map_x;
	u8 map_y = pdata->axis_map_y;
	u8 map_z = pdata->axis_map_z;
	int ret;
	unsigned char accel_data[6];
	s16 val[3];

	/*
	 * Perform read/write operation, only when device is active
	 */
	ret = is_device_on(data);
	if (ret)
		return -EINVAL;

	mutex_lock(&data->lock);

	ret = i2c_smbus_read_byte_data(data->client, LSM303DLH_A_SR_REG_A);
	/* wait till data is written to all six registers */
	while (!((ret & LSM303DLH_A_XYZ_DATA_RDY_MASK)))
		ret = i2c_smbus_read_byte_data(data->client,
						LSM303DLH_A_SR_REG_A);

	ret = i2c_smbus_read_i2c_block_data(data->client,
		   this_attr->address | MULTIPLE_I2C_TR, 6, accel_data);

	if (ret < 0) {
		dev_err(&data->client->dev, "reading xyz failed\n");
		mutex_unlock(&data->lock);
		return -EINVAL;
	}


	/* MSB is at lower address */
	val[0] = (s16)
		(((accel_data[1]) << 8) | accel_data[0]);
	val[1] = (s16)
		(((accel_data[3]) << 8) | accel_data[2]);
	val[2] = (s16)
		(((accel_data[5]) << 8) | accel_data[4]);

	val[0] >>= data->shift_adjust;
	val[1] >>= data->shift_adjust;
	val[2] >>= data->shift_adjust;

	/* modify the x,y and z values w.r.t orientation of device*/
	if (pdata->negative_x)
		val[map_x] = -val[map_x];
	if (pdata->negative_y)
		val[map_y] = -val[map_y];
	if (pdata->negative_z)
		val[map_z] = -val[map_z];

	mutex_unlock(&data->lock);

	return sprintf(buf, "%d:%d:%d:%lld\n", val[map_x], val[map_y],
		       val[map_z], iio_get_time_ns());
}

static ssize_t show_chip_id(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct lsm303dlh_a_data *data = iio_priv(dev_get_drvdata(dev));

	return sprintf(buf, "%d\n", data->pdata->chip_id);
}

static ssize_t show_operating_mode(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct lsm303dlh_a_data *data = iio_priv(dev_get_drvdata(dev));

	return sprintf(buf, "%d\n", data->mode);
}

static ssize_t set_operating_mode(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct lsm303dlh_a_data *data = iio_priv(dev_get_drvdata(dev));
	struct i2c_client *client = data->client;
	int error;
	unsigned long mode = 0;
	bool set_boot_bit = false;

	mutex_lock(&data->lock);

	error = kstrtoul(buf, 10, &mode);
	if (error) {
		count = error;
		goto exit;
	}

	if (mode > LSM303DLH_A_MODE_LP_10) {
		dev_err(&client->dev, "trying to set invalid mode\n");
		count = -EINVAL;
		goto exit;
	}

	/*
	 * If device is drived to sleep mode in suspend, update mode
	 * and return
	 */
	if (data->device_status == LSM303DLH_A_DEVICE_SUSPENDED &&
			mode == LSM303DLH_A_MODE_OFF) {
		data->mode = mode;
		goto exit;
	}

	/*  if same mode as existing, return */
	if (data->mode == mode)
		goto exit;

	/*
	 * set boot bit when device comes from suspend state
	 * to ensure correct device behavior after it resumes
	 */
	if (data->device_status == LSM303DLH_A_DEVICE_SUSPENDED)
		set_boot_bit = true;

	/* Enable the regulator if it is not turned ON earlier */
	if (data->device_status == LSM303DLH_A_DEVICE_OFF ||
		data->device_status == LSM303DLH_A_DEVICE_SUSPENDED)
		lsm303dlh_a_enable(data);

	dev_dbg(dev, "set operating mode to %lu\n", mode);
	error = lsm303dlh_a_set_mode(client, mode);
	if (error < 0) {
		dev_err(&client->dev, "Error in setting the mode\n");
		count = -EINVAL;
		goto exit;
	}

	data->mode = mode;

	if (set_boot_bit) {
		/* set boot bit to reboot memory content */
		lsm303dlh_a_setbootbit(client, LSM303DLH_A_CR2_BOOT_ENABLE);
	}

	/* If mode is OFF then disable the regulator */
	if (data->mode == LSM303DLH_A_MODE_OFF) {
		/* fall back to default values */
		data->rate = LSM303DLH_A_RATE_50;
		data->range = LSM303DLH_A_RANGE_2G;
		data->shift_adjust = LSM303DLH_A_SHIFT_ADJ_2G;
		lsm303dlh_a_disable(data);
	}
exit:
	mutex_unlock(&data->lock);
	return count;
}

static IIO_CONST_ATTR_SAMP_FREQ_AVAIL("50 100 400 1000");

static ssize_t set_sampling_frequency(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct lsm303dlh_a_data *data = iio_priv(dev_get_drvdata(dev));
	struct i2c_client *client = data->client;
	unsigned long rate = 0;
	int err;

	/* Perform read/write operation, only when device is active */
	err = is_device_on(data);
	if (err)
		return -EINVAL;

	if (strncmp(buf, "50" , 2) == 0)
		rate = LSM303DLH_A_RATE_50;

	else if (strncmp(buf, "400" , 3) == 0)
		rate = LSM303DLH_A_RATE_400;

	else if (strncmp(buf, "1000" , 4) == 0)
		rate = LSM303DLH_A_RATE_1000;

	else if (strncmp(buf, "100" , 3) == 0)
		rate = LSM303DLH_A_RATE_100;
	else
		return -EINVAL;

	mutex_lock(&data->lock);

	if (lsm303dlh_a_set_rate(client, rate)) {
		dev_err(&client->dev, "set rate failed\n");
		count = -EINVAL;
		goto exit;
	}
	data->rate = rate;

exit:
	mutex_unlock(&data->lock);
	return count;
}

/* sampling frequency - output rate in Hz */
static const char * const reg_to_rate[] = {
	"50",
	"100",
	"400",
	"1000"
};

static ssize_t show_sampling_frequency(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct lsm303dlh_a_data *data = iio_priv(dev_get_drvdata(dev));

	return sprintf(buf, "%s\n", reg_to_rate[data->rate]);
}

static IIO_CONST_ATTR(accel_xyz_scale_available, "2, 4, 8");

static const int xyz_to_scale[] = {
		2, 4, 8
};

static const char const scale_to_range[] = {
		LSM303DLH_A_RANGE_2G,
		LSM303DLH_A_RANGE_4G,
		LSM303DLH_A_RANGE_8G,
};

static int lsm303dlh_a_write_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int val, int val2,
				long mask)
{
	struct lsm303dlh_a_data *data = iio_priv(indio_dev);
	int ret = -EINVAL, i;
	bool flag = false;
	char end;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		ret = is_device_on(data);
		if (ret)
			return -EINVAL;
		mutex_lock(&data->lock);
		end = ARRAY_SIZE(xyz_to_scale);
		for (i = 0; i < end; i++) {
			if (val == xyz_to_scale[i]) {
				flag = true;
				break;
			}
		}
		if (flag) {
			ret = lsm303dlh_a_set_range(data->client,
							scale_to_range[i]);
			if (ret < 0) {
				mutex_unlock(&data->lock);
				return -EINVAL;
			}
			ret = lsm303dlh_a_set_shift(data, scale_to_range[i]);
			data->range = i;
		}
		mutex_unlock(&data->lock);
		break;
	default:
		break;
	}
	return ret;
}

static int lsm303dlh_a_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct lsm303dlh_a_data *data = iio_priv(indio_dev);

	switch (mask) {
	case 0:
		return lsm303dlh_a_xyz_read(indio_dev,
				chan->address, val);

	case IIO_CHAN_INFO_SCALE:
		*val = xyz_to_scale[data->range];
		return IIO_VAL_INT;

	default:
		break;
	}
	return -EINVAL;
}

#define LSM303DLH_CHANNEL(axis, addr)				\
	{							\
		.type = IIO_ACCEL,				\
		.modified = 1,					\
		.channel2 = IIO_MOD_##axis,			\
		.info_mask = IIO_CHAN_INFO_SCALE_SHARED_BIT,	\
		.address = addr,				\
	}

static const struct iio_chan_spec lsmdlh303_channels[] = {
	LSM303DLH_CHANNEL(X, LSM303DLH_A_OUT_X_L_A),
	LSM303DLH_CHANNEL(Y, LSM303DLH_A_OUT_Y_L_A),
	LSM303DLH_CHANNEL(Z, LSM303DLH_A_OUT_Z_L_A),
};


static IIO_DEVICE_ATTR(accel_raw,
			S_IRUGO,
			lsm303dlh_a_readdata,
			NULL,
			LSM303DLH_A_OUT_X_L_A);
static IIO_DEVICE_ATTR(sampling_frequency,
			S_IWUSR | S_IRUGO,
			show_sampling_frequency,
			set_sampling_frequency,
			LSM303DLH_A_CTRL_REG1_A);
static IIO_DEVICE_ATTR(mode,
			S_IWUSR | S_IRUGO,
			show_operating_mode,
			set_operating_mode,
			LSM303DLH_A_CTRL_REG1_A);
static IIO_DEVICE_ATTR(id,
			S_IRUGO,
			show_chip_id,
			NULL, 0);

static struct attribute *lsm303dlh_a_attributes[] = {
	&iio_dev_attr_mode.dev_attr.attr,
	&iio_dev_attr_sampling_frequency.dev_attr.attr,
	&iio_dev_attr_id.dev_attr.attr,
	&iio_dev_attr_accel_raw.dev_attr.attr,
	&iio_const_attr_sampling_frequency_available.dev_attr.attr,
	&iio_const_attr_accel_xyz_scale_available.dev_attr.attr,
	NULL
};

static const struct attribute_group lsmdlh303a_group = {
	.attrs = lsm303dlh_a_attributes,
};

static const struct iio_info lsmdlh303a_info = {
	.attrs = &lsmdlh303a_group,
	.read_raw = &lsm303dlh_a_read_raw,
	.write_raw = &lsm303dlh_a_write_raw,
	.driver_module = THIS_MODULE,
};

static void lsm303dlh_a_setup(struct lsm303dlh_a_data *data)
{
	/* set mode */
	lsm303dlh_a_set_mode(data->client, data->mode);
	/* set rate */
	lsm303dlh_a_set_rate(data->client, data->rate);
	/* set range */
	lsm303dlh_a_set_range(data->client, scale_to_range[data->range]);
	/* set boot bit to reboot memory content */
	lsm303dlh_a_setbootbit(data->client, LSM303DLH_A_CR2_BOOT_ENABLE);
}

#if (!defined(CONFIG_HAS_EARLYSUSPEND) && defined(CONFIG_PM))
static int lsm303dlh_a_suspend(struct device *dev)
{
	struct lsm303dlh_a_data *data = iio_priv(dev_get_drvdata(dev));
	int ret = 0;

	if (data->mode == LSM303DLH_A_MODE_OFF)
		return 0;

	mutex_lock(&data->lock);

	/* Set the device to sleep mode */
	lsm303dlh_a_set_mode(data->client, LSM303DLH_A_MODE_OFF);

	/* Disable regulator */
	lsm303dlh_a_disable(data);

	data->device_status = LSM303DLH_A_DEVICE_SUSPENDED;

	mutex_unlock(&data->lock);

	return ret;
}

static int lsm303dlh_a_resume(struct device *dev)
{
	struct lsm303dlh_a_data *data = iio_priv(dev_get_drvdata(dev));
	int ret = 0;


	if (data->device_status == LSM303DLH_A_DEVICE_ON ||
			data->device_status == LSM303DLH_A_DEVICE_OFF) {
		return 0;
	}
	mutex_lock(&data->lock);

	/* Enable regulator */
	lsm303dlh_a_enable(data);

	/* Set mode,rate and range */
	lsm303dlh_a_setup(data);

	mutex_unlock(&data->lock);
	return ret;
}

static const struct dev_pm_ops lsm303dlh_a_dev_pm_ops = {
	.suspend = lsm303dlh_a_suspend,
	.resume  = lsm303dlh_a_resume,
};
#endif
#ifdef CONFIG_HAS_EARLYSUSPEND
static void lsm303dlh_a_early_suspend(struct early_suspend *data)
{
	struct lsm303dlh_a_data *ddata =
		container_of(data, struct lsm303dlh_a_data, early_suspend);

	if (ddata->mode == LSM303DLH_A_MODE_OFF)
		return;

	mutex_lock(&ddata->lock);

	/* Set the device to sleep mode */
	lsm303dlh_a_set_mode(ddata->client, LSM303DLH_A_MODE_OFF);

	/* Disable regulator */
	lsm303dlh_a_disable(ddata);

	ddata->device_status = LSM303DLH_A_DEVICE_SUSPENDED;

	mutex_unlock(&ddata->lock);

}

static void lsm303dlh_a_late_resume(struct early_suspend *data)
{
	struct lsm303dlh_a_data *ddata =
		container_of(data, struct lsm303dlh_a_data, early_suspend);


	if (ddata->device_status == LSM303DLH_A_DEVICE_ON ||
			ddata->device_status == LSM303DLH_A_DEVICE_OFF) {
		return;
	}
	mutex_lock(&ddata->lock);

	/* Enable regulator */
	lsm303dlh_a_enable(ddata);

	/* Set mode,rate and range */
	lsm303dlh_a_setup(ddata);

	mutex_unlock(&ddata->lock);

}
#endif

static int lsm303dlh_a_probe(struct i2c_client *client,
					const struct i2c_device_id *id)
{
	struct lsm303dlh_a_data *data;
	struct iio_dev *indio_dev;
	int err = 0;

	indio_dev = iio_allocate_device(sizeof(*data));
	if (indio_dev == NULL) {
		dev_err(&client->dev, "memory allocation failed\n");
		err = -ENOMEM;
		goto exit;
	}
	data = iio_priv(indio_dev);

	data->mode = LSM303DLH_A_MODE_OFF;
	data->range = LSM303DLH_A_RANGE_2G;
	data->rate = LSM303DLH_A_RATE_50;
	data->device_status = LSM303DLH_A_DEVICE_OFF;
	data->client = client;

	/* check for valid platform data */
	if (!client->dev.platform_data) {
		dev_err(&client->dev, "Invalid platform data\n");
		err = -ENOMEM;
		goto exit1;
	}
	data->pdata = client->dev.platform_data;

	i2c_set_clientdata(client, indio_dev);

	data->regulator = regulator_get(&client->dev, "vdd");
	if (IS_ERR(data->regulator)) {
		err = PTR_ERR(data->regulator);
		dev_err(&client->dev, "failed to get regulator = %d\n", err);
		goto exit1;
	}
	/* Enable regulator */
	lsm303dlh_a_enable(data);

	lsm303dlh_a_setup(data);

	mutex_init(&data->lock);
	indio_dev->info = &lsmdlh303a_info;
	indio_dev->name = id->name;
	indio_dev->dev.parent = &client->dev;
	indio_dev->channels = lsmdlh303_channels;
	indio_dev->num_channels = ARRAY_SIZE(lsmdlh303_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	err = iio_device_register(indio_dev);
	if (err)
		goto exit2;

#ifdef CONFIG_HAS_EARLYSUSPEND
	data->early_suspend.level =
				EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	data->early_suspend.suspend = lsm303dlh_a_early_suspend;
	data->early_suspend.resume = lsm303dlh_a_late_resume;
	register_early_suspend(&data->early_suspend);
#endif
	/* Disable regulator */
	lsm303dlh_a_disable(data);

	return 0;

exit2:
	iio_free_device(indio_dev);
	mutex_destroy(&data->lock);
	regulator_disable(data->regulator);
	regulator_put(data->regulator);
exit1:
	kfree(data);
exit:
	return err;
}

static int __devexit lsm303dlh_a_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct lsm303dlh_a_data *data = iio_priv(indio_dev);
	int ret;

	/* safer to make device off */
	if (data->mode != LSM303DLH_A_MODE_OFF) {
		/* set mode to off */
		ret = lsm303dlh_a_set_mode(client, LSM303DLH_A_MODE_OFF);
		if (ret < 0) {
			dev_err(&client->dev, "could not turn off the device %d",
							ret);
			return ret;
		}
		if (data->device_status == LSM303DLH_A_DEVICE_ON) {
			regulator_disable(data->regulator);
			data->device_status = LSM303DLH_A_DEVICE_OFF;
		}
	}
	regulator_put(data->regulator);
	mutex_destroy(&data->lock);
	iio_device_unregister(indio_dev);
	iio_free_device(indio_dev);
	kfree(data);
	return 0;
}

static const struct i2c_device_id lsm303dlh_a_id[] = {
	{ "lsm303dlh_a", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, lsm303dlh_a_id);

static struct i2c_driver lsm303dlh_a_driver = {
	.driver = {
		.name	= "lsm303dlh_a",
	#if (!defined(CONFIG_HAS_EARLYSUSPEND) && defined(CONFIG_PM))
		.pm = &lsm303dlh_a_dev_pm_ops,
	#endif
	},
	.id_table	= lsm303dlh_a_id,
	.probe		= lsm303dlh_a_probe,
	.remove		= lsm303dlh_a_remove,
};

module_i2c_driver(lsm303dlh_a_driver);

MODULE_DESCRIPTION("lsm303dlh Accelerometer Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Naga Radhesh Y <naga.radheshy@stericsson.com>");
