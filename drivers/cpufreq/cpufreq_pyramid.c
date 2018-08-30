/*
 * drivers/cpufreq/cpufreq_pyramid.c
 *
 * Based on drivers/cpufreq/cpufreq_pyramid.c
 *
 * Copyright (C) 2010 Google, Inc.,
 * 				 2018 A$teroid
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
 * Authors: Mike Chan (mike@android.com),
 *		   Jaroslav zvezda (acroreiser@gmail.com)
 *
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <asm/cputime.h>

#include <linux/earlysuspend.h>

static atomic_t active_count = ATOMIC_INIT(0);

extern unsigned int msm_enabled;
extern unsigned int mc_eco;
extern unsigned int max_cpus_on;
extern unsigned int min_cpus_on;
static unsigned int min_cpus = 1;
static unsigned int max_cpus = 4;
static unsigned int mc_auto;
static unsigned int mc_eco_factor = 11;
static unsigned int mc_step1 = 1200000;
static unsigned int mc_step2 = 900000;
static unsigned int mc_step3 = 600000;
static unsigned int mc_2_4 = 1;
static unsigned int mc_2_4_freq = 1000000;

struct cpufreq_pyramid_cpuinfo {
	struct timer_list cpu_timer;
	int timer_idlecancel;
	u64 time_in_idle;
	u64 idle_exit_time;
	u64 timer_run_time;
	int idling;
	u64 target_set_time;
	u64 target_set_time_in_idle;
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *freq_table;
	unsigned int target_freq;
	unsigned int floor_freq;
	u64 floor_validate_time;
	int governor_enabled;
};

static DEFINE_PER_CPU(struct cpufreq_pyramid_cpuinfo, cpuinfo);

/* Workqueues handle frequency scaling */
static struct task_struct *up_task;
static struct workqueue_struct *down_wq;
static struct work_struct freq_scale_down_work;
static cpumask_t up_cpumask;
static spinlock_t up_cpumask_lock;
static cpumask_t down_cpumask;
static spinlock_t down_cpumask_lock;
static struct mutex set_speed_lock;



static int screenoff_limit_s;
static int screenoff;
static int scr_off_freq = 900000;
static int screenoff_max_cpus_on = 2;
#ifdef CONFIG_EXYNOS4_EXPORT_TEMP
static int temp_factor = 1;
#endif



/*
 * The minimum amount of time to spend at a frequency before we can ramp down.
 */
#define DEFAULT_MIN_SAMPLE_TIME (5 * USEC_PER_MSEC)
static unsigned long min_sample_time;

/*
 * The sample rate of the timer used to increase frequency
 */
#define DEFAULT_TIMER_RATE (10 * USEC_PER_MSEC)
static unsigned long timer_rate;

static int cpufreq_governor_pyramid(struct cpufreq_policy *policy,
		unsigned int event);

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_PYRAMID
static
#endif
struct cpufreq_governor cpufreq_gov_pyramid = {
	.name = "pyramid",
	.governor = cpufreq_governor_pyramid,
	.max_transition_latency = 10000000,
	.owner = THIS_MODULE,
};

#ifdef CONFIG_EXYNOS4_EXPORT_TEMP
extern unsigned int get_exynos4_temperature(void);
#endif

