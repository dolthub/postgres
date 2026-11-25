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
 * A repository's packs as one backend reads and writes them (chunks'
 * packs/backend.rs).
 */
#ifndef DOLTGRES_CHUNKS_PACKS_BACKEND_H
#define DOLTGRES_CHUNKS_PACKS_BACKEND_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs/lists.h"
#include "doltgres/chunks/packs/retire.h"
#include "doltgres/chunks/packs/writers.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lock.h"
#include "doltgres/host/shared.h"

/* ChunksPacksBackendPacks is Packs: a repository's packs, as a backend reads and writes them. */
typedef struct ChunksPacksBackendPacks ChunksPacksBackendPacks;

/* ChunksPacksBackendViewMutex is a backend's view of the packs under its LongMutex, which backend.c alone reads. */
typedef struct ChunksPacksBackendViewMutex ChunksPacksBackendViewMutex;

struct ChunksPacksBackendPacks
{
	HostSharedHeld held;

	/*
	 * location is the record's location, which names the repository's packs
	 * to the node cache while the server runs.
	 */
	uint64		location;
	/* view is the backend's view under its lock, in a box of its own. */
	ChunksPacksBackendViewMutex *view;
	/* repository is the repository's id, which the record carries. */
	uint64		repository;
	/* own is the id of the tablespace the repository's own directory is in. */
	uint32		own;
};

/* ChunksPacksBackendKept is Kept: one of a repository's kept files. */
typedef enum ChunksPacksBackendKept
{
	/*
	 * CHUNKS_PACKS_BACKEND_KEPT_MANIFEST is the manifest, which names the
	 * tree of the storages' bases.
	 */
	CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
} ChunksPacksBackendKept;

/* chunks_packs_backend_Packs_forward_moves_t is forward's (Locator, Locator): a move's from and to. */
typedef struct chunks_packs_backend_Packs_forward_moves_t
{
	ChunksTrailerLocator f0;
	ChunksTrailerLocator f1;
} chunks_packs_backend_Packs_forward_moves_t;

