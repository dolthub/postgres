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
 * A storage's changes that end or lock a row: a delete, the old key's end of
 * an update that moves its row, and a row lock, each its version pushed in
 * one checkpoint window after one prepare, under its key's stripe of the key
 * lock, over the version its read found, a logged one's after its row change
 * record; and their redo, and the choice of a key's stripe.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/txn/change.h"
#include "doltgres/txn/memtable/slice.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/written.h"
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

/* The prepare whose closure answers an end, or none to start over. */
#define TXN_MEMTABLE_TABLE_RS OptionEnded
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , bool *has_result, TxnChangeEnded *result, HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#define TXN_MEMTABLE_TABLE_DEFINE
#define TXN_MEMTABLE_TABLE_RESULT_FIELDS bool *has_result; TxnChangeEnded *result;
#define TXN_MEMTABLE_TABLE_RESULT_INIT , has_result, result
#define TXN_MEMTABLE_TABLE_RESULT_ARGS(c) , (c)->has_result, (c)->result
#define TXN_MEMTABLE_TABLE_RESULT_ERROR error
#include "doltgres/txn/memtable/table_template.h"

/* The prepare whose closure pushes a version and returns its handle. */
#define TXN_MEMTABLE_TABLE_RS Handle
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , TxnMemtableVersionHandle *result, HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#include "doltgres/txn/memtable/table_template.h"

/* TXN_CHANGE_LOGGED is the pause inside a change's checkpoint window, after its record. */
#define TXN_CHANGE_LOGGED "doltgres-change-logged"

/*
 * txn_change_changed_cid returns heap's record of the command ids of a row a
 * change at a command ends, by what its read decided, as log_heap_new_cid
 * fills it for a deleted or updated tuple: with a combo id, the row's own
 * command, the change's, and the combo id; else the change's command alone.
 */
HostNewCid
txn_change_changed_cid(TxnMemtableVersionShown shown, uint32 cmin, uint32 command)
{
	HostNewCid	cid = {.has_cmax = true, .cmax = {command}};

	if (shown.tag == TXN_MEMTABLE_VERSION_SHOWN_COMBO)
	{
		cid.has_cmin = true;
		cid.cmin.f0 = cmin;
		cid.has_combo = true;
		cid.combo.f0 = shown.u.combo;
	}
	return cid;
}

/*
 * txn_change_Ended_PartialEq_eq reports whether two ends did the same, and
 * for two stops, whether their T's are equal by T's eq.
 */
bool
txn_change_Ended_PartialEq_eq(TxnChangeEnded self, const void *self_stopped, TxnChangeEnded other,
							  const void *other_stopped, bool (*eq) (const void *a, const void *b))
{
	return self == other && (self == TXN_CHANGE_ENDED_PUSHED || eq(self_stopped, other_stopped));
}

/* txn_change_Ended_Debug_fmt is Ended's derived Debug, of a TxnChangeEndedRef. */
bool
txn_change_Ended_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnChangeEndedRef *self = value;
	StdDebugTuple t;

	if (self->tag == TXN_CHANGE_ENDED_PUSHED)
		return std_fmt_write_str(f, "Pushed", 6);
	t = std_fmt_debug_tuple(f, "Stopped", 7);
	std_DebugTuple_field(&t, self->stopped.value, self->stopped.fmt);
	return std_DebugTuple_finish(&t);
}

/*
 * txn_change_stripe returns the key lock's stripe of a storage's key: the
 * xxh3 of the storage and the key's bytes, or of the storage alone for a key
 * whose equal values can have other bytes, which by_bytes false names.
 */
uint32
txn_change_stripe(const HostStorage *storage, const uint8 *key, size_t key_len, bool by_bytes)
{
	uint32		numbers[] = {storage->tablespace, storage->database, storage->relation};
	XXH3_state_t hash;

	XXH3_64bits_reset(&hash);
	for (int i = 0; i < lengthof(numbers); i++)
	{
		uint8		le[4];

		std_u32_to_le_bytes(numbers[i], le);
		XXH3_64bits_update(&hash, le, sizeof(le));
	}
	if (by_bytes)
		XXH3_64bits_update(&hash, key, key_len);
	return (uint32) (XXH3_64bits_digest(&hash) % HOST_STRIPES);
}

