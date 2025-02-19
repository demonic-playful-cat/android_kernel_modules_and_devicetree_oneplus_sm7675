// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */
#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/fdtable.h>
#include <linux/seq_file.h>
#include <linux/hashtable.h>
#include <trace/hooks/mm.h>

#include "common.h"
#include "memstat.h"
#include "sys-memstat.h"

#define DEFINE_PROC_SHOW_ATTRIBUTE_SIZE_BUF(__name, buf_sz)		\
static int __name ## _open(struct inode *inode, struct file *file)	\
{									\
	return single_open_size(file, __name ## _show, pde_data(inode), \
				buf_sz);				\
}									\
									\
static const struct proc_ops __name ## _proc_ops = {			\
	.proc_open	= __name ## _open,				\
	.proc_read	= seq_read,					\
	.proc_lseek	= seq_lseek,					\
	.proc_release	= single_release,				\
}

static struct proc_dir_entry *mtrack_procs[MTRACK_MAX];
static struct mtrack_debugger *mtrack_debugger[MTRACK_MAX];
#define EXP_NAME_LEN (32)
#define NAME_LEN (32)

const char * const mtrack_text[MTRACK_MAX] = {
	"dma_buf",
	"gpu",
	"hybridswap"
};

static const char *bg_kthread_comm[] = {
	/* boost pool */
	"bp_prefill_camera",
	"bp_camera",
	"bp_mtk_mm",
	/* hybridswapd */
	"hybridswapd",
	/* chp kthread */
	"khpage_poold",
	/* uxmem refill kthread */
	"ux_page_pool_",
};

struct dma_info {
	char exp_name[EXP_NAME_LEN];
	char name[NAME_LEN];
	fmode_t f_mode;
	unsigned int f_flags;
	unsigned long size;
	unsigned long i_ino;
	unsigned long file_count;
	struct dma_buf *dmabuf;
	struct hlist_node head;
};

struct dma_proc {
	char name[TASK_COMM_LEN];
	pid_t pid;
	size_t size;
	struct hlist_head dma_bufs[1 << 10];
	struct list_head head;
};

struct dma_buf_priv {
	int count;
	size_t size;
	struct seq_file *s;
};

static void show_val_kb(struct seq_file *m, const char *s, unsigned long num)
{
	seq_put_decimal_ull_width_dup(m, s, num << (PAGE_SHIFT - 10), 8);
	seq_write(m, " kB\n", 4);
}

static int dma_buf_show(const struct dma_buf *buf_obj, void *private)
{
	int ret;
	struct dma_buf_attachment *attach_obj;
	int attach_count;
	struct dma_buf_priv *buf = (struct dma_buf_priv *)private;
	struct seq_file *s = buf->s;

	ret = dma_resv_lock_interruptible(buf_obj->resv, NULL);

	if (ret)
		goto err;

	spin_lock((spinlock_t *)&buf_obj->name_lock);
	seq_printf(s, "%08zu\t%08x\t%08x\t%08ld\t%s\t%08lu\t%s\n",
		   buf_obj->size,
		   buf_obj->file->f_flags, buf_obj->file->f_mode,
		   file_count(buf_obj->file),
		   buf_obj->exp_name,
		   file_inode(buf_obj->file)->i_ino,
		   buf_obj->name ?: "");
	spin_unlock((spinlock_t *)&buf_obj->name_lock);

	dma_resv_describe(buf_obj->resv, s);

	seq_puts(s, "\tAttached Devices:\n");
	attach_count = 0;

	list_for_each_entry(attach_obj, &buf_obj->attachments, node) {
		seq_printf(s, "\t%s\n", dev_name(attach_obj->dev));
		attach_count++;
	}
	dma_resv_unlock(buf_obj->resv);

	seq_printf(s, "Total %d devices attached\n\n", attach_count);

	buf->count++;
	buf->size += buf_obj->size;

	return 0;
err:
	return ret;
}

static int dma_buf_bufinfo_show(struct seq_file *s, void *unused)
{
	struct dma_buf_priv dma_buf_priv;

	dma_buf_priv.count = 0;
	dma_buf_priv.size = 0;
	dma_buf_priv.s = s;

	osvelte_info("%s:%d read %s, seq_buf size:%zu\n",
		     current->comm, current->tgid, __func__, s->size);

	seq_puts(s, "\nDma-buf Objects:\n");
	seq_printf(s, "%-8s\t%-8s\t%-8s\t%-8s\texp_name\t%-8s\n",
		   "size", "flags", "mode", "count", "ino");

	dma_buf_get_each(dma_buf_show, &dma_buf_priv);

	seq_printf(s, "\nTotal %d objects, %zu bytes\n",
		   dma_buf_priv.count, dma_buf_priv.size);

	return 0;
}
DEFINE_PROC_SHOW_ATTRIBUTE_SIZE_BUF(dma_buf_bufinfo, SZ_512K);

