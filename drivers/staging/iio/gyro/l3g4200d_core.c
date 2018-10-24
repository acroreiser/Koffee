/*
 * Copyright (C) ST-Ericsson SA 2012
 * License Terms: GNU General Public License, version 2
 *
 * Mostly this gyroscope device is a copy of magnetometer
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
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include <linux/l3g4200d.h>

#include "../iio.h"
#include "../sysfs.h"

/* Idenitification register */
#define L3G4200D_WHO_AM_I	0x0F
/* control register1  */
#define L3G4200D_CTRL_REG1	0x20
/* control register2 */
#define L3G4200D_CTRL_REG2	0x21
/* control register3 */
#define L3G4200D_CTRL_REG3	0x22
/* control register4 */
#define L3G4200D_CTRL_REG4	0x23
/* control register5 */
#define L3G4200D_CTRL_REG5	0x24
/* out temperature */
#define L3G4200D_OUT_TEMP	0x26
/* data output X register */
#define L3G4200D_OUT_X_L_A	0x28
/* data output Y register */
#define L3G4200D_OUT_Y_L_A	0x2A
/* data output Z register */
#define L3G4200D_OUT_Z_L_A	0x2C
/* status register */
#define L3G4200D_STATUS_REG_A	0x27

/* control register 1, Mode selection */
#define L3G4200D_CR1_PM_BIT 3
#define L3G4200D_CR1_PM_MASK (0x01 << L3G4200D_CR1_PM_BIT)
/* control register 1, Data Rate */
#define L3G4200D_CR1_DR_BIT 4
#define L3G4200D_CR1_DR_MASK (0x0F << L3G4200D_CR1_DR_BIT)
/* control register 1, x,y,z enable bits */
#define L3G4200D_CR1_EN_BIT 0
#define L3G4200D_CR1_EN_MASK (0x7 << L3G4200D_CR1_EN_BIT)
#define L3G4200D_CR1_AXIS_ENABLE 7

/* control register 4, self test */
#define L3G4200D_CR4_ST_BIT 1
#define L3G4200D_CR4_ST_MASK (0x03 << L3G4200D_CR4_ST_BIT)
/* control register 4, full scale */
#define L3G4200D_CR4_FS_BIT 4
#define L3G4200D_CR4_FS_MASK (0x3 << L3G4200D_CR4_FS_BIT)
/* control register 4, endianness */
#define L3G4200D_CR4_BLE_BIT 6
#define L3G4200D_CR4_BLE_MASK (0x1 << L3G4200D_CR4_BLE_BIT)
/* control register 4, Block data update */
#define L3G4200D_CR4_BDU_BIT 7
#define L3G4200D_CR4_BDU_MASK (0x1 << L3G4200D_CR4_BDU_BIT)

/* Gyroscope operating mode */
#define L3G4200D_MODE_OFF	0x00
#define L3G4200D_MODE_NORMAL	0x01

/* Expected content for WAI register */
#define WHOAMI_L3G4200D		0x00D3
/* Expected content for WAI register for L3GD20*/
#define WHOAMI_L3GD20		0x00D4

/*
 * CTRL_REG1 register rate settings
 *
 * DR1 DR0 BW1 BW0     Output data rate[Hz]
 * 0   0   0    0       100
 * 0   0   0    1       100
 * 0   0   1    0       100
 * 0   0   1    1       100
 * 0   1   0    0       200
 * 0   1   0    1       200
 * 0   1   1    0       200
 * 0   1   1    1       200
 * 1   0   0    0       400
 * 1   0   0    1       400
 * 1   0   1    0       400
 * 1   0   1    1       400
 * 1   1   0    0       800
 * 1   1   0    1       800
 * 1   1   1    0       800
 * 1   1   1    1       800
 */
#define L3G4200D_ODR_MIN_VAL	0x00
#define L3G4200D_ODR_MAX_VAL	0x0F
#define L3G4200D_RATE_100	0x00
#define L3G4200D_RATE_200	0x04
#define L3G4200D_RATE_400	0x08
#define L3G4200D_RATE_800	0x0C

/*
 * CTRL_REG4 register range settings
 *
 * FS1 FS0	FUll scale range
 * 0   0           250
 * 0   1           500
 * 1   0           2000
 * 1   1           2000
 */
#define L3G4200D_RANGE_250	0x00
#define L3G4200D_RANGE_500	0x01
#define L3G4200D_RANGE_2000	0x03

