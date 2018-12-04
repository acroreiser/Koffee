/* kernel/power/earlysuspend.c
 *
 * Copyright (C) 2005-2008 Google, Inc.
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

#include <linux/earlysuspend.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rtc.h>
#include <linux/syscalls.h> /* sys_sync */
#include <linux/wakelock.h>
#include <linux/workqueue.h>
#ifdef CONFIG_ZRAM_FOR_ANDROID
#include <asm/atomic.h>
#endif /* CONFIG_ZRAM_FOR_ANDROID */
#ifdef CONFIG_PM_SYNC_CTRL
#include <linux/pm_sync_ctrl.h>
#endif /* CONFIG_PM_SYNC_CTRL */

#include "power.h"
#include <linux/delay.h>


standby_level_e standby_level;
static struct kobject *earlysuspend_kobj;

enum {
	DEBUG_USER_STATE = 1U << 0,
	DEBUG_SUSPEND = 1U << 2,
	DEBUG_VERBOSE = 1U << 3,
};
static int debug_mask = DEBUG_USER_STATE;
#ifdef CONFIG_ZRAM_FOR_ANDROID
atomic_t optimize_comp_on = ATOMIC_INIT(0);
EXPORT_SYMBOL(optimize_comp_on);
#endif /* CONFIG_ZRAM_FOR_ANDROID */

module_param_named(debug_mask, debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP);
static int debug_mask_delay_ms = 0;
module_param_named(debug_mask_delay_ms, debug_mask_delay_ms, int, S_IRUGO | S_IWUSR | S_IWGRP);

static DEFINE_MUTEX(early_suspend_lock);
static LIST_HEAD(early_suspend_handlers);
static void early_suspend(struct work_struct *work);
static void late_resume(struct work_struct *work);
static DECLARE_WORK(early_suspend_work, early_suspend);
static DECLARE_WORK(late_resume_work, late_resume);
static DEFINE_SPINLOCK(state_lock);
enum {
	SUSPEND_REQUESTED = 0x1,
	SUSPENDED = 0x2,
	SUSPEND_REQUESTED_AND_SUSPENDED = SUSPEND_REQUESTED | SUSPENDED,
};
static int state;

#ifdef CONFIG_EARLYSUSPEND_DELAY
extern struct wake_lock ealysuspend_delay_work;
#endif

static struct hrtimer earlysuspend_timer;
static void (*earlysuspend_func)(struct early_suspend *h);
static void (*stallfunc)(struct early_suspend *h);
static int earlysuspend_timeout_ms = 7000; //7s

void register_early_suspend(struct early_suspend *handler)
{
	struct list_head *pos;

	mutex_lock(&early_suspend_lock);
	list_for_each(pos, &early_suspend_handlers) {
		struct early_suspend *e;
		e = list_entry(pos, struct early_suspend, link);
		if (e->level > handler->level)
			break;
	}
	list_add_tail(&handler->link, pos);
	if ((state & SUSPENDED) && handler->suspend)
		handler->suspend(handler);
	mutex_unlock(&early_suspend_lock);
}
EXPORT_SYMBOL(register_early_suspend);

void unregister_early_suspend(struct early_suspend *handler)
{
	mutex_lock(&early_suspend_lock);
	list_del(&handler->link);
	mutex_unlock(&early_suspend_lock);
}
EXPORT_SYMBOL(unregister_early_suspend);

