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
 * A storage's state (txn's state.rs), the shared record of its key and
 * tablespace, whether it is filled, whether the manifest holds its record,
 * its tree's totals, and the base records it holds, with the backend's holds
 * of the states its transaction found. Part of txn's contract.
 */
#ifndef DOLTGRES_TXN_STATE_H
#define DOLTGRES_TXN_STATE_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/owned.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/free_space.h"

/* RepoRepo is repo's Repo, which state reaches only by pointer; its definition comes with repo's header. */
typedef struct RepoRepo RepoRepo;

/* RepoRead is repo's Read, which gather takes only by pointer. */
typedef struct RepoRead RepoRead;

/* RepoGathered is repo's Gathered, which gather gives only in a vector. */
typedef struct RepoGathered RepoGathered;

/* TXN_STATE_STATE_POOL_OPS is State's Shape and Pool: a storage's state, its locks, atomics, and two base records' blobs. */
extern const HostSharedPoolOps TXN_STATE_STATE_POOL_OPS;

/* TXN_STATE_STATE_LOCK_INDEX is StateLock's lock: the state's lock, under which its blobs are read and written. */
#define TXN_STATE_STATE_LOCK_INDEX 0

/* TXN_STATE_FOLD_DUE_INDEX is FoldDue's atomic: one once a reclaim or a hand-off found the storage's maps linked with no memtable. */
#define TXN_STATE_FOLD_DUE_INDEX 15

/* TXN_STATE_MEMTABLE_WORD_INDEX is MemtableWord's atomic: the storage's memtable's word. */
#define TXN_STATE_MEMTABLE_WORD_INDEX 9

/* TXN_STATE_FREE_WORD_INDEX is FreeWord's atomic: the word of the storage's delta's free space map. */
#define TXN_STATE_FREE_WORD_INDEX 12

/* TXN_STATE_VALUES_WORD_INDEX is ValuesWord's atomic: the word of the storage's delta's value map. */
#define TXN_STATE_VALUES_WORD_INDEX 13

/* TXN_STATE_TUPLES_WORD_INDEX is TuplesWord's atomic: the word of the storage's delta's tuple id table. */
#define TXN_STATE_TUPLES_WORD_INDEX 14

/* TXN_STATE_SPILLS_INDEX is Spills' atomic. */
#define TXN_STATE_SPILLS_INDEX (20 + TXN_FREE_SPACE_CATEGORIES)

/* TXN_STATE_SPILLS_WRITING_INDEX is SpillsWriting's long lock: the writers' lock of the storage's spills' manifest. */
#define TXN_STATE_SPILLS_WRITING_INDEX 1

/* TXN_STATE_SPILLS_PENDING_INDEX is SpillsPending's atomic: the processes whose slot names a spill the manifest doesn't list yet. */
#define TXN_STATE_SPILLS_PENDING_INDEX (21 + TXN_FREE_SPACE_CATEGORIES)

/* TXN_STATE_SPILLS_CHANGED_INDEX is SpillsChanged's atomic: the generation of the storage's spills' manifest. */
#define TXN_STATE_SPILLS_CHANGED_INDEX (22 + TXN_FREE_SPACE_CATEGORIES)

/* TXN_STATE_MAKERS_SINGLE_OPS is Makers' Shape and Single. */
extern const HostSharedSingleOps TXN_STATE_MAKERS_SINGLE_OPS;

/* TXN_STATE_MAKING_INDEX is Making's lock: the makers' lock, under which pooled records are published and reclaimed. */
#define TXN_STATE_MAKING_INDEX 0

/* TxnStatePersistence is Persistence: whether a storage's writes are logged. */
typedef enum TxnStatePersistence
{
	TXN_STATE_PERSISTENCE_PERMANENT = 0,
	TXN_STATE_PERSISTENCE_UNLOGGED = 1,
} TxnStatePersistence;

/* txn_TupleU64StorageU64_t is Hold, (u64, Storage, u64). */
typedef struct txn_TupleU64StorageU64_t
{
	uint64		f0;
	HostStorage f1;
	uint64		f2;
} txn_TupleU64StorageU64_t;

/*
 * TxnStateDue is Due: a storage the fold launcher folds, its storage, its
 * newest base's table and index ids, and its unfolded bytes.
 */
