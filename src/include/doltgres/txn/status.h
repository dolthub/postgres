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
 * The commit status table (txn's status.rs): each transaction's entry, its
 * outcome and its count of versions, in shared memory, and each process's
 * slot of its last commit's registration.
 */
#ifndef DOLTGRES_TXN_STATUS_H
#define DOLTGRES_TXN_STATUS_H

#include "doltgres/host/delta.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/txn/memtable/version.h"

/* TXN_STATUS_FREE is the xid word of a place no entry has taken. */
#define TXN_STATUS_FREE ((uint64) 0)

/* TXN_STATUS_REMOVED is the xid word of a place whose entry was removed. */
#define TXN_STATUS_REMOVED PG_UINT64_MAX

/*
 * TXN_STATUS_FIRST_NORMAL is Postgres' FirstNormalTransactionId: below it,
 * the invalid xid, a free place's word, and the bootstrap and frozen ones,
 * whose versions are committed with no entry.
 */
#define TXN_STATUS_FIRST_NORMAL ((uint64) 3)

/* TxnStatusOutcomeTag is Outcome's variants. */
typedef enum TxnStatusOutcomeTag
{
	/*
	 * Pending in no process, as redo's entries and a prepared transaction's
	 * after a restart are, and as a place is between its claim and the store
	 * of its outcome: a zero word.
	 */
	TXN_STATUS_OUTCOME_UNOWNED,
	/* Committed, at the end of the transaction's commit record. */
	TXN_STATUS_OUTCOME_COMMITTED,

	/*
	 * Pending in the process of a number, whose slot holds the LSN once the
	 * transaction commits.
	 */
	TXN_STATUS_OUTCOME_PENDING_IN,
} TxnStatusOutcomeTag;

/* TxnStatusOutcome is Outcome: what an entry's outcome word says of its xid. */
typedef struct TxnStatusOutcome
{
	TxnStatusOutcomeTag tag;
	union
	{
		HostLsn		committed;
		uint32		pending_in;
	}			u;
} TxnStatusOutcome;

/* TxnStatusEntry is Entry: what a lookup finds of an xid's entry, its outcome and its count. */
typedef struct TxnStatusEntry
{
	TxnStatusOutcome outcome;
	uint64		count;
} TxnStatusEntry;

/*
 * TxnStatusRoomed is Roomed: an entry's slot, which a take of room or a
 * replay's take returns and a version's push counts at with no lookup.
 */
typedef struct TxnStatusRoomed
{
	uint64		f0;
} TxnStatusRoomed;

/* TxnStatusCommitTag is Commit's variants. */
typedef enum TxnStatusCommitTag
{
	/* The xid has no entry. */
	TXN_STATUS_COMMIT_MISSING,

	/*
	 * Not committed by the table's account: pending in a process whose
	 * transaction hasn't committed, or pending in none.
	 */
	TXN_STATUS_COMMIT_PENDING,
	/* Committed, at the end of its commit record. */
	TXN_STATUS_COMMIT_AT,
} TxnStatusCommitTag;

/* TxnStatusCommit is Commit: what the table says of a top-level xid's commit. */
typedef struct TxnStatusCommit
{
	TxnStatusCommitTag tag;
	union
	{
		HostLsn		at;
	}			u;
} TxnStatusCommit;

/* TXN_STATUS_TABLE_SINGLE_OPS is Table's Shape and Single: the table's one record in shared memory. */
extern const HostSharedSingleOps TXN_STATUS_TABLE_SINGLE_OPS;

/* TXN_STATUS_COMMITS_PROC_SLOTS_OPS is Commits' ProcSlots: each process's slot of its last commit's registration. */
extern const HostSharedProcSlotsOps TXN_STATUS_COMMITS_PROC_SLOTS_OPS;

/* TXN_STATUS_COMMIT_XID_INDEX is CommitXid's word: the registered top-level xid, zero for no pair. */
#define TXN_STATUS_COMMIT_XID_INDEX 0

/* TXN_STATUS_COMMIT_END_INDEX is CommitEnd's word: the end of the registered transaction's commit record. */
#define TXN_STATUS_COMMIT_END_INDEX 1

extern bool txn_status_Outcome_word(TxnStatusOutcome self, uint64 *result);
extern TxnStatusOutcome txn_status_Outcome_of(uint64 word);
extern bool txn_status_Outcome_PartialEq_eq(const TxnStatusOutcome *self, const TxnStatusOutcome *other);
extern bool txn_status_Outcome_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_status_Entry_PartialEq_eq(const TxnStatusEntry *self, const TxnStatusEntry *other);
extern bool txn_status_Entry_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_status_Roomed_PartialEq_eq(const TxnStatusRoomed *self, const TxnStatusRoomed *other);
extern bool txn_status_Roomed_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_status_key(HostXid xid, uint64 *result);
extern bool txn_status_lookup(HostIoIo *io, HostXid xid, bool *has_result, TxnStatusEntry *result, HostError *error);
extern bool txn_status_take(HostIoIo *io, HostXid xid, TxnStatusOutcome outcome, TxnStatusRoomed *result_0,
							bool *result_1, HostError *error);
extern bool txn_status_counted(HostIoTakesShort io, const HostDeltaHold *hold, TxnStatusRoomed roomed,
							   uint64 amount, HostError *error);
extern bool txn_status_settled(HostIoIo *io, HostXid xid, TxnStatusOutcome from, TxnStatusOutcome to, bool *result,
							   HostError *error);
extern bool txn_status_remove(HostIoIo *io, HostXid xid, bool *result, HostError *error);
extern bool txn_status_add(HostIoIo *io, HostXid xid, uint64 amount, bool *has_result, uint64 *result,
						   HostError *error);
extern void txn_status_registered(const HostSharedListed *listed, HostXid xid, HostLsn end);
extern void txn_status_prepared_registered(const HostSharedListed *listed, HostLsn end);
extern bool txn_status_roomed(HostIoIo *io, HostXid top, TxnStatusRoomed *result, HostError *error);
extern bool txn_status_prepared_committing(HostIoIo *io, HostXid xid, HostError *error);
extern bool txn_status_kept_roomed(HostIoIo *io, const TxnMemtableVersionWriter *writer, bool *has_result,
								   TxnStatusRoomed *result, HostError *error);
extern bool txn_status_redo_roomed(HostIoIo *io, const TxnMemtableVersionWriter *writer, bool *has_result,
								   TxnStatusRoomed *result, HostError *error);
extern bool txn_status_redo_committed(HostIoIo *io, HostXid xid, HostLsn end, HostError *error);
extern bool txn_status_lowered(HostIoIo *io, HostLsn position, HostLsn *result, HostError *error);
extern bool txn_status_Commit_PartialEq_eq(const TxnStatusCommit *self, const TxnStatusCommit *other);
extern bool txn_status_Commit_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_status_committed(HostIoIo *io, HostXid xid, TxnStatusCommit *result, HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_status_tests_a_prepared_commit_points_its_entry_at_the_committer_and_registers(void);
extern void test_txn_status_tests_a_slot_stays_its_entrys_across_a_growth_and_a_removed_ones_is_taken_again(void);
extern void test_txn_status_tests_a_growth_while_the_proc_array_cant_answer_keeps_an_ended_entry(void);
extern void test_txn_status_tests_a_commit_registered_in_its_slot_reads_back_and_its_move_settles_the_entry(void);
extern void test_txn_status_tests_a_folds_point_lies_below_a_commit_still_in_the_proc_array(void);
extern void test_txn_status_tests_a_take_of_room_moves_the_slots_pair_first(void);
extern void test_txn_status_tests_a_replayed_commit_stores_its_end_in_the_entry_its_replay_took(void);
extern void test_txn_status_tests_churn_with_no_live_entries_keeps_one_segment(void);
extern void test_txn_status_tests_a_growth_holds_what_it_keeps_in_the_least_room(void);
extern void test_txn_status_tests_entries_are_taken_found_counted_removed_and_kept_by_a_growth(void);
extern void test_txn_status_tests_an_outcomes_word_says_the_outcome(void);
extern void test_txn_status_tests_an_outcome_too_large_for_its_word_has_none(void);
extern void test_txn_status_tests_an_xids_word_is_no_empty_places(void);
extern void test_txn_status_tests_a_lookup_probes_every_place_and_neighbours_start_apart(void);
extern void test_txn_status_tests_a_table_is_crowded_at_three_quarters(void);
extern void test_txn_status_tests_a_key_whose_slot_isnt_stored_yet_leaves_its_slot_as_it_was(void);
#endif

#endif							/* DOLTGRES_TXN_STATUS_H */
