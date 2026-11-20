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
 * The checks of pg_checkversioned over a stopped server's data directory,
 * changing nothing: every repository's packs, both generations of its
 * manifest, and every chunk both generations' base trees reach.
 */
#ifndef DOLTGRES_STORE_FSCK_H
#define DOLTGRES_STORE_FSCK_H

#include "doltgres/chunks/file.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/vec.h"

/*
 * StoreFsckKeptWal is KeptWal: a WAL range a repository's fallback would
 * replay, from a manifest's redo point to the control file's, whether a start
 * at the oldest segment held stands in for a first segment that isn't kept,
 * and whether a gap in it is a finding or what the next start settles.
 */
typedef struct StoreFsckKeptWal
{
	uint32		repository;
	HostLsn		from;
	bool		oldest;
	bool		found;
} StoreFsckKeptWal;

/*
 * StoreFsckFsck is Fsck: what a check of a data directory found, each line
 * naming its repository, what doesn't verify, what the next start settles,
 * the notes of what harms nothing, and the WAL ranges a fallback would
 * replay.
 */
typedef struct StoreFsckFsck
{
	/* found, settled, and notes are each a Vec<String>. */
	StdVec		found;
	StdVec		settled;
	StdVec		notes;
	/* kept is a Vec<KeptWal>. */
	StdVec		kept;
} StoreFsckFsck;

extern void store_fsck_fsck(ChunksFileInterruptible io, StoreFsckFsck *result);
extern void store_fsck_Fsck_drop(StoreFsckFsck *self);

#ifdef DOLTGRES_TEST_STORE
extern void test_store_fsck_tests_no_repository_finds_nothing(void);
extern void test_store_fsck_tests_an_evened_repository_finds_nothing(void);
extern void test_store_fsck_tests_differing_generations_are_settled_with_the_newests_range(void);
extern void test_store_fsck_tests_a_newest_root_that_cant_be_read_is_found_with_its_range(void);
extern void test_store_fsck_tests_a_newest_manifest_not_whole_is_found_with_the_previouss_range(void);
extern void test_store_fsck_tests_a_dropped_repository_is_settled(void);
extern void test_store_fsck_tests_a_chunk_a_base_record_names_that_no_pack_holds_is_found(void);
extern void test_store_fsck_tests_a_node_whose_parent_says_another_count_is_found_naming_the_parent(void);
extern void test_store_fsck_tests_a_newest_whose_root_reads_settles_its_ranges_gap_though_a_deeper_chunk_is_missing(void);
extern void test_store_fsck_tests_a_tablespace_whose_location_is_missing_is_noted(void);
extern void test_store_fsck_tests_a_newest_identity_root_that_cant_be_read_is_found_with_its_range(void);
#endif

#endif							/* DOLTGRES_STORE_FSCK_H */
