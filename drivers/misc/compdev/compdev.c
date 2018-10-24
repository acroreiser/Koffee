/*
 * Copyright (C) ST-Ericsson SA 2011
 *
 * Display overlay compositer device driver
 *
 * Author: Anders Bauer <anders.bauer@stericsson.com>
 * for ST-Ericsson.
 *
 * Modified: Per-Daniel Olsson <per-daniel.olsson@stericsson.com>
 * for ST-Ericsson.
 *
 * License terms: GNU General Public License (GPL), version 2.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/ioctl.h>
#include <linux/sched.h>

#include <linux/compdev.h>
#include <linux/hwmem.h>
#include <linux/mm.h>
#include <video/mcde_dss.h>
#include <video/b2r2_blt.h>
#include <linux/workqueue.h>
#include <linux/completion.h>

#define BUFFER_CACHE_DEPTH 2
#define NUM_COMPDEV_BUFS 2

static LIST_HEAD(dev_list);
static DEFINE_MUTEX(dev_list_lock);
static int dev_counter;

struct compdev_buffer {
	struct hwmem_alloc *alloc;
	enum compdev_ptr_type type;
	u32 size;
	u32 paddr; /* if pinned */
};

struct compdev_img_internal {
	struct compdev_img img;
	u32 ref_count;
};

struct compdev_blt_work {
	struct work_struct work;
	struct compdev_img *src_img;
	struct compdev_img_internal *dst_img;
	int blt_handle;
	bool mcde_rotation;
	struct device *dev;
};

struct compdev_post_callback_work {
	struct work_struct work;
	struct compdev_img *img;
	post_buffer_callback pb_cb;
	void *cb_data;
	struct device *dev;
};

struct buffer_cache_context {
	struct compdev_img_internal
		*img[BUFFER_CACHE_DEPTH];
	u8 index;
	u8 unused_counter;
	struct device *dev;
};

struct dss_context {
	struct device *dev;
	struct mcde_display_device *ddev;
	struct mcde_overlay *ovly[NUM_COMPDEV_BUFS];
	struct compdev_buffer ovly_buffer[NUM_COMPDEV_BUFS];
	struct compdev_size phy_size;
	enum mcde_display_rotation display_rotation;
	enum compdev_rotation current_buffer_rotation;
	int blt_handle;
	u8 temp_img_count;
	struct compdev_img_internal *temp_img[NUM_COMPDEV_BUFS];
	struct buffer_cache_context cache_ctx;
};

struct compdev {
	struct mutex lock;
	struct miscdevice mdev;
	struct device *dev;
	struct list_head list;
	struct dss_context dss_ctx;
	u16 ref_count;
	struct workqueue_struct *worker_thread;
	int dev_index;
	post_buffer_callback pb_cb;
	post_scene_info_callback si_cb;
	struct compdev_scene_info s_info;
	u8 sync_count;
	u8 image_count;
	struct compdev_img *images[NUM_COMPDEV_BUFS];
	struct completion fence;
	void *cb_data;
	bool mcde_rotation;
};

static struct compdev *compdevs[MAX_NBR_OF_COMPDEVS];

static int compdev_post_buffers_dss(struct dss_context *dss_ctx,
		struct compdev_img *img1, struct compdev_img *img2);


static int compdev_open(struct inode *inode, struct file *file)
{
	struct compdev *cd = NULL;

	mutex_lock(&dev_list_lock);
	list_for_each_entry(cd, &dev_list, list)
		if (cd->mdev.minor == iminor(inode))
			break;

	if (&cd->list == &dev_list) {
		mutex_unlock(&dev_list_lock);
		return -ENODEV;
	}
	mutex_unlock(&dev_list_lock);
	file->private_data = cd;
	return 0;
}

static int disable_overlay(struct mcde_overlay *ovly)
{
	struct mcde_overlay_info info;

	mcde_dss_get_overlay_info(ovly, &info);
	if (info.paddr != 0) {
		/* Set the pointer to zero to disable the overlay */
		info.paddr = 0;
		mcde_dss_apply_overlay(ovly, &info);
	}
	return 0;
}

static int compdev_release(struct inode *inode, struct file *file)
{
	struct compdev *cd = NULL;
	int i;

	mutex_lock(&dev_list_lock);
	list_for_each_entry(cd, &dev_list, list)
		if (cd->mdev.minor == iminor(inode))
			break;
	mutex_unlock(&dev_list_lock);

	if (&cd->list == &dev_list)
		return -ENODEV;

	for (i = 0; i < NUM_COMPDEV_BUFS; i++) {
		disable_overlay(cd->dss_ctx.ovly[i]);
		if (cd->dss_ctx.ovly_buffer[i].paddr &&
				cd->dss_ctx.ovly_buffer[i].type ==
				COMPDEV_PTR_HWMEM_BUF_NAME_OFFSET)
			hwmem_unpin(cd->dss_ctx.ovly_buffer[i].alloc);

		cd->dss_ctx.ovly_buffer[i].alloc = NULL;
		cd->dss_ctx.ovly_buffer[i].size = 0;
		cd->dss_ctx.ovly_buffer[i].paddr = 0;
	}

	return 0;
}

static enum mcde_ovly_pix_fmt get_ovly_fmt(enum compdev_fmt fmt)
{
	switch (fmt) {
	default:
	case COMPDEV_FMT_RGB565:
		return MCDE_OVLYPIXFMT_RGB565;
	case COMPDEV_FMT_RGB888:
		return MCDE_OVLYPIXFMT_RGB888;
	case COMPDEV_FMT_RGBA8888:
		return MCDE_OVLYPIXFMT_RGBA8888;
	case COMPDEV_FMT_RGBX8888:
		return MCDE_OVLYPIXFMT_RGBX8888;
	case COMPDEV_FMT_YUV422:
		return MCDE_OVLYPIXFMT_YCbCr422;
	}
}

