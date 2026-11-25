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
 * txn's WAL records (txn's record.rs): a row change, a speculative insert's
 * completion, a page left, a place spent, a chunk written, a database-level
 * tree's change, a base advanced, map versions carried, a storage emptied,
 * and tuple ids freed, with the decoders redo and decoding read them by.
 */
#ifndef DOLTGRES_TXN_RECORD_H
#define DOLTGRES_TXN_RECORD_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/record.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/memtable.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/version.h"

/*
 * TxnRecordToast is Toast: the TOAST relation a row change's value ids are
 * mapped in, its storage's tablespace and relation number, in the row's
 * database, and its object id.
 */
typedef struct TxnRecordToast
{
	uint32		tablespace;
	uint32		relation;
	uint32		table;
} TxnRecordToast;

/*
 * TxnRecordRowChange is RowChange: a logged row change, who wrote it, the
 * storage and table it changes, its key, what it does, whether it changes no
 * key, and whether it was written frozen.
 */
typedef struct TxnRecordRowChange
{
	TxnMemtableVersionWriter writer;
	HostStorage storage;
	uint32		table;
	/* key is a Vec<u8>. */
	StdVec		key;
	TxnMemtableOp op;
	/* no_key is whether the change changes no key. */
	bool		no_key;
	/* frozen is whether an insert was written under TABLE_INSERT_FROZEN. */
	bool		frozen;
	/* heap_bytes is the row's heap bytes, zero for a change with no put. */
	uint64		heap_bytes;

	/*
	 * toast is the TOAST relation the row's out-of-line values are mapped in,
	 * present exactly when the put has value ids.
	 */
	bool		has_toast;
	TxnRecordToast toast;
	/* blobs is a Vec<(Address, u64)>: each stored value's root and length. */
	StdVec		blobs;

	/*
	 * kept is a Vec<usize>: the places among the put's values of the values
	 * an update kept from the row it replaced, in ascending order.
	 */
	StdVec		kept;
	/* speculative is whether an insert is speculative. */
	bool		speculative;
	/* replaced is the tuple id of the row an update replaces. */
	bool		has_replaced;
	HostRow		replaced;
	/* index is the index's object id on a change of an index's storage. */
	bool		has_index;
	uint32		index;
} TxnRecordRowChange;

/*
 * TxnRecordCompletion is Completion: a speculative insert's confirm or kill,
 * naming its writer, its storage, and its row by key and tuple id.
 */
typedef struct TxnRecordCompletion
{
	TxnMemtableVersionWriter writer;
	HostStorage storage;
	/* key is a Vec<u8>. */
	StdVec		key;
	HostRow		tuple;
} TxnRecordCompletion;

/*
 * TxnRecordPageLeft is PageLeft: a logged leave of a storage's fresh page for
 * the next, who left it, the storage, and the counter the leave moved to.
 */
typedef struct TxnRecordPageLeft
{
	TxnMemtableVersionWriter writer;
	HostStorage storage;
	uint64		to;
} TxnRecordPageLeft;

/*
 * TxnRecordSpent is Spent: an insert's or update's place its take spent and
 * logged before Postgres refused its row or before its resolve waited.
 */
typedef struct TxnRecordSpent
{
	TxnMemtableVersionWriter writer;
	HostStorage storage;
	HostRow		tuple;
	uint64		heap_bytes;
} TxnRecordSpent;

/*
 * TxnRecordBuiltNode is BuiltNode: a built base's tree node's part of its
 * Chunk record, its stamps, its storage's tablespace and relation, its fold
 * point, and its writer's top-level full xid.
 */
typedef struct TxnRecordBuiltNode
{
	/* stamps is a Vec<u8>. */
	StdVec		stamps;
	uint32		tablespace;
	uint32		relation;
	uint64		point;
	uint64		writer;
} TxnRecordBuiltNode;

/*
 * TxnRecordChunkWrite is ChunkWrite: a logged chunk a logged storage's write
 * put into its repository's working state's delta, which redo pushes again.
 */
typedef struct TxnRecordChunkWrite
{
	/* repository is the repository's number, never zero. */
	uint32		repository;
	ChunksAddressAddress address;
	/* bytes is a Vec<u8>. */
	StdVec		bytes;
	/* node is a built base's tree node's part, or none for any other chunk. */
	bool		has_node;
	TxnRecordBuiltNode node;
	/* writer is a blob's chunk's writer, or none for a built base's node. */
	bool		has_writer;
	TxnMemtableVersionWriter writer;
} TxnRecordChunkWrite;

/* TxnRecordTree is Tree: the database-level tree a database change is of. */
typedef enum TxnRecordTree
{
	TXN_RECORD_TREE_IDENTITY = 1,
} TxnRecordTree;

