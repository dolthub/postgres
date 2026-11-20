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
 * An index's entry changes: a put, a delete, or a counted entry's added or
 * removed copy, pushed into the index's own storage's delta in one
 * checkpoint window after one prepare, a logged one's after its row change
 * record, taking no place. An index that keeps records changes an entry's
 * record with it, in the same prepare and window.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/unwind.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/counted.h"
#include "doltgres/txn/entry.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/outcome.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/visible.h"
#include "doltgres/txn/written.h"

/* The prepare whose closure pushes an entry's changes. */
#define TXN_MEMTABLE_TABLE_RS Unit
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#include "doltgres/txn/memtable/table_template.h"

/* TxnEntryStepTag is Step's variants. */
typedef enum TxnEntryStepTag
{
	/* The entry was pushed. */
	TXN_ENTRY_STEP_DONE,
	/* An entry exists at the key, with its value. */
	TXN_ENTRY_STEP_EXISTS,
	/* A running writer to wait for, with a speculative insert's token. */
	TXN_ENTRY_STEP_WAIT,
	/* The read moved, so the resolve starts over. */
	TXN_ENTRY_STEP_RETRY,
} TxnEntryStepTag;

/* TxnEntryStep is Step: how a unique entry's turn ended. */
typedef struct TxnEntryStep
{
	TxnEntryStepTag tag;
	union
	{
		/* exists is a Vec<u8>. */
		StdVec		exists;
		struct
		{
			HostXid		f0;
			bool		has_f1;
			uint32		f1;
		}			wait;
	}			u;
} TxnEntryStep;

/* The prepare whose closure decides a unique entry's turn. */
#define TXN_MEMTABLE_TABLE_RS Step
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , TxnEntryStep *result, HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#define TXN_MEMTABLE_TABLE_DEFINE
#define TXN_MEMTABLE_TABLE_RESULT_FIELDS TxnEntryStep *result;
#define TXN_MEMTABLE_TABLE_RESULT_INIT , result
#define TXN_MEMTABLE_TABLE_RESULT_ARGS(c) , (c)->result
#define TXN_MEMTABLE_TABLE_RESULT_ERROR error
#include "doltgres/txn/memtable/table_template.h"

/* The pin whose scope reads a base entry's stamps. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultVecStamp
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , StdVec *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS StdVec *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* TXN_ENTRY_LOGGED is the pause inside an entry's checkpoint window, after each record. */
#define TXN_ENTRY_LOGGED "doltgres-entry-logged"

/* TXN_ENTRY_STAMP is the size and alignment of a stamp. */
#define TXN_ENTRY_STAMP sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp)

/* TXN_ENTRY_ENCODING is the size and alignment of a row change's encoding. */
#define TXN_ENTRY_ENCODING sizeof(TxnRecordEncoding), _Alignof(TxnRecordEncoding)

/* txn_entry_TupleRowChangeUsize_t is a formed change: its row change and the longest record its version takes. */
typedef struct txn_entry_TupleRowChangeUsize_t
{
	TxnRecordRowChange f0;
	size_t		f1;
} txn_entry_TupleRowChangeUsize_t;

/* TXN_ENTRY_CHANGE is the size and alignment of a formed change. */
#define TXN_ENTRY_CHANGE sizeof(txn_entry_TupleRowChangeUsize_t), _Alignof(txn_entry_TupleRowChangeUsize_t)

/*
 * TxnEntryFormed is Formed: an entry's change formed for its push, its row
 * change, then its record's when the index keeps records, each with the
 * longest record its version takes, its writer's slot in the commit status
 * table, and whether its storage is logged.
 */
typedef struct TxnEntryFormed
{
	/* changes is a Vec<(RowChange, usize)>. */
	StdVec		changes;
	TxnStatusRoomed roomed;
	bool		logged;
} TxnEntryFormed;

/* txn_entry_record returns the key of a row's record: RECORD, then the row's identity. */
void
txn_entry_record(const uint8 *identity, size_t identity_len, StdVec *result)
{
	static const uint8 record[] = {TXN_ENTRY_RECORD};

	std_vec_with_capacity_Checked(result, 1 + identity_len, 1, 1);
	std_vec_extend_from_slice_Checked(result, record, 1, 1, 1);
	std_vec_extend_from_slice_Checked(result, identity, identity_len, 1, 1);
}

/*
 * txn_entry_based reads an index storage's base leaf entry: a keyed table's
 * entry, its value the tuple, keeps no stamp, and a keyless table's counts
 * its row's copies before its tuple with a stamp for each; it returns the
 * tuple, refusing as data corruption a stamp on a keyed table's entry and a
 * count that differs from a keyless one's stamps.
 */
