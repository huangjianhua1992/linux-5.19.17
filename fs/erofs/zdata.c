// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 HUAWEI, Inc.
 *             https://www.huawei.com/
 */
#include "zdata.h"
#include "compress.h"
#include <linux/prefetch.h>

#include <trace/events/erofs.h>

/*
 * since pclustersize is variable for big pcluster feature, introduce slab
 * pools implementation for different pcluster sizes.
 */
struct z_erofs_pcluster_slab {
	struct kmem_cache *slab;
	unsigned int maxpages;
	char name[48];
};

#define _PCLP(n) { .maxpages = n }

static struct z_erofs_pcluster_slab pcluster_pool[] __read_mostly = {
	_PCLP(1), _PCLP(4), _PCLP(16), _PCLP(64), _PCLP(128),
	_PCLP(Z_EROFS_PCLUSTER_MAX_PAGES)
};

static void z_erofs_destroy_pcluster_pool(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pcluster_pool); ++i) {
		if (!pcluster_pool[i].slab)
			continue;
		kmem_cache_destroy(pcluster_pool[i].slab);
		pcluster_pool[i].slab = NULL;
	}
}

static int z_erofs_create_pcluster_pool(void)
{
	struct z_erofs_pcluster_slab *pcs;
	struct z_erofs_pcluster *a;
	unsigned int size;

	for (pcs = pcluster_pool;
	     pcs < pcluster_pool + ARRAY_SIZE(pcluster_pool); ++pcs) {
		size = struct_size(a, compressed_pages, pcs->maxpages);

		sprintf(pcs->name, "erofs_pcluster-%u", pcs->maxpages);
		pcs->slab = kmem_cache_create(pcs->name, size, 0,
					      SLAB_RECLAIM_ACCOUNT, NULL);
		if (pcs->slab)
			continue;

		z_erofs_destroy_pcluster_pool();
		return -ENOMEM;
	}
	return 0;
}

static struct z_erofs_pcluster *z_erofs_alloc_pcluster(unsigned int nrpages)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pcluster_pool); ++i) {
		struct z_erofs_pcluster_slab *pcs = pcluster_pool + i;
		struct z_erofs_pcluster *pcl;

		if (nrpages > pcs->maxpages)
			continue;

		pcl = kmem_cache_zalloc(pcs->slab, GFP_NOFS);
		if (!pcl)
			return ERR_PTR(-ENOMEM);
		pcl->pclusterpages = nrpages;
		return pcl;
	}
	return ERR_PTR(-EINVAL);
}

static void z_erofs_free_pcluster(struct z_erofs_pcluster *pcl)
{
	unsigned int pclusterpages = z_erofs_pclusterpages(pcl);
	int i;

	for (i = 0; i < ARRAY_SIZE(pcluster_pool); ++i) {
		struct z_erofs_pcluster_slab *pcs = pcluster_pool + i;

		if (pclusterpages > pcs->maxpages)
			continue;

		kmem_cache_free(pcs->slab, pcl);
		return;
	}
	DBG_BUGON(1);
}

/* how to allocate cached pages for a pcluster */
enum z_erofs_cache_alloctype {
	DONTALLOC,	/* don't allocate any cached pages */
	/*
	 * try to use cached I/O if page allocation succeeds or fallback
	 * to in-place I/O instead to avoid any direct reclaim.
	 */
	TRYALLOC,
};

/*
 * tagged pointer with 1-bit tag for all compressed pages
 * tag 0 - the page is just found with an extra page reference
 */
typedef tagptr1_t compressed_page_t;

#define tag_compressed_page_justfound(page) \
	tagptr_fold(compressed_page_t, page, 1)

static struct workqueue_struct *z_erofs_workqueue __read_mostly;

void z_erofs_exit_zip_subsystem(void)
{
	destroy_workqueue(z_erofs_workqueue);
	z_erofs_destroy_pcluster_pool();
}

static inline int z_erofs_init_workqueue(void)
{
	const unsigned int onlinecpus = num_possible_cpus();

	/*
	 * no need to spawn too many threads, limiting threads could minimum
	 * scheduling overhead, perhaps per-CPU threads should be better?
	 */
	z_erofs_workqueue = alloc_workqueue("erofs_unzipd",
					    WQ_UNBOUND | WQ_HIGHPRI,
					    onlinecpus + onlinecpus / 4);
	return z_erofs_workqueue ? 0 : -ENOMEM;
}

int __init z_erofs_init_zip_subsystem(void)
{
	int err = z_erofs_create_pcluster_pool();

	if (err)
		return err;
	err = z_erofs_init_workqueue();
	if (err)
		z_erofs_destroy_pcluster_pool();
	return err;
}

enum z_erofs_collectmode {
	COLLECT_SECONDARY,
	COLLECT_PRIMARY,
	/*
	 * The current collection was the tail of an exist chain, in addition
	 * that the previous processed chained collections are all decided to
	 * be hooked up to it.
	 * A new chain will be created for the remaining collections which are
	 * not processed yet, therefore different from COLLECT_PRIMARY_FOLLOWED,
	 * the next collection cannot reuse the whole page safely in
	 * the following scenario:
	 *  ________________________________________________________________
	 * |      tail (partial) page     |       head (partial) page       |
	 * |   (belongs to the next cl)   |   (belongs to the current cl)   |
	 * |_______PRIMARY_FOLLOWED_______|________PRIMARY_HOOKED___________|
	 */
	COLLECT_PRIMARY_HOOKED,
	/*
	 * a weak form of COLLECT_PRIMARY_FOLLOWED, the difference is that it
	 * could be dispatched into bypass queue later due to uptodated managed
	 * pages. All related online pages cannot be reused for inplace I/O (or
	 * pagevec) since it can be directly decoded without I/O submission.
	 */
	COLLECT_PRIMARY_FOLLOWED_NOINPLACE,
	/*
	 * The current collection has been linked with the owned chain, and
	 * could also be linked with the remaining collections, which means
	 * if the processing page is the tail page of the collection, thus
	 * the current collection can safely use the whole page (since
	 * the previous collection is under control) for in-place I/O, as
	 * illustrated below:
	 *  ________________________________________________________________
	 * |  tail (partial) page |          head (partial) page           |
	 * |  (of the current cl) |      (of the previous collection)      |
	 * |  PRIMARY_FOLLOWED or |                                        |
	 * |_____PRIMARY_HOOKED___|____________PRIMARY_FOLLOWED____________|
	 *
	 * [  (*) the above page can be used as inplace I/O.               ]
	 */
	COLLECT_PRIMARY_FOLLOWED,
};

struct z_erofs_decompress_frontend {
	struct inode *const inode;
	struct erofs_map_blocks map;

