// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Western Digital Corporation or its affiliates.
 */

#include <linux/btrfs_tree.h>
#include <linux/raid/xor.h>
#include <linux/minmax.h>
#include "ctree.h"
#include "fs.h"
#include "accessors.h"
#include "transaction.h"
#include "disk-io.h"
#include "raid-stripe-tree.h"
#include "volumes.h"
#include "print-tree.h"
#include "zoned.h"

static int btrfs_partially_delete_raid_extent(struct btrfs_trans_handle *trans,
					       struct btrfs_path *path,
					       const struct btrfs_key *oldkey,
					       u64 newlen, u64 frontpad)
{
	struct btrfs_root *stripe_root = trans->fs_info->stripe_root;
	struct btrfs_stripe_extent *extent, *newitem;
	struct extent_buffer *leaf;
	int slot;
	size_t item_size;
	struct btrfs_key newkey = {
		.objectid = oldkey->objectid + frontpad,
		.type = BTRFS_RAID_STRIPE_KEY,
		.offset = newlen,
	};
	int ret;

	ASSERT(newlen > 0);
	ASSERT(oldkey->type == BTRFS_RAID_STRIPE_KEY);

	leaf = path->nodes[0];
	slot = path->slots[0];
	item_size = btrfs_item_size(leaf, slot);

	newitem = kzalloc(item_size, GFP_NOFS);
	if (!newitem)
		return -ENOMEM;

	extent = btrfs_item_ptr(leaf, slot, struct btrfs_stripe_extent);

	for (int i = 0; i < btrfs_num_raid_stripes(item_size); i++) {
		struct btrfs_raid_stride *stride = &extent->strides[i];
		u64 phys;

		phys = btrfs_raid_stride_physical(leaf, stride) + frontpad;
		btrfs_set_stack_raid_stride_physical(&newitem->strides[i], phys);
	}

	ret = btrfs_del_item(trans, stripe_root, path);
	if (ret)
		goto out;

	btrfs_release_path(path);
	ret = btrfs_insert_item(trans, stripe_root, &newkey, newitem, item_size);

out:
	kfree(newitem);
	return ret;
}