static int compdev_setup_ovly(struct compdev_img *img,
		struct compdev_buffer *buffer,
		struct mcde_overlay *ovly,
		int z_order,
		struct dss_context *dss_ctx)
{
	int ret = 0;
	enum hwmem_mem_type memtype;
	enum hwmem_access access;
	struct hwmem_mem_chunk mem_chunk;
	size_t mem_chunk_length = 1;
	struct hwmem_region rgn = { .offset = 0, .count = 1, .start = 0 };
	struct mcde_overlay_info info;

	if (img->buf.type == COMPDEV_PTR_HWMEM_BUF_NAME_OFFSET) {
		buffer->type = COMPDEV_PTR_HWMEM_BUF_NAME_OFFSET;
		buffer->alloc = hwmem_resolve_by_name(img->buf.hwmem_buf_name);
		if (IS_ERR(buffer->alloc)) {
			ret = PTR_ERR(buffer->alloc);
			dev_warn(dss_ctx->dev,
				"HWMEM resolve failed, %d\n", ret);
			goto resolve_failed;
		}

		hwmem_get_info(buffer->alloc, &buffer->size, &memtype,
				&access);

		if (!(access & HWMEM_ACCESS_READ) ||
				memtype != HWMEM_MEM_CONTIGUOUS_SYS) {
			ret = -EACCES;
			dev_warn(dss_ctx->dev,
				"Invalid_mem overlay, %d\n", ret);
			goto invalid_mem;
		}
		ret = hwmem_pin(buffer->alloc, &mem_chunk, &mem_chunk_length);
		if (ret) {
			dev_warn(dss_ctx->dev,
				"Pin failed, %d\n", ret);
			goto pin_failed;
		}

		rgn.size = rgn.end = buffer->size;
		ret = hwmem_set_domain(buffer->alloc, HWMEM_ACCESS_READ,
			HWMEM_DOMAIN_SYNC, &rgn);
		if (ret)
			dev_warn(dss_ctx->dev,
				"Set domain failed, %d\n", ret);

		buffer->paddr = mem_chunk.paddr;
	} else if (img->buf.type == COMPDEV_PTR_PHYSICAL) {
		buffer->type = COMPDEV_PTR_PHYSICAL;
		buffer->alloc = NULL;
		buffer->size = img->buf.len;
		buffer->paddr = img->buf.offset;
	}

	info.stride = img->pitch;
	info.fmt = get_ovly_fmt(img->fmt);
	info.src_x = 0;
	info.src_y = 0;
	info.dst_x = img->dst_rect.x;
	info.dst_y = img->dst_rect.y;
	info.dst_z = z_order;
	info.w = img->dst_rect.width;
	info.h = img->dst_rect.height;
	info.dirty.x = 0;
	info.dirty.y = 0;
	info.dirty.w = img->dst_rect.width;
	info.dirty.h = img->dst_rect.height;
	info.paddr = buffer->paddr;

	mcde_dss_apply_overlay(ovly, &info);
	return ret;

pin_failed:
invalid_mem:
	buffer->alloc = NULL;
	buffer->size = 0;
	buffer->paddr = 0;

resolve_failed:
	return ret;
}

static int compdev_update_rotation(struct dss_context *dss_ctx,
		enum compdev_rotation rotation)
{
	/* Set video mode */
	struct mcde_video_mode vmode;
	int ret = 0;

	memset(&vmode, 0, sizeof(struct mcde_video_mode));
	mcde_dss_get_video_mode(dss_ctx->ddev, &vmode);
	if ((dss_ctx->display_rotation + rotation) % 180) {
		vmode.xres = dss_ctx->phy_size.height;
		vmode.yres = dss_ctx->phy_size.width;
	} else {
		vmode.xres = dss_ctx->phy_size.width;
		vmode.yres = dss_ctx->phy_size.height;
	}

	/* Set rotation */
	ret = mcde_dss_set_rotation(dss_ctx->ddev,
			(dss_ctx->display_rotation + rotation) % 360);
	if (ret != 0)
		goto exit;

	ret = mcde_dss_set_video_mode(dss_ctx->ddev, &vmode);
	if (ret != 0)
		goto exit;


	/* Apply */
	ret = mcde_dss_apply_channel(dss_ctx->ddev);
exit:
	return ret;
}

static int release_prev_frame(struct dss_context *dss_ctx)
{
	int ret = 0;
	int i;

	/* Handle unpin of previous buffers */
	for (i = 0; i < NUM_COMPDEV_BUFS; i++) {
		if (dss_ctx->ovly_buffer[i].type ==
				COMPDEV_PTR_HWMEM_BUF_NAME_OFFSET &&
				dss_ctx->ovly_buffer[i].paddr != 0) {
			hwmem_unpin(dss_ctx->ovly_buffer[i].alloc);
			hwmem_release(dss_ctx->ovly_buffer[i].alloc);
		}
		dss_ctx->ovly_buffer[i].alloc = NULL;
		dss_ctx->ovly_buffer[i].size = 0;
		dss_ctx->ovly_buffer[i].paddr = 0;
	}
	return ret;

}

