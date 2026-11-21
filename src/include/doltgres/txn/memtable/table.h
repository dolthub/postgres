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
 * A storage's memtables (txn's memtable/table.rs): the trees whose versions
 * a memtable holds and a prepared push. Part of txn's contract.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_TABLE_H
#define DOLTGRES_TXN_MEMTABLE_TABLE_H

#include "doltgres/host/delta.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/vec.h"
#include "doltgres/host/delta/list.h"
#include "doltgres/std/tuple.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/split.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/status.h"

/*
 * TxnMemtableTableOwnerOps is the Owner trait's steps that differ by owner:
 * grew and shrank count the bytes a finish pushed into one of the owner's
 * trees and a removal took out; its other steps are a tree's own, so a
 * prepare's aim names its tree for them.
 */
typedef struct TxnMemtableTableOwnerOps
{
	void		(*grew) (HostSharedRecord owner, uint64 bytes);
	void		(*shrank) (HostSharedRecord owner, uint64 bytes);
} TxnMemtableTableOwnerOps;

/*
 * TxnMemtableTableTreeOps is the Tree trait: a kind of memtable, its owner,
 * the lock its owner's word is stored under, the word, the word of each
 * process's slot that pins it, whether its versions count at their writer's
 * entry, and what a hand-off that finds it with no memtable does to its
 * owner, nothing when NULL.
 */
typedef struct TxnMemtableTableTreeOps
{
	const TxnMemtableTableOwnerOps *of;
	size_t		lock;
	size_t		word;
	size_t		pin;
	bool		counted;
	void		(*bare) (HostSharedRecord owner);
} TxnMemtableTableTreeOps;

/* TxnMemtableTableAt is At: where the hand-off found a key, its memtable's location and its range's. */
typedef struct TxnMemtableTableAt
{
	uint64		memtable;
	uint64		range;
} TxnMemtableTableAt;

/*
 * TxnMemtableTableReservation is tuples.rs's Reservation: a slot of a tuple
 * id table a prepare reserved, with the place of the key's bytes it
 * appended; here, since tuples.h includes this header and a prepare holds
 * one.
 */
typedef struct TxnMemtableTableReservation
{
	uint64		place;
} TxnMemtableTableReservation;

/*
 * TxnMemtableTableReserve is Reserve: what a prepared push takes before its
 * window, so that its finish allocates nothing: a fresh page that holds its
 * version and a full-height node on any range's page, the most spares any
 * list's push wants, a buffer for its version's record and one for a take's
 * value, what its finish saw of the range it pushed to, and the bytes of the
 * versions pushed with it that within hasn't yet counted at the owner.
 */
typedef struct TxnMemtableTableReserve
{
	bool		has_fresh;
	HostDeltaOwned fresh;
	/* spare is a Vec<Owned>. */
	StdVec		spare;
	/* buffer is a Vec<u8>. */
	StdVec		buffer;
	/* value is a Vec<u8>, the buffer a take's step writes its value into. */
	StdVec		value;
	bool		has_seen;
	TxnMemtableSplitSeen seen;
	uint64		grown;

	/*
	 * counting is whether a push by the step within runs counts at its
	 * writer's entry.
	 */
	bool		counting;
	/* counted is whether the step pushed a version it counted. */
	bool		counted;
} TxnMemtableTableReserve;

/* TxnMemtableTableTarget is Target: one push a prepare readied, its tree, where its hand-off found its key, and its reserve. */
typedef struct TxnMemtableTableTarget
{
	const TxnMemtableTableTreeOps *tree;
	TxnMemtableTableAt at;
	TxnMemtableTableReserve reserve;
} TxnMemtableTableTarget;

/*
 * TxnMemtableTableAim is Aim: a push a prepare readies, the record whose
 * word names its tree, its tree, its key, the longest record its version
 * takes, and the longest value a take writes.
 */
typedef struct TxnMemtableTableAim
{
	HostSharedRecord owner;
	const TxnMemtableTableTreeOps *tree;
	StdBytes	key;
	size_t		need;
	size_t		most;
} TxnMemtableTableAim;

