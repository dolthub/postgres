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
 * VACUUM's freeing of a storage's tuple ids: the tuple ids a take of its
 * delta's free space map took that no layer maps, freed once their takers'
 * transactions have ended with the heap bytes their takes lowered, and
 * logged for a logged storage as a TidsFreed record, whose redo gives them
 * back on a standby and in recovery.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/trees/mutate.h"
#include "doltgres/txn/free_space.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/tuples.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/vacuum.h"
#include "doltgres/txn/values.h"
#include "doltgres/txn/visible.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "vacuum.rs"

/* The map's takes that keep nothing. */
#define TXN_MEMTABLE_MAP_TS Unit
#define TXN_MEMTABLE_MAP_KEPT_OUT
#define TXN_MEMTABLE_MAP_KEPT_IN
#define TXN_MEMTABLE_MAP_DECLARE
#include "doltgres/txn/memtable/map_template.h"

/* The map-only prepare whose closure answers whether it gave back. */
#define TXN_MEMTABLE_MAP_RS Bool
#define TXN_MEMTABLE_MAP_RETURNS bool
#define TXN_MEMTABLE_MAP_RESULT_PARAMS , bool *result, HostError *error
#define TXN_MEMTABLE_MAP_DECLARE
#include "doltgres/txn/memtable/map_template.h"

/* The map-only prepare whose closure returns what its take did. */
#define TXN_MEMTABLE_MAP_RS Taken
#define TXN_MEMTABLE_MAP_RETURNS bool
#define TXN_MEMTABLE_MAP_RESULT_PARAMS , TxnMemtableMapTaken *result, HostError *error
#define TXN_MEMTABLE_MAP_DECLARE
#include "doltgres/txn/memtable/map_template.h"

/* TXN_VACUUM_PAGE_HEADER is the bytes a heap page's header takes, SizeOfPageHeaderData. */
#define TXN_VACUUM_PAGE_HEADER UINT64CONST(24)

/* TXN_VACUUM_ID is the size and alignment of a value id. */
#define TXN_VACUUM_ID sizeof(uint32), _Alignof(uint32)

/* TXN_VACUUM_RUN is the size and alignment of a run of a block's tuple ids. */
#define TXN_VACUUM_RUN sizeof(std_TupleU32U16U16U64_t), _Alignof(std_TupleU32U16U16U64_t)

/*
 * TxnVacuumFound is Found: a tuple id VACUUM finds to free, its heap bytes,
 * its row's ender's full xid, the conflict horizon its free logs, and the
 * value ids its row named that the free takes down.
 */
typedef struct TxnVacuumFound
{
	uint64		f0;
	uint64		f1;
	uint64		f2;
	uint64		f3;
	StdVec		f4;
} TxnVacuumFound;

/* TXN_VACUUM_FOUND is the size and alignment of a found tuple id. */
#define TXN_VACUUM_FOUND sizeof(TxnVacuumFound), _Alignof(TxnVacuumFound)

/*
 * TxnVacuumTook is Took: a taken offset, its tuple id, its take's writer, the
 * bytes the take lowered, the stored ender, and the value ids of the row
 * written at it with the xid whose commit decides them.
 */
typedef struct TxnVacuumTook
{
	uint64		f0;
	TxnMemtableVersionWriter f1;
	uint64		f2;
	uint64		f3;
	StdVec		ids;
	uint64		named;
} TxnVacuumTook;

/* TXN_VACUUM_TOOK is the size and alignment of a taken offset. */
#define TXN_VACUUM_TOOK sizeof(TxnVacuumTook), _Alignof(TxnVacuumTook)

/* txn_vacuum_ids_drop frees value ids. */
static void
txn_vacuum_ids_drop(StdVec *ids)
{
	std_vec_free_Checked(ids, TXN_VACUUM_ID);
}

/* txn_vacuum_found_drop frees found tuple ids. */
static void
txn_vacuum_found_drop(StdVec *found)
{
	for (size_t i = 0; i < found->len; i++)
		txn_vacuum_ids_drop(&STD_VEC_AT(found, TxnVacuumFound, i).f4);
	std_vec_free_Checked(found, TXN_VACUUM_FOUND);
}

/* txn_vacuum_took_drop frees taken offsets. */
static void
txn_vacuum_took_drop(StdVec *took)
{
	for (size_t i = 0; i < took->len; i++)
		txn_vacuum_ids_drop(&STD_VEC_AT(took, TxnVacuumTook, i).ids);
	std_vec_free_Checked(took, TXN_VACUUM_TOOK);
}

/* txn_vacuum_corrupt returns the error of a free space map's entry that doesn't decode. */
static HostError
txn_vacuum_corrupt(void)
{
	StdString	message;

	std_string_from_Checked(&message, "a free space map's entry is corrupt", 35);
	return host_Error_fork((const uint8 *) "XX001", &message);
}

/* txn_vacuum_fork returns an error of a code and a message. */
static HostError
txn_vacuum_fork(const char *code, const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) code, &message);
}

/*
 * txn_vacuum_below returns a block's entry in the base's free space tree,
 * which a take of a block the delta holds no entry for starts from.
 */
static bool
txn_vacuum_below(HostIoIo *io, TreesStoreNodeStore store, const TxnBaseBase *base, uint32 block, bool *some,
				 StdVec *result, HostError *error)
{
	TreesStoreTree tree;
	ChunksErrorError failed;
	uint8		key[4];

	*some = false;
	if (base == NULL || !base->has_free_space)
		return true;
	tree = base->free_space;
	txn_free_space_key(block, key);
	if (trees_get(io, store, TREES_STORE_TREE_NAMED(&tree), key, sizeof(key), some, result, &failed))
		return true;
	*error = txn_base_storage(&failed);
	return false;
}

/*
 * txn_vacuum_first returns a block's free bytes before its free space
 * entry's oldest version in the delta: the base's entry's, else an untouched
 * page's when the base's fresh end doesn't reach into the block, else none.
 */
static bool
txn_vacuum_first(bool has_below, const StdVec *below, uint32 block, const TxnBaseBase *base, const TxnInsertPage *page,
				 uint64 *result, HostError *error)
{
	TxnFreeSpaceFree entry;

	if (has_below)
	{
		if (!txn_free_space_Free_decode(below->ptr, below->len, block, (uint64) block << 16, page->most, &entry,
										error))
			return false;
		*result = entry.bytes;
	}
	else if ((base == NULL ? 1 : base->fresh_end) <= ((uint64) block << 16 | 1))
		*result = page->bytes;
	else
		*result = 0;
	return true;
}

/* txn_vacuum_holds reports whether a row version holds a tuple id. */
static bool
txn_vacuum_holds(TxnMemtableVersionVersion *version, uint64 tid)
{
	TxnVisibleRowVersion row;

	return txn_visible_RowVersion_of(version, &row) && row.put.tuple.f0 == tid;
}

/* txn_vacuum_mapped_frame_t is mapped's state across its pad: the key, its hold, and its versions, each while live. */
typedef struct txn_vacuum_mapped_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	HostSharedRecord record;
	const TreesStoreTree *tuple_ids;
	uint64		tid;
	bool		key_live;
	StdVec		key;
	bool		hold_live;
	HostDeltaHold hold;
	bool		versions_live;
	StdVec		versions;
	bool		value_live;
	StdVec		value;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_vacuum_mapped_frame_t;

/* txn_vacuum_mapped_body looks for the tuple id in its key's chain, then in the base's tuple id tree. */
static void
txn_vacuum_mapped_body(void *frame_)
{
	txn_vacuum_mapped_frame_t *frame = frame_;
	HostRow		tid = {frame->tid};
	ChunksErrorError failed;
	bool		some;

	*frame->result = false;
	if (!txn_memtable_tuples_get(frame->io, frame->record, tid, &some, &frame->key, frame->error))
		return;
	frame->key_live = some;
	if (some)
	{
		if (!txn_memtable_table_pruned(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, frame->record, frame->key.ptr,
									   frame->key.len, false, 0, frame->error) ||
			!host_delta_hold(frame->io, &frame->hold, frame->error))
			return;
		frame->hold_live = true;
		if (!txn_memtable_table_versions(frame->io, &frame->hold, frame->record, frame->key.ptr, frame->key.len,
										 &frame->versions, frame->error))
			return;
		frame->versions_live = true;
		for (size_t i = 0; i < frame->versions.len; i++)
			if (txn_vacuum_holds(&STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, i), frame->tid))
			{
				*frame->result = true;
				frame->ok = true;
				return;
			}
		frame->versions_live = false;
		txn_memtable_version_versions_drop(&frame->versions, 0);
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
		frame->key_live = false;
		std_vec_free_Checked(&frame->key, 1, 1);
	}
	if (frame->tuple_ids != NULL)
	{
		TreesStoreTree tree = *frame->tuple_ids;
		uint8		key[6];

		txn_base_tuple_id_key(frame->tid, key);
		if (!trees_get(frame->io, frame->store, TREES_STORE_TREE_NAMED(&tree), key, sizeof(key), frame->result,
					   &frame->value, &failed))
		{
			*frame->error = txn_base_storage(&failed);
			return;
		}
		frame->value_live = *frame->result;
	}
	frame->ok = true;
}

/* txn_vacuum_mapped_pad frees what mapped holds live. */
static void
txn_vacuum_mapped_pad(void *frame_)
{
	txn_vacuum_mapped_frame_t *frame = frame_;

	if (frame->value_live)
		std_vec_free_Checked(&frame->value, 1, 1);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	frame->value_live = frame->versions_live = frame->hold_live = frame->key_live = false;
}

/*
 * txn_vacuum_mapped reports whether a layer maps a tuple id: a version of the
 * key the delta's tuple id table names for it that holds it, once the key's
 * aborted writers' versions are pruned, or a pair of the base's tuple id
 * tree, NULL for none.
 */
static bool
txn_vacuum_mapped(HostIoIo *io, TreesStoreNodeStore store, HostSharedRecord record, const TreesStoreTree *tuple_ids,
				  uint64 tid, bool *result, HostError *error)
{
	txn_vacuum_mapped_frame_t frame = {.io = io, .store = store, .record = record, .tuple_ids = tuple_ids, .tid = tid,
	.result = result, .error = error};

	std_landing(&frame, txn_vacuum_mapped_body, txn_vacuum_mapped_pad);
	txn_vacuum_mapped_pad(&frame);
	return frame.ok;
}

/* txn_vacuum_tuple_ids returns a base's tuple id tree, NULL for none. */
static const TreesStoreTree *
txn_vacuum_tuple_ids(const TxnBaseBase *base)
{
	return base != NULL && base->has_tuple_ids ? &base->tuple_ids : NULL;
}

/*
 * txn_vacuum_frees reports whether a tuple id's block's newest free space
 * entry, the delta's else a base's, holds its offset free, as VACUUM's free
 * leaves it.
 */
bool
txn_vacuum_frees(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const TxnBaseBase *base,
				 uint64 tid, bool *result, HostError *error)
{
	uint32		block = (uint32) (tid >> 16);
	TxnMemtableMapEntry entry;
	bool		some;
	bool		ok;

	if (!txn_memtable_map_newest(io, host_shared_Held_record(held), TXN_MEMTABLE_MAP_MAP_FREE, block, &some, &entry,
								 error))
		return false;
	if (!some && !txn_vacuum_below(io, store, base, block, &entry.has_value, &entry.value, error))
		return false;
	*result = false;
	if (!entry.has_value)
		return true;
	ok = txn_free_space_frees(entry.value.ptr, entry.value.len, (uint16) (tid & 0xffff), result, error);
	std_vec_free_Checked(&entry.value, 1, 1);
	return ok;
}

/* txn_vacuum_tid_order orders taken offsets, or found tuple ids, by their tuple ids, which lead each. */
static int
txn_vacuum_tid_order(const void *a, const void *b)
{
	uint64		left = *(const uint64 *) a;
	uint64		right = *(const uint64 *) b;

	return left < right ? -1 : left > right;
}

/* txn_vacuum_taken_frame_t is taken's state across its pad: the offsets it found, while live. */
typedef struct txn_vacuum_taken_frame_t
{
	const StdVec *versions;
	bool		has_below;
	const StdVec *below;
	uint32		block;
	const TxnBaseBase *base;
	const TxnInsertPage *page;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} txn_vacuum_taken_frame_t;

