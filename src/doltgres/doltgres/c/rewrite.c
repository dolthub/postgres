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
 * The copy of a table CLUSTER and VACUUM FULL make (doltgres' rewrite.rs):
 * every row version of the old storage judged, ordered, and given its new
 * tuple id as heap's rewrite gives its tuples theirs; and the versions ALTER
 * TABLE SET TABLESPACE's move carries as they are.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/doltgres/changes.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/layout.h"
#include "doltgres/doltgres/rewrite.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/counted.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/outcome.h"
#include "doltgres/txn/status.h"
#include "doltgres/txn/values.h"
#include "pg_calls.h"

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "rewrite.rs"

/* The element layouts of the rewrite's vectors. */
#define DOLTGRES_REWRITE_BYTES 1, 1
#define DOLTGRES_REWRITE_BASED sizeof(TxnRewriteBased), _Alignof(TxnRewriteBased)
#define DOLTGRES_REWRITE_KEY sizeof(DoltgresRewriteKey), _Alignof(DoltgresRewriteKey)
#define DOLTGRES_REWRITE_OLD sizeof(DoltgresRewriteOld), _Alignof(DoltgresRewriteOld)
#define DOLTGRES_REWRITE_RECORD sizeof(doltgres_TupleVecU8Bool_t), _Alignof(doltgres_TupleVecU8Bool_t)

/* doltgres_rewrite_Based_drop is Based's drop glue, an element of Read's based. */
static void
doltgres_rewrite_Based_drop(void *based)
{
	txn_rewrite_Based_drop(based);
}

/* doltgres_rewrite_Record_drop is a (Vec<u8>, bool)'s drop glue, an element of Key's versions. */
static void
doltgres_rewrite_Record_drop(void *record)
{
	std_vec_free_Checked(&((doltgres_TupleVecU8Bool_t *) record)->f0, DOLTGRES_REWRITE_BYTES);
}

/* doltgres_rewrite_Key_drop is Key's drop glue: its bytes and its versions' records. */
static void
doltgres_rewrite_Key_drop(void *key)
{
	DoltgresRewriteKey *self = key;

	std_vec_free_Checked(&self->key, DOLTGRES_REWRITE_BYTES);
	std_vec_drop_Checked(&self->versions, DOLTGRES_REWRITE_RECORD, doltgres_rewrite_Record_drop);
}

/* doltgres_rewrite_Read_drop is Read's drop glue: its base, its base's entries, its keys, and its row versions. */
void
doltgres_rewrite_Read_drop(DoltgresRewriteRead *self)
{
	txn_base_Base_drop(&self->base);
	std_vec_drop_Checked(&self->based, DOLTGRES_REWRITE_BASED, doltgres_rewrite_Based_drop);
	std_vec_drop_Checked(&self->keys, DOLTGRES_REWRITE_KEY, doltgres_rewrite_Key_drop);
	std_vec_free_Checked(&self->olds, DOLTGRES_REWRITE_OLD);
}

/* doltgres_rewrite_fork returns a fork error of a literal message. */
static HostError
doltgres_rewrite_fork(const char *sqlstate, const char *message)
{
	StdString text;

	std_string_from_Checked(&text, message, strlen(message));
	return host_Error_fork((const uint8 *) sqlstate, &text);
}

/*
 * doltgres_rewrite_judged returns a row version judged by heap's
 * HeapTupleSatisfiesVacuum from its writer's outcome and its changer among
 * its key's versions, which is its ender unless it is a lock or its writer
 * aborted, with its ender's place and the update its ender made when that
 * moved the row to another of the table's rows.
 */
static bool
doltgres_rewrite_judged(HostIoIo *io, size_t key, uint64 tuple, uint64 xmin, DoltgresRewriteFrom from,
						HostStatus wrote, TxnMemtableVersionVersion *changer, TxnMemtableVersionVersion *versions,
						size_t versions_len, uint64 horizon, DoltgresRewriteOld *result, HostError *error)
{
	TxnMemtableVersionVersion *ender = NULL;

	if (changer != NULL)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(changer);
		TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);

		if (op.tag != TXN_MEMTABLE_VERSION_OP_REF_LOCK && op.tag != TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW)
		{
			HostStatus	outcome;

			if (!txn_outcome_outcome(io, changer, &outcome, error))
				return false;
			if (outcome != HOST_STATUS_ABORTED)
				ender = changer;
		}
	}
	result->key = key;
	result->tuple = tuple;
	result->xmin = xmin;
	result->from = from;
	if (!txn_visible_vacuumed(io, wrote, ender, horizon, &result->vacuumed, error))
		return false;
	result->has_update = false;
	result->has_ender = false;
	if (ender != NULL)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(ender);
		uint64		next = doltgres_changes_successor(ender, tuple);

		if (next != tuple && next != DOLTGRES_CHANGES_MOVED_PARTITIONS)
		{
			result->has_update = true;
			result->update_xmax = txn_memtable_version_Record_writer(&record).xid;
			result->update_next = next;
		}
		for (size_t i = 0; i < versions_len && !result->has_ender; i++)
		{
			if (&versions[i] == ender)
			{
				result->has_ender = true;
				result->ender = i;
			}
		}
	}
	return true;
}

/*
 * doltgres_rewrite_read_captures_t is read's each's captures: the base's
 * entries, the keys and row versions so far, and the horizon.
 */
typedef struct doltgres_rewrite_read_captures_t
{
	const StdVec *based;
	StdVec	   *keys;
	StdVec	   *olds;
	uint64		horizon;
} doltgres_rewrite_read_captures_t;

/*
 * doltgres_rewrite_read_closure_0_frame_t is read's each's state across its
 * pad: the key's versions as seen and its records so far, each while live.
 */
typedef struct doltgres_rewrite_read_closure_0_frame_t
{
	TxnVisibleSeen seen;
	bool		seen_live;
	DoltgresRewriteKey key;
	bool		key_live;
	doltgres_rewrite_read_captures_t *c;
	HostIoIo   *io;
	const uint8 *bytes;
	size_t		bytes_len;
	bool		has_entry;
	size_t		entry;
	StdVec	   *versions;
	HostError  *error;
	bool		ok;
} doltgres_rewrite_read_closure_0_frame_t;

/* doltgres_rewrite_read_closure_0_pad frees what read's each holds. */
static void
doltgres_rewrite_read_closure_0_pad(void *frame_)
{
	doltgres_rewrite_read_closure_0_frame_t *frame = frame_;

	if (frame->key_live)
		doltgres_rewrite_Key_drop(&frame->key);
	if (frame->seen_live)
		txn_visible_Seen_drop(&frame->seen);
	frame->key_live = frame->seen_live = false;
}

/* doltgres_rewrite_read_closure_0_body judges each row version of the key and keeps its versions' records. */
static void
doltgres_rewrite_read_closure_0_body(void *frame_)
{
	doltgres_rewrite_read_closure_0_frame_t *frame = frame_;
	doltgres_rewrite_read_captures_t *c = frame->c;
	TxnMemtableVersionVersion *versions = frame->versions->ptr;
	size_t		len = frame->versions->len;
	size_t		at = c->keys->len;
	HostSnapshot any = pghost_any();
	TxnVisibleRule rule = {&TXN_VISIBLE_SNAPSHOT_RULE_OPS, &any};

	if (!txn_visible_seen(frame->io, rule, versions, len, NULL, &frame->seen, frame->error))
		return;
	frame->seen_live = true;
	if (frame->has_entry)
	{
		const TxnRewriteBased *based = &STD_VEC_AT(c->based, TxnRewriteBased, frame->entry);

		for (size_t i = 0; i < based->stamps.len; i++)
		{
			const ChunksTrailerStamp *stamp = &STD_VEC_AT(&based->stamps, ChunksTrailerStamp, i);
			DoltgresRewriteFrom from = {DOLTGRES_REWRITE_FROM_BASE, frame->entry};
			HostRow		row = {stamp->tuple};
			TxnVisibleDirty dirty;
			TxnMemtableVersionVersion *changer;
			TxnMemtableVersionVersion *commander;
			DoltgresRewriteOld old;
			bool		visible;

			if (!txn_visible_Seen_base(&frame->seen, frame->io, rule, row, &visible, &dirty, &changer, &commander,
									   frame->error))
				return;
			if (!doltgres_rewrite_judged(frame->io, at, stamp->tuple, stamp->txn, from, HOST_STATUS_COMMITTED,
										 changer, versions, len, c->horizon, &old, frame->error))
				return;
			std_vec_push_Checked(c->olds, &old, DOLTGRES_REWRITE_OLD);
		}
	}
	for (size_t i = 0; i < frame->seen.rows.len; i++)
	{
		const TxnVisibleSeenRow *seen = &STD_VEC_AT(&frame->seen.rows, TxnVisibleSeenRow, i);
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(seen->row.version);
		DoltgresRewriteFrom from = {DOLTGRES_REWRITE_FROM_DELTA};
		DoltgresRewriteOld old;
		HostStatus	wrote;
		bool		found = false;

		for (size_t k = 0; k < len && !found; k++)
		{
			if (&versions[k] == seen->row.version)
			{
				found = true;
				from.place = k;
			}
		}
		if (!found)
		{
			*frame->error = doltgres_rewrite_fork("XX000", "a rewrite's row is none of its key's versions");
			return;
		}
		if (!txn_outcome_outcome(frame->io, seen->row.version, &wrote, frame->error))
			return;
		if (!doltgres_rewrite_judged(frame->io, at, seen->row.put.tuple.f0,
									 txn_memtable_version_Record_writer(&record).xid, from, wrote, seen->changer,
									 versions, len, c->horizon, &old, frame->error))
			return;
		std_vec_push_Checked(c->olds, &old, DOLTGRES_REWRITE_OLD);
	}
	std_slice_to_vec_Checked(&frame->key.key, frame->bytes, frame->bytes_len, DOLTGRES_REWRITE_BYTES);
	std_vec_with_capacity_Checked(&frame->key.versions, len, DOLTGRES_REWRITE_RECORD);
	frame->key_live = true;
	for (size_t i = 0; i < len; i++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&versions[i]);
		StdBytes	bytes = txn_memtable_version_Record_bytes(&record);
		doltgres_TupleVecU8Bool_t kept;
		HostStatus	outcome;

		if (!txn_outcome_outcome(frame->io, &versions[i], &outcome, frame->error))
			return;
		std_slice_to_vec_Checked(&kept.f0, bytes.ptr, bytes.len, DOLTGRES_REWRITE_BYTES);
		kept.f1 = outcome == HOST_STATUS_ABORTED;
		std_vec_push_Checked(&frame->key.versions, &kept, DOLTGRES_REWRITE_RECORD);
	}
	std_vec_push_Checked(c->keys, &frame->key, DOLTGRES_REWRITE_KEY);
	frame->key_live = false;
	frame->ok = true;
}

