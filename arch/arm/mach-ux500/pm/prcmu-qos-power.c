/*
 * Copyright (C) ST-Ericsson SA 2011
 *
 * License Terms: GNU General Public License v2
 * Author: Martin Persson
 *         Per Fransson <per.xx.fransson@stericsson.com>
 *
 * Quality of Service for the U8500 PRCM Unit interface driver
 *
 * Strongly influenced by kernel/pm_qos_params.c.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/cpufreq.h>
#include <linux/mfd/dbx500-prcmu.h>
#include <linux/cpufreq-dbx500.h>

#include <mach/prcmu-debug.h>

#define ARM_THRESHOLD_FREQ (400000)

static int qos_delayed_cpufreq_notifier(struct notifier_block *,
					unsigned long, void *);

static s32 cpufreq_requirement_queued;
static s32 cpufreq_requirement_set;

/*
 * locking rule: all changes to requirements or prcmu_qos_object list
 * and prcmu_qos_objects need to happen with prcmu_qos_lock
 * held, taken with _irqsave.  One lock to rule them all
 */
struct requirement_list {
	struct list_head list;
	union {
		s32 value;
		s32 usec;
		s32 kbps;
	};
	char *name;
};

static s32 max_compare(s32 v1, s32 v2);

struct prcmu_qos_object {
	struct requirement_list requirements;
	struct blocking_notifier_head *notifiers;
	struct miscdevice prcmu_qos_power_miscdev;
	char *name;
	s32 default_value;
	s32 force_value;
	atomic_t target_value;
	s32 (*comparitor)(s32, s32);
};

static struct prcmu_qos_object null_qos;
static BLOCKING_NOTIFIER_HEAD(prcmu_ape_opp_notifier);
static BLOCKING_NOTIFIER_HEAD(prcmu_ddr_opp_notifier);

static struct prcmu_qos_object ape_opp_qos = {
	.requirements =	{
		LIST_HEAD_INIT(ape_opp_qos.requirements.list)
	},
	.notifiers = &prcmu_ape_opp_notifier,
	.name = "ape_opp",
	/* Target value in % APE OPP */
	.default_value = 50,
	.force_value = 0,
	.target_value = ATOMIC_INIT(50),
	.comparitor = max_compare
};

static struct prcmu_qos_object ddr_opp_qos = {
	.requirements =	{
		LIST_HEAD_INIT(ddr_opp_qos.requirements.list)
	},
	.notifiers = &prcmu_ddr_opp_notifier,
	.name = "ddr_opp",
	/* Target value in % DDR OPP */
	.default_value = 25,
	.force_value = 0,
	.target_value = ATOMIC_INIT(25),
	.comparitor = max_compare
};

static struct prcmu_qos_object arm_opp_qos = {
	.requirements =	{
		LIST_HEAD_INIT(arm_opp_qos.requirements.list)
	},
	/*
	 * No notifier on ARM opp qos request, since this won't actually
	 * do anything, except changing limits for cpufreq
	 */
	.name = "arm_opp",
	/* Target value in % ARM OPP, note can be 125% */
	.default_value = 25,
	.force_value = 0,
	.target_value = ATOMIC_INIT(25),
	.comparitor = max_compare
};

static struct prcmu_qos_object *prcmu_qos_array[] = {
	&null_qos,
	&ape_opp_qos,
	&ddr_opp_qos,
	&arm_opp_qos,
};

static DEFINE_MUTEX(prcmu_qos_mutex);
static DEFINE_SPINLOCK(prcmu_qos_lock);

static bool ape_opp_50_partly_25_enabled;

static unsigned long cpufreq_opp_delay = HZ / 5;

unsigned long prcmu_qos_get_cpufreq_opp_delay(void)
{
	return cpufreq_opp_delay;
}

static struct notifier_block qos_delayed_cpufreq_notifier_block = {
	.notifier_call = qos_delayed_cpufreq_notifier,
};