static void early_suspend(struct work_struct *work)
{
	struct early_suspend *pos;
	unsigned long irqflags;
	int abort = 0;
	ktime_t calltime;
	u64 usecs64;
	int usecs;
	ktime_t starttime;

	mutex_lock(&early_suspend_lock);
	spin_lock_irqsave(&state_lock, irqflags);
#ifdef CONFIG_ZRAM_FOR_ANDROID
	atomic_set(&optimize_comp_on, 1);
#endif /* CONFIG_ZRAM_FOR_ANDROID */
	if (state == SUSPEND_REQUESTED)
		state |= SUSPENDED;
	else
		abort = 1;
	spin_unlock_irqrestore(&state_lock, irqflags);

	if (abort) {
		if (debug_mask & DEBUG_SUSPEND)
			pr_info("early_suspend: abort, state %d\n", state);
		mutex_unlock(&early_suspend_lock);
		goto abort;
	}

	if (debug_mask & DEBUG_SUSPEND)
		pr_info("early_suspend: call handlers\n");

	/*in case some device blocking in the early suspend process, especially the devices after tp.*/
	hrtimer_cancel(&earlysuspend_timer);
	hrtimer_start(&earlysuspend_timer,
			ktime_set(earlysuspend_timeout_ms / 1000, (earlysuspend_timeout_ms % 1000) * 1000000),
			HRTIMER_MODE_REL);

	list_for_each_entry(pos, &early_suspend_handlers, link) {
		if (pos->suspend != NULL) {
			if (debug_mask & DEBUG_VERBOSE){
				pr_info("early_suspend: calling %pf\n", pos->suspend);
				starttime = ktime_get();
				
			}
			//backup suspend addr.
			earlysuspend_func = pos->suspend;
			pos->suspend(pos);

			if (debug_mask & DEBUG_VERBOSE){
				calltime = ktime_get();
				usecs64 = ktime_to_ns(ktime_sub(calltime, starttime));
				do_div(usecs64, NSEC_PER_USEC);
				usecs = usecs64;
				if (usecs == 0)
					usecs = 1;
				pr_info("early_suspend: %pf complete after %ld.%03ld msecs\n",
					pos->suspend, usecs / USEC_PER_MSEC, usecs % USEC_PER_MSEC);
				if(debug_mask_delay_ms > 0){
				    printk("sleep %d ms for debug. \n", debug_mask_delay_ms);
				    msleep(debug_mask_delay_ms);
				}
			}
		}
	}
	
	hrtimer_cancel(&earlysuspend_timer);
	standby_level = STANDBY_WITH_POWER;
	mutex_unlock(&early_suspend_lock);

	if (debug_mask & DEBUG_SUSPEND)
		pr_err("early_suspend: sync\n");
#ifdef CONFIG_PM_SYNC_CTRL
	if (pm_sync_active)
		sys_sync();
#else
	sys_sync();
#endif
abort:
	spin_lock_irqsave(&state_lock, irqflags);
	if (state == SUSPEND_REQUESTED_AND_SUSPENDED)
		wake_unlock(&main_wake_lock);
	spin_unlock_irqrestore(&state_lock, irqflags);
}

static void late_resume(struct work_struct *work)
{
	struct early_suspend *pos;
	unsigned long irqflags;
	int abort = 0;
	ktime_t calltime;
	u64 usecs64;
	int usecs;
	ktime_t starttime;

	mutex_lock(&early_suspend_lock);
	spin_lock_irqsave(&state_lock, irqflags);
#ifdef CONFIG_ZRAM_FOR_ANDROID
	atomic_set(&optimize_comp_on, 0);
#endif /* CONFIG_ZRAM_FOR_ANDROID */
	if (state == SUSPENDED)
		state &= ~SUSPENDED;
	else
		abort = 1;
	spin_unlock_irqrestore(&state_lock, irqflags);

	if (abort) {
		if (debug_mask & DEBUG_SUSPEND)
			pr_info("late_resume: abort, state %d\n", state);
		goto abort;
	}
	if (debug_mask & DEBUG_SUSPEND)
		pr_info("late_resume: call handlers\n");

	/*in case some device blocking in the late_resume process, especially the devices after tp.*/
	hrtimer_cancel(&earlysuspend_timer);
	hrtimer_start(&earlysuspend_timer,
			ktime_set(earlysuspend_timeout_ms / 1000, (earlysuspend_timeout_ms % 1000) * 1000000),
			HRTIMER_MODE_REL);
	
	list_for_each_entry_reverse(pos, &early_suspend_handlers, link) {
		if (pos->resume != NULL) {
			if (debug_mask & DEBUG_VERBOSE){
				pr_info("late_resume: calling %pf\n", pos->resume);
				starttime = ktime_get();
			}
			//backup resume addr.
			earlysuspend_func = pos->resume;
			pos->resume(pos);

			if (debug_mask & DEBUG_VERBOSE){
				calltime = ktime_get();
				usecs64 = ktime_to_ns(ktime_sub(calltime, starttime));
				do_div(usecs64, NSEC_PER_USEC);
				usecs = usecs64;
				if (usecs == 0)
					usecs = 1;
				pr_info("late_resume: %pf complete after %ld.%03ld msecs\n",
					pos->resume, usecs / USEC_PER_MSEC, usecs % USEC_PER_MSEC);
				if(debug_mask_delay_ms > 0){
				    printk("sleep %d ms for debug. \n", debug_mask_delay_ms);
				    msleep(debug_mask_delay_ms);
				}
			}
	
		}
	}
	
	
	if (debug_mask & DEBUG_SUSPEND)
		pr_info("late_resume: done\n");

	hrtimer_cancel(&earlysuspend_timer);

	standby_level = STANDBY_INITIAL;
abort:
	mutex_unlock(&early_suspend_lock);
}