int btrfs_delete_raid_extent(struct btrfs_trans_handle *trans, u64 start, u64 length)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *stripe_root = fs_info->stripe_root;
	struct btrfs_path *path;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	u64 found_start;
	u64 found_end;
	u64 end = start + length;
	int slot;
	int ret;

	if (!btrfs_fs_incompat(fs_info, RAID_STRIPE_TREE) || !stripe_root)
		return 0;

	if (!btrfs_is_testing(fs_info)) {
		struct btrfs_chunk_map *map;
		bool use_rst;

		map = btrfs_find_chunk_map(fs_info, start, length);
		if (!map)
			return -EINVAL;
		use_rst = btrfs_need_stripe_tree_update(fs_info, map->type);
		btrfs_free_chunk_map(map);
		if (!use_rst)
			return 0;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	while (1) {
		key.objectid = start;
		key.type = BTRFS_RAID_STRIPE_KEY;
		key.offset = 0;

		ret = btrfs_search_slot(trans, stripe_root, &key, path, -1, 1);
		if (ret < 0)
			break;

		if (path->slots[0] == btrfs_header_nritems(path->nodes[0]))
			path->slots[0]--;

		leaf = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);
		found_start = key.objectid;
		found_end = found_start + key.offset;
		ret = 0;

		/*
		 * The stripe extent starts before the range we want to delete,
		 * but the range spans more than one stripe extent:
		 *
		 * |--- RAID Stripe Extent ---||--- RAID Stripe Extent ---|
		 *        |--- keep  ---|--- drop ---|
		 *
		 * This means we have to get the previous item, truncate its
		 * length and then restart the search.
		 */
		if (found_start > start) {
			if (slot == 0) {
				ret = btrfs_previous_item(stripe_root, path, start,
							  BTRFS_RAID_STRIPE_KEY);
				if (ret) {
					if (ret > 0)
						ret = -ENOENT;
					break;
				}
			} else {
				path->slots[0]--;
			}

			leaf = path->nodes[0];
			slot = path->slots[0];
			btrfs_item_key_to_cpu(leaf, &key, slot);
			found_start = key.objectid;
			found_end = found_start + key.offset;
			ASSERT(found_start <= start);
		}

		if (key.type != BTRFS_RAID_STRIPE_KEY)
			break;

		/* That stripe ends before we start, we're done. */
		if (found_end <= start)
			break;

		trace_btrfs_raid_extent_delete(fs_info, start, end,
					       found_start, found_end);

		/*
		 * The stripe extent starts before the range we want to delete
		 * and ends after the range we want to delete, i.e. we're
		 * punching a hole in the stripe extent:
		 *
		 *  |--- RAID Stripe Extent ---|
		 *  | keep |--- drop ---| keep |
		 *
		 * This means we need to a) truncate the existing item and b)
		 * create a second item for the remaining range.
		 */
		if (found_start < start && found_end > end) {
			size_t item_size;
			u64 diff_start = start - found_start;
			u64 diff_end = found_end - end;
			struct btrfs_stripe_extent *extent;
			struct btrfs_key newkey = {
				.objectid = end,
				.type = BTRFS_RAID_STRIPE_KEY,
				.offset = diff_end,
			};

			/* The "right" item. */
			ret = btrfs_duplicate_item(trans, stripe_root, path, &newkey);
			if (ret)
				break;

			item_size = btrfs_item_size(leaf, path->slots[0]);
			extent = btrfs_item_ptr(leaf, path->slots[0],
						struct btrfs_stripe_extent);

			for (int i = 0; i < btrfs_num_raid_stripes(item_size); i++) {
				struct btrfs_raid_stride *stride = &extent->strides[i];
				u64 phys;

				phys = btrfs_raid_stride_physical(leaf, stride);
				phys += diff_start + length;
				btrfs_set_raid_stride_physical(leaf, stride, phys);
			}

			/* The "left" item. */
			path->slots[0]--;
			btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
			btrfs_partially_delete_raid_extent(trans, path, &key,
							   diff_start, 0);
			break;
		}

		/*
		 * The stripe extent starts before the range we want to delete:
		 *
		 * |--- RAID Stripe Extent ---|
		 * |--- keep  ---|--- drop ---|
		 *
		 * This means we have to duplicate the tree item, truncate the
		 * length to the new size and then re-insert the item.
		 */
		if (found_start < start) {
			u64 diff_start = start - found_start;

			btrfs_partially_delete_raid_extent(trans, path, &key,
							   diff_start, 0);

			start += (key.offset - diff_start);
			length -= (key.offset - diff_start);
			if (length == 0)
				break;

			btrfs_release_path(path);
			continue;
		}

		/*
		 * The stripe extent ends after the range we want to delete:
		 *
		 * |--- RAID Stripe Extent ---|
		 * |--- drop  ---|--- keep ---|
		 *
		 * This means we have to duplicate the tree item, truncate the
		 * length to the new size and then re-insert the item.
		 */
		if (found_end > end) {
			u64 diff_end = found_end - end;

			btrfs_partially_delete_raid_extent(trans, path, &key,
							   key.offset - length,
							   length);
			ASSERT(key.offset - diff_end == length);
			break;
		}

		/* Finally we can delete the whole item, no more special cases. */
		ret = btrfs_del_item(trans, stripe_root, path);
		if (ret)
			break;

		start += key.offset;
		length -= key.offset;
		if (length == 0)
			break;

		btrfs_release_path(path);
	}

	btrfs_free_path(path);
	return ret;
}

static int update_raid_extent_item(struct btrfs_trans_handle *trans,
				   struct btrfs_key *key,
				   struct btrfs_stripe_extent *stripe_extent,
				   const size_t item_size)
{
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	int ret;
	int slot;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_search_slot(trans, trans->fs_info->stripe_root, key, path,
				0, 1);
	if (ret)
		return (ret == 1 ? ret : -EINVAL);

	leaf = path->nodes[0];
	slot = path->slots[0];

	write_extent_buffer(leaf, stripe_extent, btrfs_item_ptr_offset(leaf, slot),
			    item_size);
	btrfs_free_path(path);

	return ret;
}