/* txn_vacuum_taken_body walks a block's takers oldest first, keeping each offset's newest take. */
static void
txn_vacuum_taken_body(void *frame_)
{
	txn_vacuum_taken_frame_t *frame = frame_;
	StdVec	   *takes = frame->result;
	TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
	bool		has_newest = false;
	TxnFreeSpaceFree newest;
	uint64		before;
	size_t		kept = 0;

	std_vec_new(takes, TXN_VACUUM_TOOK);
	frame->result_live = true;
	if (!txn_vacuum_first(frame->has_below, frame->below, frame->block, frame->base, frame->page, &before,
						  frame->error))
		return;
	for (size_t i = 0; i < frame->versions->len; i++)
	{
		const txn_TupleWriterRowEntryU64TupleVecU32Bool_t *version = &STD_VEC_AT(frame->versions,
																				 txn_TupleWriterRowEntryU64TupleVecU32Bool_t,
																				 i);
		const TxnMemtableVersionWriter *writer = &version->f0;
		uint64		tuple = version->f1.f0;
		uint64		bytes;
		TxnVacuumTook take = {.f0 = tuple, .f1 = *writer, .f3 = version->f3};
		bool		replaced = false;
		TxnVacuumTook old;

		if (version->f4.f1)
		{
			for (size_t at = 0; at < takes->len; at++)
			{
				TxnVacuumTook *found = &STD_VEC_AT(takes, TxnVacuumTook, at);

				if (found->f0 == tuple && txn_memtable_version_Writer_PartialEq_eq(&found->f1, writer))
				{
					StdVec		ids;

					std_slice_to_vec_Checked(&ids, version->f4.f0.ptr, version->f4.f0.len, TXN_VACUUM_ID);
					txn_vacuum_ids_drop(&found->ids);
					found->ids = ids;
					found->named = writer->xid;
					break;
				}
			}
			continue;
		}
		has_newest = version->f2.has_value;
		if (has_newest &&
			!txn_free_space_Free_decode(version->f2.value.ptr, version->f2.value.len, frame->block,
										(uint64) frame->block << 16, frame->page->most, &newest, frame->error))
			return;
		bytes = has_newest ? newest.bytes : 0;
		if (tuple != 0)
		{
			if (tuple >> 16 != (uint64) frame->block || (tuple & 0xffff) == 0)
			{
				*frame->error = txn_vacuum_corrupt();
				return;
			}
			for (size_t at = 0; at < takes->len; at++)
				if (STD_VEC_AT(takes, TxnVacuumTook, at).f0 == tuple)
				{
					std_vec_remove(takes, at, &old, TXN_VACUUM_TOOK, RUST_FILE, 291, 94);
					replaced = true;
					break;
				}
			if (replaced && txn_memtable_version_Writer_PartialEq_eq(writer, &none))
			{
				take.ids = old.ids;
				take.named = old.named;
			}
			else
			{
				if (replaced)
					txn_vacuum_ids_drop(&old.ids);
				std_slice_to_vec_Checked(&take.ids, version->f4.f0.ptr, version->f4.f0.len, TXN_VACUUM_ID);
				take.named = writer->xid;
			}
			take.f2 = before > bytes ? before - bytes : 0;
			std_vec_push_Checked(takes, &take, TXN_VACUUM_TOOK);
		}
		before = bytes;
	}
	for (size_t at = 0; at < takes->len; at++)
	{
		TxnVacuumTook *take = &STD_VEC_AT(takes, TxnVacuumTook, at);

		if (has_newest && txn_free_space_Free_has(&newest, (uint16) (take->f0 & 0xffff)))
			txn_vacuum_ids_drop(&take->ids);
		else
			STD_VEC_AT(takes, TxnVacuumTook, kept++) = *take;
	}
	takes->len = kept;
	if (takes->len > 1)
		qsort(takes->ptr, takes->len, sizeof(TxnVacuumTook), txn_vacuum_tid_order);
	frame->result_live = false;
	frame->ok = true;
}

/* txn_vacuum_taken_pad frees the offsets taken found, unless it answers them. */
static void
txn_vacuum_taken_pad(void *frame_)
{
	txn_vacuum_taken_frame_t *frame = frame_;

	if (frame->result_live)
		txn_vacuum_took_drop(frame->result);
	frame->result_live = false;
}

/*
 * txn_vacuum_taken returns a block's taken offsets as tuple ids from its
 * versions, oldest first, each with the writer, the lowered bytes, the
 * stored ender, and the value ids of the newest take that names it: the
 * version before's bytes less its own. A values mark gives its ids to its
 * writer's take of its tuple id and changes no bytes, and a prune's marker
 * take, which no transaction writes, keeps the ids of the take it replaces.
 * The entry before the first version is the base's, else an untouched
 * page's; a removal leaves none.
 */
static bool
txn_vacuum_taken(const StdVec *versions, bool has_below, const StdVec *below, uint32 block, const TxnBaseBase *base,
				 const TxnInsertPage *page, StdVec *result, HostError *error)
{
	txn_vacuum_taken_frame_t frame = {.versions = versions, .has_below = has_below, .below = below, .block = block,
	.base = base, .page = page, .result = result, .error = error};

	std_landing(&frame, txn_vacuum_taken_body, txn_vacuum_taken_pad);
	txn_vacuum_taken_pad(&frame);
	return frame.ok;
}

/*
 * txn_vacuum_runs splits tuple ids with their bytes, in tuple id order, into
 * each block's runs of consecutive offsets.
 */
static void
txn_vacuum_runs(const TxnVacuumFreed *const *freed, size_t freed_len, StdVec *runs)
{
	std_vec_new(runs, TXN_VACUUM_RUN);
	for (size_t i = 0; i < freed_len; i++)
	{
		uint32		block = (uint32) (freed[i]->f0 >> 16);
		uint16		offset = (uint16) (freed[i]->f0 & 0xffff);
		std_TupleU32U16U16U64_t *last = runs->len > 0 ? &STD_VEC_AT(runs, std_TupleU32U16U16U64_t, runs->len - 1) :
			NULL;

		if (last != NULL && last->f0 == block && STD_ADD(u16, last->f1, last->f2, 355, 68) == offset)
		{
			last->f2 = STD_ADD(u16, last->f2, 1, 356, 17);
			last->f3 = STD_ADD(u64, last->f3, freed[i]->f1, 357, 17);
		}
		else
		{
			std_TupleU32U16U16U64_t run = {block, offset, 1, freed[i]->f1};

			std_vec_push_Checked(runs, &run, TXN_VACUUM_RUN);
		}
	}
}

/*
 * txn_vacuum_BlockStep_t is a block's step's captures: the state, the block,
 * its runs or bytes, and a page's most tuples.
 */
typedef struct txn_vacuum_BlockStep_t
{
	HostSharedRecord record;
	uint32		block;
	const std_TupleU32U16U16U64_t *runs;
	size_t		runs_len;
	uint64		bytes;
	uint16		most;
} txn_vacuum_BlockStep_t;

/* txn_vacuum_entry reads a block's entry inside a step, a new empty one for none, false for one that doesn't read. */
static bool
txn_vacuum_entry(const txn_vacuum_BlockStep_t *step, bool has_current, const uint8 *current, size_t current_len,
				 uint64 counter, TxnFreeSpaceFree *entry)
{
	if (!has_current)
	{
		*entry = txn_free_space_Free_new(0, NULL, 0, step->most);
		return true;
	}
	return txn_free_space_Free_parsed(current, current_len, step->block, counter, step->most, entry);
}

/*
 * txn_vacuum_given_back is a free's step on a block's entry: each run's
 * offsets set free and its bytes given back and the full flag cleared, as
 * heap's prune clears PD_PAGE_FULL; an entry that doesn't read answers
 * Retry, which only corruption causes.
 */
