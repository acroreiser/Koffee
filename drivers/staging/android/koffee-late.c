/* doze_helper.c
**
** Koffee helper module
**
** Copyright (C) 2018
**
** Yaroslav Zvezda <acroreiser@gmail.com>
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/earlysuspend.h>
#include <linux/kmod.h>

static char * argv5[] = { "bash", "/koffee-late.sh", NULL };

static char * envp[] = { "HOME=/", NULL };

static void koffee_hlp_early_suspend(struct early_suspend *h)
{
	call_usermodehelper("/system/xbin/bash", argv5, envp, UMH_NO_WAIT);
	unregister_early_suspend(&koffee_hlp_early_suspend_handler);

static struct early_suspend koffee_hlp_early_suspend_handler = 
{
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB,
	.suspend = koffee_hlp_early_suspend,
};

static int koffee_hlp_init(void)
{
	register_early_suspend(&koffee_hlp_early_suspend_handler);
	return 0;
}

static void koffee_hlp_exit(void)
{
}

module_init(koffee_hlp_init);
module_exit(koffee_hlp_exit);

MODULE_LICENSE("GPL");

