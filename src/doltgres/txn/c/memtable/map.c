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
 * The C port of txn's memtable/map.rs: a storage's delta maps, its free
 * space map and its value map, each a tree of a memtable keyed by a u32 in
 * big-endian order, whose takes a prepare readies and its finish steps on
 * the key's current value inside the window.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/varint.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/free_space.h"
#include "doltgres/txn/memtable.h"
#include "doltgres/txn/memtable/index.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/pins.h"
#include "doltgres/txn/memtable/range.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/runs.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "memtable" STD_SEP "map.rs"

/* txn_memtable_map_internal returns an internal error of the maps. */
static HostError
txn_memtable_map_internal(const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* txn_memtable_map_Map_Debug_fmt is Map's derived Debug. */
bool
txn_memtable_map_Map_Debug_fmt(const void *value, StdFormatter *f)
{
	return *(const TxnMemtableMapMap *) value == TXN_MEMTABLE_MAP_MAP_FREE ?
		std_fmt_write_str(f, "Free", strlen("Free")) :
		std_fmt_write_str(f, "Values", strlen("Values"));
}

/* TXN_MEMTABLE_MAP_FREE_TREE_OPS is Free's Tree: the memtable of a storage's free space map, of its State. */
const TxnMemtableTableTreeOps TXN_MEMTABLE_MAP_FREE_TREE_OPS = {
	.of = &TXN_MEMTABLE_TABLE_STATE_OWNER_OPS,
	.lock = TXN_STATE_STATE_LOCK_INDEX,
	.word = TXN_STATE_FREE_WORD_INDEX,
	.pin = TXN_MEMTABLE_PINS_FREE_PIN,
	.counted = true,
};

/* TXN_MEMTABLE_MAP_VALUES_TREE_OPS is Values' Tree: the memtable of a storage's value map, of its State. */
const TxnMemtableTableTreeOps TXN_MEMTABLE_MAP_VALUES_TREE_OPS = {
	.of = &TXN_MEMTABLE_TABLE_STATE_OWNER_OPS,
	.lock = TXN_STATE_STATE_LOCK_INDEX,
	.word = TXN_STATE_VALUES_WORD_INDEX,
	.pin = TXN_MEMTABLE_PINS_VALUES_PIN,
	.counted = true,
};

/* txn_memtable_map_tree returns a map's tree. */
static const TxnMemtableTableTreeOps *
txn_memtable_map_tree(TxnMemtableMapMap map)
{
	return map == TXN_MEMTABLE_MAP_MAP_FREE ? &TXN_MEMTABLE_MAP_FREE_TREE_OPS : &TXN_MEMTABLE_MAP_VALUES_TREE_OPS;
}

/* txn_memtable_map_named_most returns the longest value a take of a row's value ids writes: its count and each id. */
size_t
txn_memtable_map_named_most(size_t count)
{
	return STD_ADD(usize, 2, STD_MUL(usize, 4, count, 145, 9), 145, 5);
}

/*
 * txn_memtable_map_Entries_next gives the next version of a key's chain in a
 * map that isn't a values mark, which changes no entry, answering false on
 * an error and setting some for a version.
 */
static bool
txn_memtable_map_Entries_next(TxnMemtableVersionChain *chain, bool *some, TxnMemtableVersionVersion *result,
							  HostError *error)
{
	for (;;)
	{
		TxnMemtableVersionRecord record;

		if (!txn_memtable_version_Chain_Iterator_next(chain, some, result, error))
			return false;
		if (!*some)
			return true;
		record = txn_memtable_version_Version_record(result);
		if (txn_memtable_version_Record_op(&record).tag != TXN_MEMTABLE_VERSION_OP_REF_NAMED)
			return true;
		txn_memtable_version_Version_drop(result);
	}
}

/* txn_memtable_map_newest_in returns a key's newest version in a map's range that isn't a values mark, under its lock either way. */
static bool
txn_memtable_map_newest_in(const HostSharedGuard *guard, const HostDeltaHold *hold, const uint8 *key, size_t key_len,
						   bool *some, TxnMemtableVersionVersion *result, HostError *error)
{
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;

	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, key, key_len, some, &node, error))
		return false;
	if (!*some)
		return true;
	return txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error) &&
		txn_memtable_map_Entries_next(&chain, some, result, error);
}

/* txn_memtable_map_valued returns the value a map's version puts, or none for a removal. */
bool
txn_memtable_map_valued(const TxnMemtableVersionVersion *version, bool *some, StdBytes *result, HostError *error)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);

	*some = false;
	switch (op.tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
			*some = true;
			*result = op.u.put.value;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE:
		case TXN_MEMTABLE_VERSION_OP_REF_EMPTIED:
			return true;
		default:
			*error = txn_memtable_map_internal("a map's version neither puts nor deletes");
			return false;
	}
}

/* txn_memtable_map_lead reads the free space a free space map's entry leads with. */
static bool
txn_memtable_map_lead(StdBytes value, uint64 *result, HostError *error)
{
	size_t		at = 0;
	ChunksVarintMalformed malformed;
	StdString	message;

	if (chunks_varint_decode(value.ptr, value.len, &at, result, &malformed))
		return true;
	std_string_from_Checked(&message, "a free space map's entry is corrupt",
							strlen("a free space map's entry is corrupt"));
	*error = host_Error_fork((const uint8 *) "XX001", &message);
	return false;
}

/*
 * txn_memtable_map_claimed reports whether a take's claim was overtaken: a
 * free space map's key holds, at or above the claim's first version, a
 * version naming the claimed tuple id, and the version above that one puts at
 * least the bytes it left, or, with none above, the claim's own bytes.
 */
static bool
txn_memtable_map_claimed(const HostSharedGuard *guard, const HostDeltaHold *hold, const uint8 *key, size_t key_len,
						 HostRow tid, uint64 first, bool *result, HostError *error)
{
	bool		some;
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;
	bool		named = false;
	bool		named_bytes = false;
	uint64		named_put = 0;

	*result = false;
	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, key, key_len, &some, &node, error))
		return false;
	if (!some)
		return true;
	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error))
		return false;
	for (;;)
	{
		TxnMemtableVersionVersion version;
		TxnMemtableVersionRecord record;
		TxnMemtableVersionOpRef op;
		HostRow		tuple = {0};
		bool		has_bytes = true;
		uint64		bytes = 0;
		bool		ok = true;

		if (!txn_memtable_map_Entries_next(&chain, &some, &version, error))
			return false;
		if (!some)
			break;
		record = txn_memtable_version_Version_record(&version);
		op = txn_memtable_version_Record_op(&record);
		switch (op.tag)
		{
			case TXN_MEMTABLE_VERSION_OP_REF_PUT:
				tuple = op.u.put.tuple;
				ok = txn_memtable_map_lead(op.u.put.value, &bytes, error);
				break;
			case TXN_MEMTABLE_VERSION_OP_REF_EMPTIED:
				tuple = op.u.emptied;
				has_bytes = false;
				break;
			case TXN_MEMTABLE_VERSION_OP_REF_DELETE:
				break;
			default:
				*error = txn_memtable_map_internal("a map's version neither puts nor deletes");
				ok = false;
				break;
		}
		txn_memtable_version_Version_drop(&version);
		if (!ok)
			return false;
		if (!named)
		{
			if (tuple.f0 == tid.f0)
			{
				named = true;
				named_bytes = has_bytes;
				named_put = bytes;
			}
			continue;
		}
		*result = named_bytes && named_put >= (has_bytes ? bytes : 0);
		return true;
	}
	*result = named && named_bytes && named_put >= first;
	return true;
}

/*
 * txn_memtable_map_raised lowers a state's free space hints to a block whose
 * free space map entry rose to a category, from the one past the category it
 * was at, or from the lowest for a block with no entry in the delta.
 */
bool
txn_memtable_map_raised(HostSharedRecord state, const uint8 *key, size_t key_len, bool has_was, uint8 was,
						const uint8 *value, size_t value_len, HostError *error)
{
	uint64		bytes;
	uint8		now;

	if (key_len != TXN_MEMTABLE_MAP_KEY)
	{
		*error = txn_memtable_map_internal("a free space map's key isn't a block");
		return false;
	}
	if (!txn_free_space_value_bytes(value, value_len, &bytes, error))
		return false;
	now = txn_free_space_category(txn_free_space_avail(bytes));
	if (has_was && was >= now)
		return true;
	return txn_state_lowered(state, std_u32_from_be_bytes(key),
							 has_was ? (uint8) STD_ADD(u8, was, 1, 241, 58) : 0, now, error);
}

/* txn_memtable_map_versions_reading_t is versions' reading's captures: the key and the versions read. */
typedef struct txn_memtable_map_versions_reading_t
{
	const uint8 *key;
	StdVec	   *versions;
} txn_memtable_map_versions_reading_t;

/* txn_memtable_map_versions_reading reads a map key's whole chain under its range's shared lock. */
static bool
txn_memtable_map_versions_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
								  const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_map_versions_reading_t *reading = captures;
	bool		some;
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;

	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, reading->key, TXN_MEMTABLE_MAP_KEY,
								 &some, &node, error))
		return false;
	if (!some)
		return true;
	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error))
		return false;
	for (;;)
	{
		TxnMemtableVersionVersion version;

		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, error))
			return false;
		if (!some)
			return true;
		std_vec_push_Checked(reading->versions, &version, sizeof(version), _Alignof(TxnMemtableVersionVersion));
	}
}

/* txn_memtable_map_Version_drop is Version's drop glue for a list of them. */
static void
txn_memtable_map_Version_drop(void *self)
{
	txn_memtable_version_Version_drop(self);
}

/*
 * txn_memtable_map_versions lends a key's versions in one of a storage's
 * maps, newest first, for a caller's hold, read through the hand-off under
 * the range's shared lock, the values marks among them.
 */
bool
txn_memtable_map_versions(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
						  HostSharedRecord state, uint32 key, StdVec *result, HostError *error)
{
	uint8		bytes[TXN_MEMTABLE_MAP_KEY];
	txn_memtable_map_versions_reading_t reading = {bytes, result};
	bool		some;

	std_u32_to_be_bytes(key, bytes);
	std_vec_new(result, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	if (txn_memtable_table_lent(tree, io, hold, state, bytes, sizeof(bytes), txn_memtable_map_versions_reading,
								&reading, &some, error))
		return true;
	std_vec_clear(result, sizeof(TxnMemtableVersionVersion), txn_memtable_map_Version_drop);
	std_vec_free_Checked(result, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	return false;
}

/* txn_memtable_map_newest_reading_t is newest's reading's captures: the key and the entry read. */
typedef struct txn_memtable_map_newest_reading_t
{
	const uint8 *key;
	bool		some;
	TxnMemtableMapEntry entry;
} txn_memtable_map_newest_reading_t;

/* txn_memtable_map_newest_reading copies a map key's newest entry under its range's shared lock. */
static bool
txn_memtable_map_newest_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
								const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_map_newest_reading_t *reading = captures;
	TxnMemtableVersionVersion version;
	StdBytes	value;
	bool		ok;

	reading->some = false;
	if (!txn_memtable_map_newest_in(guard, hold, reading->key, TXN_MEMTABLE_MAP_KEY, &reading->some, &version,
									error))
		return false;
	if (!reading->some)
		return true;
	ok = txn_memtable_map_valued(&version, &reading->entry.has_value, &value, error);
	if (ok && reading->entry.has_value)
		std_slice_to_vec_Checked(&reading->entry.value, value.ptr, value.len, 1, 1);
	txn_memtable_version_Version_drop(&version);
	reading->some = ok;
	return ok;
}

/*
 * txn_memtable_map_newest returns a key's newest entry in a storage's map
 * outside any window: nothing for a key with none, and none for a removal.
 */
bool
txn_memtable_map_newest(HostIoIo *io, HostSharedRecord state, TxnMemtableMapMap map, uint32 key, bool *some,
						TxnMemtableMapEntry *result, HostError *error)
{
	uint8		bytes[TXN_MEMTABLE_MAP_KEY];
	txn_memtable_map_newest_reading_t reading = {bytes};
	bool		read;

	*some = false;
	std_u32_to_be_bytes(key, bytes);
	if (!txn_memtable_table_read(txn_memtable_map_tree(map), io, state, bytes, sizeof(bytes),
								 txn_memtable_map_newest_reading, &reading, &read, error))
		return false;
	*some = read && reading.some;
	if (*some)
		*result = reading.entry;
	return true;
}

/* txn_memtable_map_taken returns the tuple id a free space entry's version took, or names as its take's values mark, or 0 for none. */
HostRow
txn_memtable_map_taken(TxnMemtableVersionOpRef op)
{
	HostRow		none = {0};

	switch (op.tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
			return op.u.put.tuple;
		case TXN_MEMTABLE_VERSION_OP_REF_EMPTIED:
			return op.u.emptied;
		case TXN_MEMTABLE_VERSION_OP_REF_NAMED:
			return op.u.named.f0;
		default:
			return none;
	}
}

/*
 * txn_memtable_map_widened returns the full xid of a 32-bit ender a fold's
 * drop stored, widened against its take's writer's full xid: both were
 * named by delta versions within 2^31 of each other.
 */
static uint64
txn_memtable_map_widened(uint64 writer, uint32 ender)
{
	int32		apart = (int32) std_wrapping_sub_u32(ender, (uint32) writer);

	return std_wrapping_add_u64(writer, (uint64) (int64) apart);
}

/* txn_memtable_map_Taker_drop drops a taker: its entry's value and its value ids. */
static void
txn_memtable_map_Taker_drop(void *self_)
{
	txn_TupleWriterRowEntryU64TupleVecU32Bool_t *self = self_;

	if (self->f2.has_value)
		std_vec_free_Checked(&self->f2.value, 1, 1);
	std_vec_free_Checked(&self->f4.f0, sizeof(uint32), _Alignof(uint32));
}

/* txn_memtable_map_takers_drop frees takers' answer: each taker's entry's value and value ids, and the list. */
void
txn_memtable_map_takers_drop(StdVec *takers)
{
	std_vec_clear(takers, sizeof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t), txn_memtable_map_Taker_drop);
	std_vec_free_Checked(takers, sizeof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t),
						 _Alignof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t));
}

