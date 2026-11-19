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
 * A TOAST relation's value map's entries: a value id's blob, its root address
 * and locator, its raw and stored sizes, and the count of row versions that
 * name it.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/varint.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/lib.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/values.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "values.rs"

/* txn_values_maxaligned rounds a length up to Postgres' MAXALIGN, eight bytes. */
static uint64
txn_values_maxaligned(uint64 len)
{
	return (len + 7) / 8 * 8;
}

/*
 * txn_values_chunk_bytes returns the bytes heap's page takes for a toast
 * chunk of a length of data: its tuple, MAXALIGNed, with a one-byte varlena
 * header for data short enough to take one, and its line pointer.
 */
static uint64
txn_values_chunk_bytes(uint64 len)
{
	uint64		header = len < 127 ? 1 : 4;

	return txn_values_maxaligned(24 + 4 + 4 + header + len) + 4;
}

/* txn_values_corrupt returns the error of a value map entry that doesn't decode. */
static HostError
txn_values_corrupt(void)
{
	static const char message[] = "a value map's entry is corrupt";
	StdString text;

	std_string_from_Checked(&text, message, sizeof(message) - 1);
	return host_Error_fork((const uint8 *) "XX001", &text);
}

/* txn_values_key writes a value id's key in a value map, its four bytes big-endian. */
void
txn_values_key(uint32 id, uint8 result[4])
{
	result[0] = (uint8) (id >> 24);
	result[1] = (uint8) (id >> 16);
	result[2] = (uint8) (id >> 8);
	result[3] = (uint8) id;
}

/*
 * txn_values_Entry_new returns the entry an insert writes, and its redo writes
 * from the row change's field 24 alone, for a value whose blob has a root
 * address and a length in bytes: the default locator of a blob no pack holds
 * yet, its raw size the length with the 4-byte varlena header, its stored
 * size the length, and a count of one.
 */
TxnValuesEntry
txn_values_Entry_new(ChunksAddressAddress address, uint64 length)
{
	TxnValuesEntry entry = {.address = address, .locator = chunks_trailer_Locator_Default_default(),
	.raw = STD_ADD(u64, length, 4, 97, 59), .stored = length, .count = 1};

	return entry;
}

/*
 * txn_values_Entry_encode_into writes the entry's value in place of what out
 * held, allocating nothing when out has MOST's room: the address's 32 bytes,
 * the locator's bytes, then the raw size, the stored size, and the count as
 * varints.
 */
void
txn_values_Entry_encode_into(const TxnValuesEntry *self, StdVec *out)
{
	out->len = 0;
	std_vec_extend_from_slice_Checked(out, self->address.f0, sizeof(self->address.f0), 1, 1);
	chunks_trailer_Locator_write(&self->locator, out);
	chunks_varint_put_varint(out, self->raw);
	chunks_varint_put_varint(out, self->stored);
	chunks_varint_put_varint(out, self->count);
}

/* txn_values_Entry_tree returns the name of the blob tree the entry names, which a read of its value takes. */
TreesStoreTree
txn_values_Entry_tree(const TxnValuesEntry *self)
{
	return trees_store_Tree_decoded(self->address, self->locator);
}

/*
 * txn_values_Entry_parsed reads an entry's value as decode does, answering
 * false for every form decode refuses, inside a map take's step.
 */
static bool
txn_values_Entry_parsed(const uint8 *value, size_t value_len, TxnValuesEntry *result)
{
	const uint8 *sizes;
	size_t		sizes_len;
	size_t		at = 0;
	ChunksVarintMalformed malformed;

	if (value_len < 32 || value_len - 32 < CHUNKS_TRAILER_LOCATOR_LEN)
		return false;
	memcpy(result->address.f0, value, 32);
	result->locator = chunks_trailer_Locator_read(value + 32, CHUNKS_TRAILER_LOCATOR_LEN);
	sizes = value + 32 + CHUNKS_TRAILER_LOCATOR_LEN;
	sizes_len = value_len - 32 - CHUNKS_TRAILER_LOCATOR_LEN;
	if (!chunks_varint_decode(sizes, sizes_len, &at, &result->raw, &malformed) ||
		!chunks_varint_decode(sizes, sizes_len, &at, &result->stored, &malformed) ||
		!chunks_varint_decode(sizes, sizes_len, &at, &result->count, &malformed))
		return false;
	return at == sizes_len && result->count != 0;
}

/*
 * txn_values_Entry_decode reads an entry's value, refusing any other form as
 * data corruption: one cut short or with bytes past its count, and a count of
 * zero.
 */