bool
txn_entry_based(const uint8 *value, size_t value_len, const ChunksTrailerStamp *stamps, size_t stamps_len,
				bool keyless, StdBytes *result, HostError *error)
{
	ChunksErrorError failed;
	StdString	message;
	uint64		count;

	if (!keyless)
	{
		if (stamps_len == 0)
		{
			result->ptr = value;
			result->len = value_len;
			return true;
		}
		std_string_from_Checked(&message, "a keyed table's index entry keeps a stamp", 41);
		*error = host_Error_fork((const uint8 *) "XX001", &message);
		return false;
	}
	if (!txn_counted_keyless_parts(value, value_len, &count, result, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	if (count == (uint64) stamps_len)
		return true;
	std_string_from_Checked(&message, "an index entry's count of copies differs from its stamps", 56);
	*error = host_Error_fork((const uint8 *) "XX001", &message);
	return false;
}

/* txn_entry_Formed_drop frees a formed entry's row changes. */
static void
txn_entry_Formed_drop(TxnEntryFormed *self)
{
	for (size_t i = 0; i < self->changes.len; i++)
		txn_record_RowChange_drop(&STD_VEC_AT(&self->changes, txn_entry_TupleRowChangeUsize_t, i).f0);
	std_vec_free_Checked(&self->changes, TXN_ENTRY_CHANGE);
}

/*
 * txn_entry_formed_frame_t is formed's state across its pad: the entry and
 * the record it takes, each while not yet moved into a change, and the
 * changes it forms.
 */
typedef struct txn_entry_formed_frame_t
{
	HostIoIo   *io;
	TxnEntryEntry *entry;
	bool		entry_live;
	txn_TupleVecU8Op_t *record;
	bool		record_live;
	TxnEntryFormed *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} txn_entry_formed_frame_t;

/* txn_entry_formed_body forms the entry's change and then its record's, counting each key's touch. */
static void
txn_entry_formed_body(void *frame_)
{
	txn_entry_formed_frame_t *frame = frame_;
	TxnEntryEntry *entry = frame->entry;
	TxnMemtableVersionShown command = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	uint64		top = entry->writer.has_top ? entry->writer.top : entry->writer.xid;

	std_vec_with_capacity_Checked(&frame->result->changes, 2, TXN_ENTRY_CHANGE);
	frame->result->roomed = entry->roomed;
	frame->result->logged = entry->logged;
	frame->result_live = true;
	for (int item = 0; item < 2; item++)
	{
		txn_entry_TupleRowChangeUsize_t formed = {.f0 = {.writer = entry->writer, .storage = entry->storage,
		.table = entry->table, .has_index = true, .index = entry->index}};
		txn_entry_TupleRowChangeUsize_t *pushed;
		uint64		touched;
		bool		spilled;

		if (item == 0)
		{
			formed.f0.key = entry->key;
			formed.f0.op = entry->op;
			frame->entry_live = false;
		}
		else if (frame->record_live)
		{
			formed.f0.key = frame->record->f0;
			formed.f0.op = frame->record->f1;
			frame->record_live = false;
		}
		else
			break;
		std_vec_new(&formed.f0.blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
		std_vec_new(&formed.f0.kept, sizeof(size_t), _Alignof(size_t));
		formed.f1 = txn_memtable_version_encoded(&entry->writer, &formed.f0.op, command, entry->logged, 0);
		std_vec_push_Checked(&frame->result->changes, &formed, TXN_ENTRY_CHANGE);
		pushed = &STD_VEC_AT(&frame->result->changes, txn_entry_TupleRowChangeUsize_t, item);
		if (!txn_written_touched(frame->io, top, &pushed->f0.storage, TXN_WRITTEN_OF_ROWS, pushed->f0.key.ptr,
								 pushed->f0.key.len, pushed->f1, &touched, frame->error))
			return;
		if (touched >= txn_spill_threshold() &&
			!txn_spill_spilled(frame->io, &pushed->f0.storage, TXN_WRITTEN_OF_ROWS, top, &spilled, frame->error))
			return;
	}
	frame->result_live = false;
	frame->ok = true;
}

/* txn_entry_formed_pad frees what formed holds live: the entry and record not yet moved, and the changes. */
static void
txn_entry_formed_pad(void *frame_)
{
	txn_entry_formed_frame_t *frame = frame_;

	if (frame->entry_live)
	{
		std_vec_free_Checked(&frame->entry->key, 1, 1);
		txn_memtable_Op_drop(&frame->entry->op);
	}
	if (frame->record_live)
	{
		std_vec_free_Checked(&frame->record->f0, 1, 1);
		txn_memtable_Op_drop(&frame->record->f1);
	}
	if (frame->result_live)
		txn_entry_Formed_drop(frame->result);
	frame->entry_live = frame->record_live = frame->result_live = false;
}

/*
 * txn_entry_formed forms an entry's change and its record's, taking them,
 * after recording each key in the transaction's list, which an abort prunes,
 * and spilling the transaction's entries in the storage once they pass the
 * spill's threshold.
 */
static bool
txn_entry_formed(HostIoIo *io, TxnEntryEntry *entry, bool has_record, txn_TupleVecU8Op_t *record,
				 TxnEntryFormed *result, HostError *error)
{
	txn_entry_formed_frame_t frame = {.io = io, .entry = entry, .entry_live = true, .record = record,
	.record_live = has_record, .result = result, .error = error};

	std_landing(&frame, txn_entry_formed_body, txn_entry_formed_pad);
	txn_entry_formed_pad(&frame);
	return frame.ok;
}

/* txn_entry_encodings_t is a push's encodings, one per formed change, those from at on not yet taken. */
typedef struct txn_entry_encodings_t
{
	StdVec		encodings;
	size_t		at;
	bool		live;
} txn_entry_encodings_t;

/* txn_entry_encodings_drop frees a push's encodings not yet taken. */
static void
txn_entry_encodings_drop(txn_entry_encodings_t *self)
{
	if (!self->live)
		return;
	for (size_t i = self->at; i < self->encodings.len; i++)
		txn_record_Encoding_drop(&STD_VEC_AT(&self->encodings, TxnRecordEncoding, i));
	std_vec_free_Checked(&self->encodings, TXN_ENTRY_ENCODING);
	self->live = false;
}

/* txn_entry_encode makes each formed change's encoding, before the prepare its push runs in. */
static void
txn_entry_encode(const TxnEntryFormed *formed, txn_entry_encodings_t *result)
{
	std_vec_with_capacity_Checked(&result->encodings, formed->changes.len, TXN_ENTRY_ENCODING);
	result->live = true;
	for (size_t i = 0; i < formed->changes.len; i++)
	{
		TxnRecordEncoding encoding;

		txn_record_RowChange_encoding(&STD_VEC_AT(&formed->changes, txn_entry_TupleRowChangeUsize_t, i).f0,
									  &encoding);
		std_vec_push_Checked(&result->encodings, &encoding, TXN_ENTRY_ENCODING);
	}
}

/* txn_entry_pushed_frame_t is pushed's state across its pad: its window and its record's body, each while live. */
typedef struct txn_entry_pushed_frame_t
{
	HostIoTakesShort io;
	HostSharedPauses pauses;
	TxnMemtableTablePreparedState *prepared;
	const TxnEntryFormed *formed;
	txn_entry_encodings_t *encodings;
	HostDelayCheckpoint window;
	bool		window_live;
	StdVec		body;
	bool		body_live;
	HostError  *error;
	bool		ok;
} txn_entry_pushed_frame_t;

/* txn_entry_pushed_body appends each logged change's record and pushes its version, in one window. */
static void
txn_entry_pushed_body(void *frame_)
{
	txn_entry_pushed_frame_t *frame = frame_;
	static const char *const pauses[] = {TXN_ENTRY_LOGGED};
	const TxnEntryFormed *formed = frame->formed;
	TxnMemtableVersionShown command = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};

	if (!host_delay_checkpoint(HOST_IO_TAKES_SHORT_IO_DOORS(frame->io), pauses, lengthof(pauses), &frame->window,
							   frame->error))
		return;
	frame->window_live = true;
	for (size_t row = 0; row < formed->changes.len; row++)
	{
		const TxnRecordRowChange *change = &STD_VEC_AT(&formed->changes, txn_entry_TupleRowChangeUsize_t, row).f0;
		TxnRecordEncoding encoding = STD_VEC_AT(&frame->encodings->encodings, TxnRecordEncoding, row);
		TxnMemtableVersionChange pushed = {.writer = change->writer, .op = &change->op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = command};
		TxnMemtableVersionHandle handle;

		frame->encodings->at = row + 1;
		if (!formed->logged)
			txn_record_Encoding_drop(&encoding);
		else
		{
			txn_record_RowChange_encode_with(change, &encoding, &frame->body);
			frame->body_live = true;
			if (!host_wal_insert(HOST_IO_TAKES_SHORT_IO_DOORS(frame->io), HOST_WAL_KIND_ROW_CHANGE, frame->body.ptr,
								 frame->body.len, &pushed.lsn, frame->error))
				return;
			pushed.has_lsn = true;
			frame->body_live = false;
			std_vec_free_Checked(&frame->body, 1, 1);
		}
		if (!host_pause(frame->pauses, TXN_ENTRY_LOGGED, frame->error) ||
			!txn_memtable_table_finish_row(frame->io, frame->prepared, row, &pushed, true, formed->roomed, &handle,
										   frame->error))
			return;
	}
	frame->window_live = false;
	host_DelayCheckpoint_finish(&frame->window);
	frame->ok = true;
}

/* txn_entry_pushed_pad frees what pushed holds live: its record's body and its window. */
static void
txn_entry_pushed_pad(void *frame_)
{
	txn_entry_pushed_frame_t *frame = frame_;

	if (frame->body_live)
		std_vec_free_Checked(&frame->body, 1, 1);
	if (frame->window_live)
		host_DelayCheckpoint_drop(&frame->window);
	frame->body_live = frame->window_live = false;
}

/*
 * txn_entry_pushed appends each logged change's row change record, by its
 * encoding made before the window, which it takes, and pushes its version
 * with its LSN, showing its writer's command, in one checkpoint window,
 * under a prepare of the pushes.
 */
static bool
txn_entry_pushed(HostIoTakesShort io, HostSharedPauses pauses, TxnMemtableTablePreparedState *prepared,
				 const TxnEntryFormed *formed, txn_entry_encodings_t *encodings, HostError *error)
{
	txn_entry_pushed_frame_t frame = {.io = io, .pauses = pauses, .prepared = prepared, .formed = formed,
	.encodings = encodings, .error = error};

	std_landing(&frame, txn_entry_pushed_body, txn_entry_pushed_pad);
	txn_entry_pushed_pad(&frame);
	return frame.ok;
}

/* txn_entry_rows returns the keys and the longest records of a formed entry's changes, the prepare's rows. */
static size_t
txn_entry_rows(const TxnEntryFormed *formed, txn_TupleRefSliceU8Usize_t rows[2])
{
	for (size_t i = 0; i < formed->changes.len; i++)
	{
		const txn_entry_TupleRowChangeUsize_t *change = &STD_VEC_AT(&formed->changes, txn_entry_TupleRowChangeUsize_t,
																	i);

		rows[i].f0.ptr = change->f0.key.ptr;
		rows[i].f0.len = change->f0.key.len;
		rows[i].f1 = change->f1;
	}
	return formed->changes.len;
}

/* txn_entry_change_closure_2_captures_t is change's closure's captures: the formed entry and its encodings. */
typedef struct txn_entry_change_closure_2_captures_t
{
	const TxnEntryFormed *formed;
	txn_entry_encodings_t encodings;
} txn_entry_change_closure_2_captures_t;

/* txn_entry_change_closure_2 is change's prepare's closure: the entry's pushes. */
static bool
txn_entry_change_closure_2(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared, HostError *error)
{
	txn_entry_change_closure_2_captures_t *inside = captures;

	return txn_entry_pushed(HOST_IO_IO_TAKES_SHORT(io), HOST_SHARED_IO_PAUSES(io), prepared, inside->formed,
							&inside->encodings, error);
}

/* txn_entry_change_closure_2_drop frees the closure's encodings not yet taken. */
static void
txn_entry_change_closure_2_drop(void *captures)
{
	txn_entry_change_closure_2_captures_t *inside = captures;

	txn_entry_encodings_drop(&inside->encodings);
}

/* txn_entry_change_frame_t is change's state across its pad: the formed entry and its closure, each while live. */
typedef struct txn_entry_change_frame_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	TxnEntryFormed formed;
	bool		formed_live;
	txn_entry_change_closure_2_captures_t inside;
	bool		inside_live;
	HostError  *error;
	bool		ok;
} txn_entry_change_frame_t;

/* txn_entry_change_body makes the pushes' encodings and runs their prepare. */
static void
txn_entry_change_body(void *frame_)
{
	txn_entry_change_frame_t *frame = frame_;
	txn_TupleRefSliceU8Usize_t rows[2];
	size_t		rows_len = txn_entry_rows(&frame->formed, rows);

	frame->inside.formed = &frame->formed;
	frame->inside_live = true;
	txn_entry_encode(&frame->formed, &frame->inside.encodings);
	frame->inside_live = false;
	frame->ok = txn_memtable_table_prepare_Unit(frame->io, host_shared_Held_record(frame->held), rows, rows_len, NULL,
												0, false, txn_entry_change_closure_2, &frame->inside,
												txn_entry_change_closure_2_drop, frame->error);
}

/* txn_entry_change_pad frees what change holds live: its closure's encodings and its formed entry. */
static void
txn_entry_change_pad(void *frame_)
{
	txn_entry_change_frame_t *frame = frame_;

	if (frame->inside_live)
		txn_entry_change_closure_2_drop(&frame->inside);
	if (frame->formed_live)
		txn_entry_Formed_drop(&frame->formed);
	frame->inside_live = frame->formed_live = false;
}

/*
 * txn_entry_change pushes an entry's change, and its record's when the index
 * keeps records, taking them, as their keys' newest, under the index's
 * storage's state the caller holds: it records the keys in the
 * transaction's list, spills the transaction's entries in the storage once
 * they pass the spill's threshold, makes one prepare of the pushes, and in
 * one checkpoint window appends each logged change's row change record and
 * pushes its version with its LSN, showing its writer's command. It reads no
 * version and takes no stripe.
 */
bool
txn_entry_change(HostIoIo *io, const HostSharedHeld *held, TxnEntryEntry *entry, bool has_record,
				 txn_TupleVecU8Op_t *record, HostError *error)
{
	txn_entry_change_frame_t frame = {.io = io, .held = held, .error = error};

	if (!txn_entry_formed(io, entry, has_record, record, &frame.formed, error))
		return false;
	frame.formed_live = true;
	std_landing(&frame, txn_entry_change_body, txn_entry_change_pad);
	txn_entry_change_pad(&frame);
	return frame.ok;
}

/* txn_entry_base_stamps_closure_1_captures_t is base_stamps' pin scope's captures: the base's root and the key. */
typedef struct txn_entry_base_stamps_closure_1_captures_t
{
	TreesStoreTree *root;
	const uint8 *key;
	size_t		key_len;
	const TreesKeyOrder *order;
} txn_entry_base_stamps_closure_1_captures_t;

/* txn_entry_base_stamps_closure_1_frame_t is the pin scope's state across its pad: its cursor, while live. */
typedef struct txn_entry_base_stamps_closure_1_frame_t
{
	txn_entry_base_stamps_closure_1_captures_t *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesCursorCursor cursor;
	bool		cursor_live;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
} txn_entry_base_stamps_closure_1_frame_t;

/* txn_entry_base_stamps_closure_1_body seeks the key and reads the stamps of the entry it finds there. */
static void
txn_entry_base_stamps_closure_1_body(void *frame_)
{
	txn_entry_base_stamps_closure_1_frame_t *frame = frame_;
	txn_entry_base_stamps_closure_1_captures_t *captures = frame->captures;
	StdBytes	found;
	bool		same = false;

	if (!trees_cursor_Cursor_seek(frame->io, frame->pin, TREES_STORE_TREE_NAMED(captures->root), captures->key,
								  captures->key_len, captures->order, &frame->cursor, frame->error))
		return;
	frame->cursor_live = true;
	if (trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		found = trees_cursor_Cursor_key(&frame->cursor);
		same = found.len == captures->key_len && memcmp(found.ptr, captures->key, found.len) == 0;
	}
	if (same)
		frame->ok = trees_cursor_Cursor_stamps(&frame->cursor, frame->io, frame->pin, frame->result, frame->error);
	else
	{
		std_vec_new(frame->result, TXN_ENTRY_STAMP);
		frame->ok = true;
	}
}

/* txn_entry_base_stamps_closure_1_pad frees the pin scope's cursor, while live. */
static void
txn_entry_base_stamps_closure_1_pad(void *frame_)
{
	txn_entry_base_stamps_closure_1_frame_t *frame = frame_;

	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
	frame->cursor_live = false;
}

/* txn_entry_base_stamps_closure_1 is base_stamps' pin scope: the stamps of the base's entry at the key. */
static bool
txn_entry_base_stamps_closure_1(void *captures, HostIoIo *io, TreesPinPin *pin, StdVec *result,
								ChunksErrorError *error)
{
	txn_entry_base_stamps_closure_1_frame_t frame = {.captures = captures, .io = io, .pin = pin, .result = result,
	.error = error};

	std_landing(&frame, txn_entry_base_stamps_closure_1_body, txn_entry_base_stamps_closure_1_pad);
	txn_entry_base_stamps_closure_1_pad(&frame);
	return frame.ok;
}

/* txn_entry_base_stamps_frame_t is base_stamps' state across its pad: the base record it read, while live. */
typedef struct txn_entry_base_stamps_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const uint8 *key;
	size_t		key_len;
	const TreesKeyOrder *order;
	TxnBaseBase base;
	bool		base_live;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} txn_entry_base_stamps_frame_t;

