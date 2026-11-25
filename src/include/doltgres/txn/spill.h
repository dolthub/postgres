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
 * A transaction's spills of its versions (txn's spill.rs): the records whose
 * spills a manifest lists, the files it lists, each process's pending spill,
 * the spills' threshold, and the reads, spills, and settles of a storage's
 * spilled versions. Part of txn's contract.
 */
#ifndef DOLTGRES_TXN_SPILL_H
#define DOLTGRES_TXN_SPILL_H

#include "doltgres/host/delta.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/runs.h"
#include "doltgres/txn/written.h"

/*
 * TxnSpillHomeOps is the Home trait: a record whose spills a manifest lists.
 * The other half only passes its instances through, so its definition comes
 * with spill.c's body.
 */
typedef struct TxnSpillHomeOps TxnSpillHomeOps;

/* TXN_SPILL_STATE_HOME_OPS is State's Home: a storage's state, whose rows and value map spill. */
extern const TxnSpillHomeOps TXN_SPILL_STATE_HOME_OPS;

/* TXN_SPILL_BRANCH_DELTAS_HOME_OPS is BranchDeltas' Home: a working state's delta, whose chunk tree spills. */
extern const TxnSpillHomeOps TXN_SPILL_BRANCH_DELTAS_HOME_OPS;

/* TXN_SPILL_IDENTITY_HOME_OPS is the identity spills' Home: a working state's delta, whose identity tree spills (bugs row 291). */
extern const TxnSpillHomeOps TXN_SPILL_IDENTITY_HOME_OPS;

/* TxnSpillFilter is Filter: a spill's Bloom filter while it is built, in the backend's memory, its words of bits. */
typedef struct TxnSpillFilter
{
	/* words is a Vec<u64>. */
	StdVec		words;
} TxnSpillFilter;

/*
 * TxnSpillFile is File: what names a spill's file, its spilling transaction's
 * top-level xid, its kind, its number among that transaction's files of its
 * storage and tree, its length, its trailer's xxh3, the count of its filter's
 * bits, for a free space map's spill its transaction's abort kept, the end of
 * that abort, or for a carry's file of frozen versions its passing point, and,
 * for a fold's residual of a map's spills, the fold's point.
 */
typedef struct TxnSpillFile
{
	uint64		xid;
	TxnRunsKind kind;
	uint32		number;
	uint64		len;
	uint64		hash;
	uint64		bits;

	/*
	 * passed is the end of the abort that kept a free space map's spill, or
	 * the newest end of a carry's frozen versions' records, 1 for versions
	 * with none, the point a fold passes it at, or 0 for none.
	 */
	uint64		passed;

	/*
	 * from is the point of the fold whose residual the file is, from which
	 * readers read it in its sources' place, or 0 for a file that is no
	 * residual.
	 */
	uint64		from;
} TxnSpillFile;

/* txn_TupleVecFileU64_t is listed's (Vec<File>, u64): a storage's spill files, newest first, and the generation they were read at. */
typedef struct txn_TupleVecFileU64_t
{
	/* f0 is a Vec<File>. */
	StdVec		f0;
	uint64		f1;
} txn_TupleVecFileU64_t;

/* TXN_SPILL_MANIFEST is MANIFEST, the start of the name of a storage's spills' manifest file. */
#define TXN_SPILL_MANIFEST "spills-"

/* txn_TupleU64KindU32OptionU32_t is Slotted, (u64, Kind, u32, Option<u32>): a spill a process's slot names, its xid, kind, and number, and, for a merge, the lowest number of the files it replaces. */
typedef struct txn_TupleU64KindU32OptionU32_t
{
	uint64		f0;
	TxnRunsKind f1;
	uint32		f2;
	bool		has_f3;
	uint32		f3;
} txn_TupleU64KindU32OptionU32_t;

/*
 * TxnSpillKept is Kept: a spilled version of a storage's map that a fold at
 * a point leaves unapplied above an older running writer's (bugs row 228):
 * its kind, its key, and its record's bytes.
 */
typedef struct TxnSpillKept
{
	TxnRunsKind kind;
	StdVec		key;
	StdVec		bytes;
} TxnSpillKept;

/*
 * TxnSpillChain is Chain: a key of a tree a carry writes to spill files: the
 * tree, the key, and its versions oldest first, each its writer's top-level
 * xid and its record.
 */