/* doltgres_rewrite_read_closure_0 is read's each: a key's row versions judged and its versions' records kept. */
static bool
doltgres_rewrite_read_closure_0(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, bool has_place,
								size_t place, StdVec *versions, HostError *error)
{
	doltgres_rewrite_read_closure_0_frame_t frame = {.c = captures, .io = io, .bytes = key, .bytes_len = key_len,
	.has_entry = has_place, .entry = place, .versions = versions, .error = error};

	std_landing(&frame, doltgres_rewrite_read_closure_0_body, doltgres_rewrite_read_closure_0_pad);
	doltgres_rewrite_read_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_rewrite_read_frame_t is read's state across its pad: the read so
 * far, its base taken in, while live.
 */
typedef struct doltgres_rewrite_read_frame_t
{
	DoltgresRewriteRead read;
	bool		base_live;
	bool		based_live;
	bool		rest_live;
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	uint64		horizon;
	DoltgresRewriteRead *result;
	HostError  *error;
	bool		ok;
} doltgres_rewrite_read_frame_t;

/* doltgres_rewrite_read_pad frees what read holds. */
static void
doltgres_rewrite_read_pad(void *frame_)
{
	doltgres_rewrite_read_frame_t *frame = frame_;

	if (frame->rest_live)
	{
		std_vec_drop_Checked(&frame->read.keys, DOLTGRES_REWRITE_KEY, doltgres_rewrite_Key_drop);
		std_vec_free_Checked(&frame->read.olds, DOLTGRES_REWRITE_OLD);
	}
	if (frame->based_live)
		std_vec_drop_Checked(&frame->read.based, DOLTGRES_REWRITE_BASED, doltgres_rewrite_Based_drop);
	if (frame->base_live)
		txn_base_Base_drop(&frame->read.base);
	frame->rest_live = frame->based_live = frame->base_live = false;
}

/* doltgres_rewrite_read_body reads the base's entries, then each key's row versions. */
static void
doltgres_rewrite_read_body(void *frame_)
{
	doltgres_rewrite_read_frame_t *frame = frame_;
	DoltgresRewriteRead *read = &frame->read;
	doltgres_rewrite_read_captures_t captures = {.based = &read->based, .keys = &read->keys, .olds = &read->olds,
	.horizon = frame->horizon};

	if (!txn_rewrite_based(frame->io, frame->store, read->base.has_root, read->base.root, &read->based,
						   frame->error))
		return;
	frame->based_live = true;
	std_vec_new(&read->keys, DOLTGRES_REWRITE_KEY);
	std_vec_new(&read->olds, DOLTGRES_REWRITE_OLD);
	frame->rest_live = true;
	read->left_out = read->base.max_ended >= frame->horizon ? read->base.max_ended : 0;
	if (!txn_rewrite_keys(frame->io, frame->held, read->based.ptr, read->based.len, read->base.folded,
						  doltgres_rewrite_read_closure_0, &captures, NULL, frame->error))
		return;
	*frame->result = *read;
	frame->rest_live = frame->based_live = frame->base_live = false;
	frame->ok = true;
}

/*
 * doltgres_rewrite_read reads every row version of a base, which it takes,
 * and of the delta's versions it doesn't apply, each judged by heap's
 * HeapTupleSatisfiesVacuum against a removable horizon, and the horizon of
 * the rows it leaves out: a base entry's copies, whose writers committed,
 * and the delta's rows, each with its ender, the nearest version above it
 * that ends it, as a read under SnapshotAny finds it.
 */
bool
doltgres_rewrite_read(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, TxnBaseBase *base,
					  uint64 horizon, DoltgresRewriteRead *result, HostError *error)
{
	doltgres_rewrite_read_frame_t frame = {.read = {.base = *base}, .base_live = true, .io = io, .store = store,
	.held = held, .horizon = horizon, .result = result, .error = error};

	std_landing(&frame, doltgres_rewrite_read_body, doltgres_rewrite_read_pad);
	doltgres_rewrite_read_pad(&frame);
	return frame.ok;
}


/* More element layouts of the rewrite's vectors. */
#define DOLTGRES_REWRITE_NEW sizeof(doltgres_OptionU64_t), _Alignof(doltgres_OptionU64_t)
#define DOLTGRES_REWRITE_U32 sizeof(uint32), _Alignof(uint32)
#define DOLTGRES_REWRITE_FORMED sizeof(doltgres_OptionFormed_t), _Alignof(doltgres_OptionFormed_t)
#define DOLTGRES_REWRITE_IDS sizeof(doltgres_OptionVecU32_t), _Alignof(doltgres_OptionVecU32_t)
#define DOLTGRES_REWRITE_VALUE sizeof(txn_TupleU32TupleEntryU64_t), _Alignof(txn_TupleU32TupleEntryU64_t)
#define DOLTGRES_REWRITE_PAIR sizeof(doltgres_TupleU64U64_t), _Alignof(doltgres_TupleU64U64_t)
#define DOLTGRES_REWRITE_USIZE sizeof(size_t), _Alignof(size_t)

/* doltgres_TupleU64U64_t is a (u64, u64): an updater's xid and the tuple id it updated a row to. */
typedef struct doltgres_TupleU64U64_t
{
	uint64		f0;
	uint64		f1;
} doltgres_TupleU64U64_t;

/* doltgres_rewrite_Formed_drop is an Option<Formed>'s drop glue, an element of Rewritten's formed. */
static void
doltgres_rewrite_Formed_drop(void *formed)
{
	doltgres_OptionFormed_t *self = formed;

	if (self->has)
		access_heaptuple_Formed_drop(&self->f0);
}

/* doltgres_rewrite_Ids_drop is an Option<Vec<u32>>'s drop glue, an element of Rewritten's ids. */
static void
doltgres_rewrite_Ids_drop(void *ids)
{
	doltgres_OptionVecU32_t *self = ids;

	if (self->has)
		std_vec_free_Checked(&self->f0, DOLTGRES_REWRITE_U32);
}

/*
 * doltgres_rewrite_Rewritten_drop is Rewritten's drop glue: its new tuple ids,
 * bytes, rows re-formed, ids, and values.
 */
void
doltgres_rewrite_Rewritten_drop(DoltgresRewriteRewritten *self)
{
	std_vec_free_Checked(&self->new_, DOLTGRES_REWRITE_NEW);
	std_vec_free_Checked(&self->bytes, DOLTGRES_REWRITE_U32);
	std_vec_drop_Checked(&self->formed, DOLTGRES_REWRITE_FORMED, doltgres_rewrite_Formed_drop);
	std_vec_drop_Checked(&self->ids, DOLTGRES_REWRITE_IDS, doltgres_rewrite_Ids_drop);
	std_vec_free_Checked(&self->values, DOLTGRES_REWRITE_VALUE);
}

/*
 * DoltgresRewriteRewriting is Rewriting: heap's rewrite of the old storage's
 * row versions in progress, the rows' re-former, Postgres' own state, the
 * storage as read, the horizon, the updated rows written while the row they
 * were updated from could still be held (heap's rs_old_new_tid_map, a
 * sorted Vec as the port's internals may hold a BTreeSet), the packing's
 * reserve, page, and most tuples, the tuple id it gives next and its page's
 * free bytes, and what it gave the versions so far.
 */
typedef struct DoltgresRewriteRewriting
{
	DoltgresRewriteReformer reformer;
	const PgRewrite *rewrite;
	const DoltgresRewriteRead *read;
	uint64		horizon;
	StdVec		mapped;
	uint64		reserve;
	uint64		page;
	uint16		most;
	bool		has_next;
	uint64		next;
	uint64		free;
	DoltgresRewriteRewritten done;
} DoltgresRewriteRewriting;

static bool doltgres_rewrite_Reformer_store(DoltgresRewriteReformer *self, const PgCatalogCaps *caps, size_t at,
											DoltgresScanRow *result, PgPgError *error);

/* doltgres_rewrite_mapped_at returns where a pair is in Rewriting's sorted mapped pairs, or goes. */
static size_t
doltgres_rewrite_mapped_at(const StdVec *mapped, uint64 xmin, uint64 tuple, bool *found)
{
	size_t		low = 0;
	size_t		high = mapped->len;

	*found = false;
	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		const doltgres_TupleU64U64_t *pair = &STD_VEC_AT(mapped, doltgres_TupleU64U64_t, mid);

		if (pair->f0 == xmin && pair->f1 == tuple)
		{
			*found = true;
			return mid;
		}
		if (pair->f0 < xmin || (pair->f0 == xmin && pair->f1 < tuple))
			low = mid + 1;
		else
			high = mid;
	}
	return low;
}

/*
 * doltgres_rewrite_Rewriting_kept judges a row version in heap's copy loop
 * and counts it: a dead one is dropped, and drops the version held for it,
 * heap's rewrite_heap_dead_tuple; any other is kept.
 */
static bool
doltgres_rewrite_Rewriting_kept(DoltgresRewriteRewriting *self, const PgCatalogCaps *caps, size_t at, bool *result,
								PgPgError *error)
{
	const DoltgresRewriteOld *old = &STD_VEC_AT(&self->read->olds, DoltgresRewriteOld, at);
	DoltgresRewriteCounts *counts = &self->done.counts;

	switch (old->vacuumed)
	{
		case TXN_VISIBLE_VACUUMED_DEAD:
			{
				int64		held;

				counts->vacuumed += 1.0;
				if (!pg_calls_rewrite_take(pg_CatalogCaps_may_raise(caps), self->rewrite, (uint32) old->xmin,
										   old->tuple, &held, error))
					return false;
				if (held >= 0)
				{
					counts->vacuumed += 1.0;
					counts->recently_dead -= 1.0;
				}
				*result = false;
				return true;
			}
		case TXN_VISIBLE_VACUUMED_RECENTLY_DEAD:
		case TXN_VISIBLE_VACUUMED_DELETE_IN_PROGRESS:
			counts->recently_dead += 1.0;
			break;
		case TXN_VISIBLE_VACUUMED_LIVE:
		case TXN_VISIBLE_VACUUMED_INSERT_IN_PROGRESS:
			break;
	}
	counts->kept += 1.0;
	*result = true;
	return true;
}

/*
 * doltgres_rewrite_Rewriting_pack gives a row version its new tuple id,
 * heap's raw_heap_insert: on the current page while it has room for the
 * row's heap bytes as heap re-forms the row, and otherwise at the first
 * offset of the next page. A row whose re-formed key or tuple differs from
 * the one stored keeps the re-formed one.
 */
static bool
doltgres_rewrite_Rewriting_pack(DoltgresRewriteRewriting *self, const PgCatalogCaps *caps, size_t at,
								PgPgError *error)
{
	DoltgresScanRow row;
	AccessHeaptupleFormed formed;
	doltgres_OptionVecU32_t ids;
	doltgres_OptionFormed_t *kept = &STD_VEC_AT(&self->done.formed, doltgres_OptionFormed_t, at);
	doltgres_OptionVecU32_t *place = &STD_VEC_AT(&self->done.ids, doltgres_OptionVecU32_t, at);
	TxnInsertRoom room;
	uint64		bytes;
	uint64		tuple;
	bool		differs;

	if (!doltgres_rewrite_Reformer_reformed(&self->reformer, caps, at, &self->done.values, &row, &formed, &ids.has,
											&ids.f0, error))
		return false;
	if (ids.has)
		*place = ids;
	bytes = formed.heap_bytes;
	differs = std_bytes_Ord_cmp(formed.key.ptr, formed.key.len, row.key.ptr, row.key.len) != STD_ORDERING_EQUAL ||
		std_bytes_Ord_cmp(formed.tuple.ptr, formed.tuple.len, row.tuple.ptr, row.tuple.len) != STD_ORDERING_EQUAL ||
		place->has;
	doltgres_scan_Row_drop(&row);
	if (differs)
	{
		kept->has = true;
		kept->f0 = formed;
	}
	else
		access_heaptuple_Formed_drop(&formed);
	room.bytes = bytes;
	room.reserve = self->reserve;
	room.page = self->page;
	room.most = self->most;
	if (self->has_next && txn_insert_Room_fits(&room, self->free, (uint16) (self->next & 0xffff)))
		tuple = self->next;
	else
	{
		tuple = self->has_next ? ((self->next >> 16) + 1) << 16 | 1 : 1;
		self->free = self->page;
	}
	self->free = STD_SUB(u64, self->free, bytes, 385, 9);
	self->has_next = true;
	self->next = STD_ADD(u64, tuple, 1, 386, 26);
	STD_VEC_AT(&self->done.new_, doltgres_OptionU64_t, at).has = true;
	STD_VEC_AT(&self->done.new_, doltgres_OptionU64_t, at).f0 = tuple;
	STD_VEC_AT(&self->done.bytes, uint32, at) = (uint32) bytes;
	return true;
}

/*
 * doltgres_rewrite_Rewriting_rewrite writes a kept row version as heap's
 * rewrite_heap_tuple does: one an update ended is held until the row it was
 * updated to is written, unless that row was written already; and each
 * version written whose writer isn't below the horizon writes the version
 * held for it right after it, or is remembered for the version that will
 * be. Whether the written version was itself made by an update, which heap
 * asks first, needs no asking: a version is held for a writer's tuple id
 * only by the update that wrote the row there.
 */