void prcmu_qos_set_cpufreq_opp_delay(unsigned long n)
{
	if (n == 0) {
		cpufreq_unregister_notifier(&qos_delayed_cpufreq_notifier_block,
					    CPUFREQ_TRANSITION_NOTIFIER);
		prcmu_qos_update_requirement(PRCMU_QOS_DDR_OPP, "cpufreq",
					     PRCMU_QOS_DEFAULT_VALUE);
		prcmu_qos_update_requirement(PRCMU_QOS_APE_OPP, "cpufreq",
					     PRCMU_QOS_DEFAULT_VALUE);
		cpufreq_requirement_set = PRCMU_QOS_DEFAULT_VALUE;
		cpufreq_requirement_queued = PRCMU_QOS_DEFAULT_VALUE;
	} else if (cpufreq_opp_delay != 0) {
		cpufreq_register_notifier(&qos_delayed_cpufreq_notifier_block,
					    CPUFREQ_TRANSITION_NOTIFIER);
	}
	cpufreq_opp_delay = n;
}
#ifdef CONFIG_CPU_FREQ
static void update_cpu_limits(s32 extreme_value)
{
	int cpu;
	struct cpufreq_policy policy;
	int ret;
	int min_freq, max_freq;

	for_each_online_cpu(cpu) {
		ret = cpufreq_get_policy(&policy, cpu);
		if (ret) {
			pr_err("prcmu qos: get cpufreq policy failed (cpu%d)\n",
			       cpu);
			continue;
		}

		ret = dbx500_cpufreq_get_limits(cpu, extreme_value,
					       &min_freq, &max_freq);
		if (ret)
			continue;
		/*
		 * cpufreq fw does not allow frequency change if
		 * "current min freq" > "new max freq" or
		 * "current max freq" < "new min freq".
		 * Thus the intermediate steps below.
		 */
		if (policy.min > max_freq) {
			ret = cpufreq_update_freq(cpu, min_freq, policy.max);
			if (ret)
				pr_err("prcmu qos: update min cpufreq failed (1)\n");
		}
		if (policy.max < min_freq) {
			ret = cpufreq_update_freq(cpu, policy.min, max_freq);
			if (ret)
				pr_err("prcmu qos: update max cpufreq failed (2)\n");
		}

		ret = cpufreq_update_freq(cpu, min_freq, max_freq);
		if (ret)
			pr_err("prcmu qos: update max cpufreq failed (3)\n");
	}

}
#else
static inline void update_cpu_limits(s32 extreme_value) { }
#endif
/* static helper function */
static s32 max_compare(s32 v1, s32 v2)
{
	return max(v1, v2);
}

