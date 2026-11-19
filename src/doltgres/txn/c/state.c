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
 * The C port of txn's state.rs: a storage's state, a record of fixed shared
 * memory for each storage in use, found by its name, made from its base
 * record in its repository's newest manifest, and reclaimed when no process
 * holds it and either it was never filled or the manifest holds its record.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/delta.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/local.h"
#include "doltgres/host/shared.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/time.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/free_space.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "state.rs"

/* TXN_STATE_BASE_BYTES is the most bytes of a storage's base record a state keeps: the record and its address table. */
#define TXN_STATE_BASE_BYTES 512

/* TXN_STATE_ADDRESS is the length of an entry of a kept record's address table: an offset and a locator. */
#define TXN_STATE_ADDRESS ((size_t) (4 + CHUNKS_TRAILER_LOCATOR_LEN))

/* TXN_STATE_CLEAN is the saved word of a state whose record the newest manifest's base tree holds. */
#define TXN_STATE_CLEAN PG_UINT64_MAX

/* The state's long lock, SpillsWriting, its spills' manifest's writers'; StateLock's index is state.h's. */
#define TXN_STATE_SPILLS_WRITING 1

/* The state's atomics, by their INDEX in state.rs. */
#define TXN_STATE_KEY 0
#define TXN_STATE_TABLESPACE 1
#define TXN_STATE_READY 2
#define TXN_STATE_SAVED 3
#define TXN_STATE_TOTAL(n) (4 + (n))
#define TXN_STATE_COUNTER 10
#define TXN_STATE_BASE_GENERATION 11
#define TXN_STATE_UNLOGGED 16
#define TXN_STATE_OWN 17
#define TXN_STATE_DELTA_BYTES 18
#define TXN_STATE_PENDING_SINCE 19
#define TXN_STATE_HINTS 20
#define TXN_STATE_TOTALS_KNOWN (26 + TXN_FREE_SPACE_CATEGORIES)
#define TXN_STATE_TOTALS_MOVED (27 + TXN_FREE_SPACE_CATEGORIES)
#define TXN_STATE_SPILLS_PENDING (21 + TXN_FREE_SPACE_CATEGORIES)
#define TXN_STATE_SPILLS_CHANGED (22 + TXN_FREE_SPACE_CATEGORIES)
#define TXN_STATE_REACHED_AT (23 + TXN_FREE_SPACE_CATEGORIES)
#define TXN_STATE_DEFERRED_TO (24 + TXN_FREE_SPACE_CATEGORIES)
#define TXN_STATE_REACHED_SINCE (25 + TXN_FREE_SPACE_CATEGORIES)
#define TXN_STATE_RECOUNTING (28 + TXN_FREE_SPACE_CATEGORIES)

/* The makers' atomics, Tokens and Generations. */
#define TXN_STATE_TOKENS 0
#define TXN_STATE_GENERATIONS 1

/* txn_state_load loads one of a state's atomics. */
static uint64
txn_state_load(HostSharedRecord record, size_t a)
{
	HostSharedOp op = {.tag = HOST_SHARED_OP_LOAD};

	return host_shared_Record_atomic(record, a, op);
}

/* txn_state_add adds to one of a state's atomics, giving what it held before. */
static uint64
txn_state_add(HostSharedRecord record, size_t a, uint64 add)
{
	HostSharedOp op = {.tag = HOST_SHARED_OP_ADD, .u.add = add};

	return host_shared_Record_atomic(record, a, op);
}

/* txn_state_exchange exchanges one of a state's atomics from a value to another, giving what it held. */
static uint64
txn_state_exchange(HostSharedRecord record, size_t a, uint64 from, uint64 to)
{
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE, .u.exchange = {from, to}};

	return host_shared_Record_atomic(record, a, op);
}

/* txn_state_internal returns an internal error of the storage states. */
static HostError
txn_state_internal(const char *text, size_t text_len)
{
	StdString	message;

	std_string_from_Checked(&message, text, text_len);
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* txn_state_key returns a storage's key: its database and relation number, 32 bits each, which is never zero. */
static uint64
txn_state_key(const HostStorage *storage)
{
	return ((uint64) storage->database << 32) | storage->relation;
}

/* txn_state_named reports whether a state is a storage's: its key and its tablespace are the storage's. */
static bool
txn_state_named(HostSharedRecord record, const HostStorage *storage)
{
	return txn_state_load(record, TXN_STATE_KEY) == txn_state_key(storage) &&
		txn_state_load(record, TXN_STATE_TABLESPACE) == storage->tablespace;
}

/* txn_state_in_repository returns a storage's name in its repository's base tree. */
static RepoStorage
txn_state_in_repository(const HostStorage *storage)
{
	RepoStorage name = {storage->tablespace, storage->relation};

	return name;
}

/* txn_state_marker returns the storage that stands for a database the loader skipped: relation number 0 in tablespace 0. */
HostStorage
txn_state_marker(uint32 database)
{
	HostStorage storage = {.tablespace = 0, .database = database, .relation = 0};

	return storage;
}

/* txn_state_kept_frame_0_t is kept's state across its pad: the record's bytes and addresses, while live. */
typedef struct txn_state_kept_frame_0_t
{
	const TxnBaseBase *base;
	StdVec	   *result;
	StdVec		bytes;
	StdVec		addresses;
	bool		parts_live;
} txn_state_kept_frame_0_t;

/* txn_state_kept_pad_0 is kept's pad: the record's parts, while live. */
static void
txn_state_kept_pad_0(void *frame_)
{
	txn_state_kept_frame_0_t *frame = frame_;

	if (frame->parts_live)
	{
		frame->parts_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
		std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	}
}

/* txn_state_kept_body_0 is kept's body. */
static void
txn_state_kept_body_0(void *frame_)
{
	txn_state_kept_frame_0_t *frame = frame_;
	uint8		length[2];

	txn_base_Base_encode(frame->base, &frame->bytes, &frame->addresses);
	frame->parts_live = true;
	std_vec_with_capacity_Checked(frame->result,
								  std_wrapping_add_usize(std_wrapping_add_usize(2, frame->bytes.len),
														 std_wrapping_mul_usize(frame->addresses.len,
																				TXN_STATE_ADDRESS)),
								  1, 1);
	std_u16_to_le_bytes((uint16) frame->bytes.len, length);
	std_vec_extend_from_slice_Checked(frame->result, length, sizeof(length), 1, 1);
	std_vec_extend_from_slice_Checked(frame->result, frame->bytes.ptr, frame->bytes.len, 1, 1);
	for (size_t k = 0, k_len = frame->addresses.len; k < k_len; k++)
	{
		const chunks_TupleU32Locator_t *address = &STD_VEC_AT(&frame->addresses, chunks_TupleU32Locator_t, k);
		uint8		offset[4];

		std_u32_to_le_bytes(address->f0, offset);
		std_vec_extend_from_slice_Checked(frame->result, offset, sizeof(offset), 1, 1);
		chunks_trailer_Locator_write(&address->f1, frame->result);
	}
	txn_state_kept_pad_0(frame);
}

/*
 * txn_state_kept returns the blob that keeps a base record: the record's
 * length in two bytes, low byte first, the record, and its address table,
 * each entry an offset in four bytes and a locator.
 */
void
txn_state_kept(const TxnBaseBase *base, StdVec *result)
{
	txn_state_kept_frame_0_t frame = {.base = base, .result = result};

	std_landing(&frame, txn_state_kept_body_0, txn_state_kept_pad_0);
}

/* txn_state_unkept_frame_0_t is unkept's state across its pad: the address table read, while live. */
typedef struct txn_state_unkept_frame_0_t
{
	const uint8 *blob;
	size_t		length;
	const uint8 *table;
	size_t		count;
	TxnBaseBase *result;
	HostError  *error;
	bool		ok;
	StdVec		addresses;
	bool		addresses_live;
} txn_state_unkept_frame_0_t;

/* txn_state_unkept_pad_0 is unkept's pad: the address table, while live. */
static void
txn_state_unkept_pad_0(void *frame_)
{
	txn_state_unkept_frame_0_t *frame = frame_;

	if (frame->addresses_live)
	{
		frame->addresses_live = false;
		std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	}
}

/* txn_state_unkept_body_0 is unkept's body past its length checks: the address table read, then the record decoded. */
static void
txn_state_unkept_body_0(void *frame_)
{
	txn_state_unkept_frame_0_t *frame = frame_;

	std_vec_with_capacity_Checked(&frame->addresses, frame->count, sizeof(chunks_TupleU32Locator_t),
								  _Alignof(chunks_TupleU32Locator_t));
	frame->addresses_live = true;
	for (size_t k = 0, k_len = frame->count; k < k_len; k++)
	{
		const uint8 *entry = STD_PTR_AT(frame->table, std_wrapping_mul_usize(k, TXN_STATE_ADDRESS), 1);
		chunks_TupleU32Locator_t address;

		address.f0 = std_u32_from_le_bytes(entry);
		address.f1 = chunks_trailer_Locator_read(STD_PTR_AT(entry, 4, 1), CHUNKS_TRAILER_LOCATOR_LEN);
		std_vec_push_Checked(&frame->addresses, &address, sizeof(address), _Alignof(chunks_TupleU32Locator_t));
	}
	frame->ok = txn_base_Base_decode(STD_PTR_AT(frame->blob, 2, 1), frame->length, frame->addresses.ptr,
									 frame->addresses.len, frame->result, frame->error);
	txn_state_unkept_pad_0(frame);
}

/*
 * txn_state_unkept returns the base record a blob keeps, or nothing for an
 * empty blob, a storage with no base, a blob cut short being the fault its
 * caller names: an internal error for a state's own blob, data corruption
 * for a logged one.
 */
bool
txn_state_unkept(const uint8 *blob, size_t blob_len, HostError (*fault) (const char *text, size_t text_len),
				 bool *some, TxnBaseBase *result, HostError *error)
{
	txn_state_unkept_frame_0_t frame = {.blob = blob, .result = result, .error = error};
	size_t		table_len;

	*some = false;
	if (blob_len < 2)
		return true;
	frame.length = std_u16_from_le_bytes(blob);
	if (frame.length > std_wrapping_sub_usize(blob_len, 2))
	{
		*error = fault("a storage state's base record is cut short",
					   strlen("a storage state's base record is cut short"));
		return false;
	}
	frame.table = STD_PTR_AT(blob, std_wrapping_add_usize(2, frame.length), 1);
	table_len = std_wrapping_sub_usize(std_wrapping_sub_usize(blob_len, 2), frame.length);
	if (STD_REM_FOLDED(usize, table_len, TXN_STATE_ADDRESS) != 0)
	{
		*error = fault("a storage state's address table is cut short",
					   strlen("a storage state's address table is cut short"));
		return false;
	}
	frame.count = STD_DIV_FOLDED(usize, table_len, TXN_STATE_ADDRESS);
	std_landing(&frame, txn_state_unkept_body_0, txn_state_unkept_pad_0);
	*some = frame.ok;
	return frame.ok;
}

/* txn_state_totals returns the tree's totals a filled state holds, read with no lock, as Sizes. */
TxnBaseSizes
txn_state_totals(const HostSharedHeld *held)
{
	HostSharedRecord record = host_shared_Held_record(held);
	TxnBaseSizes sizes;

	sizes.entries = txn_state_load(record, TXN_STATE_TOTAL(0));
	sizes.leaf_bytes = txn_state_load(record, TXN_STATE_TOTAL(1));
	sizes.key_bytes = txn_state_load(record, TXN_STATE_TOTAL(2));
	sizes.heap_bytes = txn_state_load(record, TXN_STATE_TOTAL(3));
	sizes.rows = txn_state_load(record, TXN_STATE_TOTAL(4));
	return sizes;
}

/* txn_state_known reports whether a state's totals count its tree and every committed version of it. */
bool
txn_state_known(const HostSharedHeld *held)
{
	return txn_state_load(host_shared_Held_record(held), TXN_STATE_TOTALS_KNOWN) == 1;
}

/*
 * txn_state_totals_unknown marks a state's totals as no longer counting its
 * tree and its committed versions, for a write whose commit adds nothing to
 * them.
 */
void
txn_state_totals_unknown(HostSharedRecord record)
{
	txn_state_exchange(record, TXN_STATE_TOTALS_KNOWN, 1, 0);
}

/* txn_state_generation loads a state's base generation with no lock, which a take compares under its range's lock. */
uint64
txn_state_generation(HostSharedRecord record)
{
	return txn_state_load(record, TXN_STATE_BASE_GENERATION);
}

/* txn_state_find_frame_0_t is find's state across its pad: the sweep and the handle being looked at, while live. */
typedef struct txn_state_find_frame_0_t
{
	HostSharedWithin within;
	const HostStorage *storage;
	bool	   *some;
	HostSharedHeld *result;
	HostError  *error;
	bool		ok;
	StdVec		swept;
	bool		swept_live;
	HostSharedHeld held;
	bool		held_live;
} txn_state_find_frame_0_t;

/* txn_state_find_pad_0 is find's pad: the handle, then the sweep, while live. */
static void
txn_state_find_pad_0(void *frame_)
{
	txn_state_find_frame_0_t *frame = frame_;

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

/* txn_state_find_body_0 is find's body. */
static void
txn_state_find_body_0(void *frame_)
{
	txn_state_find_frame_0_t *frame = frame_;
	uint64		key = txn_state_key(frame->storage);
	HostStorage marked = txn_state_marker(frame->storage->database);
	uint64		marker = txn_state_key(&marked);

	if (!host_shared_sweep(frame->within, &TXN_STATE_STATE_POOL_OPS, TXN_STATE_KEY, &frame->swept, frame->error))
		return;
	frame->swept_live = true;
	for (size_t k = 0, k_len = frame->swept.len; k < k_len; k++)
	{
		const std_TupleU64U64_t *found = &STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k);
		bool		has;

		if (found->f1 != key && found->f1 != marker)
			continue;
		if (!host_shared_open(frame->within, &TXN_STATE_STATE_POOL_OPS, found->f0, &has, &frame->held,
							  frame->error))
		{
			txn_state_find_pad_0(frame);
			return;
		}
		if (!has)
			continue;
		frame->held_live = true;
		if (txn_state_named(host_shared_Held_record(&frame->held), &marked))
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("database "), STD_FMT_ARG(0),
				STD_FMT_LIT(" is refused until the server restarts, since its checkpoint's runs "
							"weren't loaded at this start")};
			StdFmtArg	args[] = {{&frame->storage->database, std_u32_Display_fmt}};
			StdFmtArguments arguments = {pieces, 3, args};
			StdString	message;

			std_fmt_format_Checked(&message, &arguments);
			*frame->error = host_Error_fork((const uint8 *) "XX001", &message);
			txn_state_find_pad_0(frame);
			return;
		}
		if (txn_state_named(host_shared_Held_record(&frame->held), frame->storage))
		{
			frame->held_live = false;
			*frame->result = frame->held;
			*frame->some = true;
			frame->ok = true;
			txn_state_find_pad_0(frame);
			return;
		}
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	frame->ok = true;
	txn_state_find_pad_0(frame);
}

/*
 * txn_state_find returns a handle on a storage's published state, or
 * nothing when none is: it sweeps the pool for the storage's key, opens each
 * location whose key matches, and compares the key and the tablespace again
 * under the handle, so a slot freed and made again for another storage
 * meanwhile is passed over. It refuses, as data corruption (XX001), a
 * storage of a database whose marker the same sweep meets.
 */
static bool
txn_state_find(HostSharedWithin within, const HostStorage *storage, bool *some, HostSharedHeld *result,
			   HostError *error)
{
	txn_state_find_frame_0_t frame = {.within = within, .storage = storage, .some = some, .result = result,
	.error = error};

	*some = false;
	std_landing(&frame, txn_state_find_body_0, txn_state_find_pad_0);
	return frame.ok;
}

/*
 * txn_state_made makes an unpublished state of a storage, with its key and
 * its tablespace and ready clear, with no lock held, since a full pool's
 * reclaim takes the makers' lock.
 */
static bool
txn_state_made(HostIoIo *io, const HostStorage *storage, HostSharedOwned *result, HostError *error)
{
	HostSharedRecord record;

	if (!host_shared_make(&TXN_STATE_STATE_POOL_OPS, io, result, error))
		return false;
	record = host_shared_Owned_record(result);
	txn_state_exchange(record, TXN_STATE_KEY, 0, txn_state_key(storage));
	txn_state_exchange(record, TXN_STATE_TABLESPACE, 0, storage->tablespace);
	return true;
}

/* txn_state_opened_frame_0_t is opened's state across its pad: the state it made and the makers' guard, while live. */
typedef struct txn_state_opened_frame_0_t
{
	HostIoIo   *io;
	const HostStorage *storage;
	HostSharedHeld *result;
	HostError  *error;
	bool		ok;
	HostSharedOwned owned;
	bool		owned_live;
	HostSharedGuard making;
	bool		making_live;
} txn_state_opened_frame_0_t;

/* txn_state_opened_pad_0 is opened's pad: the makers' guard, then the state it made, while live. */
static void
txn_state_opened_pad_0(void *frame_)
{
	txn_state_opened_frame_0_t *frame = frame_;

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

/* txn_state_opened_body_0 is opened's body. */
static void
txn_state_opened_body_0(void *frame_)
{
	txn_state_opened_frame_0_t *frame = frame_;
	HostSharedRecord makers;
	uint64		location;
	bool		some;

	if (!txn_state_find(HOST_SHARED_IO_WITHIN(frame->io), frame->storage, &some, frame->result, frame->error))
		return;
	if (some)
	{
		frame->ok = true;
		return;
	}
	if (!txn_state_made(frame->io, frame->storage, &frame->owned, frame->error))
		return;
	frame->owned_live = true;
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-state-made", frame->error) ||
		!host_shared_single(&TXN_STATE_MAKERS_SINGLE_OPS, &makers, frame->error) ||
		!host_shared_Record_lock(makers, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_MAKING_INDEX, &frame->making,
								 frame->error))
	{
		txn_state_opened_pad_0(frame);
		return;
	}
	frame->making_live = true;
	if (!txn_state_find(HOST_SHARED_GUARD_WITHIN(&frame->making), frame->storage, &some, frame->result,
						frame->error))
	{
		txn_state_opened_pad_0(frame);
		return;
	}
	if (some)
	{
		frame->ok = true;
		txn_state_opened_pad_0(frame);
		return;
	}
	frame->owned_live = false;
	location = host_shared_Owned_publish(&frame->owned);
	if (!host_shared_open(HOST_SHARED_GUARD_WITHIN(&frame->making), &TXN_STATE_STATE_POOL_OPS, location, &some,
						  frame->result, frame->error))
	{
		txn_state_opened_pad_0(frame);
		return;
	}
	if (!some)
		*frame->error = txn_state_internal("a storage state just published isn't there",
										   strlen("a storage state just published isn't there"));
	frame->ok = some;
	txn_state_opened_pad_0(frame);
}

/*
 * txn_state_opened returns a handle on a storage's state, making one when
 * none is published: in chunks' order, it makes a state with no lock held,
 * takes the makers' lock, and finds again, dropping its own on a hit, which
 * frees it unpublished; otherwise it publishes its own and opens it under
 * the same lock, so no reclaim takes it first.
 */
static bool
txn_state_opened(HostIoIo *io, const HostStorage *storage, HostSharedHeld *result, HostError *error)
{
	txn_state_opened_frame_0_t frame = {.io = io, .storage = storage, .result = result, .error = error};

	std_landing(&frame, txn_state_opened_body_0, txn_state_opened_pad_0);
	return frame.ok;
}

/* txn_state_State_Pool_count returns a state for each relation lock Postgres can grant at once. */
static size_t
txn_state_State_Pool_count(const HostSharedLimits *limits)
{
	return limits->relation_locks;
}

/* txn_state_mapped_word reports whether a word of a storage's delta's maps names one, or its spills' manifest lists a file. */
static bool
txn_state_mapped_word(HostSharedRecord record)
{
	return txn_state_mapped(record) || txn_state_load(record, TXN_STATE_SPILLS_INDEX) != 0;
}

/* txn_state_reclaim_frame_0_t is reclaim's state across its pad: the makers' guard, the sweep, the kept locations, the look, and the handle, while live. */
typedef struct txn_state_reclaim_frame_0_t
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
} txn_state_reclaim_frame_0_t;

