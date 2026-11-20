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
 * The fold of a storage's delta into its base (txn's fold.rs): what a fold
 * applies, a fold now, at startup, and in redo, a base a backend builds
 * whole, a move's copies, a keyed table's re-formed rows, and the chain of a
 * storage's transforms.
 */
#ifndef DOLTGRES_TXN_FOLD_H
#define DOLTGRES_TXN_FOLD_H

#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/fmt.h"
#include "doltgres/trees/build.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/mutate.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/visible.h"

typedef struct RepoRepo RepoRepo;

/* TxnFoldAppliedTag is Applied's variants. */
typedef enum TxnFoldAppliedTag
{
	/* Applied, with its top-level xid, or none for a frozen writer's. */
	TXN_FOLD_APPLIED_YES,
	/* Left for a later fold. */
	TXN_FOLD_APPLIED_STAYS,
	/* Dropped: its writer aborted, or committed a row lock. */
	TXN_FOLD_APPLIED_DROPPED,
} TxnFoldAppliedTag;

/* TxnFoldApplied is Applied: what a fold at a fold point does with a version. */
typedef struct TxnFoldApplied
{
	TxnFoldAppliedTag tag;
	union
	{
		struct
		{
			bool		has_top;
			uint64		top;
		}			yes;
	}			u;
} TxnFoldApplied;

/*
 * txn_TupleU64U32_t is the tuple (u64, u32): a delta row's tuple id and heap
 * bytes, or a writer's top-level xid and command.
 */
typedef struct txn_TupleU64U32_t
{
	uint64		f0;
	uint32		f1;
} txn_TupleU64U32_t;

/* txn_OptionTupleU64U32_t is an Option<(u64, u32)>: an edit's formed row's writer, or none. */
typedef struct txn_OptionTupleU64U32_t
{
	bool		has;
	txn_TupleU64U32_t v;
} txn_OptionTupleU64U32_t;

/*
 * txn_TupleU64U64VecU32_t is KeyFold's pending (u64, u64, Vec<u32>): a tuple
 * id, its ender's xid, and its row's value ids.
 */
typedef struct txn_TupleU64U64VecU32_t
{
	uint64		f0;
	uint64		f1;
	StdVec		f2;
} txn_TupleU64U64VecU32_t;

/*
 * txn_TupleLocatorLocator_t is Copies' (Locator, Locator): a chunk's locator
 * it was read at and the one its copy took.
 */
typedef struct txn_TupleLocatorLocator_t
{
	ChunksTrailerLocator f0;
	ChunksTrailerLocator f1;
} txn_TupleLocatorLocator_t;

/* TxnFoldFolding is Folding: the rule of a fold at a fold point. */
typedef struct TxnFoldFolding
{
	HostLsn		f0;
} TxnFoldFolding;

/* TxnFoldCommitting is Committing: the rule of a commit's move of its storages' totals, by its own top-level xid. */
typedef struct TxnFoldCommitting
{
	uint64		own;
} TxnFoldCommitting;

/* TXN_FOLD_FOLDING_RULE_OPS is Folding's Rule. */
extern const TxnVisibleRuleOps TXN_FOLD_FOLDING_RULE_OPS;

/* TXN_FOLD_COMMITTING_RULE_OPS is Committing's Rule. */
extern const TxnVisibleRuleOps TXN_FOLD_COMMITTING_RULE_OPS;

/*
 * TxnFoldCtidEdit is CtidEdit: a fold's edit of its base's tuple id tree, a
 * tuple id with the key and the heap bytes of the row to pair it with, or
 * none to take its pair out.
 */
typedef struct TxnFoldCtidEdit
{
	uint64		f0;
	bool		has_f1;
	struct
	{
		/* f0 is a Vec<u8>. */
		StdVec		f0;
		uint32		f1;
	}			f1;
} TxnFoldCtidEdit;

/*
 * TxnFoldKeyFold is KeyFold: what a fold makes of one key, the edit of its
 * base entry, its tuple id tree's edits, the base copies it ends, the sizes
 * of the entry it replaces and of the one it writes, and the writer of the
 * delta row its new entry is formed from.
 */
typedef struct TxnFoldKeyFold
{
	TreesMutateEdit edit;
	/* ctids is a Vec<CtidEdit>. */
	StdVec		ctids;
	/* pending is a Vec<(u64, u64, Vec<u32>)>. */
	StdVec		pending;
	TxnBaseSizes removed;
	TxnBaseSizes added;
	bool		has_formed_by;
	txn_TupleU64U32_t formed_by;
} TxnFoldKeyFold;