static TxnMemtableMapStep
txn_vacuum_given_back(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	const txn_vacuum_BlockStep_t *step = captures;
	TxnFreeSpaceFree entry;

	if (!txn_vacuum_entry(step, has_current, current, current_len, txn_state_counter(step->record), &entry))
		return TXN_MEMTABLE_MAP_STEP_RETRY;
	for (size_t i = 0; i < step->runs_len; i++)
	{
		const std_TupleU32U16U16U64_t *run = &step->runs[i];

		txn_free_space_Free_give_back(&entry, run->f1,
									  STD_SUB(u16, STD_ADD(u16, run->f1, run->f2, 384, 32), 1, 384, 32), run->f3);
	}
	entry.full = false;
	txn_free_space_Free_encode_into(&entry, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/*
 * txn_vacuum_marked is a dead tuple id's step: the entry's bytes raised by
 * bytes, its offsets as they were, and its full flag cleared, as heap's
 * prune reclaims an LP_DEAD line pointer's space; a block other than the
 * fresh page whose entry has no free offset refuses. An entry that doesn't
 * read answers Retry, which only corruption causes.
 */
static TxnMemtableMapStep
txn_vacuum_marked(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	const txn_vacuum_BlockStep_t *step = captures;
	uint64		counter = txn_state_counter(step->record);
	TxnFreeSpaceFree entry;

	if (!txn_vacuum_entry(step, has_current, current, current_len, counter, &entry))
		return TXN_MEMTABLE_MAP_STEP_RETRY;
	if (txn_free_space_Free_is_clear(&entry) && (uint64) step->block != counter >> 16)
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	entry.bytes = STD_ADD(u64, entry.bytes, step->bytes, 414, 5);
	entry.full = false;
	txn_free_space_Free_encode_into(&entry, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/*
 * txn_vacuum_cleared is a prune's step that found nothing to prune: the
 * entry's full flag cleared and nothing else, an entry gone or already clear
 * taking nothing, and one that doesn't read answering Retry, which only
 * corruption causes.
 */
static TxnMemtableMapStep
txn_vacuum_cleared(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	const txn_vacuum_BlockStep_t *step = captures;
	TxnFreeSpaceFree entry;

	if (!has_current)
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	if (!txn_free_space_Free_parsed(current, current_len, step->block, txn_state_counter(step->record), step->most,
									&entry))
		return TXN_MEMTABLE_MAP_STEP_RETRY;
	if (!entry.full)
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	entry.full = false;
	txn_free_space_Free_encode_into(&entry, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_vacuum_taken_down is a value's step, values' take down by its captured count. */
static TxnMemtableMapStep
txn_vacuum_taken_down(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	return txn_values_taken_down(has_current, current, current_len, out, *(const uint64 *) captures);
}

/* txn_vacuum_nothing is a take's then that does nothing. */
static void
txn_vacuum_nothing(void *captures)
{
}

/*
 * txn_vacuum_block_frame_t is block_never_held's state across its pad: the
 * base entry, the takers, and the offsets, each while live.
 */
typedef struct txn_vacuum_block_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	HostSharedRecord record;
	const TxnBaseBase *base;
	uint32		block;
	const TxnInsertPage *page;
	bool		has_below;
	StdVec		below;
	bool		takers_live;
	StdVec		takers;
	bool		took_live;
	StdVec		took;
	bool		free_live;
	StdVec		free;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} txn_vacuum_block_frame_t;

/* txn_vacuum_block_reread drops the block's takers and offsets, then reads them again. */
static bool
txn_vacuum_block_reread(txn_vacuum_block_frame_t *frame)
{
	if (frame->took_live)
		txn_vacuum_took_drop(&frame->took);
	if (frame->takers_live)
		txn_memtable_map_takers_drop(&frame->takers);
	frame->took_live = frame->takers_live = false;
	if (!txn_memtable_map_takers(frame->io, frame->record, TXN_MEMTABLE_MAP_MAP_FREE, frame->block, &frame->takers,
								 frame->error))
		return false;
	frame->takers_live = true;
	return true;
}

/* txn_vacuum_contains reports whether tuple ids hold one. */
static bool
txn_vacuum_contains(const StdVec *tids, uint64 tid)
{
	for (size_t i = 0; i < tids->len; i++)
		if (STD_VEC_AT(tids, uint64, i) == tid)
			return true;
	return false;
}

/*
 * txn_vacuum_block_body finds the block's taken offsets no running taker or
 * layer holds, then reads them again for their parts.
 */
static void
txn_vacuum_block_body(void *frame_)
{
	txn_vacuum_block_frame_t *frame = frame_;

	std_vec_new(frame->result, TXN_VACUUM_FOUND);
	frame->result_live = true;
	if (!txn_vacuum_below(frame->io, frame->store, frame->base, frame->block, &frame->has_below, &frame->below,
						  frame->error) ||
		!txn_vacuum_block_reread(frame) ||
		!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-vacuum-found", frame->error))
		return;
	if (!txn_vacuum_taken(&frame->takers, frame->has_below, &frame->below, frame->block, frame->base, frame->page,
						  &frame->took, frame->error))
		return;
	frame->took_live = true;
	std_vec_new(&frame->free, sizeof(uint64), _Alignof(uint64));
	frame->free_live = true;
	for (size_t i = 0; i < frame->took.len; i++)
	{
		const TxnVacuumTook *take = &STD_VEC_AT(&frame->took, TxnVacuumTook, i);
		HostXid		top = {take->f1.has_top ? take->f1.top : take->f1.xid};
		bool		running;
		bool		mapped;

		if (!host_in_progress(HOST_IO_IO_IO_DOORS(frame->io), top, &running, frame->error))
			return;
		if (running)
			continue;
		if (!txn_vacuum_mapped(frame->io, frame->store, frame->record, txn_vacuum_tuple_ids(frame->base), take->f0,
							   &mapped, frame->error))
			return;
		if (!mapped)
			std_vec_push_Checked(&frame->free, &take->f0, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->free.len > 0)
	{
		if (!txn_vacuum_block_reread(frame) ||
			!txn_vacuum_taken(&frame->takers, frame->has_below, &frame->below, frame->block, frame->base,
							  frame->page, &frame->took, frame->error))
			return;
		frame->took_live = true;
		for (size_t i = 0; i < frame->took.len; i++)
		{
			TxnVacuumTook *take = &STD_VEC_AT(&frame->took, TxnVacuumTook, i);
			TxnVacuumFound found = {.f0 = take->f0, .f1 = take->f2, .f2 = take->f3,
			.f3 = take->f3 == take->f1.xid ? 0 : take->f3};
			HostStatus	status = HOST_STATUS_ABORTED;

			if (!txn_vacuum_contains(&frame->free, take->f0))
				continue;
			if (take->ids.len > 0)
			{
				HostXid		named = {take->named};

				if (!host_status(HOST_IO_IO_IO_DOORS(frame->io), named, &status, frame->error))
					return;
			}
			if (status == HOST_STATUS_COMMITTED)
			{
				found.f4 = take->ids;
				std_vec_new(&take->ids, TXN_VACUUM_ID);
			}
			else
				std_vec_new(&found.f4, TXN_VACUUM_ID);
			std_vec_push_Checked(frame->result, &found, TXN_VACUUM_FOUND);
		}
	}
	frame->result_live = false;
	frame->ok = true;
}

/* txn_vacuum_block_pad frees what block_never_held holds live. */
static void
txn_vacuum_block_pad(void *frame_)
{
	txn_vacuum_block_frame_t *frame = frame_;

	if (frame->free_live)
		std_vec_free_Checked(&frame->free, sizeof(uint64), _Alignof(uint64));
	if (frame->took_live)
		txn_vacuum_took_drop(&frame->took);
	if (frame->takers_live)
		txn_memtable_map_takers_drop(&frame->takers);
	if (frame->has_below)
		std_vec_free_Checked(&frame->below, 1, 1);
	if (frame->result_live)
		txn_vacuum_found_drop(frame->result);
	frame->free_live = frame->took_live = frame->takers_live = frame->has_below = frame->result_live = false;
}

/*
 * txn_vacuum_block_never_held returns the tuple ids of one block that
 * never_held finds, with the heap bytes each take lowered, the ender a fold's
 * drop stored, 0 for none, and the conflict horizon its free logs: that
 * ender, 0 for the take's own writer.
 */
static bool
txn_vacuum_block_never_held(HostIoIo *io, TreesStoreNodeStore store, HostSharedRecord record, const TxnBaseBase *base,
							uint32 block, const TxnInsertPage *page, StdVec *result, HostError *error)
{
	txn_vacuum_block_frame_t frame = {.io = io, .store = store, .record = record, .base = base, .block = block,
	.page = page, .result = result, .error = error};

	std_landing(&frame, txn_vacuum_block_body, txn_vacuum_block_pad);
	txn_vacuum_block_pad(&frame);
	return frame.ok;
}

/*
 * TxnVacuumPending is a base's pending tuple id as its tree holds it: the
 * tuple id, its ender, its bytes, and its value ids.
 */
typedef struct TxnVacuumPending
{
	uint64		tid;
	uint64		ender;
	uint32		bytes;
	StdVec		ids;
} TxnVacuumPending;

/* TXN_VACUUM_PENDING is the size and alignment of a base's pending tuple id. */
#define TXN_VACUUM_PENDING sizeof(TxnVacuumPending), _Alignof(TxnVacuumPending)

/* txn_vacuum_pending_drop frees a base's pending tuple ids. */
static void
txn_vacuum_pending_drop(StdVec *pending)
{
	for (size_t i = 0; i < pending->len; i++)
		txn_vacuum_ids_drop(&STD_VEC_AT(pending, TxnVacuumPending, i).ids);
	std_vec_free_Checked(pending, TXN_VACUUM_PENDING);
}

/*
 * txn_vacuum_pending_closure_captures_t is a pending tree's pin scope's
 * captures: the tree, a block or none, and the result.
 */
typedef struct txn_vacuum_pending_closure_captures_t
{
	TreesStoreTree tree;
	bool		in_block;
	uint32		block;
	StdVec	   *result;
} txn_vacuum_pending_closure_captures_t;

/*
 * txn_vacuum_pending_closure_frame_t is the pin scope's state across its pad:
 * its cursor and the entry it reads, each while live.
 */
typedef struct txn_vacuum_pending_closure_frame_t
{
	txn_vacuum_pending_closure_captures_t *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesCursorCursor cursor;
	bool		cursor_live;
	TxnVacuumPending entry;
	bool		entry_live;
	ChunksErrorError *error;
	bool		ok;
} txn_vacuum_pending_closure_frame_t;

/* txn_vacuum_host_failed returns a host error as chunks' error of it. */
static void
txn_vacuum_host_failed(ChunksErrorError *error, HostError failed)
{
	error->tag = CHUNKS_ERROR_ERROR_HOST;
	error->u.host = failed;
}

/* txn_vacuum_pending_closure_body walks the pending tree, all of it, or from a block's first tuple id to its last. */
static void
txn_vacuum_pending_closure_body(void *frame_)
{
	txn_vacuum_pending_closure_frame_t *frame = frame_;
	txn_vacuum_pending_closure_captures_t *captures = frame->captures;
	TreesKeyOrder bytes = {.tag = TREES_KEY_ORDER_BYTES};
	HostError	failed;
	uint8		from[6];

	if (captures->in_block)
	{
		txn_base_tuple_id_key((uint64) captures->block << 16, from);
		if (!trees_cursor_Cursor_seek(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&captures->tree), from,
									  sizeof(from), &bytes, &frame->cursor, frame->error))
			return;
	}
	else if (!trees_cursor_Cursor_first(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&captures->tree),
										&frame->cursor, frame->error))
		return;
	frame->cursor_live = true;
	while (trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		StdBytes	key = trees_cursor_Cursor_key(&frame->cursor);
		StdBytes	value = trees_cursor_Cursor_value(&frame->cursor);

		if (!txn_base_tuple_id_of(key.ptr, key.len, &frame->entry.tid, &failed))
		{
			txn_vacuum_host_failed(frame->error, failed);
			return;
		}
		if (captures->in_block)
		{
			if (frame->entry.tid >> 16 != (uint64) captures->block)
				break;
			std_vec_push_Checked(captures->result, &frame->entry.tid, sizeof(uint64), _Alignof(uint64));
		}
		else
		{
			if (!txn_base_pending_read(value.ptr, value.len, &frame->entry.ender, &frame->entry.bytes,
									   &frame->entry.ids, &failed))
			{
				txn_vacuum_host_failed(frame->error, failed);
				return;
			}
			frame->entry_live = true;
			std_vec_push_Checked(captures->result, &frame->entry, TXN_VACUUM_PENDING);
			frame->entry_live = false;
		}
		if (!trees_cursor_Cursor_next(&frame->cursor, frame->io, frame->pin, frame->error))
			return;
	}
	frame->ok = true;
}

/* txn_vacuum_pending_closure_pad frees what the pin scope holds live. */
static void
txn_vacuum_pending_closure_pad(void *frame_)
{
	txn_vacuum_pending_closure_frame_t *frame = frame_;

	if (frame->entry_live)
		txn_vacuum_ids_drop(&frame->entry.ids);
	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
	frame->entry_live = frame->cursor_live = false;
}

/* txn_vacuum_pending_closure is pended's and pending_in's pin scope: the pending tree's entries it reads. */
static bool
txn_vacuum_pending_closure(void *captures, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	txn_vacuum_pending_closure_frame_t frame = {.captures = captures, .io = io, .pin = pin, .error = error};

	std_landing(&frame, txn_vacuum_pending_closure_body, txn_vacuum_pending_closure_pad);
	txn_vacuum_pending_closure_pad(&frame);
	return frame.ok;
}

/* txn_vacuum_pended_closure_0 is pended's pin scope: every entry of the pending tree. */
static bool
txn_vacuum_pended_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	return txn_vacuum_pending_closure(captures, io, pin, error);
}

/* txn_vacuum_pending_in_closure_0 is pending_in's pin scope: one block's tuple ids of the pending tree. */
static bool
txn_vacuum_pending_in_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	return txn_vacuum_pending_closure(captures, io, pin, error);
}

/* txn_vacuum_pending_read reads a pending tree's entries, all of them, or one block's tuple ids alone. */
static bool
txn_vacuum_pending_read(HostIoIo *io, TreesStoreNodeStore store, TreesStoreTree tree, bool in_block, uint32 block,
						StdVec *result, HostError *error)
{
	txn_vacuum_pending_closure_captures_t captures = {.tree = tree, .in_block = in_block, .block = block,
	.result = result};
	ChunksErrorError failed;

	if (trees_pin_pin_ResultUnit(io, store, in_block ? txn_vacuum_pending_in_closure_0 : txn_vacuum_pended_closure_0,
								 &captures, NULL, &failed))
		return true;
	*error = txn_base_storage(&failed);
	return false;
}

/* txn_vacuum_pending_in returns the tuple ids of one block that a base's pending tree holds, in order. */
static bool
txn_vacuum_pending_in(HostIoIo *io, TreesStoreNodeStore store, TreesStoreTree tree, uint32 block, StdVec *result,
					  HostError *error)
{
	return txn_vacuum_pending_read(io, store, tree, true, block, result, error);
}

/* txn_vacuum_pended_frame_t is pended's state across its pad: the pending entries it read, while live. */
typedef struct txn_vacuum_pended_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const TxnBaseBase *base;
	StdVec	   *found;
	StdVec		pending;
	bool		pending_live;
	size_t		at;
	HostError  *error;
	bool		ok;
} txn_vacuum_pended_frame_t;

/*
 * txn_vacuum_pended_body adds each pending tuple id no layer maps and the map
 * doesn't hold free, or merges it into a found one.
 */
static void
txn_vacuum_pended_body(void *frame_)
{
	txn_vacuum_pended_frame_t *frame = frame_;

	std_vec_new(&frame->pending, TXN_VACUUM_PENDING);
	frame->pending_live = true;
	if (!txn_vacuum_pending_read(frame->io, frame->store, frame->base->pending, false, 0, &frame->pending,
								 frame->error))
		return;
	for (; frame->at < frame->pending.len; frame->at++)
	{
		TxnVacuumPending *pending = &STD_VEC_AT(&frame->pending, TxnVacuumPending, frame->at);
		TxnVacuumFound *merged = NULL;
		TxnVacuumFound found;
		bool		skip;

		for (size_t i = 0; i < frame->found->len && merged == NULL; i++)
			if (STD_VEC_AT(frame->found, TxnVacuumFound, i).f0 == pending->tid)
				merged = &STD_VEC_AT(frame->found, TxnVacuumFound, i);
		if (merged != NULL)
		{
			merged->f2 = Max(merged->f2, pending->ender);
			merged->f3 = Max(merged->f3, pending->ender);
			if (merged->f4.len == 0)
			{
				txn_vacuum_ids_drop(&merged->f4);
				merged->f4 = pending->ids;
				std_vec_new(&pending->ids, TXN_VACUUM_ID);
			}
			continue;
		}
		if (!txn_vacuum_mapped(frame->io, frame->store, host_shared_Held_record(frame->held),
							   txn_vacuum_tuple_ids(frame->base), pending->tid, &skip, frame->error))
			return;
		if (!skip && !txn_vacuum_frees(frame->io, frame->store, frame->held, frame->base, pending->tid, &skip,
									   frame->error))
			return;
		if (skip)
			continue;
		found.f0 = pending->tid;
		found.f1 = pending->bytes;
		found.f2 = pending->ender;
		found.f3 = pending->ender;
		found.f4 = pending->ids;
		std_vec_push_Checked(frame->found, &found, TXN_VACUUM_FOUND);
		std_vec_new(&pending->ids, TXN_VACUUM_ID);
	}
	if (frame->found->len > 1)
		qsort(frame->found->ptr, frame->found->len, sizeof(TxnVacuumFound), txn_vacuum_tid_order);
	frame->ok = true;
}

/* txn_vacuum_pended_pad frees the pending entries pended read. */
static void
txn_vacuum_pended_pad(void *frame_)
{
	txn_vacuum_pended_frame_t *frame = frame_;

	if (frame->pending_live)
		txn_vacuum_pending_drop(&frame->pending);
	frame->pending_live = false;
}

/*
 * txn_vacuum_pended adds to VACUUM's found tuple ids those of a base's
 * pending tree, each with its heap bytes and its row's ender's full xid,
 * that no layer maps and that the free space map doesn't already hold free,
 * so a tuple id is freed once; one the delta's takes found already keeps the
 * larger of the two enders, and its take's value ids, taking the entry's
 * only when the take holds none. It keeps the found tuple ids in order.
 */
static bool
txn_vacuum_pended(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const TxnBaseBase *base,
				  StdVec *found, HostError *error)
{
	txn_vacuum_pended_frame_t frame = {.io = io, .store = store, .held = held, .base = base, .found = found,
	.error = error};

	std_landing(&frame, txn_vacuum_pended_body, txn_vacuum_pended_pad);
	txn_vacuum_pended_pad(&frame);
	return frame.ok;
}

/*
 * txn_vacuum_never_held_frame_t is never_held's state across its pad: the
 * base, a block's found tuple ids, and all found.
 */
typedef struct txn_vacuum_never_held_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const TxnInsertPage *page;
	const HostSnapshot *horizon;
	bool		base_live;
	TxnBaseBase base;
	bool		block_live;
	StdVec		block;
	bool		found_live;
	StdVec		found;
	size_t		at;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} txn_vacuum_never_held_frame_t;

/* txn_vacuum_never_held_base reads the state's base record again. */
static bool
txn_vacuum_never_held_base(txn_vacuum_never_held_frame_t *frame)
{
	bool		some;

	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->base_live = false;
	if (!txn_state_base_record(frame->io, frame->held, &some, &frame->base, frame->error))
		return false;
	frame->base_live = some;
	return true;
}

/*
 * txn_vacuum_never_held_body walks the map's blocks under each base, then the
 * base's pending tuple ids, then the horizon.
 */
static void
txn_vacuum_never_held_body(void *frame_)
{
	txn_vacuum_never_held_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	uint64		generation = txn_state_generation(record);
	bool		has_from = true;
	uint32		from = 0;

	std_vec_new(&frame->found, TXN_VACUUM_FOUND);
	frame->found_live = true;
	if (!txn_vacuum_never_held_base(frame))
		return;
	while (has_from)
	{
		txn_TupleU32Entry_t seek;
		bool		some;
		uint64		moved;

		if (!txn_memtable_map_seek(frame->io, record, TXN_MEMTABLE_MAP_MAP_FREE, from, &some, &seek, frame->error))
			return;
		if (!some)
			break;
		if (seek.f1.has_value)
			std_vec_free_Checked(&seek.f1.value, 1, 1);
		if (!txn_vacuum_block_never_held(frame->io, frame->store, record, frame->base_live ? &frame->base : NULL,
										 seek.f0, frame->page, &frame->block, frame->error))
			return;
		frame->block_live = true;
		moved = txn_state_generation(record);
		if (moved != generation)
		{
			generation = moved;
			frame->block_live = false;
			txn_vacuum_found_drop(&frame->block);
			if (!txn_vacuum_never_held_base(frame))
				return;
			continue;
		}
		for (size_t i = 0; i < frame->block.len; i++)
			std_vec_push_Checked(&frame->found, &STD_VEC_AT(&frame->block, TxnVacuumFound, i), TXN_VACUUM_FOUND);
		frame->block.len = 0;
		frame->block_live = false;
		std_vec_free_Checked(&frame->block, TXN_VACUUM_FOUND);
		has_from = seek.f0 != PG_UINT32_MAX;
		from = seek.f0 + (has_from ? 1 : 0);
	}
	if (frame->base_live && frame->base.has_pending &&
		!txn_vacuum_pended(frame->io, frame->store, frame->held, &frame->base, &frame->found, frame->error))
		return;
	std_vec_with_capacity_Checked(frame->result, frame->found.len, sizeof(TxnVacuumFreed), _Alignof(TxnVacuumFreed));
	frame->result_live = true;
	for (; frame->at < frame->found.len; frame->at++)
	{
		TxnVacuumFound *found = &STD_VEC_AT(&frame->found, TxnVacuumFound, frame->at);
		HostXid		ender = {found->f2};
		bool		removable = found->f2 == 0;

		if (!removable && !host_removable_in(HOST_IO_IO_IO_DOORS(frame->io), frame->horizon, ender, &removable,
											 frame->error))
			return;
		if (removable)
		{
			TxnVacuumFreed passed = {found->f0, found->f1, found->f3, found->f4};

			std_vec_new(&found->f4, TXN_VACUUM_ID);
			std_vec_push_Checked(frame->result, &passed, sizeof(passed), _Alignof(TxnVacuumFreed));
		}
	}
	frame->result_live = false;
	frame->ok = true;
}

/* txn_vacuum_freed_drop frees tuple ids never_held answers. */
static void
txn_vacuum_freed_drop(StdVec *freed)
{
	for (size_t i = 0; i < freed->len; i++)
		txn_vacuum_ids_drop(&STD_VEC_AT(freed, TxnVacuumFreed, i).f3);
	std_vec_free_Checked(freed, sizeof(TxnVacuumFreed), _Alignof(TxnVacuumFreed));
}

/* txn_vacuum_never_held_pad frees what never_held holds live. */
static void
txn_vacuum_never_held_pad(void *frame_)
{
	txn_vacuum_never_held_frame_t *frame = frame_;

	if (frame->result_live)
		txn_vacuum_freed_drop(frame->result);
	if (frame->found_live)
		txn_vacuum_found_drop(&frame->found);
	if (frame->block_live)
		txn_vacuum_found_drop(&frame->block);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->result_live = frame->found_live = frame->block_live = frame->base_live = false;
}

/*
 * txn_vacuum_never_held returns each tuple id of a storage that a take of
 * its delta's free space map took, that no layer maps, and whose taker's
 * top-level transaction is no longer in progress, with the heap bytes its
 * take lowered and its free's conflict horizon, in tuple id order. A block
 * whose checks a fold's install met, moving the base generation, is found
 * again under the newest base. The base's pending tuple ids follow. A tuple
 * id whose row a transaction ended is left until the horizon, a
 * NonVacuumable snapshot, has passed that ender.
 */
bool
txn_vacuum_never_held(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const TxnInsertPage *page,
					  const HostSnapshot *horizon, StdVec *result, HostError *error)
{
	txn_vacuum_never_held_frame_t frame = {.io = io, .store = store, .held = held, .page = page, .horizon = horizon,
	.result = result, .error = error};

	std_landing(&frame, txn_vacuum_never_held_body, txn_vacuum_never_held_pad);
	txn_vacuum_never_held_pad(&frame);
	return frame.ok;
}

/*
 * txn_vacuum_spent_frame_t is spent's state across its pad: the base, the
 * block's found tuple ids, and its pending ones.
 */
typedef struct txn_vacuum_spent_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	uint32		block;
	const TxnInsertPage *page;
	bool		base_live;
	TxnBaseBase base;
	bool		taken_live;
	StdVec		taken;
	bool		pending_live;
	StdVec		pending;
	uint64	   *result;
	HostError  *error;
	bool		ok;
} txn_vacuum_spent_frame_t;