static enum b2r2_blt_fmt compdev_to_blt_format(enum compdev_fmt fmt)
{
	switch (fmt) {
	case COMPDEV_FMT_RGBA8888:
		return B2R2_BLT_FMT_32_BIT_ABGR8888;
	case COMPDEV_FMT_RGB888:
		return B2R2_BLT_FMT_24_BIT_RGB888;
	case COMPDEV_FMT_RGB565:
		return B2R2_BLT_FMT_16_BIT_RGB565;
	case COMPDEV_FMT_YUV422:
		return B2R2_BLT_FMT_CB_Y_CR_Y;
	case COMPDEV_FMT_YCBCR42XMBN:
		return B2R2_BLT_FMT_YUV420_PACKED_SEMIPLANAR_MB_STE;
	case COMPDEV_FMT_YUV420_SP:
		return B2R2_BLT_FMT_YUV420_PACKED_SEMI_PLANAR;
	case COMPDEV_FMT_YVU420_SP:
		return B2R2_BLT_FMT_YVU420_PACKED_SEMI_PLANAR;
	case COMPDEV_FMT_YUV420_P:
		return B2R2_BLT_FMT_YUV420_PACKED_PLANAR;
	default:
		return B2R2_BLT_FMT_UNUSED;
	}
}

static enum b2r2_blt_transform to_blt_transform
					(enum compdev_rotation compdev_rot)
{
	switch (compdev_rot) {
	case COMPDEV_ROT_0:
		return B2R2_BLT_TRANSFORM_NONE;
	case COMPDEV_ROT_90_CCW:
		return B2R2_BLT_TRANSFORM_CCW_ROT_90;
	case COMPDEV_ROT_180:
		return B2R2_BLT_TRANSFORM_CCW_ROT_180;
	case COMPDEV_ROT_270_CCW:
		return B2R2_BLT_TRANSFORM_CCW_ROT_90;
	default:
		return B2R2_BLT_TRANSFORM_NONE;
	}
}

static u32 get_stride(u32 width, enum compdev_fmt fmt)
{
	u32 stride = 0;
	switch (fmt) {
	case COMPDEV_FMT_RGB565:
		stride = width * 2;
		break;
	case COMPDEV_FMT_RGB888:
		stride = width * 3;
		break;
	case COMPDEV_FMT_RGBX8888:
		stride = width * 4;
		break;
	case COMPDEV_FMT_RGBA8888:
		stride = width * 4;
		break;
	case COMPDEV_FMT_YUV422:
		stride = width * 2;
		break;
	case COMPDEV_FMT_YCBCR42XMBN:
	case COMPDEV_FMT_YUV420_SP:
	case COMPDEV_FMT_YVU420_SP:
	case COMPDEV_FMT_YUV420_P:
		stride = width;
		break;
	}

	/* The display controller requires 8 byte aligned strides */
	if (stride % 8)
		stride += 8 - (stride % 8);

	return stride;
}

static int alloc_comp_internal_img(enum compdev_fmt fmt,
		u16 width, u16 height, struct compdev_img_internal **img_pp)
{
	struct hwmem_alloc *alloc;
	int name;
	u32 size;
	u32 stride;
	struct compdev_img_internal *img;

	stride = get_stride(width, fmt);
	size = stride * height;
	size = PAGE_ALIGN(size);

	img = kzalloc(sizeof(struct compdev_img_internal), GFP_KERNEL);

	if (!img)
		return -ENOMEM;

	alloc = hwmem_alloc(size, HWMEM_ALLOC_HINT_WRITE_COMBINE |
			HWMEM_ALLOC_HINT_UNCACHED,
			(HWMEM_ACCESS_READ  | HWMEM_ACCESS_WRITE |
			HWMEM_ACCESS_IMPORT),
			HWMEM_MEM_CONTIGUOUS_SYS);

	if (IS_ERR(alloc)) {
		kfree(img);
		img = NULL;
		return PTR_ERR(alloc);
	}

	name = hwmem_get_name(alloc);
	if (name < 0) {
		kfree(img);
		img = NULL;
		hwmem_release(alloc);
		return name;
	}

	img->img.height = height;
	img->img.width = width;
	img->img.fmt = fmt;
	img->img.pitch = stride;
	img->img.buf.hwmem_buf_name = name;
	img->img.buf.type = COMPDEV_PTR_HWMEM_BUF_NAME_OFFSET;
	img->img.buf.offset = 0;
	img->img.buf.len = size;

	img->ref_count = 1;

	*img_pp = img;

	return 0;
}

static void free_comp_img_buf(struct compdev_img_internal *img,
		struct device *dev)
{
	dev_dbg(dev, "%s\n", __func__);

	if (img != NULL && img->ref_count) {
		img->ref_count--;
		if (img->ref_count == 0) {
			struct hwmem_alloc *alloc;
			if (img->img.buf.hwmem_buf_name > 0) {
				alloc = hwmem_resolve_by_name(
						img->img.buf.hwmem_buf_name);
				if (IS_ERR(alloc)) {
					dev_err(dev, "%s: Error getting Alloc "
						"from HWMEM\n", __func__);
					return;
				}
				/* Double release needed */
				hwmem_release(alloc);
				hwmem_release(alloc);
			}
			kfree(img);
		}
	}
}

struct compdev_img_internal *compdev_buffer_cache_get_image(
		struct buffer_cache_context *cache_ctx, enum compdev_fmt fmt,
		u16 width, u16 height)
{
	int i;
	struct compdev_img_internal *img = NULL;

	dev_dbg(cache_ctx->dev, "%s\n", __func__);

	/* First check for a cache hit */
	if (cache_ctx->unused_counter > 0) {
		u8 active_index = cache_ctx->index;
		struct compdev_img_internal *temp =
				cache_ctx->img[active_index];
		if (temp != NULL && temp->img.fmt == fmt &&
				temp->img.width == width &&
				temp->img.height == height) {
			img = temp;
			cache_ctx->unused_counter = 0;
		}
	}
	/* Check if there was a cache hit */
	if (img == NULL) {
		/* Create new buffers and release old */
		for (i = 0; i < BUFFER_CACHE_DEPTH; i++) {
			if (cache_ctx->img[i]) {
				free_comp_img_buf(cache_ctx->img[i],
						cache_ctx->dev);
				cache_ctx->img[i] = NULL;
			}
			cache_ctx->index = 0;
			if (alloc_comp_internal_img(fmt, width, height,
					&cache_ctx->img[i]))
				dev_err(cache_ctx->dev,
						"%s: Allocation error\n",
						__func__);
		}
		img = cache_ctx->img[0];
	}

	if (img != NULL) {
		img->ref_count++;
		cache_ctx->unused_counter = 0;
		cache_ctx->index++;
		if (cache_ctx->index >= BUFFER_CACHE_DEPTH)
			cache_ctx->index = 0;
	}

	return img;
}