/* txn_entry_base_stamps_body reads the base record, then the stamps at the key in its tree. */
static void
txn_entry_base_stamps_body(void *frame_)
{
	txn_entry_base_stamps_frame_t *frame = frame_;
	txn_entry_base_stamps_closure_1_captures_t captures = {.key = frame->key, .key_len = frame->key_len,
	.order = frame->order};
	ChunksErrorError failed;
	bool		some;

	if (!txn_state_base_record(frame->io, frame->held, &some, &frame->base, frame->error))
		return;
	frame->base_live = some;
	if (!some || !frame->base.has_root)
	{
		std_vec_new(frame->result, TXN_ENTRY_STAMP);
		frame->ok = true;
		return;
	}
	captures.root = &frame->base.root;
	if (!trees_pin_pin_ResultVecStamp(frame->io, frame->store, txn_entry_base_stamps_closure_1, &captures, NULL,
									  frame->result, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->ok = true;
}

/* txn_entry_base_stamps_pad frees the base record base_stamps read, while live. */
static void
txn_entry_base_stamps_pad(void *frame_)
{
	txn_entry_base_stamps_frame_t *frame = frame_;

	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->base_live = false;
}

/*
 * txn_entry_base_stamps reads the stamps of the base's entry at a key, each
 * a copy of a keyless table's row, none when the base holds no entry there.
 */
static bool
txn_entry_base_stamps(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const uint8 *key,
					  size_t key_len, const TreesKeyOrder *order, StdVec *result, HostError *error)
{
	txn_entry_base_stamps_frame_t frame = {.io = io, .store = store, .held = held, .key = key, .key_len = key_len,
	.order = order, .result = result, .error = error};

	std_landing(&frame, txn_entry_base_stamps_body, txn_entry_base_stamps_pad);
	txn_entry_base_stamps_pad(&frame);
	return frame.ok;
}

/*
 * TxnEntryCopies is Copies: what a counted key's copies answer the insert of
 * a copy under a tuple id, a live copy under that tuple id, which is counted
 * already, or the resolve's answer for the other copies.
 */
typedef struct TxnEntryCopies
{
	bool		counted;
	TxnVisibleResolved other;
} TxnEntryCopies;

/* txn_entry_TupleU64Bool_t is a decided copy: its tuple id and whether it is live. */
typedef struct txn_entry_TupleU64Bool_t
{
	uint64		f0;
	bool		f1;
} txn_entry_TupleU64Bool_t;

/* TXN_ENTRY_DECIDED is the size and alignment of a decided copy. */
#define TXN_ENTRY_DECIDED sizeof(txn_entry_TupleU64Bool_t), _Alignof(txn_entry_TupleU64Bool_t)

/* txn_entry_copies_frame_t is copies' state across its pad: the copies it decided and the value it found. */
typedef struct txn_entry_copies_frame_t
{
	HostIoIo   *io;
	TxnMemtableVersionVersion *versions;
	size_t		versions_len;
	const ChunksTrailerStamp *stamps;
	size_t		stamps_len;
	StdBytes	based;
	HostRow		row;
	StdVec		decided;
	bool		decided_live;
	bool		has_value;
	StdVec		value;
	TxnEntryCopies *result;
	HostError  *error;
	bool		ok;
} txn_entry_copies_frame_t;

/* txn_entry_decided reports whether a copy under a tuple id is decided, and when it is, whether it is live. */
static bool
txn_entry_decided(const StdVec *decided, uint64 tuple, bool *live)
{
	for (size_t i = 0; i < decided->len; i++)
	{
		const txn_entry_TupleU64Bool_t *copy = &STD_VEC_AT(decided, txn_entry_TupleU64Bool_t, i);

		if (copy->f0 == tuple)
		{
			*live = copy->f1;
			return true;
		}
	}
	return false;
}

/* txn_entry_copies_body decides each copy by its versions and then by the base's stamps. */
static void
txn_entry_copies_body(void *frame_)
{
	txn_entry_copies_frame_t *frame = frame_;
	bool		has_wait = false;
	HostXid		wait = {0};
	bool		has_token = false;
	uint32		token = 0;
	bool		live;
	StdString	message;

	std_vec_new(&frame->decided, TXN_ENTRY_DECIDED);
	frame->decided_live = true;
	for (size_t i = 0; i < frame->versions_len; i++)
	{
		TxnMemtableVersionVersion *version = &frame->versions[i];
		TxnMemtableVersionRecord record;
		TxnMemtableVersionOpRef op;
		txn_entry_TupleU64Bool_t copy;
		StdBytes	adds = {0};
		HostXid		xid;
		HostStatus	status;

		if (txn_memtable_version_Version_killed(version))
			continue;
		record = txn_memtable_version_Version_record(version);
		op = txn_memtable_version_Record_op(&record);
		if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_ADD)
		{
			copy.f0 = op.u.add.tuple.f0;
			adds = op.u.add.value;
		}
		else if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_REMOVE)
			copy.f0 = op.u.remove.f0.f0;
		else
			continue;
		if (txn_entry_decided(&frame->decided, copy.f0, &live))
			continue;
		xid.f0 = txn_memtable_version_Record_writer(&record).xid;
		if (host_is_current(HOST_IO_IO_IO_DOORS(frame->io), xid))
			status = HOST_STATUS_COMMITTED;
		else if (!txn_outcome_outcome(frame->io, version, &status, frame->error))
			return;
		if (status == HOST_STATUS_ABORTED)
			continue;
		if (status == HOST_STATUS_IN_PROGRESS)
		{
			if (!has_wait)
			{
				has_wait = true;
				wait = xid;
				has_token = txn_memtable_version_Version_token(version, &token);
			}
			copy.f1 = false;
			std_vec_push_Checked(&frame->decided, &copy, TXN_ENTRY_DECIDED);
			continue;
		}
		copy.f1 = adds.ptr != NULL;
		std_vec_push_Checked(&frame->decided, &copy, TXN_ENTRY_DECIDED);
		if (!frame->has_value && adds.ptr != NULL)
		{
			std_slice_to_vec_Checked(&frame->value, adds.ptr, adds.len, 1, 1);
			frame->has_value = true;
		}
	}
	for (size_t i = 0; i < frame->stamps_len; i++)
	{
		txn_entry_TupleU64Bool_t copy = {frame->stamps[i].tuple, true};
		StdBytes	tuple;

		if (txn_entry_decided(&frame->decided, copy.f0, &live))
			continue;
		std_vec_push_Checked(&frame->decided, &copy, TXN_ENTRY_DECIDED);
		if (!frame->has_value && frame->based.ptr != NULL)
		{
			if (!txn_entry_based(frame->based.ptr, frame->based.len, frame->stamps, frame->stamps_len, true, &tuple,
								 frame->error))
				return;
			std_slice_to_vec_Checked(&frame->value, tuple.ptr, tuple.len, 1, 1);
			frame->has_value = true;
		}
	}
	frame->result->counted = false;
	if (txn_entry_decided(&frame->decided, frame->row.f0, &live) && live)
	{
		frame->result->counted = true;
		frame->ok = true;
		return;
	}
	for (size_t i = 0; i < frame->decided.len; i++)
	{
		const txn_entry_TupleU64Bool_t *copy = &STD_VEC_AT(&frame->decided, txn_entry_TupleU64Bool_t, i);

		if (copy->f0 == frame->row.f0 || !copy->f1)
			continue;
		if (!frame->has_value)
		{
			std_string_from_Checked(&message, "a counted key's live copy has no value", 38);
			*frame->error = host_Error_fork((const uint8 *) "XX000", &message);
			return;
		}
		frame->result->other.tag = TXN_VISIBLE_RESOLVED_EXISTS;
		frame->result->other.u.exists.f0 = frame->value;
		frame->result->other.u.exists.has_f1 = false;
		frame->has_value = false;
		frame->ok = true;
		return;
	}
	if (has_wait)
	{
		frame->result->other.tag = TXN_VISIBLE_RESOLVED_WAIT;
		frame->result->other.u.wait.f0 = wait;
		frame->result->other.u.wait.has_f1 = false;
		frame->result->other.u.wait.has_f2 = has_token;
		frame->result->other.u.wait.f2 = token;
	}
	else
		frame->result->other.tag = TXN_VISIBLE_RESOLVED_FREE;
	frame->ok = true;
}