static void fill_raid_stride(struct btrfs_io_stripe *stripe,
			     struct btrfs_raid_stride *raid_stride)
{
	u64 devid = stripe->dev->devid;
	u64 physical = stripe->physical;

	btrfs_set_stack_raid_stride_devid(raid_stride, devid);
	btrfs_set_stack_raid_stride_physical(raid_stride, physical);
}

EXPORT_FOR_TESTS
int btrfs_insert_one_raid_extent(struct btrfs_trans_handle *trans,
				 struct btrfs_io_context *bioc)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_key stripe_key;
	struct btrfs_root *stripe_root = fs_info->stripe_root;
	const int num_stripes = btrfs_bg_type_to_factor(bioc->map_type);
	struct btrfs_stripe_extent *stripe_extent;
	const size_t item_size = struct_size(stripe_extent, strides, num_stripes);
	int ret;

	stripe_extent = kzalloc(item_size, GFP_NOFS);
	if (!stripe_extent) {
		btrfs_abort_transaction(trans, -ENOMEM);
		btrfs_end_transaction(trans);
		return -ENOMEM;
	}

	trace_btrfs_insert_one_raid_extent(fs_info, bioc->logical, bioc->size,
					   num_stripes);

	if (bioc->map_type & BTRFS_BLOCK_GROUP_RAID56_MASK) {
		int stripe_nr = btrfs_bioc_to_stripe_nr(bioc);
		struct btrfs_raid_stride *raid_stride = &stripe_extent->strides[0];

		fill_raid_stride(&bioc->stripes[stripe_nr], raid_stride);
	} else {
		for (int i = 0; i < num_stripes; i++) {
			struct btrfs_io_stripe *stripe = &bioc->stripes[i];
			struct btrfs_raid_stride *raid_stride =
				&stripe_extent->strides[i];

			fill_raid_stride(stripe, raid_stride);
		}
	}

	stripe_key.objectid = bioc->logical;
	stripe_key.type = BTRFS_RAID_STRIPE_KEY;
	stripe_key.offset = bioc->size;

	ret = btrfs_insert_item(trans, stripe_root, &stripe_key, stripe_extent,
				item_size);
	if (ret == -EEXIST) {
		ret = update_raid_extent_item(trans, &stripe_key, stripe_extent,
					      item_size);
		if (ret)
			btrfs_abort_transaction(trans, ret);
	} else if (ret) {
		btrfs_abort_transaction(trans, ret);
	}

	kfree(stripe_extent);

	return ret;
}

int btrfs_insert_raid_extent(struct btrfs_trans_handle *trans,
			     struct btrfs_ordered_extent *ordered_extent)
{
	struct btrfs_io_context *bioc;
	int ret;

	if (!btrfs_fs_incompat(trans->fs_info, RAID_STRIPE_TREE))
		return 0;

	list_for_each_entry(bioc, &ordered_extent->bioc_list, rst_ordered_entry) {
		ret = btrfs_insert_one_raid_extent(trans, bioc);
		if (ret)
			return ret;
	}

	while (!list_empty(&ordered_extent->bioc_list)) {
		bioc = list_first_entry(&ordered_extent->bioc_list,
					typeof(*bioc), rst_ordered_entry);
		list_del(&bioc->rst_ordered_entry);
		btrfs_put_bioc(bioc);
	}

	return 0;
}

