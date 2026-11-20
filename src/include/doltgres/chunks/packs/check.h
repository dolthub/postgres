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
 * The check of a repository's packs over a stopped server's files, changing
 * nothing, with the view of the packs it built from what it read.
 */
#ifndef DOLTGRES_CHUNKS_PACKS_CHECK_H
#define DOLTGRES_CHUNKS_PACKS_CHECK_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/packs/view.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/std/vec.h"

/*
 * ChunksPacksCheckChecked is Checked: what a check of a repository's packs
 * found, each line naming its file, with the view of the packs it built.
 */
typedef struct ChunksPacksCheckChecked
{
	/* found is a Vec<String> of what doesn't verify. */
	StdVec		found;
	/* settled is a Vec<String> of what the next start settles. */
	StdVec		settled;
	/* notes is a Vec<String> of what harms nothing. */
	StdVec		notes;

	/*
	 * dropped is whether the repository is dropped, whose packs it didn't
	 * check.
	 */
	bool		dropped;
	ChunksPacksViewView view;
} ChunksPacksCheckChecked;

extern bool chunks_packs_check_Checked_read(ChunksPacksCheckChecked *self, HostIoReadDoors io,
											const ChunksAddressAddress *address, ChunksTrailerLocator locator,
											ChunksChunkChunk *result, ChunksErrorError *error);
extern void chunks_packs_check_check(ChunksFileInterruptible io, uint32 repository, uint32 own,
									 ChunksPacksCheckChecked *result);
extern void chunks_packs_check_Checked_drop(ChunksPacksCheckChecked *self);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_check_tests_a_clean_repository_finds_nothing_and_reads_its_chunks(void);
extern void test_chunks_packs_check_tests_a_damaged_pack_record_is_found_with_the_run_entry_naming_it(void);
extern void test_chunks_packs_check_tests_a_listed_pack_whose_file_is_gone_is_found(void);
extern void test_chunks_packs_check_tests_a_pack_table_gone_with_its_previous_generation_is_found_as_the_settle_refuses_it(void);
extern void test_chunks_packs_check_tests_a_pack_table_read_as_its_previous_generation_is_settled_and_a_damaged_previous_one_found(void);
extern void test_chunks_packs_check_tests_the_files_a_crash_leaves_are_settled_or_noted(void);
extern void test_chunks_packs_check_tests_a_pack_in_no_run_is_settled(void);
extern void test_chunks_packs_check_tests_chunks_no_run_names_are_found_and_stale_entries_noted(void);
extern void test_chunks_packs_check_tests_a_tablespace_whose_directory_is_missing_is_noted(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_CHECK_H */
