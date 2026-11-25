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
 * VACUUM's and a prune's frees of a storage's tuple ids (txn's vacuum.rs):
 * the tuple ids no version holds, their frees and their redo, and a
 * take-time prune of a block.
 */
#ifndef DOLTGRES_TXN_VACUUM_H
#define DOLTGRES_TXN_VACUUM_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/record.h"

/*
 * TXN_VACUUM_LINE_POINTER is the bytes a heap line pointer takes, which
 * PageGetHeapFreeSpace keeps back for a new tuple's.
 */
#define TXN_VACUUM_LINE_POINTER UINT64CONST(4)

/*
 * TxnVacuumFreed is Freed: a tuple id never_held answers, its heap bytes,
 * its free's conflict horizon, the value ids the free takes down, a
 * Vec<u32>, and its row's stored ender, as the pending tree stores it, which
 * its record names (bugs row 320).
 */
typedef struct TxnVacuumFreed
{
	uint64		f0;
	uint64		f1;
	uint64		f2;
	StdVec		f3;
	uint64		f4;
} TxnVacuumFreed;

/*
 * TxnVacuumPrune is Prune: what a take-time prune reads a block's rows
 * under, the relation's NonVacuumable snapshot and a SnapshotAny.
 */
struct TxnVacuumPrune
{
	const HostSnapshot *horizon;
	const HostSnapshot *any;
};

extern bool txn_vacuum_never_held(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
								  const TxnInsertPage *page, const HostSnapshot *horizon, StdVec *result,
								  bool *left_aborted, HostError *error);

/*
 * TxnVacuumLeaving is a take a fold's drop takes out of the delta, as
 * VACUUM's find reads it: its tuple id, its take's writer, the bytes its take
 * lowered, its stored ender, and its row's committed value ids.
 */
typedef struct TxnVacuumLeaving
{
	uint64		tid;
	TxnMemtableVersionWriter writer;
	uint64		bytes;
	uint64		ender;
	/* ids is a Vec<u32>. */
	StdVec		ids;
} TxnVacuumLeaving;

extern bool txn_vacuum_leaving(HostIoIo *io, TreesStoreNodeStore store, const TxnBaseBase *base,
							   const TxnInsertPage *page, TxnMemtableVersionVersion *run, size_t run_len,
							   StdVec *result, HostError *error);
extern void txn_vacuum_leaving_drop(StdVec *leaving);
extern bool txn_vacuum_frees(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							 const TxnBaseBase *base, uint64 tid, uint64 point, bool *result, HostError *error);
extern bool txn_vacuum_spent(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 block,
							 const TxnInsertPage *page, uint64 *result, HostError *error);
extern bool txn_vacuum_freed(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							 const HostSharedHeld *toast, HostStorage toast_storage, HostStorage storage, bool logged,
							 const TxnInsertPage *page, const TxnVacuumFreed *tids, size_t tids_len, bool dead,
							 StdVec *result, HostError *error);
extern bool txn_vacuum_pruned(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							  HostStorage storage, bool logged, const TxnInsertPage *page, uint32 block,
							  uint64 reserve, const TxnVacuumPrune *prune, HostError *error);
extern bool txn_vacuum_held(HostIoIo *io, HostSharedRecord record, uint64 tid, bool *result, HostError *error);
extern bool txn_vacuum_redo_freed(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
								  const HostSharedHeld *toast, const TxnRecordTidsFreed *freed,
								  const TxnInsertPage *page, uint64 lsn, const TxnInsertRun *run, HostError *error);
extern bool txn_vacuum_taken_out(HostIoIo *io, const HostSharedHeld *held, const TxnRecordTidsFreed *freed,
								 uint64 lsn, HostError *error);

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/simc/simc.h"

extern SimcTaken txn_vacuum_tests_every(void);
extern bool txn_vacuum_tests_all_returned(const SimcReport *report);
extern void txn_vacuum_tests_returned(const SimcReport *report, const char *file, uint32 ended_line,
									  uint32 ended_col, uint32 line, uint32 col);