int btrfs_get_raid_extent_offset(struct btrfs_fs_info *fs_info,
				 u64 logical, u64 *length, u64 map_type,
				 u32 stripe_index, struct btrfs_io_stripe *stripe)
{
	struct btrfs_root *stripe_root = fs_info->stripe_root;
	struct btrfs_stripe_extent *stripe_extent;
	struct btrfs_key stripe_key;
	struct btrfs_key found_key;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	const u64 end = logical + *length;
	int type = BTRFS_RAID_STRIPE_KEY;
	int num_stripes;
	u64 offset;
	u64 found_logical;
	u64 found_length;
	u64 found_end;
	int slot;
	int ret;

	stripe_key.objectid = logical;
	stripe_key.type = type;
	stripe_key.offset = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	if (stripe->rst_search_commit_root) {
		path->skip_locking = 1;
		path->search_commit_root = 1;
	}

	ret = btrfs_search_slot(NULL, stripe_root, &stripe_key, path, 0, 0);
	if (ret < 0)
		goto free_path;
	if (ret) {
		if (path->slots[0] != 0)
			path->slots[0]--;

		btrfs_item_key_to_cpu(path->nodes[0], &found_key, path->slots[0]);
		if (found_key.type != type && path->slots[0] > 0)
			path->slots[0]--;
	}

	while (1) {
		leaf = path->nodes[0];
		slot = path->slots[0];

		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		found_logical = found_key.objectid;
		found_length = found_key.offset;
		found_end = found_logical + found_length;

		if (found_logical > end) {
			ret = -ENODATA;
			goto out;
		}

		if (in_range(logical, found_logical, found_length) &&
		    found_key.type == type)
			break;

		ret = btrfs_next_item(stripe_root, path);
		if (ret)
			goto out;
	}

	offset = logical - found_logical;

	/*
	 * If we have a logically contiguous, but physically non-continuous
	 * range, we need to split the bio. Record the length after which we
	 * must split the bio.
	 */
	if (end > found_end)
		*length -= end - found_end;

	num_stripes = btrfs_num_raid_stripes(btrfs_item_size(leaf, slot));
	stripe_extent = btrfs_item_ptr(leaf, slot, struct btrfs_stripe_extent);

	for (int i = 0; i < num_stripes; i++) {
		struct btrfs_raid_stride *stride = &stripe_extent->strides[i];
		u64 devid = btrfs_raid_stride_devid(leaf, stride);
		u64 physical = btrfs_raid_stride_physical(leaf, stride);

		if (devid != stripe->dev->devid)
			continue;

		if ((map_type & BTRFS_BLOCK_GROUP_DUP) && stripe_index != i)
			continue;

		stripe->physical = physical + offset;

		trace_btrfs_get_raid_extent_offset(fs_info, logical, *length,
						   stripe->physical, devid);

		ret = 0;
		goto free_path;
	}

	/* If we're here, we haven't found the requested devid in the stripe. */
	ret = -ENODATA;
out:
	if (ret > 0)
		ret = -ENODATA;
	if (ret && ret != -EIO && !stripe->rst_search_commit_root) {
		btrfs_debug(fs_info,
		"cannot find raid-stripe for logical [%llu, %llu] devid %llu, profile %s",
			  logical, logical + *length, stripe->dev->devid,
			  btrfs_bg_type_to_raid_name(map_type));
	}
free_path:
	btrfs_free_path(path);

	return ret;
}

struct btrfs_stripe_set {
	struct btrfs_fs_info *fs_info;
	struct work_struct work;
	struct list_head list;
	u64 full_stripe_logical;
	u64 full_stripe_len;
	u64 logical;
	u64 len;
	refcount_t refs;
	atomic_t pending_ios;
	unsigned int npar;
	struct btrfs_io_stripe *pstripes;
};

static void put_btrfs_stripe_set(struct btrfs_fs_info *fs_info,
				 struct btrfs_stripe_set *set)
{
	if (refcount_dec_and_test(&set->refs))
		kfree(set);
}

