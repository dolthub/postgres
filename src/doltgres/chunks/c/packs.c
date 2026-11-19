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
 * The C port of chunks' packs.rs: the records of the files that list a
 * repository's packs, the pack table's and each tablespace's runs file's.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/follow.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/index.h"
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/btree.h"
#include "doltgres/std/path.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs.rs"

/* CHUNKS_PACKS_NUMBER is the length of a pack's number in a pack table's record: its 48 bits. */
#define CHUNKS_PACKS_NUMBER ((size_t) 6)

/* CHUNKS_PACKS_LISTED is the length of a pack's entry in a pack table's whole record: its entry, then its deletion vector's generation, zero when it has none. */
#define CHUNKS_PACKS_LISTED (CHUNKS_PACKS_ENTRY + 8)

/* CHUNKS_PACKS_HEAD is the length of the head of a pack table's whole record: the generation, the two numbers, and the count of its packs. */
#define CHUNKS_PACKS_HEAD (8 + 2 * CHUNKS_PACKS_NUMBER + 4)

/* CHUNKS_PACKS_WHOLE is the kind of a record of a pack table's or runs file that holds all of it. */
#define CHUNKS_PACKS_WHOLE ((uint8) 0)

/* CHUNKS_PACKS_ADDED is the kind of a record of a pack table's file that adds a pack, or of a runs file that adds a run. */
#define CHUNKS_PACKS_ADDED ((uint8) 1)

/* CHUNKS_PACKS_REMOVED is the kind of a record of a pack table's file that retires packs, or of a runs file that removes runs. */
#define CHUNKS_PACKS_REMOVED ((uint8) 2)

/* CHUNKS_PACKS_REPLACED is the kind of a record of a runs file that puts the run a merge wrote in the place of the runs it merged. */
#define CHUNKS_PACKS_REPLACED ((uint8) 3)

/* CHUNKS_PACKS_MOVED is the kind of a record of a pack table's file that says where a tablespace's runs file ends after a change to that file alone. */
#define CHUNKS_PACKS_MOVED ((uint8) 4)

/* CHUNKS_PACKS_VECTOR is the kind of a record of a pack table's file that names the generation of a pack's deletion vector. */
#define CHUNKS_PACKS_VECTOR ((uint8) 5)

/* CHUNKS_PACKS_RESERVED is the kind of a record of a pack table's file that sets the bound of the numbers its packs' writers may be given. */
#define CHUNKS_PACKS_RESERVED ((uint8) 6)

/* CHUNKS_PACKS_TABLE_CORRUPT is the message of a record of the pack table no writer of its file makes. */
#define CHUNKS_PACKS_TABLE_CORRUPT "a record of the pack table is corrupt"

