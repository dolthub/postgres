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
 * The C port of txn's memtable/slice.rs: a row's slice of its key's chain,
 * the versions that decide the row with one tuple id, read through a
 * backend's memo of the key's walk, so a fetch or a change of one row reads a
 * bounded number of versions however long the key's chain grows.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/delta.h"
#include "doltgres/host/local.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/index.h"
#include "doltgres/txn/memtable/range.h"
#include "doltgres/txn/memtable/slice.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/outcome.h"
#include "doltgres/txn/runs.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/visible.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "memtable" STD_SEP "slice.rs"

/*
 * TxnMemtableSliceTouch is Touch, what a version changes: the row it writes,
 * the row it names, whether it only locks, and whether it is an add, which
 * changes no older row.
 */
typedef struct TxnMemtableSliceTouch
{
	bool		has_row;
	uint64		row;
	bool		has_names;
	uint64		names;
	bool		lock;
	bool		add;
} TxnMemtableSliceTouch;

/* txn_memtable_slice_Touched_t is an element of Memo's touches: a version's handle and what it touches. */
typedef struct txn_memtable_slice_Touched_t
{
	TxnMemtableVersionHandle f0;
	TxnMemtableSliceTouch f1;
} txn_memtable_slice_Touched_t;

/* txn_memtable_slice_RowPlace_t is an entry of Memo's rows: a written row and its version's place. */
typedef struct txn_memtable_slice_RowPlace_t
{
	uint64		f0;
	size_t		f1;
} txn_memtable_slice_RowPlace_t;

/* txn_memtable_slice_Named_t is an entry of Memo's named: a row and the places of the versions naming it, a Vec<usize>. */
typedef struct txn_memtable_slice_Named_t
{
	uint64		f0;
	StdVec		f1;
} txn_memtable_slice_Named_t;

/*
 * TxnMemtableSliceMemo is Memo, a walk of a key's chain: the memtable and
 * range it was walked in, its key, the range's RangeMoved, the key's head,
 * and the head's writer then, and, once the key was walked twice, its
 * versions' handles and what each touches, oldest first, with each written
 * row's place, the places of the versions naming each row, and those that
 * set or lock the key. Its maps are vectors ordered by their keys.
 */
typedef struct TxnMemtableSliceMemo
{
	bool		has_at;
	TxnMemtableTableAt at;

	/*
	 * key is a Vec<u8>, copied, since the memo outlives the call that walked
	 * it.
	 */
	StdVec		key;
	uint64		moved;
	uint64		head;
	uint64		writer_xid;
	uint32		writer_command;
	bool		walked;
	/* touches is a Vec<(Handle, Touch)>. */
	StdVec		touches;
	/* rows is a BTreeMap<u64, usize>, a Vec of RowPlace ordered by row. */
	StdVec		rows;
	/* named is a BTreeMap<u64, Vec<usize>>, a Vec of Named ordered by row. */
	StdVec		named;
	/* keyed is a Vec<usize>. */
	StdVec		keyed;
} TxnMemtableSliceMemo;

/* txn_memtable_slice_Memo_Default_default is Memo's derived Default: no walk. */
static void
txn_memtable_slice_Memo_Default_default(TxnMemtableSliceMemo *result)
{
	memset(result, 0, sizeof(*result));
	std_vec_new(&result->key, 1, 1);
	std_vec_new(&result->touches, sizeof(txn_memtable_slice_Touched_t), _Alignof(txn_memtable_slice_Touched_t));
	std_vec_new(&result->rows, sizeof(txn_memtable_slice_RowPlace_t), _Alignof(txn_memtable_slice_RowPlace_t));
	std_vec_new(&result->named, sizeof(txn_memtable_slice_Named_t), _Alignof(txn_memtable_slice_Named_t));
	std_vec_new(&result->keyed, sizeof(size_t), _Alignof(size_t));
}

/* txn_memtable_slice_Memo_drop is Memo's drop glue: its key, its handles, and its maps. */
static void
txn_memtable_slice_Memo_drop(TxnMemtableSliceMemo *self)
{
	std_vec_free_Checked(&self->key, 1, 1);
	std_vec_free_Checked(&self->touches, sizeof(txn_memtable_slice_Touched_t), _Alignof(txn_memtable_slice_Touched_t));
	std_vec_free_Checked(&self->rows, sizeof(txn_memtable_slice_RowPlace_t), _Alignof(txn_memtable_slice_RowPlace_t));
	for (size_t k = 0; k < self->named.len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&self->named, txn_memtable_slice_Named_t, k).f1, sizeof(size_t),
							 _Alignof(size_t));
	std_vec_free_Checked(&self->named, sizeof(txn_memtable_slice_Named_t), _Alignof(txn_memtable_slice_Named_t));
	std_vec_free_Checked(&self->keyed, sizeof(size_t), _Alignof(size_t));
}

/* TxnMemtableSliceMemoLock is RwLock<Memo>: its lock's state and the memo it guards. */
typedef struct TxnMemtableSliceMemoLock
{
	StdRwLock	lock;
	TxnMemtableSliceMemo memo;
} TxnMemtableSliceMemoLock;

/* txn_memtable_slice_MemoLock_Default_default is RwLock<Memo>'s Default: an unlocked lock over no walk. */
static void
txn_memtable_slice_MemoLock_Default_default(void *result, const StdLocation *caller)
{
	TxnMemtableSliceMemoLock *self = result;
	StdRwLock	fresh = STD_RW_LOCK_NEW;

	(void) caller;
	self->lock = fresh;
	txn_memtable_slice_Memo_Default_default(&self->memo);
}

/* txn_memtable_slice_MemoLock_drop is RwLock<Memo>'s drop glue: its memo's. */
static void
txn_memtable_slice_MemoLock_drop(void *value)
{
	txn_memtable_slice_Memo_drop(&((TxnMemtableSliceMemoLock *) value)->memo);
}

/* TXN_MEMTABLE_SLICE_MEMO_TYPE is RwLock<Memo>'s size, Default, and drop. */
static const StdDefaultType TXN_MEMTABLE_SLICE_MEMO_TYPE = {sizeof(TxnMemtableSliceMemoLock),
_Alignof(TxnMemtableSliceMemoLock), txn_memtable_slice_MemoLock_Default_default, txn_memtable_slice_MemoLock_drop};

