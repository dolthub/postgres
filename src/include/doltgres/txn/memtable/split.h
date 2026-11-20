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
 * A memtable range's splits (txn's memtable/split.rs): the size past which a
 * range splits.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_SPLIT_H
#define DOLTGRES_TXN_MEMTABLE_SPLIT_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"

/* TXN_MEMTABLE_SPLIT_SPLIT is SPLIT, the bytes past which a range splits. */
#define TXN_MEMTABLE_SPLIT_SPLIT (UINT64CONST(32) * 1024)

/*
 * TxnMemtableSplitSeen is Seen: what a finish saw of the range it pushed to,
 * its keys, their bytes, its pages, and the longest key pushed, which size
 * its split.
 */
typedef struct TxnMemtableSplitSeen
{
	uint64		keys;
	uint64		bytes;
	uint64		pages;
	uint64		longest;
} TxnMemtableSplitSeen;

struct TxnMemtableTableTreeOps;
struct TxnMemtableTableAt;

extern bool txn_memtable_split_split(const struct TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									 struct TxnMemtableTableAt at, const TxnMemtableSplitSeen *seen,
									 HostError *error);

#endif							/* DOLTGRES_TXN_MEMTABLE_SPLIT_H */