/* txn_vacuum_spent_body counts the block's never-held tuple ids, then its pending ones no layer maps or map frees. */
static void
txn_vacuum_spent_body(void *frame_)
{
	txn_vacuum_spent_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	bool		some;

	if (!txn_state_base_record(frame->io, frame->held, &some, &frame->base, frame->error))
		return;
	frame->base_live = some;
	if (!txn_vacuum_block_never_held(frame->io, frame->store, record, some ? &frame->base : NULL, frame->block,
									 frame->page, &frame->taken, frame->error))
		return;
	frame->taken_live = true;
	*frame->result = (uint64) frame->taken.len;
	if (some && frame->base.has_pending)
	{
		std_vec_new(&frame->pending, sizeof(uint64), _Alignof(uint64));
		frame->pending_live = true;
		if (!txn_vacuum_pending_in(frame->io, frame->store, frame->base.pending, frame->block, &frame->pending,
								   frame->error))
			return;
		for (size_t i = 0; i < frame->pending.len; i++)
		{
			uint64		tid = STD_VEC_AT(&frame->pending, uint64, i);
			bool		taken = false;
			bool		skip;

			for (size_t at = 0; at < frame->taken.len && !taken; at++)
				taken = STD_VEC_AT(&frame->taken, TxnVacuumFound, at).f0 == tid;
			if (taken)
				continue;
			if (!txn_vacuum_mapped(frame->io, frame->store, record, txn_vacuum_tuple_ids(&frame->base), tid, &skip,
								   frame->error))
				return;
			if (!skip && !txn_vacuum_frees(frame->io, frame->store, frame->held, &frame->base, tid, &skip,
										   frame->error))
				return;
			if (!skip)
				*frame->result = STD_ADD(u64, *frame->result, 1, 189, 17);
		}
	}
	frame->ok = true;
}

/* txn_vacuum_spent_pad frees what spent holds live. */
static void
txn_vacuum_spent_pad(void *frame_)
{
	txn_vacuum_spent_frame_t *frame = frame_;

	if (frame->pending_live)
		std_vec_free_Checked(&frame->pending, sizeof(uint64), _Alignof(uint64));
	if (frame->taken_live)
		txn_vacuum_found_drop(&frame->taken);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->pending_live = frame->taken_live = frame->base_live = false;
}

/*
 * txn_vacuum_spent counts a block's spent places, which ANALYZE counts dead
 * as heap counts an aborted insert's tuples and an ended row's until VACUUM
 * frees them: the tuple ids of the block's delta takes never_held finds, and
 * its base's pending tuple ids that no layer maps and the free space map
 * doesn't hold free.
 */
bool
txn_vacuum_spent(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 block,
				 const TxnInsertPage *page, uint64 *result, HostError *error)
{
	txn_vacuum_spent_frame_t frame = {.io = io, .store = store, .held = held, .block = block, .page = page,
	.result = result, .error = error};

	std_landing(&frame, txn_vacuum_spent_body, txn_vacuum_spent_pad);
	txn_vacuum_spent_pad(&frame);
	return frame.ok;
}

/*
 * TXN_VACUUM_CLAIM is the size and alignment of a dead tuple id's claim: its
 * tuple id and whether its marker claimed it.
 */
#define TXN_VACUUM_CLAIM sizeof(txn_vacuum_TupleU64Bool_t), _Alignof(txn_vacuum_TupleU64Bool_t)

/* txn_vacuum_TupleU64Bool_t is a dead tuple id and whether its marker claimed it. */
typedef struct txn_vacuum_TupleU64Bool_t
{
	uint64		f0;
	bool		f1;
} txn_vacuum_TupleU64Bool_t;

/*
 * TxnVacuumDown is a value id a free takes down: its id, the count it takes
 * down by, and its entry below the memtable.
 */
typedef struct TxnVacuumDown
{
	uint32		id;
	uint64		by;
	bool		has_based;
	StdVec		based;
} TxnVacuumDown;

/* TXN_VACUUM_DOWN is the size and alignment of a value id a free takes down. */
#define TXN_VACUUM_DOWN sizeof(TxnVacuumDown), _Alignof(TxnVacuumDown)

/* txn_vacuum_downs_drop frees value ids a free takes down. */
static void
txn_vacuum_downs_drop(StdVec *downs)
{
	for (size_t i = 0; i < downs->len; i++)
	{
		TxnVacuumDown *down = &STD_VEC_AT(downs, TxnVacuumDown, i);

		if (down->has_based)
			std_vec_free_Checked(&down->based, 1, 1);
	}
	std_vec_free_Checked(downs, TXN_VACUUM_DOWN);
}

/* txn_vacuum_marker_tuple is a marker's take's tuple: the dead tuple id its captures hold. */
static HostRow
txn_vacuum_marker_tuple(void *captures)
{
	HostRow		tuple = {*(const uint64 *) captures};

	return tuple;
}

/*
 * txn_vacuum_given_closure_2_captures_t is given's prepare's closure's
 * captures: the block's take's parts, its runs and markers, the values it
 * takes down, and how its record is logged.
 */
typedef struct txn_vacuum_given_closure_2_captures_t
{
	HostSharedRecord record;
	uint32		block;
	bool		has_below;
	const StdVec *below;
	uint64		start;
	const TxnInsertPage *page;
	const StdVec *freed;
	const StdVec *markers;
	StdVec	   *claims;

	/*
	 * toast is the TOAST storage's state, set only when the free takes values
	 * down.
	 */
	const HostSharedRecord *toast;
	const StdVec *downs;
	std_TupleU64U64_t mark;

	/*
	 * windowed is whether the record is appended in a window the closure
	 * opens, with body.
	 */
	bool		windowed;
	const StdVec *body;
	bool		has_lsn;
	uint64		lsn;
} txn_vacuum_given_closure_2_captures_t;