typedef struct TxnStateDue
{
	HostStorage storage;
	uint32		table;
	bool		has_index;
	uint32		index;
	uint64		bytes;
} TxnStateDue;

/* TxnStateHolds is Holds: a backend's handles on the states of the storages its top-level transaction found. */
typedef struct TxnStateHolds
{
	/*
	 * states is a Vec<(u64, u32, Held<State>)>, each state with its storage's
	 * key and tablespace.
	 */
	StdVec		states;
} TxnStateHolds;

/* TXN_STATE_CURRENT_TYPE is [AtomicU64; 2]'s size, Default, and drop, a BackendLocal handle's slot and generation. */
extern const StdDefaultType TXN_STATE_CURRENT_TYPE;

/* TXN_STATE_HOLDS is HOLDS: the backend's record of held states, one for its top-level transaction once it holds any. */
extern HostOwnedTable TXN_STATE_HOLDS;

/* Holds' Table, host's owned Table<Holds>, whose kind's glue comes with state.c's body. */
#define HOST_OWNED_SCOPE extern
#define HOST_OWNED_DECLARE
#define HOST_OWNED_PREFIX txn_state_Holds
#define HOST_OWNED_RS TxnStateHolds
#define HOST_OWNED_R TxnStateHolds
#include "doltgres/host/owned_template.h"

/* TxnStateMade is Made: what a state is made with, its key and its table's and index's object ids. */
typedef struct TxnStateMade
{
	TxnBaseKey	key;
	uint32		table;
	bool		has_index;
	uint32		index;
} TxnStateMade;

/* TxnStatePlace is Place: where a held state is among the backend's holds. */
typedef struct TxnStatePlace
{
	size_t		f0;
} TxnStatePlace;

/* TxnStateMissing is Missing: a storage the backend holds no state of yet, with its persistence. */
typedef struct TxnStateMissing
{
	HostStorage f0;
	TxnStatePersistence f1;
} TxnStateMissing;

/* TxnStateFindTag is Find's variants. */
typedef enum TxnStateFindTag
{
	TXN_STATE_FIND_HELD,
	TXN_STATE_FIND_MISSING,
} TxnStateFindTag;

/* TxnStateFind is Find: a held state's place, or a missing state. */
typedef struct TxnStateFind
{
	TxnStateFindTag tag;
	union
	{
		TxnStatePlace held;
		TxnStateMissing missing;
	}			u;
} TxnStateFind;

/* TxnStateFoundTag is Found's variants. */
typedef enum TxnStateFoundTag
{
	TXN_STATE_FOUND_HELD,
	TXN_STATE_FOUND_MADE,
} TxnStateFoundTag;

/* TxnStateFound is Found: a held state's place, or a missing state with what it is made with. */
typedef struct TxnStateFound
{
	TxnStateFoundTag tag;
	union
	{
		TxnStatePlace held;
		struct
		{
			TxnStateMissing f0;
			TxnStateMade f1;
		}			made;
	}			u;
} TxnStateFound;

/* txn_TupleU64HeldState_t is states' (u64, Held<State>): a published state's location and a handle on it. */
typedef struct txn_TupleU64HeldState_t
{
	uint64		f0;
	HostSharedHeld f1;
} txn_TupleU64HeldState_t;

/* TxnStateSaving is Saving: what a save's gathering stored in the states it gathered, its token and the states' locations. */
typedef struct TxnStateSaving
{
	uint64		token;
	/* locations is a Vec<u64>. */
	StdVec		locations;
} TxnStateSaving;

extern TxnBaseSizes txn_state_totals(const HostSharedHeld *held);
extern bool txn_state_known(const HostSharedHeld *held);
extern void txn_state_totals_unknown(HostSharedRecord record);
extern bool txn_state_recounted(HostIoIo *io, const HostSharedHeld *held,
								bool (*count) (void *captures, HostIoIo *io, bool *some, TxnBaseSizes *result,
											   HostError *error),
								void *count_captures, void (*count_drop) (void *captures), TxnBaseSizes *result,
								HostError *error);
