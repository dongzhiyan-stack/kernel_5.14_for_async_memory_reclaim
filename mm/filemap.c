// SPDX-License-Identifier: GPL-2.0-only
/*
 *	linux/mm/filemap.c
 *
 * Copyright (C) 1994-1999  Linus Torvalds
 */

/*
 * This file handles the generic file mmap semantics used by
 * most "normal" filesystems (but you don't /have/ to use this:
 * the NFS filesystem used to do this differently, for example)
 */
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

#define CREATE_TRACE_POINTS
#include <trace/events/filemap.h>

/*
 * FIXME: remove all knowledge of the buffer layer from the core VM
 */
#include <linux/buffer_head.h> /* for try_to_free_buffers */

#include <asm/mman.h>

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
#include "async_memory_reclaim_for_cold_file_area.h"
int is_test_file(struct address_space *mapping)
{
#if 0
#define TEST_FILE_NAME "kern_test_fi"
	struct dentry *dentry = NULL;
	struct inode *inode = NULL;
	if(mapping && mapping->host && !hlist_empty(&mapping->host->i_dentry)){
		inode = mapping->host;
		spin_lock(&inode->i_lock);
		if(atomic_read(&inode->i_count) > 0){
			dentry = hlist_entry(mapping->host->i_dentry.first, struct dentry, d_u.d_alias);
			if(dentry){
				if(strncmp(dentry->d_iname,TEST_FILE_NAME,11) == 0){
					spin_unlock(&inode->i_lock);
					return 1;
				}
			}
		}
		spin_unlock(&inode->i_lock);
	}
#else
	const char *file_system_name;
	if(mapping && mapping->host && mapping->host->i_sb){
        file_system_name = mapping->host->i_sb->s_type->name;
		if(0 == strcmp(file_system_name,"ext4") /*|| 0 == strcmp(file_system_name,"xfs")*/)
		    return 1;
	}
#endif	
	return 0;
}
#ifdef ASYNC_MEMORY_RECLAIM_DEBUG
inline struct  file_area * find_file_area_from_xarray_cache_node(struct xa_state *xas,struct file_stat_base *p_file_stat_base, pgoff_t index)
{
	//这段代码必须放到rcu lock里，保证node结构不会被释放
	//判断要查找的page是否在xarray tree的cache node里
	if(p_file_stat_base->xa_node_cache){
		/*这个内存屏障为了确保delete page函数里，释放掉node后，对p_file_stat->xa_node_cache_base_index 赋值0，执行这个函数的进程在新的rcu周期立即感知到。
		 *调用这个函数的filemap_get_read_batch()和mapping_get_entry()已经执行了smp_rmb()，这里就不用重复调用了*/
		//smp_rmb();

		//要插在的page索引在缓存的node里
		if((index >= p_file_stat_base->xa_node_cache_base_index) && (index <= (p_file_stat_base->xa_node_cache_base_index + FILE_AREA_PAGE_COUNT_MASK))){
			//unsigned int xa_offset = index & FILE_AREA_PAGE_COUNT_MASK;
			unsigned int xa_offset = (index & FILE_AREA_PAGE_COUNT_MASK) >> PAGE_COUNT_IN_AREA_SHIFT;
			struct file_area *p_file_area;
			struct xa_node *xa_node_parent = (struct xa_node *)p_file_stat_base->xa_node_cache;

			p_file_area = xa_node_parent->slots[xa_offset];
			//保存到xarray tree的file_area指针由我的代码完全控制，故先不考虑xa_is_zero、xa_is_retry、xa_is_value 这些异常判断	
			//if(p_file_area && !xa_is_zero(p_file_area) && !xa_is_retry(p_file_area) && !xa_is_value(p_file_area) && p_file_area->start_index == index & PAGE_COUNT_IN_AREA){
			if(is_file_area_entry(p_file_area)){
				p_file_area = entry_to_file_area(p_file_area);
				FILE_AREA_PRINT("%s p_file_stat:0x%llx xa_node_cache:0x%llx cache_base_index:%ld index:%ld %s\n",__func__,(u64)p_file_stat_base,(u64)p_file_stat_base->xa_node_cache,p_file_stat_base->xa_node_cache_base_index,index,p_file_area->start_index == (index & ~PAGE_COUNT_IN_AREA_MASK)?"find ok":"find fail");

				if(p_file_area->start_index == (index & ~PAGE_COUNT_IN_AREA_MASK)){
					xas->xa_offset = xa_offset;
					xas->xa_node = xa_node_parent;
					/*这里有个重大的隐藏bug，在从cache node找到匹配的file_area后，必须把它的索引更新给xas->xa_index。
					 *这样filemap_get_read_batch()里调用该函数后，goto find_page_from_file_area分支直接跳到for循环里
					 *去查找file_area的page。当该file_area的page查找完，则执行for循环的xas_next(&xas)去查找下一个索引
					 *索引的file_area。此时就要依赖xas->xa_index++去查找下一个索引的file_area。不对，不用了，因为
					 *在filemap_get_read_batch()函数一开头，已经xas->xa_index = index >> PAGE_COUNT_IN_AREA_SHIFT赋值
					 *过了。这里只用对xas->xa_offset和xas->xa_node赋值就行了，这跟filemap_get_read_batch()的for循环里的
					 *xas_load(&xas)效果一样，也是只对xas->xa_offset和xas->xa_node赋值*/
					//xas->xa_index = index >> PAGE_COUNT_IN_AREA_SHIFT;
					return p_file_area;
				}
			}
		}
	}
	return NULL;
}
#endif
#endif
/*
 * Shared mappings implemented 30.11.1994. It's not fully working yet,
 * though.
 *
 * Shared mappings now work. 15.8.1995  Bruno.
 *
 * finished 'unifying' the page and buffer cache and SMP-threaded the
 * page-cache, 21.05.1999, Ingo Molnar <mingo@redhat.com>
 *
 * SMP-threaded pagemap-LRU 1999, Andrea Arcangeli <andrea@suse.de>
 */

/*
 * Lock ordering:
 *
 *  ->i_mmap_rwsem		(truncate_pagecache)
 *    ->private_lock		(__free_pte->block_dirty_folio)
 *      ->swap_lock		(exclusive_swap_page, others)
 *        ->i_pages lock
 *
 *  ->i_rwsem
 *    ->invalidate_lock		(acquired by fs in truncate path)
 *      ->i_mmap_rwsem		(truncate->unmap_mapping_range)
 *
 *  ->mmap_lock
 *    ->i_mmap_rwsem
 *      ->page_table_lock or pte_lock	(various, mainly in memory.c)
 *        ->i_pages lock	(arch-dependent flush_dcache_mmap_lock)
 *
 *  ->mmap_lock
 *    ->invalidate_lock		(filemap_fault)
 *      ->lock_page		(filemap_fault, access_process_vm)
 *
 *  ->i_rwsem			(generic_perform_write)
 *    ->mmap_lock		(fault_in_readable->do_page_fault)
 *
 *  bdi->wb.list_lock
 *    sb_lock			(fs/fs-writeback.c)
 *    ->i_pages lock		(__sync_single_inode)
 *
 *  ->i_mmap_rwsem
 *    ->anon_vma.lock		(vma_adjust)
 *
 *  ->anon_vma.lock
 *    ->page_table_lock or pte_lock	(anon_vma_prepare and various)
 *
 *  ->page_table_lock or pte_lock
 *    ->swap_lock		(try_to_unmap_one)
 *    ->private_lock		(try_to_unmap_one)
 *    ->i_pages lock		(try_to_unmap_one)
 *    ->lruvec->lru_lock	(follow_page->mark_page_accessed)
 *    ->lruvec->lru_lock	(check_pte_range->isolate_lru_page)
 *    ->private_lock		(page_remove_rmap->set_page_dirty)
 *    ->i_pages lock		(page_remove_rmap->set_page_dirty)
 *    bdi.wb->list_lock		(page_remove_rmap->set_page_dirty)
 *    ->inode->i_lock		(page_remove_rmap->set_page_dirty)
 *    ->memcg->move_lock	(page_remove_rmap->lock_page_memcg)
 *    bdi.wb->list_lock		(zap_pte_range->set_page_dirty)
 *    ->inode->i_lock		(zap_pte_range->set_page_dirty)
 *    ->private_lock		(zap_pte_range->block_dirty_folio)
 *
 * ->i_mmap_rwsem
 *   ->tasklist_lock            (memory_failure, collect_procs_ao)
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
static void page_cache_delete_for_file_area(struct address_space *mapping,
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
	//p_file_area->pages[page_offset_in_file_area] = NULL;
	rcu_assign_pointer(p_file_area->pages[page_offset_in_file_area], NULL);

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
	if(mapping_exiting(mapping))
		xas_store(&xas, NULL);

	/*清理xarray tree的dirty、writeback、towrite标记，重点!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
	xas_init_marks(&xas);
	/*清理file_area所有的towrite、dirty、writeback的mark标记。这个函数是在把file_area从xarray tree剔除时执行的。
	 *之后file_area是无效的，有必要吗????????????。有必要，但是要把清理page的dirty、writeback、towrite mark标记代码放到
	 上边。做到每delete一个page，就要清理这个page的dirty、writeback、towrite mark标记，而不能要等到delete 4个page后再同一清理4个page的mark标记*/
	//clear_file_area_towrite_dirty_writeback_mark(p_file_area);

	//folio->mapping = NULL;必须放到前边，这个隐藏的问题很深呀

	/* Leave page->index set: truncation lookup relies upon it */
	//mapping->nrpages -= nr; 这个也要放到前边，page失效就要立即 mapping->nrpages -= nr，否则执行不了这里
}
#endif
static void page_cache_delete(struct address_space *mapping,
				   struct folio *folio, void *shadow)
{
	XA_STATE(xas, &mapping->i_pages, folio->index);
	long nr = 1;
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
	/*1:page的从xarray tree delete和 保存到xarray tree 两个过程因为加锁防护，不会并发执行，因此不用担心下边的
	 *找到的folio是file_area。*/
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		/* 如果因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
		 * 指向的这个file_stat结构。或者也会并发执行cold_file_area_delete()释放folio所属的file_area结构。
		 * 会不会跟这里page_cache_delete()起冲突？不会，cold_file_stat_delete()会xas_lock_irq加锁判断
		 * file_stat是否还有file_area，cold_file_area_delete()会xas_lock_irq加锁后判断file_area是否还有
		 * page。而page_cache_delete()执行前也会先xas_lock_irq加锁，而page_cache_delete()执行时，
		 * 一定有folio，有folio一定有file_area，有file_area一定有file_stat，因此此时不会触发执行
		 * cold_file_stat_delete()释放file_stat，不会触发cold_file_area_delete()释放file_area*/
		smp_rmb();
		if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
			return page_cache_delete_for_file_area(mapping,folio,shadow);
	}
#endif	

	mapping_set_update(&xas, mapping);

	/* hugetlb pages are represented by a single entry in the xarray */
	if (!folio_test_hugetlb(folio)) {
		xas_set_order(&xas, folio->index, folio_order(folio));
		nr = folio_nr_pages(folio);
	}

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	xas_store(&xas, shadow);
	xas_init_marks(&xas);

	folio->mapping = NULL;
	/* Leave page->index set: truncation lookup relies upon it */
	mapping->nrpages -= nr;
}
static void filemap_unaccount_folio(struct address_space *mapping,
		struct folio *folio)
{
	long nr;

	VM_BUG_ON_FOLIO(folio_mapped(folio), folio);
	if (!IS_ENABLED(CONFIG_DEBUG_VM) && unlikely(folio_mapped(folio))) {
		pr_alert("BUG: Bad page cache in process %s  pfn:%05lx\n",
			 current->comm, folio_pfn(folio));
		dump_page(&folio->page, "still mapped when deleted");
		dump_stack();
		add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);

		if (mapping_exiting(mapping) && !folio_test_large(folio)) {
			int mapcount = page_mapcount(&folio->page);

			if (folio_ref_count(folio) >= mapcount + 2) {
				/*
				 * All vmas have already been torn down, so it's
				 * a good bet that actually the page is unmapped
				 * and we'd rather not leak it: if we're wrong,
				 * another bad page check should catch it later.
				 */
				page_mapcount_reset(&folio->page);
				folio_ref_sub(folio, mapcount);
			}
		}
	}

	/* hugetlb folios do not participate in page cache accounting. */
	if (folio_test_hugetlb(folio))
		return;

	nr = folio_nr_pages(folio);

	__lruvec_stat_mod_folio(folio, NR_FILE_PAGES, -nr);
	if (folio_test_swapbacked(folio)) {
		__lruvec_stat_mod_folio(folio, NR_SHMEM, -nr);
		if (folio_test_pmd_mappable(folio))
			__lruvec_stat_mod_folio(folio, NR_SHMEM_THPS, -nr);
	} else if (folio_test_pmd_mappable(folio)) {
		__lruvec_stat_mod_folio(folio, NR_FILE_THPS, -nr);
		filemap_nr_thps_dec(mapping);
	}

	/*
	 * At this point folio must be either written or cleaned by
	 * truncate.  Dirty folio here signals a bug and loss of
	 * unwritten data - on ordinary filesystems.
	 *
	 * But it's harmless on in-memory filesystems like tmpfs; and can
	 * occur when a driver which did get_user_pages() sets page dirty
	 * before putting it, while the inode is being finally evicted.
	 *
	 * Below fixes dirty accounting after removing the folio entirely
	 * but leaves the dirty flag set: it has no effect for truncated
	 * folio and anyway will be cleared before returning folio to
	 * buddy allocator.
	 */
	if (WARN_ON_ONCE(folio_test_dirty(folio) &&
			 mapping_can_writeback(mapping)))
		folio_account_cleaned(folio, inode_to_wb(mapping->host));
}

/*
 * Delete a page from the page cache and free it. Caller has to make
 * sure the page is locked and that nobody else uses it - or that usage
 * is safe.  The caller must hold the i_pages lock.
 */
void __filemap_remove_folio(struct folio *folio, void *shadow)
{
	struct address_space *mapping = folio->mapping;

	trace_mm_filemap_delete_from_page_cache(folio);
	filemap_unaccount_folio(mapping, folio);
	page_cache_delete(mapping, folio, shadow);
}

void filemap_free_folio(struct address_space *mapping, struct folio *folio)
{
	void (*freepage)(struct page *);
	int refs = 1;

	freepage = mapping->a_ops->freepage;
	if (freepage)
		freepage(&folio->page);

	if (folio_test_large(folio) && !folio_test_hugetlb(folio))
		refs = folio_nr_pages(folio);
	folio_put_refs(folio, refs);
}

/**
 * filemap_remove_folio - Remove folio from page cache.
 * @folio: The folio.
 *
 * This must be called only on folios that are locked and have been
 * verified to be in the page cache.  It will never put the folio into
 * the free list because the caller has a reference on the page.
 */
void filemap_remove_folio(struct folio *folio)
{
	struct address_space *mapping = folio->mapping;

	BUG_ON(!folio_test_locked(folio));
	spin_lock(&mapping->host->i_lock);
	xa_lock_irq(&mapping->i_pages);
	__filemap_remove_folio(folio, NULL);
	xa_unlock_irq(&mapping->i_pages);
	if (mapping_shrinkable(mapping))
		inode_add_lru(mapping->host);
	spin_unlock(&mapping->host->i_lock);

	filemap_free_folio(mapping, folio);
}

/*
 * page_cache_delete_batch - delete several folios from page cache
 * @mapping: the mapping to which folios belong
 * @fbatch: batch of folios to delete
 *
 * The function walks over mapping->i_pages and removes folios passed in
 * @fbatch from the mapping. The function expects @fbatch to be sorted
 * by page index and is optimised for it to be dense.
 * It tolerates holes in @fbatch (mapping entries at those indices are not
 * modified).
 *
 * The function expects the i_pages lock to be held.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
static void page_cache_delete_batch_for_file_area(struct address_space *mapping,
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

		/*page_cache_delete_batch()函数能进到这里folio一定不是NULL，但是现在无法保证，需要额外判定。但不能break，而是要去查找
		 *file_area里的下一个page。因为 xas_for_each()、xas_find()等函数现在从xarray tree查找的是file_area，不是page。只有
		 *找到的file_area是NULL，才能break结束查找。错了，原page_cache_delete_batch()函数for循环退出条件就有folio是NULL.
		 *又错了，xas_for_each(&xas, folio, ULONG_MAX)里如果找到NULL page就一直向后查找，不会终止循环。直到要查找的page索引
		 *大于max才会终止循环*/
		if(!folio){
			goto next_page;
			//break
		}
		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,folio,p_file_area,page_offset_in_file_area,((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area));

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
		if(!file_area_have_page(p_file_area) && mapping_exiting(mapping))
			xas_store(&xas, NULL);

		/*是调试的文件，打印调试信息*/
		if(mapping->rh_reserved3){
			printk("%s delete_batch mapping:0x%llx file_stat:0x%llx file_area:0x%llx status:0x%x page_offset_in_file_area:%d folio:0x%llx flags:0x%lx\n",__func__,(u64)mapping,(u64)p_file_stat_base,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,(u64)folio,folio->flags);
		}

next_page:
		page_offset_in_file_area ++;

		/*如果file_area里还有page没遍历到，goto find_page_from_file_area去查找file_area里的下一个page。否则到for循环开头
		 *xas_for_each()去查找下一个file_area，此时需要find_page_from_file_area清0，这个很关键*/
		if(page_offset_in_file_area < PAGE_COUNT_IN_AREA){
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
#endif
static void page_cache_delete_batch(struct address_space *mapping,
			     struct folio_batch *fbatch)
{
	XA_STATE(xas, &mapping->i_pages, fbatch->folios[0]->index);
	long total_pages = 0;
	int i = 0;
	struct folio *folio;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		/* 如果因为这个文件file_stat可能长时间没访问，此时cold_file_stat_delete()正并发释放mapping->rh_reserved1
		 * 指向的这个file_stat结构。或者也会并发执行cold_file_area_delete()释放folio所属的file_area结构，会不会有
		 * 并发问题，不会，分析见见page_cache_delete()*/
		smp_rmb();
		if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
			return page_cache_delete_batch_for_file_area(mapping,fbatch);
	}
#endif

	mapping_set_update(&xas, mapping);
	xas_for_each(&xas, folio, ULONG_MAX) {
		if (i >= folio_batch_count(fbatch))
			break;

		/* A swap/dax/shadow entry got inserted? Skip it. */
		if (xa_is_value(folio))
			continue;
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
			continue;
		}

		WARN_ON_ONCE(!folio_test_locked(folio));

		folio->mapping = NULL;
		/* Leave folio->index set: truncation lookup relies on it */

		i++;
		xas_store(&xas, NULL);
		total_pages += folio_nr_pages(folio);
	}
	mapping->nrpages -= total_pages;
}

void delete_from_page_cache_batch(struct address_space *mapping,
				  struct folio_batch *fbatch)
{
	int i;

	if (!folio_batch_count(fbatch))
		return;

	spin_lock(&mapping->host->i_lock);
	xa_lock_irq(&mapping->i_pages);
	for (i = 0; i < folio_batch_count(fbatch); i++) {
		struct folio *folio = fbatch->folios[i];

		trace_mm_filemap_delete_from_page_cache(folio);
		filemap_unaccount_folio(mapping, folio);
	}
	page_cache_delete_batch(mapping, fbatch);
	xa_unlock_irq(&mapping->i_pages);
	if (mapping_shrinkable(mapping))
		inode_add_lru(mapping->host);
	spin_unlock(&mapping->host->i_lock);

	for (i = 0; i < folio_batch_count(fbatch); i++)
		filemap_free_folio(mapping, fbatch->folios[i]);
}

int filemap_check_errors(struct address_space *mapping)
{
	int ret = 0;
	/* Check for outstanding write errors */
	if (test_bit(AS_ENOSPC, &mapping->flags) &&
	    test_and_clear_bit(AS_ENOSPC, &mapping->flags))
		ret = -ENOSPC;
	if (test_bit(AS_EIO, &mapping->flags) &&
	    test_and_clear_bit(AS_EIO, &mapping->flags))
		ret = -EIO;
	return ret;
}
EXPORT_SYMBOL(filemap_check_errors);

static int filemap_check_and_keep_errors(struct address_space *mapping)
{
	/* Check for outstanding write errors */
	if (test_bit(AS_EIO, &mapping->flags))
		return -EIO;
	if (test_bit(AS_ENOSPC, &mapping->flags))
		return -ENOSPC;
	return 0;
}

