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

#define BUF_PAGE_COUNT (PAGE_COUNT_IN_AREA * 8)
#define SCAN_PAGE_COUNT_ONCE (PAGE_COUNT_IN_AREA * 8)

#define FILE_AREA_REFAULT 0
#define FILE_AREA_FREE 1
#define FILE_AREA_MAPCOUNT 2
#define FILE_AREA_HOT 3

//文件page扫描过一次后，去radix tree扫描空洞page时，一次在保存file_area的radix tree上扫描的node节点个数，一个节点64个file_area
#define SCAN_FILE_AREA_NODE_COUNT 2
#define FILE_AREA_PER_NODE TREE_MAP_SIZE

#define FILE_STAT_SCAN_ALL_FILE_AREA 0
#define FILE_STAT_SCAN_MAX_FILE_AREA 1
#define FILE_STAT_SCAN_IN_COOLING 2

//每次扫描文件file_stat的热file_area个数
#define SCAN_HOT_FILE_AREA_COUNT_ONCE 8
//每次扫描文件file_stat的mapcount file_area个数
#define SCAN_MAPCOUNT_FILE_AREA_COUNT_ONCE 8
//当扫描完一轮文件file_stat的temp链表上的file_area时，进入冷却期，在MMAP_FILE_AREA_COLD_AGE_DX个age周期内不再扫描这个文件上的file_area
#define MMAP_FILE_AREA_COLD_AGE_DX 5


unsigned int cold_mmap_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base * p_file_stat_base,struct file_area *p_file_area,struct page *page_buf[],int cold_page_count)
{
	unsigned int isolate_pages = 0;
	int i,traverse_page_count;
	struct page *page;
	//isolate_mode_t mode = ISOLATE_UNMAPPED;
	pg_data_t *pgdat = NULL;
	unsigned int move_page_count = 0;
	struct lruvec *lruvec = NULL,*lruvec_new = NULL;
	//unsigned long nr_reclaimed = 0;

	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx cold_page_count:%d\n",__func__,(u64)p_file_stat_base,cold_page_count);

	traverse_page_count = 0;
	/* 对file_stat加锁。在执行该函数前，已经在遍历该file_stat前执行的traverse_mmap_file_stat_get_cold_page()
	 * 函数里，执行了file_inode_lock()对inode和file_stat加锁，因为在check_one_file_area_cold_page_and_clear()
	 * 中要通过file_stat->pages[]数组遍历page.这里内存回收前再加锁其实也没事，无非是inode引用计数加1，但可靠*/
	//lock_file_stat(p_file_stat,0);
	if(0 == file_inode_lock(p_file_stat_base))
		return 0;

	/*执行到这里，就不用担心该inode会被其他进程iput释放掉*/

#if 0
	//如果文件inode和mapping已经释放了，则不能再使用mapping了，必须直接return
	if(file_stat_in_delete(p_file_stat) || (NULL == p_file_stat->mapping)){
		if(shrink_page_printk_open)
			printk("2:%s file_stat:0x%llx %d_0x%llx\n",__func__,(u64)p_file_stat,file_stat_in_delete(p_file_stat),(u64)p_file_stat->mapping);

		//如果异常退出，也要对page unlock
		for(i = 0; i< cold_page_count;i ++)
		{
			page = page_buf[i];
			if(page)
				unlock_page(page);
			else
				panic("%s page error\n",__func__);
		}
		goto err;
	}
#endif	

	/*read/write系统调用的pagecache的内存回收执行的cold_file_isolate_lru_pages()函数里里，对此时并发文件inode被delete做了严格防护，这里
	 * 对mamp的pagecache是否也需要防护并发inode被delete呢？突然觉得没有必要呀？因为文件还有文件页page没有被释放呀，就是这里正在回收的
	 * 文件页！这种情况文件inode可能会被delete吗？不会吧，必须得等文件的文件页全部被回收，才可能释放文件inode吧??????????????????*/
	for(i = 0; i< cold_page_count;i ++)
	{
		page = page_buf[i];
		if(shrink_page_printk_open)
			printk("3:%s file_stat:0x%llx file_area:0x%llx page:0x%llx\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,(u64)page);

		//此时page肯定是加锁状态，否则就主动触发crash
		if(!test_bit(PG_locked,&page->flags)){
			panic("%s page:0x%llx page->flags:0x%lx\n",__func__,(u64)page,page->flags);
		}

		if(traverse_page_count++ > 32){
			traverse_page_count = 0;
			//使用 lruvec->lru_lock 锁，且有进程阻塞在这把锁上
			if(lruvec && (spin_is_contended(&lruvec->lru_lock) || need_resched())){
				spin_unlock_irq(&lruvec->lru_lock); 
				cond_resched();
				//msleep(5);

				spin_lock_irq(&lruvec->lru_lock);
				//p_hot_cold_file_global->hot_cold_file_shrink_counter.lru_lock_contended_count ++;
			}
		}
#if 0	
		/*到这里的page，是已经pagelock的，这里就不用再pagelock了*/
		if(unlikely(pgdat != page_pgdat(page)))
		{
			//第一次进入这个if，pgdat是NULL，此时不用spin unlock，只有后续的page才需要
			if(pgdat){
				//对之前page所属pgdat进行spin unlock
				spin_unlock_irq(&pgdat->lru_lock);
				//多次开关锁次数加1
				p_hot_cold_file_global->mmap_file_lru_lock_count++;
			}
			//pgdat最新的page所属node节点对应的pgdat
			pgdat = page_pgdat(page);
			if(pgdat != p_hot_cold_file_global->p_hot_cold_file_node_pgdat[pgdat->node_id].pgdat)
				panic("pgdat not equal\n");
			//对新的page所属的pgdat进行spin lock。内核遍历lru链表都是关闭中断的，这里也关闭中断
			spin_lock_irq(&pgdat->lru_lock);
		}
#endif

		if (page && !xa_is_value(page)) {
			/*如果page映射了也表页目录，这是异常的，要给出告警信息!!!!!!!!!!!!!!!!!!!还有其他异常状态*/
			if (unlikely(PageAnon(page))|| unlikely(PageCompound(page)) || unlikely(PageSwapBacked(page))){
				panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)page,page->flags);
			}

			//第一次循环，lruvec是NULL，则先加锁。并对lruvec赋值，这样下边的if才不会成立，然后误触发内存回收，此时还没有move page到inactive lru链表
			if(NULL == lruvec){
				lruvec_new = mem_cgroup_lruvec(page_memcg(page),page_pgdat(page));
				lruvec = lruvec_new;
				spin_lock_irq(&lruvec->lru_lock);
			}else{
				lruvec_new = mem_cgroup_lruvec(page_memcg(page),page_pgdat(page));
			}

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
				isolate_pages += shrink_inactive_list_async(move_page_count,lruvec,p_hot_cold_file_global,1,LRU_INACTIVE_FILE);

				//回收后对move_page_count清0
				move_page_count = 0;

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

	//file_stat解锁
	//unlock_file_stat(p_file_stat);
	file_inode_unlock(p_file_stat_base);

	//当函数退出时，如果move_page_count大于0，则强制回收这些page
	if(move_page_count > 0){
		if(lruvec)
			spin_unlock_irq(&lruvec->lru_lock);
		//回收inactive lru链表尾的page，这些page刚才才移动到inactive lru链表尾
		isolate_pages += shrink_inactive_list_async(move_page_count,lruvec,p_hot_cold_file_global,1,LRU_INACTIVE_FILE);

	}else{
		if(lruvec)
			spin_unlock_irq(&lruvec->lru_lock);
	}

	return isolate_pages;
}

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
    if(p_file_stat->file_area_hot_count > (p_file_stat->file_area_count - (p_file_stat->file_area_count >> 3)))
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
static int inline is_mmap_file_stat_file_type(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat)
{
	if(p_file_stat->file_area_count < hot_cold_file_global_info.mmap_file_area_level_for_middle_file)
		return TEMP_FILE;
	else if(p_file_stat->file_area_count < hot_cold_file_global_info.mmap_file_area_level_for_large_file)
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
    if(p_file_stat->mapcount_file_area_count > (p_file_stat->file_area_count - (p_file_stat->file_area_count >> 3)))
		ret  = 1;
	else
		ret =  0;
#endif	
	return ret;
}
#if 0
//mmap的文件页page，内存回收失败，测试发现都是被访问页表pte置位了，则把这些page移动到file_stat->refault链表
static int  solve_reclaim_fail_page(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,struct list_head *page_list)
{
	struct page *page;
	pgoff_t last_index,area_index_for_page;
	struct file_area *p_file_area;
	void **page_slot_in_tree = NULL;
	struct hot_cold_file_area_tree_node *parent_node;

	last_index = (unsigned long)-1;
	list_for_each_entry(page,page_list,lru){

		area_index_for_page = page->index >> PAGE_COUNT_IN_AREA_SHIFT;
		//前后两个page都属于同一个file_area
		if(last_index == area_index_for_page)
			continue;

		last_index = area_index_for_page;
		parent_node = hot_cold_file_area_tree_lookup(&p_file_stat->hot_cold_file_area_tree_root_node,area_index_for_page,&page_slot_in_tree);
		if(IS_ERR(parent_node) || NULL == *page_slot_in_tree){
			panic("2:%s hot_cold_file_area_tree_lookup_and_create fail parent_node:0x%llx page_slot_in_tree:0x%llx\n",__func__,(u64)parent_node,(u64)page_slot_in_tree);
		}
		p_file_area = (struct file_area *)(*page_slot_in_tree);
		/*有可能前边的循环已经把这个file_area移动到refault链表了，那此时if不成立*/
		if(file_area_in_free_list(p_file_area)){
			if(file_area_in_free_list_error(p_file_area)){
				panic("%s file_area:0x%llx status:%d not in file_area_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
			}

			/*file_area的page在内存回收时被访问了，file_area移动到refault链表。但如果page的mapcount大于1，那要移动到file_area_mapcount链表*/
			if(page_mapcount(page) == 1){
			    clear_file_area_in_free_list(p_file_area);
			    set_file_area_in_refault_list(p_file_area);
			    list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
				if(shrink_page_printk_open1)
					printk("%s page:0x%llx file_area:0x%llx status:%d move to refault list\n",__func__,(u64)page,(u64)p_file_area,p_file_area->file_area_state);
			}
			else{
				p_file_stat->mapcount_file_area_count ++;
			    //file_area的page的mapcount大于1，则把file_area移动到file_stat->file_area_mapcount链表
			    clear_file_area_in_free_list(p_file_area);
			    set_file_area_in_mapcount_list(p_file_area);
			    list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);
				if(shrink_page_printk_open1)
					printk("%s page:0x%llx file_area:0x%llx status:%d move to mapcount list\n",__func__,(u64)page,(u64)p_file_area,p_file_area->file_area_state);

				/*如果文件file_stat的mapcount的file_area个数超过阀值，则file_stat被判定为mapcount file_stat而移动到
			    *global mmap_file_stat_mapcount_head链表。但前提file_stat必须在temp_file链表或temp_large_file链表*/
				if(is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat) && file_stat_in_file_stat_temp_head_list(p_file_stat)){
					 if(file_stat_in_file_stat_temp_head_list_error(p_file_stat))
						 panic("%s file_stat:0x%llx status error:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

					 clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
					 set_file_stat_in_mapcount_file_area_list(p_file_stat);
					 list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_mapcount_head);
					 p_hot_cold_file_global->mapcount_mmap_file_stat_count ++;
					 if(shrink_page_printk_open1)
						 printk("%s file_stat:0x%llx status:0x%llx is mapcount file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
				}
			}
		}
	}
	return 0;
}
#endif
#if 0
/*已经在cold_file_stat_delete_quick()做了针对mmap file_stat的delete*/
int  cold_mmap_file_stat_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat_del)
{  

	//spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);-----有了global->mmap_file_stat_uninit_head链表后，从global temp删除file_stat，不用再加锁

	//p_file_stat_del->mapping = NULL;多余操作
	clear_file_stat_in_file_stat_temp_head_list(p_file_stat_del);
	list_del(&p_file_stat_del->hot_cold_file_list);
	//差点忘了释放file_stat结构，不然就内存泄漏了!!!!!!!!!!!!!!
	kmem_cache_free(p_hot_cold_file_global->file_stat_cachep,p_file_stat_del);
	hot_cold_file_global_info.mmap_file_stat_count --;

	//spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

	return 0;
}
EXPORT_SYMBOL(cold_mmap_file_stat_delete);
#endif