/* txn_memtable_map_takers_reading_t is takers' reading's captures: the key and the takers read. */
typedef struct txn_memtable_map_takers_reading_t
{
	const uint8 *key;
	StdVec		takers;
} txn_memtable_map_takers_reading_t;

/* txn_memtable_map_taker reads one version as a taker, answering false on an error. */
static bool
txn_memtable_map_taker(const TxnMemtableVersionVersion *version, txn_TupleWriterRowEntryU64TupleVecU32Bool_t *taker,
					   HostError *error)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);
	uint32		ender;

	taker->f0 = txn_memtable_version_Record_writer(&record);
	taker->f1 = txn_memtable_map_taken(op);
	taker->f3 = txn_memtable_version_Version_ender(version, &ender) ? txn_memtable_map_widened(taker->f0.xid, ender) :
		0;
	std_vec_new(&taker->f4.f0, sizeof(uint32), _Alignof(uint32));
	taker->f4.f1 = false;
	taker->f2.has_value = false;
	if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_NAMED)
	{
		TxnMemtableVersionValues values = txn_memtable_version_PutRef_values(&op.u.named.f1);
		uint32		id;

		while (txn_memtable_version_Values_Iterator_next(&values, &id))
			std_vec_push_Checked(&taker->f4.f0, &id, sizeof(id), _Alignof(uint32));
		taker->f4.f1 = true;
		return true;
	}
	if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_PUT)
	{
		taker->f2.has_value = true;
		std_slice_to_vec_Checked(&taker->f2.value, op.u.put.value.ptr, op.u.put.value.len, 1, 1);
		return true;
	}
	{
		StdBytes	value;

		if (!txn_memtable_map_valued(version, &taker->f2.has_value, &value, error))
		{
			std_vec_free_Checked(&taker->f4.f0, sizeof(uint32), _Alignof(uint32));
			return false;
		}
		if (taker->f2.has_value)
			std_slice_to_vec_Checked(&taker->f2.value, value.ptr, value.len, 1, 1);
	}
	return true;
}

/* txn_memtable_map_takers_reading_frame_0_t is takers' reading's state across its pad: a version and its taker, while live. */
typedef struct txn_memtable_map_takers_reading_frame_0_t
{
	txn_memtable_map_takers_reading_t *reading;
	const HostDeltaHold *hold;
	const HostSharedGuard *guard;
	HostError  *error;
	bool		ok;
	TxnMemtableVersionVersion version;
	bool		version_live;
	txn_TupleWriterRowEntryU64TupleVecU32Bool_t taker;
	bool		taker_live;
} txn_memtable_map_takers_reading_frame_0_t;

/* txn_memtable_map_takers_reading_pad_0 is takers' reading's pad: the taker, then the version, while live. */
static void
txn_memtable_map_takers_reading_pad_0(void *frame_)
{
	txn_memtable_map_takers_reading_frame_0_t *frame = frame_;

	if (frame->taker_live)
	{
		frame->taker_live = false;
		txn_memtable_map_Taker_drop(&frame->taker);
	}
	if (frame->version_live)
	{
		frame->version_live = false;
		txn_memtable_version_Version_drop(&frame->version);
	}
}

/* txn_memtable_map_takers_reading_body_0 is takers' reading's body. */
static void
txn_memtable_map_takers_reading_body_0(void *frame_)
{
	txn_memtable_map_takers_reading_frame_0_t *frame = frame_;
	txn_memtable_map_takers_reading_t *reading = frame->reading;
	bool		some;
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;

	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, frame->guard, frame->hold, reading->key,
								 TXN_MEMTABLE_MAP_KEY, &some, &node, frame->error))
		return;
	if (!some)
	{
		frame->ok = true;
		return;
	}
	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, frame->guard, frame->hold, node, &chain,
									frame->error))
		return;
	for (;;)
	{
		bool		taken;

		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &frame->version, frame->error))
			return;
		if (!some)
			break;
		frame->version_live = true;
		taken = txn_memtable_map_taker(&frame->version, &frame->taker, frame->error);
		frame->version_live = false;
		txn_memtable_version_Version_drop(&frame->version);
		if (!taken)
			return;
		frame->taker_live = true;
		std_vec_push_Checked(&reading->takers, &frame->taker, sizeof(frame->taker),
							 _Alignof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t));
		frame->taker_live = false;
	}
	std_slice_reverse(reading->takers.ptr, reading->takers.len, sizeof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t),
					  _Alignof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t));
	frame->ok = true;
}

/* txn_memtable_map_takers_reading reads a key's chain as takers, oldest first, under its range's shared lock. */
static bool
txn_memtable_map_takers_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
								const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_map_takers_reading_frame_0_t frame = {.reading = captures, .hold = hold, .guard = guard,
	.error = error};

	std_landing(&frame, txn_memtable_map_takers_reading_body_0, txn_memtable_map_takers_reading_pad_0);
	return frame.ok;
}

/* txn_memtable_map_takers_frame_0_t is takers' state across its pad: the takers read, while live. */
typedef struct txn_memtable_map_takers_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	TxnMemtableMapMap map;
	uint8		bytes[TXN_MEMTABLE_MAP_KEY];
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	txn_memtable_map_takers_reading_t reading;
	bool		takers_live;
} txn_memtable_map_takers_frame_0_t;

/* txn_memtable_map_takers_pad_0 is takers' pad: the takers read, while live. */
static void
txn_memtable_map_takers_pad_0(void *frame_)
{
	txn_memtable_map_takers_frame_0_t *frame = frame_;

	if (frame->takers_live)
	{
		frame->takers_live = false;
		txn_memtable_map_takers_drop(&frame->reading.takers);
	}
}

/* txn_memtable_map_takers_body_0 is takers' body. */
static void
txn_memtable_map_takers_body_0(void *frame_)
{
	txn_memtable_map_takers_frame_0_t *frame = frame_;
	bool		some;

	frame->reading.key = frame->bytes;
	std_vec_new(&frame->reading.takers, sizeof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t),
				_Alignof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t));
	frame->takers_live = true;
	if (!txn_memtable_table_read(txn_memtable_map_tree(frame->map), frame->io, frame->state, frame->bytes,
								 sizeof(frame->bytes), txn_memtable_map_takers_reading, &frame->reading, &some,
								 frame->error))
	{
		txn_memtable_map_takers_pad_0(frame);
		return;
	}
	*frame->result = frame->reading.takers;
	frame->takers_live = false;
	frame->ok = true;
}

/*
 * txn_memtable_map_takers returns a key's versions in a storage's map as
 * VACUUM reads them, oldest first: each one's writer, the tuple id its take
 * took or names, its entry, its ender widened to a full xid, and its row's
 * value ids with whether it is a values mark.
 */
bool
txn_memtable_map_takers(HostIoIo *io, HostSharedRecord state, TxnMemtableMapMap map, uint32 key, StdVec *result,
						HostError *error)
{
	txn_memtable_map_takers_frame_0_t frame = {.io = io, .state = state, .map = map, .result = result,
	.error = error};

	std_u32_to_be_bytes(key, frame.bytes);
	std_landing(&frame, txn_memtable_map_takers_body_0, txn_memtable_map_takers_pad_0);
	return frame.ok;
}

/* txn_memtable_map_rows_reading_t is a free space map reading's captures: the key, the tuple id, the ender or the LSN, and the answer. */
typedef struct txn_memtable_map_rows_reading_t
{
	const uint8 *key;
	HostRow		tid;
	uint32		ender;
	uint64		lsn;
	bool		found;
	StdVec		tids;
} txn_memtable_map_rows_reading_t;

/* txn_memtable_map_ended_reading stores the ender in the version of a free space entry that took the tuple id. */
static bool
txn_memtable_map_ended_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
							   const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_map_rows_reading_t *reading = captures;
	bool		some;
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;

	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, reading->key, TXN_MEMTABLE_MAP_KEY,
								 &some, &node, error))
		return false;
	if (!some)
		return true;
	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error))
		return false;
	for (;;)
	{
		TxnMemtableVersionVersion version;
		TxnMemtableVersionRecord record;
		bool		taken;

		if (!txn_memtable_map_Entries_next(&chain, &some, &version, error))
			return false;
		if (!some)
			return true;
		record = txn_memtable_version_Version_record(&version);
		taken = txn_memtable_map_taken(txn_memtable_version_Record_op(&record)).f0 == reading->tid.f0;
		if (taken)
			txn_memtable_version_Version_end(&version, reading->ender);
		txn_memtable_version_Version_drop(&version);
		if (taken)
			return true;
	}
}

/*
 * txn_memtable_map_ended stores a fold's drop's ender in the free space
 * map's version that took a tuple id, so VACUUM reads the row's end.
 */
bool
txn_memtable_map_ended(HostIoIo *io, HostSharedRecord state, HostRow tid, uint32 ender, HostError *error)
{
	uint8		bytes[TXN_MEMTABLE_MAP_KEY];
	txn_memtable_map_rows_reading_t reading = {bytes, tid, ender};
	bool		some;

	std_u32_to_be_bytes((uint32) STD_SHR_FOLDED(u64, tid.f0, 16), bytes);
	return txn_memtable_table_read(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, state, bytes, sizeof(bytes),
								   txn_memtable_map_ended_reading, &reading, &some, error);
}

/* txn_memtable_map_marked_reading collects the tuple ids a block's free space entries put at an LSN. */
static bool
txn_memtable_map_marked_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
								const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_map_rows_reading_t *reading = captures;
	bool		some;
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;

	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, reading->key, TXN_MEMTABLE_MAP_KEY,
								 &some, &node, error))
		return false;
	if (!some)
		return true;
	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error))
		return false;
	for (;;)
	{
		TxnMemtableVersionVersion version;
		TxnMemtableVersionRecord record;
		TxnMemtableVersionOpRef op;
		uint64		at_lsn;

		if (!txn_memtable_map_Entries_next(&chain, &some, &version, error))
			return false;
		if (!some)
			return true;
		record = txn_memtable_version_Version_record(&version);
		op = txn_memtable_version_Record_op(&record);
		if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_PUT && txn_memtable_version_Record_lsn(&record, &at_lsn) &&
			at_lsn == reading->lsn && op.u.put.tuple.f0 != 0)
			std_vec_push_Checked(&reading->tids, &op.u.put.tuple, sizeof(HostRow), _Alignof(HostRow));
		txn_memtable_version_Version_drop(&version);
	}
}

/*
 * txn_memtable_map_marked returns the tuple ids a block's free space map
 * entries put with an LSN, the takes a row change record's redo made.
 */
bool
txn_memtable_map_marked(HostIoIo *io, HostSharedRecord state, uint32 block, uint64 lsn, StdVec *result,
						HostError *error)
{
	uint8		bytes[TXN_MEMTABLE_MAP_KEY];
	txn_memtable_map_rows_reading_t reading = {.key = bytes, .lsn = lsn};
	bool		some;

	std_u32_to_be_bytes(block, bytes);
	std_vec_new(&reading.tids, sizeof(HostRow), _Alignof(HostRow));
	if (!txn_memtable_table_read(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, state, bytes, sizeof(bytes),
								 txn_memtable_map_marked_reading, &reading, &some, error))
	{
		std_vec_free_Checked(&reading.tids, sizeof(HostRow), _Alignof(HostRow));
		return false;
	}
	*result = reading.tids;
	return true;
}

/* txn_memtable_map_marks_reading looks for a values mark naming a tuple id with an LSN in a block's chain. */
static bool
txn_memtable_map_marks_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
							   const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_map_rows_reading_t *reading = captures;
	bool		some;
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;

	reading->found = false;
	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, reading->key, TXN_MEMTABLE_MAP_KEY,
								 &some, &node, error))
		return false;
	if (!some)
		return true;
	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error))
		return false;
	for (;;)
	{
		TxnMemtableVersionVersion version;
		TxnMemtableVersionRecord record;
		TxnMemtableVersionOpRef op;
		uint64		at_lsn;

		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, error))
			return false;
		if (!some)
			return true;
		record = txn_memtable_version_Version_record(&version);
		op = txn_memtable_version_Record_op(&record);
		reading->found = op.tag == TXN_MEMTABLE_VERSION_OP_REF_NAMED && op.u.named.f0.f0 == reading->tid.f0 &&
			txn_memtable_version_Record_lsn(&record, &at_lsn) && at_lsn == reading->lsn;
		txn_memtable_version_Version_drop(&version);
		if (reading->found)
			return true;
	}
}