/* txn_vacuum_given_closure_2_frame_t is the closure's state across its pad: its window, while live. */
typedef struct txn_vacuum_given_closure_2_frame_t
{
	txn_vacuum_given_closure_2_captures_t *captures;
	HostIoIo   *io;
	TxnMemtableTablePreparedState *prepared;
	HostDelayCheckpoint window;
	bool		window_live;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_vacuum_given_closure_2_frame_t;

/*
 * txn_vacuum_given_closure_2_body checks the values mark, logs the record, and
 * makes the block's and the values' takes.
 */
static void
txn_vacuum_given_closure_2_body(void *frame_)
{
	txn_vacuum_given_closure_2_frame_t *frame = frame_;
	txn_vacuum_given_closure_2_captures_t *c = frame->captures;
	HostIoTakesShort io = HOST_IO_IO_TAKES_SHORT(frame->io);
	TxnMemtableVersionWriter wrote = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
	static const char *const pauses[] = {"doltgres-vacuum-logged"};
	bool		has_lsn = c->has_lsn;
	HostLsn		lsn = c->lsn;
	size_t		freeing = c->freed->len > 0;
	TxnMemtableMapTaken taken;

	if (c->toast != NULL)
	{
		std_TupleU64U64_t mark = txn_values_mark(*c->toast);

		if (mark.f0 != c->mark.f0 || mark.f1 != c->mark.f1)
		{
			*frame->result = false;
			frame->ok = true;
			return;
		}
	}
	if (c->windowed)
	{
		if (!host_delay_checkpoint(HOST_IO_IO_IO_DOORS(frame->io), pauses, lengthof(pauses), &frame->window,
								   frame->error))
			return;
		frame->window_live = true;
		if (c->body != NULL)
		{
			if (!host_wal_insert(HOST_IO_IO_IO_DOORS(frame->io), HOST_WAL_KIND_TIDS_FREED, c->body->ptr, c->body->len,
								 &lsn, frame->error))
				return;
			has_lsn = true;
		}
	}
	if (c->toast != NULL && !host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-vacuum-logged", frame->error))
		return;
	if (freeing)
	{
		txn_vacuum_BlockStep_t step = {.record = c->record, .block = c->block, .runs = c->freed->ptr,
		.runs_len = c->freed->len, .most = c->page->most};

		if (!txn_memtable_map_finish_Unit(io, frame->prepared, 0, wrote, has_lsn, lsn, c->has_below, c->below->ptr,
										  c->below->len, txn_vacuum_given_back, &step, NULL, txn_vacuum_nothing, NULL,
										  NULL, &taken, frame->error))
			return;
		if (taken != TXN_MEMTABLE_MAP_TAKEN_TOOK)
		{
			*frame->error = txn_vacuum_corrupt();
			return;
		}
	}
	for (size_t at = 0; at < c->markers->len; at++)
	{
		const std_TupleU64U64_t *marker = &STD_VEC_AT(c->markers, std_TupleU64U64_t, at);
		txn_vacuum_BlockStep_t step = {.record = c->record, .block = c->block, .bytes = marker->f1,
		.most = c->page->most};
		HostRow		tid = {marker->f0};

		if (!txn_memtable_map_took_Unit(io, frame->prepared, freeing + at, wrote, has_lsn, lsn, true, tid, c->start,
										c->has_below, c->below->ptr, c->below->len, txn_vacuum_marked, &step, NULL,
										txn_vacuum_marker_tuple, (void *) &marker->f0, NULL, txn_vacuum_nothing, NULL,
										NULL, &taken,
										frame->error))
			return;
		if (taken == TXN_MEMTABLE_MAP_TAKEN_TOOK)
			STD_VEC_AT(c->claims, txn_vacuum_TupleU64Bool_t, at).f1 = true;
		else if (taken == TXN_MEMTABLE_MAP_TAKEN_RETRY)
		{
			*frame->error = txn_vacuum_corrupt();
			return;
		}
	}
	for (size_t at = 0; c->toast != NULL && at < c->downs->len; at++)
	{
		TxnVacuumDown *down = &STD_VEC_AT(c->downs, TxnVacuumDown, at);

		if (!txn_memtable_map_finish_Unit(io, frame->prepared, freeing + c->markers->len + at, wrote, has_lsn, lsn,
										  down->has_based, down->based.ptr, down->based.len, txn_vacuum_taken_down,
										  &down->by, NULL, txn_vacuum_nothing, NULL, NULL, &taken, frame->error))
			return;
		if (taken != TXN_MEMTABLE_MAP_TAKEN_TOOK)
		{
			*frame->error = txn_vacuum_fork("XX001", "a freed row's value has no entry to take down");
			return;
		}
	}
	if (frame->window_live)
	{
		frame->window_live = false;
		host_DelayCheckpoint_finish(&frame->window);
	}
	*frame->result = true;
	frame->ok = true;
}

/* txn_vacuum_given_closure_2_pad drops the closure's window, while live. */
static void
txn_vacuum_given_closure_2_pad(void *frame_)
{
	txn_vacuum_given_closure_2_frame_t *frame = frame_;

	if (frame->window_live)
		host_DelayCheckpoint_drop(&frame->window);
	frame->window_live = false;
}

/* txn_vacuum_given_closure_2 is given's prepare's closure. */
static bool
txn_vacuum_given_closure_2(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared, bool *result,
						   HostError *error)
{
	txn_vacuum_given_closure_2_frame_t frame = {.captures = captures, .io = io, .prepared = prepared,
	.result = result, .error = error};

	std_landing(&frame, txn_vacuum_given_closure_2_body, txn_vacuum_given_closure_2_pad);
	txn_vacuum_given_closure_2_pad(&frame);
	return frame.ok;
}

/* txn_vacuum_given_frame_t is given's state across its pad: its markers, takes, and claims, each while live. */
typedef struct txn_vacuum_given_frame_t
{
	HostIoIo   *io;
	txn_vacuum_given_closure_2_captures_t captures;
	const StdVec *dead;
	bool		markers_live;
	StdVec		markers;
	bool		takes_live;
	StdVec		takes;
	bool	   *done;
	StdVec	   *claims;
	bool		claims_live;
	HostError  *error;
	bool		ok;
} txn_vacuum_given_frame_t;

/* txn_vacuum_given_body forms the markers, the takes, and the claims, and runs the map prepare. */
static void
txn_vacuum_given_body(void *frame_)
{
	txn_vacuum_given_frame_t *frame = frame_;
	txn_vacuum_given_closure_2_captures_t *c = &frame->captures;
	TxnMemtableMapTake take = {.state = c->record, .map = TXN_MEMTABLE_MAP_MAP_FREE, .key = c->block,
	.most = txn_free_space_most_len(c->page->most)};
	size_t		count;

	std_vec_new(&frame->markers, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->markers_live = true;
	for (size_t i = 0; i < frame->dead->len; i++)
	{
		const std_TupleU32U16U16U64_t *run = &STD_VEC_AT(frame->dead, std_TupleU32U16U16U64_t, i);
		uint16		end = STD_ADD(u16, run->f1, run->f2, 448, 21);

		for (uint16 at = run->f1; at < end; at++)
		{
			std_TupleU64U64_t marker = {(uint64) run->f0 << 16 | at, at == run->f1 ? run->f3 : 0};

			std_vec_push_Checked(&frame->markers, &marker, sizeof(marker), _Alignof(std_TupleU64U64_t));
		}
	}
	c->markers = &frame->markers;
	count = (c->freed->len > 0) + frame->markers.len;
	std_vec_with_capacity_Checked(&frame->takes, count, sizeof(TxnMemtableMapTake), _Alignof(TxnMemtableMapTake));
	frame->takes_live = true;
	for (size_t i = 0; i < count; i++)
		std_vec_push_Checked(&frame->takes, &take, sizeof(take), _Alignof(TxnMemtableMapTake));
	for (size_t i = 0; c->toast != NULL && i < c->downs->len; i++)
	{
		TxnMemtableMapTake value = {.state = *c->toast, .map = TXN_MEMTABLE_MAP_MAP_VALUES,
		.key = STD_VEC_AT(c->downs, TxnVacuumDown, i).id, .most = TXN_VALUES_MOST};

		std_vec_push_Checked(&frame->takes, &value, sizeof(value), _Alignof(TxnMemtableMapTake));
	}
	std_vec_with_capacity_Checked(frame->claims, frame->markers.len, TXN_VACUUM_CLAIM);
	frame->claims_live = true;
	for (size_t i = 0; i < frame->markers.len; i++)
	{
		txn_vacuum_TupleU64Bool_t claim = {STD_VEC_AT(&frame->markers, std_TupleU64U64_t, i).f0, false};

		std_vec_push_Checked(frame->claims, &claim, TXN_VACUUM_CLAIM);
	}
	c->claims = frame->claims;
	if (!txn_memtable_map_prepare_Bool(frame->io, frame->takes.ptr, frame->takes.len, txn_vacuum_given_closure_2, c,
									   NULL, frame->done, frame->error))
		return;
	frame->claims_live = !*frame->done;
	frame->ok = true;
}

/* txn_vacuum_given_pad frees what given holds live. */
static void
txn_vacuum_given_pad(void *frame_)
{
	txn_vacuum_given_frame_t *frame = frame_;

	if (frame->claims_live)
		std_vec_free_Checked(frame->claims, TXN_VACUUM_CLAIM);
	if (frame->takes_live)
		std_vec_free_Checked(&frame->takes, sizeof(TxnMemtableMapTake), _Alignof(TxnMemtableMapTake));
	if (frame->markers_live)
		std_vec_free_Checked(&frame->markers, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->claims_live = frame->takes_live = frame->markers_live = false;
}

/*
 * txn_vacuum_given gives a block's freed and dead runs back to its free
 * space entry in one map prepare, every take written by no transaction with
 * the LSN its record gets inside it: the freed runs by one take of their
 * offsets and bytes, and each dead tuple id by a take of its own, a marker
 * holding its tuple id, the run's first raising the entry's bytes by the
 * run's bytes. A marker's take refuses when the tuple id's newest naming
 * version is already a marker; it answers which dead tuple ids it claimed,
 * in order. It takes down, in the same prepare and window, each value id in
 * its TOAST storage's value map by its count, and answers done false, before
 * it opens the window, when the TOAST storage's mark moved, for its caller to
 * read them again.
 */
static bool
txn_vacuum_given(HostIoIo *io, const txn_vacuum_given_closure_2_captures_t *captures, const StdVec *dead,
				 bool *done, StdVec *claims, HostError *error)
{
	txn_vacuum_given_frame_t frame = {.io = io, .captures = *captures, .dead = dead, .done = done, .claims = claims,
	.error = error};

	std_landing(&frame, txn_vacuum_given_body, txn_vacuum_given_pad);
	txn_vacuum_given_pad(&frame);
	return frame.ok;
}

/* TXN_VACUUM_COUNT is the size and alignment of a value id and how many freed rows named it. */
#define TXN_VACUUM_COUNT sizeof(std_TupleU32U64_t), _Alignof(std_TupleU32U64_t)

/* txn_vacuum_id_order orders value ids. */
static int
txn_vacuum_id_order(const void *a, const void *b)
{
	uint32		left = *(const uint32 *) a;
	uint32		right = *(const uint32 *) b;

	return left < right ? -1 : left > right;
}

/* txn_vacuum_runs_view returns a vector viewing a block's runs in place, which nothing frees. */
static StdVec
txn_vacuum_runs_view(const std_TupleU32U16U16U64_t *runs, size_t runs_len)
{
	StdVec		view = {(void *) runs, runs_len, runs_len};

	return view;
}

/*
 * txn_vacuum_freed_frame_t is freed's state across its pad: the base, the
 * tuple ids it frees, their runs, and each block's counts, entries, downs,
 * record, and claims, each while live.
 */
typedef struct txn_vacuum_freed_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const HostSharedHeld *toast;
	HostStorage toast_storage;
	HostStorage storage;
	bool		logged;
	const TxnInsertPage *page;
	const TxnVacuumFreed *tids;
	size_t		tids_len;
	bool		dead;
	bool		base_live;
	TxnBaseBase base;
	bool		kept_live;
	StdVec		kept;
	bool		runs_live;
	StdVec		runs;
	bool		ids_live;
	StdVec		ids;
	bool		counts_live;
	StdVec		counts;
	bool		has_below;
	StdVec		below;
	bool		downs_live;
	StdVec		downs;
	bool		body_live;
	StdVec		body;
	bool		claims_live;
	StdVec		claims;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} txn_vacuum_freed_frame_t;

/* txn_vacuum_freed_block_drop frees one block's counts, entries, downs, record, and claims. */
static void
txn_vacuum_freed_block_drop(txn_vacuum_freed_frame_t *frame)
{
	if (frame->claims_live)
		std_vec_free_Checked(&frame->claims, TXN_VACUUM_CLAIM);
	if (frame->body_live)
		std_vec_free_Checked(&frame->body, 1, 1);
	if (frame->downs_live)
		txn_vacuum_downs_drop(&frame->downs);
	if (frame->has_below)
		std_vec_free_Checked(&frame->below, 1, 1);
	if (frame->counts_live)
		std_vec_free_Checked(&frame->counts, TXN_VACUUM_COUNT);
	if (frame->ids_live)
		std_vec_free_Checked(&frame->ids, TXN_VACUUM_ID);
	frame->claims_live = frame->body_live = frame->downs_live = frame->has_below = false;
	frame->counts_live = frame->ids_live = false;
}

/* txn_vacuum_freed_counts counts how many of a block's freed rows named each value id, ascending by id. */
static void
txn_vacuum_freed_counts(txn_vacuum_freed_frame_t *frame, uint32 block)
{
	std_vec_new(&frame->ids, TXN_VACUUM_ID);
	frame->ids_live = true;
	for (size_t i = 0; !frame->dead && i < frame->kept.len; i++)
	{
		const TxnVacuumFreed *freed = STD_VEC_AT(&frame->kept, const TxnVacuumFreed *, i);

		if ((uint32) (freed->f0 >> 16) != block)
			continue;
		for (size_t at = 0; at < freed->f3.len; at++)
			std_vec_push_Checked(&frame->ids, &STD_VEC_AT(&freed->f3, uint32, at), TXN_VACUUM_ID);
	}
	if (frame->ids.len > 1)
		qsort(frame->ids.ptr, frame->ids.len, sizeof(uint32), txn_vacuum_id_order);
	std_vec_new(&frame->counts, TXN_VACUUM_COUNT);
	frame->counts_live = true;
	for (size_t i = 0; i < frame->ids.len; i++)
	{
		uint32		id = STD_VEC_AT(&frame->ids, uint32, i);
		std_TupleU32U64_t *last = frame->counts.len > 0 ?
			&STD_VEC_AT(&frame->counts, std_TupleU32U64_t, frame->counts.len - 1) : NULL;

		if (last != NULL && last->f0 == id)
			last->f1 = STD_ADD(u64, last->f1, 1, 537, 17);
		else
		{
			std_TupleU32U64_t count = {id, 1};

			std_vec_push_Checked(&frame->counts, &count, TXN_VACUUM_COUNT);
		}
	}
}

/* txn_vacuum_freed_block gives back one block's runs, reading its values' entries again until their mark holds. */
static bool
txn_vacuum_freed_block(txn_vacuum_freed_frame_t *frame, const std_TupleU32U16U16U64_t *runs, size_t runs_len)
{
	uint32		block = runs[0].f0;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	uint64		horizon = 0;
	bool		toasted;
	StdVec		these = txn_vacuum_runs_view(runs, runs_len);
	StdVec		none = txn_vacuum_runs_view(NULL, 0);
	HostSharedRecord toast_record = {0};

	for (size_t i = 0; i < frame->kept.len; i++)
	{
		const TxnVacuumFreed *freed = STD_VEC_AT(&frame->kept, const TxnVacuumFreed *, i);

		if ((uint32) (freed->f0 >> 16) == block)
			horizon = Max(horizon, freed->f2);
	}
	txn_vacuum_freed_counts(frame, block);
	toasted = frame->counts.len > 0;
	if (toasted && frame->toast == NULL)
	{
		*frame->error = txn_vacuum_fork("XX000", "a freed row names values of a table with no TOAST");
		return false;
	}
	if (toasted)
		toast_record = host_shared_Held_record(frame->toast);
	for (;;)
	{
		txn_vacuum_given_closure_2_captures_t captures = {.record = record, .block = block, .below = &frame->below,
			.page = frame->page, .freed = frame->dead ? &none : &these, .toast = toasted ? &toast_record : NULL,
		.downs = &frame->downs, .windowed = true};
		bool		has_mark = false;
		bool		done;

		if (!txn_vacuum_below(frame->io, frame->store, frame->base_live ? &frame->base : NULL, block,
							  &frame->has_below, &frame->below, frame->error) ||
			!txn_vacuum_first(frame->has_below, &frame->below, block, frame->base_live ? &frame->base : NULL,
							  frame->page, &captures.start, frame->error))
			return false;
		captures.has_below = frame->has_below;
		std_vec_with_capacity_Checked(&frame->downs, frame->counts.len, TXN_VACUUM_DOWN);
		frame->downs_live = true;
		for (size_t i = 0; toasted && i < frame->counts.len; i++)
		{
			const std_TupleU32U64_t *count = &STD_VEC_AT(&frame->counts, std_TupleU32U64_t, i);
			TxnVacuumDown down = {.id = count->f0, .by = count->f1};
			std_TupleU64U64_t at;

			if (!txn_values_based(frame->io, frame->store, frame->toast, down.id, &down.has_based, &down.based, &at,
								  frame->error))
				return false;
			std_vec_push_Checked(&frame->downs, &down, TXN_VACUUM_DOWN);
			if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-vacuum-based", frame->error))
				return false;
			if (!has_mark)
				captures.mark = at;
			has_mark = true;
		}
		if (frame->logged)
		{
			TxnRecordTidsFreed record_ = {.storage = frame->storage, .horizon = horizon,
				.runs = frame->dead ? none : these, .dead = frame->dead ? these : none, .has_values = toasted,
			.values = {.f0 = frame->toast_storage, .f1 = frame->counts}};

			txn_record_TidsFreed_encode(&record_, &frame->body);
			frame->body_live = true;
			captures.body = &frame->body;
		}
		if (!txn_vacuum_given(frame->io, &captures, frame->dead ? &these : &none, &done, &frame->claims,
							  frame->error))
			return false;
		if (done)
		{
			frame->claims_live = true;
			if (toasted)
				txn_state_totals_unknown(toast_record);
			for (size_t i = 0; i < frame->claims.len; i++)
			{
				const txn_vacuum_TupleU64Bool_t *claim = &STD_VEC_AT(&frame->claims, txn_vacuum_TupleU64Bool_t, i);

				if (claim->f1)
					std_vec_push_Checked(frame->result, &claim->f0, sizeof(uint64), _Alignof(uint64));
			}
			txn_vacuum_freed_block_drop(frame);
			return true;
		}
		if (frame->body_live)
			std_vec_free_Checked(&frame->body, 1, 1);
		frame->body_live = false;
		frame->downs_live = false;
		txn_vacuum_downs_drop(&frame->downs);
		if (frame->has_below)
			std_vec_free_Checked(&frame->below, 1, 1);
		frame->has_below = false;
	}
}

/* txn_vacuum_freed_body frees the tuple ids block by block, by their runs. */
static void
txn_vacuum_freed_body(void *frame_)
{
	txn_vacuum_freed_frame_t *frame = frame_;
	bool		some;

	std_vec_new(frame->result, sizeof(uint64), _Alignof(uint64));
	frame->result_live = true;
	if (!txn_state_base_record(frame->io, frame->held, &some, &frame->base, frame->error))
		return;
	frame->base_live = some;
	std_vec_new(&frame->kept, sizeof(const TxnVacuumFreed *), _Alignof(const TxnVacuumFreed *));
	frame->kept_live = true;
	for (size_t i = 0; i < frame->tids_len; i++)
		if (!frame->dead || frame->tids[i].f1 > 0)
		{
			const TxnVacuumFreed *freed = &frame->tids[i];

			std_vec_push_Checked(&frame->kept, &freed, sizeof(freed), _Alignof(const TxnVacuumFreed *));
		}
	txn_vacuum_runs(frame->kept.ptr, frame->kept.len, &frame->runs);
	frame->runs_live = true;
	for (size_t at = 0; at < frame->runs.len;)
	{
		const std_TupleU32U16U16U64_t *runs = &STD_VEC_AT(&frame->runs, std_TupleU32U16U16U64_t, at);
		size_t		len = 1;

		while (at + len < frame->runs.len && runs[len].f0 == runs[0].f0)
			len++;
		if (!txn_vacuum_freed_block(frame, runs, len))
			return;
		at += len;
	}
	frame->result_live = false;
	frame->ok = true;
}

/* txn_vacuum_freed_pad frees what freed holds live. */
static void
txn_vacuum_freed_pad(void *frame_)
{
	txn_vacuum_freed_frame_t *frame = frame_;

	txn_vacuum_freed_block_drop(frame);
	if (frame->runs_live)
		std_vec_free_Checked(&frame->runs, TXN_VACUUM_RUN);
	if (frame->kept_live)
		std_vec_free_Checked(&frame->kept, sizeof(const TxnVacuumFreed *), _Alignof(const TxnVacuumFreed *));
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	if (frame->result_live)
		std_vec_free_Checked(frame->result, sizeof(uint64), _Alignof(uint64));
	frame->runs_live = frame->kept_live = frame->base_live = frame->result_live = false;
}

/*
 * txn_vacuum_freed frees a storage's tuple ids, each with the heap bytes its
 * take lowered and its row's ender's full xid, in tuple id order, or, for
 * dead, gives back only the bytes of those with any and keeps their offsets
 * taken, as heap's prune leaves LP_DEAD line pointers: each block's by
 * given, inside a checkpoint window that for a logged storage first appends
 * the block's TidsFreed record, with its conflict horizon, the newest of its
 * tuple ids' enders, else 0. A full free takes down in its TOAST storage's
 * value map, in the same window and record, the value ids the freed tuple
 * ids' rows named, each by how many named it, and marks the TOAST storage's
 * totals unknown; a dead tuple id keeps its row's values until its free. It
 * answers the dead tuple ids its markers claimed.
 */
bool
txn_vacuum_freed(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const HostSharedHeld *toast,
				 HostStorage toast_storage, HostStorage storage, bool logged, const TxnInsertPage *page,
				 const TxnVacuumFreed *tids, size_t tids_len, bool dead, StdVec *result, HostError *error)
{
	txn_vacuum_freed_frame_t frame = {.io = io, .store = store, .held = held, .toast = toast,
		.toast_storage = toast_storage, .storage = storage, .logged = logged, .page = page, .tids = tids,
	.tids_len = tids_len, .dead = dead, .result = result, .error = error};

	std_landing(&frame, txn_vacuum_freed_body, txn_vacuum_freed_pad);
	txn_vacuum_freed_pad(&frame);
	return frame.ok;
}

/*
 * TxnVacuumRemoval is a dead tuple id a prune takes out of its key's chain
 * once its marker claims it: the tuple id, its key, and its version.
 */
typedef struct TxnVacuumRemoval
{
	uint64		tid;
	StdVec		key;
	TxnMemtableVersionHandle handle;
} TxnVacuumRemoval;

/* TXN_VACUUM_REMOVAL is the size and alignment of a prune's removal. */
#define TXN_VACUUM_REMOVAL sizeof(TxnVacuumRemoval), _Alignof(TxnVacuumRemoval)

/* txn_vacuum_removals_drop frees a prune's removals. */
static void
txn_vacuum_removals_drop(StdVec *removals)
{
	for (size_t i = 0; i < removals->len; i++)
		std_vec_free_Checked(&STD_VEC_AT(removals, TxnVacuumRemoval, i).key, 1, 1);
	std_vec_free_Checked(removals, TXN_VACUUM_REMOVAL);
}

/* txn_vacuum_pruned_closure_8_captures_t is a prune's clear's prepare's closure's captures. */
typedef struct txn_vacuum_pruned_closure_8_captures_t
{
	txn_vacuum_BlockStep_t step;
	bool		has_below;
	const StdVec *below;
} txn_vacuum_pruned_closure_8_captures_t;

/* txn_vacuum_pruned_closure_8 is a prune's clear's prepare's closure: the block's take that clears its full flag. */
static bool
txn_vacuum_pruned_closure_8(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
							TxnMemtableMapTaken *result, HostError *error)
{
	txn_vacuum_pruned_closure_8_captures_t *inside = captures;
	TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;

	return txn_memtable_map_finish_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, none, false, 0, inside->has_below,
										inside->below->ptr, inside->below->len, txn_vacuum_cleared, &inside->step, NULL,
										txn_vacuum_nothing, NULL, NULL, result, error);
}

/*
 * txn_vacuum_pruned_frame_t is pruned's state across its pad: the base, the
 * block's entries, its takers and taken offsets, a key's chain and what the
 * snapshots see of it, the dead tuple ids and their removals, and the claims,
 * each while live.
 */
typedef struct txn_vacuum_pruned_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	HostStorage storage;
	bool		logged;
	const TxnInsertPage *page;
	uint32		block;
	uint64		reserve;
	const TxnVacuumPrune *prune;
	bool		base_live;
	TxnBaseBase base;
	bool		has_below;
	StdVec		below;
	bool		newest_live;
	TxnMemtableMapEntry newest;
	bool		takers_live;
	StdVec		takers;
	bool		took_live;
	StdVec		took;
	bool		value_live;
	StdVec		value;
	bool		key_live;
	StdVec		key;
	bool		hold_live;
	HostDeltaHold hold;
	bool		chain_live;
	StdVec		chain;
	bool		seen_live;
	TxnVisibleSeen seen;
	bool		dead_live;
	StdVec		dead;
	bool		removals_live;
	StdVec		removals;
	bool		claimed_live;
	StdVec		claimed;
	bool		removed_live;
	StdVec		removed;
	HostError  *error;
	bool		ok;
} txn_vacuum_pruned_frame_t;

/* txn_vacuum_pruned_key_drop frees one tuple id's key, chain, and seen rows. */
static void
txn_vacuum_pruned_key_drop(txn_vacuum_pruned_frame_t *frame)
{
	if (frame->seen_live)
		txn_visible_Seen_drop(&frame->seen);
	if (frame->chain_live)
		txn_memtable_version_versions_drop(&frame->chain, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	if (frame->value_live)
		std_vec_free_Checked(&frame->value, 1, 1);
	frame->seen_live = frame->chain_live = frame->hold_live = frame->key_live = frame->value_live = false;
}

/* txn_vacuum_seen_names reports whether what a snapshot saw of a chain holds a row with a tuple id, and which. */
static const TxnVisibleSeenRow *
txn_vacuum_seen_names(const TxnVisibleSeen *seen, uint64 tid)
{
	for (size_t i = 0; i < seen->rows.len; i++)
	{
		const TxnVisibleSeenRow *row = &STD_VEC_AT(&seen->rows, TxnVisibleSeenRow, i);

		if (row->row.put.tuple.f0 == tid)
			return row;
	}
	return NULL;
}

/*
 * txn_vacuum_pruned_ended answers whether the snapshot's horizon passed a
 * tuple id's row, with its version and its ender's xid.
 */
static bool
txn_vacuum_pruned_ended(txn_vacuum_pruned_frame_t *frame, uint64 tid, bool *ended,
						TxnMemtableVersionHandle *handle, uint64 *ender)
{
	HostSharedRecord record = host_shared_Held_record(frame->held);
	TxnMemtableVersionVersion *version = NULL;
	TxnVisibleRule horizon = {&TXN_VISIBLE_SNAPSHOT_RULE_OPS, frame->prune->horizon};
	TxnVisibleRule any = {&TXN_VISIBLE_SNAPSHOT_RULE_OPS, frame->prune->any};
	const TxnVisibleSeenRow *seen;

	*ended = false;
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return false;
	frame->hold_live = true;
	if (!txn_memtable_table_versions(frame->io, &frame->hold, record, frame->key.ptr, frame->key.len, &frame->chain,
									 frame->error))
		return false;
	frame->chain_live = true;
	for (size_t i = 0; i < frame->chain.len && version == NULL; i++)
		if (txn_vacuum_holds(&STD_VEC_AT(&frame->chain, TxnMemtableVersionVersion, i), tid))
			version = &STD_VEC_AT(&frame->chain, TxnMemtableVersionVersion, i);
	if (version == NULL)
		return true;
	if (!txn_visible_seen(frame->io, horizon, frame->chain.ptr, frame->chain.len, NULL, &frame->seen, frame->error))
		return false;
	frame->seen_live = true;
	seen = txn_vacuum_seen_names(&frame->seen, tid);
	frame->seen_live = false;
	txn_visible_Seen_drop(&frame->seen);
	if (seen != NULL)
		return true;
	if (!txn_visible_seen(frame->io, any, frame->chain.ptr, frame->chain.len, NULL, &frame->seen, frame->error))
		return false;
	frame->seen_live = true;
	seen = txn_vacuum_seen_names(&frame->seen, tid);
	if (seen != NULL && seen->changer != NULL)
	{
		TxnMemtableVersionRecord changed = txn_memtable_version_Version_record(seen->changer);

		*ended = true;
		*handle = version->handle;
		*ender = txn_memtable_version_Record_writer(&changed).xid;
	}
	return true;
}

/*
 * txn_vacuum_pruned_body checks the block's prune condition, finds its dead
 * tuple ids, marks them, and takes out those it claimed.
 */
static void
txn_vacuum_pruned_body(void *frame_)
{
	txn_vacuum_pruned_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	TxnFreeSpaceFree entry;
	bool		some;
	bool		has_value;
	const StdVec *value;
	uint64		floor;

	if (!txn_state_base_record(frame->io, frame->held, &some, &frame->base, frame->error))
		return;
	frame->base_live = some;
	if (!txn_vacuum_below(frame->io, frame->store, some ? &frame->base : NULL, frame->block, &frame->has_below,
						  &frame->below, frame->error) ||
		!txn_memtable_map_newest(frame->io, record, TXN_MEMTABLE_MAP_MAP_FREE, frame->block, &some, &frame->newest,
								 frame->error))
		return;
	frame->newest_live = some;
	has_value = some ? frame->newest.has_value : frame->has_below;
	value = some ? &frame->newest.value : &frame->below;
	if (!has_value)
	{
		frame->ok = true;
		return;
	}
	if (!txn_free_space_Free_decode(value->ptr, value->len, frame->block, (uint64) frame->block << 16,
									frame->page->most, &entry, frame->error))
		return;
	if (!entry.full)
	{
		floor = Max(frame->reserve, STD_ADD(u64, frame->page->bytes, TXN_VACUUM_PAGE_HEADER, 630, 80) / 10);
		if ((entry.bytes > TXN_VACUUM_LINE_POINTER ? entry.bytes - TXN_VACUUM_LINE_POINTER : 0) >= floor)
		{
			frame->ok = true;
			return;
		}
	}
	if (!txn_memtable_map_takers(frame->io, record, TXN_MEMTABLE_MAP_MAP_FREE, frame->block, &frame->takers,
								 frame->error))
		return;
	frame->takers_live = true;
	std_vec_new(&frame->dead, sizeof(TxnVacuumFreed), _Alignof(TxnVacuumFreed));
	frame->dead_live = true;
	std_vec_new(&frame->removals, TXN_VACUUM_REMOVAL);
	frame->removals_live = true;
	if (!txn_vacuum_taken(&frame->takers, frame->has_below, &frame->below, frame->block,
						  frame->base_live ? &frame->base : NULL, frame->page, &frame->took, frame->error))
		return;
	frame->took_live = true;
	for (size_t i = 0; i < frame->took.len; i++)
	{
		const TxnVacuumTook *take = &STD_VEC_AT(&frame->took, TxnVacuumTook, i);
		HostRow		tid = {take->f0};
		bool		paired = false;
		bool		ended;
		TxnMemtableVersionHandle handle;
		uint64		ender;

		if (frame->base_live && frame->base.has_tuple_ids)
		{
			TreesStoreTree tree = frame->base.tuple_ids;
			ChunksErrorError failed;
			uint8		key[6];

			txn_base_tuple_id_key(take->f0, key);
			if (!trees_get(frame->io, frame->store, TREES_STORE_TREE_NAMED(&tree), key, sizeof(key), &paired,
						   &frame->value, &failed))
			{
				*frame->error = txn_base_storage(&failed);
				return;
			}
			frame->value_live = paired;
		}
		if (take->f2 == 0 || paired)
		{
			txn_vacuum_pruned_key_drop(frame);
			continue;
		}
		if (!txn_memtable_tuples_get(frame->io, record, tid, &some, &frame->key, frame->error))
			return;
		frame->key_live = some;
		if (!some)
			continue;
		if (!txn_vacuum_pruned_ended(frame, take->f0, &ended, &handle, &ender))
			return;
		if (ended)
		{
			TxnVacuumFreed dead = {take->f0, take->f2, ender};
			TxnVacuumRemoval removal = {take->f0, frame->key, handle};

			std_vec_new(&dead.f3, TXN_VACUUM_ID);
			std_vec_push_Checked(&frame->dead, &dead, sizeof(dead), _Alignof(TxnVacuumFreed));
			std_vec_push_Checked(&frame->removals, &removal, TXN_VACUUM_REMOVAL);
			frame->key_live = false;
		}
		txn_vacuum_pruned_key_drop(frame);
	}
	if (frame->dead.len > 0)
	{
		HostStorage none = {0};

		if (!txn_vacuum_freed(frame->io, frame->store, frame->held, NULL, none, frame->storage, frame->logged,
							  frame->page, frame->dead.ptr, frame->dead.len, true, &frame->claimed, frame->error))
			return;
		frame->claimed_live = true;
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-prune-marked", frame->error))
			return;
		for (size_t i = 0; i < frame->removals.len; i++)
		{
			TxnVacuumRemoval *removal = &STD_VEC_AT(&frame->removals, TxnVacuumRemoval, i);
			bool		claimed = false;

			for (size_t at = 0; at < frame->claimed.len && !claimed; at++)
				claimed = STD_VEC_AT(&frame->claimed, uint64, at) == removal->tid;
			if (!claimed)
				continue;
			if (!txn_memtable_table_removed(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, record, removal->key.ptr,
											removal->key.len, &removal->handle, 1, &frame->removed, frame->error))
				return;
			std_vec_free_Checked(&frame->removed, sizeof(bool), _Alignof(bool));
		}
		frame->ok = true;
		return;
	}
	if (entry.full)
	{
		txn_vacuum_pruned_closure_8_captures_t inside = {.step = {.record = record, .block = frame->block,
		.most = frame->page->most}, .has_below = frame->has_below, .below = &frame->below};
		TxnMemtableMapTake take = {.state = record, .map = TXN_MEMTABLE_MAP_MAP_FREE, .key = frame->block,
		.most = txn_free_space_most_len(frame->page->most)};
		TxnMemtableMapTaken taken;

		if (!txn_memtable_map_prepare_Taken(frame->io, &take, 1, txn_vacuum_pruned_closure_8, &inside, NULL, &taken,
											frame->error))
			return;
	}
	frame->ok = true;
}

/* txn_vacuum_pruned_pad frees what pruned holds live. */
static void
txn_vacuum_pruned_pad(void *frame_)
{
	txn_vacuum_pruned_frame_t *frame = frame_;

	txn_vacuum_pruned_key_drop(frame);
	if (frame->claimed_live)
		std_vec_free_Checked(&frame->claimed, sizeof(uint64), _Alignof(uint64));
	if (frame->removals_live)
		txn_vacuum_removals_drop(&frame->removals);
	if (frame->dead_live)
		txn_vacuum_freed_drop(&frame->dead);
	if (frame->took_live)
		txn_vacuum_took_drop(&frame->took);
	if (frame->takers_live)
		txn_memtable_map_takers_drop(&frame->takers);
	if (frame->newest_live && frame->newest.has_value)
		std_vec_free_Checked(&frame->newest.value, 1, 1);
	if (frame->has_below)
		std_vec_free_Checked(&frame->below, 1, 1);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->claimed_live = frame->removals_live = frame->dead_live = frame->took_live = frame->takers_live = false;
	frame->newest_live = frame->has_below = frame->base_live = false;
}

/*
 * txn_vacuum_pruned prunes a block of a storage before an update's take of
 * its replaced row's block, on heap_page_prune_opt's condition: its free
 * space entry's full flag is set, or its free bytes, less a line pointer's,
 * are below the fill factor's reserve and a tenth of a page, the larger.
 * Each tuple id a take holds there whose row version no base maps, which
 * the relation's NonVacuumable snapshot doesn't see, and which an ender
 * ended is given back as a dead tuple id, logged with the newest ender's xid
 * as the conflict horizon, and then taken out of its key's chain only when
 * its marker claimed it. A block whose prune finds none has its flag cleared
 * with no record.
 */
bool
txn_vacuum_pruned(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, HostStorage storage,
				  bool logged, const TxnInsertPage *page, uint32 block, uint64 reserve, const TxnVacuumPrune *prune,
				  HostError *error)
{
	txn_vacuum_pruned_frame_t frame = {.io = io, .store = store, .held = held, .storage = storage, .logged = logged,
	.page = page, .block = block, .reserve = reserve, .prune = prune, .error = error};

	std_landing(&frame, txn_vacuum_pruned_body, txn_vacuum_pruned_pad);
	txn_vacuum_pruned_pad(&frame);
	return frame.ok;
}

/*
 * txn_vacuum_holding_frame_t is holding's state across its pad: the key, its
 * hold, its chain, and a removal's answer, each while live.
 */
typedef struct txn_vacuum_holding_frame_t
{
	HostIoIo   *io;
	HostSharedRecord record;
	uint64		tid;
	bool		unhold;
	bool		key_live;
	StdVec		key;
	bool		hold_live;
	HostDeltaHold hold;
	bool		chain_live;
	StdVec		chain;
	bool		removed_live;
	StdVec		removed;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_vacuum_holding_frame_t;

/*
 * txn_vacuum_holding_body finds the version holding the tuple id and, to
 * unhold it, takes it out and takes back its count.
 */
static void
txn_vacuum_holding_body(void *frame_)
{
	txn_vacuum_holding_frame_t *frame = frame_;
	HostRow		tid = {frame->tid};
	TxnMemtableVersionVersion *version = NULL;
	bool		some;

	*frame->result = false;
	if (!txn_memtable_tuples_get(frame->io, frame->record, tid, &some, &frame->key, frame->error))
		return;
	frame->key_live = some;
	if (!some)
	{
		frame->ok = true;
		return;
	}
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_table_versions(frame->io, &frame->hold, frame->record, frame->key.ptr, frame->key.len,
									 &frame->chain, frame->error))
		return;
	frame->chain_live = true;
	for (size_t i = 0; i < frame->chain.len && version == NULL; i++)
		if (txn_vacuum_holds(&STD_VEC_AT(&frame->chain, TxnMemtableVersionVersion, i), frame->tid))
			version = &STD_VEC_AT(&frame->chain, TxnMemtableVersionVersion, i);
	*frame->result = version != NULL;
	if (version != NULL && frame->unhold)
	{
		if (!txn_memtable_table_removed(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, frame->record, frame->key.ptr,
										frame->key.len, &version->handle, 1, &frame->removed, frame->error))
			return;
		frame->removed_live = true;
		if (frame->removed.len == 1 && STD_VEC_AT(&frame->removed, bool, 0) &&
			!txn_memtable_table_uncounted(frame->io, version, frame->error))
			return;
	}
	frame->ok = true;
}

/* txn_vacuum_holding_pad frees what holding holds live. */
static void
txn_vacuum_holding_pad(void *frame_)
{
	txn_vacuum_holding_frame_t *frame = frame_;

	if (frame->removed_live)
		std_vec_free_Checked(&frame->removed, sizeof(bool), _Alignof(bool));
	if (frame->chain_live)
		txn_memtable_version_versions_drop(&frame->chain, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	frame->removed_live = frame->chain_live = frame->hold_live = frame->key_live = false;
}

/*
 * txn_vacuum_holding finds the row version that holds a tuple id, in the
 * chain of the key its tuple id table names for it, reporting whether one
 * does, and to unhold it takes it out of its key's chain, taking back its
 * count at its writer's entry when this removal took it out.
 */
static bool
txn_vacuum_holding(HostIoIo *io, HostSharedRecord record, uint64 tid, bool unhold, bool *result, HostError *error)
{
	txn_vacuum_holding_frame_t frame = {.io = io, .record = record, .tid = tid, .unhold = unhold, .result = result,
	.error = error};

	std_landing(&frame, txn_vacuum_holding_body, txn_vacuum_holding_pad);
	txn_vacuum_holding_pad(&frame);
	return frame.ok;
}

/*
 * txn_vacuum_unheld takes out of its key's chain the row version that holds a
 * tuple id, if one does, taking back its count.
 */
static bool
txn_vacuum_unheld(HostIoIo *io, HostSharedRecord record, uint64 tid, HostError *error)
{
	bool		found;

	return txn_vacuum_holding(io, record, tid, true, &found, error);
}

/*
 * txn_vacuum_held reports whether a row version in a storage's delta holds a
 * tuple id, as unheld finds the version it takes out.
 */
bool
txn_vacuum_held(HostIoIo *io, HostSharedRecord record, uint64 tid, bool *result, HostError *error)
{
	return txn_vacuum_holding(io, record, tid, false, result, error);
}

/* txn_vacuum_blocks returns the blocks of a free's runs, each once, in order. */
static void
txn_vacuum_blocks(const TxnRecordTidsFreed *freed, StdVec *blocks)
{
	const StdVec *both[2] = {&freed->runs, &freed->dead};
	size_t		kept = 0;

	std_vec_new(blocks, sizeof(uint32), _Alignof(uint32));
	for (int k = 0; k < 2; k++)
		for (size_t i = 0; i < both[k]->len; i++)
			std_vec_push_Checked(blocks, &STD_VEC_AT(both[k], std_TupleU32U16U16U64_t, i).f0, sizeof(uint32),
								 _Alignof(uint32));
	if (blocks->len > 1)
		qsort(blocks->ptr, blocks->len, sizeof(uint32), txn_vacuum_id_order);
	for (size_t i = 0; i < blocks->len; i++)
		if (kept == 0 || STD_VEC_AT(blocks, uint32, kept - 1) != STD_VEC_AT(blocks, uint32, i))
			STD_VEC_AT(blocks, uint32, kept++) = STD_VEC_AT(blocks, uint32, i);
	blocks->len = kept;
}

/* txn_vacuum_of_block collects a free's runs of one block. */
static void
txn_vacuum_of_block(const StdVec *runs, uint32 block, StdVec *result)
{
	std_vec_new(result, TXN_VACUUM_RUN);
	for (size_t i = 0; i < runs->len; i++)
		if (STD_VEC_AT(runs, std_TupleU32U16U16U64_t, i).f0 == block)
			std_vec_push_Checked(result, &STD_VEC_AT(runs, std_TupleU32U16U16U64_t, i), TXN_VACUUM_RUN);
}

/*
 * txn_vacuum_taken_out_frame_t is taken_out's state across its pad: the blocks
 * and a block's claimed tuple ids, each while live.
 */
typedef struct txn_vacuum_taken_out_frame_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	const TxnRecordTidsFreed *freed;
	uint64		lsn;
	bool		blocks_live;
	StdVec		blocks;
	bool		claimed_live;
	StdVec		claimed;
	HostError  *error;
	bool		ok;
} txn_vacuum_taken_out_frame_t;

/* txn_vacuum_taken_out_body takes out, block by block, each claimed dead tuple id's version, then each freed one's. */
static void
txn_vacuum_taken_out_body(void *frame_)
{
	txn_vacuum_taken_out_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);

	txn_vacuum_blocks(frame->freed, &frame->blocks);
	frame->blocks_live = true;
	for (size_t b = 0; b < frame->blocks.len; b++)
	{
		uint32		block = STD_VEC_AT(&frame->blocks, uint32, b);

		if (!txn_memtable_map_marked(frame->io, record, block, frame->lsn, &frame->claimed, frame->error))
			return;
		frame->claimed_live = true;
		for (size_t i = 0; i < frame->claimed.len; i++)
			if (!txn_vacuum_unheld(frame->io, record, STD_VEC_AT(&frame->claimed, HostRow, i).f0, frame->error))
				return;
		for (size_t i = 0; i < frame->freed->runs.len; i++)
		{
			const std_TupleU32U16U16U64_t *run = &STD_VEC_AT(&frame->freed->runs, std_TupleU32U16U16U64_t, i);
			uint16		end;

			if (run->f0 != block)
				continue;
			end = STD_ADD(u16, run->f1, run->f2, 801, 55);
			for (uint16 at = run->f1; at < end; at++)
				if (!txn_vacuum_unheld(frame->io, record, (uint64) block << 16 | at, frame->error))
					return;
		}
		frame->claimed_live = false;
		std_vec_free_Checked(&frame->claimed, sizeof(HostRow), _Alignof(HostRow));
	}
	frame->ok = true;
}

/* txn_vacuum_taken_out_pad frees what taken_out holds live. */
static void
txn_vacuum_taken_out_pad(void *frame_)
{
	txn_vacuum_taken_out_frame_t *frame = frame_;

	if (frame->claimed_live)
		std_vec_free_Checked(&frame->claimed, sizeof(HostRow), _Alignof(HostRow));
	if (frame->blocks_live)
		std_vec_free_Checked(&frame->blocks, sizeof(uint32), _Alignof(uint32));
	frame->claimed_live = frame->blocks_live = false;
}

/*
 * txn_vacuum_taken_out does a replayed free's take-outs, as its primary's
 * prune and VACUUM did them: each dead tuple id its marker claimed, a
 * version with the record's LSN holding it, and each freed tuple id, has its
 * row version taken out of its chain.
 */
bool
txn_vacuum_taken_out(HostIoIo *io, const HostSharedHeld *held, const TxnRecordTidsFreed *freed, uint64 lsn,
					 HostError *error)
{
	txn_vacuum_taken_out_frame_t frame = {.io = io, .held = held, .freed = freed, .lsn = lsn, .error = error};

	std_landing(&frame, txn_vacuum_taken_out_body, txn_vacuum_taken_out_pad);
	txn_vacuum_taken_out_pad(&frame);
	return frame.ok;
}

/* txn_vacuum_redo_freed_closure_5_captures_t is a replayed value's take down's prepare's closure's captures. */
typedef struct txn_vacuum_redo_freed_closure_5_captures_t
{
	uint64		lsn;
	bool		has_based;
	const StdVec *based;
	uint64		by;
} txn_vacuum_redo_freed_closure_5_captures_t;

/* txn_vacuum_redo_freed_closure_5 is a replayed value's take down's prepare's closure. */
static bool
txn_vacuum_redo_freed_closure_5(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
								TxnMemtableMapTaken *result, HostError *error)
{
	txn_vacuum_redo_freed_closure_5_captures_t *inside = captures;
	TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;

	return txn_memtable_map_finish_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, none, true, inside->lsn,
										inside->has_based, inside->based->ptr, inside->based->len,
										txn_vacuum_taken_down, &inside->by, NULL, txn_vacuum_nothing, NULL, NULL,
										result, error);
}