static void compdev_buffer_cache_mark_frame
				(struct buffer_cache_context *cache_ctx)
{
	if (cache_ctx->unused_counter < 2)
		cache_ctx->unused_counter++;
	if (cache_ctx->unused_counter == 2) {
		int i;
		for (i = 0; i < BUFFER_CACHE_DEPTH; i++) {
			if (cache_ctx->img[i]) {
				free_comp_img_buf(cache_ctx->img[i],
						cache_ctx->dev);
				cache_ctx->img[i] = NULL;
			}
		}
	}
}

static bool check_hw_format(enum compdev_fmt fmt)
{
	if (fmt == COMPDEV_FMT_RGB565 ||
			fmt == COMPDEV_FMT_RGB888 ||
			fmt == COMPDEV_FMT_RGBA8888 ||
			fmt == COMPDEV_FMT_RGBX8888 ||
			fmt == COMPDEV_FMT_YUV422)
		return true;
	else
		return false;
}

static enum compdev_fmt find_compatible_fmt(enum compdev_fmt fmt, bool rotation)
{
	if (!rotation) {
		switch (fmt) {
		case COMPDEV_FMT_RGB565:
		case COMPDEV_FMT_RGB888:
		case COMPDEV_FMT_RGBA8888:
		case COMPDEV_FMT_RGBX8888:
			return fmt;
		case COMPDEV_FMT_YUV422:
		case COMPDEV_FMT_YCBCR42XMBN:
		case COMPDEV_FMT_YUV420_SP:
		case COMPDEV_FMT_YVU420_SP:
		case COMPDEV_FMT_YUV420_P:
			return COMPDEV_FMT_YUV422;
		default:
			return COMPDEV_FMT_RGBA8888;
		}
	} else {
		switch (fmt) {
		case COMPDEV_FMT_RGB565:
		case COMPDEV_FMT_RGB888:
		case COMPDEV_FMT_RGBA8888:
		case COMPDEV_FMT_RGBX8888:
			return fmt;
		case COMPDEV_FMT_YUV422:
		case COMPDEV_FMT_YCBCR42XMBN:
		case COMPDEV_FMT_YUV420_SP:
		case COMPDEV_FMT_YVU420_SP:
		case COMPDEV_FMT_YUV420_P:
			return COMPDEV_FMT_RGB888;
		default:
			return COMPDEV_FMT_RGBA8888;
		}
	}
}

static void compdev_callback_worker_function(struct work_struct *work)
{
	struct compdev_post_callback_work *cb_work =
			(struct compdev_post_callback_work *)work;

	if (cb_work->pb_cb != NULL)
		cb_work->pb_cb(cb_work->cb_data, cb_work->img);
}
static void compdev_blt_worker_function(struct work_struct *work)
{
	struct compdev_blt_work *blt_work = (struct compdev_blt_work *)work;
	struct compdev_img *src_img;
	struct compdev_img *dst_img;
	struct b2r2_blt_req req;
	int req_id;

	dev_dbg(blt_work->dev, "%s\n", __func__);

	src_img = blt_work->src_img;
	dst_img = &blt_work->dst_img->img;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);

	if (src_img->buf.type == COMPDEV_PTR_PHYSICAL) {
		req.src_img.buf.type = B2R2_BLT_PTR_PHYSICAL;
		req.src_img.buf.fd = src_img->buf.fd;
	} else {
		struct hwmem_alloc *alloc;

		req.src_img.buf.type = B2R2_BLT_PTR_HWMEM_BUF_NAME_OFFSET;
		req.src_img.buf.hwmem_buf_name = src_img->buf.hwmem_buf_name;

		alloc = hwmem_resolve_by_name(src_img->buf.hwmem_buf_name);
		if (IS_ERR(alloc)) {
			dev_warn(blt_work->dev,
				"HWMEM resolve failed\n");
		}
		hwmem_set_access(alloc,
				HWMEM_ACCESS_READ | HWMEM_ACCESS_IMPORT,
				task_tgid_nr(current));
		hwmem_release(alloc);
	}
	req.src_img.pitch = src_img->pitch;
	req.src_img.buf.offset = src_img->buf.offset;
	req.src_img.buf.len = src_img->buf.len;
	req.src_img.fmt = compdev_to_blt_format(src_img->fmt);
	req.src_img.width = src_img->width;
	req.src_img.height = src_img->height;

	req.src_rect.x = src_img->src_rect.x;
	req.src_rect.y = src_img->src_rect.y;
	req.src_rect.width = src_img->src_rect.width;
	req.src_rect.height = src_img->src_rect.height;

	if (dst_img->buf.type == COMPDEV_PTR_PHYSICAL) {
		req.dst_img.buf.type = B2R2_BLT_PTR_PHYSICAL;
		req.dst_img.buf.fd = dst_img->buf.fd;
	} else {
		req.dst_img.buf.type = B2R2_BLT_PTR_HWMEM_BUF_NAME_OFFSET;
		req.dst_img.buf.hwmem_buf_name = dst_img->buf.hwmem_buf_name;
	}
	req.dst_img.pitch = dst_img->pitch;
	req.dst_img.buf.offset = dst_img->buf.offset;
	req.dst_img.buf.len = dst_img->buf.len;
	req.dst_img.fmt = compdev_to_blt_format(dst_img->fmt);
	req.dst_img.width = dst_img->width;
	req.dst_img.height = dst_img->height;

	if (blt_work->mcde_rotation)
		req.transform = B2R2_BLT_TRANSFORM_NONE;
	else
		req.transform = to_blt_transform(src_img->rotation);
	req.dst_rect.x = 0;
	req.dst_rect.y = 0;
	req.dst_rect.width = src_img->dst_rect.width;
	req.dst_rect.height = src_img->dst_rect.height;

	req.global_alpha = 0xff;
	req.flags = B2R2_BLT_FLAG_DITHER;

	req_id = b2r2_blt_request(blt_work->blt_handle, &req);

	if (b2r2_blt_synch(blt_work->blt_handle, req_id) < 0) {
		dev_err(blt_work->dev,
				"%s: Could not perform b2r2_blt_synch",
				__func__);
	}

	dst_img->src_rect.x = 0;
	dst_img->src_rect.x = 0;
	dst_img->src_rect.width = dst_img->width;
	dst_img->src_rect.height = dst_img->height;

	dst_img->dst_rect.x = src_img->dst_rect.x;
	dst_img->dst_rect.y = src_img->dst_rect.y;
	dst_img->dst_rect.width = src_img->dst_rect.width;
	dst_img->dst_rect.height = src_img->dst_rect.height;

	dst_img->rotation = src_img->rotation;
}