/**
 * filemap_fdatawrite_wbc - start writeback on mapping dirty pages in range
 * @mapping:	address space structure to write
 * @wbc:	the writeback_control controlling the writeout
 *
 * Call writepages on the mapping using the provided wbc to control the
 * writeout.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int filemap_fdatawrite_wbc(struct address_space *mapping,
			   struct writeback_control *wbc)
{
	int ret;

	if (!mapping_can_writeback(mapping) ||
	    !mapping_tagged(mapping, PAGECACHE_TAG_DIRTY))
		return 0;

	wbc_attach_fdatawrite_inode(wbc, mapping->host);
	ret = do_writepages(mapping, wbc);
	wbc_detach_inode(wbc);
	return ret;
}
EXPORT_SYMBOL(filemap_fdatawrite_wbc);

/**
 * __filemap_fdatawrite_range - start writeback on mapping dirty pages in range
 * @mapping:	address space structure to write
 * @start:	offset in bytes where the range starts
 * @end:	offset in bytes where the range ends (inclusive)
 * @sync_mode:	enable synchronous operation
 *
 * Start writeback against all of a mapping's dirty pages that lie
 * within the byte offsets <start, end> inclusive.
 *
 * If sync_mode is WB_SYNC_ALL then this is a "data integrity" operation, as
 * opposed to a regular memory cleansing writeback.  The difference between
 * these two operations is that if a dirty page/buffer is encountered, it must
 * be waited upon, and not just skipped over.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int __filemap_fdatawrite_range(struct address_space *mapping, loff_t start,
				loff_t end, int sync_mode)
{
	struct writeback_control wbc = {
		.sync_mode = sync_mode,
		.nr_to_write = LONG_MAX,
		.range_start = start,
		.range_end = end,
	};

	return filemap_fdatawrite_wbc(mapping, &wbc);
}

static inline int __filemap_fdatawrite(struct address_space *mapping,
	int sync_mode)
{
	return __filemap_fdatawrite_range(mapping, 0, LLONG_MAX, sync_mode);
}

int filemap_fdatawrite(struct address_space *mapping)
{
	return __filemap_fdatawrite(mapping, WB_SYNC_ALL);
}
EXPORT_SYMBOL(filemap_fdatawrite);

int filemap_fdatawrite_range(struct address_space *mapping, loff_t start,
				loff_t end)
{
	return __filemap_fdatawrite_range(mapping, start, end, WB_SYNC_ALL);
}
EXPORT_SYMBOL(filemap_fdatawrite_range);

/**
 * filemap_flush - mostly a non-blocking flush
 * @mapping:	target address_space
 *
 * This is a mostly non-blocking flush.  Not suitable for data-integrity
 * purposes - I/O may not be started against all dirty pages.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int filemap_flush(struct address_space *mapping)
{
	return __filemap_fdatawrite(mapping, WB_SYNC_NONE);
}
EXPORT_SYMBOL(filemap_flush);

/**
 * filemap_range_has_page - check if a page exists in range.
 * @mapping:           address space within which to check
 * @start_byte:        offset in bytes where the range starts
 * @end_byte:          offset in bytes where the range ends (inclusive)
 *
 * Find at least one page in the range supplied, usually used to check if
 * direct writing in this range will trigger a writeback.
 *
 * Return: %true if at least one page exists in the specified range,
 * %false otherwise.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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
#endif
bool filemap_range_has_page(struct address_space *mapping,
			   loff_t start_byte, loff_t end_byte)
{
	struct page *page;
	XA_STATE(xas, &mapping->i_pages, start_byte >> PAGE_SHIFT);
	pgoff_t max = end_byte >> PAGE_SHIFT;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
			return filemap_range_has_page_for_file_area(mapping,start_byte,end_byte);
	}
#endif

	if (end_byte < start_byte)
		return false;

	rcu_read_lock();
	for (;;) {
		page = xas_find(&xas, max);
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(page)){
		    if(0 == mapping->rh_reserved1)
			panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

		    printk("%s find folio:0x%llx\n",__func__,(u64)page);
		    //goto 前rcu必须解锁
		    rcu_read_unlock();
		    goto find_file_area;
		}
#endif
		if (xas_retry(&xas, page))
			continue;
		/* Shadow entries don't count */
		if (xa_is_value(page))
			continue;
		/*
		 * We don't need to try to pin this page; we're about to
		 * release the RCU lock anyway.  It is enough to know that
		 * there was a page here recently.
		 */
		break;
	}
	rcu_read_unlock();

	return page != NULL;
}
EXPORT_SYMBOL(filemap_range_has_page);

static void __filemap_fdatawait_range(struct address_space *mapping,
				     loff_t start_byte, loff_t end_byte)
{
	pgoff_t index = start_byte >> PAGE_SHIFT;
	pgoff_t end = end_byte >> PAGE_SHIFT;
	struct pagevec pvec;
	int nr_pages;

	if (end_byte < start_byte)
		return;

	pagevec_init(&pvec);
	while (index <= end) {
		unsigned i;

		nr_pages = pagevec_lookup_range_tag(&pvec, mapping, &index,
				end, PAGECACHE_TAG_WRITEBACK);
		if (!nr_pages)
			break;

		for (i = 0; i < nr_pages; i++) {
			struct page *page = pvec.pages[i];

			wait_on_page_writeback(page);
			ClearPageError(page);
		}
		pagevec_release(&pvec);
		cond_resched();
	}
}

/**
 * filemap_fdatawait_range - wait for writeback to complete
 * @mapping:		address space structure to wait for
 * @start_byte:		offset in bytes where the range starts
 * @end_byte:		offset in bytes where the range ends (inclusive)
 *
 * Walk the list of under-writeback pages of the given address space
 * in the given range and wait for all of them.  Check error status of
 * the address space and return it.
 *
 * Since the error status of the address space is cleared by this function,
 * callers are responsible for checking the return value and handling and/or
 * reporting the error.
 *
 * Return: error status of the address space.
 */
int filemap_fdatawait_range(struct address_space *mapping, loff_t start_byte,
			    loff_t end_byte)
{
	__filemap_fdatawait_range(mapping, start_byte, end_byte);
	return filemap_check_errors(mapping);
}
EXPORT_SYMBOL(filemap_fdatawait_range);

/**
 * filemap_fdatawait_range_keep_errors - wait for writeback to complete
 * @mapping:		address space structure to wait for
 * @start_byte:		offset in bytes where the range starts
 * @end_byte:		offset in bytes where the range ends (inclusive)
 *
 * Walk the list of under-writeback pages of the given address space in the
 * given range and wait for all of them.  Unlike filemap_fdatawait_range(),
 * this function does not clear error status of the address space.
 *
 * Use this function if callers don't handle errors themselves.  Expected
 * call sites are system-wide / filesystem-wide data flushers: e.g. sync(2),
 * fsfreeze(8)
 */
int filemap_fdatawait_range_keep_errors(struct address_space *mapping,
		loff_t start_byte, loff_t end_byte)
{
	__filemap_fdatawait_range(mapping, start_byte, end_byte);
	return filemap_check_and_keep_errors(mapping);
}
EXPORT_SYMBOL(filemap_fdatawait_range_keep_errors);

/**
 * file_fdatawait_range - wait for writeback to complete
 * @file:		file pointing to address space structure to wait for
 * @start_byte:		offset in bytes where the range starts
 * @end_byte:		offset in bytes where the range ends (inclusive)
 *
 * Walk the list of under-writeback pages of the address space that file
 * refers to, in the given range and wait for all of them.  Check error
 * status of the address space vs. the file->f_wb_err cursor and return it.
 *
 * Since the error status of the file is advanced by this function,
 * callers are responsible for checking the return value and handling and/or
 * reporting the error.
 *
 * Return: error status of the address space vs. the file->f_wb_err cursor.
 */
int file_fdatawait_range(struct file *file, loff_t start_byte, loff_t end_byte)
{
	struct address_space *mapping = file->f_mapping;

	__filemap_fdatawait_range(mapping, start_byte, end_byte);
	return file_check_and_advance_wb_err(file);
}
EXPORT_SYMBOL(file_fdatawait_range);

/**
 * filemap_fdatawait_keep_errors - wait for writeback without clearing errors
 * @mapping: address space structure to wait for
 *
 * Walk the list of under-writeback pages of the given address space
 * and wait for all of them.  Unlike filemap_fdatawait(), this function
 * does not clear error status of the address space.
 *
 * Use this function if callers don't handle errors themselves.  Expected
 * call sites are system-wide / filesystem-wide data flushers: e.g. sync(2),
 * fsfreeze(8)
 *
 * Return: error status of the address space.
 */
int filemap_fdatawait_keep_errors(struct address_space *mapping)
{
	__filemap_fdatawait_range(mapping, 0, LLONG_MAX);
	return filemap_check_and_keep_errors(mapping);
}
EXPORT_SYMBOL(filemap_fdatawait_keep_errors);

/* Returns true if writeback might be needed or already in progress. */
static bool mapping_needs_writeback(struct address_space *mapping)
{
	return mapping->nrpages;
}
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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
		page = (struct page *)p_file_area->pages[page_offset_in_file_area];
		/*page_cache_delete_batch()函数能进到这里folio一定不是NULL，但是现在无法保证，需要额外判定。但不能break，而是要去查找
		 *file_area里的下一个page。因为 xas_for_each()、xas_find()等函数现在从xarray tree查找的是file_area，不是page。只有
		 *找到的page是NULL，才能break结束查找*/
		if(!page){
			goto next_page;
			//break; 
		}
		folio_index_from_xa_index = (xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area;
		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,page,p_file_area,page_offset_in_file_area,folio_index_from_xa_index);

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
#endif
bool filemap_range_has_writeback(struct address_space *mapping,
				 loff_t start_byte, loff_t end_byte)
{
	XA_STATE(xas, &mapping->i_pages, start_byte >> PAGE_SHIFT);
	pgoff_t max = end_byte >> PAGE_SHIFT;
	struct page *page;
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
			return filemap_range_has_writeback_for_file_area(mapping,start_byte,end_byte);
	}
#endif

	if (end_byte < start_byte)
		return false;

	rcu_read_lock();
	xas_for_each(&xas, page, max) {
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(page)){
			if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find page:0x%llx\n",__func__,(u64)page);
			//goto 前rcu必须解锁
			rcu_read_unlock();
			goto find_file_area;
		}
#endif
		if (xas_retry(&xas, page))
			continue;
		if (xa_is_value(page))
			continue;
		if (PageDirty(page) || PageLocked(page) || PageWriteback(page))
			break;
	}
	rcu_read_unlock();
	return page != NULL;
}
EXPORT_SYMBOL_GPL(filemap_range_has_writeback);

/**
 * filemap_write_and_wait_range - write out & wait on a file range
 * @mapping:	the address_space for the pages
 * @lstart:	offset in bytes where the range starts
 * @lend:	offset in bytes where the range ends (inclusive)
 *
 * Write out and wait upon file offsets lstart->lend, inclusive.
 *
 * Note that @lend is inclusive (describes the last byte to be written) so
 * that this function can be used to write to the very end-of-file (end = -1).
 *
 * Return: error status of the address space.
 */
int filemap_write_and_wait_range(struct address_space *mapping,
				 loff_t lstart, loff_t lend)
{
	int err = 0;

	if (mapping_needs_writeback(mapping)) {
		err = __filemap_fdatawrite_range(mapping, lstart, lend,
						 WB_SYNC_ALL);
		/*
		 * Even if the above returned error, the pages may be
		 * written partially (e.g. -ENOSPC), so we wait for it.
		 * But the -EIO is special case, it may indicate the worst
		 * thing (e.g. bug) happened, so we avoid waiting for it.
		 */
		if (err != -EIO) {
			int err2 = filemap_fdatawait_range(mapping,
						lstart, lend);
			if (!err)
				err = err2;
		} else {
			/* Clear any previously stored errors */
			filemap_check_errors(mapping);
		}
	} else {
		err = filemap_check_errors(mapping);
	}
	return err;
}
EXPORT_SYMBOL(filemap_write_and_wait_range);

void __filemap_set_wb_err(struct address_space *mapping, int err)
{
	errseq_t eseq = errseq_set(&mapping->wb_err, err);

	trace_filemap_set_wb_err(mapping, eseq);
}
EXPORT_SYMBOL(__filemap_set_wb_err);

/**
 * file_check_and_advance_wb_err - report wb error (if any) that was previously
 * 				   and advance wb_err to current one
 * @file: struct file on which the error is being reported
 *
 * When userland calls fsync (or something like nfsd does the equivalent), we
 * want to report any writeback errors that occurred since the last fsync (or
 * since the file was opened if there haven't been any).
 *
 * Grab the wb_err from the mapping. If it matches what we have in the file,
 * then just quickly return 0. The file is all caught up.
 *
 * If it doesn't match, then take the mapping value, set the "seen" flag in
 * it and try to swap it into place. If it works, or another task beat us
 * to it with the new value, then update the f_wb_err and return the error
 * portion. The error at this point must be reported via proper channels
 * (a'la fsync, or NFS COMMIT operation, etc.).
 *
 * While we handle mapping->wb_err with atomic operations, the f_wb_err
 * value is protected by the f_lock since we must ensure that it reflects
 * the latest value swapped in for this file descriptor.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int file_check_and_advance_wb_err(struct file *file)
{
	int err = 0;
	errseq_t old = READ_ONCE(file->f_wb_err);
	struct address_space *mapping = file->f_mapping;

	/* Locklessly handle the common case where nothing has changed */
	if (errseq_check(&mapping->wb_err, old)) {
		/* Something changed, must use slow path */
		spin_lock(&file->f_lock);
		old = file->f_wb_err;
		err = errseq_check_and_advance(&mapping->wb_err,
						&file->f_wb_err);
		trace_file_check_and_advance_wb_err(file, old);
		spin_unlock(&file->f_lock);
	}

	/*
	 * We're mostly using this function as a drop in replacement for
	 * filemap_check_errors. Clear AS_EIO/AS_ENOSPC to emulate the effect
	 * that the legacy code would have had on these flags.
	 */
	clear_bit(AS_EIO, &mapping->flags);
	clear_bit(AS_ENOSPC, &mapping->flags);
	return err;
}
EXPORT_SYMBOL(file_check_and_advance_wb_err);

/**
 * file_write_and_wait_range - write out & wait on a file range
 * @file:	file pointing to address_space with pages
 * @lstart:	offset in bytes where the range starts
 * @lend:	offset in bytes where the range ends (inclusive)
 *
 * Write out and wait upon file offsets lstart->lend, inclusive.
 *
 * Note that @lend is inclusive (describes the last byte to be written) so
 * that this function can be used to write to the very end-of-file (end = -1).
 *
 * After writing out and waiting on the data, we check and advance the
 * f_wb_err cursor to the latest value, and return any errors detected there.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int file_write_and_wait_range(struct file *file, loff_t lstart, loff_t lend)
{
	int err = 0, err2;
	struct address_space *mapping = file->f_mapping;

	if (mapping_needs_writeback(mapping)) {
		err = __filemap_fdatawrite_range(mapping, lstart, lend,
						 WB_SYNC_ALL);
		/* See comment of filemap_write_and_wait() */
		if (err != -EIO)
			__filemap_fdatawait_range(mapping, lstart, lend);
	}
	err2 = file_check_and_advance_wb_err(file);
	if (!err)
		err = err2;
	return err;
}
EXPORT_SYMBOL(file_write_and_wait_range);

/**
 * replace_page_cache_page - replace a pagecache page with a new one
 * @old:	page to be replaced
 * @new:	page to replace with
 *
 * This function replaces a page in the pagecache with a new one.  On
 * success it acquires the pagecache reference for the new page and
 * drops it for the old page.  Both the old and new pages must be
 * locked.  This function does not add the new page to the LRU, the
 * caller must do that.
 *
 * The remove + add is atomic.  This function cannot fail.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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
	p_file_area->pages[page_offset_in_file_area] = fnew;
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
#endif
void replace_page_cache_page(struct page *old, struct page *new)
{
	struct folio *fold = page_folio(old);
	struct folio *fnew = page_folio(new);
	struct address_space *mapping = old->mapping;
	void (*freepage)(struct page *) = mapping->a_ops->freepage;
	pgoff_t offset = old->index;
	XA_STATE(xas, &mapping->i_pages, offset);

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
			return replace_page_cache_page_for_file_area(old,new);
	}
#endif

	VM_BUG_ON_PAGE(!PageLocked(old), old);
	VM_BUG_ON_PAGE(!PageLocked(new), new);
	VM_BUG_ON_PAGE(new->mapping, new);

	get_page(new);
	new->mapping = mapping;
	new->index = offset;

	mem_cgroup_migrate(fold, fnew);

	xas_lock_irq(&xas);
	xas_store(&xas, new);

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
EXPORT_SYMBOL_GPL(replace_page_cache_page);

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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
		void *entry, *old = NULL;

		if (order > folio_order(folio)){
			panic("%s order:%d folio_order:%d error !!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,order,folio_order(folio));
			xas_split_alloc(&xas, xa_load(xas.xa, xas.xa_index),
					order, gfp);
		}
		xas_lock_irq(&xas);
		/*file_stat可能会被方法删除，则分配一个新的file_stat，具体看cold_file_stat_delete()函数*/
		if(SUPPORT_FILE_AREA_INIT_OR_DELETE == mapping->rh_reserved1){
			//if(RB_EMPTY_ROOT(&mapping->i_mmap.rb_root))
			if(!mapping_mapped(mapping))
				p_file_stat_base  = file_stat_alloc_and_init(mapping,FILE_STAT_TINY_SMALL,0);
			else
				p_file_stat_base = add_mmap_file_stat_to_list(mapping,FILE_STAT_TINY_SMALL,0);

			if(!p_file_stat_base){
				xas_set_err(&xas, -ENOMEM);
				//goto error; 
				goto unlock;
			}
		}else
			p_file_stat_base = (struct file_stat_base *)mapping->rh_reserved1;

		if(file_stat_in_delete_base(p_file_stat_base))
			panic("%s %s %d file_stat:0x%llx status:0x%x in delete\n",__func__,current->comm,current->pid,(u64)p_file_stat_base,p_file_stat_base->file_stat_status);

		xas_for_each_conflict(&xas, entry) {
			old = entry;
			//xas_lock_irq加锁后，检测到待添加的file_area已经被其他进程并发添加到xarray tree了
			if (!xa_is_value(entry)) {

				//if(!p_file_area)从进来说明file_area已经非NULL，不用再判断
				//    goto ;
				p_file_area = entry_to_file_area(entry);

				//page已经添加到file_area了
				//if(NULL != p_file_area->pages[page_offset_in_file_area]){
				if(NULL != rcu_dereference(p_file_area->pages[page_offset_in_file_area])){
					xas_set_err(&xas, -EEXIST);
					goto unlock;
				}
				//file_area已经添加到xarray tree，但是page还没有赋值到file_area->pages[]数组
				goto find_file_area;
			}
		}
#if 0
		if (old) {
			if (shadowp)
				*shadowp = old;
			/* entry may have been split before we acquired lock */
			order = xa_get_order(xas.xa, xas.xa_index);
			if (order > folio_order(folio)) {
				/* How to handle large swap entries? */
				BUG_ON(shmem_mapping(mapping));
				xas_split(&xas, old, order);
				xas_reset(&xas);
			}
		}
#else
		*shadowp = NULL;
#endif
		//分配file_area
		p_file_area  = file_area_alloc_and_init(area_index_for_page,p_file_stat_base);
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
		set_file_area_page_bit(p_file_area,page_offset_in_file_area);
		FILE_AREA_PRINT1("%s mapping:0x%llx p_file_area:0x%llx folio:0x%llx index:%ld page_offset_in_file_area:%d\n",__func__,(u64)mapping,(u64)p_file_area,(u64)folio,index,page_offset_in_file_area);

		if(NULL != p_file_area->pages[page_offset_in_file_area])
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
     
	trace_mm_filemap_add_to_page_cache(folio);

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
#endif
noinline int __filemap_add_folio(struct address_space *mapping,
		struct folio *folio, pgoff_t index, gfp_t gfp, void **shadowp)
{
	XA_STATE(xas, &mapping->i_pages, index);
	int huge = folio_test_hugetlb(folio);
	bool charged = false;
	long nr = 1;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
	/*有多种异常文件entry会保存到文件xarray tree，一共是shadow entry、tmpfs/shmem swap entry、dax entry、匿名页swap entry。
	 *shadow entry已经在被我在page_cache_delete()和page_cache_delete_batch()规避，tmpfs/shmem swap entry、匿名页swap entry
	 *把对xarray tree槽位的赋值不在filemap.c的__filemap_add_folio函数，在其他文件。只剩下dax entry在这里显式指明。
	 * */
	if(/*!dax_mapping(mapping) && !shmem_mapping(mapping) &&*/IS_SUPPORT_FILE_AREA(mapping)){
		//smp_rmb();-----------这个内存屏障放到__filemap_add_folio_for_file_area()函数里了
		//if(IS_SUPPORT_FILE_AREA(mapping))
		    return __filemap_add_folio_for_file_area(mapping,folio,index,gfp,shadowp);
	}
#endif	
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_swapbacked(folio), folio);
	mapping_set_update(&xas, mapping);

	if (!huge) {
		int error = mem_cgroup_charge(folio, NULL, gfp);
		VM_BUG_ON_FOLIO(index & (folio_nr_pages(folio) - 1), folio);
		if (error)
			return error;
		charged = true;
		xas_set_order(&xas, index, folio_order(folio));
		nr = folio_nr_pages(folio);
	}

	gfp &= GFP_RECLAIM_MASK;
	folio_ref_add(folio, nr);
	folio->mapping = mapping;
	folio->index = xas.xa_index;

	do {
		unsigned int order = xa_get_order(xas.xa, xas.xa_index);
		void *entry, *old = NULL;

		if (order > folio_order(folio))
			xas_split_alloc(&xas, xa_load(xas.xa, xas.xa_index),
					order, gfp);
		xas_lock_irq(&xas);
		xas_for_each_conflict(&xas, entry) {
			old = entry;
			if (!xa_is_value(entry)) {
				xas_set_err(&xas, -EEXIST);
				goto unlock;
			}
		}

		if (old) {
			if (shadowp)
				*shadowp = old;
			/* entry may have been split before we acquired lock */
			order = xa_get_order(xas.xa, xas.xa_index);
			if (order > folio_order(folio)) {
				/* How to handle large swap entries? */
				BUG_ON(shmem_mapping(mapping));
				xas_split(&xas, old, order);
				xas_reset(&xas);
			}
		}

		xas_store(&xas, folio);
		if (xas_error(&xas))
			goto unlock;

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

	if (xas_error(&xas))
		goto error;

	trace_mm_filemap_add_to_page_cache(folio);
	return 0;
error:
	if (charged)
		mem_cgroup_uncharge(folio);
	folio->mapping = NULL;
	/* Leave page->index set: truncation relies upon it */
	folio_put_refs(folio, nr);
	return xas_error(&xas);
}
ALLOW_ERROR_INJECTION(__filemap_add_folio, ERRNO);

