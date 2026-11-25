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
 * The C port of txn's roots.rs: a collection's roots in a repository's
 * states and deltas, its chunk spills among them, what a reader can reach
 * through shared memory rather than a manifest.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/header.h"
#include "doltgres/host/delta.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/roots.h"
#include "doltgres/txn/runs.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/values.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "roots.rs"

/* txn_roots_error returns an error of the roots of a SQLSTATE and a message. */
static HostError
txn_roots_error(const char *sqlstate, const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) sqlstate, &message);
}

/* txn_roots_Captured_drop is Captured's drop glue: its roots and its outside addresses. */
void
txn_roots_Captured_drop(TxnRootsCaptured *self)
{
	std_vec_free_Checked(&self->roots, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_vec_free_Checked(&self->outside, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
}

/* txn_roots_frame_0_t is roots' state across its pad: what it captured and what each step holds, while live. */
typedef struct txn_roots_frame_0_t
{
	HostIoIo   *io;
	uint32		database;
	HostLsn		redo;
	TxnRootsCaptured *result;
	HostError  *error;
	bool		ok;
	TxnRootsCaptured captured;
	bool		captured_live;
	/* states is a Vec<(u64, Held<State>)>, of which moved are taken. */
	StdVec		states;
	bool		states_live;
	size_t		moved;
	HostSharedHeld held;
	bool		held_live;
	TxnBaseBase base;
	bool		base_live;
	StdVec		bytes;
	StdVec		addresses;
	bool		encoded_live;
	HostDeltaHold hold;
	bool		hold_live;
	/* versions is a Vec<Version>. */
	StdVec		versions;
	bool		versions_live;
	/* piece is a node's value or a chunk's bytes, a Vec<u8>. */
	StdVec		piece;
	bool		piece_live;
	/* named is a Vec<Address> a chunk names. */
	StdVec		named;
	bool		named_live;
	txn_TupleVecU8VecVersion_t spilled;
	bool		spilled_live;
	/* after is the key a chunk spills' walk passed, a Vec<u8>. */
	StdVec		after;
	bool		after_live;
} txn_roots_frame_0_t;

/* txn_roots_step_drop drops what one step of roots holds: a key's versions, its hold, a chunk's bytes and names, and a spilled key. */
static void
txn_roots_step_drop(txn_roots_frame_0_t *frame)
{
	if (frame->named_live)
	{
		frame->named_live = false;
		std_vec_free_Checked(&frame->named, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	}
	if (frame->piece_live)
	{
		frame->piece_live = false;
		std_vec_free_Checked(&frame->piece, 1, 1);
	}
	if (frame->spilled_live)
	{
		frame->spilled_live = false;
		std_vec_free_Checked(&frame->spilled.f0, 1, 1);
		txn_memtable_version_versions_drop(&frame->spilled.f1, 0);
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

/* txn_roots_pad_0 is roots' pad: a step's holdings, the base's encoding, the base, the handle, the states, and what it captured, while live. */
static void
txn_roots_pad_0(void *frame_)
{
	txn_roots_frame_0_t *frame = frame_;

	txn_roots_step_drop(frame);
	if (frame->after_live)
	{
		frame->after_live = false;
		std_vec_free_Checked(&frame->after, 1, 1);
	}
	if (frame->encoded_live)
	{
		frame->encoded_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
		std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	}
	if (frame->base_live)
	{
		frame->base_live = false;
		txn_base_Base_drop(&frame->base);
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->states_live)
	{
		frame->states_live = false;
		for (size_t k = frame->moved; k < frame->states.len; k++)
			host_shared_Held_drop(&STD_VEC_AT(&frame->states, txn_TupleU64HeldState_t, k).f1);
		std_vec_free_Checked(&frame->states, sizeof(txn_TupleU64HeldState_t), _Alignof(txn_TupleU64HeldState_t));
	}
	if (frame->captured_live)
	{
		frame->captured_live = false;
		txn_roots_Captured_drop(&frame->captured);
	}
}

/* txn_roots_root adds a root to the captured roots. */
static bool
txn_roots_root(txn_roots_frame_0_t *frame, ChunksAddressAddress address, ChunksTrailerLocator locator,
			   HostError *error)
{
	chunks_TupleAddressLocator_t root = {address, locator};

	if (!std_vec_try_reserve(&frame->captured.roots, 1, sizeof(root), _Alignof(chunks_TupleAddressLocator_t)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(&frame->captured.roots, &root, sizeof(root), _Alignof(chunks_TupleAddressLocator_t));
	return true;
}

/*
 * txn_roots_named adds to the roots the address a record's bytes hold at an
 * offset its address table gives, with its locator, so every chunk a record
 * names is a root whatever field names it.
 */
static bool
txn_roots_named(txn_roots_frame_0_t *frame, const uint8 *bytes, size_t bytes_len, chunks_TupleU32Locator_t named,
				HostError *error)
{
	size_t		at = (size_t) named.f0;
	size_t		end = STD_ADD(usize, at, 32, 48, 37);
	ChunksAddressAddress address;

	if (end > bytes_len)
	{
		*error = txn_roots_error("XX001", "a record's address runs past its bytes");
		return false;
	}
	memcpy(address.f0, bytes + at, 32);
	return txn_roots_root(frame, address, named.f1, error);
}

/* txn_roots_row_versions adds the addresses each of a row key's versions names. */
static bool
txn_roots_row_versions(txn_roots_frame_0_t *frame, const StdVec *versions, HostError *error)
{
	for (size_t k = 0; k < versions->len; k++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&STD_VEC_AT(versions,
																						  TxnMemtableVersionVersion,
																						  k));
		TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);
		const TxnMemtableVersionPutRef *put;
		TxnMemtableVersionAddresses addresses;
		chunks_TupleU32Locator_t named;

		if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_PUT)
			put = &op.u.put;
		else if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_ADD)
			put = &op.u.add;
		else if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW)
			put = &op.u.add_row;
		else if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW)
			put = &op.u.update_row.f1;
		else
			continue;
		addresses = txn_memtable_version_PutRef_addresses(put);
		while (txn_memtable_version_Addresses_Iterator_next(&addresses, &named))
			if (!txn_roots_named(frame, put->value.ptr, put->value.len, named, error))
				return false;
	}
	return true;
}

/* txn_roots_rows_each is the walk of a state's rows: the addresses each row version's record names. */
static bool
txn_roots_rows_each(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
					size_t values_len, HostError *error)
{
	txn_roots_frame_0_t *frame = captures;

	(void) values;
	(void) values_len;
	if (!host_delta_hold(io, &frame->hold, error))
		return false;
	frame->hold_live = true;
	if (!txn_memtable_table_versions(io, &frame->hold, host_shared_Held_record(&frame->held), key, key_len,
									 &frame->versions, error))
		return false;
	frame->versions_live = true;
	if (!txn_roots_row_versions(frame, &frame->versions, error))
		return false;
	txn_roots_step_drop(frame);
	return true;
}

/* txn_roots_value_versions adds the blob root each of a value map key's versions' entry names. */
static bool
txn_roots_value_versions(txn_roots_frame_0_t *frame, const StdVec *versions, HostError *error)
{
	for (size_t k = 0; k < versions->len; k++)
	{
		bool		some;
		StdBytes	value;
		TxnValuesEntry entry;

		if (!txn_memtable_map_valued(&STD_VEC_AT(versions, TxnMemtableVersionVersion, k), &some, &value, error))
			return false;
		if (!some)
			continue;
		if (!txn_values_Entry_decode(value.ptr, value.len, &entry, error) ||
			!txn_roots_root(frame, entry.address, entry.locator, error))
			return false;
	}
	return true;
}

/* txn_roots_values_each is the walk of a state's value map: the blob root each value map version's entry names. */
static bool
txn_roots_values_each(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
					  size_t values_len, HostError *error)
{
	txn_roots_frame_0_t *frame = captures;

	(void) values;
	(void) values_len;
	if (key_len != 4)
	{
		*error = txn_roots_error("XX001", "a value map's key isn't four bytes");
		return false;
	}
	if (!host_delta_hold(io, &frame->hold, error))
		return false;
	frame->hold_live = true;
	if (!txn_memtable_map_versions(&TXN_MEMTABLE_MAP_VALUES_TREE_OPS, io, &frame->hold,
								   host_shared_Held_record(&frame->held), std_u32_from_be_bytes(key), &frame->versions,
								   error))
		return false;
	frame->versions_live = true;
	if (!txn_roots_value_versions(frame, &frame->versions, error))
		return false;
	txn_roots_step_drop(frame);
	return true;
}

/*
 * txn_roots_state_spills walks a state's spills of a kind in key order and
 * adds what each spilled version names, as Doltgrust's delta_addresses reads
 * every spill run after the memtable: a key a spill took wholly out of the
 * memtable is in none of the memtable's walks (bugs row 398).
 */
static bool
txn_roots_state_spills(txn_roots_frame_0_t *frame, TxnRunsKind kind)
{
	HostSharedRecord record = host_shared_Held_record(&frame->held);

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return false;
	frame->hold_live = true;
	for (;;)
	{
		bool		some;

		if (!txn_spill_next(&TXN_SPILL_STATE_HOME_OPS, frame->io, &frame->hold, record, kind, frame->after_live,
							frame->after.ptr, frame->after.len, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, NULL, &some,
							&frame->spilled, frame->error))
			return false;
		if (!some)
			break;
		frame->spilled_live = true;
		if (kind == TXN_RUNS_KIND_SPILLED_VALUES)
		{
			if (frame->spilled.f0.len != 4)
			{
				*frame->error = txn_roots_error("XX001", "a value map's key isn't four bytes");
				return false;
			}
			if (!txn_roots_value_versions(frame, &frame->spilled.f1, frame->error))
				return false;
		}
		else if (!txn_roots_row_versions(frame, &frame->spilled.f1, frame->error))
			return false;
		if (frame->after_live)
			std_vec_free_Checked(&frame->after, 1, 1);
		frame->after = frame->spilled.f0;
		frame->after_live = true;
		frame->spilled_live = false;
		txn_memtable_version_versions_drop(&frame->spilled.f1, 0);
	}
	if (frame->after_live)
	{
		frame->after_live = false;
		std_vec_free_Checked(&frame->after, 1, 1);
	}
	txn_roots_step_drop(frame);
	return true;
}

/* txn_roots_outside_find finds an address in the sorted outside addresses, or where it goes. */
static bool
txn_roots_outside_find(const StdVec *outside, const ChunksAddressAddress *address, size_t *at)
{
	size_t		low = 0;
	size_t		high = outside->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		int			cmp = memcmp(STD_VEC_AT(outside, ChunksAddressAddress, mid).f0, address->f0, 32);

		if (cmp == 0)
		{
			*at = mid;
			return true;
		}
		if (cmp < 0)
			low = mid + 1;
		else
			high = mid;
	}
	*at = low;
	return false;
}