/* txn_state_reclaim_pad_0 is reclaim's pad: the handle, the look, the sweep, the makers' guard, and the kept locations, while live. */
static void
txn_state_reclaim_pad_0(void *frame_)
{
	txn_state_reclaim_frame_0_t *frame = frame_;

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

/*
 * txn_state_reclaim_looked looks at one location of reclaim's sweep under
 * the makers' lock: it answers false on an error, and sets freed when it
 * freed the state.
 */
static bool
txn_state_reclaim_looked(txn_state_reclaim_frame_0_t *frame, uint64 location, bool *undecided, bool *freed)
{
	HostSharedRecord record;
	bool		clean;
	bool		memtable;
	bool		maps;

	if (!host_shared_reclaim(HOST_SHARED_GUARD_WITHIN(&frame->making), &TXN_STATE_STATE_POOL_OPS, location,
							 &frame->look, frame->error))
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
	record = host_shared_Reclaim_record(&frame->look.u.mine);
	if ((uint32) txn_state_load(record, TXN_STATE_KEY) == 0)
	{
		frame->look_live = false;
		host_shared_Look_drop(&frame->look);
		return true;
	}
	clean = txn_state_load(record, TXN_STATE_READY) == 0 || txn_state_load(record, TXN_STATE_SAVED) == TXN_STATE_CLEAN;
	memtable = txn_state_load(record, TXN_STATE_MEMTABLE_WORD_INDEX) != 0;
	maps = txn_state_mapped_word(record);
	if (!host_pause(HOST_SHARED_GUARD_PAUSES(&frame->making), "doltgres-state-reclaim-looked", frame->error))
		return false;
	if (clean && maps)
		txn_state_exchange(record, TXN_STATE_FOLD_DUE_INDEX, 0, 1);
	if (clean && !memtable && !maps)
	{
		frame->look_live = false;
		*freed = host_shared_Reclaim_free(&frame->look.u.mine);
		return true;
	}
	if (clean && memtable && !maps)
	{
		if (!std_vec_try_reserve(&frame->kept, 1, sizeof(uint64), _Alignof(uint64)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		std_vec_push_Checked(&frame->kept, &location, sizeof(location), _Alignof(uint64));
	}
	frame->look_live = false;
	host_shared_Look_drop(&frame->look);
	return true;
}

/* txn_state_reclaim_body_0 is reclaim's body. */
static void
txn_state_reclaim_body_0(void *frame_)
{
	txn_state_reclaim_frame_0_t *frame = frame_;
	HostSharedRecord makers;
	bool		undecided = false;

	std_vec_new(&frame->kept, sizeof(uint64), _Alignof(uint64));
	frame->kept_live = true;
	if (!host_shared_single(&TXN_STATE_MAKERS_SINGLE_OPS, &makers, frame->error) ||
		!host_shared_Record_lock(makers, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_MAKING_INDEX, &frame->making,
								 frame->error))
	{
		txn_state_reclaim_pad_0(frame);
		return;
	}
	frame->making_live = true;
	if (!host_shared_sweep(HOST_SHARED_GUARD_WITHIN(&frame->making), &TXN_STATE_STATE_POOL_OPS, TXN_STATE_KEY,
						   &frame->swept, frame->error))
	{
		txn_state_reclaim_pad_0(frame);
		return;
	}
	frame->swept_live = true;
	for (size_t k = 0, k_len = frame->swept.len; k < k_len; k++)
	{
		bool		freed = false;

		if (!txn_state_reclaim_looked(frame, STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k).f0, &undecided,
									  &freed))
		{
			txn_state_reclaim_pad_0(frame);
			return;
		}
		if (freed)
		{
			*frame->result = true;
			frame->ok = true;
			txn_state_reclaim_pad_0(frame);
			return;
		}
	}
	frame->swept_live = false;
	std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->making_live = false;
	host_shared_Guard_drop(&frame->making);
	for (size_t k = 0, k_len = frame->kept.len; k < k_len; k++)
	{
		bool		some;
		uint64		memtable;
		bool		abandoned = false;

		if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), &TXN_STATE_STATE_POOL_OPS,
							  STD_VEC_AT(&frame->kept, uint64, k), &some, &frame->held, frame->error))
		{
			txn_state_reclaim_pad_0(frame);
			return;
		}
		if (!some)
			continue;
		frame->held_live = true;
		if (txn_memtable_table_memtable(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, host_shared_Held_record(&frame->held),
										&memtable) &&
			!txn_memtable_table_abandoned(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io,
										  host_shared_Held_record(&frame->held), memtable, &abandoned, frame->error))
		{
			txn_state_reclaim_pad_0(frame);
			return;
		}
		if (abandoned)
		{
			*frame->result = true;
			frame->ok = true;
			txn_state_reclaim_pad_0(frame);
			return;
		}
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	*frame->result = undecided;
	frame->ok = true;
	txn_state_reclaim_pad_0(frame);
}

/*
 * txn_state_State_Pool_reclaim frees, under the makers' lock, a state that
 * is not in use and was never filled or whose record the newest manifest
 * holds, and answers whether the make should look again: when it freed one
 * or met one another reclaim was looking at. A state kept only by a memtable
 * that holds nothing and no pin names has it closed once the makers' lock is
 * released, and the make looks again.
 */
static bool
txn_state_State_Pool_reclaim(HostIoIo *io, bool *result, HostError *error)
{
	txn_state_reclaim_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, txn_state_reclaim_body_0, txn_state_reclaim_pad_0);
	return frame.ok;
}

/* TXN_STATE_BLOBS is State's blobs: its newest base record and the one a replayed fold's record named, BASE_BYTES each. */
static const size_t TXN_STATE_BLOBS[] = {TXN_STATE_BASE_BYTES, TXN_STATE_BASE_BYTES};

/* TXN_STATE_STATE_POOL_OPS is State's Shape and Pool: two locks, its atomics, and two base records' blobs. */
const HostSharedPoolOps TXN_STATE_STATE_POOL_OPS = {
	.shape = {.id = UINT64CONST(0x74786e000002), .locks = 2, .atomics = 29 + TXN_FREE_SPACE_CATEGORIES,
	.blobs = TXN_STATE_BLOBS, .blobs_len = lengthof(TXN_STATE_BLOBS)},
	.name = {"storage state", 13},
	.count = txn_state_State_Pool_count,
	.reclaim = txn_state_State_Pool_reclaim,
};

/* TXN_STATE_MAKERS_SINGLE_OPS is Makers' Shape and Single: the makers' lock and two atomics, the saves' tokens and the spills' generations. */
const HostSharedSingleOps TXN_STATE_MAKERS_SINGLE_OPS = {
	.shape = {.id = UINT64CONST(0x74786e000003), .locks = 1, .atomics = 2},
	.words = host_shared_Single_words,
};

/*
 * txn_state_unsaved marks a state not clean, so the next save gathers it; a
 * gathering's token in saved turns to not clean too, so its "saved" leaves
 * the state for the save after.
 */
static void
txn_state_unsaved(HostSharedRecord record)
{
	for (;;)
	{
		uint64		saved = txn_state_load(record, TXN_STATE_SAVED);

		if (saved == 0 || txn_state_exchange(record, TXN_STATE_SAVED, saved, 0) == saved)
			break;
	}
}

/* txn_state_keyed_frame_0_t is keyed's state across its pad: the state's guard, its blob, the base, the new blob, and the key it takes, while live. */
typedef struct txn_state_keyed_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord record;
	TxnStateMade *made;
	HostError  *error;
	bool		ok;
	bool		made_live;
	HostSharedGuard guard;
	bool		guard_live;
	StdVec		blob;
	bool		blob_live;
	TxnBaseBase base;
	bool		base_live;
	StdVec		bytes;
	bool		bytes_live;
} txn_state_keyed_frame_0_t;

/* txn_state_keyed_pad_0 is keyed's pad: the new blob, the base, the blob, the guard, and the key it takes, while live. */
static void
txn_state_keyed_pad_0(void *frame_)
{
	txn_state_keyed_frame_0_t *frame = frame_;

	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	if (frame->base_live)
	{
		frame->base_live = false;
		txn_base_Base_drop(&frame->base);
	}
	if (frame->blob_live)
	{
		frame->blob_live = false;
		std_vec_free_Checked(&frame->blob, 1, 1);
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	if (frame->made_live)
	{
		frame->made_live = false;
		txn_base_Key_drop(&frame->made->key);
	}
}

/* txn_state_keyed_body_0 is keyed's body. */
static void
txn_state_keyed_body_0(void *frame_)
{
	txn_state_keyed_frame_0_t *frame = frame_;
	bool		some;

	if (!host_shared_Record_lock(frame->record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_STATE_LOCK_INDEX,
								 &frame->guard, frame->error))
	{
		txn_state_keyed_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	frame->blob = host_shared_Guard_read_blob(&frame->guard, 0);
	frame->blob_live = true;
	if (!txn_state_unkept(frame->blob.ptr, frame->blob.len, txn_state_internal, &some, &frame->base, frame->error))
	{
		txn_state_keyed_pad_0(frame);
		return;
	}
	frame->blob_live = false;
	std_vec_free_Checked(&frame->blob, 1, 1);
	if (!some)
	{
		frame->made_live = false;
		frame->base = txn_base_Base_empty(&frame->made->key, frame->made->table, frame->made->has_index,
										  frame->made->index);
	}
	else if (frame->base.key.tag == TXN_BASE_KEY_UNKNOWN)
	{
		frame->made_live = false;
		frame->base.key = frame->made->key;
	}
	else
	{
		frame->base_live = true;
		frame->ok = true;
		txn_state_keyed_pad_0(frame);
		return;
	}
	frame->base_live = true;
	txn_state_kept(&frame->base, &frame->bytes);
	frame->bytes_live = true;
	if (!host_shared_GuardExclusive_write_blob(&frame->guard, 0, frame->bytes.ptr, frame->bytes.len, frame->error))
	{
		txn_state_keyed_pad_0(frame);
		return;
	}
	if (some)
		txn_state_unsaved(frame->record);
	frame->ok = true;
	txn_state_keyed_pad_0(frame);
}

/*
 * txn_state_keyed records a backend's key and table in a filled state redo
 * made, taking them: an empty base record holding them for a storage with no
 * saved base, which leaves the state saved, since only a write makes it
 * unsaved, and the key in a base record whose key is unknown, which marks
 * the state unsaved, so the next save writes it. Both are read again under
 * StateLock; a state whose key is known is left alone.
 */
static bool
txn_state_keyed(HostIoIo *io, HostSharedRecord record, TxnStateMade *made, HostError *error)
{
	txn_state_keyed_frame_0_t frame = {.io = io, .record = record, .made = made, .error = error, .made_live = true};

	std_landing(&frame, txn_state_keyed_body_0, txn_state_keyed_pad_0);
	return frame.ok;
}

/* txn_state_state_frame_0_t is state's state across its pad: the handle, the read record, the base, the filling guard, the blob, and what the state is made with, while live. */
typedef struct txn_state_state_frame_0_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostStorage *storage;
	TxnStatePersistence persistence;
	TxnStateMade *made;
	HostSharedHeld *result;
	HostError  *error;
	bool		ok;
	bool		made_live;
	HostSharedHeld held;
	bool		held_live;
	RepoBaseRecord read;
	bool		read_live;
	TxnBaseBase base;
	bool		base_live;
	HostSharedGuard filling;
	bool		filling_live;
	StdVec		blob;
	bool		blob_live;
} txn_state_state_frame_0_t;

/* txn_state_state_pad_0 is state's pad: the blob, the filling guard, the base, the read record, the handle, and what the state is made with, while live. */
static void
txn_state_state_pad_0(void *frame_)
{
	txn_state_state_frame_0_t *frame = frame_;

	if (frame->blob_live)
	{
		frame->blob_live = false;
		std_vec_free_Checked(&frame->blob, 1, 1);
	}
	if (frame->filling_live)
	{
		frame->filling_live = false;
		host_shared_Guard_drop(&frame->filling);
	}
	if (frame->base_live)
	{
		frame->base_live = false;
		txn_base_Base_drop(&frame->base);
	}
	if (frame->read_live)
	{
		frame->read_live = false;
		repo_BaseRecord_drop(&frame->read);
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->made_live)
	{
		frame->made_live = false;
		txn_base_Key_drop(&frame->made->key);
	}
}

/* txn_state_state_filled fills a state under its lock when ready is still clear, answering false on an error. */
static bool
txn_state_state_filled(txn_state_state_frame_0_t *frame, HostSharedRecord record, uint64 counter, TxnBaseSizes sizes)
{
	if (!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_STATE_LOCK_INDEX, &frame->filling,
								 frame->error))
		return false;
	frame->filling_live = true;
	if (txn_state_load(record, TXN_STATE_READY) == 0)
	{
		if (!frame->base_live && frame->made_live)
		{
			frame->made_live = false;
			frame->base = txn_base_Base_empty(&frame->made->key, frame->made->table, frame->made->has_index,
											  frame->made->index);
			frame->base_live = true;
		}
		if (frame->base_live)
			txn_state_kept(&frame->base, &frame->blob);
		else
			std_vec_new(&frame->blob, 1, 1);
		frame->blob_live = true;
		if (!host_shared_GuardExclusive_write_blob(&frame->filling, 0, frame->blob.ptr, frame->blob.len,
												   frame->error))
			return false;
		frame->blob_live = false;
		std_vec_free_Checked(&frame->blob, 1, 1);
		txn_state_add(record, TXN_STATE_TOTAL(0), sizes.entries);
		txn_state_add(record, TXN_STATE_TOTAL(1), sizes.leaf_bytes);
		txn_state_add(record, TXN_STATE_TOTAL(2), sizes.key_bytes);
		txn_state_add(record, TXN_STATE_TOTAL(3), sizes.heap_bytes);
		txn_state_add(record, TXN_STATE_TOTAL(4), sizes.rows);
		txn_state_exchange(record, TXN_STATE_TOTALS_KNOWN, 0, 1);
		txn_state_exchange(record, TXN_STATE_COUNTER, 0, counter);
		txn_state_exchange(record, TXN_STATE_UNLOGGED, 0, (uint64) frame->persistence);
		txn_state_exchange(record, TXN_STATE_OWN, 0, (uint64) repo_Repo_own(frame->repo));
		txn_state_exchange(record, TXN_STATE_SAVED, 0, TXN_STATE_CLEAN);
		txn_state_exchange(record, TXN_STATE_READY, 0, 1);
	}
	if (frame->base_live)
	{
		frame->base_live = false;
		txn_base_Base_drop(&frame->base);
	}
	frame->filling_live = false;
	host_shared_Guard_drop(&frame->filling);
	return true;
}

/* txn_state_state_body_0 is state's body. */
static void
txn_state_state_body_0(void *frame_)
{
	txn_state_state_frame_0_t *frame = frame_;
	HostSharedRecord record;
	RepoStorage name;
	bool		has_read;
	ChunksErrorError failure;
	uint64		counter = 1;
	TxnBaseSizes sizes = txn_base_Sizes_Default_default();

	if (!txn_state_opened(frame->io, frame->storage, &frame->held, frame->error))
	{
		txn_state_state_pad_0(frame);
		return;
	}
	frame->held_live = true;
	record = host_shared_Held_record(&frame->held);
	if (txn_state_load(record, TXN_STATE_READY) == 0)
	{
		name = txn_state_in_repository(frame->storage);
		if (!repo_Repo_base(frame->repo, frame->io, frame->reading, &name, &has_read, &frame->read, &failure))
		{
			*frame->error = txn_base_storage(&failure);
			txn_state_state_pad_0(frame);
			return;
		}
		if (has_read)
		{
			frame->read_live = true;
			if (!txn_base_Base_decode(frame->read.bytes.ptr, frame->read.bytes.len, frame->read.addresses.ptr,
									  frame->read.addresses.len, &frame->base, frame->error))
			{
				txn_state_state_pad_0(frame);
				return;
			}
			frame->base_live = true;
			frame->read_live = false;
			repo_BaseRecord_drop(&frame->read);
			counter = frame->base.counter;
			sizes = frame->base.sizes;
		}
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-state-read", frame->error) ||
			!txn_state_state_filled(frame, record, counter, sizes))
		{
			txn_state_state_pad_0(frame);
			return;
		}
	}
	if (frame->made_live)
	{
		frame->made_live = false;
		if (!txn_state_keyed(frame->io, record, frame->made, frame->error))
		{
			txn_state_state_pad_0(frame);
			return;
		}
	}
	frame->held_live = false;
	*frame->result = frame->held;
	frame->ok = true;
}

/*
 * txn_state_state returns a handle on a storage's state, filled, taking what
 * it is made with. A finder that loads ready clear reads the storage's base
 * from the repository's newest manifest with no lock held, since that read
 * takes the pack record's locks; then, under the state's lock, it fills the
 * state only if ready is still clear, storing the record, then its tree's
 * totals from the base's sizes, or zero for no base, then its tuple id
 * counter, (0,1) for no base, then its persistence and its repository's own
 * tablespace, then saved as clean, since the manifest holds the record, then
 * ready, and otherwise drops what it read. No process clears ready while any
 * holds the state, so the record read after the publish is still the newest
 * when it is stored. A backend's make gives the key and table keyed records;
 * with no saved base, its fill stores an empty base record holding them.
 */
bool
txn_state_state(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostStorage *storage,
				TxnStatePersistence persistence, bool has_made, TxnStateMade *made, HostSharedHeld *result,
				HostError *error)
{
	txn_state_state_frame_0_t frame = {.io = io, .repo = repo, .reading = reading, .storage = storage,
	.persistence = persistence, .made = made, .result = result, .error = error, .made_live = has_made};

	std_landing(&frame, txn_state_state_body_0, txn_state_state_pad_0);
	return frame.ok;
}

/* txn_state_base_record_frame_0_t is base_record's state across its pad: the state's shared guard and its blob, while live. */
typedef struct txn_state_base_record_frame_0_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	bool	   *some;
	TxnBaseBase *result;
	HostError  *error;
	bool		ok;
	HostSharedGuard guard;
	bool		guard_live;
	StdVec		blob;
	bool		blob_live;
} txn_state_base_record_frame_0_t;

/* txn_state_base_record_pad_0 is base_record's pad: the blob, then the guard, while live. */
static void
txn_state_base_record_pad_0(void *frame_)
{
	txn_state_base_record_frame_0_t *frame = frame_;

	if (frame->blob_live)
	{
		frame->blob_live = false;
		std_vec_free_Checked(&frame->blob, 1, 1);
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
}

/* txn_state_base_record_body_0 is base_record's body. */
static void
txn_state_base_record_body_0(void *frame_)
{
	txn_state_base_record_frame_0_t *frame = frame_;

	if (!host_shared_Record_lock_shared(host_shared_Held_record(frame->held), HOST_IO_IO_TAKES_SHORT(frame->io),
										TXN_STATE_STATE_LOCK_INDEX, &frame->guard, frame->error))
		return;
	frame->guard_live = true;
	frame->blob = host_shared_Guard_read_blob(&frame->guard, 0);
	frame->blob_live = true;
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	frame->ok = txn_state_unkept(frame->blob.ptr, frame->blob.len, txn_state_internal, frame->some, frame->result,
								 frame->error);
	txn_state_base_record_pad_0(frame);
}

/*
 * txn_state_base_record returns the base record a filled state holds, read
 * under the state's lock, or nothing for a storage with no base.
 */
bool
txn_state_base_record(HostIoIo *io, const HostSharedHeld *held, bool *some, TxnBaseBase *result, HostError *error)
{
	txn_state_base_record_frame_0_t frame = {.io = io, .held = held, .some = some, .result = result, .error = error};

	std_landing(&frame, txn_state_base_record_body_0, txn_state_base_record_pad_0);
	return frame.ok;
}

/*
 * txn_state_newest returns a filled state's newest base record, read under
 * the state's lock: base_record's read, since the blob read is a copy.
 */
bool
txn_state_newest(HostIoIo *io, const HostSharedHeld *held, bool *some, TxnBaseBase *result, HostError *error)
{
	return txn_state_base_record(io, held, some, result, error);
}

/*
 * txn_state_mapped reports whether a word of a storage's delta's maps names
 * one: its tuple id table, its free space map, or its value map.
 */
bool
txn_state_mapped(HostSharedRecord record)
{
	return txn_state_load(record, TXN_STATE_TUPLES_WORD_INDEX) != 0 ||
		txn_state_load(record, TXN_STATE_FREE_WORD_INDEX) != 0 || txn_state_load(record,
																				 TXN_STATE_VALUES_WORD_INDEX) != 0;
}

/* txn_state_micros returns the time as microseconds since the epoch, at least one, so it never reads as zero. */
static uint64
txn_state_micros(void)
{
	StdSystemTime now = host_now();
	StdDuration since;
	StdDuration before;
	uint64		micros;

	if (!std_SystemTime_duration_since(&now, STD_UNIX_EPOCH, &since, &before))
		since = std_Duration_Default_default();
	micros = std_saturating_add_u64(std_saturating_mul_u64(since.secs, 1000000), STD_DIV_FOLDED(u32, since.nanos,
																								1000));
	return micros > 1 ? micros : 1;
}

/* txn_state_total_stored stores one of a state's totals. */
static void
txn_state_total_stored(HostSharedRecord record, size_t n, uint64 size)
{
	for (;;)
	{
		uint64		was = txn_state_load(record, TXN_STATE_TOTAL(n));

		if (txn_state_exchange(record, TXN_STATE_TOTAL(n), was, size) == was)
			return;
	}
}

/* txn_state_place_cell_default is PlaceCell's Default for a BackendLocal of one. */
static void
txn_state_place_cell_default(void *result, const StdLocation *caller)
{
	host_shared_PlaceCell_Default_default(result);
}

/* TXN_STATE_RECOUNT_TYPE is PlaceCell<State>'s size, Default, and drop, which does nothing. */
static const StdDefaultType TXN_STATE_RECOUNT_TYPE = {sizeof(HostSharedPlaceCell), _Alignof(HostSharedPlaceCell),
txn_state_place_cell_default, NULL};

/*
 * TXN_STATE_RECOUNT is RECOUNT: the place of the state whose totals the
 * backend counts again, while it holds the state's Recounting; its
 * BackendLocal header and the process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	HostSharedPlaceCell process;
}			TXN_STATE_RECOUNT = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * txn_state_RecountEnded_Exit_run gives back the Recounting of a state whose
 * count the backend's exit cut short, which the hold the count's frame kept
 * still holds open.
 */
static bool
txn_state_RecountEnded_Exit_run(HostIoIo *io, HostError *error)
{
	HostLocalLocal recount;

	if (host_local_BackendLocal_existing(&TXN_STATE_RECOUNT.local, &recount))
	{
		HostSharedPlaceCell *cell = (HostSharedPlaceCell *) host_local_Local_Deref_deref(&recount,
																						 &TXN_STATE_RECOUNT_TYPE);
		HostSharedHeldPlaceOption place = host_shared_PlaceCell_get(cell);

		if (place.has)
		{
			host_shared_PlaceCell_set(cell, false, place.place);
			host_shared_HeldPlace_exchange(place.place, TXN_STATE_RECOUNTING, 1, 0);
		}
		host_local_Local_drop(&recount, &TXN_STATE_RECOUNT_TYPE);
	}
	return true;
}

/* TXN_STATE_RECOUNT_ENDED_TYPE_ID is RecountEnded's identity, by which its exit work is registered once. */
static const StdTypeId TXN_STATE_RECOUNT_ENDED_TYPE_ID = {"txn::state::RecountEnded", 0, 1, NULL};

/* TXN_STATE_RECOUNT_ENDED_EXIT_OPS is RecountEnded's Exit. */
static const HostExitOps TXN_STATE_RECOUNT_ENDED_EXIT_OPS = {txn_state_RecountEnded_Exit_run};

/* txn_state_recounted_frame_0_t is recounted's state across its pad: the count's closure, the base, and the backend's RECOUNT, while live. */
typedef struct txn_state_recounted_frame_0_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	bool		(*count) (void *captures, HostIoIo *io, bool *some, TxnBaseSizes *result, HostError *error);
	void	   *count_captures;
	void		(*count_drop) (void *captures);
	TxnBaseSizes *result;
	HostError  *error;
	bool		ok;
	bool		count_live;
	HostLocalLocal recount;
	bool		recount_live;
	TxnBaseBase base;
	bool		base_live;
} txn_state_recounted_frame_0_t;