/*
 * TxnRecordDatabaseChange is DatabaseChange: a change of one key of a
 * database-level tree, the tree, the writer, the repository, the key, and
 * the value it gives the key, or none for the key's removal.
 */
typedef struct TxnRecordDatabaseChange
{
	TxnRecordTree tree;
	TxnMemtableVersionWriter writer;
	/* repository is the repository's number, never zero. */
	uint32		repository;
	/* key is a Vec<u8>. */
	StdVec		key;
	/* value is a Vec<u8>, or none for the key's removal. */
	bool		has_value;
	StdVec		value;
} TxnRecordDatabaseChange;

/*
 * TxnRecordBaseAdvanced is BaseAdvanced: the base a fold installed in a
 * storage's state, its fold point, the largest top-level xid it applied, the
 * base record, the packs it wrote, whether a backend built it whole, a built
 * base's horizon, a DDL's transform's chain entries, and how many statuses
 * records the fold logged before it.
 */
typedef struct TxnRecordBaseAdvanced
{
	HostStorage storage;
	uint64		folded;
	uint64		max_folded;
	TxnBaseBase base;
	/* packs is a Vec<PackId>. */
	StdVec		packs;
	bool		built;
	uint64		horizon;
	/* chain is a Vec<Pending>, or none for every record but a chain's. */
	bool		has_chain;
	StdVec		chain;

	/*
	 * statuses is how many FoldStatuses records the fold logged before this
	 * one, 0 for a built or a chain's record (bugs row 432).
	 */
	uint64		statuses;
} TxnRecordBaseAdvanced;

/*
 * TxnRecordCarriedTake is CarriedTake: a map version a move carries, its
 * writer, the tuple id it took or emptied, the entry a put put, and the
 * ender a fold's drop stored on a take.
 */
typedef struct TxnRecordCarriedTake
{
	TxnMemtableVersionWriter writer;
	HostRow		tuple;
	/* value is a Vec<u8>, or none for a removal. */
	bool		has_value;
	StdVec		value;
	uint64		ender;

	/*
	 * mark is a Vec<u32>: the value ids of a free space map's values mark,
	 * none for any other version.
	 */
	bool		has_mark;
	StdVec		mark;
} TxnRecordCarriedTake;

/*
 * TxnRecordMapCarried is MapCarried: the map versions of one key a storage's
 * move carried into its new storage, the storage, the map, the key, and
 * each version, oldest first.
 */
typedef struct TxnRecordMapCarried
{
	HostStorage storage;
	TxnMemtableMapMap map;
	uint32		key;
	/* versions is a Vec<CarriedTake>. */
	StdVec		versions;
} TxnRecordMapCarried;

/*
 * TxnRecordCarryUnlisted is CarryUnlisted: a carry whose spill files its
 * primary took back out of its new storage's manifest after a count's add
 * failed, named by the ends of the carry's records, ascending.
 */
typedef struct TxnRecordCarryUnlisted
{
	HostStorage storage;
	/* ends is a Vec<u64>. */
	StdVec		ends;
} TxnRecordCarryUnlisted;

/*
 * TxnRecordFoldStatuses is FoldStatuses: one of a fold's records, logged
 * before its BaseAdvanced, of the writers the fold answered aborted and the
 * committed writers whose killed versions it answered aborted, at its point
 * in a storage, with its index among the fold's records of them (bugs row
 * 432).
 */
typedef struct TxnRecordFoldStatuses
{
	HostStorage storage;
	uint64		folded;
	uint64		index;
	/* aborted and killed are Vec<u64>s, ascending. */
	StdVec		aborted;
	StdVec		killed;
} TxnRecordFoldStatuses;

/* TxnRecordStorageEmptied is StorageEmptied: a storage emptied in place by a truncate. */
typedef struct TxnRecordStorageEmptied
{
	HostStorage storage;
} TxnRecordStorageEmptied;

/*
 * TxnRecordTidsFreed is TidsFreed: a logged free of a storage's tuple ids,
 * the conflict horizon, the freed and the dead tuple ids as runs, and the
 * TOAST storage whose value map the free takes values down in.
 */
typedef struct TxnRecordTidsFreed
{
	HostStorage storage;
	uint64		horizon;
	/* runs is a Vec<(u32, u16, u16, u64)>: block, first, count, and bytes. */
	StdVec		runs;
	/* dead is a Vec<(u32, u16, u16, u64)>, as runs. */
	StdVec		dead;

	/*
	 * enders is a Vec<u64>: each of runs' tuple ids' stored ender, then each
	 * of dead's, in tuple id order, which a replayed free's take-out matches
	 * (bugs row 320).
	 */
	StdVec		enders;
	/* values is the TOAST storage and each value id's count, or none. */
	bool		has_values;
	host_TupleStorageVecTupleU32U64_t values;

	/*
	 * cleared is the block whose full flag a prune that found nothing to free
	 * cleared, or none (bugs row 370).
	 */
	bool		has_cleared;
	uint32		cleared;
} TxnRecordTidsFreed;

