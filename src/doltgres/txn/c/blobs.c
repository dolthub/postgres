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
 * A row's out-of-line values' blobs (txn's blobs.rs): a value cut into a
 * blob tree whose chunks are kept by their addresses, then each chunk
 * written into the repository's working state's delta before the row's
 * window, in a checkpoint window of its own with its record when its
 * storage is logged.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/delta.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/node.h"
#include "doltgres/txn/blobs.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/written.h"

/* txn_blobs_unread returns the error of a read of a cut, which a blob's write never makes. */
static ChunksErrorError
txn_blobs_unread(const ChunksAddressAddress *address)
{
	ChunksErrorError error = {.tag = CHUNKS_ERROR_ERROR_MISSING};

	error.u.missing.f0 = *address;
	return error;
}

/* txn_blobs_Cut_Default_default is Cut's Default: no chunks. */
void
txn_blobs_Cut_Default_default(TxnBlobsCut *result)
{
	std_vec_new(&result->chunks, sizeof(txn_TupleAddressVecU8_t), _Alignof(txn_TupleAddressVecU8_t));
}

/* txn_blobs_Cut_drop is Cut's drop glue: its chunks' bytes and their vector. */
void
txn_blobs_Cut_drop(TxnBlobsCut *self)
{
	for (size_t k = 0; k < self->chunks.len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&self->chunks, txn_TupleAddressVecU8_t, k).f1, 1, 1);
	std_vec_free_Checked(&self->chunks, sizeof(txn_TupleAddressVecU8_t), _Alignof(txn_TupleAddressVecU8_t));
}

/* txn_blobs_Cut_NodeStore_as_dyn is Cut's as_dyn: the cut itself. */
static TreesStoreNodeStore
txn_blobs_Cut_NodeStore_as_dyn(void *self)
{
	return TXN_BLOBS_CUT_NODE_STORE(self);
}

/* txn_blobs_Cut_NodeStore_read is Cut's read: a cut reads no chunk. */
static bool
txn_blobs_Cut_NodeStore_read(void *self, HostIoIo *io, const ChunksAddressAddress *address,
							 ChunksTrailerLocator locator, TreesNodeFlat *result, ChunksErrorError *error)
{
	*error = txn_blobs_unread(address);
	return false;
}

/* txn_blobs_Cut_NodeStore_read_record is Cut's read_record: a cut reads no chunk. */
static bool
txn_blobs_Cut_NodeStore_read_record(void *self, HostIoIo *io, const ChunksAddressAddress *address,
									ChunksTrailerLocator locator, ChunksChunkChunk *result, ChunksErrorError *error)
{
	*error = txn_blobs_unread(address);
	return false;
}

/* txn_blobs_Cut_NodeStore_write keeps a chunk by its address, its locator the default of a chunk no pack holds yet; it takes the hashed bytes. */
static bool
txn_blobs_Cut_NodeStore_write(void *self_, HostIoIo *io, StdVec *hashed, const ChunksTrailerLocator *locators,
							  size_t locators_len, const ChunksTrailerStamps *stamps, TreesStoreTreeRef *result,
							  ChunksErrorError *error)
{
	TxnBlobsCut *self = self_;
	ChunksChunkChunk chunk;
	txn_TupleAddressVecU8_t kept;

	chunks_chunk_Chunk_stored(hashed->ptr, hashed->len, locators, locators_len, stamps, &chunk);
	kept.f0 = chunks_chunk_Chunk_address(&chunk);
	chunks_chunk_Chunk_drop(&chunk);
	if (!std_vec_try_reserve(&self->chunks, 1, sizeof(kept), _Alignof(txn_TupleAddressVecU8_t)))
	{
		std_vec_free_Checked(hashed, 1, 1);
		error->tag = CHUNKS_ERROR_ERROR_HOST;
		error->u.host = host_Error_out_of_memory();
		return false;
	}
	kept.f1 = *hashed;
	std_vec_push_Checked(&self->chunks, &kept, sizeof(kept), _Alignof(txn_TupleAddressVecU8_t));
	*result = trees_store_TreeRef_written(kept.f0, chunks_trailer_Locator_Default_default());
	return true;
}

