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
 * A checkpoint's runs of the delta (txn's runs.rs): the trees a run's file
 * holds, a repository's spills' directory, and the start's load, cleaning,
 * the checkpoint's save, and a database's drop. Part of txn's contract.
 */
#ifndef DOLTGRES_TXN_RUNS_H
#define DOLTGRES_TXN_RUNS_H

#include "doltgres/host/cache.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/path.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"

/* TxnInsertPage is insert's Page, which runs reaches only by pointer; its definition comes with insert's header. */
typedef struct TxnInsertPage TxnInsertPage;

/* TxnRunsKind is Kind: the tree a run's file holds, by the number its file names. */
typedef enum TxnRunsKind
{
	TXN_RUNS_KIND_ROWS = 0,
	TXN_RUNS_KIND_FREE = 1,
	TXN_RUNS_KIND_VALUES = 2,
	TXN_RUNS_KIND_CHUNKS = 3,
	TXN_RUNS_KIND_STATUS = 4,
	TXN_RUNS_KIND_SPILLED_ROWS = 5,
	TXN_RUNS_KIND_SPILLED_VALUES = 6,
	TXN_RUNS_KIND_SPILLED_TUPLES = 7,
	/* An index's storage's versions, framed as a rows run's. */
	TXN_RUNS_KIND_ENTRIES = 8,

	/*
	 * An index's storage's own versions a transaction spilled, read back as
	 * an entries run's.
	 */
	TXN_RUNS_KIND_SPILLED_ENTRIES = 9,
	/* A database's identity tree's versions, framed as a rows run's. */
	TXN_RUNS_KIND_IDENTITY = 10,

	/*
	 * The chunks of a transaction's blobs it spilled from its repository's
	 * working state's delta.
	 */
	TXN_RUNS_KIND_SPILLED_CHUNKS = 12,
} TxnRunsKind;

/* TXN_RUNS_FILTER_WORDS is FILTER_WORDS, the most words of a page of a spill's filter: the node cache's smallest frame. */
#define TXN_RUNS_FILTER_WORDS (HOST_CACHE_CLASSES[0].f0 / 8)

/* TxnRunsNames is Names: what a run's header names, its tree's kind, its storage's persistence, the storage, and its redo point or a spill's xid. */
typedef struct TxnRunsNames
{
	TxnRunsKind kind;
	TxnStatePersistence persistence;
	uint32		tablespace;
	uint32		database;
	uint32		relation;
	HostLsn		redo;
} TxnRunsNames;

/* TxnRunsTailTag is Tail's variants. */
typedef enum TxnRunsTailTag
{
	TXN_RUNS_TAIL_NONE,
	TXN_RUNS_TAIL_COUNTER,
	TXN_RUNS_TAIL_INDEX,
} TxnRunsTailTag;

/* TxnRunsTail is Tail: what a run's header holds past its redo point, a free space run's counter or an entries run's table and index. */
typedef struct TxnRunsTail
{
	TxnRunsTailTag tag;
	uint64		counter;
	uint32		table;
	uint32		index;
} TxnRunsTail;

/* TxnRunsKey is Key: one key's versions as a run holds them, newest first, borrowing the run's bytes. */
typedef struct TxnRunsKey
{
	StdBytes	key;
	/* versions is a Vec<Record>. */
	StdVec		versions;
} TxnRunsKey;

/* TxnRunsDecoded is Decoded: a run's file read back, its storage's persistence, its header's tail, and its keys. */
typedef struct TxnRunsDecoded
{
	TxnStatePersistence persistence;
	TxnRunsTail tail;
	/* keys is a Vec<Key>. */
	StdVec		keys;
} TxnRunsDecoded;

/* txn_TupleVecU8U64_t is an Index entry, (Vec<u8>, u64): a key and the place of the key frame it names. */
typedef struct txn_TupleVecU8U64_t
{
	StdVec		f0;
	uint64		f1;
} txn_TupleVecU8U64_t;

