/*
 * Copyright (C) 2011 ST-Ericsson SA
 * Author: Avinash A <avinash.a@stericsson.com> for ST-Ericsson
 * License terms:GNU General Public License (GPL) version 2
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/input/cyttsp.h>
#include <linux/delay.h>
#include <linux/gpio/nomadik.h>
#include <linux/i2c.h>
#ifdef CONFIG_U8500_FLASH
#include <../drivers/staging/camera_flash/adp1653_plat.h>
#endif
#include <linux/input/matrix_keypad.h>
#include <linux/mfd/tc3589x.h>
#include <linux/mfd/dbx500-prcmu.h>
#include <linux/amba/pl022.h>
#include <plat/pincfg.h>
#include <mach/hardware.h>
#include <mach/irqs.h>
#include <mach/irqs-db8500.h>
#include <asm/mach-types.h>
#include "pins-db8500.h"
#include "board-mop500.h"
#include "devices-db8500.h"

#define NUM_SSP_CLIENTS 10

#ifdef CONFIG_U8500_FLASH
static struct adp1653_platform_data __initdata adp1653_pdata_u8500_uib = {
	.irq_no = CAMERA_FLASH_INT_PIN
};
#endif

static struct i2c_board_info __initdata mop500_i2c2_devices_u8500_r3[] = {
#ifdef CONFIG_U8500_FLASH
	{
		I2C_BOARD_INFO("adp1653", 0x30),
		.platform_data = &adp1653_pdata_u8500_uib
	}
#endif
};

/* cyttsp_gpio_board_init : configures the touch panel. */
static int cyttsp_plat_init(void)
{
	int ret;

	ret = gpio_direction_output(CYPRESS_SLAVE_SELECT_GPIO, 1);
	if (ret < 0) {
		pr_err("slave select gpio direction failed\n");
		gpio_free(CYPRESS_SLAVE_SELECT_GPIO);
		return ret;
	}
	return 0;
}

/* cyttsp_gpio_board_exit : deconfigures the touch panel. */
static void cyttsp_plat_exit(void)
{
	gpio_direction_output(CYPRESS_SLAVE_SELECT_GPIO, 0);
}

static struct pl022_ssp_controller mop500_spi2_data = {
	.bus_id         = SPI023_2_CONTROLLER,
	.num_chipselect = NUM_SSP_CLIENTS,
};

static int cyttsp_wakeup(void)
{
	int ret;

	ret = gpio_request(CYPRESS_TOUCH_INT_PIN, "Wakeup_pin");
	if (ret < 0) {
		pr_err("touch gpio failed\n");
		return ret;
	}
	ret = gpio_direction_output(CYPRESS_TOUCH_INT_PIN, 1);
	if (ret < 0) {
		pr_err("touch gpio direction failed\n");
		goto out;
	}
	gpio_set_value(CYPRESS_TOUCH_INT_PIN, 0);
	gpio_set_value(CYPRESS_TOUCH_INT_PIN, 1);
	/*
	 * To wake up the controller from sleep
	 * state the interrupt pin needs to be
	 * pulsed twice with a delay greater
	 * than 2 micro seconds.
	 */
	udelay(3);
	gpio_set_value(CYPRESS_TOUCH_INT_PIN, 0);
	gpio_set_value(CYPRESS_TOUCH_INT_PIN, 1);
	ret = gpio_direction_input(CYPRESS_TOUCH_INT_PIN);
	if (ret < 0) {
		pr_err("touch gpio direction IN config failed\n");
		goto out;
	}
out:
	gpio_free(CYPRESS_TOUCH_INT_PIN);
	return 0;
}
struct cyttsp_platform_data cyttsp_platdata = {
	.maxx = 480,
	.maxy = 854,
	.use_hndshk = 0,
	/* set active distance */
	.act_dist = CY_ACT_DIST_DFLT,
	.act_intrvl = CY_ACT_INTRVL_DFLT,  /* Active refresh interval; ms */
	.tch_tmout = CY_TCH_TMOUT_DFLT,   /* Active touch timeout; ms */
	.lp_intrvl = CY_LP_INTRVL_DFLT,   /* Low power refresh interval; ms */
	.init = cyttsp_plat_init,
	.exit = cyttsp_plat_exit,
	.name = CY_SPI_NAME,
	.irq_gpio = CYPRESS_TOUCH_INT_PIN,
/*	.rst_gpio = CYPRESS_TOUCH_RST_GPIO, Notavailable in mainline */
};

static void cyttsp_spi_cs_control(u32 command)
{
	if (command == SSP_CHIP_SELECT)
		gpio_set_value(CYPRESS_SLAVE_SELECT_GPIO, 0);
	else if (command == SSP_CHIP_DESELECT)
		gpio_set_value(CYPRESS_SLAVE_SELECT_GPIO, 1);
}