/* txn_state_recounted_pad_0 is recounted's pad: the base, RECOUNT's local, and the count's closure, while live. */
static void
txn_state_recounted_pad_0(void *frame_)
{
	txn_state_recounted_frame_0_t *frame = frame_;

	if (frame->base_live)
	{
		frame->base_live = false;
		txn_base_Base_drop(&frame->base);
	}
	if (frame->recount_live)
	{
		frame->recount_live = false;
		host_local_Local_drop(&frame->recount, &TXN_STATE_RECOUNT_TYPE);
	}
	if (frame->count_live)
	{
		frame->count_live = false;
		if (frame->count_drop != NULL)
			frame->count_drop(frame->count_captures);
	}
}

/*
 * txn_state_recounted_counting runs recounted's count while the backend
 * holds the state's Recounting, answering false on the count's error, which
 * it gives back only after clearing RECOUNT and Recounting, and setting
 * counted when the count answered sizes.
 */
static bool
txn_state_recounted_counting(txn_state_recounted_frame_0_t *frame, HostSharedRecord record,
							 HostSharedPlaceCell *cell, bool *counted)
{
	uint64		moved;
	bool		ok;
	TxnBaseSizes sizes;

	host_shared_PlaceCell_set(cell, true, host_shared_Held_place(frame->held));
	moved = txn_state_load(record, TXN_STATE_TOTALS_MOVED);
	ok = host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-state-recounting", frame->error);
	if (ok)
	{
		frame->count_live = false;
		ok = frame->count(frame->count_captures, frame->io, counted, &sizes, frame->error);
	}
	else
	{
		frame->count_live = false;
		if (frame->count_drop != NULL)
			frame->count_drop(frame->count_captures);
	}
	if (ok && *counted)
	{
		txn_state_total_stored(record, 0, sizes.entries);
		txn_state_total_stored(record, 1, sizes.leaf_bytes);
		txn_state_total_stored(record, 2, sizes.key_bytes);
		txn_state_total_stored(record, 3, sizes.heap_bytes);
		txn_state_total_stored(record, 4, sizes.rows);
		txn_state_exchange(record, TXN_STATE_TOTALS_KNOWN, 0, 1);
		if (txn_state_load(record, TXN_STATE_TOTALS_MOVED) != moved)
			txn_state_totals_unknown(record);
	}
	host_shared_PlaceCell_set(cell, false, host_shared_Held_place(frame->held));
	txn_state_exchange(record, TXN_STATE_RECOUNTING, 1, 0);
	return ok;
}

/* txn_state_recounted_body_0 is recounted's body. */
static void
txn_state_recounted_body_0(void *frame_)
{
	txn_state_recounted_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	bool		some;

	if (txn_state_known(frame->held))
	{
		*frame->result = txn_state_totals(frame->held);
		frame->ok = true;
		txn_state_recounted_pad_0(frame);
		return;
	}
	if (!host_at_exit(&TXN_STATE_RECOUNT_ENDED_TYPE_ID, &TXN_STATE_RECOUNT_ENDED_EXIT_OPS, frame->error))
	{
		txn_state_recounted_pad_0(frame);
		return;
	}
	frame->recount = host_local_BackendLocal_get(&TXN_STATE_RECOUNT.local, &TXN_STATE_RECOUNT_TYPE);
	frame->recount_live = true;
	if (txn_state_exchange(record, TXN_STATE_RECOUNTING, 0, 1) == 0)
	{
		HostSharedPlaceCell *cell = (HostSharedPlaceCell *) host_local_Local_Deref_deref(&frame->recount,
																						 &TXN_STATE_RECOUNT_TYPE);
		bool		counted = false;

		if (!txn_state_recounted_counting(frame, record, cell, &counted))
		{
			txn_state_recounted_pad_0(frame);
			return;
		}
		if (counted && txn_state_known(frame->held))
		{
			*frame->result = txn_state_totals(frame->held);
			frame->ok = true;
			txn_state_recounted_pad_0(frame);
			return;
		}
	}
	if (frame->count_live)
	{
		frame->count_live = false;
		if (frame->count_drop != NULL)
			frame->count_drop(frame->count_captures);
	}
	if (!txn_state_base_record(frame->io, frame->held, &some, &frame->base, frame->error))
	{
		txn_state_recounted_pad_0(frame);
		return;
	}
	frame->base_live = some;
	*frame->result = some ? frame->base.sizes : txn_base_Sizes_Default_default();
	frame->ok = true;
	txn_state_recounted_pad_0(frame);
}

/*
 * txn_state_recounted returns a state's totals, counted again first when
 * they are unknown: one backend at a time runs the count, which answers none
 * when it met a version whose writer hasn't ended, and installs what it
 * answers unless a commit moved the totals while it counted, so no commit's
 * move is lost. A state that isn't counted gives its newest base's sizes,
 * which hold every folded version. The counting backend keeps the state's
 * place in RECOUNT, so that its exit by any path, a FATAL in the count
 * included, lets the next reader count.
 */
bool
txn_state_recounted(HostIoIo *io, const HostSharedHeld *held,
					bool (*count) (void *captures, HostIoIo *io, bool *some, TxnBaseSizes *result, HostError *error),
					void *count_captures, void (*count_drop) (void *captures), TxnBaseSizes *result,
					HostError *error)
{
	txn_state_recounted_frame_0_t frame = {.io = io, .held = held, .count = count, .count_captures = count_captures,
	.count_drop = count_drop, .result = result, .error = error, .count_live = true};

	std_landing(&frame, txn_state_recounted_body_0, txn_state_recounted_pad_0);
	return frame.ok;
}

/*
 * TXN_STATE_MOVES is how many storages' moves of their totals the backend's
 * commit adds in its critical section; a commit that moves more marks the
 * rest's totals unknown at its pre-commit, for a reader to count again.
 */
#define TXN_STATE_MOVES 64

/*
 * TxnStateMove is a storage's move of its totals at the backend's commit:
 * the place of its state, which the transaction holds until it ends, and the
 * sizes the commit adds and those it takes away, in Sizes' order.
 */
typedef struct TxnStateMove
{
	HostSharedPlaceCell place;
	StdAtomicU64 adds[5];
	StdAtomicU64 takes[5];
} TxnStateMove;

/*
 * TxnStateMoves is the backend's committing transaction's moves of its
 * totals, the first count of moves, read with no lock, so the commit's
 * critical section reads each one it made.
 */
typedef struct TxnStateMoves
{
	StdAtomicU64 count;
	TxnStateMove moves[TXN_STATE_MOVES];
} TxnStateMoves;

/* txn_state_Moves_Default_default makes a backend's moves, none made. */
static void
txn_state_Moves_Default_default(void *result_, const StdLocation *caller)
{
	TxnStateMoves *result = result_;

	std_AtomicU64_new(&result->count, 0);
	for (size_t k = 0; k < TXN_STATE_MOVES; k++)
	{
		host_shared_PlaceCell_Default_default(&result->moves[k].place);
		for (size_t i = 0; i < 5; i++)
		{
			std_AtomicU64_new(&result->moves[k].adds[i], 0);
			std_AtomicU64_new(&result->moves[k].takes[i], 0);
		}
	}
}

/* TXN_STATE_COMMITTING_TYPE is Moves' size, Default, and drop, which does nothing. */
static const StdDefaultType TXN_STATE_COMMITTING_TYPE = {sizeof(TxnStateMoves), _Alignof(TxnStateMoves),
txn_state_Moves_Default_default, NULL};

/*
 * TXN_STATE_COMMITTING is COMMITTING: the moves of the backend's committing
 * transaction's totals, made before the commit's critical section, added in
 * it, and cleared at each transaction's pre-commit, end, abort, and prepare;
 * its BackendLocal header and the process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	TxnStateMoves process;
}			TXN_STATE_COMMITTING = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* txn_state_committing_cleared clears the backend's moves of its totals, so none outlives the transaction that made it. */
void
txn_state_committing_cleared(void)
{
	HostLocalLocal local;

	if (host_local_BackendLocal_existing(&TXN_STATE_COMMITTING.local, &local))
	{
		TxnStateMoves *moves = (TxnStateMoves *) host_local_Local_Deref_deref(&local, &TXN_STATE_COMMITTING_TYPE);

		std_AtomicU64_store_relaxed(&moves->count, 0);
		host_local_Local_drop(&local, &TXN_STATE_COMMITTING_TYPE);
	}
}

/* txn_state_committing_frame_0_t is committing's state across its pad: COMMITTING's local, while live. */
typedef struct txn_state_committing_frame_0_t
{
	const HostSharedHeld *held;
	TxnBaseSizes adds;
	TxnBaseSizes takes;
	HostLocalLocal local;
	bool		local_live;
} txn_state_committing_frame_0_t;

/* txn_state_committing_pad_0 is committing's pad: COMMITTING's local, while live. */
static void
txn_state_committing_pad_0(void *frame_)
{
	txn_state_committing_frame_0_t *frame = frame_;

	if (frame->local_live)
	{
		frame->local_live = false;
		host_local_Local_drop(&frame->local, &TXN_STATE_COMMITTING_TYPE);
	}
}

/* txn_state_committing_body_0 is committing's body. */
static void
txn_state_committing_body_0(void *frame_)
{
	txn_state_committing_frame_0_t *frame = frame_;
	TxnStateMoves *local;
	uint64		count;
	TxnStateMove *moving;
	uint64		adds[5] = {frame->adds.entries, frame->adds.leaf_bytes, frame->adds.key_bytes, frame->adds.heap_bytes,
	frame->adds.rows};
	uint64		takes[5] = {frame->takes.entries, frame->takes.leaf_bytes, frame->takes.key_bytes,
	frame->takes.heap_bytes, frame->takes.rows};

	frame->local = host_local_BackendLocal_get(&TXN_STATE_COMMITTING.local, &TXN_STATE_COMMITTING_TYPE);
	frame->local_live = true;
	local = (TxnStateMoves *) host_local_Local_Deref_deref(&frame->local, &TXN_STATE_COMMITTING_TYPE);
	count = std_AtomicU64_load_relaxed(&local->count);
	if (count >= TXN_STATE_MOVES)
	{
		txn_state_totals_unknown(host_shared_Held_record(frame->held));
		txn_state_committing_pad_0(frame);
		return;
	}
	moving = &local->moves[count];
	host_shared_PlaceCell_set(&moving->place, true, host_shared_Held_place(frame->held));
	for (size_t i = 0; i < 5; i++)
		std_AtomicU64_store_relaxed(&moving->adds[i], adds[i]);
	for (size_t i = 0; i < 5; i++)
		std_AtomicU64_store_relaxed(&moving->takes[i], takes[i]);
	std_AtomicU64_store_relaxed(&local->count, STD_ADD(u64, count, 1, 309, 29));
	txn_state_committing_pad_0(frame);
}

/*
 * txn_state_committing adds a held state's move of its totals at the
 * backend's commit: what the commit adds and what it takes away, or marks
 * the state's totals unknown once the commit holds MOVES moves.
 */
void
txn_state_committing(const HostSharedHeld *held, TxnBaseSizes adds, TxnBaseSizes takes)
{
	txn_state_committing_frame_0_t frame = {.held = held, .adds = adds, .takes = takes};

	std_landing(&frame, txn_state_committing_body_0, txn_state_committing_pad_0);
}

/*
 * txn_state_committed adds the backend's moves to its states' totals, inside
 * the commit's critical section: it takes no lock, allocates nothing, and
 * returns nothing, and counts each move first in its state's TotalsMoved,
 * which a recount reads after its install.
 */
void
txn_state_committed(void)
{
	HostLocalLocal local;
	TxnStateMoves *moves;
	size_t		count;

	if (!host_local_BackendLocal_existing(&TXN_STATE_COMMITTING.local, &local))
		return;
	moves = (TxnStateMoves *) host_local_Local_Deref_deref(&local, &TXN_STATE_COMMITTING_TYPE);
	count = (size_t) std_AtomicU64_load_relaxed(&moves->count);
	for (size_t k = 0, k_len = std_min_usize(count, TXN_STATE_MOVES); k < k_len; k++)
	{
		TxnStateMove *moving = &moves->moves[k];
		HostSharedHeldPlaceOption place = host_shared_PlaceCell_get(&moving->place);

		if (!place.has)
			continue;
		host_shared_HeldPlace_add(place.place, TXN_STATE_TOTALS_MOVED, 1);
		for (size_t i = 0; i < 5; i++)
			host_shared_HeldPlace_add(place.place, TXN_STATE_TOTAL(i),
									  std_wrapping_sub_u64(std_AtomicU64_load_relaxed(&moving->adds[i]),
														   std_AtomicU64_load_relaxed(&moving->takes[i])));
	}
	host_local_Local_drop(&local, &TXN_STATE_COMMITTING_TYPE);
}

/* txn_state_grew counts a version a finish pushed into a state's delta: its bytes, and the delta's age when it held none. */
void
txn_state_grew(HostSharedRecord record, uint64 bytes)
{
	txn_state_add(record, TXN_STATE_DELTA_BYTES, bytes);
	if (txn_state_load(record, TXN_STATE_PENDING_SINCE) == 0)
		txn_state_exchange(record, TXN_STATE_PENDING_SINCE, 0, txn_state_micros());
}

/* txn_state_shrank counts the bytes of the versions a removal took out of a state's delta. */
void
txn_state_shrank(HostSharedRecord record, uint64 bytes)
{
	txn_state_add(record, TXN_STATE_DELTA_BYTES, std_wrapping_sub_u64(0, bytes));
}

/*
 * txn_state_folded_all reports whether a state's storage holds no unfolded
 * version: none in its delta, none in its spills' manifest, and no spill
 * pending, whose count a spill adds to before its versions leave the delta.
 */
static bool
txn_state_folded_all(HostSharedRecord record)
{
	return txn_state_load(record, TXN_STATE_DELTA_BYTES) == 0 &&
		txn_state_load(record, TXN_STATE_SPILLS_PENDING) == 0 && txn_state_load(record, TXN_STATE_SPILLS_INDEX) == 0;
}

/*
 * txn_state_aged restarts a state's delta's age after a fold's drop: now
 * when the delta still holds a version, else zero. It stores now first and
 * clears it only while the delta reads empty, then sets it again when a push
 * came between, so that the word isn't zero while the delta holds a version,
 * whatever the order of a concurrent push's two steps. A spill's versions
 * are unfolded too, so a storage whose spills' manifest lists a file, or
 * whose spill is pending, keeps its age through the spill until a fold takes
 * them out.
 */
bool
txn_state_aged(HostIoIo *io, HostSharedRecord record, HostError *error)
{
	uint64		now = txn_state_micros();
	uint64		seen = txn_state_load(record, TXN_STATE_PENDING_SINCE);

	while (txn_state_exchange(record, TXN_STATE_PENDING_SINCE, seen, now) != seen)
		seen = txn_state_load(record, TXN_STATE_PENDING_SINCE);
	if (txn_state_folded_all(record))
	{
		if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-state-aged-emptied", error))
			return false;
		txn_state_exchange(record, TXN_STATE_PENDING_SINCE, now, 0);
		if (!txn_state_folded_all(record))
			txn_state_exchange(record, TXN_STATE_PENDING_SINCE, 0, txn_state_micros());
	}
	return true;
}

/* txn_state_spilled returns the bytes of the files a state's spills' manifest lists. */
static bool
txn_state_spilled(HostIoIo *io, HostSharedRecord record, uint64 *result, HostError *error)
{
	HostStorage storage;
	TxnStatePersistence persistence;
	txn_TupleVecFileU64_t listed;
	uint64		sum = 0;

	*result = 0;
	if (txn_state_load(record, TXN_STATE_SPILLS_INDEX) == 0 || !txn_state_described(record, &storage, &persistence))
		return true;
	if (!txn_spill_listed(&TXN_SPILL_STATE_HOME_OPS, io, record, &storage, &listed, error))
		return false;
	for (size_t k = 0, k_len = listed.f0.len; k < k_len; k++)
		sum = std_add_u64(sum, STD_VEC_AT(&listed.f0, TxnSpillFile, k).len, STD_ACCUM_FILE, 206, 1);
	std_vec_free_Checked(&listed.f0, sizeof(TxnSpillFile), _Alignof(TxnSpillFile));
	*result = sum;
	return true;
}

/*
 * txn_state_pending returns a state's delta's bytes and the time in
 * microseconds since which it has held a version no drop took, zero for
 * none.
 */
std_TupleU64U64_t
txn_state_pending(HostSharedRecord record)
{
	std_TupleU64U64_t pending = {txn_state_load(record, TXN_STATE_DELTA_BYTES),
	txn_state_load(record, TXN_STATE_PENDING_SINCE)};

	return pending;
}

/* txn_state_stored writes a word only its holders of the state's lock exclusively write, as its caller is. */
static void
txn_state_stored(HostSharedRecord record, size_t a, uint64 value)
{
	txn_state_exchange(record, a, txn_state_load(record, a), value);
}

/* txn_state_deferred moves a state's DeferredTo up to a record's end, under the state's lock its caller holds exclusively. */
static void
txn_state_deferred(HostSharedRecord record, uint64 end)
{
	if (end > txn_state_load(record, TXN_STATE_DEFERRED_TO))
		txn_state_stored(record, TXN_STATE_DEFERRED_TO, end);
}

/*
 * txn_state_slot_frame_0_t is the state of the reads of a state's slot
 * (reach, deferring, past, and behind) across their pad: the state's guard,
 * a blob, and a base read from it, while live.
 */
typedef struct txn_state_slot_frame_0_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	HostError  *error;
	bool		ok;
	HostSharedGuard guard;
	bool		guard_live;
	StdVec		blob;
	bool		blob_live;
	TxnBaseBase base;
	bool		base_live;
	StdVec		bytes;
	bool		bytes_live;
} txn_state_slot_frame_0_t;

/* txn_state_slot_pad_0 is the slot reads' pad: the new blob, the base, the blob, then the guard, while live. */
static void
txn_state_slot_pad_0(void *frame_)
{
	txn_state_slot_frame_0_t *frame = frame_;

	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	if (frame->base_live)
	{
		frame->base_live = false;
		txn_base_Base_drop(&frame->base);
	}
	if (frame->blob_live)
	{
		frame->blob_live = false;
		std_vec_free_Checked(&frame->blob, 1, 1);
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
}

/*
 * txn_state_slot_locked takes a slot read's state's lock, exclusively or
 * shared, answering false on an error.
 */
static bool
txn_state_slot_locked(txn_state_slot_frame_0_t *frame, bool exclusive)
{
	HostSharedRecord record = host_shared_Held_record(frame->held);
	bool		locked = exclusive ?
		host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_STATE_LOCK_INDEX, &frame->guard,
								frame->error) :
		host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_STATE_LOCK_INDEX,
									   &frame->guard, frame->error);

	frame->guard_live = locked;
	return locked;
}

/*
 * txn_state_slot_unkept reads one of a slot read's state's blobs under its
 * guard as a base record, which the frame holds when there is one, answering
 * false on an error.
 */