/*
 * TXN_RECORD_PAGE_LEFT_MOST is the longest body a leave encodes to: each of
 * its six fields' head and integer at their longest.
 */
#define TXN_RECORD_PAGE_LEFT_MOST (6 * 21)

/*
 * TXN_RECORD_SPENT_MOST is the longest body a spent place encodes to: each of
 * its seven fields' head and integer at their longest.
 */
#define TXN_RECORD_SPENT_MOST (7 * 21)

/*
 * TxnRecordEncoding is Encoding: what a row change's encode needs beside the
 * change, made before a checkpoint window, a writer with room for the
 * longest body and the bytes of a put's addresses, a moved row's successor,
 * the stored values, and the kept values.
 */
typedef struct TxnRecordEncoding
{
	ChunksRecordRecordWriter writer;
	StdVec		addresses;
	StdVec		successor;
	StdVec		values;
	StdVec		kept;
} TxnRecordEncoding;

extern bool txn_record_Toast_PartialEq_eq(const TxnRecordToast *self, const TxnRecordToast *other);
extern bool txn_record_Toast_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_record_RowChange_PartialEq_eq(const TxnRecordRowChange *self, const TxnRecordRowChange *other);
extern bool txn_record_RowChange_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_record_PageLeft_PartialEq_eq(const TxnRecordPageLeft *self, const TxnRecordPageLeft *other);
extern bool txn_record_PageLeft_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_record_BuiltNode_PartialEq_eq(const TxnRecordBuiltNode *self, const TxnRecordBuiltNode *other);
extern bool txn_record_BuiltNode_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_record_ChunkWrite_PartialEq_eq(const TxnRecordChunkWrite *self, const TxnRecordChunkWrite *other);
extern bool txn_record_ChunkWrite_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_record_DatabaseChange_PartialEq_eq(const TxnRecordDatabaseChange *self, const TxnRecordDatabaseChange *other);
extern bool txn_record_DatabaseChange_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_record_BaseAdvanced_PartialEq_eq(const TxnRecordBaseAdvanced *self, const TxnRecordBaseAdvanced *other);
extern bool txn_record_BaseAdvanced_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_record_CarriedTake_PartialEq_eq(const TxnRecordCarriedTake *self, const TxnRecordCarriedTake *other);
extern bool txn_record_CarriedTake_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_record_MapCarried_PartialEq_eq(const TxnRecordMapCarried *self, const TxnRecordMapCarried *other);
extern bool txn_record_MapCarried_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_record_TidsFreed_PartialEq_eq(const TxnRecordTidsFreed *self, const TxnRecordTidsFreed *other);
extern bool txn_record_TidsFreed_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_record_Tree_Debug_fmt(const void *value, StdFormatter *f);
extern void txn_record_RowChange_encoding(const TxnRecordRowChange *self, TxnRecordEncoding *result);
extern void txn_record_Encoding_name(TxnRecordEncoding *self, HostRow tid);
extern void txn_record_Encoding_drop(TxnRecordEncoding *self);
extern void txn_record_RowChange_encode_with(const TxnRecordRowChange *self, TxnRecordEncoding *encoding,
											 StdVec *result);
extern void txn_record_RowChange_encode(const TxnRecordRowChange *self, StdVec *result);
extern bool txn_record_RowChange_decode(const uint8 *body, size_t body_len, TxnRecordRowChange *result,
										HostError *error);
extern bool txn_record_Completion_decode(const uint8 *body, size_t body_len, TxnRecordCompletion *result,
										 HostError *error);
extern bool txn_record_PageLeft_decode(const uint8 *body, size_t body_len, TxnRecordPageLeft *result,
									   HostError *error);
extern bool txn_record_Spent_is(const uint8 *body, size_t body_len, bool *result, HostError *error);
extern bool txn_record_Spent_decode(const uint8 *body, size_t body_len, TxnRecordSpent *result, HostError *error);
extern bool txn_record_ChunkWrite_decode(const uint8 *body, size_t body_len, TxnRecordChunkWrite *result,
										 HostError *error);
extern bool txn_record_DatabaseChange_decode(const uint8 *body, size_t body_len, TxnRecordDatabaseChange *result,
											 HostError *error);
extern bool txn_record_BaseAdvanced_decode(const uint8 *body, size_t body_len, TxnRecordBaseAdvanced *result,
										   HostError *error);