static int compdev_post_buffer_locked(struct compdev *cd,
		struct compdev_img *src_img)
{
	int ret = 0;
	int i;
	bool transform_needed = false;
	struct compdev_img *resulting_img;
	struct compdev_blt_work blt_work;
	struct compdev_post_callback_work cb_work;
	bool callback_work = false;
	bool bypass_case = false;

	dev_dbg(cd->dev, "%s\n", __func__);

	/* Free potential temp buffers */
	for (i = 0; i < cd->dss_ctx.temp_img_count; i++)
		free_comp_img_buf(cd->dss_ctx.temp_img[i], cd->dev);
	cd->dss_ctx.temp_img_count = 0;

	/* Check for bypass images */
	if (src_img->flags & COMPDEV_BYPASS_FLAG)
		bypass_case = true;

	/* Handle callback */
	if (cd->pb_cb != NULL) {
		callback_work = true;
		INIT_WORK((struct work_struct *)&cb_work,
				compdev_callback_worker_function);
		cb_work.img = src_img;
		cb_work.pb_cb = cd->pb_cb;
		cb_work.cb_data = cd->cb_data;
		cb_work.dev = cd->dev;
		queue_work(cd->worker_thread, (struct work_struct *)&cb_work);
	}

	if (!bypass_case) {
		/* Determine if transform is needed */
		/* First check scaling */
		if ((src_img->rotation == COMPDEV_ROT_0 ||
			src_img->rotation == COMPDEV_ROT_180) &&
			(src_img->src_rect.width != src_img->dst_rect.width ||
			src_img->src_rect.height != src_img->dst_rect.height))
			transform_needed = true;
		else if ((src_img->rotation == COMPDEV_ROT_90_CCW ||
			src_img->rotation == COMPDEV_ROT_270_CCW) &&
			(src_img->src_rect.width != src_img->dst_rect.height ||
			src_img->src_rect.height != src_img->dst_rect.width))
			transform_needed = true;

		if (!transform_needed && check_hw_format(src_img->fmt) == false)
			transform_needed = true;

		if (transform_needed) {
			u16 width = 0;
			u16 height = 0;
			enum compdev_fmt fmt;

			INIT_WORK((struct work_struct *)&blt_work,
					compdev_blt_worker_function);

			if (cd->dss_ctx.blt_handle == 0) {
				dev_dbg(cd->dev, "%s: B2R2 opened\n", __func__);
				cd->dss_ctx.blt_handle = b2r2_blt_open();
				if (cd->dss_ctx.blt_handle < 0) {
					dev_warn(cd->dev,
						"%s(%d): Failed to "
						"open b2r2 device\n",
						__func__, __LINE__);
				}
			}
			blt_work.blt_handle = cd->dss_ctx.blt_handle;
			blt_work.src_img = src_img;
			blt_work.mcde_rotation = cd->mcde_rotation;

			width = src_img->dst_rect.width;
			height = src_img->dst_rect.height;

			fmt = find_compatible_fmt(src_img->fmt,
					(!cd->mcde_rotation) &&
					(src_img->rotation != COMPDEV_ROT_0));

			blt_work.dst_img = compdev_buffer_cache_get_image
					(&cd->dss_ctx.cache_ctx,
							fmt, width, height);

			blt_work.dst_img->img.flags = src_img->flags;
			blt_work.dev = cd->dev;

			queue_work(cd->worker_thread,
					(struct work_struct *)&blt_work);
			flush_work_sync((struct work_struct *)&blt_work);

			resulting_img = &blt_work.dst_img->img;

			cd->dss_ctx.temp_img[cd->dss_ctx.temp_img_count] =
					blt_work.dst_img;
			cd->dss_ctx.temp_img_count++;

		} else {
		    resulting_img = src_img;
		}

		if (!cd->mcde_rotation)
			resulting_img->rotation = COMPDEV_ROT_0;

		cd->images[cd->image_count] = resulting_img;
		cd->image_count++;

		/* make sure that a potential callback has returned */
		if (callback_work)
			flush_work_sync((struct work_struct *)&cb_work);

		if (cd->sync_count > 1) {
			cd->sync_count--;
			mutex_unlock(&cd->lock);
			/* Wait for fence */
			wait_for_completion(&cd->fence);
			mutex_lock(&cd->lock);
		} else {
			struct compdev_img *img1 = NULL;
			struct compdev_img *img2 = NULL;

			if (cd->sync_count)
				cd->sync_count--;

			img1 = cd->images[0];
			if (cd->image_count)
				img2 = cd->images[1];

			/* Do the refresh */
			compdev_post_buffers_dss(&cd->dss_ctx, img1, img2);
			compdev_buffer_cache_mark_frame
						(&cd->dss_ctx.cache_ctx);

			if (cd->s_info.img_count > 1) {
				/* Releasing fence */
				complete(&cd->fence);
			}

			cd->sync_count = 0;
			cd->image_count = 0;
			cd->images[0] = NULL;
			cd->images[1] = NULL;
		}
	} else {
		/* make sure that a potential callback has returned */
		if (callback_work)
			flush_work_sync((struct work_struct *)&cb_work);
	}