typedef struct TxnSpillChain
{
	TxnWrittenOf of;
	StdVec		key;
	/* versions is a Vec<(u64, Vec<u8>)>. */
	StdVec		versions;
} TxnSpillChain;

/* TXN_SPILL_PENDING_PROC_SLOTS_OPS is Pending's ProcSlots: each process's slot naming its one spill no manifest lists yet. */
extern const HostSharedProcSlotsOps TXN_SPILL_PENDING_PROC_SLOTS_OPS;

extern bool txn_spill_listed(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record,
							 const HostStorage *storage, txn_TupleVecFileU64_t *result, HostError *error);
extern uint64 txn_spill_threshold(void);
extern bool txn_spill_relieved(HostIoIo *io, bool *result, HostError *error);
extern bool txn_spill_relieved_own(HostIoIo *io, bool *result, HostError *error);
extern bool txn_spill_replayed(HostIoIo *io, const HostStorage *storage, TxnWrittenOf of, uint64 xid, size_t bytes,
							   uint64 *result, HostError *error);
extern bool txn_spill_spilled(HostIoIo *io, const HostStorage *storage, TxnWrittenOf of, uint64 xid, bool *result,
							  HostError *error);
extern bool txn_spill_folded(HostIoIo *io, HostSharedRecord state, HostLsn point, HostError *error);
extern bool txn_spill_manifest_moved(HostIoIo *io, HostSharedRecord record, const HostStorage *storage,
									 HostError *error);
extern bool txn_spill_residual(HostIoIo *io, HostSharedRecord state, HostLsn point, const TxnSpillKept *kept,
							   size_t kept_len, StdVec *result, HostError *error);
extern bool txn_spill_unlisted(HostIoIo *io, HostSharedRecord state, const StdVec *residuals, HostError *error);
extern void txn_spill_Chain_drop(TxnSpillChain *self);

struct TxnRecordCarryUnlisted;

extern bool txn_spill_redo_unlisted(HostIoIo *io, const HostSharedHeld *held,
									const struct TxnRecordCarryUnlisted *unlisted, HostError *error);
extern bool txn_spill_carried(HostIoIo *io, const HostSharedHeld *held, const uint64 *writers, size_t writers_len,
							  bool (*logged) (void *captures, HostSharedLongIo *io, StdVec *chains,
											  HostError *error),
							  void *captures, HostError *error);
extern bool txn_spill_versions(const TxnSpillHomeOps *home, HostIoIo *io, const HostDeltaHold *hold,
							   HostSharedRecord state, TxnRunsKind kind, const uint8 *key, size_t key_len,
							   uint64 *listed, StdVec *result, HostError *error);
extern bool txn_spill_ordered(HostIoIo *io, StdVec *chain, StdVec *spilled, HostError *error);
extern bool txn_spill_next(const TxnSpillHomeOps *home, HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state,
						   TxnRunsKind kind, bool has_after, const uint8 *after, size_t after_len,
						   TxnMemtableTableDirection way, uint64 *listed, bool *some, txn_TupleVecU8VecVersion_t *result,
						   HostError *error);
extern bool txn_spill_taken(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record,
							bool (*read) (void *captures, uint64 *listed, HostError *error),
							void (*dropped) (void *captures), void *captures, HostError *error);
extern bool txn_spill_holds(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord state, TxnRunsKind kind,
							const uint8 *key, size_t key_len, uint64 lsn, bool *result, HostError *error);
extern uint64 txn_spill_generation(const TxnSpillHomeOps *home, HostSharedRecord state);
extern bool txn_spill_newest_value(HostIoIo *io, HostSharedRecord state, uint32 id, bool *some,
								   TxnMemtableMapEntry *result, HostError *error);
extern bool txn_spill_settle_pending(HostIoIo *io, HostError *error);
extern bool txn_spill_take_settled(HostIoIo *io, HostError *error);
extern bool txn_spill_rolling(HostIoIo *io, HostSharedRecord record, bool *marked, HostError *error);
extern bool txn_spill_rolled(HostIoIo *io, HostSharedRecord record, HostError *error);
extern bool txn_spill_wrote(HostIoIo *io, uint64 top, uint64 bytes, HostError *error);
extern bool txn_spill_unwrote(HostIoIo *io, bool has_bytes, uint64 bytes, HostError *error);
extern bool txn_spill_aborted(HostIoIo *io, uint64 xid, const HostStorage *storages, size_t storages_len,
							  HostLsn end, HostError *error);