extern void test_txn_vacuum_tests_an_aborted_inserts_tuple_id_on_the_fresh_page_is_freed_with_its_own_bytes_and_taken_again(void);
extern void test_txn_vacuum_tests_a_take_that_empties_its_entry_is_freed_with_all_the_bytes_the_entry_held(void);
extern void test_txn_vacuum_tests_a_take_no_row_names_is_freed_only_once_its_taker_has_ended(void);
extern void test_txn_vacuum_tests_an_aborted_leaves_old_offsets_and_its_new_tuple_id_are_handed_out_again(void);
extern void test_txn_vacuum_tests_a_skipped_index_pass_gives_back_a_dead_tuple_ids_bytes_and_a_later_free_its_offset_alone(void);
extern void test_txn_vacuum_tests_a_dead_tuple_id_on_a_full_block_keeps_its_bytes_until_its_free(void);
extern void test_txn_vacuum_tests_a_take_prunes_a_full_blocks_dead_versions_and_its_update_stays_on_the_block(void);
extern void test_txn_vacuum_tests_a_take_leaves_a_version_an_open_snapshot_may_see(void);
extern void test_txn_vacuum_tests_a_dead_tuple_id_is_claimed_once(void);
extern void test_txn_vacuum_tests_a_refused_markers_redo_keeps_the_version_as_the_primary_does(void);
extern void test_txn_vacuum_tests_every_raise_of_a_blocks_room_lowers_its_categorys_hint(void);
extern void test_txn_vacuum_tests_a_folded_rows_tuple_id_is_not_freed(void);
extern void test_txn_vacuum_tests_a_fold_between_the_finds_reads_leaves_its_rows_held(void);
extern void test_txn_vacuum_tests_a_fold_between_a_prunes_reads_leaves_what_a_prune_after_it_leaves(void);
extern void test_txn_vacuum_tests_a_fold_during_a_blocks_spent_count_counts_what_a_count_after_it_counts(void);
extern void test_txn_vacuum_tests_a_fold_during_a_find_finds_what_a_find_after_it_finds(void);
extern void test_txn_vacuum_tests_a_fold_during_a_free_leaves_what_a_free_after_it_leaves(void);
extern void test_txn_vacuum_tests_a_fold_during_a_frees_redo_leaves_what_the_free_left(void);
extern void test_txn_vacuum_tests_a_fold_during_a_frees_redo_leaves_what_a_redo_after_it_leaves(void);
extern void test_txn_vacuum_tests_a_dropped_rows_tuple_id_keeps_its_enders_xid_for_its_frees_horizon(void);
extern void test_txn_vacuum_tests_a_free_of_a_block_whose_takes_all_spilled_starts_from_its_newest_spilled_entry(void);
extern void test_txn_vacuum_tests_a_takes_ender_widens_against_its_writer_across_an_epoch(void);
extern void test_txn_vacuum_tests_a_dropped_rows_tuple_id_waits_for_the_horizon_to_pass_its_ender(void);
extern void test_txn_vacuum_tests_a_row_its_own_transaction_inserted_and_deleted_waits_for_the_horizon(void);
extern void test_txn_vacuum_tests_a_drops_ender_goes_on_its_rows_take_not_a_later_take_of_the_tuple_id(void);
extern void test_txn_vacuum_tests_a_frees_take_out_leaves_a_retake_on_its_key(void);
extern void test_txn_vacuum_tests_a_frees_take_out_with_no_ender_leaves_any_retake_on_its_key(void);
extern void test_txn_vacuum_tests_a_frees_take_out_takes_out_a_subtransactions_delete(void);
extern void test_txn_vacuum_tests_a_frees_enders_of_another_form_are_refused_as_corruption(void);
extern void test_txn_vacuum_tests_a_prunes_take_out_of_an_aborted_writers_own_row_leaves_a_retake(void);
extern void test_txn_vacuum_tests_a_prune_over_an_aborted_unfolded_insert_claims_nothing(void);
extern void test_txn_vacuum_tests_a_fold_around_an_open_writer_records_a_fresh_end_past_its_rows(void);
extern void test_txn_vacuum_tests_a_start_past_a_base_folded_after_its_checkpoint_counts_from_the_checkpoints_cut(void);
extern void test_txn_vacuum_tests_a_start_past_a_base_folded_after_its_checkpoint_counts_from_its_cuts_runs_entry(void);
extern void test_txn_vacuum_tests_a_start_past_a_base_of_a_storage_made_after_its_checkpoint_counts_from_empty(void);
extern void test_txn_vacuum_tests_a_start_past_the_first_save_after_a_checkpoint_that_passed_its_repository_over_counts_from_its_record(void);
extern void test_txn_vacuum_tests_a_start_past_a_copys_base_folded_after_its_runs_counts_from_its_templates_record(void);
extern void test_txn_vacuum_tests_a_removal_no_take_holds_leaves_its_block_an_untouched_pages_bytes(void);
#endif

#endif							/* DOLTGRES_TXN_VACUUM_H */
