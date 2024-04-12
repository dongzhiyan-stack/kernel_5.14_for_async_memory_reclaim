
#ifndef _ASYNC_MEMORY_RECLAIM_BASH_H_
#define _ASYNC_MEMORY_RECLAIM_BASH_H_
#include <linux/mm.h>
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

//使能kprobe打点文件页page读写必然执行的copy_page_to_iter、copy_page_from_iter_atomic等函数
#define CONFIG_ENABLE_KPROBE

//一个file_stat结构里缓存的热file_area结构个数
#define FILE_AREA_CACHE_COUNT 3
//置1才允许异步内存回收
#define ASYNC_MEMORY_RECLAIM_ENABLE 0
//置1说明说明触发了drop_cache，此时禁止异步内存回收线程处理gloabl drop_cache_file_stat_head链表上的file_stat
#define ASYNC_DROP_CACHES 1
//异步内存回收周期，单位s
#define ASYNC_MEMORY_RECLIAIM_PERIOD 60
//最大文件名字长度
#define MAX_FILE_NAME_LEN 100
//当一个文件file_stat长时间不被访问，释放掉了所有的file_area，再过FILE_STAT_DELETE_AGE_DX个周期，则释放掉file_stat结构
#define FILE_STAT_DELETE_AGE_DX  10
//一个 file_area 包含的page数，默认4个
#define PAGE_COUNT_IN_AREA_SHIFT 2
#define PAGE_COUNT_IN_AREA (1UL << PAGE_COUNT_IN_AREA_SHIFT)//4
#define PAGE_COUNT_IN_AREA_MASK (PAGE_COUNT_IN_AREA - 1)//0x3

#define TREE_MAP_SHIFT	6
#define TREE_MAP_SIZE	(1UL << TREE_MAP_SHIFT)
#define TREE_MAP_MASK (TREE_MAP_SIZE - 1)
#define TREE_ENTRY_MASK 3
#define TREE_INTERNAL_NODE 1

/*热file_area经过FILE_AREA_HOT_to_TEMP_AGE_DX个周期后，还没有被访问，则移动到file_area_temp链表*/
#define FILE_AREA_HOT_to_TEMP_AGE_DX  5
/*发生refault的file_area经过FILE_AREA_REFAULT_TO_TEMP_AGE_DX个周期后，还没有被访问，则移动到file_area_temp链表*/
#define FILE_AREA_REFAULT_TO_TEMP_AGE_DX 20
/*普通的file_area在FILE_AREA_TEMP_TO_COLD_AGE_DX个周期内没有被访问则被判定是冷file_area，然后释放这个file_area的page*/
#define FILE_AREA_TEMP_TO_COLD_AGE_DX  5
/*一个冷file_area，如果经过FILE_AREA_FREE_AGE_DX个周期，仍然没有被访问，则释放掉file_area结构*/
#define FILE_AREA_FREE_AGE_DX  10
/*当一个file_area在一个周期内访问超过FILE_AREA_HOT_LEVEL次数，则判定是热的file_area*/
#define FILE_AREA_HOT_LEVEL (PAGE_COUNT_IN_AREA << 2)
/*如果一个file_area在FILE_AREA_MOVE_HEAD_DX个周期内被访问了两次，然后才能移动到链表头*/
#define FILE_AREA_MOVE_HEAD_DX 3
/*在file_stat被判定为热文件后，记录当时的global_age。在未来HOT_FILE_COLD_AGE_DX时间内该文件进去冷却期：hot_file_update_file_status()函数中
  *只更新该文件file_area的age后，然后函数返回，不再做其他操作，节省性能*/
#define HOT_FILE_COLD_AGE_DX 10

/**针对mmap文件新加的******************************/
#define MMAP_FILE_NAME_LEN 16
struct mmap_file_shrink_counter
{
	//扫描的file_area个数
	unsigned int scan_file_area_count;
	//扫描的file_stat个数
	unsigned int scan_file_stat_count;
	//扫描到的处于delete状态的file_stat个数
	unsigned int scan_delete_file_stat_count;
	//扫描的冷file_stat个数
	unsigned int scan_cold_file_area_count;
	//扫描到的大文件转小文件的个数
	unsigned int scan_large_to_small_count;

	//释放的page个数
	unsigned int free_pages;
	//隔离的page个数
	unsigned int isolate_lru_pages;
	//file_stat的refault链表转移到temp链表的file_area个数
	unsigned int file_area_refault_to_temp_list_count;
	//释放的file_area结构个数
	unsigned int file_area_free_count;

	//释放的file_stat个数
	unsigned int del_file_stat_count;
	//释放的file_area个数
	unsigned int del_file_area_count;
	//mmap的文件，但是没有mmap映射的文件页个数
	unsigned int in_cache_file_page_count;
};
struct hot_cold_file_shrink_counter
{
	/**get_file_area_from_file_stat_list()函数******/
	//扫描的file_area个数
	unsigned int scan_file_area_count;
	//扫描的file_stat个数
	unsigned int scan_file_stat_count;
	//扫描到的处于delete状态的file_stat个数
	unsigned int scan_delete_file_stat_count;
	//扫描的冷file_stat个数
	unsigned int scan_cold_file_area_count;
	//扫描到的大文件转小文件的个数
	unsigned int scan_large_to_small_count;
	//本次扫描到但没有冷file_area的file_stat个数
	unsigned int scan_fail_file_stat_count;