/*
 * txn_change_end_frame_t is end's state across its pad and its prepares:
 * what the end takes, its row change and its read, each while live, and the
 * row's parts the push reads.
 */
typedef struct txn_change_end_frame_t
{
	HostIoIo   *io;
	HostSharedRecord record;
	HostIoWaits *waits;
	uint32		stripe;
	TxnChangeDecide decide;
	void	   *decide_captures;
	void		(*decide_drop) (void *);
	bool		decide_live;
	void	   *stopped;
	TxnRecordRowChange change;
	bool		change_live;
	TxnMemtableLockMode lock;
	bool		has_decoded;
	HostRow		decoded;
	HostRow		tuple;
	bool		logged;
	bool		has_roomed;
	TxnStatusRoomed roomed;
	size_t		need;
	TxnChangeEnded *result;
	HostError  *error;
	bool		ok;
} txn_change_end_frame_t;

/* txn_change_end_closure_0_captures_t is the push's captures: the end and the encoding it takes, while live. */
typedef struct txn_change_end_closure_0_captures_t
{
	txn_change_end_frame_t *end;
	TxnRecordEncoding encoding;
	bool		encoding_live;
} txn_change_end_closure_0_captures_t;

/* txn_change_end_closure_0_drop frees a prepare's closure's encoding, while live. */
static void
txn_change_end_closure_0_drop(void *captures)
{
	txn_change_end_closure_0_captures_t *inside = captures;

	if (inside->encoding_live)
		txn_record_Encoding_drop(&inside->encoding);
	inside->encoding_live = false;
}

/*
 * txn_change_end_closure_0_frame_t is the push's state across its pad: its
 * read's slice, its stripe's guard, its checkpoint window, and its record's
 * body, each while live.
 */
typedef struct txn_change_end_closure_0_frame_t
{
	txn_change_end_closure_0_captures_t *inside;
	HostIoIo   *io;
	TxnMemtableTablePreparedState *prepared;
	HostIoStriping striping;
	TxnMemtableSliceSlice slice;
	bool		slice_live;
	HostKeyGuard guard;
	bool		guard_live;
	HostDelayCheckpoint window;
	bool		window_live;
	StdVec		body;
	bool		body_live;
	bool	   *has_result;
	TxnChangeEnded *result;
	HostError  *error;
	bool		ok;
} txn_change_end_closure_0_frame_t;

/*
 * txn_change_end_closure_0_body reads the row's slice, which decide answers,
 * and then under the key's stripe checks that the key's newest version is
 * still the one the read found, answering none to start over when it isn't,
 * and pushes the version in a checkpoint window after a logged change's
 * record.
 */
