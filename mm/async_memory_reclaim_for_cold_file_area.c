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

#include "async_memory_reclaim_for_cold_file_area.h"


#define FILE_AREA_IN_HOT_LIST 0
#define FILE_AREA_IN_REFAULT_LIST 1
#define FILE_AREA_IN_FREE_LIST 2

#define FILE_STAT_SMALL 0  //小文件
#define FILE_STAT_NORMAL 1 //普通文件


/*允许一个周期内file_stat->temp链表上file_area移动到file_stat->temp链表头的最大次数*/
#define FILE_AREA_MOVE_TO_HEAD_COUNT_MAX 16
/*当一个文件file_area个数超过FILE_AREA_MOVE_TO_HEAD_LEVEL，才允许一个周期内file_stat->temp链表上file_area移动到file_stat->temp链表头*/
#define FILE_AREA_MOVE_TO_HEAD_LEVEL 32

struct hot_cold_file_global hot_cold_file_global_info = {
	.support_fs_type = -1,
};
unsigned long async_memory_reclaim_status = 1;

unsigned int file_area_in_update_count;
unsigned int file_area_in_update_lock_count;
unsigned int file_area_move_to_head_count;
unsigned int enable_xas_node_cache = 1;
unsigned int enable_update_file_area_age = 1;
int shrink_page_printk_open1;
int shrink_page_printk_open;

unsigned int xarray_tree_node_cache_hit;
int open_file_area_printk = 0;
int open_file_area_printk_important = 0;

static void change_memory_reclaim_age_dx(struct hot_cold_file_global *p_hot_cold_file_global);

/*****file_area、file_stat、inode 的delete*********************************************************************************/
static void i_file_area_callback(struct rcu_head *head)
{
	struct file_area *p_file_area = container_of(head, struct file_area, i_rcu);
	kmem_cache_free(hot_cold_file_global_info.file_area_cachep,p_file_area);
}
static void i_file_stat_callback(struct rcu_head *head)
{
	struct file_stat *p_file_stat = container_of(head, struct file_stat, i_rcu);
	kmem_cache_free(hot_cold_file_global_info.file_stat_cachep,p_file_stat);
}
static void i_file_stat_small_callback(struct rcu_head *head)
{
	struct file_stat_small *p_file_stat_small = container_of(head, struct file_stat_small, i_rcu);
	kmem_cache_free(hot_cold_file_global_info.small_file_stat_cachep,p_file_stat_small);
}

