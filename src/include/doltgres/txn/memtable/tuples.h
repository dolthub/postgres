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
										TxnMemtableTableReservation *result, HostError *error);
extern bool txn_memtable_tuples_given(HostIoIo *io, HostSharedRecord state, HostError *error);
extern bool txn_memtable_tuples_retired(HostIoIo *io, HostSharedRecord state, HostError *error);
extern bool txn_memtable_tuples_dropped(HostIoIo *io, HostSharedRecord state, const uint64 *tids, size_t tids_len,
										HostError *error);

#endif							/* DOLTGRES_TXN_MEMTABLE_TUPLES_H */
