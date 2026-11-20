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
 * The C port of txn's memtable/branch.rs: a working state's delta of chunks,
 * the chunks of the blob trees and the overflow chunks its transactions
 * write, in a memtable keyed by each chunk's address, a built base's node's
 * by node_key, beside its identity tree's delta, the pairs of object ids and
 * Postgres' numbers its transactions' DDL writes, in a memtable keyed by the
 * identity tree's keys. Its word is in a pooled record found by its working
 * state, the storage base's one for each repository.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/delta.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/index.h"
#include "doltgres/txn/memtable/pages.h"
#include "doltgres/txn/memtable/pins.h"
#include "doltgres/txn/memtable/range.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/state.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "memtable" STD_SEP "branch.rs"

/* TXN_MEMTABLE_BRANCH_HEAD is HEAD, the length of a built node's first piece's head: its writer's full xid and its value's length. */
#define TXN_MEMTABLE_BRANCH_HEAD 12

/* The delta's atomics only this file reads: its repository, its identity memtable's word, and its own tablespace. */
#define TXN_MEMTABLE_BRANCH_REPOSITORY 0
#define TXN_MEMTABLE_BRANCH_IDENTITY_WORD 2
#define TXN_MEMTABLE_BRANCH_OWN 6

/* txn_memtable_branch_internal returns an internal error of a working state's delta. */
static HostError
txn_memtable_branch_internal(const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* txn_memtable_branch_corrupt returns a data corruption error of a working state's delta. */
static HostError
txn_memtable_branch_corrupt(const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) "XX001", &message);
}

/*
 * txn_memtable_branch_node_key returns the key a working state's delta holds
 * a built base's tree node under: its address, then the base's storage's
 * tablespace and relation and its fold point, so that the nodes of one
 * address two built bases logged are held apart.
 */
void
txn_memtable_branch_node_key(const uint8 *address, uint32 tablespace, uint32 relation, uint64 point, uint8 *result)
{
	memcpy(result, address, TXN_MEMTABLE_BRANCH_ADDRESS);
	std_u32_to_be_bytes(tablespace, result + TXN_MEMTABLE_BRANCH_ADDRESS);
	std_u32_to_be_bytes(relation, result + TXN_MEMTABLE_BRANCH_ADDRESS + 4);
	std_u64_to_be_bytes(point, result + TXN_MEMTABLE_BRANCH_ADDRESS + 8);
}

/* txn_memtable_branch_piece_key returns the key of a built node's piece of a number. */
static void
txn_memtable_branch_piece_key(const uint8 *node, uint16 piece, uint8 *result)
{
	memcpy(result, node, TXN_MEMTABLE_BRANCH_NODE_KEY);
	std_u16_to_be_bytes(piece, result + TXN_MEMTABLE_BRANCH_NODE_KEY);
}

/*
 * txn_memtable_branch_first_piece returns the node's key a key names when it
 * is a built node's first piece's, which a walk of the delta reads a node
 * by, and nothing for a blob's chunk's key or a later piece's.
 */
bool
txn_memtable_branch_first_piece(const uint8 *key, size_t key_len, uint8 *result)
{
	if (key_len != TXN_MEMTABLE_BRANCH_PIECE_KEY || key[TXN_MEMTABLE_BRANCH_NODE_KEY] != 0 ||
		key[TXN_MEMTABLE_BRANCH_NODE_KEY + 1] != 0)
		return false;
	memcpy(result, key, TXN_MEMTABLE_BRANCH_NODE_KEY);
	return true;
}

/* txn_memtable_branch_Pieces_drop drops a Vec<Piece>'s pieces and its buffer. */
static void
txn_memtable_branch_Pieces_drop(StdVec *pieces)
{
	for (size_t k = 0; k < pieces->len; k++)
	{
		txn_TupleVecU8VecU8_t *piece = &STD_VEC_AT(pieces, txn_TupleVecU8VecU8_t, k);

		std_vec_free_Checked(&piece->f0, 1, 1);
		std_vec_free_Checked(&piece->f1, 1, 1);
	}
	std_vec_free_Checked(pieces, sizeof(txn_TupleVecU8VecU8_t), _Alignof(txn_TupleVecU8VecU8_t));
}

/* txn_memtable_branch_pieces_frame_0_t is pieces' state across its pad: the pieces built. */
typedef struct txn_memtable_branch_pieces_frame_0_t
{
	const uint8 *node;
	uint64		writer;
	const uint8 *value;
	size_t		value_len;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	bool		result_live;
	/* piece is the piece being built, its key and its bytes each while live. */
	txn_TupleVecU8VecU8_t piece;
	bool		key_live;
	bool		bytes_live;
} txn_memtable_branch_pieces_frame_0_t;

/* txn_memtable_branch_pieces_pad_0 is pieces' pad: the piece being built and the pieces built, while live. */
static void
txn_memtable_branch_pieces_pad_0(void *frame_)
{
	txn_memtable_branch_pieces_frame_0_t *frame = frame_;

	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->piece.f1, 1, 1);
	}
	if (frame->key_live)
	{
		frame->key_live = false;
		std_vec_free_Checked(&frame->piece.f0, 1, 1);
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		txn_memtable_branch_Pieces_drop(frame->result);
	}
}

/* txn_memtable_branch_pieces_push pushes a piece of a number and its bytes, whose room is reserved. */
static void
txn_memtable_branch_pieces_push(txn_memtable_branch_pieces_frame_0_t *frame, uint16 number, const uint8 *head,
								size_t head_len, const uint8 *part, size_t part_len)
{
	uint8		key[TXN_MEMTABLE_BRANCH_PIECE_KEY];

	txn_memtable_branch_piece_key(frame->node, number, key);
	std_vec_new(&frame->piece.f0, 1, 1);
	frame->key_live = true;
	std_vec_extend_from_slice_Checked(&frame->piece.f0, key, sizeof(key), 1, 1);
	std_vec_new(&frame->piece.f1, 1, 1);
	frame->bytes_live = true;
	std_vec_extend_from_slice_Checked(&frame->piece.f1, head, head_len, 1, 1);
	std_vec_extend_from_slice_Checked(&frame->piece.f1, part, part_len, 1, 1);
	std_vec_push_Checked(frame->result, &frame->piece, sizeof(frame->piece), _Alignof(txn_TupleVecU8VecU8_t));
	frame->key_live = false;
	frame->bytes_live = false;
}

/* txn_memtable_branch_pieces_body_0 is pieces' body. */
static void
txn_memtable_branch_pieces_body_0(void *frame_)
{
	txn_memtable_branch_pieces_frame_0_t *frame = frame_;
	size_t		first = Min(frame->value_len, TXN_MEMTABLE_BRANCH_PIECE - TXN_MEMTABLE_BRANCH_HEAD);
	size_t		rest = frame->value_len - first;
	uint8		head[TXN_MEMTABLE_BRANCH_HEAD];
	size_t		at = 0;

	if (frame->value_len > PG_UINT32_MAX)
	{
		*frame->error = txn_memtable_branch_internal("a built node's value is too long for its head");
		return;
	}
	std_vec_new(frame->result, sizeof(txn_TupleVecU8VecU8_t), _Alignof(txn_TupleVecU8VecU8_t));
	frame->result_live = true;
	if (!std_vec_try_reserve(frame->result,
							 STD_ADD(usize, 1,
									 std_div_ceil_usize(rest, TXN_MEMTABLE_BRANCH_PIECE, RUST_FILE,
														115, 28),
									 115, 24),
							 sizeof(txn_TupleVecU8VecU8_t), _Alignof(txn_TupleVecU8VecU8_t)))
	{
		*frame->error = host_Error_out_of_memory();
		txn_memtable_branch_pieces_pad_0(frame);
		return;
	}
	std_u64_to_be_bytes(frame->writer, head);
	std_u32_to_be_bytes((uint32) frame->value_len, head + 8);
	txn_memtable_branch_pieces_push(frame, 0, head, sizeof(head), frame->value, first);
	for (size_t from = first; from < frame->value_len; from += TXN_MEMTABLE_BRANCH_PIECE)
	{
		size_t		number = STD_ADD(usize, at, 1, 119, 35);

		if (number > PG_UINT16_MAX)
		{
			*frame->error = txn_memtable_branch_internal("a built node has too many pieces");
			txn_memtable_branch_pieces_pad_0(frame);
			return;
		}
		txn_memtable_branch_pieces_push(frame, (uint16) number, NULL, 0, frame->value + from,
										Min(frame->value_len - from, TXN_MEMTABLE_BRANCH_PIECE));
		at = number;
	}
	frame->result_live = false;
	frame->ok = true;
}

/*
 * txn_memtable_branch_pieces returns a built node's value, its bytes and the
 * trailer of its stamps, as the delta keeps it under the node's key: pieces
 * of at most PIECE bytes, the first starting with its writer's full xid and
 * the value's length, each with its key. A value whose length or count of
 * pieces its head can't hold is an internal error.
 */
bool
txn_memtable_branch_pieces(const uint8 *node, uint64 writer, const uint8 *value, size_t value_len, StdVec *result,
						   HostError *error)
{
	txn_memtable_branch_pieces_frame_0_t frame = {.node = node, .writer = writer, .value = value,
	.value_len = value_len, .result = result, .error = error};

	std_landing(&frame, txn_memtable_branch_pieces_body_0, txn_memtable_branch_pieces_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_branch_head splits a built node's first piece into its
 * writer's full xid, its value's length, and the value's first part, at an
 * offset of the piece.
 */
static bool
txn_memtable_branch_head(const StdVec *first, uint64 *writer, size_t *length, HostError *error)
{
	if (first->len < TXN_MEMTABLE_BRANCH_HEAD)
	{
		*error = txn_memtable_branch_corrupt("a built node's first piece holds no head");
		return false;
	}
	*writer = std_u64_from_be_bytes(first->ptr);
	*length = (size_t) std_u32_from_be_bytes((const uint8 *) first->ptr + 8);
	return true;
}

/* txn_memtable_branch_node_frame_0_t is node's state across its pad: the first piece, a later piece, and the value, while live. */
typedef struct txn_memtable_branch_node_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord deltas;
	const uint8 *key;
	bool	   *some;
	txn_TupleU64VecU8_t *result;
	HostError  *error;
	bool		ok;
	StdVec		first;
	bool		first_live;
	StdVec		part;
	bool		part_live;
	StdVec		value;
	bool		value_live;
} txn_memtable_branch_node_frame_0_t;

/* txn_memtable_branch_node_pad_0 is node's pad: the value, a later piece, and the first piece, while live. */
static void
txn_memtable_branch_node_pad_0(void *frame_)
{
	txn_memtable_branch_node_frame_0_t *frame = frame_;

	if (frame->value_live)
	{
		frame->value_live = false;
		std_vec_free_Checked(&frame->value, 1, 1);
	}
	if (frame->part_live)
	{
		frame->part_live = false;
		std_vec_free_Checked(&frame->part, 1, 1);
	}
	if (frame->first_live)
	{
		frame->first_live = false;
		std_vec_free_Checked(&frame->first, 1, 1);
	}
}

/* txn_memtable_branch_node_get reads the piece of a number of the frame's node into a Vec, answering whether the delta holds it. */
static bool
txn_memtable_branch_node_get(txn_memtable_branch_node_frame_0_t *frame, uint16 piece, bool *some, StdVec *result)
{
	uint8		key[TXN_MEMTABLE_BRANCH_PIECE_KEY];

	txn_memtable_branch_piece_key(frame->key, piece, key);
	return txn_memtable_branch_get(frame->io, frame->deltas, key, sizeof(key), some, result, frame->error);
}

/* txn_memtable_branch_node_missing returns the corruption error of a built node's missing piece of a number. */
static HostError
txn_memtable_branch_node_missing(uint16 piece)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("a built node's piece "), STD_FMT_ARG(0), STD_FMT_LIT(" is missing")};
	StdFmtArg	args[] = {{&piece, std_u16_Display_fmt}};
	StdFmtArguments arguments = {pieces, 3, args};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	return host_Error_fork((const uint8 *) "XX001", &message);
}