/*在判定一个file_area长时间没人访问后，执行该函数delete file_area。必须考虑此时有进程正好要并发访问这个file_area*/
int cold_file_area_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base * p_file_stat_base,struct file_area *p_file_area)
{
    XA_STATE(xas, &((struct address_space *)(p_file_stat_base->mapping))->i_pages, p_file_area->start_index >> PAGE_COUNT_IN_AREA_SHIFT);

	/*在释放file_area时，可能正有进程执行hot_file_update_file_status()遍历file_area_tree树中p_file_area指向的file_area结构，需要加锁*/
	/*如果近期file_area被访问了则不能再释放掉file_area*/

	/*现在不能再通过file_area的age判断了，再这个场景会有bug：假设file_area没有page，进程1执行filemap_get_read_batch()从xarray tree遍历到
	 *这个file_area，没找到page。因为没找到page，则不会更新global age到file_area的age。此时进程2执行cold_file_area_delete()函数里要是delete该
	 file_area，ile_area的age与global age还是相差很大，下边这个if依然不成立。

	 接着就存在一个并发问题:进程1正执行__filemap_add_folio()分配page并保存到file_area。此时如果进程2执行cold_file_area_delete()函数
	 delete file_area。此时靠xas_lock解决并发问题：

	 1：如果进程2先在cold_file_area_delete()先获取xas_lock，执行xas_store(&xas, NULL)把file_area从xarray tree剔除，接着要call_rcu延迟释放file_area。
	 因为进程1等此时可能还在filemap_get_read_batch()或mapping_get_entry()使用这个file_area。但他们都有rcu_read_lock，
	 等他们rcu_unlock_lock由内核自动释放掉file_area。继续，等进程2filemap_get_read_batch()中发现从xarray tree找到的file_area没有page，
	 然后执行到__filemap_add_folio()函数：但获取到xas_lock后，执行xas_for_each_conflict(&xas, entry)已经查不到这个file_area了，因为已经被进程2
	 执行xas_store(&xas, NULL)被从xarray tree剔除了，则进程1只能在__filemap_add_folio()里分配新的file_area了，不再使用老的file_area

	 2：如果进程1先在__filemap_add_folio()获取xas_lock，则分配page并添加到file_area里。然后进程2cold_file_area_delete()获取xas_lock，发现
	 file_area已经有了page，则file_arae_have_page(p_file_area)大于0，则不会再释放该file_area
	 */

	//if(hot_cold_file_global_info.global_age - p_file_area->file_area_age < 2 ){
	xas_lock_irq(&xas);
	if(file_area_have_page(p_file_area)){
		printk("%s file_area:0x%llx file_area_state:0x%x\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
		/*那就把它再移动回file_stat->file_area_temp链表头。有这个必要吗？没有必要的!因为该file_area是在file_stat->file_area_free链表上，如果
		  被访问了而执行hot_file_update_file_status()函数，会把这个file_area立即移动到file_stat->file_area_temp链表，这里就没有必要做了!!!!!*/
		xas_unlock_irq(&xas);
		return 1;
	}
	/*从xarray tree剔除。注意，xas_store不仅只是把保存file_area指针的xarray tree的父节点xa_node的槽位设置NULL。
	 *还有一个隐藏重要作用，如果父节点node没有成员了，还是向上逐级释放父节点xa_node，直至xarray tree全被释放*/
	xas_store(&xas, NULL);
	if (xas_error(&xas)){
		printk("%s xas_error:%d !!!!!!!!!!!!!!\n",__func__,xas_error(&xas));
		xas_unlock_irq(&xas);
		return -1;
	}
	xas_unlock_irq(&xas);

	/*到这里，一定可以确保file_area已经从xarray tree剔除，但不能保证不被其他进程在filemap_get_read_batch()或mapping_get_entry()中，
	 *在file_area已经从xarray tree剔除前已经并发访问了file_area，现在还在使用，所以要rcu延迟释放file_area结构*/

	spin_lock(&p_file_stat_base->file_stat_lock);
	if(0 == p_file_stat_base->file_area_count)
		panic("%s file_stat:0x%llx file_area:0x%llx file_area_count == 0 error\n",__func__,(u64)p_file_stat,(u64)p_file_area);
	/*该文件file_stat的file_area个数减1，把file_area从file_stat的链表剔除，这个过程要加锁*/
	p_file_stat->file_area_count --;
	list_del_rcu(&p_file_area->file_area_list);
	spin_unlock(&p_file_stat_base->file_stat_lock);

	/*rcu延迟释放file_area结构*/
	call_rcu(&p_file_area->i_rcu, i_file_area_callback);

	if(shrink_page_printk_open1)
		FILE_AREA_PRINT("%s file_area:0x%llx delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area);

	return 0;
}
EXPORT_SYMBOL(cold_file_area_delete);

/*在文件inode被iput释放后，执行该函数释放该文件file_stat的所有file_area，此时肯定没进程再访问该文件的file_stat、file_area，不用考虑并发。
 *错了，此时可能异步内存线程也会因这个文件长时间空闲而释放file_stat和file_area。又错了，当前函数就是异步内存回收线程里执行的，没这种情况*/
int cold_file_area_delete_quick(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area)
{
    //XA_STATE(xas, &((struct address_space *)(p_file_stat->mapping))->i_pages, p_file_area->start_index >> PAGE_COUNT_IN_AREA_SHIFT);
	
	//xas_lock_irq(&xas);
	if(file_area_have_page(p_file_area)){
		panic("%s file_area:0x%llx file_area_state:0x%x has page\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
		/*那就把它再移动回file_stat->file_area_temp链表头。有这个必要吗？没有必要的!因为该file_area是在file_stat->file_area_free链表上，如果
		  被访问了而执行hot_file_update_file_status()函数，会把这个file_area立即移动到file_stat->file_area_temp链表，这里就没有必要做了!!!!!*/
		//xas_unlock_irq(&xas);
		return 1;
	}
	/*从xarray tree剔除。注意，xas_store不仅只是把保存file_area指针的xarray tree的父节点xa_node的槽位设置NULL。
	 *还有一个隐藏重要作用，如果父节点node没有成员了，还是向上逐级释放父节点xa_node，直至xarray tree全被释放*/
#if 0	
    /*但是有个重大问题!!!!!!!!!!!!!!!!异步内存回收线程执行到该函数时，该文件的inode、mapping、xarray tree已经释放了。这里
	 *再访问xarray tree就是无效内存访问了。因此这段代码必须移动到在__destroy_inode_handler_post()执行，此时inode肯定没释放*/
	xas_store(&xas, NULL);
	if (xas_error(&xas)){
		printk("%s xas_error:%d !!!!!!!!!!!!!!\n",__func__,xas_error(&xas));
		//xas_unlock_irq(&xas);
		return -1;
	}
#endif	
	//xas_unlock_irq(&xas);

	/*到这里，一定可以确保file_area已经从xarray tree剔除，但不能保证不被其他进程在filemap_get_read_batch()或mapping_get_entry()中，
	 *在file_area已经从xarray tree剔除前已经并发访问了file_area，现在还在使用，所以要rcu延迟释放file_area结构*/

	//spin_lock(&p_file_stat->file_stat_lock);
	if(0 == p_file_stat_base->file_area_count)
		panic("%s file_stat:0x%llx file_area:0x%llx file_area_count == 0 error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area);
	/*该文件file_stat的file_area个数减1，把file_area从file_stat的链表剔除，这个过程要加锁*/
	p_file_stat_base->file_area_count --;
	list_del_rcu(&p_file_area->file_area_list);
	//spin_unlock(&p_file_stat->file_stat_lock);

	/*隐藏重点!!!!!!!!!!!，此时可能有进程正通过proc查询该文件的file_stat、file_area、page统计信息，正在用他们。因此也不能
	 *kmem_cache_free()直接释放该数据结构，也必须得通过rcu延迟释放，并且，这些通过proc查询的进程，必须得先rcu_read_lock，
	 再查询file_stat、file_area、page统计信息，保证rcu_read_unlock前，他们不会被释放掉*/
	//kmem_cache_free(hot_cold_file_global_info.file_area_cachep,p_file_area);
	
	/*rcu延迟释放file_area结构*/
	call_rcu(&p_file_area->i_rcu, i_file_area_callback);

	if(shrink_page_printk_open1)
		FILE_AREA_PRINT("%s file_area:0x%llx delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area);

	return 0;
}

/*在判定一个文件file_stat的page全部被释放，然后过了很长时间依然没人访问，执行该函数delete file_stat。必须考虑此时有进程并发访问该文件file_stat*/
int cold_file_stat_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base_del,char file_type)
{
	struct file_stat *p_file_stat_del;
	struct file_stat_small *p_file_stat_small_del;

	//到这里时p_file_stat_del->mapping可能被__destroy_inode_handler_post()释放inode赋值了NULL，要防护这种情况
	//struct xarray *xarray_i_pages = &(mapping->i_pages);
	struct xarray *xarray_i_pages;
	struct address_space *mapping;
	char file_stat_del = 0;
	int ret = 0;
	/*并发问题:进程1执行filemap_get_read_batch()，，从xarray tree找不到file_area，xarray tree是空树，mapping->rh_reserved1
	 *非NULL。然后执行到__filemap_add_folio()函数，打算分配file_area、分配page并保存到file_area。此时如果进程2执行
	 cold_file_stat_delete()函数delete stat。靠xas_lock(跟xa_lock一样)解决并发问题：

	 1：如果进程2先在cold_file_stat_delete()先获取xa_lock，执行p_file_stat_del->mapping->rh_reserved1 = 0令mapping的file_stat无效，
	 接着要call_rcu延迟释放file_stat。因为进程1等此时可能还在filemap_get_read_batch()或mapping_get_entry()使用这个file_stat。
	 但他们都有rcu_read_lock，等他们rcu_unlock_lock由内核自动释放掉file_stat。等进程2执行到__filemap_add_folio()，
	 获取到xas_lock后，执行if(0 == mapping->rh_reserved1)，if成立。则只能分配新的file_stat了，不会再使用老的file_stat
	 2：如果进程1先在__filemap_add_folio()获取xa_lock，则分配file_area、分配page并添加到file_area里。然后进程2执行到cold_file_stat_delete()
	 获取xa_lock锁，发现file_stat已经有了file_aree，if(p_file_stat_del->file_area_count > 0)，则不会再释放该file_stat了

	 3：最近有发现一个并发内核bug。异步内存回收线程cold_file_stat_delete()释放长时间不访问的file_stat。但是此时
	 对应文件inode被iput()释放了，最后执行到__destroy_inode_handler_post()释放inode和file_stat，此时二者就存在
	 并发释放file_stat同步不合理而造成bug。
	 cold_file_stat_delete()中依次执行p_file_stat_del->mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE;
	 p_file_stat_del->mapping=NULL;标记file_stat delete;
	 __destroy_inode_handler_post()依次执行mapping->rh_reserved1=0;p_file_stat_del->mapping=NULL;
	 标记file_stat delete;把file_stat移动到global delete链表;该函数执行后立即释放掉inode结构;

	会存在如下并发问题
	1:__destroy_inode_handler_post()中先执行p_file_stat_del->mapping=NULL后，cold_file_stat_delete()中执行
	p_file_stat_del->mapping->rh_reserved1=0而crash。
	2:__destroy_inode_handler_post()中执行后立即释放掉inode和mapping，而cold_file_stat_delete()中对
	p_file_stat_del->mapping->rh_reserved1 赋值而访问已释放的内存而crash。

	怎么解决这个并发问题，目前看只能cold_file_stat_delete()先执行file_inode_lock对inode加锁，如果inode加锁成功
	则该文件就不会被执行iput()->__destroy_inode_handler_post()。如果inode加锁失败，说明这个文件inode已经被其他进程
	iput()释放了，直接return。总之阻止二者并发执行。
    */

	//rcu_read_lock();
	//lock_file_stat(p_file_stat_del,0);
	//spin_lock(&p_file_stat_del->file_stat_lock);
	if(0 == file_inode_lock(p_file_stat_base_del))
		return ret;

	mapping = p_file_stat_base_del->mapping;
	xarray_i_pages = &(mapping->i_pages);
	xa_lock_irq(xarray_i_pages);

	/*正常这里释放文件file_stat结构时，file_area指针的xarray tree的所有成员都已经释放，是个空树，否则就触发panic*/

	/*如果file_stat的file_area个数大于0，说明此时该文件被方法访问了，在hot_file_update_file_status()中分配新的file_area。
	 *此时这个file_stat就不能释放了*/
	if(p_file_stat_base_del->file_area_count > 0){
		/*一般此时file_stat是不可能有delete标记的，但可能inode释放时__destroy_inode_handler_post中设置了delete。
		 *正常不可能，这里有lock_file_stat加锁防护*/
		if(file_stat_in_delete(p_file_stat_base_del)){
			printk("%s %s %d file_stat:0x%llx status:0x%lx in delete\n",__func__,current->comm,current->pid,(u64)p_file_stat_base_del,p_file_stat_base_del->file_stat_status);
			dump_stack();
		}	
		//spin_unlock(&p_file_stat_base_del->file_stat_lock);
		//unlock_file_stat(p_file_stat_base_del);
		xa_unlock_irq(xarray_i_pages);
		ret = 1;
		goto err;
	}

	/*正常这里释放文件file_stat结构时，file_area指针的xarray tree的所有成员都已经释放，是个空树，否则就触发panic*/
	if(p_file_stat_base_del->mapping->i_pages.xa_head != NULL)
		panic("file_stat_del:0x%llx 0x%llx !!!!!!!!\n",(u64)p_file_stat_base_del,(u64)p_file_stat_base_del->mapping->i_pages.xa_head);

	/*如果file_stat在__destroy_inode_handler_post中被释放了，file_stat一定有delete标记。否则是空闲file_stat被delete，
	 *这里得标记file_stat delete。这段对mapping->rh_reserved1清0的必须放到xa_lock_irq加锁里，因为会跟__filemap_add_folio()
	 *里判断mapping->rh_reserved1的代码构成并发。并且，如果file_stat在__destroy_inode_handler_post中被释放了，
	 *p_file_stat_base_del->mapping是NULL，这个if的p_file_stat_base_del->mapping->rh_reserved1=0会crash。现在赋值
	 *cold_file_stat_delete()中使用了file_inode_lock，已经没有这个并发问题了。*/
	if(!file_stat_in_delete(p_file_stat_base_del)/*p_file_stat_base_del->mapping*/){
		/* 文件inode的mapping->rh_reserved1清0表示file_stat无效，这__destroy_inode_handler_post()删除inode时，
		 * 发现inode的mapping->rh_reserved1是0就不再使用file_stat了，会crash。但现在在释放file_stat时，必须
		 * 改为赋值SUPPORT_FILE_AREA_INIT_OR_DELETE(1)。这样等将来该文件又被读写，发现mapping->rh_reserved1
		 * 是1，说明该文件所属文件系统支持file_area文件读写，于是__filemap_add_folio()是把file_area指针
		 * 保存到xarray tree，而不是page指针。那什么情况下要把mapping->rh_reserved1清0呢？只有iput释放inode时，
		 * 此时该文件inode肯定不会再被读写了，才能把mapping->rh_reserved1清0。此时也必须把mapping->rh_reserved1清0，
		 * 否则这个inode释放后再被其他进程被其他tmpfs等文件系统分配到，因为inode->mapping->rh_reserved1是1，会
		 * 误以为该文件支持file_area文件读写，造成crash，具体在文件inode释放iput()->__destroy_inode_handler_post()
		 * 函数有详细介绍。!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		 **/
		//p_file_stat_base_del->mapping->rh_reserved1 = 0;
		p_file_stat_base_del->mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE;/*原来赋值0，现在赋值1，原理一样*/
		barrier();
		p_file_stat_base_del->mapping = NULL;
	}
	//spin_unlock(&p_file_stat_base_del->file_stat_lock);
	//unlock_file_stat(p_file_stat_base_del);
	xa_unlock_irq(xarray_i_pages);

	/*到这里，一定可以确保file_stat跟mapping没关系了，因为mapping->rh_reserved1是0，但不能保证不被其他进程在filemap_get_read_batch()
	 *或mapping_get_entry()在mapping->rh_reserved1是0前已经并发访问了file_stat，现在还在使用，好在他们访问file_stat前都rcu_read_lock了，
	 等他们rcu_read_unlock才能真正释放file_stat结构。这个工作就需要rcu延迟释放file_area结构*/

	/*使用global_lock加锁是因为要把file_stat从p_hot_cold_file_global的链表中剔除，防止此时其他进程并发向p_hot_cold_file_global的链表添加file_stat,
	 *是否有必要改为 spin_lock_irqsave，之前就遇到过用spin_lock_irq导致打乱中断状态而造成卡死?????????????????????????????????????????????*/
	if(file_stat_in_cache_file(p_file_stat_base_del)){
		spin_lock_irq(&p_hot_cold_file_global->global_lock);
		/*这里有个非常重要的隐藏点，__destroy_inode_handler_post()和cold_file_stat_delete()存在并发释放file_stat的
		 *情况，如果到这里发现已经file_stat_in_delete了，说明__destroy_inode_handler_post()中已经标记file_stat delete了
		 *并且把file_stat移动到global delete链表了，这里就不能再list_del_rcu(&p_file_stat_base_del->hot_cold_file_list)了。
		 *而应该把mapping->rh_reserved1清0。因为有可能，__destroy_inode_handler_post()中先执行
		 *mapping->rh_reserved1清0，然后global_lock加锁，标记file_stat delete。然后cold_file_stat_delete()执行
		 *mapping->rh_reserved1=SUPPORT_FILE_AREA_INIT_OR_DELETE。到这里发现file_stat已经有了delete标记，就得
		 *再执行一次mapping->rh_reserved1 = 0清0了，否则后续这个inode被tmpfs文件系统分配到，看到mapping->rh_reserved1
		 *不是0，就会错误以为它支持file_area文件读写。

		 *但是又有一个问题，到这里时，如果文件inode被__destroy_inode_handler_post()释放了，这里再mapping->rh_reserved1 = 0
		 *清0，就会访问已经释放了的内存。因为mapping结构体属于inode的一部分。这样就有大问题了，必须得保证
		 *cold_file_stat_delete()函数里inode不能被释放，才能放心使用mapping->rh_reserved1，只能用file_inode_lock了。
		 * */
		if(!file_stat_in_delete(p_file_stat_base_del)){
			/* 在这个加个内存屏障，保证前后代码隔离开。即file_stat有delete标记后，inode->i_mapping->rh_reserved1一定是0，
			 * p_file_stat->mapping一定是NULL*/
			smp_wmb();
			/*下边的call_rcu()有smp_mb()，保证set_file_stat_in_delete()后有内存屏障*/
			set_file_stat_in_delete(p_file_stat_base_del);
			/* 从global的链表中剔除该file_stat，这个过程需要加锁，因为同时其他进程会执行hot_file_update_file_status()
			 * 向global的链表添加新的文件file_stat*/
			list_del_rcu(&p_file_stat_base_del->hot_cold_file_list);
			file_stat_del = 1;
			/*file_stat个数减1*/
			hot_cold_file_global_info.file_stat_count --;
		}
		else{
			mapping->rh_reserved1 = 0;
			/*避免spin lock时有printk打印*/
			spin_unlock_irq(&hot_cold_file_global_info.global_lock);
			printk("%s p_file_stat:0x%llx status:0x%lx already delete\n",__func__,(u64)p_file_stat_base_del,p_file_stat_base_del->file_stat_status);
			goto err;
		}
		spin_unlock_irq(&p_hot_cold_file_global->global_lock);
	}else{
		spin_lock_irq(&p_hot_cold_file_global->mmap_file_global_lock);
		if(!file_stat_in_delete(p_file_stat_base_del)){
			/*在这个加个内存屏障，保证前后代码隔离开。即file_stat有delete标记后，inode->i_mapping->rh_reserved1一定是0，p_file_stat->mapping一定是NULL*/
			smp_wmb();
			set_file_stat_in_delete(p_file_stat_base_del);
			//从global的链表中剔除该file_stat，这个过程需要加锁，因为同时其他进程会执行hot_file_update_file_status()向global的链表添加新的文件file_stat
			list_del_rcu(&p_file_stat_base_del->hot_cold_file_list);
			file_stat_del = 1;
			//file_stat个数减1
			hot_cold_file_global_info.mmap_file_stat_count --;
		}
		else{
			mapping->rh_reserved1 = 0;
			/*避免spin lock时有printk打印*/
			spin_unlock_irq(&hot_cold_file_global_info.mmap_file_global_lock);
			printk("%s p_file_stat:0x%llx status:0x%lx already delete\n",__func__,(u64)p_file_stat_base_del,p_file_stat_base_del->file_stat_status);
			goto err;
		}
		spin_unlock_irq(&p_hot_cold_file_global->mmap_file_global_lock);
	}

err:
	/* rcu延迟释放file_stat结构。call_rcu()里有smp_mb()内存屏障。但如果mapping->rh_reserved1是0了，说明上边
	 * 没有执行list_del_rcu(&p_file_stat_del->hot_cold_file_list)，那这里不能执行call_rcu()*/
	if(file_stat_base_del){
		if(FILE_STAT_NORMAL == file_type){
			p_file_stat_del = container_of(p_file_stat_base,struct file_stat,file_stat_base);
			call_rcu(&p_file_stat_del->i_rcu, i_file_stat_callback);
		}else{
			p_file_stat_small_del = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
			call_rcu(&p_file_stat_small_del->i_rcu, i_file_stat_small_callback);
		}
	}

	//rcu_read_unlock();
	file_inode_unlock_mapping(mapping);


	FILE_AREA_PRINT("%s file_stat:0x%llx delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_stat_del);

	return ret;
}
EXPORT_SYMBOL(cold_file_stat_delete);

/*在文件inode被iput释放后，执行该函数释放该文件file_stat，此时肯定没进程再访问该文件，不用考虑并发*/
static noinline int cold_file_stat_delete_quick(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base_del,char file_type)
{
	struct file_stat *p_file_stat_del;
	struct file_stat_small *p_file_stat_small_del;

	/*二者先前已经在__destroy_inode_handler_post()处理过，不可能成立*/
	if(!file_stat_in_delete(p_file_stat_base_del))
		panic("file_stat_del:0x%llx status:0x%lx!!!!!!!!\n",(u64)p_file_stat_base_del,p_file_stat_base_del->file_stat_status);
    
	/*使用global_lock加锁是因为要把file_stat从p_hot_cold_file_global的链表中剔除，防止此时其他进程并发向p_hot_cold_file_global的链表添加file_stat,
	 *是否有必要改为 spin_lock_irqsave，之前就遇到过用spin_lock_irq导致打乱中断状态而造成卡死?????????????????????????????????????????????*/
	if(file_stat_in_cache_file(p_file_stat_base_del)){
		spin_lock_irq(&p_hot_cold_file_global->global_lock);
		//从global的链表中剔除该file_stat，这个过程需要加锁，因为同时其他进程会执行hot_file_update_file_status()向global的链表添加新的文件file_stat
		list_del_rcu(&p_file_stat_base_del->hot_cold_file_list);
		//file_stat个数减1
		hot_cold_file_global_info.file_stat_count --;
		spin_unlock_irq(&p_hot_cold_file_global->global_lock);
	}else{
		spin_lock_irq(&p_hot_cold_file_global->mmap_file_global_lock);
		//从global的链表中剔除该file_stat，这个过程需要加锁，因为同时其他进程会执行hot_file_update_file_status()向global的链表添加新的文件file_stat
		list_del_rcu(&p_file_stat_base_del->hot_cold_file_list);
		//file_stat个数减1
		hot_cold_file_global_info.mmap_file_stat_count --;
		spin_unlock_irq(&p_hot_cold_file_global->mmap_file_global_lock);
	}

	/*隐藏重点，此时可能有进程正通过proc查询该文件的file_stat、file_area、page统计信息，正在用他们。因此也不能
	 *kmem_cache_free()直接释放该数据结构，也必须得通过rcu延迟释放，并且，这些通过proc查询的进程，必须得先rcu_read_lock，
	 再查询file_stat、file_area、page统计信息，保证rcu_read_unlock前，他们不会被释放掉*/
	//kmem_cache_free(hot_cold_file_global_info.file_stat_cachep,p_file_stat);
	/*rcu延迟释放file_stat结构*/
	if(FILE_STAT_NORMAL == file_type){
		p_file_stat_del = container_of(p_file_stat_base,struct file_stat,file_stat_base);
	    call_rcu(&p_file_stat_del->i_rcu, i_file_stat_callback);
	}else{
		p_file_stat_small_del = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
	    call_rcu(&p_file_stat_small_del->i_rcu, i_file_stat_small_callback);
	}

	FILE_AREA_PRINT("%s file_stat:0x%llx delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_stat_del);

	return 0;
}

/*inode在input释放inode结构最后会执行到该函数，主要是标记file_stat的delete，然后把file_stat移动到global delete链表。
 *这里有个严重的并发问题需要考虑，该函数执行后，inode结构就会被立即释放。那如果异步内存回收线程正在遍历该文件inode的mapping和
 *通过xarray tree查询page，就要非法访问内存了，因为这些内存随着inode一起被释放了!!!!!!!!!!!这个并发问题之前困扰了很久，用rcu
 *的思想很好解决。因为inode结构的释放最后是执行destroy_inode()里的call_rcu(&inode->i_rcu, i_callback)异步释放的。而在异步内存
 *回收线程遍历该inode的mapping和通过xarray tree查询page前，先rcu_read_lock，就不用担心inode结构会被释放了。rcu万岁。*/

/*但有个隐藏很深的问题，执行该函数是在iput的最后阶段执行destroy_inode()里调用的，此时文件inode的xrray tree已经被释放了，但是
 *只是把保存文件page指针从xarray tree的file_area->pages[]数组设置NULL了，并没有把保存file_area指针的的xarray tree的槽位设置NULL，
 *xarray tree还保存着file_area指针。因为我重写了文件页page从xarry tree剔除的函数！这样会有问题吗，似乎没什么影响，无非只是
 *xarray tree还保存着file_area指针。不对，有大问题，会导致xarray tree泄漏，就是它的各级父节点xa_node结构无法释放！因此，必须
 *要在iput()过程也能够，要强制释放掉文件xarray tree所有的父节点node。具体怎么办？因为xarray tree是inode的成员
 struct address_space	i_data的成员struct xarray	i_pages，是个实体数据结构。

 因此，在__destroy_inode_handler_post()标记inode、file_stat释放后，接着异步内存回收线程释放该文件file_stat的file_area函数
 cold_file_area_delete_quick()函数中，执行xas_store(&xas, NULL)，在xarray tree的槽位设置file_area NULL外，还自动逐级向上释放
 父节点xa_node。又错了，这里又有个重大bug!!!!!!!!!!!!!!!!__destroy_inode_handler_post()执行后，inode就会被立即释放掉，然后
 异步内存回收线程执行到cold_file_area_delete_quick()函数时，文件inode、mapping、xarray tree都释放了，此时执行
 xas_store(&xas, NULL)因xarray tree访问无效内存而crash。
 
 这个隐藏很低的问题，也是一点点把思路写出才猛然发现的，空想很难想到！
 历史一再证明，一字一字把思路写出来，是发现隐藏bug的有效手段

 那怎么解决，要么就在__destroy_inode_handler_post()函数里执行
 依次xas_store(&xas, NULL)释放xarray tree吧。这是最早的想法，但是查看iput->evict()->truncate_inode_pages_final()
 或op->evict_inode(inode)函数的执行，发现在truncate_inode_pages()截断文件所有pagecache前，一定执行mapping_set_exiting(mapping)
 标记mapping->flags的AS_EXITING。然后执行到truncate_inode_pages->truncate_inode_pages_range中，执行
 find_lock_entries、truncate_folio_batch_exceptionals函数，我令find_lock_entries返回非NULL，强制执行
 delete_from_page_cache_batch()释放保存file_area的xarray tree。

 不行，delete_from_page_cache_batch()依赖保存在fbatch数组的page
 指针。如果file_area的page指针全被释放了，是个空file_area，delete_from_page_cache_batch()函数直接返回，不会再释放xarray tree。
 所以，要么改写 truncate_inode_pages->truncate_inode_pages_range 函数的逻辑，空file_area也能执行delete_from_page_cache_batch()
 释放xarray tree，但代价太大。还是在iput的最后，文件pagecache全被释放了，执行到evict->destroy_inode->__destroy_inode_handler_post()中，
 再强制执行xas_store(&xas, NULL)释放xarray tree吧。但是这样会造成iput多耗时。

 又想到一个方法：当iput执行到evict是，如果inode->i_mapping->rh_reserved1不是NULL，则不再执行destroy_inode，而else执行
 __destroy_inode_handler_post()，标记file_stat delete。然后等异步内存回收线程里，执行cold_file_area_delete_quick释放掉file_stat的
 所有file_area后，再执行destroy_inode释放掉inode、mapping、xarray tree。或者两个方法结合起来，当xarray tree的层数大于1时，是大文件，
 则使用该方法延迟释放inode。当xarray tree的层数是1，是小文件，直接在__destroy_inode_handler_post()里强制执行xas_store(&xas, NULL)
 释放xarray tree。使用延迟释放inode的方法吧，在__destroy_inode_handler_post()里强制释放xarray tree需要编写一个函数：xarray tree
 遍历所有的file_area，依次执行xas_store(&xas, NULL)释放xarray tree，这个函数评估比较复杂。但是延迟释放inode需要对evict->destroy_inode
 流程有较大改动，怕改出问题，其实也还好

 又想到一个方法，在__destroy_inode_handler_post里通过inode->i_data.i_pages备份一份该文件的xarray数据结构，然后异步内存回收线程里
 直接依赖这个备份的xarray tree，依次执行xas_store(&xas, NULL)释放xarray tree，就可以了xas_store(&xas, NULL)释放xarray tree了。但是
 这个方法感觉是旁门左道。

 最终敲定的方法是：还是iput_final->evict->truncate_inode_pages_final->truncate_inode_pages->truncate_inode_pages_range中把file_area从
 xarra tree从剔除释放掉，但是不用修改truncate_inode_pages_range源码，而是修改最后调用到的find_lock_entries->find_get_entry_for_file_area源码
 1:在truncate_inode_pages_range->find_lock_entries-> find_get_entry_for_file_area函数中，mapping_exiting(mapping)成立，
   当遇到没有page的file_area，要强制执行xas_store(&xas, NULL)把file_area从xarray tree剔除。
   因为此时file_area没有page，则从find_lock_entries()保存到fbatch->folios[]数组file_area的page是0个，则从find_lock_entries函数返回
   truncate_inode_pages_range后，因为fbatch->folios[]数组没有保存该file_area的page，则不会执行
   delete_from_page_cache_batch(mapping, &fbatch)->page_cache_delete_batch()，把这个没有page的file_area从xarray tree剔除。于是只能在
   truncate_inode_pages_range->find_lock_entries调用到该函数时，遇到没有page的file_area，强制把file_area从xarray tree剔除了

2：针对truncate_inode_pages_range->find_lock_entries-> find_get_entry_for_file_area函数中，遇到有page的file_area，则find_lock_entries
   函数中会把folio保存到batch->folios[]数组，然后会执行到delete_from_page_cache_batch(mapping, &fbatch)->page_cache_delete_batch()，
   把folio从xarray tree剔除，当剔除file_area最后一个folio时，file_area没有page了，则page_cache_delete_batch()函数中强制
   执行xas_store(&xas, NULL)把file_area从xarray tree剔除释放。但是，为防止iput最后执行到__destroy_inode_handler_post时，xarray tree
   是否是空树，即判断inode->i_mapping->i_pages.xa_head是否NULL，否则crash
*/

static noinline void __destroy_inode_handler_post(struct inode *inode)
{
	/* 又一个重大隐藏bug。!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * 当iput()释放一个文件inode执行到这里，inode->i_mapping->rh_reserved1一定
	 * IS_SUPPORT_FILE_AREA_READ_WRITE()成立吗(即大于1)。不是的，如果这个file_stat长时间没访问被
	 * cold_file_stat_delete()释放了，那inode->i_mapping->rh_reserved1就被赋值1。这种情况下，该文件
	 * 执行iput()被释放，执行到__destroy_inode_handler_post()函数时，发现inode->i_mapping->rh_reserved1
	 * 是1，也要执行if里边的代码inode->i_mapping->rh_reserved1 = 0清0 !!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * 
	 * 新的问题来了，如果__destroy_inode_handler_post()和cold_file_stat_delete()并发释放file_stat，
	 * 就会存在并发问题，详细在cold_file_stat_delete()有分析，解决方法是使用file_inode_lock()阻止并发
	 * */
	//if(inode && inode->i_mapping && IS_SUPPORT_FILE_AREA_READ_WRITE(inode->i_mapping)){
	if(inode && inode->i_mapping && IS_SUPPORT_FILE_AREA(inode->i_mapping)){
		struct file_stat *p_file_stat = (struct file_stat *)inode->i_mapping->rh_reserved1;
		/*到这里，文件inode的mapping的xarray tree必然是空树，不是就crash*/  
		if(inode->i_mapping->i_pages.xa_head != NULL)
			panic("%s xarray tree not clear:0x%llx\n",__func__,(u64)(inode->i_mapping->i_pages.xa_head));

		//inode->i_mapping->rh_reserved1 = 0;
		/* 重大隐藏bug!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		 * inode释放后必须把inode->mapping->rh_reserved1清0，不能赋值SUPPORT_FILE_AREA_INIT_OR_DELETE(1)。
		 * 否则，这个inode被tmpfs等不支持file_area的文件系统分配，发现inode->mapping->rh_reserved1是1，
		 * 然后分配新的folio执行__filemap_add_folio()把folio添加到xarray tree时发现，
		 * inode->mapping->rh_reserved1是1，那就误以为该文件的文件系统是ext4、xfs、f2fs等支持file_area
		 * 文件读写的文件系统。这样就会crash了，因为tmpfs文件系统不支持file_area，会从xarray tree
		 * 得到的file_area指针当成page，必然会crash*/
		//inode->i_mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE;原来赋值0，现在赋值1，原理一样
		inode->i_mapping->rh_reserved1 = 0;
		p_file_stat->mapping = NULL;

		/*在这个加个内存屏障，保证前后代码隔离开。即file_stat有delete标记后，inode->i_mapping->rh_reserved1一定无效，p_file_stat->mapping一定是NULL*/
		smp_wmb();
		if(file_stat_in_cache_file(p_file_stat)){
			/* 又遇到一个重大的隐藏bug。如果当前文件文件页page全释放后还是长时间没访问，此时异步内存回收线程正好执行
			 * cold_file_stat_delete()释放file_stat，并global_lock加锁，标记file_stat delete，然后list_del(p_file_stat)。
			 * 然后，这里global_lock加锁，再执行list_move(&p_file_stat->hot_cold_file_list,...)那就出问题了，因为此时
			 * 此时p_file_stat已经在cold_file_stat_delete()中list_del(p_file_stat)从链表中剔除了。这个bug明显违反了
			 * 一个我之前定下的一个原则，必须要在spin_lock加锁后再判断一次file_area和file_stat状态，是否有变化，
			 * 因为可能在spin_lock加锁前一瞬间状态已经被修改了!!!!!!!!!!!!!!!!!!!!!!!*/
			spin_lock_irq(&hot_cold_file_global_info.global_lock);
			if(!file_stat_in_delete(p_file_stat)){
				/*file_stat不一定只会在global temp链表，也会在global hot等链表，这些标记不清理了*/
				//clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
				set_file_stat_in_delete(p_file_stat);
				//要兼容file_stat_small???????????????????????
				list_move(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.file_stat_delete_head);
			}else{
				/*如果到这份分支，说明file_stat先被异步内存回收线程执行cold_file_stat_delete()标记delete了，
				 *为了安全要再对inode->i_mapping->rh_reserved1清0一次，详情cold_file_stat_delete()也有解释。
				 现在用了file_inode_lock后，这种情况已经不可能了，但代码还是保留一下吧*/
				inode->i_mapping->rh_reserved1 = 0;
				/*避免spin lock时有printk打印*/
				spin_unlock_irq(&hot_cold_file_global_info.global_lock);
				printk("%s p_file_stat:0x%llx status:0x%lx already delete\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
				goto err;
			}
			spin_unlock_irq(&hot_cold_file_global_info.global_lock);
		}else{
			/* 注意，会出现极端情况。so等elf文件最初被判定为cache file而在global temp链表，然后
			 * cache_file_stat_move_to_mmap_head()函数中正把该file_stat移动到globaol mmap_file_stat_temp_head链表。会出现短暂的
			 * file_stat即没有in_cache_file也没有in_mmap_file状态，此时就会走到else分支，按照mmap文件的delete处理!!!!!!!!!!*/

			spin_lock_irq(&hot_cold_file_global_info.mmap_file_global_lock);
			if(!file_stat_in_delete(p_file_stat)){
				/*file_stat不一定只会在global temp链表，也会在global hot等链表，这些标记不清理了*/
				//clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
				set_file_stat_in_delete(p_file_stat);
				list_move(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_delete_head);
			}
			else{
				inode->i_mapping->rh_reserved1 = 0;
				spin_unlock_irq(&hot_cold_file_global_info.mmap_file_global_lock);
				printk("%s p_file_stat:0x%llx status:0x%lx already delete\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
				goto err;
			}
			spin_unlock_irq(&hot_cold_file_global_info.mmap_file_global_lock);
		}
err:
		if(shrink_page_printk_open1)
			FILE_AREA_PRINT("%s file_stat:0x%llx iput delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_stat);
	}
}
void disable_mapping_file_area(struct inode *inode)
{
	__destroy_inode_handler_post(inode);
}
EXPORT_SYMBOL(disable_mapping_file_area);

//删除p_file_stat_del对应文件的file_stat上所有的file_area，已经对应hot file tree的所有节点hot_cold_file_area_tree_node结构。最后释放掉p_file_stat_del这个file_stat数据结构
unsigned int cold_file_stat_delete_all_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,char file_type)
{
	//struct file_stat * p_file_stat,*p_file_stat_temp;
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int del_file_area_count = 0;
    struct file_stat *p_file_stat_del;
    struct file_stat_small *p_file_stat_small_del;

	if(FILE_STAT_NORMAL == file_type)
		p_file_stat_del = container_of(p_file_stat_base,struct file_stat,file_stat_base);
	else 
		p_file_stat_small_del = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);

	//temp链表
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_base->file_area_temp,file_area_list){
		/* 新的版本hot_file_update_file_status()遇到refault、hot file_area，只是做个标记，而不会把file_area移动到
		 * file_stat->refault、hot链表，因此file_stat->temp链表上的file_area除了有in_temp_list标记，还有
		 * in_refault_list、in_hot_list标记，故要把file_area_in_temp_list_error(p_file_area)判断去掉*/
#if 1		
		//if(!file_area_in_temp_list(p_file_area) /*|| file_area_in_temp_list_error(p_file_area)*/)
		if(!file_area_in_temp_list(p_file_area) || (file_area_in_temp_list_error(p_file_area) && !file_area_in_hot_list(p_file_area)))
			panic("%s file_area:0x%llx status:%d not in file_area_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
#endif		

		cold_file_area_delete_quick(p_hot_cold_file_global,&p_file_stat_del->file_stat_base,p_file_area);
		del_file_area_count ++;
	}

	if(FILE_STAT_NORMAL == file_type){
		//refault链表
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_refault,file_area_list){
			if(!file_area_in_refault_list(p_file_area) || file_area_in_refault_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_refault\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,&p_file_stat_del->file_stat_base,p_file_area);
			del_file_area_count ++;
		}
		//hot链表
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_hot,file_area_list){
			if(!file_area_in_hot_list(p_file_area) || file_area_in_hot_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_hot\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,&p_file_stat_del->file_stat_base,p_file_area);
			del_file_area_count ++;
		}
		//free链表
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_free,file_area_list){
			/*file_area链表上的file_area可能被hot_file_update_file_status()并发设置了in_refautl状态但没来的及移动到
			 * file_stat->refault链表，故这里不能判断file_area的in_free状态错误*/
			if(!file_area_in_free_list(p_file_area) || (file_area_in_free_list_error(p_file_area) && !file_area_in_refault_list(p_file_area)))
				panic("%s file_area:0x%llx status:%d not in file_area_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_del,p_file_area);
			del_file_area_count ++;
		}
#if 0	
		//free_temp链表
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_free_temp,file_area_list){
			if(!file_area_in_free_list(p_file_area) || file_area_in_free_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_free_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_del,p_file_area);
			del_file_area_count ++;
		}
#endif	
		//mapcount链表
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_mapcount,file_area_list){
			if(!file_area_in_mapcount_list(p_file_area) || file_area_in_mapcount_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_mapcount\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,&p_file_stat_del->file_stat_base,p_file_area);
			del_file_area_count ++;
		}
		//warm链表,注意，该file_area可能在update函数被并发设置了in_hot标记
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_warm,file_area_list){
			if(!file_area_in_warm_list(p_file_area) || (file_area_in_warm_list_error(p_file_area) && !file_area_in_hot_list(p_file_area)))
				panic("%s file_area:0x%llx status:%d not in file_area_wram\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,&p_file_stat_del->file_stat_base,p_file_area);
			del_file_area_count ++;
		}
	
		if(p_file_stat_del->file_area_count != 0){
		    panic("file_stat_del:0x%llx file_area_count:%d !=0 !!!!!!!!\n",(u64)p_file_stat_del,p_file_stat_del->file_area_count);
	    }
	}else{
		if(p_file_stat_small_del->file_area_count != 0){
		    panic("file_stat_del:0x%llx file_area_count:%d !=0 !!!!!!!!\n",(u64)p_file_stat_small_del,p_file_stat_small_del->file_area_count);
	    }
	}

	//把file_stat从p_hot_cold_file_global的链表中剔除，然后释放file_stat结构
	cold_file_stat_delete_quick(p_hot_cold_file_global,p_file_stat_base,file_type);

	return del_file_area_count;
}
//如果一个文件file_stat超过一定比例(比如50%)的file_area都是热的，则判定该文件file_stat是热文件，file_stat要移动到global file_stat_hot_head链表。返回1是热文件
static int inline is_file_stat_hot_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat){
	int ret;
#if 0
	//如果文件file_stat的file_area个数比较少，则比例按照50%计算
	if(p_file_stat->file_area_count < p_hot_cold_file_global->file_area_level_for_large_file){
		//超过50%的file_area是热的，则判定文件file_stat是热文件
		//if(div64_u64((u64)p_file_stat->file_area_count*100,(u64)p_file_stat->file_area_hot_count) > 50)
		if(p_file_stat->file_area_hot_count > p_file_stat->file_area_count >> 1)
			ret = 1;
		else
			ret = 0;
	}else{
		//否则，文件很大，则必须热file_area超过文件总file_area数的很多很多，才能判定是热文件。因为此时file_area很多，冷file_area的数目有很多，应该遍历回收这种file_area的page
		if(p_file_stat->file_area_hot_count > (p_file_stat->file_area_count - (p_file_stat->file_area_count >> 2)))
			ret  = 1;
		else
			ret =  0;
	}
#else
	//热文件标准：统一改成7/8及以上的file_area都是热的
	if(p_file_stat->file_area_hot_count >= (p_file_stat->file_area_count - (p_file_stat->file_area_count >> 3)))
		ret  = 1;
	else
		ret =  0;
#endif	
	return ret;
}
EXPORT_SYMBOL(cold_file_stat_delete_all_file_area);
#if 0
//当文件file_stat的file_area个数超过阀值则判定是大文件
static int inline is_file_stat_large_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat)
{
	if(p_file_stat->file_area_count > hot_cold_file_global_info.file_area_level_for_large_file)
		return 1;
	else
		return 0;
}
#endif
static int inline is_file_stat_file_type(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat)
{
	if(p_file_stat->file_area_count < hot_cold_file_global_info.file_area_level_for_middle_file)
		return TEMP_FILE;
	else if(p_file_stat->file_area_count < hot_cold_file_global_info.file_area_level_for_large_file)
		return MIDDLE_FILE;
	else
		return LARGE_FILE;
}

/**************************************************************************************/

static int inline is_file_area_move_list_head(struct file_area *p_file_area)
{
#if 0	
	/*如果file_area当前周期内被访问次数达到阀值，则被移动到链表头。此时file_area可能处于file_stat的hot、refault、temp链表。必须是
	 *if(file_area_access_count_get(p_file_area) == PAGE_COUNT_IN_AREA)，否则之后file_area每被访问一次，就要向链表头移动一次，太浪费性能。
	 *目前限定每个周期内，file_area只能向file_stat的链表头移动一次。为了降低性能损耗，感觉还是性能损耗有点大，比如访问一个2G的文件，从文件头
	 *到文件尾的page每个都被访问一遍，于是每个file_area的page都被访问一次，这样if(file_area_access_count_get(p_file_area) == PAGE_COUNT_IN_AREA)
	 *对每个file_area都成立，每个file_area都移动到file_area->hot、refault、temp链表头，太浪费性能了。于是把就调整成
	 *file_area_access_count_get(p_file_area) > PAGE_COUNT_IN_AREA了!!!!!!!!!!!但这样有个问题，就是不能保证file_area被访问过就立即移动到
	 *file_area->hot、refault、temp链表头，链表尾的file_area就不能保证全是冷file_area了。没办法，性能损耗比较大损耗也是要考虑的!!!!!!!!!!!!!!!!!!!*/
	//if((hot_cold_file_global_info.global_age == p_file_area->file_area_age) && (file_area_access_count_get(p_file_area) == PAGE_COUNT_IN_AREA)){
	if((hot_cold_file_global_info.global_age == p_file_area->file_area_age) && (file_area_access_count_get(p_file_area) > PAGE_COUNT_IN_AREA)){
		return 1;
	}
	/*如果上个周期file_area被访问过，下个周期file_area又被访问，则也把file_area移动到链表头。file_area_access_count_get(p_file_area) > 0
	 *表示上个周期file_area被访问过，hot_cold_file_global_info.global_age - p_file_area->file_area_age == 1表示是连续的两个周期*/
	else if((hot_cold_file_global_info.global_age - p_file_area->file_area_age == 1) && (file_area_access_count_get(p_file_area) > 0)){
		return 1;
	}
#else
	/*如果file_area在两个周期及以上，至少有两个周期file_aera被访问了，才把file_area移动到各自的链表头。比如在global_age周期5被访问了1次，
	 *file_area->file_area_age被赋值5 ,file_area->access_count被赋值1。等到global_age周期7，file_area又被访问了。此时age_dx=7-5=2，大于0且小于
	 FILE_AREA_MOVE_HEAD_DX，file_area->access_count又大于0，说明file_area在周期5或6被访问过一次而大于0 。那也有可能是周期7被访问导致大于0
	 的呀????????不可能，因为file_area在新的周期被访问后，要先对file_area=global_age，且对file_area->access_count清0 .age_dx大于0且
	 ,file_area->access_count大于1，一定file_area在前几个周期被访问过。现在执行is_file_area_move_list_head()函数说明当前周期又被访问了。
	 最终效果，检测出file_area至少在连续的多个周期被访问了两次，则都移动链表头。不是一次就移动链表头主要是为了过滤一时的抖动访问。*/
	int age_dx = hot_cold_file_global_info.global_age - p_file_area->file_area_age;
	if((age_dx > 0 && age_dx < FILE_AREA_MOVE_HEAD_DX) && (file_area_access_count_get(p_file_area) > 0))
		return 1;
#endif	
	return 0;
}
static int inline is_file_area_hot(struct file_area *p_file_area)
{
	if(file_area_access_count_get(p_file_area) > FILE_AREA_HOT_LEVEL)
		return 1;

	return 0;
}
/* 第3个版本------源码大幅减少、大大减少spin_lock(&p_file_stat->file_stat_lock)锁使用，降低性能损耗
 * 这个版本最大的改动是：
 * 1：当file_area的状态发生变化，不再在file_stat->temp、hot、refault链表之间相互跳转，而只是设置file_area的标记。
 * 目的有两个：第1是减少使用spin_lock(&p_file_stat->file_stat_lock)锁，减少与异步内存回收线程争抢file_stat_lock
 * 锁而减少性能损耗。第2是因为，异步内存回收线程也会在file_stat->temp、hot、refault链表之间相互移动file_area，
 * 这样就与当前函数hot_file_update_file_status()在file_stat->temp、hot、refault链表之间相互移动file_area形成
 * 并发，很容易因二者并发导致file_area设置设置错，移动到错误的file_stat->temp、hot、refault链表
 * 2：只有file_stat->temp链表的file_area被多次访问，才会file_stat_lock加锁移动到file_area到file_stat->temp链表头。
 * file_stat->hot、refault、warm链表上file_area被多次访问，只是设置file_area的ahead标记，不会再把file_area移动到
 * 各自的链表头。还是为了减少性能损耗！简单说，file_area被多次访问只是设置ahead标记，而不是移动到各自file_stat链表头
 * */
void hot_file_update_file_status(struct address_space *mapping,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,int access_count,int read_or_write)
{
	//检测file_area被访问的次数，判断是否有必要移动到file_stat->hot、refault、temp等链表头
	int file_area_move_list_head = is_file_area_move_list_head(p_file_area);
	int is_mmap_file = file_stat_in_mmap_file(p_file_stat_base);
	int is_cache_file = file_stat_in_cache_file(p_file_stat_base);
	unsigned int file_area_state;

	if(!enable_update_file_area_age)
		return;

	//file_area_in_update_count ++;
	/*hot_cold_file_global_info.global_age更新了，把最新的global age更新到本次访问的file_area->file_area_age。并对
	 * file_area->access_count清0，本周期被访问1次则加1.这段代码不管理会并发，只是一个赋值*/
	if(p_file_area->file_area_age < hot_cold_file_global_info.global_age){
		p_file_area->file_area_age = hot_cold_file_global_info.global_age;
		/*文件file_stat最近一次被访问时的全局age，不是file_area的。内存回收时如果file_stat的recent_access_age偏大，直接跳过。
		 *还有一点 file_stat和recent_access_age和cooling_off_start_age公用union类型变量，mmap文件用到cooling_off_start_age。
		 *这里会修改cooling_off_start_age，会影响mmap文件的cooling_off_start_age冷却期的判定*/
		p_file_stat->recent_access_age = hot_cold_file_global_info.global_age;

		/*file_area访问计数清0，这个必须放到is_file_area_move_list_head()后边，因为is_file_area_move_list_head()依赖这个访问计数*/
		if(file_area_access_count_get(p_file_area) && is_cache_file)
			file_area_access_count_clear(p_file_area);
		/*新的周期重置p_file_stat->file_area_move_to_head_count。但如果file_stat的file_area个数太少，
		 *file_area_move_to_head_count直接赋值0，因为file_area个数太少就没有必要再向file_stat->temp链表头移动file_area了*/
		if(p_file_stat_base->file_area_count >= FILE_AREA_MOVE_TO_HEAD_LEVEL)
			p_file_stat_base->file_area_move_to_head_count = hot_cold_file_global_info.file_area_move_to_head_count_max;
		else
			p_file_stat_base->file_area_move_to_head_count = 0;
	}

	/*file_area的page是被读，则标记file_read读，内存回收时跳过这种file_area的page，优先回收write的*/
	if(!file_area_page_is_read(p_file_area) && (FILE_AREA_PAGE_IS_READ == read_or_write)){
		set_file_area_page_read(p_file_area);
	}

	/*如果mmap的文件file_area里有cache page被read/write读写，也让这些page参与file_area_page_is_read、
	 *file_area_in_ahead判断，但不能按照cache文件file_area在各个链表之间移动到的逻辑处理，于是这里要return。
	 *其实想想也没事呀，mmap文件的cache file_area难道不应该按照cache文件逻辑处理？主要这个file_area可能有mmap page，此时就不能了*/
	if(is_mmap_file){
		if(file_area_move_list_head && !file_area_in_ahead(p_file_area))
			set_file_area_in_ahead(p_file_area);

		return;
	}
	/*既没有mmap_file标记也没有cache_file标记，说明该file_stat正在cache_file_stat_move_to_mmap_head()函数中从
	 *global temp链表移动到global mmap_file_temp链表。此时直接return，不再标记file_area的任何状态，不再移动
	 *file_area到file_stat到任何链表.因为file_area里可能既有mmap page，又有cache page，因此不能按照cache文件逻辑处理*/
	else if(!is_cache_file)
		return;

	/*file_area访问的次数加access_count，是原子操作，不用担心并发。*/
	file_area_access_count_add(p_file_area,access_count);

	file_area_state = get_file_area_list_status(p_file_area);
	switch(file_area_state){
		/*file_stat->temp链表上file_area被多次访问则移动到file_area->temp链表头。
		 *被频繁访问则标记file_area的hot标记，不再移动file_area到file_stat->hot链表*/
		//if(file_area_in_temp_list(p_file_area) && !file_area_in_hot_list(p_file_area)){
         case file_area_in_temp_list_not_have_hot_status:
			/*file_stat->temp链表上的file_area被频繁访问后，只是设置file_area的hot标记，不会立即移动到file_stat->hot链表，在异步内存回收线程里实现*/
			if(/*!file_area_in_hot_list(p_file_area) && */is_file_area_hot(p_file_area)){
				/* 不清理file_area in_temp_list状态，等把file_area移动到file_stat->hot链表后再清理，目的是:
				 * 如果重复把这些hot file_area移动挂到file_stat->hot链表，则触发crash*/
				//clear_file_area_in_temp_list(p_file_area);
				set_file_area_in_hot_list(p_file_area);
			}
			/*file_stat->temp链表上file_area被多次访问，移动到file_stat->temp链表头*/
			else if(file_area_move_list_head){
				hot_cold_file_global_info.update_file_area_move_to_head_count ++;

				/*每加锁向file_stat->temp链表头移动一次file_area，file_area_move_to_head_count减1，达到0禁止再移动file_stat->temp
				 *链表头。等下个周期再给p_file_stat->file_area_move_to_head_count赋值16或32。file_area_move_to_head_count
				 *表示一个周期内运行一个文件file_stat允许向file_stat->temp链表头移动file_area的次数。主要目的还是减少争抢锁降低损耗*/
				if(!list_is_first(&p_file_area->file_area_list,&p_file_stat_base->file_area_temp)){
					/* file_area_move_to_head_count是无符号树，因为可能异步内存回收线程里对file_area_move_to_head_count赋值0，然后这里
					 * 再执行p_file_stat->file_area_move_to_head_count --，就小于0了。不过没关系，file_area_move_to_head_count必须大于0
					 * 才允许把file_stat->temp链表的file_area移动到链表头*/
					if(p_file_stat_base->file_area_move_to_head_count > 0){
						spin_lock(&p_file_stat_base->file_stat_lock);
						/*加锁后必须再判断一次file_area是否状态变化了，异步内存回收线程可能会改变file_area的状态*/
						if(file_area_in_temp_list(p_file_area)){
							list_move(&p_file_area->file_area_list,&p_file_stat_base->file_area_temp);
							if(!file_area_in_ahead(p_file_area))
								set_file_area_in_ahead(p_file_area);
						}
						spin_unlock(&p_file_stat_base->file_stat_lock);
						p_file_stat_base->file_area_move_to_head_count --;
						//file_area_in_update_lock_count ++;
						//file_area_move_to_head_count ++;
					}
					/*如果一个周期文件的file_area移动到file_stat->temp链表头次数超过限制，后续又有file_area因多次访问
					 *而需要移动到file_stat->temp链表头，只是标记file_area的ahead标记，不再加锁移动到file_area到链表头*/
					else{
						if(!file_area_in_ahead(p_file_area))
							set_file_area_in_ahead(p_file_area);
					}
				}
			}

			hot_cold_file_global_info.update_file_area_temp_list_count ++;
			break;
		//}

		//else if(file_area_in_warm_list(p_file_area) && !file_area_in_hot_list(p_file_area)){
		case file_area_in_warm_list_not_have_hot_status:
			if(is_file_area_hot(p_file_area)){
				//clear_file_area_in_temp_list(p_file_area);
				set_file_area_in_hot_list(p_file_area);
			}
			else if(file_area_move_list_head){
				if(file_area_in_ahead(p_file_area))
				    set_file_area_in_ahead(p_file_area);

				hot_cold_file_global_info.update_file_area_move_to_head_count ++;
            }
			hot_cold_file_global_info.update_file_area_warm_list_count ++;
			break;
		//}

		/*file_stat->free链表上的file_area被访问，只是标记file_area in_free_list，异步内存回收线程里再把该file_area移动到file_stat->refault链表*/
		//else if(file_area_in_free_list(p_file_area) && !file_area_in_refault_list(p_file_area)){
		case file_area_in_free_list_not_have_refault_status:
			/* 标记file_area in_refault_list后，不清理file_area in_free_list状态，只有把file_area移动到
			 * file_stat->refault链表时再清理掉。目的是防止这种file_area在file_stat_other_list_file_area_solve()
			 * 中重复把这种file_area移动file_area->refault链表*/
			//clear_file_area_in_free_list(p_file_area);
			set_file_area_in_refault_list(p_file_area);
			hot_cold_file_global_info.update_file_area_free_list_count ++;
			break;
		//}

		/*其他情况，对file_stat->refault、hot链表上file_area的处理。如果file_area被多次访问而需要移动到各自的链表头，
		 *这里只是标记file_area的ahead标记，不再移动到链表头，降低使用file_stat_lock锁，降低性能损耗*/
		//else{
		default:
			hot_cold_file_global_info.update_file_area_other_list_count ++;
			if(file_area_move_list_head){
				if(!file_area_in_ahead(p_file_area))
				    set_file_area_in_ahead(p_file_area);

				hot_cold_file_global_info.update_file_area_move_to_head_count ++;
			}

			break;
		//}
	}

	return;
}

/* 统计page的冷热信息、是否refault。返回0表示page和file_area没有，需要分配；返回1是成功返回；返回负数有出错
 */
#if 0 /*第2个版本，舍弃*/  
int hot_file_update_file_status(struct address_space *mapping,struct file_stat *p_file_stat,struct file_area *p_file_area,int access_count)
{
	//检测file_area被访问的次数，判断是否有必要移动到file_stat->hot、refault、temp等链表头
	int file_area_move_list_head = is_file_area_move_list_head(p_file_area);
	int is_mmap_file = file_stat_in_mmap_file(p_file_stat);

	file_area_in_update_count ++;
	/*hot_cold_file_global_info.global_age更新了，把最新的global age更新到本次访问的file_area->file_area_age。并对
	 * file_area->access_count清0，本周期被访问1次则加1.这段代码不管理会并发，只是一个赋值*/
	if(p_file_area->file_area_age < hot_cold_file_global_info.global_age){
		p_file_area->file_area_age = hot_cold_file_global_info.global_age;
		/*文件file_stat最近一次被访问时的全局age，不是file_area的。内存回收时如果file_stat的recent_access_age偏大，直接跳过。
		 *还有一点 file_stat和recent_access_age和cooling_off_start_age公用union类型变量，mmap文件用到cooling_off_start_age。
		 *这里会修改cooling_off_start_age，会影响mmap文件的cooling_off_start_age冷却期的判定*/
		p_file_stat->recent_access_age = hot_cold_file_global_info.global_age;

		/*file_area访问计数清0，这个必须放到is_file_area_move_list_head()后边，因为is_file_area_move_list_head()依赖这个访问计数*/
		if(file_area_access_count_get(p_file_area) && !is_mmap_file)
			file_area_access_count_clear(p_file_area);
	}
	if(is_mmap_file)
		return 1;

	/*在file_stat被判定为热文件后，记录当时的global_age。在未来HOT_FILE_COLD_AGE_DX时间内该文件进去冷却期：hot_file_update_file_status()函数中
	 *只更新该文件file_area的age后，然后函数返回，不再做其他操作，节省性能*/
	if(p_file_stat->file_stat_hot_base_age && (p_file_stat->file_stat_hot_base_age + HOT_FILE_COLD_AGE_DX > hot_cold_file_global_info.global_age))
		goto out;

	/*file_area访问的次数加access_count，是原子操作，不用担心并发*/
	file_area_access_count_add(p_file_area,access_count);

	/*只有以下几种情况，才会执行下边spin_lock(&p_file_stat->file_stat_lock)里的代码
	  1：不管file_area处于file_stat的哪个链表，只要file_area_move_list_head大于0，就要移动到所处file_stat->file_area_temp、file_area_hot、
	  file_area_refault、file_area_free_temp、file_area_free 链表头
     2: file_area处于 tmemp链表，但是单个周期内访问计数大于热file_area阀值，要晋级为热file_area
     3：file_area处于in-free-list 链表，要晋级到refault链表
    */
	if(file_area_in_temp_list(p_file_area)){
		int hot_file_area = is_file_area_hot(p_file_area);

		/*file_area连续几个周期访问且不是热文件，需要移动到file_stat->temp链表头*/
		if(!hot_file_area && file_area_move_list_head){
			/*能否把加锁代码放到if判断里边呢？这就违反加锁后判断状态原则了，因为在加锁过程file_area状态可能就变了。
			 *解决办法是加锁后再判断一次状态。否则，可能发生file_area被其他进程并发移动到了file_stat->hot链表，
			 这里却把它移动到了file_stat->temp链表头，状态错乱了。*/
			spin_lock(&p_file_stat->file_stat_lock);
			file_area_in_update_lock_count ++;
			file_area_move_to_head_count ++;
			if(file_area_in_temp_list(p_file_area) && !list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_temp)){
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
			}
			spin_unlock(&p_file_stat->file_stat_lock);
		}
		/*上边只是file_area移动到各自的链表头，不能跨链表移动。因为异步内存回收线程可能正在回收当前file_stat各种链表上的file_area，
		 *会把file_area移动到file_stat的各种链表，这里就不能随便移动该file_stat各种链表上的file_area了，会发生"遍历的链表成员被移动
		 到其他链表，因为链表头变了导致的遍历陷入死循环"问题。跨链表移动是在下边进行，保证此时异步内存回收线程没有对file_stat在内存回收。*/

		/*如果file_stat的file_area正处于正释放page状态，此时异步内存回收线程会遍历file_stat->file_area_temp、file_area_hot、file_area_refault、
		 * file_area_free_temp、file_area_free 链表上的file_area。此时禁止hot_file_update_file_status()函数里将file_stat这些链表上的file_area
		 * 跨链表移动。为什么？比如异步内存回收线程正遍历file_stat->file_area_free_temp 链表上的file_area1，但是hot_file_update_file_status()
		 * 函数里因为这个file_area1被访问了，而把file_area1移动到了file_stat->file_area_refault链表头。然后异步内存回收线程与得到
		 * file_area1在file_stat->file_area_free_temp链表的上一个file_area，此时得到到确是file_stat->file_area_refault链表头。相当于中途从
		 * file_stat->file_area_free_temp链表跳到了file_stat->file_area_refault链表，遍历file_area。这样遍历链表将陷入死循环，因为这个循环的
		 * 退出条件是遍历到最初的file_stat->file_area_free_temp链表头，但是现在只会遍历到file_stat->file_area_refault链表头，永远退不出循环。
		 * 这种现象这里称为"遍历的链表成员被移动到其他链表，因为链表头变了导致的遍历陷入死循环"*/

		else if(hot_file_area){/*file_area是热的*/
			spin_lock(&p_file_stat->file_stat_lock);
			file_area_in_update_lock_count ++;
			/*如果file_stat正在经历内存回收，则只是把热file_area移动到file_stat->temp链表头。否则直接移动到file_stat->hot链表*/
			//if(0 == file_stat_in_free_page(p_file_stat) && 0 == file_stat_in_free_page_done(p_file_stat))
			if(file_stat_in_file_stat_temp_head_list(p_file_stat)){
				/*热file_area，则把该file_area移动到file_area_hot链表。必须这里加锁后再判断一次file_area状态，因为可能异步内存回收线程里改变了
				 *它的状态并移动到了其他file_stat的file_area链表。这就是加锁后再判断一次状态理论*/
				if(file_area_in_temp_list(p_file_area)){
					clear_file_area_in_temp_list(p_file_area);
					//设置file_area 处于 file_area_hot链表
					set_file_area_in_hot_list(p_file_area);
					//把file_area移动到file_area_hot链表头，将来这些file_area很少访问了，还会再降级移动回file_area_temp链表头
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
					//一个周期内产生的热file_area个数
					hot_cold_file_global_info.hot_cold_file_shrink_counter.hot_file_area_count_one_period ++;
					//该文件的热file_stat数加1
					p_file_stat->file_area_hot_count ++;
				}
			}else{
				file_area_move_to_head_count ++;
				/*否则，说明file_stat此时正在内存回收，不能跨链表移动，只能先暂时移动到链表头。等内存回收结束，会遍历链表头的file_area
				 *此时会把这些file_area移动到file_stat->hot链表*/
				if(!list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_temp))
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
			}
			spin_unlock(&p_file_stat->file_stat_lock);
		}
		/*如果file_area处于temp链表，但是p_file_area->shrink_time不是0.这说明该file_area在之前walk_throuth_all_file_area()函数中扫描
		  判定该file_area是冷的，然后回收内存page。但是回收内存时，正好这个file_area又被访问了，则把file_area移动到file_stat->file_area_temp
		  链表。但是内存回收流程执行到cold_file_isolate_lru_pages()函数因并发问题没发现该file_area最近被访问了，只能继续回收该file_area的page。
		  需要避免回收这种热file_area的page。于是等该file_area下次被访问，执行到这里，if成立，把该file_area移动到file_stat->file_area_refault
		  链表。这样未来一段较长时间可以避免再次回收该file_area的page。具体详情看cold_file_isolate_lru_pages()函数里的注释*/
		else if(p_file_area->shrink_time != 0){//这个if是可能成立的，不能删除!!!!!!!!!!!!!!!
			printk("%s refaut 0x%llx shrink_time:%d\n",__func__,(u64)p_file_area,p_file_area->shrink_time);
			p_file_area->shrink_time = 0;

			spin_lock(&p_file_stat->file_stat_lock);
			clear_file_area_in_temp_list(p_file_area);
			set_file_area_in_refault_list(p_file_area);
			list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
			spin_unlock(&p_file_stat->file_stat_lock);

			//一个周期内产生的refault file_area个数
			hot_cold_file_global_info.hot_cold_file_shrink_counter.refault_file_area_count_one_period ++;
			hot_cold_file_global_info.all_refault_count ++;
			hot_cold_file_global_info.refault_file_area_count_in_free_page ++;
		}
	}else if(file_area_in_free_list(p_file_area)){

		spin_lock(&p_file_stat->file_stat_lock);

		file_area_in_update_lock_count ++;
		/*file_stat处于内存回收时，只是把file_area移动到file_stat->free链表头。否则直接把file_area移动到file_stat->temp或file_stat->refault链表*/
		if(file_stat_in_file_stat_temp_head_list(p_file_stat)){

			clear_file_area_in_free_list(p_file_area);
			//file_area 的page被内存回收后，过了仅N秒左右就又被访问则发生了refault，把该file_area移动到file_area_refault链表，不再参与内存回收扫描!!!!需要设个保护期限制
			smp_rmb();
			if(p_file_area->shrink_time && (ktime_to_ms(ktime_get()) - (p_file_area->shrink_time << 10) < 60000)){
				p_file_area->shrink_time = 0;
				set_file_area_in_refault_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
				//一个周期内产生的refault file_area个数
				hot_cold_file_global_info.hot_cold_file_shrink_counter.refault_file_area_count_one_period ++;
				hot_cold_file_global_info.all_refault_count ++;
				hot_cold_file_global_info.refault_file_area_count_in_free_page ++;
			}else{
				p_file_area->shrink_time = 0;
				//file_area此时正在被内存回收而移动到了file_stat的free_list或free_temp_list链表，则直接移动到file_stat->file_area_temp链表头
				set_file_area_in_temp_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
			}
		}
		/*如果file_area处于in_free_list链表，第1次访问就移动到链表头。因为这种file_area可能被判定为refault file_araa，精度要求高.file_area在内存回收
		 *时一直是in_free_list状态，状态不会改变，也不会移动到其他链表！这个时间可能被频繁访问，只有每个周期内第一次被访问才移动到俩表头*/
		//if(file_area_in_free_list(p_file_area) && file_area_access_count_get(p_file_area) == 1)
		else
		{
			file_area_move_to_head_count ++;
			if(file_stat_in_free_page(p_file_stat)){//file_stat是in_free_page状态且file_area在file_stat->file_area_free_temp链表
				if(!list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_free_temp))
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free_temp);
			}else if(file_stat_in_free_page_done(p_file_stat)){//file_stat是in_free_page_done状态且file_area在file_stat->file_area_free链表
				if(!list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_free))
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free);
			}else{
				panic("%s file_stat:0x%llx status:0x%lx error\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			}
		}

		spin_unlock(&p_file_stat->file_stat_lock);
	}else{
		/*如果file_area在当前周期第2次被访问，则把移动到file_stat->file_area_temp、file_area_hot、file_area_refault等链表头，该链表头的file_area
		 *访问比较频繁，链表尾的file_area很少访问。将来walk_throuth_all_file_area()内存回收时，直接从这些链表尾遍历file_area即可，链表尾的都是冷
		 file_area。随之而来一个难题就是，file_area每个周期第几次被访问移动到链表头呢？最开始是1，现在改成每个周期第2次被访问再移动到链表头了。
		 因为可能有不少file_area一个周期就访问一次，就移动到链表头，性能损耗比较大，因为这个过程要spin_lock加锁。这样的话就又有一个新的问题，
		 如果file_area不是第一次访问就移动到链表头，那链表尾的file_area就不全是冷file_area了。因为链表头掺杂着最近刚访问但是只访问了一次的file_area，
		 这是热file_area！针对这个问题的解决方法是，在异步内存回收线程依次执行get_file_area_from_file_stat_list、free_page_from_file_area、
		 walk_throuth_all_file_area函数，从file_stat->file_area_temp、file_area_hot、file_area_refault链表尾遍历file_area时，发现了热file_area，即
		 file_area的age接近global age，但是file_area的访问次数是1，那还要继续遍历链表，直到连续遇到3~5个热file_area时，才能说明这个链表没冷file_area
		 了，再结束遍历。

		 这是最初的策略，现在修改成file_area被访问则移动到file_stat的hot、refault、temp链表头，要经过前边的
		 file_area_move_list_head = is_file_area_move_list_head(p_file_area)判断，file_area_move_list_head为1才会把file_area移动到链表头
		 */

		if(file_area_move_list_head){
			/*能否把加锁代码放到if判断里边呢？这就违反加锁后判断状态原则了，因为在加锁过程file_area状态可能就变了。
			 *解决办法是加锁后再判断一次状态，太麻烦了。后续看情况再决定咋优化吧*/
			spin_lock(&p_file_stat->file_stat_lock);
			file_area_in_update_lock_count ++;
			file_area_move_to_head_count ++;

			if(file_area_in_hot_list(p_file_area) && !list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_hot)){
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
			}else if(file_area_in_refault_list(p_file_area) && !list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_refault)){
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
			}

			spin_unlock(&p_file_stat->file_stat_lock);
		}
	}

out:
	return 1;
}
#endif