static void cpufreq_pyramid_timer(unsigned long data)
{
	unsigned int delta_idle;
	unsigned int delta_time;
	unsigned int temperature;
	int cpu_load;
	int load_since_change;
	u64 time_in_idle;
	u64 idle_exit_time;
	struct cpufreq_pyramid_cpuinfo *pcpu =
		&per_cpu(cpuinfo, data);
	u64 now_idle;
	unsigned int new_freq;
	unsigned int index;
	unsigned long flags;
	int soft_max;

	unsigned int cpus;

	smp_rmb();

	if (!pcpu->governor_enabled)
		goto exit;

	soft_max = pcpu->policy->max;
	/*
	 * Once pcpu->timer_run_time is updated to >= pcpu->idle_exit_time,
	 * this lets idle exit know the current idle time sample has
	 * been processed, and idle exit can generate a new sample and
	 * re-arm the timer.  This prevents a concurrent idle
	 * exit on that CPU from writing a new set of info at the same time
	 * the timer function runs (the timer function can't use that info
	 * until more time passes).
	 */
	time_in_idle = pcpu->time_in_idle;
	idle_exit_time = pcpu->idle_exit_time;
	now_idle = get_cpu_idle_time_us(data, &pcpu->timer_run_time);
	smp_wmb();

	/* If we raced with cancelling a timer, skip. */
	if (!idle_exit_time)
		goto exit;

	delta_idle = (unsigned int) cputime64_sub(now_idle, time_in_idle);
	delta_time = (unsigned int) cputime64_sub(pcpu->timer_run_time,
						  idle_exit_time);

	/*
	 * If timer ran less than 1ms after short-term sample started, retry.
	 */
	if (delta_time < 1000)
		goto rearm;

	if (delta_idle > delta_time)
		cpu_load = 0;
	else
		cpu_load = 100 * (delta_time - delta_idle) / delta_time;

	delta_idle = (unsigned int) cputime64_sub(now_idle,
						pcpu->target_set_time_in_idle);
	delta_time = (unsigned int) cputime64_sub(pcpu->timer_run_time,
						  pcpu->target_set_time);

	if ((delta_time == 0) || (delta_idle > delta_time))
		load_since_change = 0;
	else
		load_since_change =
			100 * (delta_time - delta_idle) / delta_time;

	/*
	 * Choose greater of short-term load (since last idle timer
	 * started or timer function re-armed itself) or long-term load
	 * (since last frequency change).
	 */
	if (load_since_change > cpu_load)
		cpu_load = load_since_change;

		new_freq = pcpu->policy->max * cpu_load / 100;

	if (cpufreq_frequency_table_target(pcpu->policy, pcpu->freq_table,
					   new_freq, CPUFREQ_RELATION_H,
					   &index)) {
		pr_warn_once("timer %d: cpufreq_frequency_table_target error\n",
			     (int) data);
		goto rearm;
	}

	new_freq = pcpu->freq_table[index].frequency;

	cpus = num_online_cpus();
	if(new_freq == 1704000)
		new_freq = 1700000;

		if(screenoff == 1 && screenoff_limit_s == 1)
		{	
			if(new_freq > scr_off_freq)
				new_freq = scr_off_freq;

		} 
		if(mc_eco == 1)
		{
			if (mc_2_4 == 1)
			{
				if(cpus > 2)
				{
					if(new_freq > mc_2_4_freq)
						new_freq = mc_2_4_freq;	
				}
			}
			else if (mc_auto == 1)
			{
				if(num_online_cpus() > 1)
					soft_max -= (pcpu->policy->max / 100) * (num_online_cpus() * mc_eco_factor);

				if(new_freq > soft_max)
				{
				 	new_freq = soft_max;
				 	if (cpufreq_frequency_table_target(pcpu->policy, pcpu->freq_table,
					   new_freq, CPUFREQ_RELATION_H,
					   &index)) {
						pr_warn_once("timer %d: cpufreq_frequency_table_target error\n",
			     			(int) data);
						goto rearm;
					}
					soft_max = pcpu->policy->max;

					new_freq = pcpu->freq_table[index].frequency;
				}	
			} else
			{
				switch (cpus){
					case 2:
						if(new_freq > mc_step1)
							new_freq = mc_step1;
					break;
					case 3:
						if(new_freq > mc_step2)
							new_freq = mc_step2;
					break;
					case 4:
						if(new_freq > mc_step3)
							new_freq = mc_step3;
					break;
				}
			}
		}
#ifdef CONFIG_EXYNOS4_EXPORT_TEMP
		else
		{
			if(temp_factor == 1)
			{
				temperature = get_exynos4_temperature();

				if(temperature >= 50)
				{
					if(new_freq > 1000000)
						new_freq = new_freq - 400000;
					else
						if(new_freq > 600000)
							new_freq = new_freq - 200000;
						else
							if(new_freq >= 300000)
								new_freq = new_freq - 100000;

				}
				else
				{
					if(temperature >= 40)
					{
						if(new_freq > 1000000)
							new_freq = new_freq - 200000;
						else
							if(new_freq > 600000)
								new_freq = new_freq - 100000;
					}
				}
			}
		}
#endif

	if(new_freq == 1700000)
		new_freq = 1704000;


	pcpu->floor_freq = new_freq;
	pcpu->floor_validate_time = pcpu->timer_run_time;

	if (pcpu->target_freq == new_freq) {
		goto rearm_if_notmax;
	}
	pcpu->target_set_time_in_idle = now_idle;
	pcpu->target_set_time = pcpu->timer_run_time;

	if (new_freq < pcpu->target_freq) {
		pcpu->target_freq = new_freq;
		spin_lock_irqsave(&down_cpumask_lock, flags);
		cpumask_set_cpu(data, &down_cpumask);
		spin_unlock_irqrestore(&down_cpumask_lock, flags);
		queue_work(down_wq, &freq_scale_down_work);
	} else {
		pcpu->target_freq = new_freq;
		spin_lock_irqsave(&up_cpumask_lock, flags);
		cpumask_set_cpu(data, &up_cpumask);
		spin_unlock_irqrestore(&up_cpumask_lock, flags);
		wake_up_process(up_task);
	}

rearm_if_notmax:
	/*
	 * Already set max speed and don't see a need to change that,
	 * wait until next idle to re-evaluate, don't need timer.
	 */
	if (pcpu->target_freq == pcpu->policy->max)
		goto exit;

rearm:
	if (!timer_pending(&pcpu->cpu_timer)) {
		/*
		 * If already at min: if that CPU is idle, don't set timer.
		 * Else cancel the timer if that CPU goes idle.  We don't
		 * need to re-evaluate speed until the next idle exit.
		 */
		if (pcpu->target_freq == pcpu->policy->min) {
			smp_rmb();

			if (pcpu->idling)
				goto exit;

			pcpu->timer_idlecancel = 1;
		}

		pcpu->time_in_idle = get_cpu_idle_time_us(
			data, &pcpu->idle_exit_time);
		mod_timer(&pcpu->cpu_timer,
			  jiffies + usecs_to_jiffies(timer_rate));
	}

exit:
	return;
}

