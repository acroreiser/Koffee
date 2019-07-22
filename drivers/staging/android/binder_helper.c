/* binder_helper.c
 *
 * Android IPC Subsystem
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/module.h>

int android_sdk_version = -1;
extern int binder32_init(void);
extern int binder64_init(void);

static bool is_binder32 = false;
static int set_binder32(const char *val, struct kernel_param *kp) {
	is_binder32 = false;
	if (strcmp(val, "0") < 0 && strcmp(val, "false") < 0)
		is_binder32 = true;

	return 0;

}

module_param_call(binder32, set_binder32, param_get_bool, &set_binder32, 0644);

int binder_helper_init(void)
{
	pr_err("%s: detected android_sdk_version=%d\n", __func__, android_sdk_version);
	if (android_sdk_version <= 25)
		is_binder32 = true;

	if (is_binder32)
		return binder32_init();

	return binder64_init();
}

//device_initcall(binder_helper_init);