/* txn_memtable_branch_node_body_0 is node's body. */
static void
txn_memtable_branch_node_body_0(void *frame_)
{
	txn_memtable_branch_node_frame_0_t *frame = frame_;
	uint64		writer;
	size_t		length;
	uint16		piece = 1;
	bool		some;

	*frame->some = false;
	if (!txn_memtable_branch_node_get(frame, 0, &some, &frame->first))
		return;
	if (!some)
	{
		frame->ok = true;
		return;
	}
	frame->first_live = true;
	if (!txn_memtable_branch_head(&frame->first, &writer, &length, frame->error))
	{
		txn_memtable_branch_node_pad_0(frame);
		return;
	}
	std_vec_new(&frame->value, 1, 1);
	frame->value_live = true;
	if (!std_vec_try_reserve(&frame->value, length, 1, 1))
	{
		*frame->error = host_Error_out_of_memory();
		txn_memtable_branch_node_pad_0(frame);
		return;
	}
	std_vec_extend_from_slice_Checked(&frame->value, (const uint8 *) frame->first.ptr + TXN_MEMTABLE_BRANCH_HEAD,
									  frame->first.len - TXN_MEMTABLE_BRANCH_HEAD, 1, 1);
	while (frame->value.len < length)
	{
		if (!txn_memtable_branch_node_get(frame, piece, &some, &frame->part))
		{
			txn_memtable_branch_node_pad_0(frame);
			return;
		}
		if (!some)
		{
			*frame->error = txn_memtable_branch_node_missing(piece);
			txn_memtable_branch_node_pad_0(frame);
			return;
		}
		frame->part_live = true;
		std_vec_extend_from_slice_Checked(&frame->value, frame->part.ptr, frame->part.len, 1, 1);
		frame->part_live = false;
		std_vec_free_Checked(&frame->part, 1, 1);
		if (piece == PG_UINT16_MAX)
		{
			*frame->error = txn_memtable_branch_corrupt("a built node has too many pieces");
			txn_memtable_branch_node_pad_0(frame);
			return;
		}
		piece++;
	}
	if (frame->value.len == length)
	{
		if (!txn_memtable_branch_node_get(frame, piece, &some, &frame->part))
		{
			txn_memtable_branch_node_pad_0(frame);
			return;
		}
		if (some)
			std_vec_free_Checked(&frame->part, 1, 1);
	}
	if (frame->value.len != length || some)
	{
		*frame->error = txn_memtable_branch_corrupt("a built node's pieces pass its length");
		txn_memtable_branch_node_pad_0(frame);
		return;
	}
	frame->result->f0 = writer;
	frame->result->f1 = frame->value;
	frame->value_live = false;
	*frame->some = true;
	frame->ok = true;
	txn_memtable_branch_node_pad_0(frame);
}

/*
 * txn_memtable_branch_node returns a built base's node the delta holds under
 * its key, gathered from its pieces: its writer's full xid and its value,
 * the node's bytes and the trailer of its stamps, or nothing when the delta
 * holds no first piece. A missing, short, or extra piece is data
 * corruption, so that a lost piece never reads as a shorter node.
 */
bool
txn_memtable_branch_node(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, bool *some,
						 txn_TupleU64VecU8_t *result, HostError *error)
{
	txn_memtable_branch_node_frame_0_t frame = {.io = io, .deltas = deltas, .key = key, .some = some,
	.result = result, .error = error};

	std_landing(&frame, txn_memtable_branch_node_body_0, txn_memtable_branch_node_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_branch_node_taken takes every piece of a built base's node out
 * of a working state's delta through the remover, once a pack holds the
 * node under a base that names it or no base will.
 */
bool
txn_memtable_branch_node_taken(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, HostError *error)
{
	uint16		piece = 0;

	for (;;)
	{
		uint8		piece_key[TXN_MEMTABLE_BRANCH_PIECE_KEY];
		StdVec		part;
		bool		some;

		txn_memtable_branch_piece_key(key, piece, piece_key);
		if (!txn_memtable_branch_get(io, deltas, piece_key, sizeof(piece_key), &some, &part, error))
			return false;
		if (!some)
			return true;
		std_vec_free_Checked(&part, 1, 1);
		if (!txn_memtable_branch_taken(io, deltas, piece_key, sizeof(piece_key), error))
			return false;
		if (piece == PG_UINT16_MAX)
			return true;
		piece++;
	}
}

/* txn_memtable_branch_load loads one of a working state's delta's atomics. */
static uint64
txn_memtable_branch_load(HostSharedRecord record, size_t atomic)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

	return host_shared_Record_atomic(record, atomic, load);
}

/* txn_memtable_branch_own returns the tablespace of the own directory of a working state's repository. */
uint32
txn_memtable_branch_own(HostSharedRecord record)
{
	return (uint32) txn_memtable_branch_load(record, TXN_MEMTABLE_BRANCH_OWN);
}

/* txn_memtable_branch_repository returns the number of a working state's repository, or nothing when it names none. */
bool
txn_memtable_branch_repository(HostSharedRecord record, uint32 *result)
{
	uint64		repository = txn_memtable_branch_load(record, TXN_MEMTABLE_BRANCH_REPOSITORY);

	if (repository == 0 || repository > PG_UINT32_MAX)
		return false;
	*result = (uint32) repository;
	return true;
}

/* txn_memtable_branch_BranchDeltas_Pool_count returns a delta for each storage state, since a working state's chunks are named by its storages' rows. */
static size_t
txn_memtable_branch_BranchDeltas_Pool_count(const HostSharedLimits *limits)
{
	return limits->relation_locks;
}

/* txn_memtable_branch_reclaim_frame_0_t is reclaim's state across its pad: the makers' guard, the sweep, the kept locations, the look, and the handle, while live. */
typedef struct txn_memtable_branch_reclaim_frame_0_t
{
	HostIoIo   *io;
	bool	   *result;
	HostError  *error;
	bool		ok;
	HostSharedGuard making;
	bool		making_live;
	StdVec		swept;
	bool		swept_live;
	StdVec		kept;
	bool		kept_live;
	HostSharedLook look;
	bool		look_live;
	HostSharedHeld held;
	bool		held_live;
} txn_memtable_branch_reclaim_frame_0_t;

/* txn_memtable_branch_reclaim_pad_0 is reclaim's pad: the handle, the look, the sweep, the makers' guard, and the kept locations, while live. */
static void
txn_memtable_branch_reclaim_pad_0(void *frame_)
{
	txn_memtable_branch_reclaim_frame_0_t *frame = frame_;

	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->look_live)
	{
		frame->look_live = false;
		host_shared_Look_drop(&frame->look);
	}
	if (frame->swept_live)
	{
		frame->swept_live = false;
		std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
	if (frame->making_live)
	{
		frame->making_live = false;
		host_shared_Guard_drop(&frame->making);
	}
	if (frame->kept_live)
	{
		frame->kept_live = false;
		std_vec_free_Checked(&frame->kept, sizeof(uint64), _Alignof(uint64));
	}
}

/* txn_memtable_branch_named reports whether a delta's words name a memtable or its chunk spills' manifest lists or pends a file. */
static bool
txn_memtable_branch_named(HostSharedRecord record)
{
	return txn_memtable_branch_load(record, TXN_MEMTABLE_BRANCH_CHUNK_WORD_INDEX) != 0 ||
		txn_memtable_branch_load(record, TXN_MEMTABLE_BRANCH_IDENTITY_WORD) != 0 ||
		txn_memtable_branch_load(record, TXN_MEMTABLE_BRANCH_CHUNK_SPILLS_INDEX) != 0 ||
		txn_memtable_branch_load(record, TXN_MEMTABLE_BRANCH_CHUNK_SPILLS_PENDING_INDEX) != 0;
}

/*
 * txn_memtable_branch_reclaim_looked looks at one location of reclaim's
 * sweep under the makers' lock: it answers false on an error, and sets freed
 * when it freed the delta.
 */
static bool
txn_memtable_branch_reclaim_looked(txn_memtable_branch_reclaim_frame_0_t *frame, uint64 location, bool *undecided,
								   bool *freed)
{
	bool		named;

	if (!host_shared_reclaim(HOST_SHARED_GUARD_WITHIN(&frame->making), &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS,
							 location, &frame->look, frame->error))
		return false;
	frame->look_live = true;
	if (frame->look.tag == HOST_SHARED_LOOK_THEIRS)
		*undecided = true;
	if (frame->look.tag != HOST_SHARED_LOOK_MINE)
	{
		frame->look_live = false;
		host_shared_Look_drop(&frame->look);
		return true;
	}
	named = txn_memtable_branch_named(host_shared_Reclaim_record(&frame->look.u.mine));
	if (!named)
	{
		frame->look_live = false;
		*freed = host_shared_Reclaim_free(&frame->look.u.mine);
		return true;
	}
	if (!std_vec_try_reserve(&frame->kept, 1, sizeof(uint64), _Alignof(uint64)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(&frame->kept, &location, sizeof(location), _Alignof(uint64));
	frame->look_live = false;
	host_shared_Look_drop(&frame->look);
	return true;
}

/* txn_memtable_branch_reclaim_abandoned closes the empty memtables of a kept delta, answering whether it closed one or found none. */
static bool
txn_memtable_branch_reclaim_abandoned(txn_memtable_branch_reclaim_frame_0_t *frame, bool *done)
{
	HostSharedRecord record = host_shared_Held_record(&frame->held);
	uint64		chunks;
	uint64		identity;
	bool		has_chunks = txn_memtable_table_memtable(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, record, &chunks);
	bool		has_identity = txn_memtable_table_memtable(&TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS, record, &identity);
	bool		closed = false;
	bool		abandoned;

	if (!has_chunks && !has_identity)
	{
		*done = true;
		return true;
	}
	if (has_chunks)
	{
		if (!txn_memtable_table_abandoned(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, frame->io, record, chunks, &abandoned,
										  frame->error))
			return false;
		closed |= abandoned;
	}
	if (has_identity)
	{
		if (!txn_memtable_table_abandoned(&TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS, frame->io, record, identity,
										  &abandoned, frame->error))
			return false;
		closed |= abandoned;
	}
	*done = closed;
	return true;
}

/* txn_memtable_branch_reclaim_body_0 is reclaim's body. */
static void
txn_memtable_branch_reclaim_body_0(void *frame_)
{
	txn_memtable_branch_reclaim_frame_0_t *frame = frame_;
	HostSharedRecord makers;
	bool		undecided = false;

	std_vec_new(&frame->kept, sizeof(uint64), _Alignof(uint64));
	frame->kept_live = true;
	if (!host_shared_single(&TXN_STATE_MAKERS_SINGLE_OPS, &makers, frame->error) ||
		!host_shared_Record_lock(makers, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_MAKING_INDEX, &frame->making,
								 frame->error))
	{
		txn_memtable_branch_reclaim_pad_0(frame);
		return;
	}
	frame->making_live = true;
	if (!host_shared_sweep(HOST_SHARED_GUARD_WITHIN(&frame->making), &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS,
						   TXN_MEMTABLE_BRANCH_REPOSITORY, &frame->swept, frame->error))
	{
		txn_memtable_branch_reclaim_pad_0(frame);
		return;
	}
	frame->swept_live = true;
	for (size_t k = 0; k < frame->swept.len; k++)
	{
		bool		freed = false;

		if (!txn_memtable_branch_reclaim_looked(frame, STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k).f0,
												&undecided, &freed))
		{
			txn_memtable_branch_reclaim_pad_0(frame);
			return;
		}
		if (freed)
		{
			*frame->result = true;
			frame->ok = true;
			txn_memtable_branch_reclaim_pad_0(frame);
			return;
		}
	}
	frame->swept_live = false;
	std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->making_live = false;
	host_shared_Guard_drop(&frame->making);
	for (size_t k = 0; k < frame->kept.len; k++)
	{
		bool		some;
		bool		done = false;

		if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS,
							  STD_VEC_AT(&frame->kept, uint64, k), &some, &frame->held, frame->error))
		{
			txn_memtable_branch_reclaim_pad_0(frame);
			return;
		}
		if (!some)
			continue;
		frame->held_live = true;
		if (!txn_memtable_branch_reclaim_abandoned(frame, &done))
		{
			txn_memtable_branch_reclaim_pad_0(frame);
			return;
		}
		if (done)
		{
			*frame->result = true;
			frame->ok = true;
			txn_memtable_branch_reclaim_pad_0(frame);
			return;
		}
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	*frame->result = undecided;
	frame->ok = true;
	txn_memtable_branch_reclaim_pad_0(frame);
}

/*
 * txn_memtable_branch_BranchDeltas_Pool_reclaim frees, under the makers'
 * lock, a delta no process holds whose words name no memtable and whose
 * chunk spills' manifest lists and pends no file, since no version names it
 * then, and answers whether the make should look again: when it freed one or
 * met one another reclaim was looking at. A delta kept only by memtables
 * that hold nothing and no pin names has them closed once the makers' lock
 * is released, and the make looks again.
 */
static bool
txn_memtable_branch_BranchDeltas_Pool_reclaim(HostIoIo *io, bool *result, HostError *error)
{
	txn_memtable_branch_reclaim_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, txn_memtable_branch_reclaim_body_0, txn_memtable_branch_reclaim_pad_0);
	return frame.ok;
}

/* TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS is BranchDeltas' Shape and Pool: a working state's delta, two locks, and seven atomics. */
const HostSharedPoolOps TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS = {
	.shape = {.id = UINT64CONST(0x74786e000014), .locks = 2, .atomics = 7},
	.name = {"working state's delta", 21},
	.count = txn_memtable_branch_BranchDeltas_Pool_count,
	.reclaim = txn_memtable_branch_BranchDeltas_Pool_reclaim,
};

/* txn_memtable_branch_BranchDeltas_Owner_grew counts nothing, since no fold is due by a branch's deltas' size. */
static void
txn_memtable_branch_BranchDeltas_Owner_grew(HostSharedRecord owner, uint64 bytes)
{
	(void) owner;
	(void) bytes;
}

/* txn_memtable_branch_BranchDeltas_Owner_shrank counts nothing, as grew. */
static void
txn_memtable_branch_BranchDeltas_Owner_shrank(HostSharedRecord owner, uint64 bytes)
{
	(void) owner;
	(void) bytes;
}

/* TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_OWNER_OPS is BranchDeltas' Owner: a working state's delta, whose grew and shrank count nothing. */
static const TxnMemtableTableOwnerOps TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_OWNER_OPS = {
	.grew = txn_memtable_branch_BranchDeltas_Owner_grew,
	.shrank = txn_memtable_branch_BranchDeltas_Owner_shrank,
};

/* TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS is Chunks' Tree: the memtable of a working state's chunks, whose versions count at no entry. */
const TxnMemtableTableTreeOps TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS = {
	.of = &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_OWNER_OPS,
	.lock = TXN_MEMTABLE_BRANCH_DELTAS_LOCK_INDEX,
	.word = TXN_MEMTABLE_BRANCH_CHUNK_WORD_INDEX,
	.pin = TXN_MEMTABLE_PINS_CHUNK_PIN,
};

/* TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS is Identity's Tree: the memtable of a working state's identity tree. */
const TxnMemtableTableTreeOps TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS = {
	.of = &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_OWNER_OPS,
	.lock = TXN_MEMTABLE_BRANCH_DELTAS_LOCK_INDEX,
	.word = TXN_MEMTABLE_BRANCH_IDENTITY_WORD,
	.pin = TXN_MEMTABLE_PINS_IDENTITY_PIN,
	.counted = true,
};

/* txn_memtable_branch_find_frame_0_t is find's state across its pad: the sweep and the handle, while live. */
typedef struct txn_memtable_branch_find_frame_0_t
{
	HostSharedWithin within;
	uint32		repository;
	bool	   *some;
	HostSharedHeld *result;
	HostError  *error;
	bool		ok;
	StdVec		swept;
	bool		swept_live;
	HostSharedHeld held;
	bool		held_live;
} txn_memtable_branch_find_frame_0_t;

/* txn_memtable_branch_find_pad_0 is find's pad: the handle, then the sweep, while live. */
static void
txn_memtable_branch_find_pad_0(void *frame_)
{
	txn_memtable_branch_find_frame_0_t *frame = frame_;

	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->swept_live)
	{
		frame->swept_live = false;
		std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
}

/* txn_memtable_branch_find_body_0 is find's body. */
static void
txn_memtable_branch_find_body_0(void *frame_)
{
	txn_memtable_branch_find_frame_0_t *frame = frame_;
	uint64		key = (uint64) frame->repository;

	*frame->some = false;
	if (!host_shared_sweep(frame->within, &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS, TXN_MEMTABLE_BRANCH_REPOSITORY,
						   &frame->swept, frame->error))
		return;
	frame->swept_live = true;
	for (size_t k = 0; k < frame->swept.len; k++)
	{
		const std_TupleU64U64_t *found = &STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k);
		bool		has;

		if (found->f1 != key)
			continue;
		if (!host_shared_open(frame->within, &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS, found->f0, &has,
							  &frame->held, frame->error))
		{
			txn_memtable_branch_find_pad_0(frame);
			return;
		}
		if (!has)
			continue;
		frame->held_live = true;
		if (txn_memtable_branch_load(host_shared_Held_record(&frame->held), TXN_MEMTABLE_BRANCH_REPOSITORY) == key)
		{
			frame->held_live = false;
			*frame->result = frame->held;
			*frame->some = true;
			break;
		}
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	frame->ok = true;
	txn_memtable_branch_find_pad_0(frame);
}

/*
 * txn_memtable_branch_find returns a handle on a repository's published
 * delta, or nothing when none is, comparing the repository again under the
 * handle, so a slot freed and made again for another repository meanwhile
 * is passed over.
 */
bool
txn_memtable_branch_find(HostSharedWithin within, uint32 repository, bool *some, HostSharedHeld *result,
						 HostError *error)
{
	txn_memtable_branch_find_frame_0_t frame = {.within = within, .repository = repository, .some = some,
	.result = result, .error = error};

	std_landing(&frame, txn_memtable_branch_find_body_0, txn_memtable_branch_find_pad_0);
	return frame.ok;
}

/* txn_memtable_branch_filled_frame_0_t is filled's state across its pad: the sweep, the handle, and the repositories found, while live. */
typedef struct txn_memtable_branch_filled_frame_0_t
{
	HostIoIo   *io;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	StdVec		swept;
	bool		swept_live;
	HostSharedHeld held;
	bool		held_live;
	bool		result_live;
} txn_memtable_branch_filled_frame_0_t;

/* txn_memtable_branch_filled_pad_0 is filled's pad: the handle, the sweep, and the repositories found, while live. */
static void
txn_memtable_branch_filled_pad_0(void *frame_)
{
	txn_memtable_branch_filled_frame_0_t *frame = frame_;

	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->swept_live)
	{
		frame->swept_live = false;
		std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_free_Checked(frame->result, sizeof(uint32), _Alignof(uint32));
	}
}

/* txn_memtable_branch_filled_body_0 is filled's body. */
static void
txn_memtable_branch_filled_body_0(void *frame_)
{
	txn_memtable_branch_filled_frame_0_t *frame = frame_;

	std_vec_new(frame->result, sizeof(uint32), _Alignof(uint32));
	frame->result_live = true;
	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(frame->io), &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS,
						   TXN_MEMTABLE_BRANCH_REPOSITORY, &frame->swept, frame->error))
	{
		txn_memtable_branch_filled_pad_0(frame);
		return;
	}
	frame->swept_live = true;
	for (size_t k = 0; k < frame->swept.len; k++)
	{
		const std_TupleU64U64_t *found = &STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k);
		HostSharedRecord record;
		bool		has;
		bool		named;

		if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS, found->f0,
							  &has, &frame->held, frame->error))
		{
			txn_memtable_branch_filled_pad_0(frame);
			return;
		}
		if (!has)
			continue;
		frame->held_live = true;
		record = host_shared_Held_record(&frame->held);
		named = txn_memtable_branch_load(record, TXN_MEMTABLE_BRANCH_CHUNK_WORD_INDEX) != 0 ||
			txn_memtable_branch_load(record, TXN_MEMTABLE_BRANCH_IDENTITY_WORD) != 0;
		if (found->f1 != 0 && found->f1 <= PG_UINT32_MAX && named &&
			txn_memtable_branch_load(record, TXN_MEMTABLE_BRANCH_REPOSITORY) == found->f1)
		{
			uint32		repository = (uint32) found->f1;

			if (!std_vec_try_reserve(frame->result, 1, sizeof(uint32), _Alignof(uint32)))
			{
				*frame->error = host_Error_out_of_memory();
				txn_memtable_branch_filled_pad_0(frame);
				return;
			}
			std_vec_push_Checked(frame->result, &repository, sizeof(repository), _Alignof(uint32));
		}
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	frame->result_live = false;
	frame->ok = true;
	txn_memtable_branch_filled_pad_0(frame);
}

/*
 * txn_memtable_branch_filled returns each repository whose working state's
 * delta names a memtable, from a sweep of the pool, whose versions a
 * checkpoint saves in its runs whether or not a storage of the repository
 * has a state.
 */
bool
txn_memtable_branch_filled(HostIoIo *io, StdVec *result, HostError *error)
{
	txn_memtable_branch_filled_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, txn_memtable_branch_filled_body_0, txn_memtable_branch_filled_pad_0);
	return frame.ok;
}

/* txn_memtable_branch_deltas_frame_0_t is deltas' state across its pad: the delta it made and the makers' guard, while live. */
typedef struct txn_memtable_branch_deltas_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		own;
	HostSharedHeld *result;
	HostError  *error;
	bool		ok;
	HostSharedOwned owned;
	bool		owned_live;
	HostSharedGuard making;
	bool		making_live;
} txn_memtable_branch_deltas_frame_0_t;

/* txn_memtable_branch_deltas_pad_0 is deltas' pad: the makers' guard, then the delta it made, while live. */
static void
txn_memtable_branch_deltas_pad_0(void *frame_)
{
	txn_memtable_branch_deltas_frame_0_t *frame = frame_;

	if (frame->making_live)
	{
		frame->making_live = false;
		host_shared_Guard_drop(&frame->making);
	}
	if (frame->owned_live)
	{
		frame->owned_live = false;
		host_shared_Owned_drop(&frame->owned);
	}
}