#if 0 //第二个版本，代码有点繁琐，舍弃，改进
static  unsigned int check_one_file_area_cold_page_and_clear(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct file_area *p_file_area,struct page *page_buf[],int *cold_page_count)
{
	unsigned long vm_flags;
	int ret = 0;
	struct page *page;
	struct folio *folio;
	unsigned cold_page_count_temp = 0;
	int i,j;
	struct address_space *mapping = p_file_stat->mapping;
	int file_area_cold = 0;
	struct page *pages[PAGE_COUNT_IN_AREA];
	int mapcount_file_area = 0;
	int file_area_is_hot = 0;

	//file_area已经很长一段时间没有被访问则file_area_cold置1，只有在这个大前提下，file_area的page pte没有被访问，才能回收page
	if(p_hot_cold_file_global->global_age - p_file_area->file_area_age >  MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX)
		file_area_cold = 1;

	if(cold_page_count)
		cold_page_count_temp = *cold_page_count;

	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx file_area:0x%llx get %d page\n",__func__,(u64)p_file_stat,(u64)p_file_area,ret);

	if(0 == file_area_have_page(p_file_area))
	    goto out; 

	//ret必须清0，否则会影响下边ret += page_referenced_async的ret大于0，误判page被访问pte置位了
	ret = 0;
	for(i = 0;i < PAGE_COUNT_IN_AREA;i ++){
		//page = xa_load(&mapping->i_pages, p_file_area->start_index + i);
		folio = p_file_area->pages[i];
		page = &folio->page;
		/*这里判断并清理 映射page的页表页目录pte的access bit，是否有必要对page lock_page加锁呢?需要加锁*/
		if (page && !xa_is_value(page)) {
			/*对page上锁，上锁失败就休眠，这里回收mmap的文件页的异步内存回收线程，这里没有加锁且对性能没有要求，可以休眠
			 *到底用lock_page还是trylock_page？如果trylock_page失败的话，说明这个page最近被访问了，那肯定不是冷page，就不用执行
			 *下边的page_referenced检测page的 pte了，浪费性能。??????????????????????????????????????????????????
			 *为什么用trylock_page呢？因为page_lock了实际有两种情况 1：其他进程访问这个page然后lock_page，2：其他进程内存回收
			 *这个page然后lock_pagea。后者page并不是被其他进程被访问而lock了！因此只能用lock_page了，然后再
			 *page_referenced判断page pte，因为这个page可能被其他进程内存回收而lock_page，并不是被访问了lock_page
			 */
			if(shrink_page_printk_open)
				printk("2:%s page:0x%llx index:%ld %ld_%d\n",__func__,(u64)page,page->index,p_file_area->start_index,i);
			lock_page(page);
			//if(trylock_page(page))------不要删
			{
				/*如果page被其他进程回收了，if不成立，这些就不再对该file_area的其他page进行内存回收了，其实
				 *也可以回收，但是处理起来很麻烦，后期再考虑优化优化细节吧!!!!!!!!!!!!!!!!!!!!!!*/
				if(page->mapping != mapping){
					if(shrink_page_printk_open1)
						printk("3:%s file_stat:0x%llx file_area:0x%llx status:0x%x page->mapping != mapping!!!!!!!!!\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

					unlock_page(page);
					continue;
				}
				/*如果page不是mmap的要跳过。一个文件可能是cache文件，同时也被mmap映射，因此这类的文件页page可能不是mmap的，只是cache page
				 *这个判断必须放到lock_page后边*/
				if (!page_mapped(page)){
					unlock_page(page);
					if(shrink_page_printk_open1)
						printk("4:%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx not in page_mapped error!!!!!!\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,(u64)page);

					continue;
				}
				
				if(0 == mapcount_file_area && page_mapcount(page) > 1)
					mapcount_file_area = 1;

				//检测映射page的页表pte access bit是否置位了，是的话返回1并清除pte access bit。错了，不是返回1，是反应映射page的进程个数
				/*page_referenced函数第2个参数是0里边会自动执行lock page()。这个到底要传入folio还是page????????????????????????????????*/
				ret += folio_referenced(page_folio(page), 1, page_memcg(page),&vm_flags);
				
				if(shrink_page_printk_open)
					printk("5:%s file_stat:0x%llx file_area:0x%llx page:0x%llx index:%ld file_area_cold:%d cold_page_count:%d ret:%d page_mapcount:%d access_count:%d\n",__func__,(u64)p_file_stat,(u64)p_file_area,(u64)page,page->index,file_area_cold,cold_page_count == NULL ?-1:*cold_page_count,ret,page_mapcount(page),file_area_access_count_get(p_file_area));

				/*ret大于0说明page最近被访问了，不是冷page，则赋值全局age*/
				if(ret > 0){
					unlock_page(page);
					//本次file_area已经被判定为热file_area了，continue然后遍历下一个page
					if(file_area_is_hot)
						continue;
					file_area_is_hot = 1;

					//不能放在这里，这样二者就相等了,if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <= MMAP_FILE_AREA_HOT_AGE_DX)永远成立
					//p_file_area->file_area_age = p_hot_cold_file_global->global_age;

					/*file_area必须在temp_list链表再令file_area的access_count加1，如果在固定周期内file_area的page被访问次数超过阀值，就判定为热file_area。
					 *file_area可能也在refault_list、free_list也会执行到这个函数，要过滤掉*/
					if(file_area_in_temp_list(p_file_area)){
						//file_area如果在 MMAP_FILE_AREA_HOT_AGE_DX 周期内被检测到访问 MMAP_FILE_AREA_HOT_DX 次，file_area被判定为热file_area
						if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <= MMAP_FILE_AREA_HOT_AGE_DX){

						    //file_area的page被访问了，file_area的access_count加1
						    file_area_access_count_add(p_file_area,1);
							//在规定周期内file_area被访问次数大于MMAP_FILE_AREA_HOT_DX则file_area被判定为热file_area
							if(file_area_access_count_get(p_file_area) > MMAP_FILE_AREA_HOT_DX){
								//被判定为热file_area后，对file_area的access_count清0
								file_area_access_count_clear(p_file_area);

								spin_lock(&p_file_stat->file_stat_lock);
								//file_stat->temp 链表上的file_area个数减1
								p_file_stat->file_area_count_in_temp_list --;
								//file_area移动到hot链表
								clear_file_area_in_temp_list(p_file_area);
								set_file_area_in_hot_list(p_file_area);
								list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
								spin_unlock(&p_file_stat->file_stat_lock);

								//该文件的热file_area数加1
								p_file_stat->file_area_hot_count ++;
								if(shrink_page_printk_open)
									printk("6:%s file_stat:0x%llx file_area:0x%llx is hot status:0x%x\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

								//如果文件的热file_area个数超过阀值则被判定为热文件，文件file_stat移动到global mmap_file_stat_hot_head链表
								if(is_mmap_file_stat_hot_file(p_hot_cold_file_global,p_file_stat) && file_stat_in_file_stat_temp_head_list(p_file_stat)){
									spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
									clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
									set_file_stat_in_file_stat_hot_head_list(p_file_stat);
									list_move(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_hot_head);
									hot_cold_file_global_info.hot_mmap_file_stat_count ++;
									spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
									if(shrink_page_printk_open)
										printk("7:%s file_stat:0x%llx status:0x%llx is hot file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
								}
							}
						}else{
							//超过MMAP_FILE_AREA_HOT_AGE_DX个周期后对file_area访问计数清0
							file_area_access_count_clear(p_file_area);
						}
					}

					p_file_area->file_area_age = p_hot_cold_file_global->global_age;
					
					/*这里非常重要。当file_area的一个page发现最近访问过，不能break跳出循环。而是要继续循环把file_area剩下的page也执行
					 *page_referenced()清理掉page的pte access bit。否则，这些pte access bit置位的page会对file_area接下来的冷热造成
					 *重大误判。比如，file_area对应page0~page3，page的pte access bit全置位了。在global_age=1时，执行到该函数，这个for循环
					 *里执行page_referenced()判断出file_area的page0的pte access bit置位了，判断这个file_area最近访问过，然后自动清理掉page的
					 *pte access bit。等global_age=8,10,15时，依次又在该函数的for循环判断出page1、page2、page3的pte access bit置位了。这不仅
					 *导致误判该file_area是热的！实际情况是，page0~page3在global_age=1被访问过一次后就没有再被访问了，等到global_age=15正常
					 *要被判定为冷file_area而回收掉page。但实际却错误连续判定这个file_area一直被访问。解决方法注释掉break，换成continue，这样在
					 *global_age=1时，就会把page0~page3的pte access bit全清0，就不会影响后续判断了。但是这样性能损耗会增大，后续有打算
					 *只用file_area里的1个page判断冷热，不在扫描其他page*/
					//break;
					continue;
				}else{
					/*否则，file_area的page没有被访问，要不要立即就对file_area的access_count清0??????? 修改成，如过了规定周期file_area的page依然没被访问再对
					 *file_area的access_count清0*/
					if(file_area_in_temp_list(p_file_area) && (p_hot_cold_file_global->global_age - p_file_area->file_area_age > MMAP_FILE_AREA_HOT_AGE_DX)){
						file_area_access_count_clear(p_file_area);
					}
				}

				/*cold_page_count不是NULL说明此时遍历的是file_stat->file_area_temp链表上的file_area。否则，遍历的是
				 *file_stat->file_area_refault和file_stat->file_area_free链表上的file_area，使用完page就需要unlock_page*
				 *file_area_cold是1说明此file_area是冷的，file_area的page也没有被访问，然后才回收这个file_area的page*/
				if(cold_page_count != NULL && file_area_cold){
					if(*cold_page_count < BUF_PAGE_COUNT){

						if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <  MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX - 2)
							panic("%s file_stat:0x%llx status:0x%llx is hot ,can not reclaim\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);

						//冷page保存到page_buf[]，然后参与内存回收
						page_buf[*cold_page_count] = page;
						*cold_page_count = *cold_page_count + 1;
					}
					else
						panic("%s %d error\n",__func__,*cold_page_count);
				}else{
					unlock_page(page);
				}
		 }
			/*-------很重要，不要删
			  else{
			//到这个分支，说明page被其他先lock了。1：其他进程访问这个page然后lock_page，2：其他进程内存回收这个page然后lock_pagea。
			//到底要不要令ret加1呢？想来想去不能，于是上边把trylock_page(page)改成lock_page
			//ret += 1;
			}*/
		}
	}
   
	//必须是处于global temp链表上的file_stat->file_area_temp 链表上的file_area再判断是否是mapcountfile_area
	if(file_stat_in_file_stat_temp_head_list(p_file_stat) && file_area_in_temp_list(p_file_area)){
		/*如果上边for循环遍历的file_area的page的mapcount都是1，且file_area的page上边没有遍历完，则这里继续遍历完剩余的page*/
		while(0 == mapcount_file_area && i < PAGE_COUNT_IN_AREA){
			page= pages[i];
			//if (page && !xa_is_value(page) && page_mapped(page) && page_mapcount(page) > 1){
			if (page && page_mapped(page) && page_mapcount(page) > 1){
				mapcount_file_area = 1;
			}
			i ++;
		}
		if(mapcount_file_area){
			spin_lock(&p_file_stat->file_stat_lock);
			//file_stat->temp 链表上的file_area个数减1
			p_file_stat->file_area_count_in_temp_list --;
			//文件file_stat的mapcount的file_area个数加1
			p_file_stat->mapcount_file_area_count ++;
			//file_area的page的mapcount大于1，则把file_area移动到file_stat->file_area_mapcount链表
			clear_file_area_in_temp_list(p_file_area);
			set_file_area_in_mapcount_list(p_file_area);
			list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);
			spin_unlock(&p_file_stat->file_stat_lock);

			if(shrink_page_printk_open)
				printk("8:%s file_stat:0x%llx file_area:0x%llx state:0x%x temp to mapcount\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

			/*如果文件file_stat的mapcount的file_area个数超过阀值，则file_stat被判定为mapcount file_stat而移动到
			 *global mmap_file_stat_mapcount_head链表。但前提file_stat必须在temp_file链表或temp_large_file链表*/
			if(is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat) /*&& file_stat_in_file_stat_temp_head_list(p_file_stat)*/){
				 if(file_stat_in_file_stat_temp_head_list_error(p_file_stat))
					 panic("%s file_stat:0x%llx status error:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

				 spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
				 clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
				 set_file_stat_in_mapcount_file_area_list(p_file_stat);
				 list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_mapcount_head);
				 spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
				 p_hot_cold_file_global->mapcount_mmap_file_stat_count ++;
				 if(shrink_page_printk_open1)
					 printk("9:%s file_stat:0x%llx status:0x%llx is mapcount file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
			}
		}
    }

	/*到这里有这些可能
	 *1: file_area的page都是冷的，ret是0
	 *2: file_area的page有些被访问了，ret大于0
	 *3：file_area的page都是冷的，但是有些page前边trylock_page失败了，ret大于0。这种情况目前已经不可能了
	 *
	 *如果file_area被判定被mapcount file_area，即mapcount_file_area是1，则上边该file_area被判定是冷page而
	 *保存到page_buf[]数组的page也要清空，使无效，方法就是恢复cold_page_count最初的值即可，它指向最后一个
	 *保存到page_buf[]数组最后一个page的下标
	 */
	//历的是file_stat->file_area_temp链表上的file_area是if才成立
	if((ret > 0 || mapcount_file_area) && cold_page_count != NULL && file_area_cold){
		/*走到这里，说明file_area的page可能是热的，或者page_lock失败，那就不参与内存回收了。那就要对已加锁的page解锁*/
		//不回收该file_area的page，恢复cold_page_count
		*cold_page_count = cold_page_count_temp;
		/*解除上边加锁的page lock，cold_page_count ~ cold_page_count+i 的page上边加锁了，这里解锁*/
		for(j = 0 ;j < i;j++){
			page = page_buf[*cold_page_count + j];
			if(page){
				if(shrink_page_printk_open1)
					printk("10:%s file_stat:0x%llx file_area:0x%llx cold_page_count:%d page:0x%llx\n",__func__,(u64)p_file_stat,(u64)p_file_area,*cold_page_count,(u64)page);

				unlock_page(page);
			}
		}
	}
out:
	//返回值是file_area里4个page是热page的个数
	return ret;
}

#endif
/*如果file_area是热的，则把file_area移动到file_stat->hot链表。如果file_stat的热file_area个数超过阀值，则移动到global hot链表*/
static inline void check_hot_file_area_and_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,unsigned int file_stat_list_type,unsigned int file_type)
{
	struct file_stat *p_file_stat;

	//被判定为热file_area后，对file_area的access_count清0，防止干扰后续file_area冷热判断
	file_area_access_count_clear(p_file_area);

	/*小文件只是设置一个hot标记就return，不再把file_area移动到file_area_hot链表*/
	if(FILE_STAT_TINY_SMALL == file_type){
		clear_file_area_in_temp_list(p_file_area);
		set_file_area_in_hot_list(p_file_area);
		return; 
	}else if(FILE_STAT_SMALL == file_type){
		struct file_stat_small *p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);

		spin_lock(&p_file_stat_small->file_stat_lock);
		clear_file_area_in_temp_list(p_file_area);
		set_file_area_in_hot_list(p_file_area);
		list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
		//file_stat->temp 链表上的file_area个数减1
		p_file_stat->file_area_count_in_temp_list --;
		spin_unlock(&p_file_stat_small->file_stat_lock);
		return;
	}

	p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

	spin_lock(&p_file_stat->file_stat_lock);
	if(file_area_in_temp_list(p_file_area)){
		//file_stat->temp 链表上的file_area个数减1
		p_file_stat->file_area_count_in_temp_list --;
		clear_file_area_in_temp_list(p_file_area);
	}
	else if(file_area_in_warm_list(p_file_area))
		clear_file_area_in_warm_list(p_file_area);
	else
		panic("%s file_stat:0x%llx file_area:0x%llx status:%d not in error list\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

	set_file_area_in_hot_list(p_file_area);
	//file_area移动到hot链表
	list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
	spin_unlock(&p_file_stat->file_stat_lock);

	//该文件的热file_area数加1
	p_file_stat->file_area_hot_count ++;
	if(shrink_page_printk_open)
		printk("6:%s file_stat:0x%llx file_area:0x%llx is hot status:0x%x\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

	/* 如果文件的热file_area个数超过阀值则被判定为热文件，文件file_stat移动到global mmap_file_stat_hot_head链表。
	 * 但前提是文件file_stat只能在global temp、middle file、large_file链表上*/
	if(!file_stat_in_file_stat_hot_head_list(p_file_stat) && is_mmap_file_stat_hot_file(p_hot_cold_file_global,p_file_stat)){

		spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
		/*file_stat可能被iput()并发delete了并移动到global delete链表*/
		if(!file_stat_in_delete(p_file_stat)){
			if(F_file_stat_in_file_stat_temp_head_list == file_stat_list_type){
				if(!file_stat_in_file_stat_temp_head_list(p_file_stat) ||file_stat_in_file_stat_temp_head_list_error(p_file_stat))
					panic("%s file_stat:0x%llx status error:0x%lx not in temp_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

				clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			}
			else if(F_file_stat_in_file_stat_middle_file_head_list == file_stat_list_type){
				if(!file_stat_in_file_stat_middle_file_head_list(p_file_stat) || file_stat_in_file_stat_middle_file_head_list_error(p_file_stat))
					panic("%s file_stat:0x%llx status error:0x%lx not int middle_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

				clear_file_stat_in_file_stat_middle_file_head_list(p_file_stat);
			}
			else{
				if(F_file_stat_in_file_stat_large_file_head_list != file_stat_list_type)
					panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat,file_stat_list_type);

				if(!file_stat_in_file_stat_large_file_head_list(p_file_stat) ||file_stat_in_file_stat_large_file_head_list_error(p_file_stat))
					panic("%s file_stat:0x%llx status error:0x%lx not in large_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

				clear_file_stat_in_file_stat_large_file_head_list(p_file_stat);
			}

			set_file_stat_in_file_stat_hot_head_list(p_file_stat);
			list_move(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_hot_head);
			hot_cold_file_global_info.hot_mmap_file_stat_count ++;
			if(shrink_page_printk_open)
				printk("7:%s file_stat:0x%llx status:0x%llx is hot file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
		}
		spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
	}
}
/*如果file_area是mapcount的，则把file_area移动到file_stat->mapcount链表。如果file_stat的mapcount file_area个数超过阀值，则移动到global mapcount链表*/
static inline void check_mapcount_file_area_and_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,unsigned int file_stat_list_type,unsigned int file_type)
{
	struct file_stat *p_file_stat;

	//被判定为mapcount file_area后，对file_area的access_count清0，防止干扰后续file_area冷热判断
	file_area_access_count_clear(p_file_area);

	/*小文件只是设置一个mapcount标记就return，不再把file_area移动到file_area_mapcount链表*/
	if(FILE_STAT_TINY_SMALL == file_type){
		clear_file_area_in_temp_list(p_file_area);
		set_file_area_in_mapcount_list(p_file_area);
		return;
	}else if(FILE_STAT_SMALL == file_type){
		struct file_stat_small *p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);

		spin_lock(&p_file_stat_small->file_stat_lock);
		clear_file_area_in_temp_list(p_file_area);
		set_file_area_in_mapcount_list(p_file_area);
		list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);
		//file_stat->temp 链表上的file_area个数减1
		p_file_stat->file_area_count_in_temp_list --;
		spin_unlock(&p_file_stat_small->file_stat_lock);
	}

	p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

	spin_lock(&p_file_stat->file_stat_lock);
	//文件file_stat的mapcount的file_area个数加1
	p_file_stat->mapcount_file_area_count ++;
	if(file_area_in_temp_list(p_file_area)){
		//file_stat->temp 链表上的file_area个数减1
		p_file_stat->file_area_count_in_temp_list --;
		clear_file_area_in_temp_list(p_file_area);
	}
	else if(file_area_in_warm_list(p_file_area))
		clear_file_area_in_warm_list(p_file_area);
	else
		panic("%s file_stat:0x%llx file_area:0x%llx status:%d not in error list\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

	set_file_area_in_mapcount_list(p_file_area);
	//file_area的page的mapcount大于1，则把file_area移动到file_stat->file_area_mapcount链表
	list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);
	spin_unlock(&p_file_stat->file_stat_lock);

	if(shrink_page_printk_open)
		printk("8:%s file_stat:0x%llx file_area:0x%llx state:0x%x temp to mapcount\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

	/*如果文件file_stat的mapcount的file_area个数超过阀值，则file_stat被判定为mapcount file_stat而移动到
	 *global mmap_file_stat_mapcount_head链表。但前提file_stat必须在global temp、middle_file、large_file链表*/
	if(!file_stat_in_mapcount_file_area_list(p_file_stat) && is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat)){

		spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
		/*file_stat可能被iput()并发delete了并移动到global delete链表*/
		if(!file_stat_in_delete(p_file_stat)){
			if(F_file_stat_in_file_stat_temp_head_list == file_stat_list_type){
				if(!file_stat_in_file_stat_temp_head_list(p_file_stat) ||file_stat_in_file_stat_temp_head_list_error(p_file_stat))
					panic("%s file_stat:0x%llx status error:0x%lx not in temp_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

				clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			}
			else if(F_file_stat_in_file_stat_middle_file_head_list == file_stat_list_type){
				if(!file_stat_in_file_stat_middle_file_head_list(p_file_stat) || file_stat_in_file_stat_middle_file_head_list_error(p_file_stat))
					panic("%s file_stat:0x%llx status error:0x%lx not int middle_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

				clear_file_stat_in_file_stat_middle_file_head_list(p_file_stat);
			}
			else{
				if(F_file_stat_in_file_stat_large_file_head_list != file_stat_list_type)
					panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat,file_stat_list_type);

				if(!file_stat_in_file_stat_large_file_head_list(p_file_stat) ||file_stat_in_file_stat_large_file_head_list_error(p_file_stat))
					panic("%s file_stat:0x%llx status error:0x%lx not in large_list\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

				clear_file_stat_in_file_stat_large_file_head_list(p_file_stat);
			}

			set_file_stat_in_mapcount_file_area_list(p_file_stat);
			list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_mapcount_head);
			p_hot_cold_file_global->mapcount_mmap_file_stat_count ++;
			if(shrink_page_printk_open1)
				printk("9:%s file_stat:0x%llx status:0x%llx is mapcount file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
		}
		spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
	}
}
static noinline unsigned int check_one_file_area_cold_page_and_clear(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,struct page *page_buf[],int *cold_page_count,unsigned int file_stat_list_type,struct list_head *file_area_have_cache_page_head,unsigned int file_type)
{
	unsigned long vm_flags;
	int ret = 0;
	struct page *page;
	struct folio *folio;
	unsigned cold_page_count_temp = 0;
	int i,j;
	struct address_space *mapping = p_file_stat_base->mapping;
	int file_area_cold = 0;
	struct page *pages[PAGE_COUNT_IN_AREA];
	int mapcount_file_area = 0;
	int hot_file_area = 0;
	int file_area_is_warm = 0;
	/*file_area里的page至少一个page发现是cache page的，则该file_area移动到file_area_have_cache_page_head，后续回收cache的文件页*/
	int find_file_area_have_cache_page = 0;

	unsigned int find_cache_page_count_from_mmap_file = 0;

	//file_area已经很长一段时间没有被访问则file_area_cold置1，只有在这个大前提下，file_area的page pte没有被访问，才能回收page
	if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > p_hot_cold_file_global->mmap_file_area_temp_to_cold_age_dx)
		file_area_cold = 1;

	if(cold_page_count)
		cold_page_count_temp = *cold_page_count;

	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx file_area:0x%llx get %d page\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,ret);

	if(0 == file_area_have_page(p_file_area))
		goto out; 

	//ret必须清0，否则会影响下边ret += page_referenced_async的ret大于0，误判page被访问pte置位了
	ret = 0;
	for(i = 0;i < PAGE_COUNT_IN_AREA;i ++){
		//page = xa_load(&mapping->i_pages, p_file_area->start_index + i);
		folio = p_file_area->pages[i];
		page = &folio->page;
		/*这里判断并清理 映射page的页表页目录pte的access bit，是否有必要对page lock_page加锁呢?需要加锁*/
		if (page /*&& !xa_is_value(page)*/) {
			/*对page上锁，上锁失败就休眠，这里回收mmap的文件页的异步内存回收线程，这里没有加锁且对性能没有要求，可以休眠
			 *到底用lock_page还是trylock_page？如果trylock_page失败的话，说明这个page最近被访问了，那肯定不是冷page，就不用执行
			 *下边的page_referenced检测page的 pte了，浪费性能。??????????????????????????????????????????????????
			 *为什么用trylock_page呢？因为page_lock了实际有两种情况 1：其他进程访问这个page然后lock_page，2：其他进程内存回收
			 *这个page然后lock_pagea。后者page并不是被其他进程被访问而lock了！因此只能用lock_page了，然后再
			 *page_referenced判断page pte，因为这个page可能被其他进程内存回收而lock_page，并不是被访问了lock_page
			 */
			if(shrink_page_printk_open)
				printk("2:%s page:0x%llx index:%ld %ld_%d\n",__func__,(u64)page,page->index,p_file_area->start_index,i);
			lock_page(page);
			//if(trylock_page(page))------不要删
			{
				/*如果page被其他进程回收了，if不成立，这些就不再对该file_area的其他page进行内存回收了，其实
				 *也可以回收，但是处理起来很麻烦，后期再考虑优化优化细节吧!!!!!!!!!!!!!!!!!!!!!!*/
				if(page->mapping != mapping){
					if(shrink_page_printk_open1)
						printk("3:%s file_stat:0x%llx file_area:0x%llx status:0x%x page->mapping != mapping!!!!!!!!!\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

					unlock_page(page);
					continue;
				}
				/*如果page不是mmap的要跳过。一个文件可能是cache文件，同时也被mmap映射，因此这类的文件页page可能不是mmap的，只是cache page
				 *这个判断必须放到lock_page后边*/
				if (!page_mapped(page)){
					unlock_page(page);

					find_cache_page_count_from_mmap_file ++;
					if(shrink_page_printk_open1)
						printk("4:%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx not in page_mapped error!!!!!!\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,(u64)page);

					/* 如果此时在回收cache文件file_area里的mmap文件页，如果遇到这种文件页，if不成立。不做特殊处理，
					 * 主要怕会陷入无限死循环。只有mmap文件内存回收时遇到非mmap文件页if才成立，此时下边把该file_area
					 * 移动到file_area_have_cache_page_head，后续专门回收这种cache文件页。注意，此时不能break跳出，
					 * 结束遍历file_area剩下的page。因为要保证file_area里冷mmap page还能保存到page_buf[]数组，参与
					 * mmap文件页的正常回收流程，而该file_area里的cache文件页也能靠着file_area_have_cache_page_head
					 * 链表而参与cache文件页的内存回收*/
					if((FILE_STAT_FROM_CACHE_FILE != file_stat_list_type) && (NULL != file_area_have_cache_page_head)){
						find_file_area_have_cache_page = 1;
					}
					continue;
				}
				/*一旦有一个page是mapcount的，file_area就被判定为mapcount file_area，然后break跳出结束遍历。下边
				 **cold_page_count = cold_page_count_temp恢复原始值，file_area的page保存到page_buf[]无效，不会再参与内存回收
				 * */
				if(0 == mapcount_file_area && page_mapcount(page) > 1){
					unlock_page(page);
					mapcount_file_area = 1;
					break;
				}

				//检测映射page的页表pte access bit是否置位了，是的话返回1并清除pte access bit。错了，不是返回1，是反应映射page的进程个数
				/*page_referenced函数第2个参数是0里边会自动执行lock page()。这个到底要传入folio还是page????????????????????????????????*/
				ret += folio_referenced(page_folio(page), 1, page_memcg(page),&vm_flags);

				if(shrink_page_printk_open)
					printk("5:%s file_stat:0x%llx file_area:0x%llx page:0x%llx index:%ld file_area_cold:%d cold_page_count:%d ret:%d page_mapcount:%d access_count:%d\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,(u64)page,page->index,file_area_cold,cold_page_count == NULL ?-1:*cold_page_count,ret,page_mapcount(page),file_area_access_count_get(p_file_area));

				/*ret大于0说明page最近被访问了，不是冷page，则赋值全局age*/
				if(ret > 0){
					unlock_page(page);
					/*本次遍历file_area有page被访问了，则判定为温file_area。该file_area剩下的page如果也检测到被访问了，
					 *file_area_is_warm已经是1了，直接continue，不能再执行下边判断file_area是热的代码。因为热file_area
					 *是要连续几个周期file_area的page都被访问，才能判定为热的。如果一个周期file_area的page都被访问了，
					 *不能判定file_area是热的。简单说，下边判定热file_area的代码，一个file_area每个周期只能执行一次！
					 *需要注意，判定file_area是一个page是warm的后，不能break跳出循环，而要继续取出file_area剩下的page
					 执行folio_referenced()，如果pte access bit置位了就清理掉。否则，不清理的话，下个周期page 的pte 
					 access bit位还残留着，但是并没有被访问，导致file_area误判下一个周期也访问了，误判为file_area*/
					if(file_area_is_warm)
						continue;
					file_area_is_warm = 1;

					//不能放在这里，这样二者就相等了,if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <= MMAP_FILE_AREA_HOT_AGE_DX)永远成立
					//p_file_area->file_area_age = p_hot_cold_file_global->global_age;

					/*file_area必须在temp_list链表再令file_area的access_count加1，如果在固定周期内file_area的page被访问次数超过阀值，就判定为热file_area。
					 *file_area可能也在refault_list、free_list也会执行到这个函数，要过滤掉*/
					if(file_area_in_temp_list(p_file_area) || file_area_in_warm_list(p_file_area)){
						//file_area如果在 MMAP_FILE_AREA_HOT_AGE_DX 周期内被检测到访问 MMAP_FILE_AREA_HOT_DX 次，file_area被判定为热file_area
						if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <= p_hot_cold_file_global->mmap_file_area_hot_age_dx){

							//file_area的page被访问了，file_area的access_count加1
							file_area_access_count_add(p_file_area,1);
							//在规定周期内file_area被访问次数大于MMAP_FILE_AREA_HOT_DX则file_area被判定为热file_area
							if(file_area_access_count_get(p_file_area) > MMAP_FILE_AREA_ACCESS_HOT_COUNT){
								/*file_area是热的，file_area判定为file_area的代码移动到下边了*/
								hot_file_area = 1;
							}
						}else{
							//超过MMAP_FILE_AREA_HOT_AGE_DX个周期后对file_area访问计数清0
							file_area_access_count_clear(p_file_area);
						}
					}

					p_file_area->file_area_age = p_hot_cold_file_global->global_age;

					/*这里非常重要。当file_area的一个page发现最近访问过，不能break跳出循环。而是要继续循环把file_area剩下的page也执行
					 *page_referenced()清理掉page的pte access bit。否则，这些pte access bit置位的page会对file_area接下来的冷热造成
					 *重大误判。比如，file_area对应page0~page3，page的pte access bit全置位了。在global_age=1时，执行到该函数，这个for循环
					 *里执行page_referenced()判断出file_area的page0的pte access bit置位了，判断这个file_area最近访问过，然后自动清理掉page的
					 *pte access bit。等global_age=8,10,15时，依次又在该函数的for循环判断出page1、page2、page3的pte access bit置位了。这不仅
					 *导致误判该file_area是热的！实际情况是，page0~page3在global_age=1被访问过一次后就没有再被访问了，等到global_age=15正常
					 *要被判定为冷file_area而回收掉page。但实际却错误连续判定这个file_area一直被访问。解决方法注释掉break，换成continue，这样在
					 *global_age=1时，就会把page0~page3的pte access bit全清0，就不会影响后续判断了。但是这样性能损耗会增大，后续有打算
					 *只用file_area里的1个page判断冷热，不在扫描其他page*/
					//break;
					continue;
				}else{
					/*否则，file_area的page没有被访问，要不要立即就对file_area的access_count清0??????? 修改成，如过了规定周期file_area的page依然没被访问再对
					 *file_area的access_count清0*/
					if(file_area_in_temp_list(p_file_area) || file_area_in_warm_list(p_file_area)){
						if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > p_hot_cold_file_global->mmap_file_area_hot_age_dx)
							file_area_access_count_clear(p_file_area);
					}
				}

				/*cold_page_count不是NULL说明此时遍历的是file_stat->file_area_temp链表上的file_area。否则，遍历的是
				 *file_stat->file_area_refault和file_stat->file_area_free链表上的file_area，使用完page就需要unlock_page*
				 *file_area_cold是1说明此file_area是冷的，file_area的page也没有被访问，然后才回收这个file_area的page*/
				if(cold_page_count != NULL && file_area_cold){
					if(*cold_page_count < BUF_PAGE_COUNT){

						if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <  MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX - 2)
							panic("%s file_stat:0x%llx status:0x%llx is hot ,can not reclaim\n",__func__,(u64)p_file_stat_base,(u64)p_file_stat_base->file_stat_status);

						//冷page保存到page_buf[]，然后参与内存回收
						page_buf[*cold_page_count] = page;
						*cold_page_count = *cold_page_count + 1;
					}
					else
						panic("%s %d error\n",__func__,*cold_page_count);
				}else{
					unlock_page(page);
				}
			}
			/*-------很重要，不要删
			  else{
			//到这个分支，说明page被其他先lock了。1：其他进程访问这个page然后lock_page，2：其他进程内存回收这个page然后lock_pagea。
			//到底要不要令ret加1呢？想来想去不能，于是上边把trylock_page(page)改成lock_page
			//ret += 1;
			}*/
		}
	}

	/*如果上边for循环遍历的file_area的page的mapcount都是1，且file_area的page上边没有遍历完，则这里继续遍历完剩余的page*/
	while(0 == mapcount_file_area && i < PAGE_COUNT_IN_AREA){
		page= pages[i];
		//if (page && !xa_is_value(page) && page_mapped(page) && page_mapcount(page) > 1){
		if (page && page_mapped(page) && page_mapcount(page) > 1){
			mapcount_file_area = 1;
		}
		i ++;
	}

	/*1：如果是遍历mmap文件file_stat->refault、hot、free链表上的file_area，cold_page_count是NULL，if成立。遍历
	 *   file_stat->temp、warm链表上的file_area,cold_page_count不是NULL，if才成立。
	 *
	 *2：如果file_stat_list_type是FILE_STAT_FROM_CACHE_FILE，说明此时在回收cache文件file_area的mmap文件页，
	 *   此时也不能按照mmap文件的逻辑处理热文件和mapcount文件，if不成立
	 *
	 *3：如果在回收mmap文件file_area时，发现file_area里有cache文件页，则find_file_area_have_cache_page置1，此时
	 *   要把file_area移动到file_area_have_cache_page_head链表参与cache文件页内存回收，不能再判断file_area是
	 *   hot或者mapcount的，if不成立。但是如果file_area的age太大了，不冷，也还按照hot、mapcount的处理流程走
	 */
	if(cold_page_count != NULL && (FILE_STAT_FROM_CACHE_FILE != file_stat_list_type)){

		/*file_area里的page至少一个page发现是cache page的，且该file_area的足够冷，则该file_area移动到file_area_have_cache_page_head，后续回收cache的文件页*/
		if(find_file_area_have_cache_page && 
				p_hot_cold_file_global->global_age - p_file_area->file_area_age > p_hot_cold_file_global->file_area_temp_to_cold_age_dx){

			if(file_area_in_temp_list(p_file_area)){
				spin_lock(&p_file_stat_base->file_stat_lock);
				clear_file_area_in_temp_list(p_file_area);
				set_file_area_in_free_list(p_file_area);
				list_move(&p_file_area->file_area_list,file_area_have_cache_page_head);
				spin_unlock(&p_file_stat_base->file_stat_lock);
			}
			else if(file_area_in_warm_list(p_file_area)){
				clear_file_area_in_warm_list(p_file_area);
				set_file_area_in_free_list(p_file_area);
				list_move(&p_file_area->file_area_list,file_area_have_cache_page_head);
			}
			else 
				panic("%s file_area:0x%llx status:%d in error list\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
		}else{

			/* 如果file_area里有cache page，执行不到这里。mmap文件file_stat->warm、temp链表上的，且不包含cache page的
			 * file_area才会执行到这里。如果file_area既是mapcount的又是热的，mapcount优先级更高*/
			if(mapcount_file_area){
				check_mapcount_file_area_and_file_stat(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_stat_list_type,file_type);
				p_hot_cold_file_global->mmap_file_shrink_counter.scan_mapcount_file_area_count += 1;
			}
			else if(hot_file_area){
				check_hot_file_area_and_file_stat(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_stat_list_type,file_type);
				p_hot_cold_file_global->mmap_file_shrink_counter.scan_hot_file_area_count += 1;
			}
		}
	}

	/*到这里有这些可能
	 *1: file_area的page都是冷的，ret是0
	 *2: file_area的page有些被访问了，ret大于0
	 *3：file_area的page都是冷的，但是有些page前边trylock_page失败了，ret大于0。这种情况目前已经不可能了
	 *
	 *如果file_area被判定被mapcount file_area，即mapcount_file_area是1，则上边该file_area被判定是冷page而
	 *保存到page_buf[]数组的page也要清空，使无效，方法就是恢复cold_page_count最初的值即可，它指向最后一个
	 *保存到page_buf[]数组最后一个page的下标。如果file_area被判定是热的，处理一样

	 *如果file_area里有cache的page，find_file_area_have_cache_page是1，但是file_area的有mmap page是冷的而保存
	 *到page_buf[]数组，此时要"*cold_page_count = cold_page_count_temp"清理掉page_buf[]里保存的这些冷mmap page
	 *要清理掉吗??????????????????????file_area里其他mmap page是热的，mapcount的，
	 *if(ret > 0 || mapcount_file_area || hot_file_area) 这3个条件都可能成立！分析后认为没事，因为这里清理掉
	 *page_buf[]中保存的这些mmap page，并不会影响file_area里cache page将来的内存回收
	 */
	//历的是file_stat->file_area_temp链表上的file_area是if才成立
	if((ret > 0 || mapcount_file_area || hot_file_area) && cold_page_count != NULL && file_area_cold){
		/*走到这里，说明file_area的page可能是热的，或者page_lock失败，那就不参与内存回收了。那就要对已加锁的page解锁*/
		/*解除上边加锁的page lock，cold_page_count ~ cold_page_count+i 的page上边加锁了，这里解锁*/

		for(j = cold_page_count_temp ;j < *cold_page_count;j++){
			page = page_buf[j];
			if(page){
				if(shrink_page_printk_open)
					printk("10:%s file_stat:0x%llx file_area:0x%llx cold_page_count:%d page:0x%llx unlock_page\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,*cold_page_count,(u64)page);

				unlock_page(page);
			}else
				panic("11:%s file_stat:0x%llx file_area:0x%llx cold_page_count:%d_%d page NULL error\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,cold_page_count_temp,*cold_page_count);
		}
		/*重点，不回收该file_area的page，恢复cold_page_count最初的值*/
		*cold_page_count = cold_page_count_temp;
	}
out:

	p_hot_cold_file_global->mmap_file_shrink_counter.find_cache_page_count_from_mmap_file += find_cache_page_count_from_mmap_file;
	//返回值是file_area里4个page是热page的个数
	return ret;
}

/*执行该函数的file_area，文件本身是cache文件，文件参与内存时，发现file_area->age很小。然后参与cache文件内存时，
 *发现file_area的page有mmap的，然后就执行该函数按照mmap文件页的逻辑回收该file_area里的mmap文件页*/
int cache_file_area_mmap_page_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct list_head *file_area_have_mmap_page_head,unsigned int file_type)
{
	struct file_area *p_file_area,*p_file_area_temp;
	struct file_stat *p_file_stat = NULL;
	struct file_stat_small *p_file_stat_small = NULL;
	unsigned int scan_file_area_count = 0;
	unsigned int scan_cold_file_area_count = 0;
	struct page *page_buf[BUF_PAGE_COUNT];
	int cold_page_count = 0,cold_page_count_last;
	int ret = 0;
	unsigned int reclaimed_pages = 0;
	unsigned int reclaimed_pages_ori = 0;
	unsigned int isolate_pages = 0;

	reclaimed_pages_ori = p_hot_cold_file_global->mmap_file_shrink_counter.mmap_free_pages_count;

	list_for_each_entry_safe(p_file_area,p_file_area_temp,file_area_have_mmap_page_head,file_area_list){
		/*file_area_have_cache_page_head链表上的file_area必须处于in_free状态*/
		if(!file_area_in_free_list(p_file_area) || file_area_in_free_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

		scan_file_area_count ++;

		cold_page_count_last = cold_page_count;
		ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat_base,p_file_area,page_buf,&cold_page_count,FILE_STAT_FROM_CACHE_FILE,NULL,file_type);

		/*如果check_one_file_area_cold_page_and_clear()里改变了file_area的状态，也触发crash*/
		if(!file_area_in_free_list(p_file_area) || file_area_in_free_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

		/*file_area的mmap page是热的，则直接遍历下一个file_area*/
		if(cold_page_count_last == cold_page_count)
			continue;

		scan_cold_file_area_count ++;
		/*1:凑够BUF_PAGE_COUNT个要回收的page，if成立，则开始隔离page、回收page
		 *2:page_buf剩余的空间不足容纳PAGE_COUNT_IN_AREA个page，if也成立，否则下个循环执行check_one_file_area_cold_page_and_clear函数
		 *向page_buf保存PAGE_COUNT_IN_AREA个page，将导致内存溢出*/
		if(cold_page_count >= BUF_PAGE_COUNT || (BUF_PAGE_COUNT - cold_page_count <=  PAGE_COUNT_IN_AREA)){

			isolate_pages += cold_mmap_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,p_file_area,page_buf,cold_page_count);
			reclaimed_pages = p_hot_cold_file_global->mmap_file_shrink_counter.mmap_free_pages_count;
			cold_page_count = 0;
			if(shrink_page_printk_open)
				printk("3:%s file_stat:0x%llx reclaimed_pages:%d isolate_pages:%d\n",__func__,(u64)p_file_stat_base,reclaimed_pages,isolate_pages);
		}
	}

	if(FILE_STAT_NORMAL == file_type){
		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
		if(file_stat_in_file_stat_hot_head_list(p_file_stat) || file_stat_in_mapcount_file_area_list(p_file_stat)){
			panic("%s file_stat:0x%llx status:0x%lx in hot or mapcount error\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
		}
	}
	//如果本次对文件遍历结束后，有未达到BUF_PAGE_COUNT数目要回收的page，这里就隔离+回收这些page
	if(cold_page_count){
		isolate_pages += cold_mmap_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,p_file_area,page_buf,cold_page_count);
		reclaimed_pages = p_hot_cold_file_global->mmap_file_shrink_counter.mmap_free_pages_count;
		if(shrink_page_printk_open)
			printk("5:%s mapping:0x%llx file_stat:0x%llx reclaimed_pages:%d isolate_pages:%d\n",__func__,(u64)p_file_stat->mapping,(u64)p_file_stat,reclaimed_pages,isolate_pages);
	}

	if(FILE_STAT_NORMAL == file_type){
		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
	}else if(FILE_STAT_SMALL == file_type)
		p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);

	list_for_each_entry_safe(p_file_area,p_file_area_temp,file_area_have_mmap_page_head,file_area_list){
		/*该if成立说明 1:file_area的mmap的文件页最近被访问了，在cold_mmap_file_isolate_lru_pages_and_shrink()中
		 *被赋值为p_hot_cold_file_global->global_age，二者差值小于1。 2:file_area的age没达到mmap文件页回收阀值
		 *mmap_file_area_temp_to_cold_age_dx，于是没有参与内存回收。于是把这些file_area移动到file_stat->warm链表头，后续再参与内存回收*/
		if(/*p_hot_cold_file_global->global_age - p_file_area->file_area_age <= 1 ||*/ FILE_STAT_NORMAL == file_type &&
				p_hot_cold_file_global->global_age - p_file_area->file_area_age <= p_hot_cold_file_global->mmap_file_area_temp_to_cold_age_dx){
			clear_file_area_in_free_list(p_file_area);
			set_file_area_in_warm_list(p_file_area);
			list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
		}
		/* 否则 1:file_area的mmap文件页足够冷而参与内存回收，但是mmap文件页内存回收失败。此时把file_area直接移动到
		 * file_stat->refault链表 文件页足够冷而参与内存回收而成功，该函数执行后会把file_area移动到file_stat->free链表，
		 * 然后按照cache文件页file_area的逻辑，如果file_area的age与global age差距很大则释放掉file_ara。这就有问题了，
		 * 因为cache文件页读写时会主动调用到 hot_file_update_file_status()把global age赋值给file_area的age，但是这些
		 * file_area的mmap文件页，再被mmap映射后分配page、读写，并不会执行hot_file_update_file_status()把global age赋值给
		 * file_area的age，file_area的age一直保持老的，很小很小。将来遍历到该file_area肯定会因file_area的age很小而
		 * 判定该file_area长时间没访问而释放掉file_area结构，那就出大问题了，file_area明明还有page呀，只不过是
		 * mmap的page。于是要改进，在执行cold_file_area_delete()释放file_area的地方，判断出file_area还有page后，
		 * 终止释放file_area，然后把file_area移动到file_stat->refault链表处理得了
		 */
		else{
			if(file_area_have_page(p_file_area)){
				clear_file_area_in_free_list(p_file_area);
				set_file_area_in_refault_list(p_file_area);
				if(FILE_STAT_NORMAL == file_type)
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
				else if(FILE_STAT_SMALL == file_type)
					list_move(&p_file_area->file_area_list,&p_file_stat_small->file_area_other);
			}
		}
	}


	reclaimed_pages_ori = p_hot_cold_file_global->mmap_file_shrink_counter.mmap_free_pages_count - reclaimed_pages_ori;

	p_hot_cold_file_global->mmap_file_shrink_counter.scan_file_area_count_from_cache_file += scan_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.scan_cold_file_area_count_from_cache_file += scan_cold_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.free_pages_from_cache_file += reclaimed_pages_ori;

	return scan_file_area_count;
}
int reverse_other_file_area_list_common(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,struct file_area *p_file_area,unsigned int file_area_type,unsigned int file_type,struct list_head *file_area_list)
{
	//unsigned int scan_file_area_count = 0;
	//struct file_area *p_file_area,*p_file_area_temp;
	int i,ret;
	//LIST_HEAD(file_area_list);
	struct page *page;
	struct folio *folio;

	unsigned int mapcount_to_warm_file_area_count = 0;
	unsigned int hot_to_warm_file_area_count = 0;
	unsigned int refault_to_warm_file_area_count = 0;
	unsigned int check_refault_file_area_count = 0;
	unsigned int free_file_area_count = 0;
	struct file_stat *p_file_stat;
    
	switch (file_area_type){
		case (1 << F_file_area_in_mapcount_list):
			if(!file_area_in_mapcount_list(p_file_area) || file_area_in_mapcount_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_mapcount\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			//file_area被遍历到时记录当时的global_age，不管此时file_area的page是否被访问pte置位了
			p_file_area->file_area_access_age = p_hot_cold_file_global->global_age;
			if(0 == file_area_have_page(p_file_area))
				return 0; 

			/*存在一种情况，file_area的page都是非mmap的，普通文件页!!!!!!!!!!!!!!!!!!!*/
			for(i = 0;i < PAGE_COUNT_IN_AREA;i ++){
				folio = p_file_area->pages[i];
				if(!folio){
					if(shrink_page_printk_open1)
						printk("%s file_area:0x%llx status:0x%x folio NULL\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

					continue;
				}

				page = &folio->page;
				if(page_mapcount(page) > 1)
					break;
			}
			//if成立说明file_area的page的mapcount都是1，file_area不再是mapcount file_area，则降级到temp_list链表头
			if(i == PAGE_COUNT_IN_AREA){
				//file_stat->refault、free、hot、mapcount链表上的file_area移动到file_stat->temp链表时要先对file_area->file_area_access_age清0，原因看定义
				p_file_area->file_area_access_age = 0;

				//spin_lock(&p_file_stat->file_stat_lock);现在file_area移动到file_stat->warm链表，不用加锁
				clear_file_area_in_mapcount_list(p_file_area);
				/*small文件是把file_area移动到file_stat->temp链表*/
				if(FILE_STAT_TINY_SMALL == file_type){
					set_file_area_in_temp_list(p_file_area);
					p_file_stat->file_area_count_in_temp_list ++;
				}
				else if(FILE_STAT_SMALL == file_type){
					spin_lock(&p_file_stat_base->file_stat_lock);
					set_file_area_in_temp_list(p_file_area);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
					p_file_stat_base->file_area_count_in_temp_list ++;
					spin_unlock(&p_file_stat_base->file_stat_lock);
				}
				else{
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

					set_file_area_in_warm_list(p_file_area);
					//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
					mapcount_to_warm_file_area_count ++;
					//在file_stat->file_area_temp链表的file_area个数加1，这是把file_area移动到warm链表，不能file_area_count_in_temp_list加1
					//p_file_stat->file_area_count_in_temp_list ++;
				}

				//spin_unlock(&p_file_stat->file_stat_lock);

				//在file_stat->file_area_mapcount链表的file_area个数减1
				p_file_stat->mapcount_file_area_count --;
			}
			else{	
				/*否则file_area移动到file_area_list临时链表。但要防止前边file_area被移动到其他file_stat的链表了，此时就不能再把该file_area
				 *移动到file_area_list临时链表，否则该函数最后会把file_area再移动到老的file_stat链表，file_area的状态和所处链表就错乱了，会crash*/
				if(file_area_list && file_area_in_mapcount_list(p_file_area))
					list_move(&p_file_area->file_area_list,file_area_list);
				else
					printk("%s %d file_area:0x%llx status:%d changed\n",__func__,__LINE__,(u64)p_file_area,p_file_area->file_area_state);
			}
			break;
		case (1 << F_file_area_in_hot_list):
			if(!file_area_in_hot_list(p_file_area) || file_area_in_hot_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_hot\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			//file_area被遍历到时记录当时的global_age，不管此时file_area的page是否被访问pte置位了
			p_file_area->file_area_access_age = p_hot_cold_file_global->global_age;

			//检测file_area的page最近是否被访问了
			ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat_base,p_file_area,NULL,NULL,FILE_STAT_OTHER_FILE_AREA,NULL,file_type);
			//file_area的page被访问了，依然停留在hot链表
			if(ret > 0){				
				/*否则file_area移动到file_area_list临时链表。但要防止前边check_one_file_area_cold_page_and_clear()函数file_area被
				 *移动到其他file_stat的链表了，此时就不能再把该file_area移动到file_area_list临时链表，
				 否则该函数最后会把file_area再移动到老的file_stat链表，file_area的状态和所处链表就错乱了，会crash*/
				if(file_area_list && file_area_in_hot_list(p_file_area))
					list_move(&p_file_area->file_area_list,file_area_list);
				else
					printk("%s %d file_area:0x%llx status:%d changed\n",__func__,__LINE__,(u64)p_file_area,p_file_area->file_area_state);
			}
			//file_area在MMAP_FILE_AREA_HOT_TO_TEMP_AGE_DX个周期内没有被访问，则降级到temp链表
			else if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > p_hot_cold_file_global->mmap_file_area_hot_to_temp_age_dx){
				//file_stat->refault、free、hot、mapcount链表上的file_area移动到file_stat->temp链表时要先对file_area->file_area_access_age清0，原因看定义
				p_file_area->file_area_access_age = 0;

				//spin_lock(&p_file_stat->file_stat_lock);现在file_area移动到file_stat->warm链表，不用加锁
				clear_file_area_in_hot_list(p_file_area);

				if(FILE_STAT_TINY_SMALL == file_type){
					set_file_area_in_temp_list(p_file_area);
					p_file_stat->file_area_count_in_temp_list ++;
				}
				else if(FILE_STAT_SMALL == file_type){
					spin_lock(&p_file_stat_base->file_stat_lock);
					set_file_area_in_temp_list(p_file_area);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
					p_file_stat_base->file_area_count_in_temp_list ++;
					spin_unlock(&p_file_stat_base->file_stat_lock);
				}else{
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

					set_file_area_in_warm_list(p_file_area);
					barrier();

					//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);

					//在file_stat->file_area_temp链表的file_area个数加1
					//p_file_stat->file_area_count_in_temp_list ++;

					//spin_unlock(&p_file_stat->file_stat_lock);
					hot_to_warm_file_area_count ++;
				}
				//在file_stat->file_area_hot链表的file_area个数减1
				p_file_stat->file_area_hot_count --;
			}

			break;
			/*遍历file_stat->file_area_refault链表上的file_area，如果长时间没访问，要把file_area移动回file_stat->file_area_temp链表*/
		case (1 << F_file_area_in_refault_list):
			if(!file_area_in_refault_list(p_file_area) || file_area_in_refault_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_refault\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			//file_area被遍历到时记录当时的global_age，不管此时file_area的page是否被访问pte置位了
			p_file_area->file_area_access_age = p_hot_cold_file_global->global_age;

			//检测file_area的page最近是否被访问了
			ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat_base,p_file_area,NULL,NULL,FILE_STAT_OTHER_FILE_AREA,NULL,file_type);
			if(ret > 0){
				/*否则file_area移动到file_area_list临时链表。但要防止前边check_one_file_area_cold_page_and_clear()函数file_area被
				 *移动到其他file_stat的链表了，此时就不能再把该file_area移动到file_area_list临时链表，
				 否则该函数最后会把file_area再移动到老的file_stat链表，file_area的状态和所处链表就错乱了，会crash*/
				if(file_area_list && file_area_in_refault_list(p_file_area))
					list_move(&p_file_area->file_area_list,file_area_list);
				else
					printk("%s %d file_area:0x%llx status:%d changed\n",__func__,__LINE__,(u64)p_file_area,p_file_area->file_area_state);
			}
			//file_area在MMAP_FILE_AREA_REFAULT_TO_TEMP_AGE_DX个周期内没有被访问，则降级到temp链表
			else if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > p_hot_cold_file_global->mmap_file_area_refault_to_temp_age_dx){
				//file_stat->refault、free、hot、mapcount链表上的file_area移动到file_stat->temp链表时要先对file_area->file_area_access_age清0，原因看定义
				p_file_area->file_area_access_age = 0;

				//spin_lock(&p_file_stat->file_stat_lock);现在file_area移动到file_stat->warm链表，不用加锁
				clear_file_area_in_refault_list(p_file_area);

				if(FILE_STAT_TINY_SMALL == file_type){
					set_file_area_in_temp_list(p_file_area);
					p_file_stat->file_area_count_in_temp_list ++;
				}
				else if(FILE_STAT_SMALL == file_type){
					spin_lock(&p_file_stat_base->file_stat_lock);
					set_file_area_in_temp_list(p_file_area);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
					p_file_stat_base->file_area_count_in_temp_list ++;
					spin_unlock(&p_file_stat_base->file_stat_lock);
				}
				else{
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

					set_file_area_in_warm_list(p_file_area);
					barrier();

					//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
					refault_to_warm_file_area_count ++;
					//在file_stat->file_area_temp链表的file_area个数加1
					//p_file_stat->file_area_count_in_temp_list ++;
				}
				//spin_unlock(&p_file_stat->file_stat_lock);
			}

			break;
			/*遍历file_stat->file_area_free链表上file_area，如果长时间不被访问则释放掉file_area结构。如果短时间被访问了，则把file_area移动到
			 *file_stat->file_area_refault链表，如果过了很长时间被访问了，则把file_area移动到file_stat->file_area_temp链表*/
		case (1 << F_file_area_in_free_list):

			if(!file_area_in_free_list(p_file_area) || file_area_in_free_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			//file_area被遍历到时记录当时的global_age，不管此时file_area的page是否被访问pte置位了
			p_file_area->file_area_access_age = p_hot_cold_file_global->global_age;

			//检测file_area的page最近是否被访问了
			ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat_base,p_file_area,NULL,NULL,FILE_STAT_OTHER_FILE_AREA,NULL,file_type);
			if(0 == ret){
				//file_stat->file_area_free链表上file_area，如果长时间不被访问则释放掉file_area结构，里边有把file_area从链表剔除的代码
				if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > p_hot_cold_file_global->mmap_file_area_free_age_dx){
					clear_file_area_in_free_list(p_file_area);
					if(cold_file_area_delete(p_hot_cold_file_global,p_file_stat_base,p_file_area) > 0){
						/*在释放file_area过程发现file_area分配page了，于是把file_area移动到file_stat->refault链表*/
						file_area_access_count_clear(p_file_area);
						clear_file_area_in_free_list(p_file_area);
						set_file_area_in_refault_list(p_file_area);

						/*tiny small和small文件，free和refault属性的file_ara都在同一个链表上，故不用移动该file_area*/
						if(FILE_STAT_TINY_SMALL != file_type && FILE_STAT_SMALL != file_type){
							p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
							list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
						}
						/*检测到refault的file_area个数加1*/
						p_hot_cold_file_global->check_mmap_refault_file_area_count ++;
						check_refault_file_area_count ++;
					}else{
						free_file_area_count ++;
					}
				}else{
					/*否则file_area移动到file_area_list临时链表。但要防止前边check_one_file_area_cold_page_and_clear()函数file_area被
					 *移动到其他file_stat的链表了，此时就不能再把该file_area移动到file_area_list临时链表，
					 否则该函数最后会把file_area再移动到老的file_stat链表，file_area的状态和所处链表就错乱了，会crash*/
					if(file_area_list && file_area_in_free_list(p_file_area))
						list_move(&p_file_area->file_area_list,file_area_list);
					else
						printk("%s %d file_area:0x%llx status:%d changed\n",__func__,__LINE__,(u64)p_file_area,p_file_area->file_area_state);	
				}
			}else{
#if 0	
				if(0 == p_file_area->shrink_time)
					panic("%s file_stat:0x%llx status:0x%lx p_file_area->shrink_time == 0\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

				//file_area的page被访问而pte置位了，则file_area->file_area_age记录当时的全局age。然后把file_area移动到file_stat->refault或temp链表
				//在check_one_file_area_cold_page_and_clear函数如果page被访问过，就会对file_area->file_area_age赋值，这里就不用再赋值了
				//p_file_area->file_area_age = p_hot_cold_file_global->global_age;

				clear_file_area_in_free_list(p_file_area);
				/*file_stat->file_area_free链表上file_area，短时间被访问了，则把file_area移动到file_stat->file_area_refault链表。否则
				 *移动到file_stat->file_area_temp链表*/
				if(p_file_area->shrink_time && (ktime_to_ms(ktime_get()) - (p_file_area->shrink_time << 10) < 130000)){
					set_file_area_in_refault_list(p_file_area);
					barrier();
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);	
				}
				else{
					//file_stat->refault、free、hot、mapcount链表上的file_area移动到file_stat->temp链表时要先对file_area->file_area_access_age清0
					p_file_area->file_area_access_age = 0;

					spin_lock(&p_file_stat->file_stat_lock);
					set_file_area_in_temp_list(p_file_area);
					barrier();
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
					//在file_stat->file_area_temp链表的file_area个数加1
					p_file_stat->file_area_count_in_temp_list ++;
					spin_unlock(&p_file_stat->file_stat_lock);
				}
				p_file_area->shrink_time = 0;
#endif
				/*现在只要file_stat->free链表上的file_area被访问，不管过了多长时间，都判定为refault file_area*/
				clear_file_area_in_free_list(p_file_area);
				set_file_area_in_refault_list(p_file_area);
				/*tiny small和small文件，free和refault属性的file_ara都在同一个链表上，故不用移动该file_area*/
				if(FILE_STAT_TINY_SMALL != file_type && FILE_STAT_SMALL != file_type){
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
				}

				/*检测到refault的file_area个数加1*/
				p_hot_cold_file_global->check_mmap_refault_file_area_count ++;
				check_refault_file_area_count ++;
			}
			break;
		default:
			panic("%s file_area:0x%llx status:%d error\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
	}

	p_hot_cold_file_global->mmap_file_shrink_counter.mapcount_to_warm_file_area_count += mapcount_to_warm_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.hot_to_warm_file_area_count += hot_to_warm_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.refault_to_warm_file_area_count += refault_to_warm_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.check_refault_file_area_count += check_refault_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.free_file_area_count += free_file_area_count;

	return 0;
}
/*1:遍历file_stat->file_area_mapcount上的file_area，如果file_area的page的mapcount都是1，file_area不再是mapcount file_area，则降级到file_stat->temp链表
 *2:遍历file_stat->file_area_hot、refault上的file_area，如果长时间不被访问了，则降级到file_stat->temp链表
 *3:遍历file_stat->file_area_free链表上的file_area，如果对应page还是长时间不访问则释放掉file_area，如果被访问了则升级到file_stat->temp链表
 */

//static int reverse_file_area_mapcount_and_hot_list(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct list_head *file_area_list_head,int traversal_max_count,char type,int age_dx)//file_area_list_head 是p_file_stat->file_area_mapcount 或 p_file_stat->file_area_hot链表
static noinline int reverse_other_file_area_list(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,struct list_head *file_area_list_head,int traversal_max_count,unsigned int file_area_type,int age_dx)//file_area_list_head 是p_file_stat->file_area_mapcount、hot、refault、free链表
{
	unsigned int scan_file_area_count = 0;
	struct file_area *p_file_area,*p_file_area_temp;
	//int i,ret;
	LIST_HEAD(file_area_list);
	//struct page *page;
	//struct folio *folio;
	
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,file_area_list_head,file_area_list){//从链表尾开始遍历
		//如果file_area_list_head 链表尾的file_area在规定周期内不再遍历，降低性能损耗。链表尾的file_area的file_area_access_age更小，
		//它的file_area_access_age与global_age相差小于age_dx，链表头的更小于
		if(p_hot_cold_file_global->global_age - p_file_area->file_area_access_age <= age_dx){
			if(shrink_page_printk_open)
				printk("1:%s file_stat:0x%llx type:%d  global_age:%d file_area_access_age:%d age_dx:%d\n",__func__,(u64)p_file_stat,file_area_type,p_hot_cold_file_global->global_age,p_file_area->file_area_access_age,age_dx);

			break;
		}
		if(scan_file_area_count ++ > traversal_max_count)
			break;

  	    reverse_other_file_area_list_common(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area,file_area_type,FILE_STAT_NORMAL,&file_area_list);

		//在把file_area移动到其他链表后，file_area_list_head链表可能是空的，没有file_area了，就结束遍历。其实这个判断list_for_each_entry_safe_reverse也有
		if(list_empty(file_area_list_head)){
			break;
		}
	}
	//如果file_area_list临时链表上有file_area，则要移动到 file_area_list_head链表头，最近遍历过的file_area移动到链表头
	if(!list_empty(&file_area_list)){
		list_splice(&file_area_list,file_area_list_head);
	}
	if(shrink_page_printk_open1)
		printk("2:%s file_stat:0x%llx type:%d scan_file_area_count:%d\n",__func__,(u64)p_file_stat,file_area_type,scan_file_area_count);

	
	return scan_file_area_count;
}
static noinline int reverse_other_file_area_list_small_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_small *p_file_stat_small,struct list_head *file_area_list_head,int traversal_max_count,unsigned int file_area_type,int age_dx)//file_area_list_head 是p_file_stat_small->otehr链表
{
	unsigned int scan_file_area_count = 0;
	struct file_area *p_file_area,*p_file_area_temp;
	//int i,ret;
	LIST_HEAD(file_area_list);
	//struct page *page;
	//struct folio *folio;
	
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,file_area_list_head,file_area_list){//从链表尾开始遍历
		//如果file_area_list_head 链表尾的file_area在规定周期内不再遍历，降低性能损耗。链表尾的file_area的file_area_access_age更小，
		//它的file_area_access_age与global_age相差小于age_dx，链表头的更小于
		if(p_hot_cold_file_global->global_age - p_file_area->file_area_access_age <= age_dx){
			if(shrink_page_printk_open)
				printk("1:%s file_stat:0x%llx type:%d  global_age:%d file_area_access_age:%d age_dx:%d\n",__func__,(u64)p_file_stat_small,file_area_type,p_hot_cold_file_global->global_age,p_file_area->file_area_access_age,age_dx);

			break;
		}
		if(scan_file_area_count ++ > traversal_max_count)
			break;
        //获取file_area的状态，可能是 hot、free、refault、mapcount 这几种
		file_area_type = get_file_area_list_status(p_file_area);
  	    reverse_other_file_area_list_common(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,p_file_area,file_area_type,FILE_STAT_SMALL,&file_area_list);

		//在把file_area移动到其他链表后，file_area_list_head链表可能是空的，没有file_area了，就结束遍历。其实这个判断list_for_each_entry_safe_reverse也有
		if(list_empty(file_area_list_head)){
			break;
		}
	}
	//如果file_area_list临时链表上有file_area，则要移动到 file_area_list_head链表头，最近遍历过的file_area移动到链表头
	if(!list_empty(&file_area_list)){
		list_splice(&file_area_list,file_area_list_head);
	}
	if(shrink_page_printk_open1)
		printk("2:%s file_stat:0x%llx type:%d scan_file_area_count:%d\n",__func__,(u64)p_file_stat_small,file_area_type,scan_file_area_count);

	
	return scan_file_area_count;
}

#if 0 //这段代码不要删除，有重要价值
/*遍历file_stat->file_area_refault和file_stat->file_area_free_temp链表上的file_area，根据具体情况处理*/
static int reverse_file_area_refault_and_free_list(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct file_area **file_area_last,struct list_head *file_area_list_head,int traversal_max_count,char type)
{//file_area_list_head 是file_stat->file_area_refault或file_stat->file_area_free_temp链表头

	int ret;
	unsigned int scan_file_area_count = 0;
	struct file_area *p_file_area,*p_file_area_temp;
	char delete_file_area_last = 0;

	printk("1:%s file_stat:0x%llx file_area_last:0x%llx type:%d\n",__func__,(u64)p_file_stat,(u64)*file_area_last,type);
	if(*file_area_last){//本次从上一轮扫描打断的file_area继续遍历
		p_file_area = *file_area_last;
	}
	else{
		//第一次从链表尾的file_area开始遍历
		p_file_area = list_last_entry(file_area_list_head,struct file_area,file_area_list);
		*file_area_last = p_file_area;
	}

	do {
		/*查找file_area在file_stat->file_area_temp链表上一个file_area。如果p_file_area不是链表头的file_area，直接list_prev_entry
		 * 找到上一个file_area。如果p_file_stat是链表头的file_area，那要跳过链表过，取出链表尾的file_area*/
		if(!list_is_first(&p_file_area->file_area_list,file_area_list_head))
			p_file_area_temp = list_prev_entry(p_file_area,file_area_list);
		else
			p_file_area_temp = list_last_entry(file_area_list_head,struct file_area,file_area_list);

		//检测file_area的page最近是否被访问了
		ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat,p_file_area,NULL,NULL);

		/*遍历file_stat->file_area_refault链表上的file_area，如果长时间没访问，要把file_area移动回file_stat->file_area_temp链表*/
		if(FILE_AREA_REFAULT == type ){
			if(!file_area_in_refault_list(p_file_area) || file_area_in_refault_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_refault\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			if(ret > 0){
				p_file_area->file_area_age = p_hot_cold_file_global->global_age;
			}
			//file_area在MMAP_FILE_AREA_REFAULT_TO_TEMP_AGE_DX个周期内没有被访问，则降级到temp链表
			else if(p_hot_cold_file_global->global_age - p_file_area->file_area_age >MMAP_FILE_AREA_REFAULT_TO_TEMP_AGE_DX){
				//这段if判断代码的原因分析见check_file_area_cold_page_and_clear()函数
				if(*file_area_last == p_file_area){
					*file_area_last = p_file_area_temp;
					delete_file_area_last = 1;
				}
				clear_file_area_in_refault_list(p_file_area);
				set_file_area_in_temp_list(p_file_area);
				barrier();
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
				//在file_stat->file_area_temp链表的file_area个数加1
				p_file_stat->file_area_count_in_temp_list ++;
			}
		}
		/*遍历file_stat->file_area_free_temp链表上file_area，如果长时间不被访问则释放掉file_area结构。如果短时间被访问了，则把file_area移动到
		 *file_stat->file_area_refault链表，如果过了很长时间被访问了，则把file_area移动到file_stat->file_area_temp链表*/
		else if(FILE_AREA_FREE == type){

			if(!file_area_in_free_list(p_file_area) || file_area_in_free_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			if(0 == ret){
				//file_stat->file_area_free_temp链表上file_area，如果长时间不被访问则释放掉file_area结构，里边有把file_area从链表剔除的代码
				if(p_hot_cold_file_global->global_age - p_file_area->file_area_age >  MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX){
					//这段if代码的原因分析见check_file_area_cold_page_and_clear()函数
					if(*file_area_last == p_file_area){
						*file_area_last = p_file_area_temp;
						delete_file_area_last = 1;
					}
					clear_file_area_in_free_list(p_file_area);
					cold_file_area_detele_quick(p_hot_cold_file_global,p_file_stat,p_file_area);
				}
			}else{
				if(0 == p_file_area->shrink_time)
					panic("%s file_stat:0x%llx status:0x%lx p_file_area->shrink_time == 0\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

				//这段if代码的原因分析见check_file_area_cold_page_and_clear()函数
				if(*file_area_last == p_file_area){
					*file_area_last = p_file_area_temp;
					delete_file_area_last = 1;
				}

				p_file_area->file_area_age = p_hot_cold_file_global->global_age;
				clear_file_area_in_free_list(p_file_area);
				/*file_stat->file_area_free_temp链表上file_area，短时间被访问了，则把file_area移动到file_stat->file_area_refault链表。否则
				 *移动到file_stat->file_area_temp链表*/
				if(p_file_area->shrink_time && (ktime_to_ms(ktime_get()) - (p_file_area->shrink_time << 10) < 5000)){
					set_file_area_in_refault_list(p_file_area);
					barrier();
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);	
				}
				else{
					set_file_area_in_temp_list(p_file_area);
					barrier();
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
					//在file_stat->file_area_temp链表的file_area个数加1
					p_file_stat->file_area_count_in_temp_list ++;
				}
				p_file_area->shrink_time = 0;
			}
		}

		//下一个扫描的file_area
		p_file_area = p_file_area_temp;

		//超过本轮扫描的最大file_area个数则结束本次的遍历
		if(scan_file_area_count > traversal_max_count)
			break;
		scan_file_area_count ++;

		if(0 == delete_file_area_last && p_file_area == *file_area_last)
			break;
		else if(delete_file_area_last)
			delete_file_area_last = 0;

		/*这里退出循环的条件，不能碰到链表头退出，是一个环形队列的遍历形式,以下两种情况退出循环
		 *1：上边的 遍历指定数目的file_area后，强制结束遍历
		 *2：这里的while，本次循环处理到file_area已经是第一次循环处理过了，相当于重复了
		 *3: 添加!list_empty(&file_area_list_head)判断，详情见check_file_area_cold_page_and_clear()分析
		 */
		//}while(p_file_area != *file_area_last && !list_empty(file_area_list_head));
    }while(!list_empty(file_area_list_head));

	if(!list_empty(file_area_list_head)){
		/*下个周期直接从file_area_last指向的file_area开始扫描*/
		if(!list_is_first(&p_file_area->file_area_list,file_area_list_head))
			*file_area_last = list_prev_entry(p_file_area,file_area_list);
		else
			*file_area_last = list_last_entry(file_area_list_head,struct file_area,file_area_list);
	}else{
		/*这里必须对file_area_last清NULL，否则下次执行该函数，file_area_last指向的是一个非法的file_area，从而触发crash。比如
		 *file_stat->file_area_free链表只有一个file_area，因为长时间不被访问，执行cold_file_area_detele_quick()释放了。但是释放
		 前，先执行*file_area_last = p_file_area_temp赋值，这个赋值令*file_area_last指向刚释放的file_area，因为p_file_area_temp
		 指向释放的file_area，file_stat->file_area_free链表只有这一个file_area！继续，释放唯一的file_area后，此时file_stat->file_area_free链表空
		 (即file_area_list_head链表空)，则跳出while循环。然后 *file_area_last 还是指向刚释放file_area。下次执行该函数，使用 *file_area_last
		 这个指向的已经释放的file_aera，就会crash*/
		*file_area_last = NULL;
	}

    return scan_file_area_count;
}
#endif

#if 0 //这段源码不要删除，牵涉到一个内存越界的重大bug!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
/*文件的radix tree在遍历完一次所有的page后，可能存在空洞，于是后续还要再遍历文件的radix tree获取之前没有遍历到的page*/
static unsigned int reverse_file_stat_radix_tree_hole(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat)
{
	int i,j,k;
	struct hot_cold_file_area_tree_node *parent_node;
	void **page_slot_in_tree = NULL;
	unsigned int area_index_for_page;
	int ret = 0;
	struct page *page;
	unsigned int file_page_count = p_file_stat->mapping->host->i_size >> PAGE_SHIFT;//除以4096
	unsigned int start_page_index = 0;
	struct address_space *mapping = p_file_stat->mapping;

	//p_file_stat->traverse_done是0，说明还没有遍历完一次文件的page，那先返回
	if(0 == p_file_stat->traverse_done)
		return ret;

	printk("1:%s file_stat:0x%llx file_stat->last_index:0x%d\n",__func__,(u64)p_file_stat,p_file_stat->last_index);

	//p_file_stat->last_index初值是0，后续依次是64*1、64*2、64*3等等，
	area_index_for_page = p_file_stat->last_index;

	//一次遍历SCAN_FILE_AREA_NODE_COUNT个node，一个node 64个file_area
	for(i = 0;i < SCAN_FILE_AREA_NODE_COUNT;i++){
		/*查找索引0、64*1、64*2、64*3等等的file_area的地址，保存到page_slot_in_tree。page_slot_in_tree指向的是每个node节点的第一个file_area，
		 *每个node节点一共64个file_area，都保存在node节点的slots[64]数组。下边的for循环一次查找node->slots[0]~node->slots[63]，如果是NULL，
		 *说明还没有分配file_area，是空洞，那就分配file_area并添加到radix tree。否则说明file_area已经分配了，就不用再管了*/
		parent_node = hot_cold_file_area_tree_lookup_and_create(&p_file_stat->hot_cold_file_area_tree_root_node,area_index_for_page,&page_slot_in_tree);
		if(IS_ERR(parent_node)){
			ret = -1;
			printk("%s hot_cold_file_area_tree_lookup_and_create fail\n",__func__);
			goto out;
		}
		/*一个node FILE_AREA_PER_NODE(64)个file_area。下边靠着page_slot_in_tree++依次遍历这64个file_area，如果*page_slot_in_tree
		 *是NULL，说明是空洞file_area，之前这个file_area对应的page没有分配，也没有分配file_area，于是按照area_index_for_page<<PAGE_COUNT_IN_AREA_SHIFT
		 *这个page索引，在此查找page是否分配了，是的话就分配file_area*/
		for(j = 0;j < FILE_AREA_PER_NODE - 1;){

			printk("2:%s file_stat:0x%llx i:%d j:%d page_slot_in_tree:0x%llx\n",__func__,(u64)p_file_stat,i,j,(u64)(*page_slot_in_tree));
			//如果是空树，parent_node是NULL，page_slot_in_tree是NULL，*page_slot_in_tree会导致crash
			if(NULL == *page_slot_in_tree){
				//第一次area_index_for_page是0时，start_page_index取值，依次是 0*4 、1*4、2*4、3*4....63*4
				start_page_index = (area_index_for_page + j) << PAGE_COUNT_IN_AREA_SHIFT;
				//page索引超过文件最大page数，结束遍历
				if(start_page_index > file_page_count){
					printk("3:%s start_page_index:%d > file_page_count:%d\n",__func__,start_page_index,file_page_count);
					ret = 1;
					goto out;
				}
				for(k = 0;k < PAGE_COUNT_IN_AREA;k++){
					/*这里需要优化，遍历一次radix tree就得到4个page，完全可以实现的，节省性能$$$$$$$$$$$$$$$$$$$$$$$$*/
					page = xa_load(&mapping->i_pages, start_page_index + k);
					//空洞file_area的page被分配了，那就分配file_area
					if (page && !xa_is_value(page) && page_mapped(page)) {

						//分配file_area并初始化，成功返回0，函数里边把新分配的file_area赋值给*page_slot_in_tree，即在radix tree的槽位
						if(file_area_alloc_and_init(parent_node,page_slot_in_tree,page->index >> PAGE_COUNT_IN_AREA_SHIFT,p_file_stat) < 0){
							ret = -1;
							goto out;
						}
						printk("3:%s file_stat:0x%llx alloc file_area:0x%llx\n",__func__,(u64)p_file_stat,(u64)(*page_slot_in_tree));
						/*4个连续的page只要有一个在radix tree找到，分配file_area,之后就不再查找其他page了*/
						break;
					}
				}
				printk("3:%s start_page_index:%d > file_page_count:%d\n",__func__,start_page_index,file_page_count);
				ret = 1;
				goto out;
			}
			for(k = 0;k < PAGE_COUNT_IN_AREA;k++){
				/*这里需要优化，遍历一次radix tree就得到4个page，完全可以实现的，节省性能$$$$$$$$$$$$$$$$$$$$$$$$*/
				page = xa_load(&mapping->i_pages, start_page_index + k);
				//空洞file_area的page被分配了，那就分配file_area
				if (page && !xa_is_value(page) && page_mapped(page)) {

					//分配file_area并初始化，成功返回0，函数里边把新分配的file_area赋值给*page_slot_in_tree，即在radix tree的槽位
					if(file_area_alloc_and_init(parent_node,page_slot_in_tree,page->index >> PAGE_COUNT_IN_AREA_SHIFT,p_file_stat) < 0){
						ret = -1;
						goto out;
					}
					printk("3:%s file_stat:0x%llx alloc file_area:0x%llx\n",__func__,(u64)p_file_stat,(u64)(*page_slot_in_tree));
					/*4个连续的page只要有一个在radix tree找到，分配file_area,之后就不再查找其他page了*/
					break;
				}
			}
		}
		/*这里有个重大bug，当保存file_area的radix tree的file_area全被释放了，是个空树，此时area_index_for_page指向的是radix tree的根节点的指针的地址，
		 * 即area_index_for_page指向 p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址，然后这里的page_slot_in_tree ++就有问题了。
		 * 原本的设计，area_index_for_page最初指向的是node节点node->slot[64]数组槽位0的slot的地址，然后page_slot_in_tree++依次指向槽位0到槽位63
		 * 的地址。然后看*page_slot_in_tree是否是NULL，是的话说明file_area已经分配。否则说明是空洞file_area，那就要执行xa_load()探测对应索引
		 * 的文件页是否已经分配并插入radix tree(保存page指针的radix tree)了，是的话就file_area_alloc_and_init分配file_area并保存到
		 * page_slot_in_tree指向的保存file_area的radix tree。..........但是，现在保存file_area的radix tree，是个空树，area_index_for_page
		 * 经过上边hot_cold_file_area_tree_lookup探测索引是0的file_area后，指向的是该radix tree的根节点指针的地址，
		 * 即p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址。没办法，这是radix tree的特性，如果只有一个索引是0的成员，该成员
		 * 就是保存在radix tree的根节点指针里。如此，page_slot_in_tree ++就内存越界了，越界到p_file_stat->hot_cold_file_area_tree_root_node
		 * 成员的后边，即p_file_stat的file_stat_lock、max_file_area_age、recent_access_age等成员里，然后对应page分配的话，就要创建新的
		 * file_area并保存到 p_file_stat的file_stat_lock、max_file_area_age、recent_access_age里，导致这些应该是0的成员但却很大。
		 *
		 * 解决办法是，如果该radix tree是空树，先xa_load()探测索引是0的file_aera对应的索引是0~3的文件页page是否分配了，是的话就创建file_area并保存到
		 * radix tree的p_file_stat->hot_cold_file_area_tree_root_node->root_node。然后不令page_slot_in_tree ++，而是xa_load()探测索引是1的file_aera
		 * 对应的索引是4~7的文件页page是否分配了，是的话，直接执行hot_cold_file_area_tree_lookup_and_create创建这个file_area，不是探测结束。
		 * 并且要令p_file_stat->last_index恢复0，这样下次执行该函数还是从索引是0的file_area开始探测，然后探测索引是1的file_area对应的文件页是否分配了。
		 * 这样有点啰嗦，并且会重复探测索引是0的file_area。如果索引是1的file_area的文件页page没分配，那索引是2的file_area的文件页page被分配了。
		 * 现在的代码就有问题了，不会针对这这种情况分配索引是2的file_area*/
		//page_slot_in_tree ++;

		if((NULL == parent_node) && (0 == j) && (0 == area_index_for_page)){
			printk("4:%s file_stat:0x%llx page_slot_in_tree:0x%llx_0x%llx j:%d\n",__func__,(u64)p_file_stat,(u64)page_slot_in_tree,(u64)&p_file_stat->hot_cold_file_area_tree_root_node.root_node,j);
			for(k = 0;k < PAGE_COUNT_IN_AREA;k++){
				/*探测索引是1的file_area对应的文件页page是否分配了，是的话就创建该file_area并插入radix tree*/
				page = xa_load(&mapping->i_pages, PAGE_COUNT_IN_AREA + k);
				if (page && !xa_is_value(page) && page_mapped(page)) {
					//此时file_area的radix tree还是空节点，现在创建根节点node，函数返回后page_slot_in_tree指向的是根节点node->slots[]数组槽位1的地址，下边
					//file_area_alloc_and_init再分配索引是1的file_area并添加到插入radix tree，再赋值高node->slots[1]，即槽位1
					parent_node = hot_cold_file_area_tree_lookup_and_create(&p_file_stat->hot_cold_file_area_tree_root_node,1,&page_slot_in_tree);
					if(IS_ERR(parent_node)){
						ret = -1;
						printk("%s hot_cold_file_area_tree_lookup_and_create fail\n",__func__);
						goto out;
					}

					if(NULL == parent_node || *page_slot_in_tree != NULL){
						panic("%s parent_node:0x%llx *page_slot_in_tree:0x%llx\n",__func__,(u64)parent_node,(u64)(*page_slot_in_tree));
					}
					//分配file_area并初始化，成功返回0，函数里边把新分配的file_area赋值给*page_slot_in_tree，即在radix tree的槽位
					if(file_area_alloc_and_init(parent_node,page_slot_in_tree,1,p_file_stat) < 0){
						ret = -1;
						goto out;
					}
					printk("5:%s file_stat:0x%llx alloc file_area:0x%llx\n",__func__,(u64)p_file_stat,(u64)(*page_slot_in_tree));
					/*4个连续的page只要有一个在radix tree找到，分配file_area,之后就不再查找其他page了*/
					break;
				}
			}

			/*如果parent_node不是NULL。说明上边索引是0的file_area的对应文件页page分配了，创建的根节点，parent_node就是这个根节点。并且，令j加1.
			 *这样下边再就j加1，j就是2了，page_slot_in_tree = parent_node.slots[j]指向的是索引是2的file_area，然后探测对应文件页是否分配了。
			 *因为索引是0、1的file_area已经探测过了。如果 parent_node是NULL，那说明索引是1的file_area对应的文件页page没分配，上边也没有创建
			 *根节点。于是令p_file_stat->last_index清0，直接goto out，这样下次执行该函数，还是从索引是0的file_area开始探测。这样有个问题，如经
			 *索引是1的file_area对应文件页没分配，这类直接goto out了，那索引是2的file_area对应的文件页分配，就不理会了。这种可能是存在的！索引
			 *是2的file_area的文件页page也应该探测呀，后溪再改进吧
			 */
			if(parent_node){
				j ++;
			}else{
				p_file_stat->last_index = 0;
				goto out;
			}
		}
		//j加1令page_slot_in_tree指向下一个file_area
		j++;
		//不用page_slot_in_tree ++了，虽然性能好点，但是内存越界了也不知道。page_slot_in_tree指向下一个槽位的地址
		page_slot_in_tree = &parent_node->slots[j];
#ifdef __LITTLE_ENDIAN//这个判断下端模式才成立
		if((u64)page_slot_in_tree < (u64)(&parent_node->slots[0]) || (u64)page_slot_in_tree > (u64)(&parent_node->slots[TREE_MAP_SIZE])){		
			panic("%s page_slot_in_tree:0x%llx error 0x%llx_0x%llx\n",__func__,(u64)page_slot_in_tree,(u64)(&parent_node->slots[0]),(u64)(&parent_node->slots[TREE_MAP_SIZE]));
		}
#endif	
		//page_slot_in_tree ++;
	    //area_index_for_page的取值，0，后续依次是64*1、64*2、64*3等等，
	    area_index_for_page += FILE_AREA_PER_NODE;
    }
	//p_file_stat->last_index记录下次要查找的第一个node节点的file_area的索引
	p_file_stat->last_index = area_index_for_page;
out:
	if(start_page_index > file_page_count){
		//p_file_stat->last_index清0，下次从头开始扫描文件页
		p_file_stat->last_index = 0;
	}
	return ret;
}
#endif

#if 0
/*跟reverse_file_stat_radix_tree_hole函数一样，现在不需要了*/
static int check_page_exist_and_create_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,struct hot_cold_file_area_tree_node **_parent_node,void ***_page_slot_in_tree,unsigned int area_index)
{
	/*空树时函数返回NULL并且page_slot_in_tree指向root->root_node的地址。当传入索引很大找不到file_area时，函数返回NULL并且page_slot_in_tree不会被赋值(保持原值NULL)*/

	int ret = 0;
	int k;
	pgoff_t start_page_index;
	struct page *page;
	struct page *pages[PAGE_COUNT_IN_AREA];

	//struct address_space *mapping = p_file_stat->mapping;
	void **page_slot_in_tree = *_page_slot_in_tree;
	struct hot_cold_file_area_tree_node *parent_node = *_parent_node;
	//file_area的page有一个mapcount大于1，则是mapcount file_area，再把mapcount_file_area置1
	bool mapcount_file_area = 0;
	struct file_area *p_file_area = NULL;

	start_page_index = (area_index) << PAGE_COUNT_IN_AREA_SHIFT;
    
	memset(pages,0,PAGE_COUNT_IN_AREA*sizeof(struct page *));
	//获取p_file_area对应的文件页page指针并保存到pages数组
	ret = get_page_from_file_area(p_file_stat,start_page_index,pages);
	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx start_page_index:%ld get %d page\n",__func__,(u64)p_file_stat,start_page_index,ret);

	if(ret <= 0)
	    goto out; 

	/*探测area_index对应file_area索引的page是否分配了，分配的话则分配对应的file_area。但是如果父节点不存在，需要先分配父节点*/
	for(k = 0;k < PAGE_COUNT_IN_AREA;k++){
		/*探测索引是1的file_area对应的文件页page是否分配了，是的话就创建该file_area并插入radix tree*/
		//page = xa_load(&mapping->i_pages, start_page_index + k);
		page= pages[k];
		//area_index对应file_area索引的page存在
		if (page && !xa_is_value(page) && page_mapped(page)){

			if( 0 == mapcount_file_area && page_mapcount(page) > 1)
				mapcount_file_area = 1;

			//父节点不存在则创建父节点，并令page_slot_in_tree指向area_index索引对应file_area在父节点的槽位parent_node.slots[槽位索引]槽位地址
			if(NULL == parent_node){//parent_node是NULL，page_slot_in_tree一定也是NULL
				parent_node = hot_cold_file_area_tree_lookup_and_create(&p_file_stat->hot_cold_file_area_tree_root_node,area_index,&page_slot_in_tree);
				if(IS_ERR(parent_node)){
					ret = -1;
					printk("%s hot_cold_file_area_tree_lookup_and_create fail\n",__func__);
					goto out;
				}

			}
			/*到这里，page_slot_in_tree一定不是NULL，是则触发crash。如果parent_node是NULL是有可能的，当radix tree是空树时。查找索引是0的file_area
			 *时，parent_node是NULL，page_slot_in_tree指向p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址。否则，其他情况触发crash*/
			if((area_index != 0 && NULL == parent_node) || (NULL == page_slot_in_tree)){
				panic("%s parent_node:0x%llx *page_slot_in_tree:0x%llx\n",__func__,(u64)parent_node,(u64)(*page_slot_in_tree));
			}
			p_file_area = file_area_alloc_and_init(parent_node,page_slot_in_tree,area_index,p_file_stat);
			//分配file_area并初始化，成功返回0，函数里边把新分配的file_area赋值给*page_slot_in_tree，即在radix tree的槽位
			if(NULL == p_file_area){
				ret = -1;
				goto out;
			}
			//在file_stat->file_area_temp链表的file_area个数加1
			p_file_stat->file_area_count_in_temp_list ++;

			ret = 1;
			//令_page_slot_in_tree指向新分配的file_area在radix tree的parent_node.slots[槽位索引]槽位地址
			if(NULL == *_page_slot_in_tree)
				*_page_slot_in_tree = page_slot_in_tree;

			//新分配的parent_node赋值给*_parent_node
			if(NULL == *_parent_node)
				*_parent_node = parent_node;

			//只要有一个page在radix tree找到，分配file_area,之后就不再查找其他page了
			break;
		}
	}

	/*如果上边for循环遍历的file_area的page的mapcount都是1，且file_area的page上边没有遍历完，则这里继续遍历完剩余的page*/
	while(0 == mapcount_file_area && k < PAGE_COUNT_IN_AREA){
		page= pages[k];
		if (page && !xa_is_value(page) && page_mapped(page) && page_mapcount(page) > 1){
			mapcount_file_area = 1;
		}
		k ++;
	}
	if(mapcount_file_area){
		//file_stat->temp 链表上的file_area个数减1
		p_file_stat->file_area_count_in_temp_list --;
		//文件file_stat的mapcount的file_area个数加1
		p_file_stat->mapcount_file_area_count ++;
		//file_area的page的mapcount大于1，则把file_area移动到file_stat->file_area_mapcount链表
		clear_file_area_in_temp_list(p_file_area);
		set_file_area_in_mapcount_list(p_file_area);
		list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);
		if(shrink_page_printk_open)
			printk("5:%s file_stat:0x%llx file_area:0x%llx state:0x%x is mapcount file_area\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

		/*如果文件file_stat的mapcount的file_area个数超过阀值，则file_stat被判定为mapcount file_stat而移动到
		 *global mmap_file_stat_mapcount_head链表。但前提file_stat必须在temp_file链表或temp_large_file链表*/
		if(is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat) && file_stat_in_file_stat_temp_head_list(p_file_stat)){
			 if(file_stat_in_file_stat_temp_head_list_error(p_file_stat))
				 panic("%s file_stat:0x%llx status error:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

			 clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			 set_file_stat_in_mapcount_file_area_list(p_file_stat);
			 list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_mapcount_head);
		 	 p_hot_cold_file_global->mapcount_mmap_file_stat_count ++;
			 if(shrink_page_printk_open1)
				 printk("6:%s file_stat:0x%llx status:0x%llx is mapcount file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
		}
	}

out:
	return ret;
}

/*reverse_file_stat_radix_tree_hole()最初是为了ko模式异步内存回收设计的:保存file_area指针的是radix tree，保存page指针的
 *是另一个xarray tree，二者独立。因此，需要时不时的执行该函数，遍历xarray tree，探测一个个page，看哪些page还没有创建file_area。
  没有的话则创建file_area并添加到file_stat->temp链表，后续就可以探测这些page冷热，参与内存回收。

  然而，现在file_area和page是一体了，file_area指针保存在原保存page指针的xarray tree，page指针保存在file_area->pages[]数组。
  后续，如果该mmap文件访问任意一个新的page，先分配一个page，最后执行到 __filemap_add_folio->__filemap_add_folio_for_file_area()
  ->file_area_alloc_and_init()函数，必然分配file_area并添加到file_stat->temp链表，然后把新的page保存到file_area->pages[]
  数组。也就是说，现在不用再主动执行reverse_file_stat_radix_tree_hole()探测空洞page然后创建file_area了。内核原有机制就可以
  保证mmap的文件一旦有新的page，自己创建file_area并添加到file_stat->temp链表。后续异步内存回收线程就可以探测该file_area的
  冷热page，然后内存回收。
*/
/*文件的radix tree在遍历完一次所有的page后，可能存在空洞，于是后续还要再遍历文件的radix tree获取之前没有遍历到的page*/
static unsigned int reverse_file_stat_radix_tree_hole(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat)
{
	int i,j;
	struct hot_cold_file_area_tree_node *parent_node;
	void **page_slot_in_tree = NULL;
	unsigned int base_area_index;
	int ret = 0;
	unsigned int file_page_count = p_file_stat->mapping->host->i_size >> PAGE_SHIFT;//除以4096
	unsigned int start_page_index = 0;
	struct file_area *p_file_area;

	//p_file_stat->traverse_done是0，说明还没有遍历完一次文件的page，那先返回
	if(0 == p_file_stat->traverse_done)
		return ret;
	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx file_stat->last_index:%ld\n",__func__,(u64)p_file_stat,p_file_stat->last_index);

	//p_file_stat->last_index初值是0，后续依次是64*1、64*2、64*3等等，
	base_area_index = p_file_stat->last_index;
	//一次遍历SCAN_FILE_AREA_NODE_COUNT个node，一个node 64个file_area
	for(i = 0;i < SCAN_FILE_AREA_NODE_COUNT;i++){
		//每次必须对page_slot_in_tree赋值NULL，下边hot_cold_file_area_tree_lookup()如果没找到对应索引的file_area，page_slot_in_tree还是NULL
		page_slot_in_tree = NULL;
		j = 0;

		/*查找索引0、64*1、64*2、64*3等等的file_area的地址，保存到page_slot_in_tree。page_slot_in_tree指向的是每个node节点的第一个file_area，
		 *每个node节点一共64个file_area，都保存在node节点的slots[64]数组。下边的for循环一次查找node->slots[0]~node->slots[63]，如果是NULL，
		 *说明还没有分配file_area，是空洞，那就分配file_area并添加到radix tree。否则说明file_area已经分配了，就不用再管了*/

		/*不能用hot_cold_file_area_tree_lookup_and_create，如果是空树，但是去探测索引是1的file_area，此时会直接分配索引是1的file_area对应的node节点
		 *并插入radix tree，注意是分配node节点。而根本不管索引是1的file_area对应的文件页page是否分配了。这样会分配很多没用的node节点，而不管对应索引的
		 *file_area的文件页是否分配了，浪费内存。这里只能探测file_area是否存在，不能node节点*/
		//parent_node = hot_cold_file_area_tree_lookup_and_create(&p_file_stat->hot_cold_file_area_tree_root_node,base_area_index,&page_slot_in_tree);

		/*空树时函数返回NULL并且page_slot_in_tree指向root->root_node的地址。当传入索引很大找不到file_area时，函数返回NULL并且page_slot_in_tree不会被赋值(保持原值NULL)*/
		parent_node = hot_cold_file_area_tree_lookup(&p_file_stat->hot_cold_file_area_tree_root_node,base_area_index,&page_slot_in_tree);
		if(IS_ERR(parent_node)){
			ret = -1;
			printk("2:%s hot_cold_file_area_tree_lookup_and_create fail\n",__func__);
			goto out;
		}
		/*一个node FILE_AREA_PER_NODE(64)个file_area。下边靠着page_slot_in_tree++依次遍历这64个file_area，如果*page_slot_in_tree
		 *是NULL，说明是空洞file_area，之前这个file_area对应的page没有分配，也没有分配file_area，于是按照base_area_index<<PAGE_COUNT_IN_AREA_SHIFT
		 *这个page索引，在此查找page是否分配了，是的话就分配file_area*/
		while(1){
			/* 1：hot_cold_file_area_tree_lookup中找到对应索引的file_area，parent_node非NULL，page_slot_in_tree和*page_slot_in_tree都非NULL
			 * 2：hot_cold_file_area_tree_lookup中没找到对应索引的file_area，但是父节点存在，parent_node非NULL，page_slot_in_tree非NULL，*page_slot_in_tree是NULL
			 * 3：hot_cold_file_area_tree_lookup中没找到对应索引的file_area，父节点也不存在，parent_node是NULL，page_slot_in_tree是NULL，此时不能出现*page_slot_in_tree
			 * 另外，如果radix tree是空树，lookup索引是0的file_area后，page_slot_in_tree指向p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址，
			 *    parent_node是NULL，page_slot_in_tree和*page_slot_in_tree都非NULL
			 *
			 * 简单说，
			 * 情况1：只要待查找索引的file_area的父节点存在，parent_node不是NULL，page_slot_in_tree也一定不是NULL，page_slot_in_tree指向保存
			 * file_area指针在父节点的槽位地址，即parent_node.slot[槽位索引]的地址。如果file_area存在则*page_slot_in_tree非NULL，否则*page_slot_in_tree是NULL
			 * 情况2：待查找的file_area的索引太大，没找到父节点，parent_node是NULL，page_slot_in_tree也是NULL，此时不能用*page_slot_in_tree，会crash
			 * 情况3：radix tree是空树，lookup索引是0的file_area后， parent_node是NULL，page_slot_in_tree非NULL，指向p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址
			 * */

			start_page_index = (base_area_index + j) << PAGE_COUNT_IN_AREA_SHIFT;
			if(start_page_index >= file_page_count){
				if(shrink_page_printk_open)
					printk("3:%s start_page_index:%d >= file_page_count:%d\n",__func__,start_page_index,file_page_count);

				goto out;
			}
			if(shrink_page_printk_open){
				if(page_slot_in_tree)
					printk("4:%s file_stat:0x%llx i:%d j:%d start_page_index:%d base_area_index:%d parent_node:0x%llx page_slot_in_tree:0x%llx *page_slot_in_tree:0x%llx\n",__func__,(u64)p_file_stat,i,j,start_page_index,base_area_index,(u64)parent_node,(u64)page_slot_in_tree,(u64)(*page_slot_in_tree));
				else
					printk("4:%s file_stat:0x%llx i:%d j:%d start_page_index:%d base_area_index:%d parent_node:0x%llx page_slot_in_tree:0x%llx\n",__func__,(u64)p_file_stat,i,j,start_page_index,base_area_index,(u64)parent_node,(u64)page_slot_in_tree);
			}

			/* (NULL == page_slot_in_tree)：对应情况2，radix tree现在节点太少，待查找的file_area索引太大找不到父节点和file_area的槽位，
			 * parent_node 和 page_slot_in_tree都是NULL。那就执行check_page_exist_and_create_file_area()分配父节点 parent_node，并令page_slot_in_tree指向
			 * parent_node->slots[槽位索引]槽位，然后分配对应索引的file_area并保存到parent_node->slots[槽位索引]
			 *
			 * (NULL!= page_slot_in_tree && NULL == *page_slot_in_tree)对应情况1和情况3。情况1：找到对应索引的file_area的槽位，即parent_node.slot[槽位索引]，
			 * parent_node 和 page_slot_in_tree都非NULL，但*page_slot_in_tree是NULL，那就执行check_page_exist_and_create_file_area()分配对应索引的file_area结构
			 * 并保存到parent_node.slot[槽位索引]。 情况3：radix tree是空树，lookup索引是0的file_area后， parent_node是NULL，page_slot_in_tree非NULL，
			 * page_slot_in_tree指向p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址，但如果*page_slot_in_tree是NULL，说明file_area没有分配，
			 * 那就执行check_page_exist_and_create_file_area()分配索引是0的file_area并保存到 p_file_stat->hot_cold_file_area_tree_root_node->root_node。
			 *
			 * */
			if((NULL == page_slot_in_tree)  || (NULL!= page_slot_in_tree && NULL == *page_slot_in_tree)){
				ret = check_page_exist_and_create_file_area(p_hot_cold_file_global,p_file_stat,&parent_node,&page_slot_in_tree,base_area_index + j);
				if(ret < 0){
					printk("5:%sheck_page_exist_and_create_file_area fail\n",__func__);
					goto out;
				}else if(ret >0){
					//ret 大于0说明上边创建了file_area或者node节点，这里再打印出来
					if(shrink_page_printk_open)
						printk("6:%s file_stat:0x%llx i:%d j:%d start_page_index:%d base_area_index:%d parent_node:0x%llx page_slot_in_tree:0x%llx *page_slot_in_tree:0x%llx\n",__func__,(u64)p_file_stat,i,j,start_page_index,base_area_index,(u64)parent_node,(u64)page_slot_in_tree,(u64)(*page_slot_in_tree));
				}
			}

			if(page_slot_in_tree){
				barrier();
				if(*page_slot_in_tree){
					p_file_area = (struct file_area *)(*page_slot_in_tree);
					//file_area自身保存的索引数据 跟所在radix tree的槽位位置不一致，触发crash
					if((p_file_area->start_index >>PAGE_COUNT_IN_AREA_SHIFT) != base_area_index + j)
						panic("%s file_area index error!!!!!! file_stat:0x%llx p_file_area:0x%llx p_file_area->start_index:%ld base_area_index:%d j:%d\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->start_index,base_area_index,j);

				}
			}
			/*
			//情况1：只要待查找索引的file_area的父节点存在，parent_node不是NULL，page_slot_in_tree也一定不是NULL
			if(parent_node){
			    //待查找索引的file_area不存在，则探测它对应的page是否存在，存在的话则分配file_area
			    if(NULL == *page_slot_in_tree){
					if(check_page_exist_and_create_file_area(p_hot_cold_file_global,p_file_stat,&parent_node,&page_slot_in_tree,base_area_index + j) < 0){
			           goto out;
			        }
			    }
			    //待查找索引的file_area存在，什么都不用干
			    else{}
			}
			else
			{
				//情况2：待查找的file_area的索引太大，没找到父节点，parent_node是NULL，page_slot_in_tree也是NULL，此时不能用*page_slot_in_tree，会crash
				if(NULL == page_slot_in_tree){
					if(check_page_exist_and_create_file_area(p_hot_cold_file_global,p_file_stat,&parent_node,&page_slot_in_tree,base_area_index+j) < 0){
					    goto out;
					}
					//到这里，如果指定索引的file_area的page存在，则创建父节点和file_area，parent_node和page_slot_in_tree不再是NULL，*ppage_slot_in_tree也非NULL
				}
				//情况3：radix tree是空树，lookup索引是0的file_area后， parent_node是NULL，page_slot_in_tree非NULL，指向*p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址
				else{
					if((0 == j) && (0 == base_area_index)&& (page_slot_in_tree ==  &p_file_stat->hot_cold_file_area_tree_root_node.root_node)){
						//如果索引是0的file_area不存在，则探测对应page是否存在，存在的话创建索引是0的file_area，不用创建父节点，file_area指针保存在p_file_stat->hot_cold_file_area_tree_root_node->root_node
						if(NULL == *page_slot_in_tree){
							if(check_page_exist_and_create_file_area(p_hot_cold_file_global,p_file_stat,&parent_node,&page_slot_in_tree,base_area_index + j) < 0){
								goto out;
							}
						}
					}else{
						if(check_page_exist_and_create_file_area(p_hot_cold_file_global,p_file_stat,&parent_node,&page_slot_in_tree,base_area_index + j) < 0){
						    goto out;
						}
						//这里可能进入，空树时，探测索引很大的file_area
						printk("%s j:%d base_area_index:%d page_slot_in_tree:0x%llx_0x%llx error!!!!!!!!!\n",__func__,j,base_area_index,(u64)page_slot_in_tree,(u64)&p_file_stat->hot_cold_file_area_tree_root_node.root_node);
					}
				}
			}
			*/			
			//依次只能遍历FILE_AREA_PER_NODE 个file_area
			if(j >= FILE_AREA_PER_NODE - 1)
				break;

			//j加1令page_slot_in_tree指向下一个file_area
			j++;
			if(parent_node){
				//不用page_slot_in_tree ++了，虽然性能好点，但是内存越界了也不知道。page_slot_in_tree指向下一个槽位的地址
				page_slot_in_tree = &parent_node->slots[j];
#ifdef __LITTLE_ENDIAN//这个判断下端模式才成立
				if((u64)page_slot_in_tree < (u64)(&parent_node->slots[0]) || (u64)page_slot_in_tree > (u64)(&parent_node->slots[TREE_MAP_SIZE])){		
					panic("%s page_slot_in_tree:0x%llx error 0x%llx_0x%llx\n",__func__,(u64)page_slot_in_tree,(u64)(&parent_node->slots[0]),(u64)(&parent_node->slots[TREE_MAP_SIZE]));
				}
#endif
			}else{
				/*到这里，应该radix tree是空树时才成立，要令page_slot_in_tree指向NULL，否则当前这个for循环的page_slot_in_tree值会被错误用到下个循环*/
				page_slot_in_tree = NULL;
			}
		}
		//base_area_index的取值，0，后续依次是64*1、64*2、64*3等等，
		base_area_index += FILE_AREA_PER_NODE;
	}
	//p_file_stat->last_index记录下次要查找的第一个node节点的file_area的索引
	p_file_stat->last_index = base_area_index;
out:
	if((ret >= 0) && ((base_area_index +j) << PAGE_COUNT_IN_AREA_SHIFT >= file_page_count)){
		if(shrink_page_printk_open1)
			printk("7:%s last_index = 0 last_index:%ld base_area_index:%d j:%d file_page_count:%d\n",__func__,p_file_stat->last_index,base_area_index,j,file_page_count);

		//p_file_stat->last_index清0，下次从头开始扫描文件页
		p_file_stat->last_index = 0;
	}
	return ret;
}
#endif

#if 0
static noinline unsigned int mmap_file_stat_small_list_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_small *p_file_stat_small,unsigned int scan_file_area_max,unsigned int *already_scan_file_area_count,unsigned char file_stat_list_type,struct list_head *file_area_have_cache_page_head)
{
	struct file_area *p_file_area,*p_file_area_temp;
	LIST_HEAD(file_area_move_to_temp_list);
	struct page *page_buf[BUF_PAGE_COUNT];
	int cold_page_count = 0,cold_page_count_last;
	int ret = 0;
	//unsigned int scan_file_area_count = 0;
	unsigned int reclaimed_pages = 0;
	unsigned int isolate_pages = 0;
	unsigned int scan_cold_file_area_count = 0;
	unsigned int file_stat_scan_status = FILE_STAT_SCAN_ALL_FILE_AREA;
	//unsigned int warm_to_temp_file_area_count = 0;

	//从链表尾开始遍历，链表尾的成员更老，链表头的成员是最新添加的
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_small->file_area_temp,file_area_list){
		//if(scan_file_area_count ++ > scan_file_area_max)
		//	break;

		//if(!file_area_in_warm_list(p_file_area) || file_area_in_warm_list_error(p_file_area))
		//	panic("%s:1 file_area:0x%llx status:0x%x not in file_area_warm\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

		/*遍历file_stat->file_area_temp，查找冷的file_area*/
		cold_page_count_last = cold_page_count;

		if(!file_area_in_temp_list(p_file_area)){
			if(file_area_in_free_list(p_file_area))
				reverse_other_file_area_list_common(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,p_file_area,FILE_AREA_FREE,FILE_STAT_SMALL);
			else if(file_area_in_mapcount_list(p_file_area))
				reverse_other_file_area_list_common(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,p_file_area,FILE_AREA_MAPCOUNT,FILE_STAT_SMALL);
			else if(file_area_in_hot_list(p_file_area))
				reverse_other_file_area_list_common(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,p_file_area,FILE_AREA_HOT,FILE_STAT_SMALL);
			else if(file_area_in_refault_list(p_file_area))
				reverse_other_file_area_list_common(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,p_file_area,FILE_AREA_REFAULT,FILE_STAT_SMALL);

			continue;
		}

		ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,p_file_area,page_buf,&cold_page_count,file_stat_list_type,file_area_have_cache_page_head);

		/*file_area的page都没有被访问，如果很冷的则参与内存，如果不太冷的则移动到临时链表*/
		if(0 == ret){

			/*file_area必须处于file_stat->temp链表，否则会出bug，如果file_area没有被访问，但是file_area被判定为
			 *mapcount file_area而移动到了file_stat->mapcount链表，则此时就不能再把file_area再移动到其他链表，否则file_area
			 就会处于两种链表，状态就大错特错了*/
			//if(!file_area_in_warm_list(p_file_area) || file_area_in_warm_list_error(p_file_area))
			//	panic("%s:2 file_area:0x%llx status:0x%x not in file_area_warm\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			/*二者不相等，说明file_area是冷的，并且它的page的pte本次检测也没被访问，这种情况才回收这个file_area的page。
			 *并且，如果当前p_file_area被被判定是热的或者mapcount file_area，*/

			if(cold_page_count_last != cold_page_count)
			{
				//处于file_stat->tmep链表上的file_area，移动到其他链表时，要先对file_area的access_count清0，否则会影响到
				//file_area->file_area_access_age变量，因为file_area->access_count和file_area_access_age是共享枚举变量
				file_area_access_count_clear(p_file_area);

				//spin_lock(&p_file_stat->file_stat_lock);  file_stat->warm链表的file_area移动到file_stat->free链表不用加锁

				clear_file_area_in_warm_list(p_file_area);
				set_file_area_in_free_list(p_file_area);
				//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free);
				scan_cold_file_area_count ++;

				//spin_unlock(&p_file_stat->file_stat_lock);

			}else{
				/*如果file_area的page没被访问，但是file_area还不是冷的，file_area不太冷，这里先不处理。下边
				 *会执行list_move_enhance()把这些file_area移动到链表头。其实这样的处理不太好，这种file_stat->warm链表
				 不冷不热的file_area最好一直保持在链表靠近尾巴的位置最好，按照冷热程度排序，最好，目前实现起来有难度*/
			}
		}

		/*1:凑够BUF_PAGE_COUNT个要回收的page，if成立，则开始隔离page、回收page
		 *2:page_buf剩余的空间不足容纳PAGE_COUNT_IN_AREA个page，if也成立，否则下个循环执行check_one_file_area_cold_page_and_clear函数
		 *向page_buf保存PAGE_COUNT_IN_AREA个page，将导致内存溢出*/
		if(cold_page_count >= BUF_PAGE_COUNT || (BUF_PAGE_COUNT - cold_page_count <=  PAGE_COUNT_IN_AREA)){

			isolate_pages += cold_mmap_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,p_file_area,page_buf,cold_page_count);
			reclaimed_pages = p_hot_cold_file_global->mmap_file_shrink_counter.mmap_free_pages_count;
			cold_page_count = 0;
			if(shrink_page_printk_open)
				printk("3:%s file_stat:0x%llx reclaimed_pages:%d isolate_pages:%d\n",__func__,(u64)p_file_stat_small,reclaimed_pages,isolate_pages);
		}

		*already_scan_file_area_count = *already_scan_file_area_count + 1;
		//超过本轮扫描的最大file_area个数则结束本次的遍历
		if(*already_scan_file_area_count >= scan_file_area_max){
			file_stat_scan_status = FILE_STAT_SCAN_MAX_FILE_AREA;
			break;
		}
	}

	//如果本次对文件遍历结束后，有未达到BUF_PAGE_COUNT数目要回收的page，这里就隔离+回收这些page
	if(cold_page_count){
		isolate_pages += cold_mmap_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,p_file_area,page_buf,cold_page_count);
		reclaimed_pages = p_hot_cold_file_global->mmap_file_shrink_counter.mmap_free_pages_count;
		if(shrink_page_printk_open)
			printk("5:%s  file_stat:0x%llx reclaimed_pages:%d isolate_pages:%d\n",__func__/*,p_file_stat->file_name*/,(u64)p_file_stat_small,reclaimed_pages,isolate_pages);
	}


	if(shrink_page_printk_open1)
		printk("%s file_stat:0x%llx already_scan_file_area_count:%d reclaimed_pages:%d isolate_pages:%d\n",__func__/*,p_file_stat->file_name*/,(u64)p_file_stat_small,*already_scan_file_area_count,reclaimed_pages,isolate_pages);

	/*
	   p_hot_cold_file_global->mmap_file_shrink_counter.isolate_lru_pages_from_warm += isolate_pages;
	   p_hot_cold_file_global->mmap_file_shrink_counter.scan_cold_file_area_count_from_warm += scan_cold_file_area_count;
	   p_hot_cold_file_global->mmap_file_shrink_counter.warm_to_temp_file_area_count += warm_to_temp_file_area_count;
	   */
	return file_stat_scan_status;
}
#endif
static noinline unsigned int mmap_file_stat_warm_list_file_area_solve(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,unsigned int scan_file_area_max,unsigned int *already_scan_file_area_count,unsigned int file_stat_list_type,struct list_head *file_area_have_cache_page_head,unsigned int file_type)
{
	struct file_area *p_file_area,*p_file_area_temp;
	LIST_HEAD(file_area_move_to_temp_list);
	struct page *page_buf[BUF_PAGE_COUNT];
	int cold_page_count = 0,cold_page_count_last;
	int ret = 0;
	unsigned int scan_file_area_count = 0;
	unsigned int reclaimed_pages = 0;
	unsigned int isolate_pages = 0;
	unsigned int scan_cold_file_area_count = 0;
	unsigned int warm_to_temp_file_area_count = 0;

	//从链表尾开始遍历，链表尾的成员更老，链表头的成员是最新添加的
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat->file_area_warm,file_area_list){
		if(scan_file_area_count ++ > scan_file_area_max)
			break;

		if(!file_area_in_warm_list(p_file_area) || file_area_in_warm_list_error(p_file_area))
			panic("%s:1 file_area:0x%llx status:0x%x not in file_area_warm\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

		/*遍历file_stat->file_area_temp，查找冷的file_area*/
		cold_page_count_last = cold_page_count;

		ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area,page_buf,&cold_page_count,file_stat_list_type,file_area_have_cache_page_head,file_type);
		/*以下处理流程跟check_file_area_cold_page_and_clear()函数里对各种file_area的处理基本一致*/

		/*file_area变成了mapcount或hot file_area 或者file_area里有cache文件页而设置file_area_in_free状态并
		 *移动到file_area_have_cache_page_head链表而参与cache文件页的内存回收流程*/
		if(file_area_in_hot_list(p_file_area) || file_area_in_mapcount_list(p_file_area) ||file_area_in_free_list(p_file_area)){

		}
		/*file_area的page都没有被访问，如果很冷的则参与内存，如果不太冷的则移动到临时链表*/
		else if(0 == ret){

			/*file_area必须处于file_stat->temp链表，否则会出bug，如果file_area没有被访问，但是file_area被判定为
			 *mapcount file_area而移动到了file_stat->mapcount链表，则此时就不能再把file_area再移动到其他链表，否则file_area
			 就会处于两种链表，状态就大错特错了*/
			if(!file_area_in_warm_list(p_file_area) || file_area_in_warm_list_error(p_file_area))
				panic("%s:2 file_area:0x%llx status:0x%x not in file_area_warm\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			/*二者不相等，说明file_area是冷的，并且它的page的pte本次检测也没被访问，这种情况才回收这个file_area的page。
			 *并且，如果当前p_file_area被被判定是热的或者mapcount file_area，*/

			if(cold_page_count_last != cold_page_count)
			{
				//处于file_stat->tmep链表上的file_area，移动到其他链表时，要先对file_area的access_count清0，否则会影响到
				//file_area->file_area_access_age变量，因为file_area->access_count和file_area_access_age是共享枚举变量
				file_area_access_count_clear(p_file_area);

				//spin_lock(&p_file_stat->file_stat_lock);  file_stat->warm链表的file_area移动到file_stat->free链表不用加锁

				clear_file_area_in_warm_list(p_file_area);
				set_file_area_in_free_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free);
				scan_cold_file_area_count ++;

				//spin_unlock(&p_file_stat->file_stat_lock);

			}else{
				/*如果file_area的page没被访问，但是file_area还不是冷的，file_area不太冷，这里先不处理。下边
				 *会执行list_move_enhance()把这些file_area移动到链表头。其实这样的处理不太好，这种file_stat->warm链表
				 不冷不热的file_area最好一直保持在链表靠近尾巴的位置最好，按照冷热程度排序，最好，目前实现起来有难度*/
			}
		}else if(ret > 0){
			if(!file_area_in_warm_list(p_file_area) || file_area_in_warm_list_error(p_file_area))
				panic("%s:3 file_area:0x%llx status:%d not in file_area_warm\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			/*如果file_area的page被访问了，则把file_area移动到链表头*/

			//spin_lock(&p_file_stat->file_stat_lock);
			clear_file_area_in_warm_list(p_file_area);
			set_file_area_in_temp_list(p_file_area);
			/*先把符合条件的file_area移动到临时链表，下边再把这些file_area统一移动到file_stat->temp链表*/
			list_move(&p_file_area->file_area_list,&file_area_move_to_temp_list);
			//spin_unlock(&p_file_stat->file_stat_lock);
			warm_to_temp_file_area_count ++;
		}

		/*1:凑够BUF_PAGE_COUNT个要回收的page，if成立，则开始隔离page、回收page
		 *2:page_buf剩余的空间不足容纳PAGE_COUNT_IN_AREA个page，if也成立，否则下个循环执行check_one_file_area_cold_page_and_clear函数
		 *向page_buf保存PAGE_COUNT_IN_AREA个page，将导致内存溢出*/
		if(cold_page_count >= BUF_PAGE_COUNT || (BUF_PAGE_COUNT - cold_page_count <=  PAGE_COUNT_IN_AREA)){

			isolate_pages += cold_mmap_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area,page_buf,cold_page_count);
			reclaimed_pages = p_hot_cold_file_global->mmap_file_shrink_counter.mmap_free_pages_count;
			cold_page_count = 0;
			if(shrink_page_printk_open)
				printk("3:%s file_stat:0x%llx reclaimed_pages:%d isolate_pages:%d\n",__func__,(u64)p_file_stat,reclaimed_pages,isolate_pages);
		}

		*already_scan_file_area_count = *already_scan_file_area_count + 1;
		//超过本轮扫描的最大file_area个数则结束本次的遍历
		if(*already_scan_file_area_count >= scan_file_area_max)
			break;
	}

	//如果本次对文件遍历结束后，有未达到BUF_PAGE_COUNT数目要回收的page，这里就隔离+回收这些page
	if(cold_page_count){
		isolate_pages += cold_mmap_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,&p_file_stat->file_stat_base,p_file_area,page_buf,cold_page_count);
		reclaimed_pages = p_hot_cold_file_global->mmap_file_shrink_counter.mmap_free_pages_count;
		if(shrink_page_printk_open)
			printk("5:%s file_stat:0x%llx reclaimed_pages:%d isolate_pages:%d\n",__func__,(u64)p_file_stat,reclaimed_pages,isolate_pages);
	}

	/*将链表尾已经遍历过的file_area移动到file_stat->warm链表头，下次从链表尾遍历的才是新的未遍历过的file_area。此时不用加锁!!!!!!!!!!!!!*/

	/*有一个重大隐患bug，如果上边的for循环是break跳出，则p_file_area可能就不在file_stat->warm链表了，
	 *此时再把p_file_area到p_file_stat->file_area_warm链表尾的file_area移动到p_file_stat->file_area_warm
	 *链表，file_area就处于错误的file_stat链表了，大bug!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 *怎么解决，先执行 can_file_area_move_to_list_head()函数判定file_area是否处于file_stat->warm链表，
	 *是的话才允许把p_file_area到链表尾的file_area移动到链表头*/
	if(can_file_area_move_to_list_head(p_file_area,&p_file_stat->file_area_warm,F_file_area_in_warm_list))
		list_move_enhance(&p_file_stat->file_area_warm,&p_file_area->file_area_list);

	/*将file_stat->warm链表上近期访问过的file_area移动到file_stat->temp链表头*/
	if(!list_empty(&file_area_move_to_temp_list)){

		spin_lock(&p_file_stat->file_stat_lock);
		list_splice(&file_area_move_to_temp_list,&p_file_stat->file_area_temp);
		spin_unlock(&p_file_stat->file_stat_lock);
	}

	if(shrink_page_printk_open1)
		printk("%s file_stat:0x%llx already_scan_file_area_count:%d reclaimed_pages:%d isolate_pages:%d\n",__func__,/*p_file_stat->file_name,*/(u64)p_file_stat,*already_scan_file_area_count,reclaimed_pages,isolate_pages);

	p_hot_cold_file_global->mmap_file_shrink_counter.isolate_lru_pages_from_warm += isolate_pages;
	p_hot_cold_file_global->mmap_file_shrink_counter.scan_cold_file_area_count_from_warm += scan_cold_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.warm_to_temp_file_area_count += warm_to_temp_file_area_count;

	return scan_file_area_count;
}