static void update_target(int target)
{
	s32 extreme_value;
	struct requirement_list *node;
	unsigned long flags;
	bool update = false;
	u8 op;

	mutex_lock(&prcmu_qos_mutex);

	spin_lock_irqsave(&prcmu_qos_lock, flags);
	extreme_value = prcmu_qos_array[target]->default_value;

	if (prcmu_qos_array[target]->force_value != 0) {
		extreme_value = prcmu_qos_array[target]->force_value;
		update = true;
	} else {
		list_for_each_entry(node,
				    &prcmu_qos_array[target]->requirements.list,
				    list) {
			extreme_value = prcmu_qos_array[target]->comparitor(
				extreme_value, node->value);
		}
		if (atomic_read(&prcmu_qos_array[target]->target_value)
		    != extreme_value) {
			update = true;
			atomic_set(&prcmu_qos_array[target]->target_value,
				   extreme_value);
			pr_debug("prcmu qos: new target for qos %d is %d\n",
				 target, atomic_read(
					 &prcmu_qos_array[target]->target_value
					 ));
		}
	}

	spin_unlock_irqrestore(&prcmu_qos_lock, flags);

	if (!update)
		goto unlock_and_return;

	if (prcmu_qos_array[target]->notifiers)
		blocking_notifier_call_chain(prcmu_qos_array[target]->notifiers,
					     (unsigned long)extreme_value,
					     NULL);
	switch (target) {
	case PRCMU_QOS_DDR_OPP:
		switch (extreme_value) {
		case 50:
			op = DDR_50_OPP;
			pr_debug("prcmu qos: set ddr opp to 50%%\n");
			break;
		case 100:
			op = DDR_100_OPP;
			pr_debug("prcmu qos: set ddr opp to 100%%\n");
			break;
		case 25:
			/* 25% DDR OPP is not supported on 5500 */
			if (!cpu_is_u5500()) {
				op = DDR_25_OPP;
				pr_debug("prcmu qos: set ddr opp to 25%%\n");
				break;
			}
		default:
			pr_err("prcmu qos: Incorrect ddr target value (%d)",
			       extreme_value);
			goto unlock_and_return;
		}
		prcmu_set_ddr_opp(op);
		prcmu_debug_ddr_opp_log(op);
		break;
	case PRCMU_QOS_APE_OPP:
		switch (extreme_value) {
		case 50:
			if (ape_opp_50_partly_25_enabled)
				op = APE_50_PARTLY_25_OPP;
			else
				op = APE_50_OPP;
			pr_debug("prcmu qos: set ape opp to 50%%\n");
			break;
		case 100:
			op = APE_100_OPP;
			pr_debug("prcmu qos: set ape opp to 100%%\n");
			break;
		default:
			pr_err("prcmu qos: Incorrect ape target value (%d)",
			       extreme_value);
			goto unlock_and_return;
		}
		(void)prcmu_set_ape_opp(op);
		prcmu_debug_ape_opp_log(op);
		break;
	case PRCMU_QOS_ARM_OPP:
	{
		mutex_unlock(&prcmu_qos_mutex);
		/*
		 * We can't hold the mutex since changing cpufreq
		 * will trigger an prcmu fw callback.
		 */
		update_cpu_limits(extreme_value);
		/* Return since the lock is unlocked */
		return;

		break;
	}
	default:
		pr_err("prcmu qos: Incorrect target\n");
		break;
	}

unlock_and_return:
	mutex_unlock(&prcmu_qos_mutex);
}

void prcmu_qos_force_opp(int prcmu_qos_class, s32 i)
{
	prcmu_qos_array[prcmu_qos_class]->force_value = i;
	update_target(prcmu_qos_class);
}

void prcmu_qos_voice_call_override(bool enable)
{
	int ape_opp;

	mutex_lock(&prcmu_qos_mutex);

	ape_opp_50_partly_25_enabled = enable;

	ape_opp = prcmu_get_ape_opp();

	if (ape_opp == APE_50_OPP) {
		if (enable)
			prcmu_set_ape_opp(APE_50_PARTLY_25_OPP);
		else
			prcmu_set_ape_opp(APE_50_OPP);
	}

	mutex_unlock(&prcmu_qos_mutex);
}

/**
 * prcmu_qos_requirement - returns current prcmu qos expectation
 * @prcmu_qos_class: identification of which qos value is requested
 *
 * This function returns the current target value in an atomic manner.
 */
int prcmu_qos_requirement(int prcmu_qos_class)
{
	return atomic_read(&prcmu_qos_array[prcmu_qos_class]->target_value);
}
EXPORT_SYMBOL_GPL(prcmu_qos_requirement);

/**
 * prcmu_qos_add_requirement - inserts new qos request into the list
 * @prcmu_qos_class: identifies which list of qos request to us
 * @name: identifies the request
 * @value: defines the qos request
 *
 * This function inserts a new entry in the prcmu_qos_class list of requested
 * qos performance characteristics.  It recomputes the aggregate QoS
 * expectations for the prcmu_qos_class of parameters.
 */