static bool
doltgres_rewrite_Rewriting_rewrite(DoltgresRewriteRewriting *self, const PgCatalogCaps *caps, size_t at,
								   PgPgError *error)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(caps);
	const DoltgresRewriteOld *old = &STD_VEC_AT(&self->read->olds, DoltgresRewriteOld, at);

	if (old->has_update)
	{
		bool		found;
		size_t		place = doltgres_rewrite_mapped_at(&self->mapped, old->update_xmax, old->update_next, &found);
		doltgres_TupleU64U64_t removed;

		if (!found)
			return pg_calls_rewrite_hold(raise, self->rewrite, (uint32) old->update_xmax, old->update_next,
										 (uint64) at, error);
		std_vec_remove(&self->mapped, place, &removed, DOLTGRES_REWRITE_PAIR, RUST_FILE, 327, 30);
	}
	for (;;)
	{
		uint64		xmin;
		uint64		tuple;
		int64		held;

		if (!doltgres_rewrite_Rewriting_pack(self, caps, at, error))
			return false;
		xmin = STD_VEC_AT(&self->read->olds, DoltgresRewriteOld, at).xmin;
		tuple = STD_VEC_AT(&self->read->olds, DoltgresRewriteOld, at).tuple;
		if (xmin < self->horizon)
			return true;
		if (!pg_calls_rewrite_take(raise, self->rewrite, (uint32) xmin, tuple, &held, error))
			return false;
		if (held < 0)
		{
			bool		found;
			size_t		place = doltgres_rewrite_mapped_at(&self->mapped, xmin, tuple, &found);
			doltgres_TupleU64U64_t pair = {xmin, tuple};

			if (!found)
				std_vec_insert(&self->mapped, place, &pair, DOLTGRES_REWRITE_PAIR, RUST_FILE, 341, 33);
			return true;
		}
		at = (size_t) held;
	}
}

/* doltgres_rewrite_ended_frame_t is ended's state across its pad: the places of the versions still held, while live. */
typedef struct doltgres_rewrite_ended_frame_t
{
	StdVec		held;
	bool		held_live;
	DoltgresRewriteRewriting *self;
	const PgCatalogCaps *caps;
	PgPgError  *error;
	bool		ok;
} doltgres_rewrite_ended_frame_t;

/* doltgres_rewrite_ended_pad frees what ended holds. */
static void
doltgres_rewrite_ended_pad(void *frame_)
{
	doltgres_rewrite_ended_frame_t *frame = frame_;

	if (frame->held_live)
		std_vec_free_Checked(&frame->held, DOLTGRES_REWRITE_BYTES);
	frame->held_live = false;
}

/* doltgres_rewrite_ended_body reads the versions still held and packs each in heap's order. */
static void
doltgres_rewrite_ended_body(void *frame_)
{
	doltgres_rewrite_ended_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	size_t		count;
	size_t		written;

	if (!pg_calls_rewrite_held(raise, frame->self->rewrite, NULL, 0, &count, frame->error))
		return;
	std_vec_u8_from_elem(&frame->held, 0, STD_MUL(usize, count, 8, 352, 33));
	frame->held_live = true;
	if (!pg_calls_rewrite_held(raise, frame->self->rewrite, frame->held.ptr, frame->held.len, &written,
							   frame->error))
		return;
	for (size_t i = 0; i + 8 <= frame->held.len; i += 8)
	{
		uint64		place;

		memcpy(&place, (const uint8 *) frame->held.ptr + i, sizeof(place));
		if (!doltgres_rewrite_Rewriting_pack(frame->self, frame->caps, (size_t) place, frame->error))
			return;
	}
	frame->ok = true;
}

/* doltgres_rewrite_Rewriting_ended writes the versions still held, in the order heap's end_heap_rewrite walks them. */
static bool
doltgres_rewrite_Rewriting_ended(DoltgresRewriteRewriting *self, const PgCatalogCaps *caps, PgPgError *error)
{
	doltgres_rewrite_ended_frame_t frame = {.self = self, .caps = caps, .error = error};

	std_landing(&frame, doltgres_rewrite_ended_body, doltgres_rewrite_ended_pad);
	doltgres_rewrite_ended_pad(&frame);
	return frame.ok;
}

/* doltgres_rewrite_rewritten_closure_2 is rewritten's sort key: a row version's old tuple id. */
static uint64
doltgres_rewrite_rewritten_closure_2(void *captures, const void *at)
{
	return STD_VEC_AT((const StdVec *) captures, DoltgresRewriteOld, *(const size_t *) at).tuple;
}

/* <[usize]>::sort_by_key at u64, which rewritten orders its row versions by old tuple id with. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS Usize
#define STD_SORT_T size_t
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS Usize
#define STD_STABLE_SORT_T size_t
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY_KEY
#define STD_STABLE_SORT_KEY_KS U64
#define STD_STABLE_SORT_KEY_K uint64
#define STD_STABLE_SORT_KEY_LT(a, b) (*(a) < *(b))
#include "doltgres/std/stable_sort_template.h"

/*
 * doltgres_rewrite_rewritten_frame_t is rewritten's state across its pad: the
 * rewrite in progress, its re-former, its mapped pairs, and what it gave so
 * far, and the row versions by old tuple id, each while live.
 */
typedef struct doltgres_rewrite_rewritten_frame_t
{
	DoltgresRewriteRewriting w;
	bool		done_live;
	bool		reformer_live;
	bool		mapped_live;
	StdVec		by_tuple;
	bool		by_tuple_live;
	PgRewrite	rewrite;
	const PgCatalogCaps *caps;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const DoltgresRewriteTable *table;
	StdVec	   *key_columns;
	const PgRel *new_table;
	const PgRel *index;
	bool		sorted;
	const DoltgresRewriteRead *read;
	uint64		horizon;
	DoltgresRewriteRewritten *result;
	PgPgError  *error;
	bool		ok;
} doltgres_rewrite_rewritten_frame_t;

/* doltgres_rewrite_rewritten_pad frees what rewritten holds. */
static void
doltgres_rewrite_rewritten_pad(void *frame_)
{
	doltgres_rewrite_rewritten_frame_t *frame = frame_;

	if (frame->by_tuple_live)
		std_vec_free_Checked(&frame->by_tuple, DOLTGRES_REWRITE_USIZE);
	if (frame->mapped_live)
		std_vec_free_Checked(&frame->w.mapped, DOLTGRES_REWRITE_PAIR);
	if (frame->reformer_live)
		doltgres_rewrite_Reformer_drop(&frame->w.reformer);
	if (frame->done_live)
		doltgres_rewrite_Rewritten_drop(&frame->w.done);
	frame->by_tuple_live = frame->mapped_live = frame->reformer_live = frame->done_live = false;
}

/*
 * doltgres_rewrite_rewritten_at finds a row version by its old tuple id among
 * those sorted by it, as binary_search_by_key does.
 */
static bool
doltgres_rewrite_rewritten_at(const StdVec *by_tuple, const StdVec *olds, uint64 tuple, size_t *result)
{
	size_t		size = by_tuple->len;
	size_t		base = 0;

	if (size == 0)
		return false;
	while (size > 1)
	{
		size_t		half = size / 2;
		size_t		mid = base + half;

		if (STD_VEC_AT(olds, DoltgresRewriteOld, STD_VEC_AT(by_tuple, size_t, mid)).tuple <= tuple)
			base = mid;
		size -= half;
	}
	if (STD_VEC_AT(olds, DoltgresRewriteOld, STD_VEC_AT(by_tuple, size_t, base)).tuple != tuple)
		return false;
	*result = STD_VEC_AT(by_tuple, size_t, base);
	return true;
}

/* doltgres_rewrite_rewritten_body judges and writes the row versions in the order the rewrite gives them. */
static void
doltgres_rewrite_rewritten_body(void *frame_)
{
	doltgres_rewrite_rewritten_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	DoltgresRewriteRewriting *w = &frame->w;
	DoltgresRewriteRewritten *done = &w->done;
	size_t		count = frame->read->olds.len;
	doltgres_OptionU64_t none = {0};
	doltgres_OptionFormed_t no_formed = {0};
	doltgres_OptionVecU32_t no_ids = {0};
	bool		ordered = false;
	bool		sorts = false;
	bool		kept;

	if (!pg_calls_rewrite_begin(raise, &frame->rewrite, frame->error))
		return;
	w->page = pg_calls_page_bytes();
	std_vec_with_capacity_Checked(&done->formed, count, DOLTGRES_REWRITE_FORMED);
	for (size_t i = 0; i < count; i++)
		std_vec_push_Checked(&done->formed, &no_formed, DOLTGRES_REWRITE_FORMED);
	std_vec_resize(&done->new_, count, &none, DOLTGRES_REWRITE_NEW);
	std_vec_from_elem_zeroed(&done->bytes, count, DOLTGRES_REWRITE_U32);
	std_vec_new(&done->values, DOLTGRES_REWRITE_VALUE);
	std_vec_with_capacity_Checked(&done->ids, count, DOLTGRES_REWRITE_IDS);
	for (size_t i = 0; i < count; i++)
		std_vec_push_Checked(&done->ids, &no_ids, DOLTGRES_REWRITE_IDS);
	done->packed.f0 = 1;
	frame->done_live = true;
	if (!doltgres_rewrite_Reformer_new(raise, frame->repo, frame->reading, frame->table, frame->read,
									   frame->key_columns, &w->reformer, frame->error))
		return;
	frame->reformer_live = true;
	w->rewrite = &frame->rewrite;
	w->read = frame->read;
	w->horizon = frame->horizon;
	std_vec_new(&w->mapped, DOLTGRES_REWRITE_PAIR);
	frame->mapped_live = true;
	w->reserve = w->page - pg_calls_rel_usable_bytes(frame->new_table);
	w->most = pg_calls_max_heap_tuples_per_page();
	std_vec_with_capacity_Checked(&frame->by_tuple, count, DOLTGRES_REWRITE_USIZE);
	frame->by_tuple_live = true;
	for (size_t i = 0; i < count; i++)
		std_vec_push_Checked(&frame->by_tuple, &i, DOLTGRES_REWRITE_USIZE);
	std_slice_sort_by_key_UsizeU64(frame->by_tuple.ptr, frame->by_tuple.len, doltgres_rewrite_rewritten_closure_2,
								   (void *) &frame->read->olds, NULL);
	if (frame->index != NULL)
	{
		if (!pg_calls_rewrite_order(raise, catalog, &frame->rewrite, frame->table->rel, frame->index, frame->sorted,
									&sorts, frame->error))
			return;
		ordered = true;
	}
	if (!ordered)
	{
		for (size_t i = 0; i < count; i++)
		{
			size_t		at = STD_VEC_AT(&frame->by_tuple, size_t, i);

			if (!doltgres_rewrite_Rewriting_kept(w, frame->caps, at, &kept, frame->error))
				return;
			if (kept && !doltgres_rewrite_Rewriting_rewrite(w, frame->caps, at, frame->error))
				return;
		}
	}
	else
	{
		int64		next;

		if (sorts)
		{
			for (size_t i = 0; i < count; i++)
			{
				size_t		at = STD_VEC_AT(&frame->by_tuple, size_t, i);
				DoltgresScanRow row;

				kept = true;
				if (frame->sorted && !doltgres_rewrite_Rewriting_kept(w, frame->caps, at, &kept, frame->error))
					return;
				if (!kept)
					continue;
				if (!doltgres_rewrite_Reformer_store(&w->reformer, frame->caps, at, &row, frame->error))
					return;
				doltgres_scan_Row_drop(&row);
				if (!pg_calls_rewrite_put(raise, catalog, &frame->rewrite, pg_OwnedSlot_slot(&w->reformer.slot),
										  STD_VEC_AT(&frame->read->olds, DoltgresRewriteOld, at).tuple,
										  frame->error))
					return;
			}
			if (!pg_calls_rewrite_sorted(raise, catalog, &frame->rewrite, frame->error))
				return;
		}
		for (;;)
		{
			size_t		at;

			if (!pg_calls_rewrite_next(raise, catalog, &frame->rewrite, pg_OwnedSlot_slot(&w->reformer.slot), &next,
									   frame->error))
				return;
			if (next < 0)
				break;
			if (!doltgres_rewrite_rewritten_at(&frame->by_tuple, &frame->read->olds, (uint64) next, &at))
			{
				uint64		tuple = (uint64) next;
				StdFmtPiece pieces[] = {STD_FMT_LIT("a rewrite's order gave tuple id "), STD_FMT_ARG(0),
				STD_FMT_LIT(", no row")};
				StdFmtArg	args[] = {{&tuple, std_u64_Display_fmt}};
				StdFmtArguments arguments = {pieces, 3, args};
				StdString	message;

				std_fmt_format_Checked(&message, &arguments);
				*frame->error = pg_PgError_fork((const uint8 *) "XX000", &message);
				return;
			}
			kept = true;
			if (!(sorts && frame->sorted) && !doltgres_rewrite_Rewriting_kept(w, frame->caps, at, &kept, frame->error))
				return;
			if (kept && !doltgres_rewrite_Rewriting_rewrite(w, frame->caps, at, frame->error))
				return;
		}
	}
	if (!doltgres_rewrite_Rewriting_ended(w, frame->caps, frame->error))
		return;
	frame->reformer_live = false;
	doltgres_rewrite_Reformer_drop(&w->reformer);
	if (!pg_calls_end_rewrite(raise, catalog, &frame->rewrite, frame->error))
		return;
	done->packed.f0 = w->has_next ? w->next : 1;
	done->packed.f1 = w->free;
	done->packed.f2 = w->most;
	*frame->result = *done;
	frame->done_live = false;
	frame->ok = true;
}

