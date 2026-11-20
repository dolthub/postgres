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
 * A row's write as a storage's newest (txn's insert.rs): an insert, an
 * update's new row, a speculative insert's completion, and a restart's move
 * of the tuple id counter. An update's stop value, Rust's T, is the caller's,
 * as change.h's is.
 */
#ifndef DOLTGRES_TXN_INSERT_H
#define DOLTGRES_TXN_INSERT_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/tuple.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/change.h"
#include "doltgres/txn/memtable.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/status.h"
#include "doltgres/txn/values.h"
#include "doltgres/txn/visible.h"

typedef struct TxnVacuumPrune TxnVacuumPrune;

/*
 * TxnInsertRoom is Room: what a row needs of a page, its heap bytes, the
 * bytes its table's fill factor keeps free, a page's bytes past its header,
 * and a page's most tuples.
 */
typedef struct TxnInsertRoom
{
	uint64		bytes;
	uint64		reserve;
	uint64		page;
	uint16		most;
} TxnInsertRoom;

/*
 * TxnInsertPage is Page: what redo knows of a heap page with no catalog, its
 * bytes past its header and its most tuples.
 */
typedef struct TxnInsertPage
{
	uint64		bytes;
	uint16		most;
} TxnInsertPage;

/*
 * TxnInsertInsert is Insert: a row an insert writes, its storage and table,
 * its key, its put, whether the table is keyless, whether its key is a
 * deferrable primary key's, its writer, whether it is written frozen,
 * whether its storage is logged, and whether logical decoding reads it.
 */
typedef struct TxnInsertInsert
{
	HostStorage storage;
	uint32		table;
	/* key is a Vec<u8>. */
	StdVec		key;
	TxnMemtablePut put;
	bool		keyless;
	bool		deferred;
	TxnMemtableVersionWriter writer;

	/*
	 * roomed is the slot of the writer's top-level entry in the commit status
	 * table, none for a frozen insert, whose writer has no entry.
	 */
	bool		has_roomed;
	TxnStatusRoomed roomed;
	bool		frozen;
	bool		logged;
	/* decoded is whether logical decoding reads its relation. */
	bool		decoded;
	/* toast is the TOAST relation its out-of-line values are mapped in. */
	bool		has_toast;
	TxnRecordToast toast;
	/* kept is a Vec<usize>: the put's values an update kept, ascending. */
	StdVec		kept;
	/* token is a speculative insert's token, or none for any other write. */
	bool		has_token;
	uint32		token;
	/* target is the block the backend's last insert into the relation took. */
	bool		has_target;
	uint32		target;
} TxnInsertInsert;

/*
 * TxnInsertReplaces is Replaces: the row an update replaces, its key, its
 * tuple id, the row lock its end and the new row hold, and its key's stripe.
 */
typedef struct TxnInsertReplaces
{
	/* key is a Vec<u8>. */
	StdVec		key;
	HostRow		tuple;
	TxnMemtableLockMode lock;
	uint32		stripe;
} TxnInsertReplaces;

/* TxnInsertEqualOps is Equal: whether two keys of a table are equal by their key's classes. */
typedef struct TxnInsertEqualOps
{
	bool		(*equal) (void *self, HostIoIo *io, const uint8 *a, size_t a_len, const uint8 *b, size_t b_len,
						  bool *result, HostError *error);
} TxnInsertEqualOps;

/* TxnInsertEqual is a dyn Equal: its table and its value. */
typedef struct TxnInsertEqual
{
	const TxnInsertEqualOps *ops;
	void	   *self;
} TxnInsertEqual;

/*
 * TxnInsertUniqueness is Uniqueness: what an immediate key's resolve needs,
 * the key's order, the primary key index's object id, and for a key whose
 * class compares by a function, that class's equality.
 */
typedef struct TxnInsertUniqueness
{
	const TreesKeyOrder *order;
	uint32		index;
	bool		has_equal;
	TxnInsertEqual equal;
} TxnInsertUniqueness;

/*
 * TxnInsertKeyed is Keyed: how an insert takes its key's stripe of the key
 * lock, the key's stripe, and for an immediate key its resolve's needs.
 */
typedef struct TxnInsertKeyed
{
	uint32		stripe;
	bool		has_unique;
	TxnInsertUniqueness unique;
} TxnInsertKeyed;

/*
 * TxnInsertUnique is Unique: an immediate key's read of the base before its
 * stripe, the base's row's value at the key's own bytes, the base generation
 * it read it at, the other keys equal to it, each with the base's row's
 * value at it, and the stripe's push count read first.
 */
typedef struct TxnInsertUnique
{
	/* based is a Vec<u8>, the base's row's value at the key. */
	bool		has_based;
	StdVec		based;
	uint64		generation;
	/* others is a Vec<(Vec<u8>, Option<Vec<u8>>)>. */
	StdVec		others;
	bool		has_count;
	uint64		count;
} TxnInsertUnique;

/*
 * TxnInsertProgress is Progress: a resolve's start-overs that a moved push
 * count caused since it last waited, and the hold-back flag of its stripe it
 * set after enough of them.
 */
typedef struct TxnInsertProgress
{
	uint32		starts;
	bool		has_holding;
	HostKeyHold holding;
} TxnInsertProgress;

/* TxnInsertRechecked is Rechecked: whether a resolve's read still stands under its stripe. */
typedef enum TxnInsertRechecked
{
	/* The read stands. */
	TXN_INSERT_RECHECKED_GO,
	/* The base moved since the read. */
	TXN_INSERT_RECHECKED_MOVED,
	/* The stripe's push count moved since the read. */
	TXN_INSERT_RECHECKED_RETRY,
} TxnInsertRechecked;

/* TxnInsertBase is Base: what allocation reads of the base, its free space tree, fresh end, and generation. */
typedef struct TxnInsertBase
{
	bool		has_tree;
	TreesStoreTree tree;
	uint64		fresh_end;
	uint64		generation;
} TxnInsertBase;

/*
 * TxnInsertPlaced is Placed: a known tuple id's place in its block's free
 * space map entry, read outside any window, for place_taken to take inside a
 * window: the tuple id, the row's room, the base read, the counter read, and
 * the base's entry of the block.
 */
typedef struct TxnInsertPlaced
{
	uint64		tid;
	TxnInsertRoom room;
	TxnInsertBase base;
	uint64		counter;
	bool		has_below;
	/* below is a Vec<u8>, the base's entry of the block. */
	StdVec		below;
} TxnInsertPlaced;

/* TxnInsertInsertedTag is Inserted's variants. */
typedef enum TxnInsertInsertedTag
{
	/* The tuple id the write took. */
	TXN_INSERT_INSERTED_TOOK,

	/*
	 * A live row found at an immediate key, with the tuple id the refused
	 * write spent and the live row's when a delta version wrote it.
	 */
	TXN_INSERT_INSERTED_EXISTS,
	/* An update's read stopped it, with the caller's T. */
	TXN_INSERT_INSERTED_STOPPED,
} TxnInsertInsertedTag;

/* TxnInsertInserted is Inserted<T>: what an insert or an update did, a Stopped's T in the caller's bytes. */
typedef struct TxnInsertInserted
{
	TxnInsertInsertedTag tag;
	union
	{
		/* took is the row's tuple id and the locker an update carried to it. */
		struct
		{
			uint64		tuple;
			bool		has_carried;
			uint64		carried;
		}			took;
		struct
		{
			uint64		spent;
			bool		has_live;
			uint64		live;
		}			exists;
	}			u;
} TxnInsertInserted;

/* TxnInsertInsertedRef is an Inserted<T> to format: what it did, and a Stopped's T with T's Debug. */
typedef struct TxnInsertInsertedRef
{
	const TxnInsertInserted *inserted;
	StdDebugRef stopped;
} TxnInsertInsertedRef;

/* TxnInsertCompleted is Completed: how a speculative insert completes. */
typedef enum TxnInsertCompleted
{
	/* Confirmed, its token cleared. */
	TXN_INSERT_COMPLETED_CONFIRMED,
	/* Killed, dead to every reader. */
	TXN_INSERT_COMPLETED_KILLED,
} TxnInsertCompleted;

extern bool txn_insert_Inserted_PartialEq_eq(const TxnInsertInserted *self, const void *self_stopped,
											 const TxnInsertInserted *other, const void *other_stopped,
											 bool (*eq) (const void *a, const void *b));
extern bool txn_insert_Inserted_Debug_fmt(const void *value, StdFormatter *f);
extern void txn_insert_Unique_drop(TxnInsertUnique *self);
extern TxnInsertProgress txn_insert_Progress_Default_default(void);
extern bool txn_insert_Progress_holding(const TxnInsertProgress *self);
extern void txn_insert_Progress_waiting(TxnInsertProgress *self);
extern void txn_insert_Progress_drop(TxnInsertProgress *self);
extern bool txn_insert_based(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const uint8 *key,
							 size_t key_len, const TxnInsertUniqueness *unique, uint32 stripe, TxnInsertUnique *result,
							 HostError *error);
extern bool txn_insert_resolving(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord record, const uint8 *key,
								 size_t key_len, const TxnInsertUnique *read, TxnVisibleResolved *result_0,
								 bool *has_result_1, TxnMemtableVersionHandle *result_1, HostError *error);
extern TxnInsertRechecked txn_insert_rechecked(HostIoKeyIo *io, HostSharedRecord record, const TxnInsertUnique *read,
											   uint32 stripe, TxnInsertProgress *progress);
extern bool txn_insert_waited(HostIoIo *io, HostIoWaits *waits, uint32 index, HostXid xid, bool has_tid, HostRow tid,
							  bool has_token, uint32 token, HostError *error);
extern bool txn_insert_Placed_read(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, HostRow tid,
								   uint64 heap_bytes, const TxnInsertPage *page, TxnInsertPlaced *result,
								   HostError *error);
extern TxnMemtableMapTake txn_insert_Placed_take(const TxnInsertPlaced *self, HostSharedRecord record);
extern void txn_insert_Placed_drop(TxnInsertPlaced *self);
extern bool txn_insert_place_taken(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t take,
								   HostSharedRecord record, TxnMemtableVersionWriter writer,
								   const TxnInsertPlaced *placed, HostError *error);
extern bool txn_insert_Room_fits(const TxnInsertRoom *self, uint64 free, uint16 offset);
extern uint64 txn_insert_Room_need(const TxnInsertRoom *self);
extern bool txn_insert_insert(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							  TxnInsertInsert *row, const TxnInsertRoom *room, const TxnVacuumPrune *prune,
							  const HostSharedHeld *toast, const TxnValuesEntry *toast_entries,
							  size_t toast_entries_len, TxnInsertKeyed keyed, HostIoWaits *waits,
							  TxnInsertInserted *result, HostError *error);
extern bool txn_insert_update(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							  TxnInsertInsert *row, TxnInsertReplaces *replaces, const TxnInsertRoom *room,
							  const TxnVacuumPrune *prune, const HostSharedHeld *toast,
							  const TxnValuesEntry *toast_entries, size_t toast_entries_len, TxnInsertKeyed keyed,
							  HostIoWaits *waits, TxnChangeDecide decide, void *decide_captures,
							  void (*decide_drop) (void *), void *stopped, TxnInsertInserted *result,
							  HostError *error);
extern bool txn_insert_complete(HostIoIo *io, const HostSharedHeld *held, const TxnRecordCompletion *completion,
								TxnInsertCompleted completed, bool logged, bool *result, HostError *error);
extern bool txn_insert_loaded(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							  const TxnInsertPage *page, uint64 to, HostError *error);
extern bool txn_insert_redo(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							const TxnRecordRowChange *change, uint64 lsn, const TxnInsertPage *page,
							const HostSharedHeld *toast, const TxnValuesEntry *toast_entries, size_t toast_entries_len,
							HostError *error);
extern bool txn_insert_redo_spent(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
								  const TxnRecordSpent *spent, const TxnInsertPage *page, HostError *error);
extern bool txn_insert_redo_left(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
								 const TxnRecordPageLeft *left, const TxnInsertPage *page, HostError *error);

#endif							/* DOLTGRES_TXN_INSERT_H */
