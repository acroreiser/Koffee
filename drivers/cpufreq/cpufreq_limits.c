/*
 * drivers/cpufreq/cpufreq_limits.c
 *
 * Copyright (c) 2018, Shilin Victor <chrono.monochrome@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <linux/earlysuspend.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/kobject.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/sysfs.h>

static bool module_is_loaded = false; //FIXME: move code that uses module_is_loaded to init function
static bool cpu_freq_limits = false;
static bool __is_suspend = false;

static unsigned int screenoff_min_cpufreq = 200000;
static unsigned int screenoff_max_cpufreq = 1000000;

static unsigned int screenon_min_cpufreq = 0; // screenon_min_cpufreq and screenon_max_cpufreq uses system values
static unsigned int screenon_max_cpufreq = 0;

unsigned int input_boost_freq = 800000;
EXPORT_SYMBOL(input_boost_freq);
unsigned int input_boost_ms = 200;
EXPORT_SYMBOL(input_boost_ms);
u64 last_input_time;
EXPORT_SYMBOL(last_input_time);

#define MIN_INPUT_INTERVAL (150 * USEC_PER_MSEC)

extern int get_cpufreq_forced_state(void);
extern int get_prev_cpufreq(void);
extern void set_cpufreq_forced_state(bool);

int screen_off_max_cpufreq_get_(void) {
	return screenoff_max_cpufreq;
}
EXPORT_SYMBOL(screen_off_max_cpufreq_get_);

int screen_on_min_cpufreq_get_(void) {
	return screenon_min_cpufreq;
}
EXPORT_SYMBOL(screen_on_min_cpufreq_get_);

bool cpu_freq_limits_get(void) {
	return cpu_freq_limits;
}
EXPORT_SYMBOL(cpu_freq_limits_get);

bool is_suspend(void) {
	return __is_suspend;
}
EXPORT_SYMBOL(is_suspend);

int get_max_cpufreq(void) {
	struct cpufreq_policy *policy = cpufreq_cpu_get(0);

	return screenon_max_cpufreq ? screenon_max_cpufreq : policy->max;
}

int get_min_cpufreq(void) {
	struct cpufreq_policy *policy = cpufreq_cpu_get(0);

	return screenon_min_cpufreq ? screenon_min_cpufreq : policy->min;
}

void set_min_cpufreq(int freq) {
	struct cpufreq_policy *policy = cpufreq_cpu_get(0);

	policy->min = freq;
}

static void cpufreq_limits_update(bool is_suspend_) {
	int new_min, new_max;

	if (cpu_freq_limits) {
		/*
		 * since we don't have different cpufreq settings for different CPU cores,
		 * we'll update cpufreq limits only for first CPU core.
		 */
		struct cpufreq_policy *policy = cpufreq_cpu_get(0);

		new_min = is_suspend_ ? screenoff_min_cpufreq : screenon_min_cpufreq;
		new_max = is_suspend_ ? screenoff_max_cpufreq : screenon_max_cpufreq;

		if (!screenon_max_cpufreq || !screenon_min_cpufreq ||
		    !screenoff_min_cpufreq || ! screenoff_max_cpufreq) {
			if (!__is_suspend) {
				  screenon_min_cpufreq = policy->min;
				  screenon_max_cpufreq = policy->max;
			}

			return;
		}

		policy->min = new_min;
		policy->max = new_max;

		pr_err("[cpufreq_limits] new cpufreqs are %d - %d kHz\n", policy->min, policy->max);
	}
}

static struct work_struct early_suspend_work;
static struct work_struct late_resume_work;

static void early_suspend_fn(struct early_suspend *handler)
{
	schedule_work(&early_suspend_work);
}

static void late_resume_fn(struct early_suspend *handler)
{
	schedule_work(&late_resume_work);
}

static void early_suspend_work_fn(struct work_struct *work)
{
	cpufreq_limits_update(true);
	__is_suspend = true;
}

