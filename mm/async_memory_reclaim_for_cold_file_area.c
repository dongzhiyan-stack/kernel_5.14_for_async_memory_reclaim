#include "async_memory_reclaim_for_cold_file_area.h"

struct hot_cold_file_global hot_cold_file_global_info;
unsigned long async_memory_reclaim_status = 1;

unsigned int file_area_in_update_count;
unsigned int file_area_in_update_lock_count;
unsigned int file_area_move_to_head_count;

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
/*在判定一个file_area长时间没人访问后，执行该函数delete file_area。必须考虑此时有进程正好要并发访问这个file_area*/
static int cold_file_area_detele(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct file_area *p_file_area)
{
    XA_STATE(xas, &((struct address_space *)(p_file_stat->mapping))->i_pages, p_file_area->start_index >> PAGE_COUNT_IN_AREA_SHIFT);

	/*在释放file_area时，可能正有进程执行hot_file_update_file_status()遍历file_area_tree树中p_file_area指向的file_area结构，需要加锁*/
	/*如果近期file_area被访问了则不能再释放掉file_area*/

	/*现在不能再通过file_area的age判断了，再这个场景会有bug：假设file_area没有page，进程1执行filemap_get_read_batch()从xarray tree遍历到
	 *这个file_area，没找到page。因为没找到page，则不会更新global age到file_area的age。此时进程2执行cold_file_area_detele()函数里要是delete该
	 file_area，ile_area的age与global age还是相差很大，下边这个if依然不成立。

	 接着就存在一个并发问题:进程1正执行__filemap_add_folio()分配page并保存到file_area。此时如果进程2执行cold_file_area_detele()函数
	 delete file_area。此时靠xas_lock解决并发问题：

	 1：如果进程2先在cold_file_area_detele()先获取xas_lock，执行xas_store(&xas, NULL)把file_area从xarray tree剔除，接着要call_rcu延迟释放file_area。
	 因为进程1等此时可能还在filemap_get_read_batch()或mapping_get_entry()使用这个file_area。但他们都有rcu_read_lock，
	 等他们rcu_unlock_lock由内核自动释放掉file_area。继续，等进程2filemap_get_read_batch()中发现从xarray tree找到的file_area没有page，
	 然后执行到__filemap_add_folio()函数：但获取到xas_lock后，执行xas_for_each_conflict(&xas, entry)已经查不到这个file_area了，因为已经被进程2
	 执行xas_store(&xas, NULL)被从xarray tree剔除了，则进程1只能在__filemap_add_folio()里分配新的file_area了，不再使用老的file_area

	 2：如果进程1先在__filemap_add_folio()获取xas_lock，则分配page并添加到file_area里。然后进程2cold_file_area_detele()获取xas_lock，发现
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

	spin_lock(&p_file_stat->file_stat_lock);
	if(0 == p_file_stat->file_area_count)
		panic("%s file_stat:0x%llx file_area:0x%llx file_area_count == 0 error\n",__func__,(u64)p_file_stat,(u64)p_file_area);
	/*该文件file_stat的file_area个数减1，把file_area从file_stat的链表剔除，这个过程要加锁*/
	p_file_stat->file_area_count --;
	list_del_rcu(&p_file_area->file_area_list);
	spin_unlock(&p_file_stat->file_stat_lock);

	/*rcu延迟释放file_area结构*/
	call_rcu(&p_file_area->i_rcu, i_file_area_callback);

	if(open_file_area_printk)
		printk("%s file_area:0x%llx delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area);

	return 0;
}
/*在文件inode被iput释放后，执行该函数释放该文件file_stat的所有file_area，此时肯定没进程再访问该文件的file_stat、file_area，不用考虑并发。
 *错了，此时可能异步内存线程也会因这个文件长时间空闲而释放file_stat和file_area。又错了，当前函数就是异步内存回收线程里执行的，没这种情况*/
static int cold_file_area_detele_nolock(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct file_area *p_file_area)
{
    //XA_STATE(xas, &((struct address_space *)(p_file_stat->mapping))->i_pages, p_file_area->start_index >> PAGE_COUNT_IN_AREA_SHIFT);
	
	//xas_lock_irq(&xas);
	if(file_area_have_page(p_file_area)){
		printk("%s file_area:0x%llx file_area_state:0x%x\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
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
	if(0 == p_file_stat->file_area_count)
		panic("%s file_stat:0x%llx file_area:0x%llx file_area_count == 0 error\n",__func__,(u64)p_file_stat,(u64)p_file_area);
	/*该文件file_stat的file_area个数减1，把file_area从file_stat的链表剔除，这个过程要加锁*/
	p_file_stat->file_area_count --;
	list_del_rcu(&p_file_area->file_area_list);
	//spin_unlock(&p_file_stat->file_stat_lock);

	/*隐藏重点!!!!!!!!!!!，此时可能有进程正通过proc查询该文件的file_stat、file_area、page统计信息，正在用他们。因此也不能
	 *kmem_cache_free()直接释放该数据结构，也必须得通过rcu延迟释放，并且，这些通过proc查询的进程，必须得先rcu_read_lock，
	 再查询file_stat、file_area、page统计信息，保证rcu_read_unlock前，他们不会被释放掉*/
	//kmem_cache_free(hot_cold_file_global_info.file_area_cachep,p_file_area);
	
	/*rcu延迟释放file_area结构*/
	call_rcu(&p_file_area->i_rcu, i_file_area_callback);

	if(open_file_area_printk)
		printk("%s file_area:0x%llx delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_area);

	return 0;
}

/*在判定一个文件file_stat的page全部被释放，然后过了很长时间依然没人访问，执行该函数delete file_stat。必须考虑此时有进程并发访问该文件file_stat*/
static int  cold_file_stat_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat_del)
{
	struct xarray *xarray_i_pages = &((struct address_space *)(p_file_stat_del->mapping))->i_pages;
	/*并发问题:进程1执行filemap_get_read_batch()，，从xarray tree找不到file_area，xarray tree是空树，mapping->rh_reserved1
	 *非NULL。然后执行到__filemap_add_folio()函数，打算分配file_area、分配page并保存到file_area。此时如果进程2执行
	 cold_file_stat_delete()函数delete stat。靠xas_lock(跟xa_lock一样)解决并发问题：

	 1：如果进程2先在cold_file_stat_delete()先获取xa_lock，执行p_file_stat_del->mapping->rh_reserved1 = 0令mapping的file_stat无效，
	 接着要call_rcu延迟释放file_stat。因为进程1等此时可能还在filemap_get_read_batch()或mapping_get_entry()使用这个file_stat。
	 但他们都有rcu_read_lock，等他们rcu_unlock_lock由内核自动释放掉file_stat。等进程2执行到__filemap_add_folio()，
	 获取到xas_lock后，执行if(0 == mapping->rh_reserved1)，if成立。则只能分配新的file_stat了，不会再使用老的file_stat
	 2：如果进程1先在__filemap_add_folio()获取xa_lock，则分配file_area、分配page并添加到file_area里。然后进程2执行到cold_file_stat_delete()
	 获取xa_lock好，发现file_stat已经有了file_aree，if(p_file_stat_del->file_area_count > 0)，则不会再释放该file_stat了
	 */

	//lock_file_stat(p_file_stat_del,0);
	//spin_lock(&p_file_stat_del->file_stat_lock);
	xa_lock_irq(xarray_i_pages);
    
	/*正常这里释放文件file_stat结构时，file_area指针的xarray tree的所有成员都已经释放，是个空树，否则就触发panic*/
	if(p_file_stat->mapping->xa_head != NULL)
		panic("file_stat_del:0x%llx file_area_count:%d !=0 p_file_stat->mapping:0x%llx !!!!!!!!\n",(u64)p_file_stat_del,p_file_stat_del->file_area_state,(u64)p_file_stat_del->mapping->rh_reserved1);

	/*如果file_stat的file_area个数大于0，说明此时该文件被方法访问了，在hot_file_update_file_status()中分配新的file_area。
	 *此时这个file_stat就不能释放了*/
	if(p_file_stat_del->file_area_count > 0){
		/*一般此时file_stat是不可能有delete标记的，但可能inode释放时__destroy_inode_handler_post中设置了delete。正常不可能，这里有lock_file_stat加锁防护*/
		if(file_stat_in_delete(p_file_stat_del)){
			printk("%s %s %d file_stat:0x%llx status:0x%lx in delete\n",__func__,current->comm,current->pid,(u64)p_file_stat_del,p_file_stat_del->file_stat_status);
			dump_stack();
		}	
		//spin_unlock(&p_file_stat_del->file_stat_lock);
		//unlock_file_stat(p_file_stat_del);
		xa_unlock_irq(xarray_i_pages);
		return 1;
	}
	/*如果file_stat在__destroy_inode_handler_post中被释放了，file_stat一定有delete标记。否则是空闲file_stat被delete，这里得标记file_stat delete。
	 *这段对mapping->rh_reserved1清0的必须放到xa_lock_irq加锁里，因为会跟__filemap_add_folio()里判断mapping->rh_reserved1的代码构成并发*/
	if(0 == file_stat_in_delete(p_file_stat_del)/*p_file_stat_del->mapping*/){
		/*文件inode的mapping->rh_reserved1清0表示file_stat无效，这__destroy_inode_handler_post()删除inode时，发现inode的mapping->rh_reserved1是0就不再使用file_stat了，会crash*/
		p_file_stat_del->mapping->rh_reserved1 = 0;
		barrier();
		p_file_stat_del->mapping = NULL;
	}
	//spin_unlock(&p_file_stat_del->file_stat_lock);
	//unlock_file_stat(p_file_stat_del);
	xa_unlock_irq(xarray_i_pages);

	/*到这里，一定可以确保file_stat跟mapping没关系了，因为mapping->rh_reserved1是0，但不能保证不被其他进程在filemap_get_read_batch()
	 *或mapping_get_entry()在mapping->rh_reserved1是0前已经并发访问了file_stat，现在还在使用，好在他们访问file_stat前都rcu_read_lock了，
	 等他们rcu_read_unlock才能真正释放file_stat结构。这个工作就需要rcu延迟释放file_area结构*/

	/*使用global_lock加锁是因为要把file_stat从p_hot_cold_file_global的链表中剔除，防止此时其他进程并发向p_hot_cold_file_global的链表添加file_stat,
	 *是否有必要改为 spin_lock_irqsave，之前就遇到过用spin_lock_irq导致打乱中断状态而造成卡死?????????????????????????????????????????????*/
	spin_lock_irq(&p_hot_cold_file_global->global_lock);
	/*在这个加个内存屏障，保证前后代码隔离开。即file_stat有delete标记后，inode->i_mapping->rh_reserved1一定是0，p_file_stat->mapping一定是NULL*/
	smp_wmb();
	set_file_stat_in_delete(p_file_stat_del);
	//从global的链表中剔除该file_stat，这个过程需要加锁，因为同时其他进程会执行hot_file_update_file_status()向global的链表添加新的文件file_stat
	list_del_rcu(&p_file_stat_del->hot_cold_file_list);
	//file_stat个数减1
	hot_cold_file_global_info.file_stat_count --;
	spin_unlock_irq(&p_hot_cold_file_global->global_lock);

	/*rcu延迟释放file_stat结构*/
	call_rcu(&p_file_stat_del->i_rcu, i_file_stat_callback);

	if(open_file_area_printk)
		printk("%s file_stat:0x%llx delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_stat_del);

	return 0;
}
/*在文件inode被iput释放后，执行该函数释放该文件file_stat，此时肯定没进程再访问该文件，不用考虑并发*/
static int  cold_file_stat_delete_nolock(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat_del)
{
	/*二者先前已经在__destroy_inode_handler_post()处理过，不可能成立*/
	if(!file_stat_in_delete(p_file_stat_del) || p_file_stat->mapping->rh_reserved1)
		panic("file_stat_del:0x%llx file_area_count:%d !=0 p_file_stat->mapping:0x%llx !!!!!!!!\n",(u64)p_file_stat_del,p_file_stat_del->file_area_state,(u64)p_file_stat_del->mapping->rh_reserved1);

    /*正常这里释放文件file_stat结构时，file_area指针的xarray tree的所有成员都已经释放，是个空树，否则就触发panic*/
	if(p_file_stat->mapping->xa_head != NULL)
		panic("file_stat_del:0x%llx file_area_count:%d !=0 p_file_stat->mapping:0x%llx !!!!!!!!\n",(u64)p_file_stat_del,p_file_stat_del->file_area_state,(u64)p_file_stat_del->mapping->rh_reserved1);


	/*使用global_lock加锁是因为要把file_stat从p_hot_cold_file_global的链表中剔除，防止此时其他进程并发向p_hot_cold_file_global的链表添加file_stat,
	 *是否有必要改为 spin_lock_irqsave，之前就遇到过用spin_lock_irq导致打乱中断状态而造成卡死?????????????????????????????????????????????*/
	spin_lock_irq(&p_hot_cold_file_global->global_lock);
	//从global的链表中剔除该file_stat，这个过程需要加锁，因为同时其他进程会执行hot_file_update_file_status()向global的链表添加新的文件file_stat
	list_del_rcu(&p_file_stat_del->hot_cold_file_list);
	//file_stat个数减1
	hot_cold_file_global_info.file_stat_count --;
	spin_unlock_irq(&p_hot_cold_file_global->global_lock);

	/*隐藏重点，此时可能有进程正通过proc查询该文件的file_stat、file_area、page统计信息，正在用他们。因此也不能
	 *kmem_cache_free()直接释放该数据结构，也必须得通过rcu延迟释放，并且，这些通过proc查询的进程，必须得先rcu_read_lock，
	 再查询file_stat、file_area、page统计信息，保证rcu_read_unlock前，他们不会被释放掉*/
	//kmem_cache_free(hot_cold_file_global_info.file_stat_cachep,p_file_stat);
	/*rcu延迟释放file_stat结构*/
	call_rcu(&p_file_stat_del->i_rcu, i_file_stat_callback);

	if(open_file_area_printk)
		printk("%s file_stat:0x%llx delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_stat_del);

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
 cold_file_area_detele_nolock()函数中，执行xas_store(&xas, NULL)，在xarray tree的槽位设置file_area NULL外，还自动逐级向上释放
 父节点xa_node。又错了，这里又有个重大bug!!!!!!!!!!!!!!!!__destroy_inode_handler_post()执行后，inode就会被立即释放掉，然后
 异步内存回收线程执行到cold_file_area_detele_nolock()函数时，文件inode、mapping、xarray tree都释放了，此时执行
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
 __destroy_inode_handler_post()，标记file_stat delete。然后等异步内存回收线程里，执行cold_file_area_detele_nolock释放掉file_stat的
 所有file_area后，再执行destroy_inode释放掉inode、mapping、xarray tree。或者两个方法结合起来，当xarray tree的层数大于1时，是大文件，
 则使用该方法延迟释放inode。当xarray tree的层数是1，是小文件，直接在__destroy_inode_handler_post()里强制执行xas_store(&xas, NULL)
 释放xarray tree。使用延迟释放inode的方法吧，在__destroy_inode_handler_post()里强制释放xarray tree需要编写一个函数：xarray tree
 遍历所有的file_area，依次执行xas_store(&xas, NULL)释放xarray tree，这个函数评估比较复杂。但是延迟释放inode需要对evict->destroy_inode
 流程有较大改动，怕改出问题，其实也还好

 又想到一个方法，在__destroy_inode_handler_post里通过inode->i_data.i_pages备份一份该文件的xarray数据结构，然后异步内存回收线程里
 直接依赖这个备份的xarray tree，依次执行xas_store(&xas, NULL)释放xarray tree，就可以了xas_store(&xas, NULL)释放xarray tree了。但是
 这个方法感觉是旁门左道。

*/
static void __destroy_inode_handler_post(struct inode *inode)
{
	if(inode && inode->i_mapping && inode->i_mapping->rh_reserved1){
		struct file_stat *p_file_stat = (struct file_stat *)inode->i_mapping->rh_reserved1;

		inode->i_mapping->rh_reserved1 = 0;
		/*这里不能p_file_stat->mapping清NULL，因为接着执行cold_file_area_detele_nolock()释放该文件file_stat的所有file_area时，
		 *还要用到p_file_stat->mapping而从xarray tree查找file_area*/
		p_file_stat->mapping = NULL;

		/*在这个加个内存屏障，保证前后代码隔离开。即file_stat有delete标记后，inode->i_mapping->rh_reserved1一定是0，p_file_stat->mapping一定是NULL*/
		smp_wmb();

		spin_lock_irq(&hot_cold_file_global_info.global_lock);
		/*file_stat不一定只会在global temp链表，也会在global hot等链表，这些标记不清理了*/
		//clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
		set_file_stat_in_delete(p_file_stat);
		list_move(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.file_stat_delete_head);
		spin_unlock_irq(&hot_cold_file_global_info.global_lock);
		if(open_file_area_printk)
			printk("%s file_stat:0x%llx iput delete !!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_stat);
	}
}
void disable_mapping_file_area(struct inode *inode)
{
	__destroy_inode_handler_post(inode);
}
EXPORT_SYMBOL(disable_mapping_file_area);

static int  cold_mmap_file_stat_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat_del)
{
    return 0;
}
//删除p_file_stat_del对应文件的file_stat上所有的file_area，已经对应hot file tree的所有节点hot_cold_file_area_tree_node结构。最后释放掉p_file_stat_del这个file_stat数据结构
static unsigned int cold_file_stat_delete_all_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat_del)
{
	//struct file_stat * p_file_stat,*p_file_stat_temp;
	struct file_area *p_file_area,*p_file_area_temp;
	unsigned int del_file_area_count = 0;
	//refault链表
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_refault,file_area_list){
		if(!file_area_in_refault_list(p_file_area) || file_area_in_refault_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_refault\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

		cold_file_area_detele_nolock(p_hot_cold_file_global,p_file_stat_del,p_file_area);
		del_file_area_count ++;
	}
	//hot链表
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_hot,file_area_list){
		if(!file_area_in_hot_list(p_file_area) || file_area_in_hot_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_hot\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

		cold_file_area_detele_nolock(p_hot_cold_file_global,p_file_stat_del,p_file_area);
		del_file_area_count ++;
	}
	//temp链表
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_temp,file_area_list){
		if(!file_area_in_temp_list(p_file_area) || file_area_in_temp_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

		cold_file_area_detele_nolock(p_hot_cold_file_global,p_file_stat_del,p_file_area);
		del_file_area_count ++;
	}
	//free链表
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_free,file_area_list){
		if(!file_area_in_free_list(p_file_area) || file_area_in_free_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

		cold_file_area_detele_nolock(p_hot_cold_file_global,p_file_stat_del,p_file_area);
		del_file_area_count ++;
	}
	//free_temp链表
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_free_temp,file_area_list){
		if(!file_area_in_free_list(p_file_area) || file_area_in_free_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_free_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

		cold_file_area_detele_nolock(p_hot_cold_file_global,p_file_stat_del,p_file_area);
		del_file_area_count ++;
	}
	//mapcount链表
	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,&p_file_stat_del->file_area_mapcount,file_area_list){
		if(!file_area_in_mapcount_list(p_file_area) || file_area_in_free_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_mapcount\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

		cold_file_area_detele_nolock(p_hot_cold_file_global,p_file_stat_del,p_file_area);
		del_file_area_count ++;
	}


	if(p_file_stat_del->file_area_count != 0){
		panic("file_stat_del:0x%llx file_area_count:%d !=0 !!!!!!!!\n",(u64)p_file_stat_del,p_file_stat_del->file_area_count);
	}

	//把file_stat从p_hot_cold_file_global的链表中剔除，然后释放file_stat结构
	if(file_stat_in_cache_file(p_file_stat_del))
		cold_file_stat_delete_nolock(p_hot_cold_file_global,p_file_stat_del);
	else
		cold_mmap_file_stat_delete_nolock(p_hot_cold_file_global,p_file_stat_del);

	return del_file_area_count;
}
/**************************************************************************************/

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
	unsigned int del_file_stat_count = 0,del_file_area_count = 0;
	struct file_stat * p_file_stat,*p_file_stat_temp;
#if 0	
	struct file_area *p_file_area,*p_file_area_temp;
	//LIST_HEAD(file_area_list);
	LIST_HEAD(file_stat_free_list_from_head_temp);
	LIST_HEAD(file_stat_free_list_from_head_temp_large);
	unsigned int scan_file_area_max,scan_file_stat_max;
	unsigned int scan_cold_file_area_count = 0;
	unsigned long nr_reclaimed = 0;
	unsigned int cold_file_area_count;
	unsigned int file_area_hot_to_temp_list_count = 0;

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
#endif
	/*遍历global file_stat_delete_head链表上已经被删除的文件的file_stat，
	  一次不能删除太多的file_stat对应的file_area，会长时间占有cpu，后期需要调优一下*/
	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->file_stat_delete_head,hot_cold_file_list){
		if(!file_stat_in_delete(p_file_stat) /*|| file_stat_in_delete_error(p_file_stat)*/)
			panic("%s file_stat:0x%llx not delete status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		del_file_area_count += cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat);
		del_file_stat_count ++;
	}
#if 0
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
			file_area_move_to_head_count = 0;
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