/* txn_TupleVecU8VecVecU8_t is Spilled, (Vec<u8>, Vec<Vec<u8>>): a key of a spill's file with its records, newest first. */
typedef struct txn_TupleVecU8VecVecU8_t
{
	StdVec		f0;
	StdVec		f1;
} txn_TupleVecU8VecVecU8_t;

/* txn_TupleRefSliceU8U64_t is index's (&[u8], u64): an index frame's entry, borrowing the frame. */
typedef struct txn_TupleRefSliceU8U64_t
{
	StdBytes	f0;
	uint64		f1;
} txn_TupleRefSliceU8U64_t;

/* txn_TupleU64RefSliceU8_t is decode_tuples' (u64, &[u8]): a tuple id and its row's key, borrowing the file's bytes. */
typedef struct txn_TupleU64RefSliceU8_t
{
	uint64		f0;
	StdBytes	f1;
} txn_TupleU64RefSliceU8_t;

/* txn_TupleU64OutcomeU64_t is decode_status' (u64, Outcome, u64): a top-level xid, its outcome, and its saved chain entries. */
typedef struct txn_TupleU64OutcomeU64_t
{
	uint64		f0;
	TxnStatusOutcome f1;
	uint64		f2;
} txn_TupleU64OutcomeU64_t;

/* TxnRunsWriting is Writing: a run's file while it is written; its definition is runs.c's. */
typedef struct TxnRunsWriting TxnRunsWriting;

extern bool txn_runs_Kind_of(uint8 number, TxnRunsKind *result);
extern bool txn_runs_Kind_spilled(TxnRunsKind self);
extern bool txn_runs_Names_PartialEq_eq(const TxnRunsNames *self, const TxnRunsNames *other);
extern void txn_runs_Index_drop(StdVec *index);
extern void txn_runs_Chunk_drop(StdVec *chunk);
extern void txn_runs_Decoded_drop(TxnRunsDecoded *self);
extern bool txn_runs_decode(const uint8 *bytes, size_t bytes_len, const TxnRunsNames *names, uint64 hash,
							TxnRunsDecoded *result, HostError *error);
extern bool txn_runs_spill_bits(const uint8 *bytes, size_t bytes_len, uint64 *result, HostError *error);
extern bool txn_runs_spill_filter(HostIoIo *io, const HostFile *file, uint64 len, uint64 bits, uint64 page,
								  StdVec *result, HostError *error);
extern bool txn_runs_spill_hash(const uint8 *bytes, size_t bytes_len, uint64 *result, HostError *error);
extern bool txn_runs_spill_tail_hash(HostIoIo *io, const HostFile *file, uint64 len, uint64 *result,
									 HostError *error);
extern bool txn_runs_spill_tail_count(HostIoIo *io, const HostFile *file, uint64 len, uint64 *result,
									  HostError *error);
extern bool txn_runs_spill_index(HostIoIo *io, const HostFile *file, uint64 len, uint64 *index_at, StdVec *result,
								 HostError *error);
extern bool txn_runs_spill_chunk(HostIoIo *io, const HostFile *file, const TxnRunsNames *names, uint64 start,
								 uint64 end, StdVec *result, HostError *error);
extern bool txn_runs_index(const uint8 *record_ptr, size_t record_len, StdVec *result, HostError *error);
extern bool txn_runs_decode_tuples(const uint8 *bytes, size_t bytes_len, const TxnRunsNames *names, uint64 hash,
								   StdVec *result, HostError *error);
extern bool txn_runs_decode_status(const uint8 *bytes, size_t bytes_len, const TxnRunsNames *names, uint64 hash,
								   StdVec *result, HostError *error);
extern void txn_runs_directory(uint32 repository, uint32 own, HostLsn redo, StdPathBuf *result);
extern void txn_runs_spill_name(const TxnRunsNames *names, uint32 number, StdString *result);
extern TxnRunsWriting *txn_runs_Writing_spill(const char *dir, size_t dir_len, const TxnRunsNames *names,
											  TxnRunsTail tail, uint32 number);
