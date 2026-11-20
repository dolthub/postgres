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
 * nodes each child's subtree's highest tuple id, with a checksum of its own
 * (chunks' trailer.rs).
 */
#ifndef DOLTGRES_CHUNKS_TRAILER_H
#define DOLTGRES_CHUNKS_TRAILER_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/error.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"

/* CHUNKS_TRAILER_LOCATOR_LEN is the length of a stored locator. */
#define CHUNKS_TRAILER_LOCATOR_LEN 14

/* CHUNKS_TRAILER_STAMP_LEN is the length of a stored row stamp. */
#define CHUNKS_TRAILER_STAMP_LEN 14

/*
 * ChunksTrailerLocator is where a chunk was stored when the chunk that
 * refers to it was written: the pack's number in the repository's pack
 * table, and the offset of the chunk's record in the pack above its stored
 * length. A locator whose length is zero is unknown.
 */
typedef struct ChunksTrailerLocator
{
	/* pack is the pack's number, below 2^48. */
	uint64		pack;
	/* place is the offset, below 2^40, above the stored length, below 2^24. */
	uint64		place;
} ChunksTrailerLocator;

/* chunks_TupleAddressLocator_t is the tuple (Address, Locator): an address and where its chunk was stored. */
typedef struct chunks_TupleAddressLocator_t
{
	ChunksAddressAddress f0;
	ChunksTrailerLocator f1;
} chunks_TupleAddressLocator_t;

/* chunks_TupleU32Locator_t is the tuple (u32, Locator): an offset and the locator of the address stored there. */
typedef struct chunks_TupleU32Locator_t
{
	uint32		f0;
	ChunksTrailerLocator f1;
} chunks_TupleU32Locator_t;

/* chunks_TupleLocatorLocator_t is the tuple (Locator, Locator): an old locator and its copy's. */
typedef struct chunks_TupleLocatorLocator_t
{
	ChunksTrailerLocator f0;
	ChunksTrailerLocator f1;
} chunks_TupleLocatorLocator_t;

/* ChunksTrailerStamp is a row version's stamp: the id of the transaction that wrote it and its tuple id. */
typedef struct ChunksTrailerStamp
{
	uint64		txn;
	/* tuple is the tuple id, below 2^48. */
	uint64		tuple;
} ChunksTrailerStamp;

/* chunks_trailer_Stamps_runs_t is a run: its first stamp and where it ends, counted through the runs. */
typedef struct chunks_trailer_Stamps_runs_t
{
	ChunksTrailerStamp f0;
	uint64		f1;
} chunks_trailer_Stamps_runs_t;

/* chunks_trailer_Stamps_values_t is an entry's value ids: its index and its ids in its row's order. */
typedef struct chunks_trailer_Stamps_values_t
{
	uint64		f0;
	StdVec		f1;
} chunks_trailer_Stamps_values_t;

/*
 * ChunksTrailerStamps are the row stamps of a leaf's entries, each entry's
 * in order: one by one while every entry has exactly one, and otherwise as
 * maximal runs of one transaction's consecutive tuple ids.
 */
typedef struct ChunksTrailerStamps
{
	/* single is each entry's stamp, while every entry has exactly one. */
	StdVec		single;
	/* runs is otherwise each run, a chunks_trailer_Stamps_runs_t. */
	StdVec		runs;

	/*
	 * has_ends is whether some entry does not have exactly one stamp, so that
	 * ends is kept.
	 */
	bool		has_ends;
	/* ends is where each entry's stamps end, counted through the runs. */
	StdVec		ends;

	/*
	 * max_tuples is an internal tree node's children's subtrees' highest
	 * tuple ids, 0 for one without stamps.
	 */
	StdVec		max_tuples;

	/*
	 * values is each entry with values out of line, a
	 * chunks_trailer_Stamps_values_t, in ascending index order.
	 */
	StdVec		values;
} ChunksTrailerStamps;

/* ChunksTrailerStored is a stored chunk split into its hashed bytes and its trailer. */
typedef struct ChunksTrailerStored
{
	StdBytes	hashed;
	StdBytes	locators;
	StdBytes	stamps;
	/* runs is whether the stamps are runs after how many each entry has. */
	bool		runs;

	/*
	 * max_tuples is whether the stamps are an internal tree node's subtrees'
	 * highest tuple ids.
	 */
	bool		max_tuples;
	/* values is whether the stamps are followed by the value ids section. */
	bool		values;
} ChunksTrailerStored;

extern uint64 chunks_trailer_Locator_offset(const ChunksTrailerLocator *self);
extern ChunksTrailerLocator chunks_trailer_Locator_at(uint64 pack, uint64 offset, uint32 length);
extern uint32 chunks_trailer_Locator_length(const ChunksTrailerLocator *self);
extern bool chunks_trailer_Locator_is_known(const ChunksTrailerLocator *self);
extern void chunks_trailer_Locator_write(const ChunksTrailerLocator *self, StdVec *out);
extern ChunksTrailerLocator chunks_trailer_Locator_read(const uint8 *bytes, size_t bytes_len);
extern bool chunks_trailer_VecU64_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_trailer_Locator_Debug_fmt(const void *value, StdFormatter *f);
extern ChunksTrailerLocator chunks_trailer_Locator_Default_default(void);
extern bool chunks_trailer_Locator_PartialEq_eq(const ChunksTrailerLocator *self, const ChunksTrailerLocator *other);
extern void chunks_trailer_Locator_Hash_hash(const void *self_, StdHasher state);
extern StdOrdering chunks_trailer_Locator_Ord_cmp(const ChunksTrailerLocator *self, const ChunksTrailerLocator *other);
extern bool chunks_trailer_Stamp_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_trailer_Stamp_PartialEq_eq(const ChunksTrailerStamp *self, const ChunksTrailerStamp *other);
extern bool chunks_trailer_VecStamp_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_trailer_VecStamp_PartialEq_eq(const StdVec *self, const StdVec *other);
extern bool chunks_trailer_Stamps_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_trailer_Stamps_PartialEq_eq(const ChunksTrailerStamps *self, const ChunksTrailerStamps *other);
extern void chunks_trailer_Stamp_write(const ChunksTrailerStamp *self, StdVec *out);
extern ChunksTrailerStamp chunks_trailer_Stamp_read(const uint8 *bytes, size_t bytes_len);
extern void chunks_trailer_Stamps_drop(void *self_);
extern ChunksTrailerStamps chunks_trailer_Stamps_Default_default(void);
extern ChunksTrailerStamps chunks_trailer_Stamps_subtrees(StdVec *max_tuples);
extern size_t chunks_trailer_Stamps_heap_bytes(const ChunksTrailerStamps *self);
extern bool chunks_trailer_Stamps_subtree_max(const ChunksTrailerStamps *self, size_t i, uint64 *result);
extern size_t chunks_trailer_Stamps_entries(const ChunksTrailerStamps *self);
extern uint64 chunks_trailer_Stamps_entry_len(const ChunksTrailerStamps *self, size_t i);
extern bool chunks_trailer_Stamps_first(const ChunksTrailerStamps *self, size_t i, ChunksTrailerStamp *result);
extern void chunks_trailer_Stamps_entry(const ChunksTrailerStamps *self, size_t i, StdVec *result);
extern bool chunks_trailer_Stamps_holds(const ChunksTrailerStamps *self, size_t i, uint64 tuple);
extern uint64 chunks_trailer_Stamps_max_tuple(const ChunksTrailerStamps *self);
extern void chunks_trailer_Stamps_push(ChunksTrailerStamps *self, const ChunksTrailerStamp *entry, size_t entry_len);
extern void chunks_trailer_Stamps_push_values(ChunksTrailerStamps *self, const uint32 *ids, size_t ids_len);
extern const uint32 *chunks_trailer_Stamps_values(const ChunksTrailerStamps *self, size_t i, size_t *result_len);
extern void chunks_trailer_Stamps_push_entry_of(ChunksTrailerStamps *self, const ChunksTrailerStamps *other, size_t i);
extern ChunksTrailerStamps chunks_trailer_Stamps_FromVecStamp_from(StdVec *single);
extern void chunks_trailer_store(const uint8 *hashed, size_t hashed_len, const ChunksTrailerLocator *locators,
								 size_t locators_len, const ChunksTrailerStamps *stamps, StdVec *result);
extern bool chunks_trailer_unlocated(const uint8 *stored, size_t stored_len, StdBytes *result_0,
									 ChunksTrailerStamps *result_1, ChunksErrorError *error);
extern bool chunks_trailer_Stored_split(const uint8 *stored, size_t stored_len,
										bool (*addresses) (void *captures, const uint8 *hashed, size_t hashed_len,
														   size_t *result, ChunksErrorError *error),
										void *addresses_captures, void (*addresses_drop) (void *),
										ChunksTrailerStored *result, ChunksErrorError *error);
extern ChunksTrailerLocator chunks_trailer_Stored_locator(const ChunksTrailerStored *self, size_t i);
extern void chunks_trailer_Stored_trailer(const ChunksTrailerStored *self, StdBytes *out_0, StdBytes *out_1,
										  uint8 *out_2);
extern ChunksTrailerStored chunks_trailer_Stored_of_trailer(StdBytes hashed, StdBytes locators, StdBytes stamps,
															uint8 reads);
extern bool chunks_trailer_Stored_stamps(const ChunksTrailerStored *self, ChunksTrailerStamps *result,
										 ChunksErrorError *error);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_trailer_tests_round_trips(void);
extern void test_chunks_trailer_tests_entries_with_many_stamps_store_maximal_runs(void);
extern void test_chunks_trailer_tests_the_same_stamps_store_the_same_bytes_however_they_were_pushed(void);
extern void test_chunks_trailer_tests_corrupt_runs_fail_without_overflowing_or_allocating(void);
extern void test_chunks_trailer_tests_a_flipped_byte_anywhere_in_a_trailer_fails_the_split(void);
extern void test_chunks_trailer_tests_a_locator_keeps_its_three_numbers_at_their_widths(void);
extern void test_chunks_trailer_tests_xxh3_digests_are_the_committed_table(void);
extern void test_chunks_trailer_tests_a_leafs_value_ids_follow_its_stamps_and_refuse_every_other_form(void);
extern void test_chunks_trailer_tests_internal_nodes_store_their_subtrees_highest_tuple_ids(void);
#endif

#endif							/* DOLTGRES_CHUNKS_TRAILER_H */
