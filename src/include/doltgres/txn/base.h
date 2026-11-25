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
 * A storage's base record (txn's base.rs): its sizes, its key, and the base
 * record's fields, which txn's contract gives every module.
 */
#ifndef DOLTGRES_TXN_BASE_H
#define DOLTGRES_TXN_BASE_H

#include "doltgres/std/fmt.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/store.h"

/* TxnBaseSizes is Sizes: a tree's totals, its entries, leaf bytes, key bytes, heap bytes, and rows. */
typedef struct TxnBaseSizes
{
	uint64		entries;
	uint64		leaf_bytes;
	uint64		key_bytes;
	uint64		heap_bytes;
	uint64		rows;
} TxnBaseSizes;

/*
 * TxnBaseBlockCounts is BlockCounts: how many blocks a tree's entries take,
 * its leaves and its nodes above them.
 */
typedef struct TxnBaseBlockCounts
{
	uint64		leaves;
	uint64		internal;

	/*
	 * height is the levels of nodes above the leaves, 0 for an empty tree or
	 * one leaf, as Postgres' _bt_getrootheight counts a btree's.
	 */
	uint8		height;
} TxnBaseBlockCounts;

/*
 * TxnBaseKeyColumn is KeyColumn: a column of a storage's key, its attribute
 * number, its operator class, its collation, 0 for none, its type, and
 * whether its key type is compared through its class.
 */
typedef struct TxnBaseKeyColumn
{
	uint16		attnum;
	uint32		class;
	uint32		collation;

	/*
	 * typid is the column's type when the storage's state was made, which its
	 * keys are decoded by.
	 */
	uint32		typid;

	/*
	 * compared is whether the column's key type is compared through its
	 * operator class, which orders the storage's tree by a comparator.
	 */
	bool		compared;
} TxnBaseKeyColumn;

/* TxnBaseKeyTag is Key's variants. */
typedef enum TxnBaseKeyTag
{
	/* A keyless table's storage's or an index's. */
	TXN_BASE_KEY_NONE,
	/* A table's state made in redo before a backend recorded its key. */
	TXN_BASE_KEY_UNKNOWN,
	/* Its columns in key order. */
	TXN_BASE_KEY_COLUMNS,
} TxnBaseKeyTag;

/* TxnBaseKey is Key: the key a storage's rows are encoded under, fixed when its state is made. */
typedef struct TxnBaseKey
{
	TxnBaseKeyTag tag;
	union
	{
		/* columns is a Vec<KeyColumn>. */
		StdVec		columns;
	}			u;
} TxnBaseKey;

/*
 * TxnBaseBase is Base: a storage's base record, its root, its fold point, the
 * largest top-level xid it folded, the epoch its rows are stored at, its
 * tuple id tree, the fresh end, the free space map, its sizes, its key, its
 * table's and index's object ids, the newest xid that ended a row it applied,
 * its table's pending transforms, and its read point.
 */
typedef struct TxnBaseBase
{
	/* root is the base's root, when its tree isn't empty. */
	bool		has_root;
	TreesStoreTree root;

	/*
	 * folded is the fold point, the WAL position the base's contents depend
	 * on.
	 */
	uint64		folded;
	uint64		max_folded;
	uint32		epoch;
	bool		has_tuple_ids;
	TreesStoreTree tuple_ids;
	/* fresh_end is the next tuple id past every row the base holds. */
	uint64		fresh_end;
	bool		has_free_space;
	TreesStoreTree free_space;
	TxnBaseSizes sizes;
	uint32		table;
	/* index is the index's object id for an index's storage. */
	bool		has_index;
	uint32		index;
	TxnBaseKey	key;

	/*
	 * pending is the tree of the tuple ids whose base rows a fold replaced or
	 * deleted and that VACUUM hasn't freed yet.
	 */
	bool		has_pending;
	TreesStoreTree pending;

	/*
	 * max_ended is the newest xid of an applied version that ended a row,
	 * whose writer was another xid than the row's, zero for none.
	 */
	uint64		max_ended;

	/*
	 * chain is the tree of the table's transforms not yet applied to the
	 * base, keyed by their places in the chain, oldest first.
	 */
	bool		has_chain;
	TreesStoreTree chain;

	/*
	 * read is the read point, the point readers decide by which delta and
	 * spilled versions the base applies, below a moved base's fold point and
	 * equal to it on every other base.
	 */
	uint64		read;
} TxnBaseBase;

/*
 * TxnBasePending is Pending: a transform not yet applied to a base, its DDL's
 * top-level full xid and command, the full xid of the subtransaction that ran
 * it, the transform in access's encoding, and its commit's end once a save
 * wrote it.
 */
typedef struct TxnBasePending
{
	uint64		xid;
	uint32		command;
	uint64		sub;
	/* transform is the transform's bytes, a Vec<u8>. */
	StdVec		transform;
	/* end is the commit record's end, once a save writes it. */
	bool		has_end;
	uint64		end;
} TxnBasePending;