void request_suspend_state(suspend_state_t new_state)
{
	unsigned long irqflags;
	int old_sleep;

	spin_lock_irqsave(&state_lock, irqflags);
	old_sleep = state & SUSPEND_REQUESTED;
	if (debug_mask & DEBUG_USER_STATE) {
		struct timespec ts;
		struct rtc_time tm;
		getnstimeofday(&ts);
		rtc_time_to_tm(ts.tv_sec, &tm);
		pr_info("request_suspend_state: %s (%d->%d) at %lld "
			"(%d-%02d-%02d %02d:%02d:%02d.%09lu UTC)\n",
			new_state != PM_SUSPEND_ON ? "sleep" : "wakeup",
			requested_suspend_state, new_state,
			ktime_to_ns(ktime_get()),
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
	}
	if (!old_sleep && new_state != PM_SUSPEND_ON) {
		state |= SUSPEND_REQUESTED;

        #ifdef CONFIG_EARLYSUSPEND_DELAY
        /* delay 5 seconds to enter suspend */
        wake_unlock(&ealysuspend_delay_work);
        wake_lock_timeout(&ealysuspend_delay_work, HZ * 5);
        #endif

		queue_work(suspend_work_queue, &early_suspend_work);
	} else if (old_sleep && new_state == PM_SUSPEND_ON) {
		state &= ~SUSPEND_REQUESTED;
		wake_lock(&main_wake_lock);
		queue_work(suspend_work_queue, &late_resume_work);
	}
	requested_suspend_state = new_state;
	spin_unlock_irqrestore(&state_lock, irqflags);
}

suspend_state_t get_suspend_state(void)
{
	return requested_suspend_state;
}

static enum hrtimer_restart earlysuspend_timer_func(struct hrtimer *timer)
{
	//record the lastest time stall function point.
	stallfunc = earlysuspend_func;
	printk("NOTICE: called earlysuspend_func or lateresume func = %pf. \
		stalled. \n", stallfunc);
	return HRTIMER_NORESTART;
}

/*it is used for clear the stallfunc's state*/
static ssize_t earlysuspend_stallfunc_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long data;
	int error;

	error = strict_strtoul(buf, 16, &data);
	if (error)
		return error;

	//data represent the func's addr
	stallfunc = (void (*)(struct early_suspend *h))(data);
	
	return count;
}


static ssize_t earlysuspend_stallfunc_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	if(NULL == stallfunc){		
		return sprintf(buf, "0\n"); 
	}else{
		return sprintf(buf, "%pf\n", stallfunc);
	}
}


static DEVICE_ATTR(debug_stallfunc, S_IRUGO|S_IWUSR|S_IWGRP,
		earlysuspend_stallfunc_show, earlysuspend_stallfunc_store);
		
static struct attribute *earlysuspend_attributes[] = {
	&dev_attr_debug_stallfunc.attr,
	NULL
};

static struct attribute_group earlysuspend_attribute_group = {
	.attrs = earlysuspend_attributes
};


