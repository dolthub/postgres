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
 * Stored chunks: the hashed bytes followed by a hint trailer that the
 * address does not cover, holding a locator for each address the chunk
 * refers to and, for table leaves, each row's stamp, or for internal tree
 * nodes each child's subtree's highest tuple id, with a checksum of its own.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/header.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "trailer.rs"

/* CHUNKS_TRAILER_CHECKSUM_LEN is the length of a trailer's checksum. */
#define CHUNKS_TRAILER_CHECKSUM_LEN 8

/* CHUNKS_TRAILER_HAS_LOCATORS is the trailer flag of a trailer with locators. */
#define CHUNKS_TRAILER_HAS_LOCATORS 1

/* CHUNKS_TRAILER_HAS_STAMPS is the trailer flag of a trailer with row stamps. */
#define CHUNKS_TRAILER_HAS_STAMPS 2

/*
 * CHUNKS_TRAILER_HAS_STAMP_RUNS is the trailer flag of a trailer whose
 * entries do not each have one row stamp, which then gives how many each has
 * and the stamps as runs.
 */
#define CHUNKS_TRAILER_HAS_STAMP_RUNS 4

/*
 * CHUNKS_TRAILER_HAS_MAX_TUPLES is the trailer flag of an internal tree
 * node's trailer, which gives each child's subtree's highest tuple id.
 */
#define CHUNKS_TRAILER_HAS_MAX_TUPLES 8

/*
 * CHUNKS_TRAILER_HAS_VALUES is the trailer flag of a leaf's trailer whose
 * stamps are followed by its entries' value ids, the ids of its rows' values
 * out of line.
 */
#define CHUNKS_TRAILER_HAS_VALUES 16

/* chunks_trailer_subtree_maxes_error_t is subtree_maxes' error, Option<Malformed>. */
typedef struct chunks_trailer_subtree_maxes_error_t
{
	bool		has;
	ChunksVarintMalformed x;
} chunks_trailer_subtree_maxes_error_t;

static void chunks_trailer_Stamps_keep_runs(ChunksTrailerStamps *self);
static void chunks_trailer_Stamps_append(ChunksTrailerStamps *self, ChunksTrailerStamp first, uint64 len);
static void chunks_trailer_Stamps_end_entry(ChunksTrailerStamps *self);
static bool chunks_trailer_Stamps_read_stamps(const uint8 *bytes, size_t bytes_len, bool runs,
											  ChunksTrailerStamps *result, ChunksErrorError *error);
static uint64 chunks_trailer_trailer_checksum(const uint8 *trailer, size_t trailer_len, uint32 len);

/* chunks_trailer_Locator_offset returns the offset of the chunk's record in the pack. */
uint64
chunks_trailer_Locator_offset(const ChunksTrailerLocator *self)
{
	return STD_SHR_FOLDED(u64, self->place, 24);
}

/*
 * chunks_trailer_subtree_maxes reads an internal node's subtrees' highest
 * tuple ids, varints each below 2^48, failing with why a varint is
 * malformed, or with None for one of 2^48 or more.
 */
static bool
chunks_trailer_subtree_maxes(const uint8 *bytes, size_t bytes_len, StdVec *result,
							 chunks_trailer_subtree_maxes_error_t *error)
{
	size_t		at = 0;
	volatile StdVec maxes;
	volatile bool maxes_live = false;
	bool		ok = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *maxes_v = (StdVec *) &maxes;

		std_vec_new(maxes_v, sizeof(uint64), _Alignof(uint64));
		maxes_live = true;
		ok = true;
		while (at < bytes_len)
		{
			uint64		max;
			ChunksVarintMalformed malformed;

			if (!chunks_varint_decode(bytes, bytes_len, &at, &max, &malformed))
			{
				error->has = true;
				error->x = malformed;
				ok = false;
				break;
			}
			if (max >= STD_SHL_FOLDED(u64, 1, 48))
			{
				error->has = false;
				ok = false;
				break;
			}
			std_vec_push_Checked(maxes_v, &max, sizeof(uint64), _Alignof(uint64));
		}
		maxes_live = false;
		if (ok)
			*result = *maxes_v;
		else
			std_vec_free_Checked(maxes_v, sizeof(uint64), _Alignof(uint64));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (maxes_live)
			std_vec_free_Checked((StdVec *) &maxes, sizeof(uint64), _Alignof(uint64));
		std_unwind_resume(&unwind);
	}
	return ok;
}

/*
 * chunks_trailer_Locator_at returns the locator of a record in a pack with a
 * number below 2^48, at an offset below 2^40, of a stored length below 2^24.
 */
ChunksTrailerLocator
chunks_trailer_Locator_at(uint64 pack, uint64 offset, uint32 length)
{
	ChunksTrailerLocator result = {pack, STD_SHL_FOLDED(u64, offset, 24) | (uint64) length};

	return result;
}

/* chunks_trailer_Locator_length returns the record's stored length. */
uint32
chunks_trailer_Locator_length(const ChunksTrailerLocator *self)
{
	return (uint32) (self->place & 0xffffff);
}

/* chunks_trailer_Locator_is_known reports whether the locator says where a chunk is. */
bool
chunks_trailer_Locator_is_known(const ChunksTrailerLocator *self)
{
	return chunks_trailer_Locator_length(self) != 0;
}

/*
 * chunks_trailer_Locator_write appends the locator's LOCATOR_LEN bytes: the
 * pack's number in 6, the offset in 5, and the stored length in 3, each with
 * its low byte first.
 */
void
chunks_trailer_Locator_write(const ChunksTrailerLocator *self, StdVec *out)
{
	uint8		pack[8];
	uint8		offset[8];
	uint8		length[4];

	std_u64_to_le_bytes(self->pack, pack);
	std_range_to_check(6, 8, RUST_FILE, 161, 55);
	std_vec_extend_from_slice_Checked(out, pack, 6, 1, 1);
	std_u64_to_le_bytes(chunks_trailer_Locator_offset(self), offset);
	std_range_to_check(5, 8, RUST_FILE, 162, 59);
	std_vec_extend_from_slice_Checked(out, offset, 5, 1, 1);
	std_u32_to_le_bytes(chunks_trailer_Locator_length(self), length);
	std_range_to_check(3, 4, RUST_FILE, 163, 59);
	std_vec_extend_from_slice_Checked(out, length, 3, 1, 1);
}

/* chunks_trailer_Locator_read reads a locator's LOCATOR_LEN bytes. */
ChunksTrailerLocator
chunks_trailer_Locator_read(const uint8 *bytes, size_t bytes_len)
{
	uint8		pack[8] = {0};
	uint8		offset[8] = {0};
	uint8		length[4] = {0};

	std_range_to_check(6, 8, RUST_FILE, 169, 13);
	std_range_to_check(6, bytes_len, RUST_FILE, 169, 41);
	std_slice_copy_from_slice_Checked(pack, 6, bytes, 6, 1, 1, RUST_FILE, 169, 19);
	std_range_to_check(5, 8, RUST_FILE, 170, 15);
	std_range_check(6, 11, bytes_len, RUST_FILE, 170, 43);
	std_slice_copy_from_slice_Checked(offset, 5, bytes + 6, 5, 1, 1, RUST_FILE, 170, 21);
	std_range_to_check(3, 4, RUST_FILE, 171, 15);
	std_range_check(11, CHUNKS_TRAILER_LOCATOR_LEN, bytes_len, RUST_FILE, 171, 43);
	std_slice_copy_from_slice_Checked(length, 3, bytes + 11, 3, 1, 1, RUST_FILE, 171, 21);
	return chunks_trailer_Locator_at(std_u64_from_le_bytes(pack), std_u64_from_le_bytes(offset),
									 std_u32_from_le_bytes(length));
}

/* chunks_trailer_Locator_Debug_fmt writes the locator's three numbers. */
bool
chunks_trailer_Locator_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksTrailerLocator *self = (const ChunksTrailerLocator *) value;
	StdDebugStruct locator = std_fmt_debug_struct(f, "Locator", 7);
	uint64		offset;
	uint32		length;

	std_DebugStruct_field(&locator, "pack", 4, &self->pack, std_u64_Debug_fmt);
	offset = chunks_trailer_Locator_offset(self);
	std_DebugStruct_field(&locator, "offset", 6, &offset, std_u64_Debug_fmt);
	length = chunks_trailer_Locator_length(self);
	std_DebugStruct_field(&locator, "length", 6, &length, std_u32_Debug_fmt);
	return std_DebugStruct_finish(&locator);
}

/* chunks_trailer_Stamp_write appends the stamp's 14 bytes. */
void
chunks_trailer_Stamp_write(const ChunksTrailerStamp *self, StdVec *out)
{
	uint8		txn[8];
	uint8		tuple[8];

	std_u64_to_le_bytes(self->txn, txn);
	std_vec_extend_from_slice_Checked(out, txn, 8, 1, 1);
	std_u64_to_le_bytes(self->tuple, tuple);
	std_range_to_check(6, 8, RUST_FILE, 196, 56);
	std_vec_extend_from_slice_Checked(out, tuple, 6, 1, 1);
}

/* chunks_trailer_Stamp_read reads a stamp's 14 bytes. */
ChunksTrailerStamp
chunks_trailer_Stamp_read(const uint8 *bytes, size_t bytes_len)
{
	uint8		tuple[8] = {0};
	ChunksTrailerStamp result;

	std_range_to_check(6, 8, RUST_FILE, 202, 14);
	std_range_check(8, 14, bytes_len, RUST_FILE, 202, 42);
	std_slice_copy_from_slice_Checked(tuple, 6, bytes + 8, 6, 1, 1, RUST_FILE, 202, 20);
	{
		size_t		len;

		std_range_to_check(8, bytes_len, RUST_FILE, 203, 46);
		len = 8;
		if (len != 8)
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
									 std_TryFromSliceError_Debug_fmt, RUST_FILE, 203, 63);
	}
	result.txn = std_u64_from_le_bytes(bytes);
	result.tuple = std_u64_from_le_bytes(tuple);
	return result;
}

/* chunks_trailer_Locator_Default_default is Locator's derived Default: pack and place zero, an unknown locator. */
ChunksTrailerLocator
chunks_trailer_Locator_Default_default(void)
{
	ChunksTrailerLocator locator = {0};

	return locator;
}

/* chunks_trailer_Locator_PartialEq_eq is Locator's derived PartialEq: the same pack and place. */
bool
chunks_trailer_Locator_PartialEq_eq(const ChunksTrailerLocator *self, const ChunksTrailerLocator *other)
{
	return self->pack == other->pack && self->place == other->place;
}

/* chunks_trailer_Locator_Hash_hash is Locator's derived Hash: its pack, then its place. */
void
chunks_trailer_Locator_Hash_hash(const void *self_, StdHasher state)
{
	const ChunksTrailerLocator *self = self_;

	state.ops->write_u64(state.self, self->pack);
	state.ops->write_u64(state.self, self->place);
}

/* chunks_trailer_Locator_Ord_cmp is Locator's derived Ord: by pack, then by place. */
StdOrdering
chunks_trailer_Locator_Ord_cmp(const ChunksTrailerLocator *self, const ChunksTrailerLocator *other)
{
	StdOrdering pack = std_u64_Ord_cmp(&self->pack, &other->pack);

	if (pack != STD_ORDERING_EQUAL)
		return pack;
	return std_u64_Ord_cmp(&self->place, &other->place);
}

