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
 * A memtable's ranges (txn's memtable/range.rs): the shapes of a range of a
 * storage's keys and of a memtable, which name their kinds. Part of txn's
 * contract.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_RANGE_H
#define DOLTGRES_TXN_MEMTABLE_RANGE_H

#include "doltgres/host/shared.h"
#include "doltgres/txn/memtable/index.h"

/* A range's lock, RangeLock, which owns its pages and guards its index, its versions, and its list. */
#define TXN_MEMTABLE_RANGE_RANGE_LOCK 0

/* A range's atomics, by their INDEX in range.rs. */
#define TXN_MEMTABLE_RANGE_RANGE_FIRST 0
#define TXN_MEMTABLE_RANGE_RANGE_CLOSED 1
#define TXN_MEMTABLE_RANGE_RANGE_START 2
#define TXN_MEMTABLE_RANGE_RANGE_KEYS 3
#define TXN_MEMTABLE_RANGE_RANGE_BYTES 4
#define TXN_MEMTABLE_RANGE_RANGE_KEPT 5
#define TXN_MEMTABLE_RANGE_RANGE_LOWER 6
#define TXN_MEMTABLE_RANGE_RANGE_UPPER 7
#define TXN_MEMTABLE_RANGE_RANGE_NEWEST 8
#define TXN_MEMTABLE_RANGE_RANGE_SWEPT 9
#define TXN_MEMTABLE_RANGE_RANGE_REMOVED 10
#define TXN_MEMTABLE_RANGE_RANGE_MOVED 11

/* TXN_MEMTABLE_RANGE_RANGES is RANGES, the index of a memtable's list of its ranges' locations among its record's lists. */
#define TXN_MEMTABLE_RANGE_RANGES 0

/* A memtable's lock, MemtableLock, which guards its list of ranges. */
#define TXN_MEMTABLE_RANGE_MEMTABLE_LOCK 0

/* A memtable's atomics: MemtableClosed, its closed mark, and MemtableKept, its count of its list's entries kept for splits. */
#define TXN_MEMTABLE_RANGE_MEMTABLE_CLOSED 0
#define TXN_MEMTABLE_RANGE_MEMTABLE_KEPT 1

/* TXN_MEMTABLE_RANGE_RANGE_AGED_OPS is Range's Shape and Aged: a range of a storage's keys in its memtable. */
extern const HostSharedAgedOps TXN_MEMTABLE_RANGE_RANGE_AGED_OPS;

/* TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS is Range's Keyed: a range's index of its keys. */
extern const TxnMemtableIndexKeyedOps TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS;

/* TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS is Memtable's Shape and Aged: a memtable, its list of ranges. */
extern const HostSharedAgedOps TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS;

/* TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS is Memtable's Closes: a memtable's lock and its closed mark. */
extern const TxnMemtableIndexClosesOps TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS;

#endif							/* DOLTGRES_TXN_MEMTABLE_RANGE_H */