/*
 * txn_memtable_map_marks reports whether a block's free space map holds the
 * values mark naming a tuple id with an LSN, so a redo skips a mark it
 * already made.
 */
bool
txn_memtable_map_marks(HostIoIo *io, HostSharedRecord state, uint32 block, HostRow tid, uint64 lsn, bool *result,
					   HostError *error)
{
	uint8		bytes[TXN_MEMTABLE_MAP_KEY];
	txn_memtable_map_rows_reading_t reading = {.key = bytes, .tid = tid, .lsn = lsn};
	bool		some;

	std_u32_to_be_bytes(block, bytes);
	if (!txn_memtable_table_read(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, state, bytes, sizeof(bytes),
								 txn_memtable_map_marks_reading, &reading, &some, error))
		return false;
	*result = some && reading.found;
	return true;
}

/*
 * txn_memtable_map_holds reports whether a storage's map, in its memtable
 * or, for the value map, its spills, holds the version a record of an LSN
 * pushed, so its redo skips the push.
 */
bool
txn_memtable_map_holds(HostIoIo *io, HostSharedRecord state, TxnMemtableMapMap map, uint32 key, uint64 lsn,
					   bool *result, HostError *error)
{
	uint8		bytes[TXN_MEMTABLE_MAP_KEY];

	std_u32_to_be_bytes(key, bytes);
	if (!txn_memtable_table_held(txn_memtable_map_tree(map), io, state, bytes, sizeof(bytes), lsn, result, error))
		return false;
	return map == TXN_MEMTABLE_MAP_MAP_FREE || *result ||
		txn_spill_holds(io, state, TXN_RUNS_KIND_SPILLED_VALUES, bytes, sizeof(bytes), lsn, result, error);
}

/* txn_memtable_map_cut_reading_t is cut's reading's captures: the state and the cut read. */
typedef struct txn_memtable_map_cut_reading_t
{
	HostSharedRecord state;
	txn_TupleU64VecVecU8_t cut;
} txn_memtable_map_cut_reading_t;

/* txn_memtable_map_cut_reading reads the tuple id counter and its block's free space records, under the range's shared lock. */
static bool
txn_memtable_map_cut_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
							 const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_map_cut_reading_t *reading = captures;
	uint8		key[TXN_MEMTABLE_MAP_KEY];
	bool		some;
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;

	reading->cut.f0 = txn_state_counter(reading->state);
	std_u32_to_be_bytes((uint32) STD_SHR_FOLDED(u64, reading->cut.f0, 16), key);
	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, key, sizeof(key), &some, &node,
								 error))
		return false;
	if (!some)
		return true;
	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error))
		return false;
	for (;;)
	{
		TxnMemtableVersionVersion version;
		StdVec		stored;

		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, error))
			return false;
		if (!some)
			return true;
		txn_memtable_version_Version_stored(&version, &stored);
		txn_memtable_version_Version_drop(&version);
		std_vec_push_Checked(&reading->cut.f1, &stored, sizeof(stored), _Alignof(StdVec));
	}
}

/* txn_memtable_map_bytes_drop drops a Vec<u8> in a list of them. */
static void
txn_memtable_map_bytes_drop(void *self)
{
	std_vec_free_Checked((StdVec *) self, 1, 1);
}

/*
 * txn_memtable_map_cut returns a storage's tuple id counter with the records
 * of its block's free space map key, read together under the range's shared
 * lock that holds the map's last key, or nothing for a storage with no free
 * space map's memtable.
 */
bool
txn_memtable_map_cut(HostIoIo *io, HostSharedRecord state, bool *some, txn_TupleU64VecVecU8_t *result,
					 HostError *error)
{
	uint8		bytes[TXN_MEMTABLE_MAP_KEY];
	txn_memtable_map_cut_reading_t reading = {state};
	bool		ok;

	*some = false;
	std_u32_to_be_bytes(PG_UINT32_MAX, bytes);
	std_vec_new(&reading.cut.f1, sizeof(StdVec), _Alignof(StdVec));
	ok = txn_memtable_table_read(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, state, bytes, sizeof(bytes),
								 txn_memtable_map_cut_reading, &reading, some, error);
	if (!ok || !*some)
	{
		std_vec_clear(&reading.cut.f1, sizeof(StdVec), txn_memtable_map_bytes_drop);
		std_vec_free_Checked(&reading.cut.f1, sizeof(StdVec), _Alignof(StdVec));
		*some = false;
		return ok;
	}
	*result = reading.cut;
	return true;
}

/* txn_memtable_map_seek_reading_t is seek's reading's captures: the walk's place and the entry found. */
typedef struct txn_memtable_map_seek_reading_t
{
	const StdVec *place;
	bool		some;
	txn_TupleU32Entry_t found;
} txn_memtable_map_seek_reading_t;

/* txn_memtable_map_seek_reading finds the first key at or past the place with an entry, before the step's stop. */
static bool
txn_memtable_map_seek_reading(void *captures, const HostDeltaHold *hold, const HostSharedGuard *guard, bool has_stop,
							  const uint8 *stop, size_t stop_len, HostError *error)
{
	txn_memtable_map_seek_reading_t *reading = captures;
	bool		some;
	TxnMemtableIndexNode node;

	reading->some = false;
	if (!txn_memtable_index_seek(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, reading->place->ptr,
								 reading->place->len, &some, &node, error))
		return false;
	while (some)
	{
		HostDeltaLent key;
		StdBytes	bytes;
		TxnMemtableVersionChain chain;
		TxnMemtableVersionVersion version;
		bool		has;
		bool		ok;

		if (!txn_memtable_index_key(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &key, error))
			return false;
		bytes = host_delta_Lent_Deref_deref(&key);
		if (has_stop && std_bytes_Ord_cmp(bytes.ptr, bytes.len, stop, stop_len) != STD_ORDERING_LESS)
		{
			host_delta_Lent_drop(&key);
			return true;
		}
		ok = txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error) &&
			txn_memtable_map_Entries_next(&chain, &has, &version, error);
		if (ok && has)
		{
			StdBytes	value;

			if (bytes.len != TXN_MEMTABLE_MAP_KEY)
			{
				*error = txn_memtable_map_internal("a map's key isn't 4 bytes");
				ok = false;
			}
			else
			{
				reading->found.f0 = std_u32_from_be_bytes(bytes.ptr);
				ok = txn_memtable_map_valued(&version, &reading->found.f1.has_value, &value, error);
				if (ok && reading->found.f1.has_value)
					std_slice_to_vec_Checked(&reading->found.f1.value, value.ptr, value.len, 1, 1);
				reading->some = ok;
			}
			txn_memtable_version_Version_drop(&version);
		}
		host_delta_Lent_drop(&key);
		if (!ok || reading->some)
			return ok;
		if (!txn_memtable_index_next(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &some, &node, error))
			return false;
	}
	return true;
}

/* txn_memtable_map_seek_frame_0_t is seek's state across its pad: the place, a step's hold and stop, and the entry found in its captures, while live. */
typedef struct txn_memtable_map_seek_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	TxnMemtableMapMap map;
	uint32		from;
	bool	   *some;
	txn_TupleU32Entry_t *result;
	HostError  *error;
	bool		ok;
	StdVec		place;
	bool		place_live;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		stop;
	bool		stop_live;
	txn_memtable_map_seek_reading_t reading;
} txn_memtable_map_seek_frame_0_t;

/* txn_memtable_map_seek_pad_0 is seek's pad: the entry found, the stop, the hold, and the place, while live. */
static void
txn_memtable_map_seek_pad_0(void *frame_)
{
	txn_memtable_map_seek_frame_0_t *frame = frame_;

	if (frame->reading.some)
	{
		frame->reading.some = false;
		if (frame->reading.found.f1.has_value)
			std_vec_free_Checked(&frame->reading.found.f1.value, 1, 1);
	}
	if (frame->stop_live)
	{
		frame->stop_live = false;
		std_vec_free_Checked(&frame->stop, 1, 1);
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	if (frame->place_live)
	{
		frame->place_live = false;
		std_vec_free_Checked(&frame->place, 1, 1);
	}
}

/* txn_memtable_map_seek_body_0 is seek's body. */
static void
txn_memtable_map_seek_body_0(void *frame_)
{
	txn_memtable_map_seek_frame_0_t *frame = frame_;

	*frame->some = false;
	frame->reading.place = &frame->place;
	std_vec_with_capacity_Checked(&frame->place, TXN_MEMTABLE_MAP_KEY, 1, 1);
	frame->place_live = true;
	frame->place.len = TXN_MEMTABLE_MAP_KEY;
	std_u32_to_be_bytes(frame->from, frame->place.ptr);
	for (;;)
	{
		bool		stepped;
		bool		has_stop;
		bool		ok;

		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		{
			txn_memtable_map_seek_pad_0(frame);
			return;
		}
		frame->hold_live = true;
		ok = txn_memtable_table_stepped(txn_memtable_map_tree(frame->map), frame->io, &frame->hold, frame->state, true,
										frame->place.ptr, frame->place.len, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD,
										txn_memtable_map_seek_reading, &frame->reading, &stepped, &has_stop,
										&frame->stop, frame->error);
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
		if (!ok)
		{
			txn_memtable_map_seek_pad_0(frame);
			return;
		}
		frame->stop_live = has_stop;
		if (!stepped)
			break;
		if (frame->reading.some)
		{
			frame->reading.some = false;
			*frame->some = true;
			*frame->result = frame->reading.found;
			break;
		}
		if (!has_stop)
			break;
		std_vec_free_Checked(&frame->place, 1, 1);
		frame->place = frame->stop;
		frame->stop_live = false;
	}
	frame->ok = true;
	txn_memtable_map_seek_pad_0(frame);
}

/*
 * txn_memtable_map_seek returns the first key at or past a key in a
 * storage's map with an entry, and that entry, walking step by step, or
 * nothing past the last.
 */
bool
txn_memtable_map_seek(HostIoIo *io, HostSharedRecord state, TxnMemtableMapMap map, uint32 from, bool *some,
					  txn_TupleU32Entry_t *result, HostError *error)
{
	txn_memtable_map_seek_frame_0_t frame = {.io = io, .state = state, .map = map, .from = from, .some = some,
	.result = result, .error = error};

	std_landing(&frame, txn_memtable_map_seek_body_0, txn_memtable_map_seek_pad_0);
	return frame.ok;
}

/* txn_memtable_map_named_t is named's step's captures: the change's writer, its LSN, the row's tuple id, and its value ids. */
typedef struct txn_memtable_map_named_t
{
	TxnMemtableVersionWriter writer;
	bool		has_lsn;
	uint64		lsn;
	HostRow		tuple;
	StdVec	   *values;
} txn_memtable_map_named_t;

/* txn_memtable_map_named_step pushes a values mark of the row with the reserve. */
static bool
txn_memtable_map_named_step(void *captures, TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
							const uint8 *key, size_t key_len, TxnMemtableTableReserve *reserve, HostError *error)
{
	txn_memtable_map_named_t *named = captures;
	TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_NAMED};
	TxnMemtableVersionChange change = {.writer = named->writer, .op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
	.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}, .has_lsn = named->has_lsn, .lsn = named->lsn};
	TxnMemtableVersionHandle handle;
	bool		ok;

	op.u.named.f0 = named->tuple;
	op.u.named.f1 = *named->values;
	std_vec_new(named->values, sizeof(uint32), _Alignof(uint32));
	ok = txn_memtable_table_Reserve_push(reserve, open, hold, key, key_len, &change, &handle, error);
	txn_memtable_Op_drop(&op);
	return ok;
}

/*
 * txn_memtable_map_named pushes a take's values mark inside the window: the
 * row's tuple id and its value ids, under the take's key, changing no entry,
 * taking the value ids.
 */
bool
txn_memtable_map_named(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t take,
					   TxnMemtableVersionWriter writer, bool has_lsn, uint64 lsn, HostRow tuple, StdVec *values,
					   HostError *error)
{
	txn_memtable_map_named_t named = {writer, has_lsn, lsn, tuple, values};
	bool		ok = txn_memtable_table_within(io, prepared, txn_memtable_table_Prepared_target(prepared, take),
											   txn_memtable_map_named_step, &named, error);

	std_vec_free_Checked(values, sizeof(uint32), _Alignof(uint32));
	return ok;
}

/* txn_memtable_map_took_t is took's step's captures: the take's inputs, its closures, and its answer. */
typedef struct txn_memtable_map_took_t
{
	HostSharedRecord state;
	bool		free;
	TxnMemtableVersionWriter writer;
	bool		has_lsn;
	uint64		lsn;
	bool		has_claim;
	HostRow		claim_tid;
	uint64		claim_bytes;
	bool		has_base;
	const uint8 *base;
	size_t		base_len;
	TxnMemtableMapStep (*step) (void *captures, bool has_current, const uint8 *current, size_t current_len,
								StdVec *out);
	HostRow		(*tuple) (void *captures);
	void		(*then) (void *captures);
	void	   *captures;
	TxnMemtableMapTaken *result;
} txn_memtable_map_took_t;

