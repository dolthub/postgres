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
 * A storage's delta maps (txn's memtable/map.rs): which of its maps a
 * memtable is, a take's steps, and the entries and takers a map holds.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_MAP_H
#define DOLTGRES_TXN_MEMTABLE_MAP_H

#include "doltgres/host/delta.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/version.h"

/* TXN_MEMTABLE_MAP_KEY is KEY, the length of a map's key, a u32 in big-endian order. */
#define TXN_MEMTABLE_MAP_KEY 4

/* TxnMemtableMapMap is Map: one of a storage's delta maps. */
typedef enum TxnMemtableMapMap
{
	/* The memtable of a storage's free space map. */
	TXN_MEMTABLE_MAP_MAP_FREE,
	/* The memtable of a storage's value map. */
	TXN_MEMTABLE_MAP_MAP_VALUES,
} TxnMemtableMapMap;

/*
 * TxnMemtableMapTake is Take: a take of a storage's map's key, the state
 * whose word names the map, the map, its key, and the longest value it
 * writes.
 */
typedef struct TxnMemtableMapTake
{
	HostSharedRecord state;
	TxnMemtableMapMap map;
	uint32		key;
	size_t		most;
} TxnMemtableMapTake;

/*
 * TxnMemtableMapStep is Step<T>'s variants: what a take's function answers
 * from the key's current value, a put, a removal, a try again after the
 * window, or a refusal; a put's and a removal's T comes beside it.
 */
typedef enum TxnMemtableMapStep
{
	TXN_MEMTABLE_MAP_STEP_PUT,
	TXN_MEMTABLE_MAP_STEP_REMOVE,
	TXN_MEMTABLE_MAP_STEP_RETRY,
	TXN_MEMTABLE_MAP_STEP_REFUSE,
} TxnMemtableMapStep;

/* TxnMemtableMapTaken is Taken<T>'s variants: what a take did; a took's T comes beside it. */
typedef enum TxnMemtableMapTaken
{
	TXN_MEMTABLE_MAP_TAKEN_TOOK,
	TXN_MEMTABLE_MAP_TAKEN_RETRY,
	TXN_MEMTABLE_MAP_TAKEN_REFUSED,
} TxnMemtableMapTaken;

/* TxnMemtableMapEntry is Entry, an Option<Vec<u8>>: a key's newest entry in a map, its value, or none for a removal. */
typedef struct TxnMemtableMapEntry
{
	bool		has_value;
	StdVec		value;
} TxnMemtableMapEntry;

/* txn_TupleVecU32Bool_t is a Taker's (Vec<u32>, bool): a row's value ids and whether the version is a values mark. */
typedef struct txn_TupleVecU32Bool_t
{
	/* f0 is a Vec<u32>. */
	StdVec		f0;
	bool		f1;
} txn_TupleVecU32Bool_t;

/*
 * txn_TupleWriterRowEntryU64TupleVecU32Bool_t is Taker: a free space entry's
 * version as VACUUM reads it, its writer, the tuple id its take took, its
 * entry, its ender's full xid, and its row's value ids with whether it is a
 * values mark.
 */
typedef struct txn_TupleWriterRowEntryU64TupleVecU32Bool_t
{
	TxnMemtableVersionWriter f0;
	HostRow		f1;
	TxnMemtableMapEntry f2;
	uint64		f3;
	txn_TupleVecU32Bool_t f4;
} txn_TupleWriterRowEntryU64TupleVecU32Bool_t;

/* txn_TupleU64VecVecU8_t is cut's (u64, Vec<Vec<u8>>): a storage's tuple id counter and its block's free space map records. */
typedef struct txn_TupleU64VecVecU8_t
{
	uint64		f0;
	/* f1 is a Vec<Vec<u8>>. */
	StdVec		f1;
} txn_TupleU64VecVecU8_t;

/* txn_TupleU32Entry_t is seek's (u32, Entry): a map's key and its newest entry. */
typedef struct txn_TupleU32Entry_t
{
	uint32		f0;
	TxnMemtableMapEntry f1;
} txn_TupleU32Entry_t;

/* TxnMemtableTablePreparedState is table.h's Prepared<State>, which map reaches only by pointer, as table.h includes map.h. */
struct TxnMemtableTablePreparedState;

/* TXN_MEMTABLE_MAP_FREE_TREE_OPS is Free's Tree: the memtable of a storage's free space map, of its State. */
extern const struct TxnMemtableTableTreeOps TXN_MEMTABLE_MAP_FREE_TREE_OPS;

/* TXN_MEMTABLE_MAP_VALUES_TREE_OPS is Values' Tree: the memtable of a storage's value map, of its State. */
extern const struct TxnMemtableTableTreeOps TXN_MEMTABLE_MAP_VALUES_TREE_OPS;