bool
txn_values_Entry_decode(const uint8 *value, size_t value_len, TxnValuesEntry *result, HostError *error)
{
	if (txn_values_Entry_parsed(value, value_len, result))
		return true;
	*error = txn_values_corrupt();
	return false;
}

/* txn_values_Entry_PartialEq_eq reports whether two entries are equal field by field. */
bool
txn_values_Entry_PartialEq_eq(const TxnValuesEntry *self, const TxnValuesEntry *other)
{
	return chunks_address_Address_PartialEq_eq(&self->address, &other->address) &&
		chunks_trailer_Locator_PartialEq_eq(&self->locator, &other->locator) && self->raw == other->raw &&
		self->stored == other->stored && self->count == other->count;
}

/* txn_values_Entry_Debug_fmt is Entry's derived Debug. */
bool
txn_values_Entry_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnValuesEntry *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Entry", 5);

	std_DebugStruct_field(&s, "address", 7, &self->address, chunks_address_Address_Debug_fmt);
	std_DebugStruct_field(&s, "locator", 7, &self->locator, chunks_trailer_Locator_Debug_fmt);
	std_DebugStruct_field(&s, "raw", 3, &self->raw, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "stored", 6, &self->stored, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "count", 5, &self->count, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/*
 * txn_values_sizes returns what a value map's entry counts in its TOAST
 * storage's totals: one value, the toast chunks heap would store its stored
 * bytes in as rows, and those chunks' heap bytes, beside the entry's own leaf
 * and key bytes, refusing an entry that doesn't decode.
 */
bool
txn_values_sizes(const uint8 *key, size_t key_len, const uint8 *value, size_t value_len, TxnBaseSizes *result,
				 HostError *error)
{
	TxnValuesEntry entry;
	uint64		full;
	uint64		left;
	uint64		heap_bytes;

	if (!txn_values_Entry_decode(value, value_len, &entry, error))
		return false;
	full = entry.stored / TXN_VALUES_TOAST_MAX_CHUNK_SIZE;
	left = entry.stored % TXN_VALUES_TOAST_MAX_CHUNK_SIZE;
	heap_bytes = STD_MUL(u64, full, txn_values_chunk_bytes(TXN_VALUES_TOAST_MAX_CHUNK_SIZE), 65, 22);
	if (left > 0)
		heap_bytes = STD_ADD(u64, heap_bytes, txn_values_chunk_bytes(left), 67, 9);
	result->entries = 1;
	result->leaf_bytes = trees_chunker_leaf_entry_size(key, key_len, value, value_len);
	result->key_bytes = key_len;
	result->heap_bytes = heap_bytes;
	result->rows = STD_ADD(u64, full, (uint64) (left > 0), 74, 15);
	return true;
}

/*
 * txn_values_added is a map take's step that adds one to a kept value's
 * entry's count, refusing a key with no entry or one that doesn't decode,
 * which no kept value has.
 */
TxnMemtableMapStep
txn_values_added(bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	TxnValuesEntry entry;

	if (!has_current || !txn_values_Entry_parsed(current, current_len, &entry))
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	entry.count = STD_ADD(u64, entry.count, 1, 150, 28);
	txn_values_Entry_encode_into(&entry, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/*
 * txn_values_taken_down is a map take's step that takes by from a value's
 * entry's count as that many row versions that named it go for good,
 * removing the entry when they were its last, and refusing a key with no
 * entry, one that doesn't decode, or a count below by.
 */
TxnMemtableMapStep
txn_values_taken_down(bool has_current, const uint8 *current, size_t current_len, StdVec *out, uint64 by)
{
	TxnValuesEntry entry;

	if (!has_current || !txn_values_Entry_parsed(current, current_len, &entry))
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	if (entry.count == by)
		return TXN_MEMTABLE_MAP_STEP_REMOVE;
	if (entry.count < by)
		return TXN_MEMTABLE_MAP_STEP_REFUSE;
	entry.count -= by;
	txn_values_Entry_encode_into(&entry, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_values_mark returns a TOAST state's mark, read before the fall-back: its base's generation and its spills'. */
std_TupleU64U64_t
txn_values_mark(HostSharedRecord record)
{
	std_TupleU64U64_t mark = {txn_state_generation(record), txn_spill_generation(&TXN_SPILL_STATE_HOME_OPS, record)};

	return mark;
}

/* txn_values_base_value returns a value id's entry's value in a TOAST storage's base, none for none. */
static bool
txn_values_base_value(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *toast, uint32 id,
					  bool *has_value, StdVec *value, HostError *error)
{
	bool		some;
	TxnBaseBase base;
	bool		has_root;
	TreesStoreTree root;
	uint8		key[4];
	ChunksErrorError failed;

	*has_value = false;
	if (!txn_state_base_record(io, toast, &some, &base, error))
		return false;
	if (!some)
		return true;
	has_root = base.has_root;
	root = base.root;
	txn_base_Base_drop(&base);
	if (!has_root)
		return true;
	txn_values_key(id, key);
	if (trees_get(io, store, TREES_STORE_TREE_NAMED(&root), key, 4, has_value, value, &failed))
		return true;
	*error = txn_base_storage(&failed);
	return false;
}

/*
 * txn_values_based returns a value id's entry's value below the memtable of a
 * TOAST relation's storage's value map, its spills' newest, else its base's,
 * read outside any window for a kept value's count add to fall back to, with
 * the mark read before it, which the add checks.
 */
bool
txn_values_based(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *toast, uint32 id, bool *has_value,
				 StdVec *value, std_TupleU64U64_t *mark, HostError *error)
{
	HostSharedRecord record = host_shared_Held_record(toast);
	bool		some;
	TxnMemtableMapEntry entry;

	*mark = txn_values_mark(record);
	if (!txn_spill_newest_value(io, record, id, &some, &entry, error))
		return false;
	if (some)
	{
		*has_value = entry.has_value;
		if (entry.has_value)
			*value = entry.value;
		return true;
	}
	return txn_values_base_value(io, store, toast, id, has_value, value, error);
}

/* txn_values_held returns a value id's entry's value in a TOAST relation's storage's value map as entry finds it. */
static bool
txn_values_held(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *toast, uint32 id, bool *has_value,
				StdVec *value, HostError *error)
{
	HostSharedRecord record = host_shared_Held_record(toast);
	bool		some;
	TxnMemtableMapEntry entry;

	if (!txn_memtable_map_newest(io, record, TXN_MEMTABLE_MAP_MAP_VALUES, id, &some, &entry, error))
		return false;
	if (!some && !txn_spill_newest_value(io, record, id, &some, &entry, error))
		return false;
	if (some)
	{
		*has_value = entry.has_value;
		if (entry.has_value)
			*value = entry.value;
		return true;
	}
	return txn_values_base_value(io, store, toast, id, has_value, value, error);
}

/*
 * txn_values_free reports whether a value id is free for a new out-of-line
 * value of a TOAST relation's storage: the storage's value map holds no entry
 * for it, its delta's newest else its base's. An id the delta holds a removal
 * of is free.
 */
bool
txn_values_free(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *toast, uint32 id, bool *result,
				HostError *error)
{
	bool		has_value;
	StdVec		value;

	if (!txn_values_held(io, store, toast, id, &has_value, &value, error))
		return false;
	if (has_value)
		std_vec_free_Checked(&value, 1, 1);
	*result = !has_value;
	return true;
}

/* txn_values_entry_frame_t is entry's state across its pad: the value it read, while live. */
typedef struct txn_values_entry_frame_t
{
	StdVec		value;
	TxnValuesEntry *result;
	HostError  *error;
	bool		decoded;
} txn_values_entry_frame_t;

/* txn_values_entry_body decodes the value read. */
static void
txn_values_entry_body(void *frame_)
{
	txn_values_entry_frame_t *frame = frame_;

	frame->decoded = txn_values_Entry_decode(frame->value.ptr, frame->value.len, frame->result, frame->error);
}

/* txn_values_entry_pad frees the value read. */
static void
txn_values_entry_pad(void *frame_)
{
	txn_values_entry_frame_t *frame = frame_;

	std_vec_free_Checked(&frame->value, 1, 1);
}

/*
 * txn_values_entry returns a value id's entry in a TOAST relation's storage's
 * value map, its delta's newest else its base's, false for an id the map
 * doesn't hold or whose newest entry the delta removed.
 */
bool
txn_values_entry(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *toast, uint32 id, bool *has_result,
				 TxnValuesEntry *result, HostError *error)
{
	txn_values_entry_frame_t frame = {.result = result, .error = error};

	if (!txn_values_held(io, store, toast, id, has_result, &frame.value, error))
		return false;
	if (!*has_result)
		return true;
	std_landing(&frame, txn_values_entry_body, txn_values_entry_pad);
	txn_values_entry_pad(&frame);
	return frame.decoded;
}