/* txn_memtable_branch_deltas_body_0 is deltas' body. */
static void
txn_memtable_branch_deltas_body_0(void *frame_)
{
	txn_memtable_branch_deltas_frame_0_t *frame = frame_;
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE};
	HostSharedRecord makers;
	HostSharedRecord record;
	uint64		location;
	bool		some;

	if (!txn_memtable_branch_find(HOST_SHARED_IO_WITHIN(frame->io), frame->repository, &some, frame->result,
								  frame->error))
		return;
	if (some)
	{
		frame->ok = true;
		return;
	}
	if (!host_shared_make(&TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS, frame->io, &frame->owned, frame->error))
		return;
	frame->owned_live = true;
	record = host_shared_Owned_record(&frame->owned);
	op.u.exchange.f1 = (uint64) frame->repository;
	host_shared_Record_atomic(record, TXN_MEMTABLE_BRANCH_REPOSITORY, op);
	op.u.exchange.f1 = (uint64) frame->own;
	host_shared_Record_atomic(record, TXN_MEMTABLE_BRANCH_OWN, op);
	if (!host_shared_single(&TXN_STATE_MAKERS_SINGLE_OPS, &makers, frame->error) ||
		!host_shared_Record_lock(makers, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_MAKING_INDEX, &frame->making,
								 frame->error))
	{
		txn_memtable_branch_deltas_pad_0(frame);
		return;
	}
	frame->making_live = true;
	if (!txn_memtable_branch_find(HOST_SHARED_GUARD_WITHIN(&frame->making), frame->repository, &some, frame->result,
								  frame->error))
	{
		txn_memtable_branch_deltas_pad_0(frame);
		return;
	}
	if (some)
	{
		frame->ok = true;
		txn_memtable_branch_deltas_pad_0(frame);
		return;
	}
	frame->owned_live = false;
	location = host_shared_Owned_publish(&frame->owned);
	if (!host_shared_open(HOST_SHARED_GUARD_WITHIN(&frame->making), &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS,
						  location, &some, frame->result, frame->error))
	{
		txn_memtable_branch_deltas_pad_0(frame);
		return;
	}
	if (!some)
		*frame->error = txn_memtable_branch_internal("a delta just published isn't there");
	frame->ok = some;
	txn_memtable_branch_deltas_pad_0(frame);
}

/*
 * txn_memtable_branch_deltas returns a handle on a repository's working
 * state's delta, making one when none is published as a storage's state is
 * made, with the repository's own tablespace: with no lock held, then, under
 * the makers' lock, finding again, dropping its own on a hit, which frees it
 * unpublished, and otherwise publishing its own and opening it under the same
 * lock.
 */
bool
txn_memtable_branch_deltas(HostIoIo *io, uint32 repository, uint32 own, HostSharedHeld *result, HostError *error)
{
	txn_memtable_branch_deltas_frame_0_t frame = {.io = io, .repository = repository, .own = own, .result = result,
	.error = error};

	std_landing(&frame, txn_memtable_branch_deltas_body_0, txn_memtable_branch_deltas_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_branch_prepare readies a chunk's push to a working state's
 * delta before the checkpoint window of its record and runs the window's
 * work with it, need the longest record its version takes; see table's
 * prepare. The push is finished by finish.
 */
bool
txn_memtable_branch_prepare(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len, size_t need,
							bool (*inside) (void *captures, HostIoIo *io,
											TxnMemtableTablePreparedBranchDeltas *prepared, HostError *error),
							void *inside_captures, void (*inside_drop) (void *captures), HostError *error)
{
	TxnMemtableTableAim aim = {.owner = deltas, .tree = &TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, .key = {key, key_len},
	.need = need};
	TxnMemtableTableReservation none = {0};
	bool		has_unstored;
	TxnMemtableTableReservation unstored;

	return txn_memtable_table_prepared(io, &aim, 1, false, none, inside, inside_captures, inside_drop, &has_unstored,
									   &unstored, error);
}

/*
 * txn_memtable_branch_finish pushes a prepared chunk's version inside the
 * window, allocating nothing: a put of the chunk's bytes, moved into the
 * version and back, as its writer's, the transaction whose row placed it or
 * NO_TRANSACTION for a built base's node, durable when it carries the end of
 * its WAL record. It fails only on corruption.
 */
bool
txn_memtable_branch_finish(HostIoIo *io, TxnMemtableTablePreparedBranchDeltas *prepared,
						   TxnMemtableVersionWriter writer, StdVec *chunk, bool has_lsn, uint64 lsn,
						   TxnMemtableVersionHandle *result, HostError *error)
{
	TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_PUT};
	TxnMemtableVersionChange change = {.writer = writer, .op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_KEY_SHARE,
	.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}, .has_lsn = has_lsn, .lsn = lsn};
	bool		pushed;

	op.u.put.value = *chunk;
	std_vec_new(chunk, 1, 1);
	std_vec_new(&op.u.put.addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_vec_new(&op.u.put.values, sizeof(uint32), _Alignof(uint32));
	pushed = txn_memtable_table_pushed(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, &change, result, error);
	*chunk = op.u.put.value;
	return pushed;
}

/* txn_memtable_branch_holds reports whether a working state's delta holds the version a chunk's record of an LSN pushed, so that its redo skips it. */
bool
txn_memtable_branch_holds(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len, uint64 lsn,
						  bool *result, HostError *error)
{
	return txn_memtable_table_held(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, io, deltas, key, key_len, lsn, result, error);
}

/* txn_memtable_branch_reading_t is the captures of get's, taken's, durable's, and identity_versions' reads: the key and what they read. */
typedef struct txn_memtable_branch_reading_t
{
	const uint8 *key;
	size_t		key_len;
	bool		some;
	StdVec	   *result;
	bool		result_live;
	bool		durable;
	/* version is a version read for a moment, while live. */
	TxnMemtableVersionVersion version;
	bool		version_live;
} txn_memtable_branch_reading_t;

/* txn_memtable_branch_reading_drop drops what a read holds when it unwinds: its version, then its result, while live. */
static void
txn_memtable_branch_reading_drop(txn_memtable_branch_reading_t *reading)
{
	if (reading->version_live)
	{
		reading->version_live = false;
		txn_memtable_version_Version_drop(&reading->version);
	}
	if (reading->result_live)
	{
		reading->result_live = false;
		std_vec_free_Checked(reading->result, 1, 1);
	}
}

/* txn_memtable_branch_chain finds a key under a range's shared lock and starts its chain, answering some false when the range holds none. */
static bool
txn_memtable_branch_chain(const txn_memtable_branch_reading_t *reading, const HostDeltaHold *hold,
						  const HostSharedGuard *guard, bool *some, TxnMemtableVersionChain *result, HostError *error)
{
	TxnMemtableIndexNode node;

	return txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, reading->key, reading->key_len,
								   some, &node, error) &&
		(!*some || txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, result, error));
}

/* txn_memtable_branch_get_reading is get's read under the range's shared lock: the newest version's chunk. */
static bool
txn_memtable_branch_get_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
								const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_branch_reading_t *reading = captures;
	TxnMemtableVersionChain chain;
	TxnMemtableVersionRecord record;
	TxnMemtableVersionOpRef op;
	bool		some;

	(void) at;
	if (!txn_memtable_branch_chain(reading, hold, guard, &some, &chain, error))
		return false;
	if (!some)
		return true;
	if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &reading->version, error))
		return false;
	if (!some)
		return true;
	reading->version_live = true;
	record = txn_memtable_version_Version_record(&reading->version);
	op = txn_memtable_version_Record_op(&record);
	if (op.tag != TXN_MEMTABLE_VERSION_OP_REF_PUT)
	{
		reading->version_live = false;
		txn_memtable_version_Version_drop(&reading->version);
		*error = txn_memtable_branch_internal("a chunk's version puts no chunk");
		return false;
	}
	std_vec_new(reading->result, 1, 1);
	reading->result_live = true;
	std_vec_extend_from_slice_Checked(reading->result, op.u.put.value.ptr, op.u.put.value.len, 1, 1);
	reading->version_live = false;
	txn_memtable_version_Version_drop(&reading->version);
	reading->some = true;
	return true;
}

/* txn_memtable_branch_get_frame_0_t is get's state across its pad: its read. */
typedef struct txn_memtable_branch_get_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord deltas;
	bool	   *some;
	HostError  *error;
	bool		ok;
	txn_memtable_branch_reading_t reading;
} txn_memtable_branch_get_frame_0_t;

/* txn_memtable_branch_get_pad_0 is get's pad: what its read holds, while live. */
static void
txn_memtable_branch_get_pad_0(void *frame_)
{
	txn_memtable_branch_get_frame_0_t *frame = frame_;

	txn_memtable_branch_reading_drop(&frame->reading);
}

/* txn_memtable_branch_get_body_0 is get's body. */
static void
txn_memtable_branch_get_body_0(void *frame_)
{
	txn_memtable_branch_get_frame_0_t *frame = frame_;
	bool		read;

	if (!txn_memtable_table_read(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, frame->io, frame->deltas, frame->reading.key,
								 frame->reading.key_len, txn_memtable_branch_get_reading, &frame->reading, &read,
								 frame->error))
	{
		txn_memtable_branch_get_pad_0(frame);
		return;
	}
	*frame->some = read && frame->reading.some;
	frame->reading.result_live = false;
	frame->ok = true;
}

/*
 * txn_memtable_branch_get returns the bytes of the chunk under a key in a
 * working state's delta, its newest version's, which every version of the
 * key holds alike, or nothing for a key the delta doesn't hold.
 */
bool
txn_memtable_branch_get(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len, bool *some,
						StdVec *result, HostError *error)
{
	txn_memtable_branch_get_frame_0_t frame = {.io = io, .deltas = deltas, .some = some, .error = error,
	.reading = {.key = key, .key_len = key_len, .result = result}};

	std_landing(&frame, txn_memtable_branch_get_body_0, txn_memtable_branch_get_pad_0);
	return frame.ok;
}

/* txn_memtable_branch_handles_reading is taken's read under the range's shared lock: the handles of the key's chain. */
static bool
txn_memtable_branch_handles_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
									const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_branch_reading_t *reading = captures;
	TxnMemtableVersionChain chain;
	bool		some;

	(void) at;
	if (!txn_memtable_branch_chain(reading, hold, guard, &some, &chain, error))
		return false;
	while (some)
	{
		TxnMemtableVersionVersion version;
		TxnMemtableVersionHandle handle;

		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, error))
			return false;
		if (!some)
			break;
		handle = version.handle;
		txn_memtable_version_Version_drop(&version);
		std_vec_push_Checked(reading->result, &handle, sizeof(handle), _Alignof(TxnMemtableVersionHandle));
	}
	return true;
}

/* txn_memtable_branch_taken_frame_0_t is taken's state across its pad: the handles read and the removal's answers, while live. */
typedef struct txn_memtable_branch_taken_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord deltas;
	const uint8 *key;
	size_t		key_len;
	HostError  *error;
	bool		ok;
	StdVec		handles;
	bool		handles_live;
	StdVec		taken;
	bool		taken_live;
} txn_memtable_branch_taken_frame_0_t;

