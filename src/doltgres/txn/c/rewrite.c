/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The table rewrite's read of a storage: each key a base or the delta holds,
 * with the base's entry for it and the delta's versions of it that the base
 * doesn't apply; the new storage's trees and base; and the versions and map
 * versions a rewrite or a move carries into its new storage.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/build.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/trees/mutate.h"
#include "doltgres/trees/node.h"
#include "doltgres/txn/counted.h"
#include "doltgres/txn/free_space.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/tuples.h"
#include "doltgres/txn/rewrite.h"
#include "doltgres/txn/visible.h"
#include "port/pg_bswap.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "rewrite.rs"

/* The map's takes that keep nothing, and the map-only prepare whose closure returns nothing. */
#define TXN_MEMTABLE_MAP_RS Unit
#define TXN_MEMTABLE_MAP_RETURNS bool
#define TXN_MEMTABLE_MAP_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_MAP_TS Unit
#define TXN_MEMTABLE_MAP_KEPT_OUT
#define TXN_MEMTABLE_MAP_KEPT_IN
#define TXN_MEMTABLE_MAP_DECLARE
#include "doltgres/txn/memtable/map_template.h"

/* The prepare whose closure returns nothing. */
#define TXN_MEMTABLE_TABLE_RS Unit
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#include "doltgres/txn/memtable/table_template.h"

/* TXN_REWRITE_BASED is the size and alignment of a base's entry. */
#define TXN_REWRITE_BASED sizeof(TxnRewriteBased), _Alignof(TxnRewriteBased)

/* TXN_REWRITE_ADDRESS is the size and alignment of an out-of-line address with its offset. */
#define TXN_REWRITE_ADDRESS sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t)

/* TXN_REWRITE_ID is the size and alignment of a value id. */
#define TXN_REWRITE_ID sizeof(uint32), _Alignof(uint32)

/* TXN_REWRITE_STAMP is the size and alignment of a stamp. */
#define TXN_REWRITE_STAMP sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp)

/* TXN_REWRITE_ENTRY is the size and alignment of a tree's leaf entry. */
#define TXN_REWRITE_ENTRY sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry)

/* TXN_REWRITE_COPY is the size and alignment of a copy of a rewrite's entry. */
#define TXN_REWRITE_COPY sizeof(txn_TupleU64U64U32_t), _Alignof(txn_TupleU64U64U32_t)

/* txn_rewrite_Based_drop frees a base's entry. */
void
txn_rewrite_Based_drop(TxnRewriteBased *self)
{
	std_vec_free_Checked(&self->key, 1, 1);
	std_vec_free_Checked(&self->value, 1, 1);
	std_vec_free_Checked(&self->addresses, TXN_REWRITE_ADDRESS);
	std_vec_free_Checked(&self->values, TXN_REWRITE_ID);
	std_vec_free_Checked(&self->stamps, TXN_REWRITE_STAMP);
}

/*
 * txn_rewrite_Stored_drop frees what a new entry holds of its row: a re-formed
 * row's tuple, addresses, and value ids.
 */
static void
txn_rewrite_Stored_drop(TxnRewriteStored *self)
{
	if (self->tag != TXN_REWRITE_STORED_FORMED)
		return;
	std_vec_free_Checked(&self->u.formed.tuple, 1, 1);
	std_vec_free_Checked(&self->u.formed.addresses, TXN_REWRITE_ADDRESS);
	std_vec_free_Checked(&self->u.formed.values, TXN_REWRITE_ID);
}

/* txn_rewrite_Copied_drop frees an entry of a rewrite's new table tree. */
void
txn_rewrite_Copied_drop(TxnRewriteCopied *self)
{
	std_vec_free_Checked(&self->f0, 1, 1);
	txn_rewrite_Stored_drop(&self->f1);
	std_vec_free_Checked(&self->f2, TXN_REWRITE_COPY);
}

/* txn_rewrite_based_drop frees a vector of a base's entries. */
static void
txn_rewrite_based_drop(StdVec *based)
{
	for (size_t i = 0; i < based->len; i++)
		txn_rewrite_Based_drop(&STD_VEC_AT(based, TxnRewriteBased, i));
	std_vec_free_Checked(based, TXN_REWRITE_BASED);
}

/* txn_rewrite_based_closure_0_captures_t is based's pin scope's captures: the root and the entries it reads. */
typedef struct txn_rewrite_based_closure_0_captures_t
{
	TreesStoreTree root;
	StdVec	   *entries;
} txn_rewrite_based_closure_0_captures_t;

/*
 * txn_rewrite_based_closure_0_frame_t is the pin scope's state across its pad:
 * its cursor and the entry it reads, each while live.
 */
typedef struct txn_rewrite_based_closure_0_frame_t
{
	txn_rewrite_based_closure_0_captures_t *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesCursorCursor cursor;
	bool		cursor_live;
	TxnRewriteBased entry;
	bool		entry_live;
	ChunksErrorError *error;
	bool		ok;
} txn_rewrite_based_closure_0_frame_t;

/* txn_rewrite_based_closure_0_body walks the base's table tree, copying each entry. */
static void
txn_rewrite_based_closure_0_body(void *frame_)
{
	txn_rewrite_based_closure_0_frame_t *frame = frame_;
	StdVec	   *entries = frame->captures->entries;

	if (!trees_cursor_Cursor_first(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&frame->captures->root),
								   &frame->cursor, frame->error))
		return;
	frame->cursor_live = true;
	while (trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		StdBytes	key = trees_cursor_Cursor_key(&frame->cursor);
		StdBytes	value = trees_cursor_Cursor_value(&frame->cursor);
		TreesNodeNode node = trees_cursor_Cursor_node(&frame->cursor);
		TxnRewriteBased *entry = &frame->entry;

		if (!std_vec_try_reserve(entries, 1, TXN_REWRITE_BASED))
		{
			frame->error->tag = CHUNKS_ERROR_ERROR_HOST;
			frame->error->u.host = host_Error_out_of_memory();
			return;
		}
		memset(entry, 0, sizeof(*entry));
		frame->entry_live = true;
		std_slice_to_vec_Checked(&entry->key, key.ptr, key.len, 1, 1);
		std_slice_to_vec_Checked(&entry->value, value.ptr, value.len, 1, 1);
		trees_node_Node_value_addresses(&node, trees_cursor_Cursor_index(&frame->cursor), &entry->addresses);
		if (!trees_cursor_Cursor_values(&frame->cursor, frame->io, frame->pin, &entry->values, frame->error) ||
			!trees_cursor_Cursor_stamps(&frame->cursor, frame->io, frame->pin, &entry->stamps, frame->error))
			return;
		std_vec_push_Checked(entries, entry, TXN_REWRITE_BASED);
		frame->entry_live = false;
		if (!trees_cursor_Cursor_next(&frame->cursor, frame->io, frame->pin, frame->error))
			return;
	}
	frame->ok = true;
}

/* txn_rewrite_based_closure_0_pad frees what the pin scope holds live. */
static void
txn_rewrite_based_closure_0_pad(void *frame_)
{
	txn_rewrite_based_closure_0_frame_t *frame = frame_;

	if (frame->entry_live)
		txn_rewrite_Based_drop(&frame->entry);
	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
	frame->entry_live = frame->cursor_live = false;
}

/* txn_rewrite_based_closure_0 is based's pin scope: every entry of the base's table tree. */
static bool
txn_rewrite_based_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	txn_rewrite_based_closure_0_frame_t frame = {.captures = captures, .io = io, .pin = pin, .error = error};

	std_landing(&frame, txn_rewrite_based_closure_0_body, txn_rewrite_based_closure_0_pad);
	txn_rewrite_based_closure_0_pad(&frame);
	return frame.ok;
}

