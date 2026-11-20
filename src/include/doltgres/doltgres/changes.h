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
 * What a row's delete or update decides from its read of the row's key
 * (doltgres' changes.rs), as heap's heap_delete and heap_update decide it
 * from HeapTupleSatisfiesUpdate: go on, wait, or fail with heap's TM_Result
 * and the failure data it fills.
 */
#ifndef DOLTGRES_DOLTGRES_CHANGES_H
#define DOLTGRES_DOLTGRES_CHANGES_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/txn/change.h"
#include "doltgres/txn/memtable/version.h"

/* DOLTGRES_CHANGES_TM_OK is Postgres' TM_Ok, a change that was made. */
#define DOLTGRES_CHANGES_TM_OK 0

/* DOLTGRES_CHANGES_TM_INVISIBLE is Postgres' TM_Invisible, a row the change can't see. */
#define DOLTGRES_CHANGES_TM_INVISIBLE 1

/* DOLTGRES_CHANGES_TM_SELF_MODIFIED is Postgres' TM_SelfModified, a row the caller's own transaction changed. */
#define DOLTGRES_CHANGES_TM_SELF_MODIFIED 2

/* DOLTGRES_CHANGES_TM_UPDATED is Postgres' TM_Updated, a row a committed transaction updated or moved. */
#define DOLTGRES_CHANGES_TM_UPDATED 3

/* DOLTGRES_CHANGES_TM_DELETED is Postgres' TM_Deleted, a row a committed transaction deleted. */
#define DOLTGRES_CHANGES_TM_DELETED 4

/* DOLTGRES_CHANGES_TM_BEING_MODIFIED is Postgres' TM_BeingModified, a row another running transaction holds. */
#define DOLTGRES_CHANGES_TM_BEING_MODIFIED 5

/* DOLTGRES_CHANGES_TM_WOULD_BLOCK is Postgres' TM_WouldBlock, a row SKIP LOCKED passes over. */
#define DOLTGRES_CHANGES_TM_WOULD_BLOCK 6

/*
 * DOLTGRES_CHANGES_MOVED_PARTITIONS is the tuple id
 * ItemPointerSetMovedPartitions writes for a row moved to another partition.
 */
#define DOLTGRES_CHANGES_MOVED_PARTITIONS (UINT64CONST(0xffffffff) << 16 | 0xfffd)

/*
 * DoltgresChangesFailed is Failed: a change's failure as heap's fills it, its
 * TM_Result, the tuple id of the row's successor or its own, and the ender's
 * xid and command.
 */
typedef struct DoltgresChangesFailed
{
	int32		result;
	uint64		ctid;
	uint32		xmax;
	uint32		cmax;
} DoltgresChangesFailed;

/* DoltgresChangesStopTag is Stop's variants. */
typedef enum DoltgresChangesStopTag
{
	/*
	 * A transaction to wait for, which holds the row in a mode the change's
	 * conflicts with.
	 */
	DOLTGRES_CHANGES_STOP_WAIT,
	/* Heap's failure. */
	DOLTGRES_CHANGES_STOP_FAILED,

	/*
	 * For a row lock, a lock the caller's own transaction holds as strong as
	 * the one asked for.
	 */
	DOLTGRES_CHANGES_STOP_HELD,
} DoltgresChangesStopTag;

/* DoltgresChangesStop is Stop: why a change's read stopped. */
typedef struct DoltgresChangesStop
{
	DoltgresChangesStopTag tag;
	union
	{
		HostXid		wait;
		DoltgresChangesFailed failed;
	}			u;
} DoltgresChangesStop;

/*
 * DoltgresChangesAsked is Asked: what a change asks of the row a tuple id
 * names, the tuple id, the change's command, the row lock it takes, whether
 * it only locks the row, the xid of the running update whose row it locks,
 * and the crosscheck snapshot an RI check passes, NULL for none.
 */
typedef struct DoltgresChangesAsked
{
	uint64		tuple;
	uint32		command;
	TxnMemtableLockMode mode;
	bool		locks;
	bool		has_follow;
	uint64		follow;
	const HostSnapshot *crosscheck;
} DoltgresChangesAsked;

/* DoltgresChangesBased reports whether the newest base holds the row a change asks of. */
typedef bool (*DoltgresChangesBased) (void *captures, HostIoIo *io, bool *result, HostError *error);

extern bool doltgres_changes_decided(HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len,
									 const DoltgresChangesAsked *asked, DoltgresChangesBased based,
									 void *based_captures, TxnChangeRead *result, DoltgresChangesStop *stopped,
									 HostError *error);
extern uint64 doltgres_changes_successor(TxnMemtableVersionVersion *ender, uint64 tuple);

#endif							/* DOLTGRES_DOLTGRES_CHANGES_H */
