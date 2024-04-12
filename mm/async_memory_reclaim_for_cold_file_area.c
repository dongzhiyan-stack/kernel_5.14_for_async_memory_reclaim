#include "async_memory_reclaim_for_cold_file_area.h"

struct hot_cold_file_global hot_cold_file_global_info;
//置1会把内存回收信息详细打印出来
int shrink_page_printk_open1 = 0;
//不怎么关键的调试信息
int shrink_page_printk_open = 0;
unsigned long async_memory_reclaim_status = 1;

unsigned long file_area_in_update_count;
unsigned long file_area_in_update_lock_count;

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
/* 统计page的冷热信息、是否refault。返回0表示page和file_area没有，需要分配；返回1是成功返回；返回负数有出错
 *
 * */
#if 1
int hot_file_update_file_status(struct address_space *mapping,struct file_stat *p_file_stat,struct file_area *p_file_area,int access_count)
{
	//检测file_area被访问的次数，判断是否有必要移动到file_stat->hot、refault、temp等链表头
	int file_area_move_list_head = is_file_area_move_list_head(p_file_area);
    
	file_area_in_update_count ++;
	/*hot_cold_file_global_info.global_age更新了，把最新的global age更新到本次访问的file_area->file_area_age。并对
	 * file_area->access_count清0，本周期被访问1次则加1.这段代码不管理会并发，只是一个赋值*/
	if(p_file_area->file_area_age < hot_cold_file_global_info.global_age){
		p_file_area->file_area_age = hot_cold_file_global_info.global_age;

		/*file_area访问计数清0，这个必须放到is_file_area_move_list_head()后边，因为is_file_area_move_list_head()依赖这个访问计数*/
		if(file_area_access_count_get(p_file_area))
		    file_area_access_count_clear(p_file_area);
	}

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

					/*这段代码测试热文件的，后续删除掉!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
					 *!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
					 *!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
					if(p_file_stat->file_area_hot_count > 100){
                        spin_lock(&hot_cold_file_global_info.global_lock);
						set_file_stat_in_file_stat_hot_head_list(p_file_stat);
						list_move(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.file_stat_hot_head);
						spin_unlock(&hot_cold_file_global_info.global_lock);
						p_file_stat->file_stat_hot_base_age = hot_cold_file_global_info.global_age;
					}
				}
			}else{
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
#else
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

static int walk_throuth_all_file_area(struct hot_cold_file_global *p_hot_cold_file_global)
{
#if 0	
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
			&p_hot_cold_file_global->file_stat_temp_large_file_head,&file_stat_free_list_from_head_temp_large);
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
	nr_reclaimed =  free_page_from_file_area(p_hot_cold_file_global,&file_stat_free_list_from_head_temp_large,&p_hot_cold_file_global->file_stat_temp_large_file_head); 
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
				list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->file_stat_temp_large_file_head);
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

		del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat);
		del_file_stat_count ++;
	}
	//file_stat的hot链表转移到temp链表的file_area个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.file_area_hot_to_temp_list_count2 = file_area_hot_to_temp_list_count;
	//释放的file_area个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.del_file_area_count = del_file_area_count;
	//释放的file_stat个数
	p_hot_cold_file_global->hot_cold_file_shrink_counter.del_file_stat_count = del_file_stat_count;

	if(0 == test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		return 0;

	//对没有file_area的file_stat的处理
	file_stat_has_zero_file_area_manage(p_hot_cold_file_global);

	//如果此时echo 触发了drop_cache，ASYNC_DROP_CACHES置1，则禁止异步内存回收线程处理global drop_cache_file_stat_head链表上的file_stat
	if(!test_bit(ASYNC_DROP_CACHES, &async_memory_reclaim_status))
	    //处理drop cache的文件的pagecache
	    drop_cache_truncate_inode_pages(p_hot_cold_file_global);

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
#endif
	return 0;
}

static int hot_cold_file_thread(void *p){
	struct hot_cold_file_global *p_hot_cold_file_global = (struct hot_cold_file_global *)p;
	int sleep_count = 0;

	while(1){
		sleep_count = 0;
		while(sleep_count ++ < p_hot_cold_file_global->global_age_period){
			if (kthread_should_stop())
				return 0;
			msleep(1000);
		}
		if(test_bit(ASYNC_MEMORY_RECLAIM_ENABLE, &async_memory_reclaim_status))
		{
	        //每个周期global_age加1
	        hot_cold_file_global_info.global_age ++;
			file_area_in_update_lock_count = 0;
			file_area_in_update_count = 0;
			walk_throuth_all_file_area(p_hot_cold_file_global);
			//walk_throuth_all_mmap_file_area(p_hot_cold_file_global);
		}
	}
	return 0;
}


static int __init hot_cold_file_init(void)
{
	hot_cold_file_global_info.file_stat_cachep = kmem_cache_create("file_stat",sizeof(struct file_stat),0,0,NULL);
	hot_cold_file_global_info.file_area_cachep = kmem_cache_create("file_area",sizeof(struct file_area),0,0,NULL);

	if(!hot_cold_file_global_info.file_stat_cachep || !hot_cold_file_global_info.file_area_cachep){
	    printk("%s slab 0x%llx 0x%llx error\n",__func__,(u64)hot_cold_file_global_info.file_stat_cachep,(u64)hot_cold_file_global_info.file_area_cachep);
	    return -1;
	}
	
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_hot_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_temp_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_temp_large_file_head);

	INIT_LIST_HEAD(&hot_cold_file_global_info.cold_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_delete_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.file_stat_zero_file_area_head);

	INIT_LIST_HEAD(&hot_cold_file_global_info.drop_cache_file_stat_head);

	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_temp_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_zero_file_area_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_delete_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_temp_large_file_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_hot_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_uninit_head);
	INIT_LIST_HEAD(&hot_cold_file_global_info.mmap_file_stat_mapcount_head);

	spin_lock_init(&hot_cold_file_global_info.global_lock);
	spin_lock_init(&hot_cold_file_global_info.mmap_file_global_lock);

	//atomic_set(&hot_cold_file_global_info.ref_count,0);
	//atomic_set(&hot_cold_file_global_info.inode_del_count,0);

	hot_cold_file_global_info.file_area_hot_to_temp_age_dx = FILE_AREA_HOT_to_TEMP_AGE_DX;
	hot_cold_file_global_info.file_area_refault_to_temp_age_dx = FILE_AREA_REFAULT_TO_TEMP_AGE_DX;
	hot_cold_file_global_info.file_area_temp_to_cold_age_dx = FILE_AREA_TEMP_TO_COLD_AGE_DX;
	hot_cold_file_global_info.file_area_free_age_dx = FILE_AREA_FREE_AGE_DX;
	hot_cold_file_global_info.file_stat_delete_age_dx  = FILE_STAT_DELETE_AGE_DX;
	hot_cold_file_global_info.global_age_period = ASYNC_MEMORY_RECLIAIM_PERIOD;

	//256M的page cache对应file_area个数，被判定为大文件
	hot_cold_file_global_info.file_area_level_for_large_file = (256*1024*1024)/(4096 *PAGE_COUNT_IN_AREA);
	//mmap的文件，文件页超过50M就判定为大文件
	hot_cold_file_global_info.mmap_file_area_level_for_large_file = (50*1024*1024)/(4096 *PAGE_COUNT_IN_AREA);
	//64K对应的page数
	hot_cold_file_global_info.nr_pages_level = 16;

	
	hot_cold_file_global_info.hot_cold_file_thead = kthread_run(hot_cold_file_thread,&hot_cold_file_global_info, "hot_cold_file_thread");
	if (IS_ERR(hot_cold_file_global_info.hot_cold_file_thead)) {
		printk("Failed to start  hot_cold_file_thead\n");
		return -1;

	}
	
    return 0;
}
subsys_initcall(hot_cold_file_init);