/* txn_rewrite_based_frame_t is based's state across its pad: the entries it reads, unless it answers them. */
typedef struct txn_rewrite_based_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreTree root;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} txn_rewrite_based_frame_t;

/* txn_rewrite_based_body reads the entries under a pin. */
static void
txn_rewrite_based_body(void *frame_)
{
	txn_rewrite_based_frame_t *frame = frame_;
	txn_rewrite_based_closure_0_captures_t captures = {.root = frame->root, .entries = frame->result};
	ChunksErrorError failed;

	std_vec_new(frame->result, TXN_REWRITE_BASED);
	frame->result_live = true;
	if (!trees_pin_pin_ResultUnit(frame->io, frame->store, txn_rewrite_based_closure_0, &captures, NULL, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->result_live = false;
	frame->ok = true;
}

/* txn_rewrite_based_pad frees the entries based read, unless it answers them. */
static void
txn_rewrite_based_pad(void *frame_)
{
	txn_rewrite_based_frame_t *frame = frame_;

	if (frame->result_live)
		txn_rewrite_based_drop(frame->result);
	frame->result_live = false;
}

/* txn_rewrite_based reads every entry of a base's table tree, in its key order, none for an empty tree. */
bool
txn_rewrite_based(HostIoIo *io, TreesStoreNodeStore store, bool has_root, TreesStoreTree root, StdVec *result,
				  HostError *error)
{
	txn_rewrite_based_frame_t frame = {.io = io, .store = store, .root = root, .result = result, .error = error};

	if (!has_root)
	{
		std_vec_new(result, TXN_REWRITE_BASED);
		return true;
	}
	std_landing(&frame, txn_rewrite_based_body, txn_rewrite_based_pad);
	txn_rewrite_based_pad(&frame);
	return frame.ok;
}

/*
 * txn_rewrite_keys_frame_t is keys' state across its pad and its walk: the
 * places by key, which it handed, and a key's versions.
 */
typedef struct txn_rewrite_keys_frame_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	const TxnRewriteBased *based;
	size_t		based_len;
	HostLsn		folded;
	TxnRewriteKeysEach each;
	void	   *each_captures;
	void		(*each_drop) (void *);
	bool		each_live;
	bool		places_live;
	StdVec		places;
	bool		handed_live;
	StdVec		handed;
	bool		hold_live;
	HostDeltaHold hold;
	bool		versions_live;
	StdVec		versions;
	HostError  *error;
	bool		ok;
} txn_rewrite_keys_frame_t;

/* txn_rewrite_place_order compares two places by their base entries' keys' bytes, the places map's order. */
static int
txn_rewrite_place_order(const void *a, const void *b, void *arg)
{
	const TxnRewriteBased *based = arg;
	const StdVec *left = &based[*(const size_t *) a].key;
	const StdVec *right = &based[*(const size_t *) b].key;

	return std_bytes_Ord_cmp(left->ptr, left->len, right->ptr, right->len);
}

/* txn_rewrite_place returns the place of the base entry whose key is a key's bytes, false for none. */
static bool
txn_rewrite_place(const txn_rewrite_keys_frame_t *frame, const uint8 *key, size_t key_len, size_t *place)
{
	size_t		low = 0;
	size_t		high = frame->places.len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		size_t		at = STD_VEC_AT(&frame->places, size_t, mid);
		const StdVec *found = &frame->based[at].key;
		StdOrdering order = std_bytes_Ord_cmp(found->ptr, found->len, key, key_len);

		if (order == STD_ORDERING_EQUAL)
		{
			*place = at;
			return true;
		}
		if (order == STD_ORDERING_LESS)
			low = mid + 1;
		else
			high = mid;
	}
	return false;
}

/* txn_rewrite_keys_closure_1 is keys' walk of the delta's rows: each key with its place and its unfolded versions. */
static bool
txn_rewrite_keys_closure_1(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
						   size_t values_len, HostError *error)
{
	txn_rewrite_keys_frame_t *frame = captures;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	size_t		place = 0;
	bool		has_place;

	if (!host_delta_hold(io, &frame->hold, error))
		return false;
	frame->hold_live = true;
	if (!txn_visible_versions(io, &frame->hold, record, key, key_len, frame->folded, &frame->versions, error))
		return false;
	frame->versions_live = true;
	has_place = txn_rewrite_place(frame, key, key_len, &place);
	if (has_place)
		STD_VEC_AT(&frame->handed, bool, place) = true;
	if (!frame->each(frame->each_captures, io, key, key_len, has_place, place, &frame->versions, error))
		return false;
	frame->versions_live = false;
	txn_memtable_version_versions_drop(&frame->versions, 0);
	frame->hold_live = false;
	host_delta_Hold_drop(&frame->hold);
	return true;
}

/* txn_rewrite_keys_body walks the delta's keys, then hands each base entry it didn't hand with no versions. */
static void
txn_rewrite_keys_body(void *frame_)
{
	txn_rewrite_keys_frame_t *frame = frame_;
	static const bool unhanded = false;

	std_vec_with_capacity_Checked(&frame->places, frame->based_len, sizeof(size_t), _Alignof(size_t));
	frame->places_live = true;
	for (size_t at = 0; at < frame->based_len; at++)
		std_vec_push_Checked(&frame->places, &at, sizeof(size_t), _Alignof(size_t));
	if (frame->places.len > 1)
		qsort_arg(frame->places.ptr, frame->places.len, sizeof(size_t), txn_rewrite_place_order,
				  (void *) frame->based);
	std_vec_with_capacity_Checked(&frame->handed, frame->based_len, sizeof(bool), _Alignof(bool));
	frame->handed_live = true;
	for (size_t at = 0; at < frame->based_len; at++)
		std_vec_push_Checked(&frame->handed, &unhanded, sizeof(bool), _Alignof(bool));
	if (!txn_memtable_table_walked(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, host_shared_Held_record(frame->held),
								   txn_rewrite_keys_closure_1, frame, NULL, frame->error))
		return;
	for (size_t at = 0; at < frame->based_len; at++)
	{
		if (STD_VEC_AT(&frame->handed, bool, at))
			continue;
		std_vec_new(&frame->versions, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
		frame->versions_live = true;
		if (!frame->each(frame->each_captures, frame->io, frame->based[at].key.ptr, frame->based[at].key.len, true, at,
						 &frame->versions, frame->error))
			return;
		frame->versions_live = false;
		txn_memtable_version_versions_drop(&frame->versions, 0);
	}
	frame->ok = true;
}

/* txn_rewrite_keys_pad frees what keys holds live, and its each. */
static void
txn_rewrite_keys_pad(void *frame_)
{
	txn_rewrite_keys_frame_t *frame = frame_;

	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->handed_live)
		std_vec_free_Checked(&frame->handed, sizeof(bool), _Alignof(bool));
	if (frame->places_live)
		std_vec_free_Checked(&frame->places, sizeof(size_t), _Alignof(size_t));
	if (frame->each_live && frame->each_drop != NULL)
		frame->each_drop(frame->each_captures);
	frame->versions_live = frame->hold_live = frame->handed_live = frame->places_live = frame->each_live = false;
}

/*
 * txn_rewrite_keys hands each every key a base's entries or a storage's delta
 * holds, once, with the place of its entry among the base's and its versions
 * newest first, less those the base folded at folded applied: the delta's
 * keys in their bytes' order, then the base's other entries in its key order
 * with none.
 */