/* TxnMemtableTableRetiring is an iterator over the names of pages a retirement gives back. */
typedef struct TxnMemtableTableRetiring
{
	const uint64 *names;
	size_t		len;
	size_t		at;
} TxnMemtableTableRetiring;

/*
 * TxnMemtableTablePreparedState is Prepared, at either owner: a prepare's
 * hold, kept until its settle, so every open of a finish is under a hold
 * begun before the window, its aims, its targets in the aims' order, and the
 * slot of the tuple id table it reserved.
 */
typedef struct TxnMemtableTablePreparedState
{
	HostDeltaHold hold;
	const TxnMemtableTableAim *aims;
	size_t		aims_len;

	/*
	 * first is the place among the caller's aims of aims' first: a prepare
	 * readies the caller's aims in groups an empty budget can give, aims and
	 * targets being the group's (bugs row 338).
	 */
	size_t		first;
	/* targets is a Vec<Target>. */
	StdVec		targets;
	bool		has_tuple;
	TxnMemtableTableReservation tuple;
} TxnMemtableTablePreparedState;

/* TxnMemtableTableCompactable is Compactable: a page a range names alone that the remover found worth compacting, its index in the range's list, its name, and its copy's size. */
typedef struct TxnMemtableTableCompactable
{
	uint64		entry;
	uint64		name;
	HostDeltaPageSize size;
} TxnMemtableTableCompactable;

/* TxnMemtableTableRemoving is one key removed_all takes versions out of, with the handles of the versions. */
typedef struct TxnMemtableTableRemoving
{
	StdBytes	key;
	const TxnMemtableVersionHandle *handles;
	size_t		handles_len;
} TxnMemtableTableRemoving;

/* txn_TupleHandleU64Bool_t is a version's handle with its writer's top-level xid and whether it is killed, (Handle, u64, bool), as copied hands them out. */
typedef struct txn_TupleHandleU64Bool_t
{
	TxnMemtableVersionHandle f0;
	uint64		f1;
	bool		f2;
} txn_TupleHandleU64Bool_t;

/* TXN_MEMTABLE_TABLE_STATE_OWNER_OPS is State's Owner: a storage's state. */
extern const TxnMemtableTableOwnerOps TXN_MEMTABLE_TABLE_STATE_OWNER_OPS;


/* TXN_MEMTABLE_TABLE_ROWS_TREE_OPS is Rows' Tree: a storage's rows, of its State. */
extern const TxnMemtableTableTreeOps TXN_MEMTABLE_TABLE_ROWS_TREE_OPS;

/* TxnMemtableTableDirection is Direction: the way a walk of a tree goes in key order. */
typedef enum TxnMemtableTableDirection
{
	TXN_MEMTABLE_TABLE_DIRECTION_FORWARD,
	TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD,
} TxnMemtableTableDirection;

/* txn_TupleVecU8VecVersion_t is Versions: a key and its versions, newest first, lent for a hold. */
typedef struct txn_TupleVecU8VecVersion_t
{
	/* f0 is the key, a Vec<u8>. */
	StdVec		f0;
	/* f1 is the versions, a Vec<Version>. */
	StdVec		f1;
} txn_TupleVecU8VecVersion_t;

/* txn_TupleRefSliceU8Usize_t is prepare's (&[u8], usize): a row's key and the longest record its version takes. */
typedef struct txn_TupleRefSliceU8Usize_t
{
	StdBytes	f0;
	size_t		f1;
} txn_TupleRefSliceU8Usize_t;

extern const HostDeltaHold *txn_memtable_table_PreparedState_hold(const TxnMemtableTablePreparedState *self);
extern bool txn_memtable_table_finish(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared,
									  const TxnMemtableVersionChange *change, bool has_roomed,
									  TxnStatusRoomed roomed, TxnMemtableVersionHandle *result, HostError *error);
extern bool txn_memtable_table_finish_speculative(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared,
												  const TxnMemtableVersionChange *change, uint32 token,
												  bool has_roomed, TxnStatusRoomed roomed,
												  TxnMemtableVersionHandle *result, HostError *error);