static void cpufreq_pyramid_idle_start(void)
{
	struct cpufreq_pyramid_cpuinfo *pcpu =
		&per_cpu(cpuinfo, smp_processor_id());
	int pending;

	if (!pcpu->governor_enabled)
		return;

	pcpu->idling = 1;
	smp_wmb();
	pending = timer_pending(&pcpu->cpu_timer);

	if (pcpu->target_freq != pcpu->policy->min) {
#ifdef CONFIG_SMP
		/*
		 * Entering idle while not at lowest speed.  On some
		 * platforms this can hold the other CPU(s) at that speed
		 * even though the CPU is idle. Set a timer to re-evaluate
		 * speed so this idle CPU doesn't hold the other CPUs above
		 * min indefinitely.  This should probably be a quirk of
		 * the CPUFreq driver.
		 */
		if (!pending) {
			pcpu->time_in_idle = get_cpu_idle_time_us(
				smp_processor_id(), &pcpu->idle_exit_time);
			pcpu->timer_idlecancel = 0;
			mod_timer(&pcpu->cpu_timer,
				  jiffies + usecs_to_jiffies(timer_rate));
		}
#endif
	} else {
		/*
		 * If at min speed and entering idle after load has
		 * already been evaluated, and a timer has been set just in
		 * case the CPU suddenly goes busy, cancel that timer.  The
		 * CPU didn't go busy; we'll recheck things upon idle exit.
		 */
		if (pending && pcpu->timer_idlecancel) {
			del_timer(&pcpu->cpu_timer);
			/*
			 * Ensure last timer run time is after current idle
			 * sample start time, so next idle exit will always
			 * start a new idle sampling period.
			 */
			pcpu->idle_exit_time = 0;
			pcpu->timer_idlecancel = 0;
		}
	}

}