/* txn_entry_copies_pad frees what copies holds live: its decided copies and a value it found and didn't answer. */
static void
txn_entry_copies_pad(void *frame_)
{
	txn_entry_copies_frame_t *frame = frame_;

	if (frame->decided_live)
		std_vec_free_Checked(&frame->decided, TXN_ENTRY_DECIDED);
	if (frame->has_value)
		std_vec_free_Checked(&frame->value, 1, 1);
	frame->decided_live = frame->has_value = false;
}

/*
 * txn_entry_copies decides a counted key for the insert of a copy under a
 * tuple id: each copy is decided by its newest version an aborted writer
 * didn't write, newest first, an Add making it live and a Remove dead, a
 * version of the caller's own transaction as a committed one; a base stamp
 * no such version decides is a committed live copy. A live copy under the
 * tuple id is counted already; another live copy exists, carrying the key's
 * value, the newest live Add's or the base's tuple; and otherwise another
 * copy's running writer is waited for; with none, the key is free.
 */
static bool
txn_entry_copies(HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len,
				 const ChunksTrailerStamp *stamps, size_t stamps_len, StdBytes based, HostRow row,
				 TxnEntryCopies *result, HostError *error)
{
	txn_entry_copies_frame_t frame = {.io = io, .versions = versions, .versions_len = versions_len, .stamps = stamps,
	.stamps_len = stamps_len, .based = based, .row = row, .result = result, .error = error};

	std_landing(&frame, txn_entry_copies_body, txn_entry_copies_pad);
	txn_entry_copies_pad(&frame);
	return frame.ok;
}