	struct z_erofs_pagevec_ctor vector;

	struct z_erofs_pcluster *pcl, *tailpcl;
	/* a pointer used to pick up inplace I/O pages */
	struct page **icpage_ptr;
	z_erofs_next_pcluster_t owned_head;

	enum z_erofs_collectmode mode;

	bool readahead;
	/* used for applying cache strategy on the fly */
	bool backmost;
	erofs_off_t headoffset;
};

#define DECOMPRESS_FRONTEND_INIT(__i) { \
	.inode = __i, .owned_head = Z_EROFS_PCLUSTER_TAIL, \
	.mode = COLLECT_PRIMARY_FOLLOWED, .backmost = true }

static struct page *z_pagemap_global[Z_EROFS_VMAP_GLOBAL_PAGES];
static DEFINE_MUTEX(z_pagemap_global_lock);

static void z_erofs_bind_cache(struct z_erofs_decompress_frontend *fe,
			       enum z_erofs_cache_alloctype type,
			       struct page **pagepool)
{
	struct address_space *mc = MNGD_MAPPING(EROFS_I_SB(fe->inode));
	struct z_erofs_pcluster *pcl = fe->pcl;
	bool standalone = true;
	/*
	 * optimistic allocation without direct reclaim since inplace I/O
	 * can be used if low memory otherwise.
	 */
	gfp_t gfp = (mapping_gfp_mask(mc) & ~__GFP_DIRECT_RECLAIM) |
			__GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN;
	struct page **pages;
	pgoff_t index;

	if (fe->mode < COLLECT_PRIMARY_FOLLOWED)
		return;

	pages = pcl->compressed_pages;
	index = pcl->obj.index;
	for (; index < pcl->obj.index + pcl->pclusterpages; ++index, ++pages) {
		struct page *page;
		compressed_page_t t;
		struct page *newpage = NULL;

		/* the compressed page was loaded before */
		if (READ_ONCE(*pages))
			continue;

		page = find_get_page(mc, index);

		if (page) {
			t = tag_compressed_page_justfound(page);
		} else {
			/* I/O is needed, no possible to decompress directly */
			standalone = false;
			switch (type) {
			case TRYALLOC:
				newpage = erofs_allocpage(pagepool, gfp);
				if (!newpage)
					continue;
				set_page_private(newpage,
						 Z_EROFS_PREALLOCATED_PAGE);
				t = tag_compressed_page_justfound(newpage);
				break;
			default:        /* DONTALLOC */
				continue;
			}
		}

		if (!cmpxchg_relaxed(pages, NULL, tagptr_cast_ptr(t)))
			continue;

		if (page)
			put_page(page);
		else if (newpage)
			erofs_pagepool_add(pagepool, newpage);
	}

	/*
	 * don't do inplace I/O if all compressed pages are available in
	 * managed cache since it can be moved to the bypass queue instead.
	 */
	if (standalone)
		fe->mode = COLLECT_PRIMARY_FOLLOWED_NOINPLACE;
}

/* called by erofs_shrinker to get rid of all compressed_pages */
int erofs_try_to_free_all_cached_pages(struct erofs_sb_info *sbi,
				       struct erofs_workgroup *grp)
{
	struct z_erofs_pcluster *const pcl =
		container_of(grp, struct z_erofs_pcluster, obj);
	int i;

	DBG_BUGON(z_erofs_is_inline_pcluster(pcl));
	/*
	 * refcount of workgroup is now freezed as 1,
	 * therefore no need to worry about available decompression users.
	 */
	for (i = 0; i < pcl->pclusterpages; ++i) {
		struct page *page = pcl->compressed_pages[i];

		if (!page)
			continue;

		/* block other users from reclaiming or migrating the page */
		if (!trylock_page(page))
			return -EBUSY;

		if (!erofs_page_is_managed(sbi, page))
			continue;

		/* barrier is implied in the following 'unlock_page' */
		WRITE_ONCE(pcl->compressed_pages[i], NULL);
		detach_page_private(page);
		unlock_page(page);
	}
	return 0;
}

int erofs_try_to_free_cached_page(struct page *page)
{
	struct z_erofs_pcluster *const pcl = (void *)page_private(page);
	int ret = 0;	/* 0 - busy */

	if (erofs_workgroup_try_to_freeze(&pcl->obj, 1)) {
		unsigned int i;

		DBG_BUGON(z_erofs_is_inline_pcluster(pcl));
		for (i = 0; i < pcl->pclusterpages; ++i) {
			if (pcl->compressed_pages[i] == page) {
				WRITE_ONCE(pcl->compressed_pages[i], NULL);
				ret = 1;
				break;
			}
		}
		erofs_workgroup_unfreeze(&pcl->obj, 1);

		if (ret)
			detach_page_private(page);
	}
	return ret;
}

/* page_type must be Z_EROFS_PAGE_TYPE_EXCLUSIVE */
static bool z_erofs_try_inplace_io(struct z_erofs_decompress_frontend *fe,
				   struct page *page)
{
	struct z_erofs_pcluster *const pcl = fe->pcl;

	while (fe->icpage_ptr > pcl->compressed_pages)
		if (!cmpxchg(--fe->icpage_ptr, NULL, page))
			return true;
	return false;
}

/* callers must be with pcluster lock held */
static int z_erofs_attach_page(struct z_erofs_decompress_frontend *fe,
			       struct page *page, enum z_erofs_page_type type,
			       bool pvec_safereuse)
{
	int ret;

	/* give priority for inplaceio */
	if (fe->mode >= COLLECT_PRIMARY &&
	    type == Z_EROFS_PAGE_TYPE_EXCLUSIVE &&
	    z_erofs_try_inplace_io(fe, page))
		return 0;

	ret = z_erofs_pagevec_enqueue(&fe->vector, page, type,
				      pvec_safereuse);
	fe->pcl->vcnt += (unsigned int)ret;
	return ret ? 0 : -EAGAIN;
}

static void z_erofs_try_to_claim_pcluster(struct z_erofs_decompress_frontend *f)
{
	struct z_erofs_pcluster *pcl = f->pcl;
	z_erofs_next_pcluster_t *owned_head = &f->owned_head;

	/* type 1, nil pcluster (this pcluster doesn't belong to any chain.) */
	if (cmpxchg(&pcl->next, Z_EROFS_PCLUSTER_NIL,
		    *owned_head) == Z_EROFS_PCLUSTER_NIL) {
		*owned_head = &pcl->next;
		/* so we can attach this pcluster to our submission chain. */
		f->mode = COLLECT_PRIMARY_FOLLOWED;
		return;
	}

	/*
	 * type 2, link to the end of an existing open chain, be careful
	 * that its submission is controlled by the original attached chain.
	 */
	if (cmpxchg(&pcl->next, Z_EROFS_PCLUSTER_TAIL,
		    *owned_head) == Z_EROFS_PCLUSTER_TAIL) {
		*owned_head = Z_EROFS_PCLUSTER_TAIL;
		f->mode = COLLECT_PRIMARY_HOOKED;
		f->tailpcl = NULL;
		return;
	}
	/* type 3, it belongs to a chain, but it isn't the end of the chain */
	f->mode = COLLECT_PRIMARY;
}

