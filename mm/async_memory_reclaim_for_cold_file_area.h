#ifndef _ASYNC_MEMORY_RECLAIM_BASH_H_
#define _ASYNC_MEMORY_RECLAIM_BASH_H_
#include <linux/mm.h>

//#define ASYNC_MEMORY_RECLAIM_IN_KERNEL ------在pagemap.h定义过了
//#define ASYNC_MEMORY_RECLAIM_DEBUG
#define ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY

#define CACHE_FILE_DELETE_PROTECT_BIT 0
#define MMAP_FILE_DELETE_PROTECT_BIT 1

#define FILE_AREA_IN_HOT_LIST 0
#define FILE_AREA_IN_REFAULT_LIST 1
#define FILE_AREA_IN_FREE_LIST 2

/*极小的文件，pagecache小于1M的文件*/
#define FILE_STAT_TINY_SMALL 0
/*小文件，pagecache在1M~10M的文件*/
#define FILE_STAT_SMALL 1
//普通文件，包含temp、middle、large 3类文件。pagecache在10M~30M是temp文件、30M~100M是middle文件，100M以上的是large文件 
#define FILE_STAT_NORMAL 2

/*file_area个数小于64是极小文件，在64~640是小文件*/
#define SMALL_FILE_AREA_COUNT_LEVEL 64
/*file_area个数在大于640，且小于1920是普通文件*/
#define NORMAL_TEMP_FILE_AREA_COUNT_LEVEL 640
/*file_area个数在大于1920，且小于6400是普通文件*/
#define NORMAL_MIDDLE_FILE_AREA_COUNT_LEVEL 1920
/*file_area个数在大于6400是大型文件*/
#define NORMAL_LARGE_FILE_AREA_COUNT_LEVEL  6400

#define FILE_STAT_IN_TEMP_FILE_LIST 0 /*file_stat在普通文件链表*/
#define FILE_STAT_IN_MIDDLE_FILE_LIST 1 /*file_stat在普通文件链表*/
#define FILE_STAT_IN_LARGE_FILE_LIST 2 /*file_stat在普通大文件链表*/

#define TEMP_FILE (FILE_STAT_NORMAL + 1) /*file_stat的file_area个数是普通文件*/
#define MIDDLE_FILE (FILE_STAT_NORMAL + 2) /*file_stat的file_area个数是中型文件*/
#define LARGE_FILE (FILE_STAT_NORMAL + 3) /*file_stat的file_area个数是大文件*/

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

/*热file_area经过FILE_AREA_HOT_to_TEMP_AGE_DX个周期后，还没有被访问，则移动到file_area_warm链表*/
#define FILE_AREA_HOT_to_TEMP_AGE_DX  5
/*发生refault的file_area经过FILE_AREA_REFAULT_TO_TEMP_AGE_DX个周期后，还没有被访问，则移动到file_area_warm链表*/
#define FILE_AREA_REFAULT_TO_TEMP_AGE_DX 20
/*普通的file_area在FILE_AREA_TEMP_TO_COLD_AGE_DX个周期内没有被访问则被判定是冷file_area，然后释放这个file_area的page*/
#define FILE_AREA_TEMP_TO_COLD_AGE_DX  10
/*在file_stat->warm上的file_area经过file_area_warm_to_temp_age_dx个周期没有被访问，则移动到file_stat->temp链表*/
#define FILE_AREA_WARM_TO_TEMP_AGE_DX  (FILE_AREA_TEMP_TO_COLD_AGE_DX + 10) 
/*一个冷file_area，如果经过FILE_AREA_FREE_AGE_DX个周期，仍然没有被访问，则释放掉file_area结构*/
#define FILE_AREA_FREE_AGE_DX  10
/*当一个file_area因多次访问被设置了ahead标记，经过FILE_AREA_AHEAD_CANCEL_AGE_DX个周期后file_area没有被访问，才会允许清理file_area的ahead标记*/
#define FILE_AREA_AHEAD_CANCEL_AGE_DX (FILE_AREA_TEMP_TO_COLD_AGE_DX + 10)

/*当一个file_area在一个周期内访问超过FILE_AREA_HOT_LEVEL次数，则判定是热的file_area*/
#define FILE_AREA_HOT_LEVEL (PAGE_COUNT_IN_AREA << 2)
/*如果一个file_area在FILE_AREA_MOVE_HEAD_DX个周期内被访问了两次，然后才能移动到链表头*/
#define FILE_AREA_MOVE_HEAD_DX 3
/*在file_stat被判定为热文件后，记录当时的global_age。在未来HOT_FILE_COLD_AGE_DX时间内该文件进去冷却期：hot_file_update_file_status()函数中
 *只更新该文件file_area的age后，然后函数返回，不再做其他操作，节省性能*/
#define HOT_FILE_COLD_AGE_DX 10


//一个冷file_area，如果经过FILE_AREA_TO_FREE_AGE_DX个周期，仍然没有被访问，则释放掉file_area结构
#define MMAP_FILE_AREA_TO_FREE_AGE_DX  (MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX + 20)
//发生refault的file_area经过FILE_AREA_REFAULT_TO_TEMP_AGE_DX个周期后，还没有被访问，则移动到file_area_temp链表
#define MMAP_FILE_AREA_REFAULT_TO_TEMP_AGE_DX 30
//普通的file_area在FILE_AREA_TEMP_TO_COLD_AGE_DX个周期内没有被访问则被判定是冷file_area，然后释放这个file_area的page
#define MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX  10//这个参数调的很小容易在file_area被内存回收后立即释放，这样测试了很多bug，先不要改

//file_area如果在 MMAP_FILE_AREA_HOT_AGE_DX 周期内被检测到访问 MMAP_FILE_AREA_ACCESS_HOT_COUNT 次，file_area被判定为热file_area
#define MMAP_FILE_AREA_ACCESS_HOT_COUNT 2
//hot链表上的file_area在MMAP_FILE_AREA_HOT_TO_TEMP_AGE_DX个周期内没有被访问，则降级到temp链表
#define MMAP_FILE_AREA_HOT_TO_TEMP_AGE_DX 10

//mapcount的file_area在MMAP_FILE_AREA_MAPCOUNT_AGE_DX个周期内不再遍历访问，降低性能损耗
#define MMAP_FILE_AREA_MAPCOUNT_AGE_DX 5
//hot链表上的file_area在MMAP_FILE_AREA_HOT_AGE_DX个周期内不再遍历访问，降低性能损耗
#define MMAP_FILE_AREA_HOT_AGE_DX 20
//free链表上的file_area在MMAP_FILE_AREA_HOT_AGE_DX个周期内不再遍历访问，降低性能损耗
#define MMAP_FILE_AREA_FREE_AGE_DX 5
//refault链表上的file_area在MMAP_FILE_AREA_HOT_AGE_DX个周期内不再遍历访问，降低性能损耗
#define MMAP_FILE_AREA_REFAULT_AGE_DX 5

#define SUPPORT_FS_UUID_LEN UUID_SIZE
#define SUPPORT_FS_NAME_LEN 10
#define SUPPORT_FS_COUNT 2

#define SUPPORT_FS_ALL  0
#define SUPPORT_FS_UUID 1
#define SUPPORT_FS_SINGLE     2
#define FILE_AREA_IS_READ 1

#define FILE_AREA_PAGE_IS_READ 0
#define FILE_AREA_PAGE_IS_WRITE 1
#define FILE_AREA_PAGE_IS_READ_WRITE 2


/*cache文件file_stat的file_area包含mmap的文件页。必须是负数，怕跟其他file_stat类型的宏定义有充足*/
#define FILE_STAT_FROM_CACHE_FILE  -101
/*file_area来自file_stat->free、refault、hot链表，遍历时不能移动该file_area到其他file_stat链表，并且file_area不参与内存回收*/
#define FILE_STAT_OTHER_FILE_AREA (-102)


#define MEMORY_IDLE_SCAN  0 /*内存正常，常规的巡检*/
#define MEMORY_LITTLE_RECLAIM  1/*发现内存碎片，或者前后两个周期有大量内存分配*/
#define MEMORY_PRESSURE_RECLAIM  2/*zone free内存小于high阀值，有内存紧缺迹象*/
#define MEMORY_EMERGENCY_RECLAIM  3/*内存非常紧缺*/

#define IS_IN_MEMORY_IDLE_SCAN(p_hot_cold_file_global) (MEMORY_IDLE_SCAN == p_hot_cold_file_global->memory_pressure_level)
#define IS_IN_MEMORY_LITTLE_RECLAIM(p_hot_cold_file_global) (MEMORY_LITTLE_RECLAIM == p_hot_cold_file_global->memory_pressure_level)
#define IS_IN_MEMORY_PRESSURE_RECLAIM(p_hot_cold_file_global) (MEMORY_PRESSURE_RECLAIM == p_hot_cold_file_global->memory_pressure_level)
#define IS_IN_MEMORY_EMERGENCY_RECLAIM(p_hot_cold_file_global) (MEMORY_EMERGENCY_RECLAIM == p_hot_cold_file_global->memory_pressure_level)
#define IS_MEMORY_ENOUGH(p_hot_cold_file_global) (p_hot_cold_file_global->memory_pressure_level < MEMORY_PRESSURE_RECLAIM)

/**针对mmap文件新加的******************************/
#define MMAP_FILE_NAME_LEN 16
struct mmap_file_shrink_counter
{
	//check_one_file_area_cold_page_and_clear
	unsigned int scan_mapcount_file_area_count;
	unsigned int scan_hot_file_area_count;
	unsigned int find_cache_page_count_from_mmap_file;

	//cache_file_area_mmap_page_solve
	unsigned int scan_file_area_count_from_cache_file;
	unsigned int scan_cold_file_area_count_from_cache_file;
	unsigned int free_pages_from_cache_file;

	//reverse_other_file_area_list
	unsigned int mapcount_to_warm_file_area_count;
	unsigned int hot_to_warm_file_area_count;
	unsigned int refault_to_warm_file_area_count;
	unsigned int check_refault_file_area_count;
	unsigned int free_file_area_count;

	//mmap_file_stat_warm_list_file_area_solve
	unsigned int isolate_lru_pages_from_warm;
	unsigned int scan_cold_file_area_count_from_warm;
	unsigned int warm_to_temp_file_area_count;

	//check_file_area_cold_page_and_clear
	unsigned int isolate_lru_pages_from_temp;
	unsigned int scan_cold_file_area_count_from_temp;
	unsigned int temp_to_warm_file_area_count;
	unsigned int temp_to_temp_head_file_area_count;
	unsigned int scan_file_area_count_file_move_from_cache;

	//get_file_area_from_mmap_file_stat_list
	unsigned int scan_file_area_count;
	unsigned int scan_file_stat_count;

	//scan_mmap_mapcount_file_stat
	unsigned int mapcount_to_temp_file_area_count_from_mapcount_file;

	//scan_mmap_hot_file_stat
	unsigned int hot_to_temp_file_area_count_from_hot_file;

	//walk_throuth_all_mmap_file_area
	unsigned int del_file_area_count;
	unsigned int del_file_stat_count;

	//shrink_inactive_list_async
	unsigned int mmap_free_pages_count;
	unsigned int writeback_count;
	unsigned int dirty_count;
#if 0	
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

	unsigned int scan_file_area_count_from_cache_file;	
#endif	
};
struct hot_cold_file_shrink_counter
{
	//cold_file_isolate_lru_pages_and_shrink
	unsigned int find_mmap_page_count_from_cache_file;

	//file_stat_has_zero_file_area_manage
	unsigned int del_zero_file_area_file_stat_count;
	unsigned int scan_zero_file_area_file_stat_count;

	//file_stat_other_list_file_area_solve
	unsigned int file_area_refault_to_warm_list_count;
	unsigned int file_area_hot_to_warm_list_count;
	unsigned int file_area_free_count_from_free_list;

	//file_stat_temp_list_file_area_solve
	unsigned int scan_cold_file_area_count_from_temp;
	unsigned int scan_read_file_area_count_from_temp;
	unsigned int temp_to_hot_file_area_count;
	unsigned int scan_ahead_file_area_count_from_temp;
	unsigned int temp_to_warm_file_area_count;


	//file_stat_warm_list_file_area_solve
	unsigned int scan_cold_file_area_count_from_warm;
	unsigned int scan_read_file_area_count_from_warm;            
	unsigned int scan_ahead_file_area_count_from_warm;
	unsigned int scan_file_area_count_from_warm;
	unsigned int warm_to_temp_file_area_count;
	unsigned int warm_to_hot_file_area_count;

	//mmap_file_area_cache_page_solve
	unsigned int scan_cold_file_area_count_from_mmap_file;
	unsigned int isolate_lru_pages_from_mmap_file;
	unsigned int free_pages_from_mmap_file;

	//hot_file_stat_solve
	unsigned int file_area_hot_to_warm_from_hot_file;

	//free_page_from_file_area
	unsigned int isolate_lru_pages;

	//get_file_area_from_file_stat_list
	unsigned int scan_file_area_count;
	unsigned int scan_file_stat_count;
	unsigned int scan_delete_file_stat_count;

	//walk_throuth_all_file_area
	unsigned int del_file_area_count;
	unsigned int del_file_stat_count;
	
	//shrink_inactive_list_async
	unsigned int free_pages_count;
	unsigned int writeback_count;
	unsigned int dirty_count;

	unsigned int lru_lock_contended_count;
#if 0
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

	//隔离的page个数
	unsigned int isolate_lru_pages;

	//释放的file_stat个数
	unsigned int del_file_stat_count;
	//释放的file_area个数
	unsigned int del_file_area_count;

	unsigned int lock_fail_count;
	unsigned int writeback_count;
	unsigned int dirty_count;
	unsigned int page_has_private_count;
	unsigned int mapping_count;
	unsigned int free_pages_count;
	unsigned int free_pages_fail_count;
	unsigned int page_unevictable_count; 
	unsigned int nr_unmap_fail;

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