/**
 * add_to_page_cache_locked - add a locked page to the pagecache
 * @page:	page to add
 * @mapping:	the page's address_space
 * @offset:	page index
 * @gfp_mask:	page allocation mode
 *
 * This function is used to add a page to the pagecache. It must be locked.
 * This function does not add the page to the LRU.  The caller must do that.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int add_to_page_cache_locked(struct page *page, struct address_space *mapping,
		pgoff_t offset, gfp_t gfp_mask)
{
	return __filemap_add_folio(mapping, page_folio(page), offset,
					  gfp_mask, NULL);
}
EXPORT_SYMBOL(add_to_page_cache_locked);

int filemap_add_folio(struct address_space *mapping, struct folio *folio,
				pgoff_t index, gfp_t gfp)
{
	void *shadow = NULL;
	int ret;

	__folio_set_locked(folio);
	ret = __filemap_add_folio(mapping, folio, index, gfp, &shadow);
	if (unlikely(ret))
		__folio_clear_locked(folio);
	else {
		/*
		 * The folio might have been evicted from cache only
		 * recently, in which case it should be activated like
		 * any other repeatedly accessed folio.
		 * The exception is folios getting rewritten; evicting other
		 * data from the working set, only to cache data that will
		 * get overwritten with something else, is a waste of memory.
		 */
		WARN_ON_ONCE(folio_test_active(folio));
		if (!(gfp & __GFP_WRITE) && shadow)
			workingset_refault(folio, shadow);
		folio_add_lru(folio);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(filemap_add_folio);

#ifdef CONFIG_NUMA
struct folio *filemap_alloc_folio(gfp_t gfp, unsigned int order)
{
	int n;
	struct folio *folio;

	if (cpuset_do_page_mem_spread()) {
		unsigned int cpuset_mems_cookie;
		do {
			cpuset_mems_cookie = read_mems_allowed_begin();
			n = cpuset_mem_spread_node();
			folio = __folio_alloc_node(gfp, order, n);
		} while (!folio && read_mems_allowed_retry(cpuset_mems_cookie));

		return folio;
	}
	return folio_alloc(gfp, order);
}
EXPORT_SYMBOL(filemap_alloc_folio);
#endif

/*
 * filemap_invalidate_lock_two - lock invalidate_lock for two mappings
 *
 * Lock exclusively invalidate_lock of any passed mapping that is not NULL.
 *
 * @mapping1: the first mapping to lock
 * @mapping2: the second mapping to lock
 */
void filemap_invalidate_lock_two(struct address_space *mapping1,
				 struct address_space *mapping2)
{
	if (mapping1 > mapping2)
		swap(mapping1, mapping2);
	if (mapping1)
		down_write(&mapping1->invalidate_lock);
	if (mapping2 && mapping1 != mapping2)
		down_write_nested(&mapping2->invalidate_lock, 1);
}
EXPORT_SYMBOL(filemap_invalidate_lock_two);

/*
 * filemap_invalidate_unlock_two - unlock invalidate_lock for two mappings
 *
 * Unlock exclusive invalidate_lock of any passed mapping that is not NULL.
 *
 * @mapping1: the first mapping to unlock
 * @mapping2: the second mapping to unlock
 */
void filemap_invalidate_unlock_two(struct address_space *mapping1,
				   struct address_space *mapping2)
{
	if (mapping1)
		up_write(&mapping1->invalidate_lock);
	if (mapping2 && mapping1 != mapping2)
		up_write(&mapping2->invalidate_lock);
}
EXPORT_SYMBOL(filemap_invalidate_unlock_two);

/*
 * In order to wait for pages to become available there must be
 * waitqueues associated with pages. By using a hash table of
 * waitqueues where the bucket discipline is to maintain all
 * waiters on the same queue and wake all when any of the pages
 * become available, and for the woken contexts to check to be
 * sure the appropriate page became available, this saves space
 * at a cost of "thundering herd" phenomena during rare hash
 * collisions.
 */
#define PAGE_WAIT_TABLE_BITS 8
#define PAGE_WAIT_TABLE_SIZE (1 << PAGE_WAIT_TABLE_BITS)
static wait_queue_head_t folio_wait_table[PAGE_WAIT_TABLE_SIZE] __cacheline_aligned;

static wait_queue_head_t *folio_waitqueue(struct folio *folio)
{
	return &folio_wait_table[hash_ptr(folio, PAGE_WAIT_TABLE_BITS)];
}

void __init pagecache_init(void)
{
	int i;

	for (i = 0; i < PAGE_WAIT_TABLE_SIZE; i++)
		init_waitqueue_head(&folio_wait_table[i]);

	page_writeback_init();
}

/*
 * The page wait code treats the "wait->flags" somewhat unusually, because
 * we have multiple different kinds of waits, not just the usual "exclusive"
 * one.
 *
 * We have:
 *
 *  (a) no special bits set:
 *
 *	We're just waiting for the bit to be released, and when a waker
 *	calls the wakeup function, we set WQ_FLAG_WOKEN and wake it up,
 *	and remove it from the wait queue.
 *
 *	Simple and straightforward.
 *
 *  (b) WQ_FLAG_EXCLUSIVE:
 *
 *	The waiter is waiting to get the lock, and only one waiter should
 *	be woken up to avoid any thundering herd behavior. We'll set the
 *	WQ_FLAG_WOKEN bit, wake it up, and remove it from the wait queue.
 *
 *	This is the traditional exclusive wait.
 *
 *  (c) WQ_FLAG_EXCLUSIVE | WQ_FLAG_CUSTOM:
 *
 *	The waiter is waiting to get the bit, and additionally wants the
 *	lock to be transferred to it for fair lock behavior. If the lock
 *	cannot be taken, we stop walking the wait queue without waking
 *	the waiter.
 *
 *	This is the "fair lock handoff" case, and in addition to setting
 *	WQ_FLAG_WOKEN, we set WQ_FLAG_DONE to let the waiter easily see
 *	that it now has the lock.
 */
static int wake_page_function(wait_queue_entry_t *wait, unsigned mode, int sync, void *arg)
{
	unsigned int flags;
	struct wait_page_key *key = arg;
	struct wait_page_queue *wait_page
		= container_of(wait, struct wait_page_queue, wait);

	if (!wake_page_match(wait_page, key))
		return 0;

	/*
	 * If it's a lock handoff wait, we get the bit for it, and
	 * stop walking (and do not wake it up) if we can't.
	 */
	flags = wait->flags;
	if (flags & WQ_FLAG_EXCLUSIVE) {
		if (test_bit(key->bit_nr, &key->folio->flags))
			return -1;
		if (flags & WQ_FLAG_CUSTOM) {
			if (test_and_set_bit(key->bit_nr, &key->folio->flags))
				return -1;
			flags |= WQ_FLAG_DONE;
		}
	}

	/*
	 * We are holding the wait-queue lock, but the waiter that
	 * is waiting for this will be checking the flags without
	 * any locking.
	 *
	 * So update the flags atomically, and wake up the waiter
	 * afterwards to avoid any races. This store-release pairs
	 * with the load-acquire in folio_wait_bit_common().
	 */
	smp_store_release(&wait->flags, flags | WQ_FLAG_WOKEN);
	wake_up_state(wait->private, mode);

	/*
	 * Ok, we have successfully done what we're waiting for,
	 * and we can unconditionally remove the wait entry.
	 *
	 * Note that this pairs with the "finish_wait()" in the
	 * waiter, and has to be the absolute last thing we do.
	 * After this list_del_init(&wait->entry) the wait entry
	 * might be de-allocated and the process might even have
	 * exited.
	 */
	list_del_init_careful(&wait->entry);
	return (flags & WQ_FLAG_EXCLUSIVE) != 0;
}

static void folio_wake_bit(struct folio *folio, int bit_nr)
{
	wait_queue_head_t *q = folio_waitqueue(folio);
	struct wait_page_key key;
	unsigned long flags;
	wait_queue_entry_t bookmark;

	key.folio = folio;
	key.bit_nr = bit_nr;
	key.page_match = 0;

	bookmark.flags = 0;
	bookmark.private = NULL;
	bookmark.func = NULL;
	INIT_LIST_HEAD(&bookmark.entry);

	spin_lock_irqsave(&q->lock, flags);
	__wake_up_locked_key_bookmark(q, TASK_NORMAL, &key, &bookmark);

	while (bookmark.flags & WQ_FLAG_BOOKMARK) {
		/*
		 * Take a breather from holding the lock,
		 * allow pages that finish wake up asynchronously
		 * to acquire the lock and remove themselves
		 * from wait queue
		 */
		spin_unlock_irqrestore(&q->lock, flags);
		cpu_relax();
		spin_lock_irqsave(&q->lock, flags);
		__wake_up_locked_key_bookmark(q, TASK_NORMAL, &key, &bookmark);
	}

	/*
	 * It's possible to miss clearing waiters here, when we woke our page
	 * waiters, but the hashed waitqueue has waiters for other pages on it.
	 * That's okay, it's a rare case. The next waker will clear it.
	 *
	 * Note that, depending on the page pool (buddy, hugetlb, ZONE_DEVICE,
	 * other), the flag may be cleared in the course of freeing the page;
	 * but that is not required for correctness.
	 */
	if (!waitqueue_active(q) || !key.page_match)
		folio_clear_waiters(folio);

	spin_unlock_irqrestore(&q->lock, flags);
}

static void folio_wake(struct folio *folio, int bit)
{
	if (!folio_test_waiters(folio))
		return;
	folio_wake_bit(folio, bit);
}

/*
 * A choice of three behaviors for folio_wait_bit_common():
 */
enum behavior {
	EXCLUSIVE,	/* Hold ref to page and take the bit when woken, like
			 * __folio_lock() waiting on then setting PG_locked.
			 */
	SHARED,		/* Hold ref to page and check the bit when woken, like
			 * folio_wait_writeback() waiting on PG_writeback.
			 */
	DROP,		/* Drop ref to page before wait, no check when woken,
			 * like folio_put_wait_locked() on PG_locked.
			 */
};

/*
 * Attempt to check (or get) the folio flag, and mark us done
 * if successful.
 */
static inline bool folio_trylock_flag(struct folio *folio, int bit_nr,
					struct wait_queue_entry *wait)
{
	if (wait->flags & WQ_FLAG_EXCLUSIVE) {
		if (test_and_set_bit(bit_nr, &folio->flags))
			return false;
	} else if (test_bit(bit_nr, &folio->flags))
		return false;

	wait->flags |= WQ_FLAG_WOKEN | WQ_FLAG_DONE;
	return true;
}

/* How many times do we accept lock stealing from under a waiter? */
int sysctl_page_lock_unfairness = 5;

static inline int folio_wait_bit_common(struct folio *folio, int bit_nr,
		int state, enum behavior behavior)
{
	wait_queue_head_t *q = folio_waitqueue(folio);
	int unfairness = sysctl_page_lock_unfairness;
	struct wait_page_queue wait_page;
	wait_queue_entry_t *wait = &wait_page.wait;
	bool thrashing = false;
	bool delayacct = false;
	unsigned long pflags;

	if (bit_nr == PG_locked &&
	    !folio_test_uptodate(folio) && folio_test_workingset(folio)) {
		if (!folio_test_swapbacked(folio)) {
			delayacct_thrashing_start();
			delayacct = true;
		}
		psi_memstall_enter(&pflags);
		thrashing = true;
	}

	init_wait(wait);
	wait->func = wake_page_function;
	wait_page.folio = folio;
	wait_page.bit_nr = bit_nr;

repeat:
	wait->flags = 0;
	if (behavior == EXCLUSIVE) {
		wait->flags = WQ_FLAG_EXCLUSIVE;
		if (--unfairness < 0)
			wait->flags |= WQ_FLAG_CUSTOM;
	}

	/*
	 * Do one last check whether we can get the
	 * page bit synchronously.
	 *
	 * Do the folio_set_waiters() marking before that
	 * to let any waker we _just_ missed know they
	 * need to wake us up (otherwise they'll never
	 * even go to the slow case that looks at the
	 * page queue), and add ourselves to the wait
	 * queue if we need to sleep.
	 *
	 * This part needs to be done under the queue
	 * lock to avoid races.
	 */
	spin_lock_irq(&q->lock);
	folio_set_waiters(folio);
	if (!folio_trylock_flag(folio, bit_nr, wait))
		__add_wait_queue_entry_tail(q, wait);
	spin_unlock_irq(&q->lock);

	/*
	 * From now on, all the logic will be based on
	 * the WQ_FLAG_WOKEN and WQ_FLAG_DONE flag, to
	 * see whether the page bit testing has already
	 * been done by the wake function.
	 *
	 * We can drop our reference to the folio.
	 */
	if (behavior == DROP)
		folio_put(folio);

	/*
	 * Note that until the "finish_wait()", or until
	 * we see the WQ_FLAG_WOKEN flag, we need to
	 * be very careful with the 'wait->flags', because
	 * we may race with a waker that sets them.
	 */
	for (;;) {
		unsigned int flags;

		set_current_state(state);

		/* Loop until we've been woken or interrupted */
		flags = smp_load_acquire(&wait->flags);
		if (!(flags & WQ_FLAG_WOKEN)) {
			if (signal_pending_state(state, current))
				break;

			io_schedule();
			continue;
		}

		/* If we were non-exclusive, we're done */
		if (behavior != EXCLUSIVE)
			break;

		/* If the waker got the lock for us, we're done */
		if (flags & WQ_FLAG_DONE)
			break;

		/*
		 * Otherwise, if we're getting the lock, we need to
		 * try to get it ourselves.
		 *
		 * And if that fails, we'll have to retry this all.
		 */
		if (unlikely(test_and_set_bit(bit_nr, folio_flags(folio, 0))))
			goto repeat;

		wait->flags |= WQ_FLAG_DONE;
		break;
	}

	/*
	 * If a signal happened, this 'finish_wait()' may remove the last
	 * waiter from the wait-queues, but the folio waiters bit will remain
	 * set. That's ok. The next wakeup will take care of it, and trying
	 * to do it here would be difficult and prone to races.
	 */
	finish_wait(q, wait);

	if (thrashing) {
		if (delayacct)
			delayacct_thrashing_end();
		psi_memstall_leave(&pflags);
	}

	/*
	 * NOTE! The wait->flags weren't stable until we've done the
	 * 'finish_wait()', and we could have exited the loop above due
	 * to a signal, and had a wakeup event happen after the signal
	 * test but before the 'finish_wait()'.
	 *
	 * So only after the finish_wait() can we reliably determine
	 * if we got woken up or not, so we can now figure out the final
	 * return value based on that state without races.
	 *
	 * Also note that WQ_FLAG_WOKEN is sufficient for a non-exclusive
	 * waiter, but an exclusive one requires WQ_FLAG_DONE.
	 */
	if (behavior == EXCLUSIVE)
		return wait->flags & WQ_FLAG_DONE ? 0 : -EINTR;

	return wait->flags & WQ_FLAG_WOKEN ? 0 : -EINTR;
}

#ifdef CONFIG_MIGRATION
/**
 * migration_entry_wait_on_locked - Wait for a migration entry to be removed
 * @entry: migration swap entry.
 * @ptep: mapped pte pointer. Will return with the ptep unmapped. Only required
 *        for pte entries, pass NULL for pmd entries.
 * @ptl: already locked ptl. This function will drop the lock.
 *
 * Wait for a migration entry referencing the given page to be removed. This is
 * equivalent to put_and_wait_on_page_locked(page, TASK_UNINTERRUPTIBLE) except
 * this can be called without taking a reference on the page. Instead this
 * should be called while holding the ptl for the migration entry referencing
 * the page.
 *
 * Returns after unmapping and unlocking the pte/ptl with pte_unmap_unlock().
 *
 * This follows the same logic as folio_wait_bit_common() so see the comments
 * there.
 */
void migration_entry_wait_on_locked(swp_entry_t entry, pte_t *ptep,
				spinlock_t *ptl)
{
	struct wait_page_queue wait_page;
	wait_queue_entry_t *wait = &wait_page.wait;
	bool thrashing = false;
	bool delayacct = false;
	unsigned long pflags;
	wait_queue_head_t *q;
	struct folio *folio = page_folio(pfn_swap_entry_to_page(entry));

	q = folio_waitqueue(folio);
	if (!folio_test_uptodate(folio) && folio_test_workingset(folio)) {
		if (!folio_test_swapbacked(folio)) {
			delayacct_thrashing_start();
			delayacct = true;
		}
		psi_memstall_enter(&pflags);
		thrashing = true;
	}

	init_wait(wait);
	wait->func = wake_page_function;
	wait_page.folio = folio;
	wait_page.bit_nr = PG_locked;
	wait->flags = 0;

	spin_lock_irq(&q->lock);
	folio_set_waiters(folio);
	if (!folio_trylock_flag(folio, PG_locked, wait))
		__add_wait_queue_entry_tail(q, wait);
	spin_unlock_irq(&q->lock);

	/*
	 * If a migration entry exists for the page the migration path must hold
	 * a valid reference to the page, and it must take the ptl to remove the
	 * migration entry. So the page is valid until the ptl is dropped.
	 */
	if (ptep)
		pte_unmap_unlock(ptep, ptl);
	else
		spin_unlock(ptl);

	for (;;) {
		unsigned int flags;

		set_current_state(TASK_UNINTERRUPTIBLE);

		/* Loop until we've been woken or interrupted */
		flags = smp_load_acquire(&wait->flags);
		if (!(flags & WQ_FLAG_WOKEN)) {
			if (signal_pending_state(TASK_UNINTERRUPTIBLE, current))
				break;

			io_schedule();
			continue;
		}
		break;
	}

	finish_wait(q, wait);

	if (thrashing) {
		if (delayacct)
			delayacct_thrashing_end();
		psi_memstall_leave(&pflags);
	}
}
#endif

void folio_wait_bit(struct folio *folio, int bit_nr)
{
	folio_wait_bit_common(folio, bit_nr, TASK_UNINTERRUPTIBLE, SHARED);
}
EXPORT_SYMBOL(folio_wait_bit);

int folio_wait_bit_killable(struct folio *folio, int bit_nr)
{
	return folio_wait_bit_common(folio, bit_nr, TASK_KILLABLE, SHARED);
}
EXPORT_SYMBOL(folio_wait_bit_killable);

/**
 * folio_put_wait_locked - Drop a reference and wait for it to be unlocked
 * @folio: The folio to wait for.
 * @state: The sleep state (TASK_KILLABLE, TASK_UNINTERRUPTIBLE, etc).
 *
 * The caller should hold a reference on @folio.  They expect the page to
 * become unlocked relatively soon, but do not wish to hold up migration
 * (for example) by holding the reference while waiting for the folio to
 * come unlocked.  After this function returns, the caller should not
 * dereference @folio.
 *
 * Return: 0 if the folio was unlocked or -EINTR if interrupted by a signal.
 */
int folio_put_wait_locked(struct folio *folio, int state)
{
	return folio_wait_bit_common(folio, PG_locked, state, DROP);
}

/**
 * folio_add_wait_queue - Add an arbitrary waiter to a folio's wait queue
 * @folio: Folio defining the wait queue of interest
 * @waiter: Waiter to add to the queue
 *
 * Add an arbitrary @waiter to the wait queue for the nominated @folio.
 */
