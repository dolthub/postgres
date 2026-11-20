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
 * A row's slice of its key's chain (txn's memtable/slice.rs): the versions
 * that decide one row of a key, newest first. Part of txn's contract.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_SLICE_H
#define DOLTGRES_TXN_MEMTABLE_SLICE_H

#include "doltgres/host/delta.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/version.h"

/*
 * TxnMemtableSliceSlice is Slice: the versions that decide one row of a key,
 * newest first, its own at row when the delta holds it, and the key's head.
 */
typedef struct TxnMemtableSliceSlice
{
	/* versions is a Vec<Version>. */
	StdVec		versions;
	bool		has_row;
	size_t		row;
	bool		has_head;
	TxnMemtableVersionHandle head;
} TxnMemtableSliceSlice;

extern bool txn_memtable_slice_row_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state,
											const uint8 *key, size_t key_len, HostRow tid, uint64 folded,
											TxnMemtableSliceSlice *result, HostError *error);
extern void txn_memtable_slice_Slice_drop(TxnMemtableSliceSlice *self);

#endif							/* DOLTGRES_TXN_MEMTABLE_SLICE_H */