	unsigned int find_mmap_page_count_from_cache_file;

	/**file_stat_has_zero_file_area_manage()函数****/
	unsigned int scan_zero_file_area_file_stat_count;

	unsigned int file_area_refault_to_warm_list_count;
	unsigned int file_area_hot_to_warm_list_count;
	//释放的file_area结构个数
	unsigned int file_area_free_count_from_free_list;

	unsigned int file_area_hot_to_warm_from_hot_file;

	unsigned int scan_cold_file_area_count_from_temp;
	unsigned int scan_read_file_area_count_from_temp;
	unsigned int scan_ahead_file_area_count_from_temp;
	unsigned int temp_to_hot_file_area_count;
	unsigned int temp_to_warm_file_area_count;

	unsigned int mmap_scan_cold_file_area_count_from_warm;
	unsigned int scan_cold_file_area_count_from_mmap_file;
	unsigned int isolate_lru_pages_from_mmap_file;
	unsigned int scan_ahead_file_area_count_from_warm;
	unsigned int scan_file_area_count_from_warm;
	unsigned int warm_to_temp_file_area_count;
	unsigned int warm_to_hot_file_area_count;
	unsigned int scan_cold_file_area_count_from_warm;
#endif	
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
	//unsigned int shrink_time;
	union{
		//file_area通过file_area_list添加file_stat的各种链表
		struct list_head file_area_list;
		//rcu_head和list_head都是16个字节
		struct rcu_head		i_rcu;
	};
#ifndef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY
	//该file_area代表的N个连续page的起始page索引
	pgoff_t start_index;
#endif	
	struct folio __rcu *pages[PAGE_COUNT_IN_AREA];
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

struct file_stat_base
{
	/************base***base***base************/
	struct address_space *mapping;
	union{
		//file_stat通过hot_cold_file_list添加到hot_cold_file_global的file_stat_hot_head链表
		struct list_head hot_cold_file_list;
		//rcu_head和list_head都是16个字节
		struct rcu_head		i_rcu;
	};
	//file_stat状态
	//unsigned long file_stat_status;--------------调整成int型，节省内存空间
	unsigned int file_stat_status;
	//总file_area个数
	unsigned int file_area_count;
	//热file_area个数
	//unsigned int file_area_hot_count;------------------------------------------
	//文件的file_area结构按照索引保存到这个radix tree
	//struct hot_cold_file_area_tree_root hot_cold_file_area_tree_root_node;
	//file_stat锁
	spinlock_t file_stat_lock;
	//file_stat里age最大的file_area的age，调试用
	//unsigned long max_file_area_age;
#ifdef ASYNC_MEMORY_RECLAIM_DEBUG	
	/*最近一次访问的page的file_area所在的父节点，通过它直接得到file_area，然后得到page，不用每次都遍历xarray tree*/
	struct xa_node *xa_node_cache;
	/*xa_node_cache父节点保存的起始file_area的page的索引*/
	pgoff_t  xa_node_cache_base_index;
#endif	

	union{
		//cache文件file_stat最近一次被异步内存回收访问时的age，调试用
		unsigned int recent_access_age;
		//mmap文件在扫描完一轮file_stat->temp链表上的file_area，进入冷却期，cooling_off_start_age记录当时的global age
		unsigned int cooling_off_start_age;
	};
	/*记录该文件file_stat被遍历时的全局age*/
	unsigned int recent_traverse_age;
	/*统计一个周期内file_stat->temp链表上file_area移动到file_stat->temp链表头的次数，每一个一次减1，减少到0则禁止
	 *file_stat->temp链表上file_area再移动到file_stat->temp链表头*/
	short file_area_move_to_head_count;


	/**针对mmap文件新增的****************************/
	//最新一次访问的file_area，mmap文件用
	struct file_area *file_area_last;
	//件file_stat->file_area_temp链表上已经扫描的file_stat个数，如果达到file_area_count_in_temp_list，说明这个文件的file_stat扫描完了，才会扫描下个文件file_stat的file_area
	//unsigned int scan_file_area_count_temp_list;-
	//在文件file_stat->file_area_temp链表上的file_area个数
	unsigned int file_area_count_in_temp_list;
	//文件 mapcount大于1的file_area的个数
	//unsigned int mapcount_file_area_count;--------------------------------------
	//当扫描完一轮文件file_stat的temp链表上的file_area时，置1，进入冷却期，在N个age周期内不再扫描这个文件上的file_area。
	bool cooling_off_start;
	
	//处于中间状态的file_area结构添加到这个链表，新分配的file_area就添加到这里
	struct list_head file_area_temp;
	/************base***base***base************/
}/*__attribute__((packed))*/;
struct file_stat_tiny_small
{
	struct file_stat_base file_stat_base;
}/*__attribute__((packed))*/;

/*注意，有个隐藏的问题，在filemap.c函数里，是直接p_file_stat = (struct file_stat_base *)mapping->rh_reserved1，
 *令p_file_stat指向mapping->rh_reserved1内存最开始的地址得到file_stat_base，并不是通过结构体成员的形式获取。因此，
 成员file_stat_base file_stat_base必须放到struct file_stat_small结构体最开头。还要加上__attribute__((packed))
 禁止编译器优化file_stat_small结构体内部的成员布局，禁止为了凑够8字节对齐而填充空间(比如，一个1字节大小的变量占
 空间8个字节)，这会令(struct file_stat_base *)mapping->rh_reserved1获取到的file_stat_base存在地址偏差!!!!!!!!!。
 最后，决定alloc_file_stat时，直接mapping->rh_reserved1 = &file_stat.file_stat_base，就是令mapping->rh_reserved1
 直接指向file_stat.file_stat_base结构体，这样p_file_stat = (struct file_stat_base *)mapping->rh_reserved1
 p_file_stat一定指向的是file_stat.file_stat_base结构体，不会再有对齐问题，__attribute__((packed))就不需要了*/
struct file_stat_small
{
	struct file_stat_base file_stat_base;
	/*hot、refault、free 等状态的file_area移动到这个链表*/
	struct list_head file_area_other;
}/*__attribute__((packed))*/;
//热点文件统计信息，一个文件一个
struct file_stat
{
	struct file_stat_base file_stat_base;
	unsigned int file_area_hot_count;
	unsigned int mapcount_file_area_count;

	//频繁被访问的文件page对应的file_area存入这个头结点
	struct list_head file_area_hot;
	/*快接近冷file_area的移动到这个链表*/
	struct list_head file_area_warm;
	/*每轮扫描被释放内存page的file_area结构临时先添加到这个链表，这个变量可以省掉。把这些file_area移动到临时链表，
	 *参与内存回收再移动到file_stat->free链表*/
	//struct list_head file_area_free_temp;
	//所有被释放内存page的file_area结构最后添加到这个链表，如果长时间还没被访问，就释放file_area结构。
	struct list_head file_area_free;
	//file_area的page被释放后，但很快又被访问，发生了refault，于是要把这种page添加到file_area_refault链表，短时间内不再考虑扫描和释放
	struct list_head file_area_refault;
	//file_area对应的page的pagecount大于0的，则把file_area移动到该链表
	struct list_head file_area_mapcount;
	//存放内存回收的file_area，mmap文件用
	struct list_head file_area_free_temp;
}/*__attribute__((packed))*/;

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
	struct list_head file_stat_small_file_head;
	struct list_head file_stat_tiny_small_file_head;
	/*中等大小文件移动到这个链表*/
	struct list_head file_stat_middle_file_head;
	/*如果文件file_stat上的page cache数太多，被判定为大文件，则把file_stat移动到这个链表。将来内存回收时，优先遍历这种file_stat，
	 *因为file_area足够多，能遍历到更多的冷file_area，回收到内存page*/
	struct list_head file_stat_large_file_head;
	struct list_head cold_file_head;
	//inode被删除的文件的file_stat移动到这个链表
	struct list_head file_stat_delete_head;
	struct list_head file_stat_small_delete_head;
	struct list_head file_stat_tiny_small_delete_head;
	//0个file_area的file_stat移动到这个链表
	struct list_head file_stat_zero_file_area_head;
	struct list_head file_stat_small_zero_file_area_head;
	struct list_head file_stat_tiny_small_zero_file_area_head;
	//触发drop_cache后的没有file_stat的文件，分配file_stat后保存在这个链表
	struct list_head drop_cache_file_stat_head;

	//触发drop_cache后的没有file_stat的文件个数
	unsigned int drop_cache_file_count;
	//热文件file_stat个数
	unsigned int file_stat_hot_count;
	//大文件file_stat个数
	unsigned int file_stat_large_count;
	unsigned int file_stat_middle_count;
	//文件file_stat个数
	unsigned int file_stat_count;
	unsigned int file_stat_small_count;
	unsigned int file_stat_tiny_small_count;
	//0个file_area的file_stat个数
	unsigned int file_stat_count_zero_file_area;
	unsigned int file_stat_small_count_zero_file_area;
	unsigned int file_stat_tiny_small_count_zero_file_area;

	/*当file_stat的file_area个数达到file_area_level_for_large_file时，表示该文件的page cache数太多，被判定为大文件。但一个file_area
	 *包含了多个page，一个file_area并不能填满page，因此实际file_stat的file_area个数达到file_area_level_for_large_file时，实际该文件的的page cache数会少点*/
	unsigned int file_area_level_for_large_file;
	unsigned int file_area_level_for_middle_file;
	//当一个文件的文件页page数大于nr_pages_level时，该文件的文件页page才会被本异步内存回收模块统计访问频率并回收，默认15，即64k，可通过proc接口调节大小
	unsigned int nr_pages_level;

	struct kmem_cache *file_stat_cachep;
	struct kmem_cache *file_stat_small_cachep;
	struct kmem_cache *file_stat_tiny_small_cachep;

	struct kmem_cache *file_area_cachep;
	//保存文件file_stat所有file_area的radix tree
	struct kmem_cache *hot_cold_file_area_tree_node_cachep;
	struct hot_cold_file_node_pgdat *p_hot_cold_file_node_pgdat;
	//异步内存回收线程，每个周期运行一次
	struct task_struct *hot_cold_file_thead;
	//负责内存回收，由hot_cold_file_thead线程唤醒，才会进行内存回收
	struct task_struct *async_memory_reclaim;
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
	unsigned int global_age;
	//异步内存回收周期，单位s
	unsigned int global_age_period;
	//热file_area经过file_area_refault_to_temp_age_dx个周期后，还没有被访问，则移动到file_area_temp链表
	unsigned int file_area_hot_to_temp_age_dx;
	//发生refault的file_area经过file_area_refault_to_temp_age_dx个周期后，还没有被访问，则移动到file_area_temp链表
	unsigned int file_area_refault_to_temp_age_dx;
	//普通的file_area在file_area_temp_to_cold_age_dx个周期内没有被访问则被判定是冷file_area，然后释放这个file_area的page
	unsigned int file_area_temp_to_cold_age_dx;
	//普通的file_area在file_area_temp_to_warm_age_dx个周期内没有被访问则被判定是温file_area，然后把这个file_area移动到file_stat->file_area_warm链表
	unsigned int file_area_temp_to_warm_age_dx;
	/*在file_stat->warm上的file_area经过file_area_warm_to_temp_age_dx个周期没有被访问，则移动到file_stat->temp链表*/
	unsigned int file_area_warm_to_temp_age_dx;
	/*正常情况不会回收read属性的file_area的page，但是如果该file_area确实很长很长很长时间没访问，也参与回收*/
	unsigned int file_area_reclaim_read_age_dx;
	//一个冷file_area，如果经过file_area_free_age_dx_fops个周期，仍然没有被访问，则释放掉file_area结构
	unsigned int file_area_free_age_dx;
	//当一个文件file_stat长时间不被访问，释放掉了所有的file_area，再过file_stat_delete_age_dx个周期，则释放掉file_stat结构
	unsigned int file_stat_delete_age_dx;
	/*一个周期内，运行一个文件file_stat->temp链表头向前链表头移动的file_area个数*/
	unsigned int file_area_move_to_head_count_max;

	//发生refault的次数,累加值
	unsigned long all_refault_count;
		

	char support_fs_type;
	char support_fs_uuid[SUPPORT_FS_COUNT][SUPPORT_FS_UUID_LEN];
	char support_fs_against_uuid[SUPPORT_FS_UUID_LEN];
	char support_fs_name[SUPPORT_FS_COUNT][SUPPORT_FS_NAME_LEN];

	/**针对mmap文件新增的****************************/
	//新分配的文件file_stat默认添加到file_stat_temp_head链表
	struct list_head mmap_file_stat_uninit_head;
	//当一个文件的page都遍历完后，file_stat移动到这个链表
	struct list_head mmap_file_stat_temp_head;
	struct list_head mmap_file_stat_small_file_head;
	struct list_head mmap_file_stat_tiny_small_file_head;
	struct list_head mmap_file_stat_middle_file_head;
	//文件file_stat个数超过阀值移动到这个链表
	struct list_head mmap_file_stat_large_file_head;
	//热文件移动到这个链表
	struct list_head mmap_file_stat_hot_head;
	//一个文件有太多的page的mmapcount都大于1，则把该文件file_stat移动该链表
	struct list_head mmap_file_stat_mapcount_head;
	//0个file_area的file_stat移动到这个链表，暂时没用到
	struct list_head mmap_file_stat_zero_file_area_head;
	//inode被删除的文件的file_stat移动到这个链表，暂时不需要
	struct list_head mmap_file_stat_delete_head;
	struct list_head mmap_file_stat_small_delete_head;
	struct list_head mmap_file_stat_tiny_small_delete_head;
	//每个周期频繁冗余lru_lock的次数
	unsigned int lru_lock_count;
	unsigned int mmap_file_lru_lock_count;

	//mmap文件用的全局锁
	spinlock_t mmap_file_global_lock;

