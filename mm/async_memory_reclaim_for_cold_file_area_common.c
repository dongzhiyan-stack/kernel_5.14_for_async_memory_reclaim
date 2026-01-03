#include <linux/sched/mm.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/vmpressure.h>
#include <linux/vmstat.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/mm_inline.h>
#include <linux/backing-dev.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/compaction.h>
#include <linux/notifier.h>
#include <linux/rwsem.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/memcontrol.h>
#include <linux/delayacct.h>
#include <linux/sysctl.h>
#include <linux/oom.h>
#include <linux/pagevec.h>
#include <linux/prefetch.h>
#include <linux/printk.h>
#include <linux/dax.h>
#include <linux/psi.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>

#include <linux/swapops.h>
#include <linux/balloon_compaction.h>

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>

#include <linux/kallsyms.h>
#include <linux/version.h>
#include <linux/mm_inline.h>
#include <linux/proc_fs.h>
#include <linux/xarray.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/kernel_read_file.h>

#include "async_memory_reclaim_for_cold_file_area.h"

/*****proc文件系统**********************************************************************************************************************/
//to_writeonly_cold_list_age_dx
static int to_writeonly_cold_list_age_dx_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.to_writeonly_cold_list_age_dx);
	return 0;
}
static int to_writeonly_cold_list_age_dx_open(struct inode *inode, struct file *file)
{
	return single_open(file, to_writeonly_cold_list_age_dx_show, NULL);
}
static ssize_t to_writeonly_cold_list_age_dx_write(struct file *file,
				const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.to_writeonly_cold_list_age_dx = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops to_writeonly_cold_list_age_dx_fops = {
	.proc_open		= to_writeonly_cold_list_age_dx_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= to_writeonly_cold_list_age_dx_write,
};

//file_area_cold_level
static int file_area_cold_level_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.file_area_cold_level);
	return 0;
}
static int file_area_cold_level_open(struct inode *inode, struct file *file)
{
	return single_open(file, file_area_cold_level_show, NULL);
}
static ssize_t file_area_cold_level_write(struct file *file,
				const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.file_area_cold_level = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops file_area_cold_level_fops = {
	.proc_open		= file_area_cold_level_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= file_area_cold_level_write,
};

//to_down_list_age_dx
static int to_down_list_age_dx_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.to_down_list_age_dx);
	return 0;
}
static int to_down_list_age_dx_open(struct inode *inode, struct file *file)
{
	return single_open(file, to_down_list_age_dx_show, NULL);
}
static ssize_t to_down_list_age_dx_write(struct file *file,
				const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.to_down_list_age_dx = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops to_down_list_age_dx_fops = {
	.proc_open		= to_down_list_age_dx_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= to_down_list_age_dx_write,
};

//file_area_reclaim_read_age_dx
static int file_area_reclaim_read_age_dx_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.file_area_reclaim_read_age_dx_ori);
	return 0;
}
static int file_area_reclaim_read_age_dx_open(struct inode *inode, struct file *file)
{
	return single_open(file, file_area_reclaim_read_age_dx_show, NULL);
}
static ssize_t file_area_reclaim_read_age_dx_write(struct file *file,
				const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.file_area_reclaim_read_age_dx_ori = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops file_area_reclaim_read_age_dx_fops = {
	.proc_open		= file_area_reclaim_read_age_dx_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= file_area_reclaim_read_age_dx_write,
};

//writeonly_file_age_dx
static int writeonly_file_age_dx_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.writeonly_file_age_dx_ori);
	return 0;
}
static int writeonly_file_age_dx_open(struct inode *inode, struct file *file)
{
	return single_open(file, writeonly_file_age_dx_show, NULL);
}
static ssize_t writeonly_file_age_dx_write(struct file *file,
				const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.writeonly_file_age_dx_ori = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops writeonly_file_age_dx_fops = {
	.proc_open		= writeonly_file_age_dx_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= writeonly_file_age_dx_write,
};

//reclaim_page_print_level
static int reclaim_page_print_level_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.reclaim_page_print_level);
	return 0;
}
static int reclaim_page_print_level_open(struct inode *inode, struct file *file)
{
	return single_open(file, reclaim_page_print_level_show, NULL);
}
static ssize_t reclaim_page_print_level_write(struct file *file,
				const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.reclaim_page_print_level = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops reclaim_page_print_level_fops = {
	.proc_open		= reclaim_page_print_level_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= reclaim_page_print_level_write,
};

//refault_page_print_level
static int refault_page_print_level_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.refault_page_print_level);
	return 0;
}
static int refault_page_print_level_open(struct inode *inode, struct file *file)
{
	return single_open(file, refault_page_print_level_show, NULL);
}
static ssize_t refault_page_print_level_write(struct file *file,
				const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.refault_page_print_level = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops refault_page_print_level_fops = {
	.proc_open		= refault_page_print_level_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= refault_page_print_level_write,
};

//refault_file_area_scan_dx
static int refault_file_area_scan_dx_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.refault_file_area_scan_dx);
	return 0;
}
static int refault_file_area_scan_dx_open(struct inode *inode, struct file *file)
{
	return single_open(file, refault_file_area_scan_dx_show, NULL);
}
static ssize_t refault_file_area_scan_dx_write(struct file *file,
				const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.refault_file_area_scan_dx = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops refault_file_area_scan_dx_fops = {
	.proc_open		= refault_file_area_scan_dx_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= refault_file_area_scan_dx_write,
};

//file_area_hot_to_temp_age_dx
static int file_area_hot_to_temp_age_dx_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.file_area_hot_to_temp_age_dx);
	return 0;
}
static int file_area_hot_to_temp_age_dx_open(struct inode *inode, struct file *file)
{
	return single_open(file, file_area_hot_to_temp_age_dx_show, NULL);
}
static ssize_t file_area_hot_to_temp_age_dx_write(struct file *file,
				const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.file_area_hot_to_temp_age_dx_ori = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops file_area_hot_to_temp_age_dx_fops = {
	.proc_open		= file_area_hot_to_temp_age_dx_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= file_area_hot_to_temp_age_dx_write,
};
//file_area_refault_to_temp_age_dx
static int file_area_refault_to_temp_age_dx_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.file_area_refault_to_temp_age_dx);
	return 0;
}
static int file_area_refault_to_temp_age_dx_open(struct inode *inode, struct file *file)
{
	return single_open(file, file_area_refault_to_temp_age_dx_show, NULL);
}
static ssize_t file_area_refault_to_temp_age_dx_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.file_area_refault_to_temp_age_dx_ori = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops file_area_refault_to_temp_age_dx_fops = {
	.proc_open		= file_area_refault_to_temp_age_dx_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= file_area_refault_to_temp_age_dx_write,
};
//file_area_temp_to_cold_age_dx
static int file_area_temp_to_cold_age_dx_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.file_area_temp_to_cold_age_dx);
	return 0;
}
static int file_area_temp_to_cold_age_dx_open(struct inode *inode, struct file *file)
{
	return single_open(file, file_area_temp_to_cold_age_dx_show, NULL);
}
static ssize_t file_area_temp_to_cold_age_dx_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.file_area_temp_to_cold_age_dx_ori = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops file_area_temp_to_cold_age_dx_fops = {
	.proc_open		= file_area_temp_to_cold_age_dx_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= file_area_temp_to_cold_age_dx_write,
};
//file_area_free_age_dx
static int file_area_free_age_dx_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.file_area_free_age_dx_ori);
	return 0;
}
static int file_area_free_age_dx_open(struct inode *inode, struct file *file)
{
	return single_open(file, file_area_free_age_dx_show, NULL);
}
static ssize_t file_area_free_age_dx_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.file_area_free_age_dx_ori = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops file_area_free_age_dx_fops = {
	.proc_open		= file_area_free_age_dx_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= file_area_free_age_dx_write,
};
//file_stat_delete_age_dx
static int file_stat_delete_age_dx_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.file_stat_delete_age_dx);
	return 0;
}
static int file_stat_delete_age_dx_open(struct inode *inode, struct file *file)
{
	return single_open(file, file_stat_delete_age_dx_show, NULL);
}
static ssize_t file_stat_delete_age_dx_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val < 1000)
		hot_cold_file_global_info.file_stat_delete_age_dx = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops file_stat_delete_age_dx_fops = {
	.proc_open		= file_stat_delete_age_dx_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= file_stat_delete_age_dx_write,
};
//global_age_period
static int global_age_period_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.global_age_period);
	return 0;
}
static int global_age_period_open(struct inode *inode, struct file *file)
{
	return single_open(file, global_age_period_show, NULL);
}
static ssize_t global_age_period_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val >= 10 && val <= 60)
		hot_cold_file_global_info.global_age_period = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops global_age_period_fops = {
	.proc_open		= global_age_period_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= global_age_period_write,
};
//nr_pages_level
static int nr_pages_level_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", hot_cold_file_global_info.nr_pages_level);
	return 0;
}
static int nr_pages_level_open(struct inode *inode, struct file *file)
{
	return single_open(file, nr_pages_level_show, NULL);
}
static ssize_t nr_pages_level_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val > 0)
		hot_cold_file_global_info.nr_pages_level = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops nr_pages_level_fops = {
	.proc_open		= nr_pages_level_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= nr_pages_level_write,
};
//warm_list_printk
static int warm_list_printk_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", shrink_page_printk_open1);
	return 0;
}
static int warm_list_printk_open(struct inode *inode, struct file *file)
{
	return single_open(file, warm_list_printk_show, NULL);
}
static ssize_t warm_list_printk_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val <= 1)
		warm_list_printk = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops warm_list_printk_fops = {
	.proc_open		= warm_list_printk_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= warm_list_printk_write,
};

//open_print
static int open_print_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", shrink_page_printk_open1);
	return 0;
}
static int open_print_open(struct inode *inode, struct file *file)
{
	return single_open(file, open_print_show, NULL);
}
static ssize_t open_print_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val <= 1)
		shrink_page_printk_open1 = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops open_print_fops = {
	.proc_open		= open_print_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= open_print_write,
};
//open_print_important
static int open_print_important_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", shrink_page_printk_open_important);
	return 0;
}
static int open_print_important_open(struct inode *inode, struct file *file)
{
	return single_open(file, open_print_important_show, NULL);
}
static ssize_t open_print_important_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val <= 1)
		shrink_page_printk_open_important = val;
	else
		return -EINVAL;

	return count;
}
static const struct proc_ops open_print_important_fops = {
	.proc_open		= open_print_important_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= open_print_important_write,
};

