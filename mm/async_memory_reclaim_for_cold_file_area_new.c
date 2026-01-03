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



#define AGE_DX_CHANGE_REFAULT_SLIGHT 0 
#define AGE_DX_CHANGE_REFAULT_SERIOUS 1
#define AGE_DX_CHANGE_REFAULT_CRITIAL 2
#define AGE_DX_CHANGE_WRITEONLY_IN_EMERGENCY_RECLAIM 3


/*当一个文件file_area个数超过FILE_AREA_MOVE_TO_HEAD_LEVEL，才允许一个周期内file_stat->temp链表上file_area移动到file_stat->temp链表头*/
#define FILE_AREA_MOVE_TO_HEAD_LEVEL 32
/*当mapcount值超过阀值则判定为mapcount file_area*/
#define MAPCOUNT_LEVEL 2
/*以下都是mmap文件在cache文件基础上，针对各种age的增量*/
#define MMAP_FILE_TEMP_TO_WARM_AGE_DX    20
#define MMAP_FILE_TEMP_TO_COLD_AGE_DX    30
#define MMAP_FILE_HOT_TO_TEMP_AGE_DX     6
#define MMAP_FILE_REFAULT_TO_TEMP_AGE_DX 8
#define MMAP_FILE_COLD_TO_FREE_AGE_DX    5
#define MMAP_FILE_WARM_TO_TEMP_AGE_DX  20
/*mmap file_area设置in_access标记后，过了10个周期没有被访问，就要清理掉in_access标记*/
#define MMAP_AHEAD_FILE_AREA_ACCESS_TO_COLD_AGE_DX  10
/*mmap file_area设置in_ahead标记后，过了30个周期没有被访问，就要清理掉in_ahead标记*/
#define MMAP_AHEAD_FILE_AREA_AHEAD_TO_COLD_AGE_DX  50
/*cache file_area设置in_ahead标记后，过了25个周期没有被访问，就要清理掉in_ahead标记*/
#define AHEAD_FILE_AREA_TO_COLD_AGE_DX  25
#define TINY_SMALL_TO_TINY_SMALL_ONE_AREA_LEVEL 3
#define TINY_SMALL_ONE_AREA_TO_TINY_SMALL_LEVEL 6
/*只读文件连续扫描到SCAN_SERIAL_WARM_FILE_AREA_LEVEL个file_area都是非冷的，说明age_dx需要调小，才能更容易找到冷file_area*/
#define SCAN_SERIAL_WARM_FILE_AREA_LEVEL 8
#define SCAN_SERIAL_WARM_FILE_AREA_LEVEL_FOR_READ_FILE 5
/*writeonly的文件，nrpages都是0了，但还要经理dx个周期没有被访问再移动到global temp链表，因为这种文件*/
#define WRITEONLY_FILE_MOVE_TO_TEMP_AGE_DX 300
#define HOT_READY 1
#define AHEAD_READY 2

//每次扫描文件file_stat的热file_area个数
#define SCAN_HOT_FILE_AREA_COUNT_ONCE 8
////每次扫描文件file_stat的mapcount file_area个数
#define SCAN_MAPCOUNT_FILE_AREA_COUNT_ONCE 8
/*文件file_stat长时间没有被访问，则对file_stat->refault_page_count清0*/
#define FILE_STAT_REFAULT_PAGE_COUNT_CLEAR_AGE_DX 300
struct hot_cold_file_global hot_cold_file_global_info = {
	.support_fs_type = -1,
};
unsigned long async_memory_reclaim_status = 1;

/* 一个后期比较重要的性能优化点：不再区分mmap和cache文件，合二为一，而是直接标记mmap和cache file_area：在创建folio时，
 * 执行到add_folio函数，根据mapping->i_mmap.rb_root是否为NULL，直接标记file_area为mmap或cache。后续mmap和cache文件
 * file_stat都只添加到一个链表，直接遍历这些file_stat的file_area，根据file_area的mmap和cache标记，决定file_area的
 * 冷热、age_dx是否适合回收。这个实现原理还好，优点很明显，不用再区分mmap和cache文件，有关的源码就能大大简化了。但是，
 * 这引入了新的问题，一是add_folio_for_file_area函数里，每次都要根据mapping->i_mmap.rb_root是否为NULL，
 * "标记file_area为mmap或cache"，逻辑稍微复杂，现在我还一直想简化add_folio_for_file_area函数。如果后续
 * add_folio_for_file_area函数性能损耗能正常才能考虑。并且，把mmap和cache文件都弄到一个链表上，加锁就成问题了。
 * 之前时分成两个链表，向链表添加file_stat、从链表移除file_stat，分别用global_lock和mmap_file_global_lock两把锁，
 * 用两把锁就降低了spin_lock抢占的概率。当然，两把锁、一把锁，到底损耗能有多大差异，可能也不大。这个思路后续
 * 有时间可以考虑开发一下这个思路。
 *
 * 还有一个思路，add_folio_for_file_area->file_stat_alloc_and_init_tiny_small()函数遍历，分配tiny_small_file_stat
 * 并global_lock加锁移动到global_file_stat链表上，要把tiny_small_file_stat移动到global_file_stat_temp_temp链表上，
 * 把global_lock锁换成global_lock_temp锁，减少跟异步内存回收线程移动到file_stat和iput()把file_stat从global_file_stat
 * 抢占锁的机会。但是tiny_small_file_stat移动到global_file_stat_temp_temp链表上，这些file_stat也有可能立即被iput()
 * 剔除呀，此时还得跟iput()把file_stat从global_file_stat抢占同一个锁。看来这个思路还得再打磨打磨，还是无法避免抢占
 * globla_lock锁。
 * */
unsigned int file_area_in_update_count;
unsigned int file_area_in_update_lock_count;
unsigned int file_area_move_to_head_count;
unsigned int enable_xas_node_cache = 1;
unsigned int enable_update_file_area_age = 1;
int shrink_page_printk_open1;
int shrink_page_printk_open_important;
int shrink_page_printk_open;

unsigned int xarray_tree_node_cache_hit;
int open_file_area_printk = 0;
int open_file_area_printk_important = 0;
int warm_list_printk = 0;

static void change_global_age_dx(struct hot_cold_file_global *p_hot_cold_file_global);
static void change_global_age_dx_for_mmap_file(struct hot_cold_file_global *p_hot_cold_file_global);
extern void deactivate_file_folio(struct folio *folio);
static noinline int check_memory_reclaim_necessary(struct hot_cold_file_global *p_hot_cold_file_global);

struct age_dx_param{
	unsigned int file_area_temp_to_cold_age_dx;
	unsigned int file_area_hot_to_temp_age_dx;
	unsigned int file_area_refault_to_temp_age_dx;
	unsigned int file_area_reclaim_ahead_age_dx;
	unsigned int file_area_reclaim_read_age_dx;

};
struct memory_reclaim_param
{
    unsigned int scan_hot_file_area_max;
	unsigned int scan_temp_file_stat_max;
	unsigned int scan_temp_file_area_max;
	unsigned int scan_middle_file_stat_max;
	unsigned int scan_middle_file_area_max;
	unsigned int scan_large_file_stat_max;
	unsigned int scan_large_file_area_max;
	unsigned int scan_small_file_stat_max;
	unsigned int scan_small_file_area_max;
	unsigned int scan_tiny_small_file_area_max;
	unsigned int scan_tiny_small_file_stat_max;
	//unsigned int scan_cold_file_area_count;
	unsigned int scan_cold_file_area_count;
	unsigned int mapcount_file_area_max;
	unsigned int scan_writeonly_file_stat_max;
	unsigned int scan_writeonly_file_area_max;
};
/*file_area->warm_list_num_and_access_freq只是一个unsigned char变量，bit0~bit3是access_freq，bit4~bit6是warm_list_num。
 *进程读写后执行的update函数里令access_freq加1，同时异步内存回收线程会更新warm_list_num。二者是否存在并发问题呢？
 *貌似前者执行new_val->warm_list_num_and_access_freq->val_bits->access_freq ++，后者执行new_val->warm_list_num_and_access_freq
 *->val_bits->warm_list_num = 3，貌似互不影响。但是，access_freq和warm_list_num并不是单独的变量，而是同属于一个unsigned char
 *变量。new_val->warm_list_num_and_access_freq->val_bits->access_freq ++ 并不是只会令 file_area->warm_list_num_and_access_freq
 *bit0~bit3加1，本质是file_area->warm_list_num_and_access_freq  = (file_area->warm_list_num_and_access_freq + 1)& 0xf。
 *你看到的是new_val->warm_list_num_and_access_freq->val_bits->access_freq ++，实际编译器还要把它转成
 *file_area->warm_list_num_and_access_freq  = (file_area->warm_list_num_and_access_freq + 1)& 0xf。而
 *new_val->warm_list_num_and_access_freq->val_bits->warm_list_num = 3，编译器要转成：取出file_area->warm_list_num_and_access_freq
 *的bit4~bit6，赋值3后赋值给file_area->warm_list_num_and_access_freq。因此对access_freq和warm_list_num的修改，本质
 都是对file_area->warm_list_num_and_access_freq变量的修改。因此access_freq和warm_list_num的修改，必须要做并发防护。这里采用的是
 cmpxchg()，先得到file_area->warm_list_num_and_access_freq老的值，然后access_freq加1或者warm_list_num赋值新的num。如果这个过程有
 另一个也修改了file_area->warm_list_num_and_access_freq变，会导致cmpxchg大返回值跟file_area->warm_list_num_and_access_freq老的值
 不相等，此时while循环成立，然后获取最新的file_area->warm_list_num_and_access_freq，再修改，在赋值，直到修改过程没有其他进程并发
 修改file_area->warm_list_num_and_access_freq。
 
 结论：如果存在两个进程同时修改一个变量不同的bit位，必须要考虑并发

static void inline file_area_access_freq_inc(struct file_area *p_file_area)
{
	union warm_list_num_and_access old_val,new_val;
	do{
		old_val = READ_ONCE(p_file_area->warm_list_num_and_access_freq->val);
		new_val = old_val;
		//只有4个bit位表示access_freq，最大值15，超过直接退出
		if(old_val->warm_list_num_and_access_freq->val_bits->access_freq + 1 > 15)
			break;
		//为防止不可预料的情况，必须内存乱序，必须与上0xF，从根源保证给access_freq赋的值，不会超过15
		new_val->warm_list_num_and_access_freq->val_bits->access_freq = (old_val->warm_list_num_and_access_freq->val_bits->access_freq + 1) & 0xF;

	}while(cmpxchg(p_file_area->warm_list_num_and_access_freq.val,old_val.val,new_val.val) != old_val.value);
}
*/
static void inline file_area_access_freq_clear(struct file_area *p_file_area)
{
	union warm_list_num_and_access_freq old_val,new_val;
	do{
		old_val = READ_ONCE(p_file_area->warm_list_num_and_access_freq);
		new_val = old_val;
		new_val.val_bits.access_freq = 0;

	}while(cmpxchg(&(p_file_area->warm_list_num_and_access_freq.val),old_val.val,new_val.val) != old_val.val);
}
static void inline file_area_access_freq_set(struct file_area *p_file_area,unsigned char val)
{
	union warm_list_num_and_access_freq old_val,new_val;
	do{
		old_val = READ_ONCE(p_file_area->warm_list_num_and_access_freq);
		new_val = old_val;
		new_val.val_bits.access_freq = val;

	}while(cmpxchg(&(p_file_area->warm_list_num_and_access_freq.val),old_val.val,new_val.val) != old_val.val);
}

//#define list_num_get(p_file_area)  (p_file_area->warm_list_num_and_access_freq.val_bits.warm_list_num)
//#define file_area_access_freq(p_file_area)  (p_file_area->warm_list_num_and_access_freq.val_bits.access_freq)
static void inline file_area_access_freq_inc(struct file_area *p_file_area)
{
	union warm_list_num_and_access_freq old_val,new_val;
	do{
		old_val = READ_ONCE(p_file_area->warm_list_num_and_access_freq);
		new_val = old_val;
		/*只有4个bit位表示access_freq，最大值15，超过直接退出*/
		if(old_val.val_bits.access_freq + 1 > 15)
			break;
		/*为防止不可预料的情况，必须内存乱序，必须与上0xF，从根源保证给access_freq赋的值，不会超过15*/
		new_val.val_bits.access_freq = (old_val.val_bits.access_freq + 1) & 0xF;

	}while(cmpxchg(&(p_file_area->warm_list_num_and_access_freq.val),old_val.val,new_val.val) != old_val.val);
}
static void inline list_num_update(struct file_area *p_file_area,char warm_list_num)
{
	union warm_list_num_and_access_freq old_val,new_val;
	do{
		old_val = READ_ONCE(p_file_area->warm_list_num_and_access_freq);
		new_val = old_val;
		/*只有3个bit位表示warm_list_num，最大值7，超过直接退出*/
		if(warm_list_num > 7){
			panic("update_file_area_access_freq warm_list_num:%d > 7\n",warm_list_num);
		}
		/*为防止不可预料的情况，比如内存乱序，必须与上0x7，从根源保证给warm_list_num赋的值，不会超过7*/
		new_val.val_bits.warm_list_num = warm_list_num & 0x7;

	}while(cmpxchg(&(p_file_area->warm_list_num_and_access_freq.val),old_val.val,new_val.val) != old_val.val);
}

/*****file_area、file_stat、inode 的delete*********************************************************************************/
static void i_file_area_callback(struct rcu_head *head)
{
	struct file_area *p_file_area = container_of(head, struct file_area, i_rcu);
	/*要释放的file_area如果page bit位还存在，则触发crash。正常肯定得是0*/
	if(file_area_have_page(p_file_area))
		panic("%s file_area:0x%llx file_area_state:0x%x has page error!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

	/*正常释放的file_area,file_area_delete.prev和next必须是0，否则说明异步内存回收线程在释放file_area时，又有iput()把file_area->file_area_delete移动到global delete链表了*/
	/*if(p_file_area->file_area_delete.prev || p_file_area->file_area_delete.next)
		panic("%s file_area:0x%llx file_area_state:0x%x file_area_delete error\n",__func__,(u64)p_file_area,p_file_area->file_area_state);*/

	set_file_area_in_deleted(p_file_area);

	kmem_cache_free(hot_cold_file_global_info.file_area_cachep,p_file_area);
}
static void i_file_stat_callback(struct rcu_head *head)
{
	struct file_stat_base *p_file_stat_base = container_of(head, struct file_stat_base, i_rcu);
	struct file_stat *p_file_stat = container_of(p_file_stat_base, struct file_stat, file_stat_base);

	/*注意，这里还要判断 file_araea->file_area_delete链表是否是空!!!!!!!!!!!!!!!!*/

	/*有必要在这里判断file_stat的temp、refault、hot、free、mapcount链表是否空，如果有残留file_area则panic。
	 * 防止因代码有问题，导致没处理干净所有的file_area*/
	if(!list_empty(&p_file_stat->file_stat_base.file_area_temp) || !list_empty(&p_file_stat->file_area_hot) || !list_empty(&p_file_stat->file_area_free) || !list_empty(&p_file_stat->file_area_warm_cold) || !list_empty(&p_file_stat->file_area_warm_hot) ||!list_empty(&p_file_stat->file_area_writeonly_or_cold) || !list_empty(&p_file_stat->file_area_warm))
		panic("%s file_stat:0x%llx status:0x%x  list nor empty\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_base.file_stat_status);

	kmem_cache_free(hot_cold_file_global_info.file_stat_cachep,p_file_stat);
}

/** 引入多层warm链表和global_file_stat新方案后，global_file_stat的file_area在cold_file_area_delete()时，如果被该file_area
 * 所属文件被iput()释放了，那cold_file_area_delete()里就不能再使用该文件的mmaping了，非法内存访问。具体细节如下：
         *普通的文件file_stat，当iput()释放该文件inode时，会把file_stat移动到global delete链表，然后由异步内存回收线程
		 * 遍历global delete链表，释放该file_stat的所有file_area。但是针对global_file_stat的file_area，当iput()释放这类
		 * 文件inode时，因为没有file_stat，就无法再由异步内存回收线程遍历该文件的file_stat释放file_area了。于是想了一个
		 * 办法，把file_area->pages[0、1]的内存作为file_area_delete链表，把该file_area移动到global_file_stat.file_area_delete_list
		 * 链表，不用担心并发问题，因为异步内存回收线程只会依照file_area->file_area_list链表把file_area移动到各个file_stat
		 * 链表。然后由异步内存回收线程遍历global_file_stat.file_area_delete_list，释放该链表上的file_area。但是，此时
		 * 有个并发问题，如果这里iput()把file_area移动到global_file_stat.file_area_delete_list链表时，正好异步内存回收
		 * 线程正因为file_area长时间没访问而cold_file_area_delete()释放掉。然后这个已经释放的file_area依然保存在
		 * global_file_stat.file_area_delete_list链表，之后异步内存回收线程会再释放该file_ara，那就是非法内存访问了。
		 * 为了防护这个并发问题，主要使用上边的xas_lock_irq()加锁，从xrray tree遍历该file_area，如果返回值old_entry
		 * 是NULL，说明该file_area已经被异步内存回收线程释放了，那这里就不能再把file_area移动到
		 * global_file_stat.file_area_delete_list链表了。

怎么防护这个并发问题呢？异步内存回收线程在cold_file_area_delete()要防护file_area所属的文件mappiing不能被iput()释放了，
否则xas_store(&xas, NULL)遍历mmaping的xarray tree就是非法内存释放。iput()->find_get_entry_for_file_area()要防护file_area
不能cold_file_area_delete()释放了，否则也是非法内存访问。这个并发case算是双非法内存防护了

思考的过程很麻烦，但是最终还是借助原有find_get_entry_for_file_area、cold_file_area_delete 框架，借助xas锁完美的解决了这个并发问题

//iput()进程释放文件的inode和mapping，要确保file_area结构不能被异步内存回收线程释放
iput()->find_get_entry_for_file_area
    //rcu宽限期内，确保该文件的inode和mapping不会被iput()的进程释放。以确保下边可以放心使用file_area，不会被异步内存回收线程释放掉
    rcu_read_lock();
	//设置mapping exit
    mapping_set_exiting(mapping);
	//从xrray tree查找file_area，此时因为有rcu_read_lock守护，不用担心file_area被异步内存回收线程在cold_file_area_delete函数释放掉。
    *p_file_area = xas_find(xas, file_area_max);
	
	if(!file_area_have_page(*p_file_area) && mapping_exiting(mapping)){
		(*p_file_area)->mapping = NULL;
		//内存屏障，确保file_area->mapping=0先生效
		smp_wmb();
		//xas加锁，如果file_area被异步内存回收线程在cold_file_area_delete()被释放了，返回值old_entry就是NULL
		xas_lock_irq(&xas_del);
		//该文件要被iput()释放，于是从xrray tree剔除该file_area。如果返回old_entry为NULL，说明该file_area被cold_file_area_delete()已经从xrray tree剔除xrray tre了
		old_entry = xas_store(&xas_del, NULL);
		xas_unlock_irq(&xas_del);
		if(old_entry)
			list_move(&(*p_file_area)->file_area_delete,&hot_cold_file_global_info.global_file_stat.file_area_delete_list);
	}
	rcu_read_unlock()
	//rcu异步释放inode和mapping内存
	call_rcu(&inode->i_rcu, i_callback);

//异步内存回收线程释放file_area，但此时文件mapping结构不能被iput()进程释放
cold_file_area_delete->cold_file_area_delete_lock
    //rcu宽限期内，确保该文件的inode和mapping不会被iput()的进程释放。以确保下边xas_store(&xas, NULL)从mapping的xarray tree查找file_area时，mapinig内存不会被iput()释放掉
    rcu_read_lock()
	//内存屏障，确保iput->find_get_entry_for_file_area标记file_area->mapping=NULL立即感知到，然后直接return -1。此时该文件inode和mapping已经被被释放了，不能再使用mapping了
    smp_mb();
	//如果file_area->maping是NULL，说明file_area在iput()->find_get_entry_for_file_area()已经被移动到global_file_stat.file_area_delete_list链表，
	//直接返回。后续再由异步内存回收线程遍历该链表释放该file_area
	if(NULL == p_file_area->mapping)
		return -1;
	
	//xas加锁，如果file_area在iput()->find_get_entry_for_file_area()中被并发剔除xrray tree了，下边xas_store(&xas, NULL)返回NULL，然后就不会执行call_rcu()释放file_area了
    xas_lock_irq(&xas);
	//从xrray tree剔除file_area
	old_file_area = xas_store(&xas, NULL);
	xas_unlock_irq(&xas);
	
	rcu_read_unlock();
	//rcu异步释放file_area
	if(old_file_area)
		call_rcu(&p_file_area->i_rcu, i_file_area_callback);

	总结下这个并发防护的细节
1：iput()->find_get_entry_for_file_area原有的框架，就有rcu_read_lock后xas_find(xas, file_area_max)从xrray tree查找file_area，
   如果查找到file_area，就不用担心file_area被cold_file_area_delete释放了，因为rcu_read_lock防护。
2：iput()->find_get_entry_for_file_area里，先后执行file_area->mapping = NULL；smp_wmb；call_rcu(&inode->i_rcu, i_callback)。
   如果此时异步内存回收线程并发执行cold_file_area_delete()，是rcu_read_lock;smp_mb;if(file_area->maping == NULL) return -1。
   如果感知到file_area->mapping是NULL，直接return，不再使用maping。如果file_area->maping不是NULL，但是
   iput()->find_get_entry_for_file_area里已经执行了file_area->mapping = NULL赋值，说明还没有执行smp_wmb。故此时
   cold_file_area_delete里可以放心使用mapping，因为此时已经进入rcu_read_lock防护。
3：其实有个更麻烦的问题，因为iput()->find_get_entry_for_file_area里，针对global_file_stat的file_area，正常是要把file_area移动到
   global_file_stat.file_area_delete_list链表，然后再由异步内存回收线程遍历该链表释放file_area。但是如果
   在把file_area移动到global_file_stat.file_area_delete_list链表时，异步内存回收线程正在cold_file_area_delete()释放该file_area。
   就有了并发操作，后续已经释放的file_area结构还保存在global_file_stat.file_area_delete_list链表，会被异步内存回收线程二次释放。
   为了防护这个问题借助cold_file_area_delete和iput()->find_get_entry_for_file_area函数里，都会执行的xas_lock_irq加锁后
   old_entry = xas_store(&xas_del, NULL)把file_area从xrray tree剔除。因为有xas_lock_irq加锁，如果file_area先在
   cold_file_area_delete里被call_rcu(&p_file_area->i_rcu, i_file_area_callback)释放了，那iput()->find_get_entry_for_file_area
   里执行old_entry = xas_store(&xas_del, NULL)时，返回值是NULL，就不会再把file_area移动到global_file_stat.file_area_delete_list
   链表了。因为这个并发有xas_lock_irq加锁防护，就很简单了
 */

/*在判定一个file_area长时间没人访问后，执行该函数delete file_area。必须考虑此时有进程正好要并发访问这个file_area*/
static inline int cold_file_area_delete_lock(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base * p_file_stat_base,struct file_area *p_file_area)
{
#ifdef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY	
	XA_STATE(xas, &((struct address_space *)(p_file_stat_base->mapping))->i_pages, -1);
#else	
	//XA_STATE(xas, &((struct address_space *)(p_file_stat_base->mapping))->i_pages, p_file_area->start_index >> PAGE_COUNT_IN_AREA_SHIFT);
	XA_STATE(xas, NULL, p_file_area->start_index);
#endif
	void *old_file_area = NULL;
	struct address_space *mapping;
	char is_global_file_stat = file_stat_in_global_base(p_file_stat_base);

	if(file_area_in_deleted(p_file_area))
		panic("%s file_stat:0x%llx  file_area:0x%llx status:0x%x mapping:0x%llx\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)p_file_area->mapping);

	/* 先rcu_read_lock，再smp_mb，再if(0 == p_file_area->mapping) return。如果if成立，后续就能放心使用mapping以及xas_lock，
	 * 不用担心该文件的inode和maping此时被释放了。*/
	rcu_read_lock();

	smp_mb();
	if(0 == READ_ONCE(p_file_area->mapping) || file_area_in_mapping_exit(p_file_area)){
		printk("%s file_stat:0x%llx  file_area:0x%llx status:0x%x mapping:0x%llx in_exit !!!!!!!\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)p_file_area->mapping);
		rcu_read_unlock();
		return -1;
	}
	if(is_global_file_stat)
		mapping = p_file_area->mapping;
	else
		mapping = p_file_stat_base->mapping;

	xas.xa_index = p_file_area->start_index;
	xas.xa = &mapping->i_pages;
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
		rcu_read_unlock();
		printk("%s file_area:0x%llx file_area_state:0x%x\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
		/*那就把它再移动回file_stat->file_area_temp链表头。有这个必要吗？没有必要的!因为该file_area是在file_stat->file_area_free链表上，如果
		  被访问了而执行hot_file_update_file_status()函数，会把这个file_area立即移动到file_stat->file_area_temp链表，这里就没有必要做了!!!!!*/
		xas_unlock_irq(&xas);
		return 1;
	}
#ifdef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY
	/*p_file_area->pages[0/1]的bit63必须是file_area的索引，非0。而p_file_area->pages[2/3]必须是0，否则crash 
	 * 最新方案，file_area->pages[0/1]不是file_area_inde也不是shadow时，才会触发crash。file_area->pages[2/3]可能是NULL或者shasow，其他情况触发crash*/
	//if(!folio_is_file_area_index(p_file_area->pages[0]) || !folio_is_file_area_index(p_file_area->pages[1]) || p_file_area->pages[2] || p_file_area->pages[3]){
	if(!folio_is_file_area_index_or_shadow(p_file_area->pages[0]) || !folio_is_file_area_index_or_shadow(p_file_area->pages[1]) 
			|| (p_file_area->pages[2] && !(file_area_shadow_bit_set & (u64)(p_file_area->pages[2]))) || (p_file_area->pages[3] && !(file_area_shadow_bit_set & (u64)(p_file_area->pages[3])))){
		for (int i = 0;i < PAGE_COUNT_IN_AREA;i ++)
			printk("pages[%d]:0x%llx\n",i,(u64)(p_file_area->pages[i]));

		panic("%s file_stat:0x%llx file_area:0x%llx pages[] error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area);
	}
	/*file_stat tiny模式，为了节省内存把file_area->start_index成员删掉了。但是在file_area的page全释放后，
	 *会把file_area的索引(file_area->start_index >> PAGE_COUNT_IN_AREA_SHIFT)保存到p_file_area->pages[0/1]里.
	 *于是现在从p_file_area->pages[0/1]获取file_area的索引*/
	xas.xa_index = get_file_area_start_index(p_file_area);
#endif
	/* 如果p_file_area->mapping是NULL，说明此时该文件并发被iput()，截断文件pagecache执行的find_get_entry_for_file_area函数，
	 * 或者文件iput()流程，kswapd执行page_cache_delete_for_file_area函数里，并发把p_file_area->mapping设置NULL，并
	 * 把file_area从xarray tree剔除，这里就不再重复剔除，也不能释放file_area，因为它被移动到了global_file_stat_delete链表*/
	if(READ_ONCE(p_file_area->mapping)){
		/*从xarray tree剔除。注意，xas_store不仅只是把保存file_area指针的xarray tree的父节点xa_node的槽位设置NULL。
		 *还有一个隐藏重要作用，如果父节点node没有成员了，还是向上逐级释放父节点xa_node，直至xarray tree全被释放*/
		old_file_area = xas_store(&xas, NULL);
	}else
		printk("%s file_stat:0x%llx file_area:0x%llx has move to global_file_stat_delete list\n",__func__,(u64)p_file_stat_base,(u64)p_file_area);

	/* 
	 * 在查看page_cache_delete()源码时，突然有个灵感，如果file_area在iput()和异步内存回收线程同时执行
	 * xas_store(xas,NULL)，会不会有问题？二者全程有加锁，没有并发问题，但有其他问题。重大隐藏bug!!!!!!
	 * 
	 * 当该文件的file_area0在内存回收后，没有爬个了，处于in_free链表。然后这个文件又被访问了分配了page0.
	 * 但是因为系统文件太多，导致过了很长时间都没有遍历到这个文件，导致file_area的age与global age相差很大,
	 * 时冷page。
	 *
	 * 此时，这个文件被iput()释放，设置该文件mapping的exit标记。然后执行
	 * truncate_inode_pages_range->delete_from_page_cache_batch->page_cache_delete_batch_for_file_area()
	 * 释放page0，因为此时mapping有exit标记，也会执行xas_store(xas,NULL)把该file_area从xarray tree剔除，
	 * 这个过程是xas_lock_irq(&xas)加锁的，不用担心并发问题。但是，问题来了，还没有执行最后的
	 * destroy_inode->__destroy_inode_handler_post()把该文件file_stat移动到global delete链表。
	 *
	 * 此时异步内存回收线程遍历到这个文件，这个文件有file_area0，但是因长时间
	 * 没有访问，且file_area0没有page，就要执行cold_file_area_delete_lock()释放该file_area0。于是执行上边的
	 * old_file_area = xas_store(&xas, NULL)，但是该file_area已经从xarray tree剔除了，于是在xarray tree
	 * 查不到file_area而返回0，下边的if((NULL == old_file_area)成立而触发panic。这个bug跟时机紧密相关，
	 * 但确实存在。解决办法很简单，就是如果文件mapping有exit标记，就不再panic了
	 *
	 * 不对，异步内存回收线程在扫描每一个文件时，都要先对文件inode加锁，之后是无法对该文件inode执行iput()
	 * 释放pagecache的。上边的分析不存在，但是为了安全考虑，这个判断还是加上吧
	 */
	if((NULL == old_file_area))
	{
		//if(mapping_exiting(p_file_stat_base->mapping))
		if(mapping_exiting(mapping)){
			pr_warn("%s file_stat:0x%llx file_area:0x%llx find folio error:%ld\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,xas.xa_index);
			if(NULL != p_file_area->mapping)
				panic("%s file_stat:0x%llx file_area:0x%llx index:%ld mapping != NULL\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,xas.xa_index);
		}	
		else
			panic("%s file_stat:0x%llx file_area:0x%llx find folio error:%ld\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,xas.xa_index);
	}

	if (xas_error(&xas)){
		rcu_read_unlock();

		printk("%s xas_error:%d  !!!!!!!!!!!!!!\n",__func__,xas_error(&xas));
		xas_unlock_irq(&xas);
		return -1;
	}
	xas_unlock_irq(&xas);

	/*到这里，一定可以确保file_area已经从xarray tree剔除，但不能保证不被其他进程在filemap_get_read_batch()或mapping_get_entry()中，
	 *在file_area已经从xarray tree剔除前已经并发访问了file_area，现在还在使用，所以要rcu延迟释放file_area结构*/

	spin_lock(&p_file_stat_base->file_stat_lock);
	if(0 == p_file_stat_base->file_area_count && !is_global_file_stat)
		panic("%s file_stat:0x%llx file_area:0x%llx file_area_count == 0 error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area);
	/*该文件file_stat的file_area个数减1，把file_area从file_stat的链表剔除，这个过程要加锁*/
	p_file_stat_base->file_area_count --;
	list_del_rcu(&p_file_area->file_area_list);
	spin_unlock(&p_file_stat_base->file_stat_lock);
	/*要释放的file_area清理掉in_free标记，表示该file_area要被释放了*/
	clear_file_area_in_free_list(p_file_area);

	/* p_file_area->file_area_delete赋值NULL，如果i_file_area_callback()释放该file_area时，发现它不是NULL，说明之后iput时，该file_area
	 * 又被移动到global_file_stat.file_area_delete_list链表了，那说明程序出问题，要主动panic。但是要有前提限制，就是old_file_area非
	 * NULL，该file_area没有被iput()从xrray tree剔除且把file_area移动到global_file_stat_delete链表*/
	/*if(old_file_area){
		p_file_area->file_area_delete.prev = NULL;
		p_file_area->file_area_delete.next = NULL;
	}*/

	rcu_read_unlock();

	/*rcu延迟释放file_area结构*/
	/* 如果到这里时old_file_area是NULL，说明file_area已经先被iput()进程从xrray tree剔除而移动到
	 * global_file_stat file_area_delete_list链表了，这个file_area已经无效了，这里不能再call_rcu释放file_area。
	 * 否则会double free。后续异步内存回收线程会cold_file_area_delete_quick()释放该file_area.*/
	if(old_file_area){
		call_rcu(&p_file_area->i_rcu, i_file_area_callback);
		//set_file_area_in_deleted(p_file_area);
	}else
		printk("%s file_area:0x%llx delete fail !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area);

	if(shrink_page_printk_open1)
		FILE_AREA_PRINT("%s file_area:0x%llx delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area);

	p_hot_cold_file_global->cold_file_area_delete_count ++;
	return 0;
}
/*返回值 0:file_area正常释放掉  1:file_area有page无法释放  -1:global_file_stat链表上的file_area被iput()了
 * */
int cold_file_area_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area)
{
	/* 释放file_area，要对xarray tree对应槽位赋值NULL，因此必须保证inode不能被并发释放，要加锁。但是不能返回-1，因为
	 * 返回-1是说明该file_area在释放时又分配page了，要设置该file_area in_refault。如果该文件inode被释放了，会在iput()
	 * 把file_stat移动到global detele链表处理，更不能返回-1导致设置file_area in_refault，其实设置了好像也没事!!!!!!!*/

#if 0//这个加锁放到遍历file_stat内存回收，最初执行的get_file_area_from_file_stat_list()函数里了，这里不再重复加锁
	if(file_inode_lock(p_file_stat_base) <= 0)
		return 0;
#endif
	return cold_file_area_delete_lock(p_hot_cold_file_global,p_file_stat_base,p_file_area);

#if 0
	file_inode_unlock(p_file_stat_base);
#endif
	//return 0;
}
EXPORT_SYMBOL(cold_file_area_delete);

/*在文件inode被iput释放后，执行该函数释放该文件file_stat的所有file_area，此时肯定没进程再访问该文件的file_stat、file_area，不用考虑并发。
 *错了，此时可能异步内存线程也会因这个文件长时间空闲而释放file_stat和file_area。又错了，当前函数就是异步内存回收线程里执行的，没这种情况*/
int cold_file_area_delete_quick(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area)
{
	//XA_STATE(xas, &((struct address_space *)(p_file_stat->mapping))->i_pages, p_file_area->start_index >> PAGE_COUNT_IN_AREA_SHIFT);

	if(file_area_in_deleted(p_file_area))
		panic("%s file_stat:0x%llx  file_area:0x%llx status:0x%x mapping:0x%llx\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)p_file_area->mapping);

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

	if(!file_stat_in_global_base(p_file_stat_base)){
		//spin_lock(&p_file_stat->file_stat_lock);
		if(0 == p_file_stat_base->file_area_count)
			panic("%s file_stat:0x%llx file_area:0x%llx file_area_count == 0 error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area);
		/*该文件file_stat的file_area个数减1，把file_area从file_stat的链表剔除，这个过程要加锁*/
		p_file_stat_base->file_area_count --;
		list_del_rcu(&p_file_area->file_area_list);
		//spin_unlock(&p_file_stat->file_stat_lock);
	}else{
		/* global_file_stat的file_area，可能在global_file_stat->temp链表上时，所属文件就iput了。因此该file_area被释放时，
		 * 必要要spin_lock加锁把file_area从global_file_stat->temp链表被剔除。如果file_area已经被从file_stat->temp、free、
		 * warm链表剔除，file_area->file_area_list.next是LIST_POISON1，不能再重复list_del。这个问题隐藏的非常深，后续即便该方案不用了也得谨记*/

		/* 最新方案，iput()->find_get_entry_for_file_area()不再把file_area以file_area->file_area_delete移动到global_file_stat_delete
		 * 链表，而只是标记file_area的in_mapping_delete。异步内存回收线程看到file_area有in_maping_delete标记，再把file_area以
		 * file_area_list移动到global_file_stat_delete链表，此时没有并发问题。详细原因见find_get_entry_for_file_area()。因此，
		 * 即便是global_file_stat->temp链表上的file_area，也是先由异步内存回收线程移动到global_file_stat->warm、hot链表(此时是加锁的)，
		 * 但是，后续再操作这些file_area，比如移动到global_file_stat_delete链表，不用加锁，这里剔除file_area，也不用加锁，因为这些
		 * file_area不会在global_file_stat->temp链表!!!!!!!!!!*/
#if 0	
		if(p_file_area->file_area_list.next != LIST_POISON1 && p_file_area->file_area_list.prev != LIST_POISON2){
			spin_lock(&p_file_stat_base->file_stat_lock);
			list_del_rcu(&p_file_area->file_area_list);
			p_file_stat_base->file_area_count --;
			spin_unlock(&p_file_stat_base->file_stat_lock);
		}
#else
		list_del_rcu(&p_file_area->file_area_list);
		p_file_stat_base->file_area_count --;
#endif		
	}

	/*要释放的file_area清理掉in_free标记，表示该file_area要被释放了*/
	clear_file_area_in_free_list(p_file_area);

	/*隐藏重点!!!!!!!!!!!，此时可能有进程正通过proc查询该文件的file_stat、file_area、page统计信息，正在用他们。因此也不能
	 *kmem_cache_free()直接释放该数据结构，也必须得通过rcu延迟释放，并且，这些通过proc查询的进程，必须得先rcu_read_lock，
	 再查询file_stat、file_area、page统计信息，保证rcu_read_unlock前，他们不会被释放掉*/
	//kmem_cache_free(hot_cold_file_global_info.file_area_cachep,p_file_area);

	/*p_file_area->file_area_delete.prev = NULL;
	p_file_area->file_area_delete.next = NULL;*/

	/*rcu延迟释放file_area结构*/
	call_rcu(&p_file_area->i_rcu, i_file_area_callback);
	//set_file_area_in_deleted(p_file_area);

	if(shrink_page_printk_open1)
		FILE_AREA_PRINT("%s file_area:0x%llx delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area);

	return 0;
}

/*在判定一个文件file_stat的page全部被释放，然后过了很长时间依然没人访问，执行该函数delete file_stat。必须考虑此时有进程并发访问该文件file_stat*/
int cold_file_stat_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base_del,unsigned int file_type)
{
	//struct file_stat *p_file_stat_del;
	//struct file_stat_small *p_file_stat_small_del;
	//struct file_stat_tiny_small *p_file_stat_tiny_small_del;

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
	
	/* cold_file_stat_delete()还需要对inode加锁吗，因为已经在异步内存回收线程遍历file_stat，探测内存回收而执行的get_file_area_from_file_stat_list()
	 * 函数执行file_inode_lock()过了。需要的，因为会执行这个函数的，异步内存回收线程还会在file_stat_has_zero_file_area_manage()
	 * 函数，对所有的零个file_area的file_stat进行探测，然后释放file_stat，这中file_stat跟异步内存回收线程进行内存回收的file_stat是两码事*/
	if(file_inode_lock(p_file_stat_base_del) <= 0)
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
		if(file_stat_in_delete_base(p_file_stat_base_del)){
			printk("%s %s %d file_stat:0x%llx status:0x%x in delete\n",__func__,current->comm,current->pid,(u64)p_file_stat_base_del,p_file_stat_base_del->file_stat_status);
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
	if(!file_stat_in_delete_base(p_file_stat_base_del)/*p_file_stat_base_del->mapping*/){
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
		smp_wmb();
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
	if(file_stat_in_cache_file_base(p_file_stat_base_del)){
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
		if(!file_stat_in_delete_base(p_file_stat_base_del)){
			/* 在这个加个内存屏障，保证前后代码隔离开。即file_stat有delete标记后，inode->i_mapping->rh_reserved1一定是0，
			 * p_file_stat->mapping一定是NULL*/
			smp_wmb();
			/*下边的call_rcu()有smp_mb()，保证set_file_stat_in_delete()后有内存屏障*/
			set_file_stat_in_delete_base(p_file_stat_base_del);
			/*这个内存屏障解释见print_file_stat_all_file_area_info_write()*/
			smp_mb();
			/* 从global的链表中剔除该file_stat，这个过程需要加锁，因为同时其他进程会执行hot_file_update_file_status()
			 * 向global的链表添加新的文件file_stat*/
			list_del_rcu(&p_file_stat_base_del->hot_cold_file_list);
			file_stat_del = 1;
			/*file_stat个数减1*/
			hot_cold_file_global_info.file_stat_count --;
		}
		else{
			mapping->rh_reserved1 = 0;
			p_file_stat_base_del->mapping = NULL;
			/*避免spin lock时有printk打印*/
			spin_unlock_irq(&hot_cold_file_global_info.global_lock);
			printk("%s p_file_stat:0x%llx status:0x%x already delete !!!!!!!!!!!\n",__func__,(u64)p_file_stat_base_del,p_file_stat_base_del->file_stat_status);
			goto err;
		}
		spin_unlock_irq(&p_hot_cold_file_global->global_lock);
	}else{
		spin_lock_irq(&p_hot_cold_file_global->mmap_file_global_lock);
		if(!file_stat_in_delete_base(p_file_stat_base_del)){
			/*在这个加个内存屏障，保证前后代码隔离开。即file_stat有delete标记后，inode->i_mapping->rh_reserved1一定是0，p_file_stat->mapping一定是NULL*/
			smp_wmb();
			set_file_stat_in_delete_base(p_file_stat_base_del);
			smp_mb();
			//从global的链表中剔除该file_stat，这个过程需要加锁，因为同时其他进程会执行hot_file_update_file_status()向global的链表添加新的文件file_stat
			list_del_rcu(&p_file_stat_base_del->hot_cold_file_list);
			file_stat_del = 1;
			//file_stat个数减1
			hot_cold_file_global_info.mmap_file_stat_count --;
		}
		else{
			mapping->rh_reserved1 = 0;
			p_file_stat_base_del->mapping = NULL;
			/*避免spin lock时有printk打印*/
			spin_unlock_irq(&hot_cold_file_global_info.mmap_file_global_lock);
			printk("%s p_file_stat:0x%llx status:0x%x already delete!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_stat_base_del,p_file_stat_base_del->file_stat_status);
			goto err;
		}
		spin_unlock_irq(&p_hot_cold_file_global->mmap_file_global_lock);
	}

err:
	
	/* 在释放file_stat时，标记print_file_stat为NULL，这个赋值必须放到标记file_stat in_delete后边。
	 * 作用是，保证print_file_stat_all_file_area_info()看到file_stat有in_delete标记后，就不能再把
	 * file_stat赋值给print_file_stat了。否则访问print_file_stat就是无效的内存了*/
	if(p_file_stat_base_del == hot_cold_file_global_info.print_file_stat){
		hot_cold_file_global_info.print_file_stat = NULL;
		printk("%s p_file_stat:0x%llx status:0x%x print_file_stat delete!!!\n",__func__,(u64)p_file_stat_base_del,p_file_stat_base_del->file_stat_status);
	}

	while(atomic_read(&hot_cold_file_global_info.ref_count))
		schedule();

	/* rcu延迟释放file_stat结构。call_rcu()里有smp_mb()内存屏障。但如果mapping->rh_reserved1是0了，说明上边
	 * 没有执行list_del_rcu(&p_file_stat_del->hot_cold_file_list)，那这里不能执行call_rcu()*/
	if(mapping->rh_reserved1){
			
		/*file_stat被rcu异步释放了，但是提前rcu_read_lock了，这里置1，等到确定file_stat不会被释放再rcu_read_unlock放开。
		 *注意，这个rcu_read_lock()很重要。因为调用cold_file_stat_delete()的地方，还会使用该file_stat。因此这里这里
		 *不能立即释放掉file_stat，rcu_read_lock()就保证call_rcu()释放file_stat后，不会立即释放掉file_stat.*/
		rcu_read_lock();

		if(FILE_STAT_NORMAL == file_type){
			//if(get_file_stat_type(p_file_stat_base_del) != FILE_STAT_NORMAL)
			if(file_stat_in_global_base(p_file_stat_base_del))
				panic("%s file_stat:0x%llx not normal file_stat\n",__func__,(u64)p_file_stat_base_del);

			//p_file_stat_del = container_of(p_file_stat_base_del,struct file_stat,file_stat_base);
			//call_rcu(&p_file_stat_del->i_rcu, i_file_stat_callback);
			call_rcu(&p_file_stat_base_del->i_rcu, i_file_stat_callback);
		}else if(FILE_STAT_SMALL == file_type){
			//p_file_stat_small_del = container_of(p_file_stat_base_del,struct file_stat_small,file_stat_base);
			//call_rcu(&p_file_stat_small_del->i_rcu, i_file_stat_small_callback);
			call_rcu(&p_file_stat_base_del->i_rcu, i_file_stat_small_callback);
		}
		else if(FILE_STAT_TINY_SMALL == file_type){
			//p_file_stat_tiny_small_del = container_of(p_file_stat_base_del,struct file_stat_tiny_small,file_stat_base);
			//call_rcu(&p_file_stat_tiny_small_del->i_rcu, i_file_stat_tiny_small_callback);
			call_rcu(&p_file_stat_base_del->i_rcu, i_file_stat_tiny_small_callback);
		}else
			BUG();
	}

	//rcu_read_unlock();
	file_inode_unlock_mapping(mapping);


	FILE_AREA_PRINT("%s file_stat:0x%llx delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_stat_base_del);

	return ret;
}
EXPORT_SYMBOL(cold_file_stat_delete);

/*在文件inode被iput释放后，执行该函数释放该文件file_stat，此时肯定没进程再访问该文件，不用考虑并发*/
static noinline int cold_file_stat_delete_quick(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base_del,unsigned int file_type)
{
	//struct file_stat *p_file_stat_del;
	//struct file_stat_small *p_file_stat_small_del;
	//struct file_stat_tiny_small *p_file_stat_tiny_small_del;

	/*二者先前已经在__destroy_inode_handler_post()处理过，不可能成立*/
	if(!file_stat_in_delete_base(p_file_stat_base_del))
		panic("file_stat_del:0x%llx status:0x%x!!!!!!!!\n",(u64)p_file_stat_base_del,p_file_stat_base_del->file_stat_status);

	/*使用global_lock加锁是因为要把file_stat从p_hot_cold_file_global的链表中剔除，防止此时其他进程并发向p_hot_cold_file_global的链表添加file_stat,
	 *是否有必要改为 spin_lock_irqsave，之前就遇到过用spin_lock_irq导致打乱中断状态而造成卡死?????????????????????????????????????????????*/
	if(file_stat_in_cache_file_base(p_file_stat_base_del)){
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
		if(get_file_stat_type(p_file_stat_base_del) != FILE_STAT_NORMAL)
			panic("%s file_stat:0x%llx not normal file_stat\n",__func__,(u64)p_file_stat_base_del);

		//p_file_stat_del = container_of(p_file_stat_base_del,struct file_stat,file_stat_base);
		call_rcu(&p_file_stat_base_del->i_rcu, i_file_stat_callback);
	}
	else if (FILE_STAT_SMALL == file_type){
		//p_file_stat_small_del = container_of(p_file_stat_base_del,struct file_stat_small,file_stat_base);
		call_rcu(&p_file_stat_base_del->i_rcu, i_file_stat_small_callback);
	}
	else if(FILE_STAT_TINY_SMALL == file_type){
		//p_file_stat_tiny_small_del = container_of(p_file_stat_base_del,struct file_stat_tiny_small,file_stat_base);
		call_rcu(&p_file_stat_base_del->i_rcu, i_file_stat_tiny_small_callback);
	}else
		BUG();

	FILE_AREA_PRINT("%s file_stat:0x%llx delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_stat_base_del);

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
		//struct file_stat *p_file_stat = NULL;
		//struct file_stat_small *p_file_stat_small = NULL;
		//struct file_stat_tiny_small *p_file_stat_tiny_small = NULL;
		struct file_stat_base *p_file_stat_base = (struct file_stat_base *)inode->i_mapping->rh_reserved1;

		unsigned int file_stat_type = get_file_stat_type_file_iput(p_file_stat_base);

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
#if 0
		/*这个赋值移动到该函数最后边。一定得放到标记file_stat in_delete前边吗？会不会跟异步内存回收线程此时正并发
		 *cold_file_stat_delete()释放该file_stat，存在同时修改该file_stat的问题？不会，cold_file_stat_delete()会
		 file_inode_lock()对inode加锁释放而直接返回，无法再释放该file_stat*/
		inode->i_mapping->rh_reserved1 = 0;

		/*这里把p_file_stat_base->mapping设置成NULL，会导致异步内存回收线程遍历各个global temp等链表的file_stat时，使用is_file_stat_mapping_error()判断
		 *p_file_stat_base->mapping->rh_reserved1跟file_stat是否匹配时因p_file_stat_base->mapping是NULL，而crash。
		 *目前的做法是把这个赋值放到了cold_file_delete_stat()函数里*/
		//p_file_stat_base->mapping = NULL;

		/*在这个加个内存屏障，保证前后代码隔离开。即file_stat有delete标记后，inode->i_mapping->rh_reserved1一定无效，p_file_stat->mapping一定是NULL*/
		smp_wmb();
#endif
		if(file_stat_in_cache_file_base(p_file_stat_base)){

			/*该文件的file_area都移动到了global_file_stat的链表了，直接goto err，把mapping->rh_reserved1清0。*/
			if(inode->i_mapping->rh_reserved1 == (u64)(&hot_cold_file_global_info.global_file_stat.file_stat.file_stat_base)){
                goto err;
			}
			/* 又遇到一个重大的隐藏bug。如果当前文件文件页page全释放后还是长时间没访问，此时异步内存回收线程正好执行
			 * cold_file_stat_delete()释放file_stat，并global_lock加锁，标记file_stat delete，然后list_del(p_file_stat)。
			 * 然后，这里global_lock加锁，再执行list_move(&p_file_stat->hot_cold_file_list,...)那就出问题了，因为此时
			 * 此时p_file_stat已经在cold_file_stat_delete()中list_del(p_file_stat)从链表中剔除了。这个bug明显违反了
			 * 一个我之前定下的一个原则，必须要在spin_lock加锁后再判断一次file_area和file_stat状态，是否有变化，
			 * 因为可能在spin_lock加锁前一瞬间状态已经被修改了!!!!!!!!!!!!!!!!!!!!!!!*/
			spin_lock_irq(&hot_cold_file_global_info.global_lock);
			/*存在极端情况，iput()时，异步内存回收线程正把cache文件转成mmap文件，这里成功加锁后可能把cache file_stat转成mmap文件了，此时就要走mmap文件分支处理了*/
			if(!file_stat_in_cache_file_base(p_file_stat_base)){
			    spin_unlock_irq(&hot_cold_file_global_info.global_lock);
				printk("%s p_file_stat:0x%llx status:0x%x cache change to mmap!!!!!!!\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
                goto mmap_file_solve;
			}
			if(!file_stat_in_delete_base(p_file_stat_base)){
				/*file_stat不一定只会在global temp链表，也会在global hot等链表，这些标记不清理了*/
				//clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
				/* 注意，file_stat有delete标记，不能说明file_stat移动到了global delete链表。file_stat有in_delete_file
				 * 标记才能说明file_stat被iput()移动到了global delete链表*/
				set_file_stat_in_delete_base(p_file_stat_base);

				/*尝试test_and_set_bit(file_stat_delete_protect)令变量置1，如果成功，异步内存回收线程无法再从global temp链表
				 *获取到file_stat，否则这个file_stat将在这里被从global temp链表移动到global delete链表。这样获取到的file_stat就
				 *不再是global temp链表的了，会出大问题。如果这里令变量置1失败，则只是设置file_stat的in_delete标记*/
				if(file_stat_delete_protect_try_lock(1)){
					/*只有file_stat被移动到global delete标记才会设置in_delete_file标记*/
					set_file_stat_in_delete_file_base(p_file_stat_base);
					if(FILE_STAT_NORMAL == file_stat_type){
						//p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
						//list_move(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.file_stat_delete_head);
						list_move(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_delete_head);
					}
					else if(FILE_STAT_SMALL == file_stat_type){
						//p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
						//list_move(&p_file_stat_small->hot_cold_file_list,&hot_cold_file_global_info.file_stat_small_delete_head);
						list_move(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_small_delete_head);
					}
					else{
						if(FILE_STAT_TINY_SMALL != file_stat_type)
							panic("%s file_stat:0x%llx status:0x%x file_stat_type:0x%x error\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,file_stat_type);

						//p_file_stat_tiny_small = container_of(p_file_stat_base,struct file_stat_tiny_small,file_stat_base);
						//list_move(&p_file_stat_tiny_small->hot_cold_file_list,&hot_cold_file_global_info.file_stat_tiny_small_delete_head);
						list_move(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_tiny_small_delete_head);
					}

					file_stat_delete_protect_unlock(1);
				}
			}else{
				/*如果到这份分支，说明file_stat先被异步内存回收线程执行cold_file_stat_delete()标记delete了，
				 *为了安全要再对inode->i_mapping->rh_reserved1清0一次，详情cold_file_stat_delete()也有解释。
				 现在用了file_inode_lock后，这种情况已经不可能了，但代码还是保留一下吧*/
				inode->i_mapping->rh_reserved1 = 0;
				/*避免spin lock时有printk打印*/
				spin_unlock_irq(&hot_cold_file_global_info.global_lock);
				printk("%s p_file_stat:0x%llx status:0x%x already delete\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
				goto err;
			}
			spin_unlock_irq(&hot_cold_file_global_info.global_lock);
		}else{
			/* 注意，会出现极端情况。so等elf文件最初被判定为cache file而在global temp链表，然后
			 * cache_file_stat_move_to_mmap_head()函数中正把该file_stat移动到globaol mmap_file_stat_temp_head链表。会出现短暂的
			 * file_stat即没有in_cache_file也没有in_mmap_file状态，此时就会走到else分支，按照mmap文件的delete处理!!!!!!!!!!*/
mmap_file_solve:

			/*该文件的file_area都移动到了global_file_stat的链表了，直接goto err，把mapping->rh_reserved1清0。*/
			if(inode->i_mapping->rh_reserved1 == (u64)(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_stat_base)){
                goto err;
			}

			spin_lock_irq(&hot_cold_file_global_info.mmap_file_global_lock);
			if(!file_stat_in_delete_base(p_file_stat_base)){
				/*file_stat不一定只会在global temp链表，也会在global hot等链表，这些标记不清理了*/
				//clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
				set_file_stat_in_delete_base(p_file_stat_base);

				if(file_stat_delete_protect_try_lock(0)){
					set_file_stat_in_delete_file_base(p_file_stat_base);
					if(FILE_STAT_NORMAL == file_stat_type){
						//p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
						//list_move(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_delete_head);
						list_move(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_delete_head);
					}
					else if(FILE_STAT_SMALL == file_stat_type){
						//p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
						//list_move(&p_file_stat_small->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_small_delete_head);
						list_move(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_small_delete_head);
					}
					else{
						if(FILE_STAT_TINY_SMALL != file_stat_type)
							panic("%s file_stat:0x%llx status:0x%x file_stat_type:0x%x error\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,file_stat_type);

						//p_file_stat_tiny_small = container_of(p_file_stat_base,struct file_stat_tiny_small,file_stat_base);
						//list_move(&p_file_stat_tiny_small->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_tiny_small_delete_head);
						list_move(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_tiny_small_delete_head);
					}

					file_stat_delete_protect_unlock(0);
				}
			}
			else{
				inode->i_mapping->rh_reserved1 = 0;
				spin_unlock_irq(&hot_cold_file_global_info.mmap_file_global_lock);
				printk("%s p_file_stat:0x%llx status:0x%x already delete\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
				goto err;
			}
			spin_unlock_irq(&hot_cold_file_global_info.mmap_file_global_lock);
		}
err:
		/*这个内存屏障保证前边的set_file_stat_in_delete_base(p_file_stat_base)一定在inode->i_mapping->rh_reserved1 = 0前生效。
		 *原因是遍历global temp、small、tiny_small链表上的file_stat时，会执行is_file_stat_mapping_error()判断p_file_stat跟
		 *file_stat->mapping->rh_reserved1是否一致。这个内存屏障保证is_file_stat_mapping_error()里判断出if(p_file_stat != 
		 *p_file_stat->mapping->rh_reserved1)后，执行smp_rmb();if(file_stat_in_delete(p_file_stat))判断出file_stat一定有
		 *file_stat有in_delete标记*/
		smp_wmb();
		/*必须保证即便该函数走了error分支，也要执行该赋值*/
		inode->i_mapping->rh_reserved1 = 0;

		if(shrink_page_printk_open1)
			FILE_AREA_PRINT("%s file_stat:0x%llx iput delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_stat_base);
	}
}
void disable_mapping_file_area(struct inode *inode)
{
	/*如果inode->i_mapping->rh_reserved1大于1，说明是正常的支持file_area读写的文件系统的文件inode，则执行__destroy_inode_handler_post()
	 *处理inode->i_mapping。否则inode->i_mapping->rh_reserved1是1，说明是支持file_area读写的文件系统的目录inode，或者是文件inode，
	 *但是没有读写分配page，inode->i_mapping->rh_reserved1保持1。这种情况直接else分支，令inode->i_mapping->rh_reserved1清0即可。注意，
	 *这种inode绝对不能执行__destroy_inode_handler_post()，因为这种inode没有分配file_stat，不能设置file_stat in delete，强制执行
	 *会crash的*/
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(inode->i_mapping))
		__destroy_inode_handler_post(inode);
	else
		inode->i_mapping->rh_reserved1 = 0;

	/*mapping->rh_reserved1不为NULL就crash，正常情况不可能，但是尤其是引入global_file_stat后，为了安全还是加入这个NULL判断*/
	if(inode->i_mapping->rh_reserved1)
		panic("0x%llx",(u64)inode);
}
EXPORT_SYMBOL(disable_mapping_file_area);

//删除p_file_stat_del对应文件的file_stat上所有的file_area，已经对应hot file tree的所有节点hot_cold_file_area_tree_node结构。最后释放掉p_file_stat_del这个file_stat数据结构
unsigned int cold_file_stat_delete_all_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int file_type,char is_cache_file)
{
	//struct file_stat * p_file_stat,*p_file_stat_temp;
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int del_file_area_count = 0;
	struct file_stat *p_file_stat_del = NULL;
	struct file_stat_small *p_file_stat_small_del = NULL;
	//struct file_stat_tiny_small *p_file_stat_tiny_small_del = NULL;

	//temp链表
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_base->file_area_temp,file_area_list){
		/* 新的版本hot_file_update_file_status()遇到refault、hot file_area，只是做个标记，而不会把file_area移动到
		 * file_stat->refault、hot链表，因此file_stat->temp链表上的file_area除了有in_temp_list标记，还有
		 * in_refault_list、in_hot_list标记，故要把file_area_in_temp_list_error(p_file_area)判断去掉*/
		if(FILE_STAT_TINY_SMALL != file_type){
			if(!file_area_in_temp_list(p_file_area) || (file_area_in_temp_list_error(p_file_area) && !file_area_in_hot_list(p_file_area)))
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_temp\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
		}
		else{
			/* tiny_small_file->temp链表可能同时有in_temp、in_hot、in_refault属性的file_area，故不再判断。但如果file_area一个这些状态都没有，也crash
			 * 新版本in_temp属性的file_area就是没有in_hot、in_refault、in_free属性，因此加上!file_area_in_temp_list判断*/
			if(0 == (p_file_area->file_area_state & FILE_AREA_LIST_MASK) && !file_area_in_temp_list(p_file_area))
				panic("%s:1 file_stat:0x%llx file_area:0x%llx status:0x%x not in any file_area_list\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
		}

		cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_base,p_file_area);
		del_file_area_count ++;
	}

	if(FILE_STAT_SMALL == file_type){
		p_file_stat_small_del = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
		//other链表
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_small_del->file_area_other,file_area_list){
#if 0
			/*file_area_other链表上的file_area，什么属性的都有，不再做错误判断*/
			if(!file_area_in_refault_list(p_file_area) || file_area_in_refault_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_refault\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
#endif
			if(0 == (p_file_area->file_area_state & FILE_AREA_LIST_MASK))
				panic("%s:2 file_stat:0x%llx file_area:0x%llx status:0x%x not in any file_area_list\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_base,p_file_area);
			del_file_area_count ++;
		}
	}
	else if(FILE_STAT_NORMAL == file_type){

		p_file_stat_del = container_of(p_file_stat_base,struct file_stat,file_stat_base);

		/*异步内存回收线程遍历的file_stat，把file_area移动到了p_current_scan_file_stat_info->temp链表，这个文件却被iput()了，这里把file_area再移动回file_stat的链表*/
		if(file_stat_in_file_area_in_tmp_list_base(p_file_stat_base)){
			struct current_scan_file_stat_info *p_current_scan_file_stat_info;

			p_current_scan_file_stat_info = get_normal_file_stat_current_scan_file_stat_info(p_hot_cold_file_global,p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK,is_cache_file);

			if(p_current_scan_file_stat_info->p_traverse_file_stat != p_file_stat_del)
				panic("%s file_stat_del:0x%llx status:0x%x file_stat_current:0x%llx status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_current_scan_file_stat_info->p_traverse_file_stat,p_current_scan_file_stat_info->p_traverse_file_stat->file_stat_base.file_stat_status);

			update_file_stat_next_multi_level_warm_or_writeonly_list(p_current_scan_file_stat_info,p_file_stat_del); 
		}

		//refault链表------多层warm去掉
		/*list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_refault,file_area_list){
			if(!file_area_in_refault_list(p_file_area) || file_area_in_refault_list_error(p_file_area))
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_refault\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_base,p_file_area);
			del_file_area_count ++;
		}*/
		//hot链表 多层warm机制引入后，mapcount和refault的file_area都移动到file_stat->hot链表
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_hot,file_area_list){
			if((!file_area_in_hot_list(p_file_area) && !file_area_in_mapcount_list(p_file_area) && !file_area_in_refault_list(p_file_area)) || 
					(file_area_in_hot_list_error(p_file_area) && !file_area_in_mapcount_list(p_file_area) && !file_area_in_refault_list(p_file_area)))
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_hot\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_base,p_file_area);
			del_file_area_count ++;
		}
		//free链表
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_free,file_area_list){
			/*file_area链表上的file_area可能被hot_file_update_file_status()并发设置了in_refautl状态但没来的及移动到
			 * file_stat->refault链表，故这里不能判断file_area的in_free状态错误*/
			if(!file_area_in_free_list(p_file_area) || (file_area_in_free_list_error(p_file_area) && !file_area_in_refault_list(p_file_area)))
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_free\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_base,p_file_area);
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
		//mapcount链表
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_mapcount,file_area_list){
			if(!file_area_in_mapcount_list(p_file_area) || (file_area_in_mapcount_list_error(p_file_area) && !file_area_in_hot_list(p_file_area)))
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_mapcount\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_base,p_file_area);
			del_file_area_count ++;
		}
#endif	
		//file_area_warm_hot链表
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_warm_hot,file_area_list){
			//if(!file_area_in_warm_list(p_file_area) || (file_area_in_warm_list_error(p_file_area) && !file_area_in_hot_list(p_file_area)))
			if(list_num_get(p_file_area) != POS_WARM_HOT)
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_warm_cold\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_base,p_file_area);
			del_file_area_count ++;
		}
		//file_area_writeonly_or_cold链表
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_writeonly_or_cold,file_area_list){
			if(list_num_get(p_file_area) != POS_WIITEONLY_OR_COLD)
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_warm_cold\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_base,p_file_area);
			del_file_area_count ++;
		}
		//warm_cold链表
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_warm_cold,file_area_list){
			if(list_num_get(p_file_area) != POS_WARM_COLD)
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_warm_cold\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_base,p_file_area);
			del_file_area_count ++;
		}

		//warm链表,注意，该file_area可能在update函数被并发设置了in_hot标记
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_warm,file_area_list){
			//if(!file_area_in_warm_list(p_file_area) || (file_area_in_warm_list_error(p_file_area) && !file_area_in_hot_list(p_file_area)))
			if(list_num_get(p_file_area) != POS_WARM)
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_wram\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_base,p_file_area);
			del_file_area_count ++;
		}

		/*if(p_file_stat_del->file_area_count != 0){
			panic("file_stat_del:0x%llx file_area_count:%d !=0 !!!!!!!!\n",(u64)p_file_stat_del,p_file_stat_del->file_area_count);
		}*/
	}

	if(p_file_stat_base->file_area_count != 0){
		panic("file_stat_del:0x%llx file_area_count:%d !=0  statue:0x%x del_file_area_count:%d !!!!!!!!\n",(u64)p_file_stat_base,p_file_stat_base->file_area_count,p_file_stat_base->file_stat_status,del_file_area_count);
	}
	//把file_stat从p_hot_cold_file_global的链表中剔除，然后释放file_stat结构
	cold_file_stat_delete_quick(p_hot_cold_file_global,p_file_stat_base,file_type);

	return del_file_area_count;
}
static int inline is_file_stat_may_hot_file(struct file_stat *p_file_stat){
	/*热文件标准：50%以上的file_area都是热file_area，可能成为热文件*/
	if(p_file_stat->file_area_hot_count > 10 &&
			p_file_stat->file_area_hot_count >= ( p_file_stat->file_stat_base.file_area_count >> 2))
		return 1;
	else
		return 0;
}
//如果一个文件file_stat超过一定比例(比如50%)的file_area都是热的，则判定该文件file_stat是热文件，file_stat要移动到global file_stat_hot_head链表。返回1是热文件
static int inline is_file_stat_hot_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat){
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
	/*热文件标准：统一改成7/8及以上的file_area都是热的。有个bug，如果file_area_count是0，这个if依然成立，要做个file_area_hot_count大于0的限制*/
	if(p_file_stat->file_area_hot_count > 10 &&
			p_file_stat->file_area_hot_count >= (p_file_stat->file_stat_base.file_area_count - (p_file_stat->file_stat_base.file_area_count >> 3)))
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

//如果一个文件file_stat超过一定比例的file_area都是热的，则判定该文件file_stat是热文，件返回1是热文件
static int inline is_mmap_file_stat_hot_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat){
	int ret;
#if 0	
	//如果文件file_stat的file_area个数比较少，超过3/4的file_area是热的，则判定文件file_stat是热文件
	if(p_file_stat->file_area_count < p_hot_cold_file_global->mmap_file_area_level_for_large_file){
		//if(div64_u64((u64)p_file_stat->file_area_count*100,(u64)p_file_stat->file_area_hot_count) > 50)
		if(p_file_stat->file_area_hot_count >= (p_file_stat->file_area_count - (p_file_stat->file_area_count >> 2)))
			ret = 1;
		else
			ret = 0;
	}else{
		//否则，文件很大，则必须热file_area超过文件总file_area个数的7/8，才能判定是热文件，这个比例后续看具体情况调整吧
		if(p_file_stat->file_area_hot_count > (p_file_stat->file_area_count - (p_file_stat->file_area_count >> 3)))
			ret  = 1;
		else
			ret =  0;
	}
#else
    if(p_file_stat->file_area_hot_count > (p_file_stat->file_stat_base.file_area_count - (p_file_stat->file_stat_base.file_area_count >> 3)))
		ret  = 1;
	else
		ret =  0;
#endif	
	return ret;
}
//当文件file_stat的file_area个数超过阀值则判定是大文件
/*static int inline is_mmap_file_stat_large_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat)
{
	if(p_file_stat->file_area_count > hot_cold_file_global_info.mmap_file_area_level_for_large_file)
		return 1;
	else
		return 0;
}*/
static int inline is_mmap_file_stat_file_type(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base)
{
	if(p_file_stat_base->file_area_count < hot_cold_file_global_info.mmap_file_area_level_for_middle_file)
		return TEMP_FILE;
	else if(p_file_stat_base->file_area_count < hot_cold_file_global_info.mmap_file_area_level_for_large_file)
		return MIDDLE_FILE;
	else
		return LARGE_FILE;
}

//如果一个文件file_stat超过一定比例的file_area的page都是mapcount大于1的，则判定该文件file_stat是mapcount文件，件返回1是mapcount文件
static int inline is_mmap_file_stat_mapcount_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat)
{
	int ret;
#if 0
	//如果文件file_stat的file_area个数比较少，超过3/4的file_area是mapcount的，则判定文件file_stat是mapcount文件
	if(p_file_stat->file_area_count < p_hot_cold_file_global->mmap_file_area_level_for_large_file){
		//if(div64_u64((u64)p_file_stat->file_area_count*100,(u64)p_file_stat->file_area_hot_count) > 50)
		if(p_file_stat->mapcount_file_area_count >= (p_file_stat->file_area_count - (p_file_stat->file_area_count >> 2)))
			ret = 1;
		else
			ret = 0;
	}else{
		//否则，文件很大，则必须热file_area超过文件总file_area个数的7/8，才能判定是mapcount文件，这个比例后续看具体情况调整吧
		if(p_file_stat->mapcount_file_area_count > (p_file_stat->file_area_count - (p_file_stat->file_area_count >> 3)))
			ret  = 1;
		else
			ret =  0;
	}
#else
    if(p_file_stat->mapcount_file_area_count > (p_file_stat->file_stat_base.file_area_count - (p_file_stat->file_stat_base.file_area_count >> 3)))
		ret  = 1;
	else
		ret =  0;
#endif	
	return ret;
}

static int inline is_file_stat_file_type_ori(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base* p_file_stat_base)
{
	if(p_file_stat_base->file_area_count < hot_cold_file_global_info.file_area_level_for_middle_file)
		return TEMP_FILE;
	else if(p_file_stat_base->file_area_count < hot_cold_file_global_info.file_area_level_for_large_file)
		return MIDDLE_FILE;
	else
		return LARGE_FILE;
}
static int inline is_file_stat_file_type_writeonly(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base* p_file_stat_base,unsigned int file_stat_list_type)
{
	/* 普通文件按照正常的流程走，只有write page的文件，异步内存回收时就应该多回收这种文件。这种文件称为writeonly文件，
	 * 这种文件file_stat要尽可能快的转成large或middle文件，尽管file_area个数没有达到阈值。并且这种文件一旦转成
	 * large或middle文件，尽管file_area个数减少了，也要减少更多的file_area才能转为temp_file*/
	if(!file_stat_in_writeonly_base(p_file_stat_base)){
		return is_file_stat_file_type_ori(p_hot_cold_file_global,p_file_stat_base);
	}else{
		switch (file_stat_list_type){
			case F_file_stat_in_file_stat_temp_head_list:
				/*temp文件更容易变为large文件*/
				if(p_file_stat_base->file_area_count > (hot_cold_file_global_info.file_area_level_for_middle_file >> 2))
					return LARGE_FILE;
				else
					return TEMP_FILE;
				break;
			case F_file_stat_in_file_stat_middle_file_head_list:
				if(p_file_stat_base->file_area_count > hot_cold_file_global_info.file_area_level_for_large_file >> 2)
					return LARGE_FILE;
				else if(p_file_stat_base->file_area_count < hot_cold_file_global_info.file_area_level_for_middle_file >> 2)
					return TEMP_FILE;
				else
					return MIDDLE_FILE;
				break;
			case F_file_stat_in_file_stat_large_file_head_list:
				/*large文件只有file_area个数 很少很少才会降级middel或temp文件*/
				if(p_file_stat_base->file_area_count < (hot_cold_file_global_info.file_area_level_for_middle_file >> 2))
					return MIDDLE_FILE;
				else
					return LARGE_FILE;

				break;
			default:
				panic("%s p_file_stat_base:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);
		}
	}
}

/**************************************************************************************/
#if 0 //这段代码不要删除，最新版的源码做了大幅精简，这里保存原始源码供后续参照
void hot_file_update_file_status(struct address_space *mapping,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,int access_count,int read_or_write)
{
	//检测file_area被访问的次数，判断是否有必要移动到file_stat->hot、refault、temp等链表头
	//int file_area_move_list_head = is_file_area_move_list_head(p_file_area);
    //int file_area_move_list_head_or_hot = 0;
	unsigned int file_area_state;
	int age_dx;

	if(!enable_update_file_area_age)
		return;

	/* 这里有个优化点。把"if(if(p_file_area->file_area_age < hot_cold_file_global_info.global_age)) p_file_area->file_area_age = hot_cold_file_global_info.global_age"
	 * 做成一个atomic_cmpxchg()原子操作，这样可以防止多线程同时执行if判断里的p_file_area->file_area_hot_ahead.val.hot_ready_count ++，造成
	 * hot_ready_count++并发进行时，hot_ready_count出现乱七八槽的值!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
	age_dx = hot_cold_file_global_info.global_age - p_file_area->file_area_age;
	/*hot_cold_file_global_info.global_age更新了，把最新的global age更新到本次访问的file_area->file_area_age。并对
	 * file_area->access_count清0，本周期被访问1次则加1.这段代码不管理会并发，只是一个赋值*/
	//if(p_file_area->file_area_age < hot_cold_file_global_info.global_age){
	if(age_dx){
		p_file_area->file_area_age = hot_cold_file_global_info.global_age;
		/*文件file_stat最近一次被访问时的全局age，不是file_area的。内存回收时如果file_stat的recent_access_age偏大，直接跳过。
		 *还有一点 file_stat和recent_access_age和cooling_off_start_age公用union类型变量，mmap文件用到cooling_off_start_age。
		 *这里会修改cooling_off_start_age，会影响mmap文件的cooling_off_start_age冷却期的判定*/
		p_file_stat_base->recent_access_age = hot_cold_file_global_info.global_age;

		/*新的周期重置p_file_stat->file_area_move_to_head_count。但如果file_stat的file_area个数太少，
		 *file_area_move_to_head_count直接赋值0，因为file_area个数太少就没有必要再向file_stat->temp链表头移动file_area了*/
		if(p_file_stat_base->file_area_count >= FILE_AREA_MOVE_TO_HEAD_LEVEL)
			p_file_stat_base->file_area_move_to_head_count = hot_cold_file_global_info.file_area_move_to_head_count_max;
		else
			p_file_stat_base->file_area_move_to_head_count = 0;


#ifndef FILE_AREA_IN_FREE_KSWAPD_AND_SHADOW
#if 0		
		/*在第一个AHEAD_FILE_AREA_TO_COLD_AGE_DX个周期内，file_area被访问两次，设置了in_ahead标记。在随后的
		 * 一个AHEAD_FILE_AREA_TO_COLD_AGE_DX个周期内，file_area被第一次访问设置in_access标记，再被访问一次则设置in_hot标记*/
		if(file_area_in_temp_list(p_file_area) || file_area_in_warm_list(p_file_area)){

			/*上个周期file_area设置了in_access标记，但是下次该file_area再被访问已经过了FILE_AREA_MOVE_HEAD_DX周期，那就清理掉in_access标记*/
			if(file_area_in_access(p_file_area) && age_dx > FILE_AREA_MOVE_HEAD_DX)
				clear_file_area_in_access(p_file_area);

			/*如果in_ahead的file_area超过AHEAD_FILE_AREA_TO_COLD_AGE_DX个周期才被访问，就因长时间没访问而清理掉ahead标记*/
			if(file_area_in_ahead(p_file_area) && age_dx > AHEAD_FILE_AREA_TO_COLD_AGE_DX)
				clear_file_area_in_ahead(p_file_area);

			/*3.1 再再过一个周期，该file_area再被访问则设置in_access标记  3.2 再再再过一个周期，该file_area再被访问，file_area
			 * 的in_access标记和in_ahead标记都有，说明file_area连续两个FILE_AREA_MOVE_HEAD_DX周期file_area都被访问两次，则判定file_area是hot*/
			if(file_area_in_access(p_file_area) && file_area_in_ahead(p_file_area)){
				clear_file_area_in_access(p_file_area);
				clear_file_area_in_ahead(p_file_area);
				file_area_move_list_head_or_hot = HOT_READY;
				goto bypass;
			}

			/*2:下一个周期有in_access标记但没有in_ahead的file_area再被访问，设置file_area的in_ahead。但要清理掉in_access*/
			if(file_area_in_access(p_file_area) && !file_area_in_ahead(p_file_area)){
				clear_file_area_in_access(p_file_area);
				set_file_area_in_ahead(p_file_area);
				file_area_move_list_head_or_hot = AHEAD_READY;
				hot_cold_file_global_info.update_file_area_move_to_head_count ++;
				goto bypass;
			}

			/*1:第一个周期该file_area被访问，设置file_area的in_access标记*/
			if(!file_area_in_access(p_file_area))
				set_file_area_in_access(p_file_area);
		}
#else
		/*引入多层warm链表后，file_area每次被访问仅仅令file_area的access次数加1，不再做复杂的判断*/
		file_area_access_freq_inc(p_file_area);
#endif		
#else		
		/* 如果连续FILE_AREA_CHECK_HOT_DX个周期，file_area都被访问，则判定为hot file_area。
		 * 如果在FILE_AREA_MOVE_HEAD_DX个周期内，file_area被访问两次，则判定为ahead file_area。*/
		 /*hot_ready_count只是unsigned char的一半，只有4个bit位，最大只能16*/
		if((file_area_in_temp_list(p_file_area) || file_area_in_warm_list(p_file_area)) && p_file_area->file_area_hot_ahead.hot_ready_count < 0xF){
			/* 第1次进来，下边的两个if...else if都不成立，只会执行hot_ready_count ++。后续如果每个周期file_area都被访问，
			 * 再hot_ready_count达到FILE_AREA_CHECK_HOT_DX后，被判定为hot file_area。如果是第1个周期file_area被
			 * 访问，过了几个周期再被访问，就会执行else分支，对hot_ready_count清0，并goto baypass*/
			if(1 == age_dx){
				if(p_file_area->file_area_hot_ahead.hot_ready_count >= FILE_AREA_CHECK_HOT_DX){
					/*在下边设置hot，限定只有in_temp和in_warm的file_area才能设置in_hot标记，否则in_free、refault链表上的file_area也会设置in_hot标记*/
					//set_file_area_in_hot_list(p_file_area);在下边设置hot，不标准
					file_area_move_list_head_or_hot = HOT_READY;
					/*hot_ready_count及时清零否则影响下个周期热file_area的判定*/
					p_file_area->file_area_hot_ahead.hot_ready_count = 0;
					goto bypass;
				}
			}else{
				if(p_file_area->file_area_hot_ahead.hot_ready_count > 0){
					/*一旦不是两个周期连续访问，则对hot_ready_count清0*/
					p_file_area->file_area_hot_ahead.hot_ready_count = 0;
					goto bypass;
				}
			}

			/* 正常情况，上边if(age_dx)限制只有一个线程执行这里的加1操作，保证一个周期只加1。但存在
			 * 极低概率多线程同时这里执行加1操作，导致hot_ready_count偏大，误判位hot file_area，
			 * 这个影响不大，无非是延迟回收罢了。可以用原子操作改进，但是影响性能。ahead_ready_count同理*/
			p_file_area->file_area_hot_ahead.hot_ready_count ++;
		}

		/*hot和ahead file_area的判定是否可以把代码合二为一？最初是这样考虑的，但是二者判断逻辑有差异，最后还是分开了。
		 *ahead_ready_count只是unsigned char的一半，只有4个bit位，最大只能16*/
		if(!file_area_in_ahead(p_file_area) && p_file_area->file_area_hot_ahead.ahead_ready_count < 0xF){
			/* 在FILE_AREA_MOVE_HEAD_DX个周期内，file_area被访问了两次则判定为ahead file_area，但如果
			 * file_area有了hot标记，就不再设置ahead 标记了。不行，不设置，该file_area后续会反复判断
			 * file_area是否ahead，设置了in_ahead标记，就不会了*/
			if(p_file_area->file_area_hot_ahead.ahead_ready_count > 0){
				if(age_dx < FILE_AREA_MOVE_HEAD_DX){
					if(p_file_area->file_area_hot_ahead.ahead_ready_count >= 2 /*&& file_area_in_hot_list(p_file_area)*/){
						/*设置file_area in_ahead的操作可以放在这里，这个用担心并发问题*/
						set_file_area_in_ahead(p_file_area);
						p_file_area->file_area_hot_ahead.ahead_ready_count = 0;
						file_area_move_list_head_or_hot = AHEAD_READY;
						hot_cold_file_global_info.update_file_area_move_to_head_count ++;
						goto bypass;
					}
				}else{
					/*超过了FILE_AREA_MOVE_HEAD_DX个周期，file_area才被二次访问，就不能判定为ahead file_area了*/
					p_file_area->file_area_hot_ahead.ahead_ready_count = 0;
					goto bypass;
				}
			}

			p_file_area->file_area_hot_ahead.ahead_ready_count ++;
		}
#endif		
	}

//bypass:

	/*file_area的page是被读，则标记file_read读，内存回收时跳过这种file_area的page，优先回收write的*/
	if(!file_area_page_is_read(p_file_area) && (FILE_AREA_PAGE_IS_READ == read_or_write)){
	  /*char file_name_path[MAX_FILE_NAME_LEN];
	  memset(file_name_path,0,sizeof(&file_name_path));
	  if(strncmp(get_file_name(file_name_path,p_file_stat_base),"mysqld.log",10) == 0){
	      printk("%s stat:0x%llx mapping:0x%llx inode:0x%llx p_file_area:0x%llx writeonly:%d\n",file_name_path,(u64)p_file_stat_base,(u64)p_file_stat_base->mapping,(u64)p_file_stat_base->mapping->host,(u64)p_file_area,file_stat_in_writeonly_base(p_file_stat_base));
	      dump_stack();
	  }*/

		if(file_stat_in_writeonly_base(p_file_stat_base))
			clear_file_stat_in_writeonly_base(p_file_stat_base);
       
		set_file_area_page_read(p_file_area);
	}

	file_area_state = get_file_area_list_status(p_file_area);
	switch(file_area_state){
		/*file_stat->temp链表上file_area被多次访问则移动到file_area->temp链表头。
		 *被频繁访问则标记file_area的hot标记，不再移动file_area到file_stat->hot链表*/
		//if(file_area_in_temp_list(p_file_area) && !file_area_in_hot_list(p_file_area)){
		case file_area_in_temp_list_not_have_hot_status:

			/*file_stat->temp链表上的file_area被频繁访问后，只是设置file_area的hot标记，不会立即移动到file_stat->hot链表，在异步内存回收线程里实现*/
			//if(/*!file_area_in_hot_list(p_file_area) && */is_file_area_hot(p_file_area)){
		#if 0
			if(HOT_READY == file_area_move_list_head_or_hot){
				/* 不清理file_area in_temp_list状态，等把file_area移动到file_stat->hot链表后再清理，目的是:
				 * 如果重复把这些hot file_area移动挂到file_stat->hot链表，则触发crash*/
				//clear_file_area_in_temp_list(p_file_area);
				set_file_area_in_hot_list(p_file_area);
				hot_cold_file_global_info.update_file_area_hot_list_count ++;
			}
			/*file_stat->temp链表上file_area被多次访问，移动到file_stat->temp链表头*/
			else if(AHEAD_READY == file_area_move_list_head_or_hot){
		#else
			/*新版本只要非global的文件的file_area被访问大于1次，就把file_area移动到temp链表头，现在改为global文件也适用了*/
			if(/*!file_stat_in_global_base(p_file_stat_base) && */file_area_access_freq(p_file_area) > 1){
				file_area_access_freq_clear(p_file_area);
        #endif
				//hot_cold_file_global_info.update_file_area_move_to_head_count ++;

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
							//if(!file_area_in_ahead(p_file_area))
							//	set_file_area_in_ahead(p_file_area);
						}
						spin_unlock(&p_file_stat_base->file_stat_lock);
						p_file_stat_base->file_area_move_to_head_count --;
						//file_area_in_update_lock_count ++;
						//file_area_move_to_head_count ++;
					}
					/*如果一个周期文件的file_area移动到file_stat->temp链表头次数超过限制，后续又有file_area因多次访问
					 *而需要移动到file_stat->temp链表头，只是标记file_area的ahead标记，不再加锁移动到file_area到链表头*/
					/*else{
					  if(!file_area_in_ahead(p_file_area))
					  set_file_area_in_ahead(p_file_area);
					  }*/
				}
			}

			hot_cold_file_global_info.update_file_area_temp_list_count ++;
			break;
			//}

			//else if(file_area_in_warm_list(p_file_area) && !file_area_in_hot_list(p_file_area)){
#if 0
		case file_area_in_warm_list_not_have_hot_status:
			//if(is_file_area_hot(p_file_area)){
			if(HOT_READY == file_area_move_list_head_or_hot){
				//clear_file_area_in_temp_list(p_file_area);
				set_file_area_in_hot_list(p_file_area);
				hot_cold_file_global_info.update_file_area_hot_list_count ++;
			}
			/*else if(AHEAD_READY == file_area_move_list_head_or_hot){
			  if(file_area_in_ahead(p_file_area))
			  set_file_area_in_ahead(p_file_area);

			  hot_cold_file_global_info.update_file_area_move_to_head_count ++;
			  }*/
			hot_cold_file_global_info.update_file_area_warm_list_count ++;
			break;
			//}

			/*file_stat->free链表上的file_area被访问，只是标记file_area in_free_list，异步内存回收线程里再把该file_area移动到file_stat->refault链表*/
			//else if(file_area_in_free_list(p_file_area) && !file_area_in_refault_list(p_file_area)){
#endif
		case file_area_in_free_list_not_have_refault_status:
			/* 标记file_area in_refault_list后，不清理file_area in_free_list状态，只有把file_area移动到
			 * file_stat->refault链表时再清理掉。目的是防止这种file_area在file_stat_other_list_file_area_solve()
			 * 中重复把这种file_area移动file_area->refault链表*/
			//clear_file_area_in_free_list(p_file_area);
			set_file_area_in_refault_list(p_file_area);
			hot_cold_file_global_info.update_file_area_free_list_count ++;
			
			/* 发生refault的page个数，跟workingset_refault_file一个意思，access_count是发生的refault的次数。但是有问题，
			 * 会导致统计的发生refault的page个数偏大。因为这个file_area的in_free状态在被异步内存回收线程清理掉前，这个
			 * case file_area_in_free_list_not_have_refault_status 状态就会一直成立，导致update_file_area_free_list_count
			 * 偏大，因为这个file_area的page每被访问一次，这个case file_area_in_free_list_not_have_refault_status就成立，
			 * 导致统计的update_file_area_free_list_count偏大。没事没事，因为这里设置file_area的in_refault状态，后续
			 * case file_area_in_free_list_not_have_refault_status就不成立了，最后改为使用refault_page_count了*/
			//hot_cold_file_global_info.update_file_area_free_list_count += access_count;
			/*if(p_file_stat_base->refault_page_count < USHRT_MAX - 2)
				p_file_stat_base->refault_page_count ++;*/
			break;
			//}

			/*其他情况，对file_stat->refault、hot链表上file_area的处理。如果file_area被多次访问而需要移动到各自的链表头，
			 *这里只是标记file_area的ahead标记，不再移动到链表头，降低使用file_stat_lock锁，降低性能损耗*/
			//else{
		default:
			  hot_cold_file_global_info.update_file_area_other_list_count ++;
			  /*if(file_area_move_list_head){
			  if(!file_area_in_ahead(p_file_area))
			  set_file_area_in_ahead(p_file_area);

			  hot_cold_file_global_info.update_file_area_move_to_head_count ++;
			  }*/

			break;
			//}
	}

	return;
}
#else
/*这个函数可以做成inline了，代码非常少，降低性能损耗*/
inline void hot_file_update_file_status(struct address_space *mapping,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,int read_or_write)
{
	/*if(!enable_update_file_area_age)
	  return;*/

	/* 这里有个优化点。把"if(if(p_file_area->file_area_age < hot_cold_file_global_info.global_age)) p_file_area->file_area_age = hot_cold_file_global_info.global_age"
	 * 做成一个atomic_cmpxchg()原子操作，这样可以防止多线程同时执行if判断里的p_file_area->file_area_hot_ahead.val.hot_ready_count ++，造成
	 * hot_ready_count++并发进行时，hot_ready_count出现乱七八槽的值!!!!!!test_and_clear_bit锁总线损耗大，前边加大file_area_in_init，该bit大部分时间是0*/
	if(p_file_area->file_area_age < hot_cold_file_global_info.global_age || (file_area_in_init(p_file_area) && test_and_clear_bit(F_file_area_in_init,(unsigned long *)&p_file_area->file_area_state))){
		p_file_area->file_area_age = hot_cold_file_global_info.global_age;
		/*文件file_stat最近一次被访问时的全局age，不是file_area的。内存回收时如果file_stat的recent_access_age偏大，直接跳过。
		 *还有一点 file_stat和recent_access_age和cooling_off_start_age公用union类型变量，mmap文件用到cooling_off_start_age。
		 *这里会修改cooling_off_start_age，会影响mmap文件的cooling_off_start_age冷却期的判定*/
		p_file_stat_base->recent_access_age = hot_cold_file_global_info.global_age;
		//p_file_stat_base->hot_ready_count;

		/*引入多层warm链表后，file_area每次被访问仅仅令file_area的access次数加1，不再做复杂的判断*/
		file_area_access_freq_inc(p_file_area);
	}

	/* file_area的page是被读，则标记file_read读，内存回收时跳过这种file_area的page，优先回收write的。
	 * 因为现在的文件的folio都走了预读流程，因此都会执行到get_folio_from_file_area_for_file_area()
	 * 设置file_area的in_read标记，并清理掉file_stat的writeonly标记，因此这里不再设置了*/
	if(!file_area_page_is_read(p_file_area) && (FILE_AREA_PAGE_IS_READ == read_or_write)){
		if(file_stat_in_writeonly_base(p_file_stat_base))
			clear_file_stat_in_writeonly_base(p_file_stat_base);

		set_file_area_page_read(p_file_area);
		//printk("%s file_stat:0x%llx status 0x%x mmap:%d %s in_read\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,mapping_mapped(mapping),get_file_name(p_file_stat_base));
		//dump_stack();
	}

#if 0/*为了进一步降低文件读写的性能损耗，把这个if判断也去掉。取代的办法是，将来异步内存回收线程从file_stat->free
	   链表遍历到这个file_area，发现有page，说明该file_area内存回收后发生了refault，然后设置file_area的in_refault标记*/
	if(file_area_in_free_list(p_file_area)){
		/* 标记file_area in_refault_list后，不清理file_area in_free_list状态，只有把file_area移动到
		 * file_stat->refault链表时再清理掉。目的是防止这种file_area在file_stat_other_list_file_area_solve()
		 * 中重复把这种file_area移动file_area->refault链表*/
		//clear_file_area_in_free_list(p_file_area);
		set_file_area_in_refault_list(p_file_area);
		//hot_cold_file_global_info.update_file_area_free_list_count ++;
	}
#endif
	return;
}
#endif
//EXPORT_SYMBOL(hot_file_update_file_status);
static inline void check_hot_file_stat_and_move_global(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_stat *p_file_stat,char is_global_file_stat)
{
	unsigned int file_stat_list_type = p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;

	/* 如果文件的热file_area个数超过阀值则被判定为热文件，文件file_stat移动到global mmap_file_stat_hot_head链表。
	 * 但前提是文件file_stat只能在global temp、middle file、large_file链表上*/
	if(!is_global_file_stat && !file_stat_in_file_stat_hot_head_list_base(p_file_stat_base) && is_mmap_file_stat_hot_file(p_hot_cold_file_global,p_file_stat)){

		spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
		/*file_stat可能被iput()并发delete了并移动到global delete链表*/
		if(!file_stat_in_delete_base(p_file_stat_base)){
			if((1 << F_file_stat_in_file_stat_temp_head_list) == file_stat_list_type){
				if(!file_stat_in_file_stat_temp_head_list_base(p_file_stat_base) ||file_stat_in_file_stat_temp_head_list_error_base(p_file_stat_base))
					panic("%s file_stat:0x%llx status error:0x%x not in temp_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

				clear_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
			}
			else if((1 << F_file_stat_in_file_stat_middle_file_head_list) == file_stat_list_type){
				if(!file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_middle_file_head_list_error_base(p_file_stat_base))
					panic("%s file_stat:0x%llx status error:0x%x not int middle_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

				clear_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
			}
			else{
				if((1 << F_file_stat_in_file_stat_large_file_head_list) != file_stat_list_type)
					panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);

				if(!file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base) ||file_stat_in_file_stat_large_file_head_list_error_base(p_file_stat_base))
					panic("%s file_stat:0x%llx status error:0x%x not in large_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

				clear_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
			}

			set_file_stat_in_file_stat_hot_head_list_base(p_file_stat_base);
			list_move(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_hot_head);
			hot_cold_file_global_info.hot_mmap_file_stat_count ++;
			if(shrink_page_printk_open)
				printk("7:%s file_stat:0x%llx status:0x%x is hot file\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		}
		spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
	}
}
/*如果file_area是热的，则把file_area移动到file_stat->hot链表。如果file_stat的热file_area个数超过阀值，则移动到global hot链表*/
static inline void check_hot_file_area_and_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,/*unsigned int file_stat_list_type,*/unsigned int file_type,char is_global_file_stat)
{
	struct file_stat *p_file_stat = NULL;
	unsigned int file_stat_list_type;
	//struct global_file_stat *p_global_file_stat = NULL;
	//char file_area_in_mapcount_and_hot = 0;
	//被判定为热file_area后，对file_area的access_count清0，防止干扰后续file_area冷热判断
	file_area_access_count_clear(p_file_area);
	
	/*小文件只是设置一个hot标记就return，不再把file_area移动到file_area_hot链表*/
	if(FILE_STAT_TINY_SMALL == file_type){
		clear_file_area_in_temp_list(p_file_area);
		set_file_area_in_hot_list(p_file_area);
		return; 
	}else if(FILE_STAT_SMALL == file_type){
		struct file_stat_small *p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);

		spin_lock(&p_file_stat_base->file_stat_lock);
		clear_file_area_in_temp_list(p_file_area);
		set_file_area_in_hot_list(p_file_area);
		list_move(&p_file_area->file_area_list,&p_file_stat_small->file_area_other);
		//file_stat->temp 链表上的file_area个数减1
		p_file_stat_base->file_area_count_in_temp_list --;
		spin_unlock(&p_file_stat_base->file_stat_lock);
		return;
	}

	/*normal 和global file_stat的热file_area不在这里判定了，移动warm_mutil函数判断了*/
	panic("%s file_stat:0x%llx file_area:0x%llx state:0x%x hot error\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

	if(file_stat_in_mapcount_file_area_list_base(p_file_stat_base)){
		printk("%s file_stat:0x%llx status:0x%x is mapcount file ,can not change to hot file\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		return;
	}
#if 0
	p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

	file_stat_list_type = p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;

	spin_lock(&p_file_stat_base->file_stat_lock);
	if(file_area_in_temp_list(p_file_area)){
		//file_stat->temp 链表上的file_area个数减1
		p_file_stat_base->file_area_count_in_temp_list --;
		clear_file_area_in_temp_list(p_file_area);
	}
	/*in_warm的file_area移动到file_stat->hot链表，不用spin_lock加锁优化点???????????????*/
	else if(file_area_in_warm_list(p_file_area))
		clear_file_area_in_warm_list(p_file_area);
	else
		panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in error list\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

	set_file_area_in_hot_list(p_file_area);
	//file_area移动到hot链表
	list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
	spin_unlock(&p_file_stat_base->file_stat_lock);

	//该文件的热file_area数加1
	p_file_stat->file_area_hot_count ++;
#else

#if 0	
	if(file_area_in_writeonly_or_cold_list(p_file_area)){

	}else if(file_area_in_warm_list(p_file_area)){
		if(file_area_in_warm_list_error(p_file_area))
			panic("1：%s file_stat:0x%llx file_area:0x%llx state:0x%x hot error\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

		clear_file_area_in_warm_list(p_file_area);
	}else{
		/* 一个file_area可能同时具有in_hot和in_mapconut标记，遇到这种file_area，不再移动file_area，只是将来会在hot和mapcount链表之间移动来移动去。
		 * 分析代码后，这不可能，因为get_file_area_age_mmap()函数里，限制有in_mapcount状态的file_area无法调用到该函数*/
		/*if(file_area_in_mapcount_list(p_file_area)){
			printk("2:%s file_stat:0x%llx file_area:0x%llx state:0x%x mapcount and hot\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);
			file_area_in_mapcount_and_hot = 1;
		}
		else*/
			panic("2:%s file_stat:0x%llx file_area:0x%llx state:0x%x hot error\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);
	}
#endif	
	set_file_area_in_hot_list(p_file_area);

	/*并且不用file_stat_lock加锁，因为此时file_area肯定不处于file_stat->temp链表，不用加锁*/
	if(is_global_file_stat){
		if(!file_stat_in_global_base(p_file_stat_base))
			panic("%s file_stat:0x%llx file_area:0x%llx state:0x%x not in global\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

		/*global_mmap_file_stat结构体包裹了file_stat结构体，file_stat结构体包裹了file_stat_base*/
		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
		//hot的file_area个数加1
		p_file_stat->file_area_hot_count ++;
		list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
	}else{
		if(get_file_stat_type(p_file_stat_base) != FILE_STAT_NORMAL)
			panic("%s file_stat:0x%llx file_area:0x%llx state:0x%x not normal file_stat\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

		file_stat_list_type = p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;
		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
		//文件file_stat的hot的file_area个数加1
		p_file_stat->file_area_hot_count ++;
		list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
	}

#endif	
	if(shrink_page_printk_open)
		printk("6:%s file_stat:0x%llx file_area:0x%llx is hot status:0x%x\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);
#if 0
	这段代码移动到 check_hot_file_stat_and_move_global 函数
	/* 如果文件的热file_area个数超过阀值则被判定为热文件，文件file_stat移动到global mmap_file_stat_hot_head链表。
	 * 但前提是文件file_stat只能在global temp、middle file、large_file链表上*/
	if(!is_global_file_stat && !file_stat_in_file_stat_hot_head_list_base(p_file_stat_base) && is_mmap_file_stat_hot_file(p_hot_cold_file_global,p_file_stat)){

		spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
		/*file_stat可能被iput()并发delete了并移动到global delete链表*/
		if(!file_stat_in_delete_base(p_file_stat_base)){
			if((1 << F_file_stat_in_file_stat_temp_head_list) == file_stat_list_type){
				if(!file_stat_in_file_stat_temp_head_list_base(p_file_stat_base) ||file_stat_in_file_stat_temp_head_list_error_base(p_file_stat_base))
					panic("%s file_stat:0x%llx status error:0x%x not in temp_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

				clear_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
			}
			else if((1 << F_file_stat_in_file_stat_middle_file_head_list) == file_stat_list_type){
				if(!file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_middle_file_head_list_error_base(p_file_stat_base))
					panic("%s file_stat:0x%llx status error:0x%x not int middle_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

				clear_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
			}
			else{
				if((1 << F_file_stat_in_file_stat_large_file_head_list) != file_stat_list_type)
					panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);

				if(!file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base) ||file_stat_in_file_stat_large_file_head_list_error_base(p_file_stat_base))
					panic("%s file_stat:0x%llx status error:0x%x not in large_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

				clear_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
			}

			set_file_stat_in_file_stat_hot_head_list_base(p_file_stat_base);
			list_move(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_hot_head);
			hot_cold_file_global_info.hot_mmap_file_stat_count ++;
			if(shrink_page_printk_open)
				printk("7:%s file_stat:0x%llx status:0x%x is hot file\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		}
		spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
	}
#endif	
}
/*如果file_area是mapcount的，则把file_area移动到file_stat->mapcount链表。如果file_stat的mapcount file_area个数超过阀值，则移动到global mapcount链表*/
static inline void check_mapcount_file_area_and_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,/*unsigned int file_stat_list_type,*/unsigned int file_type,char is_global_file_stat)
{
	struct file_stat *p_file_stat = NULL;
	struct global_file_stat *p_global_file_stat = NULL;
	//char file_area_in_mapcount_and_hot = 0;
	unsigned int file_stat_list_type;
	//char list_num;

	//被判定为mapcount file_area后，对file_area的access_count清0，防止干扰后续file_area冷热判断
	file_area_access_count_clear(p_file_area);

	/*小文件只是设置一个mapcount标记就return，不再把file_area移动到file_area_mapcount链表*/
	if(FILE_STAT_TINY_SMALL == file_type){
		clear_file_area_in_temp_list(p_file_area);
		set_file_area_in_mapcount_list(p_file_area);
		return;
	}else if(FILE_STAT_SMALL == file_type){
		struct file_stat_small *p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);

		spin_lock(&p_file_stat_base->file_stat_lock);
		clear_file_area_in_temp_list(p_file_area);
		set_file_area_in_mapcount_list(p_file_area);
		list_move(&p_file_area->file_area_list,&p_file_stat_small->file_area_other);
		//file_stat->temp 链表上的file_area个数减1
		p_file_stat_base->file_area_count_in_temp_list --;
		spin_unlock(&p_file_stat_base->file_stat_lock);
		return;
	}

	if(file_stat_in_file_stat_hot_head_list_base(p_file_stat_base)){
		printk("%s file_stat:0x%llx status:0x%x is hot file ,cant change to mapcount file\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		return;
	}

#if 0
	p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
	file_stat_list_type = p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;

	spin_lock(&p_file_stat_base->file_stat_lock);
	//文件file_stat的mapcount的file_area个数加1
	p_file_stat->mapcount_file_area_count ++;
	if(file_area_in_temp_list(p_file_area)){
		//file_stat->temp 链表上的file_area个数减1
		p_file_stat_base->file_area_count_in_temp_list --;
		clear_file_area_in_temp_list(p_file_area);
	}
	else if(file_area_in_warm_list(p_file_area))
		clear_file_area_in_warm_list(p_file_area);
	else
		panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in error list\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

	set_file_area_in_mapcount_list(p_file_area);
	//file_area的page的mapcount大于1，则把file_area移动到file_stat->file_area_mapcount链表。新版本mapcount file_area移动到file_area_hot链表
	list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);
	spin_unlock(&p_file_stat_base->file_stat_lock);
#else
    /* 引入global_file_stat后，有了多级warm和cold链表，file_stat_status的bit0~bit11已经被各种bit占满了，
	 * 至少还缺两个bit用于表示file_area处于file_area_warm_cold等链表。
	 * 最后决定，所有处于file_stat各种warm链表上的file_area，都设置in_warm状态。
	 *
	 * 并且，现在global_file_stat和normal file_stat的file_stat_base->temp链表上的file_area，在内存
	 * 回收前就会先一次性都移动到file_stat->warm链表上，因此执行到这里的，都是global_file_stat和
	 * file_stat各级warm和cold链表上的file_area。
	 *
	 * 最新方案又变了，每个warm或writelonly链表，都有唯一的编号，分别是POS_WIITEONLY_OR_COLD、POS_WARM、POS_WARM_HOT
	 * global_file_stat又多了POS_WARM_COLD、POS_WARM_MIDDLE、POS_WARM_MIDDLE_HOT 这3个链表。
	 * 这6个链表上的file_area都可能被判定为mapcount file_area*/
#if 0	
	if(file_area_in_writeonly_or_cold_list(p_file_area)){

	}else if(file_area_in_warm_list(p_file_area)){
		if(file_area_in_warm_list_error(p_file_area))
			panic("1：%s file_stat:0x%llx file_area:0x%llx state:0x%x mapcount error\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

		clear_file_area_in_warm_list(p_file_area);
	}else{
		/*一个file_area可能同时具有in_hot和in_mapconut标记，遇到这种file_area，不再移动file_area，只是将来会在hot和mapcount链表之间移动来移动去
		 * 分析代码后，这不可能，因为get_file_area_age_mmap()函数里，限制有in_hot状态的file_area无法调用到该函数*/
		/*if(file_area_in_hot_list(p_file_area)){
			printk("2:%s file_stat:0x%llx file_area:0x%llx state:0x%x mapcount and hot\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);
			file_area_in_mapcount_and_hot = 1;
		}
		else*/
			panic("2:%s file_stat:0x%llx file_area:0x%llx state:0x%x mapcount error\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);
	}
#else
	/*只做异常判断，不再清理list_num，以保留它是从哪个链表移动到mapcount链表的*/
	if(list_num_get(p_file_area) > POS_WARM_HOT){
		panic("2:%s file_stat:0x%llx file_area:0x%llx state:0x%x mapcount error\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);
	}
#endif
	clear_file_area_in_temp_list(p_file_area);
	set_file_area_in_mapcount_list(p_file_area);

	/* file_area在global_file_stat时，直接把mapcount file_area移动到file_area_mapcount链表。
	 * normal file_stat只把file_area移动到file_area_hot链表，没有file_area_mapcount链表，为了节省内存消耗
	 * 。并且不用file_stat_lock加锁，因为此时file_area肯定不处于file_stat->temp链表，不用加锁*/
	if(is_global_file_stat){
		if(!file_stat_in_global_base(p_file_stat_base))
			panic("%s file_stat:0x%llx file_area:0x%llx state:0x%x not in global\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

		/*global_mmap_file_stat结构体包裹了file_stat结构体，file_stat结构体包裹了file_stat_base*/
		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
		p_global_file_stat = container_of(p_file_stat,struct global_file_stat,file_stat);
		//mapcount的file_area个数加1
		p_file_stat->mapcount_file_area_count ++;
		list_move(&p_file_area->file_area_list,&p_global_file_stat->file_area_mapcount);
	}else{
		if(get_file_stat_type(p_file_stat_base) != FILE_STAT_NORMAL)
			panic("%s file_stat:0x%llx file_area:0x%llx state:0x%x not normal file_stat\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

		file_stat_list_type = p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;
		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
		//文件file_stat的mapcount的file_area个数加1
		p_file_stat->mapcount_file_area_count ++;
		list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
	}
#endif

	if(shrink_page_printk_open)
		printk("8:%s file_stat:0x%llx file_area:0x%llx state:0x%x temp to mapcount\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

	/*如果文件file_stat的mapcount的file_area个数超过阀值，则file_stat被判定为mapcount file_stat而移动到
	 *global mmap_file_stat_mapcount_head链表。但前提file_stat必须在global temp、middle_file、large_file链表*/
	if(!is_global_file_stat && !file_stat_in_mapcount_file_area_list_base(p_file_stat_base) && is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat)){

		spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
		/*file_stat可能被iput()并发delete了并移动到global delete链表*/
		if(!file_stat_in_delete_base(p_file_stat_base)){
			if((1 << F_file_stat_in_file_stat_temp_head_list) == file_stat_list_type){
				if(!file_stat_in_file_stat_temp_head_list_base(p_file_stat_base) ||file_stat_in_file_stat_temp_head_list_error_base(p_file_stat_base))
					panic("%s file_stat:0x%llx status error:0x%x not in temp_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

				clear_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
			}
			else if((1 << F_file_stat_in_file_stat_middle_file_head_list) == file_stat_list_type){
				if(!file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_middle_file_head_list_error_base(p_file_stat_base))
					panic("%s file_stat:0x%llx status error:0x%x not int middle_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

				clear_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
			}
			else{
				if((1 << F_file_stat_in_file_stat_large_file_head_list) != file_stat_list_type)
					panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);

				if(!file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base) ||file_stat_in_file_stat_large_file_head_list_error_base(p_file_stat_base))
					panic("%s file_stat:0x%llx status error:0x%x not in large_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

				clear_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
			}

			set_file_stat_in_mapcount_file_area_list_base(p_file_stat_base);
			list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_mapcount_head);
			p_hot_cold_file_global->mapcount_mmap_file_stat_count ++;
			if(shrink_page_printk_open1)
				printk("9:%s file_stat:0x%llx status:0x%x is mapcount file\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		}
		spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
	}
}
#if 0
/*特别注意，调用该函数的,传入的file_area，不仅有file_stat->temp、warm链表上的，还有file_stat->hot、mapcount链表上的!!!!!!!!!*/
unsigned int get_file_area_age(struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,unsigned int file_stat_list_type,unsigned int file_type)
{
	struct folio *folio;
	unsigned long vm_flags;
	int ret;
	unsigned char mmap_page_count = 0;

	/* 只有cache文件的file_area有mmap page时，才会设置file_area_in_mmap标记。这种情况可能吗？存在的，
	 * 一个文件最初是cache的，随着读写、内存回收，导致in_temp_list的file_area不等于总file_area个数，
	 * 此时该cache文件被mmap映射了，该文件是无法转成mmap文件的，因为只有in_temp_list的
	 * file_area不等于总file_area个数才可以转。并且，只有mmap文件的file_area，里边的page都不是mmap的，
	 * 这种file_area才会设置file_area_in_cache标记。为什么不把cache文件的cache file_area全标记in_cache，
	 * mmap文件mmap file_area全标记in_mmap？为了节省性能，因为这是默认的。
	 *
	 * 其实我也可以完全不再区分mmap文件和cache文件，只用区分mmap file_area和cache file_area。不行，
	 * 太浪费性能了。因为如果这样，cache文件的每一个file_area，都至少要遍历一次file_area里的page，判断
	 * 是否有mmap page，没有的话才能标记file_area in_cache，这只是个别情况，太浪费性能了*/
	if(likely(file_stat_in_cache_file_base(p_file_stat_base))){
		if(file_area_in_cache(p_file_area))
			panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x cache file have in_cache flag\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

		/*cache文件没有in_mmap标记的file_area直接返回file_area_age。如果有mmap标记，那就执行下边的for循环，判断file_area的age冷热*/
		if(!file_area_in_mmap(p_file_area))
			return p_file_area->file_area_age;
	}else{
		if(file_area_in_mmap(p_file_area))
			panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x mmap file have in_mmap flag\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

		/*mmap文件，但是被标记cache的file_area，说明是里边的page都是非mmap的，直接返回file_area_age。否则就执行下边的for循环，判断file_area的age冷热*/
		if(file_area_in_cache(p_file_area))
			return p_file_area->file_area_age;
	}

	/*走到这个分支，有两种情况
	 * 1：cache文件遇到mmap的file_area
	 * 2：mmap文件的mmap的file_area*/
	ret = 0;
	for(i = 0;i < PAGE_COUNT_IN_AREA;){
		folio = p_file_area->pages[i];
		if(folio && !folio_is_file_area_index(folio)){
			/* 只是page的冷热信息，不是内存回收，不用page_lock。错了，要执行folio_referenced()检测access bit位，必须要加锁。
			 * 但不能folio_trylock，而是folio_lock，一旦获取锁失败就等待别人释放锁，必须获取锁成功，然后下边探测page的access bit*/
			//if (!folio_trylock(folio))
			folio_lock(folio);

			/*不是mmap page*/
			if (!folio_mapped(folio)){
				folio_unlock(folio);
				continue;
			}

			//如果page被其他进程回收了，这里不成立，直接过滤掉page
			if(unlikely(folio->mapping != p_file_stat_base->mapping)){
				folio_unlock(folio);
				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx page->mapping:0x%llx != mapping:0x%llx\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)page,page->flags,(u64)page->mapping,(u64)mapping);
				continue;
			}
			/*file_area里每检测到一个mmap文件则加1*/
			mmap_page_count ++;
			/*遇到mapcount偏大的page的file_area，直接break，节省性能*/
			if(0 == mapcount_file_area && page_mapcount(page) > 6){
				mapcount_file_area = 1;
				folio_unlock(folio);
				break;
			}
			/*检测映射page的页表pte access bit是否置位了，是的话返回1并清除pte access bit。错了，不是返回1，
			 *是反应映射page的进程个数page_referenced函数第2个参数是0里边会自动执行lock page()*/
			ret += folio_referenced(folio, 1, folio_memcg(folio),&vm_flags);
		}

		folio_unlock(folio);

		/*当file_area->file_area_age是0时，说明file_area的4个page是第一次被check pte access bit判断冷热，此时要把i++
		 * 而把file_area的所有的page都check pte access bit一次，如果置1了会自动清理掉，否则pte access bit一直置1，会
		 * 误判为一直是热页。后续再遍历到p_file_area->file_area_age不再是0了，只用i += 2，隔一个page判断一个，节省性能
		 * 但有特殊情况，如果这个file_area最初是cache的，被访问后file_area->file_area_age赋值global age而大于0.然后再
		 * mmap映射了该page，此时因为file_area->file_area_age大于0，导致执行i += 2，隔一个page判断一个，漏掉的page如果
		 * pte access bit置1了，那内存回收时就会回收失败，浪费性能。算了，先这样判断吧。内存回收失败有针对性处理*/
		if(0 == p_file_area->file_area_age)
			i += 1;
		else
			i += 2;
	}
	/*file_area里有至少一个mmap page，且pte access bit置1了，判定为被访问了，赋值global_age*/
	if(ret > 0)
		p_file_area->file_area_age = hot_cold_file_global_info.global_age;

	if(file_stat_in_mmap_file_base(p_file_stat_base)){
		/*mmap文件，热file_area、热文件的处理*/
		if(ret > 0){
			file_area_access_count_add(p_file_area,1);
			/*如果file_area的page连续3次检测到pte access bit置1了，判定该file_area是热file_area*/
			if(file_area_access_count_get(p_file_area) > 2 && !file_area_in_hot_list(p_file_area)){
				check_hot_file_area_and_file_stat(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_stat_list_type,file_type);
				p_hot_cold_file_global->mmap_file_shrink_counter.scan_hot_file_area_count += 1;
			}
		}
		else
			file_area_access_count_clear(p_file_area);

		/*mmap文件，mapcountfile_area、mapcount文件的处理*/
		if(mapcount_file_area && !file_area_in_mapcount_list(p_file_area)){
			check_mapcount_file_area_and_file_stat(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_stat_list_type,file_type);
			p_hot_cold_file_global->mmap_file_shrink_counter.scan_mapcount_file_area_count += 1;
		}
	}

	/*file_area没有一个file_area*/
	if(0 == mmap_page_count){
		/*cache文件的file_area，但是有in_mmap标记，说明之前有mmap file_area，现在没了，于是清理掉in_mmap标记*/
		if(file_stat_in_cache_file_base(p_file_stat) && file_area_in_mmap(p_file_area))
			clear_file_area_in_mmap(p_file_area);

		/*mmap文件的file_area，没有一个mmap file_area，那就要标记in_cache*/
		if(file_stat_in_mmap_file_base(p_file_stat) && !file_area_in_cache(p_file_area))
			set_file_area_in_cache(p_file_area);
	}else{
		/*cache文件的file_area，但是有mmap page，于是标记in_mmap*/
		if(file_stat_in_cache_file_base(p_file_stat) && !file_area_in_mmap(p_file_area))
			set_file_area_in_mmap(p_file_area);

		/*mmap文件的file_area，之前没有mmap page而标记了in_cache，但是现在有mmap page了，那就清理掉in_cache标记*/
		if(file_stat_in_mmap_file_base(p_file_stat) && file_area_in_cache(p_file_area))
			clear_file_area_in_cache(p_file_area);
	}

	return p_file_area->file_area_age;
}
#endif
/*特别注意，调用该函数的,传入的file_area，不仅有file_stat->temp、warm链表上的，还有file_stat->hot、mapcount链表上的!!!!!!!!!*/
char inline get_file_area_age_quick(struct file_stat_base *p_file_stat_base,struct file_area *p_file_area/*,char *get_file_area_age_fail*/)
{
	/* 只有cache文件的file_area有mmap page时，才会设置file_area_in_mmap标记。这种情况可能吗？存在的，
	 * 一个文件最初是cache的，随着读写、内存回收，导致in_temp_list的file_area不等于总file_area个数，
	 * 此时该cache文件被mmap映射了，该文件是无法转成mmap文件的，因为只有in_temp_list的
	 * file_area不等于总file_area个数才可以转。并且，只有mmap文件的file_area，里边的page都不是mmap的，
	 * 这种file_area才会设置file_area_in_cache标记。为什么不把cache文件的cache file_area全标记in_cache，
	 * mmap文件mmap file_area全标记in_mmap？为了节省性能，因为这是默认的。
	 *
	 * 其实我也可以完全不再区分mmap文件和cache文件，只用区分mmap file_area和cache file_area。不行，
	 * 太浪费性能了。因为如果这样，cache文件的每一个file_area，都至少要遍历一次file_area里的page，判断
	 * 是否有mmap page，没有的话才能标记file_area in_cache，这只是个别情况，太浪费性能了*/
	if(likely(file_stat_in_cache_file_base(p_file_stat_base))){
		if(file_area_in_cache(p_file_area))
			panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x cache file have in_cache flag\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

		/*cache文件没有in_mmap标记的file_area直接返回file_area_age。如果有mmap标记，那就执行下边的for循环，判断file_area的age冷热*/
		if(!file_area_in_mmap(p_file_area))
			return 0;
		//return p_file_area->file_area_age;

		hot_cold_file_global_info.hot_cold_file_shrink_counter.cache_file_stat_get_file_area_fail_count += 1;
	}else{
		/*如果该文件从cache文件转过成mmap文件，就会有in_mmap标记的file_area，此时不能crash。正常的*/
		if(file_area_in_mmap(p_file_area) && !file_stat_in_from_cache_file_base(p_file_stat_base)){
			if(file_stat_in_global_base(p_file_stat_base))
				printk("%s file_stat:0x%llx status:0%x file_area:0x%llx status:0x%x mmap file have in_mmap flag\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state);
			else
				panic("%s file_stat:0x%llx status:0%x file_area:0x%llx status:0x%x mmap file have in_mmap flag\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state);
		}

		/*mmap文件，但是被标记cache的file_area，说明是里边的page都是非mmap的，直接返回file_area_age。否则就执行下边的for循环，判断file_area的age冷热*/
		if(file_area_in_cache(p_file_area)){
			hot_cold_file_global_info.hot_cold_file_shrink_counter.mmap_file_stat_get_file_area_from_cache_count += 1;
			return 0;
		}
		//return p_file_area->file_area_age;
	}
    
	/*走到这里说明上边获取file_area的age失败*/
    //*get_file_area_age_fail = 1;
	return -1;
}
/*扫描一个file_area里的page时，间隔几个page扫描一次*/
#define SCAN_PAGE_INTERVAL_IN_FILE_AREA 1
/*特别注意，调用该函数的,传入的file_area，不仅有file_stat->temp、warm链表上的，还有file_stat->hot、mapcount链表上的!!!!!!!!!*/
void get_file_area_age_mmap(struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,struct hot_cold_file_global *p_hot_cold_file_global,char *file_stat_changed,/*unsigned int file_stat_list_type,*/unsigned int file_type,char is_global_file_stat)
{
	struct folio *folio;
	unsigned long vm_flags;
	int ret,i;
	unsigned char mmap_page_count = 0,mapcount_file_area = 0;
	int scan_page_interval = SCAN_PAGE_INTERVAL_IN_FILE_AREA;
	int scan_page_count = 0;
	unsigned int age_dx;
	/*普通文件直接从file_stat_base->mapping获取mapping，global_file_stat->mapping始终是NULL*/
	struct address_space *mapping = p_file_stat_base->mapping;

#if 0
	/* 只有cache文件的file_area有mmap page时，才会设置file_area_in_mmap标记。这种情况可能吗？存在的，
	 * 一个文件最初是cache的，随着读写、内存回收，导致in_temp_list的file_area不等于总file_area个数，
	 * 此时该cache文件被mmap映射了，该文件是无法转成mmap文件的，因为只有in_temp_list的
	 * file_area不等于总file_area个数才可以转。并且，只有mmap文件的file_area，里边的page都不是mmap的，
	 * 这种file_area才会设置file_area_in_cache标记。为什么不把cache文件的cache file_area全标记in_cache，
	 * mmap文件mmap file_area全标记in_mmap？为了节省性能，因为这是默认的。
	 *
	 * 其实我也可以完全不再区分mmap文件和cache文件，只用区分mmap file_area和cache file_area。不行，
	 * 太浪费性能了。因为如果这样，cache文件的每一个file_area，都至少要遍历一次file_area里的page，判断
	 * 是否有mmap page，没有的话才能标记file_area in_cache，这只是个别情况，太浪费性能了*/
	if(likely(file_stat_in_cache_file_base(p_file_stat_base))){
		if(file_area_in_cache(p_file_area))
			panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x cache file have in_cache flag\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

		/*cache文件没有in_mmap标记的file_area直接返回file_area_age。如果有mmap标记，那就执行下边的for循环，判断file_area的age冷热*/
		if(!file_area_in_mmap(p_file_area))
			return p_file_area->file_area_age;
	}else{
		if(file_area_in_mmap(p_file_area))
			panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x mmap file have in_mmap flag\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

		/*mmap文件，但是被标记cache的file_area，说明是里边的page都是非mmap的，直接返回file_area_age。否则就执行下边的for循环，判断file_area的age冷热*/
		if(file_area_in_cache(p_file_area))
			return p_file_area->file_area_age;
	}
#endif
	/*第一次扫描file_area，把file_area里的page全都扫描pte access bit*/
	if(file_area_in_init(p_file_area)){
		clear_file_area_in_init(p_file_area);
		scan_page_interval = 1;
	}

	/*global_file_stat链表上file_area来自各种乱七八糟的文件，此时从file_area->mapping获取mapping*/
	if(is_global_file_stat){
		/* 如果p_file_area->mapping是NULL，说明file_area的文件已经被iput，这是异常情况要panic。确定这是异常情况？完全有可能
		 * 执行到这里时，该文件正在被iput()，然后标记p_file_area->mapping=NULL，故这里p_file_area->mapping是NULL完全有可能!!!!!!*/
		if(NULL == p_file_area->mapping){
			//panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx page->mapping:0x%llx != mapping:0x%llx NULL\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags,(u64)folio->mapping,(u64)p_file_stat_base->mapping);
			printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx page->mapping:0x%llx != mapping:0x%llx NULL\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags,(u64)folio->mapping,(u64)p_file_stat_base->mapping);
			return;
		}

		mapping = p_file_area->mapping;
	}

	/*走到这个分支，有两种情况
	 * 1：cache文件遇到mmap的file_area
	 * 2：mmap文件的mmap的file_area*/
	ret = 0;
	for(i = 0;i < PAGE_COUNT_IN_AREA;){
		folio = p_file_area->pages[i];
		scan_page_count ++;
		if(folio && !folio_is_file_area_index_or_shadow(folio)){

			/* 有个疑问，看cold_file_isolate_lru_pages_and_shrink()，对参与内存回收的page，
			 * 还要folio_try_get_rcu(folio)后if(folio != rcu_dereference(p_file_area->pages[i])
			 * 判断folio是否被内存回收了，这里folio_referenced()判断folio冷热，需要加这两个步骤吗?????
			 * 如果不加有什么坏处？不对，有大问题！因为这里folio_lock(folio)后，folio可能被其他进程
			 * 释放，page引用计数是0了，此时就不能再执行folio_referenced()判断folio冷热了。最后决定
			 * 当前函数添加folio_try_get_rcu和if(folio != rcu_dereference(p_file_area->pages[i]))
			 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!重大有隐藏bug*/
			if (!folio_try_get_rcu(folio)){
				goto next_folio;
			}
			if(folio != rcu_dereference(p_file_area->pages[i])){
				folio_put(folio);
				goto next_folio;
			}

			/* 只是page的冷热信息，不是内存回收，不用page_lock。错了，要执行folio_referenced()检测access bit位，必须要加锁。
			 * 但不能folio_trylock，而是folio_lock，一旦获取锁失败就等待别人释放锁，必须获取锁成功，然后下边探测page的access bit*/
			//if (!folio_trylock(folio))
			folio_lock(folio);

			/*不是mmap page*/
			if (!folio_mapped(folio)){
				folio_unlock(folio);
				folio_put(folio);
				goto next_folio;
			}

			//如果page被其他进程回收了，这里不成立，直接过滤掉page
			//if(unlikely(folio->mapping != p_file_stat_base->mapping)){
			/*有个隐藏很深的bug，如果folio->mapping和file_area->mapping都是NULL，这个if不成立，把这种有问题的folio错误放过了。因此要加上if(NULL == p_file_area->mapping)是NULL的情况*/
			if(unlikely(folio->mapping != mapping) || (NULL == folio->mapping)){
				/* 如果file_area已经被iput()，这是异常情况要panic。这种情况是绝对不可能的。因为iput()过程是：file_area的page都释放了，然后再把
				 * p_file_area->mapping=NULL。代码代码执行到这里，file_area的folio肯定不是NULL，smp_rmb()后，p_file_area->mapping肯定不是NULL。
				 * 如果这里p_file_area->mapping却是NULL，则触发panic*/
				smp_rmb();
				if(NULL == p_file_area->mapping)
					panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx page->mapping:0x%llx != mapping:0x%llx NULL\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags,(u64)folio->mapping,(u64)p_file_stat_base->mapping);

				folio_unlock(folio);
				folio_put(folio);

				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx page->mapping:0x%llx != mapping:0x%llx\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags,(u64)folio->mapping,(u64)p_file_stat_base->mapping);
				goto next_folio;
			}
			/*file_area里每检测到一个mmap文件则加1*/
			mmap_page_count ++;
			/*遇到mapcount偏大的page的file_area，直接break，节省性能*/
			if(folio_mapcount(folio) > MAPCOUNT_LEVEL){
				mapcount_file_area = 1;
				folio_unlock(folio);
				folio_put(folio);
				break;
			}
			/*检测映射page的页表pte access bit是否置位了，是的话返回1并清除pte access bit。错了，不是返回1，
			 *是反应映射page的进程个数page_referenced函数第2个参数是0里边会自动执行lock page()*/
			ret += folio_referenced(folio, 1, folio_memcg(folio),&vm_flags);

			folio_unlock(folio);
			folio_put(folio);
		}

next_folio:
		/*当file_area->file_area_age是0时，说明file_area的4个page是第一次被check pte access bit判断冷热，此时要把i++
		 * 而把file_area的所有的page都check pte access bit一次，如果置1了会自动清理掉，否则pte access bit一直置1，会
		 * 误判为一直是热页。后续再遍历到p_file_area->file_area_age不再是0了，只用i += 2，隔一个page判断一个，节省性能
		 * 但有特殊情况，如果这个file_area最初是cache的，被访问后file_area->file_area_age赋值global age而大于0.然后再
		 * mmap映射了该page，此时因为file_area->file_area_age大于0，导致执行i += 2，隔一个page判断一个，漏掉的page如果
		 * pte access bit置1了，那内存回收时就会回收失败，浪费性能。算了，先这样判断吧。内存回收失败有针对性处理*/

		/*现在方案改了，最初分配的mmap file_area都设置了mmap_init标记*/
		//if(0 == p_file_area->file_area_age)
		i += scan_page_interval;
	}

	if(1 == scan_page_interval && (scan_page_count != PAGE_COUNT_IN_AREA) && (0 == mapcount_file_area)){
		printk("%s file_area:0x%llx status:0x%x has init flag,but scan_page_count=%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,scan_page_count);
	}
	age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;

	/*file_area里有至少一个mmap page，且pte access bit置1了，判定为被访问了，赋值global_age*/
	if(ret > 0){
		p_file_area->file_area_age = p_hot_cold_file_global->global_age;
		/*mmap文件的file_area确定被访问，则把global_age赋值于recent_access_age，这个相对cache文件有延迟，但是没办法*/
		p_file_stat_base->recent_access_age = p_hot_cold_file_global->global_age;
	}

	/* 目前只允许in_temp、in_warm链表上的file_area转成hot 和mapcount file_area。现在cache file和mmap file_aea获取file_area_age采用同一个
	 * 函数接口。in_hot、in_mapcount、in_free等链表上的file_area也会执行该函数获取file_area_age，必须要屏蔽掉它们*/
	//if(file_stat_in_mmap_file_base(p_file_stat_base) && (file_area_in_temp_list(p_file_area) || file_area_in_warm_list(p_file_area)|| file_area_in_writeonly_or_cold_list(p_file_area))){
	//if(file_stat_in_mmap_file_base(p_file_stat_base) && !file_area_in_hot_list(p_file_area) && !file_area_in_mapcount_list(p_file_area)){
	/*1:tiny small或small文件的in_temp链表的file_area，可以转成hot、mapcount file_area
	 *2:file_stat或global file_stat在多层warm链表上的file_area，但是是从in_temp_list来的file_area也可以转成hot、mapcount file_area，该file_area有in_temp属性
	 *2:file_stat或global file_stat的多层warm链表上的file_area可以转成hot、mapcount file_area，此时get_file_area_list_status是0*/
	if(file_stat_in_mmap_file_base(p_file_stat_base) && (file_area_in_temp_list(p_file_area) || get_file_area_list_status(p_file_area) == 0)){
		/*mmap文件，热file_area、热文件的处理*/
//#ifndef FILE_AREA_IN_FREE_KSWAPD_AND_SHADOW
#if 0			
		if(ret > 0){
			/*连续第3次遍历到该file_area的pte access被访问则设置in_hot标记。或者，有了in_ahead标记后，在MMAP_AHEAD_FILE_AREA_TO_COLD_AGE_DX个周期内，file_area又被访问一次，也设置file_area的in_hot标记*/
			if(file_area_in_ahead(p_file_area)){
				clear_file_area_in_ahead(p_file_area);
				check_hot_file_area_and_file_stat(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_type);
				p_hot_cold_file_global->mmap_file_shrink_counter.scan_hot_file_area_count += 1;
				/* file_area由in_temp或in_warm状态变为in_hot，必须标记file_stat_changed=1，因为file_area已经移动到in_hot链表了
				 * ，不再处于in_temp或in_warm链表*/
				*file_stat_changed = 1;
				
				goto mmap_bypass;
			}

			/*连续第2次遍历到该file_area的pte access被访问则设置in_ahead标记*/
			if(file_area_in_access(p_file_area) && !file_area_in_ahead(p_file_area)){
				clear_file_area_in_access(p_file_area);
				set_file_area_in_ahead(p_file_area);
				goto mmap_bypass;
			}

			/*第1次遍历到该file_area的pte access被访问则设置in_access标记*/
			if(!file_area_in_access(p_file_area))
				set_file_area_in_access(p_file_area);
		}else{
			/*如果file_area有in_access标记后，下次再遍历到该file_area的page后，该page没有被访问了，且过了规定时间，则清理掉in_access标记*/
			if(file_area_in_access(p_file_area) && age_dx > MMAP_AHEAD_FILE_AREA_ACCESS_TO_COLD_AGE_DX)
				clear_file_area_in_access(p_file_area);

			/*file_area有in_ahead标记后，过了很长时间都没有被访问，则要清理掉in_ahead标记。目的是该file_area有变in_hot的潜力，不要轻易清理掉in_ahead标记*/
			if(file_area_in_ahead(p_file_area) &&  age_dx > MMAP_AHEAD_FILE_AREA_AHEAD_TO_COLD_AGE_DX)
				clear_file_area_in_ahead(p_file_area);
		}
#else
		if(ret > 0){
			/*引入多层warm链表后，mmap file_area的hot file_area的判定逻辑变了
			 * 1：即便是mmap文件的file_area，也需要执行file_area_access_freq_inc()里的cmpxchg令file_area的access_count加1，
			 * 因为该file_area有可能被read/write访问，此时正在update函数里并发令access_count加1，要防护并发问题
			 * 2：normal文件的热file_area的判定不在这里判定了，移动到file_stat_multi_level_warm_or_writeonly_list_file_area_solve
			 * 函数。并且针对非热file_area的判断逻辑也改变了，只要连续扫到大于2次被访问，就判定为热file_area。因为mmap文件无法
			 * 在被访问立即就感知到,必须扫page的pte access bit才能直到被访问了，扫描到一次很宝贵*/
			file_area_access_freq_inc(p_file_area);
			if(file_area_access_freq(p_file_area) > 2 && (FILE_STAT_TINY_SMALL == file_type || FILE_STAT_SMALL == file_type)){
				file_area_access_freq_clear(p_file_area);
				check_hot_file_area_and_file_stat(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_type,is_global_file_stat);
				p_hot_cold_file_global->mmap_file_shrink_counter.scan_hot_file_area_count += 1;
				/* file_area由in_temp或in_warm状态变为in_hot，必须标记file_stat_changed=1，因为file_area已经移动到in_hot链表了
				 * ，不再处于in_temp或in_warm链表*/
				*file_stat_changed = 1;

				/*现在不允许一个file_area被判定hot后再被判定为mapount，主要怕逻辑太混乱*/
				//goto mmap_bypass;
			}
		}else{
            //file_area_access_freq_clear(p_file_area);
		}
#endif
//mmap_bypass:	

		/* mmap文件，mapcountfile_area、mapcount文件的处理*/
		if(mapcount_file_area){
			check_mapcount_file_area_and_file_stat(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_type,is_global_file_stat);
			p_hot_cold_file_global->mmap_file_shrink_counter.scan_mapcount_file_area_count += 1;
			/* file_area由in_temp或in_warm状态变为in_mapcount，必须标记file_stat_changed=1，因为file_area已经移动到in_mapcount链表了
			 * ，不再处于in_temp或in_warm链表*/
			*file_stat_changed = 1;
		}
	}

	/*file_area没有一个mmap page*/
	if(0 == mmap_page_count){
		/*cache文件的file_area，但是有in_mmap标记，说明之前有mmap file_area，现在没了，于是清理掉in_mmap标记*/
		if(file_stat_in_cache_file_base(p_file_stat_base) && file_area_in_mmap(p_file_area))
			clear_file_area_in_mmap(p_file_area);

		/*mmap文件的file_area，没有一个mmap file_area，那就要标记in_cache*/
		if(file_stat_in_mmap_file_base(p_file_stat_base) && !file_area_in_cache(p_file_area)){
			set_file_area_in_cache(p_file_area);
			if(file_stat_in_test_base(p_file_stat_base))
				printk("%s file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d global_age:%d set cache\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age,p_hot_cold_file_global->global_age);
		}
	}else{
		/*cache文件的file_area，但是有mmap page，于是标记in_mmap*/
		if(file_stat_in_cache_file_base(p_file_stat_base) && !file_area_in_mmap(p_file_area))
			set_file_area_in_mmap(p_file_area);

		/*mmap文件的file_area，之前没有mmap page而标记了in_cache，但是现在有mmap page了，那就清理掉in_cache标记*/
		if(file_stat_in_mmap_file_base(p_file_stat_base) && file_area_in_cache(p_file_area)){
			clear_file_area_in_cache(p_file_area);
			if(file_stat_in_test_base(p_file_stat_base))
				printk("%s file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d global_age:%d clear cache\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age,p_hot_cold_file_global->global_age);
		}
	}

	//return p_file_area->file_area_age;
}
/*
 * get_file_area_age原本是一个函数，统计获取mmap、cache文件的file_area_age，但最终决定做成一个宏定义，主要是该函数是个热点函数，
 * 但传参太多，还都是针对mmap file_area的，但是更多情况是获取cache文件的file_area_age，直接return file_area->file_area_age就行了。
 * 没必要传递这么多参数，于是把原get_file_area_age函数拆成get_file_area_age_quick和get_file_area_age_mmap。
 *
 * 需要特殊说明，file_stat_changed主要目的是：针对mmap文件，调用get_file_area_age_mmap()获取in_temp和in_warm链表上的file_area_age时，
 * 该file_area可能转成hot 和mapcount file_area，那就要把file_stat_changed置1，说明file_ara状态变了，原函数就要注意该file_area，不能
 * 按照正常流程处理了。
 *
 * file_stat_changed还有另一个目的，get_file_area_age()宏定义第一步就要把file_stat_changed清0，如果get_file_area_age_quick()获取
 * file_area_age失败，就要把file_stat_changed置1，说明获取失败，于是才执行get_file_area_age_mmap()再次获取file_area_age。但是，
 * 执行get_file_area_age_mmap前，必须要对file_stat_changed清0，恢复原状。为什么不多定义一个变量？麻烦
 *
 * 另外一点，根本没有必要再传入file_area_age这个临时变量，直接使用file_area->file_area_age就行了，因为get_file_area_age()对
 * file_area_age是实时更新的可就是file_area最新的age，于是这个版本的get_file_area_age()不用了，用下#else分支的，更简洁。
 * 并且，get_file_area_age_quick()获取file_area_age失败返回-1，获取成功返回，直接用它的返回值判断是否执行get_file_area_age_mmap()
 * */
#if 0
#define get_file_area_age(p_file_stat_base,p_file_area,file_area_age,p_hot_cold_file_global,file_stat_changed,file_type) \
{ \
	file_stat_changed = 0; \
    file_area_age = get_file_area_age_quick(p_file_stat_base,p_file_area,&file_stat_changed);\
    if(file_stat_changed){\
		file_stat_changed = 0;\
        file_area_age = get_file_area_age_mmap(p_file_stat_base,p_file_area,p_hot_cold_file_global,&file_stat_changed,file_type);\
	}\
}
#else
/*最新的改动，get_file_area_age_quick()获取file_area_age失败返回-1，获取成功返回，直接用它的返回值判断*/
#define get_file_area_age(p_file_stat_base,p_file_area,p_hot_cold_file_global,file_stat_changed,file_type,is_global_file_stat) \
{ \
	file_stat_changed = 0; \
    if(get_file_area_age_quick(p_file_stat_base,p_file_area)){ \
        get_file_area_age_mmap(p_file_stat_base,p_file_area,p_hot_cold_file_global,&file_stat_changed,file_type,is_global_file_stat);\
	}\
}
#endif
static void all_file_stat_reclaim_pages_counter(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,char is_global_file_stat,unsigned int reclaim_pages)
{
	unsigned int file_stat_list_type = p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;
	struct reclaim_pages_counter *p_reclaim_pages_counter;

	if(file_stat_in_cache_file_base(p_file_stat_base))
		p_reclaim_pages_counter = &p_hot_cold_file_global->reclaim_pages_counter_cache;
	else
		p_reclaim_pages_counter = &p_hot_cold_file_global->reclaim_pages_counter_mmap;

	if(is_global_file_stat){
		p_reclaim_pages_counter->global_file_stat_reclaim_pages += reclaim_pages;
		return;
	}

	switch (file_stat_list_type){
		case 1 << F_file_stat_in_file_stat_tiny_small_file_head_list:
			p_reclaim_pages_counter->tiny_small_file_stat_reclaim_pages += reclaim_pages;
			break;
		case 1 << F_file_stat_in_file_stat_small_file_head_list:
			p_reclaim_pages_counter->small_file_stat_reclaim_pages += reclaim_pages;
			break;
		/* temp、middle、large甚至writeonly文件，中途使用时随着hot、mapcount的file_area增多，可能中途
		 * 变为hot、mapcount文件，这里默认都算到temp文件释放的page里了*/
		case 1 << F_file_stat_in_file_stat_temp_head_list:
		case 1 << F_file_stat_in_mapcount_file_area_list:
		case 1 << F_file_stat_in_file_stat_hot_head_list:
			p_reclaim_pages_counter->temp_file_stat_reclaim_pages += reclaim_pages;
			break;
		case 1 << F_file_stat_in_file_stat_middle_file_head_list:
			p_reclaim_pages_counter->middle_file_stat_reclaim_pages += reclaim_pages;
			break;
		case 1 << F_file_stat_in_file_stat_large_file_head_list:
			p_reclaim_pages_counter->large_file_stat_reclaim_pages += reclaim_pages;
			break;
		case 1 << F_file_stat_in_file_stat_writeonly_file_head_list:
			p_reclaim_pages_counter->writeonly_file_stat_reclaim_pages += reclaim_pages;
			break;
		default:
			panic("%s p_file_stat:0x%llx file_stat_list_type:0x%x error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);
	}
}
//遍历p_file_stat对应文件的file_area_free链表上的file_area结构，找到这些file_area结构对应的page，这些page被判定是冷页，可以回收
unsigned long cold_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,
		struct list_head *file_area_free,unsigned int scan_file_area_max_for_memory_reclaim,struct list_head *file_area_real_free,char no_set_in_free_list,struct list_head *file_area_warm_list,struct memory_reclaim_info_for_one_warm_list *memory_reclaim_info_for_one_warm_list)
{
	struct file_area *p_file_area,*tmp_file_area;
	int i;
	struct address_space *mapping;
	//pg_data_t *pgdat = NULL;
	//struct page *page;
	struct folio *folio;
	//unsigned int isolate_pages = 0;
	//int traverse_file_area_count = 0;  
	//struct lruvec *lruvec = NULL,*lruvec_new = NULL;
	//int move_page_count = 0;
	/*file_area里的page至少一个page发现是mmap的，则该file_area移动到file_area_have_mmap_page_head，后续回收mmap的文件页*/
	//int find_file_area_have_mmap_page;
	//unsigned int find_mmap_page_count_from_cache_file = 0;
	char print_once = 1;
	unsigned char mmap_page_count = 0;
	struct folio_batch fbatch;
	int ret;
	unsigned int free_pages = 0;
	int j,page_count;
	unsigned int scan_file_area_count = 0,zero_page_file_area_count = 0;
    char is_global_file_stat = file_stat_in_global_base(p_file_stat_base);
	char is_normal_file = (get_file_stat_type(p_file_stat_base) == FILE_STAT_NORMAL);
	unsigned int scan_file_area_count_in_reclaim = 0,scan_warm_file_area_count = 0,scan_zero_page_file_area_count_in_reclaim = 0;
	unsigned int scan_file_area_count_reclaim_fail = 0;
	char file_area_page_reclaim_fail;

	is_normal_file = is_global_file_stat ? 1:is_normal_file;
	/*使用前必须先对fbatch初始化*/
	folio_batch_init(&fbatch);

	/*char file_name_path[MAX_FILE_NAME_LEN];
	  memset(file_name_path,0,sizeof(&file_name_path));
	  get_file_name(file_name_path,p_file_stat_base);*/

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
	//
#if 0//这个加锁放到遍历file_stat内存回收，最初执行的get_file_area_from_file_stat_list()函数里了，这里不再重复加锁
	if(file_inode_lock(p_file_stat_base) <= 0){
		printk("%s file_stat:0x%llx status 0x%x inode lock fail\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		return 0;
	}
#endif	
	/*执行到这里，就不用担心该inode会被其他进程iput释放掉*/

	if(!is_global_file_stat)
	    mapping = p_file_stat_base->mapping;

	/*if((file_area_real_free && -1 == scan_file_area_max_for_memory_reclaim) || (NULL == file_area_real_free && -1 != scan_file_area_max_for_memory_reclaim))
		BUG();*/

	/*!!隐藏非常深的地方，这里遍历file_area_free(即)链表上的file_area时，可能该file_area在hot_file_update_file_status()中被访问而移动到了temp链表
	  这里要用list_for_each_entry_safe()，不能用list_for_each_entry!!!!!!!!!!!!!!!!!!!!!!!!*/
	list_for_each_entry_safe(p_file_area,tmp_file_area,file_area_free,file_area_list){

		scan_file_area_count_in_reclaim ++;
		/*1:如果scan的file_area中，大部分都移动到了file_area_warm_list链表，直接break，否则太浪费cpu且无法有效回收到内存
		 *2:如果scan的file_area中，大部分都没有page，直接break*/
		if(scan_warm_file_area_count > 128 && 
				((scan_file_area_count_in_reclaim - scan_warm_file_area_count < 8) || (scan_file_area_count_in_reclaim - zero_page_file_area_count < 8) ||
				 (scan_file_area_count_in_reclaim - scan_file_area_count_reclaim_fail < 8)))
			break;

		/*如果file_area近期又被访问了，但只能是in_read属性的file_area。则移动到file_area_warm_list链表，后续再移动回file_stat->warm链表*/
		if(file_area_warm_list && file_area_in_read(p_file_area)){
			if(/*file_area_access_freq(p_file_area) && */p_hot_cold_file_global->global_age - p_file_area->file_area_age < 60){
				/* 如果memory_still_memrgency_after_reclaim很大，说明内存回收后依然内存紧张，持续了很多次，此时忽略file_area的access_count，
				 * 不再把file_area移动到file_area_warm_list。但如果file_area的access_count大于2，说明访问频繁，也要移动到file_area_warm_list链表*/
				if(file_area_access_freq(p_file_area) >= 2 || (p_hot_cold_file_global->memory_still_memrgency_after_reclaim <= 3 && file_area_access_freq(p_file_area))){
					list_num_update(p_file_area,POS_WARM);
					list_move(&p_file_area->file_area_list,file_area_warm_list);
					scan_warm_file_area_count ++;
					printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x access\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
					continue;
				}
			}
		}

        /* 重大bug，引入多层warm链表功能后，file_stat和global_file_stat的内存回收，都是直接向cold_file_isolate_lru_pages_and_shrink
		 * 函数传入file_stat->writeonly_or_cold或warm_cold链表头，然后遍历这两个链表的file_area进行内存回收。于是决定就在当前
		 * 函数里，每遍历到一个file_area则设置in_free标记，即test_and_set_bit(F_file_area_in_free_list,(void *)(&p_file_area->file_area_state))
		 * 之所以要用test_and_set_bit，也是经验告诉我，有可能重复设置file_area的in_free情况，此时就主动panic。遍历完file_stat->writeonly_or_cold
		 * 或warm_cold链表的file_area后，再执行 list_splice_init把这些file_area一次性全部都移动到file_stat->free链表。这个方案看似没有问题，
		 * 实则引入了多个bug
		 * bug1:下边先test_and_set_bit设置file_area的in_free标记，但存在zero_page_file_area_count遍历的零个page的file_area过多，导致提前结束
		 * 循环。然后file_stat->writeonly_or_cold或warm_cold链表上还有没有遍历的file_area，但是却list_splice_init把这些file_area一次性全部都
		 * 移动到file_stat->free链表，但这些file_area都没有in_free标记，将来遍历到就会触发not in_free panic
		 * bug2：如果传入的file_area_real_free非NULL，则要把遍历的file_area都移动到file_area_real_free链表。当遍历的file_area个数
		 * 超过scan_file_area_max_for_memory_reclaim，则if(scan_file_area_count ++  > scan_file_area_max_for_memory_reclaim) break
		 * 结束遍历，没有执行list_move(&p_file_area->file_area_list,file_area_real_free)把file_area移动到file_area_real_free链表。
		 * 这个file_area已经执行了test_and_set_bit(F_file_area_in_free_list,(void *)(&p_file_area->file_area_state))设置in_free标记。
		 * 。最后把file_area_real_free上的file_area移动到file_stat->free链表。刚才那个file_area依然停留在file_stat->writeonly_or_cold
		 * 链表，但是有in_free标记。等再次遍历file_stat->writeonly_or_cold的file_area进行内存回收，遇到该file_area，发现有in_free标记
		 * 则会触发panic
		 * bug3：对于wrtiteonly文件，在内存紧张时，会直接遍历file_stat->free、hot、refaut链表上的file_area进行内存，但是执行该函数时
		 * 会执行test_and_set_bit(F_file_area_in_free_list,(void *)(&p_file_area->file_area_state))对这些file_area标记in_free标记，
		 * 相当于file_stat->hot、refault链表上的file_area也会被标记in_free标记，必须会因为file_area状态不对再次触发panic
		 *
		 * 当初对file_stat->temp、free、refault、hot、mapcount、多个warm链表上的file_area做严格的状态检查，是多个明智的一个决定！
		 * 主动暴露了太多file_area乱list_move而触发的bug。并且这里使用test_and_set_bit(F_file_area_in_free_list,(void *)(&p_file_area->file_area_state))
		 * ，如果重复设置in_free标记，也主动panic，也是个明智的决定，主动触发了bug
		 *
		 * 要想解决以上问题，必须确保只有遍历过的file_stat->writeonly_or_cold或warm_cold链表上的file_area才能移动到file_stat->free链表。
		 * 并且只有这些file_area才能设置in_free标记，设置过in_free标记的file_area必须移动到file_stat->free链表，不能停留在原链表
		 * */
		
		/* 采用多层warm链表内存回收的normal文件和global_file_stat，内存回收前没有机会设置in_free标记，内存回收时遍历到再设置in_free标记.
		 * 如果from_writeonly_free_list或no_set_in_free_list是1，说明是直接回收writeonly文件file_stat->free、refault、hot链表上的file_area，
		 * 此时不能设置file_area的in_free标记*/
		if(is_normal_file && !no_set_in_free_list){
			if(test_and_set_bit(F_file_area_in_free_list,(void *)(&p_file_area->file_area_state)) /*&& !from_writeonly_free_list*/){
				/* 实际测试表明，这里竟然成立了而触发panic。原来该文件是writeonly文件，内存紧张时会直接从file_stat->free链表回收file_area，
				 * 这些file_area都是有in_free标记的*/
				panic("%s file_area:0x%llx status:0x%x already set in_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
			}
		}

        /* file_area_real_free非NULL说明针对引入多层warm链表方案后，针对normal file_stat内存回收时，直接一股脑的
		 * 对file_stat->writeonly_or_cold链表上的file_area进行内存回收，不再遍历这个链表上的file_area，判断age_dx，
		 * 然后把冷file_area移动到file_area_free_temp链表进行内存回收，这些都没有了，主要怕浪费性能。没必要，在
		 * 内存回收函数里也可以这样做，对内存回收成功的file_area移动到file_area_real_free临时链表，后续再移动到
		 * file_stat->free链表。这种文件还要限制内存回收的scan_file_area_max个数，主要怕一次回收太多容易refault*/

		/* writeonly free、hot、refault链表的file_area直接内存回收和tiny small的temp链表的file_area内存回收，都不能把file_area
		 * 移动到file_area_real_free链表，此时file_area_real_free是NULL*/
		if(file_area_real_free){
			/*这里把file_area移动到file_area_real_free链表，但是下边break了，这个file_area就无法参与内存回收了。
			 *但是存在上边file_area设置了in_free标记，但是没有执行ist_move(&p_file_area->file_area_list,file_area_real_free)
			 *移动到file_area_real_free链表，而是停留在原链表，但是有in_free标记，会因状态不对而panic*/
			list_move(&p_file_area->file_area_list,file_area_real_free);
			if(!file_area_in_free_list(p_file_area))
				panic("%s file_area:0x%llx status:0x%x no in_free_list\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			/*if放到循环最后就行，但是list_move不能放到循环最后，否则上边test_and_set_bit设置了in_free标记，但是
			 *下边因为file_area或page繁忙，直接continue,就不会把这个in_free标记的file_area移动到file_area_real_free链表了*/
			//if(scan_file_area_count ++  > scan_file_area_max_for_memory_reclaim)
			//	break;

		    //list_move(&p_file_area->file_area_list,file_area_real_free);
            /*不能contine，因为file_area还要参与内存回收*/
			//continue;
		}

		/*如果扫描到的file_area大部分都没有page，直接return，不浪费性能。这个if判断也需要放到循环最后，否则这里直接
		 *跳出循环，file_area就无法参与下边的内存回收了，*/
#if 0			
		if(scan_file_area_count_for_zero_page++ > 64){
			if(zero_page_file_area_count > 60){
				/* 注意，之类不能直接return 返回，因为folio_batch_count(&fbatch)可能还包含未释放的page，注意
				 * cold_file_isolate_lru_pages_and_shrink()函数不管合适都不能中途直接return，必须从函数最后
				 * return*/
				printk("%s file_stat:0x%llx status:0x%x too much zero_page_file_area_count:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,zero_page_file_area_count);
				goto direct_return;
			}
			zero_page_file_area_count = 0;
			scan_file_area_count_for_zero_page = 0;
			/*防止for循环耗时太长导致本地cpu没有调度*/
			cond_resched();
		}
#endif		

		/*file_area一个page都没有直接返回*/
		if(!file_area_have_page(p_file_area)){
			zero_page_file_area_count ++;
			scan_zero_page_file_area_count_in_reclaim ++;
			continue;
		}

		if(file_area_in_read(p_file_area))
			p_hot_cold_file_global->read_file_area_count_in_reclaim ++;

		/* 在global_file_stat链表上的file_area，来自乱七八槽的文件，因此决定为file_area增加mapping成员，在创建
		 * file_area时把mapping保存到了file_area的mapping成员。现在对file_area的page内存回收时，取出这个mapping。
		 * 因为下边内存回收时，folio_lock后，要判断folio->mapping跟所属文件的mapping是否相等，防止该folio在
		 * folio_lock抢占锁过程，另一个进程优先抢占了锁，并且folio释放了，令folio->mapping = NULL；或者folio
		 * 被释放后又被分配赋值新的mapping给folio->mapping；甚至folio被释放后又被分配，并且还是被老的文件
		 * 分配，故folio->mapping又被赋值老的mapping。为了应对这种情况，内核采取的手段是
		 * 1：folio_try_get_rcu(folio) 令page引用计数加1，加1成功后，其他进程不能再释放该page
		 * 2: if(folio != rcu_dereference(p_file_area->pages[i])再从xarray tree读取一次folio，如果该folio
		 * 被其他进程释放了，那此时p_file_area->pages[i]读取到的就是NULL。此时就不用再回收该folio了
		 *
		 * 我认为有这些手段就行了，就能防护该folio被其他进程并发释放了，但是内核又多加了两个步骤
		 * 1：folio_lock(folio)
		 * 2：if(folio->mapping != mapping)
		 * 为什么要多加这两个步骤，我想不通，明明上边的步骤已经可以防护folio被其他进程异常释放了?????????
		 * */
		if(is_global_file_stat){
			/* 如果p_file_area->mapping是NULL，说明file_area的文件已经被iput，这是异常情况要panic。确定这是异常情况？完全有可能
			 * 执行到这里时，该文件正在被iput()，然后标记p_file_area->mapping=NULL，故这里p_file_area->mapping是NULL完全有可能!!!!!!*/
			if(NULL == p_file_area->mapping){
				//panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx page->mapping:0x%llx != mapping:0x%llx NULL\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags,(u64)folio->mapping,(u64)p_file_stat_base->mapping);
				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx page->mapping:0x%llx != mapping:0x%llx NULL\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags,(u64)folio->mapping,(u64)p_file_stat_base->mapping);
				continue;
			}

			mapping = p_file_area->mapping;
		}

		/*每遍历一个file_area，都要先对mmap_page_count清0，然后下边遍历到file_area的每一个mmap page再加1*/
		mmap_page_count = 0;

		if(file_area_page_reclaim_fail){
			scan_file_area_count_reclaim_fail ++;
		    file_area_page_reclaim_fail = 0;
		}
		//得到file_area对应的page
		for(i = 0;i < PAGE_COUNT_IN_AREA;i ++){
			folio = p_file_area->pages[i];
			if(!folio || folio_is_file_area_index_or_shadow(folio)){
				if(shrink_page_printk_open1)
					printk("%s file_area:0x%llx status:0x%x folio NULL\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

				/*如果一个file_area的page全都释放了，则file_stat->pages[0/1]就保存file_area的索引。然后第一个page又被访问了，
				 *然后这个file_area被使用。等这个file_area再次内存回收，到这里时，file_area->pages[1]就是file_area_index*/
				if(shrink_page_printk_open_important && folio_is_file_area_index_or_shadow(folio) && print_once){
					print_once = 0;
					printk(KERN_ERR"%s file_area:0x%llx status:0x%x folio_is_file_area_index!!!!!!!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
				}

				continue;
			}
			/* 这里有个bug1，如果到这里，该page正好被kswapd进程回收掉了，于是把page在file_area_state的page bit位清0了，
			 * 此时is_file_area_page_bit_set(p_file_area,i)就是0了就会触发panic。于是要把
			 * is_file_area_page_bit_set(p_file_area,i)的判断放到下边的if(unlikely(folio->mapping != mapping)后，
			 * 因为只有这里才说明该page没有被kswapd等进程释放并重新分配给其他进程用。并且，此时该page如果
			 * 被kswad释放并重新分配，folio_test_anon(folio)就可能因为page时匿名页而panic，于是下边的
			 * if (unlikely(folio_test_anon(folio))...)判断也要放到if(unlikely(folio->mapping != mapping)后!!!!!!!!!*/
		#if 0	
			if(xa_is_value(folio) || !is_file_area_page_bit_set(p_file_area,i)){
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx xa_is_value or file_area_bit error!!!!!!\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags);
			}

			/*如果page映射了也表页目录，这是异常的，要给出告警信息!!!!!!!!!!!!!!!!!!!还有其他异常状态。但实际调试
			 *遇到过page来自tmpfs文件系统，即PageSwapBacked(page)，最后错误添加到inacitve lru链表，但没有令inactive lru
			 *链表的page数加1，最后导致隔离page时触发mem_cgroup_update_lru_size()中发现lru链表page个数是负数而告警而crash*/
			if (unlikely(folio_test_anon(folio))|| unlikely(PageCompound(&folio->page)) || unlikely(folio_test_swapbacked(folio))){
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags);
			}
        #endif

			/*到这里，page经过第一阶段针对file_area的判断，合法了。下边是针对page是否能参与内存回收的合法性判断。
			 * 以下函数流程drop_cache释放pagecache执行的invalidate_mapping_pagevec函数，截断pagecache一致
			 *
			 *    执行find_lock_entries->find_get_entry->if (!folio || xa_is_value(folio))，从xarray tree得到page并判断合法，上边一判断
			 * 1：执行find_lock_entries->if (!folio_try_get_rcu(folio)) 令page引用加1
			 * 2：执行find_lock_entries->if (unlikely(folio != xas_reload(xas))) page引用计数加1后再判断page是否被其他进程释放了
			 * 3：执行find_lock_entries->if (!folio_trylock(folio)) 加锁
			 * 4：执行find_lock_entries->if (folio->mapping != mapping) 判断folio是否被其他进程释放了并重新分配给新的文件
			 * 5：执行mapping_evict_folio()，判断page是否是dirty、writeback文件页
			 * 6：执行mapping_evict_folio()，判断page引用计数计数异常，尤其是mmaped文件页。这里需要回收mmap文件页
			 * 7：执行mapping_evict_folio()->if (folio_has_private(folio)...)释放page bh
			 * 8：执行mapping_evict_folio()->remove_mapping(mapping, folio)，把page从xrray tree剔除
			 * 9：folio_unlock解锁
			 * */


            /*1：page引用计数加1，如果page引用计数原本是0则if成立，说明page已经被其他进程释放了*/
            if (!folio_try_get_rcu(folio)){
				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx folio_try_get_rcu fail\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags);
			    continue;
			}

            /*2：page被其他进程释放了，if成立。上边folio_try_get_rcu()可能失败，被其他进程抢先令page引用计数减1并释放掉，
			 *   再被新的文件分配。此时下边的if就不成立了，因为这个page即p_file_area->pages[i]已经被释放了*/
			if (unlikely(folio != rcu_dereference(p_file_area->pages[i]))) {
			    /* 参照find_lock_entries()，在folio_try_get_rcu(folio)后，如果folio内存回收失败，必须folio_put(folio)令folio
				 * 引用计数减1。而符合内存回收条件的page，在folio_batch_release()中会令page引用计数自动减1*/
			    folio_put(folio);
				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx folio != p_file_area->pages[i]\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags);
				continue;
			}

			/*3：folio加锁。这里是异步内存回收线程，获取锁失败而休眠也允许，因此把trylock_page改为lock_page*/
			folio_lock(folio);

			/*4：如果page被其他进程回收，又被新的进程访问了，分配给了新的文件，page->mapping指向了新的文件，不参与内存回收*/
			/*有个隐藏很深的bug，如果folio->mapping和file_area->mapping都是NULL，这个if不成立，把这种有问题的folio错误放过了。因此要加上if(NULL == p_file_area->mapping)是NULL的情况*/
			if(unlikely(folio->mapping != mapping) || (NULL == folio->mapping)){
				/* 如果file_area已经被iput()，这是异常情况要panic。这种情况是绝对不可能的。因为iput()过程是：file_area的page都释放了，然后再把
				 * p_file_area->mapping=NULL。代码代码执行到这里，file_area的folio肯定不是NULL，smp_rmb()后，p_file_area->mapping肯定不是NULL。
				 * 如果这里p_file_area->mapping却是NULL，则触发panic*/
				smp_rmb();
				if(NULL == p_file_area->mapping)
					panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx page->mapping:0x%llx != mapping:0x%llx NULL\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags,(u64)folio->mapping,(u64)p_file_stat_base->mapping);

			    /*参照find_lock_entries()对内存回收失败的page的处理，先folio_unlock后folio_put*/
				folio_unlock(folio);
				folio_put(folio);

				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx page->mapping:0x%llx != mapping:0x%llx\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags,(u64)folio->mapping,(u64)mapping);
				continue;
			}

            /*必须判断page没有kswaspd等进程释放掉后，再判断is_file_area_page_bit_set、folio_test_anon()等等*/
            if(xa_is_value(folio) || !is_file_area_page_bit_set(p_file_area,i)){
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx xa_is_value or file_area_bit error!!!!!!\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags);
			}
			/*如果page映射了也表页目录，这是异常的，要给出告警信息!!!!!!!!!!!!!!!!!!!还有其他异常状态。但实际调试
			 *遇到过page来自tmpfs文件系统，即PageSwapBacked(page)，最后错误添加到inacitve lru链表，但没有令inactive lru
			 *链表的page数加1，最后导致隔离page时触发mem_cgroup_update_lru_size()中发现lru链表page个数是负数而告警而crash*/
			if (unlikely(folio_test_anon(folio))|| unlikely(PageCompound(&folio->page)) || unlikely(folio_test_swapbacked(folio))){
					panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags);
			}


			/*5：page是脏页或者writeback页或者不可回收页，不参与内存回收*/
			if (folio_test_dirty(folio) || folio_test_writeback(folio) || folio_test_unevictable(folio)){
				folio_unlock(folio);
				folio_put(folio);

				if(0 == file_area_page_reclaim_fail){
					file_area_page_reclaim_fail = 1;
					printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx mapping:0x%llx page dirty or writeback or unevictable\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags,(u64)mapping);
				}

				continue;
			}

			/*6：mmap文件页的处理，清理掉mmap映射。仿照shrink_page_list()对mmap page的处理*/
			if (folio_mapped(folio)){
				enum ttu_flags flags = TTU_BATCH_FLUSH;
				mmap_page_count ++;

				try_to_unmap(folio, flags);
				/*try_to_unmap()后如果page还有mmap映射，说明内存回收失败*/
				if (folio_mapped(folio)) {
					folio_unlock(folio);
					folio_put(folio);

					printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx mapping:0x%llx try_to_unmap fail\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags,(u64)mapping);
					continue;
				}
			}

			/*7：释放page bh，page引用计数减1。照shrink_page_list()对page bh的处理
			 *   filemap_release_page()最后一个参数表示释放page bh时是否允许等待，0不允许，GFP_KERNEL允许，内存都有用!!!!!!!!!*/
			if (folio_has_private(folio) && !filemap_release_folio(folio, GFP_KERNEL)){
				folio_unlock(folio);
				folio_put(folio);

				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx mapping:0x%llx filemap_release_page fail\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,folio->flags,(u64)mapping);
				continue;
			}
					
			/*8：把page从radix tree剔除，并令page引用计数减1。流程跟invalidate_mapping_pagevec->mapping_evict_folio截断pagecache一致。
			 *而不再参照truncate_inode_pages_range截断pagecache流程了。因为前者是尝试截断pagecache，后者是强制，本场景跟前者一样*/
			ret = remove_mapping(mapping, folio);

			/*9：page解锁*/
			folio_unlock(folio);

			/* 到这里说明page内存回收失败了，按照page的属性再移动回lru链表。为什么要这样处理？这个流程跟invalidate_mapping_pagevec()
			 * 一样，看注释是把page移动到inacitve lru链表尾，这样该page就会尽快被回收掉。其实想想，这个处理也是多余的。另外要注意，
			 * 这种page在remove_mapping()中从radix tree剔除失败了，page引用计数没有减1，这种page在folio_batch_release()是不会释放回伙伴系统的*/
			if (!ret) {
				deactivate_file_folio(folio);
			}

			/*!!!!!!到这里，page才真正符合内存回收的条件。下边把这些folio保存到fbatch->folios[]数组，然后回收掉!!!!!!*/

			if(file_stat_in_test_base(p_file_stat_base))
				printk("%s file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d global_age:%d to free_pages\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age,p_hot_cold_file_global->global_age);

			/* 把folio保存到fbatch->folios[]数组。该数组容量15，数组满了返回0，释放掉fbatch->folios[]里的page后，在继续遍历其他page。
			 * 这个流程跟truncate_inode_pages_range->find_lock_entries()遍历folio到fbatch->folios[]数组，数组满了则开始回收
			 * fbatch->folios[]数组里的page。然后再执行find_lock_entries()遍历folio到fbatch->folios[]数组*/
			if (!folio_batch_add(&fbatch, folio)){
				page_count = folio_batch_count(&fbatch);	
#if 0    
				/* 把page从radix tree剔除，整个流程跟invalidate_mapping_pagevec或truncate_inode_pages_range截断pagecache一致
				 * 最新改动，不再参照truncate_inode_pages_range了，page从radix tree剔除放到前边了*/
				delete_from_page_cache_batch(mapping, &fbatch);
				/*page解锁*/
				for (j = 0; j < folio_batch_count(&fbatch); j++)
					folio_unlock(fbatch.folios[j]);
#endif				   
				/*这里真正把fbatch->folios[]保存的folio释放回伙伴系统，并自动把fbatch->nr清0，表示fbatch->folios[]数组空了，后续可以继续向fbatch->folios[]保存page*/
				folio_batch_release(&fbatch);

				/*注意，folio_batch_release()里可能会回收page失败，因此真实回收的page数 <= page_count，因此free_pages可能比真实回收的page数大*/
				free_pages += page_count;
				for(j = 0;j < page_count;j ++){
					if(folio_ref_count(fbatch.folios[j]) != 0 ){
						printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx index:%ld mapping:0x%llx folio_ref_count:%d != 0 !!!!!!\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)fbatch.folios[j],fbatch.folios[j]->flags,fbatch.folios[j]->index,(u64)mapping,folio_ref_count(fbatch.folios[j]));
					}
				}
			}
		}

		
		/* cache文件的file_area，如果有mmap page，则标记file_area in_mmap标记。mmap的文件不再处理，因为mmap文件的file_area
		 * 每次执行get_file_area_age()都会遍历该file_area的所有page，而cache文件执行get_file_area_age()，直接返回file_area_age，
		 * 而没有遍历file_area的page，正好趁着内存回收函数遍历file_area的page的机会，判断一下该file_area是否有mmap page，节省性能
		 *
		 * 还有一点，遇到过mmap文件的file_area被判定为cache file_area，但是它的mmap page每次内存回收都因访问而内存回收失败。导致
		 * file_area被判定为refault file_area，但是却无法把最新的globe_age更新到file_area_age，因为它是cache file_area。这导致
		 * 该file_area的page再次因file_area_age很小而从refault链表移动到temp链表，再移动到free链表，再次参与内存回收，再次因
		 * mmap page被访问，pte access bit置位而内存回收失败。就这样一直在循环，做无用功。针对这种情况，要把file_area的cache标记
		 * 清理掉，后续因为它所属文件是mmap文件，从而get_file_area_age时走慢速分支，就可以检测pte access bit更新file_area_age了。
		 * */
		if(mmap_page_count > 0){
			if(mmap_page_count > PAGE_COUNT_IN_AREA)
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x mmap_page_count:%d error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,mmap_page_count);

			if(file_stat_in_cache_file_base(p_file_stat_base) && !file_area_in_mmap(p_file_area)){
				set_file_area_in_mmap(p_file_area);
				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x  to mmap\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
			}

			if(file_stat_in_mmap_file_base(p_file_stat_base) && file_area_in_cache(p_file_area)){
				clear_file_area_in_cache(p_file_area);
				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x clear cache\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
			}
		}

		if(file_area_real_free){
			/*为了降低refault page高的文件的refault率，限制内存回收时扫描这个文件的page数。如果不限制，scan_file_area_max_for_memory_reclaim是-1*/
			if(scan_file_area_count ++  > scan_file_area_max_for_memory_reclaim)
				break;
		}

		/* 有些文件参与内存回收file_stat->warm等链表，有大量的0个page的file_area，内存回收时如果遍历到file_stat->warm链表有大量的0个page的file_area，
		 * 提前break退出，不浪费时间。这段代码放到for循环开头了*/
#if 0		
		if(scan_file_area_count_for_zero_page++ > 64){
			if(zero_page_file_area_count > 60){
				/* 注意，之类不能直接return 返回，因为folio_batch_count(&fbatch)可能还包含未释放的page，注意
				 * cold_file_isolate_lru_pages_and_shrink()函数不管合适都不能中途直接return，必须从函数最后
				 * return*/
				printk("%s file_stat:0x%llx status:0x%x too much zero_page_file_area_count:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,zero_page_file_area_count);
				//goto direct_return;
				break;
			}
			zero_page_file_area_count = 0;
			scan_file_area_count_for_zero_page = 0;
			/*防止for循环耗时太长导致本地cpu没有调度*/
			cond_resched();
		}
#endif
	}

	//unlock_file_stat(p_file_stat);
	//rcu_read_unlock();
#if 0
	file_inode_unlock(p_file_stat_base);
#endif

//direct_return:
	/*上边的for循环，存在folio_batch_add(fbatch, folio)向fbatch->folios[]数组保存folio后，后续执行形如
	 * if(unlikely(folio->mapping != mapping))判断导致for循环提前中断。如此这些fbatch->folios[]数组保存
	 * folio就没办法回收了，于是这里强制回收掉*/
	page_count = folio_batch_count(&fbatch);
	if(page_count){
		/*这里真正把fbatch->folios[]保存的folio释放回伙伴系统，并自动把fbatch->nr清0，表示fbatch->folios[]数组空了，后续可以继续向fbatch->folios[]保存page*/
		folio_batch_release(&fbatch);

		/*注意，folio_batch_release()里可能会回收page失败，因此真实回收的page数 <= page_count，因此free_pages可能比真实回收的page数大*/
		free_pages += page_count;
		for(j = 0;j < page_count;j ++){
			if(folio_ref_count(fbatch.folios[j]) != 0 ){
				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx index:%ld mapping:0x%llx folio_ref_count:%d != 0 !!!!!!\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)fbatch.folios[j],fbatch.folios[j]->flags,fbatch.folios[j]->index,(u64)mapping,folio_ref_count(fbatch.folios[j]));
			}
		}
	}

	if(memory_reclaim_info_for_one_warm_list){
		memory_reclaim_info_for_one_warm_list->scan_file_area_count_in_reclaim = scan_file_area_count_in_reclaim;
		memory_reclaim_info_for_one_warm_list->scan_zero_page_file_area_count_in_reclaim = scan_zero_page_file_area_count_in_reclaim;
		memory_reclaim_info_for_one_warm_list->scan_warm_file_area_count = scan_warm_file_area_count;

		p_hot_cold_file_global->memory_reclaim_info.scan_file_area_count_reclaim_fail = scan_file_area_count_reclaim_fail;
		memory_reclaim_info_for_one_warm_list->reclaim_pages_count = free_pages;
	}

	p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count = free_pages;
	p_hot_cold_file_global->alreay_reclaim_pages += free_pages;
	p_hot_cold_file_global->all_reclaim_pages_one_period += free_pages;
	return scan_file_area_count_in_reclaim;
}

#if 0
//遍历p_file_stat对应文件的file_area_free链表上的file_area结构，找到这些file_area结构对应的page，这些page被判定是冷页，可以回收
unsigned long cold_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,
		struct list_head *file_area_free/*,struct list_head *file_area_have_mmap_page_head*/)
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
	//int find_file_area_have_mmap_page;
	unsigned int find_mmap_page_count_from_cache_file = 0;
	char print_once = 1;
	unsigned char mmap_page_count = 0;

	/*char file_name_path[MAX_FILE_NAME_LEN];
	memset(file_name_path,0,sizeof(&file_name_path));
	get_file_name(file_name_path,p_file_stat_base);*/

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
	//
#if 0//这个加锁放到遍历file_stat内存回收，最初执行的get_file_area_from_file_stat_list()函数里了，这里不再重复加锁
	if(file_inode_lock(p_file_stat_base) <= 0){
		printk("%s file_stat:0x%llx status 0x%x inode lock fail\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		return 0;
	}
#endif	
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
		//find_file_area_have_mmap_page = 0;

		/*每遍历一个file_area，都要先对mmap_page_count清0，然后下边遍历到每一个mmap page再加1*/
		mmap_page_count = 0;
		//得到file_area对应的page
		for(i = 0;i < PAGE_COUNT_IN_AREA;i ++){
			folio = p_file_area->pages[i];
			if(!folio || folio_is_file_area_index(folio)){
				if(shrink_page_printk_open1)
					printk("%s file_area:0x%llx status:0x%x folio NULL\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

				/*如果一个file_area的page全都释放了，则file_stat->pages[0/1]就保存file_area的索引。然后第一个page又被访问了，
				 *然后这个file_area被使用。等这个file_area再次内存回收，到这里时，file_area->pages[1]就是file_area_index*/
				if(folio_is_file_area_index(folio) && print_once){
					print_once = 0;
					if(shrink_page_printk_open_important)
					    printk(KERN_ERR"%s file_area:0x%llx status:0x%x folio_is_file_area_index!!!!!!!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
				}

				continue;
			}
			page = &folio->page;
			if (page && !xa_is_value(page)) {
			    /*异步内存回收线程，获取锁失败而休眠也允许，因此把trylock_page改为lock_page*/
				/*if (!trylock_page(page)){
					continue;
				}*/
				lock_page(page);

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
			#if 0	
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
					if(shrink_page_printk_open_important)
					    printk("%s file:%s file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx mapping:0x%llx mmapped\n",__func__,file_name_path,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,(u64)page,page->flags,(u64)mapping);

					find_file_area_have_mmap_page = 1;
					find_mmap_page_count_from_cache_file ++;
					//break;
					continue;
				}
             #else
			     if(page_mapped(page))
				     mmap_page_count ++;
			 #endif

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
				
				if(!is_file_area_page_bit_set(p_file_area,i))
				    panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx file_area_bit error!!!!!!\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)page,page->flags);

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
					isolate_pages += shrink_inactive_list_async(move_page_count,lruvec,p_hot_cold_file_global,/*0*/1,LRU_INACTIVE_FILE);

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

		/* cache文件的file_area，如果有mmap page，则标记file_area in_mmap标记。mmap的文件不再处理，因为mmap文件的file_area
		 * 每次执行get_file_area_age()都会遍历该file_area的所有page，而cache文件执行get_file_area_age()，直接返回file_area_age，
		 * 而没有遍历file_area的page，正好趁着内存回收函数遍历file_area的page的机会，判断一下该file_area是否有mmap page，节省性能
		 *
		 * 还有一点，遇到过mmap文件的file_area被判定为cache file_area，但是它的mmap page每次内存回收都因访问而内存回收失败。导致
		 * file_area被判定为refault file_area，但是却无法把最新的globe_age更新到file_area_age，因为它是cache file_area。这导致
		 * 该file_area的page再次因file_area_age很小而从refault链表移动到temp链表，再移动到free链表，再次参与内存回收，再次因
		 * mmap page被访问，pte access bit置位而内存回收失败。就这样一直在循环，做无用功。针对这种情况，要把file_area的cache标记
		 * 清理掉，后续因为它所属文件是mmap文件，从而get_file_area_age时走慢速分支，就可以检测pte access bit更新file_area_age了。
		 * */
		if(mmap_page_count > 0){
			if(mmap_page_count > PAGE_COUNT_IN_AREA)
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x mmap_page_count:%d error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,mmap_page_count);
			if(file_stat_in_cache_file_base(p_file_stat_base) && !file_area_in_mmap(p_file_area)){
				set_file_area_in_mmap(p_file_area);
				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x  to mmap\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
			}

			if(file_stat_in_mmap_file_base(p_file_stat_base) && file_area_in_cache(p_file_area)){
				clear_file_area_in_cache(p_file_area);
				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x clear cache\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
			}
		}

#if 0	
		/* cache文件file_area内存回收时，发现file_area里的page至少一个page发现是mmap的，则该file_area移动到
		 * file_area_have_mmap_page_head，后续回收mmap的文件页。但file_area_have_mmap_page_head链表不能是NULL！否则说明此时在回收
		 * mmap文件含有cache page的file_area里的page，特例，不能按照cache文件遇到mmap page的file_area处理，怕陷入死循环*/
		if(NULL != file_area_have_mmap_page_head &&find_file_area_have_mmap_page)
			list_move(&p_file_area->file_area_list,file_area_have_mmap_page_head);
#endif		

	}

	//unlock_file_stat(p_file_stat);
	//rcu_read_unlock();
#if 0
	file_inode_unlock(p_file_stat_base);
#endif
	//当函数退出时，如果move_page_count大于0，则强制回收这些page
	if(move_page_count > 0){
		if(lruvec)
			spin_unlock_irq(&lruvec->lru_lock);

		//回收inactive lru链表尾的page，这些page刚才才移动到inactive lru链表尾
		isolate_pages += shrink_inactive_list_async(move_page_count,lruvec,p_hot_cold_file_global,/*0*/1,LRU_INACTIVE_FILE);

	}else{
		if(lruvec)
			spin_unlock_irq(&lruvec->lru_lock);
	}

	p_hot_cold_file_global->hot_cold_file_shrink_counter.find_mmap_page_count_from_cache_file += find_mmap_page_count_from_cache_file;
	return isolate_pages;
}
#endif
static inline void move_file_stat_to_global_delete_list(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned char file_type,char is_cache_file)
{
	if(is_cache_file)
		spin_lock(&p_hot_cold_file_global->global_lock);
	else
		spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);

	/* 如果file_stat有in_delete标记则移动到global delete链表，但如果有in_delete_file标记则crash，global temp链表上的file_stat不可能有in_delete_file标记.
	 * 无语了，竟然因为这个设计，触发了crash。场景是：poweroff关机截断所有文件时，iput()->__destroy_inode_handler_post()截断文件pagecache，
	 * 标记file_stat的in_delete和in_delete_file标记。正好此时异步内存回收线程先 get_file_area_from_file_stat_list()遍历global temp链表
	 * 得到该file_stat。得到file_stat后，iput()->__destroy_inode_handler_post()里标记了file_stat的in_delete和in_delete_file标记。
	 * 回到异步内存回收线程，get_file_area_from_file_stat_list()对该文件inode加锁失败，因为file_stat有delete标记，于是执行
	 * move_file_stat_to_global_delete_list()欲把file_stat移动到global delete链表。但是因为file_stat有in_delete标记，于是触发panic。
	 * 这是正常现象，要去掉这个判断。!!!!!!!!!!!!!!!!!!!!!
	 *
	 * 还有一个重大隐藏bug，move_file_stat_to_global_delete_list函数里，global_lock加锁后，如果file_stat有in_delete_file标记，
	 * 就不能再把file_stat移动到global delete链表了。因为这个file_stat已经被iput()->__destroy_inode_handler_post()并发标记
	 * in_delete_file标记，并移动到global delete链表了。bug就在这里，之前把file_stat移动到global delete链表，是判断file_stat
	 * 是否有in_delete标记，但是iput()->__destroy_inode_handler_post()并发释放inode时，是global_lock加锁后，标记file_stat的
	 * in_delete标记，然后file_stat_delete_protect_try_lock加锁成功，才会标记file_stat的in_delete_file标记，然后把file_stat
	 * 移动到global delete链表。因此，我的异步内存回收线程里，把file_stat移动到global delete链表，要判断file_stat是否有
	 * in_delete_file标记，而不是in_delete标记。!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 *
	 * file_stat被iput()并发移动到global delete链表的唯一标记是file_stat in_delete_file标记，而不是in_delete标记*/
	if(!file_stat_in_delete_base(p_file_stat_base)/* || file_stat_in_delete_file_base(p_file_stat_base)*/)
		panic("%s p_file_stat:0x%llx status:0x%x delete status fial\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
   
	/* file_stat被iput()并发移动到global delete链表的唯一标记是file_stat in_delete_file标记，而不是in_delete标记.
	 * 因此异步内存回收线程 把file_stat移动到global delete链表前要判断file_stat是否有in_delete_file标记。否则，就会
	 * 出现问题：iput()__destroy_inode_handler_post()里，global_lock加锁后，标记file_stat的in_delete标记，但是
	 * file_stat_delete_protect_try_lock加锁失败，而没有标记file_stat的in_delete_file，并把file_stat移动到global delete
	 * 链表。然后，异步内存回收线程，检测到file_stat有delete标记，于是执行move_file_stat_to_global_delete_list()
	 * 把file_stat移动global delete链表，global_lock加锁后，因file_stat有delete链表，就不再把file_stat移动到
	 * global delete链表了。于是这个file_stat就长时间残留在global temp链表，移动不到global delete链表*/
	if(/*!file_stat_in_delete_base(p_file_stat_base)*/ !file_stat_in_delete_file_base(p_file_stat_base)){
		/*凡是移动到global delete链表的file_stat都要设置in_delete_file标记*/
		set_file_stat_in_delete_file_base(p_file_stat_base);
		if(FILE_STAT_NORMAL == file_type){
			//p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
			//list_move(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.file_stat_delete_head);
			if(is_cache_file)
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_delete_head);
			else
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_delete_head);
		}
		else if(FILE_STAT_SMALL == file_type){
			//p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
			//list_move(&p_file_stat_small->hot_cold_file_list,&hot_cold_file_global_info.file_stat_small_delete_head);
			if(is_cache_file)
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_small_delete_head);
			else
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_small_delete_head);
		}
		else{
			if(FILE_STAT_TINY_SMALL != file_type)
				panic("%s file_stat:0x%llx status:0x%x file_stat_type:0x%x error\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,file_type);

			//p_file_stat_tiny_small = container_of(p_file_stat_base,struct file_stat_tiny_small,file_stat_base);
			//list_move(&p_file_stat_tiny_small->hot_cold_file_list,&hot_cold_file_global_info.file_stat_tiny_small_delete_head);
			if(is_cache_file)
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_tiny_small_delete_head);
			else
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_tiny_small_delete_head);
		}
	}
	if(is_cache_file)
	    spin_unlock(&p_hot_cold_file_global->global_lock);
	else
	    spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
}
/*遍历global file_stat_zero_file_area_head链表上的file_stat，如果file_stat对应文件长时间不被访问杂释放掉file_stat。如果file_stat对应文件又被访问了，
  则把file_stat再移动回 gloabl file_stat_temp_head、file_stat_large_file_head、file_stat_hot_head链表*/
static noinline void file_stat_has_zero_file_area_manage(struct hot_cold_file_global *p_hot_cold_file_global,struct list_head *file_stat_zero_list_head,unsigned int file_type,char is_cache_file)
{
	//struct file_stat *p_file_stat = NULL;
	struct file_stat_base *p_file_stat_base = NULL,*p_file_stat_base_temp;
	unsigned int scan_file_stat_max = 128,scan_file_stat_count = 0;
	unsigned int del_file_stat_count = 0;
	unsigned int file_stat_type;
	char file_stat_dec = 0;
	char file_stat_delete_lock = 0;
	char rcu_read_lock_flag = 0;
	spinlock_t *cache_or_mmap_file_global_lock;
	struct address_space *mapping;

	/*cache文件使用global_lock锁，mmap文件用的mmap_file_global_lock锁*/
	if(is_cache_file)
		cache_or_mmap_file_global_lock = &p_hot_cold_file_global->global_lock;
	else
		cache_or_mmap_file_global_lock = &p_hot_cold_file_global->mmap_file_global_lock;


	/*由于get_file_area_from_file_stat_list()向global file_stat_zero_file_area_head链表添加成员，这里遍历file_stat_zero_file_area_head链表成员，
	 *都是在异步内存回收线程进行的，不用spin_lock(&p_hot_cold_file_global->global_lock)加锁。除非要把file_stat_zero_file_area_head链表上的file_stat
	 *移动到 gloabl file_stat_temp_head、file_stat_large_file_head、file_stat_hot_head链表。*/

	/*在遍历global zero链表上的file_stat时，可能被并发iput()移动到了global delte链表，导致这里遍历到非法的file_stat。为了防护这种情况，
	 *要file_stat_lock。并且遍历到有delete标记的file_stat时，要移动到global delete链表。*/
	file_stat_delete_protect_lock(1);
	file_stat_delete_lock = 1;

	//向global  file_stat_zero_file_area_head添加成员是向链表头添加的，遍历则从链表尾巴开始遍历
	//list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->file_stat_zero_file_area_head,hot_cold_file_list){
	list_for_each_entry_safe_reverse(p_file_stat_base,p_file_stat_base_temp,file_stat_zero_list_head,hot_cold_file_list){

		file_stat_delete_protect_unlock(1);
		file_stat_delete_lock = 0;

		/* 现在normal、small、tiny_small的zero file_area的file_stat都是in_zero_file_area_list状态，是否有必要区分开分成3种呢?????????????
		 * 特殊处理，放到下边处理了*/
		if(!file_stat_in_zero_file_area_list_base(p_file_stat_base) /*|| file_stat_in_zero_file_area_list_error_base(p_file_stat_base)*/)
			panic("%s file_stat:0x%llx not in_zero_file_area_list status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

		/*file_stat和file_type不匹配则主动crash*/
		//is_file_stat_match_error(p_file_stat_base,file_type);

		/*如果文件mapping->rh_reserved1保存的file_stat指针不相等，crash，这个检测很关键，遇到过bug。
		 *这个检测必须放到遍历file_stat最开头，防止跳过。global_file_stat不会走到这个流程，不用做限制*/
		//is_file_stat_mapping_error(p_file_stat_base);这个判断放到下边file_inode_lock()里了，原因看注释

		/* 遍历global zero链表上的file_stat时，正好被iput()了。iput()只是标记delete，并不会把file_stat移动到global delete链表。
		 * 于是这里遍历到global zero链表上的file_stat时，必须移动到global delete链表。该函数将来可能会用file_stat->mapping->nrpages，
		 * 因此必须用file_inode_lock()确保inode和mapping不能被iput()释放掉*/
		//if(file_stat_in_delete_base(p_file_stat_base)){
		if(file_inode_lock(p_file_stat_base) <= 0){
			printk("%s file_stat:0x%llx delete status:0x%x file_type:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,file_type);
			move_file_stat_to_global_delete_list(p_hot_cold_file_global,p_file_stat_base,file_type,is_cache_file);
			goto next_file_stat;
		}
		mapping = p_file_stat_base->mapping;


		//如果file_stat对应文件长时间不被访问杂释放掉file_stat结构，这个过程不用spin_lock(&p_hot_cold_file_global->global_lock)加锁
		if(p_file_stat_base->file_area_count == 0 && p_hot_cold_file_global->global_age - p_file_stat_base->recent_access_age > p_hot_cold_file_global->file_stat_delete_age_dx){
			//如果返回值大于0说明file_stat对应文件被并发访问了，于是goto file_stat_access分支处理
			if(cold_file_stat_delete(p_hot_cold_file_global,p_file_stat_base,file_type) > 0)
				goto file_stat_access;

			/*这里，到这里有两种情况，1 : file_stat真的被释放了。2 : 在cold_file_stat_delete()里发现file_stat先被iput()
			 *标记delete标记，这种情况也认为global zero链表上的file_stat个数减少1，因为上边再次遍历这个file_stat时，会把这个
			 *file_stat移动到global delete链表*/

			/*file_stat被rcu异步释放了，但是提前rcu_read_lock了，这里置1，等到确定file_stat不会被释放再rcu_read_unlock放开*/
			rcu_read_lock_flag = 1;

			del_file_stat_count ++;
			//p_hot_cold_file_global->file_stat_count_zero_file_area --;下边统计减1了，这里不再减1
			file_stat_dec = 1;
		}
		/*如果p_file_stat->file_area_count大于0，说明最近被访问了，则把file_stat移动回 gloabl file_stat_temp_head、file_stat_large_file_head、
		 *file_stat_hot_head链表。hot_file_update_file_status()不会把file_stat移动回热文件或大文件或普通文件链表吗？不会，因为此时file_stat是
		 *in_zero_file_area_list状态，只有file_stat_in_temp_list状态才会移动到*/
		else if (p_file_stat_base->file_area_count > 0)
		{
file_stat_access:		
			//0个file_area的file_stat个数减1
			//p_hot_cold_file_global->file_stat_count_zero_file_area --;下边统计减1了，这里不再减1
			file_stat_dec = 1;

			/*file_stat可能是普通文件、中型文件、大文件，则移动到对应global 链表。也可能是热文件，不理会，
			 *异步内存回收线程里会处理*/
			spin_lock(cache_or_mmap_file_global_lock);
			if(!file_stat_in_delete_base(p_file_stat_base)){
				//clear_file_stat_in_file_stat_hot_head_list(p_file_stat);
				clear_file_stat_in_zero_file_area_list_base(p_file_stat_base);

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
				switch (file_type){
					case FILE_STAT_TINY_SMALL:
						/*file_stat移动到zero链表并没有清理原始属性，这里没有tiny_small且有其他干扰属性再crash*/
						if(file_stat_in_zero_file_area_list_error_base(p_file_stat_base)){
							if(file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base))
								clear_file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base);
							else if(file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base))
								clear_file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base);
							else
								panic("%s:1 file_stat:0x%llx not in_zero_file_area_list status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
						}

						if(p_file_stat_base->file_area_count > TINY_SMALL_TO_TINY_SMALL_ONE_AREA_LEVEL){
							set_file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base);
							if(is_cache_file)
								list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_tiny_small_file_head);
							else
								list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_tiny_small_file_head);
						}
						else{
							set_file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base);
							if(is_cache_file)
								list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_tiny_small_file_one_area_head);
							else
								list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_tiny_small_file_one_area_head);
						}
						break;
					case FILE_STAT_SMALL:
						if(!file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base) && file_stat_in_zero_file_area_list_error_base(p_file_stat_base))
							panic("%s:2 file_stat:0x%llx not in_zero_file_area_list status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
						
						//set_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
						if(is_cache_file)
							list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_small_file_head);
						else
							list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_small_file_head);
						break;
					case FILE_STAT_NORMAL:
						/*当mormal file_stat移动到zero链表后，就会清理掉file_stat的in_temp、middle、large等属性，
						 *于是当这些file_stat再移动回global temp、middle、large链表时，要根据file_area个数决定移动到哪个链表.
						 *还有一种情况漏掉了，就是writeonly文件，也是normal文件，当成temp、middle、large文件处理了*/
						file_stat_type = is_file_stat_file_type_ori(p_hot_cold_file_global,p_file_stat_base);

						if(file_stat_in_zero_file_area_list_error_base(p_file_stat_base)){
							/*有writeonly属性则清理掉，当成temp、middle、large文件处理了*/
							if(file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base))
								clear_file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base);
							else if(file_stat_in_file_stat_temp_head_list_base(p_file_stat_base))
								clear_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
							else if(file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base))
								clear_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
							else if(file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base))		
								clear_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
							else		
								panic("%s:3 file_stat:0x%llx not in_zero_file_area_list status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
						}

						//file_stat_type = get_file_stat_normal_type(p_file_stat_base);
						if(TEMP_FILE == file_stat_type){
							/*file_stat移动到global zero链表时，不再清理file_stat的in_temp状态，故这里不再清理*/
							set_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
							if(is_cache_file)
								list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_temp_head);
							else
								list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
						}else if(MIDDLE_FILE == file_stat_type){
							set_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
							if(is_cache_file)
								list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_middle_file_head);
							else
								list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_middle_file_head);
						}
						else if(LARGE_FILE == file_stat_type){
							set_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
							p_hot_cold_file_global->file_stat_large_count ++;
							if(is_cache_file)
								list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_large_file_head);
							else
								list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
						}else
							BUG();

						break;
					default:
						BUG();
				}
			}
			spin_unlock(cache_or_mmap_file_global_lock);
		}

		/*file_stat可能在上边cold_file_stat_delete()释放了，并且赋值file_stat->mapping=0。然后不能再使用file_inode_unlock()，因为它会使用file_stat->mapping->host获取inode，此时是非法内存访问*/
		//file_inode_unlock(p_file_stat_base);
		file_inode_unlock_mapping(mapping);

next_file_stat:
		file_stat_delete_protect_lock(1);
		file_stat_delete_lock = 1;
		/*如果遍历到global zero链表头，或者下一次遍历的file_stat被delete了，立即跳出遍历*/
		if(&p_file_stat_base_temp->hot_cold_file_list == file_stat_zero_list_head  || file_stat_in_delete_file_base(p_file_stat_base_temp))
			break;

		/*这个scan_file_stat_count超过max则break的判断，要放到for循环下边，原因看get_file_area_from_file_stat_list()。还有一个原因是for循环上边break会错过file_inode_unlock()解锁*/
		if++(scan_file_stat_count > scan_file_stat_max)
			break;

		if(rcu_read_lock_flag){
			rcu_read_lock_flag = 0;
			rcu_read_unlock();
		}
	}

	if(file_stat_delete_lock)
		file_stat_delete_protect_test_unlock(1);

	if(file_stat_dec){
		switch (file_type){
			case FILE_STAT_TINY_SMALL:
				p_hot_cold_file_global->file_stat_tiny_small_count_zero_file_area --;
				break;
			case FILE_STAT_SMALL:
				p_hot_cold_file_global->file_stat_small_count_zero_file_area --;
				break;
			case FILE_STAT_NORMAL:
				p_hot_cold_file_global->file_stat_count_zero_file_area --;
				break;
			default:
				BUG();
		}
	}

	spin_lock(cache_or_mmap_file_global_lock);
	/*本次遍历过的file_stat移动到链表头，让其他file_stat也得到遍历的机会*/
	if(&p_file_stat_base->hot_cold_file_list != file_stat_zero_list_head  && !file_stat_in_delete_base(p_file_stat_base)){
		/*将链表尾已经遍历过的file_stat移动到链表头，下次从链表尾遍历的才是新的未遍历过的file_stat。这个过程必须加锁*/
		if(can_file_stat_move_to_list_head(file_stat_zero_list_head,p_file_stat_base,F_file_stat_in_zero_file_area_list,is_cache_file))
			list_move_enhance(file_stat_zero_list_head,&p_file_stat_base->hot_cold_file_list);
	}
	spin_unlock(cache_or_mmap_file_global_lock);

	if(rcu_read_lock_flag){
		rcu_read_lock_flag = 0;
		rcu_read_unlock();
	}

	if(is_cache_file){
		p_hot_cold_file_global->hot_cold_file_shrink_counter.del_zero_file_area_file_stat_count += del_file_stat_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_zero_file_area_file_stat_count = scan_file_stat_count;
	}else{
		p_hot_cold_file_global->mmap_file_shrink_counter.del_zero_file_area_file_stat_count += del_file_stat_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.scan_zero_file_area_file_stat_count = scan_file_stat_count;
	}
}
static int normal_writeonly_file_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int file_stat_list_type,unsigned int scan_read_file_area_count_last)
{
	int file_not_solve_for_temp_middle_large = 0;
	unsigned int scan_read_file_area_count_dx;

	/*把writeonly文件移动到global file_stat_writeonly_file_head链表，但是要求该文件的pagecache个数必须大于1M*/
	if(!file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base)){
		if(p_file_stat_base->mapping->nrpages >= 64){
			switch(file_stat_list_type){
				case F_file_stat_in_file_stat_temp_head_list:
					clear_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
					break;
				case F_file_stat_in_file_stat_middle_file_head_list:
					clear_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
					break;
				case F_file_stat_in_file_stat_large_file_head_list:
					clear_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
					break;
				default:
					panic("%s writeonly file p_file_stat_base:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);
			}
			file_not_solve_for_temp_middle_large = 1;
			spin_lock(&p_hot_cold_file_global->global_lock);
			set_file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base);
			list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_writeonly_file_head);
			spin_unlock(&p_hot_cold_file_global->global_lock);
			p_hot_cold_file_global->in_writeonly_list_file_count ++;
			printk("%s file_stat:0x%llx status:0x%x mouve to global writeonly_list!!!!!!!!!\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		}
	}else{
		/*如果scan_read_file_area_count_dx大于0，说明从这个文件扫描到多个read file_area，需要清理掉*/
		scan_read_file_area_count_dx = p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_read_file_area_count_from_temp + p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_read_file_area_count_from_warm - scan_read_file_area_count_last;

		file_not_solve_for_temp_middle_large = 1;
		/*global writeonly_file_head链表上的文件的文件页都回收完成了，则把file_area降级到global normal temp 链表
		 * 。或者，扫描到多个read file_area，就把file_area降级到global temp链表*/
		if(!file_stat_in_writeonly_base(p_file_stat_base)){
			/*实际测试表明，有个名为b2bbd74490a8c1b6987385d2a501537a1c97b2cf41241的文件，在global->writeonly链表但是却被清理了
			 *file_stat_in_writeonly_base标记，有2千个page，当作writeonly回收，造成了5k的refault。遇到这种文件，要直接从
			 *global->writeonly链表移动走，不再做限制了*/
			/*if(scan_read_file_area_count_dx > 16  || 
					(p_file_stat_base->mapping->nrpages < 32 && p_hot_cold_file_global->global_age - p_file_stat_base->recent_access_age > WRITEONLY_FILE_MOVE_TO_TEMP_AGE_DX))*/
			{
				/*都没有文件页了，直接移动到globa temp链表*/
				spin_lock(&p_hot_cold_file_global->global_lock);
				clear_file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base);
				set_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_temp_head);
				spin_unlock(&p_hot_cold_file_global->global_lock);
				p_hot_cold_file_global->in_writeonly_list_file_count --;

				printk("%s writeonly file_stat:0x%llx status:0x%x scan_read_file_area_count_dx:%d nrpages:%ld mouve to global temp_list!!!!!!!!!\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_read_file_area_count_dx,p_file_stat_base->mapping->nrpages);
			}
		}
	}
	return file_not_solve_for_temp_middle_large;
}
void file_stat_temp_middle_large_file_change(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int file_stat_list_type, unsigned int normal_file_type,char is_cache_file)
{
	spinlock_t *cache_or_mmap_file_global_lock;

	/*cache文件使用global_lock锁，mmap文件用的mmap_file_global_lock锁*/
	if(is_cache_file)
		cache_or_mmap_file_global_lock = &p_hot_cold_file_global->global_lock;
	else
		cache_or_mmap_file_global_lock = &p_hot_cold_file_global->mmap_file_global_lock;

	switch(file_stat_list_type){
		case F_file_stat_in_file_stat_temp_head_list:
			/*普通文件升级到中型文件，必须要加锁。要考虑两种并发，都会并发修改global temp链表头
			 *1：iput()中会并发标记file_stat的delete，并且把file_stat从global temp链表头移动到global delete链表
			 2：读写文件进程执行__filemap_add_folio()向global temp添加file_stat到global temp链表头*/
			if(MIDDLE_FILE == normal_file_type){
				spin_lock(cache_or_mmap_file_global_lock);
				/*加锁后必须再判断一次file_stat的状态，如果file_stat被iput()并发标记delete了，就不能再移动file_stat到其他链表了。这点很重要!!!!!!!!!!!!*/
				if(!file_stat_in_delete_base(p_file_stat_base)){
					if(!file_stat_in_file_stat_temp_head_list_base(p_file_stat_base) || file_stat_in_file_stat_temp_head_list_error_base(p_file_stat_base))
						panic("%s file_stat:0x%llx status:0x%x not in temp_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

					clear_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
					set_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_middle_file_head);
					else 
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_middle_file_head);
				}
				spin_unlock(cache_or_mmap_file_global_lock);
			}
			/*普通文件升级到大文件，必须要加锁*/
			else if(LARGE_FILE == normal_file_type){
				spin_lock(cache_or_mmap_file_global_lock);
				/*加锁后必须再判断一次file_stat的状态，如果file_stat被iput()并发标记delete了，就不能再移动file_stat到其他链表了。这点很重要!!!!!!!!!!!!*/
				if(!file_stat_in_delete_base(p_file_stat_base)){
					if(!file_stat_in_file_stat_temp_head_list_base(p_file_stat_base) || file_stat_in_file_stat_temp_head_list_error_base(p_file_stat_base))
						panic("%s file_stat:0x%llx status:0x%x not in temp_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

					clear_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
					set_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_large_file_head);
					else 
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
				}
				spin_unlock(cache_or_mmap_file_global_lock);

				if(is_cache_file)
					p_hot_cold_file_global->file_stat_large_count ++;
			}

			break;
		case F_file_stat_in_file_stat_middle_file_head_list:
			/*中型文件降级到普通文件，必须要加锁*/
			if(TEMP_FILE == normal_file_type){
				spin_lock(cache_or_mmap_file_global_lock);
				/*加锁后必须再判断一次file_stat的状态，如果file_stat被iput()并发标记delete了，就不能再移动file_stat到其他链表了。这点很重要!!!!!!!!!!!!*/
				if(!file_stat_in_delete_base(p_file_stat_base)){
					if(!file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_middle_file_head_list_error_base(p_file_stat_base))
						panic("%s file_stat:0x%llx status:0x%x not in middle_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

					clear_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
					set_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_temp_head);
					else 
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
				}
				spin_unlock(cache_or_mmap_file_global_lock);
			}
			/*中型文件升级到大文件，不需要加锁。NO!!!必须要加锁，因为iput()中会并发标记file_stat的delete，并且把file_stat移动到global delete链表*/
			else if(LARGE_FILE == normal_file_type){
				spin_lock(cache_or_mmap_file_global_lock);
				/*加锁后必须再判断一次file_stat的状态，如果file_stat被iput()并发标记delete了，就不能再移动file_stat到其他链表了。这点很重要!!!!!!!!!!!!*/
				if(!file_stat_in_delete_base(p_file_stat_base)){
					if(!file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_middle_file_head_list_error_base(p_file_stat_base))
						panic("%s file_stat:0x%llx status:0x%x not in middle_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
					clear_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
					set_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_large_file_head);
					else 
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
				}
				spin_unlock(cache_or_mmap_file_global_lock);

				if(is_cache_file) 
					p_hot_cold_file_global->file_stat_large_count ++;
			}

			break;
		case F_file_stat_in_file_stat_large_file_head_list:
			/*大文件降级到普通文件，必须要加锁。NO!!!必须要加锁，因为iput()中会并发标记file_stat的delete，并且把file_stat移动到global delete链表*/
			if(TEMP_FILE == normal_file_type){
				spin_lock(cache_or_mmap_file_global_lock);
				/*加锁后必须再判断一次file_stat的状态，如果file_stat被iput()并发标记delete了，就不能再移动file_stat到其他链表了。这点很重要!!!!!!!!!!!!*/
				if(!file_stat_in_delete_base(p_file_stat_base)){
					if(!file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_large_file_head_list_error_base(p_file_stat_base))
						panic("%s file_stat:0x%llx status:0x%x not in large_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
					clear_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
					set_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_temp_head);
					else
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
				}
				spin_unlock(cache_or_mmap_file_global_lock);

				if(is_cache_file)
					p_hot_cold_file_global->file_stat_large_count --;
			}
			/*大文件升级到中型文件，不需要加锁.NO!!!必须要加锁，因为iput()中会并发标记file_stat的delete，并且把file_stat移动到global delete链表*/
			else if(MIDDLE_FILE == normal_file_type){
				spin_lock(cache_or_mmap_file_global_lock);
				/*加锁后必须再判断一次file_stat的状态，如果file_stat被iput()并发标记delete了，就不能再移动file_stat到其他链表了。这点很重要!!!!!!!!!!!!*/
				if(!file_stat_in_delete_base(p_file_stat_base)){
					if(!file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_large_file_head_list_error_base(p_file_stat_base))
						panic("%s file_stat:0x%llx status:0x%x not in large_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
					clear_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
					set_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_middle_file_head);
					else 
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_middle_file_head);
				}
				spin_unlock(cache_or_mmap_file_global_lock);

				if(is_cache_file)
					p_hot_cold_file_global->file_stat_large_count --;
			}

			break;
		default:
			panic("%s p_file_stat_base:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);
			break;
	}	

}
/*查看文件是否变成了热文件、大文件、普通文件，是的话则file_stat移动到对应global hot、large_file_temp、temp 链表*/
static noinline int file_stat_status_change_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,unsigned int file_stat_list_type,char is_cache_file,unsigned int scan_read_file_area_count_last)
{
	struct file_stat_base *p_file_stat_base = &p_file_stat->file_stat_base;

	/* 此时存在并发移动file_stat的情况：iput()释放file_stat，标记file_stat delete，并把file_stat移动到
	 * global delete链表。因此要global_lock加锁后再判断一次file_stat是否被iput()把file_stat移动到global delete链表了*/
	if(is_file_stat_hot_file(p_hot_cold_file_global,p_file_stat)){//热文件

		/*针对mmap文件，在get_file_area_age()函数里也会对mmap升级为热文件，这个函数也会，会不会冲突了？算了，这里也保留*/
		if(is_cache_file)
			spin_lock(&p_hot_cold_file_global->global_lock);
		else
			spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
		/*如果file_stat被iput()并发标记delete了，则不再理会*/
		if(!file_stat_in_delete_base(p_file_stat_base)){
			/*if(F_file_stat_in_file_stat_temp_head_list == file_stat_list_type)
			  clear_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
			  else if(F_file_stat_in_file_stat_middle_file_head_list == file_stat_list_type)
			  clear_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
			  else{ 
			  if(F_file_stat_in_file_stat_large_file_head_list != file_stat_list_type)
			  panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat,file_stat_list_type);
			  p_hot_cold_file_global->file_stat_large_count --;
			  clear_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
			  }*/
			switch(file_stat_list_type){
				case F_file_stat_in_file_stat_temp_head_list:
					clear_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
					break;
				case F_file_stat_in_file_stat_middle_file_head_list:
					clear_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
					break;
				case F_file_stat_in_file_stat_large_file_head_list:
					clear_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
					p_hot_cold_file_global->file_stat_large_count --;
					break;
				case F_file_stat_in_file_stat_writeonly_file_head_list:
					clear_file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base);
					break;
				default:
					panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat,file_stat_list_type);
			}

			set_file_stat_in_file_stat_hot_head_list_base(p_file_stat_base);
			if(is_cache_file){
				p_hot_cold_file_global->file_stat_hot_count ++;//热文件数加1 
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_hot_head);
			}
			else{
				p_hot_cold_file_global->mmap_file_stat_hot_count ++;//热文件数加1 
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_hot_head);
			}
			//p_file_stat->file_stat_hot_base_age = p_hot_cold_file_global->global_age;
		}
		if(is_cache_file)
			spin_unlock(&p_hot_cold_file_global->global_lock);
		else
			spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
	}else{
		int file_not_solve_for_temp_middle_large = 0;
		/* 走到这里流程的只有temp、middle、large 文件。并且只有cache文件才有writeonly标记。故只有writeonly的cache文件，if才成立。
		 * 但是存在一种情况，当writeonly文件被移动到global writeonly链表后，被读了，然后清理掉了in_writeonly标记，导致if不成立。
		 * 执行下边的file_stat_temp_middle_large_file_change就会crash。故要加file_stat_in_file_stat_writeonly_file_head_list判断*/
		if(file_stat_in_writeonly_base(p_file_stat_base) || file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base)){
			if(!is_cache_file)
				panic("%s p_file_stat:0x%llx file_stat_list_type:%d is mmap file\n",__func__,(u64)p_file_stat,file_stat_list_type);

			file_not_solve_for_temp_middle_large = normal_writeonly_file_solve(p_hot_cold_file_global,p_file_stat_base,file_stat_list_type,scan_read_file_area_count_last);
		}

		/*如果temp、middle、large文件成功移动到了了writeonly_list链表，则不再按照temp、middle、large原有相互转换流程处理*/
		if(0 == file_not_solve_for_temp_middle_large){
			/*根据file_stat的file_area个数判断文件是普通文件、中型文件、大文件*/
			unsigned int file_type = is_file_stat_file_type_ori(p_hot_cold_file_global,p_file_stat_base);

			file_stat_temp_middle_large_file_change(p_hot_cold_file_global,p_file_stat_base,file_stat_list_type,file_type,is_cache_file);
		}
	}

	return 0;
}
static void file_stat_other_list_file_area_solve_common(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,unsigned int file_area_type,unsigned int file_type)
{
	//struct file_area *p_file_area,*p_file_area_temp;
	//int scan_file_area_count = 0;
	//unsigned int file_area_hot_to_warm_list_count = 0;
	//unsigned int file_area_refault_to_warm_list_count = 0;
	unsigned int file_area_free_to_refault_list_count = 0;
	//unsigned int file_area_free_count = 0;
	//unsigned int file_area_in_list_type = -1;
	struct file_stat *p_file_stat = NULL;
	char file_stat_changed;
	unsigned int file_area_age_dx;
	char is_global_file_stat = file_stat_in_global_base(p_file_stat_base);
	char ret;

	/* 最新方案，iput()->find_get_entry_for_file_area()不再把file_area以file_area->file_area_delete移动到global_file_stat_delete
	 * 链表，而只是标记file_area的in_mapping_delete。异步内存回收线程看到file_area有in_maping_delete标记，再把file_area以
	 * file_area_list移动到global_file_stat_delete链表，此时没有并发问题。详细原因见find_get_entry_for_file_area()*/
	if(file_area_in_mapping_delete(p_file_area)){
find_global_file_area_in_mapping:
		if(!is_global_file_stat)
			panic("%s file_stat:0x%llx  file_area:0x%llx status:0x%x not in global\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

		printk("%s file_stat:0x%llx  file_area:0x%llx status:0x%x in_mapping_delete\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);
		if(file_stat_in_cache_file_base(p_file_stat_base))
			list_move(&p_file_area->file_area_list,&p_hot_cold_file_global->global_file_stat.file_area_delete_list);
		else
			list_move(&p_file_area->file_area_list,&p_hot_cold_file_global->global_mmap_file_stat.file_area_delete_list);

		return;
	}

	//mapcount的file_area降级到file_stat->warm链表，不看file_area_age
	if(file_area_type != (1 << F_file_area_in_mapcount_list)){
		//get_file_area_age()函数里，会把file_area转换成hot、mapcount file_area需要特别注意!!!!!!!!!!!!
		get_file_area_age(p_file_stat_base,p_file_area,/*file_area_age,*/p_hot_cold_file_global,file_stat_changed,file_type,is_global_file_stat);
		file_area_age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;

		if(file_stat_changed)
			panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_stat_changed error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
	}
		
	/*file_area_list_head来自file_stat->file_area_hot、file_stat->file_area_free,file_stat->file_area_refault链表头*/
	//list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,file_area_list_head,file_area_list){
	//	if(scan_file_area_count ++ > scan_file_area_max)
	//		break;

	/*file_stat和file_type不匹配则主动crash。global_file_stat在file_stat_other_list_file_area_solve()做了file_stat的match error判定*/
	if(!file_stat_in_global_base(p_file_stat_base))
		is_file_stat_match_error(p_file_stat_base,file_type);

	if(file_area_in_deleted(p_file_area))
		panic("%s file_stat:0x%llx  file_area:0x%llx status:0x%x mapping:0x%llx\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)p_file_area->mapping);

	switch (file_area_type){
		//case FILE_AREA_IN_HOT_LIST:
		case (1 << F_file_area_in_hot_list):
			unsigned int file_area_hot_to_temp_age_dx;

			if(!file_area_in_hot_list(p_file_area) || file_area_in_hot_list_error(p_file_area))
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_hot\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
			//if(-1 == file_area_in_list_type)
			//	file_area_in_list_type = F_file_area_in_hot_list;

			/*现在针对mmap的age_dx，在内存回收最开头执行的change_global_age_dx_for_mmap_file()函数里调整了，这里不再重复*/
			file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx;
			/*if(file_stat_in_cache_file_base(p_file_stat_base))
                file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx;
			else
				file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx + MMAP_FILE_HOT_TO_TEMP_AGE_DX;*/

			//file_stat->file_area_hot尾巴上长时间未被访问的file_area再降级移动回file_stat->file_area_temp链表头
			//if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > p_hot_cold_file_global->file_area_hot_to_temp_age_dx){
			if(file_area_age_dx > file_area_hot_to_temp_age_dx){
#if 0	
				/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
				if(file_area_in_ahead(p_file_area)){
					clear_file_area_in_ahead(p_file_area);
					/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
					if(IS_MEMORY_ENOUGH(p_hot_cold_file_global) || file_area_age_dx < p_hot_cold_file_global->file_area_reclaim_ahead_age_dx)
						break;
				}
#endif				
				//spin_lock(&p_file_stat->file_stat_lock); 
				clear_file_area_in_hot_list(p_file_area);

				if(FILE_STAT_TINY_SMALL == file_type){
					/*如果file_area有page则按照正常流程处理。否则说明没有page了，说明file_area的page大概率被kswapd或者直接内存回收释放掉了，那直接当成in_free的file_area处理*/
					if(file_area_have_page(p_file_area)){
						set_file_area_in_temp_list(p_file_area);
						p_file_stat_base->file_area_count_in_temp_list ++;
					}else{
						/*if(!file_area_in_free_kswapd(p_file_area))
				            printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_area_type:0x%x not in free_kswapd\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,file_area_type);*/
						set_file_area_in_free_list(p_file_area);
					}
				}
				else if(FILE_STAT_SMALL == file_type){
					/*如果file_area有page则按照正常流程处理。否则说明没有page了，说明file_area的page大概率被kswapd或者直接内存回收释放掉了，那直接当成in_free的file_area处理*/
					if(file_area_have_page(p_file_area)){
						/*small文件只是设置file_area in temp状态，不移动file_area到其他链表。small文件的话，
						 *这个file_area就以in_temp状态停留在file_area_other链表上，等下次遍历到该file_area，再考虑是回收，还是移动到temp链表。
						 *不行，small文件的file_area_other链表上不能有temp属性的file_area，必须立即移动到file_area_other链表，这个过程要加锁*/
						spin_lock(&p_file_stat_base->file_stat_lock);
						set_file_area_in_temp_list(p_file_area);
						p_file_stat_base->file_area_count_in_temp_list ++;
						/*移动到tail，方便下个周期就能从file_stat->temp链表尾遍历到该file_area，然后最快进行内存回收*/
						list_move_tail(&p_file_area->file_area_list,&p_file_stat_base->file_area_temp);
						spin_unlock(&p_file_stat_base->file_stat_lock); 
					}else{
						/*if(!file_area_in_free_kswapd(p_file_area))
				            printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_area_type:0x%x not in free_kswapd\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,file_area_type);*/
						set_file_area_in_free_list(p_file_area);
					}
				}
				else if(FILE_STAT_NORMAL == file_type){
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

					/*如果file_area有page则按照正常流程处理。否则说明没有page了，说明file_area的page大概率被kswapd或者直接内存回收释放掉了，那直接当成in_free的file_area处理*/
					if(file_area_have_page(p_file_area)){
						if(file_stat_in_cache_file_base(p_file_stat_base))
							p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_hot_to_warm_list_count += 1;
						else
							p_hot_cold_file_global->mmap_file_shrink_counter.file_area_hot_to_warm_list_count += 1;

						/*遇到file_area_hot_count是负数的情况，这里要做预防*/
						if(0 == p_file_stat->file_area_hot_count)
							panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_area_hot_count == 0 error !!!\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
						//file_stat的热file_area个数减1
						p_file_stat->file_area_hot_count --;
						//set_file_area_in_warm_list(p_file_area);
						list_num_update(p_file_area,POS_WARM);
						/*hot链表上长时间没访问的file_area现在移动到file_stat->warm链表，而不是file_stat->temp链表，这个不用spin_lock(&p_file_stat->file_stat_lock)加锁*/
						list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
						//printk("3 %s:file_stat:0x%llx file_area:0x%llx status:0x%x age:%d hot to warm\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
						//spin_unlock(&p_file_stat->file_stat_lock);	    
					}else{
						/*if(!file_area_in_free_kswapd(p_file_area))
				            printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_area_type:0x%x not in free_kswapd\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,file_area_type);*/
						set_file_area_in_free_list(p_file_area);
						list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free);
					}
				}else
					BUG();

				if(file_stat_in_test_base(p_file_stat_base))
					printk("%s:file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d global_age:%d hot to warm\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age,p_hot_cold_file_global->global_age);
			}

			break;
			//case FILE_AREA_IN_REFAULT_LIST:
		case (1 << F_file_area_in_refault_list):
			unsigned int file_area_refault_to_temp_age_dx;
			if(!file_area_in_refault_list(p_file_area) || file_area_in_refault_list_error(p_file_area))
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_refault\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
			//if(-1 == file_area_in_list_type)
			//	file_area_in_list_type = F_file_area_in_refault_list;

			/*现在针对mmap的age_dx，在内存回收最开头执行的change_global_age_dx_for_mmap_file()函数里调整了，这里不再重复*/
			file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_refault_to_temp_age_dx;
			/*if(file_stat_in_cache_file_base(p_file_stat_base))
				file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_refault_to_temp_age_dx;
			else
				file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_refault_to_temp_age_dx + MMAP_FILE_REFAULT_TO_TEMP_AGE_DX;*/

			//file_stat->file_area_hot尾巴上长时间未被访问的file_area再降级移动回file_stat->file_area_temp链表头
			//if(p_hot_cold_file_global->global_age - p_file_area->file_area_age >  p_hot_cold_file_global->file_area_refault_to_temp_age_dx){
			if(file_area_age_dx >  file_area_refault_to_temp_age_dx){
#if 0				
				/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
				if(file_area_in_ahead(p_file_area)){
					clear_file_area_in_ahead(p_file_area);
					/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
					if(IS_MEMORY_ENOUGH(p_hot_cold_file_global) || file_area_age_dx < p_hot_cold_file_global->file_area_reclaim_ahead_age_dx)
						break;
				}
#endif
				clear_file_area_in_refault_list(p_file_area);
				if(FILE_STAT_TINY_SMALL == file_type){
					/*如果file_area有page则按照正常流程处理。否则说明没有page了，说明file_area的page大概率被kswapd或者直接内存回收释放掉了，那直接当成in_free的file_area处理*/
					if(file_area_have_page(p_file_area)){
						set_file_area_in_temp_list(p_file_area);
						p_file_stat_base->file_area_count_in_temp_list ++;

					}else{
						/*if(!file_area_in_free_kswapd(p_file_area))
				            printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_area_type:0x%x not in free_kswapd\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,file_area_type);*/

						set_file_area_in_free_list(p_file_area);
					}
				}
				else if(FILE_STAT_SMALL == file_type){
					/*如果file_area有page则按照正常流程处理。否则说明没有page了，说明file_area的page大概率被kswapd或者直接内存回收释放掉了，那直接当成in_free的file_area处理*/
					if(file_area_have_page(p_file_area)){
						/*small文件只是设置file_area in temp状态，不移动file_area到其他链表。small文件的话，
						 *这个file_area就以in_temp状态停留在file_area_other链表上，等下次遍历到该file_area，再考虑是回收，还是移动到temp链表。
						 *不行，small文件的file_area_other链表上不能有temp属性的file_area，必须立即移动到file_area_other链表，这个过程要加锁*/
						spin_lock(&p_file_stat_base->file_stat_lock);
						set_file_area_in_temp_list(p_file_area);
						p_file_stat_base->file_area_count_in_temp_list ++;
						list_move(&p_file_area->file_area_list,&p_file_stat_base->file_area_temp);
						spin_unlock(&p_file_stat_base->file_stat_lock); 
					}else{
						/*if(!file_area_in_free_kswapd(p_file_area))
				            printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_area_type:0x%x not in free_kswapd\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,file_area_type);*/
						set_file_area_in_free_list(p_file_area);
					}
				}
				else if(FILE_STAT_NORMAL == file_type){
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
					/*如果file_area有page则按照正常流程处理。否则说明没有page了，说明file_area的page大概率被kswapd或者直接内存回收释放掉了，那直接当成in_free的file_area处理*/
					if(file_area_have_page(p_file_area)){
						//file_area_refault_to_warm_list_count ++;
						//p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_refault_to_warm_list_count += file_area_refault_to_warm_list_count;
						if(file_stat_in_cache_file_base(p_file_stat_base))
							p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_refault_to_warm_list_count += 1;
						else
							p_hot_cold_file_global->mmap_file_shrink_counter.file_area_refault_to_warm_list_count += 1;
						//spin_lock(&p_file_stat->file_stat_lock);	    
						//set_file_area_in_warm_list(p_file_area);
						list_num_update(p_file_area,POS_WARM);
						/*refault链表上长时间没访问的file_area现在移动到file_stat->warm链表，而不是file_stat->temp链表，这个不用spin_lock(&p_file_stat->file_stat_lock)加锁*/
						list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
						//printk("4 %s:file_stat:0x%llx file_area:0x%llx status:0x%x age:%d refault to warm\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
						//spin_unlock(&p_file_stat->file_stat_lock);	    
					}else{
						/*if(!file_area_in_free_kswapd(p_file_area))
				            printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_area_type:0x%x not in free_kswapd\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,file_area_type);*/
						list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free);
						set_file_area_in_free_list(p_file_area);
					}
				}else
					BUG();

				if(file_stat_in_test_base(p_file_stat_base))
					printk("%s:file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d global_age:%d refault to warm\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age,p_hot_cold_file_global->global_age);
			}

			break;
			//case FILE_AREA_IN_FREE_LIST:
		case (1 << F_file_area_in_free_list):
			unsigned int file_area_free_age_dx,file_area_temp_to_cold_age_dx;
			//if(-1 == file_area_in_list_type)
			//	file_area_in_list_type = F_file_area_in_free_list;
			if(!file_area_in_free_list(p_file_area) || file_area_in_temp_list(p_file_area) /*|| file_area_in_warm_list(p_file_area)*/ || file_area_in_mapcount_list(p_file_area))
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x error in_free\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			/*现在针对mmap的age_dx，在内存回收最开头执行的change_global_age_dx_for_mmap_file()函数里调整了，这里不再重复*/
			file_area_free_age_dx = p_hot_cold_file_global->file_area_free_age_dx;
			file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			/*if(file_stat_in_cache_file_base(p_file_stat_base)){
                file_area_free_age_dx = p_hot_cold_file_global->file_area_free_age_dx;
				file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
			}else{
                file_area_free_age_dx = p_hot_cold_file_global->file_area_free_age_dx + MMAP_FILE_COLD_TO_FREE_AGE_DX;
				file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx + MMAP_FILE_TEMP_TO_COLD_AGE_DX;
			}*/
			/*在遍历file_stat->temp和file_stat->warm链表上的file_area时，判断是是冷file_area而要参与内存回收，于是
			 * clear_file_area_in_temp_list(p_file_area);//这里file_area被频繁访问了，清理的in_temp还没有生效，file_area在update函数又被设置了in_hot标记
			 * set_file_area_in_free_list(p_file_area); //到这里，file_area将同时具备in_hot和in_free标记
			 * list_move(&p_file_area->file_area_list,file_area_free_temp);
			 * 于是遍历到free链表上的file_area时，可能也有in_hot属性，此时不能因file_area有in_hot标记而crash。
			 * 做法是清理in_hot标记，设置in_refault标记，因为在内存回收时被访问了
			 * */
			if(file_area_in_hot_list(p_file_area)){
				clear_file_area_in_hot_list(p_file_area);
				set_file_area_in_refault_list(p_file_area);
				printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x find hot in_free_list\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
			}

			/*1：如果在file_stat->free链表上的file_area，被访问在hot_file_update_file_status()被访问而设置了
			  in_refault_list标记，这里遍历到这种file_area就移动到file_stat->refault链表。
			  2：file_area的访问计数大于0也说明file_area在内存回收过程中被访问了，则也要移动到file_stat->refault链表
			  3：如果file_area的page数大于0，说明对file_area的page内存回收失败，还残留page。这种file_area也要移动到
			  file_stat->refault链表，长时间禁止内存回收*/

			/* 新加一个条件，writeonly的文件，即使内存回收后再被访问也不判定refault，因为refault只针对读的文件页。
			 * 其实，还可以扩展。只有file_area_in_read的file_area才判定为refault，这个控制更精细?????????????????
			 * 最新方案，update函数不再标记file_area的in_refault标记，只在该函数标记file_area的in_refault标记。
			 * 但是新的问题又来了，read属性的file_area因长时间没访问，会被清理掉read属性。然后被内存回收后移动到
			 * file_stat->free链表，接着该file_area又被访问。但是此时不是读，而是被写，这种file_area需要被判定为
			 * refault file_area吗？不行的，不能被判定为read file_area，而是需要移动到file_stat->wrtiteonly链表。
			 * 如果是writeonly文件，refault 的file_area依然留存在file_stat->free链表，但是会直接回收writeonly文件
			 * file_stat->free链表上的file_area的page，这点不用担心。但是如果这个writeonly的文件，后来变成了read
			 * 文件，那大量refault的page就永久停留在file_stat->free链表了。不过，此时这些file_area会被判定为真正的
			 * refault file_area，而移动到file_stat->refault链表，似乎也没啥问题。*/
			if(!file_stat_in_writeonly_base(p_file_stat_base) && (/*file_area_in_refault_list(p_file_area)*/
					/*|| unlikely(file_area_access_count_get(p_file_area) > 0 || */
					 file_area_have_page(p_file_area))){
				/*这段代码时新加的，是个隐藏很深的小bug。file_area在内存回收前都要对access_count清0，但是在内存回收最后，可能因对应page
				 *被访问了而access_count加1，然后对age赋值为当时的global age，但是file_area的page内存回收失败了。等了很长时间后，终于再次
				 *扫描到这个文件file_stat，但是file_area的age还是与global age相差很大了，正常就要判定这个file_area长时间没访问而释放掉。
				 *但这是正常现象不合理的！因为这个file_area的page在内存回收时被访问了。于是就通过file_area的access_count大于0而判定这个file_area的
				 *page在内存回收最后被访问了，于是就不能释放掉file_area。那就要移动到file_stat->temp链表或者refault链表!!!!!!!!!!!!!!!!!!!!*/

				/*file_area必须有in_free_list状态，否则crash，防止file_area重复移动到file_stat->refault链表*/
				if(!file_area_in_free_list(p_file_area) || (file_area_in_free_list_error(p_file_area) && !file_area_in_refault_list(p_file_area)))
					panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_free error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

				/* 非writeonly文件，但是发生refault的writeonly属性的file_area，不判定为refault file_area。而是直接移动到file_area_writeonly_or_cold链表
				 * 注意，到这里的文件可能是普通文件或者global_file_stat文件*/
				if(FILE_STAT_NORMAL == file_type && !file_area_in_read(p_file_area)){
				    file_area_access_count_clear(p_file_area);
				    clear_file_area_in_free_list(p_file_area);

					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
					list_num_update(p_file_area,POS_WIITEONLY_OR_COLD);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_writeonly_or_cold);
					break;
				}

				/* 这个if针对cache文件不会成立，但是mmap文件in_free的file_area在发生refault后，不会被标记refault。
				 * 只能在这里遍历in_free标记的file_area时，标记file_area in_refault。并refault_page_count加1。这里
				 * 就有个问题，就是针对mmap refautl page的统计有延迟，最好的解决办法是：在发生refault后，一定会
				 * 执行__filemap_add_folio()把新分配的page添加到有in_free标记的file_area->pages[]数组，此时
				 * 执行if(p_file_stat_base->refault_page_count < USHRT_MAX - 2) p_file_stat_base->refault_page_count++
				 * ，但是不想在__filemap_add_folio()函数，添加太多冗余代码。后续如果遇到mmap file针对refault
				 * 的page处理不好，再考虑这点吧?????????????????????????。但mysql低内存压测压测时发现，
				 * __filemap_add_folio()统计的总refault次数统计file_area_refault_file，增长很大，但是
				 * 统计的每个文件的refault次数p_file_stat_base->refault_page_count却没有明显增加。于是
				 * 怀疑这些都是mmap文件导致的refault次数增加，现在的方案只能修改了，把
				 * p_file_stat_base->refault_page_count ++放到__filemap_add_folio()函数里。*/
				/*最新方案，update函数不再标记file_area的in_refault标记，只在该函数标记file_area的in_refault标记*/
				/*if(!file_area_in_refault_list(p_file_area))*/{
					set_file_area_in_refault_list(p_file_area);
					/*文件发生refault次数加1*/
					/*if(p_file_stat_base->refault_page_count < USHRT_MAX - 2)
						p_file_stat_base->refault_page_count ++;*/
				}

				/*把file_area移动到file_stat->refault链表，必须对访问计数清0。否则后续该file_area不再访问则
				 *访问计数一直大于0，该file_area移动到file_stat->free链表后，因访问计数大于0每次都要移动到file_stat->refault链表*/
				file_area_access_count_clear(p_file_area);
				file_area_free_to_refault_list_count ++;
				//spin_lock(&p_file_stat->file_stat_lock);	    
				clear_file_area_in_free_list(p_file_area);
				/*上边标记refault了，这里不再重复标记*/
				//set_file_area_in_refault_list(p_file_area);

				/*tiny small文件的free、refault、hot属性的file_area都在file_stat的temp链表上，故不用移动file_area。
				 *small的free、refault、hot属性的file_area都在file_stat的other链表上，故也不用移动file_area*/
				if(FILE_STAT_NORMAL == file_type){
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
					/*refault链表上长时间没访问的file_area现在移动到file_stat->warm链表，而不是file_stat->temp链表，这个不用spin_lock(&p_file_stat->file_stat_lock)加锁*/
					if(is_global_file_stat){
						struct global_file_stat *p_global_file_stat = container_of(p_file_stat,struct global_file_stat,file_stat);
						list_move(&p_file_area->file_area_list,&p_global_file_stat->file_area_refault);
					}
					else
						list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);//-----多层warm机制文件的refault的file_area移动到file_area_hot链表
				}
				/*检测到refault的file_area个数加1*/
				p_hot_cold_file_global->check_refault_file_area_count ++;
				//spin_unlock(&p_file_stat->file_stat_lock);	    
				if(file_stat_in_test_base(p_file_stat_base))
					printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x accessed in reclaim\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
			}
			/*如果file_stat->file_area_free链表上的file_area长时间没有被访问则释放掉file_area结构。之前的代码有问题，判定释放file_area的时间是
			 *file_area_free_age_dx，这样有问题，会导致file_area被内存回收后，在下个周期file_area立即被释放掉。原因是file_area_free_age_dx=5，
			 file_area_temp_to_cold_age_dx=5，下个内存回收周期 global_age - file_area_free_age_dx肯定大于5*/

			//else if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > 
			//		(p_hot_cold_file_global->file_area_free_age_dx + p_hot_cold_file_global->file_area_temp_to_cold_age_dx)){
			else if(file_area_age_dx > file_area_free_age_dx + file_area_temp_to_cold_age_dx){
#if 0	
				/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
				if(file_area_in_ahead(p_file_area)){
					clear_file_area_in_ahead(p_file_area);
					/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
					if(IS_MEMORY_ENOUGH(p_hot_cold_file_global) || file_area_age_dx < p_hot_cold_file_global->file_area_reclaim_ahead_age_dx)
						break;
				}
#endif				
				//file_area_free_count ++;
				//p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_free_count_from_free_list += file_area_free_count;
				if(file_stat_in_cache_file_base(p_file_stat_base))
					p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_free_count_from_free_list += 1;
				else
					p_hot_cold_file_global->mmap_file_shrink_counter.file_area_free_count_from_free_list += 1;
				
				if(file_stat_in_test_base(p_file_stat_base))
					printk("%s:file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d global_age:%d delete_file_area\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age,p_hot_cold_file_global->global_age);

				/*hot_file_update_file_status()函数中会并发把file_area从file_stat->file_area_free链表移动到file_stat->file_area_free_temp
				 *链表.这里把file_stat->file_area_free链表上的file_area剔除掉并释放掉，需要spin_lock(&p_file_stat->file_stat_lock)加锁，
				 *这个函数里有加锁*/
				ret = cold_file_area_delete(p_hot_cold_file_global,p_file_stat_base,p_file_area);
				//if(cold_file_area_delete(p_hot_cold_file_global,p_file_stat_base,p_file_area) > 0){
				if(ret > 0){
					/*在释放file_area过程发现file_area分配page了，于是把file_area移动到file_stat->refault链表*/
					file_area_access_count_clear(p_file_area);
					file_area_free_to_refault_list_count ++;
					clear_file_area_in_free_list(p_file_area);
					set_file_area_in_refault_list(p_file_area);

					if(FILE_STAT_NORMAL == file_type){
						p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
						if(is_global_file_stat){
							struct global_file_stat *p_global_file_stat = container_of(p_file_stat,struct global_file_stat,file_stat);
							list_move(&p_file_area->file_area_list,&p_global_file_stat->file_area_refault);
						}
						else
							list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);//多层warm机制 refault的file_area移动到file_area_hot链表
					}
					/*检测到refault的file_area个数加1*/
					p_hot_cold_file_global->check_refault_file_area_count ++;
				}else if(ret < 0){
					goto find_global_file_area_in_mapping;
				}
			}

			break;
		case (1 << F_file_area_in_mapcount_list):
			struct folio *folio;
			int i;
			int print_once = 0;

			/* 遇到一个bug，mmap文件的mapcount的file_area，竟然有in_hot标记，这个也正常。mmap和cache文件处理合二为一后，
			 * 在异步内存回收线程遍历in_temp链表的file_area时，在没有清理掉in_temp标记瞬间，该file_area在hot_file_update
			 * 函数被标记in_hot。后续异步内存回收线程把该file_area有in_temp状态转成in_mapcount、in_free、in_warm后，该
			 * file_area同时还有in_hot标记，一个file_area有两种状态，此时不能crash*/
			if(!file_area_in_mapcount_list(p_file_area) || (file_area_in_mapcount_list_error(p_file_area) && !file_area_in_hot_list(p_file_area)))
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_mapcount\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			//file_area被遍历到时记录当时的global_age，不管此时file_area的page是否被访问pte置位了
			//p_file_area->file_area_access_age = p_hot_cold_file_global->global_age;//--------------------------------------------------------
			if(0 == file_area_have_page(p_file_area))
				return; 

			/*存在一种情况，file_area的page都是非mmap的，普通文件页!!!!!!!!!!!!!!!!!!!*/
			for(i = 0;i < PAGE_COUNT_IN_AREA;i ++){
				folio = p_file_area->pages[i];
				if(!folio || folio_is_file_area_index_or_shadow(folio)){
					if(shrink_page_printk_open1)
						printk("%s file_area:0x%llx status:0x%x folio NULL\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

					if(shrink_page_printk_open_important && folio_is_file_area_index_or_shadow(folio) && print_once){
						print_once = 0;
						printk(KERN_ERR"%s file_area:0x%llx status:0x%x folio_is_file_area_index!!!!!!!!!!!!!\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
					}
					continue;
				}

				//page = &folio->page;
				if(folio_mapcount(folio) > MAPCOUNT_LEVEL)
					break;
			}
			//if成立说明file_area的page的mapcount都是1，file_area不再是mapcount file_area，则降级到temp_list链表头
			if(i == PAGE_COUNT_IN_AREA){
				//file_stat->refault、free、hot、mapcount链表上的file_area移动到file_stat->temp链表时要先对file_area->file_area_access_age清0，原因看定义
				//p_file_area->file_area_access_age = 0;

				//spin_lock(&p_file_stat->file_stat_lock);现在file_area移动到file_stat->warm链表，不用加锁
				clear_file_area_in_mapcount_list(p_file_area);
				/* 注意，到这里file_area可能还有in_hot标记，如果该file_area长时间没访问则清理掉in_hot标记。否则
				 * 下边把该file_area移动到in_temp链表，将来会移动到in_temp链表*/
				if(file_area_in_hot_list(p_file_area)){
					get_file_area_age(p_file_stat_base,p_file_area,/*file_area_age,*/p_hot_cold_file_global,file_stat_changed,file_type,is_global_file_stat);

					if(file_stat_changed)
						panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_stat_changed error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			        if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > file_area_hot_to_temp_age_dx)
			            clear_file_area_in_hot_list(p_file_area);
				}
				if(FILE_STAT_TINY_SMALL == file_type){
					/*如果file_area有page则按照正常流程处理。否则说明没有page了，说明file_area的page大概率被kswapd或者直接内存回收释放掉了，那直接当成in_free的file_area处理*/
					if(file_area_have_page(p_file_area)){
					    set_file_area_in_temp_list(p_file_area);
					    p_file_stat_base->file_area_count_in_temp_list ++;
					}else{
						/*if(!file_area_in_free_kswapd(p_file_area))
				            printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_area_type:0x%x not in free_kswapd\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,file_area_type);*/
                        set_file_area_in_free_list(p_file_area);
					}
				}else if(FILE_STAT_SMALL == file_type){
					/*如果file_area有page则按照正常流程处理。否则说明没有page了，说明file_area的page大概率被kswapd或者直接内存回收释放掉了，那直接当成in_free的file_area处理*/
					if(file_area_have_page(p_file_area)){
						/*small文件是把file_area移动到file_stat->temp链表*/
						spin_lock(&p_file_stat_base->file_stat_lock);
						set_file_area_in_temp_list(p_file_area);
						list_move(&p_file_area->file_area_list,&p_file_stat_base->file_area_temp);
						p_file_stat_base->file_area_count_in_temp_list ++;
						spin_unlock(&p_file_stat_base->file_stat_lock);
					}else{
						/*if(!file_area_in_free_kswapd(p_file_area))
				            printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_area_type:0x%x not in free_kswapd\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,file_area_type);*/
                        set_file_area_in_free_list(p_file_area);
					}
				}
				else if(FILE_STAT_NORMAL == file_type){
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

					/*如果file_area有page则按照正常流程处理。否则说明没有page了，说明file_area的page大概率被kswapd或者直接内存回收释放掉了，那直接当成in_free的file_area处理*/
					if(file_area_have_page(p_file_area)){
						//set_file_area_in_warm_list(p_file_area);
						list_num_update(p_file_area,POS_WARM);
						//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
						list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
						//printk("5 %s:file_stat:0x%llx file_area:0x%llx status:0x%x age:%d mapcount to warm\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
						p_hot_cold_file_global->mmap_file_shrink_counter.mapcount_to_warm_file_area_count += 1;
						//mapcount_to_warm_file_area_count ++;
						//在file_stat->file_area_temp链表的file_area个数加1，这是把file_area移动到warm链表，不能file_area_count_in_temp_list加1
						//p_file_stat->file_area_count_in_temp_list ++;
						//在file_stat->file_area_mapcount链表的file_area个数减1
						p_file_stat->mapcount_file_area_count --;
					}else{
						/*if(!file_area_in_free_kswapd(p_file_area))
				            printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_area_type:0x%x not in free_kswapd\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,file_area_type);*/
					    set_file_area_in_free_list(p_file_area);
						list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free);
					}
				}else
					BUG();

				//spin_unlock(&p_file_stat->file_stat_lock);
				
				if(file_stat_in_test_base(p_file_stat_base))
					printk("%s:file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d global_age:%d mapcount to warm\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age,p_hot_cold_file_global->global_age);

			}
			break;

		default:
			panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x statue error!!!!!!\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
	}
	//}


	//p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_hot_to_warm_list_count += file_area_hot_to_warm_list_count;
	//p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_refault_to_warm_list_count += file_area_refault_to_warm_list_count;
	//释放的file_area结构个数
	//p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_free_count_from_free_list += file_area_free_count;

	//return scan_file_area_count;
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
static noinline unsigned int file_stat_other_list_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *file_area_list_head,unsigned int scan_file_area_max,unsigned int file_area_type_for_bit,unsigned int file_type)
{
	struct file_area *p_file_area,*p_file_area_temp;
	int scan_file_area_count = 0;
	char file_area_type_for_bit_changed = 0;
	//unsigned int file_area_hot_to_warm_list_count = 0;
	//unsigned int file_area_refault_to_warm_list_count = 0;
	//unsigned int file_area_free_to_refault_list_count = 0;
	//unsigned int file_area_free_count = 0;
	char is_normal_file_stat = (get_file_stat_type(p_file_stat_base) == FILE_STAT_NORMAL);

	/*file_stat和file_type不匹配则主动crash*/
	if(file_stat_in_global_base(p_file_stat_base)){
		if(file_stat_in_cache_file_base(p_file_stat_base)){
			if((u64)p_file_stat_base != (u64)(&p_hot_cold_file_global->global_file_stat.file_stat.file_stat_base))
				panic("%s cache global file_stat:0x%llx match 0x%llx error\n",__func__,(u64)p_file_stat_base,(u64)(&p_hot_cold_file_global->global_file_stat.file_stat.file_stat_base));
		}else{
			if((u64)p_file_stat_base != (u64)(&p_hot_cold_file_global->global_mmap_file_stat.file_stat.file_stat_base))
				panic("%s mmap global file_stat:0x%llx match 0x%llx error\n",__func__,(u64)p_file_stat_base,(u64)(&p_hot_cold_file_global->global_mmap_file_stat.file_stat.file_stat_base));
		}
	}else 
		is_file_stat_match_error(p_file_stat_base,file_type);

	/*file_area_list_head来自file_stat->file_area_hot、file_stat->file_area_free,file_stat->file_area_refault链表头*/
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,file_area_list_head,file_area_list){
		if(scan_file_area_count ++ > scan_file_area_max)
			break;
        /*如果file_stat->free链表上遇到同时具备in_free和in_refault的file_area(update函数里遇到in_free的file_area则设置in_refault)，
		 *需要单独处理成in_refault的file_area吗？不用，这些file_stat_other_list_file_area_solve_common()函数已经有处理*/
		/*if(F_file_area_in_free_list == file_area_type_for_bit && file_area_in_refault(p_file_area)){
            clear_file_area_in_free_list(p_file_area);
			list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
		}*/

		/*if(file_stat_in_from_cache_file_base(p_file_stat_base) && (file_area_type_for_bit == F_file_area_in_mapcount_list)){
			printk("%s file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x file_area_type_for_bit:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,file_area_type_for_bit);
			dump_stack();
		}*/

		/* normal file_stat文件，为了节省内存，hot和mapcount都保存在file_stat->hot链表，遇到该链表上mapcount file_ara，强制赋值file_area_type_for_bit为mapounct
		 * 注意，不能是global_file_stat，它有专门的file_area_mapcount链表*/
		if(is_normal_file_stat &&  (F_file_area_in_hot_list ==  file_area_type_for_bit)){
			if(file_area_in_mapcount_list(p_file_area)){
				file_area_type_for_bit = F_file_area_in_mapcount_list;
				file_area_type_for_bit_changed = 1;
			}else if(file_area_in_refault_list(p_file_area)){
				file_area_type_for_bit = F_file_area_in_refault_list;
				file_area_type_for_bit_changed = 1;
			}
		}
        file_stat_other_list_file_area_solve_common(p_hot_cold_file_global,p_file_stat_base,p_file_area,1 << file_area_type_for_bit,file_type);

		/* 如果file_area_type_for_bit_changed是1，说明是get_file_area_from_file_stat_list函数里，遍历到normal file_stat->file_area_hot链表上的mapcount
		 * file_area，此时当前函数传入的file_area_type_for_bit是F_file_area_in_hot_list。上边把file_area_type_for_bit设置为file_area_type_for_bit，用过后，
		 * 这里要把file_area_type_for_bit恢复为原来的F_file_area_in_hot_list。否则file_area_type_for_bit就会一直保持F_file_area_in_mapcount_list，直到
		 * 这个循环结束。如果这个过程碰到是in_hot的file_area，但是file_area_type_for_bit是F_file_area_in_mapcount_list，就会导致
		 * file_stat_other_list_file_area_solve_common函数里，报告"Kernel panic - not syncing:..not in file_area_mapcount" 而crash。这个crash也挺SB的，
		 * 没有通盘考虑真个循环过程，修改file_area_type_for_bit后，会导致什么后果*/
		if(file_area_type_for_bit_changed){
			file_area_type_for_bit_changed = 0;
			file_area_type_for_bit = F_file_area_in_hot_list;
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
		if(can_file_area_move_to_list_head(p_file_area,file_area_list_head,file_area_type_for_bit))
			list_move_enhance(file_area_list_head,&p_file_area->file_area_list);
	}

	//p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_refault_to_warm_list_count += file_area_refault_to_warm_list_count;
	//p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_hot_to_warm_list_count += file_area_hot_to_warm_list_count;
	//释放的file_area结构个数
	//p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_free_count_from_free_list += file_area_free_count;

	return scan_file_area_count;
}
/*遍历file_stat_small->otehr链表上的hot、refault、free链表上的file_area*/
static noinline unsigned int file_stat_small_other_list_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *file_area_list_head,unsigned int scan_file_area_max,unsigned int file_area_type_for_bit,unsigned int file_type)
{
	struct file_area *p_file_area,*p_file_area_temp;
	int scan_file_area_count = 0;
	unsigned int file_area_type;
	//unsigned int file_area_hot_to_warm_list_count = 0;
	//unsigned int file_area_refault_to_warm_list_count = 0;
	//unsigned int file_area_free_to_refault_list_count = 0;
	//unsigned int file_area_free_count = 0;

	/*file_stat和file_type不匹配则主动crash。global_file_stat不做这个判断，或者再做个其他判断???????*/
	is_file_stat_match_error(p_file_stat_base,file_type);

	/*file_area_list_head来自file_stat->file_area_hot、file_stat->file_area_free,file_stat->file_area_refault链表头*/
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,file_area_list_head,file_area_list){
		if(scan_file_area_count ++ > scan_file_area_max)
			break;
		file_area_type = get_file_area_list_status(p_file_area);
		/*如果small_file_stat->other链表上的in_free的file_area因为访问在update函数又被设置in_refault属性，
		 *则强制只给file_area_type赋值1 << F_file_area_in_free_list，这样file_stat_other_list_file_area_solve_common()
		 *函数里走in_free分支，发现file_area有in_refault属性而被判定为refault area。
		 *否则file_stat_other_list_file_area_solve_common()
		 *函数会因file_area_type同时具备in_refault和in_hot属性而主动crash。*/
		if(file_area_type == (1 << F_file_area_in_free_list | 1 << F_file_area_in_refault_list))
			file_area_type = 1 << F_file_area_in_free_list;

		/*存在file_area既有mapcount和hot标记的情况*/
		if(file_area_type == (1 << F_file_area_in_mapcount_list | 1 << F_file_area_in_hot_list))
			file_area_type = 1 << F_file_area_in_mapcount_list;

		file_stat_other_list_file_area_solve_common(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_area_type,file_type);
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

		/*对于file_stat_small->otehr链表上的hot、refault、free链表上的file_area，只用异步内存回收线程才会移动它，
		 *此时这些file_area这里肯定不会移动到其他file_stat链表，因此不用再做can_file_area_move_to_list_head判断。
		 *并且这个链表上的file_area有各种属性，没办法用can_file_area_move_to_list_head()判断这个file_area的属性
		 *是否变了，但是要判断p_file_area此时是不是指向链表头!!!!!!!!!!!!!，
		 *不过list_move_enhance()已经有判断了!!!!!!!!!!!!
		 *
         *有大问题，如果file_stat_small->otehr链表上hot、refault的file_area因长时间不访问，移动到了file_stat_small->temp链表，
		 *此时list_move_enhance()就是把file_stat_small->temp链表的file_area移动到file_stat_small->otehr链表。按照之前的经验，
		 *会破坏file_stat_small->otehr链表头，甚至出现file_stat_small->otehr链表头和file_stat_small->temp链表头相互指向
         */

		/* small_file_stat->other链表上只有hot、free、refault属性的file_area才能移动到small_file_stat->other链表头。
		 * 由于file_stat_small->other链表上有hot、free、refault等多种属性的file_area，因此这里不能设置要判定p_file_area
		 * 有这些属性时，都要把本次遍历过的file_area移动到链表头*/
		file_area_type = (1 << F_file_area_in_hot_list) | (1 << F_file_area_in_free_list) | (1 << F_file_area_in_refault_list);
		/*现在cache file和mmap file同时处理，要加上mapcount的file_area*/
		file_area_type |= (1 << F_file_area_in_mapcount_list);
		if(can_file_area_move_to_list_head_for_small_file_other(p_file_area,file_area_list_head,file_area_type))
			list_move_enhance(file_area_list_head,&p_file_area->file_area_list);
	}

	//p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_refault_to_warm_list_count += file_area_refault_to_warm_list_count;
	//p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_hot_to_warm_list_count += file_area_hot_to_warm_list_count;
	//释放的file_area结构个数
	//p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_free_count_from_free_list += file_area_free_count;

	return scan_file_area_count;
}
/*对于有in_kswapd标记的file_area，现在处理逻辑变了，不再单独处理。而是，如果没有page了则直接移动到in_free链表，否则当成普通的file_area处理*/
static int kswapd_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,unsigned int file_type)
{
	/* 如果file_area没有page，说明file_area被内存回收后没有page再被访问，没有发生refault，
	 * 此时按照file_stat->temp的file_area正常处理流程处理，移动到file_stat->free链表，然后被释放掉。
	 * 改了，现在改为，果没有page了则直接移动到in_free链表，否则当成普通的file_area处理*/
	/*if(file_area_have_page(p_file_area)){
		p_hot_cold_file_global->check_refault_file_area_kswapd_count ++;
		return 0;
	}*/

	if(file_stat_in_test_base(p_file_stat_base))
		printk("%s:file_stat:0x%llx file_stat_status:0x%x file_area:0x%llx status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state);

	if(file_area_in_temp_list(p_file_area)){
		clear_file_area_in_temp_list(p_file_area);
		/*此时，file_area已有了in_free标记，也有in_kswapd标记了!!!!!!!!!!!!*/
		set_file_area_in_free_list(p_file_area);

		if(FILE_STAT_NORMAL == file_type){
			struct file_stat *p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
			spin_lock(&p_file_stat_base->file_stat_lock);
			list_move_tail(&p_file_area->file_area_list,&p_file_stat->file_area_free);
			spin_unlock(&p_file_stat_base->file_stat_lock);
		}else if(FILE_STAT_SMALL == file_type){
			struct file_stat_small *p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
			spin_lock(&p_file_stat_base->file_stat_lock);
			list_move_tail(&p_file_area->file_area_list,&p_file_stat_small->file_area_other);
			spin_unlock(&p_file_stat_base->file_stat_lock);
		}else if(FILE_STAT_TINY_SMALL == file_type){

		}else
			BUG();

	}
#if 0	
	else if(file_area_in_warm_list(p_file_area)){
		struct file_stat *p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
	    if(FILE_STAT_NORMAL != file_type)
			panic("%s: file_stat:0x%llx file_area:0x%llx status:0x%x file_type:%d not normal\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,file_type);

		clear_file_area_in_warm_list(p_file_area);
		set_file_area_in_free_list(p_file_area);
	    list_move_tail(&p_file_area->file_area_list,&p_file_stat->file_area_free);
	}else
		panic("%s: file_stat:0x%llx file_area:0x%llx status:0x%x not in temp or warm error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
#endif

#if 0
	clear_file_area_in_free_kswapd(p_file_area);
	set_file_area_in_refault_list(p_file_area);

	/* 如果file_area长时间没有被访问，是不是不要再移动到file_stat->refault链表了，而是直接移动到file_stat->free链表，
	 * 或者干脆这里直接把file_area释放了。这个处理跟file_stat_other_list_file_area_solve_common()函数功能重叠了，
	 * 直接把file_area移动到file_stat->refault后，自动在file_stat_other_list_file_area_solve_common函数里处理*/
	if(FILE_STAT_NORMAL == file_type){
		struct file_stat *p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
		/*把file_area移动到file_stat->refault链表尾，这样如果长时间没访问，很快就能被异步内存回收线程遍历到，
		 * 然后移动到file_stat->free链表，最后尽快释放掉*/
		if(file_reea_in_temp)
			spin_lock(p_file_area->spin);
		list_move_tail(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
	}else if(FILE_STAT_SMALL == file_type){
		struct file_stat_small *p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
		list_move_tail(&p_file_area->file_area_list,&p_file_stat_small->file_area_other);
	}else if(FILE_STAT_TINY_SMALL == file_type){
		/*tiny small文件的所有file_area都在file_stat->temp链表上，因此不用list_move该file_area*/
	}else
		BUG();
#endif
	return 1;
}
/*如果扫描scan_file_area_count个file_area中，有一半不是cold的，则return 1，说明内存回收效率很低*/
static inline int check_memory_reclaim_efficiency(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_count,unsigned int scan_cold_file_area_count,unsigned int file_type)
{
	/*如果是小文件，或者 不是内存紧张模式，直接return 0，不理会内存回收效率*/
	if(FILE_STAT_TINY_SMALL == file_type || !IS_MEMORY_ENOUGH(p_hot_cold_file_global))
		return 0;

	switch(scan_file_area_count){
		case 32:
			if(scan_cold_file_area_count < 16)
				return 1;
			break;
		case 64:
			if(scan_cold_file_area_count < 32)
				return 1;
			break;
		case 128:
			if(scan_cold_file_area_count < 64)
				return 1;
			break;
		default:
	}
	return 0;
}
#if 0
/*遍历file_stat->temp链表上的file_area*/
static inline int file_stat_temp_list_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int scan_file_area_max,struct list_head *file_area_free_temp,unsigned int file_type,unsigned int age_dx_change_type)
{
	struct file_area *p_file_area = NULL,*p_file_area_temp;
	unsigned int scan_file_area_count = 0;
	unsigned int file_area_age_dx;
	unsigned int temp_to_warm_file_area_count = 0,scan_cold_file_area_count = 0;
	unsigned int scan_read_file_area_count = 0;
	unsigned int scan_ahead_file_area_count = 0;
	unsigned int temp_to_hot_file_area_count = 0;
	//unsigned int scan_hot_file_area_count;
	struct file_stat *p_file_stat = NULL;
	struct file_stat_small *p_file_stat_small = NULL;
	//unsigned int file_type;
	unsigned int file_area_type;
	//unsigned int file_area_age;
	unsigned int file_area_temp_to_warm_age_dx,file_area_temp_to_cold_age_dx;
	char file_stat_changed;
	char recliam_pages_from_from_writeonly_file = (AGE_DX_CHANGE_WRITEONLY_IN_EMERGENCY_RECLAIM == age_dx_change_type);
	/* 连续扫描到的非冷file_area个数，扫描到冷file_area则清0。用于只读文件的内存回收，如果连续扫描到多个非冷的file_area，
	 * 说明file_area_temp_to_cold_age_dx有点大，需要调小。如果这样遇到dec_temp_to_cold_age_dx_count次数。如果调小后，那说
	 * 明当前只读文件的大片file_area都刚访问过，那就结束遍历该文件的file_area了*/
	unsigned int scan_serial_warm_file_area_count = 0;
	char dec_temp_to_cold_age_dx_count = 16;
	char memory_reclaim_efficiency;
	char is_global_file_stat = file_stat_in_global_base(p_file_stat_base);
	//unsigned int scan_file_area_zero_page_count = 0;

	/*现在针对mmap的file_area_temp_to_cold_age_dx，在内存回收最开头执行的change_global_age_dx_for_mmap_file()函数里调整了，这里不再重复*/
	if(recliam_pages_from_from_writeonly_file){
		file_area_temp_to_cold_age_dx = p_hot_cold_file_global->writeonly_file_age_dx;
		file_area_temp_to_warm_age_dx = 0;
	}
	else{
	    file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
		file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_warm_age_dx;
	}
	/*if(file_stat_in_cache_file_base(p_file_stat_base)){
	  file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_warm_age_dx;
	  file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
	  }else{
	  file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_warm_age_dx + MMAP_FILE_TEMP_TO_WARM_AGE_DX;
	  file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx + MMAP_FILE_TEMP_TO_COLD_AGE_DX;
	  }*/
	//从链表尾开始遍历，链表尾的成员更老，链表头的成员是最新添加的
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_base->file_area_temp,file_area_list){

		/* 新的版本hot_file_update_file_status()遇到refault、hot file_area，只是做个标记，而不会把file_area移动到
		 * file_stat->refault、hot链表，因此file_stat->temp链表上的file_area除了有in_temp_list标记，还有
		 * in_refault_list、in_hot_list标记，故要把file_area_in_temp_list_error(p_file_area)判断去掉*/
#if 0
		if(!file_area_in_temp_list(p_file_area)  || file_area_in_temp_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
#endif

		/* scan_file_area个数超过max则break结束遍历，不能放到这里。因为此时的file_area还没有参与内存回收
		 * ，就在该函数被迫移动到file_stat->temp链表头，浪费一次遍历的机会*/
		/*if(++scan_file_area_count > scan_file_area_max)
		  break;*/
		scan_file_area_count ++;

		/*file_stat->temp链表上的file_area不可能有in_refault_list标记，file_stat->refault和free链表上的file_area才会有in_refault_list标记*/

		/*一个file_area可能在hot_file_update_file_status()中被并发设置in_temp_list、in_hot_list、in_refault_list 
		 * 这3种属性，因此要if(file_area_in_refault_list(p_file_area))也需要判断清理in_hot_list属性。in_hot_list同理*/

		/*if(file_area_in_refault_list(p_file_area)){
		  spin_lock(&p_file_stat->file_stat_lock);
		  if(file_area_in_temp_list(p_file_area))
		  clear_file_area_in_temp_list(p_file_area);
		  if(file_area_in_hot_list(p_file_area))
		  clear_file_area_in_hot_list(p_file_area);
		  list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
		  spin_unlock(&p_file_stat->file_stat_lock);
		  continue;
		  }
		  else*/

		/* 新版本update函数不再设置hot file_area，而是遍历到该file_area时发现访问频次大于阈值再判断hot file_area
		 * file_area不能处于in_refault状态，否则file_area同时具备in_refault和in_hot状态而crash。竟然又遇到in_free链表
		 * 的file_area这里因为access_count大又被赋值了in_hot属性，而导致crash。file_area还有可能mapcount呀，干脆限制in_temp*/
		if(file_area_access_freq(p_file_area) > 2 && file_area_in_temp_list(p_file_area)/*!file_area_in_refault_list(p_file_area) && !file_area_in_free_list(p_file_area)*/){
			/*之前的方案hot file_area不清理temp属性，现在也不清理*/
			//clear_file_area_in_temp_list(p_file_area);
			set_file_area_in_hot_list(p_file_area);
			file_area_access_freq_clear(p_file_area);
		}

		if(file_stat_in_test_base(p_file_stat_base))
			printk("%s:1 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d global_age:%d scan_file_area_max:%d refault_page_count:%d file_area_temp_to_cold_age_dx:%d memory_pressure_level:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age,p_hot_cold_file_global->global_age,scan_file_area_max,p_file_stat_base->refault_page_count,p_hot_cold_file_global->file_area_temp_to_cold_age_dx,p_hot_cold_file_global->memory_pressure_level);

		/*tiny small文件的所有类型的file_area都在file_stat->temp链表，因此要单独处理hot、refault、free属性的file_area。
		 *重点注意，一个有in_temp属性的file_area，还同时可能有in_refault、in_hot属性，这是现在的规则，在update函数中设置.
		 *错了，一个file_area不可能同时有in_temp和in_refault属性，只会同时有in_free和in_refault属性*/
		if(FILE_STAT_TINY_SMALL == file_type && 
				(!file_area_in_temp_list(p_file_area) || file_area_in_hot_list(p_file_area))){
			/*tiny small所有种类的file_area都在file_stat->temp链表，遇到hot和refault的file_area，只需要检测它是否长时间没访问，
			 *然后降级到temp的file_area。遇到free的file_area如果长时间没访问则要释放掉file_area结构。get_file_area_list_status()
			 是获取该file_area的状态，是free还是hot还是refault状态*/

			/*if(file_area_in_hot_list(p_file_area))
			  file_area_type = F_file_area_in_hot_list;
			  else if(file_area_in_free_list(p_file_area))
			  file_area_type = F_file_area_in_free_list;
			  else if(file_area_in_refault_list(p_file_area))
			  file_area_type = F_file_area_in_refault_list;*/

			if(open_file_area_printk)
				printk("%s:1 file_stat:0x%llx file_area:0x%llx status:0x%x tiny small file\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			/*对于同时有in_temp和in_hot属性的file_area，要清理掉in_temp属性，否则file_stat_other_list_file_area_solve_common()
			 *会因同时具备两种属性而crash*/
			if(file_area_in_temp_list(p_file_area) && file_area_in_hot_list(p_file_area)){
				/*只有file_area个数小于64时才清理in_temp属性，原因见file_stat_temp_list_file_area_solve()函数开头的注释。
				 *有个问题，如果执行这行代码时，该文件执行file_area_alloc_and_init()分配大量file_area导致
				 *p_file_stat_base->file_area_count 大于64，
				 *但是p_file_stat_base->file_area_count最新的值还没同步过来，当前cpu的还小于64，这样会不会有问题？
				 *有很大问题，因为此时会清理掉该file_area的in_temp属性，但实际file_area个数超过64，将来这个
				 *热file_area因此还是热的，在退出该函数前，会被移动到tiny_small_file->temp链表头。继续，它
				 *转换成small_file后，这个热file_area将残留在small_file->temp链表，原因见
				 *file_stat_temp_list_file_area_solve()函数开头的注释。将来遍历这个small_file->temp链表上的热
				 *file_area，但没有in_temp属性，就会因状态错误而crash。怎么解决？
				 *这个问题隐藏的很深，单靠想象根本推理不出来，而一步步写出来就发现隐藏不管了!!!!!!!!!!!!!!!!!!!!!
				 *这里用spin_lock(&p_file_stat_base->file_stat_lock)加锁，跟file_area_alloc_and_init()里的
				 spin_lock(&p_file_stat_base->file_stat_lock)加锁并发。然后这里因为是加锁后再判断
				 p_file_stat_base->file_area_count是否小于64，此时肯定没有并发问题，因为这里获取到的是 
				 p_file_stat_base->file_area_count最新的值。但是加锁才浪费性能。算了，最后决定，tiny_small_file->temp
				 链表上的file_area的in_temp和in_hot属性共存，不再清理。

				 似乎这样可以解决问题，但是脑袋突然来了一个想法，tiny_small_file->temp链表上还有一种特殊的file_area，就是
				 同时有in_free和in_refault属性的。下边if(file_area_in_free_list(p_file_area) && file_area_in_refault_list(p_file_area))
				 执行时，如果该文件分配了很多的新的file_area添加到了tiny_small_file->temp链表头。然后执行
				 clear_file_area_in_free_list(p_file_area)清理file_area的in_free属性。然后当前函数执行最后，要把这些in_refault
				 的filea_area移动到tiny_small_file->temp链表头，不在链表尾的64个file_area中。于是该文件转成small file时，
				 这个in_refault的file_area无法移动到small_stat->other链表，而是残留在small_stat->temp链表，这样将来遍历到
				 small_stat->temp链表上的in_refault的file_area时，还会crash.

				 想来想去，终于想到一个完美的办法：这里遍历到in_temp与in_refault 的file_area 和 in_refault与in_free 的file_area时，
				 如果在退出该函数前，spin_lock(&p_file_stat_base->file_stat_lock)加锁，然后判断file_area个数超过64，不把这些特殊
				 的file_area移动到tiny_small_file->temp链表头就行了，停留在链表尾的64个file_area，完美。并且该函数最后原本移动file_area到链表头本身就有加锁。
				 */

				/*if(p_file_stat_base->file_area_count <= SMALL_FILE_AREA_COUNT_LEVEL)*/
				clear_file_area_in_temp_list(p_file_area);
				p_file_stat_base->file_area_count_in_temp_list --;
			}

			file_area_type = get_file_area_list_status(p_file_area);

			/*对于同时有in_free和in_refault属性的file_area(in_free的file_area被访问而在update函数又设置了in_refault)，
			 *要清理掉in_free属性，否则file_stat_other_list_file_area_solve_common()会因同时具备两种属性而crash。现在
			 *修改了，强制赋值1 << file_area_type in_free，没有了in_free，这是为了让该file_area走common函数的in_free分支处理，严谨*/

			//if(file_area_type == (1 << F_file_area_in_free_list | 1 << F_file_area_in_refault_list)){
			if(file_area_in_free_list(p_file_area) && file_area_in_refault_list(p_file_area)){
				/*file_stat_other_list_file_area_solve_common()函数里走in_free分支，发现file_area有in_free属性，会主动清理in_free属性，这里就不清理了*/
				//clear_file_area_in_free_list(p_file_area);
				file_area_type = 1 << F_file_area_in_free_list;
			}

			/*这里遇到一个bug，tiny small文件mapcount的file_area，竟然有in_hot标记，这个是正常现象，这里强制去掉in_hot标记*/
			if(file_area_in_mapcount_list(p_file_area) && file_area_in_hot_list(p_file_area))
				file_area_type = 1 << F_file_area_in_mapcount_list;

			file_stat_other_list_file_area_solve_common(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_area_type,FILE_STAT_TINY_SMALL);
			continue;
		}else{

			if(file_area_in_hot_list(p_file_area)){
				/* 当file_area被判定为hot后，没有清理in_temp_list状态，因此第一次来这里，没有in_temp_list则crash，
				 * 防止重复把file_area移动到file_stat->hot链表.注意，该file_area可能在update函数被并发设置了in_hot标记*/
				if(!file_area_in_temp_list(p_file_area) || (file_area_in_temp_list_error(p_file_area) && !file_area_in_hot_list(p_file_area)))
					panic("%s:1 file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_temp error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

				spin_lock(&p_file_stat_base->file_stat_lock);
				clear_file_area_in_temp_list(p_file_area);
				//if(file_area_in_refault_list(p_file_area))
				//	clear_file_area_in_refault_list(p_file_area);
				/*热file_area肯定来自file_stat->temp链表，因此file_area移动到file_area->hot链表后要减1*/
				if(FILE_STAT_NORMAL == file_type){
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
					p_file_stat_base->file_area_count_in_temp_list --;
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
					p_file_stat->file_area_hot_count ++;
				}else if(FILE_STAT_SMALL == file_type){
					p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
					p_file_stat_base->file_area_count_in_temp_list --;
					list_move(&p_file_area->file_area_list,&p_file_stat_small->file_area_other);
				}else
					BUG();

				spin_unlock(&p_file_stat_base->file_stat_lock);
				temp_to_hot_file_area_count ++;
				continue;
			}
		}
		if(!file_area_in_temp_list(p_file_area) || file_area_in_temp_list_error(p_file_area))
			panic("%s:2 file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_temp error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

		/* 有些page是被kswapd进程内存回收的，这些page的file_area就不会被标记in_free。之后这些file_area的page
		 * 再被访问，就不会被标记in_fault。为了解决这个漏洞，在kswapd释放page执行到
		 * page_cache_delete_for_file_area()函数时，这些file_area没有in_free标记，被异步内存回收释放的page
		 * 有in_free标记。此时就标记file_area的in_free_kswaped。此时的file_area可能在
		 * file_stat->temp、hot、refault、free、other、warm链表，在遍历这些链表的file_area时，是否都要处理呢？
		 * 太麻烦！最终决定只在这些file_area被异步内存回收线程内存回收时，再处理。而只有file_stat->temp、warm
		 * 链表上的file_area才会参与内存回收，则在遍历file_stat->temp、warm链表上的file_area时，如果file_area
		 * 有in_free_kswaped标记，如果这些file_area有page，说明file_area的page在内存回收后，再次被访问，
		 * 则标记file_area in_refault，同时清理掉in_free_kswaped标记，然后把file_area移动到file_stat->refault或other链表。
		 * 具体还要看具体的文件file_stat类型，再做详细的处理。
		 *
		 * 漏了一种情况，就是small文件的other链表，该链表上有in_free_kswapd标记的的file_area，就始终无法遍历到了。
		 * 于是这种file_area要单独处理。还有如果这种file_area再file_stat->hot、free等链表，遍历到真的不处理吗？
		 * 最后，决定in_kswapd标记的file_area，不再单独处理。而是，如果没有page了则直接移动到in_free链表，
		 * 否则当成普通的file_area处理。并且，在file_stat->mapcount、hot、refault上的file_area，如果有
		 * in_kswapd标记，或者file_area没有page了，直接移动到file_stat->free链表，不再按照老的逻辑处理
		 */ 
		/* 如果被判定为refault file_area，直接continue，不再按照in_temp属性的file_area处理
		 *
		 * 最新改动，file_area_in_free_kswapd()不再使用了，没什么用，鸡肋，舍弃掉。因为这种file_area
		 * 可以用if(!file_area_have_page(p_file_area))代替，还占据了file_area->file_area_state一个宝贵的bit位
		 * */
		//if(file_area_in_free_kswapd(p_file_area))
		if(!file_area_have_page(p_file_area))
			if(kswapd_file_area_solve(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_type)){
				//scan_file_area_zero_page_count ++;
				goto next_file_area;
				//continue;
			}

		//get_file_area_age()函数里，会把file_area转换成hot、mapcount file_area需要特别注意!!!!!!!!!!!!
		get_file_area_age(p_file_stat_base,p_file_area,/*file_area_age,*/p_hot_cold_file_global,file_stat_changed,file_type,is_global_file_stat);
		if(file_stat_changed)
			goto next_file_area;
			//continue;

		file_area_age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;
		//file_area_age_dx = p_hot_cold_file_global->global_age - file_area_age;

		//if(file_area_age_dx > p_hot_cold_file_global->file_area_temp_to_warm_age_dx){
		if(file_area_age_dx > file_area_temp_to_warm_age_dx){
			/*file_area经过FILE_AREA_TEMP_TO_COLD_AGE_DX个周期还没有被访问，则被判定是冷file_area，然后就释放该file_area的page*/
			//if(file_area_age_dx > p_hot_cold_file_global->file_area_temp_to_cold_age_dx){
			if(file_area_age_dx > file_area_temp_to_cold_age_dx){

				/* 连续非冷file_area个数加1，针对非writeonly文件，即便是冷file_area这里也加1。保证该file_area是read page
				 * 而没有回收成功，也加1。加快scan_serial_warm_file_area_count的增大。如果回收成功则会清0*/
				if(!recliam_pages_from_from_writeonly_file){
					scan_serial_warm_file_area_count ++;
				}
				if(file_stat_in_test_base(p_file_stat_base))
					printk("%s:2_1 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d temp -> free try\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
#if 0
				/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
				if(file_area_in_ahead(p_file_area)){
					clear_file_area_in_ahead(p_file_area);
					scan_ahead_file_area_count ++;
					/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
					if(IS_MEMORY_ENOUGH(p_hot_cold_file_global) || file_area_age_dx < p_hot_cold_file_global->file_area_reclaim_ahead_age_dx)
						goto next_file_area;
				}
#endif				
				/* 当前内存不紧张且内存回收不困难，则遇到read文件页的file_area先跳过，不回收。目的是尽可能回收write文件页，
				 * read文件页一旦回收再被访问就会发生refault，不仅导致read耗时长，读磁盘功耗还高。但是如果这个read属性的
				 * file_area很长很长很长时间没访问，也参与内存回收*/
				if(file_area_page_is_read(p_file_area)){
					scan_read_file_area_count ++;
					/* 原本如果内存紧张，不再执行continue，然后回收该read文件页，但是这样容易refault。现在改为只有很长时间没访问，才会回收该read文件页
					 * 不能用continue，这样会导致无法执行该循环最后的代码：因为扫描过多热file_area而break跳出循环，因为scan_file_ara超过max而break跳出*/
					//if(!(IS_IN_MEMORY_EMERGENCY_RECLAIM(p_hot_cold_file_global) || file_area_age_dx > p_hot_cold_file_global->file_area_reclaim_read_age_dx))
					if(file_area_age_dx < p_hot_cold_file_global->file_area_reclaim_read_age_dx)
						goto next_file_area;
					    //container;
					clear_file_area_page_read(p_file_area);
				}

				//每遍历到一个就加一次锁，浪费性能，可以先移动到一个临时链表上，循环结束后加一次锁，然后把这些file_area或file_stat移动到目标链表???????
				spin_lock(&p_file_stat_base->file_stat_lock);
				/*为什么file_stat_lock加锁后要再判断一次file_area是不是被访问了,因为可能有这种情况:上边的if成立,此时file_area还没被访问。但是此时有进程
				  先执行hot_file_update_file_status()获取file_stat_lock锁,然后访问当前file_area,file_area不再冷了,当前进程此时获取file_stat_lock锁失败,
				  等获取file_stat_lock锁成功后，file_area的file_area_age就和global_age相等了。变量加减后的判断，在spin_lock前后各判断一次有必要的!!!!!*/
				//if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <  p_hot_cold_file_global->file_area_temp_to_cold_age_dx){
				if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <  file_area_temp_to_cold_age_dx){
					spin_unlock(&p_file_stat_base->file_stat_lock);
					goto next_file_area;
				}
				//access_count清0，如果内存回收期间又被访问了，access_count将大于0，将被判断为refault page。
				file_area_access_count_clear(p_file_area);
				clear_file_area_in_temp_list(p_file_area);
				p_file_stat_base->file_area_count_in_temp_list --;
				/*设置file_area处于file_stat的free_temp_list链表。这里设定，不管file_area处于file_stat->file_area_free_temp还是
				 *file_stat->file_area_free链表，都是file_area_in_free_list状态，没有必要再区分二者。主要设置file_area的状态需要
				 遍历每个file_area并file_stat_lock加锁，再多设置一次set_file_area_in_free_temp_list状态浪费性能。这点需注意!!!!!!!!!!!!!*/
				set_file_area_in_free_list(p_file_area);
				/*需要加锁，此时可能有进程执行hot_file_update_file_status()并发向该p_file_area前或者后插入新的file_area，这里是把该
				 * p_file_area从file_area_temp链表剔除，存在同时修改该p_file_area在file_area_temp链表前的file_area结构的next指针和在链表后的
				 * file_area结构的prev指针，并发修改同一个变量就需要加锁*/
				//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free_temp);
				list_move(&p_file_area->file_area_list,file_area_free_temp);
				spin_unlock(&p_file_stat_base->file_stat_lock);

				scan_cold_file_area_count ++;
				/*遇到冷file_area就清0。再加一个限制条件，必须成功回收该file_area的page，因为该file_area可能因为是read page而无法回收文件页*/
				if(scan_serial_warm_file_area_count)
					scan_serial_warm_file_area_count = 0;

				if(file_stat_in_test_base(p_file_stat_base))
					printk("%s:2_2 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d temp -> free done\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
			}else{
				/*温file_area移动到p_file_stat->file_area_warm链表。从file_stat->temp移动file_area到file_stat->warm链表，必须要加锁*/
				if(FILE_STAT_NORMAL == file_type){
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

					spin_lock(&p_file_stat_base->file_stat_lock);
					clear_file_area_in_temp_list(p_file_area);
					p_file_stat_base->file_area_count_in_temp_list --;
					//set_file_area_in_warm_list(p_file_area);
					list_num_update(p_file_area,POS_WARM);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
					spin_unlock(&p_file_stat_base->file_stat_lock);
					temp_to_warm_file_area_count ++;

					//printk("1 %s:file_stat:0x%llx file_area:0x%llx status:0x%x age:%d temp to warm\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
					if(file_stat_in_test_base(p_file_stat_base))
						printk("%s:3 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d temp -> warm\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
				}

				/*连续非冷file_area个数加1*/
				scan_serial_warm_file_area_count ++;
			}
		}

next_file_area:
	
		/*如果扫描到太多的file_area没有page，则结束遍历。不用了，改为使用check_memory_reclaim_efficiency()*/
		/*if(scan_file_area_zero_page_count > scan_file_area_max >> 4){
			break;
		}*/

		memory_reclaim_efficiency = check_memory_reclaim_efficiency(p_hot_cold_file_global,scan_file_area_count,scan_cold_file_area_count,file_type);

		/*如果是在内存紧缺模式，并且是在回收writeonly文件，那扫描的file_area个数不受scan_file_area_max限制*/
		if(recliam_pages_from_from_writeonly_file){
			/*连续扫描到多个file_area都是非冷的，或者内存回收效率太低*/
			if(scan_serial_warm_file_area_count >= SCAN_SERIAL_WARM_FILE_AREA_LEVEL || memory_reclaim_efficiency){
				scan_serial_warm_file_area_count = 0;

				/*调小file_area_temp_to_cold_age_dx以更容易回收到file_area*/
				if(file_area_temp_to_cold_age_dx > 0)
					file_area_temp_to_cold_age_dx -= 1;

				/*连续扫描到多个file_area都是非冷的，出现次数太多，说明该文件大部分file_area最近刚访问过，结束扫描该文件*/
				dec_temp_to_cold_age_dx_count --;
				if(0 == dec_temp_to_cold_age_dx_count){
					printk("1:%s:writeonly file_stat:0x%llx status:0x%x scan_file_area_count:%d scan_file_area_max:%d  dec_temp_to_cold_age_dx_count <= 0 break file_type:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_file_area_max,file_type);
					break;
				}
			}
		}else{

			/*现在每个文件的scan_file_area_max不受全局scan_file_area_max影响了，而是每个内存内存回收前，执行check_file_area_refault_and_scan_max()确定*/
			if(scan_file_area_count > scan_file_area_max){
				printk("2:%s:file_stat:0x%llx status:0x%x scan_file_area_count:%d > scan_file_area_max:%d break file_type:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_file_area_max,file_type);
				break;
			}

			/*如果扫描到的file_area有太多不是冷file_area，结束遍历*/
			/*if(scan_cold_file_area_count < scan_file_area_count >> 1){
				printk("%s:file_stat:0x%llx status:0x%x scan_file_area_count:%d scan_cold_file_area_count:%d too less  break\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_cold_file_area_count);
				break;
			}*/

			/*对于非writeonly的文件，如果遇到多次一片的file_area都是热的，则提前结束扫描该文件*/
			if(scan_serial_warm_file_area_count >= SCAN_SERIAL_WARM_FILE_AREA_LEVEL_FOR_READ_FILE || memory_reclaim_efficiency){
				scan_serial_warm_file_area_count = 0;
				dec_temp_to_cold_age_dx_count -= 5;
				if(dec_temp_to_cold_age_dx_count <= 0){
					printk("3:%s:file_stat:0x%llx status:0x%x scan_file_area_count:%d > scan_file_area_max:%d  dec_temp_to_cold_age_dx_count <= 0 break file_type:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_file_area_max,file_type);
					break;
				}
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
		 * file_area。牵涉到file_stat->temp链表，增删file_area必须要加锁!!!!!!!!!!!!!!!。并且，如果file_stat->temp链表
		 * 没有file_area则p_file_area是NULL，需要防护p_file_area是NULL的情况
		 *
		 *
	     *list_for_each_entry()机制保证，即便遍历的链表空返回的链表也是指向链表头，故p_file_stat_base不可能是NULL
		 */
		/*if(p_file_area)*/
		{
			spin_lock(&p_file_stat_base->file_stat_lock);
			/*如果是tiny small文件，则当该文件file_area个数超过64，则禁止把tiny_small_file_stat->temp链表上的hot和refault属性的file_area
			 *移动到链表头。否则将来该文件转成small文件时，这些hot和refault file_area将无法移动到small_file_stat->other链表，而是停留在
			 *small_file_stat->temp链表，于是，将来遍历到small_file_stat->temp链表有hot和refault属性的file_area，则会crash。
			 *仔细想想，如果p_file_area此时是tiny small的refault/hot/free属性的file_area，根本就执行不了list_move_enhance()把遍历过的
			 *file_area移动到链表头，因为限定了只有F_file_area_in_temp_list属性的file_area才可以*/
			if(FILE_STAT_TINY_SMALL != file_type || (FILE_STAT_TINY_SMALL == file_type && p_file_stat_base->file_area_count <= SMALL_FILE_AREA_COUNT_LEVEL)){
				if(can_file_area_move_to_list_head(p_file_area,&p_file_stat_base->file_area_temp,F_file_area_in_temp_list))
					list_move_enhance(&p_file_stat_base->file_area_temp,&p_file_area->file_area_list);
			}
			spin_unlock(&p_file_stat_base->file_stat_lock);
		}
	}

	/*参与内存回收的冷file_area个数*/
	if(file_stat_in_cache_file_base(p_file_stat_base)){
		p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_cold_file_area_count_from_temp += scan_cold_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_read_file_area_count_from_temp += scan_read_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.temp_to_hot_file_area_count += temp_to_hot_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_ahead_file_area_count_from_temp += scan_ahead_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.temp_to_warm_file_area_count += temp_to_warm_file_area_count;
	}
	else{
		p_hot_cold_file_global->mmap_file_shrink_counter.scan_cold_file_area_count_from_temp += scan_cold_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.scan_read_file_area_count_from_temp += scan_read_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.temp_to_hot_file_area_count += temp_to_hot_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.scan_ahead_file_area_count_from_temp += scan_ahead_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.temp_to_warm_file_area_count += temp_to_warm_file_area_count;
	}
	return scan_file_area_count;
}
#endif
/* 新版本，update函数不会再处理in_free、in_temp链表的file_area。并且file_area的in_temp属性去除，改为用没有
 * in_free、in_refault、in_hot、in_mapcount取代*/
static inline int file_stat_temp_list_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int scan_file_area_max,struct list_head *file_area_free_temp,unsigned int file_type,unsigned int age_dx_change_type)
{
	struct file_area *p_file_area = NULL,*p_file_area_temp;
	unsigned int scan_file_area_count = 0;
	unsigned int file_area_age_dx;
	unsigned int temp_to_warm_file_area_count = 0,scan_cold_file_area_count = 0;
	unsigned int scan_read_file_area_count = 0;
	unsigned int scan_ahead_file_area_count = 0;
	unsigned int temp_to_hot_file_area_count = 0;
	//unsigned int scan_hot_file_area_count;
	struct file_stat *p_file_stat = NULL;
	struct file_stat_small *p_file_stat_small = NULL;
	//unsigned int file_type;
	unsigned int file_area_type;
	//unsigned int file_area_age;
	unsigned int file_area_temp_to_warm_age_dx,file_area_temp_to_cold_age_dx;
	char file_stat_changed;
	char recliam_pages_from_from_writeonly_file = (AGE_DX_CHANGE_WRITEONLY_IN_EMERGENCY_RECLAIM == age_dx_change_type);
	/* 连续扫描到的非冷file_area个数，扫描到冷file_area则清0。用于只读文件的内存回收，如果连续扫描到多个非冷的file_area，
	 * 说明file_area_temp_to_cold_age_dx有点大，需要调小。如果这样遇到dec_temp_to_cold_age_dx_count次数。如果调小后，那说
	 * 明当前只读文件的大片file_area都刚访问过，那就结束遍历该文件的file_area了*/
	unsigned int scan_serial_warm_file_area_count = 0;
	char dec_temp_to_cold_age_dx_count = 16;
	char memory_reclaim_efficiency;
	char is_global_file_stat = file_stat_in_global_base(p_file_stat_base);
	//unsigned int scan_file_area_zero_page_count = 0;

	/*现在针对mmap的file_area_temp_to_cold_age_dx，在内存回收最开头执行的change_global_age_dx_for_mmap_file()函数里调整了，这里不再重复*/
	if(recliam_pages_from_from_writeonly_file){
		file_area_temp_to_cold_age_dx = p_hot_cold_file_global->writeonly_file_age_dx;
		file_area_temp_to_warm_age_dx = 0;
	}
	else{
	    file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
		file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_warm_age_dx;
	}
	/*if(file_stat_in_cache_file_base(p_file_stat_base)){
	  file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_warm_age_dx;
	  file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
	  }else{
	  file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_warm_age_dx + MMAP_FILE_TEMP_TO_WARM_AGE_DX;
	  file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx + MMAP_FILE_TEMP_TO_COLD_AGE_DX;
	  }*/
	//从链表尾开始遍历，链表尾的成员更老，链表头的成员是最新添加的
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_base->file_area_temp,file_area_list){

		
		/* scan_file_area个数超过max则break结束遍历，不能放到这里。因为此时的file_area还没有参与内存回收
		 * ，就在该函数被迫移动到file_stat->temp链表头，浪费一次遍历的机会*/
		/*if(++scan_file_area_count > scan_file_area_max)
		  break;*/
		scan_file_area_count ++;

		
		/* 新版本update函数不再设置hot file_area，而是遍历到该file_area时发现访问频次大于阈值再判断hot file_area
		 * file_area不能处于in_refault状态，否则file_area同时具备in_refault和in_hot状态而crash。竟然又遇到in_free链表
		 * 的file_area这里因为access_count大又被赋值了in_hot属性，而导致crash。file_area还有可能mapcount呀，干脆限制in_temp*/
		if(file_area_access_freq(p_file_area) > 2 && file_area_in_temp_list(p_file_area)/*!file_area_in_refault_list(p_file_area) && !file_area_in_free_list(p_file_area)*/){
			/*之前的方案hot file_area不清理temp属性，现在也不清理*/
			//clear_file_area_in_temp_list(p_file_area);
			set_file_area_in_hot_list(p_file_area);
			file_area_access_freq_clear(p_file_area);
		}

		if(file_stat_in_test_base(p_file_stat_base))
			printk("%s:1 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d global_age:%d scan_file_area_max:%d refault_page_count:%d file_area_temp_to_cold_age_dx:%d memory_pressure_level:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age,p_hot_cold_file_global->global_age,scan_file_area_max,p_file_stat_base->refault_page_count,p_hot_cold_file_global->file_area_temp_to_cold_age_dx,p_hot_cold_file_global->memory_pressure_level);

		/* 新版本file_area的in_temp属性是没有in_free、in_refault、in_hot、in_mapcount属性。并且file_area不会同时具备in_temp和in_hot属性，
		 * 也不会同时具备in_free和in_refault属性。这些代码都从update函数移除了。这个if是处理tiny_small_file->temp链表上非temp属性的file_area*/
		if(FILE_STAT_TINY_SMALL == file_type && 
				(!file_area_in_temp_list(p_file_area) /*|| file_area_in_hot_list(p_file_area)*/)){
			/*tiny small所有种类的file_area都在file_stat->temp链表，遇到hot和refault的file_area，只需要检测它是否长时间没访问，
			 *然后降级到temp的file_area。遇到free的file_area如果长时间没访问则要释放掉file_area结构。get_file_area_list_status()
			 是获取该file_area的状态，是free还是hot还是refault状态*/

			
			if(open_file_area_printk)
				printk("%s:1 file_stat:0x%llx file_area:0x%llx status:0x%x tiny small file\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			/*对于同时有in_temp和in_hot属性的file_area，要清理掉in_temp属性，否则file_stat_other_list_file_area_solve_common()
			 *会因同时具备两种属性而crash*/
			if(/*file_area_in_temp_list(p_file_area) &&*/ file_area_in_hot_list(p_file_area)){
				/*只有file_area个数小于64时才清理in_temp属性，原因见file_stat_temp_list_file_area_solve()函数开头的注释。
				 *有个问题，如果执行这行代码时，该文件执行file_area_alloc_and_init()分配大量file_area导致
				 *p_file_stat_base->file_area_count 大于64，
				 *但是p_file_stat_base->file_area_count最新的值还没同步过来，当前cpu的还小于64，这样会不会有问题？
				 *有很大问题，因为此时会清理掉该file_area的in_temp属性，但实际file_area个数超过64，将来这个
				 *热file_area因此还是热的，在退出该函数前，会被移动到tiny_small_file->temp链表头。继续，它
				 *转换成small_file后，这个热file_area将残留在small_file->temp链表，原因见
				 *file_stat_temp_list_file_area_solve()函数开头的注释。将来遍历这个small_file->temp链表上的热
				 *file_area，但没有in_temp属性，就会因状态错误而crash。怎么解决？
				 *这个问题隐藏的很深，单靠想象根本推理不出来，而一步步写出来就发现隐藏不管了!!!!!!!!!!!!!!!!!!!!!
				 *这里用spin_lock(&p_file_stat_base->file_stat_lock)加锁，跟file_area_alloc_and_init()里的
				 spin_lock(&p_file_stat_base->file_stat_lock)加锁并发。然后这里因为是加锁后再判断
				 p_file_stat_base->file_area_count是否小于64，此时肯定没有并发问题，因为这里获取到的是 
				 p_file_stat_base->file_area_count最新的值。但是加锁才浪费性能。算了，最后决定，tiny_small_file->temp
				 链表上的file_area的in_temp和in_hot属性共存，不再清理。

				 似乎这样可以解决问题，但是脑袋突然来了一个想法，tiny_small_file->temp链表上还有一种特殊的file_area，就是
				 同时有in_free和in_refault属性的。下边if(file_area_in_free_list(p_file_area) && file_area_in_refault_list(p_file_area))
				 执行时，如果该文件分配了很多的新的file_area添加到了tiny_small_file->temp链表头。然后执行
				 clear_file_area_in_free_list(p_file_area)清理file_area的in_free属性。然后当前函数执行最后，要把这些in_refault
				 的filea_area移动到tiny_small_file->temp链表头，不在链表尾的64个file_area中。于是该文件转成small file时，
				 这个in_refault的file_area无法移动到small_stat->other链表，而是残留在small_stat->temp链表，这样将来遍历到
				 small_stat->temp链表上的in_refault的file_area时，还会crash.

				 想来想去，终于想到一个完美的办法：这里遍历到in_temp与in_refault 的file_area 和 in_refault与in_free 的file_area时，
				 如果在退出该函数前，spin_lock(&p_file_stat_base->file_stat_lock)加锁，然后判断file_area个数超过64，不把这些特殊
				 的file_area移动到tiny_small_file->temp链表头就行了，停留在链表尾的64个file_area，完美。并且该函数最后原本移动file_area到链表头本身就有加锁。
				 */

				/*if(p_file_stat_base->file_area_count <= SMALL_FILE_AREA_COUNT_LEVEL)*/
				clear_file_area_in_temp_list(p_file_area);
				p_file_stat_base->file_area_count_in_temp_list --;
			}

			file_area_type = get_file_area_list_status(p_file_area);

			/*对于同时有in_free和in_refault属性的file_area(in_free的file_area被访问而在update函数又设置了in_refault)，
			 *要清理掉in_free属性，否则file_stat_other_list_file_area_solve_common()会因同时具备两种属性而crash。现在
			 *修改了，强制赋值1 << file_area_type in_free，没有了in_free，这是为了让该file_area走common函数的in_free分支处理，严谨*/

			//if(file_area_type == (1 << F_file_area_in_free_list | 1 << F_file_area_in_refault_list)){
			if(file_area_in_free_list(p_file_area) && file_area_in_refault_list(p_file_area)){
				/*file_stat_other_list_file_area_solve_common()函数里走in_free分支，发现file_area有in_free属性，会主动清理in_free属性，这里就不清理了*/
				//clear_file_area_in_free_list(p_file_area);
				file_area_type = 1 << F_file_area_in_free_list;
			}

			/*这里遇到一个bug，tiny small文件mapcount的file_area，竟然有in_hot标记，这个是正常现象，这里强制去掉in_hot标记*/
			if(file_area_in_mapcount_list(p_file_area) && file_area_in_hot_list(p_file_area))
				file_area_type = 1 << F_file_area_in_mapcount_list;

			file_stat_other_list_file_area_solve_common(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_area_type,FILE_STAT_TINY_SMALL);
			continue;
		}else{

			if(file_area_in_hot_list(p_file_area)){
				/* 当file_area被判定为hot后，没有清理in_temp_list状态，因此第一次来这里，没有in_temp_list则crash，
				 * 防止重复把file_area移动到file_stat->hot链表.注意，该file_area可能在update函数被并发设置了in_hot标记
				 * 新版本file_area不会同时具备in_temp和in_hot属性*/
				//if(!file_area_in_temp_list(p_file_area) || (file_area_in_temp_list_error(p_file_area) && !file_area_in_hot_list(p_file_area)))
				if(file_area_in_hot_list_error(p_file_area) && !file_area_in_mapcount_list(p_file_area))
					panic("%s:1 file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_temp error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

				spin_lock(&p_file_stat_base->file_stat_lock);
				clear_file_area_in_temp_list(p_file_area);
				//if(file_area_in_refault_list(p_file_area))
				//	clear_file_area_in_refault_list(p_file_area);
				/*热file_area肯定来自file_stat->temp链表，因此file_area移动到file_area->hot链表后要减1*/
				if(FILE_STAT_NORMAL == file_type){
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
					p_file_stat_base->file_area_count_in_temp_list --;
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
					p_file_stat->file_area_hot_count ++;
					BUG();
				}else if(FILE_STAT_SMALL == file_type){
					p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
					p_file_stat_base->file_area_count_in_temp_list --;
					list_move(&p_file_area->file_area_list,&p_file_stat_small->file_area_other);
				}else
					BUG();

				spin_unlock(&p_file_stat_base->file_stat_lock);
				temp_to_hot_file_area_count ++;
				continue;
			}
		}
		if(!file_area_in_temp_list(p_file_area) || file_area_in_temp_list_error(p_file_area))
			panic("%s:2 file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_temp error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

		/* 有些page是被kswapd进程内存回收的，这些page的file_area就不会被标记in_free。之后这些file_area的page
		 * 再被访问，就不会被标记in_fault。为了解决这个漏洞，在kswapd释放page执行到
		 * page_cache_delete_for_file_area()函数时，这些file_area没有in_free标记，被异步内存回收释放的page
		 * 有in_free标记。此时就标记file_area的in_free_kswaped。此时的file_area可能在
		 * file_stat->temp、hot、refault、free、other、warm链表，在遍历这些链表的file_area时，是否都要处理呢？
		 * 太麻烦！最终决定只在这些file_area被异步内存回收线程内存回收时，再处理。而只有file_stat->temp、warm
		 * 链表上的file_area才会参与内存回收，则在遍历file_stat->temp、warm链表上的file_area时，如果file_area
		 * 有in_free_kswaped标记，如果这些file_area有page，说明file_area的page在内存回收后，再次被访问，
		 * 则标记file_area in_refault，同时清理掉in_free_kswaped标记，然后把file_area移动到file_stat->refault或other链表。
		 * 具体还要看具体的文件file_stat类型，再做详细的处理。
		 *
		 * 漏了一种情况，就是small文件的other链表，该链表上有in_free_kswapd标记的的file_area，就始终无法遍历到了。
		 * 于是这种file_area要单独处理。还有如果这种file_area再file_stat->hot、free等链表，遍历到真的不处理吗？
		 * 最后，决定in_kswapd标记的file_area，不再单独处理。而是，如果没有page了则直接移动到in_free链表，
		 * 否则当成普通的file_area处理。并且，在file_stat->mapcount、hot、refault上的file_area，如果有
		 * in_kswapd标记，或者file_area没有page了，直接移动到file_stat->free链表，不再按照老的逻辑处理
		 */ 
		/* 如果被判定为refault file_area，直接continue，不再按照in_temp属性的file_area处理
		 *
		 * 最新改动，file_area_in_free_kswapd()不再使用了，没什么用，鸡肋，舍弃掉。因为这种file_area
		 * 可以用if(!file_area_have_page(p_file_area))代替，还占据了file_area->file_area_state一个宝贵的bit位
		 * */
		//if(file_area_in_free_kswapd(p_file_area))
		if(!file_area_have_page(p_file_area))
			if(kswapd_file_area_solve(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_type)){
				//scan_file_area_zero_page_count ++;
				goto next_file_area;
				//continue;
			}

		//get_file_area_age()函数里，会把file_area转换成hot、mapcount file_area需要特别注意!!!!!!!!!!!!
		get_file_area_age(p_file_stat_base,p_file_area,/*file_area_age,*/p_hot_cold_file_global,file_stat_changed,file_type,is_global_file_stat);
		if(file_stat_changed)
			goto next_file_area;
			//continue;

		file_area_age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;
		//file_area_age_dx = p_hot_cold_file_global->global_age - file_area_age;

		//if(file_area_age_dx > p_hot_cold_file_global->file_area_temp_to_warm_age_dx){
		if(file_area_age_dx > file_area_temp_to_warm_age_dx){
			/*file_area经过FILE_AREA_TEMP_TO_COLD_AGE_DX个周期还没有被访问，则被判定是冷file_area，然后就释放该file_area的page*/
			//if(file_area_age_dx > p_hot_cold_file_global->file_area_temp_to_cold_age_dx){
			if(file_area_age_dx > file_area_temp_to_cold_age_dx){

				/* 连续非冷file_area个数加1，针对非writeonly文件，即便是冷file_area这里也加1。保证该file_area是read page
				 * 而没有回收成功，也加1。加快scan_serial_warm_file_area_count的增大。如果回收成功则会清0*/
				if(!recliam_pages_from_from_writeonly_file){
					scan_serial_warm_file_area_count ++;
				}
				if(file_stat_in_test_base(p_file_stat_base))
					printk("%s:2_1 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d temp -> free try\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
#if 0
				/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
				if(file_area_in_ahead(p_file_area)){
					clear_file_area_in_ahead(p_file_area);
					scan_ahead_file_area_count ++;
					/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
					if(IS_MEMORY_ENOUGH(p_hot_cold_file_global) || file_area_age_dx < p_hot_cold_file_global->file_area_reclaim_ahead_age_dx)
						goto next_file_area;
				}
#endif				
				/* 当前内存不紧张且内存回收不困难，则遇到read文件页的file_area先跳过，不回收。目的是尽可能回收write文件页，
				 * read文件页一旦回收再被访问就会发生refault，不仅导致read耗时长，读磁盘功耗还高。但是如果这个read属性的
				 * file_area很长很长很长时间没访问，也参与内存回收*/
				if(file_area_page_is_read(p_file_area)){
					scan_read_file_area_count ++;
					/* 原本如果内存紧张，不再执行continue，然后回收该read文件页，但是这样容易refault。现在改为只有很长时间没访问，才会回收该read文件页
					 * 不能用continue，这样会导致无法执行该循环最后的代码：因为扫描过多热file_area而break跳出循环，因为scan_file_ara超过max而break跳出*/
					//if(!(IS_IN_MEMORY_EMERGENCY_RECLAIM(p_hot_cold_file_global) || file_area_age_dx > p_hot_cold_file_global->file_area_reclaim_read_age_dx))
					if(file_area_age_dx < p_hot_cold_file_global->file_area_reclaim_read_age_dx)
						goto next_file_area;
					    //container;
					clear_file_area_page_read(p_file_area);
				}

				//每遍历到一个就加一次锁，浪费性能，可以先移动到一个临时链表上，循环结束后加一次锁，然后把这些file_area或file_stat移动到目标链表???????
				spin_lock(&p_file_stat_base->file_stat_lock);
				/*为什么file_stat_lock加锁后要再判断一次file_area是不是被访问了,因为可能有这种情况:上边的if成立,此时file_area还没被访问。但是此时有进程
				  先执行hot_file_update_file_status()获取file_stat_lock锁,然后访问当前file_area,file_area不再冷了,当前进程此时获取file_stat_lock锁失败,
				  等获取file_stat_lock锁成功后，file_area的file_area_age就和global_age相等了。变量加减后的判断，在spin_lock前后各判断一次有必要的!!!!!*/
				//if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <  p_hot_cold_file_global->file_area_temp_to_cold_age_dx){
				if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <  file_area_temp_to_cold_age_dx){
					spin_unlock(&p_file_stat_base->file_stat_lock);
					goto next_file_area;
				}
				//access_count清0，如果内存回收期间又被访问了，access_count将大于0，将被判断为refault page。
				file_area_access_count_clear(p_file_area);
				clear_file_area_in_temp_list(p_file_area);
				p_file_stat_base->file_area_count_in_temp_list --;
				/*设置file_area处于file_stat的free_temp_list链表。这里设定，不管file_area处于file_stat->file_area_free_temp还是
				 *file_stat->file_area_free链表，都是file_area_in_free_list状态，没有必要再区分二者。主要设置file_area的状态需要
				 遍历每个file_area并file_stat_lock加锁，再多设置一次set_file_area_in_free_temp_list状态浪费性能。这点需注意!!!!!!!!!!!!!*/
				set_file_area_in_free_list(p_file_area);
				/*需要加锁，此时可能有进程执行hot_file_update_file_status()并发向该p_file_area前或者后插入新的file_area，这里是把该
				 * p_file_area从file_area_temp链表剔除，存在同时修改该p_file_area在file_area_temp链表前的file_area结构的next指针和在链表后的
				 * file_area结构的prev指针，并发修改同一个变量就需要加锁*/
				//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free_temp);
				list_move(&p_file_area->file_area_list,file_area_free_temp);
				spin_unlock(&p_file_stat_base->file_stat_lock);

				scan_cold_file_area_count ++;
				/*遇到冷file_area就清0。再加一个限制条件，必须成功回收该file_area的page，因为该file_area可能因为是read page而无法回收文件页*/
				if(scan_serial_warm_file_area_count)
					scan_serial_warm_file_area_count = 0;

				if(file_stat_in_test_base(p_file_stat_base))
					printk("%s:2_2 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d temp -> free done\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
			}else{
				/*温file_area移动到p_file_stat->file_area_warm链表。从file_stat->temp移动file_area到file_stat->warm链表，必须要加锁*/
				if(FILE_STAT_NORMAL == file_type){
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

					spin_lock(&p_file_stat_base->file_stat_lock);
					clear_file_area_in_temp_list(p_file_area);
					p_file_stat_base->file_area_count_in_temp_list --;
					//set_file_area_in_warm_list(p_file_area);
					list_num_update(p_file_area,POS_WARM);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
					spin_unlock(&p_file_stat_base->file_stat_lock);
					temp_to_warm_file_area_count ++;

					//printk("1 %s:file_stat:0x%llx file_area:0x%llx status:0x%x age:%d temp to warm\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
					if(file_stat_in_test_base(p_file_stat_base))
						printk("%s:3 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d temp -> warm\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
				}

				/*连续非冷file_area个数加1*/
				scan_serial_warm_file_area_count ++;
			}
		}

next_file_area:
	
		/*如果扫描到太多的file_area没有page，则结束遍历。不用了，改为使用check_memory_reclaim_efficiency()*/
		/*if(scan_file_area_zero_page_count > scan_file_area_max >> 4){
			break;
		}*/

		memory_reclaim_efficiency = check_memory_reclaim_efficiency(p_hot_cold_file_global,scan_file_area_count,scan_cold_file_area_count,file_type);

		/*如果是在内存紧缺模式，并且是在回收writeonly文件，那扫描的file_area个数不受scan_file_area_max限制*/
		if(recliam_pages_from_from_writeonly_file){
			/*连续扫描到多个file_area都是非冷的，或者内存回收效率太低*/
			if(scan_serial_warm_file_area_count >= SCAN_SERIAL_WARM_FILE_AREA_LEVEL || memory_reclaim_efficiency){
				scan_serial_warm_file_area_count = 0;

				/*调小file_area_temp_to_cold_age_dx以更容易回收到file_area*/
				if(file_area_temp_to_cold_age_dx > 0)
					file_area_temp_to_cold_age_dx -= 1;

				/*连续扫描到多个file_area都是非冷的，出现次数太多，说明该文件大部分file_area最近刚访问过，结束扫描该文件*/
				dec_temp_to_cold_age_dx_count --;
				if(0 == dec_temp_to_cold_age_dx_count){
					printk("1:%s:writeonly file_stat:0x%llx status:0x%x scan_file_area_count:%d scan_file_area_max:%d  dec_temp_to_cold_age_dx_count <= 0 break file_type:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_file_area_max,file_type);
					break;
				}
			}
		}else{

			/*现在每个文件的scan_file_area_max不受全局scan_file_area_max影响了，而是每个内存内存回收前，执行check_file_area_refault_and_scan_max()确定*/
			if(scan_file_area_count > scan_file_area_max){
				printk("2:%s:file_stat:0x%llx status:0x%x scan_file_area_count:%d > scan_file_area_max:%d break file_type:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_file_area_max,file_type);
				break;
			}

			/*如果扫描到的file_area有太多不是冷file_area，结束遍历*/
			/*if(scan_cold_file_area_count < scan_file_area_count >> 1){
				printk("%s:file_stat:0x%llx status:0x%x scan_file_area_count:%d scan_cold_file_area_count:%d too less  break\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_cold_file_area_count);
				break;
			}*/

			/*对于非writeonly的文件，如果遇到多次一片的file_area都是热的，则提前结束扫描该文件*/
			if(scan_serial_warm_file_area_count >= SCAN_SERIAL_WARM_FILE_AREA_LEVEL_FOR_READ_FILE || memory_reclaim_efficiency){
				scan_serial_warm_file_area_count = 0;
				dec_temp_to_cold_age_dx_count -= 5;
				if(dec_temp_to_cold_age_dx_count <= 0){
					printk("3:%s:file_stat:0x%llx status:0x%x scan_file_area_count:%d > scan_file_area_max:%d  dec_temp_to_cold_age_dx_count <= 0 break file_type:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_file_area_max,file_type);
					break;
				}
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
		 * file_area。牵涉到file_stat->temp链表，增删file_area必须要加锁!!!!!!!!!!!!!!!。并且，如果file_stat->temp链表
		 * 没有file_area则p_file_area是NULL，需要防护p_file_area是NULL的情况
		 *
		 *
	     *list_for_each_entry()机制保证，即便遍历的链表空返回的链表也是指向链表头，故p_file_stat_base不可能是NULL
		 */
		/*if(p_file_area)*/
		{
			spin_lock(&p_file_stat_base->file_stat_lock);
			/*如果是tiny small文件，则当该文件file_area个数超过64，则禁止把tiny_small_file_stat->temp链表上的hot和refault属性的file_area
			 *移动到链表头。否则将来该文件转成small文件时，这些hot和refault file_area将无法移动到small_file_stat->other链表，而是停留在
			 *small_file_stat->temp链表，于是，将来遍历到small_file_stat->temp链表有hot和refault属性的file_area，则会crash。
			 *仔细想想，如果p_file_area此时是tiny small的refault/hot/free属性的file_area，根本就执行不了list_move_enhance()把遍历过的
			 *file_area移动到链表头，因为限定了只有F_file_area_in_temp_list属性的file_area才可以*/
			if(FILE_STAT_TINY_SMALL != file_type || (FILE_STAT_TINY_SMALL == file_type && p_file_stat_base->file_area_count <= SMALL_FILE_AREA_COUNT_LEVEL)){
				if(can_file_area_move_to_list_head_for_temp_list_file_area(p_file_area,&p_file_stat_base->file_area_temp))
					list_move_enhance(&p_file_stat_base->file_area_temp,&p_file_area->file_area_list);
			}
			spin_unlock(&p_file_stat_base->file_stat_lock);
		}
	}

	/*参与内存回收的冷file_area个数*/
	if(file_stat_in_cache_file_base(p_file_stat_base)){
		p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_cold_file_area_count_from_temp += scan_cold_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_read_file_area_count_from_temp += scan_read_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.temp_to_hot_file_area_count += temp_to_hot_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_ahead_file_area_count_from_temp += scan_ahead_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.temp_to_warm_file_area_count += temp_to_warm_file_area_count;
	}
	else{
		p_hot_cold_file_global->mmap_file_shrink_counter.scan_cold_file_area_count_from_temp += scan_cold_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.scan_read_file_area_count_from_temp += scan_read_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.temp_to_hot_file_area_count += temp_to_hot_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.scan_ahead_file_area_count_from_temp += scan_ahead_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.temp_to_warm_file_area_count += temp_to_warm_file_area_count;
	}
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
#if 0
static noinline unsigned int file_stat_warm_list_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,unsigned int scan_file_area_max,struct list_head *file_area_free_temp,unsigned int file_type,unsigned int age_dx_change_type)
{
	struct file_area *p_file_area = NULL,*p_file_area_temp;
	struct file_stat_base *p_file_stat_base = NULL;
	unsigned int scan_file_area_count = 0;
	unsigned int file_area_age_dx;
	LIST_HEAD(file_area_move_to_temp_list);
	unsigned int scan_cold_file_area_count = 0;
	unsigned int scan_read_file_area_count = 0;
	unsigned int scan_ahead_file_area_count = 0;
	unsigned int warm_to_temp_file_area_count = 0;
	unsigned int warm_to_hot_file_area_count = 0;
	//unsigned int file_area_age;
	char file_stat_changed;
	unsigned int file_area_warm_to_temp_age_dx,file_area_temp_to_cold_age_dx;
	char recliam_pages_from_from_writeonly_file = (AGE_DX_CHANGE_WRITEONLY_IN_EMERGENCY_RECLAIM == age_dx_change_type);
	/* 连续扫描到的非冷file_area个数，扫描到冷file_area则清0。用于只读文件的内存回收，如果连续扫描到多个非冷的file_area，
	 * 说明file_area_temp_to_cold_age_dx有点大，需要调小。如果这样遇到dec_temp_to_cold_age_dx_count次数。如果调小后，那说
	 * 明当前只读文件的大片file_area都刚访问过，那就结束遍历该文件的file_area了*/
	unsigned int scan_serial_warm_file_area_count = 0;
	char dec_temp_to_cold_age_dx_count = 16;
	char memory_reclaim_efficiency;
	char is_global_file_stat = file_stat_in_global_base(p_file_stat_base);

	p_file_stat_base = &p_file_stat->file_stat_base;

	/*现在针对mmap的file_area_temp_to_cold_age_dx，在内存回收最开头执行的change_global_age_dx_for_mmap_file()函数里调整了，这里不再重复*/
	if(recliam_pages_from_from_writeonly_file){
		file_area_temp_to_cold_age_dx = p_hot_cold_file_global->writeonly_file_age_dx;
		file_area_warm_to_temp_age_dx = p_hot_cold_file_global->file_area_warm_to_temp_age_dx;
	}
	else{
		file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
		file_area_warm_to_temp_age_dx = p_hot_cold_file_global->file_area_warm_to_temp_age_dx;
	}
	/*if(file_stat_in_cache_file_base(p_file_stat_base)){
		file_area_warm_to_temp_age_dx = p_hot_cold_file_global->file_area_warm_to_temp_age_dx;
		file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
	}
	else{
		file_area_warm_to_temp_age_dx = p_hot_cold_file_global->file_area_warm_to_temp_age_dx + MMAP_FILE_WARM_TO_TEMP_AGE_DX;
		file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx + MMAP_FILE_TEMP_TO_COLD_AGE_DX;
	}*/

	//从链表尾开始遍历，链表尾的成员更老，链表头的成员是最新添加的
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat->file_area_warm,file_area_list){
		
		scan_file_area_count ++;

		/* 当file_area被判定为hot后，没有清理in_temp_list状态，因此第一次来这里，没有in_temp_list则crash，
		 * 防止重复把file_area移动到file_stat->hot链表.注意，该file_area可能在update函数被并发设置了in_hot标记*/
		if(!file_area_in_warm_list(p_file_area) || (file_area_in_warm_list_error(p_file_area) && !file_area_in_hot_list(p_file_area)))
			panic("%s file_area:0x%llx status:0x%x not in file_area_temp error\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

		if(file_stat_in_test_base(p_file_stat_base))
			printk("%s:1 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d global_age:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age,p_hot_cold_file_global->global_age);

		//if(file_area_in_free_kswapd(p_file_area))
		if(!file_area_have_page(p_file_area))
			if(kswapd_file_area_solve(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_type))
				continue;

		//get_file_area_age()函数里，会把file_area转换成hot、mapcount file_area需要特别注意!!!!!!!!!!!!
		get_file_area_age(p_file_stat_base,p_file_area,/*file_area_age,*/p_hot_cold_file_global,file_stat_changed,file_type,is_global_file_stat);
		if(file_stat_changed)
            continue;

		file_area_age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;
		/*有热file_area标记*/
		if(file_area_in_hot_list(p_file_area)){
			//spin_lock(&p_file_stat->file_stat_lock);

			clear_file_area_in_warm_list(p_file_area);
			//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
			list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot_or_mapcount);
			p_file_stat->file_area_hot_count ++;
			warm_to_hot_file_area_count ++;

			if(file_stat_in_test_base(p_file_stat_base))
				printk("%s:2 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d warm -> hot\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
			//spin_unlock(&p_file_stat->file_stat_lock);
			continue;
		}

		/* 由于把file_area移动到file_stat->temp链表后，移动起来又得spin_lock加锁，麻烦。当然，file_stat->temp
		 * 链表上的file_area可以在update函数里，在被访问时就移动到file_sta->temp链表头，避免在链表尾被遍历到，
		 * 纯属浪费，因为这是最近的刚访问过的file_area，不能参与内存回收*/
#if 0
		/*file_stat->warm链表上的file_area在file_area_warm_to_temp_age_dx个周期里被访问过，则移动到file_stat->temp链表。在
		 * file_temp链表上的file_area享有在hot_file_update()函数随着访问次数增多移动到file_stat->temp链表头的权利。
		 *注意，这里是唯一一次file_area_age_dx使用 "小于" 的情况。
		 *最新改动，因为把in_warm的file_area移动回in_temp_list，需要spin加锁，而异步内存回收方案主打"减少形如lruvec_lock锁"
		 *的使用，因此要尽可能少的把file_area从in_warm_list移动到in_temp_list，故把file_area_warm_to_temp_age_dx改为2
		 */
		if(file_area_age_dx < 2/*file_area_warm_to_temp_age_dx*/){
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
			p_file_stat_base->file_area_count_in_temp_list ++;
			//spin_unlock(&p_file_stat->file_stat_lock);
			warm_to_temp_file_area_count ++;
			
			if(file_stat_in_test_base(p_file_stat_base))
				printk("%s:3 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d warm -> temp\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
		}
#endif		
		/*否则file_stat->warm链表上长时间没访问的file_area移动的file_area移动到file_area_free_temp链表，参与内存回收，移动过程不用加锁*/
		if(file_area_age_dx > file_area_temp_to_cold_age_dx){

			/* 连续非冷file_area个数加1，针对非writeonly文件，即便是冷file_area这里也加1。保证该file_area是read page
			 * 而没有回收成功，也加1。加快scan_serial_warm_file_area_count的增大。如果回收成功则会清0*/
			if(!recliam_pages_from_from_writeonly_file){
				scan_serial_warm_file_area_count ++;
			}

			if(file_stat_in_test_base(p_file_stat_base))
				printk("%s:4 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d warm -> free try\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);

			/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
			if(file_area_in_ahead(p_file_area)){
				clear_file_area_in_ahead(p_file_area);
				scan_ahead_file_area_count ++;
				/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
				if(IS_MEMORY_ENOUGH(p_hot_cold_file_global) || file_area_age_dx < p_hot_cold_file_global->file_area_reclaim_ahead_age_dx)
					goto next_file_area;
			}

			/* 当前内存不紧张且内存回收不困难，则遇到read文件页的file_area先跳过，不回收。目的是尽可能回收write文件页，
			 * read文件页一旦回收再被访问就会发生refault，不仅导致read耗时长，读磁盘功耗还高。但是如果这个read属性的
			 * file_area很长很长很长时间没访问，也参与内存回收*/
			if(file_area_page_is_read(p_file_area)){
				scan_read_file_area_count ++;
				//if(!(IS_IN_MEMORY_PRESSURE_RECLAIM(p_hot_cold_file_global) || file_area_age_dx > p_hot_cold_file_global->file_area_reclaim_read_age_dx))
				if(file_area_age_dx < p_hot_cold_file_global->file_area_reclaim_read_age_dx)
					goto next_file_area;
				clear_file_area_page_read(p_file_area);
			}

			//access_count清0，如果内存回收期间又被访问了，access_count将大于0，将被判断为refault page。
			file_area_access_count_clear(p_file_area);
			scan_cold_file_area_count ++;
			clear_file_area_in_warm_list(p_file_area);
			set_file_area_in_free_list(p_file_area);
			list_move(&p_file_area->file_area_list,file_area_free_temp);

			/*遇到冷file_area就清0。再加一个限制条件，必须成功回收该file_area的page，因为该file_area可能因为是read page而无法回收文件页*/
			if(scan_serial_warm_file_area_count)
				scan_serial_warm_file_area_count = 0;

			if(file_stat_in_test_base(p_file_stat_base))
				printk("%s:5 file_stat:0x%llx status:0x%x file_area:0x%llx status:0x%x age:%d warm -> free done\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
		}else{
			/*连续遇到非冷file_area则加1*/
			scan_serial_warm_file_area_count ++;
		}

next_file_area:
		
		memory_reclaim_efficiency = check_memory_reclaim_efficiency(p_hot_cold_file_global,scan_file_area_count,scan_cold_file_area_count,file_type);
		if(recliam_pages_from_from_writeonly_file){
			/*连续扫描到多个file_area都是非冷的，或者内存回收效率太低*/
			if(scan_serial_warm_file_area_count >= SCAN_SERIAL_WARM_FILE_AREA_LEVEL || memory_reclaim_efficiency){
				scan_serial_warm_file_area_count = 0;

				/*调小file_area_temp_to_cold_age_dx以更容易回收到file_area。现在file_area_temp_to_cold_age_dx最小值是0，这样内存回收是不是太暴力了???????*/
				if(file_area_temp_to_cold_age_dx > 0)
					file_area_temp_to_cold_age_dx -= 1;

				/*连续扫描到多个file_area都是非冷的，出现次数太多，说明该文件大部分file_area最近刚访问过，结束扫描该文件*/
				dec_temp_to_cold_age_dx_count --;
				if(dec_temp_to_cold_age_dx_count <= 0){
					printk("1:%s:writeonly file_stat:0x%llx status:0x%x scan_file_area_count:%d scan_file_area_max:%d  dec_temp_to_cold_age_dx_count <= 0 break file_type:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_file_area_max,file_type);
					break;
				}
			}
		}else{
			/*如果是在内存紧缺模式，并且是在回收writeonly文件，那扫描的file_area个数不受scan_file_area_max限制*/
			if(scan_file_area_count > scan_file_area_max){
				if(FILE_STAT_NORMAL == file_type)
				    printk("2:%s:file_stat:0x%llx status:0x%x scan_file_area_count:%d > scan_file_area_max:%d break file_type:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_file_area_max,file_type);

				break;
			}

			/*如果扫描到的file_area有太多不是冷file_area，结束遍历*/
			/*if(scan_cold_file_area_count < scan_file_area_count >> 1){ 每次到这里if都成立
				printk("%s:file_stat:0x%llx status:0x%x scan_file_area_count:%d scan_cold_file_area_count:%d too less  break\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_cold_file_area_count);
				break;
			}*/

			/*对于非writeonly的文件，如果遇到多次一片的file_area都是热的，则提前结束扫描该文件*/
			if(scan_serial_warm_file_area_count >= SCAN_SERIAL_WARM_FILE_AREA_LEVEL_FOR_READ_FILE || memory_reclaim_efficiency){
				scan_serial_warm_file_area_count = 0;
				dec_temp_to_cold_age_dx_count -= 3;
				if(dec_temp_to_cold_age_dx_count <= 0){
					printk("3:%s:file_stat:0x%llx status:0x%x scan_file_area_count:%d scan_file_area_max:%d  dec_temp_to_cold_age_dx_count <= 0 break file_type:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_file_area_max,file_type);
					break;
				}
			}
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
		 *是的话才允许把p_file_area到链表尾的file_area移动到链表头。
	     *
		 *list_for_each_entry()机制保证，即便遍历的链表空返回的链表也是指向链表头，故p_file_area不可能是NULL
         */
		if(/*p_file_area && */can_file_area_move_to_list_head(p_file_area,&p_file_stat->file_area_warm,F_file_area_in_warm_list))
			list_move_enhance(&p_file_stat->file_area_warm,&p_file_area->file_area_list);
	}

	/*将file_stat->warm链表上近期访问过的file_area移动到file_stat->temp链表头*/
	if(!list_empty(&file_area_move_to_temp_list)){
		struct file_area *p_file_area_last = list_last_entry(&file_area_move_to_temp_list,struct file_area,file_area_list);

		spin_lock(&p_file_stat_base->file_stat_lock);
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
		list_splice(&file_area_move_to_temp_list,&p_file_stat_base->file_area_temp);
		spin_unlock(&p_file_stat_base->file_stat_lock);

		/* file_area_move_to_head_count清0，并且smp_wmb()，synchronize_rcu()。之后，进程读写文件，先执行了smp_rmb()，
		 * 再执行到hot_file_update_file_status()，看到file_area_move_to_head_count是0，或者小于0，就不会再把
		 * p_file_stat->file_area_temp链表上的file_area移动到链表头了。为什么 file_area_move_to_head_count可能小于0，
		 * 因为可能这里对file_area_move_to_head_count赋值0，hot_file_update_file_status()并发执行
		 * file_area_move_to_head_count减1，就可能小于0了*/
		p_file_stat_base->file_area_move_to_head_count = 0;
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
				panic("%s file_stat:0x%llx  status:0x%x file_area:0x%llx state:0x%x has in temp_list\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state);

			set_file_area_in_temp_list(p_file_area);
			/* 如果遍历到原file_area_move_to_temp_list链表最后一个file_area，说明这些file_area都设置过in_temp链表属性了
			 * 要立即break*/
			if(p_file_area == p_file_area_last)
				break;
		}
		barrier();
		/*list_for_each_entry遍历结束后，才能恢复p_file_stat->file_area_move_to_head_count大于0.*/
		p_file_stat_base->file_area_move_to_head_count = FILE_AREA_MOVE_TO_HEAD_COUNT_MAX;
		hot_cold_file_global_info.file_area_move_to_head_count_max = FILE_AREA_MOVE_TO_HEAD_COUNT_MAX;
	}
	/*参与内存回收的冷file_area个数*/
	if(file_stat_in_cache_file_base(p_file_stat_base)){
		p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_cold_file_area_count_from_warm += scan_cold_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_read_file_area_count_from_warm += scan_read_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_ahead_file_area_count_from_warm += scan_ahead_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_file_area_count_from_warm += scan_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.warm_to_temp_file_area_count += warm_to_temp_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.warm_to_hot_file_area_count += warm_to_hot_file_area_count;
	}else{
		p_hot_cold_file_global->mmap_file_shrink_counter.scan_cold_file_area_count_from_warm += scan_cold_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.scan_read_file_area_count_from_warm += scan_read_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.scan_ahead_file_area_count_from_warm += scan_ahead_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.scan_file_area_count_from_warm += scan_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.warm_to_temp_file_area_count += warm_to_temp_file_area_count;
		p_hot_cold_file_global->mmap_file_shrink_counter.warm_to_hot_file_area_count += warm_to_hot_file_area_count;
	}

	return scan_file_area_count;
}
#endif

/*
引入多层warm链表机制后，自然跟根据file_area的冷热程度把file_area移动到writeonly_or_cold、warm、warm_hot、hot 等链表。但是怎么反映file_area的冷热呢？
1:最初是想根据age_dx=global_age - file_area_age 差值决定file_area升级/降级到 writeonly_or_cold、warm、warm_hot、hot 链表，比如age_dx 在10~20移动到hot链表，
  age_dx在20~50移动到warm_hot链表，age_dx在50~100移动到warm链表，age_dx大于100的移动到writeonly_or_cold链表。这个是最初的方案，但是感觉太麻烦了
    if(age_dx in 10~20)
       move to hot链表
	else if(age_dx in 20~50)
	   move to warm_hot链表
	else if(age_dx in 50~100)
	   move to warm链表
	else if(age_dx > 100)
	   move to writeonly_or_cold链表
   
    仅仅if else都要用很多个，并且file_area可能处于writeonly_or_cold、warm、warm_hot、hot 等链表，都要按照这个if...else逻辑处理吗。并且，age_dx差值大小真的能
	反应file_area的冷热吗。age_dx只能反应file_area最近一次访问的访问时间呀，不能反应file_area历史上被访问的频次呀
	1:一个file_area检测到在5个周期都被访问了，总访问次数5，然后过了30个周期没有再被访问过age_dx=30
	2:另一个file_area在被访问过一次后，过了10个周期没有再被访问，age_dx=10
	请问，这两个file_area到底那个更热，哪个更应该被内存回收？如果按照age_dx差值大小，自然是后者file_area更热。但实际看是前者file_area更热。
	
	经过这点分析，我之前判断file_area冷热的方法，过分依赖age_dx，不太合理。age_dx只能反应file_area最近一次访问的访问时间呀，而file_area历史上被访问的次数也很重要呀。
    假如我能统计file_area有多少个周期被访问，次数统计到access_count(每个周期内被访问一次只加1)，
	处于writeonly_or_cold、warm、warm_hot、hot 等链表上的file_area，
	1:如果access_count大于1则把file_area向更热的一级warm链表移动，处于不同warm链表的file_area都这样判断。大大简化了热file_area向更热一级的warm链表移动的判断逻辑。
	2:如果access_count大于5，不管file_area处于什么链表，直接把file_area移动到hot链表。
	3:如果access_count是1只是把file_area所在的warm list链表头，这样链表尾的file_area都是没有访问过的.原本我只想把针对file_stat->warm链表，
	把访问过的移动到链表头，这样链表尾的file_area都是没有访问过的，内存紧张时直接从链表尾遍历file_area，能遍历到大量的冷file_area进行内存回收。
	4:如果access_count是0的file_area则不动
	5:如果access_count是0且age_dx大于60则把file_area移动到writeonly_or_cold链表

    这样就引入一个问题，怎么统计file_area的访问次数access_count，这个数据最好能统计到5~10，越大越能精细的统计file_area的冷热。但是file_area->file_area_state的32个bi位
	已经完全用满了，难道要再弄出5个bit位表示access_count？只能从现有的file_area->file_area_state已经使用的bit位，看哪些能舍弃掉而用于access_count了。
	首先想到可以把bit15~bit12的shadow bit腾出来。替代方法是，如果file_area的page是被异步内存线程回收的，则给file_area->pages[]赋值1。如果是被kswapd回收的page，是在
	file_area->pages[]赋值shadow，这个shadow一定大于1，并且bit0是1。如此依然可以根据file_area->pages[]是否是1判断page是被谁回收的。ok，这样终于省出了4个bit位了。
	但是不能这4个bit位只能表示4个数据，不能单独使用。要做成一个变量access_count，占4个bit位，这样最大值是15，access_count可以表示历史上file_area有16个周期被访问了
	
	新的问题又来了，file_area在 writeonly_or_cold、warm、warm_hot(global_file_stat还有 warm_cold、warm_middle、warm_middle_hot)链表之间来回移动，一共有6个链表，
	将来还可能会增加其他warm链表。总得给每个file_area一个编号，表示warm处于哪个链表吧。按照历史经验，存在file_area处于错误的链表这种情况。因此必须给file_area
	一个唯一的编号，表示file_area处于writeonly_or_cold、warm、warm_hot(global_file_stat还有 warm_cold、warm_middle、warm_middle_hot)的哪个链表。可是
	file_area->file_area_state的32个bit被占满了呀。只能再想办法腾出空闲的bit位了。最后决定，把原有的in_warm状态舍舍弃掉，把表示file_area访问频次的in_access、in_ahead
	状态位舍弃掉，凑出3个bit位，用warm_list_num变量表示，最大值8，足够表示file_area所在的各级warm链表编号了。
	
	于是就决定file_area->file_area_state的低8位，弄一个union warm_list_num_and_access_freq变量，bit0~bit3表示file_area的access_count，bit4~bit6表示file_area处于哪个
	warm链表warm_list_num。bit7表示file_area的in_hot状态。但是要注意，给warm_list_num_and_access_freq的access_count、warm_list_num赋值时，异步内存回收线程和
	读写文件执行update函数的进程，都会有赋值，必须考虑并发行为。最终决定用cmpxchg原子赋值方案，具体参考file_area_access_freq_inc()、list_num_update()函数。
	
	
	解决了这些基础问题，又来了一个新的问题。前边说过
	"3:如果access_count是1只是把file_area所在的warm list链表头，这样链表尾的file_area都是没有访问过的."怎么解决这个问题，这个是有必要的，我也一直相对
	file_stat->warm链表上的file_area这么处理。但是有个问题，如果我单纯只把"access_count是1只是把file_area所在的warm list链表头"，那遍历链表就要陷入
	死循环了。比如，现在从file_stat->warm链表尾开始遍历，把"access_count是1只是把file_area所在的warm list链表头"后，这些file_area聚集在链表头，
	遍历的p_file_area_pos指针会永远移动不到链表头。因为每次遍历的access_count是1的file_area都移动到了file_stat->warm链表头。p_file_area_pos指针
	会永远指向这些file_area。当然可以把他们的access_count清0，但是就清除了file_area的访问信息。最好的办法是，把access_count是1的file_area先移动到
	一个tmp临时链表头，这样p_file_area_pos指针一定会指向链表头，遍历结束。但是这个tmp临时链表移动要是全局的，因为可能file_stat->warm本次的file_area_max
	达到了，结束遍历，只能下个周期再次遍历。要把这个tmp链表放到file_stat结构体吗？当然可行。但是想想，还得再增加warm_list_num变量，表示当前正遍历
	file_stat的哪个warm链表，还得再增加一个p_head指针，用它指向当前遍历的file_stat的那个warm链表的链表头。要在file_stat结构体塞这几个变量，
	file_stat的体积又增大了，浪费空间呀。一定要弄到file_stat结构体吗？其实想想，完全可以在hot_cold_file_global结构体，增加一个变量，保存当前
	遍历的file_stat的warm链表编号、tmp临时链表、p_head指针。完全可以呀，不用非得搞在file_stat结构体里。只要做好防护，防止不同文件的file_area、
	不同warm链表的file_area错误移动到tmp临时链表即可。这个可以靠file_area->mapping来防护，靠file_area的warm_list_num防护。
	最后这个结构体定义为struct current_scan_file_stat_info。mmap和cache文件各有一个current_scan_file_stat_info，global mmap和cache file_stat也各有一个。
 * */
//static inline void current_scan_file_stat_info(struct current_scan_file_stat_info *p_current_scan_file_stat_info,struct file_stat *p_file_stat)
static inline void update_current_scan_file_stat_info_for_list_head_and_num(struct current_scan_file_stat_info *p_current_scan_file_stat_info,struct file_stat *p_file_stat)
{
	switch(p_file_stat->traverse_warm_list_num){
		case POS_WARM_HOT:
			p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_file_stat->file_area_warm_hot;
            p_current_scan_file_stat_info->p_up_file_area_list_head = &p_file_stat->file_area_hot;
			p_current_scan_file_stat_info->up_list_num = -1;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_file_stat->file_area_warm;
			p_current_scan_file_stat_info->down_list_num = POS_WARM;
			break;
		case POS_WARM:
			p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_file_stat->file_area_warm;
            p_current_scan_file_stat_info->p_up_file_area_list_head = &p_file_stat->file_area_warm_hot;
			p_current_scan_file_stat_info->up_list_num = POS_WARM_HOT;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_file_stat->file_area_warm_cold;
			p_current_scan_file_stat_info->down_list_num = POS_WARM_COLD;
			break;
		case POS_WARM_COLD:
			p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_file_stat->file_area_warm_cold;
            p_current_scan_file_stat_info->p_up_file_area_list_head = &p_file_stat->file_area_warm;
			p_current_scan_file_stat_info->up_list_num = POS_WARM;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_file_stat->file_area_writeonly_or_cold;
			p_current_scan_file_stat_info->down_list_num = POS_WIITEONLY_OR_COLD;
			break;
		case POS_WIITEONLY_OR_COLD:
			p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_file_stat->file_area_writeonly_or_cold;
            p_current_scan_file_stat_info->p_up_file_area_list_head = &p_file_stat->file_area_warm_cold;
			p_current_scan_file_stat_info->up_list_num = POS_WARM_COLD;
			p_current_scan_file_stat_info->p_down_file_area_list_head = NULL;
			p_current_scan_file_stat_info->down_list_num = -1;
			break;
		default:
			BUG();
	}
}
static inline void global_file_stat_current_scan_file_stat_info(struct current_scan_file_stat_info *p_current_scan_file_stat_info,struct global_file_stat *p_global_file_stat)
{
	switch(p_global_file_stat->file_stat.traverse_warm_list_num){
		case POS_WARM_HOT:
			p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm_hot;
            p_current_scan_file_stat_info->p_up_file_area_list_head = &p_global_file_stat->file_stat.file_area_hot;
			p_current_scan_file_stat_info->up_list_num = -1;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_global_file_stat->file_area_warm_middle_hot;
			p_current_scan_file_stat_info->down_list_num = POS_WARM_MIDDLE_HOT;
			break;
		case POS_WARM_MIDDLE_HOT:
			p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_global_file_stat->file_area_warm_middle_hot;
            p_current_scan_file_stat_info->p_up_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm_hot;
			p_current_scan_file_stat_info->up_list_num = POS_WARM_HOT;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm;
			p_current_scan_file_stat_info->down_list_num = POS_WARM;

			break;
		case POS_WARM:
			p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm;
            p_current_scan_file_stat_info->p_up_file_area_list_head = &p_global_file_stat->file_area_warm_middle_hot;
			p_current_scan_file_stat_info->up_list_num = POS_WARM_MIDDLE_HOT;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_global_file_stat->file_area_warm_middle;
			p_current_scan_file_stat_info->down_list_num = POS_WARM_MIDDLE;

			break;
		case POS_WARM_MIDDLE:
			p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_global_file_stat->file_area_warm_middle;
            p_current_scan_file_stat_info->p_up_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm;
			p_current_scan_file_stat_info->up_list_num = POS_WARM;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm_cold;
			p_current_scan_file_stat_info->down_list_num = POS_WARM_COLD;
			break;
		case POS_WARM_COLD:
			p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm_cold;
            p_current_scan_file_stat_info->p_up_file_area_list_head = &p_global_file_stat->file_area_warm_middle;
			p_current_scan_file_stat_info->up_list_num = POS_WARM_MIDDLE;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_global_file_stat->file_stat.file_area_writeonly_or_cold;
			p_current_scan_file_stat_info->down_list_num = POS_WIITEONLY_OR_COLD;
			break;
		case POS_WIITEONLY_OR_COLD:
			p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_global_file_stat->file_stat.file_area_writeonly_or_cold;
            p_current_scan_file_stat_info->p_up_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm_cold;
			p_current_scan_file_stat_info->up_list_num = POS_WARM_COLD;
			p_current_scan_file_stat_info->p_down_file_area_list_head = NULL;
			p_current_scan_file_stat_info->down_list_num = -1;
			break;
		default:
			BUG();
	}
}
/*遍历的warm链表顺序是  POS_WIITEONLY_OR_COLD、POS_WARM_COLD、POS_WARM_MIDDLE、POS_WARM、POS_WARM_MIDDLE、HOT POS_WARM_HOT ，遍历结束再从POS_WIITEONLY_OR_COLD开始*/
static inline void update_global_file_stat_next_multi_level_warm_or_writeonly_list(struct current_scan_file_stat_info *p_current_scan_file_stat_info,struct global_file_stat *p_global_file_stat)
{
	if(NULL == p_current_scan_file_stat_info->p_traverse_file_stat)
		BUG();

	/*1:当前file_stat的warm链表上的file_area完成了，更新next_num_list，要先把之前遍历过的移动到temp_head链表上的file_area移动回warm链表头*/
	if(!list_empty(&p_current_scan_file_stat_info->temp_head)){
		list_splice_init(&p_current_scan_file_stat_info->temp_head,p_current_scan_file_stat_info->p_traverse_file_area_list_head);
	}
	p_current_scan_file_stat_info->p_traverse_file_stat = NULL;
	p_current_scan_file_stat_info->p_traverse_file_area_list_head = NULL;

	switch(p_global_file_stat->file_stat.traverse_warm_list_num){
		case POS_WARM_HOT:
			/* 遍历过file_stat->warm_hot链表上的file_area后，不再允许异步内存回收线程traverse_file_stat_multi_level_warm_list()遍历
			 * file_stat->writeonly链表上的file_area了。因为现在判定有点浪费性能，反正异步内存回收时，遍历file_stat->writeonly
			 * 链表上的file_area进行内存回收时。如果file_area最近访问过，直接移动到file_stat->warm链表。*/
			//p_global_file_stat->file_stat.traverse_warm_list_num = POS_WIITEONLY_OR_COLD;
			p_global_file_stat->file_stat.traverse_warm_list_num = POS_WARM_COLD;

			/*p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_global_file_stat->file_stat.file_area_writeonly_or_cold;
			p_current_scan_file_stat_info->p_up_file_area_list_head = &p_global_file_stat->file_area_warm_cold;
			p_current_scan_file_stat_info->up_list_num = POS_WARM_COLD;
			p_current_scan_file_stat_info->p_down_file_area_list_head = NULL;
			p_current_scan_file_stat_info->down_list_num = -1;*/
			break;
		case POS_WARM_MIDDLE_HOT:
			p_global_file_stat->file_stat.traverse_warm_list_num = POS_WARM_HOT;

			/*p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm_hot;
			p_current_scan_file_stat_info->p_up_file_area_list_head = &p_global_file_stat->file_stat.file_area_hot;
			p_current_scan_file_stat_info->up_list_num = -1;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_global_file_stat->file_area_warm_middle_hot;
			p_current_scan_file_stat_info->down_list_num = POS_WARM_MIDDLE_HOT;*/
			break;
		case POS_WARM:
			p_global_file_stat->file_stat.traverse_warm_list_num = POS_WARM_MIDDLE_HOT;

			/*p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_global_file_stat->file_area_warm_middle_hot;
			p_current_scan_file_stat_info->p_up_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm_hot;
			p_current_scan_file_stat_info->up_list_num = POS_WARM_HOT;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm;
			p_current_scan_file_stat_info->down_list_num = POS_WARM;*/
			break;
		case POS_WARM_MIDDLE:
			p_global_file_stat->file_stat.traverse_warm_list_num = POS_WARM;

			/*p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm;
			p_current_scan_file_stat_info->p_up_file_area_list_head = &p_global_file_stat->file_area_warm_middle_hot;
			p_current_scan_file_stat_info->up_list_num = POS_WARM_MIDDLE_HOT;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_global_file_stat->file_area_warm_middle;
			p_current_scan_file_stat_info->down_list_num = POS_WARM_MIDDLE;*/
			break;
		case POS_WARM_COLD:
			p_global_file_stat->file_stat.traverse_warm_list_num = POS_WARM_MIDDLE;

			/*p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_global_file_stat->file_area_warm_middle;
			p_current_scan_file_stat_info->p_up_file_area_list_head = &p_global_file_stat->file_stat.file_area_warm;
			p_current_scan_file_stat_info->up_list_num = POS_WARM;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_global_file_stat->file_area_warm_cold;
			p_current_scan_file_stat_info->down_list_num = POS_WARM_COLD;*/
			break;
		case POS_WIITEONLY_OR_COLD:
			p_global_file_stat->file_stat.traverse_warm_list_num = POS_WARM_COLD;

			/*p_current_scan_file_stat_info->p_traverse_file_area_list_head = &p_global_file_stat->file_area_warm_cold;
			p_current_scan_file_stat_info->p_up_file_area_list_head = &p_global_file_stat->file_area_warm_middle;
			p_current_scan_file_stat_info->up_list_num = POS_WARM_MIDDLE;
			p_current_scan_file_stat_info->p_down_file_area_list_head = &p_global_file_stat->file_stat.file_area_writeonly_or_cold;
			p_current_scan_file_stat_info->down_list_num = POS_WIITEONLY_OR_COLD;*/
			break;
		default:
			BUG();
	}
}
struct mult_warm_list_age_dx{
	unsigned int file_area_cold_level;
	unsigned int to_down_list_age_dx;
	unsigned int to_writeonly_cold_list_age_dx;
};
/* mult_warm_list_age_dx_level_solve()函数里对file_area_cold_level、to_down_list_age_dx、to_writeonly_cold_list_age_dx
 * 的调整，对内存回收的效率、内存回收refault高影响巨大。如果这些参数偏小，内存回收效率很高，但是refault很高。这些参数
 * 偏大，内存回收效率很低，refault却很低。简单说，越容易说回收则refault越高*/
static inline void mult_warm_list_age_dx_level_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,char is_global_file_stat,unsigned int list_num,char is_cache_file, struct mult_warm_list_age_dx *p_age_dx)
{
	unsigned int refault_page_count = p_file_stat->file_stat_base.refault_page_count;

	p_age_dx->file_area_cold_level = p_hot_cold_file_global->file_area_cold_level;
	p_age_dx->to_down_list_age_dx = p_hot_cold_file_global->to_down_list_age_dx;
	p_age_dx->to_writeonly_cold_list_age_dx = p_hot_cold_file_global->to_writeonly_cold_list_age_dx;

	/*mmap文件越来越难回收*/
	if(!is_cache_file){
		//p_age_dx->file_area_cold_level += 50;
		//p_age_dx->to_down_list_age_dx += 50;
		p_age_dx->file_area_cold_level += 80;
		p_age_dx->to_down_list_age_dx += 100;
		p_age_dx->to_writeonly_cold_list_age_dx += 80;

		/* 热度越高的warm链表上的file_area，p_age_dx->to_writeonly_cold_list_age_dx越大。但是实际调试发现个问题，
		 * 比如一个原本处于hot链表上的file_area，age_dx是100，第一次扫描降级到warm_hot链表。过了几个周期，再次
		 * 被扫描到，age_dx是105，于是file_area降级到warm_hot_midle链表。又过了几个周期，age_dx是109，再次降级
		 * 到warm链表。就是file_area降级的太快，很多mmap文件file_area发生refault就是这个原因。于是想了个办法，
		 * 针对mmap文件，越靠下级链表的file_area，age_dx越大，至少让file_area在下级链表的file_area多停留一段
		 * 时间，cache文件的file_area不再这样处理
		 * */
		switch(list_num)
		{
			case POS_WARM_HOT:
				p_age_dx->to_writeonly_cold_list_age_dx += 64;
				p_age_dx->to_down_list_age_dx += 64;
				break;
			case POS_WARM_MIDDLE_HOT:
				p_age_dx->to_writeonly_cold_list_age_dx += 64;
				p_age_dx->to_down_list_age_dx += 64;
				break;
			case POS_WARM:
				p_age_dx->to_writeonly_cold_list_age_dx += 96;
				p_age_dx->to_down_list_age_dx += 96;
				break;
			case POS_WARM_MIDDLE:
				p_age_dx->to_writeonly_cold_list_age_dx += 128;
				p_age_dx->to_down_list_age_dx += 128;
				break;
			case POS_WARM_COLD:
				p_age_dx->to_writeonly_cold_list_age_dx += 128;
				p_age_dx->to_down_list_age_dx += 128;
				break;
			case POS_WIITEONLY_OR_COLD:
				break;
			default:
				panic("list_num:%d error\n",list_num);
		}

	}

	/*global_file_stat更容易回收*/
	/*if(is_global_file_stat)
		p_age_dx->to_writeonly_cold_list_age_dx -= (p_age_dx->to_writeonly_cold_list_age_dx >> 2);*/

	/*内存越紧张p_age_dx->to_writeonly_cold_list_age_dx越小*/
	switch(p_hot_cold_file_global->memory_pressure_level)
	{
		/*内存非常紧缺*/
		case MEMORY_EMERGENCY_RECLAIM:
			if(is_cache_file /*&& p_hot_cold_file_global->global_age - p_file_stat->file_stat_base.recent_access_age > 10*/){
				p_age_dx->to_writeonly_cold_list_age_dx -= (p_age_dx->to_writeonly_cold_list_age_dx >> 1);
				p_age_dx->to_down_list_age_dx -= (p_age_dx->to_down_list_age_dx >> 1);
			}
			break;
			/*内存紧缺*/
		case MEMORY_PRESSURE_RECLAIM:
			//p_age_dx->to_writeonly_cold_list_age_dx = p_age_dx->to_writeonly_cold_list_age_dx - (p_age_dx->to_writeonly_cold_list_age_dx >> 2);
			break;
			/*内存碎片有点多，或者前后两个周期分配的内存数太多*/
		case MEMORY_LITTLE_RECLAIM:
		    p_age_dx->to_writeonly_cold_list_age_dx += 256;
		    p_age_dx->to_down_list_age_dx += 128;
			break;
		case MEMORY_IDLE_SCAN:
		    p_age_dx->to_writeonly_cold_list_age_dx += 512;
		    p_age_dx->to_down_list_age_dx += 256;
			break;
		default:
			BUG();
	}

	/*refault page越多越难回收。global_file_stat的refault_page_count后续要单独调整。但只要针对普通的normal文件，不针对global_file_stat*/
	if(refault_page_count > 16 && refault_page_count <= 32){
		p_age_dx->to_writeonly_cold_list_age_dx += 32;
		p_age_dx->to_down_list_age_dx += 32;
	}else if(refault_page_count > 32 && refault_page_count <= 64){
		if(is_global_file_stat){
			p_age_dx->to_writeonly_cold_list_age_dx += 32;
			p_age_dx->to_down_list_age_dx += 32;
		}else{
			p_age_dx->to_writeonly_cold_list_age_dx += 64;
			p_age_dx->to_down_list_age_dx += 64;
		}
	}
	else{
		if(is_global_file_stat){
			p_age_dx->to_writeonly_cold_list_age_dx += (128 + 128);
			p_age_dx->to_down_list_age_dx += (128 + 96);
		}else{
			p_age_dx->to_writeonly_cold_list_age_dx += 256;
			p_age_dx->to_down_list_age_dx += (128 + 64);
		}
	}

	/*内存紧张已经持续了很长时间，降低各个age_dx*/
	if(MEMORY_EMERGENCY_RECLAIM ==  p_hot_cold_file_global->memory_pressure_level && p_hot_cold_file_global->memory_still_memrgency_after_reclaim > 5){
		/*mmap文件比cache文件大一倍*/
		int age_dx_factor = (1 - is_cache_file);

		/*如果普通文件，但热file_area很多，不降级age_dx，容易refault*/
		if(!is_global_file_stat && is_file_stat_may_hot_file(p_file_stat))
			return;


		if(p_age_dx->to_down_list_age_dx > (60 << age_dx_factor))
			p_age_dx->to_down_list_age_dx = (60 << age_dx_factor);

		if(p_age_dx->to_down_list_age_dx > (80 << age_dx_factor))
			p_age_dx->to_down_list_age_dx = (80 << age_dx_factor);

		if(p_age_dx->to_writeonly_cold_list_age_dx > (150 << age_dx_factor))
			p_age_dx->to_writeonly_cold_list_age_dx = (150 << age_dx_factor);
	}
}
#if 0
static inline unsigned int traverse_file_stat_multi_level_warm_list(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,struct current_scan_file_stat_info *p_current_scan_file_stat_info,unsigned int scan_file_area_max,char is_global_file_stat,char is_cache_file)
{
	struct file_area *p_file_area,*p_file_area_temp;
	struct global_file_stat *p_global_file_stat;
	unsigned int scan_file_area_count = 0;
	unsigned char file_stat_changed = 0;
	unsigned char access_freq;
	struct file_stat_base *p_file_stat_base = &p_file_stat->file_stat_base;
	//unsigned int file_area_in_list_state;
	unsigned int age_dx_level;
	unsigned int age_dx;
	unsigned char read_file_area;
	unsigned char list_num_for_file_area;
    struct mult_warm_list_age_dx mult_warm_list_age_dx;

	/* 如果要遍历的warm连表是空的，不能直接return，要继续执行，因为下边list_entry_is_head()会判断出p_file_area是链表头，
	 * 然后直接执行该函数最后if(list_entry_is_head(p_file_area, p_current_scan_file_stat_info->p_traverse_file_area_list_head, file_area_list))
	 * 里的代码：判断出p_file_area是链表头，说明当前链表的file_area没有了(都遍历完了或者链表原本是空的),则p_traverse_file_stat清NULL，更新next_num_list*/
	/*if(list_empty(p_current_scan_file_stat_info->p_traverse_file_area_list_head))------这段代码不能删除，警示
		return scan_file_area_count;*/


	if(1 != p_current_scan_file_stat_info->traverse_list_num)
		mult_warm_list_age_dx_level_solve(p_hot_cold_file_global,p_file_stat,is_global_file_stat,p_current_scan_file_stat_info->traverse_list_num,is_cache_file,&mult_warm_list_age_dx);

	p_file_area = p_current_scan_file_stat_info->p_traverse_first_file_area;
	while(!list_entry_is_head(p_file_area, p_current_scan_file_stat_info->p_traverse_file_area_list_head, file_area_list) 
			&& scan_file_area_count < scan_file_area_max){

		/* file_stat->temp链表上的file_area是一次性移动到file_stat->warm链表，因此file_stat->warm链表上的file_area有in_temp标记还要清理掉。
		 * 还有个问题，如果是tiny small文件转成global_file_stat，那file_area还有可能in_refault、in_hot、in_free、in_mapcount。但是感觉
		 * 每遍历file_area都要有下边一堆判断，浪费性能，最后决定
		 * 1：tiny small文件的file_area，在转成global_file_stat时，直接清理file_area的in_refault、in_hot、in_free、in_mapcount标记
		 * 2：file_stat和glboal_file_stat创建的新的file_area，先移动到file_stat->temp链表，再移动到file_stat->warm链表。等该file_area
		 * 被判定为热file_area时再清理掉in_temp标记*/
#if 0	
		if(POS_WARM == p_current_scan_file_stat_info->traverse_list_num){
			if(file_area_in_temp_list(p_file_area))
				clear_file_area_in_temp_list(p_file_area);

			/*如果file_area有in_temp标记，到这里时已经清理掉，就不会进入下边的switch...case，否则会panic*/
			file_area_in_list_state = p_file_area->file_area_state & FILE_AREA_LIST_MASK;
			/*如果是tiny small文件转成global_file_stat，那file_area还有可能in_refault、in_hot、in_free、in_mapcount*/
			if(is_global_file_stat && file_area_in_list_state){
				switch(file_area_in_list_state){
					case 1 << F_file_area_in_hot_list:
						list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_hot);
						break;
					case 1 << F_file_area_in_mapcount_list:
						struct global_file_stat *p_global_file_stat = container_of(p_file_stat,struct global_file_stat,file_stat);
						list_move(&p_file_area->file_area_list,&p_global_file_stat->file_area_mapcount);
						break;
					case 1 << F_file_area_in_refault_list:
						list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_refault);
						break;
					case 1 << F_file_area_in_free_list:
						list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_free);
						break;
						/*是0说明是global_file_stat的warm_list链表上的file_area，file_area_in_list_state是0根本不会执行到这里*/
						/*case 0:
						  break;*/
					default:
						panic("file_area:0x%llx status:0x%x\n",(u64)p_file_area,p_file_area->file_area_state);
				}
				/*file_area移动到hot、mapcount、refault、free链表了，直接遍历下一个file_area*/
				goto get_next_file_area;
			}
		}
#endif	
		list_num_for_file_area = list_num_get(p_file_area);

		if(file_area_in_deleted(p_file_area) || (p_file_area->file_area_state & FILE_AREA_LIST_MASK) != 0)
			panic("%s file_stat:0x%llx  file_area:0x%llx status:0x%x mapping:0x%llx\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,(u64)p_file_area->mapping);

		/* 重大bug，如果p_file_area在下边被list_move到了其他链表，就不能在循环最后p_file_area = list_prev_entry(p_file_area,file_area_list)
		 * 获取下一个遍历的file_area了，因为已经跨链表了，犯了同样的问题。解决办法是最开始p_file_area_temp获取本链表要遍历的下一个file_area*/
		p_file_area_temp = list_prev_entry(p_file_area,file_area_list);


		/*这里还能对file_area->file_area_state 更多bit进行异常判断??????????*/
		if(list_num_for_file_area != p_current_scan_file_stat_info->traverse_list_num || p_file_area->file_area_age > p_hot_cold_file_global->global_age){
			panic("%s file_stat:0x%llx  status:0x%x file_area:0x%llx state:0x%x file_area_num:%d  traverse_list_num:%d age:%u %u error\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_base.file_stat_status,(u64)p_file_area,p_file_area->file_area_state,list_num_for_file_area,p_current_scan_file_stat_info->traverse_list_num,p_file_area->file_area_age,p_hot_cold_file_global->global_age);
		}

		/*如果file_area没有page，再把file_area移动到zero_page_file_area_list链表。ctags时有大量的这种文件，0个page的file_area*/
		if(is_global_file_stat && !file_area_have_page(p_file_area)){
			/*向global_file_stat的链表移动file_area，需要加个file_stat_in_global_base检测，否则会造成内存越界*/
			if(file_stat_in_global_base(&p_file_stat->file_stat_base))
				panic("%s file_stat:0x%llx  file_area:0x%llx status:0x%x not in global\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

			p_global_file_stat = container_of(p_file_stat,struct global_file_stat,file_stat);
			/*访问计数必须清0，否则干预后续判断*/
			file_area_access_freq_clear(p_file_area);
			list_num_update(p_file_area,POS_ZERO_PAGE);
			list_move(&p_file_area->file_area_list,&p_global_file_stat->zero_page_file_area_list);
			goto get_next_file_area;
		}

		/* if成立说明file_area对应的文件被iput了，并把file_area移动到了global_file_stat_delete链表，这里不能
		 * list_del(&p_file_area->file_area_list)。因为将来遍历global_file_stat_delete链表上的file_area，也会
		 * list_del(&p_file_area->file_area_list)，那就从链表剔除file_area了*/
		if(file_area_in_mapping_delete(p_file_area)){
			//list_del(&p_file_area->file_area_list);
			printk("%s file_stat:0x%llx  file_area:0x%llx status:0x%x in_mapping_delete\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);
			goto get_next_file_area;
		}

		get_file_area_age(&p_file_stat->file_stat_base,p_file_area,p_hot_cold_file_global,file_stat_changed,FILE_STAT_NORMAL,is_global_file_stat);
		if(file_stat_changed)
			goto get_next_file_area;

		//如果file_stat被iput()了，那file_stat_base.mapping会被设置NULL呀，if就成立了。没事，因为对普通文件file_stat内存回收时，inode加锁了的，不会触发iput()
		if(!is_global_file_stat && p_file_area->mapping != p_file_stat->file_stat_base.mapping){
				panic("p_file_stat:0x%llx  file_area:0x%llx status:0x%x mapping:0x%llx 0x%llx\n",(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,(u64)p_file_area->mapping,(u64)p_file_stat->file_stat_base.mapping);
		}

		

		/*获取file_area前后两次扫到的时的访问次数*/
		access_freq = file_area_access_freq(p_file_area);
		read_file_area = file_area_in_read(p_file_area);
		age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;;

		
		/* cache文件只写的file_area直接移动到file_area_writeonly_or_cold链表，或者访问很频繁的，先不动?????????
		 * 还有一点，如果mmap文件解除所用page的mmap映射后，该文件不会转成cache文件。之后这些file_area就需要
		 * 大量转成writeonly file_area。并且，mmap文件里也有cache wrtiteonly的file_area。以上3中情况，目前都没处理???????*/
		/*现在发现mysql测试时，有很多refult page竟然是这里直接把file_area移动到writeonly链表导致的。于是要做限制，如果
		 *是writeonly文件，则直接把file_area移动到writeonly链表，但是非writeonly文件，如果file_area没有in_read标记，这些
		 *file_area可能只是分配了但是没有读写，导致access_freq是0*/
		if( POS_WIITEONLY_OR_COLD != list_num_for_file_area){
			if(file_stat_in_writeonly_base(p_file_stat_base) || (is_cache_file && !read_file_area /*&& access_freq > 0*/)){
								
				if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
					printk("1:%s file_stat:0x%llx file_area:0x%llx state:0x%x access_freq:%d index:%d >>> writeonly\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,access_freq,p_file_area->start_index);

				/*跨链表移动必须对file_area_access_freq清0，否则导致在新的链表也被判定位热file_area*/
				file_area_access_freq_clear(p_file_area);
				list_num_update(p_file_area,POS_WIITEONLY_OR_COLD);
				list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_writeonly_or_cold);
				goto get_next_file_area;
			}
		}else{
			/*如果是writeonly的file_area要单独处理。如果是writeonly的file_area，直接跳过，永远停留在writeonly链表。
		      如果是read属性的file_area，就要根据access_freq决定把file_area移动到上一层的链表了*/
            if(!read_file_area)
				goto get_next_file_area;
		}

		/* 1:access_freq是0没有被访问。非长长时间每访问的直接移动到file_area_writeonly_or_cold链表，否则只是移动到下一级链表
		 * 2:access_freq很大，但很长时间没访问，这样情况完全有可能，比如处于warm_hot链表的file_area前几分钟被多次访问，但是之后
		 * 就再也没有访问过了。这种情况也要file_area降级处理或者直接移动到下一级的链表，甚至是writeonly链表。阈值160可以基于file_area的属性动态调整*/
		if(0 == access_freq || age_dx > mult_warm_list_age_dx.file_area_cold_level){
			//unsigned int age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;

			/* 如果是当前遍历的是warm链表上的file_area，且这些file_area因长时间没访问而移动到下一级的链表，这些file_area如果有in_temp属性则清理掉
			 * 不用清理了，现在去掉了file_area的in_temp bit，用没有hot、mapcount、refault、free bit取代，就是全0，而新分配的file_area这些bit都是0，
			 * 故不用再专门清理file_area的in_temp*/
			/*if(POS_WARM == p_current_scan_file_stat_info->traverse_list_num && file_area_in_temp_list(p_file_area)){
				clear_file_area_in_temp_list(p_file_area);
				if(get_file_area_list_status(p_file_area) != 0)
					panic("%s file_stat:0x%llx  status:0x%x file_area:0x%llx state:0x%x file_area_num:%d  traverse_list_num:%d age:%u %u file_area status error\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_base.file_stat_status,(u64)p_file_area,p_file_area->file_area_state,list_num_for_file_area,p_current_scan_file_stat_info->traverse_list_num,p_file_area->file_area_age,p_hot_cold_file_global->global_age);
			}*/

			/* 这里的处理非常关键，如果in_read的属性的file_area，如果长时间没访问就要清理掉in_read属性。
			 * 1:根据内存紧张程度调整age_dx_level，内存越紧张age_dx_level越小
			 * 2:热度越高的warm链表上的file_area，age_dx_level越大
			 * 3:mmap的file_area，age_dx_level要调大一倍，甚至更高*/
			 
			 /* cache文件的in_read属性的file_area，age_dx_level越大。错了，这个不用判断，因为cache文件的in_read属性的file_area不会走到
			  * 这个分支，而是走上边的if分支*/
            /*if(read_file_area)
                age_dx_level *= 2;*/
		
			if(POS_WIITEONLY_OR_COLD == list_num_for_file_area)
				goto get_next_file_area;


			if(age_dx > mult_warm_list_age_dx.to_writeonly_cold_list_age_dx){

				if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
					printk("2:%s file_stat:0x%llx file_area:0x%llx state:0x%x age_dx_level:%d >>> writeonly\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,age_dx_level);

				/*如果in_read的属性的file_area，如果长时间没访问就要清理掉in_read属性。这里的处理非常关键，否则会跟干扰后续的内存回收*/
				if(read_file_area)
				    clear_file_area_page_read(p_file_area);

				list_num_update(p_file_area,POS_WIITEONLY_OR_COLD);
				list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_writeonly_or_cold);
			}
			/*如果file_area处最低级的writeonly_or_cold链表，p_down_file_area_list_head是NULL，就不再向下移动这种file_area了*/
			else if(age_dx > mult_warm_list_age_dx.to_down_list_age_dx && p_current_scan_file_stat_info->p_down_file_area_list_head){
				if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
					printk("3:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> down_list_num:%d\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,p_current_scan_file_stat_info->down_list_num);

				list_num_update(p_file_area,p_current_scan_file_stat_info->down_list_num);
				list_move(&p_file_area->file_area_list,p_current_scan_file_stat_info->p_down_file_area_list_head);
			}
		}else if(1 == access_freq && is_cache_file){/*只访问一次不温不热的file_area则移动到链表头*/
			if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
				printk("4:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> tmp\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

			list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->temp_head);
		}else if(access_freq > 5){/*访问次数频繁，直接移动到热file_stat->hot链表*/
			/*跨链表移动必须对file_area_access_freq清0，否则导致在新的链表也被判定位热file_area*/
			file_area_access_freq_clear(p_file_area);
			
			set_file_area_in_hot_list(p_file_area);
			list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_hot);
			p_file_stat->file_area_hot_count ++;
			if(file_stat_in_test_base(p_file_stat_base)|| is_global_file_stat_file_in_debug(p_file_area->mapping))
				printk("5:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> hot\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

			//mmap文件热file_area太多则移动到global hot_file_stat链表
			check_hot_file_stat_and_move_global(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_stat,is_global_file_stat);
		}else{/*被多次访问，但不是太多则把file_area移动到上一级更热的file_stat链表*/
			if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
				printk("6:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> up_list_num:%d\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,p_current_scan_file_stat_info->up_list_num);

			/*跨链表移动必须对file_area_access_freq清0，否则导致在新的链表也被判定位热file_area*/
			file_area_access_freq_clear(p_file_area);
			/*如果向上移动的warm_list是file_area_hot链表，则要设置file_area的in_hot标记，否则list_num_update*/
			if(p_current_scan_file_stat_info->p_up_file_area_list_head == &p_current_scan_file_stat_info->p_traverse_file_stat->file_area_hot){
				//if(file_area_in_temp_list(p_file_area))
				//	clear_file_area_in_temp_list(p_file_area);
				set_file_area_in_hot_list(p_file_area);
			}else	
				list_num_update(p_file_area,p_current_scan_file_stat_info->up_list_num);
			
			list_move(&p_file_area->file_area_list,p_current_scan_file_stat_info->p_up_file_area_list_head);
		}
get_next_file_area:
		/*接着遍历前一个file_area*/
		//p_file_area = list_prev_entry(p_file_area,file_area_list);
		p_file_area = p_file_area_temp;
		scan_file_area_count ++;
	}

	/* 当前链表上的file_area都遍历完了，要做几个重点工作
	 * 1：把暂存在current_scan_file_stat_info->temp_head不温不热的file_area移动到p_traverse_file_area_list_head链表头，
	 * 这样链表尾的都是没访问的过file_area，链表头的file_area都是访问过但不热的file_area，相当于按照file_area冷热程度
	 * 聚集起来了。如果遇到内存紧缺，直接从链表头遍历冷file_area进行内存回收，效率很高
	 * 2：针对普通文件file_stat，确定更新下一次遍历该文件的哪个warm链表，并且要决定是否要遍历下一个文件file_stat。什么意思，
	 * 如果file_stat->warm遍历到了很多的冷file_area，那就继续遍历file_stat->warm_hot的file_area。否则说明该文件可能
	 * 大部分file_area都是热的，不再遍历了，直接遍历下一个文件
	 * 针对global_file_stat，直接下一次遍历哪个warm链表即可，因为它只有一个
	 * */
	if(list_entry_is_head(p_file_area, p_current_scan_file_stat_info->p_traverse_file_area_list_head, file_area_list)){
		/*if(!list_empty(&p_current_scan_file_stat_info->temp_head)){
			list_splice_init(&p_current_scan_file_stat_info->temp_head,p_current_scan_file_stat_info->p_traverse_file_area_list_head);
		}*/

		printk("%s file_stat:0x%llx traverse_list_num:%d is_global_file_stat:%d is_cache_file:%d scan_file_area_count:%d scan_file_area_max:%d traverse ok\n",__func__,(u64)p_current_scan_file_stat_info->p_traverse_file_stat,p_current_scan_file_stat_info->traverse_list_num,is_global_file_stat,is_cache_file,scan_file_area_count,scan_file_area_max);
		if(is_global_file_stat){
			struct global_file_stat *p_global_file_stat = container_of(p_file_stat,struct global_file_stat,file_stat);
			update_global_file_stat_next_multi_level_warm_or_writeonly_list(p_current_scan_file_stat_info,p_global_file_stat);
		}
		else{
			/*普通文件的某个warm链表file_area遍历完后，必须把p_current_scan_file_stat_info->p_traverse_file_stat设置NULL，
			 * 然后下个周期遍历时，发现它是NULL，才会遍历的新的文件file_stat*/
			//p_current_scan_file_stat_info->p_traverse_file_stat = NULL;
			//p_current_scan_file_stat_info->p_traverse_file_area_list_head = NULL;
			update_file_stat_next_multi_level_warm_or_writeonly_list(p_current_scan_file_stat_info,p_file_stat);
		}
	}else{
		/* 否则说明遍历的file_area超过max导致的遍历结束，则把查询到的最后一个但还没遍历的file_area保存到
		 * p_current_scan_file_stat_info->p_traverse_first_file_area，下轮循环继续遍历*/

		/* 又一个SB的设计，如果上边循环最后遍历的p_file_area被移动到p_current_scan_file_stat_info->p_up_file_area_list_head、
		 * p_current_scan_file_stat_info->p_down_file_area_list_head、hot、writeonly_or_cold等链表，简单说这个file_area被
		 * 移动到另外的链表了。然后下次继续遍历该file_stat的file_area时，就不能从这个p_file_area开始遍历了，因为它已经被
		 * 移动到其他链表了！因此，必须要确保赋值给p_current_scan_file_stat_info->p_traverse_first_file_area的file_area，
		 * 没有被移动到其他链表，怎么保证呢？把p_file_area_temp赋值给p_traverse_first_file_area即可。因为p_file_area_temp
		 * 是p_file_area在遍历的链表里的前一个file_area。当因为遍历的file_area个数超过max导致的遍历结束，执行到这里时，
		 * p_file_area_temp这个file_area能确保是没有遍历过的。不对，上边的循环最后，现有p_file_area=p_file_area_temp，
		 * 然后才会有遍历的file_area个数超过max导致的遍历结束，执行到这里时，p_file_area和p_file_area_temp是同一个。
		 * 因此啥都不用改*/
		p_current_scan_file_stat_info->p_traverse_first_file_area = p_file_area;
		//p_current_scan_file_stat_info->p_traverse_first_file_area = p_file_area_temp;


        printk("%s file_stat:0x%llx traverse_list_num:%d is_global_file_stat:%d is_cache_file:%d scan_file_area_count:%d scan_file_area_max:%d not traverse ok\n",__func__,(u64)p_current_scan_file_stat_info->p_traverse_file_stat,p_current_scan_file_stat_info->traverse_list_num,is_global_file_stat,is_cache_file,scan_file_area_count,scan_file_area_max);
	}

	return scan_file_area_count;
}
#endif
static inline void check_multi_level_warm_list_file_area_valid(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,struct file_area *p_file_area,struct current_scan_file_stat_info *p_current_scan_file_stat_info,char list_num_for_file_area,char is_global_file_stat)
{
	if(file_area_in_deleted(p_file_area) || (p_file_area->file_area_state & FILE_AREA_LIST_MASK) != 0)
		panic("%s file_stat:0x%llx  file_area:0x%llx status:0x%x mapping:0x%llx\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,(u64)p_file_area->mapping);

	/*这里还能对file_area->file_area_state 更多bit进行异常判断??????????*/
	if(list_num_for_file_area != p_current_scan_file_stat_info->traverse_list_num || p_file_area->file_area_age > p_hot_cold_file_global->global_age){
		panic("%s file_stat:0x%llx  status:0x%x file_area:0x%llx state:0x%x file_area_num:%d  traverse_list_num:%d age:%u %u error\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_base.file_stat_status,(u64)p_file_area,p_file_area->file_area_state,list_num_for_file_area,p_current_scan_file_stat_info->traverse_list_num,p_file_area->file_area_age,p_hot_cold_file_global->global_age);
	}

	/* 如果file_stat被iput()了，那file_stat_base.mapping会被设置NULL呀，if就成立了。没事，因为对普通文件file_stat内存回收时，inode加锁了的，不会触发iput()
	 * 但如果文件被iput了，file_stat的file_area->mapping会被赋值NULL，此时不能触发panic，因此要加上if(!file_stat_in_delete_base())限制！错了，在遍历普通文件
	 * 的file_stat时，是inode加锁的，此时文件不可能会被iput()*/
	if(!is_global_file_stat && p_file_area->mapping != p_file_stat->file_stat_base.mapping){
		panic("p_file_stat:0x%llx  file_area:0x%llx status:0x%x mapping:0x%llx 0x%llx\n",(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,(u64)p_file_area->mapping,(u64)p_file_stat->file_stat_base.mapping);
	}
}
static inline void access_freq_solve_for_writeonly_or_cold_file_area(struct current_scan_file_stat_info *p_current_scan_file_stat_info,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area)
{

	if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
		printk("2:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> writeonly\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
	list_num_update(p_file_area,POS_WIITEONLY_OR_COLD);
	list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_writeonly_or_cold);
}
static inline void access_freq_solve_for_down_file_area(struct current_scan_file_stat_info *p_current_scan_file_stat_info,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area)
{
	if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
		printk("3:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> down_list_num:%d\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,p_current_scan_file_stat_info->down_list_num);

	list_num_update(p_file_area,p_current_scan_file_stat_info->down_list_num);
	list_move(&p_file_area->file_area_list,p_current_scan_file_stat_info->p_down_file_area_list_head);

}
static inline void access_freq_solve_for_tmp_file_area(struct current_scan_file_stat_info *p_current_scan_file_stat_info,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area)
{
	if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
		printk("4:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> tmp\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

	list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->temp_head);
}
static inline void access_freq_solve_for_up_file_area(struct current_scan_file_stat_info *p_current_scan_file_stat_info,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,unsigned char file_area_new_access_count)
{
	if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
		printk("6:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> up_list_num:%d\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,p_current_scan_file_stat_info->up_list_num);

	/*跨链表移动必须对file_area_access_freq清0，否则导致在新的链表也被判定位热file_area*/
	//file_area_access_freq_clear(p_file_area);
	file_area_access_freq_set(p_file_area,file_area_new_access_count);
	/*如果向上移动的warm_list是file_area_hot链表，则要设置file_area的in_hot标记，否则list_num_update*/
	if(p_current_scan_file_stat_info->p_up_file_area_list_head == &p_current_scan_file_stat_info->p_traverse_file_stat->file_area_hot){
		struct file_stat *p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

		//if(file_area_in_temp_list(p_file_area))
		//	clear_file_area_in_temp_list(p_file_area);
		set_file_area_in_hot_list(p_file_area);
		p_file_stat->file_area_hot_count ++;
	}else	
		list_num_update(p_file_area,p_current_scan_file_stat_info->up_list_num);

	list_move(&p_file_area->file_area_list,p_current_scan_file_stat_info->p_up_file_area_list_head);

}
static inline void access_freq_solve_for_hot_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct current_scan_file_stat_info *p_current_scan_file_stat_info,struct file_stat *p_file_stat,struct file_area *p_file_area,char is_global_file_stat)
{
	/*跨链表移动必须对file_area_access_freq清0，否则导致在新的链表也被判定位热file_area*/
	file_area_access_freq_clear(p_file_area);

	set_file_area_in_hot_list(p_file_area);
	list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_hot);
	p_file_stat->file_area_hot_count ++;
	if(file_stat_in_test_base(&p_file_stat->file_stat_base)|| is_global_file_stat_file_in_debug(p_file_area->mapping))
		printk("5:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> hot\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

	//mmap文件热file_area太多则移动到global hot_file_stat链表
	check_hot_file_stat_and_move_global(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_stat,is_global_file_stat);
}

static unsigned int traverse_file_stat_multi_level_warm_list(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,struct current_scan_file_stat_info *p_current_scan_file_stat_info,unsigned int scan_file_area_max,char is_global_file_stat,char is_cache_file,struct mult_warm_list_age_dx *p_mult_warm_list_age_dx)
{
	struct file_area *p_file_area,*p_file_area_temp;
	struct global_file_stat *p_global_file_stat;
	unsigned int scan_file_area_count = 0;
	unsigned char file_stat_changed = 0;
	unsigned char access_freq;
	struct file_stat_base *p_file_stat_base = &p_file_stat->file_stat_base;
	//unsigned int file_area_in_list_state;
	//unsigned int age_dx_level;
	unsigned int age_dx;
	unsigned char read_file_area;
	unsigned char list_num_for_file_area;
    //struct mult_warm_list_age_dx mult_warm_list_age_dx;

	/* 如果要遍历的warm连表是空的，不能直接return，要继续执行，因为下边list_entry_is_head()会判断出p_file_area是链表头，
	 * 然后直接执行该函数最后if(list_entry_is_head(p_file_area, p_current_scan_file_stat_info->p_traverse_file_area_list_head, file_area_list))
	 * 里的代码：判断出p_file_area是链表头，说明当前链表的file_area没有了(都遍历完了或者链表原本是空的),则p_traverse_file_stat清NULL，更新next_num_list*/
	/*if(list_empty(p_current_scan_file_stat_info->p_traverse_file_area_list_head))------这段代码不能删除，警示
		return scan_file_area_count;*/


	/* 观察到一个问题，随着mysql压测时间变长，只写文件越来越多，而一个只写文件每次的scan_file_area_max很大。内存紧张时，扫描一个writeonly
	 * 文件就可能消耗掉scan_file_area_max，导致本轮内存回收只能扫描一个writeonly文件，无法扫描其他writeonly文件，而从其他wrteonly文件
	 * 回收到page。并且，这种文件file_area只会存在于file_stat->warm、writeonly链表，内存回收时会在direct_recliam_file_area_for_file_stat()
	 * 函数直接从file_stat->warm、writeonly链表回收page！因此，writeonly文件不扫描太多的file_area，本身就会因为扫描太多filie_area而浪费
	 * 太多的cpu，如果此时mysql压测导致内存紧张，就无法立即回收到很多内存，结果导致kswapd内存回收而触发了大量的refault*/
	if(file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base))
		scan_file_area_max = 32;

	p_file_area = p_current_scan_file_stat_info->p_traverse_first_file_area;
	while(!list_entry_is_head(p_file_area, p_current_scan_file_stat_info->p_traverse_file_area_list_head, file_area_list) 
			&& scan_file_area_count < scan_file_area_max){

		/* file_stat->temp链表上的file_area是一次性移动到file_stat->warm链表，因此file_stat->warm链表上的file_area有in_temp标记还要清理掉。
		 * 还有个问题，如果是tiny small文件转成global_file_stat，那file_area还有可能in_refault、in_hot、in_free、in_mapcount。但是感觉
		 * 每遍历file_area都要有下边一堆判断，浪费性能，最后决定
		 * 1：tiny small文件的file_area，在转成global_file_stat时，直接清理file_area的in_refault、in_hot、in_free、in_mapcount标记
		 * 2：file_stat和glboal_file_stat创建的新的file_area，先移动到file_stat->temp链表，再移动到file_stat->warm链表。等该file_area
		 * 被判定为热file_area时再清理掉in_temp标记*/
#if 0	
		if(POS_WARM == p_current_scan_file_stat_info->traverse_list_num){
			if(file_area_in_temp_list(p_file_area))
				clear_file_area_in_temp_list(p_file_area);

			/*如果file_area有in_temp标记，到这里时已经清理掉，就不会进入下边的switch...case，否则会panic*/
			file_area_in_list_state = p_file_area->file_area_state & FILE_AREA_LIST_MASK;
			/*如果是tiny small文件转成global_file_stat，那file_area还有可能in_refault、in_hot、in_free、in_mapcount*/
			if(is_global_file_stat && file_area_in_list_state){
				switch(file_area_in_list_state){
					case 1 << F_file_area_in_hot_list:
						list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_hot);
						break;
					case 1 << F_file_area_in_mapcount_list:
						struct global_file_stat *p_global_file_stat = container_of(p_file_stat,struct global_file_stat,file_stat);
						list_move(&p_file_area->file_area_list,&p_global_file_stat->file_area_mapcount);
						break;
					case 1 << F_file_area_in_refault_list:
						list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_refault);
						break;
					case 1 << F_file_area_in_free_list:
						list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_free);
						break;
						/*是0说明是global_file_stat的warm_list链表上的file_area，file_area_in_list_state是0根本不会执行到这里*/
						/*case 0:
						  break;*/
					default:
						panic("file_area:0x%llx status:0x%x\n",(u64)p_file_area,p_file_area->file_area_state);
				}
				/*file_area移动到hot、mapcount、refault、free链表了，直接遍历下一个file_area*/
				goto get_next_file_area;
			}
		}
#endif	
		list_num_for_file_area = list_num_get(p_file_area);

		/*检测file_area的有效*/
        check_multi_level_warm_list_file_area_valid(p_hot_cold_file_global,p_file_stat,p_file_area,p_current_scan_file_stat_info,list_num_for_file_area,is_global_file_stat);
		

		/* 重大bug，如果p_file_area在下边被list_move到了其他链表，就不能在循环最后p_file_area = list_prev_entry(p_file_area,file_area_list)
		 * 获取下一个遍历的file_area了，因为已经跨链表了，犯了同样的问题。解决办法是最开始p_file_area_temp获取本链表要遍历的下一个file_area。
		 * 是否有必要，对file_area在链表的前一个file_area也做异常判断，进一步验证当前file_area是否合法??????????????????????????????????*/
		p_file_area_temp = list_prev_entry(p_file_area,file_area_list);

		/*如果file_area没有page，再把file_area移动到zero_page_file_area_list链表。ctags时有大量的这种文件，0个page的file_area*/
		if(is_global_file_stat && !file_area_have_page(p_file_area)){
			/*向global_file_stat的链表移动file_area，需要加个file_stat_in_global_base检测，否则会造成内存越界*/
			if(!file_stat_in_global_base(&p_file_stat->file_stat_base))
				panic("%s file_stat:0x%llx  file_area:0x%llx status:0x%x not in global\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

			p_global_file_stat = container_of(p_file_stat,struct global_file_stat,file_stat);
			/*访问计数必须清0，否则干预后续判断*/
			file_area_access_freq_clear(p_file_area);
			list_num_update(p_file_area,POS_ZERO_PAGE);
			list_move(&p_file_area->file_area_list,&p_global_file_stat->zero_page_file_area_list);
			p_hot_cold_file_global->memory_reclaim_info.scan_zero_page_file_area_count ++;
			goto get_next_file_area;
		}

		/* if成立说明file_area对应的文件被iput了，并把file_area移动到了global_file_stat_delete链表，这里不能
		 * list_del(&p_file_area->file_area_list)。因为将来遍历global_file_stat_delete链表上的file_area，也会
		 * list_del(&p_file_area->file_area_list)，那就从链表剔除file_area了*/

		/* 最新方案，iput()->find_get_entry_for_file_area()不再把file_area以file_area->file_area_delete移动到global_file_stat_delete
		 * 链表，而只是标记file_area的in_mapping_delete。异步内存回收线程看到file_area有in_maping_delete标记，再把file_area以
		 * file_area_list移动到global_file_stat_delete链表，此时没有并发问题。详细原因见find_get_entry_for_file_area()*/
		if(file_area_in_mapping_delete(p_file_area)){
			if(!is_global_file_stat)
				panic("%s file_stat:0x%llx  file_area:0x%llx status:0x%x not in global\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

			if(warm_list_printk)
				printk("%s file_stat:0x%llx  file_area:0x%llx status:0x%x in_mapping_delete\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

			p_hot_cold_file_global->memory_reclaim_info.scan_exit_file_area_count ++;
			if(is_cache_file)
				list_move(&p_file_area->file_area_list,&p_hot_cold_file_global->global_file_stat.file_area_delete_list);
			else
				list_move(&p_file_area->file_area_list,&p_hot_cold_file_global->global_mmap_file_stat.file_area_delete_list);

			//list_del(&p_file_area->file_area_list);
			goto get_next_file_area;
		}

		get_file_area_age(&p_file_stat->file_stat_base,p_file_area,p_hot_cold_file_global,file_stat_changed,FILE_STAT_NORMAL,is_global_file_stat);
		if(file_stat_changed)
			goto get_next_file_area;

		
		

		/*获取file_area前后两次扫到的时的访问次数*/
		access_freq = file_area_access_freq(p_file_area);
		read_file_area = file_area_in_read(p_file_area);
		age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;;

		
		/* cache文件只写的file_area直接移动到file_area_writeonly_or_cold链表，或者访问很频繁的，先不动?????????
		 * 还有一点，如果mmap文件解除所用page的mmap映射后，该文件不会转成cache文件。之后这些file_area就需要
		 * 大量转成writeonly file_area。并且，mmap文件里也有cache wrtiteonly的file_area。以上3中情况，目前都没处理???????*/
		/*现在发现mysql测试时，有很多refult page竟然是这里直接把file_area移动到writeonly链表导致的。于是要做限制，如果
		 *是writeonly文件，则直接把file_area移动到writeonly链表，但是非writeonly文件，如果file_area没有in_read标记，这些
		 *file_area可能只是分配了但是没有读写，导致access_freq是0*/
		if(POS_WIITEONLY_OR_COLD != list_num_for_file_area){
			if(file_stat_in_writeonly_base(p_file_stat_base) || (is_cache_file && !read_file_area /*&& access_freq > 0*/)){
								
				if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
					printk("1:%s file_stat:0x%llx file_area:0x%llx state:0x%x access_freq:%d index:%d >>> writeonly\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,access_freq,p_file_area->start_index);

				/*跨链表移动必须对file_area_access_freq清0，否则导致在新的链表也被判定位热file_area*/
				file_area_access_freq_clear(p_file_area);
				list_num_update(p_file_area,POS_WIITEONLY_OR_COLD);
				list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_writeonly_or_cold);

				p_hot_cold_file_global->memory_reclaim_info.warm_list_file_area_to_writeonly_list_count ++;
				goto get_next_file_area;
			}
		}else{
			/*如果是writeonly的file_area要单独处理。如果是writeonly的file_area，直接跳过，永远停留在writeonly链表。
		      如果是read属性的file_area，就要根据access_freq决定把file_area移动到上一层的链表了*/
            if(!read_file_area)
				goto get_next_file_area;
		}

		/* 1:access_freq是0没有被访问。非长长时间每访问的直接移动到file_area_writeonly_or_cold链表，否则只是移动到下一级链表
		 * 2:access_freq很大，但很长时间没访问，这样情况完全有可能，比如处于warm_hot链表的file_area前几分钟被多次访问，但是之后
		 * 就再也没有访问过了。这种情况也要file_area降级处理或者直接移动到下一级的链表，甚至是writeonly链表。阈值160可以基于file_area的属性动态调整*/
		if(0 == access_freq || age_dx > p_mult_warm_list_age_dx->file_area_cold_level){
			//unsigned int age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;

			/* 如果是当前遍历的是warm链表上的file_area，且这些file_area因长时间没访问而移动到下一级的链表，这些file_area如果有in_temp属性则清理掉
			 * 不用清理了，现在去掉了file_area的in_temp bit，用没有hot、mapcount、refault、free bit取代，就是全0，而新分配的file_area这些bit都是0，
			 * 故不用再专门清理file_area的in_temp*/
			/*if(POS_WARM == p_current_scan_file_stat_info->traverse_list_num && file_area_in_temp_list(p_file_area)){
				clear_file_area_in_temp_list(p_file_area);
				if(get_file_area_list_status(p_file_area) != 0)
					panic("%s file_stat:0x%llx  status:0x%x file_area:0x%llx state:0x%x file_area_num:%d  traverse_list_num:%d age:%u %u file_area status error\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_base.file_stat_status,(u64)p_file_area,p_file_area->file_area_state,list_num_for_file_area,p_current_scan_file_stat_info->traverse_list_num,p_file_area->file_area_age,p_hot_cold_file_global->global_age);
			}*/

			/* 这里的处理非常关键，如果in_read的属性的file_area，如果长时间没访问就要清理掉in_read属性。
			 * 1:根据内存紧张程度调整age_dx_level，内存越紧张age_dx_level越小
			 * 2:热度越高的warm链表上的file_area，age_dx_level越大
			 * 3:mmap的file_area，age_dx_level要调大一倍，甚至更高*/
			 
			 /* cache文件的in_read属性的file_area，age_dx_level越大。错了，这个不用判断，因为cache文件的in_read属性的file_area不会走到
			  * 这个分支，而是走上边的if分支*/
            /*if(read_file_area)
                age_dx_level *= 2;*/
		
			if(POS_WIITEONLY_OR_COLD == list_num_for_file_area)
				goto get_next_file_area;


			if(age_dx > p_mult_warm_list_age_dx->to_writeonly_cold_list_age_dx){

				/*如果in_read的属性的file_area，如果长时间没访问就要清理掉in_read属性。这里的处理非常关键，否则会跟干扰后续的内存回收*/
				if(read_file_area)
					clear_file_area_page_read(p_file_area);

				/*if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
				  printk("2:%s file_stat:0x%llx file_area:0x%llx state:0x%x age_dx_level:%d >>> writeonly\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,age_dx_level);
				  list_num_update(p_file_area,POS_WIITEONLY_OR_COLD);
				  list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_writeonly_or_cold);*/
				access_freq_solve_for_writeonly_or_cold_file_area(p_current_scan_file_stat_info,p_file_stat_base,p_file_area);
				p_hot_cold_file_global->memory_reclaim_info.warm_list_file_area_to_writeonly_list_count_cold ++;
			}
			/*如果file_area处最低级的writeonly_or_cold链表，p_down_file_area_list_head是NULL，就不再向下移动这种file_area了*/
			else if(age_dx > p_mult_warm_list_age_dx->to_down_list_age_dx && p_current_scan_file_stat_info->p_down_file_area_list_head){
				/*if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
				  printk("3:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> down_list_num:%d\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,p_current_scan_file_stat_info->down_list_num);

				  list_num_update(p_file_area,p_current_scan_file_stat_info->down_list_num);
				  list_move(&p_file_area->file_area_list,p_current_scan_file_stat_info->p_down_file_area_list_head);*/
				access_freq_solve_for_down_file_area(p_current_scan_file_stat_info,p_file_stat_base,p_file_area);
				p_hot_cold_file_global->memory_reclaim_info.warm_list_file_area_down_count ++;
				if(is_cache_file)
					p_hot_cold_file_global->cache_file_warm_list_file_area_down_count ++;
				else
					p_hot_cold_file_global->mmap_file_warm_list_file_area_down_count ++;
			}
		}else{

			p_hot_cold_file_global->memory_reclaim_info.warm_list_file_area_up_count ++;

			/* mmap文件的file_area的access_freq，page被访问时只会是1，因为只有上边get_file_area_age时才会探测该file_area，
			 * 如果page被访问了，则令file_area的access_freq赋值1，因此只要访问了，就移动到up上一级的链表，如此会令mmap文件
			 * 页更不容易被回收，降低refault.mmap文件的file_area也会被read/write访问令access_freq很大，这个忽略*/
			if(!is_cache_file){
				p_hot_cold_file_global->mmap_file_warm_list_file_area_up_count ++;
				access_freq_solve_for_up_file_area(p_current_scan_file_stat_info,p_file_stat_base,p_file_area,0);
				goto get_next_file_area;
			}

			p_hot_cold_file_global->cache_file_warm_list_file_area_up_count ++;
			switch(access_freq){
				case 1:
					access_freq_solve_for_tmp_file_area(p_current_scan_file_stat_info,p_file_stat_base,p_file_area);
					break;
					/* freq次数没有达到升级到hot file_area的阈值5，但又比2大，于是把file_area->access_freq更新到1，在移动到上一级up链表。
					 * 如果后续再被访问一次，该file_area就能继续升到更高一级的链表*/
				case 2:
					access_freq_solve_for_up_file_area(p_current_scan_file_stat_info,p_file_stat_base,p_file_area,0);
					break;
				case 3:
					access_freq_solve_for_up_file_area(p_current_scan_file_stat_info,p_file_stat_base,p_file_area,1);
					break;
				case 4:
					access_freq_solve_for_up_file_area(p_current_scan_file_stat_info,p_file_stat_base,p_file_area,1);
					break;
				case 5:
				default:
					access_freq_solve_for_hot_file_area(p_hot_cold_file_global,p_current_scan_file_stat_info,p_file_stat,p_file_area,is_global_file_stat);
					break;
			}
#if 0
			else if(1 == access_freq && is_cache_file){/*只访问一次不温不热的file_area则移动到链表头*/
				if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
					printk("4:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> tmp\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

				list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->temp_head);
			}else if(access_freq > 5){/*访问次数频繁，直接移动到热file_stat->hot链表*/
				/*跨链表移动必须对file_area_access_freq清0，否则导致在新的链表也被判定位热file_area*/
				file_area_access_freq_clear(p_file_area);

				set_file_area_in_hot_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_current_scan_file_stat_info->p_traverse_file_stat->file_area_hot);
				p_file_stat->file_area_hot_count ++;
				if(file_stat_in_test_base(p_file_stat_base)|| is_global_file_stat_file_in_debug(p_file_area->mapping))
					printk("5:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> hot\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

				//mmap文件热file_area太多则移动到global hot_file_stat链表
				check_hot_file_stat_and_move_global(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_stat,is_global_file_stat);
			}else{/*被多次访问，但不是太多则把file_area移动到上一级更热的file_stat链表*/
				if(file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(p_file_area->mapping))
					printk("6:%s file_stat:0x%llx file_area:0x%llx state:0x%x >>> up_list_num:%d\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,p_current_scan_file_stat_info->up_list_num);

				/*跨链表移动必须对file_area_access_freq清0，否则导致在新的链表也被判定位热file_area*/
				file_area_access_freq_clear(p_file_area);
				/*如果向上移动的warm_list是file_area_hot链表，则要设置file_area的in_hot标记，否则list_num_update*/
				if(p_current_scan_file_stat_info->p_up_file_area_list_head == &p_current_scan_file_stat_info->p_traverse_file_stat->file_area_hot){
					//if(file_area_in_temp_list(p_file_area))
					//	clear_file_area_in_temp_list(p_file_area);
					set_file_area_in_hot_list(p_file_area);
				}else	
					list_num_update(p_file_area,p_current_scan_file_stat_info->up_list_num);

				list_move(&p_file_area->file_area_list,p_current_scan_file_stat_info->p_up_file_area_list_head);
			}
#endif			
		}

get_next_file_area:
		/*接着遍历前一个file_area*/
		//p_file_area = list_prev_entry(p_file_area,file_area_list);
		p_file_area = p_file_area_temp;
		scan_file_area_count ++;
	}

	/* 当前链表上的file_area都遍历完了，要做几个重点工作
	 * 1：把暂存在current_scan_file_stat_info->temp_head不温不热的file_area移动到p_traverse_file_area_list_head链表头，
	 * 这样链表尾的都是没访问的过file_area，链表头的file_area都是访问过但不热的file_area，相当于按照file_area冷热程度
	 * 聚集起来了。如果遇到内存紧缺，直接从链表头遍历冷file_area进行内存回收，效率很高
	 * 2：针对普通文件file_stat，确定更新下一次遍历该文件的哪个warm链表，并且要决定是否要遍历下一个文件file_stat。什么意思，
	 * 如果file_stat->warm遍历到了很多的冷file_area，那就继续遍历file_stat->warm_hot的file_area。否则说明该文件可能
	 * 大部分file_area都是热的，不再遍历了，直接遍历下一个文件
	 * 针对global_file_stat，直接下一次遍历哪个warm链表即可，因为它只有一个
	 * 3：如果是只写文件，现在设定只扫描少量的file_area就退出循环，而不是扫描的file_area个数超过的原始max再结束。
	 * 因此 到这里只写的文件也要执行update_file_stat_next_multi_level_warm_or_writeonly_list()清理掉current_scan_file_stat_info
	 * */
	if(list_entry_is_head(p_file_area, p_current_scan_file_stat_info->p_traverse_file_area_list_head, file_area_list) || 
			file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base)){
		/*if(!list_empty(&p_current_scan_file_stat_info->temp_head)){
			list_splice_init(&p_current_scan_file_stat_info->temp_head,p_current_scan_file_stat_info->p_traverse_file_area_list_head);
		}*/

		if(warm_list_printk)
			printk("%s file_stat:0x%llx traverse_list_num:%d is_global_file_stat:%d is_cache_file:%d scan_file_area_count:%d scan_file_area_max:%d traverse ok\n",__func__,(u64)p_current_scan_file_stat_info->p_traverse_file_stat,p_current_scan_file_stat_info->traverse_list_num,is_global_file_stat,is_cache_file,scan_file_area_count,scan_file_area_max);

		if(is_global_file_stat){
			struct global_file_stat *p_global_file_stat = container_of(p_file_stat,struct global_file_stat,file_stat);
			update_global_file_stat_next_multi_level_warm_or_writeonly_list(p_current_scan_file_stat_info,p_global_file_stat);
		}
		else{
			/*普通文件的某个warm链表file_area遍历完后，必须把p_current_scan_file_stat_info->p_traverse_file_stat设置NULL，
			 * 然后下个周期遍历时，发现它是NULL，才会遍历的新的文件file_stat*/
			//p_current_scan_file_stat_info->p_traverse_file_stat = NULL;
			//p_current_scan_file_stat_info->p_traverse_file_area_list_head = NULL;
			update_file_stat_next_multi_level_warm_or_writeonly_list(p_current_scan_file_stat_info,p_file_stat);
		}
	}else{
		/* 否则说明遍历的file_area超过max导致的遍历结束，则把查询到的最后一个但还没遍历的file_area保存到
		 * p_current_scan_file_stat_info->p_traverse_first_file_area，下轮循环继续遍历*/

		/* 又一个SB的设计，如果上边循环最后遍历的p_file_area被移动到p_current_scan_file_stat_info->p_up_file_area_list_head、
		 * p_current_scan_file_stat_info->p_down_file_area_list_head、hot、writeonly_or_cold等链表，简单说这个file_area被
		 * 移动到另外的链表了。然后下次继续遍历该file_stat的file_area时，就不能从这个p_file_area开始遍历了，因为它已经被
		 * 移动到其他链表了！因此，必须要确保赋值给p_current_scan_file_stat_info->p_traverse_first_file_area的file_area，
		 * 没有被移动到其他链表，怎么保证呢？把p_file_area_temp赋值给p_traverse_first_file_area即可。因为p_file_area_temp
		 * 是p_file_area在遍历的链表里的前一个file_area。当因为遍历的file_area个数超过max导致的遍历结束，执行到这里时，
		 * p_file_area_temp这个file_area能确保是没有遍历过的。不对，上边的循环最后，现有p_file_area=p_file_area_temp，
		 * 然后才会有遍历的file_area个数超过max导致的遍历结束，执行到这里时，p_file_area和p_file_area_temp是同一个。
		 * 因此啥都不用改*/
		p_current_scan_file_stat_info->p_traverse_first_file_area = p_file_area;
		//p_current_scan_file_stat_info->p_traverse_first_file_area = p_file_area_temp;


		if(warm_list_printk)
			printk("%s file_stat:0x%llx traverse_list_num:%d is_global_file_stat:%d is_cache_file:%d scan_file_area_count:%d scan_file_area_max:%d not traverse ok\n",__func__,(u64)p_current_scan_file_stat_info->p_traverse_file_stat,p_current_scan_file_stat_info->traverse_list_num,is_global_file_stat,is_cache_file,scan_file_area_count,scan_file_area_max);
	}

	p_hot_cold_file_global->scan_exit_file_area_count += p_hot_cold_file_global->memory_reclaim_info.scan_exit_file_area_count;
	p_hot_cold_file_global->scan_zero_page_file_area_count += p_hot_cold_file_global->memory_reclaim_info.scan_zero_page_file_area_count;
	p_hot_cold_file_global->warm_list_file_area_up_count += p_hot_cold_file_global->memory_reclaim_info.warm_list_file_area_up_count;
	p_hot_cold_file_global->warm_list_file_area_to_writeonly_list_count += p_hot_cold_file_global->memory_reclaim_info.warm_list_file_area_to_writeonly_list_count;
	p_hot_cold_file_global->warm_list_file_area_to_writeonly_list_count_cold += p_hot_cold_file_global->memory_reclaim_info.warm_list_file_area_to_writeonly_list_count_cold;

	return scan_file_area_count;
}

static unsigned int direct_recliam_file_area_for_global_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct global_file_stat *p_global_file_stat,struct current_scan_file_stat_info *p_current_scan_file_stat_info,unsigned int scan_file_area_max_for_memory_reclaim)
{
	unsigned int free_pages = 0;
	unsigned int scan_file_area_count = 0;
	LIST_HEAD(file_area_real_free);
	LIST_HEAD(file_area_warm_list);
	//struct file_stat *p_file_stat;
	struct file_stat_base *p_file_stat_base = &p_global_file_stat->file_stat.file_stat_base;
	struct file_stat *p_file_stat = &p_global_file_stat->file_stat;

#if 0
	/*file_stat必须是normal文件，不能处于tiny_small_one_area、tiny_small、small文件链表*/
	if(file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base) || 
			file_stat_in_file_stat_small_file_head_list_base(p_file_stat_base) || 
			file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base)){
		panic("%s file_stat:0x%llx status:0x%x error\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
	}

	p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
#endif	
	/*每次内存回收前先对free_pages_count清0*/
	p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count = 0;

	/* 如果global_file_stat正在遍历file_area_writeonly_or_cold或file_area_warm_cold链表上的file_area，p_traverse_file_area_list_head
	 * 指向它们，下边cold_file_isolate_lru_pages_and_shrink()会list_move p_traverse_first_file_area 指向的file_area到其他链表，也会
	 * list_move p_traverse_file_area_list_head指向的链表上的file_area到其他链表，此时必须把p_traverse_first_file_area设置NULL。
	 * 下个周期从p_traverse_file_area_list_head指向的链表尾开始遍历，不能从p_traverse_first_file_area指向的file_area开始遍历，因为
	 * 这个file_area已经从被移动到其他链表了，不属于p_traverse_file_area_list_head指向的链表了
	 * 举个例子，本轮内存回收周期，遍历了global_file_stat的writeonly链表上的file_area，因遍历的file_area个数超过max而导致
	 * 遍历结束。此时p_traverse_first_file_area指向global_file_stat->writeonly链表尾的file_area，p_traverse_file_area_list_head
	 * 指向global_file_stat->writeonly链表头。因为内存紧张而执行该函数回收global_file_stat的writeonly和warm_cold链表上的
	 * file_area的page，回收过后把这些file_area移动到global_file_stat的file_area_free链表。下个周期，继续遍历global_file_stat
	 * 的writeonly链表尾的file_area，因为p_traverse_first_file_area不为NULL，于是从p_traverse_first_file_area指向的file_area
	 * 向前遍历，而这个file_area此时处于global_file_stat->file_area_free链表。实际情况此时遍历的就是该链表上的file_area，
	 * 则这些file_area来自warm或warm_cold链表，或者warm_list_num是0，而当前遍历的是global_file_stat->writeonly链表的file_area，
	 * 自然会因为遍历的file_area的编号warm_list_num跟当前遍历的writeonly链表号不一致而crash。解决办法很简单，
	 * 及时把p_traverse_first_file_area清NULL，下个周期直接从writeonly链表尾取file_area即可。
	 * 有一点需要注意，现在将writeonly或warm_cold链表的file_area移动到file_area_free链表时，还有file_area移动到hot链表，
	 * 都没有清理掉该file_area的warm_list_num，这就导致一旦原本遍历warm的各个file_area链表时，错误遍历到hot或者free链表
	 * ，因为没有清理掉file_area老的warm_list_num，错误以为当前遍历的file_area的warm_list_num编号跟当前遍历的warm链表一致
	 * 不能立即panic。我认为有必要warm等链表的file_area移动到hot或free链表时，必须清理掉file_area的warm_list_num为-1。还有
	 * 一点，当前还有一个crash:遍历writeonly文件warm链表的file_area时，正常file_area的warm_list_num是0，现在链表尾的file_area
	 * 却是1，1是writeonly链表编号，于是出发panic。目前根本原因还无法确定，但有可能是同一个问题。
	 */
	if(p_current_scan_file_stat_info->p_traverse_file_area_list_head == &p_global_file_stat->file_stat.file_area_writeonly_or_cold){
		//p_current_scan_file_stat_info->p_traverse_file_stat = NULL;
		//p_current_scan_file_stat_info->p_traverse_file_area_list_head = NULL;
		p_current_scan_file_stat_info->p_traverse_first_file_area = NULL;
		//p_current_scan_file_stat_info->p_up_file_area_list_head = NULL;
		//p_current_scan_file_stat_info->p_down_file_area_list_head = NULL;
	}
	scan_file_area_count = cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,&p_global_file_stat->file_stat.file_area_writeonly_or_cold,-1,&file_area_real_free,0,&file_area_warm_list,&p_hot_cold_file_global->memory_reclaim_info.memory_reclaim_info_writeonly_list);

	/* 如果当前内存很紧张，并且file_area_writeonly_or_cold链表回收的page太少，再回收file_area_warm_cold链表上的file_area，容易refault。
	 * 再加一个限制，不回收mmap文件的file_stat->warm_cold链表上的file_area，因为这导致so等mmap容易refault*/
	if(file_stat_in_cache_file_base(p_file_stat_base) && IS_IN_MEMORY_EMERGENCY_RECLAIM(p_hot_cold_file_global) && 
			p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count <= 64 && p_hot_cold_file_global->memory_still_memrgency_after_reclaim){
		
		if(p_current_scan_file_stat_info->p_traverse_file_area_list_head == &p_file_stat->file_area_warm_cold)
			p_current_scan_file_stat_info->p_traverse_first_file_area = NULL;

		/*memory_still_memrgency_after_reclaim是1至少说明已发现一次异步内存回收后，内存依然紧张*/
		scan_file_area_count += cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,&p_file_stat->file_area_warm_cold,-1,&file_area_real_free,0,&file_area_warm_list,&p_hot_cold_file_global->memory_reclaim_info.memory_reclaim_info_warm_cold_list);

        /*memory_still_memrgency_after_reclaim是2至少说明已发现2次异步内存回收后，内存依然紧张*/
		if(2 == p_hot_cold_file_global->memory_still_memrgency_after_reclaim){
		if(p_current_scan_file_stat_info->p_traverse_file_area_list_head == &p_global_file_stat->file_area_warm_middle)
			p_current_scan_file_stat_info->p_traverse_first_file_area = NULL;

		    scan_file_area_count += cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,&p_global_file_stat->file_area_warm_middle,-1,&file_area_real_free,0,&file_area_warm_list,&p_hot_cold_file_global->memory_reclaim_info.memory_reclaim_info_warm_middle_list);
		}
        /*memory_still_memrgency_after_reclaim大于3至少说明已发现3次异步内存回收后，内存依然紧张*/
		else if(p_hot_cold_file_global->memory_still_memrgency_after_reclaim >= 3){
		if(p_current_scan_file_stat_info->p_traverse_file_area_list_head == &p_file_stat->file_area_warm)
			p_current_scan_file_stat_info->p_traverse_first_file_area = NULL;

		    scan_file_area_count += cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,&p_file_stat->file_area_warm,-1,&file_area_real_free,0,&file_area_warm_list,&p_hot_cold_file_global->memory_reclaim_info.memory_reclaim_info_warm_list);
		}
	}

	/*参与内存回收的file_area都移动到file_stat->file_area_free链表，要不要清理掉file_area->warm_list_num呢？不用，将来移动回warm链表后，会重新设置file_area的warm_list_num*/
	//list_splice_init(&p_global_file_stat->file_stat.file_area_writeonly_or_cold,&p_global_file_stat->file_stat.file_area_free);
	//list_splice_init(&p_global_file_stat->file_area_warm_cold,&p_global_file_stat->file_stat.file_area_free);
	list_splice_init(&file_area_real_free,&p_global_file_stat->file_stat.file_area_free);

	/*在参与内存回收过程，发现访问过的file_area再移动回file_area_warm链表*/
	list_splice_init(&file_area_warm_list,&p_global_file_stat->file_stat.file_area_warm);

	free_pages = p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;
	//p_file_stat->reclaim_pages_last_period = free_pages;
	p_global_file_stat->file_stat.reclaim_pages += free_pages;
	all_file_stat_reclaim_pages_counter(p_hot_cold_file_global,p_file_stat_base,1,free_pages);

	//隔离的page个数
	//p_hot_cold_file_global->hot_cold_file_shrink_counter.isolate_lru_pages += isolate_lru_pages;
	//从系统启动到目前释放的page个数
	if(file_stat_in_cache_file_base(p_file_stat_base))
		p_hot_cold_file_global->free_pages += p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;
	else
		p_hot_cold_file_global->free_mmap_pages += p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;

	if(warm_list_printk)
		printk("%s file_stat:0x%llx recliam_pages:%d\n",__func__,(u64)p_file_stat_base,free_pages);

	return free_pages;

}
static unsigned int direct_recliam_file_area_for_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,struct current_scan_file_stat_info *p_current_scan_file_stat_info,unsigned int scan_file_area_max_for_memory_reclaim)
{
	unsigned int free_pages = 0;
	unsigned int scan_file_area_count = 0;
	struct file_stat_base *p_file_stat_base = &p_file_stat->file_stat_base;
	LIST_HEAD(file_area_real_free);
	LIST_HEAD(file_area_warm_list);

#if 0
	/*file_stat必须是normal文件，不能处于tiny_small_one_area、tiny_small、small文件链表*/
	if(file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base) || 
			file_stat_in_file_stat_small_file_head_list_base(p_file_stat_base) || 
			file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base)){
		panic("%s file_stat:0x%llx status:0x%x error\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
	}

	p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
#endif	
	/*每次内存回收前先对free_pages_count清0*/
	p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count = 0;

	/*原理direct_recliam_file_area_for_global_file_stat()有说*/
	if(p_current_scan_file_stat_info->p_traverse_file_area_list_head == &p_file_stat->file_area_writeonly_or_cold)
		p_current_scan_file_stat_info->p_traverse_first_file_area = NULL;
	
	scan_file_area_count = cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,&p_file_stat->file_area_writeonly_or_cold,scan_file_area_max_for_memory_reclaim,&file_area_real_free,0,&file_area_warm_list,&p_hot_cold_file_global->memory_reclaim_info.memory_reclaim_info_writeonly_list);
	
	/*内存紧张模式，又是只写文件，直接从file_stat->warm链表回收page*/
	if(file_stat_in_writeonly_base(p_file_stat_base) && IS_IN_MEMORY_EMERGENCY_RECLAIM(p_hot_cold_file_global))
		goto direct_reclaim_from_writeonly_file_warm_list;

	/* 如果当前内存很紧张，并且file_area_writeonly_or_cold链表回收的page太少，再回收file_area_warm_cold链表上的file_area，容易refault。
	 * 但是如果前一轮内存紧张而内存回收，依然内存紧张，memory_still_memrgency_after_reclaim置1，就得回收file_area_warm_cold链表上的file_area了*/
	if(file_stat_in_cache_file_base(p_file_stat_base) && IS_IN_MEMORY_EMERGENCY_RECLAIM(p_hot_cold_file_global) && p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count <= 64 && p_hot_cold_file_global->memory_still_memrgency_after_reclaim){

		if(p_current_scan_file_stat_info->p_traverse_file_area_list_head == &p_file_stat->file_area_warm_cold)
			p_current_scan_file_stat_info->p_traverse_first_file_area = NULL;

		/*memory_still_memrgency_after_reclaim是1至少说明已发现一次异步内存回收后，内存依然紧张*/
		scan_file_area_count += cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,&p_file_stat->file_area_warm_cold,-1,&file_area_real_free,0,&file_area_warm_list,&p_hot_cold_file_global->memory_reclaim_info.memory_reclaim_info_warm_cold_list);

		/* memory_still_memrgency_after_reclaim大于1，说明内存紧张模式，回收内存后依然内存紧张。这种情况已经持续
		 * 至少2次，那只能回收file_stat->warm链表上的page了。如果是writeonly文件，大量file_area可能存在于file_stat->warm链表，直接回收。
		 * 算了，最后决定针对writeonly文件，直接list_splice_init把filie_stat->warm、temp链表上的file_area移动到file_stat->writeonly链表了
		 * 这样根本不行，因为file_area移动到file_stat->writeonly链表，还得设置file_area的list_num为POS_WIITEONLY_OR_COLD，
		 * list_splice_init移动链表就做不到这点了！再加个限制，该文件的热file_area不能太多，回收这种文件的file_stat->warm的file_area，
		 * refault概率比较大*/
		if(p_hot_cold_file_global->memory_still_memrgency_after_reclaim >= 3 && 
				(/*!is_file_stat_may_hot_file(p_file_stat)&&*/ p_hot_cold_file_global->global_age - p_file_stat_base->recent_access_age >= 3)){
direct_reclaim_from_writeonly_file_warm_list:

			if(p_current_scan_file_stat_info->p_traverse_file_area_list_head == &p_file_stat->file_area_warm)
				p_current_scan_file_stat_info->p_traverse_first_file_area = NULL;

			scan_file_area_count += cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,&p_file_stat->file_area_warm,-1,&file_area_real_free,0,&file_area_warm_list,&p_hot_cold_file_global->memory_reclaim_info.memory_reclaim_info_warm_list);
		}
	}

	/*参与内存回收的file_area都移动到file_stat->file_area_free链表，要不要清理掉file_area->warm_list_num呢？不用，将来移动回warm链表后，会重新设置file_area的warm_list_num*/
	list_splice_init(&file_area_real_free,&p_file_stat->file_area_free);

	/*在参与内存回收过程，发现访问过的file_area再移动回file_area_warm链表*/
	list_splice_init(&file_area_warm_list,&p_file_stat->file_area_warm);

	free_pages = p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;
	//p_file_stat->reclaim_pages_last_period = free_pages;
	p_file_stat->reclaim_pages += free_pages;
	all_file_stat_reclaim_pages_counter(p_hot_cold_file_global,p_file_stat_base,0,free_pages);

	//隔离的page个数
	//p_hot_cold_file_global->hot_cold_file_shrink_counter.isolate_lru_pages += isolate_lru_pages;
	//从系统启动到目前释放的page个数
	if(file_stat_in_cache_file_base(p_file_stat_base))
		p_hot_cold_file_global->free_pages += p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;
	else
		p_hot_cold_file_global->free_mmap_pages += p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;

	if(warm_list_printk)
		printk("%s file_stat:0x%llx recliam_pages:%d\n",__func__,(u64)p_file_stat_base,free_pages);

	return free_pages;
}
#if 0
static unsigned int global_file_stat_file_area_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *global_file_area_delete_list_head,struct current_scan_file_stat_info *p_current_scan_file_stat_info)
{
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int delete_file_area_count = 0;

	/* 注意，此时释放的是global_file_stat.file_area_delete_list等链表上的file_area，这些file_area都是iput()过程，从xrray tree
	 * 剔除file_area后把file_area移动到该链表的。然后该函数遍历该链表释放该file_area，此时不能执行cold_file_area_delete()
	 * 释放该file_area，因为file_area已经从xrray tree剔除了*/

	/* 大傻逼，无语了，global_file_stat_delete链表上的file_area是按照其file_area_delete成员，添加到global_file_stat_delete
	 * 链表的。只有file_stat->temp、warm、refaut等链表上的file_area才是按照其file_area_list成员添加到file_stat->temp、warm、refaut
	 * 等链表的。我TM是直接把list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,global_file_area_delete_list_head,file_area_list)
	 * 的代码复制过来，没有深度思考*，犯了低级的错误。这就导致这里遍历global_file_stat_delete链表上的file_area，是按照file_area
	 * 的file_area_list的成员，container_of(p_file_area，struct file_area,file_area_list)得到file_area指针，正确需要是
	 * container_of(p_file_area，struct file_area,file_area_delete)得到file_area指针。因此，这里得到的file_area指针就是错误的
	 * ，因为指向了错误的内存地址，下边list_del(&p_file_area->file_area_delete)和cold_file_area_delete_quick()里会对file_area
	 * 进行各种赋值，很容易内存越界，出现不可预期的错误。比如，就出现下边list_del(&p_file_area->file_area_delete)时，出现
	 * "list_del corruption. prev->next should be ffff9abaefd09b80, but was ffff9abaefd09708"错误而crash，原因是
	 * global_file_stat_delete的链表，即hot_cold_file_global->global_file_stat.file_area_delete_list_temp链表头，竟然有
	 * 两个file_area->prev指向这个链表头，导致该链表上的file_area错乱，出现上边的报错*/
	//list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,global_file_area_delete_list_head,file_area_list){
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,global_file_area_delete_list_head,file_area_delete){
		/* cold_file_area_delete_quick()只会list_del_rcu(&p_file_area->file_area_list)把file_area从file_stat->temp、hot等链表剔除
		 * 但是这里file_area是通过其file_area_delete成员挂入global_file_area_delete_list_head链表的，因此还要专门
		 * list_del(&p_file_area->file_area_delete)。并且还要放到cold_file_area_delete_quick前边，因为在
		 * cold_file_area_delete_quick里，还会把把p_file_area->file_area_delete.prev 和next设置为NULL，表示已释放。
		 * 如果重复释放该file_area则触发crash。不用，list_del()本身会检测file_area重复删除,都可以*/
		list_del(&p_file_area->file_area_delete);

		/* 重大隐藏很深的bug，当异步内存回收线程在traverse_file_stat_multi_level_warm_list()函数里遍历file_area超过max而结束遍历，
		 * 把下一次要遍历的file_area保存到p_current_scan_file_stat_info->p_traverse_first_file_area。结果这个file_area被iput()
		 * 移动到global_file_area_delete_list链表，然后在这个函数里，异步内存回收线程把这个file_area释放掉掉。下一个内存回收周期
		 * ，traverse_file_stat_multi_level_warm_list()函数里，继续从p_current_scan_file_stat_info->p_traverse_first_file_area
		 * 取出file_area接着遍历，但是这个file_area已经被释放了，此时再使用这个file_area就是非法内存访问。隐藏的坑真的多！
		 * 还好我在traverse_file_stat_multi_level_warm_list()函数里，对每一个file_area都做了严格的限制，状态不对就panic。解决
		 * 办法很简单，这里检测到要释放的file_area是p_current_scan_file_stat_info->p_traverse_first_file_area，清NULL即可。但是
		 * 该如何traverse_file_stat_multi_level_warm_list()函数里，对file_area做更加严格的限制呢？我觉得，现在只对p_file_area
		 * 做了各种限制判断，还要对链表上的next、prev的file_area，做更加严格的限制?????????????????????????????????????*/
		if(p_current_scan_file_stat_info->p_traverse_first_file_area == p_file_area){
			p_current_scan_file_stat_info->p_traverse_first_file_area = NULL;
			printk("%s file_stat:0x%llx  status:0x%x file_area:0x%llx state:0x%x is p_traverse_first_file_area\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state);
		}
		cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_base,p_file_area);
		delete_file_area_count ++;
	}
	return delete_file_area_count;
}
#endif
static unsigned int global_file_stat_file_area_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *global_file_area_delete_list_head,struct current_scan_file_stat_info *p_current_scan_file_stat_info)
{
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int delete_file_area_count = 0;

	/* 注意，此时释放的是global_file_stat.file_area_delete_list等链表上的file_area，这些file_area都是iput()过程，从xrray tree
	 * 剔除file_area后把file_area移动到该链表的。然后该函数遍历该链表释放该file_area，此时不能执行cold_file_area_delete()
	 * 释放该file_area，因为file_area已经从xrray tree剔除了*/

	/* 大傻逼，无语了，global_file_stat_delete链表上的file_area是按照其file_area_delete成员，添加到global_file_stat_delete
	 * 链表的。只有file_stat->temp、warm、refaut等链表上的file_area才是按照其file_area_list成员添加到file_stat->temp、warm、refaut
	 * 等链表的。我TM是直接把list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,global_file_area_delete_list_head,file_area_list)
	 * 的代码复制过来，没有深度思考*，犯了低级的错误。这就导致这里遍历global_file_stat_delete链表上的file_area，是按照file_area
	 * 的file_area_list的成员，container_of(p_file_area，struct file_area,file_area_list)得到file_area指针，正确需要是
	 * container_of(p_file_area，struct file_area,file_area_delete)得到file_area指针。因此，这里得到的file_area指针就是错误的
	 * ，因为指向了错误的内存地址，下边list_del(&p_file_area->file_area_delete)和cold_file_area_delete_quick()里会对file_area
	 * 进行各种赋值，很容易内存越界，出现不可预期的错误。比如，就出现下边list_del(&p_file_area->file_area_delete)时，出现
	 * "list_del corruption. prev->next should be ffff9abaefd09b80, but was ffff9abaefd09708"错误而crash，原因是
	 * global_file_stat_delete的链表，即hot_cold_file_global->global_file_stat.file_area_delete_list_temp链表头，竟然有
	 * 两个file_area->prev指向这个链表头，导致该链表上的file_area错乱，出现上边的报错*/

	/* 最新方案，iput()->find_get_entry_for_file_area()不再把file_area以file_area->file_area_delete移动到global_file_stat_delete
	 * 链表，而只是标记file_area的in_mapping_delete。异步内存回收线程看到file_area有in_maping_delete标记，再把file_area以
	 * file_area_list移动到global_file_stat_delete链表，此时没有并发问题。详细原因见find_get_entry_for_file_area()*/
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,global_file_area_delete_list_head,file_area_list){
		/* cold_file_area_delete_quick()只会list_del_rcu(&p_file_area->file_area_list)把file_area从file_stat->temp、hot等链表剔除
		 * 但是这里file_area是通过其file_area_delete成员挂入global_file_area_delete_list_head链表的，因此还要专门
		 * list_del(&p_file_area->file_area_delete)。并且还要放到cold_file_area_delete_quick前边，因为在
		 * cold_file_area_delete_quick里，还会把把p_file_area->file_area_delete.prev 和next设置为NULL，表示已释放。
		 * 如果重复释放该file_area则触发crash。不用，list_del()本身会检测file_area重复删除,都可以*/
		//list_del(&p_file_area->file_area_delete);不再使用p_file_area->file_area_delete了

		/* 重大隐藏很深的bug，当异步内存回收线程在traverse_file_stat_multi_level_warm_list()函数里遍历file_area超过max而结束遍历，
		 * 把下一次要遍历的file_area保存到p_current_scan_file_stat_info->p_traverse_first_file_area。结果这个file_area被iput()
		 * 移动到global_file_area_delete_list链表，然后在这个函数里，异步内存回收线程把这个file_area释放掉掉。下一个内存回收周期
		 * ，traverse_file_stat_multi_level_warm_list()函数里，继续从p_current_scan_file_stat_info->p_traverse_first_file_area
		 * 取出file_area接着遍历，但是这个file_area已经被释放了，此时再使用这个file_area就是非法内存访问。隐藏的坑真的多！
		 * 还好我在traverse_file_stat_multi_level_warm_list()函数里，对每一个file_area都做了严格的限制，状态不对就panic。解决
		 * 办法很简单，这里检测到要释放的file_area是p_current_scan_file_stat_info->p_traverse_first_file_area，清NULL即可。但是
		 * 该如何traverse_file_stat_multi_level_warm_list()函数里，对file_area做更加严格的限制呢？我觉得，现在只对p_file_area
		 * 做了各种限制判断，还要对链表上的next、prev的file_area，做更加严格的限制?????????????????????????????????????*/
		if(p_current_scan_file_stat_info->p_traverse_first_file_area == p_file_area){
			p_current_scan_file_stat_info->p_traverse_first_file_area = NULL;
			printk("%s file_stat:0x%llx  status:0x%x file_area:0x%llx state:0x%x is p_traverse_first_file_area\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_area,p_file_area->file_area_state);
		}
		cold_file_area_delete_quick(p_hot_cold_file_global,p_file_stat_base,p_file_area);
		delete_file_area_count ++;
	}
	return delete_file_area_count;
}

static unsigned int global_file_stat_zero_page_file_area_list_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct global_file_stat *p_global_file_stat,struct list_head *global_zero_page_file_area_list_head,unsigned int scan_file_area_max)
{
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int scan_file_area_count = 0;
	char file_area_delete = 0;

	/*global_zero_page_file_area_list_head链表上的file_area*/
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,global_zero_page_file_area_list_head,file_area_list){

		if(++ scan_file_area_count > scan_file_area_max)
			break;

		if(list_num_get(p_file_area) != POS_ZERO_PAGE ||get_file_area_list_status(p_file_area) != 0 || file_area_in_deleted(p_file_area))
			panic("%s file_stat:0x%llx  status:0x%x file_area:0x%llx state:0x%x file_area status error\n",__func__,(u64)p_global_file_stat,p_global_file_stat->file_stat.file_stat_base.file_stat_status,(u64)p_file_area,p_file_area->file_area_state);

		if(file_area_delete)
			file_area_delete = 0;

		/*file_area的文件被iput()了*/
		if(file_area_in_mapping_delete(p_file_area)){
			printk("%s p_global_file_stat:0x%llx  file_area:0x%llx status:0x%x in_mapping_delete\n",__func__,(u64)p_global_file_stat,(u64)p_file_area,p_file_area->file_area_state);
			if(file_stat_in_cache_file_base(&p_global_file_stat->file_stat.file_stat_base))
				list_move(&p_file_area->file_area_list,&p_hot_cold_file_global->global_file_stat.file_area_delete_list);
			else
				list_move(&p_file_area->file_area_list,&p_hot_cold_file_global->global_mmap_file_stat.file_area_delete_list);

			continue;
		}

		if(file_area_have_page(p_file_area)){
			if(file_area_access_freq(p_file_area) < 2){
				file_area_access_freq_clear(p_file_area);			
				list_num_update(p_file_area,POS_WARM_MIDDLE);
				list_move(&p_file_area->file_area_list,&p_global_file_stat->file_area_warm_middle);
			}else{
				file_area_access_freq_clear(p_file_area);			
				list_num_update(p_file_area,POS_WARM_MIDDLE_HOT);
				list_move(&p_file_area->file_area_list,&p_global_file_stat->file_area_warm_middle_hot);
			}
		}
		/* 长时间没访问的file_area直接释放掉，这个file_area可能被iput移动到了global_file_area_delete_list_head链表，
		 * 不过已经在cold_file_area_delete_lock()函数做好了并发防护。并且iput操作的是file_area->file_area_delete，
		 * 不是file_area->file_area_list，因为不存在并发移动file_area需要加锁防护的问题*/
		else if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > 500){
			if(0 == cold_file_area_delete_lock(p_hot_cold_file_global,&p_global_file_stat->file_stat.file_stat_base,p_file_area))
				file_area_delete = 1;
		}
	}

	/* 如果是链表头，直接跳走。否则把当前file_area到链表尾的file_area全走移动到链表头，这些都是遍历过的file_area，下次直接从链表尾遍历新的file_area
	 * 但是如果file_area状态异常，不能操作 1:file_area在cold_file_area_delete_lock()函数里被释放了，则file_area->mapping是NULL 
	 * 2:file_area在上边被list_move到了其他链表。
	 * 这就够了吗，突然灵光一现，发现一个隐藏很深的的bug，如果最后一次遍历的file_area被释放了，那到这里file_area就有可能被释放了，
	 * 那就是无效内存，下边的p_file_area->mapping、list_num_get(p_file_area)就会出现非法数据，不能再使用了，此时只能靠file_area_delete表示file_area被释放了*/
	if(0 == file_area_delete &&
			p_file_area->mapping && list_num_get(p_file_area) == POS_ZERO_PAGE &&
			can_file_area_move_to_list_head_for_temp_list_file_area(p_file_area,global_zero_page_file_area_list_head))
		list_move_enhance(global_zero_page_file_area_list_head,&p_file_area->file_area_list);

	return scan_file_area_count;
}

static inline void update_normal_file_stat_current_scan_file_stat_info(struct current_scan_file_stat_info *p_current_scan_file_stat_info,struct file_stat *p_file_stat)
{
	if(file_stat_in_file_area_in_tmp_list_base(&p_file_stat->file_stat_base))
		panic("%s p_current_scan_file_stat_info:0x%llx p_file_stat:0x%llx error",__func__,(u64)p_current_scan_file_stat_info,(u64)p_file_stat);

	set_file_stat_in_file_area_in_tmp_list_base(&p_file_stat->file_stat_base);

	/*最新要遍历的file_stat赋值给p_current_scan_file_stat_info.p_file_stat*/
	p_current_scan_file_stat_info->p_traverse_file_stat = p_file_stat;
	/*得到要遍历的file_stat的warm或writeonly链表头*/
	update_current_scan_file_stat_info_for_list_head_and_num(p_current_scan_file_stat_info,p_file_stat);

	/*第一次执行到这里，p_file_stat->traverse_warm_list_num肯定是初始值0，就是 POS_WARM。后续该文件扫描过一个warm链表的file_area后，
	 * 会在把下一次扫描到该文件时，扫描的warm链表编号保存到p_file_stat->traverse_warm_list_num。等下一次扫描到文件file_stat，直接从
	 * p_file_stat->traverse_warm_list_num取出要扫描的warm链表编号*/
	p_current_scan_file_stat_info->traverse_list_num = p_file_stat->traverse_warm_list_num;
	/*如果要遍历的链表p_traverse_file_area_list_head为空，p_traverse_first_file_area就指向链表头了*/
	p_current_scan_file_stat_info->p_traverse_first_file_area = list_last_entry(p_current_scan_file_stat_info->p_traverse_file_area_list_head,struct file_area,file_area_list);
}
static inline void check_cache_file_current_scan_file_stat_info_invalid(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct current_scan_file_stat_info *p_current_scan_file_stat_info)
{
	unsigned long file_stat_type = p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;

	switch (file_stat_type){
		case 1 << F_file_stat_in_file_stat_temp_head_list:
            if(p_current_scan_file_stat_info != &p_hot_cold_file_global->current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_TEMP])
				panic("1:check_cache_file_current_scan_file_stat_info_invalid  0x%llx != 0x%llx\n",(u64)p_current_scan_file_stat_info,(u64)&p_hot_cold_file_global->current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_TEMP]);

			break;
		case 1 << F_file_stat_in_file_stat_middle_file_head_list:
            if(p_current_scan_file_stat_info != &p_hot_cold_file_global->current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_MIDDLE])
				panic("2:check_cache_file_current_scan_file_stat_info_invalid  0x%llx != 0x%llx\n",(u64)p_current_scan_file_stat_info,(u64)&p_hot_cold_file_global->current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_MIDDLE]);

			break;
		case 1 << F_file_stat_in_file_stat_large_file_head_list:
            if(p_current_scan_file_stat_info != &p_hot_cold_file_global->current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_LARGE])
				panic("3:check_cache_file_current_scan_file_stat_info_invalid  0x%llx != 0x%llx\n",(u64)p_current_scan_file_stat_info,(u64)&p_hot_cold_file_global->current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_LARGE]);

			break;
		case 1 << F_file_stat_in_file_stat_writeonly_file_head_list:
            if(p_current_scan_file_stat_info != &p_hot_cold_file_global->current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_WRITEONLY])
				panic("4:check_cache_file_current_scan_file_stat_info_invalid  0x%llx != 0x%llx\n",(u64)p_current_scan_file_stat_info,(u64)&p_hot_cold_file_global->current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_WRITEONLY]);

            break;
		default:
			if(file_stat_in_global_base(p_file_stat_base)){
				if(p_current_scan_file_stat_info != &p_hot_cold_file_global->global_file_stat.current_scan_file_stat_info)
				    panic("5:check_cache_file_current_scan_file_stat_info_invalid  0x%llx != 0x%llx\n",(u64)p_current_scan_file_stat_info,(u64)&p_hot_cold_file_global->global_file_stat.current_scan_file_stat_info);
            }
			else
				BUG();
	}
}
static inline void check_mmap_file_current_scan_file_stat_info_invalid(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct current_scan_file_stat_info *p_current_scan_file_stat_info)
{
	unsigned long file_stat_type = p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;

	switch (file_stat_type){
		case 1 << F_file_stat_in_file_stat_temp_head_list:
            if(p_current_scan_file_stat_info != &p_hot_cold_file_global->current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_TEMP])
				panic("1:check_mmap_file_current_scan_file_stat_info_invalid  0x%llx != 0x%llx\n",(u64)p_current_scan_file_stat_info,(u64)&p_hot_cold_file_global->current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_TEMP]);

			break;
		case 1 << F_file_stat_in_file_stat_middle_file_head_list:
            if(p_current_scan_file_stat_info != &p_hot_cold_file_global->current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_MIDDLE])
				panic("2:check_mmap_file_current_scan_file_stat_info_invalid  0x%llx != 0x%llx\n",(u64)p_current_scan_file_stat_info,(u64)&p_hot_cold_file_global->current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_MIDDLE]);

			break;
		case 1 << F_file_stat_in_file_stat_large_file_head_list:
            if(p_current_scan_file_stat_info != &p_hot_cold_file_global->current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_LARGE])
				panic("3:check_mmap_file_current_scan_file_stat_info_invalid  0x%llx != 0x%llx\n",(u64)p_current_scan_file_stat_info,(u64)&p_hot_cold_file_global->current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_LARGE]);

			break;
		case 1 << F_file_stat_in_file_stat_writeonly_file_head_list:
            if(p_current_scan_file_stat_info != &p_hot_cold_file_global->current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_WRITEONLY])
				panic("4:check_mmap_file_current_scan_file_stat_info_invalid  0x%llx != 0x%llx\n",(u64)p_current_scan_file_stat_info,(u64)&p_hot_cold_file_global->current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_WRITEONLY]);

            break;
		default:
			if(file_stat_in_global_base(p_file_stat_base)){
				if(p_current_scan_file_stat_info != &p_hot_cold_file_global->global_mmap_file_stat.current_scan_file_stat_info)
				    panic("5:check_mmap_file_current_scan_file_stat_info_invalid  0x%llx != 0x%llx\n",(u64)p_current_scan_file_stat_info,(u64)&p_hot_cold_file_global->global_mmap_file_stat.current_scan_file_stat_info);
            }
			else
				BUG();
	}
}
static void  print_file_stat_memory_reclaim_info(struct file_stat_base *p_file_stat_base,struct hot_cold_file_global *p_hot_cold_file_global)
{
	struct memory_reclaim_info_for_one_warm_list *p_memory_info;
	struct memory_reclaim_info *p_memory_reclaim_info = &p_hot_cold_file_global->memory_reclaim_info;
	unsigned long nrpages = 0;
	if(p_file_stat_base->mapping)
		nrpages = p_file_stat_base->mapping->nrpages;

	printk("%s memory_pressure_level:%d memory_still_memrgency_after_reclaim:%d all_reclaim_pages_one_period:%d file_stat:0x%llx pages:%ld status 0x%x is_cache_file:%d is_global:%d is_writeonly_file:%d scan_file_area_count:%d scan_file_area_max:%d scan_exit_file_area_count:%d scan_zero_page_file_area_count:%d warm_list_file_area_up_count:%d warm_list_file_area_down_count:%d warm_list_file_area_to_writeonly_list_count:%d warm_list_file_area_to_writeonly_list_count_cold:%d direct_reclaim_pages_form_writeonly_file:%d scan_file_area_count_form_writeonly_file:%d scan_other_list_file_area_count:%d scan_file_area_max_for_memory_reclaim:%d scan_file_area_count_reclaim_fail:%d\n",__func__,p_hot_cold_file_global->memory_pressure_level,p_hot_cold_file_global->memory_still_memrgency_after_reclaim,p_hot_cold_file_global->all_reclaim_pages_one_period,(u64)p_file_stat_base,nrpages,p_file_stat_base->file_stat_status,file_stat_in_cache_file_base(p_file_stat_base),file_stat_in_global_base(p_file_stat_base),file_stat_in_writeonly_base(p_file_stat_base),p_memory_reclaim_info->scan_file_area_count,p_memory_reclaim_info->scan_file_area_max,p_memory_reclaim_info->scan_exit_file_area_count,p_memory_reclaim_info->scan_zero_page_file_area_count,p_memory_reclaim_info->warm_list_file_area_up_count,p_memory_reclaim_info->warm_list_file_area_down_count,p_memory_reclaim_info->warm_list_file_area_to_writeonly_list_count,p_memory_reclaim_info->warm_list_file_area_to_writeonly_list_count_cold,p_memory_reclaim_info->direct_reclaim_pages_form_writeonly_file,p_memory_reclaim_info->scan_file_area_count_form_writeonly_file,p_memory_reclaim_info->scan_other_list_file_area_count,p_memory_reclaim_info->scan_file_area_max_for_memory_reclaim,p_memory_reclaim_info->scan_file_area_count_reclaim_fail);

	p_memory_info  = &p_memory_reclaim_info->memory_reclaim_info_writeonly_list;
	pr_info("writeonly_list reclaim_pages_count:%d scan_file_area_count_in_reclaim:%d scan_zero_page_file_area_count_in_reclaim:%d scan_warm_file_area_count:%d",p_memory_info->reclaim_pages_count,p_memory_info->scan_file_area_count_in_reclaim,p_memory_info->scan_zero_page_file_area_count_in_reclaim,p_memory_info->scan_warm_file_area_count);

	p_memory_info  = &p_memory_reclaim_info->memory_reclaim_info_warm_cold_list;
	if(p_memory_info->scan_file_area_count_in_reclaim)
		pr_info("warm_cold_list reclaim_pages_count:%d scan_file_area_count_in_reclaim:%d scan_zero_page_file_area_count_in_reclaim:%d scan_warm_file_area_count:%d",p_memory_info->reclaim_pages_count,p_memory_info->scan_file_area_count_in_reclaim,p_memory_info->scan_zero_page_file_area_count_in_reclaim,p_memory_info->scan_warm_file_area_count);

	p_memory_info  = &p_memory_reclaim_info->memory_reclaim_info_warm_middle_list;
	if(p_memory_info->scan_file_area_count_in_reclaim)
		pr_info("warm_middle_list reclaim_pages_count:%d scan_file_area_count_in_reclaim:%d scan_zero_page_file_area_count_in_reclaim:%d scan_warm_file_area_count:%d",p_memory_info->reclaim_pages_count,p_memory_info->scan_file_area_count_in_reclaim,p_memory_info->scan_zero_page_file_area_count_in_reclaim,p_memory_info->scan_warm_file_area_count);

	p_memory_info  = &p_memory_reclaim_info->memory_reclaim_info_warm_list;
	if(p_memory_info->scan_file_area_count_in_reclaim)
		pr_info("warm_list reclaim_pages_count:%d scan_file_area_count_in_reclaim:%d scan_zero_page_file_area_count_in_reclaim:%d scan_warm_file_area_count:%d",p_memory_info->reclaim_pages_count,p_memory_info->scan_file_area_count_in_reclaim,p_memory_info->scan_zero_page_file_area_count_in_reclaim,p_memory_info->scan_warm_file_area_count);

	pr_info("\n");
}
static noinline unsigned int file_stat_multi_level_warm_or_writeonly_list_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global, struct current_scan_file_stat_info *p_current_scan_file_stat_info,struct file_stat_base *p_file_stat_base,unsigned int scan_file_area_max,char is_cache_file,unsigned int scan_file_area_max_for_memory_reclaim)
{
	char is_global_file_stat;
	struct file_stat *p_file_stat = NULL;
	struct global_file_stat *p_global_file_stat = NULL;
	//struct list_head *warm_list_head;
	//struct current_scan_file_stat_info *p_current_scan_file_stat_info = NULL;
	unsigned int scan_file_area_count = 0,scan_other_list_file_area_count = 0;
    struct mult_warm_list_age_dx mult_warm_list_age_dx;


	/*if(is_cache_file)这个检查要放到后边，因为要对最终p_current_scan_file_stat_info->p_traverse_file_stat的检查
		check_cache_file_current_scan_file_stat_info_invalid(p_hot_cold_file_global,p_file_stat_base,p_current_scan_file_stat_info);
	else
		check_mmap_file_current_scan_file_stat_info_invalid(p_hot_cold_file_global,p_file_stat_base,p_current_scan_file_stat_info);*/

	/*每遍历一个file_stat都要先清理p_hot_cold_file_global->memory_reclaim_info*/
	memset(&p_hot_cold_file_global->memory_reclaim_info,0,sizeof(struct memory_reclaim_info));
	p_hot_cold_file_global->memory_reclaim_info.scan_file_area_max = scan_file_area_max;

	/*指向当前正在遍历的current_scan_file_stat_info，对于调试非常有用*/
	p_hot_cold_file_global->p_struct_current_scan_file_stat_info = p_current_scan_file_stat_info;

	p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
	if(get_file_stat_type(p_file_stat_base) == FILE_STAT_NORMAL)
		is_global_file_stat = 0;
	else if(file_stat_in_global_base(p_file_stat_base))
		is_global_file_stat = 1;
	else{
		panic("%s:file_stat:0x%llx 0x%x error\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
	}

	/*temp链表有file_area的话直接移动到file_stat->warm或global_file_stat->warm链表*/
	if(!list_empty(&p_file_stat_base->file_area_temp)){
		spin_lock(&p_file_stat_base->file_stat_lock);
		/*不能用list_splice()，因为移动file_area后不会清空前者链表头，导致这些file_area同时存在于前后两个链表头上*/
		list_splice_init(&p_file_stat_base->file_area_temp,&p_file_stat->file_area_warm);
		spin_unlock(&p_file_stat_base->file_stat_lock);

		if(file_stat_in_test_base(p_file_stat_base))
			printk("1 %s:file_stat:0x%llx\n",__func__,(u64)p_file_stat_base);
	}

	if(is_global_file_stat){
		p_global_file_stat = container_of(p_file_stat,struct global_file_stat,file_stat);

		/*if(is_cache_file)
			p_current_scan_file_stat_info = &p_hot_cold_file_global->global_file_stat.current_scan_file_stat_info;
		else
			p_current_scan_file_stat_info = &p_hot_cold_file_global->global_mmap_file_stat.current_scan_file_stat_info;*/

		/*是NULL说明是第一个遍历，那就从file_area_warm链表开始遍历*/
		if(NULL == p_current_scan_file_stat_info->p_traverse_file_stat){

			/* 如果current_scan_file_stat_info->temp_head链表头不是空，说明上次使用该current_scan_file_stat_info的
			 * 文件file_stat或global_file_stat，遍历到的不冷不热的file_area，移动到current_scan_file_stat_info->p_traverse_file_area_list_head
			 * 链表头，等遍历到完该链表上的file_area后，没有把temp_head链表头停留的file_area移动回p_traverse_file_area_list_head链表，直接crash*/
			if(NULL != p_current_scan_file_stat_info->p_traverse_file_area_list_head || !list_empty(&p_current_scan_file_stat_info->temp_head)){
				panic("1 %s:p_current_scan_file_stat_info:0x%llx temp_head not empty\n",__func__,(u64)p_current_scan_file_stat_info);
			}
			p_current_scan_file_stat_info->p_traverse_file_stat = p_file_stat;

			global_file_stat_current_scan_file_stat_info(p_current_scan_file_stat_info,p_global_file_stat);

			p_current_scan_file_stat_info->traverse_list_num = p_file_stat->traverse_warm_list_num;
			/*从链表尾的file_area开始遍历*/
			p_current_scan_file_stat_info->p_traverse_first_file_area = list_last_entry(p_current_scan_file_stat_info->p_traverse_file_area_list_head,struct file_area,file_area_list);
		}

		/* 针对global_file_stat，不管是第一次遍历还是第N次遍历，都得更新p_traverse_first_file_area指向要遍历的第一个file_area，
		 * 也得更新traverse_list_num。这就有bug，如果上次global_file_stat->warm链表的file_are没有遍历完，现在直接从链表尾
		 * 遍历file_area，就会中断上次的扫描，直接从链表尾开始遍历了，浪费性能。于是决定，针对global_file_stat，如果上次
		 * global_file_stat->warm链表上的file_area没有遍历完，下个周期遍历global_file_stat，这里就不再
		 * p_current_scan_file_stat_info->traverse_list_num和p_traverse_first_file_area赋值了，而只有等到global_file_stat->warm
		 * 链表上的file_area都遍历完，会把p_current_scan_file_stat_info->p_traverse_file_stat设置NULL。然后再遍历
		 * 该global_file_stat，p_current_scan_file_stat_info->p_traverse_file_stat是NULL，再重新对p_traverse_file_stat
		 * traverse_list_num赋值，并且执行global_file_stat_current_scan_file_stat_info()更新p_traverse_file_area_list_head等*/
		//p_current_scan_file_stat_info->traverse_list_num = p_file_stat->traverse_warm_list_num;
		//p_current_scan_file_stat_info->p_traverse_first_file_area = list_last_entry(p_current_scan_file_stat_info->p_traverse_file_area_list_head,struct file_area,file_area_list);
		
		/* 如果p_traverse_first_file_area是NULL，说明上个周期该file_stat被writeonly和warm_cold被内存回收，而p_current_scan_file_stat_info
		 * 正好指向这个file_stat，不得不把p_traverse_first_file_area设置NULL，因为p_traverse_first_file_area指向的file_area被内存会后
		 * 移动到file_stat->free链表了。详情看direct_recliam_file_area_for_global_file_stat*/
		if(!p_current_scan_file_stat_info->p_traverse_first_file_area)
		    p_current_scan_file_stat_info->p_traverse_first_file_area = list_last_entry(p_current_scan_file_stat_info->p_traverse_file_area_list_head,struct file_area,file_area_list);
		else{
			//struct file_area *p_file_area = p_current_scan_file_stat_info->p_traverse_first_file_area;
			struct file_area *p_file_area_next = list_next_entry(p_current_scan_file_stat_info->p_traverse_first_file_area,file_area_list);
			struct file_area *p_file_area_prev = list_prev_entry(p_current_scan_file_stat_info->p_traverse_first_file_area,file_area_list);

			/*由于p_current_scan_file_stat_info->p_traverse_first_file_area指向的file_area遇到好几次问题，因此检测这个file_area的有效性，
			 *非常有必要。并且还要检测该file_area在链表的下一个file_area，在链表的上一个file_area的检测在
			 *traverse_file_stat_multi_level_warm_list()函数进行，这里不再检测，最后决定就在这里检测*/
			if(!list_entry_is_head(p_file_area_prev, p_current_scan_file_stat_info->p_traverse_file_area_list_head,file_area_list))
				check_multi_level_warm_list_file_area_valid(p_hot_cold_file_global,p_file_stat,p_file_area_prev,p_current_scan_file_stat_info,list_num_get(p_file_area_prev),is_global_file_stat);

			if(!list_entry_is_head(p_file_area_next, p_current_scan_file_stat_info->p_traverse_file_area_list_head,file_area_list))
				check_multi_level_warm_list_file_area_valid(p_hot_cold_file_global,p_file_stat,p_file_area_next,p_current_scan_file_stat_info,list_num_get(p_file_area_next),is_global_file_stat);
		}
	}else{

		/*if(is_cache_file)
			p_current_scan_file_stat_info = &p_hot_cold_file_global->current_scan_file_stat_info;
		else
			p_current_scan_file_stat_info = &p_hot_cold_file_global->current_scan_mmap_file_stat_info;*/

		/*current_scan_file_stat_info.p_file_stat是NULL说明是第一次遍历，或者上一个file_stat的file_area都遍历完了，现在需要遍历下一个file_stat了*/
		if(NULL == p_current_scan_file_stat_info->p_traverse_file_stat){
			/*p_current_scan_file_stat_info.p_file_stat是NULL，p_list_head也必须是NULL，tmp_head_file_stat临时链表必须空*/
			if(NULL != p_current_scan_file_stat_info->p_traverse_file_area_list_head || !list_empty(&p_current_scan_file_stat_info->temp_head))
				panic("%s file_stat:0x%llx list_head:0x%llx tmp_head_empty:%d error\n",__func__,(u64)p_file_stat,(u64)p_current_scan_file_stat_info->p_traverse_file_area_list_head,list_empty(&p_current_scan_file_stat_info->temp_head));
			
	        //printk("1 %s: current_scan_file_stat_info:0x%llx file_stat:0x%llx 0x%x global:%d is_cache_file:%d traverse_list_num:%d\n",__func__,(u64)p_current_scan_file_stat_info,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,file_stat_in_global_base(p_file_stat_base),is_cache_file,p_current_scan_file_stat_info->traverse_list_num);
			update_normal_file_stat_current_scan_file_stat_info(p_current_scan_file_stat_info,p_file_stat);
		}

		/* 如果p_traverse_first_file_area是NULL，说明上个周期该file_stat被writeonly和warm_cold被内存回收，而p_current_scan_file_stat_info
		 * 正好指向这个file_stat，不得不把p_traverse_first_file_area设置NULL，因为p_traverse_first_file_area指向的file_area被内存会后
		 * 移动到file_stat->free链表了。详情看direct_recliam_file_area_for_global_file_stat*/
		if(!p_current_scan_file_stat_info->p_traverse_first_file_area)
		    p_current_scan_file_stat_info->p_traverse_first_file_area = list_last_entry(p_current_scan_file_stat_info->p_traverse_file_area_list_head,struct file_area,file_area_list);
		else{
			//struct file_area *p_file_area = p_current_scan_file_stat_info->p_traverse_first_file_area;
			struct file_area *p_file_area_next = list_next_entry(p_current_scan_file_stat_info->p_traverse_first_file_area,file_area_list);
			struct file_area *p_file_area_prev = list_prev_entry(p_current_scan_file_stat_info->p_traverse_first_file_area,file_area_list);

			/*重大bug，当上一轮遍历的file_stat1(假设该file_stat是global->temp链表尾的)因遍历的file_area个数超过max而结束。接着该file_stat1被iput()释放了。
			 * 但是p_current_scan_file_stat_info->p_traverse_file_stat还指向这个file_stat，p_current_scan_file_stat_info->p_traverse_first_file_area
			 * 还指向最后一次遍历的file_area的上一个。然后新的一轮遍历，就会执行到这里，但是此时p_file_stat是global->temp链表尾最新的file_stat2。
			 * 此时执行下边的check_multi_level_warm_list_file_area_valid()就会发现mapping不相等而panic。解决办法是跳转到current_scan_file_stat_delete
			 * 分支，把file_stat2更新到p_current_scan_file_stat_info->p_traverse_file_stat。这里就有个问题，如果是代码刚执行到这里，file_stat1
			 * 才被iput()并标记delete，这里能立即识别出来吗？不可能，如果是这种情况，file_stat1在global->temp链表尾，
			 * get_file_area_from_file_stat_list()函数里会先对该文件inode加锁，然后才会执行到这里，此时该文件file_stat是不可能被iput()的。
			 *
			 * 灵光一现，我的天，又发现一个隐藏bug，遍历global->writeonly链表的只写文件是直接执行file_stat_multi_level_warm_or_writeonly_list_file_area_solve()，
			 * 不会执行get_file_area_from_file_stat_list()函数，也就是遍历global->writeonly链表的只写文件，该文件file_stat是没有inode加锁的！
			 * 就是因为多想了一下，就发现这个隐藏极深的bug!!!!!!!!!!!!!!!!!!!!!!!!。因此，遍历任何一种文件，都必须执行
			 * get_file_area_from_file_stat_list()函数!!!!!!!仔细查看walk_throuth_all_file_area()源码，遍历global->writeonly链表
			 * 的只写文件，是执行的get_file_area_from_file_stat_list()函数的，只有global_file_stat的file_area的遍历才会直接执行
			 * file_stat_multi_level_warm_or_writeonly_list_file_area_solve()，敏感过度了!!!!!!!!*/
			if(file_stat_in_delete_base(&p_current_scan_file_stat_info->p_traverse_file_stat->file_stat_base) && p_current_scan_file_stat_info->p_traverse_file_stat != p_file_stat)
				goto current_scan_file_stat_delete;

			/*由于p_current_scan_file_stat_info->p_traverse_first_file_area指向的file_area遇到好几次问题，因此检测这个file_area的有效性，
			 *非常有必要。并且还要检测该file_area在链表的下一个file_area，在链表的上一个file_area的检测在
			 *traverse_file_stat_multi_level_warm_list()函数进行，这里不再检测，最后决定就在这里检测*/
			if(!list_entry_is_head(p_file_area_prev, p_current_scan_file_stat_info->p_traverse_file_area_list_head,file_area_list))
				check_multi_level_warm_list_file_area_valid(p_hot_cold_file_global,p_file_stat,p_file_area_prev,p_current_scan_file_stat_info,list_num_get(p_file_area_prev),is_global_file_stat);

			if(!list_entry_is_head(p_file_area_next, p_current_scan_file_stat_info->p_traverse_file_area_list_head,file_area_list))
				check_multi_level_warm_list_file_area_valid(p_hot_cold_file_global,p_file_stat,p_file_area_next,p_current_scan_file_stat_info,list_num_get(p_file_area_next),is_global_file_stat);
		}
	}

	/*计算file_stat的file_area各种的age_dx，最初还想如果是writeonly_or_cold链表上的file_area，不再计算mult_warm_list_age_dx。但是该链表上还
	 *有可能是readonly的file_area，这种file_area将来可能靠mult_warm_list_age_dx的参数升级到warm链表，故去掉限制*/
	//if(1 != p_current_scan_file_stat_info->traverse_list_num)
	mult_warm_list_age_dx_level_solve(p_hot_cold_file_global,p_file_stat,is_global_file_stat,p_current_scan_file_stat_info->traverse_list_num,is_cache_file,&mult_warm_list_age_dx);

	if(1/*warm_list_printk*/)
		printk("%s: current_scan_file_stat_info:0x%llx file_stat:0x%llx 0x%x global:%d is_cache_file:%d traverse_list_num:%d file_area_cold_level:%d to_down_list_age_dx:%d to_writeonly_cold_list_age_dx:%d\n",__func__,(u64)p_current_scan_file_stat_info,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,file_stat_in_global_base(p_file_stat_base),is_cache_file,p_current_scan_file_stat_info->traverse_list_num,mult_warm_list_age_dx.file_area_cold_level,mult_warm_list_age_dx.to_down_list_age_dx,mult_warm_list_age_dx.to_writeonly_cold_list_age_dx);

	/* 重大隐藏bug：如果二者不相等，是否就一定是异常情况然后panic呢？目前想到一种情况：当前normal temp file_stat->warm链表上的file_area没有遍历完，
	 * 但是大于scan_file_area_max于是结束遍历。等下次循环开始，本应该继续从global->temp链表尾选中这个file_stat开始遍历，但是该file_stat被iput()了，
	 * 被移动到了global->delete链表，于是从global->temp选中的file_stat就是新的了，传入到当前函数，此时if不成里，但是不能panic，遍历新的file_stat即可
	 * 还有一种情况，这个file_stat原本是cache文件，现在遍历过一次后，依然因为scan_file_area_max于是结束遍历。等下次循环开始，该文件转成了mmap文件，
	 * 从global->temp链表尾取出的是新的cache file_stat。而p_current_scan_file_stat_info->p_traverse_file_stat指向的file_stat已经转成mmap file_stat，
	 * 而移动到global->temp mmap链表了。于是下边的if依然不成立，但是此时不能panic，而是要遍历新的file_stat*/
	if(p_current_scan_file_stat_info->p_traverse_file_stat != p_file_stat){
current_scan_file_stat_delete:

		struct file_stat_base *p_file_stat_base_temp = &p_current_scan_file_stat_info->p_traverse_file_stat->file_stat_base;

		if(file_stat_in_delete_base(p_file_stat_base_temp) || 
				(file_stat_in_mmap_file_base(p_file_stat_base_temp) && file_stat_in_from_cache_file_base(p_file_stat_base_temp) && is_cache_file)){
               printk("update_normal_file_stat_current_scan_file_stat_info current_scan_file_stat_info:0x%llx file_stat:0x%llx 0x%x file_stat_base_temp:0x%llx 0x%x global:%d is_cache_file:%d traverse_list_num:%d\n",(u64)p_current_scan_file_stat_info,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_stat_base_temp,p_file_stat_base_temp->file_stat_status,file_stat_in_global_base(p_file_stat_base),is_cache_file,p_current_scan_file_stat_info->traverse_list_num);

			/* 重大bug，当current_scan_file_stat_info->p_traverse_file_stat指向的file_stat1(假设是global->temp链表的)被iput()释放而标记delete，
			 * 它就会被从原global->temp链表尾剔除，导致current_scan_file_stat_info->p_traverse_file_stat指向的
			 * file_stat1跟现在的global->temp链表尾的file_stat2不相等。当识别到这种情况，就要执行update_normal_file_stat_current_scan_file_stat_info()
			 * ，把现在global->temp链表尾的file_stat2赋值给current_scan_file_stat_info->p_traverse_file_stat，然后接着
			 * 就会遍历这个新的file_stat2的warm等链表上的file_area。这里边就有个重大的隐藏很深的bug，p_current_scan_file_stat_info->temp_head
			 * 链表上临时保存的已经遍历过的file_stat1的file_area就泄露了，没有移动回file_stat1->warm等链表
			 * 并且，这个file_stat1的file_stat_in_file_area_in_tmp_list_base标记也没有清理掉，将来执行
			 * cold_file_stat_delete_all_file_area()释放这个delete标记的文件的file_area时，发现file_stat1有
			 * file_stat_in_file_area_in_tmp_list_base标记，但是get_normal_file_stat_current_scan_file_stat_info()得到的
			 * p_current_scan_file_stat_info->p_traverse_file_stat却是NULL，从而触发了panic了。为什么会这样？什么情况下
			 * 一个file_stat有file_stat_in_file_area_in_tmp_list_base标记，但是get_normal_file_stat_current_scan_file_stat_info()
			 * 得到的p_current_scan_file_stat_info->p_traverse_file_stat却是NULL？根本不符合常理，因为对p_current_scan_file_stat_info->p_traverse_file_stat
			 * 标记NULL时，会执行update_file_stat_next_multi_level_warm_or_writeonly_list()，同时清理掉file_stat的
			 * file_stat_in_file_area_in_tmp_list_base标记。就是说如果一个file_stat有file_stat_in_file_area_in_tmp_list_base标记，
			 * 那get_normal_file_stat_current_scan_file_stat_info()得到的p_current_scan_file_stat_info->p_traverse_file_stat一定
			 * 指向这个file_stat，这是update_normal_file_stat_current_scan_file_stat_info()函数确定的。现在
			 * p_current_scan_file_stat_info->p_traverse_file_stat却是NULL？肯定有不符合常理的地方。耐心排查代码，发现就是
			 * 这里有问题：p_current_scan_file_stat_info->p_traverse_file_stat指向的file_stat1被iput()释放了，标记delete。但是
			 * 却只是执行update_normal_file_stat_current_scan_file_stat_info()把新的file_stat2赋值给
			 * p_current_scan_file_stat_info->p_traverse_file_stat，没有清理掉file_stat1的file_stat_in_file_area_in_tmp_list_base标记。
			 * 因此，基于以上两个问题，在这里增加执行update_file_stat_next_multi_level_warm_or_writeonly_list()即可，既清理
			 * 已iput的file_stat1的file_stat_in_file_area_in_tmp_list_base标记，又把p_current_scan_file_stat_info->temp_head
			 * 遍历过的该file_stat的file_area移动回该file_stat1的warm等链表*/
			update_file_stat_next_multi_level_warm_or_writeonly_list(p_current_scan_file_stat_info,p_current_scan_file_stat_info->p_traverse_file_stat);

			update_normal_file_stat_current_scan_file_stat_info(p_current_scan_file_stat_info,p_file_stat);
        }
		else	
		    panic("%s p_traverse_file_stat:0x%llx != p_file_stat:0x%llx global:%d is_cache_file:%d traverse_list_num:%d\n",__func__,(u64)p_current_scan_file_stat_info->p_traverse_file_stat,(u64)p_file_stat,file_stat_in_global_base(p_file_stat_base),is_cache_file,p_current_scan_file_stat_info->traverse_list_num);
	}

	/*对进程扫描的最后的file_stat进行有效检查*/
	if(is_cache_file)
		check_cache_file_current_scan_file_stat_info_invalid(p_hot_cold_file_global,&p_current_scan_file_stat_info->p_traverse_file_stat->file_stat_base,p_current_scan_file_stat_info);
	else
		check_mmap_file_current_scan_file_stat_info_invalid(p_hot_cold_file_global,&p_current_scan_file_stat_info->p_traverse_file_stat->file_stat_base,p_current_scan_file_stat_info);


	/*扫描多级warm链表上的file_area*/
	scan_file_area_count = traverse_file_stat_multi_level_warm_list(p_hot_cold_file_global,p_file_stat,p_current_scan_file_stat_info,scan_file_area_max,is_global_file_stat,is_cache_file,&mult_warm_list_age_dx);

	/*内存紧张时保持scan_file_area_max_for_memory_reclaim原始值。内存MEMORY_LITTLE_RECLAIM回收少量page*/
	switch(p_hot_cold_file_global->memory_pressure_level)
	{
		/*内存非常紧缺*/
		case MEMORY_EMERGENCY_RECLAIM:
			/*内存紧缺*/
		case MEMORY_PRESSURE_RECLAIM:
			break;
			/*内存碎片有点多，或者前后两个周期分配的内存数太多*/
		case MEMORY_LITTLE_RECLAIM:
			scan_file_area_max_for_memory_reclaim = 16;
			break;
		case MEMORY_IDLE_SCAN:
			scan_file_area_max_for_memory_reclaim = 0;
			break;
		default:
			BUG();
	}

	if(scan_file_area_max_for_memory_reclaim){
		/*如果处于内存紧张模式，则扫描writeonly、cold链表上的file_area，并回收page*/
		if(is_global_file_stat){
			direct_recliam_file_area_for_global_file_stat(p_hot_cold_file_global,p_global_file_stat,p_current_scan_file_stat_info,scan_file_area_max_for_memory_reclaim);
		}else{
			direct_recliam_file_area_for_file_stat(p_hot_cold_file_global,p_file_stat,p_current_scan_file_stat_info,scan_file_area_max_for_memory_reclaim);
		}
	}

	/*释放global_file_stat delete链表上的file_area*/
	if(is_global_file_stat){

		/*内存回收后，遍历file_stat->hot、refault、free链表上的各种file_area的处理*/
		scan_file_area_max = 128;
		scan_other_list_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,&p_file_stat->file_stat_base,&p_file_stat->file_area_hot,scan_file_area_max,F_file_area_in_hot_list,FILE_STAT_NORMAL);
		//scan_file_area_max = 128;新版本把file_area_refault移动到global_file_stat了
		//scan_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,&p_file_stat->file_stat_base,&p_file_stat->file_area_refault,scan_file_area_max,F_file_area_in_refault_list,FILE_STAT_NORMAL);
		scan_file_area_max = 128;
		scan_other_list_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,&p_file_stat->file_stat_base,&p_file_stat->file_area_free,scan_file_area_max,F_file_area_in_free_list,FILE_STAT_NORMAL);
		/* 这里有个隐藏bug，没有遍历file_stat->file_area_mapcount链表上file_area，对mapcount file_area进行降级处理。
		 * 但是新版本去除了file_stat->file_area_mapcount链表，为了节省内存，mapcount file_area都移动到file_stat->mapcount链表了*/
		scan_other_list_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,&p_file_stat->file_stat_base,&p_global_file_stat->file_area_mapcount,scan_file_area_max,F_file_area_in_mapcount_list,FILE_STAT_NORMAL);
		scan_file_area_max = 128;
		scan_other_list_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,&p_file_stat->file_stat_base,&p_global_file_stat->file_area_refault,scan_file_area_max,F_file_area_in_refault_list,FILE_STAT_NORMAL);



		/*释放已经iput()释放掉的文件的file_area*/
		if(is_cache_file){
			/* 重大隐藏bug，iput()的global file_stat的file_area都是移动到file_area_delete_list链表，这里释放该链表的file_area，然后得从file_area从
			 * file_area_delete_list链表list_del剔除掉。这样就得spin_lock加锁了，因为并发修改file_area_delete_list链表的成员了，竟然没想到这一点。
			 * 简单的东西有时就是莫名其妙的遗忘，不能太相信大脑。
			 *
			 * 最后决定，但是把file_area_delete_lock链表上的file_area一次性移动到file_area_delete_lock_temp链表，减少加锁机会。然后遍历
			 * file_area_delete_lock_temp链表上的file_area并释放，此时就不用再加锁了*/

			/* 最新方案，iput()->find_get_entry_for_file_area()不再把file_area以file_area->file_area_delete移动到global_file_stat_delete
			 * 链表，而只是标记file_area的in_mapping_delete。异步内存回收线程看到file_area有in_maping_delete标记，再把file_area以
			 * file_area_list移动到file_area_delete_list_temp链表，不用这里再list_splice_init()移动。但原先的思路很宝贵，要保留*/
			/*if(!list_empty(&p_hot_cold_file_global->global_file_stat.file_area_delete_list)){
			  spin_lock(&p_hot_cold_file_global->global_file_stat.file_area_delete_lock);
			  list_splice_init(&p_hot_cold_file_global->global_file_stat.file_area_delete_list,&p_hot_cold_file_global->global_file_stat.file_area_delete_list_temp);
			  spin_unlock(&p_hot_cold_file_global->global_file_stat.file_area_delete_lock);
			  }*/

			//global_file_stat_file_area_delete(p_hot_cold_file_global,p_file_stat_base,&p_hot_cold_file_global->global_file_stat.file_area_delete_list);
			global_file_stat_file_area_delete(p_hot_cold_file_global,p_file_stat_base,&p_hot_cold_file_global->global_file_stat.file_area_delete_list,p_current_scan_file_stat_info);
            
			global_file_stat_zero_page_file_area_list_solve(p_hot_cold_file_global,p_global_file_stat,&p_hot_cold_file_global->global_file_stat.zero_page_file_area_list,128);
		}
		else{
			/*if(!list_empty(&p_hot_cold_file_global->global_mmap_file_stat.file_area_delete_list)){
				spin_lock(&p_hot_cold_file_global->global_mmap_file_stat.file_area_delete_lock);
				list_splice_init(&p_hot_cold_file_global->global_mmap_file_stat.file_area_delete_list,&p_hot_cold_file_global->global_mmap_file_stat.file_area_delete_list_temp);
				spin_unlock(&p_hot_cold_file_global->global_mmap_file_stat.file_area_delete_lock);
			}*/
			//global_file_stat_file_area_delete(p_hot_cold_file_global,p_file_stat_base,&p_hot_cold_file_global->global_mmap_file_stat.file_area_delete_list);
			global_file_stat_file_area_delete(p_hot_cold_file_global,p_file_stat_base,&p_hot_cold_file_global->global_mmap_file_stat.file_area_delete_list,p_current_scan_file_stat_info);
			
			global_file_stat_zero_page_file_area_list_solve(p_hot_cold_file_global,p_global_file_stat,&p_hot_cold_file_global->global_mmap_file_stat.zero_page_file_area_list,128);
		}
	}
	
	p_hot_cold_file_global->memory_reclaim_info.scan_file_area_count = scan_file_area_count;
	p_hot_cold_file_global->memory_reclaim_info.scan_file_area_max_for_memory_reclaim = scan_file_area_max_for_memory_reclaim;

	/*由于普通只写文件还有个direct_reclaim_pages_form_writeonly_file，统计只读文件direct模式从free、hot、refault链表回收的page数，因此针对普通文件print_file_stat_memory_reclaim_info，放到get_file_area_from_file_stat_list_common函数*/
	if(is_global_file_stat){
		p_hot_cold_file_global->memory_reclaim_info.scan_other_list_file_area_count = scan_other_list_file_area_count;
		print_file_stat_memory_reclaim_info(p_file_stat_base,p_hot_cold_file_global);
	}

	return (scan_file_area_count /*+ scan_other_list_file_area_count*/);
}
/* 遍历global hot链表上的file_stat，再遍历这些file_stat->hot链表上的file_area，如果不再是热的，则把file_area
 * 移动到file_stat->warm链表。如果file_stat->hot链表上的热file_area个数减少到热文件阀值以下，则降级到
 * global temp、middle_file、large_file链表*/
static noinline int hot_file_stat_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct list_head *file_stat_hot_head,unsigned int scan_file_area_max,char is_cache_file)
{
	struct file_stat *p_file_stat = NULL;
	struct file_stat_base *p_file_stat_base = NULL,*p_file_stat_base_temp;
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int file_stat_type,scan_file_area_count = 0;
	unsigned int file_area_age_dx,file_area_hot_to_temp_age_dx;
	//unsigned int file_area_age;
	unsigned int file_area_hot_to_warm_list_count = 0;
	char file_stat_changed;
	char is_hot_file,is_global_file_stat;
	char file_stat_delete_lock = 0;

	file_stat_delete_protect_lock(1);
	file_stat_delete_lock = 1;
	/*现在都是file_stat_base添加到global temp、hot等链表，不再是file_stat了*/
	//list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->file_stat_hot_head,hot_cold_file_list){
	list_for_each_entry_safe_reverse(p_file_stat_base,p_file_stat_base_temp,file_stat_hot_head,hot_cold_file_list){
		file_stat_delete_protect_unlock(1); 
		file_stat_delete_lock = 0;

		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

		if(!file_stat_in_file_stat_hot_head_list_base(p_file_stat_base) || file_stat_in_file_stat_hot_head_list_error_base(p_file_stat_base))
			panic("%s file_stat:0x%llx not int file_stat_hot_head_list status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

		/*if(scan_file_area_count > scan_file_area_max)
			break;*/

		if(p_file_stat_base->recent_traverse_age < p_hot_cold_file_global->global_age)
			p_file_stat_base->recent_traverse_age = p_hot_cold_file_global->global_age;
	
		if(is_cache_file){
			file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx;
			if(file_stat_in_mmap_file_base(p_file_stat_base))
				panic("%s file_stat:0x%llx status:0x%x is mmap file\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		}
		else{
			file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx + MMAP_FILE_HOT_TO_TEMP_AGE_DX;
			if(file_stat_in_cache_file_base(p_file_stat_base))
				panic("%s file_stat:0x%llx status:0x%x is cache file\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		}

		is_global_file_stat = file_stat_in_global_base(p_file_stat_base);
		/*遍历global->file_stat_hot_head上的热文件file_stat的file_area_hot链表上的热file_area，如果哪些file_area不再被访问了，则要把
		 *file_area移动回file_stat->file_area_warm链表。同时令改文件的热file_area个数file_stat->file_area_hot_count减1*/
		list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat->file_area_hot,file_area_list){

			scan_file_area_count ++;

			/*引入多层warm链表后，hot、mapcount、refault的file_area都添加到了file_stat->hot链表，遇到这种mapcount、refault file_area直接略过*/
			if(file_area_in_mapcount_list(p_file_area) || file_area_in_refault_list(p_file_area))
				goto next_file_area;

			//file_stat->file_area_hot尾巴上长时间未被访问的file_area再降级移动回file_stat->file_area_warm链表头
			//file_area_age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;
			//get_file_area_age()函数里，会把file_area转换成hot、mapcount file_area需要特别注意!!!!!!!!!!!!
			get_file_area_age(p_file_stat_base,p_file_area,/*file_area_age,*/p_hot_cold_file_global,file_stat_changed,FILE_STAT_NORMAL,is_global_file_stat);
			if(file_stat_changed)
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_stat_changed error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

			file_area_age_dx = p_hot_cold_file_global->global_age - p_file_area->file_area_age;

			if(file_area_age_dx > file_area_hot_to_temp_age_dx){
				if(!file_area_in_hot_list(p_file_area) || file_area_in_hot_list_error(p_file_area))
					panic("%s file_area:0x%llx status:0x%x not in file_area_hot\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
#if 0			
				/*有ahead标记的file_area，即便长时间没访问也不处理，而是仅仅清理file_area的ahead标记且跳过，等到下次遍历到该file_area再处理*/
				if(file_area_in_ahead(p_file_area)){
					clear_file_area_in_ahead(p_file_area);
					/*内存不紧缺时，禁止回收有ahead标记的file_area的page*/
					if(IS_MEMORY_ENOUGH(p_hot_cold_file_global) || file_area_age_dx < p_hot_cold_file_global->file_area_reclaim_ahead_age_dx)
						continue;
				}
#endif				
				file_area_hot_to_warm_list_count ++;
				/*现在把file_area移动到file_stat->warm链表，不再移动到file_stat->temp链表，因此不用再加锁*/
				//spin_lock(&p_file_stat->file_stat_lock);
				if(0 == p_file_stat->file_area_hot_count)
					panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x file_area_hot_count == 0 error !!!\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
				p_file_stat->file_area_hot_count --;
				clear_file_area_in_hot_list(p_file_area);
				//set_file_area_in_warm_list(p_file_area);
				list_num_update(p_file_area,POS_WARM);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
				if(file_stat_in_test_base(p_file_stat_base))
					printk("2 %s:file_stat:0x%llx file_area:0x%llx status:0x%x age:%d hot to warm\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,p_file_area->file_area_age);
				//spin_unlock(&p_file_stat->file_stat_lock);	    
			}
		}

		/*重点：将链表尾已经遍历过的file_area移动到file_stat->hot链表头，下次从链表尾遍历的才是新的未遍历过的file_area。此时不用加锁!!!!!!!!!!!!!*/
		if(can_file_area_move_to_list_head(p_file_area,&p_file_stat->file_area_hot,F_file_area_in_hot_list))
		    list_move_enhance(&p_file_stat->file_area_hot,&p_file_area->file_area_list);

		/*该文件file_stat的热file_area个数file_stat->file_area_hot_count小于阀值，则被判定不再是热文件
		  然后file_stat就要移动回hot_cold_file_global->file_stat_temp_head 或 hot_cold_file_global->file_stat_temp_large_file_head链表*/
		if(file_stat_in_cache_file_base(p_file_stat_base))
			is_hot_file = is_file_stat_hot_file(p_hot_cold_file_global,p_file_stat);
		else
			is_hot_file = is_mmap_file_stat_hot_file(p_hot_cold_file_global,p_file_stat);

		//if(!is_file_stat_hot_file(p_hot_cold_file_global,p_file_stat)){
		if(!is_hot_file){

			/*热文件降级不再加global lock，因为只会把热file_stat降级到global middle_file、large_file 链表*/
			if(is_cache_file)
				spin_lock(&p_hot_cold_file_global->global_lock);
			else
				spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);

			if(!file_stat_in_delete_base(p_file_stat_base)){
				clear_file_stat_in_file_stat_hot_head_list_base(p_file_stat_base);

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
				file_stat_type = is_file_stat_file_type_ori(p_hot_cold_file_global,p_file_stat_base);
				if(TEMP_FILE == file_stat_type){
					set_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_temp_head);
					else
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
				}else if(MIDDLE_FILE == file_stat_type){
					set_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_middle_file_head);
					else
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_middle_file_head);
				}
				else{
					set_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
					p_hot_cold_file_global->file_stat_large_count ++;
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_large_file_head);
					else
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
				}

				if(is_cache_file)
					hot_cold_file_global_info.file_stat_hot_count --;//热文件数减1
				else
					hot_cold_file_global_info.mmap_file_stat_hot_count --;//热文件数减1
			}
			if(is_cache_file)
				spin_unlock(&p_hot_cold_file_global->global_lock);
			else
				spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
		}

next_file_area:
		file_stat_delete_protect_lock(1);
		file_stat_delete_lock = 1;
		if(&p_file_stat_base_temp->hot_cold_file_list == file_stat_hot_head  || file_stat_in_delete_file_base(p_file_stat_base_temp))
			break;

		/* break必须放到这里，不能当道for循环的上边。否则因为scan_file_area_count > scan_file_area_max 而break后，
		 * 当前的p_file_stat_base对应的文件，就无法遍历，内存回收。并且，如果这个file_stat如果还是链表头的，就会
		 * 在下边list_move_enhance()失败，因为它是链表头的file_stat。如此导致file_stat长时间无法遍历到，对它内存回收*/
		if(scan_file_area_count > scan_file_area_max)
			break;
	}
	if(file_stat_delete_lock)
		file_stat_delete_protect_test_unlock(1);

	/*重点：将链表尾已经遍历过的file_stat移动到p_hot_cold_file_global->file_stat_hot_head链表头。
	 *下次从链表尾遍历的才是新的未遍历过的file_stat。此时不用加锁!!!!!!!!!真的不用加锁吗????????????
	 *大错特错，如果p_file_stat此时正好被iput()释放，标记file_stat delete并移动到global delete链表，
	 *这里却把p_file_stat又移动到了global hot链表，那就出大问题了。因此，这里不用加锁防护global temp
	 *链表file_stat的增加与删除，但是要防护iput()把该file_stat并发移动到global delete链表。方法很简单，
	 *加锁后p_file_stat不是链表头，且没有delete标记即可。详细原因get_file_area_from_file_stat_list()有解释
	 !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
	if(is_cache_file)
		spin_lock(&p_hot_cold_file_global->global_lock);
	else
		spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);

	/*list_for_each_entry()机制保证，即便遍历的链表空返回的链表也是指向链表头，故p_file_stat_base不可能是NULL*/
	//if((&p_file_stat_base->hot_cold_file_list != &p_hot_cold_file_global->file_stat_hot_head) && !file_stat_in_delete_base(p_file_stat_base)){
	if(/*p_file_stat_base && */(&p_file_stat_base->hot_cold_file_list != file_stat_hot_head) && !file_stat_in_delete_base(p_file_stat_base)){
		if(can_file_stat_move_to_list_head(file_stat_hot_head,p_file_stat_base,F_file_stat_in_file_stat_hot_head_list,is_cache_file)){
			list_move_enhance(file_stat_hot_head,&p_file_stat_base->hot_cold_file_list);
		}
	}
	if(is_cache_file)
		spin_unlock(&p_hot_cold_file_global->global_lock);
	else
		spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

	//file_stat的hot链表转移到temp链表的file_area个数
	if(file_stat_in_cache_file_base(p_file_stat_base))
		p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_hot_to_warm_from_hot_file += file_area_hot_to_warm_list_count;
	else
		p_hot_cold_file_global->mmap_file_shrink_counter.file_area_hot_to_warm_from_hot_file += file_area_hot_to_warm_list_count;

	return scan_file_area_count;
}
static noinline int scan_mmap_mapcount_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max)
{
	struct file_stat *p_file_stat = NULL;
	struct file_stat_base *p_file_stat_base = NULL,*p_file_stat_base_temp;
	unsigned int mapcount_file_area_count_origin;
	unsigned int scan_file_area_count = 0;
	unsigned int mapcount_to_temp_file_area_count_from_mapcount_file = 0;
	//char file_stat_change = 0;
	LIST_HEAD(file_stat_list);
	char file_stat_delete_lock = 0;

	file_stat_delete_protect_lock(1);
	file_stat_delete_lock = 1;
	//每次都从链表尾开始遍历
	//list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->mmap_file_stat_mapcount_head,hot_cold_file_list){
	list_for_each_entry_safe_reverse(p_file_stat_base,p_file_stat_base_temp,&p_hot_cold_file_global->mmap_file_stat_mapcount_head,hot_cold_file_list){
		file_stat_delete_protect_unlock(1); 
		file_stat_delete_lock = 0;

		if(!file_stat_in_mapcount_file_area_list_base(p_file_stat_base) || file_stat_in_mapcount_file_area_list_error_base(p_file_stat_base))
			panic("%s file_stat:0x%llx not in_mapcount_file_area_list status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

	    p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
		//遍历file_stat->file_area_mapcount上的file_area，如果file_area的page的mapcount都是1，file_area不再是mapcount file_area，则降级到temp_list
		//if(!list_empty(&p_file_stat->file_area_mapcount)){新版本mapcount链表用hot替代了
		if(!list_empty(&p_file_stat->file_area_hot)){
			mapcount_file_area_count_origin = p_file_stat->mapcount_file_area_count;
			//file_stat_change = 0;

			//scan_file_area_count += reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_mapcount,SCAN_MAPCOUNT_FILE_AREA_COUNT_ONCE,1 << F_file_area_in_mapcount_list,MMAP_FILE_AREA_MAPCOUNT_AGE_DX);
		    //scan_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,&p_file_stat->file_stat_base,&p_file_stat->file_area_hot,SCAN_MAPCOUNT_FILE_AREA_COUNT_ONCE,F_file_area_in_mapcount_list,FILE_STAT_NORMAL);
			/* 引入多层warm链表后，省去了file_stat->file_area_mapcount链表，mapcount file_area全都移动到了file_stat->file_area_hot链表。此时这里不能传入F_file_area_in_mapcount_list，
			 * 而需要传入F_file_area_in_hot_list，file_stat_other_list_file_area_solve函数里碰到mapcunt的file_area，会自动设置file_area_type_for_bit为F_file_area_in_mapcount_list*/
		    scan_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,&p_file_stat->file_stat_base,&p_file_stat->file_area_hot,SCAN_MAPCOUNT_FILE_AREA_COUNT_ONCE,F_file_area_in_hot_list,FILE_STAT_NORMAL);

			if(mapcount_file_area_count_origin != p_file_stat->mapcount_file_area_count){
				//文件file_stat的mapcount的file_area个数减少到阀值以下了，降级到普通文件
				if(0 == is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat)){
					unsigned int file_stat_type;

					/* 只有file_stat从global mapcount链表移动回global temp链表，才得global mmap_file_global_lock加锁，
					 * 下边file_stat移动回global mapcount链表头不用加锁*/
					spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
					/*p_file_stat可能被iput并发标记delete并移动到global delete链表了，要加锁后防护这种情况*/
					if(!file_stat_in_delete_base(p_file_stat_base)){
						clear_file_stat_in_mapcount_file_area_list_base(p_file_stat_base);

						/*根据file_stat的file_area个数判断文件是普通文件、中型文件、大文件，并移动到匹配的global temp、middle、large链表*/
						file_stat_type = is_mmap_file_stat_file_type(p_hot_cold_file_global,p_file_stat_base);
						if(TEMP_FILE == file_stat_type){
							set_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
							list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
						}else if(MIDDLE_FILE == file_stat_type){
							set_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
							list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_middle_file_head);
						}
						else{
							set_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
							list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
						}

						spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
						p_hot_cold_file_global->mapcount_mmap_file_stat_count --;
						//file_stat_change = 1;
						if(shrink_page_printk_open1)
							printk("1:%s file_stat:0x%llx status:0x%x  mapcount to temp file\n",__func__,(u64)p_file_stat,p_file_stat_base->file_stat_status);
					}
				}
			}
		}
#if 0
		/*file_stat未发生变化，先移动到file_stat_list临时链表。如果此时global mmap_file_stat_mapcount_head链表没有file_stat了，
		  则p_file_stat_temp指向链表头，下次循环直接break跳出*/
		if(0 == file_stat_change)
			list_move(&p_file_stat->hot_cold_file_list,&file_stat_list);
#endif

		file_stat_delete_protect_lock(1);
		file_stat_delete_lock = 1;
		if(&p_file_stat_base_temp->hot_cold_file_list == &p_hot_cold_file_global->mmap_file_stat_mapcount_head  || file_stat_in_delete_file_base(p_file_stat_base_temp))
			break;

		//超出扫描的file_area上限，break
		if(scan_file_area_count > scan_file_area_max){
			break;
		}
	}
	if(file_stat_delete_lock)
		file_stat_delete_protect_test_unlock(1);

#if 0
	//如果file_stat_list临时链表还有file_stat，则把这些file_stat移动到global mmap_file_stat_hot_head链表头，下轮循环就能从链表尾巴扫描还没有扫描的file_stat了
	if(!list_empty(&file_stat_list)){
		list_splice(&file_stat_list,&p_hot_cold_file_global->mmap_file_stat_mapcount_head);
	}
#endif
	/*把本次在链表尾遍历过的file_stat移动到链表头，下次执行该函数从链表尾遍历到的是新的未遍历过的file_stat*/

	/* 本来以为这里只是global mapcount链表直接移动file_stat，不用mmap_file_global_lock加锁。真的不用加锁？
	 * 大错特错，如果p_file_stat此时正好被iput()释放，标记file_stat delete并移动到global delete链表，
	 *这里却把p_file_stat又移动到了global mapcount链表，那就出大问题了。因此，这里不用加锁防护global temp
	 *链表file_stat的增加与删除，但是要防护iput()把该file_stat并发移动到global delete链表。方法很简单，
	 *加锁后p_file_stat不是链表头，且没有delete标记即可。详细原因get_file_area_from_file_stat_list()有解释
	 !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
	spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
	if(&p_file_stat_base->hot_cold_file_list != &p_hot_cold_file_global->mmap_file_stat_mapcount_head && !file_stat_in_delete_base(p_file_stat_base)){
	    if(can_file_stat_move_to_list_head(&p_hot_cold_file_global->mmap_file_stat_mapcount_head,p_file_stat_base,F_file_stat_in_mapcount_file_area_list,0))
		    list_move_enhance(&p_hot_cold_file_global->mmap_file_stat_mapcount_head,&p_file_stat_base->hot_cold_file_list);
	}
	spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

	p_hot_cold_file_global->mmap_file_shrink_counter.mapcount_to_temp_file_area_count_from_mapcount_file += mapcount_to_temp_file_area_count_from_mapcount_file;
	return scan_file_area_count;
}
#if 0
/*把在global file_stat_temp_head链表但实际是mmap的文件file_stat从global file_stat_temp_head链表剔除，然后添加到global mmap_file_stat_temp_head链表。
 *注意，现在规定只有tiny small文件才允许从cache file转成mmap文件，不允许small、normal文件转换。因为，转成tiny small mmap文件后，可以再经先有代码
 *转成small/normal mmap文件。目的是为了降低代码复杂度，其实这个函数里也可以根据file_area个数，把tiny small cache文件转成normal mmap文件，太麻烦了 */
static noinline unsigned int cache_file_stat_move_to_mmap_head(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int file_type)
{
	int file_stat_list_del_ok = 0;
	//struct file_stat *p_file_stat = NULL;
	//struct file_stat_small *p_file_stat_small = NULL;
	//struct file_stat_tiny_small *p_file_stat_tiny_small = NULL;
#if 0
	if(FILE_STAT_NORMAL == file_type){
		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

		if(!list_empty(&p_file_stat->file_area_hot) || !list_empty(&p_file_stat->file_area_refault) || !list_empty(&p_file_stat->file_area_free) ||
				!list_empty(&p_file_stat->file_area_warm) /*|| !list_empty(&p_file_stat->file_area_free_temp)*/)/*cache file不使用file_stat->free_temp链表*/
			panic("%s file_stat:0x%llx list empty error\n",__func__,(u64)p_file_stat);
	}
#endif
	if(file_stat_in_mmap_file_base(p_file_stat_base))
		panic("%s file_stat:0x%llx status:0x%x in mmap error\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

	spin_lock(&p_hot_cold_file_global->global_lock);
	/*file_stat可能被并发iput释放掉*/
	if(!file_stat_in_delete_base(p_file_stat_base)){
		list_del(&p_file_stat_base->hot_cold_file_list);
		file_stat_list_del_ok = 1;
	}
	clear_file_stat_in_cache_file_base(p_file_stat_base);
	spin_unlock(&p_hot_cold_file_global->global_lock);

	/*如果file_stat被并发delete了，则不能在下边再list_add()把file_stat添加到mmap 的链表，否则会导致file_stat既存在源链表，
	 *又在mmap的链表，破坏了链表*/
	/*if(!file_stat_list_del_ok){
		printk("%s file_stat:0x%llx status:0x%x has delete\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		return 0;
	}*/

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
	set_file_stat_in_mmap_file_base(p_file_stat_base);
	set_file_stat_in_from_cache_file_base(p_file_stat_base);

	/*file_stat可能被并发iput释放掉*/
	if(!file_stat_in_delete_base(p_file_stat_base)){
		//if(FILE_STAT_TINY_SMALL != get_file_stat_type(p_file_stat_base))
		//	BUG();

		/*现在改为把file_stat_base结构体添加到global temp/hot/large/midlde/tiny small/small file链表，不再是file_stat或file_stat_small或file_stat_tiny_small结构体*/
		if(FILE_STAT_TINY_SMALL == file_type){
			//p_file_stat_tiny_small = container_of(p_file_stat_base,struct file_stat_tiny_small,file_stat_base);
			if(file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base))
				list_add(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_tiny_small_file_head);
			else if(file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base))
				list_add(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_tiny_small_file_one_area_head);
			else
				BUG();
		}
		else if(FILE_STAT_SMALL == file_type){
			//p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
			list_add(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_small_file_head);
		}
		else if(FILE_STAT_NORMAL == file_type){
			//p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
			if(file_stat_in_file_stat_temp_head_list_base(p_file_stat_base))
				list_add(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
			else if(file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base))
				list_add(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_middle_file_head);
			else if(file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base))
				list_add(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
			else
				BUG();
		}else
			BUG();
	}
	spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

	return 0;
}
#else
/*把在global file_stat_temp_head链表但实际是mmap的文件file_stat从global file_stat_temp_head链表剔除，然后添加到global mmap_file_stat_temp_head链表。
 *注意，现在规定只有tiny small文件才允许从cache file转成mmap文件，不允许small、normal文件转换。因为，转成tiny small mmap文件后，可以再经先有代码
 *转成small/normal mmap文件。目的是为了降低代码复杂度，其实这个函数里也可以根据file_area个数，把tiny small cache文件转成normal mmap文件，太麻烦了 */
static noinline unsigned int cache_file_stat_move_to_mmap_head(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int file_type)
{
	if(file_stat_in_mmap_file_base(p_file_stat_base))
		panic("%s file_stat:0x%llx status:0x%x in mmap error\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

	/* 之前的方案有问题
	 * 1：在iput()，会发生file_stat既没有in_cache也没有in_mmap标记，怕因file_stat状态错误触发潜在bug!!!!!!!!!!!!!!
	 * 并且，现在cache和mmap file_stat合二为一，不用再synchronize_rcu()等待进程hot_file_update_file_status()函数。
	 * 2：还有一个潜在bug。之前的cache_file_stat_move_to_mmap_head()函数，是先global_lock加锁，list_del掉file_stat，
	 * 然后再mmap_file_global_lock加锁，list_add把file_stat移动到mmap global file_stat等链表。这就有问题了。如果在
	 * global_lock加锁，list_del掉file_stat，此时文件被并发iput()，则要把该file_stat list_move到global delete链表，
	 * list_move本质list_del加list_add，可以该file_stat已经list_del了，再list_del就有问题了，这是个bug!!!!!!!!!!!
	 * 
	 * 于是，现在改为globla_lock和mmap_lock同时加锁，清理in_cache且设置in_mmap标记，避免潜在麻烦。此时iput()释放
	 * 文件inode，不管是cache文件，还是mmap文件，因为这里globla_lock和mmap_lock同时加锁，都没有了并发问题*/
	spin_lock(&p_hot_cold_file_global->global_lock);
	spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
	/*file_stat可能被并发iput释放掉，必须是加锁后判断一次是否delete了*/
	if(!file_stat_in_delete_base(p_file_stat_base)){
		clear_file_stat_in_cache_file_base(p_file_stat_base);
		set_file_stat_in_mmap_file_base(p_file_stat_base);
		set_file_stat_in_from_cache_file_base(p_file_stat_base);

		/*现在改为把file_stat_base结构体添加到global temp/hot/large/midlde/tiny small/small file链表，不再是file_stat或file_stat_small或file_stat_tiny_small结构体*/
		if(FILE_STAT_TINY_SMALL == file_type){
			if(file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base))
				list_move_tail(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_tiny_small_file_head);
			else if(file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base))
				list_move_tail(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_tiny_small_file_one_area_head);
			else
				BUG();
		}
		else if(FILE_STAT_SMALL == file_type){
			list_move_tail(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_small_file_head);
		}
		else if(FILE_STAT_NORMAL == file_type){
			/* 全都不再list_move_tail了，因为 current_scan_file_stat_info指向的要遍历的file_stat都是global temp、middel、large
			 * 链表尾的file_stat，这里把新的file_stat移动到链表尾，就得清空current_scan_file_stat_info，没这个必要*/
			if(file_stat_in_file_stat_temp_head_list_base(p_file_stat_base)){
				//list_move_tail(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
			}
			else if(file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base)){
				//list_move_tail(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_middle_file_head);
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_middle_file_head);
			}
			else if(file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base)){
				//list_move_tail(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
			}
			else if(file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base)){
				/*cache writeonly文件移动到mmap temp文件链表*/
				clear_file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base);
				set_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
				//list_move_tail(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
				list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
			}
			else
				BUG();
		}else
			BUG();
	}
	p_hot_cold_file_global->file_stat_count --;
	p_hot_cold_file_global->mmap_file_stat_count ++;
	spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
	spin_unlock(&p_hot_cold_file_global->global_lock);

	/*之前该文件是cache文件，默认有writeonly标记，现在转成mmap文件必须去除writeonly标记，否则很容易触发该mmap文件页被异步进程内存回收*/
	if(file_stat_in_writeonly_base(p_file_stat_base))
		clear_file_stat_in_writeonly_base(p_file_stat_base);
	return 0;
}
#endif
/*回收file_area->free_temp链表上的冷file_area的page，回收后的file_area移动到file_stat->free链表头*/
static noinline unsigned int free_page_from_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *file_area_free_temp,unsigned int file_type)
{
	unsigned int free_pages = 0;
	unsigned int scan_file_area_count = 0;
	//LIST_HEAD(file_area_have_mmap_page_head);

	/*每次内存回收前先对free_pages_count清0*/
	p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count = 0;
	/*释放file_stat->file_area_free_temp链表上冷file_area的page，如果遇到有mmap文件页的file_area，则会保存到file_area_have_mmap_page_head链表*/
	//isolate_lru_pages += cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_free_temp);
	//isolate_lru_pages = cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,file_area_free_temp/*,&file_area_have_mmap_page_head*/);
	scan_file_area_count = cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,file_area_free_temp,-1,NULL,1,NULL,NULL);
	free_pages = p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;/*free_pages_count本身是个累加值*/
#if 0
	/*遍历file_area_have_mmap_page_head链表上的含有mmap文件页的file_area，然后回收这些file_area上的mmap文件页*/
	if(!list_empty(&file_area_have_mmap_page_head)){
		cache_file_area_mmap_page_solve(p_hot_cold_file_global,p_file_stat_base,&file_area_have_mmap_page_head,file_stat_type);
		/* 参与内存回收后的file_area再移动回file_area_free_temp链表，回归正常流程。将来这些file_area都会移动回file_stat->free链表，
		 * 如果内存回收失败则file_area会移动回file_stat->refault链表。总之，这些包含mmap page的file_area跟正常的cache文件file_area
		 * 内存回收处理流程都一样*/
		list_splice(&file_area_have_mmap_page_head,file_area_free_temp);
	}
#endif
	if(shrink_page_printk_open1 || file_stat_in_test_base(p_file_stat_base))
		printk("1:%s %s %d p_hot_cold_file_global:0x%llx p_file_stat:0x%llx status:0x%x free_pages:%d\n",__func__,current->comm,current->pid,(u64)p_hot_cold_file_global,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,free_pages);

	/*注意，file_stat->file_area_free_temp 和 file_stat->file_area_free 各有用处。file_area_free_temp保存每次扫描释放的
	 *page的file_area。释放后把这些file_area移动到file_area_free链表，file_area_free保存的是每轮扫描释放page的所有file_area。
	 p_file_stat->file_area_free链表上的file_area结构要长时间也没被访问就释放掉*/

	/*新的版本只有移动file_stat->temp链表上的file_area才得加锁，其他链表之间的移动不用加锁*/
	//list_splice(&p_file_stat->file_area_free_temp,&p_file_stat->file_area_free);
	if(FILE_STAT_NORMAL == file_type){
		struct file_stat *p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

		list_splice(file_area_free_temp,&p_file_stat->file_area_free);
		//p_file_stat->reclaim_pages_last_period = free_pages;
		p_file_stat->reclaim_pages += free_pages;
	}else if(FILE_STAT_SMALL == file_type){
		struct file_stat_small *p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);

		list_splice(file_area_free_temp,&p_file_stat_small->file_area_other);
		p_file_stat_small->reclaim_pages += free_pages;
	}else if(FILE_STAT_TINY_SMALL == file_type){
		struct file_stat_tiny_small *p_file_stat_tiny_small = container_of(p_file_stat_base,struct file_stat_tiny_small,file_stat_base);

		/*tiny small参与内存回收后的file_area必须移动回file_area_temp链表，因此必须加锁*/
		spin_lock(&p_file_stat_base->file_stat_lock);
		//list_splice(file_area_free_temp,&p_file_stat_tiny_small->file_area_temp);
		list_splice(file_area_free_temp,&p_file_stat_base->file_area_temp);
		spin_unlock(&p_file_stat_base->file_stat_lock);

		p_file_stat_tiny_small->reclaim_pages += free_pages;
	}else
		BUG();
	/*list_splice把前者的链表成员a1...an移动到后者链表，并不会清空前者链表。必须INIT_LIST_HEAD清空前者链表，否则它一直指向之前的
	 *链表成员a1...an。后续再向该链表添加新成员b1...bn。这个链表就指向的成员就有a1...an + b1...+bn。而此时a1...an已经移动到了后者
	 *链表，相当于前者和后者链表都指向了a1...an成员，这样肯定会出问题.之前get_file_area_from_file_stat_list()函数报错
	 *"list_add corruption. next->prev should be prev"而crash估计就是这个原因!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 */

	/*现在不用p_file_stat->file_area_free_temp这个全局链表了，故list_splice()后不用再初始化该链表头*/
	//INIT_LIST_HEAD(&p_file_stat->file_area_free_temp);
	//INIT_LIST_HEAD(&file_area_free_temp);

	//隔离的page个数
	//p_hot_cold_file_global->hot_cold_file_shrink_counter.isolate_lru_pages += isolate_lru_pages;
	//从系统启动到目前释放的page个数
	if(file_stat_in_cache_file_base(p_file_stat_base))
		p_hot_cold_file_global->free_pages += p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;
	else
		p_hot_cold_file_global->free_mmap_pages += p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;

	all_file_stat_reclaim_pages_counter(p_hot_cold_file_global,p_file_stat_base,0,free_pages);
	return free_pages;
}
/*遍历file_stat_tiny_small->temp链表上的file_area，遇到hot、refault的file_area则移动到新的file_stat对应的链表。
 * 注意，执行这个函数前，必须保证没有进程再会访问该file_stat_tiny_small*/
static inline unsigned int move_tiny_small_file_area_to_normal_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_tiny_small *p_file_stat_tiny_small,struct file_stat *p_file_stat,char is_cache_file)
{
	unsigned int scan_file_area_count = 0;
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int file_area_type;

	/*从链表尾开始遍历file_area，有in_refault、in_free、in_hot属性的则移动到新的file_stat对应的链表，最多只遍历640个，即便
	  file_stat_tiny_small->temp链表上可能因短时间大量访问pagecahce而导致有很多的file_area*/
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_tiny_small->file_stat_base.file_area_temp,file_area_list){
		if(++ scan_file_area_count > NORMAL_TEMP_FILE_AREA_COUNT_LEVEL)
			break;
		/*注意，这些函数不仅cache tiny small文件转换成normal file会执行到，mmap的cache tiny small文件转换成normal file也会执行到。
		 *因此mmap的文件file_area的处理要单独分开，不能跟cache文件的处理混到一块，因此引入了reverse_other_file_area_list_common()*/
		if(!file_area_in_temp_list(p_file_area)){
			/* 其实这里就可以判断这些hot、refault的file_area，如果长时间没访问则移动回file_stat->warm链表，
			 * free的file_area则直接释放掉!!!!!!后续改进。并且，不用加file_stat->file_stat_lock锁。
			 * file_stat_tiny_small已经保证不会再有进程访问，p_file_stat只有操作p_file_stat->temp链表的file_area才用加锁!!!!!!!*/
			file_area_type = get_file_area_list_status(p_file_area);
			/*把老的file_stat的free、refaut、hot属性的file_area移动到新的file_stat对应的file_area链表，这个过程老的
			 *file_stat不用file_stat_lock加锁，因为已经保证没进程再访问它。新的file_stat也不用，因为不是把file_area移动到新的file_stat->temp链表*/
#if 0			
			if(is_cache_file)
				file_stat_other_list_file_area_solve_common(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area,file_area_type,FILE_STAT_NORMAL);
			else/*这个函数mmap的tiny small转换成small或normal文件也会调用，这里正是对mmap文件的移动file_area的处理*/
				reverse_other_file_area_list_common(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area,file_area_type,FILE_STAT_NORMAL,NULL);
#else
			/*这里牵涉到一个重大bug，原本针对in_hot、in_free、in_refault等的file_area是执行file_stat_other_list_file_area_solve_common()
			 *函数处理，想当然的以为该函数会把这些file_area移动到file_stat->free、refault、hot或small_file_stat->other链表，想当然太容易
			 *埋入隐藏bug。因为比如in_hot的file_area只有长时间只会temp链表，根本不会移动到file_stat->hot或small_file_stat->other链表。
			 *因此这些in_hot、in_free、in_refault等file_area只会残留在原tiny_smal_file_stat->temp链表，下边list_splice_init再移动到新的
			 *file_stat的temp链表，等将来遍历到这些temp链表上file_area，就会因没in_temp属性而crash!!!!!!!!!!
			 */
			switch (file_area_type){
				case (1 << F_file_area_in_hot_list):
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
					p_file_stat->file_area_hot_count ++;
					break;
				case (1 << F_file_area_in_refault_list):
					//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);多层warm禁止refault file_area移动到file_area_hot链表
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
					break;
					/*file_area同时具备in_free的in_refault属性的，那是in_free的被访问update函数设置了in_refault，要移动到file_stat->in_free链表，
					 *因为这种属性的file_area就是在in_free链表，将来执行file_stat_other_list_file_area_solve_common()会把它移动到in_refault链表*/
				case (1 << F_file_area_in_refault_list | 1 << F_file_area_in_free_list):
				case (1 << F_file_area_in_free_list):
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free);
					break;
				case (1 << F_file_area_in_mapcount_list):
					//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);//新版本改为移动到了hot链表
					p_file_stat->mapcount_file_area_count ++;
					break;
				default:
					panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x error\n",__func__,(u64)p_file_stat_tiny_small,(u64)p_file_area,p_file_area->file_area_state);
			}
#endif
		}
	}
	/*老的file_stat_tiny_small已经失效了，新的file_stat已经启用，故其他进程可能并发向file_stat->temp链表添加file_area，因此要spin_lock加锁*/
	spin_lock(&p_file_stat->file_stat_base.file_stat_lock);
	/*把file_stat_tiny_small->temp链表上的temp属性的file_area移动到新的file_stat的temp链表上。不能用list_splice，
	 * 因为list_splice()移动链表成员后，链表头依然指向这些链表成员，不是空链表，list_splice_init()会把它强制变成空链表*/
	//list_splice(&p_file_stat_tiny_small->file_area_temp,p_file_stat->file_area_temp);
	list_splice_init(&p_file_stat_tiny_small->file_stat_base.file_area_temp,&p_file_stat->file_stat_base.file_area_temp);
	spin_unlock(&p_file_stat->file_stat_base.file_stat_lock);
	return scan_file_area_count;
}
static inline unsigned int move_tiny_small_file_area_to_small_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_tiny_small *p_file_stat_tiny_small,struct file_stat_small *p_file_stat_small,char is_cache_file)
{
	unsigned int scan_file_area_count = 0;
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int file_area_type;

	/*从链表尾开始遍历file_area，有in_refault、in_free、in_hot属性的则移动到新的file_stat对应的链表，最多只遍历64个，即便
	  file_stat_tiny_small->temp链表上可能因短时间大量访问pagecahce而导致有很多的file_area*/
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_tiny_small->file_stat_base.file_area_temp,file_area_list){
		if(++ scan_file_area_count > SMALL_FILE_AREA_COUNT_LEVEL)
			break;
		if(!file_area_in_temp_list(p_file_area)){
			//printk("move_tiny_small_file_area_to_small_file: file_stat:0x%llx file_area:0x%llx status:0x%x new:0x%llx\n",(u64)p_file_stat_tiny_small,(u64)p_file_area,p_file_area->file_area_state,(u64)p_file_stat_small);

			/* 其实这里就可以判断这些hot、refault的file_area，如果长时间没访问则移动回file_stat->warm链表，
			 * free的file_area则直接释放掉!!!!!!后续改进。并且，不用加file_stat->file_stat_lock锁。
			 * file_stat_tiny_small已经保证不会再有进程访问，p_file_stat只有操作p_file_stat->temp链表的file_area才用加锁!!!!!!!*/
			file_area_type = get_file_area_list_status(p_file_area);
#if	 0

			if(is_cache_file)
				file_stat_other_list_file_area_solve_common(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,p_file_area,file_area_type,FILE_STAT_SMALL);
			else
				reverse_other_file_area_list_common(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,p_file_area,file_area_type,FILE_STAT_SMALL,NULL);
#else
			/*in_free、in_hot、in_refault属性的file_area必须强制移动到新的file_stat的free、hot、refault链表。file_stat_other_list_file_area_solve_common
			 * 函数并不会把这些file_area必须强制移动到新的small_file_stat的other链表。这导致，这些没有in_temp属性的file_area下边list_splice_init()
			 * 被移动到新的small_file_stat->temp链表，将来在这个temp链表遍历到没有in_temp属性的file_area就会crash*/
			list_move(&p_file_area->file_area_list,&p_file_stat_small->file_area_other);
			/*file_area如果没有in_hot、in_free、in_refault、in_mapcount属性中的一种则触发crash*/
			if(0 == (p_file_area->file_area_state & FILE_AREA_LIST_MASK))
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in any file_area_list\n",__func__,(u64)p_file_stat_tiny_small,(u64)p_file_area,p_file_area->file_area_state);
#endif			
		}
	}
	/*老的file_stat_tiny_small已经失效了，新的file_stat_small已经启用，故其他进程可能并发向file_stat_small->temp链表添加file_area，因此要spin_lock加锁*/
	spin_lock(&p_file_stat_small->file_stat_base.file_stat_lock);
	/*把file_stat_tiny_small->temp链表上的temp属性的file_area移动到新的file_stat的temp链表上*/
	list_splice_init(&p_file_stat_tiny_small->file_stat_base.file_area_temp,&p_file_stat_small->file_stat_base.file_area_temp);
	spin_unlock(&p_file_stat_small->file_stat_base.file_stat_lock);
	return scan_file_area_count;
}
static inline unsigned int move_small_file_area_to_normal_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_small *p_file_stat_small,struct file_stat *p_file_stat,char is_cache_file)
{
	unsigned int scan_file_area_count = 0;
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int file_area_type;

	/*从链表尾开始遍历file_area，有in_refault、in_free、in_hot属性的则移动到新的file_stat对应的链表，最多只遍历640个，即便
	  file_stat_tiny_small->temp链表上可能因短时间大量访问pagecahce而导致有很多的file_area*/
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_small->file_area_other,file_area_list){
		if(++ scan_file_area_count > NORMAL_TEMP_FILE_AREA_COUNT_LEVEL)
			break;
		if(!file_area_in_temp_list(p_file_area)){
			/* 其实这里就可以判断这些hot、refault的file_area，如果长时间没访问则移动回file_stat->warm链表，
			 * free的file_area则直接释放掉!!!!!!后续改进。并且，不用加file_stat->file_stat_lock锁。
			 * file_stat_tiny_small已经保证不会再有进程访问，p_file_stat只有操作p_file_stat->temp链表的file_area才用加锁!!!!!!!*/
			file_area_type = get_file_area_list_status(p_file_area);
#if 0	
			if(is_cache_file)
				file_stat_other_list_file_area_solve_common(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area,file_area_type,FILE_STAT_NORMAL);
			else
				reverse_other_file_area_list_common(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area,file_area_type,FILE_STAT_NORMAL,NULL);
#else
			/*in_free、in_hot、in_refault属性的file_area必须强制移动到新的file_stat的free、hot、refault链表。file_stat_other_list_file_area_solve_common
			 * 函数并不会把这些file_area必须强制移动到新的file_stat的free、hot、refault链表。这导致，这些没有in_temp属性的file_area下边list_splice_init()
			 * 被移动到新的file_stat->temp链表，将来在这个temp链表遍历到没有in_temp属性的file_area就会crash*/
			switch (file_area_type){
				case (1 << F_file_area_in_hot_list):
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
					p_file_stat->file_area_hot_count ++;
					break;
				case (1 << F_file_area_in_refault_list):
					//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);多层warm机制引入refault file_area移动到file_area_hot链表
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
					break;
					/*file_area同时具备in_free的in_refault属性的，那是in_free的被访问update函数设置了in_refault，要移动到file_stat->in_free链表，
					 *因为这种属性的file_area就是在in_free链表，将来执行file_stat_other_list_file_area_solve_common()会把它移动到in_refault链表*/
				case (1 << F_file_area_in_refault_list | 1 << F_file_area_in_free_list):
				case (1 << F_file_area_in_free_list):
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free);
					break;
				case (1 << F_file_area_in_mapcount_list):
					//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);新版本改为移动到hot链表
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
					p_file_stat->mapcount_file_area_count ++;
					break;
				default:
					panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x error\n",__func__,(u64)p_file_stat_small,(u64)p_file_area,p_file_area->file_area_state);
			}
#endif
		}
		/*防止循环耗时太长而适当调度*/
		cond_resched();
	}
	/*老的file_stat_small已经失效了，新的file_stat已经启用，故其他进程可能并发向file_stat->temp链表添加file_area，因此要spin_lock加锁*/
	spin_lock(&p_file_stat->file_stat_base.file_stat_lock);
	/*把file_stat_small->temp链表上的temp属性的file_area移动到新的file_stat的temp链表上*/
	list_splice_init(&p_file_stat_small->file_stat_base.file_area_temp,&p_file_stat->file_stat_base.file_area_temp);
	spin_unlock(&p_file_stat->file_stat_base.file_stat_lock);
	return scan_file_area_count;
}
static inline void old_file_stat_file_stat_status_change_to_new(struct file_stat_base *p_file_stat_base_old,struct file_stat_base *p_file_stat_base_new)
{
	int file_stat_bit = F_file_stat_in_file_area_list_end + 1;
	/* 不能直接给file_stat_status赋值，而是必须set_bit/clear_bit设置和清理bit位，否则又会触发多进程赋值file_stat_status
	 * 造成的并发问题*/
	for(;file_stat_bit < F_file_stat_invalid_start_index;file_stat_bit ++){
		if(test_bit(file_stat_bit,(unsigned long *)(&p_file_stat_base_old->file_stat_status)))
			set_bit(file_stat_bit,(unsigned long *)(&p_file_stat_base_new->file_stat_status));
		else
			clear_bit(file_stat_bit,(unsigned long *)(&p_file_stat_base_new->file_stat_status));
	}
}
static inline void old_file_stat_wait_print_file_stat(struct file_stat_base *p_file_stat_base_old)
{
	/*如果file_stat是print_file_stat，则file_stat马上要rcu del了，则必须把print_file_stat清NULL，
	 *保证将来不再使用这个马上要释放的file_stat*/
	smp_mb();
	if(p_file_stat_base_old == READ_ONCE(hot_cold_file_global_info.print_file_stat)){
		WRITE_ONCE(hot_cold_file_global_info.print_file_stat, NULL); 
		printk("%s file_stat:0x%llx status:0x%x is print_file_stat!!!\n",__func__,(u64)p_file_stat_base_old,p_file_stat_base_old->file_stat_status);
	}
	/* 这里非常关键，如果正在print_file_stat_all_file_area_info_write()中通过proc设置
	 * print_file_stat = p_file_stat_base_old，为了防止这里释放掉p_file_stat_base_old结构体后，
	 * print_file_stat_all_file_area_info_write()还在使用这个file_stat，强制进程退出
	 * print_file_stat_all_file_area_info_write()函数后，再释放这个file_stat。为什么？否则
	 * 这里释放掉file_stat后，print_file_stat_all_file_area_info_write()再使用file_stat就是无效内存访问*/
	while(atomic_read(&hot_cold_file_global_info.ref_count))
		schedule();
}
static inline void old_file_stat_change_to_new(struct file_stat_base *p_file_stat_base_old,struct file_stat_base *p_file_stat_base_new)
{
	WRITE_ONCE(p_file_stat_base_old->mapping->rh_reserved1,  (unsigned long)p_file_stat_base_new);
	smp_wmb();
	/* 加这个synchronize_rcu()是为了保证此时正读写文件，执行__filemap_add_folio->file_area_alloc_and_init()的进程
	 * 退出rcu宽限期，等新的进程再来，看到的该文件的mapping->rh_reserved1一定是新的file_area，老的file_stat就不会
	 * 再有进程访问了。目的是：后续要把老的file_stat的file_area全移动到新的file_stat的链表，是没有对老的file_stat_lock加锁，节省性能*/
    synchronize_rcu();	

	if(file_stat_in_replaced_file_base(p_file_stat_base_old))
	    panic("%s file_stat:0x%llx status:0x%x\n",__func__,(u64)p_file_stat_base_old,p_file_stat_base_old->file_stat_status);
		
	spin_lock(&p_file_stat_base_old->file_stat_lock);
	spin_lock(&p_file_stat_base_new->file_stat_lock);
	/*设置老的file_stat是delete状态，后续这个老的file_stat就要被新的file_stat替代了*/
	//隐藏bug，这里设置delete，如果此时正好有进程并发读写文件刚执行__filemap_add_folio函数，用到p_file_stat_base_old，发现有delte标记就会crash???去除了
	//set_file_stat_in_delete_base(p_file_stat_base_old);
	set_file_stat_in_replaced_file_base(p_file_stat_base_old);
	//p_file_stat_base_old->mapping->rh_reserved1 =  (unsigned long)p_file_stat_base_new;
    p_file_stat_base_new->mapping = p_file_stat_base_old->mapping; 

    /* 必须要累加，因为可能新的和老的file_stat并发分配file_area，并令各自的file_area_count加1，因此
	 * 此时的p_file_stat_base_new->file_area_count可能大于0，故不能直接赋值*/
    //p_file_stat_base_new->file_area_count = p_file_stat_base_old->file_area_count;
    p_file_stat_base_new->file_area_count += p_file_stat_base_old->file_area_count;
	/*现在只有old tiny small或small 文件转成更大的文件，file_stat_small和file_stat_tiny_small结构体没有file_area_hot_count
	 *和file_area_hot_count成员，后续如果old文件有file_area_hot_count和mapcount_file_area_count成员，这两行代码就不能注释了*/
    //p_file_stat_base_new->file_area_hot_count += p_file_stat_base_old->file_area_hot_count;
    //p_file_stat_base_new->mapcount_file_area_count += p_file_stat_base_old->mapcount_file_area_count;
    p_file_stat_base_new->file_area_count_in_temp_list += p_file_stat_base_old->file_area_count_in_temp_list;
    
	//p_file_stat_base_new->max_file_area_age = p_file_stat_base_old->max_file_area_age;
    p_file_stat_base_new->recent_access_age = p_file_stat_base_old->recent_access_age;
    p_file_stat_base_new->recent_traverse_age = p_file_stat_base_old->recent_traverse_age;

	/*赋值老的file_stat_status给新的file_stat*/
    old_file_stat_file_stat_status_change_to_new(p_file_stat_base_old,p_file_stat_base_new);

	spin_unlock(&p_file_stat_base_new->file_stat_lock);
	spin_unlock(&p_file_stat_base_old->file_stat_lock);
#if 0
	/*如果file_stat是print_file_stat，则file_stat马上要rcu del了，则必须把print_file_stat清NULL，
	 *保证将来不再使用这个马上要释放的file_stat*/
	smp_mb();
	if(p_file_stat_base_old == hot_cold_file_global_info.print_file_stat){
		hot_cold_file_global_info.print_file_stat = NULL; 
		printk("%s file_stat:0x%llx status:0x%x is print_file_stat!!!\n",__func__,(u64)p_file_stat_base_old,p_file_stat_base_old->file_stat_status);
	}
	/* 这里非常关键，如果正在print_file_stat_all_file_area_info_write()中通过proc设置
	 * print_file_stat = p_file_stat_base_old，为了防止这里释放掉p_file_stat_base_old结构体后，
	 * print_file_stat_all_file_area_info_write()还在使用这个file_stat，强制进程退出
	 * print_file_stat_all_file_area_info_write()函数后，再释放这个file_stat。为什么？否则
	 * 这里释放掉file_stat后，print_file_stat_all_file_area_info_write()再使用file_stat就是无效内存访问*/
	while(atomic_read(&hot_cold_file_global_info.ref_count))
		schedule();
#else
    /* 把新的file_stat指针赋值给文件的mapping->rh_reserved1，后续进程读写文件从文件mapping->rh_reserved1
	 * 获取到的是新的file_stat，最后决定这个赋值放到加锁外边，尽可能早的赋值*/
	old_file_stat_wait_print_file_stat(p_file_stat_base_old);
#endif
	/*这里执行后，马上调用rcu del异步释放掉这个file_stat*/
}
/*tiny_small的file_area个数如果超过阀值则转换成small或普通文件。这里有个隐藏很深的问题，
 *   本身tiny small文件超过64个file_area就要转换成small文件，超过640个就要转换成普通文件。但是，如果tiny small
 *   文件如果长时间没有被遍历到，文件被大量读写，file_stat_tiny_small->temp链表上的file_area可能上万个。这些file_area
 *   有in_temp属性的，也有in_refault、in_hot、in_free等属性的。异步内存回收线程遍历到这个tiny small，
 *   肯定要把in_refault、in_hot、in_free的file_area移动到新的file_stat->refault、hot、free链表上，
 *   那岂不是要遍历这上万个file_area，找出这些file_area，那太浪费性能了！
 *
 *   这个问题单凭想象，觉得很难处理，但是深入这个场景后，把变化细节一点一点列出来，发现这个很高解决，大脑会因感觉欺骗你。
 *   
 *   1：tiny small现在有64个file_area在file_stat_tiny_small->temp，其中有多个in_refault、in_hot、in_free的file_area
 *   2：进程读写多了，tiny small新增了1万个file_area，都添加到了file_stat_tiny_small->temp链表，但是都是添加到了
 *      该链表头，这点由file_area_alloc_and_init()函数保证，这点非常重要。
 *   3：异步内存回收线程遍历到这个tiny small file，发现需要转换成normal文件。只用遍历file_stat_tiny_small->temp链表
 *      尾的64个file_area，看是否有in_refault、in_hot、in_free的file_area，然后移动到新的file_stat->refault、hot、free
 *      链表。
 *
 *      问题来了，怎么保证遍历file_stat_tiny_small->temp尾64个file_area就行了，不用再向前遍历其他file_area?
 *      因为"把file_stat_tiny_small->temp链表上的file_area设置in_refault、in_hot、in_free属性" 和 
 *      "发现tiny small文件的file_area个数超过64个(上万个)而转换成normal文件"都是异步内存回收线程串行进行的。
 *      我只要保证"发现tiny small文件的file_area个数超过64个(上万个)而转换成normal文件"放到 
 *      "把file_stat_tiny_small->temp链表上的file_area设置in_refault、in_hot、in_free属性"前边执行。
 *      就一定能做到：
 *      3.1：只有tiny small文件file_area个数在64个以内时，异步内促回收线程会根据冷热程度、访问频率把
 *      file_stat_tiny_small->temp上file_area设置in_refault、in_hot、in_free属性，
 *      3.2：后续新增的file_area只会添加到file_stat_tiny_small->temp链表头。异步内存回收线程再次运行，
 *      发现file_area个数超过64个，但只用从file_stat_tiny_small->temp链表的64个file_area查找
 *      in_refault、in_hot、in_free属性的file_area即可。
 *
 *   发现一个隐藏很深的bug。当tiny_small_file的file_area个数超过64个后，后续很多file_area被频繁访问而设置in_hot标记。
 *   这种file_area同时具有in_temp和in_refault属性。并且这些file_area都靠近链表头，将来tiny_small_file转换成small file
 *   时，只链表链表尾的64个file_area，那这些file_area将无法被移动到small_file_stat->other链表，而残留在
 *   small_file_stat->temp链表，如果这些没关系，small_file_stat->temp链表上的file_area同时有in_temp和in_refault属性
 *   是正常的。
 *
 *   但是，有个其他问题，当file_stat_temp_list_file_area_solve()里正遍历这种in_temp和in_hot属性的file_area_1时，
 *   clear_file_area_in_temp_list()清理掉in_temp属性。然后执行file_stat_other_list_file_area_solve_common()检测
 *   该file_area是否需长时间没访问而需要降级。没有降低的话，此时该文件正好被频繁访问，生成了很多新的file_area
 *   添加到了tiny_small_file_stat->temp链表头。然后，在退出file_stat_temp_list_file_area_solve()前这个file_area_1
 *   就要被移动到tiny_small_file_stat->temp链表头。这样就出问题了，这个只有in_hot属性的file_area_1就不在
 *   tiny_small_file_stat->temp链表尾的64个file_area了，而是在链表头。将来这个tiny_small_file转成small_file，
 *   这个file_area_1残留在small_file_stat->temp链表，然后file_stat_temp_list_file_area_solve()遍历这个
 *   small_file_stat->temp链表的file_area_1时，因为只有in_hot属性，导致crash。解决办法是，
 *   file_stat_temp_list_file_area_solve()函数里，遍历tiny_small_file_stat->temp链表上的in_temp和in_hot属性
 *   的file_area，如果此时该文件的file_area个数超过64，就不再clear_file_area_in_temp_list()清理掉in_temp属性。
 *   是该file_area保持in_temp和in_hot属性，将来移动到small_file_stat->temp，就不会再有问题了，
 *   file_stat->temp链表允许file_area有in_temp和in_hot属性。
 *   */
static int  can_tiny_small_file_change_to_small_normal_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_tiny_small *p_file_stat_tiny_small,char is_cache_file)
{
	int ret = 0;
	struct file_stat_base *p_file_stat_base_tiny_small = &p_file_stat_tiny_small->file_stat_base;

	/*file_area_alloc_and_init()中因该文件的file_area个数超过阈值而设置了in_tiny_small_to_tail标记，然后移动到链表尾，这里清理掉标记*/
	if(file_stat_in_tiny_small_to_tail_base(p_file_stat_base_tiny_small))
		clear_file_stat_in_tiny_small_to_tail_base(p_file_stat_base_tiny_small);

	/*file_stat_tiny_small的file_area个数超过普通文件file_area个数的阀值，直接转换成普通文件file_stat*/
	if(p_file_stat_base_tiny_small->file_area_count > NORMAL_TEMP_FILE_AREA_COUNT_LEVEL){
		struct file_stat *p_file_stat;
		struct file_stat_base *p_file_stat_base;
		spinlock_t *p_global_lock;

		if(is_cache_file){
			p_global_lock = &p_hot_cold_file_global->global_lock;
			//p_file_stat_base = file_stat_alloc_and_init(p_file_stat_base_tiny_small->mapping,FILE_STAT_NORMAL,1);
		}
		else{
			p_global_lock = &p_hot_cold_file_global->mmap_file_global_lock;
			//p_file_stat_base = add_mmap_file_stat_to_list(p_file_stat_base_tiny_small->mapping,FILE_STAT_NORMAL,1);
		}

		p_file_stat_base = file_stat_alloc_and_init_other(p_file_stat_base_tiny_small->mapping,FILE_STAT_NORMAL,1,is_cache_file,file_stat_in_writeonly_base(&p_file_stat_tiny_small->file_stat_base));


		//clear_file_stat_in_file_stat_small_file_head_list_base(p_file_stat_tiny_small);
		//set_file_stat_in_file_stat_temp_file_head_list(p_file_stat_tiny_small);
		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

		/*有个隐藏很深的问题，新旧file_stat替换时，如果此时正好有进程并发读写文件，从mapping->rh_reserved1取出的
		 *file_stat依然是老的p_file_stat_tiny_small，然后分配新的file_area还是添加到老的p_file_stat_tiny_small->temp
		 *链表，然后下边把p_file_stat_tiny_small释放掉，那岂不是要丢失掉刚才新分配的file_area?解决就要靠file_stat_lock锁。
		 *
		 *并发1：异步内存回收线程执行can_tiny_small_filie_change_to_small_normal_file()函数，file_stat_tiny_small的file_area
		   个数超过阀值，分配新的file_stat替换老的file_stat_tiny_small

		   file_stat_alloc_and_init()分配新的file_stat，p_file_stat
		   file_stat_tiny_small->mapping->rh_reserved1 =  p_file_stat//使老的mapping->rh_reserved1指向新的file_stat
		   set_file_stat_in_delete(p_file_stat_tiny_small);//设置file_stat in delete状态
		   smp_wmb()//内存屏障保证先后顺序
		   p_file_stat_tiny_small->file_stat_lock加锁
		   p_file_stat->file_stat_lock加锁
		   把老p_file_stat_tiny_small->temp链表上的file_area移动到新p_file_stat->temp链表
		   p_file_stat_tiny_small->file_stat_lock解锁
		   p_file_stat->file_stat_lock解锁
		   //rcu异步释放掉老的file_stat_tiny_small
		   call_rcu(&p_file_stat_tiny_small->i_rcu, i_file_stat_tiny_small_callback_small_callback);

		  并发2：进程此时读写文件，执行 __filemap_add_folio->file_area_alloc_and_init()分配新的file_area并添加到老的
		  file_stat_tiny_small->temp链表

		   //有了rcu保证以下代码file_stat_tiny_small结构不会被释放，可以放心使用file_stat_tiny_small，这点很重要
		   rcu_read_lock();//见 __filemap_add_folio()函数
		   //如果mapping->rh_reserved1被替换为新的file_stat，这个内存屏障下次执行到这里获取到的是最新的file_stat，跟并发1的smp_wmb()成对
	       smp_rmb(); 

		   //从文件mapping->rh_reserved1取出file_stat，可能是老的file_stat_tiny_small或新分配的p_file_stat
		   p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
		   执行file_area_alloc_and_init(p_file_stat_base)到函数   //p_file_stat_base可能是老的file_stat_tiny_small或新的p_file_stat
		   {
              //p_file_stat_tiny_small->file_stat_lock加锁 或者 p_file_stat->file_stat_lock加锁
              spin_lock(&p_file_stat_base->file_stat_lock);
			  //如果p_file_stat_tiny_small已经被删除了
		      if(file_stat_in_delete(p_file_stat_base))
			  {
			      //获取新分配的file_stat并分配给p_file_stat_base
			      p_file_stat = p_file_stat_base->mapping->rh_reserved1;
                  p_file_stat_base = &p_file_stat->file_stat_base;
			  }
		      分配新的file_area并移动到p_file_stat_base->temp链表
		      //p_file_stat_tiny_small->file_stat_lock解锁 或者 p_file_stat->file_stat_lock解锁
			  spin_unlock(&p_file_stat_base->file_stat_lock);
		   }

		   rcu_read_unlock();

		   以上的并发设计完美解决了并发问题，最极端的情况：并发2先执行"p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1"
		   获得老的file_stat_tiny_small。此时并发1执行"file_stat_tiny_small->mapping->rh_reserved1 =  p_file_stat"
		   把新分配的file_stat赋值给这个文件的mapping->rh_reserved1，此时新分配的file_stat并赋值给这个文件并生效。
 
		   然后并发2在file_area_alloc_and_init()函数先抢占p_file_stat_tiny_small锁，但是里边
		   if(file_stat_in_delete(p_file_stat_base))成立，于是执行"p_file_stat = p_file_stat_base->mapping->rh_reserved1"
		   获取新分配的file_stat。接着执行"分配新的file_area并移动到p_file_stat_base->temp链表"，就是把file_area添加到新
		   分配的file_stat->temp链表。如果并发1先抢占p_file_stat_tiny_small锁，情况也是一样。如果并发1先
		   执行"file_stat_tiny_small->mapping->rh_reserved1 =  p_file_stat"，接着并发2执行
		   "p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1"，那就并没有问题了，
		   因为此时获取到的就是新的file_stat，并发点就在这里。如果并发2先执行了，分配了file_area并添加到
		   p_file_stat_tiny_small->temp链表。接着并发2执行了，只用把p_file_stat_tiny_small->temp链表已有的file_area移动
		   到新的file_stat->temp链表就行了。以上操作在old_file_stat_change_to_new()函数实现
		 */
		/*老的file_stat的主要成员赋值给新的file_stat*/
		old_file_stat_change_to_new(&p_file_stat_tiny_small->file_stat_base,&p_file_stat->file_stat_base);
		/*老的file_stat的各种属性的file_area移动到新的file_stat。注意，到这里必须保证老的file_stat不再用进程
		 *会访问，old_file_stat_change_to_new()里的synchronize_rcu()保证了这点*/
		move_tiny_small_file_area_to_normal_file(p_hot_cold_file_global,p_file_stat_tiny_small,p_file_stat,is_cache_file);

		/*注意，file_stat_alloc_and_init()里分配新的file_stat并赋值给mapping->rh_reserved1后，新的进程读写该文件，
		 * 执行到update_hot_file函数，才会使用新的file_stat，而不是老的file_stat_small。这里加个内存屏障，保证
		 * 对mapping->rh_reserved1立即生效，否则会导致还在使用老的file_stat_small，统计文件冷热信息会丢失!!!!!!!!!!!重点考虑并发问题!!!!!!!!!!!!!!!!*/
		//smp_wmb();

		spin_lock(p_global_lock);
		/*可能此时该文件被iput delete了，要防护,老的和新的file_stat都可能会被并发删除。没有必要再判断新的file_stat的是否delete了。两个文件不应该互相影响*/
		if(!file_stat_in_delete_base(p_file_stat_base_tiny_small) /*&& !file_stat_in_delete(p_file_stat)*/){
			/* 这里rcu_read_lock，然后下边call_rcu()就不会释放掉file_stat结构体了，等回到get_file_area_from_file_stat_list()函数，
			 * 确保该file_stat不会再被使用时再rcu_read_unlock放开*/
			rcu_read_lock();
			ret = 1;

			/*该file_stat从老的global链表中剔除，下边call_rcu异步释放掉*/

			/*隐藏bug，如果此时有进程在__filemap_add_folio函数()后期正使用p_file_stat_base_old，这里却把它释放了，而__filemap_add_folio函数没有rcu_read_lock保护，导致__filemap_add_folio函数用的p_file_stat_base_old。__filemap_add_folio()加了rcu_read_lock()防止这种情况*/
			list_del_rcu(&p_file_stat_base_tiny_small->hot_cold_file_list);
			/* 隐藏bug，如果这里释放掉file_stat，但是返回到get_file_area_from_file_stat_list()函数后，还要使用这个
			 * file_stat，那就是非法内存访问了。于是把call_rcu异步释放file_stat的代码放到get_file_area_from_file_stat_list()
			 * 函数最后，确保该file_stat不会再被使用时再call_rcu异步释放掉。算了，最后决定用提前rcu_read_lock防护就行了*/
			call_rcu(&p_file_stat_base_tiny_small->i_rcu, i_file_stat_tiny_small_callback);
		}
		spin_unlock(p_global_lock);

	}
	/*file_stat_tiny_small的file_area个数仅超过 但没有超过普通文件file_area个数的阀值，只是直接转换成small文件file_stat*/
	else if(p_file_stat_base_tiny_small->file_area_count > SMALL_FILE_AREA_COUNT_LEVEL){
		struct file_stat_small *p_file_stat_small;
		struct file_stat_base *p_file_stat_base;
		spinlock_t *p_global_lock;

		if(is_cache_file){
			p_global_lock = &p_hot_cold_file_global->global_lock;
			//p_file_stat_base = file_stat_alloc_and_init(p_file_stat_base_tiny_small->mapping,FILE_STAT_SMALL,1);
		}
		else{
			p_global_lock = &p_hot_cold_file_global->mmap_file_global_lock;
			//p_file_stat_base = add_mmap_file_stat_to_list(p_file_stat_base_tiny_small->mapping,FILE_STAT_SMALL,1);
		}

		p_file_stat_base = file_stat_alloc_and_init_other(p_file_stat_base_tiny_small->mapping,FILE_STAT_SMALL,1,is_cache_file,file_stat_in_writeonly_base(&p_file_stat_tiny_small->file_stat_base));

		//clear_file_stat_in_file_stat_small_file_head_list_base(p_file_stat_tiny_small);
		//set_file_stat_in_file_stat_temp_file_head_list(p_file_stat_tiny_small);
		p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);

		/*注意，file_stat_alloc_and_init()里分配新的file_stat并赋值给mapping->rh_reserved1后，新的进程读写该文件，
		 * 执行到update_hot_file函数，才会使用新的file_stat，而不是老的file_stat_small。这里加个内存屏障，保证
		 * 对mapping->rh_reserved1立即生效，否则会导致还在使用老的file_stat_small，统计文件冷热信息会丢失!!!!!!!!!!!重点考虑并发问题!!!!!!!!!!!!!!!!*/
		//smp_wmb();

		/*老的file_stat的主要成员赋值给新的file_stat*/
		old_file_stat_change_to_new(&p_file_stat_tiny_small->file_stat_base,&p_file_stat_small->file_stat_base);
		/*老的file_stat的各种属性的file_area移动到新的file_stat*/
		move_tiny_small_file_area_to_small_file(p_hot_cold_file_global,p_file_stat_tiny_small,p_file_stat_small,is_cache_file);

		spin_lock(p_global_lock);
		/*可能此时该文件被iput delete了，要防护*/
		if(!file_stat_in_delete_base(p_file_stat_base_tiny_small) /*&& !file_stat_in_delete_base(&p_file_stat_small->file_stat_base)*/){
			/* 这里rcu_read_lock，然后下边call_rcu()就不会释放掉file_stat结构体了，等回到get_file_area_from_file_stat_list()函数，
			 * 确保该file_stat不会再被使用时再rcu_read_unlock放开*/
			rcu_read_lock();
			ret = 1;

			/*该file_stat从老的global链表中剔除，下边call_rcu异步释放掉*/
			list_del_rcu(&p_file_stat_base_tiny_small->hot_cold_file_list);
			/* 隐藏bug，如果这里释放掉file_stat，但是返回到get_file_area_from_file_stat_list()函数后，还要使用这个
			 * file_stat，那就是非法内存访问了。于是把call_rcu异步释放file_stat的代码放到get_file_area_from_file_stat_list()
			 * 函数最后，确保该file_stat不会再被使用时再call_rcu异步释放掉。算了，最后决定用提前rcu_read_lock防护就行了*/
			call_rcu(&p_file_stat_base_tiny_small->i_rcu, i_file_stat_tiny_small_callback);
		}
		spin_unlock(p_global_lock);
	}
	return ret;
}
/*small的file_area个数如果超过阀值则转换成普通文件。这里有个隐藏很深的问题，
 *   本身small文件超过640个就要转换成普通文件。但是，如果small
 *   文件如果长时间没有被遍历到，文件被大量读写，该文件将有的file_area可能上万个file_area。这些file_area
 *   有in_temp属性的，停留在file_stat_small->temp，也有in_refault、in_hot、in_free等属性的，停留在
 *   file_stat_small->otehr链表，那file_stat_small->otehr链表上就可能有上万个file_area呀。
 *   那异步内存回收线程遍历到这个small，肯定要把in_refault、in_hot、in_free的file_area移动到新的
 *   file_stat->refault、hot、free链表上，那岂不是要遍历这上万个file_area，找出这些file_area，那太浪费性能了！
 *
 *   多虑了，最多只用遍历file_stat_small->otehr链表上的640个file_area就行了。因为只要保证
 *   "发现small文件的file_area个数超过640而转换成normal文件"放到
 *   "把file_stat_small->temp链表上的file_area设置in_refault、in_hot、in_free属性并移动到file_stat_small->other"
 *    前边执行就可以了。
 *   
 *    3.1：small文件file_area个数在640个以内时，异步内促回收线程会根据冷热程度、访问频率把
 *      file_stat_small->temp上file_area设置in_refault、in_hot、in_free属性并移动到file_stat_small->other链表，
 *      file_stat_small->other链表最多只有640个file_area。
 *    3.2：后续新增的file_area只会添加到file_stat_small->temp链表头。异步内存回收线程再次运行，发现file_area个数
 *      超过640个，但只用从file_stat_small->other链表的最多遍历640个file_area，
 *      按照in_refault、in_hot、in_free属性而移动到新的file_stat->refault、hot、free链表.
 */
static int can_small_file_change_to_normal_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_small *p_file_stat_small,char is_cache_file)
{
	int ret = 0;
	struct file_stat_base *p_file_stat_base_small = &p_file_stat_small->file_stat_base;

	/*file_stat_tiny_small的file_area个数超过普通文件file_area个数的阀值，直接转换成普通文件file_stat*/
	if(p_file_stat_base_small->file_area_count > NORMAL_TEMP_FILE_AREA_COUNT_LEVEL){
		struct file_stat *p_file_stat;
		struct file_stat_base *p_file_stat_base;
		spinlock_t *p_global_lock;

		if(is_cache_file){
			p_global_lock = &p_hot_cold_file_global->global_lock;
			//p_file_stat_base = file_stat_alloc_and_init(p_file_stat_base_small->mapping,FILE_STAT_NORMAL,1);
		}
		else{
			p_global_lock = &p_hot_cold_file_global->mmap_file_global_lock;
			//p_file_stat_base = add_mmap_file_stat_to_list(p_file_stat_base_small->mapping,FILE_STAT_NORMAL,1);
		}
		/*分配新的file_stat并赋值给文件的mapping->rh_reserved1*/
		p_file_stat_base = file_stat_alloc_and_init_other(p_file_stat_base_small->mapping,FILE_STAT_NORMAL,1,is_cache_file,file_stat_in_writeonly_base(&p_file_stat_small->file_stat_base));

		//clear_file_stat_in_file_stat_small_file_head_list_base(p_file_stat_small);
		//set_file_stat_in_file_stat_temp_file_head_list(p_file_stat_small);
		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

		/*注意，file_stat_alloc_and_init()里分配新的file_stat并赋值给mapping->rh_reserved1后，新的进程读写该文件，
		 * 执行到update_hot_file函数，才会使用新的file_stat，而不是老的file_stat_small。这里加个内存屏障，保证
		 * 对mapping->rh_reserved1立即生效，否则会导致还在使用老的file_stat_small，统计文件冷热信息会丢失!!!!!!!!!!!重点考虑并发问题!!!!!!!!!!!!!!!!*/
		//smp_wmb();

		/*老的file_stat的主要成员赋值给新的file_stat*/
		old_file_stat_change_to_new(&p_file_stat_small->file_stat_base,&p_file_stat->file_stat_base);
		/*老的file_stat的各种属性的file_area移动到新的file_stat*/
		move_small_file_area_to_normal_file(p_hot_cold_file_global,p_file_stat_small,p_file_stat,is_cache_file);

		spin_lock(p_global_lock);
		/*可能此时该文件被iput delete了，要防护*/
		if(!file_stat_in_delete_base(p_file_stat_base_small) /*&& !file_stat_in_delete(p_file_stat)*/){
			/* 这里rcu_read_lock，然后下边call_rcu()就不会释放掉file_stat结构体了，等回到get_file_area_from_file_stat_list()函数，
			 * 确保该file_stat不会再被使用时再rcu_read_unlock放开*/
			rcu_read_lock();
			ret = 1;

			/*该file_stat从老的global链表中剔除，下边call_rcu异步释放掉*/
			list_del_rcu(&p_file_stat_base_small->hot_cold_file_list);
			/* 有个隐藏很深的问题，是否有可能老的file_stat正在update函数里被访问，这里却把file_sata结构体释放了。
			 * 那就会导致非法内存访问？不可能，一方面update函数里都有rcu_read_lock防护，这里是call_rcu异步释放。
			 * 并且，上边的old_file_stat_change_to_new()做好了防护：老的file_stat不会再被update函数访问才会退出*/

			/* 隐藏bug，如果这里释放掉file_stat，但是返回到get_file_area_from_file_stat_list()函数后，还要使用这个
			 * file_stat，那就是非法内存访问了。于是把call_rcu异步释放file_stat的代码放到get_file_area_from_file_stat_list()
			 * 函数最后，确保该file_stat不会再被使用时再call_rcu异步释放掉。算了，最后决定用提前rcu_read_lock防护就行了*/
			call_rcu(&p_file_stat_base_small->i_rcu, i_file_stat_small_callback);
		}
		spin_unlock(p_global_lock);

	}
	return ret;
}
static inline int cache_file_change_to_mmap_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int file_type)
{
	/* 把在global file_stat_temp_head链表但实际是mmap的文件file_stat从global file_stat_temp_head链表剔除，
	 * 然后添加到global mmap_file_stat_tiny_small_temp_head链表。注意，这里仅仅针对so库、可执行文件等elf文件，这些文件
	 * 都是先read系统调用读取文件头，被判定为cache文件，然后再mmap映射了。这种文件第一次扫描到，肯定处于global 
	 * temp链表，则把他们移动到global mmap_file_stat_tiny_small_temp_head链表。有些cache文件读写后，经历内存回收
	 * 在file_stat->warm、hot、free链表都已经有file_area了，就不再移动到global mmap_file_stat_tiny_small_temp_head链表了。
	 * 因此引入if(p_file_stat_tiny_small->file_area_count == p_file_stat_tiny_small->file_area_count_in_temp_list)这个判断，因为最初
	 * 二者肯定相等。当然，也有一种可能，就是过了很长时间，经历file_area内存回收、释放和再分配，二者还是相等。
	 * 无所谓了，就是把一个file_stat移动到global mmap_file_stat_tiny_small_temp_head而已，反正这个文件也是建立的mmap映射。
	 * 最后，再注意一点，只有在global temp_list的file_stat才能这样处理。*/

	/*还有一个关键隐藏点，怎么确保这种file_stat一定是global->tiny_small_file链表上，而不是其他global链表上？首先，
	 *任一个文件的创建时一定添加到global->tiny_small_file链表上，然后异步内存线程执行到该函数时，这个文件file_stat一定
	 *在global->tiny_small_file链表上，不管这个文件的file_area有多少个。接着,才会执行把该file_stat移动到其他global链表的代码
	 * 
	 *为什么要限定只有tiny small file才能从cache file转成mmap file？想想没有这个必要，实际测试有很多normal cache文件实际是mmap
	 *文件。只要限定file_area都是in_temp的file_area就可以了
	 */

	/* 有个隐藏很深的bug，如果此时正在使用file_stat的mapping的，但是给文件inode被iput了，实际测试遇到过，导致crash，要file_inode_lock
	 * 防护inode被释放。这个加锁操作放到get_file_area_from_file_stat_list()函数里了*/

	/*最新改进，之前限制只有file_area全都是in_temp_list的file_stat才能转成mmap file_stat，不这样的话，转成过程，就要把老的file_stat->free、hot
	 * 、refault等链表上的file_area要一个个遍历，判断属性，然后移动到新的mmap file_stat的对应的file_stat->free、hot、refault链表，
	 * 太浪费性能。现在cache和mmap文件的处理合二为一了，不再做这个限制，原来file_area是什么属性，到新的mmap file_stat依然是什么属性*/
	if(mapping_mapped((struct address_space *)p_file_stat_base->mapping)/* && (p_file_stat_base->file_area_count == p_file_stat_base->file_area_count_in_temp_list)*/){
		//scan_move_to_mmap_head_file_stat_count ++;
		cache_file_stat_move_to_mmap_head(p_hot_cold_file_global,p_file_stat_base,file_type);
		return 1;
	}
	return 0;
}
int check_file_stat_is_valid(struct file_stat_base *p_file_stat_base,unsigned int file_stat_list_type,char is_cache_file)
{
	if(is_cache_file && !file_stat_in_cache_file_base(p_file_stat_base))
	    panic("%s file_stat:0x%llx status:0x%x is not cache file error\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
	else if(!is_cache_file && !file_stat_in_mmap_file_base(p_file_stat_base))
	    panic("%s file_stat:0x%llx status:0x%x is not mmap file error\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

	/* 到这里， file_stat可能被iput()并发释放file_stat，标记file_stat delete，但是不会清理file_stat in temp_head_list状态。
	 * 因此这个if不会成立*/
	switch (file_stat_list_type){
		case F_file_stat_in_file_stat_tiny_small_file_head_list:
			/*file_stat此时可能被方法iput()释放delete，但file_stat的delete的标记与file_stat in_temp_list 是共存的，不干扰file_stat in_temp_list的判断*/
			if(!file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_tiny_small_file_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_tiny_small status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

			break;
		case F_file_stat_in_file_stat_tiny_small_file_one_area_head_list:
			/*file_stat此时可能被方法iput()释放delete，但file_stat的delete的标记与file_stat in_temp_list 是共存的，不干扰file_stat in_temp_list的判断*/
			if(!file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base) || file_stat_in_file_stat_tiny_small_file_one_area_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_tiny_small_one_area status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

			break;
		case F_file_stat_in_file_stat_small_file_head_list:
			/*file_stat此时可能被方法iput()释放delete，但file_stat的delete的标记与file_stat in_temp_list 是共存的，不干扰file_stat in_temp_list的判断*/
			if(!file_stat_in_file_stat_small_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_small_file_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_small status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		case F_file_stat_in_file_stat_temp_head_list:
			/*file_stat此时可能被方法iput()释放delete，但file_stat的delete的标记与file_stat in_temp_list 是共存的，不干扰file_stat in_temp_list的判断*/
			if(!file_stat_in_file_stat_temp_head_list_base(p_file_stat_base) || file_stat_in_file_stat_temp_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_temp_head status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		case F_file_stat_in_file_stat_middle_file_head_list:
			if(!file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_middle_file_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_middle_file_head status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		case F_file_stat_in_file_stat_large_file_head_list:
			if(!file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_large_file_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_large_file_head status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		/*存在一种情况，当writeonly文件被移动到global writeonly链表后，被读了，然后清理掉了in_writeonly标记，此时执行下边的判断就会crash，故要去除!file_stat_in_writeonly_base判断*/	
		case F_file_stat_in_file_stat_writeonly_file_head_list:
			if(!file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_writeonly_file_head_list_error_base(p_file_stat_base) /*|| !file_stat_in_writeonly_base(p_file_stat_base)*/)
				panic("%s file_stat:0x%llx not int file_stat_writeonly_file_head status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;

		default:	
			panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);
			break;
	}
	return 0;
}
static inline unsigned int one_file_area_file_stat_tiny_small_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,char is_cache_file)
{
	spinlock_t *p_global_lock;
	int ret = 0;

	if(is_cache_file)
		p_global_lock = &p_hot_cold_file_global->global_lock;
	else
		p_global_lock = &p_hot_cold_file_global->mmap_file_global_lock;

	/* global tiny small链表上只有一个file_area的file_stat移动到global tiny_small_one_area链表。
	 * global tiny_small_one_area链表上的file_stat，如果个数大于一定数目则移动回global tiny small链表*/
	if(file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base)){
		/*file_stat的file_area_count必须大于0，等于0时要移动到global zero file_area链表*/
		if(p_file_stat_base->file_area_count > 0 && p_file_stat_base->file_area_count <= TINY_SMALL_TO_TINY_SMALL_ONE_AREA_LEVEL
				/*&& p_hot_cold_file_global->globe_age - p_file_stat_base->recent_access_age > 2*/){
			spin_lock(p_global_lock);
			if(!file_stat_in_delete_base(p_file_stat_base)){
				clear_file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base);
				set_file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base);

				if(is_cache_file)
					list_move(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_tiny_small_file_one_area_head);
				else
					list_move(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_tiny_small_file_one_area_head);
			}
			spin_unlock(p_global_lock);

			ret = 1;
			p_hot_cold_file_global->tiny_small_file_stat_to_one_area_count ++;
		}
	}
	else if(file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base)){
		if(p_file_stat_base->file_area_count > TINY_SMALL_ONE_AREA_TO_TINY_SMALL_LEVEL){

			/*file_area_alloc_and_init()中因该文件的file_area个数超过阈值而设置了in_tiny_small_to_tail标记，然后移动到链表尾，这里清理掉标记*/
			if(file_stat_in_tiny_small_to_tail_base(p_file_stat_base))
				clear_file_stat_in_tiny_small_to_tail_base(p_file_stat_base);

			spin_lock(p_global_lock);
			if(!file_stat_in_delete_base(p_file_stat_base)){
				clear_file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base);
				set_file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base);
				/*移动到链表尾，尽可能快的遍历到，因为有可能是是small file_stat或大file_stat*/
				if(is_cache_file)
					list_move_tail(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_tiny_small_file_head);
				else
					list_move_tail(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_tiny_small_file_head);
			}
			spin_unlock(p_global_lock);

			ret = 1;
		}
	}
	else
		BUG();

	return ret;
}
static inline unsigned int tiny_small_file_area_move_to_global_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,char is_cache_file)
{
	spinlock_t *p_global_lock;
	struct file_area *p_file_area,*p_file_area_temp;
	struct global_file_stat *p_global_file_stat = NULL;

	/* 目前只把文件真实大小小于2M的文件的file_area转移到global_file_stat的链表。有些只写的文件，一点一点写最后增大，
	 * 最后就很大了。这也没办法，后续这种文件的file_area只能存在于global_file_stat的链表，不可逆。还有一点，这里可以
	 * 放心使用p_file_stat_base->mapping，不用担心该文件inode被释放，因为每一个文件被遍历前，都inode解锁了*/
	/*1：inode->i_dentry是NULL说明bdev inode，不是真正的文件inode，直接转成global_file_stat
	 *2: 长时间没访问的tiny small file_stat也转成global_file_stat。global_age和recent_access_age的差值搞定，只要
	   二者不相等，或者差值很小，说明至少一个周期该tiny small文件没有访问，直接移动到global_file_stat。
      3: so等只读的库文件，最开始是cache文件读取elf文件头，然后mmap建立映射。这种so文件有几百上千个，如果在转成
	  mmap文件前，被异步内存回收线程在这里移动到cache global_file_stat了，那就麻烦了。因为后续这些so文件的
	  mmap属性的file_area都要添加到cache global_file_stat了，不是移动到mmap global_file_stat。这就是问题了！针对
	  这个问题，我准备用p_hot_cold_file_global->global_age 和 p_file_stat_base->recent_access_age的差值做限制。
	  如果二者相当，说明so库文件刚被read/write访问过，还没有转成mmap文件，直接return。等再过一个周期，大概率
	  这些so文件就转成mmap文件了。然后执行到get_file_area_from_file_stat_list()函数，先执行
	  cache_file_change_to_mmap_file()把cache文件转成mmap文件，然后等下个周期再把该mmap文件移动到
	  mmap global_file_stat。但是有个问题，在第一步，so文件被当作cache文件，它可能有mmap的file_area，在执行
	  get_file_area_age时，大概率探测到file_area的page的pte access bit置位了，此时就会recent_access_age=globa_age。
	  然后下个异步内存回收周期，recent_access_age和globa_age相等，在这里还是直接return，无法移动到mmap global_file_stat。
	  不会，杞人忧天了，此时文件还是cache文件，get_file_area_age时，直接返回file_area_age，不会按照mmap file_area处理，
      去检测file_area的pte access bit。出发这个so文件的还会被read/write形式访问，导致recent_access_age=globa_age赋值。
	  这个可能性很低，so文件的elf文件头，只会read一次。
	  还有一个问题，so库这些mmap文件，page会被频繁访问，基本每个周期，都会检测到很多page的pte access bit
	  置位了，然后recent_access_age=globa_age赋值。那下边的的if每个周期都直接return，导致无法移动到
	  mmap global_file_stat。解决办法是，加个限制，只有是cache文件时才这么判断*/
	if(p_file_stat_base->mapping->host /*&& p_file_stat_base->mapping->host->i_size > 2097152*/){
		if(p_file_stat_base->mapping->host->i_size > 2097152 ||
				/*hlist_empty(&p_file_stat_base->mapping->host->i_dentry) ||*/
				(is_cache_file && (p_hot_cold_file_global->global_age - p_file_stat_base->recent_access_age < 1)))
		    return 0;
	}

	if(warm_list_printk)
		printk("%s:file_stat:0x%llx 0x%x global:%d is_cache_file:%d change to global\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,file_stat_in_global_base(p_file_stat_base),is_cache_file);

	if(!file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_tiny_small_file_head_list_error_base(p_file_stat_base))
		panic("%s file_stat:0x%llx status:0x%x error\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

	/* 重大隐藏bug!!!!!!!!，file_area会泄露。这里写代码时根本就没有考虑深入。因为如果先把file_stat_base->file_area_temp
	 * 上的file_area移动到global_file_stat->warm的链表上，然后再把global_file_stat->file_stat_base赋值给文件的
	 * mapping->rh_reserved1。这段时间内，如果发生了中断、软中断或者抢占调度，这段时间该文件读写，执行add_folio，
	 * 新分配的file_area就依然会移动到老的file_stat_base->file_area_temp链表。这些file_area就会称为无主孤魂，
	 * 下边call_rcu(&p_file_stat_base->i_rcu, i_file_stat_tiny_small_callback)释放file_stat_base后，这些
	 * file_area就永远无法管理了。因此，必须要保证global_file_stat->file_stat_base的地址赋值给该文件的
	 * mapping->rh_reserved1后，并且对所有的进程生效，然后才能list_splice_init()把file_stat_base->file_area_temp
	 * 链表上的file_area移动到global_file_stat->warm链表!!!!!!!!!!!!!!!!!!!!*/
	if(is_cache_file){
		/*spin_lock(&p_file_stat_base->file_stat_lock);
		  list_splice_init(&p_file_stat_base->file_area_temp,&p_hot_cold_file_global->global_file_stat.file_area_warm);
		  spin_unlock(&p_file_stat_base->file_stat_lock);*/

		WRITE_ONCE(p_file_stat_base->mapping->rh_reserved1, (u64)(&p_hot_cold_file_global->global_file_stat.file_stat.file_stat_base));
		p_global_lock = &p_hot_cold_file_global->global_lock;
		p_global_file_stat = &p_hot_cold_file_global->global_file_stat;
	}else{
		/*spin_lock(&p_file_stat_base->file_stat_lock);
		  list_splice_init(&p_file_stat_base->file_area_temp,&p_hot_cold_file_global->global_mmap_file_stat.file_area_warm);
		  spin_unlock(&p_file_stat_base->file_stat_lock);*/

		WRITE_ONCE(p_file_stat_base->mapping->rh_reserved1, (u64)(&p_hot_cold_file_global->global_mmap_file_stat.file_stat.file_stat_base));
		p_global_lock = &p_hot_cold_file_global->mmap_file_global_lock;
		p_global_file_stat = &p_hot_cold_file_global->global_mmap_file_stat;
	}
	/* 这里要加上smp_wmb()，等synchronize_rcu执行后。新的进程读写文件，执行__filemap_add_folio()，rcu_read_lock后smp_rmb，从mapping->rh_reserved1
	 * 得到的时上边新赋值的global_file_stat*/
	smp_wmb();
	/* 如果有进程正在mapping_get_entry()、filemap_get_read_batch()、__filemap_add_folio()函数里使用老的p_file_stat_base，
	 * 这些函数都有rcu_read_lock()，这里等待这些进程退出rcu宽限期。等新的进程再执行这些函数，就会从mapping->rh_reserved1
	 * 得到刚赋值的p_hot_cold_file_global->global_file_stat、p_hot_cold_file_global->global_mmap_file_stat*/
	synchronize_rcu();

	spin_lock(p_global_lock);
	/*可能此时该文件被iput delete了，要防护*/
	if(!file_stat_in_delete_base(p_file_stat_base)){
		/*该file_stat从老的global链表中剔除，下边call_rcu异步释放掉*/
		list_del_rcu(&p_file_stat_base->hot_cold_file_list);
	}
	spin_unlock(p_global_lock);

	/*把tiny small->temp链表上的in_free、in_refault、in_mapcount等file_area清理掉原属性清理掉，并移动到global_file_stat对应链表*/
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_base->file_area_temp,file_area_list){
		
		switch (get_file_area_list_status(p_file_area)){
			case (1 << F_file_area_in_hot_list):
				list_move(&p_file_area->file_area_list,&p_global_file_stat->file_stat.file_area_hot);
				p_global_file_stat->file_stat.file_area_hot_count ++;
				break;
			case (1 << F_file_area_in_refault_list):
				//list_move(&p_file_area->file_area_list,&p_global_file_stat->file_stat.file_area_refault);多层warm机制引入refault file_area移动到file_area_hot链表 
				list_move(&p_file_area->file_area_list,&p_global_file_stat->file_stat.file_area_hot);
				break;
				/*file_area同时具备in_free的in_refault属性的，那是in_free的被访问update函数设置了in_refault，要移动到file_stat->in_free链表，
				 *因为这种属性的file_area就是在in_free链表，将来执行file_stat_other_list_file_area_solve_common()会把它移动到in_refault链表*/
			case (1 << F_file_area_in_refault_list | 1 << F_file_area_in_free_list):
			case (1 << F_file_area_in_free_list):
				list_move(&p_file_area->file_area_list,&p_global_file_stat->file_stat.file_area_free);
				break;
			case (1 << F_file_area_in_mapcount_list):
				list_move(&p_file_area->file_area_list,&p_global_file_stat->file_area_mapcount);
				break;
			//case (1 << F_file_area_in_temp_list):
			case 0:
				clear_file_area_in_temp_list(p_file_area);
				break;
			default:
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);
		}

		/* 如果file_area没有page，再把file_area移动到zero_page_file_area_list链表。ctags时有大量的这种文件，0个page的file_area。
		 * 注意，file_area不能有in_free、in_hot等状态，否则global_file_stat_zero_page_file_area_list_solve()遍历这个file_area，
		 * 因file_area有这些状态而panic*/
		if(!file_area_have_page(p_file_area) && (get_file_area_list_status(p_file_area) == 0)){
			/*访问计数必须清0，否则干预后续判断*/
			file_area_access_freq_clear(p_file_area);
			list_num_update(p_file_area,POS_ZERO_PAGE);
			list_move(&p_file_area->file_area_list,&p_global_file_stat->zero_page_file_area_list);
		}
		/*防止循环耗时太长而适当调度*/
		//cond_resched();
	}
	/* 到这里，能保证global_file_stat->file_stat_base的地址赋值给该文件的mapping->rh_reserved1，现在可以放心
	 * 把file_stat_base->file_area_temp链表上的file_area移动到global_file_stat->warm链表。还有一点，不能把
	 * file_area移动到global_file_stat.file_stat_base.temp链表，否则还得spin_lock(global_file_stat.file_stat_base.file_stat_lock)
	 * 加锁，因为其他文件会同时向global_file_stat.file_stat_base.temp链表添加file_area。而global_file_stat.file_area_warm
	 * 链表，只有异步内存回收线程会操作，不用担心并发问题*/
	spin_lock(&p_file_stat_base->file_stat_lock);
	/*if(is_cache_file)
		list_splice_init(&p_file_stat_base->file_area_temp,&p_hot_cold_file_global->global_file_stat.file_stat.file_area_warm);
	else
		list_splice_init(&p_file_stat_base->file_area_temp,&p_hot_cold_file_global->global_mmap_file_stat.file_stat.file_area_warm);*/
	list_splice_init(&p_file_stat_base->file_area_temp,&p_global_file_stat->file_stat.file_area_warm);
	spin_unlock(&p_file_stat_base->file_stat_lock);

	/*如果老的file_stat时print_file_stat，等待proc用过print_file_stat后再退出*/
	old_file_stat_wait_print_file_stat(p_file_stat_base);

	/*老的file_stat转成global_file_stat后，必须设置file_stat in_replaced，后续就不会再使用它了*/
	set_file_stat_in_replaced_file_base(p_file_stat_base);
	/* 这里rcu_read_lock，然后下边call_rcu()就不会释放掉file_stat结构体了，等回到get_file_area_from_file_stat_list()函数，
	 * 确保该file_stat不会再被使用时再rcu_read_unlock放开*/
	rcu_read_lock();

	/* 隐藏bug，如果这里释放掉file_stat，但是返回到get_file_area_from_file_stat_list()函数后，还要使用这个
	 * file_stat，那就是非法内存访问了。于是把call_rcu异步释放file_stat的代码放到get_file_area_from_file_stat_list()
	 * 函数最后，确保该file_stat不会再被使用时再call_rcu异步释放掉。算了，最后决定用提前rcu_read_lock防护就行了*/
	/*释放掉file_stat_base*/
	call_rcu(&p_file_stat_base->i_rcu, i_file_stat_tiny_small_callback);

	p_hot_cold_file_global->file_stat_count --;

	return 1;
}
/*针对refault较多的文件，增大age_dx以减少内存回收。针对writeonly文件，减少age_dx，以增大内存回收。是否有必要，针对mmap文件再增大age_dx？不用了，在内存回收最开始的位置已经在change_global_age_dx_for_mmap_file函数增大过age_dx*/
static inline void reclaim_file_area_age_dx_change(struct hot_cold_file_global *p_hot_cold_file_global,/*struct file_stat_base *p_file_stat_base,unsigned int file_stat_list_type,char is_cache_file,*/struct age_dx_param *p_age_dx_param,unsigned int age_dx_type)
{
	/*保存原始age_dx*/
	p_age_dx_param->file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx;
	p_age_dx_param->file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx;
	p_age_dx_param->file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_refault_to_temp_age_dx;
	p_age_dx_param->file_area_reclaim_ahead_age_dx  = p_hot_cold_file_global->file_area_reclaim_ahead_age_dx;
	p_age_dx_param->file_area_reclaim_read_age_dx = p_hot_cold_file_global->file_area_reclaim_read_age_dx;

	switch (age_dx_type){
		case AGE_DX_CHANGE_WRITEONLY_IN_EMERGENCY_RECLAIM:
			//p_hot_cold_file_global->file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx >> 2;
			p_hot_cold_file_global->file_area_temp_to_cold_age_dx = hot_cold_file_global_info.writeonly_file_age_dx;
			p_hot_cold_file_global->file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx >> 2;
			p_hot_cold_file_global->file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_refault_to_temp_age_dx >> 1;
			p_hot_cold_file_global->file_area_reclaim_ahead_age_dx = p_hot_cold_file_global->file_area_reclaim_ahead_age_dx >> 2;
			p_hot_cold_file_global->file_area_reclaim_read_age_dx = p_hot_cold_file_global->file_area_reclaim_read_age_dx;
			break;
		case AGE_DX_CHANGE_REFAULT_SLIGHT:
			p_hot_cold_file_global->file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx << 1;
			p_hot_cold_file_global->file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx << 1;
			p_hot_cold_file_global->file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_refault_to_temp_age_dx << 1;
			p_hot_cold_file_global->file_area_reclaim_ahead_age_dx = p_hot_cold_file_global->file_area_reclaim_ahead_age_dx << 1;
			p_hot_cold_file_global->file_area_reclaim_read_age_dx = p_hot_cold_file_global->file_area_reclaim_read_age_dx << 1;
			break;
		case AGE_DX_CHANGE_REFAULT_SERIOUS:
			p_hot_cold_file_global->file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx << 2;
			p_hot_cold_file_global->file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx << 2;
			p_hot_cold_file_global->file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_refault_to_temp_age_dx << 3;
			p_hot_cold_file_global->file_area_reclaim_ahead_age_dx = p_hot_cold_file_global->file_area_reclaim_ahead_age_dx << 2;
			p_hot_cold_file_global->file_area_reclaim_read_age_dx = p_hot_cold_file_global->file_area_reclaim_read_age_dx << 2;
			break;
		case AGE_DX_CHANGE_REFAULT_CRITIAL:
			p_hot_cold_file_global->file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx << 3;
			p_hot_cold_file_global->file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx << 2;
			p_hot_cold_file_global->file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_refault_to_temp_age_dx << 3;
			p_hot_cold_file_global->file_area_reclaim_ahead_age_dx = p_hot_cold_file_global->file_area_reclaim_ahead_age_dx << 2;
			p_hot_cold_file_global->file_area_reclaim_read_age_dx = p_hot_cold_file_global->file_area_reclaim_read_age_dx << 3;
			break;
		default:
			panic("reclaim_file_area_age_dx_change age_dx_type:%d error\n",age_dx_type);
	}
}
static inline void reclaim_file_area_age_dx_restore(struct hot_cold_file_global *p_hot_cold_file_global,/*struct file_stat_base *p_file_stat_base,unsigned int file_stat_list_type,char is_cache_file,*/struct age_dx_param *p_age_dx_param)
{
	if(0 == p_age_dx_param->file_area_temp_to_cold_age_dx)
		panic("reclaim_file_area_age_dx_restore file_area_temp_to_cold_age_dx == 0 error\n");

	p_hot_cold_file_global->file_area_temp_to_cold_age_dx = p_age_dx_param->file_area_temp_to_cold_age_dx;
	p_hot_cold_file_global->file_area_hot_to_temp_age_dx = p_age_dx_param->file_area_hot_to_temp_age_dx;
	p_hot_cold_file_global->file_area_refault_to_temp_age_dx = p_age_dx_param->file_area_refault_to_temp_age_dx;
	p_hot_cold_file_global->file_area_reclaim_ahead_age_dx = p_age_dx_param->file_area_reclaim_ahead_age_dx;
	p_hot_cold_file_global->file_area_reclaim_read_age_dx = p_age_dx_param->file_area_reclaim_read_age_dx;
}
static int scan_file_area_max_base(unsigned int file_stat_list_type,char is_cache_file,unsigned int memory_pressure_level)//0,1,2,3
{
	int scan_file_area_max = 0;

	switch(file_stat_list_type){
		case F_file_stat_in_file_stat_tiny_small_file_one_area_head_list:
		case F_file_stat_in_file_stat_tiny_small_file_head_list:
			scan_file_area_max =  16 + (1 << (memory_pressure_level + 1));//2~16
			break;
		case F_file_stat_in_file_stat_small_file_head_list:
			scan_file_area_max = 32 + (1 << (memory_pressure_level + 3));//8~64
			break;
		case F_file_stat_in_file_stat_temp_head_list:
			scan_file_area_max = 32 + (1 << ((memory_pressure_level << 1) + 1));//2^1 ~ 2^7
			break;
		case F_file_stat_in_file_stat_middle_file_head_list:
			scan_file_area_max = 64 + (1 << ((memory_pressure_level << 1) + 2));//2^2 ~ 2^8
			break;
		case F_file_stat_in_file_stat_writeonly_file_head_list:
			//fallthrough;
		case F_file_stat_in_file_stat_large_file_head_list:
			scan_file_area_max = 64 + (1 << ((memory_pressure_level << 1) + 3));//2^3 ~ 2^9
			break;
		default:
			BUG();
	}

	/*mmap文件内存回收很容易refault，减少scan_max*/
	if(!is_cache_file)
		scan_file_area_max = (scan_file_area_max >> 2);

	return scan_file_area_max;
}
static int check_file_area_refault_and_scan_max(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int file_stat_list_type,unsigned int file_type,char is_cache_file,unsigned int *age_dx_change_type)
{
	unsigned int  refault_file_area_scan_dx = p_hot_cold_file_global->refault_file_area_scan_dx;
	int scan_file_area_max = scan_file_area_max_base(file_stat_list_type,is_cache_file,p_hot_cold_file_global->memory_pressure_level);

	switch (file_type){
		case FILE_STAT_TINY_SMALL:
			if(p_file_stat_base->refault_page_count > refault_file_area_scan_dx + 64){
				scan_file_area_max = 3;
				*age_dx_change_type = AGE_DX_CHANGE_REFAULT_CRITIAL;
			}
			else if(p_file_stat_base->refault_page_count > refault_file_area_scan_dx + 32){
				scan_file_area_max = 3;
				*age_dx_change_type = AGE_DX_CHANGE_REFAULT_SERIOUS;

			}
			else if(p_file_stat_base->refault_page_count > refault_file_area_scan_dx){
				scan_file_area_max -= 10;
				*age_dx_change_type = AGE_DX_CHANGE_REFAULT_SLIGHT;
			}

			break;

		case FILE_STAT_SMALL:
			if(p_file_stat_base->refault_page_count > refault_file_area_scan_dx + 64){
				scan_file_area_max = 6;
				*age_dx_change_type = AGE_DX_CHANGE_REFAULT_CRITIAL;
			}
			else if(p_file_stat_base->refault_page_count > refault_file_area_scan_dx + 32){
				scan_file_area_max = 6;
				*age_dx_change_type = AGE_DX_CHANGE_REFAULT_SERIOUS;
			}
			else if(p_file_stat_base->refault_page_count > refault_file_area_scan_dx){

				*age_dx_change_type = AGE_DX_CHANGE_REFAULT_SLIGHT;
				scan_file_area_max -= 30;
			}
			break;

		case FILE_STAT_NORMAL:
			/*refault_page_count越大说明内存回收发生refault概率更大，要减少扫描的file_area个数*/
			if(p_file_stat_base->refault_page_count > refault_file_area_scan_dx + 64){
				*age_dx_change_type = AGE_DX_CHANGE_REFAULT_CRITIAL;
				scan_file_area_max  = 10;
			}
			else if(p_file_stat_base->refault_page_count > refault_file_area_scan_dx + 32){
				*age_dx_change_type = AGE_DX_CHANGE_REFAULT_SERIOUS;
				scan_file_area_max = 30;
			}
			else if(p_file_stat_base->refault_page_count > refault_file_area_scan_dx){
				*age_dx_change_type = AGE_DX_CHANGE_REFAULT_SLIGHT;
				scan_file_area_max -= 80;
			}
			/* reclaim_pages_last_period越大，说明文件file_stat上个周期内存回收的page数越多，增大扫描的file_area个数。
			 * 这个功能先不加了，因为该文件虽然内存回收了很多page，但是很多都热fault了，本次就不能再加大扫描了*/
			/*if(p_file_stat->reclaim_pages_last_period > 128)
			  scan_file_area_max += 128;
			  else if(p_file_stat->reclaim_pages_last_period > 64)
			  scan_file_area_max += 64;
			  else if(p_file_stat->reclaim_pages_last_period > 32)
			  scan_file_area_max += 16;*/

			break;

		default:	
			panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);
			break;
	}

	/* 如果该file_stat很长时间都没被访问了，那也不能因为历史的refault_page_count大，就一直不
	 * 扫描该file_stat的file_area。于是把file_stat_base->recent_access_age清0，下个周期就可以
	 * 扫描该文件的file_area*/
	if(p_file_stat_base->refault_page_count && p_hot_cold_file_global->global_age - p_file_stat_base->recent_access_age > FILE_STAT_REFAULT_PAGE_COUNT_CLEAR_AGE_DX)
		p_file_stat_base->refault_page_count = 0;

	if(scan_file_area_max < 0){
		printk("%s p_file_stat:0x%llx file_stat_list_type:%d scan_file_area_max:%d ====== zero!!!!!!!\n",__func__,(u64)p_file_stat_base,file_stat_list_type,scan_file_area_max);
		scan_file_area_max = 0;
	}

	return scan_file_area_max;
}
/* mysql低内存场景实际测试表明，writeonly文件在经过第一轮内存后，几乎所有的file_area都移动到了file_stat->free链表。
 * 后续该文件再被访问，file_area又分配了大量page，但是这些file_area都处于file_stat->free链表，还有少量的file_area
 * 处于file_stat->refault或hot链表。而此时处于内存紧缺模式，该文件明明有大量的pagecache，但是因为大部分file_area
 * 都处于file_stat->free链表，而无法从该文件回收大量的pagecache。为了解决这个问题，开发这个函数接口，直接从
 * file_stat->free等链表的file_area回收pagecache。当然要提防一个page都没有的file_area，如果遇到太多这种file_area
 * 就要提前遍历*/
static unsigned int direct_recliam_file_stat_free_refault_hot_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base)
{
	unsigned int free_pages = 0;
	unsigned int scan_file_area_count = 0;
	struct file_stat *p_file_stat;

	/*file_stat必须是normal文件，不能处于tiny_small_one_area、tiny_small、small文件链表*/
	if(file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base) || 
			file_stat_in_file_stat_small_file_head_list_base(p_file_stat_base) || 
			file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base)){
		panic("%s file_stat:0x%llx status:0x%x error\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
	}

	p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
	/*每次内存回收前先对free_pages_count清0*/
	p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count = 0;

	scan_file_area_count = cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,&p_file_stat->file_area_free,-1,NULL,1,NULL,NULL);
	//isolate_lru_pages += cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,&p_file_stat->file_area_refault,-1,NULL,1);多层warm机制引入refault file_area移动到file_area_hot链表
	scan_file_area_count += cold_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,&p_file_stat->file_area_hot,-1,NULL,1,NULL,NULL);
    /*本函数是暴力回收只读文件file_stat->free、refault、hot等链表的file_area，于是不再把这些file_area移动到file_stat->free链表，没有意义*/
	//list_splice(file_area_free_temp,&p_file_stat->file_area_free);

	free_pages = p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;
	all_file_stat_reclaim_pages_counter(p_hot_cold_file_global,p_file_stat_base,0,free_pages);
	//p_file_stat->reclaim_pages_last_period = free_pages;
	p_file_stat->reclaim_pages += free_pages;

	//隔离的page个数
	//p_hot_cold_file_global->hot_cold_file_shrink_counter.isolate_lru_pages += isolate_lru_pages;
	//从系统启动到目前释放的page个数
	if(file_stat_in_cache_file_base(p_file_stat_base))
		p_hot_cold_file_global->free_pages += p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;
	else
		p_hot_cold_file_global->free_mmap_pages += p_hot_cold_file_global->hot_cold_file_shrink_counter.free_pages_count;

	/*测试证明，这里直接从writeonly->free、hot链表上回收page，竟然一直都是0!!!!!!!!!*/
	p_hot_cold_file_global->memory_reclaim_info.direct_reclaim_pages_form_writeonly_file = free_pages;
	p_hot_cold_file_global->memory_reclaim_info.scan_file_area_count_form_writeonly_file = scan_file_area_count;
	p_hot_cold_file_global->direct_reclaim_pages_form_writeonly_file += free_pages;

	if(warm_list_printk)
		printk("%s file_stat:0x%llx writeonly file recliam_pages:%d\n",__func__,(u64)p_file_stat_base,free_pages);

	return free_pages;
}
static unsigned int get_file_area_from_file_stat_list_common(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int scan_file_area_max,unsigned int file_stat_list_type,unsigned int file_type,char is_cache_file)
{
	struct file_stat *p_file_stat = NULL;
	struct file_stat_small *p_file_stat_small = NULL;
	//struct file_stat_tiny_small *p_file_stat_tiny_small = NULL;
	//struct file_stat_tiny_small *p_file_stat_tiny_small = NULL;
	//struct file_area *p_file_area,*p_file_area_temp;
	LIST_HEAD(file_area_free_temp);

	unsigned int scan_file_area_count  = 0;
	int file_area_age_dx_changed = 0;
	struct age_dx_param age_dx_param;
	unsigned int age_dx_change_type = -1;
	unsigned int file_stat_nrpages = p_file_stat_base->mapping->nrpages;
	unsigned int scan_read_file_area_count_last = 0;
	int scan_file_area_max_for_memory_reclaim = 0;
	unsigned int scan_other_list_file_area_count = 0;
	//unsigned int scan_move_to_mmap_head_file_stat_count  = 0;
	//unsigned int scan_file_stat_count  = 0;
	//unsigned int real_scan_file_stat_count  = 0;
	//unsigned int scan_delete_file_stat_count = 0;
	//unsigned int scan_cold_file_area_count = 0;
	//unsigned int file_stat_in_list_type = -1;
	//unsigned int scan_fail_file_stat_count = 0;

	//unsigned int cold_file_area_for_file_stat = 0;
	//unsigned int file_stat_count_in_cold_list = 0;

	/* 从global temp和large_file_temp链表尾遍历N个file_stat，回收冷file_area的。对热file_area、refault file_area、
	 * in_free file_area的各种处理。这个不global lock加锁。但是遇到file_stat移动到其他global 链表才会global lock加锁*/
	//list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,file_stat_temp_head,hot_cold_file_list){------------这个原来的大循环不要删除

	/* tiny small和small文件的file_area个数如果超过阀值则转换成normal文件等。这个操作必须放到遍历file_stat的file_area前边，
	 * 具体分析见can_tiny_small_file_change_to_small_normal_file()函数。后来这段代码移动到遍历tiny small和small文件
	 * file_stat的入口函数里，不在这里处理*/
#if 0	
	if(FILE_STAT_TINY_SMALL ==  file_type){
		p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
		/*tiny small文件如果file_area个数超过阀值升级到small、temp、middle、large文件*/
	}else if(FILE_STAT_SMALL ==  file_type){
		p_file_stat_tiny_small = container_of(p_file_stat_base,struct file_stat_tiny_small,file_stat_base);
		/*small文件如果file_area个数超过阀值升级到temp、middle、large文件*/
	}
#endif

	/*if(scan_file_area_count > scan_file_area_max || scan_file_stat_count ++ > scan_file_stat_max)
	  return scan_file_area_count;*/

	memset(&age_dx_param,0,sizeof(struct age_dx_param));
	/*file_stat和file_type不匹配则主动crash。global_file_stat不会走到这个流程，不再做限制*/
	//if(!file_stat_in_global_base(p_file_stat_base))
	is_file_stat_match_error(p_file_stat_base,file_type);


	/* 现在遍历global temp和large_file_temp链表上的file_stat不加global lock了，但是下边需要时刻注意可能
	 * 唯一可能存在的并发移动file_stat的情况：iput()释放file_stat，标记file_stat delete，并把file_stat移动到
	 * global delete链表。以下的操作需要特别注意这种情况!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
#if 0
	/* 到这里， file_stat可能被iput()并发释放file_stat，标记file_stat delete，但是不会清理file_stat in temp_head_list状态。
	 * 因此这个if不会成立*/
	switch (file_stat_list_type){
		case F_file_stat_in_file_stat_tiny_small_file_head_list:
			/*file_stat此时可能被方法iput()释放delete，但file_stat的delete的标记与file_stat in_temp_list 是共存的，不干扰file_stat in_temp_list的判断*/
			if(!file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_tiny_small_file_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_tiny_small status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);


			/* 把在global file_stat_temp_head链表但实际是mmap的文件file_stat从global file_stat_temp_head链表剔除，
			 * 然后添加到global mmap_file_stat_tiny_small_temp_head链表。注意，这里仅仅针对so库、可执行文件等elf文件，这些文件
			 * 都是先read系统调用读取文件头，被判定为cache文件，然后再mmap映射了。这种文件第一次扫描到，肯定处于global 
			 * temp链表，则把他们移动到global mmap_file_stat_tiny_small_temp_head链表。有些cache文件读写后，经历内存回收
			 * 在file_stat->warm、hot、free链表都已经有file_area了，就不再移动到global mmap_file_stat_tiny_small_temp_head链表了。
			 * 因此引入if(p_file_stat_tiny_small->file_area_count == p_file_stat_tiny_small->file_area_count_in_temp_list)这个判断，因为最初
			 * 二者肯定相等。当然，也有一种可能，就是过了很长时间，经历file_area内存回收、释放和再分配，二者还是相等。
			 * 无所谓了，就是把一个file_stat移动到global mmap_file_stat_tiny_small_temp_head而已，反正这个文件也是建立的mmap映射。
			 * 最后，再注意一点，只有在global temp_list的file_stat才能这样处理。*/

			/*还有一个关键隐藏点，怎么确保这种file_stat一定是global->tiny_small_file链表上，而不是其他global链表上？首先，
			 *任一个文件的创建时一定添加到global->tiny_small_file链表上，然后异步内存线程执行到该函数时，这个文件file_stat一定
			 *在global->tiny_small_file链表上，不管这个文件的file_area有多少个。接着,才会执行把该file_stat移动到其他global链表的代码*/
			if(mapping_mapped((struct address_space *)p_file_stat_base->mapping) && (p_file_stat_base->file_area_count == p_file_stat_base->file_area_count_in_temp_list)){
				scan_move_to_mmap_head_file_stat_count ++;
				cache_file_stat_move_to_mmap_head(p_hot_cold_file_global,p_file_stat_base);
				return scan_file_area_count;
			}

			break;
		case F_file_stat_in_file_stat_small_file_head_list:
			/*file_stat此时可能被方法iput()释放delete，但file_stat的delete的标记与file_stat in_temp_list 是共存的，不干扰file_stat in_temp_list的判断*/
			if(!file_stat_in_file_stat_small_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_small_file_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_small status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		case F_file_stat_in_file_stat_temp_head_list:
			/*file_stat此时可能被方法iput()释放delete，但file_stat的delete的标记与file_stat in_temp_list 是共存的，不干扰file_stat in_temp_list的判断*/
			if(!file_stat_in_file_stat_temp_head_list_base(p_file_stat_base) || file_stat_in_file_stat_temp_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_temp_head status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		case F_file_stat_in_file_stat_middle_file_head_list:
			if(!file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_middle_file_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_middle_file_head status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		case F_file_stat_in_file_stat_large_file_head_list:
			if(!file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_large_file_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_large_file_head status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		default:	
			panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);
			break;
	}
#endif

#if 0
	/*一个mmapped文件不可能存在于global global temp和large_file_temp链表*/
	if(file_stat_in_mmap_file_base(p_file_stat_base)){
		panic("%s p_file_stat:0x%llx status:0x%x in_mmap_file\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
	}
#endif
	/*可能iput最后直接把delete file_stat移动到global delete链表，并标记file_stat in delete状态*/
	if(file_stat_in_delete_base(p_file_stat_base)){
		//scan_delete_file_stat_count ++;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_delete_file_stat_count += 1;
		printk("%s file_stat:0x%llx delete status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

		move_file_stat_to_global_delete_list(p_hot_cold_file_global,p_file_stat_base,file_type,is_cache_file);
		return scan_file_area_count;
	}
	/* 如果file_stat的file_area全被释放了，则把file_stat移动到hot_cold_file_global->file_stat_zero_file_area_head链表。
	 * 但是唯一可能存在的并发移动file_stat的情况是iput()释放file_stat，标记file_stat delete，并把file_stat移动到
	 * global delete链表。因此要global_lock加锁后再判断一次file_stat是否被iput()把file_stat移动到global delete链表了*/
	else if(p_file_stat_base->file_area_count == 0){
		/*本来以为只有global temp链表上file_stat移动到global zero链表才需要加锁，因为会与读写文件进程
		 *执行__filemap_add_folio()向global temp添加file_stat并发修改global temp链表。但是漏了一个并发场景，
		 *文件inode释放会执行iput()标记file_stat的delete，然后把file_stat移动到global zero，对
		 *global temp、middle、large链表上file_stat都会这样操作，并发修改这些链表头。故这里
		 *global temp、middle、large链表上file_stat移动到global zero链表，都需要global lock加锁，因为iput()
		 *同时会并发修改*global temp、middle、large链表头，移动他们上边的file_stat到global zero链表!!!!!!!!!!!!!!!*/
		if(is_cache_file)
			spin_lock(&p_hot_cold_file_global->global_lock);
		else
			spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
		/*file_stat被iput()并发标记delete了，则不再理会*/
		if(!file_stat_in_delete_base(p_file_stat_base)){
			/*在file_stat被移动到global zero链表时，不再清理file_stat原有的状态。原因是，如果清理了原有的状态，然后file_stat被
			 *iput()释放了，执行到__destroy_inode_handler_post()函数时，无法判断file_stat是tiny small、small、normal 文件!
			 *于是现在不在file_stat原有的状态了*/
			switch (file_stat_list_type){
				//如果该文件没有file_area了，则把对应file_stat移动到hot_cold_file_global->zero链表
				case F_file_stat_in_file_stat_tiny_small_file_head_list:
					//clear_file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_tiny_small_zero_file_area_head);
					else
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_tiny_small_zero_file_area_head);
					break;
				case F_file_stat_in_file_stat_tiny_small_file_one_area_head_list:
					//clear_file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_tiny_small_zero_file_area_head);
					else
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_tiny_small_zero_file_area_head);
					break;
				case F_file_stat_in_file_stat_small_file_head_list:
					//clear_file_stat_in_file_stat_small_file_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_small_zero_file_area_head);
					else
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_small_zero_file_area_head);
					break;
				case F_file_stat_in_file_stat_temp_head_list:
					//clear_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_zero_file_area_head);
					else
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_zero_file_area_head);
					break;
				case F_file_stat_in_file_stat_middle_file_head_list:
					//clear_file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_zero_file_area_head);
					else
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_zero_file_area_head);
					break;
				case F_file_stat_in_file_stat_large_file_head_list:
					//clear_file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_zero_file_area_head);
					else
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_zero_file_area_head);
					break;
				case F_file_stat_in_file_stat_writeonly_file_head_list:
					//clear_file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base);
					if(is_cache_file)
						list_move(&p_file_stat_base->hot_cold_file_list,&p_hot_cold_file_global->file_stat_zero_file_area_head);
					else
						panic("%s p_file_stat:0x%llx file_stat_list_type:%d error is_cache_file error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);
					break;

				default:
					panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);
			}

			set_file_stat_in_zero_file_area_list_base(p_file_stat_base);
			p_hot_cold_file_global->file_stat_count_zero_file_area ++;
		}
		if(is_cache_file)
			spin_unlock(&p_hot_cold_file_global->global_lock);
		else
			spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

		return scan_file_area_count;
	}

	if(file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base))
		scan_read_file_area_count_last = p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_read_file_area_count_from_temp + p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_read_file_area_count_from_warm;

	/*file_area_free_temp现在不是file_stat->file_area_free_temp全局，而是一个临时链表，故每次向该链表添加file_area前必须要初始化*/
	INIT_LIST_HEAD(&file_area_free_temp);

	/* 针对refault多的文件，减少scan_file_area_max个数。并且把refault程度记录到age_dx_change_type。后续还要根据文件的
	 * recliam_pages、nr_pages进一步更改scan_file_area_max、age_dx_change_type*/
	//scan_file_area_max = 64;
	
	/* 引入多层warm链表后，这里把scan_file_area_max改为scan_file_area_max_for_memory_reclaim了。因为如果
	 * 这里修改了scan_file_area_max，上层传入的scan_file_area_max最小是128，而check_file_area_refault_and_scan_max()
	 * 里会把scan_file_area_max修改的很小，比如10。这导致下边file_stat_multi_level_warm_or_writeonly_list_file_area_solve()扫描
	 * file_stat->warm链表的file_area超过10而停止扫描，并没有扫描完file_stat->warm链表上所有的file_area，
	 * p_current_scan_file_stat_info->p_traverse_file_stat不会置NULL。。然后当前函数返回到 get_file_area_from_file_stat_list()后，
	 * scan_file_area_max还是原始的128。结果scan_file_area_count并没有大于scan_file_area_max。于是继续扫描下一个文件的file_stat。
	 * 但是file_stat_multi_level_warm_or_writeonly_list_file_area_solve()里，发现要扫描的file_stat跟
	 * p_current_scan_file_stat_info->p_traverse_file_stat不相等主动触发panic。解决办法是，这里不再篡改scan_file_area_max，
	 * 而是引入scan_file_area_max_for_memory_reclaim，用它来控制内存回收时，要扫描的file_area个数。*/
	//scan_file_area_max = check_file_area_refault_and_scan_max(p_hot_cold_file_global,p_file_stat_base,file_stat_list_type,file_type,is_cache_file,&age_dx_change_type);
	scan_file_area_max_for_memory_reclaim = check_file_area_refault_and_scan_max(p_hot_cold_file_global,p_file_stat_base,file_stat_list_type,file_type,is_cache_file,&age_dx_change_type);

	/* 在内存紧张时，如果检测到是writeonly文件，则在reclaim_file_area_age_dx_change()里调小age_dx，以加快回收该文件的文件页。
	 * 注意，在writeonly_list链表上的文件，可能因为读被清理了writeonly标记*/
	//if(!IS_MEMORY_ENOUGH(p_hot_cold_file_global) && file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base))
	if(IS_IN_MEMORY_EMERGENCY_RECLAIM(p_hot_cold_file_global) && (file_stat_in_file_stat_writeonly_file_head_list_base(p_file_stat_base))){
		/* 如果global->writeonly链表上的file_stat有file_stat_in_writeonly_base标记说明是writeonly文件，如果没有说明该文件被清理了writeonly
		 * 文件，则要把scan_file_area_max_for_memory_reclaim设置的很小，避免大量内存回收该文件造成refault，并且很快下边执行
		 * file_stat_status_change_solve()会把该文件从global->writeonly链表剔除掉*/
		if(file_stat_in_writeonly_base(p_file_stat_base)){
			/*writeonly文件不限制内存回收的page数*/
			scan_file_area_max_for_memory_reclaim = -1;
			age_dx_change_type = AGE_DX_CHANGE_WRITEONLY_IN_EMERGENCY_RECLAIM;
		}
		else
			scan_file_area_max_for_memory_reclaim = 128;
	}


	/* 针对refault较多的文件，增大age_dx以减少内存回收。针对writeonly文件，减少age_dx，以增大内存回收。
	 * 是否有必要，针对mmap文件再增大age_dx？不用了，在内存回收最开始的位置已经执行change_global_age_dx_for_mmap_file函数增大过age_dx*/
	if(-1 != age_dx_change_type){
		reclaim_file_area_age_dx_change(p_hot_cold_file_global,&age_dx_param,age_dx_change_type);
		file_area_age_dx_changed = 1;
	}

	/* file_stat->temp链表上的file_area的处理，冷file_area且要内存回收的移动到file_area_free_temp临时链表，下边回收该链表上的file_area的page
	 * 引入global_file_stat后，normal file_stat->temp直接移动到file_stat->warm链表，不再单独遍历处理。只有tiny small和small文件才会
	 * 遍历temp链表上的file_area并决定内存回收*/
	//scan_file_area_count += file_stat_temp_list_file_area_solve(p_hot_cold_file_global,p_file_stat_base,scan_file_area_max,&file_area_free_temp,file_type,age_dx_change_type);

	if(FILE_STAT_NORMAL ==  file_type){
		struct current_scan_file_stat_info *p_current_scan_file_stat_info = get_normal_file_stat_current_scan_file_stat_info(p_hot_cold_file_global,1 << file_stat_list_type,is_cache_file);
		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

        scan_file_area_count += file_stat_multi_level_warm_or_writeonly_list_file_area_solve(p_hot_cold_file_global,p_current_scan_file_stat_info,p_file_stat_base,scan_file_area_max,is_cache_file,scan_file_area_max_for_memory_reclaim);
		//scan_file_area_max = 32;
		/*file_stat->warm链表上的file_area的处理，冷file_area且要内存回收的移动到file_area_free_temp临时链表，下边回收该链表上的file_area的page*/
		//scan_file_area_count += file_stat_warm_list_file_area_solve(p_hot_cold_file_global,p_file_stat,scan_file_area_max,&file_area_free_temp,file_type,age_dx_change_type);

		/* 回收file_area_free_temp临时链表上的冷file_area的page，回收后的file_area移动到file_stat->free链表头。
		 * 引入多层warm链表方案后，普通文件的内存回收都在file_stat_multi_level_warm_or_writeonly_list_file_area_solve()里完成了*/
		//free_page_from_file_area(p_hot_cold_file_global,p_file_stat_base,&file_area_free_temp,file_type);

		/* 内存紧缺模式，writeonly文件有文件页page的file_area可能处于file_stat->free链表，因为在此之前经过一次内存回收
		 * 把这些file_area都移动到了file_stat->free链表。这导致上边从file_stat->temp、warm链表回收到了很少的page，到
		 * 这里时,该writeonly文件依然还有大量的pagecache,即mapping->nrpages很大,则从file_stat->free链表的file_area回收page*/
		if(AGE_DX_CHANGE_WRITEONLY_IN_EMERGENCY_RECLAIM == age_dx_change_type){
			if(file_stat_in_writeonly_base(p_file_stat_base) && p_file_stat_base->mapping->nrpages > 16)
				direct_recliam_file_stat_free_refault_hot_file_area(p_hot_cold_file_global,p_file_stat_base);
		}else{ 
			/* 下边针对普通文件的file_stat_other_list_file_area_solve()的几行代码移动到了file_stat_multi_level_warm_or_writeonly_list_file_area_solve()函数，
			 * 跟global_file_stat统计scan other_list_file_area放到一起。但是后来分析不行，因为如果时writeonly文件，在内存紧张时，只能执行上边的
			 * direct_recliam_file_stat_free_refault_hot_file_area()浏览回收file_stat->free、hot、refault链表上的file_area并回收，不能再执行
			 * file_stat_other_list_file_area_solve()浏览file_stat->free、hot、refault链表上的file_area。如果把下边的代码移动到
			 * file_stat_multi_level_warm_or_writeonly_list_file_area_solve()函数，那就会direct_recliam_file_stat_free_refault_hot_file_area()
			 * 和下边的file_stat_other_list_file_area_solve()函数都执行，这样就起冲突了，最后决定普通文件的file_stat_other_list_file_area_solve()还是放到这里*/

			/*内存回收后，遍历file_stat->hot、refault、free链表上的各种file_area的处理*/
			scan_file_area_max_for_memory_reclaim = 256;
			scan_other_list_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,&p_file_stat->file_stat_base,&p_file_stat->file_area_hot,scan_file_area_max_for_memory_reclaim,F_file_area_in_hot_list,FILE_STAT_NORMAL);
			//scan_file_area_max_for_memory_reclaim = 256;新版本把file_stat->file_area_refault去除了，refault file_area移动到file_area_hot链表
			/*scan_file_area_count += *///file_stat_other_list_file_area_solve(p_hot_cold_file_global,&p_file_stat->file_stat_base,&p_file_stat->file_area_refault,scan_file_area_max_for_memory_reclaim,F_file_area_in_refault_list,FILE_STAT_NORMAL);
			scan_file_area_max_for_memory_reclaim = 128;
			scan_other_list_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,&p_file_stat->file_stat_base,&p_file_stat->file_area_free,scan_file_area_max_for_memory_reclaim,F_file_area_in_free_list,FILE_STAT_NORMAL);
			/* 这里有个隐藏bug，没有遍历file_stat->file_area_mapcount链表上file_area，对mapcount file_area进行降级处理。
			 * 但是新版本去除了file_stat->file_area_mapcount链表，为了节省内存，mapcount file_area都移动到file_stat->mapcount链表了*/
			//scan_file_area_count += file_stat_other_list_file_area_solve(p_hot_cold_file_global,&p_file_stat->file_stat_base,&p_file_stat->file_area_mapcount,scan_file_area_max_for_memory_reclaim,F_file_area_in_mapcount_list,FILE_STAT_NORMAL);
			/*这里有个隐藏bug，就是没有遍历file_stat->file_area_mapcount链表上*/	
		}

		/*查看文件是否变成了热文件、大文件、普通文件，是的话则file_stat移动到对应global hot、large_file_temp、temp 链表*/
		file_stat_status_change_solve(p_hot_cold_file_global,p_file_stat,file_stat_list_type,is_cache_file,scan_read_file_area_count_last);

		/* 如果文件file_stat在temp、middle、large、writeonly文件之间发生了变化，文件状态变了，if成立，那就要清空current_scan_file_stat_info。
		 * 这样下次周期才能令current_scan_file_stat_info指向新的文件file_stat，从而遍历新的文件file_stat*/
		if((p_file_stat_base->file_stat_status &  (1 << file_stat_list_type)) == 0){
			/*只是令p_traverse_file_stat指向NULL，下个周期就能遍历新的file_stat，不再更新num_warm_num*/
			//p_current_scan_file_stat_info->p_traverse_file_stat = NULL;
			//p_current_scan_file_stat_info->p_traverse_file_area_list_head = NULL;
			update_file_stat_next_multi_level_warm_or_writeonly_list(p_current_scan_file_stat_info,p_file_stat);
		}

		/*内存紧急模式，write文件经过一次内存回收，连一半的文件页都没有回收掉，那就调小writeonly_file_age_dx，使回收write文件页的age冷却周期减小*/
		if(AGE_DX_CHANGE_WRITEONLY_IN_EMERGENCY_RECLAIM == age_dx_change_type){
			printk("%s file_stat:0x%llx writeonly_file_recliam_pages before:%d after:%ld file_area_count:%d scan_file_area_count:%d recent_access_age:%d global_age:%d in_writeonly_list_file_count:%d\n",__func__,(u64)p_file_stat_base,file_stat_nrpages,p_file_stat_base->mapping->nrpages,p_file_stat_base->file_area_count,scan_file_area_count,p_file_stat_base->recent_access_age,p_hot_cold_file_global->global_age,p_hot_cold_file_global->in_writeonly_list_file_count);

		}

		p_hot_cold_file_global->memory_reclaim_info.scan_other_list_file_area_count = scan_other_list_file_area_count;
		/*由于普通只写文件还有个direct_reclaim_pages_form_writeonly_file，统计只读文件direct模式从free、hot、refault链表回收的page数，因此针对普通文件print_file_stat_memory_reclaim_info，放到get_file_area_from_file_stat_list_common函数*/
		print_file_stat_memory_reclaim_info(p_file_stat_base,p_hot_cold_file_global);
	}
	else{
	     scan_file_area_count += file_stat_temp_list_file_area_solve(p_hot_cold_file_global,p_file_stat_base,scan_file_area_max_for_memory_reclaim,&file_area_free_temp,file_type,age_dx_change_type);

		/*针对small和tiny small文件回收file_area_free_temp临时链表上的冷file_area的page，回收后的file_area移动到file_stat->free链表头*/
		free_page_from_file_area(p_hot_cold_file_global,p_file_stat_base,&file_area_free_temp,file_type);

		if(FILE_STAT_SMALL ==  file_type){
			p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
			/*small文件file_stat->file_area_other链表上的file_area的处理*/
			scan_file_area_max_for_memory_reclaim = 32;
			scan_file_area_count += file_stat_small_other_list_file_area_solve(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,&p_file_stat_small->file_area_other,scan_file_area_max_for_memory_reclaim,-1,FILE_STAT_SMALL);
		}

	}

	if(file_area_age_dx_changed)
		reclaim_file_area_age_dx_restore(p_hot_cold_file_global,&age_dx_param);
	//}

	//p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_delete_file_stat_count += scan_delete_file_stat_count;
	return scan_file_area_count;
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
static noinline unsigned int get_file_area_from_file_stat_list(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max,unsigned int scan_file_stat_max,struct list_head *file_stat_temp_head,unsigned int file_stat_list_type,unsigned int file_type,char is_cache_file)
{
	//file_stat_temp_head来自 hot_cold_file_global->file_stat_temp_head、file_stat_temp_large_file_head、file_stat_temp_middle_file_head 链表
	struct file_stat *p_file_stat = NULL;
	struct file_stat_small *p_file_stat_small;
	struct file_stat_tiny_small *p_file_stat_tiny_small;
	struct file_stat_base *p_file_stat_base = NULL,*p_file_stat_base_temp;
	unsigned int scan_file_area_count  = 0;
	unsigned int scan_file_stat_count  = 0;
	char file_stat_delete_lock = 0;
	int ret;
	/* 记录上一次遍历的normal file_stat，如果本次遍历结束后，当前file_stat->warm等链表的file_area还没有遍历完，则该file_stat不能移动到链表头。
	 * 而只是把p_file_stat_base_last到链表尾的遍历过的file_stat移动到链表头。初值指向链表头，目的是，如果下边for循环遍历的第一个file_stat，
	 * 都没有遍历完一个warm链表的file_area就退出循环了，则p_file_stat_base=p_file_stat_base_last赋值后，该函数最后不会把该file_stat移动到链表头，
	 * 下次循环继续遍历该file_stat。令p_file_stat_base_last指向链表头，不能直接p_file_stat_base_last=file_stat_temp_head赋值，而是要仿照下边
	 * list_for_each_entry_safe_reverse()的方法，由file_stat_temp_head指针container_of得到一个完整的file_stat_base结构，当然它实际是链表头*/
	struct file_stat_base *p_file_stat_base_last = container_of(file_stat_temp_head,struct file_stat_base,hot_cold_file_list);
	char file_stat_traverse_warm_list_num = 0;
	char file_stat_warm_or_writeonly_file_area_check_ok = 1;
	char rcu_read_lock_flag = 0;
	/* normal file_stat如果没有正常遍历file_ara，比如cache文件转成mmap文件而不遍历，cache/mmap文件因nr_pages太少导致而不遍历，或者黑名单问题
	 * 则置1。此时如果所属文件类型的current_scan_file_stat_info->p_traverse_file_stat 不是NULL则要清NULL。否则下个周期遍历新的file_stat时，
	 * 发现current_scan_file_stat_info->p_traverse_file_stat 不是NULL，则判定上次遍历的file_stat的file_area有问题，而panic*/
	char normal_file_stat_no_scan = 0;
	
	//LIST_HEAD(file_area_free_temp);
	//struct file_area *p_file_area,*p_file_area_temp;
	//unsigned int scan_move_to_mmap_head_file_stat_count  = 0;
	//unsigned int scan_file_stat_count  = 0;
	unsigned int real_scan_file_stat_count  = 0;

	//unsigned int scan_delete_file_stat_count = 0;
	//unsigned int scan_cold_file_area_count = 0;
	//unsigned int file_stat_in_list_type = -1;
	//unsigned int scan_fail_file_stat_count = 0;

	//unsigned int cold_file_area_for_file_stat = 0;
	//unsigned int file_stat_count_in_cold_list = 0;

	/*又一个重大隐藏bug，在global temp、large_file、middle_file、small file、tiny small file链表遍历file_stat、file_stat_small、
	 *file_stat_tiny_small时(因file_stat为例)。如果此时遍历到的file_stat被iput()->__destroy_inode_handler_post()并发释放，而file_stat从
	 *global temp链表剔除，然后移动到global delete链表，就出现
	 *"进程1遍历A链表时，A链表的成员被进程2并发移动到B链表，导致从A链表获取到的成员是B链表的成员或者B链表的链表头。导致进程1获取到非法链表成员而crash，或者遍历A链表时陷入死循环"
	 *重大bug。怎么解决，完整可以spin_lock加锁防护，__destroy_inode_handler_post()用原子变量作为宽限期保护。这些太复杂，其实有个更简单的方法
	 *test_and_set_bit_lock(lock_bit,&hot_cold_file_global_info.file_stat_delete_protect)。"当前函数遍历global temp链表的file_stat" 和 
	 iput()->__destroy_inode_handler_post()把file_stat从global temp链表剔除并移动到global delete链表，两个流程全用
	 *file_stat_delete_protect_lock()->test_and_set_bit_lock(file_stat_delete_protect)防护。
	 *test_and_set_bit_lock(file_stat_delete_protect)是原子操作，有内存屏障防护，不用担心并发。这样有两个结果
	 *1：进程1"在当前函数遍历global temp链表的file_stat" 首先test_and_set_bit(file_stat_delete_protect)令变量置1
	 * 然后list_for_each_entry_safe_reverse()遍历获取到global temp链表的file_stat，这个过程进程2因test_and_set_bit(file_stat_delete_protect)
	 * 令变量置1失败，无法执行"在__destroy_inode_handler_post()把file_stat从global temp链表剔除并移动到global delete链表"，而只是把标记file_stat的in_delete标记，
	 * 此时没有并发问题。后续，异步内存回收线程从global temp链表遍历到in_delete标记的file_stat，再把该file_stat移动到global delete链表，完美。
	 *
	 *2：进程2"iput()->__destroy_inode_handler_post()把file_stat从global temp链表剔除并移动到global delete链表" 
	 首先test_and_set_bit(file_stat_delete_protect)令变量置1.此时进程1因file_stat_delete_protect_lock->test_and_set_bit(file_stat_delete_protect)
	 *令变量置1失败而陷入形如while(test_and_set_bit(file_stat_delete_protect)) msleep(1) 的死循环而休眠。等到进程2执行
	 *file_stat_delete_protect_unlock(1)令变量清0，进程1从while(test_and_set_bit(file_stat_delete_protect)) msleep(1)成功令原子变量加1而加锁成功。
	 *后续进程1就可以放心list_for_each_entry_safe_reverse()遍历获取到global temp链表的file_stat，不用担心并发。
	 *
	 *这个并发设计的好处是，"iput()->__destroy_inode_handler_post()"即便test_and_set_bit(file_stat_delete_protect)令变量置1失败，也不用休眠，
	 *而是直接设置file_stat的in_delete标记就可以返回。并且file_stat_delete_protect_lock()和file_stat_delete_protect_unlock之间的代码很少。没有性能问题。
	 *
	 *又想到一个重大bug：
	 *就是list_for_each_entry_safe_reverse()循环最后，然后执行下一次循环，从p_file_stat_temp得到新的p_file_stat，这个过程有问题。就是
	 *这个过程p_file_stat_temp可能被"iput()->__destroy_inode_handler_post()"进程并发移动到global delete链表，这样从p_file_stat_temp得到新的p_file_stat
	 *就是global delete链表的file_stat了，不是global temp链表的file_stat，就会再次复现本case最初的问题。
	 *
	 *解决办法是：进程1先file_stat_delete_protect_lock(1)尝试加锁，如果这个过程p_file_stat_temp被"iput()->__destroy_inode_handler_post()"进程并发移动到
	 *global delete链表，p_file_stat_temp将有in_delete_file标记。然后进程1先file_stat_delete_protect_lock(1)加锁成功，
	 *file_stat_in_delete_file_base(p_file_stat_temp)返回1，此时进程1直接跳出循环，结束遍历。当然也可以重新遍历global temp链表！
	 *
	 *对了，p_file_stat_temp也可能是global temp链表头，这种情况也要结束遍历，因为p_file_stat_temp是非法的file_stat。
	 */
	file_stat_delete_protect_lock(1);
	file_stat_delete_lock = 1;
	/* 从global temp和large_file_temp链表尾遍历N个file_stat，回收冷file_area的。对热file_area、refault file_area、
	 * in_free file_area的各种处理。这个不global lock加锁。但是遇到file_stat移动到其他global 链表才会global lock加锁*/

	/*现在改为把file_stat_base结构体添加到global temp/hot/large/midlde/tiny small/small file链表，不再是file_stat或file_stat_small或file_stat_tiny_small结构体*/
	//list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,file_stat_temp_head,hot_cold_file_list){
	list_for_each_entry_safe_reverse(p_file_stat_base,p_file_stat_base_temp,file_stat_temp_head,hot_cold_file_list){
		/*目前的设计在跳出该循环时，必须保持file_stat_delete_protect_lock的lock状态，于是这个判断要放到break后边。
		 *现在用了file_stat_lock变量辅助，就不需要了*/
		file_stat_delete_protect_unlock(1);
		file_stat_delete_lock = 0;
		
		/*if(scan_file_area_count >= scan_file_area_max || ++scan_file_stat_count > scan_file_stat_max)
			break;*/

		/*测试file_stat状态有没有问题，有问题直接crash*/
		check_file_stat_is_valid(p_file_stat_base,file_stat_list_type,is_cache_file);
		/*如果文件mapping->rh_reserved1保存的file_stat指针不相等，crash，这个检测很关键，遇到过bug。
		 *这个检测必须放到遍历file_stat最开头，防止跳过。global_file_stat不做这个判断，或者再做个其他判断???????
		 *但是global_file_stat肯定不会执行这个函数流程，因此不再做限制了*/
		//is_file_stat_mapping_error(p_file_stat_base); 这个判断放到file_inode_lock()里了，原因看该函数注释

		
		/* 重大隐藏bug：在下边遍历文件file_stat过程，有多出会用到该文件mapping、xarray tree、mapping->i_mmap.rb_root。
		 * 比如cold_file_stat_delete()、cold_file_area_delete()、cache_file_change_to_mmap_file()。这些都得确保该文件inode不能被iput()释放了，
		 * 否则就是无效内存访问。实际测试时确实遇到过上边两处，因为inode被释放了而crash。当然可以单独在这两处单独
		 * file_inode_lock()，但是万一后期又有其他代码使用mapping、xarray tree、mapping->i_mmap.rb_root，万一忘了
		 * 加file_inode_lock()，那就又是要访问inode无效内存了。干脆在遍历文件file_stat最初就加file_inode_lock，
		 * 一劳永逸，之后就能绝对保证该文件inode不会被释放*/
		ret = file_inode_lock(p_file_stat_base);
		if(ret <= 0){
			printk("%s file_stat:0x%llx status 0x%x inode lock fail\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			/* 如果上边加锁失败，但是是因为inode被iput()而inode有了free标记，此时ret是-1，但file_stat不一定有in_delete标记，
			 * 此时不能crash*/
			if(!file_stat_in_delete_base(p_file_stat_base) && (-1 != ret))
				panic("%s file_stat:0x%llx not delete status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

			/* 遇到过在文件iput时，file_stat没有delete标记，但是inode有free标记，于是执行到move_file_stat_to_global_delete_list
			 * 因为file_stat没有delete标记而crash，这里就不再把file_stat移动到global delete链表了。可以吗？反正文件iput->
			 * __destroy_inode_handler_post()函数会把file_stat移动到globa delete链表，这里就不再移动了。不行，
			 * __destroy_inode_handler_post函数如果file_stat_delete_protect_try_lock失败，就不会再把file_stat移动到global 
			 * delete链表了，那这个delete的file_stat就会永久停留在global temp、middle、large链表了。因此异步内存回收线程
			 * 必须要把delete链表移动到global delete链表。只有加个if(file_stat_in_delete_base(p_file_stat_base))限制，
			 * 等稍后iput()里标记该file_stat delete链表后，有了delete标记，再把file_stat移动到global delete链表即可*/
			if(file_stat_in_delete_base(p_file_stat_base))
				move_file_stat_to_global_delete_list(p_hot_cold_file_global,p_file_stat_base,file_type,is_cache_file);

			goto next_file_stat;
		}

		if(p_file_stat_base->recent_traverse_age < p_hot_cold_file_global->global_age)
			p_file_stat_base->recent_traverse_age = p_hot_cold_file_global->global_age;

		/*黑名单文件不扫描，后续最好时把这种file_stat单独移动到一个专有的global file_stat链表上，避免干扰*/
		if(file_stat_in_blacklist_base(p_file_stat_base)){
			if(FILE_STAT_NORMAL == file_type)
				normal_file_stat_no_scan = 1;

			goto next_file_stat_unlock;
		}


		/* 在内存紧张模式，如果file_stat的file_area个数很多，但该文件实际的nr_pages个数很少，跳过这种文件，遍历这种文件的file_area浪费时间收益太低
		 * 把IS_IN_MEMORY_EMERGENCY_RECLAIM改为 !IS_MEMORY_ENOUGH()了，只要内存不充足，就不回收mapping->nrpages少的文件的文件页*/
		if(FILE_STAT_TINY_SMALL != file_type && !IS_MEMORY_ENOUGH(p_hot_cold_file_global) 
				/*&& p_file_stat_base->file_area_count > 0*/ && p_file_stat_base->mapping->nrpages < 6){
			if(FILE_STAT_NORMAL == file_type)
				normal_file_stat_no_scan = 1;

			/*必须goto next_file_stat_unlock，否则就忘了执行file_inode_unlock()，令inode引用计数减1*/
			//goto next_file_stat;
			goto next_file_stat_unlock;
		}
		/*测试cache文件是否能转成mmap文件，是的话转成mmap文件，然后直接continue遍历下一个文件。但是却
		 *引入了一个重大的隐藏bug。continue会导致没有执行file_stat_delete_protect_lock(1)，就跳到for
		 *循环最前边，去遍历下一个file_stat。此时没有加锁，遍历到的file_stat就可能被并发iput()而非法。
		 *就导致出上边的问题了。于是，在遍历global temp/small/tiny small 链表上的file_stat的for循环里，
		 *不能出现continue。解决办法是goto next_file_stat分支，获取下一个file_stat.*/
#if 0		
		if(cache_file_change_to_mmap_file(p_hot_cold_file_global,p_file_stat_base,file_type))
			continue;
#else
		/*cache file_stat转成mmap file_stat，但是不会释放老的cache file_stat*/
		if(is_cache_file && cache_file_change_to_mmap_file(p_hot_cold_file_global,p_file_stat_base,file_type)){
			if(FILE_STAT_NORMAL == file_type)
				normal_file_stat_no_scan = 1;

			goto next_file_stat_unlock;
		}
#endif	

		/* global tiny small链表上只有一个file_area的file_stat移动到global tiny_small_one_area链表。global tiny_small_one_area链表上的file_stat，
		 * 如果file_area个数大于一定数目则移动回global tiny small链表。if成立说明发生了这两种情况，本次循环就结束遍历。有一个隐藏情况，就是第2种
		 * 情况，tiny_small_one_area file_stat因为file_area数目很多，超过64了，其实是可以执行下边的can_tiny_small_file_change_to_small_normal_file()
		 * 直接转成small file_stat或file_stat的，但是该函数原本是针对tiny small file_stat设计的，不是针对tiny_small_one_area file_stat。为了避免
		 * 后续出现状态混乱，这类规定tiny_small_one_area file_stat不能直接转成small file_stat或file_stat，而是先转成tiny small file_stat，再转。
		 *
		 * 并且，还有一点，"把tiny small转成global_file_stat的操作"要放到"cache文件转成mmap文件"之前，否则会把cache文件(实际已经是mmap文件)移动
		 * 到cache global_file_stat，那就转错了。实际应该转到mmap global_file_stat。*/
		/*if(FILE_STAT_TINY_SMALL == file_type && one_file_area_file_stat_tiny_small_solve(p_hot_cold_file_global,p_file_stat_base,is_cache_file)){
			goto next_file_stat_unlock;
		}*/
		if(FILE_STAT_TINY_SMALL == file_type /*&& tiny_small_file_area_move_to_global_file_stat(p_hot_cold_file_global,p_file_stat_base,is_cache_file)*/){
			/*tiny_small_file_area_move_to_global_file_stat()里会把file_stat rcu异步释放掉，这里提前rcu_read_lock，
			 * 等不再使用它时再rcu_read_unlock，然后该file_stat才会被真正释放掉。最后决定把rcu_read_lock()放到该
			 * 函数里边的call_rcu()异步释放file_stat前边了，确保rcu_read_lock后不会有休眠*/
			//rcu_read_lock();
			//rcu_read_lock_flag = 1;
			if(tiny_small_file_area_move_to_global_file_stat(p_hot_cold_file_global,p_file_stat_base,is_cache_file)){
				rcu_read_lock_flag = 1;
				goto next_file_stat_unlock;
			}
		}

		/* tiny small文件的file_area个数如果超过阀值则转换成small或normal文件等。这个操作必须放到get_file_area_from_file_stat_list_common()
		 * 函数里遍历该file_stat的file_area前边，以保证该文件的in_refault、in_hot、in_free属性的file_area都集中在tiny small->temp链表尾的64
		 * file_area，后续即便大量新增file_area，都只在tiny small->temp链表头，详情见can_tiny_small_file_change_to_small_normal_file()注释*/
		if(FILE_STAT_TINY_SMALL == file_type && unlikely(p_file_stat_base->file_area_count > SMALL_FILE_AREA_COUNT_LEVEL)){
			/*can_tiny_small_file_change_to_small_normal_file()里会把file_stat rcu异步释放掉，这里提前rcu_read_lock，
			 * 等不再使用它时再rcu_read_unlock，然后该file_stat才会被真正释放掉。最后决定把rcu_read_lock()放到该
			 * 函数里边的call_rcu()异步释放file_stat前边了，确保rcu_read_lock后不会有休眠*/
			//rcu_read_lock();
			//rcu_read_lock_flag = 1;

			p_file_stat_tiny_small = container_of(p_file_stat_base,struct file_stat_tiny_small,file_stat_base);
			if(can_tiny_small_file_change_to_small_normal_file(p_hot_cold_file_global,p_file_stat_tiny_small,is_cache_file))
				rcu_read_lock_flag = 1;
		}
		/* small文件的file_area个数如果超过阀值则转换成normal文件等。这个操作必须放到get_file_area_from_file_stat_list_common()
		 * 函数里遍历该file_stat的file_area前边，以保证该文件的in_refault、in_hot、in_free属性的file_area都集中在small->other链表尾的640
		 * 个file_area，后续即便大量新增file_area，都只在small->other链表头，详情见can_small_file_change_to_normal_file()注释*/
		else if(FILE_STAT_SMALL == file_type && unlikely(p_file_stat_base->file_area_count > NORMAL_TEMP_FILE_AREA_COUNT_LEVEL)){
			/*can_small_file_change_to_normal_file()里会把file_stat rcu异步释放掉，这里提前rcu_read_lock，
			 * 等不再使用它时再rcu_read_unlock，然后该file_stat才会被真正释放掉*/
			//rcu_read_lock();
			//rcu_read_lock_flag = 1;

			p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
			if(can_small_file_change_to_normal_file(p_hot_cold_file_global,p_file_stat_small,is_cache_file))
				rcu_read_lock_flag = 1;
		}
		else{
			if(FILE_STAT_NORMAL == file_type){
				p_file_stat = container_of(p_file_stat_base, struct file_stat, file_stat_base);
				file_stat_traverse_warm_list_num = p_file_stat->traverse_warm_list_num;
			}
			/*否则，normal、small、tiny small这3大类文件，按照标准流程处理他们的各种file_area*/
			scan_file_area_count += get_file_area_from_file_stat_list_common(p_hot_cold_file_global,p_file_stat_base,scan_file_area_max,file_stat_list_type,file_type,is_cache_file);
			/*如果遍历过的file_stat的warm链表上的file_area被遍历完成了，p_file_stat->traverse_warm_list_num就会更新，file_stat_warm_or_writeonly_file_area_check_ok是1，否则0*/
			if(FILE_STAT_NORMAL == file_type){
				file_stat_warm_or_writeonly_file_area_check_ok = file_stat_traverse_warm_list_num != p_file_stat->traverse_warm_list_num;
			}
		}

next_file_stat_unlock:

		file_inode_unlock(p_file_stat_base);

next_file_stat:

		/* normal file_stat如果没有正常遍历file_ara，比如cache文件转成mmap文件而不遍历，cache/mmap文件因nr_pages太少导致而不遍历，或者黑名单问题
		 * 则置1。此时如果所属文件类型的current_scan_file_stat_info->p_traverse_file_stat 不是NULL则要清NULL。否则下个周期遍历新的file_stat时，
		 * 发现current_scan_file_stat_info->p_traverse_file_stat 不是NULL，则判定上次遍历的file_stat的file_area有问题，而panic*/
		if(normal_file_stat_no_scan){
			struct current_scan_file_stat_info *p_current_scan_file_stat_info = get_normal_file_stat_current_scan_file_stat_info(p_hot_cold_file_global,1 << file_stat_list_type,is_cache_file);
			p_file_stat = container_of(p_file_stat_base, struct file_stat, file_stat_base);

			if(warm_list_printk)
				printk("%s file_stat:0x%llx status 0x%x normal_file_stat_no_scan\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

			/*p_current_scan_file_stat_info->p_traverse_file_stat设置为NULL，下次遍历file_stat直接从链表尾file_stat开始遍历*/
			update_file_stat_next_multi_level_warm_or_writeonly_list(p_current_scan_file_stat_info,p_file_stat);

			normal_file_stat_no_scan = 0;
		}

		//p_file_stat_base_last = p_file_stat_base;
		if(warm_list_printk)
			printk("%s file_stat:0x%llx status 0x%x is_cache_file:%d scan_file_area_count:%d scan_file_area_max:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,is_cache_file,scan_file_area_count,scan_file_area_max);

       	file_stat_delete_protect_lock(1);
		file_stat_delete_lock = 1;
		/*这里有个很严重的隐患，这里最初竟然是判断p_file_stat_base是否delete了，而不是p_file_stat_base_temp是否delete，这个bug会导致什么错误，未知!!!!!!!!!!!!*/
		//if(&p_file_stat_base_temp->hot_cold_file_list == file_stat_temp_head  || file_stat_in_delete_file_base(p_file_stat_base))
		if(&p_file_stat_base_temp->hot_cold_file_list == file_stat_temp_head  || file_stat_in_delete_file_base(p_file_stat_base_temp))
			break;
		
		/* break必须放到这里，不能放到for循环的上边。否则因为scan_file_area_count > scan_file_area_max 而break后，
		 * 当前的p_file_stat_base对应的文件，就无法遍历，内存回收。并且，如果这个file_stat如果还是链表头的，就会
		 * 在下边list_move_enhance()失败，因为它是链表头的file_stat。如此导致file_stat长时间无法遍历到，对它内存回收*/
		if(scan_file_area_count >= scan_file_area_max || ++scan_file_stat_count > scan_file_stat_max)
			break;

		/*回收的page数达到预期目标，结束回收，避免过度内存回收。不能>=，因为内存IDLE模式下，reclaim_pages_target是0，alreay_reclaim_pages也是0，此时这里不能break*/
		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			break;

		/* rcu_read_lock_flag是1，说明tiny_small可能转成normal file_stat而被rcu异步释放掉了。但是由于提前rcu_read_lock了，
		 * 不用担心file_stat会被立即释放掉。于是这里才rcu_read_unlock，然后file_stat才会被真正释放掉。
		 * 到这里100%确保file_stat不会再被使用，再放开rcu，然后file_stat才会被真正释放掉*/
		if(rcu_read_lock_flag){
			rcu_read_lock_flag = 0;
			rcu_read_unlock();
		}

		/* 必须把赋值放到for循环最后，目的是：如果上边break跳出了for循环，p_file_stat_base_last此时才真的是上一次循环遍历到的file_stat。
		 * 但是有个新的问题，如果file_stat被转成mmap file_stat了，file_stat在temp、middel、large之间发生变化移动到了新的链表，
		 * 此时下边list_move_enhance()该p_file_stat_base_last就是无效的了，不会再把它之后的遍历的file_stat移动到链表头了*/
		p_file_stat_base_last = p_file_stat_base;
	}
	if(file_stat_delete_lock)
		file_stat_delete_protect_test_unlock(1);

	/*当前p_file_stat_base的warm链表上的file_area还没有遍历完，只是把它后边的遍历过的file_stat移动到链表头*/
	if(FILE_STAT_NORMAL == file_type && !file_stat_warm_or_writeonly_file_area_check_ok){
		if(warm_list_printk)
			printk("%s file_stat:0x%llx status 0x%x not traverse warm_list area , use p_file_stat_base_last:0x%llx move to head\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,(u64)p_file_stat_base_last);

		p_file_stat_base = p_file_stat_base_last;
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
		if(shrink_page_printk_open_important)
			printk("***get_file_area_from_file_stat_list() file_stat_base:0x%llx head:0x%llx base:0x%llx file_stat_list_type:%d file_type:%d is_cache_file:%d\n",(u64)p_file_stat_base,(u64)&p_file_stat_base->hot_cold_file_list,(u64)file_stat_temp_head,file_stat_list_type,file_type,is_cache_file);

		if(is_cache_file)
			spin_lock(&p_hot_cold_file_global->global_lock);
		else
			spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
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
		if(&p_file_stat_base->hot_cold_file_list != file_stat_temp_head  && !file_stat_in_delete_base(p_file_stat_base)){
			/*将链表尾已经遍历过的file_stat移动到链表头，下次从链表尾遍历的才是新的未遍历过的file_stat。这个过程必须加锁*/
			if(can_file_stat_move_to_list_head(file_stat_temp_head,p_file_stat_base,file_stat_list_type,is_cache_file)){
				list_move_enhance(file_stat_temp_head,&p_file_stat_base->hot_cold_file_list);
				
				if(shrink_page_printk_open_important)
					printk("***get_file_area_from_file_stat_list() 222\n");
			}else{
				/* 针对temp、middle、large、writeonly文件，链表成员file_stat1 <-> file_stat2 <-> file_stat3，file_stat2
				 * 和file_stat3遍历完，file_stat2由temp文件升级为hot文件。然后遍历file_stat1->warm链表上的file_area，
				 * 遍历的file_area个数超过max而遍历结束。因此执行到这里，要把file_stat1后边的file_stat2到global->temp
				 * 链表尾的file_area移动到global->temp链表头。但是因为file_stat2不再是temp文件，can_file_stat_move_to_list_head()
				 * 计算失败，此时p_current_scan_file_stat_info->p_traverse_file_stat此时指向file_stat1。之后再遍历global->temp链表
				 * 的file_stat，p_current_scan_file_stat_info->p_traverse_file_stat是file_stat1，但是global->temp链表
				 * 尾的file_stat是file_stat3，就触发panic。因此，这里的代码必须把p_current_scan_file_stat_info->p_traverse_file_stat设置为NULL*/
				if(FILE_STAT_NORMAL == file_type && !file_stat_warm_or_writeonly_file_area_check_ok){
					struct current_scan_file_stat_info *p_current_scan_file_stat_info = get_normal_file_stat_current_scan_file_stat_info(p_hot_cold_file_global,1 << file_stat_list_type,is_cache_file);
					p_file_stat = container_of(p_file_stat_base, struct file_stat, file_stat_base);

					printk("%s file_stat:0x%llx status 0x%x move to head fail\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
					/*p_current_scan_file_stat_info->p_traverse_file_stat设置为NULL，下次遍历file_stat直接从链表尾file_stat开始遍历*/
					update_file_stat_next_multi_level_warm_or_writeonly_list(p_current_scan_file_stat_info,p_file_stat);
				}
			}
		}

		if(is_cache_file)
			spin_unlock(&p_hot_cold_file_global->global_lock);
		else
			spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
	}
	/* rcu_read_lock_flag是1，说明上边遍历的最后一个file_stat，可能由于tiny_small可能转成normal file_stat而被rcu异步释放掉了
	 * 。但是由于提前rcu_read_lock了，不用担心file_stat会被立即释放掉。于是这里才rcu_read_unlock，然后file_stat才会被真正释放掉*/
	if(rcu_read_lock_flag){
		rcu_read_lock_flag = 0;
		rcu_read_unlock();
	}


	if(shrink_page_printk_open1)
		printk("3:%s %s %d p_hot_cold_file_global:0x%llx scan_file_stat_count:%d scan_file_area_count:%d real_scan_file_stat_count:%d\n",__func__,current->comm,current->pid,(u64)p_hot_cold_file_global,scan_file_stat_count,scan_file_area_count,/*scan_cold_file_area_count,file_stat_count_in_cold_list*/real_scan_file_stat_count);

	//扫描的file_area个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_file_area_count += scan_file_area_count;
	//扫描的file_stat个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_file_stat_count += scan_file_stat_count;
	//扫描到的处于delete状态的file_stat个数
	//p_hot_cold_file_global->hot_cold_file_shrink_counter.scan_delete_file_stat_count += scan_delete_file_stat_count;

	return scan_file_area_count;
}
static void memory_reclaim_param_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct memory_reclaim_param *p_memory_reclaim_param,char is_cache_file)
{
	if(is_cache_file){
		switch(p_hot_cold_file_global->memory_pressure_level){
			/*内存非常紧缺*/
			case MEMORY_EMERGENCY_RECLAIM:
				p_memory_reclaim_param->scan_tiny_small_file_stat_max  = 512;
				p_memory_reclaim_param->scan_small_file_stat_max  = 256;
				p_memory_reclaim_param->scan_temp_file_stat_max   = 256;
				p_memory_reclaim_param->scan_middle_file_stat_max = 128;
				p_memory_reclaim_param->scan_large_file_stat_max  = 64;
				p_memory_reclaim_param->scan_writeonly_file_stat_max  = 256;

				p_memory_reclaim_param->scan_tiny_small_file_area_max  = 1024 + 256;
				p_memory_reclaim_param->scan_small_file_area_max  = 512 + 256;
				/* 为什么要把temp、middle、large scan_file_area个数调到5120这么大？因为实际调试发现，有些writeonly文件，
				 * nrpages有上千个，但是这些file_stat都在global temp链表头，长时间无法被遍历到而移动到global writeonly
				 * 文件，无法被尽快内存回收，导致kswapd频繁内存回收而导致refault偏大。但是测试发现，新的问题又来了，
				 * 异步内存回收线程因为scan_temp_file_area_max调大到5120，大量内存回收，kswapd一个page都没回收，也就
				 * 一个refault page都没有。但是异步内存回收因为大量内存回收page，热fault page就大大加大了。毕竟，只要
				 * 内存回收就有概率发生refault。内核原生的惰性内存回收方案，只要不内存回收就不会refault。并且write page
				 * 只留存于inactive lru链表，即便频繁访问也不移动到active lru链表。而read page访问一次只会移动到
				 * inacitve lru链表，访问两次才会移动到active lru链表。用rocky9.2原生内核mysql 低内存压测时，
				 * refault page稳定在2k~5k，并不多，异步内存回收refaut page会波动到1w，为什么？因为mysql压测优先回收
				 * inactive lru链表上的write page，这些page不会产生refualt。但是，inactive lru链表或多或少都有
				 * read page。这些read page也会被kswapd内存回收，再被访问就会refault，这也是kswapd内存回收发生
				 * refault的根因。内存回收
				 * 1：没有哪种内存回收方案时完美的，只要内存回收就有refault的概率，回收的page越多，refault概率越大
				 * 2：内存回收要优先回收writeonly的文件的文件页page。必须要想办法在几千个文件file_stat中快速遍历到
				 * writeonly的文件，尤其是文件页page很多的writeonly文件。具体措施
				 *  2.1 弄一个glboal writeonly链表，writeonly文件都移动上去，内存回收先遍历这个链表上的文件
				 *  2.2 tiny small或small文件因文件页个数超过阈值而移动到global temp、middel、large链表时，
				 *  writeonly文件移动到global temp、middel、large链表尾，read文件移动到链表头，这样可以优先
				 *  被异步内存回收线程遍历到writeonly文件，而加快回收writeonly文件
				 * 3：内存回收要优先回收read 文件的writeonly文件页
				 *
				 * 那是不是还要保持scan_temp_file_area_max 5120这么大呢？这么大必然会扫描到很多read文件，从而回收read文件的
				 * 文件页，加大了refault的概率。可以这样调节：read文件设置scan_file_area_max上限，比如256，避免遍历太多file_area。
				 * 但是， get_file_area_from_file_stat_list中还会检测scan_file_area > max 就会结束遍历文件
				 *
				 * */
				p_memory_reclaim_param->scan_temp_file_area_max   = 5120;
				//p_memory_reclaim_param->scan_temp_file_area_max   = 2560;
				p_memory_reclaim_param->scan_middle_file_area_max = 2560;
				p_memory_reclaim_param->scan_large_file_area_max  = 2560;
				p_memory_reclaim_param->scan_writeonly_file_area_max  = 1024;

				p_memory_reclaim_param->scan_hot_file_area_max = 512;
				break;
				/*内存紧缺*/
			case MEMORY_PRESSURE_RECLAIM:
				p_memory_reclaim_param->scan_tiny_small_file_stat_max = 512;
				p_memory_reclaim_param->scan_small_file_stat_max  = 128;
				p_memory_reclaim_param->scan_temp_file_stat_max   = 128;
				p_memory_reclaim_param->scan_middle_file_stat_max = 64;
				p_memory_reclaim_param->scan_large_file_stat_max  = 64;
				p_memory_reclaim_param->scan_writeonly_file_stat_max  = 64;

				p_memory_reclaim_param->scan_tiny_small_file_area_max  = 512;
				p_memory_reclaim_param->scan_small_file_area_max  = 512;
				p_memory_reclaim_param->scan_temp_file_area_max   = 1024;
				p_memory_reclaim_param->scan_middle_file_area_max = 1024;
				p_memory_reclaim_param->scan_large_file_area_max  = 1024;
				p_memory_reclaim_param->scan_writeonly_file_area_max  = 1024;

				p_memory_reclaim_param->scan_hot_file_area_max = 256;
				break;
				/*内存碎片有点多，或者前后两个周期分配的内存数太多*/		
			case MEMORY_LITTLE_RECLAIM:
				p_memory_reclaim_param->scan_tiny_small_file_stat_max  = 512;
				p_memory_reclaim_param->scan_small_file_stat_max  = 32;
				p_memory_reclaim_param->scan_temp_file_stat_max   = 32;
				p_memory_reclaim_param->scan_middle_file_stat_max = 16;
				p_memory_reclaim_param->scan_large_file_stat_max  = 8;
				p_memory_reclaim_param->scan_writeonly_file_stat_max  = 8;

				p_memory_reclaim_param->scan_tiny_small_file_area_max  = 256;
				p_memory_reclaim_param->scan_small_file_area_max  = 128;
				p_memory_reclaim_param->scan_temp_file_area_max   = 128;
				p_memory_reclaim_param->scan_middle_file_area_max = 128;
				p_memory_reclaim_param->scan_large_file_area_max  = 128;
				p_memory_reclaim_param->scan_writeonly_file_area_max  = 128;

				p_memory_reclaim_param->scan_hot_file_area_max  = 128;
				break;

				/*一般情况*/
			default:
				/*设置大点是为了尽量多扫描so、可执行文件这种原本是mmap的文件但最初被判定为cache文件*/
				p_memory_reclaim_param->scan_tiny_small_file_stat_max  = 512;
				p_memory_reclaim_param->scan_small_file_stat_max  = 32;
				p_memory_reclaim_param->scan_temp_file_stat_max   = 32;
				p_memory_reclaim_param->scan_middle_file_stat_max = 16;
				p_memory_reclaim_param->scan_large_file_stat_max  = 8;
				p_memory_reclaim_param->scan_writeonly_file_stat_max  = 8;

				p_memory_reclaim_param->scan_tiny_small_file_area_max  = 256;
				p_memory_reclaim_param->scan_small_file_area_max  = 128;
				p_memory_reclaim_param->scan_temp_file_area_max   = 64;
				/* 目前遇到一个问题，在内存空闲时，middle或large文件，一次只扫描64个file_area，导致始终只扫描到global middle/large链表尾
				 * 的file_stat，因此这种file_stat的file_area很多，扫一个文件就扫够了。导致扫描不到其他file_stat，而其他file_stat又有大量
				 * 的空闲file_area确无法参与内存回收。注意，在global middle/large只有两个file_stat时遇到这种情况??????????????????????*/
				p_memory_reclaim_param->scan_middle_file_area_max = 64;
				p_memory_reclaim_param->scan_large_file_area_max  = 64;
				p_memory_reclaim_param->scan_writeonly_file_area_max  = 64;

				p_memory_reclaim_param->scan_hot_file_area_max  = 64;

				break;
		}
	}else{
		switch(p_hot_cold_file_global->memory_pressure_level){
			/*内存非常紧缺*/
			case MEMORY_EMERGENCY_RECLAIM:
				p_memory_reclaim_param->scan_tiny_small_file_stat_max  = 512;
				p_memory_reclaim_param->scan_small_file_stat_max  = 128;
				p_memory_reclaim_param->scan_temp_file_stat_max   = 128;
				p_memory_reclaim_param->scan_middle_file_stat_max = 64;
				p_memory_reclaim_param->scan_large_file_stat_max  = 32;

				p_memory_reclaim_param->scan_tiny_small_file_area_max  = 1024 + 256;
				p_memory_reclaim_param->scan_small_file_area_max  = 512 + 256;
				p_memory_reclaim_param->scan_temp_file_area_max   = 512;
				p_memory_reclaim_param->scan_middle_file_area_max = 512;
				p_memory_reclaim_param->scan_large_file_area_max  = 512;

				p_memory_reclaim_param->scan_hot_file_area_max = 512;
				p_memory_reclaim_param->mapcount_file_area_max = 128;
				break;
				/*内存紧缺*/
			case MEMORY_PRESSURE_RECLAIM:
				p_memory_reclaim_param->scan_tiny_small_file_stat_max  = 512;
				p_memory_reclaim_param->scan_small_file_stat_max  = 64;
				p_memory_reclaim_param->scan_temp_file_stat_max   = 64;
				p_memory_reclaim_param->scan_middle_file_stat_max = 32;
				p_memory_reclaim_param->scan_large_file_stat_max  = 16;

				p_memory_reclaim_param->scan_tiny_small_file_area_max  = 512;
				p_memory_reclaim_param->scan_small_file_area_max  = 512;
				p_memory_reclaim_param->scan_temp_file_area_max   = 256;
				p_memory_reclaim_param->scan_middle_file_area_max = 256;
				p_memory_reclaim_param->scan_large_file_area_max  = 256;

				p_memory_reclaim_param->scan_hot_file_area_max = 256;
				p_memory_reclaim_param->mapcount_file_area_max = 128;
				break;
				/*内存碎片有点多，或者前后两个周期分配的内存数太多*/		
			case MEMORY_LITTLE_RECLAIM:
				p_memory_reclaim_param->scan_tiny_small_file_stat_max  = 512;
				p_memory_reclaim_param->scan_small_file_stat_max  = 32;
				p_memory_reclaim_param->scan_temp_file_stat_max   = 32;
				p_memory_reclaim_param->scan_middle_file_stat_max = 16;
				p_memory_reclaim_param->scan_large_file_stat_max  = 8;

				p_memory_reclaim_param->scan_tiny_small_file_area_max  = 256;
				p_memory_reclaim_param->scan_small_file_area_max  = 128;
				p_memory_reclaim_param->scan_temp_file_area_max   = 128;
				p_memory_reclaim_param->scan_middle_file_area_max = 128;
				p_memory_reclaim_param->scan_large_file_area_max  = 128;

				p_memory_reclaim_param->scan_hot_file_area_max  = 128;
				p_memory_reclaim_param->mapcount_file_area_max = 64;
				break;

				/*一般情况*/
			default:
				/*设置大点是为了尽量多扫描so、可执行文件这种原本是mmap的文件但最初被判定为cache文件*/
				p_memory_reclaim_param->scan_tiny_small_file_stat_max  = 512;
				p_memory_reclaim_param->scan_small_file_stat_max  = 32;
				p_memory_reclaim_param->scan_temp_file_stat_max   = 32;
				p_memory_reclaim_param->scan_middle_file_stat_max = 16;
				p_memory_reclaim_param->scan_large_file_stat_max  = 8;

				p_memory_reclaim_param->scan_tiny_small_file_area_max  = 256;
				p_memory_reclaim_param->scan_small_file_area_max  = 128;
				p_memory_reclaim_param->scan_temp_file_area_max   = 64;
				p_memory_reclaim_param->scan_middle_file_area_max = 64;
				p_memory_reclaim_param->scan_large_file_area_max  = 64;

				p_memory_reclaim_param->scan_hot_file_area_max  = 64;
				p_memory_reclaim_param->mapcount_file_area_max = 32;
				break;
		}
	}
}

static void deleted_file_stat_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct list_head *file_stat_delete_head,unsigned int scan_file_area_max,unsigned char file_type,char is_cache_file)
{ 
	unsigned int del_file_stat_count = 0,del_file_area_count = 0,del_file_area_count_temp = 0;
	struct file_stat_base *p_file_stat_base,*p_file_stat_base_temp;

	/*现在改为把file_stat_base结构体添加到global temp/hot/large/midlde/tiny small/small file链表，不再是file_stat或file_stat_small或file_stat_tiny_small结构体*/
	//list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->file_stat_delete_head,hot_cold_file_list){
	list_for_each_entry_safe_reverse(p_file_stat_base,p_file_stat_base_temp,file_stat_delete_head,hot_cold_file_list){
		if(!file_stat_in_delete_base(p_file_stat_base) /*|| file_stat_in_delete_error(p_file_stat)*/)
			panic("%s file_stat:0x%llx not delete status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

		//del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,&p_file_stat->file_stat_base,FILE_STAT_NORMAL);
		del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat_base,file_type,is_cache_file);
		del_file_stat_count ++;
		/*防止删除file_area耗时太长*/
		if(++ del_file_area_count_temp > scan_file_area_max)
			break;
	}

	if(is_cache_file){
		p_hot_cold_file_global->hot_cold_file_shrink_counter.del_file_area_count = del_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.del_file_stat_count = del_file_stat_count;
	}else{
		p_hot_cold_file_global->hot_cold_file_shrink_counter.del_file_area_count = del_file_area_count;
		p_hot_cold_file_global->hot_cold_file_shrink_counter.del_file_stat_count = del_file_stat_count;
	}
}
static noinline void walk_throuth_all_file_area(struct hot_cold_file_global *p_hot_cold_file_global,char is_cache_file)
{
	//struct file_stat * p_file_stat,*p_file_stat_temp;
	//struct file_stat_small * p_file_stat_small,*p_file_stat_small_temp;
	//struct file_stat_tiny_small * p_file_stat_tiny_small,*p_file_stat_tiny_small_temp;

	//struct file_stat_base *p_file_stat_base,*p_file_stat_base_temp;
	//struct file_area *p_file_area,*p_file_area_temp;
	/*unsigned int scan_hot_file_area_max;
	  unsigned int scan_temp_file_area_max,scan_temp_file_stat_max;
	  unsigned int scan_middle_file_area_max,scan_middle_file_stat_max;
	  unsigned int scan_large_file_area_max,scan_large_file_stat_max;
	  unsigned int scan_small_file_area_max,scan_small_file_stat_max;
	  unsigned int scan_tiny_small_file_area_max,scan_tiny_small_file_stat_max;*/
	unsigned int scan_cold_file_area_count = 0;
	//unsigned int file_area_hot_to_warm_list_count = 0;
	//unsigned int del_file_stat_count = 0,del_file_area_count = 0,del_file_area_count_temp;
	struct memory_reclaim_param memory_reclaim_param;
	struct memory_reclaim_param *param = &memory_reclaim_param;

	memset(&memory_reclaim_param,0,sizeof(struct memory_reclaim_param));
	/*根据当前的内存状态调整各个内存回收age差参数*/
	change_global_age_dx(p_hot_cold_file_global);

	memory_reclaim_param_solve(p_hot_cold_file_global,&memory_reclaim_param,is_cache_file);

	printk("global_age:%d reclaim_pages_target:%d alreay_reclaim_pages:%d memory_pressure_level:%d memory_still_memrgency_after_reclaim:%d scan_temp_file_stat_max:%d scan_temp_file_area_max:%d scan_middle_file_stat_max:%d scan_middle_file_area_max:%d scan_large_file_stat_max:%d scan_large_file_area_max:%d scan_hot_file_area_max:%d file_area_temp_to_cold_age_dx:%d file_area_hot_to_temp_age_dx:%d file_area_refault_to_temp_age_dx:%d mapcount_file_area_max:%d scan_large_file_area_max:%d scan_large_file_stat_max:%d\n",p_hot_cold_file_global->global_age,p_hot_cold_file_global->reclaim_pages_target,p_hot_cold_file_global->alreay_reclaim_pages,p_hot_cold_file_global->memory_pressure_level,p_hot_cold_file_global->memory_still_memrgency_after_reclaim,param->scan_temp_file_stat_max,param->scan_temp_file_area_max,param->scan_middle_file_stat_max,param->scan_middle_file_area_max,param->scan_large_file_stat_max,param->scan_large_file_area_max,param->scan_hot_file_area_max,p_hot_cold_file_global->file_area_temp_to_cold_age_dx,p_hot_cold_file_global->file_area_hot_to_temp_age_dx,p_hot_cold_file_global->file_area_refault_to_temp_age_dx,param->mapcount_file_area_max,param->scan_large_file_area_max,param->scan_large_file_stat_max);

	if(is_cache_file){
		scan_cold_file_area_count += file_stat_multi_level_warm_or_writeonly_list_file_area_solve(p_hot_cold_file_global,&p_hot_cold_file_global->global_file_stat.current_scan_file_stat_info,&p_hot_cold_file_global->global_file_stat.file_stat.file_stat_base,512,is_cache_file,-1);

		/*优先回收writeonly文件页*/
		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_writeonly_file_area_max,param->scan_writeonly_file_stat_max, 
				&p_hot_cold_file_global->file_stat_writeonly_file_head,F_file_stat_in_file_stat_writeonly_file_head_list,FILE_STAT_NORMAL,is_cache_file);

		/*针对writeonly文件内存回收，内存已经充足，不再继续内存回收。以为只要内存回收就加大内存回收的概率。但现在引入多层warm机制了，可以放开回收了*/
#if 0
		if(check_memory_reclaim_necessary(p_hot_cold_file_global) < MEMORY_PRESSURE_RECLAIM){
			printk("memory enough,do not reclaim!!!!!!!\n");
			return 0;
		}
#endif		
		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			return;

		/* 遍历hot_cold_file_global->file_stat_temp_large_file_head链表尾巴上边的文件file_stat，再遍历每一个文件file_stat->temp、warm
		 * 链表尾上的file_area，判定是冷file_area的话则参与内存回收，内存回收后的file_area移动到file_stat->free链表。然后对
		 * file_stat->refault、hot、free链表上file_area进行对应处理*/
		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_large_file_area_max,param->scan_large_file_stat_max, 
				&p_hot_cold_file_global->file_stat_large_file_head,F_file_stat_in_file_stat_large_file_head_list,FILE_STAT_NORMAL,is_cache_file);

		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			return;

		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_middle_file_area_max,param->scan_middle_file_stat_max, 
				&p_hot_cold_file_global->file_stat_middle_file_head,F_file_stat_in_file_stat_middle_file_head_list,FILE_STAT_NORMAL,is_cache_file);

		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			return;

		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_temp_file_area_max,param->scan_temp_file_stat_max, 
				&p_hot_cold_file_global->file_stat_temp_head,F_file_stat_in_file_stat_temp_head_list,FILE_STAT_NORMAL,is_cache_file);

		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			return;

		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_small_file_area_max,param->scan_small_file_stat_max,
				&p_hot_cold_file_global->file_stat_small_file_head,F_file_stat_in_file_stat_small_file_head_list,FILE_STAT_SMALL,is_cache_file);

		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			return;

		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_tiny_small_file_area_max,param->scan_tiny_small_file_stat_max,
				&p_hot_cold_file_global->file_stat_tiny_small_file_head,F_file_stat_in_file_stat_tiny_small_file_head_list,FILE_STAT_TINY_SMALL,is_cache_file);

		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			return;

		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_tiny_small_file_area_max,param->scan_tiny_small_file_stat_max,
				&p_hot_cold_file_global->file_stat_tiny_small_file_one_area_head,F_file_stat_in_file_stat_tiny_small_file_one_area_head_list,FILE_STAT_TINY_SMALL,is_cache_file);
	}else{

		/*在change_global_age_dx()基础上，针对mmap文件增大age_dx，以使mmap的文件页page更不容易回收*/
		change_global_age_dx_for_mmap_file(p_hot_cold_file_global);

		scan_cold_file_area_count += file_stat_multi_level_warm_or_writeonly_list_file_area_solve(p_hot_cold_file_global,&p_hot_cold_file_global->global_mmap_file_stat.current_scan_file_stat_info,&p_hot_cold_file_global->global_mmap_file_stat.file_stat.file_stat_base,512,is_cache_file,-1);

		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			return;

		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_large_file_area_max,param->scan_large_file_stat_max, 
				&p_hot_cold_file_global->mmap_file_stat_large_file_head,F_file_stat_in_file_stat_large_file_head_list,FILE_STAT_NORMAL,is_cache_file);

		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			return;

		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_middle_file_area_max,param->scan_middle_file_stat_max, 
				&p_hot_cold_file_global->mmap_file_stat_middle_file_head,F_file_stat_in_file_stat_middle_file_head_list,FILE_STAT_NORMAL,is_cache_file);

		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			return;

		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_temp_file_area_max,param->scan_temp_file_stat_max, 
				&p_hot_cold_file_global->mmap_file_stat_temp_head,F_file_stat_in_file_stat_temp_head_list,FILE_STAT_NORMAL,is_cache_file);

		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			return;

		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_small_file_area_max,param->scan_small_file_stat_max,
				&p_hot_cold_file_global->mmap_file_stat_small_file_head,F_file_stat_in_file_stat_small_file_head_list,FILE_STAT_SMALL,is_cache_file);

		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			return;

		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_tiny_small_file_area_max,param->scan_tiny_small_file_stat_max,
				&p_hot_cold_file_global->mmap_file_stat_tiny_small_file_head,F_file_stat_in_file_stat_tiny_small_file_head_list,FILE_STAT_TINY_SMALL,is_cache_file);

		if(p_hot_cold_file_global->alreay_reclaim_pages > p_hot_cold_file_global->reclaim_pages_target)
			return;

		scan_cold_file_area_count += get_file_area_from_file_stat_list(p_hot_cold_file_global,param->scan_tiny_small_file_area_max,param->scan_tiny_small_file_stat_max,
				&p_hot_cold_file_global->mmap_file_stat_tiny_small_file_one_area_head,F_file_stat_in_file_stat_tiny_small_file_one_area_head_list,FILE_STAT_TINY_SMALL,is_cache_file);
	}

	/* 遍历global hot链表上的file_stat，再遍历这些file_stat->hot链表上的file_area，如果不再是热的，则把file_area
	 * 移动到file_stat->warm链表。如果file_stat->hot链表上的热file_area个数减少到热文件阀值以下，则降级到
	 * global temp、middle_file、large_file链表*/
	if(is_cache_file)
		hot_file_stat_solve(p_hot_cold_file_global,&p_hot_cold_file_global->file_stat_hot_head,param->scan_hot_file_area_max,is_cache_file);
	else
		hot_file_stat_solve(p_hot_cold_file_global,&p_hot_cold_file_global->mmap_file_stat_hot_head,param->scan_hot_file_area_max,is_cache_file);

	/*global mapcount链表上的files_stat的处理*/
	if(0 == is_cache_file){     
        scan_mmap_mapcount_file_stat(p_hot_cold_file_global,param->mapcount_file_area_max);
	}

	if(0 == test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		return;

#if 0
	/*遍历global file_stat_delete_head链表上已经被删除的文件的file_stat，
	  一次不能删除太多的file_stat对应的file_area，会长时间占有cpu，后期需要调优一下*/
	del_file_area_count_temp = 0;
	/*现在改为把file_stat_base结构体添加到global temp/hot/large/midlde/tiny small/small file链表，不再是file_stat或file_stat_small或file_stat_tiny_small结构体*/
	//list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->file_stat_delete_head,hot_cold_file_list){
	list_for_each_entry_safe_reverse(p_file_stat_base,p_file_stat_base_temp,&p_hot_cold_file_global->file_stat_delete_head,hot_cold_file_list){
		if(!file_stat_in_delete_base(p_file_stat_base) /*|| file_stat_in_delete_error(p_file_stat)*/)
			panic("%s file_stat:0x%llx not delete status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

		//del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,&p_file_stat->file_stat_base,FILE_STAT_NORMAL);
		del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat_base,FILE_STAT_NORMAL);
		del_file_stat_count ++;
		/*防止删除file_area耗时太长*/
		if(++ del_file_area_count_temp > 256)
			break;
	}
	
	del_file_area_count_temp = 0;
	//list_for_each_entry_safe_reverse(p_file_stat_small,p_file_stat_small_temp,&p_hot_cold_file_global->file_stat_small_delete_head,hot_cold_file_list){
	list_for_each_entry_safe_reverse(p_file_stat_base,p_file_stat_base_temp,&p_hot_cold_file_global->file_stat_small_delete_head,hot_cold_file_list){
		if(!file_stat_in_delete_base(p_file_stat_base) /*|| file_stat_in_delete_error(p_file_stat)*/)
			panic("%s file_stat:0x%llx not delete status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

		del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat_base,FILE_STAT_SMALL);
		del_file_stat_count ++;
		/*防止删除file_area耗时太长*/
		if(++ del_file_area_count_temp > 256)
			break;
	}

	del_file_area_count_temp = 0;
	//list_for_each_entry_safe_reverse(p_file_stat_tiny_small,p_file_stat_tiny_small_temp,&p_hot_cold_file_global->file_stat_tiny_small_delete_head,hot_cold_file_list){
	list_for_each_entry_safe_reverse(p_file_stat_base,p_file_stat_base_temp,&p_hot_cold_file_global->file_stat_tiny_small_delete_head,hot_cold_file_list){
		if(!file_stat_in_delete_base(p_file_stat_base) /*|| file_stat_in_delete_error(p_file_stat)*/)
			panic("%s file_stat:0x%llx not delete status:0x%x\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

		del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat_base,FILE_STAT_TINY_SMALL);
		del_file_stat_count ++;
		/*防止删除file_area耗时太长*/
		if(++ del_file_area_count_temp > 256)
			break;
	}
#endif	
	/*global delete链表上的files_stat的处理*/
    if(is_cache_file){
        deleted_file_stat_solve(p_hot_cold_file_global,&p_hot_cold_file_global->file_stat_delete_head,256,FILE_STAT_NORMAL,is_cache_file);
        deleted_file_stat_solve(p_hot_cold_file_global,&p_hot_cold_file_global->file_stat_small_delete_head,256,FILE_STAT_SMALL,is_cache_file);
        deleted_file_stat_solve(p_hot_cold_file_global,&p_hot_cold_file_global->file_stat_tiny_small_delete_head,256,FILE_STAT_TINY_SMALL,is_cache_file);
	}else{
        deleted_file_stat_solve(p_hot_cold_file_global,&p_hot_cold_file_global->mmap_file_stat_delete_head,256,FILE_STAT_NORMAL,is_cache_file);
        deleted_file_stat_solve(p_hot_cold_file_global,&p_hot_cold_file_global->mmap_file_stat_small_delete_head,256,FILE_STAT_SMALL,is_cache_file);
        deleted_file_stat_solve(p_hot_cold_file_global,&p_hot_cold_file_global->mmap_file_stat_tiny_small_delete_head,256,FILE_STAT_TINY_SMALL,is_cache_file);
	}


	if(0 == test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		return;

	/*global zero_file_area链表上的files_stat的处理*/
	if(is_cache_file){
		file_stat_has_zero_file_area_manage(p_hot_cold_file_global,&p_hot_cold_file_global->file_stat_zero_file_area_head,FILE_STAT_NORMAL,is_cache_file);
		file_stat_has_zero_file_area_manage(p_hot_cold_file_global,&p_hot_cold_file_global->file_stat_small_zero_file_area_head,FILE_STAT_SMALL,is_cache_file);
		file_stat_has_zero_file_area_manage(p_hot_cold_file_global,&p_hot_cold_file_global->file_stat_tiny_small_zero_file_area_head,FILE_STAT_TINY_SMALL,is_cache_file);
	}else{
		file_stat_has_zero_file_area_manage(p_hot_cold_file_global,&p_hot_cold_file_global->mmap_file_stat_zero_file_area_head,FILE_STAT_NORMAL,is_cache_file);
		file_stat_has_zero_file_area_manage(p_hot_cold_file_global,&p_hot_cold_file_global->mmap_file_stat_small_zero_file_area_head,FILE_STAT_SMALL,is_cache_file);
		file_stat_has_zero_file_area_manage(p_hot_cold_file_global,&p_hot_cold_file_global->mmap_file_stat_tiny_small_zero_file_area_head,FILE_STAT_TINY_SMALL,is_cache_file);
	}

	if(0 == test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		return;

	//打印所有file_stat的file_area个数和page个数
	if(shrink_page_printk_open1)
		hot_cold_file_print_all_file_stat(p_hot_cold_file_global,NULL,0,PRINT_FILE_STAT_INFO);
	//打印内存回收时统计的各个参数
	if(shrink_page_printk_open1)
		printk_shrink_param(p_hot_cold_file_global,NULL,0);

	//return 0;
}
static int inline memory_zone_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct zone *zone,unsigned long zone_free_page,int *zone_memory_tiny_count)
{
	int index;
	int memory_pressure_level = MEMORY_IDLE_SCAN;
	int free_pages_dx = (high_wmark_pages(zone) << 2) - zone_free_page;

	/*如果zone free内存低于zone水位阀值，进入紧急内存回收模式*/
	if(free_pages_dx > 0){
		//if(zone_free_page < low_wmark_pages(zone))
		if(zone_free_page < (high_wmark_pages(zone) << 1))
			memory_pressure_level = MEMORY_EMERGENCY_RECLAIM;
		else
			memory_pressure_level = MEMORY_PRESSURE_RECLAIM;

        *zone_memory_tiny_count = *zone_memory_tiny_count + 1;
		/*reclaim_pages_target累加本次预期内存要回收的目标page数*/
		p_hot_cold_file_global->reclaim_pages_target += free_pages_dx;

		printk("%s %s zone_free_page:%ld memory_pressure_level:%d zone_memory_tiny_count:%d\n",__func__,zone->name,zone_free_page,memory_pressure_level,*zone_memory_tiny_count);
	}else{/*如果内存碎片有点严重*/
		index = fragmentation_index(zone,PAGE_ALLOC_COSTLY_ORDER);
		if(index > sysctl_extfrag_threshold){
			memory_pressure_level = MEMORY_LITTLE_RECLAIM;
			printk("%s memory fragment %s index:%d\n",__func__,zone->name,index);
		}

        //*zone_memory_tiny_count = *zone_memory_tiny_count - 1;
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
	unsigned long zone_free_page,last_zone_pages = 0;
	int check_zone_free_many_pages = 0,i;
	int check_max_page_zone_in_pressure = 0;
	int check_max_page_zone_in_emergency = 0;
	int check_any_zone_little_reclaim = 0;
	//int normal_zone_pressure_level = 0;
	/*遇到内存紧张的zone加1，遇到内存充足的减1，最后如果大于0，则说明内存紧张的zone更多，进入紧急内存回收模式*/
	int zone_memory_tiny_count = 0;
	int check_any_zone_memory_pressure = 0;
	int check_any_zone_memory_emergency = 0;
	int max_page_zone_high_wmark_pages = 0;
	//int check_one_zone_memory_fragmentation = 0;

	/*每次检测内存紧张状态前都要对reclaim_pages_target清0，否则会导致reclaim_pages_target一直累加*/
	p_hot_cold_file_global->reclaim_pages_target = 0;

	//for (pgdat = first_online_pgdat();pgdat;pgdat = next_online_pgdat(pgdat)){
	for_each_online_pgdat(pgdat){
		/*发现有zone内存紧缺*/
		if(memory_pressure_level == MEMORY_EMERGENCY_RECLAIM)
			break;

		//for (zone = pgdat->node_zones; zone - pgdat->node_zones < MAX_NR_ZONES; ++zone) {
		for (i = 0; i < MAX_NR_ZONES - 1; i++) {
			zone = &pgdat->node_zones[i];
			/*空zone跳过*/
			if (!populated_zone(zone))
				continue;

			printk("%s free:%ld high:%ld\n",zone->name,zone_page_state(zone, NR_FREE_PAGES),high_wmark_pages(zone));
			if(0 == pgdat->node_id){
				if(0 == strncmp("Normal",zone->name,6)){
					zone_free_page = zone_page_state(zone, NR_FREE_PAGES);
					/*当前zone上个周期到现在free内存差值*/
					if(p_hot_cold_file_global->normal_zone_free_pages_last)
						free_page_dx = zone_free_page - p_hot_cold_file_global->normal_zone_free_pages_last;

					/*保存上一次的high阀值*/
					p_hot_cold_file_global->normal_zone_free_pages_last = zone_free_page;
					memory_pressure_level = memory_zone_solve(p_hot_cold_file_global,zone,zone_free_page,&zone_memory_tiny_count);
					/*进程优先从normal zone分配内存，如果normal zone内存紧张直接跳出循环*/
					/*if(memory_pressure_level == MEMORY_EMERGENCY_RECLAIM){
						//normal_zone_pressure_level = memory_pressure_level;
						break;
					}*/
				}
#if 1				
				/*DMA32 zone的内存太少了，发现会干扰内存紧张的判断，先去掉了。但是新的虚机大部分内存又集中在DMA32了*/
				else if(0 == strncmp("DMA32",zone->name,5)){
					zone_free_page = zone_page_state(zone, NR_FREE_PAGES);
					/*当前zone上个周期到现在free内存差值*/
					if(p_hot_cold_file_global->dma32_zone_free_pages_last)
						free_page_dx = zone_free_page - p_hot_cold_file_global->dma32_zone_free_pages_last;

					p_hot_cold_file_global->dma32_zone_free_pages_last = zone_free_page;
					memory_pressure_level = memory_zone_solve(p_hot_cold_file_global,zone,zone_free_page,&zone_memory_tiny_count);
					/*if(memory_pressure_level == MEMORY_LITTLE_RECLAIM)
						break;*/
				}
				else if(0 == strncmp("DMA",zone->name,3)){
					zone_free_page = zone_page_state(zone, NR_FREE_PAGES);
					/*当前zone上个周期到现在free内存差值*/
					if(p_hot_cold_file_global->dma_zone_free_pages_last)
						free_page_dx = zone_free_page - p_hot_cold_file_global->dma_zone_free_pages_last;

					p_hot_cold_file_global->dma_zone_free_pages_last = zone_free_page;
					memory_pressure_level = memory_zone_solve(p_hot_cold_file_global,zone,zone_free_page,&zone_memory_tiny_count);
					if(memory_pressure_level == MEMORY_EMERGENCY_RECLAIM)
						break;
				}
#endif
				else if(0 == strncmp("HighMem",zone->name,7)){
					zone_free_page = zone_page_state(zone, NR_FREE_PAGES);
					/*当前zone上个周期到现在free内存差值*/
					if(p_hot_cold_file_global->highmem_zone_free_pages_last)
						free_page_dx = zone_free_page - p_hot_cold_file_global->highmem_zone_free_pages_last;

					p_hot_cold_file_global->highmem_zone_free_pages_last = zone_free_page;
					memory_pressure_level = memory_zone_solve(p_hot_cold_file_global,zone,zone_free_page,&zone_memory_tiny_count);
					/*if(memory_pressure_level == MEMORY_EMERGENCY_RECLAIM)
						break;*/
				}
			}else if(1 == pgdat->node_id){
				if(0 == strncmp("Normal",zone->name,6)){
					zone_free_page = zone_page_state(zone, NR_FREE_PAGES);
					/*当前zone上个周期到现在free内存差值*/
					if(p_hot_cold_file_global->normal1_zone_free_pages_last)
						free_page_dx = zone_free_page - p_hot_cold_file_global->normal1_zone_free_pages_last;

					p_hot_cold_file_global->normal1_zone_free_pages_last = zone_free_page;
					memory_pressure_level = memory_zone_solve(p_hot_cold_file_global,zone,zone_free_page,&zone_memory_tiny_count);
					/*if(memory_pressure_level == MEMORY_EMERGENCY_RECLAIM)
						break;*/
				}
			}

			/*检测到有一个内存zone大量分配内存*/
			if((free_page_dx > 512) && (0 == check_zone_free_many_pages)){
		        printk("%s free_page_dx:%ld free too much page\n",__func__,free_page_dx);
				check_zone_free_many_pages = 1;
			}

			/*检测到有一个内存zone，有内存碎片 或 内存紧张 或者 内存很紧张*/
			if((0 == check_any_zone_little_reclaim) && memory_pressure_level > MEMORY_IDLE_SCAN){
				check_any_zone_little_reclaim = 1;
			}

			if((0 == check_any_zone_memory_pressure)  && (MEMORY_PRESSURE_RECLAIM == memory_pressure_level)){
				check_any_zone_memory_pressure  = 1;
			}

			if((0 == check_any_zone_memory_emergency)  && (MEMORY_EMERGENCY_RECLAIM == memory_pressure_level)){
				check_any_zone_memory_emergency = 1;
			}

			/* 检测到内存最多的zone内存紧张、内存非常紧缺 则对check_max_page_zone_in_pressure或check_max_page_zone_in_emergency置1。
			 * 如果内存最大的zone内存并不紧张，必须对二者清0，否则会受之前遍历的内存zone的紧缺状态影响*/
			if(zone_managed_pages(zone) > last_zone_pages){
				/*保存上一个遍历到的内存最多的内存zone的总page个数*/
				last_zone_pages = zone_managed_pages(zone);
				max_page_zone_high_wmark_pages = high_wmark_pages(zone);

				if(MEMORY_PRESSURE_RECLAIM == memory_pressure_level)
					check_max_page_zone_in_pressure = 1;
			    else if(MEMORY_EMERGENCY_RECLAIM == memory_pressure_level)
					check_max_page_zone_in_emergency = 1;
			    else{
                    check_max_page_zone_in_pressure = 0;
					check_max_page_zone_in_emergency = 0;
				}
			}
		}
	}

#if 0
	/* 没有发现内存碎片严重，也没有发现内存zone内存紧缺。但是如果前后两个周期某个zone发现有大量内存分配。
	 * 也令memory_pressure_level置1，进行少量内存回收*/
	if(check_zone_free_many_pages && (MEMORY_IDLE_SCAN == memory_pressure_level)){
		memory_pressure_level = MEMORY_LITTLE_RECLAIM;
	}

	/* 遇到内存紧张的zone加1，遇到内存充足的减1，最后如果小于等于0，则说明至少有一个内存zone内存还充足，
	 * 此时不会紧急内存回收模式.是否要去掉，因为即便只有一个zone内存紧张，程序分配内存也会受阻呀???但是，
	 * 如果在一个zone内存分配遇阻，还会去另一个内存zone分配内存*/
	if((zone_memory_tiny_count <= 0)&& (memory_pressure_level >= MEMORY_PRESSURE_RECLAIM)){
		memory_pressure_level = MEMORY_LITTLE_RECLAIM;
		printk("%s zone_memory_pressure_level:%d memory_tiny_or_enough_count:%d memory not tiny\n",__func__,memory_pressure_level,zone_memory_tiny_count);
	}
#else

	/*有任何一个zone内存很紧张直接return*/
	/*if(MEMORY_EMERGENCY_RECLAIM == memory_pressure_level)
		return memory_pressure_level;*/

	/* 优先以内存最多的zone的状态为准，如果内存紧张则memory_pressure_level赋值MEMORY_EMERGENCY_RECLAIM或MEMORY_PRESSURE_RECLAIM。
	 * 如果内存最多的zone内存重组，再考虑其他内存zone的紧缺状态，或者内存碎片状态。之所以这样设计，是因为实际测试发现，存在内存
	 * 最多的zone，内存紧张；但是其他内存少的zone内存，内存充足。但是此时free总内存很少，已经影响到了内存分配*/
	if(check_max_page_zone_in_emergency || (check_any_zone_memory_emergency && zone_memory_tiny_count > 1))
		memory_pressure_level = MEMORY_EMERGENCY_RECLAIM;
	else if(check_max_page_zone_in_pressure || check_any_zone_memory_emergency /*|| check_any_zone_memory_pressure*/)
		memory_pressure_level = MEMORY_PRESSURE_RECLAIM;
	else if(check_any_zone_little_reclaim || check_zone_free_many_pages || check_any_zone_memory_pressure){
		/*MEMORY_LITTLE_RECLAIM模式，没有对reclaim_pages_target赋值，这里把page最多的zone的内存水位值赋给reclaim_pages_target*/
		p_hot_cold_file_global->reclaim_pages_target = max_page_zone_high_wmark_pages;
		memory_pressure_level = MEMORY_LITTLE_RECLAIM;
	}
	else
		memory_pressure_level = MEMORY_IDLE_SCAN;
#endif
	return memory_pressure_level;
}
#define IDLE_MAX 3
int hot_cold_file_thread(void *p){
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

		if(test_bit(MEMORY_IN_RECLAIM, &async_memory_reclaim_status)){
            continue;
		}

		/*检测内存紧张状态，并在内存紧张时计算要回收的page数到reclaim_pages_target*/
		memory_pressure_level = check_memory_reclaim_necessary(p_hot_cold_file_global);
		/*不用内存回收*/
		if(MEMORY_IDLE_SCAN == memory_pressure_level){
			if(++idle_age_count < IDLE_MAX)
				continue;
		}
		idle_age_count = 0;

		if(test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status)){
			/*内存回收前记录memory_pressure_level*/	
			p_hot_cold_file_global->memory_pressure_level = memory_pressure_level;
			/*唤醒异步内存回收线程*/
			wake_up_process(p_hot_cold_file_global->async_memory_reclaim);
		}
	}
	return 0;
}
int async_memory_reclaim_main_thread(void *p){
	struct hot_cold_file_global *p_hot_cold_file_global = (struct hot_cold_file_global *)p;
    //int memory_pressure_emergecy = 0;
	int memory_pressure_level = 0;
	int repeat_reclaim = 0;
	int check_memory_enough_count = 0;

	while(!kthread_should_stop()){
		/*清空上一轮内存回收统计参数*/
		//memset(&p_hot_cold_file_global->hot_cold_file_shrink_counter,0,sizeof(struct hot_cold_file_shrink_counter));
		//memset(&p_hot_cold_file_global->mmap_file_shrink_counter,0,sizeof(struct mmap_file_shrink_counter));

		set_current_state(TASK_INTERRUPTIBLE);
		clear_bit(MEMORY_IN_RECLAIM, &async_memory_reclaim_status);
		schedule();
		set_current_state(TASK_RUNNING);
		set_bit(MEMORY_IN_RECLAIM, &async_memory_reclaim_status);
		repeat_reclaim = 0;
		p_hot_cold_file_global->memory_still_memrgency_after_reclaim = 0;

repeat_reclaim:
		//memory_pressure_emergecy = IS_IN_MEMORY_EMERGENCY_RECLAIM(p_hot_cold_file_global); 


		/*每次内存回收都要对alreay_reclaim_pages清0*/
		p_hot_cold_file_global->alreay_reclaim_pages = 0;
		/*每一轮回收的总page数，每轮内存回收前都要清0*/
		p_hot_cold_file_global->all_reclaim_pages_one_period = 0;

		/*回收cache文件页*/
		walk_throuth_all_file_area(p_hot_cold_file_global,1);
		/*回收mmap文件页*/
		//walk_throuth_all_mmap_file_area(p_hot_cold_file_global);
		walk_throuth_all_file_area(p_hot_cold_file_global,0);
#if 0
		msleep(1000);
		/*内存回收前内存紧张，内存回收后内存依然紧张，不休眠，直接进行下次内存回收*/
		if(memory_pressure_emergecy && (MEMORY_EMERGENCY_RECLAIM == check_memory_reclaim_necessary(p_hot_cold_file_global))){
			printk("memory still tiny,repeat_reclaim!!!!!!!!\n");
			/*只重复一次*/
			memory_pressure_emergecy = 0;
			/*内存回收前，必须对p_hot_cold_file_global->memory_pressure_level赋值*/
			p_hot_cold_file_global->memory_pressure_level = MEMORY_EMERGENCY_RECLAIM;
			goto repeat_reclaim;
		}
#endif
		/* 内存回收后依然内存紧张，继续进行内存回收。否则，等待几分钟，如果内存依然不紧张再休眠，主要是应对进程突然
		 * 大量分配内存，触发kswapd内存回收，产生大量refault，此时异步内存线程还在休眠，它的休眠周期最短是10s，无法
		 * 应对突然有业务大量分配内存，内存紧张的场景*/
		memory_pressure_level = check_memory_reclaim_necessary(p_hot_cold_file_global);
		/*repeat_reclaim阈值调大到100了，现在调整memory_pressure_level达到MEMORY_EMERGENCY_RECLAIM再repeat_reclaim加1，然后继续内存回收*/
		if(memory_pressure_level > MEMORY_PRESSURE_RECLAIM){
			/*暂时不允许无限制重复内存回收*/
			if(++ repeat_reclaim < 100){
				/*内存回收后依然内存紧张，把reclaim_pages_target调大8倍，大幅增大回收的page数*/
				if(MEMORY_EMERGENCY_RECLAIM == memory_pressure_level){
					p_hot_cold_file_global->reclaim_pages_target = p_hot_cold_file_global->reclaim_pages_target << 3;
					p_hot_cold_file_global->memory_still_memrgency_after_reclaim = repeat_reclaim;

					printk("%s memory still tiny,reclaim more pages repeat_reclaim:%d\n",__func__,repeat_reclaim);
				}else{
					p_hot_cold_file_global->reclaim_pages_target = p_hot_cold_file_global->reclaim_pages_target << 1;
					p_hot_cold_file_global->memory_still_memrgency_after_reclaim = 0;
				}

				/*内存回收前必须赋值p_hot_cold_file_global->memory_pressure_level*/	
				p_hot_cold_file_global->memory_pressure_level = memory_pressure_level;
				goto repeat_reclaim;
			}
		}else{
			repeat_reclaim = 0;
			p_hot_cold_file_global->memory_still_memrgency_after_reclaim = 0;
			check_memory_enough_count = 0;
			while(memory_pressure_level < MEMORY_EMERGENCY_RECLAIM && check_memory_enough_count < 30){
				memory_pressure_level = check_memory_reclaim_necessary(p_hot_cold_file_global);
				msleep(2000);
				check_memory_enough_count ++;
			}

			if(memory_pressure_level > MEMORY_PRESSURE_RECLAIM){
				printk("%s sleep sometime,find memory tiny,continue reclaim\n",__func__);
				/*每次内存回收前必须赋值p_hot_cold_file_global->memory_pressure_level*/	
				p_hot_cold_file_global->memory_pressure_level = memory_pressure_level;
				goto repeat_reclaim;
			}
		}
	}
	return 0;
}
/*在change_global_age_dx()基础上，针对mmap文件增大age_dx，以使mmap的文件页page更不容易回收*/
static void change_global_age_dx_for_mmap_file(struct hot_cold_file_global *p_hot_cold_file_global)
{
	p_hot_cold_file_global->file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx << 2;
	p_hot_cold_file_global->file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx << 1;
	p_hot_cold_file_global->file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_refault_to_temp_age_dx << 1;
	//p_hot_cold_file_global->file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_warm_age_dx;
	//p_hot_cold_file_global->file_area_warm_to_temp_age_dx = p_hot_cold_file_global->file_area_warm_to_temp_age_dx;
	p_hot_cold_file_global->file_area_reclaim_read_age_dx = p_hot_cold_file_global->file_area_reclaim_read_age_dx << 1;
	p_hot_cold_file_global->file_area_reclaim_ahead_age_dx = p_hot_cold_file_global->file_area_reclaim_ahead_age_dx << 1;

	//p_hot_cold_file_global->file_area_free_age_dx = p_hot_cold_file_global->file_area_free_age_dx << 1;
}
/*根据当前的内存状态调整各个内存回收参数*/
static void change_global_age_dx(struct hot_cold_file_global *p_hot_cold_file_global)
{
	/*每次内存回收前先恢复writeonly_file_age_dx，因为之前的内存回收可能把writeonly_file_age_dx减小到0，甚至负数*/
	p_hot_cold_file_global->writeonly_file_age_dx = p_hot_cold_file_global->writeonly_file_age_dx_ori;

	switch(p_hot_cold_file_global->memory_pressure_level){
		/*内存非常紧缺*/
		case MEMORY_EMERGENCY_RECLAIM:
			p_hot_cold_file_global->file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx_ori >> 2;
			p_hot_cold_file_global->file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx_ori >> 2;
			p_hot_cold_file_global->file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_refault_to_temp_age_dx_ori >> 2;
			p_hot_cold_file_global->file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx_ori << 1;
			p_hot_cold_file_global->file_area_warm_to_temp_age_dx = p_hot_cold_file_global->file_area_warm_to_temp_age_dx_ori << 2;
			p_hot_cold_file_global->file_area_reclaim_read_age_dx = p_hot_cold_file_global->file_area_reclaim_read_age_dx_ori;
			p_hot_cold_file_global->file_area_reclaim_ahead_age_dx = p_hot_cold_file_global->file_area_reclaim_ahead_age_dx_ori >> 2;

			//p_hot_cold_file_global->file_area_free_age_dx = p_hot_cold_file_global->file_area_free_age_dx_ori >> 2;
			break;
			/*内存紧缺*/
		case MEMORY_PRESSURE_RECLAIM:
			p_hot_cold_file_global->file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx_ori >> 1;
			p_hot_cold_file_global->file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx_ori >> 1;
			p_hot_cold_file_global->file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_refault_to_temp_age_dx_ori >> 1;
			p_hot_cold_file_global->file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx_ori << 1;
			p_hot_cold_file_global->file_area_warm_to_temp_age_dx = p_hot_cold_file_global->file_area_warm_to_temp_age_dx_ori << 1;
			p_hot_cold_file_global->file_area_reclaim_read_age_dx = p_hot_cold_file_global->file_area_reclaim_read_age_dx_ori >> 1;
			p_hot_cold_file_global->file_area_reclaim_ahead_age_dx = p_hot_cold_file_global->file_area_reclaim_ahead_age_dx_ori >> 1;

			//p_hot_cold_file_global->file_area_free_age_dx = p_hot_cold_file_global->file_area_free_age_dx_ori >> 1;
			break;
			/*内存碎片有点多，或者前后两个周期分配的内存数太多*/		
		case MEMORY_LITTLE_RECLAIM:
			/*一般情况*/
		default:
			p_hot_cold_file_global->file_area_temp_to_cold_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx_ori;
			p_hot_cold_file_global->file_area_hot_to_temp_age_dx = p_hot_cold_file_global->file_area_hot_to_temp_age_dx_ori;
			p_hot_cold_file_global->file_area_refault_to_temp_age_dx = p_hot_cold_file_global->file_area_refault_to_temp_age_dx_ori;
			p_hot_cold_file_global->file_area_temp_to_warm_age_dx = p_hot_cold_file_global->file_area_temp_to_cold_age_dx_ori;
			p_hot_cold_file_global->file_area_warm_to_temp_age_dx = p_hot_cold_file_global->file_area_warm_to_temp_age_dx_ori;
			p_hot_cold_file_global->file_area_reclaim_read_age_dx = p_hot_cold_file_global->file_area_reclaim_read_age_dx_ori;
			p_hot_cold_file_global->file_area_reclaim_ahead_age_dx = p_hot_cold_file_global->file_area_reclaim_ahead_age_dx_ori;

			//p_hot_cold_file_global->file_area_free_age_dx = p_hot_cold_file_global->file_area_free_age_dx_ori;

			break;
	}
}