static int z_erofs_lookup_pcluster(struct z_erofs_decompress_frontend *fe,
				   struct inode *inode,
				   struct erofs_map_blocks *map)
{
	struct z_erofs_pcluster *pcl = fe->pcl;
	unsigned int length;

	/* to avoid unexpected loop formed by corrupted images */
	if (fe->owned_head == &pcl->next || pcl == fe->tailpcl) {
		DBG_BUGON(1);
		return -EFSCORRUPTED;
	}

	if (pcl->pageofs_out != (map->m_la & ~PAGE_MASK)) {
		DBG_BUGON(1);
		return -EFSCORRUPTED;
	}

	length = READ_ONCE(pcl->length);
	if (length & Z_EROFS_PCLUSTER_FULL_LENGTH) {
		if ((map->m_llen << Z_EROFS_PCLUSTER_LENGTH_BIT) > length) {
			DBG_BUGON(1);
			return -EFSCORRUPTED;
		}
	} else {
		unsigned int llen = map->m_llen << Z_EROFS_PCLUSTER_LENGTH_BIT;

		if (map->m_flags & EROFS_MAP_FULL_MAPPED)
			llen |= Z_EROFS_PCLUSTER_FULL_LENGTH;

		while (llen > length &&
		       length != cmpxchg_relaxed(&pcl->length, length, llen)) {
			cpu_relax();
			length = READ_ONCE(pcl->length);
		}
	}
	mutex_lock(&pcl->lock);
	/* used to check tail merging loop due to corrupted images */
	if (fe->owned_head == Z_EROFS_PCLUSTER_TAIL)
		fe->tailpcl = pcl;

	z_erofs_try_to_claim_pcluster(fe);
	return 0;
}

static int z_erofs_register_pcluster(struct z_erofs_decompress_frontend *fe,
				     struct inode *inode,
				     struct erofs_map_blocks *map)
{
	bool ztailpacking = map->m_flags & EROFS_MAP_META;
	struct z_erofs_pcluster *pcl;
	struct erofs_workgroup *grp;
	int err;

	if (!(map->m_flags & EROFS_MAP_ENCODED)) {
		DBG_BUGON(1);
		return -EFSCORRUPTED;
	}

	/* no available pcluster, let's allocate one */
	pcl = z_erofs_alloc_pcluster(ztailpacking ? 1 :
				     map->m_plen >> PAGE_SHIFT);
	if (IS_ERR(pcl))
		return PTR_ERR(pcl);

	atomic_set(&pcl->obj.refcount, 1);
	pcl->algorithmformat = map->m_algorithmformat;
	pcl->length = (map->m_llen << Z_EROFS_PCLUSTER_LENGTH_BIT) |
		(map->m_flags & EROFS_MAP_FULL_MAPPED ?
			Z_EROFS_PCLUSTER_FULL_LENGTH : 0);

	/* new pclusters should be claimed as type 1, primary and followed */
	pcl->next = fe->owned_head;
	pcl->pageofs_out = map->m_la & ~PAGE_MASK;
	fe->mode = COLLECT_PRIMARY_FOLLOWED;

	/*
	 * lock all primary followed works before visible to others
	 * and mutex_trylock *never* fails for a new pcluster.
	 */
	mutex_init(&pcl->lock);
	DBG_BUGON(!mutex_trylock(&pcl->lock));

	if (ztailpacking) {
		pcl->obj.index = 0;	/* which indicates ztailpacking */
		pcl->pageofs_in = erofs_blkoff(map->m_pa);
		pcl->tailpacking_size = map->m_plen;
	} else {
		pcl->obj.index = map->m_pa >> PAGE_SHIFT;

		grp = erofs_insert_workgroup(inode->i_sb, &pcl->obj);
		if (IS_ERR(grp)) {
			err = PTR_ERR(grp);
			goto err_out;
		}

		if (grp != &pcl->obj) {
			fe->pcl = container_of(grp,
					struct z_erofs_pcluster, obj);
			err = -EEXIST;
			goto err_out;
		}
	}
	/* used to check tail merging loop due to corrupted images */
	if (fe->owned_head == Z_EROFS_PCLUSTER_TAIL)
		fe->tailpcl = pcl;
	fe->owned_head = &pcl->next;
	fe->pcl = pcl;
	return 0;

err_out:
	mutex_unlock(&pcl->lock);
	z_erofs_free_pcluster(pcl);
	return err;
}

static int z_erofs_collector_begin(struct z_erofs_decompress_frontend *fe,
				   struct inode *inode,
				   struct erofs_map_blocks *map)
{
	struct erofs_workgroup *grp;
	int ret;

	DBG_BUGON(fe->pcl);

	/* must be Z_EROFS_PCLUSTER_TAIL or pointed to previous pcluster */
	DBG_BUGON(fe->owned_head == Z_EROFS_PCLUSTER_NIL);
	DBG_BUGON(fe->owned_head == Z_EROFS_PCLUSTER_TAIL_CLOSED);

	if (map->m_flags & EROFS_MAP_META) {
		if ((map->m_pa & ~PAGE_MASK) + map->m_plen > PAGE_SIZE) {
			DBG_BUGON(1);
			return -EFSCORRUPTED;
		}
		goto tailpacking;
	}

	grp = erofs_find_workgroup(inode->i_sb, map->m_pa >> PAGE_SHIFT);
	if (grp) {
		fe->pcl = container_of(grp, struct z_erofs_pcluster, obj);
	} else {
tailpacking:
		ret = z_erofs_register_pcluster(fe, inode, map);
		if (!ret)
			goto out;
		if (ret != -EEXIST)
			return ret;
	}

	ret = z_erofs_lookup_pcluster(fe, inode, map);
	if (ret) {
		erofs_workgroup_put(&fe->pcl->obj);
		return ret;
	}

out:
	z_erofs_pagevec_ctor_init(&fe->vector, Z_EROFS_NR_INLINE_PAGEVECS,
				  fe->pcl->pagevec, fe->pcl->vcnt);
	/* since file-backed online pages are traversed in reverse order */
	fe->icpage_ptr = fe->pcl->compressed_pages +
			z_erofs_pclusterpages(fe->pcl);
	return 0;
}