/*
 * doltgres_rewrite_rewritten rewrites the old storage's row versions as
 * heap's heapam_relation_copy_for_cluster does: VACUUM FULL judges and
 * writes them in old tuple id order; CLUSTER with a sort judges them in that
 * order and writes the kept ones in the order CLUSTER's own sort gives them
 * back; CLUSTER on a btree without one judges and writes them in the btree's
 * order, its keys then the old tuple id, which a sort of index tuples gives;
 * and CLUSTER on any other index in the order a scan of it gives. Each new
 * tuple id is packed into the new table's pages by heap's arithmetic under
 * its fill factor. It takes the key's columns; index is NULL for none.
 */
bool
doltgres_rewrite_rewritten(const PgCatalogCaps *caps, const RepoRepo *repo, const HostSharedReader *reading,
						   const DoltgresRewriteTable *table, StdVec *key_columns, const PgRel *new_table,
						   const PgRel *index, bool sorted, const DoltgresRewriteRead *read, uint64 horizon,
						   DoltgresRewriteRewritten *result, PgPgError *error)
{
	doltgres_rewrite_rewritten_frame_t frame = {.caps = caps, .repo = repo, .reading = reading, .table = table,
		.key_columns = key_columns, .new_table = new_table, .index = index, .sorted = sorted, .read = read,
	.horizon = horizon, .result = result, .error = error};

	std_landing(&frame, doltgres_rewrite_rewritten_body, doltgres_rewrite_rewritten_pad);
	doltgres_rewrite_rewritten_pad(&frame);
	return frame.ok;
}


/*
 * doltgres_rewrite_Reformer_drop is Reformer's drop glue: its key places, its
 * slot, its epochs, and its layout or key.
 */
void
doltgres_rewrite_Reformer_drop(DoltgresRewriteReformer *self)
{
	std_vec_free_Checked(&self->keyed, DOLTGRES_REWRITE_USIZE);
	pg_OwnedSlot_drop(&self->slot);
	doltgres_scan_Epochs_drop(&self->epochs);
	if (self->has_layout)
		access_heaptuple_RowLayout_drop(&self->layout);
	else
		doltgres_layout_VecKeyed_drop(&self->key_columns);
}

/*
 * doltgres_rewrite_Reformer_new_frame_t is Reformer::new's state across its
 * pad: the key's columns it takes, the key places, and the epochs, each
 * while live.
 */
typedef struct doltgres_rewrite_Reformer_new_frame_t
{
	DoltgresRewriteReformer made;
	bool		key_columns_live;
	bool		keyed_live;
	bool		epochs_live;
	const PgMayRaise *raise;
	DoltgresRewriteReformer *result;
	PgPgError  *error;
	bool		ok;
} doltgres_rewrite_Reformer_new_frame_t;

/* doltgres_rewrite_Reformer_new_pad frees what Reformer::new holds. */
static void
doltgres_rewrite_Reformer_new_pad(void *frame_)
{
	doltgres_rewrite_Reformer_new_frame_t *frame = frame_;

	if (frame->epochs_live)
		doltgres_scan_Epochs_drop(&frame->made.epochs);
	if (frame->keyed_live)
		std_vec_free_Checked(&frame->made.keyed, DOLTGRES_REWRITE_USIZE);
	if (frame->key_columns_live)
		doltgres_layout_VecKeyed_drop(&frame->made.key_columns);
	frame->epochs_live = frame->keyed_live = frame->key_columns_live = false;
}

