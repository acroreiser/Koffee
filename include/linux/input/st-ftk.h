/*
 * STMicroelectronics FingertipK touchscreen driver
 *
 * Copyright (C) ST-Ericsson SA 2012
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
 */

#ifndef _LINUX_INPUT_FTK_H
#define _LINUX_INPUT_FTK_H


/*
 * STMT05/STMT07 FingerTipK driver platform data
 * gpio_rst: hardware reset pin (optional set -1)
 * x_min/x_max : X pixel resolution
 * y_min/y_max : Y pixel resolution
 * p_min/p_max : pressure
 * portrait : portrait (1) / landscape (0 - default) mode (optional)
 * patch_file : name of the firmware binary file
 *
 */
struct ftk_platform_data {
	int gpio_rst;
	u32 x_min;
	u32 x_max;
	u32 y_min;
	u32 y_max;
	u32 p_min;
	u32 p_max;
	bool portrait;
	char patch_file[32];
	int busnum;
};

const struct i2c_board_info *snowball_touch_get_plat_data(void);

#endif /* _LINUX_INPUT_FTK_H */