/* kernel/power/powersuspend.c
 *
 * Copyright (C) 2005-2008 Google, Inc.
 * Copyright (C) 2013 Paul Reioux
 *
 * Modified by Jean-Pierre Rasquin <yank555.lu@gmail.com>
 *
 *  v1.1 - make powersuspend not depend on a userspace initiator anymore,
 *         but use a hook in autosleep instead.
 *
 *  v1.2 - make kernel / userspace mode switchable
 *
 *  v1.3 - add a hook in display panel driver as alternative kernel trigger
 *
 *  v1.4 - add a hybrid-kernel mode, accepting both kernel hooks (first wins)
 *
 *  v1.5 - fix hybrid-kernel mode cannot be set through sysfs
 *
 *  v1.6 - replace display pannel hooks by fb_notify
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

#include <linux/powersuspend.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/fb.h>

#define MAJOR_VERSION	1
#define MINOR_VERSION	6

#define POWER_SUSPEND_DEBUG // Add debugging prints in dmesg

/* the display on process started */
#define LCD_EVENT_ON_START		0x01
/* the display on process end */
#define LCD_EVENT_ON_END		0x02
/* the display off process started */
#define LCD_EVENT_OFF_START		0x03
/* the display off process end */
#define LCD_EVENT_OFF_END		0x04

extern struct workqueue_struct *suspend_work_queue;

static DEFINE_MUTEX(power_suspend_lock);
static LIST_HEAD(power_suspend_handlers);
static void power_suspend(struct work_struct *work);
static void power_resume(struct work_struct *work);
static DECLARE_WORK(power_suspend_work, power_suspend);
static DECLARE_WORK(power_resume_work, power_resume);
static struct notifier_block fb_notifier_hook;

static int state; // Yank555.lu : Current powersave state (screen on / off)
static int mode;  // Yank555.lu : Current powersave mode  (kernel / userspace / panel / hybrid)

void register_power_suspend(struct power_suspend *handler)
{
	struct list_head *pos;

	mutex_lock(&power_suspend_lock);
	list_for_each(pos, &power_suspend_handlers) {
		struct power_suspend *p;
		p = list_entry(pos, struct power_suspend, link);
	}
	list_add_tail(&handler->link, pos);
	mutex_unlock(&power_suspend_lock);
}
EXPORT_SYMBOL(register_power_suspend);

void unregister_power_suspend(struct power_suspend *handler)
{
	mutex_lock(&power_suspend_lock);
	list_del(&handler->link);
	mutex_unlock(&power_suspend_lock);
}
EXPORT_SYMBOL(unregister_power_suspend);

static void power_suspend(struct work_struct *work)
{
	struct power_suspend *pos;
	unsigned long irqflags;
	int abort = 0;

	#ifdef POWER_SUSPEND_DEBUG
	pr_info("[POWERSUSPEND] entering suspend...\n");
	#endif
	mutex_lock(&power_suspend_lock);
	spin_lock_irqsave(&state_lock, irqflags);
	if (state == POWER_SUSPEND_INACTIVE)
		abort = 1;
	spin_unlock_irqrestore(&state_lock, irqflags);

	if (abort)
		goto abort_suspend;

	#ifdef POWER_SUSPEND_DEBUG
	pr_info("[POWERSUSPEND] suspending...\n");
	#endif
	list_for_each_entry(pos, &power_suspend_handlers, link) {
		if (pos->suspend != NULL) {
			pos->suspend(pos);
		}
	}
	#ifdef POWER_SUSPEND_DEBUG
	pr_info("[POWERSUSPEND] suspend completed.\n");
	#endif
abort_suspend:
	mutex_unlock(&power_suspend_lock);
}

static void power_resume(struct work_struct *work)
{
	struct power_suspend *pos;
	unsigned long irqflags;
	int abort = 0;

	#ifdef POWER_SUSPEND_DEBUG
	pr_info("[POWERSUSPEND] entering resume...\n");
	#endif
	mutex_lock(&power_suspend_lock);
	spin_lock_irqsave(&state_lock, irqflags);
	if (state == POWER_SUSPEND_ACTIVE)
		abort = 1;
	spin_unlock_irqrestore(&state_lock, irqflags);

	if (abort)
		goto abort_resume;

	#ifdef POWER_SUSPEND_DEBUG
	pr_info("[POWERSUSPEND] resuming...\n");
	#endif
	list_for_each_entry_reverse(pos, &power_suspend_handlers, link) {
		if (pos->resume != NULL) {
			pos->resume(pos);
		}
	}
	#ifdef POWER_SUSPEND_DEBUG
	pr_info("[POWERSUSPEND] resume completed.\n");
	#endif
abort_resume:
	mutex_unlock(&power_suspend_lock);
}

