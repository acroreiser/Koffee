/*
 * Copyright (C) ST-Ericsson SA 2010
 *
 * ST-Ericsson B2R2 dynamic debug
 *
 * Author: Fredrik Allansson <fredrik.allansson@stericsson.com>
 * for ST-Ericsson.
 *
 * License terms: GNU General Public License (GPL), version 2.
 */

#include "b2r2_debug.h"
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

int b2r2_log_levels[B2R2_LOG_LEVEL_COUNT];
static struct dentry *log_lvl_dir;
static int module_init;

#define CHARS_IN_NODE_DUMP 1544
#define DUMPED_NODE_SIZE (CHARS_IN_NODE_DUMP * sizeof(char) + 1)

static void dump_node(char *dst, struct b2r2_node *node)
{
	dst += sprintf(dst, "node 0x%08x ------------------\n",
			(unsigned int)node);

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_NIP:", node->node.GROUP0.B2R2_NIP);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_CIC:", node->node.GROUP0.B2R2_CIC);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_INS:", node->node.GROUP0.B2R2_INS);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_ACK:", node->node.GROUP0.B2R2_ACK);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_TBA:", node->node.GROUP1.B2R2_TBA);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_TTY:", node->node.GROUP1.B2R2_TTY);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_TXY:", node->node.GROUP1.B2R2_TXY);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_TSZ:", node->node.GROUP1.B2R2_TSZ);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S1CF:", node->node.GROUP2.B2R2_S1CF);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S2CF:", node->node.GROUP2.B2R2_S2CF);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S1BA:", node->node.GROUP3.B2R2_SBA);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S1TY:", node->node.GROUP3.B2R2_STY);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S1XY:", node->node.GROUP3.B2R2_SXY);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S1SZ:", node->node.GROUP3.B2R2_SSZ);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S2BA:", node->node.GROUP4.B2R2_SBA);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S2TY:", node->node.GROUP4.B2R2_STY);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S2XY:", node->node.GROUP4.B2R2_SXY);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S2SZ:", node->node.GROUP4.B2R2_SSZ);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S3BA:", node->node.GROUP5.B2R2_SBA);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S3TY:", node->node.GROUP5.B2R2_STY);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S3XY:", node->node.GROUP5.B2R2_SXY);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_S3SZ:", node->node.GROUP5.B2R2_SSZ);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_CWO:", node->node.GROUP6.B2R2_CWO);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_CWS:", node->node.GROUP6.B2R2_CWS);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_CCO:", node->node.GROUP7.B2R2_CCO);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_CML:", node->node.GROUP7.B2R2_CML);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_PMK:", node->node.GROUP8.B2R2_PMK);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_FCTL:", node->node.GROUP8.B2R2_FCTL);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_RSF:", node->node.GROUP9.B2R2_RSF);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_RZI:", node->node.GROUP9.B2R2_RZI);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_HFP:", node->node.GROUP9.B2R2_HFP);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_VFP:", node->node.GROUP9.B2R2_VFP);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_Y_RSF:", node->node.GROUP10.B2R2_RSF);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_Y_RZI:", node->node.GROUP10.B2R2_RZI);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_Y_HFP:", node->node.GROUP10.B2R2_HFP);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_Y_VFP:", node->node.GROUP10.B2R2_VFP);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_FF0:", node->node.GROUP11.B2R2_FF0);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_FF1:", node->node.GROUP11.B2R2_FF1);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_FF2:", node->node.GROUP11.B2R2_FF2);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_FF3:", node->node.GROUP11.B2R2_FF3);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_KEY1:", node->node.GROUP12.B2R2_KEY1);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_KEY2:", node->node.GROUP12.B2R2_KEY2);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_XYL:", node->node.GROUP13.B2R2_XYL);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_XYP:", node->node.GROUP13.B2R2_XYP);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_SAR:", node->node.GROUP14.B2R2_SAR);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_USR:", node->node.GROUP14.B2R2_USR);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_IVMX0:", node->node.GROUP15.B2R2_VMX0);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_IVMX1:", node->node.GROUP15.B2R2_VMX1);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_IVMX2:", node->node.GROUP15.B2R2_VMX2);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_IVMX3:", node->node.GROUP15.B2R2_VMX3);
	dst += sprintf(dst, "--\n");

	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_OVMX0:", node->node.GROUP16.B2R2_VMX0);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_OVMX1:", node->node.GROUP16.B2R2_VMX1);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_OVMX2:", node->node.GROUP16.B2R2_VMX2);
	dst += sprintf(dst, "%s\t0x%08x\n",
			"B2R2_OVMX3:", node->node.GROUP16.B2R2_VMX3);
	dst += sprintf(dst, "--\n");

}

void b2r2_debug_job_done(struct b2r2_control *cont,
		struct b2r2_node *first_node)
{
	struct b2r2_node *node = first_node;
	struct b2r2_node **dst_node;
	unsigned int node_count = 0;

