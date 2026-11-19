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
 * A storage's memtables (txn's memtable/table.rs): the trees whose versions
 * a memtable holds and a prepared push. Part of txn's contract.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_TABLE_H
#define DOLTGRES_TXN_MEMTABLE_TABLE_H

#include "doltgres/host/delta.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/vec.h"
#include "doltgres/host/delta/list.h"
#include "doltgres/std/tuple.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/split.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/status.h"

/*
 * TxnMemtableTableOwnerOps is the Owner trait's steps that differ by owner:
 * grew and shrank count the bytes a finish pushed into one of the owner's
 * trees and a removal took out; its other steps are a tree's own, so a
 * prepare's aim names its tree for them.
 */
typedef struct TxnMemtableTableOwnerOps
{
	void		(*grew) (HostSharedRecord owner, uint64 bytes);
	void		(*shrank) (HostSharedRecord owner, uint64 bytes);
} TxnMemtableTableOwnerOps;

/*
 * TxnMemtableTableTreeOps is the Tree trait: a kind of memtable, its owner,
 * the lock its owner's word is stored under, the word, the word of each
 * process's slot that pins it, whether its versions count at their writer's
 * entry, and what a hand-off that finds it with no memtable does to its
 * owner, nothing when NULL.
 */
typedef struct TxnMemtableTableTreeOps
{
	const TxnMemtableTableOwnerOps *of;
	size_t		lock;
	size_t		word;
	size_t		pin;
	bool		counted;
	void		(*bare) (HostSharedRecord owner);
} TxnMemtableTableTreeOps;

/* TxnMemtableTableAt is At: where the hand-off found a key, its memtable's location and its range's. */
typedef struct TxnMemtableTableAt
{
	uint64		memtable;
	uint64		range;
} TxnMemtableTableAt;

/*
 * TxnMemtableTableReservation is tuples.rs's Reservation: a slot of a tuple
 * id table a prepare reserved, with the place of the key's bytes it
 * appended; here, since tuples.h includes this header and a prepare holds
 * one.
 */
typedef struct TxnMemtableTableReservation
{
	uint64		place;
} TxnMemtableTableReservation;

/*
 * TxnMemtableTableReserve is Reserve: what a prepared push takes before its
 * window, so that its finish allocates nothing: a fresh page that holds its
 * version and a full-height node on any range's page, the most spares any
 * list's push wants, a buffer for its version's record and one for a take's
 * value, what its finish saw of the range it pushed to, and the bytes of the
 * versions pushed with it that within hasn't yet counted at the owner.
 */
typedef struct TxnMemtableTableReserve
{
	bool		has_fresh;
	HostDeltaOwned fresh;
	/* spare is a Vec<Owned>. */
	StdVec		spare;
	/* buffer is a Vec<u8>. */
	StdVec		buffer;
	/* value is a Vec<u8>, the buffer a take's step writes its value into. */
	StdVec		value;
	bool		has_seen;
	TxnMemtableSplitSeen seen;
	uint64		grown;
} TxnMemtableTableReserve;

/* TxnMemtableTableTarget is Target: one push a prepare readied, its tree, where its hand-off found its key, and its reserve. */
typedef struct TxnMemtableTableTarget
{
	const TxnMemtableTableTreeOps *tree;
	TxnMemtableTableAt at;
	TxnMemtableTableReserve reserve;
} TxnMemtableTableTarget;

/*
 * TxnMemtableTableAim is Aim: a push a prepare readies, the record whose
 * word names its tree, its tree, its key, the longest record its version
 * takes, and the longest value a take writes.
 */
typedef struct TxnMemtableTableAim
{
	HostSharedRecord owner;
	const TxnMemtableTableTreeOps *tree;
	StdBytes	key;
	size_t		need;
	size_t		most;
} TxnMemtableTableAim;

/* TxnMemtableTableRetiring is an iterator over the names of pages a retirement gives back. */
typedef struct TxnMemtableTableRetiring
{
	const uint64 *names;
	size_t		len;
	size_t		at;
} TxnMemtableTableRetiring;

/*
 * TxnMemtableTablePreparedState is Prepared, at either owner: a prepare's
 * hold, kept until its settle, so every open of a finish is under a hold
 * begun before the window, its aims, its targets in the aims' order, and the
 * slot of the tuple id table it reserved.
 */
