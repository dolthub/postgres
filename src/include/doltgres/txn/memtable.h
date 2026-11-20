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
 * A storage's memtable (txn's memtable.rs): what a new version does to its
 * key, its put, and the row locks' strengths, which txn's contract gives
 * every module.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_H
#define DOLTGRES_TXN_MEMTABLE_H

#include "doltgres/host/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/vec.h"

/* TxnMemtableLockMode is LockMode: a row lock's strength, Postgres' four. */
typedef enum TxnMemtableLockMode
{
	TXN_MEMTABLE_LOCK_MODE_KEY_SHARE,
	TXN_MEMTABLE_LOCK_MODE_SHARE,
	TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE,
	TXN_MEMTABLE_LOCK_MODE_UPDATE,
} TxnMemtableLockMode;

/*
 * TxnMemtablePut is Put: a value a version puts, its bytes, the addresses it
 * holds with their locators, the value ids of the row's out-of-line values,
 * the row's tuple id, and the row's heap bytes, 0 for a map's value.
 */
typedef struct TxnMemtablePut
{
	/* value is a Vec<u8>. */
	StdVec		value;
	/* addresses is a Vec<(u32, Locator)>. */
	StdVec		addresses;
	/* values is a Vec<u32>. */
	StdVec		values;
	HostRow		tuple;
	uint32		heap_bytes;
	/* carried is the full xid of the lock the row's update carried over. */
	bool		has_carried;
	uint64		carried;
} TxnMemtablePut;

/* TxnMemtableSuccessorTag is Successor's variants. */
typedef enum TxnMemtableSuccessorTag
{
	/* Another key of the same tree. */
	TXN_MEMTABLE_SUCCESSOR_KEY,
	/* Another table, as a partitioned table's row moves to another partition. */
	TXN_MEMTABLE_SUCCESSOR_OTHER_TABLE,
} TxnMemtableSuccessorTag;

/* TxnMemtableSuccessor is Successor: where an update moved a row it deleted from its key. */
typedef struct TxnMemtableSuccessor
{
	TxnMemtableSuccessorTag tag;
	union
	{
		/* key is a Vec<u8>. */
		StdVec		key;
	}			u;
} TxnMemtableSuccessor;

/* TxnMemtableOpTag is Op's variants. */
typedef enum TxnMemtableOpTag
{
	TXN_MEMTABLE_OP_PUT,
	TXN_MEMTABLE_OP_DELETE,
	TXN_MEMTABLE_OP_EMPTIED,
	TXN_MEMTABLE_OP_NAMED,
	TXN_MEMTABLE_OP_MOVE,
	TXN_MEMTABLE_OP_ADD,
	TXN_MEMTABLE_OP_REMOVE,
	TXN_MEMTABLE_OP_LOCK,
	TXN_MEMTABLE_OP_ADD_ROW,
	TXN_MEMTABLE_OP_UPDATE_ROW,
	TXN_MEMTABLE_OP_DELETE_ROW,
	TXN_MEMTABLE_OP_LOCK_ROW,
} TxnMemtableOpTag;

/* TxnMemtableOp is Op: what a new version does to its key. */
typedef struct TxnMemtableOp
{
	TxnMemtableOpTag tag;
	union
	{
		TxnMemtablePut put;
		HostRow		emptied;
		struct
		{
			HostRow		f0;
			/* f1 is a Vec<u32>. */
			StdVec		f1;
		}			named;
		TxnMemtableSuccessor move;
		TxnMemtablePut add;
		struct
		{
			HostRow		f0;
			bool		has_f1;
			TxnMemtableSuccessor f1;
		}			remove;
		TxnMemtableLockMode lock;
		TxnMemtablePut add_row;
		struct
		{
			HostRow		f0;
			TxnMemtablePut f1;
		}			update_row;
		struct
		{
			HostRow		f0;
			bool		has_f1;
			TxnMemtableSuccessor f1;
		}			delete_row;
		struct
		{
			HostRow		f0;
			TxnMemtableLockMode f1;
		}			lock_row;
	}			u;
} TxnMemtableOp;

/* txn_TupleVecU8Op_t is the tuple (Vec<u8>, Op): a key and what its change does. */
typedef struct txn_TupleVecU8Op_t
{
	/* f0 is a Vec<u8>. */
	StdVec		f0;
	TxnMemtableOp f1;
} txn_TupleVecU8Op_t;

extern void txn_memtable_Op_drop(TxnMemtableOp *self);
extern void txn_memtable_Put_drop(TxnMemtablePut *self);
extern bool txn_memtable_Op_PartialEq_eq(const TxnMemtableOp *self, const TxnMemtableOp *other);
extern bool txn_memtable_Op_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_LockMode_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_Put_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_Put_PartialEq_eq(const TxnMemtablePut *self, const TxnMemtablePut *other);
extern void txn_memtable_Successor_drop(TxnMemtableSuccessor *self);
extern bool txn_memtable_Successor_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_Successor_PartialEq_eq(const TxnMemtableSuccessor *self, const TxnMemtableSuccessor *other);

#endif							/* DOLTGRES_TXN_MEMTABLE_H */