static bool
txn_state_slot_unkept(txn_state_slot_frame_0_t *frame, size_t i, bool *some)
{
	bool		ok;

	frame->blob = host_shared_Guard_read_blob(&frame->guard, i);
	frame->blob_live = true;
	ok = txn_state_unkept(frame->blob.ptr, frame->blob.len, txn_state_internal, some, &frame->base, frame->error);
	frame->blob_live = false;
	std_vec_free_Checked(&frame->blob, 1, 1);
	frame->base_live = ok && *some;
	return ok;
}

/*
 * txn_state_slot_folded reads one of a slot read's state's blobs under its
 * guard, giving its base's fold point, zero for none, answering false on an
 * error.
 */
static bool
txn_state_slot_folded(txn_state_slot_frame_0_t *frame, size_t i, bool *some, uint64 *folded)
{
	if (!txn_state_slot_unkept(frame, i, some))
		return false;
	*folded = *some ? frame->base.folded : 0;
	if (frame->base_live)
	{
		frame->base_live = false;
		txn_base_Base_drop(&frame->base);
	}
	return true;
}

/* txn_state_reach_frame_0_t is reach's frame: the slot reads' frame, the base to keep, its record's end, and its answer. */
typedef struct txn_state_reach_frame_0_t
{
	txn_state_slot_frame_0_t slot;
	const TxnBaseBase *base;
	uint64		end;
	bool	   *result;
} txn_state_reach_frame_0_t;

/* txn_state_reach_pad_0 is reach's pad: the slot reads'. */
static void
txn_state_reach_pad_0(void *frame_)
{
	txn_state_reach_frame_0_t *frame = frame_;

	txn_state_slot_pad_0(&frame->slot);
}

/* txn_state_reach_body_0 is reach's body. */
static void
txn_state_reach_body_0(void *frame_)
{
	txn_state_reach_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->slot.held);
	bool		some;
	uint64		folded;
	uint64		reached;
	bool		open;

	if (!txn_state_slot_locked(&frame->slot, true) || !txn_state_slot_folded(&frame->slot, 0, &some, &folded) ||
		!txn_state_slot_folded(&frame->slot, 1, &some, &reached))
	{
		txn_state_reach_pad_0(frame);
		return;
	}
	open = !some || reached <= folded;
	frame->slot.ok = true;
	if (frame->base->folded <= folded)
		*frame->result = true;
	else if (!open || txn_state_load(record, TXN_STATE_DEFERRED_TO) != 0)
	{
		txn_state_deferred(record, frame->end);
		*frame->result = false;
	}
	else
	{
		txn_state_kept(frame->base, &frame->slot.bytes);
		frame->slot.bytes_live = true;
		frame->slot.ok = host_shared_GuardExclusive_write_blob(&frame->slot.guard, 1, frame->slot.bytes.ptr,
															   frame->slot.bytes.len, frame->slot.error);
		if (frame->slot.ok)
		{
			if (txn_state_load(record, TXN_STATE_REACHED_AT) == 0)
				txn_state_stored(record, TXN_STATE_REACHED_SINCE, txn_state_micros());
			txn_state_stored(record, TXN_STATE_REACHED_AT, frame->end);
			*frame->result = true;
		}
	}
	txn_state_reach_pad_0(frame);
}

/*
 * txn_state_reach keeps the base a replayed fold's record named, whose packs
 * this server didn't hold, so its own fold reaches that point, and the
 * record's end, and answers whether it did: a standby folds every point its
 * primary folded, in order, since the tuple id, free space, and pending
 * trees depend on which points were folded, so the slot keeps a record only
 * while it holds no point its fold hasn't reached and no earlier record
 * waits for the read past it; any other record is left to that read.
 */
bool
txn_state_reach(HostIoIo *io, const HostSharedHeld *held, const TxnBaseBase *base, uint64 end, bool *result,
				HostError *error)
{
	txn_state_reach_frame_0_t frame = {.slot = {.io = io, .held = held, .error = error}, .base = base, .end = end,
	.result = result};

	std_landing(&frame, txn_state_reach_body_0, txn_state_reach_pad_0);
	return frame.slot.ok;
}

/* txn_state_deferring_frame_0_t is deferring's frame: the slot reads' frame, the free's end, and its answer. */
typedef struct txn_state_deferring_frame_0_t
{
	txn_state_slot_frame_0_t slot;
	uint64		end;
	bool	   *result;
} txn_state_deferring_frame_0_t;

/* txn_state_deferring_pad_0 is deferring's pad: the slot reads'. */
static void
txn_state_deferring_pad_0(void *frame_)
{
	txn_state_deferring_frame_0_t *frame = frame_;

	txn_state_slot_pad_0(&frame->slot);
}

/* txn_state_deferring_body_0 is deferring's body. */
static void
txn_state_deferring_body_0(void *frame_)
{
	txn_state_deferring_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->slot.held);
	bool		some;
	uint64		folded;
	uint64		reached;

	if (!txn_state_slot_locked(&frame->slot, true) || !txn_state_slot_folded(&frame->slot, 0, &some, &folded) ||
		!txn_state_slot_folded(&frame->slot, 1, &some, &reached))
	{
		txn_state_deferring_pad_0(frame);
		return;
	}
	*frame->result = (some && reached > folded) || txn_state_load(record, TXN_STATE_DEFERRED_TO) != 0;
	if (*frame->result)
		txn_state_deferred(record, frame->end);
	frame->slot.ok = true;
	txn_state_deferring_pad_0(frame);
}

/*
 * txn_state_deferring says whether a replayed free's take-outs wait for the
 * read past the slot's record, because the slot holds a point its fold
 * hasn't reached or an earlier record waits for that read, and moves
 * DeferredTo up to the free's end when they do: the fold at the slot's point
 * reads the versions as its primary's fold did.
 */
bool
txn_state_deferring(HostIoIo *io, const HostSharedHeld *held, uint64 end, bool *result, HostError *error)
{
	txn_state_deferring_frame_0_t frame = {.slot = {.io = io, .held = held, .error = error}, .end = end,
	.result = result};

	std_landing(&frame, txn_state_deferring_body_0, txn_state_deferring_pad_0);
	return frame.slot.ok;
}

/* txn_state_past_frame_0_t is past's frame: the slot reads' frame and its answer. */
typedef struct txn_state_past_frame_0_t
{
	txn_state_slot_frame_0_t slot;
	bool	   *some;
	uint64	   *result;
} txn_state_past_frame_0_t;

/* txn_state_past_pad_0 is past's pad: the slot reads'. */
static void
txn_state_past_pad_0(void *frame_)
{
	txn_state_past_frame_0_t *frame = frame_;

	txn_state_slot_pad_0(&frame->slot);
}

/* txn_state_past_body_0 is past's body. */
static void
txn_state_past_body_0(void *frame_)
{
	txn_state_past_frame_0_t *frame = frame_;
	bool		some;
	uint64		folded;
	uint64		reached;

	*frame->some = false;
	if (!txn_state_slot_locked(&frame->slot, false) || !txn_state_slot_folded(&frame->slot, 0, &some, &folded) ||
		!txn_state_slot_folded(&frame->slot, 1, &some, &reached))
	{
		txn_state_past_pad_0(frame);
		return;
	}
	frame->slot.ok = true;
	if (!(some && reached > folded))
	{
		uint64		from = txn_state_load(host_shared_Held_record(frame->slot.held), TXN_STATE_REACHED_AT);

		*frame->some = from != 0;
		*frame->result = from;
	}
	txn_state_past_pad_0(frame);
}

/*
 * txn_state_past returns the end of a state's slot's record, which the read
 * past it starts from, once the fold at the slot's point has installed and
 * while that read isn't done, or nothing otherwise.
 */
bool
txn_state_past(HostIoIo *io, const HostSharedHeld *held, bool *some, uint64 *result, HostError *error)
{
	txn_state_past_frame_0_t frame = {.slot = {.io = io, .held = held, .error = error}, .some = some,
	.result = result};

	std_landing(&frame, txn_state_past_body_0, txn_state_past_pad_0);
	return frame.slot.ok;
}

/* txn_state_read_past_frame_0_t is read_past's frame: the slot reads' frame, how far the read got, the next record it found, and its answer. */
typedef struct txn_state_read_past_frame_0_t
{
	txn_state_slot_frame_0_t slot;
	uint64		to;
	bool		has_next;
	const TxnBaseBase *next_base;
	uint64		next_end;
	bool	   *result;
} txn_state_read_past_frame_0_t;

/* txn_state_read_past_pad_0 is read_past's pad: the slot reads'. */
static void
txn_state_read_past_pad_0(void *frame_)
{
	txn_state_read_past_frame_0_t *frame = frame_;

	txn_state_slot_pad_0(&frame->slot);
}

/* txn_state_read_past_body_0 is read_past's body. */
static void
txn_state_read_past_body_0(void *frame_)
{
	txn_state_read_past_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->slot.held);

	if (!txn_state_slot_locked(&frame->slot, true))
	{
		txn_state_read_past_pad_0(frame);
		return;
	}
	*frame->result = false;
	frame->slot.ok = true;
	if (frame->has_next)
	{
		txn_state_kept(frame->next_base, &frame->slot.bytes);
		frame->slot.bytes_live = true;
		frame->slot.ok = host_shared_GuardExclusive_write_blob(&frame->slot.guard, 1, frame->slot.bytes.ptr,
															   frame->slot.bytes.len, frame->slot.error);
		if (frame->slot.ok)
			txn_state_stored(record, TXN_STATE_REACHED_AT, frame->next_end);
	}
	else if (txn_state_load(record, TXN_STATE_DEFERRED_TO) <= frame->to)
	{
		txn_state_stored(record, TXN_STATE_DEFERRED_TO, 0);
		txn_state_stored(record, TXN_STATE_REACHED_AT, 0);
		txn_state_stored(record, TXN_STATE_REACHED_SINCE, 0);
		*frame->result = true;
	}
	txn_state_read_past_pad_0(frame);
}

/*
 * txn_state_read_past records how far the read past a state's slot's record
 * got: the slot takes the next fold's record it found, with that record's
 * end, or, with none, the read is done once no later record was deferred
 * since it began, which clears both words; it answers whether the read is
 * done.
 */
bool
txn_state_read_past(HostIoIo *io, const HostSharedHeld *held, uint64 to, bool has_next,
					const TxnBaseBase *next_base, uint64 next_end, bool *result, HostError *error)
{
	txn_state_read_past_frame_0_t frame = {.slot = {.io = io, .held = held, .error = error}, .to = to,
	.has_next = has_next, .next_base = next_base, .next_end = next_end, .result = result};

	std_landing(&frame, txn_state_read_past_body_0, txn_state_read_past_pad_0);
	return frame.slot.ok;
}

/* txn_state_behind_frame_0_t is behind's frame: the slot reads' frame and its answer. */
typedef struct txn_state_behind_frame_0_t
{
	txn_state_slot_frame_0_t slot;
	bool	   *some;
	TxnBaseBase *result;
} txn_state_behind_frame_0_t;

/* txn_state_behind_pad_0 is behind's pad: the slot reads'. */
static void
txn_state_behind_pad_0(void *frame_)
{
	txn_state_behind_frame_0_t *frame = frame_;

	txn_state_slot_pad_0(&frame->slot);
}

/* txn_state_behind_body_0 is behind's body. */
static void
txn_state_behind_body_0(void *frame_)
{
	txn_state_behind_frame_0_t *frame = frame_;
	bool		some;
	uint64		folded;

	*frame->some = false;
	if (!txn_state_slot_locked(&frame->slot, false) || !txn_state_slot_folded(&frame->slot, 0, &some, &folded) ||
		!txn_state_slot_unkept(&frame->slot, 1, &some))
	{
		txn_state_behind_pad_0(frame);
		return;
	}
	frame->slot.ok = true;
	if (some && frame->slot.base.folded > folded)
	{
		frame->slot.base_live = false;
		*frame->result = frame->slot.base;
		*frame->some = true;
	}
	txn_state_behind_pad_0(frame);
}

/*
 * txn_state_behind returns the base a replayed fold's record named past a
 * filled state's newest, read with the newest under the state's lock, or
 * nothing when no record reached past it; a state redo made with no base is
 * behind any record, since its storage was made after the base backup its
 * server started from.
 */
bool
txn_state_behind(HostIoIo *io, const HostSharedHeld *held, bool *some, TxnBaseBase *result, HostError *error)
{
	txn_state_behind_frame_0_t frame = {.slot = {.io = io, .held = held, .error = error}, .some = some,
	.result = result};

	std_landing(&frame, txn_state_behind_body_0, txn_state_behind_pad_0);
	return frame.slot.ok;
}

/* txn_state_founded_frame_0_t is founded's frame: the slot reads' frame and the table and index of the base it gives. */
typedef struct txn_state_founded_frame_0_t
{
	txn_state_slot_frame_0_t slot;
	uint32		table;
	bool		has_index;
	uint32		index;
} txn_state_founded_frame_0_t;

/* txn_state_founded_pad_0 is founded's pad: the slot reads'. */
static void
txn_state_founded_pad_0(void *frame_)
{
	txn_state_founded_frame_0_t *frame = frame_;

	txn_state_slot_pad_0(&frame->slot);
}

/* txn_state_founded_body_0 is founded's body. */
static void
txn_state_founded_body_0(void *frame_)
{
	txn_state_founded_frame_0_t *frame = frame_;
	bool		some;

	if (!txn_state_slot_locked(&frame->slot, true) || !txn_state_slot_unkept(&frame->slot, 0, &some))
	{
		txn_state_founded_pad_0(frame);
		return;
	}
	frame->slot.ok = true;
	if (!some)
	{
		TxnBaseKey	unknown = {.tag = TXN_BASE_KEY_UNKNOWN};

		frame->slot.base = txn_base_Base_empty(&unknown, frame->table, frame->has_index, frame->index);
		frame->slot.base_live = true;
		txn_state_kept(&frame->slot.base, &frame->slot.bytes);
		frame->slot.bytes_live = true;
		frame->slot.ok = host_shared_GuardExclusive_write_blob(&frame->slot.guard, 0, frame->slot.bytes.ptr,
															   frame->slot.bytes.len, frame->slot.error);
	}
	txn_state_founded_pad_0(frame);
}

/*
 * txn_state_founded gives a filled state redo made with no base the empty
 * base a fold builds on, of the table and index a replayed fold's record
 * named, its key unknown until a backend records it, as a backend's own make
 * gives one.
 */
bool
txn_state_founded(HostIoIo *io, const HostSharedHeld *held, uint32 table, bool has_index, uint32 index,
				  HostError *error)
{
	txn_state_founded_frame_0_t frame = {.slot = {.io = io, .held = held, .error = error}, .table = table,
	.has_index = has_index, .index = index};

	std_landing(&frame, txn_state_founded_body_0, txn_state_founded_pad_0);
	return frame.slot.ok;
}

/* txn_state_hint returns where a search of a state's delta's free space map for a category starts. */
bool
txn_state_hint(HostSharedRecord record, uint8 category, uint32 *result, HostError *error)
{
	HostSharedOp op = {.tag = HOST_SHARED_OP_LOAD};
	uint64		hint;

	if (!host_shared_Record_atomic_in(record, TXN_STATE_HINTS, TXN_FREE_SPACE_CATEGORIES, category, op, &hint, error))
		return false;
	*result = (uint32) hint;
	return true;
}

/*
 * txn_state_lowered lowers the hints of a run of categories to a block at
 * most, once the block's entry rose to them, so every search for those
 * categories starts at or below it.
 */
bool
txn_state_lowered(HostSharedRecord record, uint32 block, uint8 categories_start, uint8 categories_end,
				  HostError *error)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

	for (size_t category = categories_start; categories_start <= categories_end && category <= categories_end;
		 category++)
	{
		for (;;)
		{
			uint64		held;
			uint64		was;
			HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};

			if (!host_shared_Record_atomic_in(record, TXN_STATE_HINTS, TXN_FREE_SPACE_CATEGORIES, category, load,
											  &held, error))
				return false;
			if (held <= block)
				break;
			exchange.u.exchange.f0 = held;
			exchange.u.exchange.f1 = block;
			if (!host_shared_Record_atomic_in(record, TXN_STATE_HINTS, TXN_FREE_SPACE_CATEGORIES, category, exchange,
											  &was, error))
				return false;
			if (was == held)
				break;
		}
	}
	return true;
}

/*
 * txn_state_advanced moves a category's hint from where a search began to
 * past the blocks it passed, unless another process moved it meanwhile, and
 * answers whether it moved it.
 */
bool
txn_state_advanced(HostSharedRecord record, uint8 category, uint32 from, uint32 to, bool *result, HostError *error)
{
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE, .u.exchange = {from, to}};
	uint64		was;

	if (!host_shared_Record_atomic_in(record, TXN_STATE_HINTS, TXN_FREE_SPACE_CATEGORIES, category, op, &was, error))
		return false;
	*result = was == from;
	return true;
}

/*
 * txn_state_described returns what a filled state is of, its storage and
 * the persistence its first maker stored, or nothing for a state not filled
 * yet, whose persistence and own tablespace aren't stored.
 */
bool
txn_state_described(HostSharedRecord record, HostStorage *result_0, TxnStatePersistence *result_1)
{
	uint64		key;

	if (txn_state_load(record, TXN_STATE_READY) == 0)
		return false;
	key = txn_state_load(record, TXN_STATE_KEY);
	result_0->tablespace = (uint32) txn_state_load(record, TXN_STATE_TABLESPACE);
	result_0->database = (uint32) STD_SHR_FOLDED(u64, key, 32);
	result_0->relation = (uint32) key;
	*result_1 = txn_state_load(record, TXN_STATE_UNLOGGED) == 0 ? TXN_STATE_PERSISTENCE_PERMANENT :
		TXN_STATE_PERSISTENCE_UNLOGGED;
	return true;
}

/*
 * txn_state_spills_generation gives a storage's spills' manifest a
 * generation no other change of a manifest was given since the server
 * started, so a reader's copy of a reclaimed state's manifest never passes
 * for a new state's.
 */
bool
txn_state_spills_generation(uint64 *result, HostError *error)
{
	HostSharedRecord makers;

	if (!host_shared_single(&TXN_STATE_MAKERS_SINGLE_OPS, &makers, error))
		return false;
	*result = STD_ADD(u64, txn_state_add(makers, TXN_STATE_GENERATIONS, 1), 1, 864, 62);
	return true;
}

/* txn_state_skipped_frame_0_t is skipped's state across its pad: the marker's state and the makers' guard, while live. */
typedef struct txn_state_skipped_frame_0_t
{
	HostIoIo   *io;
	uint32		database;
	HostError  *error;
	bool		ok;
	HostSharedOwned owned;
	bool		owned_live;
	HostSharedGuard making;
	bool		making_live;
} txn_state_skipped_frame_0_t;

/* txn_state_skipped_pad_0 is skipped's pad: the makers' guard, then the marker's state, while live. */
static void
txn_state_skipped_pad_0(void *frame_)
{
	txn_state_skipped_frame_0_t *frame = frame_;

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

/* txn_state_skipped_body_0 is skipped's body. */
static void
txn_state_skipped_body_0(void *frame_)
{
	txn_state_skipped_frame_0_t *frame = frame_;
	HostStorage marked = txn_state_marker(frame->database);
	HostSharedRecord makers;

	if (!txn_state_made(frame->io, &marked, &frame->owned, frame->error))
		return;
	frame->owned_live = true;
	if (!host_shared_single(&TXN_STATE_MAKERS_SINGLE_OPS, &makers, frame->error) ||
		!host_shared_Record_lock(makers, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_MAKING_INDEX, &frame->making,
								 frame->error))
	{
		txn_state_skipped_pad_0(frame);
		return;
	}
	frame->making_live = true;
	frame->owned_live = false;
	host_shared_Owned_publish(&frame->owned);
	frame->ok = true;
	txn_state_skipped_pad_0(frame);
}

/*
 * txn_state_skipped marks a database whose repository a start's loader
 * skipped: it publishes a state keyed by the database with relation number
 * 0, never filled, which the reclaim doesn't free. Every later find of a
 * storage of that database meets it and is refused, for the server's life
 * and whatever the repository's files become, so no state of the database
 * fills and nothing lists, writes, or removes its unloaded runs. The loader
 * runs before redo, when the server has no state of the database.
 */
bool
txn_state_skipped(HostIoIo *io, uint32 database, HostError *error)
{
	txn_state_skipped_frame_0_t frame = {.io = io, .database = database, .error = error};

	std_landing(&frame, txn_state_skipped_body_0, txn_state_skipped_pad_0);
	return frame.ok;
}

/*
 * txn_state_usable refuses a database whose repository this start's loader
 * skipped, as every find of a storage's state of it does, for a caller that
 * touches a repository's files through no state.
 */
bool
txn_state_usable(HostIoIo *io, uint32 database, HostError *error)
{
	HostStorage marked = txn_state_marker(database);
	bool		some;
	HostSharedHeld held;

	if (!txn_state_find(HOST_SHARED_IO_WITHIN(io), &marked, &some, &held, error))
		return false;
	if (some)
		host_shared_Held_drop(&held);
	return true;
}

/*
 * txn_state_sweep_frame_0_t is the state of a sweep of the pool through
 * the process's handles (marked, held_back, states, due, repositories, and
 * unlogged) across its pad: the sweep, the handle looked at, a base read,
 * and what the sweep finds, while live.
 */
typedef struct txn_state_sweep_frame_0_t
{
	HostIoIo   *io;
	HostError  *error;
	bool		ok;
	StdVec		swept;
	bool		swept_live;
	HostSharedHeld held;
	bool		held_live;
	TxnBaseBase base;
	bool		base_live;
	StdVec		found;
	size_t		found_size;
	size_t		found_align;
	void		(*found_drop) (void *element);
	bool		found_live;
} txn_state_sweep_frame_0_t;

/* txn_state_sweep_pad_0 is a sweep's pad: the base, the handle, the sweep, and what it found, while live. */
static void
txn_state_sweep_pad_0(void *frame_)
{
	txn_state_sweep_frame_0_t *frame = frame_;

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
	if (frame->swept_live)
	{
		frame->swept_live = false;
		std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
	if (frame->found_live)
	{
		frame->found_live = false;
		for (size_t k = 0, k_len = frame->found_drop == NULL ? 0 : frame->found.len; k < k_len; k++)
			frame->found_drop(STD_VEC_AT_SIZE(&frame->found, k, frame->found_size));
		std_vec_free_Checked(&frame->found, frame->found_size, frame->found_align);
	}
}

/* txn_state_swept starts a sweep of the pool for the state's key through the process's handles, answering false on an error. */
static bool
txn_state_swept(txn_state_sweep_frame_0_t *frame)
{
	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(frame->io), &TXN_STATE_STATE_POOL_OPS, TXN_STATE_KEY, &frame->swept,
						   frame->error))
		return false;
	frame->swept_live = true;
	return true;
}