bool
txn_rewrite_keys(HostIoIo *io, const HostSharedHeld *held, const TxnRewriteBased *based, size_t based_len,
				 HostLsn folded, TxnRewriteKeysEach each, void *each_captures, void (*each_drop) (void *),
				 HostError *error)
{
	txn_rewrite_keys_frame_t frame = {.io = io, .held = held, .based = based, .based_len = based_len,
		.folded = folded, .each = each, .each_captures = each_captures, .each_drop = each_drop, .each_live = true,
	.error = error};

	std_landing(&frame, txn_rewrite_keys_body, txn_rewrite_keys_pad);
	txn_rewrite_keys_pad(&frame);
	return frame.ok;
}

/* TxnRewriteBlock is a block of a new storage's free space map: its free bytes and its free offsets. */
typedef struct TxnRewriteBlock
{
	uint32		block;
	uint64		bytes;
	StdVec		offsets;
} TxnRewriteBlock;

/* TXN_REWRITE_BLOCK is the size and alignment of a new free space map's block. */
#define TXN_REWRITE_BLOCK sizeof(TxnRewriteBlock), _Alignof(TxnRewriteBlock)

/* txn_rewrite_block_order orders a new free space map's blocks by their numbers. */
static int
txn_rewrite_block_order(const void *a, const void *b)
{
	uint32		left = ((const TxnRewriteBlock *) a)->block;
	uint32		right = ((const TxnRewriteBlock *) b)->block;

	return left < right ? -1 : left > right;
}

/* txn_rewrite_entries_drop frees a vector of leaf entries. */
static void
txn_rewrite_entries_drop(StdVec *entries)
{
	for (size_t i = 0; i < entries->len; i++)
	{
		TreesBuildEntry *entry = &STD_VEC_AT(entries, TreesBuildEntry, i);

		std_vec_free_Checked(&entry->key, 1, 1);
		std_vec_free_Checked(&entry->value, 1, 1);
		std_vec_free_Checked(&entry->addresses, TXN_REWRITE_ADDRESS);
		std_vec_free_Checked(&entry->stamps, TXN_REWRITE_STAMP);
		std_vec_free_Checked(&entry->values, TXN_REWRITE_ID);
	}
	std_vec_free_Checked(entries, TXN_REWRITE_ENTRY);
}

/* txn_rewrite_trees_drop frees a new storage's trees' entries. */
static void
txn_rewrite_trees_drop(TxnFoldTrees *trees)
{
	txn_rewrite_entries_drop(&trees->rows);
	txn_rewrite_entries_drop(&trees->tuple_ids);
	txn_rewrite_entries_drop(&trees->free_space);
	txn_rewrite_entries_drop(&trees->pending);
}

/* txn_rewrite_trees_new returns a new storage's empty trees. */
static void
txn_rewrite_trees_new(TxnFoldTrees *trees)
{
	std_vec_new(&trees->rows, TXN_REWRITE_ENTRY);
	std_vec_new(&trees->tuple_ids, TXN_REWRITE_ENTRY);
	std_vec_new(&trees->free_space, TXN_REWRITE_ENTRY);
	std_vec_new(&trees->pending, TXN_REWRITE_ENTRY);
}

/*
 * txn_rewrite_new_base_frame_t is new_base's state across its pad: what it
 * takes, the trees it builds, an entry's parts between its pieces, and the
 * free space map's blocks, each while live.
 */
typedef struct txn_rewrite_new_base_frame_t
{
	StdVec	   *based;
	bool		based_live;
	StdVec	   *copied;
	size_t		copied_at;
	bool		copied_live;
	bool		keyless;
	uint64		folded;
	uint64		max_folded;
	uint64		max_ended;
	TxnBaseBase *made;
	bool		made_live;
	TxnRewritePacked packed;
	const host_TupleRowU32_t *carried;
	size_t		carried_len;
	TxnFoldTrees *trees;
	bool		trees_live;
	bool		parts_live;
	TreesBuildEntry parts;
	bool		blocks_live;
	StdVec		blocks;
	TxnBaseBase *result;
	HostError  *error;
	bool		ok;
} txn_rewrite_new_base_frame_t;

/* txn_rewrite_new_base_copy forms one copied entry's row, its copies' pairs, and its sizes, taking the entry. */
static bool
txn_rewrite_new_base_copy(txn_rewrite_new_base_frame_t *frame, TxnRewriteCopied *copied, TxnBaseSizes *sizes)
{
	TreesBuildEntry *parts = &frame->parts;
	uint64		count = (uint64) copied->f2.len;
	ChunksErrorError failed;

	memset(parts, 0, sizeof(*parts));
	frame->parts_live = true;
	parts->key = copied->f0;
	std_vec_new(&copied->f0, 1, 1);
	if (copied->f1.tag == TXN_REWRITE_STORED_BASED)
	{
		size_t		at = copied->f1.u.based;
		TxnRewriteBased *based;

		std_index_check(at, frame->based->len, RUST_FILE, frame->keyless ? 137 : 144, frame->keyless ? 61 : 37);
		based = &STD_VEC_AT(frame->based, TxnRewriteBased, at);
		if (frame->keyless && copied->f2.len != based->stamps.len)
		{
			if (!txn_counted_recount(based->value.ptr, based->value.len, based->addresses.ptr, based->addresses.len,
									 count, &parts->value, &parts->addresses, &failed))
			{
				*frame->error = txn_base_storage(&failed);
				return false;
			}
		}
		else
		{
			parts->value = based->value;
			parts->addresses = based->addresses;
			std_vec_new(&based->value, 1, 1);
			std_vec_new(&based->addresses, TXN_REWRITE_ADDRESS);
		}
		parts->values = based->values;
		std_vec_new(&based->values, TXN_REWRITE_ID);
	}
	else if (frame->keyless)
	{
		txn_counted_keyless_value(count, copied->f1.u.formed.tuple.ptr, copied->f1.u.formed.tuple.len,
								  &parts->value);
		txn_counted_keyless_addresses(count, copied->f1.u.formed.addresses.ptr, copied->f1.u.formed.addresses.len,
									  &parts->addresses);
		parts->values = copied->f1.u.formed.values;
		std_vec_new(&copied->f1.u.formed.values, TXN_REWRITE_ID);
	}
	else
	{
		parts->value = copied->f1.u.formed.tuple;
		parts->addresses = copied->f1.u.formed.addresses;
		parts->values = copied->f1.u.formed.values;
		std_vec_new(&copied->f1.u.formed.tuple, 1, 1);
		std_vec_new(&copied->f1.u.formed.addresses, TXN_REWRITE_ADDRESS);
		std_vec_new(&copied->f1.u.formed.values, TXN_REWRITE_ID);
	}
	for (size_t i = 0; i < copied->f2.len; i++)
	{
		const txn_TupleU64U64U32_t *copy = &STD_VEC_AT(&copied->f2, txn_TupleU64U64U32_t, i);
		StdVec		pair;
		StdVec		key;
		uint8		tid[6];
		TreesBuildEntry entry;

		txn_base_pair_value(parts->key.ptr, parts->key.len, copy->f2, &pair);
		txn_base_tuple_id_key(copy->f1, tid);
		std_slice_to_vec_Checked(&key, tid, sizeof(tid), 1, 1);
		trees_build_Entry_new_VecU8VecU8(&key, &pair, &entry);
		std_vec_push_Checked(&frame->trees->tuple_ids, &entry, TXN_REWRITE_ENTRY);
		sizes->heap_bytes = STD_ADD(u64, sizes->heap_bytes, copy->f2, 156, 13);
	}
	sizes->entries = STD_ADD(u64, sizes->entries, 1, 158, 9);
	sizes->leaf_bytes = STD_ADD(u64, sizes->leaf_bytes,
								trees_chunker_leaf_entry_size(parts->key.ptr, parts->key.len, parts->value.ptr,
															  parts->value.len),
								159, 9);
	sizes->key_bytes = STD_ADD(u64, sizes->key_bytes, (uint64) parts->key.len, 160, 9);
	sizes->rows = STD_ADD(u64, sizes->rows, count, 161, 9);
	std_vec_with_capacity_Checked(&parts->stamps, copied->f2.len, TXN_REWRITE_STAMP);
	for (size_t i = 0; i < copied->f2.len; i++)
	{
		const txn_TupleU64U64U32_t *copy = &STD_VEC_AT(&copied->f2, txn_TupleU64U64U32_t, i);
		ChunksTrailerStamp stamp = {.txn = copy->f0, .tuple = copy->f1};

		std_vec_push_Checked(&parts->stamps, &stamp, TXN_REWRITE_STAMP);
	}
	std_vec_push_Checked(&frame->trees->rows, parts, TXN_REWRITE_ENTRY);
	frame->parts_live = false;
	return true;
}