/* doltgres_rewrite_Reformer_new_body takes the key's places, the base's epochs, and a slot over the old table. */
static void
doltgres_rewrite_Reformer_new_body(void *frame_)
{
	doltgres_rewrite_Reformer_new_frame_t *frame = frame_;
	DoltgresRewriteReformer *made = &frame->made;
	const StdVec *key_columns = &made->key_columns;
	TreesStoreReader store;
	HostIoIo	io;
	HostError	failed;

	std_vec_with_capacity_Checked(&made->keyed, key_columns->len, DOLTGRES_REWRITE_USIZE);
	frame->keyed_live = true;
	for (size_t i = 0; i < key_columns->len; i++)
		std_vec_push_Checked(&made->keyed, &STD_VEC_AT(key_columns, access_TupleUsizeOrderKeyType_t, i).f0,
							 DOLTGRES_REWRITE_USIZE);
	io = pghost_io(frame->raise);
	store = repo_Repo_reader(made->repo, HOST_CACHE_PLACEMENT_SERVER, made->reading);
	if (!doltgres_scan_Epochs_of(&io, TREES_STORE_READER_NODE_STORE(&store), made->read->base.epoch,
								 made->read->base.has_chain, made->read->base.chain, &made->epochs, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->epochs_live = true;
	if (!pg_calls_make_slot(frame->raise, made->table->rel, &made->slot, frame->error))
		return;
	*frame->result = *made;
	frame->epochs_live = frame->keyed_live = frame->key_columns_live = false;
	frame->ok = true;
}

/*
 * doltgres_rewrite_Reformer_new returns a re-former of the old storage's rows
 * under key columns, which it takes, with a slot over the old table's
 * columns.
 */
bool
doltgres_rewrite_Reformer_new(const PgMayRaise *raise, const RepoRepo *repo, const HostSharedReader *reading,
							  const DoltgresRewriteTable *table, const DoltgresRewriteRead *read, StdVec *key_columns,
							  DoltgresRewriteReformer *result, PgPgError *error)
{
	doltgres_rewrite_Reformer_new_frame_t frame = {.made = {.repo = repo, .reading = reading, .table = table,
	.read = read, .key_columns = *key_columns}, .key_columns_live = true, .raise = raise, .result = result,
	.error = error};

	std_landing(&frame, doltgres_rewrite_Reformer_new_body, doltgres_rewrite_Reformer_new_pad);
	doltgres_rewrite_Reformer_new_pad(&frame);
	return frame.ok;
}

/* doltgres_rewrite_pg_fork returns a Postgres-flavored fork error of a literal message. */
static PgPgError
doltgres_rewrite_pg_fork(const char *sqlstate, const char *message)
{
	StdString text;

	std_string_from_Checked(&text, message, strlen(message));
	return pg_PgError_fork((const uint8 *) sqlstate, &text);
}

/* doltgres_rewrite_ids_bytes appends each value id's four bytes, low byte first, as a stored row's values hold them. */
static void
doltgres_rewrite_ids_bytes(StdVec *out, uint32 id)
{
	uint8		bytes[4] = {(uint8) id, (uint8) (id >> 8), (uint8) (id >> 16), (uint8) (id >> 24)};

	std_vec_extend_from_slice_Checked(out, bytes, 4, DOLTGRES_REWRITE_BYTES);
}

/*
 * doltgres_rewrite_store_frame_t is store's state across its pad: the row
 * made so far, while live.
 */
typedef struct doltgres_rewrite_store_frame_t
{
	DoltgresScanRow row;
	bool		row_live;
	DoltgresRewriteReformer *self;
	const PgCatalogCaps *caps;
	size_t		at;
	DoltgresScanRow *result;
	PgPgError  *error;
	bool		ok;
} doltgres_rewrite_store_frame_t;

/* doltgres_rewrite_store_pad frees what store holds. */
static void
doltgres_rewrite_store_pad(void *frame_)
{
	doltgres_rewrite_store_frame_t *frame = frame_;

	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	frame->row_live = false;
}

/* doltgres_rewrite_store_body makes the row version's row as heap's tuple holds it, then stores it in the slot. */
static void
doltgres_rewrite_store_body(void *frame_)
{
	doltgres_rewrite_store_frame_t *frame = frame_;
	DoltgresRewriteReformer *self = frame->self;
	const DoltgresRewriteOld *old = &STD_VEC_AT(&self->read->olds, DoltgresRewriteOld, frame->at);
	const DoltgresRewriteKey *key = &STD_VEC_AT(&self->read->keys, DoltgresRewriteKey, old->key);
	DoltgresScanRow *row = &frame->row;
	HostError	failed;

	row->dirty = txn_visible_Dirty_Default_default();
	std_vec_new(&row->values, DOLTGRES_REWRITE_BYTES);
	if (old->from.tag == DOLTGRES_REWRITE_FROM_BASE)
	{
		const TxnRewriteBased *based = &STD_VEC_AT(&self->read->based, TxnRewriteBased, old->from.place);
		StdBytes	tuple = {based->value.ptr, based->value.len};

		if (self->table->keyless)
		{
			ChunksErrorError storage;
			uint64		count;

			if (!txn_counted_keyless_parts(based->value.ptr, based->value.len, &count, &tuple, &storage))
			{
				failed = txn_base_storage(&storage);
				*frame->error = pghost_raised(&failed);
				std_vec_free_Checked(&row->values, DOLTGRES_REWRITE_BYTES);
				return;
			}
		}
		if (!doltgres_scan_Epochs_transform(&self->epochs, doltgres_scan_Epochs_base(&self->epochs), &row->transform,
											&failed))
		{
			*frame->error = pghost_raised(&failed);
			std_vec_free_Checked(&row->values, DOLTGRES_REWRITE_BYTES);
			return;
		}
		std_slice_to_vec_Checked(&row->tuple, tuple.ptr, tuple.len, DOLTGRES_REWRITE_BYTES);
		for (size_t i = 0; i < based->values.len; i++)
			doltgres_rewrite_ids_bytes(&row->values, STD_VEC_AT(&based->values, uint32, i));
		row->top = 0;
	}
	else
	{
		const doltgres_TupleVecU8Bool_t *stored = &STD_VEC_AT(&key->versions, doltgres_TupleVecU8Bool_t,
															  old->from.place);
		TxnMemtableVersionRecord record;
		TxnMemtableVersionWriter writer;
		TxnMemtableVersionOpRef op;
		TxnMemtableVersionPutRef put;
		TxnMemtableVersionValues ids;
		TxnStatusCommit commit;
		HostIoIo	io = pghost_io_catalog(frame->caps);
		HostXid		top;
		bool		has_end = true;
		uint64		end = 0;
		uint32		id;

		if (!txn_memtable_version_stored(stored->f0.ptr, stored->f0.len, &record, &failed))
		{
			*frame->error = pghost_raised(&failed);
			std_vec_free_Checked(&row->values, DOLTGRES_REWRITE_BYTES);
			return;
		}
		writer = txn_memtable_version_Record_writer(&record);
		top.f0 = writer.has_top ? writer.top : writer.xid;
		if (!txn_status_committed(&io, top, &commit, &failed))
		{
			*frame->error = pghost_raised(&failed);
			std_vec_free_Checked(&row->values, DOLTGRES_REWRITE_BYTES);
			return;
		}
		if (commit.tag == TXN_STATUS_COMMIT_AT)
			end = commit.u.at;
		else if (top.f0 >= TXN_STATUS_FIRST_NORMAL)
			has_end = false;
		if (!doltgres_scan_Epochs_of_written(&self->epochs, top.f0, writer.command, has_end, end, &row->transform,
											 &failed))
		{
			*frame->error = pghost_raised(&failed);
			std_vec_free_Checked(&row->values, DOLTGRES_REWRITE_BYTES);
			return;
		}
		op = txn_memtable_version_Record_op(&record);
		switch (op.tag)
		{
			case TXN_MEMTABLE_VERSION_OP_REF_PUT:
				put = op.u.put;
				break;
			case TXN_MEMTABLE_VERSION_OP_REF_ADD:
				put = op.u.add;
				break;
			case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
				put = op.u.add_row;
				break;
			case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
				put = op.u.update_row.f1;
				break;
			default:
				*frame->error = doltgres_rewrite_pg_fork("XX000", "a rewrite's row version writes no row");
				std_vec_free_Checked(&row->transform, DOLTGRES_REWRITE_BYTES);
				std_vec_free_Checked(&row->values, DOLTGRES_REWRITE_BYTES);
				return;
		}
		std_slice_to_vec_Checked(&row->tuple, put.value.ptr, put.value.len, DOLTGRES_REWRITE_BYTES);
		ids = txn_memtable_version_PutRef_values(&put);
		while (txn_memtable_version_Values_Iterator_next(&ids, &id))
			doltgres_rewrite_ids_bytes(&row->values, id);
		row->top = top.f0;
	}
	std_slice_to_vec_Checked(&row->keyed, self->table->keyed, self->table->keyed_len, DOLTGRES_REWRITE_BYTES);
	row->table = pg_calls_rel_id(self->table->rel);
	std_vec_clone(&row->key, &key->key, DOLTGRES_REWRITE_BYTES);
	row->stamp.txn = old->xmin;
	row->stamp.tuple = old->tuple;
	row->command = 0;
	row->has_changer = false;
	frame->row_live = true;
	if (!doltgres_entries_stored(pg_CatalogCaps_may_raise(frame->caps), pg_OwnedSlot_slot(&self->slot),
								 self->table->rel, row, frame->error))
		return;
	*frame->result = *row;
	frame->row_live = false;
	frame->ok = true;
}

/*
 * doltgres_rewrite_Reformer_store stores a row version's row into the slot,
 * as heap's tuple holds it, and returns it; the key is cloned, since the
 * read keeps its own and the row is the slot's to own.
 */
static bool
doltgres_rewrite_Reformer_store(DoltgresRewriteReformer *self, const PgCatalogCaps *caps, size_t at,
								DoltgresScanRow *result, PgPgError *error)
{
	doltgres_rewrite_store_frame_t frame = {.self = self, .caps = caps, .at = at, .result = result,
	.error = error};

	std_landing(&frame, doltgres_rewrite_store_body, doltgres_rewrite_store_pad);
	doltgres_rewrite_store_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_rewrite_formed_frame_t is formed's state across its pad: the
 * update's keeping until toasted takes it, the slot's columns and held
 * values, the values they set, the kept values, the placing, and the values
 * toasted, each while live.
 */
typedef struct doltgres_rewrite_formed_frame_t
{
	DoltgresEntriesKeeping keeping;
	bool		keeping_live;
	StdVec		columns;
	bool		columns_live;
	StdVec		held;
	bool		held_live;
	StdVec		values;
	bool		values_live;
	StdVec		kept;
	bool		kept_live;
	AccessHeaptuplePlacing placing;
	bool		placing_live;
	bool		toasted_live;
	DoltgresRewriteReformer *self;
	const PgCatalogCaps *caps;
	AccessHeaptupleFormed *result_0;
	bool	   *has_result_1;
	DoltgresEntriesToasted *result_1;
	PgPgError  *error;
	bool		ok;
} doltgres_rewrite_formed_frame_t;

/* doltgres_rewrite_formed_pad frees what formed holds. */
static void
doltgres_rewrite_formed_pad(void *frame_)
{
	doltgres_rewrite_formed_frame_t *frame = frame_;

	if (frame->toasted_live)
		doltgres_entries_Toasted_drop(frame->result_1);
	if (frame->placing_live)
		access_heaptuple_Placing_drop(&frame->placing);
	if (frame->kept_live)
		std_vec_free_Checked(&frame->kept, sizeof(AccessHeaptupleKept), _Alignof(AccessHeaptupleKept));
	if (frame->values_live)
		std_vec_free_Checked(&frame->values, sizeof(AccessHeaptupleSet), _Alignof(AccessHeaptupleSet));
	if (frame->held_live)
		doltgres_layout_VecHeld_drop(&frame->held);
	if (frame->columns_live)
		std_vec_free_Checked(&frame->columns, sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn));
	if (frame->keeping_live)
		doltgres_entries_Keeping_drop(&frame->keeping);
	frame->toasted_live = frame->placing_live = frame->kept_live = frame->values_live = false;
	frame->held_live = frame->columns_live = frame->keeping_live = false;
}

/*
 * doltgres_rewrite_formed_body forms the slot's row by the table's current
 * layout, writing what it moves out of line.
 */
static void
doltgres_rewrite_formed_body(void *frame_)
{
	doltgres_rewrite_formed_frame_t *frame = frame_;
	DoltgresRewriteReformer *self = frame->self;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	PgSlot	   *slot = pg_OwnedSlot_slot(&self->slot);
	AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, frame->caps};
	AccessKeyNames names = {&DOLTGRES_LAYOUT_BLOB_NAMES_NAMES_OPS, NULL};
	bool		keeping = frame->keeping_live;
	HostIoIo	io;
	bool		external = false;
	int32		natts;

	if (!pg_calls_slot_getallattrs(raise, slot, frame->error))
		return;
	if (!doltgres_layout_slot_row(raise, catalog, self->table->rel, slot, &frame->columns, &frame->held,
								  frame->error))
		return;
	frame->columns_live = frame->held_live = true;
	doltgres_layout_values(frame->columns.ptr, frame->columns.len, frame->held.ptr, frame->held.len, &frame->values);
	frame->values_live = true;
	if (!self->has_layout)
	{
		StdVec		key = self->key_columns;

		std_vec_new(&self->key_columns, sizeof(access_TupleUsizeOrderKeyType_t),
					_Alignof(access_TupleUsizeOrderKeyType_t));
		frame->columns_live = false;
		access_heaptuple_RowLayout_new(&frame->columns, &key, &self->layout);
		doltgres_layout_VecKeyed_drop(&self->key_columns);
		self->has_layout = true;
	}
	std_vec_new(&frame->kept, sizeof(AccessHeaptupleKept), _Alignof(AccessHeaptupleKept));
	frame->kept_live = true;
	for (size_t i = 0; keeping && i < frame->keeping.values.len; i++)
	{
		const doltgres_TupleUsizeU32Entry_t *value = &STD_VEC_AT(&frame->keeping.values,
																 doltgres_TupleUsizeU32Entry_t, i);
		AccessHeaptupleKept kept = {value->f0, value->f2.stored, value->f2.address.f0};

		std_vec_push_Checked(&frame->kept, &kept, sizeof(AccessHeaptupleKept), _Alignof(AccessHeaptupleKept));
	}
	natts = pg_calls_slot_natts(slot);
	for (int32 column = 0; column < natts && !external; column++)
		external = pg_calls_slot_value_outside(slot, column);
	if (!access_heaptuple_RowLayout_placing(&self->layout, sorter, names, frame->values.ptr, frame->values.len,
											frame->kept.ptr, frame->kept.len, external, self->table->target,
											&frame->placing, frame->error))
		return;
	frame->placing_live = true;
	frame->kept_live = false;
	std_vec_free_Checked(&frame->kept, sizeof(AccessHeaptupleKept), _Alignof(AccessHeaptupleKept));
	io = pghost_io_catalog(frame->caps);
	frame->keeping_live = false;
	if (!doltgres_entries_toasted(frame->caps, &io, self->repo, self->reading, self->table->rel, &frame->placing,
								  keeping ? &frame->keeping : NULL, frame->has_result_1, frame->result_1,
								  frame->error))
		return;
	frame->toasted_live = *frame->has_result_1;
	frame->placing_live = false;
	if (!access_heaptuple_Placing_formed(&frame->placing, frame->result_0, frame->error))
		return;
	frame->toasted_live = false;
	frame->ok = true;
}

/*
 * doltgres_rewrite_Reformer_formed forms the row the slot holds as an insert
 * forms one: every column of the table's current descriptor, an added
 * column's missing value stored and a dropped column's left out, as heap's
 * reform_and_rewrite_tuple forms it, with the heap bytes heap's packing
 * counts, each value out of line kept as an update keeps one (entries'
 * keeping, the row as both its old and its new), as heap keeps its value ids
 * under the swap by content, and a value the placing moves out of line anew
 * written as an insert writes one (entries' toasted), under an id the old
 * value map doesn't hold. It takes the keeping, NULL for none.
 */
static bool
doltgres_rewrite_Reformer_formed(DoltgresRewriteReformer *self, const PgCatalogCaps *caps,
								 DoltgresEntriesKeeping *keeping, AccessHeaptupleFormed *result_0,
								 bool *has_result_1, DoltgresEntriesToasted *result_1, PgPgError *error)
{
	doltgres_rewrite_formed_frame_t frame = {.self = self, .caps = caps, .result_0 = result_0,
	.has_result_1 = has_result_1, .result_1 = result_1, .error = error};

	if (keeping != NULL)
	{
		frame.keeping = *keeping;
		frame.keeping_live = true;
	}
	std_landing(&frame, doltgres_rewrite_formed_body, doltgres_rewrite_formed_pad);
	doltgres_rewrite_formed_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_rewrite_Reformer_null_key returns the place of the first key column
 * the row in the slot holds NULL in, if any.
 */
bool
doltgres_rewrite_Reformer_null_key(DoltgresRewriteReformer *self, size_t *result)
{
	const PgSlot *slot = pg_OwnedSlot_slot(&self->slot);

	for (size_t i = 0; i < self->keyed.len; i++)
	{
		size_t		column = STD_VEC_AT(&self->keyed, size_t, i);

		if (pg_calls_slot_value_null(slot, (int32) column))
		{
			*result = column;
			return true;
		}
	}
	return false;
}

/* doltgres_rewrite_Reformer_rel lends the old table. */
const PgRel *
doltgres_rewrite_Reformer_rel(const DoltgresRewriteReformer *self)
{
	return self->table->rel;
}

/*
 * doltgres_rewrite_count counts a value id once more in the rewrite's values,
 * a sorted Vec as its BTreeMap's entry API keeps it.
 */
static void
doltgres_rewrite_count(StdVec *values, uint32 id, const TxnValuesEntry *entry)
{
	size_t		low = 0;
	size_t		high = values->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		txn_TupleU32TupleEntryU64_t *at = &STD_VEC_AT(values, txn_TupleU32TupleEntryU64_t, mid);

		if (at->f0 == id)
		{
			at->f1.f1 = STD_ADD(u64, at->f1.f1, 1, 550, 17);
			return;
		}
		if (at->f0 < id)
			low = mid + 1;
		else
			high = mid;
	}
	{
		txn_TupleU32TupleEntryU64_t made = {id, {*entry, 1}};

		std_vec_insert(values, low, &made, DOLTGRES_REWRITE_VALUE, RUST_FILE, 550, 17);
	}
}

/*
 * doltgres_rewrite_reformed_frame_t is reformed's state across its pad: the
 * row stored, the keeping until formed takes it, the row re-formed, and the
 * values toasted, each while live.
 */
typedef struct doltgres_rewrite_reformed_frame_t
{
	DoltgresScanRow row;
	bool		row_live;
	DoltgresEntriesKeeping keeping;
	bool		keeping_live;
	AccessHeaptupleFormed formed;
	bool		formed_live;
	DoltgresEntriesToasted toasted;
	bool		toasted_live;
	DoltgresRewriteReformer *self;
	const PgCatalogCaps *caps;
	size_t		at;
	StdVec	   *values;
	DoltgresScanRow *result_0;
	AccessHeaptupleFormed *result_1;
	bool	   *has_result_2;
	StdVec	   *result_2;
	PgPgError  *error;
	bool		ok;
} doltgres_rewrite_reformed_frame_t;

/* doltgres_rewrite_reformed_pad frees what reformed holds. */
static void
doltgres_rewrite_reformed_pad(void *frame_)
{
	doltgres_rewrite_reformed_frame_t *frame = frame_;

	if (frame->toasted_live)
		doltgres_entries_Toasted_drop(&frame->toasted);
	if (frame->formed_live)
		access_heaptuple_Formed_drop(&frame->formed);
	if (frame->keeping_live)
		doltgres_entries_Keeping_drop(&frame->keeping);
	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	frame->toasted_live = frame->formed_live = frame->keeping_live = frame->row_live = false;
}

/* doltgres_rewrite_reformed_body stores the row, re-forms it, and counts each value id it names. */
static void
doltgres_rewrite_reformed_body(void *frame_)
{
	doltgres_rewrite_reformed_frame_t *frame = frame_;
	DoltgresRewriteReformer *self = frame->self;
	bool		has_keeping = false;
	bool		has_toasted;
	HostError	failed;

	if (!doltgres_rewrite_Reformer_store(self, frame->caps, frame->at, &frame->row, frame->error))
		return;
	frame->row_live = true;
	if (frame->row.values.len > 0)
	{
		PgSlot	   *slot = pg_OwnedSlot_slot(&self->slot);
		HostIoIo	io;

		if (!pg_calls_slot_getallattrs(pg_CatalogCaps_may_raise(frame->caps), slot, frame->error))
			return;
		io = pghost_io_catalog(frame->caps);
		if (!doltgres_entries_keeping(frame->caps, &io, self->repo, self->reading, self->table->rel, slot, slot,
									  self->keyed.ptr, self->keyed.len, &has_keeping, &frame->keeping, frame->error))
			return;
		frame->keeping_live = has_keeping;
	}
	frame->keeping_live = false;
	if (!doltgres_rewrite_Reformer_formed(self, frame->caps, has_keeping ? &frame->keeping : NULL, &frame->formed,
										  &has_toasted, &frame->toasted, frame->error))
		return;
	frame->formed_live = true;
	frame->toasted_live = has_toasted;
	*frame->has_result_2 = false;
	if (has_toasted)
	{
		HostIoIo	io = pghost_io_catalog(frame->caps);
		TreesStoreReader store = repo_Repo_reader(self->repo, HOST_CACHE_PLACEMENT_SERVER, self->reading);
		const DoltgresEntriesToasted *toasted = &frame->toasted;
		size_t		fresh = 0;
		bool		same;

		for (size_t place = 0; place < toasted->ids.len; place++)
		{
			uint32		id = STD_VEC_AT(&toasted->ids, uint32, place);
			bool		kept = false;
			bool		has = false;
			TxnValuesEntry entry;

			for (size_t k = 0; k < toasted->kept.len && !kept; k++)
				kept = STD_VEC_AT(&toasted->kept, size_t, k) == place;
			if (kept)
			{
				if (!txn_values_entry(&io, TREES_STORE_READER_NODE_STORE(&store), &toasted->state, id, &has, &entry,
									  &failed))
				{
					*frame->error = pghost_raised(&failed);
					return;
				}
			}
			else if (fresh < toasted->entries.len)
			{
				entry = STD_VEC_AT(&toasted->entries, TxnValuesEntry, fresh++);
				has = true;
			}
			if (!has)
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("a kept value's map entry "), STD_FMT_ARG(0),
				STD_FMT_LIT(" is missing")};
				StdFmtArg	args[] = {{&id, std_u32_Display_fmt}};
				StdFmtArguments arguments = {pieces, 3, args};
				StdString	message;

				std_fmt_format_Checked(&message, &arguments);
				*frame->error = pg_PgError_fork((const uint8 *) "XX001", &message);
				return;
			}
			doltgres_rewrite_count(frame->values, id, &entry);
		}
		same = toasted->ids.len * 4 == frame->row.values.len - frame->row.values.len % 4;
		for (size_t i = 0; same && i < toasted->ids.len; i++)
		{
			const uint8 *bytes = (const uint8 *) frame->row.values.ptr + i * 4;
			uint32		id = (uint32) bytes[0] | (uint32) bytes[1] << 8 | (uint32) bytes[2] << 16 |
				(uint32) bytes[3] << 24;

			same = STD_VEC_AT(&toasted->ids, uint32, i) == id;
		}
		if (!same)
		{
			*frame->has_result_2 = true;
			*frame->result_2 = frame->toasted.ids;
			frame->toasted.ids = (StdVec) STD_VEC_NEW;
		}
	}
	*frame->result_0 = frame->row;
	*frame->result_1 = frame->formed;
	frame->row_live = frame->formed_live = false;
	frame->ok = true;
}