static void cpufreq_pyramid_idle_end(void)
{
	struct cpufreq_pyramid_cpuinfo *pcpu =
		&per_cpu(cpuinfo, smp_processor_id());

	pcpu->idling = 0;
	smp_wmb();

	/*
	 * Arm the timer for 1-2 ticks later if not already, and if the timer
	 * function has already processed the previous load sampling
	 * interval.  (If the timer is not pending but has not processed
	 * the previous interval, it is probably racing with us on another
	 * CPU.  Let it compute load based on the previous sample and then
	 * re-arm the timer for another interval when it's done, rather
	 * than updating the interval start time to be "now", which doesn't
	 * give the timer function enough time to make a decision on this
	 * run.)
	 */
	if (timer_pending(&pcpu->cpu_timer) == 0 &&
	    pcpu->timer_run_time >= pcpu->idle_exit_time &&
	    pcpu->governor_enabled) {
		pcpu->time_in_idle =
			get_cpu_idle_time_us(smp_processor_id(),
					     &pcpu->idle_exit_time);
		pcpu->timer_idlecancel = 0;
		mod_timer(&pcpu->cpu_timer,
			  jiffies + usecs_to_jiffies(timer_rate));
	}

}

static int cpufreq_pyramid_up_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct cpufreq_pyramid_cpuinfo *pcpu;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&up_cpumask_lock, flags);

		if (cpumask_empty(&up_cpumask)) {
			spin_unlock_irqrestore(&up_cpumask_lock, flags);
			schedule();

			if (kthread_should_stop())
				break;

			spin_lock_irqsave(&up_cpumask_lock, flags);
		}

		set_current_state(TASK_RUNNING);
		tmp_mask = up_cpumask;
		cpumask_clear(&up_cpumask);
		spin_unlock_irqrestore(&up_cpumask_lock, flags);

		for_each_cpu(cpu, &tmp_mask) {
			unsigned int j;
			unsigned int max_freq = 0;

			pcpu = &per_cpu(cpuinfo, cpu);
			smp_rmb();

			if (!pcpu->governor_enabled)
				continue;

			mutex_lock(&set_speed_lock);

			for_each_cpu(j, pcpu->policy->cpus) {
				struct cpufreq_pyramid_cpuinfo *pjcpu =
					&per_cpu(cpuinfo, j);

				if (pjcpu->target_freq > max_freq)
					max_freq = pjcpu->target_freq;
			}

			if (max_freq != pcpu->policy->cur)
				__cpufreq_driver_target(pcpu->policy,
							max_freq,
							CPUFREQ_RELATION_H);
			mutex_unlock(&set_speed_lock);
		}
	}

	return 0;
}

static void cpufreq_pyramid_freq_down(struct work_struct *work)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct cpufreq_pyramid_cpuinfo *pcpu;

	spin_lock_irqsave(&down_cpumask_lock, flags);
	tmp_mask = down_cpumask;
	cpumask_clear(&down_cpumask);
	spin_unlock_irqrestore(&down_cpumask_lock, flags);

	for_each_cpu(cpu, &tmp_mask) {
		unsigned int j;
		unsigned int max_freq = 0;

		pcpu = &per_cpu(cpuinfo, cpu);
		smp_rmb();

		if (!pcpu->governor_enabled)
			continue;

		mutex_lock(&set_speed_lock);

		for_each_cpu(j, pcpu->policy->cpus) {
			struct cpufreq_pyramid_cpuinfo *pjcpu =
				&per_cpu(cpuinfo, j);

			if (pjcpu->target_freq > max_freq)
				max_freq = pjcpu->target_freq;
		}

		if (max_freq != pcpu->policy->cur)
			__cpufreq_driver_target(pcpu->policy, max_freq,
						CPUFREQ_RELATION_H);

		mutex_unlock(&set_speed_lock);
	}
}