static void
txn_change_end_closure_0_body(void *frame_)
{
	txn_change_end_closure_0_frame_t *frame = frame_;
	txn_change_end_frame_t *end = frame->inside->end;
	TxnRecordRowChange *change = &end->change;
	static const char *const pauses[] = {TXN_CHANGE_LOGGED};
	TxnChangeRead read;
	bool		has_newest;
	TxnMemtableVersionHandle newest;
	HostIoKeyIo key_io;
	HostIoTakesShort io;
	bool		has_found;
	TxnMemtableVersionHandle found;
	TxnMemtableVersionChange pushed = {.writer = change->writer, .op = &change->op, .lock = end->lock};
	HostLsn		cid_lsn;
	TxnMemtableVersionHandle handle;
	bool		done;

	if (!txn_memtable_slice_row_versions(frame->io, txn_memtable_table_PreparedState_hold(frame->prepared),
										 end->record, change->key.ptr, change->key.len, end->tuple, 0, &frame->slice,
										 frame->error))
		return;
	frame->slice_live = true;
	if (!end->decide(end->decide_captures, frame->io, frame->slice.versions.ptr, frame->slice.versions.len, &read,
					 end->stopped, frame->error))
		return;
	if (read.tag == TXN_CHANGE_READ_STOP)
	{
		*frame->has_result = true;
		*frame->result = TXN_CHANGE_ENDED_STOPPED;
		frame->ok = true;
		return;
	}
	has_newest = frame->slice.has_head;
	newest = frame->slice.head;
	frame->slice_live = false;
	txn_memtable_slice_Slice_drop(&frame->slice);
	if (!host_key_lock(&frame->striping, frame->io, &end->stripe, 1, &frame->guard, frame->error))
		return;
	frame->guard_live = true;
	key_io = host_KeyGuard_io(&frame->guard);
	io.ops = &HOST_IO_KEY_IO_TAKES_SHORT_OPS;
	io.self = &key_io;
	if (!txn_memtable_table_newest(io, frame->prepared, 0, &has_found, &found, frame->error))
		return;
	if (has_found != has_newest || (has_found && !txn_memtable_version_Handle_PartialEq_eq(&found, &newest)))
	{
		*frame->has_result = false;
		frame->ok = true;
		return;
	}
	if (!host_delay_checkpoint(HOST_IO_TAKES_SHORT_IO_DOORS(io), pauses, lengthof(pauses), &frame->window,
							   frame->error))
		return;
	frame->window_live = true;
	if (end->logged && end->has_decoded &&
		!host_wal_new_cid(HOST_IO_TAKES_SHORT_IO_DOORS(io), &change->storage, end->decoded,
						  txn_change_changed_cid(read.u.go.shown, read.u.go.cmin, change->writer.command), &cid_lsn,
						  frame->error))
		return;
	if (end->logged)
	{
		frame->inside->encoding_live = false;
		txn_record_RowChange_encode_with(change, &frame->inside->encoding, &frame->body);
		frame->body_live = true;
		if (!host_wal_insert(HOST_IO_TAKES_SHORT_IO_DOORS(io), HOST_WAL_KIND_ROW_CHANGE, frame->body.ptr,
							 frame->body.len, &pushed.lsn, frame->error))
			return;
		pushed.has_lsn = true;
		frame->body_live = false;
		std_vec_free_Checked(&frame->body, 1, 1);
	}
	if (!host_pause(HOST_SHARED_KEY_IO_PAUSES(&key_io), TXN_CHANGE_LOGGED, frame->error))
		return;
	pushed.shown = read.u.go.shown;
	if (read.u.go.shown.tag == TXN_MEMTABLE_VERSION_SHOWN_COMBO && read.u.go.has_row)
		done = txn_memtable_table_finish_showing(io, frame->prepared, 0, &pushed, read.u.go.row,
												 read.u.go.shown.u.combo, end->has_roomed, end->roomed, &handle,
												 frame->error);
	else
		done = txn_memtable_table_finish(io, frame->prepared, &pushed, end->has_roomed, end->roomed, &handle,
										 frame->error);
	if (!done)
		return;
	frame->window_live = false;
	host_DelayCheckpoint_finish(&frame->window);
	*frame->has_result = true;
	*frame->result = TXN_CHANGE_ENDED_PUSHED;
	frame->ok = true;
}

/* txn_change_end_closure_0_pad frees what the push holds live: its record, window, guard, and slice. */
static void
txn_change_end_closure_0_pad(void *frame_)
{
	txn_change_end_closure_0_frame_t *frame = frame_;

	if (frame->body_live)
		std_vec_free_Checked(&frame->body, 1, 1);
	if (frame->window_live)
		host_DelayCheckpoint_drop(&frame->window);
	if (frame->guard_live)
		host_KeyGuard_drop(&frame->guard);
	if (frame->slice_live)
		txn_memtable_slice_Slice_drop(&frame->slice);
	frame->body_live = frame->window_live = frame->guard_live = frame->slice_live = false;
}