/* txn_entry_Step_drop frees an existing entry's value. */
static void
txn_entry_Step_drop(TxnEntryStep *self)
{
	if (self->tag == TXN_ENTRY_STEP_EXISTS)
		std_vec_free_Checked(&self->u.exists, 1, 1);
}

/*
 * txn_entry_unique_frame_t is unique's state across its pad and its
 * prepares: what it takes, the formed entry, its resolve's progress, and
 * each turn's read, stamps, and step, each while live.
 */
typedef struct txn_entry_unique_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	HostSharedRecord record;
	const TxnInsertUniqueness *keyed;
	HostIoWaits *waits;
	HostRow		row;
	bool		partial;
	uint32		stripe;
	bool		counted;
	TxnEntryFormed formed;
	bool		formed_live;
	TxnInsertProgress progress;
	bool		progress_live;
	TxnInsertUnique read;
	bool		read_live;
	StdVec		stamps;
	bool		stamps_live;
	TxnEntryStep step;
	bool		step_live;
	txn_entry_encodings_t encodings;
	TxnEntryChanged *result;
	HostError  *error;
	bool		ok;
} txn_entry_unique_frame_t;

/* txn_entry_unique_closure_2_captures_t is a unique turn's prepare's closure's captures: the entry's frame. */
typedef struct txn_entry_unique_closure_2_captures_t
{
	txn_entry_unique_frame_t *unique;
} txn_entry_unique_closure_2_captures_t;

