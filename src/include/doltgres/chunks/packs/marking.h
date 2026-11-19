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
 * A collection's marking of a repository's packs (chunks' packs/marking.rs).
 */
#ifndef DOLTGRES_CHUNKS_PACKS_MARKING_H
#define DOLTGRES_CHUNKS_PACKS_MARKING_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"

extern bool chunks_packs_marking_Packs_begin_marking(const ChunksPacksBackendPacks *self, HostIoIo *io,
													 const HostSharedReader *reading, uint64 *result,
													 ChunksErrorError *error);
extern bool chunks_packs_marking_Packs_end_marking(const ChunksPacksBackendPacks *self, HostIoIo *io,
												   ChunksErrorError *error);
extern bool chunks_packs_marking_Packs_hits(const ChunksPacksBackendPacks *self, HostIoIo *io, StdVec *result,
											ChunksErrorError *error);
extern bool chunks_packs_marking_Packs_locate(const ChunksPacksBackendPacks *self, HostIoIo *io,
											  const HostSharedReader *reading, const ChunksAddressAddress *address,
											  bool *has_result, ChunksTrailerLocator *result, ChunksErrorError *error);
extern bool chunks_packs_marking_Packs_locate_in(const ChunksPacksBackendPacks *self, HostIoIo *io,
												 const HostSharedReader *reading, const ChunksAddressAddress *address,
												 uint32 space, bool *has_result, ChunksTrailerLocator *result,
												 ChunksErrorError *error);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_marking_tests_a_locate_records_its_hit_in_a_pack_the_marking_covers(void);
extern void test_chunks_packs_marking_tests_a_locate_answers_no_dead_chunk_and_no_place_in_an_unlisted_pack(void);
extern void test_chunks_packs_marking_tests_a_marking_clears_the_name_a_terminated_collector_left(void);
extern void test_chunks_packs_marking_tests_a_locate_past_a_markings_slots_answers_nothing(void);
extern void test_chunks_packs_marking_tests_a_collection_removes_the_unfinished_packs_no_writers_word_covers(void);
extern void test_chunks_packs_marking_tests_a_collection_removes_an_unfinished_pack_alone_in_its_tablespace(void);
extern void test_chunks_packs_marking_tests_a_collection_goes_on_past_an_unfinished_pack_it_cant_remove(void);
extern void test_chunks_packs_marking_tests_a_collection_removes_a_terminated_backends_unfinished_pack(void);
extern void test_chunks_packs_marking_tests_a_collection_never_removes_a_live_writers_unfinished_pack(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_MARKING_H */