static void late_resume_work_fn(struct work_struct *work)
{
	cpufreq_limits_update(false);
	__is_suspend = false;
}

static DECLARE_WORK(early_suspend_work, early_suspend_work_fn);
static DECLARE_WORK(late_resume_work, late_resume_work_fn);

static struct early_suspend driver_early_suspend = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1,
	.suspend = early_suspend_fn,
	.resume = late_resume_fn,
};

static int cpufreq_callback(struct notifier_block *nfb,
		unsigned long event, void *data)
{
	struct cpufreq_policy *policy;
  
	if (event != CPUFREQ_ADJUST)
		return 0;
	
	policy = data; 
	  
	if (!__is_suspend) {
		screenon_max_cpufreq = policy->max;
		screenon_min_cpufreq = policy->min;
	}
	
	if (cpu_freq_limits) {
		
		if (!module_is_loaded) {
		  
			if (!screenoff_min_cpufreq)
				  screenoff_min_cpufreq = policy->min;
			if (!screenoff_max_cpufreq)
				  screenoff_max_cpufreq = policy->max;
			module_is_loaded = true;
		}
	  
		if (!__is_suspend) { 
			screenon_max_cpufreq = policy->max;
	
			screenon_min_cpufreq = policy->min;
		}	
	}

	return 0;
}

static struct notifier_block cpufreq_notifier_block = 
{
	.notifier_call = cpufreq_callback,
};

static ssize_t cpufreq_limits_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) 
{
	sprintf(buf,  "status: %s\n"
		      "Screen off settings:\n"
		      "min = %d kHz\n"
		      "max = %d kHz\n"
		      "Screen on settings:\n"
		      "min = %d kHz\n"
		      "max = %d kHz\n",
		      cpu_freq_limits ? "on" : "off",
		      screenoff_min_cpufreq,
		      screenoff_max_cpufreq,
		      screenon_min_cpufreq,  
		      screenon_max_cpufreq
	);

	return strlen(buf);
}

static ssize_t cpufreq_input_boost_freq_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) 
{
	sprintf(buf, "%d", input_boost_freq);
	return strlen(buf);
}

static ssize_t cpufreq_input_boost_freq_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret;
	
	ret = sscanf(&buf[0], "%d", &input_boost_freq);

	if (!ret)
		return -EINVAL;
	
	return count;
}

static struct kobj_attribute cpufreq_input_boost_freq_interface = __ATTR(input_boost_freq, 0644,
									   cpufreq_input_boost_freq_show,
									   cpufreq_input_boost_freq_store);

static ssize_t cpufreq_input_boost_ms_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) 
{
	sprintf(buf, "%d", input_boost_ms);
	return strlen(buf);
}

static ssize_t cpufreq_input_boost_ms_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret;
  
	ret = sscanf(&buf[0], "%d", &input_boost_ms);

	if (!ret)
		return -EINVAL;
	
	return count;
}

static struct kobj_attribute cpufreq_input_boost_ms_interface = __ATTR(input_boost_ms, 0644,
									   cpufreq_input_boost_ms_show,
									   cpufreq_input_boost_ms_store);

static ssize_t cpufreq_limits_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
  	if (!strncmp(&buf[0], "screenon_min=", 13)) {
		if (!sscanf(&buf[13], "%d", &screenon_min_cpufreq))
			goto invalid_input;
	}

	if (!strncmp(&buf[0], "screenon_max=", 13)) {
		if (!sscanf(&buf[13], "%d", &screenon_max_cpufreq))
			goto invalid_input;
	}
  
	if (!strncmp(buf, "on", 2)) {
		cpu_freq_limits = true;
		return count;
	}

	if (!strncmp(buf, "off", 3)) {
		cpu_freq_limits = false;
		return count;
	}
	
	if (!strncmp(&buf[0], "min=", 4)) {
		if (!sscanf(&buf[4], "%d", &screenoff_min_cpufreq))
			goto invalid_input;
	}

	if (!strncmp(&buf[0], "max=", 4)) {
		if (!sscanf(&buf[4], "%d", &screenoff_max_cpufreq))
			goto invalid_input;
	}
	
	return count;