/*
 * keep in mind that no referenced pclusters will be freed
 * only after a RCU grace period.
 */
static void z_erofs_rcu_callback(struct rcu_head *head)
{
	z_erofs_free_pcluster(container_of(head,
			struct z_erofs_pcluster, rcu));
}

void erofs_workgroup_free_rcu(struct erofs_workgroup *grp)
{
	struct z_erofs_pcluster *const pcl =
		container_of(grp, struct z_erofs_pcluster, obj);

	call_rcu(&pcl->rcu, z_erofs_rcu_callback);
}

static bool z_erofs_collector_end(struct z_erofs_decompress_frontend *fe)
{
	struct z_erofs_pcluster *pcl = fe->pcl;

	if (!pcl)
		return false;

	z_erofs_pagevec_ctor_exit(&fe->vector, false);
	mutex_unlock(&pcl->lock);

	/*
	 * if all pending pages are added, don't hold its reference
	 * any longer if the pcluster isn't hosted by ourselves.
	 */
	if (fe->mode < COLLECT_PRIMARY_FOLLOWED_NOINPLACE)
		erofs_workgroup_put(&pcl->obj);

	fe->pcl = NULL;
	return true;
}

static bool should_alloc_managed_pages(struct z_erofs_decompress_frontend *fe,
				       unsigned int cachestrategy,
				       erofs_off_t la)
{
	if (cachestrategy <= EROFS_ZIP_CACHE_DISABLED)
		return false;

	if (fe->backmost)
		return true;

	return cachestrategy >= EROFS_ZIP_CACHE_READAROUND &&
		la < fe->headoffset;
}

static int z_erofs_do_read_page(struct z_erofs_decompress_frontend *fe,
				struct page *page, struct page **pagepool)
{
	struct inode *const inode = fe->inode;
	struct erofs_sb_info *const sbi = EROFS_I_SB(inode);
	struct erofs_map_blocks *const map = &fe->map;
	const loff_t offset = page_offset(page);
	bool tight = true;

	enum z_erofs_cache_alloctype cache_strategy;
	enum z_erofs_page_type page_type;
	unsigned int cur, end, spiltted, index;
	int err = 0;

	/* register locked file pages as online pages in pack */
	z_erofs_onlinepage_init(page);

	spiltted = 0;
	end = PAGE_SIZE;
repeat:
	cur = end - 1;

	if (offset + cur < map->m_la ||
	    offset + cur >= map->m_la + map->m_llen) {
		erofs_dbg("out-of-range map @ pos %llu", offset + cur);

		if (z_erofs_collector_end(fe))
			fe->backmost = false;
		map->m_la = offset + cur;
		map->m_llen = 0;
		err = z_erofs_map_blocks_iter(inode, map, 0);
		if (err)
			goto err_out;
	} else {
		if (fe->pcl)
			goto hitted;
		/* didn't get a valid pcluster previously (very rare) */
	}

	if (!(map->m_flags & EROFS_MAP_MAPPED))
		goto hitted;

	err = z_erofs_collector_begin(fe, inode, map);
	if (err)
		goto err_out;

	if (z_erofs_is_inline_pcluster(fe->pcl)) {
		void *mp;

		mp = erofs_read_metabuf(&fe->map.buf, inode->i_sb,
					erofs_blknr(map->m_pa), EROFS_NO_KMAP);
		if (IS_ERR(mp)) {
			err = PTR_ERR(mp);
			erofs_err(inode->i_sb,
				  "failed to get inline page, err %d", err);
			goto err_out;
		}
		get_page(fe->map.buf.page);
		WRITE_ONCE(fe->pcl->compressed_pages[0], fe->map.buf.page);
		fe->mode = COLLECT_PRIMARY_FOLLOWED_NOINPLACE;
	} else {
		/* bind cache first when cached decompression is preferred */
		if (should_alloc_managed_pages(fe, sbi->opt.cache_strategy,
					       map->m_la))
			cache_strategy = TRYALLOC;
		else
			cache_strategy = DONTALLOC;

		z_erofs_bind_cache(fe, cache_strategy, pagepool);
	}
hitted:
	/*
	 * Ensure the current partial page belongs to this submit chain rather
	 * than other concurrent submit chains or the noio(bypass) chain since
	 * those chains are handled asynchronously thus the page cannot be used
	 * for inplace I/O or pagevec (should be processed in strict order.)
	 */
	tight &= (fe->mode >= COLLECT_PRIMARY_HOOKED &&
		  fe->mode != COLLECT_PRIMARY_FOLLOWED_NOINPLACE);

	cur = end - min_t(unsigned int, offset + end - map->m_la, end);
	if (!(map->m_flags & EROFS_MAP_MAPPED)) {
		zero_user_segment(page, cur, end);
		goto next_part;
	}

	/* let's derive page type */
	page_type = cur ? Z_EROFS_VLE_PAGE_TYPE_HEAD :
		(!spiltted ? Z_EROFS_PAGE_TYPE_EXCLUSIVE :
			(tight ? Z_EROFS_PAGE_TYPE_EXCLUSIVE :
				Z_EROFS_VLE_PAGE_TYPE_TAIL_SHARED));

	if (cur)
		tight &= (fe->mode >= COLLECT_PRIMARY_FOLLOWED);

retry:
	err = z_erofs_attach_page(fe, page, page_type,
				  fe->mode >= COLLECT_PRIMARY_FOLLOWED);
	/* should allocate an additional short-lived page for pagevec */
	if (err == -EAGAIN) {
		struct page *const newpage =
				alloc_page(GFP_NOFS | __GFP_NOFAIL);

		set_page_private(newpage, Z_EROFS_SHORTLIVED_PAGE);
		err = z_erofs_attach_page(fe, newpage,
					  Z_EROFS_PAGE_TYPE_EXCLUSIVE, true);
		if (!err)
			goto retry;
	}

	if (err)
		goto err_out;

	index = page->index - (map->m_la >> PAGE_SHIFT);

	z_erofs_onlinepage_fixup(page, index, true);

	/* bump up the number of spiltted parts of a page */
	++spiltted;
	/* also update nr_pages */
	fe->pcl->nr_pages = max_t(pgoff_t, fe->pcl->nr_pages, index + 1);
next_part:
	/* can be used for verification */
	map->m_llen = offset + cur - map->m_la;

	end = cur;
	if (end > 0)
		goto repeat;

out:
	z_erofs_onlinepage_endio(page);

	erofs_dbg("%s, finish page: %pK spiltted: %u map->m_llen %llu",
		  __func__, page, spiltted, map->m_llen);
	return err;