#if 0 /*第1个版本，舍弃*/
int hot_file_update_file_status(struct address_space *mapping,struct file_stat *p_file_stat,struct file_area *p_file_area,int access_count)
{
	int file_area_move_list_head = 0;
	int hot_file_area; 
#if 0
	//这些判断放到每个使用者函数最初吧，否则每次都执行浪费性能
	if(NULL == p_file_stat || file_stat_in_delete(p_file_stat) || 0 == access_count){
		return 0;
	}
#endif

#if 0
	//async_memory_reclaim_status不再使用smp_rmb内存屏障，而直接使用test_and_set_bit_lock/clear_bit_unlock原子操作
	if(unlikely(!test_bit(ASYNC_MEMORY_RECLAIM_ENABLE,&async_memory_reclaim_status)))
		return -1;
	/*1:如果mapping->rh_reserved1被其他代码使用，直接返回错误*/
	if(p_file_stat->mapping != mapping || access_count <= 0){
		printk("%s p_file_stat:0x%llx status:0x%lx access_count:%d error\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status,access_count);
		return -1;
	}
#endif		

	//检测file_area被访问的次数，判断是否有必要移动到file_stat->hot、refault、temp等链表头
	file_area_move_list_head = is_file_area_move_list_head(p_file_area);
	hot_file_area = is_file_area_hot(p_file_area);

	/*hot_cold_file_global_info.global_age更新了，把最新的global age更新到本次访问的file_area->file_area_age。并对
	 * file_area->access_count清0，本周期被访问1次则加1.这段代码不管理会并发，只是一个赋值*/
	if(p_file_area->file_area_age < hot_cold_file_global_info.global_age){
		p_file_area->file_area_age = hot_cold_file_global_info.global_age;
#if 0	
		if(p_file_area->file_area_age > p_file_stat->max_file_area_age)
			p_file_stat->max_file_area_age = p_file_area->file_area_age;
#endif			

		//file_area访问计数清0
		file_area_access_count_clear(p_file_area);
	}
	//file_area访问的次数加access_count，是原子操作，不用担心并发
	file_area_access_count_add(p_file_area,access_count);

	/*只有以下几种情况，才会执行下边spin_lock(&p_file_stat->file_stat_lock)里的代码
	  1：不管file_area处于file_stat的哪个链表，只要file_area_move_list_head大于0，就要移动到所处file_stat->file_area_temp、file_area_hot、
	  file_area_refault、file_area_free_temp、file_area_free 链表头
2: file_area处于 tmemp链表，但是单个周期内访问计数大于热file_area阀值，要晋级为热file_area
3：file_area处于in-free-list 链表，要晋级到refault链表
*/
	if(!(file_area_move_list_head || file_area_in_free_list(p_file_area) || hot_file_area))
	{
		//每个周期直接从file_area_tree找到file_area并且不用加锁次数加1
		hot_cold_file_global_info.hot_cold_file_shrink_counter.find_file_area_from_tree_not_lock_count ++;
		goto out;
	}

	spin_lock(&p_file_stat->file_stat_lock);


	/*如果file_area在当前周期第2次被访问，则把移动到file_stat->file_area_temp、file_area_hot、file_area_refault等链表头，该链表头的file_area
	 *访问比较频繁，链表尾的file_area很少访问。将来walk_throuth_all_file_area()内存回收时，直接从这些链表尾遍历file_area即可，链表尾的都是冷
	 file_area。随之而来一个难题就是，file_area每个周期第几次被访问移动到链表头呢？最开始是1，现在改成每个周期第2次被访问再移动到链表头了。
	 因为可能有不少file_area一个周期就访问一次，就移动到链表头，性能损耗比较大，因为这个过程要spin_lock加锁。这样的话就又有一个新的问题，
	 如果file_area不是第一次访问就移动到链表头，那链表尾的file_area就不全是冷file_area了。因为链表头掺杂着最近刚访问但是只访问了一次的file_area，
	 这是热file_area！针对这个问题的解决方法是，在异步内存回收线程依次执行get_file_area_from_file_stat_list、free_page_from_file_area、
	 walk_throuth_all_file_area函数，从file_stat->file_area_temp、file_area_hot、file_area_refault链表尾遍历file_area时，发现了热file_area，即
	 file_area的age接近global age，但是file_area的访问次数是1，那还要继续遍历链表，直到连续遇到3~5个热file_area时，才能说明这个链表没冷file_area
	 了，再结束遍历。

	 这是最初的策略，现在修改成file_area被访问则移动到file_stat的hot、refault、temp链表头，要经过前边的
	 file_area_move_list_head = is_file_area_move_list_head(p_file_area)判断，file_area_move_list_head为1才会把file_area移动到链表头
	 */
	if(file_area_move_list_head && (hot_file_area == 0))/******情况1***************************/
	{
		/*如果p_file_area不在file_area_hot或file_area_temp链表头，才把它添加到file_area_hot或file_area_temp链表头
		  file_stat的file_area_hot或file_area_temp链表头的file_area是最频繁访问的，链表尾的file_area访问频次低，内存回收光顾这些链表尾的file_area*/

		if(file_area_in_temp_list(p_file_area)){
			if(!list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_temp))
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
		}else if(file_area_in_hot_list(p_file_area)){
			if(!list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_hot))
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
		}else if(file_area_in_refault_list(p_file_area)){//在refault链表的file_area如果被访问了也移动到链表头
			if(!list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_refault))
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
		}

	}

	/*如果file_area处于in_free_list链表，第1次访问就移动到链表头。因为这种file_area可能被判定为refault file_araa，精度要求高.file_area在内存回收
	 *时一直是in_free_list状态，状态不会改变，也不会移动到其他链表！这个时间可能被频繁访问，只有每个周期内第一次被访问才移动到俩表头*/
	//if(file_area_in_free_list(p_file_area) && file_area_access_count_get(p_file_area) == 1)
	if(file_area_in_free_list(p_file_area))/******情况2***************************/
	{
		if(file_stat_in_free_page(p_file_stat)){//file_stat是in_free_page状态且file_area在file_stat->file_area_free_temp链表
			if(!list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_free_temp))
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free_temp);
		}else if(file_stat_in_free_page_done(p_file_stat)){//file_stat是in_free_page_done状态且file_area在file_stat->file_area_free链表
			if(!list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_free))
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free);
		}
	}

	/*上边只是file_area移动到各自的链表头，不能跨链表移动。因为异步内存回收线程可能正在回收当前file_stat各种链表上的file_area，
	 *会把file_area移动到file_stat的各种链表，这里就不能随便移动该file_stat各种链表上的file_area了，会发生"遍历的链表成员被移动
	 到其他链表，因为链表头变了导致的遍历陷入死循环"问题。跨链表移动是在下边进行，保证此时异步内存回收线程没有对file_stat在内存回收。*/


	/*如果file_stat的file_area正处于正释放page状态，此时异步内存回收线程会遍历file_stat->file_area_temp、file_area_hot、file_area_refault、
	 * file_area_free_temp、file_area_free 链表上的file_area。此时禁止hot_file_update_file_status()函数里将file_stat这些链表上的file_area
	 * 跨链表移动。为什么？比如异步内存回收线程正遍历file_stat->file_area_free_temp 链表上的file_area1，但是hot_file_update_file_status()
	 * 函数里因为这个file_area1被访问了，而把file_area1移动到了file_stat->file_area_refault链表头。然后异步内存回收线程与得到
	 * file_area1在file_stat->file_area_free_temp链表的上一个file_area，此时得到到确是file_stat->file_area_refault链表头。相当于中途从
	 * file_stat->file_area_free_temp链表跳到了file_stat->file_area_refault链表，遍历file_area。这样遍历链表将陷入死循环，因为这个循环的
	 * 退出条件是遍历到最初的file_stat->file_area_free_temp链表头，但是现在只会遍历到file_stat->file_area_refault链表头，永远退不出循环。
	 * 这种现象这里称为"遍历的链表成员被移动到其他链表，因为链表头变了导致的遍历陷入死循环"*/
	if(0 == file_stat_in_free_page(p_file_stat) && 0 == file_stat_in_free_page_done(p_file_stat))
		//if(file_stat_in_file_stat_temp_head_list(p_file_stat))//这个与file_stat_in_free_page(p_file_stat) == 0效果一致
	{
		/*热file_area，则把该file_area移动到file_area_hot链表。必须这里加锁后再判断一次file_area状态，因为可能异步内存回收线程里改变了
		 *它的状态并移动到了其他file_stat的file_area链表。这就是加锁后再判断一次状态理论*/
		if(hot_file_area && file_area_in_temp_list(p_file_area)){/******情况3***************************/
			clear_file_area_in_temp_list(p_file_area);
			//设置file_area 处于 file_area_hot链表
			set_file_area_in_hot_list(p_file_area);
			//把file_area移动到file_area_hot链表头，将来这些file_area很少访问了，还会再降级移动回file_area_temp链表头
			list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
			//一个周期内产生的热file_area个数
			hot_cold_file_global_info.hot_cold_file_shrink_counter.hot_file_area_count_one_period ++;
			//该文件的热file_stat数加1
			p_file_stat->file_area_hot_count ++;
		}
		//如果file_area处于file_stat的free_list或free_temp_list链表
		else if(file_area_in_free_list(p_file_area)){
			clear_file_area_in_free_list(p_file_area);
			//file_area 的page被内存回收后，过了仅N秒左右就又被访问则发生了refault，把该file_area移动到file_area_refault链表，不再参与内存回收扫描!!!!需要设个保护期限制
			smp_rmb();
			if(p_file_area->shrink_time && (ktime_to_ms(ktime_get()) - (p_file_area->shrink_time << 10) < 60000)){
				p_file_area->shrink_time = 0;
				set_file_area_in_refault_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
				//一个周期内产生的refault file_area个数
				hot_cold_file_global_info.hot_cold_file_shrink_counter.refault_file_area_count_one_period ++;
				hot_cold_file_global_info.all_refault_count ++;
				hot_cold_file_global_info.refault_file_area_count_in_free_page ++;
			}else{
				p_file_area->shrink_time = 0;
				//file_area此时正在被内存回收而移动到了file_stat的free_list或free_temp_list链表，则直接移动到file_stat->file_area_temp链表头
				set_file_area_in_temp_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
			}
		}
		/*如果file_area处于temp链表，但是p_file_area->shrink_time不是0.这说明该file_area在之前walk_throuth_all_file_area()函数中扫描
		  判定该file_area是冷的，然后回收内存page。但是回收内存时，正好这个file_area又被访问了，则把file_area移动到file_stat->file_area_temp
		  链表。但是内存回收流程执行到cold_file_isolate_lru_pages()函数因并发问题没发现该file_area最近被访问了，只能继续回收该file_area的page。
		  需要避免回收这种热file_area的page。于是等该file_area下次被访问，执行到这里，if成立，把该file_area移动到file_stat->file_area_refault
		  链表。这样未来一段较长时间可以避免再次回收该file_area的page。具体详情看cold_file_isolate_lru_pages()函数里的注释*/
		else if(file_area_in_temp_list(p_file_area) && (p_file_area->shrink_time != 0)){//这个if是可能成立的，不能删除!!!!!!!!!!!!!!!
			printk("%s refaut 0x%llx shrink_time:%d\n",__func__,(u64)p_file_area,p_file_area->shrink_time);
			p_file_area->shrink_time = 0;
			clear_file_area_in_temp_list(p_file_area);
			set_file_area_in_refault_list(p_file_area);
			list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
			//一个周期内产生的refault file_area个数
			hot_cold_file_global_info.hot_cold_file_shrink_counter.refault_file_area_count_one_period ++;
			hot_cold_file_global_info.all_refault_count ++;
			hot_cold_file_global_info.refault_file_area_count_in_free_page ++;
		}
	}

	spin_unlock(&p_file_stat->file_stat_lock);

out:
	return 1;
}
#endif
EXPORT_SYMBOL(hot_file_update_file_status);


/*****proc文件系统**********************************************************************************************************************/
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

	if(val < 100)
		hot_cold_file_global_info.file_area_hot_to_temp_age_dx = val;
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

	if(val < 100)
		hot_cold_file_global_info.file_area_refault_to_temp_age_dx = val;
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

	if(val < 100)
		hot_cold_file_global_info.file_area_temp_to_cold_age_dx = val;
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
	seq_printf(m, "%d\n", hot_cold_file_global_info.file_area_free_age_dx);
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

	if(val < 100)
		hot_cold_file_global_info.file_area_free_age_dx = val;
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

	if(val < 100)
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
void get_file_name(char *file_name_path,struct file_stat * p_file_stat)
{
	struct dentry *dentry = NULL;
	struct inode *inode = NULL;

	/*必须 hlist_empty()判断文件inode是否有dentry，没有则返回true。这里通过inode和dentry获取文件名字，必须 inode->i_lock加锁 
	 *同时 增加inode和dentry的应用计数，否则可能正使用时inode和dentry被其他进程释放了*/
	if(p_file_stat->mapping && p_file_stat->mapping->host && !hlist_empty(&p_file_stat->mapping->host->i_dentry)){
		inode = p_file_stat->mapping->host;
		spin_lock(&inode->i_lock);
		/*如果inode的引用计数是0，说明inode已经在释放环节了，不能再使用了。现在发现不一定，改为hlist_empty(&inode->i_dentry)判断*/
		if(/*atomic_read(&inode->i_count) > 0*/ !hlist_empty(&inode->i_dentry)){
			dentry = hlist_entry(inode->i_dentry.first, struct dentry, d_u.d_alias);
			//__dget(dentry);------这里不再__dget,因为全程有spin_lock(&inode->i_lock)加锁
			if(dentry){
				snprintf(file_name_path,MAX_FILE_NAME_LEN - 2,"i_count:%d dentry:0x%llx %s",atomic_read(&inode->i_count),(u64)dentry,/*dentry->d_iname*/dentry->d_name.name);
			}
			//dput(dentry);
		}
		spin_unlock(&inode->i_lock);
	}
}
static int print_one_list_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct seq_file *m,int is_proc_print,char *file_stat_name,struct list_head *file_stat_temp_head,
		unsigned int *file_stat_one_file_area_count,unsigned int *file_stat_many_file_area_count,unsigned int *file_stat_one_file_area_pages,unsigned int file_stat_in_list_type)
{
	struct file_stat *p_file_stat;
	unsigned int all_pages = 0;
	char file_name_path[MAX_FILE_NAME_LEN];
	unsigned int scan_file_stat_count = 0;

	if(!list_empty(file_stat_temp_head)){
		if(is_proc_print)
			seq_printf(m,"%s",file_stat_name);
		else	
			printk("%s",file_stat_name);
	}
	/* 如果在遍历file_stat过程，p_file_stat和它在链表的下一个file_stat即p_file_stat_temp都被iput释放了，
	 * 二者都被移动到了global delete链表，然后得到p_file_stat或p_file_stat_temp在链表的下一个file_stat，
	 * 就跨链表了，会陷入死循环，因为链表头由file_stat_temp_head变为了global delete。这样就会遍历链表
	 * 时因链表头变了而陷入死循环，老问题。怎么解决？目前暂定如果遍历到global delete链表上的file_stat，
	 * 立即break，或者限制scan_file_stat_count最大值，超过则break。
	 *
	 * 还有一种并发，如果proc打印文件信息，正打印global temp链表的file_stat文件。但是异步内存回收线程
	 * 把该file_stat从global temp链表移动到global hot链表，那就又要因为遍历global temp时链表头变了而
	 * 陷入死循环。怎么办？遍历global temp链表时，如果检测到file_stat没有in global temp list状态了，break*/
	list_for_each_entry_rcu(p_file_stat,file_stat_temp_head,hot_cold_file_list){
		scan_file_stat_count ++;

		/*异步内存线程打印文件信息时，只打印50个文件，防止刷屏打印*/
		if(!is_proc_print && scan_file_stat_count >= 50)
			break;
		/* 如果file_stat不在file_stat_in_list_type这个global链表上，说明file_stat被
		 * 异步内存回收线程移动到其他global 链表了，为陷入死循环必须break*/
		if(0 == (p_file_stat->file_stat_status & (1 << file_stat_in_list_type)))
			break;

		//atomic_inc(&hot_cold_file_global_info.ref_count);
		//lock_file_stat(p_file_stat,0);
		/*这里rcu_read_lock()，如果此时inode先被iput释放了，则file_stat_in_delete(p_file_stat)返回1，将不会再执行get_file_name()使用inode
		 *获取文件名字。如果rcu_read_lock()先执行，此时inode被iput()释放，就不会真正释放掉inode结构，直到rcu_read_unlock*/
		rcu_read_lock();
		/*如果file_stat对应的文件inode释放了，file_stat被标记了delete，此时不能再使用p_file_stat->mapping，因为mapping已经释放了.
		  但执行这个函数时，必须禁止执行cold_file_stat_delete_all_file_area()释放掉file_stat!!!!!!!!!!!!!!!!!!!!*/
		smp_rmb();//内存屏障获取最新的file_stat状态
		if(0 == file_stat_in_delete(p_file_stat)){
			if(p_file_stat->file_area_count > 1){
				*file_stat_many_file_area_count = *file_stat_many_file_area_count + 1;

				memset(file_name_path,0,sizeof(&file_name_path));
				get_file_name(file_name_path,p_file_stat);
				all_pages += p_file_stat->mapping->nrpages;

				if(is_proc_print)
					seq_printf(m,"file_stat:0x%llx max_age:%ld recent_traverse_age:%d file_area_count:%d nrpages:%ld %s\n",(u64)p_file_stat,p_file_stat->max_file_area_age,p_file_stat->recent_traverse_age,p_file_stat->file_area_count,p_file_stat->mapping->nrpages,file_name_path);
				else	
					printk("file_stat:0x%llx max_age:%ld recent_traverse_age:%d file_area_count:%d nrpages:%ld %s\n",(u64)p_file_stat,p_file_stat->max_file_area_age,p_file_stat->recent_traverse_age,p_file_stat->file_area_count,p_file_stat->mapping->nrpages,file_name_path);
			}
			else{
				*file_stat_one_file_area_count = *file_stat_one_file_area_count + 1;
				*file_stat_one_file_area_pages = *file_stat_one_file_area_pages + p_file_stat->mapping->nrpages;
			}
		}

		//unlock_file_stat(p_file_stat);
		//atomic_dec(&hot_cold_file_global_info.ref_count);
		rcu_read_unlock();

		if(need_resched())
			cond_resched();
	}

	return all_pages;
}
//遍历p_hot_cold_file_global各个链表上的file_stat的file_area个数及page个数
static noinline int hot_cold_file_print_all_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct seq_file *m,int is_proc_print)//is_proc_print:1 通过proc触发的打印
{
	unsigned int file_stat_one_file_area_count = 0,file_stat_many_file_area_count = 0;
	unsigned int file_stat_one_file_area_pages = 0,all_pages = 0;

	//如果驱动在卸载，禁止再打印file_stat信息
	if(!test_bit(ASYNC_MEMORY_RECLAIM_ENABLE,&async_memory_reclaim_status)){
		printk("async_memory_reclaime ko is remove\n");
		return 0;
	}

	all_pages += print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********cache file hot\n",&p_hot_cold_file_global->file_stat_hot_head,
			&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_hot_head_list);

	all_pages += print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********cache file temp\n",&p_hot_cold_file_global->file_stat_temp_head,
			&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_temp_head_list);
	all_pages += print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********cache file large\n",&p_hot_cold_file_global->file_stat_large_file_head,
			&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_large_file_head_list);
	all_pages += print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********cache file middle\n",&p_hot_cold_file_global->file_stat_middle_file_head,
			&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_middle_file_head_list);

	all_pages += print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********mmap file temp\n",&p_hot_cold_file_global->mmap_file_stat_temp_head,
			&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_temp_head_list);
	all_pages += print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********mmap file large\n",&p_hot_cold_file_global->mmap_file_stat_large_file_head,
			&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_large_file_head_list);
	all_pages += print_one_list_file_stat(p_hot_cold_file_global,m,is_proc_print,"*********mmap file middle\n",&p_hot_cold_file_global->mmap_file_stat_middle_file_head,
			&file_stat_one_file_area_count,&file_stat_many_file_area_count,&file_stat_one_file_area_pages,F_file_stat_in_file_stat_middle_file_head_list);

	if(is_proc_print)
		seq_printf(m,"file_stat_one_file_area_count:%d pages:%d  file_stat_many_file_area_count:%d all_pages:%d\n",file_stat_one_file_area_count,file_stat_one_file_area_pages,file_stat_many_file_area_count,all_pages);
	else	
		printk("file_stat_one_file_area_count:%d pages:%d  file_stat_many_file_area_count:%d all_pages:%d\n",file_stat_one_file_area_count,file_stat_one_file_area_pages,file_stat_many_file_area_count,all_pages);

	return 0;
}
static noinline void printk_shrink_param(struct hot_cold_file_global *p_hot_cold_file_global,struct seq_file *m,int is_proc_print)
{
	struct hot_cold_file_shrink_counter *p = &p_hot_cold_file_global->hot_cold_file_shrink_counter;
	struct mmap_file_shrink_counter *mp = &p_hot_cold_file_global->mmap_file_shrink_counter;
	if(is_proc_print){

		seq_printf(m,"\n\n********cache file********\n");
		seq_printf(m,"scan_cold_file_area_count_from_temp:%d scan_read_file_area_count_from_temp:%d scan_ahead_file_area_count_from_temp:%d scan_cold_file_area_count_from_warm:%d scan_read_file_area_count_from_warm:%d scan_ahead_file_area_count_from_warm:%d scan_file_area_count_from_warm:%d scan_cold_file_area_count_from_mmap_file:%d isolate_lru_pages_from_mmap_file:%d file_area_hot_to_warm_from_hot_file:%d isolate_lru_pages:%d free_pages_count:%d free_pages_from_mmap_file:%d find_mmap_page_count_from_cache_file:%d\n",p->scan_cold_file_area_count_from_temp,p->scan_read_file_area_count_from_temp,p->scan_ahead_file_area_count_from_temp,p->scan_cold_file_area_count_from_warm,p->scan_read_file_area_count_from_warm,p->scan_ahead_file_area_count_from_warm,p->scan_file_area_count_from_warm,p->scan_cold_file_area_count_from_mmap_file,p->isolate_lru_pages_from_mmap_file,p->file_area_hot_to_warm_from_hot_file,p->isolate_lru_pages,p->free_pages_count,p->free_pages_from_mmap_file,p->find_mmap_page_count_from_cache_file);

		seq_printf(m,"\ntemp_to_warm_file_area_count:%d warm_to_temp_file_area_count:%d del_file_area_count:%d del_file_stat_count:%d writeback_count:%d dirty_count:%d del_zero_file_area_file_stat_count:%d scan_zero_file_area_file_stat_count:%d file_area_refault_to_warm_list_count:%d file_area_hot_to_warm_list_count:%d file_area_free_count_from_free_list:%d temp_to_hot_file_area_count:%d warm_to_hot_file_area_count:%d scan_file_area_count:%d scan_file_stat_count:%d scan_delete_file_stat_count:%d\n",p->temp_to_warm_file_area_count,p->warm_to_temp_file_area_count,p->del_file_area_count,p->del_file_stat_count,p->writeback_count,p->dirty_count,p->del_zero_file_area_file_stat_count,p->scan_zero_file_area_file_stat_count,p->file_area_refault_to_warm_list_count,p->file_area_hot_to_warm_list_count,p->file_area_free_count_from_free_list,p->temp_to_hot_file_area_count,p->warm_to_hot_file_area_count,p->scan_file_area_count,p->scan_file_stat_count,p->scan_delete_file_stat_count);

		seq_printf(m,"\n\n********mmap file********\n");
		seq_printf(m,"isolate_lru_pages_from_warm:%d scan_cold_file_area_count_from_warm:%d isolate_lru_pages_from_temp:%d scan_cold_file_area_count_from_temp:%d temp_to_warm_file_area_count:%d scan_file_area_count:%d scan_file_stat_count:%d warm_to_temp_file_area_count:%d mmap_free_pages_count:%d scan_file_area_count_from_cache_file:%d scan_cold_file_area_count_from_cache_file:%d refault_to_warm_file_area_count:%d find_cache_page_count_from_mmap_file:%d\n",mp->isolate_lru_pages_from_warm,mp->scan_cold_file_area_count_from_warm,mp->isolate_lru_pages_from_temp,mp->scan_cold_file_area_count_from_temp,mp->temp_to_warm_file_area_count,mp->scan_file_area_count,mp->scan_file_stat_count,mp->warm_to_temp_file_area_count,mp->mmap_free_pages_count,mp->scan_file_area_count_from_cache_file,mp->scan_cold_file_area_count_from_cache_file,mp->refault_to_warm_file_area_count,mp->find_cache_page_count_from_mmap_file);

		seq_printf(m,"\ndel_file_stat_count:%d del_file_area_count:%d writeback_count:%d dirty_count:%d scan_mapcount_file_area_count:%d scan_hot_file_area_count:%d free_pages_from_cache_file:%d mapcount_to_warm_file_area_count:%d hot_to_warm_file_area_count:%d check_refault_file_area_count:%d free_file_area_count:%d temp_to_temp_head_file_area_count:%d scan_file_area_count_file_move_from_cache:%d mapcount_to_temp_file_area_count_from_mapcount_file:%d hot_to_temp_file_area_count_from_hot_file:%d\n",mp->del_file_stat_count,mp->del_file_area_count,mp->writeback_count,mp->dirty_count,mp->scan_mapcount_file_area_count,mp->scan_hot_file_area_count,mp->free_pages_from_cache_file,mp->mapcount_to_warm_file_area_count,mp->hot_to_warm_file_area_count,mp->check_refault_file_area_count,mp->free_file_area_count,mp->temp_to_temp_head_file_area_count,mp->scan_file_area_count_file_move_from_cache,mp->mapcount_to_temp_file_area_count_from_mapcount_file,mp->hot_to_temp_file_area_count_from_hot_file);

		seq_printf(m,"\n\n********global********\n");
		seq_printf(m,"0x%llx global_age:%d file_stat_count:%d file_stat_hot:%d file_stat_zero_file_area:%d file_stat_large_count:%d update_file_area_free_list_count:%ld update_file_area_warm_list_count:%ld update_file_area_temp_list_count:%ld update_file_area_other_list_count:%ld update_file_area_move_to_head_count:%ld free_pages:%ld free_mmap_pages:%ld check_refault_file_area_count:%ld check_mmap_refault_file_area_count:%ld\n",(u64)p_hot_cold_file_global,p_hot_cold_file_global->global_age,p_hot_cold_file_global->file_stat_count,p_hot_cold_file_global->file_stat_hot_count,p_hot_cold_file_global->file_stat_count_zero_file_area,p_hot_cold_file_global->file_stat_large_count,p_hot_cold_file_global->update_file_area_free_list_count,p_hot_cold_file_global->update_file_area_warm_list_count,p_hot_cold_file_global->update_file_area_temp_list_count,p_hot_cold_file_global->update_file_area_other_list_count,p_hot_cold_file_global->update_file_area_move_to_head_count,p_hot_cold_file_global->free_pages,p_hot_cold_file_global->free_mmap_pages,p_hot_cold_file_global->check_refault_file_area_count,p_hot_cold_file_global->check_mmap_refault_file_area_count);
	}else{
		printk("********cache file********\n");
		printk("scan_cold_file_area_count_from_temp:%d scan_read_file_area_count_from_temp:%d scan_ahead_file_area_count_from_temp:%d scan_cold_file_area_count_from_warm:%d scan_read_file_area_count_from_warm:%d scan_ahead_file_area_count_from_warm:%d scan_file_area_count_from_warm:%d scan_cold_file_area_count_from_mmap_file:%d isolate_lru_pages_from_mmap_file:%d file_area_hot_to_warm_from_hot_file:%d isolate_lru_pages:%d free_pages_count:%d free_pages_from_mmap_file:%d find_mmap_page_count_from_cache_file:%d\n",p->scan_cold_file_area_count_from_temp,p->scan_read_file_area_count_from_temp,p->scan_ahead_file_area_count_from_temp,p->scan_cold_file_area_count_from_warm,p->scan_read_file_area_count_from_warm,p->scan_ahead_file_area_count_from_warm,p->scan_file_area_count_from_warm,p->scan_cold_file_area_count_from_mmap_file,p->isolate_lru_pages_from_mmap_file,p->file_area_hot_to_warm_from_hot_file,p->isolate_lru_pages,p->free_pages_count,p->free_pages_from_mmap_file,p->find_mmap_page_count_from_cache_file);

		printk("temp_to_warm_file_area_count:%d warm_to_temp_file_area_count:%d del_file_area_count:%d del_file_stat_count:%d writeback_count:%d dirty_count:%d del_zero_file_area_file_stat_count:%d scan_zero_file_area_file_stat_count:%d file_area_refault_to_warm_list_count:%d file_area_hot_to_warm_list_count:%d file_area_free_count_from_free_list:%d temp_to_hot_file_area_count:%d warm_to_hot_file_area_count:%d scan_file_area_count:%d scan_file_stat_count:%d scan_delete_file_stat_count:%d\n",p->temp_to_warm_file_area_count,p->warm_to_temp_file_area_count,p->del_file_area_count,p->del_file_stat_count,p->writeback_count,p->dirty_count,p->del_zero_file_area_file_stat_count,p->scan_zero_file_area_file_stat_count,p->file_area_refault_to_warm_list_count,p->file_area_hot_to_warm_list_count,p->file_area_free_count_from_free_list,p->temp_to_hot_file_area_count,p->warm_to_hot_file_area_count,p->scan_file_area_count,p->scan_file_stat_count,p->scan_delete_file_stat_count);

		printk("********mmap file********\n");
		printk("isolate_lru_pages_from_warm:%d scan_cold_file_area_count_from_warm:%d isolate_lru_pages_from_temp:%d scan_cold_file_area_count_from_temp:%d temp_to_warm_file_area_count:%d scan_file_area_count:%d scan_file_stat_count:%d warm_to_temp_file_area_count:%d mmap_free_pages_count:%d scan_file_area_count_from_cache_file:%d scan_cold_file_area_count_from_cache_file:%d refault_to_warm_file_area_count:%d find_cache_page_count_from_mmap_file:%d\n",mp->isolate_lru_pages_from_warm,mp->scan_cold_file_area_count_from_warm,mp->isolate_lru_pages_from_temp,mp->scan_cold_file_area_count_from_temp,mp->temp_to_warm_file_area_count,mp->scan_file_area_count,mp->scan_file_stat_count,mp->warm_to_temp_file_area_count,mp->mmap_free_pages_count,mp->scan_file_area_count_from_cache_file,mp->scan_cold_file_area_count_from_cache_file,mp->refault_to_warm_file_area_count,mp->find_cache_page_count_from_mmap_file);

		printk("del_file_stat_count:%d del_file_area_count:%d writeback_count:%d dirty_count:%d scan_mapcount_file_area_count:%d scan_hot_file_area_count:%d free_pages_from_cache_file:%d mapcount_to_warm_file_area_count:%d hot_to_warm_file_area_count:%d check_refault_file_area_count:%d free_file_area_count:%d temp_to_temp_head_file_area_count:%d scan_file_area_count_file_move_from_cache:%d mapcount_to_temp_file_area_count_from_mapcount_file:%d hot_to_temp_file_area_count_from_hot_file:%d\n",mp->del_file_stat_count,mp->del_file_area_count,mp->writeback_count,mp->dirty_count,mp->scan_mapcount_file_area_count,mp->scan_hot_file_area_count,mp->free_pages_from_cache_file,mp->mapcount_to_warm_file_area_count,mp->hot_to_warm_file_area_count,mp->check_refault_file_area_count,mp->free_file_area_count,mp->temp_to_temp_head_file_area_count,mp->scan_file_area_count_file_move_from_cache,mp->mapcount_to_temp_file_area_count_from_mapcount_file,mp->hot_to_temp_file_area_count_from_hot_file);

		printk("********global********\n");
		printk("0x%llx global_age:%d file_stat_count:%d file_stat_hot:%d file_stat_zero_file_area:%d file_stat_large_count:%d update_file_area_free_list_count:%ld update_file_area_warm_list_count:%ld update_file_area_temp_list_count:%ld update_file_area_other_list_count:%ld update_file_area_move_to_head_count:%ld free_pages:%ld free_mmap_pages:%ld check_refault_file_area_count:%ld check_mmap_refault_file_area_count:%ld\n",(u64)p_hot_cold_file_global,p_hot_cold_file_global->global_age,p_hot_cold_file_global->file_stat_count,p_hot_cold_file_global->file_stat_hot_count,p_hot_cold_file_global->file_stat_count_zero_file_area,p_hot_cold_file_global->file_stat_large_count,p_hot_cold_file_global->update_file_area_free_list_count,p_hot_cold_file_global->update_file_area_warm_list_count,p_hot_cold_file_global->update_file_area_temp_list_count,p_hot_cold_file_global->update_file_area_other_list_count,p_hot_cold_file_global->update_file_area_move_to_head_count,p_hot_cold_file_global->free_pages,p_hot_cold_file_global->free_mmap_pages,p_hot_cold_file_global->check_refault_file_area_count,p_hot_cold_file_global->check_mmap_refault_file_area_count);
	}
}

static int async_memory_reclaime_info_show(struct seq_file *m, void *v)
{
	hot_cold_file_print_all_file_stat(&hot_cold_file_global_info,m,1);
	printk_shrink_param(&hot_cold_file_global_info,m,1);
	return 0;
}
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
#if 0	
	p = proc_create("async_drop_caches", S_IWUSR, hot_cold_file_proc_root, &async_drop_caches_fops);
	if (!p){
		printk("proc_create open_print fail\n");
		return -1;
	}
#endif
	p = proc_create_single("async_memory_reclaime_info", S_IRUGO, hot_cold_file_proc_root,async_memory_reclaime_info_show);
	if (!p){
		printk("proc_create async_memory_reclaime_info fail\n");
		return -1;
	}

	p = proc_create("enable_disable_async_memory_reclaim", S_IRUGO | S_IWUSR, hot_cold_file_proc_root,&enable_disable_async_memory_reclaim_fops);
	if (!p){
		printk("proc_create enable_disable_async_memory_reclaim fail\n");
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

	remove_proc_entry("async_memory_reclaime_info",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("async_drop_caches",p_hot_cold_file_global->hot_cold_file_proc_root);
	remove_proc_entry("enable_disable_async_memory_reclaim",p_hot_cold_file_global->hot_cold_file_proc_root);

	remove_proc_entry("async_memory_reclaime",NULL);
	return 0;
}
//遍历p_file_stat对应文件的file_area_free链表上的file_area结构，找到这些file_area结构对应的page，这些page被判定是冷页，可以回收
unsigned long cold_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat_base,
		struct list_head *file_area_free,struct list_head *file_area_have_mmap_page_head)
{
	struct file_area *p_file_area,*tmp_file_area;
	int i;
	struct address_space *mapping = NULL;
	pg_data_t *pgdat = NULL;
	struct page *page;
	struct folio *folio;
	unsigned int isolate_pages = 0;
	int traverse_file_area_count = 0;  
	struct lruvec *lruvec = NULL,*lruvec_new = NULL;
	int move_page_count = 0;
	/*file_area里的page至少一个page发现是mmap的，则该file_area移动到file_area_have_mmap_page_head，后续回收mmap的文件页*/
	int find_file_area_have_mmap_page;
	unsigned int find_mmap_page_count_from_cache_file = 0;


	/*最初方案：当前函数执行lock_file_stat()对file_stat加锁。在__destroy_inode_handler_post()中也会lock_file_stat()加锁。防止
	 * __destroy_inode_handler_post()中把inode释放了，而当前函数还在遍历该文件inode的mapping的xarray tree
	 * 查询page，访问已经释放的内存而crash。这个方案太麻烦!!!!!!!!!!!!!!，现在的方案是使用rcu，这里
	 * rcu_read_lock()和__destroy_inode_handler_post()中标记inode delete形成并发。极端情况是，二者同时执行，
	 * 但这里rcu_read_lock后，进入rcu宽限期。而__destroy_inode_handler_post()执行后，触发释放inode，然后执行到destroy_inode()里的
	 * call_rcu(&inode->i_rcu, i_callback)后，无法真正释放掉inode结构。当前函数可以放心使用inode、mapping、xarray tree。
	 * 但有一点需注意，rcu_read_lock后不能休眠，否则rcu宽限期会无限延长。
	 *
	 * 但是又有一个问题，就是下边的循环执行的时间可能会很长，并且下边执行的内存回收shrink_inactive_list_async()可能会休眠。
	 * 而rcu_read_lock后不能休眠。因此，新的解决办法是，file_inode_lock()对inode加锁，并且令inode引用计数加1。如果成功则下边
	 * 不用再担心inode被其他进程iput释放。如果失败则直接return 0。详细 file_inode_lock()有说明
	 * */