void set_power_suspend_state(int new_state)
{
	unsigned long irqflags;

	spin_lock_irqsave(&state_lock, irqflags);
	if (state == POWER_SUSPEND_INACTIVE && new_state == POWER_SUSPEND_ACTIVE) {
		#ifdef POWER_SUSPEND_DEBUG
		pr_info("[POWERSUSPEND] suspend state activated.\n");
		#endif
		state = new_state;
		queue_work(suspend_work_queue, &power_suspend_work);
	} else if (state == POWER_SUSPEND_ACTIVE && new_state == POWER_SUSPEND_INACTIVE) {
		#ifdef POWER_SUSPEND_DEBUG
		pr_info("[POWERSUSPEND] suspend state deactivated.\n");
		#endif
		state = new_state;
		queue_work(suspend_work_queue, &power_resume_work);
	}
	spin_unlock_irqrestore(&state_lock, irqflags);
}

// Autosleep hook

void set_power_suspend_state_autosleep_hook(int new_state)
{
	#ifdef POWER_SUSPEND_DEBUG
	pr_info("[POWERSUSPEND] autosleep resquests %s.\n", new_state == POWER_SUSPEND_ACTIVE ? "sleep" : "wakeup");
	#endif
	// Yank555.lu : Only allow autosleep hook changes in autosleep & hybrid mode
	if (mode == POWER_SUSPEND_AUTOSLEEP || mode == POWER_SUSPEND_HYBRID)
		set_power_suspend_state(new_state);
}

EXPORT_SYMBOL(set_power_suspend_state_autosleep_hook);

// fb_notification hook

static int fb_notifier_call(struct notifier_block *this,
				unsigned long event, void *data)
{
	#ifdef POWER_SUSPEND_DEBUG
	if (event == LCD_EVENT_ON_START || event == LCD_EVENT_OFF_START)
		pr_info("[POWERSUSPEND] panel resquests %s.\n", event == LCD_EVENT_OFF_START ? "sleep" : "wakeup");
	#endif
	if (mode == POWER_SUSPEND_PANEL || mode == POWER_SUSPEND_HYBRID)
		switch (event) {
			case LCD_EVENT_ON_START: // early notification for resume (alt. use _END for late notification)
				set_power_suspend_state(POWER_SUSPEND_INACTIVE);
				break;
			case LCD_EVENT_OFF_START: // early notification for suspend (alt. use _END for late notification)
				set_power_suspend_state(POWER_SUSPEND_ACTIVE);
				break;
		}

	return 0;
}

// ------------------------------------------ sysfs interface ------------------------------------------

static ssize_t power_suspend_state_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
        return sprintf(buf, "%u\n", state);
}

static ssize_t power_suspend_state_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int new_state = 0;

	// Yank555.lu : Only allow sysfs changes from userspace mode
	if (mode != POWER_SUSPEND_USERSPACE)
		return -EINVAL;

	sscanf(buf, "%d\n", &new_state);

	#ifdef POWER_SUSPEND_DEBUG
	pr_info("[POWERSUSPEND] userspace resquests %s.\n", new_state == POWER_SUSPEND_ACTIVE ? "sleep" : "wakeup");
	#endif
	if(new_state == POWER_SUSPEND_ACTIVE || new_state == POWER_SUSPEND_INACTIVE)
		set_power_suspend_state(new_state);

	return count;
}

static struct kobj_attribute power_suspend_state_attribute =
	__ATTR(power_suspend_state, 0666,
		power_suspend_state_show,
		power_suspend_state_store);

static ssize_t power_suspend_mode_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
        return sprintf(buf, "%u\n", mode);
}

static ssize_t power_suspend_mode_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int data = 0;

	sscanf(buf, "%d\n", &data);

	switch (data) {
		case POWER_SUSPEND_AUTOSLEEP:
		case POWER_SUSPEND_PANEL:
		case POWER_SUSPEND_USERSPACE:
		case POWER_SUSPEND_HYBRID:	mode = data;
						return count;
		default:
			return -EINVAL;
	}
	
}

static struct kobj_attribute power_suspend_mode_attribute =
	__ATTR(power_suspend_mode, 0666,
		power_suspend_mode_show,
		power_suspend_mode_store);

static ssize_t power_suspend_version_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "version: %d.%d\n", MAJOR_VERSION, MINOR_VERSION);
}