static ssize_t show_min_sample_time(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", min_sample_time);
}

static ssize_t store_min_sample_time(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	min_sample_time = val;
	return count;
}

static struct global_attr min_sample_time_attr = __ATTR(min_sample_time, 0644,
		show_min_sample_time, store_min_sample_time);


static ssize_t show_timer_rate(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", timer_rate);
}

static ssize_t store_timer_rate(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	timer_rate = val;
	return count;
}

static struct global_attr timer_rate_attr = __ATTR(timer_rate, 0644,
		show_timer_rate, store_timer_rate);


static ssize_t show_screenoff_limit(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", screenoff_limit_s);
}

static ssize_t store_screenoff_limit(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 0 || val > 1)
		return -EINVAL;

	screenoff_limit_s = val;
	return count;
}

define_one_global_rw(screenoff_limit);

static ssize_t show_mc_pseudocluster(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", mc_2_4);
}

static ssize_t store_mc_pseudocluster(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 0 || val > 1)
		return -EINVAL;

	mc_2_4 = val;
	return count;
}

define_one_global_rw(mc_pseudocluster);

static ssize_t show_mc_pseudocluster_freq(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", mc_2_4_freq);
}

static ssize_t store_mc_pseudocluster_freq(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 200000 || val > 1704000)
		return -EINVAL;

	mc_2_4_freq = val;
	return count;
}

define_one_global_rw(mc_pseudocluster_freq);

static ssize_t show_screenoff_freq(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", scr_off_freq);
}

static ssize_t store_screenoff_freq(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 200000 || val > 1400000)
		return -EINVAL;

	scr_off_freq = val;
	return count;
}

define_one_global_rw(screenoff_freq);

static ssize_t show_mc_step_1(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", mc_step1);
}

static ssize_t store_mc_step_1(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 200000 || val > 1400000)
		return -EINVAL;

	mc_step1 = val;
	return count;
}

define_one_global_rw(mc_step_1);

static ssize_t show_mc_step_2(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", mc_step2);
}

static ssize_t store_mc_step_2(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 200000 || val > 1400000)
		return -EINVAL;

	mc_step2 = val;
	return count;
}

define_one_global_rw(mc_step_2);

static ssize_t show_mc_step_3(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", mc_step3);
}

static ssize_t store_mc_step_3(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 200000 || val > 1400000)
		return -EINVAL;

	mc_step3 = val;
	return count;
}

define_one_global_rw(mc_step_3);

#ifdef CONFIG_EXYNOS4_EXPORT_TEMP
static ssize_t show_temperature_factor(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", temp_factor);
}

static ssize_t store_temperature_factor(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 0 || val > 1)
		return -EINVAL;

	temp_factor = val;
	return count;
}

define_one_global_rw(temperature_factor);
#endif

static ssize_t show_multicore_eco(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", mc_eco);
}

static ssize_t store_multicore_eco(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 0 || val > 1)
		return -EINVAL;

	mc_eco = val;
	return count;
}
define_one_global_rw(multicore_eco);

static ssize_t show_mc_eco_auto(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", mc_auto);
}

static ssize_t store_mc_eco_auto(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 0 || val > 1)
		return -EINVAL;

	mc_auto = val;
	return count;
}
define_one_global_rw(mc_eco_auto);

static ssize_t show_mc_auto_factor(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", mc_eco_factor);
}

