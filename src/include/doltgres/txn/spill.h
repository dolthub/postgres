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
 * A transaction's spills of its versions (txn's spill.rs): the records whose
 * spills a manifest lists, the files it lists, each process's pending spill,
 * the spills' threshold, and the reads, spills, and settles of a storage's
 * spilled versions. Part of txn's contract.
 */
#ifndef DOLTGRES_TXN_SPILL_H
#define DOLTGRES_TXN_SPILL_H

#include "doltgres/host/delta.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/runs.h"
#include "doltgres/txn/written.h"

/*
 * TxnSpillHomeOps is the Home trait: a record whose spills a manifest lists.
 * The other half only passes its instances through, so its definition comes
 * with spill.c's body.
 */
typedef struct TxnSpillHomeOps TxnSpillHomeOps;

/* TXN_SPILL_STATE_HOME_OPS is State's Home: a storage's state, whose rows and value map spill. */
extern const TxnSpillHomeOps TXN_SPILL_STATE_HOME_OPS;

/* TXN_SPILL_BRANCH_DELTAS_HOME_OPS is BranchDeltas' Home: a working state's delta, whose chunk tree spills. */
extern const TxnSpillHomeOps TXN_SPILL_BRANCH_DELTAS_HOME_OPS;

/* TxnSpillFilter is Filter: a spill's Bloom filter while it is built, in the backend's memory, its words of bits. */
typedef struct TxnSpillFilter
{
	/* words is a Vec<u64>. */
	StdVec		words;
} TxnSpillFilter;

/*
 * TxnSpillFile is File: what names a spill's file, its spilling transaction's
 * top-level xid, its kind, its number among that transaction's files of its
 * storage and tree, its length, its trailer's xxh3, and the count of its
 * filter's bits.
 */
typedef struct TxnSpillFile
{
	uint64		xid;
	TxnRunsKind kind;
	uint32		number;
	uint64		len;
	uint64		hash;
	uint64		bits;
} TxnSpillFile;

/* txn_TupleVecFileU64_t is listed's (Vec<File>, u64): a storage's spill files, newest first, and the generation they were read at. */
typedef struct txn_TupleVecFileU64_t
{
	/* f0 is a Vec<File>. */
	StdVec		f0;
	uint64		f1;
} txn_TupleVecFileU64_t;

/* TXN_SPILL_MANIFEST is MANIFEST, the start of the name of a storage's spills' manifest file. */
#define TXN_SPILL_MANIFEST "spills-"

/* txn_TupleU64KindU32OptionU32_t is Slotted, (u64, Kind, u32, Option<u32>): a spill a process's slot names, its xid, kind, and number, and, for a merge, the lowest number of the files it replaces. */
typedef struct txn_TupleU64KindU32OptionU32_t
{
	uint64		f0;
	TxnRunsKind f1;
	uint32		f2;
	bool		has_f3;
	uint32		f3;
} txn_TupleU64KindU32OptionU32_t;

/* TXN_SPILL_PENDING_PROC_SLOTS_OPS is Pending's ProcSlots: each process's slot naming its one spill no manifest lists yet. */
extern const HostSharedProcSlotsOps TXN_SPILL_PENDING_PROC_SLOTS_OPS;

extern bool txn_spill_listed(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record,
							 const HostStorage *storage, txn_TupleVecFileU64_t *result, HostError *error);
extern uint64 txn_spill_threshold(void);
extern bool txn_spill_relieved(HostIoIo *io, bool *result, HostError *error);
extern bool txn_spill_relieved_own(HostIoIo *io, bool *result, HostError *error);
extern bool txn_spill_replayed(HostIoIo *io, const HostStorage *storage, TxnWrittenOf of, uint64 xid, size_t bytes,
							   uint64 *result, HostError *error);
extern bool txn_spill_spilled(HostIoIo *io, const HostStorage *storage, TxnWrittenOf of, uint64 xid, bool *result,
							  HostError *error);
extern bool txn_spill_folded(HostIoIo *io, HostSharedRecord state, HostLsn point, HostError *error);
extern bool txn_spill_versions(const TxnSpillHomeOps *home, HostIoIo *io, const HostDeltaHold *hold,
							   HostSharedRecord state, TxnRunsKind kind, const uint8 *key, size_t key_len,
							   StdVec *result, HostError *error);
extern bool txn_spill_next(const TxnSpillHomeOps *home, HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state,
						   TxnRunsKind kind, bool has_after, const uint8 *after, size_t after_len,
						   TxnMemtableTableDirection way, bool *some, txn_TupleVecU8VecVersion_t *result,
						   HostError *error);
extern bool txn_spill_holds(HostIoIo *io, HostSharedRecord state, TxnRunsKind kind, const uint8 *key, size_t key_len,
							uint64 lsn, bool *result, HostError *error);
extern uint64 txn_spill_generation(const TxnSpillHomeOps *home, HostSharedRecord state);
extern bool txn_spill_newest_value(HostIoIo *io, HostSharedRecord state, uint32 id, bool *some,
								   TxnMemtableMapEntry *result, HostError *error);
extern bool txn_spill_settle_pending(HostIoIo *io, HostError *error);
extern bool txn_spill_wrote(HostIoIo *io, uint64 top, uint64 bytes, HostError *error);
extern bool txn_spill_unwrote(HostIoIo *io, bool has_bytes, uint64 bytes, HostError *error);
extern bool txn_spill_aborted(HostIoIo *io, uint64 xid, const HostStorage *storages, size_t storages_len,
							  HostError *error);
extern bool txn_spill_chunks_repository(const HostStorage *storage, uint32 *result);
extern bool txn_spill_Filter_of(size_t count, const StdBytes *keys, size_t keys_len, TxnSpillFilter *result,
								HostError *error);
extern bool txn_spill_Filter_hits(const TxnSpillFilter *self, const uint8 *key, size_t key_len);
extern HostStorage txn_spill_chunks_storage(uint32 repository);
extern bool txn_spill_chunk_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord deltas, const uint8 *key,
									 size_t key_len, StdVec *result, HostError *error);
extern bool txn_spill_chunk(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len, bool *some,
							StdVec *result, HostError *error);
extern bool txn_spill_linked(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord record,
							 const HostStorage *storage, const TxnSpillFile *linking, size_t linking_len,
							 bool has_replacing, txn_TupleU64KindU32OptionU32_t replacing, HostError *error);
extern bool txn_spill_twinned(HostIoIo *io, HostSharedRecord state, TxnRunsKind kind, const uint8 *key,
							  size_t key_len, const uint8 *bytes, size_t bytes_len, bool *result, HostError *error);
extern uint64 txn_spill_counted(const TxnRunsDecoded *decoded);
extern bool txn_spill_chunks_ended(HostIoIo *io, HostSharedRecord deltas, HostError *error);
extern bool txn_spill_named(const TxnSpillHomeOps *home, HostIoIo *io, HostSharedRecord state,
							const HostStorage *storage, StdVec *result, HostError *error);
extern bool txn_spill_tuple_key(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, HostRow tid,
								bool *some, StdVec *result, HostError *error);

#endif							/* DOLTGRES_TXN_SPILL_H */
