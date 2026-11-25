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
 * Retiring packs a collection found dead (chunks' packs/retire.rs).
 */
#ifndef DOLTGRES_CHUNKS_PACKS_RETIRE_H
#define DOLTGRES_CHUNKS_PACKS_RETIRE_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/packs/removal.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/btree.h"

/* ChunksPacksBackendPacks is backend.h's Packs, which retire reaches only by pointer. */
typedef struct ChunksPacksBackendPacks ChunksPacksBackendPacks;

/* ChunksPacksRetireForwarded is Forwarded: the moves a collection recorded for its repository's packs. */
typedef struct ChunksPacksRetireForwarded
{
	uint64		packs;

	/*
	 * froms is a BTreeSet<(u64, u64)> of the packs and offsets moved from: a
	 * Vec of std_TupleU64U64_t in order, each once.
	 */
	StdVec		froms;
} ChunksPacksRetireForwarded;

/* ChunksPacksRetireMarking is Marking: a collection's marks of what is dead, lent to retire's keep. */
typedef struct ChunksPacksRetireMarking ChunksPacksRetireMarking;

/* ChunksPacksRetireRetired is Retired: packs retired, until their removal is queued. */
typedef struct ChunksPacksRetireRetired
{
	const ChunksPacksBackendPacks *packs;
	StdVec		numbers;
	StdVec		listed;
	StdVec		files;
	ChunksPacksRemovalReservation reservation;
} ChunksPacksRetireRetired;

extern void chunks_packs_retire_froms_insert(StdVec *froms, const std_TupleU64U64_t *from, const char *file,
											 uint32 line, uint32 col);
extern bool chunks_packs_retire_Forwarded_Debug_fmt(const void *value, StdFormatter *f);
extern void chunks_packs_retire_Forwarded_of(const ChunksPacksBackendPacks *packs, StdVec *froms,
											 ChunksPacksRetireForwarded *result);
extern bool chunks_packs_retire_Forwarded_join(ChunksPacksRetireForwarded *self, ChunksPacksRetireForwarded *other,
											   ChunksPacksRetireForwarded *result, ChunksErrorError *error);
extern bool chunks_packs_retire_Forwarded_covers(const ChunksPacksRetireForwarded *self, uint64 pack, uint64 offset);
extern bool chunks_packs_retire_Marking_set_dead(ChunksPacksRetireMarking *self, uint64 number, uint64 generation,
												 const StdBTreeMap *offsets, bool *result, ChunksErrorError *error)
			STD_REQUIRES(CHUNKS_PACKS_BARRIER_HOLDING);
extern bool chunks_packs_retire_Packs_retire(const ChunksPacksBackendPacks *self, HostIoIo *io, const uint64 *numbers,
											 size_t numbers_len, ChunksPacksRetireForwarded *forwarded,
											 ChunksPacksRemovalReservation *reservation,
											 bool (*keep) (void *captures, ChunksPacksRetireMarking *marking,
														   StdVec *entries, StdVec *result,
														   ChunksErrorError *error),
											 void *keep_captures, void (*keep_drop) (void *),
											 ChunksPacksRetireRetired *result, ChunksErrorError *error);
extern const uint64 *chunks_packs_retire_Retired_numbers(const ChunksPacksRetireRetired *self, size_t *result_len);
extern bool chunks_packs_retire_Retired_restore(ChunksPacksRetireRetired *self, HostIoIo *io,
												const HostSharedReader *reading, uint64 number, uint64 *result,
												ChunksErrorError *error);
extern bool chunks_packs_retire_Retired_queue(ChunksPacksRetireRetired *self, HostIoIo *io,
											  const HostSharedReader *reading, ChunksErrorError *error);
extern void chunks_packs_retire_Forwarded_drop(ChunksPacksRetireForwarded *self);
extern void chunks_packs_retire_Retired_drop(ChunksPacksRetireRetired *self);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_retire_tests_a_retirement_takes_its_packs_out_and_queues_their_files(void);
extern void test_chunks_packs_retire_tests_a_retirement_keeps_the_packs_its_marking_found_in_use(void);
extern void test_chunks_packs_retire_tests_a_retirement_is_refused_without_its_marking_or_its_own_forwards(void);
extern void test_chunks_packs_retire_tests_a_retirement_takes_out_only_the_packs_it_reserved_entries_for(void);
extern void test_chunks_packs_retire_tests_a_retired_pack_is_restored_under_a_fresh_number_and_reads_both_ways(void);
extern void test_chunks_packs_retire_tests_a_markings_newer_vector_replaces_the_older_through_the_queue(void);
#ifdef DOLTGRES_CHUNKS_TRACE
extern void test_chunks_packs_retire_tests_a_packs_writing_adding_forwarding_and_retirement_are_traced_in_order(void);
extern void test_chunks_packs_retire_tests_the_markings_the_roots_and_the_records_transitions_are_traced(void);
#endif
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_RETIRE_H */
