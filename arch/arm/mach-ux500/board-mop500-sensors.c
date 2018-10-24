/*
 * Copyright (C) ST-Ericsson SA 2010
 *
 * License terms: GNU General Public License (GPL), version 2
 */

#include <linux/gpio.h>
#include <linux/lsm303dlh.h>
#include <linux/l3g4200d.h>
#include <linux/i2c.h>
#include <linux/input/lps001wp.h>
#include <asm/mach-types.h>
#include <mach/id.h>

#include "board-mop500.h"

/*
 * LSM303DLH accelerometer + magnetometer & L3G4200D Gyroscope sensors
 */
static struct lsm303dlh_platform_data lsm303dlh_pdata = {
	.axis_map_x = 0,
	.axis_map_y = 1,
	.axis_map_z = 2,
	.negative_x = 1,
	.negative_y = 1,
	.negative_z = 0,
};

static struct l3g4200d_gyr_platform_data l3g4200d_pdata = {
	.axis_map_x = 1,
	.axis_map_y = 0,
	.axis_map_z = 2,
	.negative_x = 0,
	.negative_y = 0,
	.negative_z = 1,
};

/*
 * Platform data for pressure sensor,
 * poll interval and min interval in millseconds.
 */
static struct lps001wp_prs_platform_data lps001wp_pdata = {
	.poll_interval = 1000,
	.min_interval = 10,
};

static struct i2c_board_info __initdata mop500_i2c2_devices[] = {
	{
		/* LSM303DLH Magnetometer */
		I2C_BOARD_INFO("lsm303dlh_m", 0x1E),
		.platform_data = &lsm303dlh_pdata,
	},
	{
		/* L3G4200D Gyroscope */
		I2C_BOARD_INFO("l3g4200d", 0x68),
		.platform_data = &l3g4200d_pdata,
	},
	{
		/* LSP001WM Barometer */
		I2C_BOARD_INFO("lps001wp_prs", 0x5C),
		.platform_data = &lps001wp_pdata,
	},
};

/*
 * Break this out due to the fact that this have changed address on snowball
 */
static struct i2c_board_info __initdata mop500_2_i2c2_devices[] = {
	{
		/* LSM303DLH Accelerometer */
		I2C_BOARD_INFO("lsm303dlh_a", 0x18),
		.platform_data = &lsm303dlh_pdata,
	},
};

/*
 * This is needed due to the fact that the i2c address changed in V7 =<
 * and there is no way of knowing if the HW is V7 or higher so we just
 * have to try and fail.
 */
static struct i2c_board_info __initdata snowball_i2c2_devices[] = {
	{
		/* LSM303DLH Accelerometer */
		I2C_BOARD_INFO("lsm303dlhc_a", 0x19),
		.platform_data = &lsm303dlh_pdata,
	},
};


/*
 * Register/Add i2c sensors
 */
void mop500_sensors_i2c_add(int busnum, struct i2c_board_info const *info,
		unsigned n)
{
	struct i2c_adapter *adap;
	struct i2c_client *client;
	int i;

	adap = i2c_get_adapter(busnum);
	if (!adap) {
		/* We have no i2c adapter yet lets create it. */
		pr_info(__FILE__ ": Creating i2c adapter %d\n", busnum);
		i2c_register_board_info(busnum, info, n);
		return;
	}

	for (i = 0; i < n; i++) {
		client = i2c_new_device(adap, &info[i]);
		if (!client)
			pr_err(__FILE__ ": failed to register %s to i2c%d\n",
					info[i].type,
					busnum);
	}

	i2c_put_adapter(adap);
}

/*
 * Register/Add i2c sensors
 */
void mop500_sensors_probe_add_lsm303dlh_a(void)
{
	static const int busnum = 2;
	struct i2c_adapter *adap;
	struct i2c_client *client;
	static const unsigned short i2c_addr_list[] = {
		0x18, 0x19, I2C_CLIENT_END };
	struct i2c_board_info i2c_info = {
			/* LSM303DLH Accelerometer */
			I2C_BOARD_INFO("lsm303dlh_a", 0),
			.platform_data = &lsm303dlh_pdata,
	};

	adap = i2c_get_adapter(busnum);
	if (!adap) {
		/* We have no i2c adapter yet lets create it. */
		pr_err(__FILE__ ": Could not get adapter %d\n", busnum);
		return;
	}
	client = i2c_new_probed_device(adap, &i2c_info,
			i2c_addr_list, NULL);
	if (!client)
		pr_err(__FILE__ ": failed to register %s to i2c%d\n",
				i2c_info.type,
				busnum);
	i2c_put_adapter(adap);
}