extern bool txn_memtable_table_finish_showing(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t row,
											  const TxnMemtableVersionChange *change, TxnMemtableVersionHandle ended,
											  uint32 combo, bool has_roomed, TxnStatusRoomed roomed,
											  TxnMemtableVersionHandle *result, HostError *error);
extern bool txn_memtable_table_finish_row(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t row,
										  const TxnMemtableVersionChange *change, bool has_roomed,
										  TxnStatusRoomed roomed, TxnMemtableVersionHandle *result,
										  HostError *error);
extern bool txn_memtable_table_newest(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t row,
									  bool *some, TxnMemtableVersionHandle *result, HostError *error);
extern bool txn_memtable_table_holds(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len,
									 uint64 lsn, bool *result, HostError *error);
extern bool txn_memtable_table_push(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len,
									const TxnMemtableVersionChange *change, TxnMemtableVersionHandle *result,
									HostError *error);
extern bool txn_memtable_table_alone_State(HostIoIo *io, HostSharedRecord owner, bool has_which,
										   TxnMemtableMapMap which, const uint8 *key, size_t key_len,
										   const TxnMemtableVersionChange *change, TxnMemtableVersionHandle *result,
										   HostError *error);
extern bool txn_memtable_table_removed(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									   const uint8 *key, size_t key_len, const TxnMemtableVersionHandle *handles,
									   size_t handles_len, StdVec *result, HostError *error);
extern bool txn_memtable_table_pruned(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									  const uint8 *key, size_t key_len, bool has_aborting, uint64 aborting,
									  HostError *error);
extern bool txn_memtable_table_waited(HostIoIo *io, HostDeltaFull full, bool spilling, HostError *error);
extern bool txn_memtable_table_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state,
										const uint8 *key, size_t key_len, StdVec *result, HostError *error);
extern bool txn_memtable_table_walked(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									  bool (*each) (void *captures, HostIoIo *io, const uint8 *key, size_t key_len,
													const StdVec *values, size_t values_len, HostError *error),
									  void *each_captures, void (*each_drop) (void *captures), HostError *error);
extern bool txn_memtable_table_walked_killed(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
											 bool (*each) (void *captures, HostIoIo *io, const uint8 *key,
														   size_t key_len, const StdVec *values, size_t values_len,
														   HostError *error),
											 void *each_captures, void (*each_drop) (void *captures),
											 HostError *error);
extern bool txn_memtable_table_next(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, bool has_after,
									const uint8 *after, size_t after_len, TxnMemtableTableDirection way, bool *some,
									txn_TupleVecU8VecVersion_t *result, HostError *error);
extern bool txn_memtable_table_range_starts(HostIoIo *io, HostSharedRecord state, StdVec *result, HostError *error);
extern bool txn_memtable_table_first_key(const HostDeltaHold *hold, uint64 range, HostDeltaLent *result,
										 HostError *error);
extern bool txn_memtable_table_search(const HostDeltaListListView *view, const HostDeltaHold *hold, const uint8 *key,
									  size_t key_len, std_TupleU64U64_t *result, HostError *error);
extern bool txn_memtable_table_found(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
									 HostSharedRecord owner, const uint8 *key, size_t key_len,
									 const HostSharedMine *pin, bool *some, TxnMemtableTableAt *result,
									 HostError *error);
extern bool txn_memtable_table_handed(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
									  const HostSharedMine *mine, HostSharedRecord owner, const uint8 *key,
									  size_t key_len, bool *some, TxnMemtableTableAt *result, HostError *error);
extern bool txn_memtable_table_Reserve_push(TxnMemtableTableReserve *self, TxnMemtableIndexKeyedOpen *open,
											const HostDeltaHold *hold, const uint8 *key, size_t key_len,
											const TxnMemtableVersionChange *change,
											TxnMemtableVersionHandle *result, HostError *error);
extern size_t txn_memtable_table_Prepared_target(const TxnMemtableTablePreparedState *self, size_t take);
extern bool txn_memtable_table_Prepared_aimed(const TxnMemtableTablePreparedState *self, size_t target,
											  HostSharedRecord *result_0, const TxnMemtableTableTreeOps **result_1,
											  HostError *error);