extern bool txn_spill_spilled_free(HostIoIo *io, uint64 xid, HostError *error);
extern bool txn_spill_redo_aborted(HostIoIo *io, uint64 xid, HostLsn end, HostError *error);
extern bool txn_spill_redo_prepared(HostIoIo *io, bool (*ended) (void *captures, HostIoIo *io, uint64 top,
																 bool *result, HostError *error),
									void *ended_captures, HostError *error);
extern bool txn_spill_chunks_repository(const HostStorage *storage, uint32 *result);
extern bool txn_spill_Filter_of(size_t count, const StdBytes *keys, size_t keys_len, TxnSpillFilter *result,
								HostError *error);
extern bool txn_spill_Filter_hits(const TxnSpillFilter *self, const uint8 *key, size_t key_len);
extern HostStorage txn_spill_chunks_storage(uint32 repository);
extern HostStorage txn_spill_identity_storage(uint32 repository);
extern bool txn_spill_identity_applied(HostIoIo *io, uint64 xid, HostLsn point, bool *result, HostError *error);
extern bool txn_spill_identity_applies(HostIoIo *io, HostSharedRecord deltas, HostLsn point, bool *result,
									   HostError *error);
extern bool txn_spill_identity_folded(HostIoIo *io, HostSharedRecord deltas, HostLsn point, bool *took,
									  HostError *error);
extern bool txn_spill_chunk_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord deltas, const uint8 *key,
									 size_t key_len, StdVec *result, HostError *error);
extern bool txn_spill_chunk(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len, bool *some,
							StdVec *result, HostError *error);
extern bool txn_spill_removed(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record,
							  const HostStorage *storage, bool (*picked) (void *captures, const TxnSpillFile *file),
							  void *picked_captures, StdVec *result, HostError *error);
extern bool txn_spill_linked(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record,
							 const HostStorage *storage, const TxnSpillFile *linking, size_t linking_len,
							 bool has_replacing, txn_TupleU64KindU32OptionU32_t replacing, HostError *error);
extern bool txn_spill_twinned(HostIoIo *io, HostSharedRecord state, TxnRunsKind kind, const uint8 *key,
							  size_t key_len, const uint8 *bytes, size_t bytes_len, bool *result, HostError *error);
extern uint64 txn_spill_counted(const TxnRunsDecoded *decoded);
#ifdef USE_INJECTION_POINTS
extern uint64 txn_spill_filter_pages_read;
extern uint64 txn_spill_reads_opened;
#endif
extern void txn_spill_loading(uint32 repository);
extern bool txn_spill_chunks_kept(HostIoIo *io, uint32 repository, HostXid xid, bool *result, HostError *error);
extern bool txn_spill_chunks_ended(HostIoIo *io, HostSharedRecord deltas, bool *result, HostError *error);
extern bool txn_spill_chunks_ended_of(HostIoIo *io, const uint32 *repositories, size_t repositories_len, bool *result,
									  HostError *error);
extern bool txn_spill_named(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord state,
							const HostStorage *storage, HostLsn redo, StdVec *result, HostError *error);