/*
 * TXN_MEMTABLE_SLICE_MEMO is MEMO, the backend's memo of the key whose chain
 * row_versions walked last: its BackendLocal header and the process's value,
 * which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	TxnMemtableSliceMemoLock process;
}			TXN_MEMTABLE_SLICE_MEMO = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* txn_memtable_slice_usize_lt is the fn item usize's PartialOrd::lt's FnMut shim, which sort_unstable passes. */
static bool
txn_memtable_slice_usize_lt(void *captures, const void *a, const void *b)
{
	(void) captures;
	return *(const size_t *) a < *(const size_t *) b;
}

/* The places' sort: sort_unstable at usize. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS Usize
#define STD_SORT_T size_t
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT txn_memtable_slice_usize_lt
#include "doltgres/std/sort_template.h"

/* txn_memtable_slice_Version_drop is a Version's drop glue as a slice's element drop takes it. */
static void
txn_memtable_slice_Version_drop(void *version)
{
	txn_memtable_version_Version_drop(version);
}

/* txn_memtable_slice_touch returns what a version changes. */
static TxnMemtableSliceTouch
txn_memtable_slice_touch(TxnMemtableVersionVersion *version)
{
	TxnMemtableSliceTouch touch = {0};
	TxnVisibleRowVersion row;
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);

	if (txn_visible_RowVersion_of(version, &row))
	{
		touch.has_row = true;
		touch.row = row.put.tuple.f0;
	}
	switch (op.tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE:
		case TXN_MEMTABLE_VERSION_OP_REF_EMPTIED:
		case TXN_MEMTABLE_VERSION_OP_REF_NAMED:
		case TXN_MEMTABLE_VERSION_OP_REF_MOVE:
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_LOCK:
			touch.lock = true;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_REMOVE:
			touch.has_names = true;
			touch.names = op.u.remove.f0.f0;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
			touch.has_names = true;
			touch.names = op.u.update_row.f0.f0;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW:
			touch.has_names = true;
			touch.names = op.u.delete_row.f0.f0;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW:
			touch.has_names = true;
			touch.names = op.u.lock_row.f0.f0;
			touch.lock = true;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD:
		case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
			touch.add = true;
			break;
	}
	return touch;
}

/* txn_memtable_slice_named returns the tuple id a successor's key ends with, as visible's own does. */
static bool
txn_memtable_slice_named(const TxnMemtableVersionSuccessorRef *successor, HostRow *result)
{
	uint8		tid[8];

	if (successor->tag == TXN_MEMTABLE_VERSION_SUCCESSOR_REF_OTHER_TABLE || successor->u.key.len < 8)
		return false;
	std_range_from_check(successor->u.key.len - 8, successor->u.key.len, RUST_FILE, 423, 34);
	memcpy(tid, successor->u.key.ptr + successor->u.key.len - 8, 8);
	result->f0 = std_u64_from_be_bytes(tid);
	return true;
}

/* txn_memtable_slice_ends_at reports whether an op is an end naming a successor whose key ends with a tuple id, giving the row it names. */
static bool
txn_memtable_slice_ends_at(const TxnMemtableVersionOpRef *op, HostRow tid, uint64 *named)
{
	const TxnMemtableVersionSuccessorRef *successor;
	HostRow		row;

	if (op->tag == TXN_MEMTABLE_VERSION_OP_REF_REMOVE && op->u.remove.has_f1)
	{
		successor = &op->u.remove.f1;
		*named = op->u.remove.f0.f0;
	}
	else if (op->tag == TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW && op->u.delete_row.has_f1)
	{
		successor = &op->u.delete_row.f1;
		*named = op->u.delete_row.f0.f0;
	}
	else
		return false;
	return txn_memtable_slice_named(successor, &row) && row.f0 == tid.f0;
}

/*
 * txn_memtable_slice_replaced_of returns the row the row with a tuple id
 * replaced, as visible's replaced reads it: the row its UpdateRow names, or
 * the row the end right under it names when that end names it as its
 * successor.
 */
static bool
txn_memtable_slice_replaced_of(TxnMemtableVersionVersion *row, TxnMemtableVersionVersion *under, HostRow tid,
							   uint64 *result)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(row);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);

	if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW)
	{
		*result = op.u.update_row.f0.f0;
		return true;
	}
	if (under == NULL)
		return false;
	record = txn_memtable_version_Version_record(under);
	op = txn_memtable_version_Record_op(&record);
	return txn_memtable_slice_ends_at(&op, tid, result);
}

/* txn_memtable_slice_rows_find finds a row's entry in an ordered map, or where it goes. */
static bool
txn_memtable_slice_rows_find(const StdVec *map, size_t size, uint64 row, size_t *at)
{
	size_t		low = 0;
	size_t		high = map->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		uint64		key = *(const uint64 *) STD_VEC_AT_SIZE(map, mid, size);

		if (key == row)
		{
			*at = mid;
			return true;
		}
		if (key < row)
			low = mid + 1;
		else
			high = mid;
	}
	*at = low;
	return false;
}

/* txn_memtable_slice_indexed adds a version, newer than every version the memo holds, to its handles and its maps. */
static void
txn_memtable_slice_indexed(TxnMemtableSliceMemo *memo, txn_memtable_slice_Touched_t entry)
{
	size_t		place = memo->touches.len;
	size_t		at;

	std_vec_push_Checked(&memo->touches, &entry, sizeof(entry), _Alignof(txn_memtable_slice_Touched_t));
	if (entry.f1.has_row)
	{
		txn_memtable_slice_RowPlace_t row = {entry.f1.row, place};

		if (txn_memtable_slice_rows_find(&memo->rows, sizeof(row), entry.f1.row, &at))
			STD_VEC_AT(&memo->rows, txn_memtable_slice_RowPlace_t, at).f1 = place;
		else
			std_vec_insert(&memo->rows, at, &row, sizeof(row), _Alignof(txn_memtable_slice_RowPlace_t), RUST_FILE,
						   357, 9);
	}
	if (entry.f1.add)
		return;
	if (entry.f1.has_names)
	{
		if (!txn_memtable_slice_rows_find(&memo->named, sizeof(txn_memtable_slice_Named_t), entry.f1.names, &at))
		{
			txn_memtable_slice_Named_t named = {entry.f1.names};

			std_vec_new(&named.f1, sizeof(size_t), _Alignof(size_t));
			std_vec_insert(&memo->named, at, &named, sizeof(named), _Alignof(txn_memtable_slice_Named_t), RUST_FILE,
						   361, 31);
		}
		std_vec_push_Checked(&STD_VEC_AT(&memo->named, txn_memtable_slice_Named_t, at).f1, &place, sizeof(place),
							 _Alignof(size_t));
		return;
	}
	std_vec_push_Checked(&memo->keyed, &place, sizeof(place), _Alignof(size_t));
}

