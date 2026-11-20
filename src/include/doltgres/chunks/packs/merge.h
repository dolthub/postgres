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
 * The merge of a tablespace's address-index runs (chunks' packs/merge.rs).
 */
#ifndef DOLTGRES_CHUNKS_PACKS_MERGE_H
#define DOLTGRES_CHUNKS_PACKS_MERGE_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/packs/view.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/vec.h"

extern void chunks_packs_merge_covered(const char *line, size_t line_len, StdVec *result);
extern bool chunks_packs_merge_due(const ChunksPacksViewView *view, uint32 *space, size_t *from, size_t *to);
extern bool chunks_packs_merge_merge_runs(ChunksPacksViewView *view, HostSharedRecord record, HostSharedFlagIo *io,
										  bool *has_result, uint32 *space, StdVec *names, ChunksErrorError *error);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_merge_tests_a_merge_puts_one_run_in_the_place_of_the_runs_it_merged(void);
extern void test_chunks_packs_merge_tests_a_merge_whose_runs_are_no_longer_listed_together_replaces_nothing(void);
extern void test_chunks_packs_merge_tests_a_merge_that_would_leave_a_covered_pack_in_no_run_fails(void);
extern void test_chunks_packs_merge_tests_a_merge_takes_no_run_marked_as_written_with_loss(void);
extern void test_chunks_packs_merge_tests_a_runs_line_carries_wide_numbers(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_MERGE_H */
