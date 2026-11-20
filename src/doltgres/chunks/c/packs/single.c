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
 * The fork's single records of chunks' shared state (chunks' packs/single.rs):
 * so far the kind of a file in the removal queue.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/single.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/record.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "single.rs"

/* chunks_packs_single_Queued_Debug_fmt is Queued's derived Debug: the variant's name, and Vector's generation in a tuple. */
bool
chunks_packs_single_Queued_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksSingleQueued *self = value;
	StdDebugTuple b;

	switch (self->tag)
	{
		case CHUNKS_PACKS_SINGLE_QUEUED_PACK:
			return std_fmt_write_str(f, "Pack", 4);
		case CHUNKS_PACKS_SINGLE_QUEUED_RUN:
			return std_fmt_write_str(f, "Run", 3);
		case CHUNKS_PACKS_SINGLE_QUEUED_VECTOR:
			b = std_fmt_debug_tuple(f, "Vector", 6);
			std_DebugTuple_field(&b, &self->u.vector, std_NonZeroU64_Debug_fmt);
			return std_DebugTuple_finish(&b);
	}
	pg_unreachable();
}

/* chunks_packs_single_Queued_PartialEq_eq is Queued's derived PartialEq: the same variant, and for Vector the same generation. */
bool
chunks_packs_single_Queued_PartialEq_eq(const ChunksPacksSingleQueued *self, const ChunksPacksSingleQueued *other)
{
	if (self->tag != other->tag)
		return false;
	if (self->tag == CHUNKS_PACKS_SINGLE_QUEUED_VECTOR)
		return self->u.vector == other->u.vector;
	return true;
}

/*
 * CHUNKS_PACKS_SINGLE_CHUNKS_STATE_ARRAYS is ChunksState's ARRAYS: the hits'
 * and the removal queue's counts and entry sizes, array::<ChunksState, I>'s
 * values, which rustc computes at compile time.
 */
static const std_TupleUsizeUsize_t CHUNKS_PACKS_SINGLE_CHUNKS_STATE_ARRAYS[] = {
	{CHUNKS_PACKS_SINGLE_HIT_SLOTS, CHUNKS_PACKS_SINGLE_HIT},
	{CHUNKS_PACKS_SINGLE_QUEUED, CHUNKS_PACKS_SINGLE_REMOVAL},
};

/* CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS is ChunksState's Shape and Single, which declares no run of words. */
const HostSharedSingleOps CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS = {
	.shape = {.id = UINT64CONST(0x6368756e6b730002), .locks = 3, .atomics = 1,
	.arrays = CHUNKS_PACKS_SINGLE_CHUNKS_STATE_ARRAYS, .arrays_len = lengthof(CHUNKS_PACKS_SINGLE_CHUNKS_STATE_ARRAYS)},
	.words = host_shared_Single_words,
};

StaticAssertDecl(CHUNKS_PACKS_SINGLE_MAKING_INDEX < 3 && CHUNKS_PACKS_SINGLE_REMOVING_INDEX < 3 &&
				 CHUNKS_PACKS_SINGLE_SPACES_INDEX < 3 &&
				 CHUNKS_PACKS_SINGLE_MAKING_INDEX != CHUNKS_PACKS_SINGLE_REMOVING_INDEX &&
				 CHUNKS_PACKS_SINGLE_MAKING_INDEX != CHUNKS_PACKS_SINGLE_SPACES_INDEX &&
				 CHUNKS_PACKS_SINGLE_REMOVING_INDEX != CHUNKS_PACKS_SINGLE_SPACES_INDEX,
				 "ChunksState's locks are distinct and below its count, distinct's assertion");