/* txn_memtable_branch_taken_pad_0 is taken's pad: the removal's answers and the handles, while live. */
static void
txn_memtable_branch_taken_pad_0(void *frame_)
{
	txn_memtable_branch_taken_frame_0_t *frame = frame_;

	if (frame->taken_live)
	{
		frame->taken_live = false;
		std_vec_free_Checked(&frame->taken, sizeof(bool), _Alignof(bool));
	}
	if (frame->handles_live)
	{
		frame->handles_live = false;
		std_vec_free_Checked(&frame->handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	}
}

/* txn_memtable_branch_taken_body_0 is taken's body. */
static void
txn_memtable_branch_taken_body_0(void *frame_)
{
	txn_memtable_branch_taken_frame_0_t *frame = frame_;
	txn_memtable_branch_reading_t reading = {.key = frame->key, .key_len = frame->key_len, .result = &frame->handles};
	bool		read;

	std_vec_new(&frame->handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	frame->handles_live = true;
	if (!txn_memtable_table_read(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, frame->io, frame->deltas, frame->key,
								 frame->key_len, txn_memtable_branch_handles_reading, &reading, &read, frame->error))
	{
		txn_memtable_branch_taken_pad_0(frame);
		return;
	}
	if (!txn_memtable_table_removed(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, frame->io, frame->deltas, frame->key,
									frame->key_len, frame->handles.ptr, frame->handles.len, &frame->taken,
									frame->error))
	{
		txn_memtable_branch_taken_pad_0(frame);
		return;
	}
	frame->taken_live = true;
	frame->ok = true;
	txn_memtable_branch_taken_pad_0(frame);
}

/*
 * txn_memtable_branch_taken takes every version of the chunk under a key out
 * of a working state's delta through the remover, once a pack holds the
 * chunk under a base that names it: a built base's node its redo wrote
 * again.
 */
bool
txn_memtable_branch_taken(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len, HostError *error)
{
	txn_memtable_branch_taken_frame_0_t frame = {.io = io, .deltas = deltas, .key = key, .key_len = key_len,
	.error = error};

	std_landing(&frame, txn_memtable_branch_taken_body_0, txn_memtable_branch_taken_pad_0);
	return frame.ok;
}

/* txn_memtable_branch_durable_reading is durable's read under the range's shared lock: whether a version carries its WAL record's end. */
static bool
txn_memtable_branch_durable_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
									const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_branch_reading_t *reading = captures;
	TxnMemtableVersionChain chain;
	bool		some;

	(void) at;
	if (!txn_memtable_branch_chain(reading, hold, guard, &some, &chain, error))
		return false;
	while (some)
	{
		TxnMemtableVersionVersion version;
		TxnMemtableVersionRecord record;
		uint64		lsn;

		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, error))
			return false;
		if (!some)
			break;
		record = txn_memtable_version_Version_record(&version);
		reading->durable = txn_memtable_version_Record_lsn(&record, &lsn);
		txn_memtable_version_Version_drop(&version);
		if (reading->durable)
			break;
	}
	return true;
}

/*
 * txn_memtable_branch_durable reports whether a working state's delta holds
 * a durable version of the chunk under a key, one that carries the end of
 * its WAL record, which a logged writer passes over; otherwise it pushes its
 * own logged copy.
 */
bool
txn_memtable_branch_durable(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len, bool *result,
							HostError *error)
{
	txn_memtable_branch_reading_t reading = {.key = key, .key_len = key_len};
	bool		read;

	if (!txn_memtable_table_read(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, io, deltas, key, key_len,
								 txn_memtable_branch_durable_reading, &reading, &read, error))
		return false;
	*result = read && reading.durable;
	return true;
}

/* txn_memtable_branch_node_base returns the base a built node's key names: its storage's tablespace and relation and its fold point. */
static void
txn_memtable_branch_node_base(const uint8 *key, uint32 *tablespace, uint32 *relation, uint64 *point)
{
	*tablespace = std_u32_from_be_bytes(key + TXN_MEMTABLE_BRANCH_ADDRESS);
	*relation = std_u32_from_be_bytes(key + TXN_MEMTABLE_BRANCH_ADDRESS + 4);
	*point = std_u64_from_be_bytes(key + TXN_MEMTABLE_BRANCH_ADDRESS + 8);
}

/* txn_memtable_branch_NodeKey_t is a built node's key, an element of nodes_taken's nodes. */
typedef struct txn_memtable_branch_NodeKey_t
{
	uint8		f0[TXN_MEMTABLE_BRANCH_NODE_KEY];
} txn_memtable_branch_NodeKey_t;

/* txn_memtable_branch_nodes_taken_frame_0_t is nodes_taken's state across its pad: the nodes picked and the caller's closure. */
typedef struct txn_memtable_branch_nodes_taken_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord deltas;
	bool		(*picked) (void *captures, HostIoIo *io, uint32 tablespace, uint32 relation, uint64 point,
						   uint64 writer, bool *result, HostError *error);
	void	   *picked_captures;
	void		(*picked_drop) (void *captures);
	HostError  *error;
	bool		ok;
	StdVec		nodes;
	bool		nodes_live;
} txn_memtable_branch_nodes_taken_frame_0_t;

/* txn_memtable_branch_nodes_taken_pad_0 is nodes_taken's pad: the nodes picked, then the caller's closure. */
static void
txn_memtable_branch_nodes_taken_pad_0(void *frame_)
{
	txn_memtable_branch_nodes_taken_frame_0_t *frame = frame_;

	if (frame->nodes_live)
	{
		frame->nodes_live = false;
		std_vec_free_Checked(&frame->nodes, sizeof(txn_memtable_branch_NodeKey_t),
							 _Alignof(txn_memtable_branch_NodeKey_t));
	}
	if (frame->picked_drop != NULL)
	{
		void		(*drop) (void *captures) = frame->picked_drop;

		frame->picked_drop = NULL;
		drop(frame->picked_captures);
	}
}

/* txn_memtable_branch_nodes_taken_each is nodes_taken's step of its walk: a first piece's node, kept when the caller picks it. */
static bool
txn_memtable_branch_nodes_taken_each(void *captures, HostIoIo *io, const uint8 *key, size_t key_len,
									 const StdVec *values, size_t values_len, HostError *error)
{
	txn_memtable_branch_nodes_taken_frame_0_t *frame = captures;
	txn_memtable_branch_NodeKey_t node;
	StdVec		first;
	bool		some;
	uint64		writer;
	size_t		length;
	bool		headed;
	uint32		tablespace;
	uint32		relation;
	uint64		point;
	bool		picked;

	(void) values;
	(void) values_len;
	if (!txn_memtable_branch_first_piece(key, key_len, node.f0))
		return true;
	if (!txn_memtable_branch_get(io, frame->deltas, key, key_len, &some, &first, error))
		return false;
	if (!some)
		return true;
	headed = txn_memtable_branch_head(&first, &writer, &length, error);
	std_vec_free_Checked(&first, 1, 1);
	if (!headed)
		return false;
	txn_memtable_branch_node_base(node.f0, &tablespace, &relation, &point);
	if (!frame->picked(frame->picked_captures, io, tablespace, relation, point, writer, &picked, error))
		return false;
	if (!picked)
		return true;
	if (!std_vec_try_reserve(&frame->nodes, 1, sizeof(node), _Alignof(txn_memtable_branch_NodeKey_t)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(&frame->nodes, &node, sizeof(node), _Alignof(txn_memtable_branch_NodeKey_t));
	return true;
}

/* txn_memtable_branch_nodes_taken_body_0 is nodes_taken's body. */
static void
txn_memtable_branch_nodes_taken_body_0(void *frame_)
{
	txn_memtable_branch_nodes_taken_frame_0_t *frame = frame_;

	std_vec_new(&frame->nodes, sizeof(txn_memtable_branch_NodeKey_t), _Alignof(txn_memtable_branch_NodeKey_t));
	frame->nodes_live = true;
	if (!txn_memtable_table_walked(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, frame->io, frame->deltas,
								   txn_memtable_branch_nodes_taken_each, frame, NULL, frame->error))
	{
		txn_memtable_branch_nodes_taken_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->nodes.len; k++)
		if (!txn_memtable_branch_node_taken(frame->io, frame->deltas,
											STD_VEC_AT(&frame->nodes, txn_memtable_branch_NodeKey_t, k).f0,
											frame->error))
		{
			txn_memtable_branch_nodes_taken_pad_0(frame);
			return;
		}
	frame->ok = true;
	txn_memtable_branch_nodes_taken_pad_0(frame);
}

/*
 * txn_memtable_branch_nodes_taken takes out of a working state's delta,
 * through the remover, every built base's node that picked answers for,
 * given its base and its writer: the nodes a recovery won't rebuild, since
 * their base record installs without a rebuild, never comes, or comes from
 * a writer that ended uncommitted. It takes picked.
 */
bool
txn_memtable_branch_nodes_taken(HostIoIo *io, HostSharedRecord deltas,
								bool (*picked) (void *captures, HostIoIo *io, uint32 tablespace, uint32 relation,
												uint64 point, uint64 writer, bool *result, HostError *error),
								void *picked_captures, void (*picked_drop) (void *captures), HostError *error)
{
	txn_memtable_branch_nodes_taken_frame_0_t frame = {.io = io, .deltas = deltas, .picked = picked,
	.picked_captures = picked_captures, .picked_drop = picked_drop, .error = error};

	std_landing(&frame, txn_memtable_branch_nodes_taken_body_0, txn_memtable_branch_nodes_taken_pad_0);
	return frame.ok;
}

/* txn_memtable_branch_Found_t is an element of every_nodes_taken's found: a repository and its delta's handle. */
typedef struct txn_memtable_branch_Found_t
{
	uint32		f0;
	HostSharedHeld f1;
} txn_memtable_branch_Found_t;

/* txn_memtable_branch_every_frame_0_t is every_nodes_taken's state across its pad: the sweep, the deltas found, of which moved are taken, and the caller's closure. */
typedef struct txn_memtable_branch_every_frame_0_t
{
	HostIoIo   *io;
	bool		(*picked) (void *captures, HostIoIo *io, uint32 repository, uint32 tablespace, uint32 relation,
						   uint64 point, uint64 writer, bool *result, HostError *error);
	void	   *picked_captures;
	void		(*picked_drop) (void *captures);
	HostError  *error;
	bool		ok;
	StdVec		swept;
	bool		swept_live;
	HostSharedHeld held;
	bool		held_live;
	StdVec		found;
	bool		found_live;
	size_t		moved;
	uint32		repository;
} txn_memtable_branch_every_frame_0_t;

/* txn_memtable_branch_every_pad_0 is every_nodes_taken's pad: the handle, the sweep, the deltas not yet taken, then the caller's closure. */
static void
txn_memtable_branch_every_pad_0(void *frame_)
{
	txn_memtable_branch_every_frame_0_t *frame = frame_;

	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->swept_live)
	{
		frame->swept_live = false;
		std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
	if (frame->found_live)
	{
		frame->found_live = false;
		for (size_t k = frame->moved; k < frame->found.len; k++)
			host_shared_Held_drop(&STD_VEC_AT(&frame->found, txn_memtable_branch_Found_t, k).f1);
		std_vec_free_Checked(&frame->found, sizeof(txn_memtable_branch_Found_t), _Alignof(txn_memtable_branch_Found_t));
	}
	if (frame->picked_drop != NULL)
	{
		void		(*drop) (void *captures) = frame->picked_drop;

		frame->picked_drop = NULL;
		drop(frame->picked_captures);
	}
}

/* txn_memtable_branch_every_picked is every_nodes_taken's closure for one delta: the caller's, given the delta's repository. */
static bool
txn_memtable_branch_every_picked(void *captures, HostIoIo *io, uint32 tablespace, uint32 relation, uint64 point,
								 uint64 writer, bool *result, HostError *error)
{
	txn_memtable_branch_every_frame_0_t *frame = captures;

	return frame->picked(frame->picked_captures, io, frame->repository, tablespace, relation, point, writer, result,
						 error);
}

/* txn_memtable_branch_every_body_0 is every_nodes_taken's body. */
static void
txn_memtable_branch_every_body_0(void *frame_)
{
	txn_memtable_branch_every_frame_0_t *frame = frame_;

	std_vec_new(&frame->found, sizeof(txn_memtable_branch_Found_t), _Alignof(txn_memtable_branch_Found_t));
	frame->found_live = true;
	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(frame->io), &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS,
						   TXN_MEMTABLE_BRANCH_REPOSITORY, &frame->swept, frame->error))
	{
		txn_memtable_branch_every_pad_0(frame);
		return;
	}
	frame->swept_live = true;
	for (size_t k = 0; k < frame->swept.len; k++)
	{
		const std_TupleU64U64_t *swept = &STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k);
		txn_memtable_branch_Found_t found;
		bool		has;

		if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS, swept->f0,
							  &has, &frame->held, frame->error))
		{
			txn_memtable_branch_every_pad_0(frame);
			return;
		}
		if (!has)
			continue;
		frame->held_live = true;
		if (swept->f1 > PG_UINT32_MAX)
		{
			frame->held_live = false;
			host_shared_Held_drop(&frame->held);
			continue;
		}
		if (!std_vec_try_reserve(&frame->found, 1, sizeof(found), _Alignof(txn_memtable_branch_Found_t)))
		{
			*frame->error = host_Error_out_of_memory();
			txn_memtable_branch_every_pad_0(frame);
			return;
		}
		found.f0 = (uint32) swept->f1;
		found.f1 = frame->held;
		frame->held_live = false;
		std_vec_push_Checked(&frame->found, &found, sizeof(found), _Alignof(txn_memtable_branch_Found_t));
	}
	frame->swept_live = false;
	std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	while (frame->moved < frame->found.len)
	{
		txn_memtable_branch_Found_t *found = &STD_VEC_AT(&frame->found, txn_memtable_branch_Found_t, frame->moved);
		bool		ok;

		frame->held = found->f1;
		frame->held_live = true;
		frame->repository = found->f0;
		frame->moved = std_wrapping_add_usize(frame->moved, 1);
		ok = txn_memtable_branch_nodes_taken(frame->io, host_shared_Held_record(&frame->held),
											 txn_memtable_branch_every_picked, frame, NULL, frame->error);
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
		if (!ok)
		{
			txn_memtable_branch_every_pad_0(frame);
			return;
		}
	}
	frame->ok = true;
	txn_memtable_branch_every_pad_0(frame);
}