const TreesStoreNodeStoreOps TXN_BLOBS_CUT_NODE_STORE_OPS = {
	.read = txn_blobs_Cut_NodeStore_read,
	.read_record = txn_blobs_Cut_NodeStore_read_record,
	.write = txn_blobs_Cut_NodeStore_write,
	.as_dyn = txn_blobs_Cut_NodeStore_as_dyn,
	.prefetch = trees_store_NodeStore_prefetch,
	.locate = trees_store_NodeStore_locate,
};

/* txn_blobs_Blobs_new returns a reader of a repository's blob trees over its packs' reader, through its working state's delta when one is published. */
bool
txn_blobs_Blobs_new(HostIoIo *io, uint32 repository, TreesStoreNodeStore packs, TxnBlobsBlobs *result,
					HostError *error)
{
	result->packs = packs;
	return txn_memtable_branch_find(HOST_SHARED_IO_WITHIN(io), repository, &result->has_deltas, &result->deltas,
									error);
}

/* txn_blobs_Blobs_drop is Blobs' drop glue: its working state's delta, when it holds one. */
void
txn_blobs_Blobs_drop(TxnBlobsBlobs *self)
{
	if (self->has_deltas)
		host_shared_Held_drop(&self->deltas);
}

/* txn_blobs_Blobs_NodeStore_as_dyn is Blobs' as_dyn: the reader itself. */
static TreesStoreNodeStore
txn_blobs_Blobs_NodeStore_as_dyn(void *self)
{
	return TXN_BLOBS_BLOBS_NODE_STORE(self);
}

/* txn_blobs_Blobs_read_frame_0_t is Blobs' read's state across its pad: the delta's chunk bytes and the chunk stored of them, while live. */
typedef struct txn_blobs_Blobs_read_frame_0_t
{
	TxnBlobsBlobs *self;
	HostIoIo   *io;
	const ChunksAddressAddress *address;
	ChunksTrailerLocator locator;
	TreesNodeFlat *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		hashed;
	bool		hashed_live;
	ChunksChunkChunk chunk;
	bool		chunk_live;
} txn_blobs_Blobs_read_frame_0_t;

/* txn_blobs_Blobs_read_pad_0 is Blobs' read's pad: the chunk, then its bytes, while live. */
static void
txn_blobs_Blobs_read_pad_0(void *frame_)
{
	txn_blobs_Blobs_read_frame_0_t *frame = frame_;

	if (frame->chunk_live)
	{
		frame->chunk_live = false;
		chunks_chunk_Chunk_drop(&frame->chunk);
	}
	if (frame->hashed_live)
	{
		frame->hashed_live = false;
		std_vec_free_Checked(&frame->hashed, 1, 1);
	}
}

/* txn_blobs_Blobs_read_moved refuses as corrupt a delta's chunk whose bytes give another address. */
static ChunksErrorError
txn_blobs_Blobs_read_moved(const ChunksAddressAddress *address)
{
	StdString text;
	StdString	message;
	static const char before[] = "the delta's chunk at ";
	static const char after[] = " has another address";

	std_to_string(address, chunks_address_Address_Display_fmt, &text);
	std_string_from_Checked(&message, before, strlen(before));
	std_vec_extend_from_slice_Checked(&message, text.ptr, text.len, 1, 1);
	std_vec_free_Checked(&text, 1, 1);
	std_vec_extend_from_slice_Checked(&message, after, strlen(after), 1, 1);
	return chunks_error_Error_corrupt(&message);
}

/* txn_blobs_Blobs_read_body_0 is Blobs' read's body. */
static void
txn_blobs_Blobs_read_body_0(void *frame_)
{
	txn_blobs_Blobs_read_frame_0_t *frame = frame_;
	TxnBlobsBlobs *self = frame->self;
	bool		some = false;
	HostError	failed;
	ChunksTrailerStamps stamps;
	ChunksAddressAddress stored;

	if (!chunks_trailer_Locator_is_known(&frame->locator) && self->has_deltas)
	{
		if (!txn_spill_chunk(frame->io, host_shared_Held_record(&self->deltas), frame->address->f0,
							 sizeof(frame->address->f0), &some, &frame->hashed, &failed))
		{
			frame->error->tag = CHUNKS_ERROR_ERROR_HOST;
			frame->error->u.host = failed;
			return;
		}
	}
	if (!some)
	{
		frame->ok = self->packs.ops->read(self->packs.self, frame->io, frame->address, frame->locator, frame->result,
										  frame->error);
		return;
	}
	frame->hashed_live = true;
	stamps = chunks_trailer_Stamps_Default_default();
	chunks_chunk_Chunk_stored(frame->hashed.ptr, frame->hashed.len, NULL, 0, &stamps, &frame->chunk);
	frame->chunk_live = true;
	stored = chunks_chunk_Chunk_address(&frame->chunk);
	if (!chunks_address_Address_PartialEq_eq(&stored, frame->address))
	{
		*frame->error = txn_blobs_Blobs_read_moved(frame->address);
		txn_blobs_Blobs_read_pad_0(frame);
		return;
	}
	frame->ok = trees_node_Flat_of(&frame->chunk, frame->result, frame->error);
	txn_blobs_Blobs_read_pad_0(frame);
}