extern bool txn_record_MapCarried_decode(const uint8 *body, size_t body_len, TxnRecordMapCarried *result,
										 HostError *error);
extern bool txn_record_StorageEmptied_decode(const uint8 *body, size_t body_len, TxnRecordStorageEmptied *result,
											 HostError *error);
extern bool txn_record_TidsFreed_decode(const uint8 *body, size_t body_len, uint16 most, uint64 page,
										TxnRecordTidsFreed *result, HostError *error);
extern void txn_record_RowChange_drop(TxnRecordRowChange *self);
extern void txn_record_Completion_drop(TxnRecordCompletion *self);
extern void txn_record_ChunkWrite_drop(TxnRecordChunkWrite *self);
extern void txn_record_DatabaseChange_drop(TxnRecordDatabaseChange *self);
extern void txn_record_BaseAdvanced_drop(TxnRecordBaseAdvanced *self);
extern void txn_record_MapCarried_drop(TxnRecordMapCarried *self);
extern void txn_record_TidsFreed_drop(TxnRecordTidsFreed *self);
extern bool txn_record_RowChange_put(const TxnRecordRowChange *self, const TxnMemtablePut **result);
extern TxnMemtablePut *txn_record_op_put(TxnMemtableOp *op);
extern void txn_record_Completion_encode(const TxnRecordCompletion *self, bool killed, StdVec *result);
extern bool txn_record_Completion_killed(const uint8 *body, size_t body_len, bool *result, HostError *error);
extern void txn_record_ChunkWrite_encode(const TxnRecordChunkWrite *self, StdVec *result);
extern bool txn_record_ChunkWrite_pieces(TxnRecordChunkWrite *self, StdVec *result, HostError *error);
extern void txn_record_BaseAdvanced_encode(const TxnRecordBaseAdvanced *self, StdVec *result);
extern void txn_record_StorageEmptied_encode(const TxnRecordStorageEmptied *self, StdVec *result);
extern void txn_record_CarryUnlisted_encode(const TxnRecordCarryUnlisted *self, StdVec *result);
extern bool txn_record_CarryUnlisted_decode(const uint8 *body, size_t body_len, TxnRecordCarryUnlisted *result,
											HostError *error);
extern void txn_record_CarryUnlisted_drop(TxnRecordCarryUnlisted *self);
extern void txn_record_FoldStatuses_encode(const TxnRecordFoldStatuses *self, StdVec *result);
extern bool txn_record_FoldStatuses_decode(const uint8 *body, size_t body_len, TxnRecordFoldStatuses *result,
										   HostError *error);
extern void txn_record_FoldStatuses_drop(TxnRecordFoldStatuses *self);

extern void txn_record_PageLeft_encode(const TxnRecordPageLeft *self, StdVec *result);
extern void txn_record_PageLeft_encode_with(const TxnRecordPageLeft *self, ChunksRecordRecordWriter *w, StdVec *result);
extern void txn_record_Spent_encode_with(const TxnRecordSpent *self, ChunksRecordRecordWriter *w, StdVec *result);
extern void txn_record_DatabaseChange_encode(const TxnRecordDatabaseChange *self, StdVec *result);

extern void txn_record_MapCarried_encode(const TxnRecordMapCarried *self, StdVec *result);
extern void txn_record_TidsFreed_encode(const TxnRecordTidsFreed *self, StdVec *result);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_record_tests_a_row_change_reads_back_and_refuses_every_other_form(void);
extern void test_txn_record_tests_a_page_leave_reads_back_and_refuses_every_other_form(void);
extern void test_txn_record_tests_a_built_bases_node_reads_back_with_its_stamps(void);
extern void test_txn_record_tests_stamps_on_a_blob_or_of_another_count_are_refused(void);
extern void test_txn_record_tests_a_moves_carried_free_space_reads_back_and_refuses_every_other_form(void);
extern void test_txn_record_tests_a_chunk_reads_back_and_refuses_every_other_form(void);
extern void test_txn_record_tests_an_updates_kept_values_read_back_in_their_values_order(void);
extern void test_txn_record_tests_a_folds_record_reads_back_and_refuses_every_other_form(void);
extern void test_txn_record_tests_a_free_reads_back_and_refuses_runs_out_of_bounds_or_order(void);
extern void test_txn_record_tests_a_frees_taken_down_values_read_back_and_refuse_every_other_form(void);
extern void test_txn_record_tests_an_index_entrys_change_reads_back_and_refuses_every_field_its_op_doesnt_carry(void);
extern void test_txn_record_tests_a_built_nodes_key_names_its_base_and_its_fields_come_together(void);
extern void test_txn_record_tests_a_frees_cleared_block_reads_back_and_refuses_every_other_form(void);
#endif

#endif							/* DOLTGRES_TXN_RECORD_H */