void folio_add_wait_queue(struct folio *folio, wait_queue_entry_t *waiter)
{
	wait_queue_head_t *q = folio_waitqueue(folio);
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	__add_wait_queue_entry_tail(q, waiter);
	folio_set_waiters(folio);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL_GPL(folio_add_wait_queue);

#ifndef clear_bit_unlock_is_negative_byte

/*
 * PG_waiters is the high bit in the same byte as PG_lock.
 *
 * On x86 (and on many other architectures), we can clear PG_lock and
 * test the sign bit at the same time. But if the architecture does
 * not support that special operation, we just do this all by hand
 * instead.
 *
 * The read of PG_waiters has to be after (or concurrently with) PG_locked
 * being cleared, but a memory barrier should be unnecessary since it is
 * in the same byte as PG_locked.
 */
static inline bool clear_bit_unlock_is_negative_byte(long nr, volatile void *mem)
{
	clear_bit_unlock(nr, mem);
	/* smp_mb__after_atomic(); */
	return test_bit(PG_waiters, mem);
}

#endif

/**
 * folio_unlock - Unlock a locked folio.
 * @folio: The folio.
 *
 * Unlocks the folio and wakes up any thread sleeping on the page lock.
 *
 * Context: May be called from interrupt or process context.  May not be
 * called from NMI context.
 */
void folio_unlock(struct folio *folio)
{
	/* Bit 7 allows x86 to check the byte's sign bit */
	BUILD_BUG_ON(PG_waiters != 7);
	BUILD_BUG_ON(PG_locked > 7);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	if (clear_bit_unlock_is_negative_byte(PG_locked, folio_flags(folio, 0)))
		folio_wake_bit(folio, PG_locked);
}
EXPORT_SYMBOL(folio_unlock);

/**
 * folio_end_private_2 - Clear PG_private_2 and wake any waiters.
 * @folio: The folio.
 *
 * Clear the PG_private_2 bit on a folio and wake up any sleepers waiting for
 * it.  The folio reference held for PG_private_2 being set is released.
 *
 * This is, for example, used when a netfs folio is being written to a local
 * disk cache, thereby allowing writes to the cache for the same folio to be
 * serialised.
 */
void folio_end_private_2(struct folio *folio)
{
	VM_BUG_ON_FOLIO(!folio_test_private_2(folio), folio);
	clear_bit_unlock(PG_private_2, folio_flags(folio, 0));
	folio_wake_bit(folio, PG_private_2);
	folio_put(folio);
}
EXPORT_SYMBOL(folio_end_private_2);

/**
 * folio_wait_private_2 - Wait for PG_private_2 to be cleared on a folio.
 * @folio: The folio to wait on.
 *
 * Wait for PG_private_2 (aka PG_fscache) to be cleared on a folio.
 */
void folio_wait_private_2(struct folio *folio)
{
	while (folio_test_private_2(folio))
		folio_wait_bit(folio, PG_private_2);
}
EXPORT_SYMBOL(folio_wait_private_2);

/**
 * folio_wait_private_2_killable - Wait for PG_private_2 to be cleared on a folio.
 * @folio: The folio to wait on.
 *
 * Wait for PG_private_2 (aka PG_fscache) to be cleared on a folio or until a
 * fatal signal is received by the calling task.
 *
 * Return:
 * - 0 if successful.
 * - -EINTR if a fatal signal was encountered.
 */
int folio_wait_private_2_killable(struct folio *folio)
{
	int ret = 0;

	while (folio_test_private_2(folio)) {
		ret = folio_wait_bit_killable(folio, PG_private_2);
		if (ret < 0)
			break;
	}

	return ret;
}
EXPORT_SYMBOL(folio_wait_private_2_killable);

/**
 * folio_end_writeback - End writeback against a folio.
 * @folio: The folio.
 */
void folio_end_writeback(struct folio *folio)
{
	/*
	 * folio_test_clear_reclaim() could be used here but it is an
	 * atomic operation and overkill in this particular case. Failing
	 * to shuffle a folio marked for immediate reclaim is too mild
	 * a gain to justify taking an atomic operation penalty at the
	 * end of every folio writeback.
	 */
	if (folio_test_reclaim(folio)) {
		folio_clear_reclaim(folio);
		folio_rotate_reclaimable(folio);
	}

	/*
	 * Writeback does not hold a folio reference of its own, relying
	 * on truncation to wait for the clearing of PG_writeback.
	 * But here we must make sure that the folio is not freed and
	 * reused before the folio_wake().
	 */
	folio_get(folio);
	if (!__folio_end_writeback(folio))
		BUG();

	smp_mb__after_atomic();
	folio_wake(folio, PG_writeback);
	acct_reclaim_writeback(folio);
	folio_put(folio);
}
EXPORT_SYMBOL(folio_end_writeback);

/*
 * After completing I/O on a page, call this routine to update the page
 * flags appropriately
 */
void page_endio(struct page *page, bool is_write, int err)
{
	if (!is_write) {
		if (!err) {
			SetPageUptodate(page);
		} else {
			ClearPageUptodate(page);
			SetPageError(page);
		}
		unlock_page(page);
	} else {
		if (err) {
			struct address_space *mapping;

			SetPageError(page);
			mapping = page_mapping(page);
			if (mapping)
				mapping_set_error(mapping, err);
		}
		end_page_writeback(page);
	}
}
EXPORT_SYMBOL_GPL(page_endio);

/**
 * __folio_lock - Get a lock on the folio, assuming we need to sleep to get it.
 * @folio: The folio to lock
 */
void __folio_lock(struct folio *folio)
{
	folio_wait_bit_common(folio, PG_locked, TASK_UNINTERRUPTIBLE,
				EXCLUSIVE);
}
EXPORT_SYMBOL(__folio_lock);

int __folio_lock_killable(struct folio *folio)
{
	return folio_wait_bit_common(folio, PG_locked, TASK_KILLABLE,
					EXCLUSIVE);
}
EXPORT_SYMBOL_GPL(__folio_lock_killable);

static int __folio_lock_async(struct folio *folio, struct wait_page_queue *wait)
{
	struct wait_queue_head *q = folio_waitqueue(folio);
	int ret = 0;

	wait->folio = folio;
	wait->bit_nr = PG_locked;

	spin_lock_irq(&q->lock);
	__add_wait_queue_entry_tail(q, &wait->wait);
	folio_set_waiters(folio);
	ret = !folio_trylock(folio);
	/*
	 * If we were successful now, we know we're still on the
	 * waitqueue as we're still under the lock. This means it's
	 * safe to remove and return success, we know the callback
	 * isn't going to trigger.
	 */
	if (!ret)
		__remove_wait_queue(q, &wait->wait);
	else
		ret = -EIOCBQUEUED;
	spin_unlock_irq(&q->lock);
	return ret;
}

/*
 * Return values:
 * true - folio is locked; mmap_lock is still held.
 * false - folio is not locked.
 *     mmap_lock has been released (mmap_read_unlock(), unless flags had both
 *     FAULT_FLAG_ALLOW_RETRY and FAULT_FLAG_RETRY_NOWAIT set, in
 *     which case mmap_lock is still held.
 *
 * If neither ALLOW_RETRY nor KILLABLE are set, will always return true
 * with the folio locked and the mmap_lock unperturbed.
 */
bool __folio_lock_or_retry(struct folio *folio, struct mm_struct *mm,
			 unsigned int flags)
{
	if (fault_flag_allow_retry_first(flags)) {
		/*
		 * CAUTION! In this case, mmap_lock is not released
		 * even though return 0.
		 */
		if (flags & FAULT_FLAG_RETRY_NOWAIT)
			return false;

		mmap_read_unlock(mm);
		if (flags & FAULT_FLAG_KILLABLE)
			folio_wait_locked_killable(folio);
		else
			folio_wait_locked(folio);
		return false;
	}
	if (flags & FAULT_FLAG_KILLABLE) {
		bool ret;

		ret = __folio_lock_killable(folio);
		if (ret) {
			mmap_read_unlock(mm);
			return false;
		}
	} else {
		__folio_lock(folio);
	}

	return true;
}

/**
 * page_cache_next_miss() - Find the next gap in the page cache.
 * @mapping: Mapping.
 * @index: Index.
 * @max_scan: Maximum range to search.
 *
 * Search the range [index, min(index + max_scan - 1, ULONG_MAX)] for the
 * gap with the lowest index.
 *
 * This function may be called under the rcu_read_lock.  However, this will
 * not atomically search a snapshot of the cache at a single point in time.
 * For example, if a gap is created at index 5, then subsequently a gap is
 * created at index 10, page_cache_next_miss covering both indices may
 * return 10 if called under the rcu_read_lock.
 *
 * Return: The index of the gap if found, otherwise an index outside the
 * range specified (in which case 'return - index >= max_scan' will be true).
 * In the rare case of index wrap-around, 0 will be returned.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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
		//page是NULL则直接break，这个跟page_cache_next_miss函数原有的if (!entry)break 同理，即遇到第一个NULL page则break结束查找
		if(p_file_area->pages[page_offset_in_file_area] == NULL)
			break;

		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,p_file_area->pages[page_offset_in_file_area],p_file_area,page_offset_in_file_area,folio_index_from_xa_index);

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
#endif
pgoff_t page_cache_next_miss(struct address_space *mapping,
		pgoff_t index, unsigned long max_scan)
{
	XA_STATE(xas, &mapping->i_pages, index);
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
			return page_cache_next_miss_for_file_area(mapping,index,max_scan);
	}
#endif

	while (max_scan--) {
		void *entry = xas_next(&xas);
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(entry)){
			if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)entry);
			//goto 前rcu必须解锁
			//rcu_read_unlock();该函数里没有rcu lock，调用者自己添加了rcu_read_lock
			goto find_file_area;
		}
#endif
		if (!entry || xa_is_value(entry))
			break;
		if (xas.xa_index == 0)
			break;
	}

	return xas.xa_index;
}
EXPORT_SYMBOL(page_cache_next_miss);

/**
 * page_cache_prev_miss() - Find the previous gap in the page cache.
 * @mapping: Mapping.
 * @index: Index.
 * @max_scan: Maximum range to search.
 *
 * Search the range [max(index - max_scan + 1, 0), index] for the
 * gap with the highest index.
 *
 * This function may be called under the rcu_read_lock.  However, this will
 * not atomically search a snapshot of the cache at a single point in time.
 * For example, if a gap is created at index 10, then subsequently a gap is
 * created at index 5, page_cache_prev_miss() covering both indices may
 * return 5 if called under the rcu_read_lock.
 *
 * Return: The index of the gap if found, otherwise an index outside the
 * range specified (in which case 'index - return >= max_scan' will be true).
 * In the rare case of wrap-around, ULONG_MAX will be returned.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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
		//page是NULL则直接break，这个跟page_cache_prev_miss函数原有的if (!entry)break 同理，即遇到第一个NULL page则break结束查找
		if(p_file_area->pages[page_offset_in_file_area] == NULL)
			break;
		
		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,p_file_area->pages[page_offset_in_file_area],p_file_area,page_offset_in_file_area,folio_index_from_xa_index);

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
#endif
pgoff_t page_cache_prev_miss(struct address_space *mapping,
			     pgoff_t index, unsigned long max_scan)
{
	XA_STATE(xas, &mapping->i_pages, index);
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
			return page_cache_prev_miss_for_file_area(mapping,index,max_scan);
	}
#endif

	while (max_scan--) {
		void *entry = xas_prev(&xas);
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(entry)){
			if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)entry);
			//goto 前rcu必须解锁
			//rcu_read_unlock();该函数里没有rcu lock，调用者自己添加了rcu_read_lock
			goto find_file_area;
		}
#endif
		if (!entry || xa_is_value(entry))
			break;
		if (xas.xa_index == ULONG_MAX)
			break;
	}

	return xas.xa_index;
}
EXPORT_SYMBOL(page_cache_prev_miss);

/*
 * Lockless page cache protocol:
 * On the lookup side:
 * 1. Load the folio from i_pages
 * 2. Increment the refcount if it's not zero
 * 3. If the folio is not found by xas_reload(), put the refcount and retry
 *
 * On the removal side:
 * A. Freeze the page (by zeroing the refcount if nobody else has a reference)
 * B. Remove the page from i_pages
 * C. Return the page to the page allocator
 *
 * This means that any page may have its reference count temporarily
 * increased by a speculative page cache (or fast GUP) lookup as it can
 * be allocated by another user before the RCU grace period expires.
 * Because the refcount temporarily acquired here may end up being the
 * last refcount on the page, any page allocation must be freeable by
 * folio_put().
 */

/*
 * mapping_get_entry - Get a page cache entry.
 * @mapping: the address_space to search
 * @index: The page cache index.
 *
 * Looks up the page cache entry at @mapping & @index.  If it is a folio,
 * it is returned with an increased refcount.  If it is a shadow entry
 * of a previously evicted folio, or a swap entry from shmem/tmpfs,
 * it is returned without further action.
 *
 * Return: The folio, swap or shadow entry, %NULL if nothing is found.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
static void *mapping_get_entry_for_file_area(struct address_space *mapping, pgoff_t index)
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

		panic("%s mapping:0x%llx p_file_area:0x%llx error!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,(u64)mapping,(u64)p_file_area);
		/*xas_retry()里有xas->xa_node = XAS_RESTART，这个隐藏的很深，这样执行xas_next(&xas)时，if(xas_not_node(node))成立，直接从
		 *xarray tree按照老的xas->xa_index重新查找，不会再执行xas->xa_index++和xas->xa_offset++而从父节点直接获取下一个索引的成员了*/
		if (xas_retry(&xas, p_file_area))
			goto repeat;
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
	folio = p_file_area->pages[page_offset_in_file_area];

	//if (!folio || xa_is_value(folio))
	if (!folio /*|| xa_is_value(p_file_area)*/)//xa_is_value()只是看bit0是否是1，其他bit位不用管
		goto out;

#ifdef ASYNC_MEMORY_RECLAIM_DEBUG
find_folio:
#endif

	/*检测查找到的page是否正确，不是则crash*/
	CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,folio,p_file_area,page_offset_in_file_area,((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area));

	if (!folio_try_get_rcu(folio))
		goto repeat;

	//if (unlikely(folio != xas_reload(&xas))) {
	if (unlikely(folio != rcu_dereference(p_file_area->pages[page_offset_in_file_area]))) {
		folio_put(folio);
		goto repeat;
	}
	//统计page引用计数
	hot_file_update_file_status(mapping,p_file_stat_base,p_file_area,1,FILE_AREA_PAGE_IS_WRITE);

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
/*这个函数可以加入node cache机制*/
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
	}
out:
	rcu_read_unlock();
	FILE_AREA_PRINT("%s mapping:0x%llx p_file_area:0x%llx folio:0x%llx index:%ld\n",__func__,(u64)mapping,(u64)p_file_area,(u64)folio,index);

	return (void *)folio;
}
EXPORT_SYMBOL(get_folio_from_file_area_for_file_area);
#endif
static void *mapping_get_entry(struct address_space *mapping, pgoff_t index)
{
	XA_STATE(xas, &mapping->i_pages, index);
	struct folio *folio;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	/* 1:如果此时有进程在__filemap_add_folio()分配file_stat并赋值给mapping->rh_reserved1,则当前进程在当前函数
	 * 不能立即看到mapping->rh_reserved1被赋值了，还是老的值NULL。于是继续执行rcu_read_lock()后边的代码在xarray tree直接查找page
	 * 这样就出现错乱了。__filemap_add_folio()中向xarray tree中保存的是file_area，这里却是从xarray tree查找page。怎么避免？
	 * 此时下边的代码 folio = xas_load(&xas)或 folio = xas_next(&xas) 查找到的folio是file_area_entry，那就goto find_file_area
	 * 重新跳到filemap_get_read_batch_for_file_area()去xarray tree查找file_area
	 *
	 * 2:如果此时正好文件长时间没访问、page全被释放了然后释放了file_stat，最后赋值mapping->rh_reserved1=1，接着此时该文件
	 * 正好被访问，if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))不成立，执行下边folio = xas_load(&xas)按照原有方法从xarray tree直接查找page，
	 * 而不是执行mapping_get_entry_for_file_area()先查找file_area再查找再查找page。这样会不会有问题？因为xarray tree保存的是
	 * file_area指针，而现在执行folio = xas_load(&xas)返回的page其实是file_area指针。这就有问题了，file_area指针和page指针
	 * 就搞错乱了!!!!!!不会，因为此时xarray tree是空树，folio = xas_load(&xas)返回的一定是NULL，然后执行
	 * __filemap_add_folio->__filemap_add_folio_for_file_area()分配file_stat、file_area、page，不会有问题
	 *
	 * 3:还有一个重大隐藏bug，如果这里mapping->rh_reserved1大于1，if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))成立，但是执行到
	 * mapping_get_entry_for_file_area(mapping,index)时，file_stat正好因为长时间未被访问而释放掉，赋值
	 * mapping->rh_reserved1 = 1。然后在mapping_get_entry_for_file_area()等等一些列...for_file_area()函数里，要是用到
	 * p_file_stat = mapping->rh_reserved1，那就是使用p_file_stat=1这个非法指针而crash。当然，如果此时不用p_file_stat指针
	 * 就没事，因此此时xarray tree是空树，也会查询不到page而返回NULL，也不会有啥事。但是为了100%安全，还是要在
	 * mapping_get_entry_for_file_area()等等一些列...for_file_area()函数里，防护mapping->rh_reserved1是1的情况：遇到
	 * mapping->rh_reserved1 是1，直接返回NULL。!!!!!!!!!!!!!!!!!!!!又错了，又有一个隐藏bug，必须要在
	 * apping_get_entry_for_file_area()等等一些列...for_file_area()函数里,rcu_read_lock()后，判断 mapping->rh_reserved1
	 * 是否大于1，等于1或者file_stat_in_delete(p_file_stat)成立，说明cold_file_stat_delete()函数已经异步释放了这个file_stat。
	 * mapping->rh_reserved1大于1的话才能放心使用mapping->rh_reserved1指向的file_stat。因为rcu_read_lock()后，就不用担心
	 * mapping->rh_reserved1指向的file_stat结构被释放cold_file_stat_delete()函数rcu异步释放掉。!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * */
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)) 这个判断放到mapping_get_entry_for_file_area()里了，因为必须要rcu_read_lock()后判断
		    return mapping_get_entry_for_file_area(mapping,index);
	}
#endif	

	rcu_read_lock();
repeat:
	xas_reset(&xas);
	folio = xas_load(&xas);
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
	if(is_file_area_entry(folio)){
		if(0 == mapping->rh_reserved1)
			panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

		printk("%s find folio:0x%llx\n",__func__,(u64)folio);
		//goto 前rcu必须解锁
		rcu_read_unlock();
		goto find_file_area;
	}
#endif

	if (xas_retry(&xas, folio))
		goto repeat;
	/*
	 * A shadow entry of a recently evicted page, or a swap entry from
	 * shmem/tmpfs.  Return it without attempting to raise page count.
	 */
	if (!folio || xa_is_value(folio))
		goto out;

	if (!folio_try_get_rcu(folio))
		goto repeat;

	if (unlikely(folio != xas_reload(&xas))) {
		folio_put(folio);
		goto repeat;
	}
out:
	rcu_read_unlock();

	return folio;
}
/**
 * __filemap_get_folio - Find and get a reference to a folio.
 * @mapping: The address_space to search.
 * @index: The page index.
 * @fgp_flags: %FGP flags modify how the folio is returned.
 * @gfp: Memory allocation flags to use if %FGP_CREAT is specified.
 *
 * Looks up the page cache entry at @mapping & @index.
 *
 * @fgp_flags can be zero or more of these flags:
 *
 * * %FGP_ACCESSED - The folio will be marked accessed.
 * * %FGP_LOCK - The folio is returned locked.
 * * %FGP_ENTRY - If there is a shadow / swap / DAX entry, return it
 *   instead of allocating a new folio to replace it.
 * * %FGP_CREAT - If no page is present then a new page is allocated using
 *   @gfp and added to the page cache and the VM's LRU list.
 *   The page is returned locked and with an increased refcount.
 * * %FGP_FOR_MMAP - The caller wants to do its own locking dance if the
 *   page is already in cache.  If the page was allocated, unlock it before
 *   returning so the caller can do the same dance.
 * * %FGP_WRITE - The page will be written to by the caller.
 * * %FGP_NOFS - __GFP_FS will get cleared in gfp.
 * * %FGP_NOWAIT - Don't get blocked by page lock.
 * * %FGP_STABLE - Wait for the folio to be stable (finished writeback)
 *
 * If %FGP_LOCK or %FGP_CREAT are specified then the function may sleep even
 * if the %GFP flags specified for %FGP_CREAT are atomic.
 *
 * If there is a page cache page, it is returned with an increased refcount.
 *
 * Return: The found folio or %NULL otherwise.
 */
struct folio *__filemap_get_folio(struct address_space *mapping, pgoff_t index,
		int fgp_flags, gfp_t gfp)
{
	struct folio *folio;

repeat:
	folio = mapping_get_entry(mapping, index);
	if (xa_is_value(folio)) {
		if (fgp_flags & FGP_ENTRY)
			return folio;
		folio = NULL;
	}
	if (!folio)
		goto no_page;

	if (fgp_flags & FGP_LOCK) {
		if (fgp_flags & FGP_NOWAIT) {
			if (!folio_trylock(folio)) {
				folio_put(folio);
				return NULL;
			}
		} else {
			folio_lock(folio);
		}

		/* Has the page been truncated? */
		if (unlikely(folio->mapping != mapping)) {
			folio_unlock(folio);
			folio_put(folio);
			goto repeat;
		}
		VM_BUG_ON_FOLIO(!folio_contains(folio, index), folio);
	}

	if (fgp_flags & FGP_ACCESSED)
		folio_mark_accessed(folio);
	else if (fgp_flags & FGP_WRITE) {
		/* Clear idle flag for buffer write */
		if (folio_test_idle(folio))
			folio_clear_idle(folio);
	}

	if (fgp_flags & FGP_STABLE)
		folio_wait_stable(folio);
no_page:
	if (!folio && (fgp_flags & FGP_CREAT)) {
		int err;
		if ((fgp_flags & FGP_WRITE) && mapping_can_writeback(mapping))
			gfp |= __GFP_WRITE;
		if (fgp_flags & FGP_NOFS)
			gfp &= ~__GFP_FS;

		folio = filemap_alloc_folio(gfp, 0);
		if (!folio)
			return NULL;

		if (WARN_ON_ONCE(!(fgp_flags & (FGP_LOCK | FGP_FOR_MMAP))))
			fgp_flags |= FGP_LOCK;

		/* Init accessed so avoid atomic mark_page_accessed later */
		if (fgp_flags & FGP_ACCESSED)
			__folio_set_referenced(folio);

		err = filemap_add_folio(mapping, folio, index, gfp);
		if (unlikely(err)) {
			folio_put(folio);
			folio = NULL;
			if (err == -EEXIST)
				goto repeat;
		}

		/*
		 * filemap_add_folio locks the page, and for mmap
		 * we expect an unlocked page.
		 */
		if (folio && (fgp_flags & FGP_FOR_MMAP))
			folio_unlock(folio);
	}

	return folio;
}
EXPORT_SYMBOL(__filemap_get_folio);
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
/*find_get_entry函数原本目录是：以xas->xa_index为起始索引，去xarray tree查找第一个不是NULL的page，
 *如果找到索引大于max，直接返回NULL。因此，这个函数这样设计，先xas_find()查找file_area，这个file_area可能
 *没有一个page，那就goto retry继续查找，直到xas_find(xas, max)因查到的索引大于max返回NULL，该函数直接return。
 *如果xas_find()找到的file_area有page，则判断合法后直接return folio。然后下次你再执行改函数时，就不能直接执行
 *xas_find(xas, max)了，而要去刚才查到的page的file_area里，继续查找其他page。直到一个file_area的page全被获取到
 *才能执行xas_find(xas, max)去查找下一个file_area*/