	return ret;
}

static int compdev_post_buffers_dss(struct dss_context *dss_ctx,
		struct compdev_img *img1, struct compdev_img *img2)
{
	int ret = 0;
	int i = 0;

	struct compdev_img *fb_img = NULL;
	struct compdev_img *ovly_img = NULL;

	/* Unpin the previous frame */
	release_prev_frame(dss_ctx);

	/* Set channel rotation */
	if (img1 != NULL &&
			(dss_ctx->current_buffer_rotation != img1->rotation)) {
		if (compdev_update_rotation(dss_ctx, img1->rotation) != 0)
			dev_warn(dss_ctx->dev,
				"Failed to update MCDE rotation "
				"(img1->rotation = %d), %d\n",
				img1->rotation, ret);
		else
			dss_ctx->current_buffer_rotation = img1->rotation;
	}

	if ((img1 != NULL) && (img1->flags & COMPDEV_OVERLAY_FLAG))
		ovly_img = img1;
	else if (img1 != NULL)
		fb_img = img1;


	if ((img2 != NULL) && (img2->flags & COMPDEV_OVERLAY_FLAG))
		ovly_img = img2;
	else if (img2 != NULL)
		fb_img = img2;

	/* Handle buffers */
	if (fb_img != NULL) {
		ret = compdev_setup_ovly(fb_img,
			&dss_ctx->ovly_buffer[i], dss_ctx->ovly[0], 1, dss_ctx);
		if (ret)
			dev_warn(dss_ctx->dev,
				"Failed to setup overlay[%d], %d\n", 0, ret);
		i++;
	} else {
		disable_overlay(dss_ctx->ovly[0]);
	}


	if (ovly_img != NULL) {
		ret = compdev_setup_ovly(ovly_img,
			&dss_ctx->ovly_buffer[i], dss_ctx->ovly[1], 0, dss_ctx);
		if (ret)
			dev_warn(dss_ctx->dev,
				"Failed to setup overlay[%d], %d\n", 1, ret);
	} else {
		disable_overlay(dss_ctx->ovly[1]);
	}

	/* Do the display update */
	mcde_dss_update_overlay(dss_ctx->ovly[0], true);

	return ret;
}

static int compdev_post_scene_info_locked(struct compdev *cd,
				struct compdev_scene_info *s_info)
{
	int ret = 0;

	dev_dbg(cd->dev, "%s\n", __func__);

	cd->s_info = *s_info;
	cd->sync_count = cd->s_info.img_count;

	/* always complete the fence in case someone is hanging incorrectly. */
	complete(&cd->fence);
	init_completion(&cd->fence);

	/* Handle callback */
	if (cd->si_cb != NULL) {
		mutex_unlock(&cd->lock);
		cd->si_cb(cd->cb_data, s_info);
		mutex_lock(&cd->lock);
	}
	return ret;
}


static int compdev_get_size_locked(struct dss_context *dss_ctx,
					struct compdev_size *size)
{
	int ret = 0;
	if ((dss_ctx->display_rotation) % 180) {
		size->height = dss_ctx->phy_size.width;
		size->width = dss_ctx->phy_size.height;
	} else {
		size->height = dss_ctx->phy_size.height;
		size->width = dss_ctx->phy_size.width;
	}

	return ret;
}

static int compdev_get_listener_state_locked(struct compdev *cd,
				enum compdev_listener_state *state)
{
	int ret = 0;

	*state = COMPDEV_LISTENER_OFF;
	if (cd->pb_cb != NULL)
		*state = COMPDEV_LISTENER_ON;
	return ret;
}