	/**free_page_from_file_area()函数******/
	//释放的page个数
	unsigned int free_pages;
	//隔离的page个数
	unsigned int isolate_lru_pages;
	//file_stat的refault链表转移到temp链表的file_area个数
	unsigned int file_area_refault_to_temp_list_count;
	//释放的file_area结构个数
	unsigned int file_area_free_count;
	//file_stat的hot链表转移到temp链表的file_area个数
	unsigned int file_area_hot_to_temp_list_count;

	/**free_page_from_file_area()函数******/
	//file_stat的hot链表转移到temp链表的file_area个数
	unsigned int file_area_hot_to_temp_list_count2;
	//释放的file_stat个数
	unsigned int del_file_stat_count;
	//释放的file_area个数
	unsigned int del_file_area_count;

	/**async_shrink_free_page()函数******/
	unsigned int lock_fail_count;
	unsigned int writeback_count;
	unsigned int dirty_count;
	unsigned int page_has_private_count;
	unsigned int mapping_count;
	unsigned int free_pages_count;
	unsigned int free_pages_fail_count;
	unsigned int page_unevictable_count; 
	unsigned int nr_unmap_fail;

	/**file_stat_has_zero_file_area_manage()函数****/
	unsigned int scan_zero_file_area_file_stat_count;

	//进程抢占lru_lock锁的次数
	unsigned int lru_lock_contended_count;
	//释放的file_area但是处于hot_file_area_cache数组的file_area个数
	unsigned int file_area_delete_in_cache_count;
	//从hot_file_area_cache命中file_area次数
	unsigned int file_area_cache_hit_count;

	//file_area内存回收期间file_area被访问的次数
	unsigned int file_area_access_count_in_free_page;
	//在内存回收期间产生的热file_area个数
	unsigned int hot_file_area_count_in_free_page;

	//一个周期内产生的热file_area个数
	unsigned int hot_file_area_count_one_period;
	//一个周期内产生的refault file_area个数
	unsigned int refault_file_area_count_one_period;
	//每个周期执行hot_file_update_file_status函数访问所有文件的所有file_area总次数
	unsigned int all_file_area_access_count;
	//每个周期直接从file_area_tree找到file_area并且不用加锁次数加1
	unsigned int find_file_area_from_tree_not_lock_count;

	//每个周期内因文件页page数太少被拒绝统计的次数
	unsigned int small_file_page_refuse_count;
	//每个周期从file_stat->file_area_last得到file_area的次数
	unsigned int find_file_area_from_last_count;

	//每个周期频繁冗余lru_lock的次数
	//unsigned int lru_lock_count;
	//释放的mmap page个数
	unsigned int mmap_free_pages_count;
	unsigned int mmap_writeback_count;
	unsigned int mmap_dirty_count;
};
//一个file_area表示了一片page范围(默认6个page)的冷热情况，比如page索引是0~5、6~11、12~17各用一个file_area来表示
struct file_area
{
	//不同取值表示file_area当前处于哪种链表
	unsigned int file_area_state;
	//该file_area最近被访问时的global_age，长时间不被访问则与global age差很多，则判定file_area是冷file_area，然后释放该file_area的page
	//如果是mmap文件页，当遍历到文件页的pte置位，才会更新对应的file_area的age为全局age，否则不更新
	unsigned int file_area_age;
	union{
		/*cache文件时，该file_area当前周期被访问的次数。mmap文件时，只有处于file_stat->temp链表上file_area才用access_count记录访问计数，
		 *处于其他file_stat->refault、hot、free等链表上file_area，不会用到access_count。但是因为跟file_area_access_age是共享枚举变量，
		 *要注意，从file_stat->refault、hot、free等链表移动file_area到file_stat->temp链表时，要对file_area_access_age清0*/
		//unsigned int access_count;
		atomic_t   access_count;
		/*处于file_stat->refault、hot、free等链表上file_area，被遍历到时记录当时的global age，不理会文件页page是否被访问了。
		 *由于和access_count是共享枚举变量，当file_area从file_stat->temp链表移动到file_stat->refault、hot、free等链表时，要对file_area_access_age清0*/
		unsigned int file_area_access_age;
	};
	//该file_area里的某个page最近一次被回收的时间点，单位秒
	unsigned int shrink_time;
	//file_area通过file_area_list添加file_stat的各种链表
	struct list_head file_area_list;
	//该file_area代表的N个连续page的起始page索引
	pgoff_t start_index;
    struct folio *pages[PAGE_COUNT_IN_AREA];
};
struct hot_cold_file_area_tree_node
{
	//与该节点树下最多能保存多少个page指针有关
	unsigned char   shift;
	//在节点在父节点中的偏移
	unsigned char   offset;
	//指向父节点
	struct hot_cold_file_area_tree_node *parent;
	//该节点下有多少个成员
	unsigned int    count;
	//是叶子节点时保存file_area结构，是索引节点时保存子节点指针
	void    *slots[TREE_MAP_SIZE];
};
struct hot_cold_file_area_tree_root
{
	unsigned int  height;//树高度
	struct hot_cold_file_area_tree_node __rcu *root_node;
};
//热点文件统计信息，一个文件一个
struct file_stat
{
	struct address_space *mapping;
	//file_stat通过hot_cold_file_list添加到hot_cold_file_global的file_stat_hot_head链表
	struct list_head hot_cold_file_list;
	//file_stat状态
	unsigned long file_stat_status;
	//总file_area个数
	unsigned int file_area_count;
	//热file_area个数
	unsigned int file_area_hot_count;
	//文件的file_area结构按照索引保存到这个radix tree
	struct hot_cold_file_area_tree_root hot_cold_file_area_tree_root_node;
	//file_stat锁
	spinlock_t file_stat_lock;
	//file_stat里age最大的file_area的age，调试用
	unsigned long max_file_area_age;
	/*最近一次访问的page的file_area所在的父节点，通过它直接得到file_area，然后得到page，不用每次都遍历xarray tree*/
	struct xa_node *xa_node_cache;
	/*xa_node_cache父节点保存的起始file_area的page的索引*/
	pgoff_t  xa_node_cache_base_index;
	/*在file_stat被判定为热文件后，记录当时的global_age。在未来HOT_FILE_COLD_AGE_DX时间内该文件进去冷却期：hot_file_update_file_status()函数中
	 *只更新该文件file_area的age后，然后函数返回，不再做其他操作，节省性能*/
	unsigned int file_stat_hot_base_age;