/*
 * txn_rewrite_new_base_blocks forms the new free space map: the last page
 * while it has an offset left, and each carried put's place.
 */
static void
txn_rewrite_new_base_blocks(txn_rewrite_new_base_frame_t *frame)
{
	uint64		counter = frame->packed.f0;
	uint64		free = frame->packed.f1;
	uint16		most = frame->packed.f2;

	std_vec_new(&frame->blocks, TXN_REWRITE_BLOCK);
	frame->blocks_live = true;
	if ((uint16) (counter & 0xffff) <= most && free > 0)
	{
		TxnRewriteBlock block = {.block = (uint32) (counter >> 16), .bytes = free};

		std_vec_new(&block.offsets, sizeof(uint16), _Alignof(uint16));
		std_vec_push_Checked(&frame->blocks, &block, TXN_REWRITE_BLOCK);
	}
	for (size_t i = 0; i < frame->carried_len; i++)
	{
		uint32		number = (uint32) (frame->carried[i].f0.f0 >> 16);
		uint16		offset = (uint16) (frame->carried[i].f0.f0 & 0xffff);
		TxnRewriteBlock *block = NULL;

		for (size_t at = 0; at < frame->blocks.len && block == NULL; at++)
			if (STD_VEC_AT(&frame->blocks, TxnRewriteBlock, at).block == number)
				block = &STD_VEC_AT(&frame->blocks, TxnRewriteBlock, at);
		if (block == NULL)
		{
			TxnRewriteBlock added = {.block = number};

			std_vec_new(&added.offsets, sizeof(uint16), _Alignof(uint16));
			std_vec_push_Checked(&frame->blocks, &added, TXN_REWRITE_BLOCK);
			block = &STD_VEC_AT(&frame->blocks, TxnRewriteBlock, frame->blocks.len - 1);
		}
		block->bytes = STD_ADD(u64, block->bytes, frame->carried[i].f1, 171, 9);
		std_vec_push_Checked(&block->offsets, &offset, sizeof(uint16), _Alignof(uint16));
	}
	if (frame->blocks.len > 1)
		qsort(frame->blocks.ptr, frame->blocks.len, sizeof(TxnRewriteBlock), txn_rewrite_block_order);
	for (size_t i = 0; i < frame->blocks.len; i++)
	{
		const TxnRewriteBlock *block = &STD_VEC_AT(&frame->blocks, TxnRewriteBlock, i);
		TxnFreeSpaceFree entry = txn_free_space_Free_new(block->bytes, block->offsets.ptr, block->offsets.len, most);
		StdVec		value;
		StdVec		key;
		uint8		number[4];
		TreesBuildEntry built;

		std_vec_new(&value, 1, 1);
		txn_free_space_Free_encode_into(&entry, &value);
		txn_free_space_key(block->block, number);
		std_slice_to_vec_Checked(&key, number, sizeof(number), 1, 1);
		trees_build_Entry_new_VecU8VecU8(&key, &value, &built);
		std_vec_push_Checked(&frame->trees->free_space, &built, TXN_REWRITE_ENTRY);
	}
}

/* txn_rewrite_new_base_body forms the trees from the copied entries and the carried puts, then the base record. */
static void
txn_rewrite_new_base_body(void *frame_)
{
	txn_rewrite_new_base_frame_t *frame = frame_;
	TxnBaseSizes sizes = txn_base_Sizes_Default_default();

	txn_rewrite_trees_new(frame->trees);
	frame->trees_live = true;
	for (; frame->copied_at < frame->copied->len; frame->copied_at++)
	{
		TxnRewriteCopied *copied = &STD_VEC_AT(frame->copied, TxnRewriteCopied, frame->copied_at);
		bool		formed = txn_rewrite_new_base_copy(frame, copied, &sizes);

		txn_rewrite_Copied_drop(copied);
		if (!formed)
		{
			frame->copied_at++;
			return;
		}
	}
	txn_rewrite_new_base_blocks(frame);
	*frame->result = *frame->made;
	frame->made_live = false;
	frame->result->folded = frame->folded;
	frame->result->max_folded = frame->max_folded;
	frame->result->max_ended = frame->max_ended;
	frame->result->counter = frame->packed.f0;
	frame->result->fresh_end = frame->packed.f0;
	frame->result->sizes = sizes;
	frame->trees_live = false;
	frame->ok = true;
}

/* txn_rewrite_new_base_pad frees what new_base holds live and what it took. */
static void
txn_rewrite_new_base_pad(void *frame_)
{
	txn_rewrite_new_base_frame_t *frame = frame_;

	if (frame->blocks_live)
	{
		for (size_t i = 0; i < frame->blocks.len; i++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->blocks, TxnRewriteBlock, i).offsets, sizeof(uint16),
								 _Alignof(uint16));
		std_vec_free_Checked(&frame->blocks, TXN_REWRITE_BLOCK);
	}
	if (frame->parts_live)
	{
		std_vec_free_Checked(&frame->parts.key, 1, 1);
		std_vec_free_Checked(&frame->parts.value, 1, 1);
		std_vec_free_Checked(&frame->parts.addresses, TXN_REWRITE_ADDRESS);
		std_vec_free_Checked(&frame->parts.stamps, TXN_REWRITE_STAMP);
		std_vec_free_Checked(&frame->parts.values, TXN_REWRITE_ID);
	}
	if (frame->trees_live)
		txn_rewrite_trees_drop(frame->trees);
	if (frame->made_live)
		txn_base_Base_drop(frame->made);
	if (frame->copied_live)
	{
		for (size_t i = frame->copied_at; i < frame->copied->len; i++)
			txn_rewrite_Copied_drop(&STD_VEC_AT(frame->copied, TxnRewriteCopied, i));
		std_vec_free_Checked(frame->copied, sizeof(TxnRewriteCopied), _Alignof(TxnRewriteCopied));
	}
	if (frame->based_live)
		txn_rewrite_based_drop(frame->based);
	frame->blocks_live = frame->parts_live = frame->trees_live = frame->made_live = false;
	frame->copied_live = frame->based_live = false;
}

/*
 * txn_rewrite_new_base returns the trees and the base record of a rewrite's
 * new storage, taking the base's entries, the copied entries, and the record
 * its state was made with: its table tree's entries hold their rows as a
 * base held them, a keyless entry's count recounted to its kept copies, or
 * as they were re-formed, a keyless one's counted, each copy's stamp its new
 * tuple id; its tuple id tree pairs each new tuple id with its key and heap
 * bytes; its free space map holds the last page's free bytes while that page
 * has an offset left, and each carried put's tuple id free with its heap
 * bytes; its counter and fresh end are the tuple id past every one the
 * packing gave; and its sizes are counted from its entries and pairs.
 */
