/*
 * Author: Shilin Victor aka ChronoMonochrome <chrono.monochrome@gmail.com>
 *
 * Copyright 2015 Shilin Victor
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

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

bool god_mode_enabled __read_mostly = 1;
EXPORT_SYMBOL(god_mode_enabled);

static ssize_t god_mode_enabled_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", (god_mode_enabled ? 1 : 0));
}

static ssize_t god_mode_enabled_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int data;

	if(sscanf(buf, "%u\n", &data) == 1) {
		if (data == 1) {
			pr_err("%s: enabled\n", __FUNCTION__);
			god_mode_enabled = true;
		} else if (data == 0) {
			pr_err("%s: disabled\n", __FUNCTION__);
			god_mode_enabled = false;
		} else
			pr_err("%s: bad value: %u\n", __FUNCTION__, data);
	} else
		pr_err("%s: unknown input!\n", __FUNCTION__);

	return count;
}

static ssize_t god_mode_version_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "v1.0 (c) ChronoMonochrome\n");
}

static struct kobj_attribute god_mode_enabled_attribute = 
	__ATTR(god_mode, 0644,
		god_mode_enabled_show,
		god_mode_enabled_store);

static struct kobj_attribute god_mode_version_attribute = 
	__ATTR(god_mode_version, 0444, god_mode_version_show, NULL);

static struct attribute *god_mode_attrs[] =
{
	&god_mode_enabled_attribute.attr,
	&god_mode_version_attribute.attr,
	NULL,
};

static struct attribute_group god_mode_attr_group =
{
	.attrs = god_mode_attrs,
};

static struct kobject *god_mode_kobj;

static int god_mode_init(void)
{
	int res = 0;

	god_mode_kobj = kobject_create_and_add("god_mode", kernel_kobj);
	if (!god_mode_kobj) {
		pr_err("%s god_mode kobject create failed!\n", __FUNCTION__);
		return -ENOMEM;
        }

	res = sysfs_create_group(god_mode_kobj,
			&god_mode_attr_group);

        if (res) {
		pr_info("%s god_mode sysfs create failed!\n", __FUNCTION__);
		kobject_put(god_mode_kobj);
	}
	return res;
}

static void god_mode_exit(void)
{
	if (god_mode_kobj != NULL)
		kobject_put(god_mode_kobj);
}

module_init(god_mode_init);
module_exit(god_mode_exit);
