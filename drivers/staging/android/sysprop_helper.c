/* sysprop_helper.c
**
** Android helper to retrieve system sdk version from build.prop file
**
** Copyright (C) 2019
**
** Shilin Victor <chrono.monochrome@gmail.com>
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
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

//#define DEBUG

#define BUF_SIZE 512
#define ANDROID_BUILD_PROP_FILE "/system/build.prop"
#define ANDROID_SDK_PROP "ro.build.version.sdk"

static long get_android_property_val(char *buf, char *prop) {
	char *orig_buf = buf;
	char *token;
	char *keyval, *tmp;
	int pos = 0;

	while ((token = strsep(&buf, "\n")) != NULL) {
		pos = token - orig_buf;

		keyval = token;
		// retrieve property name
		tmp = strsep(&keyval, "=");
		if (!tmp || !strstr(tmp, prop))
			continue;

		// retrieve property val
		tmp = strsep(&keyval, "=");

		return simple_strtol(tmp, NULL, 10);
	}

	return -ENOENT;
}

static int __get_android_sdk_version_from_buf(char *buf) {
	return (int)get_android_property_val(buf, ANDROID_SDK_PROP);
}

static int __get_android_sdk_version_from_file(char *filename) {
	struct file *f;
	char *buf = NULL;
	mm_segment_t fs;
	int sdk;

	buf = kzalloc(BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	f = filp_open(filename, O_RDONLY, 0);
	if (IS_ERR(f))
		return -PTR_ERR(f);

	if (!f)
		return -EFAULT;

	// Get current segment descriptor
	fs = get_fs();
	// Set segment descriptor associated to kernel space
	set_fs(get_ds());

	f->f_op->read(f, buf, BUF_SIZE - 1, &f->f_pos);
	// Restore segment descriptor
	set_fs(fs);

	sdk = __get_android_sdk_version_from_buf(buf);

	filp_close(f, NULL);
	kfree(buf);
	return sdk;
}

int get_android_sdk_version(void) {
	return __get_android_sdk_version_from_file(ANDROID_BUILD_PROP_FILE);
}
EXPORT_SYMBOL(get_android_sdk_version);

#ifdef DEBUG
int sysprop_helper_init(void) {
	pr_err("%s: detected %d sdk version\n", __func__, get_android_sdk_version());
	return 0;
}

void sysprop_helper_exit(void) {
}

module_init(sysprop_helper_init);
module_exit(sysprop_helper_exit);
#endif

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Shilin Victor <chrono.monochrome@gmail.com>");
MODULE_DESCRIPTION("Android helper to retrieve system sdk version from build.prop file");