static void insert_parity_stripe_work(struct work_struct *work)
{
	struct btrfs_stripe_set *set =
		container_of(work, struct btrfs_stripe_set, work);
	struct btrfs_fs_info *fs_info = set->fs_info;
	struct btrfs_trans_handle *trans;
	struct btrfs_key stripe_key;
	struct btrfs_root *stripe_root = fs_info->stripe_root;
	struct btrfs_stripe_extent *stripe_extent;
	const size_t item_size = struct_size(stripe_extent, strides, set->npar);
	int ret;

	printk(KERN_ERR "%s: called\n", __func__);
	trans = btrfs_join_transaction(stripe_root);
	if (IS_ERR(trans))
		return;

	stripe_extent = kzalloc(item_size, GFP_NOFS);
	if (!stripe_extent) {
		btrfs_abort_transaction(trans, -ENOMEM);
		btrfs_end_transaction(trans);
		return;
	}

	trace_btrfs_insert_parity_stripe(fs_info, set->logical, set->len, set->npar);

	for (int i = 0; i < set->npar; i++) {
		u64 devid = set->pstripes[i].dev->devid;
		u64 physical = set->pstripes[i].physical;
		struct btrfs_raid_stride *raid_stride = &stripe_extent->strides[i];

		btrfs_set_stack_raid_stride_devid(raid_stride, devid);
		btrfs_set_stack_raid_stride_physical(raid_stride, physical);
	}

	stripe_key.objectid = set->logical;
	stripe_key.type = BTRFS_RAID_STRIPE_PARITY_KEY;
	stripe_key.offset = set->len;

	ret = btrfs_insert_item(trans, stripe_root, &stripe_key, stripe_extent,
				item_size);
	if (ret == -EEXIST)
		ret = update_raid_extent_item(trans, &stripe_key, stripe_extent,
					      item_size);
	if (ret)
		btrfs_abort_transaction(trans, ret);

	btrfs_end_transaction(trans);
	kfree(stripe_extent);
	put_btrfs_stripe_set(fs_info, set);
}

static void raid56_write_endio(struct btrfs_bio *bbio)
{
	struct btrfs_stripe_set *set = bbio->private;
	struct btrfs_fs_info *fs_info = bbio->fs_info;

	if (atomic_dec_and_test(&set->pending_ios))
		queue_work(fs_info->endio_workers, &set->work);

	put_btrfs_stripe_set(fs_info, set);
}

static void btrfs_simple_end_io(struct bio *bio)
{
	struct btrfs_bio *bbio = btrfs_bio(bio);

	btrfs_bio_end_io(bbio, bbio->bio.bi_status);
}

static struct btrfs_stripe_set *btrfs_alloc_stripe_set(struct btrfs_io_context *bioc,
						       u64 len)
{
	struct btrfs_fs_info *fs_info = bioc->fs_info;
	const unsigned int nr_parity = btrfs_nr_parity_stripes(bioc->map_type);
	const unsigned int nr_data = bioc->num_stripes - nr_parity;
	struct btrfs_stripe_set *set;

	set = kzalloc(sizeof(struct btrfs_stripe_set), GFP_NOFS | __GFP_NOFAIL);
	if (!set)
		return ERR_PTR(-ENOMEM);

	refcount_set(&set->refs, 1);
	atomic_set(&set->pending_ios, 0);
	INIT_LIST_HEAD(&set->list);
	INIT_WORK(&set->work, insert_parity_stripe_work);
	set->fs_info = fs_info;
	set->full_stripe_logical = bioc->full_stripe_logical;
	set->full_stripe_len = nr_data * BTRFS_STRIPE_LEN;
	set->logical = bioc->logical;
	set->len = len;
	set->npar = nr_parity;
	set->pstripes = bioc->stripes + nr_data;

	spin_lock(&fs_info->stripe_set_lock);
	list_add_tail(&set->list, &fs_info->stripe_sets);
	spin_unlock(&fs_info->stripe_set_lock);
	return set;
}

static struct btrfs_stripe_set *btrfs_find_stripe_set(struct btrfs_io_context *bioc, u64 len)
{
	struct btrfs_fs_info *fs_info = bioc->fs_info;
	struct btrfs_stripe_set *set;
	bool found = false;

	spin_lock(&fs_info->stripe_set_lock);
	list_for_each_entry(set, &fs_info->stripe_sets, list) {
		if (in_range(bioc->logical, set->full_stripe_logical,
			     set->full_stripe_len)) {
			found = true;
			break;
		}
	}
	spin_unlock(&fs_info->stripe_set_lock);

	if (!found)
		return 0;

	set->len += len;
	return set;
}