bool
txn_rewrite_new_base(StdVec *based, StdVec *copied, bool keyless, uint64 folded, uint64 max_folded, uint64 max_ended,
					 TxnBaseBase *made, TxnRewritePacked packed, const host_TupleRowU32_t *carried,
					 size_t carried_len, TxnFoldTrees *result_0, TxnBaseBase *result_1, HostError *error)
{
	txn_rewrite_new_base_frame_t frame = {.based = based, .based_live = true, .copied = copied, .copied_live = true,
		.keyless = keyless, .folded = folded, .max_folded = max_folded, .max_ended = max_ended, .made = made,
		.made_live = true, .packed = packed, .carried = carried, .carried_len = carried_len, .trees = result_0,
	.result = result_1, .error = error};

	std_landing(&frame, txn_rewrite_new_base_body, txn_rewrite_new_base_pad);
	txn_rewrite_new_base_pad(&frame);
	return frame.ok;
}

/* txn_rewrite_carried_closure_5_captures_t is carried's prepare's closure's captures. */
typedef struct txn_rewrite_carried_closure_5_captures_t
{
	const HostSharedHeld *held;
	const TxnRecordRowChange *change;
	TxnMemtableVersionShown shown;
	TxnMemtableLockMode lock;
	bool		logged;
	bool		has_roomed;
	TxnStatusRoomed roomed;
	bool		has_placed;
	const TxnInsertPlaced *placed;
	bool		has_tuple;
	HostRow		tuple;
	bool		has_marks;
	StdVec	   *marks;
	size_t		marking;
	TxnRecordEncoding *encoding;
	bool	   *encoding_live;
} txn_rewrite_carried_closure_5_captures_t;

/*
 * txn_rewrite_carried_closure_5_frame_t is the closure's state across its pad:
 * its window and its record's body, each while live.
 */
typedef struct txn_rewrite_carried_closure_5_frame_t
{
	txn_rewrite_carried_closure_5_captures_t *captures;
	HostIoIo   *io;
	TxnMemtableTablePreparedState *prepared;
	HostDelayCheckpoint window;
	bool		window_live;
	StdVec		body;
	bool		body_live;
	HostError  *error;
	bool		ok;
} txn_rewrite_carried_closure_5_frame_t;

/*
 * txn_rewrite_carried_closure_5_body takes the place, appends the record,
 * pushes the mark, the version, and the slot, in one window.
 */
static void
txn_rewrite_carried_closure_5_body(void *frame_)
{
	txn_rewrite_carried_closure_5_frame_t *frame = frame_;
	txn_rewrite_carried_closure_5_captures_t *c = frame->captures;
	HostIoTakesShort io = HOST_IO_IO_TAKES_SHORT(frame->io);
	HostSharedRecord record = host_shared_Held_record(c->held);
	TxnMemtableVersionChange pushed = {.writer = c->change->writer, .op = &c->change->op, .lock = c->lock,
	.shown = c->shown};
	TxnMemtableVersionHandle handle;

	if (!host_delay_checkpoint(HOST_IO_IO_IO_DOORS(frame->io), NULL, 0, &frame->window, frame->error))
		return;
	frame->window_live = true;
	if (c->has_placed &&
		(!txn_insert_place_taken(io, frame->prepared, 0, record, c->change->writer, c->placed, frame->error) ||
		 !txn_memtable_table_counted(io, frame->prepared, c->has_roomed, c->roomed, frame->error)))
		return;
	if (c->logged)
	{
		*c->encoding_live = false;
		txn_record_RowChange_encode_with(c->change, c->encoding, &frame->body);
		frame->body_live = true;
		if (!host_wal_insert(HOST_IO_IO_IO_DOORS(frame->io), HOST_WAL_KIND_ROW_CHANGE, frame->body.ptr,
							 frame->body.len, &pushed.lsn, frame->error))
			return;
		pushed.has_lsn = true;
		frame->body_live = false;
		std_vec_free_Checked(&frame->body, 1, 1);
	}
	if (c->has_marks)
	{
		StdVec		marks = *c->marks;

		c->has_marks = false;
		std_vec_new(c->marks, TXN_REWRITE_ID);
		if (!txn_memtable_map_named(io, frame->prepared, c->marking, c->change->writer, pushed.has_lsn, pushed.lsn,
									c->tuple, &marks, frame->error) ||
			!txn_memtable_table_counted(io, frame->prepared, c->has_roomed, c->roomed, frame->error))
			return;
	}
	if (!txn_memtable_table_finish(io, frame->prepared, &pushed, c->has_roomed, c->roomed, &handle, frame->error))
		return;
	if (c->has_tuple && !txn_memtable_tuples_finish(io, frame->prepared, c->tuple, frame->error))
		return;
	frame->window_live = false;
	host_DelayCheckpoint_finish(&frame->window);
	frame->ok = true;
}

/* txn_rewrite_carried_closure_5_pad frees what the closure holds live. */
static void
txn_rewrite_carried_closure_5_pad(void *frame_)
{
	txn_rewrite_carried_closure_5_frame_t *frame = frame_;

	if (frame->body_live)
		std_vec_free_Checked(&frame->body, 1, 1);
	if (frame->window_live)
		host_DelayCheckpoint_drop(&frame->window);
	frame->body_live = frame->window_live = false;
}

/* txn_rewrite_carried_closure_5 is carried's prepare's closure. */
static bool
txn_rewrite_carried_closure_5(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared, HostError *error)
{
	txn_rewrite_carried_closure_5_frame_t frame = {.captures = captures, .io = io, .prepared = prepared,
	.error = error};

	std_landing(&frame, txn_rewrite_carried_closure_5_body, txn_rewrite_carried_closure_5_pad);
	txn_rewrite_carried_closure_5_pad(&frame);
	return frame.ok;
}

/*
 * txn_rewrite_carried_frame_t is carried's state across its pad: its place,
 * its marks, and its encoding, each while live.
 */
typedef struct txn_rewrite_carried_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const TxnRecordRowChange *change;
	TxnMemtableVersionShown shown;
	TxnMemtableLockMode lock;
	bool		logged;
	const TxnInsertPage *page;
	bool		placed_live;
	TxnInsertPlaced placed;
	bool		marks_live;
	StdVec		marks;
	bool		encoding_live;
	TxnRecordEncoding encoding;
	HostError  *error;
	bool		ok;
} txn_rewrite_carried_frame_t;

/* txn_rewrite_carried_body reads the place and the marks, then runs the version's prepare. */
static void
txn_rewrite_carried_body(void *frame_)
{
	txn_rewrite_carried_frame_t *frame = frame_;
	const TxnRecordRowChange *change = frame->change;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	bool		locks = change->op.tag == TXN_MEMTABLE_OP_LOCK || change->op.tag == TXN_MEMTABLE_OP_LOCK_ROW;
	txn_rewrite_carried_closure_5_captures_t captures = {.held = frame->held, .change = change, .shown = frame->shown,
		.lock = frame->lock, .logged = frame->logged && !locks, .placed = &frame->placed, .marks = &frame->marks,
	.encoding = &frame->encoding, .encoding_live = &frame->encoding_live};
	const TxnMemtablePut *put = NULL;
	TxnMemtableMapTake takes[2];
	size_t		takes_len = 0;
	txn_TupleRefSliceU8Usize_t row;

	if (!locks &&
		!txn_status_redo_roomed(frame->io, &change->writer, &captures.has_roomed, &captures.roomed, frame->error))
		return;
	if (change->has_index || !txn_record_RowChange_put(change, &put))
		put = NULL;
	captures.has_tuple = put != NULL;
	if (put != NULL)
		captures.tuple = put->tuple;
	if (put != NULL && frame->page != NULL)
	{
		if (!txn_insert_Placed_read(frame->io, frame->store, frame->held, put->tuple, put->heap_bytes, frame->page,
									&frame->placed, frame->error))
			return;
		frame->placed_live = true;
		captures.has_placed = true;
		takes[takes_len++] = txn_insert_Placed_take(&frame->placed, record);
	}
	if (put != NULL && frame->page != NULL && put->values.len > 0)
	{
		std_slice_to_vec_Checked(&frame->marks, put->values.ptr, put->values.len, TXN_REWRITE_ID);
		frame->marks_live = true;
		captures.has_marks = true;
	}
	captures.marking = takes_len;
	if (captures.has_marks)
	{
		TxnMemtableMapTake take = {.state = record, .map = TXN_MEMTABLE_MAP_MAP_FREE,
		.key = (uint32) (put->tuple.f0 >> 16), .most = txn_memtable_map_named_most(frame->marks.len)};

		takes[takes_len++] = take;
	}
	row.f0.ptr = change->key.ptr;
	row.f0.len = change->key.len;
	row.f1 = txn_memtable_version_encoded(&change->writer, &change->op, frame->shown, captures.logged, 0);
	txn_record_RowChange_encoding(change, &frame->encoding);
	frame->encoding_live = true;
	frame->ok = txn_memtable_table_prepare_Unit(frame->io, record, &row, 1, takes, takes_len, captures.has_tuple,
												txn_rewrite_carried_closure_5, &captures, NULL, frame->error);
}