static ssize_t store_mc_auto_factor(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 2 || val > 15)
		return -EINVAL;

	mc_eco_factor = val;

	return count;
}
define_one_global_rw(mc_auto_factor);

static ssize_t show_min_cpus_online(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", min_cpus_on);
}

static ssize_t store_min_cpus_online(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 1 || val > 4)
		return -EINVAL;

	min_cpus_on = val;
	return count;
}
define_one_global_rw(min_cpus_online);

static ssize_t show_max_cpus_online(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", max_cpus_on);
}

static ssize_t store_max_cpus_online(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 1 || val > 4)
		return -EINVAL;

	max_cpus_on = val;
	return count;
}
define_one_global_rw(max_cpus_online);

static ssize_t show_screenoff_max_cpus_online(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", screenoff_max_cpus_on);
}

static ssize_t store_screenoff_max_cpus_online(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if(val < 1 || val > 4)
		return -EINVAL;

	screenoff_max_cpus_on = val;

	if(screenoff == 1)
	{
		max_cpus = max_cpus_on;
		max_cpus_on = screenoff_max_cpus_on;
	}

	return count;
}
define_one_global_rw(screenoff_max_cpus_online);

static struct attribute *pyramid_attributes[] = {
	&min_sample_time_attr.attr,
	&timer_rate_attr.attr,
	&screenoff_freq.attr,
	&screenoff_limit.attr,
	&screenoff_max_cpus_online.attr,
	&max_cpus_online.attr,
	&min_cpus_online.attr,
#ifdef CONFIG_EXYNOS4_EXPORT_TEMP
	&temperature_factor.attr,
#endif
	&multicore_eco.attr,
	&mc_step_1.attr,
	&mc_step_2.attr,
	&mc_step_3.attr,
	&mc_eco_auto.attr,
	&mc_auto_factor.attr,
	&mc_pseudocluster.attr,
	&mc_pseudocluster_freq.attr,
	NULL,
};

static struct attribute_group pyramid_attr_group = {
	.attrs = pyramid_attributes,
	.name = "pyramid",
};

static int cpufreq_governor_pyramid(struct cpufreq_policy *policy,
		unsigned int event)
{
	int rc;
	unsigned int j;
	struct cpufreq_pyramid_cpuinfo *pcpu;
	struct cpufreq_frequency_table *freq_table;

	switch (event) {
	case CPUFREQ_GOV_START:
		if (!cpu_online(policy->cpu))
			return -EINVAL;

		freq_table =
			cpufreq_frequency_get_table(policy->cpu);

		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);
			pcpu->policy = policy;
			pcpu->target_freq = policy->cur;
			pcpu->freq_table = freq_table;
			pcpu->target_set_time_in_idle =
				get_cpu_idle_time_us(j,
					     &pcpu->target_set_time);
			pcpu->floor_freq = pcpu->target_freq;
			pcpu->floor_validate_time =
				pcpu->target_set_time;
			pcpu->governor_enabled = 1;
			smp_wmb();
		}

		/*
		 * Do not register the idle hook and create sysfs
		 * entries if we have already done so.
		 */
		if (atomic_inc_return(&active_count) > 1)
			return 0;

		rc = sysfs_create_group(cpufreq_global_kobject,
				&pyramid_attr_group);
		if (rc)
			return rc;

		msm_enabled = 1;
		break;

	case CPUFREQ_GOV_STOP:
		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);
			pcpu->governor_enabled = 0;
			smp_wmb();
			del_timer_sync(&pcpu->cpu_timer);

			/*
			 * Reset idle exit time since we may cancel the timer
			 * before it can run after the last idle exit time,
			 * to avoid tripping the check in idle exit for a timer
			 * that is trying to run.
			 */
			pcpu->idle_exit_time = 0;
		}


		flush_work(&freq_scale_down_work);
		if (atomic_dec_return(&active_count) > 0)
			return 0;

		sysfs_remove_group(cpufreq_global_kobject,
				&pyramid_attr_group);

		msm_enabled = 0;
		break;

	case CPUFREQ_GOV_LIMITS:
		if (policy->max < policy->cur)
			__cpufreq_driver_target(policy,
					policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > policy->cur)
			__cpufreq_driver_target(policy,
					policy->min, CPUFREQ_RELATION_L);
		break;
	}
	return 0;
}