extern bool txn_memtable_table_Prepared_tuple(TxnMemtableTablePreparedState *self, const HostDeltaHold **result_0,
											  HostSharedRecord *result_1, TxnMemtableTableReservation *result_2,
											  HostError *error);

/*
 * TXN_MEMTABLE_TABLE_WHOLE is the most aims a prepare readies as one group at
 * every delta_memory, so a caller of a few aims never meets a group (bugs row
 * 338).
 */
#define TXN_MEMTABLE_TABLE_WHOLE 8

/*
 * TxnMemtableTableRefusal is a carry's prepare's answer of the delta
 * budget's refusal: whether the budget refused it, having taken nothing, and
 * the refusal, which the carry waits on.
 */
typedef struct TxnMemtableTableRefusal
{
	bool		refused;
	HostDeltaFull full;
} TxnMemtableTableRefusal;

extern bool txn_memtable_table_prepared(HostIoIo *io, const TxnMemtableTableAim *aims, size_t aims_len,
										bool has_tuple, TxnMemtableTableReservation tuple,
										bool (*inside) (void *captures, HostIoIo *io,
														TxnMemtableTablePreparedState *prepared, HostError *error),
										void *inside_captures, void (*inside_drop) (void *captures),
										bool *has_unstored, TxnMemtableTableReservation *unstored,
										TxnMemtableTableRefusal *refused, HostError *error);
extern bool txn_memtable_table_refused(HostIoIo *io, HostDeltaFull full, TxnMemtableTableRefusal *refused,
									   bool *result, HostError *error);
extern bool txn_memtable_table_within(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t index,
									  bool has_roomed, TxnStatusRoomed roomed,
									  bool (*step) (void *captures, TxnMemtableIndexKeyedOpen *open,
													const HostDeltaHold *hold, const uint8 *key, size_t key_len,
													TxnMemtableTableReserve *reserve, HostError *error),
									  void *captures, HostError *error);
extern bool txn_memtable_table_settled(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									   TxnMemtableTableAt at, bool has_seen, const TxnMemtableSplitSeen *seen,
									   HostError *error);
#ifdef DOLTGRES_TEST_TXN
extern void test_txn_memtable_table_tests_a_refused_try_leaves_no_memtable_range_or_page_behind(void);
extern void test_txn_memtable_table_tests_a_refused_try_of_two_trees_leaves_neither_memtable_behind(void);
extern bool txn_memtable_table_first(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									 HostError *error);
#endif
extern bool txn_memtable_table_memtable(const TxnMemtableTableTreeOps *tree, HostSharedRecord owner, uint64 *result);
extern bool txn_memtable_table_names(const TxnMemtableTableTreeOps *tree, HostSharedRecord owner, uint64 location);
extern bool txn_memtable_table_abandoned(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
										 uint64 location, bool *result, HostError *error);
extern bool txn_memtable_table_retire_all(HostIoIo *io, HostSharedRecord state, HostError *error);

extern bool txn_memtable_table_readied(HostIoIo *io, bool has_rows, HostSharedRecord state,
									   const txn_TupleRefSliceU8Usize_t *rows, size_t rows_len,
									   const TxnMemtableMapTake *takes, size_t takes_len,
									   bool tuple, bool (*inside) (void *captures, HostIoIo *io,
																   TxnMemtableTablePreparedState *prepared,
																   HostError *error), void *inside_captures,
									   void (*inside_drop) (void *captures), TxnMemtableTableRefusal *refused,
									   HostError *error);
extern bool txn_memtable_table_pushed(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t index,
									  const TxnMemtableVersionChange *change, bool has_roomed,
									  TxnStatusRoomed roomed, TxnMemtableVersionHandle *result, HostError *error);
extern bool txn_memtable_table_finished(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t index,
										const TxnMemtableVersionChange *change, bool has_roomed, TxnStatusRoomed roomed,
										TxnMemtableVersionHandle *result,
										HostError *error);