/* TxnMemtableSliceMark is Mark: what a read under the range's lock found the chain at. */
typedef struct TxnMemtableSliceMark
{
	uint64		range;
	uint64		moved;
	uint64		head;
	uint64		writer_xid;
	uint32		writer_command;
} TxnMemtableSliceMark;

/* txn_memtable_slice_Mark_PartialEq_eq is Mark's derived PartialEq. */
static bool
txn_memtable_slice_Mark_PartialEq_eq(const TxnMemtableSliceMark *a, const TxnMemtableSliceMark *b)
{
	return a->range == b->range && a->moved == b->moved && a->head == b->head && a->writer_xid == b->writer_xid &&
		a->writer_command == b->writer_command;
}

/* txn_memtable_slice_row_versions_frame_0_t is row_versions' state across its pad. */
typedef struct txn_memtable_slice_row_versions_frame_0_t
{
	HostIoIo   *io;
	const HostDeltaHold *hold;
	HostSharedRecord state;
	const uint8 *key;
	size_t		key_len;
	HostRow		tid;
	uint64		folded;
	TxnMemtableSliceSlice *result;
	HostError  *error;
	bool		ok;
	HostLocalLocal local;
	bool		local_live;
	TxnMemtableSliceMemo memo;
	bool		memo_live;
	TxnMemtableSliceSlice slice;
	bool		slice_live;
	/* spilled is a Vec<Version> of the key's spilled versions. */
	StdVec		spilled;
	bool		spilled_live;
	/* whole is a Vec<Version> of the key's whole chain. */
	StdVec		whole;
	bool		whole_live;

	/*
	 * A read's row, the run under it, the places above it, the nearest of
	 * those, and the chain's mark.
	 */
	bool		read;
	bool		has_row;
	size_t		row_place;
	TxnMemtableVersionVersion row;
	bool		row_live;
	StdVec		under;
	bool		under_live;
	StdVec		places;
	bool		places_live;
	bool		has_nearest;
	TxnMemtableVersionVersion nearest;
	TxnMemtableSliceMark mark;
	/* candidates is a Vec<usize>. */
	StdVec		candidates;
	bool		candidates_live;
	/* kept is a Vec<Version> of the versions kept above the row. */
	StdVec		kept;
	bool		kept_live;

	/*
	 * handles is a Vec<Handle> of a chunk to read, and chunk is the chunk's
	 * versions, of which chunk_at are taken.
	 */
	StdVec		handles;
	bool		handles_live;
	StdVec		chunk;
	bool		chunk_live;
	size_t		chunk_at;
	/* walk is a Vec<(Handle, Touch)> a memo's walk gathers. */
	StdVec		walk;
	bool		walk_live;
	/* version is a version read for a moment, while live. */
	TxnMemtableVersionVersion version;
	bool		version_live;
} txn_memtable_slice_row_versions_frame_0_t;

