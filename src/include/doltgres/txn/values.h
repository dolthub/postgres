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
 * The TOAST values the fork keeps (txn's values.rs): each value id's entry in
 * a TOAST relation's value map.
 */
#ifndef DOLTGRES_TXN_VALUES_H
#define DOLTGRES_TXN_VALUES_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/lib.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/tuple.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/memtable/map.h"

/*
 * TxnValuesEntry is Entry: a value id's entry in a TOAST relation's value
 * map, whose count is never zero, since an entry goes when its count reaches
 * zero.
 */
typedef struct TxnValuesEntry
{
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;
	uint64		raw;
	uint64		stored;
	uint64		count;
} TxnValuesEntry;

/*
 * TXN_VALUES_TOAST_MAX_CHUNK_SIZE is Postgres' TOAST_MAX_CHUNK_SIZE, the
 * bytes of a value heap stores in each toast chunk: a quarter of a page past
 * its header and four line pointers, rounded down to MAXALIGN, less a chunk
 * tuple's header, its chunk_id and chunk_seq, and its data's varlena header.
 */
#define TXN_VALUES_TOAST_MAX_CHUNK_SIZE ((((uint64) CHUNKS_BLCKSZ - 40) / 4 / 8 * 8) - 24 - 4 - 4 - 4)

/* TXN_VALUES_MOST is the longest value of an entry: the address, the locator, and three varints at their longest. */
#define TXN_VALUES_MOST (32 + CHUNKS_TRAILER_LOCATOR_LEN + 3 * 10)

extern TxnValuesEntry txn_values_Entry_new(ChunksAddressAddress address, uint64 length);
extern bool txn_values_Entry_decode(const uint8 *value, size_t value_len, TxnValuesEntry *result,
									HostError *error);
extern TreesStoreTree txn_values_Entry_tree(const TxnValuesEntry *self);
extern std_TupleU64U64_t txn_values_mark(HostSharedRecord record);
extern bool txn_values_free(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *toast, uint32 id,
							bool *result, HostError *error);
extern bool txn_values_entry(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *toast, uint32 id,
							 bool *has_result, TxnValuesEntry *result, HostError *error);

extern void txn_values_key(uint32 id, uint8 result[4]);
extern void txn_values_Entry_encode_into(const TxnValuesEntry *self, StdVec *out);
extern bool txn_values_Entry_PartialEq_eq(const TxnValuesEntry *self, const TxnValuesEntry *other);
extern bool txn_values_Entry_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_values_sizes(const uint8 *key, size_t key_len, const uint8 *value, size_t value_len,
							 TxnBaseSizes *result, HostError *error);
extern TxnMemtableMapStep txn_values_added(bool has_current, const uint8 *current, size_t current_len, StdVec *out);
extern TxnMemtableMapStep txn_values_taken_down(bool has_current, const uint8 *current, size_t current_len,
												StdVec *out, uint64 by);
extern bool txn_values_based(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *toast, uint32 id,
							 bool *has_value, StdVec *value, std_TupleU64U64_t *mark, HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_values_tests_a_value_map_entry_reads_back_and_refuses_every_other_form(void);
extern void test_txn_values_tests_a_value_id_the_map_holds_is_skipped(void);
extern void test_txn_values_tests_a_value_ids_entry_reads_back(void);
extern void test_txn_values_tests_a_value_counts_heaps_toast_chunks(void);
extern void test_txn_values_tests_a_take_down_takes_one_from_a_count_and_removes_the_last(void);
#endif

#endif							/* DOLTGRES_TXN_VALUES_H */
