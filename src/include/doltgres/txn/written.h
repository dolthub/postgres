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
 * A backend's list of the keys its top-level transaction wrote to (txn's
 * written.rs), which its abort takes back and its spills' threshold reads.
 * Part of txn's contract.
 */
#ifndef DOLTGRES_TXN_WRITTEN_H
#define DOLTGRES_TXN_WRITTEN_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/owned.h"
#include "doltgres/std/vec.h"

/* TxnWrittenOf is Of: the tree a touched key is in, a storage's rows, its value map, its database's identity tree, or its repository's chunk tree. */
typedef enum TxnWrittenOf
{
	TXN_WRITTEN_OF_ROWS,
	TXN_WRITTEN_OF_VALUES,
	TXN_WRITTEN_OF_IDENTITY,
	TXN_WRITTEN_OF_CHUNKS,
} TxnWrittenOf;

/*
 * TxnWrittenWritten is Written: a backend's list of the keys its top-level
 * transaction wrote to, its top-level xid, the place where each open
 * subtransaction's keys begin, whether the list overflowed its cap, the bytes
 * each storage's tree wrote since its last spill, and the storages it
 * spilled.
 */
typedef struct TxnWrittenWritten
{
	uint64		top;
	/* touched is a Vec<Touched>, each a storage, its tree, and a key. */
	StdVec		touched;
	/* marks is a Vec<(u32, usize)>. */
	StdVec		marks;
	bool		overflowed;
	/* bytes is a Vec<(Storage, Of, u64)>. */
	StdVec		bytes;
	/* spills is a Vec<Storage>. */
	StdVec		spills;
} TxnWrittenWritten;

/* TXN_WRITTEN_WRITTEN is WRITTEN: the backend's list, one for its top-level transaction once it writes. */
extern HostOwnedTable TXN_WRITTEN_WRITTEN;

/* Written's Table, host's owned Table<Written>, whose kind's glue comes with written.c's body. */
#define HOST_OWNED_SCOPE extern
#define HOST_OWNED_DECLARE
#define HOST_OWNED_PREFIX txn_written_Written
#define HOST_OWNED_RS TxnWrittenWritten
#define HOST_OWNED_R TxnWrittenWritten
#include "doltgres/host/owned_template.h"

/* txn_TupleStorageOfVecVecU8_t is keys' (Storage, Of, Vec<Vec<u8>>): a storage's tree and the keys written to it. */
typedef struct txn_TupleStorageOfVecVecU8_t
{
	HostStorage f0;
	TxnWrittenOf f1;
	/* f2 is a Vec<Vec<u8>>. */
	StdVec		f2;
} txn_TupleStorageOfVecVecU8_t;

/* txn_TupleU64VecTupleStorageOfVecVecU8_t is keys' (u64, Vec<(Storage, Of, Vec<Vec<u8>>)>): the top-level xid and each tree's keys. */
typedef struct txn_TupleU64VecTupleStorageOfVecVecU8_t
{
	uint64		f0;
	/* f1 is a Vec<(Storage, Of, Vec<Vec<u8>>)>. */
	StdVec		f1;
} txn_TupleU64VecTupleStorageOfVecVecU8_t;

/* txn_TupleStorageOfU64U64_t is largest's (Storage, Of, u64, u64): a storage's tree, its bytes since its last spill, and the top-level xid. */
typedef struct txn_TupleStorageOfU64U64_t
{
	HostStorage f0;
	TxnWrittenOf f1;
	uint64		f2;
	uint64		f3;
} txn_TupleStorageOfU64U64_t;

extern bool txn_written_touched(HostIoIo *io, uint64 top, const HostStorage *storage, TxnWrittenOf of, const uint8 *key,
								size_t key_len, size_t version, uint64 *result, HostError *error);
extern bool txn_written_largest(HostIoIo *io, bool *some, txn_TupleStorageOfU64U64_t *result, HostError *error);
extern bool txn_written_spilled(HostIoIo *io, const HostStorage *storage, TxnWrittenOf of, HostError *error);
extern bool txn_written_keys(HostIoIo *io, bool *some, txn_TupleU64VecTupleStorageOfVecVecU8_t *result,
							 HostError *error);
extern bool txn_written_ended(HostIoIo *io, HostError *error);
extern bool txn_written_aborted(HostIoIo *io, HostError *error);
extern bool txn_written_sub_started(HostIoIo *io, uint32 sub, HostError *error);
extern bool txn_written_sub_committed(HostIoIo *io, uint32 sub, HostError *error);
extern bool txn_written_sub_aborted(HostIoIo *io, uint32 sub, HostError *error);

#endif							/* DOLTGRES_TXN_WRITTEN_H */