extern StdBytes txn_runs_Writing_path(const TxnRunsWriting *self);
extern bool txn_runs_Writing_key(TxnRunsWriting *self, HostIoIo *io, const uint8 *key, size_t key_len,
								 const StdBytes *records, size_t records_len, HostError *error);
extern bool txn_runs_Writing_end(TxnRunsWriting *self, HostIoIo *io, const uint64 *filter, size_t filter_len,
								 bool *some, RepoRunFile *result, HostError *error);
extern void txn_runs_Writing_drop(TxnRunsWriting *self);
extern void txn_runs_spill_directory(uint32 repository, uint32 own, StdPathBuf *result);
extern bool txn_runs_cleaned(HostIoIo *io, HostError *error);
extern bool txn_runs_checkpoint(HostIoIo *io, HostLsn redo, HostError *error);
extern bool txn_runs_loaded(HostIoIo *io, const TxnInsertPage *page, HostError *error);
extern bool txn_runs_dropped(HostIoIo *io, uint32 repository, HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_runs_tests_a_checkpoint_names_a_running_spill_and_a_start_links_it_back(void);
extern void test_txn_runs_tests_a_spills_decoder_refuses_another_writer_and_a_wrong_index(void);
extern void test_txn_runs_tests_a_spills_file_reads_back_with_its_index(void);
extern void test_txn_runs_tests_an_entries_runs_header_holds_its_table_and_index(void);
extern void test_txn_runs_tests_an_index_storages_loaded_entries_stay_an_entries_run_through_a_second_checkpoint(void);
extern void test_txn_runs_tests_a_takes_ender_loads_back_from_its_free_space_run(void);
extern void test_txn_runs_tests_a_runs_decoder_takes_a_rows_ender_only_on_a_transactions_take(void);
extern void test_txn_runs_tests_a_large_status_run_keeps_every_entry_through_its_load(void);
extern void test_txn_runs_tests_an_entries_run_reads_its_entries_and_refuses_heap_bytes_tuple_ids_and_other_ops(void);
extern void test_txn_runs_tests_a_checkpoints_status_run_gives_the_loaded_writers_their_outcomes(void);
extern void test_txn_runs_tests_a_status_run_reads_back_its_outcomes_and_refuses_what_isnt_one(void);
extern void test_txn_runs_tests_a_start_skips_a_repository_whose_run_is_damaged_and_loads_the_others(void);
extern void test_txn_runs_tests_a_recoverys_cleanup_resets_an_unlogged_storages_saved_base(void);
extern void test_txn_runs_tests_a_recoverys_cleanup_drops_an_unlogged_storages_loaded_rows(void);
extern void test_txn_runs_tests_a_start_moves_each_storage_with_no_free_space_run_past_its_rows(void);
extern void test_txn_runs_tests_a_start_fails_on_a_failure_that_isnt_corruption_and_keeps_its_runs(void);
extern void test_txn_runs_tests_a_start_warns_of_a_manifest_it_cant_read_and_loads_the_others_runs(void);
extern void test_txn_runs_tests_a_checkpoints_runs_survive_a_power_loss(void);
extern void test_txn_runs_tests_a_start_removes_the_runs_no_manifest_names_and_loads_the_named(void);
extern void test_txn_runs_tests_a_start_refuses_a_run_whose_length_isnt_its_entrys(void);
extern void test_txn_runs_tests_a_start_refuses_a_free_space_run_whose_counter_isnt_a_tuple_ids(void);
extern void test_txn_runs_tests_a_start_refuses_runs_whose_row_is_at_or_past_their_cut(void);
extern void test_txn_runs_tests_a_checkpoints_runs_load_back_into_the_delta(void);
extern void test_txn_runs_tests_a_checkpoints_runs_hold_each_trees_versions(void);
extern void test_txn_runs_tests_a_checkpoint_saves_its_runs_and_removes_the_unnamed(void);
extern void test_txn_runs_tests_a_runs_file_reads_back_as_written(void);
extern void test_txn_runs_tests_a_runs_decoder_refuses_what_isnt_its_run(void);
#endif

#endif							/* DOLTGRES_TXN_RUNS_H */