/*1:先file_stat->file_area_temp链表尾巴遍历file_area，如果在规定周期被访问次数超过阀值，则判定为热file_area而移动
 * file_stat->file_area_hot链表。如果file_stat->file_area_hot链表的热file_area超过阀值则该文件被判定为热文件，file_stat移动到
 * global hot链表。
 *2:如果ile_stat->file_area_temp上file_area长时间不被访问，则释放掉file_area的page，并把file_area移动到file_stat->file_area_free链表
 *  file_stat->file_area_free 和 file_stat->file_area_free 在这里一个意思。
 *3:遍历file_stat->file_area_refault、hot、mapcount、free链表上file_area，处理各不相同，升级或降级到file_stat->temp链表，或者
 *  释放掉file_area，具体看源码吧
 *4:如果file_stat->file_area_temp链表上的file_area遍历了一遍，则进入冷却期。在N个周期内，不在执行该函数遍历该文件
 *file_stat->temp、refault、free、mapcount、hot 链表上file_area。file_stat->file_area_temp链表上的file_area遍历了一遍，导致文件进入
 *冷冻期，此时页牵连无法遍历到该文件file_stat->refault、free、mapcount、hot 链表上file_area，这合理吗。当然可以分开遍历，但是目前
 *觉得没必要，因为file_stat->refault、free、mapcount、hot 链表上file_area也有冷冻期，这个冷冻期还更长，是N的好几倍。因此不会影响，还降低性能损耗
 *5:遍历文件file_stat的原生radix tree，是否存在空洞file_area，是的话则为遍历到的新的文件页创建file_area
 */