/*
 * txn_state_swept_open opens the state at a sweep's place k when its key is
 * still the one swept, answering false on an error and setting opened when
 * the frame holds it.
 */
static bool
txn_state_swept_open(txn_state_sweep_frame_0_t *frame, size_t k, bool *opened)
{
	const std_TupleU64U64_t *found = &STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k);
	bool		some;

	*opened = false;
	if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), &TXN_STATE_STATE_POOL_OPS, found->f0, &some, &frame->held,
						  frame->error))
		return false;
	if (!some)
		return true;
	if (txn_state_load(host_shared_Held_record(&frame->held), TXN_STATE_KEY) != found->f1)
	{
		host_shared_Held_drop(&frame->held);
		return true;
	}
	frame->held_live = true;
	*opened = true;
	return true;
}

/* txn_state_swept_close drops the handle a sweep opened. */
static void
txn_state_swept_close(txn_state_sweep_frame_0_t *frame)
{
	frame->held_live = false;
	host_shared_Held_drop(&frame->held);
}

/* txn_state_swept_push pushes what a sweep found onto its list, answering false when no room is left. */
static bool
txn_state_swept_push(txn_state_sweep_frame_0_t *frame, const void *element)
{
	if (!std_vec_try_reserve(&frame->found, 1, frame->found_size, frame->found_align))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(&frame->found, element, frame->found_size, frame->found_align);
	return true;
}

/* txn_state_marked_frame_0_t is marked's frame: the sweep's frame, the database, and its answer. */
typedef struct txn_state_marked_frame_0_t
{
	txn_state_sweep_frame_0_t sweep;
	uint32		database;
	bool	   *result;
} txn_state_marked_frame_0_t;

/* txn_state_marked_pad_0 is marked's pad: the sweep's. */
static void
txn_state_marked_pad_0(void *frame_)
{
	txn_state_marked_frame_0_t *frame = frame_;

	txn_state_sweep_pad_0(&frame->sweep);
}

/* txn_state_marked_body_0 is marked's body. */
static void
txn_state_marked_body_0(void *frame_)
{
	txn_state_marked_frame_0_t *frame = frame_;
	HostStorage marked = txn_state_marker(frame->database);
	uint64		key = txn_state_key(&marked);

	*frame->result = false;
	if (!txn_state_swept(&frame->sweep))
		return;
	for (size_t k = 0, k_len = frame->sweep.swept.len; k < k_len; k++)
	{
		bool		opened;

		if (STD_VEC_AT(&frame->sweep.swept, std_TupleU64U64_t, k).f1 != key)
			continue;
		if (!txn_state_swept_open(&frame->sweep, k, &opened))
		{
			txn_state_marked_pad_0(frame);
			return;
		}
		if (opened)
		{
			*frame->result = true;
			break;
		}
	}
	frame->sweep.ok = true;
	txn_state_marked_pad_0(frame);
}

/*
 * txn_state_marked reports whether this start's loader skipped a database's
 * repository, for a caller that passes over its files rather than be
 * refused.
 */
bool
txn_state_marked(HostIoIo *io, uint32 database, bool *result, HostError *error)
{
	txn_state_marked_frame_0_t frame = {.sweep = {.io = io, .error = error}, .database = database, .result = result};

	std_landing(&frame, txn_state_marked_body_0, txn_state_marked_pad_0);
	return frame.sweep.ok;
}

/*
 * txn_state_found returns a handle on a storage's published state, or
 * nothing when none is, making none, for a caller that acts only on a state
 * that is already there, as an abort's take-back does.
 */
bool
txn_state_found(HostIoIo *io, const HostStorage *storage, bool *some, HostSharedHeld *result, HostError *error)
{
	return txn_state_find(HOST_SHARED_IO_WITHIN(io), storage, some, result, error);
}

/*
 * txn_state_install_frame_0_t is the state of rechained and installed across
 * their pad: the state's guard, its newest blob, the base it held, the base
 * kept with its key, and the new blob, while live.
 */
typedef struct txn_state_install_frame_0_t
{
	HostIoTakesShort io;
	const HostSharedHeld *held;
	uint64		read;
	const TxnBaseBase *base;
	bool	   *result;
	HostError  *error;
	bool		ok;
	HostSharedGuard guard;
	bool		guard_live;
	StdVec		blob;
	bool		blob_live;
	TxnBaseBase replaced;
	bool		replaced_live;
	TxnBaseBase keyed;
	bool		keyed_live;
	StdVec		bytes;
	bool		bytes_live;
} txn_state_install_frame_0_t;

/* txn_state_install_pad_0 is the installs' pad: the new blob, the bases, the blob, then the guard, while live. */
static void
txn_state_install_pad_0(void *frame_)
{
	txn_state_install_frame_0_t *frame = frame_;

	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	if (frame->keyed_live)
	{
		frame->keyed_live = false;
		txn_base_Base_drop(&frame->keyed);
	}
	if (frame->replaced_live)
	{
		frame->replaced_live = false;
		txn_base_Base_drop(&frame->replaced);
	}
	if (frame->blob_live)
	{
		frame->blob_live = false;
		std_vec_free_Checked(&frame->blob, 1, 1);
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
}

/*
 * txn_state_install_locked takes an install's state's lock and compares its
 * base generation with the one the caller read, answering false on an error
 * and setting current when the generation is still the one read.
 */
static bool
txn_state_install_locked(txn_state_install_frame_0_t *frame, bool *current)
{
	HostSharedRecord record = host_shared_Held_record(frame->held);

	if (!host_shared_Record_lock(record, frame->io, TXN_STATE_STATE_LOCK_INDEX, &frame->guard, frame->error))
		return false;
	frame->guard_live = true;
	*current = txn_state_load(record, TXN_STATE_BASE_GENERATION) == frame->read;
	*frame->result = false;
	return true;
}

/*
 * txn_state_install_written writes an install's new blob as the state's
 * newest base, moving the generation and marking the state unsaved,
 * answering false on an error.
 */
static bool
txn_state_install_written(txn_state_install_frame_0_t *frame)
{
	HostSharedRecord record = host_shared_Held_record(frame->held);

	if (!host_shared_GuardExclusive_write_blob(&frame->guard, 0, frame->bytes.ptr, frame->bytes.len, frame->error))
		return false;
	txn_state_add(record, TXN_STATE_BASE_GENERATION, 1);
	txn_state_unsaved(record);
	*frame->result = true;
	return true;
}

/* txn_state_rechained_body_0 is rechained's body. */
static void
txn_state_rechained_body_0(void *frame_)
{
	txn_state_install_frame_0_t *frame = frame_;
	bool		current;

	if (!txn_state_install_locked(frame, &current))
	{
		txn_state_install_pad_0(frame);
		return;
	}
	if (current)
	{
		txn_state_kept(frame->base, &frame->bytes);
		frame->bytes_live = true;
		if (!txn_state_install_written(frame))
		{
			txn_state_install_pad_0(frame);
			return;
		}
	}
	frame->ok = true;
	txn_state_install_pad_0(frame);
}

/*
 * txn_state_rechained replaces the state's newest base record with one that
 * differs only in its chain of pending transforms, keeping any predecessor,
 * under the state's lock when the base generation is still the one the
 * caller read, moving the generation and marking the state unsaved; it
 * answers whether it replaced.
 */
bool
txn_state_rechained(HostIoTakesShort io, const HostSharedHeld *held, uint64 read, const TxnBaseBase *base,
					bool *result, HostError *error)
{
	txn_state_install_frame_0_t frame = {.io = io, .held = held, .read = read, .base = base, .result = result,
	.error = error};

	std_landing(&frame, txn_state_rechained_body_0, txn_state_install_pad_0);
	return frame.ok;
}

/* txn_state_installed_body_0 is installed's body. */
static void
txn_state_installed_body_0(void *frame_)
{
	txn_state_install_frame_0_t *frame = frame_;
	bool		current;
	bool		some;

	if (!txn_state_install_locked(frame, &current))
	{
		txn_state_install_pad_0(frame);
		return;
	}
	if (!current)
	{
		frame->ok = true;
		txn_state_install_pad_0(frame);
		return;
	}
	frame->blob = host_shared_Guard_read_blob(&frame->guard, 0);
	frame->blob_live = true;
	if (!txn_state_unkept(frame->blob.ptr, frame->blob.len, txn_state_internal, &some, &frame->replaced,
						  frame->error))
	{
		txn_state_install_pad_0(frame);
		return;
	}
	frame->replaced_live = some;
	if (some && frame->base->key.tag == TXN_BASE_KEY_UNKNOWN && frame->replaced.key.tag != TXN_BASE_KEY_UNKNOWN)
	{
		bool		keyed;

		txn_state_kept(frame->base, &frame->bytes);
		frame->bytes_live = true;
		if (!txn_state_unkept(frame->bytes.ptr, frame->bytes.len, txn_state_internal, &keyed, &frame->keyed,
							  frame->error))
		{
			txn_state_install_pad_0(frame);
			return;
		}
		if (!keyed)
		{
			*frame->error = txn_state_internal("a fold's base record didn't keep",
											   strlen("a fold's base record didn't keep"));
			txn_state_install_pad_0(frame);
			return;
		}
		frame->keyed_live = true;
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
		txn_base_Key_drop(&frame->keyed.key);
		frame->keyed.key = frame->replaced.key;
		frame->replaced.key.tag = TXN_BASE_KEY_UNKNOWN;
		txn_state_kept(&frame->keyed, &frame->bytes);
	}
	else
		txn_state_kept(frame->base, &frame->bytes);
	frame->bytes_live = true;
	frame->ok = txn_state_install_written(frame);
	txn_state_install_pad_0(frame);
}

/*
 * txn_state_installed makes a fold's base the state's newest, the newest it
 * replaces dropped, since every snapshot reads the newest, under the state's
 * lock when the base generation is still the one the fold read before it
 * built, moving the generation and marking the state unsaved so the next
 * save writes the new record; it answers whether it installed. The installed
 * base takes the key of the newest it replaces when the fold's base doesn't
 * know it, since a backend records a state's key in its newest base without
 * moving the generation, after the fold read it.
 */
bool
txn_state_installed(HostIoTakesShort io, const HostSharedHeld *held, uint64 read, const TxnBaseBase *base,
					bool *result, HostError *error)
{
	txn_state_install_frame_0_t frame = {.io = io, .held = held, .read = read, .base = base, .result = result,
	.error = error};

	std_landing(&frame, txn_state_installed_body_0, txn_state_install_pad_0);
	return frame.ok;
}

/*
 * txn_state_grown raises a state a backend made empty to a base built whole
 * for it and just installed: its tuple id counter to the base's, so no
 * insert takes a tuple id the base holds, and its totals by the base's
 * sizes, which no write counted.
 */
void
txn_state_grown(HostSharedRecord record, const TxnBaseBase *base)
{
	for (;;)
	{
		uint64		counter = txn_state_load(record, TXN_STATE_COUNTER);

		if (counter >= base->counter ||
			txn_state_exchange(record, TXN_STATE_COUNTER, counter, base->counter) == counter)
			break;
	}
	txn_state_add(record, TXN_STATE_TOTAL(0), base->sizes.entries);
	txn_state_add(record, TXN_STATE_TOTAL(1), base->sizes.leaf_bytes);
	txn_state_add(record, TXN_STATE_TOTAL(2), base->sizes.key_bytes);
	txn_state_add(record, TXN_STATE_TOTAL(3), base->sizes.heap_bytes);
	txn_state_add(record, TXN_STATE_TOTAL(4), base->sizes.rows);
	txn_state_unsaved(record);
}

/* txn_state_repository_base_frame_0_t is repository_base's state across its pad: the record read, while live. */
typedef struct txn_state_repository_base_frame_0_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostStorage *storage;
	bool	   *some;
	TxnBaseBase *result;
	HostError  *error;
	bool		ok;
	RepoBaseRecord read;
	bool		read_live;
} txn_state_repository_base_frame_0_t;

/* txn_state_repository_base_pad_0 is repository_base's pad: the record read, while live. */
static void
txn_state_repository_base_pad_0(void *frame_)
{
	txn_state_repository_base_frame_0_t *frame = frame_;

	if (frame->read_live)
	{
		frame->read_live = false;
		repo_BaseRecord_drop(&frame->read);
	}
}

/* txn_state_repository_base_body_0 is repository_base's body. */
static void
txn_state_repository_base_body_0(void *frame_)
{
	txn_state_repository_base_frame_0_t *frame = frame_;
	RepoStorage name = txn_state_in_repository(frame->storage);
	ChunksErrorError failure;

	if (!repo_Repo_base(frame->repo, frame->io, frame->reading, &name, frame->some, &frame->read, &failure))
	{
		*frame->error = txn_base_storage(&failure);
		return;
	}
	if (!*frame->some)
	{
		frame->ok = true;
		return;
	}
	frame->read_live = true;
	frame->ok = txn_base_Base_decode(frame->read.bytes.ptr, frame->read.bytes.len, frame->read.addresses.ptr,
									 frame->read.addresses.len, frame->result, frame->error);
	txn_state_repository_base_pad_0(frame);
}

/*
 * txn_state_repository_base returns a storage's newest saved base from its
 * repository, or nothing for a storage no save wrote.
 */
static bool
txn_state_repository_base(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
						  const HostStorage *storage, bool *some, TxnBaseBase *result, HostError *error)
{
	txn_state_repository_base_frame_0_t frame = {.io = io, .repo = repo, .reading = reading, .storage = storage,
	.some = some, .result = result, .error = error};

	std_landing(&frame, txn_state_repository_base_body_0, txn_state_repository_base_pad_0);
	return frame.ok;
}

/*
 * txn_state_stored_base returns a storage's base record without making it a
 * state: its published state's when that state is filled, and otherwise the
 * repository's newest saved base, or nothing for a storage nothing wrote or
 * saved.
 */
bool
txn_state_stored_base(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
					  const HostStorage *storage, bool *some, TxnBaseBase *result, HostError *error)
{
	bool		has;
	HostSharedHeld held;

	if (!txn_state_find(HOST_SHARED_IO_WITHIN(io), storage, &has, &held, error))
		return false;
	if (has)
	{
		bool		ready = txn_state_load(host_shared_Held_record(&held), TXN_STATE_READY) != 0;
		bool		ok = !ready || txn_state_base_record(io, &held, some, result, error);

		host_shared_Held_drop(&held);
		if (ready || !ok)
			return ok;
	}
	return txn_state_repository_base(io, repo, reading, storage, some, result, error);
}

/*
 * txn_state_readable returns the base of a filled state's that a snapshot
 * reads: its newest, which every snapshot may read, since every fold's point
 * is bounded by the storage's horizon, below every MVCC and historic
 * snapshot's xmin on the primary, and a hot standby's replay of a built
 * base cancels its snapshots at or below the base's maxFolded before it
 * installs the base; a snapshot of any other kind reads the latest
 * committed state. An MVCC or historic snapshot whose xmin doesn't pass the
 * newest base's maxFolded is an internal error.
 */
bool
txn_state_readable(HostIoIo *io, const HostSharedHeld *held, const HostSnapshot *snapshot, bool *some,
				   TxnBaseBase *result, HostError *error)
{
	if (!txn_state_newest(io, held, some, result, error))
		return false;
	if (*some &&
		(snapshot->kind == HOST_SNAPSHOT_KIND_MVCC || snapshot->kind == HOST_SNAPSHOT_KIND_HISTORIC_MVCC) &&
		snapshot->xmin.f0 <= result->max_folded)
	{
		txn_base_Base_drop(result);
		*some = false;
		*error = txn_state_internal("a snapshot reads a base folded at or past its xmin",
									strlen("a snapshot reads a base folded at or past its xmin"));
		return false;
	}
	return true;
}

/* txn_state_reset_frame_0_t is reset's state across its pad: the saved base it takes, the handle, the guard, the empty base, and its blob, while live. */
typedef struct txn_state_reset_frame_0_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostStorage *storage;
	TxnBaseBase *saved;
	HostError  *error;
	bool		ok;
	bool		saved_live;
	HostSharedHeld held;
	bool		held_live;
	HostSharedGuard filling;
	bool		filling_live;
	TxnBaseBase empty;
	bool		empty_live;
	StdVec		bytes;
	bool		bytes_live;
} txn_state_reset_frame_0_t;

/* txn_state_reset_pad_0 is reset's pad: the blob, the empty base, the guard, the handle, and the saved base, while live. */
static void
txn_state_reset_pad_0(void *frame_)
{
	txn_state_reset_frame_0_t *frame = frame_;

	if (frame->bytes_live)
	{
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
	}
	if (frame->empty_live)
	{
		frame->empty_live = false;
		txn_base_Base_drop(&frame->empty);
	}
	if (frame->filling_live)
	{
		frame->filling_live = false;
		host_shared_Guard_drop(&frame->filling);
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->saved_live)
	{
		frame->saved_live = false;
		txn_base_Base_drop(frame->saved);
	}
}

/* txn_state_reset_body_0 is reset's body. */
static void
txn_state_reset_body_0(void *frame_)
{
	txn_state_reset_frame_0_t *frame = frame_;
	HostSharedRecord record;

	if (!txn_state_opened(frame->io, frame->storage, &frame->held, frame->error))
	{
		txn_state_reset_pad_0(frame);
		return;
	}
	frame->held_live = true;
	record = host_shared_Held_record(&frame->held);
	if (!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_STATE_LOCK_INDEX, &frame->filling,
								 frame->error))
	{
		txn_state_reset_pad_0(frame);
		return;
	}
	frame->filling_live = true;
	if (txn_state_load(record, TXN_STATE_READY) == 0)
	{
		frame->empty = txn_base_Base_empty(&frame->saved->key, frame->saved->table, frame->saved->has_index,
										   frame->saved->index);
		frame->saved->key.tag = TXN_BASE_KEY_UNKNOWN;
		frame->empty_live = true;
		txn_state_kept(&frame->empty, &frame->bytes);
		frame->bytes_live = true;
		if (!host_shared_GuardExclusive_write_blob(&frame->filling, 0, frame->bytes.ptr, frame->bytes.len,
												   frame->error))
		{
			txn_state_reset_pad_0(frame);
			return;
		}
		txn_state_exchange(record, TXN_STATE_COUNTER, 0, frame->empty.counter);
		txn_state_exchange(record, TXN_STATE_UNLOGGED, 0, (uint64) TXN_STATE_PERSISTENCE_UNLOGGED);
		txn_state_exchange(record, TXN_STATE_OWN, 0, (uint64) repo_Repo_own(frame->repo));
		txn_state_exchange(record, TXN_STATE_READY, 0, 1);
	}
	frame->ok = true;
	txn_state_reset_pad_0(frame);
}

/*
 * txn_state_reset makes an unlogged storage's state anew in a recovery's
 * start, taking the saved base, as Postgres leaves an unlogged relation
 * empty there: filled with a base record that holds nothing but the saved
 * one's key, table, and index, its counter at the first tuple id, and
 * unsaved, so the next checkpoint writes the empty record over the saved
 * one. The caller frees any state the storage had first; nothing on disk
 * changes before that checkpoint, so a start that crashes inside the reset
 * does it again.
 */
bool
txn_state_reset(HostIoIo *io, const RepoRepo *repo, const HostStorage *storage, TxnBaseBase *saved,
				HostError *error)
{
	txn_state_reset_frame_0_t frame = {.io = io, .repo = repo, .storage = storage, .saved = saved, .error = error,
	.saved_live = true};

	std_landing(&frame, txn_state_reset_body_0, txn_state_reset_pad_0);
	return frame.ok;
}

/* txn_state_uncounted_frame_0_t is the state of uncounted's walk at one key across its pad: the delta's hold and the key's versions, while live. */
typedef struct txn_state_uncounted_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord record;
	const uint8 *key;
	size_t		key_len;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	bool		versions_live;
} txn_state_uncounted_frame_0_t;

