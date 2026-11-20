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
	for (size_t k = 0; k < frame->versions.len; k++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&STD_VEC_AT(&frame->versions,
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
	txn_roots_step_drop(frame);
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
	for (size_t k = 0; k < frame->versions.len; k++)
	{
		bool		some;
		StdBytes	value;
		TxnValuesEntry entry;

		if (!txn_memtable_map_valued(&STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, k), &some, &value,
									 error))
			return false;
		if (!some)
			continue;
		if (!txn_values_Entry_decode(value.ptr, value.len, &entry, error) ||
			!txn_roots_root(frame, entry.address, entry.locator, error))
			return false;
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

/* txn_roots_chunk_spills walks a working state's chunk spills in key order, each key's newest version's chunk outside the packs. */
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

		if (!txn_spill_next(&TXN_SPILL_BRANCH_DELTAS_HOME_OPS, frame->io, &frame->hold, record,
							TXN_RUNS_KIND_SPILLED_CHUNKS, frame->after_live, frame->after.ptr, frame->after.len,
							TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &some, &frame->spilled, frame->error))
			return false;
		if (!some)
			return true;
		frame->spilled_live = true;
		if (frame->spilled.f0.len != TXN_MEMTABLE_BRANCH_ADDRESS)
		{
			*frame->error = txn_roots_error("XX001", "a chunk spill's key isn't an address");
			return false;
		}
		memcpy(address.f0, frame->spilled.f0.ptr, TXN_MEMTABLE_BRANCH_ADDRESS);
		if (frame->spilled.f1.len > 0)
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

/* txn_roots_state gathers the roots of one state: its newest base record's addresses, its rows' versions', and its value map's. */
static bool
txn_roots_state(txn_roots_frame_0_t *frame)
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
								  NULL, frame->error);
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
 * among the captured outside.
 */
bool
txn_roots_roots(HostIoIo *io, uint32 database, TxnRootsCaptured *result, HostError *error)
{
	txn_roots_frame_0_t frame = {.io = io, .database = database, .result = result, .error = error};

	std_landing(&frame, txn_roots_body_0, txn_roots_pad_0);
	return frame.ok;
}