	struct file_stat *file_stat_last;
	//mmap文件个数
	unsigned int mmap_file_stat_count;
	unsigned int mmap_file_stat_small_count;
	unsigned int mmap_file_stat_tiny_small_count;
	//mapcount文件个数
	unsigned int mapcount_mmap_file_stat_count;
	//热文件个数
	unsigned int hot_mmap_file_stat_count;
	struct mmap_file_shrink_counter mmap_file_shrink_counter;
	/*当file_stat的file_area个数达到file_area_level_for_large_mmap_file时，表示该文件的page cache数太多，被判定为大文件*/
	unsigned int mmap_file_area_level_for_large_file;
	unsigned int mmap_file_area_level_for_middle_file;

	unsigned int mmap_file_area_hot_to_temp_age_dx;
	unsigned int mmap_file_area_refault_to_temp_age_dx;
	unsigned int mmap_file_area_temp_to_cold_age_dx;
	unsigned int mmap_file_area_free_age_dx;
	unsigned int mmap_file_area_temp_to_warm_age_dx;
	unsigned int mmap_file_area_warm_to_temp_age_dx;
	unsigned int mmap_file_area_hot_age_dx;

	unsigned int normal_zone_free_pages_last;
	unsigned int dma32_zone_free_pages_last;
	unsigned int highmem_zone_free_pages_last;
	unsigned int normal1_zone_free_pages_last;
	/*内存紧张等级，越大表示内存越紧张，并且还会回收有read标记和ahead标记的file_area的page*/
	unsigned int memory_pressure_level;
	
	//从系统启动到目前释放的page个数
	unsigned long free_pages;
	//从系统启动到目前释放的mmap page个数
	unsigned long free_mmap_pages;
	//在内存回收期间产生的refault file_area个数
	unsigned long check_refault_file_area_count;
	unsigned long check_mmap_refault_file_area_count;
	unsigned long update_file_area_temp_list_count;
	unsigned long update_file_area_warm_list_count;
	unsigned long update_file_area_free_list_count;

	unsigned long update_file_area_other_list_count;
	unsigned long update_file_area_move_to_head_count;
	
	unsigned long file_stat_delete_protect;
};


/*******file_area状态**********************************************************/

/*file_area_state是char类型，只有8个bit位可设置 !!!!!!!!!!!!!!!!!!!!!!!!!!!*/
enum file_area_status{
	F_file_area_in_temp_list,
	F_file_area_in_hot_list,
	F_file_area_in_warm_list,
	F_file_area_in_free_list,

	F_file_area_in_refault_list,
	/*file_area对应的page的pagecount大于0的，则把file_area移动到该链表*/
	F_file_area_in_mapcount_list,
	/*file_area连续几个周期被访问，本要移动到链表头，处于性能考虑，只是设置file_area的ahead标记。
	 *内存回收遇到有ahead且长时间没访问的file_area，先豁免一次，等下次遍历到这个file_area再回收这个file_area的page*/
	F_file_area_in_ahead,
	F_file_area_in_read,
	//F_file_area_in_cache,//file_area保存在ile_stat->hot_file_area_cache[]数组里
};
//不能使用 clear_bit_unlock、test_and_set_bit_lock、test_bit，因为要求p_file_area->file_area_state是64位数据，但实际只是u8型数据

#define MAX_FILE_AREA_LIST_BIT F_file_area_in_mapcount_list
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
FILE_AREA_LIST_STATUS(warm_list)
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

	//FILE_AREA_STATUS(cache)
	FILE_AREA_STATUS(ahead)
FILE_AREA_STATUS(read)


#define file_area_in_temp_list_not_have_hot_status (1 << F_file_area_in_temp_list)
#define file_area_in_warm_list_not_have_hot_status (1 << F_file_area_in_warm_list)
#define file_area_in_free_list_not_have_refault_status (1 << F_file_area_in_free_list)


/*******file_stat状态**********************************************************/
enum file_stat_status{//file_area_state是long类型，只有64个bit位可设置
	F_file_stat_in_file_stat_hot_head_list,
	F_file_stat_in_file_stat_tiny_small_file_head_list,
	F_file_stat_in_file_stat_small_file_head_list,
	F_file_stat_in_file_stat_temp_head_list,
	
	F_file_stat_in_file_stat_middle_file_head_list,
	F_file_stat_in_file_stat_large_file_head_list,
	F_file_stat_in_mapcount_file_area_list,//文件file_stat是mapcount文件
	F_file_stat_in_zero_file_area_list,
	
	F_file_stat_in_delete_file,//标识该file_stat被移动到了global delete链表
	//F_file_stat_in_drop_cache,
	//F_file_stat_in_free_page,//正在遍历file_stat的file_area的page，尝试释放page
	//F_file_stat_in_free_page_done,//正在遍历file_stat的file_area的page，完成了page的内存回收,
	F_file_stat_in_delete,//仅仅表示该file_stat被触发delete了，并不能说明file_stat被移动到了global delete链表
	F_file_stat_in_cache_file,//cache文件，sysctl读写产生pagecache。有些cache文件可能还会被mmap映射，要与mmap文件互斥
	F_file_stat_in_mmap_file,//mmap文件，有些mmap文件可能也会被sysctl读写产生pagecache，要与cache文件互斥
	//F_file_stat_in_large_file,
	F_file_stat_in_from_cache_file,//mmap文件是从cache文件的global temp链表移动过来的
	F_file_stat_in_from_small_file,//该文件是从small文件的global small_temp链表移动过来的
	F_file_stat_in_replaced_file,//file_stat_tiny_small或file_stat_small转成更大的文件时，老的file_stat被标记replaced
	//F_file_stat_lock,
	//F_file_stat_lock_not_block,//这个bit位置1，说明inode在删除的，但是获取file_stat锁失败
};
//不能使用 clear_bit_unlock、test_and_set_bit_lock、test_bit，因为要求p_file_stat->file_stat_status是64位数据，但这里只是u8型数据

#define MAX_FILE_STAT_LIST_BIT F_file_stat_in_mapcount_file_area_list
#define FILE_STAT_LIST_MASK ((1 << (MAX_FILE_STAT_LIST_BIT + 1)) - 1)

#if 0
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
FILE_STAT_STATUS(file_stat_middle_file_head)
FILE_STAT_STATUS(file_stat_large_file_head)
FILE_STAT_STATUS(zero_file_area)
FILE_STAT_STATUS(mapcount_file_area)
#endif

	//清理file_stat的状态，在哪个链表
