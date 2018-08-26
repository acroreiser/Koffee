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

static int hooked = 0;
static char * argv5[] = { "bash", "/koffee-late.sh", NULL };
static char * envp[] = { "HOME=/", NULL };

static void koffee_hlp_early_suspend(struct early_suspend *h);

static struct early_suspend koffee_hlp_early_suspend_handler = 
{
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB,
	.suspend = koffee_hlp_early_suspend,
};

static void koffee_hlp_early_suspend(struct early_suspend *h)
{
	call_usermodehelper("/system/xbin/bash", argv5, envp, UMH_NO_WAIT);
}


static int koffee_hlp_init(void)
{
	register_early_suspend(&koffee_hlp_early_suspend_handler);
	return 0;
}

static void koffee_hlp_exit(void)
{
}

static int koffee_set_int(const char *val, const struct kernel_param *kp)
{
	unsigned short* pvalue = kp->arg;
    int res = hooked_set_int(val, kp);

    if(res == 0)
    {
        if(hooked == 1)
        {
        	unregister_early_suspend(&koffee_hlp_early_suspend_handler);
        	printk(KERN_INFO "Koffee-Late: hooked!");
        }
    }
    return res;
}

const struct kernel_param_ops koffee_hook_int = 
{
    .set = &koffee_set_int,
};
module_param_cb(hooked, &koffee_hook_int, &hooked, S_IWUGO)

module_init(koffee_hlp_init);
module_exit(koffee_hlp_exit);

MODULE_LICENSE("GPL");