/*
 * doltgres_rewrite_Reformer_reformed re-forms a row version's row under the
 * key columns, as an insert forms it, returning the row as stored, the row
 * re-formed, and its out-of-line values' ids when a value moved out of line
 * anew; each value id the row names, with its map entry, is counted in
 * values.
 */
bool
doltgres_rewrite_Reformer_reformed(DoltgresRewriteReformer *self, const PgCatalogCaps *caps, size_t at,
								   StdVec *values, DoltgresScanRow *result_0, AccessHeaptupleFormed *result_1,
								   bool *has_result_2, StdVec *result_2, PgPgError *error)
{
	doltgres_rewrite_reformed_frame_t frame = {.self = self, .caps = caps, .at = at, .values = values,
		.result_0 = result_0, .result_1 = result_1, .has_result_2 = has_result_2, .result_2 = result_2,
	.error = error};

	std_landing(&frame, doltgres_rewrite_reformed_body, doltgres_rewrite_reformed_pad);
	doltgres_rewrite_reformed_pad(&frame);
	return frame.ok;
}


/* More element layouts of the rewrite's vectors. */
#define DOLTGRES_REWRITE_COPIED sizeof(TxnRewriteCopied), _Alignof(TxnRewriteCopied)
#define DOLTGRES_REWRITE_COPY sizeof(txn_TupleU64U64U32_t), _Alignof(txn_TupleU64U64U32_t)
#define DOLTGRES_REWRITE_CARRY sizeof(DoltgresRewriteCarry), _Alignof(DoltgresRewriteCarry)
#define DOLTGRES_REWRITE_KEPT_ROW sizeof(doltgres_rewrite_kept_t), _Alignof(doltgres_rewrite_kept_t)
#define DOLTGRES_REWRITE_ROW_ENDS sizeof(doltgres_rewrite_row_ends_t), _Alignof(doltgres_rewrite_row_ends_t)
#define DOLTGRES_REWRITE_ROWS sizeof(StdVec), _Alignof(StdVec)
#define DOLTGRES_REWRITE_BLOB sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t)

/* doltgres_rewrite_Stored_drop is Stored's drop glue: a re-formed row's tuple, addresses, and value ids. */
static void
doltgres_rewrite_Stored_drop(TxnRewriteStored *self)
{
	if (self->tag == TXN_REWRITE_STORED_FORMED)
	{
		std_vec_free_Checked(&self->u.formed.tuple, DOLTGRES_REWRITE_BYTES);
		std_vec_free_Checked(&self->u.formed.addresses, sizeof(chunks_TupleU32Locator_t),
							 _Alignof(chunks_TupleU32Locator_t));
		std_vec_free_Checked(&self->u.formed.values, DOLTGRES_REWRITE_U32);
	}
}

/*
 * doltgres_rewrite_copied returns the new table tree's entries from the kept
 * copies of the old base's entries, each under its key, in its order: a
 * re-formed copy under its re-formed row's key, the row and its value ids
 * moved out of the rewrite's results, and any other under its entry's key,
 * held as the base held it. That key is cloned, since a keyless entry's
 * copies may stay under it while others' re-formed rows join it, and so are
 * the entry's value ids for a re-formed copy that kept them, which the
 * entry's other copies may keep too.
 */
void
doltgres_rewrite_copied(const DoltgresRewriteRead *read, DoltgresRewriteRewritten *done, StdVec *result)
{
	std_vec_new(result, DOLTGRES_REWRITE_COPIED);
	for (size_t at = 0; at < read->olds.len; at++)
	{
		const DoltgresRewriteOld *old = &STD_VEC_AT(&read->olds, DoltgresRewriteOld, at);
		doltgres_OptionU64_t tuple = STD_VEC_AT(&done->new_, doltgres_OptionU64_t, at);
		doltgres_OptionFormed_t *formed = &STD_VEC_AT(&done->formed, doltgres_OptionFormed_t, at);
		doltgres_OptionVecU32_t *ids = &STD_VEC_AT(&done->ids, doltgres_OptionVecU32_t, at);
		const TxnRewriteBased *based;
		txn_TupleU64U64U32_t copy;
		StdVec		key;
		TxnRewriteStored stored;
		size_t		low = 0;
		size_t		high = result->len;
		bool		found = false;

		if (old->from.tag != DOLTGRES_REWRITE_FROM_BASE || !tuple.has)
			continue;
		based = &STD_VEC_AT(&read->based, TxnRewriteBased, old->from.place);
		if (formed->has)
		{
			formed->has = false;
			key = formed->f0.key;
			stored.tag = TXN_REWRITE_STORED_FORMED;
			stored.u.formed.tuple = formed->f0.tuple;
			stored.u.formed.addresses = formed->f0.addresses;
			if (ids->has)
			{
				ids->has = false;
				stored.u.formed.values = ids->f0;
			}
			else
				std_vec_clone(&stored.u.formed.values, &based->values, DOLTGRES_REWRITE_U32);
		}
		else
		{
			std_vec_clone(&key, &based->key, DOLTGRES_REWRITE_BYTES);
			stored.tag = TXN_REWRITE_STORED_BASED;
			stored.u.based = old->from.place;
		}
		copy.f0 = old->xmin;
		copy.f1 = tuple.f0;
		copy.f2 = STD_VEC_AT(&done->bytes, uint32, at);
		while (low < high && !found)
		{
			size_t		mid = low + (high - low) / 2;
			const StdVec *at_key = &STD_VEC_AT(result, TxnRewriteCopied, mid).f0;
			StdOrdering order = std_bytes_Ord_cmp(at_key->ptr, at_key->len, key.ptr, key.len);

			if (order == STD_ORDERING_EQUAL)
			{
				found = true;
				low = mid;
			}
			else if (order == STD_ORDERING_LESS)
				low = mid + 1;
			else
				high = mid;
		}
		if (found)
		{
			doltgres_rewrite_Stored_drop(&stored);
			std_vec_free_Checked(&key, DOLTGRES_REWRITE_BYTES);
		}
		else
		{
			TxnRewriteCopied entry = {key, stored};

			std_vec_new(&entry.f2, DOLTGRES_REWRITE_COPY);
			std_vec_insert(result, low, &entry, DOLTGRES_REWRITE_COPIED, RUST_FILE, 580, 25);
		}
		std_vec_push_Checked(&STD_VEC_AT(result, TxnRewriteCopied, low).f2, &copy, DOLTGRES_REWRITE_COPY);
	}
}

/* doltgres_rewrite_Carry_drop is Carry's drop glue: its row change. */
static void
doltgres_rewrite_Carry_drop(void *carry)
{
	txn_record_RowChange_drop(&((DoltgresRewriteCarry *) carry)->f0);
}

/*
 * doltgres_rewrite_kept_t is an entry of carrying's kept rows: an old tuple
 * id, its new one, its heap bytes, and its row re-formed.
 */
typedef struct doltgres_rewrite_kept_t
{
	uint64		old;
	uint64		new_;
	uint32		bytes;
	const AccessHeaptupleFormed *formed;
	const StdVec *ids;
} doltgres_rewrite_kept_t;

/*
 * doltgres_rewrite_row_ends_t is a kept row of a key: its version's place,
 * none for a base row, and its ender's place.
 */
typedef struct doltgres_rewrite_row_ends_t
{
	bool		has_row;
	size_t		row;
	bool		has_ender;
	size_t		ender;
} doltgres_rewrite_row_ends_t;

/*
 * doltgres_rewrite_carrying_t is carrying's state its closures read: the kept
 * rows by old tuple id and whether the table is keyless.
 */
typedef struct doltgres_rewrite_carrying_t
{
	StdVec		kept;
	bool		keyless;
} doltgres_rewrite_carrying_t;

/* doltgres_rewrite_kept_find returns a kept row by its old tuple id, or NULL. */
static const doltgres_rewrite_kept_t *
doltgres_rewrite_kept_find(const doltgres_rewrite_carrying_t *c, uint64 tuple)
{
	size_t		low = 0;
	size_t		high = c->kept.len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		const doltgres_rewrite_kept_t *at = &STD_VEC_AT(&c->kept, doltgres_rewrite_kept_t, mid);

		if (at->old == tuple)
			return at;
		if (at->old < tuple)
			low = mid + 1;
		else
			high = mid;
	}
	return NULL;
}

/* doltgres_rewrite_carrying_closure_1 is carrying's new: a kept row's new tuple id. */
static bool
doltgres_rewrite_carrying_closure_1(const doltgres_rewrite_carrying_t *c, HostRow tuple, HostRow *result)
{
	const doltgres_rewrite_kept_t *kept = doltgres_rewrite_kept_find(c, tuple.f0);

	if (kept == NULL)
		return false;
	result->f0 = kept->new_;
	return true;
}

/* doltgres_rewrite_carrying_closure_2 is carrying's rekeyed: a keyless row's key re-formed, or none. */
static bool
doltgres_rewrite_carrying_closure_2(const doltgres_rewrite_carrying_t *c, HostRow tuple, StdBytes *result)
{
	const doltgres_rewrite_kept_t *kept;

	if (!c->keyless)
		return false;
	kept = doltgres_rewrite_kept_find(c, tuple.f0);
	if (kept == NULL || kept->formed == NULL)
		return false;
	result->ptr = kept->formed->key.ptr;
	result->len = kept->formed->key.len;
	return true;
}