/* txn_memtable_slice_read_drop drops what a read gave and a pass of sliced built on it, while live. */
static void
txn_memtable_slice_read_drop(txn_memtable_slice_row_versions_frame_0_t *frame)
{
	if (frame->version_live)
	{
		frame->version_live = false;
		txn_memtable_version_Version_drop(&frame->version);
	}
	if (frame->walk_live)
	{
		frame->walk_live = false;
		std_vec_free_Checked(&frame->walk, sizeof(txn_memtable_slice_Touched_t),
							 _Alignof(txn_memtable_slice_Touched_t));
	}
	if (frame->chunk_live)
	{
		frame->chunk_live = false;
		txn_memtable_version_versions_drop(&frame->chunk, frame->chunk_at);
	}
	if (frame->handles_live)
	{
		frame->handles_live = false;
		std_vec_free_Checked(&frame->handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	}
	if (frame->kept_live)
	{
		frame->kept_live = false;
		txn_memtable_version_versions_drop(&frame->kept, 0);
	}
	if (frame->candidates_live)
	{
		frame->candidates_live = false;
		std_vec_free_Checked(&frame->candidates, sizeof(size_t), _Alignof(size_t));
	}
	if (frame->has_nearest)
	{
		frame->has_nearest = false;
		txn_memtable_version_Version_drop(&frame->nearest);
	}
	if (frame->places_live)
	{
		frame->places_live = false;
		std_vec_free_Checked(&frame->places, sizeof(size_t), _Alignof(size_t));
	}
	if (frame->under_live)
	{
		frame->under_live = false;
		txn_memtable_version_versions_drop(&frame->under, 0);
	}
	if (frame->row_live)
	{
		frame->row_live = false;
		txn_memtable_version_Version_drop(&frame->row);
	}
}

/* txn_memtable_slice_row_versions_pad_0 is row_versions' pad: a pass's holdings, the chains, the slice, the memo, and the local, while live. */
static void
txn_memtable_slice_row_versions_pad_0(void *frame_)
{
	txn_memtable_slice_row_versions_frame_0_t *frame = frame_;

	txn_memtable_slice_read_drop(frame);
	if (frame->whole_live)
	{
		frame->whole_live = false;
		txn_memtable_version_versions_drop(&frame->whole, 0);
	}
	if (frame->spilled_live)
	{
		frame->spilled_live = false;
		txn_memtable_version_versions_drop(&frame->spilled, 0);
	}
	if (frame->slice_live)
	{
		frame->slice_live = false;
		txn_memtable_slice_Slice_drop(&frame->slice);
	}
	if (frame->memo_live)
	{
		frame->memo_live = false;
		txn_memtable_slice_Memo_drop(&frame->memo);
	}
	if (frame->local_live)
	{
		frame->local_live = false;
		host_local_Local_drop(&frame->local, &TXN_MEMTABLE_SLICE_MEMO_TYPE);
	}
}

/* txn_memtable_slice_version reads the version of a memo's place under the range's guard into a Vec of them. */
static bool
txn_memtable_slice_version(txn_memtable_slice_row_versions_frame_0_t *frame, const HostSharedGuard *guard,
						   TxnMemtableVersionHandle handle, StdVec *into, HostError *error)
{
	if (!txn_memtable_version_version(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, frame->hold, handle,
									  &frame->version, error))
		return false;
	frame->version_live = true;
	std_vec_push_Checked(into, &frame->version, sizeof(frame->version), _Alignof(TxnMemtableVersionVersion));
	frame->version_live = false;
	return true;
}

/* txn_memtable_slice_handle returns the handle at a memo's place, with Rust's index check at a line. */
static TxnMemtableVersionHandle
txn_memtable_slice_handle(const TxnMemtableSliceMemo *memo, size_t place, uint32 line, uint32 col)
{
	std_index_check(place, memo->touches.len, RUST_FILE, line, col);
	return STD_VEC_AT(&memo->touches, txn_memtable_slice_Touched_t, place).f0;
}

/* txn_memtable_slice_marked returns a chain's mark under its range's shared lock. */
static bool
txn_memtable_slice_marked(txn_memtable_slice_row_versions_frame_0_t *frame, TxnMemtableTableAt at,
						  const HostSharedGuard *guard, TxnMemtableIndexNode node, TxnMemtableSliceMark *result,
						  HostError *error)
{
	TxnMemtableVersionHandle handle;
	HostSharedRecord range;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

	if (!txn_memtable_index_head(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, frame->hold, node, &result->head, error))
		return false;
	result->writer_xid = 0;
	result->writer_command = 0;
	if (txn_memtable_version_Handle_named(result->head, &handle))
	{
		TxnMemtableVersionRecord record;
		TxnMemtableVersionWriter writer;

		if (!txn_memtable_version_version(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, frame->hold, handle,
										  &frame->version, error))
			return false;
		frame->version_live = true;
		record = txn_memtable_version_Version_record(&frame->version);
		writer = txn_memtable_version_Record_writer(&record);
		result->writer_xid = writer.xid;
		result->writer_command = writer.command;
		frame->version_live = false;
		txn_memtable_version_Version_drop(&frame->version);
	}
	if (!host_delta_Hold_record(frame->hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at.range, &range, error))
		return false;
	result->range = at.range;
	result->moved = host_shared_Record_atomic(range, TXN_MEMTABLE_RANGE_RANGE_MOVED, load);
	return true;
}

/*
 * txn_memtable_slice_walked walks a key's chain under the range's lock into
 * the frame's walk, newest first: to its end, or, with stop, to the memo's
 * head as its writer wrote it, answering in reached whether it got there.
 */
static bool
txn_memtable_slice_walked(txn_memtable_slice_row_versions_frame_0_t *frame, const HostSharedGuard *guard,
						  TxnMemtableIndexNode node, bool stop, bool *reached, HostError *error)
{
	TxnMemtableSliceMemo *memo = &frame->memo;
	TxnMemtableVersionChain chain;
	TxnMemtableVersionHandle head;
	bool		has_head = stop && txn_memtable_version_Handle_named(memo->head, &head);

	*reached = false;
	std_vec_new(&frame->walk, sizeof(txn_memtable_slice_Touched_t), _Alignof(txn_memtable_slice_Touched_t));
	frame->walk_live = true;
	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, frame->hold, node, &chain, error))
		return false;
	for (;;)
	{
		bool		some;
		txn_memtable_slice_Touched_t entry;

		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &frame->version, error))
			return false;
		if (!some)
			return true;
		frame->version_live = true;
		if (stop)
		{
			TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&frame->version);
			TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);

			if (has_head && head.f0 == frame->version.handle.f0 && memo->writer_xid == writer.xid &&
				memo->writer_command == writer.command)
			{
				frame->version_live = false;
				txn_memtable_version_Version_drop(&frame->version);
				*reached = true;
				return true;
			}
		}
		entry.f0 = frame->version.handle;
		entry.f1 = txn_memtable_slice_touch(&frame->version);
		std_vec_push_Checked(&frame->walk, &entry, sizeof(entry), _Alignof(txn_memtable_slice_Touched_t));
		frame->version_live = false;
		txn_memtable_version_Version_drop(&frame->version);
	}
}

/* txn_memtable_slice_walk_free frees the frame's walk. */
static void
txn_memtable_slice_walk_free(txn_memtable_slice_row_versions_frame_0_t *frame)
{
	frame->walk_live = false;
	std_vec_free_Checked(&frame->walk, sizeof(txn_memtable_slice_Touched_t), _Alignof(txn_memtable_slice_Touched_t));
}

/*
 * txn_memtable_slice_refreshed brings the memo up to a key's chain: kept when
 * it walked this chain at this mark, extended by the versions pushed above
 * its head when only the head changed, walked whole into its maps on the
 * key's second walk, and walked into its handles alone on the first.
 */