typedef struct TxnMemtableTablePreparedState
{
	HostDeltaHold hold;
	const TxnMemtableTableAim *aims;
	size_t		aims_len;
	/* targets is a Vec<Target>. */
	StdVec		targets;
	bool		has_tuple;
	TxnMemtableTableReservation tuple;
} TxnMemtableTablePreparedState;

/* TxnMemtableTableCompactable is Compactable: a page a range names alone that the remover found worth compacting, its index in the range's list, its name, and its copy's size. */
typedef struct TxnMemtableTableCompactable
{
	uint64		entry;
	uint64		name;
	HostDeltaPageSize size;
} TxnMemtableTableCompactable;

/* TxnMemtableTableRemoving is one key removed_all takes versions out of, with the handles of the versions. */
typedef struct TxnMemtableTableRemoving
{
	StdBytes	key;
	const TxnMemtableVersionHandle *handles;
	size_t		handles_len;
} TxnMemtableTableRemoving;

/* txn_TupleHandleU64_t is a version's handle with its writer's top-level xid, (Handle, u64), as copied hands them out. */
typedef struct txn_TupleHandleU64_t
{
	TxnMemtableVersionHandle f0;
	uint64		f1;
} txn_TupleHandleU64_t;

/* TXN_MEMTABLE_TABLE_STATE_OWNER_OPS is State's Owner: a storage's state. */
extern const TxnMemtableTableOwnerOps TXN_MEMTABLE_TABLE_STATE_OWNER_OPS;


/* TXN_MEMTABLE_TABLE_ROWS_TREE_OPS is Rows' Tree: a storage's rows, of its State. */
extern const TxnMemtableTableTreeOps TXN_MEMTABLE_TABLE_ROWS_TREE_OPS;

/* TxnMemtableTableDirection is Direction: the way a walk of a tree goes in key order. */
typedef enum TxnMemtableTableDirection
{
	TXN_MEMTABLE_TABLE_DIRECTION_FORWARD,
	TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD,
} TxnMemtableTableDirection;

/* txn_TupleVecU8VecVersion_t is Versions: a key and its versions, newest first, lent for a hold. */
typedef struct txn_TupleVecU8VecVersion_t
{
	/* f0 is the key, a Vec<u8>. */
	StdVec		f0;
	/* f1 is the versions, a Vec<Version>. */
	StdVec		f1;
} txn_TupleVecU8VecVersion_t;

/* txn_TupleRefSliceU8Usize_t is prepare's (&[u8], usize): a row's key and the longest record its version takes. */
typedef struct txn_TupleRefSliceU8Usize_t
{
	StdBytes	f0;
	size_t		f1;
} txn_TupleRefSliceU8Usize_t;

extern const HostDeltaHold *txn_memtable_table_PreparedState_hold(const TxnMemtableTablePreparedState *self);
extern bool txn_memtable_table_finish(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared,
									  const TxnMemtableVersionChange *change, bool has_roomed,
									  TxnStatusRoomed roomed, TxnMemtableVersionHandle *result, HostError *error);
extern bool txn_memtable_table_finish_speculative(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared,
												  const TxnMemtableVersionChange *change, uint32 token,
												  bool has_roomed, TxnStatusRoomed roomed,
												  TxnMemtableVersionHandle *result, HostError *error);
extern bool txn_memtable_table_finish_showing(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t row,
											  const TxnMemtableVersionChange *change, TxnMemtableVersionHandle ended,
											  uint32 combo, bool has_roomed, TxnStatusRoomed roomed,
											  TxnMemtableVersionHandle *result, HostError *error);
extern bool txn_memtable_table_finish_row(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t row,
										  const TxnMemtableVersionChange *change, bool has_roomed,
										  TxnStatusRoomed roomed, TxnMemtableVersionHandle *result,
										  HostError *error);
extern bool txn_memtable_table_counted(HostIoTakesShort io, const TxnMemtableTablePreparedState *prepared,
									   bool has_roomed, TxnStatusRoomed roomed, HostError *error);
extern bool txn_memtable_table_newest(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t row,
									  bool *some, TxnMemtableVersionHandle *result, HostError *error);
extern bool txn_memtable_table_holds(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len,
									 uint64 lsn, bool *result, HostError *error);
extern bool txn_memtable_table_push(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len,
									const TxnMemtableVersionChange *change, TxnMemtableVersionHandle *result,
									HostError *error);
extern bool txn_memtable_table_alone_State(HostIoIo *io, HostSharedRecord owner, bool has_which,
										   TxnMemtableMapMap which, const uint8 *key, size_t key_len,
										   const TxnMemtableVersionChange *change, TxnMemtableVersionHandle *result,
										   HostError *error);