struct btrfs_rst_plug {
	struct blk_plug_cb cb;
	struct bio_list bios;
	struct work_struct work;
	unsigned int size;
	u32 stripe_offset;
};

struct btrfs_raid_write_ctx {
	struct btrfs_fs_info *fs_info;
	struct btrfs_io_context *bioc;
	struct bio *bio;
	struct bio *next;
	u32 stripe_bytes;
	u32 total_size;
};

static u32 btrfs_stripe_bytes(struct btrfs_io_context *bioc)
{
	u64 nr_stripes;

	nr_stripes = bioc->num_stripes - btrfs_nr_parity_stripes(bioc->map_type);
	return nr_stripes * BTRFS_STRIPE_LEN;
}

static void btrfs_rst_raid56_write_partial_stripe(
					  struct btrfs_raid_write_ctx *ctx,
					  u32 stripe_offset)
{
	struct btrfs_stripe_set *set;
	u32 len = ctx->bio->bi_iter.bi_size;

	printk(KERN_ERR "%s: called, stripe_offset=%u\n", __func__, stripe_offset);

	set = btrfs_find_stripe_set(ctx->bioc, len);
	if (!set)
		set = btrfs_alloc_stripe_set(ctx->bioc, len);

#if 0
	/* stripe set is full, calculate parity */
	if (set->len == ctx->total_size) {
		calculate_parity(set);
	} else {
		bio = bio_clone(ctx->bio);
		bio_list_add_head_or_tail(set->bios, bio);
	}
#endif
	btrfs_submit_raid56_write(ctx->bio, ctx->bioc);
}

static void btrfs_rst_raid56_write_full_stripe(struct btrfs_raid_write_ctx *ctx)
{
	struct btrfs_fs_info *fs_info = ctx->fs_info;
	struct btrfs_stripe_set *set;
	struct btrfs_bio *bbio;
	struct bio *pbio;
	struct btrfs_io_stripe *pstripe;
	struct bio_vec bvec;
	struct bvec_iter iter;
	unsigned int map_pages = ctx->total_size >> PAGE_SHIFT;
	struct folio *folio;
	blk_opf_t op;
	u64 physical;
	u32 sectorsize = fs_info->sectorsize;
	u8 *parity;
	int i = 0;

	printk(KERN_ERR "%s: called\n", __func__);

	set = btrfs_alloc_stripe_set(ctx->bioc, ctx->total_size);

	if (btrfs_use_zone_append(btrfs_bio(ctx->bio)))
		op = REQ_OP_ZONE_APPEND;
	else
		op = REQ_OP_WRITE;

	folio = folio_alloc(GFP_NOFS, get_order(ctx->total_size));
	if (!folio) {
		put_btrfs_stripe_set(fs_info, set);
		ctx->bio->bi_status = BLK_STS_RESOURCE;
		bio_endio(ctx->bio);
		return;
	}

	parity = folio_address(folio);

	bio_for_each_segment(bvec, ctx->bio, iter) {
		unsigned int poff = i * sectorsize;
		u64 end = bvec.bv_len + sectorsize - 1;
		void *src = bvec_kmap_local(&bvec);
		unsigned int blocks;

		blocks = BTRFS_BYTES_TO_BLKS(fs_info, end);
		xor_blocks(blocks, sectorsize, parity + poff, &src);
		kunmap_local(src);
		i++;
	}

	bbio = btrfs_bio_alloc(map_pages, op, fs_info, raid56_write_endio, set);
	pbio = &bbio->bio;
	bio_add_folio_nofail(pbio, folio, folio_size(folio), 0);

	pstripe = &set->pstripes[0];
	physical = pstripe->physical;

	bio_set_dev(pbio, pstripe->dev->bdev);

	pbio->bi_iter.bi_sector = physical >> SECTOR_SHIFT;
	pbio->bi_end_io = btrfs_simple_end_io;
	if (op == REQ_OP_ZONE_APPEND)
		physical = round_down(physical, fs_info->zone_size);


	refcount_inc(&set->refs);
	atomic_inc(&set->pending_ios);
	submit_bio(pbio);

	submit_bio(ctx->bio);
}