/* txn_state_uncounted_pad_0 is the walk's pad: the versions, then the hold, while live. */
static void
txn_state_uncounted_pad_0(void *frame_)
{
	txn_state_uncounted_frame_0_t *frame = frame_;

	if (frame->versions_live)
	{
		frame->versions_live = false;
		for (size_t k = 0, k_len = frame->versions.len; k < k_len; k++)
			txn_memtable_version_Version_drop(&STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, k));
		std_vec_free_Checked(&frame->versions, sizeof(TxnMemtableVersionVersion),
							 _Alignof(TxnMemtableVersionVersion));
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
}

/* txn_state_uncounted_body_0 is the walk's body at one key. */
static void
txn_state_uncounted_body_0(void *frame_)
{
	txn_state_uncounted_frame_0_t *frame = frame_;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_table_versions(frame->io, &frame->hold, frame->record, frame->key, frame->key_len,
									 &frame->versions, frame->error))
	{
		txn_state_uncounted_pad_0(frame);
		return;
	}
	frame->versions_live = true;
	for (size_t k = 0, k_len = frame->versions.len; k < k_len; k++)
	{
		TxnMemtableVersionRecord record =
			txn_memtable_version_Version_record(&STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, k));
		TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);
		TxnMemtableVersionOpRefTag op = txn_memtable_version_Record_op(&record).tag;
		HostXid		xid = {writer.has_top ? writer.top : writer.xid};
		bool		has;
		uint64		added;

		if (op == TXN_MEMTABLE_VERSION_OP_REF_LOCK || op == TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW)
			continue;
		if (!txn_status_add(frame->io, xid, PG_UINT64_MAX, &has, &added, frame->error))
		{
			txn_state_uncounted_pad_0(frame);
			return;
		}
	}
	frame->ok = true;
	txn_state_uncounted_pad_0(frame);
}

/* txn_state_uncounted_each is uncounted's walk at one key, over the state's record its captures name. */
static bool
txn_state_uncounted_each(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
						 size_t values_len, HostError *error)
{
	txn_state_uncounted_frame_0_t frame = {.io = io, .record = *(const HostSharedRecord *) captures, .key = key,
	.key_len = key_len, .error = error};

	std_landing(&frame, txn_state_uncounted_body_0, txn_state_uncounted_pad_0);
	return frame.ok;
}

/*
 * txn_state_uncounted takes back the add each counted version of a
 * storage's rows made at its writer's entry: every version but a lock's,
 * whose writer names an entry.
 */
static bool
txn_state_uncounted(HostIoIo *io, const HostSharedHeld *held, HostError *error)
{
	HostSharedRecord record = host_shared_Held_record(held);

	return txn_memtable_table_walked(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, record, txn_state_uncounted_each, &record,
									 NULL, error);
}

/* txn_state_cleared_body_0 is cleared's body, in the slot reads' frame. */
static void
txn_state_cleared_body_0(void *frame_)
{
	txn_state_slot_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	bool		some;
	TxnBaseBase empty;

	if (!txn_memtable_table_retire_all(frame->io, record, frame->error) ||
		!txn_state_slot_locked(frame, true) || !txn_state_slot_unkept(frame, 0, &some))
	{
		txn_state_slot_pad_0(frame);
		return;
	}
	frame->ok = true;
	if (some)
	{
		empty = txn_base_Base_empty(&frame->base.key, frame->base.table, frame->base.has_index, frame->base.index);
		frame->base.key.tag = TXN_BASE_KEY_UNKNOWN;
		txn_base_Base_drop(&frame->base);
		frame->base = empty;
		txn_state_kept(&frame->base, &frame->bytes);
		frame->bytes_live = true;
		frame->ok = host_shared_GuardExclusive_write_blob(&frame->guard, 0, frame->bytes.ptr, frame->bytes.len,
														  frame->error);
		if (frame->ok)
		{
			txn_state_stored(record, TXN_STATE_COUNTER, frame->base.counter);
			txn_state_stored(record, TXN_STATE_FOLD_DUE_INDEX, 0);
			txn_state_add(record, TXN_STATE_BASE_GENERATION, 1);
			txn_state_unsaved(record);
		}
	}
	txn_state_slot_pad_0(frame);
}

/*
 * txn_state_cleared retires a storage's memtables and tuple id table and
 * keeps an empty base with the newest one's key and table as its only base,
 * its counter at the first tuple id and its fold due word clear, unsaved, so
 * the next save writes the empty record over any saved one, and a read under
 * the moved generation reads it.
 */
static bool
txn_state_cleared(HostIoIo *io, const HostSharedHeld *held, HostError *error)
{
	txn_state_slot_frame_0_t frame = {.io = io, .held = held, .error = error};

	std_landing(&frame, txn_state_cleared_body_0, txn_state_slot_pad_0);
	return frame.ok;
}

/* TXN_STATE_CLEARING are the pauses a storage's clearing reaches inside its window, which the window's open loads. */
static const char *const TXN_STATE_CLEARING[] = {"doltgres-memtable-clearing", "doltgres-memtable-unlisting"};

/* txn_state_emptied_frame_0_t is emptied's state across its pad: the checkpoint's window and the record's body, while live. */
typedef struct txn_state_emptied_frame_0_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	HostError  *error;
	bool		ok;
	HostDelayCheckpoint window;
	bool		window_live;
	StdVec		body;
	bool		body_live;
} txn_state_emptied_frame_0_t;

/* txn_state_emptied_pad_0 is emptied's pad: the body, then the window, while live. */
static void
txn_state_emptied_pad_0(void *frame_)
{
	txn_state_emptied_frame_0_t *frame = frame_;

	if (frame->body_live)
	{
		frame->body_live = false;
		std_vec_free_Checked(&frame->body, 1, 1);
	}
	if (frame->window_live)
	{
		frame->window_live = false;
		host_DelayCheckpoint_drop(&frame->window);
	}
}

/* txn_state_emptied_body_0 is emptied's body. */
static void
txn_state_emptied_body_0(void *frame_)
{
	txn_state_emptied_frame_0_t *frame = frame_;
	TxnRecordStorageEmptied emptied;
	TxnStatePersistence persistence;

	if (!txn_state_uncounted(frame->io, frame->held, frame->error) ||
		!host_delay_checkpoint(HOST_IO_IO_IO_DOORS(frame->io), TXN_STATE_CLEARING, lengthof(TXN_STATE_CLEARING),
							   &frame->window, frame->error))
		return;
	frame->window_live = true;
	if (txn_state_described(host_shared_Held_record(frame->held), &emptied.storage, &persistence) &&
		persistence == TXN_STATE_PERSISTENCE_PERMANENT)
	{
		HostLsn		end;

		txn_record_StorageEmptied_encode(&emptied, &frame->body);
		frame->body_live = true;
		if (!host_wal_insert(HOST_IO_IO_IO_DOORS(frame->io), HOST_WAL_KIND_STORAGE_EMPTIED, frame->body.ptr,
							 frame->body.len, &end, frame->error))
		{
			txn_state_emptied_pad_0(frame);
			return;
		}
		frame->body_live = false;
		std_vec_free_Checked(&frame->body, 1, 1);
	}
	if (!txn_state_cleared(frame->io, frame->held, frame->error))
	{
		txn_state_emptied_pad_0(frame);
		return;
	}
	frame->window_live = false;
	host_DelayCheckpoint_finish(&frame->window);
	frame->ok = true;
}

/*
 * txn_state_emptied empties a storage in place for a truncate of a
 * relfilenode its subtransaction made, which no other process reads or
 * writes: it takes back its versions' counts, then, inside one checkpoint
 * window, logs a permanent storage's emptying and clears the storage as
 * cleared does.
 */
bool
txn_state_emptied(HostIoIo *io, const HostSharedHeld *held, HostError *error)
{
	txn_state_emptied_frame_0_t frame = {.io = io, .held = held, .error = error};

	std_landing(&frame, txn_state_emptied_body_0, txn_state_emptied_pad_0);
	return frame.ok;
}

/* txn_state_redo_emptied replays a storage's emptying: it takes back its replayed versions' counts and clears it as cleared does. */
bool
txn_state_redo_emptied(HostIoIo *io, const HostSharedHeld *held, HostError *error)
{
	return txn_state_uncounted(io, held, error) && txn_state_cleared(io, held, error);
}

/* txn_state_unlinked_frame_0_t is unlinked's state across its pad: the makers' guard, the sweep, and the claimed state, while live. */
typedef struct txn_state_unlinked_frame_0_t
{
	HostIoIo   *io;
	const HostStorage *storage;
	HostError  *error;
	bool		ok;
	HostSharedGuard making;
	bool		making_live;
	StdVec		swept;
	bool		swept_live;
	HostSharedOwned owned;
	bool		owned_live;
} txn_state_unlinked_frame_0_t;

/* txn_state_unlinked_pad_0 is unlinked's pad: the sweep, the makers' guard, then the claimed state, while live. */
static void
txn_state_unlinked_pad_0(void *frame_)
{
	txn_state_unlinked_frame_0_t *frame = frame_;

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

/* txn_state_unlinked_body_0 is unlinked's body. */
static void
txn_state_unlinked_body_0(void *frame_)
{
	txn_state_unlinked_frame_0_t *frame = frame_;
	HostSharedRecord makers;
	uint64		key = txn_state_key(frame->storage);

	if (!host_shared_single(&TXN_STATE_MAKERS_SINGLE_OPS, &makers, frame->error) ||
		!host_shared_Record_lock(makers, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_MAKING_INDEX, &frame->making,
								 frame->error))
		return;
	frame->making_live = true;
	if (!host_shared_sweep(HOST_SHARED_GUARD_WITHIN(&frame->making), &TXN_STATE_STATE_POOL_OPS, TXN_STATE_KEY,
						   &frame->swept, frame->error))
	{
		txn_state_unlinked_pad_0(frame);
		return;
	}
	frame->swept_live = true;
	for (size_t k = 0, k_len = frame->swept.len; k < k_len && !frame->owned_live; k++)
	{
		const std_TupleU64U64_t *found = &STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k);
		bool		has;

		if (found->f1 != key)
			continue;
		if (!host_shared_claim(HOST_SHARED_GUARD_WITHIN(&frame->making), &TXN_STATE_STATE_POOL_OPS, found->f0, &has,
							   &frame->owned, frame->error))
		{
			txn_state_unlinked_pad_0(frame);
			return;
		}
		if (!has)
			continue;
		if (txn_state_named(host_shared_Owned_record(&frame->owned), frame->storage))
			frame->owned_live = true;
		else
			host_shared_Owned_drop(&frame->owned);
	}
	frame->swept_live = false;
	std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->making_live = false;
	host_shared_Guard_drop(&frame->making);
	if (frame->owned_live &&
		!txn_memtable_table_retire_all(frame->io, host_shared_Owned_record(&frame->owned), frame->error))
	{
		txn_state_unlinked_pad_0(frame);
		return;
	}
	if (frame->owned_live)
	{
		frame->owned_live = false;
		host_shared_free(&frame->owned);
	}
	frame->ok = true;
}

/*
 * txn_state_unlinked frees a storage's state at its relfilenode's unlink: it
 * claims the state under the makers' lock, whatever processes hold it, after
 * which no find, gathering, or save reaches it; then, with that lock
 * released, retires its memtable with its ranges and the pages only they
 * name, and frees it, which goes when the last holder closes it. A storage
 * with no state has nothing to free, and a state whose slot was made again
 * for another storage since the sweep passed it is given back.
 */
bool
txn_state_unlinked(HostIoIo *io, const HostStorage *storage, HostError *error)
{
	txn_state_unlinked_frame_0_t frame = {.io = io, .storage = storage, .error = error};

	std_landing(&frame, txn_state_unlinked_body_0, txn_state_unlinked_pad_0);
	return frame.ok;
}

/* txn_state_held_back_frame_0_t is held_back's frame: the sweep's frame, the time, and its answer. */
typedef struct txn_state_held_back_frame_0_t
{
	txn_state_sweep_frame_0_t sweep;
	bool	   *some;
	txn_TupleU64StorageU64_t *result;
} txn_state_held_back_frame_0_t;

/* txn_state_held_back_pad_0 is held_back's pad: the sweep's. */
static void
txn_state_held_back_pad_0(void *frame_)
{
	txn_state_held_back_frame_0_t *frame = frame_;

	txn_state_sweep_pad_0(&frame->sweep);
}

/* txn_state_held_back_body_0 is held_back's body. */
static void
txn_state_held_back_body_0(void *frame_)
{
	txn_state_held_back_frame_0_t *frame = frame_;
	uint64		now = txn_state_micros();

	*frame->some = false;
	if (!txn_state_swept(&frame->sweep))
		return;
	for (size_t k = 0, k_len = frame->sweep.swept.len; k < k_len; k++)
	{
		bool		opened;
		HostSharedRecord record;
		uint64		at;
		TxnStatePersistence persistence;

		if (!txn_state_swept_open(&frame->sweep, k, &opened))
		{
			txn_state_held_back_pad_0(frame);
			return;
		}
		if (!opened)
			continue;
		record = host_shared_Held_record(&frame->sweep.held);
		at = txn_state_load(record, TXN_STATE_REACHED_AT);
		if (at == 0 || (*frame->some && frame->result->f0 <= at))
		{
			txn_state_swept_close(&frame->sweep);
			continue;
		}
		if (!txn_state_described(record, &frame->result->f1, &persistence))
		{
			*frame->some = false;
			*frame->sweep.error = txn_state_internal("a state holding a reached point isn't filled",
													 strlen("a state holding a reached point isn't filled"));
			txn_state_held_back_pad_0(frame);
			return;
		}
		frame->result->f0 = at;
		frame->result->f2 = std_saturating_sub_u64(now, txn_state_load(record, TXN_STATE_REACHED_SINCE));
		*frame->some = true;
		txn_state_swept_close(&frame->sweep);
	}
	frame->sweep.ok = true;
	txn_state_held_back_pad_0(frame);
}

/*
 * txn_state_held_back returns the end of the oldest replayed fold's record a
 * state's slot holds, or whose read past it isn't done, among every filled
 * state, which a restartpoint never passes, with its storage and how long it
 * has held one, or nothing for none.
 */
bool
txn_state_held_back(HostIoIo *io, bool *some, txn_TupleU64StorageU64_t *result, HostError *error)
{
	txn_state_held_back_frame_0_t frame = {.sweep = {.io = io, .error = error}, .some = some, .result = result};

	std_landing(&frame, txn_state_held_back_body_0, txn_state_held_back_pad_0);
	return frame.sweep.ok;
}

/* txn_state_TupleU64HeldState_drop is a (u64, Held<State>)'s drop glue: its handle's. */
static void
txn_state_TupleU64HeldState_drop(void *element)
{
	host_shared_Held_drop(&((txn_TupleU64HeldState_t *) element)->f1);
}

/* txn_state_states_frame_0_t is states' frame: the sweep's frame and the database. */
typedef struct txn_state_states_frame_0_t
{
	txn_state_sweep_frame_0_t sweep;
	uint32		database;
	StdVec	   *result;
} txn_state_states_frame_0_t;

/* txn_state_states_pad_0 is states' pad: the sweep's. */
static void
txn_state_states_pad_0(void *frame_)
{
	txn_state_states_frame_0_t *frame = frame_;

	txn_state_sweep_pad_0(&frame->sweep);
}

/* txn_state_states_body_0 is states' body. */
static void
txn_state_states_body_0(void *frame_)
{
	txn_state_states_frame_0_t *frame = frame_;

	std_vec_new(&frame->sweep.found, sizeof(txn_TupleU64HeldState_t), _Alignof(txn_TupleU64HeldState_t));
	frame->sweep.found_live = true;
	if (!txn_state_swept(&frame->sweep))
	{
		txn_state_states_pad_0(frame);
		return;
	}
	for (size_t k = 0, k_len = frame->sweep.swept.len; k < k_len; k++)
	{
		const std_TupleU64U64_t *swept = &STD_VEC_AT(&frame->sweep.swept, std_TupleU64U64_t, k);
		bool		opened;
		txn_TupleU64HeldState_t found;

		if (STD_SHR_FOLDED(u64, swept->f1, 32) != frame->database)
			continue;
		if (!txn_state_swept_open(&frame->sweep, k, &opened))
		{
			txn_state_states_pad_0(frame);
			return;
		}
		if (!opened)
			continue;
		found.f0 = swept->f0;
		found.f1 = frame->sweep.held;
		if (!txn_state_swept_push(&frame->sweep, &found))
		{
			txn_state_states_pad_0(frame);
			return;
		}
		frame->sweep.held_live = false;
	}
	frame->sweep.found_live = false;
	*frame->result = frame->sweep.found;
	frame->sweep.ok = true;
	txn_state_states_pad_0(frame);
}

/*
 * txn_state_states returns a handle on every published state of a
 * database's storages with its location, from a sweep of the pool, each
 * opened and its key compared again under the handle, so a slot made again
 * for another storage meanwhile is passed over.
 */
bool
txn_state_states(HostIoIo *io, uint32 database, StdVec *result, HostError *error)
{
	txn_state_states_frame_0_t frame = {.sweep = {.io = io, .error = error,
			.found_size = sizeof(txn_TupleU64HeldState_t),
	.found_align = _Alignof(txn_TupleU64HeldState_t), .found_drop = txn_state_TupleU64HeldState_drop},
	.database = database, .result = result};

	std_landing(&frame, txn_state_states_body_0, txn_state_states_pad_0);
	return frame.sweep.ok;
}

/* txn_state_states_free drops a list states returned. */
static void
txn_state_states_free(StdVec *states)
{
	for (size_t k = 0, k_len = states->len; k < k_len; k++)
		txn_state_TupleU64HeldState_drop(&STD_VEC_AT(states, txn_TupleU64HeldState_t, k));
	std_vec_free_Checked(states, sizeof(txn_TupleU64HeldState_t), _Alignof(txn_TupleU64HeldState_t));
}

/* txn_state_due_lt is due's sort's is_less, Reverse(bytes)'s lt: the larger bytes first. */
static bool
txn_state_due_lt(void *captures, const void *a, const void *b)
{
	return ((const TxnStateDue *) b)->bytes < ((const TxnStateDue *) a)->bytes;
}

/* The unstable sort of due's storages, largest first: core's sort at Due. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS TxnStateDue
#define STD_SORT_T TxnStateDue
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 0
#define STD_SORT_BRANCHLESS 1
#include "doltgres/std/sort_template.h"

/* txn_state_due_frame_0_t is due's frame: the sweep's frame, the due bounds, and whether recovery is in progress. */
typedef struct txn_state_due_frame_0_t
{
	txn_state_sweep_frame_0_t sweep;
	uint64		threshold;
	uint64		timeout;
	bool		asked;
	StdVec	   *result;
} txn_state_due_frame_0_t;

/* txn_state_due_pad_0 is due's pad: the sweep's. */
static void
txn_state_due_pad_0(void *frame_)
{
	txn_state_due_frame_0_t *frame = frame_;

	txn_state_sweep_pad_0(&frame->sweep);
}

/*
 * txn_state_due_based reads the base a storage at a sweep's handle is due
 * with into the sweep's frame, answering false on an error.
 */
static bool
txn_state_due_based(txn_state_due_frame_0_t *frame, bool recovering, uint64 bytes, bool old)
{
	bool		behind;
	bool		past;
	uint64		from;
	bool		some = false;
	TxnBaseBase reached;

	if (!txn_state_behind(frame->sweep.io, &frame->sweep.held, &behind, &reached, frame->sweep.error))
		return false;
	if (behind)
	{
		frame->sweep.base = reached;
		frame->sweep.base_live = true;
	}
	if (!txn_state_past(frame->sweep.io, &frame->sweep.held, &past, &from, frame->sweep.error))
		return false;
	if (behind)
		return true;
	if (past || (!recovering && bytes != 0 && (frame->asked || old || bytes >= frame->threshold)))
	{
		if (!txn_state_base_record(frame->sweep.io, &frame->sweep.held, &some, &frame->sweep.base, frame->sweep.error))
			return false;
		frame->sweep.base_live = some;
	}
	return true;
}

/* txn_state_due_body_0 is due's body. */
static void
txn_state_due_body_0(void *frame_)
{
	txn_state_due_frame_0_t *frame = frame_;
	uint64		now = txn_state_micros();
	bool		recovering = host_wal_stand(HOST_IO_IO_IO_DOORS(frame->sweep.io)).recovering;
	StdSortLess is_less = {txn_state_due_lt, NULL};

	std_vec_new(&frame->sweep.found, sizeof(TxnStateDue), _Alignof(TxnStateDue));
	frame->sweep.found_live = true;
	if (!txn_state_swept(&frame->sweep))
	{
		txn_state_due_pad_0(frame);
		return;
	}
	for (size_t k = 0, k_len = frame->sweep.swept.len; k < k_len; k++)
	{
		bool		opened;
		HostSharedRecord record;
		TxnStateDue due;
		TxnStatePersistence persistence;
		std_TupleU64U64_t pending;
		uint64		spilled;

		if (!txn_state_swept_open(&frame->sweep, k, &opened))
		{
			txn_state_due_pad_0(frame);
			return;
		}
		if (!opened)
			continue;
		record = host_shared_Held_record(&frame->sweep.held);
		if (!txn_state_described(record, &due.storage, &persistence))
		{
			txn_state_swept_close(&frame->sweep);
			continue;
		}
		pending = txn_state_pending(record);
		if (!txn_state_spilled(frame->sweep.io, record, &spilled, frame->sweep.error))
		{
			txn_state_due_pad_0(frame);
			return;
		}
		due.bytes = STD_ADD(u64, pending.f0, spilled, 1409, 27);
		if (!txn_state_due_based(frame, recovering, due.bytes,
								 pending.f1 != 0 && std_saturating_sub_u64(now, pending.f1) >= frame->timeout))
		{
			txn_state_due_pad_0(frame);
			return;
		}
		if (frame->sweep.base_live)
		{
			due.table = frame->sweep.base.table;
			due.has_index = frame->sweep.base.has_index;
			due.index = frame->sweep.base.has_index ? frame->sweep.base.index : 0;
			frame->sweep.base_live = false;
			txn_base_Base_drop(&frame->sweep.base);
			if (!txn_state_swept_push(&frame->sweep, &due))
			{
				txn_state_due_pad_0(frame);
				return;
			}
		}
		txn_state_swept_close(&frame->sweep);
	}
	std_sort_sort_TxnStateDue(frame->sweep.found.ptr, frame->sweep.found.len, &is_less);
	frame->sweep.found_live = false;
	*frame->result = frame->sweep.found;
	frame->sweep.ok = true;
	txn_state_due_pad_0(frame);
}