extern bool txn_state_held_totals_unknown(HostIoIo *io, HostError *error);
extern void txn_state_committing_cleared(void);
extern void txn_state_committing(const HostSharedHeld *held, TxnBaseSizes adds, TxnBaseSizes takes);
extern void txn_state_committed(void);
extern void txn_state_grew(HostSharedRecord record, uint64 bytes);
extern void txn_state_shrank(HostSharedRecord record, uint64 bytes);
extern bool txn_state_aged(HostIoIo *io, HostSharedRecord record, HostError *error);
extern std_TupleU64U64_t txn_state_pending(HostSharedRecord record);
extern bool txn_state_reach(HostIoIo *io, const HostSharedHeld *held, const TxnBaseBase *base, uint64 end,
							bool *result, HostError *error);
extern bool txn_state_deferring(HostIoIo *io, const HostSharedHeld *held, uint64 end, bool *result,
								HostError *error);
extern bool txn_state_past(HostIoIo *io, const HostSharedHeld *held, bool *some, uint64 *result, HostError *error);
extern bool txn_state_read_past(HostIoIo *io, const HostSharedHeld *held, uint64 to, bool has_next,
								const TxnBaseBase *next_base, uint64 next_end, bool *result, HostError *error);
extern bool txn_state_held_back(HostIoIo *io, bool *some, txn_TupleU64StorageU64_t *result, HostError *error);
extern bool txn_state_behind(HostIoIo *io, const HostSharedHeld *held, bool *some, TxnBaseBase *result,
							 HostError *error);
extern bool txn_state_founded(HostIoIo *io, const HostSharedHeld *held, uint32 table, bool has_index, uint32 index,
							  HostError *error);
extern bool txn_state_hint(HostSharedRecord record, uint8 category, uint32 *result, HostError *error);
extern bool txn_state_lowered(HostSharedRecord record, uint32 block, uint8 categories_start, uint8 categories_end,
							  HostError *error);
extern bool txn_state_advanced(HostSharedRecord record, uint8 category, uint32 from, uint32 to, bool *result,
							   HostError *error);
extern bool txn_state_described(HostSharedRecord record, HostStorage *result_0, TxnStatePersistence *result_1);
extern bool txn_state_mapped(HostSharedRecord record);
extern void txn_state_kept(const TxnBaseBase *base, StdVec *result);
extern bool txn_state_unkept(const uint8 *blob, size_t blob_len,
							 HostError (*fault) (const char *text, size_t text_len), bool *some, TxnBaseBase *result,
							 HostError *error);
extern bool txn_state_usable(HostIoIo *io, uint32 database, HostError *error);
extern bool txn_state_marked(HostIoIo *io, uint32 database, bool *result, HostError *error);
extern bool txn_state_state(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
							const HostStorage *storage, TxnStatePersistence persistence, bool has_made,
							TxnStateMade *made, HostSharedHeld *result, HostError *error);
extern bool txn_state_rechained(HostIoTakesShort io, const HostSharedHeld *held, uint64 read,
								const TxnBaseBase *base, bool *result, HostError *error);
extern bool txn_state_stored_base(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
								  const HostStorage *storage, bool *some, TxnBaseBase *result, HostError *error);
extern bool txn_state_base_record(HostIoIo *io, const HostSharedHeld *held, bool *some, TxnBaseBase *result,
								  HostError *error);
extern bool txn_state_installed(HostIoTakesShort io, const HostSharedHeld *held, uint64 read,
								const TxnBaseBase *base, bool *result, HostError *error);
extern void txn_state_grown(HostSharedRecord record, const TxnBaseBase *base);
extern bool txn_state_newest(HostIoIo *io, const HostSharedHeld *held, bool *some, TxnBaseBase *result,
							 HostError *error);
extern bool txn_state_readable(HostIoIo *io, const HostSharedHeld *held, const HostSnapshot *snapshot, bool *some,
							   TxnBaseBase *result, HostError *error);
extern bool txn_state_emptied(HostIoIo *io, const HostSharedHeld *held, HostError *error);
extern bool txn_state_redo_emptied(HostIoIo *io, const HostSharedHeld *held, HostError *error);
extern bool txn_state_unlinked(HostIoIo *io, const HostStorage *storage, HostError *error);
extern HostStorage txn_state_marker(uint32 database);
extern bool txn_state_due(HostIoIo *io, uint64 threshold, uint64 timeout, bool asked, StdVec *result,
						  HostError *error);