/*
 * TxnFoldPendingRow is one of PendingRows: a tuple id a fold makes pending,
 * its ender's xid, heap bytes, and value ids.
 */
typedef struct TxnFoldPendingRow
{
	uint64		tuple;
	uint64		ender;
	uint32		bytes;
	/* ids is a Vec<u32>. */
	StdVec		ids;
} TxnFoldPendingRow;

/*
 * TxnFoldFoldedRows is FoldedRows: what a fold makes of a storage's rows,
 * its base entries' edits with each one's formed row's writer, its tuple id
 * tree's edits, the tuple ids it makes pending in tuple id order, whether it
 * applied any version, the largest top-level xid and newest ender it
 * applied, the sizes it replaces and writes, and whether it met a version it
 * neither applied nor dropped or a keyed table's key with more than one row.
 */
typedef struct TxnFoldFoldedRows
{
	/* edits is a Vec<Edit>. */
	StdVec		edits;
	/* formed is a Vec<Option<(u64, u32)>>. */
	StdVec		formed;
	/* ctids is a Vec<CtidEdit>. */
	StdVec		ctids;
	/* pending is PendingRows, a Vec<PendingRow> in tuple id order. */
	StdVec		pending;
	bool		applied;
	uint64		max_folded;
	uint64		max_ended;
	TxnBaseSizes removed;
	TxnBaseSizes added;
	bool		unresolved;
	bool		overlapping;
} TxnFoldFoldedRows;

/* TxnFoldCaught is Caught: where a read past a state's slot's record ended. */
typedef enum TxnFoldCaught
{
	TXN_FOLD_CAUGHT_NEXT,
	TXN_FOLD_CAUGHT_DONE,
	TXN_FOLD_CAUGHT_WAITING,
} TxnFoldCaught;

/*
 * TxnFoldReached is Reached: the base a standby's fold checks against, and the
 * reports of comparator roots that differ.
 */
typedef struct TxnFoldReached
{
	const TxnBaseBase *base;
	/* warned is a Vec<String>. */
	StdVec	   *warned;
} TxnFoldReached;

/*
 * TxnFoldTrees is Trees: the entries of a base a backend builds whole, its
 * table tree's in any order, and its tuple id tree's, its free space map's,
 * and its pending tuple ids' tree's, each in its keys' byte order, each a
 * Vec<trees::Entry>.
 */
typedef struct TxnFoldTrees
{
	StdVec		rows;
	StdVec		tuple_ids;
	StdVec		free_space;
	StdVec		pending;
} TxnFoldTrees;

/*
 * TxnFoldCopies is Copies: the chunks a move copied, each by the locator it
 * was read at with the locator it took, a Vec<(Locator, Locator)> kept in
 * ascending order of the first, as Rust's BTreeMap iterates it.
 */
typedef struct TxnFoldCopies
{
	StdVec		copies;
} TxnFoldCopies;

/*
 * TxnFoldReformed is Reformed: a row re-formed to its chain's end, its tuple,
 * its addresses' offsets with their locators, its value ids, and its heap
 * bytes.
 */
typedef struct TxnFoldReformed
{
	/* tuple is a Vec<u8>. */
	StdVec		tuple;
	/* addresses is a Vec<(u32, Locator)>. */
	StdVec		addresses;
	/* values is a Vec<u32>. */
	StdVec		values;
	uint32		heap_bytes;
} TxnFoldReformed;

/*
 * TxnFoldReformOps is Reform: the re-form of a keyed table's rows stored
 * before its pending transforms to the layout at the chain's end.
 */
typedef struct TxnFoldReformOps
{
	bool		(*reformed) (void *self, HostIoIo *io, const uint8 *key, size_t key_len, ChunksTrailerStamp stamp,
							 const uint8 *tuple, size_t tuple_len, const chunks_TupleU32Locator_t *addresses,
							 size_t addresses_len, const uint32 *values, size_t values_len,
							 const TxnVisibleLive *transforms, size_t transforms_len, TxnFoldReformed *result,
							 HostError *error);
} TxnFoldReformOps;

/* TxnFoldReform is a dyn Reform: its table and its value. */
typedef struct TxnFoldReform
{
	const TxnFoldReformOps *ops;
	void	   *self;
} TxnFoldReform;