	union{
		//cache文件file_stat最近一次被异步内存回收访问时的age，调试用
		unsigned long recent_access_age;
		//mmap文件在扫描完一轮file_stat->temp链表上的file_area，进入冷却期，cooling_off_start_age记录当时的global age
		unsigned long cooling_off_start_age;
	};
	//频繁被访问的文件page对应的file_area存入这个头结点
	struct list_head file_area_hot;
	//不冷不热处于中间状态的file_area结构添加到这个链表，新分配的file_area就添加到这里
	struct list_head file_area_temp;
	//每轮扫描被释放内存page的file_area结构临时先添加到这个链表。file_area_free_temp有存在的必要
	struct list_head file_area_free_temp;
	//所有被释放内存page的file_area结构最后添加到这个链表，如果长时间还没被访问，就释放file_area结构。
	struct list_head file_area_free;
	//file_area的page被释放后，但很快又被访问，发生了refault，于是要把这种page添加到file_area_refault链表，短时间内不再考虑扫描和释放
	struct list_head file_area_refault;
#if 0
	//把最近访问的file_stat保存到hot_file_area_cache缓存数组，
	struct file_area * hot_file_area_cache[FILE_AREA_CACHE_COUNT];
	//最近一次访问的热file_area以hot_file_area_cache_index为下标保存到hot_file_area_cache数组
	unsigned char hot_file_area_cache_index;
#endif
	/*file_area_tree_node保存最近一次访问file_area的父节点，cache_file_area_tree_node_base_index是它保存的最小file_area索引。
	 *之后通过cache_file_area_tree_node->slots[]直接获取在同一个node的file_area，不用每次都遍历radix tree获取file_area*/
	unsigned int cache_file_area_tree_node_base_index;
	struct hot_cold_file_area_tree_node *cache_file_area_tree_node;

	//最新一次访问的file_area
	struct file_area *file_area_last;

	/**针对mmap文件新增的****************************/
	//根据文件mmap映射的虚拟地址，计算出文件mmap映射最大文件页索引
	unsigned int max_index;
	/*记录最近一次radix tree遍历该文件文件页索引，比如第一次遍历索引是0~11这12文件页，则last_index =11.如果last_index是0，
	 *说明该文件是第一次被遍历文件页，或者，该文件的所有文件页都被遍历过了，然后要从头开始遍历*/
	pgoff_t last_index;
	/*如果遍历完一次文件的所有page，traverse_done置1。后期每个周期加1，当traverse_done大于阀值，每个周期再尝试遍历该文件
	 *的少量page，因为这段时间文件缺页异常会分配新的文件页page。并且冷file_area的page被全部回收后，file_area会被从
	 *file_stat->mmap_file_stat_temp_head剔除并释放掉，后续就无法再从file_stat->mmap_file_stat_temp_head链表遍历到这个
	 *file_area。这种情况下，已经从radix tree遍历完一次文件的page且traverse_done是1，但是不得不每隔一段时间再遍历一次
	 *该文件的radix tree的空洞page*/
	unsigned char traverse_done;//现在使用了
	//file_area_refault链表最新一次访问的file_area
	struct file_area *file_area_refault_last;
	//file_area_free_temp链表最新一次访问的file_area
	struct file_area *file_area_free_last;
	char file_name[MMAP_FILE_NAME_LEN];
	//件file_stat->file_area_temp链表上已经扫描的file_stat个数，如果达到file_area_count_in_temp_list，说明这个文件的file_stat扫描完了，才会扫描下个文件file_stat的file_area
	unsigned int scan_file_area_count_temp_list;
	//在文件file_stat->file_area_temp链表上的file_area个数
	unsigned int file_area_count_in_temp_list;
	//file_area对应的page的pagecount大于0的，则把file_area移动到该链表
	struct list_head file_area_mapcount;
	//文件 mapcount大于1的file_area的个数
	unsigned int mapcount_file_area_count;
	//当扫描完一轮文件file_stat的temp链表上的file_area时，置1，进入冷却期，在N个age周期内不再扫描这个文件上的file_area。
	bool cooling_off_start;
};
/*hot_cold_file_node_pgdat结构体每个内存节点分配一个，内存回收前，从lruvec lru链表隔离成功page，移动到每个内存节点绑定的
 * hot_cold_file_node_pgdat结构的pgdat_page_list链表上.然后参与内存回收。内存回收后把pgdat_page_list链表上内存回收失败的
 * page在putback移动回lruvec lru链表。这样做的目的是减少内存回收失败的page在putback移动回lruvec lru链表时，可以减少
 * lruvec->lru_lock或pgdat->lru_lock加锁，详细分析见cold_file_isolate_lru_pages()函数。但实际测试时，内存回收失败的page是很少的，
 * 这个做法的意义又不太大!其实完全可以把参与内存回收的page移动到一个固定的链表也可以！*/