/* txn_rewrite_carried_pad frees what carried holds live. */
static void
txn_rewrite_carried_pad(void *frame_)
{
	txn_rewrite_carried_frame_t *frame = frame_;

	if (frame->encoding_live)
		txn_record_Encoding_drop(&frame->encoding);
	if (frame->marks_live)
		std_vec_free_Checked(&frame->marks, TXN_REWRITE_ID);
	if (frame->placed_live)
		txn_insert_Placed_drop(&frame->placed);
	frame->encoding_live = frame->marks_live = frame->placed_live = false;
}

/*
 * txn_rewrite_carried pushes a version a rewrite carries into its new
 * storage: in one checkpoint window, a logged storage's row change record
 * under the version's own writer, whose end becomes the version's LSN, then
 * the version with the command it showed and the row lock it held, counted
 * at its writer's top-level entry, and a table row's tuple id in the tuple id
 * table; a lock is never logged and counts nothing. Given the page a rewrite
 * placed it on, a table row's put first takes its tuple id's place as the
 * writer's, counted at the writer's entry, and one that names values out of
 * line pushes its values mark after its record.
 */
bool
txn_rewrite_carried(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
					const TxnRecordRowChange *change, TxnMemtableVersionShown shown, TxnMemtableLockMode lock,
					bool logged, const TxnInsertPage *page, HostError *error)
{
	txn_rewrite_carried_frame_t frame = {.io = io, .store = store, .held = held, .change = change, .shown = shown,
	.lock = lock, .logged = logged, .page = page, .error = error};

	std_landing(&frame, txn_rewrite_carried_body, txn_rewrite_carried_pad);
	txn_rewrite_carried_pad(&frame);
	return frame.ok;
}

/* txn_rewrite_value_map_frame_t is value_map's state across its pad: the trees it builds, unless it answers them. */
typedef struct txn_rewrite_value_map_frame_t
{
	const txn_TupleU32TupleEntryU64_t *values;
	size_t		values_len;
	TxnFoldTrees *trees;
	bool		trees_live;
	TxnBaseSizes *sizes;
	HostError  *error;
	bool		ok;
} txn_rewrite_value_map_frame_t;

/* txn_rewrite_value_map_body forms each kept value's entry with its recounted count, summing the sizes. */
static void
txn_rewrite_value_map_body(void *frame_)
{
	txn_rewrite_value_map_frame_t *frame = frame_;

	*frame->sizes = txn_base_Sizes_Default_default();
	txn_rewrite_trees_new(frame->trees);
	frame->trees_live = true;
	std_vec_with_capacity_Checked(&frame->trees->rows, frame->values_len, TXN_REWRITE_ENTRY);
	for (size_t i = 0; i < frame->values_len; i++)
	{
		TxnValuesEntry entry = frame->values[i].f1.f0;
		StdVec		value;
		StdVec		key;
		uint8		id[4];
		TxnBaseSizes sizes;
		TreesBuildEntry built;

		entry.count = frame->values[i].f1.f1;
		std_vec_new(&value, 1, 1);
		txn_values_Entry_encode_into(&entry, &value);
		txn_values_key(frame->values[i].f0, id);
		if (!txn_values_sizes(id, sizeof(id), value.ptr, value.len, &sizes, frame->error))
		{
			std_vec_free_Checked(&value, 1, 1);
			return;
		}
		*frame->sizes = txn_base_Sizes_summed(*frame->sizes, sizes);
		std_slice_to_vec_Checked(&key, id, sizeof(id), 1, 1);
		trees_build_Entry_new_VecU8VecU8(&key, &value, &built);
		std_vec_push_Checked(&frame->trees->rows, &built, TXN_REWRITE_ENTRY);
	}
	frame->trees_live = false;
	frame->ok = true;
}

/* txn_rewrite_value_map_pad frees the trees value_map built, unless it answers them. */
static void
txn_rewrite_value_map_pad(void *frame_)
{
	txn_rewrite_value_map_frame_t *frame = frame_;

	if (frame->trees_live)
		txn_rewrite_trees_drop(frame->trees);
	frame->trees_live = false;
}

/*
 * txn_rewrite_value_map returns the trees of a rewrite's new TOAST storage,
 * whose base is its value map: each value id the kept versions name, with
 * the old map's entry, its count recounted to the versions naming it; and
 * the base's sizes, its entries'.
 */
bool
txn_rewrite_value_map(const txn_TupleU32TupleEntryU64_t *values, size_t values_len, TxnFoldTrees *result_0,
					  TxnBaseSizes *result_1, HostError *error)
{
	txn_rewrite_value_map_frame_t frame = {.values = values, .values_len = values_len, .trees = result_0,
	.sizes = result_1, .error = error};

	std_landing(&frame, txn_rewrite_value_map_body, txn_rewrite_value_map_pad);
	txn_rewrite_value_map_pad(&frame);
	return frame.ok;
}

/*
 * txn_rewrite_spaces_closure_0_captures_t is spaces' each's captures: the
 * state, the storage, and the carried versions.
 */
typedef struct txn_rewrite_spaces_closure_0_captures_t
{
	HostSharedRecord record;
	HostStorage storage;
	StdVec	   *carried;
	bool		takers_live;
	StdVec		takers;
	bool		versions_live;
	StdVec		versions;
} txn_rewrite_spaces_closure_0_captures_t;

/* txn_rewrite_takes_drop frees carried takes. */
static void
txn_rewrite_takes_drop(StdVec *takes)
{
	for (size_t i = 0; i < takes->len; i++)
	{
		TxnRecordCarriedTake *take = &STD_VEC_AT(takes, TxnRecordCarriedTake, i);

		if (take->has_value)
			std_vec_free_Checked(&take->value, 1, 1);
		if (take->has_mark)
			std_vec_free_Checked(&take->mark, TXN_REWRITE_ID);
	}
	std_vec_free_Checked(takes, sizeof(TxnRecordCarriedTake), _Alignof(TxnRecordCarriedTake));
}

/* txn_rewrite_takers_drop frees a map key's takers as the map read them. */
static void
txn_rewrite_takers_drop(StdVec *takers)
{
	for (size_t i = 0; i < takers->len; i++)
	{
		txn_TupleWriterRowEntryU64TupleVecU32Bool_t *taker = &STD_VEC_AT(takers,
																		 txn_TupleWriterRowEntryU64TupleVecU32Bool_t,
																		 i);

		if (taker->f2.has_value)
			std_vec_free_Checked(&taker->f2.value, 1, 1);
		std_vec_free_Checked(&taker->f4.f0, TXN_REWRITE_ID);
	}
	std_vec_free_Checked(takers, sizeof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t),
						 _Alignof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t));
}

