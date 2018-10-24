/*
 * Copyright (C) ST-Ericsson SA 2012
 * License Terms: GNU General Public License, version 2
 *
 * This code is mostly based on hmc5843 driver
 *
 * Author: Srinidhi Kasagar <srinidhi.kasagar@stericsson.com>
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include <linux/lsm303dlh.h>

#include "../iio.h"
#include "../sysfs.h"

/* configuration register A */
#define LSM303DLH_M_CRA_REG	0x00
/* configuration register B */
#define LSM303DLH_M_CRB_REG	0x01
/* mode register */
#define LSM303DLH_M_MR_REG	0x02
/* data output X register */
#define LSM303DLH_M_OUT_X	0x03
/* data output Y register */
#define LSM303DLH_M_OUT_Y	0x05
/* data output Z register */
#define LSM303DLH_M_OUT_Z	0x07
/* status register */
#define LSM303DLH_M_SR_REG	0x09
/* identification registers */
#define LSM303DLH_M_IRA_REG	0x0A
#define LSM303DLH_M_IRB_REG	0x0B
#define LSM303DLH_M_IRC_REG	0x0C

/* control register A, Data Output rate */
#define LSM303DLH_M_CRA_DO_BIT	2
#define LSM303DLH_M_CRA_DO_MASK	(0x7 << LSM303DLH_M_CRA_DO_BIT)
/* control register A, measurement configuration */
#define LSM303DLH_M_CRA_MS_BIT	0
#define LSM303DLH_M_CRA_MS_MASK	(0x3 << LSM303DLH_M_CRA_MS_BIT)
/* control register B, gain configuration */
#define LSM303DLH_M_CRB_GN_BIT	5
#define LSM303DLH_M_CRB_GN_MASK	(0x7 << LSM303DLH_M_CRB_GN_BIT)
/* mode register */
#define LSM303DLH_M_MR_MD_BIT	0
#define LSM303DLH_M_MR_MD_MASK	(0x3 << LSM303DLH_M_MR_MD_BIT)
/* status register, ready  */
#define LSM303DLH_M_SR_RDY_BIT	0
#define LSM303DLH_M_SR_RDY_MASK	(0x1 << LSM303DLH_M_SR_RDY_BIT)
/* status register, data output register lock */
#define LSM303DLH_M_SR_LOC_BIT	1
#define LSM303DLH_M_SR_LOC_MASK	(0x1 << LSM303DLH_M_SR_LOC_BIT)
/* status register, regulator enabled */
#define LSM303DLH_M_SR_REN_BIT	2
#define LSM303DLH_M_SR_REN_MASK	(0x1 << LSM303DLH_M_SR_REN_BIT)

/*
 *     Control register gain settings
 *---------------------------------------------
 *GN2 | GN1| GN0|sensor input| Gain X/Y | Gain Z|
 * 0  |  0 |  1 |     +/-1.3 |   1055   |   950 |
 * 0  |  1 |  0 |     +/-1.9 |   795    |   710 |
 * 0  |  1 |  1 |     +/-2.5 |   635    |   570 |
 * 1  |  0 |  0 |     +/-4.0 |   430    |   385 |
 * 1  |  0 |  1 |     +/-4.7 |   375    |   335 |
 * 1  |  1 |  0 |     +/-5.6 |   320    |   285 |
 * 1  |  1 |  1 |     +/-8.1 |   230    |   205 |
 *---------------------------------------------
 */
#define LSM303DLH_M_RANGE_1_3G	0x01
#define LSM303DLH_M_RANGE_1_9G	0x02
#define LSM303DLH_M_RANGE_2_5G	0x03
#define LSM303DLH_M_RANGE_4_0G	0x04
#define LSM303DLH_M_RANGE_4_7G	0x05
#define LSM303DLH_M_RANGE_5_6G	0x06
#define LSM303DLH_M_RANGE_8_1G	0x07

/*
 * CRA register data output rate settings
 *
 * DO2 DO1 DO0 Minimum data output rate (Hz)
 * 0    0   0		0.75
 * 0    0   1		1.5
 * 0    1   0		3.0
 * 0    1   1		7.5
 * 1    0   0		15
 * 1    0   1		30
 * 1    1   0		75
 * 1    1   1		Not used
 */