extern void txn_fold_CtidEdit_drop(void *edit_);
extern void txn_fold_KeyFold_drop(TxnFoldKeyFold *self);
extern bool txn_fold_folded_key(HostIoIo *io, TreesStoreNodeStore store, bool has_base, TreesStoreTree base,
								const TreesKeyOrder *order, bool keyless, bool index, TxnVisibleRule rule,
								const uint8 *key, size_t key_len, TxnMemtableVersionVersion *versions,
								size_t versions_len, bool *has_result, TxnFoldKeyFold *result, uint64 *max_ended,
								HostError *error);
extern void txn_fold_FoldedRows_drop(TxnFoldFoldedRows *self);
extern bool txn_fold_folded_rows(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, bool has_base,
								 TreesStoreTree base, bool has_tuple_ids, TreesStoreTree tuple_ids,
								 const TreesKeyOrder *order, bool keyless, bool index, HostLsn point, HostLsn folded,
								 TxnFoldFoldedRows *result, HostError *error);
extern bool txn_fold_folded(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
							const HostSharedHeld *held, const TreesKeyOrder *order, bool keyless, HostLsn point,
							TxnFoldReform *reform, TxnFoldReached *reached, bool *result, HostError *error);
extern bool txn_fold_caught_up(HostIoIo *io, const HostSharedHeld *held, const TxnInsertPage *page, uint64 upto,
							   TxnFoldCaught *result, HostError *error);
extern bool txn_fold_Applied_PartialEq_eq(const TxnFoldApplied *self, const TxnFoldApplied *other);
extern bool txn_fold_Applied_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_fold_applied(HostIoIo *io, TxnMemtableVersionVersion *version, HostLsn point,
							 TxnFoldApplied *result, HostError *error);
extern bool txn_fold_applied_by(HostIoIo *io, TxnMemtableVersionWriter writer, bool has_lsn, uint64 lsn,
								HostStatus status, HostLsn point, TxnFoldApplied *result, HostError *error);
extern bool txn_fold_built(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
						   const HostSharedHeld *held, const TreesKeyOrder *order, TxnFoldTrees *trees,
						   TxnBaseBase *base, HostLsn point, bool has_nodes, uint64 nodes, uint64 horizon,
						   bool *result, HostError *error);
extern bool txn_fold_chained(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
							 const HostSharedHeld *held, TxnBasePending *pending, HostError *error);
extern bool txn_fold_committed_moves(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
									 bool has_base, TreesStoreTree base, bool has_tuple_ids, TreesStoreTree tuple_ids,
									 const TreesKeyOrder *order, bool keyless, bool index, uint64 own,
									 const uint8 *key, size_t key_len, TxnBaseSizes *result_0,
									 TxnBaseSizes *result_1, HostError *error);
extern bool txn_fold_committed_value_moves(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
										   bool has_root, TreesStoreTree root, uint64 own, const uint8 *key,
										   size_t key_len, TxnBaseSizes *result_0, TxnBaseSizes *result_1,
										   HostError *error);
extern bool txn_fold_copied(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
							const HostSharedHeld *held, TxnBaseBase *base, HostLsn point, bool has_nodes,
							uint64 nodes, TxnFoldCopies *memo, bool *result, HostError *error);
extern void txn_fold_Copies_drop(TxnFoldCopies *self);
extern bool txn_fold_fold_now(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
							  const HostSharedHeld *held, const TreesKeyOrder *order, bool keyless, bool has_point,
							  HostLsn point, uint64 horizon, StdVec *warned, const TxnInsertPage *page,
							  TxnFoldReform *reform, bool *result, HostError *error);
extern bool txn_fold_kept_out(HostIoIo *io, uint64 top, uint64 horizon, HostLsn *bound, HostError *error);
extern bool txn_fold_point(HostIoIo *io, bool *has_result, HostLsn *result, HostError *error);
extern bool txn_fold_recount(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
							 const TreesKeyOrder *order, bool keyless, bool *has_result, TxnBaseSizes *result,
							 HostError *error);
extern bool txn_fold_redo(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
						  const HostSharedHeld *held, const TxnRecordBaseAdvanced *advanced, uint64 end, bool *result,
						  HostError *error);
extern bool txn_fold_redo_chained(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
								  const HostSharedHeld *held, const TxnBasePending *entries, size_t entries_len,
								  TxnBaseBase *recorded, HostError *error);
extern bool txn_fold_settled(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
							 const HostSharedHeld *held, StdVec *result, HostError *error);
extern bool txn_fold_startup_fold(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
								  const HostSharedHeld *held, const TxnInsertPage *page, uint64 upto, bool *result,
								  HostError *error);

#endif							/* DOLTGRES_TXN_FOLD_H */
