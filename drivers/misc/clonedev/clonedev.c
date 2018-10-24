/*
 * Copyright (C) ST-Ericsson SA 2011
 *
 * Device for display cloning on external output.
 *
 * Author: Per-Daniel Olsson <per-daniel.olsson@stericsson.com>
 * for ST-Ericsson.
 *
 * License terms: GNU General Public License (GPL), version 2.
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/ioctl.h>

#include <linux/clonedev.h>

#include <linux/compdev.h>
#include <linux/mm.h>
#include <video/mcde.h>

static LIST_HEAD(dev_list);
static DEFINE_MUTEX(dev_list_lock);

struct clonedev {
	struct mutex lock;
	struct miscdevice mdev;
	struct list_head list;
	bool open;
	struct compdev *src_compdev;
	struct compdev *dst_compdev;
	bool overlay_case;
	struct compdev_size dst_size;
	struct compdev_scene_info s_info;
};

static void best_fit(struct compdev_rect *src_rect,
		struct compdev_size *dst_size,
		struct compdev_img *img)
{
	/* aspect ratio in 26.6 fixed point */
	int aspect = 1;
	int dst_w;
	int dst_h;

	if (img->rotation == COMPDEV_ROT_90_CCW ||
			img->rotation == COMPDEV_ROT_270_CCW)
		aspect = (src_rect->height << 6) / src_rect->width;
	else
		aspect = (src_rect->width << 6) / src_rect->height;

	dst_w = aspect * dst_size->height >> 6;
	dst_h = dst_size->height;
	img->dst_rect.y = 0;

	if (dst_w > dst_size->width) {
		/*
		 * Destination rectangle too wide.
		 * Clamp to image width. Keep aspect ratio.
		 */
		dst_h = (dst_size->width << 6) / aspect;
		dst_w = dst_size->width;
	}

	/* center the image */
	if (dst_w < dst_size->width) {
		int offset = (dst_size->width - dst_w) / 2;
		img->dst_rect.x = offset;
	}

	if (dst_h < dst_size->height) {
		int offset = (dst_size->height - dst_h) / 2;
		img->dst_rect.y = offset;
	}

	img->dst_rect.width = dst_w;
	img->dst_rect.height = dst_h;
}

static int clonedev_open(struct inode *inode, struct file *file)
{
	struct clonedev *cd = NULL;

	mutex_lock(&dev_list_lock);
	list_for_each_entry(cd, &dev_list, list)
		if (cd->mdev.minor == iminor(inode))
			break;

	if (&cd->list == &dev_list) {
		mutex_unlock(&dev_list_lock);
		return -ENODEV;
	}

	if (cd->open) {
		mutex_unlock(&dev_list_lock);
		return -EBUSY;
	}

	cd->open = true;

	mutex_unlock(&dev_list_lock);

	file->private_data = cd;

	return 0;
}

static int clonedev_release(struct inode *inode, struct file *file)
{
	struct clonedev *cd = NULL;

	mutex_lock(&dev_list_lock);
	list_for_each_entry(cd, &dev_list, list)
		if (cd->mdev.minor == iminor(inode))
			break;
	mutex_unlock(&dev_list_lock);

	if (&cd->list == &dev_list)
		return -ENODEV;

	cd->open = false;
	return 0;
}

static long clonedev_ioctl(struct file *file,
		unsigned int cmd,
		unsigned long arg)
{
	int ret;
	struct clonedev *cd = (struct clonedev *)file->private_data;

	mutex_lock(&cd->lock);

	switch (cmd) {
	case CLONEDEV_SET_MODE_IOC:
		/* TODO: Get the user data */

		break;

	default:
		ret = -ENOSYS;
	}

	mutex_unlock(&cd->lock);

	return ret;
}

static const struct file_operations clonedev_fops = {
	.open = clonedev_open,
	.release = clonedev_release,
	.unlocked_ioctl = clonedev_ioctl,
};

static void init_clonedev(struct clonedev *cd, const char *name)
{
	mutex_init(&cd->lock);
	INIT_LIST_HEAD(&cd->list);

	cd->mdev.minor = MISC_DYNAMIC_MINOR;
	cd->mdev.name = name;
	cd->mdev.fops = &clonedev_fops;
}