#define LSM303DLH_M_RATE_00_75		0x00
#define LSM303DLH_M_RATE_01_50		0x01
#define LSM303DLH_M_RATE_03_00		0x02
#define LSM303DLH_M_RATE_07_50		0x03
#define LSM303DLH_M_RATE_15_00		0x04
#define LSM303DLH_M_RATE_30_00		0x05
#define LSM303DLH_M_RATE_75_00		0x06
#define LSM303DLH_M_RATE_RESERVED	0x07

/* device status defines */
#define LSM303DLH_M_DEVICE_OFF		0
#define LSM303DLH_M_DEVICE_ON		1
#define LSM303DLH_M_DEVICE_SUSPENDED	2

#define	LSM303DLH_M_NORMAL_CFG		0x00
#define	LSM303DLH_M_POSITIVE_BIAS_CFG	0x01
#define	LSM303DLH_M_NEGATIVE_BIAS_CFG	0x02
#define	LSM303DLH_M_NOT_USED_CFG	0x03

/* Magnetic sensor operating mode */
#define LSM303DLH_M_CONTINUOUS_CONVERSION_MODE	0x00
#define LSM303DLH_M_SINGLE_CONVERSION_MODE	0x01
#define LSM303DLH_M_UNUSED_MODE			0x02
#define LSM303DLH_M_SLEEP_MODE			0x03

/* Multiple byte transfer enable */
#define LSM303DLH_MULTIPLE_I2C_TR	0x80
#define LSM303DLH_M_DATA_RDY		0x01

/* device CHIP ID defines */
#define LSM303DLHC_CHIP_ID		51

/*
 * The scaling frequencies are different
 * for LSM303DLH and LSM303DLHC
 * the number of elments of scaling frequency
 * is 50 and hence set this as the array size
 */
#define XY_LENGTH		50
#define Z_LENGTH		50

char xy_scale_avail[XY_LENGTH];
char z_scale_avail[Z_LENGTH];

/*
 * struct lsm303dlh_m_data - data structure used by lsm303dlh_m driver
 * @client: i2c client
 * @lock: mutex lock for sysfs operations
 * @regulator: regulator
 * @early_suspend: early suspend structure
 * @pdata: lsm303dlh platform data pointer
 * @device_status: device is ON, OFF or SUSPENDED
 * @mode: current mode of operation
 * @rate: current sampling rate
 * @config: device configuration
 * @range: current range value of magnetometer
 */

