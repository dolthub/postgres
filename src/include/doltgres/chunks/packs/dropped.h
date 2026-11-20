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
 * A dropped repository (chunks' packs/dropped.rs): the mark a drop leaves in
 * its own directory and the entries the removal's pass reads.
 */
#ifndef DOLTGRES_CHUNKS_PACKS_DROPPED_H
#define DOLTGRES_CHUNKS_PACKS_DROPPED_H

#include "doltgres/chunks/error.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"

/* The entry's atomics: Named, the dropped repository's id; Space, its own tablespace; and At, its record's location. */
#define CHUNKS_PACKS_DROPPED_NAMED_INDEX 0
#define CHUNKS_PACKS_DROPPED_SPACE_INDEX 1
#define CHUNKS_PACKS_DROPPED_AT_INDEX 2

/* CHUNKS_PACKS_DROPPED_MARK names the file in a repository's own directory that says it is dropped. */
#define CHUNKS_PACKS_DROPPED_MARK "dropped"

extern const HostSharedPoolOps CHUNKS_PACKS_DROPPED_DROPPED_REPOSITORY_POOL_OPS;

extern bool chunks_packs_dropped_marked(HostIoReadDoors io, const char *dir, size_t dir_len, bool *result,
										ChunksErrorError *error);
extern bool chunks_packs_dropped_mark(HostIoIoDoors io, const char *dir, size_t dir_len, ChunksErrorError *error);
extern bool chunks_packs_dropped_remove_directories(HostIoIo *io, uint32 repository, uint32 own, bool *result,
													ChunksErrorError *error);
extern bool chunks_packs_dropped_remove_dropped(HostIoIo *io, bool *result, ChunksErrorError *error);
extern ChunksErrorError chunks_packs_dropped_refused(uint32 repository);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_dropped_tests_a_dropped_repositorys_files_are_gone_at_once_when_no_process_holds_it(void);
extern void test_chunks_packs_dropped_tests_a_dropped_repositorys_files_wait_for_the_last_process_that_holds_it(void);
extern void test_chunks_packs_dropped_tests_a_removal_cut_short_leaves_the_mark_for_the_next_settle(void);
extern void test_chunks_packs_dropped_tests_a_settle_removes_a_repository_dropped_before_a_crash(void);
extern void test_chunks_packs_dropped_tests_two_passes_act_on_a_dropped_repositorys_entry_one_at_a_time(void);
extern void test_chunks_packs_dropped_tests_a_pass_terminated_at_any_point_leaves_its_entry_for_the_next(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_DROPPED_H */