	//lock_file_stat(p_file_stat,0);
	//rcu_read_lock();
	if(0 == file_inode_lock(p_file_stat_base)){
		printk("%s file_stat:0x%llx status 0x%lx inode lock fail\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		return 0;
    }
	/*执行到这里，就不用担心该inode会被其他进程iput释放掉*/

	mapping = p_file_stat_base->mapping;

	/*!!隐藏非常深的地方，这里遍历file_area_free(即)链表上的file_area时，可能该file_area在hot_file_update_file_status()中被访问而移动到了temp链表
	  这里要用list_for_each_entry_safe()，不能用list_for_each_entry!!!!!!!!!!!!!!!!!!!!!!!!*/
	list_for_each_entry_safe(p_file_area,tmp_file_area,file_area_free,file_area_list){

		/*如果遍历16个file_area,则检测一次是否有其他进程获取lru_lock锁失败而阻塞.有的话就释放lru_lock锁，先休眠5ms再获取锁,防止那些进程阻塞太长时间.
		 *是否有必要释放lru_lock锁时，也lock_file_stat()释放file_stat锁呢？此时可能处要使用lock_file_stat，1:inode删除 2：
		 *hot_cold_file_print_all_file_stat打印file_stat信息3:file_stat因为0个file_area而要删除.但这里仅休眠5ms不会造成太大阻塞。故不释放file_stat锁*/
		if((traverse_file_area_count++ >= 16) && (move_page_count < SWAP_CLUSTER_MAX)){
			traverse_file_area_count = 0;
			//使用 lruvec->lru_lock 锁，且有进程阻塞在这把锁上
			if(lruvec && (spin_is_contended(&lruvec->lru_lock) || need_resched())){
				spin_unlock_irq(&lruvec->lru_lock); 
				cond_resched();
				//msleep(5); 主动休眠的话再唤醒原cpu缓存数据会丢失

				spin_lock_irq(&lruvec->lru_lock);
				p_hot_cold_file_global->hot_cold_file_shrink_counter.lru_lock_contended_count ++;
			}
		}

		/*每次遍历新的file_area前必须对find_file_area_have_mmap_page清0*/
		find_file_area_have_mmap_page = 0;

		//得到file_area对应的page
		for(i = 0;i < PAGE_COUNT_IN_AREA;i ++){
			folio = p_file_area->pages[i];
			if(!folio){
				if(shrink_page_printk_open1)
					printk("%s file_area:0x%llx status:0x%x folio NULL\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

				continue;
			}
			page = &folio->page;
			if (page && !xa_is_value(page)) {
				if (!trylock_page(page)){
					continue;
				}

				/*如果page映射了也表页目录，这是异常的，要给出告警信息!!!!!!!!!!!!!!!!!!!还有其他异常状态。但实际调试
				 *遇到过page来自tmpfs文件系统，即PageSwapBacked(page)，最后错误添加到inacitve lru链表，但没有令inactive lru
				 *链表的page数加1，最后导致隔离page时触发mem_cgroup_update_lru_size()中发现lru链表page个数是负数而告警而crash*/
				if (unlikely(PageAnon(page))|| unlikely(PageCompound(page)) || unlikely(PageSwapBacked(page))){
					panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)page,page->flags);
				}

				//如果page被其他进程回收了，这里不成立，直接过滤掉page
				if(unlikely(page->mapping != mapping)){
					unlock_page(page);
					printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx page->mapping:0x%llx != mapping:0x%llx\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)page,page->flags,(u64)page->mapping,(u64)mapping);
					continue;
				}
				/* cache文件内存回收遇到mmap的文件页page，则把该file_area移动到file_area_have_mmap_page_head链表。然后立即break跳出，
				 * 不再遍历该file_area的page，而是遍历下一个file_area的page。这里边有个隐藏的问题，假如：如果file_area里的page0~page2
				 * 不是mmap文件页，被移动到inactive lru链表尾，正常参与内存回收。但是遍历到page3发现mmap的文件页，然后把
				 * 该file_area移动到file_area_have_mmap_page_head链表，参与mmap文件页冷热判断并参与内存回收。page3是mmap文件页则没事，但是
				 * page0~page2是cache文件页，参与mmap文件页冷热判断并内存回收，会不会有事？没事，mmap文件页冷热判断并内存回收的page，
				 * 限制必须是mmap文件页page，因此page0~page2不用担心被错误回收。新的方案改了，遇到mmap的文件页，继续遍历该file_area的
				 * page，只是find_file_area_have_mmap_page置1。等该file_area的page全遍历完，再把file_area移动到file_area_have_mmap_page_head
				 * 链表。这样的目的是，file_area的非mmap文件页能参与内存回收*/
				if(unlikely(page_mapped(page))){
					unlock_page(page);
					printk("%s file_stat:0x%llx status:0x%lx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx mapping:0x%llx mmapped\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,(u64)page,page->flags,(u64)mapping);

					find_file_area_have_mmap_page = 1;
					find_mmap_page_count_from_cache_file ++;
					//break;
					continue;
				}

				//第一次循环，lruvec是NULL，则先加锁。并对lruvec赋值，这样下边的if才不会成立，然后误触发内存回收，此时还没有move page到inactive lru链表
				if(NULL == lruvec){
					lruvec_new = mem_cgroup_lruvec(page_memcg(page),page_pgdat(page));
					lruvec = lruvec_new;
					spin_lock_irq(&lruvec->lru_lock);
				}else{
					lruvec_new = mem_cgroup_lruvec(page_memcg(page),page_pgdat(page));
				}
				/*实际调试发现，此时page可能还在lru缓存，没在lru链表。或者page在LRU_UNEVICTABLE这个lru链表。这两种情况的page
				 *都不能参与回收，否则把这些page错误添加到inactive链表但没有令inactive lru链表的page数加1，最后隔离这些page时
				 *会触发mem_cgroup_update_lru_size()中发现lru链表page个数是负数而告警而crash。并且，这个判断必要放到pgdat或
				 *lruvec加锁里，因为可能会被其他进程并发设置page的LRU属性或者设置page为PageUnevictable(page)然后移动到其他lru
				 *链表，这样状态纠错了。因此这段代码必须放到pgdat或lruvec加锁了!!!!!!!!!!!!!!!!!!!!!*/
				if(!PageLRU(page) || PageUnevictable(page)){
					if(shrink_page_printk_open1)
						printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx LRU:%d PageUnevictable:%d\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)page,page->flags,PageLRU(page),PageUnevictable(page));

					unlock_page(page);
					continue;
				}

				//if成立条件如果前后的两个page的lruvec不一样 或者 遍历的page数达到32，强制进行一次内存回收
				if( (move_page_count >= SWAP_CLUSTER_MAX) ||
						unlikely(lruvec != lruvec_new))
				{
					if(0 == move_page_count)
						panic("%s scan_page_count == 0 error pgdat:0x%llx lruvec:0x%llx lruvec_new:0x%llx\n",__func__,(u64)pgdat,(u64)lruvec,(u64)lruvec_new);

					//第一次进入这个if，pgdat是NULL，此时不用spin unlock，只有后续的page才需要
					if(unlikely(lruvec != lruvec_new)){
						//多次开关锁次数加1
						p_hot_cold_file_global->lru_lock_count++;
					}
					spin_unlock_irq(&lruvec->lru_lock);

					//回收inactive lru链表尾的page，这些page刚才才移动到inactive lru链表尾
					isolate_pages += shrink_inactive_list_async(move_page_count,lruvec,p_hot_cold_file_global,0,LRU_INACTIVE_FILE);

					//回收后对move_page_count清0
					move_page_count = 0;
					//回收后对遍历的file_area个数清0
					traverse_file_area_count = 0;

					//lruvec赋值最新page所属的lruvec
					lruvec = lruvec_new;
					//对新的page所属的pgdat进行spin lock。内核遍历lru链表都是关闭中断的，这里也关闭中断
					spin_lock_irq(&lruvec->lru_lock);
				}

				/*这里有个很重要的隐藏点，当执行到这里时，前后挨着的page所属的lruvec必须是同一个，这样才能
				 * list_move_tail到同一个lruvec inactive lru链表尾。否则就出乱子了，把不同lruvec的page移动到同一个。保险起见，
				 * 如果出现这种情况，强制panic*/
				if(lruvec != mem_cgroup_lruvec(page_memcg(page),page_pgdat(page)))
					panic("%s lruvec not equal error pgdat:0x%llx lruvec:0x%llx lruvec_new:0x%llx\n",__func__,(u64)pgdat,(u64)lruvec,(u64)lruvec_new);

				if(PageActive(page)){
					/*!!!!!!!!!!!重大bug，5.14的内核，把page添加到lru链表不再指定LRU_INACTIVE_FILE或LRU_ACTIVE_FILE，而是
					 *del_page_from_lru_list/add_page_to_lru_list 函数里判断page是否是acitve来决定page处于哪个链表。因此
					 *必须把ClearPageActive(page)清理page的active属性放到del_page_from_lru_list_async后边，否则会误判page处于LRU_INACTIVE_FILE链表*/
					del_page_from_lru_list(page,lruvec);
					barrier();
					//如果page在active lru链表，则清理active属性，把page从acitve链表移动到inactive链表，并令前者链表长度减1，后者链表长度加1
					ClearPageActive(page);
					barrier();
					add_page_to_lru_list_tail(page,lruvec);
				}else{
					//否则，page只是在inactive链表里移动，直接list_move即可，不用更新链表长度
					list_move_tail(&page->lru,&lruvec->lists[LRU_INACTIVE_FILE]);
				}

				//移动到inactive lru链表尾的page数加1
				move_page_count ++;
				/*这里有个问题，如果上边的if成立，触发了内核回收，当前这个page就要一直lock page，到这里才能unlock，这样
				 * 是不是lock page时间有点长。但是为了保证这个page这段时间不会被其他进程释放掉，只能一直lock page。并且
				 * 上边if里只回收32个page，还是clean page，没有io，时间很短的。*/
				unlock_page(page);

			}
		}
		/* cache文件file_area内存回收时，发现file_area里的page至少一个page发现是mmap的，则该file_area移动到
		 * file_area_have_mmap_page_head，后续回收mmap的文件页。但file_area_have_mmap_page_head链表不能是NULL！否则说明此时在回收
		 * mmap文件含有cache page的file_area里的page，特例，不能按照cache文件遇到mmap page的file_area处理，怕陷入死循环*/
		if(NULL != file_area_have_mmap_page_head &&find_file_area_have_mmap_page)
			list_move(&p_file_area->file_area_list,file_area_have_mmap_page_head);

	}

	//unlock_file_stat(p_file_stat);
	//rcu_read_unlock();
	file_inode_unlock(p_file_stat_base);

	//当函数退出时，如果move_page_count大于0，则强制回收这些page
	if(move_page_count > 0){
		if(lruvec)
			spin_unlock_irq(&lruvec->lru_lock);

		//回收inactive lru链表尾的page，这些page刚才才移动到inactive lru链表尾
		isolate_pages += shrink_inactive_list_async(move_page_count,lruvec,p_hot_cold_file_global,0,LRU_INACTIVE_FILE);

	}else{
		if(lruvec)
			spin_unlock_irq(&lruvec->lru_lock);
	}

	p_hot_cold_file_global->hot_cold_file_shrink_counter.find_mmap_page_count_from_cache_file += find_mmap_page_count_from_cache_file;
	return isolate_pages;
}

/****************************************************************************************/

/***oldnew********以上是老版本异步内存回收线程file_area内存回收核心代码，但是有一些问题，暂时停用，开发一个新版本***********/
#if 0
/*遍历hot_cold_file_global->file_stat_temp_large_file_head或file_stat_temp_head链表尾巴上边的文件file_stat，然后遍历这些file_stat的
 *file_stat->file_area_temp链表尾巴上的file_area，被判定是冷的file_area则移动到file_stat->file_area_free_temp链表。把有冷file_area的
  file_stat移动到file_stat_free_list临时链表。返回值是遍历到的冷file_area个数*/
static unsigned int get_file_area_from_file_stat_list(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max,unsigned int scan_file_stat_max,struct list_head *file_stat_temp_head,struct list_head *file_stat_free_list)
{
	//file_stat_temp_head来自 hot_cold_file_global->file_stat_temp_head 或 hot_cold_file_global->file_stat_temp_large_file_head 链表

	struct file_stat * p_file_stat,*p_file_stat_temp;
	struct file_area *p_file_area,*p_file_area_temp;
	int repeat_count = 0;

	unsigned int scan_file_area_count  = 0;
	unsigned int scan_file_stat_count  = 0;
	unsigned int real_scan_file_stat_count  = 0;
	unsigned int scan_delete_file_stat_count = 0;
	unsigned int scan_cold_file_area_count = 0;
	//unsigned int scan_large_to_small_count = 0;
	unsigned int scan_fail_file_stat_count = 0;

	unsigned int cold_file_area_for_file_stat = 0;
	unsigned int file_stat_count_in_cold_list = 0;
	unsigned int serial_file_area = 0;
	LIST_HEAD(unused_file_stat_list);
	//暂存从hot_cold_file_global->file_stat_temp_head 或 hot_cold_file_global->file_stat_temp_large_file_head 链表链表尾扫描到的file_stat
	LIST_HEAD(global_file_stat_temp_head_list);

	/*必须要先从file_stat_temp_head或file_stat_temp_large_file_head隔离多个file_stat，然后去遍历这些file_stat上的file_area，这样只用开关
	 * 一次hot_cold_file_global->global_lock锁.否则每遍历一个file_stat，都开关一次hot_cold_file_global->global_lock锁，太损耗性能。*/
	spin_lock(&p_hot_cold_file_global->global_lock);
	//先从global file_stat_temp_head链表尾隔离scan_file_stat_max个file_stat到 global_file_stat_temp_head_list 临时链表
	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,file_stat_temp_head,hot_cold_file_list){
		if(scan_file_stat_count ++ > scan_file_stat_max)
			break;
		/*这里把file_stat 移动到 global_file_stat_temp_head_list 临时链表，用不用清理的file_stat的 in_file_stat_temp_head 标记，需要的。
		 *因为hot_file_update_file_status()函数中会并发因为file_stat的 in_file_stat_temp_head 标记，而移动到file_stat的file_stat_hot_head
		 链表，不能有这种并发操作*/
		if(!file_stat_in_file_stat_temp_head_list(p_file_stat) || file_stat_in_file_stat_temp_head_list_error(p_file_stat)){
			/*正常情况file_stat肯定处于global temp_head_list链表，但是可能有进程在hot_file_update_file_status()函数并发把这个file_stat判断为
			  热文件并移动到global hot_head链表。这个不可能，因为这里先获取global_lock锁，然后再遍历file_stat*/
			panic("%s file_stat:0x%llx not int file_stat_temp_head status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			//continue;
		}
		else if(file_stat_in_delete(p_file_stat)){
			/*现在iput最后直接把delete file_stat移动到global delete链表，global temp链表不可能再遇到delete的 file_stat*/
			panic("%s file_stat:0x%llx delete status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			//如果大文件突然被删除了，这里要清理标记，并令file_stat_large_count减1
			if(file_stat_in_large_file(p_file_stat)){
				p_hot_cold_file_global->file_stat_large_count --;
				clear_file_stat_in_large_file(p_file_stat);
			}

			scan_delete_file_stat_count ++;
			clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			//如果该文件inode被释放了，则把对应file_stat移动到hot_cold_file_global->file_stat_delete_head链表
			list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_delete_head);
			continue;
		}
		/*如果这个file_stat对应的文件是mmap文件，现在被cache读写了，直接return，禁止一个文件既是cache文件又是mmap文件。
		 *walk_throuth_all_mmap_file_area()函数有详细介绍*/
		else if(file_stat_in_mmap_file(p_file_stat)){
			clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			set_file_stat_in_delete(p_file_stat);
			list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_delete_head);
			p_file_stat->mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE;
			printk("%s p_file_stat:0x%llx status:0x%lx in_mmap_file\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			continue;
		}
		
		/*现在热文件、大文件的判定不在hot_file_update_file_status()函数，而是移动到异步内存回收线程。
		 *热文件、大文件的判定移动到free_page_from_file_area()函数，不放在这里*/

		//如果file_stat的file_area全被释放了，则把file_stat移动到hot_cold_file_global->file_stat_zero_file_area_head链表
		if(p_file_stat->file_area_count == 0){
			//如果大文件的file_area全被释放了，这里要清理标记，并令file_stat_large_count减1，否则会导致file_stat_large_count泄漏
			if(file_stat_in_large_file(p_file_stat)){
				p_hot_cold_file_global->file_stat_large_count --;
				clear_file_stat_in_large_file(p_file_stat);
			}
			clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			set_file_stat_in_zero_file_area_list(p_file_stat);
			p_hot_cold_file_global->file_stat_count_zero_file_area ++;
			//如果该文件inode被释放了，则把对应file_stat移动到hot_cold_file_global->file_stat_zero_file_area_head链表
			list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_zero_file_area_head);
			continue;
		}

		/*file_stat_temp_head来自 hot_cold_file_global->file_stat_temp_head 或 hot_cold_file_global->file_stat_temp_large_file_head 链表，当是
		 * hot_cold_file_global->file_stat_temp_large_file_head时，file_stat_in_large_file(p_file_stat)才会成立*/


#if 0 
		/*当file_stat上有些file_area长时间没有被访问则会释放掉file_are结构。此时原本在hot_cold_file_global->file_stat_temp_large_file_head 链表的
		 *大文件file_stat则会因file_area数量减少而需要降级移动到hot_cold_file_global->file_stat_temp_head链表.这个判断起始可以放到
		 hot_file_update_file_status()函数，算了降低损耗。但是这段代码是冗余，于是把这段把有大文件标记但不再是大文件的file_stat移动到
		 global file_stat_temp_head链表的代码放到内存回收后期执行的free_page_from_file_area()函数里了。这两处的代码本身就是重复操作，
		 hot_file_update_file_status函数也会判断有大文件标记的file_stat是否是大文件*/
		if(file_stat_in_large_file(p_file_stat) && !is_file_stat_large_file(&hot_cold_file_global_info,p_file_stat)){

			scan_large_to_small_count ++;
			clear_file_stat_in_large_file(p_file_stat);
			/*不用现在把file_stat移动到global file_stat_temp_head链表。等该file_stat的file_area经过内存回收后，该file_stat会因为
			 *clear_file_stat_in_large_file而移动到file_stat_temp_head链表。想了想，还是现在就移动到file_stat->file_stat_temp_head链表尾，
			 否则内存回收再移动更麻烦。要移动到链表尾，这样紧接着就会从file_stat_temp_head链表链表尾扫描到该file_stat*/
			list_move_tail(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_temp_head);
			p_hot_cold_file_global->file_stat_large_count --;
			continue;
		}
#endif	
		if(p_file_stat->recent_access_age < p_hot_cold_file_global->global_age)
			p_file_stat->recent_access_age = p_hot_cold_file_global->global_age;

		//需要设置这些file_stat不再处于file_stat_temp_head链表，否则之后hot_file_update_file_status()会因该file_stat的热file_area很多而移动到global file_stat_temp_head链表
		clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
		//这里设置file_stat处于in_free_page，然后hot_file_update_file_status()中即便并发设置file_stat状态，也没事，因为都做好了并发防护
		set_file_stat_in_free_page(p_file_stat);
		//扫描到的file_stat先移动到global_file_stat_temp_head_list临时链表，下边就开始遍历这些file_stat上的file_area
		list_move(&p_file_stat->hot_cold_file_list,&global_file_stat_temp_head_list);
		real_scan_file_stat_count++;
	}
	spin_unlock(&p_hot_cold_file_global->global_lock);

	/*前边设置了参与内存回收的file_stat的in_free_page状态，但是有可能此时正好有进程hot_file_update_file_status()访问这些file_stat的file_area，
	  把file_area从file_stat->file_area_temp链表移动到file_stat->file_area_hot链表。该函数下边正好要遍历file_stat->file_area_temp链表上的file_area，
	  要避免此时hot_file_update_file_status()把正遍历的file_area从file_stat->file_area_temp链表移动到了file_stat->file_area_hot链表。否则会导致
	  边遍历file_stat->file_area_temp链表上的file_area陷入死循环。这就是"遍历的链表成员被移动到其他链表，因为链表头变了导致的遍历陷入死循环"。
	  而等这些进程都退出hot_file_update_file_status()函数，hot_cold_file_global_info.ref_count是0，这里才可以继续执行，遍历
	  file_stat->file_area_temp链表上的file_area。因为等新的进程再执行hot_file_update_file_status()，file_stat的in_free_page状态肯定已经
	  生效，hot_file_update_file_status()中就不会再把file_stat->file_area_temp链表上的file_area移动到file_stat->file_area_hot链表了
	  */
	/*新的方案，因为现在凡是执行hot_file_update_file_status()之前，一定rcu_read_lock()。故这里要等所有此时hot_file_update_file_status()中
	 *有可能正访问前边标记了in_free_page状态 file_stat的进程，执行rcu_read_unlock()，结束访问这些file_stat。然后再执行下边遍历这些
	 *file_stat->file_area_temp链表上的file_area，进行内存回收的代码*/
#if 0	
	while(atomic_read(&hot_cold_file_global_info.ref_count)){
		msleep(1);
	}
#else	
	synchronize_rcu();
#endif
	/*在遍历hot_cold_file_global->file_stat_temp_head链表期间，可能创建了新文件并创建了file_stat并添加到hot_cold_file_global->file_stat_temp_head链表，
	  下边遍历hot_cold_file_global->file_stat_hot_head链表成员期间，是否用hot_cold_file_global_info.global_lock加锁？不用，因为遍历链表期间
	  向链表添加成员没事，只要不删除成员！想想我写的内存屏障那片文章讲解list_del_rcu的代码*/
	list_for_each_entry_safe(p_file_stat,p_file_stat_temp,&global_file_stat_temp_head_list,hot_cold_file_list)//本质就是遍历p_hot_cold_file_global->file_stat_temp_head链表尾的file_stat
	{

		repeat_count = 0;
		cold_file_area_for_file_stat = 0;
repeat:
		serial_file_area = 0;
		/*注意，这里扫描的global file_stat_temp_head上的file_stat肯定有冷file_area，因为file_stat只要50%的file_area是热的，file_stat就要移动到
		  global file_stat_hot_head 链表*/
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat->file_area_temp,file_area_list)//从链表尾开始遍历，链表尾的成员更老，链表头的成员是最新添加的
		{
			if(!file_stat_in_free_page(p_file_stat) || file_stat_in_free_page_error(p_file_stat)){
				panic("%s file_stat:0x%llx not int file_stat_in_free_page status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			}

			scan_file_area_count ++;

			//file_area经过FILE_AREA_TEMP_TO_COLD_AGE_DX个周期还没有被访问，则被判定是冷file_area，然后就释放该file_area的page
			if(p_hot_cold_file_global->global_age - p_file_area->file_area_age >  p_hot_cold_file_global->file_area_temp_to_cold_age_dx){
				//每遍历到一个就加一次锁，浪费性能，可以先移动到一个临时链表上，循环结束后加一次锁，然后把这些file_area或file_stat移动到目标链表???????
				spin_lock(&p_file_stat->file_stat_lock);
				/*为什么file_stat_lock加锁后要再判断一次file_area是不是被访问了,因为可能有这种情况:上边的if成立,此时file_area还没被访问。但是此时有进程
				  先执行hot_file_update_file_status()获取file_stat_lock锁,然后访问当前file_area,file_area不再冷了,当前进程此时获取file_stat_lock锁失败,
				  等获取file_stat_lock锁成功后，file_area的file_area_age就和global_age相等了。变量加减后的判断，在spin_lock前后各判断一次有必要的!!!!!*/
				if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <=  p_hot_cold_file_global->file_area_temp_to_cold_age_dx){
					spin_unlock(&p_file_stat->file_stat_lock);    
					continue;
				}
				serial_file_area = 0;
				//access_count清0，如果内存回收期间又被访问了，access_count将大于0，将被判断为refault page。
				file_area_access_count_clear(p_file_area);
				clear_file_area_in_temp_list(p_file_area);
				/*设置file_area处于file_stat的free_temp_list链表。这里设定，不管file_area处于file_stat->file_area_free_temp还是
				 *file_stat->file_area_free链表，都是file_area_in_free_list状态，没有必要再区分二者。主要设置file_area的状态需要
				 遍历每个file_area并file_stat_lock加锁，再多设置一次set_file_area_in_free_temp_list状态浪费性能。这点需注意!!!!!!!!!!!!!*/
				set_file_area_in_free_list(p_file_area);
				/*需要加锁，此时可能有进程执行hot_file_update_file_status()并发向该p_file_area前或者后插入新的file_area，这里是把该
				 * p_file_area从file_area_temp链表剔除，存在同时修改该p_file_area在file_area_temp链表前的file_area结构的next指针和在链表后的
				 * file_area结构的prev指针，并发修改同一个变量就需要加锁*/
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free_temp);
				spin_unlock(&p_file_stat->file_stat_lock);

				cold_file_area_for_file_stat ++;
			}
			//else if(p_hot_cold_file_global->global_age == p_file_area->file_area_age)
			else 
				//否则就停止遍历file_stat->file_area_temp链表上的file_area，因为该链表上的file_area从左向右，访问频率由大向小递增，这个需要实际测试?????????
			{
				/*如果file_stat->file_area_temp链表尾连续扫到3个file_area都是热的，才停止扫描该file_stat上的file_area。因为此时
				 *file_stat->file_area_temp链表尾上的file_area可能正在被访问，file_area->file_area_age=hot_cold_file_global->global_age，
				 但是file_area还没被移动到file_stat->file_area_temp链表头。这个判断是为了过滤掉这种瞬时的热file_area干扰*/
				if(serial_file_area ++ > 1)
					break;
			}
		}
		/*第2遍遍历file_stat->file_area_temp链表尾的file_area，防止第1次遍历file_stat->file_area_temp链表尾的file_area时，该file_stat因为被访问
		  在hot_file_update_file_status()函数中被移动到file_stat->file_area_temp链表头，这样就会立即结束遍历。file_stat->file_area_temp链表尾的
		  冷file_area根本没有遍历完*/
		if(repeat_count == 0){
			repeat_count ++;
			goto repeat;
		}

		/*1: cold_file_area_for_file_stat != 0表示把有冷file_area的file_stat移动到file_stat_free_list临时链表.此时的file_sata已经不在
		file_stat_temp_head链表，不用clear_file_stat_in_file_stat_temp_head_list
		2: 如果file_stat->file_area_refault链表非空，说明也需要扫描这上边的file_area，要把上边冷的file_area移动回file_stat_temp_head_list
		链表，参数内存回收扫描，结束保护期
		3: 如果file_stat->file_area_free 和 file_stat->file_area_hot链表上也非空，说明上边也有file_area需要遍历，file_area_hot链表上的冷
		file_area需要移动回file_stat_temp_head_list链表，file_area_free链表上长时间没有被访问的file_area要释放掉file_area结构。

		因此，file_stat->file_area_temp上有冷page，或者file_stat->file_area_refault、file_area_free、file_area_hot 链表只要非空，有file_area，
		都要把file_stat结构添加到file_stat_free_list临时链表。然后free_page_from_file_area()中依次扫描这些file_stat的file_area_free_temp、
		file_area_refault、file_area_free、file_area_hot链表上file_area，按照对应策略该干啥干啥。

		这段代码是从上边的for循环移动过来的，放到这里是保证同一个file_stat只list_move到file_stat_free_list链表一次。并且，当
		file_stat->file_area_temp链表没有冷file_area或者没有一个file_area时，但是file_stat的file_area_free_temp、file_area_refault、
		file_area_free、file_area_hot链表上file_area要遍历，这样也要把该file_stat移动到file_stat_free_list链表，这样将来
		free_page_from_file_area()函数中才能从file_stat_free_list链表扫描到该file_stat，否则会出现一些问题，比如file_stat的file_area_free链表上
		长时间没访问的file_stat无法遍历到，无法释放这些file_stat结构；还有 file_stat的file_area_refault和file_area_hot链表上的冷file_area
		无法降级移动到file_stat->file_area_temp链表，这些file_stat将无法扫描到参与内存回收
		*/
		if(cold_file_area_for_file_stat != 0 || !list_empty(&p_file_stat->file_area_refault) ||
				!list_empty(&p_file_stat->file_area_free) || !list_empty(&p_file_stat->file_area_hot)){
			list_move(&p_file_stat->hot_cold_file_list,file_stat_free_list);
			//移动到file_stat_free_list链表头的file_stat个数
			file_stat_count_in_cold_list ++;
		}
		else{
			/*把没有冷file_area、file_stat->file_area_refault、file_area_free、file_area_hot还是空的file_stat移动到unused_file_stat_list
			 *临时链表，最后再移动到global file_stat_temp_head链表头，这样下轮异步内存回收不会重复扫描这些file_stat*/
			list_move(&p_file_stat->hot_cold_file_list,&unused_file_stat_list);
		}
		//累计遍历到的冷file_area个数
		scan_cold_file_area_count += cold_file_area_for_file_stat;

		/*防止在for循环耗时太长，限制遍历的文件file_stat数。这里两个问题 问题1:单个file_stat上的file_area太多了，只扫描一个file_stat这里就
		  break跳出循环了。这样下边就把global_file_stat_temp_head_list残留的file_stat移动到global file_stat_temp_head链表头了。下轮扫描从
		  global file_stat_temp_head尾就扫描不到该file_stat了。合理的做法是，把这些压根没扫描的file_stat再移动到global file_stat_temp_head尾。
		  问题2：还是 单个file_stat上的file_area太多了，没扫描完，下次再扫描该file_stat时，直接从上次结束的file_area位置处继续扫描，似乎更合理。
		  file_stat断点file_area继续扫描！但是实现起来似乎比较繁琐，算了*/
		if(scan_file_area_count > scan_file_area_max)
			break;
	}
	/*到这里还残留在global_file_stat_temp_head_list上的file_stat，是本轮就没有扫描到的。因为参与内存回收的扫描的file_area总数不能超过
	  scan_file_area_max，如果某个file_stat的file_area太多就会导致global_file_stat_temp_head_list链表上其他file_stat扫描不到。这里要把
	  这些file_stat移动到global file_stat_temp_head链表尾，下次异步内存回收继续扫描这些file_stat*/
	if(!list_empty(&global_file_stat_temp_head_list)){

		spin_lock(&p_hot_cold_file_global->global_lock);
		//设置file_stat状态要加锁
		list_for_each_entry(p_file_stat,&global_file_stat_temp_head_list,hot_cold_file_list){
			/*这里清理file_stat的in_free_page状态很重要，因为这些file_stat在该函数开头设置了in_free_page状态，这里要清理掉in_free_page状态，
			 * 否则后续扫描这些file_stat时，会出现file_stat状态错乱*/
			clear_file_stat_in_free_page(p_file_stat);
			set_file_stat_in_file_stat_temp_head_list(p_file_stat);//设置file_stat状态为head_temp_list 
			scan_fail_file_stat_count ++;
		}

		//把未遍历的file_stat再移动回global file_stat_temp_head或global file_stat_temp_large_file_head 链表尾巴
		list_splice_tail(&global_file_stat_temp_head_list,file_stat_temp_head);

		/*list_splice把前者的链表成员a1...an移动到后者链表，并不会清空前者链表。必须INIT_LIST_HEAD清空前者链表，否则它一直指向之前的链表成员
		 *a1...an。后续再向该链表添加新成员b1...bn。这个链表就指向的成员就有a1...an + b1...+bn。而此时a1...an已经移动到了后者链表，
		 *相当于前者和后者链表都指向了a1...an成员，这样肯定会出问题.之前get_file_area_from_file_stat_list()函数报错
		 "list_add corruption. next->prev should be prev"而crash估计就是这个原因!!!!!!!!!!!!!!!!!!
		 */

		//INIT_LIST_HEAD(&global_file_stat_temp_head_list)//global_file_stat_temp_head_list是局部链表，不用清，只有全局变量才必须list_splice_tail后清空链表

		spin_unlock(&p_hot_cold_file_global->global_lock);
	}

	/*unused_file_stat_list链表上的file_stat，没有冷file_area、file_stat->file_area_refault、file_area_free、file_area_hot还是空的，这里把
	 *这些file_stat移动到global file_stat_temp_head链表头，这样下轮异步内存回收不会重复扫描这些file_stat*/
	if(!list_empty(&unused_file_stat_list)){

		spin_lock(&p_hot_cold_file_global->global_lock);
		//设置file_stat状态要加锁
		list_for_each_entry(p_file_stat,&unused_file_stat_list,hot_cold_file_list){
			/*这里清理file_stat的in_free_page状态很重要，因为这些file_stat在该函数开头设置了in_free_page状态，这里要清理掉in_free_page状态，
			 * 否则后续扫描这些file_stat时，会出现file_stat状态错乱*/
			clear_file_stat_in_free_page(p_file_stat);
			set_file_stat_in_file_stat_temp_head_list(p_file_stat);//设置file_stat状态为head_temp_list 
			scan_fail_file_stat_count ++;
		}
		//移动到 global file_stat_temp_head 或 file_stat_temp_large_file_head 链表头，样下轮异步内存回收不会重复扫描这些file_stat
		list_splice(&unused_file_stat_list,file_stat_temp_head);
		spin_unlock(&p_hot_cold_file_global->global_lock);
	}

	if(shrink_page_printk_open)
		printk("3:%s %s %d p_hot_cold_file_global:0x%llx scan_file_stat_count:%d scan_file_area_count:%d scan_cold_file_area_count:%d file_stat_count_in_cold_list:%d  real_scan_file_stat_count:%d\n",__func__,current->comm,current->pid,(u64)p_hot_cold_file_global,scan_file_stat_count,scan_file_area_count,scan_cold_file_area_count,file_stat_count_in_cold_list,real_scan_file_stat_count);

	//扫描的file_area个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_file_area_count = scan_file_area_count;
	//扫描的file_stat个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_file_stat_count = real_scan_file_stat_count;
	//扫描到的处于delete状态的file_stat个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_delete_file_stat_count = scan_delete_file_stat_count;
	//扫描的冷file_stat个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_cold_file_area_count = scan_cold_file_area_count;
	//扫描到的大文件转小文件的个数
	//p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_large_to_small_count = scan_large_to_small_count;
	//本次扫描到但没有冷file_area的file_stat个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_fail_file_stat_count = scan_fail_file_stat_count;

	return scan_cold_file_area_count;
}
/*该函数主要有3个作用
 * 1：释放file_stat_free_list链表上的file_stat的file_area_free_temp链表上冷file_area的page。释放这些page后，把这些file_area移动到
 *    file_stat->file_area_free链表头
 * 2：遍历file_stat_free_list链表上的file_stat的file_area_hot链表尾上的热file_area，如果长时间没有被访问，说明变成冷file_area了，
 *    则移动到file_stat->file_area_temp链表头
 * 3：遍历file_stat_free_list链表上的file_stat的file_area_free链表尾上的file_area，如果还是长时间没有被访问，则释放掉这些file_area结构
 * 4: 遍历file_stat_free_list链表上的file_stat的file_area_refault链表尾巴的file_area，如果长时间没有被访问，则移动到file_stat->file_area_temp链表头
 * 5: 把file_stat_free_list链表上的file_stat再移动回file_stat_temp_head链表(即global file_stat_temp_head或file_stat_temp_large_file_head)头，
 *    这样下轮walk_throuth_all_file_area()再扫描，从global file_stat_temp_head或file_stat_temp_large_file_head链表尾巴扫到的file_stat
 *    都是最近没有被扫描过的，避免重复扫描
 */

/*file_stat_free_list链表上的file_stat来自本轮扫描从global file_stat_temp_head或file_stat_temp_large_file_head链表尾获取到的
  file_stat_temp_head是global file_stat_temp_head或file_stat_temp_large_file_head*/