/* txn_entry_unique_closure_2_drop frees the turn's encodings not yet taken, which the closure holds. */
static void
txn_entry_unique_closure_2_drop(void *captures)
{
	txn_entry_unique_closure_2_captures_t *inside = captures;

	txn_entry_encodings_drop(&inside->unique->encodings);
}

/*
 * txn_entry_unique_closure_2_frame_t is the turn's state across its pad: the
 * key's versions, the answer of its resolve, and its stripe's guard, each
 * while live.
 */
typedef struct txn_entry_unique_closure_2_frame_t
{
	txn_entry_unique_closure_2_captures_t *inside;
	HostIoIo   *io;
	TxnMemtableTablePreparedState *prepared;
	HostIoStriping striping;
	StdVec		versions;
	bool		versions_live;
	TxnVisibleResolved answer;
	bool		answer_live;
	HostKeyGuard guard;
	bool		guard_live;
	TxnEntryStep *result;
	HostError  *error;
	bool		ok;
} txn_entry_unique_closure_2_frame_t;

/* txn_entry_unique_closure_2_versions_drop frees the turn's versions of a counted key, while live. */
static void
txn_entry_unique_closure_2_versions_drop(txn_entry_unique_closure_2_frame_t *frame)
{
	if (!frame->versions_live)
		return;
	frame->versions_live = false;
	txn_memtable_version_versions_drop(&frame->versions, 0);
}