/* txn_rewrite_spaces_closure_0 is spaces' each: a map key's versions, carried for the new storage when it has any. */
static bool
txn_rewrite_spaces_closure_0(txn_rewrite_spaces_closure_0_captures_t *each, HostIoIo *io, TxnMemtableMapMap map,
							 const uint8 *key, size_t key_len, HostError *error)
{
	uint32		number;

	if (key_len != 4)
	{
		StdString	message;

		std_string_from_Checked(&message, "a map's key isn't 32 bits", 25);
		*error = host_Error_fork((const uint8 *) "XX001", &message);
		return false;
	}
	memcpy(&number, key, 4);
	number = pg_ntoh32(number);
	if (!txn_memtable_map_takers(io, each->record, map, number, &each->takers, error))
		return false;
	each->takers_live = true;
	std_vec_with_capacity_Checked(&each->versions, each->takers.len, sizeof(TxnRecordCarriedTake),
								  _Alignof(TxnRecordCarriedTake));
	each->versions_live = true;
	for (size_t i = 0; i < each->takers.len; i++)
	{
		txn_TupleWriterRowEntryU64TupleVecU32Bool_t *taker = &STD_VEC_AT(&each->takers,
																		 txn_TupleWriterRowEntryU64TupleVecU32Bool_t,
																		 i);
		TxnRecordCarriedTake take = {.writer = taker->f0, .tuple = taker->f1, .has_value = taker->f2.has_value,
		.value = taker->f2.value, .ender = taker->f3, .has_mark = taker->f4.f1, .mark = taker->f4.f0};

		taker->f2.has_value = false;
		std_vec_new(&taker->f4.f0, TXN_REWRITE_ID);
		if (!take.has_mark)
			std_vec_free_Checked(&take.mark, TXN_REWRITE_ID);
		std_vec_push_Checked(&each->versions, &take, sizeof(take), _Alignof(TxnRecordCarriedTake));
	}
	each->takers_live = false;
	txn_rewrite_takers_drop(&each->takers);
	if (each->versions.len > 0)
	{
		TxnRecordMapCarried carried = {.storage = each->storage, .map = map, .key = number,
		.versions = each->versions};

		if (!std_vec_try_reserve(each->carried, 1, sizeof(carried), _Alignof(TxnRecordMapCarried)))
		{
			*error = host_Error_out_of_memory();
			return false;
		}
		std_vec_push_Checked(each->carried, &carried, sizeof(carried), _Alignof(TxnRecordMapCarried));
	}
	else
		std_vec_free_Checked(&each->versions, sizeof(TxnRecordCarriedTake), _Alignof(TxnRecordCarriedTake));
	each->versions_live = false;
	return true;
}

/* txn_rewrite_spaces_closure_1 is spaces' walk of the free space map's keys. */
static bool
txn_rewrite_spaces_closure_1(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
							 size_t values_len, HostError *error)
{
	return txn_rewrite_spaces_closure_0(captures, io, TXN_MEMTABLE_MAP_MAP_FREE, key, key_len, error);
}

/* txn_rewrite_spaces_closure_2 is spaces' walk of the value map's keys. */
static bool
txn_rewrite_spaces_closure_2(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
							 size_t values_len, HostError *error)
{
	return txn_rewrite_spaces_closure_0(captures, io, TXN_MEMTABLE_MAP_MAP_VALUES, key, key_len, error);
}

/*
 * txn_rewrite_spaces_frame_t is spaces' state across its pad: its each's reads
 * and the carried versions, unless it answers them.
 */
typedef struct txn_rewrite_spaces_frame_t
{
	HostIoIo   *io;
	txn_rewrite_spaces_closure_0_captures_t each;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} txn_rewrite_spaces_frame_t;

/* txn_rewrite_spaces_body walks the free space map's keys, then the value map's. */
static void
txn_rewrite_spaces_body(void *frame_)
{
	txn_rewrite_spaces_frame_t *frame = frame_;

	std_vec_new(frame->result, sizeof(TxnRecordMapCarried), _Alignof(TxnRecordMapCarried));
	frame->result_live = true;
	if (!txn_memtable_table_walked(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, frame->io, frame->each.record,
								   txn_rewrite_spaces_closure_1, &frame->each, NULL, frame->error) ||
		!txn_memtable_table_walked(&TXN_MEMTABLE_MAP_VALUES_TREE_OPS, frame->io, frame->each.record,
								   txn_rewrite_spaces_closure_2, &frame->each, NULL, frame->error))
		return;
	frame->result_live = false;
	frame->ok = true;
}

/* txn_rewrite_spaces_pad frees what spaces holds live. */
static void
txn_rewrite_spaces_pad(void *frame_)
{
	txn_rewrite_spaces_frame_t *frame = frame_;

	if (frame->each.versions_live)
		txn_rewrite_takes_drop(&frame->each.versions);
	if (frame->each.takers_live)
		txn_rewrite_takers_drop(&frame->each.takers);
	if (frame->result_live)
	{
		for (size_t i = 0; i < frame->result->len; i++)
			txn_rewrite_takes_drop(&STD_VEC_AT(frame->result, TxnRecordMapCarried, i).versions);
		std_vec_free_Checked(frame->result, sizeof(TxnRecordMapCarried), _Alignof(TxnRecordMapCarried));
	}
	frame->each.versions_live = frame->each.takers_live = frame->result_live = false;
}

/*
 * txn_rewrite_spaces reads the map versions a move carries: each key's
 * versions in a storage's delta free space map and value map, oldest first,
 * a free space version's with its taker, tuple id, entry, and stored ender,
 * a values mark's with its value ids, and a value map version's with its
 * writer and entry, each key's named for the new storage.
 */
bool
txn_rewrite_spaces(HostIoIo *io, const HostSharedHeld *held, HostStorage storage, StdVec *result, HostError *error)
{
	txn_rewrite_spaces_frame_t frame = {.io = io,
		.each = {.record = host_shared_Held_record(held), .storage = storage, .carried = result}, .result = result,
	.error = error};

	std_landing(&frame, txn_rewrite_spaces_body, txn_rewrite_spaces_pad);
	txn_rewrite_spaces_pad(&frame);
	return frame.ok;
}