static noinline unsigned int check_file_area_cold_page_and_clear(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int scan_file_area_max,unsigned int *already_scan_file_area_count,unsigned int file_stat_list_type,unsigned int file_type)
{
	struct page *page_buf[BUF_PAGE_COUNT];
	int cold_page_count = 0,cold_page_count_last;
	int ret = 0,file_stat_scan_status = 0;
	struct file_area *p_file_area,*p_file_area_temp;
	char delete_file_area_last = 0;
	unsigned int reclaimed_pages = 0;
	LIST_HEAD(file_area_temp_head);
	/*file_area里的page至少一个page发现是cache page的，则该file_area移动到file_area_have_cache_page_head，后续回收cache的文件页*/
	LIST_HEAD(file_area_have_cache_page_head);
	LIST_HEAD(file_area_free);
	unsigned int isolate_pages = 0;
	unsigned int scan_cold_file_area_count = 0;
	unsigned int temp_to_warm_file_area_count = 0;
	unsigned int temp_to_temp_head_file_area_count = 0;
	unsigned int scan_file_area_count_file_move_from_cache = 0;
	struct file_stat *p_file_stat = NULL;
	struct file_stat_small *p_file_stat_small = NULL;
	unsigned int file_area_type;

	memset(page_buf,0,BUF_PAGE_COUNT*sizeof(struct page*));

	/*注意，执行该函数的file_stat都是处于global temp链表的，file_stat->file_area_temp和 file_stat->file_area_mapcount 链表上都有file_area,mapcount的file_area
	 *变多了，达到了该文件要变成mapcount文件的阀值。目前在下边的check_one_file_area_cold_page_and_clear函数里，只会遍历file_stat->file_area_mapcount 链表上
	 *的file_area，如果不是mapcount了，那就降级到file_stat->file_area_temp链表。没有遍历file_stat->file_area_temp链表上的file_area，如果对应page的mapcount大于1
	 *了，再把file_area升级到file_stat->file_area_mapcount链表。如果mapcount的file_area个数超过阀值，那就升级file_stat到mapcount文件。这个有必要做吗???????，
	 *想想还是做吧，否则这种file_area的page回收时很容易失败*/
	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx file_area_last:0x%llx file_area_count_in_temp_list:%d file_area_hot_count:%d mapcount_file_area_count:%d file_area_count:%d\n",__func__,(u64)p_file_stat_base,(u64)p_file_stat_base->file_area_last,p_file_stat_base->file_area_count_in_temp_list,p_file_stat_base->file_area_hot_count,p_file_stat_base->mapcount_file_area_count,p_file_stat_base->file_area_count);

	if(p_file_stat_base->file_area_last){//本次从上一轮扫描打断的file_stat继续遍历
		p_file_area = p_file_stat_base->file_area_last;
	}
	else{
		//第一次从链表尾的file_stat开始遍历。或者新的冷却期开始后也是
		p_file_area = list_last_entry(&p_file_stat_base->file_area_temp,struct file_area,file_area_list);
		p_file_stat_base->file_area_last = p_file_area;
	}

	while(!list_empty(&p_file_stat_base->file_area_temp)){

		/* 这里遇到一个麻烦事，如果该file_stat原本是so文件，被read系统调用读文件头的elf信息而被判定为cache文件，然后文件头的
		 * file_stat->temp链表上file_area因读的次数多而被判定为hot file_area，设置了file_area in_hot_list标记。然后这个文件
		 * 才mmap建立映射，此时才是个mmap文件。而这些file_stat->temp链表上的热file_area就会一直存在。然后这个cache文件
		 * 被异步内存回收线程第一次被遍历时，发现是mmap file而把file_stat移动到global mmap_file_stat_temp_head链表。
		 * 然后这里遍历这个file_stat->temp链表上的file_area，就发现是hot file_area，但这个file_area没有in_temp_list标记，
		 * 于是这里就会crash。于是增加如下判断代码，清理掉hot标记，增加temp链表。后续这个mmap文件的cache file_area就按照
		 * mmap文件的逻辑处理了，不会再按照cache文件hot、refault文件的逻辑处理了。问题来了，这个file_area有没有在
		 * file_stat移动到global mmap_file_stat_temp_head链表前，除了被判定为hot file_area，还被判定我refault、free 
		 * file_area呢？不会。hot_file_update_file_status()对于in_temp_list的file_area只会判定为hot file_area。并且
		 * 所有cache file_area移动到global mmap_file_stat_temp_head链表前，限制所有的file_area必须都处于in_temp_list
		 * 状态，其他file_stat->free、refault、hot、warm等链表必须处于空*/
		if(file_area_in_hot_list(p_file_area) && file_stat_in_from_cache_file_base(p_file_stat_base)){
			clear_file_area_in_hot_list(p_file_area); 
			set_file_area_in_temp_list(p_file_area); 
			scan_file_area_count_file_move_from_cache ++;
		}

		/*tiny small文件的所有类型的file_area都在file_stat->temp链表，因此要单独处理hot、refault、free属性的file_area*/
		if(FILE_STAT_TINY_SMALL == file_type && !file_area_in_temp_list(p_file_area)){
			/*tiny small所有种类的file_area都在file_stat->temp链表，遇到hot和refault的file_area，只需要检测它是否长时间没访问，
			 *然后降级到temp的file_area。遇到free的file_area如果长时间没访问则要释放掉file_area结构。get_file_area_list_status()
			 是获取该file_area的状态，是free还是hot还是refault状态*/

			file_area_type = get_file_area_list_status(p_file_area);
			reverse_other_file_area_list_common(p_hot_cold_file_global,p_file_stat_base,p_file_area,file_area_type,FILE_STAT_TINY_SMALL,NULL);
			/*直接处理file_stat->temp链表上的下一个file_ara，tiny small文件的所有file_area都在file_stat->temp链表，不用担心p_file_area被移动到其他file_stat->hot等链表*/
			goto next_file_area;
		}

		if(!file_area_in_temp_list(p_file_area) || file_area_in_temp_list_error(p_file_area))
			panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x not in file_area_temp\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state);

		/*查找file_area在file_stat->file_area_temp链表上一个file_area。如果p_file_area不是链表头的file_area，直接list_prev_entry
		 * 找到上一个file_area。如果p_file_stat是链表头的file_area，那要跳过链表过，取出链表尾的file_area*/
		if(!list_is_first(&p_file_area->file_area_list,&p_file_stat_base->file_area_temp)){
			p_file_area_temp = list_prev_entry(p_file_area,file_area_list);
		}
		else{
			//从链表尾遍历完一轮file_area了，文件file_stat要进入冷却期
			p_file_stat_base->cooling_off_start = 1;
			//记录此时的全局age
			p_file_stat_base->cooling_off_start_age = p_hot_cold_file_global->global_age;
			if(shrink_page_printk_open)
				printk("1_1:%s file_stat:0x%llx cooling_off_start age:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->cooling_off_start_age);

			p_file_area_temp = list_last_entry(&p_file_stat_base->file_area_temp,struct file_area,file_area_list);
		}

		/*遍历file_stat->file_area_temp，查找冷的file_area*/
		cold_page_count_last = cold_page_count;
		if(shrink_page_printk_open)
			printk("2:%s file_stat:0x%llx file_area:0x%llx index:%ld scan_file_area_count_temp_list:%d file_area_count_in_temp_list:%d\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->start_index,p_file_stat_base->scan_file_area_count_temp_list,p_file_stat_base->file_area_count_in_temp_list);

		//这个错误赋值会影响到file_stat->access_count，导致误判为热file_area。这点很重要，这点不要删除
		//p_file_area->file_area_access_age = p_hot_cold_file_global->global_age;
		
		ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat_base,p_file_area,page_buf,&cold_page_count,file_stat_list_type,&file_area_have_cache_page_head,file_type);
		/*到这里有两种可能
		 *1: file_area的page都是冷的，ret是0
		 *2: file_area的page有些被访问了，ret大于0
		 *3：file_area的page都是冷的，但是有些page前边trylock_page失败了，ret大于0,这种情况后期得再优化优化细节!!!!!!!!!!!!!

		 还有，file_area可能被判定为热file_area或mapcount file_area，已经从p_file_stat->file_area_temp链表移动走了。
		 而恰好该file_area是p_file_stat->file_area_last要强制更新p_file_stat->file_area_last为p_file_area_temp。
		 因为此时这个p_file_stat->file_area_last已经不再处于temp链表了，会导致这里遍历链表陷入死循环。原因下边有详说
		 */

		/*file_area变成了mapcount或hot file_area 或者file_area里有cache文件页而设置file_area_in_free状态并
		 *移动到file_area_have_cache_page_head链表而参与cache文件页的内存回收流程*/
		//if(!file_area_in_temp_list(p_file_area)){
		if(file_area_in_hot_list(p_file_area) || file_area_in_mapcount_list(p_file_area) ||file_area_in_free_list(p_file_area)){
			/*加下边这个if判断，是因为之前设计的以p_file_stat->file_area_last为基准的循环遍历链表有个重大bug：while循环第一次
			 *遍历p_file_area时，p_file_area和p_file_stat->file_area_last是同一个。而如果p_file_area是冷的，并且本次它的page的
			 *pte页表也没访问，那就要把file_area移动到file_stat->file_area_free链表。这样后续这个while就要陷入死循环了，
			 *因为p_file_stat->file_area_last指向的file_area移动到file_stat->file_area_free链表了。而p_file_area一个个
			 *指向file_stat->file_area_temp链表的file_area。下边的while(p_file_area != p_file_stat->file_area_last)永远不成立
			 *并且上边check_one_file_area_cold_page_and_clear()里传入的file_area是重复的，为什么，因为遍历file_stat->temp链表
			 陷入了死循环，会多次遍历同一个file_area，同一个page。故会导致里边重复判断page和lock_page，
			 然后就会出现进程hung在lock_page里，因为重复lock_page。这就是遍历链表时因成员跳到了其他链表而永无法达到循环退出条件的bug
			 解决办法是

			 1：凡是p_file_area和p_file_stat->file_area_last是同一个，一定要更新p_file_stat->file_area_last为p_file_area在
			 file_stat->file_area_temp链表的前一个file_area，即p_file_stat->file_area_last = p_file_area_temp。
			 2：下边else分支的file_area不太冷但它的page是冷的情况，要把file_area从file_stat->file_area_temp链表移除，并移动到
			 file_area_temp_head临时链表。while循环结束时再把这些file_area移动到file_stat->file_area_temp链表尾。这样避免这个
			 while循环里，重复遍历这种file_area，重复lock_page 对应的page，又要hung
			 3：while循环的退出条件加个 !list_empty(file_stat->file_area_temp)。这样做的目的是，如果file_stat->file_area_temp链表
			 只有一个file_area，而它和它的page都是冷的，它移动到ile_stat->file_area_free链表后，p_file_stat->file_area_last
			 指向啥呢？这个链表没有成员了！只能靠!list_empty(file_stat->file_area_temp)退出while循环

			 注意，还有一个隐藏bug，当下边的if成立时，这个while循环就立即退出了，不会接着遍历了。因为if成立时，
			 p_file_stat->file_area_last = p_file_area_temp，二者相等，然后下边执行p_file_area = p_file_area_temp 后，就导致
			 p_file_stat->file_area_last 和 p_file_area 也相等了，while(p_file_area != p_file_stat->file_area_last)就不成立了。
			 解决办法时，当发现该if成立，令 delete_file_area_last置1，然后下边跳出while循环的条件改为
			 if(0 == delete_file_area_last && p_file_area != p_file_stat->file_area_last) break。就是说，当发现
			 本次要移动到其他链表的p_file_area是p_file_stat->file_area_last时，令p_file_stat->file_area_last指向p_file_area在
			 file_stat->file_area_temp链表的上一个file_area(即p_file_area_temp)后，p_file_area也会指向这个file_area，此时不能
			 跳出while循环，p_file_area此时的新file_area还没使用过呢！

			 时隔半年后，再次看这个内核bug的注释，产生了很大的疑问，为什么只有p_file_area不在in_temp_list链表时时才必须要要把在
			 file_stat->temp链表的上一个file_area即p_file_area_temp更新到p_file_stat->file_area_last呢？因为此时
			 下，p_file_area已经移动到file_stat->hot、mapcount、free链表了，p_file_stat->file_area_last指向的将是
			 file_stat->hot、mapcount、free链表的file_area，不再是file_stat->temp链表的file_area。于是下边
			 if(0 == delete_file_area_last && p_file_area == p_file_stat->file_area_last)break跳出循环的条件永远不成立。
			 因为后续的p_file_area都是file_stat->temp链表的file_area，而p_file_stat->file_area_last指向的不再是。当前，
			 这种情况也是file_stat->temp链表上的file_area不能不热，不会在下边的else if(0 == ret)分支移动到
			 file_stat->free、warm链表时才会成立。如果下边的else if(0 == ret)file_stat->temp链表上成立，file_stat->temp
			 链表的file_area全移动到了file_stat->temp、warm链表，则file_stat->temp将是一个空链表，这样也会退出循环。

			 并且，下边的if成立，有两种可能。比如该循环遍历的第一个file_area时热file_area而移动到了file_stat->hot链表，
			 正好该file_area也是p_file_stat->file_area_last。还有就是遍历了一遍所有的file_stat->temp链表上的file_area后，
			 再次p_file_area和最新的p_file_stat->file_area_last又相等，下边的if岂不是又成立。怎么区分这两种情况？想多了，
			 第2种情况根本不会成立。因为，这种情况下的前一步，p_file_area_temp和p_file_stat->file_area_last是同一个，
			 并且p_file_area_temp是p_file_area在file_stat->temp链表的前一个file_area。然后下边p_file_area = p_file_area_temp赋值后，
			 if(0 == delete_file_area_last && p_file_area == p_file_stat->file_area_last) break成立，直接break跳出循环了

			 **/
			if(/*!file_area_in_temp_list(p_file_area) &&*/ (p_file_area == p_file_stat_base->file_area_last)){
				p_file_stat_base->file_area_last = p_file_area_temp;
				delete_file_area_last = 1;
			}
		}
		/*file_area的page都没有被访问，如果很冷的则参与内存，如果不太冷的则移动到临时链表*/
		else if(0 == ret){

			/*file_area必须处于file_stat->temp链表，否则会出bug，如果file_area没有被访问，但是file_area被判定为
			 *mapcount file_area而移动到了file_stat->mapcount链表，则此时就不能再把file_area再移动到其他链表，否则file_area
			 就会处于两种链表，状态就大错特错了*/
			if(!file_area_in_temp_list(p_file_area) || file_area_in_temp_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			/*二者不相等，说明file_area是冷的，并且它的page的pte本次检测也没被访问，这种情况才回收这个file_area的page。
			 *并且，如果当前p_file_area被被判定是热的或者mapcount file_area，*/

			/* 但有个bug，此时file_area可能2个page是冷的，两个page是mapcount的，此时file_area处于file_stat->mapcount链表。
			 * 还有，file_area可能2个page是冷的，两个page是热的，此时file_area处于file_stat->hot链表，总之不在file_stat->temp链表。
			 * 这个bug我当时写代码没发现，当局者迷。而我把异步内存回收做到内核时，原理忘了，报着小白怀疑态度再查看这段代码，
			 * 很容易就发现了这个bug。真的无语了，只能这样才能发现隐藏bug吗？这都是check_one_file_area_cold_page_and_clear()
			 * 函数没设计好造成的，下个版本解决*/
			if(cold_page_count_last != cold_page_count)
			{
				/*二者不相等，说明file_area是冷的，并且它的page的pte本次检测也没被访问，这种情况才回收这个file_area的page。
				 *如果file_area被判定是热的或者mapcount的，则if(cold_page_count_last != cold_page_count)不成立*/
				//if(!file_area_in_temp_list(p_file_area) || file_area_in_temp_list_error(p_file_area))
				//	panic("%s file_area:0x%llx status:%d not in file_area_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

				//处于file_stat->tmep链表上的file_area，移动到其他链表时，要先对file_area的access_count清0，否则会影响到
				//file_area->file_area_access_age变量，因为file_area->access_count和file_area_access_age是共享枚举变量
				file_area_access_count_clear(p_file_area);

				spin_lock(&p_file_stat_base->file_stat_lock);
				//file_stat->temp 链表上的file_area个数减1
				p_file_stat_base->file_area_count_in_temp_list --;

				clear_file_area_in_temp_list(p_file_area);
				/*设置file_area处于file_stat的free_temp_list链表。这里设定，不管file_area处于file_stat->file_area_free还是
				 *file_stat->file_area_free链表，都是file_area_in_free_list状态，没有必要再区分二者*/
				set_file_area_in_free_list(p_file_area);
				/*冷file_area移动到file_area_free链表参与内存回收。移动到 file_area_free链表的file_area也要每个周期都扫描。
				 *1：如果对应文件页长时间不被访问，那就释放掉file_area
				 *2：如果对应page内存回收又被访问了，file_area就要移动到refault链表，长时间不参与内存回收
				 *3：如果refault链表上的file_area的page长时间不被访问，file_area就要降级到temp链表
				 *4：如果文件file_stat的file_area全被释放了，file_stat要移动到 zero_file_area链表，然后释放掉file_stat结构
				 *5：在驱动卸载时，要释放掉所有的file_stat和file_area*/
				if(FILE_STAT_NORMAL == file_type){
			        p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
				    list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free);
					//用完之后设置NULL，防止后边非法使用当前这个p_file_stat值
					p_file_stat = NULL;
				}
				else if(FILE_STAT_SMALL == file_type){
			        p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
				    list_move(&p_file_area->file_area_list,&p_file_stat_small->file_area_other);
					p_file_stat_small = NULL;
				}else
					BUG();

				spin_unlock(&p_file_stat_base->file_stat_lock);

				scan_cold_file_area_count ++;
#if 0
				//记录file_area参与内存回收的时间
				p_file_area->shrink_time = ktime_to_ms(ktime_get()) >> 10;
#endif
			}else{
				/*如果file_area的page没被访问，但是file_area还不是冷的，file_area不太冷，则把file_area先移动到临时链表，然后该函数最后再把
				 *该临时链表上的不太冷file_area同统一移动到file_stat->file_area_warm链表尾。这样也可以避免该while循环里重复遍历到
				 *这些file_area。注意，到这个分支不是所有的file_area都要移动到warm链表，只有age差大于temp_to_warm才可以!!!!!!!!!!!*/
				if(FILE_STAT_NORMAL == file_type && p_hot_cold_file_global->global_age - p_file_area->file_area_age > p_hot_cold_file_global->mmap_file_area_temp_to_warm_age_dx){
					p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
					spin_lock(&p_file_stat->file_stat_lock);/*凡是对file_stat->file_area_temp链表成员的list_add、list_del、list_move操作都要加锁*/
					clear_file_area_in_temp_list(p_file_area);
					set_file_area_in_warm_list(p_file_area);
					//list_move(&p_file_area->file_area_list,&file_area_temp_head);
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_warm);
					spin_unlock(&p_file_stat->file_stat_lock);
					temp_to_warm_file_area_count ++;

					//用完之后设置NULL，防止后边非法使用当前这个p_file_stat值
					p_file_stat = NULL;
				}
			}
		}else if(ret > 0){
			if(!file_area_in_temp_list(p_file_area) || file_area_in_temp_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			/*如果file_area的page被访问了，则把file_area移动到链表头*/
			spin_lock(&p_file_stat_base->file_stat_lock);
			list_move(&p_file_area->file_area_list,&p_file_stat_base->file_area_temp);
			spin_unlock(&p_file_stat_base->file_stat_lock);
			temp_to_temp_head_file_area_count ++;
		}

next_file_area:

		/*1:凑够BUF_PAGE_COUNT个要回收的page，if成立，则开始隔离page、回收page
		 *2:page_buf剩余的空间不足容纳PAGE_COUNT_IN_AREA个page，if也成立，否则下个循环执行check_one_file_area_cold_page_and_clear函数
		 *向page_buf保存PAGE_COUNT_IN_AREA个page，将导致内存溢出*/
		if(cold_page_count >= BUF_PAGE_COUNT || (BUF_PAGE_COUNT - cold_page_count <=  PAGE_COUNT_IN_AREA)){

			isolate_pages += cold_mmap_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,p_file_area,page_buf,cold_page_count);
			reclaimed_pages = p_hot_cold_file_global->mmap_file_shrink_counter.mmap_free_pages_count;
			cold_page_count = 0;
			if(shrink_page_printk_open)
				printk("3:%s file_stat:0x%llx reclaimed_pages:%d isolate_pages:%d\n",__func__,(u64)p_file_stat_base,reclaimed_pages,isolate_pages);
		}

		/*下一个扫描的file_area。这个对p_file_area赋值p_file_area_temp，要放到if(*already_scan_file_area_count > scan_file_area_max) break;
		 *跳出while循环前边。否则会存在这样一种问题，前边p_file_area不是太冷而移动到了file_area_temp_head链表头，然后下边break跳出，
		 *p_file_area此时指向的是file_area已经移动到file_area_temp_head链表头链表头了，且这个链表只有这一个file_area。然后下边执行
		 *p_file_stat->file_area_last = list_prev_entry(p_file_area,file_area_list) 对p_file_stat->file_area_last赋值时，
		 *p_file_stat->file_area_last指向file_area就是file_area_temp_head链表头了。下次执行这个函数时，使用p_file_stat->file_area_last
		 *指向的file_area时非法的了*/
		p_file_area = p_file_area_temp;

		//异步内存回收线程本次运行扫描的总file_area个数加1，
		*already_scan_file_area_count = *already_scan_file_area_count + 1;
		//文件file_stat已经扫描的file_area个数加1
		p_file_stat_base->scan_file_area_count_temp_list ++;

		//超过本轮扫描的最大file_area个数则结束本次的遍历
		if(*already_scan_file_area_count >= scan_file_area_max){
			file_stat_scan_status = FILE_STAT_SCAN_MAX_FILE_AREA;
			break;
		}
		/*文件file_stat已经扫描的file_area个数超过file_stat->file_area_temp 链表的总file_area个数，停止扫描该文件的file_area。
		 *然后才会扫描global->mmap_file_stat_temp_head或mmap_file_stat_large_file_head链表上的下一个文件file_stat的file_area
		 *文件file_stat进入冷却期if也成。其实这两个功能重复了，本质都表示遍历完file_stat->temp链表上的file_area*/
		if(/*p_file_stat->scan_file_area_count_temp_list >= p_file_stat->file_area_count_in_temp_list ||*/ p_file_stat_base->cooling_off_start){
			//文件扫描的file_area个数清0，下次轮到扫描该文件的file_area时，才能继续扫描
			p_file_stat_base->scan_file_area_count_temp_list = 0;
			file_stat_scan_status = FILE_STAT_SCAN_IN_COOLING;
			break;
		}

		/*file_stat->temp链表的file_area本次全遍历了一遍，于是结束遍历*/
		if(0 == delete_file_area_last && p_file_area == p_file_stat_base->file_area_last){
			file_stat_scan_status = FILE_STAT_SCAN_ALL_FILE_AREA;
			break;
		}
		else if(1 == delete_file_area_last)
			delete_file_area_last = 0;
	}
	//while(p_file_area != p_file_stat->file_area_last) 这行代码不要删除，因为涉及到遍历链条的一个死循环的重大bug

	/*如果到这里file_stat->file_area_temp链表时空的，说明上边的file_area都被遍历过了，那就令p_file_stat->file_area_last = NULL。
	 *否则令p_file_stat->file_area_last指向本次最后在file_stat->file_area_temp链表上遍历的file_area的上一个file_area*/
	if(!list_empty(&p_file_stat_base->file_area_temp)){
		/*下个周期直接从p_file_stat->file_area_last指向的file_area开始扫描*/
		if(!list_is_first(&p_file_area->file_area_list,&p_file_stat_base->file_area_temp))
			p_file_stat_base->file_area_last = list_prev_entry(p_file_area,file_area_list);
		else
			p_file_stat_base->file_area_last = list_last_entry(&p_file_stat_base->file_area_temp,struct file_area,file_area_list);
	}else{
		p_file_stat_base->file_area_last = NULL;
		//当前文件file_stat->file_area_temp上的file_area扫描完了，需要扫描下一个文件了
		file_stat_scan_status = FILE_STAT_SCAN_ALL_FILE_AREA;
	}

	if(!list_empty(&file_area_temp_head)){
		spin_lock(&p_file_stat_base->file_stat_lock);
		//把本次扫描的暂存在file_area_temp_head临时链表上的不太冷的file_area移动到file_stat->file_area_temp链表尾
		list_splice_tail(&file_area_temp_head,&p_file_stat_base->file_area_temp);
		spin_unlock(&p_file_stat_base->file_stat_lock);
	}

	if(shrink_page_printk_open)
		printk("4:%s file_stat:0x%llx cold_page_count:%d\n",__func__,(u64)p_file_stat_base,cold_page_count);

	//如果本次对文件遍历结束后，有未达到BUF_PAGE_COUNT数目要回收的page，这里就隔离+回收这些page
	if(cold_page_count){
		isolate_pages += cold_mmap_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat_base,p_file_area,page_buf,cold_page_count);
		reclaimed_pages = p_hot_cold_file_global->mmap_file_shrink_counter.mmap_free_pages_count;
		if(shrink_page_printk_open)
			printk("5:%s file_stat:0x%llx reclaimed_pages:%d isolate_pages:%d\n",__func__,(u64)p_file_stat_base,reclaimed_pages,isolate_pages);
	}

	if(FILE_STAT_NORMAL == file_type){
		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
		/*遍历file_stat->warm链表上的file_area，冷的file_area就回收*/
		mmap_file_stat_warm_list_file_area_solve(p_hot_cold_file_global,p_file_stat,scan_file_area_max,already_scan_file_area_count,file_stat_list_type,&file_area_have_cache_page_head,file_type);
		p_file_stat = NULL;
	}

	/*file_area里的page至少一个page发现是cache page的，则该file_area上边已经移动到了file_area_have_cache_page_head，这里回收这些file_area里的cache文件页*/
	if(!list_empty(&file_area_have_cache_page_head)){
		/* 遍历file_area_have_cache_page_head链表上的file_area，符合内存回收条件的file_area移动到file_area_free
		 * 链表，然后内存回收。不符合内存回收条件的还停留在file_area_have_cache_page_head链表*/
		mmap_file_area_cache_page_solve(p_hot_cold_file_global,p_file_stat_base,&file_area_have_cache_page_head,&file_area_free,file_type);
		if(FILE_STAT_NORMAL == file_type){
			p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
			/*内存回收后的file_area全都移动到ile_stat->file_area_free链表，将来通过file_stat->file_area_free
			 *链表遍历到这些file_area还残留page，说明内存回收失败，则会判定file_area为refault file_area。当然也可以
			 *这里就可以通过file_area_have_cache_page_head遍历这些file_area，检测是否还残留page，有这个必要吗*/
			list_splice(&file_area_free,&p_file_stat->file_area_free);

			/*把未参与内存回收的file_area设置in_warm状态后，再移动到file_stat->file_area_warm链表，后续再参与内存回收*/
			list_for_each_entry_safe(p_file_area,p_file_area_temp,&file_area_have_cache_page_head,file_area_list){
				clear_file_area_in_free_list(p_file_area);
				set_file_area_in_warm_list(p_file_area);
			}
			list_splice(&file_area_have_cache_page_head,&p_file_stat->file_area_warm);

			p_file_stat = NULL;
		}else if(FILE_STAT_SMALL == file_type){
			p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
			list_splice(&file_area_free,&p_file_stat_small->file_area_other);
			list_splice(&file_area_have_cache_page_head,&p_file_stat_small->file_area_other);
			p_file_stat_small = NULL;
		}else
			BUG();
	}


	/*遍历file_stat->file_area_free链表上已经释放page的file_area，如果长时间还没被访问，那就释放掉file_area。
	 *否则访问的话，要把file_area移动到file_stat->file_area_refault或file_area_temp链表。是否一次遍历完file_area_free
	 *链表上所有的file_area呢？那估计会很损耗性能，因为要检测这些file_area的page映射页表的pte，这样太浪费性能了！
	 *也得弄个file_area_last，每次只遍历file_area_free链表上几十个file_area，file_area_last记录最后一次的
	 *file_area的上一个file_area，下次循环直接从file_area_last指向file_area开始遍历。这样就不会重复遍历file_area，
	 *也不会太浪费性能*/

	if(FILE_STAT_NORMAL == file_type){
		p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
		/*遍历file_stat->file_area_free链表上file_area，如果长时间不被访问则释放掉，如果被访问了则升级到file_stat->file_area_refault或temp链表*/
		if(!list_empty(&p_file_stat->file_area_free)){
			reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_free,32,1 << F_file_area_in_free_list,MMAP_FILE_AREA_FREE_AGE_DX);
		}
		/*遍历file_stat->file_area_refault链表上file_area，如果长时间不被访问，要降级到file_stat->file_area_temp链表*/
		if(!list_empty(&p_file_stat->file_area_refault)){
			reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_refault,16,1 << F_file_area_in_refault_list,MMAP_FILE_AREA_REFAULT_AGE_DX);
		}
		//遍历file_stat->file_area_mapcount上的file_area，如果file_area的page的mapcount都是1，file_area不再是mapcount file_area，则降级到temp_list
		if(!list_empty(&p_file_stat->file_area_mapcount)){
			reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_mapcount,8,1 << F_file_area_in_mapcount_list,MMAP_FILE_AREA_MAPCOUNT_AGE_DX);
		}
		//遍历file_stat->file_area_hot上的file_area，如果长时间不被访问了，则降级到temp_list
		if(!list_empty(&p_file_stat->file_area_hot)){
			reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_hot,8,1 << F_file_area_in_hot_list,MMAP_FILE_AREA_HOT_AGE_DX);
		}
	}else if(FILE_STAT_SMALL == file_type){
		/*small文件other链表上的file_area的处理*/
		p_file_stat_small = container_of(p_file_stat_base,struct file_stat_small,file_stat_base);
		reverse_other_file_area_list_small_file(p_hot_cold_file_global,p_file_stat_small,&p_file_stat_small->file_area_other,32,-1,MMAP_FILE_AREA_HOT_AGE_DX);
	}

	if(shrink_page_printk_open1)
		printk("%s file_stat:0x%llx already_scan_file_area_count:%d reclaimed_pages:%d isolate_pages:%d\n",__func__,/*p_file_stat->file_name,*/(u64)p_file_stat_base,*already_scan_file_area_count,reclaimed_pages,isolate_pages);

	p_hot_cold_file_global->mmap_file_shrink_counter.isolate_lru_pages_from_temp += isolate_pages;
	p_hot_cold_file_global->mmap_file_shrink_counter.scan_cold_file_area_count_from_temp += scan_cold_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.temp_to_warm_file_area_count += temp_to_warm_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.temp_to_temp_head_file_area_count += temp_to_temp_head_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.scan_file_area_count_file_move_from_cache += scan_file_area_count_file_move_from_cache;

	return file_stat_scan_status;
}
#if 0 //这个函数的作用已经分拆了，第一次扫描文件page并创建file_area的代码已经移动到scan_uninit_file_stat()函数了
/*
 * 1：如果还没有遍历过file_stat对应的文件的radix tree，先遍历一遍radix tree，得到page，分配file_area并添加到file_stat->file_area_temp链表头，
 *    还把file_area保存在radix tree
 * 2：如果已经遍历完一次文件的radix tree，则开始遍历file_stat->file_area_temp链表上的file_area的page，如果page被访问了则把file_area移动到
 * file_stat->file_area_temp链表头。如果file_area的page长时间不被访问，把file_area移动到file_stat->file_area_free链表，则回收这些page
 * 
 * 2.1：遍历file_stat->file_area_free链表上的file_area，如果对应page被访问了则file_area移动到file_stat->file_area_refault链表;
 *      如果对应page长时间不被访问则释放掉file_area
 * 2.2：如果file_stat->file_area_refault链表上file_area的page如果长时间不被访问，则移动回file_stat->file_area_temp链表
 *
 * 2.3：文件可能有page没有被file_area控制，存在空洞。就是说有些文件页page最近被访问了，才分配并加入radix tree，这些page还没有分配
 *      对应的file_area。于是遍历文件file_stat保存file_area的radix tree，找到没有file_area的槽位，计算出本应该保存在这个槽位的file_area
 *      对应的page的索引，再去保存page的radix tree的查找这个page是否分配了，是的话就分配file_area并添加到file_stat->file_area_temp链表头
 * */