//file_stat_debug_or_make_backlist
static int print_file_stat_one_file_area_info(struct seq_file *m,struct list_head *file_area_list_head,unsigned int file_area_type,char *file_area_list_name,char is_proc_print,char file_area_from_multi_warm_list)
{
	struct file_area *p_file_area = NULL;
	struct file_area *p_file_area_last = NULL;
	
	unsigned int file_area_count = 0;
	unsigned int file_area_all = 0;
	unsigned int file_area_read_count = 0;


	if(is_proc_print)
		seq_printf(m, "%s\n",file_area_list_name);
	else
		printk("%s\n",file_area_list_name);

	if(list_empty(file_area_list_head))
		return 0;

	p_file_area_last = list_last_entry(file_area_list_head,struct file_area,file_area_list);

	if(is_proc_print)
		seq_printf(m, "tail p_file_area:0x%llx status:0x%x age:%d\n",(u64)p_file_area_last,p_file_area_last->file_area_state,p_file_area_last->file_area_age);
	else
		printk("tail p_file_area:0x%llx status:0x%x age:%d\n",(u64)p_file_area_last,p_file_area_last->file_area_state,p_file_area_last->file_area_age);

	/* 执行该函数时，已经执行了rcu_read_lock()，故不用担心遍历到的p_file_area被异步内存回收线程释放了。
	 * 但是，如果该p_file_area被异步内存回收线程移动到其他file_stat->temp、hot、refault等链表了。
	 * 那这个函数就会因p_file_area被移动到了其他链表头，而导致下边的遍历陷入死循环。目前的解决办法
	 * 是：检测file_area的状态是否变了，变了的话就跳出循环。需要注意，一个file_area会具备多种状态。
	 * 比如file_stat->temp链表上的file_area同时有in_temp和in_hot属性，file_stat->free链表上的file_area
	 * 同时具备in_free和in_refault属性。还需要一点，比如遍历file_area->temp链表上的p_file_area时，
	 * 被移动到了file_stat->hot链表头，则下边的循环，下边的循环下次遍历到的p_file_area将是
	 * file_stat->hot链表头，而file_stat->hot链表头又敲好所有file_area都被释放了，那下边循环
	 * 每次遍历得到的p_file_area都是file_stat->hot链表头，又要陷入死循环。还有，如果file_stat->temp
	 * 链表上的file_area参与内存，会移动到file_area_free临时链表，情况很复杂。没有好办法，感觉
	 * 需要该函数标记file_stat的in_print标记，然后禁止该file_stat的file_area跨链表移动???????????*/
	list_for_each_entry_reverse(p_file_area,file_area_list_head,file_area_list){

		/*新版本的方案，in_temp属性的file_area去掉了，没有in_free、in_hot、in_refault、in_mapcount属性就表示in_temp属性的file_area*/
		if(-1 != file_area_type){
			if(0 == file_area_from_multi_warm_list){
				/* 遍历file_stat_small->other链表上的file_area，同时具备refault、hot、free、mapcount属性，有一个都成立
				 * file_stat->hot链表上现在的file_area有hot、mapcount、refault属性，此时不能crash*/
				//if(get_file_area_list_status(p_file_area) != file_area_type)
				if((get_file_area_list_status(p_file_area) & file_area_type) == 0){
					pr_warn("%s invalid file_area:0x%llx state:0x%x!!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
					if(is_proc_print)
						seq_printf(m,"%s invalid file_area:0x%llx state:0x%x!!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
					else
						printk("%s invalid file_area:0x%llx state:0x%x!!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

					break;
				}
			}else{
				if(list_num_get(p_file_area) != file_area_type)
					pr_warn("%s invalid num file_area:0x%llx state:0x%x!!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
			}
		}else{
			if(file_area_in_temp_list_error(p_file_area)){
				if(is_proc_print)
					seq_printf(m,"%s invalid file_area:0x%llx state:0x%x!!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
				else
					printk("%s invalid file_area:0x%llx state:0x%x!!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
			}
		}

		if(p_file_area->file_area_age > hot_cold_file_global_info.global_age){
			if(is_proc_print)
				seq_printf(m,"2:%s invalid file_area:0x%llx state:0x%x!!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
			else
				printk("2:%s invalid file_area:0x%llx state:0x%x!!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			break;
		}

		file_area_count ++;

		if(file_area_in_read(p_file_area))
			file_area_read_count ++;

		if((p_file_area->file_area_age - p_file_area_last->file_area_age > 1) || (p_file_area_last->file_area_age - p_file_area->file_area_age > 1)){
			if(is_proc_print)
				seq_printf(m, "-->  p_file_area:0x%llx status:0x%x age:%d file_area_count:%d\n",(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age,file_area_count);
			else
				printk("-->  p_file_area:0x%llx status:0x%x age:%d file_area_count:%d\n",(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age,file_area_count);

			file_area_count = 0;
			p_file_area_last = p_file_area;
		}
		file_area_all ++;
	}

	if(is_proc_print)
		seq_printf(m, "file_area_count:%d file_area_write_count:%d\n",file_area_all,file_area_all - file_area_read_count);
	else
		printk("file_area_count:%d file_area_read_count:%d\n",file_area_all,file_area_all - file_area_read_count);

	return 0;
}
/*****打印该文件所有的file_area信息**********/
static int file_stat_debug_or_make_backlist(struct seq_file *m,char is_proc_print)
{
	struct file_stat_base *p_file_stat_base;
	unsigned int file_stat_type;
	int ret = 0;
	char file_stat_in_global;

	/* 取出file_stat并打印该文件的所有file_area。但是有两个并发场景需要注意，
	 * 1:该文件inode被并发iput()释放了，要防止
	 * 2:该file_stat被异步内存回收线程并发cold_file_stat_delete()释放了，标记hot_cold_file_global_info.print_file_stat为NULL*/

	/*rcu加锁保证这个宽限期内，这个file_stat结构体不能cold_file_stat_delete()异步释放了，否则这里再使用它就是访问非法内存!!!!!!!!!!!!!!!!!!!*/
	rcu_read_lock();
	smp_rmb();
	p_file_stat_base = READ_ONCE(hot_cold_file_global_info.print_file_stat);//这个赋值留着，有警示作用
	
	file_stat_in_global = file_stat_in_global_base(p_file_stat_base);

	/* 不能使用p_file_stat_base判断该file_stat是否被异步内存回收线程cold_file_stat_delete()里标记NULL。
	 * 存在极端情况，这里"p_file_stat_base = hot_cold_file_global_info.print_file_stat"赋值后，异步内存
	 * 回收线程cold_file_stat_delete()里立即标记hot_cold_file_global_info.print_file_stat为NULL。因此
	 * 必须判断hot_cold_file_global_info.print_file_stat这个源头。  还有一点，这里rcu加锁后，再smp_rmb()，
	 * 有两种情况。 1:hot_cold_file_global_info.print_file_stat不是NULL，if成立，正常打印file_stat的
	 * file_area。如果打印file_stat的file_area过程，该file_stat被异步内存回收线程并发cold_file_stat_delete()
	 * 标记NULL，也没事，因为此时rcu宽限期，不会真正释放掉file_stat结构体，只是call_rcu()将结构体添加到
	 * 待释放的rcu链表。 2:异步内存回收先执行，cold_file_stat_delete()里标记
	 * hot_cold_file_global_info.print_file_stat为NULL。这里if不成立，就不会再使用该file_stat了。*/
	//if(!p_file_stat_base){
	if(!READ_ONCE(hot_cold_file_global_info.print_file_stat)){
		if(is_proc_print)
			seq_printf(m, "invalid file_stat\n");
		else				
			printk("invalid file_stat\n");

		goto err;
	}
	p_file_stat_base = READ_ONCE(hot_cold_file_global_info.print_file_stat);

	if(file_stat_in_replaced_file_base(p_file_stat_base) || file_stat_in_delete_base(p_file_stat_base)){
		if(is_proc_print)
			seq_printf(m, "invalid file_stat:0x%llx 0x%x has replace or delete!!!\n",(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		else
			printk("invalid file_stat:0x%llx 0x%x has replace or delete!!!\n",(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

		ret = EINVAL;
		goto err;
	}

	/*global_file_stat->mapping非法*/
	if(!file_stat_in_global && (u64)p_file_stat_base != p_file_stat_base->mapping->rh_reserved1){
		if(is_proc_print)
			seq_printf(m, "invalid file_stat:0x%llx 0x%lx\n",(u64)p_file_stat_base,p_file_stat_base->mapping->rh_reserved1);
		else
			printk("invalid file_stat:0x%llx 0x%lx\n",(u64)p_file_stat_base,p_file_stat_base->mapping->rh_reserved1);

		ret = EINVAL;
		goto err;
	}

	if(is_proc_print)
		seq_printf(m, "file_stat:0x%llx\n",(u64)p_file_stat_base);
	else				
		printk("file_stat:0x%llx\n",(u64)p_file_stat_base);

	if(file_stat_in_global)
		file_stat_type = FILE_STAT_NORMAL;
	else
		file_stat_type = get_file_stat_type(p_file_stat_base);

	//print_file_stat_one_file_area_info(m,&p_file_stat_base->file_area_temp,1 << F_file_area_in_temp_list,"temp list",1,0);
	print_file_stat_one_file_area_info(m,&p_file_stat_base->file_area_temp,-1,"temp list",1,0);

	if(FILE_STAT_SMALL == file_stat_type){
		unsigned int file_area_type = (1 << F_file_area_in_hot_list) | (1 << F_file_area_in_refault_list)| (1 << F_file_area_in_mapcount_list) | (1 << F_file_area_in_free_list);
		struct file_stat_small *p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);

		print_file_stat_one_file_area_info(m,&p_file_stat_small->file_area_other,file_area_type,"other list",1,0);
	}else if(FILE_STAT_NORMAL == file_stat_type){
		struct file_stat *p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

		//print_file_stat_one_file_area_info(m,&p_file_stat->file_area_refault,1 << F_file_area_in_refault_list,"refault list",1,0);
		print_file_stat_one_file_area_info(m,&p_file_stat->file_area_free,1 << F_file_area_in_free_list,"free list",1,0);
		print_file_stat_one_file_area_info(m,&p_file_stat->file_area_hot,1 << F_file_area_in_hot_list,"hot list",1,0);

		print_file_stat_one_file_area_info(m,&p_file_stat->file_area_warm_hot,POS_WARM_HOT,"warm_hot list",1,1);
		print_file_stat_one_file_area_info(m,&p_file_stat->file_area_warm,POS_WARM,"warm list",1,1);
		print_file_stat_one_file_area_info(m,&p_file_stat->file_area_warm_cold,POS_WARM,"warm_cold list",1,1);
		print_file_stat_one_file_area_info(m,&p_file_stat->file_area_writeonly_or_cold,POS_WIITEONLY_OR_COLD,"writeonly_cold list",1,1);

		if(file_stat_in_global_base(p_file_stat_base)){
			struct global_file_stat *p_global_file_stat = container_of(p_file_stat,struct global_file_stat,file_stat);

			print_file_stat_one_file_area_info(m,&p_global_file_stat->file_area_warm_middle_hot,POS_WARM_MIDDLE_HOT,"warm_middle_hot list",1,1);
			print_file_stat_one_file_area_info(m,&p_global_file_stat->file_area_warm_middle,POS_WARM_MIDDLE,"warm_middle list",1,1);
			//print_file_stat_one_file_area_info(m,&p_global_file_stat->file_area_warm_cold,POS_WARM_COLD,"warm_cold list",1,1);
			print_file_stat_one_file_area_info(m,&p_global_file_stat->file_area_mapcount,1 << F_file_area_in_mapcount_list,"mapcount list",1,0);
			print_file_stat_one_file_area_info(m,&p_global_file_stat->file_area_refault,1 << F_file_area_in_refault_list,"refault list",1,0);
		}
	}

err:
	rcu_read_unlock();
	return ret;
}
static int file_stat_debug_or_make_backlist_show(struct seq_file *m, void *v)
{
	return file_stat_debug_or_make_backlist(m,1);
}
static int file_stat_debug_or_make_backlist_open(struct inode *inode, struct file *file)
{
	return single_open(file, file_stat_debug_or_make_backlist_show, NULL);
}
static ssize_t file_stat_debug_or_make_backlist_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	char *file_path = NULL;
	char *p;
	struct file *file_temp;
	struct inode *inode = NULL;
	struct file_stat_base *p_file_stat_base;
	int ret = 0,file_blacklist = 0,file_debug = 0;
	int set_or_clear = -1;
	//struct path root;

	file_path = kmalloc(count + 1,GFP_KERNEL | __GFP_ZERO);
	if(!file_path)
		return -ENOMEM;

	/* 这个原子操作，是为了防止异步内存回收线程并发释放cold_file_stat_delete()或old_file_stat_change_to_new()
	 * 释放掉当前函数查找的文件file_stat_base后，这里还执行 set_file_stat_in_test(p_file_stat_base)，向已经
	 * 无效的内存写数据。这里先ref_count加1，它们俩只有ref_count是0才能释放掉p_file_stat_base*/	
	atomic_inc(&hot_cold_file_global_info.ref_count);

	if (copy_from_user(file_path, buffer, count)){
		ret = -EFAULT;
		goto free;
	}
	printk("%s\n",file_path);
	/*如果文件名字最后一个字符数换行符，下边open文件会失败，返回错误，此时必须形如echo -n /root/test.c 传递文件名字，禁止bash在文件名字末尾插入换行符*/
	if(file_path[count - 1] == '\n'){
		printk("echo -n debug set /mnt/ext4/dd > /proc/async_memory_reclaime/file_stat_debug_or_make_backlist\n");
		ret = -EINVAL;
		goto free;
	}

	p = file_path;
	if(!strncmp(p,"blacklist",9)){
        file_blacklist = 1;
		/*p偏移到blacklist字符串后*/
		p += 9;
	}
	else if(!strncmp(p,"debug",5)){
		file_debug = 1;
		/*p偏移到debug字符串后*/
		p += 5;
	}else{
		printk("only blacklist/debug\n");
        ret = -EINVAL;
        goto free;
	}

	while(*p && *p != 's' && *p != 'c')
			p ++;

    if(!strncmp(p,"set",3)){
         set_or_clear = 1;
	}
	else if(!strncmp(p,"clear",5)){
		set_or_clear = 0;
	}else{
		printk("only set/clear\n");
        ret = -EINVAL;
        goto free;
	}

	while(*p && *p != ' ')
		p ++;

	p ++;
	if(!strncmp(p,"global mmap",11)){
		rcu_read_lock();
		p_file_stat_base = &hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_stat_base;
		goto direct_global_file_stat;
	}else if(!strncmp(p,"global cache",12)){
		rcu_read_lock();
		p_file_stat_base = &hot_cold_file_global_info.global_file_stat.file_stat.file_stat_base;
        goto direct_global_file_stat;
	}

	while(*p && *p != '/')
			p ++;
		
	/*防止下边使用该文件时，inode被iput()并发释放了。不用，filp_open()成功后，会dget()令dentry引用计数加1，
	 *之后除非file_close，否则无法释放dentry和inode。但是，还有一个并发场景，异步内存回收线程
	 *cold_file_stat_delete()因为该file_stat一个file_area都没就，于是释放掉该file_stat结构，这种情况
	 *完全存在的。这样的话，下边执行if(file_stat_in_delete_base(p_file_stat_base))，就可能该file_stat
	 *被异步内存回收线程并发cold_file_stat_delete()释放了而访问无效内存。因此，必须加rcu_read_lock()
	 *保证这个宽限期内file_stat结构体无法被释放掉
     *
	 *又遇到新的问题了，还有一个隐藏bug。就是tiny small file转成small或normal，small转成normal文件时，
	 *老的file_stat会被直接异步rcu del。如果这个file_stat正好之前被赋值给了
	 *hot_cold_file_global_info.print_file_stat，则需要及时把hot_cold_file_global_info.print_file_stat
	 *清NULL。否则再使用这个file_stat就是非法内存访问。因此，当前函数也要判断file_stat是否在函数
	 *old_file_stat_change_to_new()老的file_stat转成新的file_stat时，老的file_stat是否是
	 *hot_cold_file_global_info.print_file_stat。
     */

	//rcu_read_lock();rcu 不能放在这里，因为filp_open()函数里会休眠，rcu_read_lock后不能休眠，要放到open后边
	/*根据传入的文件路径查找该文件，得到inode，再得到file_stat*/
#if 1
	file_temp = filp_open(p,O_RDONLY,0);
#else
	task_lock(&init_task);
	get_fs_root(init_task.fs, &root);
	task_unlock(&init_task);

	file_temp = file_open_root(&root, file_path, O_RDONLY, 0);
	path_put(&root);
#endif
	/*如果open文件成功，do_dentry_open->path_get()流程会令dentry引用计数加1。之后谁也无法再iput()释放了inode了。因此后续流程不用担心inode被释放*/
	if (IS_ERR(file_temp)){
		printk("file_open fail:%s %lld\n",file_path,(s64)file_temp);
		ret = -ENOENT;
		goto free;
	}

	rcu_read_lock();
	
	/*如果该文件file_stat被异步内存回收线程并发cold_file_stat_delete()释放了，则smp_rmb()查看p_file_stat_base此时就是SUPPORT_FILE_AREA_INIT_OR_DELETE，下边的if不成立*/
	smp_rmb();
	inode = file_inode(file_temp);
	p_file_stat_base = (struct file_stat_base *)(inode->i_mapping->rh_reserved1);//这个要放到内存屏障后

direct_global_file_stat:

	/*是支持的file_area的文件系统的文件，并且该文件并没有被异步内存回收线程并发cold_file_stat_delete()释放赋值SUPPORT_FILE_AREA_INIT_OR_DELETE*/
	if((u64)p_file_stat_base > SUPPORT_FILE_AREA_INIT_OR_DELETE){
		/* 此时这个file_stat被异步内存回收线程并发cold_file_stat_delete()释放了，将来使用时要注意。
		 * 存在一个极端并发场景，这里对print_file_stat赋值还没生效，异步内存回收线程里cold_file_stat_delete()
		 * 看到的print_file_stat还是null，然后把这个file_stat给释放了。将来打印时，却使用print_file_stat
		 * 保存的这个file_stat，打印file_area信息。但实际这个file_stat已经是无效的内存了，不能再访问。这个
		 * 并发问题很棘手!!!!!!!!!!!!!!!!!必须要确保这里先对print_file_stat赋值file_sat，然后再有异步内存
		 * 回收线程识别到print_file_stat非NULL，然后赋值NULL。最终想到的解决办法是：
		 *
		 * 该函数把file_stat赋值给print_file_stat
		 * if(!file_stat_in_delete_base(p_file_stat_base)){
		 *     hot_cold_file_global_info.print_file_stat = p_file_stat_base;
		 *     smp_mb();//读+写内存屏障
		 *     if(file_stat_in_delete_base(p_file_stat_base)){
		 *        hot_cold_file_global_info.print_file_stat = NULL; 
		 *     }
		 * }
		 *
		 * 异步内存回收的执行cold_file_stat_delete()释放该file_stat而标记print_file_stat为NULL
		 * cold_file_stat_delete()
		 * {
		 *     set_file_stat_in_delete_base(p_file_stat_base);
		 *     smp_mb();//读+写内存屏障
		 *     if(p_file_stat_base == hot_cold_file_global_info.print_file_stat)
		 *     {
		 *         hot_cold_file_global_info.print_file_stat = NULL;
		 *     }
		 *     //异步释放file_stat
	     *	   call_rcu(&p_file_stat_base_del->i_rcu, i_file_stat_callback);
		 * }
		 * 
		 * 这两个函数谁前执行完，同步都没有问题。主要看几个极端的例子
		 * 1:当前函数正执行"hot_cold_file_global_info.print_file_stat = p_file_stat_base"，
		 * cold_file_stat_delete()函数正执行"smp_mb()"和"if(p_file_stat_base == hot_cold_file_global_info.print_file_stat)"
		 * 此时，当前函数执行到if(file_stat_in_delete_base(p_file_stat_base))时，一定感知到file_stat的in_delete
		 * 状态。因为cold_file_stat_delete()函数此时一定执行过了"set_file_stat_in_delete_base(p_file_stat_base)"
		 * 和"smp_mb()"，已经保证把file_stat的in_delete状态同步给了执行当前函数的cpu。
		 *
		 * 2:当前函数正执行"smp_mb()",还没执行完，当前cpu产生了中断，卡在中断里一段时间，内存屏障没有生效。
		 * cold_file_stat_delete()函数正执行"set_file_stat_in_delete_base(p_file_stat_base)"。然后执行
		 * "if(p_file_stat_base == hot_cold_file_global_info.print_file_stat)"时，执行当前函数的cpu的"smp_mb()"
		 * 执行完成了。但是cold_file_stat_delete()函数的cpu并没有感知到hot_cold_file_global_info.print_file_stat是
		 * p_file_stat_base，"if(p_file_stat_base == hot_cold_file_global_info.print_file_stat)"不成立。但是
		 * cold_file_stat_delete()已经执行过了"set_file_stat_in_delete_base(p_file_stat_base)"和"smp_mb()"。
		 * 而当前函数执行过"smp_mb()"，一定能感知到file_stat的in_delete状态。故
		 * "if(file_stat_in_delete_base(p_file_stat_base))"成立
		 *
		 * 3：两个函数都在执行"smp_mb()"，如果当前函数先执行完，则cold_file_stat_delete() 执行完"smp_mb"后，
		 * 一定能感知到hot_cold_file_global_info.print_file_stat是p_file_stat_base，故
		 * "if(p_file_stat_base == hot_cold_file_global_info.print_file_stat)"成立。如果cold_file_stat_delete()
		 * 先执行完"smp_mb()"，则当前函数执行完"smp_mb"后，一定能感知到file_stat的in_delete状态，故
		 * "if(file_stat_in_delete_base(p_file_stat_base))"成立。
		 *
		 *  这个并发案例算是目前为止最抽象的无锁编程并发案例，通过把两个并发的函数的关键步骤写下来，一步步
		 *  推演，终于得到了好的解决方案。
		 *
		 *  无语了，上边的方案有问题了，如果该p_file_stat_base被异步内存回收线程并发释放，该p_file_stat_base
		 *  的内存就是无效内存了!!!!!!!!!!!而这片内存要是被别的进程分配成了新的file_stat，则这里的
		 *  file_stat_in_delete_base(p_file_stat_base)就是无效的判断了，因为这片内存时别的进程分配
		 *  的新的file_stat了。要想解决这个问题，必须保证执行到这里时，
		 *  1:p_file_stat_base不能被异步内存回收线程真正释放掉。
		 *  2:或者p_file_stat_base被异步内存回收线程真正释放掉了，这里要立即感知到，就不再使用这个
		 *  p_file_stat_base了。怎么感知到？有办法，在异步内存回收线程执行cold_file_stat_delete()，会先把
		 *  mapping->rh_reserved1标记SUPPORT_FILE_AREA_INIT_OR_DELETE。并且，执行到这里时，是可能保证该
		 *  文件inode和mapping一定不会被iput()被释放。只是无法保证该文件file_stat会因一个file_area都没有
		 *  而被异步内存回收线程执行cold_file_stat_delete()释放而已。
		 *
		 *  于是新的方案来了，核心还是借助rcu，在原有方案做小的改动即可
		 *
		 *
		 * rcu_read_lock();
		 * smp_rmb();
	     * p_file_stat_base = (struct file_stat_base *)(inode->i_mapping->rh_reserved1);
		 *
		 * if((u64)p_file_stat_base > SUPPORT_FILE_AREA_INIT_OR_DELETE)
		 * {
		 *     该函数把file_stat赋值给print_file_stat
		 *     if(!file_stat_in_delete_base(p_file_stat_base)){
		 *         hot_cold_file_global_info.print_file_stat = p_file_stat_base;
		 *         smp_mb();//读+写内存屏障
		 *         if(file_stat_in_delete_base(p_file_stat_base)){
		 *            hot_cold_file_global_info.print_file_stat = NULL; 
		 *         }
		 *     }
		 * }
		 * rcu_read_unlock();
		 *
		 * 异步内存回收的执行cold_file_stat_delete()释放该file_stat而标记print_file_stat为NULL
		 * cold_file_stat_delete()
		 * {
		 *     //标记mapping->rh_reserved1的delete标记
		 *     p_file_stat_base_del->mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE;
		 *	   smp_wmb();
		 *
		 *     set_file_stat_in_delete_base(p_file_stat_base);
		 *     smp_mb();//读+写内存屏障
		 *     if(p_file_stat_base == hot_cold_file_global_info.print_file_stat)
		 *     {
		 *         hot_cold_file_global_info.print_file_stat = NULL;
		 *     }
		 *     //异步释放file_stat
	     *	   call_rcu(&p_file_stat_base_del->i_rcu, i_file_stat_callback);
		 * }
         * 只要当前函数执行了rcu_read_lock，那异步内存回收线就无法再释放掉这个p_file_stat_base。
		 * 极端并发场景是，当前函数执行到rcu_read_lock(),但还没有执行完成。异步内存回收线程正
		 * 执行 cold_file_stat_delete()里的"p_file_stat_base_del->mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE",
		 * 如果当前函数先执行完rcu_read_lock，那异步内存回收线就无法再释放掉这个p_file_stat_base了，
		 * 当前函数可以放心使用它。如果"p_file_stat_base_del->mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE"
		 * 先执行完，但还没有执行后边的"smp_wmb()"，赋值没有对其他cpu生效。当前函数先执行完了rcu_read_lock，
		 * 依然跟前边一样，可以放心使用p_file_stat_base，不用担心被释放。但是异步内存回收线程一旦先执行了"smp_wmb"，
		 * 然后当前函数执行"smp_rmb"后，就一定能立即感知到inode->i_mapping->rh_reserved1是SUPPORT_FILE_AREA_INIT_OR_DELETE了，
		 * 这是smp_wmb+smp_rmb绝对保证的。然后当期函数就不会再使用这个p_file_stat_base了
		 *
		 * */ 


		 /* 如果file_stat_base被异步内存回收线程在old_file_stat_change_to_new()小文件转成大文件时被并发释放了，
		  * 因此要放防护这种并发，遇到要逃过。这个并发的处理跟file_stat_base被异步内存回收线程cold_file_stat_delete
		  * 释放file_stat的并发处理是完全一致的。这里再简单总结下：old_file_stat_change_to_new()的顺序是
		  * old_file_stat_change_to_new()
		  * {
		  *     p_file_stat_base_old->mapping->rh_reserved1 =  (unsigned long)p_file_stat_base_new; 
		  *     set_file_stat_in_replaced_file_base(p_file_stat_base);
		  *     smp_mb();
		  *     if(p_file_stat_base == hot_cold_file_global_info.print_file_stat)
		  *     {
		  *         hot_cold_file_global_info.print_file_stat = NULL;
		  *     }
		  *     //异步释放file_stat
	      *	    call_rcu(&p_file_stat_base_del->i_rcu, i_file_stat_callback);
          *
		  * }
		  *
		  * 当前函数的并发处理是
		  * rcu_read_lock();//有了rcu，就可以保证这个宽限期内，old_file_stat_change_to_new()无法真正释放掉file_stat结构
		  * simp_rmb();
		  * p_file_stat_base = (struct file_stat_base *)(inode->i_mapping->rh_reserved1);
		  * if(!file_stat_in_replaced_file_base(p_file_stat_base))
		  * {
		  *     hot_cold_file_global_info.print_file_stat = p_file_stat_base;
		  *     smp_mb();
		  *     if(file_stat_in_replaced_file_base(p_file_stat_base)){
		  *         hot_cold_file_global_info.print_file_stat = NULL;
		  *     }
		  * }
		  * rcu_read_unlock()
		  *
		  * 主要就是极端并发场景的处理，如果是下边"hot_cold_file_global_info.print_file_stat = p_file_stat_base
		  * "赋值后，异步内存回收线程才old_file_stat_change_to_new()里刚标记file_stat in_replace，但是这里马上
		  * 检测到file_stat_in_replaced_file_base(p_file_stat_base)，就会清理NULL。更详细的看上边的file_stat的delete的分析。
		  *
		  * 但是又来了一个隐藏很深的并发问题，当前函数执行到"if(!file_stat_in_replaced_file_base(p_file_stat_base))"
		  * 前，cpu产生了中断，在硬中断、软中断阻塞了很长时间。此时old_file_stat_change_to_new()里释放掉file_stat_base
		  * 结构体了，成为无效内存，当前函数再执行set_file_stat_in_test_base(p_file_stat_base)就是写无效内存了。
		  * 或者，这个已经释放了的file_stat_base很快又被其他进程分配为新的file_stat，然后当前函数执行
		  * if(file_stat_in_delete_base(p_file_stat_base))就会产生误判，因为file_stat已经不是老的了。
		  * 仔细一想，当前函数有rcu_read_lock 保护：如果p_file_stat_base已经在
		  * old_file_stat_change_to_new()里释放掉了，则该函数里的这两行代码
		  * "p_file_stat_base_old->mapping->rh_reserved1 =  (unsigned long)p_file_stat_base_new;smp_wmb"
		  * 肯定是执行过了，则当前函数执行rcu_read_lock;smp_rmb()后，执行
		  * "p_file_stat_base = (struct file_stat_base *)(inode->i_mapping->rh_reserved1)"一定是新的file_stat，
		  *  即p_file_stat_base_new。而如果当前函数rcu_read_lock先执行，则old_file_stat_change_to_new()
		  *  就无法再真正rcu del释放掉这个file_stat_base结构。当前函数下边可以放心对这个file_stat_base读写了
		  *
		  * 最后，为了100%防护这里能放心读写file_stat_base，有引入了 ref_count 原子变量，该该函数读写
		  * file_stat_base前，先加1，然后谁都不能再释放这个file_stat_base结构
		  * */
		 
		 if(!file_stat_in_delete_base(p_file_stat_base) && !file_stat_in_replaced_file_base(p_file_stat_base)){

			 /*设置文件黑名单，不扫描内存回收*/
			 if(file_blacklist){
				 if(set_or_clear){
					 set_file_stat_in_blacklist_base(p_file_stat_base);
					 printk("%s file_stat:0x%llx set_in_blacklist\n",p,(u64)p_file_stat_base);
				 }
				 else{
					 clear_file_stat_in_blacklist_base(p_file_stat_base);
					 printk("%s file_stat:0x%llx clear_in_blacklist\n",p,(u64)p_file_stat_base);
				 }

				 goto close;
			 }

			 if(file_debug){
				 WRITE_ONCE(hot_cold_file_global_info.print_file_stat, p_file_stat_base);
				 smp_mb();
				 if(file_stat_in_delete_base(p_file_stat_base) || file_stat_in_replaced_file_base(p_file_stat_base)){
					 WRITE_ONCE(hot_cold_file_global_info.print_file_stat, NULL);
				 }

				 if(set_or_clear){
					 if(file_stat_in_global_base(p_file_stat_base)){
						 /*如果inode是NULL，说明本次仅仅是echo -n debug set global cache设置调试global_file_stat，不是调试global_file_stat的file_area的文件*/
						 if(inode)
						     WRITE_ONCE(inode->i_mapping->rh_reserved2, 1);

						 printk("%s file_stat:0x%llx in_global set_in_debug\n",p,(u64)p_file_stat_base);
					 }
					 else{
						 /*设置文件in test模式*/
						 set_file_stat_in_test_base(p_file_stat_base);
						 printk("%s file_stat:0x%llx set_in_debug\n",p,(u64)p_file_stat_base);
					 }
				 }
				 else{
					 if(file_stat_in_global_base(p_file_stat_base)){
						 if(inode)
						     inode->i_mapping->rh_reserved2 = 0;

						 printk("%s file_stat:0x%llx in_global clear_in_debug\n",p,(u64)p_file_stat_base);
					 }else{
						 /*清理文件in test模式*/
						 clear_file_stat_in_test_base(p_file_stat_base);
						 printk("%s file_stat:0x%llx clear_in_debug\n",p,(u64)p_file_stat_base);
					 }
				 }
			 }
		 }else{
			 printk("invalid file_stat:0x%llx 0x%x has replace or delete!!!!!!!!\n",(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			 ret = -ENOENT;
			 goto close;
		 }
	}else{

		printk("file_stat not exist\n");
		ret = -ENOENT;
	}

close:	
	//filp_close(file_temp, NULL);
	fput(file_temp);
//err:
	rcu_read_unlock();
free:
	
	atomic_dec(&hot_cold_file_global_info.ref_count);
	if(file_path)	
		kfree(file_path);

	if(ret)
		return ret;

	return count;
}
static const struct proc_ops file_stat_debug_or_make_backlist_fops = {
	.proc_open		= file_stat_debug_or_make_backlist_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= file_stat_debug_or_make_backlist_write,
};

//enable_disable_async_memory_reclaim
static int enable_disable_async_memory_reclaim_show(struct seq_file *m, void *v)
{
	seq_printf(m, "ASYNC_MEMORY_RECLAIM_ENABLE:%d\n",test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status));
	return 0;
}
static int enable_disable_async_memory_reclaim_open(struct inode *inode, struct file *file)
{
	return single_open(file,enable_disable_async_memory_reclaim_show, NULL);
}
static ssize_t enable_disable_async_memory_reclaim_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{   
	int rc;
	unsigned int val;
	rc = kstrtouint_from_user(buffer, count, 10,&val);
	if (rc)
		return rc;

	if(val == 0)
		clear_bit_unlock(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status);
	else
		set_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status);

	return count;
}
static const struct proc_ops enable_disable_async_memory_reclaim_fops = {
	.proc_open		= enable_disable_async_memory_reclaim_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	.proc_release	= single_release,
	.proc_write		= enable_disable_async_memory_reclaim_write,
};
char *get_file_name_buf(char *file_name_path,struct file_stat_base *p_file_stat_base)
{
	struct dentry *dentry = NULL;
	struct inode *inode = NULL;

	/*需要rcu加锁，保证inode在这个宽限期内inode结构体不会被iput()释放掉而非法内存访问*/
	rcu_read_lock();
	/*必须 hlist_empty()判断文件inode是否有dentry，没有则返回true。这里通过inode和dentry获取文件名字，必须 inode->i_lock加锁 
	 *同时 增加inode和dentry的应用计数，否则可能正使用时inode和dentry被其他进程释放了*/
	if(p_file_stat_base->mapping && p_file_stat_base->mapping->host/* && !hlist_empty(&p_file_stat_base->mapping->host->i_dentry)*/){
		inode = p_file_stat_base->mapping->host;
		spin_lock(&inode->i_lock);
		/*如果inode的引用计数是0，说明inode已经在释放环节了，不能再使用了。现在发现不一定，改为hlist_empty(&inode->i_dentry)判断*/
		if(/*atomic_read(&inode->i_count) > 0*/ !hlist_empty(&inode->i_dentry)){
			dentry = hlist_entry(inode->i_dentry.first, struct dentry, d_u.d_alias);
			//__dget(dentry);------这里不再__dget,因为全程有spin_lock(&inode->i_lock)加锁
			if(dentry){
				snprintf(file_name_path,MAX_FILE_NAME_LEN - 2,"i_count:%d i_size:%lld dentry:0x%llx %s",atomic_read(&inode->i_count),inode->i_size,(u64)dentry,/*dentry->d_iname*/dentry->d_name.name);
				//snprintf(file_name_path,MAX_FILE_NAME_LEN - 2,"%s",dentry->d_name.name);
			}
			//dput(dentry);
		}else{
			snprintf(file_name_path,MAX_FILE_NAME_LEN - 2,"i_count:%d dentry:0x%llx lru_list_empty:%d",atomic_read(&inode->i_count),(u64)inode->i_dentry.first,list_empty(&inode->i_lru));
		}
		spin_unlock(&inode->i_lock);
	}
	rcu_read_unlock();

	return (dentry == NULL) ? NULL:dentry->d_iname;
}
char *get_file_name_no_lock_from_mapping(struct address_space *mapping)
{
	struct dentry *dentry = NULL;
	struct inode *inode = NULL;

	inode = mapping->host;
	if(!hlist_empty(&inode->i_dentry)){
		dentry = hlist_entry(inode->i_dentry.first, struct dentry, d_u.d_alias);
	}

	return (dentry == NULL) ? NULL:dentry->d_iname;
}
int get_file_name_match(struct file_stat_base *p_file_stat_base,char *file_name1,char *file_name2,char *file_name3)
{
	struct dentry *dentry = NULL;
	struct inode *inode = NULL;

	/*需要rcu加锁，保证inode在这个宽限期内inode结构体不会被iput()释放掉而非法内存访问*/
	rcu_read_lock();
	/*必须 hlist_empty()判断文件inode是否有dentry，没有则返回true。这里通过inode和dentry获取文件名字，必须 inode->i_lock加锁 
	 *同时 增加inode和dentry的应用计数，否则可能正使用时inode和dentry被其他进程释放了*/
	if(p_file_stat_base->mapping && p_file_stat_base->mapping->host/* && !hlist_empty(&p_file_stat_base->mapping->host->i_dentry)*/){
		inode = p_file_stat_base->mapping->host;
		spin_lock(&inode->i_lock);
		/*如果inode的引用计数是0，说明inode已经在释放环节了，不能再使用了。现在发现不一定，改为hlist_empty(&inode->i_dentry)判断*/
		if(/*atomic_read(&inode->i_count) > 0*/ !hlist_empty(&inode->i_dentry)){
			dentry = hlist_entry(inode->i_dentry.first, struct dentry, d_u.d_alias);
			//__dget(dentry);------这里不再__dget,因为全程有spin_lock(&inode->i_lock)加锁
			if(dentry){
				//snprintf(file_name_path,MAX_FILE_NAME_LEN - 2,"i_count:%d i_size:%lld dentry:0x%llx %s",atomic_read(&inode->i_count),inode->i_size,(u64)dentry,/*dentry->d_iname*/dentry->d_name.name);
				//snprintf(file_name_path,MAX_FILE_NAME_LEN - 2,"%s",dentry->d_name.name);
			}
			//dput(dentry);
		}else{
			//snprintf(file_name_path,MAX_FILE_NAME_LEN - 2,"i_count:%d dentry:0x%llx lru_list_empty:%d",atomic_read(&inode->i_count),(u64)inode->i_dentry.first,list_empty(&inode->i_lru));
		}
		spin_unlock(&inode->i_lock);
	}
	rcu_read_unlock();

	if(dentry){
		if(file_name1 && strcmp(dentry->d_iname,file_name1) == 0)
			return 1;
		else if(file_name2 && strcmp(dentry->d_iname,file_name2) == 0)
			return 1;
		else if(file_name3 && strcmp(dentry->d_iname,file_name3) == 0)
			return 1;
	}

	return 0;
}
static int print_one_list_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct seq_file *m,int is_proc_print,char *file_stat_name,struct list_head *file_stat_temp_head,struct list_head *file_stat_delete_list_head,unsigned int *file_stat_one_file_area_count,unsigned int *file_stat_many_file_area_count,unsigned int *file_stat_one_file_area_pages,unsigned int file_stat_in_list_type,unsigned int file_type,int print_file_stat_info_or_update_refault)
{
	//struct file_stat *p_file_stat;
	struct file_stat_base *p_file_stat_base;
	unsigned int all_pages = 0;
	char file_name_path[MAX_FILE_NAME_LEN];
	unsigned int scan_file_stat_count = 0;
	unsigned int reclaim_pages;
	unsigned int refault_page_dx;
	unsigned int reclaim_page_print_level = p_hot_cold_file_global->reclaim_page_print_level;
	unsigned int refault_page_print_level = p_hot_cold_file_global->refault_page_print_level;
	int file_area_hot_count;

	//printk("print_one_list_file_stat file_stat_in_list_type:%d file_type:%d\n",file_stat_in_list_type,file_type);
	/* 如果在遍历file_stat过程，p_file_stat和它在链表的下一个file_stat即p_file_stat_temp都被iput释放了，
	 * 二者都被移动到了global delete链表，然后得到p_file_stat或p_file_stat_temp在链表的下一个file_stat，
	 * 就跨链表了，会陷入死循环，因为链表头由file_stat_temp_head变为了global delete。这样就会遍历链表
	 * 时因链表头变了而陷入死循环，老问题。怎么解决？目前暂定如果遍历到global delete链表上的file_stat，
	 * 立即break，或者限制scan_file_stat_count最大值，超过则break。
	 *
	 * 还有一种并发，如果proc打印文件信息，正打印global temp链表的file_stat文件。但是异步内存回收线程
	 * 把该file_stat从global temp链表移动到global hot链表，那就又要因为遍历global temp时链表头变了而
	 * 陷入死循环。怎么办？遍历global temp链表时，如果检测到file_stat没有in global temp list状态了，break*/
	//list_for_each_entry_rcu(p_file_stat,file_stat_temp_head,hot_cold_file_list){
	list_for_each_entry_rcu(p_file_stat_base,file_stat_temp_head,hot_cold_file_list){//----------------------------从global temp等链表遍历file_stat时，要防止file_stat被iput()并发释放了
		scan_file_stat_count ++;
        
		/*在遍历global 各个file_stat链表上的file_stat、file_stat_small、file_stat_tiny_small等，如果
		 *这些file_stat被并发iput移动到delete链表，就会遇到以下问题
		 *1:因file_stat被移动到global delete链表大导致链表头变了，于是上边list_for_each_entry_rcu()循环
		 *就会因无法遍历到链表头file_stat_temp_head，无法退出而陷入死循环
		 *2：遍历到的file_stat是global delete链表头，即p_file_stat_base不是有效的file_stat，而是global 
		 *delete链表头。当上一个p_file_stat_base被移动到global delete链表尾，然后得到p_file_stat_base
		 *在链表的下一个file_stat时，新的p_file_stat_base就是global delete链表头。
         *
		 *要防护以上问题。需要判断每次的p_file_stat_base是不是glboal delete链表头，还要判断有没有
		 *delete标记，有的话说明本次遍历到的p_file_stat_base被移动到global delete链表头了。于是，
		 *立即结束遍历

		 *3：还有一种情况是，p_file_stat_base从glboal temp链表移动到了glboal middle链表，此时也要退出
		 *循环，否则会因链表头遍历而无法退出循环，陷入死循环
		 */

		/*p_file_stat_base是global delete链表头，立即退出*/
		if(&p_file_stat_base->hot_cold_file_list == file_stat_delete_list_head){
			printk("%s p_file_stat_base:0x%llx is global delete list\n",file_stat_name,(u64)p_file_stat_base);
			break;
		}
		/*file_stat被iput()delete了*/
		if(file_stat_in_delete_base(p_file_stat_base)){
			printk("%s p_file_stat_base:0x%llx delete\n",file_stat_name,(u64)p_file_stat_base);
			break;
		}
		/* 如果file_stat不在file_stat_in_list_type这个global链表上，说明file_stat被
		 * 异步内存回收线程移动到其他global 链表了，为陷入死循环必须break*/
		if(0 == (p_file_stat_base->file_stat_status & (1 << file_stat_in_list_type))){
			printk("%s p_file_stat_base:0x%llx statue:0x%x move another list\n",file_stat_name,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		}

		//p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
		/*异步内存线程打印文件信息时，只打印50个文件，防止刷屏打印*/
		if(!is_proc_print && scan_file_stat_count >= 50)
			break;

		//atomic_inc(&hot_cold_file_global_info.ref_count);
		//lock_file_stat(p_file_stat,0);
		/*这里rcu_read_lock()，如果此时inode先被iput释放了，则file_stat_in_delete(p_file_stat)返回1，将不会再执行get_file_name()使用inode
		 *获取文件名字。如果rcu_read_lock()先执行，此时inode被iput()释放，就不会真正释放掉inode结构，直到rcu_read_unlock*/
		rcu_read_lock();
		/*如果file_stat对应的文件inode释放了，file_stat被标记了delete，此时不能再使用p_file_stat->mapping，因为mapping已经释放了.
		  但执行这个函数时，必须禁止执行cold_file_stat_delete_all_file_area()释放掉file_stat!!!!!!!!!!!!!!!!!!!!*/
		smp_rmb();//内存屏障获取最新的file_stat状态
		if(0 == file_stat_in_delete_base(p_file_stat_base) || 0 == file_stat_in_replaced_file_base(p_file_stat_base)){

			/* 如果print_file_stat_info_or_update_refault是更新file_stat_refault，说明此时是cat /proc/async_memory_reclaime/async_memory_reclaime_info 
			 * 最后，执行close proc文件，此时才更新p_file_stat_base->refault_page_count_last*/
			if(UPDATE_FILE_STAT_REFAULT_COUNT == print_file_stat_info_or_update_refault){
				///p_file_stat_base->refault_page_count_last = p_file_stat_base->refault_page_count;
				goto no_print; 
			}

			if(p_file_stat_base->file_area_count > 0){
				if(p_file_stat_base->refault_page_count >= refault_page_print_level){
					*file_stat_many_file_area_count = *file_stat_many_file_area_count + 1;

					/*if(is_proc_print)
					  seq_printf(m,"1:refault_pages:%d last:%d 0x%llx 0x%llx\n",p_file_stat_base->refault_page_count,p_file_stat_base->refault_page_count_last,(u64)&p_file_stat_base->refault_page_count,(u64)&p_file_stat_base->refault_page_count_last);*/

					file_area_hot_count = -1;
					memset(file_name_path,0,sizeof(&file_name_path));
					get_file_name_buf(file_name_path,p_file_stat_base);
					all_pages += p_file_stat_base->mapping->nrpages;

					if(FILE_STAT_NORMAL == file_type){
						struct file_stat *p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
						reclaim_pages = p_file_stat->reclaim_pages;
						file_area_hot_count = p_file_stat->file_area_hot_count;
					}else if(FILE_STAT_SMALL == file_type){
						struct file_stat_small *p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
						reclaim_pages = p_file_stat_small->reclaim_pages;

					}else if(FILE_STAT_TINY_SMALL == file_type){
						struct file_stat_tiny_small *p_file_stat_tiny_small = container_of(p_file_stat_base,struct file_stat_tiny_small,file_stat_base);
						reclaim_pages = p_file_stat_tiny_small->reclaim_pages;
					}else
						BUG();

					if(reclaim_pages < reclaim_page_print_level)
						goto no_print;

					//if(is_proc_print)
					//	seq_printf(m,"2:refault_pages:%d last:%d\n",p_file_stat_base->refault_page_count,p_file_stat_base->refault_page_count_last);
					//printk("2:stat:0x%llx refault_pages:%d last:%d\n",(u64)p_file_stat_base,p_file_stat_base->refault_page_count,p_file_stat_base->refault_page_count_last);


					///refault_page_dx = p_file_stat_base->refault_page_count - p_file_stat_base->refault_page_count_last;
					smp_wmb();
					//p_file_stat_base->refault_page_count_last = p_file_stat_base->refault_page_count;
					if(is_proc_print){
						seq_printf(m,"stat:0x%llx status:0x%x max_age:%d traverse_age:%d file_areas:%d nrpages:%ld refault_pages:%d refault_pages_kswapd:%d reclaim_pages:%d mmap:%d write:%d file_area_hot_count:%d %s\n",(u64)p_file_stat_base,p_file_stat_base->file_stat_status,p_file_stat_base->recent_access_age,p_file_stat_base->recent_traverse_age,p_file_stat_base->file_area_count,p_file_stat_base->mapping->nrpages,p_file_stat_base->refault_page_count,p_file_stat_base->refault_page_count_last,reclaim_pages,mapping_mapped(p_file_stat_base->mapping),file_stat_in_writeonly_base(p_file_stat_base),file_area_hot_count,file_name_path);
						//seq_printf(m,"stat:0x%llx status:0x%x max_age:%d traverse_age:%d file_areas:%d nrpages:%ld refault_pages:%d %d refault_pages_dx:%d reclaim_pages:%d mmap:%d %s\n",(u64)p_file_stat_base,p_file_stat_base->file_stat_status,p_file_stat_base->recent_access_age,p_file_stat_base->recent_traverse_age,p_file_stat_base->file_area_count,p_file_stat_base->mapping->nrpages,p_file_stat_base->refault_page_count,p_file_stat_base->refault_page_count_last,refault_page_dx,reclaim_pages,mapping_mapped(p_file_stat_base->mapping),file_name_path);
						/*如果上边的seq_printf()打印file_stat数据时，seq_file->buf满了，不能再容纳打印数据，m->count和m->size相等，此时直接return -1，中断打印file_stat信息*/
						if(m->count == m->size){
							/*中断打印必须rcu_read_unlock()*/
							rcu_read_unlock();
							return -1;
						}
					}
					else	
						printk("stat:0x%llx status:0x%x max_age:%d traverse_age:%d file_areas:%d nrpages:%ld refault_pages:%d refault_pages_dx:%d reclaim_pages:%d mmap:%d %s\n",(u64)p_file_stat_base,p_file_stat_base->file_stat_status,p_file_stat_base->recent_access_age,p_file_stat_base->recent_traverse_age,p_file_stat_base->file_area_count,p_file_stat_base->mapping->nrpages,p_file_stat_base->refault_page_count,refault_page_dx,reclaim_pages,mapping_mapped(p_file_stat_base->mapping),file_name_path);

					//printk("3:stat:0x%llx refault_pages:%d last:%d\n",(u64)p_file_stat_base,p_file_stat_base->refault_page_count,p_file_stat_base->refault_page_count_last);
					//p_file_stat_base->refault_page_count_last = p_file_stat_base->refault_page_count;
				}
			}
			else{
				*file_stat_one_file_area_count = *file_stat_one_file_area_count + 1;
				*file_stat_one_file_area_pages = *file_stat_one_file_area_pages + p_file_stat_base->mapping->nrpages;
			}
		}

no_print:
		//unlock_file_stat(p_file_stat);
		//atomic_dec(&hot_cold_file_global_info.ref_count);
		rcu_read_unlock();

		if(need_resched())
			cond_resched();
	}

	if(!list_empty(file_stat_temp_head) && (PRINT_FILE_STAT_INFO == print_file_stat_info_or_update_refault)){
		if(is_proc_print)
			seq_printf(m,"%s file_count:%d\n\n",file_stat_name,scan_file_stat_count);
		else	
			printk("%s file_count:%d\n\n",file_stat_name,scan_file_stat_count);
	}

	return all_pages;
}
//遍历p_hot_cold_file_global各个链表上的file_stat的file_area个数及page个数
noinline int hot_cold_file_print_all_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct seq_file *m,int is_proc_print,int print_file_stat_info_or_update_refault)
{//is_proc_print:1 通过proc触发的打印   
	unsigned int file_stat_one_file_area_count = 0,file_stat_many_file_area_count = 0;
	unsigned int file_stat_one_file_area_pages = 0;
	long file_stat_pages = 0,all_pages = 0;

	//如果驱动在卸载，禁止再打印file_stat信息
	if(!test_bit(ASYNC_MEMORY_RECLAIM_ENABLE,&async_memory_reclaim_status)){
		printk("async_memory_reclaime ko is remove\n");
		return 0;
	}
	//printk("hot_cold_file_print_all_file_stat\n");
	file_stat_pages =  print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********cache file tiny small one area ",&p_hot_cold_file_global->file_stat_tiny_small_file_one_area_head,&p_hot_cold_file_global->file_stat_tiny_small_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_tiny_small_file_one_area_head_list,FILE_STAT_TINY_SMALL,print_file_stat_info_or_update_refault);
	/*如果print_one_list_file_stat()把seq_file->buf打满了，直接return，否则下边执行print_one_list_file_stat()也无法向m->buf保存file_stat信息，纯属浪费*/
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********cache file tiny small ",&p_hot_cold_file_global->file_stat_tiny_small_file_head,&p_hot_cold_file_global->file_stat_tiny_small_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_tiny_small_file_head_list,FILE_STAT_TINY_SMALL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********cache file small ",&p_hot_cold_file_global->file_stat_small_file_head,&p_hot_cold_file_global->file_stat_small_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_small_file_head_list,FILE_STAT_SMALL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********cache file hot ",&p_hot_cold_file_global->file_stat_hot_head,&p_hot_cold_file_global->file_stat_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_hot_head_list,FILE_STAT_NORMAL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********cache file temp ",&p_hot_cold_file_global->file_stat_temp_head,&p_hot_cold_file_global->file_stat_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_temp_head_list,FILE_STAT_NORMAL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********cache file middle ",&p_hot_cold_file_global->file_stat_middle_file_head,&p_hot_cold_file_global->file_stat_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_middle_file_head_list,FILE_STAT_NORMAL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages  += file_stat_pages;

	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********cache file large ",&p_hot_cold_file_global->file_stat_large_file_head,&p_hot_cold_file_global->file_stat_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_large_file_head_list,FILE_STAT_NORMAL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

    file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********cache writeonly file ",&p_hot_cold_file_global->file_stat_writeonly_file_head,&p_hot_cold_file_global->file_stat_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_writeonly_file_head_list,FILE_STAT_NORMAL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;


	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********mmap file tiny small one area ",&p_hot_cold_file_global->mmap_file_stat_tiny_small_file_one_area_head,&p_hot_cold_file_global->mmap_file_stat_tiny_small_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_tiny_small_file_one_area_head_list,FILE_STAT_TINY_SMALL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********mmap file tiny small ",&p_hot_cold_file_global->mmap_file_stat_tiny_small_file_head,&p_hot_cold_file_global->mmap_file_stat_tiny_small_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_tiny_small_file_head_list,FILE_STAT_TINY_SMALL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********mmap file small ",&p_hot_cold_file_global->mmap_file_stat_small_file_head,&p_hot_cold_file_global->mmap_file_stat_small_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_small_file_head_list,FILE_STAT_SMALL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;


	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********mmap file temp ",&p_hot_cold_file_global->mmap_file_stat_temp_head,&p_hot_cold_file_global->mmap_file_stat_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_temp_head_list,FILE_STAT_NORMAL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********mmap file large ",&p_hot_cold_file_global->mmap_file_stat_large_file_head,&p_hot_cold_file_global->mmap_file_stat_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_large_file_head_list,FILE_STAT_NORMAL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********mmap file middle ",&p_hot_cold_file_global->mmap_file_stat_middle_file_head,&p_hot_cold_file_global->mmap_file_stat_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_middle_file_head_list,FILE_STAT_NORMAL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********mmap file hot ",&p_hot_cold_file_global->mmap_file_stat_hot_head,&p_hot_cold_file_global->mmap_file_stat_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_hot_head_list,FILE_STAT_NORMAL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

	file_stat_pages = print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********mmap file mapcount ",&p_hot_cold_file_global->mmap_file_stat_mapcount_head,&p_hot_cold_file_global->mmap_file_stat_delete_head,&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_mapcount_file_area_list,FILE_STAT_NORMAL,print_file_stat_info_or_update_refault);
	if(file_stat_pages < 0)
		return 0;
	all_pages += file_stat_pages;

	if(PRINT_FILE_STAT_INFO == print_file_stat_info_or_update_refault){
		if(is_proc_print)
			seq_printf(m,"file_stat_one_file_area_count:%d pages:%d  file_stat_many_file_area_count:%d all_pages:%ld\n",file_stat_one_file_area_count,file_stat_one_file_area_pages,file_stat_many_file_area_count,all_pages);
		else	
			printk("file_stat_one_file_area_count:%d pages:%d  file_stat_many_file_area_count:%d all_pages:%ld\n",file_stat_one_file_area_count,file_stat_one_file_area_pages,file_stat_many_file_area_count,all_pages);
	}

	return all_pages;
}
noinline void printk_shrink_param(struct hot_cold_file_global *p_hot_cold_file_global,struct seq_file *m,int is_proc_print)
{
	struct hot_cold_file_shrink_counter *p = &p_hot_cold_file_global->hot_cold_file_shrink_counter;
	struct mmap_file_shrink_counter *mp = &p_hot_cold_file_global->mmap_file_shrink_counter;
	struct reclaim_pages_counter *rp = &p_hot_cold_file_global->reclaim_pages_counter_cache;
	struct reclaim_pages_counter *mrp = &p_hot_cold_file_global->reclaim_pages_counter_mmap;
	if(is_proc_print){

		seq_printf(m,"\n\n********cache file********\n");
		seq_printf(m,"global_cache_refault:%d refault_pages_kswapd:%d reclaim_pages tiny_small:%d small:%d temp:%d middle:%d large:%d writeonly:%d global:%d scan_cold_file_area_count_from_temp:%d scan_read_file_area_count_from_temp:%d scan_ahead_file_area_count_from_temp:%d scan_cold_file_area_count_from_warm:%d scan_read_file_area_count_from_warm:%d scan_ahead_file_area_count_from_warm:%d scan_file_area_count_from_warm:%d scan_cold_file_area_count_from_mmap_file:%d file_area_hot_to_warm_from_hot_file:%d free_pages_from_mmap_file:%d cache_file_stat_get_file_area_fail_count:%d mmap_file_stat_get_file_area_from_cache_count:%d\n",p_hot_cold_file_global->global_file_stat.file_stat.file_stat_base.refault_page_count,p_hot_cold_file_global->global_file_stat.file_stat.file_stat_base.refault_page_count_last,rp->tiny_small_file_stat_reclaim_pages,rp->small_file_stat_reclaim_pages,rp->temp_file_stat_reclaim_pages,rp->middle_file_stat_reclaim_pages,rp->large_file_stat_reclaim_pages,rp->writeonly_file_stat_reclaim_pages,rp->global_file_stat_reclaim_pages,p->scan_cold_file_area_count_from_temp,p->scan_read_file_area_count_from_temp,p->scan_ahead_file_area_count_from_temp,p->scan_cold_file_area_count_from_warm,p->scan_read_file_area_count_from_warm,p->scan_ahead_file_area_count_from_warm,p->scan_file_area_count_from_warm,p->scan_cold_file_area_count_from_mmap_file,p->file_area_hot_to_warm_from_hot_file,p->free_pages_from_mmap_file,p->cache_file_stat_get_file_area_fail_count,p->mmap_file_stat_get_file_area_from_cache_count);

		seq_printf(m,"\ntemp_to_warm_file_area_count:%d warm_to_temp_file_area_count:%d del_file_area_count:%d del_file_stat_count:%d writeback_count:%d dirty_count:%d del_zero_file_area_file_stat_count:%d scan_zero_file_area_file_stat_count:%d file_area_refault_to_warm_list_count:%d file_area_hot_to_warm_list_count:%d file_area_free_count_from_free_list:%d temp_to_hot_file_area_count:%d warm_to_hot_file_area_count:%d scan_file_area_count:%d scan_file_stat_count:%d scan_delete_file_stat_count:%d\n",p->temp_to_warm_file_area_count,p->warm_to_temp_file_area_count,p->del_file_area_count,p->del_file_stat_count,p->writeback_count,p->dirty_count,p->del_zero_file_area_file_stat_count,p->scan_zero_file_area_file_stat_count,p->file_area_refault_to_warm_list_count,p->file_area_hot_to_warm_list_count,p->file_area_free_count_from_free_list,p->temp_to_hot_file_area_count,p->warm_to_hot_file_area_count,p->scan_file_area_count,p->scan_file_stat_count,p->scan_delete_file_stat_count);

		seq_printf(m,"\n\n********mmap file********\n");
#if 0		
		seq_printf(m,"isolate_lru_pages_from_warm:%d scan_cold_file_area_count_from_warm:%d isolate_lru_pages_from_temp:%d scan_cold_file_area_count_from_temp:%d temp_to_warm_file_area_count:%d scan_file_area_count:%d scan_file_stat_count:%d warm_to_temp_file_area_count:%d mmap_free_pages_count:%d scan_file_area_count_from_cache_file:%d scan_cold_file_area_count_from_cache_file:%d refault_to_warm_file_area_count:%d find_cache_page_count_from_mmap_file:%d\n",mp->isolate_lru_pages_from_warm,mp->scan_cold_file_area_count_from_warm,mp->isolate_lru_pages_from_temp,mp->scan_cold_file_area_count_from_temp,mp->temp_to_warm_file_area_count,mp->scan_file_area_count,mp->scan_file_stat_count,mp->warm_to_temp_file_area_count,mp->mmap_free_pages_count,mp->scan_file_area_count_from_cache_file,mp->scan_cold_file_area_count_from_cache_file,mp->refault_to_warm_file_area_count,mp->find_cache_page_count_from_mmap_file);

		seq_printf(m,"\nfile_area_hot_to_warm_list_count:%d del_file_stat_count:%d del_file_area_count:%d writeback_count:%d dirty_count:%d scan_mapcount_file_area_count:%d scan_hot_file_area_count:%d free_pages_from_cache_file:%d mapcount_to_warm_file_area_count:%d hot_to_warm_file_area_count:%d check_refault_file_area_count:%d free_file_area_count:%d temp_to_temp_head_file_area_count:%d scan_file_area_count_file_move_from_cache:%d mapcount_to_temp_file_area_count_from_mapcount_file:%d hot_to_temp_file_area_count_from_hot_file:%d\n",mp->file_area_hot_to_warm_list_count,mp->del_file_stat_count,mp->del_file_area_count,mp->writeback_count,mp->dirty_count,mp->scan_mapcount_file_area_count,mp->scan_hot_file_area_count,mp->free_pages_from_cache_file,mp->mapcount_to_warm_file_area_count,mp->hot_to_warm_file_area_count,mp->check_refault_file_area_count,mp->free_file_area_count,mp->temp_to_temp_head_file_area_count,mp->scan_file_area_count_file_move_from_cache,mp->mapcount_to_temp_file_area_count_from_mapcount_file,mp->hot_to_temp_file_area_count_from_hot_file);
#else
		seq_printf(m,"global_mmap_refault:%d refault_pages_kswapd:%d reclaim_pages tiny_small:%d small:%d temp:%d middle:%d large:%d writeonly:%d global:%d scan_cold_file_area_count_from_temp:%d scan_read_file_area_count_from_temp:%d scan_ahead_file_area_count_from_temp:%d scan_cold_file_area_count_from_warm:%d scan_read_file_area_count_from_warm:%d scan_ahead_file_area_count_from_warm:%d scan_file_area_count_from_warm:%d scan_cold_file_area_count_from_mmap_file:%d file_area_hot_to_warm_from_hot_file:%d cache_file_stat_get_file_area_fail_count:%d mmap_file_stat_get_file_area_from_cache_count:%d\n",p_hot_cold_file_global->global_mmap_file_stat.file_stat.file_stat_base.refault_page_count,p_hot_cold_file_global->global_mmap_file_stat.file_stat.file_stat_base.refault_page_count_last,mrp->tiny_small_file_stat_reclaim_pages,mrp->small_file_stat_reclaim_pages,mrp->temp_file_stat_reclaim_pages,mrp->middle_file_stat_reclaim_pages,mrp->large_file_stat_reclaim_pages,mrp->writeonly_file_stat_reclaim_pages,mrp->global_file_stat_reclaim_pages,mp->scan_cold_file_area_count_from_temp,mp->scan_read_file_area_count_from_temp,mp->scan_ahead_file_area_count_from_temp,mp->scan_cold_file_area_count_from_warm,mp->scan_read_file_area_count_from_warm,mp->scan_ahead_file_area_count_from_warm,mp->scan_file_area_count_from_warm,mp->scan_cold_file_area_count_from_mmap_file,mp->file_area_hot_to_warm_from_hot_file,mp->cache_file_stat_get_file_area_fail_count,mp->mmap_file_stat_get_file_area_from_cache_count);

		seq_printf(m,"\ntemp_to_warm_file_area_count:%d warm_to_temp_file_area_count:%d del_file_area_count:%d del_file_stat_count:%d writeback_count:%d dirty_count:%d del_zero_file_area_file_stat_count:%d scan_zero_file_area_file_stat_count:%d file_area_refault_to_warm_list_count:%d file_area_hot_to_warm_list_count:%d file_area_free_count_from_free_list:%d temp_to_hot_file_area_count:%d warm_to_hot_file_area_count:%d scan_file_area_count:%d scan_file_stat_count:%d scan_delete_file_stat_count:%d scan_mapcount_file_area_count:%d\n",mp->temp_to_warm_file_area_count,mp->warm_to_temp_file_area_count,mp->del_file_area_count,mp->del_file_stat_count,mp->writeback_count,mp->dirty_count,mp->del_zero_file_area_file_stat_count,mp->scan_zero_file_area_file_stat_count,mp->file_area_refault_to_warm_list_count,mp->file_area_hot_to_warm_list_count,mp->file_area_free_count_from_free_list,mp->temp_to_hot_file_area_count,mp->warm_to_hot_file_area_count,mp->scan_file_area_count,mp->scan_file_stat_count,mp->scan_delete_file_stat_count,mp->scan_mapcount_file_area_count);
#endif		

		seq_printf(m,"\n\n********global********\n");
		seq_printf(m,"0x%llx global_age:%d < file_area_refault_file:%ld kswapd_file_area_refault_file:%ld > cold_file_area_delete_count:%d scan_exit_file_area_count:%ld direct_reclaim_pages_form_writeonly_file:%ld scan_zero_page_file_area_count:%ld warm_list_file_area_up_count:%ld warm_list_file_area_to_writeonly_list_count:%ld  warm_list_file_area_to_writeonly_list_count_cold:%ld  file_warm_list_file_area_up_count:%d %d file_warm_list_file_area_down_count:%d %d file_stat_count:%d mmap_file_stat_count:%d file_stat_hot:%d file_stat_large_count:%d read_file_area_count_in_reclaim:%d free_pages:%ld free_mmap_pages:%ld check_refault_file_area_count:%ld check_mmap_refault_file_area_count:%ld  kswapd_free_page_count:%ld async_thread_free_page_count:%ld\n",(u64)p_hot_cold_file_global,p_hot_cold_file_global->global_age,p_hot_cold_file_global->file_area_refault_file,p_hot_cold_file_global->kswapd_file_area_refault_file,p_hot_cold_file_global->cold_file_area_delete_count,p_hot_cold_file_global->scan_exit_file_area_count,p_hot_cold_file_global->direct_reclaim_pages_form_writeonly_file,p_hot_cold_file_global->scan_zero_page_file_area_count,p_hot_cold_file_global->warm_list_file_area_up_count,p_hot_cold_file_global->warm_list_file_area_to_writeonly_list_count,p_hot_cold_file_global->warm_list_file_area_to_writeonly_list_count_cold,p_hot_cold_file_global->cache_file_warm_list_file_area_up_count,p_hot_cold_file_global->mmap_file_warm_list_file_area_up_count,p_hot_cold_file_global->cache_file_warm_list_file_area_down_count,p_hot_cold_file_global->mmap_file_warm_list_file_area_down_count,p_hot_cold_file_global->file_stat_count,p_hot_cold_file_global->mmap_file_stat_count,p_hot_cold_file_global->file_stat_hot_count,p_hot_cold_file_global->file_stat_large_count,p_hot_cold_file_global->read_file_area_count_in_reclaim,p_hot_cold_file_global->free_pages,p_hot_cold_file_global->free_mmap_pages,p_hot_cold_file_global->check_refault_file_area_count,p_hot_cold_file_global->check_mmap_refault_file_area_count,p_hot_cold_file_global->kswapd_free_page_count,p_hot_cold_file_global->async_thread_free_page_count);
	}else{
		printk("********cache file********\n");
		printk("scan_cold_file_area_count_from_temp:%d scan_read_file_area_count_from_temp:%d scan_ahead_file_area_count_from_temp:%d scan_cold_file_area_count_from_warm:%d scan_read_file_area_count_from_warm:%d scan_ahead_file_area_count_from_warm:%d scan_file_area_count_from_warm:%d scan_cold_file_area_count_from_mmap_file:%d file_area_hot_to_warm_from_hot_file:%d free_pages_from_mmap_file:%d \n",p->scan_cold_file_area_count_from_temp,p->scan_read_file_area_count_from_temp,p->scan_ahead_file_area_count_from_temp,p->scan_cold_file_area_count_from_warm,p->scan_read_file_area_count_from_warm,p->scan_ahead_file_area_count_from_warm,p->scan_file_area_count_from_warm,p->scan_cold_file_area_count_from_mmap_file,p->file_area_hot_to_warm_from_hot_file,p->free_pages_from_mmap_file);

		printk("temp_to_warm_file_area_count:%d warm_to_temp_file_area_count:%d del_file_area_count:%d del_file_stat_count:%d writeback_count:%d dirty_count:%d del_zero_file_area_file_stat_count:%d scan_zero_file_area_file_stat_count:%d file_area_refault_to_warm_list_count:%d file_area_hot_to_warm_list_count:%d file_area_free_count_from_free_list:%d temp_to_hot_file_area_count:%d warm_to_hot_file_area_count:%d scan_file_area_count:%d scan_file_stat_count:%d scan_delete_file_stat_count:%d\n",p->temp_to_warm_file_area_count,p->warm_to_temp_file_area_count,p->del_file_area_count,p->del_file_stat_count,p->writeback_count,p->dirty_count,p->del_zero_file_area_file_stat_count,p->scan_zero_file_area_file_stat_count,p->file_area_refault_to_warm_list_count,p->file_area_hot_to_warm_list_count,p->file_area_free_count_from_free_list,p->temp_to_hot_file_area_count,p->warm_to_hot_file_area_count,p->scan_file_area_count,p->scan_file_stat_count,p->scan_delete_file_stat_count);

		printk("********mmap file********\n");
#if 0		
		printk("isolate_lru_pages_from_warm:%d scan_cold_file_area_count_from_warm:%d isolate_lru_pages_from_temp:%d scan_cold_file_area_count_from_temp:%d temp_to_warm_file_area_count:%d scan_file_area_count:%d scan_file_stat_count:%d warm_to_temp_file_area_count:%d mmap_free_pages_count:%d scan_file_area_count_from_cache_file:%d scan_cold_file_area_count_from_cache_file:%d refault_to_warm_file_area_count:%d find_cache_page_count_from_mmap_file:%d\n",mp->isolate_lru_pages_from_warm,mp->scan_cold_file_area_count_from_warm,mp->isolate_lru_pages_from_temp,mp->scan_cold_file_area_count_from_temp,mp->temp_to_warm_file_area_count,mp->scan_file_area_count,mp->scan_file_stat_count,mp->warm_to_temp_file_area_count,mp->mmap_free_pages_count,mp->scan_file_area_count_from_cache_file,mp->scan_cold_file_area_count_from_cache_file,mp->refault_to_warm_file_area_count,mp->find_cache_page_count_from_mmap_file);

		printk("file_area_hot_to_warm_list_count:%d del_file_stat_count:%d del_file_area_count:%d writeback_count:%d dirty_count:%d scan_mapcount_file_area_count:%d scan_hot_file_area_count:%d free_pages_from_cache_file:%d mapcount_to_warm_file_area_count:%d hot_to_warm_file_area_count:%d check_refault_file_area_count:%d free_file_area_count:%d temp_to_temp_head_file_area_count:%d scan_file_area_count_file_move_from_cache:%d mapcount_to_temp_file_area_count_from_mapcount_file:%d hot_to_temp_file_area_count_from_hot_file:%d\n",mp->file_area_hot_to_warm_list_count,mp->del_file_stat_count,mp->del_file_area_count,mp->writeback_count,mp->dirty_count,mp->scan_mapcount_file_area_count,mp->scan_hot_file_area_count,mp->free_pages_from_cache_file,mp->mapcount_to_warm_file_area_count,mp->hot_to_warm_file_area_count,mp->check_refault_file_area_count,mp->free_file_area_count,mp->temp_to_temp_head_file_area_count,mp->scan_file_area_count_file_move_from_cache,mp->mapcount_to_temp_file_area_count_from_mapcount_file,mp->hot_to_temp_file_area_count_from_hot_file);
#else
		printk("scan_cold_file_area_count_from_temp:%d scan_read_file_area_count_from_temp:%d scan_ahead_file_area_count_from_temp:%d scan_cold_file_area_count_from_warm:%d scan_read_file_area_count_from_warm:%d scan_ahead_file_area_count_from_warm:%d scan_file_area_count_from_warm:%d scan_cold_file_area_count_from_mmap_file:%d file_area_hot_to_warm_from_hot_file:%d free_pages_from_mmap_file:%d \n",mp->scan_cold_file_area_count_from_temp,mp->scan_read_file_area_count_from_temp,mp->scan_ahead_file_area_count_from_temp,mp->scan_cold_file_area_count_from_warm,mp->scan_read_file_area_count_from_warm,mp->scan_ahead_file_area_count_from_warm,mp->scan_file_area_count_from_warm,mp->scan_cold_file_area_count_from_mmap_file,mp->file_area_hot_to_warm_from_hot_file,mp->free_pages_from_mmap_file);

		printk("temp_to_warm_file_area_count:%d warm_to_temp_file_area_count:%d del_file_area_count:%d del_file_stat_count:%d writeback_count:%d dirty_count:%d del_zero_file_area_file_stat_count:%d scan_zero_file_area_file_stat_count:%d file_area_refault_to_warm_list_count:%d file_area_hot_to_warm_list_count:%d file_area_free_count_from_free_list:%d temp_to_hot_file_area_count:%d warm_to_hot_file_area_count:%d scan_file_area_count:%d scan_file_stat_count:%d scan_delete_file_stat_count:%d scan_mapcount_file_area_count:%d\n",mp->temp_to_warm_file_area_count,mp->warm_to_temp_file_area_count,mp->del_file_area_count,mp->del_file_stat_count,mp->writeback_count,mp->dirty_count,mp->del_zero_file_area_file_stat_count,mp->scan_zero_file_area_file_stat_count,mp->file_area_refault_to_warm_list_count,mp->file_area_hot_to_warm_list_count,mp->file_area_free_count_from_free_list,mp->temp_to_hot_file_area_count,mp->warm_to_hot_file_area_count,mp->scan_file_area_count,mp->scan_file_stat_count,mp->scan_delete_file_stat_count,mp->scan_mapcount_file_area_count);
#endif		
		printk("********global********\n");
		printk("0x%llx global_age:%d memory_pressure_level:%d file_stat_count:%d mmap_file_stat_count:%d file_stat_hot:%d file_stat_zero_file_area:%d file_stat_large_count:%d update_file_area_free_list_count:%ld file_area_refault_file:%ld kswapd_file_area_refault_file:%ld update_file_area_warm_list_count:%ld update_file_area_temp_list_count:%ld update_file_area_other_list_count:%ld update_file_area_move_to_head_count:%ld free_pages:%ld free_mmap_pages:%ld check_refault_file_area_count:%ld check_refault_file_area_kswapd_count:%ld check_mmap_refault_file_area_count:%ld tiny_small_file_stat_to_one_area_count:%ld file_stat_tiny_small_one_area_move_tail_count:%ld %ld kswapd_free_page_count:%ld async_thread_free_page_count:%ld\n",(u64)p_hot_cold_file_global,p_hot_cold_file_global->global_age,p_hot_cold_file_global->memory_pressure_level,p_hot_cold_file_global->file_stat_count,p_hot_cold_file_global->mmap_file_stat_count,p_hot_cold_file_global->file_stat_hot_count,p_hot_cold_file_global->file_stat_count_zero_file_area,p_hot_cold_file_global->file_stat_large_count,p_hot_cold_file_global->update_file_area_free_list_count,p_hot_cold_file_global->file_area_refault_file,p_hot_cold_file_global->kswapd_file_area_refault_file,p_hot_cold_file_global->update_file_area_warm_list_count,p_hot_cold_file_global->update_file_area_temp_list_count,p_hot_cold_file_global->update_file_area_other_list_count,p_hot_cold_file_global->update_file_area_move_to_head_count,p_hot_cold_file_global->free_pages,p_hot_cold_file_global->free_mmap_pages,p_hot_cold_file_global->check_refault_file_area_count,p_hot_cold_file_global->check_refault_file_area_kswapd_count,p_hot_cold_file_global->check_mmap_refault_file_area_count,p_hot_cold_file_global->tiny_small_file_stat_to_one_area_count,p_hot_cold_file_global->file_stat_tiny_small_one_area_move_tail_count,p_hot_cold_file_global->file_stat_tiny_small_move_tail_count ,p_hot_cold_file_global->kswapd_free_page_count,p_hot_cold_file_global->async_thread_free_page_count);
	}
}
static int async_memory_reclaime_info_show(struct seq_file *m, void *v)
{
	hot_cold_file_print_all_file_stat(&hot_cold_file_global_info,m,1,PRINT_FILE_STAT_INFO);
	printk_shrink_param(&hot_cold_file_global_info,m,1);
	return 0;
}
static int async_memory_reclaime_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, async_memory_reclaime_info_show, NULL);
}
static int async_memory_reclaime_info_release(struct inode *inode, struct file *file)
{
	hot_cold_file_print_all_file_stat(&hot_cold_file_global_info,NULL,1,UPDATE_FILE_STAT_REFAULT_COUNT);
	single_release(inode,file);
	return 0;
}
static const struct proc_ops async_memory_reclaime_info_fops = {
	.proc_open		= async_memory_reclaime_info_open,
	.proc_read		= seq_read,
	.proc_lseek     = seq_lseek,
	//.proc_release	= single_release,
	.proc_release	= async_memory_reclaime_info_release,
};

int hot_cold_file_proc_init(struct hot_cold_file_global *p_hot_cold_file_global)
{
	struct proc_dir_entry *p,*hot_cold_file_proc_root;

	hot_cold_file_proc_root = proc_mkdir("async_memory_reclaime", NULL);
	if(!hot_cold_file_proc_root)
		return -1;

	//proc_create("allow_dio", S_IRUGO | S_IWUSR, hot_cold_file_proc_root, &adio_fops);
	p_hot_cold_file_global->hot_cold_file_proc_root = hot_cold_file_proc_root;
	p = proc_create("file_area_hot_to_temp_age_dx", S_IRUGO | S_IWUSR, hot_cold_file_proc_root, &file_area_hot_to_temp_age_dx_fops);
	if (!p){
		printk("proc_create file_area_hot_to_temp_age_dx fail\n");
		return -1;
	}
	p = proc_create("file_area_refault_to_temp_age_dx", S_IRUGO | S_IWUSR, hot_cold_file_proc_root, &file_area_refault_to_temp_age_dx_fops);
	if (!p){
		printk("proc_create file_area_refault_to_temp_age_dx fail\n");
		return -1;
	}
	p = proc_create("file_area_temp_to_cold_age_dx", S_IRUGO | S_IWUSR, hot_cold_file_proc_root, &file_area_temp_to_cold_age_dx_fops);
	if (!p){
		printk("proc_create file_area_temp_to_cold_age_dx fail\n");
		return -1;
	}
	p = proc_create("file_area_free_age_dx", S_IRUGO | S_IWUSR, hot_cold_file_proc_root, &file_area_free_age_dx_fops);
	if (!p){
		printk("proc_create file_area_free_age_dx fail\n");
		return -1;
	}
	p = proc_create("file_stat_delete_age_dx", S_IRUGO | S_IWUSR, hot_cold_file_proc_root, &file_stat_delete_age_dx_fops);
	if (!p){
		printk("proc_create file_stat_delete_age_dx fail\n");
		return -1;
	}
	p = proc_create("global_age_period", S_IRUGO | S_IWUSR, hot_cold_file_proc_root, &global_age_period_fops);
	if (!p){
		printk("proc_create global_age_period fail\n");
		return -1;
	}
	p = proc_create("nr_pages_level", S_IRUGO | S_IWUSR, hot_cold_file_proc_root, &nr_pages_level_fops);
	if (!p){
		printk("proc_create nr_pages_level fail\n");
		return -1;
	}

	p = proc_create("open_print", S_IRUGO | S_IWUSR, hot_cold_file_proc_root, &open_print_fops);
	if (!p){
		printk("proc_create open_print fail\n");
		return -1;
	}
	p = proc_create("open_print_important", S_IRUGO | S_IWUSR, hot_cold_file_proc_root, &open_print_important_fops);
	if (!p){
		printk("open_print_important fail\n");
		return -1;
	}

#if 0	
	p = proc_create("async_drop_caches", S_IWUSR, hot_cold_file_proc_root, &async_drop_caches_fops);
	if (!p){
		printk("proc_create open_print fail\n");
		return -1;
	}
#endif
	//p = proc_create_single("async_memory_reclaime_info", S_IRUGO, hot_cold_file_proc_root,async_memory_reclaime_info_show);
	p = proc_create("async_memory_reclaime_info", S_IRUGO, hot_cold_file_proc_root,&async_memory_reclaime_info_fops);
	if (!p){
		printk("proc_create async_memory_reclaime_info fail\n");
		return -1;
	}

	p = proc_create("enable_disable_async_memory_reclaim", S_IRUGO | S_IWUSR, hot_cold_file_proc_root,&enable_disable_async_memory_reclaim_fops);
	if (!p){
		printk("proc_create enable_disable_async_memory_reclaim fail\n");
		return -1;
	}

	p = proc_create("file_stat_debug_or_make_backlist", S_IRUGO | S_IWUSR, hot_cold_file_proc_root,&file_stat_debug_or_make_backlist_fops);
	if (!p){
		printk("file_stat_debug_or_make_backlist fail\n");
		return -1;
	}

	p = proc_create("refault_file_area_scan_dx", S_IRUGO | S_IWUSR, hot_cold_file_proc_root,&refault_file_area_scan_dx_fops);
	if (!p){
		printk("refault_file_area_scan_dx fail\n");
		return -1;
	}
	
	p = proc_create("reclaim_page_print_level", S_IRUGO | S_IWUSR, hot_cold_file_proc_root,&reclaim_page_print_level_fops);
	if (!p){
		printk("reclaim_page_print_level fail\n");
		return -1;
	}

	p = proc_create("refault_page_print_level", S_IRUGO | S_IWUSR, hot_cold_file_proc_root,&refault_page_print_level_fops);
	if (!p){
		printk("refault_page_print_level fail\n");
		return -1;
	}

	p = proc_create("writeonly_file_age_dx", S_IRUGO | S_IWUSR, hot_cold_file_proc_root,&writeonly_file_age_dx_fops);
	if (!p){
		printk("writeonly_file_age_dx fail\n");
		return -1;
	}

	p = proc_create("file_area_reclaim_read_age_dx", S_IRUGO | S_IWUSR, hot_cold_file_proc_root,&file_area_reclaim_read_age_dx_fops);
	if (!p){
		printk("file_area_reclaim_read_age_dx fail\n");
		return -1;
	}

	return 0;
}
int hot_cold_file_proc_exit(struct hot_cold_file_global *p_hot_cold_file_global)
{
	//"file_area_hot_to_temp_age_dx"节点不存在也不会crash，自身做了防护
	remove_proc_entry("file_area_hot_to_temp_age_dx",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("file_area_refault_to_temp_age_dx",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("file_area_temp_to_cold_age_dx",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("file_area_free_age_dx",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("file_stat_delete_age_dx",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("global_age_period",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("nr_pages_level",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("open_print",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("open_print_important",p_hot_cold_file_global->hot_cold_file_proc_root);

	remove_proc_entry("async_memory_reclaime_info",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("async_drop_caches",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("enable_disable_async_memory_reclaim",p_hot_cold_file_global->hot_cold_file_proc_root);
	
	remove_proc_entry("file_stat_debug_or_make_backlist",p_hot_cold_file_global->hot_cold_file_proc_root);
	
	remove_proc_entry("refault_file_area_scan_dx",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("reclaim_page_print_level",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("refault_page_print_level",p_hot_cold_file_global->hot_cold_file_proc_root);

	remove_proc_entry("writeonly_file_age_dx",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("file_area_reclaim_read_age_dx",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("async_memory_reclaime",NULL);
	return 0;
}
static void global_file_stat_init(void)
{
	memset(&hot_cold_file_global_info.global_file_stat,0,sizeof(struct global_file_stat));
	memset(&hot_cold_file_global_info.global_mmap_file_stat,0,sizeof(struct global_file_stat));
    
    file_stat_base_struct_init(&hot_cold_file_global_info.global_file_stat.file_stat.file_stat_base,1);
    file_stat_base_struct_init(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_stat_base,0);

	/*cache global_file_stat*/
	//INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_stat.file_stat_base.file_area_temp);
	//spin_lock_init(&hot_cold_file_global_info.global_file_stat.file_stat.file_stat_base.file_stat_lock);
    set_file_stat_in_global_base(&hot_cold_file_global_info.global_file_stat.file_stat.file_stat_base);

	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_stat.file_area_hot);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_stat.file_area_warm);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_stat.file_area_warm_hot);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_stat.file_area_warm_cold);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_stat.file_area_writeonly_or_cold);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_stat.file_area_free);
	//INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_stat.file_area_refault);

	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_area_warm_middle);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_area_warm_middle_hot);
	//INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_area_warm_cold);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_area_mapcount);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_area_refault);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_area_delete_list);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.file_area_delete_list_temp);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.zero_page_file_area_list);
	spin_lock_init(&hot_cold_file_global_info.global_file_stat.file_area_delete_lock);



	/*mmap global_mmap_file_stat*/
	//INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_stat_base.file_area_temp);
	//spin_lock_init(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_stat_base.file_stat_lock);
    set_file_stat_in_global_base(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_stat_base);

	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_area_hot);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_area_warm);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_area_warm_hot);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_area_warm_cold);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_area_writeonly_or_cold);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_area_free);
	//INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_area_refault);

	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_area_warm_middle);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_area_warm_middle_hot);
	//INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_area_warm_cold);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_area_mapcount);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_area_refault);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_area_delete_list);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.file_area_delete_list_temp);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.zero_page_file_area_list);
	spin_lock_init(&hot_cold_file_global_info.global_mmap_file_stat.file_area_delete_lock);
}
#define PROTECT_SIZE 100
#define KPROBE_BUF_SIZE 2097152
char kprobe_buf[KPROBE_BUF_SIZE + PROTECT_SIZE];
int  kprobe_buf_count;
static void wakeup_kswapd_handler_post(struct kprobe *p, struct pt_regs *regs,
		unsigned long flags)
{
	int i;
	int	len;
	pg_data_t *pgdat;
	struct zone *zone = (struct zone *)(regs->di);
	const char *wakeup_zone_name = zone->name;

	for_each_online_pgdat(pgdat){
		for (i = 0; i < MAX_NR_ZONES - 1; i++) {
			zone = &pgdat->node_zones[i];  
			/*空zone跳过*/
			if(!populated_zone(zone)) 
				continue;

			if(kprobe_buf_count > KPROBE_BUF_SIZE){
				memset(kprobe_buf,0,sizeof(kprobe_buf));
				kprobe_buf_count = 0;
			}

			/*即便kprobe_buf_count等于KPROBE_BUF_SIZE，但是snprintf(...PROTECT_SIZE-20)限制了此次snprintf保存的字符串长度也不会超过PROTECT_SIZE，
			 *自然不会超出kprobe_buf数组大小(KPROBE_BUF_SIZE+PROTECT_SIZE)*/
			len = snprintf(kprobe_buf + kprobe_buf_count,PROTECT_SIZE - 20, "%s %s free:%ld high:%ld %ld %ld \n",wakeup_zone_name,zone->name,zone_page_state(zone, NR_FREE_PAGES),high_wmark_pages(zone),low_wmark_pages(zone),min_wmark_pages(zone));
			printk("%s",kprobe_buf + kprobe_buf_count);
			kprobe_buf_count += len;

			if(strlen(wakeup_zone_name))
				wakeup_zone_name = "";
		}
	}
}
static struct kprobe kp_wakeup_kswapd = {
	.symbol_name    = "wakeup_kswapd",
	.post_handler   = wakeup_kswapd_handler_post,
};
static int __init hot_cold_file_init(void)
{
	int i;
	hot_cold_file_global_info.file_area_cachep = kmem_cache_create("file_area",sizeof(struct file_area),0,0,NULL);
	
	hot_cold_file_global_info.file_stat_cachep = kmem_cache_create("file_stat",sizeof(struct file_stat),0,0,NULL);
	hot_cold_file_global_info.file_stat_small_cachep = kmem_cache_create("small_file_stat",sizeof(struct file_stat_small),0,0,NULL);
	hot_cold_file_global_info.file_stat_tiny_small_cachep = kmem_cache_create("small_file_tiny_stat",sizeof(struct file_stat_tiny_small),0,0,NULL);

	/*if(!hot_cold_file_global_info.file_stat_cachep || !hot_cold_file_global_info.file_area_cachep || hot_cold_file_global_info.file_stat_small_cachep){
		printk("%s slab 0x%llx 0x%llx error\n",__func__,(u64)hot_cold_file_global_info.file_stat_cachep,(u64)hot_cold_file_global_info.file_area_cachep);
		return -1;
	}*/

	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_hot_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_temp_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_large_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_middle_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_writeonly_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.cold_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_delete_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_small_delete_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_tiny_small_delete_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_zero_file_area_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_small_zero_file_area_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_tiny_small_zero_file_area_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_small_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_tiny_small_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_tiny_small_file_one_area_head);

	INIT_LIST_HEAD(&hot_cold_file_global_info.drop_cache_file_stat_head);

	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_temp_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_zero_file_area_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_small_zero_file_area_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_tiny_small_zero_file_area_head);

	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_delete_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_small_delete_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_tiny_small_delete_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_large_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_middle_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_hot_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_uninit_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_mapcount_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_small_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_tiny_small_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_tiny_small_file_one_area_head);

	spin_lock_init(&hot_cold_file_global_info.global_lock);
	spin_lock_init(&hot_cold_file_global_info.mmap_file_global_lock);

	atomic_set(&hot_cold_file_global_info.ref_count,0);
	//atomic_set(&hot_cold_file_global_info.inode_del_count,0);

	hot_cold_file_global_info.file_area_temp_to_cold_age_dx = FILE_AREA_TEMP_TO_COLD_AGE_DX;
	hot_cold_file_global_info.file_area_temp_to_cold_age_dx_ori = FILE_AREA_TEMP_TO_COLD_AGE_DX;
	hot_cold_file_global_info.file_area_hot_to_temp_age_dx = FILE_AREA_HOT_TO_TEMP_AGE_DX;
	hot_cold_file_global_info.file_area_hot_to_temp_age_dx_ori = FILE_AREA_HOT_TO_TEMP_AGE_DX;
	hot_cold_file_global_info.file_area_refault_to_temp_age_dx = FILE_AREA_REFAULT_TO_TEMP_AGE_DX;
	hot_cold_file_global_info.file_area_refault_to_temp_age_dx_ori = FILE_AREA_REFAULT_TO_TEMP_AGE_DX;
	/*file_area_temp_to_warm_age_dx取自file_area_temp_to_cold_age_dx的3/4*/
	hot_cold_file_global_info.file_area_temp_to_warm_age_dx = hot_cold_file_global_info.file_area_temp_to_cold_age_dx - (hot_cold_file_global_info.file_area_temp_to_cold_age_dx >> 2);
	hot_cold_file_global_info.file_area_temp_to_warm_age_dx_ori = hot_cold_file_global_info.file_area_temp_to_warm_age_dx;
	hot_cold_file_global_info.file_area_warm_to_temp_age_dx = hot_cold_file_global_info.file_area_temp_to_cold_age_dx - (hot_cold_file_global_info.file_area_temp_to_cold_age_dx >> 1);
	hot_cold_file_global_info.file_area_warm_to_temp_age_dx_ori = hot_cold_file_global_info.file_area_warm_to_temp_age_dx;
	hot_cold_file_global_info.file_area_reclaim_read_age_dx = hot_cold_file_global_info.file_area_temp_to_cold_age_dx * 20;
	hot_cold_file_global_info.file_area_reclaim_read_age_dx_ori = hot_cold_file_global_info.file_area_reclaim_read_age_dx;
	hot_cold_file_global_info.file_area_reclaim_ahead_age_dx = hot_cold_file_global_info.file_area_temp_to_cold_age_dx * 10;
	hot_cold_file_global_info.file_area_reclaim_ahead_age_dx_ori = hot_cold_file_global_info.file_area_reclaim_ahead_age_dx;
	hot_cold_file_global_info.file_area_free_age_dx = FILE_AREA_FREE_AGE_DX;
	hot_cold_file_global_info.file_area_free_age_dx_ori = FILE_AREA_FREE_AGE_DX;

	hot_cold_file_global_info.file_stat_delete_age_dx  = FILE_STAT_DELETE_AGE_DX;
	hot_cold_file_global_info.global_age_period = ASYNC_MEMORY_RECLIAIM_PERIOD;
	hot_cold_file_global_info.file_area_move_to_head_count_max = FILE_AREA_MOVE_TO_HEAD_COUNT_MAX;

	//128M的page cache对应file_area个数，被判定为大文件
	hot_cold_file_global_info.file_area_level_for_large_file = NORMAL_LARGE_FILE_AREA_COUNT_LEVEL;
	hot_cold_file_global_info.file_area_level_for_middle_file = NORMAL_MIDDLE_FILE_AREA_COUNT_LEVEL;
	//mmap的文件，文件页超过50M就判定为大文件
	hot_cold_file_global_info.mmap_file_area_level_for_large_file = NORMAL_LARGE_FILE_AREA_COUNT_LEVEL;
	hot_cold_file_global_info.mmap_file_area_level_for_middle_file = NORMAL_MIDDLE_FILE_AREA_COUNT_LEVEL;

	hot_cold_file_global_info.mmap_file_area_temp_to_cold_age_dx = MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX;
	hot_cold_file_global_info.mmap_file_area_hot_to_temp_age_dx = MMAP_FILE_AREA_HOT_TO_TEMP_AGE_DX;
	hot_cold_file_global_info.mmap_file_area_refault_to_temp_age_dx = MMAP_FILE_AREA_REFAULT_TO_TEMP_AGE_DX;
	hot_cold_file_global_info.mmap_file_area_free_age_dx = MMAP_FILE_AREA_FREE_AGE_DX;
	hot_cold_file_global_info.mmap_file_area_hot_age_dx = MMAP_FILE_AREA_HOT_AGE_DX;
	hot_cold_file_global_info.mmap_file_area_temp_to_warm_age_dx = hot_cold_file_global_info.mmap_file_area_temp_to_cold_age_dx - (hot_cold_file_global_info.mmap_file_area_temp_to_cold_age_dx >> 2);
	hot_cold_file_global_info.mmap_file_area_warm_to_temp_age_dx = hot_cold_file_global_info.mmap_file_area_temp_to_cold_age_dx - (hot_cold_file_global_info.mmap_file_area_temp_to_cold_age_dx >> 1);
	//64K对应的page数
	hot_cold_file_global_info.nr_pages_level = 16;
	/*该函数在setup_cold_file_area_reclaim_support_fs()之后，总是导致hot_cold_file_global_info.support_fs_type = -1，导致总file_area内存回收无效*/
	//hot_cold_file_global_info.support_fs_type = -1;

	global_file_stat_init();

	for(i = 0;i < CURRENT_SCAN_FILE_STAT_INFO_MAX;i ++){
		memset(&hot_cold_file_global_info.current_scan_file_stat_info[i],0,sizeof(struct current_scan_file_stat_info));
		memset(&hot_cold_file_global_info.current_scan_mmap_file_stat_info[i],0,sizeof(struct current_scan_file_stat_info));

		INIT_LIST_HEAD(&hot_cold_file_global_info.current_scan_file_stat_info[i].temp_head);
		INIT_LIST_HEAD(&hot_cold_file_global_info.current_scan_mmap_file_stat_info[i].temp_head);

		hot_cold_file_global_info.current_scan_file_stat_info[i].traverse_file_stat_type = i;
		hot_cold_file_global_info.current_scan_mmap_file_stat_info[i].traverse_file_stat_type = CURRENT_SCAN_FILE_STAT_INFO_MAX + i;
	}


	INIT_LIST_HEAD(&hot_cold_file_global_info.global_file_stat.current_scan_file_stat_info.temp_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.global_mmap_file_stat.current_scan_file_stat_info.temp_head);
	hot_cold_file_global_info.global_file_stat.current_scan_file_stat_info.traverse_file_stat_type = GLOBAL_FILE_STAT_CACHE_FILE;
	hot_cold_file_global_info.global_mmap_file_stat.current_scan_file_stat_info.traverse_file_stat_type = GLOBAL_FILE_STAT_MMAP_FILE;


	hot_cold_file_global_info.refault_file_area_scan_dx = 32;
	hot_cold_file_global_info.writeonly_file_age_dx_ori = 3;
	hot_cold_file_global_info.writeonly_file_age_dx = hot_cold_file_global_info.writeonly_file_age_dx_ori;

	hot_cold_file_global_info.file_area_cold_level = 100;
	hot_cold_file_global_info.to_down_list_age_dx = 60;
	hot_cold_file_global_info.to_writeonly_cold_list_age_dx = 180;

	hot_cold_file_global_info.async_memory_reclaim = kthread_run(async_memory_reclaim_main_thread,&hot_cold_file_global_info, "async_memory_reclaim");
	if (IS_ERR(hot_cold_file_global_info.async_memory_reclaim)) {
		printk("Failed to start  async_memory_reclaim\n");
		return -1;
	}
	hot_cold_file_global_info.hot_cold_file_thead = kthread_run(hot_cold_file_thread,&hot_cold_file_global_info, "hot_cold_file_thread");
	if (IS_ERR(hot_cold_file_global_info.hot_cold_file_thead)) {
		printk("Failed to start  hot_cold_file_thead\n");
		return -1;
	}

	i = register_kprobe(&kp_wakeup_kswapd);
	if (i < 0) {
		pr_err("kp_wakeup_kswapd register_kprobe failed, returned %d\n",i);
		return -1;
	}

	return hot_cold_file_proc_init(&hot_cold_file_global_info);
}
subsys_initcall(hot_cold_file_init);

int one_fs_uuid_string_convert(char **src_buf,unsigned char *dst_buf)
{
	char hex_buf[2];
	unsigned char hex_buf_count = 0;
	unsigned char dst_buf_count = 0;

	char *p = *src_buf;
	printk("%s uuid_str:%s\n",__func__,*src_buf);
	/*字符串尾或者','字符则退出，遇到','表示这是一个新的uuid*/
	while(*p && *p != ','){
		/*每一个uuid中间的分隔符，跳过*/
		if(*p == '-'){
			p ++;
			continue;
		}
		if(*p >= '0' && *p <= '9')
			hex_buf[hex_buf_count] = *p - '0';
		else if(*p >= 'a' && *p <= 'f')
			hex_buf[hex_buf_count] = *p - 'a' + 10;
		else{
			printk("%s invalid char:%c\n",__func__,*p);
			return -1;
		}

		hex_buf_count ++;
		if(2 == hex_buf_count){
			if(hex_buf_count >= SUPPORT_FS_UUID_LEN){
				printk("%s hex_buf_count:%d invalid\n",__func__,hex_buf_count);
				return -1;
			}
			dst_buf[dst_buf_count] = hex_buf[0] * 16 + hex_buf[1];

			hex_buf_count = 0;
			printk("%x\n",dst_buf[dst_buf_count]);
			dst_buf_count++;
		}
		p ++;
	}
	if(dst_buf_count != SUPPORT_FS_UUID_LEN){
		printk("%s end hex_buf_count:%d invalid\n",__func__,dst_buf_count);
		return -1;
	}

	if(*p == ','){
		/*p加1令src_buf指向下一个uuid第一个字符*/
		p ++;
		*src_buf = p;
		return 1;
	}
	return 0;
}
int uuid_string_convert(char *buf)
{
	int ret = 0;
	int support_fs_uuid_count = 0;
	int support_against_fs_uuid_count = 0;
	char *dst_buf;
	char *src_buf = buf;

	do{
		/*遇到!表示这是一个新的uuid开始，并且是一个排斥的文件系统，禁止该文件系统进行file_area读写*/
		if(*src_buf == '!'){
			if(support_against_fs_uuid_count >= 1)
				break;

			/*加1令src_buf指向下一个uuid第一个字符*/
			src_buf ++;
			dst_buf = hot_cold_file_global_info.support_fs_against_uuid;
			support_against_fs_uuid_count ++;
		}else{
			if(support_fs_uuid_count >= SUPPORT_FS_COUNT)
				break;

			dst_buf = hot_cold_file_global_info.support_fs_uuid[support_fs_uuid_count];
			support_fs_uuid_count ++;
		}

		ret = one_fs_uuid_string_convert(&src_buf,dst_buf);

		/*ret 0:uuid字符串结束  1:还有下一个fs uuid需要遍历 -1:遇到错误立即返回*/
	}while(ret == 1);

	printk("%s ret:%d support_fs_uuid_count:%d\n",__func__,ret,support_fs_uuid_count);
	return ret;
}
static int __init setup_cold_file_area_reclaim_support_fs(char *buf)
{
	printk("setup_cold_file_area_reclaim_support_fs:%s\n",buf);
	if(!buf)
		return 0;

	if (!strncmp(buf, "all", 3)) {
		hot_cold_file_global_info.support_fs_type = SUPPORT_FS_ALL;
	}
	//cold_file_area_reclaim_support_fs=fs=ext4,xfs
	else if (!strncmp(buf, "fs", 2)) {
		char *buf_head;
		unsigned char fs_name_len = 0;
		unsigned char fs_count = 0;

		hot_cold_file_global_info.support_fs_type = SUPPORT_FS_SINGLE;
		buf += 3;
		buf_head = buf;
		printk("1:setup_cold_file_area_reclaim_support_fs:%s\n",buf);
		while(*buf != '\0' && fs_count < SUPPORT_FS_COUNT){
			if(*buf == ','){
				if(fs_name_len < SUPPORT_FS_NAME_LEN){
					memset(hot_cold_file_global_info.support_fs_name[fs_count],0,SUPPORT_FS_NAME_LEN);
					strncpy(hot_cold_file_global_info.support_fs_name[fs_count],buf_head,fs_name_len);
					printk("1_2:setup_cold_file_area_reclaim_support_fs:%s fs_count:%d fs_name_len:%d\n",hot_cold_file_global_info.support_fs_name[fs_count],fs_count,fs_name_len);

					fs_count ++;
					fs_name_len = 0;
					buf_head = buf + 1;
				}
			}
			else
				fs_name_len ++;

			buf ++;
		}
		if(buf_head != buf){
			memset(hot_cold_file_global_info.support_fs_name[fs_count],0,SUPPORT_FS_NAME_LEN);
			strncpy(hot_cold_file_global_info.support_fs_name[fs_count],buf_head,fs_name_len);
			printk("1_3:setup_cold_file_area_reclaim_support_fs:%s fs_count:%d fs_name_len:%d\n",hot_cold_file_global_info.support_fs_name[fs_count],fs_count,fs_name_len);
		}
	}
	//cold_file_area_reclaim_support_fs=uuid=bee5938b-bdc6-463e-88a0-7eb08eb71dc3,8797e618-4fdd-4d92-a554-b553d1985a1a,!fcba6c29-e747-48a0-97a6-7bd8b0639e87
	else if (!strncmp(buf, "uuid", 4)) {
		buf += 5;
		printk("2:setup_cold_file_area_reclaim_support_fs:%s\n",buf);
		memset(hot_cold_file_global_info.support_fs_uuid,0,SUPPORT_FS_UUID_LEN);
		if(uuid_string_convert(buf))
			return -1;

		hot_cold_file_global_info.support_fs_type = SUPPORT_FS_UUID;
	}

	return 0;
}
early_param("cold_file_area_reclaim_support_fs", setup_cold_file_area_reclaim_support_fs);