static inline struct folio *find_get_entry_for_file_area(struct xa_state *xas, pgoff_t max,
		xa_mark_t mark,struct file_area **p_file_area,unsigned int *page_offset_in_file_area,struct address_space *mapping)
{
	struct folio *folio;
	//计算要查找的最大page索引对应的file_area索引
	pgoff_t file_area_max = max >> PAGE_COUNT_IN_AREA_SHIFT;
	unsigned long folio_index_from_xa_index;

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
	/*
	 * A shadow entry of a recently evicted page, a swap
	 * entry from shmem/tmpfs or a DAX entry.  Return it
	 * without attempting to raise page count.
	 */
	//if (!folio || xa_is_value(folio))//注释掉，放到下边判断
	//	return folio;
	*p_file_area = entry_to_file_area(*p_file_area);
	/*当文件iput()后，执行该函数，遇到没有file_area的page，则要强制把xarray tree剔除。原因是：
	 * iput_final->evict->truncate_inode_pages_final->truncate_inode_pages->truncate_inode_pages_range->find_lock_entries
	 *调用到该函数，mapping_exiting(mapping)成立。当遇到没有page的file_area，要强制执行xas_store(&xas, NULL)把file_area从xarray tree剔除。
	 *因为此时file_area没有page，则从find_lock_entries()保存到fbatch->folios[]数组file_area的page是0个，则从find_lock_entries函数返回
	 *truncate_inode_pages_range后，因为fbatch->folios[]数组没有保存该file_area的page，则不会执行
	 *delete_from_page_cache_batch(mapping, &fbatch)->page_cache_delete_batch()，把这个没有page的file_area从xarray tree剔除。于是只能在
	 *truncate_inode_pages_range->find_lock_entries调用到该函数时，遇到没有page的file_area，强制把file_area从xarray tree剔除了*/
	if(!file_area_have_page(*p_file_area) && mapping_exiting(mapping)){
		/*为了不干扰原有的xas，重新定义一个xas_del*/
		XA_STATE(xas_del, &mapping->i_pages, (*p_file_area)->start_index >> PAGE_COUNT_IN_AREA_SHIFT);
		/*需要用文件xarray tree的lock加锁，因为xas_store()操作必须要xarray tree加锁*/
		xas_lock_irq(&xas_del);
		xas_store(&xas_del, NULL);
		xas_unlock_irq(&xas_del);

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

	folio = (*p_file_area)->pages[*page_offset_in_file_area];
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
	CHECK_FOLIO_FROM_FILE_AREA_VALID(xas,folio,*p_file_area,*page_offset_in_file_area,folio_index_from_xa_index);

	if (!folio_try_get_rcu(folio))
		goto reset;

	//if (unlikely(folio != xas_reload(xas))) {
	if (unlikely(folio != rcu_dereference((*p_file_area)->pages[*page_offset_in_file_area]))) {
		folio_put(folio);
		goto reset;
	}

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
#endif
static inline struct folio *find_get_entry(struct xa_state *xas, pgoff_t max,
		xa_mark_t mark)
{
	struct folio *folio;

retry:
	if (mark == XA_PRESENT)
		folio = xas_find(xas, max);
	else
		folio = xas_find_marked(xas, max, mark);

	if (xas_retry(xas, folio))
		goto retry;
	/*
	 * A shadow entry of a recently evicted page, a swap
	 * entry from shmem/tmpfs or a DAX entry.  Return it
	 * without attempting to raise page count.
	 */
	if (!folio || xa_is_value(folio))
		return folio;

	if (!folio_try_get_rcu(folio))
		goto reset;

	if (unlikely(folio != xas_reload(xas))) {
		folio_put(folio);
		goto reset;
	}

	return folio;
reset:
	xas_reset(xas);
	goto retry;
}

/**
 * find_get_entries - gang pagecache lookup
 * @mapping:	The address_space to search
 * @start:	The starting page cache index
 * @end:	The final page index (inclusive).
 * @fbatch:	Where the resulting entries are placed.
 * @indices:	The cache indices corresponding to the entries in @entries
 *
 * find_get_entries() will search for and return a batch of entries in
 * the mapping.  The entries are placed in @fbatch.  find_get_entries()
 * takes a reference on any actual folios it returns.
 *
 * The entries have ascending indexes.  The indices may not be consecutive
 * due to not-present entries or large folios.
 *
 * Any shadow entries of evicted folios, or swap entries from
 * shmem/tmpfs, are included in the returned array.
 *
 * Return: The number of entries which were found.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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
#endif
unsigned find_get_entries(struct address_space *mapping, pgoff_t start,
		pgoff_t end, struct folio_batch *fbatch, pgoff_t *indices)
{
	XA_STATE(xas, &mapping->i_pages, start);
	struct folio *folio;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
		{
			/*如果fbatch->nr非0，说明下边for循环已经找到了一些page，那就清0失效，现在执行filemap_get_read_batch_for_file_area重新查找*/
			if(fbatch->nr)
				fbatch->nr = 0;

			return find_get_entries_for_file_area(mapping,start,end,fbatch,indices);
		}
	}
#endif

	rcu_read_lock();
	while ((folio = find_get_entry(&xas, end, XA_PRESENT)) != NULL) {
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(folio)){
			if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)folio);
			//goto 前rcu必须解锁
			rcu_read_unlock();
			goto find_file_area;
		}
#endif
		indices[fbatch->nr] = xas.xa_index;
		if (!folio_batch_add(fbatch, folio))
			break;
	}
	rcu_read_unlock();

	return folio_batch_count(fbatch);
}

/**
 * find_lock_entries - Find a batch of pagecache entries.
 * @mapping:	The address_space to search.
 * @start:	The starting page cache index.
 * @end:	The final page index (inclusive).
 * @fbatch:	Where the resulting entries are placed.
 * @indices:	The cache indices of the entries in @fbatch.
 *
 * find_lock_entries() will return a batch of entries from @mapping.
 * Swap, shadow and DAX entries are included.  Folios are returned
 * locked and with an incremented refcount.  Folios which are locked
 * by somebody else or under writeback are skipped.  Folios which are
 * partially outside the range are not returned.
 *
 * The entries have ascending indexes.  The indices may not be consecutive
 * due to not-present entries, large folios, folios which could not be
 * locked or folios under writeback.
 *
 * Return: The number of entries which were found.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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
#endif
unsigned find_lock_entries(struct address_space *mapping, pgoff_t start,
		pgoff_t end, struct folio_batch *fbatch, pgoff_t *indices)
{
	XA_STATE(xas, &mapping->i_pages, start);
	struct folio *folio;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
		{
			/*如果fbatch->nr非0，说明下边for循环已经找到了一些page，那就清0失效，现在执行filemap_get_read_batch_for_file_area重新查找*/
			if(fbatch->nr)
				fbatch->nr = 0;

			return find_lock_entries_for_file_area(mapping,start,end,fbatch,indices);
		}
	}
#endif

	rcu_read_lock();
	while ((folio = find_get_entry(&xas, end, XA_PRESENT))) {
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(folio)){
			if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)folio);
			//goto 前rcu必须解锁
			rcu_read_unlock();
			goto find_file_area;
		}
#endif
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
			VM_BUG_ON_FOLIO(!folio_contains(folio, xas.xa_index),
					folio);
		}
		indices[fbatch->nr] = xas.xa_index;
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

static inline
bool folio_more_pages(struct folio *folio, pgoff_t index, pgoff_t max)
{
	if (!folio_test_large(folio) || folio_test_hugetlb(folio))
		return false;
	if (index >= max)
		return false;
	return index < folio->index + folio_nr_pages(folio) - 1;
}

/**
 * find_get_pages_range - gang pagecache lookup
 * @mapping:	The address_space to search
 * @start:	The starting page index
 * @end:	The final page index (inclusive)
 * @nr_pages:	The maximum number of pages
 * @pages:	Where the resulting pages are placed
 *
 * find_get_pages_range() will search for and return a group of up to @nr_pages
 * pages in the mapping starting at index @start and up to index @end
 * (inclusive).  The pages are placed at @pages.  find_get_pages_range() takes
 * a reference against the returned pages.
 *
 * The search returns a group of mapping-contiguous pages with ascending
 * indexes.  There may be holes in the indices due to not-present pages.
 * We also update @start to index the next page for the traversal.
 *
 * Return: the number of pages which were found. If this number is
 * smaller than @nr_pages, the end of specified range has been
 * reached.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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
#endif
unsigned find_get_pages_range(struct address_space *mapping, pgoff_t *start,
			      pgoff_t end, unsigned int nr_pages,
			      struct page **pages)
{
	XA_STATE(xas, &mapping->i_pages, *start);
	struct folio *folio;
	unsigned ret = 0;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
			return find_get_pages_range_for_file_area(mapping,start,end,nr_pages,pages);
	}
#endif

	if (unlikely(!nr_pages))
		return 0;

	rcu_read_lock();
	while ((folio = find_get_entry(&xas, end, XA_PRESENT))) {
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(folio)){
			if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)folio);
			//goto 前rcu必须解锁
			rcu_read_unlock();
			goto find_file_area;
		}
#endif
		/* Skip over shadow, swap and DAX entries */
		if (xa_is_value(folio))
			continue;

again:
		pages[ret] = folio_file_page(folio, xas.xa_index);
		if (++ret == nr_pages) {
			*start = xas.xa_index + 1;
			goto out;
		}
		if (folio_more_pages(folio, xas.xa_index, end)) {
			xas.xa_index++;
			folio_ref_inc(folio);
			goto again;
		}
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

/**
 * find_get_pages_contig - gang contiguous pagecache lookup
 * @mapping:	The address_space to search
 * @index:	The starting page index
 * @nr_pages:	The maximum number of pages
 * @pages:	Where the resulting pages are placed
 *
 * find_get_pages_contig() works exactly like find_get_pages_range(),
 * except that the returned number of pages are guaranteed to be
 * contiguous.
 *
 * Return: the number of pages which were found.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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
		folio = p_file_area->pages[page_offset_in_file_area];
		/*这个跟filemap_get_read_batch()里for (folio = xas_load(&xas); folio; folio = xas_next(&xas))判断出folio是NULL则结束循环是一个效果*/
		if(!folio)
			break;

		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,folio,p_file_area,page_offset_in_file_area,((xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area));

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
#endif
unsigned find_get_pages_contig(struct address_space *mapping, pgoff_t index,
			       unsigned int nr_pages, struct page **pages)
{
	XA_STATE(xas, &mapping->i_pages, index);
	struct folio *folio;
	unsigned int ret = 0;
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
			return find_get_pages_contig_for_file_area(mapping,index,nr_pages,pages);
	}
#endif	

	if (unlikely(!nr_pages))
		return 0;

	rcu_read_lock();
	for (folio = xas_load(&xas); folio; folio = xas_next(&xas)) {
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(folio)){
			if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)folio);
			//goto 前rcu必须解锁
			rcu_read_unlock();
			goto find_file_area;
		}
#endif
		if (xas_retry(&xas, folio))
			continue;
		/*
		 * If the entry has been swapped out, we can stop looking.
		 * No current caller is looking for DAX entries.
		 */
		if (xa_is_value(folio))
			break;

		if (!folio_try_get_rcu(folio))
			goto retry;

		if (unlikely(folio != xas_reload(&xas)))
			goto put_page;

again:
		pages[ret] = folio_file_page(folio, xas.xa_index);
		if (++ret == nr_pages)
			break;
		if (folio_more_pages(folio, xas.xa_index, ULONG_MAX)) {
			xas.xa_index++;
			folio_ref_inc(folio);
			goto again;
		}
		continue;
put_page:
		folio_put(folio);
retry:
		xas_reset(&xas);
	}
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL(find_get_pages_contig);

/**
 * find_get_pages_range_tag - Find and return head pages matching @tag.
 * @mapping:	the address_space to search
 * @index:	the starting page index
 * @end:	The final page index (inclusive)
 * @tag:	the tag index
 * @nr_pages:	the maximum number of pages
 * @pages:	where the resulting pages are placed
 *
 * Like find_get_pages_range(), except we only return head pages which are
 * tagged with @tag.  @index is updated to the index immediately after the
 * last page we return, ready for the next iteration.
 *
 * Return: the number of pages which were found.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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
#endif
unsigned find_get_pages_range_tag(struct address_space *mapping, pgoff_t *index,
			pgoff_t end, xa_mark_t tag, unsigned int nr_pages,
			struct page **pages)
{
	XA_STATE(xas, &mapping->i_pages, *index);
	struct folio *folio;
	unsigned ret = 0;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
			return find_get_pages_range_tag_for_file_area(mapping,index,end,tag,nr_pages,pages);
	}
#endif

	if (unlikely(!nr_pages))
		return 0;

	rcu_read_lock();
	while ((folio = find_get_entry(&xas, end, tag))) {
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(folio)){
			if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)folio);
			//goto 前rcu必须解锁
			rcu_read_unlock();
			goto find_file_area;
		}
#endif
		/*
		 * Shadow entries should never be tagged, but this iteration
		 * is lockless so there is a window for page reclaim to evict
		 * a page we saw tagged.  Skip over it.
		 */
		if (xa_is_value(folio))
			continue;

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
EXPORT_SYMBOL(find_get_pages_range_tag);

/*
 * CD/DVDs are error prone. When a medium error occurs, the driver may fail
 * a _large_ part of the i/o request. Imagine the worst scenario:
 *
 *      ---R__________________________________________B__________
 *         ^ reading here                             ^ bad block(assume 4k)
 *
 * read(R) => miss => readahead(R...B) => media error => frustrating retries
 * => failing the whole request => read(R) => read(R+1) =>
 * readahead(R+1...B+1) => bang => read(R+2) => read(R+3) =>
 * readahead(R+3...B+2) => bang => read(R+3) => read(R+4) =>
 * readahead(R+4...B+3) => bang => read(R+4) => read(R+5) => ......
 *
 * It is going insane. Fix it by quickly scaling down the readahead size.
 */
static void shrink_readahead_size_eio(struct file_ra_state *ra)
{
	ra->ra_pages /= 4;
}

/*
 * filemap_get_read_batch - Get a batch of folios for read
 *
 * Get a batch of folios which represent a contiguous range of bytes in
 * the file.  No exceptional entries will be returned.  If @index is in
 * the middle of a folio, the entire folio will be returned.  The last
 * folio in the batch may have the readahead flag set or the uptodate flag
 * clear so that the caller can take the appropriate action.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL

/*这段代码不要删除，是第一版，代码 又乱、又长、又复杂。而算法精简过后的最新的代码 不仅精简，还没有打乱原有代码逻辑。
 *这重新说明：一个好的算法时多么重要*/