static unsigned int traverse_mmap_file_stat_get_cold_page(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,unsigned int scan_file_area_max,unsigned int *already_scan_file_area_count)
{
	int i,k;
	struct hot_cold_file_area_tree_node *parent_node;
	void **page_slot_in_tree;
	unsigned int area_index_for_page;
	int ret = 0;
	struct page *page;
	unsigned int file_page_count = p_file_stat->mapping->host->i_size >> PAGE_SHIFT;//除以4096
	struct address_space *mapping = p_file_stat->mapping;

	printk("1:%s file_stat:0x%llx file_stat->last_index:%d file_area_count:%d traverse_done:%d\n",__func__,(u64)p_file_stat,p_file_stat->last_index,p_file_stat->file_area_count,p_file_stat->traverse_done);
	if(p_file_stat->max_file_area_age || p_file_stat->recent_access_age || p_file_stat->hot_file_area_cache[0] || p_file_stat->hot_file_area_cache[1] ||p_file_stat->hot_file_area_cache[2]){
		panic("file_stat error\n");
	}

	/*p_file_stat->traverse_done非0，说明还没遍历完一次文件radix tree上所有的page，那就遍历一次，每4个page分配一个file_area*/
	if(0 == p_file_stat->traverse_done){
		/*第一次扫描文件的page，每个周期扫描SCAN_PAGE_COUNT_ONCE个page，一直到扫描完所有的page。4个page一组，每组分配一个file_area结构*/
		for(i = 0;i < SCAN_PAGE_COUNT_ONCE >> PAGE_COUNT_IN_AREA_SHIFT;i++){
			for(k = 0;k < PAGE_COUNT_IN_AREA;k++){
				/*这里需要优化，遍历一次radix tree就得到4个page，完全可以实现的，节省性能$$$$$$$$$$$$$$$$$$$$$$$$*/
				page = xa_load(&mapping->i_pages, p_file_stat->last_index + k);
				if (page && !xa_is_value(page) && page_mapped(page)) {
					area_index_for_page = page->index >> PAGE_COUNT_IN_AREA_SHIFT;
					parent_node = hot_cold_file_area_tree_lookup_and_create(&p_file_stat->hot_cold_file_area_tree_root_node,area_index_for_page,&page_slot_in_tree);
					if(IS_ERR(parent_node)){
						ret = -1;
						printk("%s hot_cold_file_area_tree_lookup_and_create fail\n",__func__);
						goto out;
					}
					if(NULL == *page_slot_in_tree){
						//分配file_area并初始化，成功返回0
						if(file_area_alloc_and_init(parent_node,page_slot_in_tree,area_index_for_page,p_file_stat) < 0){
							ret = -1;
							goto out;
						}
					}
					else{
						printk("%s file_stat:0x%llx file_area index:%d_%ld already alloc\n",__func__,(u64)p_file_stat,area_index_for_page,page->index);
					}
					/*4个连续的page只要有一个在radix tree找到，分配file_area,之后就不再查找其他page了*/
					break;
				}
			}
			p_file_stat->last_index += PAGE_COUNT_IN_AREA;
		}
		//p_file_stat->last_index += SCAN_PAGE_COUNT_ONCE;
		//if成立说明整个文件的page都扫描完了
		if(p_file_stat->last_index >= file_page_count){
			p_file_stat->traverse_done = 1;
			//file_stat->last_index清0
			p_file_stat->last_index = 0;
		}

		ret = 1;
	}else{
		/*到这个分支，文件的所有文件页都遍历了一遍。那就开始回收这个文件的文件页page了。但是可能存在空洞，上边的遍历就会不完整，有些page
		 * 还没有分配，那这里除了内存回收外，还得遍历文件文件的radix tree，找到之前没有映射的page，但是这样太浪费性能了。于是遍历保存file_area
		 * 的radix tree，找到空洞file_area，这些file_area对应的page还没有被管控起来。$$$$$$$$$$$$$$$$$$$$$$$$$$*/
		p_file_stat->traverse_done ++;

		if(!list_empty(&p_file_stat->file_area_temp))
			check_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat,scan_file_area_max,already_scan_file_area_count);
	}