/*
 * doltgres_rewrite_carrying_closure_4 is carrying's moved: a put of a kept
 * row with its new tuple id and heap bytes, its value ids renumbered when
 * the rewrite moved a value anew, and its row re-formed when that differs,
 * or false with the put dropped for a row the rewrite dropped.
 */
static bool
doltgres_rewrite_carrying_closure_4(const doltgres_rewrite_carrying_t *c, TxnMemtablePut *put)
{
	const doltgres_rewrite_kept_t *kept = doltgres_rewrite_kept_find(c, put->tuple.f0);

	if (kept != NULL && kept->ids != NULL)
	{
		std_vec_free_Checked(&put->values, DOLTGRES_REWRITE_U32);
		std_vec_clone(&put->values, kept->ids, DOLTGRES_REWRITE_U32);
	}
	if (kept == NULL)
	{
		txn_memtable_Put_drop(put);
		return false;
	}
	if (kept->formed != NULL)
	{
		std_vec_free_Checked(&put->value, DOLTGRES_REWRITE_BYTES);
		std_vec_free_Checked(&put->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
		std_vec_clone(&put->value, &kept->formed->tuple, DOLTGRES_REWRITE_BYTES);
		std_vec_clone(&put->addresses, &kept->formed->addresses, sizeof(chunks_TupleU32Locator_t),
					  _Alignof(chunks_TupleU32Locator_t));
	}
	put->tuple.f0 = kept->new_;
	put->heap_bytes = kept->bytes;
	return true;
}

/*
 * doltgres_rewrite_carrying_closure_5 is carrying's renamed: a successor's
 * key with its kept row's new tuple id, and for a keyless row its key
 * re-formed, or false with the successor dropped for a row the rewrite
 * dropped; any other successor is kept as it is.
 */
static bool
doltgres_rewrite_carrying_closure_5(const doltgres_rewrite_carrying_t *c, TxnMemtableSuccessor *successor)
{
	StdVec	   *key = &successor->u.key;
	StdBytes	prefix;
	HostRow		named = {0};
	HostRow		tuple;
	StdVec		renamed;
	size_t		at;

	if (successor->tag != TXN_MEMTABLE_SUCCESSOR_KEY || key->len < 8)
		return true;
	at = key->len - 8;
	for (size_t i = 0; i < 8; i++)
		named.f0 = named.f0 << 8 | ((const uint8 *) key->ptr)[at + i];
	if (!doltgres_rewrite_carrying_closure_1(c, named, &tuple))
	{
		txn_memtable_Successor_drop(successor);
		return false;
	}
	if (!doltgres_rewrite_carrying_closure_2(c, named, &prefix))
	{
		prefix.ptr = key->ptr;
		prefix.len = at;
	}
	std_slice_to_vec_Checked(&renamed, prefix.ptr, prefix.len, DOLTGRES_REWRITE_BYTES);
	for (int shift = 56; shift >= 0; shift -= 8)
		std_vec_u8_push_Checked(&renamed, (uint8) (tuple.f0 >> shift));
	std_vec_free_Checked(key, DOLTGRES_REWRITE_BYTES);
	*key = renamed;
	return true;
}

/*
 * doltgres_rewrite_kept_insert puts a kept row in carrying's rows by its old
 * tuple id, in place of one it had, as a BTreeMap's insert does.
 */
static void
doltgres_rewrite_kept_insert(StdVec *kept, const doltgres_rewrite_kept_t *row)
{
	size_t		low = 0;
	size_t		high = kept->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		doltgres_rewrite_kept_t *at = &STD_VEC_AT(kept, doltgres_rewrite_kept_t, mid);

		if (at->old == row->old)
		{
			*at = *row;
			return;
		}
		if (at->old < row->old)
			low = mid + 1;
		else
			high = mid;
	}
	std_vec_insert(kept, low, row, DOLTGRES_REWRITE_KEPT_ROW, RUST_FILE, 609, 18);
}

/*
 * doltgres_rewrite_carrying_frame_t is carrying's state across its pad: the
 * kept rows, each key's kept rows, the versions carried so far, and the op
 * being carried, each while live.
 */
typedef struct doltgres_rewrite_carrying_frame_t
{
	doltgres_rewrite_carrying_t c;
	bool		kept_live;
	StdVec		rows;
	bool		rows_live;
	StdVec		carried;
	bool		carried_live;
	TxnMemtableOp op;
	bool		op_live;
	StdVec		blobs;
	bool		blobs_live;
	const DoltgresRewriteRead *read;
	const DoltgresRewriteRewritten *done;
	HostStorage storage;
	uint32		table;
	bool		has_toast;
	TxnRecordToast toast;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} doltgres_rewrite_carrying_frame_t;

/* doltgres_rewrite_Rows_drop is a key's kept rows' drop glue, an element of carrying's rows. */
static void
doltgres_rewrite_Rows_drop(void *rows)
{
	std_vec_free_Checked(rows, DOLTGRES_REWRITE_ROW_ENDS);
}

/* doltgres_rewrite_carrying_pad frees what carrying holds. */
static void
doltgres_rewrite_carrying_pad(void *frame_)
{
	doltgres_rewrite_carrying_frame_t *frame = frame_;

	if (frame->blobs_live)
		std_vec_free_Checked(&frame->blobs, DOLTGRES_REWRITE_BLOB);
	if (frame->op_live)
		txn_memtable_Op_drop(&frame->op);
	if (frame->carried_live)
		std_vec_drop_Checked(&frame->carried, DOLTGRES_REWRITE_CARRY, doltgres_rewrite_Carry_drop);
	if (frame->rows_live)
		std_vec_drop_Checked(&frame->rows, DOLTGRES_REWRITE_ROWS, doltgres_rewrite_Rows_drop);
	if (frame->kept_live)
		std_vec_free_Checked(&frame->c.kept, DOLTGRES_REWRITE_KEPT_ROW);
	frame->blobs_live = frame->op_live = frame->carried_live = frame->rows_live = frame->kept_live = false;
}

/*
 * doltgres_rewrite_carrying_carry turns a version's op into the op its kept
 * rows carry, false for none, taking the op.
 */
static bool
doltgres_rewrite_carrying_carry(const doltgres_rewrite_carrying_t *c, TxnMemtableOp *op, bool ended, bool locked)
{
	HostRow		row;

	switch (op->tag)
	{
		case TXN_MEMTABLE_OP_PUT:
			if (doltgres_rewrite_carrying_closure_4(c, &op->u.put))
				return true;
			op->tag = TXN_MEMTABLE_OP_DELETE;
			return ended;
		case TXN_MEMTABLE_OP_DELETE:
			return ended;
		case TXN_MEMTABLE_OP_ADD:
		case TXN_MEMTABLE_OP_ADD_ROW:
			if (doltgres_rewrite_carrying_closure_4(c, &op->u.put))
				return true;
			op->tag = TXN_MEMTABLE_OP_DELETE;
			return false;
		case TXN_MEMTABLE_OP_UPDATE_ROW:
			{
				HostRow		replaced = op->u.update_row.f0;
				TxnMemtablePut put = op->u.update_row.f1;
				bool		moved = doltgres_rewrite_carrying_closure_4(c, &put);
				bool		renewed = doltgres_rewrite_carrying_closure_1(c, replaced, &row);

				if (moved && renewed)
				{
					op->u.update_row.f0 = row;
					op->u.update_row.f1 = put;
				}
				else if (moved)
				{
					op->tag = c->keyless ? TXN_MEMTABLE_OP_ADD : TXN_MEMTABLE_OP_ADD_ROW;
					op->u.put = put;
				}
				else if (renewed && c->keyless)
				{
					op->tag = TXN_MEMTABLE_OP_REMOVE;
					op->u.remove.f0 = row;
					op->u.remove.has_f1 = false;
				}
				else if (renewed)
				{
					op->tag = TXN_MEMTABLE_OP_DELETE_ROW;
					op->u.delete_row.f0 = row;
					op->u.delete_row.has_f1 = false;
				}
				else
					op->tag = TXN_MEMTABLE_OP_DELETE;
				return moved || renewed;
			}
		case TXN_MEMTABLE_OP_MOVE:
			if (!ended)
				return false;
			if (!doltgres_rewrite_carrying_closure_5(c, &op->u.move))
				op->tag = TXN_MEMTABLE_OP_DELETE;
			return true;
		case TXN_MEMTABLE_OP_REMOVE:
			if (!doltgres_rewrite_carrying_closure_1(c, op->u.remove.f0, &op->u.remove.f0))
				return false;
			if (op->u.remove.has_f1)
				op->u.remove.has_f1 = doltgres_rewrite_carrying_closure_5(c, &op->u.remove.f1);
			return true;
		case TXN_MEMTABLE_OP_DELETE_ROW:
			if (!doltgres_rewrite_carrying_closure_1(c, op->u.delete_row.f0, &op->u.delete_row.f0))
				return false;
			if (op->u.delete_row.has_f1)
				op->u.delete_row.has_f1 = doltgres_rewrite_carrying_closure_5(c, &op->u.delete_row.f1);
			return true;
		case TXN_MEMTABLE_OP_LOCK:
			return locked;
		case TXN_MEMTABLE_OP_LOCK_ROW:
			return doltgres_rewrite_carrying_closure_1(c, op->u.lock_row.f0, &op->u.lock_row.f0);
		case TXN_MEMTABLE_OP_EMPTIED:
		case TXN_MEMTABLE_OP_NAMED:
			return false;
	}
	return false;
}