int prcmu_qos_add_requirement(int prcmu_qos_class, char *name, s32 value)
{
	struct requirement_list *dep;
	unsigned long flags;

	dep = kzalloc(sizeof(struct requirement_list), GFP_KERNEL);
	if (dep == NULL)
		return -ENOMEM;

	if (value == PRCMU_QOS_DEFAULT_VALUE)
		dep->value = prcmu_qos_array[prcmu_qos_class]->default_value;
	else
		dep->value = value;
	dep->name = kstrdup(name, GFP_KERNEL);
	if (!dep->name)
		goto cleanup;

	spin_lock_irqsave(&prcmu_qos_lock, flags);
	list_add(&dep->list,
		 &prcmu_qos_array[prcmu_qos_class]->requirements.list);
	spin_unlock_irqrestore(&prcmu_qos_lock, flags);
	update_target(prcmu_qos_class);

	return 0;

cleanup:
	kfree(dep);
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(prcmu_qos_add_requirement);

/**
 * prcmu_qos_update_requirement - modifies an existing qos request
 * @prcmu_qos_class: identifies which list of qos request to us
 * @name: identifies the request
 * @value: defines the qos request
 *
 * Updates an existing qos requirement for the prcmu_qos_class of parameters
 * along with updating the target prcmu_qos_class value.
 *
 * If the named request isn't in the list then no change is made.
 */
int prcmu_qos_update_requirement(int prcmu_qos_class, char *name, s32 new_value)
{
	unsigned long flags;
	struct requirement_list *node;
	int pending_update = 0;

	spin_lock_irqsave(&prcmu_qos_lock, flags);
	list_for_each_entry(node,
		&prcmu_qos_array[prcmu_qos_class]->requirements.list, list) {
		if (strcmp(node->name, name) == 0) {
			if (new_value == PRCMU_QOS_DEFAULT_VALUE)
				node->value =
				prcmu_qos_array[prcmu_qos_class]->default_value;
			else
				node->value = new_value;
			pending_update = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&prcmu_qos_lock, flags);
	if (pending_update)
		update_target(prcmu_qos_class);

	return 0;
}
EXPORT_SYMBOL_GPL(prcmu_qos_update_requirement);

/**
 * prcmu_qos_remove_requirement - modifies an existing qos request
 * @prcmu_qos_class: identifies which list of qos request to us
 * @name: identifies the request
 *
 * Will remove named qos request from prcmu_qos_class list of parameters and
 * recompute the current target value for the prcmu_qos_class.
 */
void prcmu_qos_remove_requirement(int prcmu_qos_class, char *name)
{
	unsigned long flags;
	struct requirement_list *node;
	int pending_update = 0;

	spin_lock_irqsave(&prcmu_qos_lock, flags);
	list_for_each_entry(node,
		&prcmu_qos_array[prcmu_qos_class]->requirements.list, list) {
		if (strcmp(node->name, name) == 0) {
			kfree(node->name);
			list_del(&node->list);
			kfree(node);
			pending_update = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&prcmu_qos_lock, flags);
	if (pending_update)
		update_target(prcmu_qos_class);
}
EXPORT_SYMBOL_GPL(prcmu_qos_remove_requirement);

/**
 * prcmu_qos_add_notifier - sets notification entry for changes to target value
 * @prcmu_qos_class: identifies which qos target changes should be notified.
 * @notifier: notifier block managed by caller.
 *
 * will register the notifier into a notification chain that gets called
 * upon changes to the prcmu_qos_class target value.
 */
int prcmu_qos_add_notifier(int prcmu_qos_class, struct notifier_block *notifier)
{
	int retval = -EINVAL;

	if (prcmu_qos_array[prcmu_qos_class]->notifiers)
		retval = blocking_notifier_chain_register(
			prcmu_qos_array[prcmu_qos_class]->notifiers, notifier);

	return retval;
}
EXPORT_SYMBOL_GPL(prcmu_qos_add_notifier);

/**
 * prcmu_qos_remove_notifier - deletes notification entry from chain.
 * @prcmu_qos_class: identifies which qos target changes are notified.
 * @notifier: notifier block to be removed.
 *
 * will remove the notifier from the notification chain that gets called
 * upon changes to the prcmu_qos_class target value.
 */
int prcmu_qos_remove_notifier(int prcmu_qos_class,
			      struct notifier_block *notifier)
{
	int retval = -EINVAL;
	if (prcmu_qos_array[prcmu_qos_class]->notifiers)
		retval = blocking_notifier_chain_unregister(
			prcmu_qos_array[prcmu_qos_class]->notifiers, notifier);

	return retval;
}
EXPORT_SYMBOL_GPL(prcmu_qos_remove_notifier);

#define USER_QOS_NAME_LEN 32

static int prcmu_qos_power_open(struct inode *inode, struct file *filp,
				long prcmu_qos_class)
{
	int ret;
	char name[USER_QOS_NAME_LEN];

	filp->private_data = (void *)prcmu_qos_class;
	snprintf(name, USER_QOS_NAME_LEN, "file_%08x", (unsigned int)filp);
	ret = prcmu_qos_add_requirement(prcmu_qos_class, name,
					PRCMU_QOS_DEFAULT_VALUE);
	if (ret >= 0)
		return 0;

	return -EPERM;
}


static int prcmu_qos_ape_power_open(struct inode *inode, struct file *filp)
{
	return prcmu_qos_power_open(inode, filp, PRCMU_QOS_APE_OPP);
}

static int prcmu_qos_ddr_power_open(struct inode *inode, struct file *filp)
{
	return prcmu_qos_power_open(inode, filp, PRCMU_QOS_DDR_OPP);
}

static int prcmu_qos_arm_power_open(struct inode *inode, struct file *filp)
{
	return prcmu_qos_power_open(inode, filp, PRCMU_QOS_ARM_OPP);
}

static int prcmu_qos_power_release(struct inode *inode, struct file *filp)
{
	int prcmu_qos_class;
	char name[USER_QOS_NAME_LEN];

	prcmu_qos_class = (long)filp->private_data;
	snprintf(name, USER_QOS_NAME_LEN, "file_%08x", (unsigned int)filp);
	prcmu_qos_remove_requirement(prcmu_qos_class, name);

	return 0;
}

static ssize_t prcmu_qos_power_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *f_pos)
{
	s32 value;
	int prcmu_qos_class;
	char name[USER_QOS_NAME_LEN];

	prcmu_qos_class = (long)filp->private_data;
	if (count != sizeof(s32))
		return -EINVAL;
	if (copy_from_user(&value, buf, sizeof(s32)))
		return -EFAULT;
	snprintf(name, USER_QOS_NAME_LEN, "file_%08x", (unsigned int)filp);
	prcmu_qos_update_requirement(prcmu_qos_class, name, value);

	return  sizeof(s32);
}

/* Functions to provide QoS to user space */
static const struct file_operations prcmu_qos_ape_power_fops = {
	.write = prcmu_qos_power_write,
	.open = prcmu_qos_ape_power_open,
	.release = prcmu_qos_power_release,
};

/* Functions to provide QoS to user space */
static const struct file_operations prcmu_qos_ddr_power_fops = {
	.write = prcmu_qos_power_write,
	.open = prcmu_qos_ddr_power_open,
	.release = prcmu_qos_power_release,
};

static const struct file_operations prcmu_qos_arm_power_fops = {
	.write = prcmu_qos_power_write,
	.open = prcmu_qos_arm_power_open,
	.release = prcmu_qos_power_release,
};

static int register_prcmu_qos_misc(struct prcmu_qos_object *qos,
				   const struct file_operations *fops)
{
	qos->prcmu_qos_power_miscdev.minor = MISC_DYNAMIC_MINOR;
	qos->prcmu_qos_power_miscdev.name = qos->name;
	qos->prcmu_qos_power_miscdev.fops = fops;

	return misc_register(&qos->prcmu_qos_power_miscdev);
}

static void qos_delayed_work_up_fn(struct work_struct *work)
{
	prcmu_qos_update_requirement(PRCMU_QOS_DDR_OPP, "cpufreq", 100);
	prcmu_qos_update_requirement(PRCMU_QOS_APE_OPP, "cpufreq", 100);
	cpufreq_requirement_set = 100;
}

static void qos_delayed_work_down_fn(struct work_struct *work)
{
	prcmu_qos_update_requirement(PRCMU_QOS_DDR_OPP, "cpufreq",
				     PRCMU_QOS_DEFAULT_VALUE);
	prcmu_qos_update_requirement(PRCMU_QOS_APE_OPP, "cpufreq",
				     PRCMU_QOS_DEFAULT_VALUE);
	cpufreq_requirement_set = PRCMU_QOS_DEFAULT_VALUE;
}

static DECLARE_DELAYED_WORK(qos_delayed_work_up, qos_delayed_work_up_fn);
static DECLARE_DELAYED_WORK(qos_delayed_work_down, qos_delayed_work_down_fn);

static int qos_delayed_cpufreq_notifier(struct notifier_block *nb,
			unsigned long event, void *data)
{
	struct cpufreq_freqs *freq = data;
	s32 new_ddr_target;

	/* Only react once per transition and only for one core, e.g. core 0 */
	if (event != CPUFREQ_POSTCHANGE || freq->cpu != 0)
		return 0;

	/*
	 * APE and DDR OPP are always handled together in this solution.
	 * Hence no need to check both DDR and APE opp in the code below.
	 */

	/* Which DDR OPP are we aiming for? */
	if (freq->new > ARM_THRESHOLD_FREQ)
		new_ddr_target = 100;
	else
		new_ddr_target = PRCMU_QOS_DEFAULT_VALUE;

	if (new_ddr_target == cpufreq_requirement_queued) {
		/*
		 * We're already at, or going to, the target requirement.
		 * This is only a fluctuation within the interval
		 * corresponding to the same DDR requirement.
		 */
		return 0;
	}
	cpufreq_requirement_queued = new_ddr_target;

	if (freq->new > ARM_THRESHOLD_FREQ) {
		cancel_delayed_work_sync(&qos_delayed_work_down);
		/*
		 * Only schedule this requirement if it is not the current
		 * one.
		 */
		if (new_ddr_target != cpufreq_requirement_set)
			schedule_delayed_work(&qos_delayed_work_up,
					      cpufreq_opp_delay);
	} else {
		cancel_delayed_work_sync(&qos_delayed_work_up);
		/*
		 * Only schedule this requirement if it is not the current
		 * one.
		 */
		if (new_ddr_target != cpufreq_requirement_set)
			schedule_delayed_work(&qos_delayed_work_down,
					      cpufreq_opp_delay);
	}

	return 0;
}

static int __init prcmu_qos_power_init(void)
{
	int ret;

	/* 25% DDR OPP is not supported on u5500 */
	if (cpu_is_u5500()) {
		ddr_opp_qos.default_value = 50;
		atomic_set(&ddr_opp_qos.target_value, 50);
	}

	ret = register_prcmu_qos_misc(&ape_opp_qos, &prcmu_qos_ape_power_fops);
	if (ret < 0) {
		pr_err("prcmu ape qos: setup failed\n");
		return ret;
	}

	ret = register_prcmu_qos_misc(&ddr_opp_qos, &prcmu_qos_ddr_power_fops);
	if (ret < 0) {
		pr_err("prcmu ddr qos: setup failed\n");
		return ret;
	}

	ret = register_prcmu_qos_misc(&arm_opp_qos, &prcmu_qos_arm_power_fops);
	if (ret < 0) {
		pr_err("prcmu arm qos: setup failed\n");
		return ret;
	}

	prcmu_qos_add_requirement(PRCMU_QOS_DDR_OPP, "cpufreq",
				  PRCMU_QOS_DEFAULT_VALUE);
	prcmu_qos_add_requirement(PRCMU_QOS_APE_OPP, "cpufreq",
				  PRCMU_QOS_DEFAULT_VALUE);
	cpufreq_requirement_set = PRCMU_QOS_DEFAULT_VALUE;
	cpufreq_requirement_queued = PRCMU_QOS_DEFAULT_VALUE;

	cpufreq_register_notifier(&qos_delayed_cpufreq_notifier_block,
				  CPUFREQ_TRANSITION_NOTIFIER);

	return ret;
}

late_initcall(prcmu_qos_power_init);