/* txn_blobs_Blobs_NodeStore_read reads a chunk with a known locator from its pack, and any other from the working state's delta or its chunk spills, checked against its address, and then the packs. */
static bool
txn_blobs_Blobs_NodeStore_read(void *self, HostIoIo *io, const ChunksAddressAddress *address,
							   ChunksTrailerLocator locator, TreesNodeFlat *result, ChunksErrorError *error)
{
	txn_blobs_Blobs_read_frame_0_t frame = {.self = self, .io = io, .address = address, .locator = locator,
	.result = result, .error = error};

	std_landing(&frame, txn_blobs_Blobs_read_body_0, txn_blobs_Blobs_read_pad_0);
	return frame.ok;
}

/* txn_blobs_Blobs_NodeStore_read_record reads a chunk's record from the packs. */
static bool
txn_blobs_Blobs_NodeStore_read_record(void *self_, HostIoIo *io, const ChunksAddressAddress *address,
									  ChunksTrailerLocator locator, ChunksChunkChunk *result, ChunksErrorError *error)
{
	TxnBlobsBlobs *self = self_;

	return self->packs.ops->read_record(self->packs.self, io, address, locator, result, error);
}

/* txn_blobs_Blobs_NodeStore_locate finds a chunk's locator in the packs. */
static bool
txn_blobs_Blobs_NodeStore_locate(void *self_, HostIoIo *io, const ChunksAddressAddress *address, bool *some,
								 ChunksTrailerLocator *result, ChunksErrorError *error)
{
	TxnBlobsBlobs *self = self_;

	return self->packs.ops->locate(self->packs.self, io, address, some, result, error);
}

/* txn_blobs_Blobs_NodeStore_write refuses a write: a reader writes no tree node. It takes the hashed bytes. */
static bool
txn_blobs_Blobs_NodeStore_write(void *self, HostIoIo *io, StdVec *hashed, const ChunksTrailerLocator *locators,
								size_t locators_len, const ChunksTrailerStamps *stamps, TreesStoreTreeRef *result,
								ChunksErrorError *error)
{
	std_vec_free_Checked(hashed, 1, 1);
	*error = chunks_error_Error_corrupt_RefStr("a reader cannot write tree nodes",
											   strlen("a reader cannot write tree nodes"));
	return false;
}

const TreesStoreNodeStoreOps TXN_BLOBS_BLOBS_NODE_STORE_OPS = {
	.read = txn_blobs_Blobs_NodeStore_read,
	.read_record = txn_blobs_Blobs_NodeStore_read_record,
	.write = txn_blobs_Blobs_NodeStore_write,
	.as_dyn = txn_blobs_Blobs_NodeStore_as_dyn,
	.prefetch = trees_store_NodeStore_prefetch,
	.locate = txn_blobs_Blobs_NodeStore_locate,
};

/* txn_blobs_kept_frame_0_t is kept's state across its pad: the hold and the key's versions, while live. */
typedef struct txn_blobs_kept_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord deltas;
	const uint8 *key;
	size_t		key_len;
	bool		logged;
	bool	   *result;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	/* versions is a Vec<Version>. */
	StdVec		versions;
	bool		versions_live;
	StdVec		spilled;
	bool		spilled_live;
} txn_blobs_kept_frame_0_t;

