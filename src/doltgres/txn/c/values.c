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

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/repo/lib.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"

/* The map's takes that keep nothing, whose prepare's closure returns nothing. */
#define TXN_MEMTABLE_MAP_RS Unit
#define TXN_MEMTABLE_MAP_RETURNS bool
#define TXN_MEMTABLE_MAP_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_MAP_TS Unit
#define TXN_MEMTABLE_MAP_KEPT_OUT
#define TXN_MEMTABLE_MAP_KEPT_IN
#define TXN_MEMTABLE_MAP_DECLARE
#include "doltgres/txn/memtable/map_template.h"
#endif

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

#ifdef DOLTGRES_TEST_TXN

/* TXN_VALUES_TESTS_UNWRAP_FAILED is a Result's unwrap's panic on a HostError at a line and column. */
#define TXN_VALUES_TESTS_UNWRAP_FAILED(line, col, error) \
	STD_RESULT_EXPECT_FAILED((line), (col), "called `Result::unwrap()` on an `Err` value", (error), \
							 host_Error_Debug_fmt)

/* txn_values_tests_refused reports whether a value fails to decode as an entry with data corruption, XX001. */
static bool
txn_values_tests_refused(const uint8 *value, size_t value_len)
{
	TxnValuesEntry entry;
	HostError	error;
	bool		refused;

	if (txn_values_Entry_decode(value, value_len, &entry, &error))
		return false;
	refused = memcmp(host_Error_sqlstate(&error), "XX001", 5) == 0;
	host_Error_drop(&error);
	return refused;
}

/*
 * test_txn_values_tests_a_value_map_entry_reads_back_and_refuses_every_other_form:
 * an entry reads back as it was written, in MOST's room without growing its
 * buffer, and one cut short, one with a byte past its count, and one whose
 * count is zero are data corruption.
 */
void
test_txn_values_tests_a_value_map_entry_reads_back_and_refuses_every_other_form(void)
{
	TxnValuesEntry entry = {.address = chunks_address_Address_of((const uint8 *) "blob", 4),
	.locator = chunks_trailer_Locator_at(3, 640, 64), .raw = 4000, .stored = 4000, .count = 1};
	TxnValuesEntry zero = entry;
	TxnValuesEntry decoded;
	StdVec		out;
	StdVec		longer;
	StdVec		none;
	const void *at;
	uint8		key[4];
	uint8		past = 0;
	HostError	error;

	std_vec_with_capacity_Checked(&out, TXN_VALUES_MOST, 1, 1);
	at = out.ptr;
	txn_values_Entry_encode_into(&entry, &out);
	STD_ASSERT(__LINE__, 9, out.ptr == at, "an entry fits MOST's room");
	if (!txn_values_Entry_decode(out.ptr, out.len, &decoded, &error))
		TXN_VALUES_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	STD_ASSERT_EQ(__LINE__, 9, txn_values_Entry_PartialEq_eq(&decoded, &entry), &decoded, txn_values_Entry_Debug_fmt,
				  &entry, txn_values_Entry_Debug_fmt);
	txn_values_key(258, key);
	STD_ASSERT(__LINE__, 9, key[0] == 0 && key[1] == 0 && key[2] == 1 && key[3] == 2, "a key is its id big-endian");
	STD_ASSERT(__LINE__, 9, txn_values_tests_refused(out.ptr, out.len - 1), "a cut entry decoded");
	std_slice_to_vec_Checked(&longer, out.ptr, out.len, 1, 1);
	std_vec_push_Checked(&longer, &past, 1, 1);
	STD_ASSERT(__LINE__, 9, txn_values_tests_refused(longer.ptr, longer.len),
			   "an entry with a byte past its count decoded");
	STD_ASSERT(__LINE__, 9, txn_values_tests_refused(out.ptr, 20), "an entry with half an address decoded");
	zero.count = 0;
	std_vec_new(&none, 1, 1);
	txn_values_Entry_encode_into(&zero, &none);
	STD_ASSERT(__LINE__, 9, txn_values_tests_refused(none.ptr, none.len), "a count of zero decoded");
	std_vec_free_Checked(&none, 1, 1);
	std_vec_free_Checked(&longer, 1, 1);
	std_vec_free_Checked(&out, 1, 1);
}