extern bool txn_spill_tuple_key(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, HostRow tid,
								bool *some, StdVec *result, HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_spill_tests_a_merges_rewrite_and_another_transactions_listing_lose_nothing(void);
extern void test_txn_spill_tests_a_reader_paused_in_a_manifest_read_finds_each_version_once(void);
extern void test_txn_spill_tests_a_lookup_reads_an_evicted_filter_page_again(void);
extern void test_txn_spill_tests_a_spilled_free_space_maps_entries_are_read_below_the_memtable(void);
extern void test_txn_spill_tests_an_aborted_free_space_map_spill_stays_listed_until_a_fold(void);
extern void test_txn_spill_tests_the_committed_arm_spills_a_keys_oldest_committed_versions_above_its_spills(void);
extern void test_txn_spill_tests_the_committed_arms_spill_reads_back_in_order_after_a_restart(void);
extern void test_txn_spill_tests_a_free_space_spill_a_crash_ended_is_marked_with_the_next_checkpoints_redo_point(void);
extern void test_txn_spill_tests_a_prepared_transactions_spills_are_never_taken_out(void);
extern void test_txn_spill_tests_a_free_space_spills_merge_leaves_the_rows_tuple_ids_alone(void);
extern void test_txn_spill_tests_a_transaction_spilling_more_storages_than_the_budget_holds_pages_completes(void);
extern void test_txn_spill_tests_two_transactions_spilling_the_same_storages_crosswise_complete(void);
extern void test_txn_spill_tests_more_transactions_than_any_fixed_count_spill_one_storage_at_a_full_budget(void);
extern void test_txn_spill_tests_redo_over_budget_spills_the_largest_replayed_transaction(void);
extern void test_txn_spill_tests_a_second_spill_merges_with_the_first_keeping_its_versions_order(void);
extern void test_txn_spill_tests_a_small_budgets_threshold_is_an_eighth_of_it(void);
extern void test_txn_spill_tests_a_process_with_nothing_of_its_own_spills_a_paused_transactions_versions(void);
extern void test_txn_spill_tests_an_owner_spills_nothing_while_a_helper_holds_its_transactions_turn(void);
extern void test_txn_spill_tests_a_spill_of_many_keys_writes_one_file_whose_filter_takes_pages(void);
extern void test_txn_spill_tests_an_abort_leaves_its_spills_in_the_chain_for_a_fold(void);
extern void test_txn_spill_tests_a_one_range_tree_spills_at_a_full_budget(void);
extern void test_txn_spill_tests_a_merge_of_row_spills_merges_the_tuple_ids_their_entries_name(void);
extern void test_txn_spill_tests_a_merge_at_a_full_budget_lists_its_file_in_place_of_its_sources(void);
extern void test_txn_spill_tests_a_reader_across_a_merges_unchain_reads_each_version_once(void);
extern void test_txn_spill_tests_three_writers_of_small_trees_past_a_third_of_the_budget_each_spill_and_finish(void);
extern void test_txn_spill_tests_three_writers_waiting_on_committed_versions_finish_once_a_fold_installs(void);
extern void test_txn_spill_tests_a_spills_tuple_ids_still_find_their_rows_keys(void);
extern void test_txn_spill_tests_a_spilled_value_maps_entries_are_read_below_the_memtable(void);
extern void test_txn_spill_tests_a_scan_merges_spilled_keys_across_chunks_in_order(void);
extern void test_txn_spill_tests_a_spill_takes_every_version_of_its_transaction(void);
extern void test_txn_spill_tests_a_storages_spills_list_hit_and_remove(void);
extern void test_txn_spill_tests_a_pending_spill_is_found_by_its_storages_readers_until_it_settles(void);
extern void test_txn_spill_tests_a_filter_hits_its_keys_and_few_others(void);
extern void test_txn_spill_tests_a_large_filter_takes_several_pages(void);
extern void test_txn_spill_tests_a_residual_whose_listing_fails_lists_nothing_and_gives_its_count_back(void);
extern void test_txn_spill_tests_a_drop_counts_a_crashed_transactions_files_in_either_order(void);
extern void test_txn_spill_tests_a_carry_that_fails_at_any_step_lists_nothing_and_leaves_every_count(void);
extern void test_txn_spill_tests_a_merge_never_moves_a_writers_entry_past_anothers(void);
extern void test_txn_spill_tests_a_committed_writers_merge_never_moves_its_entry_past_a_later_writers(void);
extern void test_txn_spill_tests_a_keys_spilled_versions_read_in_commit_order_whatever_their_listing(void);
extern void test_txn_spill_tests_a_merge_listed_past_another_writers_file_of_its_key_loses_nothing(void);
extern void test_txn_spill_tests_a_map_keys_entries_read_in_creation_order_not_commit_order(void);
extern void test_txn_spill_tests_a_writers_two_spills_of_a_key_read_its_newer_version_first(void);
extern void test_txn_spill_tests_a_rehome_renames_a_replayed_states_manifest_to_its_new_keys_name(void);
extern void test_txn_spill_tests_a_reads_cursor_answers_each_place_as_a_fresh_read_does(void);
extern void test_txn_spill_tests_reads_trim_to_their_bound_in_reading_order(void);
extern void test_txn_spill_tests_a_reads_allocations_are_the_reads_heaps(void);
extern void test_txn_spill_tests_a_panic_inside_the_reads_heap_puts_the_port_heap_back(void);
extern void test_txn_spill_tests_a_filter_check_through_a_fresh_read_opens_nothing(void);
#endif

#endif							/* DOLTGRES_TXN_SPILL_H */