/* device status defines */
#define L3G4200D_DEVICE_OFF	0
#define L3G4200D_DEVICE_ON	1
#define L3G4200D_DEVICE_SUSPENDED	2

/* status register */
#define L3G4200D_SR_REG_A	0x27
/* status register, ready  */
#define L3G4200D_XYZ_DATA_RDY	0x80
#define L3G4200D_XYZ_DATA_RDY_BIT	3
#define L3G4200D_XYZ_DATA_RDY_MASK	(0x1 << L3G4200D_XYZ_DATA_RDY_BIT)

/* Multiple byte transfer enable */
#define MULTIPLE_I2C_TR		0x80

/*
 * struct l3g4200d_data - data structure used by l3g4200d driver
 * @client: i2c client
 * @lock: mutex lock for sysfs operations
 * @regulator: regulator
 * @early_suspend: early suspend structure
 * @pdata: l3g4200d platform data
 * @mode: current mode of operation
 * @rate: current sampling rate
 * @range: current range value of Gyroscope
 * @device_status: device is ON, OFF or SUSPENDED
 */

struct l3g4200d_data {
	struct i2c_client	*client;
	struct mutex		lock;
	struct regulator	*regulator;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
	struct l3g4200d_gyr_platform_data *pdata;

	u8			mode;
	u8			rate;
	u8			range;
	int			device_status;
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void l3g4200d_early_suspend(struct early_suspend *data);
static void l3g4200d_late_resume(struct early_suspend *data);
#endif
static inline int is_device_on(struct l3g4200d_data *data)
{
	struct i2c_client *client = data->client;
	/* Perform read/write operation only when device is active */
	if (data->device_status != L3G4200D_DEVICE_ON) {
		dev_err(&client->dev,
			"device is switched off, make it on using mode");
		return -EINVAL;
	}

	return 0;
}

/* To disable regulator and status */
static int l3g4200d_disable(struct l3g4200d_data *data)
{
	data->device_status = L3G4200D_DEVICE_OFF;

	regulator_disable(data->regulator);

	return 0;
}

/* To enable regulator and status */
static int l3g4200d_enable(struct l3g4200d_data *data)
{
	data->device_status = L3G4200D_DEVICE_ON;

	regulator_enable(data->regulator);

	return 0;
}

static s32 l3g4200d_set_mode(struct i2c_client *client,	u8 mode)
{
	int reg_val;

	if (mode > L3G4200D_MODE_NORMAL) {
		dev_err(&client->dev, "given mode not supported\n");
		return -EINVAL;
	}

	reg_val = i2c_smbus_read_byte_data(client, L3G4200D_CTRL_REG1);

	reg_val |= L3G4200D_CR1_AXIS_ENABLE;
	reg_val &= ~L3G4200D_CR1_PM_MASK;

	reg_val |= ((mode << L3G4200D_CR1_PM_BIT) & L3G4200D_CR1_PM_MASK);

	/* the 4th bit indicates the gyroscope sensor mode */
	return i2c_smbus_write_byte_data(client, L3G4200D_CTRL_REG1, reg_val);
}

static s32 l3g4200d_set_rate(struct i2c_client *client, u8 rate)
{
	int reg_val;

	if (rate > L3G4200D_ODR_MAX_VAL) {
		dev_err(&client->dev, "given rate not supported\n");
		return -EINVAL;
	}
	reg_val = i2c_smbus_read_byte_data(client, L3G4200D_CTRL_REG1);

	reg_val &= ~L3G4200D_CR1_DR_MASK;

	reg_val |= ((rate << L3G4200D_CR1_DR_BIT) & L3G4200D_CR1_DR_MASK);

	/* upper 4 bits indicate ODR of Gyroscope */
	return i2c_smbus_write_byte_data(client, L3G4200D_CTRL_REG1, reg_val);
}

static s32 l3g4200d_set_range(struct i2c_client *client, u8 range)
{
	int reg_val;

	if (range > L3G4200D_RANGE_2000) {
		dev_err(&client->dev, "given range not supported\n");
		return -EINVAL;
	}

	reg_val = (range << L3G4200D_CR4_FS_BIT);
	/*
	 * If BDU is enabled, output registers are not updated until MSB
	 * and LSB reading completes.Otherwise we will end up reading
	 * wrong data.
	 */
	reg_val |= L3G4200D_CR4_BDU_MASK;

	/* 5th and 6th bits indicate rate of gyroscope */
	return i2c_smbus_write_byte_data(client, L3G4200D_CTRL_REG4, reg_val);
}

/*
 * To read output x/y/z data register,
 * in this case x,y and z are not
 * mapped w.r.t board orientation.
 * Reading just raw data from device
 */
static ssize_t l3g4200d_xyz_read(struct iio_dev *indio_dev,
					int address,
					int *buf)
{

	struct l3g4200d_data *data = iio_priv(indio_dev);
	int lsb , msb;
	int ret;
	s16 val;

	/* Perform read/write operation, only when device is active */
	ret = is_device_on(data);
	if (ret)
		return -EINVAL;
	mutex_lock(&data->lock);

	ret = i2c_smbus_read_byte_data(data->client, L3G4200D_SR_REG_A);

	/* wait till data is written to all six registers */
	while (!(ret & L3G4200D_XYZ_DATA_RDY_MASK))
		ret = i2c_smbus_read_byte_data(data->client, L3G4200D_SR_REG_A);

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

	*buf = (s16)val;
	mutex_unlock(&data->lock);

	return IIO_VAL_INT;
}

/*
 * To read output x,y,z data register.
 * After reading change x,y and z values
 * w.r.t the orientation of the device.
 */
static ssize_t l3g4200d_readdata(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{

	struct l3g4200d_data *data = iio_priv(dev_get_drvdata(dev));
	struct l3g4200d_gyr_platform_data *pdata = data->pdata;
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	u8 map_x = pdata->axis_map_x;
	u8 map_y = pdata->axis_map_y;
	u8 map_z = pdata->axis_map_z;
	int ret;
	unsigned char gyr_data[6];
	s16 val[3];

	/* Perform read/write operation, only when device is active */
	ret = is_device_on(data);
	if (ret)
		return -EINVAL;

	mutex_lock(&data->lock);

	ret = i2c_smbus_read_byte_data(data->client, L3G4200D_SR_REG_A);
	/* wait till data is written to all six registers */
	while (!((ret & L3G4200D_XYZ_DATA_RDY_MASK)))
		ret = i2c_smbus_read_byte_data(data->client, L3G4200D_SR_REG_A);

	ret = i2c_smbus_read_i2c_block_data(data->client,
			this_attr->address | MULTIPLE_I2C_TR, 6, gyr_data);

	if (ret < 0) {
		dev_err(&data->client->dev, "reading xyz failed\n");
		mutex_unlock(&data->lock);
		return -EINVAL;
	}

	/* MSB is at lower address */
	val[0] = (s16)
		(((gyr_data[1]) << 8) | gyr_data[0]);
	val[1] = (s16)
		(((gyr_data[3]) << 8) | gyr_data[2]);
	val[2] = (s16)
		(((gyr_data[5]) << 8) | gyr_data[4]);

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

static ssize_t get_gyrotemp(struct iio_dev *indio_dev,
					int address,
					int *buf)
{
	struct l3g4200d_data *data = iio_priv(indio_dev);
	int ret;
	/* Perform read/write operation, only when device is active */
	ret = is_device_on(data);
	if (ret)
		return -EINVAL;

	ret = i2c_smbus_read_byte_data(data->client, address);
	if (ret < 0) {
		dev_err(&data->client->dev, "Error in reading Gyro temperature");
		return ret;
	}
	*buf = ret;

	return IIO_VAL_INT;
}

static ssize_t show_operating_mode(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct l3g4200d_data *data = iio_priv(dev_get_drvdata(dev));

	return sprintf(buf, "%d\n", data->mode);
}

static ssize_t set_operating_mode(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct l3g4200d_data *data = iio_priv(dev_get_drvdata(dev));
	int error;
	unsigned long mode = 0;

	mutex_lock(&data->lock);

	error = kstrtoul(buf, 10, &mode);
	if (error) {
		count = error;
		goto exit;
	}

	/* check if the received power mode is either 0 or 1 */
	if (mode < L3G4200D_MODE_OFF ||  mode > L3G4200D_MODE_NORMAL) {
		dev_err(&data->client->dev, "trying to set invalid mode\n");
		count = -EINVAL;
		goto exit;
	}
	/*
	 * If device is drived to sleep mode in suspend, update mode
	 * and return
	 */
	if (data->device_status == L3G4200D_DEVICE_SUSPENDED &&
			mode == L3G4200D_MODE_OFF) {
		data->mode = mode;
		goto exit;
	}

	 /*  if same mode as existing, return */
	if (data->mode == mode)
		goto exit;

	/* Enable the regulator if it is not turned ON earlier */
	if (data->device_status == L3G4200D_DEVICE_OFF ||
		data->device_status == L3G4200D_DEVICE_SUSPENDED)
		l3g4200d_enable(data);

	dev_dbg(dev, "set operating mode to %lu\n", mode);
	error = l3g4200d_set_mode(data->client, mode);
	if (error < 0) {
		dev_err(&data->client->dev, "Error in setting the mode\n");
		count = -EINVAL;
		goto exit;
	}

	data->mode = mode;

	/* If mode is OFF then disable the regulator */
	if (data->mode == L3G4200D_MODE_OFF) {
		/* fall back to default values */
		data->rate = L3G4200D_RATE_100;
		data->range = L3G4200D_ODR_MIN_VAL;
		l3g4200d_disable(data);
	}
exit:
	mutex_unlock(&data->lock);
	return count;
}

static IIO_CONST_ATTR_SAMP_FREQ_AVAIL("100 200 400 800");

static ssize_t set_sampling_frequency(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct l3g4200d_data *data = iio_priv(dev_get_drvdata(dev));
	unsigned long rate = 0;
	int err;

	/* Perform read/write operation, only when device is active */
	err = is_device_on(data);
	if (err)
		return -EINVAL;

	if (strncmp(buf, "100" , 3) == 0)
		rate = L3G4200D_RATE_100;

	else if (strncmp(buf, "200" , 3) == 0)
		rate = L3G4200D_RATE_200;

	else if (strncmp(buf, "400" , 3) == 0)
		rate = L3G4200D_RATE_400;

	else if (strncmp(buf, "800" , 3) == 0)
		rate = L3G4200D_RATE_800;
	else
		return -EINVAL;

	mutex_lock(&data->lock);

	if (l3g4200d_set_rate(data->client, rate)) {
		dev_err(&data->client->dev, "set rate failed\n");
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
	"100",
	"100",
	"100",
	"100",
	"200",
	"200",
	"200",
	"200",
	"400",
	"400",
	"400",
	"400",
	"800",
	"800",
	"800",
	"800"
};

static ssize_t show_sampling_frequency(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct l3g4200d_data *data = iio_priv(dev_get_drvdata(dev));

	return sprintf(buf, "%s\n", reg_to_rate[data->rate]);
}

static IIO_CONST_ATTR(gyro_xyz_scale_available, "8750000, 17500000, 70000000");

static const int xyz_to_nanoscale[] = {
	8750000, 17500000, 70000000
};

static const char const scale_to_range[] = {
		L3G4200D_RANGE_250,
		L3G4200D_RANGE_500,
		L3G4200D_RANGE_2000,
};

static int l3g4200d_write_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int val, int val2,
				long mask)
{
	struct l3g4200d_data *data = iio_priv(indio_dev);
	int ret = -EINVAL, i;
	bool flag = false;
	char end;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		ret = is_device_on(data);
		if (ret)
			return -EINVAL;
		mutex_lock(&data->lock);
		end = ARRAY_SIZE(xyz_to_nanoscale);
		for (i = 0; i < end; i++) {
			if (val == xyz_to_nanoscale[i]) {
				flag = true;
				break;
			}
		}
		if (flag) {
			ret = l3g4200d_set_range(data->client, scale_to_range[i]);
			data->range = i;
		}
		mutex_unlock(&data->lock);
	default:
		break;
	}
	return ret;
}

static int l3g4200d_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct l3g4200d_data *data = iio_priv(indio_dev);
	switch (mask) {
	case 0:
		switch (chan->type) {
		case IIO_ANGL_VEL:
			return l3g4200d_xyz_read(indio_dev,
							chan->address, val);
		case IIO_TEMP:
			return get_gyrotemp(indio_dev, chan->address , val);
		default:
			break;
		}
		break;
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ANGL_VEL:
			*val = 0;
			/* scale for X/Y and Z are different */
			*val2 = xyz_to_nanoscale[data->range];
			return IIO_VAL_INT_PLUS_NANO;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return -EINVAL;
}

#define L3G4200D_CHANNEL(axis, addr)				\
	{							\
		.type = IIO_ANGL_VEL,				\
		.modified = 1,					\
		.channel2 = IIO_MOD_##axis,			\
		.info_mask = IIO_CHAN_INFO_SCALE_SHARED_BIT,	\
		.address = addr,				\
	}

#define L3G4200D_TEMP_CHANNEL(addr)				\
	{ \
		.type = IIO_TEMP,	\
		.indexed = 1,			\
		.channel = 0,			\
		.info_mask = IIO_CHAN_INFO_SCALE_SHARED_BIT,	\
		.address = addr, \
	}

static const struct iio_chan_spec l3g4200d_channels[] = {
	L3G4200D_CHANNEL(X, L3G4200D_OUT_X_L_A),
	L3G4200D_CHANNEL(Y, L3G4200D_OUT_Y_L_A),
	L3G4200D_CHANNEL(Z, L3G4200D_OUT_Z_L_A),
	L3G4200D_TEMP_CHANNEL(L3G4200D_OUT_TEMP),
};

static IIO_DEVICE_ATTR(gyro_raw,
			S_IRUGO,
			l3g4200d_readdata,
			NULL,
			L3G4200D_OUT_X_L_A);
static IIO_DEVICE_ATTR(sampling_frequency,
			S_IWUGO | S_IRUGO,
			show_sampling_frequency,
			set_sampling_frequency,
			L3G4200D_CTRL_REG1);
static IIO_DEVICE_ATTR(mode,
			S_IWUGO | S_IRUGO,
			show_operating_mode,
			set_operating_mode,
			L3G4200D_CTRL_REG1);

static struct attribute *l3g4200d_attributes[] = {
	&iio_dev_attr_mode.dev_attr.attr,
	&iio_dev_attr_sampling_frequency.dev_attr.attr,
	&iio_dev_attr_gyro_raw.dev_attr.attr,
	&iio_const_attr_sampling_frequency_available.dev_attr.attr,
	&iio_const_attr_gyro_xyz_scale_available.dev_attr.attr,
	NULL
};

static const struct attribute_group l3g4200d_group = {
	.attrs = l3g4200d_attributes,
};

static const struct iio_info l3g4200d_info = {
	.attrs = &l3g4200d_group,
	.read_raw = &l3g4200d_read_raw,
	.write_raw = &l3g4200d_write_raw,
	.driver_module = THIS_MODULE,
};

static void l3g4200d_setup(struct l3g4200d_data *data)
{
	/* set mode */
	l3g4200d_set_mode(data->client, data->mode);
	/* set rate */
	l3g4200d_set_rate(data->client, data->rate);
	/* set range */
	l3g4200d_set_range(data->client, scale_to_range[data->range]);
}

#if (!defined(CONFIG_HAS_EARLYSUSPEND) && defined(CONFIG_PM))
static int l3g4200d_suspend(struct device *dev)
{
	struct l3g4200d_data *data = iio_priv(dev_get_drvdata(dev));
	int ret = 0;

	if (data->mode == L3G4200D_MODE_OFF)
		return 0;

	mutex_lock(&data->lock);

	/* Set the device to sleep mode */
	l3g4200d_set_mode(data->client, L3G4200D_MODE_OFF);

	/* Disable regulator */
	l3g4200d_disable(data);

	data->device_status = L3G4200D_DEVICE_SUSPENDED;

	mutex_unlock(&data->lock);

	return ret;
}

static int l3g4200d_resume(struct device *dev)
{
	struct l3g4200d_data *data = iio_priv(dev_get_drvdata(dev));
	int ret = 0;


	if (data->device_status == L3G4200D_DEVICE_ON ||
			data->device_status == L3G4200D_DEVICE_OFF) {
		return 0;
	}
	mutex_lock(&data->lock);

	/* Enable regulator */
	l3g4200d_enable(data);

	/* Set mode,rate and range */
	l3g4200d_setup(data);

	mutex_unlock(&data->lock);
	return ret;
}

static const struct dev_pm_ops l3g4200d_dev_pm_ops = {
	.suspend = l3g4200d_suspend,
	.resume  = l3g4200d_resume,
};
#endif
#ifdef CONFIG_HAS_EARLYSUSPEND
static void l3g4200d_early_suspend(struct early_suspend *data)
{
	struct l3g4200d_data *ddata =
		container_of(data, struct l3g4200d_data, early_suspend);

	if (ddata->mode == L3G4200D_MODE_OFF)
		return;

	mutex_lock(&ddata->lock);

	/* Set the device to sleep mode */
	l3g4200d_set_mode(ddata->client, L3G4200D_MODE_OFF);

	/* Disable regulator */
	l3g4200d_disable(ddata);

	ddata->device_status = L3G4200D_DEVICE_SUSPENDED;

	mutex_unlock(&ddata->lock);

}

static void l3g4200d_late_resume(struct early_suspend *data)
{
	struct l3g4200d_data *ddata =
		container_of(data, struct l3g4200d_data, early_suspend);

	if (ddata->device_status == L3G4200D_DEVICE_ON ||
			ddata->device_status == L3G4200D_DEVICE_OFF) {
		return;
	}
	mutex_lock(&ddata->lock);

	/* Enable regulator */
	l3g4200d_enable(ddata);

	/* Set mode,rate and range */
	l3g4200d_setup(ddata);

	mutex_unlock(&ddata->lock);

}
#endif

static int l3g4200d_probe(struct i2c_client *client,
					const struct i2c_device_id *id)
{
	struct l3g4200d_data *data;
	struct iio_dev *indio_dev;
	int err = 0;

	indio_dev = iio_allocate_device(sizeof(*data));
	if (indio_dev == NULL) {
		dev_err(&client->dev, "memory allocation failed\n");
		err = -ENOMEM;
		goto exit;
	}
	data = iio_priv(indio_dev);

	data->mode = L3G4200D_MODE_OFF;
	data->range = L3G4200D_RANGE_250;
	data->rate = L3G4200D_ODR_MIN_VAL;
	data->device_status = L3G4200D_DEVICE_OFF;
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
	l3g4200d_enable(data);

	mutex_init(&data->lock);

	err = i2c_smbus_read_byte_data(client, L3G4200D_WHO_AM_I);
	if (err < 0) {
		dev_err(&client->dev, "failed to read of the chip\n");
		goto exit2;
	}
	if (err == WHOAMI_L3G4200D || err == WHOAMI_L3GD20)
		dev_info(&client->dev,
				"3-Axis Gyroscope device identification: %d\n",
				err);
	else {
		dev_info(&client->dev,
				"Gyroscope identification did not match\n");
		goto exit2;
	}

	l3g4200d_setup(data);

	indio_dev->info = &l3g4200d_info;
	indio_dev->name = id->name;
	indio_dev->dev.parent = &client->dev;
	indio_dev->channels = l3g4200d_channels;
	indio_dev->num_channels = ARRAY_SIZE(l3g4200d_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	err = iio_device_register(indio_dev);
	if (err)
		goto exit2;

#ifdef CONFIG_HAS_EARLYSUSPEND
	data->early_suspend.level =
				EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	data->early_suspend.suspend = l3g4200d_early_suspend;
	data->early_suspend.resume = l3g4200d_late_resume;
	register_early_suspend(&data->early_suspend);
#endif

	/* Disable regulator */
	l3g4200d_disable(data);

	return 0;

exit2:
	iio_free_device(indio_dev);
	mutex_destroy(&data->lock);
	l3g4200d_disable(data);
	regulator_put(data->regulator);
exit1:
	kfree(data);
exit:
	return err;
}

static int __devexit l3g4200d_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct l3g4200d_data *data = iio_priv(indio_dev);
	int ret;

	/* safer to make device off */
	if (data->mode != L3G4200D_MODE_OFF) {
		/* set mode to off */
		ret = l3g4200d_set_mode(client, L3G4200D_MODE_OFF);
		if (ret < 0) {
			dev_err(&client->dev,
					"could not turn off the device %d",
					ret);
			return ret;
		}
		if (data->device_status == L3G4200D_DEVICE_ON) {
			regulator_disable(data->regulator);
			data->device_status = L3G4200D_DEVICE_OFF;
		}
	}
	regulator_put(data->regulator);
	mutex_destroy(&data->lock);
	iio_device_unregister(indio_dev);
	iio_free_device(indio_dev);
	kfree(data);
	return 0;
}

static const struct i2c_device_id l3g4200d_id[] = {
	{ "l3g4200d", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, l3g4200d_id);

static struct i2c_driver l3g4200d_driver = {
	.driver = {
		.name	= "l3g4200d",
	#if (!defined(CONFIG_HAS_EARLYSUSPEND) && defined(CONFIG_PM))
		.pm = &l3g4200d_dev_pm_ops,
	#endif
	},
	.id_table	= l3g4200d_id,
	.probe		= l3g4200d_probe,
	.remove		= l3g4200d_remove,
};

module_i2c_driver(l3g4200d_driver);

MODULE_DESCRIPTION("l3g4200d Gyroscope Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Naga Radhesh Y <naga.radheshy@stericsson.com>");