/*
 * txn_memtable_branch_every_nodes_taken takes out of every repository's
 * working state's delta each built base's node picked answers for, given the
 * repository's number besides, as nodes_taken does. It takes picked.
 */
bool
txn_memtable_branch_every_nodes_taken(HostIoIo *io,
									  bool (*picked) (void *captures, HostIoIo *io, uint32 repository,
													  uint32 tablespace, uint32 relation, uint64 point,
													  uint64 writer, bool *result, HostError *error),
									  void *picked_captures, void (*picked_drop) (void *captures), HostError *error)
{
	txn_memtable_branch_every_frame_0_t frame = {.io = io, .picked = picked, .picked_captures = picked_captures,
	.picked_drop = picked_drop, .error = error};

	std_landing(&frame, txn_memtable_branch_every_body_0, txn_memtable_branch_every_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_branch_identity_finish pushes the version of a prepared
 * identity tree key, by its place among the prepare's keys, inside the
 * window, allocating nothing, and counts it at its writer's entry, as a
 * row's push is counted, so a fold of the tree finds its writer's outcome.
 */
bool
txn_memtable_branch_identity_finish(HostIoTakesShort io, TxnMemtableTablePreparedBranchDeltas *prepared, size_t key,
									const TxnMemtableVersionChange *change, bool has_roomed, TxnStatusRoomed roomed,
									TxnMemtableVersionHandle *result, HostError *error)
{
	return txn_memtable_table_finished(io, prepared, key, change, has_roomed, roomed, result, error);
}

/* txn_memtable_branch_versions_reading is identity_versions' read under the range's shared lock: the key's chain. */
static bool
txn_memtable_branch_versions_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
									 const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_branch_reading_t *reading = captures;
	TxnMemtableVersionChain chain;
	bool		some;

	(void) at;
	if (!txn_memtable_branch_chain(reading, hold, guard, &some, &chain, error))
		return false;
	while (some)
	{
		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &reading->version, error))
			return false;
		if (!some)
			break;
		reading->version_live = true;
		std_vec_push_Checked(reading->result, &reading->version, sizeof(reading->version),
							 _Alignof(TxnMemtableVersionVersion));
		reading->version_live = false;
	}
	return true;
}

/* txn_memtable_branch_versions_frame_0_t is identity_versions' state across its pad: the versions read. */
typedef struct txn_memtable_branch_versions_frame_0_t
{
	HostIoIo   *io;
	const HostDeltaHold *hold;
	HostSharedRecord deltas;
	const uint8 *key;
	size_t		key_len;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	bool		result_live;
	txn_memtable_branch_reading_t reading;
} txn_memtable_branch_versions_frame_0_t;

/* txn_memtable_branch_versions_pad_0 is identity_versions' pad: the versions read, while live. */
static void
txn_memtable_branch_versions_pad_0(void *frame_)
{
	txn_memtable_branch_versions_frame_0_t *frame = frame_;

	txn_memtable_branch_reading_drop(&frame->reading);
	if (frame->result_live)
	{
		frame->result_live = false;
		txn_memtable_version_versions_drop(frame->result, 0);
	}
}

/* txn_memtable_branch_versions_body_0 is identity_versions' body. */
static void
txn_memtable_branch_versions_body_0(void *frame_)
{
	txn_memtable_branch_versions_frame_0_t *frame = frame_;
	bool		read;

	std_vec_new(frame->result, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	frame->result_live = true;
	if (!txn_memtable_table_lent(&TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS, frame->io, frame->hold, frame->deltas,
								 frame->key, frame->key_len, txn_memtable_branch_versions_reading, &frame->reading, &read,
								 frame->error))
	{
		txn_memtable_branch_versions_pad_0(frame);
		return;
	}
	frame->result_live = false;
	frame->ok = true;
}

/*
 * txn_memtable_branch_identity_versions returns the versions of an identity
 * tree key in a working state's delta, newest first, which borrow the
 * caller's hold.
 */
bool
txn_memtable_branch_identity_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord deltas,
									  const uint8 *key, size_t key_len, StdVec *result, HostError *error)
{
	txn_memtable_branch_versions_frame_0_t frame = {.io = io, .hold = hold, .deltas = deltas, .key = key,
	.key_len = key_len, .result = result, .error = error, .reading = {.key = key, .key_len = key_len, .result = result}};

	std_landing(&frame, txn_memtable_branch_versions_body_0, txn_memtable_branch_versions_pad_0);
	return frame.ok;
}

/* txn_memtable_branch_identity_holds reports whether a working state's delta holds the version an identity tree key's record of an LSN pushed, so that its redo skips it. */
bool
txn_memtable_branch_identity_holds(HostIoIo *io, HostSharedRecord deltas, const uint8 *key, size_t key_len,
								   uint64 lsn, bool *result, HostError *error)
{
	return txn_memtable_table_held(&TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS, io, deltas, key, key_len, lsn, result,
								   error);
}

/* txn_memtable_branch_dropped_frame_0_t is dropped's state across its pad: the makers' guard, the sweep, and the delta claimed, while live. */
typedef struct txn_memtable_branch_dropped_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	HostError  *error;
	bool		ok;
	HostSharedGuard making;
	bool		making_live;
	StdVec		swept;
	bool		swept_live;
	HostSharedOwned owned;
	bool		owned_live;
} txn_memtable_branch_dropped_frame_0_t;

/* txn_memtable_branch_dropped_pad_0 is dropped's pad: the sweep, the makers' guard, and the delta claimed, while live. */
static void
txn_memtable_branch_dropped_pad_0(void *frame_)
{
	txn_memtable_branch_dropped_frame_0_t *frame = frame_;

	if (frame->swept_live)
	{
		frame->swept_live = false;
		std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
	if (frame->making_live)
	{
		frame->making_live = false;
		host_shared_Guard_drop(&frame->making);
	}
	if (frame->owned_live)
	{
		frame->owned_live = false;
		host_shared_Owned_drop(&frame->owned);
	}
}

/* txn_memtable_branch_dropped_claimed claims the repository's delta under the makers' lock, whatever processes hold it. */
static bool
txn_memtable_branch_dropped_claimed(txn_memtable_branch_dropped_frame_0_t *frame)
{
	HostSharedRecord makers;
	uint64		key = (uint64) frame->repository;

	if (!host_shared_single(&TXN_STATE_MAKERS_SINGLE_OPS, &makers, frame->error) ||
		!host_shared_Record_lock(makers, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_MAKING_INDEX, &frame->making,
								 frame->error))
		return false;
	frame->making_live = true;
	if (!host_shared_sweep(HOST_SHARED_GUARD_WITHIN(&frame->making), &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS,
						   TXN_MEMTABLE_BRANCH_REPOSITORY, &frame->swept, frame->error))
		return false;
	frame->swept_live = true;
	for (size_t k = 0; k < frame->swept.len; k++)
	{
		const std_TupleU64U64_t *swept = &STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k);
		bool		has;

		if (swept->f1 != key)
			continue;
		if (!host_shared_claim(HOST_SHARED_GUARD_WITHIN(&frame->making), &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS,
							   swept->f0, &has, &frame->owned, frame->error))
			return false;
		if (!has)
			continue;
		frame->owned_live = true;
		if (txn_memtable_branch_load(host_shared_Owned_record(&frame->owned), TXN_MEMTABLE_BRANCH_REPOSITORY) == key)
			break;
		frame->owned_live = false;
		host_shared_Owned_drop(&frame->owned);
	}
	frame->swept_live = false;
	std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->making_live = false;
	host_shared_Guard_drop(&frame->making);
	return true;
}

/* txn_memtable_branch_dropped_body_0 is dropped's body. */
static void
txn_memtable_branch_dropped_body_0(void *frame_)
{
	txn_memtable_branch_dropped_frame_0_t *frame = frame_;
	HostStorage marker = txn_state_marker(frame->repository);

	if (!txn_state_unlinked(frame->io, &marker, frame->error) || !txn_memtable_branch_dropped_claimed(frame))
	{
		txn_memtable_branch_dropped_pad_0(frame);
		return;
	}
	if (!frame->owned_live)
	{
		frame->ok = true;
		return;
	}
	if (!txn_memtable_table_retired(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, frame->io,
									host_shared_Owned_record(&frame->owned), frame->error) ||
		!txn_memtable_table_retired(&TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS, frame->io,
									host_shared_Owned_record(&frame->owned), frame->error))
	{
		txn_memtable_branch_dropped_pad_0(frame);
		return;
	}
	frame->owned_live = false;
	host_shared_free(&frame->owned);
	frame->ok = true;
}

/*
 * txn_memtable_branch_dropped frees a repository's delta at its drop, when no
 * process writes to it: it claims the delta under the makers' lock, whatever
 * processes hold it, then, with that lock released, retires its memtables
 * with their ranges and the pages only they name, and frees it, which goes
 * when the last holder closes it.
 */
