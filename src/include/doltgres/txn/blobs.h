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
 * A repository's blob trees (txn's blobs.rs): the node store a value's blob
 * tree is cut through, the reader of a repository's blob trees, and the
 * writes and redo of a blob's chunks into its working state's delta. Part of
 * txn's contract.
 */
#ifndef DOLTGRES_TXN_BLOBS_H
#define DOLTGRES_TXN_BLOBS_H

#include "doltgres/chunks/address.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/record.h"

/* txn_TupleAddressVecU8_t is a cut chunk's (Address, Vec<u8>): its address and its hashed bytes. */
typedef struct txn_TupleAddressVecU8_t
{
	ChunksAddressAddress f0;
	StdVec		f1;
} txn_TupleAddressVecU8_t;

/* TxnBlobsCut is Cut: the node store a value's blob tree is cut through, which keeps each chunk the tree's write makes. */
typedef struct TxnBlobsCut
{
	/* chunks is a Vec<(Address, Vec<u8>)>. */
	StdVec		chunks;
} TxnBlobsCut;

/* TXN_BLOBS_CUT_NODE_STORE_OPS is Cut's NodeStore. */
extern const TreesStoreNodeStoreOps TXN_BLOBS_CUT_NODE_STORE_OPS;

/* TXN_BLOBS_CUT_NODE_STORE coerces a Cut to the NodeStore it implements. */
#define TXN_BLOBS_CUT_NODE_STORE(x) ((TreesStoreNodeStore) {&TXN_BLOBS_CUT_NODE_STORE_OPS, (x)})

/*
 * TxnBlobsBlobs is Blobs: the reader of a repository's blob trees, through
 * its working state's delta when one is published, and its packs' reader.
 */
typedef struct TxnBlobsBlobs
{
	bool		has_deltas;
	HostSharedHeld deltas;
	TreesStoreNodeStore packs;
} TxnBlobsBlobs;

/* TXN_BLOBS_BLOBS_NODE_STORE_OPS is Blobs' NodeStore. */
extern const TreesStoreNodeStoreOps TXN_BLOBS_BLOBS_NODE_STORE_OPS;

/* TXN_BLOBS_BLOBS_NODE_STORE coerces a Blobs to the NodeStore it implements. */
#define TXN_BLOBS_BLOBS_NODE_STORE(x) ((TreesStoreNodeStore) {&TXN_BLOBS_BLOBS_NODE_STORE_OPS, (x)})

extern void txn_blobs_Cut_Default_default(TxnBlobsCut *result);
extern void txn_blobs_Cut_drop(TxnBlobsCut *self);
extern bool txn_blobs_Blobs_new(HostIoIo *io, uint32 repository, TreesStoreNodeStore packs, TxnBlobsBlobs *result,
								HostError *error);
extern void txn_blobs_Blobs_drop(TxnBlobsBlobs *self);
extern bool txn_blobs_written(HostIoIo *io, const HostSharedHeld *deltas, uint32 repository,
							  TxnMemtableVersionWriter writer, StdVec *chunks, bool logged,
							  bool (*packed) (void *captures, HostIoIo *io, const ChunksAddressAddress *address,
											  bool *result, HostError *error),
							  void *packed_captures, void (*packed_drop) (void *captures), HostError *error);
extern bool txn_blobs_redo(HostIoIo *io, const HostSharedHeld *deltas, TxnRecordChunkWrite chunk, uint64 lsn,
						   HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_blobs_tests_a_logged_write_logs_a_chunk_an_unlogged_one_left_and_redo_makes_it_durable(void);
extern void test_txn_blobs_tests_a_blob_in_the_delta_reads_back_through_blobs(void);
extern void test_txn_blobs_tests_a_transactions_chunks_spill_read_back_and_leave_at_its_abort(void);
extern void test_txn_blobs_tests_a_chunk_another_running_transaction_holds_stays_after_that_ones_abort(void);
extern void test_txn_blobs_tests_a_rolled_back_chunk_takes_nothing_from_its_tops_count(void);
#endif

#endif							/* DOLTGRES_TXN_BLOBS_H */