static void clonedev_post_buffer_callback(void *data,
		struct compdev_img *cb_img)
{
	struct clonedev *cd = (struct clonedev *)data;

	mutex_lock(&cd->lock);

	if (!cd->overlay_case || (cd->overlay_case &&
			(cb_img->flags & COMPDEV_OVERLAY_FLAG))) {
		struct compdev_img img;

		img = *cb_img;

		if (img.flags & COMPDEV_BYPASS_FLAG)
			img.flags &= ~COMPDEV_BYPASS_FLAG;

		if (cd->overlay_case)
			img.rotation = cd->s_info.ovly_rotation;
		else
			img.rotation = cd->s_info.fb_rotation;

		best_fit(&img.src_rect, &cd->dst_size, &img);

		compdev_post_buffer(cd->dst_compdev, &img);
	}
	mutex_unlock(&cd->lock);
}

static void clonedev_post_scene_info_callback(void *data,
		struct compdev_scene_info *s_info)
{
	struct clonedev *cd = (struct clonedev *)data;

	mutex_lock(&cd->lock);
	if (s_info->img_count > 1)
		cd->overlay_case = true;
	else
		cd->overlay_case = false;

	cd->s_info = *s_info;
	cd->s_info.img_count = 1;
	compdev_post_scene_info(cd->dst_compdev, &cd->s_info);
	mutex_unlock(&cd->lock);
}

int clonedev_create(void)
{
	int ret;
	struct clonedev *cd;

	static int counter;
	char name[10];

	cd = kzalloc(sizeof(struct clonedev), GFP_KERNEL);
	if (!cd)
		return -ENOMEM;

	snprintf(name, sizeof(name), "%s%d", CLONEDEV_DEFAULT_DEVICE_PREFIX,
			counter++);
	init_clonedev(cd, name);

	ret = misc_register(&cd->mdev);
	if (ret)
		goto fail_register_misc;
	mutex_lock(&dev_list_lock);
	list_add_tail(&cd->list, &dev_list);
	mutex_unlock(&dev_list_lock);

	mutex_lock(&cd->lock);

	compdev_get(0, &cd->src_compdev);
	compdev_get(1, &cd->dst_compdev);
	compdev_get_size(cd->dst_compdev, &cd->dst_size);

	compdev_register_listener_callbacks(cd->src_compdev, (void *)cd,
			&clonedev_post_buffer_callback,
			&clonedev_post_scene_info_callback);

	mutex_unlock(&cd->lock);
	goto out;

fail_register_misc:
	kfree(cd);
out:
	return ret;
}

void clonedev_destroy(void)
{
	struct clonedev *cd;
	struct clonedev *tmp;

	mutex_lock(&dev_list_lock);
	list_for_each_entry_safe(cd, tmp, &dev_list, list) {
		compdev_put(cd->src_compdev);
		compdev_put(cd->dst_compdev);
		compdev_deregister_callbacks(cd->src_compdev);
		list_del(&cd->list);
		misc_deregister(&cd->mdev);
		kfree(cd);
		break;
	}
	mutex_unlock(&dev_list_lock);
}

static void clonedev_destroy_all(void)
{
	struct clonedev *cd;
	struct clonedev *tmp;

	mutex_lock(&dev_list_lock);
	list_for_each_entry_safe(cd, tmp, &dev_list, list) {
		list_del(&cd->list);
		misc_deregister(&cd->mdev);
		kfree(cd);
	}
	mutex_unlock(&dev_list_lock);

	mutex_destroy(&dev_list_lock);
}

static int __init clonedev_init(void)
{
	pr_info("%s\n", __func__);

	mutex_init(&dev_list_lock);

	return 0;
}
module_init(clonedev_init);

static void __exit clonedev_exit(void)
{
	clonedev_destroy_all();
	pr_info("%s\n", __func__);
}
module_exit(clonedev_exit);

MODULE_AUTHOR("Per-Daniel Olsson <per-daniel.olsson@stericsson.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Device for display cloning on external output");