/*
 * txn_entry_unique_closure_2_body resolves the key under the prepare's hold,
 * a counted key by its copies, and for a free key takes the stripe, rechecks
 * the read and the key's newest version, and pushes the entry.
 */
static void
txn_entry_unique_closure_2_body(void *frame_)
{
	txn_entry_unique_closure_2_frame_t *frame = frame_;
	txn_entry_unique_frame_t *unique = frame->inside->unique;
	const TxnRecordRowChange *change = &STD_VEC_AT(&unique->formed.changes, txn_entry_TupleRowChangeUsize_t, 0).f0;
	const HostDeltaHold *hold = txn_memtable_table_PreparedState_hold(frame->prepared);
	bool		has_newest;
	TxnMemtableVersionHandle newest;
	bool		has_found;
	TxnMemtableVersionHandle found;
	HostIoKeyIo key_io;
	HostIoTakesShort io;

	if (unique->counted)
	{
		TxnEntryCopies copies;
		StdBytes	based = {unique->read.has_based ? unique->read.based.ptr : NULL, unique->read.based.len};

		if (!txn_memtable_table_versions(frame->io, hold, unique->record, change->key.ptr, change->key.len,
										 &frame->versions, frame->error))
			return;
		frame->versions_live = true;
		if (!txn_entry_copies(frame->io, frame->versions.ptr, frame->versions.len, unique->stamps.ptr,
							  unique->stamps.len, based, unique->row, &copies, frame->error))
			return;
		if (copies.counted)
		{
			frame->result->tag = TXN_ENTRY_STEP_DONE;
			frame->ok = true;
			return;
		}
		frame->answer = copies.other;
		frame->answer_live = true;
		has_newest = txn_memtable_version_held(frame->versions.ptr, frame->versions.len, &newest);
		txn_entry_unique_closure_2_versions_drop(frame);
	}
	else
	{
		if (!txn_insert_resolving(frame->io, hold, unique->record, change->key.ptr, change->key.len, &unique->read,
								  &frame->answer, &has_newest, &newest, frame->error))
			return;
		frame->answer_live = true;
	}
	if (frame->answer.tag == TXN_VISIBLE_RESOLVED_EXISTS)
	{
		frame->result->tag = TXN_ENTRY_STEP_EXISTS;
		frame->result->u.exists = frame->answer.u.exists.f0;
		frame->answer_live = false;
		frame->ok = true;
		return;
	}
	if (frame->answer.tag == TXN_VISIBLE_RESOLVED_WAIT)
	{
		frame->result->tag = TXN_ENTRY_STEP_WAIT;
		frame->result->u.wait.f0 = frame->answer.u.wait.f0;
		frame->result->u.wait.has_f1 = frame->answer.u.wait.has_f2;
		frame->result->u.wait.f1 = frame->answer.u.wait.f2;
		frame->answer_live = false;
		frame->ok = true;
		return;
	}
	frame->answer_live = false;
	if (!host_key_lock(&frame->striping, frame->io, &unique->stripe, 1, &frame->guard, frame->error))
		return;
	frame->guard_live = true;
	key_io = host_KeyGuard_io(&frame->guard);
	io.ops = &HOST_IO_KEY_IO_TAKES_SHORT_OPS;
	io.self = &key_io;
	frame->result->tag = TXN_ENTRY_STEP_RETRY;
	if (txn_insert_rechecked(&key_io, unique->record, &unique->read, unique->stripe, &unique->progress) !=
		TXN_INSERT_RECHECKED_GO)
	{
		frame->ok = true;
		return;
	}
	if (!txn_memtable_table_newest(io, frame->prepared, 0, &has_found, &found, frame->error))
		return;
	if (has_found != has_newest || (has_found && !txn_memtable_version_Handle_PartialEq_eq(&found, &newest)))
	{
		frame->ok = true;
		return;
	}
	if (!txn_entry_pushed(io, HOST_SHARED_KEY_IO_PAUSES(&key_io), frame->prepared, &unique->formed,
						  &unique->encodings, frame->error))
		return;
	frame->result->tag = TXN_ENTRY_STEP_DONE;
	frame->ok = true;
}

/* txn_entry_unique_closure_2_pad frees what the turn holds live: its guard, its answer, and its versions. */
static void
txn_entry_unique_closure_2_pad(void *frame_)
{
	txn_entry_unique_closure_2_frame_t *frame = frame_;

	if (frame->guard_live)
		host_KeyGuard_drop(&frame->guard);
	if (frame->answer_live)
		txn_visible_Resolved_drop(&frame->answer);
	txn_entry_unique_closure_2_versions_drop(frame);
	frame->guard_live = frame->answer_live = false;
}

/* txn_entry_unique_closure_2 is a unique turn's prepare's closure: its resolve and its push. */
static bool
txn_entry_unique_closure_2(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
						   HostIoStriping striping, TxnEntryStep *result, HostError *error)
{
	txn_entry_unique_closure_2_frame_t frame = {.inside = captures, .io = io, .prepared = prepared,
	.striping = striping, .result = result, .error = error};

	std_landing(&frame, txn_entry_unique_closure_2_body, txn_entry_unique_closure_2_pad);
	txn_entry_unique_closure_2_pad(&frame);
	return frame.ok;
}

