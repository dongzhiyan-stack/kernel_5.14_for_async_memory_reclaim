#ifndef _ASYNC_MEMORY_RECLAIM_BASH_H_
#define _ASYNC_MEMORY_RECLAIM_BASH_H_
#include <linux/mm.h>

//#define ASYNC_MEMORY_RECLAIM_IN_KERNEL ------在pagemap.h定义过了
//#define ASYNC_MEMORY_RECLAIM_DEBUG
//#define HOT_FILE_UPDATE_FILE_STATUS_USE_OLD  hot_file_update_file_status函数使用老的方案
//#define ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY
//#define FILE_AREA_IN_FREE_KSWAPD_AND_SHADOW

#define CACHE_FILE_DELETE_PROTECT_BIT 0
#define MMAP_FILE_DELETE_PROTECT_BIT 1

#define FILE_AREA_IN_HOT_LIST 0
#define FILE_AREA_IN_REFAULT_LIST 1
#define FILE_AREA_IN_FREE_LIST 2

/*允许一个周期内file_stat->temp链表上file_area移动到file_stat->temp链表头的最大次数*/
#define FILE_AREA_MOVE_TO_HEAD_COUNT_MAX 16

/*一共3个bit位表示POS_NUM，最大到7*/
#define POS_WARM               0
#define POS_WIITEONLY_OR_COLD  1
#define POS_WARM_COLD          2
#define POS_WARM_MIDDLE        3
//#define POS_WARM             
#define POS_WARM_MIDDLE_HOT    4
#define POS_WARM_HOT           5
#define POS_ZERO_PAGE           6


/*极小的文件，pagecache小于1M的文件*/
#define FILE_STAT_TINY_SMALL 0
/*小文件，pagecache在1M~10M的文件*/
#define FILE_STAT_SMALL 1
//普通文件，包含temp、middle、large 3类文件。pagecache在10M~30M是temp文件、30M~100M是middle文件，100M以上的是large文件 
#define FILE_STAT_NORMAL 2

/*file_area个数小于64是极小文件，在64~640是小文件。现在改为file_area个数小于32是极小文件，在32~640是小文件*/
#define SMALL_FILE_AREA_COUNT_LEVEL 64
//#define SMALL_FILE_AREA_COUNT_LEVEL 32
/*file_area个数在大于640(现改为128，pagecache 2M)，且小于1920是temp文件*/
//#define NORMAL_TEMP_FILE_AREA_COUNT_LEVEL 640
#define NORMAL_TEMP_FILE_AREA_COUNT_LEVEL 128
/*file_area个数在大于1920，且小于6400是middle文件*/
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
#define MEMORY_IN_RECLAIM 2
//异步内存回收周期，单位s
#define ASYNC_MEMORY_RECLIAIM_PERIOD 10
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
#define FILE_AREA_HOT_TO_TEMP_AGE_DX  500
/*发生refault的file_area经过FILE_AREA_REFAULT_TO_TEMP_AGE_DX个周期后，还没有被访问，则移动到file_area_warm链表*/
#define FILE_AREA_REFAULT_TO_TEMP_AGE_DX 500
/*普通的file_area在FILE_AREA_TEMP_TO_COLD_AGE_DX个周期内没有被访问则被判定是冷file_area，然后释放这个file_area的page*/
#define FILE_AREA_TEMP_TO_COLD_AGE_DX  100
/*在file_stat->warm上的file_area经过file_area_warm_to_temp_age_dx个周期没有被访问，则移动到file_stat->temp链表*/
//#define FILE_AREA_WARM_TO_TEMP_AGE_DX  (FILE_AREA_TEMP_TO_COLD_AGE_DX + 10) 
/*一个冷file_area，如果经过FILE_AREA_FREE_AGE_DX个周期，仍然没有被访问，则释放掉file_area结构*/
#define FILE_AREA_FREE_AGE_DX  (FILE_AREA_TEMP_TO_COLD_AGE_DX + 600)
/*当一个file_area因多次访问被设置了ahead标记，经过FILE_AREA_AHEAD_CANCEL_AGE_DX个周期后file_area没有被访问，才会允许清理file_area的ahead标记*/
//#define FILE_AREA_AHEAD_CANCEL_AGE_DX (FILE_AREA_TEMP_TO_COLD_AGE_DX + 10)

/*当一个file_area在一个周期内访问超过FILE_AREA_HOT_LEVEL次数，则判定是热的file_area*/
#define FILE_AREA_HOT_LEVEL (PAGE_COUNT_IN_AREA << 2)
/*如果一个file_area在FILE_AREA_MOVE_HEAD_DX个周期内被访问了两次，然后才能移动到链表头*/
#define FILE_AREA_MOVE_HEAD_DX 5
/*如果连续FILE_AREA_CHECK_HOT_DX个周期cache file_area都被访问，判定file_area为热file_area*/
#define FILE_AREA_CHECK_HOT_DX 3
/*在file_stat被判定为热文件后，记录当时的global_age。在未来HOT_FILE_COLD_AGE_DX时间内该文件进去冷却期：hot_file_update_file_status()函数中
 *只更新该文件file_area的age后，然后函数返回，不再做其他操作，节省性能*/
#define HOT_FILE_COLD_AGE_DX 10


//一个冷file_area，如果经过FILE_AREA_FREE_AGE_DX个周期，仍然没有被访问，则释放掉file_area结构
#define MMAP_FILE_AREA_FREE_AGE_DX  (MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX + 600)
//发生refault的file_area经过FILE_AREA_REFAULT_TO_TEMP_AGE_DX个周期后，还没有被访问，则移动到file_area_temp链表
#define MMAP_FILE_AREA_REFAULT_TO_TEMP_AGE_DX 600
//普通的file_area在FILE_AREA_TEMP_TO_COLD_AGE_DX个周期内没有被访问则被判定是冷file_area，然后释放这个file_area的page
#define MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX  10//这个参数调的很小容易在file_area被内存回收后立即释放，这样测试了很多bug，先不要改

//file_area如果在 MMAP_FILE_AREA_HOT_AGE_DX 周期内被检测到访问 MMAP_FILE_AREA_ACCESS_HOT_COUNT 次，file_area被判定为热file_area
#define MMAP_FILE_AREA_ACCESS_HOT_COUNT 2
//hot链表上的file_area在MMAP_FILE_AREA_HOT_TO_TEMP_AGE_DX个周期内没有被访问，则降级到temp链表
#define MMAP_FILE_AREA_HOT_TO_TEMP_AGE_DX 600

//mapcount的file_area在MMAP_FILE_AREA_MAPCOUNT_AGE_DX个周期内不再遍历访问，降低性能损耗
#define MMAP_FILE_AREA_MAPCOUNT_AGE_DX 5
//hot链表上的file_area在MMAP_FILE_AREA_HOT_AGE_DX个周期内不再遍历访问，降低性能损耗
#define MMAP_FILE_AREA_HOT_AGE_DX 20
//free链表上的file_area在MMAP_FILE_AREA_HOT_AGE_DX个周期内不再遍历访问，降低性能损耗
//#define MMAP_FILE_AREA_FREE_AGE_DX 5
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

#define PRINT_FILE_STAT_INFO 0
#define UPDATE_FILE_STAT_REFAULT_COUNT 1

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

	unsigned int file_area_hot_to_warm_list_count;
	unsigned int file_area_refault_to_warm_list_count;
	unsigned int file_area_free_count_from_free_list;
	
	unsigned int scan_read_file_area_count_from_temp;
	unsigned int temp_to_hot_file_area_count;
	unsigned int scan_ahead_file_area_count_from_temp;
	unsigned int warm_to_hot_file_area_count;
	unsigned int scan_file_area_count_from_warm;
	unsigned int scan_ahead_file_area_count_from_warm;
	unsigned int scan_read_file_area_count_from_warm;
	unsigned int file_area_hot_to_warm_from_hot_file;
	unsigned int del_zero_file_area_file_stat_count;
	unsigned int scan_zero_file_area_file_stat_count;
	unsigned int cache_file_stat_get_file_area_fail_count;
	unsigned int mmap_file_stat_get_file_area_from_cache_count;
	unsigned int scan_cold_file_area_count_from_mmap_file;
	unsigned int isolate_lru_pages_from_mmap_file;
	unsigned int isolate_lru_pages;
	unsigned int free_pages_count;
	unsigned int free_pages_from_mmap_file;
	unsigned int find_mmap_page_count_from_cache_file;
	unsigned int scan_delete_file_stat_count;
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
	
	unsigned int cache_file_stat_get_file_area_fail_count;
	unsigned int mmap_file_stat_get_file_area_from_cache_count;
	unsigned int scan_hot_file_area_count;
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

#ifdef __BIG_ENDIAN
#error "__BIG_ENDIAN not support!!!!!!!!!!!"
#endif
/*bit7表示file_area处于的hot链表，it8~bit15表示file_area所处的各种状态*/
#define FILE_AREA_LIST_VAILD_START_BIT 7
union warm_list_num_and_access_freq{
	unsigned char val;
	struct{
		unsigned char access_freq:4;
		unsigned char warm_list_num:3;
		unsigned char list_hot_bit:1;
	}val_bits;
};

//一个file_area表示了一片page范围(默认6个page)的冷热情况，比如page索引是0~5、6~11、12~17各用一个file_area来表示
struct file_area
{
	/* 要把struct list_head file_area_list放到file_area结构体首地址，如果将来遍历file_stat各种链表上的file_area时，
	 * 错把链表头当成file_area,container_of后得到p_file_area指向的是链表头首地址，后续p_file_area指向的内存，是
	 * file_stat结构体内部的，p_file_area->file_area_state会因状态非法而主动crash*/
	union{
		//file_area通过file_area_list添加file_stat的各种链表
		struct list_head file_area_list;
		//rcu_head和list_head都是16个字节
		struct rcu_head		i_rcu;
	};
	//不同取值表示file_area当前处于哪种链表
	union{
		/* bit0~bit3表示file_area的访问频次计数，bit4~bit6表示file_area所在warm_or_writeonly链表的编号，bit7表示file_area处于的hot链表.
		 * bit8~bit15表示file_area所处的各种状态 bit15~bit31表示file_area的page的各种状态*/
		union warm_list_num_and_access_freq warm_list_num_and_access_freq;
		unsigned int file_area_state;
	};
	//该file_area最近被访问时的global_age，长时间不被访问则与global age差很多，则判定file_area是冷file_area，然后释放该file_area的page
	//如果是mmap文件页，当遍历到文件页的pte置位，才会更新对应的file_area的age为全局age，否则不更新
	unsigned int file_area_age;

#ifndef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY
	//该file_area代表的N个连续page的起始page索引。为了节省内存，改为int类型，因此只能最大只能支持63.9T的文件产生的pagecache。是否要做个限制????????????????????????????
	//pgoff_t start_index;/*之前start_index代表的是对应的起始folio索引*/
	unsigned int start_index;/*现在该为file_area的索引，不是对应的起始folio索引*/
#endif	
	union{
		struct folio __rcu *pages[PAGE_COUNT_IN_AREA];
		/* global_file_stat链表上的file_area在文件iput()释放时，依照file_area->file_area_delete把file_area移动到
		 * global_file_stat->file_stat_delete链表。目的是这个list_move操作不用加锁，避免跟file_area.file_area_list
		 * 链表形成并发*/
		//struct list_head file_area_delete;
	};

	#ifdef HOT_FILE_UPDATE_FILE_STATUS_USE_OLD	
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
#endif
#ifdef FILE_AREA_IN_FREE_KSWAPD_AND_SHADOW
	/*低4位表示file_area的hot ready计数，高4位ahead ready计数*/
	union{
        unsigned char file_area_hot_ahead_ready_all;
		struct{
            unsigned char hot_ready_count : 4;
            unsigned char ahead_ready_count : 4;
		}file_area_hot_ahead;
	};
#endif
	//该file_area里的某个page最近一次被回收的时间点，单位秒
	//unsigned int shrink_time;
	
	/* 
	 * 在引入global_file_stat后，大量乱七八糟的文件的file_area都会移动到到global_file_stat的链表上，这样内存回收时对
	 * file_area里的page的mapping的判断就造成了麻烦，因为内存回收时，必须在page_lock()后判断page->mapping != mapping，
	 * 但是现在global_file_stat链表上的file_area，由于属于不同的文件，就得不到该文件的mapping了。那怎么办？想了一下午
	 * 1：内存回收时，page_lock后不再判断page->mapping != mapping，就是不再理会mapping了。我有一个不太准确的想法，
	 * 内存回收时，只需folio_try_get_rcu(folio)后再判断folio != xas_reload(xas)再page_lock()即可，如果这些判断都成功，
	 * 就能说明page没有被其他进程释放，可以放心的内存该page了。但是内核原生内存回收、文件截断函数，都是page_lock()后
	 * 判断page->mapping != mapping了。如果我不判断，怕异步内存回收有潜在的风险。不能为了节省内存而冒着内存回收的风险
	 * ，最后决定放弃该方案
	 * 2：搞一个file_area_global结构体，多加一个struct address_space *mapping成员，用于保存文件mapping。每个文件在
	 * 第一次读写执行add_folio()函数时，判断inode->i_size如果小于1M，则直接分配file_area_global结构体，并移动到
	 * global_file_stat的链表。inode->i_size如果大于等于1M，则按照原生流程处理，分配file_area结构。这样有几个问题
	 *   2.1 有大量的so库文件，第一次读写时，该文件还不是mmap文件，而是read/write形式读写so库文件的elf文件头，
	 *   此时还是cache文件。后续该文件变成mmap文件，分配的mmap file_area_global就要一直添加到global_file_stat链表
	 *   ，而不是global_mmap_file_stat的链表。mmap file_area_global却移动到了global_file_stat这个cache文件链表。
	 *   这样对内存回收的判定会造成很多麻烦。当然，可以在文件变成mmap文件时，立即令该文件的mapping->rh_reserved1
	 *   由global_file_stat而指向global_mmap_file_stat，后续新分配的mmap file_area_global就会添加到
	 *   global_mmap_file_stat了，这样就解决了global_file_stat有大量mmap file_area_global了。
	 *   2.2 系统有大量inode->i_size小于1M的文件，在读写分配file_area时，分配file_area_global添加到global_file_stat
	 *   的链表时，都要global_file_stat_lock加锁，这样就会出现锁抢占问题。这也是我想以文件为单位的异步内存回收方案
	 *   的原因，分配file_area并移动到file_stat->temp链表时，只需用每个文件的file_stat_lock锁，不需用全局锁
	 *   2.3 现在有两个file_area结构体。struct file_area和struct file_area_global，这样内存回收时，就要分析判断
	 *   struct file_area和struct file_area_global两类结构体了，怎么兼容两个结构体？当然可以这样定义file_area_global
	 *   struct file_area_global{
	 *       struct file_area file_area;
	 *       struct address_space *mapping;
	 *   }
	 *   我把fle_area_global结构体添加到file_stat_global的链表时，是把file_area_global的struct file_area file_area
	 *   成员添加到file_stat_global的链表，后续异步内存回收的代码，依然还是遍历file_area结构体，只需在内存
	 *   回收需要用到文件mapping时，再container_of(p_file_area,struct file_area_global，file_area)得到
	 *   file_area_global，再由file_stat_blobal->mapping得到文件的mapping即可。这样看起来没啥问题，但是就怕
	 *   file_area_global和file_area结构体搞混，造成内存越界访问。代码控制好应该能避免，但还是有点心虚。
	 *
	 *   还有一个小问题：无法设置白名单，禁止某个对性能有要求的文件的file_area转成glboal_file_stat的链表，这样可以更好的控制
	 *   文件的pagecache，不用乱回收page
	 *
	 * 3：在file_area结构体最后再添加一个struct address_space *mapping结构体。始终只使用这一个file_area结构。在每个第一次
	 * 读写时，不再把file_area添加到global_file_stat链表，而还是分配tiny small文件结构体，还是老的流程走。后续异步内存回收
	 * 线程再把inode->i_size小于1M的tiny small 文件的file_area移动到global_file_stat链表。后续如果这个文件转成mmap文件，
	 * 直接把mapping->rh_reserved1由global_file_stat而指向global_mmap_file_stat即可。这样还可以设置文件白名单，禁止一个
	 * 某个对性能有要求的文件的file_area转成glboal_file_stat的链表，这样可以更好的控制文件的pagecache，不用乱回收page。
	 * file_area结构体只有一个，不用担心兼容。坏处就是一个file_area结构体多了8字节，3G的pagecache会导致file_area多消耗
	 * 1.5M的内存(总file_area内存消耗13.5M)。但是会释放几千个文件小于1M的文件tiny small 结构体，又会多少消耗0.38M的内存
	 * */
	union{
		unsigned int refault_pages;
		unsigned int reserved;
		struct address_space *mapping;
	};
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
	unsigned char file_area_move_to_head_count;
	unsigned int refault_page_count;
	unsigned int refault_page_count_last;