/* doltgres_rewrite_carrying_body carries each kept row's versions, each key's oldest first. */
static void
doltgres_rewrite_carrying_body(void *frame_)
{
	doltgres_rewrite_carrying_frame_t *frame = frame_;
	const DoltgresRewriteRead *read = frame->read;
	const DoltgresRewriteRewritten *done = frame->done;

	std_vec_new(&frame->c.kept, DOLTGRES_REWRITE_KEPT_ROW);
	frame->kept_live = true;
	std_vec_with_capacity_Checked(&frame->rows, read->keys.len, DOLTGRES_REWRITE_ROWS);
	frame->rows_live = true;
	for (size_t i = 0; i < read->keys.len; i++)
	{
		StdVec		rows;

		std_vec_new(&rows, DOLTGRES_REWRITE_ROW_ENDS);
		std_vec_push_Checked(&frame->rows, &rows, DOLTGRES_REWRITE_ROWS);
	}
	for (size_t at = 0; at < read->olds.len; at++)
	{
		const DoltgresRewriteOld *old = &STD_VEC_AT(&read->olds, DoltgresRewriteOld, at);
		doltgres_OptionU64_t new_ = STD_VEC_AT(&done->new_, doltgres_OptionU64_t, at);
		const doltgres_OptionFormed_t *formed = &STD_VEC_AT(&done->formed, doltgres_OptionFormed_t, at);
		const doltgres_OptionVecU32_t *ids = &STD_VEC_AT(&done->ids, doltgres_OptionVecU32_t, at);
		doltgres_rewrite_kept_t kept = {old->tuple, new_.f0, STD_VEC_AT(&done->bytes, uint32, at),
		formed->has ? &formed->f0 : NULL, ids->has ? &ids->f0 : NULL};
		doltgres_rewrite_row_ends_t ends = {old->from.tag == DOLTGRES_REWRITE_FROM_DELTA, old->from.place,
		old->has_ender, old->ender};

		if (!new_.has)
			continue;
		doltgres_rewrite_kept_insert(&frame->c.kept, &kept);
		std_vec_push_Checked(&STD_VEC_AT(&frame->rows, StdVec, old->key), &ends, DOLTGRES_REWRITE_ROW_ENDS);
	}
	std_vec_new(&frame->carried, DOLTGRES_REWRITE_CARRY);
	frame->carried_live = true;
	for (size_t at = 0; at < read->keys.len; at++)
	{
		const DoltgresRewriteKey *key = &STD_VEC_AT(&read->keys, DoltgresRewriteKey, at);
		const StdVec *rows = &STD_VEC_AT(&frame->rows, StdVec, at);

		for (size_t place = key->versions.len; place-- > 0;)
		{
			const doltgres_TupleVecU8Bool_t *version = &STD_VEC_AT(&key->versions, doltgres_TupleVecU8Bool_t, place);
			TxnMemtableVersionRecord record;
			TxnMemtableVersionOpRef ref;
			TxnMemtableLockMode lock;
			const StdVec *values = NULL;
			DoltgresRewriteCarry carry = {0};
			HostRow		row = {0};
			StdBytes	rekeyed;
			bool		has_row = true;
			bool		ended = false;
			bool		locked = false;

			if (version->f1)
				continue;
			if (!txn_memtable_version_stored(version->f0.ptr, version->f0.len, &record, frame->error))
				return;
			for (size_t k = 0; k < rows->len; k++)
			{
				const doltgres_rewrite_row_ends_t *ends = &STD_VEC_AT(rows, doltgres_rewrite_row_ends_t, k);

				ended = ended || (ends->has_ender && ends->ender == place);
				locked = locked || ((!ends->has_row || ends->row > place) && (!ends->has_ender || ends->ender < place));
			}
			ref = txn_memtable_version_Record_op(&record);
			switch (ref.tag)
			{
				case TXN_MEMTABLE_VERSION_OP_REF_PUT:
				case TXN_MEMTABLE_VERSION_OP_REF_ADD:
				case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
					row = ref.u.put.tuple;
					break;
				case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
					row = ref.u.update_row.f0;
					break;
				case TXN_MEMTABLE_VERSION_OP_REF_REMOVE:
					row = ref.u.remove.f0;
					break;
				case TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW:
					row = ref.u.delete_row.f0;
					break;
				case TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW:
					row = ref.u.lock_row.f0;
					break;
				default:
					has_row = false;
					break;
			}
			frame->op = txn_memtable_version_OpRef_owned(&ref);
			frame->op_live = true;
			if (!doltgres_rewrite_carrying_carry(&frame->c, &frame->op, ended, locked))
			{
				frame->op_live = false;
				txn_memtable_Op_drop(&frame->op);
				continue;
			}
			lock = txn_memtable_version_Record_lock(&record);
			switch (frame->op.tag)
			{
				case TXN_MEMTABLE_OP_PUT:
				case TXN_MEMTABLE_OP_ADD:
				case TXN_MEMTABLE_OP_ADD_ROW:
					carry.f0.heap_bytes = frame->op.u.put.heap_bytes;
					values = &frame->op.u.put.values;
					break;
				case TXN_MEMTABLE_OP_UPDATE_ROW:
					carry.f0.heap_bytes = frame->op.u.update_row.f1.heap_bytes;
					carry.f0.has_replaced = true;
					carry.f0.replaced = frame->op.u.update_row.f0;
					values = &frame->op.u.update_row.f1.values;
					break;
				default:
					break;
			}
			std_vec_with_capacity_Checked(&frame->blobs, values != NULL ? values->len : 0, DOLTGRES_REWRITE_BLOB);
			frame->blobs_live = true;
			for (size_t k = 0; values != NULL && k < values->len; k++)
			{
				uint32		id = STD_VEC_AT(values, uint32, k);
				const txn_TupleU32TupleEntryU64_t *value = NULL;
				size_t		low = 0;
				size_t		high = done->values.len;
				chunks_TupleAddressU64_t blob;

				while (low < high && value == NULL)
				{
					size_t		mid = low + (high - low) / 2;
					const txn_TupleU32TupleEntryU64_t *probe = &STD_VEC_AT(&done->values,
																		   txn_TupleU32TupleEntryU64_t, mid);

					if (probe->f0 == id)
						value = probe;
					else if (probe->f0 < id)
						low = mid + 1;
					else
						high = mid;
				}
				if (value == NULL)
				{
					StdFmtPiece pieces[] = {STD_FMT_LIT("a carried row's value "), STD_FMT_ARG(0),
					STD_FMT_LIT(" is in no kept version's map entries")};
					StdFmtArg	args[] = {{&id, std_u32_Display_fmt}};
					StdFmtArguments arguments = {pieces, 3, args};
					StdString	message;

					std_fmt_format_Checked(&message, &arguments);
					*frame->error = host_Error_fork((const uint8 *) "XX000", &message);
					return;
				}
				blob.f0 = value->f1.f0.address;
				blob.f1 = value->f1.f0.stored;
				std_vec_push_Checked(&frame->blobs, &blob, DOLTGRES_REWRITE_BLOB);
			}
			carry.f0.writer = txn_memtable_version_Record_writer(&record);
			carry.f0.storage = frame->storage;
			carry.f0.table = frame->table;
			if (!has_row || !doltgres_rewrite_carrying_closure_2(&frame->c, row, &rekeyed))
			{
				rekeyed.ptr = key->key.ptr;
				rekeyed.len = key->key.len;
			}
			std_slice_to_vec_Checked(&carry.f0.key, rekeyed.ptr, rekeyed.len, DOLTGRES_REWRITE_BYTES);
			carry.f0.op = frame->op;
			frame->op_live = false;
			carry.f0.no_key = lock == TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE;
			carry.f0.has_toast = frame->blobs.len > 0 && frame->has_toast;
			carry.f0.toast = frame->toast;
			carry.f0.blobs = frame->blobs;
			frame->blobs_live = false;
			std_vec_new(&carry.f0.kept, DOLTGRES_REWRITE_USIZE);
			carry.f1 = txn_memtable_version_Record_shown(&record);
			carry.f2 = lock;
			std_vec_push_Checked(&frame->carried, &carry, DOLTGRES_REWRITE_CARRY);
		}
	}
	*frame->result = frame->carried;
	frame->carried_live = false;
	frame->ok = true;
}

/*
 * doltgres_rewrite_carrying returns the versions of the old delta the new
 * storage carries, each key's oldest first: every version whose writer
 * didn't abort that writes a kept row, with its new tuple id, the heap bytes
 * it was packed with, and its row re-formed when that differs, that ends a
 * kept row, its ender, or that locks a kept row its key's change hadn't
 * ended yet, with that row's new tuple id. A version that wrote a row the
 * rewrite dropped carries only its end of the kept rows it ended, as heap's
 * rewrite leaves an update's older version with no newer one, and an update
 * whose successor it dropped carries as a delete. A keyless row re-formed
 * under a new key takes the versions that write or name it there, and a
 * successor's key is its row's key with its new tuple id. A row with values
 * out of line names the new TOAST relation and each value's blob, which the
 * new value map's base holds, so its redo writes no map entry. A re-formed
 * row's key and tuple are cloned, since a base copy's re-formed key names
 * its versions' key too and the copy's entry keeps the row. toast is NULL
 * for none.
 */
bool
doltgres_rewrite_carrying(const DoltgresRewriteRead *read, const DoltgresRewriteRewritten *done, bool keyless,
						  HostStorage storage, uint32 table, const TxnRecordToast *toast, StdVec *result,
						  HostError *error)
{
	doltgres_rewrite_carrying_frame_t frame = {.c = {.keyless = keyless}, .read = read, .done = done,
	.storage = storage, .table = table, .has_toast = toast != NULL, .result = result,
	.error = error};

	if (toast != NULL)
		frame.toast = *toast;
	std_landing(&frame, doltgres_rewrite_carrying_body, doltgres_rewrite_carrying_pad);
	doltgres_rewrite_carrying_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_rewrite_moving_captures_t is moving's each's captures: the new
 * storage, its table, its index (has_index false for a table's storage), and
 * the versions carried so far.
 */
typedef struct doltgres_rewrite_moving_captures_t
{
	HostStorage storage;
	uint32		table;
	bool		has_index;
	uint32		index;
	StdVec	   *carried;
} doltgres_rewrite_moving_captures_t;

/*
 * doltgres_rewrite_moving_closure_0 is moving's each: a key's versions whose
 * writers didn't abort, oldest first, as they are.
 */
static bool
doltgres_rewrite_moving_closure_0(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, bool has_place,
								  size_t place, StdVec *versions, HostError *error)
{
	doltgres_rewrite_moving_captures_t *c = captures;

	for (size_t i = versions->len; i-- > 0;)
	{
		TxnMemtableVersionVersion *version = &STD_VEC_AT(versions, TxnMemtableVersionVersion, i);
		TxnMemtableVersionRecord record;
		TxnMemtableVersionOpRef ref;
		DoltgresRewriteCarry carry = {0};
		HostStatus	outcome;

		if (!txn_outcome_outcome(io, version, &outcome, error))
			return false;
		if (outcome == HOST_STATUS_ABORTED)
			continue;
		record = txn_memtable_version_Version_record(version);
		ref = txn_memtable_version_Record_op(&record);
		carry.f0.op = txn_memtable_version_OpRef_owned(&ref);
		switch (carry.f0.op.tag)
		{
			case TXN_MEMTABLE_OP_UPDATE_ROW:
				carry.f0.heap_bytes = carry.f0.op.u.update_row.f1.heap_bytes;
				carry.f0.has_replaced = true;
				carry.f0.replaced = carry.f0.op.u.update_row.f0;
				break;
			case TXN_MEMTABLE_OP_PUT:
			case TXN_MEMTABLE_OP_ADD:
			case TXN_MEMTABLE_OP_ADD_ROW:
				carry.f0.heap_bytes = carry.f0.op.u.put.heap_bytes;
				break;
			default:
				break;
		}
		carry.f2 = txn_memtable_version_Record_lock(&record);
		carry.f0.writer = txn_memtable_version_Record_writer(&record);
		carry.f0.storage = c->storage;
		carry.f0.table = c->table;
		std_slice_to_vec_Checked(&carry.f0.key, key, key_len, DOLTGRES_REWRITE_BYTES);
		carry.f0.no_key = carry.f2 == TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE;
		std_vec_new(&carry.f0.blobs, DOLTGRES_REWRITE_BLOB);
		std_vec_new(&carry.f0.kept, DOLTGRES_REWRITE_USIZE);
		carry.f0.has_index = c->has_index;
		carry.f0.index = c->index;
		carry.f1 = txn_memtable_version_Record_shown(&record);
		if (!std_vec_try_reserve(c->carried, 1, DOLTGRES_REWRITE_CARRY))
		{
			txn_record_RowChange_drop(&carry.f0);
			*error = host_Error_out_of_memory();
			return false;
		}
		std_vec_push_Checked(c->carried, &carry, DOLTGRES_REWRITE_CARRY);
	}
	return true;
}

/* doltgres_rewrite_moving_frame_t is moving's state across its pad: the versions carried so far, while live. */
typedef struct doltgres_rewrite_moving_frame_t
{
	StdVec		carried;
	bool		carried_live;
	doltgres_rewrite_moving_captures_t captures;
	HostIoIo   *io;
	const HostSharedHeld *held;
	uint64		folded;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} doltgres_rewrite_moving_frame_t;

/* doltgres_rewrite_moving_pad frees what moving holds. */
static void
doltgres_rewrite_moving_pad(void *frame_)
{
	doltgres_rewrite_moving_frame_t *frame = frame_;

	if (frame->carried_live)
		std_vec_drop_Checked(&frame->carried, DOLTGRES_REWRITE_CARRY, doltgres_rewrite_Carry_drop);
	frame->carried_live = false;
}

/* doltgres_rewrite_moving_body hands each key's versions to moving's each. */
static void
doltgres_rewrite_moving_body(void *frame_)
{
	doltgres_rewrite_moving_frame_t *frame = frame_;

	std_vec_new(&frame->carried, DOLTGRES_REWRITE_CARRY);
	frame->carried_live = true;
	frame->captures.carried = &frame->carried;
	if (!txn_rewrite_keys(frame->io, frame->held, NULL, 0, frame->folded, doltgres_rewrite_moving_closure_0,
						  &frame->captures, NULL, frame->error))
		return;
	*frame->result = frame->carried;
	frame->carried_live = false;
	frame->ok = true;
}

/*
 * doltgres_rewrite_moving returns the versions of a storage's delta a move
 * carries into its new storage, each key's oldest first: every version the
 * copied base doesn't apply whose writer didn't abort, as it is, under its
 * own writer, with its tuple id, the command it showed, and the row lock it
 * held, named for the new storage and, for an index's storage, the index
 * (has_index false for none), its values' blobs not yet filled in.
 */
bool
doltgres_rewrite_moving(HostIoIo *io, const HostSharedHeld *held, uint64 folded, HostStorage storage, uint32 table,
						bool has_index, uint32 index, StdVec *result, HostError *error)
{
	doltgres_rewrite_moving_frame_t frame = {.captures = {.storage = storage, .table = table,
	.has_index = has_index, .index = index}, .io = io, .held = held, .folded = folded, .result = result,
	.error = error};

	std_landing(&frame, doltgres_rewrite_moving_body, doltgres_rewrite_moving_pad);
	doltgres_rewrite_moving_pad(&frame);
	return frame.ok;
}

#endif							/* FRONTEND */