#if 0
static void filemap_get_read_batch_for_file_area(struct address_space *mapping,
		pgoff_t index, pgoff_t max, struct folio_batch *fbatch)
{
	//XA_STATE(xas, &mapping->i_pages, index);
	XA_STATE(xas, &mapping->i_pages, index>>PAGE_COUNT_IN_AREA_SHIFT);
	struct folio *folio;

	struct file_stat *p_file_stat;
	struct file_area *p_file_area;
	int page_offset_in_file_area;
	int page_offset_in_file_area_origin;
	int reset = 0;
	int first_file_area = 0;
	void * file_area_entry;

	rcu_read_lock();

    p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	if(!file_stat_in_delete(p_file_stat)){
		    //如果此时这个file_area正在被释放，这里还能正常被使用吗？用了rcu机制做防护，后续会写详细分析!!!!!!!!!!!!!!!!!!!!!
            p_file_area = find_file_area_from_xarray_cache_node(&xas,p_file_stat,index);
            if(p_file_area)
				goto find_file_area;
		    //xas->xa_offset = 0;
		    //xas->xa_node = XAS_RESTART;
    }

	file_area_entry = xas_load(&xas);

find_file_area:	
    //得到要查找的第一个page在file_area->pages[]数组里的下标
	page_offset_in_file_area = index & PAGE_COUNT_IN_AREA_MASK;
	page_offset_in_file_area_origin  = page_offset_in_file_area;

	//for (folio = xas_load(&xas); folio; folio = xas_next(&xas)) {
	while(1){
        
		if(page_offset_in_file_area == PAGE_COUNT_IN_AREA || reset){
            file_area_entry = xas_next(&xas);

			/*如果reset置1，从xarray tree重新查找上一个file_area，此时不能对page_offset_in_file_area清0，
			 *这样继续查找上一个page*/
			if(0 == reset){
                /*统计page引用计数。如果是第一次统计，page_offset_in_file_area_origin >=0，此时访问file_area的page的访问计数是
				 * page_offset_in_file_area - page_offset_in_file_area_origin。之后，file_area的page的访问计数是page_offset_in_file_area*/
				if(page_offset_in_file_area_origin == -1)
	                hot_file_update_file_status(mapping,p_file_stat,p_file_area,page_offset_in_file_area);
				else{//访问的第一个file_area的page，访问计数是page_offset_in_file_area - page_offset_in_file_area_origin
	                hot_file_update_file_status(mapping,p_file_stat,p_file_area,page_offset_in_file_area - page_offset_in_file_area_origin);
					page_offset_in_file_area_origin = -1;
				}

				page_offset_in_file_area = 0;
            }
		}

		//1:在查找第一个file_area时，要判断file_area合法，此时page_offset_in_file_area不一定是0
		//2:后续的file_area，只在查找第一个page时才判断file_area合法，查找剩下的3个page时没必要
		//3:触发了reset强制查找xarray tree，此时也要判断一次file_area合法
		if(first_file_area == 0 || page_offset_in_file_area == 0 || reset){
			if(first_file_area == 0)
				first_file_area = 1;

			if(reset)
				reset = 0;

		    if(!file_area_entry)
			    break;

			//if (xas_retry(&xas, folio))
			if (xas_retry(&xas, file_area_entry)){
				//置1，这样下个循环folio = xas_next(&xas)才会从xarray tree查找file_area
				reset = 1;
				continue;
			}	

			/*没必要再判断file_area是xa_is_value()，xas.xa_index > max的判断放到了下边。因为xas.xa_index是file_area的索引,不是page的索引*/
			//if (xas.xa_index > max || xa_is_value(folio))
			//if (xas.xa_index  > max || xa_is_value(p_file_area))
				//break;

			//if (xa_is_sibling(folio))
			if (xa_is_sibling(file_area_entry)){
                printk("%s xa_is_sibling:0x%llx!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,(u64)file_area_entry);
				break;
			}

			p_file_area = entry_to_file_area(file_area_entry);
        }
        folio = p_file_area->pages[page_offset_in_file_area];
		//其实这个folio->index > max的判断，浪费性能，前边废了很大劲找到这个page，结果却没用。完全可以在上一个page->index == max，然后直接退出循环就行了!!!!!!!!!!!!!!!!
		if(!folio || folio->index > max)
			break;

		if (!folio_try_get_rcu(folio))
			goto retry;

		//if (unlikely(folio != xas_reload(&xas)))
	    if (unlikely(folio != rcu_dereference(p_file_area->pages[page_offset_in_file_area]))) 
			goto put_folio;

		if (!folio_batch_add(fbatch, folio))
			break;
		if (!folio_test_uptodate(folio))
			break;
		if (folio_test_readahead(folio))
			break;

		if(folio_nr_pages(folio) > 1){
            printk("%s index:%ld folio_nr_pages:%ld!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,index,folio_nr_pages(folio));
		}
        /*folio代表单个page时，看着本质是xas->xa_index = folio->index，xas->xa_offset= folio->index & XA_CHUNK_MASK。
		 *这里的核心操作是，当folio->index大于64时，folio->index & XA_CHUNK_MASK后只取出不足64的部分，即在xarray tree槽位的偏移.
		 *但是folio = xas_next(&xas)里会判断出xas->xa_offset == 63后，会自动取下一个父节点查找page*/		
		//xas_advance(&xas, folio->index + folio_nr_pages(folio) - 1);

        //file_area的page索引加1，下轮循环从file_area->pages[]得到下一个page。如果大于3则要从xarray tree查找下一个file_area
		page_offset_in_file_area ++;
		continue;
put_folio:
		folio_put(folio);
retry:
		//这里xas->xa_node = XAS_RESTART，然后folio = xas_next(&xas)里只能从xarray tree重新查找一次file_area
		xas_reset(&xas);
		//置1，这样下个循环folio = xas_next(&xas)才会从xarray tree查找file_area
		reset = 1;
	}
	/*统计page引用计数。如果是第一次统计，page_offset_in_file_area_origin >=0，此时访问file_area的page的访问计数是
	 * page_offset_in_file_area - page_offset_in_file_area_origin。之后，file_area的page的访问计数是page_offset_in_file_area*/
	if(page_offset_in_file_area_origin == -1){
		if(page_offset_in_file_area)
		    hot_file_update_file_status(mapping,p_file_stat,p_file_area,page_offset_in_file_area);
	}
	else{//访问的第一个file_area的page，访问计数是page_offset_in_file_area - page_offset_in_file_area_origin
		hot_file_update_file_status(mapping,p_file_stat,p_file_area,page_offset_in_file_area - page_offset_in_file_area_origin);
	}

	//如果本次查找的page所在xarray tree的父节点变化了，则把最新的保存到mapping->rh_reserved2
	if(p_file_stat->xa_node_cache != xas.xa_node){
	    /*保存父节点node和这个node节点slots里最小的page索引。这两个赋值可能被多进程并发赋值，导致
	     *mapping->rh_reserved2和mapping->rh_reserved3 可能不是同一个node节点的，错乱了。这就有大问题了！
	     *没事，这种情况上边的if(page && page->index == offset)就会不成立了*/
	    p_file_stat->xa_node_cache = xas.xa_node;
	    p_file_stat->xa_node_cache_base_index = index & (~FILE_AREA_PAGE_COUNT_MASK);
	}

	rcu_read_unlock();
}
#endif
static void filemap_get_read_batch_for_file_area(struct address_space *mapping,
		pgoff_t index, pgoff_t max, struct folio_batch *fbatch)
{
	//XA_STATE(xas, &mapping->i_pages, index);
	XA_STATE(xas, &mapping->i_pages, index>>PAGE_COUNT_IN_AREA_SHIFT);
	struct folio *folio = NULL;
	/*保存最后一次超找的page所属file_area的父节点*/
	struct xa_node *xa_node_vaild = NULL;
    
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = index & PAGE_COUNT_IN_AREA_MASK;
	//struct file_stat *p_file_stat;
	struct file_stat_base *p_file_stat_base;
	struct file_area *p_file_area = NULL;
	unsigned int page_offset_in_file_area_origin = page_offset_in_file_area;
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
#ifdef ASYNC_MEMORY_RECLAIM_DEBUG
	//if(!file_stat_in_delete(p_file_stat) && IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping) && enable_xas_node_cache){
		    //如果此时这个file_area正在被释放，这里还能正常被使用吗？用了rcu机制做防护，后续会写详细分析!!!!!!!!!!!!!!!!!!!!!
            p_file_area = find_file_area_from_xarray_cache_node(&xas,p_file_stat_base,index);
            if(p_file_area){
				xarray_tree_node_cache_hit ++;
				goto find_page_from_file_area;
			}
		    //xas->xa_offset = 0;
		    //xas->xa_node = XAS_RESTART;
    }
#endif	

	//for (folio = xas_load(&xas); folio; folio = xas_next(&xas)) {
	for (p_file_area = xas_load(&xas); p_file_area; p_file_area = xas_next(&xas)) {

		/*之前得做if (xas_retry(&xas, folio))等3个if判断，现在只用做if(!is_file_area_entry(p_file_area))判断就行了。到这里
		 *的p_file_area一定不是NULL，不用做这个防护*/
		if(!is_file_area_entry(p_file_area)){
			/*异常情况使xa_node_vaild必须无效*/
			xa_node_vaild = NULL;

			panic("%s mapping:0x%llx p_file_area:0x%llx error!!!!!!!!!!!!!!!!!!!!!!!!1\n",__func__,(u64)mapping,(u64)p_file_area);
		    /*xas_retry()里有xas->xa_node = XAS_RESTART，这个隐藏的很深，这样执行xas_next(&xas)时，if(xas_not_node(node))成立，直接从
		     *xarray tree按照老的xas->xa_index重新查找，不会再执行xas->xa_index++和xas->xa_offset++而从父节点直接获取下一个索引的成员了*/
		    if (xas_retry(&xas, p_file_area))
			    continue;
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
		/*p_file_area和xa_node_vaild的必须同时赋值，保证file_area一定是xa_node_vaild这个父节点的。下边对
		 *p_file_stat->xa_node_cache要用到*/
		p_file_area = entry_to_file_area(p_file_area);
		xa_node_vaild = xas.xa_node;

#ifdef ASYNC_MEMORY_RECLAIM_DEBUG
		FILE_AREA_PRINT("%s mapping:0x%llx p_file_area:0x%llx xas.xa_index:%ld xas->xa_offset:%d xa_node_cache:0x%llx cache_base_index:%ld index:%ld\n",__func__,(u64)mapping,(u64)p_file_area,xas.xa_index,xas.xa_offset,(u64)p_file_stat_base->xa_node_cache,p_file_stat_base->xa_node_cache_base_index,index);
#else
		FILE_AREA_PRINT("%s mapping:0x%llx p_file_area:0x%llx xas.xa_index:%ld xas->xa_offset:%d index:%ld\n",__func__,(u64)mapping,(u64)p_file_area,xas.xa_index,xas.xa_offset,index);
#endif		

find_page_from_file_area:
		folio = p_file_area->pages[page_offset_in_file_area];
		/*这个跟filemap_get_read_batch()里for (folio = xas_load(&xas); folio; folio = xas_next(&xas))判断出folio是NULL则结束循环是一个效果*/
		if(!folio)
			break;

		folio_index_from_xa_index = (xas.xa_index << PAGE_COUNT_IN_AREA_SHIFT) + page_offset_in_file_area;
		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID(&xas,folio,p_file_area,page_offset_in_file_area,folio_index_from_xa_index);

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

        if(folio_nr_pages(folio) > 1){
            panic("%s index:%ld folio_nr_pages:%ld!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,index,folio_nr_pages(folio));
		}
        /*folio代表单个page时，看着本质是xas->xa_index = folio->index，xas->xa_offset= folio->index & XA_CHUNK_MASK。
		 *这里的核心操作是，当folio->index大于64时，folio->index & XA_CHUNK_MASK后只取出不足64的部分，即在xarray tree槽位的偏移.
		 *但是folio = xas_next(&xas)里会判断出xas->xa_offset == 63后，会自动取下一个父节点查找page*/		
		//xas_advance(&xas, folio->index + folio_nr_pages(folio) - 1);
        
//next_folio:
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
			/*统计page引用计数。如果是第一次统计，page_offset_in_file_area_origin >=0，此时访问file_area的page的访问计数是
			 *page_offset_in_file_area - page_offset_in_file_area_origin。之后，file_area的page的访问计数是page_offset_in_file_area，
			 *此时page_offset_in_file_area与PAGE_COUNT_IN_AREA相等*/
			if(page_offset_in_file_area_origin == -1)
				hot_file_update_file_status(mapping,p_file_stat_base,p_file_area,page_offset_in_file_area,FILE_AREA_PAGE_IS_READ);//page_offset_in_file_area
			else{
				/*访问的第一个file_area，page访问计数是page_offset_in_file_area - page_offset_in_file_area_origin*/
				hot_file_update_file_status(mapping,p_file_stat_base,p_file_area,page_offset_in_file_area - page_offset_in_file_area_origin,FILE_AREA_PAGE_IS_READ);
				page_offset_in_file_area_origin = -1;
			}
            
			//要查找下一个file_area了，page_offset_in_file_area要清0
			page_offset_in_file_area = 0;
		}

		continue;
put_folio:
		folio_put(folio);
retry:
		xas_reset(&xas);
	}
	
    /*如果前边for循环异常break了，就无法统计最后file_area的访问计数了，那就在这里统计*/
	if(page_offset_in_file_area_origin == -1){
		if(page_offset_in_file_area)/*可能这个file_area一个page都没有获取到，要过滤掉*/
			hot_file_update_file_status(mapping,p_file_stat_base,p_file_area,page_offset_in_file_area,FILE_AREA_PAGE_IS_READ);
	}
	else{//访问的第一个file_area就跳出for循环了，page访问计数是page_offset_in_file_area - page_offset_in_file_area_origin
		if(page_offset_in_file_area > page_offset_in_file_area_origin)/*这样才说明至少有一个page被获取了*/
		    hot_file_update_file_status(mapping,p_file_stat_base,p_file_area,page_offset_in_file_area - page_offset_in_file_area_origin,FILE_AREA_PAGE_IS_READ);
	}

	/*如果本次查找的page所在xarray tree的父节点变化了，则把最新的保存到mapping->rh_reserved2。实际测试表明，
	 *当查找不到page时，xas_load(&xas)->xas_start 里会给xas.xa_node赋值1，即XAS_BOUNDS。导致错误赋值给
	 *p_file_stat->xa_node_cache=1，导致后续非法指针crash。因此必须判断父节点的合法性!!!!!!!!。错了，错了，
	 *当走到这里时，xas.xa_node一定是1，因为上边的for循环退出条件一定是找不到page而退出for循环。此时
	 *xas.xa_node一定是1，那if(xa_is_node(xas.xa_node))一定不会成立。因此要把这个赋值放到for循环最后一次
	 找到有效page时，把xas.xa_node赋值给p_file_stat->xa_node_cache。最后的解决方法：每次找到file_area并对
	 p_file_area赋值时，也对xa_node_vaild赋值所在父节点node。保证 p_file_area和xa_node_vaild是一体的。然后到
	 这里时，p_file_area是最后一次查找的有效page的file_area，xa_node_vaild是它所在的父节点node，可以直接赋值*/
#ifdef ASYNC_MEMORY_RECLAIM_DEBUG	
	//if(xa_is_node(xas.xa_node) && (p_file_stat->xa_node_cache != xas.xa_node)){
	if(enable_xas_node_cache 
			&& p_file_area && xa_node_vaild && (p_file_stat_base->xa_node_cache != xa_node_vaild)){
	    /*保存父节点node和这个node节点slots里最小的page索引。这两个赋值可能被多进程并发赋值，导致
	     *mapping->rh_reserved2和mapping->rh_reserved3 可能不是同一个node节点的，错乱了。这就有大问题了！
	     *没事，这种情况上边的if(page && page->index == offset)就会不成立了*/
	    //p_file_stat->xa_node_cache = xa_node_vaild;
	    p_file_stat_base->xa_node_cache = xa_node_vaild;
		/*又有一个bug，必须要把当前父节点node的其实page索引赋值给xa_node_cache_base_index，而不是当前查找的起始page索引*/
	    //p_file_stat->xa_node_cache_base_index = index & (~FILE_AREA_PAGE_COUNT_MASK);
        //p_file_stat->xa_node_cache_base_index = p_file_area->start_index & (~FILE_AREA_PAGE_COUNT_MASK);
        p_file_stat_base->xa_node_cache_base_index = p_file_area->start_index & (~FILE_AREA_PAGE_COUNT_MASK);
	}
#endif

	rcu_read_unlock();
}
#endif
static void filemap_get_read_batch(struct address_space *mapping,
		pgoff_t index, pgoff_t max, struct folio_batch *fbatch)
{
	XA_STATE(xas, &mapping->i_pages, index);
	struct folio *folio;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	/*如果此时有进程在__filemap_add_folio()分配file_stat并赋值给mapping->rh_reserved1,则当前进程在filemap_get_read_batch函数
	 * 不能立即看到mapping->rh_reserved1被赋值了，还是老的值NULL。于是继续执行rcu_read_lock()后边的代码在xarray tree直接查找page
	 * 这样就出现错乱了。__filemap_add_folio()中向xarray tree中保存的是file_area，这里却是从xarray tree查找page。怎么避免？
	 * 此时下边的代码 folio = xas_load(&xas)或 folio = xas_next(&xas) 查找到的folio是file_area_entry，那就goto find_file_area
	 * 重新跳到filemap_get_read_batch_for_file_area()去xarray tree查找file_area
	 * */
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		/*如果iput释放inode时，mapping->rh_reserved1被设置NULL并有内存屏障smp_mb。然后，其他进程会立即分配这个inode和mapping。
		 *这里smp_rmb()就会获取到最新的mapping->rh_reserved1的值0，if就不成立了。这有效保证不使用已经释放的inode和mapping。这里
		 *是先if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))判断一次，保证过滤掉mapping->rh_reserved1是0的文件，不用执行smp_rmb()节省性能。然后再判断一次if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))*/

		//smp_rmb();这个内存屏障和if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))判断放到filemap_get_read_batch_for_file_area()里了，原因见mapping_get_entry()
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
		{
			/*如果fbatch->nr非0，说明下边for循环已经找到了一些page，那就清0失效，现在执行filemap_get_read_batch_for_file_area重新查找*/
			if(fbatch->nr)
				fbatch->nr = 0;
			return filemap_get_read_batch_for_file_area(mapping,index,max,fbatch);
		}
	}
#endif	
	rcu_read_lock();
	for (folio = xas_load(&xas); folio; folio = xas_next(&xas)) {
		
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(folio)){
			if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)folio);
			rcu_read_unlock();
			goto find_file_area;
		}
#endif
		if (xas_retry(&xas, folio))
			continue;
		if (xas.xa_index > max || xa_is_value(folio))
			break;
		if (xa_is_sibling(folio))
			break;
		if (!folio_try_get_rcu(folio))
			goto retry;

		if (unlikely(folio != xas_reload(&xas)))
			goto put_folio;

		if (!folio_batch_add(fbatch, folio))
			break;
		if (!folio_test_uptodate(folio))
			break;
		if (folio_test_readahead(folio))
			break;
		xas_advance(&xas, folio->index + folio_nr_pages(folio) - 1);
		continue;
put_folio:
		folio_put(folio);
retry:
		xas_reset(&xas);
	}
	rcu_read_unlock();
}
static int filemap_read_folio(struct file *file, struct address_space *mapping,
		struct folio *folio)
{
	bool workingset = folio_test_workingset(folio);
	unsigned long pflags;
	int error;

	/*
	 * A previous I/O error may have been due to temporary failures,
	 * eg. multipath errors.  PG_error will be set again if readpage
	 * fails.
	 */
	folio_clear_error(folio);

	/* Start the actual read. The read will unlock the page. */
	if (unlikely(workingset))
		psi_memstall_enter(&pflags);
	error = mapping->a_ops->readpage(file, &folio->page);
	if (unlikely(workingset))
		psi_memstall_leave(&pflags);
	if (error)
		return error;

	error = folio_wait_locked_killable(folio);
	if (error)
		return error;
	if (folio_test_uptodate(folio))
		return 0;
	shrink_readahead_size_eio(&file->f_ra);
	return -EIO;
}

static bool filemap_range_uptodate(struct address_space *mapping,
		loff_t pos, struct iov_iter *iter, struct folio *folio)
{
	int count;

	if (folio_test_uptodate(folio))
		return true;
	/* pipes can't handle partially uptodate pages */
	if (iov_iter_is_pipe(iter))
		return false;
	if (!mapping->a_ops->is_partially_uptodate)
		return false;
	if (mapping->host->i_blkbits >= folio_shift(folio))
		return false;

	count = iter->count;
	if (folio_pos(folio) > pos) {
		count -= folio_pos(folio) - pos;
		pos = 0;
	} else {
		pos -= folio_pos(folio);
	}

	return mapping->a_ops->is_partially_uptodate(folio, pos, count);
}

static int filemap_update_page(struct kiocb *iocb,
		struct address_space *mapping, struct iov_iter *iter,
		struct folio *folio)
{
	int error;

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (!filemap_invalidate_trylock_shared(mapping))
			return -EAGAIN;
	} else {
		filemap_invalidate_lock_shared(mapping);
	}

	if (!folio_trylock(folio)) {
		error = -EAGAIN;
		if (iocb->ki_flags & (IOCB_NOWAIT | IOCB_NOIO))
			goto unlock_mapping;
		if (!(iocb->ki_flags & IOCB_WAITQ)) {
			filemap_invalidate_unlock_shared(mapping);
			/*
			 * This is where we usually end up waiting for a
			 * previously submitted readahead to finish.
			 */
			folio_put_wait_locked(folio, TASK_KILLABLE);
			return AOP_TRUNCATED_PAGE;
		}
		error = __folio_lock_async(folio, iocb->ki_waitq);
		if (error)
			goto unlock_mapping;
	}

	error = AOP_TRUNCATED_PAGE;
	if (!folio->mapping)
		goto unlock;

	error = 0;
	if (filemap_range_uptodate(mapping, iocb->ki_pos, iter, folio))
		goto unlock;

	error = -EAGAIN;
	if (iocb->ki_flags & (IOCB_NOIO | IOCB_NOWAIT | IOCB_WAITQ))
		goto unlock;

	error = filemap_read_folio(iocb->ki_filp, mapping, folio);
	goto unlock_mapping;
unlock:
	folio_unlock(folio);
unlock_mapping:
	filemap_invalidate_unlock_shared(mapping);
	if (error == AOP_TRUNCATED_PAGE)
		folio_put(folio);
	return error;
}

static int filemap_create_folio(struct file *file,
		struct address_space *mapping, pgoff_t index,
		struct folio_batch *fbatch)
{
	struct folio *folio;
	int error;

	folio = filemap_alloc_folio(mapping_gfp_mask(mapping), 0);
	if (!folio)
		return -ENOMEM;

	/*
	 * Protect against truncate / hole punch. Grabbing invalidate_lock
	 * here assures we cannot instantiate and bring uptodate new
	 * pagecache folios after evicting page cache during truncate
	 * and before actually freeing blocks.	Note that we could
	 * release invalidate_lock after inserting the folio into
	 * the page cache as the locked folio would then be enough to
	 * synchronize with hole punching. But there are code paths
	 * such as filemap_update_page() filling in partially uptodate
	 * pages or ->readahead() that need to hold invalidate_lock
	 * while mapping blocks for IO so let's hold the lock here as
	 * well to keep locking rules simple.
	 */
	filemap_invalidate_lock_shared(mapping);
	error = filemap_add_folio(mapping, folio, index,
			mapping_gfp_constraint(mapping, GFP_KERNEL));
	if (error == -EEXIST)
		error = AOP_TRUNCATED_PAGE;
	if (error)
		goto error;

	error = filemap_read_folio(file, mapping, folio);
	if (error)
		goto error;

	filemap_invalidate_unlock_shared(mapping);
	folio_batch_add(fbatch, folio);
	return 0;
error:
	filemap_invalidate_unlock_shared(mapping);
	folio_put(folio);
	return error;
}

static int filemap_readahead(struct kiocb *iocb, struct file *file,
		struct address_space *mapping, struct folio *folio,
		pgoff_t last_index)
{
	DEFINE_READAHEAD(ractl, file, &file->f_ra, mapping, folio->index);

	if (iocb->ki_flags & IOCB_NOIO)
		return -EAGAIN;
	page_cache_async_ra(&ractl, folio, last_index - folio->index);
	return 0;
}

static int filemap_get_pages(struct kiocb *iocb, struct iov_iter *iter,
		struct folio_batch *fbatch)
{
	struct file *filp = iocb->ki_filp;
	struct address_space *mapping = filp->f_mapping;
	struct file_ra_state *ra = &filp->f_ra;
	pgoff_t index = iocb->ki_pos >> PAGE_SHIFT;
	pgoff_t last_index;
	struct folio *folio;
	int err = 0;

	last_index = DIV_ROUND_UP(iocb->ki_pos + iter->count, PAGE_SIZE);
retry:
	if (fatal_signal_pending(current))
		return -EINTR;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
	if(filp->f_path.dentry && !mapping->rh_reserved3){
		struct dentry *dentry,*parent;
		dentry = filp->f_path.dentry;
		parent = dentry->d_parent;
		if(parent){
			/* 是 /root/.vim/cache/LeaderF/frecency 的话if成立，设置mapping->rh_reserved3，后续写该
			 * 文件产生的脏页都添加调试信息，以跟踪该文件tag_pages_for_writeback() crash问题*/
			if((0 == strcmp(dentry->d_iname,"frecency")) && (0 == strcmp(parent->d_iname,"LeaderF")))
				mapping->rh_reserved3 = 1;
		}
	}
#endif

	filemap_get_read_batch(mapping, index, last_index, fbatch);
	if (!folio_batch_count(fbatch)) {
		if (iocb->ki_flags & IOCB_NOIO)
			return -EAGAIN;
		page_cache_sync_readahead(mapping, ra, filp, index,
				last_index - index);
		filemap_get_read_batch(mapping, index, last_index, fbatch);
	}
	if (!folio_batch_count(fbatch)) {
		if (iocb->ki_flags & (IOCB_NOWAIT | IOCB_WAITQ))
			return -EAGAIN;
		err = filemap_create_folio(filp, mapping,
				iocb->ki_pos >> PAGE_SHIFT, fbatch);
		if (err == AOP_TRUNCATED_PAGE)
			goto retry;
		return err;
	}

	folio = fbatch->folios[folio_batch_count(fbatch) - 1];
	if (folio_test_readahead(folio)) {
		err = filemap_readahead(iocb, filp, mapping, folio, last_index);
		if (err)
			goto err;
	}
	if (!folio_test_uptodate(folio)) {
		if ((iocb->ki_flags & IOCB_WAITQ) &&
		    folio_batch_count(fbatch) > 1)
			iocb->ki_flags |= IOCB_NOWAIT;
		err = filemap_update_page(iocb, mapping, iter, folio);
		if (err)
			goto err;
	}

	return 0;
err:
	if (err < 0)
		folio_put(folio);
	if (likely(--fbatch->nr))
		return 0;
	if (err == AOP_TRUNCATED_PAGE)
		goto retry;
	return err;
}

/**
 * filemap_read - Read data from the page cache.
 * @iocb: The iocb to read.
 * @iter: Destination for the data.
 * @already_read: Number of bytes already read by the caller.
 *
 * Copies data from the page cache.  If the data is not currently present,
 * uses the readahead and readpage address_space operations to fetch it.
 *
 * Return: Total number of bytes copied, including those already read by
 * the caller.  If an error happens before any bytes are copied, returns
 * a negative error number.
 */