/* chunks_packs_single_hit returns a marking's hit as the hits array holds it: the address, then the locator's pack, offset, and length. */
void
chunks_packs_single_hit(const ChunksAddressAddress *address, ChunksTrailerLocator locator, uint8 *result)
{
	uint8		le[8];

	memset(result, 0, CHUNKS_PACKS_SINGLE_HIT);
	std_range_to_check(32, CHUNKS_PACKS_SINGLE_HIT, RUST_FILE, 94, 10);
	std_slice_copy_from_slice_Checked(result, 32, address->f0, 32, 1, 1, RUST_FILE, 94, 17);
	std_range_check(32, 40, CHUNKS_PACKS_SINGLE_HIT, RUST_FILE, 95, 10);
	std_u64_to_le_bytes(locator.pack, le);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(result, 32, 1), 8, le, 8, 1, 1, RUST_FILE, 95, 19);
	std_range_check(40, 48, CHUNKS_PACKS_SINGLE_HIT, RUST_FILE, 96, 10);
	std_u64_to_le_bytes(chunks_trailer_Locator_offset(&locator), le);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(result, 40, 1), 8, le, 8, 1, 1, RUST_FILE, 96, 19);
	std_range_from_check(48, CHUNKS_PACKS_SINGLE_HIT, RUST_FILE, 97, 10);
	std_u32_to_le_bytes(chunks_trailer_Locator_length(&locator), le);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(result, 48, 1), 4, le, 4, 1, 1, RUST_FILE, 97, 17);
}

/* chunks_packs_single_hit_of returns the address and locator a hit's bytes hold, refusing bytes of another length. */
bool
chunks_packs_single_hit_of(const uint8 *bytes, size_t bytes_len, chunks_TupleAddressLocator_t *result,
						   ChunksErrorError *error)
{
	static const char text[] = "a marking's hit is corrupt";

	if (bytes_len != CHUNKS_PACKS_SINGLE_HIT)
	{
		*error = chunks_error_Error_corrupt_RefStr(text, sizeof(text) - 1);
		return false;
	}
	std_range_to_check(32, CHUNKS_PACKS_SINGLE_HIT, RUST_FILE, 105, 35);
	std_slice_copy_from_slice_Checked(result->f0.f0, 32, bytes, 32, 1, 1, RUST_FILE, 105, 13);
	{
		uint64		pack = chunks_packs_long(bytes, bytes_len, 32);
		uint64		offset = chunks_packs_long(bytes, bytes_len, 40);
		uint32		length = chunks_packs_word(bytes, bytes_len, 48);

		result->f1 = chunks_trailer_Locator_at(pack, offset, length);
	}
	return true;
}