static struct pl022_config_chip cyttsp_ssp_config_chip = {
	.com_mode = INTERRUPT_TRANSFER,
	.iface = SSP_INTERFACE_MOTOROLA_SPI,
	/* we can act as master only */
	.hierarchy = SSP_MASTER,
	.slave_tx_disable = 0,
	.rx_lev_trig = SSP_RX_1_OR_MORE_ELEM,
	.tx_lev_trig = SSP_TX_16_OR_MORE_EMPTY_LOC,
	.ctrl_len = SSP_BITS_16,
	.wait_state = SSP_MWIRE_WAIT_ZERO,
	.duplex = SSP_MICROWIRE_CHANNEL_FULL_DUPLEX,
	.cs_control = cyttsp_spi_cs_control,
};

static struct spi_board_info cypress_spi_devices[] = {
	{
		.modalias = CY_SPI_NAME,
		.controller_data = &cyttsp_ssp_config_chip,
		.platform_data = &cyttsp_platdata,
		.max_speed_hz = 1000000,
		.bus_num = SPI023_2_CONTROLLER,
		.chip_select = 0,
		.mode = SPI_MODE_0,
	}
};

/*
 * TC35893
 */
static const unsigned int sony_keymap[] = {
	KEY(3, 1, KEY_END),
	KEY(4, 1, KEY_HOME),
	KEY(6, 4, KEY_VOLUMEDOWN),
	KEY(4, 2, KEY_EMAIL),
	KEY(3, 3, KEY_RIGHT),
	KEY(2, 5, KEY_BACKSPACE),

	KEY(6, 7, KEY_MENU),
	KEY(5, 0, KEY_ENTER),
	KEY(4, 3, KEY_0),
	KEY(3, 4, KEY_DOT),
	KEY(5, 2, KEY_UP),
	KEY(3, 5, KEY_DOWN),

	KEY(4, 5, KEY_SEND),
	KEY(0, 5, KEY_BACK),
	KEY(6, 2, KEY_VOLUMEUP),
	KEY(1, 3, KEY_SPACE),
	KEY(7, 6, KEY_LEFT),
	KEY(5, 5, KEY_SEARCH),
};

static struct matrix_keymap_data sony_keymap_data = {
	.keymap		= sony_keymap,
	.keymap_size    = ARRAY_SIZE(sony_keymap),
};

static struct tc3589x_keypad_platform_data tc35893_data = {
	.krow = TC_KPD_ROWS,
	.kcol = TC_KPD_COLUMNS,
	.debounce_period = TC_KPD_DEBOUNCE_PERIOD,
	.settle_time = TC_KPD_SETTLE_TIME,
	.irqtype = IRQF_TRIGGER_FALLING,
	.enable_wakeup = true,
	.keymap_data    = &sony_keymap_data,
	.no_autorepeat  = true,
};

static struct tc3589x_platform_data tc3589x_keypad_data = {
	.block = TC3589x_BLOCK_KEYPAD,
	.keypad = &tc35893_data,
	.irq_base = MOP500_EGPIO_IRQ_BASE,
};

static struct i2c_board_info __initdata mop500_i2c0_devices_u8500[] = {
	{
		I2C_BOARD_INFO("tc3589x", 0x44),
		.platform_data = &tc3589x_keypad_data,
		.irq = NOMADIK_GPIO_TO_IRQ(64),
		.flags = I2C_CLIENT_WAKE,
	},
};

static void mop500_cyttsp_init(void)
{
	int ret = 0;

	/*
	 * Enable the alternative C function
	 * in the PRCMU register
	 */
	prcmu_enable_spi2();
	ret = gpio_request(CYPRESS_SLAVE_SELECT_GPIO, "slave_select_gpio");
	if (ret < 0)
		pr_err("slave select gpio failed\n");
	spi_register_board_info(cypress_spi_devices,
			ARRAY_SIZE(cypress_spi_devices));
}

void __init mop500_u8500uib_r3_init()
{
	mop500_cyttsp_init();
	db8500_add_spi2(NULL, &mop500_spi2_data);
	nmk_config_pin((GPIO64_GPIO     | PIN_INPUT_PULLUP), false);

#ifdef CONFIG_U8500_FLASH
	if (machine_is_hrefv60() || machine_is_u8520()) {
		adp1653_pdata_u8500_uib.enable_gpio =
					HREFV60_CAMERA_FLASH_ENABLE;
	} else {
		adp1653_pdata_u8500_uib.enable_gpio =
					GPIO_CAMERA_FLASH_ENABLE;
	}
#endif
	mop500_uib_i2c_add(0, mop500_i2c0_devices_u8500,
			ARRAY_SIZE(mop500_i2c0_devices_u8500));
	mop500_uib_i2c_add(0, mop500_i2c0_devices_u8500,
			ARRAY_SIZE(mop500_i2c0_devices_u8500));
	mop500_uib_i2c_add(2, mop500_i2c2_devices_u8500_r3,
			ARRAY_SIZE(mop500_i2c2_devices_u8500_r3));
}