static int get_dma_buf_info(const void *data, struct file *file, unsigned int n)
{
	struct dma_proc *dma_proc;
	struct dma_info *dma_info;
	struct dma_buf *dmabuf;

	if (!is_dma_buf_file(file))
		return 0;

	dma_proc = (struct dma_proc *)data;
	hash_for_each_possible(dma_proc->dma_bufs, dma_info, head,
			       (unsigned long) file->private_data)
		if (file->private_data == dma_info->dmabuf)
			return 0;

	dma_info = kzalloc(sizeof(*dma_info), GFP_ATOMIC);
	if (!dma_info)
		return -ENOMEM;

	dmabuf = file->private_data;

	dma_info->dmabuf = dmabuf;
	strncpy(dma_info->exp_name, dmabuf->exp_name, EXP_NAME_LEN - 1);
	spin_lock(&dmabuf->name_lock);
	if (dmabuf->name)
		strncpy(dma_info->name, dmabuf->name, NAME_LEN - 1);
	spin_unlock(&dmabuf->name_lock);

	dma_info->i_ino = file_inode(dmabuf->file)->i_ino;
	dma_info->file_count = file_count(dmabuf->file);
	dma_info->f_flags = dmabuf->file->f_flags;
	dma_info->f_mode = dmabuf->file->f_mode;
	dma_info->size = dmabuf->size;

	dma_proc->size += dmabuf->size;
	hash_add(dma_proc->dma_bufs, &dma_info->head,
		 (unsigned long)dma_info->dmabuf);
	return 0;
}

static void free_dma_proc(struct dma_proc *proc)
{
	struct dma_info *tmp;
	struct hlist_node *n;
	int i;

	hash_for_each_safe(proc->dma_bufs, i, n, tmp, head) {
		hash_del(&tmp->head);
		kfree(tmp);
	}
	kfree(proc);
}

static void dma_proc_show(struct seq_file *s, struct dma_proc *proc)
{
	struct dma_info *tmp;
	int i;

	seq_printf(s, "\n%s (PID %d) size: %ld kB\nDMA Buffers:\n",
		   proc->name, proc->pid, proc->size / SZ_1K);
	seq_printf(s, "%-8s\t%-8s\t%-8s\t%-10s\t%-10s\t%-s\n",
		   "ino", "size", "count", "flags", "mode", "exp_name");

	hash_for_each(proc->dma_bufs, i, tmp, head) {
		seq_printf(s, "%08lu\t%-8zu\t%-8ld\t0x%08x\t0x%08x\t%-s\t%-s\n",
			   tmp->i_ino,
			   tmp->size / SZ_1K,
			   tmp->file_count,
			   tmp->f_flags, tmp->f_mode,
			   tmp->exp_name,
			   tmp->name);
	}
}

static int dma_buf_procinfo_show(struct seq_file *s, void *unused)
{
	struct task_struct *task, *thread;
	struct files_struct *files;
	int ret = 0;
	struct dma_proc *tmp, *n;
	LIST_HEAD(plist);

	osvelte_info("%s:%d read %s, seq_buf size:%zu\n",
		     current->comm, current->tgid, __func__, s->size);

	rcu_read_lock();
	for_each_process(task) {
		struct files_struct *group_leader_files = NULL;

		tmp = kzalloc(sizeof(*tmp), GFP_ATOMIC);
		if (!tmp) {
			ret = -ENOMEM;
			rcu_read_unlock();
			goto mem_err;
		}
		hash_init(tmp->dma_bufs);
		for_each_thread(task, thread) {
			task_lock(thread);
			if (unlikely(!group_leader_files))
				group_leader_files = task->group_leader->files;
			files = thread->files;
			if (files && (group_leader_files != files ||
				      thread == task->group_leader))
				ret = iterate_fd(files, 0, get_dma_buf_info, tmp);
			task_unlock(thread);
		}

		if (ret || hash_empty(tmp->dma_bufs))
			goto skip;

		get_task_comm(tmp->name, task);
		tmp->pid = task->tgid;
		list_add(&tmp->head, &plist);
		continue;
skip:
		free_dma_proc(tmp);
	}
	rcu_read_unlock();

	list_for_each_entry(tmp, &plist, head)
		dma_proc_show(s, tmp);

	ret = 0;
mem_err:
	list_for_each_entry_safe(tmp, n, &plist, head) {
		list_del(&tmp->head);
		free_dma_proc(tmp);
	}
	return ret;
}
DEFINE_PROC_SHOW_ATTRIBUTE_SIZE_BUF(dma_buf_procinfo, SZ_256K);

static int bg_kthread_show(struct seq_file *s, void *unused)
{
	int i;
	struct task_struct *p;

	seq_printf(s, "%-16s\t%-8s\t%-8s\n", "comm", "pid", "cpus");

	rcu_read_lock();
	for_each_process(p) {
		if (!(p->flags & PF_KTHREAD))
			continue;

		for (i = 0; i < ARRAY_SIZE(bg_kthread_comm); i++)
			if (strstr(p->comm, bg_kthread_comm[i]))
				seq_printf(s, "%-16s\t%-8d\t%*pbl\n",
					   p->comm, p->tgid,
					   cpumask_pr_args(p->cpus_ptr));
	}
	rcu_read_unlock();
	return 0;
}
DEFINE_PROC_SHOW_ATTRIBUTE(bg_kthread);

