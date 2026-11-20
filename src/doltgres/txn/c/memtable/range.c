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
 * The C port of txn's memtable/range.rs: a memtable's ranges and the
 * memtable itself, a range an aged record of the delta whose own pages hold
 * its key index and its versions, and a memtable an aged record whose list
 * names its ranges in the order of their first keys, the first of them the
 * empty key's.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/shared.h"
#include "doltgres/txn/memtable/index.h"
#include "doltgres/txn/memtable/range.h"

/* txn_memtable_range_Range_Aged_count is Range's Aged count: a record for each slot of the delta's page directory. */
static size_t
txn_memtable_range_Range_Aged_count(const HostSharedLimits *limits)
{
	return limits->delta_pages;
}

/*
 * TXN_MEMTABLE_RANGE_RANGE_AGED_OPS is Range's Shape and Aged: a range of a
 * storage's keys in its memtable, its lock, twelve atomics, and its list of
 * pages, a range for each slot of the delta's page directory, since each
 * range names a page of its own.
 */
const HostSharedAgedOps TXN_MEMTABLE_RANGE_RANGE_AGED_OPS = {
	.shape = {.id = UINT64CONST(0x74786e000010), .locks = 1, .atomics = 12, .lists = 1},
	.name = {"range", 5},
	.count = txn_memtable_range_Range_Aged_count,
};

/* TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS is Range's Closes and Keyed: RangeLock, RangeClosed, RangeFirst, RangeKeys, RangeBytes, and RangeNewest. */
const TxnMemtableIndexKeyedOps TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS = {
	.closes = {.shape = &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, .lock = TXN_MEMTABLE_RANGE_RANGE_LOCK,
	.closed = TXN_MEMTABLE_RANGE_RANGE_CLOSED},
	.first = TXN_MEMTABLE_RANGE_RANGE_FIRST,
	.count = TXN_MEMTABLE_RANGE_RANGE_KEYS,
	.bytes = TXN_MEMTABLE_RANGE_RANGE_BYTES,
	.newest = TXN_MEMTABLE_RANGE_RANGE_NEWEST,
};

/* TXN_MEMTABLE_RANGE_MEMTABLE_NAMES is Memtable's NAMES: the aged shape its list names, Range. */
static const uint64 TXN_MEMTABLE_RANGE_MEMTABLE_NAMES[] = {UINT64CONST(0x74786e000010)};

/*
 * TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS is Memtable's Shape and Aged: a
 * storage's memtable, its lock, two atomics, and its list of ranges, a
 * memtable for each slot of the delta's page directory, since each memtable
 * names a range.
 */
const HostSharedAgedOps TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS = {
	.shape = {.id = UINT64CONST(0x74786e000011), .locks = 1, .atomics = 2, .lists = 1},
	.name = {"memtable", 8},
	.names = TXN_MEMTABLE_RANGE_MEMTABLE_NAMES,
	.names_len = lengthof(TXN_MEMTABLE_RANGE_MEMTABLE_NAMES),
	.count = txn_memtable_range_Range_Aged_count,
};

/* TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS is Memtable's Closes: MemtableLock and MemtableClosed. */
const TxnMemtableIndexClosesOps TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS = {
	.shape = &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape,
	.lock = TXN_MEMTABLE_RANGE_MEMTABLE_LOCK,
	.closed = TXN_MEMTABLE_RANGE_MEMTABLE_CLOSED,
};