static unsigned long free_page_from_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct list_head * file_stat_free_list,struct list_head *file_stat_temp_head)
{
	struct file_stat *p_file_stat,*p_file_stat_temp;
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int cold_file_area_count;
	unsigned int free_pages = 0;
	unsigned int file_area_count;
	unsigned int isolate_lru_pages = 0;
	unsigned int file_area_refault_to_temp_list_count = 0;
	unsigned int file_area_free_count = 0;
	unsigned int file_area_hot_to_temp_list_count = 0;
	unsigned int scan_large_to_small_count = 0;

	/*同一个文件file_stat的file_area对应的page，更大可能是属于同一个内存节点node，所以要基于一个个文件的file_stat来扫描file_area，
	 *避免频繁开关内存节点锁pgdat->lru_lock锁*/  

	//遍历file_stat_free_list临时链表上的file_stat，释放这些file_stat的file_stat->file_area_free_temp链表上的冷file_area的page
	list_for_each_entry(p_file_stat,file_stat_free_list,hot_cold_file_list)
	{
		isolate_lru_pages += cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_free_temp);
		free_pages += p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;

		if(shrink_page_printk_open1)
			printk("1:%s %s %d p_hot_cold_file_global:0x%llx p_file_stat:0x%llx status:0x%lx free_pages:%d\n",__func__,current->comm,current->pid,(u64)p_hot_cold_file_global,(u64)p_file_stat,p_file_stat->file_stat_status,free_pages);

		/*注意，file_stat->file_area_free_temp 和 file_stat->file_area_free 各有用处。file_area_free_temp保存每次扫描释放的page的file_area。
		  释放后把这些file_area移动到file_area_free链表，file_area_free保存的是每轮扫描释放page的所有file_area，是所有的!!!!!!!!!!!!!!*/

		/*p_file_stat->file_area_free_temp上的file_area的冷内存page释放过后,下边需则把file_area_free_temp链表上的file_area结构再移动到
		 *file_area_free链表头，file_area_free链表上的file_area结构要长时间也没被访问就释放掉*/

		//if(!list_empty(&p_file_stat->file_area_free_temp)){//get_file_area_from_file_stat_list()函数中，有的file_stat没有冷file_area，但是有热file_area、refault file_area，也会移动到file_stat_free_list链表，但是file_stat->file_area_free_temp链表是空的，这里就if不成立了，因此要去掉

		/*hot_file_update_file_status()函数中会并发把file_area从file_stat->file_area_free_temp链表移动到file_stat->file_area_free_temp链表.
		  这里把file_stat->file_area_free_temp链表上的file_area移动到file_stat->file_area_free，需要加锁*/
		spin_lock(&p_file_stat->file_stat_lock);

		if(!file_stat_in_free_page(p_file_stat) || file_stat_in_free_page_error(p_file_stat)){
			panic("%s file_stat:0x%llx not int file_stat_in_free_page status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
		}

		//清理file_stat的in_free_page状态，并设置file_stat处于in_free_page_done状态
		clear_file_stat_in_free_page(p_file_stat);
		set_file_stat_in_free_page_done(p_file_stat);

		list_splice(&p_file_stat->file_area_free_temp,&p_file_stat->file_area_free);
		/*list_splice把前者的链表成员a1...an移动到后者链表，并不会清空前者链表。必须INIT_LIST_HEAD清空前者链表，否则它一直指向之前的
		 *链表成员a1...an。后续再向该链表添加新成员b1...bn。这个链表就指向的成员就有a1...an + b1...+bn。而此时a1...an已经移动到了后者
		 *链表，相当于前者和后者链表都指向了a1...an成员，这样肯定会出问题.之前get_file_area_from_file_stat_list()函数报错
		 *"list_add corruption. next->prev should be prev"而crash估计就是这个原因!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		 */
		INIT_LIST_HEAD(&p_file_stat->file_area_free_temp);

		spin_unlock(&p_file_stat->file_stat_lock);
		//}
	}
	//需要调度的话休眠一下
	cond_resched();

	/*遍历file_stat_free_list临时链表上的file_stat，然后遍历着这些file_stat->file_area_hot链表尾巴上热file_area。这些file_area之前
	 *被判定是热file_area而被移动到了file_stat->file_area_hot链表。之后，file_stat->file_area_hot链表头的file_area访问频繁，链表尾巴
	 的file_area就会变冷。则把这些file_stat->file_area_hot尾巴上长时间未被访问的file_area再降级移动回file_stat->file_area_temp链表头*/
	list_for_each_entry(p_file_stat,file_stat_free_list,hot_cold_file_list){
		cold_file_area_count = 0;
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat->file_area_hot,file_area_list){
			if(!file_area_in_hot_list(p_file_area) || file_area_in_hot_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_hot\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			//file_stat->file_area_hot尾巴上长时间未被访问的file_area再降级移动回file_stat->file_area_temp链表头
			if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > p_hot_cold_file_global->file_area_hot_to_temp_age_dx){
				cold_file_area_count = 0;
				file_area_hot_to_temp_list_count ++;
				//每遍历到一个就加一次锁，浪费性能，可以先移动到一个临时链表上，循环结束后加一次锁，然后把这些file_area或file_stat移动到目标链表?????
				spin_lock(&p_file_stat->file_stat_lock);
				clear_file_area_in_hot_list(p_file_area);
				//file_stat的热file_area个数减1
				p_file_stat->file_area_hot_count --;
				set_file_area_in_temp_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
				spin_unlock(&p_file_stat->file_stat_lock);	    
			}else{//到这里，file_area被判定还是热file_area，还是继续存在file_stat->file_area_hot链表

				/*如果file_stat->file_area_hot尾巴上连续出现2个file_area还是热file_area，则说明file_stat->file_area_hot链表尾巴上的冷
				 *file_area都遍历完了,遇到链表头的热file_area了，则停止遍历。file_stat->file_area_hot链表头到链表尾，file_area是由热到
				 *冷顺序排布的。之所以要限制连续碰到两个热file_area再break，是因为file_stat->file_area_hot尾巴上的冷file_area可能此时
				 *hot_file_update_file_status()中并发被频繁访问，变成热file_area，但还没来得及移动到file_stat->file_area_hot链表头
				 */
				if(cold_file_area_count ++ > 2)
					break;
			}
		}
	}

	//需要调度的话休眠一下
	cond_resched();

	/*遍历file_stat_free_list临时链表上的file_stat，然后看这些file_stat的file_area_free链表上的哪些file_area长时间未被访问，抓到的话就
	 *释放掉file_area结构如果file_stat->file_area_free链表上有很多file_area导致这里遍历时间很长怎么办？需要考虑一下????????*/
	list_for_each_entry(p_file_stat,file_stat_free_list,hot_cold_file_list){
		file_area_count = 0;
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat->file_area_free,file_area_list){
			/*由于这个过程没有spin_lock(&p_file_stat->file_stat_lock)加锁，file_area可能正在被访问，清理的file_area_in_free_list标记，
			 *并设置了file_area_in_hot_list或file_area_in_temp_list标记，但是file_area还没移动到file_stat的file_area_temp或file_area_hot链表。
			 *此时if(!file_area_in_free_list(p_file_area))成立，但这是正常现象。如果file_area_free链表上file_stat又被访问了，则在
			 *hot_file_update_file_status()函数中再被移动到p_file_stat->file_area_temp链表
			 */
			if(!file_area_in_free_list(p_file_area)){
				printk("%s file_area:0x%llx status:0x%x not in file_area_free !!!!!!!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
				continue;
			}
#if 0	
            /*如果p_file_stat->file_area_last在file_stat->file_area_free链表上，经历过一个周期后还没被访问，那就清空p_file_stat->file_area_last这个cache*/
			if(p_file_stat->file_area_last == p_file_area){
			    p_file_stat->file_area_last = NULL;
			}
#endif			

			if(unlikely(file_area_access_count_get(p_file_area) > 0)){
				/*这段代码时新加的，是个隐藏很深的小bug。file_area在内存回收前都要对access_count清0，但是在内存回收最后，可能因对应page
				 *被访问了而access_count加1，然后对age赋值为当时的global age，但是file_area的page内存回收失败了。等了很长时间后，终于再次
				 *扫描到这个文件file_stat，但是file_area的age还是与global age相差很大了，正常就要判定这个file_area长时间没访问而释放掉。
				 *但这是正常现象不合理的！因为这个file_area的page在内存回收时被访问了。于是就通过file_area的access_count大于0而判定这个file_area的
				 *page在内存回收最后被访问了，于是就不能释放掉file_area。那就要移动到file_stat->temp链表或者refault链表!!!!!!!!!!!!!!!!!!!!*/
				spin_lock(&p_file_stat->file_stat_lock);
				clear_file_area_in_free_list(p_file_area);
				set_file_area_in_temp_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
				spin_unlock(&p_file_stat->file_stat_lock);	    
				printk("%s file_area:0x%llx status:0x%x accessed in reclaim\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
			}
			/*如果file_stat->file_area_free链表上的file_area长时间没有被访问则释放掉file_area结构。之前的代码有问题，判定释放file_area的时间是
			 *file_area_free_age_dx，这样有问题，会导致file_area被内存回收后，在下个周期file_area立即被释放掉。原因是file_area_free_age_dx=5，
			 file_area_temp_to_cold_age_dx=5，下个内存回收周期 global_age - file_area_free_age_dx肯定大于5*/
			else if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > 
					(p_hot_cold_file_global->file_area_free_age_dx + p_hot_cold_file_global->file_area_temp_to_cold_age_dx)){
				file_area_free_count ++;
				file_area_count = 0;
				/*hot_file_update_file_status()函数中会并发把file_area从file_stat->file_area_free链表移动到file_stat->file_area_free_temp
				 *链表.这里把file_stat->file_area_free链表上的file_area剔除掉并释放掉，需要spin_lock(&p_file_stat->file_stat_lock)加锁，
				 *这个函数里有加锁*/
				cold_file_area_delete(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area);
			}
			else{
				/*如果file_stat->file_area_free链表尾连续出现3个file_area未达到释放标准,说明可能最近被访问过，则结束遍历该
				 *file_stat->file_area_free上的file_area这是防止遍历耗时太长，并且遍历到本轮扫描添加到file_stat->file_area_free上的file_area，浪费*/
				if(file_area_count ++ > 2)
					break;
			}
		}
	    cond_resched();
	}

	/*遍历 file_stat_free_list临时链表上的file_stat，然后看这些file_stat的file_area_refault链表上的file_area，如果长时间没有被访问，
	  则要移动到file_stat->file_area_temp链表*/
	list_for_each_entry(p_file_stat,file_stat_free_list,hot_cold_file_list){
		file_area_count = 0;
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat->file_area_refault,file_area_list){
			if(!file_area_in_refault_list(p_file_area) || file_area_in_refault_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_refault\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			//file_stat->file_area_hot尾巴上长时间未被访问的file_area再降级移动回file_stat->file_area_temp链表头
			if(p_hot_cold_file_global->global_age - p_file_area->file_area_age >  p_hot_cold_file_global->file_area_refault_to_temp_age_dx){
				file_area_refault_to_temp_list_count ++;
				file_area_count = 0;
				//每遍历到一个就加一次锁，浪费性能，可以先移动到一个临时链表上，循环结束后加一次锁，然后把这些file_area或file_stat移动到目标链表??????????????
				spin_lock(&p_file_stat->file_stat_lock);
				clear_file_area_in_refault_list(p_file_area);
				set_file_area_in_temp_list(p_file_area);
				p_hot_cold_file_global->refault_file_area_count_in_free_page --;
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
				spin_unlock(&p_file_stat->file_stat_lock);	    
			}else{
				/*如果file_stat->file_area_refault尾巴上连续出现2个file_area还是热file_area，则说明file_stat->file_area_hot链表尾巴上的冷
				 *file_area都遍历完了,遇到链表头的热file_area了，则停止遍历。file_stat->file_area_refault链表头到链表尾，file_area是由热到
				 冷顺序排布的。之所以要限制连续碰到两个热file_area再break，是因为file_stat->file_area_refault尾巴上的冷file_area可能此时
				 hot_file_update_file_status()中并发被频繁访问，变成热file_area，但还没来得及移动到file_area_refault链表头*/
				if(file_area_count ++ >2)
					break;
			}
		}
	}

	//需要调度的话休眠一下
	cond_resched();
	/*在内存回收结束时，遍历参与内存回收的一个个文件file_stat的file_area_temp和file_area_free链表头的file_area，是否在内存回收期间被访问了，
	 *是的话就移动到对应链表*/
	list_for_each_entry(p_file_stat,file_stat_free_list,hot_cold_file_list){
		/*如果内存回收期间file_stat->file_area_temp链表上的file_area被频繁访问，这种file_area只会移动到file_stat->file_area_temp链表头。
		  这里在内存回收结束时，检查file_stat->file_area_temp链表头是否有热file_area，有的话就释放则移动到file_stat->file_area_hot链表，
		  没有立即跳出循环*/
		list_for_each_entry_safe(p_file_area,p_file_area_temp,&p_file_stat->file_area_temp,file_area_list){
			if(!file_area_in_temp_list(p_file_area) || file_area_in_temp_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			if(file_area_access_count_get(p_file_area) > FILE_AREA_HOT_LEVEL){
				spin_lock(&p_file_stat->file_stat_lock);
				clear_file_area_in_temp_list(p_file_area);
				set_file_area_in_hot_list(p_file_area);
				p_file_stat->file_area_hot_count ++;//热file_area个数加1
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
				//在内存回收期间产生的热file_area个数
				p_hot_cold_file_global->hot_cold_file_shrink_counter.hot_file_area_count_in_free_page ++;
				spin_unlock(&p_file_stat->file_stat_lock); 
			}
			else
				break;//用不用加个过滤条件，连续两个file_area的access_count小于FILE_AREA_HOT_LEVEL时再break????????
		}
		/*如果内存回收期间file_stat->file_area_free链表上的file_area被访问了，这种file_area只会移动到file_stat->file_area_free链表头。
		  这里在内存回收结束时，检查file_stat->file_area_free链表头的file_area是否内存回收过程或结束时被访问了，是则释放则移动到
		  file_stat->file_area_refault链表，无则立即跳出循环*/
		list_for_each_entry_safe(p_file_area,p_file_area_temp,&p_file_stat->file_area_free,file_area_list){
			if(!file_area_in_free_list(p_file_area) || file_area_in_free_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			if(file_area_access_count_get(p_file_area) > 0){
				spin_lock(&p_file_stat->file_stat_lock);
				clear_file_area_in_free_list(p_file_area);
				set_file_area_in_refault_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
				//在内存回收期间产生的refault file_area个数
				p_hot_cold_file_global->refault_file_area_count_in_free_page ++;
				hot_cold_file_global_info.all_refault_count ++;
				hot_cold_file_global_info.hot_cold_file_shrink_counter.refault_file_area_count_one_period ++;
				spin_unlock(&p_file_stat->file_stat_lock);	    
			}
			else
				break;
		}
	}

	//把file_stat_free_list临时链表上释放过内存page的file_stat再移动回global file_stat_temp_head或file_stat_temp_large_file_head链表头
	if(!list_empty(file_stat_free_list)){
		spin_lock(&p_hot_cold_file_global->global_lock);
		/*突然想到，下边for循环正在进行时，要是hot_file_update_file_status()函数中把p_file_stat的下一个file_stat即p_file_stat_temp移动到
		 *其他链表了这个for循环岂不是又要发生file_stat跨链表导致死循环?不会，因为这个for循环全程 global_lock加锁，其他地方无法把
		 *file_stat移动到其他链表*/
		list_for_each_entry_safe(p_file_stat,p_file_stat_temp,file_stat_free_list,hot_cold_file_list){
			if(!file_stat_in_free_page_done(p_file_stat) || file_stat_in_free_page_done_error(p_file_stat)){
				panic("%s file_stat:0x%llx not int file_stat_in_free_page_done status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			}
			//清理file_stat的in_free_page_done状态，结束内存回收
			clear_file_stat_in_free_page_done(p_file_stat);
			/*在前边遍历这些file_stat的file_area并回收内存page过程中，file_stat是无状态的。现在这些file_stat重新移动回global的各个链表过程，
			  重新判断一下这个文件是否是大文件或者热文件，是的话则移动到对应链表*/
			if(is_file_stat_hot_file(p_hot_cold_file_global,p_file_stat)){//热文件
				set_file_stat_in_file_stat_hot_head_list(p_file_stat);
				p_hot_cold_file_global->file_stat_hot_count ++;//热文件数加1 
				list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_hot_head);
				//p_file_stat->file_stat_hot_base_age = p_hot_cold_file_global->global_age;
			}else{
				set_file_stat_in_file_stat_temp_head_list(p_file_stat);

				if(is_file_stat_large_file(p_hot_cold_file_global,p_file_stat)){//大文件
					//之前是小文件，内存回收期间变成大文件，这种情况再设置大文件标记
					if(!file_stat_in_large_file(p_file_stat)){
					    set_file_stat_in_large_file(p_file_stat);
					    /*这个if成立，说明是内存回收期间小文件变成大文件。因为file_stat期间不是in_temp_list状态，update函数不会
					     * 把文件file_stat移动到大文件链表，也不会file_stat_large_count加1，只能这里加1了*/
					    p_hot_cold_file_global->file_stat_large_count ++;
                                        }
					//p_hot_cold_file_global->file_stat_large_count ++;//大文件数加1，这不是新产生的大文件，已经加过1了
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_large_file_head);
				}
				else//普通文件
				{	
					/*如果file_stat有大文件标记，说明之前是大文件，但是经过多轮内存回收、释放file_stat->file_area_free链表上
					 * file_area后，不再是大文件了，就移动到global file_stat_temp_head链表。但是必须清理掉大文件标记。否则这
					 * 会导致状态错误:file_stat_temp_head链表上的file_stat有大文件标记，将来即便再变成大文件也无法移动到大文件链表*/
					if(file_stat_in_large_file(p_file_stat)){
					    clear_file_stat_in_large_file(p_file_stat);
					    p_hot_cold_file_global->file_stat_large_count --;
					}
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_temp_head);
				}
			}
		}
		/*把这些遍历过的file_stat移动回global file_stat_temp_head或file_stat_temp_large_file_head链表头,注意是链表头。这是因为，把这些
		 *遍历过的file_stat移动到global file_stat_temp_head或file_stat_temp_large_file_head链表头，下轮扫描才能从global file_stat_temp_head
		 或file_stat_temp_large_file_head链表尾遍历没有遍历过的的file_stat*/
		//list_splice(file_stat_free_list,file_stat_temp_head);//把这段代码移动到上边了

		/*list_splice把前者的链表成员a1...an移动到后者链表，并不会清空前者链表。必须INIT_LIST_HEAD清空前者链表，否则它一直指向之前的链表
		 *成员a1...an。后续再向该链表添加新成员b1...bn。这个链表就指向的成员就有a1...an + b1...+bn。而此时a1...an已经移动到了后者链表，
		 *相当于前者和后者链表都指向了a1...an成员，这样肯定会出问题.之前get_file_area_from_file_stat_list()函数报错
		 *"list_add corruption. next->prev should be prev"而crash估计就是这个原因!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		 */
		INIT_LIST_HEAD(file_stat_free_list);
		spin_unlock(&p_hot_cold_file_global->global_lock);
	}

	//释放的page个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages = free_pages;
	//隔离的page个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.isolate_lru_pages = isolate_lru_pages;
	//file_stat的refault链表转移到temp链表的file_area个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_refault_to_temp_list_count = file_area_refault_to_temp_list_count;
	//释放的file_area结构个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_free_count = file_area_free_count;
	//file_stat的hot链表转移到temp链表的file_area个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_hot_to_temp_list_count = file_area_hot_to_temp_list_count;
	//扫描到的大文件转小文件的个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_large_to_small_count = scan_large_to_small_count;

	if(shrink_page_printk_open)
		printk("5:%s %s %d p_hot_cold_file_global:0x%llx free_pages:%d isolate_lru_pages:%d file_stat_temp_head:0x%llx file_area_free_count:%d file_area_refault_to_list_temp_count:%d file_area_hot_to_temp_list_count:%d\n",__func__,current->comm,current->pid,(u64)p_hot_cold_file_global,free_pages,isolate_lru_pages,(u64)file_stat_temp_head,file_area_free_count,file_area_refault_to_temp_list_count,file_area_hot_to_temp_list_count);
	return free_pages;
}

static int walk_throuth_all_file_area(struct hot_cold_file_global *p_hot_cold_file_global)
{
	struct file_stat * p_file_stat,*p_file_stat_temp;
	struct file_area *p_file_area,*p_file_area_temp;
	//LIST_HEAD(file_area_list);
	LIST_HEAD(file_stat_free_list_from_head_temp);
	LIST_HEAD(file_stat_free_list_from_head_temp_large);
	unsigned int scan_file_area_max,scan_file_stat_max;
	unsigned int scan_cold_file_area_count = 0;
	unsigned long nr_reclaimed = 0;
	unsigned int cold_file_area_count;
	unsigned int file_area_hot_to_temp_list_count = 0;
	unsigned int del_file_stat_count = 0,del_file_area_count = 0;

	memset(&p_hot_cold_file_global->hot_cold_file_shrink_counter,0,sizeof(struct hot_cold_file_shrink_counter));
	
	scan_file_stat_max = 10;
	scan_file_area_max = 1024;
	/*遍历hot_cold_file_global->file_stat_temp_large_file_head链表尾巴上边的大文件file_stat，然后遍历这些大文件file_stat的file_stat->file_area_temp
	 *链表尾巴上的file_area，被判定是冷的file_area则移动到file_stat->file_area_free_temp链表。把有冷file_area的file_stat移动到
	  file_stat_free_list_from_head_temp_large临时链表。返回值是遍历到的冷file_area个数*/
	scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,scan_file_area_max,scan_file_stat_max, 
			&p_hot_cold_file_global->file_stat_large_file_head,&file_stat_free_list_from_head_temp_large);
	//需要调度的话休眠一下
	cond_resched();
	scan_file_stat_max = 64;
	scan_file_area_max = 1024;
	/*遍历hot_cold_file_global->file_stat_temp_head链表尾巴上边的小文件file_stat，然后遍历这些小文件file_stat的file_stat->file_area_temp
	 *链表尾巴上的file_area，被判定是冷的file_area则移动到file_stat->file_area_free_temp链表。把有冷file_area的file_stat移动到
	 *file_stat_free_list_from_head_temp临时链表。返回值是遍历到的冷file_area个数*/
	scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,scan_file_area_max,scan_file_stat_max, 
			&p_hot_cold_file_global->file_stat_temp_head,&file_stat_free_list_from_head_temp);

	if(0 == test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		return 0;
	/*该函数主要有5个作用
	 * 1：释放file_stat_free_list_from_head_temp_large链表上的file_stat的file_area_free_temp链表上冷file_area的page。释放这些page后，把这些
	 *   file_area移动到file_stat->file_area_free链表头
	 * 2：遍历file_stat_free_list_from_head_temp_large的file_area_hot链表尾上的热file_area，如果长时间没有被访问，说明变成冷file_area了，
	 *   则移动到file_stat->file_area_temp链表头
	 * 3：遍历file_stat_free_list_from_head_temp_large链表上的file_stat的file_area_free链表尾上的file_area，如果还是长时间没有被访问，
	 *   则释放掉这些file_area结构
	 * 4: 遍历file_stat_free_list_from_head_temp_large链表上的file_stat的file_area_refault链表尾巴的file_area，如果长时间没有被访问，则移动
	 *   到file_stat->file_area_temp链表头
	 * 5: 把file_stat_free_list_from_head_temp_large链表上的file_stat再移动回file_stat_temp_head链表(即global file_stat_temp_head或
	 *   file_stat_temp_large_file_head)头，这样下轮walk_throuth_all_file_area()再扫描，从global file_stat_temp_head或
	 *   file_stat_temp_large_file_head链表尾巴扫到的file_stat都是最近没有被扫描过的，避免重复扫描
	 */
	nr_reclaimed =  free_page_from_file_area(p_hot_cold_file_global,&file_stat_free_list_from_head_temp_large,&p_hot_cold_file_global->file_stat_large_file_head); 
	nr_reclaimed += free_page_from_file_area(p_hot_cold_file_global,&file_stat_free_list_from_head_temp,&p_hot_cold_file_global->file_stat_temp_head); 

	if(0 == test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		return 0;

	/*遍历hot_cold_file_global->file_stat_hot_head链表上的热文件file_stat，如果哪些file_stat不再是热文件，再要把file_stat移动回
	 *global->file_stat_temp_head或file_stat_temp_large_file_head链表*/
	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->file_stat_hot_head,hot_cold_file_list){
		if(!file_stat_in_file_stat_hot_head_list(p_file_stat) || file_stat_in_file_stat_hot_head_list_error(p_file_stat))
			panic("%s file_stat:0x%llx not int file_stat_hot_head_list status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		cold_file_area_count = 0;
		/*遍历global->file_stat_hot_head上的热文件file_stat的file_area_hot链表上的热file_area，如果哪些file_area不再被访问了，则要把
		 *file_area移动回file_stat->file_area_temp链表。同时令改文件的热file_area个数file_stat->file_area_hot_count减1*/
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat->file_area_hot,file_area_list){
			//file_stat->file_area_hot尾巴上长时间未被访问的file_area再降级移动回file_stat->file_area_temp链表头
			if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > p_hot_cold_file_global->file_area_hot_to_temp_age_dx){
				cold_file_area_count = 0;
				if(!file_area_in_hot_list(p_file_area) || file_area_in_hot_list_error(p_file_area))
					panic("%s file_area:0x%llx status:0x%x not in file_area_hot\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

				file_area_hot_to_temp_list_count ++;
				//每遍历到一个就加一次锁，浪费性能，可以先移动到一个临时链表上，循环结束后加一次锁，然后把这些file_area或file_stat移动到目标链表??????????????
				spin_lock(&p_file_stat->file_stat_lock);
				p_file_stat->file_area_hot_count --;
				clear_file_area_in_hot_list(p_file_area);
				set_file_area_in_temp_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
				spin_unlock(&p_file_stat->file_stat_lock);	    
			}else{//到这里，file_area被判定还是热file_area，还是继续存在file_stat->file_area_hot链表

				/*如果file_stat->file_area_hot尾巴上连续出现2个file_area还是热file_area，则说明file_stat->file_area_hot链表尾巴上的冷
				 *file_area都遍历完了,遇到链表头的热file_area了，则停止遍历。file_stat->file_area_hot链表头到链表尾，file_area是
				 *由热到冷顺序排布的。之所以要限制连续碰到两个热file_area再break，是因为file_stat->file_area_hot尾巴上的冷file_area
				 *可能此时hot_file_update_file_status()中并发被频繁访问，变成热file_area，但还没来得及移动到file_stat->file_area_hot链表头
				 */
				if(cold_file_area_count ++ > 1)
					break;
			}
		}

		/*该文件file_stat的热file_area个数file_stat->file_area_hot_count小于阀值，则被判定不再是热文件
		  然后file_stat就要移动回hot_cold_file_global->file_stat_temp_head 或 hot_cold_file_global->file_stat_temp_large_file_head链表*/
		if(!is_file_stat_hot_file(p_hot_cold_file_global,p_file_stat)){

			spin_lock(&p_hot_cold_file_global->global_lock);
			hot_cold_file_global_info.file_stat_hot_count --;//热文件数减1
			clear_file_stat_in_file_stat_hot_head_list(p_file_stat);
			set_file_stat_in_file_stat_temp_head_list(p_file_stat);//设置file_stat状态为in_head_temp_list
			if(file_stat_in_large_file(p_file_stat)){
				//set_file_stat_in_large_file(p_file_stat);重复设置状态
				//p_hot_cold_file_global->file_stat_large_count ++;//这不是新产生的大文件，已经加过1了 
				list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_large_file_head);
			}
			else
				list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_temp_head);
			spin_unlock(&p_hot_cold_file_global->global_lock);
		}
	}

	if(0 == test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		return 0;

	/*遍历global file_stat_delete_head链表上已经被删除的文件的file_stat，
	  一次不能删除太多的file_stat对应的file_area，会长时间占有cpu，后期需要调优一下*/
	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->file_stat_delete_head,hot_cold_file_list){
		if(!file_stat_in_delete(p_file_stat) /*|| file_stat_in_delete_error(p_file_stat)*/)
			panic("%s file_stat:0x%llx not delete status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,&p_file_stat->file_stat_base,FILE_STAT_NORMAL);
		del_file_stat_count ++;
	}
	//释放的file_area个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.del_file_area_count = del_file_area_count;
	//释放的file_stat个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.del_file_stat_count = del_file_stat_count;

	if(0 == test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		return 0;

	//对没有file_area的file_stat的处理
	file_stat_has_zero_file_area_manage(p_hot_cold_file_global);
#if 0
	//如果此时echo 触发了drop_cache，ASYNC_DROP_CACHES置1，则禁止异步内存回收线程处理global drop_cache_file_stat_head链表上的file_stat
	if(!test_bit(ASYNC_DROP_CACHES, &async_memory_reclaim_status))
	    //处理drop cache的文件的pagecache
	    drop_cache_truncate_inode_pages(p_hot_cold_file_global);
#endif
	if(0 == test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		return 0;

	//打印所有file_stat的file_area个数和page个数
	if(shrink_page_printk_open1)
	    hot_cold_file_print_all_file_stat(p_hot_cold_file_global,NULL,0);
	//打印内存回收时统计的各个参数
	if(shrink_page_printk_open1)
	    printk_shrink_param(p_hot_cold_file_global,NULL,0);

	/*每个周期打印hot_cold_file_shrink_counter参数后清0*/
	//memset(&p_hot_cold_file_global->hot_cold_file_shrink_counter,0,sizeof(struct hot_cold_file_shrink_counter));
	return 0;
}
#endif
/*遍历global file_stat_zero_file_area_head链表上的file_stat，如果file_stat对应文件长时间不被访问杂释放掉file_stat。如果file_stat对应文件又被访问了，
  则把file_stat再移动回 gloabl file_stat_temp_head、file_stat_large_file_head、file_stat_hot_head链表*/
static noinline void file_stat_has_zero_file_area_manage(struct hot_cold_file_global *p_hot_cold_file_global)
{
	struct file_stat *p_file_stat,*p_file_stat_temp;
	unsigned int scan_file_stat_max = 128,scan_file_stat_count = 0;
	unsigned int del_file_stat_count = 0;
	unsigned int file_stat_type;
	/*由于get_file_area_from_file_stat_list()向global file_stat_zero_file_area_head链表添加成员，这里遍历file_stat_zero_file_area_head链表成员，
	 *都是在异步内存回收线程进行的，不用spin_lock(&p_hot_cold_file_global->global_lock)加锁。除非要把file_stat_zero_file_area_head链表上的file_stat
	 *移动到 gloabl file_stat_temp_head、file_stat_large_file_head、file_stat_hot_head链表。*/

	//向global  file_stat_zero_file_area_head添加成员是向链表头添加的，遍历则从链表尾巴开始遍历
	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->file_stat_zero_file_area_head,hot_cold_file_list){
		if(!file_stat_in_zero_file_area_list(p_file_stat) || file_stat_in_zero_file_area_list_error(p_file_stat))
			panic("%s file_stat:0x%llx not in_zero_file_area_list status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		if(scan_file_stat_count++ > scan_file_stat_max)
			break;

		//如果file_stat对应文件长时间不被访问杂释放掉file_stat结构，这个过程不用spin_lock(&p_hot_cold_file_global->global_lock)加锁
		if(p_file_stat->file_area_count == 0 && p_hot_cold_file_global->global_age - p_file_stat->max_file_area_age > p_hot_cold_file_global->file_stat_delete_age_dx){
#if 0	
			/*如果该文件有pagecache没有被file_area统计到，则释放释放文件的pagecache。放到这里有问题，如果文件inode此时被删除了怎么办???????
			  决定在里边lock_file_stat()加锁，防护inode被删除*/
			file_stat_free_leak_page(p_hot_cold_file_global,p_file_stat);
#endif			

			//如果返回值大于0说明file_stat对应文件被并发访问了，于是goto file_stat_access分支处理
			if(cold_file_stat_delete(p_hot_cold_file_global,&p_file_stat->file_stat_base,FILE_STAT_NORMAL) > 0)
				goto file_stat_access;

			del_file_stat_count ++;
			//0个file_area的file_stat个数减1
			p_hot_cold_file_global->file_stat_count_zero_file_area --;
		}
		/*如果p_file_stat->file_area_count大于0，说明最近被访问了，则把file_stat移动回 gloabl file_stat_temp_head、file_stat_large_file_head、
		 *file_stat_hot_head链表。hot_file_update_file_status()不会把file_stat移动回热文件或大文件或普通文件链表吗？不会，因为此时file_stat是
		 *in_zero_file_area_list状态，只有file_stat_in_temp_list状态才会移动到*/
		else if (p_file_stat->file_area_count > 0)
		{
file_stat_access:		
			//0个file_area的file_stat个数减1
			p_hot_cold_file_global->file_stat_count_zero_file_area --;
			/*file_stat可能是普通文件、中型文件、大文件，则移动到对应global 链表。也可能是热文件，不理会，
			 *异步内存回收线程里会处理*/
			spin_lock(&p_hot_cold_file_global->global_lock);
			if(!file_stat_in_delete(p_file_stat)){
				clear_file_stat_in_file_stat_hot_head_list(p_file_stat);

				/* 为了不加锁，即便文件是普通文件也是移动到global middle_file链表。错了，NO!!!!!!!!!!!!!!
				 * 错了，因为一共有两种并发：
				 * 普通文件升级到中型文件，必须要加锁。要考虑两种并发，都会并发修改global temp链表头。
				 * 1：读写文件进程执行__filemap_add_folio()向global temp添加file_stat到global temp链表头。如果只有
				 * 这行并发，file_stat不移动到global temp链表就不用global lock加锁。但还有一种并发，iput()释放inode
				 * 2：iput()释放inode并标记file_stat的delete，然后把file_stat从global任一个链表移动到global delete链表。
				 * 此时的file_stat可能处于global temp、hot、large、middle、zero链表。因此要防护这个file_stat被iput()
				 * 并发标记file_stat delete并把file_stat移动到global delete链表。
				 *
				 * 做个总结：凡是file_stat在global temp、hot、large、middle、zero链表之间相互移动，都必须要
				 * global lock加锁，然后判断file_stat是否被iput()释放inode并标记delete!!!!!!!!!!!!!!!!!!!
				 */
				file_stat_type = is_file_stat_file_type(p_hot_cold_file_global,p_file_stat);
				if(TEMP_FILE == file_stat_type){
					set_file_stat_in_file_stat_temp_head_list(p_file_stat);
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_middle_file_head);
				}else if(MIDDLE_FILE == file_stat_type){
					set_file_stat_in_file_stat_middle_file_head_list(p_file_stat);
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_middle_file_head);
				}
				else{
					set_file_stat_in_file_stat_large_file_head_list(p_file_stat);
					p_hot_cold_file_global->file_stat_large_count ++;
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_large_file_head);
				}
			}
			spin_unlock(&p_hot_cold_file_global->global_lock);
		}
	}

	p_hot_cold_file_global->hot_cold_file_shrink_counter.del_zero_file_area_file_stat_count += del_file_stat_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_zero_file_area_file_stat_count = scan_file_stat_count;
}
/***oldnew********以上是老版本异步内存回收线程file_area内存回收核心代码，但是有一些问题，暂时停用，开发一个新版本***********/

void file_stat_temp_middle_large_file_change(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,unsigned char file_stat_list_type, unsigned int file_type,char is_cache_file)
{
	spinlock_t *cache_or_mmap_file_global_lock;

	/*cache文件使用global_lock锁，mmap文件用的mmap_file_global_lock锁*/
	if(is_cache_file)
		cache_or_mmap_file_global_lock = &p_hot_cold_file_global->global_lock;
	else
		cache_or_mmap_file_global_lock = &p_hot_cold_file_global->mmap_file_global_lock;

	switch(file_stat_list_type){
		case FILE_STAT_IN_TEMP_FILE_LIST:
			/*普通文件升级到中型文件，必须要加锁。要考虑两种并发，都会并发修改global temp链表头
			 *1：iput()中会并发标记file_stat的delete，并且把file_stat从global temp链表头移动到global delete链表
			 2：读写文件进程执行__filemap_add_folio()向global temp添加file_stat到global temp链表头*/
			if(MIDDLE_FILE == file_type){
				spin_lock(cache_or_mmap_file_global_lock);
				/*加锁后必须再判断一次file_stat的状态，如果file_stat被iput()并发标记delete了，就不能再移动file_stat到其他链表了。这点很重要!!!!!!!!!!!!*/
				if(!file_stat_in_delete(p_file_stat)){
					if(!file_stat_in_file_stat_temp_head_list(p_file_stat) || file_stat_in_file_stat_temp_head_list_error(p_file_stat))
						panic("%s file_stat:0x%llx status:0x%lx not in temp_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

					clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
					set_file_stat_in_file_stat_middle_file_head_list(p_file_stat);
					if(is_cache_file)
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_middle_file_head);
					else 
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_middle_file_head);
				}
				spin_unlock(cache_or_mmap_file_global_lock);
			}
			/*普通文件升级到大文件，必须要加锁*/
			else if(LARGE_FILE == file_type){
				spin_lock(cache_or_mmap_file_global_lock);
				/*加锁后必须再判断一次file_stat的状态，如果file_stat被iput()并发标记delete了，就不能再移动file_stat到其他链表了。这点很重要!!!!!!!!!!!!*/
				if(!file_stat_in_delete(p_file_stat)){
					if(!file_stat_in_file_stat_temp_head_list(p_file_stat) || file_stat_in_file_stat_temp_head_list_error(p_file_stat))
						panic("%s file_stat:0x%llx status:0x%lx not in temp_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

					clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
					set_file_stat_in_file_stat_large_file_head_list(p_file_stat);
					if(is_cache_file)
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_large_file_head);
					else 
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
				}
				spin_unlock(cache_or_mmap_file_global_lock);

				if(is_cache_file)
					p_hot_cold_file_global->file_stat_large_count ++;
			}

			break;
		case FILE_STAT_IN_MIDDLE_FILE_LIST:
			/*中型文件降级到普通文件，必须要加锁*/
			if(TEMP_FILE == file_type){
				spin_lock(cache_or_mmap_file_global_lock);
				/*加锁后必须再判断一次file_stat的状态，如果file_stat被iput()并发标记delete了，就不能再移动file_stat到其他链表了。这点很重要!!!!!!!!!!!!*/
				if(!file_stat_in_delete(p_file_stat)){
					if(!file_stat_in_file_stat_middle_file_head_list(p_file_stat) || file_stat_in_file_stat_middle_file_head_list_error(p_file_stat))
						panic("%s file_stat:0x%llx status:0x%lx not in middle_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

					clear_file_stat_in_file_stat_middle_file_head_list(p_file_stat);
					set_file_stat_in_file_stat_temp_head_list(p_file_stat);
					if(is_cache_file)
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_temp_head);
					else 
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
				}
				spin_unlock(cache_or_mmap_file_global_lock);
			}
			/*中型文件升级到大文件，不需要加锁。NO!!!必须要加锁，因为iput()中会并发标记file_stat的delete，并且把file_stat移动到global delete链表*/
			else if(LARGE_FILE == file_type){
				spin_lock(cache_or_mmap_file_global_lock);
				/*加锁后必须再判断一次file_stat的状态，如果file_stat被iput()并发标记delete了，就不能再移动file_stat到其他链表了。这点很重要!!!!!!!!!!!!*/
				if(!file_stat_in_delete(p_file_stat)){
					if(!file_stat_in_file_stat_middle_file_head_list(p_file_stat) || file_stat_in_file_stat_middle_file_head_list_error(p_file_stat))
						panic("%s file_stat:0x%llx status:0x%lx not in middle_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
					clear_file_stat_in_file_stat_middle_file_head_list(p_file_stat);
					set_file_stat_in_file_stat_large_file_head_list(p_file_stat);
					if(is_cache_file)
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_large_file_head);
					else 
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
				}
				spin_unlock(cache_or_mmap_file_global_lock);

				if(is_cache_file) 
					p_hot_cold_file_global->file_stat_large_count ++;
			}

			break;
		case FILE_STAT_IN_LARGE_FILE_LIST:
			/*大文件降级到普通文件，必须要加锁。NO!!!必须要加锁，因为iput()中会并发标记file_stat的delete，并且把file_stat移动到global delete链表*/
			if(TEMP_FILE == file_type){
				spin_lock(cache_or_mmap_file_global_lock);
				/*加锁后必须再判断一次file_stat的状态，如果file_stat被iput()并发标记delete了，就不能再移动file_stat到其他链表了。这点很重要!!!!!!!!!!!!*/
				if(!file_stat_in_delete(p_file_stat)){
					if(!file_stat_in_file_stat_large_file_head_list(p_file_stat) || file_stat_in_file_stat_large_file_head_list_error(p_file_stat))
						panic("%s file_stat:0x%llx status:0x%lx not in large_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
					clear_file_stat_in_file_stat_large_file_head_list(p_file_stat);
					set_file_stat_in_file_stat_temp_head_list(p_file_stat);
					if(is_cache_file)
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_temp_head);
					else
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
				}
				spin_unlock(cache_or_mmap_file_global_lock);

				if(is_cache_file)
					p_hot_cold_file_global->file_stat_large_count --;
			}
			/*大文件升级到中型文件，不需要加锁.NO!!!必须要加锁，因为iput()中会并发标记file_stat的delete，并且把file_stat移动到global delete链表*/
			else if(MIDDLE_FILE == file_type){
				spin_lock(cache_or_mmap_file_global_lock);
				/*加锁后必须再判断一次file_stat的状态，如果file_stat被iput()并发标记delete了，就不能再移动file_stat到其他链表了。这点很重要!!!!!!!!!!!!*/
				if(!file_stat_in_delete(p_file_stat)){
					if(!file_stat_in_file_stat_large_file_head_list(p_file_stat) || file_stat_in_file_stat_large_file_head_list_error(p_file_stat))
						panic("%s file_stat:0x%llx status:0x%lx not in large_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
					clear_file_stat_in_file_stat_large_file_head_list(p_file_stat);
					set_file_stat_in_file_stat_middle_file_head_list(p_file_stat);
					if(is_cache_file)
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_middle_file_head);
					else 
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_middle_file_head);
				}
				spin_unlock(cache_or_mmap_file_global_lock);

				if(is_cache_file)
					p_hot_cold_file_global->file_stat_large_count --;
			}

			break;
		default:
			panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat,file_stat_list_type);
			break;
	}	

}
/*查看文件是否变成了热文件、大文件、普通文件，是的话则file_stat移动到对应global hot、large_file_temp、temp 链表*/
static noinline int file_stat_status_change_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,unsigned char file_stat_list_type)
{
	/* 此时存在并发移动file_stat的情况：iput()释放file_stat，标记file_stat delete，并把file_stat移动到
	 * global delete链表。因此要global_lock加锁后再判断一次file_stat是否被iput()把file_stat移动到global delete链表了*/
	if(is_file_stat_hot_file(p_hot_cold_file_global,p_file_stat)){//热文件
		spin_lock(&p_hot_cold_file_global->global_lock);
		/*如果file_stat被iput()并发标记delete了，则不再理会*/
		if(!file_stat_in_delete(p_file_stat)){
			if(FILE_STAT_IN_TEMP_FILE_LIST == file_stat_list_type)
				clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			else if(FILE_STAT_IN_MIDDLE_FILE_LIST == file_stat_list_type)
				clear_file_stat_in_file_stat_middle_file_head_list(p_file_stat);
			else{ 
				if(FILE_STAT_IN_LARGE_FILE_LIST != file_stat_list_type)
					panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat,file_stat_list_type);
				p_hot_cold_file_global->file_stat_large_count --;
				clear_file_stat_in_file_stat_large_file_head_list(p_file_stat);
			}

			set_file_stat_in_file_stat_hot_head_list(p_file_stat);
			p_hot_cold_file_global->file_stat_hot_count ++;//热文件数加1 
			list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_hot_head);
			//p_file_stat->file_stat_hot_base_age = p_hot_cold_file_global->global_age;
		}
		spin_unlock(&p_hot_cold_file_global->global_lock);
	}else{
		/*根据file_stat的file_area个数判断文件是普通文件、中型文件、大文件*/
		unsigned int file_type = is_file_stat_file_type(p_hot_cold_file_global,p_file_stat);

		file_stat_temp_middle_large_file_change(p_hot_cold_file_global,p_file_stat,file_stat_list_type,file_type,1);
	}

	return 0;
}