struct hot_cold_file_node_pgdat
{
	pg_data_t *pgdat;
	struct list_head pgdat_page_list;
	struct list_head pgdat_page_list_mmap_file;
};
//热点文件统计信息全局结构体
struct hot_cold_file_global
{
	/*被判定是热文本的file_stat添加到file_stat_hot_head链表,超过50%或者80%的file_area都是热的，则该文件就是热文件，
	 * 文件的file_stat要移动到global的file_stat_hot_head链表*/
	struct list_head file_stat_hot_head;
	//新分配的文件file_stat默认添加到file_stat_temp_head链表
	struct list_head file_stat_temp_head;
	/*如果文件file_stat上的page cache数太多，被判定为大文件，则把file_stat移动到这个链表。将来内存回收时，优先遍历这种file_stat，
	 *因为file_area足够多，能遍历到更多的冷file_area，回收到内存page*/
	struct list_head file_stat_temp_large_file_head;
	struct list_head cold_file_head;
	//inode被删除的文件的file_stat移动到这个链表
	struct list_head file_stat_delete_head;
	//0个file_area的file_stat移动到这个链表
	struct list_head file_stat_zero_file_area_head;
	//触发drop_cache后的没有file_stat的文件，分配file_stat后保存在这个链表
	struct list_head drop_cache_file_stat_head;

	//触发drop_cache后的没有file_stat的文件个数
	unsigned int drop_cache_file_count;
	//热文件file_stat个数
	unsigned int file_stat_hot_count;
	//大文件file_stat个数
	unsigned int file_stat_large_count;
	//文件file_stat个数
	unsigned int file_stat_count;
	//0个file_area的file_stat个数
	unsigned int file_stat_count_zero_file_area;
	
	/*当file_stat的file_area个数达到file_area_level_for_large_file时，表示该文件的page cache数太多，被判定为大文件。但一个file_area
	 *包含了多个page，一个file_area并不能填满page，因此实际file_stat的file_area个数达到file_area_level_for_large_file时，实际该文件的的page cache数会少点*/
	unsigned int file_area_level_for_large_file;
	//当一个文件的文件页page数大于nr_pages_level时，该文件的文件页page才会被本异步内存回收模块统计访问频率并回收，默认15，即64k，可通过proc接口调节大小
	unsigned int nr_pages_level;

	struct kmem_cache *file_stat_cachep;
	struct kmem_cache *file_area_cachep;
	//保存文件file_stat所有file_area的radix tree
	struct kmem_cache *hot_cold_file_area_tree_node_cachep;
	struct hot_cold_file_node_pgdat *p_hot_cold_file_node_pgdat;
	//异步内存回收线程
	struct task_struct *hot_cold_file_thead;
	int node_count;

	//有多少个进程在执行hot_file_update_file_status函数使用文件file_stat、file_area
	atomic_t   ref_count;
	//有多少个进程在执行__destroy_inode_handler_post函数，正在删除文件inode
	atomic_t   inode_del_count;
	//内存回收各个参数统计
	struct hot_cold_file_shrink_counter hot_cold_file_shrink_counter;
	//proc文件系统根节点
	struct proc_dir_entry *hot_cold_file_proc_root;

	spinlock_t global_lock;
	//全局age，每个周期加1
	unsigned long global_age;
	//异步内存回收周期，单位s
	unsigned int global_age_period;
	//热file_area经过file_area_refault_to_temp_age_dx个周期后，还没有被访问，则移动到file_area_temp链表
	unsigned int file_area_hot_to_temp_age_dx;
	//发生refault的file_area经过file_area_refault_to_temp_age_dx个周期后，还没有被访问，则移动到file_area_temp链表
	unsigned int file_area_refault_to_temp_age_dx;
	//普通的file_area在file_area_temp_to_cold_age_dx个周期内没有被访问则被判定是冷file_area，然后释放这个file_area的page
	unsigned int file_area_temp_to_cold_age_dx;
	//一个冷file_area，如果经过file_area_free_age_dx_fops个周期，仍然没有被访问，则释放掉file_area结构
	unsigned int file_area_free_age_dx;
	//当一个文件file_stat长时间不被访问，释放掉了所有的file_area，再过file_stat_delete_age_dx个周期，则释放掉file_stat结构
	unsigned int file_stat_delete_age_dx;

	//发生refault的次数,累加值
	unsigned long all_refault_count;
	//在内存回收期间产生的refault file_area个数
	unsigned int refault_file_area_count_in_free_page;

