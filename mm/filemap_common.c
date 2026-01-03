#include <linux/export.h>
#include <linux/compiler.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/error-injection.h>
#include <linux/hash.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/pagevec.h>
#include <linux/security.h>
#include <linux/cpuset.h>
#include <linux/hugetlb.h>
#include <linux/memcontrol.h>
#include <linux/shmem_fs.h>
#include <linux/rmap.h>
#include <linux/delayacct.h>
#include <linux/psi.h>
#include <linux/ramfs.h>
#include <linux/page_idle.h>
#include <linux/migrate.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include "internal.h"
#include "async_memory_reclaim_for_cold_file_area.h"
#include <linux/version.h>

//#define CREATE_TRACE_POINTS
//#include <trace/events/filemap.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,1,0)

#else
/*
/// mm/filemap.c
1:page_cache_delete_for_file_area
2:page_cache_delete_batch_for_file_area
3:filemap_range_has_page_for_file_area
4:filemap_range_has_writeback_for_file_area
5:replace_page_cache_page_for_file_area
6:__filemap_add_folio_for_file_area
7:page_cache_next_miss_for_file_area
8:page_cache_prev_miss_for_file_area
9:mapping_get_entry_for_file_area
10:get_folio_from_file_area_for_file_area
11:find_get_entry_for_file_area
12:find_get_entries_for_file_area
13:find_lock_entries_for_file_area
14:find_get_pages_range_for_file_area
15:find_get_pages_contig_for_file_area
16:filemap_get_read_batch_for_file_area
17:mapping_seek_hole_data_for_file_area
18:next_uptodate_page_for_file_area
19:first_map_page_for_file_area
20:next_map_page_for_file_area
21:filemap_map_pages_for_file_area
*/
void page_cache_delete_for_file_area(struct address_space *mapping,
		struct folio *folio, void *shadow)
{
	//XA_STATE(xas, &mapping->i_pages, folio->index);
	XA_STATE(xas, &mapping->i_pages, folio->index >>PAGE_COUNT_IN_AREA_SHIFT);
	long nr = 1;
	struct file_area *p_file_area; 
	struct file_stat_base *p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;

	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = folio->index & PAGE_COUNT_IN_AREA_MASK;
#if 0
	mapping_set_update(&xas, mapping);//xarray shadow 的处理，先不管
#endif
	/* hugetlb pages are represented by a single entry in the xarray */
	if (!folio_test_hugetlb(folio)) {
		if(folio_nr_pages(folio) > 1){
			panic("%s folio_nr_pages:%ld\n",__func__,folio_nr_pages(folio));
		}
#if 0		
		xas_set_order(&xas, folio->index, folio_order(folio));
		nr = folio_nr_pages(folio);
#endif		
	}

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	p_file_area = xas_load(&xas);
	if(!p_file_area || !is_file_area_entry(p_file_area))
		panic("%s mapping:0x%llx folio:0x%llx file_area:0x%llx\n",__func__,(u64)mapping,(u64)folio,(u64)p_file_area);

	p_file_area = entry_to_file_area(p_file_area);
	//if(folio != p_file_area->pages[page_offset_in_file_area]){
	if(folio != rcu_dereference(p_file_area->pages[page_offset_in_file_area])){
		panic("%s mapping:0x%llx folio:0x%llx != p_file_area->pages:0x%llx\n",__func__,(u64)mapping,(u64)folio,(u64)p_file_area->pages[page_offset_in_file_area]);
	}
	/* 清理这个page在file_area->file_area_statue的对应的bit位，表示这个page被释放了。但是要放在p_file_area->pages[page_offset_in_file_area]
	 * 清NULL之前，还要内存屏障smp_wmb()隔开。目的是，读写文件的进程，从xarray tree遍历到该page时，如果此时并发有进程执行该函数删除page，
	 * 如果看到p_file_area->pages[page_offset_in_file_area]是NULL，此时page在file_area->file_area_statue的对应的bit位一定被清0了。
	 * 不对，这个并发分析的有问题。举例分析就清楚了，如果读写文件的这里正执行smp_wmb时，读写文件的进程从xarray tree得到了该page，
	 * 还不是NULL，但是此时page在file_area->file_area_statue的对应的bit位已经清0了，那读写文件的进程，如在mapping_get_entry_for_file_area
	 * 函数里，发现page存在，但是page在file_area->file_area_statue的对应的bit位缺是0，那就要crash了。故这个方案有问题，要把
	 * page在file_area->file_area_statue的对应的bit位放到p_file_area->pages[page_offset_in_file_area] = NULL赋值NULL后边。如此，
	 * 读写文件的进程mapping_get_entry_for_file_area函数里，看到page在file_area->file_area_statue的对应的bit位是0，
	 * p_file_area->pages[page_offset_in_file_area]里保存的page一定是NULL。并且，读写文件进程看到p_file_area->pages[page_offset_in_file_area]
	 * 里保存的page是NULL，就不会再判断page在file_area->file_area_statue的对应的bit位是否是1了*/
	//clear_file_area_page_bit(p_file_area,page_offset_in_file_area);
	//smp_wmb();
#ifndef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY
#ifndef FILE_AREA_IN_FREE_KSWAPD_AND_SHADOW
	/*1:kswapd进程内存回收 2:进程直接内存回收 shadow非NULL。 3:shadow是NULL，但current->mm非NULL，说明是文件截断
	 * 剩下的就是异步内存回收线程，该if才成立*/
	if(shadow)
		hot_cold_file_global_info.kswapd_free_page_count ++;
	else if(!current->mm){
		/*在file_area->file_area_state里标记file_area的这个page被释放了*/
		//set_file_area_page_shadow_bit(p_file_area,page_offset_in_file_area);
		hot_cold_file_global_info.async_thread_free_page_count ++;

		/* 最新方案，异步内存回收page时，file_area_state的shadow bit不再使用。而是把1赋值给file_area->pages[]。这样bit0是1
		 * 说明是个shadow entry，但是kswapd内存回收的的shadow又远远大于1，依此可以区分该page是被异步内存回收还是kswapd。*/
		shadow = (void *)(0x1);
		if(strncmp(current->comm,"async_memory",12))
			printk("%s %s %d async memory errror!!!!!!!!!\n",__func__,current->comm,current->pid);
	}
	rcu_assign_pointer(p_file_area->pages[page_offset_in_file_area], shadow);
#else
	/* 如果是异步内存回收线程回收的page，传入的shadow是NULL，此时是否有必要向file_area->pages[]写入一个非0值呢？
	 * 表示该page被回收了，将来被访问则判定为refault。完全可以我直接执行shadow = workingset_eviction(folio, target_memcg)
	 * 计算shadow值呀，先不搞了，异步内存回收线程回收page 跟 kswapd内存回收page，完全是两码事*/
	if(shadow)/*1:kswapd进程内存回收 2:进程直接内存回收*/
		/*bit1清0表示是kswapd内存回收的该page。将来该page将来再被访问发生refault，在workingset_refault()函数要依照shadow值
		  决定是否把该page移动到active lru链表。但是我这里将shadow的bit1清0了，破坏了原生shadow值，有问题????????但只是bit1，影响不大吧??????????????????*/
		//shadow = shadow & ~(1 << 1);
	    test_and_clear_bit(1,(unsigned long *)(&shadow));
	else{
		/*1:用户态进程截断文件，赋值NULL。
		 *2:异步内存回收线程，bit1置1 。不行，bit0也必须置1。否则将会逃过folio_is_file_area_index_or_shadow的判断，被判定是folio指针，而crash*/
		if(!current->mm){
			shadow = (void *)(0x3);
			if(strncmp(current->comm,"async_memory",12))
				printk("%s %s %d async memory errror!!!!!!!!!\n",__func__,current->comm,current->pid);
		}
		else
			shadow = NULL;
	}
	rcu_assign_pointer(p_file_area->pages[page_offset_in_file_area], shadow);
#endif

#else
	//if(shadow || strncmp(current->comm,"hot_cold",8) == 0)
	if(shadow)
		rcu_assign_pointer(p_file_area->pages[page_offset_in_file_area], file_area_shadow_bit_set);
	else
		rcu_assign_pointer(p_file_area->pages[page_offset_in_file_area], NULL);
#endif

	FILE_AREA_PRINT1("%s mapping:0x%llx folio:0x%llx index:%ld p_file_area:0x%llx page_offset_in_file_area:%d\n",__func__,(u64)mapping,(u64)folio,folio->index,(u64)p_file_area,page_offset_in_file_area);

	folio->mapping = NULL;
	mapping->nrpages -= nr;

	/*是调试的文件，打印调试信息*/
	if(mapping->rh_reserved3){
		printk("%s delete mapping:0x%llx file_stat:0x%llx file_area:0x%llx status:0x%x page_offset_in_file_area:%d folio:0x%llx flags:0x%lx\n",__func__,(u64)mapping,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,(u64)folio,folio->flags);
	}
    
	smp_wmb();
	//清理这个page在file_area->file_area_statue的对应的bit位，表示这个page被释放了
	clear_file_area_page_bit(p_file_area,page_offset_in_file_area);
	/* 如果page在xarray tree有dirty、writeback、towrite mark标记，必须清理掉，否则将来这个槽位的再有新的page，
	 * 这些mark标记会影响已经设置了dirty、writeback、towrite mark标记的错觉，从而导致判断错误*/
	if(is_file_area_page_mark_bit_set(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_DIRTY))
		clear_file_area_page_mark_bit(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_DIRTY);
	if(is_file_area_page_mark_bit_set(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_WRITEBACK))
		clear_file_area_page_mark_bit(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_WRITEBACK);
	if(is_file_area_page_mark_bit_set(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_TOWRITE))
		clear_file_area_page_mark_bit(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_TOWRITE);

	//如果这个file_area还有page，直接返回。否则才会xas_store(&xas, NULL)清空这个file_area
	if(file_area_have_page(p_file_area))
		return;

#ifdef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY
	/*file_stat tiny模式，为了节省内存把file_area->start_index成员删掉了。但是在file_area的page全释放后，
	 *会把file_area的索引(file_area->start_index >> PAGE_COUNT_IN_AREA_SHIFT)保存到p_file_area->pages[0/1]里.
	 *将来cold_file_area_delete将是从p_file_area->pages[0/1]获取file_area的索引*/
	//p_file_area->pages[0] = (struct folio *)(xas.xa_index >> 32);
	//p_file_area->pages[1] = (struct folio *)(xas.xa_index & ((1UL << 32) - 1));

	/* p_file_area->pages[0]可能还保存了shadow bit，因此不能直接给p_file_area->pages[0]赋值，要或上老的值
	 * 并且最新的方案，file_area_index成立，要bit62置1，因此，还要或上file_area_index_bit_set，令bit62置1*/
	p_file_area->pages[0] =   (struct folio *)(((xas.xa_index >> 32) | file_area_index_bit_set) | (u64)p_file_area->pages[0]);
	p_file_area->pages[1] =  (struct folio *)(((xas.xa_index & ((1UL << 32) - 1)) | file_area_index_bit_set) | (u64)p_file_area->pages[1]);
#endif	

#ifdef ASYNC_MEMORY_RECLAIM_DEBUG	
	/*如果待删除的page所属file_area的父节点是cache node，则清理掉cache node。还必须把p_file_stat->xa_node_cache_base_index成
	 * 64位最大数。确保 find_file_area_from_xarray_cache_node()里的if((index >= p_file_stat->xa_node_cache_base_index) 一定不
	 * 成立。并且p_file_stat->xa_node_cache = NULL要放到p_file_stat->xa_node_cache_base_index = -1后边，这样 
	 * find_file_area_from_xarray_cache_node()里if(p_file_stat->xa_node_cache)看到p_file_stat->xa_node_cache是NULL时，
	 * if((index >= p_file_stat->xa_node_cache_base_index) 看到的p_file_stat->xa_node_cache_base_index一定是-1。并且
	 * 这二者的赋值一定要放到xas_store(&xas, NULL)前边。xas_store(&xas, NULL)会释放掉xarray tree的node节点，也就是
	 * p_file_stat->xa_node_cache指向的node节点。此时mapping_get_entry/filemap_get_read_batch 如果再访问p_file_stat->xa_node_cache
	 * 指向的node节点，就会非法内存访问而crash。由此需要这里p_file_stat->xa_node_cache = NULL后，此时其他cpu上跑的进程执行
	 * mapping_get_entry/filemap_get_read_batch必须立即看到p_file_stat->xa_node_cache是NULL了。这就用到rcu机制。xas_store(&xas, NULL)
	 * 里本身有smp_mb()内存屏障，保证p_file_stat->xa_node_cache = NULL后立即给其他cpu发无效消息。而xas_store()删除node节点本质是把node
	 * 添加到rcu异步队列，等rcu宽限期过了才会真正删除node结构。此时正在mapping_get_entry/filemap_get_read_batch访问
	 * p_file_stat->xa_node_cache的进程，不用担心，因为rcu宽限期还没过。等新的进程再执行这两个函数，再去访问p_file_stat->xa_node_cache，
	 * 此时要先执行smp_rmb()从无效队列获取最新的p_file_stat->xa_node_cache_base_index和p_file_stat->xa_node_cache，总能感知到一个无效，
	 * 然后就不会访问p_file_stat->xa_node_cache指向的node节点了*/
	if(p_file_stat_base->xa_node_cache == xas.xa_node){
		//p_file_stat->xa_node_cache_base_index = -1;
		//p_file_stat->xa_node_cache = NULL;
		p_file_stat_base->xa_node_cache_base_index = -1;
		p_file_stat_base->xa_node_cache = NULL;
	}
#endif	

	//xas_store(&xas, shadow);不再使用shadow机制
	/*这里有个隐藏很深的坑?????????在file_area的page都释放后，file_area还要一直停留在xarray tree。因为后续如果file_area的page又被
	 *访问了，而对应xarray tree的槽位已经有file_area，依据这点判定发生了refault，file_area是refault file_area，后续长时间不再回收
	 file_area的page。故正常情况file_area的page全被释放了但file_area不能从xarray tree剔除。只有下边两种情况才行
     1:file_area的page被释放后，过了很长时间，file_area的page依然没人访问，则异步内存回收线程释放掉file_area结构，并把file_area从xarray tree剔除
     2:该文件被iput()释放inode了，mapping_exiting(maping)成立，此时执行到该函数也要把没有page的file_area从xarray tree剔除
    */
	if(mapping_exiting(mapping)){
		void *old_entry = xas_store(&xas, NULL);

		/* 可能存在一个并发，kswapd执行page_cache_delete_for_file_area()释放page，业务进程iput()释放文件执行 find_get_entries_for_file_area()
		 * 二者都会xas_store(&xas, NULL)把file_area从xarray tree剔除，但是只有成功把file_area从xarry tree剔除，返回值old_entry
		 * 非NULL，才能把file_area移动到global_file_stat.file_area_delete_list链表，依次防护重复把file_area移动到file_area_delete_list链表*/
		if(/*p_file_area->mapping && old_entry && */file_stat_in_global_base((struct file_stat_base *)mapping->rh_reserved1)){
			/*该函数全程xas_lock加锁，并且最开头可以从xarray tree查找到file_area，这里不可能从xarray tree查不到file_area*/
			if(!old_entry)
				panic("%s mapping:0x%llx p_file_area:0x%llx file_area_state:0x%x error old_entry NULL\n",__func__,(u64)mapping,(u64)p_file_area,p_file_area->file_area_state);
			/*可能并发iput()执行find_get_entry_for_file_area()把file_area->mapping置NULL了，这个没有xas_lock加锁防护*/
		    if(p_file_area->mapping){
				/*p_file_area->mapping置NULL，表示该文件iput了，马上要释放inode和mapping了*/
				WRITE_ONCE(p_file_area->mapping, 0);
				/*这个写屏障保证异步内存回收线程cold_file_area_delete()函数里，立即看到file_area->mapping是NULL*/
				smp_wmb();
				//文件iput了，此时file_area一个page都没有，于是把file_area移动到global_file_stat.file_area_delete_list链表
#if 0
				move_file_area_to_global_delete_list((struct file_stat_base *)mapping->rh_reserved1,p_file_area);
#else
				set_file_area_in_mapping_delete(p_file_area);
#endif
			}else{
				printk("%s file_area:0x%llx mapping NULL\n",__func__,(u64)p_file_area);
			}
		}
	}

	/*清理xarray tree的dirty、writeback、towrite标记，重点!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
	xas_init_marks(&xas);
	/*清理file_area所有的towrite、dirty、writeback的mark标记。这个函数是在把file_area从xarray tree剔除时执行的。
	 *之后file_area是无效的，有必要吗????????????。有必要，但是要把清理page的dirty、writeback、towrite mark标记代码放到
	 上边。做到每delete一个page，就要清理这个page的dirty、writeback、towrite mark标记，而不能要等到delete 4个page后再同一清理4个page的mark标记*/
	//clear_file_area_towrite_dirty_writeback_mark(p_file_area);

	//folio->mapping = NULL;必须放到前边，这个隐藏的问题很深呀

	/* Leave page->index set: truncation lookup relies upon it */
	//mapping->nrpages -= nr; 这个也要放到前边，page失效就要立即 mapping->nrpages -= nr，否则执行不了这里

	/* 如果不是被异步内存回收线程回收的page的file_area，就没有in_free标记。这样后续该file_area的page再被访问，
	 * 就无法被判定为refault page了。于是这里强制标记file_area的in_refault标记，并把file_area移动到in_free链表等等。
	 * no，这里不能把file_area移动到in_free链表，这会跟异步内存回收线程遍历in_free链表上的file_area起冲突，
	 * 异步内存回收线程遍历in_free链表的file_area，是没有加锁的。要么修改异步内存回收线程的代码，历in_free
	 * 链表的file_area加锁，要么这里只是标记file_area的in_free标记，但不把file_area移动到in_free链表。我
	 * 目前的代码设计，只有file_stat->temp链表上的file_area才spin_lock加锁，其他file_stat->链表上file_area
	 * 遍历和移动都不加锁，遵循历史设计吧。最终决策，这里标记file_stat的in_free_kswaped标记，异步内存回收线程
	 * 针对有in_free_kswaped标记的file_area，特殊处理*/

#ifdef FILE_AREA_IN_FREE_KSWAPD_AND_SHADOW
	/*可能一个file_area被异步内存回收线程回收标记in_free后，然后 kswapd再回收它里边的新读写产生的page，此时就不用再标记file_area in_free_kswaped了*/
	if(/*shadow && !file_area_in_free_list(p_file_area) &&*/ !file_area_in_free_kswapd(p_file_area) && shadow){
		set_file_area_in_free_kswapd(p_file_area);
		hot_cold_file_global_info.kswapd_free_page_count ++;
	}else if(file_area_in_free_list(p_file_area))
		hot_cold_file_global_info.async_thread_free_page_count ++;
#endif	
}
EXPORT_SYMBOL(page_cache_delete_for_file_area);
void page_cache_delete_batch_for_file_area(struct address_space *mapping,
		struct folio_batch *fbatch)
{
	//XA_STATE(xas, &mapping->i_pages, fbatch->folios[0]->index);
	XA_STATE(xas, &mapping->i_pages, fbatch->folios[0]->index >> PAGE_COUNT_IN_AREA_SHIFT);
	long total_pages = 0;
	int i = 0;
	struct folio *folio;
	struct file_area *p_file_area;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = fbatch->folios[0]->index & PAGE_COUNT_IN_AREA_MASK;
	struct file_stat_base *p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;

	//mapping_set_update(&xas, mapping); 不需要设置shadow operation