static long compdev_ioctl(struct file *file,
		unsigned int cmd,
		unsigned long arg)
{
	int ret;
	struct compdev *cd = (struct compdev *)file->private_data;
	struct compdev_img img;
	struct compdev_scene_info s_info;

	mutex_lock(&cd->lock);

	switch (cmd) {
	case COMPDEV_GET_SIZE_IOC:
	{
		struct compdev_size tmp;
		compdev_get_size_locked(&cd->dss_ctx, &tmp);
		ret = copy_to_user((void __user *)arg, &tmp,
							sizeof(tmp));
		if (ret)
			ret = -EFAULT;
	}
	break;
	case COMPDEV_GET_LISTENER_STATE_IOC:
	{
		enum compdev_listener_state state;
		compdev_get_listener_state_locked(cd, &state);
		ret = copy_to_user((void __user *)arg, &state,
				sizeof(state));
		if (ret)
			ret = -EFAULT;
	}
	break;
	case COMPDEV_POST_BUFFER_IOC:
		memset(&img, 0, sizeof(img));
		/* Get the user data */
		if (copy_from_user(&img, (void *)arg, sizeof(img))) {
			dev_warn(cd->dev,
				"%s: copy_from_user failed\n",
				__func__);
			mutex_unlock(&cd->lock);
			return -EFAULT;
		}
		ret = compdev_post_buffer_locked(cd, &img);

		break;
	case COMPDEV_POST_SCENE_INFO_IOC:
		memset(&s_info, 0, sizeof(s_info));
		/* Get the user data */
		if (copy_from_user(&s_info, (void *)arg, sizeof(s_info))) {
			dev_warn(cd->dev,
				"%s: copy_from_user failed\n",
				__func__);
			mutex_unlock(&cd->lock);
			return -EFAULT;
		}
		ret = compdev_post_scene_info_locked(cd, &s_info);

		break;

	default:
		ret = -ENOSYS;
	}

	mutex_unlock(&cd->lock);

	return ret;
}

static const struct file_operations compdev_fops = {
	.open = compdev_open,
	.release = compdev_release,
	.unlocked_ioctl = compdev_ioctl,
};

static void init_compdev(struct compdev *cd, const char *name)
{
	mutex_init(&cd->lock);
	INIT_LIST_HEAD(&cd->list);
	init_completion(&cd->fence);

	cd->mdev.minor = MISC_DYNAMIC_MINOR;
	cd->mdev.name = name;
	cd->mdev.fops = &compdev_fops;
	cd->dev = cd->mdev.this_device;
}

static void init_dss_context(struct dss_context *dss_ctx,
		struct mcde_display_device *ddev, struct compdev *cd)
{
	dss_ctx->ddev = ddev;
	dss_ctx->dev = cd->dev;
	memset(&dss_ctx->cache_ctx, 0, sizeof(struct buffer_cache_context));
	dss_ctx->cache_ctx.dev = dss_ctx->dev;
}

int compdev_create(struct mcde_display_device *ddev,
		struct mcde_overlay *parent_ovly, bool mcde_rotation)
{
	int ret = 0;
	int i;
	struct compdev *cd;
	struct mcde_video_mode vmode;
	struct mcde_overlay_info info;

	char name[10];

	if (dev_counter == 0) {
		for (i = 0; i < MAX_NBR_OF_COMPDEVS; i++)
			compdevs[i] = NULL;
	}

	if (dev_counter > MAX_NBR_OF_COMPDEVS)
		return -ENOMEM;

	cd = kzalloc(sizeof(struct compdev), GFP_KERNEL);
	if (!cd)
		return -ENOMEM;

	compdevs[dev_counter] = cd;
	cd->dev_index = dev_counter;

	snprintf(name, sizeof(name), "%s%d", COMPDEV_DEFAULT_DEVICE_PREFIX,
			dev_counter++);
	init_compdev(cd, name);

	init_dss_context(&cd->dss_ctx, ddev, cd);

	mcde_dss_get_video_mode(ddev, &vmode);

	cd->worker_thread = create_workqueue(name);
	if (!cd->worker_thread) {
		ret = -ENOMEM;
		goto fail_workqueue;
	}

	cd->dss_ctx.ovly[0] = parent_ovly;
	if (!cd->dss_ctx.ovly[0]) {
		ret = -ENOMEM;
		goto fail_create_ovly;
	}

	for (i = 1; i < NUM_COMPDEV_BUFS; i++) {
		cd->dss_ctx.ovly[i] = mcde_dss_create_overlay(ddev, &info);
		if (!cd->dss_ctx.ovly[i]) {
			ret = -ENOMEM;
			goto fail_create_ovly;
		}
		if (mcde_dss_enable_overlay(cd->dss_ctx.ovly[i]))
			goto fail_create_ovly;
		if (disable_overlay(cd->dss_ctx.ovly[i]))
			goto fail_create_ovly;
	}

	mcde_dss_get_native_resolution(ddev, &cd->dss_ctx.phy_size.width,
			&cd->dss_ctx.phy_size.height);
	cd->dss_ctx.display_rotation = mcde_dss_get_rotation(ddev);
	cd->dss_ctx.current_buffer_rotation = 0;

	cd->mcde_rotation = mcde_rotation;

	ret = misc_register(&cd->mdev);
	if (ret)
		goto fail_register_misc;
	mutex_lock(&dev_list_lock);
	list_add_tail(&cd->list, &dev_list);
	mutex_unlock(&dev_list_lock);

	goto out;

fail_register_misc:
fail_create_ovly:
	for (i = 0; i < NUM_COMPDEV_BUFS; i++) {
		if (cd->dss_ctx.ovly[i])
			mcde_dss_destroy_overlay(cd->dss_ctx.ovly[i]);
	}
fail_workqueue:
	kfree(cd);
out:
	return ret;
}


int compdev_get(int dev_idx, struct compdev **cd_pp)
{
	struct compdev *cd;
	cd = NULL;

	if (dev_idx >= MAX_NBR_OF_COMPDEVS)
		return -ENOMEM;

	cd = compdevs[dev_idx];
	if (cd != NULL) {
		mutex_lock(&cd->lock);
		cd->ref_count++;
		mutex_unlock(&cd->lock);
		*cd_pp = cd;
		return 0;
	} else {
		return -ENOMEM;
	}
}
EXPORT_SYMBOL(compdev_get);

int compdev_put(struct compdev *cd)
{
	int ret = 0;
	if (cd == NULL)
		return -ENOMEM;

	mutex_lock(&cd->lock);
	cd->ref_count--;
	if (cd->ref_count < 0)
		dev_warn(cd->dev,
				"%s: Incorrect ref count\n", __func__);
	mutex_unlock(&cd->lock);
	return ret;
}
EXPORT_SYMBOL(compdev_put);