/* chunks_packs_single_Removal_Debug_fmt is Removal's derived Debug. */
bool
chunks_packs_single_Removal_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksSingleRemoval *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Removal", 7);

	std_DebugStruct_field(&s, "repository", 10, &self->repository, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "space", 5, &self->space, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "kind", 4, &self->kind, chunks_packs_single_Queued_Debug_fmt);
	std_DebugStruct_field(&s, "id", 2, &self->id, chunks_pack_PackId_Debug_fmt);
	std_DebugStruct_field(&s, "epoch", 5, &self->epoch, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_packs_single_Removal_PartialEq_eq is Removal's derived PartialEq: every field equal, in order. */
bool
chunks_packs_single_Removal_PartialEq_eq(const ChunksPacksSingleRemoval *self, const ChunksPacksSingleRemoval *other)
{
	return self->repository == other->repository && self->space == other->space &&
		chunks_packs_single_Queued_PartialEq_eq(&self->kind, &other->kind) &&
		chunks_pack_PackId_PartialEq_eq(&self->id, &other->id) && self->epoch == other->epoch;
}

/*
 * chunks_packs_single_Removal_bytes returns the entry as the queue holds it:
 * the repository, the tablespace, the kind's number, four zero bytes, the id,
 * a deletion vector's generation or zero, and the epoch.
 */
void
chunks_packs_single_Removal_bytes(const ChunksPacksSingleRemoval *self, uint8 *result)
{
	uint32		kind;
	uint64		generation = 0;
	uint8		le[8];

	switch (self->kind.tag)
	{
		case CHUNKS_PACKS_SINGLE_QUEUED_PACK:
			kind = 1;
			break;
		case CHUNKS_PACKS_SINGLE_QUEUED_RUN:
			kind = 2;
			break;
		case CHUNKS_PACKS_SINGLE_QUEUED_VECTOR:
		default:
			kind = 3;
			generation = self->kind.u.vector;
			break;
	}
	memset(result, 0, CHUNKS_PACKS_SINGLE_REMOVAL);
	std_range_to_check(4, CHUNKS_PACKS_SINGLE_REMOVAL, RUST_FILE, 157, 14);
	std_u32_to_le_bytes(self->repository, le);
	std_slice_copy_from_slice_Checked(result, 4, le, 4, 1, 1, RUST_FILE, 157, 20);
	std_range_check(4, 8, CHUNKS_PACKS_SINGLE_REMOVAL, RUST_FILE, 158, 14);
	std_u32_to_le_bytes(self->space, le);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(result, 4, 1), 4, le, 4, 1, 1, RUST_FILE, 158, 21);
	std_range_check(8, 12, CHUNKS_PACKS_SINGLE_REMOVAL, RUST_FILE, 159, 14);
	std_u32_to_le_bytes(kind, le);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(result, 8, 1), 4, le, 4, 1, 1, RUST_FILE, 159, 22);
	std_range_check(16, 32, CHUNKS_PACKS_SINGLE_REMOVAL, RUST_FILE, 160, 14);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(result, 16, 1), 16, self->id.f0, 16, 1, 1, RUST_FILE,
									  160, 23);
	std_range_check(32, 40, CHUNKS_PACKS_SINGLE_REMOVAL, RUST_FILE, 161, 14);
	std_u64_to_le_bytes(generation, le);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(result, 32, 1), 8, le, 8, 1, 1, RUST_FILE, 161, 23);
	std_range_from_check(40, CHUNKS_PACKS_SINGLE_REMOVAL, RUST_FILE, 162, 14);
	std_u64_to_le_bytes(self->epoch, le);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(result, 40, 1), 8, le, 8, 1, 1, RUST_FILE, 162, 21);
}

/* chunks_packs_single_Removal_read_closure_0 is read's corrupt: the error of an entry no queueing writes. */
static ChunksErrorError
chunks_packs_single_Removal_read_closure_0(void)
{
	static const char text[] = "an entry of the removal queue is corrupt";

	return chunks_error_Error_corrupt_RefStr(text, sizeof(text) - 1);
}

/*
 * chunks_packs_single_Removal_read returns the file an entry's bytes hold, or
 * nothing for a free entry, which is all zero bytes, refusing bytes no
 * queueing writes.
 */