	/**针对mmap文件新增的****************************/
	//新分配的文件file_stat默认添加到file_stat_temp_head链表
	struct list_head mmap_file_stat_uninit_head;
	//当一个文件的page都遍历完后，file_stat移动到这个链表
	struct list_head mmap_file_stat_temp_head;
	//文件file_stat个数超过阀值移动到这个链表
	struct list_head mmap_file_stat_temp_large_file_head;
	//热文件移动到这个链表
	struct list_head mmap_file_stat_hot_head;
	//一个文件有太多的page的mmapcount都大于1，则把该文件file_stat移动该链表
	struct list_head mmap_file_stat_mapcount_head;
	//0个file_area的file_stat移动到这个链表，暂时没用到
	struct list_head mmap_file_stat_zero_file_area_head;
	//inode被删除的文件的file_stat移动到这个链表，暂时不需要
	struct list_head mmap_file_stat_delete_head;
	//每个周期频繁冗余lru_lock的次数
	unsigned int lru_lock_count;
	unsigned int mmap_file_lru_lock_count;


	//mmap文件用的全局锁
	spinlock_t mmap_file_global_lock;

	struct file_stat *file_stat_last;
	//mmap文件个数
	unsigned int mmap_file_stat_count;
	//mapcount文件个数
	unsigned int mapcount_mmap_file_stat_count;
	//热文件个数
	unsigned int hot_mmap_file_stat_count;
	struct mmap_file_shrink_counter mmap_file_shrink_counter;
	/*当file_stat的file_area个数达到file_area_level_for_large_mmap_file时，表示该文件的page cache数太多，被判定为大文件*/
	unsigned int mmap_file_area_level_for_large_file;
};


/*******file_area状态**********************************************************/
enum file_area_status{//file_area_state是char类型，只有8个bit位可设置
	F_file_area_in_temp_list,
	F_file_area_in_hot_list,
	//F_file_area_in_free_temp_list,
	F_file_area_in_free_list,
	F_file_area_in_refault_list,
	F_file_area_in_mapcount_list,//file_area对应的page的pagecount大于0的，则把file_area移动到该链表
	F_file_area_in_cache,//file_area保存在ile_stat->hot_file_area_cache[]数组里
};
//不能使用 clear_bit_unlock、test_and_set_bit_lock、test_bit，因为要求p_file_area->file_area_state是64位数据，但实际只是u8型数据