/* chunks_trailer_Stamp_Debug_fmt is Stamp's derived Debug. */
bool
chunks_trailer_Stamp_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksTrailerStamp *self = (const ChunksTrailerStamp *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Stamp", 5);

	std_DebugStruct_field(&b, "txn", 3, &self->txn, std_u64_Debug_fmt);
	std_DebugStruct_field(&b, "tuple", 5, &self->tuple, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* chunks_trailer_Stamp_PartialEq_eq is Stamp's derived PartialEq. */
bool
chunks_trailer_Stamp_PartialEq_eq(const ChunksTrailerStamp *self, const ChunksTrailerStamp *other)
{
	return self->txn == other->txn && self->tuple == other->tuple;
}

/* chunks_trailer_VecStamp_Debug_fmt is a Vec<Stamp>'s Debug: its stamps as a list. */
bool
chunks_trailer_VecStamp_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = (const StdVec *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
		std_DebugList_entry(&b, &STD_VEC_AT(v, ChunksTrailerStamp, i), chunks_trailer_Stamp_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* chunks_trailer_VecStamp_PartialEq_eq is a Vec<Stamp>'s PartialEq: the same stamps in order. */
bool
chunks_trailer_VecStamp_PartialEq_eq(const StdVec *self, const StdVec *other)
{
	if (self->len != other->len)
		return false;
	for (size_t i = 0; i < self->len; i++)
	{
		if (!chunks_trailer_Stamp_PartialEq_eq(&STD_VEC_AT(self, ChunksTrailerStamp, i),
											   &STD_VEC_AT(other, ChunksTrailerStamp, i)))
			return false;
	}
	return true;
}

/* chunks_trailer_TupleStampU64_Debug_fmt is a (Stamp, u64)'s Debug. */
static bool
chunks_trailer_TupleStampU64_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_trailer_Stamps_runs_t *self = (const chunks_trailer_Stamps_runs_t *) value;
	StdDebugTuple b = std_fmt_debug_tuple(f, "", 0);

	std_DebugTuple_field(&b, &self->f0, chunks_trailer_Stamp_Debug_fmt);
	std_DebugTuple_field(&b, &self->f1, std_u64_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/* chunks_trailer_VecTupleStampU64_Debug_fmt is a Vec<(Stamp, u64)>'s Debug. */
static bool
chunks_trailer_VecTupleStampU64_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = (const StdVec *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
		std_DebugList_entry(&b, &STD_VEC_AT(v, chunks_trailer_Stamps_runs_t, i),
							chunks_trailer_TupleStampU64_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* chunks_trailer_VecU64_Debug_fmt is a Vec<u64>'s Debug. */
bool
chunks_trailer_VecU64_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = (const StdVec *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
		std_DebugList_entry(&b, &STD_VEC_AT(v, uint64, i), std_u64_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* chunks_trailer_TupleU64VecU32_Debug_fmt is a (u64, Vec<u32>)'s Debug. */
static bool
chunks_trailer_TupleU64VecU32_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_trailer_Stamps_values_t *self = (const chunks_trailer_Stamps_values_t *) value;
	StdDebugTuple b = std_fmt_debug_tuple(f, "", 0);

	std_DebugTuple_field(&b, &self->f0, std_u64_Debug_fmt);
	std_DebugTuple_field(&b, &self->f1, chunks_header_VecU32_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/* chunks_trailer_VecTupleU64VecU32_Debug_fmt is a Vec<(u64, Vec<u32>)>'s Debug. */
static bool
chunks_trailer_VecTupleU64VecU32_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = (const StdVec *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
		std_DebugList_entry(&b, &STD_VEC_AT(v, chunks_trailer_Stamps_values_t, i),
							chunks_trailer_TupleU64VecU32_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* chunks_trailer_Stamps_Debug_fmt is Stamps' derived Debug. */
bool
chunks_trailer_Stamps_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksTrailerStamps *self = (const ChunksTrailerStamps *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Stamps", 6);
	StdOptionRef ends = {self->has_ends, {&self->ends, chunks_trailer_VecU64_Debug_fmt}};

	std_DebugStruct_field(&b, "single", 6, &self->single, chunks_trailer_VecStamp_Debug_fmt);
	std_DebugStruct_field(&b, "runs", 4, &self->runs, chunks_trailer_VecTupleStampU64_Debug_fmt);
	std_DebugStruct_field(&b, "ends", 4, &ends, std_option_Debug_fmt);
	std_DebugStruct_field(&b, "max_tuples", 10, &self->max_tuples, chunks_trailer_VecU64_Debug_fmt);
	std_DebugStruct_field(&b, "values", 6, &self->values, chunks_trailer_VecTupleU64VecU32_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* chunks_trailer_VecU64_PartialEq_eq is a Vec<u64>'s PartialEq. */
static bool
chunks_trailer_VecU64_PartialEq_eq(const StdVec *self, const StdVec *other)
{
	return self->len == other->len && memcmp(self->ptr, other->ptr, self->len * sizeof(uint64)) == 0;
}

/* chunks_trailer_Stamps_PartialEq_eq is Stamps' derived PartialEq: each field equal, in order. */
bool
chunks_trailer_Stamps_PartialEq_eq(const ChunksTrailerStamps *self, const ChunksTrailerStamps *other)
{
	if (!chunks_trailer_VecStamp_PartialEq_eq(&self->single, &other->single))
		return false;
	if (self->runs.len != other->runs.len)
		return false;
	for (size_t i = 0; i < self->runs.len; i++)
	{
		const chunks_trailer_Stamps_runs_t *a = &STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, i);
		const chunks_trailer_Stamps_runs_t *b = &STD_VEC_AT(&other->runs, chunks_trailer_Stamps_runs_t, i);

		if (!chunks_trailer_Stamp_PartialEq_eq(&a->f0, &b->f0) || a->f1 != b->f1)
			return false;
	}
	if (self->has_ends != other->has_ends ||
		(self->has_ends && !chunks_trailer_VecU64_PartialEq_eq(&self->ends, &other->ends)))
		return false;
	if (!chunks_trailer_VecU64_PartialEq_eq(&self->max_tuples, &other->max_tuples))
		return false;
	if (self->values.len != other->values.len)
		return false;
	for (size_t i = 0; i < self->values.len; i++)
	{
		const chunks_trailer_Stamps_values_t *a = &STD_VEC_AT(&self->values, chunks_trailer_Stamps_values_t, i);
		const chunks_trailer_Stamps_values_t *b = &STD_VEC_AT(&other->values, chunks_trailer_Stamps_values_t, i);

		if (a->f0 != b->f0 || a->f1.len != b->f1.len ||
			memcmp(a->f1.ptr, b->f1.ptr, a->f1.len * sizeof(uint32)) != 0)
			return false;
	}
	return true;
}

/* chunks_trailer_Stamps_drop is Stamps' drop glue: its vectors, and each entry's value ids. */
void
chunks_trailer_Stamps_drop(void *self_)
{
	ChunksTrailerStamps *self = self_;

	std_vec_free_Checked(&self->single, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	std_vec_free_Checked(&self->runs, sizeof(chunks_trailer_Stamps_runs_t), _Alignof(chunks_trailer_Stamps_runs_t));
	if (self->has_ends)
		std_vec_free_Checked(&self->ends, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&self->max_tuples, sizeof(uint64), _Alignof(uint64));
	for (size_t i = 0; i < self->values.len; i++)
		std_vec_free_Checked(&STD_VEC_AT(&self->values, chunks_trailer_Stamps_values_t, i).f1, sizeof(uint32),
							 _Alignof(uint32));
	std_vec_free_Checked(&self->values, sizeof(chunks_trailer_Stamps_values_t),
						 _Alignof(chunks_trailer_Stamps_values_t));
}

/* chunks_trailer_Stamps_Default_default is Stamps' derived Default: no stamps. */
ChunksTrailerStamps
chunks_trailer_Stamps_Default_default(void)
{
	ChunksTrailerStamps result = {0};

	std_vec_new(&result.single, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	std_vec_new(&result.runs, sizeof(chunks_trailer_Stamps_runs_t), _Alignof(chunks_trailer_Stamps_runs_t));
	std_vec_new(&result.max_tuples, sizeof(uint64), _Alignof(uint64));
	std_vec_new(&result.values, sizeof(chunks_trailer_Stamps_values_t), _Alignof(chunks_trailer_Stamps_values_t));
	return result;
}

/* chunks_trailer_Stamps_subtrees returns an internal tree node's stamps: each child's subtree's highest tuple id. */
ChunksTrailerStamps
chunks_trailer_Stamps_subtrees(StdVec *max_tuples)
{
	ChunksTrailerStamps result = chunks_trailer_Stamps_Default_default();

	std_vec_free_Checked(&result.max_tuples, sizeof(uint64), _Alignof(uint64));
	result.max_tuples = *max_tuples;
	return result;
}

/* chunks_trailer_Stamps_heap_bytes returns the bytes the stamps take beside their own struct. */
size_t
chunks_trailer_Stamps_heap_bytes(const ChunksTrailerStamps *self)
{
	size_t		single = STD_MUL(usize, self->single.cap, sizeof(ChunksTrailerStamp), 233, 9);
	size_t		runs = STD_MUL(usize, self->runs.cap, sizeof(chunks_trailer_Stamps_runs_t), 234, 15);
	size_t		total = STD_ADD(usize, single, runs, 233, 9);
	size_t		ends = self->has_ends ? STD_MUL(usize, self->ends.cap, sizeof(uint64), 235, 51) : 0;
	size_t		max_tuples;
	size_t		values;
	size_t		ids = 0;

	total = STD_ADD(usize, total, ends, 233, 9);
	max_tuples = STD_MUL(usize, self->max_tuples.cap, sizeof(uint64), 236, 15);
	total = STD_ADD(usize, total, max_tuples, 233, 9);
	values = STD_MUL(usize, self->values.cap, sizeof(chunks_trailer_Stamps_values_t), 237, 15);
	total = STD_ADD(usize, total, values, 233, 9);
	for (size_t i = 0, i_len = self->values.len; i < i_len; i++)
	{
		size_t		bytes = STD_MUL(usize, STD_VEC_AT(&self->values, chunks_trailer_Stamps_values_t, i).f1.cap,
									sizeof(uint32), 238, 49);

		ids = std_add_usize(ids, bytes, STD_ACCUM_FILE, 206, 1);
	}
	return STD_ADD(usize, total, ids, 233, 9);
}

/*
 * chunks_trailer_Stamps_subtree_max returns the highest tuple id of an
 * internal tree node's child's subtree, or false when the node's trailer
 * gives none.
 */
bool
chunks_trailer_Stamps_subtree_max(const ChunksTrailerStamps *self, size_t i, uint64 *result)
{
	if (i >= self->max_tuples.len)
		return false;
	*result = STD_VEC_AT(&self->max_tuples, uint64, i);
	return true;
}

/* chunks_trailer_Stamps_entries returns how many entries the stamps are of. */
size_t
chunks_trailer_Stamps_entries(const ChunksTrailerStamps *self)
{
	size_t		single = self->single.len;

	return self->has_ends ? self->ends.len : single;
}

/*
 * chunks_trailer_Stamps_span returns where an entry's stamps start and end,
 * counted through the runs, false past the last entry.
 */
static bool
chunks_trailer_Stamps_span(const ChunksTrailerStamps *self, size_t i, uint64 *result_0, uint64 *result_1)
{
	const StdVec *ends;
	uint64		start;
	size_t		prev;

	if (!self->has_ends)
		return false;
	ends = &self->ends;
	if (std_checked_sub_usize(i, 1, &prev))
	{
		std_index_check(prev, ends->len, RUST_FILE, 255, 53);
		start = STD_VEC_AT(ends, uint64, prev);
	}
	else
		start = 0;
	if (i >= ends->len)
		return false;
	*result_0 = start;
	*result_1 = STD_VEC_AT(ends, uint64, i);
	return true;
}

/* chunks_trailer_Stamps_entry_len returns how many stamps an entry has, none past the last entry. */
uint64
chunks_trailer_Stamps_entry_len(const ChunksTrailerStamps *self, size_t i)
{
	uint64		start;
	uint64		end;

	if (chunks_trailer_Stamps_span(self, i, &start, &end))
		return STD_SUB(u64, end, start, 261, 35);
	return (uint64) (!self->has_ends && i < self->single.len);
}

/*
 * chunks_trailer_Stamps_runs_between_iter is runs_between's iterator: the
 * range of runs left, and its map_while closure's captures.
 */
typedef struct chunks_trailer_Stamps_runs_between_iter
{
	size_t		at;
	size_t		at_end;
	const ChunksTrailerStamps *self;
	uint64		start;
	uint64		end;
} chunks_trailer_Stamps_runs_between_iter;

/* chunks_trailer_Stamps_runs_between_closure_0 is runs_between's |&(_, run_end)| run_end <= start. */
static bool
chunks_trailer_Stamps_runs_between_closure_0(void *captures, const void *elem)
{
	const uint64 *start = (const uint64 *) captures;

	return ((const chunks_trailer_Stamps_runs_t *) elem)->f1 <= *start;
}

/*
 * chunks_trailer_Stamps_runs_between_closure_1 is runs_between's map_while
 * closure: run at's part from start to end, false once the run starts at or
 * after end.
 */
static bool
chunks_trailer_Stamps_runs_between_closure_1(chunks_trailer_Stamps_runs_between_iter *captures, size_t at,
											 chunks_trailer_Stamps_runs_t *result)
{
	const StdVec *runs = &captures->self->runs;
	uint64		run_start;
	size_t		prev;
	ChunksTrailerStamp first;
	uint64		run_end;
	uint64		skip;
	uint64		len;

	if (std_checked_sub_usize(at, 1, &prev))
	{
		std_index_check(prev, runs->len, RUST_FILE, 290, 73);
		run_start = STD_VEC_AT(runs, chunks_trailer_Stamps_runs_t, prev).f1;
	}
	else
		run_start = 0;
	std_index_check(at, runs->len, RUST_FILE, 291, 45);
	first = STD_VEC_AT(runs, chunks_trailer_Stamps_runs_t, at).f0;
	run_end = STD_VEC_AT(runs, chunks_trailer_Stamps_runs_t, at).f1;
	if (!(run_start < captures->end))
		return false;
	skip = std_saturating_sub_u64(captures->start, run_start);
	result->f0.txn = first.txn;
	result->f0.tuple = STD_ADD(u64, first.tuple, skip, 294, 49);
	len = STD_SUB(u64, std_min_u64(run_end, captures->end), run_start, 294, 71);
	result->f1 = STD_SUB(u64, len, skip, 294, 71);
	return true;
}

/*
 * chunks_trailer_Stamps_runs_between returns the runs of the stamps from one
 * place to another, counted through the runs, cut at both.
 */
static chunks_trailer_Stamps_runs_between_iter
chunks_trailer_Stamps_runs_between(const ChunksTrailerStamps *self, uint64 start, uint64 end)
{
	chunks_trailer_Stamps_runs_between_iter result;
	size_t		from = std_slice_partition_point(self->runs.ptr, self->runs.len, sizeof(chunks_trailer_Stamps_runs_t),
												 chunks_trailer_Stamps_runs_between_closure_0, &start, NULL);

	result.at = from;
	result.at_end = self->runs.len;
	result.self = self;
	result.start = start;
	result.end = end;
	return result;
}

/* chunks_trailer_Stamps_runs_between_iter_next is the iterator's MapWhile next: the next run's part, false for None. */
static bool
chunks_trailer_Stamps_runs_between_iter_next(chunks_trailer_Stamps_runs_between_iter *self,
											 chunks_trailer_Stamps_runs_t *result)
{
	size_t		at;

	if (!(self->at < self->at_end))
		return false;
	at = self->at;
	self->at = at + 1;
	return chunks_trailer_Stamps_runs_between_closure_1(self, at, result);
}

/* chunks_trailer_Stamps_first returns an entry's first stamp, false for none. */
bool
chunks_trailer_Stamps_first(const ChunksTrailerStamps *self, size_t i, ChunksTrailerStamp *result)
{
	uint64		start;
	uint64		end;

	if (chunks_trailer_Stamps_span(self, i, &start, &end))
	{
		if (start < end)
		{
			chunks_trailer_Stamps_runs_between_iter runs = chunks_trailer_Stamps_runs_between(self, start, end);
			chunks_trailer_Stamps_runs_t run;

			if (!chunks_trailer_Stamps_runs_between_iter_next(&runs, &run))
				return false;
			*result = run.f0;
			return true;
		}
		return false;
	}
	if (i >= self->single.len)
		return false;
	if (self->has_ends)
		return false;
	*result = STD_VEC_AT(&self->single, ChunksTrailerStamp, i);
	return true;
}

/* chunks_trailer_Stamps_entry returns an entry's stamps, none past the last entry. */
void
chunks_trailer_Stamps_entry(const ChunksTrailerStamps *self, size_t i, StdVec *result)
{
	uint64		start;
	uint64		end;

	if (chunks_trailer_Stamps_span(self, i, &start, &end))
	{
		volatile StdVec out;
		volatile bool out_live = false;
		StdUnwind	unwind;

		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			StdVec	   *out_v = (StdVec *) &out;
			chunks_trailer_Stamps_runs_between_iter iter = chunks_trailer_Stamps_runs_between(self, start, end);
			bool		iter_done = false;
			bool		front_has = false;
			uint64		front_n = 0;
			uint64		front_len = 0;
			ChunksTrailerStamp front_first = {0};

			for (;;)
			{
				ChunksTrailerStamp elem;
				bool		has_elem = false;
				size_t		lower;

				for (;;)
				{
					chunks_trailer_Stamps_runs_t run;

					if (front_has)
					{
						if (front_n < front_len)
						{
							uint64		n = front_n;

							front_n = n + 1;
							elem.txn = front_first.txn;
							elem.tuple = STD_ADD(u64, front_first.tuple, n, 280, 95);
							has_elem = true;
							break;
						}
						front_has = false;
					}
					if (iter_done || !chunks_trailer_Stamps_runs_between_iter_next(&iter, &run))
					{
						iter_done = true;
						break;
					}
					front_has = true;
					front_first = run.f0;
					front_n = 0;
					front_len = run.f1;
				}
				if (!has_elem)
					break;
				lower = front_has ? (size_t) (front_len - front_n) : 0;
				if (!out_live)
				{
					std_vec_collect_first(out_v, &elem, lower, sizeof(ChunksTrailerStamp),
										  _Alignof(ChunksTrailerStamp));
					out_live = true;
				}
				else
					std_vec_extend_one(out_v, &elem, lower, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
			}
			if (!out_live)
				std_vec_new(out_v, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
			out_live = false;
			*result = *out_v;
			std_unwind_pop(&unwind);
		}
		else
		{
			if (out_live)
				std_vec_free_Checked((StdVec *) &out, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
			std_unwind_resume(&unwind);
		}
	}
	else
	{
		bool		has = i < self->single.len && !self->has_ends;
		StdSizeHint hint = {has ? 1 : 0, true, has ? 1 : 0};

		std_vec_with_capacity_trusted(result, hint, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		std_vec_extend_trusted_reserve(result, hint, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		if (has)
		{
			STD_VEC_AT(result, ChunksTrailerStamp, 0) = STD_VEC_AT(&self->single, ChunksTrailerStamp, i);
			result->len = 1;
		}
	}
}

/* chunks_trailer_Stamps_holds_closure_1 is holds' |&(_, run_end)| run_end <= start. */
static bool
chunks_trailer_Stamps_holds_closure_1(void *captures, const void *elem)
{
	return ((const chunks_trailer_Stamps_runs_t *) elem)->f1 <= *(const uint64 *) captures;
}

/* chunks_trailer_Stamps_holds_closure_2 is holds' |&(_, run_end)| run_end < end. */
static bool
chunks_trailer_Stamps_holds_closure_2(void *captures, const void *elem)
{
	return ((const chunks_trailer_Stamps_runs_t *) elem)->f1 < *(const uint64 *) captures;
}

/* chunks_trailer_Stamps_holds_closure_3 is holds' |&(first, _)| first.tuple <= tuple. */
static bool
chunks_trailer_Stamps_holds_closure_3(void *captures, const void *elem)
{
	return ((const chunks_trailer_Stamps_runs_t *) elem)->f0.tuple <= *(const uint64 *) captures;
}

/*
 * chunks_trailer_Stamps_holds reports whether an entry has a stamp of a tuple
 * id, by a search of its stamps, which are in tuple id order as a fold writes
 * them.
 */
bool
chunks_trailer_Stamps_holds(const ChunksTrailerStamps *self, size_t i, uint64 tuple)
{
	uint64		start;
	uint64		end;
	size_t		from;
	size_t		to;
	size_t		found;
	size_t		at;
	size_t		from_at;
	ChunksTrailerStamp first;
	uint64		run_end;
	uint64		run_start;
	size_t		prev;
	uint64		at_2;

	if (!chunks_trailer_Stamps_span(self, i, &start, &end))
		return !self->has_ends && i < self->single.len &&
			STD_VEC_AT(&self->single, ChunksTrailerStamp, i).tuple == tuple;
	from = std_slice_partition_point(self->runs.ptr, self->runs.len, sizeof(chunks_trailer_Stamps_runs_t),
									 chunks_trailer_Stamps_holds_closure_1, &start, NULL);
	to = std_slice_partition_point(self->runs.ptr, self->runs.len, sizeof(chunks_trailer_Stamps_runs_t),
								   chunks_trailer_Stamps_holds_closure_2, &end, NULL);
	to = std_min_usize(std_saturating_add_usize(to, 1), self->runs.len);
	std_range_check(from, to, self->runs.len, RUST_FILE, 307, 33);
	found = std_slice_partition_point((const chunks_trailer_Stamps_runs_t *) self->runs.ptr + from, to - from,
									  sizeof(chunks_trailer_Stamps_runs_t), chunks_trailer_Stamps_holds_closure_3,
									  &tuple, NULL);
	if (!std_checked_sub_usize(found, 1, &at))
		return false;
	from_at = STD_ADD(usize, from, at, 310, 42);
	std_index_check(from_at, self->runs.len, RUST_FILE, 310, 41);
	first = STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, from_at).f0;
	run_end = STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, from_at).f1;
	if (std_checked_sub_usize(STD_ADD(usize, from, at, 311, 26), 1, &prev))
	{
		std_index_check(prev, self->runs.len, RUST_FILE, 311, 78);
		run_start = STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, prev).f1;
	}
	else
		run_start = 0;
	at_2 = std_saturating_add_u64(run_start, STD_SUB(u64, tuple, first.tuple, 312, 43));
	return at_2 >= std_max_u64(start, run_start) && at_2 < std_min_u64(end, run_end);
}

/*
 * chunks_trailer_Stamps_max_tuple returns the largest tuple id of any stamp,
 * or of an internal tree node's subtrees, or 0 for none.
 */
uint64
chunks_trailer_Stamps_max_tuple(const ChunksTrailerStamps *self)
{
	uint64		start = 0;
	bool		has_max = false;
	uint64		max = 0;

	for (size_t i = 0, i_len = self->single.len; i < i_len; i++)
	{
		uint64		tuple = STD_VEC_AT(&self->single, ChunksTrailerStamp, i).tuple;

		max = has_max ? std_max_u64(max, tuple) : tuple;
		has_max = true;
	}
	for (size_t i = 0, i_len = self->max_tuples.len; i < i_len; i++)
	{
		uint64		tuple = STD_VEC_AT(&self->max_tuples, uint64, i);

		max = has_max ? std_max_u64(max, tuple) : tuple;
		has_max = true;
	}
	for (size_t i = 0, i_len = self->runs.len; i < i_len; i++)
	{
		ChunksTrailerStamp first = STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, i).f0;
		uint64		end = STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, i).f1;
		uint64		len = STD_SUB(u64, end, start, 321, 39);
		uint64		last = STD_SUB(u64, STD_ADD(u64, first.tuple, len, 321, 24), 1,
								   321, 24);

		start = end;
		max = has_max ? std_max_u64(max, last) : last;
		has_max = true;
	}
	return has_max ? max : 0;
}

/* binary_search_by_key at a u64 key, for values. */
#define STD_SLICE_SEARCH_SCOPE static
#define STD_SLICE_SEARCH_DECLARE
#define STD_SLICE_SEARCH_DEFINE
#define STD_SLICE_SEARCH_BS U64
#define STD_SLICE_SEARCH_B uint64
#define STD_SLICE_SEARCH_B_CMP std_u64_Ord_cmp
#include "doltgres/std/slice_search_template.h"

/* chunks_trailer_Stamps_push adds the next entry's stamps. */
void
chunks_trailer_Stamps_push(ChunksTrailerStamps *self, const ChunksTrailerStamp *entry, size_t entry_len)
{
	if (entry_len == 1 && !self->has_ends)
	{
		ChunksTrailerStamp stamp;

		std_index_check(0, entry_len, RUST_FILE, 331, 30);
		stamp = entry[0];
		std_vec_push_Checked(&self->single, &stamp, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		return;
	}
	chunks_trailer_Stamps_keep_runs(self);
	for (size_t i = 0, i_len = entry_len; i < i_len; i++)
		chunks_trailer_Stamps_append(self, entry[i], 1);
	chunks_trailer_Stamps_end_entry(self);
}

/* chunks_trailer_Stamps_push_values gives the entry pushed last its value ids, none for an entry with no value out of line. */
void
chunks_trailer_Stamps_push_values(ChunksTrailerStamps *self, const uint32 *ids, size_t ids_len)
{
	size_t		last;

	if (std_checked_sub_usize(chunks_trailer_Stamps_entries(self), 1, &last) && !(ids_len == 0))
	{
		volatile chunks_trailer_Stamps_values_t value;
		volatile bool value_live = false;
		StdUnwind	unwind;

		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			chunks_trailer_Stamps_values_t *value_v = (chunks_trailer_Stamps_values_t *) &value;

			value_v->f0 = (uint64) last;
			std_slice_to_vec_Checked(&value_v->f1, ids, ids_len, sizeof(uint32), _Alignof(uint32));
			value_live = true;
			std_vec_push_Checked(&self->values, value_v, sizeof(chunks_trailer_Stamps_values_t),
								 _Alignof(chunks_trailer_Stamps_values_t));
			value_live = false;
			std_unwind_pop(&unwind);
		}
		else
		{
			if (value_live)
				std_vec_free_Checked((StdVec *) &value.f1, sizeof(uint32), _Alignof(uint32));
			std_unwind_resume(&unwind);
		}
	}
}

/* chunks_trailer_Stamps_values_closure_0 is values' |&(at, _)| at. */
static uint64
chunks_trailer_Stamps_values_closure_0(void *captures, const void *elem)
{
	return ((const chunks_trailer_Stamps_values_t *) elem)->f0;
}

/* chunks_trailer_Stamps_values returns an entry's value ids, none for an entry with no value out of line. */
const uint32 *
chunks_trailer_Stamps_values(const ChunksTrailerStamps *self, size_t i, size_t *result_len)
{
	uint64		key = (uint64) i;
	size_t		at;

	if (std_slice_binary_search_by_keyU64(self->values.ptr, self->values.len, sizeof(chunks_trailer_Stamps_values_t),
										  &key, chunks_trailer_Stamps_values_closure_0, NULL, NULL, &at))
	{
		std_index_check(at, self->values.len, RUST_FILE, 349, 35);
		*result_len = STD_VEC_AT(&self->values, chunks_trailer_Stamps_values_t, at).f1.len;
		return (const uint32 *) STD_VEC_AT(&self->values, chunks_trailer_Stamps_values_t, at).f1.ptr;
	}
	*result_len = 0;
	return (const uint32 *) STD_DANGLING;
}

/* chunks_trailer_Stamps_push_entry_of adds another's entry's stamps as the next entry's, run by run, with its value ids. */
void
chunks_trailer_Stamps_push_entry_of(ChunksTrailerStamps *self, const ChunksTrailerStamps *other, size_t i)
{
	uint64		start;
	uint64		end;
	const uint32 *ids;
	size_t		ids_len;

	if (chunks_trailer_Stamps_span(other, i, &start, &end) && STD_SUB(u64, end, start, 357, 35) == 1 &&
		!self->has_ends)
	{
		ChunksTrailerStamp first;
		bool		has = chunks_trailer_Stamps_first(other, i, &first);
		StdSizeHint hint = {has ? 1 : 0, true, has ? 1 : 0};

		std_vec_extend_trusted_reserve(&self->single, hint, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		if (has)
		{
			STD_VEC_AT(&self->single, ChunksTrailerStamp, self->single.len) = first;
			self->single.len++;
		}
	}
	else if (chunks_trailer_Stamps_span(other, i, &start, &end))
	{
		chunks_trailer_Stamps_runs_between_iter runs;
		chunks_trailer_Stamps_runs_t run;

		chunks_trailer_Stamps_keep_runs(self);
		runs = chunks_trailer_Stamps_runs_between(other, start, end);
		while (chunks_trailer_Stamps_runs_between_iter_next(&runs, &run))
			chunks_trailer_Stamps_append(self, run.f0, run.f1);
		chunks_trailer_Stamps_end_entry(self);
	}
	else
	{
		volatile StdVec entry;
		volatile bool entry_live = false;
		StdUnwind	unwind;

		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			StdVec	   *entry_v = (StdVec *) &entry;

			chunks_trailer_Stamps_entry(other, i, entry_v);
			entry_live = true;
			chunks_trailer_Stamps_push(self, entry_v->ptr, entry_v->len);
			entry_live = false;
			std_vec_free_Checked(entry_v, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
			std_unwind_pop(&unwind);
		}
		else
		{
			if (entry_live)
				std_vec_free_Checked((StdVec *) &entry, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
			std_unwind_resume(&unwind);
		}
	}
	ids = chunks_trailer_Stamps_values(other, i, &ids_len);
	chunks_trailer_Stamps_push_values(self, ids, ids_len);
}

/* chunks_trailer_Stamps_keep_runs keeps the stamps as runs from now on. */
static void
chunks_trailer_Stamps_keep_runs(ChunksTrailerStamps *self)
{
	volatile StdVecIntoIter taken;
	volatile bool taken_live = false;
	StdUnwind	unwind;

	if (self->has_ends)
		return;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVecIntoIter *taken_v = (StdVecIntoIter *) &taken;
		uint64		n = (uint64) self->single.len;
		StdSizeHint hint = {(size_t) n, true, (size_t) n};
		StdVec		ends;
		StdVec		single;
		ChunksTrailerStamp stamp;

		std_vec_with_capacity_trusted(&ends, hint, sizeof(uint64), _Alignof(uint64));
		std_vec_extend_trusted_reserve(&ends, hint, sizeof(uint64), _Alignof(uint64));
		for (uint64 value = 1; value <= n; value++)
		{
			STD_VEC_AT(&ends, uint64, ends.len) = value;
			ends.len++;
		}
		self->ends = ends;
		self->has_ends = true;
		single = self->single;
		std_vec_new(&self->single, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		*taken_v = std_Vec_IntoIterator_into_iter(&single, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp),
												  NULL);
		taken_live = true;
		while (std_VecIntoIter_Iterator_next(taken_v, &stamp))
			chunks_trailer_Stamps_append(self, stamp, 1);
		taken_live = false;
		std_VecIntoIter_drop(taken_v);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (taken_live)
			std_VecIntoIter_drop((StdVecIntoIter *) &taken);
		std_unwind_resume(&unwind);
	}
}

/* chunks_trailer_Stamps_append adds a run of stamps after the last, joining the last run when it continues it. */
static void
chunks_trailer_Stamps_append(ChunksTrailerStamps *self, ChunksTrailerStamp first, uint64 len)
{
	uint64		last_end_or_0 = self->runs.len > 0 ?
		STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, self->runs.len - 1).f1 : 0;
	uint64		end = STD_ADD(u64, last_end_or_0, len, 381, 19);
	chunks_trailer_Stamps_runs_t run;

	if (self->runs.len > 0)
	{
		chunks_trailer_Stamps_runs_t last = STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, self->runs.len - 1);
		size_t		prev;
		uint64		prev_end;
		uint64		last_len;

		if (std_checked_sub_usize(self->runs.len, 2, &prev))
		{
			std_index_check(prev, self->runs.len, RUST_FILE, 383, 96);
			prev_end = STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, prev).f1;
		}
		else
			prev_end = 0;
		last_len = STD_SUB(u64, last.f1, prev_end, 383, 28);
		if (last.f0.txn == first.txn &&
			STD_ADD(u64, last.f0.tuple, last_len, 384, 41) == first.tuple)
		{
			if (self->runs.len == 0)
				STD_OPTION_EXPECT_FAILED(385, 38, "a last run");
			STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, self->runs.len - 1).f1 = end;
			return;
		}
	}
	run.f0 = first;
	run.f1 = end;
	std_vec_push_Checked(&self->runs, &run, sizeof(chunks_trailer_Stamps_runs_t),
						 _Alignof(chunks_trailer_Stamps_runs_t));
}

/* chunks_trailer_Stamps_end_entry ends the entry whose stamps were appended last. */
static void
chunks_trailer_Stamps_end_entry(ChunksTrailerStamps *self)
{
	uint64		end = self->runs.len > 0 ?
		STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, self->runs.len - 1).f1 : 0;

	if (!self->has_ends)
	{
		std_vec_new(&self->ends, sizeof(uint64), _Alignof(uint64));
		self->has_ends = true;
	}
	std_vec_push_Checked(&self->ends, &end, sizeof(uint64), _Alignof(uint64));
}

/*
 * chunks_trailer_Stamps_write appends the stamps' stored form: each stamp in
 * turn while every entry has one, and otherwise how many each entry has
 * followed by the runs, each its first stamp and its length, or an internal
 * tree node's subtrees' highest tuple ids in turn.
 */
static void
chunks_trailer_Stamps_write(const ChunksTrailerStamps *self, StdVec *out)
{
	uint64		start;

	if (!(self->max_tuples.len == 0))
	{
		for (size_t i = 0, i_len = self->max_tuples.len; i < i_len; i++)
			chunks_varint_put_varint(out, STD_VEC_AT(&self->max_tuples, uint64, i));
		return;
	}
	if (!self->has_ends)
	{
		for (size_t i = 0, i_len = self->single.len; i < i_len; i++)
			chunks_trailer_Stamp_write(&STD_VEC_AT(&self->single, ChunksTrailerStamp, i), out);
		return;
	}
	chunks_varint_put_varint(out, (uint64) self->ends.len);
	start = 0;
	for (size_t i = 0, i_len = self->ends.len; i < i_len; i++)
	{
		uint64		end = STD_VEC_AT(&self->ends, uint64, i);

		chunks_varint_put_varint(out, STD_SUB(u64, end, start, 413, 29));
		start = end;
	}
	start = 0;
	for (size_t i = 0, i_len = self->runs.len; i < i_len; i++)
	{
		ChunksTrailerStamp first = STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, i).f0;
		uint64		end = STD_VEC_AT(&self->runs, chunks_trailer_Stamps_runs_t, i).f1;

		chunks_trailer_Stamp_write(&first, out);
		chunks_varint_put_varint(out, STD_SUB(u64, end, start, 419, 29));
		start = end;
	}
}

/*
 * chunks_trailer_Stamps_write_values appends the value ids section's stored
 * form: for each entry with values out of line, in entry order, its index,
 * its count of ids, and the ids, each a varint, then the section's length, 4
 * bytes little-endian.
 */
static void
chunks_trailer_Stamps_write_values(const ChunksTrailerStamps *self, StdVec *out)
{
	size_t		start = out->len;
	uint32		len;
	uint8		bytes[4];

	for (size_t i = 0, i_len = self->values.len; i < i_len; i++)
	{
		const chunks_trailer_Stamps_values_t *value = &STD_VEC_AT(&self->values, chunks_trailer_Stamps_values_t, i);

		chunks_varint_put_varint(out, value->f0);
		chunks_varint_put_varint(out, (uint64) value->f1.len);
		for (size_t j = 0, j_len = value->f1.len; j < j_len; j++)
			chunks_varint_put_varint(out, (uint64) STD_VEC_AT(&value->f1, uint32, j));
	}
	len = (uint32) STD_SUB(usize, out->len, start, 433, 20);
	std_u32_to_le_bytes(len, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, 4, 1, 1);
}

/* chunks_trailer_Stamps_read_closure_0 is read's cut: the error of a malformed value ids section. */
static ChunksErrorError
chunks_trailer_Stamps_read_closure_0(void *captures)
{
	StdString	message;

	std_string_from_Checked(&message, "chunk trailer's value ids are malformed",
							sizeof("chunk trailer's value ids are malformed") - 1);
	return chunks_error_Error_corrupt(&message);
}

/*
 * chunks_trailer_Stamps_read reads stamps' stored form, as runs when runs,
 * followed by the value ids section when values, failing for any form a
 * writer does not produce: of the value ids, an entry out of order or past
 * the entries, a count of 0, an id past 2^32, an empty section, and a
 * section cut short or with bytes past its ids.
 */
static bool
chunks_trailer_Stamps_read(const uint8 *bytes, size_t bytes_len, bool runs, bool values, ChunksTrailerStamps *result,
						   ChunksErrorError *error)
{
	size_t		len_at;
	size_t		len;
	size_t		section_at;
	volatile ChunksTrailerStamps stamps;
	volatile bool stamps_live = false;
	volatile StdVec ids;
	volatile bool ids_live = false;
	bool		ok = false;
	StdUnwind	unwind;

	if (!values)
		return chunks_trailer_Stamps_read_stamps(bytes, bytes_len, runs, result, error);
	if (!std_checked_sub_usize(bytes_len, 4, &len_at))
	{
		*error = chunks_trailer_Stamps_read_closure_0(NULL);
		return false;
	}
	std_range_from_check(len_at, bytes_len, RUST_FILE, 446, 43);
	if (bytes_len - len_at != 4)
	{
		*error = chunks_trailer_Stamps_read_closure_0(NULL);
		return false;
	}
	len = (size_t) std_u32_from_le_bytes(bytes + len_at);
	if (!std_checked_sub_usize(len_at, len, &section_at))
	{
		*error = chunks_trailer_Stamps_read_closure_0(NULL);
		return false;
	}
	std_range_to_check(section_at, bytes_len, RUST_FILE, 448, 52);
	if (!chunks_trailer_Stamps_read_stamps(bytes, section_at, runs, (ChunksTrailerStamps *) &stamps, error))
		return false;
	stamps_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		ChunksTrailerStamps *stamps_v = (ChunksTrailerStamps *) &stamps;
		StdVec	   *ids_v = (StdVec *) &ids;
		const uint8 *section;
		size_t		section_len;
		size_t		at = 0;

		std_range_check(section_at, len_at, bytes_len, RUST_FILE, 449, 29);
		section = bytes + section_at;
		section_len = len_at - section_at;
		ok = true;
		while (at < section_len)
		{
			uint64		entry;
			uint64		count;
			bool		after;

			if (!chunks_varint_read_varint(section, section_len, &at, &entry, error) ||
				!chunks_varint_read_varint(section, section_len, &at, &count, error))
			{
				ok = false;
				break;
			}
			after = stamps_v->values.len == 0 ||
				entry > STD_VEC_AT(&stamps_v->values, chunks_trailer_Stamps_values_t, stamps_v->values.len - 1).f0;
			if (!after || entry >= (uint64) chunks_trailer_Stamps_entries(stamps_v) || count == 0 ||
				count > (uint64) section_len)
			{
				*error = chunks_trailer_Stamps_read_closure_0(NULL);
				ok = false;
				break;
			}
			std_vec_with_capacity_Checked(ids_v, (size_t) count, sizeof(uint32), _Alignof(uint32));
			ids_live = true;
			for (uint64 n = 0, n_end = count; n < n_end; n++)
			{
				uint64		id;

				if (!chunks_varint_read_varint(section, section_len, &at, &id, error))
				{
					ok = false;
					break;
				}
				if (id > PG_UINT32_MAX)
				{
					*error = chunks_trailer_Stamps_read_closure_0(NULL);
					ok = false;
					break;
				}
				{
					uint32		id_2 = (uint32) id;

					std_vec_push_Checked(ids_v, &id_2, sizeof(uint32), _Alignof(uint32));
				}
			}
			if (!ok)
			{
				ids_live = false;
				std_vec_free_Checked(ids_v, sizeof(uint32), _Alignof(uint32));
				break;
			}
			{
				chunks_trailer_Stamps_values_t value = {entry, *ids_v};

				std_vec_push_Checked(&stamps_v->values, &value, sizeof(chunks_trailer_Stamps_values_t),
									 _Alignof(chunks_trailer_Stamps_values_t));
				ids_live = false;
			}
		}
		if (ok && stamps_v->values.len == 0)
		{
			*error = chunks_trailer_Stamps_read_closure_0(NULL);
			ok = false;
		}
		stamps_live = false;
		if (ok)
			*result = *stamps_v;
		else
			chunks_trailer_Stamps_drop(stamps_v);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (ids_live)
			std_vec_free_Checked((StdVec *) &ids, sizeof(uint32), _Alignof(uint32));
		if (stamps_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &stamps);
		std_unwind_resume(&unwind);
	}
	return ok;
}

/* chunks_trailer_Stamps_read_stamps_closure_0 is read_stamps' corrupt: the error of malformed stamps. */
static ChunksErrorError
chunks_trailer_Stamps_read_stamps_closure_0(void *captures)
{
	StdString	message;

	std_string_from_Checked(&message, "chunk trailer's stamps are malformed",
							sizeof("chunk trailer's stamps are malformed") - 1);
	return chunks_error_Error_corrupt(&message);
}

/*
 * chunks_trailer_Stamps_read_stamps reads stamps' stored form, as runs when
 * runs, failing for any form a writer does not produce.
 */
static bool
chunks_trailer_Stamps_read_stamps(const uint8 *bytes, size_t bytes_len, bool runs, ChunksTrailerStamps *result,
								  ChunksErrorError *error)
{
	volatile ChunksTrailerStamps stamps;
	volatile bool stamps_live = false;
	volatile StdVec ends;
	volatile bool ends_live = false;
	bool		ok = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		ChunksTrailerStamps *stamps_v = (ChunksTrailerStamps *) &stamps;
		StdVec	   *ends_v = (StdVec *) &ends;
		size_t		at = 0;
		uint64		entries;
		uint64		end = 0;
		uint64		total = 0;

		if (!runs)
		{
			if (!std_is_multiple_of_usize(bytes_len, CHUNKS_TRAILER_STAMP_LEN))
				*error = chunks_trailer_Stamps_read_stamps_closure_0(NULL);
			else
			{
				StdChunks	chunks = std_slice_chunks(bytes, bytes_len, CHUNKS_TRAILER_STAMP_LEN);
				size_t		n = bytes_len / CHUNKS_TRAILER_STAMP_LEN +
					(size_t) (bytes_len % CHUNKS_TRAILER_STAMP_LEN != 0);
				StdSizeHint hint = {n, true, n};
				const void *chunk;
				size_t		chunk_len;
				StdVec		single;

				std_vec_with_capacity_trusted(&single, hint, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
				std_vec_extend_trusted_reserve(&single, hint, sizeof(ChunksTrailerStamp),
											   _Alignof(ChunksTrailerStamp));
				while (std_slice_Chunks_next(&chunks, 1, 1, &chunk, &chunk_len))
				{
					STD_VEC_AT(&single, ChunksTrailerStamp, single.len) =
						chunks_trailer_Stamp_read((const uint8 *) chunk, chunk_len);
					single.len++;
				}
				*result = chunks_trailer_Stamps_Default_default();
				std_vec_free_Checked(&result->single, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
				result->single = single;
				ok = true;
			}
			std_unwind_pop(&unwind);
			return ok;
		}
		if (!chunks_varint_read_varint(bytes, bytes_len, &at, &entries, error))
		{
			std_unwind_pop(&unwind);
			return false;
		}
		std_vec_with_capacity_Checked(ends_v, (size_t) std_min_u64(entries, (uint64) bytes_len), sizeof(uint64),
									  _Alignof(uint64));
		ends_live = true;
		ok = true;
		for (uint64 n = 0, n_end = entries; n < n_end; n++)
		{
			uint64		step;

			if (!chunks_varint_read_varint(bytes, bytes_len, &at, &step, error))
			{
				ok = false;
				break;
			}
			if (!std_checked_add_u64(end, step, &end))
			{
				*error = chunks_trailer_Stamps_read_stamps_closure_0(NULL);
				ok = false;
				break;
			}
			std_vec_push_Checked(ends_v, &end, sizeof(uint64), _Alignof(uint64));
		}
		if (!ok)
		{
			ends_live = false;
			std_vec_free_Checked(ends_v, sizeof(uint64), _Alignof(uint64));
			std_unwind_pop(&unwind);
			return false;
		}
		*stamps_v = chunks_trailer_Stamps_Default_default();
		stamps_v->ends = *ends_v;
		stamps_v->has_ends = true;
		ends_live = false;
		stamps_live = true;
		while (at < bytes_len)
		{
			size_t		chunk_end = STD_ADD(usize, at, CHUNKS_TRAILER_STAMP_LEN, 490, 51);
			ChunksTrailerStamp first;
			uint64		len;
			uint64		past;
			bool		has_last;
			chunks_trailer_Stamps_runs_t run;

			if (!(at <= chunk_end && chunk_end <= bytes_len))
			{
				*error = chunks_trailer_Stamps_read_stamps_closure_0(NULL);
				ok = false;
				break;
			}
			first = chunks_trailer_Stamp_read(bytes + at, CHUNKS_TRAILER_STAMP_LEN);
			at = STD_ADD(usize, at, CHUNKS_TRAILER_STAMP_LEN, 491, 13);
			if (!chunks_varint_read_varint(bytes, bytes_len, &at, &len, error))
			{
				ok = false;
				break;
			}
			has_last = std_checked_add_u64(first.tuple, len, &past) && len > 0 &&
				past <= STD_SHL_FOLDED(u64, 1, 48);
			if (!(std_checked_add_u64(total, len, &total) && has_last && total <= end))
			{
				*error = chunks_trailer_Stamps_read_stamps_closure_0(NULL);
				ok = false;
				break;
			}
			run.f0 = first;
			run.f1 = total;
			std_vec_push_Checked(&stamps_v->runs, &run, sizeof(chunks_trailer_Stamps_runs_t),
								 _Alignof(chunks_trailer_Stamps_runs_t));
		}
		if (ok && total != end)
		{
			*error = chunks_trailer_Stamps_read_stamps_closure_0(NULL);
			ok = false;
		}
		stamps_live = false;
		if (ok)
			*result = *stamps_v;
		else
			chunks_trailer_Stamps_drop(stamps_v);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (stamps_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &stamps);
		if (ends_live)
			std_vec_free_Checked((StdVec *) &ends, sizeof(uint64), _Alignof(uint64));
		std_unwind_resume(&unwind);
	}
	return ok;
}

/* chunks_trailer_Stamps_FromVecStamp_from is Stamps' From<Vec<Stamp>>: one stamp for each entry. */
ChunksTrailerStamps
chunks_trailer_Stamps_FromVecStamp_from(StdVec *single)
{
	ChunksTrailerStamps result = chunks_trailer_Stamps_Default_default();

	std_vec_free_Checked(&result.single, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	result.single = *single;
	return result;
}

/*
 * chunks_trailer_store returns a chunk's stored bytes: its hashed bytes,
 * then a trailer with the locators of its addresses (in address table order),
 * the stamps of its rows or its subtrees' highest tuple ids, its rows' value
 * ids when any has one, and the trailer's checksum when there are any
 * locators or stamps.
 */
void
chunks_trailer_store(const uint8 *hashed, size_t hashed_len, const ChunksTrailerLocator *locators, size_t locators_len,
					 const ChunksTrailerStamps *stamps, StdVec *result)
{
	bool		has_locators = false;
	bool		has_stamps;
	bool		has_max_tuples;
	volatile StdVec out;
	volatile bool out_live = false;
	StdUnwind	unwind;

	for (size_t i = 0, i_len = locators_len; i < i_len; i++)
	{
		if (chunks_trailer_Locator_is_known(&locators[i]))
		{
			has_locators = true;
			break;
		}
	}
	has_stamps = chunks_trailer_Stamps_entries(stamps) > 0;
	has_max_tuples = !(stamps->max_tuples.len == 0);
	CHUNKS_CONTRACT(517, 5, !(has_stamps && has_max_tuples), "a node with both row stamps and subtrees");
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *out_v = (StdVec *) &out;
		uint8		le[4];

		if (!has_locators && !has_stamps && !has_max_tuples)
		{
			std_vec_with_capacity_Checked(out_v, STD_ADD(usize, hashed_len, 4, 519, 42), 1, 1);
			out_live = true;
			std_vec_extend_from_slice_Checked(out_v, hashed, hashed_len, 1, 1);
			std_u32_to_le_bytes(0, le);
			std_vec_extend_from_slice_Checked(out_v, le, 4, 1, 1);
		}
		else
		{
			bool		has_values = !(stamps->values.len == 0);
			size_t		len = 5 + CHUNKS_TRAILER_CHECKSUM_LEN;
			size_t		sum = 0;
			size_t		start;
			uint8		runs;
			uint8		max_tuples;
			uint8		values;
			uint8		flags;
			uint32		len_2;
			uint8		checksum[8];

			len = STD_ADD(usize, len, STD_MUL(usize, locators_len, CHUNKS_TRAILER_LOCATOR_LEN,
											  527, 11), 525, 15);
			len = STD_ADD(usize, len, STD_MUL(usize, chunks_trailer_Stamps_entries(stamps), CHUNKS_TRAILER_STAMP_LEN,
											  528, 11), 525, 15);
			len = STD_ADD(usize, len, STD_MUL(usize, stamps->max_tuples.len, 7, 529, 11),
						  525, 15);
			for (size_t i = 0, i_len = stamps->values.len; i < i_len; i++)
			{
				size_t		ids_len = STD_VEC_AT(&stamps->values, chunks_trailer_Stamps_values_t, i).f1.len;
				size_t		bytes = STD_ADD(usize, 4, STD_MUL(usize, 5, ids_len, 530, 51),
											530, 47);

				sum = std_add_usize(sum, bytes, STD_ACCUM_FILE, 206, 1);
			}
			len = STD_ADD(usize, len, sum, 525, 15);
			std_vec_with_capacity_Checked(out_v, STD_ADD(usize, hashed_len, len, 531, 38), 1, 1);
			out_live = true;
			std_vec_extend_from_slice_Checked(out_v, hashed, hashed_len, 1, 1);
			start = out_v->len;
			runs = stamps->has_ends ? CHUNKS_TRAILER_HAS_STAMP_RUNS : 0;
			max_tuples = has_max_tuples ? CHUNKS_TRAILER_HAS_MAX_TUPLES : 0;
			values = has_values ? CHUNKS_TRAILER_HAS_VALUES : 0;
			flags = (has_locators ? CHUNKS_TRAILER_HAS_LOCATORS : 0) |
				(has_stamps ? CHUNKS_TRAILER_HAS_STAMPS | runs : 0) | max_tuples | values;
			std_vec_u8_push_Checked(out_v, flags);
			if (has_locators)
			{
				for (size_t i = 0, i_len = locators_len; i < i_len; i++)
					chunks_trailer_Locator_write(&locators[i], out_v);
			}
			if (has_stamps || has_max_tuples)
				chunks_trailer_Stamps_write(stamps, out_v);
			if (has_values)
				chunks_trailer_Stamps_write_values(stamps, out_v);
			len_2 = (uint32) STD_ADD(usize, STD_SUB(usize, out_v->len, start, 552, 16),
									 CHUNKS_TRAILER_CHECKSUM_LEN, 552, 16);
			std_range_from_check(start, out_v->len, RUST_FILE, 553, 49);
			std_u64_to_le_bytes(chunks_trailer_trailer_checksum((const uint8 *) out_v->ptr + start, out_v->len - start, len_2),
								checksum);
			std_vec_extend_from_slice_Checked(out_v, checksum, 8, 1, 1);
			std_u32_to_le_bytes(len_2, le);
			std_vec_extend_from_slice_Checked(out_v, le, 4, 1, 1);
		}
		out_live = false;
		*result = *out_v;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (out_live)
			std_vec_free_Checked((StdVec *) &out, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/* chunks_trailer_unlocated_closure_0 is unlocated's |_| Ok(0): no addresses. */
static bool
chunks_trailer_unlocated_closure_0(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
								   ChunksErrorError *error)
{
	*result = 0;
	return true;
}

/*
 * chunks_trailer_unlocated splits a stored chunk whose trailer holds no
 * locators into its hashed bytes and its stamps, as a built base's node
 * record carries them beside each other.
 */
bool
chunks_trailer_unlocated(const uint8 *stored, size_t stored_len, StdBytes *result_0, ChunksTrailerStamps *result_1,
						 ChunksErrorError *error)
{
	ChunksTrailerStored split;

	if (!chunks_trailer_Stored_split(stored, stored_len, chunks_trailer_unlocated_closure_0, NULL, NULL, &split, error))
		return false;
	if (!chunks_trailer_Stored_stamps(&split, result_1, error))
		return false;
	*result_0 = split.hashed;
	return true;
}

/* chunks_trailer_trailer_checksum returns the checksum of a trailer's flags, locators, and stamps with its stored length. */
static uint64
chunks_trailer_trailer_checksum(const uint8 *trailer, size_t trailer_len, uint32 len)
{
	XXH3_state_t hasher;
	uint8		le[4];

	XXH3_INITSTATE(&hasher);
	XXH3_64bits_reset(&hasher);
	XXH3_64bits_update(&hasher, trailer, trailer_len);
	std_u32_to_le_bytes(len, le);
	XXH3_64bits_update(&hasher, le, 4);
	return XXH3_64bits_digest(&hasher);
}

/* chunks_trailer_Stored_split_closure_0 is split's short: the error of a chunk shorter than its trailer. */
static ChunksErrorError
chunks_trailer_Stored_split_closure_0(void *captures)
{
	StdString	message;

	std_string_from_Checked(&message, "stored chunk is shorter than its trailer",
							sizeof("stored chunk is shorter than its trailer") - 1);
	return chunks_error_Error_corrupt(&message);
}

/* CHUNKS_TRAILER_CORRUPT sets the error to Error::corrupt of a literal message. */
#define CHUNKS_TRAILER_CORRUPT(literal) \
	do { \
		StdString	message_; \
\
		std_string_from_Checked(&message_, (literal), sizeof(literal) - 1); \
		*error = chunks_error_Error_corrupt(&message_); \
	} while (0)

/*
 * chunks_trailer_Stored_split reads a stored chunk's trailer, whose hashed
 * bytes hold the addresses addresses counts, after checking its checksum.
 */
bool
chunks_trailer_Stored_split(const uint8 *stored, size_t stored_len,
							bool (*addresses) (void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
											   ChunksErrorError *error),
							void *addresses_captures, void (*addresses_drop) (void *), ChunksTrailerStored *result,
							ChunksErrorError *error)
{
	volatile bool addresses_live = true;
	bool		ok = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		size_t		len_at;
		size_t		len;
		size_t		start;
		size_t		checksum_at;
		uint64		checksum;
		StdBytes	hashed;
		StdBytes	trailer;
		size_t		trailer_at;
		uint8		flags;
		size_t		locators_len;
		uint8		sections;
		bool		values;

		if (!std_checked_sub_usize(stored_len, 4, &len_at))
		{
			*error = chunks_trailer_Stored_split_closure_0(NULL);
			goto done;
		}
		std_range_from_check(len_at, stored_len, RUST_FILE, 593, 44);
		if (stored_len - len_at != 4)
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
									 std_TryFromSliceError_Debug_fmt, RUST_FILE, 593, 66);
		len = (size_t) std_u32_from_le_bytes(stored + len_at);
		if (len == 0)
		{
			std_range_to_check(len_at, stored_len, RUST_FILE, 596, 32);
			memset(result, 0, sizeof(*result));
			result->hashed.ptr = stored;
			result->hashed.len = len_at;
			result->locators.ptr = (const uint8 *) STD_DANGLING;
			result->stamps.ptr = (const uint8 *) STD_DANGLING;
			ok = true;
			goto done;
		}
		if (!std_checked_sub_usize(len_at, len, &start) || !(len > CHUNKS_TRAILER_CHECKSUM_LEN))
		{
			*error = chunks_trailer_Stored_split_closure_0(NULL);
			goto done;
		}
		checksum_at = STD_SUB(usize, len_at, CHUNKS_TRAILER_CHECKSUM_LEN, 605, 27);
		std_range_check(checksum_at, len_at, stored_len, RUST_FILE, 606, 49);
		if (len_at - checksum_at != 8)
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
									 std_TryFromSliceError_Debug_fmt, RUST_FILE, 606, 82);
		checksum = std_u64_from_le_bytes(stored + checksum_at);
		std_range_check(start, checksum_at, stored_len, RUST_FILE, 607, 36);
		if (chunks_trailer_trailer_checksum(stored + start, checksum_at - start, (uint32) len) != checksum)
		{
			CHUNKS_TRAILER_CORRUPT("chunk trailer does not match its checksum");
			goto done;
		}
		std_range_to_check(start, stored_len, RUST_FILE, 610, 41);
		hashed.ptr = stored;
		hashed.len = start;
		trailer_at = STD_ADD(usize, start, 1, 610, 60);
		std_range_check(trailer_at, checksum_at, stored_len, RUST_FILE, 610, 59);
		trailer.ptr = stored + trailer_at;
		trailer.len = checksum_at - trailer_at;
		std_index_check(start, stored_len, RUST_FILE, 611, 21);
		flags = stored[start];
		if ((flags & CHUNKS_TRAILER_HAS_LOCATORS) != 0)
		{
			size_t		count;

			addresses_live = false;
			if (!addresses(addresses_captures, hashed.ptr, hashed.len, &count, error))
				goto done;
			locators_len = STD_MUL(usize, count, CHUNKS_TRAILER_LOCATOR_LEN,
								   612, 59);
		}
		else
			locators_len = 0;
		sections = flags & (CHUNKS_TRAILER_HAS_STAMPS | CHUNKS_TRAILER_HAS_MAX_TUPLES);
		if (locators_len > trailer.len || (sections == 0 && locators_len != trailer.len))
		{
			CHUNKS_TRAILER_CORRUPT("chunk trailer has the wrong length");
			goto done;
		}
		if (sections == (CHUNKS_TRAILER_HAS_STAMPS | CHUNKS_TRAILER_HAS_MAX_TUPLES))
		{
			CHUNKS_TRAILER_CORRUPT("chunk trailer has both row stamps and subtree tuple ids");
			goto done;
		}
		values = (flags & CHUNKS_TRAILER_HAS_VALUES) != 0;
		if (values && sections != CHUNKS_TRAILER_HAS_STAMPS)
		{
			CHUNKS_TRAILER_CORRUPT("chunk trailer has value ids without row stamps");
			goto done;
		}
		std_range_from_check(locators_len, trailer.len, RUST_FILE, 624, 39);
		result->stamps.ptr = trailer.ptr + locators_len;
		result->stamps.len = trailer.len - locators_len;
		result->runs = (flags & CHUNKS_TRAILER_HAS_STAMP_RUNS) != 0;
		result->max_tuples = (flags & CHUNKS_TRAILER_HAS_MAX_TUPLES) != 0;
		std_range_to_check(locators_len, trailer.len, RUST_FILE, 626, 47);
		result->hashed = hashed;
		result->locators.ptr = trailer.ptr;
		result->locators.len = locators_len;
		result->values = values;
		ok = true;
done:
		if (addresses_live && addresses_drop != NULL)
			addresses_drop(addresses_captures);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (addresses_live && addresses_drop != NULL)
			addresses_drop(addresses_captures);
		std_unwind_resume(&unwind);
	}
	return ok;
}

/*
 * chunks_trailer_Stored_locator returns the locator of the chunk's address at
 * the position, which is unknown when the trailer has none.
 */
ChunksTrailerLocator
chunks_trailer_Stored_locator(const ChunksTrailerStored *self, size_t i)
{
	size_t		from = STD_MUL(usize, i, CHUNKS_TRAILER_LOCATOR_LEN, 632, 27);
	size_t		to = STD_MUL(usize, STD_ADD(usize, i, 1, 632, 45), CHUNKS_TRAILER_LOCATOR_LEN,
							 632, 44);
	ChunksTrailerLocator unknown = {0};

	if (from <= to && to <= self->locators.len)
		return chunks_trailer_Locator_read(self->locators.ptr + from, to - from);
	return unknown;
}

/*
 * chunks_trailer_Stored_trailer returns the chunk's locators and stamps, its
 * value ids section among them, as its trailer stores them, with how the
 * stamps read (1 for runs, 2 for an internal tree node's subtrees' highest
 * tuple ids, 4 for stamps followed by value ids).
 */
void
chunks_trailer_Stored_trailer(const ChunksTrailerStored *self, StdBytes *out_0, StdBytes *out_1, uint8 *out_2)
{
	*out_0 = self->locators;
	*out_1 = self->stamps;
	*out_2 = (uint8) self->runs | (uint8) ((uint8) self->max_tuples << 1) | (uint8) ((uint8) self->values << 2);
}

/* chunks_trailer_Stored_of_trailer returns the split of a chunk whose trailer held what trailer returned. */
ChunksTrailerStored
chunks_trailer_Stored_of_trailer(StdBytes hashed, StdBytes locators, StdBytes stamps, uint8 reads)
{
	ChunksTrailerStored result;

	result.hashed = hashed;
	result.locators = locators;
	result.stamps = stamps;
	result.runs = (reads & 1) != 0;
	result.max_tuples = (reads & 2) != 0;
	result.values = (reads & 4) != 0;
	return result;
}

/*
 * chunks_trailer_Stored_stamps returns the row stamps of the chunk's
 * entries, or an internal tree node's subtrees' highest tuple ids.
 */
bool
chunks_trailer_Stored_stamps(const ChunksTrailerStored *self, ChunksTrailerStamps *result, ChunksErrorError *error)
{
	StdVec		max_tuples;
	chunks_trailer_subtree_maxes_error_t malformed;

	if (!self->max_tuples)
		return chunks_trailer_Stamps_read(self->stamps.ptr, self->stamps.len, self->runs, self->values, result, error);
	if (chunks_trailer_subtree_maxes(self->stamps.ptr, self->stamps.len, &max_tuples, &malformed))
	{
		*result = chunks_trailer_Stamps_subtrees(&max_tuples);
		return true;
	}
	if (malformed.has)
		*error = chunks_varint_Malformed_error(malformed.x);
	else
		CHUNKS_TRAILER_CORRUPT("chunk trailer's subtree tuple ids are malformed");
	return false;
}

#ifdef DOLTGRES_TEST_CHUNKS

#include "doltgres/std/fmt.h"
#include "doltgres/std/test_fs.h"
#include "doltgres_chunks_testdata.h"
#include "xxhash.h"

/* test_chunks_trailer_tests_round_trips_closure_0 is round_trips' |_| Ok(locators.len()): the captured count. */
static bool
test_chunks_trailer_tests_round_trips_closure_0(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
												ChunksErrorError *error)
{
	*result = *(const size_t *) captures;
	return true;
}

/* test_chunks_trailer_tests_round_trips_closure_1 is round_trips' |_| Ok(1). */
static bool
test_chunks_trailer_tests_round_trips_closure_1(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
												ChunksErrorError *error)
{
	*result = 1;
	return true;
}

/*
 * test_chunks_trailer_tests_round_trips: a chunk's locators and stamps, alone
 * and together, read back as they were stored, and unknown locators alone
 * store no trailer.
 */
void
test_chunks_trailer_tests_round_trips(void)
{
	volatile ChunksTrailerStamps stamps;
	volatile bool stamps_live = false;
	volatile ChunksTrailerStamps none;
	volatile bool none_live = false;
	volatile StdVec stored;
	volatile bool stored_live = false;
	volatile ChunksTrailerStamps read;
	volatile bool read_live = false;
	volatile StdVec bare;
	volatile bool bare_live = false;
	volatile ChunksErrorError error;
	volatile bool error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 hashed_bytes[] = {'h', 'a', 's', 'h', 'e', 'd', ' ', 'b', 'y', 't', 'e', 's'};
		static const uint8 x[] = {'x'};
		ChunksTrailerStamps *stamps_v = (ChunksTrailerStamps *) &stamps;
		ChunksTrailerStamps *none_v = (ChunksTrailerStamps *) &none;
		StdVec	   *stored_v = (StdVec *) &stored;
		ChunksTrailerStamps *read_v = (ChunksTrailerStamps *) &read;
		StdVec	   *bare_v = (StdVec *) &bare;
		ChunksTrailerLocator locators[2];
		ChunksTrailerLocator unknown[1] = {{0}};
		ChunksTrailerStamp *boxed;
		StdVec		single;
		const ChunksTrailerLocator *case_locators[3];
		size_t		case_locators_len[3];
		const ChunksTrailerStamps *case_stamps[3];
		ChunksTrailerStored split;
		ChunksTrailerLocator located;
		StdBytes	expected = {hashed_bytes, sizeof(hashed_bytes)};
		size_t		bare_len;
		size_t		expected_len = 5;

		locators[0] = chunks_trailer_Locator_at(3, STD_SHL_FOLDED(u64, 1, 40) - 1,
												(uint32) (STD_SHL_FOLDED(u32, 1, 24) - 1));
		memset(&locators[1], 0, sizeof(locators[1]));
		boxed = std_box_new_uninit_layout_Checked(2 * sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		STD_CHECK_ALIGNED(boxed, _Alignof(ChunksTrailerStamp), 668, 35);
		STD_CHECK_NULL_DEREF(boxed, 668, 35);
		boxed[0].txn = PG_UINT64_MAX;
		boxed[0].tuple = STD_SHL_FOLDED(u64, 1, 48) - 1;
		boxed[1].txn = 7;
		boxed[1].tuple = 9;
		std_box_slice_into_vec_Checked(&single, boxed, 2, sizeof(ChunksTrailerStamp));
		*stamps_v = chunks_trailer_Stamps_FromVecStamp_from(&single);
		stamps_live = true;
		*none_v = chunks_trailer_Stamps_Default_default();
		none_live = true;
		case_locators[0] = locators;
		case_locators_len[0] = 2;
		case_stamps[0] = stamps_v;
		case_locators[1] = locators;
		case_locators_len[1] = 2;
		case_stamps[1] = none_v;
		case_locators[2] = (const ChunksTrailerLocator *) STD_DANGLING;
		case_locators_len[2] = 0;
		case_stamps[2] = stamps_v;
		for (size_t start = 0, n; std_array_IntoIter_next(&start, 3, 3, &n);)
		{
			const ChunksTrailerLocator *case_locator = case_locators[n];
			size_t		locators_len = case_locators_len[n];
			const ChunksTrailerStamps *case_stamp = case_stamps[n];

			chunks_trailer_store(hashed_bytes, sizeof(hashed_bytes), case_locator, locators_len, case_stamp, stored_v);
			stored_live = true;
			if (!chunks_trailer_Stored_split(stored_v->ptr, stored_v->len, test_chunks_trailer_tests_round_trips_closure_0,
											 &locators_len, NULL, &split, (ChunksErrorError *) &error))
			{
				error_live = true;
				std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
										 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
										 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 672, 72);
			}
			STD_ASSERT_EQ(673, 13,
						  split.hashed.len == expected.len && memcmp(split.hashed.ptr, expected.ptr, expected.len) == 0,
						  &split.hashed, std_bytes_Debug_fmt, &expected, std_bytes_Debug_fmt);
			for (size_t i = 0; i < locators_len; i++)
			{
				located = chunks_trailer_Stored_locator(&split, i);
				STD_ASSERT_EQ(675, 17, chunks_trailer_Locator_PartialEq_eq(&located, &case_locator[i]), &located,
							  chunks_trailer_Locator_Debug_fmt, &case_locator[i], chunks_trailer_Locator_Debug_fmt);
			}
			if (!chunks_trailer_Stored_stamps(&split, read_v, (ChunksErrorError *) &error))
			{
				error_live = true;
				std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
										 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
										 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 677, 39);
			}
			read_live = true;
			STD_ASSERT_EQ(677, 13, chunks_trailer_Stamps_PartialEq_eq(read_v, case_stamp), read_v,
						  chunks_trailer_Stamps_Debug_fmt, case_stamp, chunks_trailer_Stamps_Debug_fmt);
			read_live = false;
			chunks_trailer_Stamps_drop(read_v);
			stored_live = false;
			std_vec_free_Checked(stored_v, 1, 1);
		}
		chunks_trailer_store(x, sizeof(x), unknown, 1, none_v, bare_v);
		bare_live = true;
		bare_len = bare_v->len;
		STD_ASSERT_EQ(680, 9, bare_len == expected_len, &bare_len, std_usize_Debug_fmt, &expected_len,
					  std_usize_Debug_fmt);
		if (!chunks_trailer_Stored_split(bare_v->ptr, bare_v->len, test_chunks_trailer_tests_round_trips_closure_1,
										 NULL, NULL, &split, (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 681, 50);
		}
		located = chunks_trailer_Stored_locator(&split, 0);
		STD_ASSERT(681, 9, !chunks_trailer_Locator_is_known(&located),
				   "!Stored::split(&bare, |_| Ok(1)).unwrap().locator(0).is_known()");
		bare_live = false;
		std_vec_free_Checked(bare_v, 1, 1);
		none_live = false;
		chunks_trailer_Stamps_drop(none_v);
		stamps_live = false;
		chunks_trailer_Stamps_drop(stamps_v);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (error_live)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		if (read_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &read);
		if (stored_live)
			std_vec_free_Checked((StdVec *) &stored, 1, 1);
		if (bare_live)
			std_vec_free_Checked((StdVec *) &bare, 1, 1);
		if (none_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &none);
		if (stamps_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &stamps);
		std_unwind_resume(&unwind);
	}
}

/* chunks_trailer_tests_copies returns the stamps of a run of copies one transaction inserted. */
static void
chunks_trailer_tests_copies(uint64 txn, uint64 first, uint64 count, StdVec *result)
{
	uint64		end = STD_ADD(u64, first, count, 686, 17);
	uint64		n = end > first ? end - first : 0;
	StdSizeHint hint = {(size_t) n, true, (size_t) n};

	std_vec_with_capacity_trusted(result, hint, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	std_vec_extend_trusted_reserve(result, hint, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	for (uint64 tuple = first, tuple_end = end; tuple < tuple_end; tuple++)
	{
		ChunksTrailerStamp *stamp = &STD_VEC_AT(result, ChunksTrailerStamp, result->len);

		stamp->txn = txn;
		stamp->tuple = tuple;
		result->len++;
	}
}

/* chunks_trailer_tests_VecU8_PartialEq_eq is Vec<u8>'s PartialEq: the same bytes. */
static bool
chunks_trailer_tests_VecU8_PartialEq_eq(const StdVec *a, const StdVec *b)
{
	return a->len == b->len && memcmp(a->ptr, b->ptr, a->len) == 0;
}

/* test_chunks_trailer_tests_entries_with_many_stamps_store_maximal_runs_closure_0 is |_| Ok(0). */
static bool
test_chunks_trailer_tests_entries_with_many_stamps_store_maximal_runs_closure_0(void *captures, const uint8 *hashed,
																				size_t hashed_len, size_t *result,
																				ChunksErrorError *error)
{
	*result = 0;
	return true;
}

/*
 * The Vec<Stamp>s of entries_with_many_stamps_store_maximal_runs' statements:
 * copies' result, an entry read back, the three runs of an array, and their
 * concatenation.
 */
enum
{
	MAXIMAL_TEST_COPIES,
	MAXIMAL_TEST_ENTRY,
	MAXIMAL_TEST_PART,
	MAXIMAL_TEST_CONCAT = MAXIMAL_TEST_PART + 3,
	MAXIMAL_TEST_VECS,
};

/* MAXIMAL_TEST_GOLDEN_PARTS is the number of the golden bytes' parts. */
#define MAXIMAL_TEST_GOLDEN_PARTS 7

/*
 * test_chunks_trailer_tests_entries_with_many_stamps_store_maximal_runs:
 * entries with other than one stamp store maximal runs of one transaction's
 * consecutive tuple ids, with the bytes a golden trailer has, and read back
 * each entry's stamps.
 */
void
test_chunks_trailer_tests_entries_with_many_stamps_store_maximal_runs(void)
{
	volatile StdVec vecs[MAXIMAL_TEST_VECS];
	volatile bool vecs_live[MAXIMAL_TEST_VECS] = {0};
	volatile ChunksTrailerStamps stamps;
	volatile bool stamps_live = false;
	volatile StdVec stored;
	volatile bool stored_live = false;
	volatile StdVec parts[MAXIMAL_TEST_GOLDEN_PARTS];
	volatile bool parts_live[MAXIMAL_TEST_GOLDEN_PARTS] = {0};
	volatile StdVec golden;
	volatile bool golden_live = false;
	volatile ChunksTrailerStamps read;
	volatile bool read_live = false;
	volatile ChunksTrailerStamps from[2];
	volatile bool from_live[2] = {0};
	volatile ChunksTrailerStamps copied;
	volatile bool copied_live = false;
	volatile StdVec keyed;
	volatile bool keyed_live = false;
	volatile ChunksErrorError error;
	volatile bool error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 h[] = {'h'};
		static const uint8 flags[] = {CHUNKS_TRAILER_HAS_STAMPS | CHUNKS_TRAILER_HAS_STAMP_RUNS, 3, 1, 6, 1};
		static const uint8 run_0[] = {5, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0, 4};
		static const uint8 run_1[] = {6, 0, 0, 0, 0, 0, 0, 0, 14, 0, 0, 0, 0, 0, 1};
		static const uint8 run_2[] = {5, 0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0, 0, 0, 3};
		static const uint8 checksum[] = {198, 246, 182, 77, 63, 175, 210, 204};
		static const uint8 *const literals[] = {flags, run_0, run_1, run_2, checksum};
		static const size_t literal_lens[] = {sizeof(flags), sizeof(run_0), sizeof(run_1), sizeof(run_2),
		sizeof(checksum)};
		static const uint32 literal_lines[] = {698, 699, 700, 701, 702};
		StdVec	   *v = (StdVec *) vecs;
		ChunksTrailerStamps *stamps_v = (ChunksTrailerStamps *) &stamps;
		StdVec	   *stored_v = (StdVec *) &stored;
		StdVec	   *parts_v = (StdVec *) parts;
		StdVec	   *golden_v = (StdVec *) &golden;
		ChunksTrailerStamps *read_v = (ChunksTrailerStamps *) &read;
		ChunksTrailerStamps *from_v = (ChunksTrailerStamps *) from;
		ChunksTrailerStamps *copied_v = (ChunksTrailerStamps *) &copied;
		StdVec	   *keyed_v = (StdVec *) &keyed;
		ChunksTrailerStored split;
		uint8		len_bytes[4];
		uint64		entry_len;
		ChunksTrailerStamp first;
		bool		has_first;
		uint64		expected_entry_len = 6;
		ChunksTrailerStamp expected_first = {5, 11};
		StdOptionRef first_ref;
		StdOptionRef expected_first_ref = {true, {&expected_first, chunks_trailer_Stamp_Debug_fmt}};
		uint64		max_tuple;
		uint64		expected_max_tuple = 22;
		bool		holds;
		size_t		keyed_len;
		size_t		expected_keyed_len = 1 + 1 + 1000 * CHUNKS_TRAILER_STAMP_LEN + CHUNKS_TRAILER_CHECKSUM_LEN + 4;

		*stamps_v = chunks_trailer_Stamps_Default_default();
		stamps_live = true;
		chunks_trailer_tests_copies(5, 10, 1, &v[MAXIMAL_TEST_COPIES]);
		vecs_live[MAXIMAL_TEST_COPIES] = true;
		chunks_trailer_Stamps_push(stamps_v, v[MAXIMAL_TEST_COPIES].ptr, v[MAXIMAL_TEST_COPIES].len);
		vecs_live[MAXIMAL_TEST_COPIES] = false;
		std_vec_free_Checked(&v[MAXIMAL_TEST_COPIES], sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		chunks_trailer_tests_copies(5, 11, 3, &v[MAXIMAL_TEST_PART]);
		vecs_live[MAXIMAL_TEST_PART] = true;
		chunks_trailer_tests_copies(6, 14, 1, &v[MAXIMAL_TEST_PART + 1]);
		vecs_live[MAXIMAL_TEST_PART + 1] = true;
		chunks_trailer_tests_copies(5, 20, 2, &v[MAXIMAL_TEST_PART + 2]);
		vecs_live[MAXIMAL_TEST_PART + 2] = true;
		std_slice_concat(&v[MAXIMAL_TEST_PART], 3, sizeof(StdVec), sizeof(ChunksTrailerStamp),
						 _Alignof(ChunksTrailerStamp), &v[MAXIMAL_TEST_CONCAT]);
		vecs_live[MAXIMAL_TEST_CONCAT] = true;
		chunks_trailer_Stamps_push(stamps_v, v[MAXIMAL_TEST_CONCAT].ptr, v[MAXIMAL_TEST_CONCAT].len);
		vecs_live[MAXIMAL_TEST_CONCAT] = false;
		std_vec_free_Checked(&v[MAXIMAL_TEST_CONCAT], sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		for (int i = 0; i < 3; i++)
		{
			vecs_live[MAXIMAL_TEST_PART + i] = false;
			std_vec_free_Checked(&v[MAXIMAL_TEST_PART + i], sizeof(ChunksTrailerStamp),
								 _Alignof(ChunksTrailerStamp));
		}
		chunks_trailer_tests_copies(5, 22, 1, &v[MAXIMAL_TEST_COPIES]);
		vecs_live[MAXIMAL_TEST_COPIES] = true;
		chunks_trailer_Stamps_push(stamps_v, v[MAXIMAL_TEST_COPIES].ptr, v[MAXIMAL_TEST_COPIES].len);
		vecs_live[MAXIMAL_TEST_COPIES] = false;
		std_vec_free_Checked(&v[MAXIMAL_TEST_COPIES], sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		chunks_trailer_store(h, sizeof(h), (const ChunksTrailerLocator *) STD_DANGLING, 0, stamps_v, stored_v);
		stored_live = true;
		std_slice_to_vec_Checked(&parts_v[0], h, sizeof(h), 1, 1);
		parts_live[0] = true;
		for (int i = 0; i < 5; i++)
		{
			uint8	   *boxed = std_box_new_uninit_layout_Checked(literal_lens[i], 1);

			STD_CHECK_ALIGNED(boxed, 1, literal_lines[i], 13);
			STD_CHECK_NULL_DEREF(boxed, literal_lines[i], 13);
			memcpy(boxed, literals[i], literal_lens[i]);
			std_box_slice_into_vec_Checked(&parts_v[1 + i], boxed, literal_lens[i], 1);
			parts_live[1 + i] = true;
		}
		std_u32_to_le_bytes(58, len_bytes);
		std_slice_to_vec_Checked(&parts_v[6], len_bytes, sizeof(len_bytes), 1, 1);
		parts_live[6] = true;
		std_slice_concat(parts_v, MAXIMAL_TEST_GOLDEN_PARTS, sizeof(StdVec), 1, 1, golden_v);
		golden_live = true;
		for (int i = 0; i < MAXIMAL_TEST_GOLDEN_PARTS; i++)
		{
			parts_live[i] = false;
			std_vec_free_Checked(&parts_v[i], 1, 1);
		}
		STD_ASSERT_EQ(706, 9, chunks_trailer_tests_VecU8_PartialEq_eq(stored_v, golden_v), stored_v,
					  chunks_chunk_VecU8_Debug_fmt, golden_v, chunks_chunk_VecU8_Debug_fmt);
		if (!chunks_trailer_Stored_split(stored_v->ptr, stored_v->len,
										 test_chunks_trailer_tests_entries_with_many_stamps_store_maximal_runs_closure_0,
										 NULL, NULL, &split, (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 707, 54);
		}
		if (!chunks_trailer_Stored_stamps(&split, read_v, (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 707, 63);
		}
		read_live = true;
		STD_ASSERT_EQ(708, 9, chunks_trailer_Stamps_PartialEq_eq(read_v, stamps_v), read_v,
					  chunks_trailer_Stamps_Debug_fmt, stamps_v, chunks_trailer_Stamps_Debug_fmt);
		chunks_trailer_Stamps_entry(read_v, 1, &v[MAXIMAL_TEST_ENTRY]);
		vecs_live[MAXIMAL_TEST_ENTRY] = true;
		chunks_trailer_tests_copies(5, 11, 3, &v[MAXIMAL_TEST_PART]);
		vecs_live[MAXIMAL_TEST_PART] = true;
		chunks_trailer_tests_copies(6, 14, 1, &v[MAXIMAL_TEST_PART + 1]);
		vecs_live[MAXIMAL_TEST_PART + 1] = true;
		chunks_trailer_tests_copies(5, 20, 2, &v[MAXIMAL_TEST_PART + 2]);
		vecs_live[MAXIMAL_TEST_PART + 2] = true;
		std_slice_concat(&v[MAXIMAL_TEST_PART], 3, sizeof(StdVec), sizeof(ChunksTrailerStamp),
						 _Alignof(ChunksTrailerStamp), &v[MAXIMAL_TEST_CONCAT]);
		vecs_live[MAXIMAL_TEST_CONCAT] = true;
		STD_ASSERT_EQ(709, 9, chunks_trailer_VecStamp_PartialEq_eq(&v[MAXIMAL_TEST_ENTRY], &v[MAXIMAL_TEST_CONCAT]),
					  &v[MAXIMAL_TEST_ENTRY], chunks_trailer_VecStamp_Debug_fmt, &v[MAXIMAL_TEST_CONCAT],
					  chunks_trailer_VecStamp_Debug_fmt);
		vecs_live[MAXIMAL_TEST_CONCAT] = false;
		std_vec_free_Checked(&v[MAXIMAL_TEST_CONCAT], sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		for (int i = 0; i < 3; i++)
		{
			vecs_live[MAXIMAL_TEST_PART + i] = false;
			std_vec_free_Checked(&v[MAXIMAL_TEST_PART + i], sizeof(ChunksTrailerStamp),
								 _Alignof(ChunksTrailerStamp));
		}
		vecs_live[MAXIMAL_TEST_ENTRY] = false;
		std_vec_free_Checked(&v[MAXIMAL_TEST_ENTRY], sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		entry_len = chunks_trailer_Stamps_entry_len(read_v, 1);
		has_first = chunks_trailer_Stamps_first(read_v, 1, &first);
		first_ref.has = has_first;
		first_ref.some = STD_ITEM(&first, chunks_trailer_Stamp_Debug_fmt);
		STD_ASSERT_EQ(710, 9,
					  entry_len == expected_entry_len && has_first &&
					  chunks_trailer_Stamp_PartialEq_eq(&first, &expected_first),
					  &STD_TUPLE(2, STD_ITEM(&entry_len, std_u64_Debug_fmt), STD_ITEM(&first_ref, std_option_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&expected_entry_len, std_u64_Debug_fmt),
								 STD_ITEM(&expected_first_ref, std_option_Debug_fmt)),
					  std_tuple_Debug_fmt);
		chunks_trailer_Stamps_entry(read_v, 2, &v[MAXIMAL_TEST_ENTRY]);
		vecs_live[MAXIMAL_TEST_ENTRY] = true;
		chunks_trailer_tests_copies(5, 22, 1, &v[MAXIMAL_TEST_COPIES]);
		vecs_live[MAXIMAL_TEST_COPIES] = true;
		STD_ASSERT_EQ(711, 9, chunks_trailer_VecStamp_PartialEq_eq(&v[MAXIMAL_TEST_ENTRY], &v[MAXIMAL_TEST_COPIES]),
					  &v[MAXIMAL_TEST_ENTRY], chunks_trailer_VecStamp_Debug_fmt, &v[MAXIMAL_TEST_COPIES],
					  chunks_trailer_VecStamp_Debug_fmt);
		vecs_live[MAXIMAL_TEST_COPIES] = false;
		std_vec_free_Checked(&v[MAXIMAL_TEST_COPIES], sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		vecs_live[MAXIMAL_TEST_ENTRY] = false;
		std_vec_free_Checked(&v[MAXIMAL_TEST_ENTRY], sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		max_tuple = chunks_trailer_Stamps_max_tuple(read_v);
		STD_ASSERT_EQ(712, 9, max_tuple == expected_max_tuple, &max_tuple, std_u64_Debug_fmt, &expected_max_tuple,
					  std_u64_Debug_fmt);
		STD_ASSERT(713, 9,
				   chunks_trailer_Stamps_holds(read_v, 1, 11) && chunks_trailer_Stamps_holds(read_v, 1, 13) &&
				   chunks_trailer_Stamps_holds(read_v, 1, 14) && chunks_trailer_Stamps_holds(read_v, 1, 21),
				   "read.holds(1, 11) && read.holds(1, 13) && read.holds(1, 14) && read.holds(1, 21)");
		STD_ASSERT(714, 9,
				   !chunks_trailer_Stamps_holds(read_v, 1, 10) && !chunks_trailer_Stamps_holds(read_v, 1, 15) &&
				   !chunks_trailer_Stamps_holds(read_v, 1, 22) && !chunks_trailer_Stamps_holds(read_v, 0, 11),
				   "!read.holds(1, 10) && !read.holds(1, 15) && !read.holds(1, 22) && !read.holds(0, 11)");
		STD_ASSERT(715, 9,
				   chunks_trailer_Stamps_holds(read_v, 0, 10) && chunks_trailer_Stamps_holds(read_v, 2, 22) &&
				   !chunks_trailer_Stamps_holds(read_v, 3, 22),
				   "read.holds(0, 10) && read.holds(2, 22) && !read.holds(3, 22)");
		chunks_trailer_tests_copies(9, 1, 3, &v[MAXIMAL_TEST_COPIES]);
		vecs_live[MAXIMAL_TEST_COPIES] = true;
		vecs_live[MAXIMAL_TEST_COPIES] = false;
		from_v[0] = chunks_trailer_Stamps_FromVecStamp_from(&v[MAXIMAL_TEST_COPIES]);
		from_live[0] = true;
		holds = chunks_trailer_Stamps_holds(&from_v[0], 1, 2);
		if (holds)
		{
			chunks_trailer_tests_copies(9, 1, 3, &v[MAXIMAL_TEST_COPIES]);
			vecs_live[MAXIMAL_TEST_COPIES] = true;
			vecs_live[MAXIMAL_TEST_COPIES] = false;
			from_v[1] = chunks_trailer_Stamps_FromVecStamp_from(&v[MAXIMAL_TEST_COPIES]);
			from_live[1] = true;
			holds = !chunks_trailer_Stamps_holds(&from_v[1], 1, 3);
			from_live[1] = false;
			chunks_trailer_Stamps_drop(&from_v[1]);
		}
		from_live[0] = false;
		chunks_trailer_Stamps_drop(&from_v[0]);
		STD_ASSERT(716, 9, holds,
				   "Stamps::from(copies(9, 1, 3)).holds(1, 2) && !Stamps::from(copies(9, 1, 3)).holds(1, 3)");
		*copied_v = chunks_trailer_Stamps_Default_default();
		copied_live = true;
		for (size_t i = 0, i_end = 3; i < i_end; i++)
			chunks_trailer_Stamps_push_entry_of(copied_v, read_v, i);
		STD_ASSERT_EQ(719, 9, chunks_trailer_Stamps_PartialEq_eq(copied_v, stamps_v), copied_v,
					  chunks_trailer_Stamps_Debug_fmt, stamps_v, chunks_trailer_Stamps_Debug_fmt);
		chunks_trailer_tests_copies(9, 1, 1000, &v[MAXIMAL_TEST_COPIES]);
		vecs_live[MAXIMAL_TEST_COPIES] = true;
		vecs_live[MAXIMAL_TEST_COPIES] = false;
		from_v[0] = chunks_trailer_Stamps_FromVecStamp_from(&v[MAXIMAL_TEST_COPIES]);
		from_live[0] = true;
		chunks_trailer_store(h, sizeof(h), (const ChunksTrailerLocator *) STD_DANGLING, 0, &from_v[0], keyed_v);
		keyed_live = true;
		from_live[0] = false;
		chunks_trailer_Stamps_drop(&from_v[0]);
		keyed_len = keyed_v->len;
		STD_ASSERT_EQ(721, 9, keyed_len == expected_keyed_len, &keyed_len, std_usize_Debug_fmt, &expected_keyed_len,
					  std_usize_Debug_fmt);
		keyed_live = false;
		std_vec_free_Checked(keyed_v, 1, 1);
		copied_live = false;
		chunks_trailer_Stamps_drop(copied_v);
		read_live = false;
		chunks_trailer_Stamps_drop(read_v);
		golden_live = false;
		std_vec_free_Checked(golden_v, 1, 1);
		stored_live = false;
		std_vec_free_Checked(stored_v, 1, 1);
		stamps_live = false;
		chunks_trailer_Stamps_drop(stamps_v);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (error_live)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		if (vecs_live[MAXIMAL_TEST_CONCAT])
			std_vec_free_Checked((StdVec *) &vecs[MAXIMAL_TEST_CONCAT], sizeof(ChunksTrailerStamp),
								 _Alignof(ChunksTrailerStamp));
		for (int i = 0; i < 3; i++)
		{
			if (vecs_live[MAXIMAL_TEST_PART + i])
				std_vec_free_Checked((StdVec *) &vecs[MAXIMAL_TEST_PART + i], sizeof(ChunksTrailerStamp),
									 _Alignof(ChunksTrailerStamp));
		}
		if (vecs_live[MAXIMAL_TEST_COPIES])
			std_vec_free_Checked((StdVec *) &vecs[MAXIMAL_TEST_COPIES], sizeof(ChunksTrailerStamp),
								 _Alignof(ChunksTrailerStamp));
		if (vecs_live[MAXIMAL_TEST_ENTRY])
			std_vec_free_Checked((StdVec *) &vecs[MAXIMAL_TEST_ENTRY], sizeof(ChunksTrailerStamp),
								 _Alignof(ChunksTrailerStamp));
		for (int i = 1; i >= 0; i--)
		{
			if (from_live[i])
				chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &from[i]);
		}
		for (int i = 0; i < MAXIMAL_TEST_GOLDEN_PARTS; i++)
		{
			if (parts_live[i])
				std_vec_free_Checked((StdVec *) &parts[i], 1, 1);
		}
		if (keyed_live)
			std_vec_free_Checked((StdVec *) &keyed, 1, 1);
		if (copied_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &copied);
		if (read_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &read);
		if (golden_live)
			std_vec_free_Checked((StdVec *) &golden, 1, 1);
		if (stored_live)
			std_vec_free_Checked((StdVec *) &stored, 1, 1);
		if (stamps_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &stamps);
		std_unwind_resume(&unwind);
	}
}

/*
 * test_chunks_trailer_tests_the_same_stamps_store_the_same_bytes_however_they_were_pushed:
 * stamps pushed whole, in parts, entry by entry, or from a vector store the
 * same bytes.
 */
void
test_chunks_trailer_tests_the_same_stamps_store_the_same_bytes_however_they_were_pushed(void)
{
	volatile StdVec copied[3];
	volatile bool copied_live[3] = {0};
	volatile StdVec all;
	volatile bool all_live = false;
	volatile ChunksTrailerStamps whole;
	volatile bool whole_live = false;
	volatile ChunksTrailerStamps parts;
	volatile bool parts_live = false;
	volatile StdVec left;
	volatile bool left_live = false;
	volatile StdVec right;
	volatile bool right_live = false;
	volatile StdVec keyed;
	volatile bool keyed_live = false;
	volatile ChunksTrailerStamps pushed;
	volatile bool pushed_live = false;
	volatile ChunksTrailerStamps from;
	volatile bool from_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 h[] = {'h'};
		StdVec	   *copied_v = (StdVec *) copied;
		StdVec	   *all_v = (StdVec *) &all;
		ChunksTrailerStamps *whole_v = (ChunksTrailerStamps *) &whole;
		ChunksTrailerStamps *parts_v = (ChunksTrailerStamps *) &parts;
		StdVec	   *left_v = (StdVec *) &left;
		StdVec	   *right_v = (StdVec *) &right;
		StdVec	   *keyed_v = (StdVec *) &keyed;
		ChunksTrailerStamps *pushed_v = (ChunksTrailerStamps *) &pushed;
		ChunksTrailerStamps *from_v = (ChunksTrailerStamps *) &from;

		chunks_trailer_tests_copies(3, 1, 2, &copied_v[0]);
		copied_live[0] = true;
		chunks_trailer_tests_copies(3, 3, 1, &copied_v[1]);
		copied_live[1] = true;
		chunks_trailer_tests_copies(4, 7, 2, &copied_v[2]);
		copied_live[2] = true;
		std_slice_concat(copied_v, 3, sizeof(StdVec), sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp), all_v);
		all_live = true;
		for (int i = 0; i < 3; i++)
		{
			copied_live[i] = false;
			std_vec_free_Checked(&copied_v[i], sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		}
		*whole_v = chunks_trailer_Stamps_Default_default();
		whole_live = true;
		std_range_to_check(3, all_v->len, RUST_FILE, 728, 24);
		chunks_trailer_Stamps_push(whole_v, all_v->ptr, 3);
		std_range_from_check(3, all_v->len, RUST_FILE, 729, 24);
		chunks_trailer_Stamps_push(whole_v, &STD_VEC_AT(all_v, ChunksTrailerStamp, 3), all_v->len - 3);
		*parts_v = chunks_trailer_Stamps_Default_default();
		parts_live = true;
		std_range_to_check(3, all_v->len, RUST_FILE, 731, 24);
		chunks_trailer_Stamps_push(parts_v, all_v->ptr, 3);
		chunks_trailer_Stamps_push_entry_of(parts_v, whole_v, 1);
		chunks_trailer_store(h, sizeof(h), (const ChunksTrailerLocator *) STD_DANGLING, 0, whole_v, left_v);
		left_live = true;
		chunks_trailer_store(h, sizeof(h), (const ChunksTrailerLocator *) STD_DANGLING, 0, parts_v, right_v);
		right_live = true;
		STD_ASSERT_EQ(733, 9, chunks_trailer_tests_VecU8_PartialEq_eq(left_v, right_v), left_v,
					  chunks_chunk_VecU8_Debug_fmt, right_v, chunks_chunk_VecU8_Debug_fmt);
		right_live = false;
		std_vec_free_Checked(right_v, 1, 1);
		left_live = false;
		std_vec_free_Checked(left_v, 1, 1);
		chunks_trailer_tests_copies(3, 1, 4, keyed_v);
		keyed_live = true;
		*pushed_v = chunks_trailer_Stamps_Default_default();
		pushed_live = true;
		for (size_t i = 0, i_len = keyed_v->len; i < i_len; i++)
			chunks_trailer_Stamps_push(pushed_v, &STD_VEC_AT(keyed_v, ChunksTrailerStamp, i), 1);
		chunks_trailer_store(h, sizeof(h), (const ChunksTrailerLocator *) STD_DANGLING, 0, pushed_v, left_v);
		left_live = true;
		keyed_live = false;
		*from_v = chunks_trailer_Stamps_FromVecStamp_from(keyed_v);
		from_live = true;
		chunks_trailer_store(h, sizeof(h), (const ChunksTrailerLocator *) STD_DANGLING, 0, from_v, right_v);
		right_live = true;
		STD_ASSERT_EQ(737, 9, chunks_trailer_tests_VecU8_PartialEq_eq(left_v, right_v), left_v,
					  chunks_chunk_VecU8_Debug_fmt, right_v, chunks_chunk_VecU8_Debug_fmt);
		right_live = false;
		std_vec_free_Checked(right_v, 1, 1);
		from_live = false;
		chunks_trailer_Stamps_drop(from_v);
		left_live = false;
		std_vec_free_Checked(left_v, 1, 1);
		pushed_live = false;
		chunks_trailer_Stamps_drop(pushed_v);
		parts_live = false;
		chunks_trailer_Stamps_drop(parts_v);
		whole_live = false;
		chunks_trailer_Stamps_drop(whole_v);
		all_live = false;
		std_vec_free_Checked(all_v, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (right_live)
			std_vec_free_Checked((StdVec *) &right, 1, 1);
		if (from_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &from);
		if (left_live)
			std_vec_free_Checked((StdVec *) &left, 1, 1);
		for (int i = 0; i < 3; i++)
		{
			if (copied_live[i])
				std_vec_free_Checked((StdVec *) &copied[i], sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		}
		if (pushed_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &pushed);
		if (keyed_live)
			std_vec_free_Checked((StdVec *) &keyed, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		if (parts_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &parts);
		if (whole_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &whole);
		if (all_live)
			std_vec_free_Checked((StdVec *) &all, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		std_unwind_resume(&unwind);
	}
}

/* test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_0 is read: Stamps::read of runs without value ids. */
static bool
test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_0(void *captures, const uint8 *runs, size_t runs_len, ChunksTrailerStamps *result,
																						ChunksErrorError *error)
{
	return chunks_trailer_Stamps_read(runs, runs_len, true, false, result, error);
}

/* test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_1 is run: a stored run of transaction 1 from a tuple id, with a stored length. */
static void
test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_1(void *captures, uint64 tuple, const uint8 *len, size_t len_len, StdVec *result)
{
	static const uint8 txn[] = {1, 0, 0, 0, 0, 0, 0, 0};
	uint8		tuple_bytes[8];
	StdBytes	parts[3];

	std_u64_to_le_bytes(tuple, tuple_bytes);
	std_range_to_check(6, sizeof(tuple_bytes), RUST_FILE, 743, 96);
	parts[0].ptr = txn;
	parts[0].len = sizeof(txn);
	parts[1].ptr = tuple_bytes;
	parts[1].len = 6;
	parts[2].ptr = len;
	parts[2].len = len_len;
	std_slice_concat(parts, 3, sizeof(StdBytes), 1, 1, result);
}

/*
 * test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating:
 * a run's length past the bytes, past 2^48 tuple ids, or past the entries'
 * count, and a run of no stamps, fail with no overflow and no allocation for
 * the claimed length.
 */
void
test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating(void)
{
	volatile StdVec runs[2];
	volatile bool runs_live[2] = {0};
	volatile StdVec concat;
	volatile bool concat_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 huge[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01};
		static const uint8 zero[] = {0};
		static const uint8 two[] = {2};
		static const uint8 three[] = {3};
		StdVec	   *runs_v = (StdVec *) runs;
		StdVec	   *concat_v = (StdVec *) &concat;
		ChunksTrailerStamps read;
		ChunksErrorError error;
		StdBytes	parts[3];
		bool		ok;

		{
			static const uint8 head[] = {1, 2};

			test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_1(NULL, 1, huge, sizeof(huge), &runs_v[0]);
			runs_live[0] = true;
			parts[0].ptr = head;
			parts[0].len = sizeof(head);
			parts[1].ptr = runs_v[0].ptr;
			parts[1].len = runs_v[0].len;
			std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, concat_v);
			concat_live = true;
			ok = test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_0(NULL, concat_v->ptr, concat_v->len, &read, &error);
			if (ok)
				chunks_trailer_Stamps_drop(&read);
			else
				chunks_error_Error_drop(&error);
			concat_live = false;
			std_vec_free_Checked(concat_v, 1, 1);
			runs_live[0] = false;
			std_vec_free_Checked(&runs_v[0], 1, 1);
			STD_ASSERT(745, 9, !ok, "read(&[[1, 2].as_slice(), &run(1, &huge)].concat()).is_err()");
		}
		{
			static const uint8 head[] = {1, 0xff, 0xff, 0xff, 0xff, 0x0f};

			test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_1(NULL, 1, huge, sizeof(huge), &runs_v[0]);
			runs_live[0] = true;
			parts[0].ptr = head;
			parts[0].len = sizeof(head);
			parts[1].ptr = runs_v[0].ptr;
			parts[1].len = runs_v[0].len;
			std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, concat_v);
			concat_live = true;
			ok = test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_0(NULL, concat_v->ptr, concat_v->len, &read, &error);
			if (ok)
				chunks_trailer_Stamps_drop(&read);
			else
				chunks_error_Error_drop(&error);
			concat_live = false;
			std_vec_free_Checked(concat_v, 1, 1);
			runs_live[0] = false;
			std_vec_free_Checked(&runs_v[0], 1, 1);
			STD_ASSERT(746, 9, !ok, "read(&[[1, 0xff, 0xff, 0xff, 0xff, 0x0f].as_slice(), &run(1, &huge)].concat()).is_err()");
		}
		{
			static const uint8 head[] = {1, 3};

			test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_1(NULL, STD_SHL_FOLDED(u64, 1, 48) - 1, three, sizeof(three), &runs_v[0]);
			runs_live[0] = true;
			parts[0].ptr = head;
			parts[0].len = sizeof(head);
			parts[1].ptr = runs_v[0].ptr;
			parts[1].len = runs_v[0].len;
			std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, concat_v);
			concat_live = true;
			ok = test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_0(NULL, concat_v->ptr, concat_v->len, &read, &error);
			if (ok)
				chunks_trailer_Stamps_drop(&read);
			else
				chunks_error_Error_drop(&error);
			concat_live = false;
			std_vec_free_Checked(concat_v, 1, 1);
			runs_live[0] = false;
			std_vec_free_Checked(&runs_v[0], 1, 1);
			STD_ASSERT(747, 9, !ok, "read(&[[1, 3].as_slice(), &run((1 << 48) - 1, &[3])].concat()).is_err()");
		}
		{
			static const uint8 head[] = {1, 3};

			test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_1(NULL, 1, two, sizeof(two), &runs_v[0]);
			runs_live[0] = true;
			parts[0].ptr = head;
			parts[0].len = sizeof(head);
			parts[1].ptr = runs_v[0].ptr;
			parts[1].len = runs_v[0].len;
			std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, concat_v);
			concat_live = true;
			ok = test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_0(NULL, concat_v->ptr, concat_v->len, &read, &error);
			if (ok)
				chunks_trailer_Stamps_drop(&read);
			else
				chunks_error_Error_drop(&error);
			concat_live = false;
			std_vec_free_Checked(concat_v, 1, 1);
			runs_live[0] = false;
			std_vec_free_Checked(&runs_v[0], 1, 1);
			STD_ASSERT(748, 9, !ok, "read(&[[1, 3].as_slice(), &run(1, &[2])].concat()).is_err()");
		}
		{
			static const uint8 head[] = {1, 3};

			test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_1(NULL, 1, zero, sizeof(zero), &runs_v[0]);
			runs_live[0] = true;
			test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_1(NULL, 1, three, sizeof(three), &runs_v[1]);
			runs_live[1] = true;
			parts[0].ptr = head;
			parts[0].len = sizeof(head);
			parts[1].ptr = runs_v[0].ptr;
			parts[1].len = runs_v[0].len;
			parts[2].ptr = runs_v[1].ptr;
			parts[2].len = runs_v[1].len;
			std_slice_concat(parts, 3, sizeof(StdBytes), 1, 1, concat_v);
			concat_live = true;
			ok = test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_0(NULL, concat_v->ptr, concat_v->len, &read, &error);
			if (ok)
				chunks_trailer_Stamps_drop(&read);
			else
				chunks_error_Error_drop(&error);
			concat_live = false;
			std_vec_free_Checked(concat_v, 1, 1);
			runs_live[1] = false;
			std_vec_free_Checked(&runs_v[1], 1, 1);
			runs_live[0] = false;
			std_vec_free_Checked(&runs_v[0], 1, 1);
			STD_ASSERT(749, 9, !ok, "read(&[[1, 3].as_slice(), &run(1, &[0]), &run(1, &[3])].concat()).is_err()");
		}
		{
			static const uint8 head[] = {1, 3};

			test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_1(NULL, 1, three, sizeof(three), &runs_v[0]);
			runs_live[0] = true;
			parts[0].ptr = head;
			parts[0].len = sizeof(head);
			parts[1].ptr = runs_v[0].ptr;
			parts[1].len = runs_v[0].len;
			std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, concat_v);
			concat_live = true;
			ok = test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating_closure_0(NULL, concat_v->ptr, concat_v->len, &read, &error);
			if (ok)
				chunks_trailer_Stamps_drop(&read);
			else
				chunks_error_Error_drop(&error);
			concat_live = false;
			std_vec_free_Checked(concat_v, 1, 1);
			runs_live[0] = false;
			std_vec_free_Checked(&runs_v[0], 1, 1);
			STD_ASSERT(750, 9, ok, "read(&[[1, 3].as_slice(), &run(1, &[3])].concat()).is_ok()");
		}
		std_unwind_pop(&unwind);
	}
	else
	{
		if (concat_live)
			std_vec_free_Checked((StdVec *) &concat, 1, 1);
		for (int i = 1; i >= 0; i--)
		{
			if (runs_live[i])
				std_vec_free_Checked((StdVec *) &runs[i], 1, 1);
		}
		std_unwind_resume(&unwind);
	}
}

/* test_chunks_trailer_tests_a_flipped_byte_anywhere_in_a_trailer_fails_the_split_closure_0 is |_| Ok(locators.len()). */
static bool
test_chunks_trailer_tests_a_flipped_byte_anywhere_in_a_trailer_fails_the_split_closure_0(void *captures,
																						 const uint8 *hashed,
																						 size_t hashed_len,
																						 size_t *result,
																						 ChunksErrorError *error)
{
	*result = lengthof(*(const ChunksTrailerLocator (*)[1]) captures);
	return true;
}

/* test_chunks_trailer_tests_a_flipped_byte_anywhere_in_a_trailer_fails_the_split_closure_1 is |_| Ok(locators.len()). */
static bool
test_chunks_trailer_tests_a_flipped_byte_anywhere_in_a_trailer_fails_the_split_closure_1(void *captures,
																						 const uint8 *hashed,
																						 size_t hashed_len,
																						 size_t *result,
																						 ChunksErrorError *error)
{
	*result = lengthof(*(const ChunksTrailerLocator (*)[1]) captures);
	return true;
}

/*
 * test_chunks_trailer_tests_a_flipped_byte_anywhere_in_a_trailer_fails_the_split:
 * a byte flipped in a trailer's flags, locators, stamps, or checksum fails the
 * split's checksum.
 */
void
test_chunks_trailer_tests_a_flipped_byte_anywhere_in_a_trailer_fails_the_split(void)
{
	volatile ChunksTrailerStamps stamps;
	volatile bool stamps_live = false;
	volatile StdVec stored;
	volatile bool stored_live = false;
	volatile StdVec flipped;
	volatile bool flipped_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 hashed_bytes[] = {'h', 'a', 's', 'h', 'e', 'd', ' ', 'b', 'y', 't', 'e', 's'};
		ChunksTrailerStamps *stamps_v = (ChunksTrailerStamps *) &stamps;
		StdVec	   *stored_v = (StdVec *) &stored;
		StdVec	   *flipped_v = (StdVec *) &flipped;
		ChunksTrailerLocator locators[1];
		ChunksTrailerStamp *boxed;
		StdVec		single;
		size_t		start;
		size_t		stamp_at;
		size_t		ats[7];
		ChunksTrailerStored split;
		ChunksErrorError error;
		bool		ok;

		locators[0] = chunks_trailer_Locator_at(3, 77, 900);
		boxed = std_box_new_uninit_layout_Checked(2 * sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		STD_CHECK_ALIGNED(boxed, _Alignof(ChunksTrailerStamp), 756, 35);
		STD_CHECK_NULL_DEREF(boxed, 756, 35);
		boxed[0].txn = 41;
		boxed[0].tuple = 9;
		boxed[1].txn = 42;
		boxed[1].tuple = 10;
		std_box_slice_into_vec_Checked(&single, boxed, 2, sizeof(ChunksTrailerStamp));
		*stamps_v = chunks_trailer_Stamps_FromVecStamp_from(&single);
		stamps_live = true;
		chunks_trailer_store(hashed_bytes, sizeof(hashed_bytes), locators, lengthof(locators), stamps_v, stored_v);
		stored_live = true;
		start = sizeof(hashed_bytes);
		stamp_at = STD_ADD(usize, STD_ADD(usize, start, 1, 759, 24), CHUNKS_TRAILER_LOCATOR_LEN, 759, 24);
		ats[0] = start;
		ats[1] = STD_ADD(usize, start, 1, 760, 27);
		ats[2] = STD_ADD(usize, start, 5, 760, 38);
		ats[3] = stamp_at;
		ats[4] = STD_ADD(usize, stamp_at, 3, 760, 59);
		ats[5] = STD_SUB(usize, stored_v->len, 6, 760, 73);
		ats[6] = STD_SUB(usize, stored_v->len, 2, 760, 91);
		for (size_t i = 0, n; std_array_IntoIter_next(&i, 7, 7, &n);)
		{
			size_t		at = ats[n];

			std_vec_clone(flipped_v, stored_v, 1, 1);
			flipped_live = true;
			std_index_check(at, flipped_v->len, RUST_FILE, 762, 20);
			STD_VEC_AT(flipped_v, uint8, at) ^= 0x10;
			ok = chunks_trailer_Stored_split(flipped_v->ptr, flipped_v->len,
											 test_chunks_trailer_tests_a_flipped_byte_anywhere_in_a_trailer_fails_the_split_closure_0,
											 locators, NULL, &split, &error);
			if (ok)
			{
				static const StdFmtPiece pieces[] = {STD_FMT_LIT("byte "), STD_FMT_ARG(0)};
				StdFmtArg	args[] = {{&at, std_usize_Display_fmt}};
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};

				std_panic_fmt(RUST_FILE, 763, 13, &arguments);
			}
			chunks_error_Error_drop(&error);
			flipped_live = false;
			std_vec_free_Checked(flipped_v, 1, 1);
		}
		ok = chunks_trailer_Stored_split(stored_v->ptr, stored_v->len,
										 test_chunks_trailer_tests_a_flipped_byte_anywhere_in_a_trailer_fails_the_split_closure_1,
										 locators, NULL, &split, &error);
		if (!ok)
			chunks_error_Error_drop(&error);
		STD_ASSERT(765, 9, ok, "Stored::split(&stored, |_| Ok(locators.len())).is_ok()");
		stored_live = false;
		std_vec_free_Checked(stored_v, 1, 1);
		stamps_live = false;
		chunks_trailer_Stamps_drop(stamps_v);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (flipped_live)
			std_vec_free_Checked((StdVec *) &flipped, 1, 1);
		if (stored_live)
			std_vec_free_Checked((StdVec *) &stored, 1, 1);
		if (stamps_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &stamps);
		std_unwind_resume(&unwind);
	}
}

/*
 * test_chunks_trailer_tests_a_locator_keeps_its_three_numbers_at_their_widths:
 * a locator's stored bytes hold a pack's number of 48 bits, an offset of 40,
 * and a length of 24, and give them back; a number above 32 bits is no other
 * pack's; and locators sort by pack and then by offset.
 */
void
test_chunks_trailer_tests_a_locator_keeps_its_three_numbers_at_their_widths(void)
{
	volatile StdVec stored;
	volatile bool stored_live = false;
	volatile StdVec stored_1;
	volatile bool stored_1_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *stored_v = (StdVec *) &stored;
		StdVec	   *stored_1_v = (StdVec *) &stored_1;
		ChunksTrailerLocator widest;
		ChunksTrailerLocator wide;
		ChunksTrailerLocator other;
		ChunksTrailerLocator left;
		ChunksTrailerLocator read;
		size_t		len;
		size_t		expected_len = CHUNKS_TRAILER_LOCATOR_LEN;
		uint64		pack;
		uint64		offset;
		uint32		length;
		uint64		expected_pack = STD_SHL_FOLDED(u64, 1, 48) - 1;
		uint64		expected_offset = STD_SHL_FOLDED(u64, 1, 40) - 1;
		uint32		expected_length = STD_SHL_FOLDED(u32, 1, 24) - 1;
		bool		less;

		widest = chunks_trailer_Locator_at(STD_SHL_FOLDED(u64, 1, 48) - 1, STD_SHL_FOLDED(u64, 1, 40) - 1,
										   STD_SHL_FOLDED(u32, 1, 24) - 1);
		std_vec_new(stored_v, 1, 1);
		stored_live = true;
		chunks_trailer_Locator_write(&widest, stored_v);
		len = stored_v->len;
		STD_ASSERT_EQ(809, 9, len == expected_len, &len, std_usize_Debug_fmt, &expected_len, std_usize_Debug_fmt);
		read = chunks_trailer_Locator_read(stored_v->ptr, stored_v->len);
		STD_ASSERT_EQ(810, 9, chunks_trailer_Locator_PartialEq_eq(&read, &widest), &read,
					  chunks_trailer_Locator_Debug_fmt, &widest, chunks_trailer_Locator_Debug_fmt);
		pack = widest.pack;
		offset = chunks_trailer_Locator_offset(&widest);
		length = chunks_trailer_Locator_length(&widest);
		STD_ASSERT_EQ(811, 9, pack == expected_pack && offset == expected_offset && length == expected_length,
					  &STD_TUPLE(3, STD_ITEM(&pack, std_u64_Debug_fmt), STD_ITEM(&offset, std_u64_Debug_fmt),
								 STD_ITEM(&length, std_u32_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(3, STD_ITEM(&expected_pack, std_u64_Debug_fmt),
								 STD_ITEM(&expected_offset, std_u64_Debug_fmt),
								 STD_ITEM(&expected_length, std_u32_Debug_fmt)),
					  std_tuple_Debug_fmt);
		wide = chunks_trailer_Locator_at(STD_SHL_FOLDED(u64, 1, 32) + 1, 9, 1);
		std_vec_new(stored_1_v, 1, 1);
		stored_1_live = true;
		chunks_trailer_Locator_write(&wide, stored_1_v);
		read = chunks_trailer_Locator_read(stored_1_v->ptr, stored_1_v->len);
		STD_ASSERT_EQ(815, 9, chunks_trailer_Locator_PartialEq_eq(&read, &wide), &read,
					  chunks_trailer_Locator_Debug_fmt, &wide, chunks_trailer_Locator_Debug_fmt);
		other = chunks_trailer_Locator_at(1, 9, 1);
		if (chunks_trailer_Locator_PartialEq_eq(&wide, &other))
			std_assert_failed(RUST_FILE, 816, 9, "!=", &wide, chunks_trailer_Locator_Debug_fmt, &other,
							  chunks_trailer_Locator_Debug_fmt, NULL);
		left = chunks_trailer_Locator_at(5, 1, 900);
		other = chunks_trailer_Locator_at(5, 2, 1);
		less = chunks_trailer_Locator_Ord_cmp(&left, &other) == STD_ORDERING_LESS;
		if (less)
		{
			left = chunks_trailer_Locator_at(5, STD_SHL_FOLDED(u64, 1, 40) - 1, 1);
			other = chunks_trailer_Locator_at(6, 0, 1);
			less = chunks_trailer_Locator_Ord_cmp(&left, &other) == STD_ORDERING_LESS;
		}
		STD_ASSERT(817, 9, less,
				   "Locator::at(5, 1, 900) < Locator::at(5, 2, 1) && Locator::at(5, (1 << 40) - 1, 1) < Locator::at(6, 0, 1)");
		stored_1_live = false;
		std_vec_free_Checked(stored_1_v, 1, 1);
		stored_live = false;
		std_vec_free_Checked(stored_v, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (stored_1_live)
			std_vec_free_Checked((StdVec *) &stored_1, 1, 1);
		if (stored_live)
			std_vec_free_Checked((StdVec *) &stored, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/* test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form_closure_0 is |_| Ok(0). */
static bool
test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form_closure_0(void *captures,
																									const uint8 *hashed,
																									size_t hashed_len,
																									size_t *result,
																									ChunksErrorError *error)
{
	*result = 0;
	return true;
}

/*
 * test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form_closure_2
 * is section: the captured two stamps, then a value ids section of the
 * bytes and a stored length, read.
 */
static bool
test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form_closure_2(void *captures,
																									const uint8 *bytes,
																									size_t bytes_len,
																									uint32 len,
																									ChunksTrailerStamps *result,
																									ChunksErrorError *error)
{
	const StdVec *two = (const StdVec *) captures;
	volatile StdVec out;
	volatile bool out_live = false;
	bool		ok = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *out_v = (StdVec *) &out;
		uint8		len_bytes[4];

		std_vec_clone(out_v, two, 1, 1);
		out_live = true;
		std_vec_extend_from_slice_Checked(out_v, bytes, bytes_len, 1, 1);
		std_u32_to_le_bytes(len, len_bytes);
		std_vec_extend_from_slice_Checked(out_v, len_bytes, sizeof(len_bytes), 1, 1);
		ok = chunks_trailer_Stamps_read(out_v->ptr, out_v->len, false, true, result, error);
		out_live = false;
		std_vec_free_Checked(out_v, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (out_live)
			std_vec_free_Checked((StdVec *) &out, 1, 1);
		std_unwind_resume(&unwind);
	}
	return ok;
}

/*
 * test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form_closure_3
 * is fits: section of the bytes with their own length.
 */
static bool
test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form_closure_3(void *captures,
																									const uint8 *bytes,
																									size_t bytes_len,
																									ChunksTrailerStamps *result,
																									ChunksErrorError *error)
{
	return test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form_closure_2(captures,
																											   bytes,
																											   bytes_len,
																											   (uint32) bytes_len,
																											   result,
																											   error);
}

/*
 * test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form:
 * bugs row 98: a leaf's entries' value ids follow its stamps, single or as
 * runs, and read back with each entry's ids, none for an entry with none; and
 * a section with an entry out of order or past the entries, a count of 0, an
 * id past 2^32, no entry, a cut, or a length past the trailer is data
 * corruption.
 */
void
test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form(void)
{
	volatile ChunksTrailerStamps stamps;
	volatile bool stamps_live = false;
	volatile StdVec copies;
	volatile bool copies_live = false;
	volatile StdVec stored;
	volatile bool stored_live = false;
	volatile ChunksTrailerStamps read;
	volatile bool read_live = false;
	volatile ChunksTrailerStamps copied;
	volatile bool copied_live = false;
	volatile StdVec two;
	volatile bool two_live = false;
	volatile ChunksErrorError error;
	volatile bool error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 h[] = {'h'};
		static const uint32 first_values[] = {7, PG_UINT32_MAX};
		static const uint32 last_values[] = {9};
		static const uint8 fit[] = {0, 1, 7, 1, 1, 8};
		static const uint8 out_of_order[] = {1, 1, 7, 0, 1, 7};
		static const uint8 twice[] = {0, 1, 7, 0, 1, 8};
		static const uint8 past_entries[] = {2, 1, 7};
		static const uint8 count_0[] = {0, 0};
		static const uint8 cut[] = {0, 2, 7};
		static const uint8 past_trailer[] = {0, 1, 7};
		static const uint32 eight[] = {8};
		ChunksTrailerStamps *stamps_v = (ChunksTrailerStamps *) &stamps;
		StdVec	   *copies_v = (StdVec *) &copies;
		StdVec	   *stored_v = (StdVec *) &stored;
		ChunksTrailerStamps *read_v = (ChunksTrailerStamps *) &read;
		ChunksTrailerStamps *copied_v = (ChunksTrailerStamps *) &copied;
		StdVec	   *two_v = (StdVec *) &two;
		const bool	runs_cases[] = {false, true};
		ChunksTrailerStored split;
		ChunksTrailerStamp stamp;
		StdVec		values[3];
		StdVec		expected[3];
		StdVec		value;
		StdVec		expected_value;
		uint8		past[5];
		uint8		past_id[7];
		StdBytes	bytes_cases[7];
		const char *what_cases[7];
		ChunksTrailerStamps fitted;
		bool		ok;

		for (size_t i = 0, n; std_array_IntoIter_next(&i, 2, 2, &n);)
		{
			bool		runs = runs_cases[n];

			*stamps_v = chunks_trailer_Stamps_Default_default();
			stamps_live = true;
			chunks_trailer_tests_copies(5, 1, 1, copies_v);
			copies_live = true;
			chunks_trailer_Stamps_push(stamps_v, copies_v->ptr, copies_v->len);
			copies_live = false;
			std_vec_free_Checked(copies_v, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
			chunks_trailer_Stamps_push_values(stamps_v, first_values, lengthof(first_values));
			chunks_trailer_tests_copies(5, 2, runs ? 2 : 1, copies_v);
			copies_live = true;
			chunks_trailer_Stamps_push(stamps_v, copies_v->ptr, copies_v->len);
			copies_live = false;
			std_vec_free_Checked(copies_v, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
			chunks_trailer_tests_copies(5, 9, 1, copies_v);
			copies_live = true;
			chunks_trailer_Stamps_push(stamps_v, copies_v->ptr, copies_v->len);
			copies_live = false;
			std_vec_free_Checked(copies_v, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
			chunks_trailer_Stamps_push_values(stamps_v, last_values, lengthof(last_values));
			chunks_trailer_store(h, sizeof(h), (const ChunksTrailerLocator *) STD_DANGLING, 0, stamps_v, stored_v);
			stored_live = true;
			if (!chunks_trailer_Stored_split(stored_v->ptr, stored_v->len,
											 test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form_closure_0,
											 NULL, NULL, &split, (ChunksErrorError *) &error))
			{
				error_live = true;
				std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
										 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
										 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 835, 58);
			}
			if (!chunks_trailer_Stored_stamps(&split, read_v, (ChunksErrorError *) &error))
			{
				error_live = true;
				std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
										 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
										 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 835, 67);
			}
			read_live = true;
			STD_ASSERT_EQ(836, 13, chunks_trailer_Stamps_PartialEq_eq(read_v, stamps_v), read_v,
						  chunks_trailer_Stamps_Debug_fmt, stamps_v, chunks_trailer_Stamps_Debug_fmt);
			for (size_t j = 0; j < 3; j++)
			{
				values[j].ptr = (void *) chunks_trailer_Stamps_values(read_v, j, &values[j].len);
				values[j].cap = 0;
			}
			expected[0].ptr = (void *) first_values;
			expected[0].len = lengthof(first_values);
			expected[1].ptr = STD_DANGLING;
			expected[1].len = 0;
			expected[2].ptr = (void *) last_values;
			expected[2].len = lengthof(last_values);
			ok = true;
			for (size_t j = 0; j < 3 && ok; j++)
			{
				expected[j].cap = 0;
				ok = values[j].len == expected[j].len &&
					memcmp(values[j].ptr, expected[j].ptr, values[j].len * sizeof(uint32)) == 0;
			}
			STD_ASSERT_EQ(837, 13, ok,
						  &STD_TUPLE(3, STD_ITEM(&values[0], chunks_header_VecU32_Debug_fmt),
									 STD_ITEM(&values[1], chunks_header_VecU32_Debug_fmt),
									 STD_ITEM(&values[2], chunks_header_VecU32_Debug_fmt)),
						  std_tuple_Debug_fmt,
						  &STD_TUPLE(3, STD_ITEM(&expected[0], chunks_header_VecU32_Debug_fmt),
									 STD_ITEM(&expected[1], chunks_header_VecU32_Debug_fmt),
									 STD_ITEM(&expected[2], chunks_header_VecU32_Debug_fmt)),
						  std_tuple_Debug_fmt);
			*copied_v = chunks_trailer_Stamps_Default_default();
			copied_live = true;
			for (size_t j = 0, j_end = 3; j < j_end; j++)
				chunks_trailer_Stamps_push_entry_of(copied_v, read_v, j);
			STD_ASSERT_EQ(840, 13, chunks_trailer_Stamps_PartialEq_eq(copied_v, stamps_v), copied_v,
						  chunks_trailer_Stamps_Debug_fmt, stamps_v, chunks_trailer_Stamps_Debug_fmt);
			copied_live = false;
			chunks_trailer_Stamps_drop(copied_v);
			read_live = false;
			chunks_trailer_Stamps_drop(read_v);
			stored_live = false;
			std_vec_free_Checked(stored_v, 1, 1);
			stamps_live = false;
			chunks_trailer_Stamps_drop(stamps_v);
		}
		std_vec_new(two_v, 1, 1);
		two_live = true;
		stamp.txn = 5;
		stamp.tuple = 1;
		chunks_trailer_Stamp_write(&stamp, two_v);
		stamp.txn = 5;
		stamp.tuple = 2;
		chunks_trailer_Stamp_write(&stamp, two_v);
		if (!test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form_closure_3(two_v,
																												 fit,
																												 sizeof(fit),
																												 read_v,
																												 (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 852, 46);
		}
		read_live = true;
		value.ptr = (void *) chunks_trailer_Stamps_values(read_v, 1, &value.len);
		value.cap = 0;
		expected_value.ptr = (void *) eight;
		expected_value.len = lengthof(eight);
		expected_value.cap = 0;
		STD_ASSERT_EQ(852, 9,
					  value.len == expected_value.len &&
					  memcmp(value.ptr, expected_value.ptr, value.len * sizeof(uint32)) == 0,
					  &value, chunks_header_VecU32_Debug_fmt, &expected_value, chunks_header_VecU32_Debug_fmt);
		read_live = false;
		chunks_trailer_Stamps_drop(read_v);
		past[0] = 0x80;
		past[1] = 0x80;
		past[2] = 0x80;
		past[3] = 0x80;
		past[4] = 0x10;
		past_id[0] = 0;
		past_id[1] = 1;
		memcpy(past_id + 2, past, sizeof(past));
		bytes_cases[0] = (StdBytes)
		{
			out_of_order, sizeof(out_of_order)
		};
		what_cases[0] = "an entry out of order";
		bytes_cases[1] = (StdBytes)
		{
			twice, sizeof(twice)
		};
		what_cases[1] = "an entry twice";
		bytes_cases[2] = (StdBytes)
		{
			past_entries, sizeof(past_entries)
		};
		what_cases[2] = "an entry past the entries";
		bytes_cases[3] = (StdBytes)
		{
			count_0, sizeof(count_0)
		};
		what_cases[3] = "a count of 0";
		bytes_cases[4] = (StdBytes)
		{
			past_id, sizeof(past_id)
		};
		what_cases[4] = "an id past 2^32";
		bytes_cases[5] = (StdBytes)
		{
			(const uint8 *) STD_DANGLING, 0
		};
		what_cases[5] = "no entry";
		bytes_cases[6] = (StdBytes)
		{
			cut, sizeof(cut)
		};
		what_cases[6] = "a cut section";
		for (size_t i = 0, n; std_array_IntoIter_next(&i, 7, 7, &n);)
		{
			StdBytes	bytes = bytes_cases[n];
			StdStr		what = {what_cases[n], strlen(what_cases[n])};

			ok = test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form_closure_3(two_v,
																													 bytes.ptr,
																													 bytes.len,
																													 &fitted,
																													 (ChunksErrorError *) &error);
			if (ok)
			{
				static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" read")};
				StdFmtArg	args[] = {{&what, std_str_Display_fmt}};
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};

				chunks_trailer_Stamps_drop(&fitted);
				std_panic_fmt(RUST_FILE, 863, 13, &arguments);
			}
			chunks_error_Error_drop((ChunksErrorError *) &error);
		}
		ok = test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form_closure_2(two_v,
																												 past_trailer,
																												 sizeof(past_trailer),
																												 100,
																												 &fitted,
																												 (ChunksErrorError *) &error);
		if (ok)
		{
			chunks_trailer_Stamps_drop(&fitted);
			std_panic(RUST_FILE, 865, 9, "a length past the trailer read", sizeof("a length past the trailer read") - 1);
		}
		chunks_error_Error_drop((ChunksErrorError *) &error);
		two_live = false;
		std_vec_free_Checked(two_v, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (error_live)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		if (copies_live)
			std_vec_free_Checked((StdVec *) &copies, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		if (copied_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &copied);
		if (read_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &read);
		if (stored_live)
			std_vec_free_Checked((StdVec *) &stored, 1, 1);
		if (stamps_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &stamps);
		if (two_live)
			std_vec_free_Checked((StdVec *) &two, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/* test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids_closure_0 is |_| Ok(locators.len()): the captured count. */
static bool
test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids_closure_0(void *captures,
																						  const uint8 *hashed,
																						  size_t hashed_len,
																						  size_t *result,
																						  ChunksErrorError *error)
{
	*result = *(const size_t *) captures;
	return true;
}

/* test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids_closure_1 is the leaf's |_| Ok(0). */
static bool
test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids_closure_1(void *captures,
																						  const uint8 *hashed,
																						  size_t hashed_len,
																						  size_t *result,
																						  ChunksErrorError *error)
{
	*result = 0;
	return true;
}

/* test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids_closure_2 is both's |_| Ok(0). */
static bool
test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids_closure_2(void *captures,
																						  const uint8 *hashed,
																						  size_t hashed_len,
																						  size_t *result,
																						  ChunksErrorError *error)
{
	*result = 0;
	return true;
}

/* test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids_closure_3 is too_high's |_| Ok(0). */
static bool
test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids_closure_3(void *captures,
																						  const uint8 *hashed,
																						  size_t hashed_len,
																						  size_t *result,
																						  ChunksErrorError *error)
{
	*result = 0;
	return true;
}

/*
 * test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids:
 * an internal node's subtrees' highest tuple ids round trip with and without
 * locators, a leaf has none, and a trailer with both stamps and subtree ids,
 * or with an id past 2^48, is refused.
 */
void
test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids(void)
{
	volatile ChunksTrailerStamps subtrees;
	volatile bool subtrees_live = false;
	volatile StdVec stored;
	volatile bool stored_live = false;
	volatile ChunksTrailerStamps read;
	volatile bool read_live = false;
	volatile ChunksTrailerStamps temp;
	volatile bool temp_live = false;
	volatile StdVec leaf;
	volatile bool leaf_live = false;
	volatile StdVec both;
	volatile bool both_live = false;
	volatile StdVec too_high;
	volatile bool too_high_live = false;
	volatile ChunksErrorError error;
	volatile bool error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 hashed_bytes[] = {'h', 'a', 's', 'h', 'e', 'd', ' ', 'b', 'y', 't', 'e', 's'};
		static const uint8 h[] = {'h'};
		ChunksTrailerStamps *subtrees_v = (ChunksTrailerStamps *) &subtrees;
		StdVec	   *stored_v = (StdVec *) &stored;
		ChunksTrailerStamps *read_v = (ChunksTrailerStamps *) &read;
		ChunksTrailerStamps *temp_v = (ChunksTrailerStamps *) &temp;
		StdVec	   *leaf_v = (StdVec *) &leaf;
		StdVec	   *both_v = (StdVec *) &both;
		StdVec	   *too_high_v = (StdVec *) &too_high;
		ChunksTrailerLocator locators[2];
		uint64	   *maxes;
		ChunksTrailerStamp *single;
		StdVec		vec;
		ChunksTrailerStored split;
		uint64		subtree_max[3];
		StdOptionRef found[3];
		uint64		expected_max = STD_SHL_FOLDED(u64, 1, 48) - 1;
		uint64		zero = 0;
		StdOptionRef expected[3] = {{true, {&zero, std_u64_Debug_fmt}}, {true, {&expected_max, std_u64_Debug_fmt}},
		{false, {NULL, std_u64_Debug_fmt}}};
		uint64		max_tuple;
		size_t		entries;
		size_t		expected_entries = 0;
		uint32		len;
		uint64		checksum;
		uint8		checksum_bytes[8];
		size_t		at;
		uint8		replacement[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x40};
		uint8		len_bytes[4];
		StdArrayIntoIter replace_with;
		StdBound	range_start;
		StdBound	range_end;
		StdIterator iter;
		StdVecSplice splice;
		bool		ok;

		locators[0] = chunks_trailer_Locator_at(3, 77, 900);
		memset(&locators[1], 0, sizeof(locators[1]));
		maxes = std_box_new_uninit_layout_Checked(2 * sizeof(uint64), _Alignof(uint64));
		STD_CHECK_ALIGNED(maxes, _Alignof(uint64), 771, 41);
		STD_CHECK_NULL_DEREF(maxes, 771, 41);
		maxes[0] = 0;
		maxes[1] = STD_SHL_FOLDED(u64, 1, 48) - 1;
		std_box_slice_into_vec_Checked(&vec, maxes, 2, sizeof(uint64));
		*subtrees_v = chunks_trailer_Stamps_subtrees(&vec);
		subtrees_live = true;
		for (size_t i = 0, n; std_array_IntoIter_next(&i, 2, 2, &n);)
		{
			const ChunksTrailerLocator *case_locators = n == 0 ? locators : (const ChunksTrailerLocator *) STD_DANGLING;
			size_t		locators_len = n == 0 ? lengthof(locators) : 0;

			chunks_trailer_store(hashed_bytes, sizeof(hashed_bytes), case_locators, locators_len, subtrees_v, stored_v);
			stored_live = true;
			if (!chunks_trailer_Stored_split(stored_v->ptr, stored_v->len,
											 test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids_closure_0,
											 &locators_len, NULL, &split, (ChunksErrorError *) &error))
			{
				error_live = true;
				std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
										 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
										 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 774, 72);
			}
			if (!chunks_trailer_Stored_stamps(&split, read_v, (ChunksErrorError *) &error))
			{
				error_live = true;
				std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
										 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
										 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 775, 39);
			}
			read_live = true;
			STD_ASSERT_EQ(776, 13, chunks_trailer_Stamps_PartialEq_eq(read_v, subtrees_v), read_v,
						  chunks_trailer_Stamps_Debug_fmt, subtrees_v, chunks_trailer_Stamps_Debug_fmt);
			found[0].has = chunks_trailer_Stamps_subtree_max(read_v, 0, &subtree_max[0]);
			found[0].some = STD_ITEM(&subtree_max[0], std_u64_Debug_fmt);
			found[1].has = chunks_trailer_Stamps_subtree_max(read_v, 1, &subtree_max[1]);
			found[1].some = STD_ITEM(&subtree_max[1], std_u64_Debug_fmt);
			found[2].has = chunks_trailer_Stamps_subtree_max(read_v, 2, &subtree_max[2]);
			found[2].some = STD_ITEM(&subtree_max[2], std_u64_Debug_fmt);
			STD_ASSERT_EQ(777, 13,
						  found[0].has && subtree_max[0] == zero &&
						  found[1].has && subtree_max[1] == expected_max && !found[2].has,
						  &STD_TUPLE(3, STD_ITEM(&found[0], std_option_Debug_fmt), STD_ITEM(&found[1], std_option_Debug_fmt),
									 STD_ITEM(&found[2], std_option_Debug_fmt)),
						  std_tuple_Debug_fmt,
						  &STD_TUPLE(3, STD_ITEM(&expected[0], std_option_Debug_fmt),
									 STD_ITEM(&expected[1], std_option_Debug_fmt),
									 STD_ITEM(&expected[2], std_option_Debug_fmt)),
						  std_tuple_Debug_fmt);
			max_tuple = chunks_trailer_Stamps_max_tuple(read_v);
			entries = chunks_trailer_Stamps_entries(read_v);
			STD_ASSERT_EQ(781, 13, max_tuple == expected_max && entries == expected_entries,
						  &STD_TUPLE(2, STD_ITEM(&max_tuple, std_u64_Debug_fmt), STD_ITEM(&entries, std_usize_Debug_fmt)),
						  std_tuple_Debug_fmt,
						  &STD_TUPLE(2, STD_ITEM(&expected_max, std_u64_Debug_fmt),
									 STD_ITEM(&expected_entries, std_usize_Debug_fmt)),
						  std_tuple_Debug_fmt);
			read_live = false;
			chunks_trailer_Stamps_drop(read_v);
			stored_live = false;
			std_vec_free_Checked(stored_v, 1, 1);
		}
		single = std_box_new_uninit_layout_Checked(sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		STD_CHECK_ALIGNED(single, _Alignof(ChunksTrailerStamp), 783, 51);
		STD_CHECK_NULL_DEREF(single, 783, 51);
		single[0].txn = 1;
		single[0].tuple = 5;
		std_box_slice_into_vec_Checked(&vec, single, 1, sizeof(ChunksTrailerStamp));
		*temp_v = chunks_trailer_Stamps_FromVecStamp_from(&vec);
		temp_live = true;
		chunks_trailer_store(h, sizeof(h), (const ChunksTrailerLocator *) STD_DANGLING, 0, temp_v, leaf_v);
		leaf_live = true;
		temp_live = false;
		chunks_trailer_Stamps_drop(temp_v);
		if (!chunks_trailer_Stored_split(leaf_v->ptr, leaf_v->len,
										 test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids_closure_1,
										 NULL, NULL, &split, (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 784, 52);
		}
		if (!chunks_trailer_Stored_stamps(&split, read_v, (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 784, 70);
		}
		read_live = true;
		found[0].has = chunks_trailer_Stamps_subtree_max(read_v, 0, &subtree_max[0]);
		found[0].some = STD_ITEM(&subtree_max[0], std_u64_Debug_fmt);
		STD_ASSERT_EQ(784, 9, !found[0].has, &found[0], std_option_Debug_fmt, &expected[2], std_option_Debug_fmt);
		read_live = false;
		chunks_trailer_Stamps_drop(read_v);
		maxes = std_box_new_uninit_layout_Checked(sizeof(uint64), _Alignof(uint64));
		STD_CHECK_ALIGNED(maxes, _Alignof(uint64), 785, 59);
		STD_CHECK_NULL_DEREF(maxes, 785, 59);
		maxes[0] = 7;
		std_box_slice_into_vec_Checked(&vec, maxes, 1, sizeof(uint64));
		*temp_v = chunks_trailer_Stamps_subtrees(&vec);
		temp_live = true;
		chunks_trailer_store(h, sizeof(h), (const ChunksTrailerLocator *) STD_DANGLING, 0, temp_v, both_v);
		both_live = true;
		temp_live = false;
		chunks_trailer_Stamps_drop(temp_v);
		std_index_check(1, both_v->len, RUST_FILE, 786, 13);
		STD_VEC_AT(both_v, uint8, 1) |= CHUNKS_TRAILER_HAS_STAMPS;
		len = (uint32) STD_SUB(usize, STD_SUB(usize, both_v->len, 1, 787, 20), 4, 787, 20);
		at = STD_SUB(usize, both_v->len, 12, 788, 50);
		std_range_check(1, at, both_v->len, RUST_FILE, 788, 46);
		checksum = chunks_trailer_trailer_checksum((const uint8 *) both_v->ptr + 1, at - 1, len);
		at = STD_SUB(usize, both_v->len, 12, 789, 18);
		std_range_check(at, STD_ADD(usize, at, 8, 790, 18), both_v->len, RUST_FILE, 790, 13);
		std_u64_to_le_bytes(checksum, checksum_bytes);
		std_slice_copy_from_slice_Checked((uint8 *) both_v->ptr + at, 8, checksum_bytes, sizeof(checksum_bytes), 1, 1,
										  RUST_FILE, 790, 26);
		ok = chunks_trailer_Stored_split(both_v->ptr, both_v->len,
										 test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids_closure_2,
										 NULL, NULL, &split, (ChunksErrorError *) &error);
		if (!ok)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		STD_ASSERT(791, 9, !ok, "Stored::split(&both, |_| Ok(0)).is_err()");
		maxes = std_box_new_uninit_layout_Checked(sizeof(uint64), _Alignof(uint64));
		STD_CHECK_ALIGNED(maxes, _Alignof(uint64), 792, 63);
		STD_CHECK_NULL_DEREF(maxes, 792, 63);
		maxes[0] = 0;
		std_box_slice_into_vec_Checked(&vec, maxes, 1, sizeof(uint64));
		*temp_v = chunks_trailer_Stamps_subtrees(&vec);
		temp_live = true;
		chunks_trailer_store(h, sizeof(h), (const ChunksTrailerLocator *) STD_DANGLING, 0, temp_v, too_high_v);
		too_high_live = true;
		temp_live = false;
		chunks_trailer_Stamps_drop(temp_v);
		replace_with.data = replacement;
		replace_with.len = sizeof(replacement);
		replace_with.size = 1;
		replace_with.start = 0;
		replace_with.end = sizeof(replacement);
		range_start.tag = STD_BOUND_INCLUDED;
		range_start.index = 2;
		range_end.tag = STD_BOUND_EXCLUDED;
		range_end.index = 3;
		iter.ops = &STD_ARRAY_INTO_ITER_ITERATOR_OPS;
		iter.self = &replace_with;
		splice = std_vec_splice(too_high_v, range_start, range_end, 1, 1, NULL, iter);
		std_VecSplice_drop(&splice);
		len = (uint32) STD_SUB(usize, STD_SUB(usize, too_high_v->len, 1, 794, 20), 4, 794, 20);
		at = STD_SUB(usize, too_high_v->len, 12, 795, 18);
		std_range_check(1, at, too_high_v->len, RUST_FILE, 796, 50);
		checksum = chunks_trailer_trailer_checksum((const uint8 *) too_high_v->ptr + 1, at - 1, len);
		std_range_check(at, STD_ADD(usize, at, 8, 797, 22), too_high_v->len, RUST_FILE, 797, 17);
		std_u64_to_le_bytes(checksum, checksum_bytes);
		std_slice_copy_from_slice_Checked((uint8 *) too_high_v->ptr + at, 8, checksum_bytes, sizeof(checksum_bytes), 1,
										  1, RUST_FILE, 797, 30);
		std_u32_to_le_bytes(len, len_bytes);
		replace_with.data = len_bytes;
		replace_with.len = sizeof(len_bytes);
		replace_with.size = 1;
		replace_with.start = 0;
		replace_with.end = sizeof(len_bytes);
		range_start.tag = STD_BOUND_INCLUDED;
		range_start.index = STD_SUB(usize, too_high_v->len, 4, 798, 25);
		range_end.tag = STD_BOUND_UNBOUNDED;
		range_end.index = 0;
		splice = std_vec_splice(too_high_v, range_start, range_end, 1, 1, NULL, iter);
		std_VecSplice_drop(&splice);
		if (!chunks_trailer_Stored_split(too_high_v->ptr, too_high_v->len,
										 test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids_closure_3,
										 NULL, NULL, &split, (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 799, 53);
		}
		ok = chunks_trailer_Stored_stamps(&split, read_v, (ChunksErrorError *) &error);
		if (ok)
			chunks_trailer_Stamps_drop(read_v);
		else
			chunks_error_Error_drop((ChunksErrorError *) &error);
		STD_ASSERT(799, 9, !ok, "Stored::split(&too_high, |_| Ok(0)).unwrap().stamps().is_err()");
		too_high_live = false;
		std_vec_free_Checked(too_high_v, 1, 1);
		both_live = false;
		std_vec_free_Checked(both_v, 1, 1);
		leaf_live = false;
		std_vec_free_Checked(leaf_v, 1, 1);
		subtrees_live = false;
		chunks_trailer_Stamps_drop(subtrees_v);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (error_live)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		if (read_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &read);
		if (temp_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &temp);
		if (stored_live)
			std_vec_free_Checked((StdVec *) &stored, 1, 1);
		if (too_high_live)
			std_vec_free_Checked((StdVec *) &too_high, 1, 1);
		if (both_live)
			std_vec_free_Checked((StdVec *) &both, 1, 1);
		if (leaf_live)
			std_vec_free_Checked((StdVec *) &leaf, 1, 1);
		if (subtrees_live)
			chunks_trailer_Stamps_drop((ChunksTrailerStamps *) &subtrees);
		std_unwind_resume(&unwind);
	}
}


/*
 * test_chunks_trailer_tests_xxh3_digests_are_the_committed_table checks
 * that the vendored xxHash's XXH3 digests of the lengths 0 to 300 and the
 * block boundaries, taken one shot and fed in growing pieces, are the table
 * committed in chunks' testdata, which the crate's own test checks too.
 */
void
test_chunks_trailer_tests_xxh3_digests_are_the_committed_table(void)
{
	static const size_t boundaries[] = {1023, 1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097, 16383, 16384, 16385};
	static const char path[] = DOLTGRES_CHUNKS_TESTDATA "/xxh3_64.tsv";
	StdString	table = STD_VEC_NEW;
	StdVec		committed;
	StdIoError	io_error;
	uint8	   *input = palloc(16385);

	for (size_t n = 0; n < 301 + lengthof(boundaries); n++)
	{
		size_t		len = n < 301 ? n : boundaries[n - 301];
		XXH3_state_t *state = XXH3_createState();
		uint64		one_shot;
		uint64		streamed;
		size_t		at = 0;
		size_t		piece = 1;
		char		line[48];
		int			line_len;

		for (size_t i = 0; i < len; i++)
			input[i] = (uint8) (i * 131 + 7);
		one_shot = XXH3_64bits(input, len);
		XXH3_64bits_reset(state);
		while (at < len)
		{
			size_t		end = Min(at + piece, len);

			XXH3_64bits_update(state, input + at, end - at);
			at = end;
			piece++;
		}
		streamed = XXH3_64bits_digest(state);
		XXH3_freeState(state);
		if (streamed != one_shot)
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("length "), STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&len, std_usize_Display_fmt}};
			StdFmtArguments message = {pieces, 2, args};

			std_assert_failed(RUST_FILE, 885, 13, "==", &streamed, std_u64_Debug_fmt, &one_shot, std_u64_Debug_fmt,
							  &message);
		}
		line_len = snprintf(line, sizeof(line), "%zu\t%016" PRIx64 "\n", len, one_shot);
		std_string_push_str_Checked(&table, line, (size_t) line_len);
	}
	pfree(input);
	if (!std_fs_read(path, sizeof(path) - 1, &committed, &io_error))
		std_panic(RUST_FILE, 888, 20, "couldn't read testdata/xxh3_64.tsv", sizeof("couldn't read testdata/xxh3_64.tsv") - 1);
	if (committed.len != table.len || memcmp(committed.ptr, table.ptr, table.len) != 0)
	{
		StdStr		left = {committed.ptr, committed.len};

		std_assert_failed(RUST_FILE, 888, 9, "==", &left, std_str_Debug_fmt, &table, std_String_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&committed, 1, 1);
	std_vec_free_Checked(&table, 1, 1);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