/*
 * txn_vacuum_redo_frame_t is redo_freed's state across its pad: the base, the
 * blocks, and a block's or value's reads, each while live.
 */
typedef struct txn_vacuum_redo_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const HostSharedHeld *toast;
	const TxnRecordTidsFreed *freed;
	const TxnInsertPage *page;
	uint64		lsn;
	bool		base_live;
	TxnBaseBase base;
	bool		blocks_live;
	StdVec		blocks;
	bool		runs_live;
	StdVec		runs;
	bool		dead_live;
	StdVec		dead;
	bool		has_below;
	StdVec		below;
	bool		claims_live;
	StdVec		claims;
	bool		has_based;
	StdVec		based;
	HostError  *error;
	bool		ok;
} txn_vacuum_redo_frame_t;

/* txn_vacuum_redo_block_drop frees one block's runs, entry, and claims. */
static void
txn_vacuum_redo_block_drop(txn_vacuum_redo_frame_t *frame)
{
	if (frame->claims_live)
		std_vec_free_Checked(&frame->claims, TXN_VACUUM_CLAIM);
	if (frame->has_below)
		std_vec_free_Checked(&frame->below, 1, 1);
	if (frame->dead_live)
		std_vec_free_Checked(&frame->dead, TXN_VACUUM_RUN);
	if (frame->runs_live)
		std_vec_free_Checked(&frame->runs, TXN_VACUUM_RUN);
	frame->claims_live = frame->has_below = frame->dead_live = frame->runs_live = false;
}

