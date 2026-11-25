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
 * A branch's deltas (txn's memtable/branch.rs): a working state's record of
 * its memtables of chunks and of its identity tree, the chunks and built
 * nodes they hold, and its identity tree's versions.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_BRANCH_H
#define DOLTGRES_TXN_MEMTABLE_BRANCH_H

#include "doltgres/host/delta.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/blob.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/status.h"

/* TXN_MEMTABLE_BRANCH_ADDRESS is ADDRESS, the length of a chunk's address, a blob's chunk's key in the memtable. */
#define TXN_MEMTABLE_BRANCH_ADDRESS 32

/* TXN_MEMTABLE_BRANCH_NODE_KEY is NODE_KEY, the length of a built base's tree node's key in a working state's delta. */
#define TXN_MEMTABLE_BRANCH_NODE_KEY (TXN_MEMTABLE_BRANCH_ADDRESS + 16)

/* TXN_MEMTABLE_BRANCH_LEAF is LEAF, the most bytes a blob's leaf chunk takes: its bytes and its header's four, which PIECE is sized by. */
#define TXN_MEMTABLE_BRANCH_LEAF (TREES_BLOB_MAX_LEAF + 4)

/* TXN_MEMTABLE_BRANCH_PIECE_KEY is PIECE_KEY, the length of the key of a piece of a built node: its node's key and its number. */
#define TXN_MEMTABLE_BRANCH_PIECE_KEY (TXN_MEMTABLE_BRANCH_NODE_KEY + 2)

/* TXN_MEMTABLE_BRANCH_PIECE is PIECE, the most bytes of a built node's value one piece holds. */
#define TXN_MEMTABLE_BRANCH_PIECE (TXN_MEMTABLE_BRANCH_LEAF - (TXN_MEMTABLE_BRANCH_PIECE_KEY - TXN_MEMTABLE_BRANCH_ADDRESS))

/* txn_TupleVecU8VecU8_t is Piece, (Vec<u8>, Vec<u8>): a part of a chunk as a working state's delta keeps it, its key and its bytes. */
typedef struct txn_TupleVecU8VecU8_t
{
	StdVec		f0;
	StdVec		f1;
} txn_TupleVecU8VecU8_t;

/* txn_TupleU64VecU8_t is node's (u64, Vec<u8>): a built node's writer's full xid and its value. */
typedef struct txn_TupleU64VecU8_t
{
	uint64		f0;
	StdVec		f1;
} txn_TupleU64VecU8_t;

/*
 * TxnMemtableTablePreparedBranchDeltas is Prepared<BranchDeltas>: a prepare's
 * hold and its targets in a working state's delta, handed to the prepare's
 * closure; table.c's Prepared, which serves either owner.
 */
typedef TxnMemtableTablePreparedState TxnMemtableTablePreparedBranchDeltas;

/* The delta's lock, DeltasLock, and its long lock, ChunkSpillsWriting, the writers' lock of its chunk spills' manifest. */
#define TXN_MEMTABLE_BRANCH_DELTAS_LOCK_INDEX 0
#define TXN_MEMTABLE_BRANCH_CHUNK_SPILLS_WRITING_INDEX 1

/* The delta's long lock IdentitySpillsWriting, the writers' lock of its identity spills' manifest (bugs row 291). */
#define TXN_MEMTABLE_BRANCH_IDENTITY_SPILLS_WRITING_INDEX 2

/* The delta's atomics of its chunk spills' manifest: ChunkSpills, ChunkSpillsPending, and ChunkSpillsChanged. */
#define TXN_MEMTABLE_BRANCH_CHUNK_SPILLS_INDEX 3
#define TXN_MEMTABLE_BRANCH_CHUNK_SPILLS_PENDING_INDEX 4
#define TXN_MEMTABLE_BRANCH_CHUNK_SPILLS_CHANGED_INDEX 5

/* TXN_MEMTABLE_BRANCH_CHUNK_SPILLS_TAKEN_INDEX is ChunkSpillsTaken's atomic: the take generation of the delta's chunk spills. */
#define TXN_MEMTABLE_BRANCH_CHUNK_SPILLS_TAKEN_INDEX 7

/* The delta's atomics of its identity spills' manifest: IdentitySpills, IdentitySpillsPending, and IdentitySpillsChanged. */
#define TXN_MEMTABLE_BRANCH_IDENTITY_SPILLS_INDEX 8
#define TXN_MEMTABLE_BRANCH_IDENTITY_SPILLS_PENDING_INDEX 9
#define TXN_MEMTABLE_BRANCH_IDENTITY_SPILLS_CHANGED_INDEX 10

/* TXN_MEMTABLE_BRANCH_IDENTITY_SPILLS_TAKEN_INDEX is IdentitySpillsTaken's atomic: the take generation of the delta's identity spills. */
#define TXN_MEMTABLE_BRANCH_IDENTITY_SPILLS_TAKEN_INDEX 11

/* TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS is Identity's Tree: the memtable of a working state's identity tree. */
extern const TxnMemtableTableTreeOps TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS;

/* TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS is BranchDeltas' Shape and Pool: a working state's deltas. */
extern const HostSharedPoolOps TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS;

/* TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS is Chunks' Tree: the memtable of a working state's chunks. */
extern const TxnMemtableTableTreeOps TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS;

/* TXN_MEMTABLE_BRANCH_CHUNK_WORD_INDEX is ChunkWord's atomic: the word naming a working state's chunks' memtable. */
#define TXN_MEMTABLE_BRANCH_CHUNK_WORD_INDEX 1

/* TXN_MEMTABLE_BRANCH_IDENTITY_WORD is IdentityWord's atomic: the word naming a working state's identity memtable. */
#define TXN_MEMTABLE_BRANCH_IDENTITY_WORD 2

extern void txn_memtable_branch_node_key(const uint8 *address, uint32 tablespace, uint32 relation, uint64 point,
										 uint8 *result);
extern bool txn_memtable_branch_pieces(const uint8 *node, uint64 writer, const uint8 *value, size_t value_len,
									   StdVec *result, HostError *error);
extern bool txn_memtable_branch_node(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, bool *some,
									 txn_TupleU64VecU8_t *result, HostError *error);
extern bool txn_memtable_branch_node_taken(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, HostError *error);
extern bool txn_memtable_branch_find(HostSharedWithin within, uint32 repository, bool *some, HostSharedHeld *result,
									 HostError *error);
extern bool txn_memtable_branch_deltas(HostIoIo *io, uint32 repository, uint32 own, HostSharedHeld *result,
									   HostError *error);
extern bool txn_memtable_branch_get(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len,
									bool *some, StdVec *result, HostError *error);
extern bool txn_memtable_branch_taken(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len,
									  HostError *error);
extern bool txn_memtable_branch_durable(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len,
										bool *result, HostError *error);
extern bool txn_memtable_branch_nodes_taken(HostIoIo *io, HostSharedRecord deltas,
											bool (*picked) (void *captures, HostIoIo *io, uint32 tablespace,
															uint32 relation, uint64 point, uint64 writer,
															bool *result, HostError *error),
											void *picked_captures, void (*picked_drop) (void *captures),
											HostError *error);
extern bool txn_memtable_branch_every_nodes_taken(HostIoIo *io,
												  bool (*picked) (void *captures, HostIoIo *io, uint32 repository,
																  uint32 tablespace, uint32 relation, uint64 point,
																  uint64 writer, bool *result, HostError *error),
												  void *picked_captures, void (*picked_drop) (void *captures),
												  HostError *error);
extern bool txn_memtable_branch_identity_finish(HostIoTakesShort io, TxnMemtableTablePreparedBranchDeltas *prepared,
												size_t key, const TxnMemtableVersionChange *change, bool has_roomed,
												TxnStatusRoomed roomed, TxnMemtableVersionHandle *result,
												HostError *error);
extern bool txn_memtable_branch_identity_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord deltas,
												  const uint8 *key, size_t key_len, StdVec *result, HostError *error);
extern bool txn_memtable_branch_first_piece(const uint8 *key, size_t key_len, uint8 *result);
extern uint32 txn_memtable_branch_own(HostSharedRecord record);
extern bool txn_memtable_branch_rehomed(HostIoIo *io, uint32 repository, uint32 from, uint32 to,
										HostError *error);
extern bool txn_memtable_branch_repository(HostSharedRecord record, uint32 *result);
extern bool txn_memtable_branch_filled(HostIoIo *io, StdVec *result, HostError *error);
extern bool txn_memtable_branch_prepare(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len,
										size_t need, bool (*inside) (void *captures, HostIoIo *io,
																	 TxnMemtableTablePreparedBranchDeltas *prepared,
																	 HostError *error),
										void *inside_captures, void (*inside_drop) (void *captures),
										HostError *error);
extern bool txn_memtable_branch_finish(HostIoIo *io, TxnMemtableTablePreparedBranchDeltas *prepared,
									   TxnMemtableVersionWriter writer, StdVec *chunk, bool has_lsn, uint64 lsn,
									   TxnMemtableVersionHandle *result, HostError *error);
extern bool txn_memtable_branch_holds(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len,
									  uint64 lsn, bool *result, HostError *error);
extern bool txn_memtable_branch_dropped(HostIoIo *io, uint32 repository, HostError *error);
extern bool txn_memtable_branch_identity_holds(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len,
											   uint64 lsn, bool *result, HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern bool txn_memtable_branch_tests_held_bools_Debug_fmt(const void *value, StdFormatter *f);
extern void test_txn_memtable_branch_tests_an_abort_leaves_the_chunk_a_committed_transaction_wrote(void);
extern void test_txn_memtable_branch_tests_a_chunk_reads_back_by_its_address_until_its_repositorys_drop(void);
extern void test_txn_memtable_branch_tests_a_delta_whose_prepare_pushed_nothing_is_reclaimed(void);
extern void test_txn_memtable_branch_tests_a_chunks_versions_name_their_writer_and_are_durable_once_logged(void);
extern void test_txn_memtable_branch_tests_a_chunk_is_held_by_its_records_lsn(void);
#endif

#endif							/* DOLTGRES_TXN_MEMTABLE_BRANCH_H */