static int info_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "osvelte version v%d.%d.%d based on kernel-5.10\n",
		   OSVELTE_MAJOR, OSVELTE_MINOR, OSVELTE_PATCH_NUM);
	return 0;
}
DEFINE_PROC_SHOW_ATTRIBUTE(info);

void unregister_mtrack_debugger(enum mtrack_type type,
				struct mtrack_debugger *debugger)
{
	mtrack_debugger[type] = NULL;
}
EXPORT_SYMBOL_GPL(unregister_mtrack_debugger);

int register_mtrack_debugger(enum mtrack_type type,
			     struct mtrack_debugger *debugger)
{
	if (!debugger)
		return -EINVAL;

	if (mtrack_debugger[type])
		return -EEXIST;

	mtrack_debugger[type] = debugger;
	return 0;
}
EXPORT_SYMBOL_GPL(register_mtrack_debugger);


int register_mtrack_procfs(enum mtrack_type t, const char *name, umode_t mode,
			   const struct proc_ops *proc_ops, void *data)
{
	struct proc_dir_entry *entry;
	if (!mtrack_procs[t])
		return -EBUSY;

	entry = proc_create_data(name, mode, mtrack_procs[t], proc_ops, data);
	if (!entry)
		return -ENOMEM;

	return 0;
}
EXPORT_SYMBOL_GPL(register_mtrack_procfs);

void unregister_mtrack_procfs(enum mtrack_type t, const char *name)
{
	if (!unlikely(mtrack_procs[t]))
		return;

	remove_proc_subtree(name, mtrack_procs[t]);
}
EXPORT_SYMBOL_GPL(unregister_mtrack_procfs);

inline long read_mtrack_mem_usage(enum mtrack_type t, enum mtrack_subtype s)
{
	struct mtrack_debugger *d = mtrack_debugger[t];

	if (d && d->mem_usage)
		return d->mem_usage(s);
	return 0;
}

inline long read_pid_mtrack_mem_usage(enum mtrack_type t,
				      enum mtrack_subtype s, pid_t pid)
{
	struct mtrack_debugger *d = mtrack_debugger[t];

	if (d && d->pid_mem_usage)
		return d->pid_mem_usage(s, pid);
	return 0;
}

inline void dump_mtrack_usage_stat(enum mtrack_type t, bool verbose)
{
	struct mtrack_debugger *d = mtrack_debugger[t];

	if (d && d->dump_usage_stat) {
		osvelte_info("======= dump_%s\n", mtrack_text[t]);
		return d->dump_usage_stat(verbose);
	}
}

static void extra_meminfo_proc_show(void *data, struct seq_file *m)
{
	show_val_kb(m, "IonTotalCache:  ",
			read_mtrack_mem_usage(MTRACK_DMABUF, MTRACK_DMABUF_POOL));
	show_val_kb(m, "IonTotalUsed:   ",
			read_mtrack_mem_usage(MTRACK_DMABUF, MTRACK_DMABUF_SYSTEM_HEAP));
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_BOOSTPOOL)
	show_val_kb(m, "RsvPool:        ",
			read_mtrack_mem_usage(MTRACK_DMABUF, MTRACK_DMABUF_BOOST_POOL));
#endif /* CONFIG_OPLUS_FEATURE_MM_BOOSTPOOL */
	show_val_kb(m, "GPUTotalUsed:   ",
			read_mtrack_mem_usage(MTRACK_GPU, MTRACK_GPU_TOTAL));
}

int sys_memstat_init(void)
{
	struct proc_dir_entry *root;
	struct proc_dir_entry *dir_entry;
	int i;

	OSVELTE_STATIC_ASSERT(sizeof(struct dma_info) <= 128);

	if (register_trace_android_vh_meminfo_proc_show(extra_meminfo_proc_show, NULL)) {
		pr_err("register extra meminfo proc failed.\n");
		return -EINVAL;
	}

	root = proc_mkdir("osvelte", NULL);
	if (!root) {
		pr_err("create osvelte dir failed\n");
		return -ENOMEM;
	}
	proc_create("info", 0444, root, &info_proc_ops);
	proc_create("bg_kthread", 0444, root, &bg_kthread_proc_ops);
	/* proc_create("hybridswap_info", 0444, root, &hybridswap_info_proc_ops); */

	/* create mtrack dir here */
	for (i = 0; i < MTRACK_MAX; i++) {
		mtrack_procs[i] = proc_mkdir(mtrack_text[i], root);
		if (!mtrack_procs[i]) {
			osvelte_err("proc_fs: create %s failed\n",
				    mtrack_text[i]);
		}
	}

	dir_entry = mtrack_procs[MTRACK_DMABUF];
	if (dir_entry) {
		proc_create("procinfo", 0444, dir_entry,
			    &dma_buf_procinfo_proc_ops);
		proc_create("bufinfo", 0444, dir_entry,
			    &dma_buf_bufinfo_proc_ops);
	}
	return 0;
}

int sys_memstat_exit(void)
{
	remove_proc_subtree("osvelte", NULL);
	unregister_trace_android_vh_meminfo_proc_show(extra_meminfo_proc_show, NULL);
	return 0;
}