extern bool txn_memtable_table_removed(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									   const uint8 *key, size_t key_len, const TxnMemtableVersionHandle *handles,
									   size_t handles_len, StdVec *result, HostError *error);
extern bool txn_memtable_table_pruned(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									  const uint8 *key, size_t key_len, bool has_aborting, uint64 aborting,
									  HostError *error);
extern bool txn_memtable_table_uncounted(HostIoIo *io, const TxnMemtableVersionVersion *version, HostError *error);
extern bool txn_memtable_table_waited(HostIoIo *io, HostDeltaFull full, bool spilling, HostError *error);
extern bool txn_memtable_table_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state,
										const uint8 *key, size_t key_len, StdVec *result, HostError *error);
extern bool txn_memtable_table_walked(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									  bool (*each) (void *captures, HostIoIo *io, const uint8 *key, size_t key_len,
													const StdVec *values, size_t values_len, HostError *error),
									  void *each_captures, void (*each_drop) (void *captures), HostError *error);
extern bool txn_memtable_table_walked_killed(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
											 bool (*each) (void *captures, HostIoIo *io, const uint8 *key,
														   size_t key_len, const StdVec *values, size_t values_len,
														   HostError *error),
											 void *each_captures, void (*each_drop) (void *captures),
											 HostError *error);
extern bool txn_memtable_table_next(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, bool has_after,
									const uint8 *after, size_t after_len, TxnMemtableTableDirection way, bool *some,
									txn_TupleVecU8VecVersion_t *result, HostError *error);
extern bool txn_memtable_table_range_starts(HostIoIo *io, HostSharedRecord state, StdVec *result, HostError *error);
extern bool txn_memtable_table_first_key(const HostDeltaHold *hold, uint64 range, HostDeltaLent *result,
										 HostError *error);
extern bool txn_memtable_table_search(const HostDeltaListListView *view, const HostDeltaHold *hold, const uint8 *key,
									  size_t key_len, std_TupleU64U64_t *result, HostError *error);
extern bool txn_memtable_table_found(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
									 HostSharedRecord owner, const uint8 *key, size_t key_len,
									 const HostSharedMine *pin, bool *some, TxnMemtableTableAt *result,
									 HostError *error);
extern bool txn_memtable_table_handed(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
									  const HostSharedMine *mine, HostSharedRecord owner, const uint8 *key,
									  size_t key_len, bool *some, TxnMemtableTableAt *result, HostError *error);
extern bool txn_memtable_table_Reserve_push(TxnMemtableTableReserve *self, TxnMemtableIndexKeyedOpen *open,
											const HostDeltaHold *hold, const uint8 *key, size_t key_len,
											const TxnMemtableVersionChange *change,
											TxnMemtableVersionHandle *result, HostError *error);
extern size_t txn_memtable_table_Prepared_target(const TxnMemtableTablePreparedState *self, size_t take);
extern bool txn_memtable_table_Prepared_aimed(const TxnMemtableTablePreparedState *self, size_t target,
											  HostSharedRecord *result_0, const TxnMemtableTableTreeOps **result_1,
											  HostError *error);
extern bool txn_memtable_table_Prepared_tuple(TxnMemtableTablePreparedState *self, const HostDeltaHold **result_0,
											  HostSharedRecord *result_1, TxnMemtableTableReservation *result_2,
											  HostError *error);
extern bool txn_memtable_table_prepared(HostIoIo *io, const TxnMemtableTableAim *aims, size_t aims_len,
										bool has_tuple, TxnMemtableTableReservation tuple,
										bool (*inside) (void *captures, HostIoIo *io,
														TxnMemtableTablePreparedState *prepared, HostError *error),
										void *inside_captures, void (*inside_drop) (void *captures),
										bool *has_unstored, TxnMemtableTableReservation *unstored,
										HostError *error);
extern bool txn_memtable_table_within(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t index,
									  bool (*step) (void *captures, TxnMemtableIndexKeyedOpen *open,
													const HostDeltaHold *hold, const uint8 *key, size_t key_len,
													TxnMemtableTableReserve *reserve, HostError *error),
									  void *captures, HostError *error);
extern bool txn_memtable_table_settled(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									   TxnMemtableTableAt at, bool has_seen, const TxnMemtableSplitSeen *seen,
									   HostError *error);