bool
txn_memtable_branch_dropped(HostIoIo *io, uint32 repository, HostError *error)
{
	txn_memtable_branch_dropped_frame_0_t frame = {.io = io, .repository = repository, .error = error};

	std_landing(&frame, txn_memtable_branch_dropped_body_0, txn_memtable_branch_dropped_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/chunks/trailer.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/test.h"
#include "doltgres/txn/memtable/testing.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/written.h"

/* TXN_MEMTABLE_BRANCH_TESTS_REPOSITORY is the repository whose working state the tests' deltas are. */
#define TXN_MEMTABLE_BRANCH_TESTS_REPOSITORY 9350

/* txn_memtable_branch_tests_chunked_frame_0_t is chunked's state across its pad: its arguments, and its chunk's bytes, while live. */
typedef struct txn_memtable_branch_tests_chunked_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord deltas;
	const uint8 *address;
	TxnMemtableVersionWriter writer;
	StdBytes	chunk;
	bool		has_lsn;
	uint64		lsn;
	HostError  *error;
	bool		ok;
	StdVec		bytes;
	bool		bytes_live;
} txn_memtable_branch_tests_chunked_frame_0_t;

/* txn_memtable_branch_tests_chunked_pad_0 is chunked's pad: the chunk's bytes, while live. */
static void
txn_memtable_branch_tests_chunked_pad_0(void *frame_)
{
	txn_memtable_branch_tests_chunked_frame_0_t *frame = frame_;

	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
}

/* txn_memtable_branch_tests_chunked_inside is chunked's prepare's closure: the chunk's finish under its writer. */
static bool
txn_memtable_branch_tests_chunked_inside(void *captures, HostIoIo *io, TxnMemtableTablePreparedBranchDeltas *prepared,
										 HostError *error)
{
	txn_memtable_branch_tests_chunked_frame_0_t *frame = captures;
	TxnMemtableVersionHandle handle;

	return txn_memtable_branch_finish(io, prepared, frame->writer, &frame->bytes, frame->has_lsn, frame->lsn, &handle,
									  error);
}

/* txn_memtable_branch_tests_chunked_body_0 is chunked's body. */
static void
txn_memtable_branch_tests_chunked_body_0(void *frame_)
{
	txn_memtable_branch_tests_chunked_frame_0_t *frame = frame_;

	std_slice_to_vec_Checked(&frame->bytes, frame->chunk.ptr, frame->chunk.len, 1, 1);
	frame->bytes_live = true;
	frame->ok = txn_memtable_branch_prepare(frame->io, frame->deltas, frame->address, TXN_MEMTABLE_BRANCH_ADDRESS,
											txn_memtable_version_largest(frame->bytes.len),
											txn_memtable_branch_tests_chunked_inside, frame, NULL, frame->error);
	txn_memtable_branch_tests_chunked_pad_0(frame);
}

/*
 * txn_memtable_branch_tests_chunked pushes a chunk at an address into a
 * working state's delta through its prepare and finish under a writer, with
 * the end of its WAL record when it has one.
 */
static bool
txn_memtable_branch_tests_chunked(HostIoIo *io, HostSharedRecord deltas, const uint8 *address,
								  TxnMemtableVersionWriter writer, const char *chunk, bool has_lsn, uint64 lsn,
								  HostError *error)
{
	txn_memtable_branch_tests_chunked_frame_0_t frame = {.io = io, .deltas = deltas, .address = address,
	.writer = writer, .has_lsn = has_lsn, .lsn = lsn, .error = error};

	frame.chunk.ptr = (const uint8 *) chunk;
	frame.chunk.len = strlen(chunk);
	std_landing(&frame, txn_memtable_branch_tests_chunked_body_0, txn_memtable_branch_tests_chunked_pad_0);
	return frame.ok;
}

/* txn_memtable_branch_tests_chunks pushes a chunk, as chunked's unwrap does at its place. */
static void
txn_memtable_branch_tests_chunks(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord deltas, uint8 fill,
								 TxnMemtableVersionWriter writer, bool has_lsn, uint64 lsn)
{
	uint8		address[TXN_MEMTABLE_BRANCH_ADDRESS];
	HostError	error;

	memset(address, fill, sizeof(address));
	if (!txn_memtable_branch_tests_chunked(io, deltas, address, writer, "chunk", has_lsn, lsn, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/* txn_memtable_branch_tests_deltas_of holds a repository's working state's deltas, as deltas' unwrap does at its place. */
static HostSharedHeld
txn_memtable_branch_tests_deltas_of(uint32 line, uint32 col, HostIoIo *io, uint32 repository)
{
	HostSharedHeld held;
	HostError	error;

	if (!txn_memtable_branch_deltas(io, repository, HOST_PG_DEFAULT, &held, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return held;
}

/* txn_memtable_branch_tests_got_is asserts the chunk at an address, or none, as assert_eq on get's unwrap does at their places. */
static void
txn_memtable_branch_tests_got_is(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, HostIoIo *io,
								 HostSharedRecord deltas, uint8 fill, const char *expected)
{
	uint8		address[TXN_MEMTABLE_BRANCH_ADDRESS];
	HostError	error;
	StdVec		got;
	bool		some;
	StdBytes	got_bytes;
	StdBytes	wanted = {(const uint8 *) expected, expected != NULL ? strlen(expected) : 0};
	StdOptionRef left;
	StdOptionRef right = {expected != NULL, STD_ITEM(&wanted, std_bytes_Debug_fmt)};

	memset(address, fill, sizeof(address));
	if (!txn_memtable_branch_get(io, deltas, address, sizeof(address), &some, &got, &error))
		STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	got_bytes.ptr = got.ptr;
	got_bytes.len = some ? got.len : 0;
	left.has = some;
	left.some = STD_ITEM(&got_bytes, std_bytes_Debug_fmt);
	STD_ASSERT_EQ(line, col, some == (expected != NULL) &&
				  (!some || (got.len == wanted.len && memcmp(got.ptr, wanted.ptr, wanted.len) == 0)), &left,
				  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (some)
		std_vec_free_Checked(&got, 1, 1);
}

/* txn_memtable_branch_tests_reclaims runs BranchDeltas' reclaim, as its unwrap does at its place, answering whether it freed one. */
static bool
txn_memtable_branch_tests_reclaims(uint32 line, uint32 col, HostIoIo *io)
{
	HostError	error;
	bool		freed;

	if (!TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS.reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return freed;
}

/* txn_memtable_branch_tests_none_next is std::iter::Empty's next: no item. */
static bool
txn_memtable_branch_tests_none_next(void *self, void *result)
{
	return false;
}

/* txn_memtable_branch_tests_none_size_hint is std::iter::Empty's size_hint: no item left. */
static StdSizeHint
txn_memtable_branch_tests_none_size_hint(const void *self)
{
	StdSizeHint hint = {.has_upper = true};

	return hint;
}

/* TXN_MEMTABLE_BRANCH_TESTS_NONE_ITERATOR_OPS is std::iter::Empty's Iterator, of the pages a retirement of none takes. */
static const StdIteratorOps TXN_MEMTABLE_BRANCH_TESTS_NONE_ITERATOR_OPS = {
	.next = txn_memtable_branch_tests_none_next,
	.size_hint = txn_memtable_branch_tests_none_size_hint,
};

/* txn_memtable_branch_tests_used_is retires none and asserts the delta's use is back at its start, as retire's unwrap and assert_eq do at their places. */
static void
txn_memtable_branch_tests_used_is(uint32 unwrap_line, uint32 unwrap_col, uint32 line, uint32 col, HostIoIo *io,
								  uint64 start)
{
	StdIterator none = {&TXN_MEMTABLE_BRANCH_TESTS_NONE_ITERATOR_OPS};
	HostError	error;
	uint64		used;

	if (!host_delta_retire(io, NULL, 0, none, &error))
		STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	used = host_delta_used();
	STD_ASSERT_EQ(line, col, used == start, &used, std_u64_Debug_fmt, &start, std_u64_Debug_fmt);
}

/*
 * The chunk tree's case: a chunk pushed at its address reads back by it, an
 * address the delta doesn't hold reads nothing, the reclaim keeps the delta
 * while its memtable holds a chunk, and the repository's drop retires its
 * memtable and frees it, so the delta's use returns to its start.
 */
void
test_txn_memtable_branch_tests_a_chunk_reads_back_by_its_address_until_its_repositorys_drop(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_branch_tests_deltas_of(717, 63, io, TXN_MEMTABLE_BRANCH_TESTS_REPOSITORY);
	HostError	error;

	txn_memtable_branch_tests_chunks(718, 94, io, host_shared_Held_record(&held), 1,
									 TXN_MEMTABLE_VERSION_NO_TRANSACTION, false, 0);
	txn_memtable_branch_tests_got_is(719, 9, 719, 58, io, host_shared_Held_record(&held), 1, "chunk");
	txn_memtable_branch_tests_got_is(720, 9, 720, 58, io, host_shared_Held_record(&held), 2, NULL);
	host_shared_Held_drop(&held);
	STD_ASSERT(722, 9, !txn_memtable_branch_tests_reclaims(722, 44, io),
			   "!BranchDeltas::reclaim(io).unwrap()");
	held = txn_memtable_branch_tests_deltas_of(723, 63, io, TXN_MEMTABLE_BRANCH_TESTS_REPOSITORY);
	txn_memtable_branch_tests_got_is(724, 9, 724, 58, io, host_shared_Held_record(&held), 1, "chunk");
	host_shared_Held_drop(&held);
	if (!txn_memtable_branch_dropped(io, TXN_MEMTABLE_BRANCH_TESTS_REPOSITORY, &error))
		STD_RESULT_EXPECT_FAILED(726, 33, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_branch_tests_used_is(727, 52, 728, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_branch_tests_nothing is the reclaim case's prepare's closure, which pushes nothing. */
static bool
txn_memtable_branch_tests_nothing(void *captures, HostIoIo *io, TxnMemtableTablePreparedBranchDeltas *prepared,
								  HostError *error)
{
	return true;
}

/*
 * The chunk tree's reclaim case: a prepare whose window pushes no chunk
 * leaves the memtable it made to its settle, which closes it, and the
 * reclaim then frees the delta, so the delta's use returns to its start.
 */
void
test_txn_memtable_branch_tests_a_delta_whose_prepare_pushed_nothing_is_reclaimed(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_branch_tests_deltas_of(740, 63, io, 9351);
	uint8		address[TXN_MEMTABLE_BRANCH_ADDRESS];
	HostError	error;
	uint64		location;
	bool		has_memtable;
	StdOptionRef memtable;
	StdOptionRef none = {false};

	memset(address, 3, sizeof(address));
	if (!txn_memtable_branch_prepare(io, host_shared_Held_record(&held), address, sizeof(address), 64,
									 txn_memtable_branch_tests_nothing, NULL, NULL, &error))
		STD_RESULT_EXPECT_FAILED(741, 70, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	has_memtable = txn_memtable_table_memtable(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, host_shared_Held_record(&held),
											   &location);
	memtable.has = has_memtable;
	memtable.some = STD_ITEM(&location, std_u64_Debug_fmt);
	STD_ASSERT_EQ(742, 9, !has_memtable, &memtable, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	host_shared_Held_drop(&held);
	STD_ASSERT(744, 9, txn_memtable_branch_tests_reclaims(744, 43, io),
			   "BranchDeltas::reclaim(io).unwrap()");
	txn_memtable_branch_tests_used_is(745, 52, 746, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_branch_tests_durable_is runs durable, as its unwrap does at its place. */
static bool
txn_memtable_branch_tests_durable_is(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord deltas,
									 const uint8 *address)
{
	HostError	error;
	bool		durable;

	if (!txn_memtable_branch_durable(io, deltas, address, TXN_MEMTABLE_BRANCH_ADDRESS, &durable, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return durable;
}

/* txn_memtable_branch_tests_versions_t is the durable case's read's captures: its address, and the chunk's versions' (xid, lsn), a Vec<(u64, Option<u64>)>. */
typedef struct txn_memtable_branch_tests_versions_t
{
	const uint8 *address;
	StdVec		versions;
} txn_memtable_branch_tests_versions_t;

/* txn_memtable_branch_tests_version_t is a chunk version's (u64, Option<u64>): its writer's xid and its record's end. */
typedef struct txn_memtable_branch_tests_version_t
{
	uint64		xid;
	bool		has_lsn;
	uint64		lsn;
} txn_memtable_branch_tests_version_t;

/* txn_memtable_branch_tests_reading is the durable case's read: each of the chunk's versions' writer's xid and record's end, newest first. */
static bool
txn_memtable_branch_tests_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
								  const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_branch_tests_versions_t *c = captures;
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;
	TxnMemtableVersionVersion version;
	bool		some;

	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, c->address,
								 TXN_MEMTABLE_BRANCH_ADDRESS, &some, &node, error))
		return false;
	if (!some)
	{
		*error = txn_memtable_branch_internal("no chunk");
		return false;
	}
	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error))
		return false;
	std_vec_new(&c->versions, sizeof(txn_memtable_branch_tests_version_t),
				_Alignof(txn_memtable_branch_tests_version_t));
	for (;;)
	{
		TxnMemtableVersionRecord record;
		txn_memtable_branch_tests_version_t entry;

		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, error))
		{
			std_vec_free_Checked(&c->versions, sizeof(entry), _Alignof(txn_memtable_branch_tests_version_t));
			return false;
		}
		if (!some)
			return true;
		record = txn_memtable_version_Version_record(&version);
		entry.xid = txn_memtable_version_Record_writer(&record).xid;
		entry.has_lsn = txn_memtable_version_Record_lsn(&record, &entry.lsn);
		txn_memtable_version_Version_drop(&version);
		std_vec_push_Checked(&c->versions, &entry, sizeof(entry), _Alignof(txn_memtable_branch_tests_version_t));
	}
}

/* txn_memtable_branch_tests_version_Debug_fmt is a chunk version's (u64, Option<u64>)'s Debug. */
static bool
txn_memtable_branch_tests_version_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_branch_tests_version_t *version = value;
	StdOptionRef lsn = {version->has_lsn, STD_ITEM(&version->lsn, std_u64_Debug_fmt)};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&version->xid, std_u64_Debug_fmt), STD_ITEM(&lsn, std_option_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_memtable_branch_tests_versions_Debug_fmt is a Vec<(u64, Option<u64>)>'s Debug. */
static bool
txn_memtable_branch_tests_versions_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *versions = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < versions->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(versions, txn_memtable_branch_tests_version_t, k),
							txn_memtable_branch_tests_version_Debug_fmt);
	return std_DebugList_finish(&list);
}

/*
 * V2's case and PLAN's chunk spill row: a chunk's versions name the writer
 * that pushed them, and a chunk an unlogged write pushed isn't durable until a
 * logged write pushes its own copy at the address, which carries its
 * record's end; the bytes read back either way.
 */
void
test_txn_memtable_branch_tests_a_chunks_versions_name_their_writer_and_are_durable_once_logged(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_branch_tests_deltas_of(757, 83, io, 9354);
	HostSharedRecord deltas = host_shared_Held_record(&held);
	TxnMemtableVersionWriter writer = {.xid = 7};
	uint8		address[TXN_MEMTABLE_BRANCH_ADDRESS];
	txn_memtable_branch_tests_versions_t captures;
	txn_memtable_branch_tests_version_t wanted[2] = {{7, true, UINT64CONST(0x100000a28)}, {7, false, 0}};
	StdVec		expected = {wanted, 2, 2};
	StdOptionRef left;
	StdOptionRef right = {true, STD_ITEM(&expected, txn_memtable_branch_tests_versions_Debug_fmt)};
	HostError	error;
	bool		read;
	bool		eq;

	memset(address, 4, sizeof(address));
	captures.address = address;
	txn_memtable_branch_tests_chunks(760, 14, io, deltas, 4, writer, false, 0);
	STD_ASSERT(761, 9, !txn_memtable_branch_tests_durable_is(761, 55, io, deltas, address),
			   "!durable(io, held.record(), &address).unwrap()");
	txn_memtable_branch_tests_chunks(769, 10, io, deltas, 4, writer, true, UINT64CONST(0x100000a28));
	STD_ASSERT(770, 9, txn_memtable_branch_tests_durable_is(770, 54, io, deltas, address),
			   "durable(io, held.record(), &address).unwrap()");
	txn_memtable_branch_tests_got_is(771, 9, 771, 53, io, deltas, 4, "chunk");
	if (!txn_memtable_table_read(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, io, deltas, address, sizeof(address),
								 txn_memtable_branch_tests_reading, &captures, &read, &error))
		STD_RESULT_EXPECT_FAILED(778, 87, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = read;
	left.some = STD_ITEM(&captures.versions, txn_memtable_branch_tests_versions_Debug_fmt);
	eq = read && captures.versions.len == 2;
	for (size_t k = 0; k < 2 && eq; k++)
	{
		const txn_memtable_branch_tests_version_t *v = &STD_VEC_AT(&captures.versions,
																   txn_memtable_branch_tests_version_t, k);

		eq = v->xid == wanted[k].xid && v->has_lsn == wanted[k].has_lsn && (!v->has_lsn || v->lsn == wanted[k].lsn);
	}
	STD_ASSERT_EQ(779, 9, eq, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (read)
		std_vec_free_Checked(&captures.versions, sizeof(txn_memtable_branch_tests_version_t),
							 _Alignof(txn_memtable_branch_tests_version_t));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_branch_tests_held_bools_Debug_fmt is a [bool; 3]'s Debug. */
bool
txn_memtable_branch_tests_held_bools_Debug_fmt(const void *value, StdFormatter *f)
{
	const bool *held = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < 3; k++)
		std_DebugList_entry(&list, &held[k], std_bool_Debug_fmt);
	return std_DebugList_finish(&list);
}

/*
 * I12's test for chunks: a chunk pushed with its record's end is held by
 * that LSN at its address, not by another LSN or at another address, and an
 * unlogged copy is held by none.
 */
void
test_txn_memtable_branch_tests_a_chunk_is_held_by_its_records_lsn(void)
{
	static const uint8 fills[3] = {6, 6, 7};
	static const uint64 lsns[3] = {UINT64CONST(0x100000a28), UINT64CONST(0x100000a29), UINT64CONST(0x100000a28)};
	static const bool expected[3] = {true, false, false};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_branch_tests_deltas_of(789, 83, io, 9362);
	HostSharedRecord deltas = host_shared_Held_record(&held);
	HostError	error;
	bool		found[3];

	txn_memtable_branch_tests_chunks(790, 109, io, deltas, 6, TXN_MEMTABLE_VERSION_NO_TRANSACTION, true,
									 UINT64CONST(0x100000a28));
	txn_memtable_branch_tests_chunks(791, 94, io, deltas, 7, TXN_MEMTABLE_VERSION_NO_TRANSACTION, false, 0);
	for (size_t k = 0; k < 3; k++)
	{
		uint8		address[TXN_MEMTABLE_BRANCH_ADDRESS];

		memset(address, fills[k], sizeof(address));
		if (!txn_memtable_branch_holds(io, deltas, address, sizeof(address), lsns[k], &found[k], &error))
			STD_RESULT_EXPECT_FAILED(793, 75, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	}
	STD_ASSERT_EQ(794, 9, found[0] == expected[0] && found[1] == expected[1] && found[2] == expected[2],
				  found, txn_memtable_branch_tests_held_bools_Debug_fmt, expected,
				  txn_memtable_branch_tests_held_bools_Debug_fmt);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_branch_tests_wrote is the abort case's wrote: the chunk's writer of a transaction's xid. */
static TxnMemtableVersionWriter
txn_memtable_branch_tests_wrote(HostXid xid)
{
	TxnMemtableVersionWriter writer = {.xid = xid.f0};

	return writer;
}

/* txn_memtable_branch_tests_writers_t is the abort case's read's captures: its address, and the chunk's versions' writers' xids, a Vec<u64>. */
typedef struct txn_memtable_branch_tests_writers_t
{
	const uint8 *address;
	StdVec		writers;
} txn_memtable_branch_tests_writers_t;

/* txn_memtable_branch_tests_writers_reading is the abort case's read: each of the chunk's versions' writer's xid, newest first. */
static bool
txn_memtable_branch_tests_writers_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
										  const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_branch_tests_writers_t *c = captures;
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;
	TxnMemtableVersionVersion version;
	bool		some;

	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, c->address,
								 TXN_MEMTABLE_BRANCH_ADDRESS, &some, &node, error))
		return false;
	if (!some)
	{
		*error = txn_memtable_branch_internal("no chunk");
		return false;
	}
	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error))
		return false;
	std_vec_new(&c->writers, sizeof(uint64), _Alignof(uint64));
	for (;;)
	{
		TxnMemtableVersionRecord record;
		uint64		xid;

		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, error))
		{
			std_vec_free_Checked(&c->writers, sizeof(uint64), _Alignof(uint64));
			return false;
		}
		if (!some)
			return true;
		record = txn_memtable_version_Version_record(&version);
		xid = txn_memtable_version_Record_writer(&record).xid;
		txn_memtable_version_Version_drop(&version);
		std_vec_push_Checked(&c->writers, &xid, sizeof(xid), _Alignof(uint64));
	}
}

/*
 * 2g's chunk abort: an aborted transaction's take-back prunes its own
 * version of a chunk and leaves the version a committed transaction wrote,
 * so the chunk a committed row names reads back.
 */
void
test_txn_memtable_branch_tests_an_abort_leaves_the_chunk_a_committed_transaction_wrote(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("the abort didn't take back its own version alone")};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_branch_tests_deltas_of(805, 63, io, 9389);
	HostSharedRecord deltas = host_shared_Held_record(&held);
	HostXid		committed = simc_begin();
	HostXid		aborted = simc_begin();
	uint8		address[TXN_MEMTABLE_BRANCH_ADDRESS];
	HostStorage chunks;
	HostError	error;
	uint64		touched;
	txn_memtable_branch_tests_writers_t captures;
	StdVec		expected = {&committed.f0, 1, 1};
	StdOptionRef left;
	StdOptionRef right = {true, STD_ITEM(&expected, chunks_trailer_VecU64_Debug_fmt)};
	StdFmtArguments message = {pieces, lengthof(pieces), NULL};
	bool		read;

	memset(address, 4, sizeof(address));
	captures.address = address;
	txn_memtable_branch_tests_chunks(808, 70, io, deltas, 4, txn_memtable_branch_tests_wrote(committed), false, 0);
	simc_commit(committed);
	chunks = txn_spill_chunks_storage(9389);
	if (!txn_written_touched(io, aborted.f0, &chunks, TXN_WRITTEN_OF_CHUNKS, address, sizeof(address), 0, &touched,
							 &error))
		STD_RESULT_EXPECT_FAILED(811, 103, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_branch_tests_chunks(812, 68, io, deltas, 4, txn_memtable_branch_tests_wrote(aborted), false, 0);
	simc_abort(aborted);
	if (!txn_written_aborted(io, &error))
		STD_RESULT_EXPECT_FAILED(814, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_branch_tests_got_is(815, 9, 815, 58, io, deltas, 4, "chunk");
	if (!txn_memtable_table_read(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, io, deltas, address, sizeof(address),
								 txn_memtable_branch_tests_writers_reading, &captures, &read, &error))
		STD_RESULT_EXPECT_FAILED(820, 109, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = read;
	left.some = STD_ITEM(&captures.writers, chunks_trailer_VecU64_Debug_fmt);
	if (!(read && captures.writers.len == 1 && STD_VEC_AT(&captures.writers, uint64, 0) == committed.f0))
		std_assert_failed(RUST_FILE, 821, 9, "==", &left, std_option_Debug_fmt, &right,
						  std_option_Debug_fmt, &message);
	if (read)
		std_vec_free_Checked(&captures.writers, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	if (!txn_memtable_branch_dropped(io, 9389, &error))
		STD_RESULT_EXPECT_FAILED(823, 33, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_TXN */