	/**针对mmap文件新增的****************************/
	//最新一次访问的file_area，mmap文件用
	//struct file_area *file_area_last;
	//件file_stat->file_area_temp链表上已经扫描的file_stat个数，如果达到file_area_count_in_temp_list，说明这个文件的file_stat扫描完了，才会扫描下个文件file_stat的file_area
	//unsigned int scan_file_area_count_temp_list;-
	//在文件file_stat->file_area_temp链表上的file_area个数
	unsigned int file_area_count_in_temp_list;
	//文件 mapcount大于1的file_area的个数
	//unsigned int mapcount_file_area_count;--------------------------------------
	//当扫描完一轮文件file_stat的temp链表上的file_area时，置1，进入冷却期，在N个age周期内不再扫描这个文件上的file_area。
	//bool cooling_off_start;
	
	//处于中间状态的file_area结构添加到这个链表，新分配的file_area就添加到这里
	struct list_head file_area_temp;
	/************base***base***base************/
}/*__attribute__((packed))*/;
struct file_stat_tiny_small
{
	struct file_stat_base file_stat_base;
	unsigned int reclaim_pages;
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
	unsigned int reclaim_pages;
}/*__attribute__((packed))*/;
//热点文件统计信息，一个文件一个
struct file_stat
{
	struct file_stat_base file_stat_base;
	unsigned int file_area_hot_count;
	unsigned int mapcount_file_area_count;

	//频繁被访问的文件page对应的file_area存入这个头结点。新的方案，mapcount的file_area也移动到这个链表
	struct list_head file_area_hot;
	/*温热的file_area移动到这个链表*/
	struct list_head file_area_warm_hot;
	/*温冷的file_area移动到这个链表*/
	struct list_head file_area_warm;
	struct list_head file_area_warm_cold;
	/*只读的file_area、很长时间未访问的file_area移动到这个链表*/
	struct list_head file_area_writeonly_or_cold;

	/* p_file_area_pos、p_file_area_pos_list_head、temp_head、warm_list_num成员导致file_stat结构体
	 * 大小增加不少。怎么办？完全可以把这些变量移动到hot_cold_file_global全局结构体，指向异步内存回收线程
	 * 当前正在遍历的file_stat->warm等链表头，链表编号，临时保存访问过的file_area的temp_head，完全一样的效果*/
#if 0	
	/* 这个指针依次从file_area_warm、file_area_writeonly_or_cold、file_area_hot链表尾向链表头指向一个个file_area
	 * ，判断file_area冷热程度移动到对应的链表*/
	struct list_head *p_file_area_pos;
	/*指向正在遍历的file_area_warm、file_area_writeonly_or_cold等链表的链表头。定义这个变量是为了，实时直到当前遍历的哪个warm链表头的file_area*/
	struct list_head *p_file_area_pos_list_head;
	/* 遍历file_area_warm、file_area_writeonly_or_cold等链表file_area时，遇到访问过的file_area先移动到temp_head
	 * 链表，因为这些file_area非冷非热，不适合升级到高一级的warm链表，或者降级到低一级的warm链表。等遍历完当前链表的所有
	 * file_area，再把该链表的file_area移动回刚才遍历的链表头。这样做的目的是，让访问过的file_area
	 * 都集中在warm的链表头，链表尾都是没访问过的file_area。后续一旦内存紧张，直接从warm链表头遍历没有访问过的file_area
	 * 即可，有很大概率遍历到的是冷file_area，加快了遍历的效率，不用担心遍历到太多的热的file_area，影响遍历效率*/
	struct list_head temp_head;
	/*指向正在遍历的file_area_warm、file_area_writeonly_or_cold等链表的编号*/
	char   warm_list_num;
#endif    

	/*每轮扫描被释放内存page的file_area结构临时先添加到这个链表，这个变量可以省掉。把这些file_area移动到临时链表，
	 *参与内存回收再移动到file_stat->free链表*/
	//struct list_head file_area_free_temp;
	//所有被释放内存page的file_area结构最后添加到这个链表，如果长时间还没被访问，就释放file_area结构。
	struct list_head file_area_free;
	//file_area的page被释放后，但很快又被访问，发生了refault，于是要把这种page添加到file_area_refault链表，短时间内不再考虑扫描和释放
	//现在决定把refault file_area移动到file_area_hot链表了，从而改名为file_area_warm_cold链表
	//struct list_head file_area_refault;
	
	//file_area对应的page的pagecount大于0的，则把file_area移动到该链表
	//struct list_head file_area_mapcount;
	//存放内存回收的file_area，mmap文件用
	//struct list_head file_area_free_temp;
	/*file_stat回收的总page数*/
	unsigned int reclaim_pages;
	char traverse_warm_list_num;
	/*上一个周期file_stat回收的总page数*/
	//unsigned int reclaim_pages_last_period;
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
struct reclaim_pages_counter{
	unsigned int tiny_small_file_stat_reclaim_pages;
	unsigned int small_file_stat_reclaim_pages;
	unsigned int temp_file_stat_reclaim_pages;
	unsigned int middle_file_stat_reclaim_pages;
	unsigned int large_file_stat_reclaim_pages;
	unsigned int writeonly_file_stat_reclaim_pages;
	unsigned int global_file_stat_reclaim_pages;
};
#define FILE_STAT_CACHE_FILE          0
#define FILE_STAT_MMAP_FILE           1
#define GLOBAL_FILE_STAT_CACHE_FILE   2
#define GLOBAL_FILE_STAT_MMAP_FILE    3

struct current_scan_file_stat_info{
	/*异步内存回收线程当前正在遍历的normal file_stat*/
	struct file_stat *p_traverse_file_stat;
	/* 这个指针依次从file_area_warm、file_area_writeonly_or_cold、file_area_hot链表尾向链表头指向一个个file_area
	 * ，判断file_area冷热程度移动到对应的链表*/
	struct file_area *p_traverse_first_file_area;
	/*指向正在遍历的file_area_warm、file_area_writeonly_or_cold等链表的链表头。定义这个变量是为了，实时直到当前遍历的哪个warm链表头的file_area*/
	struct list_head *p_traverse_file_area_list_head;
	/*file_area被判定热时，升级到更高一级的warm or hot链表*/
	struct list_head *p_up_file_area_list_head;
	/*file_area被判定冷时，降级到更低一级的warm or cold链表*/
	struct list_head *p_down_file_area_list_head;

	/* 遍历file_area_warm、file_area_writeonly_or_cold等链表file_area时，遇到访问过的file_area先移动到temp_head
	 * 链表，因为这些file_area非冷非热，不适合升级到高一级的warm链表，或者降级到低一级的warm链表。等遍历完当前链表的所有
	 * file_area，再把该链表的file_area移动回刚才遍历的链表头。这样做的目的是，让访问过的file_area
	 * 都集中在warm的链表头，链表尾都是没访问过的file_area。后续一旦内存紧张，直接从warm链表头遍历没有访问过的file_area
	 * 即可，有很大概率遍历到的是冷file_area，加快了遍历的效率，不用担心遍历到太多的热的file_area，影响遍历效率*/
	//struct list_head file_stat_tmp_head;
	struct list_head temp_head;
	/*指向正在遍历的file_area_warm、file_area_writeonly_or_cold等链表的编号*/
	char   traverse_list_num;
	char   up_list_num;
	char   down_list_num;
	char   traverse_file_stat_type;

	/* 异步内存回收线程当前正在遍历的normal file_stat->warm等链表的file_area时，遇到要要移动到
	 * 链表头的file_area，先临时移动到这个链表，等遍历完该链表的所有file_area再把这些file_area移动回file_stat->warm等链表*/
    //struct list_head tmp_file_stat_list_head;
	unsigned long move_to_head_file_area_count;
	unsigned long move_to_high_level_file_area_count;
	unsigned long move_to_low_level_file_area_count;
	unsigned long scan_writeonly_file_area_count;
	unsigned long scan_file_area_count;
};

/* 有个隐藏很深的问题，正常只会把cache文件的file_area转到global_file_stat的链表上，但是随后该文件变成mmap文件了。
 * 该文件再分配的的page和file_area都是mmap的了，再把这些file_area都移动到global_file_stat的链表，后续内存回收
 * 就会造成很大的麻烦，因为这些file_area不是cache page。至少global_file_stat得有mapcount链表。并且，后续该mmap
 * 的file_area都要添加到global_file_stat吗？最后决定还是添加到global_mmap_file_stat链表上。具体步骤是，
 * 在add_folio()函数里，检测到文件的mappint->rh_reserved1是global_file_stat，但该文件是mmap文件，则
 * 把global_mmap_file_stat结构体地址赋值给该文件mappint->rh_reserved1，如此该赋值生效前，该文件的mmap fil_area
 * 依然会移动到global_file_stat链表上(会很少)，赋值生效后，文件的mmap fil_area都会移动到global_mmap_file_stat
 * */
struct global_file_stat{
    struct file_stat file_stat;

	//struct list_head file_area_warm_cold;
	struct list_head file_area_warm_middle_hot;
	struct list_head file_area_warm_middle;

	struct list_head file_area_mapcount;
	struct list_head file_area_refault;
	
	struct list_head file_area_delete_list;
	struct list_head file_area_delete_list_temp;
	struct list_head zero_page_file_area_list;
	spinlock_t file_area_delete_lock;
	char traverse_file_stat_type;

	struct current_scan_file_stat_info current_scan_file_stat_info;
};
struct memory_reclaim_info_for_one_warm_list{
	unsigned int scan_file_area_count_in_reclaim;
	unsigned int scan_zero_page_file_area_count_in_reclaim;
	unsigned int scan_warm_file_area_count;

	unsigned int reclaim_pages_count;
};
struct memory_reclaim_info{
	unsigned int scan_file_area_count;
	unsigned int scan_file_area_max;

	unsigned int scan_exit_file_area_count;
	unsigned int scan_zero_page_file_area_count;

	unsigned int warm_list_file_area_up_count;
	unsigned int warm_list_file_area_down_count;

	unsigned int warm_list_file_area_to_writeonly_list_count;
	unsigned int warm_list_file_area_to_writeonly_list_count_cold;

	unsigned int direct_reclaim_pages_form_writeonly_file;
	unsigned int scan_file_area_count_form_writeonly_file;

	unsigned int scan_other_list_file_area_count;
	int scan_file_area_max_for_memory_reclaim;
	unsigned int scan_file_area_count_reclaim_fail;

	struct memory_reclaim_info_for_one_warm_list  memory_reclaim_info_writeonly_list;
	struct memory_reclaim_info_for_one_warm_list  memory_reclaim_info_warm_cold_list;
	struct memory_reclaim_info_for_one_warm_list  memory_reclaim_info_warm_middle_list;
	struct memory_reclaim_info_for_one_warm_list  memory_reclaim_info_warm_list;
};
#define CURRENT_SCAN_FILE_STAT_INFO_TEMP 0
#define CURRENT_SCAN_FILE_STAT_INFO_MIDDLE 1
#define CURRENT_SCAN_FILE_STAT_INFO_LARGE 2
#define CURRENT_SCAN_FILE_STAT_INFO_WRITEONLY 3
/*真TM傻逼，要定义一个有4个成员的数组，竟然直接用CURRENT_SCAN_FILE_STAT_INFO_MAX(3)，好低级的错误，不过脑子的思考就容易犯错呀!!!!!!!!*/
//#define CURRENT_SCAN_FILE_STAT_INFO_MAX  CURRENT_SCAN_FILE_STAT_INFO_WRITEONLY
#define CURRENT_SCAN_FILE_STAT_INFO_MAX  (CURRENT_SCAN_FILE_STAT_INFO_WRITEONLY + 1)
//热点文件统计信息全局结构体
struct hot_cold_file_global
{
	/*指向每次遍历的current_scan_file_stat_info结构体，调试用*/
	struct current_scan_file_stat_info *p_struct_current_scan_file_stat_info;
	struct memory_reclaim_info memory_reclaim_info;
	unsigned int alreay_reclaim_pages;
	unsigned int reclaim_pages_target;
	unsigned int cache_file_warm_list_file_area_up_count;
	unsigned int mmap_file_warm_list_file_area_up_count;
	unsigned int cache_file_warm_list_file_area_down_count;
	unsigned int mmap_file_warm_list_file_area_down_count;
	struct global_file_stat global_file_stat;
	struct global_file_stat global_mmap_file_stat;
	struct current_scan_file_stat_info current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_MAX];
	struct current_scan_file_stat_info current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_MAX];
	struct reclaim_pages_counter reclaim_pages_counter_cache;
	struct reclaim_pages_counter reclaim_pages_counter_mmap;
	unsigned int read_file_area_count_in_reclaim;
	unsigned int cold_file_area_delete_count;

	/*被判定是热文本的file_stat添加到file_stat_hot_head链表,超过50%或者80%的file_area都是热的，则该文件就是热文件，
	 * 文件的file_stat要移动到global的file_stat_hot_head链表*/
	struct list_head file_stat_hot_head;
	//新分配的文件file_stat默认添加到file_stat_temp_head链表
	struct list_head file_stat_temp_head;
	struct list_head file_stat_small_file_head;
	struct list_head file_stat_tiny_small_file_head;
	struct list_head file_stat_tiny_small_file_one_area_head;
	/*中等大小文件移动到这个链表*/
	struct list_head file_stat_middle_file_head;
	/*如果文件file_stat上的page cache数太多，被判定为大文件，则把file_stat移动到这个链表。将来内存回收时，优先遍历这种file_stat，
	 *因为file_area足够多，能遍历到更多的冷file_area，回收到内存page*/
	struct list_head file_stat_large_file_head;
	struct list_head file_stat_writeonly_file_head;
	struct list_head cold_file_head;
	//inode被删除的文件的file_stat移动到这个链表
	struct list_head file_stat_delete_head;
	struct list_head file_stat_small_delete_head;
	struct list_head file_stat_tiny_small_delete_head;
	//0个file_area的file_stat移动到这个链表
	struct list_head file_stat_zero_file_area_head;
	//struct list_head file_stat_middle_zero_file_area_head;
	//struct list_head file_stat_large_zero_file_area_head;
	struct list_head file_stat_small_zero_file_area_head;
	struct list_head file_stat_tiny_small_zero_file_area_head;
	//触发drop_cache后的没有file_stat的文件，分配file_stat后保存在这个链表
	struct list_head drop_cache_file_stat_head;

	//触发drop_cache后的没有file_stat的文件个数
	unsigned int drop_cache_file_count;
	//热文件file_stat个数
	unsigned int file_stat_hot_count;
	unsigned int mmap_file_stat_hot_count;
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
	unsigned int file_area_hot_to_temp_age_dx_ori;
	//发生refault的file_area经过file_area_refault_to_temp_age_dx个周期后，还没有被访问，则移动到file_area_temp链表
	unsigned int file_area_refault_to_temp_age_dx;
	unsigned int file_area_refault_to_temp_age_dx_ori;
	//普通的file_area在file_area_temp_to_cold_age_dx个周期内没有被访问则被判定是冷file_area，然后释放这个file_area的page
	unsigned int file_area_temp_to_cold_age_dx;
	unsigned int file_area_temp_to_cold_age_dx_ori;
	//普通的file_area在file_area_temp_to_warm_age_dx个周期内没有被访问则被判定是温file_area，然后把这个file_area移动到file_stat->file_area_warm链表
	unsigned int file_area_temp_to_warm_age_dx;
	unsigned int file_area_temp_to_warm_age_dx_ori;
	/*在file_stat->warm上的file_area经过file_area_warm_to_temp_age_dx个周期没有被访问，则移动到file_stat->temp链表*/
	unsigned int file_area_warm_to_temp_age_dx;
	unsigned int file_area_warm_to_temp_age_dx_ori;
	/*正常情况不会回收read属性的file_area的page，但是如果该file_area确实很长很长很长时间没访问，也参与回收*/
	unsigned int file_area_reclaim_read_age_dx;
	unsigned int file_area_reclaim_read_age_dx_ori;
	unsigned int file_area_reclaim_ahead_age_dx;
	unsigned int file_area_reclaim_ahead_age_dx_ori;
	unsigned int file_area_cold_level;
	unsigned int to_down_list_age_dx;
	unsigned int to_writeonly_cold_list_age_dx;
	//一个冷file_area，如果经过file_area_free_age_dx_fops个周期，仍然没有被访问，则释放掉file_area结构
	unsigned int file_area_free_age_dx;
	unsigned int file_area_free_age_dx_ori;
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
	struct list_head mmap_file_stat_tiny_small_file_one_area_head;
	struct list_head mmap_file_stat_middle_file_head;
	//文件file_stat个数超过阀值移动到这个链表
	struct list_head mmap_file_stat_large_file_head;
	//热文件移动到这个链表
	struct list_head mmap_file_stat_hot_head;
	//一个文件有太多的page的mmapcount都大于1，则把该文件file_stat移动该链表
	struct list_head mmap_file_stat_mapcount_head;
	//0个file_area的file_stat移动到这个链表，暂时没用到
	struct list_head mmap_file_stat_zero_file_area_head;
	struct list_head mmap_file_stat_small_zero_file_area_head;
	struct list_head mmap_file_stat_tiny_small_zero_file_area_head;
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
	unsigned int dma_zone_free_pages_last;
	unsigned int highmem_zone_free_pages_last;
	unsigned int normal1_zone_free_pages_last;
	/*内存紧张等级，越大表示内存越紧张，并且还会回收有read标记和ahead标记的file_area的page*/
	unsigned int memory_pressure_level;
	/*内存紧急模式，内存回收后，检测内存依然紧张*/
	unsigned int memory_still_memrgency_after_reclaim;
	
	//从系统启动到目前释放的page个数
	unsigned long free_pages;
	//从系统启动到目前释放的mmap page个数
	unsigned long free_mmap_pages;
	//在内存回收期间产生的refault file_area个数
	unsigned long check_refault_file_area_count;
	unsigned long check_refault_file_area_kswapd_count;
	unsigned long check_mmap_refault_file_area_count;
	unsigned long update_file_area_temp_list_count;
	unsigned long update_file_area_warm_list_count;
	unsigned long update_file_area_free_list_count;
	unsigned long file_area_refault_file;

	unsigned long update_file_area_other_list_count;
	unsigned long update_file_area_move_to_head_count;
	unsigned long update_file_area_hot_list_count;
	
	unsigned long file_stat_delete_protect;

	struct file_stat_base *print_file_stat;
	
	unsigned long tiny_small_file_stat_to_one_area_count;
	unsigned long file_stat_tiny_small_one_area_move_tail_count;
	unsigned long file_stat_tiny_small_move_tail_count;
	
	unsigned long kswapd_free_page_count;
	unsigned long async_thread_free_page_count;
	unsigned long kswapd_file_area_refault_file;
	//atomic_t   kswapd_file_area_refault_file;
	
	unsigned int refault_file_area_scan_dx;
	unsigned int reclaim_page_print_level;
	unsigned int refault_page_print_level;
	unsigned int writeonly_file_age_dx_ori;
	unsigned int writeonly_file_age_dx;
	unsigned int in_writeonly_list_file_count;
	/*统计每个内存回收周期回收的page数，每次内存回收前都清0*/
	unsigned int all_reclaim_pages_one_period;
	unsigned long warm_list_file_area_up_count;
	unsigned long warm_list_file_area_down_count;
	unsigned long warm_list_file_area_to_writeonly_list_count;
	unsigned long warm_list_file_area_to_writeonly_list_count_cold;
	unsigned long scan_exit_file_area_count;
	unsigned long scan_zero_page_file_area_count;
	unsigned long direct_reclaim_pages_form_writeonly_file;
};


