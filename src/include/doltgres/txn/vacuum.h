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
 * VACUUM's and a prune's frees of a storage's tuple ids (txn's vacuum.rs):
 * the tuple ids no version holds, their frees and their redo, and a
 * take-time prune of a block.
 */
#ifndef DOLTGRES_TXN_VACUUM_H
#define DOLTGRES_TXN_VACUUM_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/record.h"

/*
 * TXN_VACUUM_LINE_POINTER is the bytes a heap line pointer takes, which
 * PageGetHeapFreeSpace keeps back for a new tuple's.
 */
#define TXN_VACUUM_LINE_POINTER UINT64CONST(4)

/*
 * TxnVacuumFreed is Freed: a tuple id never_held answers, its heap bytes,
 * its free's conflict horizon, and the value ids the free takes down, a
 * Vec<u32>.
 */
typedef struct TxnVacuumFreed
{
	uint64		f0;
	uint64		f1;
	uint64		f2;
	StdVec		f3;
} TxnVacuumFreed;

/*
 * TxnVacuumPrune is Prune: what a take-time prune reads a block's rows
 * under, the relation's NonVacuumable snapshot and a SnapshotAny.
 */
struct TxnVacuumPrune
{
	const HostSnapshot *horizon;
	const HostSnapshot *any;
};

extern bool txn_vacuum_never_held(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
								  const TxnInsertPage *page, const HostSnapshot *horizon, StdVec *result,
								  HostError *error);
extern bool txn_vacuum_frees(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							 const TxnBaseBase *base, uint64 tid, bool *result, HostError *error);
extern bool txn_vacuum_spent(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint32 block,
							 const TxnInsertPage *page, uint64 *result, HostError *error);
extern bool txn_vacuum_freed(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							 const HostSharedHeld *toast, HostStorage toast_storage, HostStorage storage, bool logged,
							 const TxnInsertPage *page, const TxnVacuumFreed *tids, size_t tids_len, bool dead,
							 StdVec *result, HostError *error);
extern bool txn_vacuum_pruned(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							  HostStorage storage, bool logged, const TxnInsertPage *page, uint32 block,
							  uint64 reserve, const TxnVacuumPrune *prune, HostError *error);
extern bool txn_vacuum_held(HostIoIo *io, HostSharedRecord record, uint64 tid, bool *result, HostError *error);
extern bool txn_vacuum_redo_freed(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
								  const HostSharedHeld *toast, const TxnRecordTidsFreed *freed,
								  const TxnInsertPage *page, uint64 lsn, HostError *error);
extern bool txn_vacuum_taken_out(HostIoIo *io, const HostSharedHeld *held, const TxnRecordTidsFreed *freed,
								 uint64 lsn, HostError *error);

#endif							/* DOLTGRES_TXN_VACUUM_H */