/* txn_values_tests_made_t is a TOAST relation's state in a new repository, with the reader its store reads through. */
typedef struct txn_values_tests_made_t
{
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld toast;
} txn_values_tests_made_t;

/* txn_values_tests_made returns a new database's TOAST relation's state of a relation in pg_default. */
static txn_values_tests_made_t
txn_values_tests_made(HostIoIo *io, uint32 database, uint32 relation)
{
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = database, .relation = relation};
	txn_values_tests_made_t made;
	ChunksErrorError failed;
	HostError	error;

	if (!repo_Repo_create(io, database, HOST_PG_DEFAULT, &made.repo, &failed))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	if (!host_shared_read(io, &made.reading, &error) ||
		!txn_state_state(io, &made.repo, &made.reading, &storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL,
						 &made.toast, &error))
		TXN_VALUES_TESTS_UNWRAP_FAILED(__LINE__, 0, &error);
	return made;
}

/* txn_values_tests_made_drop frees what made returned. */
static void
txn_values_tests_made_drop(txn_values_tests_made_t *made)
{
	host_shared_Held_drop(&made->toast);
	host_shared_Reader_drop(&made->reading);
	repo_Repo_drop(&made->repo);
}

/* txn_values_tests_put is a take's step that puts the entry its captures hold. */
static TxnMemtableMapStep
txn_values_tests_put(void *captures, bool has_current, const uint8 *current, size_t current_len, StdVec *out)
{
	txn_values_Entry_encode_into(captures, out);
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_values_tests_nothing is a take's then that does nothing. */
static void
txn_values_tests_nothing(void *captures)
{
}

/* txn_values_tests_written_t is written's closure's captures: the writer and the entry it puts. */
typedef struct txn_values_tests_written_t
{
	TxnMemtableVersionWriter writer;
	TxnValuesEntry *entry;
} txn_values_tests_written_t;

/* txn_values_tests_written_inside is written's prepare's closure: the one take's finish. */
static bool
txn_values_tests_written_inside(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
								HostError *error)
{
	txn_values_tests_written_t *c = captures;
	TxnStatusRoomed unroomed = {0};
	TxnMemtableMapTaken taken;

	return txn_memtable_map_finish_Unit(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, c->writer, false, unroomed, false, 0,
										false, NULL, 0, txn_values_tests_put, c->entry, NULL,
										txn_values_tests_nothing, NULL, NULL, &taken, error);
}

/* txn_values_tests_written puts a value id's entry into a TOAST relation's value map by a writer. */
static void
txn_values_tests_written(HostIoIo *io, const HostSharedHeld *toast, uint32 id, uint64 xid, TxnValuesEntry *entry)
{
	TxnMemtableMapTake take = {.state = host_shared_Held_record(toast), .map = TXN_MEMTABLE_MAP_MAP_VALUES,
	.key = id, .most = TXN_VALUES_MOST};
	txn_values_tests_written_t c = {.writer = {.xid = xid}, .entry = entry};
	HostError	error;

	if (!txn_memtable_map_prepare_Unit(io, &take, 1, txn_values_tests_written_inside, &c, NULL, &error))
		TXN_VALUES_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
}

/*
 * test_txn_values_tests_a_value_id_the_map_holds_is_skipped: an id the TOAST
 * relation's value map holds in its delta is skipped, as GetNewOidWithIndex
 * skips one the TOAST index holds, and the next id is taken.
 */
void
test_txn_values_tests_a_value_id_the_map_holds_is_skipped(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_values_tests_made_t made = txn_values_tests_made(io, 9310, 16390);
	TreesStoreReader store = repo_Repo_reader(&made.repo, HOST_CACHE_PLACEMENT_SERVER, &made.reading);
	TxnValuesEntry entry = {.address = chunks_address_Address_of((const uint8 *) "v", 1),
	.locator = chunks_trailer_Locator_Default_default(), .raw = 4000, .stored = 4000, .count = 1};
	bool		held;
	bool		next;
	HostError	error;

	txn_values_tests_written(io, &made.toast, 100, 41, &entry);
	if (!txn_values_free(io, TREES_STORE_READER_NODE_STORE(&store), &made.toast, 100, &held, &error) ||
		!txn_values_free(io, TREES_STORE_READER_NODE_STORE(&store), &made.toast, 101, &next, &error))
		TXN_VALUES_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	STD_ASSERT(__LINE__, 9, !held, "an id the map holds is not free");
	STD_ASSERT(__LINE__, 9, next, "the next id is free");
	txn_values_tests_made_drop(&made);
	simc_TestIo_drop(&test_io);
}

/*
 * test_txn_values_tests_a_value_ids_entry_reads_back: a value id's entry the
 * delta holds reads back as it was written, and an id the map doesn't hold
 * has none.
 */
void
test_txn_values_tests_a_value_ids_entry_reads_back(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	txn_values_tests_made_t made = txn_values_tests_made(io, 9311, 16391);
	TreesStoreReader store = repo_Repo_reader(&made.repo, HOST_CACHE_PLACEMENT_SERVER, &made.reading);
	TxnValuesEntry written = txn_values_Entry_new(chunks_address_Address_of((const uint8 *) "value", 5), 5000);
	TxnValuesEntry read;
	TxnValuesEntry absent;
	bool		has_read;
	bool		has_absent;
	HostError	error;

	txn_values_tests_written(io, &made.toast, 200, 42, &written);
	if (!txn_values_entry(io, TREES_STORE_READER_NODE_STORE(&store), &made.toast, 200, &has_read, &read, &error) ||
		!txn_values_entry(io, TREES_STORE_READER_NODE_STORE(&store), &made.toast, 201, &has_absent, &absent,
						  &error))
		TXN_VALUES_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	STD_ASSERT(__LINE__, 9, has_read && txn_values_Entry_PartialEq_eq(&read, &written),
			   "an id's entry reads back as written");
	STD_ASSERT(__LINE__, 9, !has_absent, "an id the map doesn't hold has no entry");
	txn_values_tests_made_drop(&made);
	simc_TestIo_drop(&test_io);
}

/* txn_values_tests_sized returns a value's sizes by its stored bytes: its entries, its rows, and its heap bytes. */
static TxnBaseSizes
txn_values_tests_sized(uint64 stored)
{
	TxnValuesEntry entry = {.address = chunks_address_Address_of((const uint8 *) "blob", 4),
	.locator = chunks_trailer_Locator_Default_default(), .raw = stored + 4, .stored = stored, .count = 1};
	StdVec		value;
	uint8		key[4];
	TxnBaseSizes sizes;
	HostError	error;

	std_vec_new(&value, 1, 1);
	txn_values_Entry_encode_into(&entry, &value);
	txn_values_key(1, key);
	if (!txn_values_sizes(key, sizeof(key), value.ptr, value.len, &sizes, &error))
		TXN_VALUES_TESTS_UNWRAP_FAILED(__LINE__, 9, &error);
	std_vec_free_Checked(&value, 1, 1);
	return sizes;
}

/* txn_values_tests_sized_is asserts a value's entries, rows, and heap bytes. */
static void
txn_values_tests_sized_is(uint32 line, uint64 stored, uint64 entries, uint64 rows, uint64 heap_bytes)
{
	TxnBaseSizes sizes = txn_values_tests_sized(stored);

	STD_ASSERT(line, 5, sizes.entries == entries && sizes.rows == rows && sizes.heap_bytes == heap_bytes,
			   "a value counts heap's toast chunks");
}

/*
 * test_txn_values_tests_a_value_counts_heaps_toast_chunks: a value counts the
 * toast chunks heap stores it in, TOAST_MAX_CHUNK_SIZE bytes each but its
 * last, and their heap tuples' bytes with their line pointers, a short last
 * chunk taking a one-byte varlena header.
 */
void
test_txn_values_tests_a_value_counts_heaps_toast_chunks(void)
{
	STD_ASSERT_EQ_U64(__LINE__, 5, TXN_VALUES_TOAST_MAX_CHUNK_SIZE, 1996);
	txn_values_tests_sized_is(__LINE__, 6400, 1, 4, 3 * 2036 + 452);
	txn_values_tests_sized_is(__LINE__, 2560, 1, 2, 2036 + 604);
	txn_values_tests_sized_is(__LINE__, 2112, 1, 2, 2036 + 156);
	txn_values_tests_sized_is(__LINE__, 1996, 1, 1, 2036);
}

/*
 * test_txn_values_tests_a_take_down_takes_one_from_a_count_and_removes_the_last:
 * a take-down takes its amount from a value's count and removes the entry at
 * its last, an add then a take-down leaves the count as it was, and a key
 * with no entry, one that doesn't decode, or a count below the amount is
 * refused.
 */
void
test_txn_values_tests_a_take_down_takes_one_from_a_count_and_removes_the_last(void)
{
	TxnValuesEntry entry = {.address = chunks_address_Address_of((const uint8 *) "blob", 4),
	.locator = chunks_trailer_Locator_Default_default(), .raw = 4000, .stored = 4000, .count = 2};
	TxnValuesEntry decoded;
	StdVec		two;
	StdVec		one;
	StdVec		three;
	StdVec		back;
	StdVec		scratch;
	HostError	error;

	std_vec_new(&two, 1, 1);
	std_vec_new(&one, 1, 1);
	std_vec_new(&three, 1, 1);
	std_vec_new(&back, 1, 1);
	std_vec_new(&scratch, 1, 1);
	txn_values_Entry_encode_into(&entry, &two);
	STD_ASSERT(__LINE__, 5, txn_values_taken_down(true, two.ptr, two.len, &one, 1) == TXN_MEMTABLE_MAP_STEP_PUT,
			   "a take-down of one from two puts");
	if (!txn_values_Entry_decode(one.ptr, one.len, &decoded, &error))
		TXN_VALUES_TESTS_UNWRAP_FAILED(__LINE__, 16, &error);
	STD_ASSERT_EQ_U64(__LINE__, 5, decoded.count, 1);
	STD_ASSERT(__LINE__, 5,
			   txn_values_taken_down(true, one.ptr, one.len, &scratch, 1) == TXN_MEMTABLE_MAP_STEP_REMOVE,
			   "a take-down of the last removes the entry");
	STD_ASSERT(__LINE__, 5,
			   txn_values_taken_down(true, two.ptr, two.len, &scratch, 2) == TXN_MEMTABLE_MAP_STEP_REMOVE,
			   "a take-down of the whole count removes the entry");
	STD_ASSERT(__LINE__, 5,
			   txn_values_taken_down(true, two.ptr, two.len, &scratch, 3) == TXN_MEMTABLE_MAP_STEP_REFUSE,
			   "a take-down past the count is refused");
	STD_ASSERT(__LINE__, 5, txn_values_added(true, two.ptr, two.len, &three) == TXN_MEMTABLE_MAP_STEP_PUT,
			   "an add puts");
	STD_ASSERT(__LINE__, 5, txn_values_taken_down(true, three.ptr, three.len, &back, 1) == TXN_MEMTABLE_MAP_STEP_PUT,
			   "a take-down after an add puts");
	STD_ASSERT(__LINE__, 5, back.len == two.len && memcmp(back.ptr, two.ptr, two.len) == 0,
			   "an add then a take-down leaves the count as it was");
	STD_ASSERT(__LINE__, 5, txn_values_taken_down(false, NULL, 0, &scratch, 1) == TXN_MEMTABLE_MAP_STEP_REFUSE,
			   "a key with no entry is refused");
	STD_ASSERT(__LINE__, 5, txn_values_taken_down(true, two.ptr, 10, &scratch, 1) == TXN_MEMTABLE_MAP_STEP_REFUSE,
			   "an entry that doesn't decode is refused");
	std_vec_free_Checked(&scratch, 1, 1);
	std_vec_free_Checked(&back, 1, 1);
	std_vec_free_Checked(&three, 1, 1);
	std_vec_free_Checked(&one, 1, 1);
	std_vec_free_Checked(&two, 1, 1);
}

#endif							/* DOLTGRES_TEST_TXN */