/*******file_area状态**********************************************************/

/* file_area_state是char类型，只有8个bit位可设置。现在修改了 bit31~bit16 这16个bit位分别用于。file_area_have_page、
 * writeback、dirty、towrite 的bit位，剩下的只有16个bit还能使用。现在bit15~bit12又用于shadow bit了，只剩下12个bit位可用了
 * 最新方案，bit7~bit15用于表示file_area所在的file_stat的链表状态，去除warm、掉warm、ahead、access bit，
 * 用file_stat_status的bit0~bit3表示file_area的访问频次计数，用于统计file_area历史上被访问的频次，这样更能预测file_area
 * 将来被访问的概率。并且，bit15~bit12不再用于shadow bit，异步内存回收的page，在file_area->pages[0]赋值1也能表示shadow bit了
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!*/
enum file_area_status{
	F_file_area_in_hot_list = FILE_AREA_LIST_VAILD_START_BIT,//7
	
	//F_file_area_in_temp_list, in_temp属性取消了，替代方案是，只要file_area没有hot、free、refault、mapcount属性就是in_temp属性
	//F_file_area_in_hot_list,
	//F_file_area_in_warm_list,
	F_file_area_in_free_list,
	F_file_area_in_refault_list,
	/*file_area对应的page的pagecount大于0的，则把file_area移动到该链表*/
	F_file_area_in_mapcount_list,
	FILE_AREA_LIST_VAILD_END_BIT = F_file_area_in_mapcount_list,
	/* 为什么要增加in_mapping_exit属性，只要文件iput()后遍历到file_area，就设置file_area的in_mapping_exit状态。
	 * 后续再遇到这种file_area，要万分小心，绝对不能再对它cold_file_area_delete而从xarray tree剔除*/
	F_file_area_in_mapping_exit,

	/*file_area连续几个周期被访问，本要移动到链表头，处于性能考虑，只是设置file_area的ahead标记。
	 *内存回收遇到有ahead且长时间没访问的file_area，先豁免一次，等下次遍历到这个file_area再回收这个file_area的page*/
	//F_file_area_in_ahead,
	F_file_area_in_read,//bit 12
	F_file_area_in_cache,
	F_file_area_in_mmap,	
	F_file_area_in_init,/*新分配file_area后立即设置该标记*/

	/*file_area第一次被访问设置access bit，第2次被访问只是ahead bit，第3次被访问设置hot bit*/
	//F_file_area_in_access,
	//F_file_area_in_free_kswapd,/*file_area的page被kswapd进程内存回收，不是被异步内存回收线程回收*/
	//F_file_area_in_cache,//file_area保存在ile_stat->hot_file_area_cache[]数组里
};
//不能使用 clear_bit_unlock、test_and_set_bit_lock、test_bit，因为要求p_file_area->file_area_state是64位数据，但实际只是u8型数据