/*
 * txn_vacuum_redo_body gives each block back unless its entry holds the
 * record, takes the values down, then does the take-outs.
 */
static void
txn_vacuum_redo_body(void *frame_)
{
	txn_vacuum_redo_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	bool		some;
	bool		deferring;

	if (!txn_state_base_record(frame->io, frame->held, &some, &frame->base, frame->error))
		return;
	frame->base_live = some;
	txn_vacuum_blocks(frame->freed, &frame->blocks);
	frame->blocks_live = true;
	for (size_t b = 0; b < frame->blocks.len; b++)
	{
		uint32		block = STD_VEC_AT(&frame->blocks, uint32, b);
		txn_vacuum_given_closure_2_captures_t captures = {.record = record, .block = block, .below = &frame->below,
		.page = frame->page, .freed = &frame->runs, .has_lsn = true, .lsn = frame->lsn};
		bool		holds;
		bool		done;

		if (!txn_memtable_map_holds(frame->io, record, TXN_MEMTABLE_MAP_MAP_FREE, block, frame->lsn, &holds,
									frame->error))
			return;
		if (holds)
			continue;
		txn_vacuum_of_block(&frame->freed->runs, block, &frame->runs);
		frame->runs_live = true;
		txn_vacuum_of_block(&frame->freed->dead, block, &frame->dead);
		frame->dead_live = true;
		if (!txn_vacuum_below(frame->io, frame->store, frame->base_live ? &frame->base : NULL, block,
							  &frame->has_below, &frame->below, frame->error) ||
			!txn_vacuum_first(frame->has_below, &frame->below, block, frame->base_live ? &frame->base : NULL,
							  frame->page, &captures.start, frame->error))
			return;
		captures.has_below = frame->has_below;
		if (!txn_vacuum_given(frame->io, &captures, &frame->dead, &done, &frame->claims, frame->error))
			return;
		if (!done)
		{
			*frame->error = txn_vacuum_fork("XX000", "a free's redo was asked to read its values again");
			return;
		}
		frame->claims_live = true;
		txn_vacuum_redo_block_drop(frame);
	}
	if (frame->freed->has_values)
	{
		const StdVec *values = &frame->freed->values.f1;
		HostSharedRecord toast;

		if (frame->toast == NULL)
		{
			*frame->error = txn_vacuum_fork("XX000", "a free's redo has no TOAST storage");
			return;
		}
		toast = host_shared_Held_record(frame->toast);
		for (size_t i = 0; i < values->len; i++)
		{
			const std_TupleU32U64_t *value = &STD_VEC_AT(values, std_TupleU32U64_t, i);
			TxnMemtableMapTake take = {.state = toast, .map = TXN_MEMTABLE_MAP_MAP_VALUES, .key = value->f0,
			.most = TXN_VALUES_MOST};
			txn_vacuum_redo_freed_closure_5_captures_t inside = {.lsn = frame->lsn, .based = &frame->based,
			.by = value->f1};
			std_TupleU64U64_t mark;
			TxnMemtableMapTaken taken;
			bool		holds;

			if (!txn_memtable_map_holds(frame->io, toast, TXN_MEMTABLE_MAP_MAP_VALUES, value->f0, frame->lsn, &holds,
										frame->error))
				return;
			if (holds)
				continue;
			if (!txn_values_based(frame->io, frame->store, frame->toast, value->f0, &frame->has_based, &frame->based,
								  &mark, frame->error))
				return;
			inside.has_based = frame->has_based;
			if (!txn_memtable_map_prepare_Taken(frame->io, &take, 1, txn_vacuum_redo_freed_closure_5, &inside, NULL,
												&taken, frame->error))
				return;
			if (taken != TXN_MEMTABLE_MAP_TAKEN_TOOK)
			{
				*frame->error = txn_vacuum_fork("XX001", "a freed row's value has no entry to take down");
				return;
			}
			if (frame->has_based)
				std_vec_free_Checked(&frame->based, 1, 1);
			frame->has_based = false;
		}
	}
	if (!txn_state_deferring(frame->io, frame->held, frame->lsn, &deferring, frame->error))
		return;
	frame->ok = deferring || txn_vacuum_taken_out(frame->io, frame->held, frame->freed, frame->lsn, frame->error);
}