extern bool chunks_packs_backend_copied_file(HostIoIoDoors io, const char *from_path, size_t from_path_len,
											 const char *to_path, size_t to_path_len, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_close(ChunksPacksBackendPacks *self, HostIoIo *io,
											 const HostSharedReader *reading, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_flush(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
											 const HostSharedReader *reading, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_copied(const ChunksPacksBackendPacks *self, HostIoIo *io,
											  const HostSharedReader *reading, const char *into, size_t into_len,
											  ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_created(const ChunksPacksBackendPacks *self, HostIoIo *io,
											   const HostSharedReader *reading, uint32 database, uint32 own,
											   ChunksErrorError *error);
extern uint64 chunks_packs_backend_Packs_location(const ChunksPacksBackendPacks *self);
extern bool chunks_packs_backend_Packs_find(const ChunksPacksBackendPacks *self, HostIoIo *io,
											const HostSharedReader *reading, const ChunksAddressAddress *address,
											bool *has_result, ChunksTrailerLocator *result, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_records(const ChunksPacksBackendPacks *self, HostIoIo *io,
											   const HostSharedReader *reading, const ChunksAddressAddress *address,
											   StdVec *result, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_find_in(const ChunksPacksBackendPacks *self, HostIoIo *io,
											   const HostSharedReader *reading, const ChunksAddressAddress *address,
											   uint32 space, bool *has_result, ChunksTrailerLocator *result,
											   ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_read(const ChunksPacksBackendPacks *self, HostIoIo *io,
											const HostSharedReader *reading, const ChunksAddressAddress *address,
											ChunksTrailerLocator locator, ChunksChunkChunk *result,
											ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_forwarded(const ChunksPacksBackendPacks *self, HostIoIo *io,
												 const HostSharedReader *reading, ChunksTrailerLocator locator,
												 bool *has_result, ChunksTrailerLocator *result,
												 ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_with_prefix(const ChunksPacksBackendPacks *self, HostIoIo *io,
												   const HostSharedReader *reading, const char *prefix,
												   size_t prefix_len, StdVec *result, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_numbers(const ChunksPacksBackendPacks *self, HostIoIo *io,
											   const HostSharedReader *reading, StdVec *result,
											   ChunksErrorError *error);

/*
 * chunks_packs_backend_Packs_spaces is spaces: its BTreeMap<u64, u32> is a
 * StdVec of std_TupleU64U32_t in the numbers' order.
 */
extern bool chunks_packs_backend_Packs_spaces(const ChunksPacksBackendPacks *self, HostIoIo *io,
											  const HostSharedReader *reading, StdVec *result,
											  ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_newest_vector(const ChunksPacksBackendPacks *self, HostIoIo *io,
													 const HostSharedReader *reading, uint64 *result,
													 ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_holds(const ChunksPacksBackendPacks *self, HostIoIo *io,
											 const HostSharedReader *reading, const ChunksPackPackId *ids,
											 size_t ids_len, bool *result, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_pack_reader(const ChunksPacksBackendPacks *self, HostIoIo *io,
												   const HostSharedReader *reading, uint64 number, bool *has_result,
												   StdArcInner **result, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_writer(const ChunksPacksBackendPacks *self, HostIoIo *io,
											  const HostSharedReader *reading, uint32 space,
											  ChunksPackPackWriterInDir *result, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_kept_from(const ChunksPacksBackendPacks *self, uint64 *result,
												 ChunksErrorError *error);
extern uint64 chunks_packs_backend_Packs_reserved(const ChunksPacksBackendPacks *self);
extern bool chunks_packs_backend_Packs_writing(const ChunksPacksBackendPacks *self, HostIoIo *io, uint64 number);
extern bool chunks_packs_backend_Packs_add(const ChunksPacksBackendPacks *self, HostIoIo *io,
										   const HostSharedReader *reading, ChunksPackPack *pack,
										   ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_add_deferred(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
													const HostSharedReader *reading, ChunksPackPack *pack,
													ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_add_still_writing(const ChunksPacksBackendPacks *self, HostIoIo *io,
														 const HostSharedReader *reading, ChunksPackPack *pack,
														 ChunksPacksWritersWriting *result,
														 ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_current(const ChunksPacksBackendPacks *self, HostIoIo *io,
											   const HostSharedReader *reading, const ChunksTrailerLocator *locators,
											   size_t locators_len, bool *has_result, StdVec *result,
											   ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_advise(const ChunksPacksBackendPacks *self, HostIoIo *io,
											  const HostSharedReader *reading, ChunksTrailerLocator locator,
											  ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_forward(const ChunksPacksBackendPacks *self, HostIoIo *io,
											   const HostSharedReader *reading,
											   const chunks_packs_backend_Packs_forward_moves_t *moves,
											   size_t moves_len, ChunksPacksRetireForwarded *result,
											   ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_compact_lists(const ChunksPacksBackendPacks *self, HostIoIo *io,
													 const HostSharedReader *reading, ChunksErrorError *error);
extern uint64 chunks_packs_backend_Packs_rotations(const ChunksPacksBackendPacks *self);
extern bool chunks_packs_backend_Packs_write_kept(const ChunksPacksBackendPacks *self, HostIoIo *io,
												  ChunksPacksBackendKept kept, const uint8 *bytes, size_t bytes_len,
												  ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_write_kept_if(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
													 ChunksPacksBackendKept kept, const uint8 *bytes,
													 size_t bytes_len, uint64 rotations, bool *result,
													 ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_read_kept(const ChunksPacksBackendPacks *self, HostIoIo *io,
												 ChunksPacksBackendKept kept, bool *has_result, StdVec *result_0,
												 bool *has_result_1, ChunksFileFallback *result_1,
												 ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_read_previous(const ChunksPacksBackendPacks *self, HostIoIo *io,
													 ChunksPacksBackendKept kept, bool *has_result, StdVec *result,
													 ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_merge_runs(const ChunksPacksBackendPacks *self, HostIoIo *io,
												  const HostSharedReader *reading, bool *result,
												  ChunksErrorError *error);
extern void chunks_packs_backend_Packs_drop(ChunksPacksBackendPacks *self);

/*
 * The packs' functions other modules of packs call: over takes the held
 * record, which it drops on an error.
 */
/* chunks_packs_backend_Read is a read over the backend's view, brought up to date, with the entry's Io. */
typedef bool (*chunks_packs_backend_Read) (void *captures, ChunksPacksViewView *view, HostIoIo *io,
										   ChunksErrorError *error);

/* chunks_packs_backend_Change is a change to the list files under the lists lock. */
typedef bool (*chunks_packs_backend_Change) (void *captures, ChunksPacksListsChange *change, ChunksErrorError *error);

extern HostLockLending chunks_packs_backend_Packs_view_lock(const ChunksPacksBackendPacks *self, HostIoTakesShort io);
extern bool chunks_packs_backend_Packs_viewed(const ChunksPacksBackendPacks *self, HostIoIo *io,
											  chunks_packs_backend_Read read, void *captures, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_changed(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
											   const HostSharedReader *reading, chunks_packs_backend_Change change,
											   void *captures, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_over(HostIoIo *io, HostSharedHeld *held, uint64 location,
											ChunksPacksBackendPacks *result, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_unreserve(const ChunksPacksBackendPacks *self, HostIoIo *io,
												 const HostSharedReader *reading, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_even_manifest(const ChunksPacksBackendPacks *self, HostIoIo *io,
													 ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_compact(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
											   const HostSharedReader *reading, bool all, ChunksErrorError *error);
extern bool chunks_packs_backend_Packs_remove_space(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
													const HostSharedReader *reading, uint32 space,
													ChunksErrorError *error);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_backend_tests_a_collection_keeps_every_pack_from_the_lowest_a_process_has_open(void);
extern void test_chunks_packs_backend_tests_a_process_writes_the_packs_of_one_repository_at_a_time(void);
extern void test_chunks_packs_backend_tests_a_collection_reads_the_number_another_backends_writer_has_open(void);
extern void test_chunks_packs_backend_tests_a_kept_file_written_by_one_backend_reads_in_another(void);
extern void test_chunks_packs_backend_tests_a_read_of_a_kept_file_at_any_step_of_its_write_returns_one_whole_generation(void);
extern void test_chunks_packs_backend_tests_a_collection_between_a_writers_slot_and_its_number_keeps_the_writers_pack(void);
extern void test_chunks_packs_backend_tests_a_terminated_backends_exit_removes_its_dropped_writers_file_and_appends_its_deferred_adds(void);
extern void test_chunks_packs_backend_tests_a_failed_append_at_a_backends_exit_leaves_the_add_for_the_next_close(void);
extern void test_chunks_packs_backend_tests_a_failed_allocation_in_a_packs_over_keeps_the_process_list(void);
extern void test_chunks_packs_backend_tests_a_writers_number_is_under_a_bound_the_table_holds(void);
extern void test_chunks_packs_backend_tests_a_repository_at_its_last_number_refuses_a_writer(void);
extern void test_chunks_packs_backend_tests_a_manifests_write_marks_the_record_while_its_generations_may_differ(void);
extern void test_chunks_packs_backend_tests_a_manifest_written_once_more_never_undoes_another_write_of_it(void);
extern void test_chunks_packs_backend_tests_a_conditional_write_of_the_manifest_lands_only_over_the_one_it_read(void);
extern void test_chunks_packs_backend_tests_packs_closed_in_another_backend_are_refused(void);
extern void test_chunks_packs_backend_tests_a_copy_copies_a_directory_of_the_repository_as_a_directory(void);
extern void test_chunks_packs_backend_tests_two_backends_packs_over_one_record_list_what_either_adds(void);
extern void test_chunks_packs_backend_tests_a_merge_with_no_room_for_its_replaced_runs_is_not_started(void);
extern void test_chunks_packs_backend_tests_a_view_keeps_the_packs_of_a_missing_tablespace_unread_until_it_is_back(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_BACKEND_H */
