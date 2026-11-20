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
 * Opening and creating a repository's packs (chunks' packs/open.rs).
 */
#ifndef DOLTGRES_CHUNKS_PACKS_OPEN_H
#define DOLTGRES_CHUNKS_PACKS_OPEN_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/view.h"
#include "doltgres/host/io.h"
#include "doltgres/std/path.h"

/* CHUNKS_PACKS_OPEN_MOVED_FROM is the mark a move's finish writes in the directory the repository moved from. */
#define CHUNKS_PACKS_OPEN_MOVED_FROM "moved"

/* chunks_TupleU32PathBuf_t is a repository's directory with the tablespace its packs are listed under. */
typedef struct chunks_TupleU32PathBuf_t
{
	uint32		f0;
	StdPathBuf	f1;
} chunks_TupleU32PathBuf_t;

extern ChunksErrorError chunks_packs_open_gone(const char *path, size_t path_len);
extern bool chunks_packs_open_read_whole(const ChunksPacksViewListOps *ops, HostIoReadDoors io, const char *path,
										 size_t path_len, void *list, ChunksPacksViewRead *read,
										 ChunksErrorError *error);

/*
 * chunks_packs_open_repositories is repositories: its Vec<(NonZeroU32, u32)>
 * is a StdVec of std_TupleU32U32_t in order.
 */
extern bool chunks_packs_open_repositories(HostIoReadDoors io, StdVec *result, ChunksErrorError *error);
extern bool chunks_packs_open_directories(HostIoReadDoors io, uint32 repository, uint32 own, StdVec *result,
										  ChunksErrorError *error);
extern bool chunks_packs_open_spaced_directories(HostIoReadDoors io, uint32 repository, uint32 own, StdVec *result,
												 ChunksErrorError *error);
extern bool chunks_packs_open_found(HostSharedWithin within, uint32 repository, bool *has_result, HostSharedHeld *held,
									uint64 *location, ChunksErrorError *error);
extern bool chunks_packs_open_made_dir(HostIoIo *io, const char *dir, size_t dir_len, ChunksErrorError *error);
extern bool chunks_packs_open_emptied(HostIoIo *io, ChunksErrorError *error);
extern bool chunks_packs_open_forgot(HostIoIo *io, uint32 repository, uint32 from, ChunksErrorError *error);
extern bool chunks_packs_open_moved_in(HostIoIo *io, const char *copy, size_t copy_len, const char *dir,
									   size_t dir_len, ChunksErrorError *error);
extern bool chunks_packs_open_moving_from(HostIoIo *io, const char *dir, size_t dir_len, ChunksErrorError *error);
extern bool chunks_packs_open_moved_from(HostIoIo *io, const char *dir, size_t dir_len, ChunksErrorError *error);
extern bool chunks_packs_open_dropping(HostIoReadDoors io, uint32 repository, uint32 space, bool *result,
									   ChunksErrorError *error);

extern bool chunks_packs_open_Packs_open(HostIoIo *io, uint32 repository, uint32 own,
										 ChunksPacksBackendPacks *result, ChunksErrorError *error);
extern bool chunks_packs_open_Packs_create(HostIoIo *io, uint32 repository, uint32 own,
										   ChunksPacksBackendPacks *result, ChunksErrorError *error);

extern bool chunks_packs_open_Packs_dropped(HostIoIo *io, uint32 repository, uint32 own, bool *result,
											ChunksErrorError *error);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_open_tests_a_list_file_that_is_not_whole_makes_no_record(void);
extern void test_chunks_packs_open_tests_a_created_repositorys_packs_are_listed_by_the_next_to_open_it(void);
extern void test_chunks_packs_open_tests_two_backends_that_open_one_repository_make_one_record(void);
extern void test_chunks_packs_open_tests_a_full_pool_of_idle_records_makes_room_for_a_new_repository(void);
extern void test_chunks_packs_open_tests_a_record_made_again_gives_no_number_given_before(void);
extern void test_chunks_packs_open_tests_a_full_pool_of_records_with_bounds_ahead_makes_room_by_writing_one_back(void);
extern void test_chunks_packs_open_tests_a_full_pool_of_records_that_defer_adds_makes_room_by_appending_ones(void);
extern void test_chunks_packs_open_tests_a_closed_repositorys_deferred_adds_are_in_its_list_files(void);
extern void test_chunks_packs_open_tests_a_backend_opens_a_repository_while_a_reclaim_looks_at_its_record(void);
extern void test_chunks_packs_open_tests_a_full_pool_of_records_whose_manifests_differ_makes_room_by_writing_one_once_more(void);
extern void test_chunks_packs_open_tests_a_record_made_before_a_drop_is_no_record_of_the_id_made_again(void);
extern void test_chunks_packs_open_tests_a_record_made_again_from_the_list_files_lists_what_the_last_one_did(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_OPEN_H */