extern bool txn_memtable_table_lent(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
									HostSharedRecord owner, const uint8 *key, size_t key_len,
									bool (*reading) (void *captures,
													 const HostDeltaHold *hold, TxnMemtableTableAt at,
													 const HostSharedGuard *guard, HostError *error),
									void *captures, bool *some, HostError *error);
extern bool txn_memtable_table_read(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									const uint8 *key, size_t key_len, bool (*reading) (void *captures,
																					   const HostDeltaHold *hold,
																					   TxnMemtableTableAt at,
																					   const HostSharedGuard *guard,
																					   HostError *error),
									void *captures, bool *some,
									HostError *error);
extern bool txn_memtable_table_held(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									const uint8 *key, size_t key_len, uint64 lsn, bool *result, HostError *error);
extern bool txn_memtable_table_alone(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									 const uint8 *key, size_t key_len, const TxnMemtableVersionChange *change,
									 bool has_roomed, TxnStatusRoomed roomed, TxnMemtableVersionHandle *result,
									 HostError *error);
extern bool txn_memtable_table_removed_all(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
										   const TxnMemtableTableRemoving *keys, size_t keys_len, StdVec *result,
										   HostError *error);
extern bool txn_memtable_table_chain(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
									 HostSharedRecord owner, const uint8 *key, size_t key_len, StdVec *result,
									 HostError *error);
extern bool txn_memtable_table_copied(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									  bool killed, bool (*each) (void *captures, HostIoIo *io, const uint8 *key,
																 size_t key_len, const StdVec *records,
																 size_t records_len,
																 const txn_TupleHandleU64Bool_t *handles,
																 size_t handles_len, HostError *error),
									  void *captures,
									  HostError *error);
extern bool txn_memtable_table_stepped(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
									   HostSharedRecord owner, bool has_place, const uint8 *place, size_t place_len,
									   TxnMemtableTableDirection way,
									   bool (*reading) (void *captures, const HostDeltaHold *hold,
														const HostSharedGuard *guard, bool has_stop,
														const uint8 *stop, size_t stop_len, HostError *error),
									   void *captures, bool *some, bool *has_stop,
									   StdVec *stop, HostError *error);
extern bool txn_memtable_table_unlist(const TxnMemtableTableTreeOps *tree, TxnMemtableIndexOpen open,
									  const HostDeltaHold *hold, uint64 entry, uint64 memtable, bool *result,
									  HostError *error);
extern bool txn_memtable_table_retired(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									   HostError *error);

/* TXN_MEMTABLE_TABLE_RETIRING_ITERATOR_OPS is the retiring names' Iterator, whose drop does nothing. */
extern const StdIteratorOps TXN_MEMTABLE_TABLE_RETIRING_ITERATOR_OPS;

extern bool txn_memtable_table_filled(const HostDeltaMade *made, const HostSharedShapeOps *shape, size_t list,
									  uint64 entry, HostError *error);
extern bool txn_memtable_table_unlisted(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
										HostSharedRecord owner, TxnMemtableTableAt at, HostError *error);