out:
	return ret;
}
#endif

static noinline int traverse_mmap_file_stat_get_cold_page(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int scan_file_area_max,unsigned int *already_scan_file_area_count,unsigned int file_stat_list_type,unsigned int file_type)
{
	int ret;
	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx file_area_count:%d \n",__func__,(u64)p_file_stat_base,/*p_file_stat->last_index,*/p_file_stat_base->file_area_count/*,p_file_stat->traverse_done*/);

	if(p_file_stat_base->max_file_area_age/* || p_file_stat->hot_file_area_cache[0] || p_file_stat->hot_file_area_cache[1] ||p_file_stat->hot_file_area_cache[2]*/){
		panic("file_stat error p_file_stat:0x%llx\n",(u64)p_file_stat_base);
	}

	/*到这个分支，文件的所有文件页都遍历了一遍。那就开始回收这个文件的文件页page了。但是可能存在空洞，上边的遍历就会不完整，有些page
	 * 还没有分配，那这里除了内存回收外，还得遍历文件文件的radix tree，找到之前没有映射的page，但是这样太浪费性能了。于是遍历保存file_area
	 * 的radix tree，找到空洞file_area，这些file_area对应的page还没有被管控起来*/

	//令inode引用计数加1，防止遍历该文件的radix tree时文件inode被释放了
	if(file_inode_lock(p_file_stat_base) == 0)
	{
		printk("%s file_stat:0x%llx status 0x%lx inode lock fail\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		return -1;
	}
	ret = check_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat_base,scan_file_area_max,already_scan_file_area_count,file_stat_list_type,file_type);
	//令inode引用计数减1
	file_inode_unlock(p_file_stat_base);

	//返回值是1是说明当前这个file_stat的file_area已经全扫描完了，则扫描该file_stat在global->mmap_file_stat_large_file_head或global->mmap_file_stat_temp_head链表上的上一个file_stat的file_area
	return ret;
}

/*
目前遍历各种global 链表上的file_stat，或者file_stat链表上的file_area，有两种形式。
1:比如 check_file_area_cold_page_and_clear()函数，遍历file_stat->temp 链表上的file_area，从链表尾到头遍历，每次定义一个file_stat->file_area_last指针。它指向一个文件file_stat->temp链表上每轮遍历的最后一个file_area。下轮再次遍历这个文件file_stat->temp链表上的file_area时，直接从file_stat->file_area_last指针指向的file_area开始遍历就行。这种形式的好处是，当遍历到热file_area，留在链表头，遍历到冷file_area留在链表尾巴，冷file_area都聚集在file_stat->temp 链表尾。而当遍历完一轮file_stat->temp 链表上的file_area时，file_stat冷却N个周期后才能再次遍历file_stat->temp 链表上的file_area。等N个周期后，继续从file_stat->temp 链表尾遍历file_area，只用遍历链表尾的冷file_area后，就可以结束遍历，进入冷却期。这样就可以大大降级性能损耗，因为不用遍历整个file_stat->temp链表。这个方案的缺点时，在文件file_stat进去冷却期后，N个周期内，不再遍历file_stat->temp 链表上的file_area，也牵连到不能遍历 file_stat->free、refault、hot、mapcount 链表上的file_area。因为check_file_area_cold_page_and_clear()函数中，这些链表上的file_area是连续遍历的。后期可以考虑把遍历file_stat->temp 链表上的file_area 和 遍历 file_stat->free、refault、hot、mapcount 链表上的file_area 分开????????????????????????????????其实也没必要分开，file_stat的冷却期N，也不会太长，而file_stat->free、refault、hot、mapcount  链表上的file_area 的page都比较特殊，根本不用频繁遍历，N个周期不遍历也没啥事，反而能降低性能损耗。

2:比如 reverse_file_area_mapcount_and_hot_list 函数，每次都从file_stat->file_area_mapcount链表尾遍历一定数据的file_area，并记录当时的global age，然后移动到链表头。下次还是从file_stat->file_area_mapcount链表尾开始遍历file_area，如果file_area的age与gloabl age小于M，结束遍历。就是说，这些链表上的file_area必须间隔M个周期才能再遍历一次，降级性能损耗。这种的优点是设计简单，易于理解，但是不能保证链表尾的都是冷file_area。
 * */