extern bool txn_memtable_map_Map_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_map_named(HostIoTakesShort io, struct TxnMemtableTablePreparedState *prepared, size_t take,
								   TxnMemtableVersionWriter writer, bool has_lsn, uint64 lsn, HostRow tuple,
								   StdVec *values, HostError *error);
extern size_t txn_memtable_map_named_most(size_t count);
extern bool txn_memtable_map_valued(const TxnMemtableVersionVersion *version, bool *some, StdBytes *result,
									HostError *error);
extern bool txn_memtable_map_versions(const struct TxnMemtableTableTreeOps *tree, HostIoIo *io,
									  const HostDeltaHold *hold, HostSharedRecord state, uint32 key, StdVec *result,
									  HostError *error);
extern bool txn_memtable_map_newest(HostIoIo *io, HostSharedRecord state, TxnMemtableMapMap map, uint32 key,
									bool *some, TxnMemtableMapEntry *result, HostError *error);
extern bool txn_memtable_map_takers(HostIoIo *io, HostSharedRecord state, TxnMemtableMapMap map, uint32 key,
									StdVec *result, HostError *error);
extern bool txn_memtable_map_ended(HostIoIo *io, HostSharedRecord state, HostRow tid, uint32 ender, HostError *error);
extern void txn_memtable_map_takers_drop(StdVec *takers);
extern HostRow txn_memtable_map_taken(TxnMemtableVersionOpRef op);
extern bool txn_memtable_map_marked(HostIoIo *io, HostSharedRecord state, uint32 block, uint64 lsn, StdVec *result,
									HostError *error);
extern bool txn_memtable_map_marks(HostIoIo *io, HostSharedRecord state, uint32 block, HostRow tid, uint64 lsn,
								   bool *result, HostError *error);
extern bool txn_memtable_map_holds(HostIoIo *io, HostSharedRecord state, TxnMemtableMapMap map, uint32 key, uint64 lsn,
								   bool *result, HostError *error);
extern bool txn_memtable_map_raised(HostSharedRecord state, const uint8 *key, size_t key_len, bool has_was, uint8 was,
									const uint8 *value, size_t value_len, HostError *error);
extern bool txn_memtable_map_cut(HostIoIo *io, HostSharedRecord state, bool *some, txn_TupleU64VecVecU8_t *result,
								 HostError *error);
extern bool txn_memtable_map_took_core(HostIoTakesShort io, struct TxnMemtableTablePreparedState *prepared,
									   size_t take, TxnMemtableVersionWriter writer, bool has_lsn, uint64 lsn,
									   bool has_claim, HostRow claim_tid, uint64 claim_bytes, bool has_base,
									   const uint8 *base, size_t base_len,
									   TxnMemtableMapStep (*step) (void *captures, bool has_current,
																   const uint8 *current, size_t current_len,
																   StdVec *out),
									   HostRow (*tuple) (void *captures), void (*then) (void *captures),
									   void *captures, TxnMemtableMapTaken *result, HostError *error);
extern bool txn_memtable_map_seek(HostIoIo *io, HostSharedRecord state, TxnMemtableMapMap map, uint32 from, bool *some,
								  txn_TupleU32Entry_t *result, HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void txn_memtable_map_tests_newest_is(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col,
											 HostIoIo *io, HostSharedRecord state, TxnMemtableMapMap map,
											 uint32 key, bool has_expected, const TxnMemtableMapEntry *expected);
extern void test_txn_memtable_map_tests_a_failure_after_the_chunks_and_before_the_window_leaves_the_chunk_unreferenced(void);
extern void test_txn_memtable_map_tests_a_seek_reads_on_after_a_writer_follows_its_read_of_an_empty_map(void);
extern void test_txn_memtable_map_tests_one_window_takes_in_the_rows_storage_and_its_toast_relations(void);
extern void test_txn_memtable_map_tests_a_cut_reads_the_counter_with_its_blocks_entry_from_the_last_range(void);
extern void test_txn_memtable_map_tests_a_map_only_prepare_takes_its_entry_and_makes_no_rows_memtable(void);
extern void test_txn_memtable_map_tests_the_free_space_maps_takes_newest_and_seek(void);
extern void test_txn_memtable_map_tests_the_value_maps_takes_newest_and_seek(void);
extern void test_txn_memtable_map_tests_a_maps_seek_goes_on_past_a_ranges_end(void);
extern void test_txn_memtable_map_tests_a_value_maps_seek_goes_on_past_a_ranges_end(void);
extern void txn_memtable_map_tests_seen_drop(void *kept);
extern bool txn_memtable_map_tests_seen_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_map_tests_seen_eq(const TxnMemtableMapEntry *a, const TxnMemtableMapEntry *b);
#endif

#endif							/* DOLTGRES_TXN_MEMTABLE_MAP_H */