	/* if some error occurred while processing this page */
err_out:
	SetPageError(page);
	goto out;
}

static bool z_erofs_get_sync_decompress_policy(struct erofs_sb_info *sbi,
				       unsigned int readahead_pages)
{
	/* auto: enable for read_folio, disable for readahead */
	if ((sbi->opt.sync_decompress == EROFS_SYNC_DECOMPRESS_AUTO) &&
	    !readahead_pages)
		return true;

	if ((sbi->opt.sync_decompress == EROFS_SYNC_DECOMPRESS_FORCE_ON) &&
	    (readahead_pages <= sbi->opt.max_sync_decompress_pages))
		return true;

	return false;
}

static bool z_erofs_page_is_invalidated(struct page *page)
{
	return !page->mapping && !z_erofs_is_shortlived_page(page);
}

static int z_erofs_decompress_pcluster(struct super_block *sb,
				       struct z_erofs_pcluster *pcl,
				       struct page **pagepool)
{
	struct erofs_sb_info *const sbi = EROFS_SB(sb);
	unsigned int pclusterpages = z_erofs_pclusterpages(pcl);
	struct z_erofs_pagevec_ctor ctor;
	unsigned int i, inputsize, outputsize, llen, nr_pages;
	struct page *pages_onstack[Z_EROFS_VMAP_ONSTACK_PAGES];
	struct page **pages, **compressed_pages, *page;

	enum z_erofs_page_type page_type;
	bool overlapped, partial;
	int err;

	might_sleep();
	DBG_BUGON(!READ_ONCE(pcl->nr_pages));

	mutex_lock(&pcl->lock);
	nr_pages = pcl->nr_pages;

	if (nr_pages <= Z_EROFS_VMAP_ONSTACK_PAGES) {
		pages = pages_onstack;
	} else if (nr_pages <= Z_EROFS_VMAP_GLOBAL_PAGES &&
		   mutex_trylock(&z_pagemap_global_lock)) {
		pages = z_pagemap_global;
	} else {
		gfp_t gfp_flags = GFP_KERNEL;

		if (nr_pages > Z_EROFS_VMAP_GLOBAL_PAGES)
			gfp_flags |= __GFP_NOFAIL;

		pages = kvmalloc_array(nr_pages, sizeof(struct page *),
				       gfp_flags);

		/* fallback to global pagemap for the lowmem scenario */
		if (!pages) {
			mutex_lock(&z_pagemap_global_lock);
			pages = z_pagemap_global;
		}
	}

	for (i = 0; i < nr_pages; ++i)
		pages[i] = NULL;

	err = 0;
	z_erofs_pagevec_ctor_init(&ctor, Z_EROFS_NR_INLINE_PAGEVECS,
				  pcl->pagevec, 0);

	for (i = 0; i < pcl->vcnt; ++i) {
		unsigned int pagenr;

		page = z_erofs_pagevec_dequeue(&ctor, &page_type);

		/* all pages in pagevec ought to be valid */
		DBG_BUGON(!page);
		DBG_BUGON(z_erofs_page_is_invalidated(page));

		if (z_erofs_put_shortlivedpage(pagepool, page))
			continue;

		if (page_type == Z_EROFS_VLE_PAGE_TYPE_HEAD)
			pagenr = 0;
		else
			pagenr = z_erofs_onlinepage_index(page);

		DBG_BUGON(pagenr >= nr_pages);

		/*
		 * currently EROFS doesn't support multiref(dedup),
		 * so here erroring out one multiref page.
		 */
		if (pages[pagenr]) {
			DBG_BUGON(1);
			SetPageError(pages[pagenr]);
			z_erofs_onlinepage_endio(pages[pagenr]);
			err = -EFSCORRUPTED;
		}
		pages[pagenr] = page;
	}
	z_erofs_pagevec_ctor_exit(&ctor, true);

	overlapped = false;
	compressed_pages = pcl->compressed_pages;

	for (i = 0; i < pclusterpages; ++i) {
		unsigned int pagenr;

		page = compressed_pages[i];
		/* all compressed pages ought to be valid */
		DBG_BUGON(!page);

		if (z_erofs_is_inline_pcluster(pcl)) {
			if (!PageUptodate(page))
				err = -EIO;
			continue;
		}

		DBG_BUGON(z_erofs_page_is_invalidated(page));
		if (!z_erofs_is_shortlived_page(page)) {
			if (erofs_page_is_managed(sbi, page)) {
				if (!PageUptodate(page))
					err = -EIO;
				continue;
			}

			/*
			 * only if non-head page can be selected
			 * for inplace decompression
			 */
			pagenr = z_erofs_onlinepage_index(page);

			DBG_BUGON(pagenr >= nr_pages);
			if (pages[pagenr]) {
				DBG_BUGON(1);
				SetPageError(pages[pagenr]);
				z_erofs_onlinepage_endio(pages[pagenr]);
				err = -EFSCORRUPTED;
			}
			pages[pagenr] = page;

			overlapped = true;
		}

		/* PG_error needs checking for all non-managed pages */
		if (PageError(page)) {
			DBG_BUGON(PageUptodate(page));
			err = -EIO;
		}
	}

	if (err)
		goto out;

	llen = pcl->length >> Z_EROFS_PCLUSTER_LENGTH_BIT;
	if (nr_pages << PAGE_SHIFT >= pcl->pageofs_out + llen) {
		outputsize = llen;
		partial = !(pcl->length & Z_EROFS_PCLUSTER_FULL_LENGTH);
	} else {
		outputsize = (nr_pages << PAGE_SHIFT) - pcl->pageofs_out;
		partial = true;
	}

	if (z_erofs_is_inline_pcluster(pcl))
		inputsize = pcl->tailpacking_size;
	else
		inputsize = pclusterpages * PAGE_SIZE;

	err = z_erofs_decompress(&(struct z_erofs_decompress_req) {
					.sb = sb,
					.in = compressed_pages,
					.out = pages,
					.pageofs_in = pcl->pageofs_in,
					.pageofs_out = pcl->pageofs_out,
					.inputsize = inputsize,
					.outputsize = outputsize,
					.alg = pcl->algorithmformat,
					.inplace_io = overlapped,
					.partial_decoding = partial
				 }, pagepool);

out:
	/* must handle all compressed pages before actual file pages */
	if (z_erofs_is_inline_pcluster(pcl)) {
		page = compressed_pages[0];
		WRITE_ONCE(compressed_pages[0], NULL);
		put_page(page);
	} else {
		for (i = 0; i < pclusterpages; ++i) {
			page = compressed_pages[i];

			if (erofs_page_is_managed(sbi, page))
				continue;

			/* recycle all individual short-lived pages */
			(void)z_erofs_put_shortlivedpage(pagepool, page);
			WRITE_ONCE(compressed_pages[i], NULL);
		}
	}