extern bool txn_state_rekeyed(HostIoIo *io, uint32 database, uint32 from, uint32 to, HostError *error);
extern bool txn_state_rehomed(HostIoIo *io, uint32 database, uint32 from, uint32 to, HostError *error);
extern bool txn_state_ended(HostIoIo *io, HostError *error);
extern bool txn_state_holds(HostIoIo *io, const HostStorage *storage, TxnStatePersistence persistence,
							TxnStateFind *result, HostError *error);
extern bool txn_state_held_to_end(HostIoIo *io, const HostStorage *storage, bool *some, TxnStatePlace *result,
								  HostError *error);
extern uint64 txn_state_counter(HostSharedRecord record);
extern uint32 txn_state_space(HostSharedRecord record);
extern uint64 txn_state_generation(HostSharedRecord record);
extern void txn_state_moved(HostSharedRecord record, uint64 from, uint64 to);

extern bool txn_state_spills_generation(uint64 *result, HostError *error);
extern bool txn_state_skipped(HostIoIo *io, uint32 database, HostError *error);
extern bool txn_state_found(HostIoIo *io, const HostStorage *storage, bool *some, HostSharedHeld *result,
							HostError *error);
extern bool txn_state_reset(HostIoIo *io, const RepoRepo *repo, const HostStorage *storage, TxnBaseBase *saved,
							HostError *error);
extern bool txn_state_states(HostIoIo *io, uint32 database, StdVec *result, HostError *error);
extern void txn_state_states_free(StdVec *states);
extern bool txn_state_repositories(HostIoIo *io, StdVec *result, HostError *error);
extern uint32 txn_state_own(HostSharedRecord record);
extern bool txn_state_unlogged(HostIoIo *io, StdVec *result, HostError *error);
extern bool txn_state_gather(HostIoIo *io, uint32 database, const RepoRead *read, StdVec *result_0,
							 TxnStateSaving *result_1, HostError *error);
extern bool txn_state_Saving_saved(TxnStateSaving *self, HostIoIo *io, HostError *error);
extern bool txn_state_held_call(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
								TxnStateFound *found,
								bool (*call) (void *captures, HostIoIo *io, const RepoRepo *repo,
											  const HostSharedHeld *held, HostError *error),
								void *call_captures, void (*call_drop) (void *captures), HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_state_tests_a_reclaim_keeps_a_state_whose_manifest_lists_a_spill(void);
extern void test_txn_state_tests_a_snapshot_reads_the_newest_base_past_its_max_folded(void);
extern void test_txn_state_tests_an_install_keeps_a_key_recorded_after_its_fold_read_the_base(void);
extern void test_txn_state_tests_an_install_replaces_the_newest_and_refuses_a_moved_generation(void);
extern void test_txn_state_tests_a_backend_records_its_key_in_a_state_and_redo_takes_none(void);
extern void test_txn_state_tests_a_linked_free_space_map_or_tuple_id_table_keeps_the_counter_from_falling_back(void);
extern void test_txn_state_tests_a_save_gathers_a_linked_states_counter_and_lets_an_idle_ones_fall_back(void);
extern void test_txn_state_tests_a_backend_holds_its_transactions_states_until_its_end(void);
extern void test_txn_state_tests_a_state_saved_reclaimed_and_made_again_reads_the_same(void);
extern void test_txn_state_tests_the_free_at_the_unlink_takes_a_held_state(void);
extern void test_txn_state_tests_a_write_between_a_gathering_and_its_saved_leaves_the_state_unsaved(void);
extern void test_txn_state_tests_a_state_opened_while_a_reclaim_looks_at_it_is_kept(void);
extern void test_txn_state_tests_the_pool_holds_a_state_for_each_relation_lock_and_reclaims_idle_ones(void);
extern void test_txn_state_tests_a_maker_terminated_before_its_fill_leaves_the_fill_to_the_next_finder(void);
extern void test_txn_state_tests_of_two_racing_fillers_the_second_to_the_lock_drops_its_read(void);
extern void test_txn_state_tests_a_state_is_filled_from_its_base_and_found_again(void);
extern void test_txn_state_tests_the_largest_base_record_fits_a_states_blob_and_reads_back(void);
extern void test_txn_state_tests_two_backends_that_open_one_storage_make_one_state(void);
#endif

#endif							/* DOLTGRES_TXN_STATE_H */
