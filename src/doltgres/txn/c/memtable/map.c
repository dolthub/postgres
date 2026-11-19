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
		std_vec_clear(&frame->reading.takers, sizeof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t),
					  txn_memtable_map_Taker_drop);
		std_vec_free_Checked(&frame->reading.takers, sizeof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t),
							 _Alignof(txn_TupleWriterRowEntryU64TupleVecU32Bool_t));
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