/* txn_blobs_kept_pad_0 is kept's pad: the versions, then the hold, while live. */
static void
txn_blobs_kept_pad_0(void *frame_)
{
	txn_blobs_kept_frame_0_t *frame = frame_;

	if (frame->spilled_live)
	{
		frame->spilled_live = false;
		txn_memtable_version_versions_drop(&frame->spilled, 0);
	}
	if (frame->versions_live)
	{
		frame->versions_live = false;
		txn_memtable_version_versions_drop(&frame->versions, 0);
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
}

/* txn_blobs_kept_body_0 is kept's body. */
static void
txn_blobs_kept_body_0(void *frame_)
{
	txn_blobs_kept_frame_0_t *frame = frame_;
	TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;

	*frame->result = false;
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_table_chain(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, frame->io, &frame->hold, frame->deltas,
								  frame->key, frame->key_len, &frame->versions, frame->error))
	{
		txn_blobs_kept_pad_0(frame);
		return;
	}
	frame->versions_live = true;
	if (!txn_spill_chunk_versions(frame->io, &frame->hold, frame->deltas, frame->key, frame->key_len,
								  &frame->spilled, frame->error))
	{
		txn_blobs_kept_pad_0(frame);
		return;
	}
	frame->spilled_live = true;
	std_vec_extend_from_slice_Checked(&frame->versions, frame->spilled.ptr, frame->spilled.len,
									  sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	frame->spilled_live = false;
	std_vec_free_Checked(&frame->spilled, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	for (size_t k = 0; k < frame->versions.len && !*frame->result; k++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&STD_VEC_AT(&frame->versions,
																						  TxnMemtableVersionVersion,
																						  k));
		TxnMemtableVersionWriter writer;
		uint64		lsn;
		HostXid		xid;

		if (frame->logged && !txn_memtable_version_Record_lsn(&record, &lsn))
			continue;
		writer = txn_memtable_version_Record_writer(&record);
		xid.f0 = writer.xid;
		*frame->result = txn_memtable_version_Writer_PartialEq_eq(&writer, &none) ||
			host_is_current(HOST_IO_IO_IO_DOORS(frame->io), xid);
	}
	frame->ok = true;
	txn_blobs_kept_pad_0(frame);
}

/*
 * txn_blobs_kept reports whether the delta or the repository's chunk spills
 * hold a version of the chunk under a key that a write may name in place of
 * its own, one with a WAL record's end for a logged write: a built node's,
 * or one of the current transaction that can't roll back without the write.
 */
static bool
txn_blobs_kept(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len, bool logged, bool *result,
			   HostError *error)
{
	txn_blobs_kept_frame_0_t frame = {.io = io, .deltas = deltas, .key = key, .key_len = key_len, .logged = logged,
	.result = result, .error = error};

	std_landing(&frame, txn_blobs_kept_body_0, txn_blobs_kept_pad_0);
	return frame.ok;
}

/* txn_blobs_written_closure_captures_t is written's prepare's closure's captures: the writer, the chunk's bytes it takes, and its record's body, if logged. */
typedef struct txn_blobs_written_closure_captures_t
{
	TxnMemtableVersionWriter writer;
	StdVec	   *bytes;
	const StdVec *body;
} txn_blobs_written_closure_captures_t;

/* txn_blobs_written_closure_frame_0_t is written's prepare's closure's state across its pad: the checkpoint window, while live. */
typedef struct txn_blobs_written_closure_frame_0_t
{
	txn_blobs_written_closure_captures_t *captures;
	HostIoIo   *io;
	TxnMemtableTablePreparedBranchDeltas *prepared;
	HostError  *error;
	bool		ok;
	HostDelayCheckpoint window;
	bool		window_live;
} txn_blobs_written_closure_frame_0_t;

/* txn_blobs_written_closure_pad_0 is written's prepare's closure's pad: the window, while live. */
static void
txn_blobs_written_closure_pad_0(void *frame_)
{
	txn_blobs_written_closure_frame_0_t *frame = frame_;

	if (frame->window_live)
	{
		frame->window_live = false;
		host_DelayCheckpoint_drop(&frame->window);
	}
}

/* txn_blobs_written_closure_body_0 pushes the chunk's version in a checkpoint window, after its record when logged. */
static void
txn_blobs_written_closure_body_0(void *frame_)
{
	txn_blobs_written_closure_frame_0_t *frame = frame_;
	txn_blobs_written_closure_captures_t *c = frame->captures;
	bool		has_lsn = c->body != NULL;
	HostLsn		lsn = 0;
	TxnMemtableVersionHandle handle;

	if (!host_delay_checkpoint(HOST_IO_IO_IO_DOORS(frame->io), NULL, 0, &frame->window, frame->error))
		return;
	frame->window_live = true;
	if ((has_lsn && !host_wal_insert(HOST_IO_IO_IO_DOORS(frame->io), HOST_WAL_KIND_CHUNK, c->body->ptr, c->body->len,
									 &lsn, frame->error)) ||
		!txn_memtable_branch_finish(frame->io, frame->prepared, c->writer, c->bytes, has_lsn, lsn, &handle,
									frame->error))
	{
		txn_blobs_written_closure_pad_0(frame);
		return;
	}
	frame->window_live = false;
	host_DelayCheckpoint_finish(&frame->window);
	frame->ok = true;
	txn_blobs_written_closure_pad_0(frame);
}

/* txn_blobs_written_closure_1 is written's prepare's closure. */
static bool
txn_blobs_written_closure_1(void *captures, HostIoIo *io, TxnMemtableTablePreparedBranchDeltas *prepared,
							HostError *error)
{
	txn_blobs_written_closure_frame_0_t frame = {.captures = captures, .io = io, .prepared = prepared,
	.error = error};

	std_landing(&frame, txn_blobs_written_closure_body_0, txn_blobs_written_closure_pad_0);
	return frame.ok;
}

/* txn_blobs_written_frame_0_t is written's state across its pad: the chunks it takes, from the one at moved, the chunk's bytes, and its record's body, while live. */
typedef struct txn_blobs_written_frame_0_t
{
	HostIoIo   *io;
	const HostSharedHeld *deltas;
	uint32		repository;
	TxnMemtableVersionWriter writer;
	StdVec		chunks;
	bool		logged;
	bool		(*packed) (void *captures, HostIoIo *io, const ChunksAddressAddress *address, bool *result,
						   HostError *error);
	void	   *packed_captures;
	void		(*packed_drop) (void *captures);
	HostError  *error;
	bool		ok;
	bool		chunks_live;
	size_t		moved;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		body;
	bool		body_live;
} txn_blobs_written_frame_0_t;

/* txn_blobs_written_pad_0 is written's pad: the body, the bytes, the chunks left, and the packed closure, while live. */
static void
txn_blobs_written_pad_0(void *frame_)
{
	txn_blobs_written_frame_0_t *frame = frame_;

	if (frame->body_live)
	{
		frame->body_live = false;
		std_vec_free_Checked(&frame->body, 1, 1);
	}
	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	if (frame->chunks_live)
	{
		frame->chunks_live = false;
		for (size_t k = frame->moved; k < frame->chunks.len; k++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->chunks, txn_TupleAddressVecU8_t, k).f1, 1, 1);
		std_vec_free_Checked(&frame->chunks, sizeof(txn_TupleAddressVecU8_t), _Alignof(txn_TupleAddressVecU8_t));
	}
	if (frame->packed_drop != NULL)
	{
		void		(*drop) (void *captures) = frame->packed_drop;

		frame->packed_drop = NULL;
		drop(frame->packed_captures);
	}
}