static bool
txn_memtable_slice_refreshed(txn_memtable_slice_row_versions_frame_0_t *frame, TxnMemtableTableAt at,
							 const HostSharedGuard *guard, TxnMemtableIndexNode node, TxnMemtableSliceMark mark,
							 HostError *error)
{
	TxnMemtableSliceMemo *memo = &frame->memo;
	bool		same = memo->has_at && memo->at.memtable == at.memtable && memo->at.range == at.range &&
		memo->key.len == frame->key_len && memcmp(memo->key.ptr, frame->key, frame->key_len) == 0 &&
		memo->moved == mark.moved;
	bool		reached;
	bool		walked;

	if (same && memo->head == mark.head && memo->writer_xid == mark.writer_xid &&
		memo->writer_command == mark.writer_command)
		return true;
	if (same && memo->walked)
	{
		if (!txn_memtable_slice_walked(frame, guard, node, true, &reached, error))
			return false;
		if (reached)
		{
			for (size_t k = frame->walk.len; k > 0; k--)
				txn_memtable_slice_indexed(memo, STD_VEC_AT(&frame->walk, txn_memtable_slice_Touched_t, k - 1));
			txn_memtable_slice_walk_free(frame);
			memo->head = mark.head;
			memo->writer_xid = mark.writer_xid;
			memo->writer_command = mark.writer_command;
			return true;
		}
		txn_memtable_slice_walk_free(frame);
	}
	walked = same;
	txn_memtable_slice_Memo_drop(memo);
	txn_memtable_slice_Memo_Default_default(memo);
	memo->has_at = true;
	memo->at = at;
	std_vec_extend_from_slice_Checked(&memo->key, frame->key, frame->key_len, 1, 1);
	memo->moved = mark.moved;
	memo->head = mark.head;
	memo->writer_xid = mark.writer_xid;
	memo->writer_command = mark.writer_command;
	memo->walked = walked;
	if (!txn_memtable_slice_walked(frame, guard, node, false, &reached, error))
		return false;
	std_slice_reverse(frame->walk.ptr, frame->walk.len, sizeof(txn_memtable_slice_Touched_t),
					  _Alignof(txn_memtable_slice_Touched_t));
	if (walked)
	{
		for (size_t k = 0; k < frame->walk.len; k++)
			txn_memtable_slice_indexed(memo, STD_VEC_AT(&frame->walk, txn_memtable_slice_Touched_t, k));
		txn_memtable_slice_walk_free(frame);
		return true;
	}
	std_vec_free_Checked(&memo->touches, sizeof(txn_memtable_slice_Touched_t), _Alignof(txn_memtable_slice_Touched_t));
	memo->touches = frame->walk;
	frame->walk_live = false;
	return true;
}

/* txn_memtable_slice_touches reports whether the version at a memo's place sets or locks the key, names the row, or locks the row it replaced. */
static bool
txn_memtable_slice_touches(const TxnMemtableSliceMemo *memo, size_t place, HostRow tid, bool has_replaced,
						   uint64 replaced)
{
	TxnMemtableSliceTouch t;

	std_index_check(place, memo->touches.len, RUST_FILE, 282, 17);
	t = STD_VEC_AT(&memo->touches, txn_memtable_slice_Touched_t, place).f1;
	return !t.add && (!t.has_names || t.names == tid.f0 || (t.lock && t.has_names && has_replaced &&
															t.names == replaced));
}

/* txn_memtable_slice_named_places pushes the places of a row's naming versions newer than a place, and touching when asked. */
static void
txn_memtable_slice_named_places(const TxnMemtableSliceMemo *memo, uint64 row, bool has_place, size_t place,
								bool touching, HostRow tid, bool has_replaced, uint64 replaced, StdVec *result)
{
	size_t		at;
	const StdVec *named;

	if (!txn_memtable_slice_rows_find(&memo->named, sizeof(txn_memtable_slice_Named_t), row, &at))
		return;
	named = &STD_VEC_AT(&memo->named, txn_memtable_slice_Named_t, at).f1;
	for (size_t k = 0; k < named->len; k++)
	{
		size_t		p = STD_VEC_AT(named, size_t, k);

		if ((!has_place || p > place) &&
			(!touching || txn_memtable_slice_touches(memo, p, tid, has_replaced, replaced)))
			std_vec_push_Checked(result, &p, sizeof(p), _Alignof(size_t));
	}
}

/*
 * txn_memtable_slice_places writes into an empty Vec the places of the
 * versions newer than a row's place, or all for a row the delta doesn't
 * hold, that set or lock the key, name the row, or lock the row it replaced,
 * in order from the oldest: from the memo's maps once the key was walked
 * twice, and by a look at each version on its first walk.
 */
static void
txn_memtable_slice_places(const TxnMemtableSliceMemo *memo, HostRow tid, bool has_place, size_t place,
						  bool has_replaced, uint64 replaced, StdVec *result)
{
	if (!memo->walked)
	{
		for (size_t p = 0; p < memo->touches.len; p++)
			if ((!has_place || p > place) && txn_memtable_slice_touches(memo, p, tid, has_replaced, replaced))
				std_vec_push_Checked(result, &p, sizeof(p), _Alignof(size_t));
		return;
	}
	for (size_t k = 0; k < memo->keyed.len; k++)
	{
		size_t		p = STD_VEC_AT(&memo->keyed, size_t, k);

		if (!has_place || p > place)
			std_vec_push_Checked(result, &p, sizeof(p), _Alignof(size_t));
	}
	txn_memtable_slice_named_places(memo, tid.f0, has_place, place, false, tid, has_replaced, replaced, result);
	if (has_replaced && replaced != tid.f0)
		txn_memtable_slice_named_places(memo, replaced, has_place, place, true, tid, has_replaced, replaced, result);
	std_slice_sort_unstable_Usize(result->ptr, result->len);
}

/*
 * txn_memtable_slice_read_row is read_row, a step under the range's
 * shared lock: it brings the memo up to the key's chain and reads the row
 * with the tuple id and the run under it into the frame's read, or leaves no
 * read for a key the memtable holds no version of.
 */