struct lsm303dlh_m_data {
	struct i2c_client	*client;
	struct mutex		lock;
	struct regulator	*regulator;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
	struct lsm303dlh_platform_data *pdata;
	int			device_status;
	u8			mode;
	u8			rate;
	u8			config;
	u8			range;
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void lsm303dlh_m_early_suspend(struct early_suspend *data);
static void lsm303dlh_m_late_resume(struct early_suspend *data);
#endif

static s32 lsm303dlh_config(struct i2c_client *client, u8 mode)
{
	/* the lower two bits indicates the magnetic sensor mode */
	return i2c_smbus_write_byte_data(client,
					LSM303DLH_M_MR_REG, mode & 0x03);
}

static inline int is_device_on(struct lsm303dlh_m_data *data)
{
	struct i2c_client *client = data->client;
	/*  Perform read/write operation only when device is active */
	if (data->device_status != LSM303DLH_M_DEVICE_ON) {
		dev_err(&client->dev,
			"device is switched off, make it on using mode");
		return -EINVAL;
	}

	return 0;
}

/* disable regulator and update status */
static int lsm303dlh_m_disable(struct lsm303dlh_m_data *data)
{
	data->device_status = LSM303DLH_M_DEVICE_OFF;

	regulator_disable(data->regulator);

	return 0;
}

/* enable regulator and update status */
static int lsm303dlh_m_enable(struct lsm303dlh_m_data *data)
{
	data->device_status = LSM303DLH_M_DEVICE_ON;

	regulator_enable(data->regulator);

	return 0;
}

/*
 * To read output x/y/z data register,
 * in this case x,y and z are not
 * mapped w.r.t board orientation.
 * Reading just raw data from device
 */
static ssize_t lsm303dlh_m_xyz_read(struct iio_dev *indio_dev,
					int address,
					int *buf)
{
	struct lsm303dlh_m_data *data = iio_priv(indio_dev);
	int ret;

	/* Perform read/write operation, only when device is active */
	ret = is_device_on(data);
	if (ret)
		return -EINVAL;

	mutex_lock(&data->lock);

	ret = i2c_smbus_read_byte_data(data->client, LSM303DLH_M_SR_REG);

	/* wait till data is written to all six registers */
	while (!(ret & LSM303DLH_M_DATA_RDY))
		ret = i2c_smbus_read_byte_data(data->client,
						LSM303DLH_M_SR_REG);

	ret = i2c_smbus_read_word_swapped(data->client, address);

	if (ret < 0) {
		dev_err(&data->client->dev, "reading xyz failed\n");
		mutex_unlock(&data->lock);
		return -EINVAL;
	}

	mutex_unlock(&data->lock);

	*buf = (s16)ret;

	return IIO_VAL_INT;
}

/*
 * To read output x,y,z data register.
 * After reading change x,y and z values
 * w.r.t the orientation of the device.
 */
static ssize_t lsm303dlh_m_readdata(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct lsm303dlh_m_data *data = iio_priv(dev_get_drvdata(dev));
	struct lsm303dlh_platform_data *pdata = data->pdata;
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	u8 map_x = pdata->axis_map_x;
	u8 map_y = pdata->axis_map_y;
	u8 map_z = pdata->axis_map_z;
	int ret;
	unsigned char magn_data[6];
	s16 val[3];

	/* Perform read/write operation, only when device is active */
	ret = is_device_on(data);
	if (ret)
		return -EINVAL;

	mutex_lock(&data->lock);

	ret = i2c_smbus_read_byte_data(data->client, LSM303DLH_M_SR_REG);

	/* wait till data is written to all six registers */
	while (!(ret & LSM303DLH_M_DATA_RDY))
		ret = i2c_smbus_read_byte_data(data->client,
					LSM303DLH_M_SR_REG);

	ret = i2c_smbus_read_i2c_block_data(data->client,
				this_attr->address |
				LSM303DLH_MULTIPLE_I2C_TR,
				6, magn_data);

	if (ret < 0) {
		dev_err(&data->client->dev, "reading xyz failed\n");
		mutex_unlock(&data->lock);
		return -EINVAL;
	}

	/* MSB is at lower address */
	val[0] = (s16)
			(((magn_data[0]) << 8) | magn_data[1]);
	val[1] = (s16)
			(((magn_data[2]) << 8) | magn_data[3]);
	val[2] = (s16)
			(((magn_data[4]) << 8) | magn_data[5]);
	/* check if chip is DHLC */
	if (data->pdata->chip_id == LSM303DLHC_CHIP_ID)
		/*
		 * the out registers are in x, z and y order
		 * so swap y and z values
		 */
		swap(val[1], val[2]);
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

static ssize_t show_operating_mode(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct lsm303dlh_m_data *data = iio_priv(dev_get_drvdata(dev));

	return sprintf(buf, "%d\n", data->mode);
}

static ssize_t set_operating_mode(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct lsm303dlh_m_data *data = iio_priv(dev_get_drvdata(dev));
	struct i2c_client *client = data->client;
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	int error;
	unsigned long mode = 0;

	mutex_lock(&data->lock);

	error = kstrtoul(buf, 10, &mode);
	if (error) {
		count = error;
		goto exit;
	}

	if (mode > LSM303DLH_M_SLEEP_MODE) {
		dev_err(&client->dev, "trying to set invalid mode\n");
		count = -EINVAL;
		goto exit;
	}

	/*
	 * If device is driven to sleep mode in suspend, update mode
	 * and return
	 */
	if (data->device_status == LSM303DLH_M_DEVICE_SUSPENDED &&
			mode == LSM303DLH_M_SLEEP_MODE) {
		data->mode = mode;
		goto exit;
	}

	if (data->mode == mode)
		goto exit;

	/* Enable the regulator if it is not turned on earlier */
	if (data->device_status == LSM303DLH_M_DEVICE_OFF ||
		data->device_status == LSM303DLH_M_DEVICE_SUSPENDED)
		lsm303dlh_m_enable(data);

	dev_dbg(dev, "set operating mode to %lu\n", mode);

	error = i2c_smbus_write_byte_data(client, this_attr->address, mode);
	if (error < 0) {
		dev_err(&client->dev, "Error in setting the mode\n");
		count = -EINVAL;
		goto exit;
	}

	data->mode = mode;
	/* if sleep mode, disable the regulator */
	if (data->mode == LSM303DLH_M_SLEEP_MODE)
		lsm303dlh_m_disable(data);
exit:
	mutex_unlock(&data->lock);
	return count;
}

/*
 * Magnetic sensor operating mode: CRA_REG
 * ms1 ms0
 * 0	0	Normal measurement configuration
 * 0	1	Positive bias configuration.
 * 1	0	Negative bias configuration.
 * 1	1	This configuration is not used
 */
static s32 lsm303dlh_set_config(struct i2c_client *client, u8 config)
{
	struct lsm303dlh_m_data *data = i2c_get_clientdata(client);
	u8 reg_val;

	reg_val = (config & LSM303DLH_M_CRA_MS_MASK) |
			(data->rate << LSM303DLH_M_CRA_DO_BIT);
	return i2c_smbus_write_byte_data(client, LSM303DLH_M_CRA_REG, reg_val);
}


static IIO_CONST_ATTR_SAMP_FREQ_AVAIL("0.75 1.5 3.0 7.5 15 30 75");

static s32 lsm303dlh_m_set_range(struct i2c_client *client, u8 range)
{
	u8 reg_val;

	reg_val = range << LSM303DLH_M_CRB_GN_BIT;

	return i2c_smbus_write_byte_data(client, LSM303DLH_M_CRB_REG, reg_val);
}

static s32 lsm303dlh_m_set_rate(struct i2c_client *client, u8 rate)
{
	struct lsm303dlh_m_data *data = i2c_get_clientdata(client);
	u8 reg_val;

	reg_val =  (data->config) | (rate << LSM303DLH_M_CRA_DO_BIT);
	if (rate >= LSM303DLH_M_RATE_RESERVED) {
		dev_err(&client->dev, "given rate not supported\n");
		return -EINVAL;
	}

	return i2c_smbus_write_byte_data(client, LSM303DLH_M_CRA_REG, reg_val);
}

static ssize_t set_sampling_frequency(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct lsm303dlh_m_data *data = iio_priv(dev_get_drvdata(dev));
	struct i2c_client *client = data->client;
	unsigned long rate = 0;
	int err;

	err = is_device_on(data);
	if (err)
		return err;

	if (strncmp(buf, "0.75" , 4) == 0)
		rate = LSM303DLH_M_RATE_00_75;

	else if (strncmp(buf, "1.5" , 3) == 0)
		rate = LSM303DLH_M_RATE_01_50;

	else if (strncmp(buf, "3.0" , 3) == 0)
		rate = LSM303DLH_M_RATE_03_00;

	else if (strncmp(buf, "7.5" , 3) == 0)
		rate = LSM303DLH_M_RATE_07_50;

	else if (strncmp(buf, "15" , 2) == 0)
		rate = LSM303DLH_M_RATE_15_00;

	else if (strncmp(buf, "30" , 2) == 0)
		rate = LSM303DLH_M_RATE_30_00;

	else if (strncmp(buf, "75" , 2) == 0)
		rate = LSM303DLH_M_RATE_75_00;
	else
		return -EINVAL;

	mutex_lock(&data->lock);

	if (lsm303dlh_m_set_rate(client, rate)) {
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
	"0.75",
	"1.5",
	"3.0",
	"7.5",
	"15",
	"30",
	"75",
	"res",
};

static int xy_to_nanoscale[] = {
	947870, 1257860, 1574800, 2325580, 2666670, 3125000, 4347830
};

static int z_to_nanoscale[] = {
	1052630, 1408450, 17543820, 2597400, 2985070, 3508770, 4878050
};

static int xy_to_nanoscale_dlhc[] = {
	 909090, 1169590, 1492540, 2222220, 2500000, 3030300, 4347830
};

static int z_to_nanoscale_dlhc[] = {
	1020410, 1315790, 1666660, 2500000, 2816900, 3389830, 4878050
};

static const char const scale_to_range[] = {
	LSM303DLH_M_RANGE_1_3G,
	LSM303DLH_M_RANGE_1_9G,
	LSM303DLH_M_RANGE_2_5G,
	LSM303DLH_M_RANGE_4_0G,
	LSM303DLH_M_RANGE_4_7G,
	LSM303DLH_M_RANGE_5_6G,
	LSM303DLH_M_RANGE_8_1G
};
static ssize_t show_sampling_frequency(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct lsm303dlh_m_data *data = iio_priv(dev_get_drvdata(dev));

	return sprintf(buf, "%s\n", reg_to_rate[data->rate]);
}

static IIO_DEV_ATTR_SAMP_FREQ(S_IWUSR | S_IRUGO,
			show_sampling_frequency,
			set_sampling_frequency);

static IIO_CONST_ATTR(magnet_xy_scale_available, xy_scale_avail);

static IIO_CONST_ATTR(magnet_z_scale_available, z_scale_avail);

static int lsm303dlh_write_raw(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan,
			int val, int val2,
			long mask)
{
	struct lsm303dlh_m_data *data = iio_priv(indio_dev);
	int ret = -EINVAL, i;
	bool flag = false;
	char end;
	int *xy_scale;
	int *z_scale;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		ret = is_device_on(data);
		if (ret)
			return -EINVAL;
		mutex_lock(&data->lock);

		if (data->pdata->chip_id == LSM303DLHC_CHIP_ID) {
			xy_scale = xy_to_nanoscale_dlhc;
			z_scale = z_to_nanoscale_dlhc;
		} else {
			xy_scale = xy_to_nanoscale;
			z_scale = z_to_nanoscale;
		}
		end = ARRAY_SIZE(xy_to_nanoscale);
		if (chan->address == LSM303DLH_M_OUT_X ||
				chan->address == LSM303DLH_M_OUT_Y) {
			for (i = 0; i < end; i++) {
				if (val == xy_scale[i]) {
					flag = true;
					break;
				}
			}
		} else if (chan->address == LSM303DLH_M_OUT_Z) {
			for (i = 0; i < end; i++) {
				if (val == z_scale[i]) {
					flag = true;
					break;
				}
			}
		}
		if (flag) {
			ret = lsm303dlh_m_set_range(data->client,
						scale_to_range[data->range]);
			data->range = i;
		}
		mutex_unlock(&data->lock);
		break;
	default:
		break;
	}
	return ret;
}

static int lsm303dlh_read_raw(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan,
			int *val, int *val2,
			long mask)
{
	struct lsm303dlh_m_data *data = iio_priv(indio_dev);

	switch (mask) {
	case 0:
		return lsm303dlh_m_xyz_read(indio_dev,
					chan->address, val);
	case IIO_CHAN_INFO_SCALE:
		/* scale for X/Y and Z are different */
		if (chan->address == LSM303DLH_M_OUT_X ||
				chan->address == LSM303DLH_M_OUT_Y){
			/* check if chip is DHLC */
			if (data->pdata->chip_id == LSM303DLHC_CHIP_ID)
				*val2 = xy_to_nanoscale_dlhc[data->range];
			else
				*val2 = xy_to_nanoscale[data->range];
		} else {
			/* check if chip is DHLC */
			if (data->pdata->chip_id == LSM303DLHC_CHIP_ID)
				*val2 = z_to_nanoscale_dlhc[data->range];
			else
				*val2 = z_to_nanoscale[data->range];
		}

		return IIO_VAL_INT_PLUS_NANO;
	default:
		break;
	}
	return -EINVAL;
}

#define LSM303DLH_CHANNEL(axis, addr)				\
	{							\
		.type = IIO_MAGN,				\
		.modified = 1,					\
		.channel2 = IIO_MOD_##axis,			\
		.info_mask = IIO_CHAN_INFO_SCALE_SEPARATE_BIT,	\
		.address = addr,				\
	}

static const struct iio_chan_spec lsmdlh303_channels[] = {
	LSM303DLH_CHANNEL(X, LSM303DLH_M_OUT_X),
	LSM303DLH_CHANNEL(Y, LSM303DLH_M_OUT_Y),
	LSM303DLH_CHANNEL(Z, LSM303DLH_M_OUT_Z),
};

static IIO_DEVICE_ATTR(mode,
			S_IWUSR | S_IRUGO,
			show_operating_mode,
			set_operating_mode,
			LSM303DLH_M_MR_REG);
static IIO_DEVICE_ATTR(magn_raw, S_IRUGO,
			lsm303dlh_m_readdata,
			NULL,
			LSM303DLH_M_OUT_X);

static struct attribute *lsm303dlh_m_attributes[] = {
	&iio_dev_attr_mode.dev_attr.attr,
	&iio_dev_attr_sampling_frequency.dev_attr.attr,
	&iio_dev_attr_magn_raw.dev_attr.attr,
	&iio_const_attr_sampling_frequency_available.dev_attr.attr,
	&iio_const_attr_magnet_xy_scale_available.dev_attr.attr,
	&iio_const_attr_magnet_z_scale_available.dev_attr.attr,
	NULL
};

static const struct attribute_group lsmdlh303m_group = {
	.attrs = lsm303dlh_m_attributes,
};

static const struct iio_info lsmdlh303m_info = {
	.attrs = &lsmdlh303m_group,
	.read_raw = &lsm303dlh_read_raw,
	.write_raw = &lsm303dlh_write_raw,
	.driver_module = THIS_MODULE,
};

static void lsm303dlh_m_setup(struct lsm303dlh_m_data *data)
{
	/* set the magnetic sensor operating mode */
	lsm303dlh_set_config(data->client, data->config);
	/* set to the default rate */
	lsm303dlh_m_set_rate(data->client, data->rate);
	/* set the magnetic sensor mode */
	lsm303dlh_config(data->client, data->mode);
	/* set the range */
	lsm303dlh_m_set_range(data->client, scale_to_range[data->range]);
}

#if (!defined(CONFIG_HAS_EARLYSUSPEND) && defined(CONFIG_PM))
static int lsm303dlh_m_suspend(struct device *dev)
{
	struct lsm303dlh_m_data *data = iio_priv(dev_get_drvdata(dev));
	int ret = 0;

	if (data->mode == LSM303DLH_M_SLEEP_MODE)
		return 0;

	mutex_lock(&data->lock);

	/* Set the device to sleep mode */
	lsm303dlh_config(data->client, LSM303DLH_M_SLEEP_MODE);

	/* Disable regulator */
	lsm303dlh_m_disable(data);

	data->device_status = LSM303DLH_M_DEVICE_SUSPENDED;

	mutex_unlock(&data->lock);

	return ret;
}

static int lsm303dlh_m_resume(struct device *dev)
{
	struct lsm303dlh_m_data *data = iio_priv(dev_get_drvdata(dev));
	int ret = 0;

	if (data->device_status == LSM303DLH_M_DEVICE_ON ||
		data->device_status == LSM303DLH_M_DEVICE_OFF) {
		return 0;
	}
	mutex_lock(&data->lock);

	/* Enable regulator */
	lsm303dlh_m_enable(data);

	/* Setup device parameters */
	lsm303dlh_m_setup(data);

	mutex_unlock(&data->lock);
	return ret;
}

static const struct dev_pm_ops lsm303dlh_m_dev_pm_ops = {
	.suspend = lsm303dlh_m_suspend,
	.resume = lsm303dlh_m_resume,
};
#endif
#ifdef CONFIG_HAS_EARLYSUSPEND
static void lsm303dlh_m_early_suspend(struct early_suspend *data)
{
	struct lsm303dlh_m_data *ddata =
		container_of(data, struct lsm303dlh_m_data, early_suspend);

	if (ddata->mode == LSM303DLH_M_SLEEP_MODE)
		return;

	mutex_lock(&ddata->lock);

	/* Set the device to sleep mode */
	lsm303dlh_config(ddata->client, LSM303DLH_M_SLEEP_MODE);

	/* Disable regulator */
	lsm303dlh_m_disable(ddata);

	ddata->device_status = LSM303DLH_M_DEVICE_SUSPENDED;

	mutex_unlock(&ddata->lock);
}

static void lsm303dlh_m_late_resume(struct early_suspend *data)
{
	struct lsm303dlh_m_data *ddata =
		container_of(data, struct lsm303dlh_m_data, early_suspend);

	if (ddata->device_status == LSM303DLH_M_DEVICE_ON ||
		ddata->device_status == LSM303DLH_M_DEVICE_OFF) {
		return;
	}
	mutex_lock(&ddata->lock);

	/* Enable regulator */
	lsm303dlh_m_enable(ddata);

	/* Setup device parameters */
	lsm303dlh_m_setup(ddata);

	mutex_unlock(&ddata->lock);

}
#endif

static int lsm303dlh_m_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct lsm303dlh_m_data *data;
	struct iio_dev *indio_dev;
	int err;

	indio_dev = iio_allocate_device(sizeof(*data));
	if (indio_dev == NULL) {
		dev_err(&client->dev, "memory allocation failed\n");
		err = -ENOMEM;
		goto exit;
	}

	data = iio_priv(indio_dev);

	data->mode = LSM303DLH_M_SLEEP_MODE;
	data->config = LSM303DLH_M_NORMAL_CFG;
	data->range = LSM303DLH_M_RANGE_1_3G;
	data->rate = LSM303DLH_M_RATE_00_75;

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
		dev_err(&client->dev, "failed to get regulator\n");
		err = PTR_ERR(data->regulator);
		goto exit1;
	}