/* 现在规定只有file_stat->warm上长时间没访问的file_area才会移动到file_stat->temp链表，file_stat->refault、hot、free
 * 上的file_area只会移动到file_stat->warmm链表。为什么要这样？因为要减少使用spin_lock(&p_file_stat->file_stat_lock)加锁：
 * 向xarray tree增加page而执行__filemap_add_folio()函数时，要先执行spin_lock(&p_file_stat->file_stat_lock)加锁，然后
 * 向file_stat->temp链表添加为新的page分配的file_area。异步内存回收线程里，如果将file_stat->refault、hot、free链表频繁
 * 移动到file_stat->temp链表，二者将发生锁竞争，导致__filemap_add_folio()会频繁因spin_lock(&p_file_stat->file_stat_lock)
 * 锁竞争而耗时长，得不偿失。现在只有file_stat->warm上的file_area才会移动到file_stat->temp链表，并且经过算法优化，可以
 * 做到异步内存回收线程每次运行：先把file_stat->warm链表上符合条件的file_area移动到临时链表，然后只
 * spin_lock(&p_file_stat->file_stat_lock)加锁一次，就可以把这些file_area移动到file_stat->temp链表，大大减少了加锁次数。
 */

/*遍历file_stat->hot、refault、free链表上的各种file_area的处理*/
static noinline unsigned int file_stat_other_list_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,struct list_head *file_area_list_head,unsigned int scan_file_area_max,unsigned char file_area_type)
{
	struct file_area *p_file_area,*p_file_area_temp;
	int scan_file_area_count = 0;
	unsigned int file_area_hot_to_warm_list_count = 0;
	unsigned int file_area_refault_to_warm_list_count = 0;
	unsigned int file_area_free_to_refault_list_count = 0;
	unsigned int file_area_free_count = 0;
	unsigned int file_area_in_list_type = -1;

	/*file_area_list_head来自file_stat->file_area_hot、file_stat->file_area_free,file_stat->file_area_refault链表头*/
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,file_area_list_head,file_area_list){
		if(scan_file_area_count ++ > scan_file_area_max)
			break;

		switch (file_area_type){
			case FILE_AREA_IN_HOT_LIST:
				if(!file_area_in_hot_list(p_file_area) || file_area_in_hot_list_error(p_file_area))
					panic("%s file_area:0x%llx status:%d not in file_area_hot\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
				if(-1 == file_area_in_list_type)
					file_area_in_list_type = F_file_area_in_hot_list;

				//file_stat->file_area_hot尾巴上长时间未被访问的file_area再降级移动回file_stat->file_area_temp链表头
				if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > p_hot_cold_file_global->file_area_hot_to_temp_age_dx){
					/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
					if(file_area_in_ahead(p_file_area)){
						clear_file_area_in_ahead(p_file_area);
						/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
						if(IS_MEMORY_ENOUGH(p_hot_cold_file_global))
							break;
					}
					file_area_hot_to_warm_list_count ++;
					//spin_lock(&p_file_stat->file_stat_lock);	    
					clear_file_area_in_hot_list(p_file_area);
					//file_stat的热file_area个数减1
					p_file_stat->file_area_hot_count --;
					set_file_area_in_warm_list(p_file_area);
					/*hot链表上长时间没访问的file_area现在移动到file_stat->warm链表，而不是file_stat->temp链表，这个不用spin_lock(&p_file_stat->file_stat_lock)加锁*/
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
					//spin_unlock(&p_file_stat->file_stat_lock);	    
				}

				break;
			case FILE_AREA_IN_REFAULT_LIST:
				if(!file_area_in_refault_list(p_file_area) || file_area_in_refault_list_error(p_file_area))
					panic("%s file_area:0x%llx status:%d not in file_area_refault\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
				if(-1 == file_area_in_list_type)
					file_area_in_list_type = F_file_area_in_refault_list;

				//file_stat->file_area_hot尾巴上长时间未被访问的file_area再降级移动回file_stat->file_area_temp链表头
				if(p_hot_cold_file_global->global_age - p_file_area->file_area_age >  p_hot_cold_file_global->file_area_refault_to_temp_age_dx){
					/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
					if(file_area_in_ahead(p_file_area)){
						clear_file_area_in_ahead(p_file_area);
						/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
						if(IS_MEMORY_ENOUGH(p_hot_cold_file_global))
							break;
					}
					file_area_refault_to_warm_list_count ++;
					//spin_lock(&p_file_stat->file_stat_lock);	    
					clear_file_area_in_refault_list(p_file_area);
					set_file_area_in_warm_list(p_file_area);
					/*refault链表上长时间没访问的file_area现在移动到file_stat->warm链表，而不是file_stat->temp链表，这个不用spin_lock(&p_file_stat->file_stat_lock)加锁*/
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
					//spin_unlock(&p_file_stat->file_stat_lock);	    
				}

				break;
			case FILE_AREA_IN_FREE_LIST:
				if(-1 == file_area_in_list_type)
					file_area_in_list_type = F_file_area_in_free_list;

				/*1：如果在file_stat->free链表上的file_area，被访问在hot_file_update_file_status()被访问而设置了
				  in_refault_list标记，这里遍历到这种file_area就移动到file_stat->refault链表。
				  2：file_area的访问计数大于0也说明file_area在内存回收过程中被访问了，则也要移动到file_stat->refault链表
				  3：如果file_area的page数大于0，说明对file_area的page内存回收失败，还残留page。这种file_area也要移动到
				  file_stat->refault链表，长时间禁止内存回收*/
				if(file_area_in_refault_list(p_file_area) || unlikely(file_area_access_count_get(p_file_area) > 0
							|| file_area_have_page(p_file_area))){
					/*这段代码时新加的，是个隐藏很深的小bug。file_area在内存回收前都要对access_count清0，但是在内存回收最后，可能因对应page
					 *被访问了而access_count加1，然后对age赋值为当时的global age，但是file_area的page内存回收失败了。等了很长时间后，终于再次
					 *扫描到这个文件file_stat，但是file_area的age还是与global age相差很大了，正常就要判定这个file_area长时间没访问而释放掉。
					 *但这是正常现象不合理的！因为这个file_area的page在内存回收时被访问了。于是就通过file_area的access_count大于0而判定这个file_area的
					 *page在内存回收最后被访问了，于是就不能释放掉file_area。那就要移动到file_stat->temp链表或者refault链表!!!!!!!!!!!!!!!!!!!!*/

					/*file_area必须有in_free_list状态，否则crash，防止file_area重复移动到file_stat->refault链表*/
					if(!file_area_in_free_list(p_file_area) || (file_area_in_free_list_error(p_file_area) && !file_area_in_refault_list(p_file_area)))
						panic("%s file_area:0x%llx status:%d not in file_area_free error\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

					/*把file_area移动到file_stat->refault链表，必须对访问计数清0。否则后续该file_area不再访问则
					 *访问计数一直大于0，该file_area移动到file_stat->free链表后，因访问计数大于0每次都要移动到file_stat->refault链表*/
					file_area_access_count_clear(p_file_area);
					file_area_free_to_refault_list_count ++;
					//spin_lock(&p_file_stat->file_stat_lock);	    
					clear_file_area_in_free_list(p_file_area);
					set_file_area_in_refault_list(p_file_area);
					/*refault链表上长时间没访问的file_area现在移动到file_stat->warm链表，而不是file_stat->temp链表，这个不用spin_lock(&p_file_stat->file_stat_lock)加锁*/
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
					/*检测到refault的file_area个数加1*/
					p_hot_cold_file_global->check_refault_file_area_count ++;
					//spin_unlock(&p_file_stat->file_stat_lock);	    
					printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x accessed in reclaim\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);
				}
				/*如果file_stat->file_area_free链表上的file_area长时间没有被访问则释放掉file_area结构。之前的代码有问题，判定释放file_area的时间是
				 *file_area_free_age_dx，这样有问题，会导致file_area被内存回收后，在下个周期file_area立即被释放掉。原因是file_area_free_age_dx=5，
				 file_area_temp_to_cold_age_dx=5，下个内存回收周期 global_age - file_area_free_age_dx肯定大于5*/
				else if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > 
						(p_hot_cold_file_global->file_area_free_age_dx + p_hot_cold_file_global->file_area_temp_to_cold_age_dx)){
					/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
					if(file_area_in_ahead(p_file_area)){
						clear_file_area_in_ahead(p_file_area);
						/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
						if(IS_MEMORY_ENOUGH(p_hot_cold_file_global))
							break;
					}
					file_area_free_count ++;
					/*hot_file_update_file_status()函数中会并发把file_area从file_stat->file_area_free链表移动到file_stat->file_area_free_temp
					 *链表.这里把file_stat->file_area_free链表上的file_area剔除掉并释放掉，需要spin_lock(&p_file_stat->file_stat_lock)加锁，
					 *这个函数里有加锁*/
					if(cold_file_area_delete(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area) > 0){
						/*在释放file_area过程发现file_area分配page了，于是把file_area移动到file_stat->refault链表*/
						file_area_access_count_clear(p_file_area);
						file_area_free_to_refault_list_count ++;
						clear_file_area_in_free_list(p_file_area);
						set_file_area_in_refault_list(p_file_area);
						list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
						/*检测到refault的file_area个数加1*/
						p_hot_cold_file_global->check_refault_file_area_count ++;
					}
				}

				break;
			default:
				break;
		}
	}
	/* file_area_list_head链表非空且p_file_area不是指向链表头且p_file_area不是该链表的第一个成员，
	 * 则执行list_move_enhance()把本次遍历过的file_area~链表尾的file_area移动到链表
	 * file_area_list_head头，这样下次从链表尾遍历的是新的未遍历过的file_area。
	 * 当上边循环是遍历完所有链表所有成员才退出，p_file_area此时指向的是链表头file_area_list_head，
	 * 此时 p_file_area->file_area_list 跟 file_area_list_head 就相等了。当然，这种情况，是绝对不能
	 * 把p_file_area到链表尾的file_area移动到file_area_list_head链表头了，会出严重内核bug*/

	/* 在把遍历过的状态已经改变的file_area移动到file_area_list_head链表头时，必须判断p_file_area不能
	 * 不是链表头。否则出现了一个bug，因p_file_area是链表头而导致can_file_area_move_to_list_head()误判
	 * 该p_file_area状态合法，而错误执行list_move_enhance()把遍历过的file_area移动到链表头*/
	//if(!list_empty(file_area_list_head) && p_file_area->file_area_list != file_area_list_head && p_file_area->file_area_list != file_area_list_head.next)
	{
		/*将链表尾已经遍历过的file_area移动到file_area_list_head链表头，下次从链表尾遍历的才是新的未遍历过的file_area。此时不用加锁!!!!!!!!!!!!!*/
		if(can_file_area_move_to_list_head(p_file_area,file_area_list_head,file_area_in_list_type))
			list_move_enhance(file_area_list_head,&p_file_area->file_area_list);
	}

	p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_refault_to_warm_list_count += file_area_refault_to_warm_list_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_hot_to_warm_list_count += file_area_hot_to_warm_list_count;
	//释放的file_area结构个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_free_count_from_free_list += file_area_free_count;

	return scan_file_area_count;
}
/*遍历file_stat->temp链表上的file_area*/
static noinline int file_stat_temp_list_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,unsigned int scan_file_area_max,struct list_head *file_area_free_temp)
{
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int scan_file_area_count = 0;
	unsigned int file_area_age_dx;
	unsigned int temp_to_warm_file_area_count = 0,scan_cold_file_area_count = 0;
	unsigned int scan_read_file_area_count = 0;
	unsigned int scan_ahead_file_area_count = 0;
	unsigned int temp_to_hot_file_area_count = 0;

	//从链表尾开始遍历，链表尾的成员更老，链表头的成员是最新添加的
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat->file_area_temp,file_area_list){

		/* 新的版本hot_file_update_file_status()遇到refault、hot file_area，只是做个标记，而不会把file_area移动到
		 * file_stat->refault、hot链表，因此file_stat->temp链表上的file_area除了有in_temp_list标记，还有
		 * in_refault_list、in_hot_list标记，故要把file_area_in_temp_list_error(p_file_area)判断去掉*/
#if 0
		if(!file_area_in_temp_list(p_file_area)  || file_area_in_temp_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
#endif
		if(++ scan_file_area_count > scan_file_area_max)
			break;

		/*file_stat->temp链表上的file_area不可能有in_refault_list标记，file_stat->refault和free链表上的file_area才会有in_refault_list标记*/
#if 0
		/*一个file_area可能在hot_file_update_file_status()中被并发设置in_temp_list、in_hot_list、in_refault_list 
		 * 这3种属性，因此要if(file_area_in_refault_list(p_file_area))也需要判断清理in_hot_list属性。in_hot_list同理*/
		if(file_area_in_refault_list(p_file_area)){
			spin_lock(&p_file_stat->file_stat_lock);
			if(file_area_in_temp_list(p_file_area))
				clear_file_area_in_temp_list(p_file_area);
			if(file_area_in_hot_list(p_file_area))
				clear_file_area_in_hot_list(p_file_area);
			list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
			spin_unlock(&p_file_stat->file_stat_lock);
			continue;
		}
		else 
#endif
		if(file_area_in_hot_list(p_file_area)){
			spin_lock(&p_file_stat->file_stat_lock);
			/* 当file_area被判定为hot后，没有清理in_temp_list状态，因此第一次来这里，没有in_temp_list则crash，
			 * 防止重复把file_area移动到file_stat->hot链表.注意，该file_area可能在update函数被并发设置了in_hot标记*/
			if(!file_area_in_temp_list(p_file_area) || (file_area_in_temp_list_error(p_file_area) && !file_area_in_hot_list(p_file_area)))
			    panic("%s file_area:0x%llx status:%d not in file_area_temp error\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			clear_file_area_in_temp_list(p_file_area);
			//if(file_area_in_refault_list(p_file_area))
			//	clear_file_area_in_refault_list(p_file_area);
			/*热file_area肯定来自file_stat->temp链表，因此file_area移动到file_area->hot链表后要减1*/
			p_file_stat->file_area_count_in_temp_list --;
			list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
			spin_unlock(&p_file_stat->file_stat_lock);
			temp_to_hot_file_area_count ++;
			continue;
		}

		file_area_age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;

		if(file_area_age_dx > p_hot_cold_file_global->file_area_temp_to_warm_age_dx){
			/*file_area经过FILE_AREA_TEMP_TO_COLD_AGE_DX个周期还没有被访问，则被判定是冷file_area，然后就释放该file_area的page*/
			if(file_area_age_dx > p_hot_cold_file_global->file_area_temp_to_cold_age_dx){
				/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
				if(file_area_in_ahead(p_file_area)){
					clear_file_area_in_ahead(p_file_area);
					scan_ahead_file_area_count ++;
						/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
						if(IS_MEMORY_ENOUGH(p_hot_cold_file_global))
							continue;
				}
				/* 当前内存不紧张且内存回收不困难，则遇到read文件页的file_area先跳过，不回收。目的是尽可能回收write文件页，
				 * read文件页一旦回收再被访问就会发生refault，不仅导致read耗时长，读磁盘功耗还高。但是如果这个read属性的
				 * file_area很长很长很长时间没访问，也参与内存回收*/
				if(file_area_page_is_read(p_file_area)){
					scan_read_file_area_count ++;
					if(!(IS_IN_MEMORY_EMERGENCY_RECLAIM(p_hot_cold_file_global) || file_area_age_dx > p_hot_cold_file_global->file_area_reclaim_read_age_dx))
						continue;
					clear_file_area_page_read(p_file_area);
				}

				//每遍历到一个就加一次锁，浪费性能，可以先移动到一个临时链表上，循环结束后加一次锁，然后把这些file_area或file_stat移动到目标链表???????
				spin_lock(&p_file_stat->file_stat_lock);
				/*为什么file_stat_lock加锁后要再判断一次file_area是不是被访问了,因为可能有这种情况:上边的if成立,此时file_area还没被访问。但是此时有进程
				  先执行hot_file_update_file_status()获取file_stat_lock锁,然后访问当前file_area,file_area不再冷了,当前进程此时获取file_stat_lock锁失败,
				  等获取file_stat_lock锁成功后，file_area的file_area_age就和global_age相等了。变量加减后的判断，在spin_lock前后各判断一次有必要的!!!!!*/
				if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <  p_hot_cold_file_global->file_area_temp_to_cold_age_dx){
					spin_unlock(&p_file_stat->file_stat_lock);    
					continue;
				}
				//access_count清0，如果内存回收期间又被访问了，access_count将大于0，将被判断为refault page。
				file_area_access_count_clear(p_file_area);
				clear_file_area_in_temp_list(p_file_area);
				p_file_stat->file_area_count_in_temp_list --;
				/*设置file_area处于file_stat的free_temp_list链表。这里设定，不管file_area处于file_stat->file_area_free_temp还是
				 *file_stat->file_area_free链表，都是file_area_in_free_list状态，没有必要再区分二者。主要设置file_area的状态需要
				 遍历每个file_area并file_stat_lock加锁，再多设置一次set_file_area_in_free_temp_list状态浪费性能。这点需注意!!!!!!!!!!!!!*/
				set_file_area_in_free_list(p_file_area);
				/*需要加锁，此时可能有进程执行hot_file_update_file_status()并发向该p_file_area前或者后插入新的file_area，这里是把该
				 * p_file_area从file_area_temp链表剔除，存在同时修改该p_file_area在file_area_temp链表前的file_area结构的next指针和在链表后的
				 * file_area结构的prev指针，并发修改同一个变量就需要加锁*/
				//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free_temp);
				list_move(&p_file_area->file_area_list,file_area_free_temp);
				spin_unlock(&p_file_stat->file_stat_lock);

				scan_cold_file_area_count ++;
			}
			/*温file_area移动到p_file_stat->file_area_warm链表。从file_stat->temp移动file_area到file_stat->warm链表，必须要加锁*/
			else{
				spin_lock(&p_file_stat->file_stat_lock);
				clear_file_area_in_temp_list(p_file_area);
				p_file_stat->file_area_count_in_temp_list --;
				set_file_area_in_warm_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
				spin_unlock(&p_file_stat->file_stat_lock);
				temp_to_warm_file_area_count ++;
			}
		}
	}

	/* p_file_stat->file_area_temp链表非空且p_file_area不是指向链表头且p_file_area不是该链表的第一个成员，
	 * 则执行list_move_enhance()把本次遍历过的file_area~链表尾的file_area移动到链表
	 * p_file_stat->file_area_temp头，这样下次从链表尾遍历的是新的未遍历过的file_area。
	 * 当上边循环是遍历完所有链表所有成员才退出，p_file_area此时指向的是链表头p_file_stat->file_area_temp，
	 * 此时 p_file_area->file_area_list 跟 &p_file_stat->file_area_temp 就相等了。当然，这种情况，是绝对不能
	 * 把p_file_area到链表尾的file_area移动到p_file_stat->file_area_temp链表头了，会出严重内核bug*/
	//if(!list_empty(p_file_stat->file_area_temp) && p_file_area->file_area_list != &p_file_stat->file_area_temp && p_file_area->file_area_list != p_file_stat->file_area_temp.next)
	{
		/* 将链表尾已经遍历过的file_area移动到file_stat->temp链表头，下次从链表尾遍历的才是新的未遍历过的
		 * file_area。牵涉到file_stat->temp链表，增删file_area必须要加锁!!!!!!!!!!!!!!!*/
		spin_lock(&p_file_stat->file_stat_lock);
		if(can_file_area_move_to_list_head(p_file_area,&p_file_stat->file_area_temp,F_file_area_in_temp_list))
		    list_move_enhance(&p_file_stat->file_area_temp,&p_file_area->file_area_list);
		spin_unlock(&p_file_stat->file_stat_lock);
	}

	/*参与内存回收的冷file_area个数*/
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_cold_file_area_count_from_temp += scan_cold_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_read_file_area_count_from_temp += scan_read_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.temp_to_hot_file_area_count += temp_to_hot_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_ahead_file_area_count_from_temp += scan_ahead_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.temp_to_warm_file_area_count += temp_to_warm_file_area_count;
	return scan_file_area_count;
}
/* 现在规定只有file_stat->warm上近期访问过的file_area才会移动到file_stat->temp链表，file_stat->refault、hot、free
 * 上的file_area只会移动到file_stat->warmm链表。为什么要这样？因为要减少使用spin_lock(&p_file_stat->file_stat_lock)加锁：
 * 读写文件的进程向xarray tree增加page而执行__filemap_add_folio()函数时，要先执行spin_lock(&p_file_stat->file_stat_lock)加锁，
 * 然后向file_stat->temp链表添加为新的page分配的file_area。异步内存回收线程里，如果将file_stat->refault、hot、free链表频繁
 * 移动到file_stat->temp链表，二者将发生锁竞争，导致__filemap_add_folio()会频繁因spin_lock(&p_file_stat->file_stat_lock)
 * 锁竞争而耗时长，得不偿失。现在只有file_stat->warm上的file_area才会移动到file_stat->temp链表，并且
 * file_stat_warm_list_file_area_solve()函数经过算法优化，可以做到异步内存回收线程每次运行时：先把file_stat->warm链表上
 * 符合条件的file_area先移动到临时链表file_area_move_to_temp_list，然后只用
 * spin_lock(&p_file_stat->file_stat_lock)加锁一次，就可以把这些file_area移动到file_stat->temp链表，大大减少了加锁次数。
 * */

/* 遍历file_stat->warm链表上的file_area，长时间没访问的移动到file_stat->temp链表*/
static noinline unsigned int file_stat_warm_list_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,unsigned int scan_file_area_max,struct list_head *file_area_free_temp)
{
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int scan_file_area_count = 0;
	unsigned int file_area_age_dx;
	LIST_HEAD(file_area_move_to_temp_list);
	unsigned int scan_cold_file_area_count = 0;
	unsigned int scan_read_file_area_count = 0;
	unsigned int scan_ahead_file_area_count = 0;
	unsigned int warm_to_temp_file_area_count = 0;
	unsigned int warm_to_hot_file_area_count = 0;

	//从链表尾开始遍历，链表尾的成员更老，链表头的成员是最新添加的
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat->file_area_warm,file_area_list){
		if(++ scan_file_area_count > scan_file_area_max)
			break;

		file_area_age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;
		/*有热file_area标记*/
		if(file_area_in_hot_list(p_file_area)){
			//spin_lock(&p_file_stat->file_stat_lock);
			/* 当file_area被判定为hot后，没有清理in_temp_list状态，因此第一次来这里，没有in_temp_list则crash，
			 * 防止重复把file_area移动到file_stat->hot链表.注意，该file_area可能在update函数被并发设置了in_hot标记*/
			if(!file_area_in_warm_list(p_file_area) || (file_area_in_warm_list_error(p_file_area) && !file_area_in_hot_list(p_file_area)))
			    panic("%s file_area:0x%llx status:%d not in file_area_temp error\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			clear_file_area_in_warm_list(p_file_area);
			list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
			warm_to_hot_file_area_count ++;
			//spin_unlock(&p_file_stat->file_stat_lock);
			continue;
		}
		/*file_stat->warm链表上的file_area在file_area_warm_to_temp_age_dx个周期里被访问过，则移动到file_stat->temp链表。在
		 * file_temp链表上的file_area享有在hot_file_update()函数随着访问次数增多移动到file_stat->temp链表头的权利。
		 *注意，这里是唯一一次file_area_age_dx使用 小于 */
		else if(file_area_age_dx < hot_cold_file_global_info.file_area_warm_to_temp_age_dx){
			//每遍历到一个就加一次锁，浪费性能，可以先移动到一个临时链表上，循环结束后加一次锁，然后把这些file_area或file_stat移动到目标链表???????
			//spin_lock(&p_file_stat->file_stat_lock);

			clear_file_area_in_warm_list(p_file_area);
			/* 这里file_stat->warm链表上的file_area移动到file_area_list临时链表时，不能设置file_area的in_temp_list状态。
			 * 否则，hot_file_update_file_status()函数里，检测到file_area的in_temp_list状态，会把file_area移动到
			 * file_stat->temp链表头，那就状态错乱了！因此此时file_area还在file_area_list临时链表或file_stat->warm
			 * 链表，并且没有spin_lock(&p_file_stat->file_stat_lock)加锁。因此这里不能设置file_area的in_temp_list状态，
			 * 要设置也得下边spin_lock(&p_file_stat->file_stat_lock)加锁后再设置。这个并发问题隐藏的很深!!!!!!!!!!!!!!!!!!*/
			//set_file_area_in_temp_list(p_file_area);

			/*先把符合条件的file_area移动到临时链表，下边再把这些file_area统一移动到file_stat->temp链表*/
			list_move(&p_file_area->file_area_list,&file_area_move_to_temp_list);
			p_file_stat->file_area_count_in_temp_list ++;
			//spin_unlock(&p_file_stat->file_stat_lock);
			warm_to_temp_file_area_count ++;
		}
		/*否则file_stat->warm链表上长时间没访问的file_area移动的file_area移动到file_area_free_temp链表，参与内存回收，移动过程不用加锁*/
		else if(file_area_age_dx > p_hot_cold_file_global->file_area_temp_to_cold_age_dx){
			/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
			if(file_area_in_ahead(p_file_area)){
				clear_file_area_in_ahead(p_file_area);
				scan_ahead_file_area_count ++;
				/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
				if(IS_MEMORY_ENOUGH(p_hot_cold_file_global))
					continue;
			}

			/* 当前内存不紧张且内存回收不困难，则遇到read文件页的file_area先跳过，不回收。目的是尽可能回收write文件页，
			 * read文件页一旦回收再被访问就会发生refault，不仅导致read耗时长，读磁盘功耗还高。但是如果这个read属性的
			 * file_area很长很长很长时间没访问，也参与内存回收*/
			if(file_area_page_is_read(p_file_area)){
				scan_read_file_area_count ++;
				if(!(IS_IN_MEMORY_PRESSURE_RECLAIM(p_hot_cold_file_global) || file_area_age_dx > p_hot_cold_file_global->file_area_reclaim_read_age_dx))
					continue;
				clear_file_area_page_read(p_file_area);
			}

			//access_count清0，如果内存回收期间又被访问了，access_count将大于0，将被判断为refault page。
			file_area_access_count_clear(p_file_area);
			scan_cold_file_area_count ++;
			clear_file_area_in_warm_list(p_file_area);
			set_file_area_in_free_list(p_file_area);
			list_move(&p_file_area->file_area_list,file_area_free_temp);
		}
	}

	/* p_file_stat->file_area_warm链表非空且p_file_area不是指向链表头且p_file_area不是该链表的第一个成员，
	 * 则执行list_move_enhance()把本次遍历过的file_area~链表尾的file_area移动到链表
	 * p_file_stat->file_area_warm头，这样下次从链表尾遍历的是新的未遍历过的file_area。
	 * 当上边循环是遍历完所有链表所有成员才退出，p_file_area此时指向的是链表头p_file_stat->file_area_warm，
	 * 此时 p_file_area->file_area_list 跟 &p_file_stat->file_area_warm 就相等了。当然，这种情况，是绝对不能
	 * 把p_file_area到链表尾的file_area移动到p_file_stat->file_area_warm链表头了，会出严重内核bug*/
	//if(!list_empty(p_file_stat->file_area_warm) && p_file_area->file_area_list != &p_file_stat->file_area_warm && p_file_area->file_area_list != p_file_stat->file_area_warm.next)
	{
		/*将链表尾已经遍历过的file_area移动到file_stat->warm链表头，下次从链表尾遍历的才是新的未遍历过的file_area。此时不用加锁!!!!!!!!!!!!!*/

		/*有一个重大隐患bug，如果上边的for循环是break跳出，则p_file_area可能就不在file_stat->warm链表了，
		 *此时再把p_file_area到p_file_stat->file_area_warm链表尾的file_area移动到p_file_stat->file_area_warm
		 *链表，file_area就处于错误的file_stat链表了，大bug!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		 *怎么解决，先执行 can_file_area_move_to_list_head()函数判定file_area是否处于file_stat->warm链表，
		 *是的话才允许把p_file_area到链表尾的file_area移动到链表头*/
		if(can_file_area_move_to_list_head(p_file_area,&p_file_stat->file_area_warm,F_file_area_in_warm_list))
			list_move_enhance(&p_file_stat->file_area_warm,&p_file_area->file_area_list);
	}

	/*将file_stat->warm链表上近期访问过的file_area移动到file_stat->temp链表头*/
	if(!list_empty(&file_area_move_to_temp_list)){
		struct file_area *p_file_area_last = list_last_entry(&file_area_move_to_temp_list,struct file_area,file_area_list);

		spin_lock(&p_file_stat->file_stat_lock);
		/* 要先加锁后，再设置file_area的in_temp_list状态，再list_splice()把file_area移动到file_stat->temp链表头，
		 * 防止hot_file_update_file_status()函数中并发移动这些有in_temp_list标记的file_area到file_stat->temp链表头。
		 * 但是考虑到下边list_for_each_entry()循环设置file_area的in_temp_list状态，耗时，于是考虑先加锁把
		 * file_area移动到file_stat->temp链表头。然后释放锁后，再执行list_for_each_entry()置file_area的in_temp_list
		 * 状态。这样不会有并发问题，因为加锁+list_splice()后file_area已经在file_stat->temp链表了。然后释放锁，
		 * 再设置file_area的in_temp_list状态，此时不会有并发的问题了*/
#if 0	
		list_for_each_entry(p_file_area,file_area_list,file_area_list){
			set_file_area_in_temp_list(p_file_area); 
		}
#endif		
		list_splice(&file_area_move_to_temp_list,&p_file_stat->file_area_temp);
		spin_unlock(&p_file_stat->file_stat_lock);

		/* file_area_move_to_head_count清0，并且smp_wmb()，synchronize_rcu()。之后，进程读写文件，先执行了smp_rmb()，
		 * 再执行到hot_file_update_file_status()，看到file_area_move_to_head_count是0，或者小于0，就不会再把
		 * p_file_stat->file_area_temp链表上的file_area移动到链表头了。为什么 file_area_move_to_head_count可能小于0，
		 * 因为可能这里对file_area_move_to_head_count赋值0，hot_file_update_file_status()并发执行
		 * file_area_move_to_head_count减1，就可能小于0了*/
		p_file_stat->file_area_move_to_head_count = 0;
		/* 即便新的age周期，因为file_area_move_to_head_count_max是0，赋值给file_area_move_to_head_count还是0*/
		hot_cold_file_global_info.file_area_move_to_head_count_max = 0;
		smp_wmb();
		/* 为什么要再加个 synchronize_rcu()，这是保证所有在 hot_file_update_file_status()函数的进程全部退出.
		 * 然后下次他们再读写文件，比如执行mapping_get_entry->smp_rmb->hot_file_update_file_status()，
		 * 都先执行了smp_rmb()，保证都看到list_splice(file_area_list,&p_file_stat->file_area_temp)移动的
		 * 这些file_area真的生效，因为list_splice(file_area_list,&p_file_stat->file_area_temp)后执行了
		 * smp_wmb()。ok，后续这里再set_file_area_in_temp_list(p_file_area)。这样就能绝对保证，
		 * 读写文件的进程，一定先看到这里file_area是先移动到p_file_stat->file_area_temp链表，
		 * 然后再看到这里设置file_area的in_temp_list状态。如果先看到这里设置file_area的in_temp_list状态，
		 * 而 后看到file_area移动到file_stat->temp链表，就会错误把还在file_stat->warm链表的多次访问的file_area移动
		 * 到file_stat->temp链表头*/
		synchronize_rcu();


		/* 这里有个重大bug，上边list_splice()后，原file_area_move_to_temp_list链表的file_area都移动到p_file_stat->file_area_temp
		 * 链表了。这里list_for_each_entry再从file_area_move_to_temp_list指向的第一个file_area开始遍历，就有大问题了。因为实际
		 * 遍历的这些file_area已经处于链表头p_file_stat->file_area_temp引领的链表了。这样list_for_each_entry就会陷入死循环，因为
		 * 遍历到的链表头是p_file_stat->file_area_temp,不是file_area_move_to_temp_list。怎么解决这个并发问题？上边list_splice
		 * 把file_area移动到p_file_stat->file_area_temp链表头后，把p_file_stat->file_area_move_to_head_count = 0清0。然后smp_wmb()，
		 * 然后再 synchronize_rcu()。这样就能保证所有在 hot_file_update_file_status()函数的进程全部退出.
		 * 然后下次他们再读写文件，比如执行mapping_get_entry->smp_rmb->hot_file_update_file_status()，都先执行了smp_rmb()，
		 * 保证都看到 p_file_stat->file_area_move_to_head_count是0。这样写该文件执行到 hot_file_update_file_status()函数，
		 * 因为p_file_stat->file_area_move_to_head_count是0，就不会再list_move把这些file_area移动到p_file_stat->file_area_temp
		 * 链表头了。下边list_for_each_entry()通过file_area_move_to_temp_list链表头遍历这些file_area，不用担心这些file_area被
		 * hot_file_update_file_status()函数中移动到p_file_stat->file_area_temp链表头，打乱这些file_area的先后顺序。
		 * 简单说，这些file_area原来在file_area_move_to_temp_list链表的先后顺序。在这些file_area移动到
		 * p_file_stat->file_area_temp链表后，在执行list_for_each_entry过程，这些file_area
		 * 在p_file_stat->file_area_temp链表也要保持同样的先后顺序。接着，等遍历到原file_area_move_to_temp_list链表指向的最后
		 * file_area，即p_file_area_last，遍历结束。
		 *
		 * 为什么list_for_each_entry()遍历原file_area_move_to_temp_list链表的现在在p_file_stat->file_area_temp链表的
		 * file_area时，要保持这些file_area的先后顺序？主要为了将来考虑，现在没事，因为现在list_for_each_entry()里
		 * 是先遍历靠前的file_area，然后设置这些file_area的in_temp状态。然后 hot_file_update_file_status()里检测到
		 * in_temp状态的file_area，才会把file_area移动到file_stat->file_area_temp链表头。此时该file_area在链表后边的
		 * file_area没有设置in_temp状态，不用担心被hot_file_update_file_status()移动到file_stat->file_area_temp链表
		 * 头。这个设计主要是为了将来考虑，代码改来改去，忘了，出现疏漏，会把这些file_area随机移动到
		 * p_file_stat->file_area_temp链表任意位置，出现不可预料的问题。
		 * 故还是这里设置file_area_move_to_head_count=0等等，绝对禁止hot_file_update_file_status()函数里把任何file_area
		 * 移动到p_file_stat->file_area_temp链表头
		 *
		 * 完美，说到底，这样实现了一个抑制list_move的无锁编程。这个方法真的完美吗？no，突发一个想法，涌上心头，如果
		 * list_for_each_entry()过程新的age周期到来，那hot_file_update_file_status()函数就会对
		 * p_file_stat->file_area_move_to_head_count 赋值hot_cold_file_global_info.file_area_move_to_head_count_max ，默认16 。
		 * 这样p_file_stat->file_area_move_to_head_count就大于0了，hot_file_update_file_status()函数就可能把这些file_area
		 * 移动到p_file_stat->file_area_temp链表头了，又打乱了这些原file_area_move_to_temp_list链表的file_area的先后排列
		 * 顺序。这个bug隐藏很深!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		 * 解决方法很简单，上边p_file_stat->file_area_move_to_head_count = 0清0时，
		 * hot_cold_file_global_info.file_area_move_to_head_count_max也清0，即便hot_file_update_file_status()里新的age周期
		 * 对p_file_stat->file_area_move_to_head_count赋值，也是0，hot_file_update_file_status()里就不可能
		 * 把这些file_area移动到p_file_stat->file_area_temp链表头了
		 *
		 * 读写该文件执行到 hot_file_update_file_status()函数，因为p_file_stat->file_area_move_to_head_count是0*/
		list_for_each_entry(p_file_area,&file_area_move_to_temp_list,file_area_list){
			/*正常遍历原file_area_move_to_temp_list指向的链表头的file_area，肯定是不会有in_temnp链表属性的。*/
			if(file_area_in_temp_list(p_file_area))
				panic("%s file_stat:0x%llx  status:0x%lx has in temp_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

			set_file_area_in_temp_list(p_file_area);
			/* 如果遍历到原file_area_move_to_temp_list链表最后一个file_area，说明这些file_area都设置过in_temp链表属性了
			 * 要立即break*/
			if(p_file_area == p_file_area_last)
				break;
		}
		barrier();
		/*list_for_each_entry遍历结束后，才能恢复p_file_stat->file_area_move_to_head_count大于0.*/
		p_file_stat->file_area_move_to_head_count = FILE_AREA_MOVE_TO_HEAD_COUNT_MAX;
		hot_cold_file_global_info.file_area_move_to_head_count_max = FILE_AREA_MOVE_TO_HEAD_COUNT_MAX;
	}
	/*参与内存回收的冷file_area个数*/
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_cold_file_area_count_from_warm += scan_cold_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_read_file_area_count_from_warm += scan_read_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_ahead_file_area_count_from_warm += scan_ahead_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_file_area_count_from_warm += scan_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.warm_to_temp_file_area_count += warm_to_temp_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.warm_to_hot_file_area_count += warm_to_hot_file_area_count;

	return scan_file_area_count;
}
/* mmap文件内存回收遇到时，发现file_area里的page至少一个page发现是cache page的，则该file_area移动到
 * file_area_have_cache_page_head链表，现在遍历该链表回收这些cache文件页*/
int mmap_file_area_cache_page_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *file_area_have_cache_page_head,struct list_head *file_area_free_temp,char file_type)
{
	struct file_area *p_file_area,*p_file_area_temp;
	int scan_cold_file_area_count = 0,scan_read_file_area_count = 0,file_area_age_dx;
	unsigned int isolate_lru_pages,free_pages;
	//unsigned int scan_file_area_count = 0;
	//LIST_HEAD(file_area_free_temp);

	list_for_each_entry_safe(p_file_area,p_file_area_temp,file_area_have_cache_page_head,file_area_list){
		/*file_area_have_cache_page_head链表上的file_area必须处于in_free状态*/
		if(!file_area_in_free_list(p_file_area) || file_area_in_free_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
        /*不用限制scan的最大file_area个数，因为单个mmap文件scan的最大file_area数是有限制的*/
		//if(++ scan_file_area_count > 512)
		//	break;

		file_area_age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;
		if(file_area_age_dx > p_hot_cold_file_global->file_area_temp_to_cold_age_dx){
			/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
			if(file_area_in_ahead(p_file_area)){
				clear_file_area_in_ahead(p_file_area);
				/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
				if(IS_MEMORY_ENOUGH(p_hot_cold_file_global))
					continue;
			}

			/* 当前内存不紧张且内存回收不困难，则遇到read文件页的file_area先跳过，不回收。目的是尽可能回收write文件页，
			 * read文件页一旦回收再被访问就会发生refault，不仅导致read耗时长，读磁盘功耗还高。但是如果这个read属性的
			 * file_area很长很长很长时间没访问，也参与内存回收*/
			if(file_area_page_is_read(p_file_area)){
				scan_read_file_area_count ++;
				if(!(IS_IN_MEMORY_EMERGENCY_RECLAIM(p_hot_cold_file_global) || file_area_age_dx > p_hot_cold_file_global->file_area_reclaim_read_age_dx))
					continue;
				clear_file_area_page_read(p_file_area);
			}

			scan_cold_file_area_count ++;
			//set_file_area_in_free_list(p_file_area); file_area_have_cache_page_head链表上的file_area必须处于in_free状态，不再重复设置
			//list_move(&p_file_area->file_area_list,&file_area_free_temp);
			list_move(&p_file_area->file_area_list,file_area_free_temp);
		}
	}

	free_pages = p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;
	/*回收这些mmap文件但包含cache文件的file_area的page*/
	isolate_lru_pages = cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,file_area_free_temp,NULL);
	free_pages = p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count - free_pages;

	/* 重大隐藏bug!!!!!!!，file_area_free_temp链表上的file_area参与内存回收后，还得要把file_area再移动回file_stat->free链表，
	 * 然后看后续file_area是否被访问而决定是否移动到file_stat->refault链表。处理措施是，把局部变量file_area_free_temp改为
	 * 由该函数传入file_area_free_temp，而该函数执行后，再把file_area_free_temp链表上的file_area移动到file_stat->free链表*/

	/*无数次的实践证实，凡事凭着感觉写出的代码都有出错的可能。解决办法是：一行一行分析每行代码的所用，把函数的每一种执行
	 *情况都列举出来，就能发现隐藏的bug了*/

	if(shrink_page_printk_open1)
		printk("%s %s %d p_hot_cold_file_global:0x%llx p_file_stat:0x%llx status:0x%lx free_pages:%d\n",__func__,current->comm,current->pid,(u64)p_hot_cold_file_global,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,free_pages);

	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_cold_file_area_count_from_mmap_file = scan_cold_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.isolate_lru_pages_from_mmap_file += isolate_lru_pages;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_from_mmap_file += free_pages;
	return free_pages;
}
/* 遍历global hot链表上的file_stat，再遍历这些file_stat->hot链表上的file_area，如果不再是热的，则把file_area
 * 移动到file_stat->warm链表。如果file_stat->hot链表上的热file_area个数减少到热文件阀值以下，则降级到
 * global temp、middle_file、large_file链表*/