#if 0 //下边的代码很有意义，不要删除，犯过很多错误
static int get_file_area_from_mmap_file_stat_list(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max,unsigned int scan_file_stat_max,struct list_head *file_stat_temp_head)//file_stat_temp_head链表来自 global->mmap_file_stat_temp_head 和 global->mmap_file_stat_large_file_head 链表
{
	struct file_stat * p_file_stat = NULL,*p_file_stat_temp = NULL;
	unsigned int scan_file_area_count  = 0;
	unsigned int scan_file_stat_count  = 0;
	unsigned int free_pages = 0;
	int ret = 0;
	char delete_file_stat_last = 0;
	char scan_next_file_stat = 0;

	if(list_empty(file_stat_temp_head))
		return ret;

	printk("1:%s file_stat_last:0x%llx\n",__func__,(u64)p_hot_cold_file_global->file_stat_last);
	if(p_hot_cold_file_global->file_stat_last){//本次从上一轮扫描打断的file_stat继续遍历
		p_file_stat = p_hot_cold_file_global->file_stat_last;
	}
	else{
		//第一次从链表尾的file_stat开始遍历
		p_file_stat = list_last_entry(file_stat_temp_head,struct file_stat,hot_cold_file_list);
		p_hot_cold_file_global->file_stat_last = p_file_stat;
	}	

	do{
		/*加个panic判断，如果p_file_stat是链表头p_hot_cold_file_global->mmap_file_stat_temp_head，那就触发panic*/

		/*查找file_stat在global->mmap_file_stat_temp_head链表上一个file_stat。如果p_file_stat不是链表头的file_stat，直接list_prev_entry
		 * 找到上一个file_stat。如果p_file_stat是链表头的file_stat，那要跳过链表过，取出链表尾的file_stat*/
		if(!list_is_first(&p_file_stat->hot_cold_file_list,file_stat_temp_head))
			p_file_stat_temp = list_prev_entry(p_file_stat,hot_cold_file_list);
		else
			p_file_stat_temp = list_last_entry(file_stat_temp_head,struct file_stat,hot_cold_file_list);

		if(!file_stat_in_file_stat_temp_head_list(p_file_stat) || file_stat_in_file_stat_temp_head_list_error(p_file_stat)){
			panic("%s file_stat:0x%llx not int file_stat_temp_head status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
		}


		/*因为存在一种并发，1：文件mmap映射分配file_stat向global mmap_file_stat_temp_head添加，并赋值mapping->rh_reserved1=p_file_stat，
		 * 2：这个文件cache读写执行hot_file_update_file_status()，分配file_stat向global file_stat_temp_head添加,并赋值
		 * mapping->rh_reserved1=p_file_stat。因为二者流程并发执行，因为mapping->rh_reserved1是NULL，导致两个流程都分配了file_stat并赋值
		 * 给mapping->rh_reserved1。因此这里的file_stat可能就是cache读写产生的，目前先暂定把mapping->rh_reserved1清0，让下次文件cache读写
		 * 再重新分配file_stat并赋值给mapping->rh_reserved1。这个问题没有其他并发问题，无非就是分配两个file_stat都赋值给mapping->rh_reserved1。
		 *
		 * 还有一点，异步内存回收线程walk_throuth_all_file_area()回收cache文件的page时，从global temp链表遍历file_stat时，要判断
		 * if(file_stat_in_mmap_file(p_file_stat))，是的话也要p_file_stat->mapping->rh_reserved1 = 0并跳过这个file_stat
		 *
		 * 这个问题有个解决方法，就是mmap文件分配file_stat 和cache文件读写分配file_stat，都是用global_lock锁，现在用的是各自的锁。
		 * 这样就避免了分配两个file_stat并都赋值给mapping->rh_reserved1
		 * */
		if(file_stat_in_cache_file(p_file_stat)){
			/*如果p_file_stat从从global mmap_file_stat_temp_head链表剔除，且与p_hot_cold_file_global->file_stat_last指向同一个file_stat。
			 *那要把p_file_stat在global mmap_file_stat_temp_head链表的上一个file_stat(即p_file_stat_temp)赋值给p_hot_cold_file_global->file_stat_last。
			 *否则，会导致下边的while(p_file_stat != p_hot_cold_file_global->file_stat_last)永远不成立,陷入死循环,详解见check_file_area_cold_page_and_clear()*/
			if(p_hot_cold_file_global->file_stat_last == p_file_stat){
				p_hot_cold_file_global->file_stat_last = p_file_stat_temp;
				delete_file_stat_last = 1;
			}
			spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
			clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			set_file_stat_in_delete(p_file_stat);
			p_file_stat->mapping->rh_reserved1 = 0;
			list_del(&p_file_stat->hot_cold_file_list);
			spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

			/*释放掉file_stat的所有file_area，最后释放掉file_stat。但释放file_stat用的还是p_hot_cold_file_global->global_lock锁防护
			 *并发，这点后期需要改进!!!!!!!!!!!!!!!!!!!!!!!!!*/
			cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat);
			printk("%s p_file_stat:0x%llx status:0x%lx in_cache_file\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			//p_file_stat = p_file_stat_temp;
			//continue;
			goto next;
		}else if(file_stat_in_delete(p_file_stat)){
			printk("%s p_file_stat:0x%llx status:0x%lx in_delete\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			//上边有解释
			if(p_hot_cold_file_global->file_stat_last == p_file_stat){
				p_hot_cold_file_global->file_stat_last = p_file_stat_temp;
				delete_file_stat_last = 1;
			}

			/*释放掉file_stat的所有file_area，最后释放掉file_stat*/
			cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat);
			//p_file_stat = p_file_stat_temp;
			//continue;
			goto next;
		}

		/*针对0个file_area的file_stat，不能把它移动到mmap_file_stat_zero_file_area_head链表，然后释放掉file_stat。因为如果后续这个文件file_stat
		 *又有文件页page被访问并分配，建立页表页目录映射，我的代码感知不到这点，但是file_stat已经释放了。这种情况下的文件页就无法被内存回收了!
		 *那什么情况下才能释放file_stat呢？在unmap 文件时，可以释放file_stat吗？可以，但是需要保证在unmap且没有其他进程mmap映射这个文件时，
		 *才能unmap时释放掉file_stat结构。这样稍微有点麻烦！还有一种情况可以释放file_stat，就是文件indoe被释放时，这种情况肯定可以释放掉
		 *file_stat结构*/
		if(p_file_stat->file_area_count == 0){
			/*spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
			  clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			  set_file_stat_in_zero_file_area_list(p_file_stat);
			  list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_zero_file_area_head);
			  spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
			  goto next;*/
		}

		ret = traverse_mmap_file_stat_get_cold_page(p_hot_cold_file_global,p_file_stat,scan_file_area_max,&scan_file_area_count);
		//返回值是1是说明当前这个file_stat的temp链表上的file_area已经全扫描完了，则扫描该file_stat在global->mmap_file_stat_large_file_head或global->mmap_file_stat_temp_head链表上的上一个file_stat的file_area
		if(ret > 0){
			scan_next_file_stat = 1; 
		}else if(ret < 0){
			return -1;
		}

#if 0 
		//---------------------重大后期改进
		/*ret是1说明这个file_stat还没有从radix tree遍历完一次所有的page，那就把file_stat移动到global mmap_file_stat_temp_head链表尾
		//这样下个周期还能继续扫描这个文件radix tree的page--------------这个解决方案不行，好的解决办法是每次设定最多遍历新文件的page
		//的个数，如果本次这个文件没有遍历完，下次也要从这个文件继续遍历。
		//有个更好的解决方案，新的文件的file_stat要添加到global mmap_temp_not_done链表，只有这个文件的page全遍历完一次，再把这个file_stat
		//移动到global mmap_file_stat_temp_head链表。异步内存回收线程每次两个链表上的文件file_stat按照一定比例都分开遍历，谁也不影响谁*/
		if(1 == ret){
			if(!list_is_last(&p_file_stat->hot_cold_file_list,file_stat_temp_head)){
				spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
				list_move_tail(&p_file_stat->hot_cold_file_list,file_stat_temp_head);
				spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
			}
		}
#endif

next:
		//只有当前的file_stat的temp链表上的file_area扫描完，才能扫描下一个file_stat
		if(scan_next_file_stat){
			printk("%s p_file_stat:0x%llx file_area:%d scan complete,next scan file_stat:0x%llx\n",__func__,(u64)p_file_stat,p_file_stat->file_area_count,(u64)p_file_stat_temp);
			//下一个file_stat
			//p_file_stat = p_file_stat_temp;
			scan_file_stat_count ++;
		}

		//遍历指定数目的file_stat和file_area后，强制结束遍历
		if(scan_file_area_count >= scan_file_area_max || scan_file_stat_count  >= scan_file_stat_max){
			printk("%s scan_file_area_count:%d scan_file_stat_count:%d exceed max\n",__func__,scan_file_area_count,scan_file_stat_count);
			break;
		}

		if(0 == delete_file_stat_last && p_file_stat == p_hot_cold_file_global->file_stat_last){
			printk("%s p_file_stat:0x%llx == p_hot_cold_file_global->file_stat_last\n",__func__,(u64)p_file_stat);
			break;
		}
		else if(delete_file_stat_last)
			delete_file_stat_last = 0;

		//在scan_next_file_stat时把p_file_stat = p_file_stat_temp赋值放到下边。因为，如果上边可能break了，
		//而p_file_stat = p_file_stat_temp已经赋值过了，但这个file_stat根本没扫描。接着跳出while循环，
		//下边对p_hot_cold_file_global->file_stat_last新的file_stat，而当前的file_stat就漏掉扫描了!!!!!!!!
		if(scan_next_file_stat){
			//下一个file_stat
			p_file_stat = p_file_stat_temp;
		}


	/*这里退出循环的条件，不能碰到链表头退出，是一个环形队列的遍历形式。主要原因是不想模仿read/write文件页的内存回收思路：
	 *先加锁从global temp链表隔离几十个文件file_stat，清理file_stat的状态，然后内存回收。内存回收后再把file_stat加锁
	 *移动到global temp链表头。这样太麻烦了，还浪费性能。针对mmap文件页的内存回收，不用担心并发问题，不用这么麻烦
	 *
	 * 以下两种情况退出循环
	 *1：上边的 遍历指定数目的file_stat和file_area后，强制结束遍历
	 *2：这里的while，本次循环处理到file_stat已经是第一次循环处理过了，相当于重复了
	 *3：添加!list_empty(file_stat_temp_head)判断，原理分析在check_file_area_cold_page_and_clear()函数
	 */
	//}while(p_file_stat != p_hot_cold_file_global->file_stat_last);
	//}while(p_file_stat != p_hot_cold_file_global->file_stat_last && !list_empty(file_stat_temp_head));
	}while(!list_empty(file_stat_temp_head));

	/*scan_next_file_stat如果是1，说明当前文件file_stat的temp链表上已经扫描的file_area个数超过该文件temp链表的总file_area个数，
	 *然后才能更新p_hot_cold_file_global->file_stat_last，这样下次才能扫描该file_stat在global->mmap_file_stat_large_file_head
	 *或global->mmap_file_stat_temp_head链表上的上一个file_stat*/
	if(1 == scan_next_file_stat){
		if(!list_empty(file_stat_temp_head)){
			/*p_hot_cold_file_global->file_stat_last指向_hot_cold_file_global->file_stat_temp_head链表上一个file_area，下个周期
			 *直接从p_hot_cold_file_global->file_stat_last指向的file_stat开始扫描*/
			if(!list_is_first(&p_file_stat->hot_cold_file_list,file_stat_temp_head))
				p_hot_cold_file_global->file_stat_last = list_prev_entry(p_file_stat,hot_cold_file_list);
			else
				p_hot_cold_file_global->file_stat_last = list_last_entry(file_stat_temp_head,struct file_stat,hot_cold_file_list);
		}else{
			p_hot_cold_file_global->file_stat_last = NULL;
		}
	}
	//err:
	return free_pages;
}
#endif
static noinline int get_file_area_from_mmap_file_stat_list_common(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat_base *p_file_stat_base,unsigned int scan_file_area_max,unsigned int file_stat_list_type,unsigned int file_type)//file_stat_temp_head链表来自 global temp、middle、large file 链表
{
	struct file_stat *p_file_stat = NULL,*p_file_stat_temp = NULL;
	unsigned int scan_file_area_count  = 0;
	unsigned int scan_file_stat_count  = 0;
	//unsigned int free_pages = 0;
	int ret = 0;
	LIST_HEAD(file_stat_list);
	/*这个跟file_stat_list_type有区别，这个是真实标记file_stat处于哪个global 链表*/
	//unsigned int file_stat_in_list_type = 0;


	//if(list_empty(file_stat_temp_head))
	//	return ret;
	if(shrink_page_printk_open)
		printk("1:%s file_stat_last:0x%llx\n",__func__,(u64)p_hot_cold_file_global->file_stat_last);

	//每次都从链表尾开始遍历
	//list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,file_stat_temp_head,hot_cold_file_list){

	/*如果p_file_stat所处的global temp、middle、large链表错误，触发panic*/
    switch (file_stat_list_type){
		case F_file_stat_in_file_stat_tiny_small_file_head_list:
			/*file_stat此时可能被方法iput()释放delete，但file_stat的delete的标记与file_stat in_temp_list 是共存的，不干扰file_stat in_temp_list的判断*/
			if(!file_stat_in_file_stat_temp_head_list_base(p_file_stat_base) || file_stat_in_file_stat_temp_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_temp_head status:0x%lx\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		case F_file_stat_in_file_stat_small_file_head_list:
			/*file_stat此时可能被方法iput()释放delete，但file_stat的delete的标记与file_stat in_temp_list 是共存的，不干扰file_stat in_temp_list的判断*/
			if(!file_stat_in_file_stat_temp_head_list_base(p_file_stat_base) || file_stat_in_file_stat_temp_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_temp_head status:0x%lx\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		case F_file_stat_in_file_stat_temp_head_list:
			/*file_stat此时可能被方法iput()释放delete，但file_stat的delete的标记与file_stat in_temp_list 是共存的，不干扰file_stat in_temp_list的判断*/
			if(!file_stat_in_file_stat_temp_head_list_base(p_file_stat_base) || file_stat_in_file_stat_temp_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_temp_head status:0x%lx\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		case F_file_stat_in_file_stat_middle_file_head_list:
			if(!file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_middle_file_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_middle_file_head status:0x%lx\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		case F_file_stat_in_file_stat_large_file_head_list:
			if(!file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base) || file_stat_in_file_stat_large_file_head_list_error_base(p_file_stat_base))
				panic("%s file_stat:0x%llx not int file_stat_large_file_head status:0x%lx\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
			break;
		default:	
			panic("%s p_file_stat:0x%llx file_stat_list_type:%d error\n",__func__,(u64)p_file_stat_base,file_stat_list_type);
			break;
	}


	//遍历指定数目的file_stat和file_area后，强制结束遍历。包括遍历delete等文件，scan_file_area_count下边遍历file_area时自动增加，这里不加1
	/*if(scan_file_area_count >= scan_file_area_max){
		if(shrink_page_printk_open)
			printk("%s scan_file_area_count:%d scan_file_stat_count:%d exceed max\n",__func__,scan_file_area_count,scan_file_stat_count);

		break;
	}
	scan_file_stat_count ++;*/

	/*因为存在一种并发，1：文件mmap映射分配file_stat向global mmap_file_stat_temp_head添加，并赋值mapping->rh_reserved1=p_file_stat，
	 * 2：这个文件cache读写执行hot_file_update_file_status()，分配file_stat向global file_stat_temp_head添加,并赋值
	 * mapping->rh_reserved1=p_file_stat。因为二者流程并发执行，因为mapping->rh_reserved1是NULL，导致两个流程都分配了file_stat并赋值
	 * 给mapping->rh_reserved1。因此这里的file_stat可能就是cache读写产生的，目前先暂定把mapping->rh_reserved1清0，让下次文件cache读写
	 * 再重新分配file_stat并赋值给mapping->rh_reserved1。这个问题没有其他并发问题，无非就是分配两个file_stat都赋值给mapping->rh_reserved1。
	 *
	 * 还有一点，异步内存回收线程walk_throuth_all_file_area()回收cache文件的page时，从global temp链表遍历file_stat时，要判断
	 * if(file_stat_in_mmap_file(p_file_stat))，是的话也要p_file_stat->mapping->rh_reserved1 = 0并跳过这个file_stat
	 *
	 * 这个问题有个解决方法，就是mmap文件分配file_stat 和cache文件读写分配file_stat，都是用global_lock锁，现在用的是各自的锁。
	 * 这样就避免了分配两个file_stat并都赋值给mapping->rh_reserved1
	 * */
	if(file_stat_in_cache_file_base(p_file_stat_base)){
		panic("%s p_file_stat:0x%llx status:0x%lx in_cache_file\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

		spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
		clear_file_stat_in_file_stat_temp_head_list_base(p_file_stat_base);
		set_file_stat_in_delete_base(p_file_stat_base);
		//p_file_stat->mapping->rh_reserved1 = 0;
		p_file_stat_base->mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE;/*之前赋值0，现在赋值1，一样效果*/
		list_del(&p_file_stat_base->hot_cold_file_list);
		spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

		/*释放掉file_stat的所有file_area，最后释放掉file_stat。但释放file_stat用的还是p_hot_cold_file_global->global_lock锁防护
		 *并发，这点后期需要改进!!!!!!!!!!!!!!!!!!!!!!!!!*/
		cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat_base,file_type);
		return 0;
	}else if(file_stat_in_delete_base(p_file_stat_base)){
		printk("%s p_file_stat:0x%llx status:0x%lx in_delete\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);
		return 0;
	}

	/*file_stat的file_area全被释放了释放后，过了一段时间依然没有被访问则释放掉file_stat*/
	if((p_file_stat_base->file_area_count == 0) && (p_hot_cold_file_global->global_age - p_file_stat_base->recent_access_age > FILE_STAT_DELETE_AGE_DX)){
		cold_file_stat_delete(p_hot_cold_file_global,p_file_stat_base,file_type);
		return 0;
	}

	//如果文件file_stat还在冷却期，不扫描这个文件file_stat->temp链表上的file_area，只是把file_stat移动到file_stat_list临时链表
	if(p_file_stat_base->cooling_off_start){
		if(p_hot_cold_file_global->global_age - p_file_stat_base->cooling_off_start_age < MMAP_FILE_AREA_COLD_AGE_DX){
			/*每次都加锁太浪费性能，改为list_for_each_entry_safe_reverse()结束后，统一把遍历过的file_stat移动到
			 *file_stat_temp_head(global temp、middle、large)链表头，保证下次从链表尾遍历的file_stat是新的未遍历
			 *过的file_stat*/
#if 0	
			/*还是使用p_hot_cold_file_global->file_stat_last从global temp链表尾指向链表头的一个个file_stat，遍历
			 * 一个个file_stat吧，这样不用遍历每个file_stat都加global mmap_file_global_lock锁了*/
			spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
			list_move(&p_file_stat->hot_cold_file_list,&file_stat_list);
			spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
#endif				
			return 0;
		}
		else{
			p_file_stat_base->cooling_off_start = 0;
		}
	}
	/*file_stat的recent_access_age记录最近一次被访问时的global age。但这个赋值必须得保证file_stat不是处于冷却期。
	 * 否则这里对p_file_stat->recent_access_age赋值就是对p_file_stat->cooling_off_start_age赋值，因为二者是union类型*/
	if(0 == p_file_stat_base->cooling_off_start)
		p_file_stat_base->recent_access_age = p_hot_cold_file_global->global_age;

	/*针对0个file_area的file_stat，不能把它移动到mmap_file_stat_zero_file_area_head链表，然后释放掉file_stat。因为如果后续这个文件file_stat
	 *又有文件页page被访问并分配，建立页表页目录映射，我的代码感知不到这点，但是file_stat已经释放了。这种情况下的文件页就无法被内存回收了!
	 *那什么情况下才能释放file_stat呢？在unmap 文件时，可以释放file_stat吗？可以，但是需要保证在unmap且没有其他进程mmap映射这个文件时，
	 *才能unmap时释放掉file_stat结构。这样稍微有点麻烦！还有一种情况可以释放file_stat，就是文件indoe被释放时，这种情况肯定可以释放掉
	 *file_stat结构*/
	if(p_file_stat_base->file_area_count == 0){//这段代码比较重要不要删除---------------------------
		/*spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
		  clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
		  set_file_stat_in_zero_file_area_list(p_file_stat);
		  list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_zero_file_area_head);
		  spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
		  goto next;*/
	}

	ret = traverse_mmap_file_stat_get_cold_page(p_hot_cold_file_global,p_file_stat_base,scan_file_area_max,&scan_file_area_count,file_stat_list_type,file_type);
	//返回值是1是说明当前这个file_stat的temp链表上的file_area已经全扫描完了，则扫描该file_stat在global->mmap_file_stat_large_file_head或global->mmap_file_stat_temp_head链表上的上一个file_stat的file_area
	if(ret < 0){
		//return -1;
		/*不能直接return -1。因此此时file_stat_list链表保存了已经遍历过file_stat，这里直接return的话，会导致
		 * 无法执行下边的list_splice(&file_stat_list,file_stat_temp_head)把这些file_stat再移动回global temp或temp_large_file链表,
		 * 故导致这些file_stat一直残留在file_stat_list这个临时链表，状态就错了，将来iput() list_del这些file_stat就会__list_del_entry_valid()报错*/
		goto err;
	}

	/*到这里，只有几种情况
	 *1：当前文件p_file_stat->temp链表上的file_area扫描了一遍，ret是1，此时需要把p_file_stat移动到file_stat_list临时链表，然后下轮for循环扫描下一个文件
	 *2：当前文件p_file_stat->temp链表上的file_area太多了，已经扫描的file_area个数超过scan_file_stat_max，break退出，下次执行该函数还要继续扫描p_file_stat这个文件
	 * */

	//只有当前的file_stat的temp链表上的file_area扫描完，才能扫描下一个file_stat。或者当前文件处于冷却期，也扫描下一个file_stat
	if(ret == FILE_STAT_SCAN_ALL_FILE_AREA || ret == FILE_STAT_SCAN_IN_COOLING){
		/*遍历过的文件file_stat移动到file_stat_list临时链表。但可能这个file_stat因为热file_area增多而变成了热file_area而移动到了global hot链表。
		 *此时这里再把这个热file_area移动到file_stat_list临时链表，该函数最后再把它移动到global temp链表，那该热file_stat处于的链表就错了，会crash
		 *解决办法是限制这个file_stat必须处于global temp链表才能移动到file_stat_list临时链表*/
		if(file_stat_in_file_stat_temp_head_list_base(p_file_stat_base) || file_stat_in_file_stat_middle_file_head_list_base(p_file_stat_base) || 
				file_stat_in_file_stat_large_file_head_list_base(p_file_stat_base)){
			unsigned int normal_file_type;

			p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);
			/*根据file_stat的file_area个数判断文件是普通文件、中型文件、大文件*/
			normal_file_type = is_mmap_file_stat_file_type(p_hot_cold_file_global,p_file_stat);
			/*file_stat的file_area个数与file_stat所处global 链表不匹配，则把file_stat移动到匹配的global temp、middle、large链表*/
			file_stat_temp_middle_large_file_change(p_hot_cold_file_global,p_file_stat,file_stat_list_type,normal_file_type,0);
		}
		if(shrink_page_printk_open)
			printk("%s p_file_stat:0x%llx file_area:%d scan complete,next scan file_stat:0x%llx\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_area_count,(u64)p_file_stat_temp);
	}
	else{
		if(ret != FILE_STAT_SCAN_MAX_FILE_AREA)
			panic("%s file_stat:0x%llx status:0x%lx exception scan_file_area_count:%d scan_file_stat_count:%d ret:%d\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,scan_file_area_count,scan_file_stat_count,ret);
	}

	//}
err:
	
	return scan_file_area_count;
}

static noinline int get_file_area_from_mmap_file_stat_list(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max,unsigned int scan_file_stat_max,struct list_head *file_stat_temp_head,unsigned int file_stat_list_type)//file_stat_temp_head链表来自 global temp、middle、large file 链表
{
	struct file_stat * p_file_stat = NULL,*p_file_stat_temp = NULL;
	unsigned int scan_file_area_count  = 0;
	unsigned int scan_file_stat_count  = 0;
	//unsigned int free_pages = 0;
	int ret = 0;
	LIST_HEAD(file_stat_list);
	/*这个跟file_stat_list_type有区别，这个是真实标记file_stat处于哪个global 链表*/
	unsigned int file_stat_in_list_type = 0;


	if(list_empty(file_stat_temp_head))
		return ret;
	if(shrink_page_printk_open)
		printk("1:%s file_stat_last:0x%llx\n",__func__,(u64)p_hot_cold_file_global->file_stat_last);

	//每次都从链表尾开始遍历
	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,file_stat_temp_head,hot_cold_file_list){
		if(scan_file_area_count >= scan_file_area_max)
			break;

		scan_file_area_count += get_file_area_from_mmap_file_stat_list_common(p_hot_cold_file_global,&p_file_stat->file_stat_base,scan_file_area_max,file_stat_list_type,FILE_STAT_NORMAL);
		scan_file_stat_count ++;
	}
//err:
	//如果file_stat_list临时链表还有file_stat，则把这些file_stat移动到global temp链表头，下轮循环就能从链表尾巴扫描还没有扫描的file_stat了
	spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);/*这是把file_stat移动到global temp链表头，因此必须要加global lock*/
#if 0
	if(!list_empty(&file_stat_list)){
		list_splice(&file_stat_list,file_stat_temp_head);
	}
#else
	/*p_file_stat不能是链表头，并且不能是被iput()并发标记delete并移动到global delete链表。详情见见get_file_area_from_file_stat_list()函数*/
	if(&p_file_stat->hot_cold_file_list != file_stat_temp_head  && !file_stat_in_delete(p_file_stat)){
		/*把本次遍历过的file_stat移动到file_stat_temp_head链表头，保证下次你从file_stat_temp_head链表尾遍历的file_stat是最新的没有遍历过的，此时必须加锁*/
		if(can_file_stat_move_to_list_head(p_file_stat,file_stat_in_list_type))
			list_move_enhance(file_stat_temp_head,&p_file_stat->hot_cold_file_list);
	}
#endif	
	spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

	p_hot_cold_file_global->mmap_file_shrink_counter.scan_file_area_count += scan_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.scan_file_stat_count += scan_file_stat_count;
	return scan_file_area_count;
}
static noinline int get_file_area_from_mmap_file_stat_small_list(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max,unsigned int scan_file_stat_max,struct list_head *file_stat_temp_head,unsigned int file_stat_list_type)
{
	struct file_stat_small *p_file_stat_small = NULL,*p_file_stat_small_temp = NULL;
	unsigned int scan_file_area_count  = 0;
	unsigned int scan_file_stat_count  = 0;
	//unsigned int free_pages = 0;
	int ret = 0;
	LIST_HEAD(file_stat_list);
	/*这个跟file_stat_list_type有区别，这个是真实标记file_stat处于哪个global 链表*/
	unsigned int file_stat_in_list_type = 0;


	if(list_empty(file_stat_temp_head))
		return ret;
	if(shrink_page_printk_open)
		printk("1:%s file_stat_last:0x%llx\n",__func__,(u64)p_hot_cold_file_global->file_stat_last);

	//每次都从链表尾开始遍历
	list_for_each_entry_safe_reverse(p_file_stat_small,p_file_stat_small_temp,file_stat_temp_head,hot_cold_file_list){	   
		if(scan_file_area_count >= scan_file_area_max)
			break;
		/* small文件的file_area个数如果超过阀值则转换成normal文件等。这个操作必须放到get_file_area_from_file_stat_list_common()
		 * 函数里遍历该file_stat的file_area前边，以保证该文件的in_refault、in_hot、in_free属性的file_area都集中在small->other链表尾的640
		 * 个file_area，后续即便大量新增file_area，都只在small->other链表头，详情见can_small_file_change_to_normal_file()注释*/
		if(unlikely(p_file_stat_small->file_area_count > NORMAL_TEMP_FILE_AREA_COUNT_LEVEL))
			can_small_file_change_to_normal_file(p_hot_cold_file_global,p_file_stat_small,0);
		else 
			scan_file_area_count += get_file_area_from_mmap_file_stat_list_common(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,scan_file_area_max,file_stat_list_type,FILE_STAT_SMALL);

		scan_file_stat_count ++;
	}
//err:
	//如果file_stat_list临时链表还有file_stat，则把这些file_stat移动到global temp链表头，下轮循环就能从链表尾巴扫描还没有扫描的file_stat了
	spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);/*这是把file_stat移动到global temp链表头，因此必须要加global lock*/
#if 0
	if(!list_empty(&file_stat_list)){
		list_splice(&file_stat_list,file_stat_temp_head);
	}
#else
	/*p_file_stat_small不能是链表头，并且不能是被iput()并发标记delete并移动到global delete链表。详情见见get_file_area_from_file_stat_list()函数*/
	if(&p_file_stat_small->hot_cold_file_list != file_stat_temp_head  && !file_stat_in_delete_base(&p_file_stat_small->file_stat_base)){
		/*把本次遍历过的file_stat移动到file_stat_temp_head链表头，保证下次你从file_stat_temp_head链表尾遍历的file_stat是最新的没有遍历过的，此时必须加锁*/
		if(can_file_stat_move_to_list_head_base(&p_file_stat_small->file_stat_base,file_stat_in_list_type))
			list_move_enhance(file_stat_temp_head,&p_file_stat_small->hot_cold_file_list);
	}
#endif	
	spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

	p_hot_cold_file_global->mmap_file_shrink_counter.scan_file_area_count += scan_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.scan_file_stat_count += scan_file_stat_count;
	return scan_file_area_count;
}
static noinline int get_file_area_from_mmap_file_stat_tiny_small_list(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max,unsigned int scan_file_stat_max,struct list_head *file_stat_temp_head,unsigned int file_stat_list_type)
{
	struct file_stat_tiny_small *p_file_stat_tiny_small = NULL,*p_file_stat_tiny_small_temp = NULL;
	unsigned int scan_file_area_count  = 0;
	unsigned int scan_file_stat_count  = 0;
	//unsigned int free_pages = 0;
	int ret = 0;
	LIST_HEAD(file_stat_list);
	/*这个跟file_stat_list_type有区别，这个是真实标记file_stat处于哪个global 链表*/
	unsigned int file_stat_in_list_type = 0;


	if(list_empty(file_stat_temp_head))
		return ret;
	if(shrink_page_printk_open)
		printk("1:%s file_stat_last:0x%llx\n",__func__,(u64)p_hot_cold_file_global->file_stat_last);

	//每次都从链表尾开始遍历
	list_for_each_entry_safe_reverse(p_file_stat_tiny_small,p_file_stat_tiny_small_temp,file_stat_temp_head,hot_cold_file_list){
		if(scan_file_area_count >= scan_file_area_max)
			break;
		/* tiny small文件的file_area个数如果超过阀值则转换成small或normal文件等。这个操作必须放到get_file_area_from_file_stat_list_common()
		 * 函数里遍历该file_stat的file_area前边，以保证该文件的in_refault、in_hot、in_free属性的file_area都集中在tiny small->temp链表尾的64
		 * file_area，后续即便大量新增file_area，都只在tiny small->temp链表头，详情见can_tiny_small_file_change_to_small_normal_file()注释*/
		if(unlikely(p_file_stat_tiny_small->file_area_count > SMALL_FILE_AREA_COUNT_LEVEL))
			can_tiny_small_file_change_to_small_normal_file(p_hot_cold_file_global,p_file_stat_tiny_small,0);
		else 
			scan_file_area_count += get_file_area_from_mmap_file_stat_list_common(p_hot_cold_file_global,&p_file_stat_tiny_small->file_stat_base,scan_file_area_max,file_stat_list_type,FILE_STAT_TINY_SMALL);
		scan_file_stat_count ++;
	}
//err:
	//如果file_stat_list临时链表还有file_stat，则把这些file_stat移动到global temp链表头，下轮循环就能从链表尾巴扫描还没有扫描的file_stat了
	spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);/*这是把file_stat移动到global temp链表头，因此必须要加global lock*/
#if 0
	if(!list_empty(&file_stat_list)){
		list_splice(&file_stat_list,file_stat_temp_head);
	}
#else
	/*p_file_stat_tiny_small不能是链表头，并且不能是被iput()并发标记delete并移动到global delete链表。详情见见get_file_area_from_file_stat_list()函数*/
	if(&p_file_stat_tiny_small->hot_cold_file_list != file_stat_temp_head  && !file_stat_in_delete_base(&p_file_stat_tiny_small->file_stat_base)){
		/*把本次遍历过的file_stat移动到file_stat_temp_head链表头，保证下次你从file_stat_temp_head链表尾遍历的file_stat是最新的没有遍历过的，此时必须加锁*/
		if(can_file_stat_move_to_list_head_base(&p_file_stat_tiny_small->file_stat_base,file_stat_in_list_type))
			list_move_enhance(file_stat_temp_head,&p_file_stat_tiny_small->hot_cold_file_list);
	}
#endif	
	spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

	p_hot_cold_file_global->mmap_file_shrink_counter.scan_file_area_count += scan_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.scan_file_stat_count += scan_file_stat_count;
	return scan_file_area_count;
}

#if 0
static noinline int get_file_area_from_small_mmap_file_stat_list(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max,unsigned int scan_file_stat_max,struct list_head *file_stat_temp_head,unsigned char file_stat_list_type)//file_stat_temp_head链表来自 global small file 链表
{
	struct file_stat_small *p_file_stat_small = NULL,*p_file_stat_small_temp = NULL;
    //struct file_stat *p_file_stat;
    struct file_stat_base *p_file_stat_base;
    struct file_area *p_file_area,*p_file_area_temp;
	unsigned int scan_file_area_count  = 0;
	unsigned int scan_file_stat_count  = 0;
	//unsigned int free_pages = 0;
	int ret = 0;
	LIST_HEAD(file_stat_list);
	/*这个跟file_stat_list_type有区别，这个是真实标记file_stat处于哪个global 链表*/
	//unsigned int file_stat_in_list_type = 0;
	/*file_area里的page至少一个page发现是cache page的，则该file_area移动到file_area_have_cache_page_head，后续回收cache的文件页*/
	LIST_HEAD(file_area_have_cache_page_head);
	LIST_HEAD(file_area_free);

	if(list_empty(file_stat_temp_head))
		return ret;
	if(shrink_page_printk_open)
		printk("1:%s file_stat_last:0x%llx\n",__func__,(u64)p_hot_cold_file_global->file_stat_last);

	//每次都从链表尾开始遍历
	list_for_each_entry_safe_reverse(p_file_stat_small,p_file_stat_small_temp,file_stat_temp_head,hot_cold_file_list){

		//文件变大了，不再使用file_area_small而释放掉，重新分配file_stat
		if(p_file_stat_small->file_area_count > 64){
			struct file_stat *p_file_stat;

			//clear_file_stat_in_file_stat_small_file_head_list(p_file_stat_small);
			//set_file_stat_in_file_stat_temp_file_head_list(p_file_stat_small);
			p_file_stat_base = add_mmap_file_stat_to_list(p_file_stat_small->mapping,FILE_STAT_NORMAL,1);
			p_file_stat = container_of(p_file_stat_base,struct file_stat,file_stat_base);

			/*注意，file_stat_alloc_and_init()里分配新的file_stat并赋值给mapping->rh_reserved1后，新的进程读写该文件，
			 * 执行到update_hot_file函数，才会使用新的file_stat，而不是老的file_stat_small。这里加个内存屏障，保证
			 * 对mapping->rh_reserved1立即生效，否则会导致还在使用老的file_stat_small，统计文件冷热信息会丢失*/
			smp_wmb();
			spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
			/*可能此时该文件被iput delete了，要防护*/
			if(!file_stat_in_delete_base(&p_file_stat_small->file_stat_base) && !file_stat_in_delete(p_file_stat)){
				memcpy(&p_file_stat->file_stat_base,&p_file_stat_small->file_stat_base,sizeof(struct file_stat_base));
				call_rcu(&p_file_stat_small->i_rcu, i_file_stat_small_callback);
			}
			spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

			continue;
		}

		//遍历指定数目的file_stat和file_area后，强制结束遍历。包括遍历delete等文件，scan_file_area_count下边遍历file_area时自动增加，这里不加1
		if(scan_file_area_count >= scan_file_area_max){
			if(shrink_page_printk_open)
				printk("%s scan_file_area_count:%d scan_file_stat_count:%d exceed max\n",__func__,scan_file_area_count,scan_file_stat_count);

			break;
		}
		scan_file_stat_count ++;

		/*因为存在一种并发，1：文件mmap映射分配file_stat向global mmap_file_stat_temp_head添加，并赋值mapping->rh_reserved1=p_file_stat，
		 * 2：这个文件cache读写执行hot_file_update_file_status()，分配file_stat向global file_stat_temp_head添加,并赋值
		 * mapping->rh_reserved1=p_file_stat。因为二者流程并发执行，因为mapping->rh_reserved1是NULL，导致两个流程都分配了file_stat并赋值
		 * 给mapping->rh_reserved1。因此这里的file_stat可能就是cache读写产生的，目前先暂定把mapping->rh_reserved1清0，让下次文件cache读写
		 * 再重新分配file_stat并赋值给mapping->rh_reserved1。这个问题没有其他并发问题，无非就是分配两个file_stat都赋值给mapping->rh_reserved1。
		 *
		 * 还有一点，异步内存回收线程walk_throuth_all_file_area()回收cache文件的page时，从global temp链表遍历file_stat时，要判断
		 * if(file_stat_in_mmap_file(p_file_stat))，是的话也要p_file_stat->mapping->rh_reserved1 = 0并跳过这个file_stat
		 *
		 * 这个问题有个解决方法，就是mmap文件分配file_stat 和cache文件读写分配file_stat，都是用global_lock锁，现在用的是各自的锁。
		 * 这样就避免了分配两个file_stat并都赋值给mapping->rh_reserved1
		 * */
		if(file_stat_in_cache_file_base(&p_file_stat_small->file_stat_base)){
			panic("%s p_file_stat:0x%llx status:0x%lx in_cache_file\n",__func__,(u64)p_file_stat_small,p_file_stat_small->file_stat_status);

		}else if(file_stat_in_delete_base(&p_file_stat_small->file_stat_base)){
			printk("%s p_file_stat:0x%llx status:0x%lx in_delete\n",__func__,(u64)p_file_stat_small,p_file_stat_small->file_stat_status);
			continue;
		}

		/*file_stat的file_area全被释放了释放后，过了一段时间依然没有被访问则释放掉file_stat*/
		if((p_file_stat_small->file_area_count == 0) && (p_hot_cold_file_global->global_age - p_file_stat_small->recent_access_age > FILE_STAT_DELETE_AGE_DX)){
			cold_file_stat_delete(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,FILE_STAT_SMALL);
			continue;
		}

		//如果文件file_stat还在冷却期，不扫描这个文件file_stat->temp链表上的file_area，只是把file_stat移动到file_stat_list临时链表
		if(p_file_stat_small->cooling_off_start){
			if(p_hot_cold_file_global->global_age - p_file_stat_small->cooling_off_start_age < MMAP_FILE_AREA_COLD_AGE_DX){
				continue;
			}
			else{
				p_file_stat_small->cooling_off_start = 0;
			}
		}
		/*file_stat的recent_access_age记录最近一次被访问时的global age。但这个赋值必须得保证file_stat不是处于冷却期。
		 * 否则这里对p_file_stat->recent_access_age赋值就是对p_file_stat->cooling_off_start_age赋值，因为二者是union类型*/
		if(0 == p_file_stat_small->cooling_off_start)
			p_file_stat_small->recent_access_age = p_hot_cold_file_global->global_age;

		INIT_LIST_HEAD(&file_area_have_cache_page_head);
		ret = mmap_file_stat_small_list_file_area_solve(p_hot_cold_file_global,p_file_stat_small,scan_file_area_max,&scan_file_area_count,file_stat_list_type,&file_area_have_cache_page_head);
		//返回值是1是说明当前这个file_stat的temp链表上的file_area已经全扫描完了，则扫描该file_stat在global->mmap_file_stat_large_file_head或global->mmap_file_stat_temp_head链表上的上一个file_stat的file_area
		if(ret < 0){
			goto err;
		}

		/*file_area里的page至少一个page发现是cache page的，则该file_area上边已经移动到了file_area_have_cache_page_head，这里回收这些file_area里的cache文件页*/
		if(!list_empty(&file_area_have_cache_page_head)){
			INIT_LIST_HEAD(&file_area_free);
			/* 遍历file_area_have_cache_page_head链表上的file_area，符合内存回收条件的file_area移动到file_area_free
			 * 链表，然后内存回收。不符合内存回收条件的还停留在file_area_have_cache_page_head链表*/
			mmap_file_area_cache_page_solve(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,&file_area_have_cache_page_head,&file_area_free,FILE_STAT_SMALL);
			/*内存回收后的file_area全都移动到ile_stat->file_area_free链表，将来通过file_stat->file_area_free
			 *链表遍历到这些file_area还残留page，说明内存回收失败，则会判定file_area为refault file_area。当然也可以
			 *这里就可以通过file_area_have_cache_page_head遍历这些file_area，检测是否还残留page，有这个必要吗*/
			//list_splice(&file_area_free,&p_file_stat->file_area_free);
			list_splice(&file_area_free,&p_file_stat_small->file_area_temp);

			/*把未参与内存回收的file_area设置in_warm状态后，再移动到file_stat->file_area_warm链表，后续再参与内存回收*/
			list_for_each_entry_safe(p_file_area,p_file_area_temp,&file_area_have_cache_page_head,file_area_list){
				clear_file_area_in_free_list(p_file_area);
				set_file_area_in_temp_list(p_file_area);
			}
			//list_splice(&file_area_have_cache_page_head,&p_file_stat->file_area_warm);
			list_splice(&file_area_have_cache_page_head,&p_file_stat_small->file_area_temp);
		}


		/*到这里，只有几种情况
		 *1：当前文件p_file_stat->temp链表上的file_area扫描了一遍，ret是1，此时需要把p_file_stat移动到file_stat_list临时链表，然后下轮for循环扫描下一个文件
		 *2：当前文件p_file_stat->temp链表上的file_area太多了，已经扫描的file_area个数超过scan_file_stat_max，break退出，下次执行该函数还要继续扫描p_file_stat这个文件
		 * */

		//只有当前的file_stat的temp链表上的file_area扫描完，才能扫描下一个file_stat。或者当前文件处于冷却期，也扫描下一个file_stat
		if(ret == FILE_STAT_SCAN_ALL_FILE_AREA || ret == FILE_STAT_SCAN_IN_COOLING){
			if(shrink_page_printk_open)
				printk("%s p_file_stat:0x%llx file_area_count:%d scan complete,next scan file_stat:0x%llx\n",__func__,(u64)p_file_stat_small,p_file_stat_small->file_area_count,(u64)p_file_stat_small_temp);
		}
		else{
			if(ret != FILE_STAT_SCAN_MAX_FILE_AREA)
				panic("%s file_stat:0x%llx status:0x%lx exception scan_file_area_count:%d scan_file_stat_count:%d ret:%d\n",__func__,(u64)p_file_stat_small,p_file_stat_small->file_stat_status,scan_file_area_count,scan_file_stat_count,ret);
		}
	}
err:
	//如果file_stat_list临时链表还有file_stat，则把这些file_stat移动到global temp链表头，下轮循环就能从链表尾巴扫描还没有扫描的file_stat了
	spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);/*这是把file_stat移动到global temp链表头，因此必须要加global lock*/
	/*p_file_stat不能是链表头，并且不能是被iput()并发标记delete并移动到global delete链表。详情见见get_file_area_from_file_stat_list()函数*/
	if(&p_file_stat_small->hot_cold_file_list != file_stat_temp_head  && !file_stat_in_delete_base(&p_file_stat_small->file_stat_base)){
		/*把本次遍历过的file_stat移动到file_stat_temp_head链表头，保证下次你从file_stat_temp_head链表尾遍历的file_stat是最新的没有遍历过的，此时必须加锁*/
		if(can_file_stat_small_move_to_list_head(p_file_stat_small,F_file_stat_in_file_stat_small_file_head_list))
			list_move_enhance(file_stat_temp_head,&p_file_stat_small->hot_cold_file_list);
	}
	spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

	p_hot_cold_file_global->mmap_file_shrink_counter.scan_file_area_count += scan_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.scan_file_stat_count += scan_file_stat_count;
	return scan_file_area_count;
}
#endif
/*scan_uninit_file_stat()最初是为了ko模式异步内存回收设计的:保存file_area指针的是radix tree，保存page指针的
 *是另一个xarray tree，二者独立。因此，要先执行该函数，遍历xarray tree，探测一个个page，有page的则创建file_area。
  并添加到file_stat->temp链表，后续就可以探测这些page冷热，参与内存回收。

  然而，现在file_area和page是一体了，file_area指针保存在原保存page指针的xarray tree，page指针保存在file_area->pages[]数组。
  后续，如果该mmap文件访问任意一个新的page，先分配一个page，最后执行到 __filemap_add_folio->__filemap_add_folio_for_file_area()
  ->file_area_alloc_and_init()函数，必然分配file_area并添加到file_stat->temp链表，然后把新的page保存到file_area->pages[]
  数组。也就是说，现在不用再主动执行scan_uninit_file_stat()探测page然后创建file_area了。内核原有机制就可以
  保证mmap的文件一旦有新的page，自己创建file_area并添加到file_stat->temp链表。后续异步内存回收线程就可以探测该file_area的
  冷热page，然后内存回收。

  但是这就引入了一个新的问题，并发:异步内存回收线程对file_area、file_stat的链表add/del操作都会跟
  __filemap_add_folio_for_file_area()中也会对file_area、file_stat的链表add操作形成并发
*/
#if 0
//扫描global mmap_file_stat_uninit_head链表上的file_stat的page，page存在的话则创建file_area，否则一直遍历完这个文件的所有page，才会遍历下一个文件
static int scan_uninit_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct list_head *mmap_file_stat_uninit_head,unsigned int scan_page_max)
{
	int k;
	struct hot_cold_file_area_tree_node *parent_node;
	void **page_slot_in_tree;
	unsigned int area_index_for_page;
	int ret = 0;
	struct page *page;
	struct page *pages[PAGE_COUNT_IN_AREA];
	struct address_space *mapping;
	unsigned int scan_file_area_max = scan_page_max >> PAGE_COUNT_IN_AREA_SHIFT;
	unsigned int scan_file_area_count = 0;
	struct file_stat *p_file_stat,*p_file_stat_temp;
	unsigned int file_page_count;
	char mapcount_file_area = 0;
	struct file_area *p_file_area = NULL;

	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,mmap_file_stat_uninit_head,hot_cold_file_list){
		if(p_file_stat->file_stat_status != (1 << F_file_stat_in_mmap_file)){
			/*实际测试这里遇到过file_stat in delte，则把file_stat移动到global mmap_file_stat_temp_head链表尾，
			 *稍后get_file_area_from_mmap_file_stat_list()函数就会把这个delete的file_stat释放掉*/
			if(file_stat_in_delete(p_file_stat)){
				spin_lock(&hot_cold_file_global_info.mmap_file_global_lock);
				list_move_tail(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
				spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
				printk("%s file_stat:0x%llx status:0x%lx in delete\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
				continue;
			}
			else
				panic("%s file_stat:0x%llx status error:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
		}
		mapping = p_file_stat->mapping;
		file_page_count = p_file_stat->mapping->host->i_size >> PAGE_SHIFT;//除以4096

		if(shrink_page_printk_open)
			printk("1:%s scan file_stat:0x%llx\n",__func__,(u64)p_file_stat);

		/*这个while循环扫一个文件file_stat的page，存在的话则创建file_area。有下边这几种情况
		 *1:文件page太多，扫描的file_area超过mac，文件的page还没扫描完，直接break，下次执行函数还扫描这个文件，直到扫描完
		 *2:文件page很少，扫描的file_area未超过max就break，于是把file_stat移动到global->mmap_file_stat_large_file_head或
		 *  global->mmap_file_stat_temp_head链表。这个file_stat就从global->mmap_file_stat_uninit_head链表尾剔除了，然后扫描第2个文件file_stat*/
		while(scan_file_area_count++ < scan_file_area_max){

			memset(pages,0,PAGE_COUNT_IN_AREA*sizeof(struct page *));
			//获取p_file_stat->last_index对应的PAGE_COUNT_IN_AREA文件页page指针并保存到pages数组
			ret = get_page_from_file_area(p_file_stat,p_file_stat->last_index,pages);

			if(shrink_page_printk_open)
				printk("2:%s file_stat:0x%llx start_page_index:%ld get %d page file_area_count_in_temp_list:%d\n",__func__,(u64)p_file_stat,p_file_stat->last_index,ret,p_file_stat->file_area_count_in_temp_list);
			/*遇到一个重大问题，上边打印"file_stat:0xffff8c9f5fbb1320 start_page_index:464 get 0 page"，然后之后每次执行该函数，都从global mmap_file_stat_uninit_head
			 *链表尾遍历file_stat:0xffff8c9f5fbb1320的起始索引是464的4个page，但这些page都没有，于是ret是0，这导致直接goto out。回收每次就陷入了死循环，无法遍历
			 *global mmap_file_stat_uninit_head链表尾其他file_stat，以及file_stat:0xffff8c9f5fbb1320 start_page_index:464 索引后边的page。简单说，因为一个文件
			 *file_stat的page存在空洞，导致每次执行到该函数都都一直遍历这个文件的空洞page，陷入死循环。解决方法是，遇到文件空洞page，ret是0，继续遍历下一个后边的page
			 *避免陷入死循环*/
			if(0 == ret){
			    p_file_stat->last_index += PAGE_COUNT_IN_AREA;
				if(p_file_stat->last_index >= file_page_count){
				    goto complete;
				}

				continue;
			}
			if(ret < 0){
				printk("2_1:%s file_stat:0x%llx start_page_index:%ld get %d fail\n",__func__,(u64)p_file_stat,p_file_stat->last_index,ret);
				goto out; 
			}

			mapcount_file_area = 0;
			p_file_area = NULL;
			/*第一次扫描文件的page，每个周期扫描SCAN_PAGE_COUNT_ONCE个page，一直到扫描完所有的page。4个page一组，每组分配一个file_area结构*/
			for(k = 0;k < PAGE_COUNT_IN_AREA;k++){
				/*这里需要优化，遍历一次radix tree就得到4个page，完全可以实现的，节省性能$$$$$$$$$$$$$$$$$$$$$$$$*/
				//page = xa_load(&mapping->i_pages, p_file_stat->last_index + k);
				page = pages[k];
				if (page && !xa_is_value(page) && page_mapped(page)) {
					//mapcount file_area
					if(0 == mapcount_file_area && page_mapcount(page) > 1){
						mapcount_file_area = 1;
					}

					area_index_for_page = page->index >> PAGE_COUNT_IN_AREA_SHIFT;
					page_slot_in_tree = NULL;
					parent_node = hot_cold_file_area_tree_lookup_and_create(&p_file_stat->hot_cold_file_area_tree_root_node,area_index_for_page,&page_slot_in_tree);
					if(IS_ERR(parent_node)){
						ret = -1;
						printk("3:%s hot_cold_file_area_tree_lookup_and_create fail\n",__func__);
						goto out;
					}
					if(NULL == *page_slot_in_tree){
						//分配file_area并初始化，成功返回非NULL
						p_file_area = file_area_alloc_and_init(parent_node,page_slot_in_tree,area_index_for_page,p_file_stat);
						if(p_file_area == NULL){
							ret = -1;
							goto out;
						}
					}
					else{
						panic("4:%s file_stat:0x%llx file_area index:%d_%ld 0x%llx already alloc!!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_stat,area_index_for_page,page->index,(u64)(*page_slot_in_tree));
					}
					//file_stat->temp 链表上的file_area个数加1
					p_file_stat->file_area_count_in_temp_list ++;
					/*4个连续的page只要有一个在radix tree找到，分配file_area,之后就不再查找其他page了*/
					break;
				}else{
					if(shrink_page_printk_open1)
						printk("4_1:%s file_stat:0x%llx start_page_index:%ld page:0x%llx error\n",__func__,(u64)p_file_stat,p_file_stat->last_index,(u64)page);
				}
			}

			/*如果上边for循环遍历的file_area的page的mapcount都是1，且file_area的page上边没有遍历完，则这里继续遍历完剩余的page*/
			while(0 == mapcount_file_area && k < PAGE_COUNT_IN_AREA){
				page= pages[k];
				if (page && !xa_is_value(page) && page_mapped(page) && page_mapcount(page) > 1){
					mapcount_file_area = 1;
				}
				k ++;
			}
			if(mapcount_file_area){
				if(!file_area_in_temp_list(p_file_area) || file_area_in_temp_list_error(p_file_area)){
					panic("%s file_area:0x%llx status:%d not in file_area_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
				}

				//文件file_stat的mapcount的file_area个数加1
				p_file_stat->mapcount_file_area_count ++;
				//file_stat->temp 链表上的file_area个数减1
				p_file_stat->file_area_count_in_temp_list --;
				//file_area的page的mapcount大于1，则把file_area移动到file_stat->file_area_mapcount链表
				clear_file_area_in_temp_list(p_file_area);
				set_file_area_in_mapcount_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);
				if(shrink_page_printk_open)
					printk("5:%s file_stat:0x%llx file_area:0x%llx state:0x%x is mapcount file_area\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);
			}

			//每扫描1个file_area，p_file_stat->last_index加PAGE_COUNT_IN_AREA
			p_file_stat->last_index += PAGE_COUNT_IN_AREA;

			//if成立说明整个文件的page都扫描完了
			if(p_file_stat->last_index >= file_page_count){
complete:				
				if(shrink_page_printk_open1)
					printk("6:%s file_stat:0x%llx %s all page scan complete p_file_stat->last_index:%ld file_page_count:%d\n",__func__,(u64)p_file_stat,p_file_stat->file_name,p_file_stat->last_index,file_page_count);

				//p_file_stat->traverse_done = 1;

				//对file_stat->last_index清0，后续有用于保存最近一次扫描的file_area的索引
				p_file_stat->last_index = 0;
				//在文件file_stat移动到temp链表时，p_file_stat->file_area_count_in_temp_list是文件的总file_area个数
				//p_file_stat->file_area_count_in_temp_list = p_file_stat->file_area_count;//上边已经加1了

				/*文件的page扫描完了，把file_stat从global mmap_file_stat_uninit_head链表移动到global mmap_file_stat_temp_head或
				 *mmap_file_stat_large_file_head。这个过程必须加锁，因为与add_mmap_file_stat_to_list()存在并发修改global mmap_file_stat_uninit_head
				 *链表的情况。后续file_stat再移动到大文件、zero_file_area等链表，就不用再加锁了，完全是异步内存回收线程的单线程操作*/
				spin_lock(&hot_cold_file_global_info.mmap_file_global_lock);

				/*新分配的file_stat必须设置in_file_stat_temp_head_list链表。这个设置file_stat状态的操作必须放到 把file_stat添加到
				 *tmep链表前边，还要加内存屏障。否则会出现一种极端情况，异步内存回收线程从temp链表遍历到这个file_stat，
				 *但是file_stat还没有设置为in_temp_list状态。这样有问题会触发panic。因为mmap文件异步内存回收线程，
				 *从temp链表遍历file_stat没有mmap_file_global_lock加锁，所以与这里存在并发操作。而针对cache文件，异步内存回收线程
				 *从global temp链表遍历file_stat，全程global_lock加锁，不会跟向global temp链表添加file_stat存在方法，但最好改造一下*/
				set_file_stat_in_file_stat_temp_head_list(p_file_stat);
				smp_wmb();

				if(is_mmap_file_stat_large_file(p_hot_cold_file_global,p_file_stat)){
					set_file_stat_in_large_file(p_file_stat);
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
				}
				else
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);

				spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
	
				/*如果文件file_stat的mapcount的file_area个数超过阀值，则file_stat被判定为mapcount file_stat而移动到
				 *global mmap_file_stat_mapcount_head链表。但前提file_stat必须在temp_file链表或temp_large_file链表*/
				if(is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat) && file_stat_in_file_stat_temp_head_list(p_file_stat)){
					if(file_stat_in_file_stat_temp_head_list_error(p_file_stat))
						panic("%s file_stat:0x%llx status error:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

					clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
					set_file_stat_in_mapcount_file_area_list(p_file_stat);
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_mapcount_head);
					p_hot_cold_file_global->mapcount_mmap_file_stat_count ++;
					if(shrink_page_printk_open)
						printk("6:%s file_stat:0x%llx status:0x%llx is mapcount file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
				}

				break;
			}
		}

		//如果扫描的文件页page数达到本次的限制，结束本次的scan
		if(scan_file_area_count >= scan_file_area_max){
			break;
		}
	}
out:
	return ret;
}
#endif
static noinline int scan_mmap_mapcount_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max)
{
	struct file_stat *p_file_stat,*p_file_stat_temp;
	unsigned int mapcount_file_area_count_origin;
	unsigned int scan_file_area_count = 0;
	unsigned int mapcount_to_temp_file_area_count_from_mapcount_file = 0;
	char file_stat_change = 0;
	LIST_HEAD(file_stat_list);

	//每次都从链表尾开始遍历
	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->mmap_file_stat_mapcount_head,hot_cold_file_list){
		if(!file_stat_in_mapcount_file_area_list(p_file_stat) || file_stat_in_mapcount_file_area_list_error(p_file_stat))
			panic("%s file_stat:0x%llx not in_mapcount_file_area_list status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		//遍历file_stat->file_area_mapcount上的file_area，如果file_area的page的mapcount都是1，file_area不再是mapcount file_area，则降级到temp_list
		if(!list_empty(&p_file_stat->file_area_mapcount)){
			mapcount_file_area_count_origin = p_file_stat->mapcount_file_area_count;
			file_stat_change = 0;

			scan_file_area_count += reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_mapcount,SCAN_MAPCOUNT_FILE_AREA_COUNT_ONCE,1 << F_file_area_in_mapcount_list,MMAP_FILE_AREA_MAPCOUNT_AGE_DX);

			if(mapcount_file_area_count_origin != p_file_stat->mapcount_file_area_count){
				//文件file_stat的mapcount的file_area个数减少到阀值以下了，降级到普通文件
				if(0 == is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat)){
					unsigned int file_stat_type;

					/* 只有file_stat从global mapcount链表移动回global temp链表，才得global mmap_file_global_lock加锁，
					 * 下边file_stat移动回global mapcount链表头不用加锁*/
					spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
					/*p_file_stat可能被iput并发标记delete并移动到global delete链表了，要加锁后防护这种情况*/
					if(!file_stat_in_delete(p_file_stat)){
						clear_file_stat_in_mapcount_file_area_list(p_file_stat);

						/*根据file_stat的file_area个数判断文件是普通文件、中型文件、大文件，并移动到匹配的global temp、middle、large链表*/
						file_stat_type = is_mmap_file_stat_file_type(p_hot_cold_file_global,p_file_stat);
						if(TEMP_FILE == file_stat_type){
							set_file_stat_in_file_stat_temp_head_list(p_file_stat);
							list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
						}else if(MIDDLE_FILE == file_stat_type){
							set_file_stat_in_file_stat_middle_file_head_list(p_file_stat);
							list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_middle_file_head);
						}
						else{
							set_file_stat_in_file_stat_large_file_head_list(p_file_stat);
							list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
						}

						spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
						p_hot_cold_file_global->mapcount_mmap_file_stat_count --;
						file_stat_change = 1;
						if(shrink_page_printk_open1)
							printk("1:%s file_stat:0x%llx status:0x%llx  mapcount to temp file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
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
		//超出扫描的file_area上限，break
		if(scan_file_area_count > scan_file_area_max){
			break;
		}
	}
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
	if(&p_file_stat->hot_cold_file_list != &p_hot_cold_file_global->mmap_file_stat_mapcount_head && !file_stat_in_delete(p_file_stat)){
	    if(can_file_stat_move_to_list_head(p_file_stat,F_file_stat_in_mapcount_file_area_list))
		    list_move_enhance(&p_hot_cold_file_global->mmap_file_stat_mapcount_head,&p_file_stat->hot_cold_file_list);
	}
	spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

	p_hot_cold_file_global->mmap_file_shrink_counter.mapcount_to_temp_file_area_count_from_mapcount_file += mapcount_to_temp_file_area_count_from_mapcount_file;
	return scan_file_area_count;
}
static noinline int scan_mmap_hot_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max)
{
	struct file_stat *p_file_stat,*p_file_stat_temp;
	unsigned int file_area_hot_count_origin;
	unsigned int scan_file_area_count = 0;
	unsigned int hot_to_temp_file_area_count_from_hot_file = 0;
	char file_stat_change = 0;
	LIST_HEAD(file_stat_list);


	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->mmap_file_stat_hot_head,hot_cold_file_list){
		if(!file_stat_in_file_stat_hot_head_list(p_file_stat) || file_stat_in_file_stat_hot_head_list_error(p_file_stat))
			panic("%s file_stat:0x%llx not in_file_stat_hot_head_list status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		//遍历file_stat->file_area_hot上的file_area，如果长时间不被访问了，则降级到temp_list
		if(!list_empty(&p_file_stat->file_area_hot)){
			file_area_hot_count_origin = p_file_stat->file_area_hot_count;

			scan_file_area_count += reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_hot,SCAN_HOT_FILE_AREA_COUNT_ONCE,1 << F_file_area_in_hot_list,MMAP_FILE_AREA_HOT_AGE_DX);

			if(file_area_hot_count_origin != p_file_stat->file_area_hot_count){
				//文件file_stat的mapcount的file_area个数减少到阀值以下了，降级到普通文件
				if(0 == is_mmap_file_stat_hot_file(p_hot_cold_file_global,p_file_stat)){
					unsigned int file_stat_type;

					spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
					/*p_file_stat可能被iput并发标记delete并移动到global delete链表了，要加锁后防护这种情况*/
					if(!file_stat_in_delete(p_file_stat)){
						clear_file_stat_in_file_stat_hot_head_list(p_file_stat);
						set_file_stat_in_file_stat_temp_head_list(p_file_stat);

						/*根据file_stat的file_area个数判断文件是普通文件、中型文件、大文件，并移动到匹配的global temp、middle、large链表*/
						file_stat_type = is_mmap_file_stat_file_type(p_hot_cold_file_global,p_file_stat);
						if(TEMP_FILE == file_stat_type){
							set_file_stat_in_file_stat_temp_head_list(p_file_stat);
							list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
						}else if(MIDDLE_FILE == file_stat_type){
							set_file_stat_in_file_stat_middle_file_head_list(p_file_stat);
							list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_middle_file_head);
						}
						else{
							set_file_stat_in_file_stat_large_file_head_list(p_file_stat);
							list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_large_file_head);
						}

						spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
						p_hot_cold_file_global->hot_mmap_file_stat_count --;
						hot_to_temp_file_area_count_from_hot_file ++;
						file_stat_change = 1;
						if(shrink_page_printk_open1)
							printk("1:%s file_stat:0x%llx status:0x%llx  hot to temp file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
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
		//超出扫描的file_area上限，break
		if(scan_file_area_count > scan_file_area_max){
			break;
		}
	}
#if 0
	//如果file_stat_list临时链表还有file_stat，则把这些file_stat移动到global mmap_file_stat_hot_head链表头，下轮循环就能从链表尾巴扫描还没有扫描的file_stat了
	if(!list_empty(&file_stat_list)){
		list_splice(&file_stat_list,&p_hot_cold_file_global->mmap_file_stat_hot_head);
	}
#endif	
	/*把本次在链表尾遍历过的file_stat移动到链表头，下次执行该函数从链表尾遍历到的是新的未遍历过的file_stat*/

	/* 本来以为这里只是global mapcount链表直接移动file_stat，不用mmap_file_global_lock加锁。真的不用加锁？
	 * 大错特错，如果p_file_stat此时正好被iput()释放，标记file_stat delete并移动到global delete链表，
	 *这里却把p_file_stat又移动到了global hot链表，那就出大问题了。因此，这里不用加锁防护global temp
	 *链表file_stat的增加与删除，但是要防护iput()把该file_stat并发移动到global delete链表。方法很简单，
	 *加锁后p_file_stat不是链表头，且没有delete标记即可。详细原因get_file_area_from_file_stat_list()有解释
	 !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
	spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
	if(&p_file_stat->hot_cold_file_list != &p_hot_cold_file_global->mmap_file_stat_hot_head && !file_stat_in_delete(p_file_stat)){
	    if(can_file_stat_move_to_list_head(p_file_stat,F_file_stat_in_file_stat_hot_head_list))
		    list_move_enhance(&p_hot_cold_file_global->mmap_file_stat_hot_head,&p_file_stat->hot_cold_file_list);
	}
	spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

	p_hot_cold_file_global->mmap_file_shrink_counter.hot_to_temp_file_area_count_from_hot_file += hot_to_temp_file_area_count_from_hot_file;
	return scan_file_area_count;
}
int walk_throuth_all_mmap_file_area(struct hot_cold_file_global *p_hot_cold_file_global)
{
	int ret;
	unsigned int scan_file_area_max,scan_file_stat_max;
	unsigned int del_file_area_count = 0,del_file_stat_count = 0,del_file_area_count_temp;
	struct file_stat *p_file_stat,*p_file_stat_temp;
	struct file_stat_small *p_file_stat_small,*p_file_stat_small_temp;
	struct file_stat_tiny_small *p_file_stat_tiny_small,*p_file_stat_tiny_small_temp;

	if(shrink_page_printk_open)
		printk("%s mmap_file_stat_count:%d mapcount_mmap_file_stat_count:%d hot_mmap_file_stat_count:%d\n",__func__,p_hot_cold_file_global->mmap_file_stat_count,p_hot_cold_file_global->mapcount_mmap_file_stat_count,p_hot_cold_file_global->hot_mmap_file_stat_count);

#if 0	
	//扫描global mmap_file_stat_uninit_head链表上的file_stat
	ret = scan_uninit_file_stat(p_hot_cold_file_global,&p_hot_cold_file_global->mmap_file_stat_uninit_head,512);
	if(ret < 0)
		return ret;
#endif


	//扫描大文件file_area
	scan_file_stat_max = 8;
	scan_file_area_max = 256;
	ret = get_file_area_from_mmap_file_stat_list(p_hot_cold_file_global,scan_file_area_max,scan_file_stat_max,&p_hot_cold_file_global->mmap_file_stat_large_file_head,F_file_stat_in_file_stat_large_file_head_list);
	if(ret < 0)
		return ret;

	//扫描中型文件file_area
	scan_file_stat_max = 16;
	scan_file_area_max = 128;
	ret = get_file_area_from_mmap_file_stat_list(p_hot_cold_file_global,scan_file_area_max,scan_file_stat_max,&p_hot_cold_file_global->mmap_file_stat_middle_file_head,F_file_stat_in_file_stat_middle_file_head_list);
	if(ret < 0)
		return ret;

	//扫描小文件file_area
	scan_file_stat_max = 32;
	scan_file_area_max = 128;
	ret = get_file_area_from_mmap_file_stat_list(p_hot_cold_file_global,scan_file_area_max,scan_file_stat_max,&p_hot_cold_file_global->mmap_file_stat_temp_head,F_file_stat_in_file_stat_temp_head_list);
	if(ret < 0)
		return ret;


	//扫描small文件file_area
	scan_file_stat_max = 32;
	scan_file_area_max = 128;
	ret = get_file_area_from_mmap_file_stat_small_list(p_hot_cold_file_global,scan_file_area_max,scan_file_stat_max,&p_hot_cold_file_global->mmap_file_stat_temp_head,F_file_stat_in_file_stat_small_file_head_list);
	if(ret < 0)
		return ret;

	//扫描极小文件file_area
	scan_file_stat_max = 32;
	scan_file_area_max = 128;
	ret = get_file_area_from_mmap_file_stat_tiny_small_list(p_hot_cold_file_global,scan_file_area_max,scan_file_stat_max,&p_hot_cold_file_global->mmap_file_stat_temp_head,F_file_stat_in_file_stat_tiny_small_file_head_list);
	if(ret < 0)
		return ret;


	scan_file_area_max = 32;
	//扫描热文件的file_area
	ret = scan_mmap_hot_file_stat(p_hot_cold_file_global,scan_file_area_max);
	if(ret < 0)
		return ret;

	scan_file_area_max = 32;
	//扫描mapcount文件的file_area
	ret = scan_mmap_mapcount_file_stat(p_hot_cold_file_global,scan_file_area_max);

	/*遍历global file_stat_delete_head链表上已经被删除的文件的file_stat，
	  一次不能删除太多的file_stat对应的file_area，会长时间占有cpu，后期需要调优一下*/
	del_file_area_count_temp = 0;
	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->mmap_file_stat_delete_head,hot_cold_file_list){
		if(!file_stat_in_delete(p_file_stat) /*|| file_stat_in_delete_error(p_file_stat)*/)
			panic("%s file_stat:0x%llx not delete status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,&p_file_stat->file_stat_base,FILE_STAT_NORMAL);
		del_file_stat_count ++;
		/*防止耗时太长*/
		if(++ del_file_area_count_temp > 256)
			break;
	}
    
	del_file_area_count_temp = 0;
	list_for_each_entry_safe_reverse(p_file_stat_small,p_file_stat_small_temp,&p_hot_cold_file_global->mmap_file_stat_small_delete_head,hot_cold_file_list){
		if(!file_stat_in_delete_base(&p_file_stat_small->file_stat_base) /*|| file_stat_in_delete_error(p_file_stat)*/)
			panic("%s file_stat:0x%llx not delete status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,&p_file_stat_small->file_stat_base,FILE_STAT_SMALL);
		del_file_stat_count ++;
		/*防止耗时太长*/
		if(++ del_file_area_count_temp > 256)
			break;
	}

	del_file_area_count_temp = 0;
	list_for_each_entry_safe_reverse(p_file_stat_tiny_small,p_file_stat_tiny_small_temp,&p_hot_cold_file_global->mmap_file_stat_tiny_small_delete_head,hot_cold_file_list){
		if(!file_stat_in_delete_base(&p_file_stat_tiny_small->file_stat_base) /*|| file_stat_in_delete_error(p_file_stat)*/)
			panic("%s file_stat:0x%llx not delete status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,&p_file_stat_tiny_small->file_stat_base,FILE_STAT_TINY_SMALL);
		del_file_stat_count ++;
		/*防止耗时太长*/
		if(++ del_file_area_count_temp > 256)
			break;
	}


	/*从系统启动到现在释放的mmap page数*/
    p_hot_cold_file_global->free_mmap_pages += p_hot_cold_file_global->mmap_file_shrink_counter.mmap_free_pages_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.del_file_area_count += del_file_area_count;
	p_hot_cold_file_global->mmap_file_shrink_counter.del_file_stat_count += del_file_stat_count;

	return ret;
}
EXPORT_SYMBOL(walk_throuth_all_mmap_file_area);
