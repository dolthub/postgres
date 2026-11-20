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
 * The rewrite of a table under its new primary key (doltgres' rekey.rs, the
 * rewrite text's section 6, its §8 to §10): every row version of the old
 * storage re-formed under the new key, keeping its tuple id and its writer,
 * each new key's versions ordered by when their changes became visible and
 * written as one chain, or as rows named by their tuple ids where their
 * lifetimes overlap.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/doltgres/rekey.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/txn/counted.h"
#include "doltgres/txn/status.h"
#include "pg_calls.h"

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "rekey.rs"

/*
 * DoltgresRekeyVisible is Visible: when a version's change became visible,
 * in the order a key's versions are pushed (PRevPF 8's rulings on the text's
 * §10): a frozen writer's first, then the base's rows, then committed
 * writers' by their top-level transaction's commit record's end, then the
 * rewriting transaction's own; each then by its command and its old chain
 * order.
 */
typedef struct DoltgresRekeyVisible
{
	uint8		f0;
	uint64		f1;
	uint32		f2;
	uint64		f3;
} DoltgresRekeyVisible;

/* DoltgresRekeyChange is Change: a version that writes or ends a row, its writer, the command it showed, and its row lock. */
typedef struct DoltgresRekeyChange
{
	TxnMemtableVersionWriter writer;
	TxnMemtableVersionShown shown;
	TxnMemtableLockMode lock;
} DoltgresRekeyChange;

/*
 * doltgres_TupleVisibleChangeOptionU64_t is a (Visible, Change, Option<u64>):
 * a row's end, when it became visible, the change that ended it, and the
 * tuple id an update moved it to.
 */
typedef struct doltgres_TupleVisibleChangeOptionU64_t
{
	DoltgresRekeyVisible f0;
	DoltgresRekeyChange f1;
	bool		has_f2;
	uint64		f2;
} doltgres_TupleVisibleChangeOptionU64_t;

/*
 * DoltgresRekeyMoved is Moved: an old row version under the new key, its
 * tuple id, its writer's xid as its stamp holds it, its row re-formed under
 * the new key, whose key is its new key, the ids of its values out of line,
 * when it became visible, the change that wrote it, none for a base row, and
 * its end.
 */
typedef struct DoltgresRekeyMoved
{
	uint64		tuple;
	uint64		xmin;
	AccessHeaptupleFormed formed;
	/* ids is a Vec<u32>. */
	StdVec		ids;
	DoltgresRekeyVisible start;
	bool		has_wrote;
	DoltgresRekeyChange wrote;
	bool		has_end;
	doltgres_TupleVisibleChangeOptionU64_t end;
} DoltgresRekeyMoved;

/* doltgres_TupleBytesVecUsize_t is a (&[u8], Vec<usize>): a new key and the places of its rows among the moved. */
typedef struct doltgres_TupleBytesVecUsize_t
{
	StdBytes	f0;
	StdVec		f1;
} doltgres_TupleBytesVecUsize_t;

/* doltgres_TupleU64Usize_t is a (u64, usize): a tuple id and its row's place among the moved. */
typedef struct doltgres_TupleU64Usize_t
{
	uint64		f0;
	size_t		f1;
} doltgres_TupleU64Usize_t;

/* doltgres_TupleVisibleBool_t is a (Visible, bool): when an event became visible, and whether it starts its row. */
typedef struct doltgres_TupleVisibleBool_t
{
	DoltgresRekeyVisible f0;
	bool		f1;
} doltgres_TupleVisibleBool_t;

/* doltgres_TupleVisibleBoolUsize_t is a (Visible, bool, usize): an event, its start or end of the row at its place. */
typedef struct doltgres_TupleVisibleBoolUsize_t
{
	DoltgresRekeyVisible f0;
	bool		f1;
	size_t		f2;
} doltgres_TupleVisibleBoolUsize_t;

/* The element layouts of the rewrite's vectors. */
#define DOLTGRES_REKEY_BYTES 1, 1
#define DOLTGRES_REKEY_U32 sizeof(uint32), _Alignof(uint32)
#define DOLTGRES_REKEY_USIZE sizeof(size_t), _Alignof(size_t)
#define DOLTGRES_REKEY_ADDRESS sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t)
#define DOLTGRES_REKEY_STAMP sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp)
#define DOLTGRES_REKEY_ENTRY sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry)
#define DOLTGRES_REKEY_VALUE sizeof(txn_TupleU32TupleEntryU64_t), _Alignof(txn_TupleU32TupleEntryU64_t)
#define DOLTGRES_REKEY_CARRIED \
	sizeof(doltgres_TupleVecU8OpWriterShownLockMode_t), _Alignof(doltgres_TupleVecU8OpWriterShownLockMode_t)
#define DOLTGRES_REKEY_MOVED sizeof(DoltgresRekeyMoved), _Alignof(DoltgresRekeyMoved)
#define DOLTGRES_REKEY_KEY sizeof(doltgres_TupleBytesVecUsize_t), _Alignof(doltgres_TupleBytesVecUsize_t)
#define DOLTGRES_REKEY_BY_TUPLE sizeof(doltgres_TupleU64Usize_t), _Alignof(doltgres_TupleU64Usize_t)
#define DOLTGRES_REKEY_EVENT sizeof(doltgres_TupleVisibleBoolUsize_t), _Alignof(doltgres_TupleVisibleBoolUsize_t)

/* doltgres_rekey_Visible_lt is Visible's tuple order, its fields compared in turn. */
static bool
doltgres_rekey_Visible_lt(const DoltgresRekeyVisible *a, const DoltgresRekeyVisible *b)
{
	if (a->f0 != b->f0)
		return a->f0 < b->f0;
	if (a->f1 != b->f1)
		return a->f1 < b->f1;
	if (a->f2 != b->f2)
		return a->f2 < b->f2;
	return a->f3 < b->f3;
}

/* doltgres_rekey_TupleVisibleBool_lt is a (Visible, bool)'s tuple order, false before true. */
static bool
doltgres_rekey_TupleVisibleBool_lt(const doltgres_TupleVisibleBool_t *a, const doltgres_TupleVisibleBool_t *b)
{
	if (doltgres_rekey_Visible_lt(&a->f0, &b->f0))
		return true;
	if (doltgres_rekey_Visible_lt(&b->f0, &a->f0))
		return false;
	return !a->f1 && b->f1;
}

/* <[usize]>::sort_by_key at Visible, which rekeyed orders each new key's rows by. */
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
#define STD_STABLE_SORT_KEY_KS Visible
#define STD_STABLE_SORT_KEY_K DoltgresRekeyVisible
#define STD_STABLE_SORT_KEY_LT(a, b) doltgres_rekey_Visible_lt((a), (b))
#include "doltgres/std/stable_sort_template.h"

/* <[(Visible, bool, usize)]>::sort_by_key at (Visible, bool), which laid orders a key's events by. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS TupleVisibleBoolUsize
#define STD_SORT_T doltgres_TupleVisibleBoolUsize_t
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS TupleVisibleBoolUsize
#define STD_STABLE_SORT_T doltgres_TupleVisibleBoolUsize_t
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY_KEY
#define STD_STABLE_SORT_KEY_KS TupleVisibleBool
#define STD_STABLE_SORT_KEY_K doltgres_TupleVisibleBool_t
#define STD_STABLE_SORT_KEY_LT(a, b) doltgres_rekey_TupleVisibleBool_lt((a), (b))
#include "doltgres/std/stable_sort_template.h"

/* doltgres_rekey_Entry_drop is Entry's drop glue, an element of Rekeyed's rows and tuple ids. */
static void
doltgres_rekey_Entry_drop(void *entry)
{
	trees_build_Entry_drop(entry);
}

/* doltgres_rekey_Carried_drop is a carried version's drop glue: its key and its op. */
static void
doltgres_rekey_Carried_drop(void *carried)
{
	doltgres_TupleVecU8OpWriterShownLockMode_t *self = carried;

	std_vec_free_Checked(&self->f0, DOLTGRES_REKEY_BYTES);
	txn_memtable_Op_drop(&self->f1);
}

/* doltgres_rekey_Rekeyed_drop is Rekeyed's drop glue: its entries, its pairs, its carried versions, and its values. */
void
doltgres_rekey_Rekeyed_drop(DoltgresRekeyRekeyed *self)
{
	std_vec_drop_Checked(&self->rows, DOLTGRES_REKEY_ENTRY, doltgres_rekey_Entry_drop);
	std_vec_drop_Checked(&self->tuple_ids, DOLTGRES_REKEY_ENTRY, doltgres_rekey_Entry_drop);
	std_vec_drop_Checked(&self->carried, DOLTGRES_REKEY_CARRIED, doltgres_rekey_Carried_drop);
	std_vec_free_Checked(&self->values, DOLTGRES_REKEY_VALUE);
}

/* doltgres_rekey_Moved_drop is Moved's drop glue: its row re-formed and its value ids. */
static void
doltgres_rekey_Moved_drop(void *moved)
{
	DoltgresRekeyMoved *self = moved;

	access_heaptuple_Formed_drop(&self->formed);
	std_vec_free_Checked(&self->ids, DOLTGRES_REKEY_U32);
}

/* doltgres_rekey_Key_drop is a (&[u8], Vec<usize>)'s drop glue: its places. */
static void
doltgres_rekey_Key_drop(void *key)
{
	std_vec_free_Checked(&((doltgres_TupleBytesVecUsize_t *) key)->f1, DOLTGRES_REKEY_USIZE);
}

/* doltgres_rekey_change returns the change a stored version makes. */
static bool
doltgres_rekey_change(const uint8 *bytes, size_t bytes_len, DoltgresRekeyChange *result, PgPgError *error)
{
	TxnMemtableVersionRecord record;
	HostError	failed;

	if (!txn_memtable_version_stored(bytes, bytes_len, &record, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	result->writer = txn_memtable_version_Record_writer(&record);
	result->shown = txn_memtable_version_Record_shown(&record);
	result->lock = txn_memtable_version_Record_lock(&record);
	return true;
}

/*
 * doltgres_rekey_visible returns when a stored version's change became
 * visible: a frozen writer's first, a committed writer's at its top-level
 * transaction's commit record's end, and the rewriting transaction's own
 * last, each by its command and its old chain order.
 */
static bool
doltgres_rekey_visible(HostIoIo *io, const uint8 *bytes, size_t bytes_len, uint64 chained,
					   DoltgresRekeyVisible *result, PgPgError *error)
{
	TxnMemtableVersionRecord held;
	TxnMemtableVersionWriter writer;
	HostXid		xid;
	HostXid		top;
	HostStatus	status;
	TxnStatusCommit commit;
	HostError	failed;

	if (!txn_memtable_version_stored(bytes, bytes_len, &held, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	writer = txn_memtable_version_Record_writer(&held);
	top.f0 = writer.has_top ? writer.top : writer.xid;
	result->f2 = writer.command;
	result->f3 = chained;
	if (top.f0 < TXN_STATUS_FIRST_NORMAL)
	{
		result->f0 = 0;
		result->f1 = 0;
		return true;
	}
	xid.f0 = writer.xid;
	if (!host_status(HOST_IO_IO_IO_DOORS(io), xid, &status, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	if (status == HOST_STATUS_IN_PROGRESS)
	{
		result->f0 = 3;
		result->f1 = 0;
		return true;
	}
	if (!txn_status_committed(io, top, &commit, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	if (commit.tag == TXN_STATUS_COMMIT_AT)
	{
		result->f0 = 2;
		result->f1 = commit.u.at;
		return true;
	}
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("committed transaction "), STD_FMT_ARG(0),
		STD_FMT_LIT(" has no commit status entry")};
		StdFmtArg	args[] = {{&top.f0, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*error = pg_PgError_fork((const uint8 *) "XX001", &message);
		return false;
	}
}

/*
 * doltgres_rekey_put returns the put of a moved row under its new key; its
 * tuple, addresses, and value ids are cloned, since the row keeps its own
 * for the key's other versions and its base entry.
 */
static TxnMemtablePut
doltgres_rekey_put(const DoltgresRekeyMoved *row)
{
	TxnMemtablePut put = {.tuple = {row->tuple}, .heap_bytes = (uint32) row->formed.heap_bytes};

	std_slice_to_vec_Checked(&put.value, row->formed.tuple.ptr, row->formed.tuple.len, DOLTGRES_REKEY_BYTES);
	std_slice_to_vec_Checked(&put.addresses, row->formed.addresses.ptr, row->formed.addresses.len,
							 DOLTGRES_REKEY_ADDRESS);
	std_slice_to_vec_Checked(&put.values, row->ids.ptr, row->ids.len, DOLTGRES_REKEY_U32);
	return put;
}

/* doltgres_rekey_successor returns the successor key of a tuple id an update moved a row to, if a moved row has it. */
static bool
doltgres_rekey_successor(const StdVec *moved, const StdVec *by_tuple, uint64 next, TxnMemtableSuccessor *result)
{
	size_t		low = 0;
	size_t		high = by_tuple->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		const doltgres_TupleU64Usize_t *at = &STD_VEC_AT(by_tuple, doltgres_TupleU64Usize_t, mid);

		if (at->f0 == next)
		{
			const DoltgresRekeyMoved *row = &STD_VEC_AT(moved, DoltgresRekeyMoved, at->f1);
			uint8		bytes[8];

			for (int i = 0; i < 8; i++)
				bytes[i] = (uint8) (next >> (56 - 8 * i));
			result->tag = TXN_MEMTABLE_SUCCESSOR_KEY;
			std_vec_with_capacity_Checked(&result->u.key, row->formed.key.len + 8, DOLTGRES_REKEY_BYTES);
			std_vec_extend_from_slice_Checked(&result->u.key, row->formed.key.ptr, row->formed.key.len,
											  DOLTGRES_REKEY_BYTES);
			std_vec_extend_from_slice_Checked(&result->u.key, bytes, 8, DOLTGRES_REKEY_BYTES);
			return true;
		}
		if (at->f0 < next)
			low = mid + 1;
		else
			high = mid;
	}
	return false;
}

/* doltgres_rekey_carry pushes a carried version of a key, an op it takes, and a change. */
static void
doltgres_rekey_carry(DoltgresRekeyRekeyed *done, const uint8 *key, size_t key_len, TxnMemtableOp *op,
					 const DoltgresRekeyChange *change)
{
	doltgres_TupleVecU8OpWriterShownLockMode_t carried = {.f1 = *op, .f2 = change->writer, .f3 = change->shown,
	.f4 = change->lock};

	std_slice_to_vec_Checked(&carried.f0, key, key_len, DOLTGRES_REKEY_BYTES);
	std_vec_push_Checked(&done->carried, &carried, DOLTGRES_REKEY_CARRIED);
}

/* doltgres_rekey_laid_closure_4 is laid's |&(visible, starts, _)| (visible, starts), its events' sort key. */
static doltgres_TupleVisibleBool_t
doltgres_rekey_laid_closure_4(void *captures, const void *event)
{
	const doltgres_TupleVisibleBoolUsize_t *self = event;
	doltgres_TupleVisibleBool_t key = {self->f0, self->f1};

	return key;
}

/*
 * doltgres_rekey_laid_frame_t is laid's state across its pad: its events and
 * the rows the base keeps, while live.
 */
typedef struct doltgres_rekey_laid_frame_t
{
	const StdVec *moved;
	const StdVec *by_tuple;
	StdBytes	key;
	const StdVec *rows;
	bool		keyless;
	bool		overlapping;
	DoltgresRekeyRekeyed *done;
	StdVec		events;
	bool		events_live;
	StdVec		based;
	bool		based_live;
} doltgres_rekey_laid_frame_t;

/* doltgres_rekey_laid_pad frees laid's events and the rows the base keeps. */
static void
doltgres_rekey_laid_pad(void *frame_)
{
	doltgres_rekey_laid_frame_t *frame = frame_;

	if (frame->events_live)
		std_vec_free_Checked(&frame->events, DOLTGRES_REKEY_EVENT);
	if (frame->based_live)
		std_vec_free_Checked(&frame->based, DOLTGRES_REKEY_USIZE);
	frame->events_live = frame->based_live = false;
}

/* doltgres_rekey_ended pushes a row's end, unless its successor is a row of its own key that a chain carries. */
static void
doltgres_rekey_ended(doltgres_rekey_laid_frame_t *frame, const DoltgresRekeyMoved *row)
{
	TxnMemtableOp op;
	TxnMemtableSuccessor moved_to;
	bool		has_moved_to;

	if (!frame->keyless && row->end.has_f2)
		for (size_t i = 0; i < frame->rows->len; i++)
			if (STD_VEC_AT(frame->moved, DoltgresRekeyMoved, STD_VEC_AT(frame->rows, size_t, i)).tuple == row->end.f2)
				return;
	has_moved_to = row->end.has_f2 && doltgres_rekey_successor(frame->moved, frame->by_tuple, row->end.f2, &moved_to);
	if (frame->keyless)
	{
		op.tag = TXN_MEMTABLE_OP_REMOVE;
		op.u.remove.f0.f0 = row->tuple;
		op.u.remove.has_f1 = has_moved_to;
		if (has_moved_to)
			op.u.remove.f1 = moved_to;
	}
	else if (frame->overlapping)
	{
		op.tag = TXN_MEMTABLE_OP_DELETE_ROW;
		op.u.delete_row.f0.f0 = row->tuple;
		op.u.delete_row.has_f1 = has_moved_to;
		if (has_moved_to)
			op.u.delete_row.f1 = moved_to;
	}
	else if (has_moved_to)
	{
		op.tag = TXN_MEMTABLE_OP_MOVE;
		op.u.move = moved_to;
	}
	else
		op.tag = TXN_MEMTABLE_OP_DELETE;
	doltgres_rekey_carry(frame->done, frame->key.ptr, frame->key.len, &op, &row->end.f1);
}

/* doltgres_rekey_started pushes the version that writes a row, or keeps the row for the base when no version does. */
static void
doltgres_rekey_started(doltgres_rekey_laid_frame_t *frame, size_t at, const DoltgresRekeyMoved *row)
{
	TxnMemtableOp op;
	bool		has_before = false;
	size_t		before = 0;

	for (size_t i = 0; !frame->keyless && i < frame->rows->len; i++)
	{
		const DoltgresRekeyMoved *candidate = &STD_VEC_AT(frame->moved, DoltgresRekeyMoved,
														  STD_VEC_AT(frame->rows, size_t, i));

		if (candidate->has_end && candidate->end.has_f2 && candidate->end.f2 == row->tuple)
		{
			has_before = true;
			before = STD_VEC_AT(frame->rows, size_t, i);
			break;
		}
	}
	if (!row->has_wrote && (!frame->overlapping || frame->keyless))
	{
		std_vec_push_Checked(&frame->based, &at, DOLTGRES_REKEY_USIZE);
		return;
	}
	if (!row->has_wrote)
	{
		DoltgresRekeyChange frozen = {TXN_MEMTABLE_VERSION_NO_TRANSACTION,
		{.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}, TXN_MEMTABLE_LOCK_MODE_UPDATE};

		op.tag = TXN_MEMTABLE_OP_ADD_ROW;
		op.u.add_row = doltgres_rekey_put(row);
		doltgres_rekey_carry(frame->done, frame->key.ptr, frame->key.len, &op, &frozen);
		return;
	}
	if (frame->keyless)
	{
		op.tag = TXN_MEMTABLE_OP_ADD;
		op.u.add = doltgres_rekey_put(row);
	}
	else if (!frame->overlapping)
	{
		op.tag = TXN_MEMTABLE_OP_PUT;
		op.u.put = doltgres_rekey_put(row);
	}
	else if (has_before)
	{
		op.tag = TXN_MEMTABLE_OP_UPDATE_ROW;
		op.u.update_row.f0.f0 = STD_VEC_AT(frame->moved, DoltgresRekeyMoved, before).tuple;
		op.u.update_row.f1 = doltgres_rekey_put(row);
	}
	else
	{
		op.tag = TXN_MEMTABLE_OP_ADD_ROW;
		op.u.add_row = doltgres_rekey_put(row);
	}
	doltgres_rekey_carry(frame->done, frame->key.ptr, frame->key.len, &op, &row->wrote);
}

/*
 * doltgres_rekey_based writes the base's entry of a key's rows no version
 * wrote: one row, or a keyless key's counted copies, with each row's pair in
 * the tuple id tree.
 */
static void
doltgres_rekey_based(doltgres_rekey_laid_frame_t *frame)
{
	DoltgresRekeyRekeyed *done = frame->done;
	const DoltgresRekeyMoved *row = &STD_VEC_AT(frame->moved, DoltgresRekeyMoved, STD_VEC_AT(&frame->based, size_t, 0));
	uint64		count = frame->based.len;
	TreesBuildEntry entry;

	if (frame->keyless)
	{
		txn_counted_keyless_value(count, row->formed.tuple.ptr, row->formed.tuple.len, &entry.value);
		txn_counted_keyless_addresses(count, row->formed.addresses.ptr, row->formed.addresses.len, &entry.addresses);
	}
	else
	{
		std_slice_to_vec_Checked(&entry.value, row->formed.tuple.ptr, row->formed.tuple.len, DOLTGRES_REKEY_BYTES);
		std_slice_to_vec_Checked(&entry.addresses, row->formed.addresses.ptr, row->formed.addresses.len,
								 DOLTGRES_REKEY_ADDRESS);
	}
	std_vec_with_capacity_Checked(&entry.stamps, frame->based.len, DOLTGRES_REKEY_STAMP);
	for (size_t i = 0; i < frame->based.len; i++)
	{
		const DoltgresRekeyMoved *base_row = &STD_VEC_AT(frame->moved, DoltgresRekeyMoved,
														 STD_VEC_AT(&frame->based, size_t, i));
		ChunksTrailerStamp stamp = {base_row->xmin, base_row->tuple};
		uint8		tuple_id[6];
		StdVec		key;
		StdVec		pair;
		TreesBuildEntry paired;

		std_vec_push_Checked(&entry.stamps, &stamp, DOLTGRES_REKEY_STAMP);
		txn_base_pair_value(frame->key.ptr, frame->key.len, (uint32) base_row->formed.heap_bytes, &pair);
		txn_base_tuple_id_key(base_row->tuple, tuple_id);
		std_slice_to_vec_Checked(&key, tuple_id, sizeof(tuple_id), DOLTGRES_REKEY_BYTES);
		trees_build_Entry_new_VecU8VecU8(&key, &pair, &paired);
		std_vec_push_Checked(&done->tuple_ids, &paired, DOLTGRES_REKEY_ENTRY);
		done->sizes.heap_bytes = STD_ADD(u64, done->sizes.heap_bytes, base_row->formed.heap_bytes, 269, 13);
	}
	done->sizes.entries = STD_ADD(u64, done->sizes.entries, 1, 271, 9);
	done->sizes.leaf_bytes = STD_ADD(u64, done->sizes.leaf_bytes,
									 trees_chunker_leaf_entry_size(frame->key.ptr, frame->key.len, entry.value.ptr,
																   entry.value.len), 272, 9);
	done->sizes.key_bytes = STD_ADD(u64, done->sizes.key_bytes, frame->key.len, 273, 9);
	done->sizes.rows = STD_ADD(u64, done->sizes.rows, count, 274, 9);
	std_slice_to_vec_Checked(&entry.key, frame->key.ptr, frame->key.len, DOLTGRES_REKEY_BYTES);
	std_slice_to_vec_Checked(&entry.values, row->ids.ptr, row->ids.len, DOLTGRES_REKEY_U32);
	std_vec_push_Checked(&done->rows, &entry, DOLTGRES_REKEY_ENTRY);
}

/* doltgres_rekey_laid_body lays the key's rows out in the order their changes became visible. */
static void
doltgres_rekey_laid_body(void *frame_)
{
	doltgres_rekey_laid_frame_t *frame = frame_;

	std_vec_new(&frame->events, DOLTGRES_REKEY_EVENT);
	frame->events_live = true;
	for (size_t i = 0; i < frame->rows->len; i++)
	{
		size_t		at = STD_VEC_AT(frame->rows, size_t, i);
		const DoltgresRekeyMoved *row = &STD_VEC_AT(frame->moved, DoltgresRekeyMoved, at);
		doltgres_TupleVisibleBoolUsize_t start = {row->start, true, at};

		std_vec_push_Checked(&frame->events, &start, DOLTGRES_REKEY_EVENT);
		if (row->has_end)
		{
			doltgres_TupleVisibleBoolUsize_t end = {row->end.f0, false, at};

			std_vec_push_Checked(&frame->events, &end, DOLTGRES_REKEY_EVENT);
		}
	}
	std_slice_sort_by_key_TupleVisibleBoolUsizeTupleVisibleBool(frame->events.ptr, frame->events.len,
																doltgres_rekey_laid_closure_4, NULL, NULL);
	std_vec_new(&frame->based, DOLTGRES_REKEY_USIZE);
	frame->based_live = true;
	for (size_t i = 0; i < frame->events.len; i++)
	{
		const doltgres_TupleVisibleBoolUsize_t *event = &STD_VEC_AT(&frame->events, doltgres_TupleVisibleBoolUsize_t, i);
		const DoltgresRekeyMoved *row = &STD_VEC_AT(frame->moved, DoltgresRekeyMoved, event->f2);

		if (event->f1)
			doltgres_rekey_started(frame, event->f2, row);
		else if (row->has_end)
			doltgres_rekey_ended(frame, row);
	}
	if (frame->based.len > 0)
		doltgres_rekey_based(frame);
}

/* doltgres_rekey_laid lays one new key's rows out into the new storage, in the order their changes became visible. */
static void
doltgres_rekey_laid(const StdVec *moved, const StdVec *by_tuple, StdBytes key, const StdVec *rows, bool keyless,
					bool overlapping, DoltgresRekeyRekeyed *done)
{
	doltgres_rekey_laid_frame_t frame = {.moved = moved, .by_tuple = by_tuple, .key = key, .rows = rows,
	.keyless = keyless, .overlapping = overlapping, .done = done};

	std_landing(&frame, doltgres_rekey_laid_body, doltgres_rekey_laid_pad);
	doltgres_rekey_laid_pad(&frame);
}

/* doltgres_rekey_rekeyed_closure_3 is rekeyed's |&at| moved[at].start, a key's rows' sort key. */
static DoltgresRekeyVisible
doltgres_rekey_rekeyed_closure_3(void *captures, const void *at)
{
	const StdVec *moved = captures;

	return STD_VEC_AT(moved, DoltgresRekeyMoved, *(const size_t *) at).start;
}

/*
 * doltgres_rekey_rekeyed_frame_t is rekeyed's state across its pad: the
 * values counted, the rows moved, a row re-formed and its ids until moved
 * takes them, the keys and the tuple ids' places, and the new storage, each
 * while live.
 */
typedef struct doltgres_rekey_rekeyed_frame_t
{
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	DoltgresRewriteReformer *reformer;
	const DoltgresRewriteRead *read;
	bool		keyless;
	DoltgresRekeyRekeyed *result;
	PgPgError  *error;
	bool		ok;
	StdVec		values;
	bool		values_live;
	StdVec		moved;
	bool		moved_live;
	AccessHeaptupleFormed formed;
	bool		formed_live;
	StdVec		ids;
	bool		ids_live;
	StdVec		keys;
	bool		keys_live;
	StdVec		by_tuple;
	bool		by_tuple_live;
	DoltgresRekeyRekeyed done;
	bool		done_live;
} doltgres_rekey_rekeyed_frame_t;

/* doltgres_rekey_rekeyed_pad frees what rekeyed holds. */
static void
doltgres_rekey_rekeyed_pad(void *frame_)
{
	doltgres_rekey_rekeyed_frame_t *frame = frame_;

	if (frame->done_live)
		doltgres_rekey_Rekeyed_drop(&frame->done);
	if (frame->by_tuple_live)
		std_vec_free_Checked(&frame->by_tuple, DOLTGRES_REKEY_BY_TUPLE);
	if (frame->keys_live)
		std_vec_drop_Checked(&frame->keys, DOLTGRES_REKEY_KEY, doltgres_rekey_Key_drop);
	if (frame->ids_live)
		std_vec_free_Checked(&frame->ids, DOLTGRES_REKEY_U32);
	if (frame->formed_live)
		access_heaptuple_Formed_drop(&frame->formed);
	if (frame->moved_live)
		std_vec_drop_Checked(&frame->moved, DOLTGRES_REKEY_MOVED, doltgres_rekey_Moved_drop);
	if (frame->values_live)
		std_vec_free_Checked(&frame->values, DOLTGRES_REKEY_VALUE);
	frame->done_live = frame->by_tuple_live = frame->keys_live = frame->ids_live = frame->formed_live =
		frame->moved_live = frame->values_live = false;
}

/*
 * doltgres_rekey_ids_of returns a delta version's value ids, those its put
 * names, or none for any other op.
 */
static bool
doltgres_rekey_ids_of(const uint8 *bytes, size_t bytes_len, StdVec *result, PgPgError *error)
{
	TxnMemtableVersionRecord record;
	TxnMemtableVersionOpRef op;
	const TxnMemtableVersionPutRef *put;
	TxnMemtableVersionValues values;
	HostError	failed;
	uint32		id;

	if (!txn_memtable_version_stored(bytes, bytes_len, &record, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	std_vec_new(result, DOLTGRES_REKEY_U32);
	op = txn_memtable_version_Record_op(&record);
	switch (op.tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
			put = &op.u.put;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD:
			put = &op.u.add;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
			put = &op.u.add_row;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
			put = &op.u.update_row.f1;
			break;
		default:
			return true;
	}
	values = txn_memtable_version_PutRef_values(put);
	while (txn_memtable_version_Values_Iterator_next(&values, &id))
		std_vec_push_Checked(result, &id, DOLTGRES_REKEY_U32);
	return true;
}

/*
 * doltgres_rekey_moved re-forms an old row version under the new key, raising
 * 23502 for a live row with a NULL in a new key column, and pushes it among
 * the moved; read.based's value ids are cloned, since the read keeps its own.
 */
static bool
doltgres_rekey_moved(doltgres_rekey_rekeyed_frame_t *frame, size_t at)
{
	const DoltgresRewriteOld *old = &STD_VEC_AT(&frame->read->olds, DoltgresRewriteOld, at);
	const DoltgresRewriteKey *key = &STD_VEC_AT(&frame->read->keys, DoltgresRewriteKey, old->key);
	DoltgresScanRow stored;
	bool		has_ids;
	size_t		column;
	DoltgresRekeyMoved moved = {.tuple = old->tuple, .xmin = old->xmin};

	if (!doltgres_rewrite_Reformer_reformed(frame->reformer, frame->caps, at, &frame->values, &stored, &frame->formed,
											&has_ids, &frame->ids, frame->error))
		return false;
	doltgres_scan_Row_drop(&stored);
	frame->formed_live = true;
	frame->ids_live = has_ids;
	if ((old->vacuumed == TXN_VISIBLE_VACUUMED_LIVE || old->vacuumed == TXN_VISIBLE_VACUUMED_INSERT_IN_PROGRESS) &&
		doltgres_rewrite_Reformer_null_key(frame->reformer, &column) &&
		!pg_calls_key_null_column(pg_CatalogCaps_may_raise(frame->caps), pg_CatalogCaps_catalog(frame->caps),
								  doltgres_rewrite_Reformer_rel(frame->reformer), (int32) column, frame->error))
		return false;
	if (!has_ids)
	{
		if (old->from.tag == DOLTGRES_REWRITE_FROM_BASE)
		{
			const TxnRewriteBased *based = &STD_VEC_AT(&frame->read->based, TxnRewriteBased, old->from.place);

			std_slice_to_vec_Checked(&frame->ids, based->values.ptr, based->values.len, DOLTGRES_REKEY_U32);
		}
		else
		{
			const doltgres_TupleVecU8Bool_t *version = &STD_VEC_AT(&key->versions, doltgres_TupleVecU8Bool_t,
																   old->from.place);

			if (!doltgres_rekey_ids_of(version->f0.ptr, version->f0.len, &frame->ids, frame->error))
				return false;
		}
		frame->ids_live = true;
	}
	if (old->from.tag == DOLTGRES_REWRITE_FROM_BASE)
	{
		moved.start.f0 = 1;
	}
	else
	{
		const doltgres_TupleVecU8Bool_t *version = &STD_VEC_AT(&key->versions, doltgres_TupleVecU8Bool_t,
															   old->from.place);
		uint64		chained = ((uint64) old->key << 32) | (uint64) STD_SUB(usize, key->versions.len,
																		   old->from.place, 112, 66);

		if (!doltgres_rekey_change(version->f0.ptr, version->f0.len, &moved.wrote, frame->error) ||
			!doltgres_rekey_visible(frame->io, version->f0.ptr, version->f0.len, chained, &moved.start, frame->error))
			return false;
		moved.has_wrote = true;
	}
	if (old->has_ender)
	{
		const doltgres_TupleVecU8Bool_t *ender = &STD_VEC_AT(&key->versions, doltgres_TupleVecU8Bool_t, old->ender);
		uint64		chained = ((uint64) old->key << 32) | (uint64) STD_SUB(usize, key->versions.len, old->ender,
																		   112, 66);

		if (!doltgres_rekey_visible(frame->io, ender->f0.ptr, ender->f0.len, chained, &moved.end.f0, frame->error) ||
			!doltgres_rekey_change(ender->f0.ptr, ender->f0.len, &moved.end.f1, frame->error))
			return false;
		moved.has_end = true;
		moved.end.has_f2 = old->has_update;
		moved.end.f2 = old->update_next;
	}
	if (!std_vec_try_reserve(&frame->moved, 1, DOLTGRES_REKEY_MOVED))
	{
		StdString	message;

		std_string_from_Checked(&message, "out of memory", 13);
		*frame->error = pg_PgError_fork((const uint8 *) "53200", &message);
		return false;
	}
	moved.formed = frame->formed;
	moved.ids = frame->ids;
	frame->formed_live = frame->ids_live = false;
	std_vec_push_Checked(&frame->moved, &moved, DOLTGRES_REKEY_MOVED);
	return true;
}

/* doltgres_rekey_keyed puts a moved row's place under its new key, a sorted Vec as its BTreeMap's entry API keeps it. */
static void
doltgres_rekey_keyed(StdVec *keys, const uint8 *key, size_t key_len, size_t at)
{
	size_t		low = 0;
	size_t		high = keys->len;
	doltgres_TupleBytesVecUsize_t made = {{key, key_len}};

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		doltgres_TupleBytesVecUsize_t *probe = &STD_VEC_AT(keys, doltgres_TupleBytesVecUsize_t, mid);
		StdOrdering order = std_bytes_Ord_cmp(probe->f0.ptr, probe->f0.len, key, key_len);

		if (order == STD_ORDERING_EQUAL)
		{
			std_vec_push_Checked(&probe->f1, &at, DOLTGRES_REKEY_USIZE);
			return;
		}
		if (order == STD_ORDERING_LESS)
			low = mid + 1;
		else
			high = mid;
	}
	std_vec_new(&made.f1, DOLTGRES_REKEY_USIZE);
	std_vec_push_Checked(&made.f1, &at, DOLTGRES_REKEY_USIZE);
	std_vec_insert(keys, low, &made, DOLTGRES_REKEY_KEY, RUST_FILE, 133, 14);
}

/* doltgres_rekey_by_tuple puts a moved row's place under its tuple id, a later row's replacing an earlier's, as collect does. */
static void
doltgres_rekey_by_tuple(StdVec *by_tuple, uint64 tuple, size_t at)
{
	size_t		low = 0;
	size_t		high = by_tuple->len;
	doltgres_TupleU64Usize_t made = {tuple, at};

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		doltgres_TupleU64Usize_t *probe = &STD_VEC_AT(by_tuple, doltgres_TupleU64Usize_t, mid);

		if (probe->f0 == tuple)
		{
			probe->f1 = at;
			return;
		}
		if (probe->f0 < tuple)
			low = mid + 1;
		else
			high = mid;
	}
	std_vec_insert(by_tuple, low, &made, DOLTGRES_REKEY_BY_TUPLE, RUST_FILE, 135, 100);
}

/* doltgres_rekey_overlapping reports whether a key's rows, in order, overlap: a row ending after the next starts, or never. */
static bool
doltgres_rekey_overlapping(const StdVec *moved, const StdVec *rows)
{
	for (size_t i = 0; i + 1 < rows->len; i++)
	{
		const DoltgresRekeyMoved *row = &STD_VEC_AT(moved, DoltgresRekeyMoved, STD_VEC_AT(rows, size_t, i));
		const DoltgresRekeyMoved *next = &STD_VEC_AT(moved, DoltgresRekeyMoved, STD_VEC_AT(rows, size_t, i + 1));

		if (!row->has_end || doltgres_rekey_Visible_lt(&next->start, &row->end.f0))
			return true;
	}
	return false;
}

/* doltgres_rekey_rekeyed_body re-forms every row version, groups them by new key, and lays each key's out. */
static void
doltgres_rekey_rekeyed_body(void *frame_)
{
	doltgres_rekey_rekeyed_frame_t *frame = frame_;

	std_vec_new(&frame->values, DOLTGRES_REKEY_VALUE);
	frame->values_live = true;
	std_vec_with_capacity_Checked(&frame->moved, frame->read->olds.len, DOLTGRES_REKEY_MOVED);
	frame->moved_live = true;
	for (size_t at = 0; at < frame->read->olds.len; at++)
		if (!doltgres_rekey_moved(frame, at))
			return;
	std_vec_new(&frame->keys, DOLTGRES_REKEY_KEY);
	frame->keys_live = true;
	for (size_t at = 0; at < frame->moved.len; at++)
	{
		const DoltgresRekeyMoved *row = &STD_VEC_AT(&frame->moved, DoltgresRekeyMoved, at);

		doltgres_rekey_keyed(&frame->keys, row->formed.key.ptr, row->formed.key.len, at);
	}
	std_vec_new(&frame->by_tuple, DOLTGRES_REKEY_BY_TUPLE);
	frame->by_tuple_live = true;
	for (size_t at = 0; at < frame->moved.len; at++)
		doltgres_rekey_by_tuple(&frame->by_tuple, STD_VEC_AT(&frame->moved, DoltgresRekeyMoved, at).tuple, at);
	std_vec_new(&frame->done.rows, DOLTGRES_REKEY_ENTRY);
	std_vec_new(&frame->done.tuple_ids, DOLTGRES_REKEY_ENTRY);
	std_vec_new(&frame->done.carried, DOLTGRES_REKEY_CARRIED);
	std_vec_new(&frame->done.values, DOLTGRES_REKEY_VALUE);
	frame->done_live = true;
	for (size_t i = 0; i < frame->keys.len; i++)
	{
		doltgres_TupleBytesVecUsize_t *key = &STD_VEC_AT(&frame->keys, doltgres_TupleBytesVecUsize_t, i);

		std_slice_sort_by_key_UsizeVisible(key->f1.ptr, key->f1.len, doltgres_rekey_rekeyed_closure_3,
										   (void *) &frame->moved, NULL);
	}
	for (size_t i = 0; i < frame->keys.len; i++)
	{
		const doltgres_TupleBytesVecUsize_t *key = &STD_VEC_AT(&frame->keys, doltgres_TupleBytesVecUsize_t, i);
		bool		overlapping = !frame->keyless && doltgres_rekey_overlapping(&frame->moved, &key->f1);

		doltgres_rekey_laid(&frame->moved, &frame->by_tuple, key->f0, &key->f1, frame->keyless, overlapping,
							&frame->done);
	}
	std_vec_free_Checked(&frame->done.values, DOLTGRES_REKEY_VALUE);
	frame->done.values = frame->values;
	frame->values_live = false;
	*frame->result = frame->done;
	frame->done_live = false;
	frame->ok = true;
}

/*
 * doltgres_rekey_rekeyed re-forms every row version of the old storage under
 * the new key's columns and lays each new key's versions out (the text's
 * §10): a key whose rows never overlap as one chain, its row in the new base
 * when it is a base row, its delta rows as Puts and each end as the
 * successor's Put at the same key, a Move naming the successor's new key and
 * tuple id, or a Delete; a key whose rows overlap as rows named by their
 * tuple ids, a base row as an AddRow written frozen (Differences row 31),
 * each end an UpdateRow by the successor at the same key or a DeleteRow; and
 * a keyless table's rows as counted copies, ended by Remove. Before anything
 * is written, a live row with a NULL in a new key column raises 23502, as
 * Postgres' ALTER TABLE does; a row the rewriting transaction deleted isn't
 * live, since the table's lock leaves no other delete running, as Postgres'
 * check passes over it. Row locks carry nothing, since a lock vanishes with
 * its transaction and the rewriting transaction's own conflict with nothing.
 */
bool
doltgres_rekey_rekeyed(const PgCatalogCaps *caps, HostIoIo *io, DoltgresRewriteReformer *reformer,
					   const DoltgresRewriteRead *read, bool keyless, DoltgresRekeyRekeyed *result, PgPgError *error)
{
	doltgres_rekey_rekeyed_frame_t frame = {.caps = caps, .io = io, .reformer = reformer, .read = read,
	.keyless = keyless, .result = result, .error = error};

	std_landing(&frame, doltgres_rekey_rekeyed_body, doltgres_rekey_rekeyed_pad);
	doltgres_rekey_rekeyed_pad(&frame);
	return frame.ok;
}

#endif							/* FRONTEND */