	/* enable regulators */
	lsm303dlh_m_enable(data);

	lsm303dlh_m_setup(data);

	mutex_init(&data->lock);

	indio_dev->info = &lsmdlh303m_info;
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
	data->early_suspend.suspend = lsm303dlh_m_early_suspend;
	data->early_suspend.resume = lsm303dlh_m_late_resume;
	register_early_suspend(&data->early_suspend);
#endif

	/* disable regulator */
	lsm303dlh_m_disable(data);

	if (data->pdata->chip_id == LSM303DLHC_CHIP_ID)	{
		strcpy(xy_scale_avail, "909090, 1169590, 1492540, 2222220, 2500000, 3030300, 4347830");
		strcpy(z_scale_avail, "1020410, 1315790, 1666660, 2500000, 2816900, 3389830, 4878050");
	} else {
		strcpy(xy_scale_avail, "947870, 1257860, 1574800, 2325580, 2666670, 3125000, 4347830");
		strcpy(z_scale_avail, "947870, 1257860, 1574800, 2325580, 2666670, 3125000, 4347830");
	}

	return 0;

exit2:
	regulator_disable(data->regulator);
	mutex_destroy(&data->lock);
	regulator_put(data->regulator);
exit1:
	iio_free_device(indio_dev);
exit:
	return err;
}