/* txn_memtable_map_took_pushed pushes the step's put or removal with the reserve and runs then, answering false on an error. */
static bool
txn_memtable_map_took_pushed(txn_memtable_map_took_t *took, TxnMemtableIndexKeyedOpen *open,
							 const HostDeltaHold *hold, const uint8 *key, size_t key_len,
							 TxnMemtableTableReserve *reserve, TxnMemtableMapStep answer, StdVec *value, bool has_was,
							 uint8 was, HostError *error)
{
	HostRow		tuple = took->tuple(took->captures);
	TxnMemtableOp op = {0};
	TxnMemtableVersionChange change = {.writer = took->writer, .op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
	.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}, .has_lsn = took->has_lsn, .lsn = took->lsn};
	TxnMemtableVersionHandle handle;
	bool		ok;

	if (answer == TXN_MEMTABLE_MAP_STEP_PUT)
	{
		op.tag = TXN_MEMTABLE_OP_PUT;
		op.u.put.value = *value;
		std_vec_new(&op.u.put.addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
		std_vec_new(&op.u.put.values, sizeof(uint32), _Alignof(uint32));
		op.u.put.tuple = tuple;
	}
	else
	{
		reserve->value = *value;
		op.tag = tuple.f0 == 0 ? TXN_MEMTABLE_OP_DELETE : TXN_MEMTABLE_OP_EMPTIED;
		op.u.emptied = tuple;
	}
	ok = txn_memtable_table_Reserve_push(reserve, open, hold, key, key_len, &change, &handle, error);
	if (op.tag == TXN_MEMTABLE_OP_PUT)
		reserve->value = op.u.put.value;
	if (!ok)
		return false;
	if (took->free && op.tag == TXN_MEMTABLE_OP_PUT &&
		!txn_memtable_map_raised(took->state, key, key_len, has_was, was, reserve->value.ptr, reserve->value.len,
								 error))
		return false;
	took->then(took->captures);
	*took->result = TXN_MEMTABLE_MAP_TAKEN_TOOK;
	return true;
}

/* txn_memtable_map_took_step is took's step at the open range that holds the take's key. */
static bool
txn_memtable_map_took_step(void *captures, TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
						   const uint8 *key, size_t key_len, TxnMemtableTableReserve *reserve, HostError *error)
{
	txn_memtable_map_took_t *took = captures;
	bool		some;
	TxnMemtableVersionVersion newest;
	bool		has_delta = false;
	StdBytes	delta = {0};
	bool		has_was = false;
	uint8		was = 0;
	StdVec		value;
	TxnMemtableMapStep answer;
	bool		ok;

	if (took->has_claim)
	{
		bool		claimed;

		if (!txn_memtable_map_claimed(open->open.guard, hold, key, key_len, took->claim_tid, took->claim_bytes,
									  &claimed, error))
			return false;
		if (claimed)
		{
			*took->result = TXN_MEMTABLE_MAP_TAKEN_REFUSED;
			return true;
		}
	}
	value = reserve->value;
	std_vec_new(&reserve->value, 1, 1);
	if (!txn_memtable_map_newest_in(open->open.guard, hold, key, key_len, &some, &newest, error))
	{
		reserve->value = value;
		return false;
	}
	ok = !some || txn_memtable_map_valued(&newest, &has_delta, &delta, error);
	if (ok && took->free && has_delta)
	{
		uint64		bytes;

		ok = txn_free_space_value_bytes(delta.ptr, delta.len, &bytes, error);
		has_was = ok;
		was = ok ? txn_free_space_category(txn_free_space_avail(bytes)) : 0;
	}
	if (!ok)
	{
		if (some)
			txn_memtable_version_Version_drop(&newest);
		reserve->value = value;
		return false;
	}
	answer = some ? took->step(took->captures, has_delta, delta.ptr, delta.len, &value) :
		took->step(took->captures, took->has_base, took->base, took->base_len, &value);
	if (some)
		txn_memtable_version_Version_drop(&newest);
	if (answer == TXN_MEMTABLE_MAP_STEP_RETRY || answer == TXN_MEMTABLE_MAP_STEP_REFUSE)
	{
		reserve->value = value;
		*took->result = answer == TXN_MEMTABLE_MAP_STEP_RETRY ? TXN_MEMTABLE_MAP_TAKEN_RETRY :
			TXN_MEMTABLE_MAP_TAKEN_REFUSED;
		return true;
	}
	return txn_memtable_map_took_pushed(took, open, hold, key, key_len, reserve, answer, &value, has_was, was, error);
}

/*
 * txn_memtable_map_took_core is took's body at every kept value: inside the
 * window, under the take's range's lock, a take with a claim is refused when
 * the claim was overtaken; the step reads the key's newest entry in the
 * delta, else the base's value given, and answers a put, a removal, a try
 * again, or a refusal; a put or a removal is pushed with the reserve, its
 * tuple id the one the kept value names, a free space map's put lowers the
 * state's hints to the block, and then runs on the kept value. step, tuple,
 * and then share captures, through which the kept value goes; each runs at
 * most once.
 */
bool
txn_memtable_map_took_core(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t take,
						   TxnMemtableVersionWriter writer, bool has_lsn, uint64 lsn, bool has_claim,
						   HostRow claim_tid, uint64 claim_bytes, bool has_base, const uint8 *base, size_t base_len,
						   TxnMemtableMapStep (*step) (void *captures, bool has_current, const uint8 *current,
													   size_t current_len, StdVec *out),
						   HostRow (*tuple) (void *captures), void (*then) (void *captures), void *captures,
						   TxnMemtableMapTaken *result, HostError *error)
{
	size_t		target = txn_memtable_table_Prepared_target(prepared, take);
	HostSharedRecord state;
	const TxnMemtableTableTreeOps *tree;
	txn_memtable_map_took_t took = {.writer = writer, .has_lsn = has_lsn, .lsn = lsn, .has_claim = has_claim,
		.claim_tid = claim_tid, .claim_bytes = claim_bytes, .has_base = has_base, .base = base, .base_len = base_len,
	.step = step, .tuple = tuple, .then = then, .captures = captures, .result = result};

	if (!txn_memtable_table_Prepared_aimed(prepared, target, &state, &tree, error))
		return false;
	took.state = state;
	took.free = tree == &TXN_MEMTABLE_MAP_FREE_TREE_OPS;
	return txn_memtable_table_within(io, prepared, target, txn_memtable_map_took_step, &took, error);
}

/* The map's takes that keep nothing, and its prepare whose closure returns nothing, which map.c defines. */
#define TXN_MEMTABLE_MAP_RS Unit
#define TXN_MEMTABLE_MAP_RETURNS bool
#define TXN_MEMTABLE_MAP_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_MAP_TS Unit
#define TXN_MEMTABLE_MAP_KEPT_OUT
#define TXN_MEMTABLE_MAP_KEPT_IN
#define TXN_MEMTABLE_MAP_DECLARE
#define TXN_MEMTABLE_MAP_DEFINE
#define TXN_MEMTABLE_MAP_RESULT_FIELDS
#define TXN_MEMTABLE_MAP_RESULT_INIT
#define TXN_MEMTABLE_MAP_RESULT_ARGS(c)
#define TXN_MEMTABLE_MAP_RESULT_ERROR error
#define TXN_MEMTABLE_MAP_KEPT_ARG NULL
#define TXN_MEMTABLE_MAP_KEPT_CALL(k)
#define TXN_MEMTABLE_MAP_KEPT_DROP(k)
#include "doltgres/txn/memtable/map_template.h"

/* The map-only prepare whose closure answers a bool, which map.c defines. */
#define TXN_MEMTABLE_MAP_RS Bool
#define TXN_MEMTABLE_MAP_RETURNS bool
#define TXN_MEMTABLE_MAP_RESULT_PARAMS , bool *result, HostError *error
#define TXN_MEMTABLE_MAP_DECLARE
#define TXN_MEMTABLE_MAP_DEFINE
#define TXN_MEMTABLE_MAP_RESULT_FIELDS bool *result;
#define TXN_MEMTABLE_MAP_RESULT_INIT , result
#define TXN_MEMTABLE_MAP_RESULT_ARGS(c) , (c)->result
#define TXN_MEMTABLE_MAP_RESULT_ERROR error
#include "doltgres/txn/memtable/map_template.h"

/* The map-only prepare whose closure returns what its take did, which map.c defines where Taken lives. */
#define TXN_MEMTABLE_MAP_RS Taken
#define TXN_MEMTABLE_MAP_RETURNS bool
#define TXN_MEMTABLE_MAP_RESULT_PARAMS , TxnMemtableMapTaken *result, HostError *error
#define TXN_MEMTABLE_MAP_DECLARE
#define TXN_MEMTABLE_MAP_DEFINE
#define TXN_MEMTABLE_MAP_RESULT_FIELDS TxnMemtableMapTaken *result;
#define TXN_MEMTABLE_MAP_RESULT_INIT , result
#define TXN_MEMTABLE_MAP_RESULT_ARGS(c) , (c)->result
#define TXN_MEMTABLE_MAP_RESULT_ERROR error
#include "doltgres/txn/memtable/map_template.h"

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/simc/simc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/test.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/testing.h"

/* txn_memtable_map_tests_seen_drop drops a take's kept Seen, an Option<Vec<u8>>. */
void
txn_memtable_map_tests_seen_drop(void *kept)
{
	TxnMemtableMapEntry *seen = kept;

	if (seen->has_value)
		std_vec_free_Checked(&seen->value, 1, 1);
}

#define TXN_MEMTABLE_MAP_TS Seen
#define TXN_MEMTABLE_MAP_KEPT_OUT , TxnMemtableMapEntry *kept
#define TXN_MEMTABLE_MAP_KEPT_IN , const TxnMemtableMapEntry *kept
#define TXN_MEMTABLE_MAP_KEPT_ARG kept
#define TXN_MEMTABLE_MAP_KEPT_CALL(k) , (k)
#define TXN_MEMTABLE_MAP_KEPT_DROP(k) txn_memtable_map_tests_seen_drop(k)
#define TXN_MEMTABLE_MAP_DECLARE
#define TXN_MEMTABLE_MAP_DEFINE
#include "doltgres/txn/memtable/map_template.h"

#define TXN_MEMTABLE_TABLE_RS TakenSeen
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , TxnMemtableMapTaken *result_0, TxnMemtableMapEntry *result_1, \
	uint32 *result_2, HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#define TXN_MEMTABLE_TABLE_DEFINE
#define TXN_MEMTABLE_TABLE_RESULT_FIELDS TxnMemtableMapTaken *result_0; TxnMemtableMapEntry *result_1; \
	uint32 *result_2;
#define TXN_MEMTABLE_TABLE_RESULT_INIT , result_0, result_1, result_2
#define TXN_MEMTABLE_TABLE_RESULT_ARGS(c) , (c)->result_0, (c)->result_1, (c)->result_2
#define TXN_MEMTABLE_TABLE_RESULT_ERROR error
#include "doltgres/txn/memtable/table_template.h"

/* txn_memtable_map_tests_Answer is a Case's answer: the step a take's function gives from the key's Seen, the Seen it keeps beside a put or a removal. */
typedef TxnMemtableMapStep (*txn_memtable_map_tests_Answer) (TxnMemtableMapEntry seen, TxnMemtableMapEntry *kept);