/* txn_change_end_closure_0 is the prepare's closure of an end: its push, or none to start over. */
static bool
txn_change_end_closure_0(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared, HostIoStriping striping,
						 bool *has_result, TxnChangeEnded *result, HostError *error)
{
	txn_change_end_closure_0_frame_t frame = {.inside = captures, .io = io, .prepared = prepared, .striping = striping,
	.has_result = has_result, .result = result, .error = error};

	std_landing(&frame, txn_change_end_closure_0_body, txn_change_end_closure_0_pad);
	txn_change_end_closure_0_pad(&frame);
	return frame.ok;
}

/* txn_change_end_body counts the key's touch, spilling past the threshold, then prepares until a push or a stop. */
static void
txn_change_end_body(void *frame_)
{
	txn_change_end_frame_t *frame = frame_;
	TxnRecordRowChange *change = &frame->change;
	uint64		top = change->writer.has_top ? change->writer.top : change->writer.xid;
	uint64		touched;
	bool		spilled;

	if (!txn_written_touched(frame->io, top, &change->storage, TXN_WRITTEN_OF_ROWS, change->key.ptr, change->key.len,
							 frame->need, &touched, frame->error))
		return;
	if (touched >= txn_spill_threshold() &&
		!txn_spill_spilled(frame->io, &change->storage, TXN_WRITTEN_OF_ROWS, top, &spilled, frame->error))
		return;
	for (;;)
	{
		txn_change_end_closure_0_captures_t inside = {.end = frame};
		txn_TupleRefSliceU8Usize_t rows = {{change->key.ptr, change->key.len}, frame->need};
		bool		has_ended;

		if (!host_key_wait(frame->waits, HOST_IO_IO_IO_DOORS(frame->io), frame->stripe, frame->error))
			return;
		txn_record_RowChange_encoding(change, &inside.encoding);
		inside.encoding_live = true;
		if (!txn_memtable_table_prepare_striped_OptionEnded(frame->io, frame->waits, frame->record, &rows, 1, NULL, 0,
															false, txn_change_end_closure_0, &inside,
															txn_change_end_closure_0_drop, &has_ended, frame->result,
															frame->error))
			return;
		if (has_ended)
		{
			frame->ok = true;
			return;
		}
	}
}

/* txn_change_end_pad frees what end took: its row change and its read. */
static void
txn_change_end_pad(void *frame_)
{
	txn_change_end_frame_t *frame = frame_;

	if (frame->change_live)
		txn_record_RowChange_drop(&frame->change);
	if (frame->decide_live && frame->decide_drop != NULL)
		frame->decide_drop(frame->decide_captures);
	frame->change_live = frame->decide_live = false;
}

/*
 * txn_change_end pushes a change that ends or locks a row, taking it, as its
 * key's newest, under the state the caller holds: one prepare of the push,
 * then under the prepare's hold a read of the row's slice of its key's
 * versions, which decide answers, then under the key's stripe, which a
 * striping token made from the caller's Waits lends, a recheck that the
 * key's newest version is still the one the read found, starting over when
 * it isn't, and a window that appends a logged change's row change record
 * and pushes the version with its LSN. A read that stops returns Stopped, its
 * T in the caller's bytes, once the prepare gave its reservations back. A
 * lock is never logged, so it has no record and no LSN. Before its prepare it
 * records the key in the transaction's list, which an abort prunes.
 */