/*
 * txn_state_due returns each storage whose delta is due a fold, largest
 * first, from a sweep of the pool, counting its spills' files with its
 * delta's bytes, so a spill never makes a storage look folded: one holding
 * at least threshold bytes, one whose oldest unfolded version is timeout
 * microseconds old, and, when asked, one holding any version, as a writer
 * refused at the delta's budget or the checkpointer asks. A storage a
 * replayed fold's record reached past its newest base is due, the record's
 * base naming its table, as is one whose read past its reached record isn't
 * done, in recovery or after a promotion, since a standby folds every one of
 * its primary's fold points in order; in recovery no other storage is. A
 * storage with no base, which no fold has made yet, is passed over
 * otherwise, since a worker opens a relation by its base's table id.
 */
bool
txn_state_due(HostIoIo *io, uint64 threshold, uint64 timeout, bool asked, StdVec *result, HostError *error)
{
	txn_state_due_frame_0_t frame = {.sweep = {.io = io, .error = error, .found_size = sizeof(TxnStateDue),
	.found_align = _Alignof(TxnStateDue)}, .threshold = threshold, .timeout = timeout, .asked = asked,
	.result = result};

	std_landing(&frame, txn_state_due_body_0, txn_state_due_pad_0);
	return frame.sweep.ok;
}

/*
 * txn_state_rekeyed stores the tablespace a database's default moved to in
 * place of the one it moved from as the tablespace of each of the
 * database's states' storages, as the move gives every relation of its
 * default tablespace the new one in its locator, so that a read after the
 * move finds the state its storage had.
 */
bool
txn_state_rekeyed(HostIoIo *io, uint32 database, uint32 from, uint32 to, HostError *error)
{
	StdVec		states;

	if (!txn_state_states(io, database, &states, error))
		return false;
	for (size_t k = 0, k_len = states.len; k < k_len; k++)
		txn_state_exchange(host_shared_Held_record(&STD_VEC_AT(&states, txn_TupleU64HeldState_t, k).f1),
						   TXN_STATE_TABLESPACE, from, to);
	txn_state_states_free(&states);
	return true;
}

/*
 * txn_state_rehomed rekeys a database's states and stores the tablespace
 * its repository moved to in place of the one it moved from as the
 * repository tablespace of each, at the move's finish, whose copy is the
 * repository's own directory from then on, so that a checkpoint, a fold, and
 * a spill's files find the repository where it stands.
 */
bool
txn_state_rehomed(HostIoIo *io, uint32 database, uint32 from, uint32 to, HostError *error)
{
	StdVec		states;

	if (!txn_state_rekeyed(io, database, from, to, error) || !txn_state_states(io, database, &states, error))
		return false;
	for (size_t k = 0, k_len = states.len; k < k_len; k++)
		txn_state_exchange(host_shared_Held_record(&STD_VEC_AT(&states, txn_TupleU64HeldState_t, k).f1),
						   TXN_STATE_OWN, from, to);
	txn_state_states_free(&states);
	return true;
}

/* txn_state_repositories_frame_0_t is repositories' frame: the sweep's frame and its answer. */
typedef struct txn_state_repositories_frame_0_t
{
	txn_state_sweep_frame_0_t sweep;
	StdVec	   *result;
} txn_state_repositories_frame_0_t;

/* txn_state_repositories_pad_0 is repositories' pad: the sweep's. */
static void
txn_state_repositories_pad_0(void *frame_)
{
	txn_state_repositories_frame_0_t *frame = frame_;

	txn_state_sweep_pad_0(&frame->sweep);
}

/*
 * txn_state_repositories_listed adds a filled state's repository to what
 * repositories found, refusing a second tablespace for its database,
 * answering false on an error.
 */
static bool
txn_state_repositories_listed(txn_state_repositories_frame_0_t *frame, std_TupleU32U32_t repository)
{
	for (size_t k = 0, k_len = frame->sweep.found.len; k < k_len; k++)
	{
		const std_TupleU32U32_t *other = &STD_VEC_AT(&frame->sweep.found, std_TupleU32U32_t, k);

		if (other->f0 != repository.f0)
			continue;
		if (other->f1 != repository.f1)
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("states of database "), STD_FMT_ARG(0),
				STD_FMT_LIT(" name two repository tablespaces, "), STD_FMT_ARG(1), STD_FMT_LIT(" and "),
			STD_FMT_ARG(2)};
			StdFmtArg	args[] = {{&repository.f0, std_u32_Display_fmt}, {&other->f1, std_u32_Display_fmt},
			{&repository.f1, std_u32_Display_fmt}};
			StdFmtArguments arguments = {pieces, 6, args};
			StdString	message;

			std_fmt_format_Checked(&message, &arguments);
			*frame->sweep.error = host_Error_fork((const uint8 *) "XX001", &message);
			return false;
		}
		return true;
	}
	return txn_state_swept_push(&frame->sweep, &repository);
}

/* txn_state_repositories_body_0 is repositories' body. */
static void
txn_state_repositories_body_0(void *frame_)
{
	txn_state_repositories_frame_0_t *frame = frame_;

	std_vec_new(&frame->sweep.found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	frame->sweep.found_live = true;
	if (!txn_state_swept(&frame->sweep))
	{
		txn_state_repositories_pad_0(frame);
		return;
	}
	for (size_t k = 0, k_len = frame->sweep.swept.len; k < k_len; k++)
	{
		bool		opened;
		HostSharedRecord record;
		HostStorage storage;
		TxnStatePersistence persistence;
		std_TupleU32U32_t repository;

		if (!txn_state_swept_open(&frame->sweep, k, &opened))
		{
			txn_state_repositories_pad_0(frame);
			return;
		}
		if (!opened)
			continue;
		record = host_shared_Held_record(&frame->sweep.held);
		if (txn_state_described(record, &storage, &persistence))
		{
			repository.f0 = (uint32) STD_SHR_FOLDED(u64, STD_VEC_AT(&frame->sweep.swept, std_TupleU64U64_t, k).f1, 32);
			repository.f1 = (uint32) txn_state_load(record, TXN_STATE_OWN);
			if (!txn_state_repositories_listed(frame, repository))
			{
				txn_state_repositories_pad_0(frame);
				return;
			}
		}
		txn_state_swept_close(&frame->sweep);
	}
	frame->sweep.found_live = false;
	*frame->result = frame->sweep.found;
	frame->sweep.ok = true;
	txn_state_repositories_pad_0(frame);
}

/*
 * txn_state_repositories returns each repository with a filled state, its
 * database and its own directory's tablespace, from a sweep of the pool,
 * refusing as data corruption (XX001) two states of one database that name
 * two tablespaces, so that no repository is written twice.
 */
bool
txn_state_repositories(HostIoIo *io, StdVec *result, HostError *error)
{
	txn_state_repositories_frame_0_t frame = {.sweep = {.io = io, .error = error,
			.found_size = sizeof(std_TupleU32U32_t),
	.found_align = _Alignof(std_TupleU32U32_t)}, .result = result};

	std_landing(&frame, txn_state_repositories_body_0, txn_state_repositories_pad_0);
	return frame.sweep.ok;
}

/* txn_state_own returns the tablespace of the own directory of the repository a filled state's storage is in. */
uint32
txn_state_own(HostSharedRecord record)
{
	return (uint32) txn_state_load(record, TXN_STATE_OWN);
}

/* txn_state_unlogged_frame_0_t is unlogged's frame: the sweep's frame and its answer. */
typedef struct txn_state_unlogged_frame_0_t
{
	txn_state_sweep_frame_0_t sweep;
	StdVec	   *result;
} txn_state_unlogged_frame_0_t;

/* txn_state_unlogged_pad_0 is unlogged's pad: the sweep's. */
static void
txn_state_unlogged_pad_0(void *frame_)
{
	txn_state_unlogged_frame_0_t *frame = frame_;

	txn_state_sweep_pad_0(&frame->sweep);
}

/* txn_state_unlogged_body_0 is unlogged's body. */
static void
txn_state_unlogged_body_0(void *frame_)
{
	txn_state_unlogged_frame_0_t *frame = frame_;

	std_vec_new(&frame->sweep.found, sizeof(HostStorage), _Alignof(HostStorage));
	frame->sweep.found_live = true;
	if (!txn_state_swept(&frame->sweep))
	{
		txn_state_unlogged_pad_0(frame);
		return;
	}
	for (size_t k = 0, k_len = frame->sweep.swept.len; k < k_len; k++)
	{
		bool		opened;
		HostStorage storage;
		TxnStatePersistence persistence;

		if (!txn_state_swept_open(&frame->sweep, k, &opened))
		{
			txn_state_unlogged_pad_0(frame);
			return;
		}
		if (!opened)
			continue;
		if (txn_state_described(host_shared_Held_record(&frame->sweep.held), &storage, &persistence) &&
			persistence == TXN_STATE_PERSISTENCE_UNLOGGED && !txn_state_swept_push(&frame->sweep, &storage))
		{
			txn_state_unlogged_pad_0(frame);
			return;
		}
		txn_state_swept_close(&frame->sweep);
	}
	frame->sweep.found_live = false;
	*frame->result = frame->sweep.found;
	frame->sweep.ok = true;
	txn_state_unlogged_pad_0(frame);
}

/* txn_state_unlogged returns the storage of every filled state of an unlogged storage, from a sweep of the pool. */
bool
txn_state_unlogged(HostIoIo *io, StdVec *result, HostError *error)
{
	txn_state_unlogged_frame_0_t frame = {.sweep = {.io = io, .error = error, .found_size = sizeof(HostStorage),
	.found_align = _Alignof(HostStorage)}, .result = result};

	std_landing(&frame, txn_state_unlogged_body_0, txn_state_unlogged_pad_0);
	return frame.sweep.ok;
}

/* txn_state_gathered_frame_0_t is gathered's frame: the slot reads' frame, the save's token, and the edit it gives. */
typedef struct txn_state_gathered_frame_0_t
{
	txn_state_slot_frame_0_t slot;
	uint64		token;
	bool	   *some;
	RepoGathered *result;
} txn_state_gathered_frame_0_t;

/* txn_state_gathered_pad_0 is gathered's pad: the slot reads'. */
static void
txn_state_gathered_pad_0(void *frame_)
{
	txn_state_gathered_frame_0_t *frame = frame_;

	txn_state_slot_pad_0(&frame->slot);
}

/* txn_state_gathered_body_0 is gathered's body. */
static void
txn_state_gathered_body_0(void *frame_)
{
	txn_state_gathered_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->slot.held);
	uint64		saved;
	bool		some;
	uint64		counter;
	uint64		key;

	*frame->some = false;
	if (!txn_state_slot_locked(&frame->slot, true))
		return;
	saved = txn_state_load(record, TXN_STATE_SAVED);
	if (txn_state_load(record, TXN_STATE_READY) == 0 || saved == TXN_STATE_CLEAN)
	{
		frame->slot.ok = true;
		txn_state_gathered_pad_0(frame);
		return;
	}
	if (!txn_state_slot_unkept(&frame->slot, 0, &some))
	{
		txn_state_gathered_pad_0(frame);
		return;
	}
	if ((txn_state_load(record, TXN_STATE_MEMTABLE_WORD_INDEX) | txn_state_load(record, TXN_STATE_FREE_WORD_INDEX) |
		 txn_state_load(record, TXN_STATE_TUPLES_WORD_INDEX)) == 0)
	{
		txn_state_stored(record, TXN_STATE_COUNTER, some ? frame->slot.base.fresh_end : 1);
		if (!some)
			txn_state_exchange(record, TXN_STATE_SAVED, saved, TXN_STATE_CLEAN);
	}
	frame->slot.ok = true;
	if (!some)
	{
		txn_state_gathered_pad_0(frame);
		return;
	}
	counter = txn_state_load(record, TXN_STATE_COUNTER);
	if (frame->slot.base.counter != counter)
	{
		frame->slot.base.counter = counter;
		txn_state_kept(&frame->slot.base, &frame->slot.bytes);
		frame->slot.bytes_live = true;
		frame->slot.ok = host_shared_GuardExclusive_write_blob(&frame->slot.guard, 0, frame->slot.bytes.ptr,
															   frame->slot.bytes.len, frame->slot.error);
		if (!frame->slot.ok)
		{
			txn_state_gathered_pad_0(frame);
			return;
		}
	}
	txn_state_exchange(record, TXN_STATE_SAVED, saved, frame->token);
	key = txn_state_load(record, TXN_STATE_KEY);
	frame->result->tag = REPO_GATHERED_BASE;
	frame->result->u.base.storage.tablespace = (uint32) txn_state_load(record, TXN_STATE_TABLESPACE);
	frame->result->u.base.storage.relation = (uint32) key;
	frame->result->u.base.folded = frame->slot.base.folded;
	txn_base_Base_encode(&frame->slot.base, &frame->result->u.base.record.bytes,
						 &frame->result->u.base.record.addresses);
	*frame->some = true;
	txn_state_gathered_pad_0(frame);
}

/*
 * txn_state_gathered is one state's part of a save: under the state's lock,
 * for a ready state whose saved isn't clean, it first lets the tuple id
 * counter fall back to the next tuple id past every row its base record
 * holds, (0,1) for no base, when no memtable of its rows or its free space
 * map and no tuple id table is linked, since no version or entry then names
 * a tuple id or a place past it and no insert has taken one; then it stores
 * the save's token in saved and returns the record a make would read back,
 * its counter the word's one load, which it keeps in the blob, so the blob
 * is the record the manifest holds once the save lands. A storage with no
 * base gives no record: idle, its state is the manifest's and turns clean,
 * and otherwise its tuple ids are named only by versions its WAL redoes.
 */
static bool
txn_state_gathered(const HostSharedHeld *held, HostIoIo *io, uint64 token, bool *some, RepoGathered *result,
				   HostError *error)
{
	txn_state_gathered_frame_0_t frame = {.slot = {.io = io, .held = held, .error = error}, .token = token,
	.some = some, .result = result};

	std_landing(&frame, txn_state_gathered_body_0, txn_state_gathered_pad_0);
	return frame.slot.ok;
}

/* txn_state_gather_frame_0_t is gather's state across its pad: the database's states, and the edits and locations gathered, while live. */
typedef struct txn_state_gather_frame_0_t
{
	HostIoIo   *io;
	uint32		database;
	StdVec	   *result_0;
	TxnStateSaving *result_1;
	HostError  *error;
	bool		ok;
	StdVec		states;
	bool		states_live;
	StdVec		edits;
	bool		edits_live;
	StdVec		locations;
	bool		locations_live;
	RepoGathered edit;
	bool		edit_live;
} txn_state_gather_frame_0_t;

/* txn_state_gather_pad_0 is gather's pad: the edit being pushed, the locations, the edits, then the states, while live. */
static void
txn_state_gather_pad_0(void *frame_)
{
	txn_state_gather_frame_0_t *frame = frame_;

	if (frame->edit_live)
	{
		frame->edit_live = false;
		repo_Gathered_drop(&frame->edit);
	}
	if (frame->locations_live)
	{
		frame->locations_live = false;
		std_vec_free_Checked(&frame->locations, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->edits_live)
	{
		frame->edits_live = false;
		for (size_t k = 0, k_len = frame->edits.len; k < k_len; k++)
			repo_Gathered_drop(&STD_VEC_AT(&frame->edits, RepoGathered, k));
		std_vec_free_Checked(&frame->edits, sizeof(RepoGathered), _Alignof(RepoGathered));
	}
	if (frame->states_live)
	{
		frame->states_live = false;
		txn_state_states_free(&frame->states);
	}
}

/* txn_state_gather_body_0 is gather's body. */
static void
txn_state_gather_body_0(void *frame_)
{
	txn_state_gather_frame_0_t *frame = frame_;
	HostSharedRecord makers;
	uint64		token;

	if (!host_shared_single(&TXN_STATE_MAKERS_SINGLE_OPS, &makers, frame->error))
		return;
	token = STD_ADD(u64, txn_state_add(makers, TXN_STATE_TOKENS, 1), 1, 1501, 66);
	std_vec_new(&frame->edits, sizeof(RepoGathered), _Alignof(RepoGathered));
	frame->edits_live = true;
	std_vec_new(&frame->locations, sizeof(uint64), _Alignof(uint64));
	frame->locations_live = true;
	if (!txn_state_states(frame->io, frame->database, &frame->states, frame->error))
	{
		txn_state_gather_pad_0(frame);
		return;
	}
	frame->states_live = true;
	for (size_t k = 0, k_len = frame->states.len; k < k_len; k++)
	{
		const txn_TupleU64HeldState_t *state = &STD_VEC_AT(&frame->states, txn_TupleU64HeldState_t, k);
		bool		some;

		if (!txn_state_gathered(&state->f1, frame->io, token, &some, &frame->edit, frame->error))
		{
			txn_state_gather_pad_0(frame);
			return;
		}
		if (!some)
			continue;
		frame->edit_live = true;
		std_vec_push_Checked(&frame->edits, &frame->edit, sizeof(frame->edit), _Alignof(RepoGathered));
		frame->edit_live = false;
		std_vec_push_Checked(&frame->locations, &state->f0, sizeof(uint64), _Alignof(uint64));
	}
	frame->edits_live = false;
	*frame->result_0 = frame->edits;
	frame->locations_live = false;
	frame->result_1->token = token;
	frame->result_1->locations = frame->locations;
	frame->ok = true;
	txn_state_gather_pad_0(frame);
}

/*
 * txn_state_gather is the save's gathering for a repository's database,
 * called after the save's read of the manifest: for each ready state of the
 * database whose saved isn't clean, read one at a time under each state's
 * lock, it gives the record a make would read back and stores the save's
 * token in saved, which saved turns to clean once the manifest that holds
 * the record is written.
 */
bool
txn_state_gather(HostIoIo *io, uint32 database, const RepoRead *read, StdVec *result_0, TxnStateSaving *result_1,
				 HostError *error)
{
	txn_state_gather_frame_0_t frame = {.io = io, .database = database, .result_0 = result_0, .result_1 = result_1,
	.error = error};

	std_landing(&frame, txn_state_gather_body_0, txn_state_gather_pad_0);
	return frame.ok;
}

/* txn_state_Saving_saved_frame_0_t is saved's state across its pad: the saving it takes, while live. */
typedef struct txn_state_Saving_saved_frame_0_t
{
	TxnStateSaving *self;
	HostIoIo   *io;
	HostError  *error;
	bool		ok;
	bool		self_live;
} txn_state_Saving_saved_frame_0_t;

/* txn_state_Saving_saved_pad_0 is saved's pad: the saving's locations, while live. */
static void
txn_state_Saving_saved_pad_0(void *frame_)
{
	txn_state_Saving_saved_frame_0_t *frame = frame_;

	if (frame->self_live)
	{
		frame->self_live = false;
		std_vec_free_Checked(&frame->self->locations, sizeof(uint64), _Alignof(uint64));
	}
}

/* txn_state_Saving_saved_body_0 is saved's body. */
static void
txn_state_Saving_saved_body_0(void *frame_)
{
	txn_state_Saving_saved_frame_0_t *frame = frame_;

	for (size_t k = 0, k_len = frame->self->locations.len; k < k_len; k++)
	{
		bool		some;
		HostSharedHeld held;

		if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), &TXN_STATE_STATE_POOL_OPS,
							  STD_VEC_AT(&frame->self->locations, uint64, k), &some, &held, frame->error))
		{
			txn_state_Saving_saved_pad_0(frame);
			return;
		}
		if (!some)
			continue;
		txn_state_exchange(host_shared_Held_record(&held), TXN_STATE_SAVED, frame->self->token, TXN_STATE_CLEAN);
		host_shared_Held_drop(&held);
	}
	frame->ok = true;
	txn_state_Saving_saved_pad_0(frame);
}

/*
 * txn_state_Saving_saved is called once the save's manifest write landed,
 * taking the saving: it turns each gathered state's saved from the token to
 * clean, and leaves a state whose word moved since, by a write or another
 * gathering, as it is.
 */