extern bool txn_memtable_table_closed_names(StdVec *names, const HostDeltaListClosed *closed,
											const HostDeltaHold *hold, HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_memtable_table_tests_ranges_never_outnumber_pages_through_splits_and_removals(void);
extern void test_txn_memtable_table_tests_removals_look_at_the_pages_they_touch_and_at_every_page_once_per_page_count(void);
extern void test_txn_memtable_table_tests_a_removal_that_retires_no_page_runs_no_free_pass(void);
extern void test_txn_memtable_table_tests_a_ranges_pages_double_as_it_fills(void);
extern void test_txn_memtable_table_tests_a_removal_counts_its_records_and_its_keys_bytes_dead(void);
extern void test_txn_memtable_table_tests_a_prepare_pinned_while_the_remover_empties_the_first_range_pushes_once_either_way(void);
extern void test_txn_memtable_table_tests_hold_ends_that_kept_no_retirement_wake_no_writer(void);
extern void test_txn_memtable_table_tests_removals_at_the_budget_free_memory_for_a_waiting_writer(void);
extern void test_txn_memtable_table_tests_a_readers_hold_ending_after_a_removal_frees_memory_for_a_waiting_writer(void);
extern void test_txn_memtable_table_tests_a_hold_ending_before_a_refused_writer_counts_itself_leaves_it_the_pass(void);
extern void test_txn_memtable_table_tests_a_writer_holding_readers_waits_without_pinning_another_processs_frees(void);
extern void test_txn_memtable_table_tests_a_prepared_transactions_versions_stay_until_its_rollback_then_go_at_the_next_prune(void);
extern void test_txn_memtable_table_tests_a_subtransactions_write_rolled_back_while_its_top_commits_is_taken_back_and_the_tops_kept(void);
extern void test_txn_memtable_table_tests_a_transaction_past_the_lists_cap_leaves_its_unlisted_versions_to_the_next_prune(void);
extern void test_txn_memtable_table_tests_a_compaction_whose_page_is_dropped_before_its_second_lock_gives_its_copy_back(void);
extern void test_txn_memtable_table_tests_a_compaction_at_the_budget_takes_its_copies_past_it_in_one_block(void);
extern void test_txn_memtable_table_tests_a_page_over_half_dead_is_compacted_and_every_live_handle_reads_the_same(void);
extern void test_txn_memtable_table_tests_a_ranges_first_page_is_never_compacted(void);
extern void test_txn_memtable_table_tests_a_sweep_drops_a_split_ranges_sealed_empty_page_from_both_halves(void);
extern void test_txn_memtable_table_tests_a_split_halfs_removals_keep_it_open_until_its_last_key_and_leave_the_shared_pages_to_the_other(void);
extern void test_txn_memtable_table_tests_a_split_after_the_remover_dropped_pages_counts_only_the_pages_its_list_names(void);
extern void test_txn_memtable_table_tests_a_walk_reads_once_a_key_written_where_the_next_range_was_retired(void);
extern void test_txn_memtable_table_tests_a_backward_scan_lends_each_key_once_across_a_retired_range_before_it(void);
extern void test_txn_memtable_table_tests_an_abort_takes_back_the_versions_of_the_keys_its_transaction_wrote_to(void);
extern void test_txn_memtable_table_tests_a_prune_takes_out_the_versions_of_aborted_writers_only(void);
extern void test_txn_memtable_table_tests_a_removal_takes_versions_out_of_a_chain_and_the_key_out_with_its_last(void);
extern void test_txn_memtable_table_tests_a_removal_closes_an_emptied_range_and_an_emptied_memtable(void);
extern void test_txn_memtable_table_tests_a_backward_scan_reads_the_upper_half_of_a_range_that_split_under_it(void);
extern void test_txn_memtable_table_tests_a_walk_held_between_two_ranges_while_the_second_splits_hands_out_each_key_once(void);
extern void test_txn_memtable_table_tests_a_scan_lends_each_key_with_a_version_once_either_way(void);
extern void test_txn_memtable_table_tests_a_scan_goes_on_across_a_split_between_its_calls(void);
extern void test_txn_memtable_table_tests_a_keys_versions_are_lent_newest_first_for_the_hold(void);
extern void test_txn_memtable_table_tests_a_reclaim_keeps_a_state_whose_map_alone_is_linked(void);
extern void test_txn_memtable_table_tests_a_rows_version_is_held_by_its_records_lsn(void);
extern void test_txn_memtable_table_tests_a_walk_hands_out_each_key_once_across_a_split_and_its_halves(void);
extern void test_txn_memtable_table_tests_a_reclaim_keeps_a_state_whose_maps_are_linked_and_marks_it_due(void);
extern void test_txn_memtable_table_tests_a_rows_hand_off_that_meets_linked_maps_marks_a_fold_due(void);
extern void test_txn_memtable_table_tests_a_finish_longer_than_its_prepare_is_refused(void);
extern void test_txn_memtable_table_tests_a_memtable_whose_empty_key_holds_a_version_stays_open(void);
extern void test_txn_memtable_table_tests_a_dropper_held_after_its_clear_and_a_closer_close_the_memtable_once(void);
extern void test_txn_memtable_table_tests_an_unpinners_refused_close_leaves_the_memtable_to_the_reclaim(void);
extern void test_txn_memtable_table_tests_a_closer_that_reads_a_prepares_pin_leaves_its_memtable_open(void);
extern void test_txn_memtable_table_tests_a_prepare_that_pins_after_a_closers_read_goes_around(void);
extern void test_txn_memtable_table_tests_a_backend_that_exits_inside_its_window_leaves_the_close_to_the_reclaim(void);
extern void test_txn_memtable_table_tests_a_first_insert_makes_its_memtables_before_its_hold(void);
extern void test_txn_memtable_table_tests_a_memtable_no_finish_reached_is_closed_by_its_last_unpinner(void);
extern void test_txn_memtable_table_tests_a_finish_after_two_splits_follows_the_chain_to_an_open_range(void);
extern void test_txn_memtable_table_tests_a_finish_after_a_split_pushes_into_its_half_with_nothing_taken(void);
extern void test_txn_memtable_table_tests_a_rows_version_reads_back_its_records_lsn(void);
extern void test_txn_memtable_table_tests_a_split_that_fails_before_its_kept_leaves_its_entry_kept(void);
extern void test_txn_memtable_table_tests_a_retirement_that_cannot_open_a_page_leaves_the_range_for_the_next_finder(void);
extern void test_txn_memtable_table_tests_a_split_whose_half_fails_to_build_leaves_the_range_open(void);
extern void test_txn_memtable_table_tests_a_writer_held_across_a_split_writes_into_its_half(void);
extern void test_txn_memtable_table_tests_a_split_short_at_every_take_gives_up_and_leaves_its_entry_for_the_next(void);
extern void test_txn_memtable_table_tests_a_range_past_split_splits_at_its_middle_key(void);
extern void test_txn_memtable_table_tests_two_finders_of_a_split_range_put_its_halves_in_once(void);
extern void test_txn_memtable_table_tests_two_finders_of_a_closed_range_take_out_only_that_range(void);
extern void test_txn_memtable_table_tests_two_writers_that_find_a_closed_memtable_clear_it_once(void);
extern void test_txn_memtable_table_tests_a_memtable_emptied_and_never_written_again_leaves_its_state_reclaimable(void);
extern void test_txn_memtable_table_tests_a_closer_terminated_before_the_clear_leaves_it_to_the_next_writer(void);
extern void test_txn_memtable_table_tests_a_reclaim_keeps_a_state_with_a_memtable(void);
extern void test_txn_memtable_table_tests_an_unlinked_storage_retires_its_memtable(void);
extern void test_txn_memtable_table_tests_a_long_keys_version_goes_on_a_page_that_holds_it(void);
extern void test_txn_memtable_table_tests_a_ranges_search_finds_the_range_for_every_key(void);
extern void test_txn_memtable_table_tests_a_closed_range_found_is_taken_out_and_the_writer_starts_over(void);
extern void test_txn_memtable_table_tests_a_writer_that_reaches_a_retired_range_writes_to_the_next_memtable(void);
extern void test_txn_memtable_table_tests_a_first_writer_makes_a_memtable_whose_first_key_is_empty(void);
extern void test_txn_memtable_table_tests_two_first_writers_make_one_memtable(void);
extern void test_txn_memtable_table_tests_a_split_whose_memtable_was_retired_before_its_hold_ends_without_opening_it(void);
extern bool txn_memtable_table_tests_usizes_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_table_tests_keys_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_table_tests_keys_eq(const StdVec *a, const StdVec *b);
extern void txn_memtable_table_tests_keys_free(StdVec *keys);
extern void txn_memtable_table_tests_keys_of(const char *const *keys, size_t keys_len, StdVec *result);
extern bool txn_memtable_table_tests_collecting(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values, size_t values_len, HostError *error);
extern void test_txn_memtable_table_tests_a_prepare_with_no_tuple_slot_leaves_its_unstored_slot_as_it_was(void);
#endif

#endif							/* DOLTGRES_TXN_MEMTABLE_TABLE_H */