static int __devexit lsm303dlh_m_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct lsm303dlh_m_data *data = iio_priv(indio_dev);
	int ret;

	/* its safe to set the mode to sleep */
	if (data->mode != LSM303DLH_M_SLEEP_MODE) {
		ret = lsm303dlh_config(client, LSM303DLH_M_SLEEP_MODE);
		if (ret < 0) {
			dev_err(&client->dev,
				"could not place the device in sleep mode %d",
				ret);
			return ret;
		}
		if (data->device_status == LSM303DLH_M_DEVICE_ON)
			regulator_disable(data->regulator);
		data->device_status = LSM303DLH_M_DEVICE_OFF;
	}
	regulator_put(data->regulator);
	mutex_destroy(&data->lock);
	iio_device_unregister(indio_dev);
	iio_free_device(indio_dev);

	return 0;
}

static const struct i2c_device_id lsm303dlh_m_id[] = {
	{ "lsm303dlh_m", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, lsm303dlh_m_id);

static struct i2c_driver lsm303dlh_m_driver = {
	.driver = {
		.name	= "lsm303dlh_m",
	#if (!defined(CONFIG_HAS_EARLYSUSPEND) && defined(CONFIG_PM))
		.pm	= &lsm303dlh_m_dev_pm_ops,
	#endif
	},
	.id_table	= lsm303dlh_m_id,
	.probe		= lsm303dlh_m_probe,
	.remove		= lsm303dlh_m_remove,
};

module_i2c_driver(lsm303dlh_m_driver);

MODULE_DESCRIPTION("lsm303dlh Magnetometer Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("srinidhi kasagar <srinidhi.kasagar@stericsson.com>");