static noinline int hot_file_stat_solve(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max)
{
	struct file_stat *p_file_stat,*p_file_stat_temp;
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int file_stat_type,scan_file_area_count = 0;
	unsigned int file_area_age_dx;
	unsigned int file_area_hot_to_warm_list_count = 0;

	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->file_stat_hot_head,hot_cold_file_list){
		if(!file_stat_in_file_stat_hot_head_list(p_file_stat) || file_stat_in_file_stat_hot_head_list_error(p_file_stat))
			panic("%s file_stat:0x%llx not int file_stat_hot_head_list status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		if(scan_file_area_count > scan_file_area_max)
			break;
		
		if(p_file_stat->recent_traverse_age < p_hot_cold_file_global->global_age)
			p_file_stat->recent_traverse_age = p_hot_cold_file_global->global_age;

		/*遍历global->file_stat_hot_head上的热文件file_stat的file_area_hot链表上的热file_area，如果哪些file_area不再被访问了，则要把
		 *file_area移动回file_stat->file_area_warm链表。同时令改文件的热file_area个数file_stat->file_area_hot_count减1*/
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat->file_area_hot,file_area_list){

			scan_file_area_count ++;
			//file_stat->file_area_hot尾巴上长时间未被访问的file_area再降级移动回file_stat->file_area_warm链表头
			file_area_age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;

			if(file_area_age_dx > p_hot_cold_file_global->file_area_hot_to_temp_age_dx){
				if(!file_area_in_hot_list(p_file_area) || file_area_in_hot_list_error(p_file_area))
					panic("%s file_area:0x%llx status:0x%x not in file_area_hot\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
				/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
				if(file_area_in_ahead(p_file_area)){
					clear_file_area_in_ahead(p_file_area);
					/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
					if(IS_MEMORY_ENOUGH(p_hot_cold_file_global))
						continue;
				}
				file_area_hot_to_warm_list_count ++;
				/*现在把file_area移动到file_stat->warm链表，不再移动到file_stat->temp链表，因此不用再加锁*/
				//spin_lock(&p_file_stat->file_stat_lock);
				p_file_stat->file_area_hot_count --;
				clear_file_area_in_hot_list(p_file_area);
				set_file_area_in_warm_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
				//spin_unlock(&p_file_stat->file_stat_lock);	    
			}
		}

		/*重点：将链表尾已经遍历过的file_area移动到file_stat->hot链表头，下次从链表尾遍历的才是新的未遍历过的file_area。此时不用加锁!!!!!!!!!!!!!*/
		if(can_file_area_move_to_list_head(p_file_area,&p_file_stat->file_area_hot,F_file_area_in_hot_list))
		    list_move_enhance(&p_file_stat->file_area_hot,&p_file_area->file_area_list);

		/*该文件file_stat的热file_area个数file_stat->file_area_hot_count小于阀值，则被判定不再是热文件
		  然后file_stat就要移动回hot_cold_file_global->file_stat_temp_head 或 hot_cold_file_global->file_stat_temp_large_file_head链表*/
		if(!is_file_stat_hot_file(p_hot_cold_file_global,p_file_stat)){

			/*热文件降级不再加global lock，因为只会把热file_stat降级到global middle_file、large_file 链表*/
			spin_lock(&p_hot_cold_file_global->global_lock);
			if(!file_stat_in_delete(p_file_stat)){
				hot_cold_file_global_info.file_stat_hot_count --;//热文件数减1
				clear_file_stat_in_file_stat_hot_head_list(p_file_stat);

				/* 为了不加锁，即便文件是普通文件也是移动到global middle_file链表。错了，NO!!!!!!!!!!!!!!
				 * 错了，因为一共有两种并发：
				 * 普通文件升级到中型文件，必须要加锁。要考虑两种并发，都会并发修改global temp链表头。
				 * 1：读写文件进程执行__filemap_add_folio()向global temp添加file_stat到global temp链表头。如果只有
				 * 这行并发，file_stat不移动到global temp链表就不用global lock加锁。但还有一种并发，iput()释放inode
				 * 2：iput()释放inode并标记file_stat的delete，然后把file_stat从global任一个链表移动到global delete链表。
				 * 此时的file_stat可能处于global temp、hot、large、middle、zero链表。因此要防护这个file_stat被iput()
				 * 并发标记file_stat delete并把file_stat移动到global delete链表。
				 *
				 * 做个总结：凡是file_stat在global temp、hot、large、middle、zero链表之间相互移动，都必须要
				 * global lock加锁，然后判断file_stat是否被iput()释放inode并标记delete!!!!!!!!!!!!!!!!!!!
				 */
				file_stat_type = is_file_stat_file_type(p_hot_cold_file_global,p_file_stat);
				if(TEMP_FILE == file_stat_type){
					set_file_stat_in_file_stat_temp_head_list(p_file_stat);
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_temp_head);
				}else if(MIDDLE_FILE == file_stat_type){
					set_file_stat_in_file_stat_middle_file_head_list(p_file_stat);
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_middle_file_head);
				}
				else{
					set_file_stat_in_file_stat_large_file_head_list(p_file_stat);
					p_hot_cold_file_global->file_stat_large_count ++;
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_large_file_head);
				}
			}
			spin_unlock(&p_hot_cold_file_global->global_lock);
		}
	}

	/*重点：将链表尾已经遍历过的file_stat移动到p_hot_cold_file_global->file_stat_hot_head链表头。
	 *下次从链表尾遍历的才是新的未遍历过的file_stat。此时不用加锁!!!!!!!!!真的不用加锁吗????????????
	 *大错特错，如果p_file_stat此时正好被iput()释放，标记file_stat delete并移动到global delete链表，
	 *这里却把p_file_stat又移动到了global hot链表，那就出大问题了。因此，这里不用加锁防护global temp
	 *链表file_stat的增加与删除，但是要防护iput()把该file_stat并发移动到global delete链表。方法很简单，
	 *加锁后p_file_stat不是链表头，且没有delete标记即可。详细原因get_file_area_from_file_stat_list()有解释
	 !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
	spin_lock(&p_hot_cold_file_global->global_lock);
	if(&p_file_stat->hot_cold_file_list != &p_hot_cold_file_global->file_stat_hot_head && !file_stat_in_delete(p_file_stat)){
		if(can_file_stat_move_to_list_head(p_file_stat,F_file_stat_in_file_stat_hot_head_list))
			list_move_enhance(&p_hot_cold_file_global->file_stat_hot_head,&p_file_stat->hot_cold_file_list);
	}
	spin_unlock(&p_hot_cold_file_global->global_lock);

	//file_stat的hot链表转移到temp链表的file_area个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_hot_to_warm_from_hot_file = file_area_hot_to_warm_list_count;
	return scan_file_area_count;
}
/*把在global file_stat_temp_head链表但实际是mmap的文件file_stat从global file_stat_temp_head链表剔除，然后添加到global mmap_file_stat_temp_head链表*/
static noinline unsigned int cache_file_stat_move_to_mmap_head(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat_base,char file_stat_type)
{

	if(FILE_STAT_NORMAL == file_stat_type){
		struct file_stat *p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

		if(!list_empty(&p_file_stat->file_area_hot) || !list_empty(&p_file_stat->file_area_refault) || !list_empty(&p_file_stat->file_area_free) ||
				!list_empty(&p_file_stat->file_area_warm) /*|| !list_empty(&p_file_stat->file_area_free_temp)*/)/*cache file不使用file_stat->free_temp链表*/
			panic("%s file_stat:0x%llx list empty error\n",__func__,(u64)p_file_stat);
	}

	spin_lock(&p_hot_cold_file_global->global_lock);
	/*file_stat可能被并发iput释放掉*/
	if(!file_stat_in_delete(p_file_stat_base)){
		list_del(&p_file_stat_base->hot_cold_file_list);
	}
	clear_file_stat_in_cache_file(p_file_stat_base);
	spin_unlock(&p_hot_cold_file_global->global_lock);

	/* 清理掉file_stat的in_cache状态，然后file_stat的in_cache_file和in_mmap_file状态都没有。然后执行
	 * synchronize_rcu()等待所有正在hot_file_update_file_status()函数中"设置file_stat的任何状态，移动
	 * file_area到任何file_stat的链表"的进程退出rcu宽限期，也就是hot_file_update_file_status()函数。
	 * 因为这些进程文件读写的流程是
	 * filemap_read->filemap_get_pages->filemap_get_read_batch
	 *
	 * rcu_read_lock
	 * hot_file_update_file_status()
	 * rcu_read_unlock
	 *
	 * synchronize_rcu()执行后，进程执行到hot_file_update_file_status()检测到该file_stat in_cache_file和in_mmap_file状态
	 * 都没有，更新age后直接返回，不再设置file_stat的任何状态，不再移动file_area到任何file_stat的链表。
	 *
	 * 但是新的问题来了，看iput()释放文件最后执行__destroy_inode_handler_post()函数，是要基于文件file_stat是in_cache_file
	 * 还是in_mmap_file来决定把文件释放后file_stat移动到global delete或global mmap_file_delete链表。现在file_stat
	 * in_cache_file和in_mmap_file状态都没有了，iput()释放该文件要把file_stat移动到哪个delete链表??????
	 * 最后决定在__destroy_inode_handler_post()函数处理，if(in_cache_file) {move_to global delete_list} 
	 * else{move_to global_mmap_file_delete_list}
	 * */
	synchronize_rcu();

	spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
	set_file_stat_in_mmap_file(p_file_stat_base);
	set_file_stat_in_from_cache_file(p_file_stat_base);

	/*file_stat可能被并发iput释放掉*/
	if(!file_stat_in_delete(p_file_stat_base)){
		if(FILE_STAT_SMALL == file_stat_type)
		    list_add(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_small_file_head);
		else
		    list_add(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
	}
	spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

	return 0;
}
/*回收file_area->free_temp链表上的冷file_area的page，回收后的file_area移动到file_stat->free链表头*/
static noinline unsigned int free_page_from_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *file_area_free_temp,char file_stat_type)
{
	unsigned int free_pages = 0;
	unsigned int isolate_lru_pages = 0;
	LIST_HEAD(file_area_have_mmap_page_head);

	/*释放file_stat->file_area_free_temp链表上冷file_area的page，如果遇到有mmap文件页的file_area，则会保存到file_area_have_mmap_page_head链表*/
	//isolate_lru_pages += cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_free_temp);
	isolate_lru_pages = cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,file_area_free_temp,&file_area_have_mmap_page_head);
	free_pages = p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;/*free_pages_count本身是个累加值*/

	/*遍历file_area_have_mmap_page_head链表上的含有mmap文件页的file_area，然后回收这些file_area上的mmap文件页*/
	if(!list_empty(&file_area_have_mmap_page_head)){
		cache_file_area_mmap_page_solve(p_hot_cold_file_global,p_file_stat_base,&file_area_have_mmap_page_head,file_stat_type);
		/* 参与内存回收后的file_area再移动回file_area_free_temp链表，回归正常流程。将来这些file_area都会移动回file_stat->free链表，
		 * 如果内存回收失败则file_area会移动回file_stat->refault链表。总之，这些包含mmap page的file_area跟正常的cache文件file_area
		 * 内存回收处理流程都一样*/
		list_splice(&file_area_have_mmap_page_head,file_area_free_temp);
	}

	if(shrink_page_printk_open1)
		printk("1:%s %s %d p_hot_cold_file_global:0x%llx p_file_stat:0x%llx status:0x%lx free_pages:%d\n",__func__,current->comm,current->pid,(u64)p_hot_cold_file_global,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,free_pages);

	/*注意，file_stat->file_area_free_temp 和 file_stat->file_area_free 各有用处。file_area_free_temp保存每次扫描释放的
	 *page的file_area。释放后把这些file_area移动到file_area_free链表，file_area_free保存的是每轮扫描释放page的所有file_area。
	 p_file_stat->file_area_free链表上的file_area结构要长时间也没被访问就释放掉*/

	/*新的版本只有移动file_stat->temp链表上的file_area才得加锁，其他链表之间的移动不用加锁*/
	//list_splice(&p_file_stat->file_area_free_temp,&p_file_stat->file_area_free);
	if(FILE_STAT_NORMAL == file_stat_type){
		struct file_stat *p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
	    list_splice(file_area_free_temp,&p_file_stat->file_area_free);
	}
	/*list_splice把前者的链表成员a1...an移动到后者链表，并不会清空前者链表。必须INIT_LIST_HEAD清空前者链表，否则它一直指向之前的
	 *链表成员a1...an。后续再向该链表添加新成员b1...bn。这个链表就指向的成员就有a1...an + b1...+bn。而此时a1...an已经移动到了后者
	 *链表，相当于前者和后者链表都指向了a1...an成员，这样肯定会出问题.之前get_file_area_from_file_stat_list()函数报错
	 *"list_add corruption. next->prev should be prev"而crash估计就是这个原因!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 */

	/*现在不用p_file_stat->file_area_free_temp这个全局链表了，故list_splice()后不用再初始化该链表头*/
	//INIT_LIST_HEAD(&p_file_stat->file_area_free_temp);
	//INIT_LIST_HEAD(&file_area_free_temp);

	//隔离的page个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.isolate_lru_pages += isolate_lru_pages;

	return free_pages;
}
/*
 * 该函数的作用
 * 1：从global temp、large_file、middle_file链表尾遍历N个file_stat，这个过程不global lock加锁。这也是这个新版本的最大改动!
 * 因为__filemap_add_folio()向global temp链表添加新的file_stat也要global lock加锁，会造成抢占global lock锁失败而延迟。
 * 但也不是一直不加锁。只有以下几种情况才会global lock加锁
 * 1.1 遍历到的file_stat文件状态发生变化，比如小文件、中文件、大文件、小/中/大热文件 状态变了，file_stat要list_move()
 * 到对应的global temp、middle、large、hot 链表上。此时就需要global lock加锁。
 * 1.2 在遍历global temp和large_file_temp链表尾遍历N个file_stat结束后，把遍历过的N个file_stat移动到链表头，
 *     此时需要global lock加锁
 * 1.3 file_area 0个file_area的话则移动到global file_stat_zero_file_area_head链表
 *
 * 2：对遍历到的N个file_stat的处理
 * 2.1：依次遍历file_stat->temp链表尾的N个file_area，如果是冷的则把file_area移动到
 *      file_stat->free_temp链表， 遍历结束后则统一回收file_stat->free_temp链表上冷file_area的冷page。然后把
 *      这些file_area移动到file_stat->free链表头。
 *
 *      如果遍历到的file_stat->temp链表尾的N个file_area中，遇到热的file_area则加锁移动到file_stat->hot链表。
 *      遇到不冷不热的file_area则移动到file_stat->warm链表。遇到最近访问过的file_area则移动
 *      到file_stat->temp链表头，这样下次才能从file_stat->temp链表尾遍历到新的未曾遍历过的file_area
 *
 * 2.2 依次遍历file_stat->hot和refault 链表尾的少量的N个file_area，如果长时间未访问则降级移动到file_stat->temp
 *     链表头。否则，把遍历到的N个file_area再移动到file_stat->hot和refault 链表头，保证下次从链表尾遍历
 *     到的file_area是新的未遍历过的file_area.
 * 2.3 依次遍历file_stat->free 链表尾的N个file_area，如果长时间没访问则释放掉file_area结构。否则把遍历到的
 *     file_area移动到file_stat->free 链表头，保证下次从链表尾遍历到的file_area是新的未遍历过的file_area。
 *     遇到有refault标记的file_area则加锁移动到file_stat->refault链表。
 * */
static noinline unsigned int get_file_area_from_file_stat_list(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max,unsigned int scan_file_stat_max,struct list_head *file_stat_temp_head,unsigned char file_stat_list_type)
{
	//file_stat_temp_head来自 hot_cold_file_global->file_stat_temp_head、file_stat_temp_large_file_head、file_stat_temp_middle_file_head 链表

	struct file_stat *p_file_stat,*p_file_stat_temp;
	//struct file_area *p_file_area,*p_file_area_temp;
	LIST_HEAD(file_area_free_temp);

	unsigned int scan_file_area_count  = 0;
	unsigned int scan_move_to_mmap_head_file_stat_count  = 0;
	unsigned int scan_file_stat_count  = 0;
	unsigned int real_scan_file_stat_count  = 0;
	unsigned int scan_delete_file_stat_count = 0;
	unsigned int scan_cold_file_area_count = 0;
	unsigned int file_stat_in_list_type = -1;
	//unsigned int scan_fail_file_stat_count = 0;

	//unsigned int cold_file_area_for_file_stat = 0;
	unsigned int file_stat_count_in_cold_list = 0;

	/* 从global temp和large_file_temp链表尾遍历N个file_stat，回收冷file_area的。对热file_area、refault file_area、
	 * in_free file_area的各种处理。这个不global lock加锁。但是遇到file_stat移动到其他global 链表才会global lock加锁*/
	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,file_stat_temp_head,hot_cold_file_list){

		if(scan_file_area_count > scan_file_area_max || scan_file_stat_count ++ > scan_file_stat_max)
			break;
		
		if(p_file_stat->recent_traverse_age < p_hot_cold_file_global->global_age)
			p_file_stat->recent_traverse_age = p_hot_cold_file_global->global_age;

		/* 现在遍历global temp和large_file_temp链表上的file_stat不加global lock了，但是下边需要时刻注意可能
		 * 唯一可能存在的并发移动file_stat的情况：iput()释放file_stat，标记file_stat delete，并把file_stat移动到
		 * global delete链表。以下的操作需要特别注意这种情况!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/

		/* 到这里， file_stat可能被iput()并发释放file_stat，标记file_stat delete，但是不会清理file_stat in temp_head_list状态。
		 * 因此这个if不会成立*/
		if(FILE_STAT_IN_TEMP_FILE_LIST == file_stat_list_type){
			file_stat_in_list_type = F_file_stat_in_file_stat_temp_head_list;

			/*file_stat此时可能被方法iput()释放delete，但file_stat的delete的标记与file_stat in_temp_list 是共存的，不干扰file_stat in_temp_list的判断*/
			if(!file_stat_in_file_stat_temp_head_list(p_file_stat) || file_stat_in_file_stat_temp_head_list_error(p_file_stat))
				panic("%s file_stat:0x%llx not int file_stat_temp_head status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

			/* 把在global file_stat_temp_head链表但实际是mmap的文件file_stat从global file_stat_temp_head链表剔除，
			 * 然后添加到global mmap_file_stat_temp_head链表。注意，这里仅仅针对so库、可执行文件等elf文件，这些文件
			 * 都是先read系统调用读取文件头，被判定为cache文件，然后再mmap映射了。这种文件第一次扫描到，肯定处于global 
			 * temp链表，则把他们移动到global mmap_file_stat_temp_head链表。有些cache文件读写后，经历内存回收
			 * 在file_stat->warm、hot、free链表都已经有file_area了，就不再移动到global mmap_file_stat_temp_head链表了。
			 * 因此引入if(p_file_stat->file_area_count == p_file_stat->file_area_count_in_temp_list)这个判断，因为最初
			 * 二者肯定相等。当然，也有一种可能，就是过了很长时间，经历file_area内存回收、释放和再分配，二者还是相等。
			 * 无所谓了，就是把一个file_stat移动到global mmap_file_stat_temp_head而已，反正这个文件也是建立的mmap映射。
			 * 最后，再注意一点，只有在global temp_list的file_stat才能这样处理。*/
			if(mapping_mapped((struct address_space *)p_file_stat->mapping) && (p_file_stat->file_area_count == p_file_stat->file_area_count_in_temp_list)){
				scan_move_to_mmap_head_file_stat_count ++;
				cache_file_stat_move_to_mmap_head(p_hot_cold_file_global,&p_file_stat->file_stat_base,FILE_STAT_NORMAL);
				continue;
			}
		}
		else if(FILE_STAT_IN_MIDDLE_FILE_LIST == file_stat_list_type){
			file_stat_in_list_type = F_file_stat_in_file_stat_middle_file_head_list;

			if(!file_stat_in_file_stat_middle_file_head_list(p_file_stat) || file_stat_in_file_stat_middle_file_head_list_error(p_file_stat))
				panic("%s file_stat:0x%llx not int file_stat_middle_file_head status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
		}
		else{
			file_stat_in_list_type = F_file_stat_in_file_stat_large_file_head_list;

			if(FILE_STAT_IN_LARGE_FILE_LIST != file_stat_list_type)
				panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat,file_stat_list_type);

			if(!file_stat_in_file_stat_large_file_head_list(p_file_stat) || file_stat_in_file_stat_large_file_head_list_error(p_file_stat))
				panic("%s file_stat:0x%llx not int file_stat_large_file_head status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
		}

		/*一个mmapped文件不可能存在于global global temp和large_file_temp链表*/
		if(file_stat_in_mmap_file(p_file_stat)){
			panic("%s p_file_stat:0x%llx status:0x%lx in_mmap_file\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
		}

		/*可能iput最后直接把delete file_stat移动到global delete链表，并标记file_stat in delete状态*/
		if(file_stat_in_delete(p_file_stat)){
			scan_delete_file_stat_count ++;
			printk("%s file_stat:0x%llx delete status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			continue;
		}
		/* 如果file_stat的file_area全被释放了，则把file_stat移动到hot_cold_file_global->file_stat_zero_file_area_head链表。
		 * 但是唯一可能存在的并发移动file_stat的情况是iput()释放file_stat，标记file_stat delete，并把file_stat移动到
		 * global delete链表。因此要global_lock加锁后再判断一次file_stat是否被iput()把file_stat移动到global delete链表了*/
		else if(p_file_stat->file_area_count == 0){
			/*本来以为只有global temp链表上file_stat移动到global zero链表才需要加锁，因为会与读写文件进程
			 *执行__filemap_add_folio()向global temp添加file_stat并发修改global temp链表。但是漏了一个并发场景，
			 *文件inode释放会执行iput()标记file_stat的delete，然后把file_stat移动到global zero，对
			 *global temp、middle、large链表上file_stat都会这样操作，并发修改这些链表头。故这里
			 *global temp、middle、large链表上file_stat移动到global zero链表，都需要global lock加锁，因为iput()
			 *同时会并发修改*global temp、middle、large链表头，移动他们上边的file_stat到global zero链表!!!!!!!!!!!!!!!*/
			spin_lock(&p_hot_cold_file_global->global_lock);
			/*file_stat被iput()并发标记delete了，则不再理会*/
			if(!file_stat_in_delete(p_file_stat)){
				if(FILE_STAT_IN_TEMP_FILE_LIST == file_stat_list_type)
					clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
				else if(FILE_STAT_IN_MIDDLE_FILE_LIST == file_stat_list_type)
					clear_file_stat_in_file_stat_middle_file_head_list(p_file_stat);
				else{
					if(FILE_STAT_IN_LARGE_FILE_LIST != file_stat_list_type)
						panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat,file_stat_list_type);

					clear_file_stat_in_file_stat_large_file_head_list(p_file_stat);
					p_hot_cold_file_global->file_stat_large_count --;
				}

				set_file_stat_in_zero_file_area_list(p_file_stat);
				p_hot_cold_file_global->file_stat_count_zero_file_area ++;
				//如果该文件inode被释放了，则把对应file_stat移动到hot_cold_file_global->file_stat_zero_file_area_head链表
				list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_zero_file_area_head);
			}
			spin_unlock(&p_hot_cold_file_global->global_lock);

			continue;
		}

		/*file_area_free_temp现在不是file_stat->file_area_free_temp全局，而是一个临时链表，故每次向该链表添加file_area前必须要初始化*/
		INIT_LIST_HEAD(&file_area_free_temp);

		scan_file_area_max = 64;
		/*file_stat->temp链表上的file_area的处理，冷file_area且要内存回收的移动到file_area_free_temp临时链表，下边回收该链表上的file_area的page*/
		scan_file_area_count += file_stat_temp_list_file_area_solve(p_hot_cold_file_global,p_file_stat,scan_file_area_max,&file_area_free_temp);

		scan_file_area_max = 32;
		/*file_stat->warm链表上的file_area的处理，冷file_area且要内存回收的移动到file_area_free_temp临时链表，下边回收该链表上的file_area的page*/
		scan_file_area_count += file_stat_warm_list_file_area_solve(p_hot_cold_file_global,p_file_stat,scan_file_area_max,&file_area_free_temp);

		/*回收file_area_free_temp临时链表上的冷file_area的page，回收后的file_area移动到file_stat->free链表头*/
		free_page_from_file_area(p_hot_cold_file_global,&p_file_stat->file_stat_base,&file_area_free_temp,FILE_STAT_NORMAL);

		/*内存回收后，遍历file_stat->hot、refault、free链表上的各种file_area的处理*/
		scan_file_area_max = 16;
		scan_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_hot,scan_file_area_max,FILE_AREA_IN_HOT_LIST);
		scan_file_area_max = 16;
		scan_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_refault,scan_file_area_max,FILE_AREA_IN_REFAULT_LIST);
		scan_file_area_max = 64;
		scan_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_free,scan_file_area_max,FILE_AREA_IN_FREE_LIST);

		/*查看文件是否变成了热文件、大文件、普通文件，是的话则file_stat移动到对应global hot、large_file_temp、temp 链表*/
		file_stat_status_change_solve(p_hot_cold_file_global,p_file_stat,file_stat_list_type);
	}

	/* 这里有个重大隐藏bug，下边list_move_enhance()将p_file_stat~链表尾的遍历过的file_stat移动到file_stat_temp_head
	 * 链表头。如果这些file_stat被并发iput()释放，在__destroy_inode_handler_post()中标记file_stat delete，然后把
	 * file_stat移动到了global delete链表。接着这里，global_lock加锁后，再把这些file_stat移动到file_stat_temp_head
	 * 链表头，那就有问题了，相当于把delete的file_stat再移动到global temp、middel、large 链表，会有大问题。怎么解决？
	 * 还不想用锁。
	 *
	 * 想到的办法是：定义一个全局变量enable_move_file_stat_to_delete_list，正常是1。然后这里先把
	 * enable_move_file_stat_to_delete_list清0，然后synchronize_rcu()等待rcu宽限期退出。而__destroy_inode_handler_post()
	 * 中先rcu_read_lock，然后if(enable_move_file_stat_to_delete_list)才允许把file_stat
	 * list_move移动到global delete链表，但是允许标记file_stat的delete标记。
	 * 这样就是 运用rcu+变量控制的方法禁止链表list_move的方法：这里等从synchronize_rcu()
	 * 退出，__destroy_inode_handler_post()中就无法再把file_stat移动到global delete链表，但是会把file_stat标记delete。
	 * 后续异步内存线程遍历到有delete标记的file_stat，再把该file_stat移动到global delete链表即可。
	 *
	 * 这个方法完全可行，通过rcu+变量控制的方法禁止链表list_move。但是有点麻烦。其实仔细想想还有一个更简单的方法。
	 * 什么都不用动，只用下边spin_lock(&p_hot_cold_file_global->global_lock)后，判断一下p_file_stat是否有delete标记
	 * 即可。如果有delete标记，说明该file_stat被iput并发释放标记delete，并移动到global delete链表了。这里
	 * 只能取得该file_stat在file_stat_temp_head链表的下一个file_stat，然后判断这个新的file_stat是否有delete标记，
	 * 一直判断到链表头的file_stat，最极端的情况是这些原p_file_stat到链表尾的file_stat全都有delete标记，那
	 * list_move_enhance()就不用再移动链表了，直接返回。错了，分析错误了。
	 *
	 * spin_lock(&p_hot_cold_file_global->global_lock)加锁后，p_file_stat这file_stat确实可能被iput标记delete，并且
	 * 被移动到global delete链表，但是p_file_stat到链表尾之间的file_stat绝对不可能有delete标记。因为这些file_stat
	 * 一旦被iput()标记delete并移动到global delete链表，是全程spin_lock(&p_hot_cold_file_global->global_lock)加锁的，
	 * 然后这里spin_lock(&p_hot_cold_file_global->global_lock)加锁后，这些file_stat绝对保证已经移动到了global delete
	 * 链表，不会再存在于p_file_stat到链表尾之间。

	 * 如果不是原p_file_stat被标记delete，而是它到链表尾中间的某个file_stat被标记delete了，那会怎么办？没事，
	 * 因为spin_lock(&p_hot_cold_file_global->global_lock)加锁后，这个被标记delete的file_stat已经从file_stat_temp_head
	 * 链表移动到global delete链表了，已经不在原p_file_stat到链表尾之间了，那还担心什么。
	 *
	 * 但是又有一个问题，如果spin_lock(&p_hot_cold_file_global->global_lock)加锁后，p_file_stat有delete标记并移动到
	 * global delete链表。于是得到p_file_stat在链表的下一个next file_stat，然后把next file_stat到链表尾的file_stat
	 * 移动到file_stat_temp_head链表头。这样还是有问题，因为此时p_file_stat处于global delete链表，得到next file_stat
	 * 也是delete链表，next file_stat到链表尾的file_stat都是delete file_stat，把这些file_stat移动到
	 * file_stat_temp_head链表头(global temp middle large)，那file_stat就有状态问题了。所以这个问题，因此，
	 * 最终解决方案是，如果p_file_stat有delete标记，不再执行list_move_enhance()移动p_file_stat到链表尾的file_stat到
	 * file_stat_temp_head链表头了。毕竟这个概率很低的，无非就不移动而已，但是保证了稳定性。
	 *
	 * 最终决定，globa lock加锁前，先取得它在链表的下一个 next file_stat。然后加锁后，如果file_stat有delete标记，
	 * 那就判断它在链表的下一个 next file_stat有没有delete标记，没有的话就把next file_stat到链表尾的file_stat移动到
	 * file_stat_temp_head链表头。如果也有delete标记，那就不移动了。但是next file_stat也有可能是global delete链表头，
	 * 也有可能是file_stat_temp_head链表头，太麻烦了，风险太大，还是判定file_stat有delete标记，就不再执行
	 * list_move_enhance()得了
	 *
	 * */

	/* file_stat_temp_head链表非空且p_file_area不是指向链表头且p_file_area不是该链表的第一个成员，
	 * 则执行list_move_enhance()把本次遍历过的file_area~链表尾的file_area移动到链表
	 * file_stat_temp_head头，这样下次从链表尾遍历的是新的未遍历过的file_area。
	 * 当上边循环是遍历完所有链表所有成员才退出，p_file_area此时指向的是链表头file_stat_temp_head，
	 * 此时 p_file_stat->hot_cold_file_list 跟 &file_stat_temp_head 就相等了。当然，这种情况，是绝对不能
	 * 把p_file_stat到链表尾的file_stat移动到file_stat_temp_head链表头了，会出严重内核bug*/
	//if(!list_empty(file_stat_temp_head) && &p_file_stat->hot_cold_file_list != &file_stat_temp_head && &p_file_stat->hot_cold_file_list != file_stat_temp_head.next)
	{
		spin_lock(&p_hot_cold_file_global->global_lock);
#if 0	
		p_file_stat_next = list_next_entry(p_file_stat, hot_cold_file_list);
		if(file_stat_in_delete(p_file_stat)){
			if(!file_stat_in_delete(p_file_stat_next) && p_file_stat_next != &p_hot_cold_file_global->file_stat_delete_head){
				/*将链表尾已经遍历过的file_stat移动到链表头，下次从链表尾遍历的才是新的未遍历过的file_stat。这个过程必须加锁*/
				list_move_enhance(file_stat_temp_head,&p_file_stat_next->hot_cold_file_list);
			}
		}else
			list_move_enhance(file_stat_temp_head,&p_file_stat->hot_cold_file_list);
#endif
		/*p_file_stat不能是链表头，并且不能是被iput()并发标记delete并移动到global delete链表*/
		if(&p_file_stat->hot_cold_file_list != file_stat_temp_head  && !file_stat_in_delete(p_file_stat)){
			/*将链表尾已经遍历过的file_stat移动到链表头，下次从链表尾遍历的才是新的未遍历过的file_stat。这个过程必须加锁*/
		    if(can_file_stat_move_to_list_head(p_file_stat,file_stat_in_list_type))
			    list_move_enhance(file_stat_temp_head,&p_file_stat->hot_cold_file_list);
		}

		spin_unlock(&p_hot_cold_file_global->global_lock);
	}


	if(shrink_page_printk_open1)
		printk("3:%s %s %d p_hot_cold_file_global:0x%llx scan_file_stat_count:%d scan_file_area_count:%d scan_cold_file_area_count:%d file_stat_count_in_cold_list:%d  real_scan_file_stat_count:%d\n",__func__,current->comm,current->pid,(u64)p_hot_cold_file_global,scan_file_stat_count,scan_file_area_count,scan_cold_file_area_count,file_stat_count_in_cold_list,real_scan_file_stat_count);

	//扫描的file_area个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_file_area_count += scan_file_area_count;
	//扫描的file_stat个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_file_stat_count += scan_file_stat_count;
	//扫描到的处于delete状态的file_stat个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_delete_file_stat_count += scan_delete_file_stat_count;

	return scan_cold_file_area_count;
}
/*遍历file_stat_small->temp链表上的file_area*/
static noinline int file_stat_small_list_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_small *p_file_stat_small,unsigned int scan_file_area_max,struct list_head *file_area_free_temp)
{
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int scan_file_area_count = 0;
	unsigned int file_area_age_dx;
	unsigned int temp_to_warm_file_area_count = 0,scan_cold_file_area_count = 0;
	unsigned int scan_read_file_area_count = 0;
	unsigned int scan_ahead_file_area_count = 0;
	unsigned int temp_to_hot_file_area_count = 0;

	//从链表尾开始遍历，链表尾的成员更老，链表头的成员是最新添加的
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_small->file_area_temp,file_area_list){

		/* 新的版本hot_file_update_file_status()遇到refault、hot file_area，只是做个标记，而不会把file_area移动到
		 * file_stat->refault、hot链表，因此file_stat->temp链表上的file_area除了有in_temp_list标记，还有
		 * in_refault_list、in_hot_list标记，故要把file_area_in_temp_list_error(p_file_area)判断去掉*/
#if 0
		if(!file_area_in_temp_list(p_file_area)  || file_area_in_temp_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
#endif
		if(++ scan_file_area_count > scan_file_area_max)
			break;

		file_area_age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;

		/*一个file_area可能在hot_file_update_file_status()中被并发设置in_temp_list、in_hot_list、in_refault_list 
		 * 这3种属性，因此要if(file_area_in_refault_list(p_file_area))也需要判断清理in_hot_list属性。in_hot_list同理*/
		if(file_area_in_temp_list(p_file_area)){

			/*file_area经过FILE_AREA_TEMP_TO_COLD_AGE_DX个周期还没有被访问，则被判定是冷file_area，然后就释放该file_area的page*/
			if(file_area_age_dx > p_hot_cold_file_global->file_area_temp_to_cold_age_dx){
				/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
				if(file_area_in_ahead(p_file_area)){
					clear_file_area_in_ahead(p_file_area);
					scan_ahead_file_area_count ++;
					/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
					if(IS_MEMORY_ENOUGH(p_hot_cold_file_global))
						continue;
				}
				/* 当前内存不紧张且内存回收不困难，则遇到read文件页的file_area先跳过，不回收。目的是尽可能回收write文件页，
				 * read文件页一旦回收再被访问就会发生refault，不仅导致read耗时长，读磁盘功耗还高。但是如果这个read属性的
				 * file_area很长很长很长时间没访问，也参与内存回收*/
				if(file_area_page_is_read(p_file_area)){
					scan_read_file_area_count ++;
					if(!(IS_IN_MEMORY_EMERGENCY_RECLAIM(p_hot_cold_file_global) || file_area_age_dx > p_hot_cold_file_global->file_area_reclaim_read_age_dx))
						continue;
					clear_file_area_page_read(p_file_area);
				}

				//每遍历到一个就加一次锁，浪费性能，可以先移动到一个临时链表上，循环结束后加一次锁，然后把这些file_area或file_stat移动到目标链表???????
				spin_lock(&p_file_stat->file_stat_lock);
				/*为什么file_stat_lock加锁后要再判断一次file_area是不是被访问了,因为可能有这种情况:上边的if成立,此时file_area还没被访问。但是此时有进程
				  先执行hot_file_update_file_status()获取file_stat_lock锁,然后访问当前file_area,file_area不再冷了,当前进程此时获取file_stat_lock锁失败,
				  等获取file_stat_lock锁成功后，file_area的file_area_age就和global_age相等了。变量加减后的判断，在spin_lock前后各判断一次有必要的!!!!!*/
				if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <  p_hot_cold_file_global->file_area_temp_to_cold_age_dx){
					spin_unlock(&p_file_stat->file_stat_lock);    
					continue;
				}
				//access_count清0，如果内存回收期间又被访问了，access_count将大于0，将被判断为refault page。
				file_area_access_count_clear(p_file_area);
				clear_file_area_in_temp_list(p_file_area);
				p_file_stat->file_area_count_in_temp_list --;
				/*设置file_area处于file_stat的free_temp_list链表。这里设定，不管file_area处于file_stat->file_area_free_temp还是
				 *file_stat->file_area_free链表，都是file_area_in_free_list状态，没有必要再区分二者。主要设置file_area的状态需要
				 遍历每个file_area并file_stat_lock加锁，再多设置一次set_file_area_in_free_temp_list状态浪费性能。这点需注意!!!!!!!!!!!!!*/
				set_file_area_in_free_list(p_file_area);
				/*需要加锁，此时可能有进程执行hot_file_update_file_status()并发向该p_file_area前或者后插入新的file_area，这里是把该
				 * p_file_area从file_area_temp链表剔除，存在同时修改该p_file_area在file_area_temp链表前的file_area结构的next指针和在链表后的
				 * file_area结构的prev指针，并发修改同一个变量就需要加锁*/
				//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free_temp);
				list_move(&p_file_area->file_area_list,file_area_free_temp);
				spin_unlock(&p_file_stat->file_stat_lock);

				scan_cold_file_area_count ++;
			}
		}
		else if(file_area_in_refault_list(p_file_area)){
			if(file_area_age_dx >  p_hot_cold_file_global->file_area_refault_to_temp_age_dx){
				clear_file_area_in_refault_list(p_file_area);
				set_file_area_in_temp_list(p_file_area);
			}
		}
		else if(file_area_in_hot_list(p_file_area)){
			temp_to_hot_file_area_count ++;

			if(file_area_age_dx > p_hot_cold_file_global->file_area_hot_to_temp_age_dx){
				clear_file_area_in_hot_list(p_file_area);
				set_file_area_in_temp_list(p_file_area);
			}
		}
		else if(file_area_in_free_list(p_file_area)){
			/*如果file_stat->file_area_free链表上的file_area长时间没有被访问则释放掉file_area结构。之前的代码有问题，判定释放file_area的时间是
			 *file_area_free_age_dx，这样有问题，会导致file_area被内存回收后，在下个周期file_area立即被释放掉。原因是file_area_free_age_dx=5，
			 file_area_temp_to_cold_age_dx=5，下个内存回收周期 global_age - file_area_free_age_dx肯定大于5*/
			if(file_area_age_dx > 
					(p_hot_cold_file_global->file_area_free_age_dx + p_hot_cold_file_global->file_area_temp_to_cold_age_dx)){
				/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
				if(file_area_in_ahead(p_file_area)){
					clear_file_area_in_ahead(p_file_area);
					/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
					if(IS_MEMORY_ENOUGH(p_hot_cold_file_global))
						break;
				}
				file_area_free_count ++;
				/*hot_file_update_file_status()函数中会并发把file_area从file_stat->file_area_free链表移动到file_stat->file_area_free_temp
				 *链表.这里把file_stat->file_area_free链表上的file_area剔除掉并释放掉，需要spin_lock(&p_file_stat->file_stat_lock)加锁，
				 *这个函数里有加锁*/
				if(cold_file_area_delete(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area) > 0){
					/*在释放file_area过程发现file_area分配page了，于是把file_area移动到file_stat->refault链表*/
					file_area_access_count_clear(p_file_area);
					file_area_free_to_refault_list_count ++;
					clear_file_area_in_free_list(p_file_area);
					set_file_area_in_refault_list(p_file_area);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
					/*检测到refault的file_area个数加1*/
					p_hot_cold_file_global->check_refault_file_area_count ++;
				}
			}
		}
	}