/* txn_rewrite_carried_value is a carried map version's step: its entry put, or its removal for none. */
static TxnMemtableMapStep
txn_rewrite_carried_value(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	const TxnRecordCarriedTake *version = captures;

	if (!version->has_value)
		return TXN_MEMTABLE_MAP_STEP_REMOVE;
	std_vec_extend_from_slice_Checked(out, version->value.ptr, version->value.len, 1, 1);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_rewrite_carried_tuple is a carried map version's take's tuple: the tuple id it names. */
static HostRow
txn_rewrite_carried_tuple(void *captures)
{
	return ((const TxnRecordCarriedTake *) captures)->tuple;
}

/* txn_rewrite_nothing is a take's then that does nothing. */
static void
txn_rewrite_nothing(void *captures)
{
}

/* txn_rewrite_spaced_closure_2_captures_t is spaced's prepare's closure's captures. */
typedef struct txn_rewrite_spaced_closure_2_captures_t
{
	const TxnRecordMapCarried *carried;
	const StdVec *body;
	bool		has_replayed;
	uint64		replayed;
} txn_rewrite_spaced_closure_2_captures_t;

/* txn_rewrite_spaced_closure_2_frame_t is the closure's state across its pad: its window, while live. */
typedef struct txn_rewrite_spaced_closure_2_frame_t
{
	txn_rewrite_spaced_closure_2_captures_t *captures;
	HostIoIo   *io;
	TxnMemtableTablePreparedState *prepared;
	HostDelayCheckpoint window;
	bool		window_live;
	HostError  *error;
	bool		ok;
} txn_rewrite_spaced_closure_2_frame_t;

/* txn_rewrite_spaced_closure_2_body appends the record outside redo and pushes each version in one window. */
static void
txn_rewrite_spaced_closure_2_body(void *frame_)
{
	txn_rewrite_spaced_closure_2_frame_t *frame = frame_;
	txn_rewrite_spaced_closure_2_captures_t *c = frame->captures;
	HostIoTakesShort io = HOST_IO_IO_TAKES_SHORT(frame->io);
	bool		has_lsn = c->has_replayed;
	HostLsn		lsn = c->replayed;
	HostRow		unclaimed = {0};
	TxnMemtableMapTaken taken;

	if (!c->has_replayed)
	{
		if (!host_delay_checkpoint(HOST_IO_IO_IO_DOORS(frame->io), NULL, 0, &frame->window, frame->error))
			return;
		frame->window_live = true;
	}
	if (c->body != NULL)
	{
		if (!host_wal_insert(HOST_IO_IO_IO_DOORS(frame->io), HOST_WAL_KIND_MAP_CARRIED, c->body->ptr, c->body->len,
							 &lsn, frame->error))
			return;
		has_lsn = true;
	}
	for (size_t at = 0; at < c->carried->versions.len; at++)
	{
		const TxnRecordCarriedTake *version = &STD_VEC_AT(&c->carried->versions, TxnRecordCarriedTake, at);

		if (version->has_mark)
		{
			StdVec		mark;

			std_slice_to_vec_Checked(&mark, version->mark.ptr, version->mark.len, TXN_REWRITE_ID);
			if (!txn_memtable_map_named(io, frame->prepared, at, version->writer, has_lsn, lsn, version->tuple, &mark,
										frame->error))
				return;
			continue;
		}
		if (!txn_memtable_map_took_Unit(io, frame->prepared, at, version->writer, has_lsn, lsn, false, unclaimed, 0,
										false, NULL, 0, txn_rewrite_carried_value, (void *) version, NULL,
										txn_rewrite_carried_tuple, (void *) version, NULL, txn_rewrite_nothing, NULL,
										NULL, &taken, frame->error))
			return;
	}
	if (frame->window_live)
	{
		frame->window_live = false;
		host_DelayCheckpoint_finish(&frame->window);
	}
	frame->ok = true;
}

/* txn_rewrite_spaced_closure_2_pad drops the closure's window, while live. */
static void
txn_rewrite_spaced_closure_2_pad(void *frame_)
{
	txn_rewrite_spaced_closure_2_frame_t *frame = frame_;

	if (frame->window_live)
		host_DelayCheckpoint_drop(&frame->window);
	frame->window_live = false;
}

/* txn_rewrite_spaced_closure_2 is spaced's prepare's closure. */
static bool
txn_rewrite_spaced_closure_2(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
							 HostError *error)
{
	txn_rewrite_spaced_closure_2_frame_t frame = {.captures = captures, .io = io, .prepared = prepared,
	.error = error};

	std_landing(&frame, txn_rewrite_spaced_closure_2_body, txn_rewrite_spaced_closure_2_pad);
	txn_rewrite_spaced_closure_2_pad(&frame);
	return frame.ok;
}

/* txn_rewrite_spaced_frame_t is spaced's state across its pad: its takes and its record's body, each while live. */
typedef struct txn_rewrite_spaced_frame_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	const TxnRecordMapCarried *carried;
	bool		logged;
	uint16		most;
	bool		has_replayed;
	uint64		replayed;
	bool		takes_live;
	StdVec		takes;
	bool		body_live;
	StdVec		body;
	HostError  *error;
	bool		ok;
} txn_rewrite_spaced_frame_t;

/* txn_rewrite_spaced_body readies a take for each carried version, runs the prepare, and stores each stored ender. */
static void
txn_rewrite_spaced_body(void *frame_)
{
	txn_rewrite_spaced_frame_t *frame = frame_;
	const TxnRecordMapCarried *carried = frame->carried;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	txn_rewrite_spaced_closure_2_captures_t captures = {.carried = carried, .has_replayed = frame->has_replayed,
	.replayed = frame->replayed};
	size_t		most;

	if (frame->has_replayed)
	{
		bool		holds;

		if (!txn_memtable_map_holds(frame->io, record, carried->map, carried->key, frame->replayed, &holds,
									frame->error))
			return;
		if (holds)
		{
			frame->ok = true;
			return;
		}
	}
	most = carried->map == TXN_MEMTABLE_MAP_MAP_FREE ? txn_free_space_most_len(frame->most) : TXN_VALUES_MOST;
	std_vec_with_capacity_Checked(&frame->takes, carried->versions.len, sizeof(TxnMemtableMapTake),
								  _Alignof(TxnMemtableMapTake));
	frame->takes_live = true;
	for (size_t i = 0; i < carried->versions.len; i++)
	{
		const TxnRecordCarriedTake *version = &STD_VEC_AT(&carried->versions, TxnRecordCarriedTake, i);
		TxnMemtableMapTake take = {.state = record, .map = carried->map, .key = carried->key,
		.most = version->has_mark ? txn_memtable_map_named_most(version->mark.len) : most};

		std_vec_push_Checked(&frame->takes, &take, sizeof(take), _Alignof(TxnMemtableMapTake));
	}
	if (frame->logged && !frame->has_replayed)
	{
		txn_record_MapCarried_encode(carried, &frame->body);
		frame->body_live = true;
		captures.body = &frame->body;
	}
	if (!txn_memtable_map_prepare_Unit(frame->io, frame->takes.ptr, frame->takes.len, txn_rewrite_spaced_closure_2,
									   &captures, NULL, frame->error))
		return;
	for (size_t i = 0; i < carried->versions.len; i++)
	{
		const TxnRecordCarriedTake *version = &STD_VEC_AT(&carried->versions, TxnRecordCarriedTake, i);

		if (version->ender != 0 &&
			!txn_memtable_map_ended(frame->io, record, version->tuple, (uint32) version->ender, frame->error))
			return;
	}
	frame->ok = true;
}

/* txn_rewrite_spaced_pad frees what spaced holds live. */
static void
txn_rewrite_spaced_pad(void *frame_)
{
	txn_rewrite_spaced_frame_t *frame = frame_;

	if (frame->body_live)
		std_vec_free_Checked(&frame->body, 1, 1);
	if (frame->takes_live)
		std_vec_free_Checked(&frame->takes, sizeof(TxnMemtableMapTake), _Alignof(TxnMemtableMapTake));
	frame->body_live = frame->takes_live = false;
}

/*
 * txn_rewrite_spaced pushes a key's carried map versions into a moved
 * storage's new storage, in one checkpoint window outside redo: a logged
 * storage's record first, whose end each version keeps as its LSN, or redo's
 * record's end, then each version as its put of its entry, its removal, or
 * its values mark, with its writer and tuple id, and then each take's or
 * mark's stored ender. Redo passes over a key whose chain holds the record's
 * LSN already.
 */
bool
txn_rewrite_spaced(HostIoIo *io, const HostSharedHeld *held, const TxnRecordMapCarried *carried, bool logged,
				   uint16 most, bool has_replayed, uint64 replayed, HostError *error)
{
	txn_rewrite_spaced_frame_t frame = {.io = io, .held = held, .carried = carried, .logged = logged, .most = most,
	.has_replayed = has_replayed, .replayed = replayed, .error = error};

	std_landing(&frame, txn_rewrite_spaced_body, txn_rewrite_spaced_pad);
	txn_rewrite_spaced_pad(&frame);
	return frame.ok;
}