bool
txn_change_end(HostIoIo *io, const HostSharedHeld *held, TxnChangeEnding *row, HostIoWaits *waits, uint32 stripe,
			   TxnChangeDecide decide, void *decide_captures, void (*decide_drop) (void *), void *stopped,
			   TxnChangeEnded *result, HostError *error)
{
	bool		lock = row->op.tag == TXN_MEMTABLE_OP_LOCK || row->op.tag == TXN_MEMTABLE_OP_LOCK_ROW;
	TxnMemtableVersionShown combo = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMBO};
	txn_change_end_frame_t frame = {.io = io, .record = host_shared_Held_record(held), .waits = waits,
		.stripe = stripe, .decide = decide, .decide_captures = decide_captures, .decide_drop = decide_drop,
		.decide_live = true, .stopped = stopped, .change_live = true, .lock = row->lock,
		.has_decoded = row->has_decoded, .decoded = row->decoded, .tuple = row->tuple,
		.logged = row->logged && !lock, .has_roomed = !lock, .roomed = row->roomed, .result = result,
	.error = error};

	frame.change.writer = row->writer;
	frame.change.storage = row->storage;
	frame.change.table = row->table;
	frame.change.key = row->key;
	frame.change.op = row->op;
	frame.change.no_key = row->no_key;
	std_vec_new(&frame.change.blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
	std_vec_new(&frame.change.kept, sizeof(size_t), _Alignof(size_t));
	frame.need = txn_memtable_version_encoded(&frame.change.writer, &frame.change.op, combo, frame.logged, 0);
	std_landing(&frame, txn_change_end_body, txn_change_end_pad);
	txn_change_end_pad(&frame);
	return frame.ok;
}

/* txn_change_redo_closure_0_captures_t is a redo's prepare's closure's captures: the version it pushes and its slot. */
typedef struct txn_change_redo_closure_0_captures_t
{
	const TxnMemtableVersionChange *pushed;
	bool		has_roomed;
	TxnStatusRoomed roomed;
} txn_change_redo_closure_0_captures_t;

/* txn_change_redo_closure_0 is a redo's prepare's closure: it pushes the replayed version. */
static bool
txn_change_redo_closure_0(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
						  TxnMemtableVersionHandle *result, HostError *error)
{
	txn_change_redo_closure_0_captures_t *inside = captures;

	return txn_memtable_table_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, inside->pushed, inside->has_roomed,
									 inside->roomed, result, error);
}

/*
 * txn_change_redo applies a replayed row change that ends a row, or that
 * changes an index's entry, in the startup process or on a standby, with no
 * catalog access: it pushes the change's version with the record's LSN, its
 * shown command replayed, unless the checkpoint's runs brought it back,
 * which the delta holds by its key and the record's LSN.
 */
bool
txn_change_redo(HostIoIo *io, const HostSharedHeld *held, const TxnRecordRowChange *change, uint64 lsn,
				HostError *error)
{
	HostSharedRecord record = host_shared_Held_record(held);
	TxnMemtableVersionShown replayed = {.tag = TXN_MEMTABLE_VERSION_SHOWN_REPLAYED};
	TxnMemtableVersionChange pushed = {.writer = change->writer, .op = &change->op, .shown = replayed,
	.has_lsn = true, .lsn = lsn};
	txn_change_redo_closure_0_captures_t inside = {.pushed = &pushed};
	uint64		top = change->writer.has_top ? change->writer.top : change->writer.xid;
	txn_TupleRefSliceU8Usize_t rows = {{change->key.ptr, change->key.len}};
	uint64		replayed_bytes;
	bool		holds;
	bool		spilled;
	TxnMemtableVersionHandle handle;

	if (!txn_status_redo_roomed(io, &change->writer, &inside.has_roomed, &inside.roomed, error) ||
		!txn_memtable_table_holds(io, record, change->key.ptr, change->key.len, lsn, &holds, error))
		return false;
	if (holds)
		return true;
	pushed.lock = change->no_key ? TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE : TXN_MEMTABLE_LOCK_MODE_UPDATE;
	rows.f1 = txn_memtable_version_encoded(&change->writer, &change->op, replayed, true, lsn);
	if (!txn_spill_replayed(io, &change->storage, TXN_WRITTEN_OF_ROWS, top, rows.f1, &replayed_bytes, error))
		return false;
	if (replayed_bytes >= txn_spill_threshold() &&
		!txn_spill_spilled(io, &change->storage, TXN_WRITTEN_OF_ROWS, top, &spilled, error))
		return false;
	return txn_memtable_table_prepare_Handle(io, record, &rows, 1, NULL, 0, false, txn_change_redo_closure_0, &inside,
											 NULL, &handle, error);
}