bool
txn_state_Saving_saved(TxnStateSaving *self, HostIoIo *io, HostError *error)
{
	txn_state_Saving_saved_frame_0_t frame = {.self = self, .io = io, .error = error, .self_live = true};

	std_landing(&frame, txn_state_Saving_saved_body_0, txn_state_Saving_saved_pad_0);
	return frame.ok;
}

/* txn_state_counter loads a state's tuple id counter, the next fresh tuple id, with no lock. */
uint64
txn_state_counter(HostSharedRecord record)
{
	return txn_state_load(record, TXN_STATE_COUNTER);
}

/*
 * txn_state_space returns the space of the packs a storage's trees are
 * written into, its own tablespace, so a fold's packs go where the storage
 * is: the repository's own directory's for the repository's own tablespace,
 * and the tablespace's id for any other.
 */
uint32
txn_state_space(HostSharedRecord record)
{
	uint32		tablespace = (uint32) txn_state_load(record, TXN_STATE_TABLESPACE);

	return tablespace == (uint32) txn_state_load(record, TXN_STATE_OWN) ? CHUNKS_PACKS_DEFAULT_SPACE : tablespace;
}

/*
 * txn_state_moved moves a state's counter from the tuple id a take read to
 * the next, under the counter's block's range lock, and marks the state not
 * clean, so the next save gathers the counter; a gathering's token in saved
 * turns to not clean too, so its "saved" leaves the state for the save
 * after.
 */
void
txn_state_moved(HostSharedRecord record, uint64 from, uint64 to)
{
	txn_state_exchange(record, TXN_STATE_COUNTER, from, to);
	txn_state_unsaved(record);
}

/* txn_TupleU64U32HeldState_t is a held state with its storage's key and tablespace, (u64, u32, Held<State>). */
typedef struct txn_TupleU64U32HeldState_t
{
	uint64		f0;
	uint32		f1;
	HostSharedHeld f2;
} txn_TupleU64U32HeldState_t;

/* txn_state_current_default is [AtomicU64; 2]'s Default for a BackendLocal of one: both zero. */
static void
txn_state_current_default(void *result, const StdLocation *caller)
{
	std_AtomicU64_new(&((StdAtomicU64 *) result)[0], 0);
	std_AtomicU64_new(&((StdAtomicU64 *) result)[1], 0);
}

/* TXN_STATE_CURRENT_TYPE is [AtomicU64; 2]'s size, Default, and drop, which does nothing. */
const StdDefaultType TXN_STATE_CURRENT_TYPE = {2 * sizeof(StdAtomicU64), _Alignof(StdAtomicU64),
txn_state_current_default, NULL};

/*
 * TXN_STATE_CURRENT is CURRENT: the handle of the backend's record of held
 * states, its slot and its generation, with a generation of zero when it
 * has none; its BackendLocal header and the process's value, which follows
 * it.
 */
static struct
{
	HostLocalBackendLocal local;
	StdAtomicU64 process[2];
}			TXN_STATE_CURRENT = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* TXN_STATE_HOLDS is HOLDS: the backend's record of held states, one for its top-level transaction once it holds any. */
HostOwnedTable TXN_STATE_HOLDS = HOST_OWNED_TABLE_NEW;

/* txn_state_Holds_table returns Holds' table, HOLDS. */
static HostOwnedTable *
txn_state_Holds_table(void)
{
	return &TXN_STATE_HOLDS;
}

/* txn_state_Holds_drop forgets the backend's handle on the record, so its next transaction makes its own, then drops its states. */
static void
txn_state_Holds_drop(void *record)
{
	TxnStateHolds *holds = record;
	HostLocalLocal current;

	if (host_local_BackendLocal_existing(&TXN_STATE_CURRENT.local, &current))
	{
		std_AtomicU64_store_relaxed(&((StdAtomicU64 *) host_local_Local_Deref_deref(&current,
																					&TXN_STATE_CURRENT_TYPE))[1], 0);
		host_local_Local_drop(&current, &TXN_STATE_CURRENT_TYPE);
	}
	for (size_t k = 0, k_len = holds->states.len; k < k_len; k++)
		host_shared_Held_drop(&STD_VEC_AT(&holds->states, txn_TupleU64U32HeldState_t, k).f2);
	std_vec_free_Checked(&holds->states, sizeof(txn_TupleU64U32HeldState_t), _Alignof(txn_TupleU64U32HeldState_t));
}

/* Holds' Table, host's owned Table<Holds>, with its kind's glue. */
#define HOST_OWNED_SCOPE extern
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX txn_state_Holds
#define HOST_OWNED_RS TxnStateHolds
#define HOST_OWNED_R TxnStateHolds
#define HOST_OWNED_R_DROP txn_state_Holds_drop
#define HOST_OWNED_KIND 1
#define HOST_OWNED_TABLE_FN txn_state_Holds_table
#include "doltgres/host/owned_template.h"

/* Holds' Table's with at (), for the calls that give nothing back. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX txn_state_Holds
#define HOST_OWNED_RS TxnStateHolds
#define HOST_OWNED_R TxnStateHolds
#define HOST_OWNED_R_DROP txn_state_Holds_drop
#define HOST_OWNED_KIND 1
#define HOST_OWNED_TABLE_FN txn_state_Holds_table
#define HOST_OWNED_WITH_TS Unit
#define HOST_OWNED_WITH_UNIT
#include "doltgres/host/owned_template.h"

/* Holds' Table's with at Find, for holds. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX txn_state_Holds
#define HOST_OWNED_RS TxnStateHolds
#define HOST_OWNED_R TxnStateHolds
#define HOST_OWNED_R_DROP txn_state_Holds_drop
#define HOST_OWNED_KIND 1
#define HOST_OWNED_TABLE_FN txn_state_Holds_table
#define HOST_OWNED_WITH_TS TxnStateFind
#define HOST_OWNED_WITH_T TxnStateFind
#include "doltgres/host/owned_template.h"

/* Holds' Table's with at Option<usize>, for held_to_end. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX txn_state_Holds
#define HOST_OWNED_RS TxnStateHolds
#define HOST_OWNED_R TxnStateHolds
#define HOST_OWNED_R_DROP txn_state_Holds_drop
#define HOST_OWNED_KIND 1
#define HOST_OWNED_TABLE_FN txn_state_Holds_table
#define HOST_OWNED_WITH_TS StdUsizeOption
#define HOST_OWNED_WITH_T StdUsizeOption
#include "doltgres/host/owned_template.h"

/* txn_state_current returns the backend's handle on its record of held states, answering false for none. */
static bool
txn_state_current(HostOwnedHandle *handle)
{
	HostLocalLocal current = host_local_BackendLocal_get(&TXN_STATE_CURRENT.local, &TXN_STATE_CURRENT_TYPE);
	StdAtomicU64 *words = (StdAtomicU64 *) host_local_Local_Deref_deref(&current, &TXN_STATE_CURRENT_TYPE);

	handle->generation = std_AtomicU64_load_relaxed(&words[1]);
	handle->slot = (uint32) std_AtomicU64_load_relaxed(&words[0]);
	host_local_Local_drop(&current, &TXN_STATE_CURRENT_TYPE);
	return handle->generation != 0;
}

/* txn_state_holding returns the handle of the backend's record of held states, made for its top-level transaction when it has none. */
static bool
txn_state_holding(HostIoIo *io, HostOwnedHandle *result, HostError *error)
{
	TxnStateHolds holds;
	HostLocalLocal current;
	StdAtomicU64 *words;

	if (txn_state_current(result))
		return true;
	std_vec_new(&holds.states, sizeof(txn_TupleU64U32HeldState_t), _Alignof(txn_TupleU64U32HeldState_t));
	if (!host_owned_TableTxnStateHolds_make_in_transaction(&TXN_STATE_HOLDS, io, holds, result, error))
		return false;
	current = host_local_BackendLocal_get(&TXN_STATE_CURRENT.local, &TXN_STATE_CURRENT_TYPE);
	words = (StdAtomicU64 *) host_local_Local_Deref_deref(&current, &TXN_STATE_CURRENT_TYPE);
	std_AtomicU64_store_relaxed(&words[0], result->slot);
	std_AtomicU64_store_relaxed(&words[1], result->generation);
	host_local_Local_drop(&current, &TXN_STATE_CURRENT_TYPE);
	return true;
}

/* txn_state_held_totals_unknown_call marks every held state's totals unknown. */
static bool
txn_state_held_totals_unknown_call(void *captures, HostIoIo *io, TxnStateHolds *holds, HostError *error)
{
	for (size_t k = 0, k_len = holds->states.len; k < k_len; k++)
		txn_state_totals_unknown(host_shared_Held_record(&STD_VEC_AT(&holds->states, txn_TupleU64U32HeldState_t,
																	 k).f2));
	return true;
}

/*
 * txn_state_held_totals_unknown marks the totals of every state the
 * backend's top-level transaction holds as unknown, for a prepare, whose
 * commit adds nothing, and for a transaction whose list overflowed, whose
 * keys aren't known.
 */
bool
txn_state_held_totals_unknown(HostIoIo *io, HostError *error)
{
	HostOwnedHandle handle;

	if (!txn_state_current(&handle))
		return true;
	return host_owned_TableTxnStateHolds_withUnit(&TXN_STATE_HOLDS, io, handle, txn_state_held_totals_unknown_call,
												  NULL, NULL, error);
}

/*
 * txn_state_ended ends the backend's record of held states, if it has one,
 * at its top-level transaction's commit or prepare, since a resource owner's
 * release at a commit warns of every record it still remembers.
 */
bool
txn_state_ended(HostIoIo *io, HostError *error)
{
	HostOwnedHandle handle;

	if (!txn_state_current(&handle))
		return true;
	return host_owned_TableTxnStateHolds_end(&TXN_STATE_HOLDS, io, handle, error);
}

/* txn_state_held_t is held's call's captures: the repository and its reader, the find it takes, and the caller's call, while live. */
typedef struct txn_state_held_t
{
	const RepoRepo *repo;
	const HostSharedReader *reading;
	TxnStateFound *found;
	bool		found_live;
	bool		(*call) (void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
						 HostError *error);
	void	   *call_captures;
	void		(*call_drop) (void *captures);
	bool		call_live;
} txn_state_held_t;

/* txn_state_held_drop drops held's call's captures: the find's make, then the caller's call, while live. */
static void
txn_state_held_drop(void *captures)
{
	txn_state_held_t *held = captures;

	if (held->found_live)
	{
		held->found_live = false;
		if (held->found->tag == TXN_STATE_FOUND_MADE)
			txn_base_Key_drop(&held->found->u.made.f1.key);
	}
	if (held->call_live)
	{
		held->call_live = false;
		if (held->call_drop != NULL)
			held->call_drop(held->call_captures);
	}
}

/*
 * txn_state_held_with is held's call under the backend's record: it finds
 * the held state's place, or makes the missing state and holds it, then
 * lends the state to the caller's call.
 */
static bool
txn_state_held_with(void *captures, HostIoIo *io, TxnStateHolds *holds, HostError *error)
{
	txn_state_held_t *held = captures;
	size_t		at;

	if (held->found->tag == TXN_STATE_FOUND_HELD)
	{
		held->found_live = false;
		at = held->found->u.held.f0;
	}
	else
	{
		const HostStorage *storage = &held->found->u.made.f0.f0;
		txn_TupleU64U32HeldState_t made = {txn_state_key(storage), storage->tablespace};

		if (!std_vec_try_reserve(&holds->states, 1, sizeof(made), _Alignof(txn_TupleU64U32HeldState_t)))
		{
			*error = host_Error_out_of_memory();
			txn_state_held_drop(held);
			return false;
		}
		held->found_live = false;
		if (!txn_state_state(io, held->repo, held->reading, storage, held->found->u.made.f0.f1, true,
							 &held->found->u.made.f1, &made.f2, error))
		{
			txn_state_held_drop(held);
			return false;
		}
		std_vec_push_Checked(&holds->states, &made, sizeof(made), _Alignof(txn_TupleU64U32HeldState_t));
		at = holds->states.len - 1;
	}
	if (at >= holds->states.len)
		txn_state_held_drop(held);
	std_index_check(at, holds->states.len, RUST_FILE, 1600, 25);
	held->call_live = false;
	return held->call(held->call_captures, io, held->repo,
					  &STD_VEC_AT(&holds->states, txn_TupleU64U32HeldState_t, at).f2, error);
}

/*
 * txn_state_held_call lends a call the state of a storage the backend's
 * top-level transaction holds, taking the find and the call: one the
 * transaction found before, or the one state finds or makes, filled, which
 * the transaction then holds until its end; the call gets the repository
 * too, for a reader of its base. It is held's body for every result type,
 * whose instances give the call their result through its captures.
 */
bool
txn_state_held_call(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, TxnStateFound *found,
					bool (*call) (void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								  HostError *error),
					void *call_captures, void (*call_drop) (void *captures), HostError *error)
{
	txn_state_held_t held = {.repo = repo, .reading = reading, .found = found, .found_live = true, .call = call,
	.call_captures = call_captures, .call_drop = call_drop, .call_live = true};
	HostOwnedHandle handle;

	if (!txn_state_holding(io, &handle, error))
	{
		txn_state_held_drop(&held);
		return false;
	}
	return host_owned_TableTxnStateHolds_withUnit(&TXN_STATE_HOLDS, io, handle, txn_state_held_with, &held,
												  txn_state_held_drop, error);
}

/* txn_state_holds_t is holds' call's captures: the storage, its persistence, and its key. */
typedef struct txn_state_holds_t
{
	const HostStorage *storage;
	TxnStatePersistence persistence;
	uint64		key;
} txn_state_holds_t;

/* txn_state_held_place returns where the backend's record holds a storage's state, answering false for nowhere. */
static bool
txn_state_held_place(const TxnStateHolds *holds, uint64 key, uint32 tablespace, size_t *result)
{
	for (size_t k = 0, k_len = holds->states.len; k < k_len; k++)
	{
		const txn_TupleU64U32HeldState_t *state = &STD_VEC_AT(&holds->states, txn_TupleU64U32HeldState_t, k);

		if (state->f0 == key && state->f1 == tablespace)
		{
			*result = k;
			return true;
		}
	}
	return false;
}

/* txn_state_holds_call finds the storage's place in the backend's record, or that it is missing. */
static bool
txn_state_holds_call(void *captures, HostIoIo *io, TxnStateHolds *holds, TxnStateFind *result, HostError *error)
{
	const txn_state_holds_t *finding = captures;
	size_t		at;

	if (txn_state_held_place(holds, finding->key, finding->storage->tablespace, &at))
	{
		result->tag = TXN_STATE_FIND_HELD;
		result->u.held.f0 = at;
	}
	else
	{
		result->tag = TXN_STATE_FIND_MISSING;
		result->u.missing.f0 = *finding->storage;
		result->u.missing.f1 = finding->persistence;
	}
	return true;
}

/*
 * txn_state_holds finds whether the backend's top-level transaction holds a
 * storage's state, so that a caller of held reads the catalog for the
 * state's make, before any lock and outside the record's lend, only when it
 * doesn't.
 */
bool
txn_state_holds(HostIoIo *io, const HostStorage *storage, TxnStatePersistence persistence, TxnStateFind *result,
				HostError *error)
{
	txn_state_holds_t finding = {storage, persistence, txn_state_key(storage)};
	HostOwnedHandle handle;

	if (!txn_state_holding(io, &handle, error))
		return false;
	return host_owned_TableTxnStateHolds_withTxnStateFind(&TXN_STATE_HOLDS, io, handle, txn_state_holds_call, &finding,
														  NULL, result, error);
}

/* txn_state_held_to_end_t is held_to_end's calls' captures: the storage's key and tablespace, and the state found. */
typedef struct txn_state_held_to_end_t
{
	uint64		key;
	uint32		tablespace;
	HostSharedHeld *held;
	bool		held_live;
} txn_state_held_to_end_t;

/* txn_state_held_to_end_drop drops the found state the second call didn't take. */
static void
txn_state_held_to_end_drop(void *captures)
{
	txn_state_held_to_end_t *finding = captures;

	if (finding->held_live)
	{
		finding->held_live = false;
		host_shared_Held_drop(finding->held);
	}
}

/* txn_state_held_to_end_found finds the storage's place in the backend's record. */
static bool
txn_state_held_to_end_found(void *captures, HostIoIo *io, TxnStateHolds *holds, StdUsizeOption *result,
							HostError *error)
{
	const txn_state_held_to_end_t *finding = captures;

	result->has = txn_state_held_place(holds, finding->key, finding->tablespace, &result->x);
	return true;
}

/* txn_state_held_to_end_taken takes the found state into the backend's record and gives its place. */
static bool
txn_state_held_to_end_taken(void *captures, HostIoIo *io, TxnStateHolds *holds, StdUsizeOption *result,
							HostError *error)
{
	txn_state_held_to_end_t *finding = captures;
	txn_TupleU64U32HeldState_t taken = {finding->key, finding->tablespace};

	if (!std_vec_try_reserve(&holds->states, 1, sizeof(taken), _Alignof(txn_TupleU64U32HeldState_t)))
	{
		*error = host_Error_out_of_memory();
		txn_state_held_to_end_drop(finding);
		return false;
	}
	finding->held_live = false;
	taken.f2 = *finding->held;
	std_vec_push_Checked(&holds->states, &taken, sizeof(taken), _Alignof(txn_TupleU64U32HeldState_t));
	result->has = true;
	result->x = holds->states.len - 1;
	return true;
}

/*
 * txn_state_held_to_end returns where the backend's top-level transaction
 * holds a storage's published state, taking one it doesn't hold, as a TOAST
 * storage's at its write, into its record of held states, so that a commit's
 * move of the state's totals has a place that lives to the transaction's
 * end; none when no state is published for the storage.
 */
bool
txn_state_held_to_end(HostIoIo *io, const HostStorage *storage, bool *some, TxnStatePlace *result,
					  HostError *error)
{
	HostSharedHeld held;
	txn_state_held_to_end_t finding = {txn_state_key(storage), storage->tablespace, &held};
	HostOwnedHandle handle;
	StdUsizeOption at;
	bool		found;

	*some = false;
	if (!txn_state_holding(io, &handle, error) ||
		!host_owned_TableTxnStateHolds_withStdUsizeOption(&TXN_STATE_HOLDS, io, handle, txn_state_held_to_end_found,
														  &finding, NULL, &at, error))
		return false;
	if (!at.has)
	{
		if (!txn_state_find(HOST_SHARED_IO_WITHIN(io), storage, &found, &held, error))
			return false;
		if (!found)
			return true;
		finding.held_live = true;
		if (!host_owned_TableTxnStateHolds_withStdUsizeOption(&TXN_STATE_HOLDS, io, handle,
															  txn_state_held_to_end_taken, &finding,
															  txn_state_held_to_end_drop, &at, error))
			return false;
	}
	*some = true;
	result->f0 = at.x;
	return true;
}

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/std/test.h"

/* txn_state_tests_read_back asserts that a blob unkept reads back as a base record or none, as unwrap and assert_eq do at their places. */
static void
txn_state_tests_read_back(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, const uint8 *blob,
						  size_t blob_len, bool has_expected, const TxnBaseBase *expected)
{
	TxnBaseBase back;
	bool		some;
	HostError	error;
	StdOptionRef left;
	StdOptionRef right = {has_expected, STD_ITEM(expected, txn_base_Base_Debug_fmt)};

	if (!txn_state_unkept(blob, blob_len, txn_state_internal, &some, &back, &error))
		STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = some;
	left.some = STD_ITEM(&back, txn_base_Base_Debug_fmt);
	STD_ASSERT_EQ(line, col, some == has_expected && (!some || txn_base_Base_PartialEq_eq(&back, expected)), &left,
				  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (some)
		txn_base_Base_drop(&back);
}

/* The fork's own: the largest base record, every field present at its width's largest, fits a state's blob and reads back whole, and an empty blob is a storage with no base. */
void
test_txn_state_tests_the_largest_base_record_fits_a_states_blob_and_reads_back(void)
{
	TxnBaseBase largest = txn_base_tests_full();
	TxnBaseBase empty = txn_base_tests_empty();
	StdVec		blob;
	StdVec		kept_empty;

	largest.sizes.entries = PG_UINT64_MAX;
	largest.sizes.leaf_bytes = PG_UINT64_MAX;
	largest.sizes.key_bytes = PG_UINT64_MAX;
	largest.sizes.heap_bytes = PG_UINT64_MAX;
	largest.sizes.rows = PG_UINT64_MAX;
	txn_state_kept(&largest, &blob);
	if (blob.len > TXN_STATE_BASE_BYTES)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("the largest record keeps "), STD_FMT_ARG(0),
		STD_FMT_LIT(" bytes")};
		StdFmtArg	args[1] = {{&blob.len, std_usize_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 1791, 9, &arguments);
	}
	txn_state_tests_read_back(1792, 9, 1792, 44, blob.ptr, blob.len, true, &largest);
	txn_state_kept(&empty, &kept_empty);
	txn_state_tests_read_back(1793, 9, 1793, 54, kept_empty.ptr, kept_empty.len, true, &empty);
	std_vec_free_Checked(&kept_empty, 1, 1);
	txn_state_tests_read_back(1794, 9, 1794, 42, NULL, 0, false, NULL);
	std_vec_free_Checked(&blob, 1, 1);
	txn_base_Base_drop(&largest);
	txn_base_Base_drop(&empty);
}
#endif