static bool
txn_memtable_slice_read_row(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
							const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_slice_row_versions_frame_0_t *frame = captures;
	TxnMemtableSliceMemo *memo = &frame->memo;
	TxnMemtableIndexNode node;
	bool		some;
	size_t		place = 0;
	bool		has_place = false;
	size_t		below;
	bool		has_replaced;
	uint64		replaced = 0;

	(void) hold;
	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, frame->hold, frame->key, frame->key_len,
								 &some, &node, error))
		return false;
	if (!some)
		return true;
	if (!txn_memtable_slice_marked(frame, at, guard, node, &frame->mark, error) ||
		!txn_memtable_slice_refreshed(frame, at, guard, node, frame->mark, error))
		return false;
	if (memo->walked)
	{
		size_t		at_row;

		if (txn_memtable_slice_rows_find(&memo->rows, sizeof(txn_memtable_slice_RowPlace_t), frame->tid.f0, &at_row))
		{
			has_place = true;
			place = STD_VEC_AT(&memo->rows, txn_memtable_slice_RowPlace_t, at_row).f1;
		}
	}
	else
		for (size_t k = memo->touches.len; k > 0 && !has_place; k--)
		{
			TxnMemtableSliceTouch t = STD_VEC_AT(&memo->touches, txn_memtable_slice_Touched_t, k - 1).f1;

			if (t.has_row && t.row == frame->tid.f0)
			{
				has_place = true;
				place = k - 1;
			}
		}
	std_vec_new(&frame->under, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	frame->under_live = true;
	std_vec_new(&frame->places, sizeof(size_t), _Alignof(size_t));
	frame->places_live = true;
	frame->read = true;
	frame->has_row = has_place;
	if (!has_place)
	{
		txn_memtable_slice_places(memo, frame->tid, false, 0, false, 0, &frame->places);
		return true;
	}
	frame->row_place = place;
	if (!txn_memtable_version_version(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, frame->hold,
									  txn_memtable_slice_handle(memo, place, 252, 49), &frame->row,
									  error))
		return false;
	frame->row_live = true;
	below = place;
	if (below > 0)
	{
		TxnMemtableVersionRecord record;
		TxnMemtableVersionOpRef op;
		uint64		named;

		if (!txn_memtable_version_version(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, frame->hold,
										  txn_memtable_slice_handle(memo, below - 1, 256, 49),
										  &frame->version, error))
			return false;
		frame->version_live = true;
		record = txn_memtable_version_Version_record(&frame->version);
		op = txn_memtable_version_Record_op(&record);
		if (txn_memtable_slice_ends_at(&op, frame->tid, &named))
		{
			std_vec_push_Checked(&frame->under, &frame->version, sizeof(frame->version),
								 _Alignof(TxnMemtableVersionVersion));
			frame->version_live = false;
			below--;
		}
		else
		{
			frame->version_live = false;
			txn_memtable_version_Version_drop(&frame->version);
		}
	}
	for (;;)
	{
		if (below == 0)
			break;
		std_index_check(below - 1, memo->touches.len, RUST_FILE, 265, 12);
		if (!STD_VEC_AT(&memo->touches, txn_memtable_slice_Touched_t, below - 1).f1.lock)
			break;
		if (!txn_memtable_slice_version(frame, guard,
										txn_memtable_slice_handle(memo, below - 1, 267, 50),
										&frame->under, error))
			return false;
		below--;
	}
	has_replaced = txn_memtable_slice_replaced_of(&frame->row,
												  frame->under.len > 0 ?
												  &STD_VEC_AT(&frame->under, TxnMemtableVersionVersion, 0) : NULL,
												  frame->tid, &replaced);
	txn_memtable_slice_places(memo, frame->tid, true, place, has_replaced, replaced, &frame->places);
	if (frame->places.len > 0)
	{
		if (!txn_memtable_version_version(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, frame->hold,
										  txn_memtable_slice_handle(memo, STD_VEC_AT(&frame->places, size_t, 0),
																	272, 73),
										  &frame->nearest, error))
			return false;
		frame->has_nearest = true;
	}
	return true;
}

/*
 * txn_memtable_slice_read_more is read_more, a step under the range's
 * shared lock: it reads the versions of the chunk's handles into the frame's
 * chunk, or leaves no read when the chain moved a version or changed its
 * head since the mark.
 */
static bool
txn_memtable_slice_read_more(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
							 const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_slice_row_versions_frame_0_t *frame = captures;
	TxnMemtableIndexNode node;
	TxnMemtableSliceMark now;
	bool		some;

	(void) hold;
	frame->read = false;
	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, frame->hold, frame->key, frame->key_len,
								 &some, &node, error))
		return false;
	if (!some)
		return true;
	if (!txn_memtable_slice_marked(frame, at, guard, node, &now, error))
		return false;
	if (!txn_memtable_slice_Mark_PartialEq_eq(&now, &frame->mark))
		return true;
	std_vec_new(&frame->chunk, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	frame->chunk_live = true;
	frame->chunk_at = 0;
	for (size_t k = 0; k < frame->handles.len; k++)
		if (!txn_memtable_slice_version(frame, guard, STD_VEC_AT(&frame->handles, TxnMemtableVersionHandle, k),
										&frame->chunk, error))
			return false;
	frame->read = true;
	return true;
}

/*
 * txn_memtable_slice_assembled orders a slice's versions newest first into
 * the frame's slice: those kept above the row, newest first, the row, and the
 * run under it, with the key's head.
 */
