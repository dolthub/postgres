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
 * A row's write as a storage's newest (txn's insert.rs): an insert, an
 * update's new row, a speculative insert's completion, and a restart's move
 * of the tuple id counter. An update's stop value, Rust's T, is the caller's,
 * as change.h's is.
 */
#ifndef DOLTGRES_TXN_INSERT_H
#define DOLTGRES_TXN_INSERT_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/tuple.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/change.h"
#include "doltgres/txn/memtable.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/status.h"
#include "doltgres/txn/values.h"
#include "doltgres/txn/visible.h"

typedef struct TxnVacuumPrune TxnVacuumPrune;

/*
 * TxnInsertRoom is Room: what a row needs of a page, its heap bytes, the
 * bytes its table's fill factor keeps free, a page's bytes past its header,
 * and a page's most tuples.
 */
typedef struct TxnInsertRoom
{
	uint64		bytes;
	uint64		reserve;
	uint64		page;
	uint16		most;
} TxnInsertRoom;

/*
 * TxnInsertPage is Page: what redo knows of a heap page with no catalog, its
 * bytes past its header and its most tuples.
 */
typedef struct TxnInsertPage
{
	uint64		bytes;
	uint16		most;
} TxnInsertPage;

/*
 * TxnInsertInsert is Insert: a row an insert writes, its storage and table,
 * its key, its put, whether the table is keyless, whether its key is a
 * deferrable primary key's, its writer, whether it is written frozen,
 * whether its storage is logged, and whether logical decoding reads it.
 */
typedef struct TxnInsertInsert
{
	HostStorage storage;
	uint32		table;
	/* key is a Vec<u8>. */
	StdVec		key;
	TxnMemtablePut put;
	bool		keyless;
	bool		deferred;
	TxnMemtableVersionWriter writer;

	/*
	 * roomed is the slot of the writer's top-level entry in the commit status
	 * table, none for a frozen insert, whose writer has no entry.
	 */
	bool		has_roomed;
	TxnStatusRoomed roomed;
	bool		frozen;
	bool		logged;
	/* decoded is whether logical decoding reads its relation. */
	bool		decoded;
	/* toast is the TOAST relation its out-of-line values are mapped in. */
	bool		has_toast;
	TxnRecordToast toast;
	/* kept is a Vec<usize>: the put's values an update kept, ascending. */
	StdVec		kept;
	/* token is a speculative insert's token, or none for any other write. */
	bool		has_token;
	uint32		token;
	/* target is the block the backend's last insert into the relation took. */
	bool		has_target;
	uint32		target;
} TxnInsertInsert;

/*
 * TxnInsertReplaces is Replaces: the row an update replaces, its key, its
 * tuple id, the row lock its end and the new row hold, and its key's stripe.
 */
typedef struct TxnInsertReplaces
{
	/* key is a Vec<u8>. */
	StdVec		key;
	HostRow		tuple;
	TxnMemtableLockMode lock;
	uint32		stripe;
} TxnInsertReplaces;

/* TxnInsertEqualOps is Equal: whether two keys of a table are equal by their key's classes. */
typedef struct TxnInsertEqualOps
{
	bool		(*equal) (void *self, HostIoIo *io, const uint8 *a, size_t a_len, const uint8 *b, size_t b_len,
						  bool *result, HostError *error);
} TxnInsertEqualOps;

/* TxnInsertEqual is a dyn Equal: its table and its value. */
typedef struct TxnInsertEqual
{
	const TxnInsertEqualOps *ops;
	void	   *self;
} TxnInsertEqual;

/*
 * TxnInsertUniqueness is Uniqueness: what an immediate key's resolve needs,
 * the key's order, the primary key index's object id, and for a key whose
 * class compares by a function, that class's equality.
 */
typedef struct TxnInsertUniqueness
{
	const TreesKeyOrder *order;
	uint32		index;
	bool		has_equal;
	TxnInsertEqual equal;
} TxnInsertUniqueness;

/*
 * TxnInsertKeyed is Keyed: how an insert takes its key's stripe of the key
 * lock, the key's stripe, and for an immediate key its resolve's needs.
 */
typedef struct TxnInsertKeyed
{
	uint32		stripe;
	bool		has_unique;
	TxnInsertUniqueness unique;
} TxnInsertKeyed;

/*
 * TxnInsertUnique is Unique: an immediate key's read of the base before its
 * stripe, the base's row's value at the key's own bytes, the base generation
 * it read it at, the other keys equal to it, each with the base's row's
 * value at it, and the stripe's push count read first.
 */
typedef struct TxnInsertUnique
{
	/* based is a Vec<u8>, the base's row's value at the key. */
	bool		has_based;
	StdVec		based;
	uint64		generation;
	/* others is a Vec<(Vec<u8>, Option<Vec<u8>>)>. */
	StdVec		others;
	bool		has_count;
	uint64		count;
} TxnInsertUnique;

/*
 * TxnInsertProgress is Progress: a resolve's start-overs that a moved push
 * count caused since it last waited, and the hold-back flag of its stripe it
 * set after enough of them.
 */
typedef struct TxnInsertProgress
{
	uint32		starts;
	bool		has_holding;
	HostKeyHold holding;
} TxnInsertProgress;

/* TxnInsertRechecked is Rechecked: whether a resolve's read still stands under its stripe. */
typedef enum TxnInsertRechecked
{
	/* The read stands. */
	TXN_INSERT_RECHECKED_GO,
	/* The base moved since the read. */
	TXN_INSERT_RECHECKED_MOVED,
	/* The stripe's push count moved since the read. */
	TXN_INSERT_RECHECKED_RETRY,
} TxnInsertRechecked;

/* TxnInsertBase is Base: what allocation reads of the base, its free space tree, fresh end, and generation. */
/* TxnInsertCarriedPut is a carry's put of another writer: its tuple id, its heap bytes, and its writer. */
typedef struct TxnInsertCarriedPut
{
	HostRow		tid;
	uint64		heap_bytes;
	TxnMemtableVersionWriter writer;
} TxnInsertCarriedPut;

/* TxnInsertCarriedTake is a carried put's take: its block and the take's version's record. */
typedef struct TxnInsertCarriedTake
{
	uint32		block;
	StdVec		record;
} TxnInsertCarriedTake;

typedef struct TxnInsertBase
{
	bool		has_tree;
	TreesStoreTree tree;
	uint64		fresh_end;
	uint64		generation;
} TxnInsertBase;

/*
 * TxnInsertPlaced is Placed: a known tuple id's place in its block's free
 * space map entry, read outside any window, for place_decided to decide before a
 * window: the tuple id, the row's room, the base read, the counter read, the
 * block's entry, and the base's entry of the block.
 */
typedef struct TxnInsertPlaced
{
	uint64		tid;
	TxnInsertRoom room;
	TxnInsertBase base;
	uint64		counter;
	bool		has_current;

	/*
	 * current is a Vec<u8>, the block's newest entry, which place_decided
	 * compares (bugs row 342).
	 */
	StdVec		current;
	bool		has_below;
	/* below is a Vec<u8>, the base's entry of the block. */
	StdVec		below;
} TxnInsertPlaced;

/* TxnInsertInsertedTag is Inserted's variants. */
typedef enum TxnInsertInsertedTag
{
	/* The tuple id the write took. */
	TXN_INSERT_INSERTED_TOOK,

	/*
	 * A live row found at an immediate key, with the tuple id the refused
	 * write spent and the live row's when a delta version wrote it.
	 */
	TXN_INSERT_INSERTED_EXISTS,
	/* An update's read stopped it, with the caller's T. */
	TXN_INSERT_INSERTED_STOPPED,
} TxnInsertInsertedTag;

/* TxnInsertInserted is Inserted<T>: what an insert or an update did, a Stopped's T in the caller's bytes. */
typedef struct TxnInsertInserted
{
	TxnInsertInsertedTag tag;
	union
	{
		/* took is the row's tuple id and the locker an update carried to it. */
		struct
		{
			uint64		tuple;
			bool		has_carried;
			uint64		carried;
		}			took;
		struct
		{
			uint64		spent;
			bool		has_live;
			uint64		live;
		}			exists;
	}			u;
} TxnInsertInserted;

/* TxnInsertInsertedRef is an Inserted<T> to format: what it did, and a Stopped's T with T's Debug. */
typedef struct TxnInsertInsertedRef
{
	const TxnInsertInserted *inserted;
	StdDebugRef stopped;
} TxnInsertInsertedRef;

/* TxnInsertCompleted is Completed: how a speculative insert completes. */
typedef enum TxnInsertCompleted
{
	/* Confirmed, its token cleared. */
	TXN_INSERT_COMPLETED_CONFIRMED,
	/* Killed, dead to every reader. */
	TXN_INSERT_COMPLETED_KILLED,
} TxnInsertCompleted;

extern bool txn_insert_Inserted_PartialEq_eq(const TxnInsertInserted *self, const void *self_stopped,
											 const TxnInsertInserted *other, const void *other_stopped,
											 bool (*eq) (const void *a, const void *b));
extern bool txn_insert_Inserted_Debug_fmt(const void *value, StdFormatter *f);
extern void txn_insert_Unique_drop(TxnInsertUnique *self);
extern TxnInsertProgress txn_insert_Progress_Default_default(void);
extern bool txn_insert_Progress_holding(const TxnInsertProgress *self);
extern void txn_insert_Progress_waiting(TxnInsertProgress *self);
extern void txn_insert_Progress_drop(TxnInsertProgress *self);
extern bool txn_insert_based(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const uint8 *key,
							 size_t key_len, const TxnInsertUniqueness *unique, uint32 stripe, TxnInsertUnique *result,
							 HostError *error);
extern bool txn_insert_resolving(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord record, const uint8 *key,
								 size_t key_len, const TxnInsertUnique *read, TxnVisibleResolved *result_0,
								 bool *has_result_1, TxnMemtableVersionHandle *result_1, HostError *error);
extern TxnInsertRechecked txn_insert_rechecked(HostIoKeyIo *io, HostSharedRecord record, const TxnInsertUnique *read,
											   uint32 stripe, TxnInsertProgress *progress);
extern bool txn_insert_waited(HostIoIo *io, HostIoWaits *waits, uint32 index, HostXid xid, bool has_tid, HostRow tid,
							  bool has_token, uint32 token, HostError *error);
extern bool txn_insert_Placed_read(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, HostRow tid,
								   uint64 heap_bytes, const TxnInsertPage *page, TxnInsertPlaced *result,
								   HostError *error);
extern TxnMemtableMapTake txn_insert_Placed_take(const TxnInsertPlaced *self, HostSharedRecord record);

/*
 * TxnInsertOutcome answers whether the WAL ahead of replay ends a top-level
 * transaction, by its commit or its abort, at or before a base's point (bugs
 * row 401).
 */
typedef bool (*TxnInsertOutcome) (void *captures, HostIoIo *io, uint64 top, uint64 point, bool *result,
								  HostError *error);

/*
 * TxnInsertRun is a redo's view of a base that a record ends at or before
 * (bugs row 401): the base's point, whether the point applies the record's
 * writer or saw it abort, and how to ask the WAL ahead of replay for the
 * outcome of a writer the commit log still shows open.
 */
typedef struct TxnInsertRun
{
	uint64		point;
	bool		applied;
	TxnInsertOutcome ask;
	void	   *captures;
} TxnInsertRun;

extern void txn_insert_Placed_drop(TxnInsertPlaced *self);
extern void txn_insert_carried_takes_drop(StdVec *takes);
extern bool txn_insert_carried_takes(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
									 const TxnInsertCarriedPut *puts, size_t puts_len, const TxnInsertPage *page,
									 StdVec *result, HostError *error);
extern bool txn_insert_place_decided(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t take,
									 HostSharedRecord record, const TxnInsertPlaced *placed,
									 TxnMemtableMapDecision *result, bool *moved, HostError *error);
extern bool txn_insert_place_taken(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t take,
								   TxnMemtableVersionWriter writer, bool has_roomed, TxnStatusRoomed roomed,
								   bool has_lsn, uint64 lsn, const TxnInsertPlaced *placed,
								   const TxnMemtableMapDecision *decision, HostError *error);
extern bool txn_insert_Room_fits(const TxnInsertRoom *self, uint64 free, uint16 offset);
extern uint64 txn_insert_Room_need(const TxnInsertRoom *self);
extern bool txn_insert_insert(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							  TxnInsertInsert *row, const TxnInsertRoom *room, const TxnVacuumPrune *prune,
							  const HostSharedHeld *toast, const TxnValuesEntry *toast_entries,
							  size_t toast_entries_len, TxnInsertKeyed keyed, HostIoWaits *waits,
							  TxnInsertInserted *result, HostError *error);
extern bool txn_insert_update(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							  TxnInsertInsert *row, TxnInsertReplaces *replaces, const TxnInsertRoom *room,
							  const TxnVacuumPrune *prune, const HostSharedHeld *toast,
							  const TxnValuesEntry *toast_entries, size_t toast_entries_len, TxnInsertKeyed keyed,
							  HostIoWaits *waits, TxnChangeDecide decide, void *decide_captures,
							  void (*decide_drop) (void *), void *stopped, TxnInsertInserted *result,
							  HostError *error);
extern bool txn_insert_complete(HostIoIo *io, const HostSharedHeld *held, const TxnRecordCompletion *completion,
								TxnInsertCompleted completed, bool logged, bool *result, HostError *error);
extern bool txn_insert_loaded(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							  const TxnInsertPage *page, uint64 to, HostError *error);
extern bool txn_insert_redo(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							const TxnRecordRowChange *change, uint64 lsn, const TxnInsertPage *page,
							const TxnInsertRun *run, const HostSharedHeld *toast, const TxnValuesEntry *toast_entries,
							size_t toast_entries_len, HostError *error);
extern bool txn_insert_redo_spent(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
								  const TxnRecordSpent *spent, uint64 end, const TxnInsertPage *page,
								  const TxnInsertRun *run, HostError *error);
extern bool txn_insert_redo_left(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
								 const TxnRecordPageLeft *left, uint64 end, const TxnInsertPage *page,
								 HostError *error);
extern void txn_insert_runs_passed(HostIoIo *io, uint64 end);
extern bool txn_insert_run_out(HostIoIo *io, const TxnInsertRun *run, const HostSharedHeld *held,
							   TxnMemtableMapMap map, uint32 key, bool *result, HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern bool txn_insert_tests_taken(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
								   TxnInsertInsert *row, const TxnInsertRoom *room, const HostSharedHeld *toast,
								   const TxnValuesEntry *toast_entries, size_t toast_entries_len, uint64 *result,
								   HostError *error);
extern HostStorage txn_insert_tests_stored(uint32 database);
extern TxnInsertInsert txn_insert_tests_row(HostStorage storage, uint64 n);
extern void test_txn_insert_tests_inserts_take_heaps_tuple_ids_by_offsets_and_by_bytes(void);
extern void test_txn_insert_tests_a_row_the_last_page_cant_hold_goes_to_the_lowest_page_left_with_room(void);
extern void test_txn_insert_tests_redo_leaves_the_free_space_map_and_the_counter_as_the_primarys(void);
extern void test_txn_insert_tests_a_take_redone_again_changes_nothing(void);
extern void test_txn_insert_tests_an_insert_maps_its_out_of_line_values_in_its_window(void);
extern void test_txn_insert_tests_a_gathering_from_an_inserts_prepare_to_its_push_keeps_the_counter(void);
extern void test_txn_insert_tests_an_insert_that_leaves_the_fresh_page_and_fails_redoes_as_the_primary(void);
extern void test_txn_insert_tests_a_row_whose_value_references_sit_in_an_overflow_chunk_redoes_its_map_entries(void);
extern void test_txn_insert_tests_an_insert_that_fails_after_its_chunks_maps_no_value(void);
extern void test_txn_insert_tests_redo_from_any_point_gives_a_row_with_all_its_map_entries_or_none(void);
extern void test_txn_insert_tests_a_kept_values_count_add_is_redone_once(void);
extern void test_txn_insert_tests_a_load_moves_the_counter_past_every_loaded_tid_and_left_page(void);
extern void test_txn_insert_tests_an_insert_logical_decoding_reads_logs_heaps_record_of_its_command(void);
extern void test_txn_insert_tests_a_checkpoints_runs_and_every_record_after_redo_as_the_primary(void);
extern void test_txn_insert_tests_a_window_between_a_sweeps_rows_and_its_cut_leaves_its_take_without_its_row(void);
extern void test_txn_insert_tests_windows_after_a_sweeps_cut_are_in_its_runs_with_neither_their_rows_nor_their_takes(void);
extern void test_txn_insert_tests_an_aborted_inserts_versions_and_takes_stay_until_a_fold(void);
extern void test_txn_insert_tests_an_update_keeps_its_key_or_moves_it_and_redoes_as_the_primary(void);
extern void test_txn_insert_tests_a_keyless_or_deferrable_update_names_its_rows_by_their_tuple_ids(void);
extern void test_txn_insert_tests_an_updates_combo_command_shows_on_the_row_it_ends(void);
extern void test_txn_insert_tests_a_speculative_insert_holds_its_token_until_confirmed(void);
extern void test_txn_insert_tests_an_aborted_updates_new_row_and_end_stay_until_a_fold(void);
extern void test_txn_insert_tests_a_resolve_whose_stripe_moves_three_times_holds_its_writers_back(void);
extern void test_txn_insert_tests_a_checkpoint_between_an_updates_count_adds_and_its_row_leaves_the_counts_once(void);
extern void test_txn_insert_tests_a_checkpoint_before_or_after_an_update_leaves_its_kept_counts_once(void);
extern void test_txn_insert_tests_a_crash_after_a_key_change_whose_runs_hold_one_chain_redoes_both(void);
extern void test_txn_insert_tests_a_resolve_whose_first_target_is_refused_ends_in_bounded_turns(void);
extern void test_txn_insert_tests_a_frozen_inserts_row_change_replays_with_no_status_room(void);
extern void test_txn_insert_tests_a_takes_run_past_a_base_ends_at_the_first_writer_it_didnt_apply(void);
#endif

#endif							/* DOLTGRES_TXN_INSERT_H */