/* txn_blobs_written_chunk writes one chunk into the delta, unless the delta or the packs keep it already. */
static bool
txn_blobs_written_chunk(txn_blobs_written_frame_0_t *frame, const ChunksAddressAddress *address)
{
	HostSharedRecord record = host_shared_Held_record(frame->deltas);
	HostStorage storage = txn_spill_chunks_storage(frame->repository);
	HostXid		top = {frame->writer.has_top ? frame->writer.top : frame->writer.xid};
	bool		skip;
	size_t		need;
	uint64		touched;
	txn_blobs_written_closure_captures_t captures = {frame->writer, &frame->bytes, NULL};

	if (!txn_blobs_kept(frame->io, record, address->f0, sizeof(address->f0), frame->logged, &skip, frame->error) ||
		(!skip && !frame->packed(frame->packed_captures, frame->io, address, &skip, frame->error)))
		return false;
	if (skip)
		return true;
	need = txn_memtable_version_largest(frame->bytes.len);
	if (!txn_written_touched(frame->io, top.f0, &storage, TXN_WRITTEN_OF_CHUNKS, address->f0, sizeof(address->f0),
							 need, &touched, frame->error))
		return false;
	if (touched >= txn_spill_threshold())
	{
		bool		spilled;

		if (!txn_spill_spilled(frame->io, &storage, TXN_WRITTEN_OF_CHUNKS, top.f0, &spilled, frame->error))
			return false;
	}
	if (frame->logged)
	{
		TxnRecordChunkWrite write = {.repository = frame->repository, .address = *address, .bytes = frame->bytes,
		.has_writer = true, .writer = frame->writer};

		write.writer.command = 0;
		txn_record_ChunkWrite_encode(&write, &frame->body);
		frame->body_live = true;
		captures.body = &frame->body;
	}
	return txn_memtable_branch_prepare(frame->io, record, address->f0, sizeof(address->f0), need,
									   txn_blobs_written_closure_1, &captures, NULL, frame->error);
}