bool
chunks_packs_single_Removal_read(const uint8 *bytes, size_t bytes_len, bool *some, ChunksPacksSingleRemoval *result,
								 ChunksErrorError *error)
{
	uint32		kind;
	uint64		generation;
	ChunksPacksSingleQueued queued;

	if (bytes_len != CHUNKS_PACKS_SINGLE_REMOVAL)
	{
		*error = chunks_packs_single_Removal_read_closure_0();
		return false;
	}
	kind = chunks_packs_word(bytes, bytes_len, 8);
	generation = chunks_packs_long(bytes, bytes_len, 32);
	if (kind == 0 && generation == 0)
	{
		bool		zero = true;

		for (size_t k = 0, k_len = bytes_len; k < k_len; k++)
			if (bytes[k] != 0)
			{
				zero = false;
				break;
			}
		if (zero)
		{
			*some = false;
			return true;
		}
	}
	if (kind == 1 && generation == 0)
		queued.tag = CHUNKS_PACKS_SINGLE_QUEUED_PACK;
	else if (kind == 2 && generation == 0)
		queued.tag = CHUNKS_PACKS_SINGLE_QUEUED_RUN;
	else if (kind == 3 && generation != 0)
	{
		queued.tag = CHUNKS_PACKS_SINGLE_QUEUED_VECTOR;
		queued.u.vector = generation;
	}
	else
	{
		*error = chunks_packs_single_Removal_read_closure_0();
		return false;
	}
	std_range_check(16, 32, bytes_len, RUST_FILE, 179, 34);
	std_slice_copy_from_slice_Checked(result->id.f0, 16, STD_PTR_AT(bytes, 16, 1), 16, 1, 1, RUST_FILE,
									  179, 12);
	result->repository = chunks_packs_word(bytes, bytes_len, 0);
	result->space = chunks_packs_word(bytes, bytes_len, 4);
	result->kind = queued;
	result->epoch = chunks_packs_long(bytes, bytes_len, 40);
	*some = true;
	return true;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_packs_single_tests_TupleAddressLocator_PartialEq_eq is an (Address, Locator)'s PartialEq: both parts equal. */
static bool
chunks_packs_single_tests_TupleAddressLocator_PartialEq_eq(const chunks_TupleAddressLocator_t *self,
														   const chunks_TupleAddressLocator_t *other)
{
	return chunks_address_Address_PartialEq_eq(&self->f0, &other->f0) &&
		chunks_trailer_Locator_PartialEq_eq(&self->f1, &other->f1);
}

/*
 * test_chunks_packs_single_tests_a_hit_keeps_a_wide_locator is the test: a
 * hit gives back its address and a locator whose pack's number is above 32
 * bits, and bytes of another length are no hit.
 */
void
test_chunks_packs_single_tests_a_hit_keeps_a_wide_locator(void)
{
	ChunksAddressAddress address = chunks_address_Address_of((const uint8 *) "hit", 3);
	ChunksTrailerLocator locator = chunks_trailer_Locator_at((UINT64CONST(1) << 47) + 3, (UINT64CONST(1) << 39) + 5, 77);
	uint8		bytes[CHUNKS_PACKS_SINGLE_HIT];
	chunks_TupleAddressLocator_t got;
	chunks_TupleAddressLocator_t want = {address, locator};
	ChunksErrorError error;

	chunks_packs_single_hit(&address, locator, bytes);
	if (!chunks_packs_single_hit_of(bytes, sizeof(bytes), &got, &error))
		STD_RESULT_EXPECT_FAILED(195, 35, "called `Result::unwrap()` on an `Err` value", &error,
								 chunks_error_Error_Debug_fmt);
	STD_ASSERT_EQ(195, 9, chunks_packs_single_tests_TupleAddressLocator_PartialEq_eq(&got, &want), &got,
				  chunks_record_tests_TupleAddressLocator_Debug_fmt, &want,
				  chunks_record_tests_TupleAddressLocator_Debug_fmt);
	std_range_from_check(1, sizeof(bytes), RUST_FILE, 196, 39);
	if (chunks_packs_single_hit_of(STD_PTR_AT(bytes, 1, 1), sizeof(bytes) - 1, &got, &error))
		std_panic(RUST_FILE, 196, 9, "assertion failed: matches!(hit_of(&bytes[1..]), Err(Error::Corrupt(_)))",
				  sizeof("assertion failed: matches!(hit_of(&bytes[1..]), Err(Error::Corrupt(_)))") - 1);
	{
		bool		corrupt = error.tag == CHUNKS_ERROR_ERROR_CORRUPT;

		chunks_error_Error_drop(&error);
		if (!corrupt)
			std_panic(RUST_FILE, 196, 9,
					  "assertion failed: matches!(hit_of(&bytes[1..]), Err(Error::Corrupt(_)))",
					  sizeof("assertion failed: matches!(hit_of(&bytes[1..]), Err(Error::Corrupt(_)))") - 1);
	}
}

#endif							/* DOLTGRES_TEST_CHUNKS */