/* txn_memtable_map_tests_put is Step::Put as a Case's answer: a put keeping the Seen. */
static TxnMemtableMapStep
txn_memtable_map_tests_put(TxnMemtableMapEntry seen, TxnMemtableMapEntry *kept)
{
	*kept = seen;
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_memtable_map_tests_remove is Step::Remove as a Case's answer: a removal keeping the Seen. */
static TxnMemtableMapStep
txn_memtable_map_tests_remove(TxnMemtableMapEntry seen, TxnMemtableMapEntry *kept)
{
	*kept = seen;
	return TXN_MEMTABLE_MAP_STEP_REMOVE;
}

/* txn_memtable_map_tests_retry is the maps' case's answer of a retry. */
static TxnMemtableMapStep
txn_memtable_map_tests_retry(TxnMemtableMapEntry seen, TxnMemtableMapEntry *kept)
{
	txn_memtable_map_tests_seen_drop(&seen);
	return TXN_MEMTABLE_MAP_STEP_RETRY;
}

/* txn_memtable_map_tests_refuse is the maps' case's answer of a refusal. */
static TxnMemtableMapStep
txn_memtable_map_tests_refuse(TxnMemtableMapEntry seen, TxnMemtableMapEntry *kept)
{
	txn_memtable_map_tests_seen_drop(&seen);
	return TXN_MEMTABLE_MAP_STEP_REFUSE;
}

/* txn_memtable_map_tests_taken_t is taken's captures: its take's step's answer and value, the change, and what the take did. */
typedef struct txn_memtable_map_tests_taken_t
{
	txn_memtable_map_tests_Answer answer;
	StdBytes	value;
	const TxnMemtableVersionChange *change;
	uint32		ran;
} txn_memtable_map_tests_taken_t;

/* txn_memtable_map_tests_taken_step is taken's step: the value written, and the answer at what the take saw of its key. */
static TxnMemtableMapStep
txn_memtable_map_tests_taken_step(void *captures, bool has_current, const uint8 *current, size_t current_len,
								  StdVec *out, TxnMemtableMapEntry *kept)
{
	txn_memtable_map_tests_taken_t *c = captures;
	TxnMemtableMapEntry seen = {has_current};

	std_vec_extend_from_slice_Checked(out, c->value.ptr, c->value.len, 1, 1);
	if (has_current)
		std_slice_to_vec_Checked(&seen.value, current, current_len, 1, 1);
	return c->answer(seen, kept);
}

/* txn_memtable_map_tests_taken_then is taken's then: it counts its runs. */
static void
txn_memtable_map_tests_taken_then(void *captures, const TxnMemtableMapEntry *kept)
{
	txn_memtable_map_tests_taken_t *c = captures;

	c->ran++;
}

/* txn_memtable_map_tests_taken_inside is taken's prepare's closure: the take's finish, then the row's. */
static bool
txn_memtable_map_tests_taken_inside(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
									TxnMemtableMapTaken *result_0, TxnMemtableMapEntry *result_1, uint32 *result_2,
									HostError *error)
{
	txn_memtable_map_tests_taken_t *c = captures;
	TxnMemtableVersionHandle handle;
	TxnStatusRoomed roomed = {0};

	c->ran = 0;
	if (!txn_memtable_map_finish_Seen(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, txn_memtable_testing_writer(7), false, 0,
									  true, (const uint8 *) "base", 4, txn_memtable_map_tests_taken_step, c, NULL,
									  txn_memtable_map_tests_taken_then, c, NULL, result_0, result_1, error))
		return false;
	if (!txn_memtable_table_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, c->change, false, roomed,
								   &handle, error))
	{
		if (*result_0 == TXN_MEMTABLE_MAP_TAKEN_TOOK)
			txn_memtable_map_tests_seen_drop(result_1);
		return false;
	}
	*result_2 = c->ran;
	return true;
}

/* txn_memtable_map_tests_taken_frame_0_t is taken's state across its pad: its arguments, and its put, while live. */
typedef struct txn_memtable_map_tests_taken_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	TxnMemtableMapTake take;
	StdBytes	row;
	txn_memtable_map_tests_taken_t captures;
	TxnMemtableMapTaken *result_0;
	TxnMemtableMapEntry *result_1;
	uint32	   *result_2;
	HostError  *error;
	bool		ok;
	TxnMemtableOp op;
	bool		op_live;
} txn_memtable_map_tests_taken_frame_0_t;

/* txn_memtable_map_tests_taken_pad_0 is taken's pad: its put, while live. */
static void
txn_memtable_map_tests_taken_pad_0(void *frame_)
{
	txn_memtable_map_tests_taken_frame_0_t *frame = frame_;

	if (frame->op_live)
	{
		frame->op_live = false;
		txn_memtable_Op_drop(&frame->op);
	}
}

/* txn_memtable_map_tests_taken_body_0 is taken's body. */
static void
txn_memtable_map_tests_taken_body_0(void *frame_)
{
	txn_memtable_map_tests_taken_frame_0_t *frame = frame_;
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7),
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	txn_TupleRefSliceU8Usize_t rows[1];

	frame->op = txn_memtable_testing_put((const uint8 *) "v", 1);
	frame->op_live = true;
	change.op = &frame->op;
	frame->captures.change = &change;
	rows[0].f0 = frame->row;
	rows[0].f1 = txn_memtable_version_encoded(&change.writer, change.op, change.shown, false, 0);
	frame->ok = txn_memtable_table_prepare_TakenSeen(frame->io, frame->state, rows, 1, &frame->take, 1, false,
													 txn_memtable_map_tests_taken_inside, &frame->captures, NULL,
													 frame->result_0, frame->result_1, frame->result_2,
													 frame->error);
	txn_memtable_map_tests_taken_pad_0(frame);
}

/*
 * txn_memtable_map_tests_taken makes one take of a map's key, with base as
 * the key's base value, in the prepare of a row's push, and answers what the
 * take did and how many times then ran; a step that puts writes value.
 */
static bool
txn_memtable_map_tests_taken(HostIoIo *io, HostSharedRecord state, TxnMemtableMapMap map, uint32 key,
							 const uint8 *row, size_t row_len, txn_memtable_map_tests_Answer answer,
							 const uint8 *value, size_t value_len, TxnMemtableMapTaken *result_0,
							 TxnMemtableMapEntry *result_1, uint32 *result_2, HostError *error)
{
	txn_memtable_map_tests_taken_frame_0_t frame = {.io = io, .state = state, .result_0 = result_0,
	.result_1 = result_1, .result_2 = result_2, .error = error};

	frame.take.state = state;
	frame.take.map = map;
	frame.take.key = key;
	frame.take.most = 8;
	frame.row.ptr = row;
	frame.row.len = row_len;
	frame.captures.answer = answer;
	frame.captures.value.ptr = value;
	frame.captures.value.len = value_len;
	std_landing(&frame, txn_memtable_map_tests_taken_body_0, txn_memtable_map_tests_taken_pad_0);
	return frame.ok;
}

/* txn_memtable_map_tests_seen_Debug_fmt is a Seen's Debug, of an Option<Vec<u8>>. */
bool
txn_memtable_map_tests_seen_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableMapEntry *seen = value;
	StdBytes	bytes = {seen->value.ptr, seen->value.len};
	StdOptionRef option = {seen->has_value, STD_ITEM(&bytes, std_bytes_Debug_fmt)};

	return std_option_Debug_fmt(&option, f);
}

/* txn_memtable_map_tests_took_t is a take's (Taken<Seen>, u32): what it did, the Seen a took keeps, and its then's runs. */
typedef struct txn_memtable_map_tests_took_t
{
	TxnMemtableMapTaken taken;
	TxnMemtableMapEntry kept;
	uint32		ran;
} txn_memtable_map_tests_took_t;

/* txn_memtable_map_tests_taken_Debug_fmt is a Taken<Seen>'s Debug, of a take's (Taken<Seen>, u32). */
static bool
txn_memtable_map_tests_taken_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_map_tests_took_t *took = value;
	StdDebugTuple tuple;

	switch (took->taken)
	{
		case TXN_MEMTABLE_MAP_TAKEN_TOOK:
			tuple = std_fmt_debug_tuple(f, "Took", 4);
			std_DebugTuple_field(&tuple, &took->kept, txn_memtable_map_tests_seen_Debug_fmt);
			return std_DebugTuple_finish(&tuple);
		case TXN_MEMTABLE_MAP_TAKEN_RETRY:
			return std_fmt_write_str(f, "Retry", 5);
		default:
			return std_fmt_write_str(f, "Refused", 7);
	}
}

/* txn_memtable_map_tests_took_Debug_fmt is a take's (Taken<Seen>, u32)'s Debug. */
static bool
txn_memtable_map_tests_took_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_map_tests_took_t *took = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(took, txn_memtable_map_tests_taken_Debug_fmt),
								  STD_ITEM(&took->ran, std_u32_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_memtable_map_tests_seen_eq reports whether two Seens are equal. */
bool
txn_memtable_map_tests_seen_eq(const TxnMemtableMapEntry *a, const TxnMemtableMapEntry *b)
{
	return a->has_value == b->has_value &&
		(!a->has_value || (a->value.len == b->value.len &&
						   (a->value.len == 0 || memcmp(a->value.ptr, b->value.ptr, a->value.len) == 0)));
}

/* txn_memtable_map_tests_took_eq reports whether two takes' (Taken<Seen>, u32) are equal. */
static bool
txn_memtable_map_tests_took_eq(const txn_memtable_map_tests_took_t *a, const txn_memtable_map_tests_took_t *b)
{
	return a->taken == b->taken && a->ran == b->ran &&
		(a->taken != TXN_MEMTABLE_MAP_TAKEN_TOOK || txn_memtable_map_tests_seen_eq(&a->kept, &b->kept));
}

/* txn_memtable_map_tests_newest_is asserts a key's newest entry, as assert_eq on newest's unwrap does at their places. */
void
txn_memtable_map_tests_newest_is(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, HostIoIo *io,
								 HostSharedRecord state, TxnMemtableMapMap map, uint32 key, bool has_expected,
								 const TxnMemtableMapEntry *expected)
{
	TxnMemtableMapEntry entry;
	HostError	error;
	bool		some;
	StdOptionRef left;
	StdOptionRef right = {has_expected, STD_ITEM(expected, txn_memtable_map_tests_seen_Debug_fmt)};

	if (!txn_memtable_map_newest(io, state, map, key, &some, &entry, &error))
		STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = some;
	left.some = STD_ITEM(&entry, txn_memtable_map_tests_seen_Debug_fmt);
	STD_ASSERT_EQ(line, col, some == has_expected && (!some || txn_memtable_map_tests_seen_eq(&entry, expected)),
				  &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (some)
		txn_memtable_map_tests_seen_drop(&entry);
}

/* txn_memtable_map_tests_sought_t is a seek's Option<(u32, Entry)>. */
typedef struct txn_memtable_map_tests_sought_t
{
	bool		some;
	txn_TupleU32Entry_t found;
} txn_memtable_map_tests_sought_t;

/* txn_memtable_map_tests_sought_Debug_fmt is a seek's Option<(u32, Entry)>'s Debug. */
static bool
txn_memtable_map_tests_sought_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_map_tests_sought_t *sought = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&sought->found.f0, std_u32_Debug_fmt),
								  STD_ITEM(&sought->found.f1, txn_memtable_map_tests_seen_Debug_fmt));
	StdOptionRef option = {sought->some, STD_ITEM(&tuple, std_tuple_Debug_fmt)};

	return std_option_Debug_fmt(&option, f);
}

/* txn_memtable_map_tests_sought_list_t is a list of seeks to format: its seeks and their count. */
typedef struct txn_memtable_map_tests_sought_list_t
{
	const txn_memtable_map_tests_sought_t *sought;
	size_t		len;
} txn_memtable_map_tests_sought_list_t;

/* txn_memtable_map_tests_sought_list_Debug_fmt is a Vec of seeks' Debug. */
static bool
txn_memtable_map_tests_sought_list_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_map_tests_sought_list_t *list = value;
	StdDebugList builder = std_fmt_debug_list(f);

	for (size_t k = 0; k < list->len; k++)
		std_DebugList_entry(&builder, &list->sought[k], txn_memtable_map_tests_sought_Debug_fmt);
	return std_DebugList_finish(&builder);
}

/*
 * txn_memtable_map_tests_sought_are asserts seeks from starts, as assert_eq
 * on the Vec of seek's unwraps does at their places.
 */
static void
txn_memtable_map_tests_sought_are(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, HostIoIo *io,
								  HostSharedRecord state, TxnMemtableMapMap map, const uint32 *froms,
								  const txn_memtable_map_tests_sought_t *expected, size_t len)
{
	txn_memtable_map_tests_sought_t sought[4];
	txn_memtable_map_tests_sought_list_t left = {sought, len};
	txn_memtable_map_tests_sought_list_t right = {expected, len};
	HostError	error;
	bool		eq = true;

	for (size_t k = 0; k < len; k++)
		if (!txn_memtable_map_seek(io, state, map, froms[k], &sought[k].some, &sought[k].found, &error))
			STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	for (size_t k = 0; k < len && eq; k++)
		eq = sought[k].some == expected[k].some &&
			(!sought[k].some || (sought[k].found.f0 == expected[k].found.f0 &&
								 txn_memtable_map_tests_seen_eq(&sought[k].found.f1, &expected[k].found.f1)));
	STD_ASSERT_EQ(line, col, eq, &left, txn_memtable_map_tests_sought_list_Debug_fmt, &right,
				  txn_memtable_map_tests_sought_list_Debug_fmt);
	for (size_t k = 0; k < len; k++)
		if (sought[k].some)
			txn_memtable_map_tests_seen_drop(&sought[k].found.f1);
}

/* txn_memtable_map_tests_entry returns an Entry of a literal value, or none for NULL, borrowing it. */
static TxnMemtableMapEntry
txn_memtable_map_tests_entry(const char *value)
{
	TxnMemtableMapEntry entry = {.has_value = value != NULL};

	if (value != NULL)
	{
		entry.value.ptr = (void *) value;
		entry.value.len = strlen(value);
		entry.value.cap = entry.value.len;
	}
	return entry;
}

/* txn_memtable_map_tests_case_t is a Case: its key, its step's answer, the value a put writes, and what it does. */
typedef struct txn_memtable_map_tests_case_t
{
	uint32		key;
	txn_memtable_map_tests_Answer answer;
	const char *value;
	TxnMemtableMapTaken taken;
	const char *kept;
	uint32		ran;
} txn_memtable_map_tests_case_t;

/*
 * txn_memtable_map_tests_takes runs the maps' case on one map: each take's
 * step sees the key's current value, the base for a key with no entry, its
 * newest put after a put, and none after a removal; a put and a removal run
 * then once, and a retry and a refusal push nothing and never run it; newest
 * answers its three cases, and seek gives every key at or past its start in
 * order, removals included.
 */