/* txn_blobs_written_body_0 is written's body. */
static void
txn_blobs_written_body_0(void *frame_)
{
	txn_blobs_written_frame_0_t *frame = frame_;

	while (frame->moved < frame->chunks.len)
	{
		txn_TupleAddressVecU8_t *chunk = &STD_VEC_AT(&frame->chunks, txn_TupleAddressVecU8_t, frame->moved);
		ChunksAddressAddress address = chunk->f0;

		frame->bytes = chunk->f1;
		frame->bytes_live = true;
		frame->moved++;
		if (!txn_blobs_written_chunk(frame, &address))
		{
			txn_blobs_written_pad_0(frame);
			return;
		}
		if (frame->body_live)
		{
			frame->body_live = false;
			std_vec_free_Checked(&frame->body, 1, 1);
		}
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	frame->ok = true;
	txn_blobs_written_pad_0(frame);
}

/*
 * txn_blobs_written writes a blob's chunks into a repository's working
 * state's delta under their writer, each in its own prepare: a logged
 * storage's chunk in a checkpoint window with its record, and an unlogged or
 * temporary storage's chunk with no record. It passes over a chunk the packs
 * hold or one its own transaction wrote that can't roll back without the
 * write, durable for a logged write. Each chunk it writes counts in its
 * transaction's written list as the repository's chunk tree. It takes the
 * chunks and the packed closure.
 */
bool
txn_blobs_written(HostIoIo *io, const HostSharedHeld *deltas, uint32 repository, TxnMemtableVersionWriter writer,
				  StdVec *chunks, bool logged,
				  bool (*packed) (void *captures, HostIoIo *io, const ChunksAddressAddress *address, bool *result,
								  HostError *error),
				  void *packed_captures, void (*packed_drop) (void *captures), HostError *error)
{
	txn_blobs_written_frame_0_t frame = {.io = io, .deltas = deltas, .repository = repository, .writer = writer,
		.chunks = *chunks, .logged = logged, .packed = packed, .packed_captures = packed_captures,
	.packed_drop = packed_drop, .error = error, .chunks_live = true};

	std_landing(&frame, txn_blobs_written_body_0, txn_blobs_written_pad_0);
	return frame.ok;
}

/* txn_blobs_redo_closure_captures_t is redo's prepare's closure's captures: the writer, the piece's bytes it takes, and the record's end. */
typedef struct txn_blobs_redo_closure_captures_t
{
	TxnMemtableVersionWriter writer;
	StdVec	   *bytes;
	uint64		lsn;
} txn_blobs_redo_closure_captures_t;

/* txn_blobs_redo_closure_0 is redo's prepare's closure: the piece's version pushed at the record's end. */
static bool
txn_blobs_redo_closure_0(void *captures, HostIoIo *io, TxnMemtableTablePreparedBranchDeltas *prepared,
						 HostError *error)
{
	txn_blobs_redo_closure_captures_t *c = captures;
	TxnMemtableVersionHandle handle;

	return txn_memtable_branch_finish(io, prepared, c->writer, c->bytes, true, c->lsn, &handle, error);
}

/* txn_blobs_redo_frame_0_t is redo's state across its pad: the pieces, from the one at moved, while live. */
typedef struct txn_blobs_redo_frame_0_t
{
	HostIoIo   *io;
	const HostSharedHeld *deltas;
	TxnRecordChunkWrite chunk;
	uint64		lsn;
	HostError  *error;
	bool		ok;
	/* pieces is a Vec<(Vec<u8>, Vec<u8>)>. */
	StdVec		pieces;
	bool		pieces_live;
	size_t		moved;
} txn_blobs_redo_frame_0_t;

/* txn_blobs_redo_pad_0 is redo's pad: the pieces left, while live. */
static void
txn_blobs_redo_pad_0(void *frame_)
{
	txn_blobs_redo_frame_0_t *frame = frame_;

	if (frame->pieces_live)
	{
		frame->pieces_live = false;
		for (size_t k = frame->moved; k < frame->pieces.len; k++)
		{
			txn_TupleVecU8VecU8_t *piece = &STD_VEC_AT(&frame->pieces, txn_TupleVecU8VecU8_t, k);

			std_vec_free_Checked(&piece->f0, 1, 1);
			std_vec_free_Checked(&piece->f1, 1, 1);
		}
		std_vec_free_Checked(&frame->pieces, sizeof(txn_TupleVecU8VecU8_t), _Alignof(txn_TupleVecU8VecU8_t));
	}
}

/* txn_blobs_redo_piece pushes one piece unless the delta holds its version already. */
static bool
txn_blobs_redo_piece(txn_blobs_redo_frame_0_t *frame, TxnMemtableVersionWriter writer, txn_TupleVecU8VecU8_t *piece)
{
	HostSharedRecord record = host_shared_Held_record(frame->deltas);
	TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
	bool		blob = !txn_memtable_version_Writer_PartialEq_eq(&writer, &none);
	bool		held;
	size_t		need;
	txn_blobs_redo_closure_captures_t captures = {writer, &piece->f1, frame->lsn};

	if (!(blob ? txn_memtable_branch_holds(frame->io, record, piece->f0.ptr, piece->f0.len, frame->lsn, &held,
										   frame->error) :
		  txn_memtable_branch_durable(frame->io, record, piece->f0.ptr, piece->f0.len, &held, frame->error)))
		return false;
	if (held)
		return true;
	need = txn_memtable_version_largest(piece->f1.len);
	if (blob)
	{
		HostStorage storage = txn_spill_chunks_storage(frame->chunk.repository);
		uint64		top = writer.has_top ? writer.top : writer.xid;
		uint64		replayed;
		bool		spilled;

		if (!txn_spill_replayed(frame->io, &storage, TXN_WRITTEN_OF_CHUNKS, top, need, &replayed, frame->error) ||
			(replayed >= txn_spill_threshold() &&
			 !txn_spill_spilled(frame->io, &storage, TXN_WRITTEN_OF_CHUNKS, top, &spilled, frame->error)))
			return false;
	}
	return txn_memtable_branch_prepare(frame->io, record, piece->f0.ptr, piece->f0.len, need, txn_blobs_redo_closure_0,
									   &captures, NULL, frame->error);
}

/* txn_blobs_redo_body_0 is redo's body. */
static void
txn_blobs_redo_body_0(void *frame_)
{
	txn_blobs_redo_frame_0_t *frame = frame_;
	TxnMemtableVersionWriter writer = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
	uint32		repository = frame->chunk.repository;

	if (frame->chunk.has_writer)
		writer = frame->chunk.writer;
	if (!txn_record_ChunkWrite_pieces(&frame->chunk, &frame->pieces, frame->error))
		return;
	frame->chunk.repository = repository;
	frame->pieces_live = true;
	while (frame->moved < frame->pieces.len)
	{
		txn_TupleVecU8VecU8_t *piece = &STD_VEC_AT(&frame->pieces, txn_TupleVecU8VecU8_t, frame->moved);
		bool		pushed = txn_blobs_redo_piece(frame, writer, piece);

		std_vec_free_Checked(&piece->f0, 1, 1);
		std_vec_free_Checked(&piece->f1, 1, 1);
		frame->moved++;
		if (!pushed)
		{
			txn_blobs_redo_pad_0(frame);
			return;
		}
	}
	frame->ok = true;
	txn_blobs_redo_pad_0(frame);
}

/*
 * txn_blobs_redo pushes a replayed chunk's logged version into its
 * repository's working state's delta, with no catalog access, each piece of
 * a built node's on its own unless the delta holds a logged version of that
 * key already, and a blob's chunk under its writer unless the delta holds
 * the version this record pushed; a blob's chunk counts toward its
 * transaction's replayed bytes. It takes the chunk.
 */
bool
txn_blobs_redo(HostIoIo *io, const HostSharedHeld *deltas, TxnRecordChunkWrite chunk, uint64 lsn, HostError *error)
{
	txn_blobs_redo_frame_0_t frame = {.io = io, .deltas = deltas, .chunk = chunk, .lsn = lsn, .error = error};

	std_landing(&frame, txn_blobs_redo_body_0, txn_blobs_redo_pad_0);
	return frame.ok;
}