ssize_t filemap_read(struct kiocb *iocb, struct iov_iter *iter,
		ssize_t already_read)
{
	struct file *filp = iocb->ki_filp;
	struct file_ra_state *ra = &filp->f_ra;
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	struct folio_batch fbatch;
	int i, error = 0;
	bool writably_mapped;
	loff_t isize, end_offset;

	if (unlikely(iocb->ki_pos >= inode->i_sb->s_maxbytes))
		return 0;
	if (unlikely(!iov_iter_count(iter)))
		return 0;

	iov_iter_truncate(iter, inode->i_sb->s_maxbytes);
	folio_batch_init(&fbatch);

	do {
		cond_resched();

		/*
		 * If we've already successfully copied some data, then we
		 * can no longer safely return -EIOCBQUEUED. Hence mark
		 * an async read NOWAIT at that point.
		 */
		if ((iocb->ki_flags & IOCB_WAITQ) && already_read)
			iocb->ki_flags |= IOCB_NOWAIT;

		if (unlikely(iocb->ki_pos >= i_size_read(inode)))
			break;

		error = filemap_get_pages(iocb, iter, &fbatch);
		if (error < 0)
			break;

		/*
		 * i_size must be checked after we know the pages are Uptodate.
		 *
		 * Checking i_size after the check allows us to calculate
		 * the correct value for "nr", which means the zero-filled
		 * part of the page is not copied back to userspace (unless
		 * another truncate extends the file - this is desired though).
		 */
		isize = i_size_read(inode);
		if (unlikely(iocb->ki_pos >= isize))
			goto put_folios;
		end_offset = min_t(loff_t, isize, iocb->ki_pos + iter->count);

		/*
		 * Once we start copying data, we don't want to be touching any
		 * cachelines that might be contended:
		 */
		writably_mapped = mapping_writably_mapped(mapping);

		/*
		 * When a sequential read accesses a page several times, only
		 * mark it as accessed the first time.
		 */
		if (iocb->ki_pos >> PAGE_SHIFT !=
		    ra->prev_pos >> PAGE_SHIFT)
			folio_mark_accessed(fbatch.folios[0]);

		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct folio *folio = fbatch.folios[i];
			size_t fsize = folio_size(folio);
			size_t offset = iocb->ki_pos & (fsize - 1);
			size_t bytes = min_t(loff_t, end_offset - iocb->ki_pos,
					     fsize - offset);
			size_t copied;

			if (end_offset < folio_pos(folio))
				break;
			if (i > 0)
				folio_mark_accessed(folio);
			/*
			 * If users can be writing to this folio using arbitrary
			 * virtual addresses, take care of potential aliasing
			 * before reading the folio on the kernel side.
			 */
			if (writably_mapped)
				flush_dcache_folio(folio);

			copied = copy_folio_to_iter(folio, offset, bytes, iter);

			already_read += copied;
			iocb->ki_pos += copied;
			ra->prev_pos = iocb->ki_pos;

			if (copied < bytes) {
				error = -EFAULT;
				break;
			}
		}
put_folios:
		for (i = 0; i < folio_batch_count(&fbatch); i++)
			folio_put(fbatch.folios[i]);
		folio_batch_init(&fbatch);
	} while (iov_iter_count(iter) && iocb->ki_pos < isize && !error);

	file_accessed(filp);

	return already_read ? already_read : error;
}
EXPORT_SYMBOL_GPL(filemap_read);

/**
 * generic_file_read_iter - generic filesystem read routine
 * @iocb:	kernel I/O control block
 * @iter:	destination for the data read
 *
 * This is the "read_iter()" routine for all filesystems
 * that can use the page cache directly.
 *
 * The IOCB_NOWAIT flag in iocb->ki_flags indicates that -EAGAIN shall
 * be returned when no data can be read without waiting for I/O requests
 * to complete; it doesn't prevent readahead.
 *
 * The IOCB_NOIO flag in iocb->ki_flags indicates that no new I/O
 * requests shall be made for the read or for readahead.  When no data
 * can be read, -EAGAIN shall be returned.  When readahead would be
 * triggered, a partial, possibly empty read shall be returned.
 *
 * Return:
 * * number of bytes copied, even for partial reads
 * * negative error code (or 0 if IOCB_NOIO) if nothing was read
 */
ssize_t
generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	size_t count = iov_iter_count(iter);
	ssize_t retval = 0;

	if (!count)
		return 0; /* skip atime */

	if (iocb->ki_flags & IOCB_DIRECT) {
		struct file *file = iocb->ki_filp;
		struct address_space *mapping = file->f_mapping;
		struct inode *inode = mapping->host;

		if (iocb->ki_flags & IOCB_NOWAIT) {
			if (filemap_range_needs_writeback(mapping, iocb->ki_pos,
						iocb->ki_pos + count - 1))
				return -EAGAIN;
		} else {
			retval = filemap_write_and_wait_range(mapping,
						iocb->ki_pos,
					        iocb->ki_pos + count - 1);
			if (retval < 0)
				return retval;
		}

		file_accessed(file);

		retval = mapping->a_ops->direct_IO(iocb, iter);
		if (retval >= 0) {
			iocb->ki_pos += retval;
			count -= retval;
		}
		if (retval != -EIOCBQUEUED)
			iov_iter_revert(iter, count - iov_iter_count(iter));

		/*
		 * Btrfs can have a short DIO read if we encounter
		 * compressed extents, so if there was an error, or if
		 * we've already read everything we wanted to, or if
		 * there was a short read because we hit EOF, go ahead
		 * and return.  Otherwise fallthrough to buffered io for
		 * the rest of the read.  Buffered reads will not work for
		 * DAX files, so don't bother trying.
		 */
		if (retval < 0 || !count || IS_DAX(inode))
			return retval;
		if (iocb->ki_pos >= i_size_read(inode))
			return retval;
	}

	return filemap_read(iocb, iter, retval);
}
EXPORT_SYMBOL(generic_file_read_iter);

static inline loff_t folio_seek_hole_data(struct xa_state *xas,
		struct address_space *mapping, struct folio *folio,
		loff_t start, loff_t end, bool seek_data)
{
	const struct address_space_operations *ops = mapping->a_ops;
	size_t offset, bsz = i_blocksize(mapping->host);

	if (xa_is_value(folio) || folio_test_uptodate(folio))
		return seek_data ? start : end;
	if (!ops->is_partially_uptodate)
		return seek_data ? end : start;

	xas_pause(xas);
	rcu_read_unlock();
	folio_lock(folio);
	if (unlikely(folio->mapping != mapping))
		goto unlock;

	offset = offset_in_folio(folio, start) & ~(bsz - 1);

	do {
		if (ops->is_partially_uptodate(folio, offset, bsz) ==
							seek_data)
			break;
		start = (start + bsz) & ~(bsz - 1);
		offset += bsz;
	} while (offset < folio_size(folio));
unlock:
	folio_unlock(folio);
	rcu_read_lock();
	return start;
}

static inline size_t seek_folio_size(struct xa_state *xas, struct folio *folio)
{
	if (xa_is_value(folio))
		return PAGE_SIZE << xa_get_order(xas->xa, xas->xa_index);
	return folio_size(folio);
}
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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
#endif
/**
 * mapping_seek_hole_data - Seek for SEEK_DATA / SEEK_HOLE in the page cache.
 * @mapping: Address space to search.
 * @start: First byte to consider.
 * @end: Limit of search (exclusive).
 * @whence: Either SEEK_HOLE or SEEK_DATA.
 *
 * If the page cache knows which blocks contain holes and which blocks
 * contain data, your filesystem can use this function to implement
 * SEEK_HOLE and SEEK_DATA.  This is useful for filesystems which are
 * entirely memory-based such as tmpfs, and filesystems which support
 * unwritten extents.
 *
 * Return: The requested offset on success, or -ENXIO if @whence specifies
 * SEEK_DATA and there is no data after @start.  There is an implicit hole
 * after @end - 1, so SEEK_HOLE returns @end if all the bytes between @start
 * and @end contain data.
 */
loff_t mapping_seek_hole_data(struct address_space *mapping, loff_t start,
		loff_t end, int whence)
{
	XA_STATE(xas, &mapping->i_pages, start >> PAGE_SHIFT);
	pgoff_t max = (end - 1) >> PAGE_SHIFT;
	bool seek_data = (whence == SEEK_DATA);
	struct folio *folio;
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
	/*page的从xarray tree delete和 保存到xarray tree 两个过程因为加锁防护，不会并发执行，因此不用担心下边的
	 *找到的folio是file_area*/
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
			return mapping_seek_hole_data_for_file_area(mapping,start,end,whence);
	}
#endif	

	if (end <= start)
		return -ENXIO;

	rcu_read_lock();
	while ((folio = find_get_entry(&xas, max, XA_PRESENT))) {
		loff_t pos = (u64)xas.xa_index << PAGE_SHIFT;
		size_t seek_size;

		if (start < pos) {
			if (!seek_data)
				goto unlock;
			start = pos;
		}

		seek_size = seek_folio_size(&xas, folio);
		pos = round_up((u64)pos + 1, seek_size);
		start = folio_seek_hole_data(&xas, mapping, folio, start, pos,
				seek_data);
		if (start < pos)
			goto unlock;
		if (start >= end)
			break;
		if (seek_size > PAGE_SIZE)
			xas_set(&xas, pos >> PAGE_SHIFT);
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

#ifdef CONFIG_MMU
#define MMAP_LOTSAMISS  (100)
/*
 * lock_folio_maybe_drop_mmap - lock the page, possibly dropping the mmap_lock
 * @vmf - the vm_fault for this fault.
 * @folio - the folio to lock.
 * @fpin - the pointer to the file we may pin (or is already pinned).
 *
 * This works similar to lock_folio_or_retry in that it can drop the
 * mmap_lock.  It differs in that it actually returns the folio locked
 * if it returns 1 and 0 if it couldn't lock the folio.  If we did have
 * to drop the mmap_lock then fpin will point to the pinned file and
 * needs to be fput()'ed at a later point.
 */
static int lock_folio_maybe_drop_mmap(struct vm_fault *vmf, struct folio *folio,
				     struct file **fpin)
{
	if (folio_trylock(folio))
		return 1;

	/*
	 * NOTE! This will make us return with VM_FAULT_RETRY, but with
	 * the mmap_lock still held. That's how FAULT_FLAG_RETRY_NOWAIT
	 * is supposed to work. We have way too many special cases..
	 */
	if (vmf->flags & FAULT_FLAG_RETRY_NOWAIT)
		return 0;

	*fpin = maybe_unlock_mmap_for_io(vmf, *fpin);
	if (vmf->flags & FAULT_FLAG_KILLABLE) {
		if (__folio_lock_killable(folio)) {
			/*
			 * We didn't have the right flags to drop the mmap_lock,
			 * but all fault_handlers only check for fatal signals
			 * if we return VM_FAULT_RETRY, so we need to drop the
			 * mmap_lock here and return 0 if we don't have a fpin.
			 */
			if (*fpin == NULL)
				mmap_read_unlock(vmf->vma->vm_mm);
			return 0;
		}
	} else
		__folio_lock(folio);

	return 1;
}

/*
 * Synchronous readahead happens when we don't even find a page in the page
 * cache at all.  We don't want to perform IO under the mmap sem, so if we have
 * to drop the mmap sem we return the file that was pinned in order for us to do
 * that.  If we didn't pin a file then we return NULL.  The file that is
 * returned needs to be fput()'ed when we're done with it.
 */
static struct file *do_sync_mmap_readahead(struct vm_fault *vmf)
{
	struct file *file = vmf->vma->vm_file;
	struct file_ra_state *ra = &file->f_ra;
	struct address_space *mapping = file->f_mapping;
	DEFINE_READAHEAD(ractl, file, ra, mapping, vmf->pgoff);
	struct file *fpin = NULL;
	unsigned long vm_flags = vmf->vma->vm_flags;
	unsigned int mmap_miss;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	/* Use the readahead code, even if readahead is disabled */
	if (vm_flags & VM_HUGEPAGE) {
		fpin = maybe_unlock_mmap_for_io(vmf, fpin);
		ractl._index &= ~((unsigned long)HPAGE_PMD_NR - 1);
		ra->size = HPAGE_PMD_NR;
		/*
		 * Fetch two PMD folios, so we get the chance to actually
		 * readahead, unless we've been told not to.
		 */
		if (!(vm_flags & VM_RAND_READ))
			ra->size *= 2;
		ra->async_size = HPAGE_PMD_NR;
		page_cache_ra_order(&ractl, ra, HPAGE_PMD_ORDER);
		return fpin;
	}
#endif

	/* If we don't want any read-ahead, don't bother */
	if (vm_flags & VM_RAND_READ)
		return fpin;
	if (!ra->ra_pages)
		return fpin;

	if (vm_flags & VM_SEQ_READ) {
		fpin = maybe_unlock_mmap_for_io(vmf, fpin);
		page_cache_sync_ra(&ractl, ra->ra_pages);
		return fpin;
	}

	/* Avoid banging the cache line if not needed */
	mmap_miss = READ_ONCE(ra->mmap_miss);
	if (mmap_miss < MMAP_LOTSAMISS * 10)
		WRITE_ONCE(ra->mmap_miss, ++mmap_miss);

	/*
	 * Do we miss much more than hit in this file? If so,
	 * stop bothering with read-ahead. It will only hurt.
	 */
	if (mmap_miss > MMAP_LOTSAMISS)
		return fpin;

	/*
	 * mmap read-around
	 */
	fpin = maybe_unlock_mmap_for_io(vmf, fpin);
	ra->start = max_t(long, 0, vmf->pgoff - ra->ra_pages / 2);
	ra->size = ra->ra_pages;
	ra->async_size = ra->ra_pages / 4;
	ractl._index = ra->start;
	page_cache_ra_order(&ractl, ra, 0);
	return fpin;
}

/*
 * Asynchronous readahead happens when we find the page and PG_readahead,
 * so we want to possibly extend the readahead further.  We return the file that
 * was pinned if we have to drop the mmap_lock in order to do IO.
 */
static struct file *do_async_mmap_readahead(struct vm_fault *vmf,
					    struct folio *folio)
{
	struct file *file = vmf->vma->vm_file;
	struct file_ra_state *ra = &file->f_ra;
	DEFINE_READAHEAD(ractl, file, ra, file->f_mapping, vmf->pgoff);
	struct file *fpin = NULL;
	unsigned int mmap_miss;

	/* If we don't want any read-ahead, don't bother */
	if (vmf->vma->vm_flags & VM_RAND_READ || !ra->ra_pages)
		return fpin;

	mmap_miss = READ_ONCE(ra->mmap_miss);
	if (mmap_miss)
		WRITE_ONCE(ra->mmap_miss, --mmap_miss);

	if (folio_test_readahead(folio)) {
		fpin = maybe_unlock_mmap_for_io(vmf, fpin);
		page_cache_async_ra(&ractl, folio, ra->ra_pages);
	}
	return fpin;
}

/**
 * filemap_fault - read in file data for page fault handling
 * @vmf:	struct vm_fault containing details of the fault
 *
 * filemap_fault() is invoked via the vma operations vector for a
 * mapped memory region to read in file data during a page fault.
 *
 * The goto's are kind of ugly, but this streamlines the normal case of having
 * it in the page cache, and handles the special cases reasonably without
 * having a lot of duplicated code.
 *
 * vma->vm_mm->mmap_lock must be held on entry.
 *
 * If our return value has VM_FAULT_RETRY set, it's because the mmap_lock
 * may be dropped before doing I/O or by lock_folio_maybe_drop_mmap().
 *
 * If our return value does not have VM_FAULT_RETRY set, the mmap_lock
 * has not been released.
 *
 * We never return with VM_FAULT_RETRY and a bit from VM_FAULT_ERROR set.
 *
 * Return: bitwise-OR of %VM_FAULT_ codes.
 */
vm_fault_t filemap_fault(struct vm_fault *vmf)
{
	int error;
	struct file *file = vmf->vma->vm_file;
	struct file *fpin = NULL;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	pgoff_t max_idx, index = vmf->pgoff;
	struct folio *folio;
	vm_fault_t ret = 0;
	bool mapping_locked = false;

	max_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	if (unlikely(index >= max_idx))
		return VM_FAULT_SIGBUS;

	/*
	 * Do we have something in the page cache already?
	 */
	folio = filemap_get_folio(mapping, index);
	if (likely(folio)) {
		/*
		 * We found the page, so try async readahead before waiting for
		 * the lock.
		 */
		if (!(vmf->flags & FAULT_FLAG_TRIED))
			fpin = do_async_mmap_readahead(vmf, folio);
		if (unlikely(!folio_test_uptodate(folio))) {
			filemap_invalidate_lock_shared(mapping);
			mapping_locked = true;
		}
	} else {
		/* No page in the page cache at all */
		count_vm_event(PGMAJFAULT);
		count_memcg_event_mm(vmf->vma->vm_mm, PGMAJFAULT);
		ret = VM_FAULT_MAJOR;
		fpin = do_sync_mmap_readahead(vmf);
retry_find:
		/*
		 * See comment in filemap_create_folio() why we need
		 * invalidate_lock
		 */
		if (!mapping_locked) {
			filemap_invalidate_lock_shared(mapping);
			mapping_locked = true;
		}
		folio = __filemap_get_folio(mapping, index,
					  FGP_CREAT|FGP_FOR_MMAP,
					  vmf->gfp_mask);
		if (!folio) {
			if (fpin)
				goto out_retry;
			filemap_invalidate_unlock_shared(mapping);
			return VM_FAULT_OOM;
		}
	}

	if (!lock_folio_maybe_drop_mmap(vmf, folio, &fpin))
		goto out_retry;

	/* Did it get truncated? */
	if (unlikely(folio->mapping != mapping)) {
		folio_unlock(folio);
		folio_put(folio);
		goto retry_find;
	}
	VM_BUG_ON_FOLIO(!folio_contains(folio, index), folio);

	/*
	 * We have a locked page in the page cache, now we need to check
	 * that it's up-to-date. If not, it is going to be due to an error.
	 */
	if (unlikely(!folio_test_uptodate(folio))) {
		/*
		 * The page was in cache and uptodate and now it is not.
		 * Strange but possible since we didn't hold the page lock all
		 * the time. Let's drop everything get the invalidate lock and
		 * try again.
		 */
		if (!mapping_locked) {
			folio_unlock(folio);
			folio_put(folio);
			goto retry_find;
		}
		goto page_not_uptodate;
	}

	/*
	 * We've made it this far and we had to drop our mmap_lock, now is the
	 * time to return to the upper layer and have it re-find the vma and
	 * redo the fault.
	 */
	if (fpin) {
		folio_unlock(folio);
		goto out_retry;
	}
	if (mapping_locked)
		filemap_invalidate_unlock_shared(mapping);

	/*
	 * Found the page and have a reference on it.
	 * We must recheck i_size under page lock.
	 */
	max_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	if (unlikely(index >= max_idx)) {
		folio_unlock(folio);
		folio_put(folio);
		return VM_FAULT_SIGBUS;
	}

	vmf->page = folio_file_page(folio, index);
	return ret | VM_FAULT_LOCKED;

page_not_uptodate:
	/*
	 * Umm, take care of errors if the page isn't up-to-date.
	 * Try to re-read it _once_. We do this synchronously,
	 * because there really aren't any performance issues here
	 * and we need to check for errors.
	 */
	fpin = maybe_unlock_mmap_for_io(vmf, fpin);
	error = filemap_read_folio(file, mapping, folio);
	if (fpin)
		goto out_retry;
	folio_put(folio);

	if (!error || error == AOP_TRUNCATED_PAGE)
		goto retry_find;
	filemap_invalidate_unlock_shared(mapping);

	return VM_FAULT_SIGBUS;

out_retry:
	/*
	 * We dropped the mmap_lock, we need to return to the fault handler to
	 * re-find the vma and come back and find our hopefully still populated
	 * page.
	 */
	if (folio)
		folio_put(folio);
	if (mapping_locked)
		filemap_invalidate_unlock_shared(mapping);
	if (fpin)
		fput(fpin);
	return ret | VM_FAULT_RETRY;
}
EXPORT_SYMBOL(filemap_fault);

static bool filemap_map_pmd(struct vm_fault *vmf, struct page *page)
{
	struct mm_struct *mm = vmf->vma->vm_mm;

	/* Huge page is mapped? No need to proceed. */
	if (pmd_trans_huge(*vmf->pmd)) {
		unlock_page(page);
		put_page(page);
		return true;
	}

	if (pmd_none(*vmf->pmd) && PageTransHuge(page)) {
		vm_fault_t ret = do_set_pmd(vmf, page);
		if (!ret) {
			/* The page is mapped successfully, reference consumed. */
			unlock_page(page);
			return true;
		}
	}

	if (pmd_none(*vmf->pmd))
		pmd_install(mm, vmf->pmd, &vmf->prealloc_pte);

	/* See comment in handle_pte_fault() */
	if (pmd_devmap_trans_unstable(vmf->pmd)) {
		unlock_page(page);
		put_page(page);
		return true;
	}

	return false;
}

static struct folio *next_uptodate_page(struct folio *folio,
				       struct address_space *mapping,
				       struct xa_state *xas, pgoff_t end_pgoff)
{
	unsigned long max_idx;

	do {
		if (!folio)
			return NULL;
		if (xas_retry(xas, folio))
			continue;
		if (xa_is_value(folio))
			continue;
		if (folio_test_locked(folio))
			continue;
		if (!folio_try_get_rcu(folio))
			continue;
		/* Has the page moved or been split? */
		if (unlikely(folio != xas_reload(xas)))
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
		if (xas->xa_index >= max_idx)
			goto unlock;
		return folio;
unlock:
		folio_unlock(folio);
skip:
		folio_put(folio);
	} while ((folio = xas_next_entry(xas, end_pgoff)) != NULL);

	return NULL;
}

static inline struct folio *first_map_page(struct address_space *mapping,
					  struct xa_state *xas,
					  pgoff_t end_pgoff)
{
	return next_uptodate_page(xas_find(xas, end_pgoff),
				  mapping, xas, end_pgoff);
}

static inline struct folio *next_map_page(struct address_space *mapping,
					 struct xa_state *xas,
					 pgoff_t end_pgoff)
{
	return next_uptodate_page(xas_next_entry(xas, end_pgoff),
				  mapping, xas, end_pgoff);
}

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
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

		folio = p_file_area->pages[page_offset_in_file_area_temp];
		FILE_AREA_PRINT("3:%s %s %d p_file_area:0x%llx file_area_state:0x%x folio:0x%llx xas.xa_index:%ld page_offset_in_file_area_temp:%d folio->index:%ld\n",__func__,current->comm,current->pid,(u64)p_file_area,p_file_area->file_area_state,(u64)folio,xas->xa_index,page_offset_in_file_area_temp,folio != NULL ?folio->index:-1);

		if(!folio)
			goto next_folio;

		/*检测查找到的page是否正确，不是则crash*/
		CHECK_FOLIO_FROM_FILE_AREA_VALID(xas,folio,p_file_area,page_offset_in_file_area_temp,folio_index_from_xa_index);

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
#endif
vm_fault_t filemap_map_pages(struct vm_fault *vmf,
			     pgoff_t start_pgoff, pgoff_t end_pgoff)
{
	struct vm_area_struct *vma = vmf->vma;
	struct file *file = vma->vm_file;
	struct address_space *mapping = file->f_mapping;
	pgoff_t last_pgoff = start_pgoff;
	unsigned long addr;
	XA_STATE(xas, &mapping->i_pages, start_pgoff);
	struct folio *folio;
	struct page *page;
	unsigned int mmap_miss = READ_ONCE(file->f_ra.mmap_miss);
	vm_fault_t ret = 0;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
	/*page的从xarray tree delete和 保存到xarray tree 两个过程因为加锁防护，不会并发执行，因此不用担心下边的
	 *找到的folio是file_area*/
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		//smp_rmb();
		//if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping))
			return filemap_map_pages_for_file_area(vmf,start_pgoff,end_pgoff);
	}
#endif	

	rcu_read_lock();
	folio = first_map_page(mapping, &xas, end_pgoff);
	if (!folio)
		goto out;

	if (filemap_map_pmd(vmf, &folio->page)) {
		ret = VM_FAULT_NOPAGE;
		goto out;
	}

	addr = vma->vm_start + ((start_pgoff - vma->vm_pgoff) << PAGE_SHIFT);
	vmf->pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd, addr, &vmf->ptl);
	do {
again:
		page = folio_file_page(folio, xas.xa_index);
		if (PageHWPoison(page))
			goto unlock;

		if (mmap_miss > 0)
			mmap_miss--;

		addr += (xas.xa_index - last_pgoff) << PAGE_SHIFT;
		vmf->pte += xas.xa_index - last_pgoff;
		last_pgoff = xas.xa_index;

		if (!pte_none(*vmf->pte))
			goto unlock;

		/* We're about to handle the fault */
		if (vmf->address == addr)
			ret = VM_FAULT_NOPAGE;

		do_set_pte(vmf, page, addr);
		/* no need to invalidate: a not-present page won't be cached */
		update_mmu_cache(vma, addr, vmf->pte);
		if (folio_more_pages(folio, xas.xa_index, end_pgoff)) {
			xas.xa_index++;
			folio_ref_inc(folio);
			goto again;
		}
		folio_unlock(folio);
		continue;
unlock:
		if (folio_more_pages(folio, xas.xa_index, end_pgoff)) {
			xas.xa_index++;
			goto again;
		}
		folio_unlock(folio);
		folio_put(folio);
	} while ((folio = next_map_page(mapping, &xas, end_pgoff)) != NULL);
	pte_unmap_unlock(vmf->pte, vmf->ptl);