static struct kobj_attribute power_suspend_version_attribute =
	__ATTR(power_suspend_version, 0444,
		power_suspend_version_show,
		NULL);

static struct attribute *power_suspend_attrs[] =
{
	&power_suspend_state_attribute.attr,
	&power_suspend_mode_attribute.attr,
	&power_suspend_version_attribute.attr,
	NULL,
};

static struct attribute_group power_suspend_attr_group =
{
	.attrs = power_suspend_attrs,
};

static struct kobject *power_suspend_kobj;

#if 0
// ------------------ sysfs interface -----------------------
static int __init power_suspend_init(void)
{

	int sysfs_result;

        power_suspend_kobj = kobject_create_and_add("power_suspend",
				kernel_kobj);
        if (!power_suspend_kobj) {
                pr_err("%s kobject create failed!\n", __FUNCTION__);
                return -ENOMEM;
        }

        sysfs_result = sysfs_create_group(power_suspend_kobj,
			&power_suspend_attr_group);

        if (sysfs_result) {
                pr_info("%s group create failed!\n", __FUNCTION__);
                kobject_put(power_suspend_kobj);
                return -ENOMEM;
        }

	fb_notifier_hook.notifier_call = fb_notifier_call;
	if (fb_register_client(&fb_notifier_hook) != 0) {
                pr_info("%s fb_notify hook create failed!\n", __FUNCTION__);
                return -ENOMEM;
	}

//	mode = POWER_SUSPEND_AUTOSLEEP;	// Yank555.lu : Default to autosleep mode
//	mode = POWER_SUSPEND_USERSPACE;	// Yank555.lu : Default to userspace mode
//	mode = POWER_SUSPEND_PANEL;	// Yank555.lu : Default to display panel mode
	mode = POWER_SUSPEND_HYBRID;	// Yank555.lu : Default to display panel / autosleep hybrid mode

	return 0;
}

core_initcall(power_suspend_init);

#endif

static int __init earlysuspend_init(void)
{

	int sysfs_result;

        power_suspend_kobj = kobject_create_and_add("power_suspend",
				kernel_kobj);
        if (!power_suspend_kobj) {
                pr_err("%s kobject create failed!\n", __FUNCTION__);
                return -ENOMEM;
        }

        sysfs_result = sysfs_create_group(power_suspend_kobj,
			&power_suspend_attr_group);

        if (sysfs_result) {
                pr_info("%s group create failed!\n", __FUNCTION__);
                kobject_put(power_suspend_kobj);
                return -ENOMEM;
        }

	fb_notifier_hook.notifier_call = fb_notifier_call;
	if (fb_register_client(&fb_notifier_hook) != 0) {
                pr_info("%s fb_notify hook create failed!\n", __FUNCTION__);
                return -ENOMEM;
	}

//	mode = POWER_SUSPEND_AUTOSLEEP;	// Yank555.lu : Default to autosleep mode
//	mode = POWER_SUSPEND_USERSPACE;	// Yank555.lu : Default to userspace mode
//	mode = POWER_SUSPEND_PANEL;	// Yank555.lu : Default to display panel mode
	mode = POWER_SUSPEND_HYBRID;	// Yank555.lu : Default to display panel / autosleep hybrid mode

	stallfunc = NULL;
	hrtimer_init(&earlysuspend_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	earlysuspend_timer.function = earlysuspend_timer_func;

	earlysuspend_kobj = kobject_create_and_add("earlysuspend", power_kobj);
	if (!earlysuspend_kobj)
		return -ENOMEM;
	return sysfs_create_group(earlysuspend_kobj, &earlysuspend_attribute_group);

	return 0;
}

static void __exit power_suspend_exit(void)
{
	if (power_suspend_kobj != NULL)
		kobject_put(power_suspend_kobj);
}

core_initcall(earlysuspend_init);
module_exit(power_suspend_exit);

MODULE_AUTHOR("Paul Reioux <reioux@gmail.com> / Jean-Pierre Rasquin <yank555.lu@gmail.com>");
MODULE_DESCRIPTION("power_suspend - A replacement kernel PM driver for"
        "Android's deprecated early_suspend/late_resume PM driver!");
MODULE_LICENSE("GPL v2");