static void
txn_memtable_slice_assembled(txn_memtable_slice_row_versions_frame_0_t *frame)
{
	TxnMemtableSliceSlice *slice = &frame->slice;

	std_slice_reverse(frame->kept.ptr, frame->kept.len, sizeof(TxnMemtableVersionVersion),
					  _Alignof(TxnMemtableVersionVersion));
	slice->versions = frame->kept;
	frame->kept_live = false;
	frame->slice_live = true;
	slice->has_row = frame->row_live;
	slice->row = slice->versions.len;
	if (frame->row_live)
	{
		std_vec_push_Checked(&slice->versions, &frame->row, sizeof(frame->row), _Alignof(TxnMemtableVersionVersion));
		frame->row_live = false;
	}
	std_vec_extend_from_slice_Checked(&slice->versions, frame->under.ptr, frame->under.len,
									  sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	frame->under_live = false;
	std_vec_free_Checked(&frame->under, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	slice->has_head = txn_memtable_version_Handle_named(frame->mark.head, &slice->head) &&
		frame->mark.head != TXN_MEMTABLE_INDEX_NONE;
}

/* txn_memtable_slice_empty makes the frame's slice an empty one: no versions, no row, and no head. */
static void
txn_memtable_slice_empty(txn_memtable_slice_row_versions_frame_0_t *frame)
{
	std_vec_new(&frame->slice.versions, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	frame->slice.has_row = false;
	frame->slice.has_head = false;
	frame->slice_live = true;
}

/* txn_memtable_slice_in_base reports whether a base folded at the fold point applied a version. */
static bool
txn_memtable_slice_in_base(txn_memtable_slice_row_versions_frame_0_t *frame, TxnMemtableVersionVersion *version,
						   bool *result)
{
	return txn_visible_in_base(frame->io, version, (HostLsn) frame->folded, result, frame->error);
}

/*
 * txn_memtable_slice_chunk reads one chunk of the candidates above the row:
 * the nearest read, or the chunk's handles' versions under the range's
 * shared lock, answering read false when the chain changed since its mark.
 */
static bool
txn_memtable_slice_chunk(txn_memtable_slice_row_versions_frame_0_t *frame, size_t from, size_t to)
{
	bool		some;

	if (frame->has_nearest)
	{
		std_vec_new(&frame->chunk, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
		frame->chunk_live = true;
		frame->chunk_at = 0;
		std_vec_push_Checked(&frame->chunk, &frame->nearest, sizeof(frame->nearest),
							 _Alignof(TxnMemtableVersionVersion));
		frame->has_nearest = false;
		frame->read = true;
		return true;
	}
	std_range_check(from, to, frame->candidates.len, RUST_FILE, 176, 25);
	std_vec_new(&frame->handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	frame->handles_live = true;
	for (size_t k = from; k < to; k++)
	{
		TxnMemtableVersionHandle handle = txn_memtable_slice_handle(&frame->memo,
																	STD_VEC_AT(&frame->candidates, size_t, k),
																	176, 66);

		std_vec_push_Checked(&frame->handles, &handle, sizeof(handle), _Alignof(TxnMemtableVersionHandle));
	}
	if (!txn_memtable_table_lent(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, frame->hold, frame->state, frame->key,
								 frame->key_len, txn_memtable_slice_read_more, frame, &some, frame->error))
		return false;
	frame->read = frame->read && some;
	frame->handles_live = false;
	std_vec_free_Checked(&frame->handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	return true;
}

/*
 * txn_memtable_slice_kept keeps each version of a chunk a base folded at the
 * fold point didn't apply, answering settled once one settles the row: it
 * isn't a lock, it names no row or the row, and its writer committed or is
 * the current transaction.
 */
static bool
txn_memtable_slice_kept(txn_memtable_slice_row_versions_frame_0_t *frame, size_t from, size_t to, bool *settled)
{
	*settled = false;
	std_range_check(from, to, frame->candidates.len, RUST_FILE, 184, 59);
	for (size_t k = from; k < to && frame->chunk_at < frame->chunk.len; k++)
	{
		size_t		place = STD_VEC_AT(&frame->candidates, size_t, k);
		bool		applied;
		TxnMemtableSliceTouch t;
		bool		settles;

		frame->version = STD_VEC_AT(&frame->chunk, TxnMemtableVersionVersion, frame->chunk_at);
		frame->chunk_at = std_wrapping_add_usize(frame->chunk_at, 1);
		frame->version_live = true;
		if (!txn_memtable_slice_in_base(frame, &frame->version, &applied))
			return false;
		if (applied)
		{
			frame->version_live = false;
			txn_memtable_version_Version_drop(&frame->version);
			continue;
		}
		std_index_check(place, frame->memo.touches.len, RUST_FILE, 188, 25);
		t = STD_VEC_AT(&frame->memo.touches, txn_memtable_slice_Touched_t, place).f1;
		settles = !t.lock && (!t.has_names || t.names == frame->tid.f0);
		if (settles)
		{
			TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&frame->version);
			HostXid		xid = {txn_memtable_version_Record_writer(&record).xid};

			if (!host_is_current(HOST_IO_IO_IO_DOORS(frame->io), xid))
			{
				HostStatus	status;

				if (!txn_outcome_outcome(frame->io, &frame->version, &status, frame->error))
					return false;
				settles = status == HOST_STATUS_COMMITTED;
			}
		}
		std_vec_push_Checked(&frame->kept, &frame->version, sizeof(frame->version),
							 _Alignof(TxnMemtableVersionVersion));
		frame->version_live = false;
		if (settles)
		{
			*settled = true;
			return true;
		}
	}
	frame->chunk_live = false;
	txn_memtable_version_versions_drop(&frame->chunk, frame->chunk_at);
	return true;
}

/*
 * txn_memtable_slice_sliced is row_versions over the backend's memo, which
 * the frame holds for the call: it reads the row and the run under it, then
 * the versions above it in chunks that double, each under the range's
 * shared lock, and asks their writers' outcomes with no lock held, starting
 * again when the range moved a version or the key's head changed. A failed
 * first read leaves the memo empty, since its walk may be partway.
 */
static bool
txn_memtable_slice_sliced(txn_memtable_slice_row_versions_frame_0_t *frame)
{
	for (;;)
	{
		bool		some;
		size_t		from = 0;
		size_t		chunk = 1;
		bool		again = false;

		frame->read = false;
		if (!txn_memtable_table_lent(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, frame->hold, frame->state,
									 frame->key, frame->key_len, txn_memtable_slice_read_row, frame, &some,
									 frame->error))
		{
			txn_memtable_slice_Memo_drop(&frame->memo);
			txn_memtable_slice_Memo_Default_default(&frame->memo);
			return false;
		}
		if (!some || !frame->read)
		{
			txn_memtable_slice_empty(frame);
			return true;
		}
		if (frame->row_live)
		{
			bool		applied;

			if (!txn_memtable_slice_in_base(frame, &frame->row, &applied))
				return false;
			if (applied)
			{
				frame->row_live = false;
				txn_memtable_version_Version_drop(&frame->row);
				std_vec_truncate(&frame->under, 0, sizeof(TxnMemtableVersionVersion),
								 txn_memtable_slice_Version_drop);
				if (frame->has_nearest)
				{
					frame->has_nearest = false;
					txn_memtable_version_Version_drop(&frame->nearest);
				}
				std_vec_truncate(&frame->places, 0, sizeof(size_t), NULL);
				txn_memtable_slice_places(&frame->memo, frame->tid, false, 0, false, 0, &frame->places);
			}
		}
		frame->candidates = frame->places;
		frame->candidates_live = true;
		frame->places_live = false;
		std_vec_new(&frame->kept, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
		frame->kept_live = true;
		while (from < frame->candidates.len)
		{
			size_t		to = Min(STD_ADD(usize, from, chunk, 171, 23), frame->candidates.len);
			bool		settled;

			if (!txn_memtable_slice_chunk(frame, from, to))
				return false;
			if (!frame->read)
			{
				again = true;
				break;
			}
			if (!txn_memtable_slice_kept(frame, from, to, &settled))
				return false;
			if (settled)
				break;
			from = to;
			chunk = STD_MUL(usize, chunk, 2, 198, 34);
		}
		if (again)
		{
			txn_memtable_slice_read_drop(frame);
			continue;
		}
		txn_memtable_slice_assembled(frame);
		txn_memtable_slice_read_drop(frame);
		return true;
	}
}

/*
 * txn_memtable_slice_whole is a row's slice when its key has spilled
 * versions, which are older than every version the memtable holds and which
 * the memo never walks: the key's whole chain and then its spills', less
 * those a base folded at the fold point applied, with the row's place among
 * them and the memtable's newest as the head.
 */
static bool
txn_memtable_slice_whole(txn_memtable_slice_row_versions_frame_0_t *frame)
{
	TxnMemtableSliceSlice *slice = &frame->slice;
	StdVec		versions;

	if (!txn_memtable_table_versions(frame->io, frame->hold, frame->state, frame->key, frame->key_len, &frame->whole,
									 frame->error))
		return false;
	frame->whole_live = true;
	slice->has_head = txn_memtable_version_held(frame->whole.ptr, frame->whole.len, &slice->head);
	versions = frame->whole;
	frame->whole_live = false;
	if (!txn_visible_unfolded(frame->io, &versions, (HostLsn) frame->folded, &slice->versions, frame->error))
		return false;
	frame->slice_live = true;
	slice->has_row = false;
	for (size_t k = 0; k < slice->versions.len && !slice->has_row; k++)
	{
		TxnVisibleRowVersion row;

		if (txn_visible_RowVersion_of(&STD_VEC_AT(&slice->versions, TxnMemtableVersionVersion, k), &row) &&
			row.put.tuple.f0 == frame->tid.f0)
		{
			slice->has_row = true;
			slice->row = k;
		}
	}
	return true;
}

/* txn_memtable_slice_memo_lock takes the backend's memo's lock with no wait, answering whether it holds it. */
static bool
txn_memtable_slice_memo_lock(txn_memtable_slice_row_versions_frame_0_t *frame, StdRwLockWriteGuard *guard,
							 TxnMemtableSliceMemoLock **lock)
{
	*lock = (TxnMemtableSliceMemoLock *) host_local_Local_Deref_deref(&frame->local, &TXN_MEMTABLE_SLICE_MEMO_TYPE);
	switch (std_rw_lock_try_write(&(*lock)->lock, &(*lock)->memo, guard))
	{
		case STD_TRY_LOCK_OK:
			return true;
		case STD_TRY_LOCK_POISONED:
			std_RwLockWriteGuard_drop(guard);
			return false;
		case STD_TRY_LOCK_WOULD_BLOCK:
			return false;
	}
	pg_unreachable();
}

/* txn_memtable_slice_row_versions_body_0 is row_versions' body. */
static void
txn_memtable_slice_row_versions_body_0(void *frame_)
{
	txn_memtable_slice_row_versions_frame_0_t *frame = frame_;
	StdRwLockWriteGuard guard;
	TxnMemtableSliceMemoLock *lock;
	bool		sliced;

	frame->local = host_local_BackendLocal_get(&TXN_MEMTABLE_SLICE_MEMO.local, &TXN_MEMTABLE_SLICE_MEMO_TYPE);
	frame->local_live = true;
	if (txn_memtable_slice_memo_lock(frame, &guard, &lock))
	{
		frame->memo = lock->memo;
		txn_memtable_slice_Memo_Default_default(&lock->memo);
		std_RwLockWriteGuard_drop(&guard);
	}
	else
		txn_memtable_slice_Memo_Default_default(&frame->memo);
	frame->memo_live = true;
	sliced = txn_memtable_slice_sliced(frame);
	if (txn_memtable_slice_memo_lock(frame, &guard, &lock))
	{
		txn_memtable_slice_Memo_drop(&lock->memo);
		lock->memo = frame->memo;
		frame->memo_live = false;
		std_RwLockWriteGuard_drop(&guard);
	}
	if (!sliced)
	{
		txn_memtable_slice_row_versions_pad_0(frame);
		return;
	}
	std_vec_new(&frame->spilled, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	if (!txn_spill_versions(&TXN_SPILL_STATE_HOME_OPS, frame->io, frame->hold, frame->state, TXN_RUNS_KIND_SPILLED_ROWS,
							frame->key, frame->key_len, &frame->spilled, frame->error))
	{
		txn_memtable_slice_row_versions_pad_0(frame);
		return;
	}
	frame->spilled_live = true;
	if (frame->spilled.len > 0)
	{
		frame->slice_live = false;
		txn_memtable_slice_Slice_drop(&frame->slice);
		if (!txn_memtable_slice_whole(frame))
		{
			txn_memtable_slice_row_versions_pad_0(frame);
			return;
		}
	}
	*frame->result = frame->slice;
	frame->slice_live = false;
	frame->ok = true;
	txn_memtable_slice_row_versions_pad_0(frame);
}

/*
 * txn_memtable_slice_row_versions returns the slice of a key's chain in a
 * storage's rows memtable that decides the row with a tuple id, for a
 * caller's hold: above the row, each version that sets or locks its key,
 * names it, or locks the row it replaced, from the row upward through the
 * first that settles it (isn't a lock and was written by a committed or the
 * current transaction), or all of them; the row; and under it, the end
 * naming it as its successor and the locks it carried. With a fold point,
 * the versions a base folded at it applied are left out first. The walk is
 * the memo's, extended by the versions pushed since and walked again when
 * its range moved a version. A key with spilled versions once the slice is
 * read gives its whole chain instead.
 */
bool
txn_memtable_slice_row_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, const uint8 *key,
								size_t key_len, HostRow tid, uint64 folded, TxnMemtableSliceSlice *result,
								HostError *error)
{
	txn_memtable_slice_row_versions_frame_0_t frame = {.io = io, .hold = hold, .state = state, .key = key,
	.key_len = key_len, .tid = tid, .folded = folded, .result = result, .error = error};

	std_landing(&frame, txn_memtable_slice_row_versions_body_0, txn_memtable_slice_row_versions_pad_0);
	return frame.ok;
}

/* txn_memtable_slice_Slice_drop drops a slice's versions. */
void
txn_memtable_slice_Slice_drop(TxnMemtableSliceSlice *self)
{
	txn_memtable_version_versions_drop(&self->versions, 0);
}
