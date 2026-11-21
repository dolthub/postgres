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
#include "doltgres/txn/fold.h"
#include "doltgres/txn/free_space.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/tuples.h"
#include "doltgres/txn/spill.h"
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
 * txn_vacuum_spilled_below replaces a block's entry below the memtable by its
 * newest version in the storage's spills when they hold one, which a step on
 * a block whose memtable holds no version starts from, as an insert's take
 * does; a spilled removal leaves none.
 */
static bool
txn_vacuum_spilled_below(HostIoIo *io, HostSharedRecord record, uint32 block, bool *has_below, StdVec *below,
						 HostError *error)
{
	TxnMemtableMapEntry entry;
	bool		some;

	if (!txn_memtable_map_spilled_newest(io, record, block, &some, &entry, error))
		return false;
	if (!some)
		return true;
	if (*has_below)
		std_vec_free_Checked(below, 1, 1);
	*has_below = entry.has_value;
	if (entry.has_value)
		*below = entry.value;
	return true;
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

/* txn_vacuum_spilled reports whether a block's spilled takes took a tuple id, which VACUUM frees only once a fold took their file. */
static bool
txn_vacuum_spilled(const StdVec *spilled, uint64 tid)
{
	for (size_t i = 0; i < spilled->len; i++)
		if (STD_VEC_AT(spilled, HostRow, i).f0 == tid)
			return true;
	return false;
}

/*
 * txn_vacuum_committed_ids moves out a take's value ids when the xid whose
 * commit decides them committed, and gives none otherwise, since an aborted
 * row's values went at its abort (bugs row 146).
 */
static bool
txn_vacuum_committed_ids(HostIoIo *io, TxnVacuumTook *take, StdVec *result, HostError *error)
{
	HostStatus	status = HOST_STATUS_ABORTED;

	if (take->ids.len > 0)
	{
		HostXid		named = {take->named};

		if (!host_status(HOST_IO_IO_IO_DOORS(io), named, &status, error))
			return false;
	}
	if (status == HOST_STATUS_COMMITTED)
	{
		*result = take->ids;
		std_vec_new(&take->ids, TXN_VACUUM_ID);
	}
	else
		std_vec_new(result, TXN_VACUUM_ID);
	return true;
}

/*
 * txn_vacuum_stored returns a found tuple id's stored ender from its ender's
 * xid and its free's horizon, which is 0 only for a row its own writer ended
 * (OWN) or for none: the xid marked OWN, the xid, or 0.
 */
static uint64
txn_vacuum_stored(uint64 ender, uint64 horizon)
{
	return ender != 0 && horizon == 0 ? ender | TXN_BASE_OWN : ender;
}

/* txn_vacuum_logged returns a pending tuple id's free's conflict horizon: its ender's xid, or 0 for a row its own writer ended (OWN). */
static uint64
txn_vacuum_logged(uint64 ender)
{
	return (ender & TXN_BASE_OWN) == 0 ? ender : 0;
}

/* txn_vacuum_leaving_frame_t is leaving's state across its pad: the run's takers, the base's entry below, and the taken offsets, while live. */
typedef struct txn_vacuum_leaving_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const TxnBaseBase *base;
	const TxnInsertPage *page;
	TxnMemtableVersionVersion *run;
	size_t		run_len;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	StdVec		takers;
	bool		takers_live;
	bool		has_below;
	StdVec		below;
	StdVec		took;
	bool		took_live;
	bool		result_live;
} txn_vacuum_leaving_frame_t;

/* txn_vacuum_leaving_drop frees the takes leaving gives. */
void
txn_vacuum_leaving_drop(StdVec *leaving)
{
	for (size_t i = 0; i < leaving->len; i++)
		txn_vacuum_ids_drop(&STD_VEC_AT(leaving, TxnVacuumLeaving, i).ids);
	std_vec_free_Checked(leaving, sizeof(TxnVacuumLeaving), _Alignof(TxnVacuumLeaving));
}

/* txn_vacuum_leaving_pad frees what leaving holds live. */
static void
txn_vacuum_leaving_pad(void *frame_)
{
	txn_vacuum_leaving_frame_t *frame = frame_;

	if (frame->took_live)
		txn_vacuum_took_drop(&frame->took);
	if (frame->has_below)
		std_vec_free_Checked(&frame->below, 1, 1);
	if (frame->takers_live)
		txn_memtable_map_takers_drop(&frame->takers);
	if (frame->result_live)
		txn_vacuum_leaving_drop(frame->result);
	frame->took_live = frame->has_below = frame->takers_live = frame->result_live = false;
}

/* txn_vacuum_leaving_body is leaving's body. */
static void
txn_vacuum_leaving_body(void *frame_)
{
	txn_vacuum_leaving_frame_t *frame = frame_;
	HostRow		taken = {0};
	uint32		block;

	std_vec_new(frame->result, sizeof(TxnVacuumLeaving), _Alignof(TxnVacuumLeaving));
	frame->result_live = true;
	for (size_t k = 0; k < frame->run_len && taken.f0 == 0; k++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&frame->run[k]);

		taken = txn_memtable_map_taken(txn_memtable_version_Record_op(&record));
	}
	if (taken.f0 == 0)
	{
		frame->result_live = false;
		frame->ok = true;
		return;
	}
	block = (uint32) (taken.f0 >> 16);
	std_vec_new(&frame->takers, sizeof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t),
				_Alignof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t));
	frame->takers_live = true;
	if (!std_vec_try_reserve_exact(&frame->takers, frame->run_len, sizeof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t),
								   _Alignof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t)))
	{
		*frame->error = host_Error_out_of_memory();
		return;
	}
	for (size_t k = frame->run_len; k > 0; k--)
	{
		txn_TupleWriterRowEntryU64TupleVecU32Bool_t taker;

		if (!txn_memtable_map_taker(&frame->run[k - 1], &taker, frame->error))
			return;
		std_vec_push_Checked(&frame->takers, &taker, sizeof(taker), _Alignof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t));
	}
	if (!txn_vacuum_below(frame->io, frame->store, frame->base, block, &frame->has_below, &frame->below,
						  frame->error) ||
		!txn_vacuum_taken(&frame->takers, frame->has_below, &frame->below, block, frame->base, frame->page,
						  &frame->took, frame->error))
		return;
	frame->took_live = true;
	if (!std_vec_try_reserve_exact(frame->result, frame->took.len, sizeof(TxnVacuumLeaving),
								   _Alignof(TxnVacuumLeaving)))
	{
		*frame->error = host_Error_out_of_memory();
		return;
	}
	for (size_t i = 0; i < frame->took.len; i++)
	{
		TxnVacuumTook *take = &STD_VEC_AT(&frame->took, TxnVacuumTook, i);
		TxnVacuumLeaving leaving = {take->f0, take->f1, take->f2, take->f3};

		if (!txn_vacuum_committed_ids(frame->io, take, &leaving.ids, frame->error))
			return;
		std_vec_push_Checked(frame->result, &leaving, sizeof(leaving), _Alignof(TxnVacuumLeaving));
	}
	frame->result_live = false;
	frame->ok = true;
}

/*
 * txn_vacuum_leaving returns the taken offsets of a run of a block's free
 * space versions, newest first, that a fold's drop takes out of the delta,
 * as VACUUM's find reads them (taken) against the base's entry below the
 * delta: each tuple id with its take's writer, the bytes its take lowered,
 * its stored ender, and its row's value ids when the writer whose commit
 * decides them committed (committed_ids); a run with no take gives none.
 */