	for (i = 0; i < nr_pages; ++i) {
		page = pages[i];
		if (!page)
			continue;

		DBG_BUGON(z_erofs_page_is_invalidated(page));

		/* recycle all individual short-lived pages */
		if (z_erofs_put_shortlivedpage(pagepool, page))
			continue;

		if (err < 0)
			SetPageError(page);

		z_erofs_onlinepage_endio(page);
	}

	if (pages == z_pagemap_global)
		mutex_unlock(&z_pagemap_global_lock);
	else if (pages != pages_onstack)
		kvfree(pages);

	pcl->nr_pages = 0;
	pcl->vcnt = 0;

	/* pcluster lock MUST be taken before the following line */
	WRITE_ONCE(pcl->next, Z_EROFS_PCLUSTER_NIL);
	mutex_unlock(&pcl->lock);
	return err;
}

static void z_erofs_decompress_queue(const struct z_erofs_decompressqueue *io,
				     struct page **pagepool)
{
	z_erofs_next_pcluster_t owned = io->head;

	while (owned != Z_EROFS_PCLUSTER_TAIL_CLOSED) {
		struct z_erofs_pcluster *pcl;

		/* no possible that 'owned' equals Z_EROFS_WORK_TPTR_TAIL */
		DBG_BUGON(owned == Z_EROFS_PCLUSTER_TAIL);

		/* no possible that 'owned' equals NULL */
		DBG_BUGON(owned == Z_EROFS_PCLUSTER_NIL);

		pcl = container_of(owned, struct z_erofs_pcluster, next);
		owned = READ_ONCE(pcl->next);

		z_erofs_decompress_pcluster(io->sb, pcl, pagepool);
		erofs_workgroup_put(&pcl->obj);
	}
}

static void z_erofs_decompressqueue_work(struct work_struct *work)
{
	struct z_erofs_decompressqueue *bgq =
		container_of(work, struct z_erofs_decompressqueue, u.work);
	struct page *pagepool = NULL;

	DBG_BUGON(bgq->head == Z_EROFS_PCLUSTER_TAIL_CLOSED);
	z_erofs_decompress_queue(bgq, &pagepool);

	erofs_release_pages(&pagepool);
	kvfree(bgq);
}

static void z_erofs_decompress_kickoff(struct z_erofs_decompressqueue *io,
				       bool sync, int bios)
{
	struct erofs_sb_info *const sbi = EROFS_SB(io->sb);

	/* wake up the caller thread for sync decompression */
	if (sync) {
		if (!atomic_add_return(bios, &io->pending_bios))
			complete(&io->u.done);

		return;
	}

	if (atomic_add_return(bios, &io->pending_bios))
		return;
	/* Use workqueue and sync decompression for atomic contexts only */
	if (in_atomic() || irqs_disabled()) {
		queue_work(z_erofs_workqueue, &io->u.work);
		/* enable sync decompression for readahead */
		if (sbi->opt.sync_decompress == EROFS_SYNC_DECOMPRESS_AUTO)
			sbi->opt.sync_decompress = EROFS_SYNC_DECOMPRESS_FORCE_ON;
		return;
	}
	z_erofs_decompressqueue_work(&io->u.work);
}

static struct page *pickup_page_for_submission(struct z_erofs_pcluster *pcl,
					       unsigned int nr,
					       struct page **pagepool,
					       struct address_space *mc)
{
	const pgoff_t index = pcl->obj.index;
	gfp_t gfp = mapping_gfp_mask(mc);
	bool tocache = false;

	struct address_space *mapping;
	struct page *oldpage, *page;

	compressed_page_t t;
	int justfound;

repeat:
	page = READ_ONCE(pcl->compressed_pages[nr]);
	oldpage = page;

	if (!page)
		goto out_allocpage;

	/* process the target tagged pointer */
	t = tagptr_init(compressed_page_t, page);
	justfound = tagptr_unfold_tags(t);
	page = tagptr_unfold_ptr(t);

	/*
	 * preallocated cached pages, which is used to avoid direct reclaim
	 * otherwise, it will go inplace I/O path instead.
	 */
	if (page->private == Z_EROFS_PREALLOCATED_PAGE) {
		WRITE_ONCE(pcl->compressed_pages[nr], page);
		set_page_private(page, 0);
		tocache = true;
		goto out_tocache;
	}
	mapping = READ_ONCE(page->mapping);

	/*
	 * file-backed online pages in plcuster are all locked steady,
	 * therefore it is impossible for `mapping' to be NULL.
	 */
	if (mapping && mapping != mc)
		/* ought to be unmanaged pages */
		goto out;

	/* directly return for shortlived page as well */
	if (z_erofs_is_shortlived_page(page))
		goto out;

	lock_page(page);

	/* only true if page reclaim goes wrong, should never happen */
	DBG_BUGON(justfound && PagePrivate(page));

	/* the page is still in manage cache */
	if (page->mapping == mc) {
		WRITE_ONCE(pcl->compressed_pages[nr], page);

		ClearPageError(page);
		if (!PagePrivate(page)) {
			/*
			 * impossible to be !PagePrivate(page) for
			 * the current restriction as well if
			 * the page is already in compressed_pages[].
			 */
			DBG_BUGON(!justfound);

			justfound = 0;
			set_page_private(page, (unsigned long)pcl);
			SetPagePrivate(page);
		}

		/* no need to submit io if it is already up-to-date */
		if (PageUptodate(page)) {
			unlock_page(page);
			page = NULL;
		}
		goto out;
	}

	/*
	 * the managed page has been truncated, it's unsafe to
	 * reuse this one, let's allocate a new cache-managed page.
	 */
	DBG_BUGON(page->mapping);
	DBG_BUGON(!justfound);

	tocache = true;
	unlock_page(page);
	put_page(page);
out_allocpage:
	page = erofs_allocpage(pagepool, gfp | __GFP_NOFAIL);
	if (oldpage != cmpxchg(&pcl->compressed_pages[nr], oldpage, page)) {
		erofs_pagepool_add(pagepool, page);
		cond_resched();
		goto repeat;
	}
out_tocache:
	if (!tocache || add_to_page_cache_lru(page, mc, index + nr, gfp)) {
		/* turn into temporary page if fails (1 ref) */
		set_page_private(page, Z_EROFS_SHORTLIVED_PAGE);
		goto out;
	}
	attach_page_private(page, pcl);
	/* drop a refcount added by allocpage (then we have 2 refs here) */
	put_page(page);

out:	/* the only exit (for tracing and debugging) */
	return page;
}

