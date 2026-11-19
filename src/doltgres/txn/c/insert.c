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

#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/cursor.h"
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
			return self->u.took == other->u.took;
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
			std_DebugTuple_field(&t, &inserted->u.took, std_u64_Debug_fmt);
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
	StdBytes	at = trees_cursor_Cursor_key(&frame->cursor);
	HostError	failed;

	*result = false;
	if (!trees_cursor_Cursor_is_valid(&frame->cursor))
		return true;
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

/* txn_insert_seen_body reads the block's newest entry, the delta's else the base's, and decodes it. */
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

	if (!txn_memtable_map_newest(frame->io, frame->record, TXN_MEMTABLE_MAP_MAP_FREE, frame->block, &some, &newest,
								 frame->error))
		return;
	if (some)
	{
		result->has_current = newest.has_value;
		result->current = newest.value;
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
 * under its lock, and the base's entry the take falls back to.
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
txn_insert_Got_tuple(void *captures, const TxnInsertGot *got)
{
	HostRow		tuple = {got->tag == TXN_INSERT_GOT_LEFT ? 0 : got->tid};

	return tuple;
}

/* The map's take of a block's free space entry, which keeps what the take got. */
#define TXN_MEMTABLE_MAP_TS Got
#define TXN_MEMTABLE_MAP_KEPT_OUT , TxnInsertGot *kept
#define TXN_MEMTABLE_MAP_KEPT_IN , const TxnInsertGot *kept
#define TXN_MEMTABLE_MAP_DECLARE
#define TXN_MEMTABLE_MAP_DEFINE
#define TXN_MEMTABLE_MAP_KEPT_ARG kept
#define TXN_MEMTABLE_MAP_KEPT_CALL(k) , (k)
#define TXN_MEMTABLE_MAP_KEPT_DROP(k)
#include "doltgres/txn/memtable/map_template.h"

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

/* txn_insert_TakeStep_t is a take's step's captures: what it compares and the entry it reads. */
typedef struct txn_insert_TakeStep_t
{
	bool		has_expected;
	const StdVec *expected;
	HostSharedRecord record;
	const TxnInsertBase *base;
	const TxnInsertRoom *room;
	uint32		block;
	uint64		counter;
	uint64		tid;
} txn_insert_TakeStep_t;

/* txn_insert_unmoved reports whether a block's entry and the base are as they were read outside the window. */
static bool
txn_insert_unmoved(const txn_insert_TakeStep_t *step, bool has_current, const uint8 *current, size_t current_len)
{
	if (has_current != step->has_expected)
		return false;
	if (has_current && (current_len != step->expected->len ||
						(current_len != 0 && memcmp(current, step->expected->ptr, current_len) != 0)))
		return false;
	return txn_state_generation(step->record) == step->base->generation;
}

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
 * page, pure over its input: Retry when the entry or the base moved since
 * they were read outside the window, Refuse when the entry has no room for
 * the row, and otherwise the entry with its lowest free offset and the row's
 * bytes taken, written into out, or its removal when it was the last.
 */
static TxnMemtableMapStep
txn_insert_take_block(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out,
					  TxnInsertGot *kept)
{
	const txn_insert_TakeStep_t *step = captures;
	TxnFreeSpaceFree entry;
	uint16		offset;

	if (!txn_insert_unmoved(step, has_current, current, current_len))
		return TXN_MEMTABLE_MAP_STEP_RETRY;
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
 * its counter offsets handed to the map; Retry when the entry, the counter,
 * or the base moved since they were read. An untouched page has a page's
 * bytes free; a touched one with no entry is full.
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

	if (!txn_insert_unmoved(step, has_current, current, current_len) ||
		txn_state_counter(step->record) != step->counter)
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

/* txn_insert_stored is a stored value's step: its entry written where the map holds none, else Retry. */
static TxnMemtableMapStep
txn_insert_stored(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	if (has_current)
		return TXN_MEMTABLE_MAP_STEP_RETRY;
	txn_values_Entry_encode_into(captures, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_insert_added is a kept value's step, values' count add. */
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

/* txn_insert_nothing_got is a take's then over what it got that does nothing. */
static void
txn_insert_nothing_got(void *captures, const TxnInsertGot *got)
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
 * answer, the stripes' guard, the checkpoint window, and a record's body,
 * each while live.
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
	uint32		stripes[2] = {w->keyed.stripe, w->replaces.stripe};
	HostIoKeyIo key_io;
	HostIoTakesShort io;
	HostIoIoDoors doors;
	TxnMemtableVersionWriter writer = w->change.writer;
	TxnInsertRoom own;
	const TxnInsertRoom *room = w->room;
	TxnMemtableMapTaken taken;
	TxnInsertGot got;
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
	if (!host_delay_checkpoint(doors, pauses, lengthof(pauses), &frame->window, frame->error))
		return;
	frame->window_live = true;
	if (w->has_replacing &&
		((turn->target.tag == TXN_INSERT_TARGET_BLOCK &&
		  (uint32) (w->replaces.tuple.f0 >> 16) == turn->target.u.block) ||
		 (turn->target.tag == TXN_INSERT_TARGET_FRESH && w->replaces.tuple.f0 >> 16 == turn->target.u.fresh >> 16)))
	{
		own = *w->room;
		own.reserve = 0;
		room = &own;
	}
	if (turn->target.tag == TXN_INSERT_TARGET_TAKEN)
	{
		taken = TXN_MEMTABLE_MAP_TAKEN_TOOK;
		got.tag = TXN_INSERT_GOT_TID;
		got.tid = turn->target.u.taken;
	}
	else
	{
		txn_insert_TakeStep_t step = {.has_expected = turn->seen.has_current, .expected = &turn->seen.current,
			.record = turn->record, .base = turn->base, .room = room, .block = turn->block,
		.counter = turn->target.tag == TXN_INSERT_TARGET_FRESH ? turn->target.u.fresh : 0};
		bool		fresh = turn->target.tag == TXN_INSERT_TARGET_FRESH;
		HostRow		unclaimed = {0};

		if (!txn_memtable_map_took_Got(io, frame->prepared, 0, writer, false, 0, false, unclaimed, 0,
									   turn->seen.has_below, turn->seen.below.ptr, turn->seen.below.len,
									   fresh ? txn_insert_take_fresh : txn_insert_take_block, &step, NULL,
									   txn_insert_Got_tuple, NULL, NULL,
									   fresh ? txn_insert_fresh_moved : txn_insert_nothing_got, &step, NULL,
									   &taken, &got, frame->error))
			return;
		if (taken == TXN_MEMTABLE_MAP_TAKEN_TOOK &&
			!txn_memtable_table_counted(io, frame->prepared, w->has_roomed, w->roomed, frame->error))
			return;
	}
	if (taken == TXN_MEMTABLE_MAP_TAKEN_RETRY ||
		(taken == TXN_MEMTABLE_MAP_TAKEN_TOOK && got.tag == TXN_INSERT_GOT_LEFT))
	{
		if (taken == TXN_MEMTABLE_MAP_TAKEN_TOOK && w->logged && turn->target.tag == TXN_INSERT_TARGET_FRESH)
		{
			TxnRecordPageLeft left = {.writer = writer, .storage = w->change.storage,
			.to = STD_ADD(u64, turn->target.u.fresh >> 16, 1, 965, 83) << 16 | 1};
			HostLsn		left_lsn;

			turn->leaving_live = false;
			txn_record_PageLeft_encode_with(&left, &turn->leaving, &frame->body);
			if (!txn_insert_turn_closure_9_logged(frame, doors, HOST_WAL_KIND_PAGE_LEFT, &left_lsn))
				return;
		}
		txn_insert_turn_closure_9_finish(frame, TXN_INSERT_TURN_RETRY);
		return;
	}
	if (taken == TXN_MEMTABLE_MAP_TAKEN_REFUSED)
	{
		txn_insert_turn_closure_9_finish(frame, TXN_INSERT_TURN_REFUSED);
		return;
	}
	tid = got.tid;
	if ((has_waiting || has_refused) && w->logged && turn->target.tag != TXN_INSERT_TARGET_TAKEN)
	{
		TxnRecordSpent spent = {.writer = writer, .storage = w->change.storage, .tuple = {tid},
		.heap_bytes = room->bytes};
		HostLsn		spent_lsn;

		turn->spending_live = false;
		txn_record_Spent_encode_with(&spent, &turn->spending, &frame->body);
		if (!txn_insert_turn_closure_9_logged(frame, doors, HOST_WAL_KIND_ROW_CHANGE, &spent_lsn))
			return;
	}
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
	if (has_refused)
	{
		txn_insert_turn_closure_9_finish(frame, TXN_INSERT_TURN_EXISTS);
		frame->result->u.exists.f0 = tid;
		frame->result->u.exists.has_f1 = has_live;
		frame->result->u.exists.f1 = live;
		return;
	}
	for (size_t at = 0; at < turn->stored; at++)
	{
		if (!txn_memtable_map_finish_Unit(io, frame->prepared, at + turn->first, writer, false, 0, false, NULL, 0,
										  txn_insert_stored, (void *) &w->entries[at], NULL, txn_insert_nothing, NULL,
										  NULL, &taken, frame->error))
			return;
		if (taken != TXN_MEMTABLE_MAP_TAKEN_TOOK)
		{
			txn_insert_turn_closure_9_finish(frame, TXN_INSERT_TURN_RETRY);
			return;
		}
		if (!txn_memtable_table_counted(io, frame->prepared, w->has_roomed, w->roomed, frame->error))
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
	for (size_t at = 0; at < turn->kept->len; at++)
	{
		const TxnInsertKept *read = &STD_VEC_AT(turn->kept, TxnInsertKept, at);

		if (!txn_memtable_map_finish_Unit(io, frame->prepared, turn->first + turn->stored + at, writer, has_lsn, lsn,
										  read->has_f0, read->f0.ptr, read->f0.len, txn_insert_added, NULL, NULL,
										  txn_insert_nothing, NULL, NULL, &taken, frame->error))
			return;
		if (taken != TXN_MEMTABLE_MAP_TAKEN_TOOK)
		{
			std_string_from_Checked(&message, "a kept value's map entry is missing", 35);
			*frame->error = host_Error_fork((const uint8 *) "XX001", &message);
			return;
		}
		if (!txn_memtable_table_counted(io, frame->prepared, w->has_roomed, w->roomed, frame->error))
			return;
	}
	if (turn->marks_live && turn->marks.len > 0)
	{
		HostRow		row = {tid};
		StdVec		marks = turn->marks;

		std_vec_new(&turn->marks, TXN_INSERT_ID);
		if (!txn_memtable_map_named(io, frame->prepared, turn->marking, writer, has_lsn, lsn, row, &marks,
									frame->error) ||
			!txn_memtable_table_counted(io, frame->prepared, w->has_roomed, w->roomed, frame->error))
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
	if (turn->has_flagged)
	{
		txn_insert_TakeStep_t step = {.record = turn->record, .base = turn->base, .room = room,
		.block = turn->flagged};

		if (!txn_memtable_map_finish_Unit(io, frame->prepared, turn->flagging, writer, false, 0,
										  turn->flagged_seen.has_below, turn->flagged_seen.below.ptr,
										  turn->flagged_seen.below.len, txn_insert_filled, &step, NULL,
										  txn_insert_nothing, NULL, NULL, &taken, frame->error))
			return;
		if (taken == TXN_MEMTABLE_MAP_TAKEN_RETRY)
		{
			std_string_from_Checked(&message, "a free space map's entry is corrupt", 35);
			*frame->error = host_Error_fork((const uint8 *) "XX001", &message);
			return;
		}
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
	if (frame->guard_live)
		host_KeyGuard_drop(&frame->guard);
	if (frame->answer_live)
		txn_visible_Resolved_drop(&frame->answer);
	if (frame->lockers_live)
		std_vec_free_Checked(&frame->lockers, TXN_INSERT_LOCKER);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	frame->body_live = frame->window_live = frame->guard_live = false;
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
						size_t key_len, size_t need, bool unique, HostStorage over[3], TxnWrittenOf overs[3],
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
	HostStorage over[3];
	TxnWrittenOf overs[3];
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
		if (frame->flagged_seen.has_current)
			std_vec_free_Checked(&frame->flagged_seen.current, 1, 1);
		frame->flagged_seen.has_current = false;
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
			frame->result->u.took = turn.u.took;
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
 * removal, which no entry was before it either.
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
	TxnMemtableMapStep (*step) (void *captures, bool has_current, const uint8 *current, size_t current_len,
								StdVec *out);
	txn_insert_JumpStep_t *captures;
	bool		has_roomed;
	TxnStatusRoomed roomed;
} txn_insert_jump_closure_0_captures_t;

/* txn_insert_jump_closure_0 is a jump's prepare's closure: the counter block's take, counted when it took. */
static bool
txn_insert_jump_closure_0(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
						  TxnMemtableMapTaken *result, HostError *error)
{
	txn_insert_jump_closure_0_captures_t *inside = captures;

	if (!txn_memtable_map_finish_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, inside->writer, false, 0,
									  inside->seen->has_below, inside->seen->below.ptr, inside->seen->below.len,
									  inside->step, inside->captures, NULL, txn_insert_jump_moved, inside->captures,
									  NULL, result, error))
		return false;
	return *result != TXN_MEMTABLE_MAP_TAKEN_TOOK ||
		txn_memtable_table_counted(HOST_IO_IO_TAKES_SHORT(io), prepared, inside->has_roomed, inside->roomed, error);
}

/* txn_insert_jump_frame_t is jump's state across its pad: the counter block's entries it read. */
typedef struct txn_insert_jump_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	TxnMemtableVersionWriter writer;
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
	.step = frame->step, .captures = &frame->step_captures};
	TxnMemtableMapTake take;

	if (!txn_insert_read_base(frame->io, frame->held, &frame->base, frame->error) ||
		!txn_insert_seen(frame->io, frame->store, record, &frame->base, frame->step_captures.room, block,
						 &frame->seen, frame->error))
		return;
	take = txn_insert_turn_take(record, TXN_MEMTABLE_MAP_MAP_FREE, block,
								txn_free_space_most_len(frame->step_captures.room->most));
	if (!txn_memtable_version_Writer_PartialEq_eq(&frame->writer, &none) &&
		!txn_status_redo_roomed(frame->io, &frame->writer, &inside.has_roomed, &inside.roomed, frame->error))
		return;
	frame->ok = txn_memtable_map_prepare_Taken(frame->io, &take, 1, txn_insert_jump_closure_0, &inside, NULL,
											   frame->result, frame->error);
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
 * what the entry becomes, and answers what the take did.
 */
static bool
txn_insert_jump(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, TxnMemtableVersionWriter writer,
				uint64 counter, uint64 to, const TxnInsertRoom *room,
				TxnMemtableMapStep (*step) (void *captures, bool has_current, const uint8 *current,
											size_t current_len, StdVec *out),
				TxnMemtableMapTaken *result, HostError *error)
{
	txn_insert_jump_frame_t frame = {.io = io, .store = store, .held = held, .writer = writer,
		.step_captures = {.record = host_shared_Held_record(held), .room = room, .counter = counter, .to = to},
	.step = step, .result = result, .error = error};

	std_landing(&frame, txn_insert_jump_body, txn_insert_jump_pad);
	txn_insert_jump_pad(&frame);
	return frame.ok;
}

/*
 * txn_insert_leave_fresh leaves the fresh page at a counter's load for the
 * next, as redo replays a leave: the page's counter offsets go to its free
 * space entry and the counter moves to the next block's first offset.
 */
static bool
txn_insert_leave_fresh(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
					   TxnMemtableVersionWriter writer, uint64 counter, const TxnInsertRoom *room, HostError *error)
{
	uint64		to = STD_ADD(u64, counter >> 16, 1, 1519, 15) << 16 | 1;
	TxnMemtableMapTaken taken;

	return txn_insert_jump(io, store, held, writer, counter, to, room, txn_insert_jumped, &taken, error);
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
		if (!txn_insert_jump(io, store, held, TXN_MEMTABLE_VERSION_NO_TRANSACTION, txn_state_counter(record), to,
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
					 const TxnRecordPageLeft *left, const TxnInsertPage *page, HostError *error)
{
	TxnInsertRoom room = {.page = page->bytes, .most = page->most};
	uint64		counter = txn_state_counter(host_shared_Held_record(held));

	if (STD_ADD(u64, counter >> 16, 1, 1503, 8) != left->to >> 16)
		return true;
	return txn_insert_leave_fresh(io, store, held, left->writer, counter, &room, error);
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
	if (seen.has_current)
		std_vec_free_Checked(&seen.current, 1, 1);
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

/* txn_insert_Placed_drop frees a place's base entry of its block. */
void
txn_insert_Placed_drop(TxnInsertPlaced *self)
{
	if (self->has_below)
		std_vec_free_Checked(&self->below, 1, 1);
	self->has_below = false;
}

/*
 * txn_insert_place_taken takes a placed tuple id's place out of its block's
 * entry inside the window of the prepare whose take Placed_take readied it,
 * as the writer's, with redo's step, so redo of the row's record finds it
 * taken; a place the entry doesn't hold free is an internal error.
 */
bool
txn_insert_place_taken(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t take,
					   HostSharedRecord record, TxnMemtableVersionWriter writer, const TxnInsertPlaced *placed,
					   HostError *error)
{
	txn_insert_ReplayStep_t step = {.record = record, .base = &placed->base, .room = &placed->room,
	.counter = placed->counter, .tid = placed->tid};
	HostRow		unclaimed = {0};
	TxnMemtableMapTaken taken;
	StdFmtPiece pieces[] = {STD_FMT_LIT("a carried version's place "), STD_FMT_ARG(0),
	STD_FMT_LIT(" isn't free in its storage's base")};
	StdFmtArg	args[] = {{&placed->tid, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	StdString	message;

	if (!txn_memtable_map_took_Unit(io, prepared, take, writer, false, 0, false, unclaimed, 0, placed->has_below,
									placed->below.ptr, placed->below.len, txn_insert_replayed, &step, NULL,
									txn_insert_replayed_tuple, &step, NULL, txn_insert_nothing, NULL, NULL, &taken,
									error))
		return false;
	if (taken == TXN_MEMTABLE_MAP_TAKEN_TOOK)
		return true;
	std_fmt_format_Checked(&message, &arguments);
	*error = host_Error_fork((const uint8 *) "XX000", &message);
	return false;
}

/* txn_insert_redo_spent_closure_0_captures_t is redo_spent's prepare's closure's captures. */
typedef struct txn_insert_redo_spent_closure_0_captures_t
{
	TxnMemtableVersionWriter writer;
	const TxnInsertSeen *seen;
	txn_insert_ReplayStep_t *step;
	bool		has_roomed;
	TxnStatusRoomed roomed;
} txn_insert_redo_spent_closure_0_captures_t;

/* txn_insert_redo_spent_moved is redo_spent's take's then: the counter moved past a tuple id at or past it. */
static void
txn_insert_redo_spent_moved(void *captures)
{
	const txn_insert_ReplayStep_t *step = captures;

	if (step->tid >= step->counter)
		txn_state_moved(step->record, step->counter, STD_ADD(u64, step->tid, 1, 1479, 55));
}

/* txn_insert_redo_spent_closure_0 is redo_spent's prepare's closure: the place's take, counted when it took. */
static bool
txn_insert_redo_spent_closure_0(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
								HostError *error)
{
	txn_insert_redo_spent_closure_0_captures_t *inside = captures;
	HostRow		unclaimed = {0};
	TxnMemtableMapTaken taken;

	if (!txn_memtable_map_took_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, inside->writer, false, 0, false,
									unclaimed, 0, inside->seen->has_below, inside->seen->below.ptr,
									inside->seen->below.len, txn_insert_replayed, inside->step, NULL,
									txn_insert_replayed_tuple, inside->step, NULL, txn_insert_redo_spent_moved,
									inside->step, NULL, &taken, error))
		return false;
	return taken != TXN_MEMTABLE_MAP_TAKEN_TOOK ||
		txn_memtable_table_counted(HOST_IO_IO_TAKES_SHORT(io), prepared, inside->has_roomed, inside->roomed, error);
}

/* txn_insert_redo_spent_frame_t is redo_spent's state across its pad: the block's entries it read. */
typedef struct txn_insert_redo_spent_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const TxnRecordSpent *spent;
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

	for (;;)
	{
		uint64		counter;
		uint32		block;
		txn_insert_ReplayStep_t step;
		txn_insert_redo_spent_closure_0_captures_t inside = {.writer = frame->spent->writer, .seen = &frame->seen,
		.step = &step};
		TxnMemtableMapTake take;

		if (!txn_insert_read_base(frame->io, frame->held, &frame->base, frame->error))
			return;
		counter = txn_state_counter(record);
		if (tid >> 16 > counter >> 16)
		{
			if (!txn_insert_leave_fresh(frame->io, frame->store, frame->held, frame->spent->writer, counter,
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
		frame->ok = txn_memtable_map_prepare_Unit(frame->io, &take, 1, txn_insert_redo_spent_closure_0, &inside,
												  NULL, frame->error);
		return;
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
 * take did, as redo takes a row's, and pushes no version.
 */
bool
txn_insert_redo_spent(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
					  const TxnRecordSpent *spent, const TxnInsertPage *page, HostError *error)
{
	txn_insert_redo_spent_frame_t frame = {.io = io, .store = store, .held = held, .spent = spent,
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
	const HostSharedHeld *toast;
	const TxnValuesEntry *entries;
	size_t		entries_len;
	bool		has_roomed;
	TxnStatusRoomed roomed;
	TxnInsertRoom room;
	uint64		tid;
	size_t		need;
	bool		pushed;
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

/* txn_insert_redo_refuse is a replayed stored value's step: its entry where the map holds none, else nothing. */
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

	if (!txn_memtable_map_took_Unit(io, prepared, 0, change->writer, false, 0, false, unclaimed, 0,
									frame->seen.has_below, frame->seen.below.ptr, frame->seen.below.len,
									txn_insert_replayed, &step, NULL, txn_insert_replayed_tuple, &step, NULL,
									txn_insert_replayed_moved, &step, NULL, &taken, error))
		return false;
	if (taken == TXN_MEMTABLE_MAP_TAKEN_TOOK &&
		!txn_memtable_table_counted(io, prepared, frame->has_roomed, frame->roomed, error))
		return false;
	for (size_t at = 0; frame->toast != NULL && at < frame->entries_len; at++)
	{
		if (!txn_memtable_map_finish_Unit(io, prepared, at + 1, change->writer, false, 0, false, NULL, 0,
										  txn_insert_redo_refuse, (void *) &frame->entries[at], NULL,
										  txn_insert_nothing, NULL, NULL, &taken, error))
			return false;
		if (taken == TXN_MEMTABLE_MAP_TAKEN_TOOK &&
			!txn_memtable_table_counted(io, prepared, frame->has_roomed, frame->roomed, error))
			return false;
	}
	for (size_t at = 0; at < frame->kept.len; at++)
	{
		const txn_insert_TupleU32OptionVecU8_t *kept = &STD_VEC_AT(&frame->kept, txn_insert_TupleU32OptionVecU8_t,
																   at);

		if (!txn_memtable_map_finish_Unit(io, prepared, 1 + frame->stored.len + at, change->writer, true, frame->lsn,
										  kept->has_f1, kept->f1.ptr, kept->f1.len, txn_insert_added, NULL, NULL,
										  txn_insert_nothing, NULL, NULL, &taken, error))
			return false;
		if (taken != TXN_MEMTABLE_MAP_TAKEN_TOOK)
		{
			std_string_from_Checked(&message, "a kept value's map entry is missing", 35);
			*error = host_Error_fork((const uint8 *) "XX001", &message);
			return false;
		}
		if (!txn_memtable_table_counted(io, prepared, frame->has_roomed, frame->roomed, error))
			return false;
	}
	if (frame->marks.len > 0)
	{
		StdVec		marks = frame->marks;

		std_vec_new(&frame->marks, TXN_INSERT_ID);
		if (!txn_memtable_map_named(io, prepared, frame->marking, change->writer, true, frame->lsn, row, &marks,
									error) ||
			!txn_memtable_table_counted(io, prepared, frame->has_roomed, frame->roomed, error))
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
	if (!txn_memtable_tuples_finish(io, prepared, row, error))
		return false;
	if (frame->has_flagged)
	{
		txn_insert_TakeStep_t filled = {.record = step.record, .base = &frame->base, .room = &frame->room,
		.block = frame->flagged};

		if (!txn_memtable_map_finish_Unit(io, prepared, frame->takes.len - 1, change->writer, false, 0,
										  frame->flagged_seen.has_below, frame->flagged_seen.below.ptr,
										  frame->flagged_seen.below.len, txn_insert_filled, &filled, NULL,
										  txn_insert_nothing, NULL, NULL, &taken, error))
			return false;
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
	for (;;)
	{
		uint32		block;
		bool		marked;

		if (!txn_insert_read_base(frame->io, frame->held, &frame->base, frame->error))
			return;
		frame->counter = txn_state_counter(record);
		if (frame->tid >> 16 > frame->counter >> 16)
		{
			if (!txn_insert_leave_fresh(frame->io, frame->store, frame->held, change->writer, frame->counter,
										&frame->room, frame->error))
				return;
			continue;
		}
		block = (uint32) (frame->tid >> 16);
		if (!txn_insert_seen(frame->io, frame->store, record, &frame->base, &frame->room, block, &frame->seen,
							 frame->error))
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
		frame->ok = txn_memtable_table_prepare_Unit(frame->io, record, &row, 1, frame->takes.ptr, frame->takes.len,
													true, txn_insert_redo_closure_9, frame, NULL, frame->error);
		return;
	}
}

/* txn_insert_redo_pad frees what redo holds live. */
static void
txn_insert_redo_pad(void *frame_)
{
	txn_insert_redo_frame_t *frame = frame_;

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
 * way.
 */
bool
txn_insert_redo(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const TxnRecordRowChange *change,
				uint64 lsn, const TxnInsertPage *page, const HostSharedHeld *toast,
				const TxnValuesEntry *toast_entries, size_t toast_entries_len, HostError *error)
{
	txn_insert_redo_frame_t frame = {.io = io, .store = store, .held = held, .change = change, .lsn = lsn,
		.toast = toast, .entries = toast_entries, .entries_len = toast_entries_len,
	.room = {.bytes = change->heap_bytes, .page = page->bytes, .most = page->most}, .error = error};

	std_landing(&frame, txn_insert_redo_body, txn_insert_redo_pad);
	txn_insert_redo_pad(&frame);
	return frame.ok;
}