	/*查找start_byte~end_byte地址范围内的有效page并返回，一直查找max索引的page结束。因为，xas_for_each()里调用的
	 *xas_find()和xas_next_entry()都是以xas->xa_offset为起始索引从xarray tree查找page，找不到则xas->xa_offset加1继续查找，
	 直到查找第一个有效的page。或者xas->xa_offset大于max还是没有找到有效page，则返回NULL*/
	//xas_for_each(&xas, folio, ULONG_MAX) {
	xas_for_each(&xas, p_file_area, ULONG_MAX) {
		if(!p_file_area || !is_file_area_entry(p_file_area))
			panic("%s mapping:0x%llx p_file_area:0x%llx NULL\n",__func__,(u64)mapping,(u64)p_file_area);
		p_file_area = entry_to_file_area(p_file_area);

find_page_from_file_area:
		//folio = p_file_area->pages[page_offset_in_file_area];
		folio = rcu_dereference(p_file_area->pages[page_offset_in_file_area]);
		/*如果fiolio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
		folio_is_file_area_index_or_shadow_and_clear_NULL(folio);

		/*page_cache_delete_batch()函数能进到这里folio一定不是NULL，但是现在无法保证，需要额外判定。但不能break，而是要去查找
		 *file_area里的下一个page。因为 xas_for_each()、xas_find()等函数现在从xarray tree查找的是file_area，不是page。只有
		 *找到的file_area是NULL，才能break结束查找。错了，原page_cache_delete_batch()函数for循环退出条件就有folio是NULL.
		 *又错了，xas_for_each(&xas, folio, ULONG_MAX)里如果找到NULL page就一直向后查找，不会终止循环。直到要查找的page索引
		 *大于max才会终止循环。file_area精简后，可能file_area的一个folio被释放了，变成了file_area的索引，现在连续释放该
		 *file_area的所有page，是可能遇到folio是file_area索引的*/
		if(!folio){
			goto next_page;
			//break
		}
		if(!file_area_have_page(p_file_area))
			panic("%s file_area:0x%llx folio:0x%llx page_offset_in_file_area:%d mapping:0x%llx_0x%llx file_area_have_page error\n",__func__,(u64)p_file_area,(u64)folio,page_offset_in_file_area,(u64)mapping,(u64)((folio)->mapping));

		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,mapping,folio,p_file_area,page_offset_in_file_area,((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area));

		if (i >= folio_batch_count(fbatch))
			break;

		/* A swap/dax/shadow entry got inserted? Skip it. */
		//if (xa_is_value(folio)){
		if (xa_is_value(folio) || folio_nr_pages(folio) > 1){
			panic("%s folio:0x%llx xa_is_value folio_nr_pages:%ld\n",__func__,(u64)folio,folio_nr_pages(folio));
			//continue;
		}
		/*
		 * A page got inserted in our range? Skip it. We have our
		 * pages locked so they are protected from being removed.
		 * If we see a page whose index is higher than ours, it
		 * means our page has been removed, which shouldn't be
		 * possible because we're holding the PageLock.
		 */
		if (folio != fbatch->folios[i]) {
			VM_BUG_ON_FOLIO(folio->index >
					fbatch->folios[i]->index, folio);
			/*原来folio的xarray tree，现在要被其他进程保存了新的folio，要么folio被释放了。这里不能continue了，
			 *直接continue就是查询下一个file_area了，正确是goto next_page 查询下一个page。不能再执行
			 *clear_file_area_page_bit()和xas_store(&xas, NULL)，因为此时可能新的folio已经保存到了这个槽位*/
			//continue;

			//这里主要是检查新的folio 和 它在file_area->file_area_state 中的是否设置了bit位，如果状态错了，panic
			if(folio && !is_file_area_page_bit_set(p_file_area,page_offset_in_file_area))
				panic("%s mapping:0x%llx p_file_area:0x%llx file_area_state:0x%x error\n",__func__,(u64)mapping,(u64)p_file_area,p_file_area->file_area_state);

			goto next_page;
		}

		WARN_ON_ONCE(!folio_test_locked(folio));

		folio->mapping = NULL;
		/* Leave folio->index set: truncation lookup relies on it */

		i++;
		//xas_store(&xas, NULL);
		//p_file_area->pages[page_offset_in_file_area] = NULL;
		rcu_assign_pointer(p_file_area->pages[page_offset_in_file_area], NULL);
		total_pages += folio_nr_pages(folio);
#ifdef ASYNC_MEMORY_RECLAIM_DEBUG
		/*page_cache_delete_for_file_area函数有详细说明*/
		if(p_file_stat_base->xa_node_cache == xas.xa_node){
			p_file_stat_base->xa_node_cache_base_index = -1;
			p_file_stat_base->xa_node_cache = NULL;
		}
#endif		
		FILE_AREA_PRINT1("%s mapping:0x%llx folio:0x%llx index:%ld p_file_area:0x%llx page_offset_in_file_area:%d\n",__func__,(u64)mapping,(u64)folio,folio->index,(u64)p_file_area,page_offset_in_file_area);

		smp_wmb();
		/*清理这个page在file_area->file_area_statue的对应的bit位，表示这个page被释放了*/
		clear_file_area_page_bit(p_file_area,page_offset_in_file_area);
		/* 如果page在xarray tree有dirty、writeback、towrite mark标记，必须清理掉，否则将来这个槽位的再有新的page，
		 * 这些mark标记会影响已经设置了dirty、writeback、towrite mark标记的错觉，从而导致判断错误*/
		if(is_file_area_page_mark_bit_set(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_DIRTY))
			clear_file_area_page_mark_bit(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_DIRTY);
		if(is_file_area_page_mark_bit_set(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_WRITEBACK))
			clear_file_area_page_mark_bit(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_WRITEBACK);
		if(is_file_area_page_mark_bit_set(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_TOWRITE))
			clear_file_area_page_mark_bit(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_TOWRITE);

		/*只有这个file_area没有page了，才会xas_store(&xas, NULL)清空这个file_area。这种情况完全是可能存在的，比如
		 *一个file_area有page0、page1、page2、page3，现在从page1开始 delete，并没有从page0，那当delete 到page3时，
		 *是不能xas_store(&xas, NULL)把file_area清空的
        
		 正常情况file_area没有page不能直接从xarray tree剔除。只有file_area的page被释放后长时间依然没人访问才能由异
		 步内存回收线程把file_area从xarray tree剔除 或者 文件iput释放结构时mapping_exiting(mapping)成立，执行到该函数，
		 才能把file_area从xarray tree剔除
        */
		//if(!file_area_have_page(p_file_area) && mapping_exiting(mapping))
		//	xas_store(&xas, NULL);
		if(!file_area_have_page(p_file_area)){
			if(mapping_exiting(mapping)){
				/*page_cache_delete_batch_for_file_area()函数会循环遍历多个file_area。为了不干扰原生的xas，重新定义一个xas_del
				 *page_cache_delete_for_file_area不需要这样*/
				XA_STATE(xas_del, &mapping->i_pages, p_file_area->start_index); 
				void *old_entry = xas_store(&xas_del, NULL);

				/* 可能存在一个并发，kswapd执行page_cache_delete_for_file_area()释放page，业务进程iput()释放文件执行 find_get_entries_for_file_area()
				 * 二者都会xas_store(&xas, NULL)把file_area从xarray tree剔除，但是只有成功把file_area从xarry tree剔除，返回值old_entry
				 * 非NULL，才能把file_area移动到global_file_stat.file_area_delete_list链表，依次防护重复把file_area移动到file_area_delete_list链表*/
				if(/*p_file_area->mapping && old_entry && */file_stat_in_global_base((struct file_stat_base *)mapping->rh_reserved1)){
					/*该函数全程xas_lock加锁，并且最开头可以从xarray tree查找到file_area，这里不可能从xarray tree查不到file_area*/
					if(!old_entry)
						panic("%s mapping:0x%llx p_file_area:0x%llx file_area_state:0x%x error old_entry NULL\n",__func__,(u64)mapping,(u64)p_file_area,p_file_area->file_area_state);
					/*可能并发iput()执行find_get_entry_for_file_area()把file_area->mapping置NULL了，这个没有xas_lock加锁防护*/
		            if(p_file_area->mapping){
						/*p_file_area->mapping置NULL，表示该文件iput了，马上要释放inode和mapping了*/
						WRITE_ONCE(p_file_area->mapping, 0);
						/*这个写屏障保证异步内存回收线程cold_file_area_delete()函数里，立即看到file_area->mapping是NULL*/
						smp_wmb();
						/*文件iput了，此时file_area一个page都没有，于是把file_area移动到global_file_stat.file_area_delete_list链表*/
#if 0	
						move_file_area_to_global_delete_list((struct file_stat_base *)mapping->rh_reserved1,p_file_area);
#else
						set_file_area_in_mapping_delete(p_file_area);
#endif
					}else{
						printk("%s file_area:0x%llx mapping NULL\n",__func__,(u64)p_file_area);
					}
				}
			}

#ifdef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY
			/*file_stat tiny模式，为了节省内存把file_area->start_index成员删掉了。但是在file_area的page全释放后，
			 *会把file_area的索引(file_area->start_index >> PAGE_COUNT_IN_AREA_SHIFT)保存到p_file_area->pages[0/1]里.
			 *将来cold_file_area_delete将是从p_file_area->pages[0/1]获取file_area的索引*/
			//p_file_area->pages[0] = (struct folio *)(xas.xa_index >> 32);
			//p_file_area->pages[1] = (struct folio *)(xas.xa_index & ((1UL << 32) - 1));

			/* p_file_area->pages[0]可能还保存了shadow bit，因此不能直接给p_file_area->pages[0]赋值，要或上老的值
			 * 并且最新的方案，file_area_index成立，要bit62置1，因此，还要或上file_area_index_bit_set，令bit62置1*/
			p_file_area->pages[0] =   (struct folio *)(((xas.xa_index >> 32) | file_area_index_bit_set) | (u64)p_file_area->pages[0]);
			p_file_area->pages[1] =  (struct folio *)(((xas.xa_index & ((1UL << 32) - 1)) | file_area_index_bit_set) | (u64)p_file_area->pages[1]);
#endif	
		}

		/*是调试的文件，打印调试信息*/
		if(mapping->rh_reserved3){
			printk("%s delete_batch mapping:0x%llx file_stat:0x%llx file_area:0x%llx status:0x%x page_offset_in_file_area:%d folio:0x%llx flags:0x%lx\n",__func__,(u64)mapping,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,(u64)folio,folio->flags);
		}

next_page:
		page_offset_in_file_area ++;

		/*如果file_area里还有page没遍历到，goto find_page_from_file_area去查找file_area里的下一个page。否则到for循环开头
		 *xas_for_each()去查找下一个file_area，此时需要find_page_from_file_area清0，这个很关键*/
		if(page_offset_in_file_area < PAGE_COUNT_IN_AREA && file_area_have_page(p_file_area)){
			/*page_offset_in_file_area加1不能放到这里，重大逻辑错误。比如，上边判断page_offset_in_file_area是3的folio，
			 *然后执行到f(page_offset_in_file_area < PAGE_COUNT_IN_AREA)判断时，正常就应该不成立的，因为file_area的最后一个folio已经遍历过了*/
			//page_offset_in_file_area ++;
			goto find_page_from_file_area;
		}
		else
			page_offset_in_file_area = 0;
	}
	mapping->nrpages -= total_pages;
}
EXPORT_SYMBOL(page_cache_delete_batch_for_file_area);
bool filemap_range_has_page_for_file_area(struct address_space *mapping,
		loff_t start_byte, loff_t end_byte)
{
	//struct page *page;
	//XA_STATE(xas, &mapping->i_pages, start_byte >> PAGE_SHIFT);
	XA_STATE(xas, &mapping->i_pages, (start_byte >> PAGE_SHIFT) >> PAGE_COUNT_IN_AREA_SHIFT);

	//要查找的最后一个page
	//pgoff_t max = end_byte >> PAGE_SHIFT;
	/*要查找的最后一个file_area的索引，有余数要加1。错了，不用加1，因为file_area的索引是从0开始*/
	pgoff_t max = (end_byte >> PAGE_SHIFT) >> PAGE_COUNT_IN_AREA_SHIFT;
	struct file_area* p_file_area;
	struct file_stat_base* p_file_stat_base;
#if 0	
	/*要查找的最后一个page在file_area里的偏移*/
	pgoff_t max_page_offset_in_file_area = (end_byte >> PAGE_SHIFT) & PAGE_COUNT_IN_AREA_MASK;
	/*要查找的第一个page在file_area里的偏移*/
	pgoff_t start_page_offset_in_file_area = (start_byte >> PAGE_SHIFT) & PAGE_COUNT_IN_AREA_MASK;
	//要查找的第一个page在file_area->pages[]数组里的偏移，令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = (start_byte >> PAGE_SHIFT) & PAGE_COUNT_IN_AREA_MASK;
#endif

	if (end_byte < start_byte)
		return false;

	rcu_read_lock();
	
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	for (;;) {
		//page = xas_find(&xas, max);
		/*查找start_byte~end_byte地址范围内第一个有效的page对应的file_area，找不到返回NULL,然后下边return NULL。
		 *xas_find()会令xa.xa_offset自动加1*/
		p_file_area = xas_find(&xas, max);/*这里的max是要查询的最大file_area的索引，不是最大的page索引，很关键!!!!!!!!!*/
		//if (xas_retry(&xas, page))
		if (xas_retry(&xas, p_file_area))
			continue;

		/* Shadow entries don't count */
		//if (xa_is_value(page)){
		if (xa_is_value(p_file_area) || !is_file_area_entry(p_file_area)){
			panic("%s p_file_area:0x%llx xa_is_value\n",__func__,(u64)p_file_area);
			//continue;
		}
		if(!p_file_area)
			break;

		p_file_area = entry_to_file_area(p_file_area);
		/*重点，隐藏很深的问题，如果遇到有效的file_area但却没有page，那只能硬着头皮一直向后查找，直至找到max。
		 *这种情况是完全存在的，file_area的page全被回收了，但是file_area还残留着，file_area存在并不代表page存在!!!!!!!!!!*/
		if(!file_area_have_page(p_file_area)){
			continue;
		}
		/*
		 * We don't need to try to pin this page; we're about to
		 * release the RCU lock anyway.  It is enough to know that
		 * there was a page here recently.
		 */
		break;
	}
	rcu_read_unlock();
	FILE_AREA_PRINT("%s mapping:0x%llx p_file_area:0x%llx start_byte:%lld end_byte:%lld\n",__func__,(u64)mapping,(u64)p_file_area,start_byte,end_byte);

	//return page != NULL;

	/*file_area有page则返回1*/
	return  (p_file_area != NULL && file_area_have_page(p_file_area));
}
EXPORT_SYMBOL(filemap_range_has_page_for_file_area);
bool filemap_range_has_writeback_for_file_area(struct address_space *mapping,
		loff_t start_byte, loff_t end_byte)
{
	//XA_STATE(xas, &mapping->i_pages, start_byte >> PAGE_SHIFT);
	//pgoff_t max = end_byte >> PAGE_SHIFT;
	struct page *page;

	XA_STATE(xas, &mapping->i_pages, (start_byte >> PAGE_SHIFT) >> PAGE_COUNT_IN_AREA_SHIFT);
	/*要查找的最后一个file_area的索引，有余数要加1。错了，不用加1，因为file_area的索引是从0开始*/
	pgoff_t max = (end_byte >> PAGE_SHIFT) >> PAGE_COUNT_IN_AREA_SHIFT;
	struct file_area *p_file_area;
	struct file_stat_base *p_file_stat_base;
	pgoff_t max_page = (end_byte >> PAGE_SHIFT);
	//要查找的第一个page在file_area->pages[]数组里的偏移，令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = (start_byte >> PAGE_SHIFT) & PAGE_COUNT_IN_AREA_MASK;
	unsigned long folio_index_from_xa_index;

	if (end_byte < start_byte)
		return false;

	rcu_read_lock();

	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	/*查找start_byte~end_byte地址范围内的有效page并返回，一直查找max索引的page结束。因为，xas_for_each()里调用的
	 *xas_find()和xas_next_entry()都是以xas->xa_offset为起始索引从xarray tree查找page，找不到则xas->xa_offset加1继续查找，
	 直到查找第一个有效的page。或者xas->xa_offset大于max还是没有找到有效page，则返回NULL*/

	//xas_for_each(&xas, page, max) {
	/*一个个查找start_byte~end_byte地址范围内的有效file_area并返回，一直查找max索引的file_area结束*/
	xas_for_each(&xas, p_file_area, max) {/*这里的max是要查询的最大file_area的索引，不是最大的page索引，很关键!!!!!!!!!*/
		//if (xas_retry(&xas, page))
		if (xas_retry(&xas, p_file_area))
			continue;
		if (xa_is_value(p_file_area)){
			panic("%s page:0x%llx xa_is_value\n",__func__,(u64)p_file_area);
			//continue;
		}

		if(!is_file_area_entry(p_file_area))
			panic("%s mapping:0x%llx p_file_area:0x%llx  NULL\n",__func__,(u64)mapping,(u64)p_file_area);
		p_file_area = entry_to_file_area(p_file_area);

find_page_from_file_area:
		//page = (struct page *)p_file_area->pages[page_offset_in_file_area];
		page = (struct page *)rcu_dereference(p_file_area->pages[page_offset_in_file_area]);
		/*如果page是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
		folio_is_file_area_index_or_shadow_and_clear_NULL(page);
		/*page_cache_delete_batch()函数能进到这里folio一定不是NULL，但是现在无法保证，需要额外判定。但不能break，而是要去查找
		 *file_area里的下一个page。因为 xas_for_each()、xas_find()等函数现在从xarray tree查找的是file_area，不是page。只有
		 *找到的page是NULL，才能break结束查找*/
		if(!page){
			goto next_page;
			//break; 
		}
		folio_index_from_xa_index = (xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area;
		/* 如果有进程此时并发page_cache_delete_for_file_area()里释放该page，这个内存屏障，确保，看到的page不是NULL时，
		 * page在file_area->file_area_statue的对应的bit位一定是1，不是0*/
		smp_rmb();
		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,mapping,page,p_file_area,page_offset_in_file_area,folio_index_from_xa_index);

		//超过了最大索引的page，则本次没有找到有效page
		if(folio_index_from_xa_index > max_page){
			page = NULL;
			break;
		}

		if (PageDirty(page) || PageLocked(page) || PageWriteback(page))
			break;

next_page:
		page_offset_in_file_area ++;

		/*如果file_area里还有page没遍历到，goto find_page_from_file_area去查找file_area里的下一个page。否则到for循环开头
		 *xas_for_each()去查找下一个file_area，此时需要对find_page_from_file_area清0*/
		if(page_offset_in_file_area < PAGE_COUNT_IN_AREA){
			/*page_offset_in_file_area加1不能放到这里，重大逻辑错误。比如，上边判断page_offset_in_file_area是3的folio，
			 *然后执行到f(page_offset_in_file_area < PAGE_COUNT_IN_AREA)判断时，正常就应该不成立的，因为file_area的最后一个folio已经遍历过了*/
			//page_offset_in_file_area ++;
			goto find_page_from_file_area;
		}
		else
			page_offset_in_file_area = 0;
	}
	rcu_read_unlock();
	FILE_AREA_PRINT("%s mapping:0x%llx p_file_area:0x%llx start_byte:%lld end_byte:%lld page:0x%llx\n",__func__,(u64)mapping,(u64)p_file_area,start_byte,end_byte,(u64)page);

	return page != NULL;
}
EXPORT_SYMBOL(filemap_range_has_writeback_for_file_area);
void replace_page_cache_page_for_file_area(struct page *old, struct page *new)
{
	struct folio *fold = page_folio(old);
	struct folio *fnew = page_folio(new);
	struct address_space *mapping = old->mapping;
	void (*freepage)(struct page *) = mapping->a_ops->freepage;
	pgoff_t offset = old->index;
	//XA_STATE(xas, &mapping->i_pages, offset);
	XA_STATE(xas, &mapping->i_pages, offset >> PAGE_COUNT_IN_AREA_SHIFT);
	struct file_area *p_file_area;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = offset & PAGE_COUNT_IN_AREA_MASK;

	VM_BUG_ON_PAGE(!PageLocked(old), old);
	VM_BUG_ON_PAGE(!PageLocked(new), new);
	VM_BUG_ON_PAGE(new->mapping, new);

	get_page(new);
	new->mapping = mapping;
	new->index = offset;

	mem_cgroup_migrate(fold, fnew);

	xas_lock_irq(&xas);
	//xas_store(&xas, new);
	/*如果此时file_stat或者file_area cold_file_stat_delete()、cold_file_area_delete被释放了，那肯定是不合理的
	 *这里会触发panic*/
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        panic("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	p_file_area = (struct file_area *)xas_load(&xas);
	if(!p_file_area || !is_file_area_entry(p_file_area))
		panic("%s mapping:0x%llx p_file_area:0x%llx error\n",__func__,(u64)mapping,(u64)p_file_area);

	p_file_area = entry_to_file_area(p_file_area);
	//if(old != (struct page *)p_file_area->pages[page_offset_in_file_area]){
	if(old != (struct page *)rcu_dereference(p_file_area->pages[page_offset_in_file_area])){
		panic("%s mapping:0x%llx old:0x%llx != p_file_area->pages:0x%llx\n",__func__,(u64)mapping,(u64)old,(u64)p_file_area->pages[page_offset_in_file_area]);
	}
	//p_file_area->pages[page_offset_in_file_area] = fnew;
	rcu_assign_pointer(p_file_area->pages[page_offset_in_file_area],fnew);	
	FILE_AREA_PRINT1("%s mapping:0x%llx p_file_area:0x%llx old:0x%llx fnew:0x%llx page_offset_in_file_area:%d\n",__func__,(u64)mapping,(u64)p_file_area,(u64)old,(u64)fnew,page_offset_in_file_area);

	old->mapping = NULL;
	/* hugetlb pages do not participate in page cache accounting. */
	if (!PageHuge(old))
		__dec_lruvec_page_state(old, NR_FILE_PAGES);
	if (!PageHuge(new))
		__inc_lruvec_page_state(new, NR_FILE_PAGES);
	if (PageSwapBacked(old))
		__dec_lruvec_page_state(old, NR_SHMEM);
	if (PageSwapBacked(new))
		__inc_lruvec_page_state(new, NR_SHMEM);
	xas_unlock_irq(&xas);
	if (freepage)
		freepage(old);
	put_page(old);
}
EXPORT_SYMBOL(replace_page_cache_page_for_file_area);
static void inline async_and_kswapd_refault_page_count(struct file_stat_base *p_file_stat_base,struct folio *folio_temp,void **shadowp)
{
	/* 统计发生refault的page数，跟workingset_refault_file同一个含义。但是有个问题，只有被异步内存回收线程
	 * 回收的page的file_area才会被标记in_free，被kswapd内存回收的page的file_area，就不会标记in_free
	 * 了。问题就出在这里，当这些file_area的page将来被访问，发生refault，但是这些file_area因为没有in_free
	 * 标记，导致这里if不成立，而无法统计发生refault的page，漏掉了。怎么解决，kswapd内存回收的page最终
	 * 也是执行page_cache_delete_for_file_area函数释放page的，在该函数里，如果file_area没有in_free标记，
	 * 则标记in_free。后续该file_area的page再被访问，这里就可以统计到了。
	 *
	 * 有个问题，假设file_area里有3个page，只有一个page0内存回收成功，还有两个page，page1、page3没回收
	 * 成功。file_area被设置了in_free标记。如果将来page1被访问了，这里file_area_refault_file岂不是要
	 * 加1了，这就是误加1了，因为page1并没有发生refault。仔细想想不会，因为page1存在于file_area，将来
	 * 该page被访问，直接从xrray tree找到file_area再找到page1，就返回了，不会执行到当前函数。即便执行
	 * 到当前函数，因为page1存在于file_area，上边xas_set_err(&xas, -EEXIST)就返回了，不会执行到这里的
	 * hot_cold_file_global_info.file_area_refault_file ++。*/
	/*filemap_add_folio()函数增加refault次数有限制条件，就是gfp没有__GFP_WRITE标记，即folio不是write的，而是read的*/

	/*如果该page被异步内存回收线程回收而做了shadow标记*/
	if(1 == (u64)folio_temp){
		/*异步内存回收造成的refault page数统计到/proc/vmstat里*/
		atomic_long_add(1, &vm_node_stat[WORKINGSET_REFAULT_FILE]);

		/* file_area_refault_file存在多进程并发加1的情况而不准。这只是一个粗略的统计值，只要跟WORKINGSET_REFAULT_FILE统计的refault别偏差太大就行*/
		hot_cold_file_global_info.file_area_refault_file ++;
		if(p_file_stat_base->refault_page_count < USHRT_MAX - 2)
			p_file_stat_base->refault_page_count ++;

	}else{
		/*否则folio_temp非NULL，说明是kswapd内存的page保存的shadow。如果是文件截断回收的page则folio_temp是NULL。但是上边加了if(folio_temp)限制*/
		if(shadowp)
			*shadowp = folio_temp;

		/*这是统计kswapd造成的refault page数，有意义吗，反正我也统计不了呀。算了还是先统计吧*/
		hot_cold_file_global_info.kswapd_file_area_refault_file ++;
		if(p_file_stat_base->refault_page_count_last < USHRT_MAX - 2)
			p_file_stat_base->refault_page_count_last ++;
	}
}
noinline int __filemap_add_folio_for_file_area(struct address_space *mapping,
		struct folio *folio, pgoff_t index, gfp_t gfp, void **shadowp)
{
	/*index是long型？area_index_for_page也有必要定义成long型吧???????????????*/
	unsigned int area_index_for_page = index >> PAGE_COUNT_IN_AREA_SHIFT;
	//XA_STATE(xas, &mapping->i_pages, index);
	XA_STATE(xas, &mapping->i_pages, area_index_for_page);
	int huge = folio_test_hugetlb(folio);
	bool charged = false;
	long nr = 1;
	//struct file_stat *p_file_stat;
	struct file_stat_base *p_file_stat_base;
	struct file_area *p_file_area;
	struct folio *folio_temp;
	gfp_t gfp_ori = gfp;
	
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = index & PAGE_COUNT_IN_AREA_MASK;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_swapbacked(folio), folio);
	//mapping_set_update(&xas, mapping); shadow 操作，这里不再设置
	FILE_AREA_PRINT("%s mapping:0x%llx folio:0x%llx index:%ld area_index_for_page:%d\n",__func__,(u64)mapping,(u64)folio,index,area_index_for_page);
	
	/* 这段代码有个隐藏很深的bug!!!!!!!!!!!!，如果进程1文件open后，mmap映射，然后读写映射的地址产生缺页异常。
	 * 接着分配新的page并执行该函数：加global mmap_file_global_lock锁后，分配file_stat并赋值给mapping->rh_reserved1。
	 * 同时，进程2也open该文件，直接读写该文件，然后分配新的page并执行到函数：加global file_global_lock锁后，分配
	 * file_stat并赋值给mapping->rh_reserved1。因为cache文件mmap文件用的global锁不一样，所以无法避免同时分配
	 * file_stat并赋值给mapping->rh_reserved1，这位就错乱了。依次，这段分配file_stat并赋值给mapping->rh_reserved1
	 * 的代码要放到xas_lock_irq(&xas)这个锁里，可以避免这种情况*/
#if 0
	p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	if(!p_file_stat){
		//分配file_stat
		if(RB_EMPTY_ROOT(&mapping->i_mmap.rb_root))
			p_file_stat  = file_stat_alloc_and_init(mapping);
		else
			p_file_stat = add_mmap_file_stat_to_list(mapping);
		if(!p_file_stat){
			xas_set_err(&xas, -ENOMEM);
			goto error; 
		}
	}
#endif
	if (!huge) {
		int error = mem_cgroup_charge(folio, NULL, gfp);
		VM_BUG_ON_FOLIO(index & (folio_nr_pages(folio) - 1), folio);
		if (error)
			return error;
		charged = true;
		/*xas_set_order()里会把page索引重新赋值给xas.xa_index，而xas.xa_index正确应该是file_area索引*/
		//xas_set_order(&xas, index, folio_order(folio));
		xas_set_order(&xas, area_index_for_page, folio_order(folio));
		nr = folio_nr_pages(folio);
	}

	/*这里会去掉gfp的__GFP_WRITE标记*/
	gfp &= GFP_RECLAIM_MASK;
	folio_ref_add(folio, nr);
	folio->mapping = mapping;
	//folio->index = xas.xa_index;
	folio->index = index;

	if(nr != 1 || folio_order(folio) != 0){
		panic("%s index:%ld folio->index:%ld nr:%ld folio_order(folio):%d\n",__func__,index,folio->index,nr,folio_order(folio));
	}
   
	/*这里加rcu_read_lock+rmp_rmb() 很重要，目的有两个。详细mapping_get_entry和mapping_get_entry_for_file_area也有说明。
	 *1：当前文件可能被异步内存回收线程有file_stat_tiny_small转成成file_stat_small，然后标记replaced后，就rcu异步释放掉。
	     这个rcu_read_lock可以保证file_stat_tiny_small结构体不会被立即释放掉，否则当前函数使用的file_stat_tiny_small内存就是无效
	  2: 当前文件file_stat可能因长时间不使用被异步内存回收线程并发 cold_file_stat_delete() rcu异步释放掉，并标记
	     file_stat->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE.rcu_read_lock保证file_stat结构体不会被立即释放掉，否则这里使用
		 file_stat就是无效内存访问。smp_rmb()是保证立即看到mapping->rh_reserved1是SUPPORT_FILE_AREA_INIT_OR_DELETE。其实不用加内存
		 屏障cold_file_stat_delete()函数和当前函数都有xas_lock_irq(&xas)加锁判断mapping->rh_reserved1是否是SUPPORT_FILE_AREA_INIT_OR_DELETE
		 为了保险，还是加上smp_rmb()，以防止将来下边的if(SUPPORT_FILE_AREA_INIT_OR_DELETE == mapping->rh_reserved1)没有放到xas_lock_irq()加锁里*/
	rcu_read_lock();
	smp_rmb();
	do {
		//这里边有执行xas_load()，感觉浪费性能吧!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		unsigned int order = xa_get_order(xas.xa, xas.xa_index);
		void *entry/*, *old = NULL*/;
		/*这个赋值NULL必须，因为这里可能第1次把folio添加到xarray tree失败，然后第2次这里赋值NULL 就受上一个page的影响了*/
		folio_temp = NULL;

		if (order > folio_order(folio)){
			panic("%s order:%d folio_order:%d error !!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,order,folio_order(folio));
			xas_split_alloc(&xas, xa_load(xas.xa, xas.xa_index),
					order, gfp);
		}
		xas_lock_irq(&xas);
		/*file_stat可能会被方法删除，则分配一个新的file_stat，具体看cold_file_stat_delete()函数*/
		if(SUPPORT_FILE_AREA_INIT_OR_DELETE == READ_ONCE(mapping->rh_reserved1)){
			p_file_stat_base = file_stat_alloc_and_init_tiny_small(mapping,!mapping_mapped(mapping));

			if(!p_file_stat_base){
				xas_set_err(&xas, -ENOMEM);
				//goto error; 
				goto unlock;
			}
		}else
			p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;

		if(file_stat_in_delete_base(p_file_stat_base))
			panic("%s %s %d file_stat:0x%llx status:0x%x in delete\n",__func__,current->comm,current->pid,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

		//xas_lock_irq加锁后，检测到待添加的file_area已经被其他进程并发添加到xarray tree了
		xas_for_each_conflict(&xas, entry) {
			//old = entry;
			//if (!xa_is_value(entry)) 

			//if(!p_file_area)从进来说明file_area已经非NULL，不用再判断
			//    goto ;
			p_file_area = entry_to_file_area(entry);

			/*如果p_file_area->pages[0/1]保存的folio是NULL，或者是folio_is_file_area_index_or_shadow(folio)，都要分配新的page。
			 *否则才说明是有效的page指针，直接goto unlock，不用再分配新的。如果正好file_area的索引是0保存在p_file_area->pages[0/1]，
			 *此时if也不成立，也要分配新的page。只有不是NULL且不是file_area索引时才说明是有效的folio指针，此时才会goto unlock，不用再分配新的page*/
			folio_temp = rcu_dereference(p_file_area->pages[page_offset_in_file_area]);
			//page已经添加到file_area了
			//if(NULL != p_file_area->pages[page_offset_in_file_area])
			//if(NULL != folio_temp && !folio_is_file_area_index_or_shadow(folio_temp)){
			if(folio_temp){
				if(!folio_is_file_area_index_or_shadow(folio_temp)){
					xas_set_err(&xas, -EEXIST);
					goto unlock;
				}

				/*统计refault page个数，但只有read的refault page才计入refault统计*/
				if(!(gfp_ori & __GFP_WRITE)){
					async_and_kswapd_refault_page_count(p_file_stat_base,folio_temp,shadowp);

					if((file_stat_in_test_base(p_file_stat_base) || is_global_file_stat_file_in_debug(mapping)) && (1 == (u64)folio_temp)){
						printk("%s refault file_stat:0x%llx file_area:0x%llx status:0x%x index:%ld  %s\n",__func__,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,index,get_file_name_no_lock_from_mapping(mapping));
					}
				}

			}

			//file_area已经添加到xarray tree，但是page还没有赋值到file_area->pages[]数组
			goto find_file_area;
		}

		//分配file_area
		p_file_area  = file_area_alloc_and_init(area_index_for_page,p_file_stat_base,mapping);
		if(!p_file_area){
			//xas_unlock_irq(&xas);
			xas_set_err(&xas, -ENOMEM);
			//goto error; 
			goto unlock; 
		}

		//xas_store(&xas, folio);
		xas_store(&xas, file_area_to_entry(p_file_area));
		if (xas_error(&xas))
			goto unlock;

find_file_area:
		/*if(get_file_name_match(p_file_stat_base,"binlog.index","resolv.conf","system.devices")){
			printk("%s file_stat:0x%llx status 0x%x mmap:%d mapping:0x%llx add_folio\n",__func__,(u64)p_file_stat_base,p_file_stat_base->file_stat_status,mapping_mapped(mapping),(u64)mapping);
			dump_stack();
		}*/
		
		set_file_area_page_bit(p_file_area,page_offset_in_file_area);
		FILE_AREA_PRINT1("%s mapping:0x%llx p_file_area:0x%llx folio:0x%llx index:%ld page_offset_in_file_area:%d\n",__func__,(u64)mapping,(u64)p_file_area,(u64)folio,index,page_offset_in_file_area);
        
		/*不是NULL并且不是file_area的索引时，才触发crash，这个判断是多余的???????????????????。算了还是加上这个判断吧，多一个异常判断多点靠谱，最然看着没啥用*/
		folio_temp = rcu_dereference(p_file_area->pages[page_offset_in_file_area]);
		if(NULL != folio_temp && !folio_is_file_area_index_or_shadow(folio_temp))
			panic("%s p_file_area->pages:0x%llx != NULL error folio:0x%llx\n",__func__,(u64)p_file_area->pages[page_offset_in_file_area],(u64)folio);

		/*这里跟delete file_area page的两个函数配合，在set/clear file_area->file_area_state和向file_area->pages[]保存page/设置NULL
		 *之间都加了个内存屏障。虽然这3个函数的这些操作前都加了spin_lock(&mapping->host->i_lock锁，但是分析spin_lock/spin_unlock
		 *源码后，spin_lock加锁能100%保证对两个变量的赋值一定按照顺序生效吗。比如该函数里执行
		 *"set_file_area_page_bit(p_file_area,page_offset_in_file_area)" 和 "p_file_area->pages[page_offset_in_file_area] = folio"
		 *后，delete_from_page_cache_batch_for_file_area()函数先执行
		 *"folio = p_file_area->pages[page_offset_in_file_area] ;if(!folio) goto next_page"和
		 *"clear_file_area_page_bit(p_file_area,page_offset_in_file_area)" ，存在一种可能，folio = p_file_area->pages[page_offset_in_file_area]
		 *得到的folio不是NULL，cache在多核cpu之间已经同步生效。但是file_area->file_area_state里的page bit还是0，set操作还没生效。
		 *于是clear_file_area_page_bit(p_file_area,page_offset_in_file_area)里触发crash，因为file_area->pages[]里存在page，但是对应的
		 *file_area->file_area_state里的page bit是0，就会触发crash。因此在这两个函数里，才进行
		 *"set/clear file_area->file_area_state跟向file_area->pages[]保存page/设置NULL，之间都加了个内存屏障"，确保该函数里
		 *"set_file_area_page_bit(p_file_area,page_offset_in_file_area)"一定先在"p_file_area->pages[page_offset_in_file_area] = folio"
		 *生效。反过来，delete_from_page_cache_batch_for_file_area()和page_cache_delete_for_file_area()函数里也要加同样的内存屏障，
		 *确保对"p_file_area->pages[page_offset_in_file_area]=NULL" 先于"clear_file_area_page_bit(p_file_area,page_offset_in_file_area)"
		 *之前生效，然后保证该函数先看到p_file_area->pages[page_offset_in_file_area]里的page是NULL，
		 *"set_file_area_page_bit(p_file_area,page_offset_in_file_area)"执行后，p_file_area->pages[page_offset_in_file_area]一定是NULL，
		 *否则"if(NULL != p_file_area->pages[page_offset_in_file_area])"会触发crash。
		 *
		 * 但是理论上spin_lock加锁肯定能防护变量cpu cache同步延迟问题，加个smp_wmb/smp_mb内存屏障没啥用。此时发现个问题，我在看内核原生
		 * page_cache_delete/page_cache_delete_batch/__filemap_add_folio 向xarray tree保存page指针或者删除page，都是spin_lock(xas_lock)
		 * 加锁后，执行xas_store(&xas, folio)或xas_store(&xas, folio)，里边最后都是执行rcu_assign_pointer(*slot, entry)把page指针或者NULL
		 * 保存到xarray tree里父节点的槽位。并且这些函数xas_load查找page指针时，里边都是执行rcu_dereference_check(node->slots[offset],...)
		 * 返回page指针。于是，在这3个函数里，查找page指针 或者 保存page指针到xarray tree也都使用rcu_assign_pointer和rcu_dereference_check。
		 * 目的是：这两个rcu函数都是对变量的volatile访问，再加上内存屏障，绝对保证对变量的访问没有cache影响，并且每次都是从内存中访问。
		 * 实在没其他思路了，只能先这样考虑了。
		 *
		 * 还有一点，我怀疑这个bug的触发时机跟我的另一个bug有关:file_stat_lock()里遇到引用计数是0的inode，则错误的执行iput()释放掉该inode。
		 * 这导致inode的引用计数是-1，后续该inode又被进程访问，inode的引用计数是0.结果此时触发了关机，这导致该inode被进程访问时，该inode
		 * 被umount进程强制执行evict()释放掉。inode一边被使用一边被释放，可能会触发未知问题。虽然umount进程会执行
		 * page_cache_delete_batch_for_file_area()释放文件inode的page，而此时访问该inode的进程可能正执行__filemap_add_folio_for_file_area()
		 * 向file_area->pages[]保存page并设置page在file_area->file_area_state的bit位，但是两个进程都是spin_lock(&mapping->host->i_lock加锁
		 * 进行的操作，不会有问题吧?其他会导致有问题的场景，也没有。现在已经解决"file_stat_lock()里遇到引用计数是0的inode，则错误的执行
		 * iput()释放掉该inode"的bug，这个问题估计不会再出现。以上就是针对"20240723  复制新的虚拟机后 clear_file_area_page_bit crash"
		 * case的最终分析，被折磨了快3周!!!!!!!!!!!!!
		 */
		smp_wmb();
		//folio指针保存到file_area
		//p_file_area->pages[page_offset_in_file_area] = folio;
		rcu_assign_pointer(p_file_area->pages[page_offset_in_file_area], folio);

		mapping->nrpages += nr;

		/* hugetlb pages do not participate in page cache accounting */
		if (!huge) {
			__lruvec_stat_mod_folio(folio, NR_FILE_PAGES, nr);
			if (folio_test_pmd_mappable(folio))
				__lruvec_stat_mod_folio(folio,
						NR_FILE_THPS, nr);
		}
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));
	//} while (0);

	if (xas_error(&xas))
		goto error;
     
	/*trace_mm_filemap_add_to_page_cache(folio);*/
	    
	rcu_read_unlock();
	return 0;
error:
//if(p_file_area) 在这里把file_area释放掉??????????有没有必要
//	file_area_alloc_free();

	rcu_read_unlock();

	if (charged)
		mem_cgroup_uncharge(folio);
	folio->mapping = NULL;
	/* Leave page->index set: truncation relies upon it */
	folio_put_refs(folio, nr);
	return xas_error(&xas);
}
EXPORT_SYMBOL(__filemap_add_folio_for_file_area);
pgoff_t page_cache_next_miss_for_file_area(struct address_space *mapping,
		pgoff_t index, unsigned long max_scan)
{
	//XA_STATE(xas, &mapping->i_pages, index);
	XA_STATE(xas, &mapping->i_pages, index >> PAGE_COUNT_IN_AREA_SHIFT);
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = index & PAGE_COUNT_IN_AREA_MASK;
	struct file_area *p_file_area;
	struct file_stat_base *p_file_stat_base;
	unsigned long folio_index_from_xa_index = 0;
	struct folio *folio;
    
	/*该函数没有rcu_read_lock，但是调用者里已经执行了rcu_read_lock，这点需要注意!!!!!!!!!!!!!!*/

	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	/*while (max_scan--) {//max_scan原本代表扫描的最多扫描的page数，现在代表的是最多扫描的file_area数，
	 *自然不能再用了。于是放到下边if(max_scan)那里*/
	while (1) {
		//xas_next()里边自动令xas->xa_index和xas->xa_offset加1
		void *entry = xas_next(&xas);
		//if (!entry || xa_is_value(entry))
		if(!entry)
			break;

		if(xa_is_value(entry) || !is_file_area_entry(entry))
			panic("%s mapping:0x%llx p_file_area:0x%llx error\n",__func__,(u64)mapping,(u64)entry);

		p_file_area = entry_to_file_area(entry);
find_page_from_file_area:
		max_scan--;
		if(0 == max_scan)
			break;

		folio_index_from_xa_index = (xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area;
		//if (xas.xa_index == 0)这个判断怎么可能成立??????????????????????
		//if ((xas.xa_index + page_offset_in_file_area)  == 0)
		if (folio_index_from_xa_index  == 0)
			break;
		
		folio = rcu_dereference(p_file_area->pages[page_offset_in_file_area]);
		/*如果folio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
		folio_is_file_area_index_or_shadow_and_clear_NULL(folio);
		//page是NULL则直接break，这个跟page_cache_next_miss函数原有的if (!entry)break 同理，即遇到第一个NULL page则break结束查找
		//if(p_file_area->pages[page_offset_in_file_area] == NULL)
		if(!folio)
			break;

		/* 如果有进程此时并发page_cache_delete_for_file_area()里释放该page，这个内存屏障，确保，看到的page不是NULL时，
		 * page在file_area->file_area_statue的对应的bit位一定是1，不是0*/
		smp_rmb();
		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,mapping,p_file_area->pages[page_offset_in_file_area],p_file_area,page_offset_in_file_area,folio_index_from_xa_index);

		page_offset_in_file_area ++;

		/*如果file_area里还有page没遍历到，goto find_page_from_file_area去查找file_area里的下一个page。否则到while循环开头
		 *xas_next(&xas)去查找下一个file_area，此时需要find_page_from_file_area清0，这个很关键*/
		if(page_offset_in_file_area < PAGE_COUNT_IN_AREA){
			/*page_offset_in_file_area加1不能放到这里，重大逻辑错误。比如，上边判断page_offset_in_file_area是3的folio，
			 *然后执行到f(page_offset_in_file_area < PAGE_COUNT_IN_AREA)判断时，正常就应该不成立的，因为file_area的最后一个folio已经遍历过了*/
			//page_offset_in_file_area ++;
			goto find_page_from_file_area;
		}
		else
			page_offset_in_file_area = 0;
	}

	FILE_AREA_PRINT("%s mapping:0x%llx index:%ld return:%ld\n",__func__,(u64)mapping,index,xas.xa_index + page_offset_in_file_area);
	
	//return xas.xa_index;
	//return (xas.xa_index + page_offset_in_file_area);

	/*这里要返回第一个空洞page的索引，但xas.xa_index加1代表个(1<< PAGE_COUNT_IN_AREA_SHIFT)个page，因此
	 * xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT才是真实的page索引*/
	return ((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area);
}
EXPORT_SYMBOL(page_cache_next_miss_for_file_area);
pgoff_t page_cache_prev_miss_for_file_area(struct address_space *mapping,
		pgoff_t index, unsigned long max_scan)
{
	//XA_STATE(xas, &mapping->i_pages, index);
	XA_STATE(xas, &mapping->i_pages, index >> PAGE_COUNT_IN_AREA_SHIFT);
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = index & PAGE_COUNT_IN_AREA_MASK;
	struct file_area *p_file_area;
	struct file_stat_base *p_file_stat_base;
	unsigned long folio_index_from_xa_index = 0 ;
	struct folio *folio;

	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	/*while (max_scan--) {//max_scan原本代表扫描的最多扫描的page数，现在代表的是最多扫描的file_area数，
	 *自然不能再用了。于是放到下边if(max_scan)那里*/
	while (1) {
		//xas_prev()里边自动令xas->xa_index和xas->xa_offset减1
		void *entry = xas_prev(&xas);
		//if (!entry || xa_is_value(entry))
		if (!entry)
			break;
		if(xa_is_value(entry) || !is_file_area_entry(entry))
			panic("%s mapping:0x%llx p_file_area:0x%llx error\n",__func__,(u64)mapping,(u64)entry);

		p_file_area = entry_to_file_area(entry);
find_page_from_file_area:
		max_scan--;
		if(0 == max_scan)
			break;

		folio_index_from_xa_index = (xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area;
		//if (xas.xa_index == ULONG_MAX) 这个判断怎么可能成立??????????????????????
		//if ((xas.xa_index + page_offset_in_file_area)  == ULONG_MAX)
		if (folio_index_from_xa_index == ULONG_MAX)
			break;

		folio = rcu_dereference(p_file_area->pages[page_offset_in_file_area]);
		/*如果folio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
		folio_is_file_area_index_or_shadow_and_clear_NULL(folio);
		//page是NULL则直接break，这个跟page_cache_prev_miss函数原有的if (!entry)break 同理，即遇到第一个NULL page则break结束查找
		//if(p_file_area->pages[page_offset_in_file_area] == NULL)
		if(!folio)
			break;
		
		/* 如果有进程此时并发page_cache_delete_for_file_area()里释放该page，这个内存屏障，确保，看到的page不是NULL时，
		 * page在file_area->file_area_statue的对应的bit位一定是1，不是0*/
		smp_rmb();
		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,mapping,p_file_area->pages[page_offset_in_file_area],p_file_area,page_offset_in_file_area,folio_index_from_xa_index);

		/*如果page_offset_in_file_area是0,则说明file_area的page都被遍历过了，那就到for循环开头xas_prev(&xas)去查找上一个file_area。
		 *否则，只是令page_offset_in_file_area减1，goto find_page_from_file_area去查找file_area里的上一个page*/
		if(page_offset_in_file_area == 0)
			page_offset_in_file_area = PAGE_COUNT_IN_AREA - 1;
		else{
			page_offset_in_file_area --;
			goto find_page_from_file_area;
		}
	}

	FILE_AREA_PRINT("%s mapping:0x%llx index:%ld return:%ld\n",__func__,(u64)mapping,index,xas.xa_index + page_offset_in_file_area);
	
	//return xas.xa_index;
	//return (xas.xa_index + page_offset_in_file_area);
	return ((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area);
}
EXPORT_SYMBOL(page_cache_prev_miss_for_file_area);
void *mapping_get_entry_for_file_area(struct address_space *mapping, pgoff_t index)
{
	//XA_STATE(xas, &mapping->i_pages, index);
	//page索引除以2，转成file_area索引
	unsigned int area_index_for_page = index >> PAGE_COUNT_IN_AREA_SHIFT;
	XA_STATE(xas, &mapping->i_pages, area_index_for_page);
	struct folio *folio = NULL;

	//struct file_stat *p_file_stat;
	struct file_stat_base *p_file_stat_base;
	struct file_area *p_file_area;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = index & PAGE_COUNT_IN_AREA_MASK;
	rcu_read_lock();

	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();

#ifdef ASYNC_MEMORY_RECLAIM_DEBUG
	/*mapping->rh_reserved1必须大于1，跟file_stat_in_delete(p_file_stat)一个效果，只用一个*/
	//if(!file_stat_in_delete(p_file_stat) && IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//如果此时这个file_area正在被释放，这里还能正常被使用吗？用了rcu机制做防护，后续会写详细分析!!!!!!!!!!!!!!!!!!!!!
		p_file_area = find_file_area_from_xarray_cache_node(&xas,p_file_stat_base,index);
		if(p_file_area){
			//令page索引与上0x3得到它在file_area的pages[]数组的下标
			folio = p_file_area->pages[page_offset_in_file_area];
			/*如果folio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
			folio_is_file_area_index_or_shadow_and_clear_NULL(folio);
			if(folio && folio->index == index){
				xarray_tree_node_cache_hit ++;
				goto find_folio;
			}
			/*走到这里，说明找到了file_area但没有找到匹配索引的page。那就重置xas，重新重xarray tree查找。能否这里直接返回NULL，
			 *即判断为查找page失败呢?不能，因为此时其他进程可能也在并发执行__filemap_add_folio、mapping_get_entry、page_cache_delete
			 *并发修改p_file_stat->xa_node_cache和p_file_stat->xa_node_cache_base_index，导致二者不匹配，即不代表同一个node节点。只能重置重新查找了*/
			xas.xa_offset = area_index_for_page;
			xas.xa_node = XAS_RESTART;
		}
	}
#endif	
    /*执行到这里，可能mapping->rh_reserved1指向的file_stat被释放了，该文件的文件页page都被释放了。用不用这里直接return NULL，不再执行下边的
	 * p_file_area = xas_load(&xas)遍历xarray tree？怕此时遍历xarray tree有问题!没事，因为此时xarray tree是空树，p_file_area = xas_load(&xas)
	 * 直接返回NULL，和直接return NULL一样的效果*/

repeat:
	xas_reset(&xas);

	//folio = xas_load(&xas);
	p_file_area = xas_load(&xas);

	/*之前得做if (xas_retry(&xas, folio))等3个if判断，现在只用做if(!is_file_area_entry(p_file_area))判断就行了*/
	if(!is_file_area_entry(p_file_area)){
		if(!p_file_area)
			goto out;

		/*xas_retry()里有xas->xa_node = XAS_RESTART，这个隐藏的很深，这样执行xas_next(&xas)时，if(xas_not_node(node))成立，直接从
		 *xarray tree按照老的xas->xa_index重新查找，不会再执行xas->xa_index++和xas->xa_offset++而从父节点直接获取下一个索引的成员了*/
		if (xas_retry(&xas, p_file_area))
			goto repeat;

		panic("%s mapping:0x%llx p_file_area:0x%llx error!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,(u64)mapping,(u64)p_file_area);
		if(xa_is_value(p_file_area))
			goto out;
	}
#if 0
	if (xas_retry(&xas, p_file_area))
		goto repeat;
	if (!folio || xa_is_value(folio))
		goto out;
#endif

	p_file_area = entry_to_file_area(p_file_area);
	//folio = p_file_area->pages[page_offset_in_file_area];
	folio = rcu_dereference(p_file_area->pages[page_offset_in_file_area]);
	/*如果folio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
	folio_is_file_area_index_or_shadow_and_clear_NULL(folio);

	//if (!folio || xa_is_value(folio))
	if (!folio /*|| xa_is_value(folio)*/)//xa_is_value()只是看bit0是否是1，其他bit位不用管
		goto out;

#ifdef ASYNC_MEMORY_RECLAIM_DEBUG
find_folio:
#endif

	/* 检测查找到的page是否正确，不是则crash。由于最新版本，还会判断查找到的page对应的file_area->file_area_state的
	 * bit位是否置1了，表示该page保存到了file_area->pages[]数组，没有置1就要crash。但是有个并发问题，如果
	 * 该page此时被其他进程执行page_cache_delete()并发删除，会并发把page在file_area->file_area_statue的对应的bit位
	 * 清0，导致这里判定page存在但是page在file_area->file_area_statue的对应的bit位缺时0，于是会触发crash。解决
	 * 方法时，把这个判断放到该page判定有效且没有被其他进程并发释放后边*/
	//CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,folio,p_file_area,page_offset_in_file_area,((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area));

	if (!folio_try_get_rcu(folio))
		goto repeat;

	//if (unlikely(folio != xas_reload(&xas))) {
	if (unlikely(folio != rcu_dereference(p_file_area->pages[page_offset_in_file_area]))) {
		folio_put(folio);
		goto repeat;
	}
	/*到这里才判定page有有效，没有被其他进程并发释放掉*/
	CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,mapping,folio,p_file_area,page_offset_in_file_area,((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area));
	//统计page引用计数
	//hot_file_update_file_status(mapping,p_file_stat_base,p_file_area,1,FILE_AREA_PAGE_IS_WRITE/*,folio->index*/);
	hot_file_update_file_status(mapping,p_file_stat_base,p_file_area,FILE_AREA_PAGE_IS_WRITE);

#ifdef ASYNC_MEMORY_RECLAIM_DEBUG
	/*如果本次查找的page所在xarray tree的父节点变化了，则把最新的保存到mapping->rh_reserved2。
	 *同时必须判断父节点的合法性，分析见filemap_get_read_batch_for_file_area()。其实这里不用判断，走到这里肯定父节点合法.*/
	//if(xa_is_node(xas.xa_node) && p_file_stat->xa_node_cache != xas.xa_node){
	if(p_file_stat_base->xa_node_cache != xas.xa_node){
		/*保存父节点node和这个node节点slots里最小的page索引。这两个赋值可能被多进程并发赋值，导致
		 *mapping->rh_reserved2和mapping->rh_reserved3 可能不是同一个node节点的，错乱了。这就有大问题了！
		 *没事，这种情况上边的if(page && page->index == offset)就会不成立了*/
		//p_file_stat->xa_node_cache = xas.xa_node;
		//p_file_stat->xa_node_cache_base_index = index & (~FILE_AREA_PAGE_COUNT_MASK);
		p_file_stat_base->xa_node_cache = xas.xa_node;
		p_file_stat_base->xa_node_cache_base_index = index & (~FILE_AREA_PAGE_COUNT_MASK);
	}
#endif

out:
	rcu_read_unlock();

#ifdef ASYNC_MEMORY_RECLAIM_DEBUG
	FILE_AREA_PRINT("%s mapping:0x%llx p_file_area:0x%llx folio:0x%llx index:%ld xa_node_cache:0x%llx cache_base_index:%ld\n",__func__,(u64)mapping,(u64)p_file_area,(u64)folio,index,(u64)p_file_stat_base->xa_node_cache,p_file_stat_base->xa_node_cache_base_index);
#else
	FILE_AREA_PRINT("%s mapping:0x%llx p_file_area:0x%llx folio:0x%llx index:%ld\n",__func__,(u64)mapping,(u64)p_file_area,(u64)folio,index);
#endif

	return folio;
}
EXPORT_SYMBOL(mapping_get_entry_for_file_area);
/*这个函数可以加入node cache机制。这个函数做成inline形式，因为调用频繁，降低性能损耗*/
void *get_folio_from_file_area_for_file_area(struct address_space *mapping,pgoff_t index)
{
	struct file_area *p_file_area;
	struct file_stat_base *p_file_stat_base;
	struct folio *folio = NULL;

	rcu_read_lock();
	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();

	/*内存屏障后再探测mapping->rh_reserved1是否是0，即对应文件inode已经被释放了。那mapping已经失效*/
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		p_file_area = xa_load(&mapping->i_pages,index >> PAGE_COUNT_IN_AREA_SHIFT);
		if(!p_file_area)
		{
			goto out;
		}
		p_file_area = entry_to_file_area(p_file_area);
		folio = p_file_area->pages[index & PAGE_COUNT_IN_AREA_MASK];
		/*如果folio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
		folio_is_file_area_index_or_shadow_and_clear_NULL(folio);
	
		/* 到这里才判定page有有效，没有被其他进程并发释放掉。但这里是内核预读代码page_cache_ra_unbounded()调用的，
		 * 原生代码并没有判定该page是否会因page内存回收而判定page是否无效，这里还要判断吗？目前只判断索引*/
		if(folio && (/*folio->index != index ||*/ folio->mapping != mapping)){
			/*if成立说明该folio被内存回收了，那就设置folio为NULL而无效。这里确实遇到过，是正常现象，不能panic*/
	        //panic("%s %s %d index:%ld folio->index:%ld folio:0x%llx mapping:0x%llx\n",__func__,current->comm,current->pid,index,folio->index,(u64)folio,(u64)mapping);
	        pr_warn("%s %s %d index:%ld folio->index:%ld folio:0x%llx mapping:0x%llx\n",__func__,current->comm,current->pid,index,folio->index,(u64)folio,(u64)mapping);
			folio = NULL;
		}
	}
out:
	rcu_read_unlock();
	FILE_AREA_PRINT("%s mapping:0x%llx p_file_area:0x%llx folio:0x%llx index:%ld\n",__func__,(u64)mapping,(u64)p_file_area,(u64)folio,index);

	/* 这些都时预读产生的page，标记file_area的in_read标记。但是不令file_area的访问计数加1，不赋值file_area_age=global_age，
	 * 节省性能。我只要标记file_area的in_read，异步内存回收线程就不会立即回收这些file_area的page了。后续这些folio真的被用
	 * 户读，会执行到filemap_get_read_batch()，令file_area的访问计数加1，赋值file_area_age=global_age。但是又遇到问题了，
	 * 因为这里的set_file_area_in_read，把文件的所有file_area都在这里设置了in_read标记。因为所有的page大部分都是预读流程
	 * 产生的，因为这里设置了in_read标记，导致后续这些file_area的page被真正读写时，执行到hot_file_update_file_status()函数，
	 * 因为file_area已经有了in_read标记，不再清理file_area的in_writeonly标记。导致原本读属性的文件被判定为writeonly
	 * 文件，导致更容易回收这些文件的file_area的page，导致refault率升高。注意，这是实际测试遇到的问题!!!!!!!!注意，实际
	 * 测试还证明，预读时执行到的page_cache_ra_unbounded函数，如果第一次在get_folio_from_file_area_for_file_area()从xarray
	 * tree没有查到folio，就无法标记file_area的in_read属性和清理file_stat的writeonly标记。然后分配folio和file_area并添加
	 * 到xarray tree。这个预读的folio就不会执行get_folio_from_file_area_for_file_area()了。后续该folio被读执行到
	 * hot_file_update_file_status()，file_area没有in_read标记，还需要再标记file_area的in_read标记。*/
	/* hot_file_update_file_status()和get_folio_from_file_area_for_file_area()函数都需要标记file_area的in_read标记
	 * 和清理文件file_stat的writeonly标记，只在hot_file_update_file_status()里设置可以吗，浪费性能！不行，没办法解决*/
	if(folio && !file_area_in_read(p_file_area)){
		set_file_area_in_read(p_file_area);
		if(file_stat_in_writeonly_base(p_file_stat_base))
			clear_file_stat_in_writeonly_base(p_file_stat_base);

		//p_file_area->file_area_age = hot_cold_file_global_info.global_age; 
	}
	return (void *)folio;
}
EXPORT_SYMBOL(get_folio_from_file_area_for_file_area);

/*find_get_entry函数原本目录是：以xas->xa_index为起始索引，去xarray tree查找第一个不是NULL的page，
 *如果找到索引大于max，直接返回NULL。因此，这个函数这样设计，先xas_find()查找file_area，这个file_area可能
 *没有一个page，那就goto retry继续查找，直到xas_find(xas, max)因查到的索引大于max返回NULL，该函数直接return。
 *如果xas_find()找到的file_area有page，则判断合法后直接return folio。然后下次你再执行改函数时，就不能直接执行
 *xas_find(xas, max)了，而要去刚才查到的page的file_area里，继续查找其他page。直到一个file_area的page全被获取到
 *才能执行xas_find(xas, max)去查找下一个file_area*/
inline struct folio *find_get_entry_for_file_area(struct xa_state *xas, pgoff_t max,
		xa_mark_t mark,struct file_area **p_file_area,unsigned int *page_offset_in_file_area,struct address_space *mapping)
{
	struct folio *folio;
	//计算要查找的最大page索引对应的file_area索引
	pgoff_t file_area_max = max >> PAGE_COUNT_IN_AREA_SHIFT;
	unsigned long folio_index_from_xa_index;
	void *old_entry;

	/*如果*p_file_area不是NULL，说明上次执行该函数里的xas_find(xas, max)找到的file_area，还有剩余的page没有获取
	 *先goto find_page_from_file_area分支把这个file_area剩下的page探测完*/
	if(*p_file_area != NULL)
		goto find_page_from_file_area;
retry:
	/*这里会令xas.xa_index和xas.xa_offset自动加1，然后去查找下一个file_area。那自然是不行的。find_get_entries()里
	 *folio = find_get_entry_for_file_area(&xas, end, XA_PRESENT)调用该函数，只是一次获取一个page，不是一个file_area。
	 *要保证一个file_area的page全被获取过了，才能再执行xas_find()获取下一个file_area*/
	if (mark == XA_PRESENT)
		//folio = xas_find(xas, max);
		*p_file_area = xas_find(xas, file_area_max);/*这里的必须是查找的最大file_area的索引file_area_max，不能最大page索引max*/
	else
		//folio = xas_find_marked(xas, max, mark);
		*p_file_area = xas_find_marked(xas, file_area_max, mark);

	if (NULL == *p_file_area){
		FILE_AREA_PRINT("%s %s %d p_file_area NULL max:%ld xas.xa_index:%ld page_offset_in_file_area:%d\n",__func__,current->comm,current->pid,max,xas->xa_index,*page_offset_in_file_area);

		return NULL;
	}

	//if (xas_retry(xas, folio))
	if (xas_retry(xas, *p_file_area))
		goto retry;

    /* 突然有个想法，如果上边p_file_area = xas_find(xas, file_area_max) 和异步内存回收线程cold_file_area_delete并发执行。
	 * 这里先返回了p_file_area。然后下边p_file_area->start_index，p_file_area->mapping = 0这样使用file_area。此时因为该
	 * file_area没有page，就会被异步内存回收线程cold_file_area_delete()释放掉，那岂不是到这里p_file_area->mapping = 0
	 * 就是非法内存访问了。郁闷了半天，像当前函数这样使用file_area，在filemap.c文件里随处可见：都是先xrray tree里查找
	 * p_file_area，然后查找file_area里的page。如果此时file_area被异步内存回收线程cold_file_area_delete了，那就是非法
	 * 内存访问或者向非法内存赋值了。风险太大了，这个问题太大了，我的file_area异步内存回收方案从一开始的设计就有问题。
	 * 想了半个小时，根本没事!!!!!!!因为filemap.c从xrray tree查找file_area再使用file_area，全程都有rcu_read_lock防护，
	 * 异步内存回收线程cold_file_area_delete无法真正释放掉file_area结构体。这个设计思路一开始就有，只是时间长了，
	 * 记忆不深了，自己吓唬自己!!!!!!!!!!*/

	/*
	 * A shadow entry of a recently evicted page, a swap
	 * entry from shmem/tmpfs or a DAX entry.  Return it
	 * without attempting to raise page count.
	 */
	//if (!folio || xa_is_value(folio))//注释掉，放到下边判断
	//	return folio;
	*p_file_area = entry_to_file_area(*p_file_area);

	/* 在iput状态，mapping_exiting()成立，找到file_area立即设置set_file_area_in_mapping_delete()，异步内存回收线程碰到
	 * 这种file_area，都不能在对该file_area进行list_move移动到其他链表，或者cold_file_area_delete()，作为一个保险。
	 * 主要以前遇到过bug，异步内存回收线程把这里已经iput()的file_area，执行cold_file_area_delete()错误把file_area从
	 * xarray tree剔除了，导致这个xarray tree所属文件的对应索引的file_area被错误从xarray tree剔除了。
	 * 接着，继续执行下边的代码，在file_area没有page时，把file_area移动到global_file_stat_delete链表，后续异步内存
	 * 回收线程再遍历global_file_stat_delete链表上的file_area，释放掉。注意，如果文件不是mapping_exiting()状态，
	 * 直接goto find_page_from_file_area，获取file_area的page。这个方案取消了，原因看下边*/
	/*if(mapping_exiting(mapping)){
		if(!file_area_in_mapping_delete(*p_file_area))
			set_file_area_in_mapping_delete(*p_file_area);
	}
	else{
		goto find_page_from_file_area; 
	}*/

	/* 重大bug，引入多层warm链表机制后，tiny small文件的file_area全都移动到global_file_stat链表，释放掉文件file_stat，隐患就此
	 * 引入。这些文件在iput()释放时，因为file_stat早已经释放掉，无法在最后执行的__destroy_inode_handler_post函数里，把file_stat
	 * 移动到global->delete链表，然后异步内存回收线程再从global->delete链表得到该file_stat，释放掉file_area。于是想了一个办法，
	 * 在iput()截断文件pagecache必然执行truncate_inode_pages_range->find_lock_entries->find_get_entry_for_file_area 函数里，遇到
	 * 0个page的file_area，则把file_area按照其file_area_delete成员移动到global_file_stat.file_area_delete_list链表，后续再有异步
	 * 内存回收线程从global_file_stat.file_area_delete_list链表得到该file_area释放掉。正确情况，file_area都是以其file_area_list
	 * 成员添加到file_stat->temp、refault、多层warm链表。为什么不按照file_area的file_area_list成员，把file_area移动到
	 * global_file_stat.file_area_delete_list链表呢，防护并发问题。因此异步内存回收线程同时也会操作file_area的file_area_list成员
	 * 令file_area在file_stat->temp、refault、多层warm链表之间来回移动。以上的种种思考，看似没有问题，实则埋入了好几大坑
	 * 1：iput()截断文件pagecache执行到find_get_entry_for_file_area 函数，只把0个page的file_area移动到global_file_stat.file_area_delete_list
	 * 链表。完全有可能file_area还有page呀，那就无法把file_area移动到global_file_stat.file_area_delete_list了。等该文件iput完成，
	 * 释放掉inode，这些file_area依然留存在原来的file_stat->temp、refault、多层warm链表，file_area->mapping已无效，文件inode、mapping已经释放了
	 * 2：即便iput()执行到find_get_entry_for_file_area 函数，遇到的都是0个page的file_area，都把这些file_area按照其file_area_delete成员
	 * 移动到global_file_stat.file_area_delete_list链表。然后iput()释放掉文件inode。但这些file_area依然按照其file_area_list成员
	 * 存在于file_stat->temp、refault、多层warm链表呀，但是这些file_area->mapping无效，因为文件inode、mapping已经释放了
	 *
	 * 这两种情况都会导致一个严重的后果，就是文件inode、mapping已经释放了，但是之前属于这个文件的file_area依然留存于
	 * file_stat->temp、refault、多层warm链表，这些file_area->mapping已经无效。等这些file_area因常见没访问而执行cold_file_area_delete()，
	 * 在该函数里，这些file_area此时肯定一个page都没有，因为该文件已经iput()释放了所有page。于是按照file_area->start_index从
	 * file_area->mapping指向的radix/xarray tree搜索file_area，然后执行 xas_store(xas，NULL)把搜索到的file_area从radix/xarray tree
	 * 剔除。就会出大问题，如果此时file_area->mapping指向的mapping内存，已经被其他slab内存分配，比如xfs_inode，dentry，xas_store(xas，NULL)
	 * 就是完全的无效内存操作了，会篡改xfs_inode，dentry内存里的数据，出现不可预料的问题。它TM容易出非法内存越界了，无语了。
	 * 如果file_area->mapping指向的mapping内存，比如mapping1，又被新的文件inode2、mapping2分配了。假设file_area->start_index是0。则
	 * cold_file_area_delete()时，xas_store(xas，NULL)就是按照索引0从xarry tree剔除mapping2的file_area0了。相当于错误把正在使用的
	 * inode2、mapping2，从xarray tree剔除了索引是0的file_area0。如果该file_area0还有page，如此inode2在iput()文件截断时，就无法从
	 * xarray tree搜索到file_area0了，也无法释放file_area0里边的page，后果就是iput()执行到evit()->clear_inode()，BUG_ON(inode->i_data.nrpages)
	 * 因为该inode的还有page，于是报错kernel BUG at fs/inode.c:606 而crash
	 *
	 * 怎么解决？
	 * 1:因为iput()截断文件pagecache执行到find_get_entry_for_file_area 函数，只把0个page的file_area移动到global_file_stat.file_area_delete_list，
	 * 针对还有page的file_area，最后肯定执行page_cache_delete_batch或page_cache_delete，释放掉page。于是要在这两个函数里
	 * 把file_area移动到global_file_stat.file_area_delete_list链表，标记file_area->mapping=NULL。不用担心二者会跟异步内存回收
	 * 线程的cold_file_area_delete()形成并发，因为只有file_area有page时才会执行这两个delete函数，释放page。因为file_area有page，
	 * 异步内存回收线程执行到cold_file_area_delete()最开头就会return，因为file_area还有page，肯定不能释放掉file_area。并且，两个delete
	 * 函数，释放file_area的page时，全程xas_lock加锁。而等二者释放掉page，因为此时iput()文件，mapping_exiting成立，会直接
	 * xas_store(xas,NULL)把file_area从xarray tree剔除，然后把file_area移动到global_file_stat.file_area_delete_list链表，再标记
	 * file_area->mapping=NULL。。而等异步内存回收线程获得xas_lock锁，xas_store(xas,NULL)返回NULL，就不会在cold_file_area_delete()
	 * 里释放file_area了。因为file_area已经移动到global_file_stat.file_area_delete_list链表，后续走cold_file_area_delete_quick()释放。
	 *
	 * 是否还存在其他并发问题呢？
	 * 1.1 iput()执行的find_get_entry_for_file_area()可能会跟kswapd内存回收执行的page_cache_delete()并发，把
	 * file_area移动到global_file_stat.file_area_delete_list链表，因为find_get_entry_for_file_area()里不是全程xas_lock加锁。
	 * 二者的并发就要靠global_file_stat.file_area_delete_lock加锁防护：先global_file_stat.file_area_delete_lock加锁，然后
	 * 判断file_area是否有in_mapping_delete标记，没有再把file_area移动到lobal_file_stat.file_area_delete_list链表，接着
	 * 设置file_area的in_mapping_delete标记。如果已经有了in_mapping_delete标记，就不再移动file_area了。
	 * 1.2 iput()执行的find_get_entry_for_file_area()标记file_area的in_mapping_delete标记，跟异步内存回收线程里
	 * cold_file_area_delete()释放该file_area，存在并发。这个没问题，这两个函数原始设计，就考虑了二者的并发，find_get_entry_for_file_area()
	 * 里标记file_area->mapping=NULL,以及xas_lock加锁后old_entry = xas_store(&xas_del, NULL)，old_entry非NULL才会把file_area
	 * 移动到global_file_stat.file_area_delete_list链表。cold_file_area_delete()函数里，if(NULL == file_area->mapping)直接return。
	 * 然后xas_lock加锁后old_entry = xas_store(&xas_del, NULL)，old_entry非NULL才会释放掉file_area结构。详细原理看
	 * cold_file_area_delete()上方的注释。
	 *
	 * 感慨，并发无处不在，并发问题太麻烦了。要发挥想象力，想象潜在的并发问题。
	 *
	 * 2:凡是移动到global_file_stat.file_area_delete_list链表的file_area,标记file_area->mapping=NULL后，还要标记file_area的
	 * in_mapping_delete标记，说明该文件iput了，inode释放了。此时该file_area一方面靠其file_area_delete成员添加到
	 * global_file_stat.file_area_delete_list链表，还靠file_area的file_area_list成员留存于file_stat->temp、free、warm等链表。
	 * 一个file_area同时处于两个链表就很危险。一方面，异步内存回收线程遍历global_file_stat.file_area_delete_list链表上的file_area
	 * 并cold_file_area_delete_quick()释放时，除了要把file_area从global_file_stat.file_area_delete_list链表剔除，还要把file_area
	 * 从file_stat->temp、free、warm等链表剔除。另一方面，异步内存回收线程从file_stat->temp、free、warm等链表遍历到有
	 * in_mapping_delete标记的file_area，要直接从file_stat->temp、free、warm等链表剔除，因为这个file_area此时还处于
	 * global_file_stat.file_area_delete_list链表。
	 * 
	 * 总之，一个被iput()的文件的file_area，必须立即被异步内存回收线程感知到，不能再用file_area->mapping的mapping去xas_store(xas,NULL)
	 * 从xarray tree剔除page。这个mapping可能已经新的文件的，那就错误把新的文件的file_area错误xarray tree剔除了。如果mapping内存
	 * 被其他slab内存分配走了，那xas_store(xas,NULL)就是内存踩踏了。
	 *
	 * 3:还有一点，find_get_entry_for_file_area 把file_area移动到global_file_stat.file_area_delete_list链表时，要先防护
	 * if(file_area->mapping != NULL)判断file_area->mapping是否是NULL，因为iput()过程，可能会多次执行find_get_entry_for_file_area()，
	 * 如果不过if(file_area->mapping != NULL)防护，就会把file_area多次list_add到global_file_stat.file_area_delete_list链表，如此就会扰乱该链表上的file_area
	 *
	 * 最后，再总结一下3处并发
	 * 1:iput()文件执行find_get_entries_for_file_area()跟异步内存回收线程执行cold_file_area_delete()的并发，这个已说过多次
	 * 2:iput()文件执行find_get_entries_for_file_area()跟kswapd内存回收执行page_cache_delete()的并发，二者都会把
	 * p_file_area->mapping置NULL，且把file_area移动到global_file_stat_delete链表。不会并发执行，kswapd内存回收先xas_lock，再
	 * 执行page_cache_delete()，此时file_area一定有page，iput()执行到find_get_entry_for_file_area()，因为file_area有page
	 * 就不会把file_area移动到global_file_stat_delete链表。况且，把file_area移动到global_file_stat_delete链表执行的
	 * move_file_area_to_global_delete_list()函数，已经防护了file_area重复移动到global_file_stat_delete链表。写代码就得
	 * 这样，得想办法从源头防护，从底层防护
	 * 3:异步内存回收线程执行cold_file_area_delete()跟kswapd内存回收执行page_cache_delete()的并发，前者把file_area移动到
	 * global_file_stat_delete链表，后者会释放掉file_area。二者不存在方法，因为前者是xas_lock，再执行page_cache_delete()，
	 * 此时file_area一定有page，而异步内存回收线程执行cold_file_area_delete()后，xas_lock后，因为file_area有page就直接
	 * return了。不对，有个潜在大隐患，page_cache_delete()后，mapping可能就被释放了呀，此时cold_file_area_delete()
	 * 里，xas_lock(mapping)，mapping就可能释放了呀，这是非法内存访问!不会，因为page_cache_delete()流程是
	 * xas_lock(mapping);file_area->mapping=NULL;smp_wmb();把file_area移动到global_file_stat_delete链表;释放mapping。
	 * cold_file_area_delete()流程是rcu_read_lock();if(file_area->mapping == NULL) return;xas_lock(mapping);
	 * if(file_area_have_page)return。有了rcu防护，cold_file_area_delete()里xas_lock(mapping)不用担心mapping
	 * 被释放，这个跟并发1细节一样。
	 * */

	/*当文件iput()后，执行该函数，遇到没有file_area的page，则要强制把xarray tree剔除。原因是：
	 * iput_final->evict->truncate_inode_pages_final->truncate_inode_pages->truncate_inode_pages_range->find_lock_entries
	 *调用到该函数，mapping_exiting(mapping)成立。当遇到没有page的file_area，要强制执行xas_store(&xas, NULL)把file_area从xarray tree剔除。
	 *因为此时file_area没有page，则从find_lock_entries()保存到fbatch->folios[]数组file_area的page是0个，则从find_lock_entries函数返回
	 *truncate_inode_pages_range后，因为fbatch->folios[]数组没有保存该file_area的page，则不会执行
	 *delete_from_page_cache_batch(mapping, &fbatch)->page_cache_delete_batch()，把这个没有page的file_area从xarray tree剔除。于是只能在
	 *truncate_inode_pages_range->find_lock_entries调用到该函数时，遇到没有page的file_area，强制把file_area从xarray tree剔除了*/
	//if(!file_area_have_page(*p_file_area) && mapping_exiting(mapping)){
	if((*p_file_area)->mapping && !file_area_have_page(*p_file_area) && mapping_exiting(mapping)){
		/*为了不干扰原有的xas，重新定义一个xas_del*/
#ifdef ASYNC_MEMORY_RECLAIM_FILE_AREA_TINY
		XA_STATE(xas_del, &mapping->i_pages, get_file_area_start_index(*p_file_area));

		/* p_file_area->pages[0/1]的bit63必须是file_area的索引，非0。而p_file_area->pages[2/3]必须是0，否则crash。心在不适用了
		 * 最新方案，file_area->pages[0/1]不是file_area_inde也不是shadow时，才会触发crash。file_area->pages[2/3]可能是NULL或者shasow，其他情况触发crash*/
		if(!folio_is_file_area_index_or_shadow((*p_file_area)->pages[0]) || !folio_is_file_area_index_or_shadow((*p_file_area)->pages[1]) 
				|| ((*p_file_area)->pages[2] && !(file_area_shadow_bit_set & (u64)((*p_file_area)->pages[2]))) || ((*p_file_area)->pages[3] && !(file_area_shadow_bit_set & (u64)((*p_file_area)->pages[3])))){
			for (int i = 0;i < PAGE_COUNT_IN_AREA;i ++)
				printk("pages[%d]:0x%llx\n",i,(u64)((*p_file_area)->pages[i]));

			panic("%s file_area:0x%llx pages[] error\n",__func__,(u64)p_file_area);
		}
#else		
		//XA_STATE(xas_del, &mapping->i_pages, (*p_file_area)->start_index >> PAGE_COUNT_IN_AREA_SHIFT);
		XA_STATE(xas_del, &mapping->i_pages, (*p_file_area)->start_index);
#endif	
		WRITE_ONCE((*p_file_area)->mapping, 0);
		smp_wmb();
		/*需要用文件xarray tree的lock加锁，因为xas_store()操作必须要xarray tree加锁*/
		xas_lock_irq(&xas_del);
		/*正常情况这里不可能成立，因为此时文件inode处于iput()，不会有进程访问产生新的page。
		  况且上边已经做了if(!file_area_have_page(*p_file_area))防护，以防万一还是spin_lock
		  加锁再防护一次，防止此时有进程并发读写该文件导致file_area分配了新的page*/
		if(file_area_have_page(*p_file_area))
			panic("%s file_area:0x%llx have page\n",__func__,(u64)(*p_file_area));
		old_entry = xas_store(&xas_del, NULL);
		xas_unlock_irq(&xas_del);

		/* 普通的文件file_stat，当iput()释放该文件inode时，会把file_stat移动到global delete链表，然后由异步内存回收线程
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
		 * global_file_stat.file_area_delete_list链表了。*/
		/*这个方案有了新问题，异步内存回收线程get_file_area_mmap_age()和cold_file_isolate_lru_pages_and_shrink()正在
		 *遍历file_area->pages[0、1]的page，但是同时这个file_area的文件被iput()了，然后find_get_entry_for_file_area()里
		 *执行move_file_area_to_global_delete_list()把file_area以file_area->pages[0、1]作为struct list_head file_area_delete
		 *移动到global_file_stat_delete链表，就会修改file_area->pages[0、1]，指向该file_area在global_file_stat_delete链表
		 *前后的file_area。总之，此时file_area->pages[0、1]保存的不再是folio指针，而是指向file_area。然后，get_file_area_mmap_age()
		 *和cold_file_isolate_lru_pages_and_shrink()函数里错把file_area->pages[0、1]当成folio指针，进行内存回收或folio_referenced()
		 *就会导致异常的内存踩踏了！解决办法就是iput()针对global_file_stat的文件file_area不再移动到global_file_stat_delete链表，
		 *只是标记set_file_area_in_mapping_delete，后续异步内存回收线程遇到这种file_area再移动到global_file_stat_delete链表。
		 *但是有一点要注意，要在old_entry = xas_store(&xas_del, NULL)把file_area从xarray tree剔除后，再执行
		 *set_file_area_in_mapping_delete(*p_file_area)，保证异步内存回收线程看到file_area有in_mapping_exit标记后，再把file_area
		 *移动到global_file_stat_delete链表，保证此时file_area已经被从xarray tree剔除掉*/	
		if(/*old_entry && */file_stat_in_global_base((struct file_stat_base *)mapping->rh_reserved1)){
			if(old_entry){
#if 0
				move_file_area_to_global_delete_list((struct file_stat_base *)mapping->rh_reserved1,*p_file_area);
#else
				/*注意，set_file_area_in_mapping_delete必须放到这里做，确保该file_area没有因为长时间没访问，被判定是冷file_area，
				 *而被异步内存回收线程主动执行cold_file_area_delete()释放掉*/
				set_file_area_in_mapping_delete(*p_file_area);
#endif		
			}else{
				/*如果old_entry是NULL，但file_area->mapping不是NULL则panic。不可能，因为上边已经清NULL了*/
				/*if(NULL != (*p_file_area)->mapping)
					panic("%s file_area:0x%llx file_area mapping NULL\n",__func__,(u64)p_file_area);*/
				
				printk("%s file_area:0x%llx old_entry NULL\n",__func__,(u64)p_file_area);
			}
				
		}else{
			/* 对p_file_area->file_area_delete链表赋值NULL,表示该file_area没有被移动到global_file_stat.file_area_delete_list链表。有问题，
			 * 到这里就不能再对file_area赋值了，因为file_area可能被异步内存回收线程释放了。错了有rcu_read_lock防护不用担心file_area
			 * 被释放，这个赋值移动到cold_file_area_delete函数了*/
            /*(*p_file_area)->file_area_delete.prev = NULL;
            (*p_file_area)->file_area_delete.next = NULL;*/
		}

		*page_offset_in_file_area = 0;
		/*goto retry分支里执行xas_find()，会自动令xas->xa_offset++，进而查找下一个索引的file_area*/
		goto retry;
	}
#if 0	
	/*如果file_area没有page，直接continue遍历下一个file_area，这段代码是否多余?????????????得额外判断file_area的索引是否超出最大值!*/
	if(!file_area_have_page(*p_file_area)){
		*page_offset_in_file_area = 0;
		goto retry;
	}
#endif	

find_page_from_file_area:
	if(*page_offset_in_file_area >= PAGE_COUNT_IN_AREA){
		panic("%s p_file_area:0x%llx page_offset_in_file_area:%d error\n",__func__,(u64)*p_file_area,*page_offset_in_file_area);
	}

	folio_index_from_xa_index = (xas->xa_index << PAGE_COUNT_IN_AREA_SHIFT) + *page_offset_in_file_area;
	//if(folio->index > max){
	if(folio_index_from_xa_index > max){
		FILE_AREA_PRINT("%s %s %d p_file_area:0x%llx max:%ld xas.xa_index:%ld page_offset_in_file_area:%d return NULL\n",__func__,current->comm,current->pid,(u64)*p_file_area,max,xas->xa_index,*page_offset_in_file_area);

		return NULL;
	}

	//folio = (*p_file_area)->pages[*page_offset_in_file_area];
	folio = rcu_dereference((*p_file_area)->pages[*page_offset_in_file_area]);
	/*如果fiolio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
	folio_is_file_area_index_or_shadow_and_clear_NULL(folio);
	FILE_AREA_PRINT("%s %s %d p_file_area:0x%llx file_area_state:0x%x folio:0x%llx xas.xa_index:%ld folio->index:%ld\n",__func__,current->comm,current->pid,(u64)*p_file_area,(*p_file_area)->file_area_state,(u64)folio,xas->xa_index,folio != NULL ?folio->index:-1);

	/*注意，原本是xas_find()函数里找到max索引的page时，返回NULL。还有一种情况，如果page索引不是4对齐，file_area的索引正好等于max，
	 *到这里时file_area->pages[]数组里的page正好就大于max。这两种情况都用 if(folio->index > max)判定。但是，不能因为folio是NULL
	 *就break。因为原版的find_get_entry()里folio = xas_find()返回的folio是NULL，然后返回NULL而结束查找page。此时因为xarray tree保存的是
	 *page。现在xarray tree保存的是file_area，只有p_file_area = xas_find()找到的file_area是NULL，才能返回NULL而结束查找page。
	 *现在，p_file_area = xas_find()返回的file_area不是NULL，但是可能里边的page时NULL，因为被回收了。不能因为file_area里有NULL page就
	 *return NULL而查找。而是要goto next_page去查找下一个page。为什么？这样才符合原本find_get_entry()函数里执行folio = xas_find()的
	 *查询原则：从起始索引开始查找page，遇到NULL page就继续向后查找，直到查找的page索引大于max*/
	//if(!folio || folio->index > max)
	if(!folio)
		goto next_folio;
#if 0
	//这段代码放到上边合适点，更贴合原版代码逻辑
	if((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + *page_offset_in_file_area > max /*folio->index > max*/){
		FILE_AREA_PRINT("%s p_file_area:0x%llx folio:0x%llx folio->index:%ld max:%ld xas.xa_index:%ld\n",__func__,(u64)*p_file_area,(u64)folio,folio->index,max,xas->xa_index);
		return NULL;
	}
#endif
	/*检测查找到的page是否正确，不是则crash*/
	//CHECK_FOLIO_FROM_FILE_AREA_VALID(xas,folio,*p_file_area,*page_offset_in_file_area,folio_index_from_xa_index);

	if (!folio_try_get_rcu(folio))
		goto reset;

	//if (unlikely(folio != xas_reload(xas))) {
	if (unlikely(folio != rcu_dereference((*p_file_area)->pages[*page_offset_in_file_area]))) {
		folio_put(folio);
		goto reset;
	}
	CHECK_FOLIO_FROM_FILE_AREA_VALID(xas,mapping,folio,*p_file_area,*page_offset_in_file_area,folio_index_from_xa_index);

next_folio:
	*page_offset_in_file_area = *page_offset_in_file_area + 1;
	/*如果page_offset_in_file_area是4，说明一个file_area里的page都被获取过了，如果下次再执行该函数，必须执行xas_find(xas, max)
	 *获取新的file_area。于是令*p_file_area=NULL, *page_offset_in_file_area清0。这样下次执行该函数，才会执行xas_find(xas, max)
	 *查找下一个file_area，并从这个file_area的第一个page开始*/
	if(*page_offset_in_file_area == PAGE_COUNT_IN_AREA){
		*p_file_area = NULL;
		*page_offset_in_file_area = 0;
		/*如果此时folio是NULL，不能在下边return folio返回NULL，这会结束page查找。而要goto folio分支，去执行
		 *p_file_area = xas_find(xas, file_area_max)去查找下一个file_area，然后获取这个新的file_area的page。这个函数能直接return的只有3种情况
		 *1:查找的file_area索引大于file_area_max(if(!*p_file_area)那里) 2：查找到的page索引大于大于max(if(folio->index > max)那里) 3：查找到了有效page(即return folio)*/
		if(!folio)
			goto retry;
	}
	else{
		/*去查找当前file_area的下一个page。但只有folio是NULL的情况下!!!!!!!如果folio是合法的，直接return folio返回给调用者。
		 *然后调用者下次执行该函数，因为 *p_file_area 不是NULL，直接获取这个file_area的下一个page*/
		if(!folio)
			goto find_page_from_file_area;
	}
	return folio;
reset:
	/*xas_reset()会xas->xa_node = XAS_RESTART，然后goto retry时执行xas_find()时，直接执行entry = xas_load(xas)重新获取当前索引的file_area。
	 *如果xas->xa_node不是XAS_RESTART，那xas_find()里是先执行xas_next_offset(xas)令xas->xa_offset加1、xas->xa_index加1，然后去查询
	 *下一个索引file_area。简单说，一个是重新查询当前索引的file_area，一个是查询下一个索引的file_area*/
	xas_reset(xas);
	goto retry;
}
EXPORT_SYMBOL(find_get_entry_for_file_area);
unsigned find_get_entries_for_file_area(struct address_space *mapping, pgoff_t start,
		pgoff_t end, struct folio_batch *fbatch, pgoff_t *indices)
{
	//XA_STATE(xas, &mapping->i_pages, start);
	XA_STATE(xas, &mapping->i_pages, start >> PAGE_COUNT_IN_AREA_SHIFT);
	struct folio *folio;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = start & PAGE_COUNT_IN_AREA_MASK;
	/*必须赋初值NULL，表示file_area无效，这样find_get_entry_for_file_area()里才会xas_find()查找*/
	struct file_area *p_file_area = NULL;
	struct file_stat_base *p_file_stat_base;

	FILE_AREA_PRINT("%s %s %d mapping:0x%llx start:%ld end:%ld\n",__func__,current->comm,current->pid,(u64)mapping,start,end);
	
	rcu_read_lock();
	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	while ((folio = find_get_entry_for_file_area(&xas, end, XA_PRESENT,&p_file_area,&page_offset_in_file_area,mapping)) != NULL) {
		//indices[fbatch->nr] = xas.xa_index; xax.xa_index现在代表的是file_area索引，不是page索引
		indices[fbatch->nr] = folio->index;
		if (!folio_batch_add(fbatch, folio))
			break;
	}
	rcu_read_unlock();

	return folio_batch_count(fbatch);
}
EXPORT_SYMBOL(find_get_entries_for_file_area);
unsigned find_lock_entries_for_file_area(struct address_space *mapping, pgoff_t start,
		pgoff_t end, struct folio_batch *fbatch, pgoff_t *indices)
{
	//XA_STATE(xas, &mapping->i_pages, start);
	XA_STATE(xas, &mapping->i_pages, start >> PAGE_COUNT_IN_AREA_SHIFT);
	struct folio *folio;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = start & PAGE_COUNT_IN_AREA_MASK;
	/*必须赋初值NULL，表示file_area无效，这样find_get_entry_for_file_area()里才会xas_find()查找*/
	struct file_area *p_file_area = NULL;
	struct file_stat_base *p_file_stat_base;

	FILE_AREA_PRINT("%s %s %d mapping:0x%llx start:%ld end:%ld\n",__func__,current->comm,current->pid,(u64)mapping,start,end);
	
	rcu_read_lock();
	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	while ((folio = find_get_entry_for_file_area(&xas, end, XA_PRESENT,&p_file_area,&page_offset_in_file_area,mapping))) {
		if (!xa_is_value(folio)) {
			if (folio->index < start)
				goto put;
			if (folio->index + folio_nr_pages(folio) - 1 > end)
				goto put;
			if (!folio_trylock(folio))
				goto put;
			if (folio->mapping != mapping ||
					folio_test_writeback(folio))
				goto unlock;
			//VM_BUG_ON_FOLIO(!folio_contains(folio, xas.xa_index),
			VM_BUG_ON_FOLIO(!folio_contains(folio, folio->index),
					folio);
		}
		//indices[fbatch->nr] = xas.xa_index;
		indices[fbatch->nr] = folio->index;
		if (!folio_batch_add(fbatch, folio))
			break;
		continue;
unlock:
		folio_unlock(folio);
put:
		folio_put(folio);
	}
	rcu_read_unlock();

	return folio_batch_count(fbatch);
}
EXPORT_SYMBOL(find_lock_entries_for_file_area);
unsigned find_get_pages_range_for_file_area(struct address_space *mapping, pgoff_t *start,
		pgoff_t end, unsigned int nr_pages,
		struct page **pages)
{
	//XA_STATE(xas, &mapping->i_pages, *start);
	XA_STATE(xas, &mapping->i_pages, *start >> PAGE_COUNT_IN_AREA_SHIFT);
	struct folio *folio;
	unsigned ret = 0;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = *start & PAGE_COUNT_IN_AREA_MASK;
	/*必须赋初值NULL，表示file_area无效，这样find_get_entry_for_file_area()里才会xas_find()查找*/
	struct file_area *p_file_area = NULL;
	struct file_stat_base *p_file_stat_base;

	if (unlikely(!nr_pages))
		return 0;

	FILE_AREA_PRINT("%s %s %d mapping:0x%llx start:%ld end:%ld nr_pages:%d\n",__func__,current->comm,current->pid,(u64)mapping,*start,end,nr_pages);

	rcu_read_lock();
	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	while ((folio = find_get_entry_for_file_area(&xas, end, XA_PRESENT,&p_file_area,&page_offset_in_file_area,mapping))) {
		/* Skip over shadow, swap and DAX entries */
		if (xa_is_value(folio))
			continue;

		//again:
		if(folio_nr_pages(folio) > 1)
			panic("%s folio:0x%llx folio_nr_pages > 1 %ld\n",__func__,(u64)folio,folio_nr_pages(folio));

		//pages[ret] = folio_file_page(folio, xas.xa_index);
		pages[ret] = folio_file_page(folio, folio->index);
		if (++ret == nr_pages) {
			//*start = xas.xa_index + 1;
			*start = folio->index + 1;
			goto out;
		}
		/*
		   if (folio_more_pages(folio, xas.xa_index, end)) {
		   xas.xa_index++;
		   folio_ref_inc(folio);
		   goto again;
		   }*/
	}

	/*
	 * We come here when there is no page beyond @end. We take care to not
	 * overflow the index @start as it confuses some of the callers. This
	 * breaks the iteration when there is a page at index -1 but that is
	 * already broken anyway.
	 */
	if (end == (pgoff_t)-1)
		*start = (pgoff_t)-1;
	else
		*start = end + 1;
out:
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL(find_get_pages_range_for_file_area);
unsigned find_get_pages_contig_for_file_area(struct address_space *mapping, pgoff_t index,
			       unsigned int nr_pages, struct page **pages)
{
	//XA_STATE(xas, &mapping->i_pages, index);
	XA_STATE(xas, &mapping->i_pages, index >> PAGE_COUNT_IN_AREA_SHIFT);
	struct folio *folio;
	unsigned int ret = 0;

	struct file_area *p_file_area;
	struct file_stat_base *p_file_stat_base;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = index & PAGE_COUNT_IN_AREA_MASK;

	if (unlikely(!nr_pages))
		return 0;

	FILE_AREA_PRINT("%s mapping:0x%llx index:%ld nr_pages:%d\n",__func__,(u64)mapping,index,nr_pages);

	rcu_read_lock();
	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	//for (folio = xas_load(&xas); folio; folio = xas_next(&xas)) {
	for (p_file_area = xas_load(&xas); p_file_area; p_file_area = xas_next(&xas)) {

		//if(!p_file_area || !is_file_area_entry(p_file_area)) 为了提升性能，这些判断去掉
		//	panic("%s mapping:0x%llx p_file_area:0x%llx NULL\n",__func__,(u64)mapping,(u64)p_file_area);

		/*xas_retry()里有xas->xa_node = XAS_RESTART，这个隐藏的很深，这样执行xas_next(&xas)时，if(xas_not_node(node))成立，直接从
		 *xarray tree按照老的xas->xa_index重新查找，不会再执行xas->xa_index++和xas->xa_offset++而从父节点直接取出成员了*/
		//if (xas_retry(&xas, folio))
		if (xas_retry(&xas, p_file_area))
			continue;

		/*
		 * If the entry has been swapped out, we can stop looking.
		 * No current caller is looking for DAX entries.
		 */
		//if (xa_is_value(folio))
		//	break;

		if(xa_is_value(p_file_area) || xa_is_sibling(p_file_area))
			panic("%s p_file_area:0x%llx error\n",__func__,(u64)p_file_area);

		p_file_area = entry_to_file_area(p_file_area);

find_page_from_file_area:
		//folio = p_file_area->pages[page_offset_in_file_area];
		folio = rcu_dereference(p_file_area->pages[page_offset_in_file_area]);
		/*如果fiolio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
		folio_is_file_area_index_or_shadow_and_clear_NULL(folio);
		/*这个跟filemap_get_read_batch()里for (folio = xas_load(&xas); folio; folio = xas_next(&xas))判断出folio是NULL则结束循环是一个效果*/
		if(!folio)
			break;

		/*检测查找到的page是否正确，不是则crash*/
		//CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,folio,p_file_area,page_offset_in_file_area,((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area));

		/*如果获取的page引用计数是0，说明已经被其他进程释放了。则直接goto retry从xarray tree按照老的xas.xa_index重新查找
		 *file_area，然后查找page。其实没有必要重新查找file_area，直接goto find_page_from_file_area重新获取page就行了!!!!!!!!!!*/
		if (!folio_try_get_rcu(folio))
			goto retry;

		//if (unlikely(folio != xas_reload(&xas)))
		if (unlikely(folio != rcu_dereference(p_file_area->pages[page_offset_in_file_area]))){ 
			/*当前page获取失败，把folio_put(folio)释放引用计数放到这里，然后goto next_folio分支，直接获取下一个page*/
			folio_put(folio);
			//goto put_page;
			goto next_folio;
		}
		CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,mapping,folio,p_file_area,page_offset_in_file_area,((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area));

		//again:
		//pages[ret] = folio_file_page(folio, xas.xa_index);
		pages[ret] = folio_file_page(folio, (xas.xa_index + page_offset_in_file_area));
		if (++ret == nr_pages)
			break;

		/*if (folio_more_pages(folio, xas.xa_index, ULONG_MAX)) {
		  xas.xa_index++;
		  folio_ref_inc(folio);
		  goto again;
		  }*/
		if(folio_nr_pages(folio) > 1){
			panic("%s folio:0x%llx folio_nr_pages:%ld\n",__func__,(u64)folio,folio_nr_pages(folio));
		}
next_folio:
		page_offset_in_file_area ++;

		/*如果file_area里还有page没遍历到，goto find_page_from_file_area去查找file_area里的下一个page。否则到for循环开头
		 *xas_for_each()去查找下一个file_area，此时需要find_page_from_file_area清0，这个很关键*/
		if(page_offset_in_file_area < PAGE_COUNT_IN_AREA){
			/*page_offset_in_file_area加1不能放到这里，重大逻辑错误。比如，上边判断page_offset_in_file_area是3的folio，
			 *然后执行到f(page_offset_in_file_area < PAGE_COUNT_IN_AREA)判断时，正常就应该不成立的，因为file_area的最后一个folio已经遍历过了*/
			//page_offset_in_file_area ++;
			goto find_page_from_file_area;
		}
		else{
			//要查找下一个file_area了，page_offset_in_file_area要清0
			page_offset_in_file_area = 0;
		}

		continue;
		//put_page:这段代码移动到上边了
		//		folio_put(folio);
retry:
		xas_reset(&xas);
	}
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL(find_get_pages_contig_for_file_area);
unsigned find_get_pages_range_tag_for_file_area(struct address_space *mapping, pgoff_t *index,
		pgoff_t end, xa_mark_t tag, unsigned int nr_pages,
		struct page **pages)
{
	XA_STATE(xas, &mapping->i_pages, *index >> PAGE_COUNT_IN_AREA_SHIFT);
	struct folio *folio;
	unsigned ret = 0;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = *index & PAGE_COUNT_IN_AREA_MASK;
	/*必须赋初值NULL，表示file_area无效，这样find_get_entry_for_file_area()里才会xas_find()查找*/
	struct file_area *p_file_area = NULL;
	struct file_stat_base *p_file_stat_base;

	if (unlikely(!nr_pages))
		return 0;

	FILE_AREA_PRINT("%s %s %d mapping:0x%llx index:%ld nr_pages:%d end:%ld tag:%d page_offset_in_file_area:%d xas.xa_index:%ld\n",__func__,current->comm,current->pid,(u64)mapping,*index,nr_pages,end,tag,page_offset_in_file_area,xas.xa_index);

	rcu_read_lock();
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	while ((folio = find_get_entry_for_file_area(&xas, end, tag,&p_file_area,&page_offset_in_file_area,mapping))) {
		/*
		 * Shadow entries should never be tagged, but this iteration
		 * is lockless so there is a window for page reclaim to evict
		 * a page we saw tagged.  Skip over it.
		 */
		if (xa_is_value(folio))
			continue;

		if(folio_nr_pages(folio) > 1)
			panic("%s folio:0x%llx folio_nr_pages > 1 %ld\n",__func__,(u64)folio,folio_nr_pages(folio));

		pages[ret] = &folio->page;
		if (++ret == nr_pages) {
			*index = folio->index + folio_nr_pages(folio);
			goto out;
		}
	}

	/*
	 * We come here when we got to @end. We take care to not overflow the
	 * index @index as it confuses some of the callers. This breaks the
	 * iteration when there is a page at index -1 but that is already
	 * broken anyway.
	 */
	if (end == (pgoff_t)-1)
		*index = (pgoff_t)-1;
	else
		*index = end + 1;
out:
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL(find_get_pages_range_tag_for_file_area);
void filemap_get_read_batch_for_file_area(struct address_space *mapping,
		pgoff_t index, pgoff_t max, struct folio_batch *fbatch)
{
	//XA_STATE(xas, &mapping->i_pages, index);
	XA_STATE(xas, &mapping->i_pages, index>>PAGE_COUNT_IN_AREA_SHIFT);
	struct folio *folio = NULL;
    
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = index & PAGE_COUNT_IN_AREA_MASK;
	struct file_stat_base *p_file_stat_base;
	struct file_area *p_file_area = NULL;
	//unsigned int page_offset_in_file_area_origin = page_offset_in_file_area;
	unsigned long folio_index_from_xa_index;
	
	rcu_read_lock();
	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();

	//for (folio = xas_load(&xas); folio; folio = xas_next(&xas)) {
	for (p_file_area = xas_load(&xas); p_file_area; p_file_area = xas_next(&xas)) {

		/*之前得做if (xas_retry(&xas, folio))等3个if判断，现在只用做if(!is_file_area_entry(p_file_area))判断就行了。到这里
		 *的p_file_area一定不是NULL，不用做这个防护*/
		if(!is_file_area_entry(p_file_area)){

		    /*xas_retry()里有xas->xa_node = XAS_RESTART，这个隐藏的很深，这样执行xas_next(&xas)时，if(xas_not_node(node))成立，直接从
		     *xarray tree按照老的xas->xa_index重新查找，不会再执行xas->xa_index++和xas->xa_offset++而从父节点直接获取下一个索引的成员了*/
		    if (xas_retry(&xas, p_file_area))
			    continue;
			
			panic("%s mapping:0x%llx p_file_area:0x%llx error!!!!!!!!!!!!!!!!!!!!!!!!1\n",__func__,(u64)mapping,(u64)p_file_area);
            if(xa_is_value(p_file_area))
				break;
			if (xa_is_sibling(p_file_area))
				break;
        }
#if 0	
		if (xas_retry(&xas, folio))
			continue;
		/*if(xas.xa_index > max)判断放到下边了，因为这里只能file_area的索引，不能判断page的索引。
		 *另外两个判断放到一起，其实这两个判断可以放到__filemap_add_folio()里，在保存file_area到xarray tree时就判断，在查询时不再判断*/
		if (xas.xa_index > max || xa_is_value(folio))
	    		break;
		if (xa_is_sibling(folio))
			break;
        
        if(xa_is_sibling(p_file_area))
			break;
#endif
		p_file_area = entry_to_file_area(p_file_area);

		
#ifdef ASYNC_MEMORY_RECLAIM_DEBUG
		FILE_AREA_PRINT("%s mapping:0x%llx p_file_area:0x%llx xas.xa_index:%ld xas->xa_offset:%d xa_node_cache:0x%llx cache_base_index:%ld index:%ld\n",__func__,(u64)mapping,(u64)p_file_area,xas.xa_index,xas.xa_offset,(u64)p_file_stat_base->xa_node_cache,p_file_stat_base->xa_node_cache_base_index,index);
#else
		FILE_AREA_PRINT("%s mapping:0x%llx p_file_area:0x%llx xas.xa_index:%ld xas->xa_offset:%d index:%ld\n",__func__,(u64)mapping,(u64)p_file_area,xas.xa_index,xas.xa_offset,index);
#endif		

find_page_from_file_area:
		//folio = p_file_area->pages[page_offset_in_file_area];
		folio = rcu_dereference(p_file_area->pages[page_offset_in_file_area]);
		/*如果fiolio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
		folio_is_file_area_index_or_shadow_and_clear_NULL(folio);
		/*这个跟filemap_get_read_batch()里for (folio = xas_load(&xas); folio; folio = xas_next(&xas))判断出folio是NULL则结束循环是一个效果*/
		if(!folio)
			break;

		folio_index_from_xa_index = (xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area;
		/*检测查找到的page是否正确，不是则crash*/
		//CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,folio,p_file_area,page_offset_in_file_area,folio_index_from_xa_index);

		/*查找的page超过最大索引*/
		//if(folio->index > max /*xas.xa_index + page_offset_in_file_area > max*/)
		if(folio_index_from_xa_index > max )
			break;

        /*如果获取的page引用计数是0，说明已经被其他进程释放了。则直接goto retry从xarray tree按照老的xas.xa_index重新查找
		 *file_area，然后查找page。其实没有必要重新查找file_area，直接goto find_page_from_file_area重新获取page就行了!!!!!!!!!!*/
		if (!folio_try_get_rcu(folio)){
            printk("%s mapping:0x%llx folio:0x%llx index:%ld !folio_try_get_rcu(folio)\n",__func__,(u64)mapping,(u64)folio,folio->index);
			goto retry;//goto find_page_from_file_area;
		}

		//if (unlikely(folio != xas_reload(&xas)))
	    if (unlikely(folio != rcu_dereference(p_file_area->pages[page_offset_in_file_area]))){
            printk("%s mapping:0x%llx folio:0x%llx index:%ld folio != rcu_dereference(p_file_area->pages[page_offset_in_file_area]\n",__func__,(u64)mapping,(u64)folio,folio->index);
			/*当前page获取失败，把folio_put(folio)释放引用计数放到这里，然后goto next_folio分支，直接获取下一个page。这个思路错了。
			 *原版filemap_get_read_batch()函数在重新获取page异常后，是重新去xarray tree查找page，这里也要goto put_folio，
			 *然后执行xas_reset(&xas)重置xas，然后按照当前xas->xa_index和xas->xa_offset重新查找file_area，
			 再按照当前page_offset_in_file_area重新查找page。要理解 filemap_get_read_batch()函数查找page的原则，遇到非法page
			 要么尝试重新查找，要么立即break，不会一直向后查找而找到超出最大索引而break。这点跟find_get_entrie()原理不一样*/
			
			goto put_folio;
		    //folio_put(folio);
			//goto next_folio;
        }
		CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,mapping,folio,p_file_area,page_offset_in_file_area,folio_index_from_xa_index);

        FILE_AREA_PRINT("%s mapping:0x%llx folio:0x%llx index:%ld page_offset_in_file_area:%d\n",__func__,(u64)mapping,(u64)folio,folio->index,page_offset_in_file_area);

		if (!folio_batch_add(fbatch, folio))
			break;
		/*执行到这里，才真正获取到当前folio，然后才能令page_offset_in_file_area加1。但为了兼容还是加1放到next_folio那里了。
		 *但是在if (!folio_test_uptodate(folio))和if (folio_test_readahead(folio))两个成功获取page但break终止的分支都额外添加加1了*/
		//page_offset_in_file_area ++;
		if (!folio_test_uptodate(folio)){
			page_offset_in_file_area ++;
			break;
		}
		if (folio_test_readahead(folio)){
			page_offset_in_file_area ++;
			break;
		}
#if 0/*这个早期的调试信息先去掉*/
        if(folio_nr_pages(folio) > 1){
            panic("%s index:%ld folio_nr_pages:%ld!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,index,folio_nr_pages(folio));
		}
#endif		
        /*folio代表单个page时，看着本质是xas->xa_index = folio->index，xas->xa_offset= folio->index & XA_CHUNK_MASK。
		 *这里的核心操作是，当folio->index大于64时，folio->index & XA_CHUNK_MASK后只取出不足64的部分，即在xarray tree槽位的偏移.
		 *但是folio = xas_next(&xas)里会判断出xas->xa_offset == 63后，会自动取下一个父节点查找page*/		
		//xas_advance(&xas, folio->index + folio_nr_pages(folio) - 1);
        
		page_offset_in_file_area ++;

		/*如果file_area里还有page没遍历到，goto find_page_from_file_area去查找file_area里的下一个page。否则到for循环开头
		 *xas_for_each()去查找下一个file_area，此时需要find_page_from_file_area清0，这个很关键*/
		if(page_offset_in_file_area < PAGE_COUNT_IN_AREA){
			/*page_offset_in_file_area加1不能放到这里，重大逻辑错误。比如，上边判断page_offset_in_file_area是3的folio，
			 *然后执行到f(page_offset_in_file_area < PAGE_COUNT_IN_AREA)判断时，正常就应该不成立的，因为file_area的最后一个folio已经遍历过了*/
			//page_offset_in_file_area ++;
			goto find_page_from_file_area;
		}
		else{
			/* file_area的page都被访问完，则更新file_area的age和access_count。但新版本不管file_area的page被访问几个，
			 * 都视file_area访问一次，主要是为了降低性能损耗*/
			hot_file_update_file_status(mapping,p_file_stat_base,p_file_area,FILE_AREA_PAGE_IS_READ);

			//要查找下一个file_area了，page_offset_in_file_area要清0
			page_offset_in_file_area = 0;
		}

		continue;
put_folio:
		folio_put(folio);
retry:
		xas_reset(&xas);
	}
	
	/*如果前边for循环异常break了，就无法统计最后file_area的访问计数了，那就在这里统计。
	  但只有page_offset_in_file_area大于0，才能执行hot_file_update_file_status()。page_offset_in_file_area
	  非0说明上边的for循环是file_area的page没有遍历完就异常break了，没有对page_offset_in_file_area清0，
	  也就没有在上边的for循环执行hot_file_update_file_status()更新file_area的age和access_count。但是有
	  个特殊情况，初始值page_offset_in_file_area是1，然后上边遍历file_area时的第一个page时，因为page内存
	  被回收了，导致上边异常break，这个page这里并没有被访问，但是这里的if还会成立并更新file_area的age和
	  access_count。其实也没啥，这个page只是没有访问成功，但毕竟还是访问了。*/
	if(p_file_area && page_offset_in_file_area)
		hot_file_update_file_status(mapping,p_file_stat_base,p_file_area,FILE_AREA_PAGE_IS_READ);

	rcu_read_unlock();
}
EXPORT_SYMBOL(filemap_get_read_batch_for_file_area);
/*以下函数因为依赖原始文件的其他函数，不在这里单独列了*/
#if 0
loff_t mapping_seek_hole_data_for_file_area(struct address_space *mapping, loff_t start,
		loff_t end, int whence)
{
	//XA_STATE(xas, &mapping->i_pages, start >> PAGE_SHIFT);
	XA_STATE(xas, &mapping->i_pages, (start >> PAGE_SHIFT) >> PAGE_COUNT_IN_AREA_SHIFT);
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = (start >> PAGE_SHIFT) & PAGE_COUNT_IN_AREA_MASK;
	pgoff_t max = (end - 1) >> PAGE_SHIFT;
	bool seek_data = (whence == SEEK_DATA);
	struct folio *folio;
	struct file_area *p_file_area = NULL;

	if (end <= start)
		return -ENXIO;

	rcu_read_lock();
	//while ((folio = find_get_entry(&xas, max, XA_PRESENT))) {
	while ((folio = find_get_entry_for_file_area(&xas, max, XA_PRESENT,&p_file_area,&page_offset_in_file_area,mapping))) {
		//loff_t pos = (u64)xas.xa_index << PAGE_SHIFT;
		loff_t pos = (((u64)xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area) << PAGE_SHIFT;
		size_t seek_size;

		printk("%s %s %d p_file_area:0x%llx file_area_state:0x%x folio:0x%llx xas.xa_index:%ld page_offset_in_file_area:%d folio->index:%ld\n",__func__,current->comm,current->pid,(u64)p_file_area, p_file_area != NULL ? p_file_area->file_area_state:-1,(u64)folio,xas.xa_index,page_offset_in_file_area,folio->index);

		if (start < pos) {
			if (!seek_data)
				goto unlock;
			start = pos;
		}
		/*seek_folio_size()会会判断xa_is_value(folio)，这里提前判断，是则crash*/
		if (xa_is_value(folio))
			panic("%s %s %d mapping:0x%llx p_file_area:0x%llx folio:0x%llx xa_is_value error\n",__func__,current->comm,current->pid,(u64)mapping,(u64)p_file_area,(u64)folio);

		/*本质就是一个page的大小,4K*/
		seek_size = seek_folio_size(&xas, folio);
		pos = round_up((u64)pos + 1, seek_size);
		/*这个函数看着不用动，保持原样*/
		start = folio_seek_hole_data(&xas, mapping, folio, start, pos,
				seek_data);
		if (start < pos)
			goto unlock;
		if (start >= end)
			break;
		//if (seek_size > PAGE_SIZE)
		//	xas_set(&xas, pos >> PAGE_SHIFT);
		if (seek_size > PAGE_SIZE){
			/* 要把最新的pos文件地址除以4转换成file_area的索引，然后保存到xas.xa_index。还要把pos不足4的部分更新到
			 * page_offset_in_file_area，然后执行find_get_entry_for_file_area()才会按照最新的pos索引查找page*/
			xas_set(&xas, (pos >> PAGE_SHIFT) >> PAGE_COUNT_IN_AREA_SHIFT);
			page_offset_in_file_area = (pos >> PAGE_SHIFT) & PAGE_COUNT_IN_AREA_MASK;
		}
		if (!xa_is_value(folio))
			folio_put(folio);
	}
	if (seek_data)
		start = -ENXIO;
unlock:
	rcu_read_unlock();
	if (folio && !xa_is_value(folio))
		folio_put(folio);
	if (start > end)
		return end;
	return start;
}

//static struct folio *next_uptodate_page_for_file_area(struct folio *folio,
static struct folio *next_uptodate_page_for_file_area(struct file_area **p_file_area_ori,
		struct address_space *mapping,
		struct xa_state *xas, pgoff_t end_pgoff,unsigned int *page_offset_in_file_area,int get_page_from_file_area)
{
	unsigned long max_idx;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area_temp = *page_offset_in_file_area;
	struct folio *folio;
	struct file_area *p_file_area = *p_file_area_ori;
	unsigned long folio_index_from_xa_index = 0;

	FILE_AREA_PRINT("1:%s %s %d xas.xa_index:%ld page_offset_in_file_area_temp:%d get_page_from_file_area:%d end_pgoff:%ld\n",__func__,current->comm,current->pid,xas->xa_index,page_offset_in_file_area_temp,get_page_from_file_area,end_pgoff);

	/*file_area还有剩下page没有遍历完，直接goto find_page_from_file_area获取剩下的page*/
	if(get_page_from_file_area)
		goto next_folio;
		//goto find_page_from_file_area;

	do {
		//if (!folio)
		if (!p_file_area)
			return NULL;
		/*xas_retry()里会重置xas.xa_node=XAS_RESTART，continue后xas_next_entry()按照当前索再查找一下file_area*/
		//if (xas_retry(xas, folio))
		if (xas_retry(xas, p_file_area))
			continue;
		//if (xa_is_value(folio))
		//	continue;
		if (xa_is_value(p_file_area) || !is_file_area_entry(p_file_area))
			panic("1:%s %s %d mapping:0x%llx p_file_area:0x%llx file_area_state:0x%x error\n",__func__,current->comm,current->pid,(u64)mapping,(u64)p_file_area,p_file_area->file_area_state);

		p_file_area = entry_to_file_area(p_file_area);

find_page_from_file_area:
		if(page_offset_in_file_area_temp >= PAGE_COUNT_IN_AREA)
			panic("2:%s %s %d mapping:0x%llx p_file_area:0x%llx page_offset_in_file_area_temp:%d error\n",__func__,current->comm,current->pid,(u64)mapping,(u64)p_file_area,page_offset_in_file_area_temp);

		folio_index_from_xa_index = (xas->xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area_temp;
		/*原函数在xas_next_entry()里判断要查找的page索引是否超出end_pgoff，超出的话就退出循环。这里因为
		 *xas_next_entry()查找的是file_area，故在这里要专门判断超找的page索引是否超出end_pgoff*/
		//if((xas->xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area_temp > end_pgoff){
		if(folio_index_from_xa_index > end_pgoff){
			FILE_AREA_PRINT("2:%s %s %d p_file_area:0x%llx file_area_state:0x%x xas.xa_index:%ld page_offset_in_file_area_temp:%d return NULL\n",__func__,current->comm,current->pid,(u64)p_file_area,p_file_area->file_area_state,xas->xa_index,page_offset_in_file_area_temp);

			return NULL;
		}

		//folio = p_file_area->pages[page_offset_in_file_area_temp];
		folio = rcu_dereference(p_file_area->pages[page_offset_in_file_area_temp]);
		/*如果fiolio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
		folio_is_file_area_index_or_shadow_and_clear_NULL(folio);
		FILE_AREA_PRINT("3:%s %s %d p_file_area:0x%llx file_area_state:0x%x folio:0x%llx xas.xa_index:%ld page_offset_in_file_area_temp:%d folio->index:%ld\n",__func__,current->comm,current->pid,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,xas->xa_index,page_offset_in_file_area_temp,folio != NULL ?folio->index:-1);

		if(!folio)
			goto next_folio;

		/*检测查找到的page是否正确，不是则crash*/
		//CHECK_FOLIO_FROM_FILE_AREA_VALID(xas,folio,p_file_area,page_offset_in_file_area_temp,folio_index_from_xa_index);

		if (folio_test_locked(folio))
			goto next_folio;
		//continue;不能continue，此时是去查找下一个file_area了，要goto next_folio查找file_area里的下一个page
		if (!folio_try_get_rcu(folio))
			goto next_folio;
		//continue;
		/* Has the page moved or been split? */
		//if (unlikely(folio != xas_reload(xas)))
		if (unlikely(folio != rcu_dereference(p_file_area->pages[page_offset_in_file_area_temp]))) 
			goto skip;
		if (!folio_test_uptodate(folio) || folio_test_readahead(folio))
			goto skip;
		if (!folio_trylock(folio))
			goto skip;
		if (folio->mapping != mapping)
			goto unlock;
		if (!folio_test_uptodate(folio))
			goto unlock;
		max_idx = DIV_ROUND_UP(i_size_read(mapping->host), PAGE_SIZE);

		CHECK_FOLIO_FROM_FILE_AREA_VALID(xas,mapping,folio,p_file_area,page_offset_in_file_area_temp,folio_index_from_xa_index);

		/*隐藏很深的问题:原函数在xas_next_entry()里判断要查找的page索引是否超出max_idx，超出的话就goto unlock。这里因为
		 *xas_next_entry()查找的是file_area，故在这里要专门判断超找的page索引是否超出max_idx*/
		//if (xas->xa_index >= max_idx)
		//if (((xas->xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area_temp) >= max_idx)
		if (folio_index_from_xa_index >= max_idx)
			goto unlock;

#if 0
		/*page_offset_in_file_area保存当前查找到的page在file_area的索引，下次filemap_map_pages再次执行next_map_page()
		 *时，直接令page_offset_in_file_area加1而从file_area查找到下一个索引的page，不用再查找xarray tree得到page。但是有个
		 *前提，page_offset_in_file_area_temp必须小于3。因为如果page_offset_in_file_area_temp是3，说明当前file_area里的
		 page都遍历过了，下次再执行filemap_map_pages->next_map_page()时，必须从xarray tree查找新的下一个索引的file_area了，
		 *此时就要*page_offset_in_file_area = 0清0，表示从新的file_area的第一个page开始查找。并且还要把p_file_area_ori清NULL，
		 *令上一次传入的file_area失效，这样filemap_map_pages->next_map_page()才会查找新的file_area*/
		if(page_offset_in_file_area_temp < (PAGE_COUNT_IN_AREA -1)){
			/*page_offset_in_file_area_temp加1再赋值，下次执行该函数才会从file_area的下一个page开始查找*/
			*page_offset_in_file_area = page_offset_in_file_area_temp + 1;
			if(p_file_area != *p_file_area_ori)
				*p_file_area_ori = p_file_area;
		}
		else{
			*page_offset_in_file_area = 0;
			*p_file_area_ori = NULL;
		}
#else
		/*上边的方案有个重大bug，就是令page_offset_in_file_area_temp加1后赋值给*page_offset_in_file_area。这直接导致回到
		 *filemap_map_pages_for_file_area()函数里执行 
		 folio_index_for_xa_index = (xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area;
		 addr += (folio_index_for_xa_index - last_pgoff) << PAGE_SHIFT;
		 计算page映射的用户态虚拟地址addr就有问题了。因为令page_offset_in_file_area_temp加1后赋值给*page_offset_in_file_area了。
		 这导致计算出来的page索引folio_index_for_xa_index 比 page真实索引大1，然后page映射的用户态虚拟地址addr也就大了
		 PAGE_SHIFT即4K。这样就出大问题了，映射page的用户态虚拟地址addr与page 就不一直了，虚拟地址映射的物理地址错乱了！
		 这导致mmap映射文件后，从0地址读到的4K数据，不是文件地址0~4k的文件数据，而是4k~8K的文件地址数据。因此，这里绝对要
		 保持page_offset_in_file_area_temp的现在的数据复制给page_offset_in_file_area，保持原值!这样回到
		 filemap_map_pages_for_file_area()函数里执行folio_index_for_xa_index = (xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area
		 计算出的page索引folio_index_for_xa_index与page的真实索引是相等的。
		 **/
		*page_offset_in_file_area = page_offset_in_file_area_temp;
		/*即便page_offset_in_file_area是3页不再对*p_file_area_ori=NULL设置NULL了。下次执行next_map_page_for_file_area()函数中处理，
		 *发现p_file_area有效，但page_offset_in_file_area是3，说明当前file_area的page都用过了，直接查找下一个file_area。*/
		*p_file_area_ori = p_file_area;
#endif
		FILE_AREA_PRINT("4:%s %s %d p_file_area:0x%llx find folio:0x%llx xas page index:%ld folio->index:%ld\n",__func__,current->comm,current->pid,(u64)p_file_area,(u64)folio,(xas->xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area_temp,folio->index);

		return folio;

		/*重点，遇到非法的page，不能直接执行下次循环，而是要去next_folio分支，令page_offset_in_file_area_temp加1，查询file_area的下一个page是否合法*/
unlock:
		folio_unlock(folio);

	    FILE_AREA_PRINT("5:%s %s %d unlock\n",__func__,current->comm,current->pid);
skip:
		folio_put(folio);
	    FILE_AREA_PRINT("5:%s %s %d skip\n",__func__,current->comm,current->pid);

next_folio:
	    FILE_AREA_PRINT("6:%s %s %d next_folio xas page index:%ld\n",__func__,current->comm,current->pid,(xas->xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area_temp);

		page_offset_in_file_area_temp ++;
		/*如果page_offset_in_file_area_temp小于4则goto find_page_from_file_area查找file_area里的下一个page。否则
		 *按顺序执行xas_next_entry()去查找下一个索引的file_area*/
		if(page_offset_in_file_area_temp < PAGE_COUNT_IN_AREA){
			goto find_page_from_file_area;
		}
		else{
			page_offset_in_file_area_temp = 0;
		}

	//} while ((folio = xas_next_entry(xas, end_pgoff)) != NULL);
	} while ((p_file_area = xas_next_entry(xas, end_pgoff >> PAGE_COUNT_IN_AREA_SHIFT)) != NULL);

	return NULL;
}

static inline struct folio *first_map_page_for_file_area(struct address_space *mapping,
		struct xa_state *xas,
		pgoff_t end_pgoff,unsigned int *page_offset_in_file_area,struct file_area **p_file_area)
{
	/*找到第一个有效page，一直向后找,直至找到传入的最大索引，依然找不到返回NULL*/
	//return next_uptodate_page_for_file_area(xas_find(xas, end_pgoff),
	//			  mapping, xas, end_pgoff);

	/*找到第一个有效file_area，一直向后找，直至找到传入的最大索引，依然找不到返回NULL*/
	*p_file_area = xas_find(xas, end_pgoff >> PAGE_COUNT_IN_AREA_SHIFT);
	return next_uptodate_page_for_file_area(p_file_area,
			mapping, xas, end_pgoff,page_offset_in_file_area,0);
}
#if 0
static inline struct folio *next_map_page_for_file_area(struct address_space *mapping,
		struct xa_state *xas,
		pgoff_t end_pgoff,unsigned int *page_offset_in_file_area,struct file_area **p_file_area)
{
	//return next_uptodate_page(xas_next_entry(xas, end_pgoff),
	//			  mapping, xas, end_pgoff);

	/*如果p_file_area不是NULL，说明上一次执行当前函数找到的file_area还有剩下的page没使用，这个page在file_area的
	 *起始索引是page_offset_in_file_area，本次执行该函数直接使用这个page*/
	if(*p_file_area)
		return next_uptodate_page_for_file_area(p_file_area,mapping, xas, end_pgoff,page_offset_in_file_area,1);
	else{
		*p_file_area = xas_next_entry(xas, end_pgoff >> PAGE_COUNT_IN_AREA_SHIFT);
		return next_uptodate_page_for_file_area(p_file_area,mapping, xas, end_pgoff,page_offset_in_file_area,0);
	}
}
#else
static inline struct folio *next_map_page_for_file_area(struct address_space *mapping,
		struct xa_state *xas,
		pgoff_t end_pgoff,unsigned int *page_offset_in_file_area,struct file_area **p_file_area)
{
	//return next_uptodate_page(xas_next_entry(xas, end_pgoff),
	//			  mapping, xas, end_pgoff);

	/*如果p_file_area不是NULL且page_offset_in_file_area小于3，说明上一次执行当前函数或第一次执行first_map_page_for_file_area()函数，
	 *找到的file_area还有剩下的page没使用，本次要查找的page在是file_page->pages[page_offset_in_file_area+1]。否则走else分支查找下一个file_area*/
	if(*p_file_area && *page_offset_in_file_area < (PAGE_COUNT_IN_AREA - 1))
		return next_uptodate_page_for_file_area(p_file_area,mapping, xas, end_pgoff,page_offset_in_file_area,1);
	else{
		/*到这个分支，有两种情况，一种是*p_file_area本身是NULL，必须查找新的file_area。另一种是它非NULL，但是*page_offset_in_file_area是3，此时
		 * 也要查找下一个file_area，因此它的page都遍历过了。但是需要对 *page_offset_in_file_area强制清0，表示从下一个file_area的第1个page开始遍历*/
		if(*p_file_area && *page_offset_in_file_area == (PAGE_COUNT_IN_AREA - 1))
			*page_offset_in_file_area = 0;

		*p_file_area = xas_next_entry(xas, end_pgoff >> PAGE_COUNT_IN_AREA_SHIFT);
		return next_uptodate_page_for_file_area(p_file_area,mapping, xas, end_pgoff,page_offset_in_file_area,0);
	}
}
#endif
vm_fault_t filemap_map_pages_for_file_area(struct vm_fault *vmf,
		pgoff_t start_pgoff, pgoff_t end_pgoff)
{
	struct vm_area_struct *vma = vmf->vma;
	struct file *file = vma->vm_file;
	struct address_space *mapping = file->f_mapping;
	pgoff_t last_pgoff = start_pgoff;//上一次判断的page索引
	unsigned long addr;
	//XA_STATE(xas, &mapping->i_pages, start_pgoff);
	XA_STATE(xas, &mapping->i_pages, start_pgoff >> PAGE_COUNT_IN_AREA_SHIFT);
	struct folio *folio;
	struct page *page;
	unsigned int mmap_miss = READ_ONCE(file->f_ra.mmap_miss);
	vm_fault_t ret = 0;

	/*初值必须赋于NULL，表示file_area无效，否则会令first_map_page_for_file_area()错误使用这个file_area*/
	struct file_area *p_file_area = NULL;
	struct file_stat_base *p_file_stat_base;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标，记录第一个page在第一个file_area里的偏移
	unsigned int page_offset_in_file_area = start_pgoff & PAGE_COUNT_IN_AREA_MASK;
	unsigned long folio_index_for_xa_index;

	rcu_read_lock();
	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	//folio = first_map_page(mapping, &xas, end_pgoff);
	folio = first_map_page_for_file_area(mapping, &xas, end_pgoff,&page_offset_in_file_area,&p_file_area);
	if (!folio)
		goto out;

	if (filemap_map_pmd(vmf, &folio->page)) {
		ret = VM_FAULT_NOPAGE;
		goto out;
	}
    /*addr是映射start_pgoff这个索引page对应的用户态虚拟地址*/
	addr = vma->vm_start + ((start_pgoff - vma->vm_pgoff) << PAGE_SHIFT);
	vmf->pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd, addr, &vmf->ptl);
	do {
again:
		/*之前xas.xa_index代表page索引，现在代表file_area索引，乘以4再加上page_offset_in_file_area才是page索引*/
		folio_index_for_xa_index = (xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area;

		//page = folio_file_page(folio, xas.xa_index);
		page = folio_file_page(folio, folio_index_for_xa_index);
		if (PageHWPoison(page))
			goto unlock;

		if (mmap_miss > 0)
			mmap_miss--;


		//addr += (xas.xa_index - last_pgoff) << PAGE_SHIFT;
		addr += (folio_index_for_xa_index - last_pgoff) << PAGE_SHIFT;
		//vmf->pte += xas.xa_index - last_pgoff;
		vmf->pte += folio_index_for_xa_index - last_pgoff;
		//last_pgoff = xas.xa_index;
		last_pgoff = folio_index_for_xa_index;

		if (!pte_none(*vmf->pte))
			goto unlock;

		/* We're about to handle the fault */
		if (vmf->address == addr)
			ret = VM_FAULT_NOPAGE;

		do_set_pte(vmf, page, addr);
		/* no need to invalidate: a not-present page won't be cached */
		update_mmu_cache(vma, addr, vmf->pte);
		//if (folio_more_pages(folio, xas.xa_index, end_pgoff)) {
		if (folio_more_pages(folio, folio_index_for_xa_index, end_pgoff)) {
			panic("1:%s %s %d mapping:0x%llx folio:0x%llx folio_nr_pages:%ld > 1\n",__func__,current->comm,current->pid,(u64)mapping,(u64)folio,folio_nr_pages(folio));
			xas.xa_index++;
			folio_ref_inc(folio);
			goto again;
		}
		folio_unlock(folio);
		continue;
unlock:
		//if (folio_more_pages(folio, xas.xa_index, end_pgoff)) {
		if (folio_more_pages(folio, folio_index_for_xa_index, end_pgoff)) {
			panic("2:%s %s %d mapping:0x%llx folio:0x%llx folio_nr_pages:%ld > 1\n",__func__,current->comm,current->pid,(u64)mapping,(u64)folio,folio_nr_pages(folio));
			xas.xa_index++;
			goto again;
		}
		folio_unlock(folio);
		folio_put(folio);
	//} while ((folio = next_map_page(mapping, &xas, end_pgoff)) != NULL);
	} while ((folio = next_map_page_for_file_area(mapping, &xas, end_pgoff,&page_offset_in_file_area,&p_file_area)) != NULL);
	pte_unmap_unlock(vmf->pte, vmf->ptl);
out:
	rcu_read_unlock();
	WRITE_ONCE(file->f_ra.mmap_miss, mmap_miss);
	return ret;
}

/*
/// fs/fs-writeback.c
1:inode_do_switch_wbs_for_file_area
*/
bool inode_do_switch_wbs_for_file_area(struct inode *inode,
		struct bdi_writeback *old_wb,
		struct bdi_writeback *new_wb)
{
	struct address_space *mapping = inode->i_mapping;
	XA_STATE(xas, &mapping->i_pages, 0);
	struct folio *folio;
	bool switched = false;

	struct file_area *p_file_area;
	struct file_stat_base *p_file_stat_base;
	//unsigned int page_offset_in_file_area = 0;
	int i;
	int mark_page_count = 0;

	spin_lock(&inode->i_lock);
	/*该函数没有rcu_read_lock，但是有xa_lock_irq，也能防止并发delete file_stat和file_area*/
	xa_lock_irq(&mapping->i_pages);

	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	/*
	 * Once I_FREEING or I_WILL_FREE are visible under i_lock, the eviction
	 * path owns the inode and we shouldn't modify ->i_io_list.
	 */
	if (unlikely(inode->i_state & (I_FREEING | I_WILL_FREE)))
		goto skip_switch;

	trace_inode_switch_wbs(inode, old_wb, new_wb);

	FILE_AREA_PRINT("%s %s %d mapping:0x%llx inode:0x%llx\n",__func__,current->comm,current->pid,(u64)mapping,(u64)inode);
	/*
	 * Count and transfer stats.  Note that PAGECACHE_TAG_DIRTY points
	 * to possibly dirty folios while PAGECACHE_TAG_WRITEBACK points to
	 * folios actually under writeback.
	 */
	//xas_for_each_marked(&xas, folio, ULONG_MAX, PAGECACHE_TAG_DIRTY) {
	xas_for_each_marked(&xas, p_file_area, ULONG_MAX, PAGECACHE_TAG_DIRTY) {
		long nr;
		if(!is_file_area_entry(p_file_area))
			panic("%s mapping:0x%llx p_file_area:0x%llx  error\n",__func__,(u64)mapping,(u64)p_file_area);

		p_file_area = entry_to_file_area(p_file_area);
		/*原有for循环是遍历保存page指针的xarray tree，统计有多少个dirty mark的page。现在是先统计有多少个有
		 *dirty mark的file_area，再统计file_area有多少个有dirty mark的page，效果一样。mark_page_count是有dirty mark的page有效性判断*/
		mark_page_count = 0;
		for(i = 0;i < PAGE_COUNT_IN_AREA;i ++){
			//folio = p_file_area->pages[i];
			folio = rcu_dereference(p_file_area->pages[i]);
			/*如果folio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
			folio_is_file_area_index_or_shadow_and_clear_NULL(folio);
			if (folio && folio_test_dirty(folio)) {
				nr = folio_nr_pages(folio);
				wb_stat_mod(old_wb, WB_RECLAIMABLE, -nr);
				wb_stat_mod(new_wb, WB_RECLAIMABLE, nr);

				mark_page_count ++;
			}
		}
		if(mark_page_count != file_area_page_mark_bit_count(p_file_area,PAGECACHE_TAG_DIRTY)){
			panic("%s mapping:0x%llx p_file_area:0x%llx xas.xa_index:%ld dirty page count %d %d error\n",__func__,(u64)mapping,(u64)p_file_area,xas.xa_index,mark_page_count,file_area_page_mark_bit_count(p_file_area,PAGECACHE_TAG_DIRTY));
		}
		/*这里有个for循环退出的问题需要深思。原本for循环是一直查找page，直至查找的page索引是ULONG_MAX才退出。这里该怎么处理？
		 *假如一共5个page，对应2个file_area。这个for循环里的代码会执行两次，判断这两个file_area所有page，这样正好不会有遗漏。额外代码不用添加*/
	}

	xas_set(&xas, 0);
	//xas_for_each_marked(&xas, folio, ULONG_MAX, PAGECACHE_TAG_WRITEBACK) {
	xas_for_each_marked(&xas, p_file_area, ULONG_MAX, PAGECACHE_TAG_WRITEBACK) {
		long nr;
		if(!is_file_area_entry(p_file_area))
			panic("%s mapping:0x%llx p_file_area:0x%llx  error\n",__func__,(u64)mapping,(u64)p_file_area);

		p_file_area = entry_to_file_area(p_file_area);

		if(0 == file_area_page_mark_bit_count(p_file_area,PAGECACHE_TAG_WRITEBACK)){
			panic("%s mapping:0x%llx p_file_area:0x%llx xas.xa_index:%ld dirty page count error\n",__func__,(u64)mapping,(u64)p_file_area,xas.xa_index);
		}
		mark_page_count = 0;
		for(i = 0;i < PAGE_COUNT_IN_AREA;i ++){
			//folio = p_file_area->pages[i];
			folio = rcu_dereference(p_file_area->pages[i]);
			/*如果folio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
			folio_is_file_area_index_or_shadow_and_clear_NULL(folio);
			if(folio && folio_test_writeback(folio)){
				nr = folio_nr_pages(folio);
				/*这个异常判断用两个panic替代*/
				//WARN_ON_ONCE(!folio_test_writeback(folio));
				wb_stat_mod(old_wb, WB_WRITEBACK, -nr);
				wb_stat_mod(new_wb, WB_WRITEBACK, nr);

				mark_page_count ++;
			}
		}
		if(!mark_page_count || (mark_page_count != file_area_page_mark_bit_count(p_file_area,PAGECACHE_TAG_WRITEBACK))){
			panic("%s mapping:0x%llx p_file_area:0x%llx xas.xa_index:%ld writeback page count %d_%d error\n",__func__,(u64)mapping,(u64)p_file_area,xas.xa_index,mark_page_count,file_area_page_mark_bit_count(p_file_area,PAGECACHE_TAG_WRITEBACK));
		}
	}

	if (mapping_tagged(mapping, PAGECACHE_TAG_WRITEBACK)) {
		atomic_dec(&old_wb->writeback_inodes);
		atomic_inc(&new_wb->writeback_inodes);
	}

	wb_get(new_wb);

	/*
	 * Transfer to @new_wb's IO list if necessary.  If the @inode is dirty,
	 * the specific list @inode was on is ignored and the @inode is put on
	 * ->b_dirty which is always correct including from ->b_dirty_time.
	 * The transfer preserves @inode->dirtied_when ordering.  If the @inode
	 * was clean, it means it was on the b_attached list, so move it onto
	 * the b_attached list of @new_wb.
	 */
	if (!list_empty(&inode->i_io_list)) {
		inode->i_wb = new_wb;

		if (inode->i_state & I_DIRTY_ALL) {
			struct inode *pos;

			list_for_each_entry(pos, &new_wb->b_dirty, i_io_list)
				if (time_after_eq(inode->dirtied_when,
							pos->dirtied_when))
					break;
			inode_io_list_move_locked(inode, new_wb,
					pos->i_io_list.prev);
		} else {
			inode_cgwb_move_to_attached(inode, new_wb);
		}
	} else {
		inode->i_wb = new_wb;
	}

	/* ->i_wb_frn updates may race wbc_detach_inode() but doesn't matter */
	inode->i_wb_frn_winner = 0;
	inode->i_wb_frn_avg_time = 0;
	inode->i_wb_frn_history = 0;
	switched = true;
skip_switch:
	/*
	 * Paired with load_acquire in unlocked_inode_to_wb_begin() and
	 * ensures that the new wb is visible if they see !I_WB_SWITCH.
	 */
	smp_store_release(&inode->i_state, inode->i_state & ~I_WB_SWITCH);

	xa_unlock_irq(&mapping->i_pages);
	spin_unlock(&inode->i_lock);

	return switched;
}

/* 
/// mm/migrate.c
1:folio_migrate_mapping_for_file_area
*/
int folio_migrate_mapping_for_file_area(struct address_space *mapping,
		struct folio *newfolio, struct folio *folio, int extra_count)
{
	//XA_STATE(xas, &mapping->i_pages, folio_index(folio));
	XA_STATE(xas, &mapping->i_pages, folio_index(folio) >> PAGE_COUNT_IN_AREA_SHIFT);
	struct zone *oldzone, *newzone;
	int dirty;
	int expected_count = expected_page_refs(mapping, &folio->page) + extra_count;
	long nr = folio_nr_pages(folio);

	struct file_area *p_file_area;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = folio_index(folio) & PAGE_COUNT_IN_AREA_MASK;

	if (!mapping) {
		/* Anonymous page without mapping */
		if (folio_ref_count(folio) != expected_count)
			return -EAGAIN;

		/* No turning back from here */
		newfolio->index = folio->index;
		newfolio->mapping = folio->mapping;
		if (folio_test_swapbacked(folio))
			__folio_set_swapbacked(newfolio);

		return MIGRATEPAGE_SUCCESS;
	}

	oldzone = folio_zone(folio);
	newzone = folio_zone(newfolio);

	xas_lock_irq(&xas);
	if (!folio_ref_freeze(folio, expected_count)) {
		xas_unlock_irq(&xas);
		return -EAGAIN;
	}

	/*
	 * Now we know that no one else is looking at the folio:
	 * no turning back from here.
	 */
	newfolio->index = folio->index;
	newfolio->mapping = folio->mapping;
	folio_ref_add(newfolio, nr); /* add cache reference */
	if (folio_test_swapbacked(folio)) {
		__folio_set_swapbacked(newfolio);
		if (folio_test_swapcache(folio)) {
			folio_set_swapcache(newfolio);
			newfolio->private = folio_get_private(folio);
		}
	} else {
		VM_BUG_ON_FOLIO(folio_test_swapcache(folio), folio);
	}

	/* Move dirty while page refs frozen and newpage not yet exposed */
	dirty = folio_test_dirty(folio);
	if (dirty) {
		folio_clear_dirty(folio);
		folio_set_dirty(newfolio);
	}

	//xas_store(&xas, newfolio);
	/*如果此时file_stat或者file_area cold_file_stat_delete()、cold_file_area_delete被释放了，那肯定是不合理的
	 *这里会触发panic*/
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        panic("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	p_file_area = (struct file_area *)xas_load(&xas);
	if(!p_file_area || !is_file_area_entry(p_file_area))
		panic("%s mapping:0x%llx p_file_area:0x%llx error\n",__func__,(u64)mapping,(u64)p_file_area);

	p_file_area = entry_to_file_area(p_file_area);
	//if(folio != (struct folio *)p_file_area->pages[page_offset_in_file_area]){
	if(folio != (struct folio *)rcu_dereference(p_file_area->pages[page_offset_in_file_area])){
		panic("%s mapping:0x%llx folio:0x%llx != p_file_area->pages:0x%llx\n",__func__,(u64)mapping,(u64)folio,(u64)p_file_area->pages[page_offset_in_file_area]);
	}
	//p_file_area->pages[page_offset_in_file_area] = newfolio;
	rcu_assign_pointer(p_file_area->pages[page_offset_in_file_area],newfolio);
	FILE_AREA_PRINT1("%s mapping:0x%llx p_file_area:0x%llx folio:0x%llx newfolio:0x%llx page_offset_in_file_area:%d\n",__func__,(u64)mapping,(u64)p_file_area,(u64)folio,(u64)newfolio,page_offset_in_file_area);


	/*
	 * Drop cache reference from old page by unfreezing
	 * to one less reference.
	 * We know this isn't the last reference.
	 */
	folio_ref_unfreeze(folio, expected_count - nr);

	xas_unlock(&xas);
	/* Leave irq disabled to prevent preemption while updating stats */

	/*
	 * If moved to a different zone then also account
	 * the page for that zone. Other VM counters will be
	 * taken care of when we establish references to the
	 * new page and drop references to the old page.
	 *
	 * Note that anonymous pages are accounted for
	 * via NR_FILE_PAGES and NR_ANON_MAPPED if they
	 * are mapped to swap space.
	 */
	if (newzone != oldzone) {
		struct lruvec *old_lruvec, *new_lruvec;
		struct mem_cgroup *memcg;

		memcg = folio_memcg(folio);
		old_lruvec = mem_cgroup_lruvec(memcg, oldzone->zone_pgdat);
		new_lruvec = mem_cgroup_lruvec(memcg, newzone->zone_pgdat);

		__mod_lruvec_state(old_lruvec, NR_FILE_PAGES, -nr);
		__mod_lruvec_state(new_lruvec, NR_FILE_PAGES, nr);
		if (folio_test_swapbacked(folio) && !folio_test_swapcache(folio)) {
			__mod_lruvec_state(old_lruvec, NR_SHMEM, -nr);
			__mod_lruvec_state(new_lruvec, NR_SHMEM, nr);
		}
#ifdef CONFIG_SWAP
		if (folio_test_swapcache(folio)) {
			__mod_lruvec_state(old_lruvec, NR_SWAPCACHE, -nr);
			__mod_lruvec_state(new_lruvec, NR_SWAPCACHE, nr);
		}
#endif
		if (dirty && mapping_can_writeback(mapping)) {
			__mod_lruvec_state(old_lruvec, NR_FILE_DIRTY, -nr);
			__mod_zone_page_state(oldzone, NR_ZONE_WRITE_PENDING, -nr);
			__mod_lruvec_state(new_lruvec, NR_FILE_DIRTY, nr);
			__mod_zone_page_state(newzone, NR_ZONE_WRITE_PENDING, nr);
		}
	}
	local_irq_enable();

	return MIGRATEPAGE_SUCCESS;
}

/*
/// mm/page-writeback.c
1:tag_pages_for_writeback_for_file_area
2:__folio_mark_dirty_for_file_area
3:__folio_end_writeback_for_file_area
4:__folio_start_writeback_for_file_area
*/
void tag_pages_for_writeback_for_file_area(struct address_space *mapping,
		pgoff_t start, pgoff_t end)
{
	//XA_STATE(xas, &mapping->i_pages, start);
	XA_STATE(xas, &mapping->i_pages, start >> PAGE_COUNT_IN_AREA_SHIFT);
	unsigned int tagged = 0;
	void *page;

	struct file_area *p_file_area;
	struct file_stat_base *p_file_stat_base;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = start & PAGE_COUNT_IN_AREA_MASK;
	pgoff_t file_area_end_index = end >> PAGE_COUNT_IN_AREA_SHIFT;

	FILE_AREA_PRINT("%s %s %d mapping:0x%llx start:%ld end:%ld\n",__func__,current->comm,current->pid,(u64)mapping,start,end);
	/*该函数没有rcu_read_lock，但是有xa_lock_irqsave加锁，也能防止file_stat被方法delete*/
	xas_lock_irq(&xas);

	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
		printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	//xas_for_each_marked(&xas, page, end, PAGECACHE_TAG_DIRTY) {
	xas_for_each_marked(&xas, p_file_area, file_area_end_index, PAGECACHE_TAG_DIRTY) {
		if(!is_file_area_entry(p_file_area))
			panic("%s mapping:0x%llx p_file_area:0x%llx  error\n",__func__,(u64)mapping,(u64)p_file_area);

		p_file_area = entry_to_file_area(p_file_area);
		/*file_area可能会重复设置towrite mark，这个函数里做的有防护，为了不大改，维持老代码*/
		xas_set_mark(&xas, PAGECACHE_TAG_TOWRITE);

find_page_from_file_area:
		/*有个重大bug需要注意。这里是模拟原for循环里page索引大于end后就退出，现在for循环是搜索file_area，判断page索引是否
		 * 超出end必须在这里单独判断。但是，xas->xa_index索引是file_area的，不是page的，要乘以4才是page的索引*/
		//if(xas->xa_index + page_offset_in_file_area > ){
		if((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area > end){
			break;
		}

		/*是调试的文件，打印调试信息*/
		if(mapping->rh_reserved3){
			printk("%s mark_towrite mapping:0x%llx file_stat:0x%llx file_area:0x%llx status:0x%x page_offset_in_file_area:%d folio:0x%llx\n",__func__,(u64)mapping,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,(u64)page);
		}

		/*file_area里的4个page都是dirty页吗，那可不一定，必须检测是否有脏页mark的page，才能在file_area里设置towrite mark*/
		if(is_file_area_page_mark_bit_set(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_DIRTY)){
			//page = p_file_area->pages[page_offset_in_file_area];
			page = rcu_dereference(p_file_area->pages[page_offset_in_file_area]);
			/*如果folio是file_area的索引，则对folio清NULL，避免folio干扰后续判断*/
			folio_is_file_area_index_or_shadow_and_clear_NULL(page);
			/*实际测试表明，存在并发情况，file_area有dirty mark但是page没有dirty标记，因为page dirty标记被另外的进程先清理了，后续它会再清理ditry mark标记*/
			if(!page/* || !PageDirty(page)*/){
				panic("%s mapping:0x%llx p_file_area:0x%llx page:0x%llx not dirty\n",__func__,(u64)mapping,(u64)p_file_area,(u64)page);
			}
			/*检测查找到的page是否正确，不是则crash*/
			CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,mapping,(struct page *)page,p_file_area,page_offset_in_file_area,((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area));

			set_file_area_page_mark_bit(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_TOWRITE);
		}

		page_offset_in_file_area ++;
		if(page_offset_in_file_area < PAGE_COUNT_IN_AREA)
			goto find_page_from_file_area;
		else
			page_offset_in_file_area = 0;

		if (++tagged % XA_CHECK_SCHED)
			continue;

		/*重置xas->xa_node=XAS_RESTART，更新xas->xa_index，确定下次查找的page的索引!!!!!!!!!!!!!!*/
		xas_pause(&xas);
		xas_unlock_irq(&xas);
		cond_resched();
		xas_lock_irq(&xas);
	}
	xas_unlock_irq(&xas);
}

void __folio_mark_dirty_for_file_area(struct folio *folio, struct address_space *mapping,
		int warn)
{
	unsigned long flags;
    struct file_stat_base *p_file_stat_base;
	FILE_AREA_PRINT("%s %s %d mapping:0x%llx folio:0x%llx\n",__func__,current->comm,current->pid,(u64)mapping,(u64)folio);

	/*该函数没有rcu_read_lock，但是有xa_lock_irqsave加锁，也能防止file_stat被方法delete*/
	xa_lock_irqsave(&mapping->i_pages, flags);
	//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
	/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
	 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
	 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
	 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
	 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
	 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
	 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
	 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
	 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	smp_rmb();
	if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
        printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

	if (folio->mapping) {	/* Race with truncate? */
		XA_STATE(xas, &mapping->i_pages, folio_index(folio) >> PAGE_COUNT_IN_AREA_SHIFT);
		struct file_area *p_file_area;
		//令page索引与上0x3得到它在file_area的pages[]数组的下标
		unsigned int page_offset_in_file_area = folio_index(folio) & PAGE_COUNT_IN_AREA_MASK;

		WARN_ON_ONCE(warn && !folio_test_uptodate(folio));
		folio_account_dirtied(folio, mapping);
		//__xa_set_mark(&mapping->i_pages, folio_index(folio),
		//		PAGECACHE_TAG_DIRTY);
		p_file_area = xas_load(&xas);
		if(!is_file_area_entry(p_file_area)){
			panic("%s mapping:0x%llx p_file_area:0x%llx  error\n",__func__,(u64)mapping,(u64)p_file_area);
		}
		p_file_area = entry_to_file_area(p_file_area);

		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID_MARK(&xas,mapping,folio,p_file_area->pages[page_offset_in_file_area],p_file_area,page_offset_in_file_area,((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area));

		/*先标记file_area是dirtry，然后在file_area的page的dirty*/
		xas_set_mark(&xas, PAGECACHE_TAG_DIRTY);
		/*存在page被多次标记writeback的情况，这里不做page多次标记writeback就panic的判断*/
		set_file_area_page_mark_bit(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_DIRTY);

		/*是调试的文件，打印调试信息*/
		if(mapping->rh_reserved3){
			printk("%s mark_dirty mapping:0x%llx file_stat:0x%llx file_area:0x%llx status:0x%x page_offset_in_file_area:%d folio:0x%llx flags:0x%lx\n",__func__,(u64)mapping,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,(u64)folio,folio->flags);
		}
	}
	xa_unlock_irqrestore(&mapping->i_pages, flags);
}

bool __folio_end_writeback_for_file_area(struct folio *folio)
{
	long nr = folio_nr_pages(folio);
	struct address_space *mapping = folio_mapping(folio);
	bool ret;

	folio_memcg_lock(folio);
	if (mapping && mapping_use_writeback_tags(mapping)) {
		struct inode *inode = mapping->host;
		struct backing_dev_info *bdi = inode_to_bdi(inode);
		unsigned long flags;
        struct file_stat_base *p_file_stat_base;
		FILE_AREA_PRINT("%s %s %d mapping:0x%llx folio:0x%llx index:%ld\n",__func__,current->comm,current->pid,(u64)mapping,(u64)folio,folio->index);
		/*该函数没有rcu_read_lock，但是有xa_lock_irqsave加锁，也能防止file_stat被方法delete*/
		xa_lock_irqsave(&mapping->i_pages, flags);
		
		//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
		p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
		/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
		 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
		 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
		 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
		 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
		 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
		 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
		 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
		 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		 * */
		smp_rmb();
		if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
			printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

		ret = folio_test_clear_writeback(folio);
		if (ret) {
			XA_STATE(xas, &mapping->i_pages, folio_index(folio) >> PAGE_COUNT_IN_AREA_SHIFT);
			struct file_area *p_file_area;
			//令page索引与上0x3得到它在file_area的pages[]数组的下标
			unsigned int page_offset_in_file_area = folio_index(folio) & PAGE_COUNT_IN_AREA_MASK;

			//__xa_clear_mark(&mapping->i_pages, folio_index(folio),
			//			PAGECACHE_TAG_WRITEBACK);
			p_file_area = xas_load(&xas);
			if(!is_file_area_entry(p_file_area)){
				panic("%s mapping:0x%llx p_file_area:0x%llx  error\n",__func__,(u64)mapping,(u64)p_file_area);
			}
			p_file_area = entry_to_file_area(p_file_area);
			/*检测查找到的page是否正确，不是则crash*/
			CHECK_FOLIO_FROM_FILE_AREA_VALID_MARK(&xas,mapping,folio,p_file_area->pages[page_offset_in_file_area],p_file_area,page_offset_in_file_area,((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area));

			/*先清理file_area的writeback标记，再清理file_area里的page的writeback mark。错了，只有file_area的4个page的writeback
			 * mark全被清理了，才能清理file_area的writeback mark标记位，因此把清理file_area的writeback mark放到下边了*/
			//xas_clear_mark(&xas, PAGECACHE_TAG_WRITEBACK);
			if(!is_file_area_page_mark_bit_set(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_WRITEBACK)){
				panic("%s mapping:0x%llx p_file_area:0x%llx page:0x%llx not writeback\n",__func__,(u64)mapping,(u64)p_file_area,(u64)folio);
			}

			clear_file_area_page_mark_bit(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_WRITEBACK);
			/*只有file_area的4个page的writebackmark全被清理了,才能清理file_area的writeback mark标记位*/
			if(0 == file_area_page_mark_bit_count(p_file_area,PAGECACHE_TAG_WRITEBACK))
				xas_clear_mark(&xas, PAGECACHE_TAG_WRITEBACK);

			if(mapping->rh_reserved3){
				printk("%s clear_writeback mapping:0x%llx file_stat:0x%llx file_area:0x%llx status:0x%x page_offset_in_file_area:%d folio:0x%llx flags:0x%lx\n",__func__,(u64)mapping,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,(u64)folio,folio->flags);
			}

			if (bdi->capabilities & BDI_CAP_WRITEBACK_ACCT) {
				struct bdi_writeback *wb = inode_to_wb(inode);

				wb_stat_mod(wb, WB_WRITEBACK, -nr);
				__wb_writeout_add(wb, nr);
				if (!mapping_tagged(mapping,
							PAGECACHE_TAG_WRITEBACK))
					wb_inode_writeback_end(wb);
			}
		}

		if (mapping->host && !mapping_tagged(mapping,
					PAGECACHE_TAG_WRITEBACK))
			sb_clear_inode_writeback(mapping->host);

		xa_unlock_irqrestore(&mapping->i_pages, flags);
	} else {
		ret = folio_test_clear_writeback(folio);
	}
	if (ret) {
		lruvec_stat_mod_folio(folio, NR_WRITEBACK, -nr);
		zone_stat_mod_folio(folio, NR_ZONE_WRITE_PENDING, -nr);
		node_stat_mod_folio(folio, NR_WRITTEN, nr);
	}
	folio_memcg_unlock(folio);
	return ret;
}

bool __folio_start_writeback_for_file_area(struct folio *folio, bool keep_write)
{
	long nr = folio_nr_pages(folio);
	struct address_space *mapping = folio_mapping(folio);
	bool ret;
	int access_ret;

	folio_memcg_lock(folio);
	if (mapping && mapping_use_writeback_tags(mapping)) {
		//XA_STATE(xas, &mapping->i_pages, folio_index(folio));
		XA_STATE(xas, &mapping->i_pages, folio_index(folio) >> PAGE_COUNT_IN_AREA_SHIFT);
		struct inode *inode = mapping->host;
		struct backing_dev_info *bdi = inode_to_bdi(inode);
		unsigned long flags;

		struct file_area *p_file_area;
		struct file_stat_base *p_file_stat_base;
		//令page索引与上0x3得到它在file_area的pages[]数组的下标
		unsigned int page_offset_in_file_area = folio_index(folio) & PAGE_COUNT_IN_AREA_MASK;

		FILE_AREA_PRINT("%s %s %d mapping:0x%llx folio:0x%llx\n",__func__,current->comm,current->pid,(u64)mapping,(u64)folio);
		/*该函数没有rcu_read_lock，但是有xa_lock_irqsave加锁，也能防止file_stat被方法delete*/
		xas_lock_irqsave(&xas, flags);

		//p_file_stat = (struct file_stat *)mapping->rh_reserved1;
		p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;
		/* 必须要在rcu_read_lock()后，再执行smp_rmb()，再判断mapping->rh_reserved1指向的file_stat是否有效。
		 * 因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
		 * 指向的这个file_stat结构，并且赋值mapping->rh_reserved1=1。rcu_read_lock()保证file_stat不会立即被释放。 
		 * smp_rmb()是要立即感知到mapping->rh_reserved1的最新值——即1。还有，p_file_stat = (struct file_stat *)mapping->rh_reserved1
		 * 赋值必须放到smp_rmb()内存屏障前边，因为可能这里赋值时mapping->rh_reserved1还是正常，smp_rmb()执行后，
		 * IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)执行时mapping->rh_reserved1已经被cold_file_stat_delete()赋值1了。
		 * 如果不用smp_rmb()内存屏障隔开，可能会出现if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))先执行，此时
		 * mapping->rh_reserved1还是正常的，但是再等执行p_file_stat = (struct file_stat *)mapping->rh_reserved1就是1了，
		 * 此时就错过判断mapping->rh_reserved1非法了，然后执行mapping->rh_reserved1这个file_stat而crash!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		 * */
		smp_rmb();
		if(unlikely(!IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)))
			printk("%s %s %d mapping:0x%llx file_stat:0x%lx has delete,do not use this file_stat!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,current->comm,current->pid,(u64)mapping,mapping->rh_reserved1);

		//xas_load(&xas);
		p_file_area = xas_load(&xas);
		/*此时file_area不可能非法*/
		if(!is_file_area_entry(p_file_area)){
			panic("%s mapping:0x%llx p_file_area:0x%llx  error\n",__func__,(u64)mapping,(u64)p_file_area);
		}
		p_file_area = entry_to_file_area(p_file_area);

		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID_MARK(&xas,mapping,folio,p_file_area->pages[page_offset_in_file_area],p_file_area,page_offset_in_file_area,((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area));

		ret = folio_test_set_writeback(folio);
		if (!ret) {
			bool on_wblist;

			on_wblist = mapping_tagged(mapping,
					PAGECACHE_TAG_WRITEBACK);

			xas_set_mark(&xas, PAGECACHE_TAG_WRITEBACK);
			/*上边此时只是标记file_area的writeback mark，这里标记file_area里的page的writeback mark*/
			set_file_area_page_mark_bit(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_WRITEBACK);
			if (bdi->capabilities & BDI_CAP_WRITEBACK_ACCT) {
				struct bdi_writeback *wb = inode_to_wb(inode);

				wb_stat_mod(wb, WB_WRITEBACK, nr);
				if (!on_wblist)
					wb_inode_writeback_start(wb);
			}

			/*
			 * We can come through here when swapping
			 * anonymous folios, so we don't necessarily
			 * have an inode to track for sync.
			 */
			if (mapping->host && !on_wblist)
				sb_mark_inode_writeback(mapping->host);
		}
		
		/*是调试的文件，打印调试信息*/
		if(mapping->rh_reserved3){
			printk("%s clear_dirty mapping:0x%llx file_stat:0x%llx file_area:0x%llx status:0x%x page_offset_in_file_area:%d folio:0x%llx flags:0x%lx\n",__func__,(u64)mapping,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,(u64)folio,folio->flags);
		}

		if (!folio_test_dirty(folio)){
			//xas_clear_mark(&xas, PAGECACHE_TAG_DIRTY);

			/*上边此时只是标记file_area的dirty mark，这里标记file_area里的page的dirty mark。
			 *错了，只有file_area的page的dirty mark标记位全被清理掉，才能清理file_area的mark标记*/
			clear_file_area_page_mark_bit(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_DIRTY);
			if(0 == file_area_page_mark_bit_count(p_file_area,PAGECACHE_TAG_DIRTY))
				xas_clear_mark(&xas, PAGECACHE_TAG_DIRTY);

			if(mapping->rh_reserved3){
				printk("%s real_clear_dirty mapping:0x%llx file_stat:0x%llx file_area:0x%llx status:0x%x page_offset_in_file_area:%d folio:0x%llx flags:0x%lx\n",__func__,(u64)mapping,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,(u64)folio,folio->flags);
			}
		}

		/*是调试的文件，打印调试信息*/
		if(mapping->rh_reserved3){
			printk("%s clear_towrite mapping:0x%llx file_stat:0x%llx file_area:0x%llx status:0x%x page_offset_in_file_area:%d folio:0x%llx flags:0x%lx keep_write:%d\n",__func__,(u64)mapping,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,(u64)folio,folio->flags,keep_write);
		}

		if (!keep_write){
			//xas_clear_mark(&xas, PAGECACHE_TAG_TOWRITE);
			/*上边此时只是标记file_area的towrite mark，这里标记file_area里的page的towrite mark
			 *错了，只有file_area的page的towrite mark标记位全被清理掉，才能清理file_area的mark标记*/
			clear_file_area_page_mark_bit(p_file_area,page_offset_in_file_area,PAGECACHE_TAG_TOWRITE);
			if(0 == file_area_page_mark_bit_count(p_file_area,PAGECACHE_TAG_TOWRITE))
				xas_clear_mark(&xas, PAGECACHE_TAG_TOWRITE);
		}
		xas_unlock_irqrestore(&xas, flags);
	} else {
		ret = folio_test_set_writeback(folio);
	}
	if (!ret) {
		lruvec_stat_mod_folio(folio, NR_WRITEBACK, nr);
		zone_stat_mod_folio(folio, NR_ZONE_WRITE_PENDING, nr);
	}
	folio_memcg_unlock(folio);
	access_ret = arch_make_folio_accessible(folio);
	/*
	 * If writeback has been triggered on a page that cannot be made
	 * accessible, it is too late to recover here.
	 */
	VM_BUG_ON_FOLIO(access_ret != 0, folio);

	return ret;
}
#endif
#endif