#define MAX_FILE_AREA_LIST_BIT F_file_area_in_refault_list
#define FILE_AREA_LIST_MASK ((1 << (MAX_FILE_AREA_LIST_BIT + 1)) - 1)
//清理file_area的状态，在哪个链表
#define CLEAR_FILE_AREA_LIST_STATUS(list_name) \
	static inline void clear_file_area_in_##list_name(struct file_area *p_file_area)\
    { p_file_area->file_area_state &= ~(1 << F_file_area_in_##list_name);}
//设置file_area在哪个链表的状态
#define SET_FILE_AREA_LIST_STATUS(list_name) \
	static inline void set_file_area_in_##list_name(struct file_area *p_file_area)\
    { p_file_area->file_area_state |= (1 << F_file_area_in_##list_name);}
//测试file_area在哪个链表
#define TEST_FILE_AREA_LIST_STATUS(list_name) \
	static inline int file_area_in_##list_name(struct file_area *p_file_area)\
    {return p_file_area->file_area_state & (1 << F_file_area_in_##list_name);}

#define TEST_FILE_AREA_LIST_STATUS_ERROR(list_name) \
	static inline int file_area_in_##list_name##_error(struct file_area *p_file_area)\
    {return p_file_area->file_area_state & (~(1 << F_file_area_in_##list_name) & FILE_AREA_LIST_MASK);}

#define FILE_AREA_LIST_STATUS(list_name)     \
	CLEAR_FILE_AREA_LIST_STATUS(list_name) \
	SET_FILE_AREA_LIST_STATUS(list_name)  \
	TEST_FILE_AREA_LIST_STATUS(list_name) \
	TEST_FILE_AREA_LIST_STATUS_ERROR(list_name)

FILE_AREA_LIST_STATUS(temp_list)
FILE_AREA_LIST_STATUS(hot_list)
//FILE_AREA_LIST_STATUS(free_temp_list)
FILE_AREA_LIST_STATUS(free_list)
FILE_AREA_LIST_STATUS(refault_list)
FILE_AREA_LIST_STATUS(mapcount_list)

//清理file_area的状态，在哪个链表
#define CLEAR_FILE_AREA_STATUS(status) \
	static inline void clear_file_area_in_##status(struct file_area *p_file_area)\
    { p_file_area->file_area_state &= ~(1 << F_file_area_in_##status);}
//设置file_area在哪个链表的状态
#define SET_FILE_AREA_STATUS(status) \
	static inline void set_file_area_in_##status(struct file_area *p_file_area)\
    { p_file_area->file_area_state |= (1 << F_file_area_in_##status);}
//测试file_area在哪个链表
#define TEST_FILE_AREA_STATUS(status) \
	static inline int file_area_in_##status(struct file_area *p_file_area)\
    {return p_file_area->file_area_state & (1 << F_file_area_in_##status);}

#define FILE_AREA_STATUS(status)     \
	CLEAR_FILE_AREA_STATUS(status) \
	SET_FILE_AREA_STATUS(status)  \
	TEST_FILE_AREA_STATUS(status) 

FILE_AREA_STATUS(cache)


/*******file_stat状态**********************************************************/
enum file_stat_status{//file_area_state是long类型，只有64个bit位可设置
	F_file_stat_in_file_stat_hot_head_list,
	F_file_stat_in_file_stat_temp_head_list,
	F_file_stat_in_zero_file_area_list,
	F_file_stat_in_mapcount_file_area_list,//文件file_stat是mapcount文件
	F_file_stat_in_drop_cache,
	F_file_stat_in_free_page,//正在遍历file_stat的file_area的page，尝试释放page
	F_file_stat_in_free_page_done,//正在遍历file_stat的file_area的page，完成了page的内存回收,
	F_file_stat_in_delete,
    F_file_stat_in_cache_file,//cache文件，sysctl读写产生pagecache。有些cache文件可能还会被mmap映射，要与mmap文件互斥
	F_file_stat_in_mmap_file,//mmap文件，有些mmap文件可能也会被sysctl读写产生pagecache，要与cache文件互斥
	F_file_stat_in_large_file,
	F_file_stat_lock,
	F_file_stat_lock_not_block,//这个bit位置1，说明inode在删除的，但是获取file_stat锁失败
};
//不能使用 clear_bit_unlock、test_and_set_bit_lock、test_bit，因为要求p_file_stat->file_stat_status是64位数据，但这里只是u8型数据

#define MAX_FILE_STAT_LIST_BIT F_file_stat_in_free_page_done
#define FILE_STAT_LIST_MASK ((1 << (MAX_FILE_STAT_LIST_BIT + 1)) - 1)

//清理file_stat的状态，在哪个链表
#define CLEAR_FILE_STAT_STATUS(name)\
	static inline void clear_file_stat_in_##name##_list(struct file_stat *p_file_stat)\
    {p_file_stat->file_stat_status &= ~(1 << F_file_stat_in_##name##_list);}
//设置file_stat在哪个链表的状态
#define SET_FILE_STAT_STATUS(name)\
	static inline void set_file_stat_in_##name##_list(struct file_stat *p_file_stat)\
    {p_file_stat->file_stat_status |= (1 << F_file_stat_in_##name##_list);}
//测试file_stat在哪个链表
#define TEST_FILE_STAT_STATUS(name)\
	static inline int file_stat_in_##name##_list(struct file_stat *p_file_stat)\
    {return (p_file_stat->file_stat_status & (1 << F_file_stat_in_##name##_list));}
#define TEST_FILE_STAT_STATUS_ERROR(name)\
	static inline int file_stat_in_##name##_list##_error(struct file_stat *p_file_stat)\
    {return p_file_stat->file_stat_status & (~(1 << F_file_stat_in_##name##_list) & FILE_STAT_LIST_MASK);}

#define FILE_STAT_STATUS(name) \
	CLEAR_FILE_STAT_STATUS(name) \
	SET_FILE_STAT_STATUS(name) \
	TEST_FILE_STAT_STATUS(name) \
	TEST_FILE_STAT_STATUS_ERROR(name)

FILE_STAT_STATUS(file_stat_hot_head)
FILE_STAT_STATUS(file_stat_temp_head)
FILE_STAT_STATUS(zero_file_area)
FILE_STAT_STATUS(mapcount_file_area)

//清理文件的状态，大小文件等
#define CLEAR_FILE_STATUS(name)\
    static inline void clear_file_stat_in_##name(struct file_stat *p_file_stat)\
    {p_file_stat->file_stat_status &= ~(1 << F_file_stat_in_##name);}
//设置文件的状态，大小文件等
#define SET_FILE_STATUS(name)\
    static inline void set_file_stat_in_##name(struct file_stat *p_file_stat)\
    {p_file_stat->file_stat_status |= (1 << F_file_stat_in_##name);}
//测试文件的状态，大小文件等
#define TEST_FILE_STATUS(name)\
    static inline int file_stat_in_##name(struct file_stat *p_file_stat)\
    {return (p_file_stat->file_stat_status & (1 << F_file_stat_in_##name));}
#define TEST_FILE_STATUS_ERROR(name)\
    static inline int file_stat_in_##name##_error(struct file_stat *p_file_stat)\
    {return p_file_stat->file_stat_status & (~(1 << F_file_stat_in_##name) & FILE_STAT_LIST_MASK);}

#define FILE_STATUS(name) \
	CLEAR_FILE_STATUS(name) \
	SET_FILE_STATUS(name) \
	TEST_FILE_STATUS(name)\
	TEST_FILE_STATUS_ERROR(name)

FILE_STATUS(large_file)
//FILE_STATUS(delete)
FILE_STATUS(drop_cache)

//清理文件的状态，大小文件等
#define CLEAR_FILE_STATUS_ATOMIC(name)\
    static inline void clear_file_stat_in_##name(struct file_stat *p_file_stat)\
    {clear_bit_unlock(F_file_stat_in_##name,&p_file_stat->file_stat_status);}
//设置文件的状态，大小文件等
#define SET_FILE_STATUS_ATOMIC(name)\
    static inline void set_file_stat_in_##name(struct file_stat *p_file_stat)\
    {if(test_and_set_bit_lock(F_file_stat_in_##name,&p_file_stat->file_stat_status)) \
		/*如果这个file_stat的bit位被多进程并发设置，不可能,应该发生了某种异常，触发crash*/  \
	    panic("file_stat:0x%llx status:0x%lx alreay set %d bit\n",(u64)p_file_stat,p_file_stat->file_stat_status,F_file_stat_in_##name); \
	}
//测试文件的状态，大小文件等
#define TEST_FILE_STATUS_ATOMIC(name)\
    static inline int file_stat_in_##name(struct file_stat *p_file_stat)\
    {return test_bit(F_file_stat_in_##name,&p_file_stat->file_stat_status);}
#define TEST_FILE_STATUS_ATOMIC_ERROR(name)\
    static inline int file_stat_in_##name##_error(struct file_stat *p_file_stat)\
    {return p_file_stat->file_stat_status & (~(1 << F_file_stat_in_##name) & FILE_STAT_LIST_MASK);}

#define FILE_STATUS_ATOMIC(name) \
	CLEAR_FILE_STATUS_ATOMIC(name) \
	SET_FILE_STATUS_ATOMIC(name) \
	TEST_FILE_STATUS_ATOMIC(name) \
	TEST_FILE_STATUS_ATOMIC_ERROR(name) \
/* 为什么 file_stat的in_free_page、free_page_done的状态要使用test_and_set_bit_lock/clear_bit_unlock，主要是get_file_area_from_file_stat_list()函数开始内存回收，
 * 要把file_stat设置成in_free_page状态，此时hot_file_update_file_status()里就不能再把这些file_stat的file_area跨链表移动。而把file_stat设置成
 * in_free_page状态，只是加了global global_lock锁，没有加file_stat->file_stat_lock锁。没有加锁file_stat->file_stat_lock锁，就无法避免
 * hot_file_update_file_status()把把这些file_stat的file_area跨链表移动。因此，file_stat的in_free_page、free_page_done的状态设置要考虑原子操作吧，
 * 并且此时要避免此时有进程在执行hot_file_update_file_status()函数。这些在hot_file_update_file_status()和get_file_area_from_file_stat_list()函数
 * 有说明其实file_stat设置in_free_page、free_page_done 状态都有spin lock加锁，不使用test_and_set_bit_lock、clear_bit_unlock也行，
 * 目前暂定先用test_and_set_bit_lock、clear_bit_unlock吧，后续再考虑其他优化*/
FILE_STATUS_ATOMIC(free_page)
FILE_STATUS_ATOMIC(free_page_done)
FILE_STATUS_ATOMIC(delete)
FILE_STATUS_ATOMIC(cache_file)
FILE_STATUS_ATOMIC(mmap_file)

extern struct hot_cold_file_global hot_cold_file_global_info;
//置1会把内存回收信息详细打印出来
extern int shrink_page_printk_open1;
//不怎么关键的调试信息
extern int shrink_page_printk_open;
extern unsigned long async_memory_reclaim_status;

static inline void lock_file_stat(struct file_stat * p_file_stat,int not_block){
	//如果有其他进程对file_stat的lock加锁，while成立，则休眠等待这个进程释放掉lock，然后自己加锁
	while(test_and_set_bit_lock(F_file_stat_lock, &p_file_stat->file_stat_status)){
		if(not_block){//if成立说明inode在删除的，但是获取file_stat锁失败，此时正获取file_stat锁的进程要立即释放掉file_stat锁
		    if(test_and_set_bit_lock(F_file_stat_lock_not_block,&p_file_stat->file_stat_status)){
				//F_file_stat_lock_not_block这个bit位可能被多进程并发设置，如果已经被设置了，先不考虑触发crash
		        //panic("file_stat:0x%llx status:0x%x alreay set stat_lock_not_block\n",(u64)p_file_stat,p_file_stat->file_stat_status);
			}
			not_block = 0;
		}
		/*其实好点是每个file_stat都有休眠等待队列，进程获取file_stat失败则再休眠等待队列休眠，而不是直接msleep，后期改进吧?????*/
		msleep(1);
		//dump_stack();
	}
}
static inline void unlock_file_stat(struct file_stat * p_file_stat){
	//如果file_stat被设置了not_block标记，则要先清理掉
	test_and_clear_bit(F_file_stat_lock_not_block,&p_file_stat->file_stat_status);
	clear_bit_unlock(F_file_stat_lock, &p_file_stat->file_stat_status);
}
/*根据p_file_area对应的起始文件页page索引从文件mapping radix tree一次性得到PAGE_COUNT_IN_AREA个page，这个遍历一次radix tree
 *就能得到PAGE_COUNT_IN_AREA个page*/
static inline int get_page_from_file_area(struct file_stat *p_file_stat,pgoff_t file_area_start_page_index,struct page **pages)
{
	struct address_space *mapping = p_file_stat->mapping;
	int i,ret;
	ret = find_get_pages_contig(mapping,file_area_start_page_index,PAGE_COUNT_IN_AREA,pages);
	for(i = 0;i < ret;i++){
		put_page(pages[i]);//上边会令page引用计数加1，这里只能再减1，先强制减1了，后期需要优化find_get_pages_contig()函数
	}
	return ret;
}
static inline struct file_stat *file_stat_alloc_and_init(struct address_space *mapping)
{
	struct file_stat * p_file_stat;

    /*这里有个问题，hot_cold_file_global_info.global_lock有个全局大锁，每个进程执行到这里就会获取到。合理的是
	应该用每个文件自己的spin lock锁!比如file_stat里的spin lock锁，但是在这里，每个文件的file_stat结构还没分配!!!!!!!!!!!!*/
	spin_lock(&hot_cold_file_global_info.global_lock);
	//如果两个进程同时访问一个文件，同时执行到这里，需要加锁。第1个进程加锁成功后，分配file_stat并赋值给
	//mapping->rh_reserved1，第2个进程获取锁后执行到这里mapping->rh_reserved1就会成立
	if(mapping->rh_reserved1){
		printk("%s file_stat:0x%llx already alloc\n",__func__,(u64)mapping->rh_reserved1);
		p_file_stat = (struct file_stat *)mapping->rh_reserved1;
		goto out;
	}
	//新的文件分配file_stat,一个文件一个，保存文件热点区域访问数据
	p_file_stat = kmem_cache_alloc(hot_cold_file_global_info.file_stat_cachep,GFP_ATOMIC);
	if (!p_file_stat) {
		printk("%s file_stat alloc fail\n",__func__);
		goto out;
	}
	//file_stat个数加1
	hot_cold_file_global_info.file_stat_count ++;
	memset(p_file_stat,0,sizeof(struct file_stat));
	//设置文件是cache文件状态，有些cache文件可能还会被mmap映射，要与mmap文件互斥，要么是cache文件要么是mmap文件，不能两者都是 
	set_file_stat_in_cache_file(p_file_stat);
	//初始化file_area_hot头结点
	INIT_LIST_HEAD(&p_file_stat->file_area_hot);
	INIT_LIST_HEAD(&p_file_stat->file_area_temp);
	//INIT_LIST_HEAD(&p_file_stat->file_area_cold);
	INIT_LIST_HEAD(&p_file_stat->file_area_free_temp);
	INIT_LIST_HEAD(&p_file_stat->file_area_free);
	INIT_LIST_HEAD(&p_file_stat->file_area_refault);
	INIT_LIST_HEAD(&p_file_stat->file_area_mapcount);

	//mapping->file_stat记录该文件绑定的file_stat结构，将来判定是否对该文件分配了file_stat
	mapping->rh_reserved1 = (unsigned long)p_file_stat;
	//file_stat记录mapping结构
	p_file_stat->mapping = mapping;
	//设置file_stat in_temp_list最好放到把file_stat添加到global temp链表操作前，原因在add_mmap_file_stat_to_list()有分析
	set_file_stat_in_file_stat_temp_head_list(p_file_stat);
	smp_wmb();
	//把针对该文件分配的file_stat结构添加到hot_cold_file_global_info的file_stat_temp_head链表
	list_add(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.file_stat_temp_head);
	//新分配的file_stat必须设置in_file_stat_temp_head_list链表
	//set_file_stat_in_file_stat_temp_head_list(p_file_stat);
	spin_lock_init(&p_file_stat->file_stat_lock);
out:	
	spin_unlock(&hot_cold_file_global_info.global_lock);

    return p_file_stat;
}
static inline struct file_area *file_area_alloc_and_init(unsigned int area_index_for_page,struct file_stat * p_file_stat)
{
	struct file_area *p_file_area = NULL;

        spin_lock(&p_file_stat->file_stat_lock);
	/*到这里，针对当前page索引的file_area结构还没有分配,page_slot_in_tree是槽位地址，*page_slot_in_tree是槽位里的数据，就是file_area指针，
	  但是NULL，于是针对本次page索引，分配file_area结构*/
	p_file_area = kmem_cache_alloc(hot_cold_file_global_info.file_area_cachep,GFP_ATOMIC);
	if (!p_file_area) {
		//spin_unlock(&p_file_stat->file_stat_lock);
		printk("%s file_area alloc fail\n",__func__);
		goto out;
	}
	memset(p_file_area,0,sizeof(struct file_area));
	//把新分配的file_area添加到file_area_temp链表
	list_add(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
	//保存该file_area对应的起始page索引，一个file_area默认包含8个索引挨着依次增大page，start_index保存其中第一个page的索引
	p_file_area->start_index = area_index_for_page << PAGE_COUNT_IN_AREA_SHIFT;//area_index_for_page * PAGE_COUNT_IN_AREA;
	p_file_stat->file_area_count ++;//文件file_stat的file_area个数加1
	set_file_area_in_temp_list(p_file_area);//新分配的file_area必须设置in_temp_list链表

out:
	spin_unlock(&p_file_stat->file_stat_lock);

        return p_file_area;
}


extern int hot_file_update_file_status(struct address_space *mapping,struct file_stat *p_file_stat,struct file_area *p_file_area,int access_count);

extern void printk_shrink_param(struct hot_cold_file_global *p_hot_cold_file_global,struct seq_file *m,int is_proc_print);
extern int hot_cold_file_print_all_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct seq_file *m,int is_proc_print);//is_proc_print:1 通过proc触发的打印
extern void get_file_name(char *file_name_path,struct file_stat * p_file_stat);
extern void file_stat_free_leak_page(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat);
extern int drop_cache_truncate_inode_pages(struct hot_cold_file_global *p_hot_cold_file_global);
extern int hot_cold_file_proc_init(struct hot_cold_file_global *p_hot_cold_file_global);
extern int hot_cold_file_proc_exit(struct hot_cold_file_global *p_hot_cold_file_global);
extern unsigned long cold_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct list_head *file_area_free);
extern unsigned int cold_mmap_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct file_area *p_file_area,struct page *page_buf[],int cold_page_count);

//extern unsigned int async_shrink_free_page(struct pglist_data *pgdat,struct lruvec *lruvec,struct list_head *page_list,struct scan_control *sc,struct reclaim_stat *stat);
#endif