invalid_input:
	pr_err("[cpufreq_limits] invalid input");
	return -EINVAL;
}

static struct kobj_attribute cpufreq_limits_interface = __ATTR(cpufreq_limits_on_suspend, 0644,
									   cpufreq_limits_show,
									   cpufreq_limits_store);

static struct attribute *cpufreq_attrs[] = {
	&cpufreq_limits_interface.attr,
	&cpufreq_input_boost_freq_interface.attr,
	&cpufreq_input_boost_ms_interface.attr,
	NULL,
};

static struct attribute_group cpufreq_interface_group = {
	.attrs = cpufreq_attrs,
};

static struct kobject *cpufreq_kobject;

static void cpufreq_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	u64 now;
	
	if (!input_boost_freq)
		return;

	now = ktime_to_us(ktime_get());
	if (now - last_input_time < MIN_INPUT_INTERVAL)
		return;

	last_input_time = ktime_to_us(ktime_get());
}

static int cpufreq_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void cpufreq_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpufreq_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler cpufreq_input_handler = {
	.event          = cpufreq_input_event,
	.connect        = cpufreq_input_connect,
	.disconnect     = cpufreq_input_disconnect,
	.name           = "cpufreq_input_boost",
	.id_table       = cpufreq_ids,
};

static int cpufreq_limits_driver_init(void)
{
	int ret;

	pr_err("[cpufreq_limits] initialized module with min %d and max %d MHz limits",
					 screenoff_min_cpufreq / 1000,  screenoff_max_cpufreq / 1000
	);

	register_early_suspend(&driver_early_suspend);

	if (!cpufreq_kobject) {
		cpufreq_kobject = kobject_create_and_add("cpufreq", kernel_kobj);
		if (!cpufreq_kobject) {
			pr_err("[cpufreq] Failed to create kobject interface\n");
		}
	}

	ret = sysfs_create_group(cpufreq_kobject, &cpufreq_interface_group);
	if (ret) {
		kobject_put(cpufreq_kobject);
	}

	ret = input_register_handler(&cpufreq_input_handler);

	if (ret)
		pr_err("Cannot register cpufreq input handler.\n");
/*
	if (prcmu_qos_add_requirement(PRCMU_QOS_APE_OPP, "APEBOOST", PRCMU_QOS_DEFAULT_VALUE))
		pr_err("pcrm_qos_add APE failed\n");

	if (prcmu_qos_add_requirement(PRCMU_QOS_DDR_OPP, "DDRBOOST", PRCMU_QOS_DEFAULT_VALUE)) 
		pr_err("pcrm_qos_add DDR failed\n");
*/
	cpufreq_register_notifier(&cpufreq_notifier_block, CPUFREQ_POLICY_NOTIFIER);

	return ret;
}

static void cpufreq_limits_driver_exit(void)
{
	//prcmu_qos_remove_requirement(PRCMU_QOS_APE_OPP, "APEBOOST");
	//prcmu_qos_remove_requirement(PRCMU_QOS_DDR_OPP, "DDRBOOST");

	unregister_early_suspend(&driver_early_suspend);
	sysfs_remove_group(cpufreq_kobject, &cpufreq_interface_group);
	if (cpufreq_kobject != NULL)
		kobject_put(cpufreq_kobject);
	cpufreq_unregister_notifier(&cpufreq_notifier_block, CPUFREQ_POLICY_NOTIFIER);
	input_unregister_handler(&cpufreq_input_handler);
}

module_init(cpufreq_limits_driver_init);
module_exit(cpufreq_limits_driver_exit);

MODULE_AUTHOR("Shilin Victor <chrono.monochrome@gmail.com>");
MODULE_DESCRIPTION("CPUfreq limits on suspend");
MODULE_LICENSE("GPL");