#if 0
	/*参与内存回收的冷file_area个数*/
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_cold_file_area_count_from_temp += scan_cold_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_read_file_area_count_from_temp += scan_read_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.temp_to_hot_file_area_count += temp_to_hot_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_ahead_file_area_count_from_temp += scan_ahead_file_area_count;
	p_hot_cold_file_global->hot_cold_file_shrink_counter.temp_to_warm_file_area_count += temp_to_warm_file_area_count;
#endif	
	return scan_file_area_count;
}

static noinline unsigned int get_file_area_from_file_stat_small_list(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max,unsigned int scan_file_stat_max,struct list_head *file_stat_temp_head,unsigned char file_stat_list_type)
{
	struct file_stat *p_file_stat_small,*p_file_stat__small_temp;
	//struct file_area *p_file_area,*p_file_area_temp;
	LIST_HEAD(file_area_free_temp);

	unsigned int scan_file_area_count  = 0;
	unsigned int scan_move_to_mmap_head_file_stat_count  = 0;
	unsigned int scan_file_stat_count  = 0;
	unsigned int real_scan_file_stat_count  = 0;
	unsigned int scan_delete_file_stat_count = 0;
	unsigned int scan_cold_file_area_count = 0;
	unsigned int file_stat_in_list_type = -1;
	//unsigned int scan_fail_file_stat_count = 0;

	//unsigned int cold_file_area_for_file_stat = 0;
	unsigned int file_stat_count_in_cold_list = 0;



	list_for_each_entry_safe_reverse(p_file_stat_small,p_file_stat_small_temp,file_stat_temp_head,hot_cold_file_list){

		if(!file_stat_in_file_stat_small_file_head_list(p_file_stat_small) || file_stat_in_file_stat_small_file_head_list_error(p_file_stat_small))
		        panic("%s file_stat:0x%llx not int file_stat_temp_head status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		if(scan_file_area_count > scan_file_area_max || scan_file_stat_count ++ > scan_file_stat_max)
			break;

	        //要确认很小的文件，不断地pagecache写，是否会导致inode->i_size和mapping->nrpages一起增大???????????????????????????	
		if(file_stat_small->file_area_count > 68){
			struct file_stat *p_file_stat;

			clear_file_stat_in_file_stat_small_file_head_list(p_file_stat_small);
			set_file_stat_in_file_stat_temp_file_head_list(p_file_stat_small);
			p_file_stat = file_stat_alloc_and_init(p_file_stat_small->mapping);

			/*注意，file_stat_alloc_and_init()里分配新的file_stat并赋值给mapping->rh_reserved1后，新的进程读写该文件，
			 * 执行到update_hot_file函数，才会使用新的file_stat，而不是老的file_stat_small。这里加个内存屏障，保证
			 * 对mapping->rh_reserved1立即生效，否则会导致还在使用老的file_stat_small，统计文件冷热信息会丢失*/
			smp_wmb();
			spin_lock(&p_hot_cold_file_global->global_lock);
			/*可能此时该文件被iput delete了，要防护*/
			if(!file_stat_in_delete(p_file_stat_small) && !file_stat_in_delete(p_file_stat)){
				memcpy(p_file_stat->file_stat_base,p_file_stat_small->file_stat_base);
				call_rcu(&p_file_stat_small->i_rcu, i_file_stat_callback);
			}
			spin_unlock(&p_hot_cold_file_global->global_lock);

			continue;
		}

		if(mapping_mapped((struct address_space *)p_file_stat_small->mapping) && (p_file_stat_small->file_area_count == p_file_stat_small->file_area_count_in_temp_list)){
			scan_move_to_mmap_head_file_stat_count ++;
			//????????????????????????????????????????????????要兼容file_stat_small
			cache_file_stat_move_to_mmap_head(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,FILE_STAT_SMALL);
			continue;
		}

		/*可能iput最后直接把delete file_stat移动到global delete链表，并标记file_stat in delete状态*/
		if(file_stat_in_delete(p_file_stat_small)){
			scan_delete_file_stat_count ++;
			printk("%s file_stat:0x%llx delete status:0x%lx\n",__func__,(u64)p_file_stat_small,p_file_stat_small->file_stat_status);
			continue;
		}
		/* 如果file_stat的file_area全被释放了，则把file_stat移动到hot_cold_file_global->file_stat_zero_file_area_head链表。
		 * 但是唯一可能存在的并发移动file_stat的情况是iput()释放file_stat，标记file_stat delete，并把file_stat移动到
		 * global delete链表。因此要global_lock加锁后再判断一次file_stat是否被iput()把file_stat移动到global delete链表了*/
		else if(p_file_stat_small->file_area_count == 0){
			/*本来以为只有global temp链表上file_stat移动到global zero链表才需要加锁，因为会与读写文件进程
			 *执行__filemap_add_folio()向global temp添加file_stat并发修改global temp链表。但是漏了一个并发场景，
			 *文件inode释放会执行iput()标记file_stat的delete，然后把file_stat移动到global zero，对
			 *global temp、middle、large链表上file_stat都会这样操作，并发修改这些链表头。故这里
			 *global temp、middle、large链表上file_stat移动到global zero链表，都需要global lock加锁，因为iput()
			 *同时会并发修改*global temp、middle、large链表头，移动他们上边的file_stat到global zero链表!!!!!!!!!!!!!!!*/
			spin_lock(&p_hot_cold_file_global->global_lock);
			/*file_stat被iput()并发标记delete了，则不再理会*/
			if(!file_stat_in_delete(p_file_stat_small)){
				
				clear_file_stat_in_file_stat_small_file_head_list(p_file_stat_small);
				set_file_stat_in_zero_file_area_list(p_file_stat_small);
				p_hot_cold_file_global->file_stat_count_zero_file_area ++;
				//如果该文件inode被释放了，则把对应file_stat移动到hot_cold_file_global->file_stat_zero_file_area_head链表
				//????????????????????????????????????????????????????要兼容file_stat_small
				list_move(&p_file_stat_small->hot_cold_file_list,&p_hot_cold_file_global->file_stat_zero_file_area_head);
			}
			spin_unlock(&p_hot_cold_file_global->global_lock);

			continue;
		}

		/*file_area_free_temp现在不是file_stat->file_area_free_temp全局，而是一个临时链表，故每次向该链表添加file_area前必须要初始化*/
		INIT_LIST_HEAD(&file_area_free_temp);

		scan_file_area_max = 64;
		/*file_stat->temp链表上的file_area的处理，冷file_area且要内存回收的移动到file_area_free_temp临时链表，下边回收该链表上的file_area的page*/
		scan_file_area_count += file_stat_small_list_file_area_solve(p_hot_cold_file_global,p_file_stat_small,scan_file_area_max,&file_area_free_temp);


		/*回收file_area_free_temp临时链表上的冷file_area的page，回收后的file_area移动到file_stat->free链表头*/
		free_page_from_file_area(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,&file_area_free_temp,FILE_STAT_SMALL);
	}

        return scan_file_area_count;	
}
static noinline int walk_throuth_all_file_area(struct hot_cold_file_global *p_hot_cold_file_global)
{
	struct file_stat * p_file_stat,*p_file_stat_temp;
	//struct file_area *p_file_area,*p_file_area_temp;
	unsigned int scan_hot_file_area_max;
	unsigned int scan_temp_file_area_max,scan_temp_file_stat_max;
	unsigned int scan_middle_file_area_max,scan_middle_file_stat_max;
	unsigned int scan_large_file_area_max,scan_large_file_stat_max;
	unsigned int scan_cold_file_area_count = 0;
	//unsigned int file_area_hot_to_warm_list_count = 0;
	unsigned int del_file_stat_count = 0,del_file_area_count = 0;

	/*根据当前的内存状态调整各个内存回收age差参数*/
	change_memory_reclaim_age_dx(p_hot_cold_file_global);

	switch(p_hot_cold_file_global->memory_pressure_level){
		/*内存非常紧缺*/
		case MEMORY_EMERGENCY_RECLAIM:
			scan_temp_file_stat_max   = 128;
			scan_middle_file_stat_max = 64;
			scan_large_file_stat_max  = 32;

			scan_temp_file_area_max   = 512;
			scan_middle_file_area_max = 512;
			scan_large_file_area_max  = 512;

			scan_hot_file_area_max = 512;
			break;
		/*内存紧缺*/
		case MEMORY_PRESSURE_RECLAIM:
			scan_temp_file_stat_max   = 64;
			scan_middle_file_stat_max = 32;
			scan_large_file_stat_max  = 16;

			scan_temp_file_area_max   = 256;
			scan_middle_file_area_max = 256;
			scan_large_file_area_max  = 256;
			
			scan_hot_file_area_max = 256;
			break;
		/*内存碎片有点多，或者前后两个周期分配的内存数太多*/		
		case MEMORY_LITTLE_RECLAIM:
			scan_temp_file_stat_max   = 32;
			scan_middle_file_stat_max = 16;
			scan_large_file_stat_max  = 8;

			scan_temp_file_area_max   = 128;
			scan_middle_file_area_max = 128;
			scan_large_file_area_max  = 128;
			
			scan_hot_file_area_max  = 128;
			break;

		/*一般情况*/
		default:
			/*设置大点是为了尽量多扫描so、可执行文件这种原本是mmap的文件但最初被判定为cache文件*/
			scan_temp_file_stat_max   = 16;
			scan_middle_file_stat_max = 8;
			scan_large_file_stat_max  = 2;

			scan_temp_file_area_max   = 64;
			scan_middle_file_area_max = 64;
			scan_large_file_area_max  = 64;
			
			scan_hot_file_area_max  = 64;
			break;
	}
	printk("global_age:%d memory_pressure_level:%d scan_temp_file_stat_max:%d scan_temp_file_area_max:%d scan_middle_file_stat_max:%d scan_middle_file_area_max:%d scan_large_file_stat_max:%d scan_large_file_area_max:%d scan_hot_file_area_max:%d file_area_temp_to_cold_age_dx:%d file_area_hot_to_temp_age_dx:%d file_area_refault_to_temp_age_dx:%d\n",p_hot_cold_file_global->global_age,p_hot_cold_file_global->memory_pressure_level,scan_temp_file_stat_max,scan_temp_file_area_max,scan_middle_file_stat_max,scan_middle_file_area_max,scan_large_file_stat_max,scan_large_file_area_max,scan_hot_file_area_max,p_hot_cold_file_global->file_area_temp_to_cold_age_dx,p_hot_cold_file_global->file_area_hot_to_temp_age_dx,p_hot_cold_file_global->file_area_refault_to_temp_age_dx);
	
    get_file_area_from_file_stat_small_list(p_hot_cold_file_global,scan_temp_file_area_max*2,scan_temp_file_stat_max*2,&p_hot_cold_file_global->file_stat_small_file_head,FILE_STAT_SMALL);

	/* 遍历hot_cold_file_global->file_stat_temp_large_file_head链表尾巴上边的文件file_stat，再遍历每一个文件file_stat->temp、warm
	 * 链表尾上的file_area，判定是冷file_area的话则参与内存回收，内存回收后的file_area移动到file_stat->free链表。然后对
	 * file_stat->refault、hot、free链表上file_area进行对应处理*/
	scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,scan_large_file_area_max,scan_large_file_stat_max, 
			&p_hot_cold_file_global->file_stat_large_file_head,FILE_STAT_IN_LARGE_FILE_LIST);

	/* 遍历hot_cold_file_global->file_stat_temp_middle_file_head链表尾巴上边的文件file_stat，再遍历每一个文件file_stat->temp、warm
	 * 链表尾上的file_area，判定是冷file_area的话则参与内存回收，内存回收后的file_area移动到file_stat->free链表。然后对
	 * file_stat->refault、hot、free链表上file_area进行对应处理*/
	scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,scan_middle_file_area_max,scan_middle_file_stat_max, 
			&p_hot_cold_file_global->file_stat_middle_file_head,FILE_STAT_IN_MIDDLE_FILE_LIST);


	/* 遍历hot_cold_file_global->file_stat_temp_file_head链表尾巴上边的文件file_stat，再遍历每一个文件file_stat->temp、warm
	 * 链表尾上的file_area，判定是冷file_area的话则参与内存回收，内存回收后的file_area移动到file_stat->free链表。然后对
	 * file_stat->refault、hot、free链表上file_area进行对应处理*/
	scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,scan_temp_file_area_max,scan_temp_file_stat_max, 
			&p_hot_cold_file_global->file_stat_temp_head,FILE_STAT_IN_TEMP_FILE_LIST);


	/* 遍历global hot链表上的file_stat，再遍历这些file_stat->hot链表上的file_area，如果不再是热的，则把file_area
	 * 移动到file_stat->warm链表。如果file_stat->hot链表上的热file_area个数减少到热文件阀值以下，则降级到
	 * global temp、middle_file、large_file链表*/
	hot_file_stat_solve(p_hot_cold_file_global,scan_hot_file_area_max);
	if(0 == test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		return 0;

	/*遍历global file_stat_delete_head链表上已经被删除的文件的file_stat，
	  一次不能删除太多的file_stat对应的file_area，会长时间占有cpu，后期需要调优一下*/
	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->file_stat_delete_head,hot_cold_file_list){
		if(!file_stat_in_delete(p_file_stat) /*|| file_stat_in_delete_error(p_file_stat)*/)
			panic("%s file_stat:0x%llx not delete status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,&p_file_stat->file_stat_base,FILE_STAT_NORMAL);
		del_file_stat_count ++;
		/*防止删除file_area耗时太长*/
		if(del_file_area_count > 256)
			break;
	}
	
	//从系统启动到目前释放的page个数
	p_hot_cold_file_global->free_pages += p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;
	//释放的file_area个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.del_file_area_count = del_file_area_count;
	//释放的file_stat个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.del_file_stat_count = del_file_stat_count;

	if(0 == test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		return 0;

	//对没有file_area的file_stat的处理
	file_stat_has_zero_file_area_manage(p_hot_cold_file_global);

	if(0 == test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		return 0;

	//打印所有file_stat的file_area个数和page个数
	if(shrink_page_printk_open1)
		hot_cold_file_print_all_file_stat(p_hot_cold_file_global,NULL,0);
	//打印内存回收时统计的各个参数
	if(shrink_page_printk_open1)
		printk_shrink_param(p_hot_cold_file_global,NULL,0);

	return 0;
}
static int inline memory_zone_solve(struct zone *zone,unsigned long zone_free_page,int *zone_memory_tiny_or_enough_count)
{
	int index;
	int memory_pressure_level = MEMORY_IDLE_SCAN;

	/*如果zone free内存低于zone水位阀值，进入紧急内存回收模式*/
	if(zone_free_page < high_wmark_pages(zone)){
		if(zone_free_page < low_wmark_pages(zone))
			memory_pressure_level = MEMORY_EMERGENCY_RECLAIM;
		else
			memory_pressure_level = MEMORY_PRESSURE_RECLAIM;

        *zone_memory_tiny_or_enough_count = *zone_memory_tiny_or_enough_count + 1;
		printk("%s %s zone_free_page:%ld memory_pressure_level:%d\n",__func__,zone->name,zone_free_page,memory_pressure_level);
	}else{/*如果内存碎片有点严重*/
		index = fragmentation_index(zone,PAGE_ALLOC_COSTLY_ORDER);
		if(index > sysctl_extfrag_threshold){
			memory_pressure_level = MEMORY_LITTLE_RECLAIM;
			printk("%s memory fragment %s index:%d\n",__func__,zone->name,index);
		}
        *zone_memory_tiny_or_enough_count = *zone_memory_tiny_or_enough_count - 1;
	}

	return memory_pressure_level;
}
/*根据 内存碎片程度、每个内存zone可用内存、上次内存回收page数，决定本次是否进入紧急内存回收模式以及本次预期扫描的file_area个数*/
static noinline int check_memory_reclaim_necessary(struct hot_cold_file_global *p_hot_cold_file_global)
{
	/*内存紧张的程度*/
	int memory_pressure_level = MEMORY_IDLE_SCAN;
	long free_page_dx = 0;
	pg_data_t *pgdat;
	struct zone *zone;
	unsigned long zone_free_page;
	int check_zone_free_many_pages;
	/*遇到内存紧张的zone加1，遇到内存充足的减1，最后如果大于0，则说明内存紧张的zone更多，进入紧急内存回收模式*/
	int zone_memory_tiny_or_enough_count = 0;

	for (pgdat = first_online_pgdat();pgdat;pgdat = next_online_pgdat(pgdat)){
		/*发现有zone内存紧缺*/
		if(memory_pressure_level > MEMORY_LITTLE_RECLAIM)
			break;

		for (zone = pgdat->node_zones; zone - pgdat->node_zones < MAX_NR_ZONES; ++zone) {
			/*空zone跳过*/
			if (!populated_zone(zone))
				continue;

			if(0 == pgdat->node_id){
				if(0 == strncmp("Normal",zone->name,6)){
					zone_free_page = zone_page_state(zone, NR_FREE_PAGES);
					/*当前zone上个周期到现在free内存差值*/
					if(p_hot_cold_file_global->normal_zone_free_pages_last)
						free_page_dx = zone_free_page - p_hot_cold_file_global->normal_zone_free_pages_last;

					/*保存上一次的high阀值*/
					p_hot_cold_file_global->normal_zone_free_pages_last = zone_free_page;
					memory_pressure_level = memory_zone_solve(zone,zone_free_page,&zone_memory_tiny_or_enough_count);
					if(memory_pressure_level > MEMORY_LITTLE_RECLAIM)
						break;
				}
				else if(0 == strncmp("HighMem",zone->name,7)){
					zone_free_page = zone_page_state(zone, NR_FREE_PAGES);
					/*当前zone上个周期到现在free内存差值*/
					if(p_hot_cold_file_global->highmem_zone_free_pages_last)
						free_page_dx = zone_free_page - p_hot_cold_file_global->highmem_zone_free_pages_last;

					p_hot_cold_file_global->highmem_zone_free_pages_last = zone_free_page;
					memory_pressure_level = memory_zone_solve(zone,zone_free_page,&zone_memory_tiny_or_enough_count);
					if(memory_pressure_level > MEMORY_LITTLE_RECLAIM)
						break;
				}
				else if(0 == strncmp("DMA32",zone->name,5)){
					zone_free_page = zone_page_state(zone, NR_FREE_PAGES);
					/*当前zone上个周期到现在free内存差值*/
					if(p_hot_cold_file_global->dma32_zone_free_pages_last)
						free_page_dx = zone_free_page - p_hot_cold_file_global->dma32_zone_free_pages_last;

					p_hot_cold_file_global->dma32_zone_free_pages_last = zone_free_page;
					memory_pressure_level = memory_zone_solve(zone,zone_free_page,&zone_memory_tiny_or_enough_count);
					if(memory_pressure_level > MEMORY_LITTLE_RECLAIM)
						break;
				}
			}else if(1 == pgdat->node_id){
				if(0 == strncmp("Normal",zone->name,6)){
					zone_free_page = zone_page_state(zone, NR_FREE_PAGES);
					/*当前zone上个周期到现在free内存差值*/
					if(p_hot_cold_file_global->normal1_zone_free_pages_last)
						free_page_dx = zone_free_page - p_hot_cold_file_global->normal1_zone_free_pages_last;

					p_hot_cold_file_global->normal1_zone_free_pages_last = zone_free_page;
					memory_pressure_level = memory_zone_solve(zone,zone_free_page,&zone_memory_tiny_or_enough_count);
					if(memory_pressure_level > MEMORY_LITTLE_RECLAIM)
						break;
				}
			}

			if((free_page_dx > 512) && (0 == check_zone_free_many_pages)){
		        printk("%s free_page_dx:%ld free too much page\n",__func__,free_page_dx);
				check_zone_free_many_pages = 1;
			}
		}
	}

	/* 没有发现内存碎片严重，也没有发现内存zone内存紧缺。但是如果前后两个周期某个zone发现有大量内存分配。
	 * 也令memory_pressure_level置1，进行少量内存回收*/
	if(check_zone_free_many_pages && (MEMORY_IDLE_SCAN == memory_pressure_level)){
		memory_pressure_level = MEMORY_LITTLE_RECLAIM;
	}

	/*遇到内存紧张的zone加1，遇到内存充足的减1，最后如果小于等于0，则说明至少有一个内存zone内存还充足，此时不会紧急内存回收模式*/
	if((zone_memory_tiny_or_enough_count <= 0)&& (memory_pressure_level >= MEMORY_PRESSURE_RECLAIM)){
		memory_pressure_level = MEMORY_LITTLE_RECLAIM;
		printk("%s zone_memory_tiny_or_enough_count:%d memory not tiny\n",__func__,zone_memory_tiny_or_enough_count);
	}

	return memory_pressure_level;
}
#define IDLE_MAX 3
static int hot_cold_file_thread(void *p){
	struct hot_cold_file_global *p_hot_cold_file_global = (struct hot_cold_file_global *)p;

	int sleep_count;
	int memory_pressure_level;
	/*设置为IDLE_MAX是为了第一次就能扫描文件file_stat，主要是为了扫描so、可执行文件 这种现在是mmap文件但最初被判定为cache文件*/
	int idle_age_count = IDLE_MAX;

	while(!kthread_should_stop()){
		sleep_count = 0;
		while(++sleep_count < p_hot_cold_file_global->global_age_period){
			msleep(1000);
		}
		//每个周期global_age加1
		hot_cold_file_global_info.global_age ++;

		memory_pressure_level = check_memory_reclaim_necessary(p_hot_cold_file_global);
		/*不用内存回收*/
		if(MEMORY_IDLE_SCAN == memory_pressure_level){
			if(++idle_age_count < IDLE_MAX)
				continue;
		}
		idle_age_count = 0;

		if(test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		{
			/*内存回收前记录memory_pressure_level*/	
			p_hot_cold_file_global->memory_pressure_level = memory_pressure_level;
			/*唤醒异步内存回收线程*/
			wake_up_process(p_hot_cold_file_global->async_memory_reclaim);
		}
	}
	return 0;
}
static int async_memory_reclaim_main_thread(void *p){
	struct hot_cold_file_global *p_hot_cold_file_global = (struct hot_cold_file_global *)p;

	while(!kthread_should_stop()){
		/*清空上一轮内存回收统计参数*/
		memset(&p_hot_cold_file_global->hot_cold_file_shrink_counter,0,sizeof(struct hot_cold_file_shrink_counter));
		memset(&p_hot_cold_file_global->mmap_file_shrink_counter,0,sizeof(struct mmap_file_shrink_counter));

		/*回收cache文件页*/
		walk_throuth_all_file_area(p_hot_cold_file_global);
		/*回收mmap文件页*/
		walk_throuth_all_mmap_file_area(p_hot_cold_file_global);

		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}
	return 0;
}
/*根据当前的内存状态调整各个内存回收参数*/
static void change_memory_reclaim_age_dx(struct hot_cold_file_global *p_hot_cold_file_global)
{
	switch(p_hot_cold_file_global->memory_pressure_level){
		/*内存非常紧缺*/
		case MEMORY_EMERGENCY_RECLAIM:
			p_hot_cold_file_global->file_area_temp_to_cold_age_dx = FILE_AREA_TEMP_TO_COLD_AGE_DX >> 1;
			p_hot_cold_file_global->file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			p_hot_cold_file_global->file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			//p_hot_cold_file_global->file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			//p_hot_cold_file_global->file_area_warm_to_temp_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			//p_hot_cold_file_global->file_area_reclaim_read_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;

			break;
			/*内存紧缺*/
		case MEMORY_PRESSURE_RECLAIM:
			p_hot_cold_file_global->file_area_temp_to_cold_age_dx = FILE_AREA_TEMP_TO_COLD_AGE_DX - (FILE_AREA_TEMP_TO_COLD_AGE_DX >> 2);
			p_hot_cold_file_global->file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			p_hot_cold_file_global->file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			//p_hot_cold_file_global->file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			//p_hot_cold_file_global->file_area_warm_to_temp_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			//p_hot_cold_file_global->file_area_reclaim_read_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;

			break;
			/*内存碎片有点多，或者前后两个周期分配的内存数太多*/		
		case MEMORY_LITTLE_RECLAIM:
			/*一般情况*/
		default:
			p_hot_cold_file_global->file_area_temp_to_cold_age_dx = FILE_AREA_TEMP_TO_COLD_AGE_DX;
			p_hot_cold_file_global->file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx << 3;
			p_hot_cold_file_global->file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx << 4;
			//p_hot_cold_file_global->file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			//p_hot_cold_file_global->file_area_warm_to_temp_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			//p_hot_cold_file_global->file_area_reclaim_read_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			break;
	}
}

static int __init hot_cold_file_init(void)
{
	hot_cold_file_global_info.file_stat_cachep = kmem_cache_create("file_stat",sizeof(struct file_stat),0,0,NULL);
	hot_cold_file_global_info.file_area_cachep = kmem_cache_create("file_area",sizeof(struct file_area),0,0,NULL);
	hot_cold_file_global_info.small_file_stat_cachep = kmem_cache_create("small_file_stat",sizeof(struct file_stat_small),0,0,NULL);

	if(!hot_cold_file_global_info.file_stat_cachep || !hot_cold_file_global_info.file_area_cachep || ){
		printk("%s slab 0x%llx 0x%llx error\n",__func__,(u64)hot_cold_file_global_info.file_stat_cachep,(u64)hot_cold_file_global_info.file_area_cachep);
		return -1;
	}

	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_hot_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_temp_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_large_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_middle_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.cold_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_delete_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_zero_file_area_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_small_file_head);

	INIT_LIST_HEAD(&hot_cold_file_global_info.drop_cache_file_stat_head);

	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_temp_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_zero_file_area_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_delete_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_large_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_middle_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_hot_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_uninit_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_mapcount_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_small_file_head);

	spin_lock_init(&hot_cold_file_global_info.global_lock);
	spin_lock_init(&hot_cold_file_global_info.mmap_file_global_lock);

	//atomic_set(&hot_cold_file_global_info.ref_count,0);
	//atomic_set(&hot_cold_file_global_info.inode_del_count,0);

	hot_cold_file_global_info.file_area_temp_to_cold_age_dx = FILE_AREA_TEMP_TO_COLD_AGE_DX;
	hot_cold_file_global_info.file_area_hot_to_temp_age_dx = FILE_AREA_TEMP_TO_COLD_AGE_DX << 3;
	hot_cold_file_global_info.file_area_refault_to_temp_age_dx = FILE_AREA_TEMP_TO_COLD_AGE_DX << 4;
	/*file_area_temp_to_warm_age_dx取自file_area_temp_to_cold_age_dx的3/4*/
	hot_cold_file_global_info.file_area_temp_to_warm_age_dx = hot_cold_file_global_info.file_area_temp_to_cold_age_dx - (hot_cold_file_global_info.file_area_temp_to_cold_age_dx >> 2);
	hot_cold_file_global_info.file_area_warm_to_temp_age_dx = hot_cold_file_global_info.file_area_temp_to_cold_age_dx - (hot_cold_file_global_info.file_area_temp_to_cold_age_dx >> 1);
	hot_cold_file_global_info.file_area_reclaim_read_age_dx = hot_cold_file_global_info.file_area_temp_to_cold_age_dx * 20;
	hot_cold_file_global_info.file_area_free_age_dx = FILE_AREA_FREE_AGE_DX;

	hot_cold_file_global_info.file_stat_delete_age_dx  = FILE_STAT_DELETE_AGE_DX;
	hot_cold_file_global_info.global_age_period = ASYNC_MEMORY_RECLIAIM_PERIOD;
	hot_cold_file_global_info.file_area_move_to_head_count_max = FILE_AREA_MOVE_TO_HEAD_COUNT_MAX;

	//128M的page cache对应file_area个数，被判定为大文件
	hot_cold_file_global_info.file_area_level_for_large_file = (128*1024*1024)/(4096 *PAGE_COUNT_IN_AREA);
	hot_cold_file_global_info.file_area_level_for_middle_file = hot_cold_file_global_info.file_area_level_for_large_file/2;
	//mmap的文件，文件页超过50M就判定为大文件
	hot_cold_file_global_info.mmap_file_area_level_for_large_file = (50*1024*1024)/(4096 *PAGE_COUNT_IN_AREA);
	hot_cold_file_global_info.mmap_file_area_level_for_middle_file = hot_cold_file_global_info.mmap_file_area_level_for_large_file/2;

	hot_cold_file_global_info.mmap_file_area_temp_to_cold_age_dx = MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX;
	hot_cold_file_global_info.mmap_file_area_hot_to_temp_age_dx = MMAP_FILE_AREA_HOT_TO_TEMP_AGE_DX;
	hot_cold_file_global_info.mmap_file_area_refault_to_temp_age_dx = MMAP_FILE_AREA_REFAULT_TO_TEMP_AGE_DX;
	hot_cold_file_global_info.mmap_file_area_free_age_dx = MMAP_FILE_AREA_TO_FREE_AGE_DX;
	hot_cold_file_global_info.mmap_file_area_hot_age_dx = MMAP_FILE_AREA_HOT_AGE_DX;
	hot_cold_file_global_info.mmap_file_area_temp_to_warm_age_dx = hot_cold_file_global_info.mmap_file_area_temp_to_cold_age_dx - (hot_cold_file_global_info.mmap_file_area_temp_to_cold_age_dx >> 2);
	hot_cold_file_global_info.mmap_file_area_warm_to_temp_age_dx = hot_cold_file_global_info.mmap_file_area_temp_to_cold_age_dx - (hot_cold_file_global_info.mmap_file_area_temp_to_cold_age_dx >> 1);
	//64K对应的page数
	hot_cold_file_global_info.nr_pages_level = 16;
	/*该函数在setup_cold_file_area_reclaim_support_fs()之后，总是导致hot_cold_file_global_info.support_fs_type = -1，导致总file_area内存回收无效*/
	//hot_cold_file_global_info.support_fs_type = -1;


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