/*
 * txn_roots_outside_chunk adds to a collection's roots each address a chunk
 * no pack holds names, a chunk of the delta of chunks or of its spills, and
 * the chunk's own address to those captured outside.
 */
static bool
txn_roots_outside_chunk(txn_roots_frame_0_t *frame, ChunksAddressAddress address, const uint8 *hashed,
						size_t hashed_len, HostError *error)
{
	ChunksErrorError failed;
	ChunksTrailerLocator unknown = chunks_trailer_Locator_Default_default();
	size_t		at;

	if (!chunks_header_addresses(hashed, hashed_len, &frame->named, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	frame->named_live = true;
	if (!std_vec_try_reserve(&frame->captured.roots, frame->named.len, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	for (size_t k = 0; k < frame->named.len; k++)
	{
		chunks_TupleAddressLocator_t root = {STD_VEC_AT(&frame->named, ChunksAddressAddress, k), unknown};

		std_vec_push_Checked(&frame->captured.roots, &root, sizeof(root), _Alignof(chunks_TupleAddressLocator_t));
	}
	frame->named_live = false;
	std_vec_free_Checked(&frame->named, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (!txn_roots_outside_find(&frame->captured.outside, &address, &at))
		std_vec_insert(&frame->captured.outside, at, &address, sizeof(address), _Alignof(ChunksAddressAddress),
					   RUST_FILE, 143, 5);
	return true;
}

/* txn_roots_chunks_each is the walk of the delta of chunks: the addresses each chunk it holds names, a built node's gathered from its pieces. */
static bool
txn_roots_chunks_each(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
					  size_t values_len, HostError *error)
{
	txn_roots_frame_0_t *frame = captures;
	HostSharedRecord record = host_shared_Held_record(&frame->held);
	ChunksAddressAddress address;
	uint8		node[TXN_MEMTABLE_BRANCH_NODE_KEY];
	bool		some;

	(void) values;
	(void) values_len;
	if (key_len < TXN_MEMTABLE_BRANCH_ADDRESS)
	{
		*error = txn_roots_error("XX001", "a delta of chunks' key isn't an address");
		return false;
	}
	memcpy(address.f0, key, TXN_MEMTABLE_BRANCH_ADDRESS);
	if (txn_memtable_branch_first_piece(key, key_len, node))
	{
		txn_TupleU64VecU8_t gathered;

		if (!txn_memtable_branch_node(io, record, node, &some, &gathered, error))
			return false;
		if (some)
			frame->piece = gathered.f1;
	}
	else if (key_len == TXN_MEMTABLE_BRANCH_PIECE_KEY)
		return true;
	else if (!txn_memtable_branch_get(io, record, key, key_len, &some, &frame->piece, error))
		return false;
	if (!some)
		return true;
	frame->piece_live = true;
	if (!txn_roots_outside_chunk(frame, address, frame->piece.ptr, frame->piece.len, error))
		return false;
	txn_roots_step_drop(frame);
	return true;
}

/* txn_roots_chunk_spills walks a working state's chunk spills in key order, each key's newest version's chunk outside the packs, a built node's gathered from its pieces. */
static bool
txn_roots_chunk_spills(txn_roots_frame_0_t *frame)
{
	HostSharedRecord record = host_shared_Held_record(&frame->held);

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return false;
	frame->hold_live = true;
	for (;;)
	{
		bool		some;
		ChunksAddressAddress address;
		TxnMemtableVersionRecord newest;
		TxnMemtableVersionOpRef op;
		uint8		node[TXN_MEMTABLE_BRANCH_NODE_KEY];
		txn_TupleU64VecU8_t gathered;

		if (!txn_spill_next(&TXN_SPILL_BRANCH_DELTAS_HOME_OPS, frame->io, &frame->hold, record,
							TXN_RUNS_KIND_SPILLED_CHUNKS, frame->after_live, frame->after.ptr, frame->after.len,
							TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, NULL, &some, &frame->spilled, frame->error))
			return false;
		if (!some)
			return true;
		frame->spilled_live = true;
		if (frame->spilled.f0.len != TXN_MEMTABLE_BRANCH_ADDRESS &&
			frame->spilled.f0.len != TXN_MEMTABLE_BRANCH_PIECE_KEY)
		{
			*frame->error = txn_roots_error("XX001", "a chunk spill's key isn't an address");
			return false;
		}
		memcpy(address.f0, frame->spilled.f0.ptr, TXN_MEMTABLE_BRANCH_ADDRESS);
		if (txn_memtable_branch_first_piece(frame->spilled.f0.ptr, frame->spilled.f0.len, node))
		{
			if (!txn_memtable_branch_node(frame->io, record, node, &some, &gathered, frame->error))
				return false;
			if (some)
			{
				frame->piece = gathered.f1;
				frame->piece_live = true;
				if (!txn_roots_outside_chunk(frame, address, frame->piece.ptr, frame->piece.len, frame->error))
					return false;
				frame->piece_live = false;
				std_vec_free_Checked(&frame->piece, 1, 1);
			}
		}
		else if (frame->spilled.f0.len == TXN_MEMTABLE_BRANCH_ADDRESS && frame->spilled.f1.len > 0)
		{
			newest = txn_memtable_version_Version_record(&STD_VEC_AT(&frame->spilled.f1, TxnMemtableVersionVersion, 0));
			op = txn_memtable_version_Record_op(&newest);
			if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_PUT &&
				!txn_roots_outside_chunk(frame, address, op.u.put.value.ptr, op.u.put.value.len, frame->error))
				return false;
		}
		if (frame->after_live)
			std_vec_free_Checked(&frame->after, 1, 1);
		frame->after = frame->spilled.f0;
		frame->after_live = true;
		frame->spilled_live = false;
		txn_memtable_version_versions_drop(&frame->spilled.f1, 0);
	}
}

static bool txn_roots_gathered(txn_roots_frame_0_t *frame);

/*
 * txn_roots_state gathers the roots of one state: its newest base record's
 * addresses, then its rows' and its value map's versions, the memtable's and
 * then its spills', as Doltgrust's capture reads bases before the delta. It
 * gathers them again when an install moved the state's base generation past
 * the one it took before the base, since that install's base may be the only
 * namer of a spill the walk then passed over, and its drop may have taken
 * versions out; with the generation unmoved, the base it read is the one
 * every spill it passed over is folded into (bugs row 398).
 */
static bool
txn_roots_state(txn_roots_frame_0_t *frame)
{
	HostSharedRecord record = host_shared_Held_record(&frame->held);
	size_t		start = frame->captured.roots.len;
	uint64		generation;

	do
	{
		frame->captured.roots.len = start;
		generation = txn_state_generation(record);
		if (!txn_roots_gathered(frame))
			return false;
	} while (txn_state_moved_past(record, generation));
	return true;
}

/* txn_roots_gathered gathers one state's roots once: txn_roots_state's work between its generation's reads. */
static bool
txn_roots_gathered(txn_roots_frame_0_t *frame)
{
	HostSharedRecord record = host_shared_Held_record(&frame->held);
	bool		some;

	if (!txn_state_newest(frame->io, &frame->held, &some, &frame->base, frame->error))
		return false;
	if (some)
	{
		frame->base_live = true;
		txn_base_Base_encode(&frame->base, &frame->bytes, &frame->addresses);
		frame->encoded_live = true;
		for (size_t k = 0; k < frame->addresses.len; k++)
			if (!txn_roots_named(frame, frame->bytes.ptr, frame->bytes.len,
								 STD_VEC_AT(&frame->addresses, chunks_TupleU32Locator_t, k), frame->error))
				return false;
		frame->encoded_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
		std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
		frame->base_live = false;
		txn_base_Base_drop(&frame->base);
	}
	return txn_memtable_table_walked(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, record, txn_roots_rows_each, frame,
									 NULL, frame->error) &&
		txn_memtable_table_walked(&TXN_MEMTABLE_MAP_VALUES_TREE_OPS, frame->io, record, txn_roots_values_each, frame,
								  NULL, frame->error) &&
		txn_roots_state_spills(frame, TXN_RUNS_KIND_SPILLED_ROWS) &&
		txn_roots_state_spills(frame, TXN_RUNS_KIND_SPILLED_VALUES);
}

/*
 * txn_roots_Wal is a database's roots the WAL names past a redo point, as
 * Doltgrust's WalRoots keeps them: the redo point, the end of the last record
 * read, and the roots, a Vec<Root>.
 */
typedef struct txn_roots_Wal
{
	uint32		database;
	HostLsn		redo;
	HostLsn		position;
	StdVec		roots;
} txn_roots_Wal;

/* txn_roots_wals is the capturing process's Vec<Wal>, one for each database it captured, read on at each capture. */
static StdVec txn_roots_wals;

/* txn_roots_wals_made is whether txn_roots_wals was made. */
static bool txn_roots_wals_made;

/* TXN_ROOTS_ROOT is a Root's size and alignment, for a Vec<Root>. */
#define TXN_ROOTS_ROOT sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t)

/* txn_roots_wal_root adds a root to a WAL's roots. */
static void
txn_roots_wal_root(StdVec *roots, ChunksAddressAddress address, ChunksTrailerLocator locator)
{
	chunks_TupleAddressLocator_t root = {address, locator};

	std_vec_push_Checked(roots, &root, TXN_ROOTS_ROOT);
}

/* txn_roots_wal_named adds the address each offset of a record's bytes holds, with its locator. */
static bool
txn_roots_wal_named(StdVec *roots, const uint8 *bytes, size_t bytes_len, const StdVec *named, HostError *error)
{
	for (size_t k = 0; k < named->len; k++)
	{
		const chunks_TupleU32Locator_t *at = &STD_VEC_AT(named, chunks_TupleU32Locator_t, k);
		ChunksAddressAddress address;

		if (at->f0 > bytes_len || bytes_len - at->f0 < sizeof(address.f0))
		{
			*error = txn_roots_error("XX001", "a record's address runs past its bytes");
			return false;
		}
		memcpy(address.f0, bytes + at->f0, sizeof(address.f0));
		txn_roots_wal_root(roots, address, at->f1);
	}
	return true;
}

/* txn_roots_wal_children adds each chunk a chunk's hashed bytes name, by its address alone. */
static bool
txn_roots_wal_children(StdVec *roots, const uint8 *hashed, size_t hashed_len, HostError *error)
{
	StdVec		named;
	ChunksErrorError failed;

	if (!chunks_header_addresses(hashed, hashed_len, &named, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	for (size_t k = 0; k < named.len; k++)
		txn_roots_wal_root(roots, STD_VEC_AT(&named, ChunksAddressAddress, k), chunks_trailer_Locator_Default_default());
	std_vec_free_Checked(&named, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	return true;
}

/* txn_roots_wal_frame_0_t is wal's state across its pad: a record read, and a record decoded from it, while live. */
typedef struct txn_roots_wal_frame_0_t
{
	HostIoIo   *io;
	txn_roots_Wal *wal;
	HostError  *error;
	bool		ok;
	HostForkRecord record;
	bool		record_live;
	TxnRecordBaseAdvanced advanced;
	bool		advanced_live;
	TxnRecordRowChange change;
	bool		change_live;
	TxnRecordChunkWrite chunk;
	bool		chunk_live;
	TxnRecordMapCarried carried;
	bool		carried_live;
	StdVec		bytes;
	StdVec		addresses;
	bool		encoded_live;
} txn_roots_wal_frame_0_t;

/* txn_roots_wal_decoded_drop drops the record wal decoded and its encoded base, while live. */
static void
txn_roots_wal_decoded_drop(txn_roots_wal_frame_0_t *frame)
{
	if (frame->encoded_live)
	{
		std_vec_free_Checked(&frame->bytes, 1, 1);
		std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	}
	if (frame->advanced_live)
		txn_record_BaseAdvanced_drop(&frame->advanced);
	if (frame->change_live)
		txn_record_RowChange_drop(&frame->change);
	if (frame->chunk_live)
		txn_record_ChunkWrite_drop(&frame->chunk);
	if (frame->carried_live)
		txn_record_MapCarried_drop(&frame->carried);
	frame->encoded_live = frame->advanced_live = frame->change_live = frame->chunk_live = false;
	frame->carried_live = false;
}

/* txn_roots_wal_pad_0 is wal's pad: the decoded record and the record read, while live. */
static void
txn_roots_wal_pad_0(void *frame_)
{
	txn_roots_wal_frame_0_t *frame = frame_;

	txn_roots_wal_decoded_drop(frame);
	if (frame->record_live)
		std_vec_free_Checked(&frame->record.f2, 1, 1);
	frame->record_live = false;
}

/* txn_roots_wal_advanced adds the roots of a fold's or a build's base: what its record's address table names. */
static bool
txn_roots_wal_advanced(txn_roots_wal_frame_0_t *frame, const uint8 *body, size_t body_len)
{
	if (!txn_record_BaseAdvanced_decode(body, body_len, &frame->advanced, frame->error))
		return false;
	frame->advanced_live = true;
	if (frame->advanced.storage.database == frame->wal->database)
	{
		txn_base_Base_encode(&frame->advanced.base, &frame->bytes, &frame->addresses);
		frame->encoded_live = true;
		if (!txn_roots_wal_named(&frame->wal->roots, frame->bytes.ptr, frame->bytes.len, &frame->addresses,
								 frame->error))
			return false;
	}
	txn_roots_wal_decoded_drop(frame);
	return true;
}

/* txn_roots_wal_changed adds the roots of a row change's put: its out-of-line addresses and its stored values' roots. */
static bool
txn_roots_wal_changed(txn_roots_wal_frame_0_t *frame, const uint8 *body, size_t body_len)
{
	const TxnMemtablePut *put;
	bool		is;

	if (!txn_record_Completion_killed(body, body_len, &is, frame->error))
		return false;
	if (is)
		return true;
	if (!txn_record_Spent_is(body, body_len, &is, frame->error))
		return false;
	if (is)
		return true;
	if (!txn_record_RowChange_decode(body, body_len, &frame->change, frame->error))
		return false;
	frame->change_live = true;
	switch (frame->change.op.tag)
	{
		case TXN_MEMTABLE_OP_PUT:
			put = &frame->change.op.u.put;
			break;
		case TXN_MEMTABLE_OP_ADD:
			put = &frame->change.op.u.add;
			break;
		case TXN_MEMTABLE_OP_ADD_ROW:
			put = &frame->change.op.u.add_row;
			break;
		case TXN_MEMTABLE_OP_UPDATE_ROW:
			put = &frame->change.op.u.update_row.f1;
			break;
		default:
			put = NULL;
			break;
	}
	if (frame->change.storage.database == frame->wal->database && put != NULL)
	{
		if (!txn_roots_wal_named(&frame->wal->roots, put->value.ptr, put->value.len, &put->addresses, frame->error))
			return false;
		for (size_t k = 0; k < frame->change.blobs.len; k++)
			txn_roots_wal_root(&frame->wal->roots, STD_VEC_AT(&frame->change.blobs, chunks_TupleAddressU64_t, k).f0,
							   chunks_trailer_Locator_Default_default());
	}
	txn_roots_wal_decoded_drop(frame);
	return true;
}

/* txn_roots_wal_chunk adds the chunks a logged chunk names, by their addresses alone. */
static bool
txn_roots_wal_chunk(txn_roots_wal_frame_0_t *frame, const uint8 *body, size_t body_len)
{
	if (!txn_record_ChunkWrite_decode(body, body_len, &frame->chunk, frame->error))
		return false;
	frame->chunk_live = true;
	if (frame->chunk.repository == frame->wal->database &&
		!txn_roots_wal_children(&frame->wal->roots, frame->chunk.bytes.ptr, frame->chunk.bytes.len, frame->error))
		return false;
	txn_roots_wal_decoded_drop(frame);
	return true;
}

/* txn_roots_wal_carried adds the blob root of each value map version a rewrite carried. */
static bool
txn_roots_wal_carried(txn_roots_wal_frame_0_t *frame, const uint8 *body, size_t body_len)
{
	if (!txn_record_MapCarried_decode(body, body_len, &frame->carried, frame->error))
		return false;
	frame->carried_live = true;
	if (frame->carried.storage.database == frame->wal->database &&
		frame->carried.map == TXN_MEMTABLE_MAP_MAP_VALUES)
		for (size_t k = 0; k < frame->carried.versions.len; k++)
		{
			const TxnRecordCarriedTake *take = &STD_VEC_AT(&frame->carried.versions, TxnRecordCarriedTake, k);
			TxnValuesEntry entry;

			if (!take->has_value)
				continue;
			if (!txn_values_Entry_decode(take->value.ptr, take->value.len, &entry, frame->error))
				return false;
			txn_roots_wal_root(&frame->wal->roots, entry.address, entry.locator);
		}
	txn_roots_wal_decoded_drop(frame);
	return true;
}

/*
 * txn_roots_wal_unread names the record in a record's failed read's error when
 * it is data corruption, so a capture refuses what it can't parse rather than
 * rooting it, and leaves any other error as it is (bugs row 418).
 */
static void
txn_roots_wal_unread(txn_roots_wal_frame_0_t *frame)
{
	StdStr		inner;
	char		head[96];
	int			len;
	StdString	message;

	if (memcmp(host_Error_sqlstate(frame->error), "XX001", 5) != 0)
		return;
	inner = host_Error_message(frame->error);
	len = snprintf(head, sizeof(head), "the WAL record ending at %X/%08X names chunks a collection can't read: ",
				   (uint32) (frame->record.f1 >> 32), (uint32) frame->record.f1);
	std_string_from_Checked(&message, head, (size_t) len);
	std_vec_extend_from_slice_Checked(&message, inner.ptr, inner.len, 1, 1);
	host_Error_drop(frame->error);
	*frame->error = host_Error_fork((const uint8 *) "XX001", &message);
}

/* txn_roots_wal_body_0 is wal's body: each record past the WAL's position, until none is left. */
static void
txn_roots_wal_body_0(void *frame_)
{
	txn_roots_wal_frame_0_t *frame = frame_;
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(frame->io);
	bool		received = host_wal_stand(doors).recovering;
	uint32		kinds = (1u << host_WalKind_value(HOST_WAL_KIND_ROW_CHANGE)) |
		(1u << host_WalKind_value(HOST_WAL_KIND_CHUNK)) |
		(1u << host_WalKind_value(HOST_WAL_KIND_BASE_ADVANCED)) |
		(1u << host_WalKind_value(HOST_WAL_KIND_MAP_CARRIED));

	for (;;)
	{
		uint8		kind;
		bool		read;

		if (!host_next_fork_record(doors, frame->wal->position, kinds, received, &frame->record_live, &frame->record,
								   frame->error))
			return;
		if (!frame->record_live)
			break;
		kind = frame->record.f0;
		if (kind == host_WalKind_value(HOST_WAL_KIND_BASE_ADVANCED))
			read = txn_roots_wal_advanced(frame, frame->record.f2.ptr, frame->record.f2.len);
		else if (kind == host_WalKind_value(HOST_WAL_KIND_ROW_CHANGE))
			read = txn_roots_wal_changed(frame, frame->record.f2.ptr, frame->record.f2.len);
		else if (kind == host_WalKind_value(HOST_WAL_KIND_CHUNK))
			read = txn_roots_wal_chunk(frame, frame->record.f2.ptr, frame->record.f2.len);
		else
			read = txn_roots_wal_carried(frame, frame->record.f2.ptr, frame->record.f2.len);
		if (!read)
		{
			txn_roots_wal_unread(frame);
			return;
		}
		frame->wal->position = frame->record.f1;
		frame->record_live = false;
		std_vec_free_Checked(&frame->record.f2, 1, 1);
	}
	frame->ok = true;
}

/*
 * txn_roots_wal adds to a capture's roots those the fork's WAL records name
 * past a redo point for a database, which recovery after a crash would read,
 * as Doltgrust's capture_roots adds its WalRoots': each base a fold or a build
 * advanced to, each out-of-line address and stored value a row change puts,
 * each chunk a logged chunk names, and each value map entry a rewrite carried.
 * It reads on from where its last capture of the database stopped, from the
 * redo point again when that moved, and in recovery up to the received WAL,
 * which a standby's crash replays (bugs row 406). The fork's other kinds
 * name no chunk a collection could retire: a database change's value is
 * inline, with no address table; a move's copy and finish copy or rename the
 * moving repository's files whole under the database's lock, reading no
 * chunk by name, and a collection passes over a repository that stands only
 * as a committed move's copy; a repository's creation and drop make and
 * remove directories; and a confirm, a page left, tuple ids freed, a storage
 * emptied, a carry unlisted, and a test record carry tuple ids, ends, and
 * counts.
 */
static bool
txn_roots_wal(HostIoIo *io, uint32 database, HostLsn redo, StdVec *roots, HostError *error)
{
	txn_roots_Wal *wal = NULL;
	txn_roots_wal_frame_0_t frame = {.io = io, .error = error};

	if (!txn_roots_wals_made)
	{
		std_vec_new(&txn_roots_wals, sizeof(txn_roots_Wal), _Alignof(txn_roots_Wal));
		txn_roots_wals_made = true;
	}
	for (size_t k = 0; k < txn_roots_wals.len && wal == NULL; k++)
		if (STD_VEC_AT(&txn_roots_wals, txn_roots_Wal, k).database == database)
			wal = &STD_VEC_AT(&txn_roots_wals, txn_roots_Wal, k);
	if (wal == NULL)
	{
		txn_roots_Wal made = {.database = database, .redo = redo, .position = redo};

		std_vec_new(&made.roots, TXN_ROOTS_ROOT);
		std_vec_push_Checked(&txn_roots_wals, &made, sizeof(made), _Alignof(txn_roots_Wal));
		wal = &STD_VEC_AT(&txn_roots_wals, txn_roots_Wal, txn_roots_wals.len - 1);
	}
	if (wal->redo != redo)
	{
		wal->redo = wal->position = redo;
		wal->roots.len = 0;
	}
	frame.wal = wal;
	std_landing(&frame, txn_roots_wal_body_0, txn_roots_wal_pad_0);
	txn_roots_wal_pad_0(&frame);
	if (!frame.ok)
	{
		wal->redo = 0;
		return false;
	}
	for (size_t k = 0; k < wal->roots.len; k++)
		std_vec_push_Checked(roots, &STD_VEC_AT(&wal->roots, chunks_TupleAddressLocator_t, k), TXN_ROOTS_ROOT);
	return true;
}

/* txn_roots_body_0 is roots' body. */
static void
txn_roots_body_0(void *frame_)
{
	txn_roots_frame_0_t *frame = frame_;
	bool		some;

	std_vec_new(&frame->captured.roots, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_vec_new(&frame->captured.outside, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	frame->captured_live = true;
	if (!txn_state_states(frame->io, frame->database, &frame->states, frame->error))
	{
		txn_roots_pad_0(frame);
		return;
	}
	frame->states_live = true;
	while (frame->moved < frame->states.len)
	{
		frame->held = STD_VEC_AT(&frame->states, txn_TupleU64HeldState_t, frame->moved).f1;
		frame->held_live = true;
		frame->moved = std_wrapping_add_usize(frame->moved, 1);
		if (!txn_roots_state(frame))
		{
			txn_roots_pad_0(frame);
			return;
		}
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	frame->states_live = false;
	std_vec_free_Checked(&frame->states, sizeof(txn_TupleU64HeldState_t), _Alignof(txn_TupleU64HeldState_t));
	if (frame->redo != 0 && !txn_roots_wal(frame->io, frame->database, frame->redo, &frame->captured.roots,
										   frame->error))
	{
		txn_roots_pad_0(frame);
		return;
	}
	if (frame->database == 0)
	{
		*frame->error = txn_roots_error("XX000", "a collection's repository has the id 0");
		txn_roots_pad_0(frame);
		return;
	}
	if (!txn_memtable_branch_find(HOST_SHARED_IO_WITHIN(frame->io), frame->database, &some, &frame->held,
								  frame->error))
	{
		txn_roots_pad_0(frame);
		return;
	}
	if (some)
	{
		frame->held_live = true;
		if (!txn_memtable_table_walked(&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS, frame->io,
									   host_shared_Held_record(&frame->held), txn_roots_chunks_each, frame, NULL,
									   frame->error) ||
			!txn_roots_chunk_spills(frame))
		{
			txn_roots_pad_0(frame);
			return;
		}
	}
	*frame->result = frame->captured;
	frame->captured_live = false;
	frame->ok = true;
	txn_roots_pad_0(frame);
}

/*
 * txn_roots_roots returns what a collection of a database's repository marks
 * from in its states and its working state's delta of chunks: each chunk the
 * address table of a state's newest base record names, which every running
 * snapshot reads, whatever field of the record names it; each row version's
 * out-of-line addresses; each value map version's blob root; and each address
 * a chunk the delta of chunks holds names, that chunk being in no pack and
 * among the captured outside; and given a redo point, not zero, what the
 * fork's WAL records name past it (txn_roots_wal; bugs row 406).
 */
bool
txn_roots_roots(HostIoIo *io, uint32 database, HostLsn redo, TxnRootsCaptured *result, HostError *error)
{
	txn_roots_frame_0_t frame = {.io = io, .database = database, .redo = redo, .result = result, .error = error};

	std_landing(&frame, txn_roots_body_0, txn_roots_pad_0);
	return frame.ok;
}