extern TxnBaseBase txn_base_Base_empty(TxnBaseKey *key, uint32 table, bool has_index, uint32 index);
extern TxnBaseKey txn_base_Key_of(StdVec *columns);
extern bool txn_base_chain_of(HostIoIo *io, TreesStoreNodeStore store, bool has_chain, TreesStoreTree chain,
							  StdVec *result, HostError *error);
extern void txn_base_tuple_id_key(uint64 tuple_id, uint8 result[6]);
extern bool txn_base_tuple_id_of(const uint8 *key, size_t key_len, uint64 *result, HostError *error);
extern void txn_base_pair_value(const uint8 *key, size_t key_len, uint32 heap_bytes, StdVec *result);
extern bool txn_base_pair_parts(const uint8 *value, size_t value_len, StdBytes *result_0, uint32 *result_1,
								HostError *error);
extern HostError txn_base_storage(ChunksErrorError *error);
extern TxnBaseSizes txn_base_Sizes_summed(TxnBaseSizes self, TxnBaseSizes other);
extern TxnBaseBlockCounts txn_base_Sizes_counts(const TxnBaseSizes *self, uint32 target);
extern void txn_base_Base_encode(const TxnBaseBase *self, StdVec *result_0, StdVec *result_1);
extern bool txn_base_Base_decode(const uint8 *bytes, size_t bytes_len, const chunks_TupleU32Locator_t *addresses,
								 size_t addresses_len, TxnBaseBase *result, HostError *error);
extern TxnBaseSizes txn_base_Sizes_Default_default(void);
extern bool txn_base_Sizes_PartialEq_eq(const TxnBaseSizes *self, const TxnBaseSizes *other);
extern bool txn_base_Sizes_Debug_fmt(const void *value, StdFormatter *f);
extern TxnBaseBlockCounts txn_base_BlockCounts_Default_default(void);
extern bool txn_base_BlockCounts_PartialEq_eq(const TxnBaseBlockCounts *self, const TxnBaseBlockCounts *other);
extern bool txn_base_BlockCounts_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_base_KeyColumn_PartialEq_eq(const TxnBaseKeyColumn *self, const TxnBaseKeyColumn *other);
extern bool txn_base_KeyColumn_Debug_fmt(const void *value, StdFormatter *f);
extern void txn_base_Key_Clone_clone(const TxnBaseKey *self, TxnBaseKey *result);
extern bool txn_base_Key_PartialEq_eq(const TxnBaseKey *self, const TxnBaseKey *other);
extern bool txn_base_Key_Debug_fmt(const void *value, StdFormatter *f);
extern void txn_base_Key_drop(TxnBaseKey *self);
extern bool txn_base_Base_PartialEq_eq(const TxnBaseBase *self, const TxnBaseBase *other);
extern bool txn_base_Base_Debug_fmt(const void *value, StdFormatter *f);
extern void txn_base_Base_drop(TxnBaseBase *self);
extern bool txn_base_Pending_same(const TxnBasePending *self, const TxnBasePending *other);
extern void txn_base_Pending_Clone_clone(const TxnBasePending *self, TxnBasePending *result);
extern bool txn_base_Pending_PartialEq_eq(const TxnBasePending *self, const TxnBasePending *other);
extern bool txn_base_Pending_Debug_fmt(const void *value, StdFormatter *f);
extern void txn_base_Pending_drop(TxnBasePending *self);
extern void txn_base_chain_key(uint32 place, uint8 result[4]);
extern void txn_base_chain_value(const TxnBasePending *pending, StdVec *result);
extern bool txn_base_chain_read(const uint8 *value, size_t value_len, TxnBasePending *result, HostError *error);
extern void txn_base_pending_value(uint64 ender, uint32 heap_bytes, const uint32 *ids, size_t ids_len,
								   StdVec *result);
extern bool txn_base_pending_read(const uint8 *value, size_t value_len, uint64 *result_0, uint32 *result_1,
								  StdVec *result_2, HostError *error);

/*
 * TXN_BASE_OWN is OWN, the bit of a pending tuple id's ender word that marks
 * a row its own writer ended, whose free waits for the horizon to pass that
 * writer and logs no conflict horizon; full xids stay below it.
 */
#define TXN_BASE_OWN (UINT64CONST(1) << 63)

#ifdef DOLTGRES_TEST_TXN
extern void txn_base_Base_Clone_clone(const TxnBaseBase *self, TxnBaseBase *result);
extern TxnBaseBase txn_base_tests_full(void);
extern TxnBaseBase txn_base_tests_empty(void);
extern void test_txn_base_tests_a_base_record_round_trips_at_its_limits(void);
extern void test_txn_base_tests_a_cut_short_or_unlocated_record_is_corrupt(void);
extern void test_txn_base_tests_every_other_form_of_a_base_record_is_corrupt(void);
extern void test_txn_base_tests_a_storage_key_round_trips_and_every_other_form_is_corrupt(void);
extern void test_txn_base_tests_planned_counts_follow_the_sizes_alone(void);
extern void test_txn_base_tests_planned_counts_of_entries_larger_than_a_node_are_those_of_the_tree_the_splitter_makes(void);
extern void test_txn_base_tests_a_chain_entry_round_trips_and_every_other_form_is_corrupt(void);
#endif

#endif							/* DOLTGRES_TXN_BASE_H */
