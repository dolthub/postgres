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
 * A storage's inserts: a row's tuple id, taken by heap's order from the block
 * the last insert took, a block whose free space map entry has room, or the
 * fresh page, and the row's change record, version, and tuple id table
 * entry, all made in one checkpoint window after one prepare, which reads and
 * reserves everything that can fail first.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/foldhash.h"
#include "doltgres/std/hashmap.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/txn/fold.h"
#include "doltgres/txn/free_space.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/tuples.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/vacuum.h"
#include "doltgres/txn/written.h"
#include "port/pg_bswap.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "insert.rs"

/* TXN_INSERT_XLTW_INSERT_INDEX is Postgres' XLTW_InsertIndex, which _bt_check_unique's wait names. */
#define TXN_INSERT_XLTW_INSERT_INDEX 5

/* TXN_INSERT_HOLD_BACK is how many start-overs before a resolve holds its stripe's other writers back. */
#define TXN_INSERT_HOLD_BACK 3

/* TXN_INSERT_OTHER is the size and alignment of an other key equal to a resolve's key, with the base's value at it. */
#define TXN_INSERT_OTHER sizeof(txn_insert_TupleVecU8OptionVecU8_t), _Alignof(txn_insert_TupleVecU8OptionVecU8_t)

/* txn_insert_TupleVecU8OptionVecU8_t is another key equal to a resolve's key, and the base's value at it. */
typedef struct txn_insert_TupleVecU8OptionVecU8_t
{
	StdVec		f0;
	bool		has_f1;
	StdVec		f1;
} txn_insert_TupleVecU8OptionVecU8_t;

/* txn_insert_TupleVecU8VecU8_t is a base row read by a resolve: its key and its value. */
typedef struct txn_insert_TupleVecU8VecU8_t
{
	StdVec		f0;
	StdVec		f1;
} txn_insert_TupleVecU8VecU8_t;

/* The pin whose scope reads a resolve's base row. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultOptionRow
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , bool *some, txn_insert_TupleVecU8VecU8_t *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS bool *some; txn_insert_TupleVecU8VecU8_t *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .some = some, .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->some, frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/*
 * txn_insert_Room_fits reports whether a page with free bytes, its next
 * offset the one given, has room for the row, as heap's raw_heap_insert
 * decides: an offset under its most tuples, and the row's heap bytes with
 * the fill factor's reserve.
 */
bool
txn_insert_Room_fits(const TxnInsertRoom *self, uint64 free, uint16 offset)
{
	return offset <= self->most && free >= STD_ADD(u64, self->bytes, self->reserve, 54, 40);
}

/*
 * txn_insert_Room_need returns the free bytes a page must have for an insert
 * of the row, as heap's RelationGetBufferForTuple asks: the row's heap bytes
 * with the fill factor's reserve, or once those pass a nearly empty page's
 * free bytes, the larger of the row's and those.
 */
uint64
txn_insert_Room_need(const TxnInsertRoom *self)
{
	uint64		lines = STD_MUL(u64, (uint64) (self->most / 8), TXN_VACUUM_LINE_POINTER, 62, 54);
	uint64		nearly_empty = STD_ADD(u64, STD_SUB(u64, TXN_FREE_SPACE_MAX_REQUEST, lines, 62, 28),
									   TXN_VACUUM_LINE_POINTER, 62, 28);
	uint64		wanted = STD_ADD(u64, self->bytes, self->reserve, 63, 15);

	if (wanted > nearly_empty)
		return Max(self->bytes, nearly_empty);
	return STD_ADD(u64, self->bytes, self->reserve, 65, 22);
}

/*
 * txn_insert_Inserted_PartialEq_eq reports whether two writes did the same
 * with the same tuple ids, and for two stops, whether their T's are equal by
 * T's eq.
 */
bool
txn_insert_Inserted_PartialEq_eq(const TxnInsertInserted *self, const void *self_stopped,
								 const TxnInsertInserted *other, const void *other_stopped,
								 bool (*eq) (const void *a, const void *b))
{
	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case TXN_INSERT_INSERTED_TOOK:
			return self->u.took.tuple == other->u.took.tuple &&
				self->u.took.has_carried == other->u.took.has_carried &&
				self->u.took.carried == other->u.took.carried;
		case TXN_INSERT_INSERTED_EXISTS:
			return self->u.exists.spent == other->u.exists.spent &&
				self->u.exists.has_live == other->u.exists.has_live &&
				(!self->u.exists.has_live || self->u.exists.live == other->u.exists.live);
		case TXN_INSERT_INSERTED_STOPPED:
			return eq(self_stopped, other_stopped);
	}
	pg_unreachable();
}

/* txn_insert_Inserted_Debug_fmt is Inserted's derived Debug, of a TxnInsertInsertedRef. */
bool
txn_insert_Inserted_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnInsertInsertedRef *self = value;
	const TxnInsertInserted *inserted = self->inserted;
	StdOptionRef live = {inserted->u.exists.has_live, {&inserted->u.exists.live, std_u64_Debug_fmt}};
	StdDebugTuple t;

	switch (inserted->tag)
	{
		case TXN_INSERT_INSERTED_TOOK:
			t = std_fmt_debug_tuple(f, "Took", 4);
			std_DebugTuple_field(&t, &inserted->u.took.tuple, std_u64_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case TXN_INSERT_INSERTED_EXISTS:
			t = std_fmt_debug_tuple(f, "Exists", 6);
			std_DebugTuple_field(&t, &inserted->u.exists.spent, std_u64_Debug_fmt);
			std_DebugTuple_field(&t, &live, std_option_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case TXN_INSERT_INSERTED_STOPPED:
			t = std_fmt_debug_tuple(f, "Stopped", 7);
			std_DebugTuple_field(&t, self->stopped.value, self->stopped.fmt);
			return std_DebugTuple_finish(&t);
	}
	pg_unreachable();
}

/* txn_insert_Progress_Default_default returns a resolve's progress before any start-over. */
TxnInsertProgress
txn_insert_Progress_Default_default(void)
{
	TxnInsertProgress progress = {0};

	return progress;
}

/* txn_insert_Progress_holding reports whether the resolve holds its stripe's other writers back. */
bool
txn_insert_Progress_holding(const TxnInsertProgress *self)
{
	return self->has_holding;
}

/* txn_insert_Progress_drop releases the resolve's hold-back flag, while it holds one. */
void
txn_insert_Progress_drop(TxnInsertProgress *self)
{
	if (self->has_holding)
		host_KeyHold_drop(&self->holding);
	self->has_holding = false;
}

/* txn_insert_Progress_waiting clears the resolve's start-overs and its hold-back flag before it waits. */
void
txn_insert_Progress_waiting(TxnInsertProgress *self)
{
	txn_insert_Progress_drop(self);
	*self = txn_insert_Progress_Default_default();
}

/* txn_insert_Unique_drop frees an immediate key's read: the base's value at it and the other keys equal to it. */
void
txn_insert_Unique_drop(TxnInsertUnique *self)
{
	if (self->has_based)
		std_vec_free_Checked(&self->based, 1, 1);
	for (size_t i = 0; i < self->others.len; i++)
	{
		txn_insert_TupleVecU8OptionVecU8_t *other = &STD_VEC_AT(&self->others, txn_insert_TupleVecU8OptionVecU8_t, i);

		std_vec_free_Checked(&other->f0, 1, 1);
		if (other->has_f1)
			std_vec_free_Checked(&other->f1, 1, 1);
	}
	std_vec_free_Checked(&self->others, TXN_INSERT_OTHER);
}

/* txn_insert_bytes_eq reports whether a key's bytes are a vector's, as [u8]'s PartialEq compares them. */
static bool
txn_insert_bytes_eq(StdBytes a, const uint8 *b, size_t b_len)
{
	return a.len == b_len && (b_len == 0 || memcmp(a.ptr, b, b_len) == 0);
}

/* txn_insert_based_closure_2_captures_t is based's pin scope's captures: the root, the key, and its uniqueness. */
typedef struct txn_insert_based_closure_2_captures_t
{
	TreesStoreTree *root;
	const uint8 *key;
	size_t		key_len;
	const TxnInsertUniqueness *unique;
} txn_insert_based_closure_2_captures_t;

/* txn_insert_based_closure_2_frame_t is the pin scope's state across its pad: its cursor, while live. */
typedef struct txn_insert_based_closure_2_frame_t
{
	txn_insert_based_closure_2_captures_t *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesCursorCursor cursor;
	bool		cursor_live;
	TreesCursorCursor last;
	bool		last_live;
	bool	   *some;
	txn_insert_TupleVecU8VecU8_t *result;
	ChunksErrorError *error;
	bool		ok;
} txn_insert_based_closure_2_frame_t;

/* txn_insert_based_closure_2_equal asks the key's class whether the cursor's key equals the asked one. */
static bool
txn_insert_based_closure_2_equal(txn_insert_based_closure_2_frame_t *frame, bool *result)
{
	const TxnInsertEqual *equal = &frame->captures->unique->equal;
	StdBytes	at;
	HostError	failed;

	*result = false;
	if (!trees_cursor_Cursor_is_valid(&frame->cursor))
		return true;
	at = trees_cursor_Cursor_key(&frame->cursor);
	if (equal->ops->equal(equal->self, frame->io, at.ptr, at.len, frame->captures->key, frame->captures->key_len,
						  result, &failed))
		return true;
	frame->error->tag = CHUNKS_ERROR_ERROR_HOST;
	frame->error->u.host = failed;
	return false;
}

/* txn_insert_based_closure_2_row copies the cursor's row: its key and its value. */
static void
txn_insert_based_closure_2_row(txn_insert_based_closure_2_frame_t *frame)
{
	StdBytes	key = trees_cursor_Cursor_key(&frame->cursor);
	StdBytes	value = trees_cursor_Cursor_value(&frame->cursor);

	std_slice_to_vec_Checked(&frame->result->f0, key.ptr, key.len, 1, 1);
	std_slice_to_vec_Checked(&frame->result->f1, value.ptr, value.len, 1, 1);
	*frame->some = true;
}

/*
 * txn_insert_based_closure_2_body seeks the key: its own row, or for a key
 * whose equal values can have other bytes, the row there or the one before
 * it that its class calls equal.
 */
static void
txn_insert_based_closure_2_body(void *frame_)
{
	txn_insert_based_closure_2_frame_t *frame = frame_;
	txn_insert_based_closure_2_captures_t *captures = frame->captures;
	bool		equal;

	*frame->some = false;
	if (!trees_cursor_Cursor_seek(frame->io, frame->pin, TREES_STORE_TREE_NAMED(captures->root), captures->key,
								  captures->key_len, captures->unique->order, &frame->cursor, frame->error))
		return;
	frame->cursor_live = true;
	if (!captures->unique->has_equal)
	{
		if (trees_cursor_Cursor_is_valid(&frame->cursor) &&
			txn_insert_bytes_eq(trees_cursor_Cursor_key(&frame->cursor), captures->key, captures->key_len))
			txn_insert_based_closure_2_row(frame);
		frame->ok = true;
		return;
	}
	if (!txn_insert_based_closure_2_equal(frame, &equal))
		return;
	if (equal)
	{
		txn_insert_based_closure_2_row(frame);
		frame->ok = true;
		return;
	}
	if (trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		if (!trees_cursor_Cursor_prev(&frame->cursor, frame->io, frame->pin, frame->error))
			return;
	}
	else
	{
		if (!trees_cursor_Cursor_last(frame->io, frame->pin, TREES_STORE_TREE_NAMED(captures->root), &frame->last,
									  frame->error))
			return;
		frame->last_live = true;
		frame->cursor_live = false;
		trees_cursor_Cursor_drop(&frame->cursor);
		frame->cursor = frame->last;
		frame->cursor_live = true;
		frame->last_live = false;
	}
	if (!txn_insert_based_closure_2_equal(frame, &equal))
		return;
	if (equal)
		txn_insert_based_closure_2_row(frame);
	frame->ok = true;
}

/* txn_insert_based_closure_2_pad frees the pin scope's cursor, while live. */
static void
txn_insert_based_closure_2_pad(void *frame_)
{
	txn_insert_based_closure_2_frame_t *frame = frame_;

	if (frame->last_live)
		trees_cursor_Cursor_drop(&frame->last);
	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
	frame->last_live = frame->cursor_live = false;
}

/* txn_insert_based_closure_2 is based's pin scope: the base's row at the key, or one its class calls equal. */
static bool
txn_insert_based_closure_2(void *captures, HostIoIo *io, TreesPinPin *pin, bool *some,
						   txn_insert_TupleVecU8VecU8_t *result, ChunksErrorError *error)
{
	txn_insert_based_closure_2_frame_t frame = {.captures = captures, .io = io, .pin = pin, .some = some,
	.result = result, .error = error};

	std_landing(&frame, txn_insert_based_closure_2_body, txn_insert_based_closure_2_pad);
	txn_insert_based_closure_2_pad(&frame);
	return frame.ok;
}

/*
 * txn_insert_based_frame_t is based's state across its pad: the base record,
 * the base row it read, the hold of its walk of the delta and the keys it
 * found, and its result, each while live.
 */
typedef struct txn_insert_based_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const uint8 *key;
	size_t		key_len;
	const TxnInsertUniqueness *unique;
	uint32		stripe;
	TxnBaseBase base;
	bool		base_live;
	bool		has_row;
	txn_insert_TupleVecU8VecU8_t row;
	HostDeltaHold hold;
	bool		hold_live;
	bool		has_after;
	StdVec		after;
	txn_TupleVecU8VecVersion_t next;
	bool		next_live;
	StdVec		keys;
	bool		keys_live;
	TxnInsertUnique *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} txn_insert_based_frame_t;

/* txn_insert_keys_drop frees a vector of keys. */
static void
txn_insert_keys_drop(StdVec *keys)
{
	for (size_t i = 0; i < keys->len; i++)
		std_vec_free_Checked(&STD_VEC_AT(keys, StdVec, i), 1, 1);
	std_vec_free_Checked(keys, sizeof(StdVec), _Alignof(StdVec));
}

/* txn_insert_has_other reports whether a resolve's other keys hold a key. */
static bool
txn_insert_has_other(const StdVec *others, const StdVec *key)
{
	for (size_t i = 0; i < others->len; i++)
	{
		const txn_insert_TupleVecU8OptionVecU8_t *other = &STD_VEC_AT(others, txn_insert_TupleVecU8OptionVecU8_t,
																	  i);

		if (trees_store_VecU8_PartialEq_eq(&other->f0, key))
			return true;
	}
	return false;
}

/* txn_insert_based_body reads the key's base row before the stripe, then the delta's other keys equal to it. */
static void
txn_insert_based_body(void *frame_)
{
	txn_insert_based_frame_t *frame = frame_;
	TxnInsertUnique *result = frame->result;
	ChunksErrorError failed;
	bool		some;

	result->has_based = false;
	std_vec_new(&result->others, TXN_INSERT_OTHER);
	result->has_count = frame->unique->has_equal;
	if (result->has_count)
		result->count = host_key_count(frame->stripe);
	result->generation = txn_state_generation(host_shared_Held_record(frame->held));
	frame->result_live = true;
	if (!txn_state_base_record(frame->io, frame->held, &some, &frame->base, frame->error))
		return;
	frame->base_live = some;
	if (some && frame->base.has_root)
	{
		txn_insert_based_closure_2_captures_t captures = {.root = &frame->base.root, .key = frame->key,
		.key_len = frame->key_len, .unique = frame->unique};

		if (!trees_pin_pin_ResultOptionRow(frame->io, frame->store, txn_insert_based_closure_2, &captures, NULL,
										   &frame->has_row, &frame->row, &failed))
		{
			*frame->error = txn_base_storage(&failed);
			return;
		}
	}
	if (frame->has_row)
	{
		StdBytes	row = {frame->row.f0.ptr, frame->row.f0.len};

		if (txn_insert_bytes_eq(row, frame->key, frame->key_len))
		{
			frame->has_row = false;
			std_vec_free_Checked(&frame->row.f0, 1, 1);
			result->has_based = true;
			result->based = frame->row.f1;
		}
		else
		{
			txn_insert_TupleVecU8OptionVecU8_t other = {.f0 = frame->row.f0, .has_f1 = true, .f1 = frame->row.f1};

			std_vec_push_Checked(&result->others, &other, TXN_INSERT_OTHER);
			frame->has_row = false;
		}
	}
	if (!frame->unique->has_equal)
	{
		frame->result_live = false;
		frame->ok = true;
		return;
	}
	std_vec_new(&frame->keys, sizeof(StdVec), _Alignof(StdVec));
	frame->keys_live = true;
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	for (;;)
	{
		bool		found;

		if (!txn_memtable_table_next(frame->io, &frame->hold, host_shared_Held_record(frame->held),
									 frame->has_after, frame->after.ptr, frame->after.len,
									 TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &found, &frame->next, frame->error))
			return;
		if (!found)
			break;
		frame->next_live = true;
		if (frame->has_after)
			std_vec_free_Checked(&frame->after, 1, 1);
		frame->has_after = false;
		std_slice_to_vec_Checked(&frame->after, frame->next.f0.ptr, frame->next.f0.len, 1, 1);
		frame->has_after = true;
		std_vec_push_Checked(&frame->keys, &frame->next.f0, sizeof(StdVec), _Alignof(StdVec));
		frame->next_live = false;
		txn_memtable_version_versions_drop(&frame->next.f1, 0);
	}
	frame->hold_live = false;
	host_delta_Hold_drop(&frame->hold);
	for (size_t i = 0; i < frame->keys.len; i++)
	{
		StdVec	   *other = &STD_VEC_AT(&frame->keys, StdVec, i);
		StdBytes	bytes = {other->ptr, other->len};
		bool		equal;

		if (txn_insert_bytes_eq(bytes, frame->key, frame->key_len) || txn_insert_has_other(&result->others, other))
			continue;
		if (!frame->unique->equal.ops->equal(frame->unique->equal.self, frame->io, other->ptr, other->len, frame->key,
											 frame->key_len, &equal, frame->error))
			return;
		if (equal)
		{
			txn_insert_TupleVecU8OptionVecU8_t taken = {.f0 = *other};

			std_vec_push_Checked(&result->others, &taken, TXN_INSERT_OTHER);
			std_vec_new(other, 1, 1);
		}
	}
	frame->result_live = false;
	frame->ok = true;
}

/* txn_insert_based_pad frees what based holds live. */
static void
txn_insert_based_pad(void *frame_)
{
	txn_insert_based_frame_t *frame = frame_;

	if (frame->next_live)
	{
		std_vec_free_Checked(&frame->next.f0, 1, 1);
		txn_memtable_version_versions_drop(&frame->next.f1, 0);
	}
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->has_after)
		std_vec_free_Checked(&frame->after, 1, 1);
	if (frame->keys_live)
		txn_insert_keys_drop(&frame->keys);
	if (frame->has_row)
	{
		std_vec_free_Checked(&frame->row.f0, 1, 1);
		std_vec_free_Checked(&frame->row.f1, 1, 1);
	}
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	if (frame->result_live)
		txn_insert_Unique_drop(frame->result);
	frame->next_live = frame->hold_live = frame->has_after = frame->keys_live = false;
	frame->has_row = frame->base_live = frame->result_live = false;
}

/*
 * txn_insert_based reads an immediate key before its stripe, through the
 * key's order: the base's row at the key's own bytes, the base generation it
 * read it at, and for a key whose equal values can have other bytes, the
 * base's row its class calls equal and the delta's other keys equal to it,
 * and the stripe's push count read first.
 */
bool
txn_insert_based(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const uint8 *key,
				 size_t key_len, const TxnInsertUniqueness *unique, uint32 stripe, TxnInsertUnique *result,
				 HostError *error)
{
	txn_insert_based_frame_t frame = {.io = io, .store = store, .held = held, .key = key, .key_len = key_len,
	.unique = unique, .stripe = stripe, .result = result, .error = error};

	std_landing(&frame, txn_insert_based_body, txn_insert_based_pad);
	txn_insert_based_pad(&frame);
	return frame.ok;
}

/* txn_insert_resolving_frame_t is resolving's state across its pad: the keys' versions and their answers. */
typedef struct txn_insert_resolving_frame_t
{
	HostIoIo   *io;
	const HostDeltaHold *hold;
	HostSharedRecord record;
	const uint8 *key;
	size_t		key_len;
	const TxnInsertUnique *read;
	StdVec		versions;
	bool		versions_live;
	StdVec		others;
	bool		others_live;
	TxnVisibleResolved found;
	bool		found_live;
	TxnVisibleResolved *answer;
	bool		answer_live;
	bool	   *has_newest;
	TxnMemtableVersionHandle *newest;
	HostError  *error;
	bool		ok;
} txn_insert_resolving_frame_t;

/* txn_insert_based_bytes returns a base value as Option<&[u8]>'s slice, NULL for none. */
static StdBytes
txn_insert_based_bytes(bool has, const StdVec *value)
{
	StdBytes	bytes = {has ? value->ptr : NULL, value->len};

	return bytes;
}

/* txn_insert_resolving_body resolves the key's own versions and then each other key equal to it. */
static void
txn_insert_resolving_body(void *frame_)
{
	txn_insert_resolving_frame_t *frame = frame_;
	const TxnInsertUnique *read = frame->read;

	if (!txn_memtable_table_versions(frame->io, frame->hold, frame->record, frame->key, frame->key_len,
									 &frame->versions, frame->error))
		return;
	frame->versions_live = true;
	if (!txn_visible_resolved(frame->io, frame->versions.ptr, frame->versions.len,
							  txn_insert_based_bytes(read->has_based, &read->based), frame->answer, frame->error))
		return;
	frame->answer_live = true;
	for (size_t i = 0; i < read->others.len; i++)
	{
		const txn_insert_TupleVecU8OptionVecU8_t *other = &STD_VEC_AT(&read->others,
																	  txn_insert_TupleVecU8OptionVecU8_t, i);

		if (!txn_memtable_table_versions(frame->io, frame->hold, frame->record, other->f0.ptr, other->f0.len,
										 &frame->others, frame->error))
			return;
		frame->others_live = true;
		if (!txn_visible_resolved(frame->io, frame->others.ptr, frame->others.len,
								  txn_insert_based_bytes(other->has_f1, &other->f1), &frame->found, frame->error))
			return;
		frame->found_live = true;
		frame->others_live = false;
		txn_memtable_version_versions_drop(&frame->others, 0);
		if (frame->answer->tag != TXN_VISIBLE_RESOLVED_EXISTS &&
			(frame->found.tag == TXN_VISIBLE_RESOLVED_EXISTS || frame->answer->tag == TXN_VISIBLE_RESOLVED_FREE))
		{
			txn_visible_Resolved_drop(frame->answer);
			*frame->answer = frame->found;
		}
		else
			txn_visible_Resolved_drop(&frame->found);
		frame->found_live = false;
	}
	*frame->has_newest = txn_memtable_version_held(frame->versions.ptr, frame->versions.len, frame->newest);
	frame->answer_live = false;
	frame->ok = true;
}

/* txn_insert_resolving_pad frees what resolving holds live. */
static void
txn_insert_resolving_pad(void *frame_)
{
	txn_insert_resolving_frame_t *frame = frame_;

	if (frame->found_live)
		txn_visible_Resolved_drop(&frame->found);
	if (frame->others_live)
		txn_memtable_version_versions_drop(&frame->others, 0);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	if (frame->answer_live)
		txn_visible_Resolved_drop(frame->answer);
	frame->found_live = frame->others_live = frame->versions_live = frame->answer_live = false;
}

/*
 * txn_insert_resolving resolves an immediate key under a prepare's hold from
 * its read: the key's own versions and each other key equal to it, a live
 * row at any of them deciding it, then a running write, then a free key, and
 * returns the key's newest version, which its stripe's recheck compares.
 */
bool
txn_insert_resolving(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord record, const uint8 *key,
					 size_t key_len, const TxnInsertUnique *read, TxnVisibleResolved *result_0, bool *has_result_1,
					 TxnMemtableVersionHandle *result_1, HostError *error)
{
	txn_insert_resolving_frame_t frame = {.io = io, .hold = hold, .record = record, .key = key, .key_len = key_len,
	.read = read, .answer = result_0, .has_newest = has_result_1, .newest = result_1, .error = error};

	std_landing(&frame, txn_insert_resolving_body, txn_insert_resolving_pad);
	txn_insert_resolving_pad(&frame);
	return frame.ok;
}

/*
 * txn_insert_rechecked rechecks an immediate key's read under its stripe: the
 * base moved since it, or the stripe's push count did, which after enough
 * start-overs sets its hold-back flag, or the read stands.
 */
TxnInsertRechecked
txn_insert_rechecked(HostIoKeyIo *io, HostSharedRecord record, const TxnInsertUnique *read, uint32 stripe,
					 TxnInsertProgress *progress)
{
	if (txn_state_generation(record) != read->generation)
		return TXN_INSERT_RECHECKED_MOVED;
	if (!read->has_count || host_key_count(stripe) == read->count)
		return TXN_INSERT_RECHECKED_GO;
	progress->starts = STD_ADD(u32, progress->starts, 1, 610, 13);
	if (progress->starts >= TXN_INSERT_HOLD_BACK && !progress->has_holding)
		progress->has_holding = host_key_hold(io, stripe, &progress->holding);
	return TXN_INSERT_RECHECKED_RETRY;
}

/*
 * txn_insert_waited waits for a running writer an immediate key's resolve
 * found: a speculative insert's token, or the writer's transaction, naming
 * the index and the inserted row's tuple id as _bt_check_unique does.
 */
bool
txn_insert_waited(HostIoIo *io, HostIoWaits *waits, uint32 index, HostXid xid, bool has_tid, HostRow tid,
				  bool has_token, uint32 token, HostError *error)
{
	HostOperation on = {.relation = index, .tid = tid.f0, .oper = TXN_INSERT_XLTW_INSERT_INDEX};
	bool		waited;

	if (has_token)
		return host_speculative_wait(waits, HOST_IO_IO_IO_DOORS(io), xid, token, error);
	return host_xact_wait(waits, HOST_IO_IO_IO_DOORS(io), xid, has_tid, on, true, &waited, error);
}

/* txn_insert_read_base reads the state's base generation, then its base record. */
static bool
txn_insert_read_base(HostIoIo *io, const HostSharedHeld *held, TxnInsertBase *result, HostError *error)
{
	TxnBaseBase base;
	bool		some;

	result->generation = txn_state_generation(host_shared_Held_record(held));
	if (!txn_state_base_record(io, held, &some, &base, error))
		return false;
	result->has_tree = some && base.has_free_space;
	if (result->has_tree)
		result->tree = base.free_space;
	result->fresh_end = some ? base.fresh_end : 1;
	if (some)
		txn_base_Base_drop(&base);
	return true;
}

/* txn_insert_fresh_end returns the tuple id a block's entry is read at: the counter's on its block, else the base's. */
static uint64
txn_insert_fresh_end(uint32 block, const TxnInsertBase *base, uint64 counter)
{
	return (uint64) block == counter >> 16 ? counter : base->fresh_end;
}

/* txn_insert_decoded reads a block's entry, whose all-clear bitmap is valid for the fresh page and the counter's. */
static bool
txn_insert_decoded(const uint8 *value, size_t value_len, uint32 block, const TxnInsertBase *base, uint64 counter,
				   const TxnInsertRoom *room, TxnFreeSpaceFree *result, HostError *error)
{
	return txn_free_space_Free_decode(value, value_len, block, txn_insert_fresh_end(block, base, counter), room->most,
									  result, error);
}

/* txn_insert_parsed reads an entry of a block as decoded does inside a take's step, false for one that doesn't read. */
static bool
txn_insert_parsed(const uint8 *value, size_t value_len, uint32 block, const TxnInsertBase *base, uint64 counter,
				  const TxnInsertRoom *room, TxnFreeSpaceFree *result)
{
	return txn_free_space_Free_parsed(value, value_len, block, txn_insert_fresh_end(block, base, counter), room->most,
									  result);
}

/* TxnInsertSeen is Seen: a block's entry read outside a window, the bytes a take compares, and the base's. */
typedef struct TxnInsertSeen
{
	bool		has_current;
	StdVec		current;
	bool		has_below;
	StdVec		below;
} TxnInsertSeen;

/* txn_insert_Seen_drop frees a block's entries read outside a window. */
static void
txn_insert_Seen_drop(TxnInsertSeen *self)
{
	if (self->has_current)
		std_vec_free_Checked(&self->current, 1, 1);
	if (self->has_below)
		std_vec_free_Checked(&self->below, 1, 1);
	self->has_current = self->has_below = false;
}

/* txn_insert_Seen_read is a take's read of a block whose entry it saw outside the window, with the base generation read before it. */
static TxnMemtableMapRead
txn_insert_Seen_read(const TxnInsertSeen *self, const TxnInsertBase *base)
{
	return (TxnMemtableMapRead)
	{
		.has_entry = self->has_current, .entry = self->has_current ? self->current.ptr : NULL,
			.entry_len = self->has_current ? self->current.len : 0, .generation = base->generation
	};
}

/* txn_insert_seen_frame_t is seen's state across its pad: what it read, while live. */
typedef struct txn_insert_seen_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	HostSharedRecord record;
	const TxnInsertBase *base;
	const TxnInsertRoom *room;
	uint32		block;
	TxnInsertSeen *result;
	HostError  *error;
	bool		ok;
} txn_insert_seen_frame_t;

/* txn_insert_seen_body reads the block's newest entry, the memtable's, else its spills', else the base's, and decodes it. */
static void
txn_insert_seen_body(void *frame_)
{
	txn_insert_seen_frame_t *frame = frame_;
	TxnInsertSeen *result = frame->result;
	TxnMemtableMapEntry newest;
	TxnFreeSpaceFree free;
	ChunksErrorError failed;
	uint8		key[4];
	bool		some;
	bool		spilled;

	if (!txn_memtable_map_in_memtable(frame->io, frame->record, TXN_MEMTABLE_MAP_MAP_FREE, frame->block, &some,
									  &newest, frame->error))
		return;
	if (some)
	{
		result->has_current = newest.has_value;
		result->current = newest.value;
	}
	else if (!txn_memtable_map_spilled_newest(frame->io, frame->record, frame->block, &spilled, &newest, frame->error))
		return;
	else if (spilled)
	{
		result->has_below = newest.has_value;
		result->below = newest.value;
		if (result->has_below)
		{
			std_slice_to_vec_Checked(&result->current, result->below.ptr, result->below.len, 1, 1);
			result->has_current = true;
		}
	}
	else if (frame->base->has_tree)
	{
		TreesStoreTree tree = frame->base->tree;

		txn_free_space_key(frame->block, key);
		if (!trees_get(frame->io, frame->store, TREES_STORE_TREE_NAMED(&tree), key,
					   sizeof(key), &result->has_below, &result->below, &failed))
		{
			*frame->error = txn_base_storage(&failed);
			return;
		}
		if (result->has_below)
		{
			std_slice_to_vec_Checked(&result->current, result->below.ptr, result->below.len, 1, 1);
			result->has_current = true;
		}
	}
	if (result->has_current &&
		!txn_insert_decoded(result->current.ptr, result->current.len, frame->block, frame->base,
							txn_state_counter(frame->record), frame->room, &free, frame->error))
		return;
	frame->ok = true;
}

/* txn_insert_seen_pad frees what seen read, unless it answers it. */
static void
txn_insert_seen_pad(void *frame_)
{
	txn_insert_seen_frame_t *frame = frame_;

	if (!frame->ok)
		txn_insert_Seen_drop(frame->result);
}

/*
 * txn_insert_seen reads a block's entry outside any window, the delta's
 * newest else the base's, and decodes it there, so a corrupt entry raises
 * XX001 outside the critical section; it returns the bytes a take compares
 * under its lock, and the entry below the memtable the take falls back to:
 * the spills' newest, else the base's.
 */
static bool
txn_insert_seen(HostIoIo *io, TreesStoreNodeStore store, HostSharedRecord record, const TxnInsertBase *base,
				const TxnInsertRoom *room, uint32 block, TxnInsertSeen *result, HostError *error)
{
	txn_insert_seen_frame_t frame = {.io = io, .store = store, .record = record, .base = base, .room = room,
	.block = block, .result = result, .error = error};

	result->has_current = result->has_below = false;
	std_landing(&frame, txn_insert_seen_body, txn_insert_seen_pad);
	txn_insert_seen_pad(&frame);
	return frame.ok;
}

/* txn_insert_candidate_frame_t is candidate's state across its pad: the entries its search read, while live. */
typedef struct txn_insert_candidate_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	HostSharedRecord record;
	const TxnInsertBase *base;
	const TxnInsertRoom *room;
	uint32		from;
	uint32		fresh;
	StdVec		based;
	bool		based_live;
	txn_TupleU32Entry_t delta;
	bool		delta_live;
	TxnMemtableMapEntry newest;
	bool		newest_live;
	bool	   *some;
	uint32	   *result;
	HostError  *error;
	bool		ok;
} txn_insert_candidate_frame_t;

/* txn_insert_candidate_newest reads a block's newest delta entry, and whether the delta has one. */
static bool
txn_insert_candidate_newest(txn_insert_candidate_frame_t *frame, uint32 block, bool *some)
{
	if (!txn_memtable_map_newest(frame->io, frame->record, TXN_MEMTABLE_MAP_MAP_FREE, block, some, &frame->newest,
								 frame->error))
		return false;
	frame->newest_live = *some;
	return true;
}

/* txn_insert_candidate_newest_drop frees the newest delta entry candidate read, while live. */
static void
txn_insert_candidate_newest_drop(txn_insert_candidate_frame_t *frame)
{
	if (frame->newest_live && frame->newest.has_value)
		std_vec_free_Checked(&frame->newest.value, 1, 1);
	frame->newest_live = false;
}

/* txn_insert_candidate_turn_drop frees the entries one step of candidate's search read. */
static void
txn_insert_candidate_turn_drop(txn_insert_candidate_frame_t *frame)
{
	if (frame->based_live)
		std_vec_free_Checked(&frame->based, 1, 1);
	if (frame->delta_live && frame->delta.f1.has_value)
		std_vec_free_Checked(&frame->delta.f1.value, 1, 1);
	frame->based_live = frame->delta_live = false;
	txn_insert_candidate_newest_drop(frame);
}

/* txn_insert_candidate_body searches the base's tree and the delta's map for a block with room. */
static void
txn_insert_candidate_body(void *frame_)
{
	txn_insert_candidate_frame_t *frame = frame_;
	uint32		from = frame->from;
	uint8		category;
	uint64		need;
	uint32		hint;
	uint32		scan;
	bool		hinted;

	*frame->some = false;
	if (!txn_free_space_needed(STD_SUB(u64, txn_insert_Room_need(frame->room), TXN_VACUUM_LINE_POINTER, 682, 39),
							   &category, frame->error))
		return;
	need = STD_ADD(u64, txn_free_space_threshold(category), TXN_VACUUM_LINE_POINTER, 683, 16);
	if (!txn_state_hint(frame->record, category, &hint, frame->error))
		return;
	scan = Max(from, hint);
	hinted = hint >= from;
	for (;;)
	{
		bool		has_based = false;
		uint32		based_block = 0;
		bool		has_delta;
		uint32		block;
		const StdVec *value;
		bool		has_value;
		bool		reaches = false;
		bool		fits = false;
		bool		some;
		TreesStoreTree tree = frame->base->tree;

		txn_insert_candidate_turn_drop(frame);
		if (frame->base->has_tree &&
			!txn_free_space_seek(frame->io, frame->store, TREES_STORE_TREE_NAMED(&tree), from, need, &has_based,
								 &based_block, &frame->based, frame->error))
			return;
		frame->based_live = has_based;
		if (!txn_memtable_map_seek(frame->io, frame->record, TXN_MEMTABLE_MAP_MAP_FREE, scan, &has_delta,
								   &frame->delta, frame->error))
			return;
		frame->delta_live = has_delta;
		if (!has_delta && !has_based)
		{
			frame->ok = true;
			return;
		}
		if (!has_delta || (has_based && frame->delta.f0 > based_block))
		{
			bool		masked = false;

			if (based_block < scan)
			{
				if (!txn_insert_candidate_newest(frame, based_block, &some))
					return;
				masked = some;
			}
			if (masked || based_block == frame->fresh)
			{
				from = STD_ADD(u32, based_block, 1, 699, 28);
				scan = Max(scan, from);
				continue;
			}
			*frame->some = true;
			*frame->result = based_block;
			frame->ok = true;
			return;
		}
		block = frame->delta.f0;
		has_value = frame->delta.f1.has_value;
		value = &frame->delta.f1.value;
		if (has_value)
		{
			uint64		bytes;

			if (!txn_free_space_value_bytes(value->ptr, value->len, &bytes, frame->error))
				return;
			reaches = bytes >= need;
		}
		if (has_value && reaches)
		{
			TxnFreeSpaceFree free;
			uint16		lowest;

			if (!txn_insert_decoded(value->ptr, value->len, block, frame->base, txn_state_counter(frame->record),
									frame->room, &free, frame->error))
				return;
			fits = txn_free_space_Free_lowest(&free, &lowest);
		}
		if (fits && block != frame->fresh)
		{
			*frame->some = true;
			*frame->result = block;
			frame->ok = true;
			return;
		}
		if (hinted && !reaches && block != frame->fresh)
		{
			if (!txn_state_advanced(frame->record, category, hint, STD_ADD(u32, block, 1, 717, 99), &hinted,
									frame->error))
				return;
		}
		else
			hinted = false;
		if (hinted)
		{
			hint = STD_ADD(u32, block, 1, 719, 20);
			if (!txn_insert_candidate_newest(frame, block, &some))
				return;
			if (some && frame->newest.has_value)
			{
				uint64		bytes;

				if (!txn_free_space_value_bytes(frame->newest.value.ptr, frame->newest.value.len, &bytes,
												frame->error))
					return;
				if (bytes >= need)
				{
					if (!txn_state_lowered(frame->record, block, category, category, frame->error))
						return;
					hinted = false;
				}
			}
		}
		from = Max(from, STD_ADD(u32, block, 1, 727, 34));
		scan = STD_ADD(u32, block, 1, 727, 45);
	}
}

/* txn_insert_candidate_pad frees the entries candidate's search read. */
static void
txn_insert_candidate_pad(void *frame_)
{
	txn_insert_candidate_turn_drop(frame_);
}

/*
 * txn_insert_candidate returns the lowest block at or past from, other than
 * the fresh page, whose entry has room for the row by heap's free space
 * categories, the delta's entry for a block masking the base's whatever it
 * holds. The base's tree is searched by its summaries, and the delta's map
 * from the category's hint: a search that began at the hint moves it past
 * each block it passes whose entry is of a lower category, then reads the
 * block again and lowers the hint back to it when a raise came between.
 */
static bool
txn_insert_candidate(HostIoIo *io, TreesStoreNodeStore store, HostSharedRecord record, const TxnInsertBase *base,
					 const TxnInsertRoom *room, uint32 from, uint32 fresh, bool *some, uint32 *result,
					 HostError *error)
{
	txn_insert_candidate_frame_t frame = {.io = io, .store = store, .record = record, .base = base, .room = room,
	.from = from, .fresh = fresh, .some = some, .result = result, .error = error};

	std_landing(&frame, txn_insert_candidate_body, txn_insert_candidate_pad);
	txn_insert_candidate_pad(&frame);
	return frame.ok;
}

/* TXN_INSERT_KEPT is the size and alignment of a kept value's base entry and mark. */
#define TXN_INSERT_KEPT sizeof(TxnInsertKept), _Alignof(TxnInsertKept)

/*
 * TxnInsertKept is Kept: a kept value's map entry as read outside the
 * window, its value below the memtable, which a count add falls back to, and
 * the TOAST storage's values mark it was read at.
 */
typedef struct TxnInsertKept
{
	bool		has_f0;
	StdVec		f0;
	std_TupleU64U64_t f1;
} TxnInsertKept;

/* txn_insert_kept_drop frees kept values' base entries. */
static void
txn_insert_kept_drop(StdVec *kept)
{
	for (size_t i = 0; i < kept->len; i++)
	{
		TxnInsertKept *at = &STD_VEC_AT(kept, TxnInsertKept, i);

		if (at->has_f0)
			std_vec_free_Checked(&at->f0, 1, 1);
	}
	std_vec_free_Checked(kept, TXN_INSERT_KEPT);
}

/* txn_insert_kept_bases_frame_t is kept_bases' state across its pad: the entries it read, while live. */
typedef struct txn_insert_kept_bases_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *toast;
	const StdVec *values;
	const StdVec *places;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} txn_insert_kept_bases_frame_t;

/* txn_insert_kept_bases_body reads each kept value's base entry, pausing after each. */
static void
txn_insert_kept_bases_body(void *frame_)
{
	txn_insert_kept_bases_frame_t *frame = frame_;
	StdString	message;

	std_vec_new(frame->result, TXN_INSERT_KEPT);
	frame->result_live = true;
	for (size_t i = 0; i < frame->places->len; i++)
	{
		size_t		place = STD_VEC_AT(frame->places, size_t, i);
		TxnInsertKept kept;

		if (place >= frame->values->len)
		{
			std_string_from_Checked(&message, "a kept place is past the row's", 30);
			*frame->error = host_Error_fork((const uint8 *) "XX000", &message);
			return;
		}
		if (!txn_values_based(frame->io, frame->store, frame->toast, STD_VEC_AT(frame->values, uint32, place),
							  &kept.has_f0, &kept.f0, &kept.f1, frame->error))
			return;
		std_vec_push_Checked(frame->result, &kept, TXN_INSERT_KEPT);
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-kept-read", frame->error))
			return;
	}
	frame->result_live = false;
	frame->ok = true;
}

/* txn_insert_kept_bases_pad frees the entries kept_bases read, unless it answers them. */
static void
txn_insert_kept_bases_pad(void *frame_)
{
	txn_insert_kept_bases_frame_t *frame = frame_;

	if (frame->result_live)
		txn_insert_kept_drop(frame->result);
	frame->result_live = false;
}

/*
 * txn_insert_kept_bases reads the base entries of the values an update kept
 * from the row it replaces, with the TOAST storage's values mark each was
 * read at, none for a row with no TOAST relation or no put.
 */
static bool
txn_insert_kept_bases(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *toast, const StdVec *values,
					  const StdVec *places, StdVec *result, HostError *error)
{
	txn_insert_kept_bases_frame_t frame = {.io = io, .store = store, .toast = toast, .values = values,
	.places = places, .result = result, .error = error};

	if (toast == NULL || values == NULL)
	{
		std_vec_new(result, TXN_INSERT_KEPT);
		return true;
	}
	std_landing(&frame, txn_insert_kept_bases_body, txn_insert_kept_bases_pad);
	txn_insert_kept_bases_pad(&frame);
	return frame.ok;
}

/* TxnInsertTargetTag is Target's variants. */
typedef enum TxnInsertTargetTag
{
	/* A block other than the fresh page. */
	TXN_INSERT_TARGET_BLOCK,
	/* The fresh page at a counter's load. */
	TXN_INSERT_TARGET_FRESH,
	/* The tuple id an earlier turn took before it waited. */
	TXN_INSERT_TARGET_TAKEN,
} TxnInsertTargetTag;

/* TxnInsertTarget is Target: the place a turn of the insert tries. */
typedef struct TxnInsertTarget
{
	TxnInsertTargetTag tag;
	union
	{
		uint32		block;
		uint64		fresh;
		uint64		taken;
	}			u;
} TxnInsertTarget;

/* TxnInsertGotTag is Got's variants. */
typedef enum TxnInsertGotTag
{
	/* A freed or an entry's tuple id. */
	TXN_INSERT_GOT_TID,
	/* The counter's tuple id on the fresh page. */
	TXN_INSERT_GOT_COUNTED,
	/* The fresh page left for a new one. */
	TXN_INSERT_GOT_LEFT,
} TxnInsertGotTag;

/* TxnInsertGot is Got: what a take answered under its block's lock, a tuple id, or a fresh page left for a new one. */
typedef struct TxnInsertGot
{
	TxnInsertGotTag tag;
	uint64		tid;
} TxnInsertGot;

/* txn_insert_Got_tuple returns the tuple id a take holds in its version, none for a leave, which takes none. */
static HostRow
txn_insert_Got_tuple(const TxnInsertGot *self)
{
	HostRow		tuple = {self->tag == TXN_INSERT_GOT_LEFT ? 0 : self->tid};

	return tuple;
}

/* The map's takes that keep nothing, and the map-only prepare whose closure returns nothing. */
#define TXN_MEMTABLE_MAP_RS Unit
#define TXN_MEMTABLE_MAP_RETURNS bool
#define TXN_MEMTABLE_MAP_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_MAP_TS Unit
#define TXN_MEMTABLE_MAP_KEPT_OUT
#define TXN_MEMTABLE_MAP_KEPT_IN
#define TXN_MEMTABLE_MAP_DECLARE
#include "doltgres/txn/memtable/map_template.h"

/* The map-only prepare whose closure returns what its take did. */
#define TXN_MEMTABLE_MAP_RS Taken
#define TXN_MEMTABLE_MAP_RETURNS bool
#define TXN_MEMTABLE_MAP_RESULT_PARAMS , TxnMemtableMapTaken *result, HostError *error
#define TXN_MEMTABLE_MAP_DECLARE
#include "doltgres/txn/memtable/map_template.h"

/* TxnInsertTurnTag is Turn's variants. */
typedef enum TxnInsertTurnTag
{
	TXN_INSERT_TURN_TOOK,
	TXN_INSERT_TURN_REFUSED,
	TXN_INSERT_TURN_RETRY,
	TXN_INSERT_TURN_MOVED,
	TXN_INSERT_TURN_EXISTS,
	TXN_INSERT_TURN_WAIT,
	TXN_INSERT_TURN_STOPPED,
} TxnInsertTurnTag;

/* TxnInsertTurn is Turn<T>: how a turn ended, a Stopped's T in the caller's bytes. */
typedef struct TxnInsertTurn
{
	TxnInsertTurnTag tag;
	union
	{
		uint64		took;
		struct
		{
			uint64		f0;
			bool		has_f1;
			uint64		f1;
		}			exists;
		struct
		{
			HostXid		f0;
			bool		has_f1;
			HostRow		f1;
			bool		has_f2;
			uint32		f2;
		}			wait;
	}			u;
} TxnInsertTurn;

/* The prepare whose closure runs a turn. */
#define TXN_MEMTABLE_TABLE_RS Turn
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , TxnInsertTurn *result, HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#define TXN_MEMTABLE_TABLE_DEFINE
#define TXN_MEMTABLE_TABLE_RESULT_FIELDS TxnInsertTurn *result;
#define TXN_MEMTABLE_TABLE_RESULT_INIT , result
#define TXN_MEMTABLE_TABLE_RESULT_ARGS(c) , (c)->result
#define TXN_MEMTABLE_TABLE_RESULT_ERROR error
#include "doltgres/txn/memtable/table_template.h"

/* The prepare whose closure returns nothing. */
#define TXN_MEMTABLE_TABLE_RS Unit
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#include "doltgres/txn/memtable/table_template.h"

/*
 * TxnInsertOld is Old: the end an update pushes on the row it replaces
 * before its new row, its row change, the longest record its version takes,
 * and its row lock.
 */
typedef struct TxnInsertOld
{
	TxnRecordRowChange change;
	size_t		need;
	TxnMemtableLockMode lock;
} TxnInsertOld;

/* TXN_INSERT_LOCKER is the size and alignment of a carried locker: its writer and its mode. */
#define TXN_INSERT_LOCKER sizeof(txn_TupleWriterLockMode_t), _Alignof(txn_TupleWriterLockMode_t)

/*
 * TxnInsertWriting is Writing: what every turn of an insert or an update
 * writes, the new row's change and the longest record its version takes,
 * the old row's end for an update that pushes one, whether the storage is
 * logged and logical decoding reads it, the row's room, its TOAST relation's
 * state with its stored values' entries, the key's stripe and resolve, and
 * an update's replaced row with its read.
 */
typedef struct TxnInsertWriting
{
	TxnRecordRowChange change;
	bool		has_roomed;
	TxnStatusRoomed roomed;
	bool		has_token;
	uint32		token;

	/*
	 * taken is the tuple id a turn took before its resolve waited, which
	 * every later turn keeps.
	 */
	bool		has_taken;
	uint64		taken;
	size_t		need;
	bool		has_old;
	TxnInsertOld old;
	bool		logged;
	bool		decoded;
	const TxnInsertRoom *room;

	/*
	 * toast is the TOAST relation's state, NULL for none, and entries its
	 * stored values' entries.
	 */
	const HostSharedHeld *toast;
	const TxnValuesEntry *entries;
	size_t		entries_len;
	TxnInsertKeyed keyed;
	bool		has_replacing;
	TxnInsertReplaces replaces;
	TxnChangeDecide decide;
	void	   *decide_captures;
	void		(*decide_drop) (void *);
	void	   *stopped;
	TxnInsertProgress progress;

	/*
	 * carrying is a Vec<(Writer, LockMode)>: the replaced row's lockers the
	 * last turn's read found.
	 */
	StdVec		carrying;
} TxnInsertWriting;

/* txn_insert_Writing_drop frees what every turn writes. */
static void
txn_insert_Writing_drop(TxnInsertWriting *self)
{
	txn_record_RowChange_drop(&self->change);
	if (self->has_old)
		txn_record_RowChange_drop(&self->old.change);
	if (self->has_replacing)
	{
		std_vec_free_Checked(&self->replaces.key, 1, 1);
		if (self->decide_drop != NULL)
			self->decide_drop(self->decide_captures);
	}
	txn_insert_Progress_drop(&self->progress);
	std_vec_free_Checked(&self->carrying, TXN_INSERT_LOCKER);
}

/* txn_insert_named returns a successor naming the new row by its key and a tuple id the take sets later. */
static void
txn_insert_named(const StdVec *key, TxnMemtableSuccessor *result)
{
	static const uint8 zero[8] = {0};

	result->tag = TXN_MEMTABLE_SUCCESSOR_KEY;
	std_vec_with_capacity_Checked(&result->u.key, key->len + 8, 1, 1);
	std_vec_extend_from_slice_Checked(&result->u.key, key->ptr, key->len, 1, 1);
	std_vec_extend_from_slice_Checked(&result->u.key, zero, 8, 1, 1);
}

/*
 * txn_insert_ended forms the end an update pushes on the row it replaces
 * before its new row, false when its new row's own op ends it: a keyless
 * table's removal of the copy, a deferrable key's delete of its row, or a
 * keyed table's move, each naming the new row by its key and the tuple id the
 * take sets.
 */
static bool
txn_insert_ended(const TxnInsertInsert *row, const TxnInsertReplaces *replaces, bool same, TxnInsertOld *result)
{
	TxnMemtableVersionShown combo = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMBO};
	TxnRecordRowChange *change = &result->change;

	if (!row->keyless && same)
		return false;
	memset(change, 0, sizeof(*change));
	if (row->keyless)
	{
		change->op.tag = TXN_MEMTABLE_OP_REMOVE;
		change->op.u.remove.f0 = replaces->tuple;
		change->op.u.remove.has_f1 = true;
		txn_insert_named(&row->key, &change->op.u.remove.f1);
	}
	else if (row->deferred)
	{
		change->op.tag = TXN_MEMTABLE_OP_DELETE_ROW;
		change->op.u.delete_row.f0 = replaces->tuple;
		change->op.u.delete_row.has_f1 = true;
		txn_insert_named(&row->key, &change->op.u.delete_row.f1);
	}
	else
	{
		change->op.tag = TXN_MEMTABLE_OP_MOVE;
		txn_insert_named(&row->key, &change->op.u.move);
	}
	result->need = txn_memtable_version_encoded(&row->writer, &change->op, combo, row->logged, 0);
	change->writer = row->writer;
	change->storage = row->storage;
	change->table = row->table;
	std_vec_new(&change->key, 1, 1);
	std_vec_new(&change->blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
	std_vec_new(&change->kept, sizeof(size_t), _Alignof(size_t));
	change->no_key = replaces->lock == TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE;
	result->lock = replaces->lock;
	std_slice_to_vec_Checked(&change->key, replaces->key.ptr, replaces->key.len, 1, 1);
	return true;
}

/* txn_insert_TakeStep_t is a take's step's captures: the entry it reads. */
typedef struct txn_insert_TakeStep_t
{
	HostSharedRecord record;
	const TxnInsertBase *base;
	const TxnInsertRoom *room;
	uint32		block;
	uint64		counter;
	uint64		tid;
} txn_insert_TakeStep_t;

/*
 * txn_insert_filled is the step of an update whose new row found no room on
 * its replaced row's block: the block's full flag set, as heap_update sets
 * PD_PAGE_FULL when its new tuple goes to another page; a block with no entry
 * or one already flagged takes nothing, and an entry that doesn't read
 * answers Retry, which only corruption causes.
 */
static TxnMemtableMapStep
txn_insert_filled(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	const txn_insert_TakeStep_t *step = captures;
	TxnFreeSpaceFree entry;

	if (!has_current)
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	if (!txn_insert_parsed(current, current_len, step->block, step->base, txn_state_counter(step->record),
						   step->room, &entry))
		return TXN_MEMTABLE_MAP_STEP_RETRY;
	if (entry.full)
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	entry.full = true;
	txn_free_space_Free_encode_into(&entry, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/*
 * txn_insert_take_block is a take's step on a block other than the fresh
 * page, pure over its input: Refuse when the entry has no room for the row,
 * and otherwise the entry with its lowest free offset and the row's bytes
 * taken, written into out, or its removal when it was the last.
 */
static TxnMemtableMapStep
txn_insert_take_block(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out,
					  TxnInsertGot *kept)
{
	const txn_insert_TakeStep_t *step = captures;
	TxnFreeSpaceFree entry;
	uint16		offset;

	if (!has_current)
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	if (!txn_insert_parsed(current, current_len, step->block, step->base, txn_state_counter(step->record),
						   step->room, &entry))
		return TXN_MEMTABLE_MAP_STEP_RETRY;
	if (!txn_free_space_Free_lowest(&entry, &offset) || entry.bytes < txn_insert_Room_need(step->room))
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	txn_free_space_Free_take(&entry, offset, step->room->bytes);
	kept->tag = TXN_INSERT_GOT_TID;
	kept->tid = (uint64) step->block << 16 | offset;
	if (txn_free_space_Free_is_clear(&entry))
		return TXN_MEMTABLE_MAP_STEP_REMOVE;
	txn_free_space_Free_encode_into(&entry, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/*
 * txn_insert_take_fresh is a take's step on the fresh page, the counter's
 * block, pure over its input: its lowest freed offset, else the counter's
 * while the page has room and offsets, else the page left for a new one with
 * its counter offsets handed to the map; Retry when the counter moved since
 * it was read. An untouched page has a page's bytes free; a touched one with
 * no entry is full.
 */
static TxnMemtableMapStep
txn_insert_take_fresh(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out,
					  TxnInsertGot *kept)
{
	const txn_insert_TakeStep_t *step = captures;
	uint32		block = (uint32) (step->counter >> 16);
	uint16		offset = (uint16) (step->counter & 0xffff);
	TxnFreeSpaceFree entry;
	uint16		freed;
	uint16		next;

	if (txn_state_counter(step->record) != step->counter)
		return TXN_MEMTABLE_MAP_STEP_RETRY;
	if (has_current)
	{
		if (!txn_free_space_Free_parsed(current, current_len, block, step->counter, step->room->most, &entry))
			return TXN_MEMTABLE_MAP_STEP_RETRY;
	}
	else
		entry = txn_free_space_Free_new(offset == 1 ? step->room->page : 0, NULL, 0, step->room->most);
	if (txn_free_space_Free_lowest(&entry, &freed) && entry.bytes >= txn_insert_Room_need(step->room))
	{
		txn_free_space_Free_take(&entry, freed, step->room->bytes);
		kept->tag = TXN_INSERT_GOT_TID;
		kept->tid = (uint64) block << 16 | freed;
		next = offset;
	}
	else if (offset <= step->room->most && entry.bytes >= txn_insert_Room_need(step->room))
	{
		entry.bytes = STD_SUB(u64, entry.bytes, step->room->bytes, 1178, 13);
		kept->tag = TXN_INSERT_GOT_COUNTED;
		kept->tid = step->counter;
		next = STD_ADD(u16, offset, 1, 1179, 37);
	}
	else
	{
		txn_free_space_Free_free_from(&entry, offset, step->room->most);
		kept->tag = TXN_INSERT_GOT_LEFT;
		kept->tid = 0;
		if (txn_free_space_Free_is_clear(&entry))
			return TXN_MEMTABLE_MAP_STEP_REMOVE;
		txn_free_space_Free_encode_into(&entry, out);
		return TXN_MEMTABLE_MAP_STEP_PUT;
	}
	if (entry.bytes == 0 || (txn_free_space_Free_is_clear(&entry) && next > step->room->most))
		return TXN_MEMTABLE_MAP_STEP_REMOVE;
	txn_free_space_Free_encode_into(&entry, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_insert_fresh_moved is a fresh take's then: the counter past the tuple id it counted, or a leave's next page. */
static void
txn_insert_fresh_moved(void *captures, const TxnInsertGot *got)
{
	const txn_insert_TakeStep_t *step = captures;

	if (got->tag == TXN_INSERT_GOT_COUNTED)
		txn_state_moved(step->record, got->tid, STD_ADD(u64, got->tid, 1, 952, 69));
	else if (got->tag == TXN_INSERT_GOT_LEFT)
		txn_state_moved(step->record, step->counter, STD_ADD(u64, step->counter >> 16, 1, 953, 65) << 16 | 1);
}

/* txn_insert_Took_t is a turn's take's captures: its step's, whether its block is the fresh page, and what it got. */
typedef struct txn_insert_Took_t
{
	txn_insert_TakeStep_t step;
	bool		fresh;
	TxnInsertGot got;
} txn_insert_Took_t;

/* txn_insert_took_step is a turn's take's step, the fresh page's or another block's, keeping what it got. */
static TxnMemtableMapStep
txn_insert_took_step(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	txn_insert_Took_t *took = captures;

	if (took->fresh)
		return txn_insert_take_fresh(&took->step, has_current, current, current_len, out, &took->got);
	return txn_insert_take_block(&took->step, has_current, current, current_len, out, &took->got);
}

/* txn_insert_took_then is a turn's take's then: the fresh page's counter moved past what it got. */
static void
txn_insert_took_then(void *captures)
{
	txn_insert_Took_t *took = captures;

	if (took->fresh)
		txn_insert_fresh_moved(&took->step, &took->got);
}

/*
 * txn_insert_stored is a stored value's step: its entry written, where the map
 * holds none, since a new value id skips every id the map holds (H13). Its
 * take has no read to compare: the value id is new, so no entry falls back
 * from under it (bugs row 342).
 */
static TxnMemtableMapStep
txn_insert_stored(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	Assert(!has_current);
	txn_values_Entry_encode_into(captures, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/*
 * txn_insert_added is a kept value's step, values' count add. Its take falls
 * back to the newest entry its caller read whole, which a spill moves
 * unchanged, and the values mark its caller rechecks before the window
 * covers a write between, so it has no read to compare (bugs row 342).
 */
static TxnMemtableMapStep
txn_insert_added(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	return txn_values_added(has_current, current, current_len, out);
}

/* TXN_INSERT_TAKE is the size and alignment of a prepare's take of a map's key. */
#define TXN_INSERT_TAKE sizeof(TxnMemtableMapTake), _Alignof(TxnMemtableMapTake)

/* TXN_INSERT_ROW is the size and alignment of a prepare's row. */
#define TXN_INSERT_ROW sizeof(txn_TupleRefSliceU8Usize_t), _Alignof(txn_TupleRefSliceU8Usize_t)

/* TXN_INSERT_ID is the size and alignment of a value id. */
#define TXN_INSERT_ID sizeof(uint32), _Alignof(uint32)

/* txn_insert_nothing is a take's then that does nothing. */
static void
txn_insert_nothing(void *captures)
{
}

/* txn_insert_locker_eq is (Writer, LockMode)'s PartialEq: a carried locker's writer and its mode. */
static bool
txn_insert_locker_eq(const void *a, const void *b)
{
	const txn_TupleWriterLockMode_t *self = a;
	const txn_TupleWriterLockMode_t *other = b;

	return txn_memtable_version_Writer_PartialEq_eq(&self->f0, &other->f0) && self->f1 == other->f1;
}

/* txn_insert_put returns the put of a row change's op, NULL for an op that puts none. */
static TxnMemtablePut *
txn_insert_put(TxnMemtableOp *op)
{
	switch (op->tag)
	{
		case TXN_MEMTABLE_OP_PUT:
			return &op->u.put;
		case TXN_MEMTABLE_OP_ADD:
			return &op->u.add;
		case TXN_MEMTABLE_OP_ADD_ROW:
			return &op->u.add_row;
		case TXN_MEMTABLE_OP_UPDATE_ROW:
			return &op->u.update_row.f1;
		default:
			return NULL;
	}
}

/* txn_insert_contains reports whether ascending places hold one. */
static bool
txn_insert_contains(const StdVec *places, size_t place)
{
	for (size_t i = 0; i < places->len; i++)
		if (STD_VEC_AT(places, size_t, i) == place)
			return true;
	return false;
}

/*
 * txn_insert_turn_frame_t is a turn's state across its pad and its prepare:
 * what it reads and builds before the prepare, each while live, which its
 * closure reads and consumes.
 */
typedef struct txn_insert_turn_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	HostSharedRecord record;
	const TxnInsertBase *base;
	TxnInsertWriting *w;
	HostIoWaits *waits;
	const TxnInsertUnique *read;
	const StdVec *kept;
	TxnInsertTarget target;
	uint32		block;
	TxnInsertSeen seen;
	TxnRecordEncoding encoding;
	bool		encoding_live;
	TxnRecordEncoding ending;
	bool		ending_live;
	ChunksRecordRecordWriter leaving;
	bool		leaving_live;
	ChunksRecordRecordWriter spending;
	bool		spending_live;
	StdVec		takes;
	bool		takes_live;
	size_t		first;
	size_t		flagging;
	size_t		marking;
	size_t		carried;
	size_t		stored;
	bool		has_flagged;
	uint32		flagged;
	TxnInsertSeen flagged_seen;
	StdVec		marks;
	bool		marks_live;
	StdVec		key;
	bool		key_live;
	bool		has_replaced;
	StdVec		replaced;
	StdVec		rows;
	bool		rows_live;
	bool		carries;
	TxnInsertTurn *result;
	HostError  *error;
	bool		ok;
} txn_insert_turn_frame_t;

/*
 * txn_insert_turn_closure_9_frame_t is the turn's prepare's closure's state
 * across its pad: the replaced key's versions and lockers, the resolve's
 * answer, the stripes' guard, the decisions of the target's take and the
 * flagged block's, the checkpoint window, and a record's body, each while
 * live, and the target's take's captures.
 */
typedef struct txn_insert_turn_closure_9_frame_t
{
	txn_insert_turn_frame_t *turn;
	HostIoIo   *io;
	TxnMemtableTablePreparedState *prepared;
	HostIoStriping striping;
	StdVec		versions;
	bool		versions_live;
	StdVec		lockers;
	bool		lockers_live;
	TxnVisibleResolved answer;
	bool		answer_live;
	HostKeyGuard guard;
	bool		guard_live;
	txn_insert_Took_t took;
	TxnMemtableMapDecision taking;
	bool		taking_live;
	TxnMemtableMapDecision flagging;
	bool		flagging_live;
	HostDelayCheckpoint window;
	bool		window_live;
	StdVec		body;
	bool		body_live;
	TxnInsertTurn *result;
	HostError  *error;
	bool		ok;
} txn_insert_turn_closure_9_frame_t;

/* txn_insert_turn_closure_9_finish ends the turn's window and answers a turn of the tag. */
static void
txn_insert_turn_closure_9_finish(txn_insert_turn_closure_9_frame_t *frame, TxnInsertTurnTag tag)
{
	frame->window_live = false;
	host_DelayCheckpoint_finish(&frame->window);
	frame->result->tag = tag;
	frame->ok = true;
}

/* txn_insert_turn_closure_9_logged appends a record's body to the WAL, freeing the body. */
static bool
txn_insert_turn_closure_9_logged(txn_insert_turn_closure_9_frame_t *frame, HostIoIoDoors doors, HostWalKind kind,
								 HostLsn *lsn)
{
	frame->body_live = true;
	if (!host_wal_insert(doors, kind, frame->body.ptr, frame->body.len, lsn, frame->error))
		return false;
	frame->body_live = false;
	std_vec_free_Checked(&frame->body, 1, 1);
	return true;
}

/*
 * txn_insert_turn_closure_9_spent logs, for a logged turn that took its
 * target, the take of a tuple id the turn keeps without its row, answering
 * the record's end, which the take carries.
 */
static bool
txn_insert_turn_closure_9_spent(txn_insert_turn_closure_9_frame_t *frame, HostIoIoDoors doors, uint64 tid,
								const TxnInsertRoom *room, bool *has_lsn, HostLsn *lsn)
{
	txn_insert_turn_frame_t *turn = frame->turn;
	TxnInsertWriting *w = turn->w;
	TxnRecordSpent spent = {.writer = w->change.writer, .storage = w->change.storage, .tuple = {tid},
	.heap_bytes = room->bytes};

	*has_lsn = false;
	if (!w->logged || turn->target.tag == TXN_INSERT_TARGET_TAKEN)
		return true;
	turn->spending_live = false;
	txn_record_Spent_encode_with(&spent, &turn->spending, &frame->body);
	*has_lsn = true;
	return txn_insert_turn_closure_9_logged(frame, doors, HOST_WAL_KIND_ROW_CHANGE, lsn);
}

/*
 * txn_insert_turn_closure_9_took pushes the turn's take of its target's
 * block as decided, carrying the end of the record that names its tuple id
 * when has_lsn; a target taken already has no take.
 */
static bool
txn_insert_turn_closure_9_took(txn_insert_turn_closure_9_frame_t *frame, HostIoTakesShort io, bool has_lsn,
							   HostLsn lsn)
{
	TxnInsertWriting *w = frame->turn->w;

	if (frame->turn->target.tag == TXN_INSERT_TARGET_TAKEN)
		return true;
	return txn_memtable_map_decided_finish(io, frame->prepared, 0, w->change.writer, w->has_roomed, w->roomed,
										   has_lsn, lsn, &frame->taking, txn_insert_Got_tuple(&frame->took.got),
										   txn_insert_took_then, &frame->took, frame->error);
}

/* txn_insert_turn_closure_9_body reads under the prepare's hold, rechecks under the stripes, and runs the window. */
static void
txn_insert_turn_closure_9_body(void *frame_)
{
	txn_insert_turn_closure_9_frame_t *frame = frame_;
	txn_insert_turn_frame_t *turn = frame->turn;
	TxnInsertWriting *w = turn->w;
	const HostDeltaHold *hold = txn_memtable_table_PreparedState_hold(frame->prepared);
	static const char *const pauses[] = {"doltgres-update-ended", "doltgres-insert-logged"};
	bool		has_recheck[2] = {false, false};
	bool		has_newest[2] = {false, false};
	TxnMemtableVersionHandle newest[2];
	TxnMemtableVersionShown shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	uint32		cmin = 0;
	bool		has_ended = false;
	TxnMemtableVersionHandle ended;
	bool		has_refused = false;
	bool		has_live = false;
	uint64		live = 0;
	bool		has_waiting = false;
	HostXid		waiting_xid = {0};
	bool		has_waiting_tid = false;
	HostRow		waiting_tid = {0};
	bool		has_waiting_token = false;
	uint32		waiting_token = 0;
	bool		has_carried;
	uint64		carried;
	TxnMemtablePut *carrier;
	uint32		stripes[2] = {w->keyed.stripe, w->replaces.stripe};
	HostIoKeyIo key_io;
	HostIoTakesShort io;
	HostIoIoDoors doors;
	TxnMemtableVersionWriter writer = w->change.writer;
	TxnInsertRoom own;
	const TxnInsertRoom *room = w->room;
	size_t		keeping[TXN_MEMTABLE_TABLE_KEPT_MOST];
	size_t		keeping_len = 0;
	TxnMemtableMapTaken taken;
	uint64		tid;
	TxnMemtableVersionShown own_shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	TxnMemtableVersionHandle handle;
	TxnMemtablePut *put;
	bool		has_lsn = false;
	HostLsn		lsn = 0;
	HostLsn		cid_lsn;
	bool		has_mark = false;
	std_TupleU64U64_t mark = {0};
	StdString	message;
	bool		done;

	if (w->has_replacing)
	{
		TxnChangeRead decided;

		if (!txn_memtable_table_versions(frame->io, hold, turn->record, w->replaces.key.ptr, w->replaces.key.len,
										 &frame->versions, frame->error))
			return;
		frame->versions_live = true;
		if (!w->decide(w->decide_captures, frame->io, frame->versions.ptr, frame->versions.len, &decided, w->stopped,
					   frame->error))
			return;
		if (decided.tag == TXN_CHANGE_READ_STOP)
		{
			frame->result->tag = TXN_INSERT_TURN_STOPPED;
			frame->ok = true;
			return;
		}
		shown = decided.u.go.shown;
		cmin = decided.u.go.cmin;
		has_ended = decided.u.go.has_row;
		ended = decided.u.go.row;
		if (turn->carries)
		{
			if (!txn_visible_lockers(frame->io, frame->versions.ptr, frame->versions.len, w->replaces.tuple,
									 w->change.writer.command, &frame->lockers, frame->error))
				return;
			frame->lockers_live = true;
			if (!std_vec_PartialEq_eq(&frame->lockers, &w->carrying, sizeof(txn_TupleWriterLockMode_t),
									  txn_insert_locker_eq))
			{
				std_vec_free_Checked(&w->carrying, TXN_INSERT_LOCKER);
				w->carrying = frame->lockers;
				frame->lockers_live = false;
				frame->result->tag = TXN_INSERT_TURN_RETRY;
				frame->ok = true;
				return;
			}
		}
		if (!txn_visible_carries(frame->io, frame->versions.ptr, frame->versions.len, w->replaces.tuple,
								 w->change.writer.command, &has_carried, &carried, frame->error))
			return;
		carrier = txn_record_op_put(&w->change.op);
		if (carrier->has_carried != has_carried || (has_carried && carrier->carried != carried))
		{
			w->need = w->need - 8 * carrier->has_carried + 8 * has_carried;
			carrier->has_carried = has_carried;
			carrier->carried = has_carried ? carried : 0;
			frame->result->tag = TXN_INSERT_TURN_RETRY;
			frame->ok = true;
			return;
		}
		{
			size_t		row = w->has_old && !trees_store_VecU8_PartialEq_eq(&w->replaces.key, &turn->key);

			has_recheck[row] = true;
			has_newest[row] = txn_memtable_version_held(frame->versions.ptr, frame->versions.len, &newest[row]);
		}
	}
	if (turn->read != NULL)
	{
		if (!txn_insert_resolving(frame->io, hold, turn->record, turn->key.ptr, turn->key.len, turn->read,
								  &frame->answer, &has_newest[0], &newest[0], frame->error))
			return;
		frame->answer_live = true;
		has_recheck[0] = true;
		if (frame->answer.tag == TXN_VISIBLE_RESOLVED_EXISTS)
		{
			has_refused = true;
			has_live = frame->answer.u.exists.has_f1;
			live = frame->answer.u.exists.f1.f0;
		}
		else if (frame->answer.tag == TXN_VISIBLE_RESOLVED_WAIT)
		{
			has_waiting = true;
			waiting_xid = frame->answer.u.wait.f0;
			has_waiting_tid = frame->answer.u.wait.has_f1;
			waiting_tid = frame->answer.u.wait.f1;
			has_waiting_token = frame->answer.u.wait.has_f2;
			waiting_token = frame->answer.u.wait.f2;
			if (w->has_taken)
			{
				frame->result->tag = TXN_INSERT_TURN_WAIT;
				frame->result->u.wait.f0 = waiting_xid;
				frame->result->u.wait.has_f1 = has_waiting_tid;
				frame->result->u.wait.f1 = waiting_tid;
				frame->result->u.wait.has_f2 = has_waiting_token;
				frame->result->u.wait.f2 = waiting_token;
				frame->ok = true;
				return;
			}
		}
		frame->answer_live = false;
		txn_visible_Resolved_drop(&frame->answer);
	}
	if (!host_key_lock(&frame->striping, frame->io, stripes, w->has_replacing ? 2 : 1, &frame->guard, frame->error))
		return;
	frame->guard_live = true;
	key_io = host_KeyGuard_io(&frame->guard);
	io.ops = &HOST_IO_KEY_IO_TAKES_SHORT_OPS;
	io.self = &key_io;
	doors = HOST_IO_TAKES_SHORT_IO_DOORS(io);
	if (turn->read != NULL)
	{
		switch (txn_insert_rechecked(&key_io, turn->record, turn->read, w->keyed.stripe, &w->progress))
		{
			case TXN_INSERT_RECHECKED_GO:
				break;
			case TXN_INSERT_RECHECKED_MOVED:
				frame->result->tag = TXN_INSERT_TURN_MOVED;
				frame->ok = true;
				return;
			case TXN_INSERT_RECHECKED_RETRY:
				frame->result->tag = TXN_INSERT_TURN_RETRY;
				frame->ok = true;
				return;
		}
	}
	for (size_t row = 0; row < 2; row++)
	{
		bool		has_found;
		TxnMemtableVersionHandle found;

		if (!has_recheck[row])
			continue;
		if (!txn_memtable_table_newest(io, frame->prepared, row, &has_found, &found, frame->error))
			return;
		if (has_found != has_newest[row] ||
			(has_found && !txn_memtable_version_Handle_PartialEq_eq(&found, &newest[row])))
		{
			frame->result->tag = TXN_INSERT_TURN_RETRY;
			frame->ok = true;
			return;
		}
	}
	if (w->has_replacing &&
		((turn->target.tag == TXN_INSERT_TARGET_BLOCK &&
		  (uint32) (w->replaces.tuple.f0 >> 16) == turn->target.u.block) ||
		 (turn->target.tag == TXN_INSERT_TARGET_FRESH && w->replaces.tuple.f0 >> 16 == turn->target.u.fresh >> 16)))
	{
		own = *w->room;
		own.reserve = 0;
		room = &own;
	}
	if (turn->target.tag != TXN_INSERT_TARGET_TAKEN)
		keeping[keeping_len++] = 0;
	if (turn->has_flagged && !has_waiting && !has_refused)
		keeping[keeping_len++] = turn->flagging;
	if (!txn_memtable_map_kept(io, frame->prepared, keeping, keeping_len, frame->error))
		return;
	if (turn->target.tag == TXN_INSERT_TARGET_TAKEN)
	{
		frame->took.got.tag = TXN_INSERT_GOT_TID;
		frame->took.got.tid = turn->target.u.taken;
	}
	else
	{
		TxnMemtableMapRead read = txn_insert_Seen_read(&turn->seen, turn->base);

		frame->took.step.record = turn->record;
		frame->took.step.base = turn->base;
		frame->took.step.room = room;
		frame->took.step.block = turn->block;
		frame->took.step.counter = turn->target.tag == TXN_INSERT_TARGET_FRESH ? turn->target.u.fresh : 0;
		frame->took.fresh = turn->target.tag == TXN_INSERT_TARGET_FRESH;
		if (!txn_memtable_map_decided(frame->prepared, 0, turn->record, turn->seen.has_below, turn->seen.below.ptr,
									  turn->seen.below.len, &read, txn_insert_took_step, &frame->took, &frame->taking,
									  frame->error))
			return;
		frame->taking_live = true;
		if (frame->taking.taken != TXN_MEMTABLE_MAP_TAKEN_TOOK)
		{
			frame->result->tag = frame->taking.taken == TXN_MEMTABLE_MAP_TAKEN_REFUSED ? TXN_INSERT_TURN_REFUSED :
				TXN_INSERT_TURN_RETRY;
			frame->ok = true;
			return;
		}
	}
	if (frame->took.got.tag != TXN_INSERT_GOT_LEFT && turn->has_flagged && !has_waiting && !has_refused)
	{
		txn_insert_TakeStep_t step = {.record = turn->record, .base = turn->base, .room = room,
		.block = turn->flagged};
		TxnMemtableMapRead read = txn_insert_Seen_read(&turn->flagged_seen, turn->base);

		if (!txn_memtable_map_decided(frame->prepared, turn->flagging, turn->record, turn->flagged_seen.has_below,
									  turn->flagged_seen.below.ptr, turn->flagged_seen.below.len, &read,
									  txn_insert_filled, &step, &frame->flagging, frame->error))
			return;
		frame->flagging_live = true;
		if (frame->flagging.taken == TXN_MEMTABLE_MAP_TAKEN_MOVED)
		{
			frame->result->tag = TXN_INSERT_TURN_RETRY;
			frame->ok = true;
			return;
		}
		if (frame->flagging.taken == TXN_MEMTABLE_MAP_TAKEN_RETRY)
		{
			std_string_from_Checked(&message, "a free space map's entry is corrupt", 35);
			*frame->error = host_Error_fork((const uint8 *) "XX001", &message);
			return;
		}
	}
	if (!host_delay_checkpoint(doors, pauses, lengthof(pauses), &frame->window, frame->error))
		return;
	frame->window_live = true;
	if (frame->took.got.tag == TXN_INSERT_GOT_LEFT)
	{
		if (w->logged && turn->target.tag == TXN_INSERT_TARGET_FRESH)
		{
			TxnRecordPageLeft left = {.writer = writer, .storage = w->change.storage,
			.to = STD_ADD(u64, turn->target.u.fresh >> 16, 1, 965, 83) << 16 | 1};

			turn->leaving_live = false;
			txn_record_PageLeft_encode_with(&left, &turn->leaving, &frame->body);
			if (!txn_insert_turn_closure_9_logged(frame, doors, HOST_WAL_KIND_PAGE_LEFT, &lsn))
				return;
			has_lsn = true;
		}
		if (!txn_insert_turn_closure_9_took(frame, io, has_lsn, lsn))
			return;
		txn_insert_turn_closure_9_finish(frame, TXN_INSERT_TURN_RETRY);
		return;
	}
	tid = frame->took.got.tid;
	if (has_waiting || has_refused)
	{
		if (!txn_insert_turn_closure_9_spent(frame, doors, tid, room, &has_lsn, &lsn) ||
			!txn_insert_turn_closure_9_took(frame, io, has_lsn, lsn))
			return;
		if (has_waiting)
		{
			txn_insert_turn_closure_9_finish(frame, TXN_INSERT_TURN_WAIT);
			w->has_taken = true;
			w->taken = tid;
			frame->result->u.wait.f0 = waiting_xid;
			frame->result->u.wait.has_f1 = has_waiting_tid;
			frame->result->u.wait.f1 = waiting_tid;
			frame->result->u.wait.has_f2 = has_waiting_token;
			frame->result->u.wait.f2 = waiting_token;
			return;
		}
		txn_insert_turn_closure_9_finish(frame, TXN_INSERT_TURN_EXISTS);
		frame->result->u.exists.f0 = tid;
		frame->result->u.exists.has_f1 = has_live;
		frame->result->u.exists.f1 = live;
		return;
	}
	if (w->toast != NULL)
	{
		has_mark = true;
		mark = txn_values_mark(host_shared_Held_record(w->toast));
	}
	for (size_t i = 0; i < turn->kept->len; i++)
	{
		const TxnInsertKept *read = &STD_VEC_AT(turn->kept, TxnInsertKept, i);

		if (!has_mark || mark.f0 != read->f1.f0 || mark.f1 != read->f1.f1)
		{
			txn_insert_turn_closure_9_finish(frame, TXN_INSERT_TURN_RETRY);
			return;
		}
	}
	if (w->logged && w->decoded && w->has_replacing &&
		!host_wal_new_cid(doors, &w->change.storage, w->replaces.tuple,
						  txn_change_changed_cid(shown, cmin, writer.command), &cid_lsn, frame->error))
		return;
	if (w->has_old && turn->ending_live)
	{
		TxnMemtableOp *op = &w->old.change.op;
		TxnMemtableSuccessor *named = NULL;
		TxnMemtableVersionChange end = {.writer = writer, .op = op, .lock = w->old.lock, .shown = shown};
		uint8		be[8];
		HostRow		row = {tid};

		if (op->tag == TXN_MEMTABLE_OP_MOVE)
			named = &op->u.move;
		else if (op->tag == TXN_MEMTABLE_OP_REMOVE && op->u.remove.has_f1)
			named = &op->u.remove.f1;
		else if (op->tag == TXN_MEMTABLE_OP_DELETE_ROW && op->u.delete_row.has_f1)
			named = &op->u.delete_row.f1;
		if (named != NULL && named->tag == TXN_MEMTABLE_SUCCESSOR_KEY)
		{
			uint64		big = pg_hton64(tid);

			memcpy(be, &big, 8);
			memcpy((uint8 *) named->u.key.ptr + STD_SUB(usize, named->u.key.len, 8, 1021, 26), be, 8);
			txn_record_Encoding_name(&turn->ending, row);
		}
		if (w->logged)
		{
			turn->ending_live = false;
			txn_record_RowChange_encode_with(&w->old.change, &turn->ending, &frame->body);
			if (!txn_insert_turn_closure_9_logged(frame, doors, HOST_WAL_KIND_ROW_CHANGE, &end.lsn))
				return;
			end.has_lsn = true;
		}
		if (!host_pause(HOST_SHARED_KEY_IO_PAUSES(&key_io), "doltgres-update-ended", frame->error))
			return;
		if (shown.tag == TXN_MEMTABLE_VERSION_SHOWN_COMBO && has_ended)
			done = txn_memtable_table_finish_showing(io, frame->prepared, 1, &end, ended, shown.u.combo,
													 w->has_roomed, w->roomed, &handle, frame->error);
		else
			done = txn_memtable_table_finish_row(io, frame->prepared, 1, &end, w->has_roomed, w->roomed, &handle,
												 frame->error);
		if (!done)
			return;
	}
	put = txn_insert_put(&w->change.op);
	if (put != NULL)
		put->tuple.f0 = tid;
	if (w->logged && w->decoded)
	{
		HostNewCid	cid = {.has_cmin = true, .cmin = {writer.command}};
		HostRow		row = {tid};

		if (!host_wal_new_cid(doors, &w->change.storage, row, cid, &cid_lsn, frame->error))
			return;
	}
	if (w->logged)
	{
		turn->encoding_live = false;
		txn_record_RowChange_encode_with(&w->change, &turn->encoding, &frame->body);
		if (!txn_insert_turn_closure_9_logged(frame, doors, HOST_WAL_KIND_ROW_CHANGE, &lsn))
			return;
		has_lsn = true;
	}
	if (!txn_insert_turn_closure_9_took(frame, io, has_lsn, lsn))
		return;
	if (frame->flagging_live && frame->flagging.taken == TXN_MEMTABLE_MAP_TAKEN_TOOK)
	{
		HostRow		unflagged = {0};

		if (!txn_memtable_map_decided_finish(io, frame->prepared, turn->flagging, writer, w->has_roomed, w->roomed,
											 has_lsn, lsn, &frame->flagging, unflagged, txn_insert_nothing, NULL,
											 frame->error))
			return;
	}
	txn_memtable_table_unkept(frame->prepared);
	for (size_t at = 0; at < turn->stored; at++)
	{
		if (!txn_memtable_map_finish_Unit(io, frame->prepared, at + turn->first, writer, w->has_roomed, w->roomed,
										  has_lsn, lsn, false, NULL, 0, NULL, txn_insert_stored,
										  (void *) &w->entries[at], NULL, txn_insert_nothing, NULL, NULL, &taken,
										  frame->error))
			return;
	}
	for (size_t at = 0; at < turn->kept->len; at++)
	{
		const TxnInsertKept *read = &STD_VEC_AT(turn->kept, TxnInsertKept, at);

		if (!txn_memtable_map_finish_Unit(io, frame->prepared, turn->first + turn->stored + at, writer,
										  w->has_roomed, w->roomed, has_lsn, lsn, read->has_f0, read->f0.ptr,
										  read->f0.len, NULL, txn_insert_added, NULL, NULL, txn_insert_nothing, NULL, NULL,
										  &taken, frame->error))
			return;
		if (taken != TXN_MEMTABLE_MAP_TAKEN_TOOK)
		{
			std_string_from_Checked(&message, "a kept value's map entry is missing", 35);
			*frame->error = host_Error_fork((const uint8 *) "XX001", &message);
			return;
		}
	}
	if (turn->marks_live && turn->marks.len > 0)
	{
		HostRow		row = {tid};
		StdVec		marks = turn->marks;

		std_vec_new(&turn->marks, TXN_INSERT_ID);
		if (!txn_memtable_map_named(io, frame->prepared, turn->marking, writer, w->has_roomed, w->roomed, has_lsn,
									lsn, row, &marks, frame->error))
			return;
	}
	if (!host_pause(HOST_SHARED_KEY_IO_PAUSES(&key_io), "doltgres-insert-logged", frame->error))
		return;
	if (!w->has_old && shown.tag == TXN_MEMTABLE_VERSION_SHOWN_COMBO)
	{
		own_shown.tag = TXN_MEMTABLE_VERSION_SHOWN_ENDING;
		own_shown.u.ending = shown.u.combo;
	}
	if (turn->carries)
	{
		for (size_t at = 0; at < w->carrying.len; at++)
		{
			const txn_TupleWriterLockMode_t *locker = &STD_VEC_AT(&w->carrying, txn_TupleWriterLockMode_t, at);
			TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_LOCK_ROW, .u.lock_row = {{tid}, locker->f1}};
			TxnMemtableVersionChange held = {.writer = locker->f0, .op = &op, .lock = locker->f1,
			.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
			TxnStatusRoomed none = {0};

			if (!txn_memtable_table_finish_row(io, frame->prepared, turn->carried + at, &held, false, none, &handle,
											   frame->error))
				return;
		}
	}
	{
		TxnMemtableVersionChange pushed = {.writer = writer, .op = &w->change.op,
			.lock = w->has_replacing ? w->replaces.lock : TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = own_shown,
		.has_lsn = has_lsn, .lsn = lsn};
		HostRow		row = {tid};

		if (w->has_token)
			done = txn_memtable_table_finish_speculative(io, frame->prepared, &pushed, w->token, w->has_roomed,
														 w->roomed, &handle, frame->error);
		else if (own_shown.tag == TXN_MEMTABLE_VERSION_SHOWN_ENDING && has_ended)
			done = txn_memtable_table_finish_showing(io, frame->prepared, 0, &pushed, ended, own_shown.u.ending,
													 w->has_roomed, w->roomed, &handle, frame->error);
		else
			done = txn_memtable_table_finish(io, frame->prepared, &pushed, w->has_roomed, w->roomed, &handle,
											 frame->error);
		if (!done || !txn_memtable_tuples_finish(io, frame->prepared, row, frame->error))
			return;
	}
	txn_insert_turn_closure_9_finish(frame, TXN_INSERT_TURN_TOOK);
	frame->result->u.took = tid;
}

/* txn_insert_turn_closure_9_pad frees what the turn's closure holds live. */
static void
txn_insert_turn_closure_9_pad(void *frame_)
{
	txn_insert_turn_closure_9_frame_t *frame = frame_;

	if (frame->body_live)
		std_vec_free_Checked(&frame->body, 1, 1);
	if (frame->window_live)
		host_DelayCheckpoint_drop(&frame->window);
	if (frame->taking_live)
		txn_memtable_map_Decision_drop(&frame->taking);
	if (frame->flagging_live)
		txn_memtable_map_Decision_drop(&frame->flagging);
	txn_memtable_table_unkept(frame->prepared);
	if (frame->guard_live)
		host_KeyGuard_drop(&frame->guard);
	if (frame->answer_live)
		txn_visible_Resolved_drop(&frame->answer);
	if (frame->lockers_live)
		std_vec_free_Checked(&frame->lockers, TXN_INSERT_LOCKER);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	frame->body_live = frame->window_live = frame->guard_live = false;
	frame->taking_live = frame->flagging_live = false;
	frame->answer_live = frame->lockers_live = frame->versions_live = false;
}

/* txn_insert_turn_closure_9 is the turn's prepare's closure. */
static bool
txn_insert_turn_closure_9(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
						  HostIoStriping striping, TxnInsertTurn *result, HostError *error)
{
	txn_insert_turn_closure_9_frame_t frame = {.turn = captures, .io = io, .prepared = prepared,
	.striping = striping, .result = result, .error = error};

	std_landing(&frame, txn_insert_turn_closure_9_body, txn_insert_turn_closure_9_pad);
	if (frame.window_live && !frame.ok)
		host_DelayCheckpoint_failed(&frame.window, frame.error);
	txn_insert_turn_closure_9_pad(&frame);
	return frame.ok;
}

/* txn_insert_turn_take returns a take of a map's key: the state naming the map, and the longest value it writes. */
static TxnMemtableMapTake
txn_insert_turn_take(HostSharedRecord state, TxnMemtableMapMap map, uint32 key, size_t most)
{
	TxnMemtableMapTake take = {.state = state, .map = map, .key = key, .most = most};

	return take;
}

/*
 * txn_insert_turn_touched counts a key's touch, and when its transaction's
 * keys in the tree pass the spill's threshold, names the tree, once only
 * when unique is set.
 */
static bool
txn_insert_turn_touched(HostIoIo *io, uint64 top, const HostStorage *storage, TxnWrittenOf of, const uint8 *key,
						size_t key_len, size_t need, bool unique, HostStorage over[4], TxnWrittenOf overs[4],
						size_t *over_len, HostError *error)
{
	uint64		touched;

	if (!txn_written_touched(io, top, storage, of, key, key_len, need, &touched, error))
		return false;
	if (touched < txn_spill_threshold())
		return true;
	for (size_t i = 0; unique && i < *over_len; i++)
		if (host_Storage_PartialEq_eq(&over[i], storage) && overs[i] == of)
			return true;
	over[*over_len] = *storage;
	overs[*over_len] = of;
	(*over_len)++;
	return true;
}

/* txn_insert_turn_body readies the turn's prepare: the target's entry, the encodings, the takes, and the rows. */
static void
txn_insert_turn_body(void *frame_)
{
	txn_insert_turn_frame_t *frame = frame_;
	TxnInsertWriting *w = frame->w;
	const TxnRecordRowChange *change = &w->change;
	const TxnMemtablePut *put;
	size_t		free_most = txn_free_space_most_len(w->room->most);
	uint64		top = change->writer.has_top ? change->writer.top : change->writer.xid;
	HostStorage over[4];
	TxnWrittenOf overs[4];
	size_t		over_len = 0;
	TxnMemtableMapTake take;
	txn_TupleRefSliceU8Usize_t row;

	switch (frame->target.tag)
	{
		case TXN_INSERT_TARGET_BLOCK:
			frame->block = frame->target.u.block;
			break;
		case TXN_INSERT_TARGET_FRESH:
			frame->block = (uint32) (frame->target.u.fresh >> 16);
			break;
		case TXN_INSERT_TARGET_TAKEN:
			frame->block = (uint32) (frame->target.u.taken >> 16);
			break;
	}
	if (frame->target.tag != TXN_INSERT_TARGET_TAKEN &&
		!txn_insert_seen(frame->io, frame->store, frame->record, frame->base, w->room, frame->block, &frame->seen,
						 frame->error))
		return;
	txn_record_RowChange_encoding(change, &frame->encoding);
	frame->encoding_live = true;
	if (w->has_old)
	{
		txn_record_RowChange_encoding(&w->old.change, &frame->ending);
		frame->ending_live = true;
	}
	frame->leaving = chunks_record_RecordWriter_with_capacity(TXN_RECORD_PAGE_LEFT_MOST);
	frame->leaving_live = true;
	frame->spending = chunks_record_RecordWriter_with_capacity(TXN_RECORD_SPENT_MOST);
	frame->spending_live = true;
	std_vec_new(&frame->takes, TXN_INSERT_TAKE);
	frame->takes_live = true;
	if (frame->target.tag != TXN_INSERT_TARGET_TAKEN)
	{
		take = txn_insert_turn_take(frame->record, TXN_MEMTABLE_MAP_MAP_FREE, frame->block, free_most);
		std_vec_push_Checked(&frame->takes, &take, TXN_INSERT_TAKE);
	}
	frame->first = frame->takes.len;
	if (w->toast != NULL && txn_record_RowChange_put(change, &put))
	{
		HostSharedRecord toast = host_shared_Held_record(w->toast);

		for (int pass = 0; pass < 2; pass++)
			for (size_t i = 0; i < (pass == 0 ? put->values.len : change->kept.len); i++)
			{
				size_t		place = pass == 0 ? i : STD_VEC_AT(&change->kept, size_t, i);

				if (pass == 0 && txn_insert_contains(&change->kept, place))
					continue;
				std_index_check(place, put->values.len, RUST_FILE, 808, 76);
				take = txn_insert_turn_take(toast, TXN_MEMTABLE_MAP_MAP_VALUES, STD_VEC_AT(&put->values, uint32, place),
											TXN_VALUES_MOST);
				std_vec_push_Checked(&frame->takes, &take, TXN_INSERT_TAKE);
			}
	}
	frame->flagging = frame->takes.len;
	if (w->has_replacing && (uint32) (w->replaces.tuple.f0 >> 16) != frame->block)
	{
		frame->flagged = (uint32) (w->replaces.tuple.f0 >> 16);
		take = txn_insert_turn_take(frame->record, TXN_MEMTABLE_MAP_MAP_FREE, frame->flagged, free_most);
		std_vec_push_Checked(&frame->takes, &take, TXN_INSERT_TAKE);
		if (!txn_insert_seen(frame->io, frame->store, frame->record, frame->base, w->room, frame->flagged,
							 &frame->flagged_seen, frame->error))
			return;
		frame->has_flagged = true;
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-insert-flagged-read", frame->error))
			return;
	}
	if (txn_record_RowChange_put(change, &put))
		std_slice_to_vec_Checked(&frame->marks, put->values.ptr, put->values.len, TXN_INSERT_ID);
	else
		std_vec_new(&frame->marks, TXN_INSERT_ID);
	frame->marks_live = true;
	frame->marking = frame->takes.len;
	if (frame->marks.len > 0)
	{
		take = txn_insert_turn_take(frame->record, TXN_MEMTABLE_MAP_MAP_FREE, frame->block,
									txn_memtable_map_named_most(frame->marks.len));
		std_vec_push_Checked(&frame->takes, &take, TXN_INSERT_TAKE);
	}
	std_slice_to_vec_Checked(&frame->key, change->key.ptr, change->key.len, 1, 1);
	frame->key_live = true;
	if (w->has_old)
	{
		std_slice_to_vec_Checked(&frame->replaced, w->old.change.key.ptr, w->old.change.key.len, 1, 1);
		frame->has_replaced = true;
	}
	std_vec_new(&frame->rows, TXN_INSERT_ROW);
	frame->rows_live = true;
	row.f0.ptr = frame->key.ptr;
	row.f0.len = frame->key.len;
	row.f1 = w->need;
	std_vec_push_Checked(&frame->rows, &row, TXN_INSERT_ROW);
	if (w->has_old)
	{
		row.f0.ptr = frame->replaced.ptr;
		row.f0.len = frame->replaced.len;
		row.f1 = w->old.need;
		std_vec_push_Checked(&frame->rows, &row, TXN_INSERT_ROW);
	}
	frame->carries = frame->has_replaced && !trees_store_VecU8_PartialEq_eq(&frame->replaced, &frame->key) &&
		(change->op.tag == TXN_MEMTABLE_OP_ADD || change->op.tag == TXN_MEMTABLE_OP_ADD_ROW);
	frame->carried = frame->rows.len;
	if (frame->carries)
		for (size_t i = 0; i < w->carrying.len; i++)
		{
			const txn_TupleWriterLockMode_t *locker = &STD_VEC_AT(&w->carrying, txn_TupleWriterLockMode_t, i);
			TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_LOCK_ROW, .u.lock_row = {{0}, locker->f1}};
			TxnMemtableVersionShown command = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};

			row.f0.ptr = frame->key.ptr;
			row.f0.len = frame->key.len;
			row.f1 = txn_memtable_version_encoded(&locker->f0, &op, command, false, 0);
			std_vec_push_Checked(&frame->rows, &row, TXN_INSERT_ROW);
		}
	frame->stored = w->toast != NULL ? w->entries_len : 0;
	if (!txn_insert_turn_touched(frame->io, top, &change->storage, TXN_WRITTEN_OF_ROWS, frame->key.ptr,
								 frame->key.len, w->need, false, over, overs, &over_len, frame->error))
		return;
	if (w->has_old &&
		!txn_insert_turn_touched(frame->io, top, &w->old.change.storage, TXN_WRITTEN_OF_ROWS, frame->replaced.ptr,
								 frame->replaced.len, w->old.need, false, over, overs, &over_len, frame->error))
		return;
	if (w->toast != NULL && txn_record_RowChange_put(change, &put))
	{
		HostStorage storage;
		TxnStatePersistence persistence;

		if (txn_state_described(host_shared_Held_record(w->toast), &storage, &persistence))
			for (size_t i = 0; i < put->values.len; i++)
			{
				uint8		id[4];

				std_u32_to_be_bytes(STD_VEC_AT(&put->values, uint32, i), id);
				if (!txn_insert_turn_touched(frame->io, top, &storage, TXN_WRITTEN_OF_VALUES, id, sizeof(id),
											 TXN_VALUES_MOST, true, over, overs, &over_len, frame->error))
					return;
			}
	}
	for (size_t i = 0; i < frame->takes.len; i++)
	{
		const TxnMemtableMapTake *take = &STD_VEC_AT(&frame->takes, TxnMemtableMapTake, i);
		uint8		block[4];

		if (take->map != TXN_MEMTABLE_MAP_MAP_FREE)
			continue;
		std_u32_to_be_bytes(take->key, block);
		if (!txn_insert_turn_touched(frame->io, top, &change->storage, TXN_WRITTEN_OF_FREE, block, sizeof(block),
									 take->most, true, over, overs, &over_len, frame->error))
			return;
	}
	for (size_t i = 0; i < over_len; i++)
	{
		bool		spilled;

		if (!txn_spill_spilled(frame->io, &over[i], overs[i], top, &spilled, frame->error))
			return;
	}
	frame->ok = txn_memtable_table_prepare_striped_Turn(frame->io, frame->waits, frame->record, frame->rows.ptr,
														frame->rows.len, frame->takes.ptr, frame->takes.len, true,
														txn_insert_turn_closure_9, frame, NULL, frame->result,
														frame->error);
}

/* txn_insert_turn_pad frees what the turn holds live. */
static void
txn_insert_turn_pad(void *frame_)
{
	txn_insert_turn_frame_t *frame = frame_;

	if (frame->rows_live)
		std_vec_free_Checked(&frame->rows, TXN_INSERT_ROW);
	if (frame->has_replaced)
		std_vec_free_Checked(&frame->replaced, 1, 1);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	if (frame->marks_live)
		std_vec_free_Checked(&frame->marks, TXN_INSERT_ID);
	if (frame->has_flagged)
		txn_insert_Seen_drop(&frame->flagged_seen);
	if (frame->takes_live)
		std_vec_free_Checked(&frame->takes, TXN_INSERT_TAKE);
	if (frame->spending_live)
		chunks_record_RecordWriter_drop(&frame->spending);
	if (frame->leaving_live)
		chunks_record_RecordWriter_drop(&frame->leaving);
	if (frame->ending_live)
		txn_record_Encoding_drop(&frame->ending);
	if (frame->encoding_live)
		txn_record_Encoding_drop(&frame->encoding);
	txn_insert_Seen_drop(&frame->seen);
	frame->rows_live = frame->has_replaced = frame->key_live = frame->marks_live = frame->has_flagged = false;
	frame->takes_live = frame->spending_live = frame->leaving_live = frame->ending_live = false;
	frame->encoding_live = false;
}

/*
 * txn_insert_turn makes one prepare of the row's push, an update's end of the
 * row it replaces, a take of the target's free space entry, the stored
 * values' map entries, the kept values' count adds, and a tuple id table
 * slot. Under the prepare's hold it reads an update's replaced key, which
 * decide answers, and an immediate key's versions, which the resolve
 * answers; under the keys' stripes it rechecks that the base hasn't moved
 * and that each key's newest version is the one it read, and runs its window
 * in heap_update's order: the take, and when it answers a tuple id, for a key
 * the resolve found live or running only the take's record, else the stored
 * values' entries, a check that each kept value's TOAST base is still the
 * one read, the replaced row's end with its record, heap's record of the
 * row's command where logical decoding reads the relation, the row's record,
 * the kept values' count adds with its LSN, the version, and the slot. It
 * reads the target's entry first, with no window open, and before its
 * prepare it records the row's keys and its value ids in the transaction's
 * list, which an abort prunes.
 */
static bool
txn_insert_turn(HostIoIo *io, TreesStoreNodeStore store, HostSharedRecord record, const TxnInsertBase *base,
				TxnInsertWriting *w, HostIoWaits *waits, const TxnInsertUnique *read, const StdVec *kept,
				TxnInsertTarget target, TxnInsertTurn *result, HostError *error)
{
	txn_insert_turn_frame_t frame = {.io = io, .store = store, .record = record, .base = base, .w = w,
	.waits = waits, .read = read, .kept = kept, .target = target, .result = result, .error = error};

	std_landing(&frame, txn_insert_turn_body, txn_insert_turn_pad);
	txn_insert_turn_pad(&frame);
	return frame.ok;
}

/* TxnInsertTried is how a tried turn ends the written loop: it answered, the loop starts again, or the turn refused. */
typedef enum TxnInsertTried
{
	TXN_INSERT_TRIED_ANSWERED,
	TXN_INSERT_TRIED_AGAIN,
	TXN_INSERT_TRIED_REFUSED,
	TXN_INSERT_TRIED_FAILED,
} TxnInsertTried;

/*
 * txn_insert_written_frame_t is written's state across its pad: what every
 * turn writes, and each pass's immediate key's read and kept values' entries,
 * each while live.
 */
typedef struct txn_insert_written_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	HostSharedRecord record;
	TxnInsertInsert *row;
	bool		row_live;
	const TxnInsertRoom *room;
	const TxnVacuumPrune *prune;
	HostIoWaits *waits;
	TxnInsertWriting w;
	bool		w_live;
	bool		resolving;
	TxnInsertUnique read;
	bool		read_live;
	StdVec		kept;
	bool		kept_live;
	TxnInsertBase base;
	TxnInsertInserted *result;
	HostError  *error;
	bool		ok;
} txn_insert_written_frame_t;

/* txn_insert_tried runs a turn at a target and answers how the written loop goes on. */
static TxnInsertTried
txn_insert_tried(txn_insert_written_frame_t *frame, TxnInsertTarget target)
{
	TxnInsertWriting *w = &frame->w;
	TxnInsertTurn turn;
	uint32		index;

	if (!txn_insert_turn(frame->io, frame->store, frame->record, &frame->base, w, frame->waits,
						 frame->read_live ? &frame->read : NULL, &frame->kept, target, &turn, frame->error))
		return TXN_INSERT_TRIED_FAILED;
	switch (turn.tag)
	{
		case TXN_INSERT_TURN_TOOK:
			frame->result->tag = TXN_INSERT_INSERTED_TOOK;
			frame->result->u.took.tuple = turn.u.took;
			frame->result->u.took.has_carried = txn_record_op_put(&w->change.op)->has_carried;
			frame->result->u.took.carried = txn_record_op_put(&w->change.op)->carried;
			return TXN_INSERT_TRIED_ANSWERED;
		case TXN_INSERT_TURN_EXISTS:
			frame->result->tag = TXN_INSERT_INSERTED_EXISTS;
			frame->result->u.exists.spent = turn.u.exists.f0;
			frame->result->u.exists.has_live = turn.u.exists.has_f1;
			frame->result->u.exists.live = turn.u.exists.f1;
			return TXN_INSERT_TRIED_ANSWERED;
		case TXN_INSERT_TURN_STOPPED:
			frame->result->tag = TXN_INSERT_INSERTED_STOPPED;
			return TXN_INSERT_TRIED_ANSWERED;
		case TXN_INSERT_TURN_WAIT:
			txn_insert_Progress_waiting(&w->progress);
			index = w->keyed.has_unique ? w->keyed.unique.index : 0;
			if (!txn_insert_waited(frame->io, frame->waits, index, turn.u.wait.f0, turn.u.wait.has_f1,
								   turn.u.wait.f1, turn.u.wait.has_f2, turn.u.wait.f2, frame->error))
				return TXN_INSERT_TRIED_FAILED;
			return TXN_INSERT_TRIED_AGAIN;
		case TXN_INSERT_TURN_RETRY:
		case TXN_INSERT_TURN_MOVED:
			return TXN_INSERT_TRIED_AGAIN;
		case TXN_INSERT_TURN_REFUSED:
			return TXN_INSERT_TRIED_REFUSED;
	}
	pg_unreachable();
}

/* txn_insert_written_pass_drop frees a pass's immediate key's read and kept values' entries. */
static void
txn_insert_written_pass_drop(txn_insert_written_frame_t *frame)
{
	if (frame->read_live)
		txn_insert_Unique_drop(&frame->read);
	if (frame->kept_live)
		txn_insert_kept_drop(&frame->kept);
	frame->read_live = frame->kept_live = false;
}

/* txn_insert_target returns a target of a tag at a block, a counter, or a taken tuple id. */
static TxnInsertTarget
txn_insert_target(TxnInsertTargetTag tag, uint64 at)
{
	TxnInsertTarget target = {.tag = tag};

	if (tag == TXN_INSERT_TARGET_BLOCK)
		target.u.block = (uint32) at;
	else if (tag == TXN_INSERT_TARGET_FRESH)
		target.u.fresh = at;
	else
		target.u.taken = at;
	return target;
}

/*
 * txn_insert_pass runs one pass of written: the immediate key's read, the
 * base, the kept values' entries, then turns at an earlier turn's taken
 * place, an update's replaced row's block, the last insert's, the maps'
 * candidates, and the fresh page, until one answers or the pass starts again.
 */
static TxnInsertTried
txn_insert_pass(txn_insert_written_frame_t *frame)
{
	TxnInsertWriting *w = &frame->w;
	const TxnInsertRoom *room = frame->room;
	TxnInsertTried tried;
	uint64		counter;
	uint32		fresh;
	uint32		from = 0;
	bool		has_preferred = w->has_replacing;
	uint32		preferred = (uint32) (w->replaces.tuple.f0 >> 16);
	const TxnMemtablePut *put = NULL;

	if (!txn_insert_Progress_holding(&w->progress) &&
		!host_key_wait(frame->waits, HOST_IO_IO_IO_DOORS(frame->io), w->keyed.stripe, frame->error))
		return TXN_INSERT_TRIED_FAILED;
	if (frame->resolving)
	{
		if (!txn_insert_based(frame->io, frame->store, frame->held, w->change.key.ptr, w->change.key.len,
							  &w->keyed.unique, w->keyed.stripe, &frame->read, frame->error))
			return TXN_INSERT_TRIED_FAILED;
		frame->read_live = true;
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-insert-resolved", frame->error))
			return TXN_INSERT_TRIED_FAILED;
	}
	if (!txn_insert_read_base(frame->io, frame->held, &frame->base, frame->error))
		return TXN_INSERT_TRIED_FAILED;
	txn_record_RowChange_put(&w->change, &put);
	if (!txn_insert_kept_bases(frame->io, frame->store, w->toast, put != NULL ? &put->values : NULL,
							   &w->change.kept, &frame->kept, frame->error))
		return TXN_INSERT_TRIED_FAILED;
	frame->kept_live = true;
	counter = txn_state_counter(frame->record);
	fresh = (uint32) (counter >> 16);
	if (w->has_taken)
	{
		tried = txn_insert_tried(frame, txn_insert_target(TXN_INSERT_TARGET_TAKEN, w->taken));
		return tried == TXN_INSERT_TRIED_REFUSED ? TXN_INSERT_TRIED_AGAIN : tried;
	}
	if (frame->prune != NULL && has_preferred)
	{
		TxnInsertPage page = {.bytes = room->page, .most = room->most};

		if (!txn_vacuum_pruned(frame->io, frame->store, frame->held, w->change.storage, w->logged, &page, preferred,
							   room->reserve, frame->prune, frame->error))
			return TXN_INSERT_TRIED_FAILED;
	}
	if (has_preferred)
	{
		tried = preferred == fresh ? txn_insert_tried(frame, txn_insert_target(TXN_INSERT_TARGET_FRESH, counter)) :
			txn_insert_tried(frame, txn_insert_target(TXN_INSERT_TARGET_BLOCK, preferred));
		if (tried != TXN_INSERT_TRIED_REFUSED)
			return tried;
	}
	if (frame->row->has_target)
	{
		uint32		target = frame->row->target;

		if (target == fresh)
		{
			tried = txn_insert_tried(frame, txn_insert_target(TXN_INSERT_TARGET_FRESH, counter));
			return tried == TXN_INSERT_TRIED_REFUSED ? TXN_INSERT_TRIED_AGAIN : tried;
		}
		tried = txn_insert_tried(frame, txn_insert_target(TXN_INSERT_TARGET_BLOCK, target));
		if (tried != TXN_INSERT_TRIED_REFUSED)
			return tried;
	}
	for (;;)
	{
		bool		some;
		uint32		block;

		if (!txn_insert_candidate(frame->io, frame->store, frame->record, &frame->base, room, from, fresh, &some,
								  &block, frame->error))
			return TXN_INSERT_TRIED_FAILED;
		if (!some)
			break;
		tried = txn_insert_tried(frame, txn_insert_target(TXN_INSERT_TARGET_BLOCK, block));
		if (tried != TXN_INSERT_TRIED_REFUSED)
			return tried;
		from = STD_ADD(u32, block, 1, 442, 49);
	}
	tried = txn_insert_tried(frame, txn_insert_target(TXN_INSERT_TARGET_FRESH, counter));
	return tried == TXN_INSERT_TRIED_REFUSED ? TXN_INSERT_TRIED_AGAIN : tried;
}

/* txn_insert_written_body forms what every turn writes, then runs passes until one answers. */
static void
txn_insert_written_body(void *frame_)
{
	txn_insert_written_frame_t *frame = frame_;
	TxnInsertInsert *row = frame->row;
	TxnInsertWriting *w = &frame->w;
	TxnRecordRowChange *change = &w->change;
	bool		same = w->has_replacing && trees_store_VecU8_PartialEq_eq(&w->replaces.key, &row->key);
	TxnMemtableVersionShown shown = {.tag = w->has_replacing ? TXN_MEMTABLE_VERSION_SHOWN_ENDING :
	TXN_MEMTABLE_VERSION_SHOWN_COMMAND};

	if (w->has_replacing)
	{
		w->has_old = true;
		w->has_old = txn_insert_ended(row, &w->replaces, same, &w->old);
	}
	if (row->keyless)
	{
		change->op.tag = TXN_MEMTABLE_OP_ADD;
		change->op.u.add = row->put;
	}
	else if (row->deferred && w->has_replacing && same)
	{
		change->op.tag = TXN_MEMTABLE_OP_UPDATE_ROW;
		change->op.u.update_row.f0 = w->replaces.tuple;
		change->op.u.update_row.f1 = row->put;
	}
	else if (row->deferred)
	{
		change->op.tag = TXN_MEMTABLE_OP_ADD_ROW;
		change->op.u.add_row = row->put;
	}
	else
	{
		change->op.tag = TXN_MEMTABLE_OP_PUT;
		change->op.u.put = row->put;
	}
	change->key = row->key;
	change->kept = row->kept;
	frame->row_live = false;
	w->need = txn_memtable_version_encoded(&row->writer, &change->op, shown, row->logged, 0);
	change->writer = row->writer;
	change->storage = row->storage;
	change->table = row->table;
	change->frozen = row->frozen;
	change->heap_bytes = frame->room->bytes;
	change->has_toast = row->has_toast;
	change->toast = row->toast;
	change->speculative = row->has_token;
	change->has_replaced = w->has_replacing;
	change->replaced = w->replaces.tuple;
	std_vec_with_capacity_Checked(&change->blobs, w->toast != NULL ? w->entries_len : 0,
								  sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
	for (size_t i = 0; w->toast != NULL && i < w->entries_len; i++)
	{
		chunks_TupleAddressU64_t blob = {w->entries[i].address, w->entries[i].stored};

		std_vec_push_Checked(&change->blobs, &blob, sizeof(blob), _Alignof(chunks_TupleAddressU64_t));
	}
	frame->resolving = w->keyed.has_unique && !same;
	w->has_roomed = row->has_roomed;
	w->roomed = row->roomed;
	w->has_token = row->has_token;
	w->token = row->token;
	w->logged = row->logged;
	w->decoded = row->decoded;
	w->progress = txn_insert_Progress_Default_default();
	for (;;)
	{
		TxnInsertTried tried = txn_insert_pass(frame);

		txn_insert_written_pass_drop(frame);
		if (tried == TXN_INSERT_TRIED_FAILED)
			return;
		if (tried == TXN_INSERT_TRIED_ANSWERED)
		{
			frame->ok = true;
			return;
		}
	}
}

/* txn_insert_written_pad frees what written holds live: a pass's reads, what every turn writes, and the row it took. */
static void
txn_insert_written_pad(void *frame_)
{
	txn_insert_written_frame_t *frame = frame_;

	txn_insert_written_pass_drop(frame);
	if (frame->w_live)
		txn_insert_Writing_drop(&frame->w);
	if (frame->row_live)
	{
		std_vec_free_Checked(&frame->row->key, 1, 1);
		txn_memtable_Put_drop(&frame->row->put);
		std_vec_free_Checked(&frame->row->kept, sizeof(size_t), _Alignof(size_t));
	}
	frame->w_live = frame->row_live = false;
}

/*
 * txn_insert_written writes a row as the storage's newest, taking it, under
 * the state the caller holds, and returns its tuple id: each pass reads the
 * base and the delta's free space map with no window open, picks the block
 * heap would (an update's replaced row's, then the last insert's, the fresh
 * page itself when that is the last, then the lowest the maps show with
 * room, then the fresh page), and turns there until one takes the place. A
 * turn whose immediate key's resolve must wait takes its place first, and
 * every later turn writes the row at that tuple id.
 */
static bool
txn_insert_written(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, TxnInsertInsert *row,
				   const TxnInsertRoom *room, const TxnVacuumPrune *prune, const HostSharedHeld *toast,
				   const TxnValuesEntry *toast_entries, size_t toast_entries_len, TxnInsertKeyed keyed,
				   HostIoWaits *waits, TxnInsertReplaces *replaces, TxnChangeDecide decide, void *decide_captures,
				   void (*decide_drop) (void *), void *stopped, TxnInsertInserted *result, HostError *error)
{
	txn_insert_written_frame_t frame = {.io = io, .store = store, .held = held, .record = host_shared_Held_record(held),
		.row = row, .row_live = true, .room = room, .prune = prune, .waits = waits, .w_live = true, .result = result,
	.error = error};

	frame.w.room = room;
	frame.w.toast = toast;
	frame.w.entries = toast_entries;
	frame.w.entries_len = toast_entries_len;
	frame.w.keyed = keyed;
	frame.w.has_replacing = replaces != NULL;
	if (replaces != NULL)
		frame.w.replaces = *replaces;
	frame.w.decide = decide;
	frame.w.decide_captures = decide_captures;
	frame.w.decide_drop = decide_drop;
	frame.w.stopped = stopped;
	std_vec_new(&frame.w.carrying, TXN_INSERT_LOCKER);
	std_vec_new(&frame.w.change.key, 1, 1);
	std_vec_new(&frame.w.change.blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
	std_vec_new(&frame.w.change.kept, sizeof(size_t), _Alignof(size_t));
	std_landing(&frame, txn_insert_written_body, txn_insert_written_pad);
	txn_insert_written_pad(&frame);
	return frame.ok;
}

/* txn_insert_insert writes a row, taking it, as the storage's newest, and returns its tuple id. */
bool
txn_insert_insert(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, TxnInsertInsert *row,
				  const TxnInsertRoom *room, const TxnVacuumPrune *prune, const HostSharedHeld *toast,
				  const TxnValuesEntry *toast_entries, size_t toast_entries_len, TxnInsertKeyed keyed,
				  HostIoWaits *waits, TxnInsertInserted *result, HostError *error)
{
	return txn_insert_written(io, store, held, row, room, prune, toast, toast_entries, toast_entries_len, keyed, waits,
							  NULL, NULL, NULL, NULL, NULL, result, error);
}

/*
 * txn_insert_update writes an update's new row as insert writes a row,
 * taking it, the row it replaces, and its read, and ends the row it replaces
 * in the same window: under the prepare's hold its read of the replaced row's
 * key answers decide, whose stop it returns once the prepare gave its
 * reservations back, and under both keys' stripes it rechecks that each
 * key's newest version is still the one it read. An update that keeps its
 * key pushes its new row over the row it replaces; one that moves its row to
 * another key, and every keyless table's, first pushes the replaced row's
 * end naming the new row with its own record, and one that moves a
 * row-addressed row carries the replaced row's running lockers to the new
 * row. Its new row's block is the replaced row's when that block has room
 * for the row's bytes, its fillfactor's reserve aside.
 */
bool
txn_insert_update(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, TxnInsertInsert *row,
				  TxnInsertReplaces *replaces, const TxnInsertRoom *room, const TxnVacuumPrune *prune,
				  const HostSharedHeld *toast, const TxnValuesEntry *toast_entries, size_t toast_entries_len,
				  TxnInsertKeyed keyed, HostIoWaits *waits, TxnChangeDecide decide, void *decide_captures,
				  void (*decide_drop) (void *), void *stopped, TxnInsertInserted *result, HostError *error)
{
	return txn_insert_written(io, store, held, row, room, prune, toast, toast_entries, toast_entries_len, keyed, waits,
							  replaces, decide, decide_captures, decide_drop, stopped, result, error);
}

/* txn_insert_ReplayStep_t is a replayed take's step's captures: the state, base, room, counter, and tuple id. */
typedef struct txn_insert_ReplayStep_t
{
	HostSharedRecord record;
	const TxnInsertBase *base;
	const TxnInsertRoom *room;
	uint64		counter;
	uint64		tid;
} txn_insert_ReplayStep_t;

/*
 * txn_insert_replayed is redo's step that takes a replayed tuple id's place
 * out of its block's entry: a free offset's bit and bytes go, and an offset
 * at or past the counter's on the fresh page takes its bytes and sets each
 * offset it passes free; any other take is applied already and changes
 * nothing. The fresh page's entry stays while the page can take a row, any
 * other block's while it has a free offset.
 */
static TxnMemtableMapStep
txn_insert_replayed(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	const txn_insert_ReplayStep_t *step = captures;
	uint32		block = (uint32) (step->tid >> 16);
	uint16		offset = (uint16) (step->tid & 0xffff);
	bool		fresh = (uint64) block == step->counter >> 16;
	uint16		at = (uint16) (step->counter & 0xffff);
	TxnFreeSpaceFree entry;
	uint64		next;
	bool		kept;

	if (txn_state_counter(step->record) != step->counter)
		return TXN_MEMTABLE_MAP_STEP_RETRY;
	if (has_current)
	{
		if (!txn_insert_parsed(current, current_len, block, step->base, step->counter, step->room, &entry))
			return TXN_MEMTABLE_MAP_STEP_RETRY;
	}
	else if (fresh && (step->counter & 0xffff) == 1)
		entry = txn_free_space_Free_new(step->room->page, NULL, 0, step->room->most);
	else
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	if (txn_free_space_Free_has(&entry, offset))
		txn_free_space_Free_take(&entry, offset, step->room->bytes);
	else if (fresh && offset >= at)
	{
		txn_free_space_Free_free_from(&entry, at, STD_SUB(u16, offset, 1, 1657, 33));
		entry.bytes = entry.bytes > step->room->bytes ? entry.bytes - step->room->bytes : 0;
	}
	else
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	next = Max(step->counter & 0xffff, STD_ADD(u64, step->tid & 0xffff, 1, 1662, 39));
	if (fresh)
		kept = entry.bytes > 0 && (!txn_free_space_Free_is_clear(&entry) || next <= (uint64) step->room->most);
	else
		kept = !txn_free_space_Free_is_clear(&entry);
	if (!kept)
		return TXN_MEMTABLE_MAP_STEP_REMOVE;
	txn_free_space_Free_encode_into(&entry, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_insert_replayed_tuple is a replayed take's tuple: the tuple id it replays. */
static HostRow
txn_insert_replayed_tuple(void *captures)
{
	const txn_insert_ReplayStep_t *step = captures;
	HostRow		tuple = {step->tid};

	return tuple;
}

/* txn_insert_replayed_moved is a replayed take's then: the counter moved past a tuple id at or past it. */
static void
txn_insert_replayed_moved(void *captures)
{
	const txn_insert_ReplayStep_t *step = captures;

	if (step->tid >= step->counter)
		txn_state_moved(step->record, step->counter, STD_ADD(u64, step->tid, 1, 1290, 55));
}

/* txn_insert_JumpStep_t is a counter jump's step's captures: the state, the room, the counter, and its target. */
typedef struct txn_insert_JumpStep_t
{
	HostSharedRecord record;
	const TxnInsertRoom *room;
	uint64		counter;
	uint64		to;
} txn_insert_JumpStep_t;

/*
 * txn_insert_jumped is the step of a counter's jump from its load to a later
 * tuple id: it sets free each offset of the counter's block from the
 * counter's to the one before to's, or to a page's most tuples when to is a
 * later block, as a leave hands its counter offsets to the map.
 */
static TxnMemtableMapStep
txn_insert_jumped(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	const txn_insert_JumpStep_t *step = captures;
	uint32		block = (uint32) (step->counter >> 16);
	uint16		offset = (uint16) (step->counter & 0xffff);
	TxnFreeSpaceFree entry;
	uint16		last;

	if (txn_state_counter(step->record) != step->counter)
		return TXN_MEMTABLE_MAP_STEP_RETRY;
	if (has_current)
	{
		if (!txn_free_space_Free_parsed(current, current_len, block, step->counter, step->room->most, &entry))
			return TXN_MEMTABLE_MAP_STEP_RETRY;
	}
	else
		entry = txn_free_space_Free_new(offset == 1 ? step->room->page : 0, NULL, 0, step->room->most);
	if (step->to >> 16 == step->counter >> 16)
		last = STD_SUB(u16, (uint16) (step->to & 0xffff), 1, 1583, 17);
	else
		last = step->room->most;
	txn_free_space_Free_free_from(&entry, offset, last);
	if (txn_free_space_Free_is_clear(&entry))
		return TXN_MEMTABLE_MAP_STEP_REMOVE;
	txn_free_space_Free_encode_into(&entry, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/*
 * txn_insert_kept is the step of the loader's jump of the counter: the
 * counter block's entry stays as it is, and a block with none gets a
 * removal, which no entry was before it either. That removal, which only a
 * restarted server holds, changes nothing a standby compares with its
 * primary: a fold whose run it ends deletes an entry no base holds, VACUUM's
 * walk of the takes above it reads it as no entry (bugs row 432), and the
 * counter it moves is the one the replayed tids already reached (H3).
 */
static TxnMemtableMapStep
txn_insert_kept(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	const txn_insert_JumpStep_t *step = captures;

	if (txn_state_counter(step->record) != step->counter)
		return TXN_MEMTABLE_MAP_STEP_RETRY;
	if (!has_current)
		return TXN_MEMTABLE_MAP_STEP_REMOVE;
	out->len = 0;
	std_vec_extend_from_slice_Checked(out, current, current_len, 1, 1);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_insert_jump_moved is a counter jump's then: the counter moved from its load to where it jumps. */
static void
txn_insert_jump_moved(void *captures)
{
	const txn_insert_JumpStep_t *step = captures;

	txn_state_moved(step->record, step->counter, step->to);
}

/* txn_insert_jump_closure_0_captures_t is a jump's prepare's closure's captures. */
typedef struct txn_insert_jump_closure_0_captures_t
{
	TxnMemtableVersionWriter writer;
	const TxnInsertSeen *seen;
	const TxnInsertBase *base;
	TxnMemtableMapStep (*step) (void *captures, bool has_current, const uint8 *current, size_t current_len,
								StdVec *out);
	txn_insert_JumpStep_t *captures;
	bool		has_roomed;
	TxnStatusRoomed roomed;
	bool		has_lsn;
	uint64		lsn;
} txn_insert_jump_closure_0_captures_t;

/* txn_insert_jump_closure_0 is a jump's prepare's closure: the counter block's take. */
static bool
txn_insert_jump_closure_0(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
						  TxnMemtableMapTaken *result, HostError *error)
{
	txn_insert_jump_closure_0_captures_t *inside = captures;
	TxnMemtableMapRead read = txn_insert_Seen_read(inside->seen, inside->base);

	return txn_memtable_map_finish_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, inside->writer, inside->has_roomed,
										inside->roomed, inside->has_lsn, inside->lsn, inside->seen->has_below,
										inside->seen->below.ptr,
										inside->seen->below.len, &read, inside->step, inside->captures, NULL,
										txn_insert_jump_moved, inside->captures, NULL, result, error);
}

/* txn_insert_jump_frame_t is jump's state across its pad: the counter block's entries it read. */
typedef struct txn_insert_jump_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	TxnMemtableVersionWriter writer;
	bool		has_lsn;
	uint64		lsn;
	txn_insert_JumpStep_t step_captures;
	TxnMemtableMapStep (*step) (void *captures, bool has_current, const uint8 *current, size_t current_len,
								StdVec *out);
	TxnInsertBase base;
	TxnInsertSeen seen;
	TxnMemtableMapTaken *result;
	HostError  *error;
	bool		ok;
} txn_insert_jump_frame_t;

/* txn_insert_jump_body reads the counter block's entry and takes it in one map-only prepare. */
static void
txn_insert_jump_body(void *frame_)
{
	txn_insert_jump_frame_t *frame = frame_;
	HostSharedRecord record = frame->step_captures.record;
	uint32		block = (uint32) (frame->step_captures.counter >> 16);
	TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
	txn_insert_jump_closure_0_captures_t inside = {.writer = frame->writer, .seen = &frame->seen,
		.base = &frame->base, .step = frame->step, .captures = &frame->step_captures, .has_lsn = frame->has_lsn,
	.lsn = frame->lsn};
	TxnMemtableMapTake take = txn_insert_turn_take(record, TXN_MEMTABLE_MAP_MAP_FREE, block,
												   txn_free_space_most_len(frame->step_captures.room->most));
	HostStorage storage;
	TxnStatePersistence persistence;

	if (!txn_memtable_version_Writer_PartialEq_eq(&frame->writer, &none) &&
		txn_state_described(record, &storage, &persistence))
	{
		uint64		top = frame->writer.has_top ? frame->writer.top : frame->writer.xid;
		uint64		bytes;
		bool		spilled;

		if (!txn_spill_replayed(frame->io, &storage, TXN_WRITTEN_OF_FREE, top, take.most + 4, &bytes, frame->error))
			return;
		if (bytes >= txn_spill_threshold() &&
			!txn_spill_spilled(frame->io, &storage, TXN_WRITTEN_OF_FREE, top, &spilled, frame->error))
			return;
	}
	if (!txn_memtable_version_Writer_PartialEq_eq(&frame->writer, &none) &&
		!txn_status_redo_roomed(frame->io, &frame->writer, &inside.has_roomed, &inside.roomed, frame->error))
		return;
	for (;;)
	{
		if (!txn_insert_read_base(frame->io, frame->held, &frame->base, frame->error) ||
			!txn_insert_seen(frame->io, frame->store, record, &frame->base, frame->step_captures.room, block,
							 &frame->seen, frame->error) ||
			!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-insert-jump-seen", frame->error) ||
			!txn_memtable_map_prepare_Taken(frame->io, &take, 1, txn_insert_jump_closure_0, &inside, NULL,
											frame->result, frame->error))
			return;
		if (*frame->result != TXN_MEMTABLE_MAP_TAKEN_MOVED)
			break;
		txn_insert_Seen_drop(&frame->seen);
	}
	frame->ok = true;
}

/* txn_insert_jump_pad frees the entries jump read. */
static void
txn_insert_jump_pad(void *frame_)
{
	txn_insert_jump_frame_t *frame = frame_;

	txn_insert_Seen_drop(&frame->seen);
}

/*
 * txn_insert_jump moves the counter from its load to a later tuple id by one
 * map-only prepare and a take of the counter block's entry, whose step says
 * what the entry becomes, and answers what the take did. A replayed writer's
 * take counts toward its transaction's free space map spill, as redo counts
 * its rows, before the entry is read, so a spill it makes moves no version
 * from under the entry the take falls back to; while the take answers that
 * the entry or the base moved since the read, as a spill at its prepare's
 * wait or a fold makes them, it reads the entry and prepares again (bugs row
 * 342). The take carries lsn when has_lsn.
 */
static bool
txn_insert_jump(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, TxnMemtableVersionWriter writer,
				bool has_lsn, uint64 lsn, uint64 counter, uint64 to, const TxnInsertRoom *room,
				TxnMemtableMapStep (*step) (void *captures, bool has_current, const uint8 *current,
											size_t current_len, StdVec *out),
				TxnMemtableMapTaken *result, HostError *error)
{
	txn_insert_jump_frame_t frame = {.io = io, .store = store, .held = held, .writer = writer, .has_lsn = has_lsn,
		.lsn = lsn,
		.step_captures = {.record = host_shared_Held_record(held), .room = room, .counter = counter, .to = to},
	.step = step, .result = result, .error = error};

	std_landing(&frame, txn_insert_jump_body, txn_insert_jump_pad);
	txn_insert_jump_pad(&frame);
	return frame.ok;
}

/*
 * txn_insert_leave_fresh leaves the fresh page at a counter's load for the
 * next, as redo replays a leave: the page's counter offsets go to its free
 * space entry and the counter moves to the next block's first offset. Its take
 * carries lsn, the end of the record redo replays.
 */
static bool
txn_insert_leave_fresh(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
					   TxnMemtableVersionWriter writer, uint64 lsn, uint64 counter, const TxnInsertRoom *room,
					   HostError *error)
{
	uint64		to = STD_ADD(u64, counter >> 16, 1, 1519, 15) << 16 | 1;
	TxnMemtableMapTaken taken;

	return txn_insert_jump(io, store, held, writer, true, lsn, counter, to, room, txn_insert_jumped, &taken, error);
}

/*
 * txn_insert_loaded moves a storage's tuple id counter up to a tuple id once
 * a restart loaded the checkpoint's runs into its delta, with no catalog: a
 * counter already there stays. Each move is one take of the counter block's
 * entry that leaves the entry as the runs loaded it, written by no
 * transaction.
 */
bool
txn_insert_loaded(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const TxnInsertPage *page,
				  uint64 to, HostError *error)
{
	HostSharedRecord record = host_shared_Held_record(held);
	TxnInsertRoom room = {.page = page->bytes, .most = page->most};
	TxnMemtableMapTaken taken;

	while (txn_state_counter(record) < to)
	{
		if (!txn_insert_jump(io, store, held, TXN_MEMTABLE_VERSION_NO_TRANSACTION, false, 0, txn_state_counter(record), to,
							 &room, txn_insert_kept, &taken, error))
			return false;
	}
	return true;
}

/*
 * txn_insert_redo_left applies a replayed leave of a storage's fresh page,
 * with no catalog access: when the state's counter still names the page
 * before the record's, it leaves that page for the next, and otherwise the
 * leave is applied already.
 */
bool
txn_insert_redo_left(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
					 const TxnRecordPageLeft *left, uint64 end, const TxnInsertPage *page, HostError *error)
{
	TxnInsertRoom room = {.page = page->bytes, .most = page->most};
	uint64		counter = txn_state_counter(host_shared_Held_record(held));

	if (STD_ADD(u64, counter >> 16, 1, 1503, 8) != left->to >> 16)
		return true;
	return txn_insert_leave_fresh(io, store, held, left->writer, end, counter, &room, error);
}

/* txn_insert_Placed_read reads a known tuple id's place, its row's heap bytes on a page of a storage's. */
bool
txn_insert_Placed_read(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, HostRow tid,
					   uint64 heap_bytes, const TxnInsertPage *page, TxnInsertPlaced *result, HostError *error)
{
	TxnInsertSeen seen;

	result->tid = tid.f0;
	result->room.bytes = heap_bytes;
	result->room.reserve = 0;
	result->room.page = page->bytes;
	result->room.most = page->most;
	if (!txn_insert_read_base(io, held, &result->base, error))
		return false;
	result->counter = txn_state_counter(host_shared_Held_record(held));
	if (!txn_insert_seen(io, store, host_shared_Held_record(held), &result->base, &result->room,
						 (uint32) (tid.f0 >> 16), &seen, error))
		return false;
	result->has_current = seen.has_current;
	result->current = seen.current;
	result->has_below = seen.has_below;
	result->below = seen.below;
	return true;
}

/* txn_insert_Placed_take returns the take a prepare readies for the place: its block's free space map entry. */
TxnMemtableMapTake
txn_insert_Placed_take(const TxnInsertPlaced *self, HostSharedRecord record)
{
	return txn_insert_turn_take(record, TXN_MEMTABLE_MAP_MAP_FREE, (uint32) (self->tid >> 16),
								txn_free_space_most_len(self->room.most));
}

/* txn_insert_Placed_drop frees a place's entries of its block. */
void
txn_insert_Placed_drop(TxnInsertPlaced *self)
{
	if (self->has_current)
		std_vec_free_Checked(&self->current, 1, 1);
	if (self->has_below)
		std_vec_free_Checked(&self->below, 1, 1);
	self->has_current = self->has_below = false;
}

/* txn_insert_unfree returns the internal error of a carried version's place its storage's base doesn't hold free. */
static HostError
txn_insert_unfree(uint64 tid)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("a carried version's place "), STD_FMT_ARG(0),
	STD_FMT_LIT(" isn't free in its storage's base")};
	StdFmtArg	args[] = {{&tid, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/*
 * txn_insert_place_decided decides, before the window of the prepare whose
 * take Placed_take readied, the take of a placed tuple id's place out of its
 * block's entry, with redo's step, so redo of the row's record finds it
 * taken, and keeps the block's range for the decision's finish
 * (place_taken); a place the entry doesn't hold free is an internal error.
 * It answers moved, deciding nothing, when the block's entry or the base
 * moved since Placed_read read them, so its caller reads the place again
 * (bugs rows 342 and 432).
 */
bool
txn_insert_place_decided(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t take,
						 HostSharedRecord record, const TxnInsertPlaced *placed, TxnMemtableMapDecision *result,
						 bool *moved, HostError *error)
{
	txn_insert_ReplayStep_t step = {.record = record, .base = &placed->base, .room = &placed->room,
	.counter = placed->counter, .tid = placed->tid};
	TxnMemtableMapRead read = {.has_entry = placed->has_current, .entry = placed->has_current ? placed->current.ptr : NULL,
	.entry_len = placed->has_current ? placed->current.len : 0, .generation = placed->base.generation};

	if (!txn_memtable_map_kept(io, prepared, &take, 1, error) ||
		!txn_memtable_map_decided(prepared, take, record, placed->has_below, placed->below.ptr, placed->below.len,
								  &read, txn_insert_replayed, &step, result, error))
		return false;
	*moved = result->taken == TXN_MEMTABLE_MAP_TAKEN_MOVED;
	if (result->taken == TXN_MEMTABLE_MAP_TAKEN_TOOK || *moved)
		return true;
	txn_memtable_map_Decision_drop(result);
	*error = txn_insert_unfree(placed->tid);
	return false;
}

/*
 * txn_insert_place_taken pushes a placed tuple id's decided take, as the
 * writer's, inside the window after the row's record, whose end it carries
 * when has_lsn.
 */
bool
txn_insert_place_taken(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t take,
					   TxnMemtableVersionWriter writer, bool has_roomed, TxnStatusRoomed roomed, bool has_lsn,
					   uint64 lsn, const TxnInsertPlaced *placed, const TxnMemtableMapDecision *decision,
					   HostError *error)
{
	HostRow		tuple = {placed->tid};

	return txn_memtable_map_decided_finish(io, prepared, take, writer, has_roomed, roomed, has_lsn, lsn, decision,
										   tuple, txn_insert_nothing, NULL, error);
}

/* txn_insert_carried_entry_t is carried_takes' block entry: the block and its entry after the takes so far, none for a removal. */
typedef struct txn_insert_carried_entry_t
{
	uint32		block;
	bool		has_current;
	StdVec		current;
} txn_insert_carried_entry_t;

/* txn_insert_carried_frame_t is carried_takes' state across its pad: the blocks' entries, a block's seen, and the takes, while live. */
typedef struct txn_insert_carried_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const TxnInsertCarriedPut *puts;
	size_t		puts_len;
	const TxnInsertPage *page;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	TxnInsertBase base;
	StdVec		entries;
	bool		entries_live;
	TxnInsertSeen seen;
	bool		result_live;
} txn_insert_carried_frame_t;

/* txn_insert_carried_pad frees what carried_takes holds live. */
static void
txn_insert_carried_pad(void *frame_)
{
	txn_insert_carried_frame_t *frame = frame_;

	txn_insert_Seen_drop(&frame->seen);
	if (frame->entries_live)
	{
		for (size_t k = 0; k < frame->entries.len; k++)
		{
			txn_insert_carried_entry_t *entry = &STD_VEC_AT(&frame->entries, txn_insert_carried_entry_t, k);

			if (entry->has_current)
				std_vec_free_Checked(&entry->current, 1, 1);
		}
		std_vec_free_Checked(&frame->entries, sizeof(txn_insert_carried_entry_t),
							 _Alignof(txn_insert_carried_entry_t));
	}
	if (frame->result_live)
		txn_insert_carried_takes_drop(frame->result);
	frame->entries_live = frame->result_live = false;
}

/* txn_insert_carried_body takes each put's place after the take before it of its block, the first from the block's entry below the memtable. */
static void
txn_insert_carried_body(void *frame_)
{
	txn_insert_carried_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	uint64		counter = txn_state_counter(record);

	std_vec_new(frame->result, sizeof(TxnInsertCarriedTake), _Alignof(TxnInsertCarriedTake));
	frame->result_live = true;
	std_vec_new(&frame->entries, sizeof(txn_insert_carried_entry_t), _Alignof(txn_insert_carried_entry_t));
	frame->entries_live = true;
	if (!txn_insert_read_base(frame->io, frame->held, &frame->base, frame->error))
		return;
	if (!std_vec_try_reserve_exact(frame->result, frame->puts_len, sizeof(TxnInsertCarriedTake),
								   _Alignof(TxnInsertCarriedTake)))
	{
		*frame->error = host_Error_out_of_memory();
		return;
	}
	for (size_t k = 0; k < frame->puts_len; k++)
	{
		const TxnInsertCarriedPut *put = &frame->puts[k];
		TxnInsertRoom room = {.bytes = put->heap_bytes, .page = frame->page->bytes, .most = frame->page->most};
		uint32		block = (uint32) (put->tid.f0 >> 16);
		txn_insert_carried_entry_t *entry = NULL;
		txn_insert_ReplayStep_t step = {.record = record, .base = &frame->base, .room = &room, .counter = counter,
		.tid = put->tid.f0};
		TxnMemtableVersionShown command = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
		TxnMemtableOp op = {0};
		TxnInsertCarriedTake take = {.block = block};
		StdVec		value;

		for (size_t at = 0; at < frame->entries.len && entry == NULL; at++)
			if (STD_VEC_AT(&frame->entries, txn_insert_carried_entry_t, at).block == block)
				entry = &STD_VEC_AT(&frame->entries, txn_insert_carried_entry_t, at);
		if (entry == NULL)
		{
			txn_insert_carried_entry_t seen = {.block = block};

			if (!txn_insert_seen(frame->io, frame->store, record, &frame->base, &room, block, &frame->seen,
								 frame->error))
				return;
			seen.has_current = frame->seen.has_current;
			seen.current = frame->seen.current;
			frame->seen.has_current = false;
			txn_insert_Seen_drop(&frame->seen);
			if (!std_vec_try_reserve(&frame->entries, 1, sizeof(seen), _Alignof(txn_insert_carried_entry_t)))
			{
				if (seen.has_current)
					std_vec_free_Checked(&seen.current, 1, 1);
				*frame->error = host_Error_out_of_memory();
				return;
			}
			std_vec_push_Checked(&frame->entries, &seen, sizeof(seen), _Alignof(txn_insert_carried_entry_t));
			entry = &STD_VEC_AT(&frame->entries, txn_insert_carried_entry_t, frame->entries.len - 1);
		}
		std_vec_new(&value, 1, 1);
		switch (txn_insert_replayed(&step, entry->has_current, entry->current.ptr, entry->current.len, &value))
		{
			case TXN_MEMTABLE_MAP_STEP_PUT:
				op.tag = TXN_MEMTABLE_OP_PUT;
				op.u.put.value = value;
				std_vec_new(&op.u.put.addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
				std_vec_new(&op.u.put.values, sizeof(uint32), _Alignof(uint32));
				op.u.put.tuple = put->tid;
				break;
			case TXN_MEMTABLE_MAP_STEP_REMOVE:
				std_vec_free_Checked(&value, 1, 1);
				op.tag = TXN_MEMTABLE_OP_EMPTIED;
				op.u.emptied = put->tid;
				break;
			case TXN_MEMTABLE_MAP_STEP_RETRY:
			case TXN_MEMTABLE_MAP_STEP_REFUSE:
				std_vec_free_Checked(&value, 1, 1);
				*frame->error = txn_insert_unfree(put->tid.f0);
				return;
		}
		std_vec_new(&take.record, 1, 1);
		txn_memtable_version_encode(&put->writer, &op, TXN_MEMTABLE_LOCK_MODE_UPDATE, command, false, 0, &take.record);
		if (entry->has_current)
			std_vec_free_Checked(&entry->current, 1, 1);
		entry->has_current = op.tag == TXN_MEMTABLE_OP_PUT;
		if (entry->has_current)
		{
			entry->current = op.u.put.value;
			std_vec_free_Checked(&op.u.put.addresses, sizeof(chunks_TupleU32Locator_t),
								 _Alignof(chunks_TupleU32Locator_t));
			std_vec_free_Checked(&op.u.put.values, sizeof(uint32), _Alignof(uint32));
		}
		std_vec_push_Checked(frame->result, &take, sizeof(take), _Alignof(TxnInsertCarriedTake));
	}
	frame->result_live = false;
	frame->ok = true;
}

/* txn_insert_carried_takes_drop frees a carry's takes. */
void
txn_insert_carried_takes_drop(StdVec *takes)
{
	for (size_t k = 0; k < takes->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(takes, TxnInsertCarriedTake, k).record, 1, 1);
	std_vec_free_Checked(takes, sizeof(TxnInsertCarriedTake), _Alignof(TxnInsertCarriedTake));
}

/*
 * txn_insert_carried_takes returns the free space map versions by which a
 * carry's puts of other writers take their places in a new storage whose
 * spill files they go to (P2), each put's block and its take's record, in
 * the puts' order: each as place_taken takes it after the take before it of
 * its block, the first from the block's entry below the memtable, so their
 * chains read as the memtable's would have.
 */
bool
txn_insert_carried_takes(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
						 const TxnInsertCarriedPut *puts, size_t puts_len, const TxnInsertPage *page, StdVec *result,
						 HostError *error)
{
	txn_insert_carried_frame_t frame = {.io = io, .store = store, .held = held, .puts = puts, .puts_len = puts_len,
	.page = page, .result = result, .error = error};

	std_landing(&frame, txn_insert_carried_body, txn_insert_carried_pad);
	txn_insert_carried_pad(&frame);
	return frame.ok;
}

/* txn_insert_RunKey_t is a key of RUNS: a base's point, a storage, one of its maps, and a key of that map. */
typedef struct txn_insert_RunKey_t
{
	uint64		point;
	HostStorage storage;
	uint32		map;
	uint32		key;
} txn_insert_RunKey_t;

/*
 * txn_insert_Run_t is an entry of RUNS: a key, and whether its run as the
 * base's fold took it has ended. A value given to or taken from the map is
 * a whole entry's f1, since the map copies the entry's value span with its
 * padding.
 */
typedef struct txn_insert_Run_t
{
	txn_insert_RunKey_t f0;
	bool		f1;
} txn_insert_Run_t;

/* txn_insert_RunKey_hash is RunKey's Hash. */
static void
txn_insert_RunKey_hash(const void *key, StdHasher state)
{
	const txn_insert_RunKey_t *self = key;

	std_Hasher_write_u64(state, self->point);
	std_Hasher_write_u32(state, self->storage.tablespace);
	std_Hasher_write_u32(state, self->storage.database);
	std_Hasher_write_u32(state, self->storage.relation);
	std_Hasher_write_u32(state, self->map);
	std_Hasher_write_u32(state, self->key);
}

/* txn_insert_RunKey_eq is RunKey's PartialEq. */
static bool
txn_insert_RunKey_eq(const void *a, const void *b)
{
	const txn_insert_RunKey_t *x = a;
	const txn_insert_RunKey_t *y = b;

	return x->point == y->point && host_Storage_PartialEq_eq(&x->storage, &y->storage) && x->map == y->map &&
		x->key == y->key;
}

/* TXN_INSERT_RUNS_MAP is the table of RUNS' map, a HashMap<RunKey, bool>. */
static const StdHashMapType TXN_INSERT_RUNS_MAP = {
	STD_HASHMAP_SHAPE(txn_insert_Run_t),
	.key_hash = txn_insert_RunKey_hash,
	.key_eq = txn_insert_RunKey_eq,
	.hasher = &FOLDHASH_FAST_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = foldhash_fast_RandomState_Default_default,
};

/* txn_insert_Runs_t is RUNS' value: the newest point its map holds a key at, 0 while it holds none, and the map. */
typedef struct txn_insert_Runs_t
{
	uint64		reach;
	StdHashMap	map;
} txn_insert_Runs_t;

/* taken at Runs and IoMutex<Runs>, which insert.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Runs
#define HOST_LOCK_TAKEN_T txn_insert_Runs_t
#define HOST_LOCK_TAKEN_DEFAULT(place) \
	((place)->reach = 0, std_HashMap_Default_default(&(place)->map, &TXN_INSERT_RUNS_MAP))
#define HOST_LOCK_TAKEN_DROP(place) std_HashMap_drop(&((txn_insert_Runs_t *) (place))->map, &TXN_INSERT_RUNS_MAP)
#define HOST_LOCK_TAKEN_TYPE TXN_INSERT_RUNS_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Runs
#define HOST_LOCK_T txn_insert_Runs_t
#define HOST_LOCK_DEFAULT(place) \
	((place)->reach = 0, std_HashMap_Default_default(&(place)->map, &TXN_INSERT_RUNS_MAP))
#include "doltgres/host/lock_template.h"

/* txn_insert_IoMutexRuns_drop is IoMutex<Runs>' drop glue: its value's. */
static void
txn_insert_IoMutexRuns_drop(void *value)
{
	std_HashMap_drop(&((HostLockIoMutexRuns *) value)->value.map, &TXN_INSERT_RUNS_MAP);
}

/* TXN_INSERT_RUNS_TYPE is IoMutex<Runs>' size, Default, and drop. */
static const StdDefaultType TXN_INSERT_RUNS_TYPE = {sizeof(HostLockIoMutexRuns), _Alignof(HostLockIoMutexRuns),
host_lock_IoMutexRuns_Default_default, txn_insert_IoMutexRuns_drop};

/*
 * TXN_INSERT_RUNS is RUNS: for each key of a storage's maps the startup
 * process's redo met past a base that already applied some of its takes,
 * whether the key's run as the base's fold took it has ended (bugs row 401);
 * its BackendLocal header and the process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexRuns process;
}			TXN_INSERT_RUNS = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * txn_insert_run_out_frame_t is run_out's state across its pad: RUNS' local
 * and guard, the key's hold, and its versions, each while live.
 */
typedef struct txn_insert_run_out_frame_t
{
	HostIoIo   *io;
	const TxnInsertRun *run;
	const HostSharedHeld *held;
	TxnMemtableMapMap map;
	uint32		key;
	bool	   *result;
	HostError  *error;
	bool		ok;
	HostLocalLocal local;
	bool		local_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	bool		versions_live;
	StdVec		spilled;
	bool		spilled_live;
} txn_insert_run_out_frame_t;

/* txn_insert_run_out_pad frees what run_out holds live: the versions, the hold, the guard, and the local. */
static void
txn_insert_run_out_pad(void *frame_)
{
	txn_insert_run_out_frame_t *frame = frame_;

	if (frame->spilled_live)
		txn_memtable_version_versions_drop(&frame->spilled, 0);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->guard_live)
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (frame->local_live)
		host_local_Local_drop(&frame->local, &TXN_INSERT_RUNS_TYPE);
	frame->spilled_live = frame->versions_live = frame->hold_live = frame->guard_live = frame->local_live = false;
}

/*
 * txn_insert_run_seen judges each of a key's loaded versions by the fold's
 * own test at the run's point, asking the WAL ahead for a writer the commit
 * log still shows open, and ends the run at the first the point neither
 * applies nor saw abort.
 */
static bool
txn_insert_run_seen(txn_insert_run_out_frame_t *frame, StdVec *versions, bool *ended)
{
	for (size_t i = 0; i < versions->len && !*ended; i++)
	{
		TxnMemtableVersionVersion *version = &STD_VEC_AT(versions, TxnMemtableVersionVersion, i);
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
		TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);
		uint64		top = writer.has_top ? writer.top : writer.xid;
		TxnFoldApplied applied;
		bool		ended_by = false;

		if (!txn_fold_map_applied(frame->io, version, frame->run->point, &applied, frame->error))
			return false;
		if (applied.tag == TXN_FOLD_APPLIED_STAYS && top >= TXN_STATUS_FIRST_NORMAL &&
			!frame->run->ask(frame->run->captures, frame->io, top, frame->run->point, &ended_by, frame->error))
			return false;
		*ended = applied.tag == TXN_FOLD_APPLIED_STAYS && !ended_by;
	}
	return true;
}

/*
 * txn_insert_run_out_body finds the key's entry in RUNS, seeding it from the
 * versions the start loaded on its first meeting, then decides.
 */
static void
txn_insert_run_out_body(void *frame_)
{
	txn_insert_run_out_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	txn_insert_RunKey_t key = {.point = frame->run->point, .map = (uint32) frame->map, .key = frame->key};
	TxnStatePersistence persistence;
	txn_insert_Runs_t *runs;
	txn_insert_Run_t *run;
	bool	   *ended;
	bool		seeded = false;

	*frame->result = false;
	(void) txn_state_described(record, &key.storage, &persistence);
	frame->local = host_local_BackendLocal_get(&TXN_INSERT_RUNS.local, &TXN_INSERT_RUNS_TYPE);
	frame->local_live = true;
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->local,
																						   &TXN_INSERT_RUNS_TYPE),
										  &TXN_INSERT_RUNS_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	runs = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
	run = std_HashMap_get(&runs->map, &TXN_INSERT_RUNS_MAP, &key);
	ended = run != NULL ? &run->f1 : NULL;
	if (ended == NULL && frame->run->applied)
	{
		uint8		bytes[4];

		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
			return;
		frame->hold_live = true;
		if (!txn_memtable_map_versions(frame->map == TXN_MEMTABLE_MAP_MAP_FREE ? &TXN_MEMTABLE_MAP_FREE_TREE_OPS :
									   &TXN_MEMTABLE_MAP_VALUES_TREE_OPS, frame->io, &frame->hold, record, frame->key,
									   &frame->versions, frame->error))
			return;
		frame->versions_live = true;
		std_u32_to_be_bytes(frame->key, bytes);
		if (!txn_spill_versions(&TXN_SPILL_STATE_HOME_OPS, frame->io, &frame->hold, record,
								frame->map == TXN_MEMTABLE_MAP_MAP_FREE ? TXN_RUNS_KIND_SPILLED_FREE :
								TXN_RUNS_KIND_SPILLED_VALUES, bytes, sizeof(bytes), NULL, &frame->spilled,
								frame->error))
			return;
		frame->spilled_live = true;
		if (!txn_insert_run_seen(frame, &frame->versions, &seeded) ||
			!txn_insert_run_seen(frame, &frame->spilled, &seeded))
			return;
	}
	*frame->result = frame->run->applied && !(ended != NULL ? *ended : seeded);
	if (!*frame->result && (ended == NULL || !*ended))
	{
		txn_insert_Run_t now = {.f1 = true};
		txn_insert_Run_t old;

		(void) std_HashMap_insert(&runs->map, &TXN_INSERT_RUNS_MAP, &key, &now.f1, &old.f1);
	}
	else if (ended == NULL)
	{
		txn_insert_Run_t open = {.f1 = false};
		txn_insert_Run_t old;

		(void) std_HashMap_insert(&runs->map, &TXN_INSERT_RUNS_MAP, &key, &open.f1, &old.f1);
	}
	runs->reach = Max(runs->reach, key.point);
	frame->ok = true;
}

/*
 * txn_insert_run_out answers whether redo leaves out a take of a map's key
 * because the base it replays onto already holds it, as the base's fold held
 * the key's oldest run of versions whose writers its point applied or saw
 * abort (bugs row 401): a key's run stays open while every version of it the
 * start loaded and every take of it redo met is by such a writer, and a take
 * lies in it when its own writer is one too; the first take of any other
 * writer ends the run, and every later take of the key replays. With no run,
 * no base holds the take.
 */
bool
txn_insert_run_out(HostIoIo *io, const TxnInsertRun *run, const HostSharedHeld *held, TxnMemtableMapMap map,
				   uint32 key, bool *result, HostError *error)
{
	txn_insert_run_out_frame_t frame = {.io = io, .run = run, .held = held, .map = map, .key = key, .result = result,
	.error = error};

	*result = false;
	if (run == NULL)
		return true;
	std_landing(&frame, txn_insert_run_out_body, txn_insert_run_out_pad);
	txn_insert_run_out_pad(&frame);
	return frame.ok;
}

/*
 * txn_insert_runs_passed empties RUNS once redo replays a record ending past
 * every point it holds a key at, since no later record ends at or before
 * them, so a standby keeps no runs past its start (bugs row 401).
 */
void
txn_insert_runs_passed(HostIoIo *io, uint64 end)
{
	HostLocalLocal local = host_local_BackendLocal_get(&TXN_INSERT_RUNS.local, &TXN_INSERT_RUNS_TYPE);
	HostLockLockedRwLockWriteGuard guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&local,
																												   &TXN_INSERT_RUNS_TYPE),
																  &TXN_INSERT_RUNS_LOCK_TYPE, &io->held);
	txn_insert_Runs_t *runs = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&guard);

	if (runs->reach != 0 && runs->reach < end)
	{
		std_HashMap_clear(&runs->map, &TXN_INSERT_RUNS_MAP);
		runs->reach = 0;
	}
	host_lock_LockedRwLockWriteGuard_drop(&guard);
	host_local_Local_drop(&local, &TXN_INSERT_RUNS_TYPE);
}

/* txn_insert_redo_spent_closure_0_captures_t is redo_spent's prepare's closure's captures, and what its take did. */
typedef struct txn_insert_redo_spent_closure_0_captures_t
{
	TxnMemtableVersionWriter writer;
	const TxnInsertSeen *seen;
	txn_insert_ReplayStep_t *step;
	bool		has_roomed;
	TxnStatusRoomed roomed;
	bool		left;
	uint64		end;
	TxnMemtableMapTaken taken;
} txn_insert_redo_spent_closure_0_captures_t;

/* txn_insert_redo_spent_moved is redo_spent's take's then: the counter moved past a tuple id at or past it. */
static void
txn_insert_redo_spent_moved(void *captures)
{
	const txn_insert_ReplayStep_t *step = captures;

	if (step->tid >= step->counter)
		txn_state_moved(step->record, step->counter, STD_ADD(u64, step->tid, 1, 1479, 55));
}

/*
 * txn_insert_redo_spent_closure_0 is redo_spent's prepare's closure: the
 * place's take, or with the take left out the counter's move alone, while
 * the counter is the one read.
 */
static bool
txn_insert_redo_spent_closure_0(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
								HostError *error)
{
	txn_insert_redo_spent_closure_0_captures_t *inside = captures;
	HostRow		unclaimed = {0};
	TxnMemtableMapRead read = txn_insert_Seen_read(inside->seen, inside->step->base);

	if (inside->left)
	{
		inside->taken = txn_state_counter(inside->step->record) == inside->step->counter ?
			TXN_MEMTABLE_MAP_TAKEN_TOOK : TXN_MEMTABLE_MAP_TAKEN_MOVED;
		if (inside->taken == TXN_MEMTABLE_MAP_TAKEN_TOOK)
			txn_insert_redo_spent_moved(inside->step);
		return true;
	}
	return txn_memtable_map_took_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, inside->writer, inside->has_roomed,
									  inside->roomed, true, inside->end, false, unclaimed, 0, inside->seen->has_below,
									  inside->seen->below.ptr, inside->seen->below.len, &read, txn_insert_replayed,
									  inside->step, NULL, txn_insert_replayed_tuple, inside->step, NULL,
									  txn_insert_redo_spent_moved, inside->step, NULL, &inside->taken, error);
}

/* txn_insert_redo_spent_frame_t is redo_spent's state across its pad: the block's entries it read. */
typedef struct txn_insert_redo_spent_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const TxnRecordSpent *spent;
	uint64		end;
	const TxnInsertRun *run;
	TxnInsertRoom room;
	TxnInsertBase base;
	TxnInsertSeen seen;
	HostError  *error;
	bool		ok;
} txn_insert_redo_spent_frame_t;

/* txn_insert_redo_spent_body leaves fresh pages until the place's block is reached, then takes the place. */
static void
txn_insert_redo_spent_body(void *frame_)
{
	txn_insert_redo_spent_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	uint64		tid = frame->spent->tuple.f0;
	bool		left;

	if (!txn_insert_run_out(frame->io, frame->run, frame->held, TXN_MEMTABLE_MAP_MAP_FREE, (uint32) (tid >> 16), &left,
							frame->error))
		return;
	for (;;)
	{
		uint64		counter;
		uint32		block;
		txn_insert_ReplayStep_t step;
		txn_insert_redo_spent_closure_0_captures_t inside = {.writer = frame->spent->writer, .seen = &frame->seen,
		.step = &step, .left = left, .end = frame->end, .taken = TXN_MEMTABLE_MAP_TAKEN_TOOK};
		TxnMemtableMapTake take;

		if (!txn_insert_read_base(frame->io, frame->held, &frame->base, frame->error))
			return;
		counter = txn_state_counter(record);
		if (tid >> 16 > counter >> 16)
		{
			if (!txn_insert_leave_fresh(frame->io, frame->store, frame->held, frame->spent->writer, frame->end, counter,
										&frame->room, frame->error))
				return;
			continue;
		}
		block = (uint32) (tid >> 16);
		if (!txn_insert_seen(frame->io, frame->store, record, &frame->base, &frame->room, block, &frame->seen,
							 frame->error))
			return;
		take = txn_insert_turn_take(record, TXN_MEMTABLE_MAP_MAP_FREE, block,
									txn_free_space_most_len(frame->room.most));
		if (!txn_status_redo_roomed(frame->io, &frame->spent->writer, &inside.has_roomed, &inside.roomed,
									frame->error))
			return;
		step.record = record;
		step.base = &frame->base;
		step.room = &frame->room;
		step.counter = counter;
		step.tid = tid;
		if (!txn_memtable_map_prepare_Unit(frame->io, &take, 1, txn_insert_redo_spent_closure_0, &inside, NULL,
										   frame->error))
			return;
		if (inside.taken != TXN_MEMTABLE_MAP_TAKEN_MOVED)
		{
			frame->ok = true;
			return;
		}
		txn_insert_Seen_drop(&frame->seen);
	}
}

/* txn_insert_redo_spent_pad frees the entries redo_spent read. */
static void
txn_insert_redo_spent_pad(void *frame_)
{
	txn_insert_redo_spent_frame_t *frame = frame_;

	txn_insert_Seen_drop(&frame->seen);
}

/*
 * txn_insert_redo_spent applies a replayed spent place, with no catalog
 * access: it takes the place out of the free space map as the primary's
 * take did, as redo takes a row's, and pushes no version, reading the
 * block's entry and preparing again while its take answers that the entry or
 * the base moved since the read (bugs row 342). A take its block's run holds
 * is left out, and the counter still moves (bugs row 401).
 */
bool
txn_insert_redo_spent(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
					  const TxnRecordSpent *spent, uint64 end, const TxnInsertPage *page, const TxnInsertRun *run,
					  HostError *error)
{
	txn_insert_redo_spent_frame_t frame = {.io = io, .store = store, .held = held, .spent = spent, .end = end,
		.run = run,
	.room = {.bytes = spent->heap_bytes, .page = page->bytes, .most = page->most}, .error = error};

	std_landing(&frame, txn_insert_redo_spent_body, txn_insert_redo_spent_pad);
	txn_insert_redo_spent_pad(&frame);
	return frame.ok;
}

/* txn_insert_complete_frame_t is complete's state across its pad: its hold and the key's versions, each while live. */
typedef struct txn_insert_complete_frame_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	const TxnRecordCompletion *completion;
	TxnInsertCompleted completed;
	bool		logged;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	bool		versions_live;
	StdVec		body;
	bool		body_live;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_insert_complete_frame_t;

/* txn_insert_complete_body finds the row's version, confirms or kills it, and logs the completion. */
static void
txn_insert_complete_body(void *frame_)
{
	txn_insert_complete_frame_t *frame = frame_;
	const TxnRecordCompletion *completion = frame->completion;
	TxnMemtableVersionVersion *row = NULL;
	HostLsn		lsn;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_table_versions(frame->io, &frame->hold, host_shared_Held_record(frame->held),
									 completion->key.ptr, completion->key.len, &frame->versions, frame->error))
		return;
	frame->versions_live = true;
	for (size_t i = 0; i < frame->versions.len && row == NULL; i++)
	{
		TxnMemtableVersionVersion *version = &STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, i);
		TxnVisibleRowVersion written;

		if (txn_visible_RowVersion_of(version, &written) &&
			host_Row_PartialEq_eq(&written.put.tuple, &completion->tuple))
			row = version;
	}
	if (row != NULL && frame->completed == TXN_INSERT_COMPLETED_CONFIRMED)
		txn_memtable_version_Version_confirm(row);
	else if (row != NULL)
		txn_memtable_version_Version_kill(row);
	*frame->result = row != NULL;
	frame->versions_live = false;
	txn_memtable_version_versions_drop(&frame->versions, 0);
	frame->hold_live = false;
	host_delta_Hold_drop(&frame->hold);
	if (frame->logged)
	{
		bool		killed = frame->completed == TXN_INSERT_COMPLETED_KILLED;

		txn_record_Completion_encode(completion, killed, &frame->body);
		frame->body_live = true;
		if (!host_wal_insert(HOST_IO_IO_IO_DOORS(frame->io), killed ? HOST_WAL_KIND_ROW_CHANGE : HOST_WAL_KIND_CONFIRM,
							 frame->body.ptr, frame->body.len, &lsn, frame->error))
			return;
	}
	frame->ok = true;
}

/* txn_insert_complete_pad frees what complete holds live. */
static void
txn_insert_complete_pad(void *frame_)
{
	txn_insert_complete_frame_t *frame = frame_;

	if (frame->body_live)
		std_vec_free_Checked(&frame->body, 1, 1);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->body_live = frame->versions_live = frame->hold_live = false;
}

/*
 * txn_insert_complete completes the speculative insert a completion names in
 * a storage, as heap's heap_finish_speculative clears its token and
 * heap_abort_speculative kills its tuple, by a read-modify-write of its
 * version's hint word under a hold, and for a logged storage appends its
 * record: a confirm of its own kind, which logical decoding reads, or a kill
 * as a row change, which redo applies here again. It reports whether the
 * delta holds the row's version.
 */
bool
txn_insert_complete(HostIoIo *io, const HostSharedHeld *held, const TxnRecordCompletion *completion,
					TxnInsertCompleted completed, bool logged, bool *result, HostError *error)
{
	txn_insert_complete_frame_t frame = {.io = io, .held = held, .completion = completion, .completed = completed,
	.logged = logged, .result = result, .error = error};

	std_landing(&frame, txn_insert_complete_body, txn_insert_complete_pad);
	txn_insert_complete_pad(&frame);
	return frame.ok;
}

/* txn_insert_TupleU32OptionVecU8_t is a kept value's id with its base entry, a replayed count add's fallback. */
typedef struct txn_insert_TupleU32OptionVecU8_t
{
	uint32		f0;
	bool		has_f1;
	StdVec		f1;
} txn_insert_TupleU32OptionVecU8_t;

/* TXN_INSERT_REPLAYED_KEPT is the size and alignment of a replayed kept value's id and base entry. */
#define TXN_INSERT_REPLAYED_KEPT sizeof(txn_insert_TupleU32OptionVecU8_t), _Alignof(txn_insert_TupleU32OptionVecU8_t)

/* txn_insert_redo_frame_t is redo's state across its pad and its prepare: what it reads first, each while live. */
typedef struct txn_insert_redo_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const TxnRecordRowChange *change;
	uint64		lsn;
	const TxnInsertRun *run;
	const HostSharedHeld *toast;
	const TxnValuesEntry *entries;
	size_t		entries_len;

	/*
	 * left and left_flagged say whether the runs of the row's block and of
	 * its old version's block hold their takes, and outs, for each stored and
	 * then kept value, whether its id's run holds its take.
	 */
	bool		left;
	bool		left_flagged;
	StdVec		outs;
	bool		outs_live;
	bool		has_roomed;
	TxnStatusRoomed roomed;
	TxnInsertRoom room;
	uint64		tid;
	size_t		need;
	bool		pushed;

	/*
	 * mark is the state's mark the entries were read at, and done whether the
	 * window found it unmoved.
	 */
	std_TupleU64U64_t mark;
	bool		done;
	StdVec		stored;
	bool		stored_live;
	StdVec		kept;
	bool		kept_live;
	TxnInsertBase base;
	uint64		counter;
	TxnInsertSeen seen;
	StdVec		takes;
	bool		takes_live;
	StdVec		marks;
	bool		marks_live;
	size_t		marking;
	bool		has_flagged;
	uint32		flagged;
	TxnInsertSeen flagged_seen;
	HostError  *error;
	bool		ok;
} txn_insert_redo_frame_t;

/*
 * txn_insert_redo_refuse is a replayed stored value's step: its entry where
 * the map holds none, else nothing; as txn_insert_stored's, its take has no
 * read to compare (bugs row 342).
 */
static TxnMemtableMapStep
txn_insert_redo_refuse(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	if (has_current)
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	txn_values_Entry_encode_into(captures, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_insert_redo_closure_9 is redo's prepare's closure: the take, the map's changes, the version, and the slot. */
static bool
txn_insert_redo_closure_9(void *captures, HostIoIo *io_, TxnMemtableTablePreparedState *prepared, HostError *error)
{
	txn_insert_redo_frame_t *frame = captures;
	const TxnRecordRowChange *change = frame->change;
	HostIoTakesShort io = HOST_IO_IO_TAKES_SHORT(io_);
	txn_insert_ReplayStep_t step = {.record = host_shared_Held_record(frame->held), .base = &frame->base,
	.room = &frame->room, .counter = frame->counter, .tid = frame->tid};
	HostRow		unclaimed = {0};
	HostRow		row = {frame->tid};
	TxnMemtableMapTaken taken;
	TxnMemtableVersionHandle handle;
	StdString	message;
	std_TupleU64U64_t mark = txn_values_mark(step.record);
	TxnMemtableMapRead read = txn_insert_Seen_read(&frame->seen, &frame->base);

	frame->done = mark.f0 == frame->mark.f0 && mark.f1 == frame->mark.f1 &&
		(!frame->left || txn_state_counter(step.record) == frame->counter);
	if (!frame->done)
		return true;
	if (frame->left)
		txn_insert_replayed_moved(&step);
	else if (!txn_memtable_map_took_Unit(io, prepared, 0, change->writer, frame->has_roomed, frame->roomed, true,
										 frame->lsn, false, unclaimed, 0, frame->seen.has_below, frame->seen.below.ptr,
										 frame->seen.below.len, &read, txn_insert_replayed, &step, NULL,
										 txn_insert_replayed_tuple, &step, NULL, txn_insert_replayed_moved, &step,
										 NULL, &taken, error))
		return false;
	if (!frame->left && taken == TXN_MEMTABLE_MAP_TAKEN_MOVED)
	{
		frame->done = false;
		return true;
	}
	for (size_t at = 0; frame->toast != NULL && at < frame->entries_len; at++)
	{
		if (STD_VEC_AT(&frame->outs, bool, at))
			continue;
		if (!txn_memtable_map_finish_Unit(io, prepared, at + 1, change->writer, frame->has_roomed, frame->roomed,
										  true, frame->lsn, false, NULL, 0, NULL, txn_insert_redo_refuse,
										  (void *) &frame->entries[at], NULL, txn_insert_nothing, NULL, NULL, &taken,
										  error))
			return false;
	}
	for (size_t at = 0; at < frame->kept.len; at++)
	{
		const txn_insert_TupleU32OptionVecU8_t *kept = &STD_VEC_AT(&frame->kept, txn_insert_TupleU32OptionVecU8_t,
																   at);

		if (STD_VEC_AT(&frame->outs, bool, frame->stored.len + at))
			continue;
		if (!txn_memtable_map_finish_Unit(io, prepared, 1 + frame->stored.len + at, change->writer,
										  frame->has_roomed, frame->roomed, true, frame->lsn, kept->has_f1,
										  kept->f1.ptr, kept->f1.len, NULL, txn_insert_added, NULL, NULL,
										  txn_insert_nothing, NULL, NULL, &taken, error))
			return false;
		if (taken != TXN_MEMTABLE_MAP_TAKEN_TOOK)
		{
			std_string_from_Checked(&message, "a kept value's map entry is missing", 35);
			*error = host_Error_fork((const uint8 *) "XX001", &message);
			return false;
		}
	}
	if (frame->marks.len > 0 && !frame->left)
	{
		StdVec		marks = frame->marks;

		std_vec_new(&frame->marks, TXN_INSERT_ID);
		if (!txn_memtable_map_named(io, prepared, frame->marking, change->writer, frame->has_roomed, frame->roomed,
									true, frame->lsn, row, &marks, error))
			return false;
	}
	if (!frame->pushed)
	{
		TxnMemtableVersionChange pushed = {.writer = change->writer, .op = &change->op,
			.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_REPLAYED},
		.has_lsn = true, .lsn = frame->lsn};

		if (!txn_memtable_table_finish(io, prepared, &pushed, frame->has_roomed, frame->roomed, &handle, error))
			return false;
	}
	if ((frame->run == NULL || !frame->run->applied) && !txn_memtable_tuples_finish(io, prepared, row, error))
		return false;
	if (frame->has_flagged && !frame->left_flagged)
	{
		TxnInsertBase flagged_base = frame->base;
		txn_insert_TakeStep_t filled = {.record = step.record, .base = &flagged_base, .room = &frame->room,
		.block = frame->flagged};

		for (;;)
		{
			TxnMemtableMapRead flagged = txn_insert_Seen_read(&frame->flagged_seen, &flagged_base);

			if (!txn_memtable_map_finish_Unit(io, prepared, frame->takes.len - 1, change->writer, frame->has_roomed,
											  frame->roomed, true, frame->lsn, frame->flagged_seen.has_below,
											  frame->flagged_seen.below.ptr, frame->flagged_seen.below.len, &flagged,
											  txn_insert_filled, &filled, NULL, txn_insert_nothing, NULL, NULL, &taken,
											  error))
				return false;
			if (taken != TXN_MEMTABLE_MAP_TAKEN_MOVED)
				break;
			txn_insert_Seen_drop(&frame->flagged_seen);
			if (!txn_insert_read_base(frame->io, frame->held, &flagged_base, error) ||
				!txn_insert_seen(frame->io, frame->store, step.record, &flagged_base, &frame->room, frame->flagged,
								 &frame->flagged_seen, error))
				return false;
		}
		if (taken == TXN_MEMTABLE_MAP_TAKEN_RETRY)
		{
			std_string_from_Checked(&message, "a free space map's entry is corrupt", 35);
			*error = host_Error_fork((const uint8 *) "XX001", &message);
			return false;
		}
	}
	return true;
}

/* txn_insert_redo_body reads what the replayed row needs, leaves fresh pages until its block, and runs its prepare. */
static void
txn_insert_redo_body(void *frame_)
{
	txn_insert_redo_frame_t *frame = frame_;
	const TxnRecordRowChange *change = frame->change;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	TxnMemtableVersionShown replayed = {.tag = TXN_MEMTABLE_VERSION_SHOWN_REPLAYED};
	const TxnMemtablePut *put = NULL;
	uint64		top = change->writer.has_top ? change->writer.top : change->writer.xid;
	StdString	message;
	TxnMemtableMapTake take;
	txn_TupleRefSliceU8Usize_t row;

	if (!txn_status_redo_roomed(frame->io, &change->writer, &frame->has_roomed, &frame->roomed, frame->error))
		return;
	if (!txn_record_RowChange_put(change, &put))
	{
		std_string_from_Checked(&message, "a replayed insert carries no row", 32);
		*frame->error = host_Error_fork((const uint8 *) "XX000", &message);
		return;
	}
	frame->tid = put->tuple.f0;
	std_vec_new(&frame->stored, TXN_INSERT_ID);
	frame->stored_live = true;
	for (size_t place = 0; place < put->values.len; place++)
		if (!txn_insert_contains(&change->kept, place))
			std_vec_push_Checked(&frame->stored, &STD_VEC_AT(&put->values, uint32, place), TXN_INSERT_ID);
	std_vec_new(&frame->kept, TXN_INSERT_REPLAYED_KEPT);
	frame->kept_live = true;
	for (size_t i = 0; frame->toast != NULL && i < change->kept.len; i++)
	{
		size_t		place = STD_VEC_AT(&change->kept, size_t, i);
		txn_insert_TupleU32OptionVecU8_t kept;
		std_TupleU64U64_t mark;
		bool		holds;

		if (place >= put->values.len)
		{
			std_string_from_Checked(&message, "a kept value's place is past the row's", 38);
			*frame->error = host_Error_fork((const uint8 *) "XX001", &message);
			return;
		}
		kept.f0 = STD_VEC_AT(&put->values, uint32, place);
		if (!txn_memtable_map_holds(frame->io, host_shared_Held_record(frame->toast), TXN_MEMTABLE_MAP_MAP_VALUES,
									kept.f0, frame->lsn, &holds, frame->error))
			return;
		if (holds)
			continue;
		if (!txn_values_based(frame->io, frame->store, frame->toast, kept.f0, &kept.has_f1, &kept.f1, &mark,
							  frame->error))
			return;
		std_vec_push_Checked(&frame->kept, &kept, TXN_INSERT_REPLAYED_KEPT);
	}
	if (!txn_insert_run_out(frame->io, frame->run, frame->held, TXN_MEMTABLE_MAP_MAP_FREE, (uint32) (frame->tid >> 16),
							&frame->left, frame->error))
		return;
	if (change->has_replaced && change->replaced.f0 >> 16 != frame->tid >> 16 &&
		!txn_insert_run_out(frame->io, frame->run, frame->held, TXN_MEMTABLE_MAP_MAP_FREE,
							(uint32) (change->replaced.f0 >> 16), &frame->left_flagged, frame->error))
		return;
	std_vec_new(&frame->outs, sizeof(bool), _Alignof(bool));
	frame->outs_live = true;
	for (size_t i = 0; frame->toast != NULL && i < frame->stored.len + frame->kept.len; i++)
	{
		uint32		id = i < frame->stored.len ? STD_VEC_AT(&frame->stored, uint32, i) :
			STD_VEC_AT(&frame->kept, txn_insert_TupleU32OptionVecU8_t, i - frame->stored.len).f0;
		bool		out;

		if (!txn_insert_run_out(frame->io, frame->run, frame->toast, TXN_MEMTABLE_MAP_MAP_VALUES, id, &out,
								frame->error))
			return;
		std_vec_push_Checked(&frame->outs, &out, sizeof(bool), _Alignof(bool));
	}
	frame->need = txn_memtable_version_encoded(&change->writer, &change->op, replayed, true, frame->lsn);
	if (!txn_memtable_table_holds(frame->io, record, change->key.ptr, change->key.len, frame->lsn, &frame->pushed,
								  frame->error))
		return;
	if (!frame->pushed)
	{
		uint64		bytes;
		bool		spilled;

		if (!txn_spill_replayed(frame->io, &change->storage, TXN_WRITTEN_OF_ROWS, top, frame->need, &bytes,
								frame->error))
			return;
		if (bytes >= txn_spill_threshold() &&
			!txn_spill_spilled(frame->io, &change->storage, TXN_WRITTEN_OF_ROWS, top, &spilled, frame->error))
			return;
	}
	if (!frame->pushed)
	{
		size_t		frees = 1 + (change->has_replaced && change->replaced.f0 >> 16 != frame->tid >> 16);
		size_t		freeing = frees * (txn_free_space_most_len(frame->room.most) + 4);
		uint64		bytes;
		bool		spilled;

		if (!txn_spill_replayed(frame->io, &change->storage, TXN_WRITTEN_OF_FREE, top, freeing, &bytes,
								frame->error))
			return;
		if (bytes >= txn_spill_threshold() &&
			!txn_spill_spilled(frame->io, &change->storage, TXN_WRITTEN_OF_FREE, top, &spilled, frame->error))
			return;
	}
	frame->pushed = frame->pushed || (frame->run != NULL && frame->run->applied);
	for (;;)
	{
		uint32		block;
		bool		marked;

		if (!txn_insert_read_base(frame->io, frame->held, &frame->base, frame->error))
			return;
		frame->counter = txn_state_counter(record);
		if (frame->tid >> 16 > frame->counter >> 16)
		{
			if (!txn_insert_leave_fresh(frame->io, frame->store, frame->held, change->writer, frame->lsn, frame->counter,
										&frame->room, frame->error))
				return;
			continue;
		}
		block = (uint32) (frame->tid >> 16);
		frame->mark = txn_values_mark(record);
		if (!txn_insert_seen(frame->io, frame->store, record, &frame->base, &frame->room, block, &frame->seen,
							 frame->error) ||
			!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-redo-take-seen", frame->error))
			return;
		std_vec_new(&frame->takes, TXN_INSERT_TAKE);
		frame->takes_live = true;
		take = txn_insert_turn_take(record, TXN_MEMTABLE_MAP_MAP_FREE, block,
									txn_free_space_most_len(frame->room.most));
		std_vec_push_Checked(&frame->takes, &take, TXN_INSERT_TAKE);
		if (frame->toast != NULL)
		{
			HostSharedRecord toast = host_shared_Held_record(frame->toast);

			for (size_t i = 0; i < frame->stored.len + frame->kept.len; i++)
			{
				uint32		id = i < frame->stored.len ? STD_VEC_AT(&frame->stored, uint32, i) :
					STD_VEC_AT(&frame->kept, txn_insert_TupleU32OptionVecU8_t, i - frame->stored.len).f0;

				take = txn_insert_turn_take(toast, TXN_MEMTABLE_MAP_MAP_VALUES, id, TXN_VALUES_MOST);
				std_vec_push_Checked(&frame->takes, &take, TXN_INSERT_TAKE);
			}
		}
		if (!txn_memtable_map_marks(frame->io, record, block, put->tuple, frame->lsn, &marked, frame->error))
			return;
		if (marked)
			std_vec_new(&frame->marks, TXN_INSERT_ID);
		else
			std_slice_to_vec_Checked(&frame->marks, put->values.ptr, put->values.len, TXN_INSERT_ID);
		frame->marks_live = true;
		frame->marking = frame->takes.len;
		if (frame->marks.len > 0)
		{
			take = txn_insert_turn_take(record, TXN_MEMTABLE_MAP_MAP_FREE, block,
										txn_memtable_map_named_most(frame->marks.len));
			std_vec_push_Checked(&frame->takes, &take, TXN_INSERT_TAKE);
		}
		if (change->has_replaced && (uint32) (change->replaced.f0 >> 16) != block)
		{
			frame->flagged = (uint32) (change->replaced.f0 >> 16);
			take = txn_insert_turn_take(record, TXN_MEMTABLE_MAP_MAP_FREE, frame->flagged,
										txn_free_space_most_len(frame->room.most));
			std_vec_push_Checked(&frame->takes, &take, TXN_INSERT_TAKE);
			if (!txn_insert_seen(frame->io, frame->store, record, &frame->base, &frame->room, frame->flagged,
								 &frame->flagged_seen, frame->error))
				return;
			frame->has_flagged = true;
		}
		row.f0.ptr = change->key.ptr;
		row.f0.len = change->key.len;
		row.f1 = frame->need;
		if (!txn_memtable_table_prepare_Unit(frame->io, record, &row, 1, frame->takes.ptr, frame->takes.len, true,
											 txn_insert_redo_closure_9, frame, NULL, frame->error))
			return;
		if (frame->done)
		{
			frame->ok = true;
			return;
		}
		if (frame->has_flagged)
			txn_insert_Seen_drop(&frame->flagged_seen);
		frame->has_flagged = false;
		std_vec_free_Checked(&frame->marks, TXN_INSERT_ID);
		frame->marks_live = false;
		std_vec_free_Checked(&frame->takes, TXN_INSERT_TAKE);
		frame->takes_live = false;
		txn_insert_Seen_drop(&frame->seen);
	}
}

/* txn_insert_redo_pad frees what redo holds live. */
static void
txn_insert_redo_pad(void *frame_)
{
	txn_insert_redo_frame_t *frame = frame_;

	if (frame->outs_live)
		std_vec_free_Checked(&frame->outs, sizeof(bool), _Alignof(bool));
	frame->outs_live = false;
	if (frame->has_flagged)
		txn_insert_Seen_drop(&frame->flagged_seen);
	if (frame->marks_live)
		std_vec_free_Checked(&frame->marks, TXN_INSERT_ID);
	if (frame->takes_live)
		std_vec_free_Checked(&frame->takes, TXN_INSERT_TAKE);
	txn_insert_Seen_drop(&frame->seen);
	if (frame->kept_live)
	{
		for (size_t i = 0; i < frame->kept.len; i++)
		{
			txn_insert_TupleU32OptionVecU8_t *kept = &STD_VEC_AT(&frame->kept, txn_insert_TupleU32OptionVecU8_t, i);

			if (kept->has_f1)
				std_vec_free_Checked(&kept->f1, 1, 1);
		}
		std_vec_free_Checked(&frame->kept, TXN_INSERT_REPLAYED_KEPT);
	}
	if (frame->stored_live)
		std_vec_free_Checked(&frame->stored, TXN_INSERT_ID);
	frame->has_flagged = frame->marks_live = frame->takes_live = frame->kept_live = frame->stored_live = false;
}

/*
 * txn_insert_redo applies a replayed insert's row change to its storage, in
 * the startup process or on a standby, with no catalog access: it takes the
 * row's place out of the free space map as the primary's take did, by the
 * record's tuple id and heap bytes, pushes the row's version with the
 * record's LSN, and stores the tuple id table's entry. A tuple id past the
 * fresh page leaves that page for the next, and the counter moves past every
 * tuple id replayed. A take applies only when its offset is free in its
 * block's entry, or at or past the counter's offset on the counter's block,
 * so a record redone again takes nothing twice. A row with out-of-line
 * values has each value's map entry written unless the map holds the id
 * already. A row version the checkpoint's runs brought back isn't pushed
 * again, while the record's map changes and its tuple id's slot apply either
 * way. The entries a take falls back to are read before its prepare, which
 * may spill at the delta's budget or meet a fold's install; a window that
 * finds the state's mark moved since writes nothing and reads them again, so
 * no take compares against an entry a spill moved out of the memtable. Past a
 * base its record ends at or before, each take its key's run holds is left
 * out, the counter still moving, and a row the base applied is neither pushed
 * nor its slot stored (bugs row 401).
 */
bool
txn_insert_redo(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const TxnRecordRowChange *change,
				uint64 lsn, const TxnInsertPage *page, const TxnInsertRun *run, const HostSharedHeld *toast,
				const TxnValuesEntry *toast_entries, size_t toast_entries_len, HostError *error)
{
	txn_insert_redo_frame_t frame = {.io = io, .store = store, .held = held, .change = change, .lsn = lsn, .run = run,
		.toast = toast, .entries = toast_entries, .entries_len = toast_entries_len,
	.room = {.bytes = change->heap_bytes, .page = page->bytes, .most = page->most}, .error = error};

	std_landing(&frame, txn_insert_redo_body, txn_insert_redo_pad);
	txn_insert_redo_pad(&frame);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/chunks/trailer.h"
#include "doltgres/repo/lib.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/txn/blobs.h"
#include "doltgres/txn/change.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/runs.h"
#include "doltgres/txn/values.h"
#include "doltgres/txn/visible.h"

/*
 * txn_insert_tests_taken inserts a row as a test does, taking its key's
 * stripe with a test's Waits and no resolve, and returns its tuple id.
 */
bool
txn_insert_tests_taken(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, TxnInsertInsert *row,
					   const TxnInsertRoom *room, const HostSharedHeld *toast, const TxnValuesEntry *toast_entries,
					   size_t toast_entries_len, uint64 *result, HostError *error)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("a test's insert gave "), STD_FMT_ARG(0)};
	HostIoWaits waits = host_io_Waits_testing();
	TxnInsertKeyed keyed = {.stripe = txn_change_stripe(&row->storage, row->key.ptr, row->key.len, true)};
	TxnInsertInserted inserted;
	TxnInsertInsertedRef other = {&inserted, {NULL, std_unit_Debug_fmt}};
	StdFmtArg	args[] = {{&other, txn_insert_Inserted_Debug_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	StdString	message;

	if (!txn_insert_insert(io, store, held, row, room, NULL, toast, toast_entries, toast_entries_len, keyed, &waits,
						   &inserted, error))
		return false;
	if (inserted.tag == TXN_INSERT_INSERTED_TOOK)
	{
		*result = inserted.u.took.tuple;
		return true;
	}
	std_fmt_format_Checked(&message, &arguments);
	*error = host_Error_fork((const uint8 *) "XX000", &message);
	return false;
}

/* txn_insert_tests_stored returns a test's storage in pg_default, by its database. */
HostStorage
txn_insert_tests_stored(uint32 database)
{
	return (HostStorage)
	{
		.tablespace = HOST_PG_DEFAULT, .database = database, .relation = 16384
	};
}

/* txn_insert_tests_row returns a keyed row's insert by its key's number, written by a transaction's first command. */
TxnInsertInsert
txn_insert_tests_row(HostStorage storage, uint64 n)
{
	TxnInsertInsert row = {.storage = storage, .table = 16385, .put = {.heap_bytes = 32}, .writer = {.xid = 41},
	.logged = true};
	uint8		key[8];

	std_u64_to_be_bytes(n, key);
	std_slice_to_vec_Checked(&row.key, key, sizeof(key), 1, 1);
	std_slice_to_vec_Checked(&row.put.value, (const uint8 *) "row", 3, 1, 1);
	std_vec_new(&row.put.addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_vec_new(&row.put.values, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&row.kept, sizeof(size_t), _Alignof(size_t));
	return row;
}

/* TXN_INSERT_TESTS_UNWRAP_FAILED is a Result's unwrap's panic on a HostError at a line and column. */
#define TXN_INSERT_TESTS_UNWRAP_FAILED(line, col, error) \
	STD_RESULT_EXPECT_FAILED((line), (col), "called `Result::unwrap()` on an `Err` value", (error), \
							 host_Error_Debug_fmt)

/* TXN_INSERT_TESTS_PAGE is an 8 kB heap page as redo knows it: its bytes past its header and its most tuples. */
static const TxnInsertPage TXN_INSERT_TESTS_PAGE = {.bytes = 8168, .most = 291};

/* txn_insert_tests_room returns what a row of some heap bytes needs of an 8 kB heap page, with no fill factor's reserve. */
static TxnInsertRoom
txn_insert_tests_room(uint64 bytes)
{
	TxnInsertRoom room = {.bytes = bytes, .page = 8168, .most = 291};

	return room;
}

/* txn_insert_tests_repository makes a test's repository of a database and a reader of the shared state, unwrapped at the lines a case gives. */
static void
txn_insert_tests_repository(HostIoIo *io, uint32 database, uint32 create_line, uint32 create_col, uint32 read_line,
							uint32 read_col, RepoRepo *repo, HostSharedReader *reading)
{
	ChunksErrorError failed;
	HostError	error;

	if (!repo_Repo_create(io, database, HOST_PG_DEFAULT, repo, &failed))
		STD_RESULT_EXPECT_FAILED(create_line, create_col, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	if (!host_shared_read(io, reading, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(read_line, read_col, &error);
}

/* txn_insert_tests_stated returns the state of a storage, unwrapped at a line and column. */
static HostSharedHeld
txn_insert_tests_stated(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, HostStorage storage,
						uint32 line, uint32 col)
{
	HostSharedHeld held;
	HostError	error;

	if (!txn_state_state(io, repo, reading, &storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(line, col, &error);
	return held;
}

/* txn_insert_tests_took inserts a row as taken does, unwrapped at a line and column, and returns its tuple id. */
static uint64
txn_insert_tests_took(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, TxnInsertInsert row,
					  uint64 bytes, const HostSharedHeld *toast, const TxnValuesEntry *entries, size_t entries_len,
					  uint32 line, uint32 col)
{
	TxnInsertRoom room = txn_insert_tests_room(bytes);
	HostError	error;
	uint64		tid;

	if (!txn_insert_tests_taken(io, store, held, &row, &room, toast, entries, entries_len, &tid, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(line, col, &error);
	return tid;
}

/* txn_insert_tests_ended ends the test's transaction as its commit's callbacks do, unwrapped at a line and column. */
static void
txn_insert_tests_ended(HostIoIo *io, uint32 line, uint32 col)
{
	HostError	error;

	if (!txn_written_ended(io, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(line, col, &error);
}

/* txn_insert_tests_inserted returns the tuple ids a new storage's inserts of a row's heap bytes take, one row a key. */
static StdVec
txn_insert_tests_inserted(HostIoIo *io, uint32 database, uint64 bytes, uint64 rows)
{
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TreesStoreReader store;
	StdVec		tids;

	txn_insert_tests_repository(io, database, 1740, 91, 1741, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(database), 1742, 110);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	std_vec_with_capacity_Checked(&tids, rows, sizeof(uint64), _Alignof(uint64));
	for (uint64 n = 0; n < rows; n++)
	{
		uint64		tid = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held,
												txn_insert_tests_row(txn_insert_tests_stored(database), n), bytes,
												NULL, NULL, 0, 1745, 91);

		std_vec_push_Checked(&tids, &tid, sizeof(tid), _Alignof(uint64));
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return tids;
}

/* txn_insert_tests_u64s_is asserts at a line and column that some u64s are those wanted, as slices compare. */
static void
txn_insert_tests_u64s_is(uint32 line, uint32 col, const uint64 *got, size_t got_len, const uint64 *want,
						 size_t want_len)
{
	StdVec		left = {(void *) got, got_len, got_len};
	StdVec		right = {(void *) want, want_len, want_len};

	STD_ASSERT_EQ(line, col, got_len == want_len && memcmp(got, want, got_len * sizeof(uint64)) == 0, &left,
				  chunks_trailer_VecU64_Debug_fmt, &right, chunks_trailer_VecU64_Debug_fmt);
}

/*
 * PLAN item 2's allocation on a new table, heap's packing: rows of 28 heap
 * bytes take (0,1) up to (0,291), a page's most tuples, then (1,1); rows of
 * 240 heap bytes fill a page by its bytes at its 34th row, and the 35th goes
 * to (1,1).
 */
void
test_txn_insert_tests_inserts_take_heaps_tuple_ids_by_offsets_and_by_bytes(void)
{
	static const uint64 first[] = {1, 2};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdVec		narrow = txn_insert_tests_inserted(io, 9301, 28, 292);
	StdVec		wide;

	txn_insert_tests_u64s_is(1756, 9, narrow.ptr, 2, first, lengthof(first));
	STD_ASSERT_EQ_U64(1757, 9, STD_VEC_AT(&narrow, uint64, 290), 291);
	STD_ASSERT_EQ_U64(1758, 9, STD_VEC_AT(&narrow, uint64, 291), 1 << 16 | 1);
	wide = txn_insert_tests_inserted(io, 9302, 240, 35);
	STD_ASSERT_EQ_U64(1760, 9, STD_VEC_AT(&wide, uint64, 33), 34);
	STD_ASSERT_EQ_U64(1761, 9, STD_VEC_AT(&wide, uint64, 34), 1 << 16 | 1);
	txn_insert_tests_ended(io, 1762, 35);
	std_vec_free_Checked(&wide, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&narrow, sizeof(uint64), _Alignof(uint64));
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_last_page_t is the last page case's captures: its storage, its state and store, the rows put, and the block the last took. */
typedef struct txn_insert_tests_last_page_t
{
	HostStorage storage;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	uint64		n;
	bool		has_target;
	uint32		target;
} txn_insert_tests_last_page_t;

/* txn_insert_tests_last_page_put is the last page case's put: the next key's row of some heap bytes, at the block the last row took. */
static uint64
txn_insert_tests_last_page_put(txn_insert_tests_last_page_t *c, HostIoIo *io, uint64 bytes)
{
	TxnInsertInsert row;
	uint64		tid;

	c->n++;
	row = txn_insert_tests_row(c->storage, c->n);
	row.has_target = c->has_target;
	row.target = c->target;
	tid = txn_insert_tests_took(io, c->store, c->held, row, bytes, NULL, NULL, 0, 1781, 93);
	c->has_target = true;
	c->target = (uint32) (tid >> 16);
	return tid;
}

/*
 * PLAN item 2's allocation in RelationGetBufferForTuple's order, with a fresh
 * page left by its bytes handing its counter offsets to the map: rows of 3000
 * heap bytes take (0,1), (0,2), then (1,1), (1,2), (2,1); rows that fit the
 * last page go there, (2,2) and (2,3); and a row the last page can't hold
 * goes to the lowest page whose room the map shows, the page left first, at
 * its first counter offset, (0,3).
 */
void
test_txn_insert_tests_a_row_the_last_page_cant_hold_goes_to_the_lowest_page_left_with_room(void)
{
	static const uint64 sizes[] = {3000, 3000, 3000, 3000, 3000, 2000, 3000, 2000};
	static const uint64 want[] = {1, 2, 1 << 16 | 1, 1 << 16 | 2, 2 << 16 | 1, 2 << 16 | 2, 2 << 16 | 3, 3};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TreesStoreReader store;
	txn_insert_tests_last_page_t c = {.storage = txn_insert_tests_stored(9303)};
	uint64		taken[lengthof(sizes)];

	txn_insert_tests_repository(io, 9303, 1773, 87, 1774, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, c.storage, 1775, 95);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	c.store = TREES_STORE_READER_NODE_STORE(&store);
	c.held = &held;
	for (size_t k = 0; k < lengthof(sizes); k++)
		taken[k] = txn_insert_tests_last_page_put(&c, io, sizes[k]);
	txn_insert_tests_u64s_is(1787, 9, taken, lengthof(taken), want, lengthof(want));
	txn_insert_tests_ended(io, 1788, 35);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_logged_tag_t is Logged's variants. */
typedef enum txn_insert_tests_logged_tag_t
{
	/* A row change with its record's end. */
	TXN_INSERT_TESTS_LOGGED_ROW,
	/* A fresh page's leave. */
	TXN_INSERT_TESTS_LOGGED_LEFT,
	/* A chunk with its record's end. */
	TXN_INSERT_TESTS_LOGGED_CHUNK,
} txn_insert_tests_logged_tag_t;

/*
 * txn_insert_tests_logged_t is Logged: one of a database's logged writes, as
 * redo reads it back, a row change with its record's end, a fresh page's
 * leave, or a chunk with its record's end.
 */
typedef struct txn_insert_tests_logged_t
{
	txn_insert_tests_logged_tag_t tag;
	union
	{
		TxnRecordRowChange row;
		TxnRecordPageLeft left;
		TxnRecordChunkWrite chunk;
	}			u;
	HostLsn		end;
} txn_insert_tests_logged_t;

/* txn_insert_tests_logged_drop drops a logged write. */
static void
txn_insert_tests_logged_drop(txn_insert_tests_logged_t *write)
{
	if (write->tag == TXN_INSERT_TESTS_LOGGED_ROW)
		txn_record_RowChange_drop(&write->u.row);
	else if (write->tag == TXN_INSERT_TESTS_LOGGED_CHUNK)
		txn_record_ChunkWrite_drop(&write->u.chunk);
}

/* txn_insert_tests_logged_free frees a Vec<Logged> and its writes. */
static void
txn_insert_tests_logged_free(StdVec *writes)
{
	for (size_t k = 0; k < writes->len; k++)
		txn_insert_tests_logged_drop(&STD_VEC_AT(writes, txn_insert_tests_logged_t, k));
	std_vec_free_Checked(writes, sizeof(txn_insert_tests_logged_t), _Alignof(txn_insert_tests_logged_t));
}

/* txn_insert_tests_logged returns the writes the simulation logged for a database's storages and its repository, in their order. */
static StdVec
txn_insert_tests_logged(uint32 database)
{
	StdVec		logged = simc_logged();
	StdVec		writes;

	std_vec_new(&writes, sizeof(txn_insert_tests_logged_t), _Alignof(txn_insert_tests_logged_t));
	for (size_t k = 0; k < logged.len; k++)
	{
		simc_TupleU8VecU8Lsn_t *one = &STD_VEC_AT(&logged, simc_TupleU8VecU8Lsn_t, k);
		txn_insert_tests_logged_t write = {.end = one->f2};
		HostWalKind kind;
		HostError	error;
		bool		kept = false;

		if (!host_WalKind_of(one->f0, &kind))
			kind = HOST_WAL_KIND_REPOSITORY_CREATED;
		if (kind == HOST_WAL_KIND_ROW_CHANGE)
		{
			write.tag = TXN_INSERT_TESTS_LOGGED_ROW;
			if (!txn_record_RowChange_decode(one->f1.ptr, one->f1.len, &write.u.row, &error))
				host_Error_drop(&error);
			else if (!(kept = write.u.row.storage.database == database))
				txn_record_RowChange_drop(&write.u.row);
		}
		else if (kind == HOST_WAL_KIND_PAGE_LEFT)
		{
			write.tag = TXN_INSERT_TESTS_LOGGED_LEFT;
			if (!txn_record_PageLeft_decode(one->f1.ptr, one->f1.len, &write.u.left, &error))
				host_Error_drop(&error);
			else
				kept = write.u.left.storage.database == database;
		}
		else if (kind == HOST_WAL_KIND_CHUNK)
		{
			write.tag = TXN_INSERT_TESTS_LOGGED_CHUNK;
			if (!txn_record_ChunkWrite_decode(one->f1.ptr, one->f1.len, &write.u.chunk, &error))
				host_Error_drop(&error);
			else if (!(kept = write.u.chunk.repository == database))
				txn_record_ChunkWrite_drop(&write.u.chunk);
		}
		if (kept)
			std_vec_push_Checked(&writes, &write, sizeof(write), _Alignof(txn_insert_tests_logged_t));
		std_vec_free_Checked(&one->f1, 1, 1);
	}
	std_vec_free_Checked(&logged, sizeof(simc_TupleU8VecU8Lsn_t), _Alignof(simc_TupleU8VecU8Lsn_t));
	return writes;
}

/* txn_insert_tests_count returns how many of some logged writes are of a variant. */
static size_t
txn_insert_tests_count(const StdVec *writes, txn_insert_tests_logged_tag_t tag)
{
	size_t		count = 0;

	for (size_t k = 0; k < writes->len; k++)
		count += STD_VEC_AT(writes, txn_insert_tests_logged_t, k).tag == tag;
	return count;
}

/* txn_insert_tests_mapped_t is an Option<Option<Vec<u8>>>: a key's newest map entry, a removal's none, or none. */
typedef struct txn_insert_tests_mapped_t
{
	bool		some;
	TxnMemtableMapEntry entry;
} txn_insert_tests_mapped_t;

/* txn_insert_tests_mapped_Debug_fmt is an Option<Option<Vec<u8>>>'s Debug. */
static bool
txn_insert_tests_mapped_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_insert_tests_mapped_t *mapped = value;
	StdBytes	bytes = {mapped->entry.value.ptr, mapped->entry.value.len};
	StdOptionRef inner = {mapped->some && mapped->entry.has_value, {&bytes, std_bytes_Debug_fmt}};
	StdOptionRef outer = {mapped->some, {&inner, std_option_Debug_fmt}};

	return std_option_Debug_fmt(&outer, f);
}

/* txn_insert_tests_mapped_eq reports whether two Option<Option<Vec<u8>>>s are equal. */
static bool
txn_insert_tests_mapped_eq(const txn_insert_tests_mapped_t *a, const txn_insert_tests_mapped_t *b)
{
	return a->some == b->some &&
		(!a->some || (a->entry.has_value == b->entry.has_value &&
					  (!a->entry.has_value ||
					   trees_store_VecU8_PartialEq_eq(&a->entry.value, &b->entry.value))));
}

/* txn_insert_tests_mapped_drop drops a map entry's value. */
static void
txn_insert_tests_mapped_drop(txn_insert_tests_mapped_t *mapped)
{
	if (mapped->some && mapped->entry.has_value)
		std_vec_free_Checked(&mapped->entry.value, 1, 1);
}

/* txn_insert_tests_newest returns a map's newest entry of a key, unwrapped at a line and column. */
static txn_insert_tests_mapped_t
txn_insert_tests_newest(HostIoIo *io, HostSharedRecord record, TxnMemtableMapMap map, uint32 key, uint32 line,
						uint32 col)
{
	txn_insert_tests_mapped_t mapped = {0};
	HostError	error;

	if (!txn_memtable_map_newest(io, record, map, key, &mapped.some, &mapped.entry, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(line, col, &error);
	return mapped;
}

/* txn_insert_tests_free_t is a state's free space map entries of its first four blocks, and its counter. */
typedef struct txn_insert_tests_free_t
{
	txn_insert_tests_mapped_t entries[4];
	uint64		counter;
} txn_insert_tests_free_t;

/* txn_insert_tests_free_entries returns a state's free space map entries of its first four blocks, with its counter. */
static txn_insert_tests_free_t
txn_insert_tests_free_entries(HostIoIo *io, HostSharedRecord record)
{
	txn_insert_tests_free_t free;

	for (uint32 block = 0; block < 4; block++)
		free.entries[block] = txn_insert_tests_newest(io, record, TXN_MEMTABLE_MAP_MAP_FREE, block, 1821, 70);
	free.counter = txn_state_counter(record);
	return free;
}

/* txn_insert_tests_mappeds_Debug_fmt is a Vec<Option<Option<Vec<u8>>>>'s Debug. */
static bool
txn_insert_tests_mappeds_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *mappeds = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < mappeds->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(mappeds, txn_insert_tests_mapped_t, k),
							txn_insert_tests_mapped_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_insert_tests_free_Debug_fmt is the Debug of a state's first four blocks' entries with its counter, the tuple (entries, counter). */
static bool
txn_insert_tests_free_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_insert_tests_free_t *free = value;
	StdVec		entries = {(void *) free->entries, lengthof(free->entries), lengthof(free->entries)};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&entries, txn_insert_tests_mappeds_Debug_fmt),
								  STD_ITEM(&free->counter, std_u64_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_insert_tests_free_drop drops a state's first four blocks' entries. */
static void
txn_insert_tests_free_drop(txn_insert_tests_free_t *free)
{
	for (size_t k = 0; k < lengthof(free->entries); k++)
		txn_insert_tests_mapped_drop(&free->entries[k]);
}

/*
 * txn_insert_tests_free_is asserts at a line and column, with a message or
 * none, that a replica's entries and counter are the primary's.
 */
static void
txn_insert_tests_free_is(uint32 line, uint32 col, const txn_insert_tests_free_t *got,
						 const txn_insert_tests_free_t *want, const StdFmtArguments *message)
{
	bool		eq = got->counter == want->counter;

	for (size_t k = 0; eq && k < lengthof(got->entries); k++)
		eq = txn_insert_tests_mapped_eq(&got->entries[k], &want->entries[k]);
	if (!eq)
		std_assert_failed(RUST_FILE, line, col, "==", got, txn_insert_tests_free_Debug_fmt, want,
						  txn_insert_tests_free_Debug_fmt, message);
}

/*
 * txn_insert_tests_replica_t is Replica: a new database's storage a
 * primary's logged writes were redone on, its state, its TOAST relation's
 * state when a row change named one, and its repository's working state's
 * delta of chunks.
 */
typedef struct txn_insert_tests_replica_t
{
	HostSharedHeld held;
	bool		has_toast;
	HostSharedHeld toast;
	HostSharedHeld deltas;
} txn_insert_tests_replica_t;

/* txn_insert_tests_replica_drop drops a replica's states and its delta of chunks. */
static void
txn_insert_tests_replica_drop(txn_insert_tests_replica_t *replica)
{
	host_shared_Held_drop(&replica->held);
	if (replica->has_toast)
		host_shared_Held_drop(&replica->toast);
	host_shared_Held_drop(&replica->deltas);
}

/*
 * txn_insert_tests_redone redoes a primary's logged writes, in their given
 * order, on a state as the fork's resource manager does: each row change with
 * its TOAST relation's state when it named one, each leave, and each chunk
 * into deltas. It takes the writes.
 */
static void
txn_insert_tests_redone(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
						const HostSharedHeld *toast, const HostSharedHeld *deltas, StdVec *writes)
{
	for (size_t k = 0; k < writes->len; k++)
	{
		txn_insert_tests_logged_t *write = &STD_VEC_AT(writes, txn_insert_tests_logged_t, k);
		HostError	error;

		if (write->tag == TXN_INSERT_TESTS_LOGGED_ROW)
		{
			const TxnRecordRowChange *change = &write->u.row;
			const HostSharedHeld *named = change->has_toast ? toast : NULL;
			StdVec		entries;
			bool		ok;

			std_vec_with_capacity_Checked(&entries, change->blobs.len, sizeof(TxnValuesEntry),
										  _Alignof(TxnValuesEntry));
			for (size_t b = 0; b < change->blobs.len; b++)
			{
				const chunks_TupleAddressU64_t *blob = &STD_VEC_AT(&change->blobs, chunks_TupleAddressU64_t, b);
				TxnValuesEntry entry = txn_values_Entry_new(blob->f0, blob->f1);

				std_vec_push_Checked(&entries, &entry, sizeof(entry), _Alignof(TxnValuesEntry));
			}
			ok = txn_insert_redo(io, store, held, change, write->end, &TXN_INSERT_TESTS_PAGE, NULL, named,
								 named == NULL ? NULL : entries.ptr, named == NULL ? 0 : entries.len, &error);
			std_vec_free_Checked(&entries, sizeof(TxnValuesEntry), _Alignof(TxnValuesEntry));
			if (!ok)
				TXN_INSERT_TESTS_UNWRAP_FAILED(1870, 71, &error);
			txn_record_RowChange_drop(&write->u.row);
		}
		else if (write->tag == TXN_INSERT_TESTS_LOGGED_LEFT)
		{
			if (!txn_insert_redo_left(io, store, held, &write->u.left, write->end, &TXN_INSERT_TESTS_PAGE, &error))
				TXN_INSERT_TESTS_UNWRAP_FAILED(1872, 80, &error);
		}
		else if (!txn_blobs_redo(io, deltas, write->u.chunk, write->end, &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(1873, 82, &error);
	}
	std_vec_free_Checked(writes, sizeof(txn_insert_tests_logged_t), _Alignof(txn_insert_tests_logged_t));
}

/*
 * txn_insert_tests_replica redoes a primary's logged writes, in their given
 * order, on a new database's storage with no catalog, as the fork's resource
 * manager does: each row change with its TOAST relation's state and the
 * value map entries its field 24 gives, each leave, and each chunk into the
 * new repository's working state. It takes the writes.
 */
static txn_insert_tests_replica_t
txn_insert_tests_replica(HostIoIo *io, uint32 database, StdVec *writes)
{
	txn_insert_tests_replica_t replica = {0};
	RepoRepo	repo;
	HostSharedReader reading;
	TreesStoreReader store;
	HostError	error;

	txn_insert_tests_repository(io, database, 1836, 91, 1837, 46, &repo, &reading);
	replica.held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(database), 1838, 110);
	for (size_t k = 0; !replica.has_toast && k < writes->len; k++)
	{
		const txn_insert_tests_logged_t *write = &STD_VEC_AT(writes, txn_insert_tests_logged_t, k);

		if (write->tag == TXN_INSERT_TESTS_LOGGED_ROW && write->u.row.has_toast)
		{
			HostStorage storage = {.tablespace = write->u.row.toast.tablespace, .database = database,
			.relation = write->u.row.toast.relation};

			replica.toast = txn_insert_tests_stated(io, &repo, &reading, storage, 1845, 94);
			replica.has_toast = true;
		}
	}
	if (!txn_memtable_branch_deltas(io, database, HOST_PG_DEFAULT, &replica.deltas, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(1847, 97, &error);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	txn_insert_tests_redone(io, TREES_STORE_READER_NODE_STORE(&store), &replica.held,
							replica.has_toast ? &replica.toast : NULL, &replica.deltas, writes);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return replica;
}

/*
 * txn_insert_tests_replayed redoes a primary's writes on a new database's
 * replica and compares its entries and counter with the primary's, with a
 * message naming the replica when named.
 */
static void
txn_insert_tests_replayed(HostIoIo *io, uint32 database, StdVec *writes, const txn_insert_tests_free_t *expected,
						  bool named, uint32 line, uint32 col)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("replica "), STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&database, std_u32_Display_fmt}};
	StdFmtArguments message = {pieces, lengthof(pieces), args};
	txn_insert_tests_replica_t replica = txn_insert_tests_replica(io, database, writes);
	txn_insert_tests_free_t got;

	if (replica.has_toast)
	{
		host_shared_Held_drop(&replica.toast);
		replica.has_toast = false;
	}
	host_shared_Held_drop(&replica.deltas);
	got = txn_insert_tests_free_entries(io, host_shared_Held_record(&replica.held));
	txn_insert_tests_free_is(line, col, &got, expected, named ? &message : NULL);
	txn_insert_tests_free_drop(&got);
	host_shared_Held_drop(&replica.held);
}

/*
 * CommPF 5's redo cases: the primary's inserts, replayed on another storage
 * from their logged records with no catalog, leave every block's free space
 * entry and the counter as the primary's, the reference placement's case
 * among them, whose last row goes to (0,3) after page 2 is left; in the
 * records' order, and with two row changes logged out of their tuple ids'
 * order, as two inserts' windows allow.
 */
void
test_txn_insert_tests_redo_leaves_the_free_space_map_and_the_counter_as_the_primarys(void)
{
	static const uint64 sizes[] = {3000, 3000, 3000, 3000, 3000, 2000, 3000, 2000};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	primary;
	HostSharedReader reading;
	HostSharedHeld held;
	TreesStoreReader store;
	bool		has_target = false;
	uint32		target = 0;
	txn_insert_tests_free_t expected;
	StdVec		writes;
	StdVec		swapped;

	txn_insert_tests_repository(io, 9304, 1888, 95, 1889, 50, &primary, &reading);
	held = txn_insert_tests_stated(io, &primary, &reading, txn_insert_tests_stored(9304), 1893, 109);
	store = repo_Repo_reader(&primary, HOST_CACHE_PLACEMENT_SERVER, &reading);
	for (uint64 n = 0; n < lengthof(sizes); n++)
	{
		TxnInsertInsert row = txn_insert_tests_row(txn_insert_tests_stored(9304), n);
		uint64		tid;

		row.has_target = has_target;
		row.target = target;
		tid = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, row, sizes[n], NULL, NULL, 0,
									1898, 104);
		has_target = true;
		target = (uint32) (tid >> 16);
	}
	expected = txn_insert_tests_free_entries(io, host_shared_Held_record(&held));
	writes = txn_insert_tests_logged(9304);
	STD_ASSERT_EQ_U64(1902, 9, txn_insert_tests_count(&writes, TXN_INSERT_TESTS_LOGGED_LEFT), 3);
	txn_insert_tests_logged_free(&writes);
	swapped = txn_insert_tests_logged(9304);
	{
		txn_insert_tests_logged_t first = STD_VEC_AT(&swapped, txn_insert_tests_logged_t, 0);

		STD_VEC_AT(&swapped, txn_insert_tests_logged_t, 0) = STD_VEC_AT(&swapped, txn_insert_tests_logged_t, 1);
		STD_VEC_AT(&swapped, txn_insert_tests_logged_t, 1) = first;
	}
	writes = txn_insert_tests_logged(9304);
	txn_insert_tests_replayed(io, 9305, &writes, &expected, true, 1908, 13);
	txn_insert_tests_replayed(io, 9306, &swapped, &expected, true, 1908, 13);
	txn_insert_tests_ended(io, 1910, 35);
	txn_insert_tests_free_drop(&expected);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&primary);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_appended moves the writes of one vector onto the end of another. */
static void
txn_insert_tests_appended(StdVec *to, StdVec *from)
{
	std_vec_extend_from_slice_Checked(to, from->ptr, from->len, sizeof(txn_insert_tests_logged_t),
									  _Alignof(txn_insert_tests_logged_t));
	std_vec_free_Checked(from, sizeof(txn_insert_tests_logged_t), _Alignof(txn_insert_tests_logged_t));
}

/*
 * PLAN's I12 for a free space take: the reference placement's inserts redone
 * with each record twice in a row, and redone whole and then whole again, as
 * a checkpoint's runs that hold a window's take can be followed by its
 * record, leave every block's free space entry and the counter as the
 * primary's, since a take whose offset is no longer free and not past the
 * counter changes nothing.
 */
void
test_txn_insert_tests_a_take_redone_again_changes_nothing(void)
{
	static const uint64 sizes[] = {3000, 3000, 3000, 3000, 3000, 2000, 3000, 2000};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TreesStoreReader store;
	txn_insert_tests_free_t expected;
	StdVec		a;
	StdVec		b;
	StdVec		twice;
	StdVec		again;

	txn_insert_tests_repository(io, 9307, 1920, 87, 1921, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9307), 1922, 106);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	for (uint64 n = 0; n < lengthof(sizes); n++)
		txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held,
							  txn_insert_tests_row(txn_insert_tests_stored(9307), n), sizes[n], NULL, NULL, 0,
							  1926, 73);
	expected = txn_insert_tests_free_entries(io, host_shared_Held_record(&held));
	a = txn_insert_tests_logged(9307);
	b = txn_insert_tests_logged(9307);
	std_vec_with_capacity_Checked(&twice, a.len + b.len, sizeof(txn_insert_tests_logged_t),
								  _Alignof(txn_insert_tests_logged_t));
	for (size_t k = 0; k < a.len && k < b.len; k++)
	{
		std_vec_push_Checked(&twice, &STD_VEC_AT(&a, txn_insert_tests_logged_t, k), sizeof(txn_insert_tests_logged_t),
							 _Alignof(txn_insert_tests_logged_t));
		std_vec_push_Checked(&twice, &STD_VEC_AT(&b, txn_insert_tests_logged_t, k), sizeof(txn_insert_tests_logged_t),
							 _Alignof(txn_insert_tests_logged_t));
	}
	std_vec_free_Checked(&a, sizeof(txn_insert_tests_logged_t), _Alignof(txn_insert_tests_logged_t));
	std_vec_free_Checked(&b, sizeof(txn_insert_tests_logged_t), _Alignof(txn_insert_tests_logged_t));
	again = txn_insert_tests_logged(9307);
	b = txn_insert_tests_logged(9307);
	txn_insert_tests_appended(&again, &b);
	txn_insert_tests_replayed(io, 9308, &twice, &expected, true, 1934, 13);
	txn_insert_tests_replayed(io, 9309, &again, &expected, true, 1934, 13);
	txn_insert_tests_ended(io, 1936, 35);
	txn_insert_tests_free_drop(&expected);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_toasted returns the state of a test's TOAST relation 16390 in a database, unwrapped at a line and column. */
static HostSharedHeld
txn_insert_tests_toasted(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, uint32 database,
						 uint32 line, uint32 col)
{
	HostStorage storage = txn_insert_tests_stored(database);

	storage.relation = 16390;
	return txn_insert_tests_stated(io, repo, reading, storage, line, col);
}

/* TXN_INSERT_TESTS_TOAST is the TOAST relation a test's valued rows name: pg_default's 16390, of table 16391. */
static const TxnRecordToast TXN_INSERT_TESTS_TOAST = {.tablespace = HOST_PG_DEFAULT, .relation = 16390, .table = 16391};

/* TXN_INSERT_TESTS_IDS is the value ids a test's valued rows name. */
static const uint32 TXN_INSERT_TESTS_IDS[] = {16400, 16401};

/* txn_insert_tests_values sets an insert's value ids to the test's two, naming its TOAST relation. */
static void
txn_insert_tests_values(TxnInsertInsert *row)
{
	std_vec_free_Checked(&row->put.values, sizeof(uint32), _Alignof(uint32));
	std_slice_to_vec_Checked(&row->put.values, TXN_INSERT_TESTS_IDS, lengthof(TXN_INSERT_TESTS_IDS), sizeof(uint32),
							 _Alignof(uint32));
	row->has_toast = true;
	row->toast = TXN_INSERT_TESTS_TOAST;
}

/* txn_insert_tests_row_changes returns pointers to the row changes among some logged writes, in their order. */
static StdVec
txn_insert_tests_row_changes(const StdVec *writes)
{
	StdVec		rows;

	std_vec_new(&rows, sizeof(const TxnRecordRowChange *), _Alignof(const TxnRecordRowChange *));
	for (size_t k = 0; k < writes->len; k++)
	{
		const txn_insert_tests_logged_t *write = &STD_VEC_AT(writes, txn_insert_tests_logged_t, k);
		const TxnRecordRowChange *row = &write->u.row;

		if (write->tag == TXN_INSERT_TESTS_LOGGED_ROW)
			std_vec_push_Checked(&rows, &row, sizeof(row), _Alignof(const TxnRecordRowChange *));
	}
	return rows;
}

/*
 * Rule (C) of the out-of-line values text: an insert of a row with two
 * out-of-line values writes each value's map entry, with a count of one,
 * into its TOAST relation's value map in the row's window, and logs the
 * value ids with their TOAST relation in the row's record (fields 24 to 27).
 */
void
test_txn_insert_tests_an_insert_maps_its_out_of_line_values_in_its_window(void)
{
	static const char *const bytes[] = {"one", "two"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostSharedHeld toast;
	TreesStoreReader store;
	TxnValuesEntry entries[2];
	TxnInsertInsert valued;
	StdVec		writes;
	StdVec		logged;
	StdVec		ids = {(void *) TXN_INSERT_TESTS_IDS, 2, 2};

	txn_insert_tests_repository(io, 9311, 1946, 87, 1947, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9311), 1948, 106);
	toast = txn_insert_tests_toasted(io, &repo, &reading, 9311, 1950, 102);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	for (size_t k = 0; k < lengthof(entries); k++)
	{
		entries[k].address = chunks_address_Address_of((const uint8 *) bytes[k], strlen(bytes[k]));
		entries[k].locator = chunks_trailer_Locator_Default_default();
		entries[k].raw = 4000;
		entries[k].stored = 4000;
		entries[k].count = 1;
	}
	valued = txn_insert_tests_row(txn_insert_tests_stored(9311), 1);
	txn_insert_tests_values(&valued);
	txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, valued, 60, &toast, entries,
						  lengthof(entries), 1963, 75);
	for (size_t k = 0; k < lengthof(entries); k++)
	{
		txn_insert_tests_mapped_t mapped = txn_insert_tests_newest(io, host_shared_Held_record(&toast),
																   TXN_MEMTABLE_MAP_MAP_VALUES,
																   TXN_INSERT_TESTS_IDS[k], 1965, 75);
		TxnValuesEntry entry;
		HostError	error;

		if (!mapped.some)
			STD_OPTION_UNWRAP_FAILED(1965, 84);
		if (!mapped.entry.has_value)
			STD_OPTION_UNWRAP_FAILED(1965, 93);
		if (!txn_values_Entry_decode(mapped.entry.value.ptr, mapped.entry.value.len, &entry, &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(1966, 48, &error);
		STD_ASSERT_EQ(1966, 13, txn_values_Entry_PartialEq_eq(&entry, &entries[k]), &entry,
					  txn_values_Entry_Debug_fmt, &entries[k], txn_values_Entry_Debug_fmt);
		txn_insert_tests_mapped_drop(&mapped);
	}
	writes = txn_insert_tests_logged(9311);
	logged = txn_insert_tests_row_changes(&writes);
	STD_ASSERT_EQ_U64(1974, 9, logged.len, 1);
	{
		const TxnRecordRowChange *change = STD_VEC_AT(&logged, const TxnRecordRowChange *, 0);
		const TxnMemtablePut *put;
		StdOptionRef got = {change->has_toast, {&change->toast, txn_record_Toast_Debug_fmt}};
		StdOptionRef want = {true, {&TXN_INSERT_TESTS_TOAST, txn_record_Toast_Debug_fmt}};

		if (!txn_record_RowChange_put(change, &put))
			STD_OPTION_UNWRAP_FAILED(1975, 36);
		STD_ASSERT_EQ(1975, 9, put->values.len == 2 &&
					  memcmp(put->values.ptr, TXN_INSERT_TESTS_IDS, sizeof(TXN_INSERT_TESTS_IDS)) == 0,
					  &put->values, chunks_header_VecU32_Debug_fmt, &ids, chunks_header_VecU32_Debug_fmt);
		STD_ASSERT_EQ(1976, 9, change->has_toast &&
					  txn_record_Toast_PartialEq_eq(&change->toast, &TXN_INSERT_TESTS_TOAST), &got,
					  std_option_Debug_fmt, &want, std_option_Debug_fmt);
	}
	std_vec_free_Checked(&logged, sizeof(const TxnRecordRowChange *), _Alignof(const TxnRecordRowChange *));
	txn_insert_tests_logged_free(&writes);
	txn_insert_tests_ended(io, 1977, 35);
	host_shared_Held_drop(&toast);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_linked reports whether a state's rows' memtable, free space map, and tuple id table are each linked. */
static void
txn_insert_tests_linked(HostSharedRecord record, bool linked[3])
{
	static const HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

	linked[0] = host_shared_Record_atomic(record, TXN_STATE_MEMTABLE_WORD_INDEX, load) != 0;
	linked[1] = host_shared_Record_atomic(record, TXN_STATE_FREE_WORD_INDEX, load) != 0;
	linked[2] = host_shared_Record_atomic(record, TXN_STATE_TUPLES_WORD_INDEX, load) != 0;
}

/* txn_insert_tests_gathering_fork returns a gathering case's actor's error, the fork's internal one, of a formatted message. */
static HostError
txn_insert_tests_gathering_fork(const StdFmtArguments *arguments)
{
	StdString	message;

	std_fmt_format_Checked(&message, arguments);
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* txn_insert_tests_opened opens a storage's database's repository, a reader, and the storage's state, dropping what it opened on a failure. */
static bool
txn_insert_tests_opened(HostIoIo *io, HostStorage storage, RepoRepo *repo, HostSharedReader *reading, HostSharedHeld *held,
						HostError *error)
{
	if (!txn_state_tests_reopened(io, storage.database, repo, error))
		return false;
	if (!host_shared_read(io, reading, error))
	{
		repo_Repo_drop(repo);
		return false;
	}
	if (!txn_state_state(io, repo, reading, &storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, held, error))
	{
		host_shared_Reader_drop(reading);
		repo_Repo_drop(repo);
		return false;
	}
	return true;
}

/* txn_insert_tests_gathering_inserter is the gathering case's inserter: its row takes (0,1). */
static bool
txn_insert_tests_gathering_inserter(void *captures, HostIoIo *io, HostError *error)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("the insert took "), STD_FMT_ARG(0)};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TxnInsertRoom room = txn_insert_tests_room(28);
	uint64		tid;
	bool		ok;

	if (!txn_insert_tests_opened(io, txn_insert_tests_stored(9312), &repo, &reading, &held, error))
		return false;
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
		TxnInsertInsert row = txn_insert_tests_row(txn_insert_tests_stored(9312), 1);

		ok = txn_insert_tests_taken(io, TREES_STORE_READER_NODE_STORE(&store), &held, &row, &room, NULL, NULL, 0,
									&tid, error);
	}
	if (ok && tid != 1)
	{
		StdFmtArg	args[] = {{&tid, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		*error = txn_insert_tests_gathering_fork(&arguments);
		ok = false;
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/* txn_insert_tests_gathering_between is the gathering case's step between a save's manifest write and its saved, which does nothing. */
static void
txn_insert_tests_gathering_between(void *captures, HostIoIo *io)
{
}

/* txn_insert_tests_found_t is what a gathering found: whether the state's three words are linked, and its counter. */
typedef struct txn_insert_tests_found_t
{
	bool		linked[3];
	uint64		counter;
} txn_insert_tests_found_t;

/* txn_insert_tests_linked_Debug_fmt is a [bool; 3]'s Debug. */
static bool
txn_insert_tests_linked_Debug_fmt(const void *value, StdFormatter *f)
{
	const bool *linked = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < 3; k++)
		std_DebugList_entry(&list, &linked[k], std_bool_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_insert_tests_found_Debug_fmt is what a gathering found's Debug, the tuple ([bool; 3], u64). */
static bool
txn_insert_tests_found_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_insert_tests_found_t *found = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(found->linked, txn_insert_tests_linked_Debug_fmt),
								  STD_ITEM(&found->counter, std_u64_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_insert_tests_gathering_gatherer is one of the gathering case's gatherers: a save's gathering finds the state's words linked and its counter where the insert put it. */
static bool
txn_insert_tests_gathering_gatherer(void *captures, HostIoIo *io, HostError *error)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("a gathering found "), STD_FMT_ARG(0)};
	uint64		counter = *(const uint64 *) captures;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	txn_insert_tests_found_t found;
	bool		ok = true;

	if (!txn_insert_tests_opened(io, txn_insert_tests_stored(9312), &repo, &reading, &held, error))
		return false;
	txn_state_tests_saved(io, &repo, 9312, txn_insert_tests_gathering_between, NULL);
	txn_insert_tests_linked(host_shared_Held_record(&held), found.linked);
	found.counter = txn_state_counter(host_shared_Held_record(&held));
	if (!(found.linked[0] && found.linked[1] && found.linked[2] && found.counter == counter))
	{
		StdFmtArg	args[] = {{&found, txn_insert_tests_found_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		*error = txn_insert_tests_gathering_fork(&arguments);
		ok = false;
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/*
 * PLAN item 7's tuple id counter: a save's gathering held at each point from
 * an insert's prepare to its push, at the prepare's pause before the take, at
 * the window's pause between the row's record and its push, and at the
 * settle's pause after the push, finds the storage's rows' memtable, free
 * space map, and tuple id table linked, and keeps the counter where the
 * insert put it, so the row's tuple id is never handed out again.
 */
void
test_txn_insert_tests_a_gathering_from_an_inserts_prepare_to_its_push_keeps_the_counter(void)
{
	static const uint64 counters[] = {1, 2, 2};
	static const char *const names[] = {"before", "logged", "pushed"};
	static const SimcStep before[] = {{"before", 6, UINT64_MAX}};
	static const SimcStep logged[] = {{"logged", 6, UINT64_MAX}};
	static const SimcStep pushed[] = {{"pushed", 6, UINT64_MAX}, {"inserter", 8, UINT64_MAX}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcActor	inserter = {txn_insert_tests_gathering_inserter};
	RepoRepo	repo;
	ChunksErrorError failed;
	SimcRun		run;
	SimcReport	report;

	if (!repo_Repo_create(io, 9312, HOST_PG_DEFAULT, &repo, &failed))
		STD_RESULT_EXPECT_FAILED(1999, 81, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	repo_Repo_drop(&repo);
	run = simc_Run_new(163);
	simc_Run_actor(&run, "inserter", 8, inserter);
	for (size_t k = 0; k < lengthof(counters); k++)
	{
		SimcActor	gatherer = {txn_insert_tests_gathering_gatherer, (void *) &counters[k]};

		simc_Run_actor(&run, names[k], strlen(names[k]), gatherer);
	}
	simc_Run_until(&run, "inserter", 8, "doltgres-memtable-prepared");
	simc_Run_script(&run, before, lengthof(before));
	simc_Run_until(&run, "inserter", 8, "doltgres-insert-logged");
	simc_Run_script(&run, logged, lengthof(logged));
	simc_Run_until(&run, "inserter", 8, "doltgres-memtable-unpinned");
	simc_Run_script(&run, pushed, lengthof(pushed));
	report = simc_Run_finish(run);
	txn_vacuum_tests_returned(&report, RUST_FILE, 2029, 67, 2030, 9);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_failed_is asserts at a line and column that an insert failed with the fork's internal error, and drops the error. */
static void
txn_insert_tests_failed_is(uint32 line, uint32 col, bool failed, HostError *error)
{
	bool		internal = failed && memcmp(host_Error_sqlstate(error), "XX000", 5) == 0;

	if (failed)
		host_Error_drop(error);
	STD_ASSERT(line, col, internal, "failed.is_err_and(|error| error.sqlstate() == b\"XX000\")");
}

/*
 * The leave case: an insert that leaves the fresh page and then fails before
 * its row's record, a crash, and redo of the logged records on another
 * storage leave every block's free space entry and the counter as the
 * primary's, since the leave's record hands the page's counter offsets to the
 * map and the failed turn logged nothing.
 */
void
test_txn_insert_tests_an_insert_that_leaves_the_fresh_page_and_fails_redoes_as_the_primary(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TreesStoreReader store;
	TxnInsertRoom room = txn_insert_tests_room(3000);
	TxnInsertInsert row;
	HostError	error;
	uint64		tid;
	bool		failed;
	txn_insert_tests_free_t expected;
	StdVec		writes;

	txn_insert_tests_repository(io, 9313, 2041, 87, 2042, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9313), 2043, 106);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	for (uint64 n = 0; n < 2; n++)
		txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held,
							  txn_insert_tests_row(txn_insert_tests_stored(9313), n), 3000, NULL, NULL, 0,
							  2047, 73);
	simc_refuse_any_take(1);
	row = txn_insert_tests_row(txn_insert_tests_stored(9313), 2);
	failed = !txn_insert_tests_taken(io, TREES_STORE_READER_NODE_STORE(&store), &held, &row, &room, NULL, NULL, 0,
									 &tid, &error);
	simc_refuse_nothing();
	txn_insert_tests_failed_is(2052, 9, failed, &error);
	writes = txn_insert_tests_logged(9313);
	STD_ASSERT_EQ_U64(2054, 9, txn_insert_tests_count(&writes, TXN_INSERT_TESTS_LOGGED_ROW), 2);
	STD_ASSERT_EQ_U64(2055, 9, txn_insert_tests_count(&writes, TXN_INSERT_TESTS_LOGGED_LEFT), 1);
	expected = txn_insert_tests_free_entries(io, host_shared_Held_record(&held));
	STD_ASSERT_EQ_U64(2057, 9, expected.counter, 1 << 16 | 1);
	txn_insert_tests_replayed(io, 9314, &writes, &expected, false, 2059, 9);
	txn_insert_tests_ended(io, 2060, 35);
	txn_insert_tests_free_drop(&expected);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_mapped returns a TOAST relation's value map entries for the test's two value ids. */
static void
txn_insert_tests_mapped(HostIoIo *io, const HostSharedHeld *toast, txn_insert_tests_mapped_t mapped[2])
{
	for (size_t k = 0; k < lengthof(TXN_INSERT_TESTS_IDS); k++)
		mapped[k] = txn_insert_tests_newest(io, host_shared_Held_record(toast), TXN_MEMTABLE_MAP_MAP_VALUES,
											TXN_INSERT_TESTS_IDS[k], 2065, 79);
}

/*
 * txn_insert_tests_valued returns an insert of a row with two out-of-line
 * values whose references sit in an overflow chunk, so the row's own bytes
 * hold only that chunk's address, with each value's map entry.
 */
static TxnInsertInsert
txn_insert_tests_valued(uint32 database, TxnValuesEntry entries[2])
{
	static const char *const bytes[] = {"one", "two"};
	static const uint64 lengths[] = {4000, 300000};
	TxnInsertInsert valued = txn_insert_tests_row(txn_insert_tests_stored(database), 1);
	ChunksAddressAddress overflow = chunks_address_Address_of((const uint8 *) "overflow", 8);
	chunks_TupleU32Locator_t address = {.f0 = 0, .f1 = chunks_trailer_Locator_Default_default()};

	std_vec_free_Checked(&valued.put.value, 1, 1);
	std_slice_to_vec_Checked(&valued.put.value, overflow.f0, sizeof(overflow.f0), 1, 1);
	std_vec_free_Checked(&valued.put.addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_slice_to_vec_Checked(&valued.put.addresses, &address, 1, sizeof(chunks_TupleU32Locator_t),
							 _Alignof(chunks_TupleU32Locator_t));
	txn_insert_tests_values(&valued);
	for (size_t k = 0; k < 2; k++)
		entries[k] = txn_values_Entry_new(chunks_address_Address_of((const uint8 *) bytes[k], 3), lengths[k]);
	return valued;
}

/* txn_insert_tests_valued_t is a storage's first four blocks' free space entries with its TOAST relation's entries of the test's two value ids. */
typedef struct txn_insert_tests_valued_t
{
	txn_insert_tests_mapped_t entries[4];
	txn_insert_tests_mapped_t values[2];
} txn_insert_tests_valued_t;

/* txn_insert_tests_valued_Debug_fmt is the Debug of a storage's entries with its value map's, the tuple of their two lists. */
static bool
txn_insert_tests_valued_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_insert_tests_valued_t *valued = value;
	StdVec		entries = {(void *) valued->entries, 4, 4};
	StdVec		values = {(void *) valued->values, 2, 2};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&entries, txn_insert_tests_mappeds_Debug_fmt),
								  STD_ITEM(&values, txn_insert_tests_mappeds_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_insert_tests_valued_of returns a storage's first four blocks' entries with its TOAST relation's two value entries. */
static txn_insert_tests_valued_t
txn_insert_tests_valued_of(HostIoIo *io, const HostSharedHeld *held, const HostSharedHeld *toast)
{
	txn_insert_tests_free_t free = txn_insert_tests_free_entries(io, host_shared_Held_record(held));
	txn_insert_tests_valued_t valued;

	memcpy(valued.entries, free.entries, sizeof(valued.entries));
	txn_insert_tests_mapped(io, toast, valued.values);
	return valued;
}

/* txn_insert_tests_valued_drop drops a storage's entries and its value map's. */
static void
txn_insert_tests_valued_drop(txn_insert_tests_valued_t *valued)
{
	for (size_t k = 0; k < 4; k++)
		txn_insert_tests_mapped_drop(&valued->entries[k]);
	for (size_t k = 0; k < 2; k++)
		txn_insert_tests_mapped_drop(&valued->values[k]);
}

/* txn_insert_tests_row_is asserts at a line and column that a tuple id table's row is key 1's. */
static void
txn_insert_tests_row_is(uint32 line, uint32 col, bool some, const StdVec *row)
{
	static const uint8 one[8] = {0, 0, 0, 0, 0, 0, 0, 1};
	StdBytes	got = {some ? row->ptr : NULL, some ? row->len : 0};
	StdBytes	want = {one, sizeof(one)};
	StdOptionRef left = {some, {&got, std_bytes_Debug_fmt}};
	StdOptionRef right = {true, {&want, std_bytes_Debug_fmt}};

	STD_ASSERT_EQ(line, col, some && row->len == sizeof(one) && memcmp(row->ptr, one, sizeof(one)) == 0, &left,
				  std_option_Debug_fmt, &right, std_option_Debug_fmt);
}

/*
 * The overflow chunk case: a row whose out-of-line values' references sit in
 * an overflow chunk, so its own bytes name none, redone after a crash from
 * its record's field 24 alone, leaves its tuple id, its free space entries,
 * and its TOAST relation's value map entries as the primary's.
 */
void
test_txn_insert_tests_a_row_whose_value_references_sit_in_an_overflow_chunk_redoes_its_map_entries(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostSharedHeld toast;
	TreesStoreReader store;
	TxnValuesEntry entries[2];
	TxnInsertInsert row;
	uint64		tid;
	HostRow		tuple;
	txn_insert_tests_valued_t expected;
	txn_insert_tests_valued_t got;
	txn_insert_tests_replica_t replica;
	StdVec		writes;
	StdVec		tupled;
	HostError	error;
	bool		some;
	bool		eq = true;

	txn_insert_tests_repository(io, 9315, 2088, 87, 2089, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9315), 2090, 106);
	toast = txn_insert_tests_toasted(io, &repo, &reading, 9315, 2099, 10);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	row = txn_insert_tests_valued(9315, entries);
	tid = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, row, 60, &toast, entries, 2,
								2103, 82);
	expected = txn_insert_tests_valued_of(io, &held, &toast);
	STD_ASSERT(2105, 9, expected.values[0].some && expected.values[0].entry.has_value &&
			   expected.values[1].some && expected.values[1].entry.has_value,
			   "expected.1.iter().all(|entry| matches!(entry, Some(Some(_))))");
	writes = txn_insert_tests_logged(9315);
	replica = txn_insert_tests_replica(io, 9316, &writes);
	if (!replica.has_toast)
		STD_OPTION_UNWRAP_FAILED(2107, 47);
	got = txn_insert_tests_valued_of(io, &replica.held, &replica.toast);
	for (size_t k = 0; eq && k < 4; k++)
		eq = txn_insert_tests_mapped_eq(&got.entries[k], &expected.entries[k]);
	for (size_t k = 0; eq && k < 2; k++)
		eq = txn_insert_tests_mapped_eq(&got.values[k], &expected.values[k]);
	STD_ASSERT_EQ(2108, 9, eq, &got, txn_insert_tests_valued_Debug_fmt, &expected,
				  txn_insert_tests_valued_Debug_fmt);
	tuple.f0 = tid;
	if (!txn_memtable_tuples_get(io, host_shared_Held_record(&replica.held), tuple, &some, &tupled, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2109, 70, &error);
	txn_insert_tests_row_is(2109, 9, some, &tupled);
	if (some)
		std_vec_free_Checked(&tupled, 1, 1);
	txn_insert_tests_ended(io, 2110, 35);
	txn_insert_tests_valued_drop(&got);
	txn_insert_tests_replica_drop(&replica);
	txn_insert_tests_valued_drop(&expected);
	host_shared_Held_drop(&toast);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_unpacked is written's answer for a chunk's packing: no chunk is in a pack yet. */
static bool
txn_insert_tests_unpacked(void *captures, HostIoIo *io, const ChunksAddressAddress *address, bool *result,
						  HostError *error)
{
	*result = false;
	return true;
}

/*
 * txn_insert_tests_written writes a row's two out-of-line values' chunks
 * into its repository's working state, logged, under the row's writer, as the
 * fork's insert entry does before the row's window, and returns their working
 * state.
 */
static HostSharedHeld
txn_insert_tests_written(HostIoIo *io, uint32 database, TxnMemtableVersionWriter writer,
						 const TxnValuesEntry entries[2])
{
	static const char *const bytes[] = {"one", "two"};
	HostSharedHeld deltas;
	HostError	error;
	StdVec		chunks;
	bool		named = true;

	if (!txn_memtable_branch_deltas(io, database, HOST_PG_DEFAULT, &deltas, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2117, 73, &error);
	std_vec_with_capacity_Checked(&chunks, 2, sizeof(txn_TupleAddressVecU8_t), _Alignof(txn_TupleAddressVecU8_t));
	for (size_t k = 0; k < 2; k++)
	{
		txn_TupleAddressVecU8_t chunk = {.f0 = chunks_address_Address_of((const uint8 *) bytes[k], 3)};

		std_slice_to_vec_Checked(&chunk.f1, (const uint8 *) bytes[k], 3, 1, 1);
		std_vec_push_Checked(&chunks, &chunk, sizeof(chunk), _Alignof(txn_TupleAddressVecU8_t));
	}
	for (size_t k = 0; named && k < 2; k++)
		named = memcmp(STD_VEC_AT(&chunks, txn_TupleAddressVecU8_t, k).f0.f0, entries[k].address.f0,
					   sizeof(entries[k].address.f0)) == 0;
	STD_ASSERT(2119, 9, named, "chunks.iter().zip(entries).all(|((address, _), entry)| *address == entry.address)");
	if (!txn_blobs_written(io, &deltas, database, writer, &chunks, true, txn_insert_tests_unpacked, NULL, NULL,
						   &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2120, 91, &error);
	return deltas;
}

/* txn_insert_tests_mappeds_is asserts at a line and column, with a message or none, that two value map entries are those wanted. */
static void
txn_insert_tests_mappeds_is(uint32 line, uint32 col, const txn_insert_tests_mapped_t got[2],
							const txn_insert_tests_mapped_t want[2], const StdFmtArguments *message)
{
	StdVec		left = {(void *) got, 2, 2};
	StdVec		right = {(void *) want, 2, 2};

	if (!txn_insert_tests_mapped_eq(&got[0], &want[0]) || !txn_insert_tests_mapped_eq(&got[1], &want[1]))
		std_assert_failed(RUST_FILE, line, col, "==", &left, txn_insert_tests_mappeds_Debug_fmt, &right,
						  txn_insert_tests_mappeds_Debug_fmt, message);
}

/* txn_insert_tests_durable reports whether a chunk of a repository's working state is durable, unwrapped at a line and column. */
static bool
txn_insert_tests_durable(HostIoIo *io, const HostSharedHeld *deltas, const TxnValuesEntry *entry, uint32 line,
						 uint32 col)
{
	HostError	error;
	bool		durable;

	if (!txn_memtable_branch_durable(io, host_shared_Held_record(deltas), entry->address.f0,
									 sizeof(entry->address.f0), &durable, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(line, col, &error);
	return durable;
}

/*
 * Rule (C)'s first case: an insert with two out-of-line values that fails
 * after its chunks' records and before its window leaves no value map entry
 * and logs no row change, and its chunks stay in the working state
 * unreferenced.
 */
void
test_txn_insert_tests_an_insert_that_fails_after_its_chunks_maps_no_value(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostSharedHeld toast;
	HostSharedHeld deltas;
	TreesStoreReader store;
	TxnValuesEntry entries[2];
	TxnInsertInsert row;
	TxnInsertRoom room = txn_insert_tests_room(60);
	txn_insert_tests_mapped_t mapped[2];
	txn_insert_tests_mapped_t none[2] = {0};
	HostError	error;
	uint64		tid;
	bool		failed;
	StdVec		writes;

	txn_insert_tests_repository(io, 9317, 2130, 87, 2131, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9317), 2132, 106);
	toast = txn_insert_tests_toasted(io, &repo, &reading, 9317, 2141, 10);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	row = txn_insert_tests_valued(9317, entries);
	deltas = txn_insert_tests_written(io, 9317, row.writer, entries);
	simc_refuse_any_take(0);
	failed = !txn_insert_tests_taken(io, TREES_STORE_READER_NODE_STORE(&store), &held, &row, &room, &toast, entries,
									 2, &tid, &error);
	simc_refuse_nothing();
	txn_insert_tests_failed_is(2149, 9, failed, &error);
	txn_insert_tests_mapped(io, &toast, mapped);
	txn_insert_tests_mappeds_is(2150, 9, mapped, none, NULL);
	writes = txn_insert_tests_logged(9317);
	STD_ASSERT_EQ_U64(2152, 9, txn_insert_tests_count(&writes, TXN_INSERT_TESTS_LOGGED_CHUNK), 2);
	STD_ASSERT(2153, 9, txn_insert_tests_count(&writes, TXN_INSERT_TESTS_LOGGED_ROW) == 0,
			   "!writes.iter().any(|write| matches!(write, Logged::Row(..)))");
	for (size_t k = 0; k < 2; k++)
		STD_ASSERT(2155, 13, txn_insert_tests_durable(io, &deltas, &entries[k], 2155, 76),
				   "branch::durable(io, deltas.record(), &entry.address.0).unwrap()");
	txn_insert_tests_ended(io, 2157, 35);
	txn_insert_tests_logged_free(&writes);
	for (size_t k = 0; k < 2; k++)
		txn_insert_tests_mapped_drop(&mapped[k]);
	host_shared_Held_drop(&deltas);
	host_shared_Held_drop(&toast);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_from_t is what a replica redone from a point holds: its row by the tuple id, and its two value map entries. */
typedef struct txn_insert_tests_from_t
{
	bool		some;
	StdVec		row;
	txn_insert_tests_mapped_t values[2];
} txn_insert_tests_from_t;

/* txn_insert_tests_from_Debug_fmt is the Debug of what a replica holds, the tuple (row, values). */
static bool
txn_insert_tests_from_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_insert_tests_from_t *from = value;
	StdBytes	row = {from->row.ptr, from->row.len};
	StdOptionRef some = {from->some, {&row, std_bytes_Debug_fmt}};
	StdVec		values = {(void *) from->values, 2, 2};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&some, std_option_Debug_fmt),
								  STD_ITEM(&values, txn_insert_tests_mappeds_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_insert_tests_from_eq reports whether what two replicas hold is equal. */
static bool
txn_insert_tests_from_eq(const txn_insert_tests_from_t *a, const txn_insert_tests_from_t *b)
{
	return a->some == b->some && (!a->some || trees_store_VecU8_PartialEq_eq(&a->row, &b->row)) &&
		txn_insert_tests_mapped_eq(&a->values[0], &b->values[0]) &&
		txn_insert_tests_mapped_eq(&a->values[1], &b->values[1]);
}

/*
 * Rule (C)'s second case, as far as the logged records show it: redo started
 * at each R across an insert with two out-of-line values, before its chunks'
 * records, between them and its row change, and after the row change, gives
 * the row with every value map entry or neither, since field 24 makes the
 * row and its entries one record.
 */
void
test_txn_insert_tests_redo_from_any_point_gives_a_row_with_all_its_map_entries_or_none(void)
{
	static const struct
	{
		uint32		database;
		size_t		from;
	}			froms[] = {{9319, 0}, {9320, 2}, {9321, 3}};
	static const uint8 one[8] = {0, 0, 0, 0, 0, 0, 0, 1};
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("from "), STD_FMT_ARG(0)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostSharedHeld toast;
	HostSharedHeld deltas;
	TreesStoreReader store;
	TxnValuesEntry entries[2];
	TxnInsertInsert row;
	txn_insert_tests_mapped_t primary[2];
	uint64		tid;
	StdVec		writes;

	txn_insert_tests_repository(io, 9318, 2168, 87, 2169, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9318), 2170, 106);
	toast = txn_insert_tests_toasted(io, &repo, &reading, 9318, 2179, 10);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	row = txn_insert_tests_valued(9318, entries);
	deltas = txn_insert_tests_written(io, 9318, row.writer, entries);
	host_shared_Held_drop(&deltas);
	tid = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, row, 60, &toast, entries, 2,
								2184, 82);
	txn_insert_tests_mapped(io, &toast, primary);
	writes = txn_insert_tests_logged(9318);
	STD_ASSERT_EQ_U64(2186, 9, writes.len, 3);
	txn_insert_tests_logged_free(&writes);
	for (size_t k = 0; k < lengthof(froms); k++)
	{
		StdFmtArg	args[] = {{&froms[k].from, std_usize_Display_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};
		txn_insert_tests_replica_t replica;
		txn_insert_tests_from_t got = {0};
		txn_insert_tests_from_t want = {0};
		HostRow		tuple = {tid};
		HostError	error;

		writes = txn_insert_tests_logged(9318);
		for (size_t w = 0; w < froms[k].from && w < writes.len; w++)
			txn_insert_tests_logged_drop(&STD_VEC_AT(&writes, txn_insert_tests_logged_t, w));
		{
			size_t		skipped = Min(froms[k].from, writes.len);

			memmove(writes.ptr, &STD_VEC_AT(&writes, txn_insert_tests_logged_t, skipped),
					(writes.len - skipped) * sizeof(txn_insert_tests_logged_t));
			writes.len -= skipped;
		}
		replica = txn_insert_tests_replica(io, froms[k].database, &writes);
		if (!txn_memtable_tuples_get(io, host_shared_Held_record(&replica.held), tuple, &got.some, &got.row, &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(2190, 73, &error);
		if (replica.has_toast)
			txn_insert_tests_mapped(io, &replica.toast, got.values);
		if (froms[k].from != 3)
		{
			want.some = true;
			want.row = (StdVec)
			{
				(void *) one, sizeof(one), sizeof(one)
			};
			want.values[0] = primary[0];
			want.values[1] = primary[1];
		}
		if (!txn_insert_tests_from_eq(&got, &want) && froms[k].from == 3)
			std_assert_failed(RUST_FILE, 2196, 22, "==", &got, txn_insert_tests_from_Debug_fmt, &want,
							  txn_insert_tests_from_Debug_fmt, &message);
		if (!txn_insert_tests_from_eq(&got, &want))
			std_assert_failed(RUST_FILE, 2197, 22, "==", &got, txn_insert_tests_from_Debug_fmt, &want,
							  txn_insert_tests_from_Debug_fmt, &message);
		for (size_t e = 0; e < 2; e++)
		{
			bool		durable = txn_insert_tests_durable(io, &replica.deltas, &entries[e], 2200, 92);
			bool		wanted = froms[k].from == 0;

			STD_ASSERT_EQ(2200, 17, durable == wanted, &durable, std_bool_Debug_fmt, &wanted,
						  std_bool_Debug_fmt);
		}
		for (size_t e = 0; e < 2; e++)
			txn_insert_tests_mapped_drop(&got.values[e]);
		if (got.some)
			std_vec_free_Checked(&got.row, 1, 1);
		txn_insert_tests_replica_drop(&replica);
	}
	txn_insert_tests_ended(io, 2203, 35);
	for (size_t k = 0; k < 2; k++)
		txn_insert_tests_mapped_drop(&primary[k]);
	host_shared_Held_drop(&toast);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_counts reads the map counts of the test's two value ids in a TOAST relation, unwrapped at the lines and columns a case gives. */
static void
txn_insert_tests_counts(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *toast, uint32 error_line,
						uint32 error_col, uint32 none_line, uint32 none_col, uint64 counts[2])
{
	for (size_t k = 0; k < 2; k++)
	{
		TxnValuesEntry entry;
		HostError	error;
		bool		has;

		if (!txn_values_entry(io, store, toast, TXN_INSERT_TESTS_IDS[k], &has, &entry, &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(error_line, error_col, &error);
		if (!has)
			STD_OPTION_UNWRAP_FAILED(none_line, none_col);
		counts[k] = entry.count;
	}
}

/*
 * (4b)'s section 5: an update that keeps a row's two out-of-line values,
 * field 28, redone adds one to each kept value's map entry with the record's
 * LSN, and redone again, which the map holds by the id and that LSN, adds
 * nothing.
 */
void
test_txn_insert_tests_a_kept_values_count_add_is_redone_once(void)
{
	static const uint64 once[] = {1, 1};
	static const uint64 twice[] = {2, 2};
	static const size_t kept[] = {0, 1};
	static const uint8 one[8] = {0, 0, 0, 0, 0, 0, 0, 1};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostSharedHeld toast;
	HostSharedHeld deltas;
	TreesStoreReader store;
	TxnValuesEntry entries[2];
	TxnInsertInsert row;
	bool		has_relation;
	TxnRecordToast relation;
	uint64		tid;
	TxnRecordRowChange update = {.writer = {.xid = 42}, .table = 16385, .heap_bytes = 60};
	ChunksAddressAddress overflow = chunks_address_Address_of((const uint8 *) "overflow", 8);
	chunks_TupleU32Locator_t address = {.f0 = 0, .f1 = chunks_trailer_Locator_Default_default()};
	uint64		counts[2];
	HostError	error;

	txn_insert_tests_repository(io, 9323, 2212, 87, 2213, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9323), 2214, 106);
	toast = txn_insert_tests_toasted(io, &repo, &reading, 9323, 2223, 10);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	row = txn_insert_tests_valued(9323, entries);
	deltas = txn_insert_tests_written(io, 9323, row.writer, entries);
	host_shared_Held_drop(&deltas);
	has_relation = row.has_toast;
	relation = row.toast;
	tid = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, row, 60, &toast, entries, 2,
								2229, 82);
	update.storage = txn_insert_tests_stored(9323);
	std_slice_to_vec_Checked(&update.key, one, sizeof(one), 1, 1);
	update.op.tag = TXN_MEMTABLE_OP_PUT;
	std_slice_to_vec_Checked(&update.op.u.put.value, overflow.f0, sizeof(overflow.f0), 1, 1);
	std_slice_to_vec_Checked(&update.op.u.put.addresses, &address, 1, sizeof(chunks_TupleU32Locator_t),
							 _Alignof(chunks_TupleU32Locator_t));
	std_slice_to_vec_Checked(&update.op.u.put.values, TXN_INSERT_TESTS_IDS, 2, sizeof(uint32), _Alignof(uint32));
	update.op.u.put.tuple.f0 = tid + 1;
	update.op.u.put.heap_bytes = 60;
	update.has_toast = has_relation;
	update.toast = relation;
	std_vec_new(&update.blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
	std_slice_to_vec_Checked(&update.kept, kept, lengthof(kept), sizeof(size_t), _Alignof(size_t));
	txn_insert_tests_counts(io, TREES_STORE_READER_NODE_STORE(&store), &toast, 2254, 82, 2254, 91, counts);
	txn_insert_tests_u64s_is(2256, 9, counts, 2, once, 2);
	if (!txn_insert_redo(io, TREES_STORE_READER_NODE_STORE(&store), &held, &update, 0x100000, &TXN_INSERT_TESTS_PAGE,
						 NULL, &toast, NULL, 0, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2257, 81, &error);
	txn_insert_tests_counts(io, TREES_STORE_READER_NODE_STORE(&store), &toast, 2254, 82, 2254, 91, counts);
	txn_insert_tests_u64s_is(2258, 9, counts, 2, twice, 2);
	if (!txn_insert_redo(io, TREES_STORE_READER_NODE_STORE(&store), &held, &update, 0x100000, &TXN_INSERT_TESTS_PAGE,
						 NULL, &toast, NULL, 0, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2259, 81, &error);
	txn_insert_tests_counts(io, TREES_STORE_READER_NODE_STORE(&store), &toast, 2254, 82, 2254, 91, counts);
	txn_insert_tests_u64s_is(2260, 9, counts, 2, twice, 2);
	txn_insert_tests_ended(io, 2261, 35);
	txn_record_RowChange_drop(&update);
	host_shared_Held_drop(&toast);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_left_t is the load case's captures: the bytes of the free space entry a leave handed to the map. */
typedef struct txn_insert_tests_left_t
{
	StdVec		left;
} txn_insert_tests_left_t;

/* txn_insert_tests_left_step is the load case's take's step: it puts the left page's entry. */
static TxnMemtableMapStep
txn_insert_tests_left_step(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	const txn_insert_tests_left_t *c = captures;

	std_vec_extend_from_slice_Checked(out, c->left.ptr, c->left.len, 1, 1);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_insert_tests_left_then is the load case's take's then, which does nothing. */
static void
txn_insert_tests_left_then(void *captures)
{
}

/* txn_insert_tests_left_inside is the load case's prepare's closure: block 0's take of the left page's entry, with no transaction. */
static bool
txn_insert_tests_left_inside(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
							 TxnMemtableMapTaken *result, HostError *error)
{
	TxnStatusRoomed unroomed = {0};

	return txn_memtable_map_finish_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, TXN_MEMTABLE_VERSION_NO_TRANSACTION,
										false, unroomed,
										false, 0, false, NULL, 0, NULL, txn_insert_tests_left_step, captures, NULL,
										txn_insert_tests_left_then, NULL, NULL, result, error);
}

/* txn_insert_tests_loaded_t is the load case's state: the counter and the first four blocks' free space entries. */
typedef txn_insert_tests_free_t txn_insert_tests_loaded_t;

/* txn_insert_tests_loaded_Debug_fmt is the load case's state's Debug, the tuple (counter, entries). */
static bool
txn_insert_tests_loaded_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_insert_tests_free_t *loaded = value;
	StdVec		entries = {(void *) loaded->entries, lengthof(loaded->entries), lengthof(loaded->entries)};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&loaded->counter, std_u64_Debug_fmt),
								  STD_ITEM(&entries, txn_insert_tests_mappeds_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_insert_tests_load runs the runs' loader on a storage up to a tuple id, unwrapped at a line and column. */
static void
txn_insert_tests_load(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint64 to, uint32 line,
					  uint32 col)
{
	HostError	error;

	if (!txn_insert_loaded(io, store, held, &TXN_INSERT_TESTS_PAGE, to, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(line, col, &error);
}

/*
 * The checkpoint runs text's L2: a restart whose loaded runs hold rows up to
 * (1,5), and block 0's free space entry from a page a leave handed to the
 * map, offsets 3 on, moves the counter to (1,6), the tuple id its loader
 * gives, and leaves the entry as loaded; a second load changes nothing; an
 * unrelated transaction's abort leaves the move and the entry; and the next
 * inserts take (0,3) and (0,4), never a loaded row's tuple id.
 */
void
test_txn_insert_tests_a_load_moves_the_counter_past_every_loaded_tid_and_left_page(void)
{
	static const uint64 want[] = {3, 4};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TreesStoreReader store;
	txn_insert_tests_left_t c;
	uint16		offsets[289];
	TxnFreeSpaceFree free;
	TxnMemtableMapTake take;
	TxnMemtableMapTaken taken;
	txn_insert_tests_free_t expected;
	txn_insert_tests_free_t again;
	HostXid		unrelated;
	HostError	error;
	uint64		tids[2];
	bool		eq;

	txn_insert_tests_repository(io, 9322, 2273, 87, 2274, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9322), 2275, 106);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	for (uint16 offset = 3; offset <= 291; offset++)
		offsets[offset - 3] = offset;
	free = txn_free_space_Free_new(8168 - 6000, offsets, lengthof(offsets), 291);
	std_vec_new(&c.left, 1, 1);
	txn_free_space_Free_encode_into(&free, &c.left);
	take.state = host_shared_Held_record(&held);
	take.map = TXN_MEMTABLE_MAP_MAP_FREE;
	take.key = 0;
	take.most = txn_free_space_most_len(291);
	if (!txn_memtable_map_prepare_Taken(io, &take, 1, txn_insert_tests_left_inside, &c, NULL, &taken, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2287, 10, &error);
	txn_insert_tests_load(io, TREES_STORE_READER_NODE_STORE(&store), &held, 1 << 16 | 6, 2290, 55);
	expected = txn_insert_tests_free_entries(io, host_shared_Held_record(&held));
	STD_ASSERT_EQ_U64(2292, 9, expected.counter, 1 << 16 | 6);
	{
		txn_insert_tests_mapped_t left = {.some = true, .entry = {.has_value = true, .value = c.left}};

		STD_ASSERT_EQ(2293, 9, txn_insert_tests_mapped_eq(&expected.entries[0], &left), &expected.entries[0],
					  txn_insert_tests_mapped_Debug_fmt, &left, txn_insert_tests_mapped_Debug_fmt);
	}
	txn_insert_tests_load(io, TREES_STORE_READER_NODE_STORE(&store), &held, 1 << 16 | 6, 2294, 55);
	unrelated = simc_begin();
	simc_abort(unrelated);
	again = txn_insert_tests_free_entries(io, host_shared_Held_record(&held));
	eq = again.counter == expected.counter;
	for (size_t k = 0; eq && k < lengthof(again.entries); k++)
		eq = txn_insert_tests_mapped_eq(&again.entries[k], &expected.entries[k]);
	STD_ASSERT_EQ(2297, 9, eq, &again, txn_insert_tests_loaded_Debug_fmt, &expected,
				  txn_insert_tests_loaded_Debug_fmt);
	for (uint64 n = 10; n < 12; n++)
		tids[n - 10] = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held,
											 txn_insert_tests_row(txn_insert_tests_stored(9322), n), 28, NULL, NULL, 0,
											 2300, 90);
	txn_insert_tests_u64s_is(2301, 9, tids, 2, want, 2);
	txn_insert_tests_ended(io, 2302, 35);
	txn_insert_tests_free_drop(&again);
	txn_insert_tests_free_drop(&expected);
	std_vec_free_Checked(&c.left, 1, 1);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_ours returns the new cid records the simulation's WAL took for database 9324's storages, in their order. */
static StdVec
txn_insert_tests_ours(void)
{
	StdVec		cids = simc_new_cids();
	StdVec		ours;

	std_vec_new(&ours, sizeof(simc_TupleStorageRowNewCidLsn_t), _Alignof(simc_TupleStorageRowNewCidLsn_t));
	for (size_t k = 0; k < cids.len; k++)
	{
		const simc_TupleStorageRowNewCidLsn_t *cid = &STD_VEC_AT(&cids, simc_TupleStorageRowNewCidLsn_t, k);

		if (cid->f0.database == 9324)
			std_vec_push_Checked(&ours, cid, sizeof(*cid), _Alignof(simc_TupleStorageRowNewCidLsn_t));
	}
	std_vec_free_Checked(&cids, sizeof(simc_TupleStorageRowNewCidLsn_t), _Alignof(simc_TupleStorageRowNewCidLsn_t));
	return ours;
}

/* txn_insert_tests_own returns the end of the newest row change database 9324's storages logged, if any. */
static bool
txn_insert_tests_own(HostLsn *end)
{
	StdVec		writes = txn_insert_tests_logged(9324);
	bool		found = false;

	for (size_t k = writes.len; !found && k > 0; k--)
	{
		const txn_insert_tests_logged_t *write = &STD_VEC_AT(&writes, txn_insert_tests_logged_t, k - 1);

		if (write->tag == TXN_INSERT_TESTS_LOGGED_ROW)
		{
			*end = write->end;
			found = true;
		}
	}
	txn_insert_tests_logged_free(&writes);
	return found;
}

/*
 * CommPF 5's NEW_CID ruling: a logged insert into a relation logical
 * decoding reads logs heap's record of its row's command before its own
 * record, with the row's tuple id and the inserting command, as heap_insert
 * does; one into any other relation, or an unlogged one, logs none.
 */
void
test_txn_insert_tests_an_insert_logical_decoding_reads_logs_heaps_record_of_its_command(void)
{
	static const struct
	{
		bool		logged;
		bool		decoded;
	}			cases[] = {{true, false}, {false, true}, {true, true}};
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("logged "), STD_FMT_ARG(0), STD_FMT_LIT(", decoded "),
	STD_FMT_ARG(1)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TreesStoreReader store;

	txn_insert_tests_repository(io, 9324, 2312, 87, 2313, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9324), 2314, 106);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	for (size_t n = 0; n < lengthof(cases); n++)
	{
		bool		logged = cases[n].logged;
		bool		decoded = cases[n].decoded;
		TxnInsertInsert inserted = txn_insert_tests_row(txn_insert_tests_stored(9324), n);
		uint64		tid;
		StdVec		cids;

		inserted.logged = logged;
		inserted.decoded = decoded;
		inserted.writer.command = 5;
		tid = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, inserted, 28, NULL, NULL, 0,
									2328, 71);
		cids = txn_insert_tests_ours();
		if (cids.len == 1 && logged && decoded)
		{
			const simc_TupleStorageRowNewCidLsn_t *cid = &STD_VEC_AT(&cids, simc_TupleStorageRowNewCidLsn_t, 0);
			HostStorage storage = txn_insert_tests_stored(9324);
			HostRow		row = {tid};
			HostNewCid	want = {.has_cmin = true, .cmin = {5}};
			StdTupleRef got = STD_TUPLE(3, STD_ITEM(&cid->f0, host_Storage_Debug_fmt),
										STD_ITEM(&cid->f1, host_Row_Debug_fmt),
										STD_ITEM(&cid->f2, host_NewCid_Debug_fmt));
			StdTupleRef wanted = STD_TUPLE(3, STD_ITEM(&storage, host_Storage_Debug_fmt),
										   STD_ITEM(&row, host_Row_Debug_fmt),
										   STD_ITEM(&want, host_NewCid_Debug_fmt));
			HostLsn		own;

			STD_ASSERT_EQ(2333, 21, host_Storage_PartialEq_eq(&cid->f0, &storage) && cid->f1.f0 == tid &&
						  cid->f2.has_cmin && cid->f2.cmin.f0 == 5 && !cid->f2.has_cmax && !cid->f2.has_combo,
						  &got, std_tuple_Debug_fmt, &wanted, std_tuple_Debug_fmt);
			STD_ASSERT(2334, 21, txn_insert_tests_own(&own) && cid->f3 < own,
					   "own().is_some_and(|own| end < own)");
		}
		else if (!(cids.len == 0 && !(logged && decoded)))
		{
			StdFmtArg	args[] = {{&logged, std_bool_Display_fmt}, {&decoded, std_bool_Display_fmt}};
			StdFmtArguments message = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 2336, 22, &message);
		}
		std_vec_free_Checked(&cids, sizeof(simc_TupleStorageRowNewCidLsn_t),
							 _Alignof(simc_TupleStorageRowNewCidLsn_t));
	}
	txn_insert_tests_ended(io, 2339, 35);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_chain_t is a key of a storage's rows with its count of versions, as a walk hands them out. */
typedef struct txn_insert_tests_chain_t
{
	StdVec		key;
	size_t		versions;
} txn_insert_tests_chain_t;

/* txn_insert_tests_chains_each is chains' step: a key and its count of versions pushed onto the list. */
static bool
txn_insert_tests_chains_each(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
							 size_t values_len, HostError *error)
{
	txn_insert_tests_chain_t chain = {.versions = values_len};

	std_slice_to_vec_Checked(&chain.key, key, key_len, 1, 1);
	std_vec_push_Checked(captures, &chain, sizeof(chain), _Alignof(txn_insert_tests_chain_t));
	return true;
}

/* txn_insert_tests_chains returns each key of a storage's rows with its count of versions, as a walk hands them out. */
static StdVec
txn_insert_tests_chains(HostIoIo *io, HostSharedRecord record)
{
	StdVec		chains;
	HostError	error;

	std_vec_new(&chains, sizeof(txn_insert_tests_chain_t), _Alignof(txn_insert_tests_chain_t));
	if (!txn_memtable_table_walked(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, record, txn_insert_tests_chains_each,
								   &chains, NULL, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2349, 56, &error);
	return chains;
}

/* txn_insert_tests_chains_free frees a list of a storage's keys with their counts. */
static void
txn_insert_tests_chains_free(StdVec *chains)
{
	for (size_t k = 0; k < chains->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(chains, txn_insert_tests_chain_t, k).key, 1, 1);
	std_vec_free_Checked(chains, sizeof(txn_insert_tests_chain_t), _Alignof(txn_insert_tests_chain_t));
}

/* txn_insert_tests_chain_Debug_fmt is a key and its count's Debug, the tuple (key, count). */
static bool
txn_insert_tests_chain_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_insert_tests_chain_t *chain = value;
	StdBytes	key = {chain->key.ptr, chain->key.len};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&key, std_bytes_Debug_fmt),
								  STD_ITEM(&chain->versions, std_usize_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_insert_tests_chains_Debug_fmt is a Vec<(Vec<u8>, usize)>'s Debug. */
static bool
txn_insert_tests_chains_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *chains = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < chains->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(chains, txn_insert_tests_chain_t, k), txn_insert_tests_chain_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_insert_tests_chains_eq reports whether two lists of a storage's keys with their counts are equal, as Vec<(Vec<u8>, usize)>s compare. */
static bool
txn_insert_tests_chains_eq(const StdVec *a, const StdVec *b)
{
	bool		eq = a->len == b->len;

	for (size_t k = 0; eq && k < a->len; k++)
	{
		const txn_insert_tests_chain_t *left = &STD_VEC_AT(a, txn_insert_tests_chain_t, k);
		const txn_insert_tests_chain_t *right = &STD_VEC_AT(b, txn_insert_tests_chain_t, k);

		eq = left->versions == right->versions && trees_store_VecU8_PartialEq_eq(&left->key, &right->key);
	}
	return eq;
}

/*
 * txn_insert_tests_held_once asserts at a line and column, with a message or
 * none, that a storage's rows are keys 0 up to a count, each with one
 * version, and frees the list.
 */
static void
txn_insert_tests_held_once(uint32 line, uint32 col, StdVec *chains, uint64 count, const StdFmtArguments *message)
{
	StdVec		want;

	std_vec_new(&want, sizeof(txn_insert_tests_chain_t), _Alignof(txn_insert_tests_chain_t));
	for (uint64 n = 0; n < count; n++)
	{
		txn_insert_tests_chain_t chain = {.versions = 1};
		uint8		key[8];

		std_u64_to_be_bytes(n, key);
		std_slice_to_vec_Checked(&chain.key, key, sizeof(key), 1, 1);
		std_vec_push_Checked(&want, &chain, sizeof(chain), _Alignof(txn_insert_tests_chain_t));
	}
	if (!txn_insert_tests_chains_eq(chains, &want))
		std_assert_failed(RUST_FILE, line, col, "==", chains, txn_insert_tests_chains_Debug_fmt, &want,
						  txn_insert_tests_chains_Debug_fmt, message);
	txn_insert_tests_chains_free(&want);
	txn_insert_tests_chains_free(chains);
}

/* txn_insert_tests_no_edits is the checkpoint case's save's gathering, which edits nothing. */
static bool
txn_insert_tests_no_edits(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result, ChunksErrorError *error)
{
	std_vec_new(result, sizeof(RepoGathered), _Alignof(RepoGathered));
	return true;
}

/* txn_insert_tests_no_marks is the checkpoint case's save's marks, which mark nothing. */
static bool
txn_insert_tests_no_marks(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	return true;
}

/*
 * I12 and the checkpoint runs text's straddle case: a checkpoint's runs
 * loaded after a crash and followed by every logged record leave each
 * block's free space entry and the counter as the primary's, so no tid is
 * handed out twice, and every row held once: when the runs hold everything,
 * so redo pushes no row again; when they hold the inserts' free space takes
 * but not their rows; and when they are a restartpoint's, written in the
 * middle of the records' redo.
 */
void
test_txn_insert_tests_a_checkpoints_runs_and_every_record_after_redo_as_the_primary(void)
{
	static const struct
	{
		uint32		source;
		uint32		database;
		bool		rows;
		size_t		redone_before;
	}			cases[] = {{9326, 9327, true, SIZE_MAX}, {9328, 9329, false, SIZE_MAX}, {9331, 9332, true, 5}};
	static const uint64 sizes[] = {3000, 3000, 3000, 3000, 3000, 2000, 3000, 2000};
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;

	for (size_t k = 0; k < lengthof(cases); k++)
	{
		uint32		database = cases[k].database;
		StdFmtArg	args[] = {{&database, std_u32_Display_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};
		RepoRepo	primary;
		RepoRepo	repo;
		HostSharedReader reading;
		HostSharedHeld held;
		HostSharedHeld deltas;
		TreesStoreReader store;
		txn_insert_tests_free_t expected;
		txn_insert_tests_free_t got;
		txn_insert_tests_replica_t replica;
		StdArcInner *manifest;
		const RepoManifest *newest;
		RepoRuns	runs;
		StdVec		before;
		StdVec		writes;
		StdVec		chains;
		ChunksErrorError failed;
		HostError	error;
		size_t		held_before;
		size_t		kept = 0;

		txn_insert_tests_repository(io, cases[k].source, 2367, 96, 2368, 50, &primary, &reading);
		held = txn_insert_tests_stated(io, &primary, &reading, txn_insert_tests_stored(cases[k].source),
									   2370, 108);
		store = repo_Repo_reader(&primary, HOST_CACHE_PLACEMENT_SERVER, &reading);
		for (uint64 n = 0; n < lengthof(sizes); n++)
			txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held,
								  txn_insert_tests_row(txn_insert_tests_stored(cases[k].source), n), sizes[n], NULL,
								  NULL, 0, 2374, 79);
		expected = txn_insert_tests_free_entries(io, host_shared_Held_record(&held));
		before = txn_insert_tests_logged(cases[k].source);
		for (size_t w = Min(cases[k].redone_before, before.len); w < before.len; w++)
			txn_insert_tests_logged_drop(&STD_VEC_AT(&before, txn_insert_tests_logged_t, w));
		before.len = Min(cases[k].redone_before, before.len);
		held_before = txn_insert_tests_count(&before, TXN_INSERT_TESTS_LOGGED_ROW);
		replica = txn_insert_tests_replica(io, database, &before);
		txn_insert_tests_replica_drop(&replica);
		host_shared_Held_drop(&held);
		simc_checkpointed(UINT64CONST(0x100000000));
		if (!txn_runs_checkpoint(io, UINT64CONST(0x100000001), &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(2383, 56, &error);
		if (!repo_Repo_open(io, database, HOST_PG_DEFAULT, &repo, &failed))
			STD_RESULT_EXPECT_FAILED(2384, 61, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		if (!repo_Repo_manifest(&repo, io, &manifest, &failed))
			STD_RESULT_EXPECT_FAILED(2385, 46, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		newest = repo_Repo_newest(manifest);
		if (!newest->has_runs)
			STD_OPTION_UNWRAP_FAILED(2385, 68);
		repo_Runs_Clone_clone(&newest->runs, &runs);
		repo_ArcManifest_drop(manifest);
		for (size_t f = 0; f < runs.files.len; f++)
		{
			const RepoRunFile *file = &STD_VEC_AT(&runs.files, RepoRunFile, f);

			if (cases[k].rows || file->kind != TXN_RUNS_KIND_ROWS)
				STD_VEC_AT(&runs.files, RepoRunFile, kept++) = *file;
		}
		runs.files.len = kept;
		if (!repo_Repo_save(&repo, io, &reading, true, &runs, txn_insert_tests_no_edits, NULL,
							txn_insert_tests_no_marks, NULL, &failed))
			STD_RESULT_EXPECT_FAILED(2387, 84, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		for (size_t u = 0; u < 2; u++)
		{
			HostStorage unlinked = txn_insert_tests_stored(u == 0 ? cases[k].source : database);

			if (!txn_state_unlinked(io, &unlinked, &error))
				TXN_INSERT_TESTS_UNWRAP_FAILED(2389, 56, &error);
		}
		if (!txn_runs_loaded(io, &TXN_INSERT_TESTS_PAGE, &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(2391, 71, &error);
		held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(database), 2393, 107);
		chains = txn_insert_tests_chains(io, host_shared_Held_record(&held));
		{
			size_t		len = chains.len;
			size_t		want = cases[k].rows ? held_before : 0;

			if (len != want)
				std_assert_failed(RUST_FILE, 2394, 13, "==", &len, std_usize_Debug_fmt, &want,
								  std_usize_Debug_fmt, &message);
		}
		txn_insert_tests_chains_free(&chains);
		if (!txn_memtable_branch_deltas(io, database, HOST_PG_DEFAULT, &deltas, &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(2395, 69, &error);
		store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
		writes = txn_insert_tests_logged(cases[k].source);
		txn_insert_tests_redone(io, TREES_STORE_READER_NODE_STORE(&store), &held, NULL, &deltas, &writes);
		got = txn_insert_tests_free_entries(io, host_shared_Held_record(&held));
		txn_insert_tests_free_is(2398, 13, &got, &expected, &message);
		txn_insert_tests_free_drop(&got);
		chains = txn_insert_tests_chains(io, host_shared_Held_record(&held));
		txn_insert_tests_held_once(2400, 13, &chains, 8, &message);
		writes = txn_insert_tests_logged(cases[k].source);
		for (size_t w = 0; w < writes.len; w++)
		{
			const txn_insert_tests_logged_t *write = &STD_VEC_AT(&writes, txn_insert_tests_logged_t, w);
			const TxnMemtablePut *put;
			StdVec		tupled;
			bool		some;

			if (write->tag != TXN_INSERT_TESTS_LOGGED_ROW || !txn_record_RowChange_put(&write->u.row, &put))
				continue;
			if (!txn_memtable_tuples_get(io, host_shared_Held_record(&held), put->tuple, &some, &tupled, &error))
				TXN_INSERT_TESTS_UNWRAP_FAILED(2405, 74, &error);
			if (!some || !trees_store_VecU8_PartialEq_eq(&tupled, &write->u.row.key))
			{
				StdBytes	row = {some ? tupled.ptr : NULL, some ? tupled.len : 0};
				StdBytes	key = {write->u.row.key.ptr, write->u.row.key.len};
				StdOptionRef left = {some, {&row, std_bytes_Debug_fmt}};
				StdOptionRef right = {true, {&key, std_bytes_Debug_fmt}};

				std_assert_failed(RUST_FILE, 2405, 21, "==", &left, std_option_Debug_fmt, &right,
								  std_option_Debug_fmt, &message);
			}
			std_vec_free_Checked(&tupled, 1, 1);
		}
		txn_insert_tests_logged_free(&writes);
		host_shared_Held_drop(&deltas);
		host_shared_Held_drop(&held);
		repo_Repo_drop(&repo);
		txn_insert_tests_free_drop(&expected);
		host_shared_Reader_drop(&reading);
		repo_Repo_drop(&primary);
	}
	txn_insert_tests_ended(io, 2409, 35);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/* txn_insert_tests_straddle_t is the straddle's inserter's captures: its database, its count of inserts, and its rows' heap bytes. */
typedef struct txn_insert_tests_straddle_t
{
	uint32		database;
	uint64		inserts;
	uint64		bytes;
} txn_insert_tests_straddle_t;

/* txn_insert_tests_checkpointer is a case's checkpointer: a checkpoint whose redo is past the test's last. */
static bool
txn_insert_tests_checkpointer(void *captures, HostIoIo *io, HostError *error)
{
	return txn_runs_checkpoint(io, UINT64CONST(0x100000001), error);
}

/* txn_insert_tests_straddle_inserter is the straddle's inserter: its inserts, one window each, while the checkpoint waits. */
static bool
txn_insert_tests_straddle_inserter(void *captures, HostIoIo *io, HostError *error)
{
	const txn_insert_tests_straddle_t *c = captures;
	HostStorage storage = txn_insert_tests_stored(c->database);
	TxnInsertRoom room = txn_insert_tests_room(c->bytes);
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	bool		ok = true;

	if (!txn_insert_tests_opened(io, storage, &repo, &reading, &held, error))
		return false;
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);

		for (uint64 n = 1; ok && n <= c->inserts; n++)
		{
			TxnInsertInsert row = txn_insert_tests_row(storage, n);
			uint64		tid;

			ok = txn_insert_tests_taken(io, TREES_STORE_READER_NODE_STORE(&store), &held, &row, &room, NULL, NULL, 0,
										&tid, error);
		}
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/*
 * txn_insert_tests_straddled runs a count of inserts of rows of some heap
 * bytes, each a whole window, while a checkpoint waits at a pause of its sweep
 * of their storage, which holds one such row already, then crashes, loads the
 * checkpoint's runs, which hold that one row, and redoes the logged records,
 * and checks every block's free space entry and the counter are as they were
 * before the crash and each row is held once.
 */
static void
txn_insert_tests_straddled(uint32 database, const char *pause, uint64 bytes, uint64 inserts)
{
	static const SimcStep steps[] = {{"inserter", 8, UINT64_MAX}, {"checkpointer", 12, UINT64_MAX}};
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = txn_insert_tests_stored(database);
	txn_insert_tests_straddle_t c = {.database = database, .inserts = inserts, .bytes = bytes};
	SimcActor	checkpointer = {txn_insert_tests_checkpointer};
	SimcActor	inserter = {txn_insert_tests_straddle_inserter, &c};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostSharedHeld deltas;
	TreesStoreReader store;
	txn_insert_tests_free_t expected;
	txn_insert_tests_free_t got;
	SimcRun		run;
	SimcReport	report;
	StdVec		writes;
	HostError	error;

	txn_insert_tests_repository(io, database, 2422, 59, 2423, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, storage, 2426, 99);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, txn_insert_tests_row(storage, 0), bytes,
						  NULL, NULL, 0, 2428, 62);
	host_shared_Held_drop(&held);
	simc_checkpointed(UINT64CONST(0x100000000));
	run = simc_Run_new(1);
	simc_Run_actor(&run, "checkpointer", 12, checkpointer);
	simc_Run_actor(&run, "inserter", 8, inserter);
	simc_Run_until(&run, "checkpointer", 12, pause);
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_vacuum_tests_returned(&report, RUST_FILE, 2442, 67, 2443, 9);
	simc_Report_drop(&report);
	held = txn_insert_tests_stated(io, &repo, &reading, storage, 2446, 99);
	expected = txn_insert_tests_free_entries(io, host_shared_Held_record(&held));
	host_shared_Held_drop(&held);
	if (!txn_state_unlinked(io, &storage, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2449, 33, &error);
	if (!txn_runs_loaded(io, &TXN_INSERT_TESTS_PAGE, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2450, 67, &error);
	held = txn_insert_tests_stated(io, &repo, &reading, storage, 2451, 95);
	{
		StdVec		chains = txn_insert_tests_chains(io, host_shared_Held_record(&held));

		txn_insert_tests_held_once(2453, 9, &chains, 1, NULL);
	}
	if (!txn_memtable_branch_deltas(io, database, HOST_PG_DEFAULT, &deltas, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2454, 65, &error);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	writes = txn_insert_tests_logged(database);
	txn_insert_tests_redone(io, TREES_STORE_READER_NODE_STORE(&store), &held, NULL, &deltas, &writes);
	got = txn_insert_tests_free_entries(io, host_shared_Held_record(&held));
	txn_insert_tests_free_is(2457, 9, &got, &expected, NULL);
	txn_insert_tests_free_drop(&got);
	{
		StdVec		chains = txn_insert_tests_chains(io, host_shared_Held_record(&held));

		txn_insert_tests_held_once(2458, 9, &chains, 1 + inserts, NULL);
	}
	txn_insert_tests_ended(io, 2459, 35);
	host_shared_Held_drop(&deltas);
	host_shared_Held_drop(&held);
	txn_insert_tests_free_drop(&expected);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/*
 * The sweep's order: an insert whose whole window runs between a
 * checkpoint's sweep of its storage's rows and its cut of the free space map
 * leaves the runs its take without its row, under the cut's counter, so redo
 * pushes the row and takes nothing twice.
 */
void
test_txn_insert_tests_a_window_between_a_sweeps_rows_and_its_cut_leaves_its_take_without_its_row(void)
{
	txn_insert_tests_straddled(9330, "doltgres-runs-rows-swept", 28, 1);
}

/*
 * The cut: inserts whose whole windows run after a checkpoint's cut of the
 * free space map, before the cut's walk, are in the runs with neither their
 * rows nor their takes, whatever the walk meets after of the counter's block
 * and of the block the inserts go on to once they leave it, so redo applies
 * each once.
 */
void
test_txn_insert_tests_windows_after_a_sweeps_cut_are_in_its_runs_with_neither_their_rows_nor_their_takes(void)
{
	txn_insert_tests_straddled(9333, "doltgres-runs-free-cut", 3000, 3);
}

/* txn_insert_tests_value_entry returns a value's map entry of 4000 bytes, counted once, by its bytes' address. */
static TxnValuesEntry
txn_insert_tests_value_entry(const char *bytes)
{
	TxnValuesEntry entry = {.raw = 4000, .stored = 4000, .count = 1};

	entry.address = chunks_address_Address_of((const uint8 *) bytes, strlen(bytes));
	entry.locator = chunks_trailer_Locator_Default_default();
	return entry;
}

/* txn_insert_tests_valued_by returns a key's row written by a transaction, naming one value id in the test's TOAST relation. */
static TxnInsertInsert
txn_insert_tests_valued_by(uint64 key, HostXid xid, uint32 id)
{
	TxnInsertInsert row = txn_insert_tests_row(txn_insert_tests_stored(9388), key);

	row.writer.xid = xid.f0;
	std_vec_free_Checked(&row.put.values, sizeof(uint32), _Alignof(uint32));
	std_slice_to_vec_Checked(&row.put.values, &id, 1, sizeof(uint32), _Alignof(uint32));
	row.has_toast = true;
	row.toast = TXN_INSERT_TESTS_TOAST;
	return row;
}

/*
 * An aborted insert's row, its value map entries, and its free space take
 * stay after its take-back beside a committed insert's, for the fold's logged
 * decision to drop, as a standby drops them (bugs row 432).
 */
void
test_txn_insert_tests_an_aborted_inserts_versions_and_takes_stay_until_a_fold(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostSharedHeld toast;
	TreesStoreReader store;
	TxnValuesEntry one = txn_insert_tests_value_entry("one");
	TxnValuesEntry two = txn_insert_tests_value_entry("two");
	HostXid		committed;
	HostXid		aborted;
	txn_insert_tests_mapped_t mapped;
	txn_insert_tests_mapped_t free;
	txn_insert_tests_mapped_t now;
	HostDeltaHold hold;
	HostError	error;
	StdVec		chain;
	uint8		key[8];

	txn_insert_tests_repository(io, 9388, 2486, 87, 2487, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9388), 2488, 106);
	toast = txn_insert_tests_toasted(io, &repo, &reading, 9388, 2490, 102);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	committed = simc_begin();
	aborted = simc_begin();
	txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held,
						  txn_insert_tests_valued_by(1, committed, 16400), 60, &toast, &one, 1, 2505, 82);
	simc_commit(committed);
	txn_insert_tests_ended(io, 2507, 28);
	txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held,
						  txn_insert_tests_valued_by(2, aborted, 16401), 60, &toast, &two, 1, 2512, 83);
	mapped = txn_insert_tests_newest(io, host_shared_Held_record(&toast), TXN_MEMTABLE_MAP_MAP_VALUES, 16401,
									 2513, 69);
	STD_ASSERT(2513, 9, mapped.some,
			   "map::newest(io, toast.record(), Map::Values, 16401).unwrap().is_some()");
	txn_insert_tests_mapped_drop(&mapped);
	free = txn_insert_tests_newest(io, host_shared_Held_record(&held), TXN_MEMTABLE_MAP_MAP_FREE, 0, 2514, 65);
	simc_abort(aborted);
	if (!txn_written_aborted(io, 0, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2516, 30, &error);
	mapped = txn_insert_tests_newest(io, host_shared_Held_record(&toast), TXN_MEMTABLE_MAP_MAP_VALUES, 16401,
									 2517, 72);
	STD_ASSERT(2517, 9, mapped.some, "the aborted insert's value map entry stays");
	txn_insert_tests_mapped_drop(&mapped);
	mapped = txn_insert_tests_newest(io, host_shared_Held_record(&toast), TXN_MEMTABLE_MAP_MAP_VALUES, 16400,
									 2518, 69);
	STD_ASSERT(2518, 9, mapped.some,
			   "map::newest(io, toast.record(), Map::Values, 16400).unwrap().is_some()");
	txn_insert_tests_mapped_drop(&mapped);
	now = txn_insert_tests_newest(io, host_shared_Held_record(&held), TXN_MEMTABLE_MAP_MAP_FREE, 0, 2519, 65);
	STD_ASSERT_EQ(2519, 9, txn_insert_tests_mapped_eq(&now, &free), &now, txn_insert_tests_mapped_Debug_fmt,
				  &free, txn_insert_tests_mapped_Debug_fmt);
	txn_insert_tests_mapped_drop(&now);
	if (!host_delta_hold(io, &hold, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2520, 42, &error);
	std_u64_to_be_bytes(2, key);
	if (!txn_memtable_table_versions(io, &hold, host_shared_Held_record(&held), key, sizeof(key), &chain, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2521, 80, &error);
	STD_ASSERT_EQ_U64(2521, 9, chain.len, 1);
	txn_memtable_version_versions_drop(&chain, 0);
	std_u64_to_be_bytes(1, key);
	if (!txn_memtable_table_versions(io, &hold, host_shared_Held_record(&held), key, sizeof(key), &chain, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2522, 83, &error);
	STD_ASSERT_EQ_U64(2522, 9, chain.len, 1);
	txn_memtable_version_versions_drop(&chain, 0);
	host_delta_Hold_drop(&hold);
	txn_insert_tests_mapped_drop(&free);
	host_shared_Held_drop(&toast);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_op_t is what a version of a key does, by its name, with the tuple ids it names or writes. */
typedef struct txn_insert_tests_op_t
{
	const char *name;
	uint64		at;
} txn_insert_tests_op_t;

/* txn_insert_tests_named returns the tuple id at the end of a successor's key, or 0 for any other successor. */
static uint64
txn_insert_tests_named(bool has, const TxnMemtableVersionSuccessorRef *successor)
{
	if (has && successor->tag == TXN_MEMTABLE_VERSION_SUCCESSOR_REF_KEY && successor->u.key.len > 8)
		return std_u64_from_be_bytes(successor->u.key.ptr + successor->u.key.len - 8);
	return 0;
}

/* txn_insert_tests_ops returns what each version of a key does, newest first, with the tuple ids it names or writes. */
static StdVec
txn_insert_tests_ops(HostIoIo *io, HostSharedRecord record, const uint8 *key, size_t key_len)
{
	HostDeltaHold hold;
	HostError	error;
	StdVec		versions;
	StdVec		ops;

	if (!host_delta_hold(io, &hold, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2528, 42, &error);
	if (!txn_memtable_table_versions(io, &hold, record, key, key_len, &versions, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2529, 64, &error);
	std_vec_with_capacity_Checked(&ops, versions.len, sizeof(txn_insert_tests_op_t), _Alignof(txn_insert_tests_op_t));
	for (size_t k = 0; k < versions.len; k++)
	{
		TxnMemtableVersionRecord got = txn_memtable_version_Version_record(&STD_VEC_AT(&versions,
																					   TxnMemtableVersionVersion, k));
		TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&got);
		txn_insert_tests_op_t one = {"other", 0};

		switch (op.tag)
		{
			case TXN_MEMTABLE_VERSION_OP_REF_PUT:
				one = (txn_insert_tests_op_t)
				{
					"put", op.u.put.tuple.f0
				};
				break;
			case TXN_MEMTABLE_VERSION_OP_REF_ADD:
				one = (txn_insert_tests_op_t)
				{
					"add", op.u.add.tuple.f0
				};
				break;
			case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
				one = (txn_insert_tests_op_t)
				{
					"add row", op.u.add_row.tuple.f0
				};
				break;
			case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
				one = (txn_insert_tests_op_t)
				{
					"update row", op.u.update_row.f0.f0 << 32 | op.u.update_row.f1.tuple.f0
				};
				break;
			case TXN_MEMTABLE_VERSION_OP_REF_MOVE:
				one = (txn_insert_tests_op_t)
				{
					"move", 0
				};
				break;
			case TXN_MEMTABLE_VERSION_OP_REF_REMOVE:
				one = (txn_insert_tests_op_t)
				{
					"remove", op.u.remove.f0.f0 << 32 |
						txn_insert_tests_named(op.u.remove.has_f1, &op.u.remove.f1)
				};
				break;
			case TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW:
				one = (txn_insert_tests_op_t)
				{
					"delete row", op.u.delete_row.f0.f0 << 32 |
						txn_insert_tests_named(op.u.delete_row.has_f1, &op.u.delete_row.f1)
				};
				break;
			default:
				break;
		}
		std_vec_push_Checked(&ops, &one, sizeof(one), _Alignof(txn_insert_tests_op_t));
	}
	txn_memtable_version_versions_drop(&versions, 0);
	host_delta_Hold_drop(&hold);
	return ops;
}

/* txn_insert_tests_op_Debug_fmt is what a version does's Debug, the tuple (name, tuple id). */
static bool
txn_insert_tests_op_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_insert_tests_op_t *op = value;
	StdStr		name = {op->name, strlen(op->name)};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&name, std_str_Debug_fmt), STD_ITEM(&op->at, std_u64_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_insert_tests_ops_Debug_fmt is a Vec<(&str, u64)>'s Debug. */
static bool
txn_insert_tests_ops_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *ops = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < ops->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(ops, txn_insert_tests_op_t, k), txn_insert_tests_op_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_insert_tests_ops_is asserts at a line and column that a key's versions do what another list says, and frees both lists. */
static void
txn_insert_tests_ops_is(uint32 line, uint32 col, StdVec *got, StdVec *want)
{
	bool		eq = got->len == want->len;

	for (size_t k = 0; eq && k < got->len; k++)
	{
		const txn_insert_tests_op_t *a = &STD_VEC_AT(got, txn_insert_tests_op_t, k);
		const txn_insert_tests_op_t *b = &STD_VEC_AT(want, txn_insert_tests_op_t, k);

		eq = a->at == b->at && strcmp(a->name, b->name) == 0;
	}
	STD_ASSERT_EQ(line, col, eq, got, txn_insert_tests_ops_Debug_fmt, want, txn_insert_tests_ops_Debug_fmt);
	std_vec_free_Checked(got, sizeof(txn_insert_tests_op_t), _Alignof(txn_insert_tests_op_t));
	std_vec_free_Checked(want, sizeof(txn_insert_tests_op_t), _Alignof(txn_insert_tests_op_t));
}

/* txn_insert_tests_ops_are asserts at a line and column that a key's versions do what an array says, and frees the list. */
static void
txn_insert_tests_ops_are(uint32 line, uint32 col, StdVec got, const txn_insert_tests_op_t *want, size_t want_len)
{
	StdVec		wanted;

	std_slice_to_vec_Checked(&wanted, want, want_len, sizeof(txn_insert_tests_op_t), _Alignof(txn_insert_tests_op_t));
	txn_insert_tests_ops_is(line, col, &got, &wanted);
}

/* txn_insert_tests_updated_decide is updated's read: on with the command go shows, over the newest, or with none a stop with the count of versions it read. */
static bool
txn_insert_tests_updated_decide(void *captures, HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len,
								TxnChangeRead *result, void *stopped, HostError *error)
{
	const TxnMemtableVersionShown *go = captures;

	if (go == NULL)
	{
		*(size_t *) stopped = versions_len;
		result->tag = TXN_CHANGE_READ_STOP;
		return true;
	}
	result->tag = TXN_CHANGE_READ_GO;
	result->u.go.shown = *go;
	result->u.go.cmin = 0;
	result->u.go.has_row = versions_len > 0;
	if (versions_len > 0)
		result->u.go.row = versions[0].handle;
	return true;
}

/*
 * txn_insert_tests_updated runs an update of a test's row with a test's
 * Waits, each key's stripe by its bytes, and a read that goes on with the
 * command go shows, or with none stops with the count of versions it read,
 * which it writes to stopped. It takes the row and the replaced key.
 */
static TxnInsertInserted
txn_insert_tests_updated(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, TxnInsertInsert row,
						 StdVec key, uint64 tuple, const TxnInsertRoom *room, const TxnInsertUniqueness *unique,
						 const TxnMemtableVersionShown *go, size_t *stopped)
{
	HostIoWaits waits = host_io_Waits_testing();
	TxnInsertReplaces replaces = {.key = key, .tuple = {tuple}, .lock = TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE};
	TxnInsertKeyed keyed = {.stripe = txn_change_stripe(&row.storage, row.key.ptr, row.key.len, true)};
	TxnInsertInserted inserted;
	HostError	error;

	replaces.stripe = txn_change_stripe(&row.storage, key.ptr, key.len, true);
	if (unique != NULL)
	{
		keyed.has_unique = true;
		keyed.unique = *unique;
	}
	if (!txn_insert_update(io, store, held, &row, &replaces, room, NULL, NULL, NULL, 0, keyed, &waits,
						   txn_insert_tests_updated_decide, (void *) go, NULL, stopped, &inserted, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2577, 95, &error);
	return inserted;
}

/* txn_insert_tests_replayed_rows replays a database's logged row changes of a relation on another storage, each by its kind: a put's by the insert's redo, an end's by the change's. */
static void
txn_insert_tests_replayed_rows(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 database,
							   uint32 relation)
{
	StdVec		writes = txn_insert_tests_logged(database);

	for (size_t k = 0; k < writes.len; k++)
	{
		const txn_insert_tests_logged_t *write = &STD_VEC_AT(&writes, txn_insert_tests_logged_t, k);
		const TxnMemtablePut *put;
		HostError	error;

		if (write->tag != TXN_INSERT_TESTS_LOGGED_ROW || write->u.row.storage.relation != relation)
			continue;
		if (txn_record_RowChange_put(&write->u.row, &put))
		{
			if (!txn_insert_redo(io, store, held, &write->u.row, write->end, &TXN_INSERT_TESTS_PAGE, NULL, NULL, NULL,
								 0, &error))
				TXN_INSERT_TESTS_UNWRAP_FAILED(2589, 81, &error);
		}
		else if (!txn_change_redo(io, held, &write->u.row, write->end, &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(2590, 73, &error);
	}
	txn_insert_tests_logged_free(&writes);
}

/* txn_insert_tests_usize_eq reports whether two usizes are equal, a Stopped's T compared. */
static bool
txn_insert_tests_usize_eq(const void *a, const void *b)
{
	return *(const size_t *) a == *(const size_t *) b;
}

/* txn_insert_tests_inserted_is asserts at a line and column that an update did what was wanted, a Stopped's count compared too. */
static void
txn_insert_tests_inserted_is(uint32 line, uint32 col, const TxnInsertInserted *got, size_t got_stopped,
							 const TxnInsertInserted *want, size_t want_stopped)
{
	TxnInsertInsertedRef left = {got, {&got_stopped, std_usize_Debug_fmt}};
	TxnInsertInsertedRef right = {want, {&want_stopped, std_usize_Debug_fmt}};

	STD_ASSERT_EQ(line, col, txn_insert_Inserted_PartialEq_eq(got, &got_stopped, want, &want_stopped,
															  txn_insert_tests_usize_eq),
				  &left, txn_insert_Inserted_Debug_fmt, &right, txn_insert_Inserted_Debug_fmt);
}

/* txn_insert_tests_key returns a test's key by its number, its bytes big-endian. */
static StdVec
txn_insert_tests_key(uint64 n)
{
	uint8		bytes[8];
	StdVec		key;

	std_u64_to_be_bytes(n, bytes);
	std_slice_to_vec_Checked(&key, bytes, sizeof(bytes), 1, 1);
	return key;
}

/* txn_insert_tests_key_ops returns what each version of a test's numbered key does. */
static StdVec
txn_insert_tests_key_ops(HostIoIo *io, const HostSharedHeld *held, uint64 n)
{
	StdVec		key = txn_insert_tests_key(n);
	StdVec		ops = txn_insert_tests_ops(io, host_shared_Held_record(held), key.ptr, key.len);

	std_vec_free_Checked(&key, 1, 1);
	return ops;
}

/* txn_insert_tests_update_made makes the update case's database's repository, a reader, and its storage's state. */
static void
txn_insert_tests_update_made(HostIoIo *io, uint32 database, RepoRepo *repo, HostSharedReader *reading,
							 HostSharedHeld *held)
{
	txn_insert_tests_repository(io, database, 2606, 95, 2607, 50, repo, reading);
	*held = txn_insert_tests_stated(io, repo, reading, txn_insert_tests_stored(database), 2609, 105);
}

/*
 * (4b)'s section 4, an update's forms: one that keeps its key pushes its new
 * row's Put over the row, on the row's block while it has room, with no end
 * of its own; one that moves its row to another key pushes the old key's Move
 * with its own record before the new key's Put; a read that stops pushes and
 * logs nothing and gives back what it stopped with; an immediate key's move
 * onto a live row's key finds it; and the records replayed on another storage
 * give each key's chain as the primary's.
 */
void
test_txn_insert_tests_an_update_keeps_its_key_or_moves_it_and_redoes_as_the_primary(void)
{
	static const TxnMemtableVersionShown command = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	RepoRepo	replica_repo;
	HostSharedReader replica_reading;
	HostSharedHeld replica;
	TreesStoreReader store;
	TreesStoreReader replica_store;
	TxnInsertRoom room = txn_insert_tests_room(28);
	TxnInsertInsert row;
	TxnInsertInserted inserted;
	TxnInsertInserted want = {.tag = TXN_INSERT_INSERTED_TOOK};
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	TxnInsertUniqueness unique = {.order = &order, .index = 16386};
	HostXid		committed;
	uint64		first;
	uint64		other;
	size_t		stopped = 0;
	size_t		records;
	StdVec		writes;
	StdVec		changes;

	txn_insert_tests_update_made(io, 9340, &repo, &reading, &held);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	first = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held,
								  txn_insert_tests_row(txn_insert_tests_stored(9340), 1), 28, NULL, NULL, 0,
								  2616, 81);
	committed = simc_begin();
	simc_commit(committed);
	row = txn_insert_tests_row(txn_insert_tests_stored(9340), 3);
	row.writer.xid = committed.f0;
	other = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, row, 28, NULL, NULL, 0,
								  2620, 102);
	inserted = txn_insert_tests_updated(io, TREES_STORE_READER_NODE_STORE(&store), &held,
										txn_insert_tests_row(txn_insert_tests_stored(9340), 1),
										txn_insert_tests_key(1), first, &room, NULL, &command, &stopped);
	want.u.took.tuple = 3;
	txn_insert_tests_inserted_is(2622, 9, &inserted, stopped, &want, 0);
	{
		txn_insert_tests_op_t ops[] = {{"put", 3}, {"put", first}};

		txn_insert_tests_ops_are(2623, 9, txn_insert_tests_key_ops(io, &held, 1), ops, lengthof(ops));
	}
	writes = txn_insert_tests_logged(9340);
	records = writes.len;
	txn_insert_tests_logged_free(&writes);
	inserted = txn_insert_tests_updated(io, TREES_STORE_READER_NODE_STORE(&store), &held,
										txn_insert_tests_row(txn_insert_tests_stored(9340), 2),
										txn_insert_tests_key(1), 3, &room, NULL, NULL, &stopped);
	want.tag = TXN_INSERT_INSERTED_STOPPED;
	txn_insert_tests_inserted_is(2626, 9, &inserted, stopped, &want, 2);
	writes = txn_insert_tests_logged(9340);
	STD_ASSERT_EQ_U64(2627, 9, writes.len, records);
	txn_insert_tests_logged_free(&writes);
	inserted = txn_insert_tests_updated(io, TREES_STORE_READER_NODE_STORE(&store), &held,
										txn_insert_tests_row(txn_insert_tests_stored(9340), 2),
										txn_insert_tests_key(1), 3, &room, NULL, &command, &stopped);
	want.tag = TXN_INSERT_INSERTED_TOOK;
	want.u.took.tuple = 4;
	txn_insert_tests_inserted_is(2629, 9, &inserted, stopped, &want, 0);
	{
		txn_insert_tests_op_t moved[] = {{"move", 0}, {"put", 3}, {"put", first}};
		txn_insert_tests_op_t put[] = {{"put", 4}};

		txn_insert_tests_ops_are(2630, 9, txn_insert_tests_key_ops(io, &held, 1), moved, lengthof(moved));
		txn_insert_tests_ops_are(2631, 9, txn_insert_tests_key_ops(io, &held, 2), put, lengthof(put));
	}
	writes = txn_insert_tests_logged(9340);
	std_vec_new(&changes, sizeof(const TxnRecordRowChange *), _Alignof(const TxnRecordRowChange *));
	for (size_t k = records; k < writes.len; k++)
	{
		const txn_insert_tests_logged_t *write = &STD_VEC_AT(&writes, txn_insert_tests_logged_t, k);
		const TxnRecordRowChange *change = &write->u.row;

		if (write->tag == TXN_INSERT_TESTS_LOGGED_ROW)
			std_vec_push_Checked(&changes, &change, sizeof(change), _Alignof(const TxnRecordRowChange *));
	}
	if (changes.len != 2)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" row changes")};
		StdFmtArg	args[] = {{&changes.len, std_usize_Display_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 2640, 71, &message);
	}
	{
		const TxnRecordRowChange *ended = STD_VEC_AT(&changes, const TxnRecordRowChange *, 0);
		const TxnRecordRowChange *put = STD_VEC_AT(&changes, const TxnRecordRowChange *, 1);
		StdVec		one = txn_insert_tests_key(1);
		StdVec		two = txn_insert_tests_key(2);
		uint8		four[8];
		TxnMemtableOp end = {.tag = TXN_MEMTABLE_OP_MOVE, .u.move = {.tag = TXN_MEMTABLE_SUCCESSOR_KEY}};
		bool		yes = true;
		StdBytes	ended_key = {ended->key.ptr, ended->key.len};
		StdBytes	one_key = {one.ptr, one.len};
		StdBytes	put_key = {put->key.ptr, put->key.len};
		StdBytes	two_key = {two.ptr, two.len};

		std_vec_clone(&end.u.move.u.key, &two, 1, 1);
		std_u64_to_be_bytes(4, four);
		std_vec_extend_from_slice_Checked(&end.u.move.u.key, four, sizeof(four), 1, 1);
		{
			StdTupleRef got = STD_TUPLE(3, STD_ITEM(&ended_key, std_bytes_Debug_fmt),
										STD_ITEM(&ended->op, txn_memtable_Op_Debug_fmt),
										STD_ITEM(&ended->no_key, std_bool_Debug_fmt));
			StdTupleRef wanted = STD_TUPLE(3, STD_ITEM(&one_key, std_bytes_Debug_fmt),
										   STD_ITEM(&end, txn_memtable_Op_Debug_fmt),
										   STD_ITEM(&yes, std_bool_Debug_fmt));

			STD_ASSERT_EQ(2642, 9, trees_store_VecU8_PartialEq_eq(&ended->key, &one) &&
						  txn_memtable_Op_PartialEq_eq(&ended->op, &end) && ended->no_key, &got, std_tuple_Debug_fmt,
						  &wanted, std_tuple_Debug_fmt);
		}
		STD_ASSERT_EQ(2643, 9, trees_store_VecU8_PartialEq_eq(&put->key, &two), &put_key,
					  std_bytes_Debug_fmt, &two_key, std_bytes_Debug_fmt);
		txn_memtable_Op_drop(&end);
		std_vec_free_Checked(&two, 1, 1);
		std_vec_free_Checked(&one, 1, 1);
	}
	std_vec_free_Checked(&changes, sizeof(const TxnRecordRowChange *), _Alignof(const TxnRecordRowChange *));
	txn_insert_tests_logged_free(&writes);
	inserted = txn_insert_tests_updated(io, TREES_STORE_READER_NODE_STORE(&store), &held,
										txn_insert_tests_row(txn_insert_tests_stored(9340), 3),
										txn_insert_tests_key(2), 4, &room, &unique, &command, &stopped);
	STD_ASSERT(2647, 9, inserted.tag == TXN_INSERT_INSERTED_EXISTS, "matches!(onto, Inserted::Exists(..))");
	{
		txn_insert_tests_op_t ops[] = {{"put", other}};

		txn_insert_tests_ops_are(2648, 9, txn_insert_tests_key_ops(io, &held, 3), ops, lengthof(ops));
	}
	txn_insert_tests_update_made(io, 9341, &replica_repo, &replica_reading, &replica);
	host_shared_Reader_drop(&replica_reading);
	replica_store = repo_Repo_reader(&replica_repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	txn_insert_tests_replayed_rows(io, TREES_STORE_READER_NODE_STORE(&replica_store), &replica, 9340, 16384);
	for (uint64 n = 1; n <= 3; n++)
	{
		StdVec		got = txn_insert_tests_key_ops(io, &replica, n);
		StdVec		primary = txn_insert_tests_key_ops(io, &held, n);

		txn_insert_tests_ops_is(2653, 13, &got, &primary);
	}
	txn_insert_tests_ended(io, 2655, 35);
	host_shared_Held_drop(&replica);
	repo_Repo_drop(&replica_repo);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_made_in returns the state of one of a database's relations' storages, unwrapped where the keyless case's made unwraps it. */
static HostSharedHeld
txn_insert_tests_made_in(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, uint32 database,
						 uint32 relation)
{
	HostStorage storage = txn_insert_tests_stored(database);

	storage.relation = relation;
	return txn_insert_tests_stated(io, repo, reading, storage, 2671, 93);
}

/* txn_insert_tests_copy returns a keyless table's copy of a row by its number. */
static TxnInsertInsert
txn_insert_tests_copy(uint64 n)
{
	TxnInsertInsert row = txn_insert_tests_row(txn_insert_tests_stored(9342), n);

	row.keyless = true;
	return row;
}

/* txn_insert_tests_rowed returns a deferrable key's row of relation 16390 by its number. */
static TxnInsertInsert
txn_insert_tests_rowed(uint64 n)
{
	HostStorage storage = txn_insert_tests_stored(9342);
	TxnInsertInsert row;

	storage.relation = 16390;
	row = txn_insert_tests_row(storage, n);
	row.deferred = true;
	return row;
}

/* txn_insert_tests_took_is asserts at a line and column that an update took a tuple id. */
static void
txn_insert_tests_took_is(uint32 line, uint32 col, const TxnInsertInserted *got, size_t stopped, uint64 tid)
{
	TxnInsertInserted want = {.tag = TXN_INSERT_INSERTED_TOOK, .u.took.tuple = tid};

	txn_insert_tests_inserted_is(line, col, got, stopped, &want, 0);
}

/*
 * (4b)'s sections 4 and 4a, the row-addressed forms: a keyless table's
 * update removes the copy it replaces, naming the new copy's tuple id, then
 * adds the new copy to the same key; a deferrable key's update that keeps its
 * key replaces its row by its tuple id, and one that moves it deletes the row
 * by its tuple id, naming the new row, and adds the new row to the new key;
 * each replayed as the primary's.
 */
void
test_txn_insert_tests_a_keyless_or_deferrable_update_names_its_rows_by_their_tuple_ids(void)
{
	static const TxnMemtableVersionShown command = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	static const uint32 relations[] = {16384, 16390};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	RepoRepo	replica_repo;
	HostSharedReader reading;
	HostSharedHeld keyless;
	HostSharedHeld deferred;
	TreesStoreReader store;
	TreesStoreReader replica_store;
	TxnInsertRoom room = txn_insert_tests_room(28);
	TxnInsertInserted took;
	ChunksErrorError failed;
	size_t		stopped = 0;
	uint64		first;

	txn_insert_tests_repository(io, 9342, 2666, 87, 2667, 46, &repo, &reading);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	keyless = txn_insert_tests_made_in(io, &repo, &reading, 9342, 16384);
	deferred = txn_insert_tests_made_in(io, &repo, &reading, 9342, 16390);
	first = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &keyless, txn_insert_tests_copy(1), 28,
								  NULL, NULL, 0, 2677, 71);
	took = txn_insert_tests_updated(io, TREES_STORE_READER_NODE_STORE(&store), &keyless, txn_insert_tests_copy(1),
									txn_insert_tests_key(1), first, &room, NULL, &command, &stopped);
	txn_insert_tests_took_is(2679, 9, &took, stopped, 2);
	{
		txn_insert_tests_op_t ops[] = {{"add", 2}, {"remove", first << 32 | 2}, {"add", first}};

		txn_insert_tests_ops_are(2680, 9, txn_insert_tests_key_ops(io, &keyless, 1), ops, lengthof(ops));
	}
	first = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &deferred, txn_insert_tests_rowed(1), 28,
								  NULL, NULL, 0, 2682, 73);
	took = txn_insert_tests_updated(io, TREES_STORE_READER_NODE_STORE(&store), &deferred, txn_insert_tests_rowed(1),
									txn_insert_tests_key(1), first, &room, NULL, &command, &stopped);
	txn_insert_tests_took_is(2684, 9, &took, stopped, 2);
	{
		txn_insert_tests_op_t ops[] = {{"update row", first << 32 | 2}, {"add row", first}};

		txn_insert_tests_ops_are(2685, 9, txn_insert_tests_key_ops(io, &deferred, 1), ops, lengthof(ops));
	}
	took = txn_insert_tests_updated(io, TREES_STORE_READER_NODE_STORE(&store), &deferred, txn_insert_tests_rowed(2),
									txn_insert_tests_key(1), 2, &room, NULL, &command, &stopped);
	txn_insert_tests_took_is(2687, 9, &took, stopped, 3);
	{
		StdVec		ops = txn_insert_tests_key_ops(io, &deferred, 1);
		txn_insert_tests_op_t want = {"delete row", UINT64CONST(2) << 32 | 3};
		const txn_insert_tests_op_t *got = &STD_VEC_AT(&ops, txn_insert_tests_op_t, 0);

		if (ops.len == 0)
			std_index_check(0, 0, RUST_FILE, 2688, 20);
		STD_ASSERT_EQ(2688, 9, got->at == want.at && strcmp(got->name, want.name) == 0, got,
					  txn_insert_tests_op_Debug_fmt, &want, txn_insert_tests_op_Debug_fmt);
		std_vec_free_Checked(&ops, sizeof(txn_insert_tests_op_t), _Alignof(txn_insert_tests_op_t));
	}
	{
		txn_insert_tests_op_t ops[] = {{"add row", 3}};

		txn_insert_tests_ops_are(2689, 9, txn_insert_tests_key_ops(io, &deferred, 2), ops, lengthof(ops));
	}
	if (!repo_Repo_create(io, 9343, HOST_PG_DEFAULT, &replica_repo, &failed))
		STD_RESULT_EXPECT_FAILED(2690, 95, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	replica_store = repo_Repo_reader(&replica_repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	for (size_t r = 0; r < lengthof(relations); r++)
	{
		const HostSharedHeld *primary = r == 0 ? &keyless : &deferred;
		HostSharedHeld replica = txn_insert_tests_made_in(io, &replica_repo, &reading, 9343, relations[r]);

		txn_insert_tests_replayed_rows(io, TREES_STORE_READER_NODE_STORE(&replica_store), &replica, 9342,
									   relations[r]);
		for (uint64 n = 1; n <= 2; n++)
		{
			StdVec		got = txn_insert_tests_key_ops(io, &replica, n);
			StdVec		want = txn_insert_tests_key_ops(io, primary, n);

			txn_insert_tests_ops_is(2696, 17, &got, &want);
		}
		host_shared_Held_drop(&replica);
	}
	txn_insert_tests_ended(io, 2699, 35);
	repo_Repo_drop(&replica_repo);
	host_shared_Held_drop(&deferred);
	host_shared_Held_drop(&keyless);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_shown_t is what a version shows: its raw command id and the command id that ended it. */
typedef struct txn_insert_tests_shown_t
{
	uint32		raw;
	uint32		ended;
} txn_insert_tests_shown_t;

/* txn_insert_tests_shown_Debug_fmt is what a version shows's Debug, the tuple (raw, ended). */
static bool
txn_insert_tests_shown_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_insert_tests_shown_t *shown = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&shown->raw, std_u32_Debug_fmt),
								  STD_ITEM(&shown->ended, std_u32_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_insert_tests_showns_Debug_fmt is a Vec<(u32, u32)>'s Debug. */
static bool
txn_insert_tests_showns_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *shows = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < shows->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(shows, txn_insert_tests_shown_t, k), txn_insert_tests_shown_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_insert_tests_shows returns what each version of a test's numbered key shows, newest first. */
static StdVec
txn_insert_tests_shows(HostIoIo *io, const HostSharedHeld *held, uint64 n)
{
	StdVec		key = txn_insert_tests_key(n);
	HostDeltaHold hold;
	HostError	error;
	StdVec		versions;
	StdVec		shows;

	if (!host_delta_hold(io, &hold, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2716, 46, &error);
	if (!txn_memtable_table_versions(io, &hold, host_shared_Held_record(held), key.ptr, key.len, &versions, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2717, 79, &error);
	std_vec_with_capacity_Checked(&shows, versions.len, sizeof(txn_insert_tests_shown_t),
								  _Alignof(txn_insert_tests_shown_t));
	for (size_t k = 0; k < versions.len; k++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&STD_VEC_AT(&versions,
																						  TxnMemtableVersionVersion,
																						  k));
		txn_insert_tests_shown_t shown = {txn_memtable_version_Record_raw(&record),
		txn_memtable_version_Record_ended(&record)};

		std_vec_push_Checked(&shows, &shown, sizeof(shown), _Alignof(txn_insert_tests_shown_t));
	}
	txn_memtable_version_versions_drop(&versions, 0);
	host_delta_Hold_drop(&hold);
	std_vec_free_Checked(&key, 1, 1);
	return shows;
}

/* txn_insert_tests_shows_are asserts at a line and column that what a key's versions show is what was wanted, and frees the list. */
static void
txn_insert_tests_shows_are(uint32 line, uint32 col, StdVec shows, const txn_insert_tests_shown_t *want,
						   size_t want_len)
{
	StdVec		wanted = {(void *) want, want_len, want_len};
	bool		eq = shows.len == want_len &&
		memcmp(shows.ptr, want, want_len * sizeof(txn_insert_tests_shown_t)) == 0;

	STD_ASSERT_EQ(line, col, eq, &shows, txn_insert_tests_showns_Debug_fmt, &wanted,
				  txn_insert_tests_showns_Debug_fmt);
	std_vec_free_Checked(&shows, sizeof(txn_insert_tests_shown_t), _Alignof(txn_insert_tests_shown_t));
}

/*
 * (4b)'s section 6 and CommPF 7's ruling on a same-key update: an update of a
 * row its own transaction wrote shows the combo command id its read gave on
 * the row it ends, the new row's Put showing its own command for its own row
 * and the combo id for the row it ended, and a move's end showing the combo
 * id as its own.
 */
void
test_txn_insert_tests_an_updates_combo_command_shows_on_the_row_it_ends(void)
{
	static const txn_insert_tests_shown_t kept[] = {{1, 7}, {0, 0}};
	static const txn_insert_tests_shown_t both = {8, 8};
	static const txn_insert_tests_shown_t moved[] = {{2, 2}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TreesStoreReader store;
	TxnInsertRoom room = txn_insert_tests_room(28);
	TxnMemtableVersionShown combo = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMBO, .u.combo = 7};
	TxnInsertInsert row;
	TxnInsertInserted took;
	size_t		stopped = 0;
	uint64		first;
	uint64		second;

	txn_insert_tests_repository(io, 9344, 2709, 87, 2710, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9344), 2711, 106);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	first = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held,
								  txn_insert_tests_row(txn_insert_tests_stored(9344), 1), 28, NULL, NULL, 0,
								  2720, 81);
	row = txn_insert_tests_row(txn_insert_tests_stored(9344), 1);
	row.writer.command = 1;
	took = txn_insert_tests_updated(io, TREES_STORE_READER_NODE_STORE(&store), &held, row, txn_insert_tests_key(1),
									first, &room, NULL, &combo, &stopped);
	if (took.tag != TXN_INSERT_INSERTED_TOOK)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		TxnInsertInsertedRef shown = {&took, {&stopped, std_usize_Debug_fmt}};
		StdFmtArg	args[] = {{&shown, txn_insert_Inserted_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 2723, 50, &message);
	}
	second = took.u.took.tuple;
	txn_insert_tests_shows_are(2724, 9, txn_insert_tests_shows(io, &held, 1), kept, lengthof(kept));
	row = txn_insert_tests_row(txn_insert_tests_stored(9344), 2);
	row.writer.command = 2;
	combo.u.combo = 8;
	took = txn_insert_tests_updated(io, TREES_STORE_READER_NODE_STORE(&store), &held, row, txn_insert_tests_key(1),
									second, &room, NULL, &combo, &stopped);
	STD_ASSERT(2727, 9, took.tag == TXN_INSERT_INSERTED_TOOK, "matches!(took, Inserted::Took(_))");
	{
		StdVec		shows = txn_insert_tests_shows(io, &held, 1);

		const txn_insert_tests_shown_t *got;

		std_index_check(0, shows.len, RUST_FILE, 2728, 20);
		got = &STD_VEC_AT(&shows, txn_insert_tests_shown_t, 0);
		STD_ASSERT_EQ(2728, 9, got->raw == both.raw && got->ended == both.ended, got,
					  txn_insert_tests_shown_Debug_fmt, &both, txn_insert_tests_shown_Debug_fmt);
		std_vec_free_Checked(&shows, sizeof(txn_insert_tests_shown_t), _Alignof(txn_insert_tests_shown_t));
	}
	txn_insert_tests_shows_are(2729, 9, txn_insert_tests_shows(io, &held, 2), moved, lengthof(moved));
	txn_insert_tests_ended(io, 2730, 35);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_token_is asserts at a line and column that a key's newest version holds the token wanted, or none. */
static void
txn_insert_tests_token_is(uint32 line, uint32 col, const StdVec *versions, uint32 index_line, uint32 index_col,
						  bool has_want, uint32 want)
{
	uint32		token = 0;
	bool		has;
	StdOptionRef got;
	StdOptionRef wanted = {has_want, {&want, std_u32_Debug_fmt}};

	std_index_check(0, versions->len, RUST_FILE, index_line, index_col);
	has = txn_memtable_version_Version_token(&STD_VEC_AT(versions, TxnMemtableVersionVersion, 0), &token);
	got = (StdOptionRef)
	{
		has,
		{
			&token, std_u32_Debug_fmt
		}
	};
	STD_ASSERT_EQ(line, col, has == has_want && (!has || token == want), &got, std_option_Debug_fmt, &wanted,
				  std_option_Debug_fmt);
}

/* txn_insert_tests_completion returns the completion of a speculative insert of a key at a tuple id, written by a writer. */
static TxnRecordCompletion
txn_insert_tests_completion(TxnMemtableVersionWriter writer, uint64 key, uint64 tid)
{
	TxnRecordCompletion completion = {.writer = writer, .storage = txn_insert_tests_stored(9345), .tuple = {tid}};

	completion.key = txn_insert_tests_key(key);
	return completion;
}

/* txn_insert_tests_completed runs a speculative insert's completion, logged or not, unwrapped at a line and column, and answers it. */
static bool
txn_insert_tests_completed(HostIoIo *io, const HostSharedHeld *held, TxnRecordCompletion completion,
						   TxnInsertCompleted completed, bool logged, uint32 line, uint32 col)
{
	HostError	error;
	bool		result;

	if (!txn_insert_complete(io, held, &completion, completed, logged, &result, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(line, col, &error);
	std_vec_free_Checked(&completion.key, 1, 1);
	return result;
}

/* txn_insert_tests_bools_Debug_fmt is a Vec<bool>'s Debug. */
static bool
txn_insert_tests_bools_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *bools = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < bools->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(bools, bool, k), std_bool_Debug_fmt);
	return std_DebugList_finish(&list);
}

/*
 * (4b)'s speculative insert: an insert with a token pushes its version
 * holding the token, which a Dirty read of the key reports with the running
 * writer, and the insert's confirm clears it; a killed one is left out of
 * what a checkpoint's runs write.
 */
void
test_txn_insert_tests_a_speculative_insert_holds_its_token_until_confirmed(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TreesStoreReader store;
	HostXid		running;
	TxnMemtableVersionWriter writer;
	TxnInsertInsert row;
	uint64		first;
	uint64		second;
	HostDeltaHold hold;
	StdVec		versions;
	SimcTaken	dirty;
	HostSnapshot lent;
	TxnVisibleRule rule;
	TxnVisibleSeen seen;
	HostError	error;
	StdVec		writes;
	StdVec		killed;
	StdVec		chains;
	uint8		one[8];

	txn_insert_tests_repository(io, 9345, 2740, 87, 2741, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9345), 2742, 106);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	running = simc_begin();
	writer = (TxnMemtableVersionWriter)
	{
		.xid = running.f0
	};
	row = txn_insert_tests_row(txn_insert_tests_stored(9345), 1);
	row.writer = writer;
	row.has_token = true;
	row.token = 5;
	first = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, row, 28, NULL, NULL, 0,
								  2748, 110);
	if (!host_delta_hold(io, &hold, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2749, 42, &error);
	std_u64_to_be_bytes(1, one);
	if (!txn_memtable_table_versions(io, &hold, host_shared_Held_record(&held), one, sizeof(one), &versions, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2750, 87, &error);
	txn_insert_tests_token_is(2751, 9, &versions, 2751, 20, true, 5);
	dirty = simc_snapshot(HOST_SNAPSHOT_KIND_DIRTY, false);
	lent = simc_Taken_lent(&dirty);
	rule.ops = &TXN_VISIBLE_SNAPSHOT_RULE_OPS;
	rule.self = &lent;
	if (!txn_visible_seen(io, rule, versions.ptr, versions.len, NULL, &seen, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2753, 70, &error);
	std_index_check(0, seen.rows.len, RUST_FILE, 2754, 21);
	{
		const TxnVisibleDirty *found = &STD_VEC_AT(&seen.rows, TxnVisibleSeenRow, 0).dirty;
		uint32		five = 5;
		StdOptionRef xmin = {found->has_xmin, {&found->xmin, host_Xid_Debug_fmt}};
		StdOptionRef token = {found->has_token, {&found->token, std_u32_Debug_fmt}};
		StdOptionRef want_xmin = {true, {&running, host_Xid_Debug_fmt}};
		StdOptionRef want_token = {true, {&five, std_u32_Debug_fmt}};
		StdTupleRef got = STD_TUPLE(2, STD_ITEM(&xmin, std_option_Debug_fmt), STD_ITEM(&token, std_option_Debug_fmt));
		StdTupleRef want = STD_TUPLE(2, STD_ITEM(&want_xmin, std_option_Debug_fmt),
									 STD_ITEM(&want_token, std_option_Debug_fmt));

		STD_ASSERT_EQ(2754, 9, found->has_xmin && found->xmin.f0 == running.f0 && found->has_token &&
					  found->token == 5, &got, std_tuple_Debug_fmt, &want, std_tuple_Debug_fmt);
	}
	STD_ASSERT(2761, 9, txn_insert_tests_completed(io, &held, txn_insert_tests_completion(writer, 1, first),
												   TXN_INSERT_COMPLETED_CONFIRMED, true, 2761, 90),
			   "complete(io, &held, &completion(1, first), (Completed::Confirmed, true)).unwrap()");
	txn_insert_tests_token_is(2762, 9, &versions, 2762, 20, false, 0);
	txn_visible_Seen_drop(&seen);
	txn_memtable_version_versions_drop(&versions, 0);
	host_delta_Hold_drop(&hold);
	row = txn_insert_tests_row(txn_insert_tests_stored(9345), 2);
	row.writer = writer;
	row.has_token = true;
	row.token = 6;
	second = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, row, 28, NULL, NULL, 0,
								   2766, 110);
	STD_ASSERT(2767, 9, txn_insert_tests_completed(io, &held, txn_insert_tests_completion(writer, 2, second),
												   TXN_INSERT_COMPLETED_KILLED, true, 2767, 88),
			   "complete(io, &held, &completion(2, second), (Completed::Killed, true)).unwrap()");
	STD_ASSERT(2768, 9, !txn_insert_tests_completed(io, &held, txn_insert_tests_completion(writer, 3, second),
													TXN_INSERT_COMPLETED_KILLED, false, 2768, 90),
			   "!complete(io, &held, &completion(3, second), (Completed::Killed, false)).unwrap()");
	writes = txn_insert_tests_logged(9345);
	std_vec_new(&killed, sizeof(bool), _Alignof(bool));
	for (size_t k = 0; k < writes.len; k++)
	{
		const txn_insert_tests_logged_t *write = &STD_VEC_AT(&writes, txn_insert_tests_logged_t, k);

		if (write->tag == TXN_INSERT_TESTS_LOGGED_ROW)
			std_vec_push_Checked(&killed, &write->u.row.speculative, sizeof(bool), _Alignof(bool));
	}
	{
		static const bool both[] = {true, true};
		StdVec		wanted = {(void *) both, 2, 2};
		bool		eq = killed.len == 2 && STD_VEC_AT(&killed, bool, 0) && STD_VEC_AT(&killed, bool, 1);

		STD_ASSERT_EQ(2773, 9, eq, &killed, txn_insert_tests_bools_Debug_fmt, &wanted,
					  txn_insert_tests_bools_Debug_fmt);
	}
	std_vec_free_Checked(&killed, sizeof(bool), _Alignof(bool));
	txn_insert_tests_logged_free(&writes);
	chains = txn_insert_tests_chains(io, host_shared_Held_record(&held));
	{
		txn_insert_tests_chain_t want = {.key = {one, sizeof(one), sizeof(one)}, .versions = 1};
		StdVec		wanted = {&want, 1, 1};

		STD_ASSERT_EQ(2774, 9, txn_insert_tests_chains_eq(&chains, &wanted), &chains, txn_insert_tests_chains_Debug_fmt, &wanted,
					  txn_insert_tests_chains_Debug_fmt);
	}
	txn_insert_tests_chains_free(&chains);
	txn_insert_tests_ended(io, 2775, 35);
	simc_Taken_drop(&dirty);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/*
 * An aborted update that moves its row stays whole after its take-back: the
 * new key's put and the old key's move over the committed row's put, for the
 * fold's logged decision to drop (bugs row 432).
 */
void
test_txn_insert_tests_an_aborted_updates_new_row_and_end_stay_until_a_fold(void)
{
	static const TxnMemtableVersionShown command = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TreesStoreReader store;
	TxnInsertRoom room = txn_insert_tests_room(28);
	TxnInsertInsert row;
	TxnInsertInserted moved;
	HostXid		committed;
	HostXid		aborted;
	HostError	error;
	size_t		stopped = 0;
	uint64		first;

	txn_insert_tests_repository(io, 9389, 2785, 87, 2786, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, txn_insert_tests_stored(9389), 2787, 106);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	committed = simc_begin();
	aborted = simc_begin();
	row = txn_insert_tests_row(txn_insert_tests_stored(9389), 1);
	row.writer.xid = committed.f0;
	first = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, row, 28, NULL, NULL, 0,
								  2792, 66);
	simc_commit(committed);
	txn_insert_tests_ended(io, 2794, 28);
	row = txn_insert_tests_row(txn_insert_tests_stored(9389), 2);
	row.writer.xid = aborted.f0;
	moved = txn_insert_tests_updated(io, TREES_STORE_READER_NODE_STORE(&store), &held, row, txn_insert_tests_key(1),
									 first, &room, NULL, &command, &stopped);
	STD_ASSERT(2798, 9, moved.tag == TXN_INSERT_INSERTED_TOOK, "matches!(moved, Inserted::Took(_))");
	{
		StdVec		ops = txn_insert_tests_key_ops(io, &held, 1);
		StdStr		got;
		StdStr		want = {"move", 4};

		std_index_check(0, ops.len, RUST_FILE, 2799, 20);
		got = (StdStr)
		{
			STD_VEC_AT(&ops, txn_insert_tests_op_t, 0).name,
				strlen(STD_VEC_AT(&ops, txn_insert_tests_op_t, 0).name)
		};
		STD_ASSERT_EQ(2799, 9, strcmp(got.ptr, "move") == 0, &got, std_str_Debug_fmt, &want,
					  std_str_Debug_fmt);
		std_vec_free_Checked(&ops, sizeof(txn_insert_tests_op_t), _Alignof(txn_insert_tests_op_t));
	}
	simc_abort(aborted);
	if (!txn_written_aborted(io, 0, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2801, 30, &error);
	{
		txn_insert_tests_op_t ended[] = {{"move", 0}, {"put", first}};
		txn_insert_tests_op_t put[] = {{"put", moved.u.took.tuple}};

		txn_insert_tests_ops_are(2802, 9, txn_insert_tests_key_ops(io, &held, 1), ended, lengthof(ended));
		txn_insert_tests_ops_are(2803, 9, txn_insert_tests_key_ops(io, &held, 2), put, lengthof(put));
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_Folded_Equal_equal is Folded's Equal: keys are equal when their bytes are. */
static bool
txn_insert_tests_Folded_Equal_equal(void *self, HostIoIo *io, const uint8 *a, size_t a_len, const uint8 *b, size_t b_len,
									bool *result, HostError *error)
{
	*result = a_len == b_len && memcmp(a, b, a_len) == 0;
	return true;
}

/*
 * TXN_INSERT_TESTS_FOLDED_OPS is Folded's Equal, a test's class equality
 * that calls keys equal when their bytes are, so a resolve takes the path of
 * a key whose class compares by a function, its stripe the storage's, with no
 * key equal to another.
 */
static const TxnInsertEqualOps TXN_INSERT_TESTS_FOLDED_OPS = {txn_insert_tests_Folded_Equal_equal};

/* txn_insert_tests_resolver is the stripe case's resolver: an immediate key's insert under the storage's stripe takes (0,4). */
static bool
txn_insert_tests_resolver(void *captures, HostIoIo *io, HostError *error)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("the resolve gave "), STD_FMT_ARG(0)};
	HostStorage storage = txn_insert_tests_stored(9390);
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	TxnInsertRoom room = txn_insert_tests_room(28);
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TxnInsertInserted inserted;
	bool		ok;

	if (!txn_insert_tests_opened(io, storage, &repo, &reading, &held, error))
		return false;
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
		TxnInsertKeyed keyed = {.stripe = txn_change_stripe(&storage, NULL, 0, false), .has_unique = true,
		.unique = {.order = &order, .index = 16386, .has_equal = true, .equal = {&TXN_INSERT_TESTS_FOLDED_OPS, NULL}}};
		HostIoWaits waits = host_io_Waits_testing();
		TxnInsertInsert row = txn_insert_tests_row(storage, 100);

		ok = txn_insert_insert(io, TREES_STORE_READER_NODE_STORE(&store), &held, &row, &room, NULL, NULL, NULL, 0,
							   keyed, &waits, &inserted, error);
	}
	if (ok && !(inserted.tag == TXN_INSERT_INSERTED_TOOK && inserted.u.took.tuple == 4))
	{
		TxnInsertInsertedRef other = {&inserted, {NULL, std_unit_Debug_fmt}};
		StdFmtArg	args[] = {{&other, txn_insert_Inserted_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		*error = txn_insert_tests_gathering_fork(&arguments);
		ok = false;
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/* txn_insert_tests_inserteds_Debug_fmt is a Vec<Inserted<()>>'s Debug. */
static bool
txn_insert_tests_inserteds_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *inserteds = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < inserteds->len; k++)
	{
		TxnInsertInsertedRef one = {&STD_VEC_AT(inserteds, TxnInsertInserted, k), {NULL, std_unit_Debug_fmt}};

		std_DebugList_entry(&list, &one, txn_insert_Inserted_Debug_fmt);
	}
	return std_DebugList_finish(&list);
}

/* txn_insert_tests_pusher is the stripe case's pusher: four keyless-striped inserts, the last after the resolve's. */
static bool
txn_insert_tests_pusher(void *captures, HostIoIo *io, HostError *error)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("the pushes gave "), STD_FMT_ARG(0)};
	static const uint64 tids[] = {1, 2, 3, 5};
	HostStorage storage = txn_insert_tests_stored(9390);
	TxnInsertRoom room = txn_insert_tests_room(28);
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	StdVec		took;
	bool		ok = true;

	if (!txn_insert_tests_opened(io, storage, &repo, &reading, &held, error))
		return false;
	std_vec_new(&took, sizeof(TxnInsertInserted), _Alignof(TxnInsertInserted));
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
		HostIoWaits waits = host_io_Waits_testing();

		for (uint64 n = 1; ok && n <= 4; n++)
		{
			TxnInsertKeyed keyed = {.stripe = txn_change_stripe(&storage, NULL, 0, false)};
			TxnInsertInsert row = txn_insert_tests_row(storage, n);
			TxnInsertInserted inserted;

			ok = txn_insert_insert(io, TREES_STORE_READER_NODE_STORE(&store), &held, &row, &room, NULL, NULL, NULL, 0,
								   keyed, &waits, &inserted, error);
			if (ok)
				std_vec_push_Checked(&took, &inserted, sizeof(inserted), _Alignof(TxnInsertInserted));
		}
	}
	if (ok)
	{
		bool		eq = took.len == lengthof(tids);

		for (size_t k = 0; eq && k < took.len; k++)
			eq = STD_VEC_AT(&took, TxnInsertInserted, k).tag == TXN_INSERT_INSERTED_TOOK &&
				STD_VEC_AT(&took, TxnInsertInserted, k).u.took.tuple == tids[k];
		if (!eq)
		{
			StdFmtArg	args[] = {{&took, txn_insert_tests_inserteds_Debug_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			*error = txn_insert_tests_gathering_fork(&arguments);
			ok = false;
		}
	}
	std_vec_free_Checked(&took, sizeof(TxnInsertInserted), _Alignof(TxnInsertInserted));
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/*
 * (4b)'s section 3's hold-back flag: a resolve over its storage's stripe,
 * held at its read while another writer of the storage pushes a row three
 * times, starts over three times and holds the stripe back at the third, so
 * that writer's next insert sleeps before its read until the resolve
 * finishes, within 3 plus 2 rounds, and takes the tuple id after the
 * resolve's.
 */
void
test_txn_insert_tests_a_resolve_whose_stripe_moves_three_times_holds_its_writers_back(void)
{
	static const SimcStep steps[] = {{"pusher", 6, UINT64_MAX}, {"resolver", 8, UINT64_MAX}, {"pusher", 6, UINT64_MAX}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcActor	resolver = {txn_insert_tests_resolver};
	SimcActor	pusher = {txn_insert_tests_pusher};
	RepoRepo	repo;
	ChunksErrorError failed;
	SimcRun		run;
	SimcReport	report;

	if (!repo_Repo_create(io, 9390, HOST_PG_DEFAULT, &repo, &failed))
		STD_RESULT_EXPECT_FAILED(2825, 81, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	repo_Repo_drop(&repo);
	run = simc_Run_new(1);
	simc_Run_actor(&run, "resolver", 8, resolver);
	simc_Run_actor(&run, "pusher", 6, pusher);
	for (int k = 0; k < 3; k++)
	{
		simc_Run_until(&run, "resolver", 8, "doltgres-insert-resolved");
		simc_Run_until(&run, "pusher", 6, "doltgres-memtable-unpinned");
	}
	simc_Run_until(&run, "resolver", 8, "doltgres-insert-resolved");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_vacuum_tests_returned(&report, RUST_FILE, 2863, 67, 2864, 9);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_go is an update's read that goes on as the transaction's command, naming no row. */
static bool
txn_insert_tests_go(void *captures, HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len,
					TxnChangeRead *result, void *stopped, HostError *error)
{
	*result = (TxnChangeRead)
	{
		.tag = TXN_CHANGE_READ_GO, .u.go.shown.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND
	};
	return true;
}

/* txn_insert_tests_kept_t is kept_across's updater's captures: its database and its row's tuple id. */
typedef struct txn_insert_tests_kept_t
{
	uint32		database;
	uint64		tid;
} txn_insert_tests_kept_t;

/* txn_insert_tests_kept_updater is kept_across's updater: an update of the row that keeps its two out-of-line values. */
static bool
txn_insert_tests_kept_updater(void *captures, HostIoIo *io, HostError *error)
{
	static const size_t kept[] = {0, 1};
	const txn_insert_tests_kept_t *c = captures;
	HostStorage storage = txn_insert_tests_stored(c->database);
	HostStorage toasted = storage;
	TxnInsertRoom room = txn_insert_tests_room(60);
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostSharedHeld toast;
	bool		ok;

	toasted.relation = 16390;
	if (!txn_insert_tests_opened(io, storage, &repo, &reading, &held, error))
		return false;
	if (!txn_state_state(io, &repo, &reading, &toasted, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &toast, error))
	{
		host_shared_Held_drop(&held);
		host_shared_Reader_drop(&reading);
		repo_Repo_drop(&repo);
		return false;
	}
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
		TxnValuesEntry entries[2];
		TxnInsertInsert row = txn_insert_tests_valued(c->database, entries);
		TxnInsertReplaces replaces = {.tuple = {c->tid}, .lock = TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE};
		TxnInsertKeyed keyed = {.stripe = txn_change_stripe(&storage, row.key.ptr, row.key.len, true)};
		HostIoWaits waits = host_io_Waits_testing();
		TxnInsertInserted inserted;

		std_vec_free_Checked(&row.kept, sizeof(size_t), _Alignof(size_t));
		std_slice_to_vec_Checked(&row.kept, kept, lengthof(kept), sizeof(size_t), _Alignof(size_t));
		std_vec_clone(&replaces.key, &row.key, 1, 1);
		replaces.stripe = txn_change_stripe(&storage, replaces.key.ptr, replaces.key.len, true);
		ok = txn_insert_update(io, TREES_STORE_READER_NODE_STORE(&store), &held, &row, &replaces, &room, NULL, &toast,
							   NULL, 0, keyed, &waits, txn_insert_tests_go, NULL, NULL, NULL, &inserted, error);
	}
	host_shared_Held_drop(&toast);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/*
 * txn_insert_tests_kept_across runs an update that keeps a row's two
 * out-of-line values while a checkpoint runs, placed by a script: with the
 * update held between its count adds' push and its row's push, the
 * checkpoint's runs hold the adds and not the row; run before the update,
 * neither; after it, both. A crash and redo of every logged record then leave
 * each kept value's count at 2, and a second crash during that redo and redo
 * again leave it at 2.
 */
static void
txn_insert_tests_kept_across(uint32 database, void (*script) (SimcRun *run))
{
	static const uint64 twice[] = {2, 2};
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = txn_insert_tests_stored(database);
	HostStorage toasted = storage;
	txn_insert_tests_kept_t c = {.database = database};
	SimcActor	checkpointer = {txn_insert_tests_checkpointer};
	SimcActor	updater = {txn_insert_tests_kept_updater, &c};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostSharedHeld toast;
	HostSharedHeld deltas;
	TreesStoreReader store;
	TxnValuesEntry entries[2];
	TxnInsertInsert row;
	SimcRun		run;
	SimcReport	report;
	HostError	error;

	toasted.relation = 16390;
	txn_insert_tests_repository(io, database, 2878, 59, 2879, 46, &repo, &reading);
	row = txn_insert_tests_valued(database, entries);
	deltas = txn_insert_tests_written(io, database, row.writer, entries);
	host_shared_Held_drop(&deltas);
	held = txn_insert_tests_stated(io, &repo, &reading, storage, 2884, 99);
	toast = txn_insert_tests_toasted(io, &repo, &reading, database, 2885, 100);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	c.tid = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, row, 60, &toast, entries, 2,
								  2887, 78);
	host_shared_Held_drop(&toast);
	host_shared_Held_drop(&held);
	simc_checkpointed(UINT64CONST(0x100000000));
	run = simc_Run_new(1);
	simc_Run_actor(&run, "checkpointer", 12, checkpointer);
	simc_Run_actor(&run, "updater", 7, updater);
	script(&run);
	report = simc_Run_finish(run);
	txn_vacuum_tests_returned(&report, RUST_FILE, 2915, 67, 2916, 9);
	simc_Report_drop(&report);
	if (!txn_memtable_branch_deltas(io, database, HOST_PG_DEFAULT, &deltas, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(2918, 65, &error);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	for (int k = 0; k < 2; k++)
	{
		StdVec		writes;
		uint64		counts[2];

		if (!txn_state_unlinked(io, &storage, &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(2921, 37, &error);
		if (!txn_state_unlinked(io, &toasted, &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(2922, 37, &error);
		if (!txn_runs_loaded(io, &TXN_INSERT_TESTS_PAGE, &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(2923, 71, &error);
		held = txn_insert_tests_stated(io, &repo, &reading, storage, 2924, 99);
		toast = txn_insert_tests_toasted(io, &repo, &reading, database, 2925, 100);
		writes = txn_insert_tests_logged(database);
		txn_insert_tests_redone(io, TREES_STORE_READER_NODE_STORE(&store), &held, &toast, &deltas, &writes);
		txn_insert_tests_counts(io, TREES_STORE_READER_NODE_STORE(&store), &toast, 2927, 95,
								2927, 104, counts);
		txn_insert_tests_u64s_is(2928, 13, counts, 2, twice, 2);
		host_shared_Held_drop(&toast);
		host_shared_Held_drop(&held);
	}
	txn_insert_tests_ended(io, 2930, 35);
	host_shared_Held_drop(&deltas);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/* txn_insert_tests_kept_between holds the update between its count adds' push and its row's push while the checkpoint runs. */
static void
txn_insert_tests_kept_between(SimcRun *run)
{
	static const SimcStep steps[] = {{"checkpointer", 12, UINT64_MAX}, {"updater", 7, UINT64_MAX},
	{"checkpointer", 12, UINT64_MAX}};

	simc_Run_until(run, "updater", 7, "doltgres-insert-logged");
	simc_Run_script(run, steps, lengthof(steps));
}

/*
 * (4b)'s section 5: an update held between its kept values' count adds and
 * its row's push while a checkpoint sweeps its storage's rows and then its
 * maps, a crash, and redo leave each kept value's count at 2, not 3, and a
 * second crash during that redo too.
 */
void
test_txn_insert_tests_a_checkpoint_between_an_updates_count_adds_and_its_row_leaves_the_counts_once(void)
{
	txn_insert_tests_kept_across(9391, txn_insert_tests_kept_between);
}

/* txn_insert_tests_kept_before runs the checkpoint before the update. */
static void
txn_insert_tests_kept_before(SimcRun *run)
{
	static const SimcStep steps[] = {{"checkpointer", 12, UINT64_MAX}, {"updater", 7, UINT64_MAX}};

	simc_Run_script(run, steps, lengthof(steps));
}

/* txn_insert_tests_kept_after runs the checkpoint after the update. */
static void
txn_insert_tests_kept_after(SimcRun *run)
{
	static const SimcStep steps[] = {{"updater", 7, UINT64_MAX}, {"checkpointer", 12, UINT64_MAX}};

	simc_Run_script(run, steps, lengthof(steps));
}

/*
 * (4b)'s section 5: a checkpoint before an update that keeps two values, or
 * after it, a crash, and redo leave each kept value's count at 2.
 */
void
test_txn_insert_tests_a_checkpoint_before_or_after_an_update_leaves_its_kept_counts_once(void)
{
	txn_insert_tests_kept_across(9392, txn_insert_tests_kept_before);
	txn_insert_tests_kept_across(9393, txn_insert_tests_kept_after);
}

/* txn_insert_tests_moving_updater is the key change case's updater: an update that moves its row from key 1 to key 2. */
static bool
txn_insert_tests_moving_updater(void *captures, HostIoIo *io, HostError *error)
{
	const uint64 *tid = captures;
	HostStorage storage = txn_insert_tests_stored(9394);
	TxnInsertRoom room = txn_insert_tests_room(28);
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	bool		ok;

	if (!txn_insert_tests_opened(io, storage, &repo, &reading, &held, error))
		return false;
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
		TxnInsertInsert row = txn_insert_tests_row(storage, 2);
		TxnInsertReplaces replaces = {.key = txn_insert_tests_key(1), .tuple = {*tid},
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE};
		TxnInsertKeyed keyed = {.stripe = txn_change_stripe(&storage, row.key.ptr, row.key.len, true)};
		HostIoWaits waits = host_io_Waits_testing();
		TxnInsertInserted inserted;

		replaces.stripe = txn_change_stripe(&storage, replaces.key.ptr, replaces.key.len, true);
		ok = txn_insert_update(io, TREES_STORE_READER_NODE_STORE(&store), &held, &row, &replaces, &room, NULL, NULL,
							   NULL, 0, keyed, &waits, txn_insert_tests_go, NULL, NULL, NULL, &inserted, error);
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/*
 * (4b)'s section 10: an update that moves its row from key 1 to key 2, held
 * between its old key's move and its new key's put while a checkpoint runs,
 * so the runs hold the move and not the put, a crash, and redo of every
 * logged record leave both chains as the primary's, and a second crash
 * during that redo too.
 */
void
test_txn_insert_tests_a_crash_after_a_key_change_whose_runs_hold_one_chain_redoes_both(void)
{
	static const SimcStep steps[] = {{"checkpointer", 12, UINT64_MAX}, {"updater", 7, UINT64_MAX},
	{"checkpointer", 12, UINT64_MAX}};
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = txn_insert_tests_stored(9394);
	uint64		tid;
	SimcActor	checkpointer = {txn_insert_tests_checkpointer};
	SimcActor	updater = {txn_insert_tests_moving_updater, &tid};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TreesStoreReader store;
	StdVec		wanted;
	StdVec		primary;
	SimcRun		run;
	SimcReport	report;
	HostError	error;

	txn_insert_tests_repository(io, 9394, 2962, 59, 2963, 46, &repo, &reading);
	held = txn_insert_tests_stated(io, &repo, &reading, storage, 2966, 99);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	tid = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &held, txn_insert_tests_row(storage, 1), 28,
								NULL, NULL, 0, 2968, 64);
	host_shared_Held_drop(&held);
	simc_checkpointed(UINT64CONST(0x100000000));
	run = simc_Run_new(1);
	simc_Run_actor(&run, "checkpointer", 12, checkpointer);
	simc_Run_actor(&run, "updater", 7, updater);
	simc_Run_until(&run, "updater", 7, "doltgres-insert-logged");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_vacuum_tests_returned(&report, RUST_FILE, 2993, 67, 2994, 9);
	simc_Report_drop(&report);
	held = txn_insert_tests_stated(io, &repo, &reading, storage, 2997, 99);
	primary = txn_insert_tests_chains(io, host_shared_Held_record(&held));
	host_shared_Held_drop(&held);
	std_vec_new(&wanted, sizeof(txn_insert_tests_chain_t), _Alignof(txn_insert_tests_chain_t));
	for (uint64 n = 1; n <= 2; n++)
	{
		txn_insert_tests_chain_t chain = {.key = txn_insert_tests_key(n), .versions = 3 - n};

		std_vec_push_Checked(&wanted, &chain, sizeof(chain), _Alignof(txn_insert_tests_chain_t));
	}
	STD_ASSERT_EQ(3000, 9, txn_insert_tests_chains_eq(&primary, &wanted), &primary,
				  txn_insert_tests_chains_Debug_fmt, &wanted, txn_insert_tests_chains_Debug_fmt);
	txn_insert_tests_chains_free(&wanted);
	store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
	for (int k = 0; k < 2; k++)
	{
		StdVec		chains;

		if (!txn_state_unlinked(io, &storage, &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(3003, 37, &error);
		if (!txn_runs_loaded(io, &TXN_INSERT_TESTS_PAGE, &error))
			TXN_INSERT_TESTS_UNWRAP_FAILED(3004, 71, &error);
		held = txn_insert_tests_stated(io, &repo, &reading, storage, 3005, 99);
		txn_insert_tests_replayed_rows(io, TREES_STORE_READER_NODE_STORE(&store), &held, 9394, 16384);
		chains = txn_insert_tests_chains(io, host_shared_Held_record(&held));
		STD_ASSERT_EQ(3007, 13, txn_insert_tests_chains_eq(&chains, &primary), &chains,
					  txn_insert_tests_chains_Debug_fmt, &primary, txn_insert_tests_chains_Debug_fmt);
		txn_insert_tests_chains_free(&chains);
		host_shared_Held_drop(&held);
	}
	txn_insert_tests_ended(io, 3009, 35);
	txn_insert_tests_chains_free(&primary);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/* txn_insert_tests_resolving_t is resolving's resolver's captures: its storage's relation and the block it tries first, when given. */
typedef struct txn_insert_tests_resolving_t
{
	uint32		relation;
	bool		has_target;
	uint32		target;
} txn_insert_tests_resolving_t;

/* txn_insert_tests_resolving_resolver is resolving's resolver: an insert of row 1 resolved over its storage's stripe, which takes a tuple id. */
static bool
txn_insert_tests_resolving_resolver(void *captures, HostIoIo *io, HostError *error)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("the resolve gave "), STD_FMT_ARG(0)};
	const txn_insert_tests_resolving_t *c = captures;
	HostStorage storage = txn_insert_tests_stored(9397);
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	TxnInsertRoom room = txn_insert_tests_room(28);
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	TxnInsertInserted inserted;
	bool		ok;

	storage.relation = c->relation;
	if (!txn_insert_tests_opened(io, storage, &repo, &reading, &held, error))
		return false;
	{
		TreesStoreReader store = repo_Repo_reader(&repo, HOST_CACHE_PLACEMENT_SERVER, &reading);
		TxnInsertKeyed keyed = {.stripe = txn_change_stripe(&storage, NULL, 0, false), .has_unique = true,
		.unique = {.order = &order, .index = 16386, .has_equal = true, .equal = {&TXN_INSERT_TESTS_FOLDED_OPS, NULL}}};
		HostIoWaits waits = host_io_Waits_testing();
		TxnInsertInsert row = txn_insert_tests_row(storage, 1);

		row.has_target = c->has_target;
		row.target = c->target;
		ok = txn_insert_insert(io, TREES_STORE_READER_NODE_STORE(&store), &held, &row, &room, NULL, NULL, NULL, 0,
							   keyed, &waits, &inserted, error);
	}
	if (ok && inserted.tag != TXN_INSERT_INSERTED_TOOK)
	{
		TxnInsertInsertedRef other = {&inserted, {NULL, std_unit_Debug_fmt}};
		StdFmtArg	args[] = {{&other, txn_insert_Inserted_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		*error = txn_insert_tests_gathering_fork(&arguments);
		ok = false;
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/*
 * txn_insert_tests_resolving runs one insert of row 1 into a storage of
 * database 9397 by its relation, resolved over the storage's stripe, trying
 * the block target first when given, and terminates it past yields when
 * given.
 */
static SimcReport
txn_insert_tests_resolving(uint32 relation, bool has_target, uint32 target, bool has_yields, uint64 yields)
{
	txn_insert_tests_resolving_t c = {.relation = relation, .has_target = has_target, .target = target};
	SimcActor	resolver = {txn_insert_tests_resolving_resolver, &c};
	SimcRun		run = simc_Run_new(1);

	simc_Run_actor(&run, "resolver", 8, resolver);
	if (has_yields)
		simc_Run_terminate_at(&run, "resolver", 8, yields);
	return simc_Run_finish(run);
}

/* txn_insert_tests_returned_is asserts at a line and column that a run ended with every actor returning Ok, its message the run's outcome. */
static void
txn_insert_tests_returned_is(uint32 line, uint32 col, const SimcReport *report)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&report->outcome, simc_Outcome_Debug_fmt}};
	StdFmtArguments message = {pieces, lengthof(pieces), args};

	if (!txn_vacuum_tests_all_returned(report))
		std_panic_fmt(RUST_FILE, line, col, &message);
}

/*
 * Bugs row 86: a resolve over its storage's stripe whose first target is
 * refused goes on to its next target in the same pass, since its refused turn
 * pushed nothing and so left the stripe's push count as the resolve read it,
 * and the insert ends within twice the yields a plain insert of its row takes.
 */
void
test_txn_insert_tests_a_resolve_whose_first_target_is_refused_ends_in_bounded_turns(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	ChunksErrorError failed;
	SimcReport	plain;
	SimcReport	refused;
	StdVec		trace;
	uint64		bound;

	if (!repo_Repo_create(io, 9397, HOST_PG_DEFAULT, &repo, &failed))
		STD_RESULT_EXPECT_FAILED(3044, 81, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	repo_Repo_drop(&repo);
	plain = txn_insert_tests_resolving(16384, false, 0, false, 0);
	txn_insert_tests_returned_is(3050, 9, &plain);
	trace = simc_Report_trace(&plain);
	bound = 2 * (uint64) trace.len;
	std_vec_free_Checked(&trace, sizeof(size_t), _Alignof(size_t));
	simc_Report_drop(&plain);
	refused = txn_insert_tests_resolving(16388, true, 5, true, bound);
	txn_insert_tests_returned_is(3054, 9, &refused);
	simc_Report_drop(&refused);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 97: an insert written frozen, as COPY FREEZE writes it, takes no
 * room in the commit status table, its writer FrozenTransactionId having no
 * entry, and its row change replays on another storage the same way: the
 * replay pushes the row and asks the table for nothing.
 */
void
test_txn_insert_tests_a_frozen_inserts_row_change_replays_with_no_status_room(void)
{
	static const uint32 databases[] = {9464, 9465};
	static const uint8 one[8] = {0, 0, 0, 0, 0, 0, 0, 1};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading;
	RepoRepo	repos[2];
	HostSharedHeld helds[2];
	TxnInsertInsert frozen = txn_insert_tests_row(txn_insert_tests_stored(9464), 1);
	TreesStoreReader store;
	TreesStoreReader replica;
	HostDeltaHold hold;
	StdVec		versions;
	StdVec		tupled;
	HostRow		tuple;
	HostError	error;
	bool		some;

	if (!host_shared_read(io, &reading, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(3064, 46, &error);
	for (size_t k = 0; k < lengthof(databases); k++)
	{
		ChunksErrorError failed;

		if (!repo_Repo_create(io, databases[k], HOST_PG_DEFAULT, &repos[k], &failed))
			STD_RESULT_EXPECT_FAILED(3067, 95, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		helds[k] = txn_insert_tests_stated(io, &repos[k], &reading, txn_insert_tests_stored(databases[k]),
										   3069, 107);
	}
	frozen.writer = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
	frozen.frozen = true;
	store = repo_Repo_reader(&repos[0], HOST_CACHE_PLACEMENT_SERVER, &reading);
	tuple.f0 = txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &helds[0], frozen, 28, NULL, NULL, 0,
									 3075, 72);
	replica = repo_Repo_reader(&repos[1], HOST_CACHE_PLACEMENT_SERVER, &reading);
	txn_insert_tests_replayed_rows(io, TREES_STORE_READER_NODE_STORE(&replica), &helds[1], 9464, 16384);
	if (!host_delta_hold(io, &hold, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(3078, 42, &error);
	if (!txn_memtable_table_versions(io, &hold, host_shared_Held_record(&helds[1]), one, sizeof(one), &versions,
									 &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(3079, 94, &error);
	{
		TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
		bool		matched = versions.len == 1;

		if (matched)
		{
			TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&STD_VEC_AT(&versions,
																							  TxnMemtableVersionVersion,
																							  0));
			TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);

			matched = txn_memtable_version_Writer_PartialEq_eq(&writer, &none);
		}
		STD_ASSERT(3080, 9, matched,
				   "matches!(versions[..], [ref version] if version.record().writer() == crate::memtable::version::NO_TRANSACTION)");
	}
	txn_memtable_version_versions_drop(&versions, 0);
	host_delta_Hold_drop(&hold);
	if (!txn_memtable_tuples_get(io, host_shared_Held_record(&helds[1]), tuple, &some, &tupled, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(3084, 68, &error);
	txn_insert_tests_row_is(3084, 9, some, &tupled);
	if (some)
		std_vec_free_Checked(&tupled, 1, 1);
	txn_insert_tests_ended(io, 3085, 35);
	for (size_t k = lengthof(databases); k-- > 0;)
	{
		host_shared_Held_drop(&helds[k]);
		repo_Repo_drop(&repos[k]);
	}
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* txn_insert_tests_ask_t is a test run's ask's captures: the answer it gives, and the top-level xid it was asked of. */
typedef struct txn_insert_tests_ask_t
{
	bool		answer;
	uint64		asked;
} txn_insert_tests_ask_t;

/* txn_insert_tests_asked is a test run's ask: its captures' answer, the xid it was asked of kept. */
static bool
txn_insert_tests_asked(void *captures, HostIoIo *io, uint64 top, uint64 point, bool *result, HostError *error)
{
	txn_insert_tests_ask_t *ask = captures;

	ask->asked = top;
	*result = ask->answer;
	return true;
}

/* txn_insert_tests_run_is asserts what a run says of a take of a free space map's key, at a line. */
static void
txn_insert_tests_run_is(HostIoIo *io, const TxnInsertRun *run, const HostSharedHeld *held, uint32 key, bool expected,
						uint32 line)
{
	HostError	error;
	bool		out;

	if (!txn_insert_run_out(io, run, held, TXN_MEMTABLE_MAP_MAP_FREE, key, &out, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(line, 9, &error);
	STD_ASSERT(line, 9, out == expected, "out == expected");
}

/*
 * txn_insert_tests_runs_frame_t is the run case's checks' state: the io, the
 * two storages' states, the writer still open, and whether it is.
 */
typedef struct txn_insert_tests_runs_frame_t
{
	HostIoIo   *io;
	HostSharedHeld *helds;
	HostXid		writer;
	bool		open;
} txn_insert_tests_runs_frame_t;

/* txn_insert_tests_runs_body is the run case's checks, the open writer aborted once they need it no more. */
static void
txn_insert_tests_runs_body(void *frame_)
{
	txn_insert_tests_runs_frame_t *frame = frame_;
	HostIoIo   *io = frame->io;
	txn_insert_tests_ask_t ask = {0};
	TxnInsertRun applied = {.point = 0x100000, .applied = true, .ask = txn_insert_tests_asked, .captures = &ask};
	TxnInsertRun other = {.point = 0x100000, .ask = txn_insert_tests_asked, .captures = &ask};
	TxnInsertRun later = {.point = 0x200000, .applied = true, .ask = txn_insert_tests_asked, .captures = &ask};

	txn_insert_tests_run_is(io, &applied, &frame->helds[0], 0, true, __LINE__);
	txn_insert_tests_run_is(io, &applied, &frame->helds[0], 0, true, __LINE__);
	txn_insert_tests_run_is(io, &other, &frame->helds[0], 0, false, __LINE__);
	txn_insert_tests_run_is(io, &applied, &frame->helds[0], 0, false, __LINE__);
	txn_insert_tests_run_is(io, &applied, &frame->helds[0], 1, true, __LINE__);
	txn_insert_tests_run_is(io, &later, &frame->helds[0], 0, true, __LINE__);
	txn_insert_tests_run_is(io, NULL, &frame->helds[0], 2, false, __LINE__);
	txn_insert_tests_run_is(io, &applied, &frame->helds[1], 0, false, __LINE__);
	STD_ASSERT_EQ_U64(__LINE__, 9, ask.asked, frame->writer.f0);
	ask.answer = true;
	txn_insert_tests_run_is(io, &later, &frame->helds[1], 0, true, __LINE__);
	frame->open = false;
	simc_abort(frame->writer);
}

/*
 * txn_insert_tests_runs_pad aborts the run case's open writer and ends the
 * test's transaction when a check panics, so no later case meets them.
 */
static void
txn_insert_tests_runs_pad(void *frame_)
{
	txn_insert_tests_runs_frame_t *frame = frame_;
	HostError	error;

	if (!frame->open)
		return;
	frame->open = false;
	simc_abort(frame->writer);
	if (!txn_written_ended(frame->io, &error))
		host_Error_drop(&error);
}

/*
 * Bugs row 401: past a base, redo leaves out a take of a map's key while
 * every version of the key the start loaded and every take of it redo met is
 * by a writer the base's point applied or saw abort: an aborted writer's
 * loaded version keeps the run open, an open writer's ends it unless the WAL
 * ahead ends the writer by the point, and once another writer's take
 * replays, every later take of the key replays; each key and each point has
 * a run of its own, and with no run nothing is left out.
 */
void
test_txn_insert_tests_a_takes_run_past_a_base_ends_at_the_first_writer_it_didnt_apply(void)
{
	static const uint32 databases[] = {9498, 9499};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading;
	RepoRepo	repos[2];
	HostSharedHeld helds[2];
	HostXid		writers[2];
	txn_insert_tests_runs_frame_t frame = {0};
	HostError	error;

	if (!host_shared_read(io, &reading, &error))
		TXN_INSERT_TESTS_UNWRAP_FAILED(__LINE__, 46, &error);
	for (size_t k = 0; k < lengthof(databases); k++)
	{
		ChunksErrorError failed;
		TxnInsertInsert row = txn_insert_tests_row(txn_insert_tests_stored(databases[k]), 1);
		TreesStoreReader store;

		if (!repo_Repo_create(io, databases[k], HOST_PG_DEFAULT, &repos[k], &failed))
			STD_RESULT_EXPECT_FAILED(__LINE__, 95, "called `Result::unwrap()` on an `Err` value", &failed,
									 chunks_error_Error_Debug_fmt);
		helds[k] = txn_insert_tests_stated(io, &repos[k], &reading, txn_insert_tests_stored(databases[k]),
										   __LINE__, 107);
		writers[k] = simc_begin();
		row.writer.xid = writers[k].f0;
		store = repo_Repo_reader(&repos[k], HOST_CACHE_PLACEMENT_SERVER, &reading);
		(void) txn_insert_tests_took(io, TREES_STORE_READER_NODE_STORE(&store), &helds[k], row, 28, NULL, NULL, 0,
									 __LINE__, 72);
	}
	simc_abort(writers[0]);
	frame.io = io;
	frame.helds = helds;
	frame.writer = writers[1];
	frame.open = true;
	std_landing(&frame, txn_insert_tests_runs_body, txn_insert_tests_runs_pad);
	txn_insert_tests_ended(io, __LINE__, 35);
	for (size_t k = lengthof(databases); k-- > 0;)
	{
		host_shared_Held_drop(&helds[k]);
		repo_Repo_drop(&repos[k]);
	}
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}
#endif