static void
txn_memtable_map_tests_takes(TxnMemtableMapMap map, uint32 database)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	static const txn_memtable_map_tests_case_t steps[7] = {
		{5, txn_memtable_map_tests_put, "one", TXN_MEMTABLE_MAP_TAKEN_TOOK, "base", 1},
		{5, txn_memtable_map_tests_retry, "x", TXN_MEMTABLE_MAP_TAKEN_RETRY, NULL, 0},
		{5, txn_memtable_map_tests_refuse, "x", TXN_MEMTABLE_MAP_TAKEN_REFUSED, NULL, 0},
		{5, txn_memtable_map_tests_remove, "", TXN_MEMTABLE_MAP_TAKEN_TOOK, "one", 1},
		{5, txn_memtable_map_tests_put, "two", TXN_MEMTABLE_MAP_TAKEN_TOOK, NULL, 1},
		{9, txn_memtable_map_tests_put, "nine", TXN_MEMTABLE_MAP_TAKEN_TOOK, "base", 1},
		{3, txn_memtable_map_tests_remove, "", TXN_MEMTABLE_MAP_TAKEN_TOOK, "base", 1},
	};
	static const uint32 froms[4] = {0, 4, 6, 10};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, database);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableMapEntry two = txn_memtable_map_tests_entry("two");
	TxnMemtableMapEntry none = txn_memtable_map_tests_entry(NULL);
	txn_memtable_map_tests_sought_t expected[4] = {{true, {3}}, {true, {5}}, {true, {9}}, {false}};

	for (size_t at = 0; at < lengthof(steps); at++)
	{
		char		row[16];
		txn_memtable_map_tests_took_t took = {0};
		txn_memtable_map_tests_took_t wanted;
		HostError	error;

		wanted.taken = steps[at].taken;
		wanted.kept = txn_memtable_map_tests_entry(steps[at].kept);
		wanted.ran = steps[at].ran;
		snprintf(row, sizeof(row), "row%zu", at);
		if (!txn_memtable_map_tests_taken(io, state, map, steps[at].key, (const uint8 *) row, strlen(row),
										  steps[at].answer, (const uint8 *) steps[at].value, strlen(steps[at].value),
										  &took.taken, &took.kept, &took.ran, &error))
			STD_RESULT_EXPECT_FAILED(616, 82, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_memtable_map_tests_took_eq(&took, &wanted))
		{
			StdFmtArg	args[1] = {{&at, std_usize_Display_fmt}};
			StdFmtArguments message = {pieces, lengthof(pieces), args};

			std_assert_failed(RUST_FILE, 616, 13, "==", &took, txn_memtable_map_tests_took_Debug_fmt, &wanted,
							  txn_memtable_map_tests_took_Debug_fmt, &message);
		}
		if (took.taken == TXN_MEMTABLE_MAP_TAKEN_TOOK)
			txn_memtable_map_tests_seen_drop(&took.kept);
	}
	txn_memtable_map_tests_newest_is(618, 9, 618, 46, io, state, map, 5, true, &two);
	txn_memtable_map_tests_newest_is(619, 9, 619, 46, io, state, map, 3, true, &none);
	txn_memtable_map_tests_newest_is(620, 9, 620, 46, io, state, map, 4, false, NULL);
	expected[0].found.f1 = none;
	expected[1].found.f1 = two;
	expected[2].found.f1 = txn_memtable_map_tests_entry("nine");
	txn_memtable_map_tests_sought_are(622, 9, 621, 94, io, state, map, froms, expected, 4);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* The free space map's case: its takes, newest, and seek. */
void
test_txn_memtable_map_tests_the_free_space_maps_takes_newest_and_seek(void)
{
	txn_memtable_map_tests_takes(TXN_MEMTABLE_MAP_MAP_FREE, 9340);
}

/* The value map's case: the same forms as the free space map's. */
void
test_txn_memtable_map_tests_the_value_maps_takes_newest_and_seek(void)
{
	txn_memtable_map_tests_takes(TXN_MEMTABLE_MAP_MAP_VALUES, 9341);
}

/*
 * txn_memtable_map_tests_seek_past runs the seek's walk on one map: a seek
 * that finds no key at or past its start in one range goes on from the next
 * range's first key, and past the last range finds nothing.
 */