static struct z_erofs_decompressqueue *
jobqueue_init(struct super_block *sb,
	      struct z_erofs_decompressqueue *fgq, bool *fg)
{
	struct z_erofs_decompressqueue *q;

	if (fg && !*fg) {
		q = kvzalloc(sizeof(*q), GFP_KERNEL | __GFP_NOWARN);
		if (!q) {
			*fg = true;
			goto fg_out;
		}
		INIT_WORK(&q->u.work, z_erofs_decompressqueue_work);
	} else {
fg_out:
		q = fgq;
		init_completion(&fgq->u.done);
		atomic_set(&fgq->pending_bios, 0);
	}
	q->sb = sb;
	q->head = Z_EROFS_PCLUSTER_TAIL_CLOSED;
	return q;
}

/* define decompression jobqueue types */
enum {
	JQ_BYPASS,
	JQ_SUBMIT,
	NR_JOBQUEUES,
};

static void *jobqueueset_init(struct super_block *sb,
			      struct z_erofs_decompressqueue *q[],
			      struct z_erofs_decompressqueue *fgq, bool *fg)
{
	/*
	 * if managed cache is enabled, bypass jobqueue is needed,
	 * no need to read from device for all pclusters in this queue.
	 */
	q[JQ_BYPASS] = jobqueue_init(sb, fgq + JQ_BYPASS, NULL);
	q[JQ_SUBMIT] = jobqueue_init(sb, fgq + JQ_SUBMIT, fg);

	return tagptr_cast_ptr(tagptr_fold(tagptr1_t, q[JQ_SUBMIT], *fg));
}

static void move_to_bypass_jobqueue(struct z_erofs_pcluster *pcl,
				    z_erofs_next_pcluster_t qtail[],
				    z_erofs_next_pcluster_t owned_head)
{
	z_erofs_next_pcluster_t *const submit_qtail = qtail[JQ_SUBMIT];
	z_erofs_next_pcluster_t *const bypass_qtail = qtail[JQ_BYPASS];

	DBG_BUGON(owned_head == Z_EROFS_PCLUSTER_TAIL_CLOSED);
	if (owned_head == Z_EROFS_PCLUSTER_TAIL)
		owned_head = Z_EROFS_PCLUSTER_TAIL_CLOSED;

	WRITE_ONCE(pcl->next, Z_EROFS_PCLUSTER_TAIL_CLOSED);

	WRITE_ONCE(*submit_qtail, owned_head);
	WRITE_ONCE(*bypass_qtail, &pcl->next);

	qtail[JQ_BYPASS] = &pcl->next;
}

static void z_erofs_decompressqueue_endio(struct bio *bio)
{
	tagptr1_t t = tagptr_init(tagptr1_t, bio->bi_private);
	struct z_erofs_decompressqueue *q = tagptr_unfold_ptr(t);
	blk_status_t err = bio->bi_status;
	struct bio_vec *bvec;
	struct bvec_iter_all iter_all;

	bio_for_each_segment_all(bvec, bio, iter_all) {
		struct page *page = bvec->bv_page;

		DBG_BUGON(PageUptodate(page));
		DBG_BUGON(z_erofs_page_is_invalidated(page));

		if (err)
			SetPageError(page);

		if (erofs_page_is_managed(EROFS_SB(q->sb), page)) {
			if (!err)
				SetPageUptodate(page);
			unlock_page(page);
		}
	}
	z_erofs_decompress_kickoff(q, tagptr_unfold_tags(t), -1);
	bio_put(bio);
}

static void z_erofs_submit_queue(struct super_block *sb,
				 struct z_erofs_decompress_frontend *f,
				 struct page **pagepool,
				 struct z_erofs_decompressqueue *fgq,
				 bool *force_fg)
{
	struct erofs_sb_info *const sbi = EROFS_SB(sb);
	z_erofs_next_pcluster_t qtail[NR_JOBQUEUES];
	struct z_erofs_decompressqueue *q[NR_JOBQUEUES];
	void *bi_private;
	z_erofs_next_pcluster_t owned_head = f->owned_head;
	/* bio is NULL initially, so no need to initialize last_{index,bdev} */
	pgoff_t last_index;
	struct block_device *last_bdev;
	unsigned int nr_bios = 0;
	struct bio *bio = NULL;

	bi_private = jobqueueset_init(sb, q, fgq, force_fg);
	qtail[JQ_BYPASS] = &q[JQ_BYPASS]->head;
	qtail[JQ_SUBMIT] = &q[JQ_SUBMIT]->head;

	/* by default, all need io submission */
	q[JQ_SUBMIT]->head = owned_head;

	do {
		struct erofs_map_dev mdev;
		struct z_erofs_pcluster *pcl;
		pgoff_t cur, end;
		unsigned int i = 0;
		bool bypass = true;

		/* no possible 'owned_head' equals the following */
		DBG_BUGON(owned_head == Z_EROFS_PCLUSTER_TAIL_CLOSED);
		DBG_BUGON(owned_head == Z_EROFS_PCLUSTER_NIL);

		pcl = container_of(owned_head, struct z_erofs_pcluster, next);

		/* close the main owned chain at first */
		owned_head = cmpxchg(&pcl->next, Z_EROFS_PCLUSTER_TAIL,
				     Z_EROFS_PCLUSTER_TAIL_CLOSED);
		if (z_erofs_is_inline_pcluster(pcl)) {
			move_to_bypass_jobqueue(pcl, qtail, owned_head);
			continue;
		}

		/* no device id here, thus it will always succeed */
		mdev = (struct erofs_map_dev) {
			.m_pa = blknr_to_addr(pcl->obj.index),
		};
		(void)erofs_map_dev(sb, &mdev);

		cur = erofs_blknr(mdev.m_pa);
		end = cur + pcl->pclusterpages;

		do {
			struct page *page;

			page = pickup_page_for_submission(pcl, i++, pagepool,
							  MNGD_MAPPING(sbi));
			if (!page)
				continue;

			if (bio && (cur != last_index + 1 ||
				    last_bdev != mdev.m_bdev)) {
submit_bio_retry:
				submit_bio(bio);
				bio = NULL;
			}

			if (!bio) {
				bio = bio_alloc(mdev.m_bdev, BIO_MAX_VECS,
						REQ_OP_READ, GFP_NOIO);
				bio->bi_end_io = z_erofs_decompressqueue_endio;

				last_bdev = mdev.m_bdev;
				bio->bi_iter.bi_sector = (sector_t)cur <<
					LOG_SECTORS_PER_BLOCK;
				bio->bi_private = bi_private;
				if (f->readahead)
					bio->bi_opf |= REQ_RAHEAD;
				++nr_bios;
			}

			if (bio_add_page(bio, page, PAGE_SIZE, 0) < PAGE_SIZE)
				goto submit_bio_retry;

			last_index = cur;
			bypass = false;
		} while (++cur < end);

		if (!bypass)
			qtail[JQ_SUBMIT] = &pcl->next;
		else
			move_to_bypass_jobqueue(pcl, qtail, owned_head);
	} while (owned_head != Z_EROFS_PCLUSTER_TAIL);

	if (bio)
		submit_bio(bio);

	/*
	 * although background is preferred, no one is pending for submission.
	 * don't issue workqueue for decompression but drop it directly instead.
	 */
	if (!*force_fg && !nr_bios) {
		kvfree(q[JQ_SUBMIT]);
		return;
	}
	z_erofs_decompress_kickoff(q[JQ_SUBMIT], *force_fg, nr_bios);
}

