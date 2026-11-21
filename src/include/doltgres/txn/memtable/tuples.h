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
 * A storage's tuple id table in its delta (txn's memtable/tuples.rs): its
 * shape, a prepared reservation's finish, a tid's key, and the table's
 * retirement once it is empty. Part of txn's contract.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_TUPLES_H
#define DOLTGRES_TXN_MEMTABLE_TUPLES_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/table.h"

/* TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS is Tuples' Shape and Aged: a storage's tuple id table, its slot and byte pages and its counts. */
extern const HostSharedAgedOps TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS;

extern bool txn_memtable_tuples_finish(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, HostRow tid,
									   HostError *error);
extern bool txn_memtable_tuples_get(HostIoIo *io, HostSharedRecord state, HostRow tid, bool *some, StdVec *result,
									HostError *error);
extern bool txn_memtable_tuples_emptied(HostIoIo *io, HostSharedRecord state, HostError *error);

extern bool txn_memtable_tuples_reserve(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len,
										TxnMemtableTableReservation *result, TxnMemtableTableRefusal *refused,
										HostError *error);
extern bool txn_memtable_tuples_given(HostIoIo *io, HostSharedRecord state, HostError *error);
extern bool txn_memtable_tuples_retired(HostIoIo *io, HostSharedRecord state, HostError *error);
extern bool txn_memtable_tuples_dropped(HostIoIo *io, HostSharedRecord state, const uint64 *tids, size_t tids_len,
										HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_memtable_tuples_tests_a_reader_held_across_a_growth_reads_from_the_next_table(void);
extern void test_txn_memtable_tuples_tests_two_prepares_at_the_bound_each_get_a_slot_through_one_growth(void);
extern void test_txn_memtable_tuples_tests_a_tid_used_again_names_its_newest_key(void);
extern void test_txn_memtable_tuples_tests_a_reservation_no_finish_stored_is_given_back(void);
extern void test_txn_memtable_tuples_tests_two_tids_of_one_first_slot_are_each_found(void);
extern void test_txn_memtable_tuples_tests_a_tables_first_pages_are_the_smallest_that_hold_them(void);
extern void test_txn_memtable_tuples_tests_a_tables_tids_are_found_after_inserts_and_a_growth(void);
extern void test_txn_memtable_tuples_tests_a_table_of_mostly_dropped_tids_is_compacted_at_its_load_factor(void);
extern void test_txn_memtable_tuples_tests_a_compaction_with_a_reservation_outstanding_moves_no_byte_page(void);
extern void test_txn_memtable_tuples_tests_a_writer_waits_on_a_compactions_exclusive_lock(void);
extern void test_txn_memtable_tuples_tests_a_compaction_failed_at_any_step_leaves_every_tid_found(void);

/* txn_memtable_tuples_tests_stored_t is stored's prepare's captures: the tid and the change. */
typedef struct txn_memtable_tuples_tests_stored_t
{
	uint64		tid;
	const TxnMemtableVersionChange *change;
} txn_memtable_tuples_tests_stored_t;

extern bool txn_memtable_tuples_tests_stored_inside(void *captures, HostIoIo *io,
													TxnMemtableTablePreparedState *prepared, HostError *error);
#endif

#endif							/* DOLTGRES_TXN_MEMTABLE_TUPLES_H */