static void btrfs_rst_raid56_submit_write_bios(struct btrfs_io_context *bioc,
					struct bio *orig_bios, u32 total_size)
{
	struct btrfs_raid_write_ctx ctx = {
		.fs_info = bioc->fs_info,
		.bioc = bioc,
		.bio = orig_bios,
		.next = orig_bios->bi_next,
		.stripe_bytes = btrfs_stripe_bytes(bioc),
		.total_size = total_size,
	};
	u32 stripe_offset = (ctx.bio->bi_iter.bi_sector << SECTOR_SHIFT) %
		ctx.stripe_bytes;

	if (stripe_offset)
		btrfs_rst_raid56_write_partial_stripe(&ctx, stripe_offset);

	while (ctx.total_size >= ctx.stripe_bytes)
		btrfs_rst_raid56_write_full_stripe(&ctx);

	if (ctx.total_size)
		btrfs_rst_raid56_write_partial_stripe(&ctx, 0);

	return;
}

static void btrfs_raid_unplug(struct btrfs_rst_plug *plug)
{
	btrfs_rst_raid56_submit_write_bios(plug->cb.data, plug->bios.head,
					   plug->size);
	kfree(plug);
}

static void btrfs_raid_unplug_work(struct work_struct *work)
{
	btrfs_raid_unplug(container_of(work, struct btrfs_rst_plug, work));
}

static void btrfs_raid_unplug_cb(struct blk_plug_cb *cb, bool from_schedule)
{
	struct btrfs_rst_plug *plug = container_of(cb, struct btrfs_rst_plug, cb);

	if (!plug->size) {
		kfree(plug);
		return;
	}

	if (from_schedule) {
		INIT_WORK(&plug->work, btrfs_raid_unplug_work);
		kblockd_schedule_work(&plug->work);
	} else {
		btrfs_raid_unplug(plug);
	}
}

static void btrfs_raid_plug_flush(struct btrfs_rst_plug *plug)
{
	struct btrfs_io_context *bioc = plug->cb.data;
	struct bio *bios = plug->bios.head;
	unsigned int total_size = plug->size;

	bio_list_init(&plug->bios);
	plug->size = 0;
	plug->stripe_offset = 0;
	btrfs_rst_raid56_submit_write_bios(bioc, bios, total_size);
}

static bool btrfs_raid_plug_need_flush(struct btrfs_rst_plug *plug,
				       struct bio *bio, u64 stripe_bytes)
{
	struct bio *last = plug->bios.tail;

	if (UINT_MAX - plug->size < bio->bi_iter.bi_size)
		return true;
	if (last && bio->bi_iter.bi_sector != bio_end_sector(last))
		return true;
	return false;
}

static struct btrfs_rst_plug *btrfs_raid_check_plugged(
					       struct btrfs_io_context *bioc)
{
	struct blk_plug_cb *cb;

	cb = blk_check_plugged(btrfs_raid_unplug_cb, bioc,
			       sizeof(struct btrfs_rst_plug));
	if (!cb)
		return NULL;
	return container_of(cb, struct btrfs_rst_plug, cb);
}

void btrfs_rst_raid56_submit_write_bio(struct btrfs_io_context *bioc,
				       struct bio *bio)
{
	const u32 stripe_bytes = btrfs_stripe_bytes(bioc);
	struct btrfs_rst_plug *plug;

restart:
	plug = btrfs_raid_check_plugged(bioc);
	if (!plug) {
		btrfs_rst_raid56_submit_write_bios(bioc, bio,
						   bio->bi_iter.bi_size);
		return;
	}

	if (btrfs_raid_plug_need_flush(plug, bio, stripe_bytes)) {
		btrfs_raid_plug_flush(plug);
		goto restart;
	}

	if (!plug->size) {
		plug->stripe_offset =
			(bio->bi_iter.bi_sector << SECTOR_SHIFT) % stripe_bytes;
	}

	plug->size += bio->bi_iter.bi_size;
	bio_list_add(&plug->bios, bio);
}