static int cpufreq_pyramid_idle_notifier(struct notifier_block *nb,
					     unsigned long val,
					     void *data)
{
	switch (val) {
	case IDLE_START:
		cpufreq_pyramid_idle_start();
		break;
	case IDLE_END:
		cpufreq_pyramid_idle_end();
		break;
	}

	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void pyramid_suspend(struct early_suspend *handler)
{
	screenoff = 1;
	max_cpus = max_cpus_on;
	min_cpus = min_cpus_on;
	min_cpus_on = 1;
	max_cpus_on = screenoff_max_cpus_on;
}

static void pyramid_resume(struct early_suspend *handler)
{
	screenoff = 0;
	max_cpus_on = max_cpus;
	min_cpus_on = min_cpus;
}

static struct early_suspend pyramid_early_suspend_driver = {
        .level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 10,
        .suspend = pyramid_suspend,
        .resume = pyramid_resume,
};
#endif	/* CONFIG_HAS_EARLYSUSPEND */

static struct notifier_block cpufreq_pyramid_idle_nb = {
	.notifier_call = cpufreq_pyramid_idle_notifier,
};

static int __init cpufreq_pyramid_init(void)
{
	unsigned int i;
	struct cpufreq_pyramid_cpuinfo *pcpu;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&pyramid_early_suspend_driver);
#endif

	min_sample_time = DEFAULT_MIN_SAMPLE_TIME;
	timer_rate = DEFAULT_TIMER_RATE;

	/* Initalize per-cpu timers */
	for_each_possible_cpu(i) {
		pcpu = &per_cpu(cpuinfo, i);
		init_timer(&pcpu->cpu_timer);
		pcpu->cpu_timer.function = cpufreq_pyramid_timer;
		pcpu->cpu_timer.data = i;
	}

	up_task = kthread_create(cpufreq_pyramid_up_task, NULL,
				 "kpyramid_up");
	if (IS_ERR(up_task))
		return PTR_ERR(up_task);

	sched_setscheduler_nocheck(up_task, SCHED_FIFO, &param);
	get_task_struct(up_task);

	/* No rescuer thread, bind to CPU queuing the work for possibly
	   warm cache (probably doesn't matter much). */
	down_wq = alloc_workqueue("pyramid_down", WQ_POWER_EFFICIENT, 1);

	if (!down_wq)
		goto err_freeuptask;

	INIT_WORK(&freq_scale_down_work,
		  cpufreq_pyramid_freq_down);

	spin_lock_init(&up_cpumask_lock);
	spin_lock_init(&down_cpumask_lock);
	mutex_init(&set_speed_lock);

	idle_notifier_register(&cpufreq_pyramid_idle_nb);
	return cpufreq_register_governor(&cpufreq_gov_pyramid);

err_freeuptask:
	put_task_struct(up_task);
	return -ENOMEM;
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_PYRAMID
fs_initcall(cpufreq_pyramid_init);
#else
module_init(cpufreq_pyramid_init);
#endif

static void __exit cpufreq_pyramid_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_pyramid);
	kthread_stop(up_task);
	put_task_struct(up_task);
	destroy_workqueue(down_wq);
#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&pyramid_early_suspend_driver);
#endif
}

module_exit(cpufreq_pyramid_exit);

MODULE_AUTHOR("Jaroslav Zvezda <acroreiser@gmail.com>");
MODULE_DESCRIPTION("'Pyramid' - A tunable cpufreq + hotplug governor for many workloads");
MODULE_LICENSE("GPL");