bool
txn_vacuum_leaving(HostIoIo *io, TreesStoreNodeStore store, const TxnBaseBase *base, const TxnInsertPage *page,
				   TxnMemtableVersionVersion *run, size_t run_len, StdVec *result, HostError *error)
{
	txn_vacuum_leaving_frame_t frame = {.io = io, .store = store, .base = base, .page = page, .run = run,
	.run_len = run_len, .result = result, .error = error};

	std_landing(&frame, txn_vacuum_leaving_body, txn_vacuum_leaving_pad);
	txn_vacuum_leaving_pad(&frame);
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
	/* spilled is a Vec<Row> of the tuple ids the block's spilled takes took. */
	StdVec		spilled;
	bool		spilled_live;
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
		!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-vacuum-found", frame->error) ||
		!txn_memtable_map_spilled_takes(frame->io, frame->record, frame->block, &frame->spilled, frame->error))
		return;
	frame->spilled_live = true;
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
		if (running || txn_vacuum_spilled(&frame->spilled, take->f0))
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

			if (!txn_vacuum_contains(&frame->free, take->f0))
				continue;
			if (!txn_vacuum_committed_ids(frame->io, take, &found.f4, frame->error))
				return;
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
	if (frame->spilled_live)
		std_vec_free_Checked(&frame->spilled, sizeof(HostRow), _Alignof(HostRow));
	if (frame->has_below)
		std_vec_free_Checked(&frame->below, 1, 1);
	if (frame->result_live)
		txn_vacuum_found_drop(frame->result);
	frame->free_live = frame->took_live = frame->takers_live = frame->spilled_live = frame->has_below =
		frame->result_live = false;
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
			merged->f2 = Max(merged->f2, pending->ender & ~TXN_BASE_OWN);
			merged->f3 = Max(merged->f3, txn_vacuum_logged(pending->ender));
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
		found.f2 = pending->ender & ~TXN_BASE_OWN;
		found.f3 = txn_vacuum_logged(pending->ender);
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
 * pending tree, each with its heap bytes, its row's ender's full xid, and its
 * free's conflict horizon, that xid, or 0 for a row its own writer ended
 * (TXN_BASE_OWN), that no layer maps and that the free space map doesn't
 * already hold free,
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
		if (txn_state_moved_past(record, generation))
		{
			generation = txn_state_generation(record);
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
			TxnVacuumFreed passed = {found->f0, found->f1, found->f3, found->f4, txn_vacuum_stored(found->f2, found->f3)};

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
 * txn_vacuum_spent_body counts the block's never-held tuple ids, then its
 * pending ones no layer maps or map frees, and counts again under the newest
 * base when an install moved the base before it finished.
 */
static void
txn_vacuum_spent_body(void *frame_)
{
	txn_vacuum_spent_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	bool		some;

	for (;;)
	{
		uint64		generation = txn_state_generation(record);

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
		if (!txn_state_moved_past(record, generation))
			break;
		txn_vacuum_spent_pad(frame);
	}
	frame->ok = true;
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

	/* generation is the base generation the block's entry below was read at. */
	uint64		generation;
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
 * txn_vacuum_given_closure_2_body checks the base generation and the values
 * mark, logs the record, and makes the block's and the values' takes.
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
	TxnStatusRoomed unroomed = {0};
	bool		moved = txn_state_generation(c->record) != c->generation;

	if (!moved && c->toast != NULL)
	{
		std_TupleU64U64_t mark = txn_values_mark(*c->toast);

		moved = mark.f0 != c->mark.f0 || mark.f1 != c->mark.f1;
	}
	if (moved)
	{
		*frame->result = false;
		frame->ok = true;
		return;
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

		if (!txn_memtable_map_finish_Unit(io, frame->prepared, 0, wrote, false, unroomed, has_lsn, lsn, c->has_below,
										  c->below->ptr,
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

		if (!txn_memtable_map_took_Unit(io, frame->prepared, freeing + at, wrote, false, unroomed, has_lsn, lsn, true,
										tid, c->start,
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

		if (!txn_memtable_map_finish_Unit(io, frame->prepared, freeing + c->markers->len + at, wrote, false, unroomed,
										  has_lsn, lsn,
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
 * it opens the window, when an install moved the base since the block's entry
 * was read or the TOAST storage's mark moved, for its caller to read them
 * again.
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

	/*
	 * enders is a Vec<u64>: the stored enders of a block's freed tuple ids,
	 * which its record names.
	 */
	bool		enders_live;
	StdVec		enders;
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
	if (frame->enders_live)
		std_vec_free_Checked(&frame->enders, sizeof(uint64), _Alignof(uint64));
	frame->claims_live = frame->body_live = frame->downs_live = frame->has_below = false;
	frame->counts_live = frame->ids_live = frame->enders_live = false;
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

/*
 * txn_vacuum_freed_block gives back one block's runs, reading the base and the
 * block's and its values' entries again until neither the base nor their mark
 * moved; it pauses at doltgres-vacuum-freeing after each read of the base.
 */
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
	std_vec_new(&frame->enders, sizeof(uint64), _Alignof(uint64));
	frame->enders_live = true;
	for (size_t i = 0; i < frame->kept.len; i++)
	{
		const TxnVacuumFreed *freed = STD_VEC_AT(&frame->kept, const TxnVacuumFreed *, i);

		if ((uint32) (freed->f0 >> 16) == block)
			std_vec_push_Checked(&frame->enders, &freed->f4, sizeof(uint64), _Alignof(uint64));
	}
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
		txn_vacuum_given_closure_2_captures_t captures = {.record = record,
			.generation = txn_state_generation(record), .block = block, .below = &frame->below, .page = frame->page,
			.freed = frame->dead ? &none : &these, .toast = toasted ? &toast_record : NULL, .downs = &frame->downs,
		.windowed = true};
		bool		has_mark = false;
		bool		some;
		bool		done;

		if (frame->base_live)
			txn_base_Base_drop(&frame->base);
		frame->base_live = false;
		if (!txn_state_base_record(frame->io, frame->held, &some, &frame->base, frame->error))
			return false;
		frame->base_live = some;
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-vacuum-freeing", frame->error) ||
			!txn_vacuum_below(frame->io, frame->store, frame->base_live ? &frame->base : NULL, block,
							  &frame->has_below, &frame->below, frame->error) ||
			!txn_vacuum_first(frame->has_below, &frame->below, block, frame->base_live ? &frame->base : NULL,
							  frame->page, &captures.start, frame->error) ||
			!txn_vacuum_spilled_below(frame->io, record, block, &frame->has_below, &frame->below, frame->error))
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
				.runs = frame->dead ? none : these, .dead = frame->dead ? these : none, .enders = frame->enders,
				.has_values = toasted,
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

	std_vec_new(frame->result, sizeof(uint64), _Alignof(uint64));
	frame->result_live = true;
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

/*
 * txn_vacuum_pruned_closure_8_captures_t is a prune's clear's prepare's
 * closure's captures, with the base generation its entry below was read at,
 * and moved, which the closure sets when an install moved the base since.
 */
typedef struct txn_vacuum_pruned_closure_8_captures_t
{
	txn_vacuum_BlockStep_t step;
	uint64		generation;
	bool		has_below;
	const StdVec *below;
	bool		moved;
} txn_vacuum_pruned_closure_8_captures_t;

/*
 * txn_vacuum_pruned_closure_8 is a prune's clear's prepare's closure: the
 * block's take that clears its full flag, or none when an install moved the
 * base since its entry was read.
 */
static bool
txn_vacuum_pruned_closure_8(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
							TxnMemtableMapTaken *result, HostError *error)
{
	txn_vacuum_pruned_closure_8_captures_t *inside = captures;
	TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
	TxnStatusRoomed unroomed = {0};

	inside->moved = txn_state_generation(inside->step.record) != inside->generation;
	if (inside->moved)
	{
		*result = TXN_MEMTABLE_MAP_TAKEN_RETRY;
		return true;
	}
	return txn_memtable_map_finish_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, none, false, unroomed, false, 0,
										inside->has_below,
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
	/* spilled is a Vec<Row> of the tuple ids the block's spilled takes took. */
	StdVec		spilled;
	bool		spilled_live;
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

/* txn_vacuum_pruned_pad frees what pruned holds live. */
static void
txn_vacuum_pruned_pad(void *frame_)
{
	txn_vacuum_pruned_frame_t *frame = frame_;

	txn_vacuum_pruned_key_drop(frame);
	if (frame->spilled_live)
		std_vec_free_Checked(&frame->spilled, sizeof(HostRow), _Alignof(HostRow));
	frame->spilled_live = false;
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
 * txn_vacuum_pruned_body checks the block's prune condition, finds its dead
 * tuple ids, marks them, and takes out those it claimed; only a changer's end
 * makes a tuple id dead, so a dead one a prune claims has an ender; a clear of the full
 * flag whose base an install moved since its entry was read checks the block
 * again from the newest base (bugs row 240). It pauses at
 * doltgres-prune-based after each read of the base.
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

	for (;;)
	{
		uint64		generation = txn_state_generation(record);

		if (!txn_state_base_record(frame->io, frame->held, &some, &frame->base, frame->error))
			return;
		frame->base_live = some;
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-prune-based", frame->error) ||
			!txn_vacuum_below(frame->io, frame->store, some ? &frame->base : NULL, frame->block, &frame->has_below,
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
		if (!txn_memtable_map_spilled_takes(frame->io, record, frame->block, &frame->spilled, frame->error))
			return;
		frame->spilled_live = true;
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
			if (take->f2 == 0 || paired || txn_vacuum_spilled(&frame->spilled, take->f0))
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
				TxnVacuumFreed dead = {take->f0, take->f2, ender, .f4 = ender == take->f1.xid ? ender | TXN_BASE_OWN : ender};
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
			.most = frame->page->most}, .generation = generation, .has_below = frame->has_below, .below = &frame->below};
			TxnMemtableMapTake take = {.state = record, .map = TXN_MEMTABLE_MAP_MAP_FREE, .key = frame->block,
			.most = txn_free_space_most_len(frame->page->most)};
			TxnMemtableMapTaken taken;

			if (!txn_vacuum_spilled_below(frame->io, record, frame->block, &frame->has_below, &frame->below,
										  frame->error))
				return;
			inside.has_below = frame->has_below;
			if (!txn_memtable_map_prepare_Taken(frame->io, &take, 1, txn_vacuum_pruned_closure_8, &inside, NULL,
												&taken, frame->error))
				return;
			if (inside.moved)
			{
				txn_vacuum_pruned_pad(frame);
				continue;
			}
		}
		frame->ok = true;
		return;
	}
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

	/*
	 * ender is the stored ender whose row the version must hold, or NULL for
	 * any version.
	 */
	const uint64 *ender;
	bool		unhold;
	bool		key_live;
	StdVec		key;
	bool		hold_live;
	HostDeltaHold hold;
	bool		chain_live;
	StdVec		chain;
	bool		seen_live;
	TxnVisibleSeen seen;
	bool		removed_live;
	StdVec		removed;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_vacuum_holding_frame_t;

/*
 * txn_vacuum_holding_body finds the version holding the tuple id, the one
 * whose row a stored ender ended when it has one, and, to unhold it, takes it
 * out and takes back its count.
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

	/*
	 * An ender with OWN is the row's own writer, found by its writer, since
	 * every rule sees no row of an aborted writer.
	 */
	if (frame->ender == NULL || (*frame->ender & TXN_BASE_OWN) != 0)
	{
		for (size_t i = 0; i < frame->chain.len && version == NULL; i++)
		{
			TxnMemtableVersionVersion *held = &STD_VEC_AT(&frame->chain, TxnMemtableVersionVersion, i);
			TxnMemtableVersionRecord stored = txn_memtable_version_Version_record(held);

			if (txn_vacuum_holds(held, frame->tid) &&
				(frame->ender == NULL ||
				 txn_memtable_version_Record_writer(&stored).xid == (*frame->ender & ~TXN_BASE_OWN)))
				version = held;
		}
	}
	else
	{
		TxnVisibleRule every = {&TXN_FOLD_EVERY_RULE_OPS, NULL};

		if (!txn_visible_seen(frame->io, every, frame->chain.ptr, frame->chain.len, NULL, &frame->seen,
							  frame->error))
			return;
		frame->seen_live = true;
		for (size_t i = 0; i < frame->seen.rows.len && version == NULL; i++)
		{
			const TxnVisibleSeenRow *row = &STD_VEC_AT(&frame->seen.rows, TxnVisibleSeenRow, i);
			TxnMemtableVersionRecord changer;

			if (row->row.put.tuple.f0 != frame->tid || row->changer == NULL)
				continue;
			changer = txn_memtable_version_Version_record(row->changer);
			if (txn_memtable_version_Record_writer(&changer).xid == (*frame->ender & ~TXN_BASE_OWN))
				version = row->row.version;
		}
	}
	*frame->result = version != NULL;
	if (version != NULL && frame->unhold)
	{
		if (!txn_memtable_table_removed(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, frame->record, frame->key.ptr,
										frame->key.len, &version->handle, 1, &frame->removed, frame->error))
			return;
		frame->removed_live = true;
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
	if (frame->seen_live)
		txn_visible_Seen_drop(&frame->seen);
	if (frame->chain_live)
		txn_memtable_version_versions_drop(&frame->chain, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	frame->removed_live = frame->seen_live = frame->chain_live = frame->hold_live = frame->key_live = false;
}

/*
 * txn_vacuum_holding finds the row version that holds a tuple id, in the
 * chain of the key its tuple id table names for it, one whose row a stored
 * ender ended when ender isn't NULL, reporting whether one does, and to
 * unhold it takes it out of its key's chain, taking back its count at its
 * writer's entry when this removal took it out.
 */
static bool
txn_vacuum_holding(HostIoIo *io, HostSharedRecord record, uint64 tid, const uint64 *ender, bool unhold, bool *result,
				   HostError *error)
{
	txn_vacuum_holding_frame_t frame = {.io = io, .record = record, .tid = tid, .ender = ender, .unhold = unhold,
	.result = result, .error = error};

	std_landing(&frame, txn_vacuum_holding_body, txn_vacuum_holding_pad);
	txn_vacuum_holding_pad(&frame);
	return frame.ok;
}

/*
 * txn_vacuum_unheld takes out of its key's chain the row version that holds a
 * tuple id, if one does, taking back its count; with a stored ender, only the
 * version of the freed incarnation, whose row that ender ended, so a later
 * take of the tuple id a lagging standby already replayed is passed over
 * (bugs row 320). An ender of 0 takes out nothing, since every such free's
 * incarnation, an aborted row nothing ended, a fold already dropped, on the
 * primary and at the standby's same point.
 */
static bool
txn_vacuum_unheld(HostIoIo *io, HostSharedRecord record, uint64 tid, const uint64 *ender, HostError *error)
{
	bool		found;

	if (ender != NULL && *ender == 0)
		return true;
	return txn_vacuum_holding(io, record, tid, ender, true, &found, error);
}

/*
 * txn_vacuum_held reports whether a row version in a storage's delta holds a
 * tuple id, as unheld finds the version it takes out.
 */
bool
txn_vacuum_held(HostIoIo *io, HostSharedRecord record, uint64 tid, bool *result, HostError *error)
{
	return txn_vacuum_holding(io, record, tid, NULL, false, result, error);
}

/* txn_vacuum_dead_ender returns a claimed dead tuple id's stored ender, field 10's after every freed tuple id's, from the free's dead runs, which hold it. */
static const uint64 *
txn_vacuum_dead_ender(const TxnRecordTidsFreed *freed, uint64 tid)
{
	size_t		at = 0;

	for (size_t i = 0; i < freed->runs.len; i++)
		at += STD_VEC_AT(&freed->runs, std_TupleU32U16U16U64_t, i).f2;
	for (size_t i = 0; i < freed->dead.len; i++)
	{
		const std_TupleU32U16U16U64_t *run = &STD_VEC_AT(&freed->dead, std_TupleU32U16U16U64_t, i);
		uint64		first = (uint64) run->f0 << 16 | run->f1;

		if (tid >= first && tid < first + run->f2)
			return &STD_VEC_AT(&freed->enders, uint64, at + (size_t) (tid - first));
		at += run->f2;
	}
	Assert(false);
	return NULL;
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
			if (!txn_vacuum_unheld(frame->io, record, STD_VEC_AT(&frame->claimed, HostRow, i).f0,
								   txn_vacuum_dead_ender(frame->freed, STD_VEC_AT(&frame->claimed, HostRow, i).f0),
								   frame->error))
				return;
		for (size_t i = 0, ender = 0; i < frame->freed->runs.len; i++)
		{
			const std_TupleU32U16U16U64_t *run = &STD_VEC_AT(&frame->freed->runs, std_TupleU32U16U16U64_t, i);
			uint16		end;

			if (run->f0 != block)
			{
				ender += run->f2;
				continue;
			}
			end = STD_ADD(u16, run->f1, run->f2, 801, 55);
			for (uint16 at = run->f1; at < end; at++, ender++)
				if (!txn_vacuum_unheld(frame->io, record, (uint64) block << 16 | at,
									   &STD_VEC_AT(&frame->freed->enders, uint64, ender), frame->error))
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
	TxnStatusRoomed unroomed = {0};

	return txn_memtable_map_finish_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, none, false, unroomed, true,
										inside->lsn,
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
 * record, reading the base and the block's entry again while an install
 * moved the base since they were read, and pausing at
 * doltgres-vacuum-freeing after each read of the base; it then takes the
 * values down and does the take-outs.
 */
static void
txn_vacuum_redo_body(void *frame_)
{
	txn_vacuum_redo_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	bool		deferring;

	txn_vacuum_blocks(frame->freed, &frame->blocks);
	frame->blocks_live = true;
	for (size_t b = 0; b < frame->blocks.len; b++)
	{
		uint32		block = STD_VEC_AT(&frame->blocks, uint32, b);
		bool		holds;
		bool		done = false;

		if (!txn_memtable_map_holds(frame->io, record, TXN_MEMTABLE_MAP_MAP_FREE, block, frame->lsn, &holds,
									frame->error))
			return;
		if (holds)
			continue;
		txn_vacuum_of_block(&frame->freed->runs, block, &frame->runs);
		frame->runs_live = true;
		txn_vacuum_of_block(&frame->freed->dead, block, &frame->dead);
		frame->dead_live = true;
		while (!done)
		{
			txn_vacuum_given_closure_2_captures_t captures = {.record = record,
				.generation = txn_state_generation(record), .block = block, .below = &frame->below,
			.page = frame->page, .freed = &frame->runs, .has_lsn = true, .lsn = frame->lsn};
			bool		some;

			if (frame->base_live)
				txn_base_Base_drop(&frame->base);
			frame->base_live = false;
			if (frame->has_below)
				std_vec_free_Checked(&frame->below, 1, 1);
			frame->has_below = false;
			if (!txn_state_base_record(frame->io, frame->held, &some, &frame->base, frame->error))
				return;
			frame->base_live = some;
			if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-vacuum-freeing", frame->error) ||
				!txn_vacuum_below(frame->io, frame->store, frame->base_live ? &frame->base : NULL, block,
								  &frame->has_below, &frame->below, frame->error) ||
				!txn_vacuum_first(frame->has_below, &frame->below, block, frame->base_live ? &frame->base : NULL,
								  frame->page, &captures.start, frame->error) ||
				!txn_vacuum_spilled_below(frame->io, record, block, &frame->has_below, &frame->below, frame->error))
				return;
			captures.has_below = frame->has_below;
			if (!txn_vacuum_given(frame->io, &captures, &frame->dead, &done, &frame->claims, frame->error))
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

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/chunks/header.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/delta.h"
#include "doltgres/repo/lib.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/txn/change.h"
#include "doltgres/txn/fold.h"
#include "doltgres/txn/status.h"
#include "doltgres/txn/written.h"

/* TXN_VACUUM_TESTS_UNWRAP_FAILED is a Result's unwrap's panic on a HostError at a line and column. */
#define TXN_VACUUM_TESTS_UNWRAP_FAILED(line, col, error) \
	STD_RESULT_EXPECT_FAILED((line), (col), "called `Result::unwrap()` on an `Err` value", (error), \
							 host_Error_Debug_fmt)

/* TXN_VACUUM_TESTS_PAGE is PAGE, an 8 kB heap page: its bytes past its header and its most tuples. */
static const TxnInsertPage TXN_VACUUM_TESTS_PAGE = {.bytes = 8168, .most = 291};

/* txn_vacuum_tests_case_t is a case run on a new database's storage, with a reader of its repository. */
typedef void (*txn_vacuum_tests_case_t) (void *captures, HostIoIo *io, TreesStoreNodeStore store,
										 const HostSharedHeld *held);

/* txn_vacuum_tests_entry_t is an Option<Free>: a block's newest free space entry, or none. */
typedef struct txn_vacuum_tests_entry_t
{
	bool		has;
	TxnFreeSpaceFree free;
} txn_vacuum_tests_entry_t;

/*
 * txn_vacuum_tests_logged_t is Logged: one of a database's records, with its
 * end, a row change or a free.
 */
typedef struct txn_vacuum_tests_logged_t
{
	/* freed is whether the record is a free, else a row change. */
	bool		freed;
	TxnRecordRowChange row;
	TxnRecordTidsFreed free;
	HostLsn		end;
} txn_vacuum_tests_logged_t;

/* txn_vacuum_tests_every returns a horizon that has passed every xid. */
SimcTaken
txn_vacuum_tests_every(void)
{
	HostXid		horizon = {UINT64_MAX};

	return simc_non_vacuumable(horizon);
}

/* txn_vacuum_tests_with runs a case on a new database's storage, with a reader of its repository. */
static void
txn_vacuum_tests_with(HostIoIo *io, uint32 database, txn_vacuum_tests_case_t case_, void *captures)
{
	HostStorage storage = txn_insert_tests_stored(database);
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	ChunksErrorError failed;
	HostError	error;

	if (!repo_Repo_create(io, database, HOST_PG_DEFAULT, &repo, &failed))
		STD_RESULT_EXPECT_FAILED(832, 91, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	if (!host_shared_read(io, &reading, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(833, 46, &error);
	if (!txn_state_state(io, &repo, &reading, &storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(834, 110, &error);
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);

		case_(captures, io, TREES_STORE_READER_NODE_STORE(&store), &held);
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
}

/*
 * txn_vacuum_tests_inserted inserts a key's row of some heap bytes into a
 * database's storage as a transaction's, and returns its tuple id.
 */
static uint64
txn_vacuum_tests_inserted(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 database,
						  uint64 key, uint64 bytes, HostXid xid)
{
	TxnInsertInsert insert = txn_insert_tests_row(txn_insert_tests_stored(database), key);
	TxnInsertRoom room = {.bytes = bytes, .page = TXN_VACUUM_TESTS_PAGE.bytes, .most = TXN_VACUUM_TESTS_PAGE.most};
	HostError	error;
	uint64		tid;

	insert.writer.xid = xid.f0;
	insert.put.heap_bytes = (uint32) bytes;
	if (!txn_insert_tests_taken(io, store, held, &insert, &room, NULL, NULL, 0, &tid, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(851, 110, &error);
	return tid;
}

/* txn_vacuum_tests_ended ends a test's transaction, committed or aborted, as its callbacks do. */
static void
txn_vacuum_tests_ended(HostIoIo *io, HostXid xid, bool committed)
{
	HostError	error;

	if (committed)
	{
		simc_commit(xid);
		if (!txn_written_ended(io, &error))
			TXN_VACUUM_TESTS_UNWRAP_FAILED(859, 36, &error);
	}
	else
	{
		simc_abort(xid);
		if (!txn_written_aborted(io, 0, &error))
			TXN_VACUUM_TESTS_UNWRAP_FAILED(863, 38, &error);
	}
}

/* txn_vacuum_tests_entry returns a block's newest free space entry in a storage's delta, read at the storage's counter. */
static txn_vacuum_tests_entry_t
txn_vacuum_tests_entry(HostIoIo *io, const HostSharedHeld *held, uint32 block)
{
	HostSharedRecord record = host_shared_Held_record(held);
	txn_vacuum_tests_entry_t entry = {0};
	TxnMemtableMapEntry newest;
	HostError	error;
	bool		some;

	if (!txn_memtable_map_newest(io, record, TXN_MEMTABLE_MAP_MAP_FREE, block, &some, &newest, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(870, 70, &error);
	if (!some || !newest.has_value)
		return entry;
	if (!txn_free_space_Free_decode(newest.value.ptr, newest.value.len, block, txn_state_counter(record),
									TXN_VACUUM_TESTS_PAGE.most, &entry.free, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(871, 84, &error);
	std_vec_free_Checked(&newest.value, 1, 1);
	entry.has = true;
	return entry;
}

/* txn_vacuum_tests_some returns an entry of a block's free space. */
static txn_vacuum_tests_entry_t
txn_vacuum_tests_some(TxnFreeSpaceFree free)
{
	return (txn_vacuum_tests_entry_t)
	{
		true, free
	};
}

/* txn_vacuum_tests_entry_Debug_fmt is an Option<Free>'s Debug. */
static bool
txn_vacuum_tests_entry_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_vacuum_tests_entry_t *entry = value;
	StdOptionRef option = {entry->has, {&entry->free, txn_free_space_Free_Debug_fmt}};

	return std_option_Debug_fmt(&option, f);
}

/* txn_vacuum_tests_entry_eq reports whether two Option<Free>s are equal. */
static bool
txn_vacuum_tests_entry_eq(const txn_vacuum_tests_entry_t *a, const txn_vacuum_tests_entry_t *b)
{
	return a->has == b->has && (!a->has || txn_free_space_Free_PartialEq_eq(&a->free, &b->free));
}

/* txn_vacuum_tests_entry_is asserts at a line and column that a block's entry is the one wanted. */
static void
txn_vacuum_tests_entry_is(uint32 line, uint32 col, txn_vacuum_tests_entry_t got, txn_vacuum_tests_entry_t want)
{
	STD_ASSERT_EQ(line, col, txn_vacuum_tests_entry_eq(&got, &want), &got, txn_vacuum_tests_entry_Debug_fmt, &want,
				  txn_vacuum_tests_entry_Debug_fmt);
}

/* txn_vacuum_tests_logged_free frees a Vec<Logged> and its records. */
static void
txn_vacuum_tests_logged_free(StdVec *records)
{
	for (size_t k = 0; k < records->len; k++)
	{
		txn_vacuum_tests_logged_t *record = &STD_VEC_AT(records, txn_vacuum_tests_logged_t, k);

		if (record->freed)
			txn_record_TidsFreed_drop(&record->free);
		else
			txn_record_RowChange_drop(&record->row);
	}
	std_vec_free_Checked(records, sizeof(txn_vacuum_tests_logged_t), _Alignof(txn_vacuum_tests_logged_t));
}

/* txn_vacuum_tests_logged returns the row changes and frees the simulation logged for a database's storages, in their order. */
static StdVec
txn_vacuum_tests_logged(uint32 database)
{
	StdVec		logged = simc_logged();
	StdVec		records;

	std_vec_new(&records, sizeof(txn_vacuum_tests_logged_t), _Alignof(txn_vacuum_tests_logged_t));
	for (size_t k = 0; k < logged.len; k++)
	{
		simc_TupleU8VecU8Lsn_t *one = &STD_VEC_AT(&logged, simc_TupleU8VecU8Lsn_t, k);
		txn_vacuum_tests_logged_t record = {.end = one->f2};
		HostWalKind kind;
		HostError	error;

		if (host_WalKind_of(one->f0, &kind) && kind == HOST_WAL_KIND_ROW_CHANGE)
		{
			if (!txn_record_RowChange_decode(one->f1.ptr, one->f1.len, &record.row, &error))
				host_Error_drop(&error);
			else if (record.row.storage.database != database)
				txn_record_RowChange_drop(&record.row);
			else
				std_vec_push_Checked(&records, &record, sizeof(record), _Alignof(txn_vacuum_tests_logged_t));
		}
		else if (host_WalKind_of(one->f0, &kind) && kind == HOST_WAL_KIND_TIDS_FREED)
		{
			record.freed = true;
			if (!txn_record_TidsFreed_decode(one->f1.ptr, one->f1.len, TXN_VACUUM_TESTS_PAGE.most,
											 TXN_VACUUM_TESTS_PAGE.bytes, &record.free, &error))
				host_Error_drop(&error);
			else if (record.free.storage.database != database)
				txn_record_TidsFreed_drop(&record.free);
			else
				std_vec_push_Checked(&records, &record, sizeof(record), _Alignof(txn_vacuum_tests_logged_t));
		}
		std_vec_free_Checked(&one->f1, 1, 1);
	}
	std_vec_free_Checked(&logged, sizeof(simc_TupleU8VecU8Lsn_t), _Alignof(simc_TupleU8VecU8Lsn_t));
	return records;
}

/* txn_vacuum_tests_freed_Debug_fmt is a Freed's Debug, the tuple (tid, bytes, horizon, values). */
static bool
txn_vacuum_tests_freed_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnVacuumFreed *freed = value;
	StdTupleRef tuple = STD_TUPLE(4, STD_ITEM(&freed->f0, std_u64_Debug_fmt), STD_ITEM(&freed->f1, std_u64_Debug_fmt),
								  STD_ITEM(&freed->f2, std_u64_Debug_fmt),
								  STD_ITEM(&freed->f3, chunks_header_VecU32_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_vacuum_tests_found_Debug_fmt is a Vec<Freed>'s Debug. */
static bool
txn_vacuum_tests_found_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *found = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < found->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(found, TxnVacuumFreed, k), txn_vacuum_tests_freed_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_vacuum_tests_found_free frees a Vec<Freed> and each one's value ids. */
static void
txn_vacuum_tests_found_free(StdVec *found)
{
	for (size_t k = 0; k < found->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(found, TxnVacuumFreed, k).f3, sizeof(uint32), _Alignof(uint32));
	std_vec_free_Checked(found, sizeof(TxnVacuumFreed), _Alignof(TxnVacuumFreed));
}

/*
 * txn_vacuum_tests_found_is_with asserts at a line and column, with a
 * message or none, that the tuple ids found are those wanted, none naming a
 * value.
 */
static void
txn_vacuum_tests_found_is_with(uint32 line, uint32 col, const StdVec *found, const TxnVacuumFreed *want,
							   size_t want_len, const StdFmtArguments *message)
{
	StdVec		wanted = {(void *) want, want_len, want_len};
	bool		eq = found->len == want_len;

	for (size_t k = 0; eq && k < want_len; k++)
	{
		const TxnVacuumFreed *got = &STD_VEC_AT(found, TxnVacuumFreed, k);

		eq = got->f0 == want[k].f0 && got->f1 == want[k].f1 && got->f2 == want[k].f2 && got->f3.len == 0;
	}
	if (!eq)
		std_assert_failed(RUST_FILE, line, col, "==", found, txn_vacuum_tests_found_Debug_fmt, &wanted,
						  txn_vacuum_tests_found_Debug_fmt, message);
}

/* txn_vacuum_tests_found_is asserts at a line and column that the tuple ids found are those wanted, none naming a value. */
static void
txn_vacuum_tests_found_is(uint32 line, uint32 col, const StdVec *found, const TxnVacuumFreed *want, size_t want_len)
{
	txn_vacuum_tests_found_is_with(line, col, found, want, want_len, NULL);
}

/* txn_vacuum_tests_found returns the tuple ids never_held finds under a horizon, unwrapped at a line and column. */
static StdVec
txn_vacuum_tests_found(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const SimcTaken *horizon,
					   uint32 line, uint32 col)
{
	HostSnapshot lent = simc_Taken_lent(horizon);
	HostError	error;
	StdVec		found;

	if (!txn_vacuum_never_held(io, store, held, &TXN_VACUUM_TESTS_PAGE, &lent, &found, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(line, col, &error);
	return found;
}

/* txn_vacuum_tests_found_every returns the tuple ids never_held finds past every xid, unwrapped at a line and column. */
static StdVec
txn_vacuum_tests_found_every(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 line,
							 uint32 col)
{
	SimcTaken	every = txn_vacuum_tests_every();
	StdVec		found = txn_vacuum_tests_found(io, store, held, &every, line, col);

	simc_Taken_drop(&every);
	return found;
}

/*
 * txn_vacuum_tests_claimed frees a test storage's tuple ids, or for dead
 * gives back their bytes, as a logged storage with no TOAST storage, and
 * returns the dead tuple ids claimed, unwrapped at a line and column.
 */
static StdVec
txn_vacuum_tests_claimed(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 database,
						 const TxnVacuumFreed *tids, size_t tids_len, bool dead, uint32 line, uint32 col)
{
	HostStorage toast = {0};
	HostError	error;
	StdVec		claimed;

	if (!txn_vacuum_freed(io, store, held, NULL, toast, txn_insert_tests_stored(database), true,
						  &TXN_VACUUM_TESTS_PAGE, tids, tids_len, dead, &claimed, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(line, col, &error);
	return claimed;
}

/* txn_vacuum_tests_freed frees a test storage's found tuple ids as claimed does, and drops what it claimed. */
static void
txn_vacuum_tests_freed(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 database,
					   const StdVec *found, bool dead, uint32 line, uint32 col)
{
	StdVec		claimed = txn_vacuum_tests_claimed(io, store, held, database, found->ptr, found->len, dead, line, col);

	std_vec_free_Checked(&claimed, sizeof(uint64), _Alignof(uint64));
}

/* txn_vacuum_tests_redone redoes a free on a storage, unwrapped at a line and column. */
static void
txn_vacuum_tests_redone(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
						const TxnRecordTidsFreed *free, HostLsn end, uint32 line, uint32 col)
{
	HostError	error;

	if (!txn_vacuum_redo_freed(io, store, held, NULL, free, &TXN_VACUUM_TESTS_PAGE, end, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(line, col, &error);
}

/* txn_vacuum_tests_redo redoes a row change on a storage, unwrapped at a line and column. */
static void
txn_vacuum_tests_redo(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
					  const TxnRecordRowChange *change, HostLsn end, uint32 line, uint32 col)
{
	HostError	error;

	if (!txn_insert_redo(io, store, held, change, end, &TXN_VACUUM_TESTS_PAGE, NULL, NULL, 0, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(line, col, &error);
}

/*
 * txn_vacuum_tests_aborted_t is the aborted insert case's captures: the
 * entry the primary's free left, the records logged, and the free's place
 * among them.
 */
typedef struct txn_vacuum_tests_aborted_t
{
	txn_vacuum_tests_entry_t primary;
	StdVec		records;
	size_t		at;
} txn_vacuum_tests_aborted_t;

/* txn_vacuum_tests_aborted_primary is the aborted insert case's primary: it frees the aborted row's tuple id and takes it again. */
static void
txn_vacuum_tests_aborted_primary(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	static const TxnVacuumFreed want[] = {{2, 300, 0}};
	static const uint16 two[] = {2};
	txn_vacuum_tests_aborted_t *c = captures;
	HostXid		one = simc_begin();
	HostXid		second;
	HostXid		three;
	HostXid		four;
	StdVec		found;

	STD_ASSERT_EQ_U64(910, 13, txn_vacuum_tests_inserted(io, store, held, 9428, 1, 100, one), 1);
	txn_vacuum_tests_ended(io, one, true);
	second = simc_begin();
	STD_ASSERT_EQ_U64(913, 13, txn_vacuum_tests_inserted(io, store, held, 9428, 2, 300, second), 2);
	txn_vacuum_tests_ended(io, second, false);
	three = simc_begin();
	STD_ASSERT_EQ_U64(916, 13, txn_vacuum_tests_inserted(io, store, held, 9428, 3, 100, three), 3);
	txn_vacuum_tests_ended(io, three, true);
	found = txn_vacuum_tests_found_every(io, store, held, 918, 79);
	txn_vacuum_tests_found_is(919, 13, &found, want, lengthof(want));
	txn_vacuum_tests_freed(io, store, held, 9428, &found, false, 920, 90);
	txn_vacuum_tests_entry_is(921, 13, txn_vacuum_tests_entry(io, held, 0),
							  txn_vacuum_tests_some(txn_free_space_Free_new(8168 - 500 + 300, two, 1, 291)));
	txn_vacuum_tests_found_free(&found);
	found = txn_vacuum_tests_found_every(io, store, held, 922, 78);
	txn_vacuum_tests_found_is(922, 13, &found, NULL, 0);
	txn_vacuum_tests_found_free(&found);
	c->primary = txn_vacuum_tests_entry(io, held, 0);
	four = simc_begin();
	STD_ASSERT_EQ_U64(925, 13, txn_vacuum_tests_inserted(io, store, held, 9428, 4, 300, four), 2);
	txn_vacuum_tests_ended(io, four, true);
}

/* txn_vacuum_tests_aborted_replica is the aborted insert case's replica: it redoes the primary's records up to the free, then the free twice. */
static void
txn_vacuum_tests_aborted_replica(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	txn_vacuum_tests_aborted_t *c = captures;
	const txn_vacuum_tests_logged_t *free = &STD_VEC_AT(&c->records, txn_vacuum_tests_logged_t, c->at);

	for (size_t k = 0; k < c->at; k++)
	{
		const txn_vacuum_tests_logged_t *record = &STD_VEC_AT(&c->records, txn_vacuum_tests_logged_t, k);

		if (!record->freed)
			txn_vacuum_tests_redo(io, store, held, &record->row, record->end, 936, 78);
	}
	txn_vacuum_tests_redone(io, store, held, &free->free, free->end, 939, 68);
	txn_vacuum_tests_entry_is(940, 13, txn_vacuum_tests_entry(io, held, 0), c->primary);
	txn_vacuum_tests_redone(io, store, held, &free->free, free->end, 941, 68);
	txn_vacuum_tests_entry_is(942, 13, txn_vacuum_tests_entry(io, held, 0), c->primary);
}

/* txn_vacuum_tests_freed_is asserts at a line and column that a logged free is the one wanted. */
static void
txn_vacuum_tests_freed_is(uint32 line, uint32 col, const TxnRecordTidsFreed *got, const TxnRecordTidsFreed *want)
{
	STD_ASSERT_EQ(line, col, txn_record_TidsFreed_PartialEq_eq(got, want), got, txn_record_TidsFreed_Debug_fmt, want,
				  txn_record_TidsFreed_Debug_fmt);
}

/*
 * Bugs row 44's first case and D4's standby case: an aborted insert on the
 * fresh page leaves its tuple id taken with no row, which VACUUM frees with
 * the 300 heap bytes its own take lowered, logging one TidsFreed with no
 * horizon, after which the next insert of its size takes it again, while the
 * committed rows' tuple ids stay. A storage that redoes the primary's records
 * up to the free holds the same free space entry, and a free redone again
 * gives nothing back twice.
 */
void
test_txn_vacuum_tests_an_aborted_inserts_tuple_id_on_the_fresh_page_is_freed_with_its_own_bytes_and_taken_again(void)
{
	static std_TupleU32U16U16U64_t runs[] = {{0, 2, 1, 300}};
	static uint64 enders[] = {0};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_vacuum_tests_aborted_t c = {0};
	TxnRecordTidsFreed want = {.storage = txn_insert_tests_stored(9428), .runs = {runs, 1, 1}, .enders = {enders, 1, 1}};

	txn_vacuum_tests_with(io, 9428, txn_vacuum_tests_aborted_primary, &c);
	c.records = txn_vacuum_tests_logged(9428);
	while (c.at < c.records.len && !STD_VEC_AT(&c.records, txn_vacuum_tests_logged_t, c.at).freed)
		c.at++;
	if (c.at == c.records.len)
		STD_OPTION_UNWRAP_FAILED(929, 88);
	txn_vacuum_tests_freed_is(932, 9, &STD_VEC_AT(&c.records, txn_vacuum_tests_logged_t, c.at).free, &want);
	txn_vacuum_tests_with(io, 9432, txn_vacuum_tests_aborted_replica, &c);
	txn_vacuum_tests_logged_free(&c.records);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_takers_free frees a Vec<Taker> and each taker's entry and value ids. */
static void
txn_vacuum_tests_takers_free(StdVec *takers)
{
	for (size_t k = 0; k < takers->len; k++)
	{
		txn_TupleWriterRowEntryU64TupleVecU32Bool_t *taker = &STD_VEC_AT(takers,
																		 txn_TupleWriterRowEntryU64TupleVecU32Bool_t, k);

		if (taker->f2.has_value)
			std_vec_free_Checked(&taker->f2.value, 1, 1);
		std_vec_free_Checked(&taker->f4.f0, sizeof(uint32), _Alignof(uint32));
	}
	std_vec_free_Checked(takers, sizeof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t),
						 _Alignof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t));
}

/* txn_vacuum_tests_takers returns the takers of a block's free space entry in a storage's delta, unwrapped at a line and column. */
static StdVec
txn_vacuum_tests_takers(HostIoIo *io, const HostSharedHeld *held, uint32 line, uint32 col)
{
	HostError	error;
	StdVec		takers;

	if (!txn_memtable_map_takers(io, host_shared_Held_record(held), TXN_MEMTABLE_MAP_MAP_FREE, 0, &takers, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(line, col, &error);
	return takers;
}

/* txn_vacuum_tests_u64s_is asserts at a line and column that some u64s are those wanted, as arrays compare. */
static void
txn_vacuum_tests_u64s_is(uint32 line, uint32 col, const uint64 *got, const uint64 *want, size_t len)
{
	StdVec		left = {(void *) got, len, len};
	StdVec		right = {(void *) want, len, len};

	STD_ASSERT_EQ(line, col, memcmp(got, want, len * sizeof(uint64)) == 0, &left, chunks_trailer_VecU64_Debug_fmt,
				  &right, chunks_trailer_VecU64_Debug_fmt);
}

/* txn_vacuum_tests_emptying is the emptying take's case: an aborted insert's take empties the fresh page's entry, and its free gives back all its bytes. */
static void
txn_vacuum_tests_emptying(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	static const TxnVacuumFreed want[] = {{2, 4084, 0}};
	static const uint16 two[] = {2};
	HostXid		one = simc_begin();
	HostXid		second;
	HostXid		three;
	StdVec		takers;
	StdVec		found;

	STD_ASSERT_EQ_U64(955, 13, txn_vacuum_tests_inserted(io, store, held, 9429, 1, 4084, one), 1);
	txn_vacuum_tests_ended(io, one, true);
	second = simc_begin();
	STD_ASSERT_EQ_U64(958, 13, txn_vacuum_tests_inserted(io, store, held, 9429, 2, 4084, second), 2);
	txn_vacuum_tests_ended(io, second, false);
	takers = txn_vacuum_tests_takers(io, held, 960, 73);
	{
		const txn_TupleWriterRowEntryU64TupleVecU32Bool_t *last =
			takers.len == 0 ? NULL : &STD_VEC_AT(&takers, txn_TupleWriterRowEntryU64TupleVecU32Bool_t, takers.len - 1);
		HostRow		row = {2};
		StdBytes	value = last == NULL ? (StdBytes) {0} : (StdBytes) {last->f2.value.ptr, last->f2.value.len};
		StdOptionRef entry = {last != NULL && last->f2.has_value, {&value, std_bytes_Debug_fmt}};
		StdOptionRef none = {false, {NULL, std_bytes_Debug_fmt}};
		StdTupleRef got = STD_TUPLE(3, STD_ITEM(last == NULL ? NULL : &last->f0.xid, std_u64_Debug_fmt),
									STD_ITEM(last == NULL ? NULL : &last->f1, host_Row_Debug_fmt),
									STD_ITEM(&entry, std_option_Debug_fmt));
		StdTupleRef wanted = STD_TUPLE(3, STD_ITEM(&second.f0, std_u64_Debug_fmt), STD_ITEM(&row, host_Row_Debug_fmt),
									   STD_ITEM(&none, std_option_Debug_fmt));
		StdOptionRef left = {last != NULL, {&got, std_tuple_Debug_fmt}};
		StdOptionRef right = {true, {&wanted, std_tuple_Debug_fmt}};

		STD_ASSERT_EQ(962, 13, last != NULL && last->f0.xid == second.f0 && last->f1.f0 == 2 &&
					  !last->f2.has_value, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	txn_vacuum_tests_takers_free(&takers);
	found = txn_vacuum_tests_found_every(io, store, held, 963, 79);
	txn_vacuum_tests_found_is(964, 13, &found, want, lengthof(want));
	txn_vacuum_tests_freed(io, store, held, 9429, &found, false, 965, 90);
	txn_vacuum_tests_found_free(&found);
	txn_vacuum_tests_entry_is(966, 13, txn_vacuum_tests_entry(io, held, 0),
							  txn_vacuum_tests_some(txn_free_space_Free_new(4084, two, 1, 291)));
	three = simc_begin();
	STD_ASSERT_EQ_U64(968, 13, txn_vacuum_tests_inserted(io, store, held, 9429, 3, 4084, three), 2);
	txn_vacuum_tests_ended(io, three, true);
}

/*
 * P1's emptying take: an aborted insert whose take leaves the fresh page no
 * bytes removes the page's entry by an Emptied holding its tuple id, and
 * VACUUM frees that tuple id with all 4084 bytes the entry held before it, so
 * the next insert of its size takes it again.
 */
void
test_txn_vacuum_tests_a_take_that_empties_its_entry_is_freed_with_all_the_bytes_the_entry_held(void)
{
	SimcTestIo	test_io = simc_io();

	txn_vacuum_tests_with(&test_io.io, 9429, txn_vacuum_tests_emptying, NULL);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_unnamed is the unnamed take's case: a spent place stays taken while its taker runs, and is freed once it ended. */
static void
txn_vacuum_tests_unnamed(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	static const TxnVacuumFreed want[] = {{1, 120, 0}};
	HostXid		spender = simc_begin();
	TxnRecordSpent spent = {.writer = {.xid = spender.f0}, .storage = txn_insert_tests_stored(9430), .tuple = {1},
	.heap_bytes = 120};
	HostXid		next;
	HostError	error;
	StdVec		found;

	if (!txn_insert_redo_spent(io, store, held, &spent, &TXN_VACUUM_TESTS_PAGE, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(984, 64, &error);
	found = txn_vacuum_tests_found_every(io, store, held, 985, 78);
	txn_vacuum_tests_found_is(985, 13, &found, NULL, 0);
	txn_vacuum_tests_found_free(&found);
	simc_commit(spender);
	found = txn_vacuum_tests_found_every(io, store, held, 987, 79);
	txn_vacuum_tests_found_is(988, 13, &found, want, lengthof(want));
	txn_vacuum_tests_freed(io, store, held, 9430, &found, false, 989, 90);
	txn_vacuum_tests_found_free(&found);
	next = simc_begin();
	STD_ASSERT_EQ_U64(991, 13, txn_vacuum_tests_inserted(io, store, held, 9430, 1, 120, next), 1);
	txn_vacuum_tests_ended(io, next, true);
}

/*
 * D2's race, bugs row 73's spent place, and bugs row 44's insert failed after
 * its take and its value id Retry after its take, each of which leaves a
 * take that no row names: it stays taken while its taker's transaction runs,
 * and VACUUM frees it with its bytes once that transaction ended, after which
 * the next insert takes it.
 */
void
test_txn_vacuum_tests_a_take_no_row_names_is_freed_only_once_its_taker_has_ended(void)
{
	SimcTestIo	test_io = simc_io();

	txn_vacuum_tests_with(&test_io.io, 9430, txn_vacuum_tests_unnamed, NULL);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_leaving_t is the aborted leave case's captures: the store, the state, and the block the last insert took. */
typedef struct txn_vacuum_tests_leaving_t
{
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	bool		has_target;
	uint32		target;
} txn_vacuum_tests_leaving_t;

/* txn_vacuum_tests_leaving_insert is the aborted leave case's insert: a key's row of some heap bytes at the last insert's block, committed or aborted. */
static uint64
txn_vacuum_tests_leaving_insert(txn_vacuum_tests_leaving_t *c, HostIoIo *io, uint64 key, uint64 bytes,
								bool committed)
{
	HostXid		writer = simc_begin();
	TxnInsertInsert row = txn_insert_tests_row(txn_insert_tests_stored(9435), key);
	TxnInsertRoom room = {.bytes = bytes, .page = TXN_VACUUM_TESTS_PAGE.bytes, .most = TXN_VACUUM_TESTS_PAGE.most};
	HostError	error;
	uint64		tid;

	row.writer.xid = writer.f0;
	row.has_target = c->has_target;
	row.target = c->target;
	if (!txn_insert_tests_taken(io, c->store, c->held, &row, &room, NULL, NULL, 0, &tid, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(1011, 103, &error);
	c->has_target = true;
	c->target = (uint32) (tid >> 16);
	txn_vacuum_tests_ended(io, writer, committed);
	return tid;
}

/* txn_vacuum_tests_leaving is the aborted leave case: the new page's tuple id an aborted insert took is freed and taken again, then the old page's high offset. */
static void
txn_vacuum_tests_leaving(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	static const TxnVacuumFreed want[] = {{1 << 16 | 1, 3200, 0}};
	static const uint64 first[] = {1, 2};
	static const uint64 again[] = {1 << 16 | 1, 1 << 16 | 2, 3};
	txn_vacuum_tests_leaving_t c = {.store = store, .held = held};
	uint64		taken[3];
	StdVec		found;

	taken[0] = txn_vacuum_tests_leaving_insert(&c, io, 1, 2500, true);
	taken[1] = txn_vacuum_tests_leaving_insert(&c, io, 2, 2500, true);
	txn_vacuum_tests_u64s_is(1016, 13, taken, first, 2);
	STD_ASSERT_EQ_U64(1017, 13, txn_vacuum_tests_leaving_insert(&c, io, 3, 3200, false), 1 << 16 | 1);
	found = txn_vacuum_tests_found_every(io, store, held, 1018, 79);
	txn_vacuum_tests_found_is(1019, 13, &found, want, lengthof(want));
	txn_vacuum_tests_freed(io, store, held, 9435, &found, false, 1020, 90);
	txn_vacuum_tests_found_free(&found);
	taken[0] = txn_vacuum_tests_leaving_insert(&c, io, 4, 3000, true);
	taken[1] = txn_vacuum_tests_leaving_insert(&c, io, 5, 3000, true);
	taken[2] = txn_vacuum_tests_leaving_insert(&c, io, 6, 3000, true);
	txn_vacuum_tests_u64s_is(1022, 13, taken, again, 3);
}

/*
 * Bugs row 44's aborted leave: an aborted insert that left the fresh page for
 * a new one keeps the leave, and VACUUM frees the new page's tuple id it
 * took, so the next row, preferring the backend's last block, takes (1,1)
 * again, and once the new page is full a row the old page can hold takes its
 * high offset (0,3).
 */
void
test_txn_vacuum_tests_an_aborted_leaves_old_offsets_and_its_new_tuple_id_are_handed_out_again(void)
{
	SimcTestIo	test_io = simc_io();

	txn_vacuum_tests_with(&test_io.io, 9435, txn_vacuum_tests_leaving, NULL);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_skipped_t is the skipped index pass case's captures: the entries the primary's frees left, and the records logged. */
typedef struct txn_vacuum_tests_skipped_t
{
	txn_vacuum_tests_entry_t primary[2];
	StdVec		records;
} txn_vacuum_tests_skipped_t;

/* txn_vacuum_tests_skipped_primary is the skipped index pass case's primary: a dead tuple id's bytes come back, then its offset. */
static void
txn_vacuum_tests_skipped_primary(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	static const TxnVacuumFreed want[] = {{2, 300, 0}};
	static const TxnVacuumFreed drained[] = {{2, 0, 0}};
	static const uint16 two[] = {2};
	txn_vacuum_tests_skipped_t *c = captures;
	HostXid		one = simc_begin();
	HostXid		second;
	HostXid		three;
	HostXid		four;
	HostXid		five;
	StdVec		found;
	StdVec		dead;

	STD_ASSERT_EQ_U64(1039, 13, txn_vacuum_tests_inserted(io, store, held, 9431, 1, 100, one), 1);
	txn_vacuum_tests_ended(io, one, true);
	second = simc_begin();
	STD_ASSERT_EQ_U64(1042, 13, txn_vacuum_tests_inserted(io, store, held, 9431, 2, 300, second), 2);
	txn_vacuum_tests_ended(io, second, false);
	three = simc_begin();
	STD_ASSERT_EQ_U64(1045, 13, txn_vacuum_tests_inserted(io, store, held, 9431, 3, 100, three), 3);
	txn_vacuum_tests_ended(io, three, true);
	found = txn_vacuum_tests_found_every(io, store, held, 1047, 79);
	txn_vacuum_tests_found_is(1048, 13, &found, want, lengthof(want));
	txn_vacuum_tests_freed(io, store, held, 9431, &found, true, 1049, 89);
	txn_vacuum_tests_found_free(&found);
	txn_vacuum_tests_entry_is(1050, 13, txn_vacuum_tests_entry(io, held, 0),
							  txn_vacuum_tests_some(txn_free_space_Free_new(8168 - 500 + 300, NULL, 0, 291)));
	c->primary[0] = txn_vacuum_tests_entry(io, held, 0);
	dead = txn_vacuum_tests_found_every(io, store, held, 1052, 78);
	txn_vacuum_tests_found_is(1053, 13, &dead, drained, lengthof(drained));
	txn_vacuum_tests_freed(io, store, held, 9431, &dead, true, 1054, 88);
	txn_vacuum_tests_found_free(&dead);
	txn_vacuum_tests_entry_is(1055, 13, txn_vacuum_tests_entry(io, held, 0), c->primary[0]);
	four = simc_begin();
	STD_ASSERT_EQ_U64(1057, 13, txn_vacuum_tests_inserted(io, store, held, 9431, 4, 300, four), 4);
	txn_vacuum_tests_ended(io, four, true);
	found = txn_vacuum_tests_found_every(io, store, held, 1059, 79);
	txn_vacuum_tests_found_is(1060, 13, &found, drained, lengthof(drained));
	txn_vacuum_tests_freed(io, store, held, 9431, &found, false, 1061, 90);
	txn_vacuum_tests_found_free(&found);
	txn_vacuum_tests_entry_is(1062, 13, txn_vacuum_tests_entry(io, held, 0),
							  txn_vacuum_tests_some(txn_free_space_Free_new(8168 - 800 + 300, two, 1, 291)));
	c->primary[1] = txn_vacuum_tests_entry(io, held, 0);
	five = simc_begin();
	STD_ASSERT_EQ_U64(1065, 13, txn_vacuum_tests_inserted(io, store, held, 9431, 5, 100, five), 2);
	txn_vacuum_tests_ended(io, five, true);
}

/* txn_vacuum_tests_entries_Debug_fmt is a Vec<Option<Free>>'s Debug. */
static bool
txn_vacuum_tests_entries_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *entries = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < entries->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(entries, txn_vacuum_tests_entry_t, k), txn_vacuum_tests_entry_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_vacuum_tests_skipped_replica is the skipped index pass case's replica: it redoes the primary's records before key 5's row, each free twice. */
static void
txn_vacuum_tests_skipped_replica(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	static const uint8 five[8] = {0, 0, 0, 0, 0, 0, 0, 5};
	txn_vacuum_tests_skipped_t *c = captures;
	StdVec		primary = {c->primary, lengthof(c->primary), lengthof(c->primary)};
	StdVec		redone;
	bool		eq;

	std_vec_new(&redone, sizeof(txn_vacuum_tests_entry_t), _Alignof(txn_vacuum_tests_entry_t));
	for (size_t k = 0; k < c->records.len; k++)
	{
		const txn_vacuum_tests_logged_t *record = &STD_VEC_AT(&c->records, txn_vacuum_tests_logged_t, k);
		txn_vacuum_tests_entry_t once;

		if (!record->freed && record->row.key.len == sizeof(five) &&
			memcmp(record->row.key.ptr, five, sizeof(five)) == 0)
			break;
		if (!record->freed)
		{
			txn_vacuum_tests_redo(io, store, held, &record->row, record->end, 1085, 106);
			continue;
		}
		txn_vacuum_tests_redone(io, store, held, &record->free, record->end, 1087, 81);
		once = txn_vacuum_tests_entry(io, held, 0);
		txn_vacuum_tests_redone(io, store, held, &record->free, record->end, 1089, 81);
		txn_vacuum_tests_entry_is(1090, 25, txn_vacuum_tests_entry(io, held, 0), once);
		std_vec_push_Checked(&redone, &once, sizeof(once), _Alignof(txn_vacuum_tests_entry_t));
	}
	eq = redone.len == primary.len;
	for (size_t k = 0; eq && k < redone.len; k++)
		eq = txn_vacuum_tests_entry_eq(&STD_VEC_AT(&redone, txn_vacuum_tests_entry_t, k), &c->primary[k]);
	STD_ASSERT_EQ(1095, 13, eq, &redone, txn_vacuum_tests_entries_Debug_fmt, &primary,
				  txn_vacuum_tests_entries_Debug_fmt);
	std_vec_free_Checked(&redone, sizeof(txn_vacuum_tests_entry_t), _Alignof(txn_vacuum_tests_entry_t));
}

/* txn_vacuum_tests_frees_Debug_fmt is a Vec<&TidsFreed>'s Debug. */
static bool
txn_vacuum_tests_frees_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *frees = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < frees->len; k++)
		std_DebugList_entry(&list, STD_VEC_AT(frees, const TxnRecordTidsFreed *, k), txn_record_TidsFreed_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_vacuum_tests_frees_is asserts at a line and column that the frees logged are those wanted, in their order. */
static void
txn_vacuum_tests_frees_is(uint32 line, uint32 col, const StdVec *records, const TxnRecordTidsFreed *const *want,
						  size_t want_len)
{
	StdVec		got;
	StdVec		wanted = {(void *) want, want_len, want_len};
	bool		eq;

	std_vec_new(&got, sizeof(const TxnRecordTidsFreed *), _Alignof(const TxnRecordTidsFreed *));
	for (size_t k = 0; k < records->len; k++)
	{
		const txn_vacuum_tests_logged_t *record = &STD_VEC_AT(records, txn_vacuum_tests_logged_t, k);
		const TxnRecordTidsFreed *free = &record->free;

		if (record->freed)
			std_vec_push_Checked(&got, &free, sizeof(free), _Alignof(const TxnRecordTidsFreed *));
	}
	eq = got.len == want_len;
	for (size_t k = 0; eq && k < want_len; k++)
		eq = txn_record_TidsFreed_PartialEq_eq(STD_VEC_AT(&got, const TxnRecordTidsFreed *, k), want[k]);
	STD_ASSERT_EQ(line, col, eq, &got, txn_vacuum_tests_frees_Debug_fmt, &wanted, txn_vacuum_tests_frees_Debug_fmt);
	std_vec_free_Checked(&got, sizeof(const TxnRecordTidsFreed *), _Alignof(const TxnRecordTidsFreed *));
}

/*
 * D5: a VACUUM that skips its index pass gives back an aborted insert's 300
 * bytes and keeps its offset taken, as heap's prune leaves an LP_DEAD line
 * pointer with its space reclaimed, logging them as field 6's runs, so the
 * next row takes the counter's (0,4) in that space; a later VACUUM finds the
 * tuple id with no bytes left and frees its offset alone, and the next row
 * takes (0,2). A storage that redoes the primary's records holds the same
 * entry after each free, and each free redone again gives nothing back twice.
 */
void
test_txn_vacuum_tests_a_skipped_index_pass_gives_back_a_dead_tuple_ids_bytes_and_a_later_free_its_offset_alone(void)
{
	static std_TupleU32U16U16U64_t dead_runs[] = {{0, 2, 1, 300}};
	static std_TupleU32U16U16U64_t after_runs[] = {{0, 2, 1, 0}};
	static uint64 enders[] = {0};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_vacuum_tests_skipped_t c = {0};
	TxnRecordTidsFreed dead = {.storage = txn_insert_tests_stored(9431), .dead = {dead_runs, 1, 1}, .enders = {enders, 1, 1}};
	TxnRecordTidsFreed after = {.storage = dead.storage, .runs = {after_runs, 1, 1}, .enders = {enders, 1, 1}};
	const TxnRecordTidsFreed *const want[] = {&dead, &after};

	txn_vacuum_tests_with(io, 9431, txn_vacuum_tests_skipped_primary, &c);
	c.records = txn_vacuum_tests_logged(9431);
	txn_vacuum_tests_frees_is(1079, 9, &c.records, want, lengthof(want));
	txn_vacuum_tests_with(io, 9433, txn_vacuum_tests_skipped_replica, &c);
	txn_vacuum_tests_logged_free(&c.records);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_full_block is the full block case: a dead tuple id on a block whose entry has no free offset keeps its bytes until its free. */
static void
txn_vacuum_tests_full_block(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	static const TxnVacuumFreed want[] = {{5, 28, 0}};
	static const uint16 five[] = {5};
	txn_vacuum_tests_entry_t none = {0};
	StdVec		found;

	for (uint64 key = 1; key <= 292; key++)
	{
		HostXid		writer = simc_begin();
		uint64		tid = txn_vacuum_tests_inserted(io, store, held, 9434, key, 28, writer);

		STD_ASSERT_EQ_U64(1111, 17, tid, key <= 291 ? key : 1 << 16 | 1);
		txn_vacuum_tests_ended(io, writer, key != 5);
	}
	txn_vacuum_tests_entry_is(1114, 13, txn_vacuum_tests_entry(io, held, 0), none);
	found = txn_vacuum_tests_found_every(io, store, held, 1115, 79);
	txn_vacuum_tests_found_is(1116, 13, &found, want, lengthof(want));
	txn_vacuum_tests_freed(io, store, held, 9434, &found, true, 1117, 89);
	txn_vacuum_tests_found_free(&found);
	txn_vacuum_tests_entry_is(1118, 13, txn_vacuum_tests_entry(io, held, 0), none);
	found = txn_vacuum_tests_found_every(io, store, held, 1119, 79);
	txn_vacuum_tests_found_is(1120, 13, &found, want, lengthof(want));
	txn_vacuum_tests_freed(io, store, held, 9434, &found, false, 1121, 90);
	txn_vacuum_tests_found_free(&found);
	txn_vacuum_tests_entry_is(1122, 13, txn_vacuum_tests_entry(io, held, 0),
							  txn_vacuum_tests_some(txn_free_space_Free_new(28, five, 1, 291)));
}

/*
 * D5's kept bytes: a dead tuple id on a block other than the fresh page whose
 * entry has no free offset left takes no marker, so it keeps its 28 bytes,
 * and the later free gives back its offset with them; heap's
 * PageGetHeapFreeSpace likewise gives such a page no room while its pointers
 * are all used, dead ones included.
 */
void
test_txn_vacuum_tests_a_dead_tuple_id_on_a_full_block_keeps_its_bytes_until_its_free(void)
{
	SimcTestIo	test_io = simc_io();

	txn_vacuum_tests_with(&test_io.io, 9434, txn_vacuum_tests_full_block, NULL);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_updated_decide is updated's read of the replaced key's versions: it goes on as the transaction's command, over the newest. */
static bool
txn_vacuum_tests_updated_decide(void *captures, HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len,
								TxnChangeRead *result, void *stopped, HostError *error)
{
	result->tag = TXN_CHANGE_READ_GO;
	result->u.go.shown.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND;
	result->u.go.cmin = 0;
	result->u.go.has_row = versions_len > 0;
	if (versions_len > 0)
		result->u.go.row = versions[0].handle;
	return true;
}

/*
 * txn_vacuum_tests_updated updates a key's row of a database's storage to
 * some heap bytes as a transaction's, taking a block by heap's order after a
 * take-time prune when prune names the snapshots, and returns the new row's
 * tuple id.
 */
static uint64
txn_vacuum_tests_updated(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 database,
						 uint64 key, uint64 tuple, uint64 bytes, HostXid xid, const TxnVacuumPrune *prune)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("a case's update gave "), STD_FMT_ARG(0)};
	HostIoWaits waits = host_io_Waits_testing();
	TxnInsertInsert row = txn_insert_tests_row(txn_insert_tests_stored(database), key);
	uint32		stripe = txn_change_stripe(&row.storage, row.key.ptr, row.key.len, true);
	TxnInsertReplaces replaces = {.tuple = {tuple}, .lock = TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE, .stripe = stripe};
	TxnInsertKeyed keyed = {.stripe = stripe};
	TxnInsertRoom room = {.bytes = bytes, .page = TXN_VACUUM_TESTS_PAGE.bytes, .most = TXN_VACUUM_TESTS_PAGE.most};
	TxnInsertInserted inserted;
	TxnInsertInsertedRef other = {&inserted, {NULL, std_unit_Debug_fmt}};
	StdFmtArg	args[] = {{&other, txn_insert_Inserted_Debug_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	HostError	error;

	row.writer.xid = xid.f0;
	std_vec_clone(&replaces.key, &row.key, 1, 1);
	if (!txn_insert_update(io, store, held, &row, &replaces, &room, prune, NULL, NULL, 0, keyed, &waits,
						   txn_vacuum_tests_updated_decide, NULL, NULL, NULL, &inserted, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(1152, 111, &error);
	if (inserted.tag != TXN_INSERT_INSERTED_TOOK)
		std_panic_fmt(RUST_FILE, 1154, 22, &arguments);
	return inserted.u.took.tuple;
}

/* txn_vacuum_tests_committed_status takes a transaction's commit status entry, its commit record ending at an LSN, unwrapped at a line and column. */
static void
txn_vacuum_tests_committed_status(HostIoIo *io, HostXid xid, HostLsn end, uint32 line, uint32 col)
{
	TxnStatusOutcome outcome = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = end};
	TxnStatusRoomed roomed;
	HostError	error;
	bool		took;

	if (!txn_status_take(io, xid, outcome, &roomed, &took, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(line, col, &error);
}

/*
 * txn_vacuum_tests_filled fills a database's storage with fourteen committed
 * rows of 1036 heap bytes, seven a page, (0,1) to (1,7), and commits an
 * update of key 1 whose new row finds no room on block 0 and goes to (2,1),
 * setting block 0's full flag, and returns the updater's xid. When committed
 * is set, each commit first takes its status entry there, which a fold reads.
 */
static HostXid
txn_vacuum_tests_filled(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 database,
						HostLsn committed)
{
	HostXid		writer = simc_begin();
	HostXid		updater;
	txn_vacuum_tests_entry_t entry;

	for (uint64 key = 1; key <= 14; key++)
	{
		uint64		tid = txn_vacuum_tests_inserted(io, store, held, database, key, 1036, writer);

		STD_ASSERT_EQ_U64(1165, 13, tid, ((key - 1) / 7) << 16 | ((key - 1) % 7 + 1));
	}
	if (committed != 0)
		txn_vacuum_tests_committed_status(io, writer, committed, __LINE__, 9);
	txn_vacuum_tests_ended(io, writer, true);
	updater = simc_begin();
	STD_ASSERT_EQ_U64(1169, 9, txn_vacuum_tests_updated(io, store, held, database, 1, 1, 1036, updater, NULL),
					  2 << 16 | 1);
	if (committed != 0)
		txn_vacuum_tests_committed_status(io, updater, committed, __LINE__, 9);
	txn_vacuum_tests_ended(io, updater, true);
	entry = txn_vacuum_tests_entry(io, held, 0);
	if (!entry.has)
		STD_OPTION_UNWRAP_FAILED(1171, 36);
	STD_ASSERT(1171, 9, entry.free.full, "entry(io, held, 0).unwrap().full");
	return updater;
}

/* txn_vacuum_tests_versions_len returns how many versions of a key's row the delta holds, under a hold taken and given back at the lines the case unwraps them. */
static size_t
txn_vacuum_tests_versions_len(HostIoIo *io, const HostSharedHeld *held, uint64 key, uint32 hold_line,
							  uint32 hold_col, uint32 line, uint32 col)
{
	HostDeltaHold hold;
	HostError	error;
	StdVec		chain;
	uint8		bytes[8];
	size_t		len;

	if (!host_delta_hold(io, &hold, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(hold_line, hold_col, &error);
	std_u64_to_be_bytes(key, bytes);
	if (!txn_memtable_table_versions(io, &hold, host_shared_Held_record(held), bytes, sizeof(bytes), &chain, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(line, col, &error);
	len = chain.len;
	host_delta_Hold_drop(&hold);
	txn_memtable_version_versions_drop(&chain, 0);
	return len;
}

/* txn_vacuum_tests_pruning_t is the take-time prune case's captures: block 0's entry after the full update. */
typedef struct txn_vacuum_tests_pruning_t
{
	txn_vacuum_tests_entry_t moved;
} txn_vacuum_tests_pruning_t;

/* txn_vacuum_tests_pruning_primary is the take-time prune case's primary: the next update of a row on the full block prunes it first and stays on it. */
static void
txn_vacuum_tests_pruning_primary(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	static std_TupleU32U16U16U64_t dead[] = {{0, 1, 1, 1036}};
	txn_vacuum_tests_pruning_t *c = captures;
	HostXid		updater = txn_vacuum_tests_filled(io, store, held, 9436, 0);
	HostXid		next;
	SimcTaken	horizon;
	SimcTaken	any;
	HostSnapshot horizon_lent;
	HostSnapshot any_lent;
	TxnVacuumPrune prune;
	txn_vacuum_tests_entry_t block;
	StdVec		records;
	TxnRecordTidsFreed want;
	const TxnRecordTidsFreed *const wanted[] = {&want};

	c->moved = txn_vacuum_tests_entry(io, held, 0);
	next = simc_begin();
	horizon = simc_non_vacuumable(next);
	any = simc_snapshot(HOST_SNAPSHOT_KIND_ANY, false);
	horizon_lent = simc_Taken_lent(&horizon);
	any_lent = simc_Taken_lent(&any);
	prune.horizon = &horizon_lent;
	prune.any = &any_lent;
	STD_ASSERT_EQ_U64(1191, 13, txn_vacuum_tests_updated(io, store, held, 9436, 3, 3, 1036, next, &prune), 8);
	txn_vacuum_tests_ended(io, next, true);
	block = txn_vacuum_tests_entry(io, held, 0);
	if (!block.has)
		STD_OPTION_UNWRAP_FAILED(1193, 44);
	STD_ASSERT(1194, 13, !block.free.full, "!block.full");
	STD_ASSERT(1195, 13, !txn_free_space_Free_has(&block.free, 1), "!block.has(1)");
	STD_ASSERT_EQ_U64(1198, 13, txn_vacuum_tests_versions_len(io, held, 1, 1196, 46, 1197, 88), 1);
	records = txn_vacuum_tests_logged(9436);
	want = (TxnRecordTidsFreed)
	{
		.storage = txn_insert_tests_stored(9436), .horizon = updater.f0, .dead =
		{
			dead, 1, 1
		},
			.enders =
		{
			&updater.f0, 1, 1
		}
	};
	txn_vacuum_tests_frees_is(1208, 13, &records, wanted, lengthof(wanted));
	txn_vacuum_tests_logged_free(&records);
	simc_Taken_drop(&any);
	simc_Taken_drop(&horizon);
}

/* txn_vacuum_tests_pruning_replica is the take-time prune case's replica: redoing the primary's rows up to the full update sets the full flag too. */
static void
txn_vacuum_tests_pruning_replica(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	txn_vacuum_tests_pruning_t *c = captures;
	StdVec		records = txn_vacuum_tests_logged(9436);

	for (size_t k = 0; k < records.len; k++)
	{
		txn_vacuum_tests_logged_t *record = &STD_VEC_AT(&records, txn_vacuum_tests_logged_t, k);
		bool		update;

		if (record->freed)
			continue;
		update = record->row.has_replaced;
		record->row.storage = txn_insert_tests_stored(9437);
		txn_vacuum_tests_redo(io, store, held, &record->row, record->end, 1218, 74);
		if (update)
			break;
	}
	txn_vacuum_tests_logged_free(&records);
	txn_vacuum_tests_entry_is(1223, 13, txn_vacuum_tests_entry(io, held, 0), c->moved);
}

/*
 * Bugs row 75 (R1, R2): an update whose new row left block 0 sets the
 * block's full flag, which a replica that redoes the primary's records sets
 * too; the next update of a row on block 0, past every snapshot's horizon,
 * prunes the block first, as heap_page_prune_opt does on a full page: key 1's
 * old version leaves its chain, its 1036 bytes come back with its offset
 * kept, logged with the updater's xid as the conflict horizon, the flag
 * clears, and the new row takes (0,8), as heap's does.
 */
void
test_txn_vacuum_tests_a_take_prunes_a_full_blocks_dead_versions_and_its_update_stays_on_the_block(void)
{
	SimcTestIo	test_io = simc_io();
	txn_vacuum_tests_pruning_t c = {0};

	txn_vacuum_tests_with(&test_io.io, 9436, txn_vacuum_tests_pruning_primary, &c);
	txn_vacuum_tests_with(&test_io.io, 9437, txn_vacuum_tests_pruning_replica, &c);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_held_back is the held-back case: a snapshot that may see key 1's old version keeps it, and the next update leaves block 0. */
static void
txn_vacuum_tests_held_back(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	HostXid		updater = txn_vacuum_tests_filled(io, store, held, 9438, 0);
	SimcTaken	horizon = simc_non_vacuumable(updater);
	SimcTaken	any = simc_snapshot(HOST_SNAPSHOT_KIND_ANY, false);
	HostSnapshot horizon_lent = simc_Taken_lent(&horizon);
	HostSnapshot any_lent = simc_Taken_lent(&any);
	TxnVacuumPrune prune = {&horizon_lent, &any_lent};
	HostXid		next = simc_begin();

	STD_ASSERT_EQ_U64(1239, 13, txn_vacuum_tests_updated(io, store, held, 9438, 3, 3, 1036, next, &prune),
					  2 << 16 | 2);
	txn_vacuum_tests_ended(io, next, true);
	STD_ASSERT_EQ_U64(1242, 13, txn_vacuum_tests_versions_len(io, held, 1, 1241, 46, 1242, 87), 2);
	simc_Taken_drop(&any);
	simc_Taken_drop(&horizon);
}

/*
 * Bugs row 75's held-back case: a snapshot that may still see key 1's old
 * version keeps it, as heap's prune keeps a tuple its horizon doesn't pass,
 * so the next update of a row on block 0 finds no room there and its new row
 * goes to (2,2).
 */
void
test_txn_vacuum_tests_a_take_leaves_a_version_an_open_snapshot_may_see(void)
{
	SimcTestIo	test_io = simc_io();

	txn_vacuum_tests_with(&test_io.io, 9438, txn_vacuum_tests_held_back, NULL);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_claimed_is asserts at a line and column that the dead tuple ids a free claimed are those wanted, and frees them. */
static void
txn_vacuum_tests_claimed_is(uint32 line, uint32 col, StdVec *claimed, const uint64 *want, size_t want_len)
{
	StdVec		wanted = {(void *) want, want_len, want_len};
	bool		eq = claimed->len == want_len && memcmp(claimed->ptr, want, want_len * sizeof(uint64)) == 0;

	STD_ASSERT_EQ(line, col, eq, claimed, chunks_trailer_VecU64_Debug_fmt, &wanted, chunks_trailer_VecU64_Debug_fmt);
	std_vec_free_Checked(claimed, sizeof(uint64), _Alignof(uint64));
}

/* txn_vacuum_tests_claimed_once is the claimed once case: a second give-back of a dead tuple id claims nothing. */
static void
txn_vacuum_tests_claimed_once(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	static const TxnVacuumFreed want[] = {{2, 300, 0}};
	static const uint64 two[] = {2};
	HostXid		one = simc_begin();
	HostXid		second;
	StdVec		found;
	StdVec		claimed;

	STD_ASSERT_EQ_U64(1254, 13, txn_vacuum_tests_inserted(io, store, held, 9439, 1, 100, one), 1);
	txn_vacuum_tests_ended(io, one, true);
	second = simc_begin();
	STD_ASSERT_EQ_U64(1257, 13, txn_vacuum_tests_inserted(io, store, held, 9439, 2, 300, second), 2);
	txn_vacuum_tests_ended(io, second, false);
	found = txn_vacuum_tests_found_every(io, store, held, 1259, 79);
	txn_vacuum_tests_found_is(1260, 13, &found, want, lengthof(want));
	claimed = txn_vacuum_tests_claimed(io, store, held, 9439, found.ptr, found.len, true, 1261, 100);
	txn_vacuum_tests_claimed_is(1261, 13, &claimed, two, lengthof(two));
	claimed = txn_vacuum_tests_claimed(io, store, held, 9439, found.ptr, found.len, true, 1262, 100);
	txn_vacuum_tests_claimed_is(1262, 13, &claimed, NULL, 0);
	txn_vacuum_tests_found_free(&found);
	txn_vacuum_tests_entry_is(1263, 13, txn_vacuum_tests_entry(io, held, 0),
							  txn_vacuum_tests_some(txn_free_space_Free_new(8168 - 400 + 300, NULL, 0, 291)));
}

/*
 * PRevPF 7's R1: a dead tuple id's marker is its one claim, so a second
 * give-back of it, as a concurrent prune's, claims nothing and gives its 300
 * bytes back once.
 */
void
test_txn_vacuum_tests_a_dead_tuple_id_is_claimed_once(void)
{
	SimcTestIo	test_io = simc_io();

	txn_vacuum_tests_with(&test_io.io, 9439, txn_vacuum_tests_claimed_once, NULL);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_refused_holds reports whether key 5's chain holds a row version at tuple id 5. */
static bool
txn_vacuum_tests_refused_holds(HostIoIo *io, const HostSharedHeld *held)
{
	static const uint8 five[8] = {0, 0, 0, 0, 0, 0, 0, 5};
	HostDeltaHold hold;
	HostError	error;
	StdVec		chain;
	bool		holds = false;

	if (!host_delta_hold(io, &hold, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(1274, 46, &error);
	if (!txn_memtable_table_versions(io, &hold, host_shared_Held_record(held), five, sizeof(five), &chain, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(1275, 88, &error);
	for (size_t k = 0; !holds && k < chain.len; k++)
	{
		TxnVisibleRowVersion row;

		holds = txn_visible_RowVersion_of(&STD_VEC_AT(&chain, TxnMemtableVersionVersion, k), &row) &&
			row.put.tuple.f0 == 5;
	}
	txn_memtable_version_versions_drop(&chain, 0);
	host_delta_Hold_drop(&hold);
	return holds;
}

/* txn_vacuum_tests_refused_primary is the refused marker case's primary: a dead tuple id on a full block keeps its row version. */
static void
txn_vacuum_tests_refused_primary(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	static const TxnVacuumFreed dead[] = {{5, 28, 0}};
	HostXid		writer = simc_begin();
	HostXid		updater;
	txn_vacuum_tests_entry_t none = {0};
	StdVec		claimed;

	for (uint64 key = 1; key <= 292; key++)
		txn_vacuum_tests_inserted(io, store, held, 9440, key, 28, writer);
	txn_vacuum_tests_ended(io, writer, true);
	updater = simc_begin();
	STD_ASSERT_EQ_U64(1285, 13, txn_vacuum_tests_updated(io, store, held, 9440, 5, 5, 28, updater, NULL),
					  1 << 16 | 2);
	txn_vacuum_tests_ended(io, updater, true);
	txn_vacuum_tests_entry_is(1287, 13, txn_vacuum_tests_entry(io, held, 0), none);
	claimed = txn_vacuum_tests_claimed(io, store, held, 9440, dead, lengthof(dead), true, 1289, 112);
	txn_vacuum_tests_claimed_is(1288, 13, &claimed, NULL, 0);
	STD_ASSERT(1292, 13, txn_vacuum_tests_refused_holds(io, held), "holds(io, held)");
}

/* txn_vacuum_tests_redo_all redoes every row change and free a database logged on another database's storage. */
static void
txn_vacuum_tests_redo_all(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 from, uint32 to,
						  uint32 row_line, uint32 row_col, uint32 free_line, uint32 free_col)
{
	StdVec		records = txn_vacuum_tests_logged(from);

	for (size_t k = 0; k < records.len; k++)
	{
		txn_vacuum_tests_logged_t *record = &STD_VEC_AT(&records, txn_vacuum_tests_logged_t, k);

		if (record->freed)
		{
			record->free.storage = txn_insert_tests_stored(to);
			txn_vacuum_tests_redone(io, store, held, &record->free, record->end, free_line, free_col);
		}
		else
		{
			record->row.storage = txn_insert_tests_stored(to);
			txn_vacuum_tests_redo(io, store, held, &record->row, record->end, row_line, row_col);
		}
	}
	txn_vacuum_tests_logged_free(&records);
}

/* txn_vacuum_tests_refused_replica is the refused marker case's replica: redoing the primary's records keeps the row version too. */
static void
txn_vacuum_tests_refused_replica(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	txn_vacuum_tests_redo_all(io, store, held, 9440, 9441, 1299, 82, 1303, 81);
	STD_ASSERT(1307, 13, txn_vacuum_tests_refused_holds(io, held), "holds(io, held)");
}

/*
 * PRevPF 7's R2b: a dead tuple id whose marker refuses, here on a full block
 * other than the fresh page, keeps its row version on the primary, and a
 * storage that redoes the primary's records keeps it too.
 */
void
test_txn_vacuum_tests_a_refused_markers_redo_keeps_the_version_as_the_primary_does(void)
{
	SimcTestIo	test_io = simc_io();

	txn_vacuum_tests_with(&test_io.io, 9440, txn_vacuum_tests_refused_primary, NULL);
	txn_vacuum_tests_with(&test_io.io, 9441, txn_vacuum_tests_refused_replica, NULL);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_hint returns a storage's hint for a category, unwrapped at a line and column. */
static uint32
txn_vacuum_tests_hint(const HostSharedHeld *held, uint8 category, uint32 line, uint32 col)
{
	HostError	error;
	uint32		hint;

	if (!txn_state_hint(host_shared_Held_record(held), category, &hint, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(line, col, &error);
	return hint;
}

/* txn_vacuum_tests_passed inserts five rows of 2700 heap bytes, the second aborted, so a search passes block 0 and moves its category's hint to 1. */
static void
txn_vacuum_tests_passed(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 database,
						uint8 category)
{
	static const struct
	{
		uint64		key;
		bool		committed;
	}			rows[] = {{1, true}, {2, false}, {3, true}, {4, true}, {5, true}};

	for (size_t k = 0; k < lengthof(rows); k++)
	{
		HostXid		writer = simc_begin();

		txn_vacuum_tests_inserted(io, store, held, database, rows[k].key, 2700, writer);
		txn_vacuum_tests_ended(io, writer, rows[k].committed);
	}
	STD_ASSERT_EQ_U64(1327, 13, txn_vacuum_tests_hint(held, category, 1327, 61), 1);
}

/* txn_vacuum_tests_raised is the raise case's free of a never-held tuple id, which lowers the hint back to block 0. */
static void
txn_vacuum_tests_raised(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	static const TxnVacuumFreed want[] = {{2, 2700, 0}};
	uint8		category = *(const uint8 *) captures;
	HostXid		writer;
	StdVec		found;

	txn_vacuum_tests_passed(io, store, held, 9448, category);
	found = txn_vacuum_tests_found_every(io, store, held, 1331, 79);
	txn_vacuum_tests_found_is(1332, 13, &found, want, lengthof(want));
	txn_vacuum_tests_freed(io, store, held, 9448, &found, false, 1333, 90);
	txn_vacuum_tests_found_free(&found);
	STD_ASSERT_EQ_U64(1334, 13, txn_vacuum_tests_hint(held, category, 1334, 61), 0);
	writer = simc_begin();
	STD_ASSERT_EQ_U64(1336, 13, txn_vacuum_tests_inserted(io, store, held, 9448, 6, 2700, writer), 2);
	txn_vacuum_tests_ended(io, writer, true);
}

/* txn_vacuum_tests_raised_dead is the raise case's give-back of a dead tuple id's bytes, which lowers the hint back to block 0. */
static void
txn_vacuum_tests_raised_dead(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	uint8		category = *(const uint8 *) captures;
	StdVec		found;

	txn_vacuum_tests_passed(io, store, held, 9449, category);
	found = txn_vacuum_tests_found_every(io, store, held, 1341, 79);
	txn_vacuum_tests_freed(io, store, held, 9449, &found, true, 1342, 89);
	txn_vacuum_tests_found_free(&found);
	STD_ASSERT_EQ_U64(1343, 13, txn_vacuum_tests_hint(held, category, 1343, 61), 0);
}

/* txn_vacuum_tests_raised_redo is the raise case's redo of a free on another storage, which lowers the hint back to block 0. */
static void
txn_vacuum_tests_raised_redo(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	uint8		category = *(const uint8 *) captures;
	StdVec		records = txn_vacuum_tests_logged(9448);

	for (size_t k = 0; k < records.len; k++)
	{
		txn_vacuum_tests_logged_t *record = &STD_VEC_AT(&records, txn_vacuum_tests_logged_t, k);
		HostError	error;
		bool		advanced;

		if (!record->freed)
		{
			record->row.storage = txn_insert_tests_stored(9450);
			txn_vacuum_tests_redo(io, store, held, &record->row, record->end, 1350, 82);
			continue;
		}
		record->free.storage = txn_insert_tests_stored(9450);
		if (!txn_state_advanced(host_shared_Held_record(held), category, 0, 1, &advanced, &error))
			TXN_VACUUM_TESTS_UNWRAP_FAILED(1354, 82, &error);
		STD_ASSERT(1354, 25, advanced, "state::advanced(held.record(), category, (0, 1)).unwrap()");
		txn_vacuum_tests_redone(io, store, held, &record->free, record->end, 1355, 81);
		STD_ASSERT_EQ_U64(1356, 25, txn_vacuum_tests_hint(held, category, 1356, 73), 0);
		break;
	}
	txn_vacuum_tests_logged_free(&records);
}

/*
 * Bugs row 83's raise paths: a search for room passes a full block that
 * isn't the fresh page and moves its category's hint past it; every free
 * that raises the block's entry then lowers the hint back to it, a
 * never-held tuple id's free, a dead tuple id's give-back of its bytes, and
 * the redo of a free on another storage, so the next search finds the block.
 */
void
test_txn_vacuum_tests_every_raise_of_a_blocks_room_lowers_its_categorys_hint(void)
{
	SimcTestIo	test_io = simc_io();
	HostError	error;
	uint8		category;

	if (!txn_free_space_needed(2700, &category, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(1320, 49, &error);
	txn_vacuum_tests_with(&test_io.io, 9448, txn_vacuum_tests_raised, &category);
	txn_vacuum_tests_with(&test_io.io, 9449, txn_vacuum_tests_raised_dead, &category);
	txn_vacuum_tests_with(&test_io.io, 9450, txn_vacuum_tests_raised_redo, &category);
	simc_TestIo_drop(&test_io);
}

/*
 * txn_vacuum_tests_committed inserts a key's row of 100 heap bytes into a
 * database's storage as a transaction whose commit record ends at 100, and
 * returns its tuple id.
 */
static uint64
txn_vacuum_tests_committed(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 database,
						   uint64 key)
{
	HostXid		xid = simc_begin();
	uint64		tid = txn_vacuum_tests_inserted(io, store, held, database, key, 100, xid);

	txn_vacuum_tests_committed_status(io, xid, 100, 1369, 78);
	txn_vacuum_tests_ended(io, xid, true);
	return tid;
}

/* txn_vacuum_tests_made_t is a case's keyless table's storage: its repository, a reader of the shared state, and its state. */
typedef struct txn_vacuum_tests_made_t
{
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
} txn_vacuum_tests_made_t;

/*
 * txn_vacuum_tests_made makes a keyless table's storage in a new database,
 * its repository, reader, and state unwrapped at the lines and columns the
 * case gives.
 */
static void
txn_vacuum_tests_made(HostIoIo *io, uint32 database, uint32 create_line, uint32 create_col, uint32 read_line,
					  uint32 read_col, uint32 state_line, uint32 state_col, txn_vacuum_tests_made_t *made)
{
	HostStorage storage = txn_insert_tests_stored(database);
	TxnStateMade table = {.key = {.tag = TXN_BASE_KEY_NONE}, .table = 16384};
	ChunksErrorError failed;
	HostError	error;

	if (!repo_Repo_create(io, database, HOST_PG_DEFAULT, &made->repo, &failed))
		STD_RESULT_EXPECT_FAILED(create_line, create_col, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	if (!host_shared_read(io, &made->reading, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(read_line, read_col, &error);
	if (!txn_state_state(io, &made->repo, &made->reading, &storage, TXN_STATE_PERSISTENCE_PERMANENT, true, &table,
						 &made->held, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(state_line, state_col, &error);
}

/* txn_vacuum_tests_made_drop drops a case's storage's state, reader, and repository. */
static void
txn_vacuum_tests_made_drop(txn_vacuum_tests_made_t *made)
{
	host_shared_Held_drop(&made->held);
	host_shared_Reader_drop(&made->reading);
	repo_Repo_drop(&made->repo);
}

/* txn_vacuum_tests_folded reports whether a fold of a keyless storage up to 200 installed a base, unwrapped at a line and column. */
static bool
txn_vacuum_tests_folded(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
						const HostSharedHeld *held, uint32 line, uint32 col)
{
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	HostError	error;
	bool		folded;

	if (!txn_fold_folded(io, repo, reading, held, &order, false, 200, &TXN_VACUUM_TESTS_PAGE, NULL, NULL, &folded, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(line, col, &error);
	return folded;
}

/*
 * The fold's drop beside VACUUM (bugs row 78): a row a fold applied and
 * dropped from the delta keeps its tuple id held, since the installed base's
 * tuple id tree pairs it, so VACUUM frees nothing.
 */
void
test_txn_vacuum_tests_a_folded_rows_tuple_id_is_not_freed(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_vacuum_tests_made_t made;
	TreesStoreReader store;
	StdVec		found;

	txn_vacuum_tests_made(io, 9446, 1381, 87, 1382, 46, 1383, 106, &made);
	store = repo_Repo_reader(&made.repo, HOST_CACHE_PLACEMENT_SERVER, &made.reading);
	STD_ASSERT_EQ_U64(1385, 9,
					  txn_vacuum_tests_committed(io, TREES_STORE_READER_NODE_STORE(&store), &made.held, 9446, 1), 1);
	STD_ASSERT(1387, 9, txn_vacuum_tests_folded(io, &made.repo, &made.reading, &made.held, 1387, 101),
			   "crate::fold::folded(io, (&repo, &reading), &held, &order, false, 200, (None, None)).unwrap()");
	found = txn_vacuum_tests_found_every(io, TREES_STORE_READER_NODE_STORE(&store), &made.held, 1388, 76);
	txn_vacuum_tests_found_is(1388, 9, &found, NULL, 0);
	txn_vacuum_tests_found_free(&found);
	txn_vacuum_tests_made_drop(&made);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_opened opens a database's repository and a reader of the shared state, as an actor of a fold race reads them. */
static bool
txn_vacuum_tests_opened(HostIoIo *io, uint32 database, RepoRepo *repo, HostSharedReader *reading, HostError *error)
{
	ChunksErrorError failed;

	if (!repo_Repo_open(io, database, HOST_PG_DEFAULT, repo, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	if (!host_shared_read(io, reading, error))
	{
		repo_Repo_drop(repo);
		return false;
	}
	return true;
}

/* txn_vacuum_tests_racing_state opens the fold race's storage's state, dropping its repository and reader on a failure. */
static bool
txn_vacuum_tests_racing_state(HostIoIo *io, uint32 database, RepoRepo *repo, HostSharedReader *reading,
							  HostSharedHeld *held, HostError *error)
{
	HostStorage storage = txn_insert_tests_stored(database);

	if (!txn_vacuum_tests_opened(io, database, repo, reading, error))
		return false;
	if (!txn_state_state(io, repo, reading, &storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, held, error))
	{
		host_shared_Reader_drop(reading);
		repo_Repo_drop(repo);
		return false;
	}
	return true;
}

/* txn_vacuum_tests_racing_vacuum is the fold race's VACUUM, of the database its captures name: its find of never-held tuple ids finds none. */
static bool
txn_vacuum_tests_racing_vacuum(void *captures, HostIoIo *io, HostError *error)
{
	static const char text[] = "VACUUM freed a folded row's tuple id";
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	SimcTaken	every;
	HostSnapshot lent;
	StdVec		found;
	bool		ok;

	if (!txn_vacuum_tests_racing_state(io, *(const uint32 *) captures, &repo, &reading, &held, error))
		return false;
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);

		every = txn_vacuum_tests_every();
		lent = simc_Taken_lent(&every);
		ok = txn_vacuum_never_held(io, TREES_STORE_READER_NODE_STORE(&store), &held, &TXN_VACUUM_TESTS_PAGE, &lent,
								   &found, error);
		simc_Taken_drop(&every);
	}
	if (ok)
	{
		if (found.len != 0)
		{
			StdString	message;

			std_string_from_Checked(&message, text, sizeof(text) - 1);
			*error = host_Error_fork((const uint8 *) "XX000", &message);
			ok = false;
		}
		txn_vacuum_tests_found_free(&found);
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/* txn_vacuum_tests_racing_folder is a fold race's folder, of the database its captures name: it folds the storage's committed rows. */
static bool
txn_vacuum_tests_racing_folder(void *captures, HostIoIo *io, HostError *error)
{
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	bool		folded;
	bool		ok;

	if (!txn_vacuum_tests_racing_state(io, *(const uint32 *) captures, &repo, &reading, &held, error))
		return false;
	ok = txn_fold_folded(io, &repo, &reading, &held, &order, false, 200, &TXN_VACUUM_TESTS_PAGE, NULL, NULL, &folded, error);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/* txn_vacuum_tests_endings_Debug_fmt is a Vec<(String, Ending)>'s Debug. */
static bool
txn_vacuum_tests_endings_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *endings = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < endings->len; k++)
	{
		const simc_TupleStringEnding_t *ending = &STD_VEC_AT(endings, simc_TupleStringEnding_t, k);
		StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&ending->f0, std_String_Debug_fmt),
									  STD_ITEM(&ending->f1, simc_Ending_Debug_fmt));

		std_DebugList_entry(&list, &tuple, std_tuple_Debug_fmt);
	}
	return std_DebugList_finish(&list);
}

/* txn_vacuum_tests_all_returned reports whether a run ended with every actor returning Ok. */
bool
txn_vacuum_tests_all_returned(const SimcReport *report)
{
	bool		returned = report->outcome.tag == SIMC_OUTCOME_ENDED;

	for (size_t k = 0; returned && k < report->outcome.u.ended.len; k++)
	{
		const SimcEnding *ending = &STD_VEC_AT(&report->outcome.u.ended, simc_TupleStringEnding_t, k).f1;

		returned = ending->tag == SIMC_ENDING_RETURNED && !ending->is_err;
	}
	return returned;
}

/*
 * txn_vacuum_tests_returned asserts that a run ended with every actor
 * returning Ok, panicking at a test's line and column for its outcome and at
 * another for its endings.
 */
void
txn_vacuum_tests_returned(const SimcReport *report, const char *file, uint32 ended_line, uint32 ended_col,
						  uint32 line, uint32 col)
{
	if (report->outcome.tag != SIMC_OUTCOME_ENDED)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report->outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(file, ended_line, ended_col, &message);
	}
	if (!txn_vacuum_tests_all_returned(report))
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report->outcome.u.ended, txn_vacuum_tests_endings_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(file, line, col, &message);
	}
}

/*
 * PRevPF 7's generation re-read: a fold that installs and drops between
 * VACUUM's read of a block's takes and its mapped checks moves the base
 * generation, so the find reads the block again under the newest base and
 * frees nothing of the folded row.
 */
void
test_txn_vacuum_tests_a_fold_between_the_finds_reads_leaves_its_rows_held(void)
{
	static const SimcStep steps[] = {{"folder", 6, UINT64_MAX}, {"vacuum", 6, UINT64_MAX}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint32		database = 9447;
	SimcActor	vacuum = {txn_vacuum_tests_racing_vacuum, &database};
	SimcActor	folder = {txn_vacuum_tests_racing_folder, &database};
	SimcRun		run;
	SimcReport	report;

	{
		txn_vacuum_tests_made_t made;
		TreesStoreReader store;

		txn_vacuum_tests_made(io, 9447, 1400, 91, 1401, 50, 1402, 110, &made);
		store = repo_Repo_reader(&made.repo, HOST_CACHE_PLACEMENT_SERVER, &made.reading);
		STD_ASSERT_EQ_U64(1404, 13,
						  txn_vacuum_tests_committed(io, TREES_STORE_READER_NODE_STORE(&store), &made.held, 9447, 1),
						  1);
		txn_vacuum_tests_made_drop(&made);
	}
	run = simc_Run_new(81);
	simc_Run_actor(&run, "vacuum", 6, vacuum);
	simc_Run_actor(&run, "folder", 6, folder);
	simc_Run_until(&run, "vacuum", 6, "doltgres-vacuum-found");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_vacuum_tests_returned(&report, RUST_FILE, 1430, 67, 1431, 9);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_prune_updated updates key 3's row of a database's full block 0 as a new transaction's, with a take-time prune, and returns its new row's tuple id. */
static uint64
txn_vacuum_tests_prune_updated(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 database)
{
	HostXid		next = simc_begin();
	SimcTaken	horizon = simc_non_vacuumable(next);
	SimcTaken	any = simc_snapshot(HOST_SNAPSHOT_KIND_ANY, false);
	HostSnapshot horizon_lent = simc_Taken_lent(&horizon);
	HostSnapshot any_lent = simc_Taken_lent(&any);
	TxnVacuumPrune prune = {&horizon_lent, &any_lent};
	uint64		tid = txn_vacuum_tests_updated(io, store, held, database, 3, 3, 1036, next, &prune);

	txn_vacuum_tests_ended(io, next, true);
	simc_Taken_drop(&any);
	simc_Taken_drop(&horizon);
	return tid;
}

/* txn_vacuum_tests_raced_t is the prune race's captures: its database, then the update's new row's tuple id. */
typedef struct txn_vacuum_tests_raced_t
{
	uint32		database;
	uint64		tid;
} txn_vacuum_tests_raced_t;

/* txn_vacuum_tests_racing_updater is the prune race's update of key 3, whose new row's tuple id it writes into its captures. */
static bool
txn_vacuum_tests_racing_updater(void *captures, HostIoIo *io, HostError *error)
{
	txn_vacuum_tests_raced_t *raced = captures;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;

	if (!txn_vacuum_tests_racing_state(io, raced->database, &repo, &reading, &held, error))
		return false;
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);

		raced->tid = txn_vacuum_tests_prune_updated(io, TREES_STORE_READER_NODE_STORE(&store), &held,
													raced->database);
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return true;
}

/* txn_vacuum_tests_committed_at ends a transaction committed, its status entry's commit record ending at an LSN. */
static void
txn_vacuum_tests_committed_at(HostIoIo *io, HostXid xid, HostLsn end)
{
	txn_vacuum_tests_committed_status(io, xid, end, __LINE__, 5);
	txn_vacuum_tests_ended(io, xid, true);
}

/* txn_vacuum_tests_folded_at folds a case's keyless storage up to a point, asserting that it installed a base. */
static void
txn_vacuum_tests_folded_at(HostIoIo *io, const txn_vacuum_tests_made_t *made, HostLsn point)
{
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	HostError	error;
	bool		installed;

	if (!txn_fold_folded(io, &made->repo, &made->reading, &made->held, &order, false, point, &TXN_VACUUM_TESTS_PAGE,
						 NULL, NULL, &installed, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	STD_ASSERT(__LINE__, 5, installed, "installed");
}

/*
 * txn_vacuum_tests_filled_at makes a new storage of a database whose base at
 * 150 holds block 0 full: fourteen rows of 1036 bytes committed at 100 and
 * folded at 120, then key 1's update to (2,1), committed at 130, which flags
 * the block. Key 2's update to 100 bytes, committed at 175, then takes (0,8)
 * in the delta; when folded, a fold at 200 takes that take out of the delta.
 */
static void
txn_vacuum_tests_filled_at(HostIoIo *io, uint32 database, bool folded)
{
	txn_vacuum_tests_made_t made;
	TreesStoreNodeStore store;
	TreesStoreReader reader;
	HostXid		writer;
	HostXid		updater;
	HostXid		shrinker;

	txn_vacuum_tests_made(io, database, __LINE__, 2, __LINE__, 2, __LINE__, 2, &made);
	reader = repo_Repo_reader(&made.repo, HOST_CACHE_PLACEMENT_SERVER, &made.reading);
	store = TREES_STORE_READER_NODE_STORE(&reader);
	writer = simc_begin();
	for (uint64 key = 1; key <= 14; key++)
		(void) txn_vacuum_tests_inserted(io, store, &made.held, database, key, 1036, writer);
	txn_vacuum_tests_committed_at(io, writer, 100);
	txn_vacuum_tests_folded_at(io, &made, 120);
	updater = simc_begin();
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_updated(io, store, &made.held, database, 1, 1, 1036, updater, NULL),
					  2 << 16 | 1);
	txn_vacuum_tests_committed_at(io, updater, 130);
	txn_vacuum_tests_folded_at(io, &made, 150);
	shrinker = simc_begin();
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_updated(io, store, &made.held, database, 2, 2, 100, shrinker, NULL),
					  8);
	txn_vacuum_tests_committed_at(io, shrinker, 175);
	if (folded)
		txn_vacuum_tests_folded_at(io, &made, 200);
	txn_vacuum_tests_made_drop(&made);
}

/* txn_vacuum_tests_block_0 returns a database's storage's block 0 entry, read through a state of its own. */
static txn_vacuum_tests_entry_t
txn_vacuum_tests_block_0(HostIoIo *io, uint32 database)
{
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostError	error;
	txn_vacuum_tests_entry_t entry;

	if (!txn_vacuum_tests_racing_state(io, database, &repo, &reading, &held, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	entry = txn_vacuum_tests_entry(io, &held, 0);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return entry;
}

/*
 * Bugs row 240's prune: a fold that installs a base with block 0's full flag
 * and drops the flag's version between a take-time prune's read of the older
 * base and its read of the delta moves the base generation, so the prune
 * reads the block again under the newest base and clears the flag, and the
 * update's new row and block 0's entry are a twin storage's that folds
 * before the same update.
 */
void
test_txn_vacuum_tests_a_fold_between_a_prunes_reads_leaves_what_a_prune_after_it_leaves(void)
{
	static const SimcStep steps[] = {{"folder", 6, UINT64_MAX}, {"updater", 7, UINT64_MAX}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_vacuum_tests_raced_t raced = {.database = 9463};
	SimcActor	updater = {txn_vacuum_tests_racing_updater, &raced};
	SimcActor	folder = {txn_vacuum_tests_racing_folder, &raced.database};
	uint64		twin;
	SimcRun		run;
	SimcReport	report;

	txn_vacuum_tests_filled_at(io, 9466, true);
	{
		RepoRepo	repo;
		HostSharedReader reading;
		HostSharedHeld held;
		HostError	error;

		if (!txn_vacuum_tests_racing_state(io, 9466, &repo, &reading, &held, &error))
			TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
		{
			TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);

			twin = txn_vacuum_tests_prune_updated(io, TREES_STORE_READER_NODE_STORE(&store), &held, 9466);
		}
		host_shared_Held_drop(&held);
		host_shared_Reader_drop(&reading);
		repo_Repo_drop(&repo);
	}
	txn_vacuum_tests_filled_at(io, 9463, false);
	run = simc_Run_new(83);
	simc_Run_actor(&run, "updater", 7, updater);
	simc_Run_actor(&run, "folder", 6, folder);
	simc_Run_until(&run, "updater", 7, "doltgres-prune-based");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_vacuum_tests_returned(&report, __FILE__, __LINE__, 2, __LINE__, 2);
	simc_Report_drop(&report);
	STD_ASSERT_EQ_U64(__LINE__, 5, raced.tid, twin);
	txn_vacuum_tests_entry_is(__LINE__, 5, txn_vacuum_tests_block_0(io, 9463), txn_vacuum_tests_block_0(io, 9466));
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_counted_t is the spent count race's captures: its database, then block 0's count of spent places. */
typedef struct txn_vacuum_tests_counted_t
{
	uint32		database;
	uint64		count;
} txn_vacuum_tests_counted_t;

/* txn_vacuum_tests_racing_counter is the spent count race's count of block 0's spent places, which it writes into its captures. */
static bool
txn_vacuum_tests_racing_counter(void *captures, HostIoIo *io, HostError *error)
{
	txn_vacuum_tests_counted_t *counted = captures;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	bool		ok;

	if (!txn_vacuum_tests_racing_state(io, counted->database, &repo, &reading, &held, error))
		return false;
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);

		ok = txn_vacuum_spent(io, TREES_STORE_READER_NODE_STORE(&store), &held, 0, &TXN_VACUUM_TESTS_PAGE,
							  &counted->count, error);
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/* txn_vacuum_tests_committed_filled fills a new storage of a database as filled does, its commits at 100, folding it at 200 when folded. */
static void
txn_vacuum_tests_committed_filled(HostIoIo *io, uint32 database, bool folded)
{
	txn_vacuum_tests_made_t made;
	TreesStoreReader store;

	txn_vacuum_tests_made(io, database, __LINE__, 2, __LINE__, 2, __LINE__, 2, &made);
	store = repo_Repo_reader(&made.repo, HOST_CACHE_PLACEMENT_SERVER, &made.reading);
	(void) txn_vacuum_tests_filled(io, TREES_STORE_READER_NODE_STORE(&store), &made.held, database, 100);
	if (folded)
		txn_vacuum_tests_folded_at(io, &made, 200);
	txn_vacuum_tests_made_drop(&made);
}

/*
 * Bugs row 240's spent count: a fold that installs a base mapping block 0's
 * live rows and drops their versions between ANALYZE's read of the base and
 * its mapped checks, while key 1's dead row keeps the block's takes in the
 * delta, moves the base generation, so the count reads the block again under
 * the newest base and counts what a twin storage that folds first counts.
 */
void
test_txn_vacuum_tests_a_fold_during_a_blocks_spent_count_counts_what_a_count_after_it_counts(void)
{
	static const SimcStep steps[] = {{"folder", 6, UINT64_MAX}, {"counter", 7, UINT64_MAX}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_vacuum_tests_counted_t counted = {.database = 9467};
	txn_vacuum_tests_counted_t twin = {.database = 9451};
	SimcActor	counter = {txn_vacuum_tests_racing_counter, &counted};
	SimcActor	folder = {txn_vacuum_tests_racing_folder, &counted.database};
	HostError	error;
	SimcRun		run;
	SimcReport	report;

	txn_vacuum_tests_committed_filled(io, 9451, true);
	if (!txn_vacuum_tests_racing_counter(&twin, io, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	txn_vacuum_tests_committed_filled(io, 9467, false);
	run = simc_Run_new(84);
	simc_Run_actor(&run, "counter", 7, counter);
	simc_Run_actor(&run, "folder", 6, folder);
	simc_Run_until(&run, "counter", 7, "doltgres-vacuum-found");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_vacuum_tests_returned(&report, __FILE__, __LINE__, 2, __LINE__, 2);
	simc_Report_drop(&report);
	STD_ASSERT_EQ_U64(__LINE__, 5, counted.count, twin.count);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_finding_t is the find race's captures: its database, then the tuple ids VACUUM's find found. */
typedef struct txn_vacuum_tests_finding_t
{
	uint32		database;
	StdVec		tids;
} txn_vacuum_tests_finding_t;

/* txn_vacuum_tests_racing_finder is the find race's VACUUM, whose find's tuple ids it writes into its captures. */
static bool
txn_vacuum_tests_racing_finder(void *captures, HostIoIo *io, HostError *error)
{
	txn_vacuum_tests_finding_t *finding = captures;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	SimcTaken	every;
	HostSnapshot lent;
	StdVec		found;
	bool		ok;

	if (!txn_vacuum_tests_racing_state(io, finding->database, &repo, &reading, &held, error))
		return false;
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);

		every = txn_vacuum_tests_every();
		lent = simc_Taken_lent(&every);
		ok = txn_vacuum_never_held(io, TREES_STORE_READER_NODE_STORE(&store), &held, &TXN_VACUUM_TESTS_PAGE, &lent,
								   &found, error);
		simc_Taken_drop(&every);
	}
	if (ok)
	{
		std_vec_new(&finding->tids, sizeof(uint64), _Alignof(uint64));
		for (size_t i = 0; i < found.len; i++)
			std_vec_push_Checked(&finding->tids, &STD_VEC_AT(&found, TxnVacuumFreed, i).f0, sizeof(uint64),
								 _Alignof(uint64));
		txn_vacuum_tests_found_free(&found);
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/*
 * Bugs row 240's find: a fold that installs a base mapping block 0's live
 * rows and drops their versions between VACUUM's read of the block's takes
 * and its mapped checks, while key 1's dead row keeps the takes in the delta,
 * moves the base generation, so the find reads the block again under the
 * newest base and finds what a twin storage that folds first finds.
 */
void
test_txn_vacuum_tests_a_fold_during_a_find_finds_what_a_find_after_it_finds(void)
{
	static const SimcStep steps[] = {{"folder", 6, UINT64_MAX}, {"finder", 6, UINT64_MAX}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_vacuum_tests_finding_t finding = {.database = 9454};
	txn_vacuum_tests_finding_t twin = {.database = 9455};
	SimcActor	finder = {txn_vacuum_tests_racing_finder, &finding};
	SimcActor	folder = {txn_vacuum_tests_racing_folder, &finding.database};
	HostError	error;
	SimcRun		run;
	SimcReport	report;

	txn_vacuum_tests_committed_filled(io, twin.database, true);
	if (!txn_vacuum_tests_racing_finder(&twin, io, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	txn_vacuum_tests_committed_filled(io, finding.database, false);
	run = simc_Run_new(86);
	simc_Run_actor(&run, "finder", 6, finder);
	simc_Run_actor(&run, "folder", 6, folder);
	simc_Run_until(&run, "finder", 6, "doltgres-vacuum-found");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_vacuum_tests_returned(&report, __FILE__, __LINE__, 2, __LINE__, 2);
	simc_Report_drop(&report);
	STD_ASSERT_EQ_U64(__LINE__, 5, finding.tids.len, twin.tids.len);
	for (size_t i = 0; i < twin.tids.len; i++)
		STD_ASSERT_EQ_U64(__LINE__, 9, STD_VEC_AT(&finding.tids, uint64, i), STD_VEC_AT(&twin.tids, uint64, i));
	std_vec_free_Checked(&finding.tids, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&twin.tids, sizeof(uint64), _Alignof(uint64));
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_racing_freer is the free race's VACUUM, of the database its captures name: it frees key 1's old tuple id, (0,1). */
static bool
txn_vacuum_tests_racing_freer(void *captures, HostIoIo *io, HostError *error)
{
	uint32		database = *(const uint32 *) captures;
	TxnVacuumFreed freed = {.f0 = 1, .f1 = 1036};
	HostStorage toast = {0};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	StdVec		claimed;
	bool		ok;

	if (!txn_vacuum_tests_racing_state(io, database, &repo, &reading, &held, error))
		return false;
	std_vec_new(&freed.f3, TXN_VACUUM_ID);
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);

		ok = txn_vacuum_freed(io, TREES_STORE_READER_NODE_STORE(&store), &held, NULL, toast,
							  txn_insert_tests_stored(database), true, &TXN_VACUUM_TESTS_PAGE, &freed, 1, false, &claimed,
							  error);
	}
	if (ok)
		std_vec_free_Checked(&claimed, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&freed.f3, TXN_VACUUM_ID);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/*
 * Bugs row 240's free: a fold that installs a base and drops a take of block
 * 0's between VACUUM's read of the base and its give back of (0,1) moves the
 * base generation, so the free reads the block's entry again under the
 * newest base, and block 0's entry is a twin storage's that folds before the
 * same free.
 */
void
test_txn_vacuum_tests_a_fold_during_a_free_leaves_what_a_free_after_it_leaves(void)
{
	static const SimcStep steps[] = {{"folder", 6, UINT64_MAX}, {"freer", 5, UINT64_MAX}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint32		database = 9452;
	uint32		twin = 9453;
	SimcActor	freer = {txn_vacuum_tests_racing_freer, &database};
	SimcActor	folder = {txn_vacuum_tests_racing_folder, &database};
	HostError	error;
	SimcRun		run;
	SimcReport	report;

	txn_vacuum_tests_filled_at(io, twin, true);
	if (!txn_vacuum_tests_racing_freer(&twin, io, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	txn_vacuum_tests_filled_at(io, database, false);
	run = simc_Run_new(85);
	simc_Run_actor(&run, "freer", 5, freer);
	simc_Run_actor(&run, "folder", 6, folder);
	simc_Run_until(&run, "freer", 5, "doltgres-vacuum-freeing");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_vacuum_tests_returned(&report, __FILE__, __LINE__, 2, __LINE__, 2);
	simc_Report_drop(&report);
	txn_vacuum_tests_entry_is(__LINE__, 5, txn_vacuum_tests_block_0(io, database), txn_vacuum_tests_block_0(io, twin));
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_redoing_t is the redo race's captures: its database, and the free's record it replays with its end. */
typedef struct txn_vacuum_tests_redoing_t
{
	uint32		database;
	const TxnRecordTidsFreed *free;
	HostLsn		end;
} txn_vacuum_tests_redoing_t;

/* txn_vacuum_tests_racing_replayer is the redo race's replay of a free on the database its captures name. */
static bool
txn_vacuum_tests_racing_replayer(void *captures, HostIoIo *io, HostError *error)
{
	const txn_vacuum_tests_redoing_t *redoing = captures;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	bool		ok;

	if (!txn_vacuum_tests_racing_state(io, redoing->database, &repo, &reading, &held, error))
		return false;
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);

		ok = txn_vacuum_redo_freed(io, TREES_STORE_READER_NODE_STORE(&store), &held, NULL, redoing->free,
								   &TXN_VACUUM_TESTS_PAGE, redoing->end, error);
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/*
 * Bugs row 319's case: a free logs exactly what it gives back, (0,1)'s 1036
 * bytes, and its record, replayed with a fold that installs a base and drops
 * a take of block 0's between the redo's read of the base and its give back,
 * leaves block 0's entry that of the twin storage whose free logged it. Its
 * databases are used by no other test, since the simulation's log holds every
 * test's records.
 */
void
test_txn_vacuum_tests_a_fold_during_a_frees_redo_leaves_what_the_free_left(void)
{
	static const SimcStep steps[] = {{"folder", 6, UINT64_MAX}, {"redoer", 6, UINT64_MAX}};
	static std_TupleU32U16U16U64_t runs[] = {{0, 1, 1, 1036}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint32		twin = 9532;
	txn_vacuum_tests_redoing_t redoing = {.database = 9531};
	static uint64 enders[] = {0};
	TxnRecordTidsFreed want = {.storage = txn_insert_tests_stored(twin), .runs = {runs, 1, 1}, .enders = {enders, 1, 1}};
	const TxnRecordTidsFreed *wants[] = {&want};
	SimcActor	redoer = {txn_vacuum_tests_racing_replayer, &redoing};
	SimcActor	folder = {txn_vacuum_tests_racing_folder, &redoing.database};
	HostError	error;
	StdVec		records;
	SimcRun		run;
	SimcReport	report;

	txn_vacuum_tests_filled_at(io, twin, true);
	if (!txn_vacuum_tests_racing_freer(&twin, io, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	records = txn_vacuum_tests_logged(twin);
	for (size_t k = 0; k < records.len && redoing.free == NULL; k++)
	{
		const txn_vacuum_tests_logged_t *record = &STD_VEC_AT(&records, txn_vacuum_tests_logged_t, k);

		if (record->freed)
		{
			redoing.free = &record->free;
			redoing.end = record->end;
		}
	}
	txn_vacuum_tests_frees_is(__LINE__, 5, &records, wants, lengthof(wants));
	txn_vacuum_tests_filled_at(io, redoing.database, false);
	run = simc_Run_new(87);
	simc_Run_actor(&run, "redoer", 6, redoer);
	simc_Run_actor(&run, "folder", 6, folder);
	simc_Run_until(&run, "redoer", 6, "doltgres-vacuum-freeing");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_vacuum_tests_returned(&report, __FILE__, __LINE__, 2, __LINE__, 2);
	simc_Report_drop(&report);
	txn_vacuum_tests_entry_is(__LINE__, 5, txn_vacuum_tests_block_0(io, redoing.database),
							  txn_vacuum_tests_block_0(io, twin));
	txn_vacuum_tests_logged_free(&records);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_racing_redoer is the redo race's replay, at 300, of a free giving back (0,1)'s 1036 bytes on the database its captures name. */
static bool
txn_vacuum_tests_racing_redoer(void *captures, HostIoIo *io, HostError *error)
{
	uint32		database = *(const uint32 *) captures;
	std_TupleU32U16U16U64_t run = {0, 1, 1, 1036};
	TxnRecordTidsFreed free = {.storage = txn_insert_tests_stored(database), .runs = txn_vacuum_runs_view(&run, 1),
	.dead = txn_vacuum_runs_view(NULL, 0)};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	bool		ok;

	if (!txn_vacuum_tests_racing_state(io, database, &repo, &reading, &held, error))
		return false;
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);

		ok = txn_vacuum_redo_freed(io, TREES_STORE_READER_NODE_STORE(&store), &held, NULL, &free,
								   &TXN_VACUUM_TESTS_PAGE, 300, error);
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/*
 * Bugs row 240's redo: a fold that installs a base and drops a take of block
 * 0's between a free's redo's read of the base and its give back of (0,1)
 * moves the base generation, so the redo reads the block's entry again under
 * the newest base, and block 0's entry is a twin storage's that folds before
 * the same redo.
 */
void
test_txn_vacuum_tests_a_fold_during_a_frees_redo_leaves_what_a_redo_after_it_leaves(void)
{
	static const SimcStep steps[] = {{"folder", 6, UINT64_MAX}, {"redoer", 6, UINT64_MAX}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint32		database = 9456;
	uint32		twin = 9468;
	SimcActor	redoer = {txn_vacuum_tests_racing_redoer, &database};
	SimcActor	folder = {txn_vacuum_tests_racing_folder, &database};
	HostError	error;
	SimcRun		run;
	SimcReport	report;

	txn_vacuum_tests_filled_at(io, twin, true);
	if (!txn_vacuum_tests_racing_redoer(&twin, io, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	txn_vacuum_tests_filled_at(io, database, false);
	run = simc_Run_new(87);
	simc_Run_actor(&run, "redoer", 6, redoer);
	simc_Run_actor(&run, "folder", 6, folder);
	simc_Run_until(&run, "redoer", 6, "doltgres-vacuum-freeing");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_vacuum_tests_returned(&report, __FILE__, __LINE__, 2, __LINE__, 2);
	simc_Report_drop(&report);
	txn_vacuum_tests_entry_is(__LINE__, 5, txn_vacuum_tests_block_0(io, database), txn_vacuum_tests_block_0(io, twin));
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_deleted deletes a key's row of a storage as a transaction's, a delete version pushed onto the key's chain. */
static void
txn_vacuum_tests_deleted(HostIoIo *io, const HostSharedHeld *held, uint64 key, HostXid xid)
{
	TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_DELETE};
	TxnMemtableVersionChange change = {.writer = {.xid = xid.f0}, .op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
	.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableVersionHandle handle;
	HostError	error;
	uint8		bytes[8];

	std_u64_to_be_bytes(key, bytes);
	if (!txn_memtable_table_push(io, host_shared_Held_record(held), bytes, sizeof(bytes), &change, &handle, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(1441, 69, &error);
}

/* txn_vacuum_tests_horizons returns the conflict horizons of the frees a database logged, in their order. */
static StdVec
txn_vacuum_tests_horizons(uint32 database)
{
	StdVec		records = txn_vacuum_tests_logged(database);
	StdVec		horizons;

	std_vec_new(&horizons, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0; k < records.len; k++)
	{
		const txn_vacuum_tests_logged_t *record = &STD_VEC_AT(&records, txn_vacuum_tests_logged_t, k);

		if (record->freed)
			std_vec_push_Checked(&horizons, &record->free.horizon, sizeof(uint64), _Alignof(uint64));
	}
	txn_vacuum_tests_logged_free(&records);
	return horizons;
}

/* txn_vacuum_tests_u64s_is_with asserts at a line and column, with a message, that a Vec<u64> is the u64s wanted. */
static void
txn_vacuum_tests_u64s_is_with(uint32 line, uint32 col, const StdVec *got, const uint64 *want, size_t want_len,
							  const StdFmtArguments *message)
{
	StdVec		wanted = {(void *) want, want_len, want_len};

	if (got->len != want_len || memcmp(got->ptr, want, want_len * sizeof(uint64)) != 0)
		std_assert_failed(RUST_FILE, line, col, "==", got, chunks_trailer_VecU64_Debug_fmt, &wanted,
						  chunks_trailer_VecU64_Debug_fmt, message);
}

/* txn_vacuum_tests_enders runs the dropped rows' case on a database: same is whether the rows' own writer ended them. */
static void
txn_vacuum_tests_enders(HostIoIo *io, uint32 database, bool same)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&same, std_bool_Display_fmt}};
	StdFmtArguments message = {pieces, lengthof(pieces), args};
	txn_vacuum_tests_made_t made;
	TreesStoreReader reader;
	TreesStoreNodeStore store;
	HostXid		writer;
	uint64		tids[2];
	uint64		kept;
	uint64		enders[2];
	uint64		kept_enders[2];
	StdVec		takers;
	StdVec		stored_enders;
	StdVec		found;
	StdVec		horizons;
	TxnVacuumFreed want[2];
	TxnBaseBase base;
	HostError	error;
	bool		any = false;
	bool		some;

	txn_vacuum_tests_made(io, database, 1456, 95, 1457, 50, 1459, 107, &made);
	reader = repo_Repo_reader(&made.repo, HOST_CACHE_PLACEMENT_SERVER, &made.reading);
	store = TREES_STORE_READER_NODE_STORE(&reader);
	writer = simc_begin();
	for (uint64 key = 1; key <= 2; key++)
		tids[key - 1] = txn_vacuum_tests_inserted(io, store, &made.held, database, key, 100, writer);
	kept = txn_vacuum_tests_inserted(io, store, &made.held, database, 3, 100, writer);
	txn_vacuum_tests_committed_status(io, writer, 100, 1464, 85);
	if (same)
	{
		txn_vacuum_tests_deleted(io, &made.held, 1, writer);
		txn_vacuum_tests_deleted(io, &made.held, 2, writer);
		txn_vacuum_tests_ended(io, writer, true);
		enders[0] = enders[1] = 0;
	}
	else
	{
		HostXid		deleter;
		HostXid		updater;

		txn_vacuum_tests_ended(io, writer, true);
		deleter = simc_begin();
		updater = simc_begin();
		txn_vacuum_tests_deleted(io, &made.held, 1, deleter);
		txn_vacuum_tests_updated(io, store, &made.held, database, 2, tids[1], 100, updater, NULL);
		txn_vacuum_tests_committed_status(io, deleter, 120, 1478, 94);
		txn_vacuum_tests_ended(io, deleter, true);
		txn_vacuum_tests_committed_status(io, updater, 140, 1478, 94);
		txn_vacuum_tests_ended(io, updater, true);
		enders[0] = deleter.f0;
		enders[1] = updater.f0;
	}
	STD_ASSERT(1485, 13, txn_vacuum_tests_folded(io, &made.repo, &made.reading, &made.held, 1485, 105),
			   "crate::fold::folded(io, (&repo, &reading), &held, &order, false, 200, (None, None)).unwrap()");
	takers = txn_vacuum_tests_takers(io, &made.held, __LINE__, 9);
	if (takers.len != 0)
		std_panic_fmt(RUST_FILE, __LINE__, 9, &message);
	txn_vacuum_tests_takers_free(&takers);
	if (!txn_state_base_record(io, &made.held, &some, &base, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	if (!some || !base.has_pending)
		STD_OPTION_UNWRAP_FAILED(__LINE__, 9);
	std_vec_new(&stored_enders, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0; k < lengthof(tids); k++)
	{
		uint8		key[6];
		StdVec		value;
		ChunksErrorError failed;
		uint64		ender;
		uint32		heap_bytes;
		StdVec		values;
		bool		found_value;

		txn_base_tuple_id_key(tids[k], key);
		if (!trees_get(io, store, TREES_STORE_TREE_NAMED(&base.pending), key, sizeof(key), &found_value, &value,
					   &failed))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		if (!found_value)
			STD_OPTION_UNWRAP_FAILED(__LINE__, 9);
		if (!txn_base_pending_read(value.ptr, value.len, &ender, &heap_bytes, &values, &error))
			TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
		std_vec_free_Checked(&values, sizeof(uint32), _Alignof(uint32));
		std_vec_free_Checked(&value, 1, 1);
		std_vec_push_Checked(&stored_enders, &ender, sizeof(uint64), _Alignof(uint64));
	}
	txn_base_Base_drop(&base);
	kept_enders[0] = same ? writer.f0 | TXN_BASE_OWN : enders[0];
	kept_enders[1] = same ? writer.f0 | TXN_BASE_OWN : enders[1];
	txn_vacuum_tests_u64s_is_with(1496, 13, &stored_enders, kept_enders, lengthof(kept_enders), &message);
	found = txn_vacuum_tests_found_every(io, store, &made.held, 1497, 81);
	want[0] = (TxnVacuumFreed)
	{
		tids[0], 100, enders[0]
	};
	want[1] = (TxnVacuumFreed)
	{
		tids[1], 100, enders[1]
	};
	txn_vacuum_tests_found_is_with(1498, 13, &found, want, lengthof(want), &message);
	for (size_t k = 0; k < found.len; k++)
		any = any || STD_VEC_AT(&found, TxnVacuumFreed, k).f0 == kept;
	STD_ASSERT(1499, 13, !any, "!found.iter().any(|&(tid, ..)| tid == kept)");
	txn_vacuum_tests_freed(io, store, &made.held, database, &found, false, 1500, 96);
	horizons = txn_vacuum_tests_horizons(database);
	txn_vacuum_tests_u64s_is_with(1508, 13, &horizons, &enders[1], 1, &message);
	std_vec_free_Checked(&horizons, sizeof(uint64), _Alignof(uint64));
	txn_vacuum_tests_found_free(&found);
	std_vec_free_Checked(&stored_enders, sizeof(uint64), _Alignof(uint64));
	txn_vacuum_tests_made_drop(&made);
}

/*
 * The fold's drop's D2: rows a fold's point applied and whose tuple ids its
 * base doesn't pair leave the delta with the drop, and with them their
 * takes, whose tuple ids the base's pending tree holds with each row's
 * ender's xid (bugs row 228's (E)), so VACUUM's free of the tuple ids logs
 * the newest as the block's conflict horizon, as heap's prune record holds
 * the newest removed tuple's xmax: a row another transaction deleted and one
 * another transaction updated give their enders, and a row its own
 * transaction inserted and deleted keeps its writer's xid, marked OWN, but
 * gives no horizon, so a block of only such rows logs 0.
 */
void
test_txn_vacuum_tests_a_dropped_rows_tuple_id_keeps_its_enders_xid_for_its_frees_horizon(void)
{
	SimcTestIo	test_io = simc_io();

	txn_vacuum_tests_enders(&test_io.io, 9457, false);
	txn_vacuum_tests_enders(&test_io.io, 9458, true);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_widened is the epoch case: a take's ender just below an epoch's start widens against its writer to the earlier epoch's. */
static void
txn_vacuum_tests_widened(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	HostXid		xid = {(UINT64CONST(2) << 32) + 5};
	uint64		tid = txn_vacuum_tests_inserted(io, store, held, 9459, 1, 100, xid);
	HostRow		taken = {tid};
	HostError	error;
	StdVec		takers;
	StdVec		records;
	txn_TupleWriterRowEntryU64TupleVecU32Bool_t *last;
	TxnVacuumFreed freeing;
	StdVec		claimed;

	if (!txn_memtable_map_ended(io, host_shared_Held_record(held), taken, xid.f0, 0xfffffff0, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(1521, 66, &error);
	takers = txn_vacuum_tests_takers(io, held, 1522, 93);
	if (takers.len == 0)
		STD_OPTION_UNWRAP_FAILED(1522, 108);
	last = &STD_VEC_AT(&takers, txn_TupleWriterRowEntryU64TupleVecU32Bool_t, takers.len - 1);
	{
		HostRow		row = {tid};
		uint64		writer = (UINT64CONST(2) << 32) + 5;
		uint64		ender = (UINT64CONST(2) << 32) - 16;
		StdTupleRef got = STD_TUPLE(3, STD_ITEM(&last->f0.xid, std_u64_Debug_fmt), STD_ITEM(&last->f1, host_Row_Debug_fmt),
									STD_ITEM(&last->f3, std_u64_Debug_fmt));
		StdTupleRef want = STD_TUPLE(3, STD_ITEM(&writer, std_u64_Debug_fmt), STD_ITEM(&row, host_Row_Debug_fmt),
									 STD_ITEM(&ender, std_u64_Debug_fmt));

		STD_ASSERT_EQ(1523, 13, last->f0.xid == writer && last->f1.f0 == tid && last->f3 == ender, &got,
					  std_tuple_Debug_fmt, &want, std_tuple_Debug_fmt);
	}
	freeing = (TxnVacuumFreed)
	{
		tid, 100, last->f3
	};
	std_vec_new(&freeing.f3, sizeof(uint32), _Alignof(uint32));
	claimed = txn_vacuum_tests_claimed(io, store, held, 9459, &freeing, 1, false, 1525, 18);
	std_vec_free_Checked(&claimed, sizeof(uint64), _Alignof(uint64));
	txn_vacuum_tests_takers_free(&takers);
	records = txn_vacuum_tests_logged(9459);
	if (records.len == 0 || !STD_VEC_AT(&records, txn_vacuum_tests_logged_t, records.len - 1).freed)
		STD_PANIC(1526, 75, "no free was logged");
	STD_ASSERT_EQ_U64(1527, 13, STD_VEC_AT(&records, txn_vacuum_tests_logged_t, records.len - 1).free.horizon,
					  (UINT64CONST(2) << 32) - 16);
	txn_vacuum_tests_logged_free(&records);
	if (!txn_written_ended(io, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(1528, 39, &error);
}

/*
 * D2's width: a take keeps its row's ender's 32 bits, which read back
 * widened against the take's writer's full xid, so an ender just below an
 * epoch's start whose take's writer is in the next epoch reads as the earlier
 * epoch's, and the free's record carries that full xid as its horizon.
 */
void
test_txn_vacuum_tests_a_takes_ender_widens_against_its_writer_across_an_epoch(void)
{
	SimcTestIo	test_io = simc_io();

	txn_vacuum_tests_with(&test_io.io, 9459, txn_vacuum_tests_widened, NULL);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 101: VACUUM's find leaves a dropped row's tuple id while the
 * relation's horizon hasn't passed its ender, as heap's prune keeps a row a
 * standby's feedback holds, and finds it once the horizon passes.
 */
void
test_txn_vacuum_tests_a_dropped_rows_tuple_id_waits_for_the_horizon_to_pass_its_ender(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_vacuum_tests_made_t made;
	TreesStoreReader reader;
	TreesStoreNodeStore store;
	HostXid		writer;
	HostXid		deleter;
	HostXid		updater;
	uint64		tids[2];
	SimcTaken	horizon;
	StdVec		found;
	TxnVacuumFreed between[1];
	TxnVacuumFreed passed[2];

	txn_vacuum_tests_made(io, 9461, 1539, 87, 1540, 46, 1541, 106, &made);
	reader = repo_Repo_reader(&made.repo, HOST_CACHE_PLACEMENT_SERVER, &made.reading);
	store = TREES_STORE_READER_NODE_STORE(&reader);
	writer = simc_begin();
	for (uint64 key = 1; key <= 2; key++)
		tids[key - 1] = txn_vacuum_tests_inserted(io, store, &made.held, 9461, key, 100, writer);
	txn_vacuum_tests_committed_status(io, writer, 100, 1545, 81);
	txn_vacuum_tests_ended(io, writer, true);
	deleter = simc_begin();
	updater = simc_begin();
	txn_vacuum_tests_deleted(io, &made.held, 1, deleter);
	txn_vacuum_tests_updated(io, store, &made.held, 9461, 2, tids[1], 100, updater, NULL);
	txn_vacuum_tests_committed_status(io, deleter, 120, 1551, 82);
	txn_vacuum_tests_ended(io, deleter, true);
	txn_vacuum_tests_committed_status(io, updater, 140, 1551, 82);
	txn_vacuum_tests_ended(io, updater, true);
	STD_ASSERT(1555, 9, txn_vacuum_tests_folded(io, &made.repo, &made.reading, &made.held, 1555, 101),
			   "crate::fold::folded(io, (&repo, &reading), &held, &order, false, 200, (None, None)).unwrap()");
	horizon = simc_non_vacuumable(deleter);
	found = txn_vacuum_tests_found(io, store, &made.held, &horizon, 1556, 102);
	simc_Taken_drop(&horizon);
	txn_vacuum_tests_found_is(1557, 9, &found, NULL, 0);
	txn_vacuum_tests_found_free(&found);
	horizon = simc_non_vacuumable(updater);
	found = txn_vacuum_tests_found(io, store, &made.held, &horizon, 1558, 100);
	simc_Taken_drop(&horizon);
	between[0] = (TxnVacuumFreed)
	{
		tids[0], 100, deleter.f0
	};
	txn_vacuum_tests_found_is(1559, 9, &found, between, lengthof(between));
	txn_vacuum_tests_found_free(&found);
	found = txn_vacuum_tests_found_every(io, store, &made.held, 1560, 78);
	passed[0] = between[0];
	passed[1] = (TxnVacuumFreed)
	{
		tids[1], 100, updater.f0
	};
	txn_vacuum_tests_found_is(1561, 9, &found, passed, lengthof(passed));
	txn_vacuum_tests_found_free(&found);
	txn_vacuum_tests_made_drop(&made);
	simc_TestIo_drop(&test_io);
}

/*
 * PRevPF 8's R101a: a row its own transaction inserted and deleted keeps its
 * tuple id taken while the relation's horizon hasn't passed that transaction,
 * as heap's prune keeps a tuple whose xmax equals its xmin, and its free then
 * logs no conflict horizon.
 */
void
test_txn_vacuum_tests_a_row_its_own_transaction_inserted_and_deleted_waits_for_the_horizon(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_vacuum_tests_made_t made;
	TreesStoreReader reader;
	TreesStoreNodeStore store;
	HostXid		writer;
	uint64		tid;
	SimcTaken	horizon;
	StdVec		found;
	TxnVacuumFreed passed[1];

	txn_vacuum_tests_made(io, 9462, 1572, 87, 1573, 46, 1574, 106, &made);
	reader = repo_Repo_reader(&made.repo, HOST_CACHE_PLACEMENT_SERVER, &made.reading);
	store = TREES_STORE_READER_NODE_STORE(&reader);
	writer = simc_begin();
	tid = txn_vacuum_tests_inserted(io, store, &made.held, 9462, 1, 100, writer);
	txn_vacuum_tests_committed_status(io, writer, 100, 1578, 81);
	txn_vacuum_tests_deleted(io, &made.held, 1, writer);
	txn_vacuum_tests_ended(io, writer, true);
	STD_ASSERT(1582, 9, txn_vacuum_tests_folded(io, &made.repo, &made.reading, &made.held, 1582, 101),
			   "crate::fold::folded(io, (&repo, &reading), &held, &order, false, 200, (None, None)).unwrap()");
	horizon = simc_non_vacuumable(writer);
	found = txn_vacuum_tests_found(io, store, &made.held, &horizon, 1583, 101);
	simc_Taken_drop(&horizon);
	txn_vacuum_tests_found_is(1584, 9, &found, NULL, 0);
	txn_vacuum_tests_found_free(&found);
	found = txn_vacuum_tests_found_every(io, store, &made.held, 1585, 76);
	passed[0] = (TxnVacuumFreed)
	{
		tid, 100, 0
	};
	txn_vacuum_tests_found_is(1585, 9, &found, passed, lengthof(passed));
	txn_vacuum_tests_found_free(&found);
	txn_vacuum_tests_made_drop(&made);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_spilled_block is the spilled block case: two takes of a block spilled, then a free of the first's tuple id. */
static void
txn_vacuum_tests_spilled_block(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	HostStorage storage = txn_insert_tests_stored(9471);
	HostXid		writer = simc_begin();
	uint64		first = txn_vacuum_tests_inserted(io, store, held, 9471, 1, 100, writer);
	uint64		second = txn_vacuum_tests_inserted(io, store, held, 9471, 2, 100, writer);
	uint32		block = (uint32) (first >> 16);
	TxnVacuumFreed freeing = {first, 100, 0};
	txn_vacuum_tests_entry_t before;
	txn_vacuum_tests_entry_t want;
	HostError	error;
	StdVec		claimed;
	StdVec		chain;
	HostDeltaHold hold;
	bool		spilled;

	(void) captures;
	STD_ASSERT(__LINE__, 9, (uint32) (second >> 16) == block, "both takes are of one block");
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_FREE, writer.f0, &spilled, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	STD_ASSERT(__LINE__, 9, spilled, "spilled(io, &storage, Of::Free, writer).unwrap()");
	if (!host_delta_hold(io, &hold, &error) ||
		!txn_memtable_map_versions(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, &hold, host_shared_Held_record(held), block,
								   &chain, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	STD_ASSERT(__LINE__, 9, chain.len == 0, "the block's takes left the memtable");
	txn_memtable_version_versions_drop(&chain, 0);
	host_delta_Hold_drop(&hold);
	before = txn_vacuum_tests_entry(io, held, block);
	STD_ASSERT(__LINE__, 9, before.has, "the block's newest spilled entry reads");
	want = before;
	txn_free_space_Free_give_back(&want.free, (uint16) (first & 0xffff), (uint16) (first & 0xffff), 100);
	want.free.full = false;
	txn_vacuum_tests_committed_status(io, writer, 100, __LINE__, 9);
	txn_vacuum_tests_ended(io, writer, true);
	std_vec_new(&freeing.f3, sizeof(uint32), _Alignof(uint32));
	claimed = txn_vacuum_tests_claimed(io, store, held, 9471, &freeing, 1, false, __LINE__, 9);
	std_vec_free_Checked(&claimed, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&freeing.f3, sizeof(uint32), _Alignof(uint32));
	txn_vacuum_tests_entry_is(__LINE__, 9, txn_vacuum_tests_entry(io, held, block), want);
}

/*
 * VACUUM starts from a block's newest version, spilled or not: a free of a
 * tuple id of a block whose free space takes all spilled gives its bytes and
 * offset back to the block's newest spilled entry, never to the base's entry
 * below it, so the block keeps the other take.
 */
void
test_txn_vacuum_tests_a_free_of_a_block_whose_takes_all_spilled_starts_from_its_newest_spilled_entry(void)
{
	SimcTestIo	test_io = simc_io();

	txn_vacuum_tests_with(&test_io.io, 9471, txn_vacuum_tests_spilled_block, NULL);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_taken_again is the case of a freed tuple id another writer took again before the fold's drop ended its first row. */
static void
txn_vacuum_tests_taken_again(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	HostXid		first = {5};
	HostXid		second = {6};
	uint64		tid = txn_vacuum_tests_inserted(io, store, held, 9472, 1, 100, first);
	TxnVacuumFreed freeing = {tid, 100, 0};
	HostRow		taken = {tid};
	HostError	error;
	StdVec		claimed;
	StdVec		takers;
	uint64		enders[2] = {0};

	txn_vacuum_tests_ended(io, first, true);
	std_vec_new(&freeing.f3, sizeof(uint32), _Alignof(uint32));
	claimed = txn_vacuum_tests_claimed(io, store, held, 9472, &freeing, 1, false, __LINE__, 12);
	std_vec_free_Checked(&claimed, sizeof(uint64), _Alignof(uint64));
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_inserted(io, store, held, 9472, 2, 100, second), tid);
	txn_vacuum_tests_ended(io, second, true);
	if (!txn_memtable_map_ended(io, host_shared_Held_record(held), taken, first.f0, 9, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 87, &error);
	takers = txn_vacuum_tests_takers(io, held, __LINE__, 11);
	for (size_t i = 0; i < takers.len; i++)
	{
		const txn_TupleWriterRowEntryU64TupleVecU32Bool_t *taker = &STD_VEC_AT(&takers,
																			   txn_TupleWriterRowEntryU64TupleVecU32Bool_t,
																			   i);

		if (taker->f1.f0 == tid && taker->f0.xid == first.f0)
			enders[0] = taker->f3;
		else if (taker->f1.f0 == tid && taker->f0.xid == second.f0)
			enders[1] = taker->f3;
	}
	txn_vacuum_tests_takers_free(&takers);
	STD_ASSERT_EQ_U64(__LINE__, 5, enders[0], 9);
	STD_ASSERT_EQ_U64(__LINE__, 5, enders[1], 0);
}

/*
 * A fold's drop stores a row's ender on the take by which the row's writer
 * took its tuple id: on a standby whose fold lags its replay, VACUUM may
 * already have freed the tuple id and another writer taken it again, and
 * that later take keeps no ender, as on its primary, where the fold ran
 * before the free.
 */
void
test_txn_vacuum_tests_a_drops_ender_goes_on_its_rows_take_not_a_later_take_of_the_tuple_id(void)
{
	SimcTestIo	test_io = simc_io();

	txn_vacuum_tests_with(&test_io.io, 9472, txn_vacuum_tests_taken_again, NULL);
	simc_TestIo_drop(&test_io);
}


/* txn_vacuum_tests_holders returns the writers of the row versions in a tuple id's key's chain that hold it, newest first. */
static StdVec
txn_vacuum_tests_holders(HostIoIo *io, const HostSharedHeld *held, uint64 tid)
{
	HostSharedRecord record = host_shared_Held_record(held);
	HostRow		row = {tid};
	HostDeltaHold hold;
	HostError	error;
	StdVec		key;
	StdVec		chain;
	StdVec		writers;
	bool		some;

	std_vec_new(&writers, sizeof(uint64), _Alignof(uint64));
	if (!txn_memtable_tuples_get(io, record, row, &some, &key, &error) || !some ||
		!host_delta_hold(io, &hold, &error) ||
		!txn_memtable_table_versions(io, &hold, record, key.ptr, key.len, &chain, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	for (size_t i = 0; i < chain.len; i++)
	{
		TxnMemtableVersionVersion *version = &STD_VEC_AT(&chain, TxnMemtableVersionVersion, i);
		TxnMemtableVersionRecord stored = txn_memtable_version_Version_record(version);
		uint64		xid = txn_memtable_version_Record_writer(&stored).xid;

		if (txn_vacuum_holds(version, tid))
			std_vec_push_Checked(&writers, &xid, sizeof(uint64), _Alignof(uint64));
	}
	txn_memtable_version_versions_drop(&chain, 0);
	host_delta_Hold_drop(&hold);
	std_vec_free_Checked(&key, 1, 1);
	return writers;
}

/*
 * txn_vacuum_tests_retaken is row 320's same-key case: a row ended by a
 * deleter, its tuple id freed and taken again on the same key, the retake
 * ended by another writer when its captures say so, then the free's take-out
 * as a lagging standby defers it.
 */
static void
txn_vacuum_tests_retaken(void *captures, HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held)
{
	const bool *ended = captures;
	HostXid		writer = simc_begin();
	HostXid		deleter = simc_begin();
	HostXid		retaker = simc_begin();
	HostXid		ender = simc_begin();
	uint64		tid = txn_vacuum_tests_inserted(io, store, held, 9473, 1, 100, writer);
	TxnVacuumFreed freeing = {tid, 100, deleter.f0, .f4 = deleter.f0};
	std_TupleU32U16U16U64_t run = {(uint32) (tid >> 16), (uint16) tid, 1, 100};
	uint64		enders[] = {deleter.f0};
	TxnRecordTidsFreed freed = {.storage = txn_insert_tests_stored(9473), .runs = txn_vacuum_runs_view(&run, 1),
	.dead = txn_vacuum_runs_view(NULL, 0), .enders = {enders, 1, 1}};
	HostError	error;
	StdVec		claimed;
	StdVec		holders;

	txn_vacuum_tests_ended(io, writer, true);
	txn_vacuum_tests_deleted(io, held, 1, deleter);
	txn_vacuum_tests_ended(io, deleter, true);
	std_vec_new(&freeing.f3, sizeof(uint32), _Alignof(uint32));
	claimed = txn_vacuum_tests_claimed(io, store, held, 9473, &freeing, 1, false, __LINE__, 12);
	std_vec_free_Checked(&claimed, sizeof(uint64), _Alignof(uint64));
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_inserted(io, store, held, 9473, 1, 100, retaker), tid);
	txn_vacuum_tests_ended(io, retaker, true);
	if (*ended)
	{
		txn_vacuum_tests_deleted(io, held, 1, ender);
		txn_vacuum_tests_ended(io, ender, true);
	}
	holders = txn_vacuum_tests_holders(io, held, tid);
	STD_ASSERT_EQ_U64(__LINE__, 5, holders.len, 2);
	std_vec_free_Checked(&holders, sizeof(uint64), _Alignof(uint64));
	if (!txn_vacuum_taken_out(io, held, &freed, 1, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	holders = txn_vacuum_tests_holders(io, held, tid);
	STD_ASSERT_EQ_U64(__LINE__, 5, holders.len, 1);
	STD_ASSERT_EQ_U64(__LINE__, 5, STD_VEC_AT(&holders, uint64, 0), retaker.f0);
	std_vec_free_Checked(&holders, sizeof(uint64), _Alignof(uint64));
}

/*
 * Bugs row 320's same-key case: a lagging standby's deferred take-out of a
 * free whose tuple id was taken again on the same key takes out the freed
 * incarnation's version, the one its stored ender ended, and leaves the
 * retake's, live or ended by another writer, as its primary does.
 */
void
test_txn_vacuum_tests_a_frees_take_out_leaves_a_retake_on_its_key(void)
{
	SimcTestIo	test_io = simc_io();
	bool		live = false;
	bool		ended = true;

	txn_vacuum_tests_with(&test_io.io, 9473, txn_vacuum_tests_retaken, &live);
	simc_TestIo_drop(&test_io);
	test_io = simc_io();
	txn_vacuum_tests_with(&test_io.io, 9473, txn_vacuum_tests_retaken, &ended);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_holder checks that a tuple id's key's chain has one version that holds it, its writer's. */
static void
txn_vacuum_tests_holder(HostIoIo *io, const HostSharedHeld *held, uint64 tid, HostXid writer, uint32 line)
{
	StdVec		holders = txn_vacuum_tests_holders(io, held, tid);

	STD_ASSERT_EQ_U64(line, 5, holders.len, 1);
	STD_ASSERT_EQ_U64(line, 5, STD_VEC_AT(&holders, uint64, 0), writer.f0);
	std_vec_free_Checked(&holders, sizeof(uint64), _Alignof(uint64));
}

/* txn_vacuum_tests_taken_out_as takes a free of one tuple id with its stored ender out of a storage's delta, as a lagging standby defers it, in field 6 when dead. */
static void
txn_vacuum_tests_taken_out_as(HostIoIo *io, const HostSharedHeld *held, uint32 database, uint64 tid, uint64 ender,
							  bool dead)
{
	std_TupleU32U16U16U64_t run = {(uint32) (tid >> 16), (uint16) tid, 1, 100};
	TxnRecordTidsFreed freed = {.storage = txn_insert_tests_stored(database), .runs = txn_vacuum_runs_view(&run, 1),
	.dead = txn_vacuum_runs_view(NULL, 0), .enders = {&ender, 1, 1}};
	HostError	error;

	if (dead)
	{
		freed.dead = freed.runs;
		freed.runs = txn_vacuum_runs_view(NULL, 0);
	}
	if (!txn_vacuum_taken_out(io, held, &freed, 1, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
}

/* TXN_VACUUM_TESTS_RETAKE_RUNNING and the two after it are how the no-ender case's retake ends: left running, committed, or aborted. */
#define TXN_VACUUM_TESTS_RETAKE_RUNNING 0
#define TXN_VACUUM_TESTS_RETAKE_COMMITTED 1
#define TXN_VACUUM_TESTS_RETAKE_ABORTED 2

/*
 * txn_vacuum_tests_unended is row 320's no-ender case: an aborted insert a
 * fold dropped beside another key's committed row, so its free finds no
 * ender, its tuple id taken again on the same key by a retake that ends as
 * the case says, then the free's take-out, in field 5 or as a dead run in
 * field 6, as a lagging standby defers it; a retake aborted as a standby
 * replays its abort keeps its version until a later fold, of another key's
 * committed row, drops it.
 */
static void
txn_vacuum_tests_unended(int retake, bool dead)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_vacuum_tests_made_t made;
	TreesStoreReader reader;
	TreesStoreNodeStore store;
	HostXid		writer = simc_begin();
	HostXid		keeper = simc_begin();
	HostXid		retaker;
	uint64		tid;
	uint64		ender;
	StdVec		found;
	StdVec		claimed;

	txn_vacuum_tests_made(io, 9474, __LINE__, 2, __LINE__, 2, __LINE__, 2, &made);
	reader = repo_Repo_reader(&made.repo, HOST_CACHE_PLACEMENT_SERVER, &made.reading);
	store = TREES_STORE_READER_NODE_STORE(&reader);
	tid = txn_vacuum_tests_inserted(io, store, &made.held, 9474, 1, 100, writer);
	txn_vacuum_tests_ended(io, writer, false);
	(void) txn_vacuum_tests_inserted(io, store, &made.held, 9474, 2, 100, keeper);
	txn_vacuum_tests_committed_at(io, keeper, 110);
	txn_vacuum_tests_folded_at(io, &made, 120);
	found = txn_vacuum_tests_found_every(io, store, &made.held, __LINE__, 9);
	STD_ASSERT_EQ_U64(__LINE__, 5, found.len, 1);
	STD_ASSERT_EQ_U64(__LINE__, 5, STD_VEC_AT(&found, TxnVacuumFreed, 0).f0, tid);
	ender = STD_VEC_AT(&found, TxnVacuumFreed, 0).f4;
	STD_ASSERT_EQ_U64(__LINE__, 5, ender, 0);
	claimed = txn_vacuum_tests_claimed(io, store, &made.held, 9474, found.ptr, found.len, false, __LINE__, 12);
	std_vec_free_Checked(&claimed, sizeof(uint64), _Alignof(uint64));
	txn_vacuum_tests_found_free(&found);
	retaker = simc_begin();
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_inserted(io, store, &made.held, 9474, 1, 100, retaker), tid);
	if (retake == TXN_VACUUM_TESTS_RETAKE_ABORTED)
	{
		HostXid		later = simc_begin();

		simc_abort(retaker);
		STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_versions_len(io, &made.held, 1, __LINE__, 9, __LINE__, 9), 1);
		txn_vacuum_tests_taken_out_as(io, &made.held, 9474, tid, ender, dead);
		STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_versions_len(io, &made.held, 1, __LINE__, 9, __LINE__, 9), 1);
		(void) txn_vacuum_tests_inserted(io, store, &made.held, 9474, 3, 100, later);
		txn_vacuum_tests_committed_at(io, later, 150);
		txn_vacuum_tests_folded_at(io, &made, 200);
		STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_versions_len(io, &made.held, 1, __LINE__, 9, __LINE__, 9), 0);
	}
	else
	{
		if (retake == TXN_VACUUM_TESTS_RETAKE_COMMITTED)
			txn_vacuum_tests_committed_at(io, retaker, 130);
		txn_vacuum_tests_holder(io, &made.held, tid, retaker, __LINE__);
		txn_vacuum_tests_taken_out_as(io, &made.held, 9474, tid, ender, dead);
		txn_vacuum_tests_holder(io, &made.held, tid, retaker, __LINE__);
		if (retake == TXN_VACUUM_TESTS_RETAKE_RUNNING)
			txn_vacuum_tests_committed_at(io, retaker, 130);
	}
	txn_vacuum_tests_made_drop(&made);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 320's no-ender case: a free whose tuple id's row a fold already
 * dropped stores no ender, and its deferred take-out on a lagging standby
 * takes out nothing, in field 5 or as a skipped index pass's dead run in
 * field 6, so a retake of the tuple id on the same key stays, left running,
 * committed, or aborted until a fold drops it, as on its primary.
 */
void
test_txn_vacuum_tests_a_frees_take_out_with_no_ender_leaves_any_retake_on_its_key(void)
{
	for (int retake = TXN_VACUUM_TESTS_RETAKE_RUNNING; retake <= TXN_VACUUM_TESTS_RETAKE_ABORTED; retake++)
	{
		txn_vacuum_tests_unended(retake, false);
		txn_vacuum_tests_unended(retake, true);
	}
}

/*
 * Bugs row 320's subtransaction case: a row deleted in a subtransaction
 * that SAVEPOINT began and RELEASE ended has its subtransaction's xid for
 * the ender its primary's free finds after a fold, and the free's deferred
 * take-out on a lagging standby, which holds the same changes unfolded,
 * takes out the freed incarnation's version by it and leaves a retake on
 * the same key.
 */
void
test_txn_vacuum_tests_a_frees_take_out_takes_out_a_subtransactions_delete(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_vacuum_tests_made_t primary;
	txn_vacuum_tests_made_t standby;
	TreesStoreReader primary_reader;
	TreesStoreReader standby_reader;
	TreesStoreNodeStore primary_store;
	TreesStoreNodeStore standby_store;
	HostXid		writer = simc_begin();
	HostXid		deleter;
	HostXid		released;
	HostXid		retaker;
	uint64		tid;
	uint64		ender;
	StdVec		found;
	StdVec		holders;

	txn_vacuum_tests_made(io, 9475, __LINE__, 2, __LINE__, 2, __LINE__, 2, &primary);
	txn_vacuum_tests_made(io, 9476, __LINE__, 2, __LINE__, 2, __LINE__, 2, &standby);
	primary_reader = repo_Repo_reader(&primary.repo, HOST_CACHE_PLACEMENT_SERVER, &primary.reading);
	standby_reader = repo_Repo_reader(&standby.repo, HOST_CACHE_PLACEMENT_SERVER, &standby.reading);
	primary_store = TREES_STORE_READER_NODE_STORE(&primary_reader);
	standby_store = TREES_STORE_READER_NODE_STORE(&standby_reader);
	tid = txn_vacuum_tests_inserted(io, primary_store, &primary.held, 9475, 1, 100, writer);
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_inserted(io, standby_store, &standby.held, 9476, 1, 100, writer),
					  tid);
	txn_vacuum_tests_committed_at(io, writer, 100);
	deleter = simc_begin();
	released = simc_begin_under(deleter);
	txn_vacuum_tests_deleted(io, &primary.held, 1, released);
	txn_vacuum_tests_deleted(io, &standby.held, 1, released);
	txn_vacuum_tests_committed_status(io, released, 110, __LINE__, 5);
	simc_commit(released);
	txn_vacuum_tests_committed_at(io, deleter, 110);
	txn_vacuum_tests_folded_at(io, &primary, 120);
	found = txn_vacuum_tests_found_every(io, primary_store, &primary.held, __LINE__, 9);
	STD_ASSERT_EQ_U64(__LINE__, 5, found.len, 1);
	STD_ASSERT_EQ_U64(__LINE__, 5, STD_VEC_AT(&found, TxnVacuumFreed, 0).f0, tid);
	ender = STD_VEC_AT(&found, TxnVacuumFreed, 0).f4;
	STD_ASSERT_EQ_U64(__LINE__, 5, ender, released.f0);
	txn_vacuum_tests_found_free(&found);
	{
		TxnVacuumFreed freeing = {tid, 100, ender, .f4 = ender};
		StdVec		claimed;

		std_vec_new(&freeing.f3, sizeof(uint32), _Alignof(uint32));
		claimed = txn_vacuum_tests_claimed(io, standby_store, &standby.held, 9476, &freeing, 1, false, __LINE__, 12);
		std_vec_free_Checked(&claimed, sizeof(uint64), _Alignof(uint64));
	}
	retaker = simc_begin();
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_inserted(io, standby_store, &standby.held, 9476, 1, 100, retaker),
					  tid);
	txn_vacuum_tests_committed_at(io, retaker, 130);
	holders = txn_vacuum_tests_holders(io, &standby.held, tid);
	STD_ASSERT_EQ_U64(__LINE__, 5, holders.len, 2);
	std_vec_free_Checked(&holders, sizeof(uint64), _Alignof(uint64));
	txn_vacuum_tests_taken_out_as(io, &standby.held, 9476, tid, ender, false);
	txn_vacuum_tests_holder(io, &standby.held, tid, retaker, __LINE__);
	txn_vacuum_tests_made_drop(&standby);
	txn_vacuum_tests_made_drop(&primary);
	simc_TestIo_drop(&test_io);
}

/* txn_vacuum_tests_enders_body returns a free's body of two tuple ids in field 5 and one dead in field 6, with the enders given. */
static StdVec
txn_vacuum_tests_enders_body(const uint64 *enders, size_t enders_len)
{
	static const std_TupleU32U16U16U64_t runs[] = {{0, 1, 2, 200}};
	static const std_TupleU32U16U16U64_t dead[] = {{0, 5, 1, 100}};
	TxnRecordTidsFreed freed = {.storage = txn_insert_tests_stored(9477), .horizon = 9,
		.runs = txn_vacuum_runs_view(runs, lengthof(runs)), .dead = txn_vacuum_runs_view(dead, lengthof(dead)),
	.enders = {(void *) enders, enders_len, enders_len}};
	StdVec		body;

	txn_record_TidsFreed_encode(&freed, &body);
	return body;
}

/* txn_vacuum_tests_rewritten returns a free's body written again field by field with field 10 replaced by the bytes given, or left out for none. */
static StdVec
txn_vacuum_tests_rewritten(const StdVec *body, const uint8 *enders, size_t enders_len)
{
	ChunksRecordRecord record;
	ChunksRecordRecordWriter writer = chunks_record_RecordWriter_new();
	ChunksErrorError failed;
	StdVec		rewritten;

	if (!chunks_record_Record_value(body->ptr, body->len, NULL, 0, &record, &failed))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	for (uint32 number = 1; number <= 4; number++)
	{
		uint64		value;

		if (!chunks_record_Record_uint(&record, number, &value, &failed))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		chunks_record_RecordWriter_uint(&writer, number, value);
	}
	for (uint32 number = 5; number <= 6; number++)
	{
		size_t		len;
		const uint8 *bytes = chunks_record_Record_bytes(&record, number, &len);

		if (bytes != NULL)
			chunks_record_RecordWriter_field(&writer, number, bytes, len);
	}
	if (enders != NULL)
		chunks_record_RecordWriter_field(&writer, 10, enders, enders_len);
	rewritten = chunks_record_RecordWriter_finish(&writer);
	chunks_record_RecordWriter_drop(&writer);
	chunks_record_Record_drop(&record);
	return rewritten;
}

/* txn_vacuum_tests_decoded decodes a free's body, checking that it reads back as the free given, or with none given, that it is refused as data corruption. */
static void
txn_vacuum_tests_decoded(StdVec body, const TxnRecordTidsFreed *want, uint32 line)
{
	TxnRecordTidsFreed decoded;
	HostError	error;
	bool		read = txn_record_TidsFreed_decode(body.ptr, body.len, TXN_VACUUM_TESTS_PAGE.most,
												   TXN_VACUUM_TESTS_PAGE.bytes, &decoded, &error);

	if (want != NULL)
	{
		if (!read)
			TXN_VACUUM_TESTS_UNWRAP_FAILED(line, 9, &error);
		STD_ASSERT(line, 5, txn_record_TidsFreed_PartialEq_eq(&decoded, want), "the free reads back");
		txn_record_TidsFreed_drop(&decoded);
	}
	else
	{
		STD_ASSERT(line, 5, !read, "the body is refused");
		STD_ASSERT(line, 5, memcmp(host_Error_sqlstate(&error), "XX001", 5) == 0, "as data corruption");
		host_Error_drop(&error);
	}
	std_vec_free_Checked(&body, 1, 1);
}

/*
 * Bugs row 320's field 10: a free's enders, one for each tuple id of field
 * 5 then of field 6, read back with an ender of 0 in either field, and a
 * body written again field by field reads back the same; fewer or more
 * enders than tuple ids, field 10 left out of a free with runs or written
 * into one with none, and a cut varint are each refused as data corruption.
 */
void
test_txn_vacuum_tests_a_frees_enders_of_another_form_are_refused_as_corruption(void)
{
	static const std_TupleU32U16U16U64_t runs[] = {{0, 1, 2, 200}};
	static const std_TupleU32U16U16U64_t dead[] = {{0, 5, 1, 100}};
	static const uint64 enders[] = {7, 0, 0};
	static const uint64 more[] = {7, 0, 0, 4};
	static const uint8 one[] = {7};
	static const uint8 cut[] = {7, 0, 0x89};
	TxnRecordTidsFreed want = {.storage = txn_insert_tests_stored(9477), .horizon = 9,
		.runs = txn_vacuum_runs_view(runs, lengthof(runs)), .dead = txn_vacuum_runs_view(dead, lengthof(dead)),
	.enders = {(void *) enders, lengthof(enders), lengthof(enders)}};
	TxnRecordTidsFreed none = {.storage = txn_insert_tests_stored(9477), .horizon = 9,
		.runs = txn_vacuum_runs_view(NULL, 0), .dead = txn_vacuum_runs_view(NULL, 0),
	.enders = {NULL, 0, 0}};
	StdVec		body = txn_vacuum_tests_enders_body(enders, lengthof(enders));
	StdVec		empty;
	StdVec		own;
	const uint8 *written;
	size_t		written_len;

	{
		ChunksRecordRecord record;
		ChunksErrorError failed;

		if (!chunks_record_Record_value(body.ptr, body.len, NULL, 0, &record, &failed))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		written = chunks_record_Record_bytes(&record, 10, &written_len);
		STD_ASSERT(__LINE__, 5, written != NULL, "field 10 is written");
		own = txn_vacuum_tests_rewritten(&body, written, written_len);
		chunks_record_Record_drop(&record);
	}
	txn_vacuum_tests_decoded(own, &want, __LINE__);
	txn_vacuum_tests_decoded(txn_vacuum_tests_rewritten(&body, NULL, 0), NULL, __LINE__);
	txn_vacuum_tests_decoded(txn_vacuum_tests_rewritten(&body, cut, lengthof(cut)), NULL, __LINE__);
	txn_vacuum_tests_decoded(body, &want, __LINE__);
	txn_vacuum_tests_decoded(txn_vacuum_tests_enders_body(enders, 2), NULL, __LINE__);
	txn_vacuum_tests_decoded(txn_vacuum_tests_enders_body(more, lengthof(more)), NULL, __LINE__);
	txn_record_TidsFreed_encode(&none, &empty);
	txn_vacuum_tests_decoded(txn_vacuum_tests_rewritten(&empty, NULL, 0), &none, __LINE__);
	txn_vacuum_tests_decoded(txn_vacuum_tests_rewritten(&empty, one, lengthof(one)), NULL, __LINE__);
	std_vec_free_Checked(&empty, 1, 1);
}

/* txn_vacuum_tests_pruned_block prunes block 0 of a case's storage past every xid, as a take on it does when its free bytes are below the reserve. */
static void
txn_vacuum_tests_pruned_block(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 database)
{
	SimcTaken	horizon = txn_vacuum_tests_every();
	SimcTaken	any = simc_snapshot(HOST_SNAPSHOT_KIND_ANY, false);
	HostSnapshot horizon_lent = simc_Taken_lent(&horizon);
	HostSnapshot any_lent = simc_Taken_lent(&any);
	TxnVacuumPrune prune = {.horizon = &horizon_lent, .any = &any_lent};
	HostError	error;

	if (!txn_vacuum_pruned(io, store, held, txn_insert_tests_stored(database), true, &TXN_VACUUM_TESTS_PAGE, 0,
						   TXN_VACUUM_TESTS_PAGE.bytes, &prune, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	simc_Taken_drop(&any);
	simc_Taken_drop(&horizon);
}

/*
 * Bugs row 320's prune case: a row an aborted writer inserted and deleted
 * itself is claimed dead, before the abort's cleanup takes it out, by a
 * prune whose record names its ender with OWN; a lagging standby holding the
 * same changes marks it dead at its own record's LSN, frees it with the
 * ender its primary's VACUUM finds, and takes it again on the same key, and
 * both records' deferred take-outs, the prune's and the free's, take out
 * only the aborted writer's version by its writer and leave the retake.
 */
void
test_txn_vacuum_tests_a_prunes_take_out_of_an_aborted_writers_own_row_leaves_a_retake(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_vacuum_tests_made_t primary;
	txn_vacuum_tests_made_t standby;
	TreesStoreReader primary_reader;
	TreesStoreReader standby_reader;
	TreesStoreNodeStore primary_store;
	TreesStoreNodeStore standby_store;
	HostXid		aborted = simc_begin();
	HostXid		retaker = simc_begin();
	const TxnRecordTidsFreed *pruned = NULL;
	uint64		tid;
	StdVec		records;
	StdVec		found;
	StdVec		marked;
	HostLsn		marking_end = 0;
	TxnVacuumFreed freeing;
	HostError	error;

	txn_vacuum_tests_made(io, 9478, __LINE__, 2, __LINE__, 2, __LINE__, 2, &primary);
	txn_vacuum_tests_made(io, 9479, __LINE__, 2, __LINE__, 2, __LINE__, 2, &standby);
	primary_reader = repo_Repo_reader(&primary.repo, HOST_CACHE_PLACEMENT_SERVER, &primary.reading);
	standby_reader = repo_Repo_reader(&standby.repo, HOST_CACHE_PLACEMENT_SERVER, &standby.reading);
	primary_store = TREES_STORE_READER_NODE_STORE(&primary_reader);
	standby_store = TREES_STORE_READER_NODE_STORE(&standby_reader);
	tid = txn_vacuum_tests_inserted(io, primary_store, &primary.held, 9478, 1, 100, aborted);
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_inserted(io, standby_store, &standby.held, 9479, 1, 100, aborted),
					  tid);
	txn_vacuum_tests_deleted(io, &primary.held, 1, aborted);
	txn_vacuum_tests_deleted(io, &standby.held, 1, aborted);
	simc_abort(aborted);
	txn_vacuum_tests_pruned_block(io, primary_store, &primary.held, 9478);
	records = txn_vacuum_tests_logged(9478);
	for (size_t k = 0; k < records.len; k++)
		if (STD_VEC_AT(&records, txn_vacuum_tests_logged_t, k).freed)
			pruned = &STD_VEC_AT(&records, txn_vacuum_tests_logged_t, k).free;
	STD_ASSERT(__LINE__, 5, pruned != NULL, "the prune logs its free");
	STD_ASSERT_EQ_U64(__LINE__, 5, pruned->runs.len, 0);
	STD_ASSERT_EQ_U64(__LINE__, 5, pruned->dead.len, 1);
	STD_ASSERT_EQ_U64(__LINE__, 5, pruned->enders.len, 1);
	STD_ASSERT_EQ_U64(__LINE__, 5, STD_VEC_AT(&pruned->enders, uint64, 0), aborted.f0 | TXN_BASE_OWN);
	found = txn_vacuum_tests_found_every(io, primary_store, &primary.held, __LINE__, 9);
	STD_ASSERT_EQ_U64(__LINE__, 5, found.len, 1);
	freeing = STD_VEC_AT(&found, TxnVacuumFreed, 0);
	STD_ASSERT_EQ_U64(__LINE__, 5, freeing.f0, tid);
	{
		TxnVacuumFreed marking = {tid, 100, 0, .f4 = aborted.f0 | TXN_BASE_OWN};
		TxnVacuumFreed freed = {tid, freeing.f1, freeing.f2, .f4 = freeing.f4};
		StdVec		claimed;

		std_vec_new(&marking.f3, sizeof(uint32), _Alignof(uint32));
		std_vec_new(&freed.f3, sizeof(uint32), _Alignof(uint32));
		claimed = txn_vacuum_tests_claimed(io, standby_store, &standby.held, 9479, &marking, 1, true, __LINE__, 12);
		std_vec_free_Checked(&claimed, sizeof(uint64), _Alignof(uint64));
		marked = txn_vacuum_tests_logged(9479);
		for (size_t k = 0; k < marked.len; k++)
			if (STD_VEC_AT(&marked, txn_vacuum_tests_logged_t, k).freed)
				marking_end = STD_VEC_AT(&marked, txn_vacuum_tests_logged_t, k).end;
		STD_ASSERT(__LINE__, 5, marking_end != 0, "the marking logs its free");
		claimed = txn_vacuum_tests_claimed(io, standby_store, &standby.held, 9479, &freed, 1, false, __LINE__, 12);
		std_vec_free_Checked(&claimed, sizeof(uint64), _Alignof(uint64));
	}
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_inserted(io, standby_store, &standby.held, 9479, 1, 100, retaker),
					  tid);
	txn_vacuum_tests_committed_at(io, retaker, 130);
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_versions_len(io, &standby.held, 1, __LINE__, 9, __LINE__, 9), 3);
	{
		TxnRecordTidsFreed taken = *pruned;

		taken.storage = txn_insert_tests_stored(9479);
		if (!txn_vacuum_taken_out(io, &standby.held, &taken, marking_end, &error))
			TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	}
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_versions_len(io, &standby.held, 1, __LINE__, 9, __LINE__, 9), 2);
	txn_vacuum_tests_holder(io, &standby.held, tid, retaker, __LINE__);
	txn_vacuum_tests_taken_out_as(io, &standby.held, 9479, tid, freeing.f4, false);
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_versions_len(io, &standby.held, 1, __LINE__, 9, __LINE__, 9), 2);
	txn_vacuum_tests_holder(io, &standby.held, tid, retaker, __LINE__);
	if (!txn_written_aborted(io, 0, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	txn_vacuum_tests_found_free(&found);
	txn_vacuum_tests_logged_free(&marked);
	txn_vacuum_tests_logged_free(&records);
	txn_vacuum_tests_made_drop(&standby);
	txn_vacuum_tests_made_drop(&primary);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 320's aborted insert: a prune over a block whose only row an
 * aborted writer inserted, unfolded and before the abort's cleanup takes it
 * out, claims nothing, since only a changer's end makes a tuple id dead, and
 * logs no free.
 */
void
test_txn_vacuum_tests_a_prune_over_an_aborted_unfolded_insert_claims_nothing(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_vacuum_tests_made_t made;
	TreesStoreReader reader;
	TreesStoreNodeStore store;
	HostXid		aborted = simc_begin();
	StdVec		records;
	HostError	error;

	txn_vacuum_tests_made(io, 9480, __LINE__, 2, __LINE__, 2, __LINE__, 2, &made);
	reader = repo_Repo_reader(&made.repo, HOST_CACHE_PLACEMENT_SERVER, &made.reading);
	store = TREES_STORE_READER_NODE_STORE(&reader);
	(void) txn_vacuum_tests_inserted(io, store, &made.held, 9480, 1, 100, aborted);
	simc_abort(aborted);
	txn_vacuum_tests_pruned_block(io, store, &made.held, 9480);
	records = txn_vacuum_tests_logged(9480);
	for (size_t k = 0; k < records.len; k++)
		STD_ASSERT(__LINE__, 5, !STD_VEC_AT(&records, txn_vacuum_tests_logged_t, k).freed, "the prune logs no free");
	STD_ASSERT_EQ_U64(__LINE__, 5, txn_vacuum_tests_versions_len(io, &made.held, 1, __LINE__, 9, __LINE__, 9), 1);
	if (!txn_written_aborted(io, 0, &error))
		TXN_VACUUM_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	txn_vacuum_tests_logged_free(&records);
	txn_vacuum_tests_made_drop(&made);
	simc_TestIo_drop(&test_io);
}

#endif