static void
txn_memtable_map_tests_seek_past(uint32 database, TxnMemtableMapMap map, const TxnMemtableTableTreeOps *tree,
								 uint32 ranged_line, uint32 ranged_col, uint32 taken_line, uint32 taken_col,
								 uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col)
{
	static const uint32 froms[2] = {4, 10};
	static const uint8 six[4] = {0, 0, 0, 6};
	static const uint32 keys[2] = {3, 9};
	static const char *const rows[2] = {"r3", "r9"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, database);
	HostSharedRecord state = host_shared_Held_record(&held);
	StdBytes	firsts[1] = {{six, sizeof(six)}};
	StdVec		ranges;
	HostError	error;
	txn_memtable_map_tests_sought_t expected[2] = {{true, {9}}, {false}};

	if (!txn_memtable_testing_ranged(tree, io, state, firsts, 1, &ranges, &error))
		STD_RESULT_EXPECT_FAILED(ranged_line, ranged_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0; k < lengthof(keys); k++)
	{
		txn_memtable_map_tests_took_t took;

		if (!txn_memtable_map_tests_taken(io, state, map, keys[k], (const uint8 *) rows[k], 2,
										  txn_memtable_map_tests_put, (const uint8 *) "v", 1, &took.taken, &took.kept,
										  &took.ran, &error))
			STD_RESULT_EXPECT_FAILED(taken_line, taken_col, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (took.taken == TXN_MEMTABLE_MAP_TAKEN_TOOK)
			txn_memtable_map_tests_seen_drop(&took.kept);
	}
	expected[0].found.f1 = txn_memtable_map_tests_entry("v");
	txn_memtable_map_tests_sought_are(line, col, unwrap_line, unwrap_col, io, state, map, froms, expected, 2);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * The seek's walk: a seek that finds no key at or past its start in one
 * range goes on from the next range's first key, and past the last range
 * finds nothing.
 */
void
test_txn_memtable_map_tests_a_maps_seek_goes_on_past_a_ranges_end(void)
{
	txn_memtable_map_tests_seek_past(9348, TXN_MEMTABLE_MAP_MAP_FREE, &TXN_MEMTABLE_MAP_FREE_TREE_OPS, 649, 59,
									 651, 68, 654, 9, 653, 94);
}

/*
 * The value map's seek past a range's end, as the free space map's: a seek
 * from a key past a range's last entry goes on forward to the next range's
 * first entry, and finds none past the last.
 */
void
test_txn_memtable_map_tests_a_value_maps_seek_goes_on_past_a_ranges_end(void)
{
	txn_memtable_map_tests_seek_past(9368, TXN_MEMTABLE_MAP_MAP_VALUES, &TXN_MEMTABLE_MAP_VALUES_TREE_OPS,
									 826, 61, 828, 70, 831, 9, 830, 96);
}

/* txn_memtable_map_tests_record_Debug_fmt is a record's Debug, of a Vec<u8>: its bytes as a list. */
static bool
txn_memtable_map_tests_record_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *record = value;
	StdBytes	bytes = {record->ptr, record->len};

	return std_bytes_Debug_fmt(&bytes, f);
}

/* txn_memtable_map_tests_records_Debug_fmt is records' Debug, of a Vec<Vec<u8>>. */
static bool
txn_memtable_map_tests_records_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *records = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < records->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(records, StdVec, k), txn_memtable_map_tests_record_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_memtable_map_tests_records_free frees a Vec<Vec<u8>>. */
static void
txn_memtable_map_tests_records_free(StdVec *records)
{
	for (size_t k = 0; k < records->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(records, StdVec, k), 1, 1);
	std_vec_free_Checked(records, sizeof(StdVec), _Alignof(StdVec));
}

/* txn_memtable_map_tests_cut_Debug_fmt is a cut's (u64, Vec<Vec<u8>>)'s Debug. */
static bool
txn_memtable_map_tests_cut_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_TupleU64VecVecU8_t *cut = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&cut->f0, std_u64_Debug_fmt),
								  STD_ITEM(&cut->f1, txn_memtable_map_tests_records_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_memtable_map_tests_valued is the cut case's valued: a record's put's value, or none for another op. */
static void
txn_memtable_map_tests_valued(const StdVec *record, StdVec *result)
{
	TxnMemtableVersionRecord stored;
	TxnMemtableVersionOpRef op;
	HostError	error;

	if (!txn_memtable_version_stored(record->ptr, record->len, &stored, &error))
		STD_RESULT_EXPECT_FAILED(703, 75, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	op = txn_memtable_version_Record_op(&stored);
	if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_PUT)
		std_slice_to_vec_Checked(result, op.u.put.value.ptr, op.u.put.value.len, 1, 1);
	else
		std_vec_new(result, 1, 1);
}

/* txn_memtable_map_tests_values is the cut case's values: each record's put's value. */
static void
txn_memtable_map_tests_values(const StdVec *records, StdVec *result)
{
	std_vec_with_capacity_Checked(result, records->len, sizeof(StdVec), _Alignof(StdVec));
	for (size_t k = 0; k < records->len; k++)
		txn_memtable_map_tests_valued(&STD_VEC_AT(records, StdVec, k), &STD_VEC_AT(result, StdVec, k));
	result->len = records->len;
}

/* txn_memtable_map_tests_cut_none asserts that a storage has no cut, as assert_eq on cut's unwrap does at their places. */
static void
txn_memtable_map_tests_cut_none(HostIoIo *io, HostSharedRecord state)
{
	txn_TupleU64VecVecU8_t cut;
	HostError	error;
	bool		some;
	StdOptionRef left;
	StdOptionRef right = {false};

	if (!txn_memtable_map_cut(io, state, &some, &cut, &error))
		STD_RESULT_EXPECT_FAILED(697, 35, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = some;
	left.some = STD_ITEM(&cut, txn_memtable_map_tests_cut_Debug_fmt);
	STD_ASSERT_EQ(697, 9, !some, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
}

/*
 * The cut's case: a storage with no free space map has no cut; with one of
 * two ranges, the cut reads the counter and the versions of its block's
 * entry from the last range, newest first, and no version for a counter
 * whose block has no entry.
 */
void
test_txn_memtable_map_tests_a_cut_reads_the_counter_with_its_blocks_entry_from_the_last_range(void)
{
	static const uint8 six[4] = {0, 0, 0, 6};
	static const uint32 keys[3] = {3, 9, 9};
	static const char *const rows[3] = {"r3", "r9", "s9"};
	static const char *const puts[3] = {"three", "nine1", "nine2"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9349);
	HostSharedRecord state = host_shared_Held_record(&held);
	StdBytes	firsts[1] = {{six, sizeof(six)}};
	StdVec		ranges;
	HostError	error;
	txn_TupleU64VecVecU8_t cut;
	txn_TupleU64VecVecU8_t left;
	txn_TupleU64VecVecU8_t right;
	StdVec		wanted;
	StdOptionRef left_cut;
	StdOptionRef right_cut;
	bool		some;
	bool		eq;

	txn_memtable_map_tests_cut_none(io, state);
	if (!txn_memtable_testing_ranged(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, state, firsts, 1, &ranges, &error))
		STD_RESULT_EXPECT_FAILED(698, 59, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0; k < lengthof(keys); k++)
	{
		txn_memtable_map_tests_took_t took;

		if (!txn_memtable_map_tests_taken(io, state, TXN_MEMTABLE_MAP_MAP_FREE, keys[k], (const uint8 *) rows[k], 2,
										  txn_memtable_map_tests_put, (const uint8 *) puts[k], strlen(puts[k]),
										  &took.taken, &took.kept, &took.ran, &error))
			STD_RESULT_EXPECT_FAILED(700, 69, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (took.taken == TXN_MEMTABLE_MAP_TAKEN_TOOK)
			txn_memtable_map_tests_seen_drop(&took.kept);
	}
	txn_state_moved(state, 1, 9 << 16 | 4);
	if (!txn_memtable_map_cut(io, state, &some, &cut, &error))
		STD_RESULT_EXPECT_FAILED(710, 49, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!some)
		STD_OPTION_UNWRAP_FAILED(710, 58);
	left.f0 = cut.f0;
	txn_memtable_map_tests_values(&cut.f1, &left.f1);
	right.f0 = 9 << 16 | 4;
	std_vec_with_capacity_Checked(&wanted, 2, sizeof(StdVec), _Alignof(StdVec));
	std_slice_to_vec_Checked(&STD_VEC_AT(&wanted, StdVec, 0), (const uint8 *) "nine2", 5, 1, 1);
	std_slice_to_vec_Checked(&STD_VEC_AT(&wanted, StdVec, 1), (const uint8 *) "nine1", 5, 1, 1);
	wanted.len = 2;
	right.f1 = wanted;
	eq = left.f0 == right.f0 && left.f1.len == 2;
	for (size_t k = 0; k < left.f1.len && eq; k++)
	{
		const StdVec *a = &STD_VEC_AT(&left.f1, StdVec, k);
		const StdVec *b = &STD_VEC_AT(&right.f1, StdVec, k);

		eq = a->len == b->len && memcmp(a->ptr, b->ptr, a->len) == 0;
	}
	STD_ASSERT_EQ(711, 9, eq, &left, txn_memtable_map_tests_cut_Debug_fmt, &right,
				  txn_memtable_map_tests_cut_Debug_fmt);
	txn_memtable_map_tests_records_free(&right.f1);
	txn_memtable_map_tests_records_free(&left.f1);
	txn_memtable_map_tests_records_free(&cut.f1);
	txn_state_moved(state, 9 << 16 | 4, 10 << 16 | 1);
	if (!txn_memtable_map_cut(io, state, &some, &cut, &error))
		STD_RESULT_EXPECT_FAILED(713, 35, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	right.f0 = 10 << 16 | 1;
	std_vec_new(&right.f1, sizeof(StdVec), _Alignof(StdVec));
	left_cut.has = some;
	left_cut.some = STD_ITEM(&cut, txn_memtable_map_tests_cut_Debug_fmt);
	right_cut.has = true;
	right_cut.some = STD_ITEM(&right, txn_memtable_map_tests_cut_Debug_fmt);
	STD_ASSERT_EQ(713, 9, some && cut.f0 == right.f0 && cut.f1.len == 0, &left_cut, std_option_Debug_fmt,
				  &right_cut, std_option_Debug_fmt);
	if (some)
		txn_memtable_map_tests_records_free(&cut.f1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

#define TXN_MEMTABLE_MAP_RS TakenRefused
#define TXN_MEMTABLE_MAP_RETURNS bool
#define TXN_MEMTABLE_MAP_RESULT_PARAMS , TxnMemtableMapTaken *result_0, bool *result_1, StdVec *result_2, \
	HostError *error
#define TXN_MEMTABLE_MAP_DECLARE
#define TXN_MEMTABLE_MAP_DEFINE
#define TXN_MEMTABLE_MAP_RESULT_FIELDS TxnMemtableMapTaken *result_0; bool *result_1; StdVec *result_2;
#define TXN_MEMTABLE_MAP_RESULT_INIT , result_0, result_1, result_2
#define TXN_MEMTABLE_MAP_RESULT_ARGS(c) , (c)->result_0, (c)->result_1, (c)->result_2
#define TXN_MEMTABLE_MAP_RESULT_ERROR error
#include "doltgres/txn/memtable/map_template.h"

/* txn_memtable_map_tests_left is the map-only case's step: it writes "left" and puts it. */
static TxnMemtableMapStep
txn_memtable_map_tests_left(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	std_vec_extend_from_slice_Checked(out, (const uint8 *) "left", 4, 1, 1);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_memtable_map_tests_nothing is a take's then that does nothing. */
static void
txn_memtable_map_tests_nothing(void *captures)
{
}

/*
 * txn_memtable_map_tests_map_only_inside is the map-only case's prepare's
 * closure: the take's finish, then a row's finish, whose refusal it answers
 * as its SQLSTATE.
 */
static bool
txn_memtable_map_tests_map_only_inside(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
									   TxnMemtableMapTaken *result_0, bool *result_1, StdVec *result_2,
									   HostError *error)
{
	const TxnMemtableVersionChange *change = captures;
	TxnMemtableVersionHandle handle;
	TxnStatusRoomed roomed = {0};
	HostError	refused;

	if (!txn_memtable_map_finish_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, txn_memtable_testing_writer(7), false, 0,
									  false, NULL, 0, txn_memtable_map_tests_left, NULL, NULL,
									  txn_memtable_map_tests_nothing, NULL, NULL, result_0, error))
		return false;
	*result_1 = txn_memtable_table_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, change, false, roomed, &handle,
										  &refused);
	if (!*result_1)
	{
		std_slice_to_vec_Checked(result_2, host_Error_sqlstate(&refused), 5, 1, 1);
		host_Error_drop(&refused);
	}
	return true;
}

/* txn_memtable_map_tests_map_only_t is the map-only case's (Taken<()>, Result<(), Vec<u8>>). */
typedef struct txn_memtable_map_tests_map_only_t
{
	TxnMemtableMapTaken taken;
	bool		ok;
	StdVec		err;
} txn_memtable_map_tests_map_only_t;

/* txn_memtable_map_tests_taken_unit_Debug_fmt is a Taken<()>'s Debug. */
static bool
txn_memtable_map_tests_taken_unit_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableMapTaken *taken = value;
	StdDebugTuple tuple;
	char		unit = 0;

	switch (*taken)
	{
		case TXN_MEMTABLE_MAP_TAKEN_TOOK:
			tuple = std_fmt_debug_tuple(f, "Took", 4);
			std_DebugTuple_field(&tuple, &unit, std_unit_Debug_fmt);
			return std_DebugTuple_finish(&tuple);
		case TXN_MEMTABLE_MAP_TAKEN_RETRY:
			return std_fmt_write_str(f, "Retry", 5);
		default:
			return std_fmt_write_str(f, "Refused", 7);
	}
}

/* txn_memtable_map_tests_refused_Debug_fmt is a Result<(), Vec<u8>>'s Debug, of the map-only case's pair. */
static bool
txn_memtable_map_tests_refused_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_map_tests_map_only_t *pair = value;
	StdBytes	err = {pair->err.ptr, pair->err.len};
	StdDebugTuple tuple;
	char		unit = 0;

	if (pair->ok)
	{
		tuple = std_fmt_debug_tuple(f, "Ok", 2);
		std_DebugTuple_field(&tuple, &unit, std_unit_Debug_fmt);
	}
	else
	{
		tuple = std_fmt_debug_tuple(f, "Err", 3);
		std_DebugTuple_field(&tuple, &err, std_bytes_Debug_fmt);
	}
	return std_DebugTuple_finish(&tuple);
}

/* txn_memtable_map_tests_map_only_Debug_fmt is the map-only case's (Taken<()>, Result<(), Vec<u8>>)'s Debug. */
static bool
txn_memtable_map_tests_map_only_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_map_tests_map_only_t *pair = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&pair->taken, txn_memtable_map_tests_taken_unit_Debug_fmt),
								  STD_ITEM(pair, txn_memtable_map_tests_refused_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/*
 * The map-only prepare's case, a redo of a fresh page's leave: a prepare of
 * a take alone finishes the take by its index among the takes, refuses a
 * row's finish, and makes no memtable of the storage's rows.
 */
void
test_txn_memtable_map_tests_a_map_only_prepare_takes_its_entry_and_makes_no_rows_memtable(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9353);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableMapTake take = {.state = state, .map = TXN_MEMTABLE_MAP_MAP_FREE, .key = 7, .most = 8};
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	txn_memtable_map_tests_map_only_t pair = {0};
	txn_memtable_map_tests_map_only_t wanted = {.taken = TXN_MEMTABLE_MAP_TAKEN_TOOK};
	TxnMemtableMapEntry left = txn_memtable_map_tests_entry("left");
	HostError	error;
	uint64		location;
	bool		has_memtable;
	StdOptionRef memtable;
	StdOptionRef none = {false};

	if (!txn_memtable_map_prepare_TakenRefused(io, &take, 1, txn_memtable_map_tests_map_only_inside, &change, NULL,
											   &pair.taken, &pair.ok, &pair.err, &error))
		STD_RESULT_EXPECT_FAILED(738, 10, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	wanted.err.ptr = (void *) "XX000";
	wanted.err.len = 5;
	wanted.err.cap = 5;
	STD_ASSERT_EQ(739, 9, pair.taken == TXN_MEMTABLE_MAP_TAKEN_TOOK && !pair.ok &&
				  pair.err.len == 5 && memcmp(pair.err.ptr, "XX000", 5) == 0, &pair,
				  txn_memtable_map_tests_map_only_Debug_fmt, &wanted, txn_memtable_map_tests_map_only_Debug_fmt);
	if (!pair.ok)
		std_vec_free_Checked(&pair.err, 1, 1);
	txn_memtable_map_tests_newest_is(740, 9, 740, 52, io, state, TXN_MEMTABLE_MAP_MAP_FREE, 7, true,
									 &left);
	has_memtable = txn_memtable_table_memtable(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, state, &location);
	memtable.has = has_memtable;
	memtable.some = STD_ITEM(&location, std_u64_Debug_fmt);
	STD_ASSERT_EQ(741, 9, !has_memtable, &memtable, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	txn_memtable_Op_drop(&op);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

#define TXN_MEMTABLE_TABLE_RS Unit
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#include "doltgres/txn/memtable/table_template.h"

/* txn_memtable_map_tests_written is the window case's step: it writes its value and puts it. */
static TxnMemtableMapStep
txn_memtable_map_tests_written(void *captures, bool has_current, const uint8 *current, size_t current_len,
							   StdVec *out)
{
	const char *value = captures;

	std_vec_extend_from_slice_Checked(out, (const uint8 *) value, strlen(value), 1, 1);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_memtable_map_tests_window_inside is the window case's prepare's closure: its three takes' finishes, then the row's. */
static bool
txn_memtable_map_tests_window_inside(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
									 HostError *error)
{
	static const char *const values[3] = {"free", "v100", "v101"};
	const TxnMemtableVersionChange *change = captures;
	TxnMemtableVersionHandle handle;
	TxnStatusRoomed roomed = {0};
	TxnMemtableMapTaken taken;

	for (size_t take = 0; take < lengthof(values); take++)
		if (!txn_memtable_map_finish_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, take, txn_memtable_testing_writer(7),
										  false, 0, false, NULL, 0, txn_memtable_map_tests_written,
										  (void *) values[take], NULL, txn_memtable_map_tests_nothing, NULL, NULL,
										  &taken, error))
			return false;
	return txn_memtable_table_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, change, false, roomed, &handle, error);
}

/* txn_memtable_map_tests_newest_t is a newest's Option<Entry>. */
typedef struct txn_memtable_map_tests_newest_t
{
	bool		some;
	TxnMemtableMapEntry entry;
} txn_memtable_map_tests_newest_t;

/* txn_memtable_map_tests_newest_Debug_fmt is a newest's Option<Entry>'s Debug. */
static bool
txn_memtable_map_tests_newest_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_map_tests_newest_t *newest = value;
	StdOptionRef option = {newest->some, STD_ITEM(&newest->entry, txn_memtable_map_tests_seen_Debug_fmt)};

	return std_option_Debug_fmt(&option, f);
}

/* txn_memtable_map_tests_newest_into reads a key's newest entry, as newest's unwrap does at its place. */
static void
txn_memtable_map_tests_newest_into(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state,
								   TxnMemtableMapMap map, uint32 key, txn_memtable_map_tests_newest_t *result)
{
	HostError	error;

	if (!txn_memtable_map_newest(io, state, map, key, &result->some, &result->entry, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/* txn_memtable_map_tests_newests_t is a list of newests to format: its newests and their count. */
typedef struct txn_memtable_map_tests_newests_t
{
	const txn_memtable_map_tests_newest_t *newest;
	size_t		len;
} txn_memtable_map_tests_newests_t;

/* txn_memtable_map_tests_newests_Debug_fmt is an array of newests' Debug. */
static bool
txn_memtable_map_tests_newests_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_map_tests_newests_t *list = value;
	StdDebugList builder = std_fmt_debug_list(f);

	for (size_t k = 0; k < list->len; k++)
		std_DebugList_entry(&builder, &list->newest[k], txn_memtable_map_tests_newest_Debug_fmt);
	return std_DebugList_finish(&builder);
}

/*
 * Ruling (C)'s window: one prepare readies a row, its free space take in the
 * row's storage, and two value map takes in a second storage, its TOAST
 * relation's, and one window finishes all four, each take landing in its own
 * storage's map and in no other's.
 */
void
test_txn_memtable_map_tests_one_window_takes_in_the_rows_storage_and_its_toast_relations(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld row = txn_memtable_testing_created(io, 9355);
	HostSharedHeld toast = txn_memtable_testing_created(io, 9356);
	HostSharedRecord rows = host_shared_Held_record(&row);
	HostSharedRecord values = host_shared_Held_record(&toast);
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableMapTake takes[3];
	txn_TupleRefSliceU8Usize_t keyed[1];
	HostError	error;
	txn_memtable_map_tests_newest_t entries[5];
	txn_memtable_map_tests_newest_t expected[5] = {0};
	txn_memtable_map_tests_newests_t left = {entries, 5};
	txn_memtable_map_tests_newests_t right = {expected, 5};
	const HostSharedRecord states[5] = {rows, values, values, rows, values};
	const TxnMemtableMapMap maps[5] = {TXN_MEMTABLE_MAP_MAP_FREE, TXN_MEMTABLE_MAP_MAP_VALUES,
	TXN_MEMTABLE_MAP_MAP_VALUES, TXN_MEMTABLE_MAP_MAP_VALUES, TXN_MEMTABLE_MAP_MAP_FREE};
	static const uint32 keys[5] = {7, 100, 101, 100, 7};
	bool		eq = true;

	takes[0].state = rows;
	takes[0].map = TXN_MEMTABLE_MAP_MAP_FREE;
	takes[0].key = 7;
	takes[0].most = 8;
	takes[1].state = values;
	takes[1].map = TXN_MEMTABLE_MAP_MAP_VALUES;
	takes[1].key = 100;
	takes[1].most = 8;
	takes[2] = takes[1];
	takes[2].key = 101;
	keyed[0].f0.ptr = (const uint8 *) "k";
	keyed[0].f0.len = 1;
	keyed[0].f1 = txn_memtable_version_encoded(&change.writer, change.op, change.shown, false, 0);
	if (!txn_memtable_table_prepare_Unit(io, rows, keyed, 1, takes, 3, false, txn_memtable_map_tests_window_inside,
										 &change, NULL, &error))
		STD_RESULT_EXPECT_FAILED(772, 10, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_map_tests_newest_into(774, 44, io, states[0], maps[0], keys[0], &entries[0]);
	txn_memtable_map_tests_newest_into(775, 50, io, states[1], maps[1], keys[1], &entries[1]);
	txn_memtable_map_tests_newest_into(776, 50, io, states[2], maps[2], keys[2], &entries[2]);
	txn_memtable_map_tests_newest_into(777, 48, io, states[3], maps[3], keys[3], &entries[3]);
	txn_memtable_map_tests_newest_into(778, 46, io, states[4], maps[4], keys[4], &entries[4]);
	expected[0].some = true;
	expected[0].entry = txn_memtable_map_tests_entry("free");
	expected[1].some = true;
	expected[1].entry = txn_memtable_map_tests_entry("v100");
	expected[2].some = true;
	expected[2].entry = txn_memtable_map_tests_entry("v101");
	for (size_t k = 0; k < 5 && eq; k++)
		eq = entries[k].some == expected[k].some &&
			(!entries[k].some || txn_memtable_map_tests_seen_eq(&entries[k].entry, &expected[k].entry));
	STD_ASSERT_EQ(781, 9, eq, &left, txn_memtable_map_tests_newests_Debug_fmt, &right,
				  txn_memtable_map_tests_newests_Debug_fmt);
	for (size_t k = 0; k < 5; k++)
		if (entries[k].some)
			txn_memtable_map_tests_seen_drop(&entries[k].entry);
	txn_memtable_Op_drop(&op);
	host_shared_Held_drop(&toast);
	host_shared_Held_drop(&row);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_map_tests_ending_Debug_fmt is a run's (String, Ending)'s Debug: its actor's name and its ending. */
static bool
txn_memtable_map_tests_ending_Debug_fmt(const void *value, StdFormatter *f)
{
	const simc_TupleStringEnding_t *ending = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&ending->f0, std_String_Debug_fmt),
								  STD_ITEM(&ending->f1, simc_Ending_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_memtable_map_tests_endings_Debug_fmt is a run's endings' Debug, of its Vec<(String, Ending)>. */
static bool
txn_memtable_map_tests_endings_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *endings = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < endings->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(endings, simc_TupleStringEnding_t, k),
							txn_memtable_map_tests_ending_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_memtable_map_tests_seeker is the replaced memtable case's seeker: its seek from the start finds the writer's entry. */
static bool
txn_memtable_map_tests_seeker(void *captures, HostIoIo *io, HostError *error)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("the seek found "), STD_FMT_ARG(0)};
	HostSharedHeld held;
	txn_memtable_map_tests_sought_t found;
	StdFmtArg	args[1] = {{&found, txn_memtable_map_tests_sought_Debug_fmt}};
	StdFmtArguments message = {pieces, lengthof(pieces), args};
	StdString text;
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9351, &held, error))
		return false;
	ok = txn_memtable_map_seek(io, host_shared_Held_record(&held), TXN_MEMTABLE_MAP_MAP_FREE, 0, &found.some,
							   &found.found, error);
	host_shared_Held_drop(&held);
	if (!ok)
		return false;
	if (found.some && found.found.f0 == 5 && found.found.f1.has_value && found.found.f1.value.len == 1 &&
		memcmp(found.found.f1.value.ptr, "v", 1) == 0)
	{
		txn_memtable_map_tests_seen_drop(&found.found.f1);
		return true;
	}
	std_fmt_format_Checked(&text, &message);
	if (found.some)
		txn_memtable_map_tests_seen_drop(&found.found.f1);
	*error = host_Error_fork((const uint8 *) "XX000", &text);
	return false;
}

/* txn_memtable_map_tests_writer is the replaced memtable case's writer: its take puts the entry the seeker reads. */
static bool
txn_memtable_map_tests_writer(void *captures, HostIoIo *io, HostError *error)
{
	HostSharedHeld held;
	txn_memtable_map_tests_took_t took;
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9351, &held, error))
		return false;
	ok = txn_memtable_map_tests_taken(io, host_shared_Held_record(&held), TXN_MEMTABLE_MAP_MAP_FREE, 5,
									  (const uint8 *) "row", 3, txn_memtable_map_tests_put, (const uint8 *) "v", 1,
									  &took.taken, &took.kept, &took.ran, error);
	host_shared_Held_drop(&held);
	if (ok && took.taken == TXN_MEMTABLE_MAP_TAKEN_TOOK)
		txn_memtable_map_tests_seen_drop(&took.kept);
	return ok;
}

/*
 * The walk's step past a memtable replaced meanwhile: a seek whose read met
 * its map's memtable with no entry, which a first writer's take then puts one
 * in or replaces with a new memtable before the seek goes on, reads that
 * entry and never the memtable it left.
 */
void
test_txn_memtable_map_tests_a_seek_reads_on_after_a_writer_follows_its_read_of_an_empty_map(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcRun		run;
	SimcStep	steps[2] = {{"writer", 6, PG_UINT64_MAX}, {"seeker", 6, PG_UINT64_MAX}};
	SimcActor	seeker = {txn_memtable_map_tests_seeker};
	SimcActor	writer = {txn_memtable_map_tests_writer};
	SimcReport	report;
	HostSharedHeld held;
	HostError	error;
	const StdVec *endings;
	bool		returned = true;

	held = txn_memtable_testing_created(io, 9351);
	if (!txn_memtable_table_first(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, host_shared_Held_record(&held), &error))
		STD_RESULT_EXPECT_FAILED(666, 53, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	host_shared_Held_drop(&held);
	run = simc_Run_new(1);
	simc_Run_actor(&run, "seeker", 6, seeker);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_until(&run, "seeker", 6, "doltgres-memtable-step");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	if (report.outcome.tag != SIMC_OUTCOME_ENDED)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[1] = {{&report.outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 683, 67, &message);
	}
	endings = &report.outcome.u.ended;
	for (size_t k = 0; k < endings->len && returned; k++)
	{
		const SimcEnding *ending = &STD_VEC_AT(endings, simc_TupleStringEnding_t, k).f1;

		returned = ending->tag == SIMC_ENDING_RETURNED && !ending->is_err;
	}
	if (!returned)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[1] = {{endings, txn_memtable_map_tests_endings_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 684, 9, &message);
	}
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_map_tests_chunked is the failure case's chunk's push: its finish with no transaction and a point. */
static bool
txn_memtable_map_tests_chunked(void *captures, HostIoIo *io, TxnMemtableTablePreparedBranchDeltas *prepared,
							   HostError *error)
{
	StdVec	   *chunk = captures;
	TxnMemtableVersionHandle handle;

	return txn_memtable_branch_finish(io, prepared, TXN_MEMTABLE_VERSION_NO_TRANSACTION, chunk, true,
									  UINT64CONST(0x100000a28), &handle, error);
}

/* txn_memtable_map_tests_canceled is the failure case's row's prepare's closure: a cancel before its window. */
static bool
txn_memtable_map_tests_canceled(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
								HostError *error)
{
	static const char message[] = "canceling statement due to user request";
	StdString text;

	std_slice_to_vec_Checked(&text, (const uint8 *) message, sizeof(message) - 1, 1, 1);
	*error = host_Error_fork((const uint8 *) "57014", &text);
	return false;
}

/* txn_memtable_map_tests_holds_k is the failure case's read: whether the row's range holds "k". */
static bool
txn_memtable_map_tests_holds_k(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
							   const HostSharedGuard *guard, HostError *error)
{
	bool	   *held = captures;
	TxnMemtableIndexNode node;

	return txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, (const uint8 *) "k", 1, held, &node,
								   error);
}

/*
 * Ruling (C)'s failure case: an insert that pushes its value's chunk in its
 * own window and fails before its row's window leaves the chunk in the
 * working state's delta, reading back before a fold, and unreferenced, since
 * no value map entry names it, so a collection would drop it; and no row.
 */
void
test_txn_memtable_map_tests_a_failure_after_the_chunks_and_before_the_window_leaves_the_chunk_unreferenced(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld row = txn_memtable_testing_created(io, 9357);
	HostSharedHeld toast = txn_memtable_testing_created(io, 9358);
	HostSharedRecord rows = host_shared_Held_record(&row);
	HostSharedRecord values = host_shared_Held_record(&toast);
	HostSharedHeld deltas;
	uint8		address[TXN_MEMTABLE_BRANCH_ADDRESS];
	StdVec		chunk;
	TxnMemtableMapTake take = {.state = values, .map = TXN_MEMTABLE_MAP_MAP_VALUES, .key = 100, .most = 64};
	txn_TupleRefSliceU8Usize_t keyed[1];
	HostError	error;
	HostError	failed;
	char		unit = 0;
	StdBytes	sqlstate;
	StdBytes	canceled = {(const uint8 *) "57014", 5};
	StdVec		got;
	bool		some;
	StdBytes	got_bytes;
	StdBytes	chunk_bytes = {(const uint8 *) "chunk", 5};
	StdOptionRef left;
	StdOptionRef right;
	txn_memtable_map_tests_sought_t none = {false};
	txn_memtable_map_tests_sought_t sought;
	bool		held = true;
	bool		read;
	bool		no = false;

	if (!txn_memtable_branch_deltas(io, 9357, HOST_PG_DEFAULT, &deltas, &error))
		STD_RESULT_EXPECT_FAILED(794, 103, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	memset(address, 5, sizeof(address));
	std_slice_to_vec_Checked(&chunk, (const uint8 *) "chunk", 5, 1, 1);
	if (!txn_memtable_branch_prepare(io, host_shared_Held_record(&deltas), address, sizeof(address),
									 txn_memtable_version_largest(chunk.len), txn_memtable_map_tests_chunked, &chunk,
									 NULL, &error))
		STD_RESULT_EXPECT_FAILED(801, 10, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	keyed[0].f0.ptr = (const uint8 *) "k";
	keyed[0].f0.len = 1;
	keyed[0].f1 = 64;
	if (txn_memtable_table_prepare_Unit(io, rows, keyed, 1, &take, 1, false, txn_memtable_map_tests_canceled, NULL,
										NULL, &failed))
		STD_RESULT_EXPECT_FAILED(806, 10, "called `Result::unwrap_err()` on an `Ok` value", &unit,
								 std_unit_Debug_fmt);
	sqlstate.ptr = host_Error_sqlstate(&failed);
	sqlstate.len = 5;
	STD_ASSERT_EQ(807, 9, memcmp(sqlstate.ptr, "57014", 5) == 0, &sqlstate, std_bytes_Debug_fmt,
				  &canceled, std_bytes_Debug_fmt);
	host_Error_drop(&failed);
	if (!txn_memtable_branch_get(io, host_shared_Held_record(&deltas), address, sizeof(address), &some, &got,
								 &error))
		STD_RESULT_EXPECT_FAILED(808, 63, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	got_bytes.ptr = got.ptr;
	got_bytes.len = got.len;
	left.has = some;
	left.some = STD_ITEM(&got_bytes, std_bytes_Debug_fmt);
	right.has = true;
	right.some = STD_ITEM(&chunk_bytes, std_bytes_Debug_fmt);
	STD_ASSERT_EQ(808, 9, some && got.len == 5 && memcmp(got.ptr, "chunk", 5) == 0, &left,
				  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (some)
		std_vec_free_Checked(&got, 1, 1);
	if (!txn_memtable_map_seek(io, values, TXN_MEMTABLE_MAP_MAP_VALUES, 0, &sought.some, &sought.found, &error))
		STD_RESULT_EXPECT_FAILED(809, 53, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(809, 9, !sought.some, &sought, txn_memtable_map_tests_sought_Debug_fmt, &none,
				  txn_memtable_map_tests_sought_Debug_fmt);
	if (sought.some)
		txn_memtable_map_tests_seen_drop(&sought.found.f1);
	txn_memtable_map_tests_newest_is(810, 9, 810, 51, io, rows, TXN_MEMTABLE_MAP_MAP_FREE, 0, false,
									 NULL);
	if (!txn_memtable_table_read(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, rows, (const uint8 *) "k", 1,
								 txn_memtable_map_tests_holds_k, &held, &read, &error))
		STD_RESULT_EXPECT_FAILED(814, 73, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = read;
	left.some = STD_ITEM(&held, std_bool_Debug_fmt);
	right.has = true;
	right.some = STD_ITEM(&no, std_bool_Debug_fmt);
	STD_ASSERT_EQ(814, 9, read && !held, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	std_vec_free_Checked(&chunk, 1, 1);
	host_shared_Held_drop(&deltas);
	host_shared_Held_drop(&toast);
	host_shared_Held_drop(&row);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_TXN */