/* txn_entry_unique_turn_drop frees a turn's read and stamps, which live for one turn. */
static void
txn_entry_unique_turn_drop(txn_entry_unique_frame_t *frame)
{
	if (frame->read_live)
		txn_insert_Unique_drop(&frame->read);
	if (frame->stamps_live)
		std_vec_free_Checked(&frame->stamps, TXN_ENTRY_STAMP);
	frame->read_live = frame->stamps_live = false;
}

/* txn_entry_unique_body runs the entry's turns until one pushes it, finds an entry, or stops for a partial check. */
static void
txn_entry_unique_body(void *frame_)
{
	txn_entry_unique_frame_t *frame = frame_;
	const TxnRecordRowChange *change = &STD_VEC_AT(&frame->formed.changes, txn_entry_TupleRowChangeUsize_t, 0).f0;
	txn_TupleRefSliceU8Usize_t rows[2];
	size_t		rows_len = txn_entry_rows(&frame->formed, rows);

	frame->counted = change->op.tag == TXN_MEMTABLE_OP_ADD;
	frame->progress = txn_insert_Progress_Default_default();
	frame->progress_live = true;
	for (;;)
	{
		txn_entry_unique_closure_2_captures_t inside = {.unique = frame};

		if (!txn_insert_Progress_holding(&frame->progress) &&
			!host_key_wait(frame->waits, HOST_IO_IO_IO_DOORS(frame->io), frame->stripe, frame->error))
			return;
		if (!txn_insert_based(frame->io, frame->store, frame->held, change->key.ptr, change->key.len, frame->keyed,
							  frame->stripe, &frame->read, frame->error))
			return;
		frame->read_live = true;
		if (frame->counted)
		{
			if (!txn_entry_base_stamps(frame->io, frame->store, frame->held, change->key.ptr, change->key.len,
									   frame->keyed->order, &frame->stamps, frame->error))
				return;
		}
		else
			std_vec_new(&frame->stamps, TXN_ENTRY_STAMP);
		frame->stamps_live = true;
		txn_entry_encode(&frame->formed, &frame->encodings);
		if (!txn_memtable_table_prepare_striped_Step(frame->io, frame->waits, frame->record, rows, rows_len, NULL, 0,
													 false, txn_entry_unique_closure_2, &inside,
													 txn_entry_unique_closure_2_drop, &frame->step, frame->error))
			return;
		frame->step_live = true;
		txn_entry_unique_turn_drop(frame);
		switch (frame->step.tag)
		{
			case TXN_ENTRY_STEP_DONE:
				frame->result->tag = TXN_ENTRY_CHANGED_DONE;
				frame->ok = true;
				return;
			case TXN_ENTRY_STEP_EXISTS:
				frame->result->tag = TXN_ENTRY_CHANGED_EXISTS;
				frame->result->u.exists = frame->step.u.exists;
				frame->step_live = false;
				frame->ok = true;
				return;
			case TXN_ENTRY_STEP_WAIT:
				frame->step_live = false;
				if (frame->partial)
				{
					frame->result->tag = TXN_ENTRY_CHANGED_RUNNING;
					frame->ok = true;
					return;
				}
				txn_insert_Progress_waiting(&frame->progress);
				if (!txn_insert_waited(frame->io, frame->waits, frame->keyed->index, frame->step.u.wait.f0, true,
									   frame->row, frame->step.u.wait.has_f1, frame->step.u.wait.f1, frame->error))
					return;
				break;
			case TXN_ENTRY_STEP_RETRY:
				frame->step_live = false;
				break;
		}
	}
}

/* txn_entry_unique_pad frees what unique holds live: a turn's step, read, and stamps, its progress, and its entry. */
static void
txn_entry_unique_pad(void *frame_)
{
	txn_entry_unique_frame_t *frame = frame_;

	if (frame->step_live)
		txn_entry_Step_drop(&frame->step);
	txn_entry_unique_turn_drop(frame);
	txn_entry_encodings_drop(&frame->encodings);
	if (frame->progress_live)
		txn_insert_Progress_drop(&frame->progress);
	if (frame->formed_live)
		txn_entry_Formed_drop(&frame->formed);
	frame->step_live = frame->progress_live = frame->formed_live = false;
}

/*
 * txn_entry_unique pushes an immediately checked unique index's entry,
 * taking it, as change does, after resolving its key as an immediate key's
 * insert resolves it: it reads the key in the base before the key's stripe,
 * and under the prepare's hold its versions. An entry that exists at the key
 * answers its value; another transaction's running write of it is waited
 * for, naming the index and the inserted row's tuple id, and the key
 * resolved again, or for a partial check answered as running with no wait;
 * and a free key takes the push under the stripe, once its read is
 * rechecked there, the resolve starting over when it moved. A keyless
 * table's entry, a counted copy, is decided by its key's copies instead, the
 * base's stamps read beside its base read: a live copy under the inserted
 * row's tuple id is counted already, so nothing is written.
 */
bool
txn_entry_unique(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, TxnEntryEntry *entry,
				 bool has_recorded, txn_TupleVecU8Op_t *recorded, const TxnInsertUniqueness *keyed,
				 HostIoWaits *waits, HostRow row, bool partial, TxnEntryChanged *result, HostError *error)
{
	txn_entry_unique_frame_t frame = {.io = io, .store = store, .held = held, .record = host_shared_Held_record(held),
		.keyed = keyed, .waits = waits, .row = row, .partial = partial, .result = result,
	.error = error};

	frame.stripe = txn_change_stripe(&entry->storage, entry->key.ptr, entry->key.len, !keyed->has_equal);
	if (!txn_entry_formed(io, entry, has_recorded, recorded, &frame.formed, error))
		return false;
	frame.formed_live = true;
	std_landing(&frame, txn_entry_unique_body, txn_entry_unique_pad);
	txn_entry_unique_pad(&frame);
	return frame.ok;
}