/* txn_vacuum_redo_pad frees what redo_freed holds live. */
static void
txn_vacuum_redo_pad(void *frame_)
{
	txn_vacuum_redo_frame_t *frame = frame_;

	txn_vacuum_redo_block_drop(frame);
	if (frame->has_based)
		std_vec_free_Checked(&frame->based, 1, 1);
	if (frame->blocks_live)
		std_vec_free_Checked(&frame->blocks, sizeof(uint32), _Alignof(uint32));
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->has_based = frame->blocks_live = frame->base_live = false;
}

/*
 * txn_vacuum_redo_freed replays a free of tuple ids: each block's freed runs
 * go back into its free space entry, their offsets free and their bytes
 * given back, and its dead runs' bytes with their markers, by given at the
 * record's LSN; a block whose entry's chain holds a version with that LSN is
 * given back already. Its values are taken down unless their map holds the
 * record. Then, unless a standby defers them past its fold point, each dead
 * tuple id whose marker claimed it, and each freed tuple id's row version
 * still there, is taken out of its chain.
 */
bool
txn_vacuum_redo_freed(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
					  const HostSharedHeld *toast, const TxnRecordTidsFreed *freed, const TxnInsertPage *page,
					  uint64 lsn, HostError *error)
{
	txn_vacuum_redo_frame_t frame = {.io = io, .store = store, .held = held, .toast = toast, .freed = freed,
	.page = page, .lsn = lsn, .error = error};

	std_landing(&frame, txn_vacuum_redo_body, txn_vacuum_redo_pad);
	txn_vacuum_redo_pad(&frame);
	return frame.ok;
}