static void z_erofs_runqueue(struct super_block *sb,
			     struct z_erofs_decompress_frontend *f,
			     struct page **pagepool, bool force_fg)
{
	struct z_erofs_decompressqueue io[NR_JOBQUEUES];

	if (f->owned_head == Z_EROFS_PCLUSTER_TAIL)
		return;
	z_erofs_submit_queue(sb, f, pagepool, io, &force_fg);

	/* handle bypass queue (no i/o pclusters) immediately */
	z_erofs_decompress_queue(&io[JQ_BYPASS], pagepool);

	if (!force_fg)
		return;

	/* wait until all bios are completed */
	wait_for_completion_io(&io[JQ_SUBMIT].u.done);

	/* handle synchronous decompress queue in the caller context */
	z_erofs_decompress_queue(&io[JQ_SUBMIT], pagepool);
}

/*
 * Since partial uptodate is still unimplemented for now, we have to use
 * approximate readmore strategies as a start.
 */
static void z_erofs_pcluster_readmore(struct z_erofs_decompress_frontend *f,
				      struct readahead_control *rac,
				      erofs_off_t end,
				      struct page **pagepool,
				      bool backmost)
{
	struct inode *inode = f->inode;
	struct erofs_map_blocks *map = &f->map;
	erofs_off_t cur;
	int err;

	if (backmost) {
		map->m_la = end;
		err = z_erofs_map_blocks_iter(inode, map,
					      EROFS_GET_BLOCKS_READMORE);
		if (err)
			return;

		/* expend ra for the trailing edge if readahead */
		if (rac) {
			loff_t newstart = readahead_pos(rac);

			cur = round_up(map->m_la + map->m_llen, PAGE_SIZE);
			readahead_expand(rac, newstart, cur - newstart);
			return;
		}
		end = round_up(end, PAGE_SIZE);
	} else {
		end = round_up(map->m_la, PAGE_SIZE);

		if (!map->m_llen)
			return;
	}

	cur = map->m_la + map->m_llen - 1;
	while (cur >= end) {
		pgoff_t index = cur >> PAGE_SHIFT;
		struct page *page;

		page = erofs_grab_cache_page_nowait(inode->i_mapping, index);
		if (page) {
			if (PageUptodate(page)) {
				unlock_page(page);
			} else {
				err = z_erofs_do_read_page(f, page, pagepool);
				if (err)
					erofs_err(inode->i_sb,
						  "readmore error at page %lu @ nid %llu",
						  index, EROFS_I(inode)->nid);
			}
			put_page(page);
		}

		if (cur < PAGE_SIZE)
			break;
		cur = (index << PAGE_SHIFT) - 1;
	}
}

static int z_erofs_read_folio(struct file *file, struct folio *folio)
{
	struct page *page = &folio->page;
	struct inode *const inode = page->mapping->host;
	struct erofs_sb_info *const sbi = EROFS_I_SB(inode);
	struct z_erofs_decompress_frontend f = DECOMPRESS_FRONTEND_INIT(inode);
	struct page *pagepool = NULL;
	int err;

	trace_erofs_readpage(page, false);
	f.headoffset = (erofs_off_t)page->index << PAGE_SHIFT;

	z_erofs_pcluster_readmore(&f, NULL, f.headoffset + PAGE_SIZE - 1,
				  &pagepool, true);
	err = z_erofs_do_read_page(&f, page, &pagepool);
	z_erofs_pcluster_readmore(&f, NULL, 0, &pagepool, false);

	(void)z_erofs_collector_end(&f);

	/* if some compressed cluster ready, need submit them anyway */
	z_erofs_runqueue(inode->i_sb, &f, &pagepool,
			 z_erofs_get_sync_decompress_policy(sbi, 0));

	if (err)
		erofs_err(inode->i_sb, "failed to read, err [%d]", err);

	erofs_put_metabuf(&f.map.buf);
	erofs_release_pages(&pagepool);
	return err;
}

static void z_erofs_readahead(struct readahead_control *rac)
{
	struct inode *const inode = rac->mapping->host;
	struct erofs_sb_info *const sbi = EROFS_I_SB(inode);
	struct z_erofs_decompress_frontend f = DECOMPRESS_FRONTEND_INIT(inode);
	struct page *pagepool = NULL, *head = NULL, *page;
	unsigned int nr_pages;

	f.readahead = true;
	f.headoffset = readahead_pos(rac);

	z_erofs_pcluster_readmore(&f, rac, f.headoffset +
				  readahead_length(rac) - 1, &pagepool, true);
	nr_pages = readahead_count(rac);
	trace_erofs_readpages(inode, readahead_index(rac), nr_pages, false);

	while ((page = readahead_page(rac))) {
		set_page_private(page, (unsigned long)head);
		head = page;
	}

	while (head) {
		struct page *page = head;
		int err;

		/* traversal in reverse order */
		head = (void *)page_private(page);

		err = z_erofs_do_read_page(&f, page, &pagepool);
		if (err)
			erofs_err(inode->i_sb,
				  "readahead error at page %lu @ nid %llu",
				  page->index, EROFS_I(inode)->nid);
		put_page(page);
	}
	z_erofs_pcluster_readmore(&f, rac, 0, &pagepool, false);
	(void)z_erofs_collector_end(&f);

	z_erofs_runqueue(inode->i_sb, &f, &pagepool,
			 z_erofs_get_sync_decompress_policy(sbi, nr_pages));
	erofs_put_metabuf(&f.map.buf);
	erofs_release_pages(&pagepool);
}

const struct address_space_operations z_erofs_aops = {
	.read_folio = z_erofs_read_folio,
	.readahead = z_erofs_readahead,
};