	while (node != NULL) {
		node_count++;
		node = node->next;
	}

	mutex_lock(&cont->last_job_lock);

	if (cont->last_job) {
		node = cont->last_job;
		while (node != NULL) {
			struct b2r2_node *tmp = node->next;
			kfree(node);
			node = tmp;
		}
		cont->last_job = NULL;
	}

	node = first_node;
	dst_node = &cont->last_job;
	while (node != NULL) {
		*dst_node = kzalloc(sizeof(**dst_node), GFP_KERNEL);
		if (!(*dst_node))
			goto last_job_alloc_failed;

		memcpy(*dst_node, node, sizeof(**dst_node));

		dst_node = &((*dst_node)->next);
		node = node->next;
	}

	mutex_unlock(&cont->last_job_lock);

	return;

last_job_alloc_failed:
	mutex_unlock(&cont->last_job_lock);

	while (cont->last_job != NULL) {
		struct b2r2_node *tmp = cont->last_job->next;
		kfree(cont->last_job);
		cont->last_job = tmp;
	}

	return;
}

static ssize_t last_job_read(struct file *filp, char __user *buf,
		size_t bytes, loff_t *off)
{
	struct b2r2_control *cont = filp->f_dentry->d_inode->i_private;
	struct b2r2_node *node = cont->last_job;
	int node_count = 0;
	int i;

	size_t size;
	size_t count;
	loff_t offs = *off;

	for (; node != NULL; node = node->next)
		node_count++;

	size = node_count * DUMPED_NODE_SIZE;

	if (node_count != cont->prev_node_count) {
		kfree(cont->last_job_chars);

		cont->last_job_chars = kzalloc(size, GFP_KERNEL);
		if (!cont->last_job_chars)
			return 0;
		cont->prev_node_count = node_count;
	}

	mutex_lock(&cont->last_job_lock);
	node = cont->last_job;
	for (i = 0; i < node_count; i++) {
		BUG_ON(node == NULL);
		dump_node(cont->last_job_chars +
			i * DUMPED_NODE_SIZE/sizeof(char),
			node);
		node = node->next;
	}
	mutex_unlock(&cont->last_job_lock);

	if (offs > size)
		return 0;

	if (offs + bytes > size)
		count = size - offs;
	else
		count = bytes;

	if (copy_to_user(buf, cont->last_job_chars + offs, count))
		return -EFAULT;

	*off = offs + count;
	return count;
}

static const struct file_operations last_job_fops = {
	.read = last_job_read,
};

int b2r2_debug_init(struct b2r2_control *cont)
{
	int i;

	if (!module_init) {
		for (i = 0; i < B2R2_LOG_LEVEL_COUNT; i++)
			b2r2_log_levels[i] = 0;

#if !defined(CONFIG_DYNAMIC_DEBUG) && defined(CONFIG_DEBUG_FS)
		/*
		 * If dynamic debug is disabled we need some other way to
		 * control the log prints
		 */
		log_lvl_dir = debugfs_create_dir("b2r2_log", NULL);

		/* No need to save the files,
		 * they will be removed recursively */
		if (!IS_ERR_OR_NULL(log_lvl_dir)) {
			(void)debugfs_create_bool("warnings", 0644, log_lvl_dir,
				&b2r2_log_levels[B2R2_LOG_LEVEL_WARN]);
			(void)debugfs_create_bool("info", 0644, log_lvl_dir,
				&b2r2_log_levels[B2R2_LOG_LEVEL_INFO]);
			(void)debugfs_create_bool("debug", 0644, log_lvl_dir,
				&b2r2_log_levels[B2R2_LOG_LEVEL_DEBUG]);
			(void)debugfs_create_bool("regdumps", 0644, log_lvl_dir,
				&b2r2_log_levels[B2R2_LOG_LEVEL_REGDUMP]);
		}

#elif defined(CONFIG_DYNAMIC_DEBUG)
		/* log_lvl_dir is never used */
		(void)log_lvl_dir;
#endif
		module_init++;
	}

	if (!IS_ERR_OR_NULL(cont->debugfs_debug_root_dir)) {
		/* No need to save the file,
		 * it will be removed recursively */
		(void)debugfs_create_file("last_job", 0444,
				cont->debugfs_debug_root_dir, cont,
				&last_job_fops);
	}

	mutex_init(&cont->last_job_lock);

	return 0;
}

void b2r2_debug_exit(void)
{
#if !defined(CONFIG_DYNAMIC_DEBUG) && defined(CONFIG_DEBUG_FS)
	module_init--;
	if (!module_init && !IS_ERR_OR_NULL(log_lvl_dir)) {
		debugfs_remove_recursive(log_lvl_dir);
		log_lvl_dir = NULL;
	}
#endif
}