#define MAX_FILE_AREA_LIST_BIT FILE_AREA_LIST_VAILD_END_BIT
//0XFFF &  ~0X7F = 0XF80
#define FILE_AREA_LIST_MASK ((1 << (MAX_FILE_AREA_LIST_BIT + 1)) - 1) & (~((1 << FILE_AREA_LIST_VAILD_START_BIT) - 1))
//清理file_area的状态，在哪个链表
#define CLEAR_FILE_AREA_LIST_STATUS(list_name) \
	static inline void clear_file_area_in_##list_name(struct file_area *p_file_area)\
{clear_bit_unlock(F_file_area_in_##list_name,(unsigned long *)(&p_file_area->file_area_state));}
//{ p_file_area->file_area_state &= ~(1 << F_file_area_in_##list_name);}
//设置file_area在哪个链表的状态
#define SET_FILE_AREA_LIST_STATUS(list_name) \
	static inline void set_file_area_in_##list_name(struct file_area *p_file_area)\
{set_bit(F_file_area_in_##list_name,(unsigned long *)(&p_file_area->file_area_state));}
//{ p_file_area->file_area_state |= (1 << F_file_area_in_##list_name);}
//测试file_area在哪个链表
#define TEST_FILE_AREA_LIST_STATUS(list_name) \
	static inline int file_area_in_##list_name(struct file_area *p_file_area)\
{return test_bit(F_file_area_in_##list_name,(unsigned long *)(&p_file_area->file_area_state));}
//{return p_file_area->file_area_state & (1 << F_file_area_in_##list_name);}

/*这个测试file_area_state的error状态，无法使用set_bit/clear_bit形式，需要特别注意!!!!!!!!!*/
#define TEST_FILE_AREA_LIST_STATUS_ERROR(list_name) \
	static inline int file_area_in_##list_name##_error(struct file_area *p_file_area)\
{return READ_ONCE(p_file_area->file_area_state) & (~(1 << F_file_area_in_##list_name) & FILE_AREA_LIST_MASK);}

#define FILE_AREA_LIST_STATUS(list_name)     \
	CLEAR_FILE_AREA_LIST_STATUS(list_name) \
	SET_FILE_AREA_LIST_STATUS(list_name)  \
	TEST_FILE_AREA_LIST_STATUS(list_name) \
	TEST_FILE_AREA_LIST_STATUS_ERROR(list_name)

//FILE_AREA_LIST_STATUS(temp_list)
FILE_AREA_LIST_STATUS(hot_list)
//FILE_AREA_LIST_STATUS(warm_list)
FILE_AREA_LIST_STATUS(free_list)
FILE_AREA_LIST_STATUS(refault_list)
FILE_AREA_LIST_STATUS(mapcount_list)

#define set_file_area_in_temp_list(p_file_area) {}
#define file_area_in_temp_list(p_file_area) (0 == (p_file_area->file_area_state & FILE_AREA_LIST_MASK))
#define clear_file_area_in_temp_list(p_file_area) {}
#define file_area_in_temp_list_error(p_file_area) (0 != (p_file_area->file_area_state & FILE_AREA_LIST_MASK))

	//清理file_area的状态，在哪个链表
#define CLEAR_FILE_AREA_STATUS(status) \
		static inline void clear_file_area_in_##status(struct file_area *p_file_area)\
{clear_bit_unlock(F_file_area_in_##status,(unsigned long *)(&p_file_area->file_area_state));}
//{ p_file_area->file_area_state &= ~(1 << F_file_area_in_##status);}
	//设置file_area在哪个链表的状态
#define SET_FILE_AREA_STATUS(status) \
		static inline void set_file_area_in_##status(struct file_area *p_file_area)\
{set_bit(F_file_area_in_##status,(unsigned long *)(&p_file_area->file_area_state));}
//{ p_file_area->file_area_state |= (1 << F_file_area_in_##status);}
	//测试file_area在哪个链表
#define TEST_FILE_AREA_STATUS(status) \
		static inline int file_area_in_##status(struct file_area *p_file_area)\
{return test_bit(F_file_area_in_##status,(unsigned long *)(&p_file_area->file_area_state));}
//{return p_file_area->file_area_state & (1 << F_file_area_in_##status);}

#define FILE_AREA_STATUS(status)     \
		CLEAR_FILE_AREA_STATUS(status) \
	SET_FILE_AREA_STATUS(status)  \
	TEST_FILE_AREA_STATUS(status) 

	FILE_AREA_STATUS(cache)
	FILE_AREA_STATUS(mmap)
FILE_AREA_STATUS(init)
	//FILE_AREA_STATUS(ahead)
	FILE_AREA_STATUS(read)
FILE_AREA_STATUS(mapping_exit)
	//FILE_AREA_STATUS(access)
	//FILE_AREA_LIST_STATUS(free_kswapd)


#define file_area_in_writeonly_or_cold_list (p_file_area->file_area_state & FILE_AREA_LIST_MASK == 0)
#define file_area_in_writeonly_or_cold_list_invaild (p_file_area->file_area_state & FILE_AREA_LIST_MASK != 0)

#define file_area_in_temp_list_not_have_hot_status (1 << F_file_area_in_temp_list)
#define file_area_in_warm_list_not_have_hot_status (1 << F_file_area_in_warm_list)
#define file_area_in_free_list_not_have_refault_status (1 << F_file_area_in_free_list)


/*******file_stat状态**********************************************************/
enum file_stat_status{//file_area_state是long类型，只有64个bit位可设置
	F_file_stat_in_file_stat_hot_head_list,
	F_file_stat_in_file_stat_tiny_small_file_one_area_head_list,//只有一个file_area的file_stat_tiny_small移动到这个链表头
	F_file_stat_in_file_stat_tiny_small_file_head_list,
	F_file_stat_in_file_stat_small_file_head_list,

	F_file_stat_in_file_stat_temp_head_list,//
	F_file_stat_in_file_stat_middle_file_head_list,
	F_file_stat_in_file_stat_large_file_head_list,
    F_file_stat_in_file_stat_writeonly_file_head_list,//该文件只有write 的page，没有读page。如果是normal文件则移动到writeonly_normal链表，加快遍历到
	
	F_file_stat_in_mapcount_file_area_list,//文件file_stat是mapcount文件
	F_file_stat_in_zero_file_area_list,//
	F_file_stat_in_file_area_list_end = F_file_stat_in_zero_file_area_list,
	F_file_stat_in_cache_file,//cache文件，sysctl读写产生pagecache。有些cache文件可能还会被mmap映射，要与mmap文件互斥
	F_file_stat_in_mmap_file,//mmap文件，有些mmap文件可能也会被sysctl读写产生pagecache，要与cache文件互斥

	F_file_stat_in_global,
	F_file_stat_in_from_cache_file,//mmap文件是从cache文件的global temp链表移动过来的
	F_file_stat_in_test,
	F_file_stat_in_blacklist,//设置文件黑名单，不扫描内存回收

	/* tiny small文件如果file_area个数超过阈值，则在file_area_alloc_and_init()函数把file_stat_tiny_small移动到链表尾，
	 * 并设置in_tiny_small_to_tail标记。后续file_area_alloc_and_init()中就不会再移动到file_stat_tiny_small移动到链表尾*/
	F_file_stat_in_tiny_small_to_tail,
	/*该file_stat的file_area被移动到了current_scan_file_stat_info->tmp链表*/
	F_file_stat_in_file_area_in_tmp_list,
    F_file_stat_in_writeonly,//该文件只有write 的page，没有读page，这种文件即便file_area个数少也要移动到middel或large文件，内存回收优先回收这种文件。
	F_file_stat_invalid_start_index,
	F_file_stat_in_delete_file,//标识该file_stat被移动到了global delete链表	
	F_file_stat_in_delete,//仅仅表示该file_stat被触发delete了，并不能说明file_stat被移动到了global delete链表
	//F_file_stat_in_drop_cache,
	//F_file_stat_in_free_page,//正在遍历file_stat的file_area的page，尝试释放page
	//F_file_stat_in_free_page_done,//正在遍历file_stat的file_area的page，完成了page的内存回收,
	//F_file_stat_in_large_file,
	//F_file_stat_in_from_small_file,//该文件是从small文件的global small_temp链表移动过来的
	F_file_stat_in_replaced_file,//file_stat_tiny_small或file_stat_small转成更大的文件时，老的file_stat被标记replaced
	F_file_stat_max_index,
	//F_file_stat_lock,
	//F_file_stat_lock_not_block,//这个bit位置1，说明inode在删除的，但是获取file_stat锁失败
};
//不能使用 clear_bit_unlock、test_and_set_bit_lock、test_bit，因为要求p_file_stat->file_stat_status是64位数据，但这里只是u8型数据

//#define MAX_FILE_STAT_LIST_BIT F_file_stat_in_mapcount_file_area_list
#define MAX_FILE_STAT_LIST_BIT F_file_stat_in_zero_file_area_list
#define FILE_STAT_LIST_MASK ((1 << (MAX_FILE_STAT_LIST_BIT + 1)) - 1)

#define FILE_STAT_STATUS_INVALID_MASK (((1 << F_file_stat_max_index) - 1) & (~((1 << (F_file_stat_invalid_start_index + 1)) - 1)))
/*检测file_stat是否有异常状态，有的话就不能执行list_move_enhance()把本次遍历过的file_stat移动到链表头*/
#define file_stat_status_invalid_check(file_stat_status) (READ_ONCE(file_stat_status) & FILE_STAT_STATUS_INVALID_MASK)

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
{clear_bit_unlock(F_file_stat_in_##name##_list,(unsigned long *)(&p_file_stat_base->file_stat_status));}
//{p_file_stat_base->file_stat_status &= ~(1 << F_file_stat_in_##name##_list);}
	//设置file_stat在哪个链表的状态
#define SET_FILE_STAT_STATUS_BASE(name)\
		static inline void set_file_stat_in_##name##_list_base(struct file_stat_base *p_file_stat_base)\
{set_bit(F_file_stat_in_##name##_list,(unsigned long *)(&p_file_stat_base->file_stat_status));}
//{p_file_stat_base->file_stat_status |= (1 << F_file_stat_in_##name##_list);}
	//测试file_stat在哪个链表
#define TEST_FILE_STAT_STATUS_BASE(name)\
		static inline int file_stat_in_##name##_list_base(struct file_stat_base *p_file_stat_base)\
{return test_bit(F_file_stat_in_##name##_list,(unsigned long *)(&p_file_stat_base->file_stat_status));}
//{return (p_file_stat_base->file_stat_status & (1 << F_file_stat_in_##name##_list));}
#define TEST_FILE_STAT_STATUS_ERROR_BASE(name)\
		static inline int file_stat_in_##name##_list##_error_base(struct file_stat_base *p_file_stat_base)\
{return READ_ONCE(p_file_stat_base->file_stat_status) & (~(1 << F_file_stat_in_##name##_list) & FILE_STAT_LIST_MASK);}
//{return p_file_stat_base->file_stat_status & (~(1 << F_file_stat_in_##name##_list) & FILE_STAT_LIST_MASK);}

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
FILE_STAT_STATUS_BASE(file_stat_tiny_small_file_one_area_head)
FILE_STAT_STATUS_BASE(zero_file_area)
FILE_STAT_STATUS_BASE(file_stat_writeonly_file_head)
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
{clear_bit_unlock(F_file_stat_in_##name,(unsigned long *)(&p_file_stat_base->file_stat_status));}
//{p_file_stat_base->file_stat_status &= ~(1 << F_file_stat_in_##name);}
	//设置文件的状态，大小文件等
#define SET_FILE_STATUS_BASE(name)\
		static inline void set_file_stat_in_##name##_base(struct file_stat_base *p_file_stat_base)\
{set_bit(F_file_stat_in_##name,(unsigned long *)(&p_file_stat_base->file_stat_status));}
//{p_file_stat_base->file_stat_status |= (1 << F_file_stat_in_##name);}
	//测试文件的状态，大小文件等
#define TEST_FILE_STATUS_BASE(name)\
		static inline int file_stat_in_##name##_base(struct file_stat_base *p_file_stat_base)\
{return test_bit(F_file_stat_in_##name,(unsigned long *)(&p_file_stat_base->file_stat_status));}
//{return (p_file_stat_base->file_stat_status & (1 << F_file_stat_in_##name));}
#define TEST_FILE_STATUS_ERROR_BASE(name)\
		static inline int file_stat_in_##name##_error_base(struct file_stat_base *p_file_stat_base)\
{return READ_ONCE(p_file_stat_base->file_stat_status) & (~(1 << F_file_stat_in_##name) & FILE_STAT_LIST_MASK);}
//{return p_file_stat_base->file_stat_status & (~(1 << F_file_stat_in_##name) & FILE_STAT_LIST_MASK);}

#define FILE_STATUS_BASE(name) \
		CLEAR_FILE_STATUS_BASE(name) \
	SET_FILE_STATUS_BASE(name) \
	TEST_FILE_STATUS_BASE(name)\
	TEST_FILE_STATUS_ERROR_BASE(name)

FILE_STATUS_BASE(delete)
FILE_STATUS_BASE(test)
FILE_STATUS_BASE(delete_file)
FILE_STATUS_BASE(cache_file)
FILE_STATUS_BASE(mmap_file)
FILE_STATUS_BASE(from_cache_file)
//FILE_STATUS_BASE(from_small_file)
FILE_STATUS_BASE(replaced_file)
FILE_STATUS_BASE(blacklist)
FILE_STATUS_BASE(writeonly)
FILE_STATUS_BASE(global)
FILE_STATUS_BASE(tiny_small_to_tail)
FILE_STATUS_BASE(file_area_in_tmp_list)


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
extern int warm_list_printk;

#define is_global_file_stat_file_in_debug(mapping) (1 == mapping->rh_reserved2)
#define list_num_get(p_file_area)  (p_file_area->warm_list_num_and_access_freq.val_bits.warm_list_num)
#define file_area_access_freq(p_file_area)  (p_file_area->warm_list_num_and_access_freq.val_bits.access_freq)

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

#if 0
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
#else
#define FILE_AREA_PRINT(fmt,...)  {}
#define FILE_AREA_PRINT1(fmt,...) {}
#endif

#ifdef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY

#define CHECK_FOLIO_FROM_FILE_AREA_VALID(xas,mapping,folio,p_file_area,page_offset_in_file_area,folio_index_from_xa_index) \
	if((folio)->index != folio_index_from_xa_index || !is_file_area_page_bit_set(p_file_area,page_offset_in_file_area) || (folio)->mapping != mapping)\
panic("%s xas:0x%llx file_area:0x%llx folio:0x%llx page_offset_in_file_area:%d mapping:0x%llx_0x%llx\n",__func__,(u64)xas,(u64)p_file_area,(u64)folio,page_offset_in_file_area,(u64)mapping,(u64)((folio)->mapping));

#define CHECK_FOLIO_FROM_FILE_AREA_VALID_MARK(xas,mapping,folio,folio_from_file_area,p_file_area,page_offset_in_file_area,folio_index_from_xa_index) \
	if(folio->index != folio_index_from_xa_index || folio != folio_from_file_area || !is_file_area_page_bit_set(p_file_area,page_offset_in_file_area) || (folio)->mapping != mapping)\
panic("%s xas:0x%llx file_area:0x%llx folio:0x%llx folio_from_file_area:0x%llx page_offset_in_file_area:%d mapping:0x%llx_0x%llx\n",__func__,(u64)xas,(u64)p_file_area,(u64)folio,(u64)folio_from_file_area,page_offset_in_file_area,(u64)mapping,(u64)((folio)->mapping));

#else

#define CHECK_FOLIO_FROM_FILE_AREA_VALID(xas,mapping,folio,p_file_area,page_offset_in_file_area,folio_index_from_xa_index) \
	if((folio)->index != (((p_file_area)->start_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area) || (folio)->index != folio_index_from_xa_index || (folio)->mapping != mapping)\
panic("%s xas:0x%llx file_area:0x%llx folio:0x%llx page_offset_in_file_area:%d mapping:0x%llx_0x%llx\n",__func__,(u64)xas,(u64)p_file_area,(u64)folio,page_offset_in_file_area,(u64)mapping,(u64)((folio)->mapping));

#define CHECK_FOLIO_FROM_FILE_AREA_VALID_MARK(xas,mapping,folio,folio_from_file_area,p_file_area,page_offset_in_file_area,folio_index_from_xa_index) \
	if(folio->index != (((p_file_area)->start_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area) || folio->index != folio_index_from_xa_index || folio != folio_from_file_area || (folio)->mapping != mapping)\
panic("%s xas:0x%llx file_area:0x%llx folio:0x%llx folio_from_file_area:0x%llx page_offset_in_file_area:%d mapping:0x%llx_0x%llx\n",__func__,(u64)xas,(u64)p_file_area,(u64)folio,(u64)folio_from_file_area,page_offset_in_file_area,(u64)mapping,(u64)((folio)->mapping));

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

#if 1
static inline void clear_file_area_page_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{	
	//page在 p_file_area->file_area_state对应的bit位清0
	if (!test_and_clear_bit(PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area , (unsigned long *)&p_file_area->file_area_state))
		panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d already clear\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area);
}
static inline void set_file_area_page_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{
	//page在 p_file_area->file_area_state对应的bit位置1
	if(test_and_set_bit(PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area,(unsigned long *)&p_file_area->file_area_state))
		panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d already set\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area);
}
//测试page_offset_in_file_area这个位置的page在p_file_area->file_area_state对应的bit位是否置1了
/*这个测试file_area_state的error状态，无法使用set_bit/clear_bit形式，需要特别注意!!!!!!!!!*/
static inline int is_file_area_page_bit_set(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{
	//unsigned int file_area_page_bit_set = 1 << (PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area);
	unsigned int file_area_page_bit_set = (PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area);

	//return (READ_ONCE(p_file_area->file_area_state) & file_area_page_bit_set);
	return (test_bit(file_area_page_bit_set,(unsigned long *)&p_file_area->file_area_state));
}
/*这个测试file_area_state的error状态，无法使用set_bit/clear_bit形式，需要特别注意!!!!!!!!!*/
static inline int file_area_have_page(struct file_area *p_file_area)
{
	return  (READ_ONCE(p_file_area->file_area_state) & ~((1 << PAGE_BIT_OFFSET_IN_FILE_AREA_BASE) - 1));//0XF000 0000
}

#if 0
/*page shadow bit15~bit12*/
#define SHADOW_IN_FILE_AREA_BASE   (sizeof(unsigned int)*8 - PAGE_COUNT_IN_AREA*5)
static inline void clear_file_area_page_shadow_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{	
	clear_bit(SHADOW_IN_FILE_AREA_BASE + page_offset_in_file_area , (unsigned long *)&p_file_area->file_area_state);
}
static inline void set_file_area_page_shadow_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{
	if(test_and_set_bit(SHADOW_IN_FILE_AREA_BASE + page_offset_in_file_area,(unsigned long *)&p_file_area->file_area_state))
		panic("shadow %s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d already set\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area);
}
static inline int is_file_area_page_shadow(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{
	return (test_bit(SHADOW_IN_FILE_AREA_BASE + page_offset_in_file_area,(unsigned long *)&p_file_area->file_area_state));
}
#endif

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
		file_area_page_bit_clear =  DIRTY_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area;
	}else if (PAGECACHE_TAG_WRITEBACK == type){
		file_area_page_bit_clear = WRITEBACK_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area;
	}else{
		if(PAGECACHE_TAG_TOWRITE != type)
			panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d type:%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,type);

		file_area_page_bit_clear = TOWRITE_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area;
	}
	//page在 p_file_area->file_area_state对应的bit位清0
	clear_bit_unlock(file_area_page_bit_clear,(unsigned long *)&p_file_area->file_area_state);

}
static inline void set_file_area_page_mark_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area,xa_mark_t type)
{
	unsigned int file_area_page_mark_bit_set;

	if(PAGECACHE_TAG_DIRTY == type){
		file_area_page_mark_bit_set = DIRTY_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area;
	}else if (PAGECACHE_TAG_WRITEBACK == type){
		file_area_page_mark_bit_set = WRITEBACK_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area;
	}else{
		if(PAGECACHE_TAG_TOWRITE != type)
			panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d type:%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,type);

		file_area_page_mark_bit_set = TOWRITE_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area;
	}

	//page在 p_file_area->file_area_state对应的bit位置1
	set_bit(file_area_page_mark_bit_set,(unsigned long *)&p_file_area->file_area_state);
}
//测试page_offset_in_file_area这个位置的page在p_file_area->file_area_state对应的bit位是否置1了
static inline int is_file_area_page_mark_bit_set(struct file_area *p_file_area,unsigned char page_offset_in_file_area,xa_mark_t type)
{
	unsigned int file_area_page_mark_bit_set;

	if(PAGECACHE_TAG_DIRTY == type){
		file_area_page_mark_bit_set = DIRTY_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area;
	}else if (PAGECACHE_TAG_WRITEBACK == type){
		file_area_page_mark_bit_set = WRITEBACK_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area;
	}else{
		if(PAGECACHE_TAG_TOWRITE != type)
			panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d type:%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,type);

		file_area_page_mark_bit_set = TOWRITE_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area;
	}

	return (test_bit(file_area_page_mark_bit_set,(unsigned long *)&p_file_area->file_area_state));
}

/*统计有多少个 mark page置位了，比如file_area有3个page是writeback，则返回3*/
/*这个测试file_area_state的error状态，无法使用set_bit/clear_bit形式，需要特别注意!!!!!!!!!*/
static inline int file_area_page_mark_bit_count(struct file_area *p_file_area,char type)
{
	unsigned int file_area_page_mark;
	int count = 0;
	unsigned long page_mark_mask = (1 << PAGE_COUNT_IN_AREA) - 1;/*与上0xF，得到4个bit哪些置位0*/

	if(PAGECACHE_TAG_DIRTY == type){
		file_area_page_mark = (READ_ONCE(p_file_area->file_area_state) >> DIRTY_MARK_IN_FILE_AREA_BASE) & page_mark_mask;
	}else if (PAGECACHE_TAG_WRITEBACK == type){
		file_area_page_mark = (READ_ONCE(p_file_area->file_area_state) >> WRITEBACK_MARK_IN_FILE_AREA_BASE) & page_mark_mask;
	}else{
		if(PAGECACHE_TAG_TOWRITE != type)
			panic("%s file_area:0x%llx file_area_state:0x%x type:%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,type);

		file_area_page_mark = (READ_ONCE(p_file_area->file_area_state) >> TOWRITE_MARK_IN_FILE_AREA_BASE) & page_mark_mask;
	}
	while(file_area_page_mark){
		if(file_area_page_mark & 0x1)
			count ++;

		file_area_page_mark = file_area_page_mark >> 1;
	}

	return count;
}

#else
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
#endif

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
extern int shrink_page_printk_open_important;


#ifdef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY 
#define file_area_shadow_bit_set (1UL << 61)
#define file_area_index_bit_set  (1UL << 62)
/* folio非0，但不是有效的指针，即bit63是0，说明保存在file_area->folio[]中的是file_area的索引，不是有效的page指针。
 * 但是，最新方案，bit62是1说明是index，bit61是1说明是shadow(page刚内存回收)。因此，file_area->pages[0~3]除了保存
 * 的是folio或NULL指针外，还有 1:是file_area_index(bit62是1) 2:是shadow(bit61是1)  3:file_area_index和shadow共存(bit62和bit61是1)*/
#define folio_is_file_area_index_or_shadow(folio) ((0 == ((u64)folio & (1UL << 63))) && ((u64)folio & (file_area_shadow_bit_set | file_area_index_bit_set)))

/*如果folio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
#define folio_is_file_area_index_or_shadow_and_clear_NULL(folio) \
{ \
	if(folio_is_file_area_index_or_shadow(folio))\
		folio = NULL; \
}

/* p_file_area)->pages[]中保存的file_area的索引file_area->start_index >> PAGE_COUNT_IN_AREA_SHIFT后的，不不用再左移PAGE_COUNT_IN_AREA_SHIFT后的
 * 要与上U32_MAX，因为bit62 和 bit61可能是1*/
#define get_file_area_start_index(p_file_area) (((u64)((p_file_area)->pages[0]) << 32) + (u64)((u64)((p_file_area)->pages[1]) & U32_MAX))

#else
#define folio_is_file_area_index_or_shadow(folio) xa_is_value(folio)

#define folio_is_file_area_index_or_shadow_and_clear_NULL(folio) \
{ \
	if(folio_is_file_area_index_or_shadow(folio))\
		folio = NULL; \
}
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

	/*为了防护没有加锁就解锁，于是主动触发crash*/
	//clear_bit_unlock(lock_bit,&hot_cold_file_global_info.file_stat_delete_protect);
	if(!test_and_clear_bit(lock_bit,&hot_cold_file_global_info.file_stat_delete_protect))
		BUG();
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
static inline unsigned int get_file_stat_normal_type_all(struct file_stat_base *p_file_stat_base)
{
	unsigned int file_stat_list_bit = p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;

	switch (file_stat_list_bit){
		case 1 << F_file_stat_in_file_stat_temp_head_list:
		case 1 << F_file_stat_in_file_stat_middle_file_head_list:
		case 1 << F_file_stat_in_file_stat_large_file_head_list:
			return 1;

		default:
			return 0;
	}
	return 0;

}
static inline unsigned int get_file_stat_normal_type(struct file_stat_base *p_file_stat_base)
{
	unsigned int file_stat_list_bit = p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;

	switch (file_stat_list_bit){
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
static inline unsigned int get_file_stat_type_common(unsigned int file_stat_list_bit)
{
	switch (file_stat_list_bit){
		case 1 << F_file_stat_in_file_stat_small_file_head_list:
			return FILE_STAT_SMALL;
		case 1 << F_file_stat_in_file_stat_tiny_small_file_head_list:
		case 1 << F_file_stat_in_file_stat_tiny_small_file_one_area_head_list:
			return FILE_STAT_TINY_SMALL;

		case 1 << F_file_stat_in_file_stat_temp_head_list:
		case 1 << F_file_stat_in_file_stat_hot_head_list:
		case 1 << F_file_stat_in_file_stat_middle_file_head_list:
		case 1 << F_file_stat_in_file_stat_large_file_head_list:
		case 1 << F_file_stat_in_mapcount_file_area_list:
		case 1 << F_file_stat_in_file_stat_writeonly_file_head_list:
			return FILE_STAT_NORMAL;

		default:
			return -1;
	}
	return -1;
}

static inline unsigned int get_file_stat_type(struct file_stat_base *p_file_stat_base)
{
	unsigned int file_stat_list_bit = p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;

	return get_file_stat_type_common(file_stat_list_bit);
}
static inline unsigned int get_file_stat_type_file_iput(struct file_stat_base *p_file_stat_base)
{
	unsigned int file_stat_list_bit = p_file_stat_base->file_stat_status & FILE_STAT_LIST_MASK;

	/* iput()文件时，遇到in_zero_list的file_stat，这种file_stat还保留了原始in_temp、in_middle等状态，
	 * 这里要清理掉file_stat的in_zero状态，否则get_file_stat_type()会返回-1，现在只返回in_temp、in_middle原始状态*/

	/*编译老是告警，warning: array subscript ‘long unsigned int[0]’ is partly outside array bounds*/
	//test_and_clear_bit(F_file_stat_in_zero_file_area_list,(unsigned long *)&file_stat_list_bit);

	if(file_stat_list_bit & (1 << F_file_stat_in_zero_file_area_list))
		file_stat_list_bit &= ~(1 << F_file_stat_in_zero_file_area_list);

	return get_file_stat_type_common(file_stat_list_bit);
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
 *  过了几个星期，突然再看这个并发方案，突然有个疑问。如果file_stat被iput()提前释放了，set_file_stat in delete，
 *  p_file_stat_base->mapping->rh_reserved1赋值0。之后inode和mapping就是无效内存了。然后异步内存回收线程执行
 *  is_file_stat_mapping_error()用到if(p_file_stat_base != p_file_stat_base->mapping->rh_reserved1)，
 *  p_file_stat_base->mapping->rh_reserved1岂不是无效内存访问？没关系，又不向这个内存写数据，只是读。
 *  此时p_file_stat_base->mapping->rh_reserved1是0，is_file_stat_mapping_error()中
 *  if(p_file_stat_base != p_file_stat_base->mapping->rh_reserved1)就不成立了，然后
 *  if(file_stat_in_delete_base(p_file_stat_base))也成立。如果这个文件inode又被新的进程分配了，则
 *  inode->mapping->rh_reserved1就会指向新的file_stat，上边的分析也成立
 *
 * 1:rcu_read_lock()防止inode被iput()释放了，导致 p_file_stat_base->mapping->rh_reserved1无效内存访问。
 * 2:rcu_read_lock()加printk打印会导致休眠吧，这点要控制
 * 3:smp_rmb()保证p_file_stat_base->mapping->rh_reserved1在iput()被赋值0后，file_stat一定有delete标记。iput()里是set_file_stat_in_delete;smp_wmb;p_file_stat_base->mapping->rh_reserved1=0
 *
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
 * BUG！BUG！BUG！上一个方案以为rcu_read_lock();if((unsigned long)p_file_stat_base != (p_file_stat_base)->mapping->rh_reserved1) 这个
 * 用法没问题。如果文件inode和mapping被释放了，p_file_stat_base->mapping->rh_reserved1就是无效内存访问！上一个方案竟然以为
 * "p_file_stat_base->mapping->rh_reserved1岂不是无效内存访问？没关系，又不向这个内存写数据，只是读" 真搞不清楚当初咋想的。无效内存
 * 就是无效内存，绝对读都不能读。深入想一想，如果mapping被属内存被某一个slab分配，mapping->rh_reserved1内存出的数据还碰巧就是
 * p_file_stat_base指针，那is_file_stat_mapping_error()就失效了。因此"无效内存读是决定不能读的!!!!!!"要想解决这个问题，可以把
 * is_file_stat_mapping_error()放到file_inode_lock()后边，file_inode_lock()能防护inode被释放。但是我想让is_file_stat_mapping_error()
 * 独立于file_inode_lock()之外。完全可以，就像file_inode_lock()的实现:"先rcu_read_lock，再smp_rmb()，再立即判断file_stat有in_delete"
 * 标记。这就重复造车了。于是决定把 is_file_stat_mapping_error()移动到file_inode_lock()里"rcu_read_lock，再smp_rmb()，再立即判断file_stat有in_delete"
 * 后边。is_file_stat_mapping_error()源码还维持原版
 * */
#define is_file_stat_mapping_error(p_file_stat_base) \
{ \
	rcu_read_lock();\
	if((unsigned long)p_file_stat_base != READ_ONCE((p_file_stat_base)->mapping->rh_reserved1)){  \
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
#if 0
static inline void print_file_name(struct address_space *mapping)
{
    struct inode *inode = mapping->host;
	if(/*init_printk && */inode){
        if(!hlist_empty(&inode->i_dentry)){
             struct dentry *dentry = hlist_entry(inode->i_dentry.first, struct dentry, d_u.d_alias);
			 struct dentry *parent;
			 if(dentry){
                 parent = dentry->d_parent;
				 if(strcmp("libgvfsdbus.so",dentry->d_name.name) == 0 || strcmp("pipewire-0.3",parent->d_name.name) == 0 ||
						 strcmp("cursors",parent->d_name.name) == 0){
					 //mapping->rh_reserved2 = 1;

					 if(strcmp("libgvfsdbus.so",dentry->d_name.name) == 0 || strcmp("left_ptr_watch",dentry->d_name.name) == 0 || strcmp("watch",dentry->d_name.name) == 0
						|| strcmp("bottom_tee",dentry->d_name.name) == 0 ||strcmp("dnd-ask",dentry->d_name.name) == 0 || strcmp("pointer-move",dentry->d_name.name) == 0 ||
						strcmp("libpipewire-module-rtkit.so",dentry->d_name.name) == 0 || strcmp("libpipewire-module-rt.so",dentry->d_name.name) == 0 ){
				         printk("%s %s %d inode:0x%llx mapping:0x%llx %s/%s\n",__func__,current->comm,current->pid,(u64)inode,(u64)mapping,parent->d_name.name,dentry->d_name.name);
						 mapping->rh_reserved3 = 1;
						 mapping->rh_reserved2 = 1;
					     dump_stack();
					 }
				 }
			 }else
				 printk("%s %s_%d inode:0x%llx print_file_name dentry null\n",__func__,current->comm,current->pid,(u64)inode);
		}else
			printk("%s %s_%d inode:0x%llx print_file_name inode1 null\n",__func__,current->comm,current->pid,(u64)inode);
	}else
		printk("%s %s_%d inode mapping:0x%llx null\n",__func__,current->comm,current->pid,(u64)mapping);
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

	//print_file_name(mapping);
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
		INIT_LIST_HEAD(&p_file_stat->file_area_warm_hot);
		INIT_LIST_HEAD(&p_file_stat->file_area_warm);
		INIT_LIST_HEAD(&p_file_stat->file_area_warm_cold);
		INIT_LIST_HEAD(&p_file_stat->file_area_writeonly_or_cold);
		//INIT_LIST_HEAD(&p_file_stat->file_area_free_temp);
		INIT_LIST_HEAD(&p_file_stat->file_area_free);
		//INIT_LIST_HEAD(&p_file_stat->file_area_refault);
		//INIT_LIST_HEAD(&p_file_stat->file_area_mapcount);

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

	//print_file_name(mapping);
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
		INIT_LIST_HEAD(&p_file_stat->file_area_warm_hot);
		INIT_LIST_HEAD(&p_file_stat->file_area_warm);
		INIT_LIST_HEAD(&p_file_stat->file_area_warm_cold);
		INIT_LIST_HEAD(&p_file_stat->file_area_writeonly_or_cold);

		INIT_LIST_HEAD(&p_file_stat_base->file_area_temp);
		/*mmap文件需要p_file_stat->file_area_free_temp暂存参与内存回收的file_area，不能注释掉*/
		//INIT_LIST_HEAD(&p_file_stat->file_area_free_temp);
		INIT_LIST_HEAD(&p_file_stat->file_area_free);
		//INIT_LIST_HEAD(&p_file_stat->file_area_refault);
		//file_area对应的page的pagecount大于0的，则把file_area移动到该链表
		//INIT_LIST_HEAD(&p_file_stat->file_area_mapcount);

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


	/*新分配的file_stat的recent_access_age赋值global age，否则就是0，可能会被识别为冷文件而迅速释放掉*/
	p_file_stat_base->recent_access_age = hot_cold_file_global_info.global_age;
	//设置file_stat的in mmap文件状态
	hot_cold_file_global_info.mmap_file_stat_count++;
	spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
	if(shrink_page_printk_open)
		printk("%s file_stat:0x%llx\n",__func__,(u64)p_file_stat_base);

out:
	return p_file_stat_base;
}
static inline void file_stat_base_struct_init(struct file_stat_base *p_file_stat_base,char is_cache_file)
{
	//设置文件是mmap文件状态，有些mmap文件可能还会被读写，要与cache文件互斥，要么是cache文件要么是mmap文件，不能两者都是
	if(is_cache_file){
		set_file_stat_in_cache_file_base(p_file_stat_base);
		hot_cold_file_global_info.file_stat_tiny_small_count++;
		/*只有cache文件才设置writeonly标记*/
		set_file_stat_in_writeonly_base(p_file_stat_base);
	}
	else{
		set_file_stat_in_mmap_file_base(p_file_stat_base);
		hot_cold_file_global_info.mmap_file_stat_tiny_small_count++;
	}

	INIT_LIST_HEAD(&p_file_stat_base->file_area_temp);
	spin_lock_init(&p_file_stat_base->file_stat_lock); 
}
static inline void file_stat_base_init(struct address_space *mapping,struct file_stat_base *p_file_stat_base,char is_cache_file)
{
	mapping->rh_reserved1 = (unsigned long)(p_file_stat_base);
	p_file_stat_base->mapping = mapping;

	file_stat_base_struct_init(p_file_stat_base,is_cache_file);
#if 0	
	//设置文件是mmap文件状态，有些mmap文件可能还会被读写，要与cache文件互斥，要么是cache文件要么是mmap文件，不能两者都是
	if(is_cache_file){
		set_file_stat_in_cache_file_base(p_file_stat_base);
		hot_cold_file_global_info.file_stat_tiny_small_count++;
		/*只有cache文件才设置writeonly标记*/
		set_file_stat_in_writeonly_base(p_file_stat_base);
	}
	else{
		set_file_stat_in_mmap_file_base(p_file_stat_base);
		hot_cold_file_global_info.mmap_file_stat_tiny_small_count++;
	}
	INIT_LIST_HEAD(&p_file_stat_base->file_area_temp);
	spin_lock_init(&p_file_stat_base->file_stat_lock);
#endif	
	/*新分配的file_stat的recent_access_age赋值global age，否则就是0，可能会被识别为冷文件而迅速释放掉*/
	p_file_stat_base->recent_access_age = hot_cold_file_global_info.global_age;
}
static inline struct file_stat_base *file_stat_alloc_and_init_tiny_small(struct address_space *mapping,char is_cache_file)
{
	struct file_stat_tiny_small *p_file_stat_tiny_small = NULL;
	struct file_stat_base *p_file_stat_base = NULL;
	spinlock_t *p_global_lock;
	if(is_cache_file)
		p_global_lock = &hot_cold_file_global_info.global_lock;
	else
		p_global_lock = &hot_cold_file_global_info.mmap_file_global_lock;

	p_file_stat_tiny_small = kmem_cache_alloc(hot_cold_file_global_info.file_stat_tiny_small_cachep,GFP_ATOMIC);
	if (!p_file_stat_tiny_small) {
		printk("%s file_stat alloc fail\n",__func__);
		goto out;
	}
	memset(p_file_stat_tiny_small,0,sizeof(struct file_stat_tiny_small));

	spin_lock(p_global_lock);

	/*如果已经有进程并发分配了file_stat并执行file_stat_base_init()赋值给了mapping->rh_reserved1，那就释放掉本次分配的file_stat*/
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		spin_unlock(p_global_lock);

		kmem_cache_free(hot_cold_file_global_info.file_stat_tiny_small_cachep,p_file_stat_tiny_small);
		p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;

		printk("%s file_stat:0x%llx already alloc\n",__func__,(u64)mapping->rh_reserved1);
		goto out;
	}
	p_file_stat_base = &p_file_stat_tiny_small->file_stat_base;
	/* 对file_stat_base初始化，重点赋值mapping->rh_reserved1 = p_file_stat_base。这个赋值必须放到spin_lock加锁代码里，不能放到spin_unlock后。
	 * 否则，进程1 spin_unlock，还没赋值mapping->rh_reserved1 = p_file_stat_base。进程spin_lock成功，此时mapping->rh_reserved1还没赋值，
	 * 导致这里又把同一个文件的file_stat_tiny_small添加到mmap_file_stat_tiny_small_file_head链表，泄漏了，乱套了*/
	file_stat_base_init(mapping,p_file_stat_base,is_cache_file);
	set_file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base);

	if(is_cache_file){
		//hot_cold_file_global_info.file_stat_tiny_small_count++;
		hot_cold_file_global_info.file_stat_count++;
		list_add(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_tiny_small_file_head);
	}
	else{
		hot_cold_file_global_info.mmap_file_stat_tiny_small_count++;
		list_add(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_tiny_small_file_head);
	}

	spin_unlock(p_global_lock);


out:
	return p_file_stat_base;
}

static inline  struct current_scan_file_stat_info *get_normal_file_stat_current_scan_file_stat_info(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int file_stat_list_type,char is_cache_file)
{
	switch (file_stat_list_type){
		case (1 << F_file_stat_in_file_stat_temp_head_list):
			if(is_cache_file)
				return &p_hot_cold_file_global->current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_TEMP];
			else
				return &p_hot_cold_file_global->current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_TEMP];
		case (1 << F_file_stat_in_file_stat_middle_file_head_list):
			if(is_cache_file)
				return &p_hot_cold_file_global->current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_MIDDLE];
			else
				return &p_hot_cold_file_global->current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_MIDDLE];
		case (1 << F_file_stat_in_file_stat_large_file_head_list):
			if(is_cache_file)
				return &p_hot_cold_file_global->current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_LARGE];
			else
				return &p_hot_cold_file_global->current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_LARGE];
		case (1 << F_file_stat_in_file_stat_writeonly_file_head_list):
			if(is_cache_file)
				return &p_hot_cold_file_global->current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_WRITEONLY];
			else
				return &p_hot_cold_file_global->current_scan_mmap_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_WRITEONLY]; 
		default:
			panic("file_stat_list_type:0x%x is_cache_file:%d\n",file_stat_list_type,is_cache_file);
	}
}

/* 当current_scan_file_stat_info->p_traverse_file_stat指向的global->temp、middle、large、writeonly尾巴的链表file_stat，
 * 1:被iput()释放标记delete了、因file_area个数发生变化转成normal writeonly、temp、middle、large文件而文件状态变化
 * 2:当创建新的file_stat移动到global->temp、middle、large、writeonly链表尾巴，current_scan_file_stat_info->p_traverse_file_stat
 * 指向的file_stat不是global->temp、middle、large、writeonly链表尾的file_stat了，那就要清空
 * current_scan_file_stat_info->p_traverse_file_stat = NULL，令其失效，下边遍历global->temp、middle、large、writeonly链表尾的
 * file_stat时，再赋值给current_scan_file_stat_info->p_traverse_file_stat，这样才不会在
 * file_stat_multi_level_warm_or_writeonly_list_file_area_solve()函数里，因为current_scan_file_stat_info->p_traverse_file_stat
 * 跟global->temp、middle、large、writeonly链表尾的file_stat不相等而 panic。
 * 
 * 另外，仅仅更新下一次遍历该文件file_stat时，要遍历的warm链表编号，其他的p_traverse_file_area_list_head等信息在下次遍历到该文件file_stat时再更新*/
static inline void update_file_stat_next_multi_level_warm_or_writeonly_list(struct current_scan_file_stat_info *p_current_scan_file_stat_info,struct file_stat *p_file_stat)
{
	/*如果p_traverse_file_stat已经是NULL，说明p_traverse_file_stat已经没有指向遍历过的file_stat了，直接return*/
	if(NULL == p_current_scan_file_stat_info->p_traverse_file_stat)
		return;

	if(!file_stat_in_file_area_in_tmp_list_base(&p_current_scan_file_stat_info->p_traverse_file_stat->file_stat_base))
		panic("%s p_current_scan_file_stat_info:0x%llx p_file_stat:0x%llx error",__func__,(u64)p_current_scan_file_stat_info,(u64)p_file_stat);

	clear_file_stat_in_file_area_in_tmp_list_base(&p_current_scan_file_stat_info->p_traverse_file_stat->file_stat_base);

	/*1:当前file_stat的warm链表上的file_area完成了，更新next_num_list，还要把p_traverse_file_stat设置NULL，这样下次遍历同类型的
	 * file_stat时，才会更新到p_traverse_file_stat。同时，还要先把之前遍历过的移动到temp_head链表上的file_area移动回warm链表头
	 *2:当前file_stat的状态发生变化了，比如normal file_stat转成writeonly文件，或者temp、midle、large文件之间来回转换。执行同样的操作*/
	if(!list_empty(&p_current_scan_file_stat_info->temp_head)){
		list_splice_init(&p_current_scan_file_stat_info->temp_head,p_current_scan_file_stat_info->p_traverse_file_area_list_head);
	}
	/*普通文件的某个warm链表file_area遍历完后，必须把p_current_scan_file_stat_info->p_traverse_file_stat设置NULL，
	 * 然后下个周期遍历时，发现它是NULL，才会遍历的新的文件file_stat*/
	p_current_scan_file_stat_info->p_traverse_file_stat = NULL;
	p_current_scan_file_stat_info->p_traverse_file_area_list_head = NULL;
	p_current_scan_file_stat_info->p_traverse_first_file_area = NULL;
	p_current_scan_file_stat_info->p_up_file_area_list_head = NULL;
	p_current_scan_file_stat_info->p_down_file_area_list_head = NULL;

	switch(p_file_stat->traverse_warm_list_num){
		case POS_WARM_HOT:
			/* 遍历过file_stat->warm_hot链表上的file_area后，不再允许异步内存回收线程traverse_file_stat_multi_level_warm_list()遍历
			 * file_stat->writeonly链表上的file_area了。因为现在判定有点浪费性能，反正异步内存回收时，遍历file_stat->writeonly
			 * 链表上的file_area进行内存回收时。如果file_area最近访问过，直接移动到file_stat->warm链表。*/
			//p_file_stat->traverse_warm_list_num = POS_WIITEONLY_OR_COLD;
			p_file_stat->traverse_warm_list_num = POS_WARM_COLD;
			break;
		case POS_WARM:
			p_file_stat->traverse_warm_list_num = POS_WARM_HOT;
			break;
		case POS_WARM_COLD:
			p_file_stat->traverse_warm_list_num = POS_WARM;
			break;
		case POS_WIITEONLY_OR_COLD:
			p_file_stat->traverse_warm_list_num = POS_WARM_COLD;
			break;
		default:
			BUG();
	}
}

static inline struct file_stat_base *file_stat_alloc_and_init_other(struct address_space *mapping,unsigned int file_type,char free_old_file_stat,char is_cache_file,char is_writeonly_file)
{
	struct file_stat_base *p_file_stat_base = NULL;
	spinlock_t *p_global_lock;

	if(is_cache_file)
		p_global_lock = &hot_cold_file_global_info.global_lock;
	else
		p_global_lock = &hot_cold_file_global_info.mmap_file_global_lock;

	if(FILE_STAT_SMALL == file_type){
		struct file_stat_small *p_file_stat_small;

		p_file_stat_small = kmem_cache_alloc(hot_cold_file_global_info.file_stat_small_cachep,GFP_ATOMIC);
		if (!p_file_stat_small) {
			spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
			printk("%s file_stat alloc fail\n",__func__);
			goto out;
		}
		memset(p_file_stat_small,0,sizeof(struct file_stat_small));

		spin_lock(p_global_lock);

		/*1:如果两个进程同时访问一个文件，同时执行到这里，需要加锁。第1个进程加锁成功后，分配file_stat并赋值给
		 *mapping->rh_reserved1，第2个进程获取锁后执行到这里mapping->rh_reserved1就会成立
		 *2:异步内存回收功能禁止了
		 *3:当small file_stat转到normal file_stat，释放老的small file_stat然后分配新的normal file_stat，此时
		 *free_old_file_stat 是1，下边的if不成立，忽略mapping->rh_reserved1，进而才不会goto out，而是分配新的file_stat
		 *mapping->rh_reserved1指向的老的file_stat另有代码释放掉这里不用管
		 */
		if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping) && !free_old_file_stat){
			spin_unlock(p_global_lock);
			
			kmem_cache_free(hot_cold_file_global_info.file_stat_small_cachep,p_file_stat_small);
			p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;

			printk("%s file_stat:0x%llx already alloc\n",__func__,(u64)mapping->rh_reserved1);
			goto out;  
		}

		p_file_stat_base = &p_file_stat_small->file_stat_base;
		file_stat_base_init(mapping,p_file_stat_base,is_cache_file);
		//mapping->file_stat记录该文件绑定的file_stat结构，将来判定是否对该文件分配了file_stat
		//mapping->rh_reserved1 = (unsigned long)(p_file_stat_base);
		//p_file_stat_base->mapping = mapping;
		//设置文件是mmap文件状态，有些mmap文件可能还会被读写，要与cache文件互斥，要么是cache文件要么是mmap文件，不能两者都是 
		//set_file_stat_in_mmap_file_base(p_file_stat_base);
		//INIT_LIST_HEAD(&p_file_stat_base->file_area_temp);
		//spin_lock_init(&p_file_stat_base->file_stat_lock);

		INIT_LIST_HEAD(&p_file_stat_small->file_area_other);

		set_file_stat_in_file_stat_small_file_head_list_base(p_file_stat_base);

		if(is_cache_file){
			hot_cold_file_global_info.file_stat_small_count++;
			/*writeonly文件要移动到链表尾，这样写个周期就可以被异步内存回收线程遍历到*/
			if(0 == is_writeonly_file)
				list_add(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_small_file_head);
			else
				list_add_tail(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_small_file_head);
		}
		else{
			hot_cold_file_global_info.mmap_file_stat_count++;
			list_add(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_small_file_head);
		}
		spin_unlock(p_global_lock);
	}
	else if(FILE_STAT_NORMAL == file_type){
		struct file_stat *p_file_stat;

		p_file_stat = kmem_cache_alloc(hot_cold_file_global_info.file_stat_cachep,GFP_ATOMIC);
		if (!p_file_stat) {
			spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
			printk("%s file_stat alloc fail\n",__func__);
			goto out;
		}
		memset(p_file_stat,0,sizeof(struct file_stat));

		spin_lock(p_global_lock);

		if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping) && !free_old_file_stat){
			spin_unlock(p_global_lock);

			kmem_cache_free(hot_cold_file_global_info.file_stat_cachep,p_file_stat);
			p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;

			printk("%s file_stat:0x%llx already alloc\n",__func__,(u64)mapping->rh_reserved1);
			goto out;  
		}

		p_file_stat_base = &p_file_stat->file_stat_base;
		file_stat_base_init(mapping,p_file_stat_base,is_cache_file);

		//设置文件是mmap文件状态，有些mmap文件可能还会被读写，要与cache文件互斥，要么是cache文件要么是mmap文件，不能两者都是 
		//set_file_stat_in_mmap_file_base(p_file_stat_base);
		INIT_LIST_HEAD(&p_file_stat->file_area_hot);
		INIT_LIST_HEAD(&p_file_stat->file_area_warm);
		INIT_LIST_HEAD(&p_file_stat->file_area_warm_hot);
		INIT_LIST_HEAD(&p_file_stat->file_area_warm_cold);
		INIT_LIST_HEAD(&p_file_stat->file_area_writeonly_or_cold);
		
		INIT_LIST_HEAD(&p_file_stat_base->file_area_temp);
		/*mmap文件需要p_file_stat->file_area_free_temp暂存参与内存回收的file_area，不能注释掉*/
		//INIT_LIST_HEAD(&p_file_stat->file_area_free_temp);
		INIT_LIST_HEAD(&p_file_stat->file_area_free);
		//INIT_LIST_HEAD(&p_file_stat->file_area_refault);
		//file_area对应的page的pagecount大于0的，则把file_area移动到该链表
		//INIT_LIST_HEAD(&p_file_stat->file_area_mapcount);

		//mapping->file_stat记录该文件绑定的file_stat结构，将来判定是否对该文件分配了file_stat
		//mapping->rh_reserved1 = (unsigned long)(p_file_stat_base);
		//p_file_stat_base->mapping = mapping;
#if 1
		/*新分配的file_stat必须设置in_file_stat_temp_head_list链表。这个设置file_stat状态的操作必须放到 把file_stat添加到
		 *tmep链表前边，还要加内存屏障。否则会出现一种极端情况，异步内存回收线程从temp链表遍历到这个file_stat，
		 *但是file_stat还没有设置为in_temp_list状态。这样有问题会触发panic。因为mmap文件异步内存回收线程，
		 *从temp链表遍历file_stat没有mmap_file_global_lock加锁，所以与这里存在并发操作。而针对cache文件，异步内存回收线程
		 *从global temp链表遍历file_stat，全程global_lock加锁，不会跟向global temp链表添加file_stat存在方法，但最好改造一下*/
		set_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
		smp_wmb();
#endif	
		//spin_lock_init(&p_file_stat_base->file_stat_lock);
		if(is_cache_file){
			hot_cold_file_global_info.file_stat_count++;
			/*writeonly文件要移动到链表尾，这样写个周期就可以被异步内存回收线程遍历到*/
			if(0 == is_writeonly_file)
				list_add(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_temp_head);
			else{
				/*得到temp文件的current_scan_file_stat_info*/
				struct current_scan_file_stat_info *p_current_scan_file_stat_info = get_normal_file_stat_current_scan_file_stat_info(&hot_cold_file_global_info,1 << F_file_stat_in_file_stat_temp_head_list,is_cache_file);
				/* 把新分配的file_stat移动到global->temp链表尾，而如果current_scan_file_stat_info[CURRENT_SCAN_FILE_STAT_INFO_TEMP]
				 * ->p_traverse_file_stat不是NULL，说明它保存了上个周期遍历的glboal->temp链表尾的file_stat->warm链表的
				 * file_area，但没有遍历完file_stat->warm链表上所有的file_area，于是p_traverse_file_stat指向该file_stat。
				 * 该p_traverse_file_stat永远指向global->temp链表尾的file_stat，这是规定。现在要向glboal->temp链表添加
				 * 新的file_stat了，必须令p_traverse_file_stat赋值NULL。下个周期，从global->temp链表尾得到这个新的
				 * file_stat，再赋值p_traverse_file_stat*/
				update_file_stat_next_multi_level_warm_or_writeonly_list(p_current_scan_file_stat_info,p_file_stat);         
				list_add_tail(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_temp_head);
			}
		}
		else{
			hot_cold_file_global_info.mmap_file_stat_count++;
			list_add(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_temp_head);
		}

		spin_unlock(p_global_lock);

	}else
		BUG();

	if(shrink_page_printk_open)
		printk("%s file_stat:0x%llx\n",__func__,(u64)p_file_stat_base);

out:
	return p_file_stat_base;
}

/* 引入global_file_stat，在把file_stat_tiny_small文件的file_area都移动到global_file_stat链表后，是令该文件的
 * mapping->rh_reserved1 = &p_hot_cold_file_global->global_file_stat.file_stat_base，即mapping->rh_reserved1 指向
 * global_file_stat.file_stat_base，后续就跟普通文件的file_stat_base一摸一样了，因此file_area_alloc_and_init()分配
 * file_area，初始化file_area，把file_area移动到p_file_stat_base->file_area_temp链表，对global_file_stat和普通的
 * 文件file_stat来说，处理都一样*/
static inline struct file_area *file_area_alloc_and_init(unsigned int area_index_for_page,struct file_stat_base *p_file_stat_base,struct address_space *mapping)
{
	struct file_area *p_file_area = NULL;

	/*大部分小于1M的so文件，最初都是read/write读写，被判定为cache文件。转成global_file_stat后，file_area都是移动到了
	 * hot_cold_file_global_info.global_file_stat链表。后续mmap建立映射后，检测到该情况，分配page和file_area，要令
	 * mapping->rh_reserved1 = hot_cold_file_global_info.global_mmap_file_stat.file_stat_base，使后续分配的file_area都移动到global_mmap_file_stat链表*/
	if(mapping_mapped(mapping) && file_stat_in_global_base(p_file_stat_base)){
		if(p_file_stat_base != &hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_stat_base){
			printk("mapping:0x%llx file_stat_base:0x%llx change to global_mmap_file_stat\n",(u64)mapping,(u64)p_file_stat_base);
			mapping->rh_reserved1 = (u64)(&hot_cold_file_global_info.global_mmap_file_stat.file_stat.file_stat_base);
		}
	}

	/* 有个并发分配file_area造成file_area内存泄露的的bug，如果多进程分配同一个索引的file_area，在这里加锁阻塞。等第2加锁的进程加锁成功，
	 * 依然会分配同一个索引的file_area并返回，造成前边同一个索引的file_area泄露了。因此要加锁后判断同一个索引的file_area是否已经分配了
	 * 不用，因为执行该函数对同一个文件分配file_area前的add_folio函数，xas_lock_irq(&xas)加锁了，同一个文件同时只有一个函数执行该函数分配file_area*/
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
	//p_file_area->start_index = area_index_for_page << PAGE_COUNT_IN_AREA_SHIFT;//area_index_for_page * PAGE_COUNT_IN_AREA;
	p_file_area->start_index = area_index_for_page;//area_index_for_page * PAGE_COUNT_IN_AREA;
#endif	
	p_file_stat_base->file_area_count ++;//文件file_stat的file_area个数加1
	set_file_area_in_temp_list(p_file_area);//新分配的file_area必须设置in_temp_list链表

	//在file_stat->file_area_temp链表的file_area个数加1
	p_file_stat_base->file_area_count_in_temp_list ++;
	set_file_area_in_init(p_file_area);
	p_file_area->mapping = mapping;

	
out:
	spin_unlock(&p_file_stat_base->file_stat_lock);

	/* 如果tiny small文件的file_area个数超过阀值了，则把file_stat移动到global tiny_small链表尾，异步内存回收
	 * 线程下个周期就会把该file_stat转成大file_stat或small file_stat。这个操作完全可以去掉。不行，有个漏洞。
	 * 这个if很容易成立，file_area个数大于阀值时，每次执行到这里if都成立，浪费性能。可以判断是否是否在链表尾
	 * global file_stat_tiny_small_file_head或global file_stat_tiny_small_file_one_area_head或
	 * global mmap_file_stat_tiny_small_file_head链表尾，太麻烦了，这个函数不允许浪费性能。那怎么解决，把
	 * file_area个数很少的tiny small file stat尽可能移动到global tiny small one area链表，尽可能移动到快的发现
	 * 在global tiny small链表上的file_area个数很多的file_stat。也不太好。最后决定这样处理
	 * 1：把global file_stat_tiny_small_file链表上的file_stat尽可能多移动到global file_stat_tiny_small_file_one_area
	 * 链表，只要file_area个数小于3都移动，如此global file_stat_tiny_small_file链表上的file_stat会尽可能少
	 * 2：当前函数，只判断global file_stat_tiny_small_file_one_area链表上file_stat_tiny_small_one_area，
	 * 如果file_area个数大于阀值，则把该file_stat_tiny_small_one_area移动到global file_stat_tiny_small链表，
	 * 这样if判断条件就很少了，也不用判断file_stat_tiny_small_file_one_area是否处于链表头。移动链表后，
	 * if(file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base))就不成立了*/
#if 0	
	if(file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base) && p_file_stat_base->file_area_count > SMALL_FILE_AREA_COUNT_LEVEL &&
			!file_stat_in_tiny_small_to_tail_base(p_file_stat_base)){
		
		/*设置in_tiny_small_to_tail标记，保证上边的if只成立一次*/
		set_file_stat_in_tiny_small_to_tail_base(p_file_stat_base);

		if(file_stat_in_cache_file_base(p_file_stat_base)){
			spin_lock(&hot_cold_file_global_info.global_lock);
			/* 1：一切把file_stat移动到global temp、small、tiny_small链表的的操作，加锁后都要判断file_stat是否被iput释放了
			 * 但是执行该函数时，该文件正被读写分配folio，文件不可能会iput()，这个判断是多余的，先留着吧。
			 * 2：但是，还有另一个重点，这里跟跟异步内存回收线程经常并发，因此加锁后，必须再判断，file_stat状态是否改变了，
			 * 必须再判断一次file_stat状态!!!!!!!!!!!!!!!!!!!*
			 * 3：还要再判断一次file_stat是否还是in_cache_file状态，因为异步内存回收线程可能把cache file转成mmap file了。
			 *    mmap文件不判断转成cache文件，故mmap文件不做这个判断*/
			if(!file_stat_in_delete_base(p_file_stat_base) && file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base) &&
					file_stat_in_cache_file_base(p_file_stat_base)){
				/*clear_file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base);
				set_file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base);
				list_move_tail(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_tiny_small_file_head);*/
				
				/*最后决定还是不要跨链表移动了，只是移动到本链表的链表尾，怕出现并发问题*/
				list_move_tail(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_tiny_small_file_one_area_head);
			}
			spin_unlock(&hot_cold_file_global_info.global_lock);
		}
		else{
			spin_lock(&hot_cold_file_global_info.mmap_file_global_lock);
			if(!file_stat_in_delete_base(p_file_stat_base) && file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base)){
				/*clear_file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base);
				set_file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base);
				list_move_tail(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_tiny_small_file_head);*/

				list_move_tail(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_tiny_small_file_one_area_head);
			}
			spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
		}
		hot_cold_file_global_info.file_stat_tiny_small_one_area_move_tail_count ++;
	}
#endif

	/* 1:该文件可能被并发iput()释放掉  2:异步内存回收线程正把该文件并发由cache文件转成mmap文件 3:异步内存回收线程正把该文件并发由tiny small转成small或normal文件
	 * 还要考虑一点，如果异步内存回收线程，正在使用的file_stat正好是该p_file_stat_base，会有并发问题吗？想想不会的*/
	if(file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base) && p_file_stat_base->file_area_count > NORMAL_TEMP_FILE_AREA_COUNT_LEVEL &&
			!file_stat_in_tiny_small_to_tail_base(p_file_stat_base)){
         
		/*设置in_tiny_small_to_tail标记，保证上边的if只成立一次*/
		set_file_stat_in_tiny_small_to_tail_base(p_file_stat_base);
		if(file_stat_in_cache_file_base(p_file_stat_base)){
			spin_lock(&hot_cold_file_global_info.global_lock);
			if( !file_stat_in_delete_base(p_file_stat_base) && file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base) &&
					file_stat_in_cache_file_base(p_file_stat_base)){
				list_move_tail(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.file_stat_tiny_small_file_head);
			}
			spin_unlock(&hot_cold_file_global_info.global_lock);
		}
		else{
			spin_lock(&hot_cold_file_global_info.mmap_file_global_lock);
			if(!file_stat_in_delete_base(p_file_stat_base) && file_stat_in_file_stat_tiny_small_file_head_list_base(p_file_stat_base)){
				list_move_tail(&p_file_stat_base->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_tiny_small_file_head);
			}
			spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
		}

		hot_cold_file_global_info.file_stat_tiny_small_move_tail_count ++;
	}

	/* 新分配的file_area，不管有没有访问都赋值当前global_age，否则就是0，可能会被识别为冷file_area而迅速释放掉.因为怕有些
	 * file_area分配后，对应的page不会被读写，就没有机会执行到update函数，赋值file_area_age=global_age，并令access_count加1.
	 * 。决定这里不再赋值了，
	 * 而是读写时执行hot_file_update_file_status()再给file_area_age赋值。但是这里赋值的话，file_area_age跟global_age就相等了，
	 * 然后很快对应的page被访问了，而执行到update函数，if(file_area_age < global_age)不成立，就不会令file_area的access_count加
	 * 1。实际mysql测试，发现大量的文件，预读时分配folio和file_area。但是因为这里的赋值，导致这些file_area的page很快被访问时，
	 * 执行到update函数，if(file_area_age < global_age)不成立，就不会令file_area的access_count加1。然后异步内存回收线程扫描到
	 * 这些file_area，access_count是0，有很大概率被移动到下一层的warm链表，很容易被回收掉，然后发生refault。现在决定，这个赋值
	 * 还留着。update函数修改为if(file_area_age < global_age || test_and_clear_bit(F_file_area_in_init,&p_file_area->file_area_state))
	 * 这里新分配的file_area因为有init标记，如此该if依然成立，然后顺利令file_area的access_count加1*/
	p_file_area->file_area_age = hot_cold_file_global_info.global_age; 
	return p_file_area;
}
#if 1
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
 *文件的inode，此时返回1。如果inode被iput释放而有free标记则返回-1。总之加锁失败返回值 <= 0*/
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
	if(file_stat_in_delete_base(p_file_stat_base) || (NULL == READ_ONCE(p_file_stat_base->mapping))){
		//不要忘了异常return要先释放锁
		rcu_read_unlock();
		return 0;
	}

	/* 判断file_stat和file_stat->mapping->rh_reserved1是否匹配，不匹配则crash。注意，到这里rcu_read_lock可以确保该文件
	 * inode和mapping结构体不会被立即释放掉，可以放心使用file_stat->mapping->rh_reserved1*/
	is_file_stat_mapping_error(p_file_stat_base);

	inode = p_file_stat_base->mapping->host;

	spin_lock(&inode->i_lock);
	/*执行到这里，inode肯定没有被释放，并且inode->i_lock加锁成功，其他进程就无法再释放这个inode了。错了，又一个隐藏很深的bug。
	 *!!!!!!!!!!!!!!!!因为其他进程此时可能正在iput()->__destroy_inode_handler_post()中触发释放inode。这里rcu_read_unlock后，
	 *inode就会立即被释放掉，然后下边再使用inode就会访问无效inode结构而crash。rcu_read_unlock要放到对inode引用计数加1后*/

	//unlock_file_stat(p_file_stat);
	//rcu_read_unlock();

	/*inode正被其他进程iput释放，加锁失败。此时该文件的file_stat就可能没有in_delete标记，这个遇到过*/
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
		}else{
			if(shrink_page_printk_open_important)
				printk("%s file_stat:0x%llx inode:0x%llx icount0!!!!!!! i_nlink:%d nrpages:%ld lru_list_empty:%d %s\n",__func__,(u64)p_file_stat_base,(u64)inode,inode->i_nlink,inode->i_mapping->nrpages,list_empty(&inode->i_lru),inode->i_sb->s_id);
		}
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
    
	/*加锁成功lock_fail是0而返回1，因inode有free标记而加锁失败则lock_fail是1而返回-1*/
	return (0 == lock_fail) ? 1:-1;
}
#else
static void inline file_inode_unlock(struct file_stat_base * p_file_stat_base)
{
}
static void inline file_inode_unlock_mapping(struct address_space *mapping)
{
}
static int inline file_inode_lock(struct file_stat_base *p_file_stat_base)
{
	return 1;
}
#endif

#ifdef HOT_FILE_UPDATE_FILE_STATUS_USE_OLD
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
#else

#ifdef FILE_AREA_IN_FREE_KSWAPD_AND_SHADOW
static void inline file_area_access_count_clear(struct file_area *p_file_area)
{
	p_file_area->file_area_hot_ahead.hot_ready_count = 0;
}
static void inline file_area_access_count_add(struct file_area *p_file_area,int count)
{
	/*hot_ready_count只是unsigned char的一半，占了4个bit位，最大只能16*/
	if(p_file_area->file_area_hot_ahead.hot_ready_count < 0xF)
		p_file_area->file_area_hot_ahead.hot_ready_count ++;
}
static int inline file_area_access_count_get(struct file_area *p_file_area)
{
	return p_file_area->file_area_hot_ahead.hot_ready_count;
}
#else
static void inline file_area_access_count_clear(struct file_area *p_file_area)
{
}
static void inline file_area_access_count_add(struct file_area *p_file_area,int count)
{
}
static int inline file_area_access_count_get(struct file_area *p_file_area)
{
	return 0;
}

#endif
#endif
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

			if(shrink_page_printk_open_important)
				printk("%ps->list_move_enhance() head:0x%llx ok\n",__builtin_return_address(0),(u64)head);
		}else{
			if(shrink_page_printk_open_important || first != head->next)
			printk("%ps->list_move_enhance() head:0x%llx first:0x%llx head->next:0x%llx %d fail\n",__builtin_return_address(0),(u64)head,(u64)first,(u64)head->next,list_is_last(tail,head));
		}
	}
}
/*现在temp的file_area，是所有表示file_area所在链表的bit都是0，特殊处理*/
static int inline can_file_area_move_to_list_head_for_temp_list_file_area(struct file_area *p_file_area,struct list_head *file_area_list_head)
{
	//p_file_area在链表的后一个file_area
	struct file_area *p_file_area_next = list_next_entry(p_file_area, file_area_list);
	//p_file_area在链表的前一个file_area
	struct file_area *p_file_area_prev = list_prev_entry(p_file_area, file_area_list);

	/*file_area不能是链表头*/
	if(&p_file_area->file_area_list == file_area_list_head)
		return 0;
	//file_area在链表的前一个file_area不是链表头，这个判断其实可以不用加，在list_move_enhance()函数就有判断
	/*if(&p_file_area_prev->file_area_list == file_area_list_head)
	  return 0;*/

	//file_area在链表的后一个file_area可能是链表头
	/*if(&p_file_area_next->file_area_list == file_area_list_head)
	  return 0;*/

	/* 如果file_area不在file_area_in_list_type这个file_stat的链表上，测试失败
	 * 如果file_area检测到在其他file_stat链表上，测试失败
	 * */
	if(get_file_area_list_status(p_file_area) != 0 || get_file_area_list_status(p_file_area_prev) != 0 || get_file_area_list_status(p_file_area_next) != 0){
		printk("%ps->can_file_area_move file_area_list_head:0x%llx file_area:0x%llx state:0x%x next:0x%llx state:0x%x prev:0x%llx state:0x%x p_file_area_error\n",__builtin_return_address(0),(u64)file_area_list_head,(u64)p_file_area,p_file_area->file_area_state,(u64)p_file_area_next,p_file_area_next->file_area_state,(u64)p_file_area_prev,p_file_area_prev->file_area_state);
		return 0;
	}

	return 1;
}
/*测试file_area是否真的在file_area_in_list_type这个file_stat的链表(file_stat->temp、hot、refault、warm、mapcount链表)，不在则不能把p_file_area从链表尾的file_area移动到链表头*/
static int inline can_file_area_move_to_list_head(struct file_area *p_file_area,struct list_head *file_area_list_head,unsigned int file_area_in_list_type_bit)
{
#if 0	
	/*file_area不能是链表头*/
	if(&p_file_area->file_area_list == file_area_list_head)
		return 0;
	/*如果file_area不在file_area_in_list_type这个file_stat的链表上，测试失败*/
    if(0 == (p_file_area->file_area_state & (1 << file_area_in_list_type_bit)))
		return 0;
    /*如果file_area检测到在其他file_stat链表上，测试失败*/
	if(p_file_area->file_area_state & (~(1 << file_area_in_list_type_bit) & FILE_AREA_LIST_MASK))
		return 0;
#endif
	unsigned int file_area_in_list_type = 1 << file_area_in_list_type_bit;
	//p_file_area在链表的后一个file_area
	struct file_area *p_file_area_next = list_next_entry(p_file_area, file_area_list);
	//p_file_area在链表的前一个file_area
	struct file_area *p_file_area_prev = list_prev_entry(p_file_area, file_area_list);

	/*file_area不能是链表头*/
	if(&p_file_area->file_area_list == file_area_list_head)
		return 0;
	//file_area在链表的前一个file_area不是链表头，这个判断其实可以不用加，在list_move_enhance()函数就有判断
	/*if(&p_file_area_prev->file_area_list == file_area_list_head)
	  return 0;*/

	//file_area在链表的后一个file_area可能是链表头
	/*if(&p_file_area_next->file_area_list == file_area_list_head)
	  return 0;*/

	/* 如果file_area不在file_area_in_list_type这个file_stat的链表上，测试失败
	 * 如果file_area检测到在其他file_stat链表上，测试失败
	 * */
	if(0 == (p_file_area->file_area_state & file_area_in_list_type) ||  p_file_area->file_area_state & (~(file_area_in_list_type) & FILE_AREA_LIST_MASK)){ 
		printk("%ps->can_file_area_move file_area_list_head:0x%llx file_area:0x%llx state:0x%x file_area_in_list_type:0x%x p_file_area_error\n",__builtin_return_address(0),(u64)file_area_list_head,(u64)p_file_area,p_file_area->file_area_state,file_area_in_list_type);
		return 0;
	}

	/*同样检测前一个file_area是否合法*/
	/*这里有个问题，在遍历in_free链表上的file_area时，file_area有in_free和in_refault标记时，这里的prev file_area，下边的
	 * next file_area的异常前后file_area判定，有很大概率判定导致if成立，而直接return 0，导致无法把本次遍历的file_area移动到file_stat->free链表头*/
	if(&p_file_area_prev->file_area_list != file_area_list_head){
		if(0 == (p_file_area_prev->file_area_state & file_area_in_list_type) ||  p_file_area_prev->file_area_state & (~(file_area_in_list_type) & FILE_AREA_LIST_MASK)){
			printk("%ps->can_file_area_move file_area_list_head:0x%llx file_area:0x%llx p_file_area_prev:0x%llx state:0x%x file_area_in_list_type:0x%x p_file_area_prev error\n",__builtin_return_address(0),(u64)file_area_list_head,(u64)p_file_area,(u64)p_file_area_prev,p_file_area_prev->file_area_state,file_area_in_list_type);
			return 0;
		}
	}

	/*同样检测后一个file_area是否合法，但它可能是链表头，要过滤掉*/
	if(&p_file_area_next->file_area_list != file_area_list_head){
		if(0 == (p_file_area_next->file_area_state & file_area_in_list_type) ||  p_file_area_next->file_area_state & (~(file_area_in_list_type) & FILE_AREA_LIST_MASK)){
			printk("%ps->can_file_area_move file_area_list_head:0x%llx file_area:0x%llx p_file_area_next:0x%llx state:0x%x file_area_in_list_type:0x%x p_file_area_next error\n",__builtin_return_address(0),(u64)file_area_list_head,(u64)p_file_area,(u64)p_file_area_next,p_file_area_next->file_area_state,file_area_in_list_type);
			return 0;
		}
	}

	if(shrink_page_printk_open1)
		printk("%ps->can_file_area_move file_area_list_head:0x%llx file_area:0x%llx\n",__builtin_return_address(0),(u64)file_area_list_head,(u64)p_file_area);

	return 1;
}
static int inline can_file_area_move_to_list_head_for_small_file_other(struct file_area *p_file_area,struct list_head *file_area_list_head,unsigned int file_area_in_list_type)
{
	//p_file_area在链表的后一个file_area
	struct file_area *p_file_area_next = list_next_entry(p_file_area, file_area_list);
	//p_file_area在链表的前一个file_area
	struct file_area *p_file_area_prev = list_prev_entry(p_file_area, file_area_list);

	/*file_area不能是链表头*/
	if(&p_file_area->file_area_list == file_area_list_head)
		return 0;
	//file_area在链表的前一个file_area不是链表头，这个判断其实可以不用加，在list_move_enhance()函数就有判断
	/*if(&p_file_area_prev->file_area_list == file_area_list_head)
	  return 0;*/

	//file_area在链表的后一个file_area可能是链表头
	/*if(&p_file_area_next->file_area_list == file_area_list_head)
	  return 0;*/

	/* 如果file_area不在file_area_in_list_type这个file_stat的链表上，测试失败
	 * 如果file_area检测到在其他file_stat链表上，测试失败
	 * */
	if(0 == (p_file_area->file_area_state & file_area_in_list_type) ||  p_file_area->file_area_state & (~(file_area_in_list_type) & FILE_AREA_LIST_MASK)){ 
		printk("%ps->can_file_area_move other_list file_area_list_head:0x%llx file_area:0x%llx state:0x%x file_area_in_list_type:0x%x p_file_area_error\n",__builtin_return_address(0),(u64)file_area_list_head,(u64)p_file_area,p_file_area->file_area_state,file_area_in_list_type);
		return 0;
	}

	/*同样检测前一个file_area是否合法*/
	if(&p_file_area_prev->file_area_list != file_area_list_head){
		if(0 == (p_file_area_prev->file_area_state & file_area_in_list_type) ||  p_file_area_prev->file_area_state & (~(file_area_in_list_type) & FILE_AREA_LIST_MASK)){
			printk("%ps->can_file_area_move other_list file_area_list_head:0x%llx file_area:0x%llx p_file_area_prev:0x%llx state:0x%x file_area_in_list_type:0x%x p_file_area_prev error\n",__builtin_return_address(0),(u64)file_area_list_head,(u64)p_file_area,(u64)p_file_area_prev,p_file_area_prev->file_area_state,file_area_in_list_type);
			return 0;
		}
	}

	/*同样检测后一个file_area是否合法，但它可能是链表头，要过滤掉*/
	if(&p_file_area_next->file_area_list != file_area_list_head){
		if(0 == (p_file_area_next->file_area_state & file_area_in_list_type) ||  p_file_area_next->file_area_state & (~(file_area_in_list_type) & FILE_AREA_LIST_MASK)){
			printk("%ps->can_file_area_move other_list file_area_list_head:0x%llx file_area:0x%llx p_file_area_next:0x%llx state:0x%x file_area_in_list_type:0x%x p_file_area_next error\n",__builtin_return_address(0),(u64)file_area_list_head,(u64)p_file_area,(u64)p_file_area_next,p_file_area_next->file_area_state,file_area_in_list_type);
			return 0;
		}
	}

	if(shrink_page_printk_open1)
		printk("%ps->can_file_area_move other_list file_area_list_head:0x%llx file_area:0x%llx\n",__builtin_return_address(0),(u64)file_area_list_head,(u64)p_file_area);
	return 1;
}

static int inline can_file_stat_move_to_list_head_for_one(struct file_stat_base *p_file_stat_base,unsigned int file_stat_in_list_type,char is_cache_file)
{
	if(is_cache_file && !file_stat_in_cache_file_base(p_file_stat_base)){
		printk("%ps->can_file_stat_move one file_stat:0x%llx status:0x%x  file_stat_in_list_type_bit:%d mmap file not move to cache file head\n",__builtin_return_address(0),(u64)p_file_stat_base,p_file_stat_base->file_stat_status,file_stat_in_list_type);
		return 1;
	}
	else if(!is_cache_file && !file_stat_in_mmap_file_base(p_file_stat_base)){
		printk("%ps->can_file_stat_move one file_stat:0x%llx status:0x%x file_stat_in_list_type_bit:%d cache file not move to  mmap file head\n",__builtin_return_address(0),(u64)p_file_stat_base,p_file_stat_base->file_stat_status,file_stat_in_list_type);
		return 1;
	}

	/*if成立说明file_stat有异常属性，被标记delete了或者从cache文件转成mmap文件而被list_del了等等*/
	if(file_stat_status_invalid_check(p_file_stat_base->file_stat_status)){
		printk("%ps->can_file_stat_move one file_stat:0x%llx status:0x%x file_stat_in_list_type_bit:%d file_stat_status_invalid_check error\n",__builtin_return_address(0),(u64)p_file_stat_base,p_file_stat_base->file_stat_status,file_stat_in_list_type);
		return 1;
	}

	/*1:如果file_stat不在file_stat_in_list_type这个global链表上，测试失败
	 *2:如果file_stat检测到在file_stat_in_list_type除外的其他global链表上，测试失败*/
	if(0 == (p_file_stat_base->file_stat_status & (1 << file_stat_in_list_type))  ||  p_file_stat_base->file_stat_status & (~(1 << file_stat_in_list_type) & FILE_STAT_LIST_MASK)){
		/*如果是tiny small file转成了tiny small file one area，不再打印，这个很常见*/
		if(!(file_stat_in_file_stat_tiny_small_file_one_area_head_list_base(p_file_stat_base) && (F_file_stat_in_file_stat_tiny_small_file_head_list == file_stat_in_list_type)))
		    printk("%ps->can_file_stat_move one file_stat:0x%llx status:0x%x file_stat_in_list_type_bit:%d file_stat_type error\n",__builtin_return_address(0),(u64)p_file_stat_base,p_file_stat_base->file_stat_status,file_stat_in_list_type);

		return 1;
	}

	return 0;
}
/*测试file_stat是否真的在file_stat_in_list_type这个global链表上(global temp、middle、large、hot、mapcount链表)，不在则不能把p_file_stat到链表尾的file_stat移动到链表头*/
static int inline can_file_stat_move_to_list_head(struct list_head *file_stat_temp_head,struct file_stat_base *p_file_stat_base,unsigned int file_stat_in_list_type,char is_cache_file)
{
	struct file_stat_base *p_file_stat_base_next = list_next_entry(p_file_stat_base, hot_cold_file_list);
	struct file_stat_base *p_file_stat_base_prev = list_prev_entry(p_file_stat_base, hot_cold_file_list);

	/*如果file_stat在链表前后没有成员，失败。如果file_stat_base已经从链表删除，失败*/
	if(list_empty(&p_file_stat_base->hot_cold_file_list) || p_file_stat_base->hot_cold_file_list.next == LIST_POISON1 || p_file_stat_base->hot_cold_file_list.prev == LIST_POISON2){
		printk("%ps->can_file_stat_move file_stat:0x%llx  is_cache_file:%d file_stat_in_list_type_bit:%d error!!!!!!!!!!!!!!\n",__builtin_return_address(0),(u64)p_file_stat_base,is_cache_file,file_stat_in_list_type);
		return 0;
	}

	/*检测p_file_stat_base是否合法*/
	if(can_file_stat_move_to_list_head_for_one(p_file_stat_base,file_stat_in_list_type,is_cache_file)){
		printk("%ps->can_file_stat_move file_stat:0x%llx status:0x%x is_cache_file:%d file_stat_in_list_type_bit:%d error\n",__builtin_return_address(0),(u64)p_file_stat_base,p_file_stat_base->file_stat_status,is_cache_file,file_stat_in_list_type);
		return 0;
	}

	/*检测p_file_stat_base在链表的下一个成员是否合法。如果它是链表头，就不检测了*/
	if(file_stat_temp_head != &p_file_stat_base_next->hot_cold_file_list){
		if(can_file_stat_move_to_list_head_for_one(p_file_stat_base_next,file_stat_in_list_type,is_cache_file)){
			printk("%ps->can_file_stat_move file_stat:0x%llx file_stat_next:0x%llx status:0x%x is_cache_file:%d file_stat_in_list_type_bit:%d error!!!\n",__builtin_return_address(0),(u64)p_file_stat_base,(u64)p_file_stat_base_next,p_file_stat_base_next->file_stat_status,is_cache_file,file_stat_in_list_type);
			return 0;
		}
	}

	/*检测p_file_stat_base在链表的上一个成员是否合法。它不可能是链表头，如果链表只有一个成员，prev是链表头但上边的if就会跳过。
	 *错了，还有一种情况，链表头有很多成员，p_file_stat_base是第一个成员，此时p_file_stat_base_prev就是链表头，因此还是要判断
	 *p_file_stat_base_prev是否是链表头*/
	if(file_stat_temp_head != &p_file_stat_base_prev->hot_cold_file_list){
		if(can_file_stat_move_to_list_head_for_one(p_file_stat_base_prev,file_stat_in_list_type,is_cache_file)){
			printk("%ps->can_file_stat_move file_stat:0x%llx file_stat_prev:0x%llx status:0x%x is_cache_file:%d file_stat_in_list_type_bit:%d error!!!\n",__builtin_return_address(0),(u64)p_file_stat_base,(u64)p_file_stat_base_prev,p_file_stat_base_prev->file_stat_status,is_cache_file,file_stat_in_list_type);
			return 0;
		}
	}

	if(shrink_page_printk_open1)
		printk("can_file_stat_move_to_list_head file_stat:0x%llx\n",(u64)p_file_stat_base);
	return 1;
}
#if 0
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
#endif
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

static void inline set_file_area_in_deleted(struct file_area *p_file_area)
{
	p_file_area->file_area_state = -1;
}
static char inline file_area_in_deleted(struct file_area *p_file_area)
{
	return (p_file_area->file_area_state == -1);
}
#if 0
/* 如果file_area被iput()释放了，原本是想同时设置file_area的in_free、in_temp、in_refault、in_hot等状态，表示该file_area
 * 被iput()释放了，但是就引入了新的问题：如果该file_area此时正被异步内存回收线程遍历，发现file_area同时具备in_free、in_temp等
 * 状态，因状态不对而触发panic，类似问题很多。这是个并发问题，不要解决。干脆file_area->mapping赋值NULL表示file_area被iput了*/
static void inline set_file_area_in_mapping_delete(struct file_area *p_file_area)
{
	/*正常file_area不可能同时存在in_temp和in_free标记，暂时以二者标记file_area的in_mapping_delete标记*/
	set_file_area_in_temp_list(p_file_area);
	set_file_area_in_free_list(p_file_area);
}
static char inline file_area_in_mapping_delete(struct file_area *p_file_area)
{
	return file_area_in_temp_list(p_file_area) && file_area_in_free_list(p_file_area);
}
#else
static void inline set_file_area_in_mapping_delete(struct file_area *p_file_area)
{
	//p_file_area->mapping = NULL;
	set_file_area_in_mapping_exit(p_file_area);
}
static char inline file_area_in_mapping_delete(struct file_area *p_file_area)
{
	/* 非global_file_stat的正常的file_stat的file_area也会在iput()时把p_file_area->mapping设置NULL，这会导致这些file_stat的file_area被误判
	 * 为有file_area_in_mapping_delete标记，造成误判，把把个file_area移动到global_file_stat_delete链表，大错特错，因此要去掉(p_file_area->mapping == NULL)*/
	//return file_area_in_mapping_exit(p_file_area) || (p_file_area->mapping == NULL);
	return file_area_in_mapping_exit(p_file_area);
}
#endif
#if 0
/* 当文件iput时，针对没有page的file_area，要把file_area移动到global_file_stat_delete链表，用的是file_area的
 * file_area_delete成员，本质就是file_area->page[0/1]。此时二者要么是NULL，要么是xa_is_value。不可能是page
 * 指针，如果是page指针，这里就要crash，负责就会覆盖掉page->page[0/1]里保存的page指针*/
static void inline check_file_area_delete_list_is_not_page(struct file_area *p_file_area)
{
   if((p_file_area->file_area_delete.prev && !xa_is_value(p_file_area->file_area_delete.prev)) ||
		   (p_file_area->file_area_delete.next && !xa_is_value(p_file_area->file_area_delete.next)))
	   panic("check_file_area_delete_list_is_not_page file_area:0x%llx error\n",(u64)p_file_area);
}
static void inline move_file_area_to_global_delete_list(struct file_stat_base *p_file_stat_base,struct file_area *p_file_area)
{
	if(file_stat_in_cache_file_base(p_file_stat_base)){
		spin_lock(&hot_cold_file_global_info.global_file_stat.file_area_delete_lock);
		/* 这个判断是多余的，一个file_area只可能添加到global_file_stat_delete_list链表一次，第一次进来，file_area->page[0/1]要么是0，
		 * 表示page被释放了。要么bit0是1，表示被异步内存回收或kswapd线程回收而在file_area->page[0/1]做了标记。只可能是以上情况，
		 * 如果不是，说明file_area被重复添加到了global_file_stat_delete_list链表，这是异常的，要在
		 * check_file_area_delete_list_is_not_page函数里panic，不能在这里做if限制，也没这个必要。于是把set_file_area_in_mapping_delete()
		 * 放到iput()流程，检测到file_area没有page，立即对file_area进行set_file_area_in_mapping_delete()*/
		//if(!file_area_in_mapping_delete(p_file_area)){
			check_file_area_delete_list_is_not_page(p_file_area);

			/* 写代码稍微不过脑子就犯了错，file_area->file_area_delete的next和prev来自file_area->page[0/1]，默认是0，
			 * 根本就没有添加到其他链表，故不能用list_move，而是list_add，首次添加到其他链表用list_add*/
			//list_move(&(*p_file_area)->file_area_delete,&hot_cold_file_global_info.global_file_stat.file_area_delete_list);
			list_add(&p_file_area->file_area_delete,&hot_cold_file_global_info.global_file_stat.file_area_delete_list);
			//set_file_area_in_mapping_delete(p_file_area);
		//}
		spin_unlock(&hot_cold_file_global_info.global_file_stat.file_area_delete_lock);
	}
	else{
		spin_lock(&hot_cold_file_global_info.global_mmap_file_stat.file_area_delete_lock);
		//if(!file_area_in_mapping_delete(p_file_area)){
			check_file_area_delete_list_is_not_page(p_file_area);

			//list_move(&(*p_file_area)->file_area_delete,&hot_cold_file_global_info.global_mmap_file_stat.file_area_delete_list);
			list_add(&p_file_area->file_area_delete,&hot_cold_file_global_info.global_mmap_file_stat.file_area_delete_list);
			//set_file_area_in_mapping_delete(p_file_area);
		//}
		spin_unlock(&hot_cold_file_global_info.global_mmap_file_stat.file_area_delete_lock);
	}
}
#endif
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

//extern void can_tiny_small_file_change_to_small_normal_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_tiny_small *p_file_stat_tiny_small,char is_cache_file);
//extern void can_small_file_change_to_normal_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_small *p_file_stat_small,char is_cache_file);
extern int reverse_other_file_area_list_common(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,unsigned int file_area_type,unsigned int file_type,struct list_head *file_area_list);

inline void hot_file_update_file_status(struct address_space *mapping,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,int read_or_write);
//extern void hot_file_update_file_status(struct address_space *mapping,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,int access_count,int read_or_write/*,unsigned long index*/);
extern char *get_file_name_buf(char *file_name_path,struct file_stat_base *p_file_stat_base);
extern int get_file_name_match(struct file_stat_base *p_file_stat_base,char *file_name1,char *file_name2,char *file_name3); 
extern char *get_file_name_no_lock_from_mapping(struct address_space *mapping);
//extern unsigned long cold_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *file_area_free,struct list_head *file_area_have_mmap_page_head);
extern unsigned int cold_mmap_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base * p_file_stat_base,struct file_area *p_file_area,struct page *page_buf[],int cold_page_count);
extern unsigned long shrink_inactive_list_async(unsigned long nr_to_scan, struct lruvec *lruvec,struct hot_cold_file_global *p_hot_cold_file_global,int is_mmap_file, enum lru_list lru);
extern int walk_throuth_all_mmap_file_area(struct hot_cold_file_global *p_hot_cold_file_global);
//extern int cold_mmap_file_stat_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat_del);
extern unsigned int cold_file_stat_delete_all_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int file_type,char is_cache_file);
extern int cold_file_stat_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base_del,unsigned int file_type);
extern int cold_file_area_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area);

extern void file_stat_temp_middle_large_file_change(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int file_stat_list_type, unsigned int normal_file_type,char is_cache_file);
extern int mmap_file_area_cache_page_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *file_area_have_cache_page_head,struct list_head *file_area_free_temp,unsigned int file_type);
extern int cache_file_area_mmap_page_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *file_area_have_mmap_page_head,unsigned int file_type);
extern int check_file_stat_is_valid(struct file_stat_base *p_file_stat_base,unsigned int file_stat_list_type,char is_cache_file);
extern noinline int hot_cold_file_print_all_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct seq_file *m,int is_proc_print,int print_file_stat_info_or_update_refault);
extern noinline void printk_shrink_param(struct hot_cold_file_global *p_hot_cold_file_global,struct seq_file *m,int is_proc_print);
extern int hot_cold_file_thread(void *p);
extern int async_memory_reclaim_main_thread(void *p);
#endif
