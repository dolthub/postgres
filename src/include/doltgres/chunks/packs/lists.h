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
 * A change to a repository's list files (chunks' packs/lists.rs), which only
 * the packs' own modules make.
 */
#ifndef DOLTGRES_CHUNKS_PACKS_LISTS_H
#define DOLTGRES_CHUNKS_PACKS_LISTS_H

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/packs/view.h"
#include "doltgres/chunks/trace.h"

/* ChunksPacksListsRotating is Rotating: what a change did to the record's count of rotations. */
typedef enum ChunksPacksListsRotating
{
	CHUNKS_PACKS_LISTS_ROTATING_NO,
	CHUNKS_PACKS_LISTS_ROTATING_ODD,
	CHUNKS_PACKS_LISTS_ROTATING_RENAMED,
} ChunksPacksListsRotating;

/*
 * ChunksPacksListsChange is Change: a change to a repository's list files
 * under way, the lists lock, the changer's view, current to the record, and
 * the snapshot it installs.
 */
typedef struct ChunksPacksListsChange
{
	ChunksPacksViewView *view;
	HostSharedRecord record;
	HostSharedLongGuard long_;
	ChunksPacksSharedSnapshot snapshot;
	bool		installed;
	ChunksPacksListsRotating rotating;
#ifdef DOLTGRES_CHUNKS_TRACE

	/*
	 * noted is a Vec<Event> of the transitions the change makes, which its
	 * install records.
	 */
	StdVec		noted;
#endif
} ChunksPacksListsChange;

extern ChunksFileTornTail chunks_packs_lists_end(const ChunksPacksViewListOps *ops, const ChunksPacksViewRead *read,
												 const void *list, const char *path, size_t path_len);
extern bool chunks_packs_lists_written(const ChunksPacksViewListOps *ops, HostIoIoDoors io, const char *path,
									   size_t path_len, void *list, ChunksPacksViewRead *read, bool *placing,
									   ChunksErrorError *error);
extern ChunksPacksSharedListTail chunks_packs_lists_tail_of(const ChunksPacksViewListOps *ops,
															const ChunksPacksViewRead *read, const void *list);
extern bool chunks_packs_lists_past_slack(const ChunksPacksViewView *view);
extern bool chunks_packs_lists_Change_begin(ChunksPacksViewView *view, HostSharedRecord record,
											HostSharedLongGuard *long_, ChunksPacksListsChange *result,
											ChunksErrorError *error);
extern void chunks_packs_lists_Change_note(ChunksPacksListsChange *self,
										   void (*event) (void *captures, ChunksTraceEvent *result), void *captures);
extern bool chunks_packs_lists_Change_add(ChunksPacksListsChange *self, const ChunksPacksTableEntry *entry,
										  bool defer, ChunksErrorError *error);
extern bool chunks_packs_lists_Change_flush(ChunksPacksListsChange *self, ChunksErrorError *error);
extern bool chunks_packs_lists_Change_change_runs(ChunksPacksListsChange *self, uint32 space, const uint8 *record,
												  size_t record_len, ChunksErrorError *error);
extern bool chunks_packs_lists_Change_change_forwarding(ChunksPacksListsChange *self, const uint8 *record,
														size_t record_len, ChunksErrorError *error);
extern bool chunks_packs_lists_Change_lists_together(const ChunksPacksListsChange *self, uint32 space,
													 const StdString *names, size_t names_len);
extern bool chunks_packs_lists_Change_name_vector(ChunksPacksListsChange *self, uint64 number, uint64 generation,
												  ChunksErrorError *error);
extern bool chunks_packs_lists_Change_reserve(ChunksPacksListsChange *self, uint64 read, ChunksErrorError *error);
extern bool chunks_packs_lists_Change_unreserve(ChunksPacksListsChange *self, ChunksErrorError *error);
extern bool chunks_packs_lists_Change_retire(ChunksPacksListsChange *self, const uint64 *numbers,
											 size_t numbers_len, ChunksErrorError *error);
extern bool chunks_packs_lists_Change_install(ChunksPacksListsChange *self, ChunksErrorError *error);
extern void chunks_packs_lists_Change_drop(ChunksPacksListsChange *self);
extern bool chunks_packs_lists_Rewrite_begin(ChunksPacksViewView *view, HostSharedRecord record,
											 HostSharedFlagIo *io, ChunksPacksListsChange *result,
											 ChunksErrorError *error);
extern bool chunks_packs_lists_Rewrite_compact(ChunksPacksListsChange *self, bool all, ChunksErrorError *error);
extern bool chunks_packs_lists_Rewrite_remove_space(ChunksPacksListsChange *self, uint32 space,
													ChunksErrorError *error);

#ifdef DOLTGRES_TEST_CHUNKS
extern HostSharedOwned chunks_packs_lists_tests_repository(HostIoIo *io, const StdPathBuf *dir);
extern void chunks_packs_lists_tests_changed(HostIoIo *io, HostSharedRecord record, ChunksPacksViewView *view,
											 void (*body) (ChunksPacksListsChange *change, void *captures),
											 void *captures);
extern void test_chunks_packs_lists_tests_a_pack_added_is_listed_by_every_view(void);
extern void test_chunks_packs_lists_tests_deferred_adds_are_appended_by_the_add_that_passes_their_bound(void);
extern void test_chunks_packs_lists_tests_the_first_pack_of_a_tablespace_writes_its_runs_file(void);
extern void test_chunks_packs_lists_tests_a_change_dropped_before_its_install_changes_nothing_a_view_lists(void);
extern void test_chunks_packs_lists_tests_list_files_written_whole_list_what_they_listed(void);
extern void test_chunks_packs_lists_tests_only_a_list_file_past_its_slack_is_written_whole(void);
extern void test_chunks_packs_lists_tests_a_retirements_records_lead_every_view_to_the_copies(void);
extern void test_chunks_packs_lists_tests_a_refresh_across_a_whole_write_lists_the_packs_before_it_or_after_it(void);
extern void test_chunks_packs_lists_tests_a_whole_write_that_fails_before_its_renames_leaves_the_count_even(void);
extern void test_chunks_packs_lists_tests_a_removed_tablespace_reads_a_runs_file_made_again_from_its_start(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_LISTS_H */