/* chunks_packs_Listed_Debug_fmt is Listed's derived Debug. */
bool
chunks_packs_Listed_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksListed *self = value;
	StdOptionRef dead = {self->has_dead, {&self->dead, std_NonZeroU64_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "Listed", strlen("Listed"));

	std_DebugStruct_field(&s, "space", strlen("space"), &self->space, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "id", strlen("id"), &self->id, chunks_pack_PackId_Debug_fmt);
	std_DebugStruct_field(&s, "dead", strlen("dead"), &dead, std_option_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_packs_Listed_PartialEq_eq is Listed's derived PartialEq. */
bool
chunks_packs_Listed_PartialEq_eq(const ChunksPacksListed *self, const ChunksPacksListed *other)
{
	return self->space == other->space && chunks_pack_PackId_PartialEq_eq(&self->id, &other->id) &&
		self->has_dead == other->has_dead && (!self->has_dead || self->dead == other->dead);
}

/* chunks_packs_Tail_Debug_fmt is Tail's derived Debug. */
bool
chunks_packs_Tail_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksTail *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Tail", strlen("Tail"));

	std_DebugStruct_field(&s, "end", strlen("end"), &self->end, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "generation", strlen("generation"), &self->generation, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_packs_Tail_Default_default is Tail's derived Default: both zero. */
ChunksPacksTail
chunks_packs_Tail_Default_default(void)
{
	ChunksPacksTail tail = {0};

	return tail;
}

/* chunks_packs_Tail_PartialEq_eq is Tail's derived PartialEq. */
bool
chunks_packs_Tail_PartialEq_eq(const ChunksPacksTail *self, const ChunksPacksTail *other)
{
	return self->end == other->end && self->generation == other->generation;
}

/* chunks_packs_Tail_bytes writes the tail as a pack table's record holds it, in CHUNKS_PACKS_TAIL bytes. */
void
chunks_packs_Tail_bytes(const ChunksPacksTail *self, uint8 *result)
{
	std_u64_to_le_bytes(self->end, result);
	std_u64_to_le_bytes(self->generation, STD_PTR_AT(result, 8, 1));
}

/* chunks_packs_Tail_read returns the tail a pack table's record holds in its CHUNKS_PACKS_TAIL bytes. */
ChunksPacksTail
chunks_packs_Tail_read(const uint8 *bytes)
{
	ChunksPacksTail tail;

	tail.end = chunks_packs_long(bytes, CHUNKS_PACKS_TAIL, 0);
	tail.generation = chunks_packs_long(bytes, CHUNKS_PACKS_TAIL, 8);
	return tail;
}

/* chunks_packs_word returns the little-endian u32 at a place in bytes that hold it. */
uint32
chunks_packs_word(const uint8 *bytes, size_t bytes_len, size_t at)
{
	size_t		end = STD_ADD(usize, at, 4, 164, 34);

	std_range_check(at, end, bytes_len, RUST_FILE, 164, 29);
	if (std_wrapping_sub_usize(end, at) != 4)
		STD_RESULT_EXPECT_FAILED(164, 53, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	return std_u32_from_le_bytes(STD_PTR_AT(bytes, at, 1));
}

/* chunks_packs_number returns the pack's number at a place in bytes that hold its CHUNKS_PACKS_NUMBER bytes, low byte first. */
static uint64
chunks_packs_number(const uint8 *bytes, size_t bytes_len, size_t at)
{
	uint8		number[8] = {0};
	size_t		end = STD_ADD(usize, at, CHUNKS_PACKS_NUMBER, 170, 49);

	std_range_check(at, end, bytes_len, RUST_FILE, 170, 44);
	memcpy(number, STD_PTR_AT(bytes, at, 1), CHUNKS_PACKS_NUMBER);
	return std_u64_from_le_bytes(number);
}

/* chunks_packs_number_bytes writes a pack's number as a pack table's record holds it, in CHUNKS_PACKS_NUMBER bytes. */
static void
chunks_packs_number_bytes(uint64 number, uint8 *result)
{
	uint8		bytes[8];

	std_u64_to_le_bytes(number, bytes);
	memcpy(result, bytes, CHUNKS_PACKS_NUMBER);
}

/* chunks_packs_long returns the little-endian u64 at a place in bytes that hold it. */
uint64
chunks_packs_long(const uint8 *bytes, size_t bytes_len, size_t at)
{
	size_t		end = STD_ADD(usize, at, 8, 183, 34);

	std_range_check(at, end, bytes_len, RUST_FILE, 183, 29);
	if (std_wrapping_sub_usize(end, at) != 8)
		STD_RESULT_EXPECT_FAILED(183, 53, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	return std_u64_from_le_bytes(STD_PTR_AT(bytes, at, 1));
}

/* chunks_packs_record returns a record of a pack table's or runs file of a kind. */
static void
chunks_packs_record(uint8 kind, const uint8 *body, size_t body_len, StdVec *result)
{
	std_vec_with_capacity_Checked(result, STD_ADD(usize, 1, body_len, 188, 41), 1, 1);
	std_vec_u8_push_Checked(result, kind);
	std_vec_extend_from_slice_Checked(result, body, body_len, 1, 1);
}

/* chunks_packs_entry_bytes writes a pack's entry as a pack table's record holds it, in CHUNKS_PACKS_ENTRY bytes. */
void
chunks_packs_entry_bytes(const ChunksPacksTableEntry *entry, uint8 *result)
{
	chunks_packs_number_bytes(entry->f0, result);
	std_u32_to_le_bytes(entry->f1, STD_PTR_AT(result, CHUNKS_PACKS_NUMBER, 1));
	memcpy(STD_PTR_AT(result, CHUNKS_PACKS_NUMBER + 4, 1), entry->f2.f0, sizeof(entry->f2.f0));
}

/* chunks_packs_entry_of returns the pack an entry of a pack table's record names, in its CHUNKS_PACKS_ENTRY bytes. */
ChunksPacksTableEntry
chunks_packs_entry_of(const uint8 *bytes)
{
	ChunksPacksTableEntry entry;

	entry.f0 = chunks_packs_number(bytes, CHUNKS_PACKS_ENTRY, 0);
	entry.f1 = chunks_packs_word(bytes, CHUNKS_PACKS_ENTRY, CHUNKS_PACKS_NUMBER);
	memcpy(entry.f2.f0, STD_PTR_AT(bytes, CHUNKS_PACKS_NUMBER + 4, 1), sizeof(entry.f2.f0));
	return entry;
}

/* chunks_packs_Table_Default_default is Table's derived Default: all zero, with no packs and no runs. */
void
chunks_packs_Table_Default_default(ChunksPacksTable *result)
{
	result->generation = 0;
	result->added = 0;
	result->reserved = 0;
	std_vec_new(&result->packs, sizeof(chunks_TupleU64Listed_t), _Alignof(chunks_TupleU64Listed_t));
	std_vec_new(&result->runs, sizeof(chunks_TupleU32Tail_t), _Alignof(chunks_TupleU32Tail_t));
}

/* chunks_packs_Table_drop is Table's drop glue: its packs and its runs. */
void
chunks_packs_Table_drop(ChunksPacksTable *self)
{
	std_vec_free_Checked(&self->packs, sizeof(chunks_TupleU64Listed_t), _Alignof(chunks_TupleU64Listed_t));
	std_vec_free_Checked(&self->runs, sizeof(chunks_TupleU32Tail_t), _Alignof(chunks_TupleU32Tail_t));
}

/* chunks_packs_table_packs_Debug_fmt is the Debug of a table's packs, a map's. */
static bool
chunks_packs_table_packs_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *self = value;
	StdDebugMap map = std_fmt_debug_map(f);

	for (size_t k = 0, k_len = self->len; k < k_len; k++)
	{
		const chunks_TupleU64Listed_t *entry = &STD_VEC_AT(self, chunks_TupleU64Listed_t, k);

		std_DebugMap_entry(&map, &entry->f0, std_u64_Debug_fmt, &entry->f1, chunks_packs_Listed_Debug_fmt);
	}
	return std_DebugMap_finish(&map);
}

/* chunks_packs_table_runs_Debug_fmt is the Debug of a table's runs tails, a map's. */
static bool
chunks_packs_table_runs_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *self = value;
	StdDebugMap map = std_fmt_debug_map(f);

	for (size_t k = 0, k_len = self->len; k < k_len; k++)
	{
		const chunks_TupleU32Tail_t *entry = &STD_VEC_AT(self, chunks_TupleU32Tail_t, k);

		std_DebugMap_entry(&map, &entry->f0, std_u32_Debug_fmt, &entry->f1, chunks_packs_Tail_Debug_fmt);
	}
	return std_DebugMap_finish(&map);
}

/* chunks_packs_Table_Debug_fmt is Table's derived Debug. */
bool
chunks_packs_Table_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksTable *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Table", strlen("Table"));

	std_DebugStruct_field(&s, "generation", strlen("generation"), &self->generation, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "added", strlen("added"), &self->added, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "reserved", strlen("reserved"), &self->reserved, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "packs", strlen("packs"), &self->packs, chunks_packs_table_packs_Debug_fmt);
	std_DebugStruct_field(&s, "runs", strlen("runs"), &self->runs, chunks_packs_table_runs_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_packs_Table_PartialEq_eq is Table's derived PartialEq: its numbers, and its packs and runs pair by pair. */
bool
chunks_packs_Table_PartialEq_eq(const ChunksPacksTable *self, const ChunksPacksTable *other)
{
	if (self->generation != other->generation || self->added != other->added || self->reserved != other->reserved ||
		self->packs.len != other->packs.len || self->runs.len != other->runs.len)
		return false;
	for (size_t k = 0, k_len = self->packs.len; k < k_len; k++)
	{
		const chunks_TupleU64Listed_t *a = &STD_VEC_AT(&self->packs, chunks_TupleU64Listed_t, k);
		const chunks_TupleU64Listed_t *b = &STD_VEC_AT(&other->packs, chunks_TupleU64Listed_t, k);

		if (a->f0 != b->f0 || !chunks_packs_Listed_PartialEq_eq(&a->f1, &b->f1))
			return false;
	}
	for (size_t k = 0, k_len = self->runs.len; k < k_len; k++)
	{
		const chunks_TupleU32Tail_t *a = &STD_VEC_AT(&self->runs, chunks_TupleU32Tail_t, k);
		const chunks_TupleU32Tail_t *b = &STD_VEC_AT(&other->runs, chunks_TupleU32Tail_t, k);

		if (a->f0 != b->f0 || !chunks_packs_Tail_PartialEq_eq(&a->f1, &b->f1))
			return false;
	}
	return true;
}

/* chunks_packs_table_packs_find reports whether a table's packs list a number, with its place, or else the place it goes in their order. */
static bool
chunks_packs_table_packs_find(const StdVec *packs, uint64 number, size_t *at)
{
	size_t		low = 0;
	size_t		high = packs->len;

	while (low < high)
	{
		size_t		middle = std_wrapping_add_usize(low, STD_SHR_FOLDED(usize, std_wrapping_sub_usize(high, low), 1));
		uint64		found = STD_VEC_AT(packs, chunks_TupleU64Listed_t, middle).f0;

		if (found == number)
		{
			*at = middle;
			return true;
		}
		if (found < number)
			low = std_wrapping_add_usize(middle, 1);
		else
			high = middle;
	}
	*at = low;
	return false;
}

/* chunks_packs_table_packs_get is BTreeMap::get of a table's packs: how they list a number, or NULL. */
ChunksPacksListed *
chunks_packs_table_packs_get(const StdVec *packs, uint64 number)
{
	size_t		at;

	if (!chunks_packs_table_packs_find(packs, number, &at))
		return NULL;
	return &STD_VEC_AT(packs, chunks_TupleU64Listed_t, at).f1;
}

/* chunks_packs_table_packs_insert is BTreeMap::insert of a table's packs: it lists a number in place of what they listed there. */
void
chunks_packs_table_packs_insert(StdVec *packs, uint64 number, const ChunksPacksListed *listed)
{
	size_t		at;
	chunks_TupleU64Listed_t entry = {number, *listed};

	if (chunks_packs_table_packs_find(packs, number, &at))
		STD_VEC_AT(packs, chunks_TupleU64Listed_t, at).f1 = *listed;
	else
		std_vec_insert(packs, at, &entry, sizeof(entry), _Alignof(chunks_TupleU64Listed_t), RUST_FILE,
					   297, 28);
}

/* chunks_packs_table_packs_remove is BTreeMap::remove of a table's packs, reporting whether they listed the number. */
bool
chunks_packs_table_packs_remove(StdVec *packs, uint64 number)
{
	size_t		at;
	chunks_TupleU64Listed_t removed;

	if (!chunks_packs_table_packs_find(packs, number, &at))
		return false;
	std_vec_remove(packs, at, &removed, sizeof(removed), _Alignof(chunks_TupleU64Listed_t), RUST_FILE,
				   308, 32);
	return true;
}

/* chunks_packs_table_runs_find reports whether a table's runs tails hold a tablespace's, with its place, or else the place it goes. */
static bool
chunks_packs_table_runs_find(const StdVec *runs, uint32 space, size_t *at)
{
	size_t		low = 0;
	size_t		high = runs->len;

	while (low < high)
	{
		size_t		middle = std_wrapping_add_usize(low, STD_SHR_FOLDED(usize, std_wrapping_sub_usize(high, low), 1));
		uint32		found = STD_VEC_AT(runs, chunks_TupleU32Tail_t, middle).f0;

		if (found == space)
		{
			*at = middle;
			return true;
		}
		if (found < space)
			low = std_wrapping_add_usize(middle, 1);
		else
			high = middle;
	}
	*at = low;
	return false;
}

/* chunks_packs_table_runs_get is BTreeMap::get of a table's runs tails: a tablespace's, or NULL. */
ChunksPacksTail *
chunks_packs_table_runs_get(const StdVec *runs, uint32 space)
{
	size_t		at;

	if (!chunks_packs_table_runs_find(runs, space, &at))
		return NULL;
	return &STD_VEC_AT(runs, chunks_TupleU32Tail_t, at).f1;
}

/* chunks_packs_table_runs_insert is BTreeMap::insert of a table's runs tails: it sets a tablespace's in place of what they held. */
void
chunks_packs_table_runs_insert(StdVec *runs, uint32 space, ChunksPacksTail tail)
{
	size_t		at;
	chunks_TupleU32Tail_t entry = {space, tail};

	if (chunks_packs_table_runs_find(runs, space, &at))
		STD_VEC_AT(runs, chunks_TupleU32Tail_t, at).f1 = tail;
	else
		std_vec_insert(runs, at, &entry, sizeof(entry), _Alignof(chunks_TupleU32Tail_t), RUST_FILE,
					   298, 27);
}

/*
 * chunks_packs_Table_whole returns the record that begins a pack table's
 * file: the generation, the highest number a pack was added at, the bound of
 * the writers' numbers, each pack's entry and its deletion vector's
 * generation in number order, and each tablespace's runs tail, so a whole
 * write keeps them.  It writes the record into the one allocation its length
 * needs, so nothing is left owned when that allocation fails.
 */
void
chunks_packs_Table_whole(const ChunksPacksTable *self, StdVec *result)
{
	uint8		bytes[CHUNKS_PACKS_LISTED];
	size_t		len = std_wrapping_add_usize(std_wrapping_add_usize(1 + CHUNKS_PACKS_HEAD,
																	std_wrapping_mul_usize(self->packs.len,
																						   CHUNKS_PACKS_LISTED)),
											 std_wrapping_mul_usize(self->runs.len, 4 + CHUNKS_PACKS_TAIL));

	std_vec_with_capacity_Checked(result, len, 1, 1);
	std_vec_u8_push_Checked(result, CHUNKS_PACKS_WHOLE);
	std_u64_to_le_bytes(self->generation, bytes);
	std_vec_extend_from_slice_Checked(result, bytes, 8, 1, 1);
	chunks_packs_number_bytes(self->added, bytes);
	std_vec_extend_from_slice_Checked(result, bytes, CHUNKS_PACKS_NUMBER, 1, 1);
	chunks_packs_number_bytes(self->reserved, bytes);
	std_vec_extend_from_slice_Checked(result, bytes, CHUNKS_PACKS_NUMBER, 1, 1);
	std_u32_to_le_bytes((uint32) self->packs.len, bytes);
	std_vec_extend_from_slice_Checked(result, bytes, 4, 1, 1);
	for (size_t k = 0, k_len = self->packs.len; k < k_len; k++)
	{
		const chunks_TupleU64Listed_t *listed = &STD_VEC_AT(&self->packs, chunks_TupleU64Listed_t, k);
		ChunksPacksTableEntry entry = {listed->f0, listed->f1.space, listed->f1.id};

		chunks_packs_entry_bytes(&entry, bytes);
		std_u64_to_le_bytes(listed->f1.has_dead ? listed->f1.dead : 0, STD_PTR_AT(bytes, CHUNKS_PACKS_ENTRY, 1));
		std_vec_extend_from_slice_Checked(result, bytes, CHUNKS_PACKS_LISTED, 1, 1);
	}
	for (size_t k = 0, k_len = self->runs.len; k < k_len; k++)
	{
		const chunks_TupleU32Tail_t *tail = &STD_VEC_AT(&self->runs, chunks_TupleU32Tail_t, k);

		std_u32_to_le_bytes(tail->f0, bytes);
		chunks_packs_Tail_bytes(&tail->f1, STD_PTR_AT(bytes, 4, 1));
		std_vec_extend_from_slice_Checked(result, bytes, 4 + CHUNKS_PACKS_TAIL, 1, 1);
	}
}

/* chunks_packs_Table_added returns the record that adds a pack, with where its tablespace's runs file ends after the add. */
void
chunks_packs_Table_added(const ChunksPacksTableEntry *entry, ChunksPacksTail runs, StdVec *result)
{
	uint8		body[CHUNKS_PACKS_ENTRY + CHUNKS_PACKS_TAIL];

	chunks_packs_entry_bytes(entry, body);
	chunks_packs_Tail_bytes(&runs, STD_PTR_AT(body, CHUNKS_PACKS_ENTRY, 1));
	chunks_packs_record(CHUNKS_PACKS_ADDED, body, sizeof(body), result);
}

/* chunks_packs_Table_reserved returns the record that sets the bound of the numbers pack writers may be given. */
void
chunks_packs_Table_reserved(uint64 bound, StdVec *result)
{
	uint8		body[CHUNKS_PACKS_NUMBER];

	chunks_packs_number_bytes(bound, body);
	chunks_packs_record(CHUNKS_PACKS_RESERVED, body, sizeof(body), result);
}

/* chunks_packs_Table_retired returns the record that retires packs, written into the one allocation its length needs. */
void
chunks_packs_Table_retired(const uint64 *numbers, size_t numbers_len, StdVec *result)
{
	std_vec_with_capacity_Checked(result,
								  std_wrapping_add_usize(1, std_wrapping_mul_usize(numbers_len, CHUNKS_PACKS_NUMBER)),
								  1, 1);
	std_vec_u8_push_Checked(result, CHUNKS_PACKS_REMOVED);
	for (size_t k = 0, k_len = numbers_len; k < k_len; k++)
	{
		uint8		bytes[CHUNKS_PACKS_NUMBER];

		chunks_packs_number_bytes(numbers[k], bytes);
		std_vec_extend_from_slice_Checked(result, bytes, sizeof(bytes), 1, 1);
	}
}

/* chunks_packs_Table_vector returns the record that names the generation, never zero, of a listed pack's deletion vector. */
void
chunks_packs_Table_vector(uint64 number, uint64 generation, StdVec *result)
{
	uint8		body[CHUNKS_PACKS_NUMBER + 8];

	chunks_packs_number_bytes(number, body);
	std_u64_to_le_bytes(generation, STD_PTR_AT(body, CHUNKS_PACKS_NUMBER, 1));
	chunks_packs_record(CHUNKS_PACKS_VECTOR, body, sizeof(body), result);
}

/*
 * chunks_packs_Table_moved returns the record that says where a tablespace's
 * runs file ends after a change to that file alone: a merge of runs, a
 * removal, or a whole write of it.
 */
void
chunks_packs_Table_moved(uint32 space, ChunksPacksTail runs, StdVec *result)
{
	uint8		body[4 + CHUNKS_PACKS_TAIL];

	std_u32_to_le_bytes(space, body);
	chunks_packs_Tail_bytes(&runs, STD_PTR_AT(body, 4, 1));
	chunks_packs_record(CHUNKS_PACKS_MOVED, body, sizeof(body), result);
}

/*
 * chunks_packs_Table_apply_whole applies a whole record's body, whose packs'
 * entries take entries_len bytes after its head: its numbers, then its packs,
 * then its runs tails, each in place of the table's, a key's last entry kept
 * as a BTreeMap's collect keeps it.  Each map is made in the one allocation
 * its entries need before the table takes it, so nothing is left owned when
 * an allocation fails.
 */
static void
chunks_packs_Table_apply_whole(ChunksPacksTable *self, const uint8 *body, size_t body_len, size_t entries_len)
{
	const uint8 *entries = STD_PTR_AT(body, CHUNKS_PACKS_HEAD, 1);
	const uint8 *tails = STD_PTR_AT(entries, entries_len, 1);
	size_t		tails_len = std_wrapping_sub_usize(std_wrapping_sub_usize(body_len, CHUNKS_PACKS_HEAD), entries_len);
	size_t		packs_len = STD_DIV_FOLDED(usize, entries_len, CHUNKS_PACKS_LISTED);
	size_t		runs_len = STD_DIV_FOLDED(usize, tails_len, 4 + CHUNKS_PACKS_TAIL);
	StdVec		packs;
	StdVec		runs;

	self->generation = chunks_packs_long(body, body_len, 0);
	self->added = chunks_packs_number(body, body_len, 8);
	self->reserved = chunks_packs_number(body, body_len, 8 + CHUNKS_PACKS_NUMBER);
	std_vec_with_capacity_Checked(&packs, packs_len, sizeof(chunks_TupleU64Listed_t),
								  _Alignof(chunks_TupleU64Listed_t));
	for (size_t k = 0, k_len = packs_len; k < k_len; k++)
	{
		const uint8 *at = STD_PTR_AT(entries, std_wrapping_mul_usize(k, CHUNKS_PACKS_LISTED), 1);
		ChunksPacksTableEntry entry = chunks_packs_entry_of(at);
		uint64		dead = chunks_packs_long(at, CHUNKS_PACKS_LISTED, CHUNKS_PACKS_ENTRY);
		ChunksPacksListed listed = {entry.f1, entry.f2, dead != 0, dead};

		chunks_packs_table_packs_insert(&packs, entry.f0, &listed);
	}
	std_vec_free_Checked(&self->packs, sizeof(chunks_TupleU64Listed_t), _Alignof(chunks_TupleU64Listed_t));
	self->packs = packs;
	std_vec_with_capacity_Checked(&runs, runs_len, sizeof(chunks_TupleU32Tail_t), _Alignof(chunks_TupleU32Tail_t));
	for (size_t k = 0, k_len = runs_len; k < k_len; k++)
	{
		const uint8 *at = STD_PTR_AT(tails, std_wrapping_mul_usize(k, 4 + CHUNKS_PACKS_TAIL), 1);

		chunks_packs_table_runs_insert(&runs, chunks_packs_word(at, 4 + CHUNKS_PACKS_TAIL, 0),
									   chunks_packs_Tail_read(STD_PTR_AT(at, 4, 1)));
	}
	std_vec_free_Checked(&self->runs, sizeof(chunks_TupleU32Tail_t), _Alignof(chunks_TupleU32Tail_t));
	self->runs = runs;
}

/* chunks_packs_Table_corrupt returns the error of a record of the pack table no writer of its file makes. */
static bool
chunks_packs_Table_corrupt(ChunksErrorError *error)
{
	*error = chunks_error_Error_corrupt_RefStr(CHUNKS_PACKS_TABLE_CORRUPT, strlen(CHUNKS_PACKS_TABLE_CORRUPT));
	return false;
}

/* chunks_packs_Table_apply changes the table as one record of its file says, refusing a record no writer of the file makes. */
bool
chunks_packs_Table_apply(ChunksPacksTable *self, const uint8 *record, size_t record_len, ChunksErrorError *error)
{
	const uint8 *body;
	size_t		body_len;

	if (record_len == 0)
		return chunks_packs_Table_corrupt(error);
	body = STD_PTR_AT(record, 1, 1);
	body_len = std_wrapping_sub_usize(record_len, 1);
	switch (record[0])
	{
		case CHUNKS_PACKS_WHOLE:
			{
				size_t		rest_len;
				size_t		entries_len;

				if (body_len < CHUNKS_PACKS_HEAD)
					return chunks_packs_Table_corrupt(error);
				rest_len = std_wrapping_sub_usize(body_len, CHUNKS_PACKS_HEAD);
				if (!std_checked_mul_usize((size_t) chunks_packs_word(body, body_len, 8 + 2 * CHUNKS_PACKS_NUMBER),
										   CHUNKS_PACKS_LISTED, &entries_len) ||
					entries_len > rest_len ||
					STD_REM_FOLDED(usize, std_wrapping_sub_usize(rest_len, entries_len), 4 + CHUNKS_PACKS_TAIL) != 0)
					return chunks_packs_Table_corrupt(error);
				chunks_packs_Table_apply_whole(self, body, body_len, entries_len);
				return true;
			}
		case CHUNKS_PACKS_ADDED:
			{
				ChunksPacksTableEntry entry;
				ChunksPacksListed listed;

				if (body_len != CHUNKS_PACKS_ENTRY + CHUNKS_PACKS_TAIL)
					return chunks_packs_Table_corrupt(error);
				entry = chunks_packs_entry_of(body);
				listed.space = entry.f1;
				listed.id = entry.f2;
				listed.has_dead = false;
				listed.dead = 0;
				self->added = std_max_u64(self->added, entry.f0);
				chunks_packs_table_packs_insert(&self->packs, entry.f0, &listed);
				chunks_packs_table_runs_insert(&self->runs, entry.f1,
											   chunks_packs_Tail_read(STD_PTR_AT(body, CHUNKS_PACKS_ENTRY, 1)));
				return true;
			}
		case CHUNKS_PACKS_VECTOR:
			{
				ChunksPacksListed *listed;
				uint64		generation = 0;

				if (body_len < CHUNKS_PACKS_NUMBER)
					return chunks_packs_Table_corrupt(error);
				if (body_len == CHUNKS_PACKS_NUMBER + 8)
					generation = std_u64_from_le_bytes(STD_PTR_AT(body, CHUNKS_PACKS_NUMBER, 1));
				listed = chunks_packs_table_packs_get(&self->packs, chunks_packs_number(body, body_len, 0));
				if (listed == NULL || generation == 0)
					return chunks_packs_Table_corrupt(error);
				listed->has_dead = true;
				listed->dead = generation;
				return true;
			}
		case CHUNKS_PACKS_REMOVED:
			if (STD_REM_FOLDED(usize, body_len, CHUNKS_PACKS_NUMBER) != 0)
				return chunks_packs_Table_corrupt(error);
			for (size_t k = 0, k_len = STD_DIV_FOLDED(usize, body_len, CHUNKS_PACKS_NUMBER); k < k_len; k++)
				chunks_packs_table_packs_remove(&self->packs,
												chunks_packs_number(body, body_len,
																	std_wrapping_mul_usize(k, CHUNKS_PACKS_NUMBER)));
			return true;
		case CHUNKS_PACKS_RESERVED:
			if (body_len != CHUNKS_PACKS_NUMBER)
				return chunks_packs_Table_corrupt(error);
			self->reserved = chunks_packs_number(body, body_len, 0);
			return true;
		case CHUNKS_PACKS_MOVED:
			if (body_len != 4 + CHUNKS_PACKS_TAIL)
				return chunks_packs_Table_corrupt(error);
			chunks_packs_table_runs_insert(&self->runs, std_u32_from_le_bytes(body),
										   chunks_packs_Tail_read(STD_PTR_AT(body, 4, 1)));
			return true;
		default:
			return chunks_packs_Table_corrupt(error);
	}
}

/* chunks_packs_field_end returns where the field of a runs file's line that starts at a place ends: its next colon, or the line's end. */
static size_t
chunks_packs_field_end(const char *line, size_t line_len, size_t at)
{
	while (at < line_len && line[at] != ':')
		at = std_wrapping_add_usize(at, 1);
	return at;
}

/* chunks_packs_run_name returns the file name of the run a line of a runs file names: the line up to its first colon. */
StdStr
chunks_packs_run_name(const char *line, size_t line_len)
{
	StdStr		name = {line, chunks_packs_field_end(line, line_len, 0)};

	return name;
}

/*
 * chunks_packs_lossy reports whether a line of a runs file marks its run as
 * written from a pack whose address table didn't read whole: its third field
 * is the mark.
 */
bool
chunks_packs_lossy(const char *line, size_t line_len)
{
	size_t		at = 0;
	size_t		end;

	for (size_t k = 0, k_len = 2; k < k_len; k++)
	{
		end = chunks_packs_field_end(line, line_len, at);
		if (end == line_len)
			return false;
		at = std_wrapping_add_usize(end, 1);
	}
	end = chunks_packs_field_end(line, line_len, at);
	return std_wrapping_sub_usize(end, at) == strlen(CHUNKS_PACKS_LOSSY) &&
		memcmp(STD_PTR_AT(line, at, 1), CHUNKS_PACKS_LOSSY, strlen(CHUNKS_PACKS_LOSSY)) == 0;
}

/* chunks_packs_next_line is str::lines' next: a text's next line from a place, without its "\n" or "\r\n". */
static bool
chunks_packs_next_line(const char *text, size_t text_len, size_t *at, StdStr *line)
{
	size_t		start = *at;
	size_t		end = start;

	if (start >= text_len)
		return false;
	while (end < text_len && text[end] != '\n')
		end = std_wrapping_add_usize(end, 1);
	line->ptr = STD_PTR_AT(text, start, 1);
	line->len = std_wrapping_sub_usize(end, start);
	if (end < text_len)
	{
		*at = std_wrapping_add_usize(end, 1);
		if (line->len > 0 && line->ptr[line->len - 1] == '\r')
			line->len = std_wrapping_sub_usize(line->len, 1);
	}
	else
		*at = end;
	return true;
}

/* chunks_packs_Runs_named reports whether one of a text's lines is the file name of the run a runs file's line names. */
static bool
chunks_packs_Runs_named(const StdString *line, const char *names, size_t names_len)
{
	StdStr		run = chunks_packs_run_name(line->ptr, line->len);
	size_t		at = 0;
	StdStr		name;

	while (chunks_packs_next_line(names, names_len, &at, &name))
		if (name.len == run.len && memcmp(name.ptr, run.ptr, run.len) == 0)
			return true;
	return false;
}

/* chunks_packs_Runs_Default_default is Runs' Default: generation 0 and no lines. */
void
chunks_packs_Runs_Default_default(ChunksPacksRuns *result)
{
	result->generation = 0;
	std_vec_new(&result->lines, sizeof(StdString), _Alignof(StdString));
}

/* chunks_packs_Runs_drop is Runs' drop glue: its lines'. */
void
chunks_packs_Runs_drop(ChunksPacksRuns *self)
{
	std_vec_drop_Checked(&self->lines, sizeof(StdString), _Alignof(StdString), std_String_drop);
}

/* chunks_packs_Runs_lines_Debug_fmt is the Debug of Runs' lines, a Vec<String>'s: a list of quoted strings. */
static bool
chunks_packs_Runs_lines_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *self = value;
	StdDebugList builder = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = self->len; k < k_len; k++)
	{
		const StdString *line = &STD_VEC_AT(self, StdString, k);
		StdStr text = {line->ptr, line->len};

		std_DebugList_entry(&builder, &text, std_str_Debug_fmt);
	}
	return std_DebugList_finish(&builder);
}

/* chunks_packs_Runs_Debug_fmt is Runs' derived Debug. */
bool
chunks_packs_Runs_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksRuns *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Runs", strlen("Runs"));

	std_DebugStruct_field(&s, "generation", strlen("generation"), &self->generation, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "lines", strlen("lines"), &self->lines, chunks_packs_Runs_lines_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_packs_Runs_PartialEq_eq is Runs' derived PartialEq: its generation, and its lines one by one. */
bool
chunks_packs_Runs_PartialEq_eq(const ChunksPacksRuns *self, const ChunksPacksRuns *other)
{
	if (self->generation != other->generation || self->lines.len != other->lines.len)
		return false;
	for (size_t k = 0, k_len = self->lines.len; k < k_len; k++)
	{
		const StdString *a = &STD_VEC_AT(&self->lines, StdString, k);
		const StdString *b = &STD_VEC_AT(&other->lines, StdString, k);

		if (a->len != b->len || (a->len != 0 && memcmp(a->ptr, b->ptr, a->len) != 0))
			return false;
	}
	return true;
}

/* chunks_packs_joined returns lines joined by "\n", as [String]::join does, after a prefix of bytes. */
static void
chunks_packs_joined(const uint8 *prefix, size_t prefix_len, const StdString *lines, size_t lines_len, StdVec *result)
{
	std_vec_new(result, 1, 1);
	std_vec_extend_from_slice_Checked(result, prefix, prefix_len, 1, 1);
	for (size_t k = 0; k < lines_len; k++)
	{
		if (k != 0)
			std_vec_u8_push_Checked(result, '\n');
		std_vec_extend_from_slice_Checked(result, lines[k].ptr, lines[k].len, 1, 1);
	}
}

/*
 * chunks_packs_body_frame_0_t is the state across its pad of a runs file's
 * record made from a joined body: its arguments, its result, and the body it
 * holds.
 */
typedef struct chunks_packs_body_frame_0_t
{
	const ChunksPacksRuns *self;
	const StdString *names;
	size_t		names_len;
	const char *line;
	size_t		line_len;
	StdVec	   *result;
	StdVec		body;
} chunks_packs_body_frame_0_t;

/* chunks_packs_body_pad_0 is a joined record's pad: its body. */
static void
chunks_packs_body_pad_0(void *frame_)
{
	chunks_packs_body_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->body, 1, 1);
	std_vec_new(&frame->body, 1, 1);
}

/* chunks_packs_body_run runs a joined record's body in a frame of its arguments. */
static void
chunks_packs_body_run(chunks_packs_body_frame_0_t *frame, void (*body) (void *frame_))
{
	std_vec_new(&frame->body, 1, 1);
	std_landing(frame, body, chunks_packs_body_pad_0);
}

/* chunks_packs_Runs_whole_body_0 is whole's body. */
static void
chunks_packs_Runs_whole_body_0(void *frame_)
{
	chunks_packs_body_frame_0_t *frame = frame_;
	uint8		generation[8];

	std_u64_to_le_bytes(frame->self->generation, generation);
	chunks_packs_joined(generation, 8, frame->self->lines.ptr, frame->self->lines.len, &frame->body);
	chunks_packs_record(CHUNKS_PACKS_WHOLE, frame->body.ptr, frame->body.len, frame->result);
	chunks_packs_body_pad_0(frame);
}

/* chunks_packs_Runs_whole returns the record that begins a runs file: the generation and every run's line. */
void
chunks_packs_Runs_whole(const ChunksPacksRuns *self, StdVec *result)
{
	chunks_packs_body_frame_0_t frame = {.self = self, .result = result};

	chunks_packs_body_run(&frame, chunks_packs_Runs_whole_body_0);
}

/* chunks_packs_Runs_added returns the record that adds a run, by its line. */
void
chunks_packs_Runs_added(const char *line, size_t line_len, StdVec *result)
{
	chunks_packs_record(CHUNKS_PACKS_ADDED, (const uint8 *) line, line_len, result);
}

/* chunks_packs_Runs_removed_body_0 is removed's body. */
static void
chunks_packs_Runs_removed_body_0(void *frame_)
{
	chunks_packs_body_frame_0_t *frame = frame_;

	chunks_packs_joined(NULL, 0, frame->names, frame->names_len, &frame->body);
	chunks_packs_record(CHUNKS_PACKS_REMOVED, frame->body.ptr, frame->body.len, frame->result);
	chunks_packs_body_pad_0(frame);
}

/* chunks_packs_Runs_removed returns the record that removes runs, by their file names. */
void
chunks_packs_Runs_removed(const StdString *names, size_t names_len, StdVec *result)
{
	chunks_packs_body_frame_0_t frame = {.names = names, .names_len = names_len, .result = result};

	chunks_packs_body_run(&frame, chunks_packs_Runs_removed_body_0);
}

/* chunks_packs_Runs_replaced_body_0 is replaced's body. */
static void
chunks_packs_Runs_replaced_body_0(void *frame_)
{
	chunks_packs_body_frame_0_t *frame = frame_;

	chunks_packs_joined(NULL, 0, frame->names, frame->names_len, &frame->body);
	std_vec_u8_push_Checked(&frame->body, '\0');
	std_vec_extend_from_slice_Checked(&frame->body, frame->line, frame->line_len, 1, 1);
	chunks_packs_record(CHUNKS_PACKS_REPLACED, frame->body.ptr, frame->body.len, frame->result);
	chunks_packs_body_pad_0(frame);
}

/*
 * chunks_packs_Runs_replaced returns the record that puts a merged run's line
 * in the place of the runs it merged, by their file names.
 */
void
chunks_packs_Runs_replaced(const StdString *names, size_t names_len, const char *line, size_t line_len,
						   StdVec *result)
{
	chunks_packs_body_frame_0_t frame = {.names = names, .names_len = names_len, .line = line,
	.line_len = line_len, .result = result};

	chunks_packs_body_run(&frame, chunks_packs_Runs_replaced_body_0);
}

/* chunks_packs_Runs_corrupt returns the error of a record of a runs file no writer of it makes. */
static bool
chunks_packs_Runs_corrupt(ChunksErrorError *error)
{
	static const char text[] = "a record of a runs file is corrupt";

	*error = chunks_error_Error_corrupt_RefStr(text, sizeof(text) - 1);
	return false;
}

/* chunks_packs_Runs_line returns a line as a String of its own. */
static StdString
chunks_packs_Runs_line(const char *line, size_t line_len)
{
	StdString	owned;

	std_string_from_Checked(&owned, line, line_len);
	return owned;
}

/*
 * chunks_packs_Runs_apply_frame_0_t is apply's state across its pad: its
 * arguments, its result, and the lines and line it holds.
 */
typedef struct chunks_packs_Runs_apply_frame_0_t
{
	ChunksPacksRuns *self;
	const uint8 *record;
	size_t		record_len;
	ChunksErrorError *error;
	bool		ok;
	StdVec		lines;
	StdString	owned;
} chunks_packs_Runs_apply_frame_0_t;

/* chunks_packs_Runs_apply_pad_0 is apply's pad: the line, then the lines. */
static void
chunks_packs_Runs_apply_pad_0(void *frame_)
{
	chunks_packs_Runs_apply_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->owned, 1, 1);
	std_vec_new(&frame->owned, 1, 1);
	std_vec_drop_Checked(&frame->lines, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_new(&frame->lines, sizeof(StdString), _Alignof(StdString));
}

/* chunks_packs_Runs_apply_owned makes the frame's line a String of its own. */
static void
chunks_packs_Runs_apply_owned(chunks_packs_Runs_apply_frame_0_t *frame, const char *line, size_t line_len)
{
	frame->owned = chunks_packs_Runs_line(line, line_len);
}

/* chunks_packs_Runs_apply_pushed pushes the frame's line onto a list of lines, which then holds it. */
static void
chunks_packs_Runs_apply_pushed(chunks_packs_Runs_apply_frame_0_t *frame, StdVec *lines)
{
	std_vec_push_Checked(lines, &frame->owned, sizeof(frame->owned), _Alignof(StdString));
	std_vec_new(&frame->owned, 1, 1);
}

/* chunks_packs_Runs_apply_body_0 is apply's body. */
static void
chunks_packs_Runs_apply_body_0(void *frame_)
{
	chunks_packs_Runs_apply_frame_0_t *frame = frame_;
	ChunksPacksRuns *self = frame->self;
	const char *body;
	size_t		body_len;
	StdUtf8Error utf8;

	frame->ok = true;
	if (frame->record_len == 0)
		goto corrupt;
	body = (const char *) STD_PTR_AT(frame->record, 1, 1);
	body_len = std_wrapping_sub_usize(frame->record_len, 1);
	switch (frame->record[0])
	{
		case CHUNKS_PACKS_WHOLE:
			{
				const char *text;
				size_t		text_len;
				size_t		at = 0;
				StdStr		line;

				if (body_len < 8)
					goto corrupt;
				self->generation = std_u64_from_le_bytes((const uint8 *) body);
				text = STD_PTR_AT(body, 8, 1);

				text_len = std_wrapping_sub_usize(body_len, 8);
				if (!std_str_from_utf8((const uint8 *) text, text_len, &utf8))
					goto corrupt;
				while (chunks_packs_next_line(text, text_len, &at, &line))
				{
					chunks_packs_Runs_apply_owned(frame, line.ptr, line.len);
					chunks_packs_Runs_apply_pushed(frame, &frame->lines);
				}
				chunks_packs_Runs_drop(self);
				self->lines = frame->lines;
				std_vec_new(&frame->lines, sizeof(StdString), _Alignof(StdString));
				break;
			}
		case CHUNKS_PACKS_ADDED:
			{
				StdStr		name;

				if (!std_str_from_utf8((const uint8 *) body, body_len, &utf8))
					goto corrupt;
				name = chunks_packs_run_name(body, body_len);
				for (size_t k = 0, k_len = self->lines.len; k < k_len; k++)
				{
					const StdString *old = &STD_VEC_AT(&self->lines, StdString, k);
					StdStr		named = chunks_packs_run_name(old->ptr, old->len);

					if (named.len == name.len && memcmp(named.ptr, name.ptr, name.len) == 0)
						goto done;
				}
				chunks_packs_Runs_apply_owned(frame, body, body_len);
				chunks_packs_Runs_apply_pushed(frame, &self->lines);
				break;
			}
		case CHUNKS_PACKS_REMOVED:
			{
				size_t		kept = 0;

				if (!std_str_from_utf8((const uint8 *) body, body_len, &utf8))
					goto corrupt;
				for (size_t k = 0, k_len = self->lines.len; k < k_len; k++)
				{
					StdString  *line = &STD_VEC_AT(&self->lines, StdString, k);

					if (chunks_packs_Runs_named(line, body, body_len))
						std_String_drop(line);
					else
						STD_VEC_AT(&self->lines, StdString, kept++) = *line;
				}
				self->lines.len = kept;
				break;
			}
		case CHUNKS_PACKS_REPLACED:
			{
				const char *nul;
				size_t		names_len;
				size_t		position;
				size_t		kept = 0;

				if (!std_str_from_utf8((const uint8 *) body, body_len, &utf8))
					goto corrupt;
				nul = body_len == 0 ? NULL : memchr(body, '\0', body_len);
				if (nul == NULL)
					goto corrupt;
				names_len = (size_t) (nul - body);
				position = self->lines.len;
				for (size_t k = 0, k_len = self->lines.len; k < k_len; k++)
					if (chunks_packs_Runs_named(&STD_VEC_AT(&self->lines, StdString, k), body, names_len))
					{
						position = k;
						break;
					}
				for (size_t k = 0, k_len = self->lines.len; k < k_len; k++)
				{
					StdString  *line = &STD_VEC_AT(&self->lines, StdString, k);

					if (chunks_packs_Runs_named(line, body, names_len))
						std_String_drop(line);
					else
						STD_VEC_AT(&self->lines, StdString, kept++) = *line;
				}
				self->lines.len = kept;
				chunks_packs_Runs_apply_owned(frame, STD_PTR_AT(nul, 1, 1),
											  std_wrapping_sub_usize(std_wrapping_sub_usize(body_len, names_len), 1));
				std_vec_insert(&self->lines, std_min_usize(position, self->lines.len), &frame->owned,
							   sizeof(frame->owned), _Alignof(StdString), RUST_FILE, 391, 28);
				std_vec_new(&frame->owned, 1, 1);
				break;
			}
		default:
			goto corrupt;
	}
	goto done;
corrupt:
	frame->ok = chunks_packs_Runs_corrupt(frame->error);
done:
	chunks_packs_Runs_apply_pad_0(frame);
}

/*
 * chunks_packs_Runs_apply changes the runs as one record of their file says,
 * refusing a record no writer of the file makes.
 */
bool
chunks_packs_Runs_apply(ChunksPacksRuns *self, const uint8 *record, size_t record_len, ChunksErrorError *error)
{
	chunks_packs_Runs_apply_frame_0_t frame = {.self = self, .record = record, .record_len = record_len,
	.error = error};

	std_vec_new(&frame.lines, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&frame.owned, 1, 1);
	std_landing(&frame, chunks_packs_Runs_apply_body_0, chunks_packs_Runs_apply_pad_0);
	return frame.ok;
}

/* chunks_packs_followed_frame_0_t is followed's state across its pad: the forwards' fences. */
typedef struct chunks_packs_followed_frame_0_t
{
	HostIoReadDoors io;
	const ChunksIndexAddressIndexLocator *forwards;
	const ChunksPacksTable *table;
	ChunksTrailerLocator copy;
	ChunksTrailerLocator *result;
	ChunksErrorError *error;
	bool		ok;
	ChunksPacksFollowIndexed asked;
} chunks_packs_followed_frame_0_t;

/* chunks_packs_followed_pad_0 is followed's pad: the forwards' fences. */
static void
chunks_packs_followed_pad_0(void *frame_)
{
	chunks_packs_followed_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->asked.fences, sizeof(const ChunksIndexFences *), _Alignof(const ChunksIndexFences *));
	std_vec_new(&frame->asked.fences, sizeof(const ChunksIndexFences *), _Alignof(const ChunksIndexFences *));
}

/*
 * chunks_packs_followed_body_0 is followed's body: the walk through the
 * forwards' blocks, a stop at a failed read the forwards' unread error, and
 * one at a forward that falls the damage of the block it was read from.
 */
static void
chunks_packs_followed_body_0(void *frame_)
{
	chunks_packs_followed_frame_0_t *frame = frame_;
	ChunksIndexBlocks blocks = {{&CHUNKS_INDEX_ADDRESS_INDEX_LOCATOR_RUNS_OPS, (void *) frame->forwards}};
	ChunksPacksFollowForwards forwards = {&CHUNKS_PACKS_FOLLOW_INDEXED_FORWARDS_OPS, &frame->asked};
	ChunksPacksFollowStopped stopped;
	ChunksIndexUnreadError unread;

	frame->asked.source.ops = &CHUNKS_INDEX_BLOCKS_SOURCE_OPS;
	frame->asked.source.self = &blocks;
	std_vec_free_Checked(&frame->asked.fences, sizeof(const ChunksIndexFences *),
						 _Alignof(const ChunksIndexFences *));
	frame->asked.fences = chunks_index_AddressIndexLocator_fences(frame->forwards);
	frame->ok = chunks_packs_follow_follow(forwards, &frame->io, frame->copy, &frame->table->packs,
										   frame->table->added, frame->result, &stopped, &unread);
	if (!frame->ok && stopped.tag == CHUNKS_PACKS_FOLLOW_STOPPED_SOURCE)
		*frame->error = chunks_index_AddressIndexLocator_unread(frame->forwards, &unread);
	else if (!frame->ok)
	{
		size_t		runs_len;
		StdArcInner *const *runs = chunks_index_AddressIndexLocator_runs(frame->forwards, &runs_len);
		uint64		offset = chunks_trailer_Locator_offset(&stopped.from);
		StdString	holds;
		StdFmtPiece pieces[] = {STD_FMT_LIT("a forward from offset "), STD_FMT_ARG(0), STD_FMT_LIT(" of pack "),
			STD_FMT_ARG(1), STD_FMT_LIT(" to pack "), STD_FMT_ARG(2),
		STD_FMT_LIT(", which is not a later pack the table added")};
		StdFmtArg	args[] = {{&offset, std_u64_Display_fmt}, {&stopped.from.pack, std_u64_Display_fmt},
		{&stopped.to.pack, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, 7, args};

		std_index_check(stopped.read.f0, runs_len, RUST_FILE, 434, 49);
		std_fmt_format_Checked(&holds, &arguments);
		*frame->error = chunks_index_RunLocator_damaged(std_arc_Deref_deref(runs[stopped.read.f0],
																			_Alignof(ChunksIndexRunLocator)),
														stopped.read.f1, holds.ptr, holds.len);
		std_String_drop(&holds);
	}
	chunks_packs_followed_pad_0(frame);
}

/*
 * chunks_packs_followed returns the last place of a copy that collections
 * moved, through a repository's forwards, stopping at a pack the table lists
 * or at a place no forward leads on from. A forward that leads to a pack
 * numbered no higher than its own, or higher than any the table added, is
 * damage where it was read.
 */
bool
chunks_packs_followed(HostIoReadDoors io, const ChunksIndexAddressIndexLocator *forwards,
					  const ChunksPacksTable *table, ChunksTrailerLocator copy, ChunksTrailerLocator *result,
					  ChunksErrorError *error)
{
	chunks_packs_followed_frame_0_t frame = {.io = io, .forwards = forwards, .table = table, .copy = copy,
	.result = result, .error = error};

	std_vec_new(&frame.asked.fences, sizeof(const ChunksIndexFences *), _Alignof(const ChunksIndexFences *));
	std_landing(&frame, chunks_packs_followed_body_0, chunks_packs_followed_pad_0);
	return frame.ok;
}

/* chunks_packs_forward_entries_closure_0 is forward_entries' |entry| entry.0: an entry's key, the old place's. */
static ChunksAddressAddress
chunks_packs_forward_entries_closure_0(void *captures, const void *entry)
{
	(void) captures;
	return ((const chunks_TupleAddressLocator_t *) entry)->f0;
}

/* The sort of (Address, Locator) entries that forward_entries runs: core's shared sort parts at the tuple, which its stable sort calls. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS TupleAddressLocator
#define STD_SORT_T chunks_TupleAddressLocator_t
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

/* The stable sort_by_key of (Address, Locator) entries by their key, which forward_entries runs. */
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS TupleAddressLocator
#define STD_STABLE_SORT_T chunks_TupleAddressLocator_t
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY_KEY
#define STD_STABLE_SORT_KEY_KS Address
#define STD_STABLE_SORT_KEY_K ChunksAddressAddress
#define STD_STABLE_SORT_KEY_LT(a, b) chunks_address_Address_PartialOrd_lt((a), (b))
#include "doltgres/std/stable_sort_template.h"

/*
 * chunks_packs_forward_entries_frame_0_t is forward_entries' state across its
 * pad: its arguments, its result, and the entries it holds.
 */
typedef struct chunks_packs_forward_entries_frame_0_t
{
	const chunks_TupleLocatorLocator_t *moves;
	size_t		moves_len;
	StdVec	   *result;
	StdVec		entries;
} chunks_packs_forward_entries_frame_0_t;

/* chunks_packs_forward_entries_pad_0 is forward_entries' pad: the entries. */
static void
chunks_packs_forward_entries_pad_0(void *frame_)
{
	chunks_packs_forward_entries_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_vec_new(&frame->entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
}

/* chunks_packs_forward_entries_body_0 is forward_entries' body. */
static void
chunks_packs_forward_entries_body_0(void *frame_)
{
	chunks_packs_forward_entries_frame_0_t *frame = frame_;

	std_vec_with_capacity_Checked(&frame->entries, frame->moves_len, sizeof(chunks_TupleAddressLocator_t),
								  _Alignof(chunks_TupleAddressLocator_t));
	for (size_t k = 0; k < frame->moves_len; k++)
	{
		const chunks_TupleLocatorLocator_t *move = &frame->moves[k];
		std_TupleU64U64_t from = {move->f0.pack, chunks_trailer_Locator_offset(&move->f0)};
		chunks_TupleAddressLocator_t entry = {chunks_packs_follow_place_key(&from), move->f1};

		std_vec_push_Checked(&frame->entries, &entry, sizeof(entry), _Alignof(chunks_TupleAddressLocator_t));
	}
	std_slice_sort_by_key_TupleAddressLocatorAddress(frame->entries.ptr, frame->entries.len,
													 chunks_packs_forward_entries_closure_0, NULL, NULL);
	*frame->result = frame->entries;
	std_vec_new(&frame->entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
}

/*
 * chunks_packs_forward_entries returns the entries of a run of forwards for
 * the chunks a collection moved, each from its old place to its copy's: the
 * copy's place under the old place's key, in key order.
 */
void
chunks_packs_forward_entries(const chunks_TupleLocatorLocator_t *moves, size_t moves_len, StdVec *result)
{
	chunks_packs_forward_entries_frame_0_t frame = {.moves = moves, .moves_len = moves_len, .result = result};

	std_vec_new(&frame.entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_landing(&frame, chunks_packs_forward_entries_body_0, chunks_packs_forward_entries_pad_0);
}

/*
 * chunks_packs_kept_forwards_frame_0_t is kept_forwards' state across its
 * pad: its arguments, its result, and the dropped places' keys and the
 * forwards it holds.
 */
typedef struct chunks_packs_kept_forwards_frame_0_t
{
	HostIoIoDoors io;
	const ChunksIndexAddressIndexLocator *forwards;
	const std_TupleU64U64_t *dropped;
	size_t		dropped_len;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		keys;
	StdVec		kept;
} chunks_packs_kept_forwards_frame_0_t;

/* chunks_packs_kept_forwards_pad_0 is kept_forwards' pad: the forwards, then the keys. */
static void
chunks_packs_kept_forwards_pad_0(void *frame_)
{
	chunks_packs_kept_forwards_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->kept, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_vec_new(&frame->kept, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_vec_free_Checked(&frame->keys, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	std_vec_new(&frame->keys, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
}

/* chunks_packs_kept_forwards_place returns where a key is, or goes, among the dropped places' keys, and whether it is there. */
static size_t
chunks_packs_kept_forwards_place(const StdVec *keys, const ChunksAddressAddress *key, bool *found)
{
	size_t		low = 0;
	size_t		high = keys->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		int			cmp = memcmp(STD_VEC_AT(keys, ChunksAddressAddress, mid).f0, key->f0, sizeof(key->f0));

		if (cmp == 0)
		{
			*found = true;
			return mid;
		}
		if (cmp < 0)
			low = mid + 1;
		else
			high = mid;
	}
	*found = false;
	return low;
}

/* chunks_packs_kept_forwards_body_0 is kept_forwards' body. */
static void
chunks_packs_kept_forwards_body_0(void *frame_)
{
	chunks_packs_kept_forwards_frame_0_t *frame = frame_;
	size_t		runs_len;
	size_t		kept = 0;

	for (size_t k = 0; k < frame->dropped_len; k++)
	{
		ChunksAddressAddress key = chunks_packs_follow_place_key(&frame->dropped[k]);
		bool		found;
		size_t		at = chunks_packs_kept_forwards_place(&frame->keys, &key, &found);

		if (!found)
			std_vec_insert(&frame->keys, at, &key, sizeof(key), _Alignof(ChunksAddressAddress), RUST_FILE, 416, 74);
	}
	(void) chunks_index_AddressIndexLocator_runs(frame->forwards, &runs_len);
	if (!chunks_index_AddressIndexLocator_merged(frame->forwards, HOST_IO_IO_DOORS_READ_DOORS(frame->io), 0, runs_len,
												 &frame->kept, frame->error))
		goto done;
	for (size_t k = 0, k_len = frame->kept.len; k < k_len; k++)
	{
		const chunks_TupleAddressLocator_t *entry = &STD_VEC_AT(&frame->kept, chunks_TupleAddressLocator_t, k);
		bool		found;

		(void) chunks_packs_kept_forwards_place(&frame->keys, &entry->f0, &found);
		if (!found)
			STD_VEC_AT(&frame->kept, chunks_TupleAddressLocator_t, kept++) = *entry;
	}
	frame->kept.len = kept;
	*frame->result = frame->kept;
	std_vec_new(&frame->kept, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	frame->ok = true;
done:
	chunks_packs_kept_forwards_pad_0(frame);
}

/*
 * chunks_packs_kept_forwards returns every forward of a repository's forwards
 * but those that lead from the places a collection drops, as the entries of
 * the one run that takes the place of them all.
 */
bool
chunks_packs_kept_forwards(HostIoIoDoors io, const ChunksIndexAddressIndexLocator *forwards,
						   const std_TupleU64U64_t *dropped, size_t dropped_len, StdVec *result,
						   ChunksErrorError *error)
{
	chunks_packs_kept_forwards_frame_0_t frame = {.io = io, .forwards = forwards, .dropped = dropped,
	.dropped_len = dropped_len, .result = result, .error = error};

	std_vec_new(&frame.keys, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	std_vec_new(&frame.kept, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_landing(&frame, chunks_packs_kept_forwards_body_0, chunks_packs_kept_forwards_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* CHUNKS_PACKS_TESTS_UNWRAP_ERR is Result::unwrap_err's message on an Ok. */
#define CHUNKS_PACKS_TESTS_UNWRAP_ERR "called `Result::unwrap_err()` on an `Ok` value"

/* chunks_packs_tests_Options_Debug_fmt is a pair of Options' Debug, in a tuple, as two StdOptionRefs. */
static bool
chunks_packs_tests_Options_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdOptionRef *options = value;
	StdDebugTuple b = std_fmt_debug_tuple(f, "", 0);

	std_DebugTuple_field(&b, &options[0], std_option_Debug_fmt);
	std_DebugTuple_field(&b, &options[1], std_option_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/* chunks_packs_tests_OptionVec_Debug_fmt is a Vec<Option<T>>'s Debug, of StdOptionRefs, or an array's, which reads alike. */
static bool
chunks_packs_tests_OptionVec_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *options = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = options->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(options, StdOptionRef, k), std_option_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_tests_Pair_Debug_fmt is a (u64, usize)'s Debug, in a tuple. */
static bool
chunks_packs_tests_Pair_Debug_fmt(const void *value, StdFormatter *f)
{
	const std_TupleU64U64_t *pair = value;
	StdDebugTuple b = std_fmt_debug_tuple(f, "", 0);

	std_DebugTuple_field(&b, &pair->f0, std_u64_Debug_fmt);
	std_DebugTuple_field(&b, &pair->f1, std_u64_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/* chunks_packs_tests_id returns a pack id of one byte repeated. */
static ChunksPackPackId
chunks_packs_tests_id(uint8 byte)
{
	ChunksPackPackId id;

	memset(id.f0, byte, sizeof(id.f0));
	return id;
}

/* chunks_packs_tests_listed_at returns a pack as a table lists it, with no deletion vector. */
static ChunksPacksListed
chunks_packs_tests_listed_at(uint32 space, uint8 byte)
{
	ChunksPacksListed listed = {.space = space, .id = chunks_packs_tests_id(byte)};

	return listed;
}

/* chunks_packs_tests_generation returns a deletion vector's generation, or none for 0. */
static bool
chunks_packs_tests_generation(uint64 number, uint64 *result)
{
	*result = number;
	return number != 0;
}

/* chunks_packs_tests_generation_of returns a deletion vector's generation, which unwraps at a line and column. */
static uint64
chunks_packs_tests_generation_of(uint64 number, uint32 line, uint32 col)
{
	uint64		generation;

	if (!chunks_packs_tests_generation(number, &generation))
		STD_OPTION_UNWRAP_FAILED(line, col);
	return generation;
}

/* chunks_packs_tests_tail returns a tail. */
static ChunksPacksTail
chunks_packs_tests_tail(uint64 end, uint64 generation)
{
	ChunksPacksTail tail = {end, generation};

	return tail;
}

/* chunks_packs_tests_place returns a copy's place in a pack. */
static ChunksTrailerLocator
chunks_packs_tests_place(uint64 pack, uint64 offset)
{
	return chunks_trailer_Locator_at(pack, offset, 1);
}

/* chunks_packs_tests_entry returns a pack's table entry. */
static ChunksPacksTableEntry
chunks_packs_tests_entry(uint64 number, uint32 space, uint8 byte)
{
	ChunksPacksTableEntry entry = {number, space, chunks_packs_tests_id(byte)};

	return entry;
}

/* chunks_packs_tests_table returns a table of a generation, its highest number added, and its bound. */
static ChunksPacksTable
chunks_packs_tests_table(uint64 generation, uint64 added, uint64 reserved)
{
	ChunksPacksTable table;

	chunks_packs_Table_Default_default(&table);
	table.generation = generation;
	table.added = added;
	table.reserved = reserved;
	return table;
}

/* chunks_packs_tests_listing returns a table that lists packs, the highest number added being the last's. */
static ChunksPacksTable
chunks_packs_tests_listing(const uint64 *packs, size_t packs_len)
{
	ChunksPacksTable table = chunks_packs_tests_table(0, packs_len == 0 ? 0 : packs[packs_len - 1], 0);

	for (size_t k = 0; k < packs_len; k++)
	{
		ChunksPacksListed listed = chunks_packs_tests_listed_at(0, (uint8) packs[k]);

		chunks_packs_table_packs_insert(&table.packs, packs[k], &listed);
	}
	return table;
}

/* ChunksPacksTestsMoves is a run's moves, from one place to another. */
typedef struct ChunksPacksTestsMoves
{
	const chunks_TupleLocatorLocator_t *moves;
	size_t		len;
} ChunksPacksTestsMoves;

/*
 * chunks_packs_tests_forwards_of writes each set of moves as a run of
 * forwards in a directory and returns the runs' index, oldest first.
 */
static ChunksIndexAddressIndexLocator
chunks_packs_tests_forwards_of(HostIoIo *io, const char *dir, size_t dir_len, const ChunksPacksTestsMoves *runs,
							   size_t runs_len)
{
	StdVec		written;

	std_vec_new(&written, sizeof(ChunksIndexRunLocator), _Alignof(ChunksIndexRunLocator));
	for (size_t k = 0; k < runs_len; k++)
	{
		StdString	name;
		StdPathBuf	path;
		StdVec		entries;
		ChunksIndexRunLocator run;
		ChunksErrorError error;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(".fwd")};
		StdFmtArg	args[] = {{&k, std_usize_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&name, &arguments);
		std_path_join(dir, dir_len, name.ptr, name.len, &path);
		std_vec_free_Checked(&name, 1, 1);
		chunks_packs_forward_entries(runs[k].moves, runs[k].len, &entries);
		if (!chunks_index_RunLocator_write(HOST_IO_IO_IO_DOORS(io), path.bytes.ptr, path.bytes.len, entries.ptr,
										   entries.len, &run, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 492, 102);
		std_vec_free_Checked(&entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
		std_vec_free_Checked(&path.bytes, 1, 1);
		std_vec_push_Checked(&written, &run, sizeof(run), _Alignof(ChunksIndexRunLocator));
	}
	return chunks_index_AddressIndexLocator_new(&written);
}

/* chunks_packs_tests_packs_at returns a table's listing of a pack, which panics as a BTreeMap's index of a missing key does. */
static const ChunksPacksListed *
chunks_packs_tests_packs_at(const ChunksPacksTable *table, uint64 number)
{
	static const char missing[] = "no entry found for key";
	const ChunksPacksListed *listed = chunks_packs_table_packs_get(&table->packs, number);

	if (listed == NULL)
		std_option_expect_failed(missing, sizeof(missing) - 1, STD_BTREE_DIR "map.rs", 2653, 23);
	return listed;
}

/* chunks_packs_tests_applied applies a record to a table, which unwraps at a line and column. */
static void
chunks_packs_tests_applied(ChunksPacksTable *table, const StdVec *record, uint32 line, uint32 col)
{
	ChunksErrorError error;

	if (!chunks_packs_Table_apply(table, record->ptr, record->len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
}

/* chunks_packs_tests_applied_freed applies a record, which it frees, to a table, which unwraps at a line and column. */
static void
chunks_packs_tests_applied_freed(ChunksPacksTable *table, StdVec *record, uint32 line, uint32 col)
{
	chunks_packs_tests_applied(table, record, line, col);
	std_vec_free_Checked(record, 1, 1);
}

/* chunks_packs_tests_table_eq asserts at a line that two tables are equal. */
static void
chunks_packs_tests_table_eq(const ChunksPacksTable *left, const ChunksPacksTable *right, uint32 line)
{
	STD_ASSERT_EQ(line, 9, chunks_packs_Table_PartialEq_eq(left, right), left, chunks_packs_Table_Debug_fmt, right,
				  chunks_packs_Table_Debug_fmt);
}

/* chunks_packs_tests_whole_read returns a default table with a table's whole record applied, which unwraps at a line. */
static ChunksPacksTable
chunks_packs_tests_whole_read(const ChunksPacksTable *table, ChunksPacksTable *read, uint32 line)
{
	StdVec		whole;

	chunks_packs_Table_whole(table, &whole);
	chunks_packs_tests_applied_freed(read, &whole, line, 36);
	return *read;
}

/* chunks_packs_tests_refused reports whether applying a record to a table is refused as corrupt. */
static bool
chunks_packs_tests_refused(ChunksPacksTable *table, const uint8 *record, size_t record_len)
{
	ChunksErrorError error;
	bool		corrupt;

	if (chunks_packs_Table_apply(table, record, record_len, &error))
		return false;
	corrupt = error.tag == CHUNKS_ERROR_ERROR_CORRUPT;
	chunks_error_Error_drop(&error);
	return corrupt;
}

/* chunks_packs_tests_refused_at asserts at a line and column that a table refuses a record as corrupt, with the record's Debug as the message. */
static void
chunks_packs_tests_refused_at(ChunksPacksTable *table, const uint8 *record, size_t record_len, uint32 line, uint32 col)
{
	if (!chunks_packs_tests_refused(table, record, record_len))
	{
		StdBytes	bytes = {record, record_len};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&bytes, std_bytes_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, line, col, &arguments);
	}
}

/* chunks_packs_tests_tempdir makes a test's temporary directory, which unwraps at a line. */
static SimcTempDir
chunks_packs_tests_tempdir(uint32 line)
{
	SimcTempDir dir;
	StdIoError	error;

	if (!simc_tempdir(&dir, &error))
		STD_RESULT_EXPECT_FAILED(line, 34, CHUNKS_PACKS_FIXTURES_UNWRAP, &error, std_io_Error_Debug_fmt);
	return dir;
}

/* chunks_packs_tests_appended appends a record, which it frees, through a tail in place, which unwraps at a line and column. */
static void
chunks_packs_tests_appended_at(HostIoIo *io, ChunksFileTornTail *torn, StdVec *record, uint32 line, uint32 col)
{
	chunks_packs_tests_appended(RUST_FILE, io, torn, record, line, col);
}

/*
 * test_chunks_packs_tests_a_table_read_in_steps_from_an_offset_is_the_table_read_whole:
 * a pack table read whole from its file is the table a reader has after
 * reading the file's records in two steps, from where it last read to a tail
 * and from there to the end.
 */
void
test_chunks_packs_tests_a_table_read_in_steps_from_an_offset_is_the_table_read_whole(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcTempDir dir = chunks_packs_tests_tempdir(502);
	size_t		dir_len;
	const char *dir_path = simc_TempDir_path(&dir, &dir_len);
	StdPathBuf	path;
	ChunksPacksTable first = chunks_packs_tests_table(7, 1, 64);
	ChunksPacksListed listed = chunks_packs_tests_listed_at(0, 1);
	ChunksPacksTableEntry two = chunks_packs_tests_entry(2, 5, 2);
	ChunksFileTornTail torn;
	ChunksErrorError error;
	StdVec		record;
	uint64		from;
	uint64		to;
	uint64		retired = 1;
	bool		placing = false;
	bool		has;
	ChunksFileAppended appended;
	ChunksPacksTable whole;
	ChunksPacksTable expected = chunks_packs_tests_table(7, 2, 128);
	ChunksPacksTable stepped;
	uint64		steps[2][2];

	std_path_join(dir_path, dir_len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &path);
	chunks_packs_table_packs_insert(&first.packs, 1, &listed);
	chunks_packs_table_runs_insert(&first.runs, 0, chunks_packs_tests_tail(40, 3));
	chunks_packs_Table_whole(&first, &record);
	chunks_packs_Table_drop(&first);
	if (!chunks_file_write_appended(HOST_IO_IO_IO_DOORS(io), path.bytes.ptr, path.bytes.len, record.ptr, record.len,
									&placing, &torn, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 511, 75);
	std_vec_free_Checked(&record, 1, 1);
	chunks_packs_Table_added(&two, chunks_packs_tests_tail(90, 4), &record);
	chunks_packs_tests_appended_at(io, &torn, &record, 512, 84);
	from = chunks_file_TornTail_end(&torn);
	chunks_packs_Table_retired(&retired, 1, &record);
	chunks_packs_tests_appended_at(io, &torn, &record, 514, 63);
	chunks_packs_Table_reserved(128, &record);
	chunks_packs_tests_appended_at(io, &torn, &record, 515, 62);
	chunks_packs_Table_moved(0, chunks_packs_tests_tail(64, 9), &record);
	chunks_packs_tests_appended_at(io, &torn, &record, 516, 68);

	chunks_packs_Table_Default_default(&whole);
	if (!chunks_file_read_appended(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len, &has, &appended, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 519, 48);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(519, 57);
	for (size_t k = 0; k < appended.records.len; k++)
		chunks_packs_tests_applied(&whole, &STD_VEC_AT(&appended.records, StdVec, k), 520, 34);
	chunks_file_Appended_drop(&appended);
	listed = chunks_packs_tests_listed_at(5, 2);
	chunks_packs_table_packs_insert(&expected.packs, 2, &listed);
	chunks_packs_table_runs_insert(&expected.runs, 0, chunks_packs_tests_tail(64, 9));
	chunks_packs_table_runs_insert(&expected.runs, 5, chunks_packs_tests_tail(90, 4));
	chunks_packs_tests_table_eq(&whole, &expected, 529);

	chunks_packs_Table_Default_default(&stepped);
	to = chunks_file_TornTail_end(&torn);
	steps[0][0] = sizeof(CHUNKS_FILE_APPENDED);
	steps[0][1] = from;
	steps[1][0] = from;
	steps[1][1] = to;
	for (size_t s = 0; s < lengthof(steps); s++)
	{
		StdVec		records;

		if (!chunks_file_appended_between(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len, steps[s][0],
										  steps[s][1], &has, &records, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 534, 65);
		if (!has)
			STD_OPTION_UNWRAP_FAILED(534, 74);
		for (size_t k = 0; k < records.len; k++)
			chunks_packs_tests_applied(&stepped, &STD_VEC_AT(&records, StdVec, k), 535, 40);
		chunks_pack_VecVecU8_drop(&records);
	}
	chunks_packs_tests_table_eq(&stepped, &whole, 538);
	chunks_packs_Table_drop(&stepped);
	chunks_packs_Table_drop(&expected);
	chunks_packs_Table_drop(&whole);
	chunks_file_TornTail_drop(&torn);
	std_vec_free_Checked(&path.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_tests_a_whole_record_of_a_table_keeps_every_runs_tail: a
 * whole write of a pack table keeps every tablespace's runs tail and the
 * bound of the writers' numbers, and a table's whole record gives the table
 * back.
 */
void
test_chunks_packs_tests_a_whole_record_of_a_table_keeps_every_runs_tail(void)
{
	ChunksPacksTable table = chunks_packs_tests_table(11, 9, 73);
	ChunksPacksTable read = chunks_packs_tests_table(1, 99, 5);
	ChunksPacksListed listed;

	listed = chunks_packs_tests_listed_at(0, 3);
	chunks_packs_table_packs_insert(&table.packs, 3, &listed);
	listed = chunks_packs_tests_listed_at(7, 9);
	chunks_packs_table_packs_insert(&table.packs, 9, &listed);
	chunks_packs_table_runs_insert(&table.runs, 0, chunks_packs_tests_tail(100, 1));
	chunks_packs_table_runs_insert(&table.runs, 7, chunks_packs_tests_tail(200, 2));
	chunks_packs_table_runs_insert(&table.runs, 8, chunks_packs_tests_tail(300, 3));
	listed = chunks_packs_tests_listed_at(1, 1);
	chunks_packs_table_packs_insert(&read.packs, 1, &listed);
	chunks_packs_table_runs_insert(&read.runs, 4, chunks_packs_tests_tail(1, 1));
	read = chunks_packs_tests_whole_read(&table, &read, 559);
	chunks_packs_tests_table_eq(&read, &table, 560);
	chunks_packs_Table_drop(&read);
	chunks_packs_Table_drop(&table);
}

/* chunks_packs_tests_dead_eq asserts at a line that two pairs of packs' deletion vectors' generations are equal. */
static void
chunks_packs_tests_dead_eq(const ChunksPacksListed *a, const ChunksPacksListed *b, bool has_left, uint64 left,
						   bool has_right, uint64 right, uint32 line)
{
	if (a->has_dead != has_left || (has_left && a->dead != left) || b->has_dead != has_right ||
		(has_right && b->dead != right))
	{
		StdOptionRef got[2] = {{a->has_dead, STD_ITEM(&a->dead, std_NonZeroU64_Debug_fmt)},
		{b->has_dead, STD_ITEM(&b->dead, std_NonZeroU64_Debug_fmt)}};
		StdOptionRef want[2] = {{has_left, STD_ITEM(&left, std_NonZeroU64_Debug_fmt)},
		{has_right, STD_ITEM(&right, std_NonZeroU64_Debug_fmt)}};

		std_assert_failed(RUST_FILE, line, 9, "==", got, chunks_packs_tests_Options_Debug_fmt, want,
						  chunks_packs_tests_Options_Debug_fmt, NULL);
	}
}

/* chunks_packs_tests_dead_none asserts at a line that a pack has no deletion vector. */
static void
chunks_packs_tests_dead_none(const ChunksPacksListed *listed, uint32 line)
{
	if (listed->has_dead)
	{
		StdOptionRef got = {true, STD_ITEM(&listed->dead, std_NonZeroU64_Debug_fmt)};
		StdOptionRef want = {false, STD_ITEM(&listed->dead, std_NonZeroU64_Debug_fmt)};

		std_assert_failed(RUST_FILE, line, 9, "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt, NULL);
	}
}

/*
 * test_chunks_packs_tests_a_tables_records_name_each_packs_deletion_vector: a
 * pack table's record names the generation of a listed pack's deletion
 * vector, a later one takes its place, a whole record keeps each pack's, a
 * pack added has none, and a pack retired takes its own with it.
 */
void
test_chunks_packs_tests_a_tables_records_name_each_packs_deletion_vector(void)
{
	ChunksPacksTable table;
	ChunksPacksTable read;
	ChunksPacksTableEntry entry;
	ChunksPacksListed want;
	StdVec		record;
	uint64		retired = 1;
	uint8		bad[13] = {CHUNKS_PACKS_VECTOR, 2};

	chunks_packs_Table_Default_default(&table);
	entry = chunks_packs_tests_entry(1, 0, 1);
	chunks_packs_Table_added(&entry, chunks_packs_tests_tail(8, 1), &record);
	chunks_packs_tests_applied_freed(&table, &record, 568, 64);
	entry = chunks_packs_tests_entry(2, 0, 2);
	chunks_packs_Table_added(&entry, chunks_packs_tests_tail(9, 1), &record);
	chunks_packs_tests_applied_freed(&table, &record, 569, 64);
	chunks_packs_tests_dead_none(chunks_packs_tests_packs_at(&table, 1), 570);
	chunks_packs_Table_vector(1, chunks_packs_tests_generation_of(4, 571, 53), &record);
	chunks_packs_tests_applied_freed(&table, &record, 571, 64);
	want.has_dead = chunks_packs_tests_generation(4, &want.dead);
	chunks_packs_tests_dead_eq(chunks_packs_tests_packs_at(&table, 1), chunks_packs_tests_packs_at(&table, 2),
							   want.has_dead, want.dead, false, 0, 572);
	chunks_packs_Table_vector(1, chunks_packs_tests_generation_of(6, 573, 53), &record);
	chunks_packs_tests_applied_freed(&table, &record, 573, 64);
	want = chunks_packs_tests_listed_at(0, 1);
	want.has_dead = chunks_packs_tests_generation(6, &want.dead);
	STD_ASSERT_EQ(574, 9, chunks_packs_Listed_PartialEq_eq(chunks_packs_tests_packs_at(&table, 1), &want),
				  chunks_packs_tests_packs_at(&table, 1), chunks_packs_Listed_Debug_fmt, &want,
				  chunks_packs_Listed_Debug_fmt);

	chunks_packs_Table_Default_default(&read);
	read = chunks_packs_tests_whole_read(&table, &read, 577);
	chunks_packs_tests_table_eq(&read, &table, 578);
	chunks_packs_Table_drop(&read);

	chunks_packs_Table_retired(&retired, 1, &record);
	chunks_packs_tests_applied_freed(&table, &record, 580, 44);
	entry = chunks_packs_tests_entry(3, 0, 3);
	chunks_packs_Table_added(&entry, chunks_packs_tests_tail(10, 1), &record);
	chunks_packs_tests_applied_freed(&table, &record, 581, 65);
	{
		bool		none = true;
		StdVec		deads;

		std_vec_new(&deads, sizeof(StdOptionRef), _Alignof(StdOptionRef));
		for (size_t k = 0; k < table.packs.len; k++)
		{
			const ChunksPacksListed *listed = &STD_VEC_AT(&table.packs, chunks_TupleU64Listed_t, k).f1;
			StdOptionRef dead = {listed->has_dead, STD_ITEM(&listed->dead, std_NonZeroU64_Debug_fmt)};

			none = none && !listed->has_dead;
			std_vec_push_Checked(&deads, &dead, sizeof(dead), _Alignof(StdOptionRef));
		}
		if (!none || deads.len != 2)
		{
			StdOptionRef nones[2] = {{false}, {false}};
			StdVec		wanted = {nones, 2, 2};

			std_assert_failed(RUST_FILE, 582, 9, "==", &deads, chunks_packs_tests_OptionVec_Debug_fmt, &wanted,
							  chunks_packs_tests_OptionVec_Debug_fmt, NULL);
		}
		std_vec_free_Checked(&deads, sizeof(StdOptionRef), _Alignof(StdOptionRef));
	}
	chunks_packs_Table_vector(1, chunks_packs_tests_generation_of(7, 583, 56), &record);
	chunks_packs_tests_refused_at(&table, record.ptr, record.len, 585, 13);
	std_vec_free_Checked(&record, 1, 1);
	chunks_packs_tests_refused_at(&table, bad, sizeof(bad), 585, 13);
	chunks_packs_tests_dead_none(chunks_packs_tests_packs_at(&table, 2), 587);
	chunks_packs_Table_drop(&table);
}

/*
 * test_chunks_packs_tests_a_table_record_no_writer_makes_is_refused: a record
 * of a pack table that no writer makes is refused as corrupt, and changes
 * nothing after it.
 */
void
test_chunks_packs_tests_a_table_record_no_writer_makes_is_refused(void)
{
	ChunksPacksTable table;
	ChunksPacksTable empty;
	ChunksPacksTableEntry entry = chunks_packs_tests_entry(1, 0, 1);
	StdVec		added;
	StdVec		moved;
	StdVec		shorter;
	static const uint8 nine[] = {9};
	static const uint8 removed[] = {CHUNKS_PACKS_REMOVED, 1};
	static const uint8 vector[] = {CHUNKS_PACKS_VECTOR, 1};

	chunks_packs_Table_Default_default(&table);
	chunks_packs_Table_added(&entry, chunks_packs_tests_tail(8, 1), &added);
	chunks_packs_Table_moved(1, chunks_packs_tests_tail(1, 1), &moved);
	std_range_to_check(added.len - 1, added.len, RUST_FILE, 598, 14);
	std_range_to_check(5, moved.len, RUST_FILE, 599, 14);
	chunks_packs_tests_refused_at(&table, NULL, 0, 603, 13);
	chunks_packs_tests_refused_at(&table, nine, sizeof(nine), 603, 13);
	chunks_packs_tests_refused_at(&table, added.ptr, added.len - 1, 603, 13);
	chunks_packs_tests_refused_at(&table, moved.ptr, 5, 603, 13);
	chunks_packs_tests_refused_at(&table, removed, sizeof(removed), 603, 13);
	chunks_packs_tests_refused_at(&table, vector, sizeof(vector), 603, 13);
	std_vec_free_Checked(&moved, 1, 1);
	std_vec_free_Checked(&added, 1, 1);
	chunks_packs_Table_Default_default(&empty);
	chunks_packs_Table_whole(&empty, &shorter);
	shorter.len--;
	STD_ASSERT(607, 9, chunks_packs_tests_refused(&table, shorter.ptr, shorter.len),
			   "matches!(table.apply(&short), Err(Error::Corrupt(_)))");
	std_vec_free_Checked(&shorter, 1, 1);
	chunks_packs_tests_table_eq(&table, &empty, 608);
	chunks_packs_Table_drop(&empty);
	chunks_packs_Table_drop(&table);
}

/* chunks_packs_tests_Lines_Debug_fmt is a Vec<String>'s Debug, or a [&str]'s, which reads alike. */
static bool
chunks_packs_tests_Lines_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *lines = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = lines->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(lines, StdString, k), std_String_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_tests_lines_eq asserts at a line that runs' lines are the lines given. */
static void
chunks_packs_tests_lines_eq(const StdVec *lines, const char *const *want, size_t want_len, uint32 line)
{
	bool		same = lines->len == want_len;
	StdVec		wanted;

	for (size_t k = 0; same && k < want_len; k++)
	{
		const StdString *got = &STD_VEC_AT(lines, StdString, k);

		same = got->len == strlen(want[k]) && memcmp(got->ptr, want[k], got->len) == 0;
	}
	if (same)
		return;
	std_vec_new(&wanted, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0; k < want_len; k++)
	{
		StdString text;

		std_string_from_Checked(&text, want[k], strlen(want[k]));
		std_vec_push_Checked(&wanted, &text, sizeof(text), _Alignof(StdString));
	}
	std_assert_failed(RUST_FILE, line, 9, "==", lines, chunks_packs_tests_Lines_Debug_fmt, &wanted,
					  chunks_packs_tests_Lines_Debug_fmt, NULL);
}

/* chunks_packs_tests_runs_applied applies a record, which it frees, to runs, which unwraps at a line and column. */
static void
chunks_packs_tests_runs_applied(ChunksPacksRuns *runs, StdVec *record, uint32 line, uint32 col)
{
	ChunksErrorError error;

	if (!chunks_packs_Runs_apply(runs, record->ptr, record->len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	std_vec_free_Checked(record, 1, 1);
}

/* chunks_packs_tests_runs_refused reports whether runs refuse a record as corrupt. */
static bool
chunks_packs_tests_runs_refused(ChunksPacksRuns *runs, const uint8 *record, size_t record_len)
{
	ChunksErrorError error;
	bool		corrupt;

	if (chunks_packs_Runs_apply(runs, record, record_len, &error))
		return false;
	corrupt = error.tag == CHUNKS_ERROR_ERROR_CORRUPT;
	chunks_error_Error_drop(&error);
	return corrupt;
}

/* chunks_packs_tests_strings returns Strings of literal lines. */
static StdVec
chunks_packs_tests_strings(const char *const *lines, size_t lines_len)
{
	StdVec		strings;

	std_vec_new(&strings, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0; k < lines_len; k++)
	{
		StdString text;

		std_string_from_Checked(&text, lines[k], strlen(lines[k]));
		std_vec_push_Checked(&strings, &text, sizeof(text), _Alignof(StdString));
	}
	return strings;
}

/*
 * test_chunks_packs_tests_a_runs_files_records_add_replace_and_remove_runs: a
 * runs file's records add a run once, put a merged run where the first run
 * it merged was, and remove runs.
 */
void
test_chunks_packs_tests_a_runs_files_records_add_replace_and_remove_runs(void)
{
	static const char *const two[] = {"a.idx:1", "b.idx:2"};
	static const char *const merged[] = {"b.idx", "c.idx"};
	static const char *const first[] = {"a.idx"};
	static const char *const after[] = {"a.idx:1", "d.idx:2,3"};
	static const char *const last[] = {"d.idx:2,3"};
	static const uint8 replaced[] = {CHUNKS_PACKS_REPLACED, 'x'};
	static const uint8 moved[] = {CHUNKS_PACKS_MOVED};
	ChunksPacksRuns runs;
	ChunksPacksRuns whole = {5, chunks_packs_tests_strings(two, lengthof(two))};
	StdVec		names;
	StdVec		record;

	chunks_packs_Runs_Default_default(&runs);
	chunks_packs_Runs_whole(&whole, &record);
	chunks_packs_tests_runs_applied(&runs, &record, 616, 36);
	STD_ASSERT_EQ(617, 9, chunks_packs_Runs_PartialEq_eq(&runs, &whole), &runs, chunks_packs_Runs_Debug_fmt, &whole,
				  chunks_packs_Runs_Debug_fmt);
	chunks_packs_Runs_added("c.idx:3", 7, &record);
	chunks_packs_tests_runs_applied(&runs, &record, 618, 45);
	chunks_packs_Runs_added("c.idx:3", 7, &record);
	chunks_packs_tests_runs_applied(&runs, &record, 619, 45);
	names = chunks_packs_tests_strings(merged, lengthof(merged));
	chunks_packs_Runs_replaced(names.ptr, names.len, "d.idx:2,3", 9, &record);
	std_vec_drop_Checked(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	chunks_packs_tests_runs_applied(&runs, &record, 620, 93);
	chunks_packs_tests_lines_eq(&runs.lines, after, lengthof(after), 621);
	names = chunks_packs_tests_strings(first, lengthof(first));
	chunks_packs_Runs_removed(names.ptr, names.len, &record);
	std_vec_drop_Checked(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	chunks_packs_tests_runs_applied(&runs, &record, 622, 59);
	chunks_packs_tests_lines_eq(&runs.lines, last, lengthof(last), 623);
	STD_ASSERT(624, 9, chunks_packs_tests_runs_refused(&runs, replaced, sizeof(replaced)),
			   "matches!(runs.apply(&[REPLACED, b'x']), Err(Error::Corrupt(_)))");
	STD_ASSERT(625, 9, chunks_packs_tests_runs_refused(&runs, moved, sizeof(moved)),
			   "matches!(runs.apply(&[MOVED]), Err(Error::Corrupt(_)))");
	chunks_packs_Runs_drop(&whole);
	chunks_packs_Runs_drop(&runs);
}

/* chunks_packs_tests_Locators_Debug_fmt is a Vec<Locator>'s Debug, or a [Locator]'s, which reads alike. */
static bool
chunks_packs_tests_Locators_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *locators = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = locators->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(locators, ChunksTrailerLocator, k), chunks_trailer_Locator_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_tests_copies_eq asserts at a line that entries lead to the places given, in order. */
static void
chunks_packs_tests_copies_eq(const StdVec *entries, const ChunksTrailerLocator *want, size_t want_len, uint32 line)
{
	StdVec		got;
	StdVec		wanted;
	bool		same = entries->len == want_len;

	std_vec_new(&got, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	for (size_t k = 0; k < entries->len; k++)
	{
		const ChunksTrailerLocator *to = &STD_VEC_AT(entries, chunks_TupleAddressLocator_t, k).f1;

		same = same && chunks_trailer_Locator_PartialEq_eq(to, &want[k]);
		std_vec_push_Checked(&got, to, sizeof(*to), _Alignof(ChunksTrailerLocator));
	}
	if (!same)
	{
		std_slice_to_vec_Checked(&wanted, want, want_len, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
		std_assert_failed(RUST_FILE, line, 9, "==", &got, chunks_packs_tests_Locators_Debug_fmt, &wanted,
						  chunks_packs_tests_Locators_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&got, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
}

/*
 * test_chunks_packs_tests_a_runs_forwards_are_in_the_order_of_their_places: a
 * run's forwards are in the order of their places, by pack and then by
 * offset, whatever order they were moved in.
 */
void
test_chunks_packs_tests_a_runs_forwards_are_in_the_order_of_their_places(void)
{
	chunks_TupleLocatorLocator_t moves[] = {
		{chunks_packs_tests_place(2, 0), chunks_packs_tests_place(7, 3)},
		{chunks_packs_tests_place(1, 256), chunks_packs_tests_place(7, 2)},
		{chunks_packs_tests_place(1, 1), chunks_packs_tests_place(7, 1)},
	};
	ChunksTrailerLocator want[] = {chunks_packs_tests_place(7, 1), chunks_packs_tests_place(7, 2),
	chunks_packs_tests_place(7, 3)};
	StdVec		entries;
	bool		ordered = true;

	chunks_packs_forward_entries(moves, lengthof(moves), &entries);
	chunks_packs_tests_copies_eq(&entries, want, lengthof(want), 634);
	for (size_t k = 0; k + 1 < entries.len; k++)
		ordered = ordered && memcmp(STD_VEC_AT(&entries, chunks_TupleAddressLocator_t, k).f0.f0,
									STD_VEC_AT(&entries, chunks_TupleAddressLocator_t, k + 1).f0.f0,
									sizeof(ChunksAddressAddress)) < 0;
	STD_ASSERT(635, 9, ordered, "entries.windows(2).all(|pair| pair[0].0 < pair[1].0)");
	std_vec_free_Checked(&entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
}

/* chunks_packs_tests_follows asserts at a line that a place is followed through forwards to another, which unwraps at a column. */
static void
chunks_packs_tests_follows(HostIoIo *io, const ChunksIndexAddressIndexLocator *forwards, const ChunksPacksTable *table,
						   ChunksTrailerLocator copy, ChunksTrailerLocator want, uint32 line, uint32 col)
{
	ChunksTrailerLocator followed;
	ChunksErrorError error;

	if (!chunks_packs_followed(HOST_IO_IO_READ_DOORS(io), forwards, table, copy, &followed, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	STD_ASSERT_EQ(line, 9, chunks_trailer_Locator_PartialEq_eq(&followed, &want), &followed,
				  chunks_trailer_Locator_Debug_fmt, &want, chunks_trailer_Locator_Debug_fmt);
}

/*
 * test_chunks_packs_tests_a_forward_is_found_through_one_run_and_the_newest_of_two_wins:
 * a forward is found through the one run that holds it, and when two runs
 * hold a place's forward, the newer run's is followed.
 */
void
test_chunks_packs_tests_a_forward_is_found_through_one_run_and_the_newest_of_two_wins(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcTempDir dir = chunks_packs_tests_tempdir(644);
	size_t		dir_len;
	const char *dir_path = simc_TempDir_path(&dir, &dir_len);
	static const uint64 numbers[] = {5, 9};
	ChunksPacksTable table = chunks_packs_tests_listing(numbers, lengthof(numbers));
	chunks_TupleLocatorLocator_t old[] = {
		{chunks_packs_tests_place(1, 10), chunks_packs_tests_place(5, 100)},
		{chunks_packs_tests_place(1, 20), chunks_packs_tests_place(5, 200)},
	};
	chunks_TupleLocatorLocator_t newer[] = {{chunks_packs_tests_place(1, 10), chunks_packs_tests_place(9, 50)}};
	ChunksPacksTestsMoves runs[] = {{old, lengthof(old)}, {newer, lengthof(newer)}};
	ChunksIndexAddressIndexLocator one;
	ChunksIndexAddressIndexLocator two;

	one = chunks_packs_tests_forwards_of(io, dir_path, dir_len, runs, 1);
	chunks_packs_tests_follows(io, &one, &table, chunks_packs_tests_place(1, 10), chunks_packs_tests_place(5, 100), 648,
							   61);
	chunks_packs_tests_follows(io, &one, &table, chunks_packs_tests_place(1, 20), chunks_packs_tests_place(5, 200), 649,
							   61);
	two = chunks_packs_tests_forwards_of(io, dir_path, dir_len, runs, 2);
	chunks_packs_tests_follows(io, &two, &table, chunks_packs_tests_place(1, 10), chunks_packs_tests_place(9, 50), 651,
							   61);
	chunks_packs_tests_follows(io, &two, &table, chunks_packs_tests_place(1, 20), chunks_packs_tests_place(5, 200), 652,
							   61);
	chunks_index_AddressIndexLocator_drop(&two);
	chunks_index_AddressIndexLocator_drop(&one);
	chunks_packs_Table_drop(&table);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_tests_a_walk_follows_a_chain_to_its_end_and_stops_at_a_listed_pack:
 * a walk follows a chain of forwards to the place none leads on from, and
 * stops before that at a pack the table lists, whose forwards count for
 * nothing while it is listed. A copy no forward leads from is its own last
 * place.
 */
void
test_chunks_packs_tests_a_walk_follows_a_chain_to_its_end_and_stops_at_a_listed_pack(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcTempDir dir = chunks_packs_tests_tempdir(662);
	size_t		dir_len;
	const char *dir_path = simc_TempDir_path(&dir, &dir_len);
	chunks_TupleLocatorLocator_t first[] = {{chunks_packs_tests_place(1, 10), chunks_packs_tests_place(2, 30)}};
	chunks_TupleLocatorLocator_t second[] = {{chunks_packs_tests_place(2, 30), chunks_packs_tests_place(3, 40)}};
	ChunksPacksTestsMoves runs[] = {{first, 1}, {second, 1}};
	ChunksIndexAddressIndexLocator forwards = chunks_packs_tests_forwards_of(io, dir_path, dir_len, runs, 2);
	static const uint64 nine[] = {9};
	static const uint64 two_nine[] = {2, 9};
	ChunksPacksTable table = chunks_packs_tests_listing(nine, lengthof(nine));
	ChunksPacksTable listed;

	chunks_packs_tests_follows(io, &forwards, &table, chunks_packs_tests_place(1, 10), chunks_packs_tests_place(3, 40),
							   667, 66);
	chunks_packs_tests_follows(io, &forwards, &table, chunks_packs_tests_place(2, 30), chunks_packs_tests_place(3, 40),
							   668, 66);
	chunks_packs_tests_follows(io, &forwards, &table, chunks_packs_tests_place(1, 11), chunks_packs_tests_place(1, 11),
							   669, 66);
	listed = chunks_packs_tests_listing(two_nine, lengthof(two_nine));
	chunks_packs_tests_follows(io, &forwards, &listed, chunks_packs_tests_place(1, 10), chunks_packs_tests_place(2, 30),
							   671, 67);
	chunks_packs_tests_follows(io, &forwards, &listed, chunks_packs_tests_place(2, 30), chunks_packs_tests_place(2, 30),
							   672, 67);
	chunks_packs_Table_drop(&listed);
	chunks_packs_Table_drop(&table);
	chunks_index_AddressIndexLocator_drop(&forwards);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_tests_a_forward_to_a_pack_that_is_not_later_is_refused_where_it_is_read:
 * a forward that leads to a lower pack, to its own pack, or to a pack
 * numbered higher than any the table added is refused as damage of the
 * run's block it was read from, with both places in the message.
 */
void
test_chunks_packs_tests_a_forward_to_a_pack_that_is_not_later_is_refused_where_it_is_read(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcTempDir dir = chunks_packs_tests_tempdir(681);
	size_t		dir_len;
	const char *dir_path = simc_TempDir_path(&dir, &dir_len);
	chunks_TupleLocatorLocator_t lead[] = {{chunks_packs_tests_place(1, 10), chunks_packs_tests_place(3, 40)}};
	chunks_TupleLocatorLocator_t moves[] = {
		{chunks_packs_tests_place(3, 40), chunks_packs_tests_place(2, 5)},
		{chunks_packs_tests_place(3, 41), chunks_packs_tests_place(3, 6)},
		{chunks_packs_tests_place(3, 42), chunks_packs_tests_place(8, 7)},
	};
	ChunksPacksTestsMoves runs[] = {{lead, 1}, {moves, lengthof(moves)}};
	ChunksIndexAddressIndexLocator forwards = chunks_packs_tests_forwards_of(io, dir_path, dir_len, runs, 2);
	static const uint64 seven[] = {7};
	ChunksPacksTable table = chunks_packs_tests_listing(seven, lengthof(seven));
	ChunksTrailerLocator copies[] = {chunks_packs_tests_place(1, 10), chunks_packs_tests_place(3, 41),
	chunks_packs_tests_place(3, 42)};
	static const uint64 froms[] = {40, 41, 42};
	static const uint64 tos[] = {2, 3, 8};
	StdPathBuf	run_path;

	std_path_join(dir_path, dir_len, "1.fwd", 5, &run_path);
	for (size_t k = 0; k < lengthof(copies); k++)
	{
		ChunksTrailerLocator followed;
		ChunksErrorError refused;
		StdString	names;
		StdFmtPiece pieces[] = {STD_FMT_LIT("a forward from offset "), STD_FMT_ARG(0), STD_FMT_LIT(" of pack 3 to pack "),
		STD_FMT_ARG(1), STD_FMT_LIT(",")};
		StdFmtArg	args[] = {{&froms[k], std_u64_Display_fmt}, {&tos[k], std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		bool		damaged;

		if (chunks_packs_followed(HOST_IO_IO_READ_DOORS(io), &forwards, &table, copies[k], &followed, &refused))
			STD_RESULT_EXPECT_FAILED(686, 65, CHUNKS_PACKS_TESTS_UNWRAP_ERR, &followed,
									 chunks_trailer_Locator_Debug_fmt);
		std_fmt_format_Checked(&names, &arguments);
		damaged = refused.tag == CHUNKS_ERROR_ERROR_DAMAGED && refused.u.damaged.offset == 8 &&
			std_path_PartialEq_eq(refused.u.damaged.path.bytes.ptr, refused.u.damaged.path.bytes.len,
								  run_path.bytes.ptr, run_path.bytes.len) &&
			std_str_contains_str_Checked(refused.u.damaged.message.ptr, refused.u.damaged.message.len, names.ptr,
										 names.len);
		std_vec_free_Checked(&names, 1, 1);
		if (!damaged)
		{
			StdFmtPiece failed_pieces[] = {STD_FMT_ARG(0)};
			StdFmtArg	failed_args[] = {{&refused, chunks_error_Error_Debug_fmt}};
			StdFmtArguments failed = {failed_pieces, lengthof(failed_pieces), failed_args};

			std_panic_fmt(RUST_FILE, 688, 13, &failed);
		}
		chunks_error_Error_drop(&refused);
	}
	std_vec_free_Checked(&run_path.bytes, 1, 1);
	chunks_packs_Table_drop(&table);
	chunks_index_AddressIndexLocator_drop(&forwards);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_tests_kept returns the forwards kept but those from dropped places, which unwraps at a line and column. */
static StdVec
chunks_packs_tests_kept(HostIoIo *io, const ChunksIndexAddressIndexLocator *forwards,
						const std_TupleU64U64_t *dropped, size_t dropped_len, uint32 line, uint32 col)
{
	StdVec		kept;
	ChunksErrorError error;

	if (!chunks_packs_kept_forwards(HOST_IO_IO_IO_DOORS(io), forwards, dropped, dropped_len, &kept, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return kept;
}

/* chunks_packs_tests_run_written writes a run of entries in a directory by its name, which unwraps at a line and column. */
static ChunksIndexRunLocator
chunks_packs_tests_run_written(HostIoIo *io, const char *dir, size_t dir_len, const char *name, const StdVec *entries,
							   uint32 line, uint32 col)
{
	StdPathBuf	path;
	ChunksIndexRunLocator run;
	ChunksErrorError error;

	std_path_join(dir, dir_len, name, strlen(name), &path);
	if (!chunks_index_RunLocator_write(HOST_IO_IO_IO_DOORS(io), path.bytes.ptr, path.bytes.len, entries->ptr,
									   entries->len, &run, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	std_vec_free_Checked(&path.bytes, 1, 1);
	return run;
}

/*
 * test_chunks_packs_tests_a_merge_that_drops_places_keeps_every_other_forward:
 * a merge that drops places leaves one run's entries without their forwards
 * and with every other, the newest first for a place two runs held, so a
 * walk from a dropped place ends there.
 */
void
test_chunks_packs_tests_a_merge_that_drops_places_keeps_every_other_forward(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcTempDir dir = chunks_packs_tests_tempdir(702);
	size_t		dir_len;
	const char *dir_path = simc_TempDir_path(&dir, &dir_len);
	chunks_TupleLocatorLocator_t old[] = {
		{chunks_packs_tests_place(1, 10), chunks_packs_tests_place(5, 100)},
		{chunks_packs_tests_place(1, 20), chunks_packs_tests_place(5, 200)},
		{chunks_packs_tests_place(2, 30), chunks_packs_tests_place(5, 300)},
	};
	chunks_TupleLocatorLocator_t new_moves[] = {
		{chunks_packs_tests_place(1, 20), chunks_packs_tests_place(9, 50)},
		{chunks_packs_tests_place(4, 40), chunks_packs_tests_place(9, 60)},
	};
	ChunksPacksTestsMoves runs[] = {{old, lengthof(old)}, {new_moves, lengthof(new_moves)}};
	ChunksIndexAddressIndexLocator forwards = chunks_packs_tests_forwards_of(io, dir_path, dir_len, runs, 2);
	static const std_TupleU64U64_t dropped[] = {{1, 10}, {4, 40}, {6, 60}};
	ChunksTrailerLocator want[] = {chunks_packs_tests_place(9, 50), chunks_packs_tests_place(5, 200),
	chunks_packs_tests_place(5, 300)};
	StdVec		kept = chunks_packs_tests_kept(io, &forwards, dropped, lengthof(dropped), 706, 79);
	StdVec		written;
	ChunksIndexRunLocator run;
	ChunksIndexAddressIndexLocator merged;
	static const uint64 numbers[] = {5, 9};
	ChunksPacksTable table = chunks_packs_tests_listing(numbers, lengthof(numbers));

	chunks_packs_tests_copies_eq(&kept, want, lengthof(want), 707);
	run = chunks_packs_tests_run_written(io, dir_path, dir_len, "merged.fwd", &kept, 708, 99);
	std_vec_new(&written, sizeof(ChunksIndexRunLocator), _Alignof(ChunksIndexRunLocator));
	std_vec_push_Checked(&written, &run, sizeof(run), _Alignof(ChunksIndexRunLocator));
	merged = chunks_index_AddressIndexLocator_new(&written);
	chunks_packs_tests_follows(io, &merged, &table, chunks_packs_tests_place(1, 10), chunks_packs_tests_place(1, 10), 710,
							   64);
	chunks_packs_tests_follows(io, &merged, &table, chunks_packs_tests_place(4, 40), chunks_packs_tests_place(4, 40), 711,
							   64);
	chunks_packs_tests_follows(io, &merged, &table, chunks_packs_tests_place(1, 20), chunks_packs_tests_place(9, 50), 712,
							   64);
	chunks_packs_tests_follows(io, &merged, &table, chunks_packs_tests_place(2, 30), chunks_packs_tests_place(5, 300), 713,
							   64);
	chunks_packs_Table_drop(&table);
	chunks_index_AddressIndexLocator_drop(&merged);
	std_vec_free_Checked(&kept, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	chunks_index_AddressIndexLocator_drop(&forwards);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * chunks_packs_tests_forwarding_read reads the forwarding file's runs, opens
 * each run they name, and returns their index with the lines, as
 * the_forwarding_file_lists_the_runs_a_walk_reads' read does.
 */
static ChunksIndexAddressIndexLocator
chunks_packs_tests_forwarding_read(HostIoIo *io, const char *dir, size_t dir_len, const StdPathBuf *path,
								   ChunksPacksRuns *runs)
{
	ChunksFileAppended appended;
	ChunksErrorError error;
	bool		has;
	StdVec		opened;

	chunks_packs_Runs_Default_default(runs);
	if (!chunks_file_read_appended(HOST_IO_IO_READ_DOORS(io), path->bytes.ptr, path->bytes.len, &has, &appended,
								   &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 727, 52);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(727, 61);
	for (size_t k = 0; k < appended.records.len; k++)
	{
		const StdVec *record = &STD_VEC_AT(&appended.records, StdVec, k);

		if (!chunks_packs_Runs_apply(runs, record->ptr, record->len, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 728, 37);
	}
	chunks_file_Appended_drop(&appended);
	std_vec_new(&opened, sizeof(ChunksIndexRunLocator), _Alignof(ChunksIndexRunLocator));
	for (size_t k = 0; k < runs->lines.len; k++)
	{
		const StdString *line = &STD_VEC_AT(&runs->lines, StdString, k);
		StdStr		name = chunks_packs_run_name(line->ptr, line->len);
		StdPathBuf	run_path;
		ChunksIndexRunLocator run;

		std_path_join(dir, dir_len, name.ptr, name.len, &run_path);
		if (!chunks_index_RunLocator_open(HOST_IO_IO_READ_DOORS(io), run_path.bytes.ptr, run_path.bytes.len, &run,
										  &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 730, 103);
		std_vec_free_Checked(&run_path.bytes, 1, 1);
		std_vec_push_Checked(&opened, &run, sizeof(run), _Alignof(ChunksIndexRunLocator));
	}
	return chunks_index_AddressIndexLocator_new(&opened);
}

/*
 * test_chunks_packs_tests_the_forwarding_file_lists_the_runs_a_walk_reads: the
 * forwarding file lists the runs of forwards as a runs file lists an
 * index's: a reader that applies its records opens the runs they name,
 * oldest first, and follows a chain through them, and a merge's record puts
 * its run in the place of the runs it merged.
 */
void
test_chunks_packs_tests_the_forwarding_file_lists_the_runs_a_walk_reads(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcTempDir dir = chunks_packs_tests_tempdir(723);
	size_t		dir_len;
	const char *dir_path = simc_TempDir_path(&dir, &dir_len);
	StdPathBuf	path;
	static const uint64 nine[] = {9};
	ChunksPacksTable table = chunks_packs_tests_listing(nine, lengthof(nine));
	chunks_TupleLocatorLocator_t a[] = {{chunks_packs_tests_place(1, 10), chunks_packs_tests_place(2, 30)}};
	chunks_TupleLocatorLocator_t b[] = {{chunks_packs_tests_place(2, 30), chunks_packs_tests_place(3, 40)}};
	static const char *const a_line[] = {"a.fwd:1"};
	static const char *const both[] = {"a.fwd:1", "b.fwd:2"};
	static const char *const merged_names[] = {"a.fwd", "b.fwd"};
	static const char *const merged_line[] = {"c.fwd:2"};
	static const std_TupleU64U64_t dropped[] = {{1, 10}};
	ChunksPacksRuns start_runs = {1, chunks_packs_tests_strings(a_line, lengthof(a_line))};
	ChunksPacksRuns runs;
	ChunksIndexAddressIndexLocator forwards;
	ChunksIndexRunLocator run;
	ChunksFileTornTail torn;
	ChunksErrorError error;
	StdVec		entries;
	StdVec		record;
	StdVec		kept;
	StdVec		names;
	bool		placing = false;

	std_path_join(dir_path, dir_len, CHUNKS_PACKS_FORWARDING, strlen(CHUNKS_PACKS_FORWARDING), &path);
	chunks_packs_forward_entries(a, 1, &entries);
	run = chunks_packs_tests_run_written(io, dir_path, dir_len, "a.fwd", &entries, 735, 107);
	chunks_index_RunLocator_drop(&run);
	std_vec_free_Checked(&entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	chunks_packs_Runs_whole(&start_runs, &record);
	chunks_packs_Runs_drop(&start_runs);
	if (!chunks_file_write_appended(HOST_IO_IO_IO_DOORS(io), path.bytes.ptr, path.bytes.len, record.ptr, record.len,
									&placing, &torn, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 739, 36);
	std_vec_free_Checked(&record, 1, 1);
	chunks_packs_forward_entries(b, 1, &entries);
	run = chunks_packs_tests_run_written(io, dir_path, dir_len, "b.fwd", &entries, 738, 107);
	chunks_index_RunLocator_drop(&run);
	std_vec_free_Checked(&entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	chunks_packs_Runs_added("b.fwd:2", 7, &record);
	chunks_packs_tests_appended_at(io, &torn, &record, 739, 71);
	forwards = chunks_packs_tests_forwarding_read(io, dir_path, dir_len, &path, &runs);
	chunks_packs_tests_lines_eq(&runs.lines, both, lengthof(both), 741);
	chunks_packs_tests_follows(io, &forwards, &table, chunks_packs_tests_place(1, 10), chunks_packs_tests_place(3, 40),
							   742, 66);

	kept = chunks_packs_tests_kept(io, &forwards, dropped, lengthof(dropped), 744, 61);
	run = chunks_packs_tests_run_written(io, dir_path, dir_len, "c.fwd", &kept, 745, 63);
	chunks_index_RunLocator_drop(&run);
	std_vec_free_Checked(&kept, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	names = chunks_packs_tests_strings(merged_names, lengthof(merged_names));
	chunks_packs_Runs_replaced(names.ptr, names.len, "c.fwd:2", 7, &record);
	std_vec_drop_Checked(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	chunks_packs_tests_appended_at(io, &torn, &record, 746, 96);
	chunks_index_AddressIndexLocator_drop(&forwards);
	chunks_packs_Runs_drop(&runs);
	forwards = chunks_packs_tests_forwarding_read(io, dir_path, dir_len, &path, &runs);
	chunks_packs_tests_lines_eq(&runs.lines, merged_line, lengthof(merged_line), 748);
	chunks_packs_tests_follows(io, &forwards, &table, chunks_packs_tests_place(1, 10), chunks_packs_tests_place(1, 10),
							   749, 66);
	chunks_packs_tests_follows(io, &forwards, &table, chunks_packs_tests_place(2, 30), chunks_packs_tests_place(3, 40),
							   750, 66);
	chunks_index_AddressIndexLocator_drop(&forwards);
	chunks_packs_Runs_drop(&runs);
	chunks_file_TornTail_drop(&torn);
	chunks_packs_Table_drop(&table);
	std_vec_free_Checked(&path.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_tests_a_table_lists_packs_by_their_whole_numbers: a pack
 * numbered above 32 bits is added, named a vector, and retired by its whole
 * number, and a whole record keeps it: the pack whose number has the same
 * low bits is another pack.
 */
void
test_chunks_packs_tests_a_table_lists_packs_by_their_whole_numbers(void)
{
	uint64		wide = (UINT64CONST(1) << 32) + 2;
	ChunksPacksTable table;
	ChunksPacksTable read;
	ChunksPacksTableEntry entry;
	StdVec		record;
	std_TupleU64U64_t got;
	std_TupleU64U64_t want = {wide, 2};
	uint64		generation = 4;

	chunks_packs_Table_Default_default(&table);
	entry = chunks_packs_tests_entry(2, 0, 2);
	chunks_packs_Table_added(&entry, chunks_packs_tests_tail(8, 1), &record);
	chunks_packs_tests_applied_freed(&table, &record, 759, 64);
	entry = chunks_packs_tests_entry(wide, 0, 3);
	chunks_packs_Table_added(&entry, chunks_packs_tests_tail(9, 1), &record);
	chunks_packs_tests_applied_freed(&table, &record, 760, 67);
	got.f0 = table.added;
	got.f1 = table.packs.len;
	STD_ASSERT_EQ(761, 9, got.f0 == want.f0 && got.f1 == want.f1, &got, chunks_packs_tests_Pair_Debug_fmt, &want,
				  chunks_packs_tests_Pair_Debug_fmt);
	chunks_packs_Table_vector(wide, generation, &record);
	chunks_packs_tests_applied_freed(&table, &record, 763, 55);
	chunks_packs_tests_dead_eq(chunks_packs_tests_packs_at(&table, 2), chunks_packs_tests_packs_at(&table, wide), false,
							   0, true, generation, 764);
	chunks_packs_Table_Default_default(&read);
	read = chunks_packs_tests_whole_read(&table, &read, 766);
	chunks_packs_tests_table_eq(&read, &table, 767);
	chunks_packs_Table_drop(&read);
	chunks_packs_Table_retired(&wide, 1, &record);
	chunks_packs_tests_applied_freed(&table, &record, 768, 47);
	if (table.packs.len != 1 || STD_VEC_AT(&table.packs, chunks_TupleU64Listed_t, 0).f0 != 2)
	{
		StdVec		keys;
		uint64		two = 2;
		StdVec		wanted = {&two, 1, 1};

		std_vec_new(&keys, sizeof(uint64), _Alignof(uint64));
		for (size_t k = 0; k < table.packs.len; k++)
			std_vec_push_Checked(&keys, &STD_VEC_AT(&table.packs, chunks_TupleU64Listed_t, k).f0, sizeof(uint64),
								 _Alignof(uint64));
		std_assert_failed(RUST_FILE, 769, 9, "==", &keys, chunks_packs_tests_Numbers_Debug_fmt, &wanted,
						  chunks_packs_tests_Numbers_Debug_fmt, NULL);
	}
	chunks_packs_Table_drop(&table);
}

/* chunks_packs_tests_key returns a place's key. */
static ChunksAddressAddress
chunks_packs_tests_key(uint64 pack, uint64 offset)
{
	std_TupleU64U64_t place = {pack, offset};

	return chunks_packs_follow_place_key(&place);
}

/* chunks_packs_tests_keys_ne asserts at a line that two places' keys differ. */
static void
chunks_packs_tests_keys_ne(ChunksAddressAddress left, ChunksAddressAddress right, uint32 line)
{
	if (chunks_address_Address_PartialEq_eq(&left, &right))
		std_assert_failed(RUST_FILE, line, 9, "!=", &left, chunks_address_Address_Debug_fmt, &right,
						  chunks_address_Address_Debug_fmt, NULL);
}

/*
 * test_chunks_packs_tests_a_places_key_holds_its_whole_number_and_offset: two
 * places whose packs' numbers differ only above their low 32 bits have
 * different keys, as two whose offsets do, and keys sort by pack and then by
 * offset.
 */
void
test_chunks_packs_tests_a_places_key_holds_its_whole_number_and_offset(void)
{
	ChunksAddressAddress a;
	ChunksAddressAddress b;

	chunks_packs_tests_keys_ne(chunks_packs_tests_key(1, 7), chunks_packs_tests_key((UINT64CONST(1) << 32) + 1, 7),
							   776);
	chunks_packs_tests_keys_ne(chunks_packs_tests_key(1, 7), chunks_packs_tests_key(1, (UINT64CONST(1) << 32) + 7),
							   777);
	a = chunks_packs_tests_key(1, PG_UINT64_MAX);
	b = chunks_packs_tests_key(2, 0);
	STD_ASSERT(778, 9, memcmp(a.f0, b.f0, sizeof(a.f0)) < 0, "place_key(&(1, u64::MAX)) < place_key(&(2, 0))");
	a = chunks_packs_tests_key(UINT64CONST(1) << 40, 0);
	b = chunks_packs_tests_key(255, PG_UINT64_MAX);
	STD_ASSERT(779, 9, memcmp(a.f0, b.f0, sizeof(a.f0)) > 0, "place_key(&(1 << 40, 0)) > place_key(&(255, u64::MAX))");
}

#endif							/* DOLTGRES_TEST_CHUNKS */