extern bool txn_memtable_table_first(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									 HostError *error);
extern bool txn_memtable_table_memtable(const TxnMemtableTableTreeOps *tree, HostSharedRecord owner, uint64 *result);
extern bool txn_memtable_table_abandoned(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
										 uint64 location, bool *result, HostError *error);
extern bool txn_memtable_table_retire_all(HostIoIo *io, HostSharedRecord state, HostError *error);

extern bool txn_memtable_table_readied(HostIoIo *io, bool has_rows, HostSharedRecord state,
									   const txn_TupleRefSliceU8Usize_t *rows, size_t rows_len,
									   const TxnMemtableMapTake *takes, size_t takes_len,
									   bool tuple, bool (*inside) (void *captures, HostIoIo *io,
																   TxnMemtableTablePreparedState *prepared,
																   HostError *error), void *inside_captures,
									   void (*inside_drop) (void *captures),
									   HostError *error);
extern bool txn_memtable_table_pushed(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t index,
									  const TxnMemtableVersionChange *change, TxnMemtableVersionHandle *result,
									  HostError *error);
extern bool txn_memtable_table_finished(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t index,
										const TxnMemtableVersionChange *change, bool has_roomed, TxnStatusRoomed roomed,
										TxnMemtableVersionHandle *result,
										HostError *error);
extern bool txn_memtable_table_lent(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
									HostSharedRecord owner, const uint8 *key, size_t key_len,
									bool (*reading) (void *captures,
													 const HostDeltaHold *hold, TxnMemtableTableAt at,
													 const HostSharedGuard *guard, HostError *error),
									void *captures, bool *some, HostError *error);
extern bool txn_memtable_table_read(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									const uint8 *key, size_t key_len, bool (*reading) (void *captures,
																					   const HostDeltaHold *hold,
																					   TxnMemtableTableAt at,
																					   const HostSharedGuard *guard,
																					   HostError *error),
									void *captures, bool *some,
									HostError *error);
extern bool txn_memtable_table_held(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									const uint8 *key, size_t key_len, uint64 lsn, bool *result, HostError *error);
extern bool txn_memtable_table_alone(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									 const uint8 *key, size_t key_len, const TxnMemtableVersionChange *change,
									 TxnMemtableVersionHandle *result,
									 HostError *error);
extern bool txn_memtable_table_removed_all(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
										   const TxnMemtableTableRemoving *keys, size_t keys_len, StdVec *result,
										   HostError *error);
extern bool txn_memtable_table_chain(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
									 HostSharedRecord owner, const uint8 *key, size_t key_len, StdVec *result,
									 HostError *error);
extern bool txn_memtable_table_copied(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									  bool killed, bool (*each) (void *captures, HostIoIo *io, const uint8 *key,
																 size_t key_len, const StdVec *records,
																 size_t records_len,
																 const txn_TupleHandleU64_t *handles,
																 size_t handles_len, HostError *error),
									  void *captures,
									  HostError *error);
extern bool txn_memtable_table_stepped(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
									   HostSharedRecord owner, bool has_place, const uint8 *place, size_t place_len,
									   TxnMemtableTableDirection way,
									   bool (*reading) (void *captures, const HostDeltaHold *hold,
														const HostSharedGuard *guard, bool has_stop,
														const uint8 *stop, size_t stop_len, HostError *error),
									   void *captures, bool *some, bool *has_stop,
									   StdVec *stop, HostError *error);
extern bool txn_memtable_table_unlist(const TxnMemtableTableTreeOps *tree, TxnMemtableIndexOpen open,
									  const HostDeltaHold *hold, uint64 entry, uint64 memtable, bool *result,
									  HostError *error);
extern bool txn_memtable_table_retired(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
									   HostError *error);

/* TXN_MEMTABLE_TABLE_RETIRING_ITERATOR_OPS is the retiring names' Iterator, whose drop does nothing. */
extern const StdIteratorOps TXN_MEMTABLE_TABLE_RETIRING_ITERATOR_OPS;

extern bool txn_memtable_table_filled(const HostDeltaMade *made, const HostSharedShapeOps *shape, size_t list,
									  uint64 entry, HostError *error);
extern bool txn_memtable_table_unlisted(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
										HostSharedRecord owner, TxnMemtableTableAt at, HostError *error);
extern bool txn_memtable_table_closed_names(StdVec *names, const HostDeltaListClosed *closed,
											const HostDeltaHold *hold, HostError *error);

#endif							/* DOLTGRES_TXN_MEMTABLE_TABLE_H */