int compdev_get_size(struct compdev *cd, struct compdev_size *size)
{
	int ret = 0;
	if (cd == NULL)
		return -ENOMEM;

	mutex_lock(&cd->lock);

	ret = compdev_get_size_locked(&cd->dss_ctx, size);

	mutex_unlock(&cd->lock);
	return ret;
}
EXPORT_SYMBOL(compdev_get_size);

int compdev_get_listener_state(struct compdev *cd,
	enum compdev_listener_state *listener_state)
{
	int ret = 0;
	if (cd == NULL)
		return -ENOMEM;

	mutex_lock(&cd->lock);

	ret = compdev_get_listener_state_locked(cd, listener_state);

	mutex_unlock(&cd->lock);
	return ret;
}
EXPORT_SYMBOL(compdev_get_listener_state);


int compdev_post_buffer(struct compdev *cd, struct compdev_img *img)
{
	int ret = 0;
	if (cd == NULL)
		return -ENOMEM;

	mutex_lock(&cd->lock);

	ret = compdev_post_buffer_locked(cd, img);

	mutex_unlock(&cd->lock);
	return ret;
}
EXPORT_SYMBOL(compdev_post_buffer);

int compdev_post_scene_info(struct compdev *cd,
			struct compdev_scene_info *s_info)
{
	int ret = 0;
	if (cd == NULL)
		return -ENOMEM;

	mutex_lock(&cd->lock);

	ret = compdev_post_scene_info_locked(cd, s_info);

	mutex_unlock(&cd->lock);
	return ret;
}
EXPORT_SYMBOL(compdev_post_scene_info);

int compdev_register_listener_callbacks(struct compdev *cd, void *data,
		post_buffer_callback pb_cb, post_scene_info_callback si_cb)
{
	int ret = 0;
	if (cd == NULL)
		return -ENOMEM;
	mutex_lock(&cd->lock);
	cd->cb_data = data;
	cd->pb_cb = pb_cb;
	cd->si_cb = si_cb;
	mutex_unlock(&cd->lock);
	return ret;
}
EXPORT_SYMBOL(compdev_register_listener_callbacks);

int compdev_deregister_callbacks(struct compdev *cd)
{
	int ret = 0;
	if (cd == NULL)
		return -ENOMEM;
	mutex_lock(&cd->lock);
	cd->cb_data = NULL;
	cd->pb_cb = NULL;
	cd->si_cb = NULL;
	mutex_unlock(&cd->lock);
	return ret;
}
EXPORT_SYMBOL(compdev_deregister_callbacks);

void compdev_destroy(struct mcde_display_device *ddev)
{
	struct compdev *cd;
	struct compdev *tmp;
	int i;

	mutex_lock(&dev_list_lock);
	list_for_each_entry_safe(cd, tmp, &dev_list, list) {
		if (cd->dss_ctx.ddev == ddev) {
			list_del(&cd->list);
			misc_deregister(&cd->mdev);
			for (i = 1; i < NUM_COMPDEV_BUFS; i++)
				mcde_dss_destroy_overlay(cd->dss_ctx.ovly[i]);
			b2r2_blt_close(cd->dss_ctx.blt_handle);

			release_prev_frame(&cd->dss_ctx);

			/* Free potential temp buffers */
			for (i = 0; i < cd->dss_ctx.temp_img_count; i++)
				free_comp_img_buf(cd->dss_ctx.temp_img[i],
						cd->dev);

			for (i = 0; i < BUFFER_CACHE_DEPTH; i++) {
				if (cd->dss_ctx.cache_ctx.img[i]) {
					free_comp_img_buf
						(cd->dss_ctx.cache_ctx.img[i],
							cd->dev);
					cd->dss_ctx.cache_ctx.img[i] = NULL;
				}
			}

			destroy_workqueue(cd->worker_thread);
			kfree(cd);
			break;
		}
	}
	dev_counter--;
	mutex_unlock(&dev_list_lock);
}

static void compdev_destroy_all(void)
{
	struct compdev *cd;
	struct compdev *tmp;
	int i;

	mutex_lock(&dev_list_lock);
	list_for_each_entry_safe(cd, tmp, &dev_list, list) {
		list_del(&cd->list);
		misc_deregister(&cd->mdev);
		for (i = 0; i < NUM_COMPDEV_BUFS; i++)
			mcde_dss_destroy_overlay(cd->dss_ctx.ovly[i]);

		release_prev_frame(&cd->dss_ctx);
		/* Free potential temp buffers */
		for (i = 0; i < cd->dss_ctx.temp_img_count; i++)
			free_comp_img_buf(cd->dss_ctx.temp_img[i], cd->dev);

		for (i = 0; i < BUFFER_CACHE_DEPTH; i++) {
			if (cd->dss_ctx.cache_ctx.img[i]) {
				free_comp_img_buf
					(cd->dss_ctx.cache_ctx.img[i],
						cd->dev);
				cd->dss_ctx.cache_ctx.img[i] = NULL;
			}
		}

		kfree(cd);
	}
	mutex_unlock(&dev_list_lock);

	mutex_destroy(&dev_list_lock);
}

static int __init compdev_init(void)
{
	pr_info("%s\n", __func__);

	mutex_init(&dev_list_lock);

	return 0;
}
module_init(compdev_init);

static void __exit compdev_exit(void)
{
	compdev_destroy_all();
	pr_info("%s\n", __func__);
}
module_exit(compdev_exit);

MODULE_AUTHOR("Anders Bauer <anders.bauer@stericsson.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Display overlay device driver");

