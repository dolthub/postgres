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
 * A change that ends or locks a row (txn's change.rs): its read of the key's
 * versions, its push under the key's stripe, and its redo. A read's stop
 * value, Rust's T, is the caller's: the caller gives the bytes the read
 * writes it into, and the end hands them back with its Stopped.
 */
#ifndef DOLTGRES_TXN_CHANGE_H
#define DOLTGRES_TXN_CHANGE_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/txn/memtable.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/status.h"

/*
 * TxnChangeEnding is Ending: a change that ends or locks a row, its storage
 * and table, its key, what it does, its writer, the row lock its version
 * holds, whether it changes no key, whether its storage is logged, and the
 * row's tuple id when logical decoding reads its relation.
 */
typedef struct TxnChangeEnding
{
	HostStorage storage;
	uint32		table;
	/* key is a Vec<u8>. */
	StdVec		key;
	TxnMemtableOp op;
	TxnMemtableVersionWriter writer;

	/*
	 * roomed is the slot of the writer's top-level entry in the commit status
	 * table, which the change's version counts at unless it only locks.
	 */
	TxnStatusRoomed roomed;
	TxnMemtableLockMode lock;
	/* no_key is whether the change keeps every unique index's columns. */
	bool		no_key;
	bool		logged;
	bool		has_decoded;
	HostRow		decoded;
	/* tuple is the tuple id of the row the change ends or locks. */
	HostRow		tuple;
} TxnChangeEnding;

/* TxnChangeReadTag is Read's variants. */
typedef enum TxnChangeReadTag
{
	/*
	 * Go on to the push, with the command the version shows, the row's own
	 * command, and the version that holds the row.
	 */
	TXN_CHANGE_READ_GO,
	/* Stop with what the caller does instead, written to the caller's T. */
	TXN_CHANGE_READ_STOP,
} TxnChangeReadTag;

/* TxnChangeRead is Read<T>: what a change's read of its key's versions decided, a Stop's T in the caller's bytes. */
typedef struct TxnChangeRead
{
	TxnChangeReadTag tag;
	union
	{
		struct
		{
			TxnMemtableVersionShown shown;
			uint32		cmin;
			bool		has_row;
			TxnMemtableVersionHandle row;
		}			go;
	}			u;
} TxnChangeRead;

/* TxnChangeEnded is Ended<T>: whether an end pushed its version, or its read stopped it with the caller's T. */
typedef enum TxnChangeEnded
{
	TXN_CHANGE_ENDED_PUSHED,
	TXN_CHANGE_ENDED_STOPPED,
} TxnChangeEnded;

/* TxnChangeEndedRef is an Ended<T> to format: whether it pushed, and a Stopped's T with T's Debug. */
typedef struct TxnChangeEndedRef
{
	TxnChangeEnded tag;
	StdDebugRef stopped;
} TxnChangeEndedRef;

/*
 * TxnChangeDecide is a change's read of its key's versions, newest first:
 * it writes its Read, and a Stop's T into the caller's bytes it was given.
 */
typedef bool (*TxnChangeDecide) (void *captures, HostIoIo *io, TxnMemtableVersionVersion *versions,
								 size_t versions_len, TxnChangeRead *result, void *stopped, HostError *error);

extern bool txn_change_Ended_PartialEq_eq(TxnChangeEnded self, const void *self_stopped, TxnChangeEnded other,
										  const void *other_stopped, bool (*eq) (const void *a, const void *b));
extern bool txn_change_Ended_Debug_fmt(const void *value, StdFormatter *f);
extern HostNewCid txn_change_changed_cid(TxnMemtableVersionShown shown, uint32 cmin, uint32 command);
extern uint32 txn_change_stripe(const HostStorage *storage, const uint8 *key, size_t key_len, bool by_bytes);
extern bool txn_change_end(HostIoIo *io, const HostSharedHeld *held, TxnChangeEnding *row, HostIoWaits *waits,
						   uint32 stripe, TxnChangeDecide decide, void *decide_captures,
						   void (*decide_drop) (void *), void *stopped, TxnChangeEnded *result, HostError *error);
extern bool txn_change_redo(HostIoIo *io, const HostSharedHeld *held, const TxnRecordRowChange *change, uint64 lsn,
							HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_change_tests_a_keys_stripe_is_its_storage_and_bytes_or_its_storage_alone(void);
extern void test_txn_change_tests_a_delete_is_logged_pushed_and_redone_once_and_a_lock_is_never_logged(void);
extern void test_txn_change_tests_an_aborted_delete_and_lock_stay_until_a_fold(void);
extern void test_txn_change_tests_a_change_of_a_spilled_row_reads_its_spilled_version(void);
#endif

#endif							/* DOLTGRES_TXN_CHANGE_H */