out:
	rcu_read_unlock();
	WRITE_ONCE(file->f_ra.mmap_miss, mmap_miss);
	return ret;
}
EXPORT_SYMBOL(filemap_map_pages);

vm_fault_t filemap_page_mkwrite(struct vm_fault *vmf)
{
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	struct folio *folio = page_folio(vmf->page);
	vm_fault_t ret = VM_FAULT_LOCKED;

	sb_start_pagefault(mapping->host->i_sb);
	file_update_time(vmf->vma->vm_file);
	folio_lock(folio);
	if (folio->mapping != mapping) {
		folio_unlock(folio);
		ret = VM_FAULT_NOPAGE;
		goto out;
	}
	/*
	 * We mark the folio dirty already here so that when freeze is in
	 * progress, we are guaranteed that writeback during freezing will
	 * see the dirty folio and writeprotect it again.
	 */
	folio_mark_dirty(folio);
	folio_wait_stable(folio);
out:
	sb_end_pagefault(mapping->host->i_sb);
	return ret;
}

const struct vm_operations_struct generic_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= filemap_page_mkwrite,
};

/* This is used for a general mmap of a disk file */

int generic_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct address_space *mapping = file->f_mapping;

	if (!mapping->a_ops->readpage)
		return -ENOEXEC;
	file_accessed(file);
	vma->vm_ops = &generic_file_vm_ops;
	return 0;
}

/*
 * This is for filesystems which do not implement ->writepage.
 */
int generic_file_readonly_mmap(struct file *file, struct vm_area_struct *vma)
{
	if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_MAYWRITE))
		return -EINVAL;
	return generic_file_mmap(file, vma);
}
#else
vm_fault_t filemap_page_mkwrite(struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}
int generic_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	return -ENOSYS;
}
int generic_file_readonly_mmap(struct file *file, struct vm_area_struct *vma)
{
	return -ENOSYS;
}
#endif /* CONFIG_MMU */

EXPORT_SYMBOL(filemap_page_mkwrite);
EXPORT_SYMBOL(generic_file_mmap);
EXPORT_SYMBOL(generic_file_readonly_mmap);

static struct folio *do_read_cache_folio(struct address_space *mapping,
		pgoff_t index, filler_t filler, void *data, gfp_t gfp)
{
	struct folio *folio;
	int err;
repeat:
	folio = filemap_get_folio(mapping, index);
	if (!folio) {
		folio = filemap_alloc_folio(gfp, 0);
		if (!folio)
			return ERR_PTR(-ENOMEM);
		err = filemap_add_folio(mapping, folio, index, gfp);
		if (unlikely(err)) {
			folio_put(folio);
			if (err == -EEXIST)
				goto repeat;
			/* Presumably ENOMEM for xarray node */
			return ERR_PTR(err);
		}

filler:
		if (filler)
			err = filler(data, &folio->page);
		else
			err = mapping->a_ops->readpage(data, &folio->page);

		if (err < 0) {
			folio_put(folio);
			return ERR_PTR(err);
		}

		folio_wait_locked(folio);
		if (!folio_test_uptodate(folio)) {
			folio_put(folio);
			return ERR_PTR(-EIO);
		}

		goto out;
	}
	if (folio_test_uptodate(folio))
		goto out;

	if (!folio_trylock(folio)) {
		folio_put_wait_locked(folio, TASK_UNINTERRUPTIBLE);
		goto repeat;
	}

	/* Folio was truncated from mapping */
	if (!folio->mapping) {
		folio_unlock(folio);
		folio_put(folio);
		goto repeat;
	}

	/* Someone else locked and filled the page in a very small window */
	if (folio_test_uptodate(folio)) {
		folio_unlock(folio);
		goto out;
	}

	/*
	 * A previous I/O error may have been due to temporary
	 * failures.
	 * Clear page error before actual read, PG_error will be
	 * set again if read page fails.
	 */
	folio_clear_error(folio);
	goto filler;

out:
	folio_mark_accessed(folio);
	return folio;
}

/**
 * read_cache_folio - read into page cache, fill it if needed
 * @mapping:	the page's address_space
 * @index:	the page index
 * @filler:	function to perform the read
 * @data:	first arg to filler(data, page) function, often left as NULL
 *
 * Read into the page cache. If a page already exists, and PageUptodate() is
 * not set, try to fill the page and wait for it to become unlocked.
 *
 * If the page does not get brought uptodate, return -EIO.
 *
 * The function expects mapping->invalidate_lock to be already held.
 *
 * Return: up to date page on success, ERR_PTR() on failure.
 */
struct folio *read_cache_folio(struct address_space *mapping, pgoff_t index,
		filler_t filler, void *data)
{
	return do_read_cache_folio(mapping, index, filler, data,
			mapping_gfp_mask(mapping));
}
EXPORT_SYMBOL(read_cache_folio);

static struct page *do_read_cache_page(struct address_space *mapping,
		pgoff_t index, filler_t *filler, void *data, gfp_t gfp)
{
	struct folio *folio;

	folio = do_read_cache_folio(mapping, index, filler, data, gfp);
	if (IS_ERR(folio))
		return &folio->page;
	return folio_file_page(folio, index);
}

struct page *read_cache_page(struct address_space *mapping,
				pgoff_t index, filler_t *filler, void *data)
{
	return do_read_cache_page(mapping, index, filler, data,
			mapping_gfp_mask(mapping));
}
EXPORT_SYMBOL(read_cache_page);

/**
 * read_cache_page_gfp - read into page cache, using specified page allocation flags.
 * @mapping:	the page's address_space
 * @index:	the page index
 * @gfp:	the page allocator flags to use if allocating
 *
 * This is the same as "read_mapping_page(mapping, index, NULL)", but with
 * any new page allocations done using the specified allocation flags.
 *
 * If the page does not get brought uptodate, return -EIO.
 *
 * The function expects mapping->invalidate_lock to be already held.
 *
 * Return: up to date page on success, ERR_PTR() on failure.
 */
struct page *read_cache_page_gfp(struct address_space *mapping,
				pgoff_t index,
				gfp_t gfp)
{
	return do_read_cache_page(mapping, index, NULL, NULL, gfp);
}
EXPORT_SYMBOL(read_cache_page_gfp);

int pagecache_write_begin(struct file *file, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned flags,
				struct page **pagep, void **fsdata)
{
	const struct address_space_operations *aops = mapping->a_ops;

	return aops->write_begin(file, mapping, pos, len, flags,
							pagep, fsdata);
}
EXPORT_SYMBOL(pagecache_write_begin);

int pagecache_write_end(struct file *file, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned copied,
				struct page *page, void *fsdata)
{
	const struct address_space_operations *aops = mapping->a_ops;

	return aops->write_end(file, mapping, pos, len, copied, page, fsdata);
}
EXPORT_SYMBOL(pagecache_write_end);

/*
 * Warn about a page cache invalidation failure during a direct I/O write.
 */
void dio_warn_stale_pagecache(struct file *filp)
{
	static DEFINE_RATELIMIT_STATE(_rs, 86400 * HZ, DEFAULT_RATELIMIT_BURST);
	char pathname[128];
	char *path;

	errseq_set(&filp->f_mapping->wb_err, -EIO);
	if (__ratelimit(&_rs)) {
		path = file_path(filp, pathname, sizeof(pathname));
		if (IS_ERR(path))
			path = "(unknown)";
		pr_crit("Page cache invalidation failure on direct I/O.  Possible data corruption due to collision with buffered I/O!\n");
		pr_crit("File: %s PID: %d Comm: %.20s\n", path, current->pid,
			current->comm);
	}
}

ssize_t
generic_file_direct_write(struct kiocb *iocb, struct iov_iter *from)
{
	struct file	*file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	struct inode	*inode = mapping->host;
	loff_t		pos = iocb->ki_pos;
	ssize_t		written;
	size_t		write_len;
	pgoff_t		end;

	write_len = iov_iter_count(from);
	end = (pos + write_len - 1) >> PAGE_SHIFT;

	if (iocb->ki_flags & IOCB_NOWAIT) {
		/* If there are pages to writeback, return */
		if (filemap_range_has_page(file->f_mapping, pos,
					   pos + write_len - 1))
			return -EAGAIN;
	} else {
		written = filemap_write_and_wait_range(mapping, pos,
							pos + write_len - 1);
		if (written)
			goto out;
	}

	/*
	 * After a write we want buffered reads to be sure to go to disk to get
	 * the new data.  We invalidate clean cached page from the region we're
	 * about to write.  We do this *before* the write so that we can return
	 * without clobbering -EIOCBQUEUED from ->direct_IO().
	 */
	written = invalidate_inode_pages2_range(mapping,
					pos >> PAGE_SHIFT, end);
	/*
	 * If a page can not be invalidated, return 0 to fall back
	 * to buffered write.
	 */
	if (written) {
		if (written == -EBUSY)
			return 0;
		goto out;
	}

	written = mapping->a_ops->direct_IO(iocb, from);

	/*
	 * Finally, try again to invalidate clean pages which might have been
	 * cached by non-direct readahead, or faulted in by get_user_pages()
	 * if the source of the write was an mmap'ed region of the file
	 * we're writing.  Either one is a pretty crazy thing to do,
	 * so we don't support it 100%.  If this invalidation
	 * fails, tough, the write still worked...
	 *
	 * Most of the time we do not need this since dio_complete() will do
	 * the invalidation for us. However there are some file systems that
	 * do not end up with dio_complete() being called, so let's not break
	 * them by removing it completely.
	 *
	 * Noticeable example is a blkdev_direct_IO().
	 *
	 * Skip invalidation for async writes or if mapping has no pages.
	 */
	if (written > 0 && mapping->nrpages &&
	    invalidate_inode_pages2_range(mapping, pos >> PAGE_SHIFT, end))
		dio_warn_stale_pagecache(file);

	if (written > 0) {
		pos += written;
		write_len -= written;
		if (pos > i_size_read(inode) && !S_ISBLK(inode->i_mode)) {
			i_size_write(inode, pos);
			mark_inode_dirty(inode);
		}
		iocb->ki_pos = pos;
	}
	if (written != -EIOCBQUEUED)
		iov_iter_revert(from, write_len - iov_iter_count(from));
out:
	return written;
}
EXPORT_SYMBOL(generic_file_direct_write);

ssize_t generic_perform_write(struct kiocb *iocb, struct iov_iter *i)
{
	struct file *file = iocb->ki_filp;
	loff_t pos = iocb->ki_pos;
	struct address_space *mapping = file->f_mapping;
	const struct address_space_operations *a_ops = mapping->a_ops;
	long status = 0;
	ssize_t written = 0;
	unsigned int flags = 0;

	do {
		struct page *page;
		unsigned long offset;	/* Offset into pagecache page */
		unsigned long bytes;	/* Bytes to write to page */
		size_t copied;		/* Bytes copied from user */
		void *fsdata;

		offset = (pos & (PAGE_SIZE - 1));
		bytes = min_t(unsigned long, PAGE_SIZE - offset,
						iov_iter_count(i));

again:
		/*
		 * Bring in the user page that we will copy from _first_.
		 * Otherwise there's a nasty deadlock on copying from the
		 * same page as we're writing to, without it being marked
		 * up-to-date.
		 */
		if (unlikely(fault_in_iov_iter_readable(i, bytes) == bytes)) {
			status = -EFAULT;
			break;
		}

		if (fatal_signal_pending(current)) {
			status = -EINTR;
			break;
		}

		status = a_ops->write_begin(file, mapping, pos, bytes, flags,
						&page, &fsdata);
		if (unlikely(status < 0))
			break;

		if (mapping_writably_mapped(mapping))
			flush_dcache_page(page);

		copied = copy_page_from_iter_atomic(page, offset, bytes, i);
		flush_dcache_page(page);

		status = a_ops->write_end(file, mapping, pos, bytes, copied,
						page, fsdata);
		if (unlikely(status != copied)) {
			iov_iter_revert(i, copied - max(status, 0L));
			if (unlikely(status < 0))
				break;
		}
		cond_resched();

		if (unlikely(status == 0)) {
			/*
			 * A short copy made ->write_end() reject the
			 * thing entirely.  Might be memory poisoning
			 * halfway through, might be a race with munmap,
			 * might be severe memory pressure.
			 */
			if (copied)
				bytes = copied;
			goto again;
		}
		pos += status;
		written += status;

		balance_dirty_pages_ratelimited(mapping);
	} while (iov_iter_count(i));

	return written ? written : status;
}
EXPORT_SYMBOL(generic_perform_write);

/**
 * __generic_file_write_iter - write data to a file
 * @iocb:	IO state structure (file, offset, etc.)
 * @from:	iov_iter with data to write
 *
 * This function does all the work needed for actually writing data to a
 * file. It does all basic checks, removes SUID from the file, updates
 * modification times and calls proper subroutines depending on whether we
 * do direct IO or a standard buffered write.
 *
 * It expects i_rwsem to be grabbed unless we work on a block device or similar
 * object which does not need locking at all.
 *
 * This function does *not* take care of syncing data in case of O_SYNC write.
 * A caller has to handle it. This is mainly due to the fact that we want to
 * avoid syncing under i_rwsem.
 *
 * Return:
 * * number of bytes written, even for truncated writes
 * * negative error code if no data has been written at all
 */
ssize_t __generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	struct inode 	*inode = mapping->host;
	ssize_t		written = 0;
	ssize_t		err;
	ssize_t		status;

	/* We can write back this queue in page reclaim */
	current->backing_dev_info = inode_to_bdi(inode);
	err = file_remove_privs(file);
	if (err)
		goto out;

	err = file_update_time(file);
	if (err)
		goto out;

	if (iocb->ki_flags & IOCB_DIRECT) {
		loff_t pos, endbyte;

		written = generic_file_direct_write(iocb, from);
		/*
		 * If the write stopped short of completing, fall back to
		 * buffered writes.  Some filesystems do this for writes to
		 * holes, for example.  For DAX files, a buffered write will
		 * not succeed (even if it did, DAX does not handle dirty
		 * page-cache pages correctly).
		 */
		if (written < 0 || !iov_iter_count(from) || IS_DAX(inode))
			goto out;

		pos = iocb->ki_pos;
		status = generic_perform_write(iocb, from);
		/*
		 * If generic_perform_write() returned a synchronous error
		 * then we want to return the number of bytes which were
		 * direct-written, or the error code if that was zero.  Note
		 * that this differs from normal direct-io semantics, which
		 * will return -EFOO even if some bytes were written.
		 */
		if (unlikely(status < 0)) {
			err = status;
			goto out;
		}
		/*
		 * We need to ensure that the page cache pages are written to
		 * disk and invalidated to preserve the expected O_DIRECT
		 * semantics.
		 */
		endbyte = pos + status - 1;
		err = filemap_write_and_wait_range(mapping, pos, endbyte);
		if (err == 0) {
			iocb->ki_pos = endbyte + 1;
			written += status;
			invalidate_mapping_pages(mapping,
						 pos >> PAGE_SHIFT,
						 endbyte >> PAGE_SHIFT);
		} else {
			/*
			 * We don't know how much we wrote, so just return
			 * the number of bytes which were direct-written
			 */
		}
	} else {
		written = generic_perform_write(iocb, from);
		if (likely(written > 0))
			iocb->ki_pos += written;
	}
out:
	current->backing_dev_info = NULL;
	return written ? written : err;
}
EXPORT_SYMBOL(__generic_file_write_iter);

/**
 * generic_file_write_iter - write data to a file
 * @iocb:	IO state structure
 * @from:	iov_iter with data to write
 *
 * This is a wrapper around __generic_file_write_iter() to be used by most
 * filesystems. It takes care of syncing the file in case of O_SYNC file
 * and acquires i_rwsem as needed.
 * Return:
 * * negative error code if no data has been written at all of
 *   vfs_fsync_range() failed for a synchronous write
 * * number of bytes written, even for truncated writes
 */
ssize_t generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	ssize_t ret;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret > 0)
		ret = __generic_file_write_iter(iocb, from);
	inode_unlock(inode);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}
EXPORT_SYMBOL(generic_file_write_iter);

/**
 * filemap_release_folio() - Release fs-specific metadata on a folio.
 * @folio: The folio which the kernel is trying to free.
 * @gfp: Memory allocation flags (and I/O mode).
 *
 * The address_space is trying to release any data attached to a folio
 * (presumably at folio->private).
 *
 * This will also be called if the private_2 flag is set on a page,
 * indicating that the folio has other metadata associated with it.
 *
 * The @gfp argument specifies whether I/O may be performed to release
 * this page (__GFP_IO), and whether the call may block
 * (__GFP_RECLAIM & __GFP_FS).
 *
 * Return: %true if the release was successful, otherwise %false.
 */
bool filemap_release_folio(struct folio *folio, gfp_t gfp)
{
	struct address_space * const mapping = folio->mapping;

	BUG_ON(!folio_test_locked(folio));
	if (folio_test_writeback(folio))
		return false;

	if (mapping && mapping->a_ops->releasepage)
		return mapping->a_ops->releasepage(&folio->page, gfp);
	return try_to_free_buffers(&folio->page);
}
EXPORT_SYMBOL(filemap_release_folio);