#define CLEAR_FILE_STAT_STATUS_BASE(name)\
		static inline void clear_file_stat_in_##name##_list_base(struct file_stat_base *p_file_stat_base)\
{p_file_stat_base->file_stat_status &= ~(1 << F_file_stat_in_##name##_list);}
	//设置file_stat在哪个链表的状态
#define SET_FILE_STAT_STATUS_BASE(name)\
		static inline void set_file_stat_in_##name##_list_base(struct file_stat_base *p_file_stat_base)\
{p_file_stat_base->file_stat_status |= (1 << F_file_stat_in_##name##_list);}
	//测试file_stat在哪个链表
#define TEST_FILE_STAT_STATUS_BASE(name)\
		static inline int file_stat_in_##name##_list_base(struct file_stat_base *p_file_stat_base)\
{return (p_file_stat_base->file_stat_status & (1 << F_file_stat_in_##name##_list));}
#define TEST_FILE_STAT_STATUS_ERROR_BASE(name)\
		static inline int file_stat_in_##name##_list##_error_base(struct file_stat_base *p_file_stat_base)\
{return p_file_stat_base->file_stat_status & (~(1 << F_file_stat_in_##name##_list) & FILE_STAT_LIST_MASK);}

#define FILE_STAT_STATUS_BASE(name) \
		CLEAR_FILE_STAT_STATUS_BASE(name) \
	SET_FILE_STAT_STATUS_BASE(name) \
	TEST_FILE_STAT_STATUS_BASE(name) \
	TEST_FILE_STAT_STATUS_ERROR_BASE(name)

FILE_STAT_STATUS_BASE(file_stat_hot_head)
FILE_STAT_STATUS_BASE(file_stat_temp_head)
FILE_STAT_STATUS_BASE(file_stat_middle_file_head)
FILE_STAT_STATUS_BASE(file_stat_large_file_head)
FILE_STAT_STATUS_BASE(file_stat_small_file_head)
FILE_STAT_STATUS_BASE(file_stat_tiny_small_file_head)
FILE_STAT_STATUS_BASE(zero_file_area)
FILE_STAT_STATUS_BASE(mapcount_file_area)


#if 0
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

FILE_STATUS(delete)
FILE_STATUS(delete_file)
FILE_STATUS(cache_file)
FILE_STATUS(mmap_file)
FILE_STATUS(from_cache_file)
FILE_STATUS(from_small_file)
FILE_STATUS(replaced_file)
#endif


	//清理文件的状态，大小文件等
#define CLEAR_FILE_STATUS_BASE(name)\
		static inline void clear_file_stat_in_##name##_base(struct file_stat_base *p_file_stat_base)\
{p_file_stat_base->file_stat_status &= ~(1 << F_file_stat_in_##name);}
	//设置文件的状态，大小文件等
#define SET_FILE_STATUS_BASE(name)\
		static inline void set_file_stat_in_##name##_base(struct file_stat_base *p_file_stat_base)\
{p_file_stat_base->file_stat_status |= (1 << F_file_stat_in_##name);}
	//测试文件的状态，大小文件等
#define TEST_FILE_STATUS_BASE(name)\
		static inline int file_stat_in_##name##_base(struct file_stat_base *p_file_stat_base)\
{return (p_file_stat_base->file_stat_status & (1 << F_file_stat_in_##name));}
#define TEST_FILE_STATUS_ERROR_BASE(name)\
		static inline int file_stat_in_##name##_error_base(struct file_stat_base *p_file_stat_base)\
{return p_file_stat_base->file_stat_status & (~(1 << F_file_stat_in_##name) & FILE_STAT_LIST_MASK);}

#define FILE_STATUS_BASE(name) \
		CLEAR_FILE_STATUS_BASE(name) \
	SET_FILE_STATUS_BASE(name) \
	TEST_FILE_STATUS_BASE(name)\
	TEST_FILE_STATUS_ERROR_BASE(name)

FILE_STATUS_BASE(delete)
FILE_STATUS_BASE(delete_file)
FILE_STATUS_BASE(cache_file)
FILE_STATUS_BASE(mmap_file)
FILE_STATUS_BASE(from_cache_file)
FILE_STATUS_BASE(from_small_file)
FILE_STATUS_BASE(replaced_file)


/*设置/清除file_stat状态使用test_and_set_bit/clear_bit，是异步内存回收1.0版本的产物，现在不再需要。
 *因为设置/清理file_stat状态全都spin_lock加锁。并且会置/清除file_stat状态的只有两个场景，
 *一个是第1次创建file_stat添加到global temp链表时，这个有spin_lock加锁不用担心。第2个场景是，
 *文件被异步iput()释放file_stat并标记file_stat delete，此时也有spin_lock加锁，但是异步内存回收线程
 *遍历global temp/small/tiny small文件时，会spin_lock加锁情况下在is_file_stat_mapping_error()函数里
 *判断file_stat是否有delete标记。并且，异步内存回收线程会在对file_stat进行内存回收时判断file_stat是否有
 *delete标记。这就得非常注意了，因为file_stat的delete标记可能不会立即感知到，因此这两处异步内存回收线程
 *还通过内存屏障+其他变量辅助判断方法，判断file_stat是否有delete标记。后续，如果还有其他地方要判断
 *file_stat是否有delete标记，需要特别注意
 */
#if 0 
	//-----------------------------这段注释掉的代码不要删除
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
//FILE_STATUS_ATOMIC(free_page)
//FILE_STATUS_ATOMIC(free_page_done)
/*标记file_stat delete可能在cold_file_stat_delete()和__destroy_inode_handler_post()并发执行，存在重复设置可能，用FILE_STATUS_ATOMIC会因重复设置而crash*/
//FILE_STATUS_ATOMIC(delete)
FILE_STATUS_ATOMIC(cache_file)//------------设置这些file_stat状态都有spin_lock加锁，因为不用
FILE_STATUS_ATOMIC(mmap_file)
FILE_STATUS_ATOMIC(from_cache_file)
FILE_STATUS_ATOMIC(from_small_file)
FILE_STATUS_ATOMIC(replaced_file)

	//清理文件的状态，大小文件等
#define CLEAR_FILE_STATUS_ATOMIC_BASE(name)\
		static inline void clear_file_stat_in_##name##_base(struct file_stat_base *p_file_stat_base)\
{clear_bit_unlock(F_file_stat_in_##name,&p_file_stat_base->file_stat_status);}
	//设置文件的状态，大小文件等
#define SET_FILE_STATUS_ATOMIC_BASE(name)\
		static inline void set_file_stat_in_##name##_base(struct file_stat_base *p_file_stat_base)\
{if(test_and_set_bit_lock(F_file_stat_in_##name,&p_file_stat_base->file_stat_status)) \
	/*如果这个file_stat的bit位被多进程并发设置，不可能,应该发生了某种异常，触发crash*/  \
	panic("file_stat:0x%llx status:0x%lx alreay set %d bit\n",(u64)p_file_stat_base,p_file_stat_base->file_stat_status,F_file_stat_in_##name); \
}
	//测试文件的状态，大小文件等
#define TEST_FILE_STATUS_ATOMIC_BASE(name)\
		static inline int file_stat_in_##name##_base(struct file_stat_base *p_file_stat_base)\
{return test_bit(F_file_stat_in_##name,&p_file_stat_base->file_stat_status);}
#define TEST_FILE_STATUS_ATOMIC_ERROR_BASE(name)\
		static inline int file_stat_in_##name##_error_base(struct file_stat_base *p_file_stat_base)\
{return p_file_stat_base->file_stat_status & (~(1 << F_file_stat_in_##name) & FILE_STAT_LIST_MASK);}

#define FILE_STATUS_ATOMIC_BASE(name) \
		CLEAR_FILE_STATUS_ATOMIC_BASE(name) \
	SET_FILE_STATUS_ATOMIC_BASE(name) \
	TEST_FILE_STATUS_ATOMIC_BASE(name) \
	TEST_FILE_STATUS_ATOMIC_ERROR_BASE(name) \

	FILE_STATUS_ATOMIC_BASE(cache_file)
FILE_STATUS_ATOMIC_BASE(mmap_file)

	FILE_STATUS_ATOMIC_BASE(from_cache_file)
FILE_STATUS_ATOMIC_BASE(from_small_file)
FILE_STATUS_ATOMIC_BASE(replaced_file)
#endif

extern struct hot_cold_file_global hot_cold_file_global_info;

extern unsigned long async_memory_reclaim_status;
extern unsigned int file_area_in_update_count;
extern unsigned int file_area_in_update_lock_count;
extern unsigned int file_area_move_to_head_count;

extern unsigned int enable_xas_node_cache;
extern unsigned int enable_update_file_area_age;
extern int shrink_page_printk_open1;
extern int shrink_page_printk_open;
extern unsigned int xarray_tree_node_cache_hit;
extern int open_file_area_printk;
extern int open_file_area_printk_important;

/** file_area的page bit/writeback mark bit/dirty mark bit/towrite mark bit统计**************************************************************/
#define FILE_AREA_PAGE_COUNT_SHIFT (XA_CHUNK_SHIFT + PAGE_COUNT_IN_AREA_SHIFT)//6+2
#define FILE_AREA_PAGE_COUNT_MASK ((1 << FILE_AREA_PAGE_COUNT_SHIFT) - 1)//0xFF 

/*file_area->file_area_state 的bit31~bit28 这个4个bit位标志file_area。注意，现在按照一个file_area只有4个page在
 *p_file_area->file_area_state的bit28~bit31写死了。如果file_area代表8个page，这里就得改动了!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * */
//#define PAGE_BIT_OFFSET_IN_FILE_AREA_BASE (sizeof(&p_file_area->file_area_state)*8 - PAGE_COUNT_IN_AREA)//28  这个编译不通过
#define PAGE_BIT_OFFSET_IN_FILE_AREA_BASE (sizeof(unsigned int)*8 - PAGE_COUNT_IN_AREA)

/*writeback mark:bit27~bit24 dirty mark:bit23~bit20  towrite mark:bit19~bit16*/
#define WRITEBACK_MARK_IN_FILE_AREA_BASE (sizeof(unsigned int)*8 - PAGE_COUNT_IN_AREA*2)
#define DIRTY_MARK_IN_FILE_AREA_BASE     (sizeof(unsigned int)*8 - PAGE_COUNT_IN_AREA*3)
#define TOWRITE_MARK_IN_FILE_AREA_BASE   (sizeof(unsigned int)*8 - PAGE_COUNT_IN_AREA*4)

#define FILE_AREA_PRINT(fmt,...) \
    do{ \
        if(open_file_area_printk) \
			printk(fmt,##__VA_ARGS__); \
	}while(0);

#define FILE_AREA_PRINT1(fmt,...) \
    do{ \
        if(open_file_area_printk_important) \
			printk(fmt,##__VA_ARGS__); \
	}while(0);

#ifdef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY

#define CHECK_FOLIO_FROM_FILE_AREA_VALID(xas,folio,p_file_area,page_offset_in_file_area,folio_index_from_xa_index) \
	if((folio)->index != folio_index_from_xa_index)\
panic("%s xas:0x%llx file_area:0x%llx folio:0x%llx page_offset_in_file_area:%d\n",__func__,(u64)xas,(u64)p_file_area,(u64)folio,page_offset_in_file_area);

#define CHECK_FOLIO_FROM_FILE_AREA_VALID_MARK(xas,folio,folio_from_file_area,p_file_area,page_offset_in_file_area,folio_index_from_xa_index) \
	if(folio->index != folio_index_from_xa_index || folio != folio_from_file_area)\
panic("%s xas:0x%llx file_area:0x%llx folio:0x%llx folio_from_file_area:0x%llx page_offset_in_file_area:%d\n",__func__,(u64)xas,(u64)p_file_area,(u64)folio,(u64)folio_from_file_area,page_offset_in_file_area);

#else

#define CHECK_FOLIO_FROM_FILE_AREA_VALID(xas,folio,p_file_area,page_offset_in_file_area,folio_index_from_xa_index) \
	if((folio)->index != ((p_file_area)->start_index + page_offset_in_file_area) || (folio)->index != folio_index_from_xa_index)\
panic("%s xas:0x%llx file_area:0x%llx folio:0x%llx page_offset_in_file_area:%d\n",__func__,(u64)xas,(u64)p_file_area,(u64)folio,page_offset_in_file_area);

#define CHECK_FOLIO_FROM_FILE_AREA_VALID_MARK(xas,folio,folio_from_file_area,p_file_area,page_offset_in_file_area,folio_index_from_xa_index) \
	if(folio->index != ((p_file_area)->start_index + page_offset_in_file_area) || folio->index != folio_index_from_xa_index || folio != folio_from_file_area)\
panic("%s xas:0x%llx file_area:0x%llx folio:0x%llx folio_from_file_area:0x%llx page_offset_in_file_area:%d\n",__func__,(u64)xas,(u64)p_file_area,(u64)folio,(u64)folio_from_file_area,page_offset_in_file_area);

#endif

static inline struct file_area *entry_to_file_area(void * file_area_entry)
{
	return (struct file_area *)((unsigned long)file_area_entry | 0x8000000000000000);
}
static inline void *file_area_to_entry(struct file_area *p_file_area)
{
	return (void *)((unsigned long)p_file_area & 0x7fffffffffffffff);
}
static inline int is_file_area_entry(void *file_area_entry)
{
	//最高的4个bit位依次是 0、1、1、1 则说明是file_area_entry，bit0和bit1也得是1
	return ((unsigned long)file_area_entry & 0xF000000000000003) == 0x7000000000000000;
}
static inline void clear_file_area_page_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{
	unsigned int file_area_page_bit_clear = ~(1 << (PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area));
	unsigned int file_area_page_bit_set = 1 << (PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area);
	//如果这个page在 p_file_area->file_area_state对应的bit位没有置1，触发panic
	//if((p_file_area->file_area_state | file_area_page_bit_clear) != (sizeof(&p_file_area->file_area_state)*8 - 1))
	if((p_file_area->file_area_state & file_area_page_bit_set) == 0)
		panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d file_area_page_bit_set:0x%x already clear\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,file_area_page_bit_set);

	//page在 p_file_area->file_area_state对应的bit位清0
	p_file_area->file_area_state = p_file_area->file_area_state & file_area_page_bit_clear;

}
static inline void set_file_area_page_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{
	unsigned int file_area_page_bit_set = 1 << (PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area);
	//如果这个page在 p_file_area->file_area_state对应的bit位已经置1了，触发panic
	if(p_file_area->file_area_state & file_area_page_bit_set)
		panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d file_area_page_bit_set:0x%x already set\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,file_area_page_bit_set);

	//page在 p_file_area->file_area_state对应的bit位置1
	p_file_area->file_area_state = p_file_area->file_area_state | file_area_page_bit_set;
}
//测试page_offset_in_file_area这个位置的page在p_file_area->file_area_state对应的bit位是否置1了
static inline int is_file_area_page_bit_set(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{
	unsigned int file_area_page_bit_set = 1 << (PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area);

	return (p_file_area->file_area_state & file_area_page_bit_set);
}
static inline int file_area_have_page(struct file_area *p_file_area)
{
	return  (p_file_area->file_area_state & ~((1 << PAGE_BIT_OFFSET_IN_FILE_AREA_BASE) - 1));//0XF000 0000
}


/*探测file_area里的page是读还是写*/
static inline void set_file_area_page_read(struct file_area *p_file_area/*,unsigned char page_offset_in_file_area*/)
{
    set_file_area_in_read(p_file_area);
}
static inline void clear_file_area_page_read(struct file_area *p_file_area/*,unsigned char page_offset_in_file_area*/)
{
    clear_file_area_in_read(p_file_area);
}
static inline int file_area_page_is_read(struct file_area *p_file_area/*,unsigned char page_offset_in_file_area*/)
{
	return  file_area_in_read(p_file_area);
}

/*清理file_area所有的towrite、dirty、writeback的mark标记。这个函数是在把file_area从xarray tree剔除时执行的，之后file_area是无效的，有必要吗????????????*/
static inline void clear_file_area_towrite_dirty_writeback_mark(struct file_area *p_file_area)
{
    
}
static inline void clear_file_area_page_mark_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area,xa_mark_t type)
{
	unsigned int file_area_page_bit_clear;

	if(PAGECACHE_TAG_DIRTY == type){
		file_area_page_bit_clear = ~(1 << (DIRTY_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area));
	}else if (PAGECACHE_TAG_WRITEBACK == type){
		file_area_page_bit_clear = ~(1 << (WRITEBACK_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area));
	}else{
		if(PAGECACHE_TAG_TOWRITE != type)
			panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d type:%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,type);

		file_area_page_bit_clear = ~(1 << (TOWRITE_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area));
	}
	//page在 p_file_area->file_area_state对应的bit位清0
	p_file_area->file_area_state = p_file_area->file_area_state & file_area_page_bit_clear;

}
static inline void set_file_area_page_mark_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area,xa_mark_t type)
{
	unsigned int file_area_page_mark_bit_set;

	if(PAGECACHE_TAG_DIRTY == type){
		file_area_page_mark_bit_set = 1 << (DIRTY_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area);
	}else if (PAGECACHE_TAG_WRITEBACK == type){
		file_area_page_mark_bit_set = 1 << (WRITEBACK_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area);
	}else{
		if(PAGECACHE_TAG_TOWRITE != type)
			panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d type:%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,type);

		file_area_page_mark_bit_set = 1 << (TOWRITE_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area);
	}

	//page在 p_file_area->file_area_state对应的bit位置1
	p_file_area->file_area_state = p_file_area->file_area_state | file_area_page_mark_bit_set;
}
//测试page_offset_in_file_area这个位置的page在p_file_area->file_area_state对应的bit位是否置1了
static inline int is_file_area_page_mark_bit_set(struct file_area *p_file_area,unsigned char page_offset_in_file_area,xa_mark_t type)
{
	unsigned int file_area_page_mark_bit_set;

	if(PAGECACHE_TAG_DIRTY == type){
		file_area_page_mark_bit_set = 1 << (DIRTY_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area);
	}else if (PAGECACHE_TAG_WRITEBACK == type){
		file_area_page_mark_bit_set = 1 << (WRITEBACK_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area);
	}else{
		if(PAGECACHE_TAG_TOWRITE != type)
			panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d type:%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,type);

		file_area_page_mark_bit_set = 1 << (TOWRITE_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area);
	}

	return (p_file_area->file_area_state & file_area_page_mark_bit_set);
}

/*统计有多少个 mark page置位了，比如file_area有3个page是writeback，则返回3*/
static inline int file_area_page_mark_bit_count(struct file_area *p_file_area,char type)
{
	unsigned int file_area_page_mark;
	int count = 0;
	unsigned long page_mark_mask = (1 << PAGE_COUNT_IN_AREA) - 1;/*与上0xF，得到4个bit哪些置位0*/

	if(PAGECACHE_TAG_DIRTY == type){
		file_area_page_mark = (p_file_area->file_area_state >> DIRTY_MARK_IN_FILE_AREA_BASE) & page_mark_mask;
	}else if (PAGECACHE_TAG_WRITEBACK == type){
		file_area_page_mark = (p_file_area->file_area_state >> WRITEBACK_MARK_IN_FILE_AREA_BASE) & page_mark_mask;
	}else{
		if(PAGECACHE_TAG_TOWRITE != type)
			panic("%s file_area:0x%llx file_area_state:0x%x type:%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,type);

		file_area_page_mark = (p_file_area->file_area_state >> TOWRITE_MARK_IN_FILE_AREA_BASE) & page_mark_mask;
	}
	while(file_area_page_mark){
		if(file_area_page_mark & 0x1)
			count ++;

		file_area_page_mark = file_area_page_mark >> 1;
	}

	return count;
}
static inline void is_cold_file_area_reclaim_support_fs(struct address_space *mapping,struct super_block *sb)
{
	if(SUPPORT_FS_ALL == hot_cold_file_global_info.support_fs_type){
		if(sb->s_type){
			if(0 == strcmp(sb->s_type->name,"ext4") || 0 == strcmp(sb->s_type->name,"xfs") || 0 == strcmp(sb->s_type->name,"f2fs"))
				mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE;
		}
	}
	else if(SUPPORT_FS_SINGLE == hot_cold_file_global_info.support_fs_type){
		if(sb->s_type){
			int i;
			for(i = 0;i < SUPPORT_FS_COUNT;i ++){
				if(0 == strcmp(sb->s_type->name,hot_cold_file_global_info.support_fs_name[i])){
					mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE;
					break;
				}
			}
		}
	}
	else if(SUPPORT_FS_UUID == hot_cold_file_global_info.support_fs_type){
		if(0 == memcmp(sb->s_uuid.b , hot_cold_file_global_info.support_fs_uuid[0], SUPPORT_FS_UUID_LEN) || 0 == memcmp(sb->s_uuid.b , hot_cold_file_global_info.support_fs_uuid[1], SUPPORT_FS_UUID_LEN)){
			if(memcmp(sb->s_uuid.b , hot_cold_file_global_info.support_fs_against_uuid, SUPPORT_FS_UUID_LEN))
			    mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE;
		}	
	}
}
/* 测试文件支持file_area形式读写文件和内存回收，并且已经分配了file_stat
 * mapping->rh_reserved1有3种状态
 *情况1:mapping->rh_reserved1是0：文件所属文件系统不支持file_area形式读写文件和内存回收
  情况2:mapping->rh_reserved1是1: 文件inode是初始化状态，但还没有读写文件而分配file_stat；或者文件读写后长时间未读写而文件页page全回收，
     file_stat被释放了。总之此时文件file_stat未分配，一个文件页page都没有
  情况3:mapping->rh_reserved1大于1：此时文件分配file_stat，走filemap.c里for_file_area正常读写文件流程
 */
/*#define IS_SUPPORT_FILE_AREA_READ_WRITE(mapping) \
    (mapping->rh_reserved1 > SUPPORT_FILE_AREA_INIT_OR_DELETE) 移动到 include/linux/pagemap.h 文件了*/
/*测试文件支持file_area形式读写文件和内存回收，此时情况2(mapping->rh_reserved1是1)和情况3(mapping->rh_reserved1>1)都要返回true*/
/*#define IS_SUPPORT_FILE_AREA(mapping) \
	(mapping->rh_reserved1 >=  SUPPORT_FILE_AREA_INIT_OR_DELETE)*/

/*****************************************************************************************************************************************************/
extern int shrink_page_printk_open1;
extern int shrink_page_printk_open;

#ifdef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY 
/*folio非0，但不是有效的指针，即bit63是0，说明保存在file_area->folio[]中的是file_area的索引，不是有效的page指针*/
#define folio_is_file_area_index(folio) (((u64)folio & (1UL << 63)) == 0)

/*如果fiolio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
#define folio_is_file_area_index_and_clear_NULL(folio) \
{ \
	if(folio_is_file_area_index(folio))\
		folio = NULL; \
}

/*p_file_area)->pages[]中保存的file_area的索引file_area->start_index >> PAGE_COUNT_IN_AREA_SHIFT后的，不不用再左移PAGE_COUNT_IN_AREA_SHIFT后的*/
#define get_file_area_start_index(p_file_area) (((u64)((p_file_area)->pages[0]) << 32) + (u64)((p_file_area)->pages[1]))

#else
#define folio_is_file_area_index(folio) 0
#define folio_is_file_area_index(folio) {}
#endif

static inline void file_stat_delete_protect_lock(char is_cache_file)
{
	unsigned long lock_bit;
	if(is_cache_file)
		lock_bit = CACHE_FILE_DELETE_PROTECT_BIT;
	else
		lock_bit = MMAP_FILE_DELETE_PROTECT_BIT;

	while(test_and_set_bit_lock(lock_bit,&hot_cold_file_global_info.file_stat_delete_protect))
		cond_resched();
}
static inline int file_stat_delete_protect_try_lock(char is_cache_file)
{
	unsigned long lock_bit;
	if(is_cache_file)
		lock_bit = CACHE_FILE_DELETE_PROTECT_BIT;
	else
		lock_bit = MMAP_FILE_DELETE_PROTECT_BIT;

	return  (0 == test_and_set_bit_lock(lock_bit,&hot_cold_file_global_info.file_stat_delete_protect));
}
static inline void file_stat_delete_protect_unlock(char is_cache_file)
{
	unsigned long lock_bit;
	if(is_cache_file)
		lock_bit = CACHE_FILE_DELETE_PROTECT_BIT;
	else
		lock_bit = MMAP_FILE_DELETE_PROTECT_BIT;

	clear_bit_unlock(lock_bit,&hot_cold_file_global_info.file_stat_delete_protect);
}
static inline void file_stat_delete_protect_test_unlock(char is_cache_file)
{
	unsigned long lock_bit;
	if(is_cache_file)
		lock_bit = CACHE_FILE_DELETE_PROTECT_BIT;
	else
		lock_bit = MMAP_FILE_DELETE_PROTECT_BIT;

	if(!test_and_clear_bit(lock_bit,&hot_cold_file_global_info.file_stat_delete_protect))
		BUG();
}

static inline unsigned int get_file_area_list_status(struct file_area *p_file_area)
{
	return p_file_area->file_area_state & FILE_AREA_LIST_MASK;
}
static inline long get_file_stat_normal_type_all(struct file_stat_base *file_stat_base)
{
	unsigned long file_stat_type = file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;

	switch (file_stat_type){
		case 1 << F_file_stat_in_file_stat_temp_head_list:
		case 1 << F_file_stat_in_file_stat_middle_file_head_list:
		case 1 << F_file_stat_in_file_stat_large_file_head_list:
			return 1;

		default:
			return 0;
	}
	return 0;

}
static inline long get_file_stat_normal_type(struct file_stat_base *file_stat_base)
{
	unsigned long file_stat_type = file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;

	switch (file_stat_type){
		case 1 << F_file_stat_in_file_stat_temp_head_list:
			return TEMP_FILE;
		case 1 << F_file_stat_in_file_stat_middle_file_head_list:
			return MIDDLE_FILE;
		case 1 << F_file_stat_in_file_stat_large_file_head_list:
			return LARGE_FILE;

		default:
			return -1;
	}
	return -1;

}
/*判断文件是否是tiny small文件、small文件、普通文件*/
static inline long get_file_stat_type(struct file_stat_base *file_stat_base)
{
	unsigned long file_stat_type = file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;

	switch (file_stat_type){
		case 1 << F_file_stat_in_file_stat_small_file_head_list:
			return FILE_STAT_SMALL;
		case 1 << F_file_stat_in_file_stat_tiny_small_file_head_list:
			return FILE_STAT_TINY_SMALL;

		case 1 << F_file_stat_in_file_stat_temp_head_list:
		case 1 << F_file_stat_in_file_stat_hot_head_list:
		case 1 << F_file_stat_in_file_stat_middle_file_head_list:
		case 1 << F_file_stat_in_file_stat_large_file_head_list:
		case 1 << F_file_stat_in_mapcount_file_area_list:
			return FILE_STAT_NORMAL;

		default:
			return -1;
	}
	return -1;
}
#define is_file_stat_match_error(p_file_stat_base,file_type) \
{ \
	if(get_file_stat_type(p_file_stat_base) != file_type)  \
	panic("%s file_stat:0x%llx match file_type:%d error\n",__func__,(u64)p_file_stat_base,file_type); \
}

/*检测该file_stat跟file_stat->mapping->rh_reserved1是否一致。但如果检测时该文件被并发iput()，执行到__destroy_inode_handler_post()
 *赋值file_stat->mapping->rh_reserved1赋值0，此时不能crash，但要给出告警信息*/

/*遇到一个重大bug，inode->mapping->rh_reserved1被释放后又被新的进程分配而导致mapping->rh_reserved1不是0。这就导致!!!!!!!!!!!!!!!!!!!
 *p_file_stat_base != (p_file_stat_base)->mapping->rh_reserved1成立，但是因为inode又被新的进程分配了而mapping->rh_reserved1是新的file_stat指针，
 *于是这里crash。因此要替换成file_stat_in_delete_base(p_file_stat_base)是否成立，这个file_stat的in_delete标记是我的代码控制*/
#if 0
#define is_file_stat_mapping_error(p_file_stat_base) \
{ \
	if((unsigned long)p_file_stat_base != (p_file_stat_base)->mapping->rh_reserved1){  \
		if(0 == (p_file_stat_base)->mapping->rh_reserved1)\
	        printk(KERN_EMERG"%s file_stat:0x%llx status:0x%lx mapping:0x%llx delete!!!!!!!!!!!!\n",__func__,(u64)p_file_stat_base,(p_file_stat_base)->file_stat_status,(u64)((p_file_stat_base)->mapping)); \
		else \
	        panic("%s file_stat:0x%llx match mapping:0x%llx 0x%llx error\n",__func__,(u64)p_file_stat_base,(u64)((p_file_stat_base)->mapping),(u64)((p_file_stat_base)->mapping->rh_reserved1)); \
	}\
}
#else
/* 
 *  当前有问题的方案
 *  iput()->destroy_inode()，释放inode，标记file_stat in_delete
 *  {
 *	   //p_file_stat_base->mapping = NULL; 这是先注释掉，异步内存回收线程再把它设置NULL
 *     p_file_stat_base->mapping->rh_reserved1 = 0；
 *     smp_wmb();
 *     set_file_stat_in_delete(p_file_stat_base)
 *     file_stat_delete_protect_lock(){
 *         //加锁成功才会把file_stat移动到global delete链表
 *         list_move(file_stat,global_delete_list)
 *     }
 *  }
 *
 * 在异步内存回收线程遍历global temp、small、tiny small链表上的file_stat时，首先执行is_file_stat_mapping_error()判断p_file_stat_base
 * 跟p_file_stat_base->mapping->rh_reserved1是否相等，如果不相等则crash。除非p_file_stat_base->mapping->rh_reserved1是0，
 * 因为p_file_stat_base->mapping->rh_reserved1是0，说明该file_stat在iput()被释放而标记0。
 *
 *is_file_stat_mapping_error(p_file_stat_base)
 *{
 *	if((unsigned long)p_file_stat_base != p_file_stat_base->mapping->rh_reserved1){
 *		if(0 == (p_file_stat_base)->mapping->rh_reserved1)
 *	        printk(""); 
 *		else 
 *	        panic(); 
 *	}
 *}
 * 
 * 这个方案看着貌似没问题，但是有隐患
 *
 * bug：如果iput()->destroy_inode() 释放inode时，可能因file_stat_delete_protect_lock()加锁失败而只是标记file_stat1的in_delete
 * 标记，且p_file_stat1_base->mapping->rh_reserved1赋值0，而没有把file_stat1移动到global delete链表。等异步内存回收线程后来
 * 遍历global temp、small、tiny small链表上的file_stat时，遍历到这个已经标记in_delete的file_stat1。
 * p_file_stat_base1->mapping->rh_reserved1可能不再是0了。因为p_file_stat1_base->mapping指向的inode被iput()释放后，然后
 * 这个inode又被新的进程、新的文件分配，mapping包含在inode结构体里。于是mapping->rh_reserved1=新的文件的file_stat2。
 * 于是执行到is_file_stat_mapping_error()判断file_stat1是否合法时，if(0 == p_file_stat_base->mapping->rh_reserved1)就是
 * 本质就是 if(0 != p_file_stat2_base)，p_file_stat_base->mapping->rh_reserved1指向的是file_stat2了，这样会crash。
 *
 * 于是，如果出现p_file_stat_base跟 (p_file_stat_base)->mapping->rh_reserved1 不一致的情况，说明这个file_stat被iput()释放了。
 * 不能if(0 == p_file_stat_base->mapping->rh_reserved1)判断file_stat是否被delete了。而是要判断file_stat是否有in_delete标记，
 * 如果有in_delete标记，就不在触发crash。
 *
 * 想了几个解决方案
 *
 * 方案1：这个是曾经想过的一个失败的方案，但是很有意义，因为这个错误很容易犯
 *
 * iput()->destroy_inode()里释放inode，标记file_stat in_delete
 * {
	   p_rh_reserved1 = &p_file_stat_base->mapping->rh_reserved1;
 *     p_file_stat_base->mapping = NULL;
 *     smp_wmb();
 *     p_file_stat_base->mapping->rh_reserved1 = 0; (实际代码是*p_rh_reserved1 = 0，这里为了演示方便)
 *     set_file_stat_in_delete(p_file_stat_base)
 * }
 * 
 * 异步内存回收线程遍历global temp、small、tiny small链表上的file_stat时，is_file_stat_mapping_error()里执行
 * is_file_stat_mapping_error(p_file_stat_base)
 *  {
 *     smp_rmb();
 *     if(p_file_stat_base->mapping){
 *         if((unsigned long)p_file_stat_base != p_file_stat_base->mapping->rh_reserved1){
 *             smp_rmb();
 *             if(NULL == p_file_stat_base->mapping)
 *                 printk("file_stat delete");
 *             else 
 *                 panic();
 *         }
 *     }
 * }
 * 如果if((unsigned long)p_file_stat_base != p_file_stat_base->mapping->rh_reserved1)成立，再判断
 * p_file_stat_base->mapping是NUll,说明file_stat被释放了，就不再crash。貌似方案没事，但有大问题
 *
 * 这个设计会因p_file_stat_base->mapping是NULL而crash。因为 if(p_file_stat_base->mapping)成立后，
 * 此时异步内存回收线程执行if((unsigned long)p_file_stat_base != p_file_stat_base->mapping->rh_reserved1)这行代码时，
 * 该文件file_stat被iput()->destroy_inode()并发释放了，于是此时p_file_stat_base->mapping 赋值0。
 * 于是异步内存回收线程执行is_file_stat_mapping_error()里的if((unsigned long)p_file_stat_base != p_file_stat_base->mapping->rh_reserved1)
 * 因p_file_stat_base->mapping是NULL而crash。
 *
 * 这个问题如果iput()->destroy_inode()和is_file_stat_mapping_error()都spin_lock加锁防护这个并发问题能很容易解决。
 * 但是我不想用spin_lock锁，真的就无法靠内存屏障、rcu实现无锁编程吗？
 *
 * 苦想，终于想到了，
 *
 * iput()->destroy_inode()这样设计
 * {
 *     //p_file_stat_base->mapping = NULL;这个赋值去掉，不在iput()时标记p_file_stat_base->mapping为NULL
 *
 *      set_file_stat_in_delete(p_file_stat_base)
 *      smp_wmb(); //保证file_stat的In_delete标记先于p_file_stat_base->mapping->rh_reserved1赋值0生效
 *      p_file_stat_base->mapping->rh_reserved1 = 0;
 *  }
 *
 *  is_file_stat_mapping_error()里这样设计
 *  {
 *      if((unsigned long)p_file_stat_base != p_file_stat_base->mapping->rh_reserved1)
 *      {
 *           smp_rmb();
 *           if(file_stat_in_delete_base(p_file_stat_base))
 *               printk("file_stat delete")
 *           else 
 *              panic();
 *      }
 *  }
 *  如果is_file_stat_mapping_error()里发现p_file_stat_base 和 p_file_stat_base->mapping->rh_reserved1不相等，
 *  说明该文件被iput()->destroy_inode()释放了，此时smp_rmb()后，if(file_stat_in_delete_base(p_file_stat_base))
 *  file_stat一定有in_delete标记，此时只是printk打印，不会panic。该方案iput()->destroy_inode()中不再
 *  p_file_stat_base->mapping = NULL赋值  。故is_file_stat_mapping_error()不用担心它被并发赋值NULL。
 *  并且，iput()->destroy_inode()中的设计，保证file_stat的in_delete标记先于p_file_stat_base->mapping->rh_reserved1赋值0生效.
 *  is_file_stat_mapping_error()中看到if((unsigned long)p_file_stat_base != p_file_stat_base->mapping->rh_reserved1)
	不相等，此时一定file_stat一定有in_delete标记，故if(file_stat_in_delete_base(p_file_stat_base))一定成立，就不会crash
 *  
 *  这是个很完美的无锁并发设计模型，要充分吸取这个无锁编程的思想。
 *  
 *  最后，还有一个很重要的地方，如果 is_file_stat_mapping_error()里使用 p_file_stat_base->mapping->rh_reserved1是，
 *  该文件inode被iput()并发释放了，p_file_stat_base->mapping就是无效内存访问了。要防护这种情况，于是is_file_stat_mapping_error()
 *  里还要加上rcu_read_lock()
 *
 * 1:rcu_read_lock()防止inode被iput()释放了，导致 p_file_stat_base->mapping->rh_reserved1无效内存访问。
 * 2:rcu_read_lock()加printk打印会导致休眠吧，这点要控制
 * 3:smp_rmb()保证p_file_stat_base->mapping->rh_reserved1在iput()被赋值0后，file_stat一定有delete标记。iput()里是set_file_stat_in_delete;smp_wmb;p_file_stat_base->mapping->rh_reserved1=0
 * */
#define is_file_stat_mapping_error(p_file_stat_base) \
{ \
	rcu_read_lock();\
	if((unsigned long)p_file_stat_base != (p_file_stat_base)->mapping->rh_reserved1){  \
		smp_rmb();\
		if(file_stat_in_delete_base(p_file_stat_base)){\
			rcu_read_unlock(); \
			printk(KERN_WARNING "%s file_stat:0x%llx status:0x%x mapping:0x%llx delete!!!!!!!!!!!!\n",__func__,(u64)p_file_stat_base,(p_file_stat_base)->file_stat_status,(u64)((p_file_stat_base)->mapping)); \
			goto out;\
		} \
		else \
		panic("%s file_stat:0x%llx match mapping:0x%llx 0x%llx error\n",__func__,(u64)p_file_stat_base,(u64)((p_file_stat_base)->mapping),(u64)((p_file_stat_base)->mapping->rh_reserved1)); \
	}\
	rcu_read_unlock();\
	out:	\
}
#endif

static inline struct file_stat_base *file_stat_alloc_and_init(struct address_space *mapping,unsigned int file_type,char free_old_file_stat)
{
	struct file_stat * p_file_stat = NULL;
	struct file_stat_small *p_file_stat_small = NULL;
	struct file_stat_tiny_small *p_file_stat_tiny_small = NULL;
	struct file_stat_base *p_file_stat_base = NULL;

	/*这里有个问题，hot_cold_file_global_info.global_lock有个全局大锁，每个进程执行到这里就会获取到。合理的是
	  应该用每个文件自己的spin lock锁!比如file_stat里的spin lock锁，但是在这里，每个文件的file_stat结构还没分配!!!!!!!!!!!!*/
	spin_lock(&hot_cold_file_global_info.global_lock);
	//如果两个进程同时访问一个文件，同时执行到这里，需要加锁。第1个进程加锁成功后，分配file_stat并赋值给
	//mapping->rh_reserved1，第2个进程获取锁后执行到这里mapping->rh_reserved1就会成立
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping) && !free_old_file_stat){
		printk("%s file_stat:0x%llx already alloc\n",__func__,(u64)mapping->rh_reserved1);
		//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
		p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
		goto out;
	}

	if(FILE_STAT_TINY_SMALL == file_type){
		//新的文件分配file_stat,一个文件一个，保存文件热点区域访问数据
		p_file_stat_tiny_small = kmem_cache_alloc(hot_cold_file_global_info.file_stat_tiny_small_cachep,GFP_ATOMIC);
		if (!p_file_stat_tiny_small) {
			printk("%s file_stat alloc fail\n",__func__);
			goto out;
		}
		//file_stat个数加1
		hot_cold_file_global_info.file_stat_tiny_small_count ++;
		memset(p_file_stat_tiny_small,0,sizeof(struct file_stat_tiny_small));
		p_file_stat_base = &p_file_stat_tiny_small->file_stat_base;
		//设置文件是cache文件状态，有些cache文件可能还会被mmap映射，要与mmap文件互斥，要么是cache文件要么是mmap文件，不能两者都是 
		set_file_stat_in_cache_file_base(p_file_stat_base);
		//初始化file_area_hot头结点
		INIT_LIST_HEAD(&p_file_stat_base->file_area_temp);

		//mapping->file_stat记录该文件绑定的file_stat结构的file_stat_base的地址，将来判定是否对该文件分配了file_stat
		mapping->rh_reserved1 = (unsigned long)(p_file_stat_base);
		//file_stat记录mapping结构
		p_file_stat_base->mapping = mapping;

		//设置file_stat in_temp_list最好放到把file_stat添加到global temp链表操作前，原因在add_mmap_file_stat_to_list()有分析
		set_file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base);
		smp_wmb();
		//把针对该文件分配的file_stat结构添加到hot_cold_file_global_info的file_stat_temp_head链表
		list_add(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_tiny_small_file_head);
	    spin_lock_init(&p_file_stat_base->file_stat_lock);

	}
	else if(FILE_STAT_SMALL == file_type){
		//新的文件分配file_stat,一个文件一个，保存文件热点区域访问数据
		p_file_stat_small = kmem_cache_alloc(hot_cold_file_global_info.file_stat_small_cachep,GFP_ATOMIC);
		if (!p_file_stat_small) {
			printk("%s file_stat alloc fail\n",__func__);
			goto out;
		}
		//file_stat个数加1
		hot_cold_file_global_info.file_stat_small_count ++;
		memset(p_file_stat_small,0,sizeof(struct file_stat_small));
		p_file_stat_base = &p_file_stat_small->file_stat_base;
		//设置文件是cache文件状态，有些cache文件可能还会被mmap映射，要与mmap文件互斥，要么是cache文件要么是mmap文件，不能两者都是 
		set_file_stat_in_cache_file_base(p_file_stat_base);
		//初始化file_area_hot头结点
		INIT_LIST_HEAD(&p_file_stat_base->file_area_temp);
		INIT_LIST_HEAD(&p_file_stat_small->file_area_other);

		//mapping->file_stat记录该文件绑定的file_stat结构，将来判定是否对该文件分配了file_stat
		mapping->rh_reserved1 = (unsigned long)(p_file_stat_base);
		//file_stat记录mapping结构
		p_file_stat_base->mapping = mapping;

		//设置file_stat in_temp_list最好放到把file_stat添加到global temp链表操作前，原因在add_mmap_file_stat_to_list()有分析
		set_file_stat_in_file_stat_small_file_head_list_base(p_file_stat_base);
		smp_wmb();
		//把针对该文件分配的file_stat结构添加到hot_cold_file_global_info的file_stat_temp_head链表
		list_add(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_small_file_head);
	    spin_lock_init(&p_file_stat_base->file_stat_lock);

	}
	/*如果是小文件使用精简的file_stat_small，大文件才使用file_stat结构，为了降低内存消耗*/
	else if(FILE_STAT_NORMAL == file_type){
		//新的文件分配file_stat,一个文件一个，保存文件热点区域访问数据
		p_file_stat = kmem_cache_alloc(hot_cold_file_global_info.file_stat_cachep,GFP_ATOMIC);
		if (!p_file_stat) {
			printk("%s file_stat alloc fail\n",__func__);
			goto out;
		}
		memset(p_file_stat,0,sizeof(struct file_stat));
		p_file_stat_base = &p_file_stat->file_stat_base;
		//设置文件是cache文件状态，有些cache文件可能还会被mmap映射，要与mmap文件互斥，要么是cache文件要么是mmap文件，不能两者都是 
		set_file_stat_in_cache_file_base(p_file_stat_base);
		//初始化file_area_hot头结点
		INIT_LIST_HEAD(&p_file_stat->file_area_hot);
		INIT_LIST_HEAD(&p_file_stat_base->file_area_temp);
		INIT_LIST_HEAD(&p_file_stat->file_area_warm);
		//INIT_LIST_HEAD(&p_file_stat->file_area_free_temp);
		INIT_LIST_HEAD(&p_file_stat->file_area_free);
		INIT_LIST_HEAD(&p_file_stat->file_area_refault);
		INIT_LIST_HEAD(&p_file_stat->file_area_mapcount);

		//mapping->file_stat记录该文件绑定的file_stat结构，将来判定是否对该文件分配了file_stat
		mapping->rh_reserved1 = (unsigned long)(p_file_stat_base);
		//file_stat记录mapping结构
		p_file_stat_base->mapping = mapping;

		//设置file_stat in_temp_list最好放到把file_stat添加到global temp链表操作前，原因在add_mmap_file_stat_to_list()有分析
		set_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
		smp_wmb();
		//把针对该文件分配的file_stat结构添加到hot_cold_file_global_info的file_stat_temp_head链表
		list_add(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_temp_head);
	    spin_lock_init(&p_file_stat_base->file_stat_lock);

	}else
		BUG();

	//file_stat个数加1
	hot_cold_file_global_info.file_stat_count ++;
	//新分配的file_stat必须设置in_file_stat_temp_head_list链表
	//set_file_stat_in_file_stat_temp_head_list(p_file_stat);
out:	
	spin_unlock(&hot_cold_file_global_info.global_lock);

	return p_file_stat_base;
}
/*mmap文件跟cache文件的file_stat都保存在mapping->rh_reserved1，这样会不会有冲突?并且，主要有如下几点
 * 1：cache文件分配file_stat并保存到mapping->rh_reserved1是file_stat_alloc_and_init()函数，mmap文件分配file_stat并
 * 添添加到mapping->rh_reserved1是add_mmap_file_stat_to_list()。二者第一次执行时，都是该文件被读写，第一次分配page
 * 然后执行__filemap_add_folio_for_file_area()把page添加到xarray tree。这个过程需要防止并发，对mapping->rh_reserved1同时赋值
 * 这点，在__filemap_add_folio_for_file_area()开头有详细注释
 * 2:cache文件和mmap文件一个用的global file_global_lock，一个是global mmap_file_global_lock锁。分开使用，否则这个
 * 全局锁同时被多个进程抢占，阻塞时间会很长，把大锁分成小锁。但是分开用，就无法防止cache文件和mmap的并发!!!
 * 3：最重要的，一个文件，即有mmap映射读写、又有cache读写，怎么判断冷热和内存回收？mapping->rh_reserved1代表的file_stat
 * 是代表cache文件还是mmap文件？按照先到先得处理：
 *
 * 如果__filemap_add_folio_for_file_area()中添加该文件的第一个page到xarray tree，
 * 分配file_stat时，该文件已经建立了mmap映射，即mapping->i_mmap非NULL，则该文件就是mmap文件，然后执行add_mmap_file_stat_to_list()
 * 分配的file_stat添加global mmap_file_stat_uninit_head链表。后续，如果该文件被cache读写(read/write系统调用读写)，执行到
 * hot_file_update_file_status()函数时，只更新file_area的age，立即返回，不能再把file_area启动到file_stat->hot、refault等链表。
 * mmap文件的file_area是否移动到file_stat->hot、refault等链表，在check_file_area_cold_page_and_clear()中进行。其实，这种
 * 情况下，这些file_area在 hot_file_update_file_status()中把file_area启动到file_stat->hot、refault等链表，似乎也可以????????????
 *
 * 相反，如果__filemap_add_folio_for_file_area()中添加该文件的第一个page到xarray tree，该文件没有mmap映射，则判定为cache文件。
 * 如果后续该文件又mmap映射了，依然判定为cache文件，否则关系会错乱。但不用担心回收内存有问题，因为cache文件内存回收会跳过mmap
 * 的文件页。
 * */
static inline struct file_stat_base *add_mmap_file_stat_to_list(struct address_space *mapping,unsigned int file_type,char free_old_file_stat)
{
	struct file_stat *p_file_stat = NULL;
	struct file_stat_small *p_file_stat_small = NULL;
	struct file_stat_tiny_small *p_file_stat_tiny_small = NULL;
	struct file_stat_base *p_file_stat_base = NULL;

	spin_lock(&hot_cold_file_global_info.mmap_file_global_lock);
	/*1:如果两个进程同时访问一个文件，同时执行到这里，需要加锁。第1个进程加锁成功后，分配file_stat并赋值给
	 *mapping->rh_reserved1，第2个进程获取锁后执行到这里mapping->rh_reserved1就会成立
	 *2:异步内存回收功能禁止了
	 *3:当small file_stat转到normal file_stat，释放老的small file_stat然后分配新的normal file_stat，此时
	 *free_old_file_stat 是1，下边的if不成立，忽略mapping->rh_reserved1，进而才不会goto out，而是分配新的file_stat
	 */
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping) && !free_old_file_stat){
		//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
		p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
		spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
		printk("%s file_stat:0x%llx already alloc\n",__func__,(u64)mapping->rh_reserved1);
		goto out;  
	}
	/*如果是小文件使用精简的file_stat_small，大文件才使用file_stat结构，为了降低内存消耗*/
	if(FILE_STAT_TINY_SMALL == file_type){
		p_file_stat_tiny_small = kmem_cache_alloc(hot_cold_file_global_info.file_stat_tiny_small_cachep,GFP_ATOMIC);
		if (!p_file_stat_tiny_small) {
			spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
			printk("%s file_stat alloc fail\n",__func__);
			goto out;
		}
		//设置file_stat的in mmap文件状态
		hot_cold_file_global_info.mmap_file_stat_tiny_small_count++;
		memset(p_file_stat_tiny_small,0,sizeof(struct file_stat_tiny_small));
		p_file_stat_base = &p_file_stat_tiny_small->file_stat_base;
		//mapping->file_stat记录该文件绑定的file_stat结构，将来判定是否对该文件分配了file_stat
		//这里得把file_stat_base赋值给mapping->rh_reserved1，不再是整个file_stat结构体????????????????????????
		mapping->rh_reserved1 = (unsigned long)(p_file_stat_base);
		p_file_stat_base->mapping = mapping;
		//设置文件是mmap文件状态，有些mmap文件可能还会被读写，要与cache文件互斥，要么是cache文件要么是mmap文件，不能两者都是 
		set_file_stat_in_mmap_file_base(p_file_stat_base);
		INIT_LIST_HEAD(&p_file_stat_base->file_area_temp);

		set_file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base);
		list_add(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_tiny_small_file_head);
		spin_lock_init(&p_file_stat_base->file_stat_lock);

	}
	else if(FILE_STAT_SMALL == file_type){
		p_file_stat_small = kmem_cache_alloc(hot_cold_file_global_info.file_stat_small_cachep,GFP_ATOMIC);
		if (!p_file_stat_small) {
			spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
			printk("%s file_stat alloc fail\n",__func__);
			goto out;
		}
		//设置file_stat的in mmap文件状态
		hot_cold_file_global_info.mmap_file_stat_small_count++;
		memset(p_file_stat_small,0,sizeof(struct file_stat_small));
		p_file_stat_base = &p_file_stat_small->file_stat_base;
		//mapping->file_stat记录该文件绑定的file_stat结构，将来判定是否对该文件分配了file_stat
		mapping->rh_reserved1 = (unsigned long)(p_file_stat_base);
		p_file_stat_base->mapping = mapping;
		//设置文件是mmap文件状态，有些mmap文件可能还会被读写，要与cache文件互斥，要么是cache文件要么是mmap文件，不能两者都是 
		set_file_stat_in_mmap_file_base(p_file_stat_base);
		INIT_LIST_HEAD(&p_file_stat_base->file_area_temp);
		INIT_LIST_HEAD(&p_file_stat_small->file_area_other);

		set_file_stat_in_file_stat_small_file_head_list_base(p_file_stat_base);
		list_add(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_small_file_head);
		spin_lock_init(&p_file_stat_base->file_stat_lock);

	}
	else if(FILE_STAT_NORMAL == file_type){
		p_file_stat = kmem_cache_alloc(hot_cold_file_global_info.file_stat_cachep,GFP_ATOMIC);
		if (!p_file_stat) {
			spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
			printk("%s file_stat alloc fail\n",__func__);
			goto out;
		}
		memset(p_file_stat,0,sizeof(struct file_stat));
		p_file_stat_base = &p_file_stat->file_stat_base;
		//设置文件是mmap文件状态，有些mmap文件可能还会被读写，要与cache文件互斥，要么是cache文件要么是mmap文件，不能两者都是 
		set_file_stat_in_mmap_file_base(p_file_stat_base);
		INIT_LIST_HEAD(&p_file_stat->file_area_hot);
		INIT_LIST_HEAD(&p_file_stat_base->file_area_temp);
		INIT_LIST_HEAD(&p_file_stat->file_area_warm);
		/*mmap文件需要p_file_stat->file_area_free_temp暂存参与内存回收的file_area，不能注释掉*/
		//INIT_LIST_HEAD(&p_file_stat->file_area_free_temp);
		INIT_LIST_HEAD(&p_file_stat->file_area_free);
		INIT_LIST_HEAD(&p_file_stat->file_area_refault);
		//file_area对应的page的pagecount大于0的，则把file_area移动到该链表
		INIT_LIST_HEAD(&p_file_stat->file_area_mapcount);

		//mapping->file_stat记录该文件绑定的file_stat结构，将来判定是否对该文件分配了file_stat
		mapping->rh_reserved1 = (unsigned long)(p_file_stat_base);
		p_file_stat_base->mapping = mapping;
#if 1
		/*新分配的file_stat必须设置in_file_stat_temp_head_list链表。这个设置file_stat状态的操作必须放到 把file_stat添加到
		 *tmep链表前边，还要加内存屏障。否则会出现一种极端情况，异步内存回收线程从temp链表遍历到这个file_stat，
		 *但是file_stat还没有设置为in_temp_list状态。这样有问题会触发panic。因为mmap文件异步内存回收线程，
		 *从temp链表遍历file_stat没有mmap_file_global_lock加锁，所以与这里存在并发操作。而针对cache文件，异步内存回收线程
		 *从global temp链表遍历file_stat，全程global_lock加锁，不会跟向global temp链表添加file_stat存在方法，但最好改造一下*/
		set_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
		smp_wmb();
#endif	
		spin_lock_init(&p_file_stat_base->file_stat_lock);
		list_add(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_temp_head);

	}else
		BUG();


	//设置file_stat的in mmap文件状态
	hot_cold_file_global_info.mmap_file_stat_count++;
	spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
	if(shrink_page_printk_open)
		printk("%s file_stat:0x%llx\n",__func__,(u64)p_file_stat_base);

out:
	return p_file_stat_base;
}
static inline struct file_area *file_area_alloc_and_init(unsigned int area_index_for_page,struct file_stat_base *p_file_stat_base)
{
	struct file_area *p_file_area = NULL;

	spin_lock(&p_file_stat_base->file_stat_lock);
#if 0	
	/* 如果file_stat是delete的，此时有两种情况，文件被iput()标记了delete，不可能。还有一种情况就是small文件转换成normal文件 
	 * 或者 tiny small文件转成成small文件，这个老的small或者tiny small file_stat被标记了。则从mapping->rh_reserved1获取新的
	 * file_stat。详细注释见can_tiny_small_file_change_to_small_normal_file()*/
	if(file_stat_in_replace_file_base(p_file_stat_base)){----------执行到这里时，file_stat可能被异步内存回收线程标记delete或者replace，故不能触发panic
	    panic("%s file_stat:0x%llx error\n",__func__,(u64)p_file_stat_base); \
	}
#endif	
	/*到这里，针对当前page索引的file_area结构还没有分配,page_slot_in_tree是槽位地址，*page_slot_in_tree是槽位里的数据，就是file_area指针，
	  但是NULL，于是针对本次page索引，分配file_area结构*/
	p_file_area = kmem_cache_alloc(hot_cold_file_global_info.file_area_cachep,GFP_ATOMIC);
	if (!p_file_area) {
		//spin_unlock(&p_file_stat->file_stat_lock);
		printk("%s file_area alloc fail\n",__func__);
		goto out;
	}
	memset(p_file_area,0,sizeof(struct file_area));
	/* 新分配的file_area必须添加到file_stat->temp链表头，对于tiny small文件来说，保证in_refault、in_free、in_hot
	 * 的file_area一定聚聚在file_stat_tiny_small->temp链表尾，将来tiny small转换成small文件或者normal文件，
	 * 只用从file_stat_tiny_small->temp链表尾就能获取到in_refault、in_free、in_hot的file_area，然后移动到新的file_stat的对应链表*/
	list_add(&p_file_area->file_area_list,&p_file_stat_base->file_area_temp);
#ifndef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY	
	//保存该file_area对应的起始page索引，一个file_area默认包含8个索引挨着依次增大page，start_index保存其中第一个page的索引
	p_file_area->start_index = area_index_for_page << PAGE_COUNT_IN_AREA_SHIFT;//area_index_for_page * PAGE_COUNT_IN_AREA;
#endif	
	p_file_stat_base->file_area_count ++;//文件file_stat的file_area个数加1
	set_file_area_in_temp_list(p_file_area);//新分配的file_area必须设置in_temp_list链表

	//在file_stat->file_area_temp链表的file_area个数加1
	p_file_stat_base->file_area_count_in_temp_list ++;

out:
	spin_unlock(&p_file_stat_base->file_stat_lock);

	return p_file_area;
}
/*令inode引用计数减1，如果inode引用计数是0则释放inode结构*/
static void inline file_inode_unlock(struct file_stat_base * p_file_stat_base)
{
    struct inode *inode = p_file_stat_base->mapping->host;
    //令inode引用计数减1，如果inode引用计数是0则释放inode结构
	iput(inode);
}
static void inline file_inode_unlock_mapping(struct address_space *mapping)
{
    struct inode *inode = mapping->host;
    //令inode引用计数减1，如果inode引用计数是0则释放inode结构
	iput(inode);
}

/*对文件inode加锁，如果inode已经处于释放状态则返回0，此时不能再遍历该文件的inode的address_space的radix tree获取page，释放page，
 *此时inode已经要释放了，inode、address_space、radix tree都是无效内存。否则，令inode引用计数加1，然后其他进程就无法再释放这个
 *文件的inode，此时返回1*/
static int inline file_inode_lock(struct file_stat_base *p_file_stat_base)
{
    /*不能在这里赋值，因为可能文件inode被iput后p_file_stat->mapping赋值NULL，这样会crash*/
	//struct inode *inode = p_file_stat->mapping->host;
	struct inode *inode;
	int lock_fail = 0;

	/*这里有个隐藏很深的bug!!!!!!!!!!!!!!!!如果此时其他进程并发执行iput()最后执行到__destroy_inode_handler_post()触发删除inode，
	 *然后就会立即把inode结构释放掉。此时当前进程可能执行到file_inode_lock()函数的spin_lock(&inode->i_lock)时，但inode已经被释放了，
	 则会访问已释放的inode的mapping的xarray 而crash。怎么防止这种并发？*/
    
	/*最初方案：当前函数执行lock_file_stat()对file_stat加锁。在__destroy_inode_handler_post()中也会lock_file_stat()加锁。防止
	 * __destroy_inode_handler_post()中把inode释放了，而当前函数还在遍历该文件inode的mapping的xarray tree
	 * 查询page，访问已经释放的内存而crash。这个方案太麻烦!!!!!!!!!!!!!!，现在的方案是使用rcu，这里
	 * rcu_read_lock()和__destroy_inode_handler_post()中标记inode delete形成并发。极端情况是，二者同时执行，
	 * 但这里rcu_read_lock后，进入rcu宽限期。而__destroy_inode_handler_post()执行后，触发释放inode，然后执行到destroy_inode()里的
	 * call_rcu(&inode->i_rcu, i_callback)后，无法真正释放掉inode结构。当前函数可以放心使用inode、mapping、xarray tree。
	 * 但有一点需注意，rcu_read_lock后不能休眠，否则rcu宽限期会无限延长。*/

	//lock_file_stat(p_file_stat,0);
	rcu_read_lock();
	smp_rmb();
	if(file_stat_in_delete_base(p_file_stat_base) || (NULL == p_file_stat_base->mapping)){
		//不要忘了异常return要先释放锁
		rcu_read_unlock();
		return 0;
	}
	inode = p_file_stat_base->mapping->host;

	spin_lock(&inode->i_lock);
	/*执行到这里，inode肯定没有被释放，并且inode->i_lock加锁成功，其他进程就无法再释放这个inode了。错了，又一个隐藏很深的bug。
	 *!!!!!!!!!!!!!!!!因为其他进程此时可能正在iput()->__destroy_inode_handler_post()中触发释放inode。这里rcu_read_unlock后，
	 *inode就会立即被释放掉，然后下边再使用inode就会访问无效inode结构而crash。rcu_read_unlock要放到对inode引用计数加1后*/

	//unlock_file_stat(p_file_stat);
	//rcu_read_unlock();

	/*inode正被其他进程iput释放，加锁失败*/
	if(inode->i_state & (I_FREEING|I_WILL_FREE|I_NEW)){
		lock_fail = 1;
	}
	/*如果inode引用计数是0了，说明没人再用，加锁失败。并且iput()强制触发释放掉该inode，否则会成为只有一个文件页的file_stat，
	 *但是又因加锁失败而无法回收，对内存回收干扰。但iput要放到spin_unlock(&inode->i_lock)后*/
	else if(atomic_read(&inode->i_count) == 0){
		if(!hlist_empty(&inode->i_dentry)){
			struct dentry *dentry = hlist_entry(inode->i_dentry.first, struct dentry, d_u.d_alias);
			if(dentry)
				printk("%s file_stat:0x%llx inode:0x%llx dentry:0x%llx %s icount0!!!!!!!\n",__func__,(u64)p_file_stat_base,(u64)inode,(u64)dentry,dentry->d_name.name);
			else 
				printk("%s file_stat:0x%llx inode:0x%llx dentry:0x%llx icount0!!!!!!!\n",__func__,(u64)p_file_stat_base,(u64)inode,(u64)dentry);
		}else
			printk("%s file_stat:0x%llx inode:0x%llx icount0!!!!!!! i_nlink:%d nrpages:%ld\n",__func__,(u64)p_file_stat_base,(u64)inode,inode->i_nlink,inode->i_mapping->nrpages);
		//iput(inode);

		//lock_fail = 2;引用计数是0是正常现象，此时也能加锁成功，只要保证inode此时不是已经释放的状态
	}

	//加锁成功则令inode引用计数加1，之后就不用担心inode被其他进程释放掉
	if(0 == lock_fail)
		atomic_inc(&inode->i_count);

	spin_unlock(&inode->i_lock);
	rcu_read_unlock();

	/* 这里强制令inode引用计数减1，会导致iput引用计数异常减1，导致删除文件时ihold()中发现inode引用计数少了1而触发warn。
	 * 还推测可能会inode引用引用计数少了1而被提前iput释放，而此时还有进程在使用这个已经释放的文件inode，就是访问非法内存了*/
#if 0	
	if(2 == lock_fail)
		iput(inode);
#endif

	return (0 == lock_fail);
}
static void inline file_area_access_count_clear(struct file_area *p_file_area)
{
	atomic_set(&p_file_area->access_count,0);
}
static void inline file_area_access_count_add(struct file_area *p_file_area,int count)
{
	atomic_add(count,&p_file_area->access_count);
}
static int inline file_area_access_count_get(struct file_area *p_file_area)
{
	return atomic_read(&p_file_area->access_count);
}
/*head代表一段链表，first~tail是这个链表尾的几个连续成员，该函数是把first~tail指向的几个成员移动到链表头*/
//void list_move_enhance(struct list_head *head,struct list_head *first,struct list_head *tail)
static void inline list_move_enhance(struct list_head *head,struct list_head *first)
{
	/*链表不能空*/
	if(!list_empty(head)){
		/*指向链表最后一个成员*/
		struct list_head *tail = head->prev;

		/*1:first不能指向链表头 2:first不能是链表的第一个成员 3:tail必须是链表尾的成员*/
		if(first != head && head->next != first && list_is_last(tail,head)){
			/*first的上一个链表成员*/
			struct list_head *new_tail = first->prev;
			/*链表的第一个成员*/
			struct list_head *old_head = head->next;

			/*head<-->old_head<-->new_tail<-->first<-->tail -----> head<-->first <-->tail<-->old_head<-->new_tail
			 *
			 *head<-->old_head(new_tail)<-->first(tail) -----> head<-->first(tail)<-->old_head(new_tail)
			 */
			head->next = first;
			head->prev = new_tail;

			first->prev = head;
			tail->next  = old_head;

			old_head->prev = tail;
			new_tail->next = head;
		}else
			printk("%ps->list_move_enhance() head:0x%llx first:0x%llx head->next:0x%llx %d\n",__builtin_return_address(0),(u64)head,(u64)first,(u64)head->next,list_is_last(tail,head));
	}
}
/*测试file_area是否真的在file_area_in_list_type这个file_stat的链表(file_stat->temp、hot、refault、warm、mapcount链表)，不在则不能把p_file_area从链表尾的file_area移动到链表头*/
static int inline can_file_area_move_to_list_head(struct file_area *p_file_area,struct list_head *file_area_list_head,unsigned int file_area_in_list_type)
{
	/*file_area不能是链表头*/
	if(&p_file_area->file_area_list == file_area_list_head)
		return 0;
	/*如果file_area不在file_area_in_list_type这个file_stat的链表上，测试失败*/
    if(0 == (p_file_area->file_area_state & (1 << file_area_in_list_type)))
		return 0;
    /*如果file_area检测到在其他file_stat链表上，测试失败*/
	if(p_file_area->file_area_state & (~(1 << file_area_in_list_type) & FILE_AREA_LIST_MASK))
		return 0;

	printk("can_file_area_move_to_list_head file_area:0x%llx\n",(u64)p_file_area);
	return 1;
}
/*测试file_stat是否真的在file_stat_in_list_type这个global链表上(global temp、middle、large、hot、mapcount链表)，不在则不能把p_file_stat到链表尾的file_stat移动到链表头*/
static int inline can_file_stat_move_to_list_head(struct file_stat_base *p_file_stat_base,unsigned int file_stat_in_list_type)
{
	/*如果file_stat不在file_stat_in_list_type这个global链表上，测试失败*/
    if(0 == (p_file_stat_base->file_stat_status & (1 << file_stat_in_list_type)))
		return 0;
    /*如果file_stat检测到在file_stat_in_list_type除外的其他global链表上，测试失败*/
	if(p_file_stat_base->file_stat_status & (~(1 << file_stat_in_list_type) & FILE_STAT_LIST_MASK))
		return 0;

	printk("can_file_stat_move_to_list_head file_stat:0x%llx\n",(u64)p_file_stat_base);
	return 1;
}
static int inline can_file_stat_move_to_list_head_base(struct file_stat_base *p_file_stat_base,unsigned int file_stat_in_list_type)
{
	/*如果file_stat不在file_stat_in_list_type这个global链表上，测试失败*/
    if(0 == (p_file_stat_base->file_stat_status & (1 << file_stat_in_list_type)))
		return 0;
    /*如果file_stat检测到在file_stat_in_list_type除外的其他global链表上，测试失败*/
	if(p_file_stat_base->file_stat_status & (~(1 << file_stat_in_list_type) & FILE_STAT_LIST_MASK))
		return 0;

	printk("can_file_stat_move_to_list_head file_stat:0x%llx\n",(u64)p_file_stat_base);
	return 1;
}

static void inline i_file_stat_small_callback(struct rcu_head *head)
{
	struct file_stat_base *p_file_stat_base = container_of(head, struct file_stat_base, i_rcu);
	struct file_stat_small *p_file_stat_small = container_of(p_file_stat_base, struct file_stat_small, file_stat_base);

	/*有必要在这里判断file_stat的temp、refault、hot、free、mapcount链表是否空，如果有残留file_area则panic。
	 * 防止can_tiny_small_file_change_to_small_normal_file()把tiny small转换成其他文件时，因代码有问题，导致没处理干净所有的file_area*/
	if(!list_empty(&p_file_stat_small->file_stat_base.file_area_temp) || !list_empty(&p_file_stat_small->file_area_other))
		panic("%s file_stat_small:0x%llx status:0x%llx  list nor empty\n",__func__,(u64)p_file_stat_small,(u64)p_file_stat_small->file_stat_base.file_stat_status);

	kmem_cache_free(hot_cold_file_global_info.file_stat_small_cachep,p_file_stat_small);
}
static void inline i_file_stat_tiny_small_callback(struct rcu_head *head)
{
	struct file_stat_base *p_file_stat_base = container_of(head, struct file_stat_base, i_rcu);
	struct file_stat_tiny_small *p_file_stat_tiny_small = container_of(p_file_stat_base, struct file_stat_tiny_small, file_stat_base);

	/*有必要在这里判断file_stat的temp、refault、hot、free、mapcount链表是否空，如果有残留file_area则panic。
	 * 防止can_small_file_change_to_normal_file()把tiny small转换成其他文件时，因代码有问题，导致没处理干净所有的file_area*/
	if(!list_empty(&p_file_stat_tiny_small->file_stat_base.file_area_temp))
		panic("%s file_stat_small:0x%llx status:0x%llx  list nor empty\n",__func__,(u64)p_file_stat_tiny_small,(u64)p_file_stat_tiny_small->file_stat_base.file_stat_status);

	kmem_cache_free(hot_cold_file_global_info.file_stat_tiny_small_cachep,p_file_stat_tiny_small);
}
#if 0
/*遍历file_stat_tiny_small->temp链表上的file_area，遇到hot、refault的file_area则移动到新的file_stat对应的链表。
 * 注意，执行这个函数前，必须保证没有进程再会访问该file_stat_tiny_small*/
static inline unsigned int move_tiny_small_file_area_to_normal_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_tiny_small *p_file_stat_tiny_small,struct file_stat *p_file_stat,char is_cache_file)
{
	unsigned int scan_file_area_count = 0;
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int file_area_type;

	/*从链表尾开始遍历file_area，有in_refault、in_free、in_hot属性的则移动到新的file_stat对应的链表，最多只遍历640个，即便
	  file_stat_tiny_small->temp链表上可能因短时间大量访问pagecahce而导致有很多的file_area*/
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_tiny_small->file_area_temp,file_area_list){
		if(++ scan_file_area_count > NORMAL_TEMP_FILE_AREA_COUNT_LEVEL)
			break;
		if(!file_area_in_temp_list(p_file_area)){
			/* 其实这里就可以判断这些hot、refault的file_area，如果长时间没访问则移动回file_stat->warm链表，
			 * free的file_area则直接释放掉!!!!!!后续改进。并且，不用加file_stat->file_stat_lock锁。
			 * file_stat_tiny_small已经保证不会再有进程访问，p_file_stat只有操作p_file_stat->temp链表的file_area才用加锁!!!!!!!*/
			file_area_type = get_file_area_list_status(p_file_area);
			/*把老的file_stat的free、refaut、hot属性的file_area移动到新的file_stat对应的file_area链表，这个过程老的
			 *file_stat不用file_stat_lock加锁，因为已经保证没进程再访问它。新的file_stat也不用，因为不是把file_area移动到新的file_stat->temp链表*/
			if(is_cache_file)
				file_stat_other_list_file_area_solve_common(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area,file_area_type,FILE_STAT_NORMAL);
			else/*这个函数mmap的tiny small转换成small或normal文件也会调用，这里正是对mmap文件的移动file_area的处理*/
				reverse_other_file_area_list_common(p_hot_cold_file_global,&p_file_stat_tiny_small->file_stat_base,p_file_area,file_area_type,FILE_STAT_SMALL);
		}
	}
	/*把file_stat_tiny_small->temp链表上的temp属性的file_area移动到新的file_stat的temp链表上。不能用list_splice，
	 * 因为list_splice()移动链表成员后，链表头依然指向这些链表成员，不是空链表，list_splice_init()会把它强制变成空链表*/
	//list_splice(&p_file_stat_tiny_small->file_area_temp,p_file_stat->file_area_temp);
	list_splice_init(&p_file_stat_tiny_small->file_area_temp,p_file_stat->file_area_temp);
	return scan_file_area_count;
}
static inline unsigned int move_tiny_small_file_area_to_small_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_tiny_small *p_file_stat_tiny_small,struct file_stat_small *p_file_stat_small)
{
	unsigned int scan_file_area_count = 0;
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int file_area_type;

	/*从链表尾开始遍历file_area，有in_refault、in_free、in_hot属性的则移动到新的file_stat对应的链表，最多只遍历64个，即便
	  file_stat_tiny_small->temp链表上可能因短时间大量访问pagecahce而导致有很多的file_area*/
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_tiny_small->file_area_temp,file_area_list){
		if(++ scan_file_area_count > SMALL_FILE_AREA_COUNT_LEVEL)
			break;
		if(!file_area_in_temp_list(p_file_area)){
			/* 其实这里就可以判断这些hot、refault的file_area，如果长时间没访问则移动回file_stat->warm链表，
			 * free的file_area则直接释放掉!!!!!!后续改进。并且，不用加file_stat->file_stat_lock锁。
			 * file_stat_tiny_small已经保证不会再有进程访问，p_file_stat只有操作p_file_stat->temp链表的file_area才用加锁!!!!!!!*/
			file_area_type = get_file_area_list_status(p_file_area);
			if(is_cache_file)
				file_stat_other_list_file_area_solve_common(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,p_file_area,file_area_type,FILE_STAT_SMALL);
			else
				reverse_other_file_area_list_common(p_hot_cold_file_global,&p_file_stat_tiny_small->file_stat_base,p_file_area,file_area_type,FILE_STAT_SMALL);
		}
	}
	/*把file_stat_tiny_small->temp链表上的temp属性的file_area移动到新的file_stat的temp链表上*/
	list_splice_init(&p_file_stat_tiny_small->file_area_temp,p_file_stat_small->file_area_temp);
	return scan_file_area_count;
}
static inline unsigned int move_small_file_area_to_normal_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_tiny_small *p_file_stat_tiny_small,struct file_stat *p_file_stat)
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
			if(is_cache_file)
				file_stat_other_list_file_area_solve_common(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area,file_area_type,FILE_STAT_NORMAL);
			else
				reverse_other_file_area_list_common(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,p_file_area,FILE_AREA_REFAULT,FILE_STAT_NORMAL);
		}
		/*防止循环耗时太长而适当调度*/
		cond_resched();
	}
	/*把file_stat_small->temp链表上的temp属性的file_area移动到新的file_stat的temp链表上*/
	list_splice_init(&p_file_stat_small->file_area_temp,p_file_stat->file_area_temp);
	return scan_file_area_count;
}
#endif

extern void can_tiny_small_file_change_to_small_normal_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_tiny_small *p_file_stat_tiny_small,char is_cache_file);
extern void can_small_file_change_to_normal_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_small *p_file_stat_small,char is_cache_file);
extern int reverse_other_file_area_list_common(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,unsigned int file_area_type,unsigned int file_type,struct list_head *file_area_list);

extern void hot_file_update_file_status(struct address_space *mapping,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,int access_count,int read_or_write);
extern void get_file_name(char *file_name_path,struct file_stat_base *p_file_stat_base);
extern unsigned long cold_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *file_area_free,struct list_head *file_area_have_mmap_page_head);
extern unsigned int cold_mmap_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base * p_file_stat_base,struct file_area *p_file_area,struct page *page_buf[],int cold_page_count);
extern unsigned long shrink_inactive_list_async(unsigned long nr_to_scan, struct lruvec *lruvec,struct hot_cold_file_global *p_hot_cold_file_global,int is_mmap_file, enum lru_list lru);
extern int walk_throuth_all_mmap_file_area(struct hot_cold_file_global *p_hot_cold_file_global);
//extern int cold_mmap_file_stat_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat_del);
extern unsigned int cold_file_stat_delete_all_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int file_type);
extern int cold_file_stat_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base_del,unsigned int file_type);
extern int cold_file_area_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area);

extern void file_stat_temp_middle_large_file_change(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int file_stat_list_type, unsigned int file_type,char is_cache_file);
extern int mmap_file_area_cache_page_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *file_area_have_cache_page_head,struct list_head *file_area_free_temp,unsigned int file_type);
extern int cache_file_area_mmap_page_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *file_area_have_mmap_page_head,unsigned int file_type);
extern int check_file_stat_is_valid(struct file_stat_base *p_file_stat_base,unsigned int file_stat_list_type);
#endif
