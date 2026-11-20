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
 * A table rewrite's and a move's new storage (txn's rewrite.rs): the old
 * base's entries, the keys of the old storage, the new base, the row
 * versions and map versions a rewrite or a move carries, and a new TOAST
 * storage's value map.
 */
#ifndef DOLTGRES_TXN_REWRITE_H
#define DOLTGRES_TXN_REWRITE_H

#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/fold.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/memtable.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/values.h"

/*
 * TxnRewriteBased is Based: a base's entry, its key, its value, its
 * out-of-line addresses, its value ids, and its copies' stamps.
 */
typedef struct TxnRewriteBased
{
	/* key is a Vec<u8>. */
	StdVec		key;
	/* value is a Vec<u8>. */
	StdVec		value;
	/* addresses is a Vec<(u32, Locator)>. */
	StdVec		addresses;
	/* values is a Vec<u32>. */
	StdVec		values;
	/* stamps is a Vec<Stamp>. */
	StdVec		stamps;
} TxnRewriteBased;

/* TxnRewriteStoredTag is Stored's variants. */
typedef enum TxnRewriteStoredTag
{
	/* A base entry's value, addresses, and value ids, by the entry's place. */
	TXN_REWRITE_STORED_BASED,
	/* A row re-formed to the table's current columns. */
	TXN_REWRITE_STORED_FORMED,
} TxnRewriteStoredTag;

/* TxnRewriteStored is Stored: what a new table tree's entry holds of its row. */
typedef struct TxnRewriteStored
{
	TxnRewriteStoredTag tag;
	union
	{
		size_t		based;
		struct
		{
			/* tuple is a Vec<u8>. */
			StdVec		tuple;
			/* addresses is a Vec<(u32, Locator)>. */
			StdVec		addresses;
			/* values is a Vec<u32>. */
			StdVec		values;
		}			formed;
	}			u;
} TxnRewriteStored;

/* txn_TupleU64U64U32_t is a copy of a rewrite's entry: its writer's xid, its new tuple id, and its heap bytes. */
typedef struct txn_TupleU64U64U32_t
{
	uint64		f0;
	uint64		f1;
	uint32		f2;
} txn_TupleU64U64U32_t;

/*
 * TxnRewriteCopied is Copied: an entry of a rewrite's new table tree, its
 * key, what it holds of its row, and its copies, a Vec<(u64, u64, u32)> of
 * each copy's writer's xid, its new tuple id, and its heap bytes.
 */
typedef struct TxnRewriteCopied
{
	StdVec		f0;
	TxnRewriteStored f1;
	StdVec		f2;
} TxnRewriteCopied;

/*
 * TxnRewritePacked is Packed: where a rewrite's packing of new tuple ids
 * ended, the tuple id past the last it gave, the free bytes of that tuple
 * id's page, and a page's most tuples.
 */
typedef struct TxnRewritePacked
{
	uint64		f0;
	uint64		f1;
	uint16		f2;
} TxnRewritePacked;

/*
 * TxnRewriteKeysEach is keys' each: a key, the place of its entry among the
 * base's or none, and its versions newest first.
 */
typedef bool (*TxnRewriteKeysEach) (void *captures, HostIoIo *io, const uint8 *key, size_t key_len,
									bool has_place, size_t place, StdVec *versions, HostError *error);

/*
 * txn_TupleEntryU64_t is the tuple (values::Entry, u64): an old value map's
 * entry and the count of the kept versions naming it.
 */
typedef struct txn_TupleEntryU64_t
{
	TxnValuesEntry f0;
	uint64		f1;
} txn_TupleEntryU64_t;

/*
 * txn_TupleU32TupleEntryU64_t is one entry of value_map's BTreeMap<u32,
 * (Entry, u64)>: a value id and its entry and count; value_map takes them
 * in ascending order of id, each id once, as the map iterates them.
 */
typedef struct txn_TupleU32TupleEntryU64_t
{
	uint32		f0;
	txn_TupleEntryU64_t f1;
} txn_TupleU32TupleEntryU64_t;

extern bool txn_rewrite_based(HostIoIo *io, TreesStoreNodeStore store, bool has_root, TreesStoreTree root,
							  StdVec *result, HostError *error);
extern bool txn_rewrite_keys(HostIoIo *io, const HostSharedHeld *held, const TxnRewriteBased *based,
							 size_t based_len, HostLsn folded, TxnRewriteKeysEach each, void *each_captures,
							 void (*each_drop) (void *), HostError *error);
extern bool txn_rewrite_new_base(StdVec *based, StdVec *copied, bool keyless, uint64 folded, uint64 max_folded,
								 uint64 max_ended, TxnBaseBase *made, TxnRewritePacked packed,
								 const host_TupleRowU32_t *carried, size_t carried_len,
								 TxnFoldTrees *result_0, TxnBaseBase *result_1, HostError *error);
extern bool txn_rewrite_carried(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held,
								const TxnRecordRowChange *change, TxnMemtableVersionShown shown,
								TxnMemtableLockMode lock, bool logged, const TxnInsertPage *page, HostError *error);

/* txn_rewrite_value_map's values are in ascending order of id, each id once, as the BTreeMap's. */
extern bool txn_rewrite_value_map(const txn_TupleU32TupleEntryU64_t *values, size_t values_len,
								  TxnFoldTrees *result_0, TxnBaseSizes *result_1, HostError *error);
extern bool txn_rewrite_spaces(HostIoIo *io, const HostSharedHeld *held, HostStorage storage, StdVec *result,
							   HostError *error);
extern bool txn_rewrite_spaced(HostIoIo *io, const HostSharedHeld *held, const TxnRecordMapCarried *carried,
							   bool logged, uint16 most, bool has_replayed, uint64 replayed, HostError *error);
extern void txn_rewrite_Based_drop(TxnRewriteBased *self);
extern void txn_rewrite_Copied_drop(TxnRewriteCopied *self);

#endif							/* DOLTGRES_TXN_REWRITE_H */