/*
 * Check which accelerometer chip is mounted on UIB and
 * read the chip ID to detect whether chip is LSM303DHL/LSM303DHLC.
 */
static int mop500_get_acc_id(void)
{
	int status;
	union i2c_smbus_data data;
	struct i2c_adapter *i2c2;

	i2c2 = i2c_get_adapter(2);
	if (!i2c2) {
		pr_err("failed to get i2c adapter\n");
		return -1;
	}
	status = i2c_smbus_xfer(i2c2, 0x18 , 0 ,
			I2C_SMBUS_READ, 0x0F ,
			I2C_SMBUS_BYTE_DATA, &data);
	if (status < 0) {
		status = i2c_smbus_xfer(i2c2, 0x19 , 0 ,
				I2C_SMBUS_READ, 0x0F ,
				I2C_SMBUS_BYTE_DATA, &data);
	}
	i2c_put_adapter(i2c2);
	return (status < 0) ? status : data.byte;
}

static int __init mop500_sensors_init(void)
{
	int ret;

	if (!machine_is_snowball() && !uib_is_stuib() &&
			!uib_is_u8500uib() && !uib_is_u8500uibr3())
		return 0;

	if (machine_is_hrefv60() || machine_is_u8520() || machine_is_u9540()) {
		lsm303dlh_pdata.irq_a1 = HREFV60_ACCEL_INT1_GPIO;
		lsm303dlh_pdata.irq_a2 = HREFV60_ACCEL_INT2_GPIO;
		lsm303dlh_pdata.irq_m = HREFV60_MAGNET_DRDY_GPIO;
	} else if (machine_is_snowball()) {
		lsm303dlh_pdata.irq_a1 = SNOWBALL_ACCEL_INT1_GPIO;
		lsm303dlh_pdata.irq_a2 = SNOWBALL_ACCEL_INT2_GPIO;
		lsm303dlh_pdata.irq_m = SNOWBALL_MAGNET_DRDY_GPIO;
	} else {
		lsm303dlh_pdata.irq_a1 = GPIO_ACCEL_INT1;
		lsm303dlh_pdata.irq_a2 = GPIO_ACCEL_INT2;
		lsm303dlh_pdata.irq_m = GPIO_MAGNET_DRDY;
	}

	/* Special sensors data for 8500 UIBs */
	if (uib_is_u8500uib() || uib_is_u8500uibr3()) {
		lsm303dlh_pdata.axis_map_x = 1;
		lsm303dlh_pdata.axis_map_y = 0;
		lsm303dlh_pdata.negative_x = 0;
		lsm303dlh_pdata.negative_y = 0;
		lsm303dlh_pdata.negative_z = 1;

		l3g4200d_pdata.axis_map_x = 0;
		l3g4200d_pdata.axis_map_y = 1;
		l3g4200d_pdata.negative_x = 1;
		l3g4200d_pdata.negative_y = 0;
		l3g4200d_pdata.negative_z = 1;
	}

	ret = mop500_get_acc_id();
	if (ret < 0)
		printk(KERN_ERR " Failed to get Accelerometer chip ID\n");
	else
		lsm303dlh_pdata.chip_id = ret;

	if (machine_is_snowball()) {
		if (cpu_is_u8500v21())
			/* This is ugly but we cant know what address to use */
			mop500_sensors_probe_add_lsm303dlh_a();
		else /* Add the accelerometer with new addr */
			mop500_sensors_i2c_add(2, snowball_i2c2_devices,
					ARRAY_SIZE(snowball_i2c2_devices));
	} else /* none snowball have the old addr */
		mop500_sensors_i2c_add(2, mop500_2_i2c2_devices,
				ARRAY_SIZE(mop500_2_i2c2_devices));

	mop500_sensors_i2c_add(2, mop500_i2c2_devices,
			ARRAY_SIZE(mop500_i2c2_devices));
	return 0;
}

module_init(mop500_sensors_init);
