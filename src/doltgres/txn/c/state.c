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
#include "doltgres/host/lock.h"
#include "doltgres/host/shared.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/foldhash.h"
#include "doltgres/std/hashmap.h"
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
#include "doltgres/txn/runs.h"
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

/*
 * TXN_STATE_BASE_END is BaseEnd, the latest end of the records that named the
 * bases installed in the state, which a save flushes the WAL through before
 * its manifest names the newest (bugs row 350).
 */
#define TXN_STATE_BASE_END (32 + TXN_FREE_SPACE_CATEGORIES)

/*
 * TXN_STATE_SPILL_SPACE is SpillSpace, the tablespace its spills' names
 * carry, fixed at its fill or by the spills a start loads, which no move of
 * its database changes (bugs row 438).
 */
#define TXN_STATE_SPILL_SPACE (33 + TXN_FREE_SPACE_CATEGORIES)

/*
 * TXN_STATE_MANIFEST_SPACE is ManifestSpace, the tablespace its spills'
 * manifest file is named by: its storage's key's, which a move's rehome
 * renames the file to (bugs row 438).
 */
#define TXN_STATE_MANIFEST_SPACE (34 + TXN_FREE_SPACE_CATEGORIES)

/*
 * TXN_STATE_REACHED_FROM is where the read of the slot's record starts, at or
 * before the record, which a reached point's fold reads back for the writers
 * the record decided (bugs row 432).
 */
#define TXN_STATE_REACHED_FROM (35 + TXN_FREE_SPACE_CATEGORIES)

/*
 * TXN_STATE_DROPPING is set from before a fold's install until its drop has
 * taken out what the install folded, so no save writes the state's delta
 * between the two (bugs row 432).
 */
#define TXN_STATE_DROPPING (36 + TXN_FREE_SPACE_CATEGORIES)

/*
 * TXN_STATE_WALKERS is Walkers, how many rows spills of the state are between
 * the start of their walk of the memtable and the end of their take-out
 * (bugs row 432).
 */
#define TXN_STATE_WALKERS (37 + TXN_FREE_SPACE_CATEGORIES)

/*
 * TXN_STATE_ROLLING is Rolling, how many folds of the state that read a
 * running top's rolled-back subtransactions' memtable rows, as VACUUM's
 * does, are between their first read and the end of their drop (bugs row
 * 432).
 */
#define TXN_STATE_ROLLING (38 + TXN_FREE_SPACE_CATEGORIES)

/* TXN_STATE_DROPPED_NAP_MICROS is how long a save naps between its looks at a fold's drop. */
#define TXN_STATE_DROPPED_NAP_MICROS 1000

/*
 * TXN_STATE_LOGGING is Logging's flag, which every change of the base
 * generation holds, so an install that logs its base's record checks the
 * generation, inserts the record with no lock held, and publishes the base
 * under that generation (bugs row 350).
 */
#define TXN_STATE_LOGGING 0

/* TXN_STATE_LOGGING_PAUSES names the pause inside a logging install's checkpoint delay. */
static const char *const TXN_STATE_LOGGING_PAUSES[] = {"doltgres-state-logging"};

/*
 * TXN_STATE_COMMITTING_INDEX is Committing, one past the id of the process
 * whose turn it is at spilling the state's committed versions, so no two
 * spill one key's versions at once, or zero.
 */
#define TXN_STATE_COMMITTING_INDEX (29 + TXN_FREE_SPACE_CATEGORIES)

/*
 * TXN_STATE_NUMBERED is Numbered, the highest number a spill file of an ended
 * transaction of the state took, by the committed arm or a fold's residual,
 * which take their numbers through it, so two of them never take one number.
 */
#define TXN_STATE_NUMBERED (30 + TXN_FREE_SPACE_CATEGORIES)

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

/*
 * txn_state_spill_space_filled fixes the tablespace a state's spill files'
 * names carry at its fill, and names its spills' manifest by the same, from
 * its storage's and its repository's own.
 */
static void
txn_state_spill_space_filled(HostSharedRecord record)
{
	uint32		tablespace = (uint32) txn_state_load(record, TXN_STATE_TABLESPACE);
	uint32		named = txn_runs_named_space(tablespace, (uint32) txn_state_load(record, TXN_STATE_OWN));

	txn_state_exchange(record, TXN_STATE_SPILL_SPACE, 0, named);
	txn_state_exchange(record, TXN_STATE_MANIFEST_SPACE, 0, named);
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

#ifdef USE_INJECTION_POINTS
/* txn_state_bases_decoded is how many base records the process has decoded, which tests count. */
uint64		txn_state_bases_decoded;
#endif

/* txn_state_unkept_body_0 is unkept's body past its length checks: the address table read, then the record decoded. */
static void
txn_state_unkept_body_0(void *frame_)
{
	txn_state_unkept_frame_0_t *frame = frame_;

#ifdef USE_INJECTION_POINTS
	txn_state_bases_decoded++;
#endif
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

/*
 * txn_state_moved_past reports whether an install moved a state's base
 * generation past the one a reader took its base at, loaded after every read
 * before it (bugs rows 240 and 241). An install moves the generation by an
 * atomic add, which has full barrier semantics, under the state's lock, and
 * its drop starts only after the install returns; the load follows a read
 * barrier, so a reader that read the delta and then finds the generation
 * unmoved read no version a newer base's drop took out.
 */
bool
txn_state_moved_past(HostSharedRecord record, uint64 generation)
{
	HostSharedOp op = {.tag = HOST_SHARED_OP_LOAD_ACQUIRE};

	return host_shared_Record_atomic(record, TXN_STATE_BASE_GENERATION, op) != generation;
}

/* TXN_STATE_INDEX_KEY is the length of a key of the states' pool's index: a storage's tablespace, database, and relation. */
#define TXN_STATE_INDEX_KEY ((size_t) 12)

/* txn_state_index_key writes the key the states' pool's index holds a storage's state under. */
static void
txn_state_index_key(const HostStorage *storage, uint8 result[TXN_STATE_INDEX_KEY])
{
	std_u32_to_be_bytes(storage->tablespace, result);
	std_u32_to_be_bytes(storage->database, result + 4);
	std_u32_to_be_bytes(storage->relation, result + 8);
}

/*
 * txn_state_published publishes a state made of a storage, under the makers'
 * lock, and answers its location, after the index holds that location for
 * the storage (bugs row 405): an open of the location answers nothing until
 * the publish, which changes the slot's state and not its generation, so
 * every lookup that finds the location reaches the state once it's
 * published, and no lookup misses a published state.
 */
static bool
txn_state_published(HostSharedGuard *making, HostSharedOwned *owned, const HostStorage *storage, uint64 *location,
					HostError *error)
{
	uint8		key[TXN_STATE_INDEX_KEY];

	txn_state_index_key(storage, key);
	*location = host_shared_Owned_location(owned);
	if (!host_shared_index_put(HOST_SHARED_GUARD_WITHIN(making), &TXN_STATE_STATE_POOL_OPS, key, *location, error))
		return false;
	(void) host_shared_Owned_publish(owned);
	return host_pause(HOST_SHARED_GUARD_PAUSES(making), "doltgres-state-published", error);
}

/*
 * txn_state_unindexed takes a storage's state at a location out of the index,
 * under the makers' lock, before the reclaim or the unlink that claimed it
 * frees its slot (bugs row 405), so the index names no slot the pool can give
 * to another record.
 */
static bool
txn_state_unindexed(HostSharedGuard *making, const HostStorage *storage, uint64 location, HostError *error)
{
	uint8		key[TXN_STATE_INDEX_KEY];

	txn_state_index_key(storage, key);
	return host_shared_index_remove(HOST_SHARED_GUARD_WITHIN(making), &TXN_STATE_STATE_POOL_OPS, key, location,
									error);
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
	HostSharedHeld held;
	bool		held_live;
} txn_state_find_frame_0_t;

/* txn_state_find_pad_0 is find's pad: the handle, while live. */
static void
txn_state_find_pad_0(void *frame_)
{
	txn_state_find_frame_0_t *frame = frame_;

	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
}

/*
 * txn_state_indexed opens the state the index holds a storage's location for
 * into the frame's handle, answering whether it is that storage's: the handle
 * keeps the record's slot until it drops, and a location names its slot's
 * generation, so the open reaches only the state put there while it stays
 * published, and its name is checked under the handle.
 */
static bool
txn_state_indexed(txn_state_find_frame_0_t *frame, const HostStorage *storage, bool *result)
{
	uint8		key[TXN_STATE_INDEX_KEY];
	uint64		location;
	bool		found;

	*result = false;
	txn_state_index_key(storage, key);
	if (!host_shared_index_find(frame->within, &TXN_STATE_STATE_POOL_OPS, key, &found, &location, frame->error))
		return false;
	if (!found)
		return true;
	if (!host_shared_open(frame->within, &TXN_STATE_STATE_POOL_OPS, location, &found, &frame->held, frame->error))
		return false;
	if (!found)
		return true;
	frame->held_live = true;
	*result = txn_state_named(host_shared_Held_record(&frame->held), storage);
	if (!*result)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	return true;
}

/* txn_state_find_body_0 is find's body. */
static void
txn_state_find_body_0(void *frame_)
{
	txn_state_find_frame_0_t *frame = frame_;
	HostStorage marked = txn_state_marker(frame->storage->database);
	bool		has;

	if (!txn_state_indexed(frame, &marked, &has))
	{
		txn_state_find_pad_0(frame);
		return;
	}
	if (has)
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
	if (!txn_state_indexed(frame, frame->storage, &has))
	{
		txn_state_find_pad_0(frame);
		return;
	}
	if (has)
	{
		frame->held_live = false;
		*frame->result = frame->held;
		*frame->some = true;
	}
	frame->ok = true;
	txn_state_find_pad_0(frame);
}

/*
 * txn_state_find returns a handle on a storage's published state, or
 * nothing when none is: it looks the storage up in the states' pool's index,
 * which holds exactly the published states no reclaim or unlink has taken
 * out (bugs row 405), opens the location, and compares the key and the
 * tablespace again under the handle. It refuses, as data corruption (XX001),
 * a storage of a database whose marker the index holds; only a start's
 * loader makes a marker, after it frees every state of the database and
 * before any process opens one, and no state of a marked database is made
 * after it.
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
	if (!txn_state_published(&frame->making, &frame->owned, frame->storage, &location, frame->error))
	{
		txn_state_opened_pad_0(frame);
		return;
	}
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

	/*
	 * unsaved is the repository and own tablespace of the first idle filled
	 * state the sweep found unsaved, which a reclaim that frees nothing
	 * saves.
	 */
	bool		has_unsaved;
	uint32		unsaved;
	uint32		unsaved_own;
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
 * the makers' lock: it answers false on an error, sets freed when it freed
 * the state, and keeps the first idle unsaved state's repository.
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
	if (!clean && !frame->has_unsaved)
	{
		HostStorage storage;
		TxnStatePersistence persistence;

		frame->has_unsaved = txn_state_described(record, &storage, &persistence);
		frame->unsaved = storage.database;
		frame->unsaved_own = txn_state_own(record);
	}
	if (clean && !memtable && !maps)
	{
		HostStorage storage;
		TxnStatePersistence persistence;

		(void) txn_state_described(record, &storage, &persistence);
		if (!txn_state_unindexed(&frame->making, &storage, location, frame->error))
			return false;
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
	bool		took = false;
	bool		cleaned = false;

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
	if (!undecided && frame->has_unsaved &&
		!txn_runs_save(frame->io, frame->unsaved, frame->unsaved_own, &took, &cleaned, frame->error))
	{
		txn_state_reclaim_pad_0(frame);
		return;
	}
	*frame->result = undecided || (frame->has_unsaved && (took || cleaned));
	frame->ok = true;
	txn_state_reclaim_pad_0(frame);
}

/*
 * txn_state_State_Pool_reclaim frees, under the makers' lock, a state that
 * is not in use and was never filled or whose record the newest manifest
 * holds, and answers whether the make should look again: when it freed one
 * or met one another reclaim was looking at. A state kept only by a memtable
 * that holds nothing and no pin names has it closed once the makers' lock is
 * released, and the make looks again. With nothing freed, it saves the
 * repository of an idle state no manifest holds, after releasing the makers'
 * lock, and the make looks again when the save turned a state clean or took
 * an identity version out (item 6's caller (2), bugs row 340).
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

/* TXN_STATE_STATE_POOL_OPS is State's Shape and Pool: two locks, a flag, its atomics, and two base records' blobs. */
const HostSharedPoolOps TXN_STATE_STATE_POOL_OPS = {
	.shape = {.id = UINT64CONST(0x74786e000002), .locks = 2, .flags = 1, .atomics = 39 + TXN_FREE_SPACE_CATEGORIES,
	.blobs = TXN_STATE_BLOBS, .blobs_len = lengthof(TXN_STATE_BLOBS)},
	.name = {"storage state", 13},
	.count = txn_state_State_Pool_count,
	.reclaim = txn_state_State_Pool_reclaim,
	.index = TXN_STATE_INDEX_KEY,
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

/* txn_state_Cut_t is an entry of CRASH's cuts: a storage, whether its first make took its entry, and its cut, when a run names one. */
typedef struct txn_state_Cut_t
{
	HostStorage f0;
	struct
	{
		bool		made;
		bool		has_cut;
		uint64		cut;
	}			f1;
} txn_state_Cut_t;

/* txn_state_Storage_hash is Storage's Hash, as CRASH's cuts key it. */
static void
txn_state_Storage_hash(const void *key, StdHasher state)
{
	const HostStorage *self = key;

	std_Hasher_write_u32(state, self->tablespace);
	std_Hasher_write_u32(state, self->database);
	std_Hasher_write_u32(state, self->relation);
}

/* txn_state_Storage_eq is Storage's PartialEq, as CRASH's cuts compare keys. */
static bool
txn_state_Storage_eq(const void *a, const void *b)
{
	return host_Storage_PartialEq_eq(a, b);
}

/* TXN_STATE_CUTS_MAP is the table of CRASH's cuts, a HashMap<Storage, Cut>. */
static const StdHashMapType TXN_STATE_CUTS_MAP = {
	STD_HASHMAP_SHAPE(txn_state_Cut_t),
	.key_hash = txn_state_Storage_hash,
	.key_eq = txn_state_Storage_eq,
	.hasher = &FOLDHASH_FAST_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = foldhash_fast_RandomState_Default_default,
};

/* txn_state_Crash_t is CRASH's value: each repository whose runs the start loaded with their checkpoint's redo point, and the storages' cuts. */
typedef struct txn_state_Crash_t
{
	/* redos is a Vec<(u32, u64)> of repositories and redo points. */
	StdVec		redos;
	StdHashMap	cuts;
} txn_state_Crash_t;

/* txn_state_Crash_drop is Crash's drop glue: its repositories and its cuts. */
static void
txn_state_Crash_drop(txn_state_Crash_t *crash)
{
	std_vec_free_Checked(&crash->redos, sizeof(std_TupleU32U64_t), _Alignof(std_TupleU32U64_t));
	std_HashMap_drop(&crash->cuts, &TXN_STATE_CUTS_MAP);
}

/* taken at Crash and IoMutex<Crash>, which state.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Crash
#define HOST_LOCK_TAKEN_T txn_state_Crash_t
#define HOST_LOCK_TAKEN_DEFAULT(place) \
	(std_vec_new(&(place)->redos, sizeof(std_TupleU32U64_t), _Alignof(std_TupleU32U64_t)), \
	 std_HashMap_Default_default(&(place)->cuts, &TXN_STATE_CUTS_MAP))
#define HOST_LOCK_TAKEN_DROP(place) txn_state_Crash_drop(place)
#define HOST_LOCK_TAKEN_TYPE TXN_STATE_CRASH_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Crash
#define HOST_LOCK_T txn_state_Crash_t
#define HOST_LOCK_DEFAULT(place) \
	(std_vec_new(&(place)->redos, sizeof(std_TupleU32U64_t), _Alignof(std_TupleU32U64_t)), \
	 std_HashMap_Default_default(&(place)->cuts, &TXN_STATE_CUTS_MAP))
#include "doltgres/host/lock_template.h"

/* txn_state_IoMutexCrash_drop is IoMutex<Crash>'s drop glue: its value's. */
static void
txn_state_IoMutexCrash_drop(void *value)
{
	txn_state_Crash_drop(&((HostLockIoMutexCrash *) value)->value);
}

/* TXN_STATE_CRASH_TYPE is IoMutex<Crash>'s size, Default, and drop. */
static const StdDefaultType TXN_STATE_CRASH_TYPE = {sizeof(HostLockIoMutexCrash), _Alignof(HostLockIoMutexCrash),
host_lock_IoMutexCrash_Default_default, txn_state_IoMutexCrash_drop};

/*
 * TXN_STATE_CRASH is CRASH: what the startup process's load of a
 * checkpoint's runs tells its makes (bugs row 408), each repository's redo
 * point and each storage's cut, its BackendLocal header and the process's
 * value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexCrash process;
}			TXN_STATE_CRASH = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * txn_state_crashed runs a step on CRASH under its lock, the process's
 * local and guard dropped after it.
 */
static void
txn_state_crashed(HostIoIo *io, void (*step) (void *captures, txn_state_Crash_t *crash), void *captures)
{
	HostLocalLocal local = host_local_BackendLocal_get(&TXN_STATE_CRASH.local, &TXN_STATE_CRASH_TYPE);
	HostLockLockedRwLockWriteGuard guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&local,
																												   &TXN_STATE_CRASH_TYPE),
																  &TXN_STATE_CRASH_LOCK_TYPE, &io->held);

	step(captures, host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&guard));
	host_lock_LockedRwLockWriteGuard_drop(&guard);
	host_local_Local_drop(&local, &TXN_STATE_CRASH_TYPE);
}

/* txn_state_crash_redo_step is crash_redo's step: the repository and its redo point stored. */
static void
txn_state_crash_redo_step(void *captures, txn_state_Crash_t *crash)
{
	std_vec_push_Checked(&crash->redos, captures, sizeof(std_TupleU32U64_t), _Alignof(std_TupleU32U64_t));
}

/*
 * txn_state_crash_redo stores in CRASH a repository and the redo point of the
 * checkpoint whose runs of it the startup process loads (bugs row 408).
 */
void
txn_state_crash_redo(HostIoIo *io, uint32 repository, HostLsn redo)
{
	std_TupleU32U64_t captures = {repository, redo};

	txn_state_crashed(io, txn_state_crash_redo_step, &captures);
}

/* txn_state_crash_cut_t is crash_cut's step's captures: the storage and its cut. */
typedef struct txn_state_crash_cut_t
{
	HostStorage storage;
	uint64		cut;
} txn_state_crash_cut_t;

/* txn_state_crash_cut_step is crash_cut's step: the cut stored, unless the storage's make took its entry already. */
static void
txn_state_crash_cut_step(void *captures, txn_state_Crash_t *crash)
{
	txn_state_crash_cut_t *c = captures;
	txn_state_Cut_t *entry = std_HashMap_get(&crash->cuts, &TXN_STATE_CUTS_MAP, &c->storage);
	txn_state_Cut_t value = {.f1 = {.has_cut = true, .cut = c->cut}};
	txn_state_Cut_t old;

	if (entry == NULL)
		(void) std_HashMap_insert(&crash->cuts, &TXN_STATE_CUTS_MAP, &c->storage, &value.f1, &old.f1);
}

/*
 * txn_state_crash_cut stores in CRASH a storage's cut, its tuple id counter
 * at the checkpoint whose runs the startup process loads, as a free run's
 * header or the cuts run names it (bugs row 408).
 */
void
txn_state_crash_cut(HostIoIo *io, const HostStorage *storage, uint64 cut)
{
	txn_state_crash_cut_t captures = {*storage, cut};

	txn_state_crashed(io, txn_state_crash_cut_step, &captures);
}

/* txn_state_crash_counter_t is crash_counter's step's captures: the storage, its base's fold point, and the counter. */
typedef struct txn_state_crash_counter_t
{
	HostStorage storage;
	HostLsn		folded;
	uint64	   *counter;
} txn_state_crash_counter_t;

/*
 * txn_state_crash_counter_step is crash_counter's step: at the storage's
 * first make, a base past its repository's redo point starts the counter at
 * the cut, or empty with none, and the entry is taken either way.
 */
static void
txn_state_crash_counter_step(void *captures, txn_state_Crash_t *crash)
{
	txn_state_crash_counter_t *c = captures;
	txn_state_Cut_t *entry = std_HashMap_get(&crash->cuts, &TXN_STATE_CUTS_MAP, &c->storage);
	txn_state_Cut_t made = {.f1 = {.made = true}};
	txn_state_Cut_t old;
	const std_TupleU32U64_t *redo = NULL;

	for (size_t k = 0; k < crash->redos.len && redo == NULL; k++)
		if (STD_VEC_AT(&crash->redos, std_TupleU32U64_t, k).f0 == c->storage.database)
			redo = &STD_VEC_AT(&crash->redos, std_TupleU32U64_t, k);
	if (redo == NULL || (entry != NULL && entry->f1.made))
		return;
	if (c->folded > redo->f1)
		*c->counter = entry != NULL ? entry->f1.cut : 1;
	if (entry != NULL)
		entry->f1.made = true;
	else
		(void) std_HashMap_insert(&crash->cuts, &TXN_STATE_CUTS_MAP, &c->storage, &made.f1, &old.f1);
}

/*
 * txn_state_crash_counter sets a make's counter for a crash start (bugs row
 * 408): a storage whose base's fold point is past the redo point of the
 * checkpoint whose runs of its repository the startup process loaded starts
 * at its cut there,
 * or empty when no run names one, since redo replays every row the base
 * holds past that point; each storage's first make in the process takes its
 * entry, so a later make keeps the base's fresh end.
 */
static void
txn_state_crash_counter(HostIoIo *io, const HostStorage *storage, HostLsn folded, uint64 *counter)
{
	txn_state_crash_counter_t captures = {*storage, folded, counter};

	txn_state_crashed(io, txn_state_crash_counter_step, &captures);
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
		txn_state_spill_space_filled(record);
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
			counter = frame->base.fresh_end;
			sizes = frame->base.sizes;
			txn_state_crash_counter(frame->io, frame->storage, frame->base.folded, &counter);
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

/*
 * txn_state_base_record_frame_0_t is base_record's state across its pad: the
 * base generation read with the base, when asked for, and the state's shared
 * guard and its blob, while live.
 */
typedef struct txn_state_base_record_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord record;
	bool	   *some;
	TxnBaseBase *result;
	uint64	   *generation;
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

	if (!host_shared_Record_lock_shared(frame->record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_STATE_LOCK_INDEX,
										&frame->guard, frame->error))
		return;
	frame->guard_live = true;
	frame->blob = host_shared_Guard_read_blob(&frame->guard, 0);
	frame->blob_live = true;
	if (frame->generation != NULL)
		*frame->generation = txn_state_generation(frame->record);
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
	txn_state_base_record_frame_0_t frame = {.io = io, .record = host_shared_Held_record(held), .some = some,
	.result = result, .error = error};

	std_landing(&frame, txn_state_base_record_body_0, txn_state_base_record_pad_0);
	return frame.ok;
}

/* txn_state_read_point returns the read point of a state's newest base, or 0 for a state with none, read under the state's lock. */
bool
txn_state_read_point(HostIoIo *io, HostSharedRecord record, uint64 *result, HostError *error)
{
	bool		some;
	TxnBaseBase newest;
	txn_state_base_record_frame_0_t frame = {.io = io, .record = record, .some = &some, .result = &newest,
	.error = error};

	std_landing(&frame, txn_state_base_record_body_0, txn_state_base_record_pad_0);
	if (!frame.ok)
		return false;
	*result = some ? newest.read : 0;
	if (some)
		txn_base_Base_drop(&newest);
	return true;
}

/* txn_state_committed_taken takes a process's turn at spilling a state's committed versions, answering whether it took it. */
bool
txn_state_committed_taken(HostSharedRecord record, uint64 id)
{
	return txn_state_exchange(record, TXN_STATE_COMMITTING_INDEX, 0, id + 1) == 0;
}

/* txn_state_committed_given gives a process's turn at spilling a state's committed versions back. */
void
txn_state_committed_given(HostSharedRecord record, uint64 id)
{
	txn_state_exchange(record, TXN_STATE_COMMITTING_INDEX, id + 1, 0);
}

/*
 * txn_state_numbered takes the number of a spill file of an ended transaction
 * of a state: past past, one past the transaction's files of the kind listed
 * or pended, and past every number such a file of the state took before.
 */
uint32
txn_state_numbered(HostSharedRecord record, uint32 past)
{
	uint64		taken = txn_state_load(record, TXN_STATE_NUMBERED);

	for (;;)
	{
		uint64		number = Max(taken + 1, (uint64) past);
		uint64		seen = txn_state_exchange(record, TXN_STATE_NUMBERED, taken, number);

		if (seen == taken)
			return (uint32) number;
		taken = seen;
	}
}

/* txn_state_delta_bytes returns the encoded bytes of every version in a state's delta. */
uint64
txn_state_delta_bytes(HostSharedRecord record)
{
	return txn_state_load(record, TXN_STATE_DELTA_BYTES);
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
 * counted when the count answered sizes. A count that a commit's move of the
 * totals met leaves them unknown, and one a fold's install met counts again
 * (bugs rows 240 and 316).
 */
static bool
txn_state_recounted_counting(txn_state_recounted_frame_0_t *frame, HostSharedRecord record,
							 HostSharedPlaceCell *cell, bool *counted)
{
	uint64		moved;
	uint64		generation;
	bool		ok;
	TxnBaseSizes sizes;

	host_shared_PlaceCell_set(cell, true, host_shared_Held_place(frame->held));
	moved = txn_state_load(record, TXN_STATE_TOTALS_MOVED);
	generation = txn_state_generation(record);
	ok = host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-state-recounting", frame->error);
	frame->count_live = false;
	if (!ok && frame->count_drop != NULL)
		frame->count_drop(frame->count_captures);
	while (ok)
	{
		ok = frame->count(frame->count_captures, frame->io, counted, &sizes, frame->error);
		if (!ok || !*counted)
			break;
		txn_state_total_stored(record, 0, sizes.entries);
		txn_state_total_stored(record, 1, sizes.leaf_bytes);
		txn_state_total_stored(record, 2, sizes.key_bytes);
		txn_state_total_stored(record, 3, sizes.heap_bytes);
		txn_state_total_stored(record, 4, sizes.rows);
		txn_state_exchange(record, TXN_STATE_TOTALS_KNOWN, 0, 1);
		if (txn_state_load(record, TXN_STATE_TOTALS_MOVED) != moved)
			txn_state_totals_unknown(record);
		if (!txn_state_moved_past(record, generation))
			break;
		txn_state_totals_unknown(record);
		moved = txn_state_load(record, TXN_STATE_TOTALS_MOVED);
		generation = txn_state_generation(record);
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

/* txn_state_dropping marks a state's fold as between its install and the end of its drop, or clears the mark. */
void
txn_state_dropping(HostSharedRecord record, bool dropping)
{
	txn_state_stored(record, TXN_STATE_DROPPING, dropping);
}

/*
 * txn_state_walked counts a rows spill of a state into its walkers, or out,
 * with an add that is a full barrier, so a fold that marks its rolling and
 * then loads the walkers and a spill that counts itself in and then loads
 * the mark never both miss the other (bugs row 432).
 */
void
txn_state_walked(HostSharedRecord record, bool walking)
{
	(void) txn_state_add(record, TXN_STATE_WALKERS, walking ? 1 : PG_UINT64_MAX);
}

/* txn_state_walkers returns how many rows spills of a state are between their walk's start and their take-out's end. */
uint64
txn_state_walkers(HostSharedRecord record)
{
	return txn_state_load(record, TXN_STATE_WALKERS);
}

/*
 * txn_state_rolled counts a fold of a state that reads a running top's
 * rolled-back subtransactions' memtable rows into its rolling, or out, with
 * an add that is a full barrier, pairing with walked's.
 */
void
txn_state_rolled(HostSharedRecord record, bool rolling)
{
	(void) txn_state_add(record, TXN_STATE_ROLLING, rolling ? 1 : PG_UINT64_MAX);
}

/* txn_state_rolling reports whether a fold of a state reads a running top's rolled-back subtransactions' memtable rows. */
bool
txn_state_rolling(HostSharedRecord record)
{
	return txn_state_load(record, TXN_STATE_ROLLING) != 0;
}

/*
 * txn_state_dropped waits until no fold of a state is between its install
 * and the end of its drop, which a save waits for before it reads the
 * state's delta, holding no lock nor any of the delta's holds meanwhile.
 */
bool
txn_state_dropped(HostIoIo *io, HostSharedRecord record, HostError *error)
{
	while (txn_state_load(record, TXN_STATE_DROPPING) != 0)
		if (!host_nap(HOST_IO_IO_IO_DOORS(io), std_Duration_from_micros(TXN_STATE_DROPPED_NAP_MICROS), error))
			return false;
	return true;
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

/* txn_state_reach_frame_0_t is reach's frame: the slot reads' frame, the base to keep, its record's start and end, and its answer. */
typedef struct txn_state_reach_frame_0_t
{
	txn_state_slot_frame_0_t slot;
	const TxnBaseBase *base;
	uint64		start;
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
			txn_state_stored(record, TXN_STATE_REACHED_FROM, frame->start);
			txn_state_stored(record, TXN_STATE_REACHED_AT, frame->end);
			*frame->result = true;
		}
	}
	txn_state_reach_pad_0(frame);
}

/*
 * txn_state_reach keeps the base a replayed fold's record named, whose packs
 * this server didn't hold, so its own fold reaches that point, and the
 * record's start and end, and answers whether it did: a standby folds every point its
 * primary folded, in order, since the tuple id, free space, and pending
 * trees depend on which points were folded, so the slot keeps a record only
 * while it holds no point its fold hasn't reached and no earlier record
 * waits for the read past it; any other record is left to that read.
 */
bool
txn_state_reach(HostIoIo *io, const HostSharedHeld *held, const TxnBaseBase *base, uint64 start, uint64 end,
				bool *result, HostError *error)
{
	txn_state_reach_frame_0_t frame = {.slot = {.io = io, .held = held, .error = error}, .base = base,
	.start = start, .end = end, .result = result};

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

/* txn_state_read_past_frame_0_t is read_past's frame: the slot reads' frame, how far the read got, the next record it found and where its read started, and its answer. */
typedef struct txn_state_read_past_frame_0_t
{
	txn_state_slot_frame_0_t slot;
	uint64		to;
	bool		has_next;
	const TxnBaseBase *next_base;
	uint64		next_from;
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
		{
			txn_state_stored(record, TXN_STATE_REACHED_FROM, frame->next_from);
			txn_state_stored(record, TXN_STATE_REACHED_AT, frame->next_end);
		}
	}
	else if (txn_state_load(record, TXN_STATE_DEFERRED_TO) <= frame->to)
	{
		txn_state_stored(record, TXN_STATE_DEFERRED_TO, 0);
		txn_state_stored(record, TXN_STATE_REACHED_FROM, 0);
		txn_state_stored(record, TXN_STATE_REACHED_AT, 0);
		txn_state_stored(record, TXN_STATE_REACHED_SINCE, 0);
		*frame->result = true;
	}
	txn_state_read_past_pad_0(frame);
}

/*
 * txn_state_read_past records how far the read past a state's slot's record
 * got: the slot takes the next fold's record it found, with that record's
 * end and where its read started, or, with none, the read is done once no
 * later record was deferred since it began, which clears the words; it
 * answers whether the read is done.
 */
bool
txn_state_read_past(HostIoIo *io, const HostSharedHeld *held, uint64 to, bool has_next,
					const TxnBaseBase *next_base, uint64 next_from, uint64 next_end, bool *result,
					HostError *error)
{
	txn_state_read_past_frame_0_t frame = {.slot = {.io = io, .held = held, .error = error}, .to = to,
	.has_next = has_next, .next_base = next_base, .next_from = next_from, .next_end = next_end,
	.result = result};

	std_landing(&frame, txn_state_read_past_body_0, txn_state_read_past_pad_0);
	return frame.slot.ok;
}

/* txn_state_behind_frame_0_t is behind's frame: the slot reads' frame, its answer, and where its record's read starts and ends. */
typedef struct txn_state_behind_frame_0_t
{
	txn_state_slot_frame_0_t slot;
	bool	   *some;
	TxnBaseBase *result;
	uint64	   *from;
	uint64	   *end;
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
		*frame->from = txn_state_load(host_shared_Held_record(frame->slot.held), TXN_STATE_REACHED_FROM);
		*frame->end = txn_state_load(host_shared_Held_record(frame->slot.held), TXN_STATE_REACHED_AT);
		*frame->some = true;
	}
	txn_state_behind_pad_0(frame);
}

/*
 * txn_state_behind returns the base a replayed fold's record named past a
 * filled state's newest, with where that record's read starts and its end,
 * read with the newest under the state's lock, or nothing when no record
 * reached past it; a state redo made with no base is
 * behind any record, since its storage was made after the base backup its
 * server started from.
 */
bool
txn_state_behind(HostIoIo *io, const HostSharedHeld *held, bool *some, TxnBaseBase *result, uint64 *from,
				 uint64 *end, HostError *error)
{
	txn_state_behind_frame_0_t frame = {.slot = {.io = io, .held = held, .error = error}, .some = some,
	.result = result, .from = from, .end = end};

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
	uint64		location;

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
	frame->ok = txn_state_published(&frame->making, &frame->owned, &marked, &location, frame->error);
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
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-state-found", error))
		return false;
	return txn_state_find(HOST_SHARED_IO_WITHIN(io), storage, some, result, error);
}

/*
 * txn_state_install_frame_0_t is the state of rechained and installed across
 * their pad: the logging flag, the logged record's end and checkpoint delay,
 * the state's guard, its newest blob, the base it held, the base kept with
 * its key, and the new blob, while live.
 */
typedef struct txn_state_install_frame_0_t
{
	HostIoTakesShort io;
	const HostSharedHeld *held;
	uint64		read;
	const TxnBaseBase *base;
	TxnStateLogged logged;
	void	   *logged_captures;
	bool	   *result;
	HostError  *error;
	bool		ok;
	HostSharedFlagGuard logging;
	bool		logging_live;
	HostLsn		end;
	HostDelayCheckpoint window;
	bool		window_live;
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

/*
 * txn_state_install_pad_0 is the installs' pad: the new blob, the bases, the
 * blob, the guard, the checkpoint delay, whose drop unfinished stops the
 * server, and the flag, while live.
 */
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
	if (frame->window_live)
	{
		frame->window_live = false;
		host_DelayCheckpoint_drop(&frame->window);
	}
	if (frame->logging_live)
	{
		frame->logging_live = false;
		host_shared_FlagGuard_drop(&frame->logging);
	}
}

/*
 * txn_state_install_logged inserts a current logging install's record, with
 * the state's lock released and taken again: outside recovery inside a
 * checkpoint delay, which the install's write finishes, so an error before
 * the base is published stops the server and recovery installs it from the
 * record. The flag holds the generation meanwhile.
 */
static bool
txn_state_install_logged(txn_state_install_frame_0_t *frame)
{
	HostSharedRecord record = host_shared_Held_record(frame->held);
	HostLsn		reached = txn_state_load(record, TXN_STATE_REACHED_AT);
	HostSharedFlagIo logging = host_shared_FlagGuard_io(&frame->logging);
	HostIoIoDoors doors = HOST_SHARED_FLAG_IO_IO_DOORS(&logging);

	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	if (!host_wal_stand(doors).recovering)
	{
		if (!host_delay_checkpoint(doors, TXN_STATE_LOGGING_PAUSES, lengthof(TXN_STATE_LOGGING_PAUSES),
								   &frame->window, frame->error))
			return false;
		frame->window_live = true;
	}
	if (!frame->logged(frame->logged_captures, doors, reached, &frame->end, frame->error) ||
		!host_pause(HOST_SHARED_FLAG_IO_PAUSES(&logging), "doltgres-state-logging", frame->error) ||
		!host_shared_Record_lock(record, frame->io, TXN_STATE_STATE_LOCK_INDEX, &frame->guard, frame->error))
		return false;
	frame->guard_live = true;
	return true;
}

/*
 * txn_state_install_locked takes the state's logging flag, then an install's
 * state's lock, and compares its base generation with the one the caller
 * read, answering false on an error and setting current when the generation
 * is still the one read; a current logging install then logs its record.
 */
static bool
txn_state_install_locked(txn_state_install_frame_0_t *frame, bool *current)
{
	HostSharedRecord record = host_shared_Held_record(frame->held);

	if (!host_shared_Record_flag(record, frame->io, TXN_STATE_LOGGING, &frame->logging, frame->error))
		return false;
	frame->logging_live = true;
	if (!host_shared_Record_lock(record, frame->io, TXN_STATE_STATE_LOCK_INDEX, &frame->guard, frame->error))
		return false;
	frame->guard_live = true;
	*current = txn_state_load(record, TXN_STATE_BASE_GENERATION) == frame->read;
	*frame->result = false;
	return !*current || frame->logged == NULL || txn_state_install_logged(frame);
}

/*
 * txn_state_install_written writes an install's new blob as the state's
 * newest base, moving the generation, raising the state's base end to its
 * record's, marking the state unsaved, and finishing a logging install's
 * checkpoint delay, answering false on an error.
 */
static bool
txn_state_install_written(txn_state_install_frame_0_t *frame)
{
	HostSharedRecord record = host_shared_Held_record(frame->held);

	if (!host_shared_GuardExclusive_write_blob(&frame->guard, 0, frame->bytes.ptr, frame->bytes.len, frame->error))
		return false;
	txn_state_add(record, TXN_STATE_BASE_GENERATION, 1);
	if (frame->end > txn_state_load(record, TXN_STATE_BASE_END))
		txn_state_stored(record, TXN_STATE_BASE_END, frame->end);
	txn_state_unsaved(record);
	if (frame->window_live)
	{
		frame->window_live = false;
		host_DelayCheckpoint_finish(&frame->window);
	}
	*frame->result = true;
	return true;
}

/*
 * txn_state_install_kept encodes an install's new base as its blob, read
 * against the state's newest under the lock: the new base takes the key of
 * the newest it replaces when it doesn't know it, since a backend records a
 * state's key in its newest base without moving the generation, after the
 * install's caller read it, answering false on an error.
 */
static bool
txn_state_install_kept(txn_state_install_frame_0_t *frame)
{
	bool		some;
	bool		keyed;

	frame->blob = host_shared_Guard_read_blob(&frame->guard, 0);
	frame->blob_live = true;
	if (!txn_state_unkept(frame->blob.ptr, frame->blob.len, txn_state_internal, &some, &frame->replaced,
						  frame->error))
		return false;
	frame->replaced_live = some;
	txn_state_kept(frame->base, &frame->bytes);
	frame->bytes_live = true;
	if (!some || frame->base->key.tag != TXN_BASE_KEY_UNKNOWN || frame->replaced.key.tag == TXN_BASE_KEY_UNKNOWN)
		return true;
	if (!txn_state_unkept(frame->bytes.ptr, frame->bytes.len, txn_state_internal, &keyed, &frame->keyed,
						  frame->error))
		return false;
	if (!keyed)
	{
		*frame->error = txn_state_internal("an install's base record didn't keep",
										   strlen("an install's base record didn't keep"));
		return false;
	}
	frame->keyed_live = true;
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	txn_base_Key_drop(&frame->keyed.key);
	frame->keyed.key = frame->replaced.key;
	frame->replaced.key.tag = TXN_BASE_KEY_UNKNOWN;
	txn_state_kept(&frame->keyed, &frame->bytes);
	frame->bytes_live = true;
	return true;
}

/* txn_state_install_body_0 is rechained's and installed's body. */
static void
txn_state_install_body_0(void *frame_)
{
	txn_state_install_frame_0_t *frame = frame_;
	bool		current;

	if (!txn_state_install_locked(frame, &current))
	{
		if (frame->window_live)
			host_DelayCheckpoint_failed(&frame->window, frame->error);
		txn_state_install_pad_0(frame);
		return;
	}
	frame->ok = !current || (txn_state_install_kept(frame) && txn_state_install_written(frame));
	if (frame->window_live && !frame->ok)
		host_DelayCheckpoint_failed(&frame->window, frame->error);
	txn_state_install_pad_0(frame);
}

/*
 * txn_state_rechained replaces the state's newest base record with one that
 * differs only in its chain of pending transforms, keeping any predecessor,
 * under the state's lock when the base generation is still the one the
 * caller read, moving the generation and marking the state unsaved; it
 * answers whether it replaced. It takes a key recorded since the caller's
 * read, as installed does. With logged, it inserts the new base's record
 * first, as install_logged does.
 */
bool
txn_state_rechained(HostIoTakesShort io, const HostSharedHeld *held, uint64 read, const TxnBaseBase *base,
					TxnStateLogged logged, void *logged_captures, bool *result, HostError *error)
{
	txn_state_install_frame_0_t frame = {.io = io, .held = held, .read = read, .base = base, .logged = logged,
	.logged_captures = logged_captures, .result = result, .error = error};

	std_landing(&frame, txn_state_install_body_0, txn_state_install_pad_0);
	return frame.ok;
}

/*
 * txn_state_installed makes a fold's base the state's newest, the newest it
 * replaces dropped, since every snapshot reads the newest, under the state's
 * lock when the base generation is still the one the fold read before it
 * built, moving the generation and marking the state unsaved so the next
 * save writes the new record; it answers whether it installed. The installed
 * base takes the key of the newest it replaces when the fold's base doesn't
 * know it, since a backend records a state's key in its newest base without
 * moving the generation, after the fold read it. With logged, it inserts the
 * new base's record first, as install_logged does.
 */
bool
txn_state_installed(HostIoTakesShort io, const HostSharedHeld *held, uint64 read, const TxnBaseBase *base,
					TxnStateLogged logged, void *logged_captures, bool *result, HostError *error)
{
	txn_state_install_frame_0_t frame = {.io = io, .held = held, .read = read, .base = base, .logged = logged,
	.logged_captures = logged_captures, .result = result, .error = error};

	std_landing(&frame, txn_state_install_body_0, txn_state_install_pad_0);
	return frame.ok;
}

/*
 * txn_state_raised raises a state's tuple id counter to at least a tuple id,
 * as Doltgrust's replayed tuple does, and marks the state not clean, so the
 * next save cuts past it (bugs row 489).
 */
void
txn_state_raised(HostSharedRecord record, uint64 counter)
{
	for (;;)
	{
		uint64		now = txn_state_load(record, TXN_STATE_COUNTER);

		if (now >= counter || txn_state_exchange(record, TXN_STATE_COUNTER, now, counter) == now)
			break;
	}
	txn_state_unsaved(record);
}

/*
 * txn_state_grown raises a state a backend made empty to a base built whole
 * for it and just installed: its tuple id counter to the base's fresh end,
 * so no insert takes a tuple id the base holds, and its totals by the base's
 * sizes, which no write counted.
 */
void
txn_state_grown(HostSharedRecord record, const TxnBaseBase *base)
{
	txn_state_raised(record, base->fresh_end);
	txn_state_add(record, TXN_STATE_TOTAL(0), base->sizes.entries);
	txn_state_add(record, TXN_STATE_TOTAL(1), base->sizes.leaf_bytes);
	txn_state_add(record, TXN_STATE_TOTAL(2), base->sizes.key_bytes);
	txn_state_add(record, TXN_STATE_TOTAL(3), base->sizes.heap_bytes);
	txn_state_add(record, TXN_STATE_TOTAL(4), base->sizes.rows);
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
 * txn_state_checked refuses, as an internal error, a base an MVCC or historic
 * snapshot whose xmin doesn't pass its maxFolded would read.
 */
static bool
txn_state_checked(const HostSnapshot *snapshot, bool *some, TxnBaseBase *result, HostError *error)
{
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
	return txn_state_newest(io, held, some, result, error) && txn_state_checked(snapshot, some, result, error);
}

/*
 * txn_state_readable_since answers whether an install moved a filled state's
 * base generation past the one a reader took its base at (bugs rows 240 and
 * 241), costing one load when it hasn't, and when it has, returns the newest
 * base for a snapshot to read, as readable returns it, with the generation
 * it read it at, both read under the state's lock, under which an install
 * moves both.
 */
bool
txn_state_readable_since(HostIoIo *io, HostSharedRecord record, const HostSnapshot *snapshot, uint64 generation,
						 bool *moved, bool *some, TxnBaseBase *result, uint64 *now, HostError *error)
{
	txn_state_base_record_frame_0_t frame = {.io = io, .record = record, .some = some, .result = result,
	.generation = now, .error = error};

	*moved = txn_state_generation(record) != generation;
	*some = false;
	if (!*moved)
		return true;
	std_landing(&frame, txn_state_base_record_body_0, txn_state_base_record_pad_0);
	return frame.ok && txn_state_checked(snapshot, some, result, error);
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
		txn_state_exchange(record, TXN_STATE_COUNTER, 0, frame->empty.fresh_end);
		txn_state_exchange(record, TXN_STATE_UNLOGGED, 0, (uint64) TXN_STATE_PERSISTENCE_UNLOGGED);
		txn_state_exchange(record, TXN_STATE_OWN, 0, (uint64) repo_Repo_own(frame->repo));
		txn_state_spill_space_filled(record);
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
		const TxnMemtableVersionVersion *version = &STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, k);
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
		TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);
		HostXid		xid = {writer.has_top ? writer.top : writer.xid};
		bool		has;
		uint64		added;

		if (!txn_memtable_version_Version_counted(version))
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
			txn_state_stored(record, TXN_STATE_COUNTER, frame->base.fresh_end);
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
	HostSharedFlagGuard logging;
	bool		logging_live;
	HostDelayCheckpoint window;
	bool		window_live;
	StdVec		body;
	bool		body_live;
} txn_state_emptied_frame_0_t;

/*
 * txn_state_emptied_pad_0 is emptied's and redo_emptied's pad: the body, the
 * window, then the logging flag, while live.
 */
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
	if (frame->logging_live)
	{
		frame->logging_live = false;
		host_shared_FlagGuard_drop(&frame->logging);
	}
}

/*
 * txn_state_emptied_flagged takes the state's logging flag, which clearing a
 * storage holds as an install does, since it moves the base generation.
 */
static bool
txn_state_emptied_flagged(txn_state_emptied_frame_0_t *frame)
{
	if (!host_shared_Record_flag(host_shared_Held_record(frame->held), HOST_IO_IO_TAKES_SHORT(frame->io),
								 TXN_STATE_LOGGING, &frame->logging, frame->error))
		return false;
	frame->logging_live = true;
	return true;
}

/* txn_state_emptied_body_0 is emptied's body. */
static void
txn_state_emptied_body_0(void *frame_)
{
	txn_state_emptied_frame_0_t *frame = frame_;
	TxnRecordStorageEmptied emptied;
	TxnStatePersistence persistence;

	if (!txn_state_emptied_flagged(frame) || !txn_state_uncounted(frame->io, frame->held, frame->error) ||
		!host_delay_checkpoint(HOST_IO_IO_IO_DOORS(frame->io), TXN_STATE_CLEARING, lengthof(TXN_STATE_CLEARING),
							   &frame->window, frame->error))
	{
		txn_state_emptied_pad_0(frame);
		return;
	}
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
			host_DelayCheckpoint_failed(&frame->window, frame->error);
			txn_state_emptied_pad_0(frame);
			return;
		}
		frame->body_live = false;
		std_vec_free_Checked(&frame->body, 1, 1);
	}
	if (!txn_state_cleared(frame->io, frame->held, frame->error))
	{
		host_DelayCheckpoint_failed(&frame->window, frame->error);
		txn_state_emptied_pad_0(frame);
		return;
	}
	frame->window_live = false;
	host_DelayCheckpoint_finish(&frame->window);
	frame->ok = true;
	txn_state_emptied_pad_0(frame);
}

/*
 * txn_state_emptied empties a storage in place for a truncate of a
 * relfilenode its subtransaction made, which no other process reads or
 * writes: under the logging flag, it takes back its versions' counts, then,
 * inside one checkpoint window, logs a permanent storage's emptying and
 * clears the storage as cleared does.
 */
bool
txn_state_emptied(HostIoIo *io, const HostSharedHeld *held, HostError *error)
{
	txn_state_emptied_frame_0_t frame = {.io = io, .held = held, .error = error};

	std_landing(&frame, txn_state_emptied_body_0, txn_state_emptied_pad_0);
	return frame.ok;
}

/* txn_state_redo_emptied_body_0 is redo_emptied's body. */
static void
txn_state_redo_emptied_body_0(void *frame_)
{
	txn_state_emptied_frame_0_t *frame = frame_;

	frame->ok = txn_state_emptied_flagged(frame) && txn_state_uncounted(frame->io, frame->held, frame->error) &&
		txn_state_cleared(frame->io, frame->held, frame->error);
	txn_state_emptied_pad_0(frame);
}

/*
 * txn_state_redo_emptied replays a storage's emptying: under the logging
 * flag, it takes back its replayed versions' counts and clears it as
 * cleared does.
 */
bool
txn_state_redo_emptied(HostIoIo *io, const HostSharedHeld *held, HostError *error)
{
	txn_state_emptied_frame_0_t frame = {.io = io, .held = held, .error = error};

	std_landing(&frame, txn_state_redo_emptied_body_0, txn_state_emptied_pad_0);
	return frame.ok;
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
		if (frame->owned_live && !txn_state_unindexed(&frame->making, frame->storage, found->f0, frame->error))
		{
			txn_state_unlinked_pad_0(frame);
			return;
		}
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
void
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
	uint64		start;
	uint64		end;

	if (!txn_state_behind(frame->sweep.io, &frame->sweep.held, &behind, &reached, &start, &end, frame->sweep.error))
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
 * move finds the state its storage had; under the makers' lock, it moves
 * each such state's entry in the index to the storage's new key (bugs row
 * 405). A lookup by either key between a state's exchange and its entry's
 * move would find nothing, and none can come: every lookup of a storage by
 * name is a process connected to its database, movedb runs this only once
 * CountOtherDBBackends finds no other process of the database, fold workers
 * included, and the move's redo ends a standby's sessions of the database
 * before it, while background processes reach states by sweeping the pool.
 */
bool
txn_state_rekeyed(HostIoIo *io, uint32 database, uint32 from, uint32 to, HostError *error)
{
	StdVec		states;
	HostSharedRecord makers;
	HostSharedGuard making;
	bool		ok = true;

	if (!txn_state_states(io, database, &states, error))
		return false;
	if (!host_shared_single(&TXN_STATE_MAKERS_SINGLE_OPS, &makers, error) ||
		!host_shared_Record_lock(makers, HOST_IO_IO_TAKES_SHORT(io), TXN_STATE_MAKING_INDEX, &making, error))
	{
		txn_state_states_free(&states);
		return false;
	}
	for (size_t k = 0, k_len = states.len; k < k_len && ok; k++)
	{
		const txn_TupleU64HeldState_t *state = &STD_VEC_AT(&states, txn_TupleU64HeldState_t, k);
		HostSharedRecord record = host_shared_Held_record(&state->f1);
		HostStorage storage;
		TxnStatePersistence persistence;
		uint8		key[TXN_STATE_INDEX_KEY];

		if (txn_state_exchange(record, TXN_STATE_TABLESPACE, from, to) != from)
			continue;
		(void) txn_state_described(record, &storage, &persistence);
		txn_state_index_key(&storage, key);
		storage.tablespace = from;
		ok = txn_state_unindexed(&making, &storage, state->f0, error) &&
			host_shared_index_put(HOST_SHARED_GUARD_WITHIN(&making), &TXN_STATE_STATE_POOL_OPS, key, state->f0,
								  error);
	}
	host_shared_Guard_drop(&making);
	txn_state_states_free(&states);
	return ok;
}

/*
 * txn_state_rehomed rekeys a database's states and stores the tablespace
 * its repository moved to in place of the one it moved from as the
 * repository tablespace of each, at the move's finish, whose copy is the
 * repository's own directory from then on, so that a checkpoint, a fold, and
 * a spill's files find the repository where it stands; it then renames each
 * filled state's spills' manifest to its storage's new key's name (bugs row
 * 438).
 */
bool
txn_state_rehomed(HostIoIo *io, uint32 database, uint32 from, uint32 to, HostError *error)
{
	StdVec		states;
	bool		ok = true;

	if (!txn_state_rekeyed(io, database, from, to, error) || !txn_state_states(io, database, &states, error))
		return false;
	for (size_t k = 0, k_len = states.len; k < k_len; k++)
		txn_state_exchange(host_shared_Held_record(&STD_VEC_AT(&states, txn_TupleU64HeldState_t, k).f1),
						   TXN_STATE_OWN, from, to);
	for (size_t k = 0, k_len = states.len; k < k_len && ok; k++)
	{
		HostSharedRecord record = host_shared_Held_record(&STD_VEC_AT(&states, txn_TupleU64HeldState_t, k).f1);
		HostStorage storage;
		TxnStatePersistence persistence;

		ok = !txn_state_described(record, &storage, &persistence) ||
			txn_spill_manifest_moved(io, record, &storage, error);
	}
	txn_state_states_free(&states);
	return ok;
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

/*
 * txn_state_delta_clear reports whether a state's delta holds no version or
 * take, in its memtables or in a spill file: its rows', free space map's, and
 * tuple id table's words are clear, its spills' manifest lists no file, and
 * no spill is pending.
 */
static bool
txn_state_delta_clear(HostSharedRecord record)
{
	return (txn_state_load(record, TXN_STATE_MEMTABLE_WORD_INDEX) | txn_state_load(record, TXN_STATE_FREE_WORD_INDEX) |
			txn_state_load(record, TXN_STATE_TUPLES_WORD_INDEX) | txn_state_load(record, TXN_STATE_SPILLS_INDEX) |
			txn_state_load(record, TXN_STATE_SPILLS_PENDING)) == 0;
}

/*
 * txn_state_gathered_frame_0_t is gathered's frame: the slot reads' frame,
 * the save's token, and the edit and base end it gives.
 */
typedef struct txn_state_gathered_frame_0_t
{
	txn_state_slot_frame_0_t slot;
	uint64		token;
	bool	   *some;
	RepoGathered *result;
	HostLsn    *end;
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
	if (txn_state_delta_clear(record))
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
	txn_state_exchange(record, TXN_STATE_SAVED, saved, frame->token);
	key = txn_state_load(record, TXN_STATE_KEY);
	frame->result->tag = REPO_GATHERED_BASE;
	frame->result->u.base.storage.tablespace = (uint32) txn_state_load(record, TXN_STATE_TABLESPACE);
	frame->result->u.base.storage.relation = (uint32) key;
	frame->result->u.base.folded = frame->slot.base.folded;
	txn_base_Base_encode(&frame->slot.base, &frame->result->u.base.record.bytes,
						 &frame->result->u.base.record.addresses);
	*frame->end = txn_state_load(record, TXN_STATE_BASE_END);
	*frame->some = true;
	txn_state_gathered_pad_0(frame);
}

/*
 * txn_state_gathered is one state's part of a save: under the state's lock,
 * for a ready state whose saved isn't clean, it first lets the tuple id
 * counter fall back to the next tuple id past every row its base record
 * holds, (0,1) for no base, when its delta holds no version or take, in its
 * memtables or in a spill file, since no version or entry then names a tuple
 * id or a place past it and no insert has taken one; then it stores
 * the save's token in saved and returns the record a make would read back,
 * the base as its fold wrote it, the same on every server (bugs row 483); the
 * checkpoint's cut keeps the counter for a restart. A storage with no
 * base gives no record: idle, its state is the manifest's and turns clean,
 * and otherwise its tuple ids are named only by versions its WAL redoes. With
 * a record, it gives the state's base end, read under the same lock.
 */
static bool
txn_state_gathered(const HostSharedHeld *held, HostIoIo *io, uint64 token, bool *some, RepoGathered *result,
				   HostLsn *end, HostError *error)
{
	txn_state_gathered_frame_0_t frame = {.slot = {.io = io, .held = held, .error = error}, .token = token,
	.some = some, .result = result, .end = end};

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
	HostLsn		latest = 0;

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
		HostLsn		end;

		if (!txn_state_gathered(&state->f1, frame->io, token, &some, &frame->edit, &end, frame->error))
		{
			txn_state_gather_pad_0(frame);
			return;
		}
		if (!some)
			continue;
		latest = Max(latest, end);
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
	frame->result_1->end = latest;
	frame->ok = true;
	txn_state_gather_pad_0(frame);
}

/*
 * txn_state_gather is the save's gathering for a repository's database,
 * called after the save's read of the manifest: for each ready state of the
 * database whose saved isn't clean, read one at a time under each state's
 * lock, it gives the record a make would read back and stores the save's
 * token in saved, which saved turns to clean once the manifest that holds
 * the record is written. The saving's end is the latest base end among the
 * records it gives.
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

	return txn_runs_named_space(tablespace, (uint32) txn_state_load(record, TXN_STATE_OWN));
}

/* txn_state_spill_space returns the tablespace a filled state's spills' names carry. */
uint32
txn_state_spill_space(HostSharedRecord record)
{
	return (uint32) txn_state_load(record, TXN_STATE_SPILL_SPACE);
}

/* txn_state_spill_space_loaded stores the tablespace the spills a start loads into a state name, before any of its own. */
void
txn_state_spill_space_loaded(HostSharedRecord record, uint32 named)
{
	txn_state_stored(record, TXN_STATE_SPILL_SPACE, named);
}

/* txn_state_manifest_space returns the tablespace a filled state's spills' manifest file is named by. */
uint32
txn_state_manifest_space(HostSharedRecord record)
{
	return (uint32) txn_state_load(record, TXN_STATE_MANIFEST_SPACE);
}

/* txn_state_manifest_space_moved stores the tablespace a state's spills' manifest file was renamed to, under its writers' lock. */
void
txn_state_manifest_space_moved(HostSharedRecord record, uint32 named)
{
	txn_state_stored(record, TXN_STATE_MANIFEST_SPACE, named);
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
#include "doltgres/chunks/trailer.h"
#include "doltgres/simc/simc.h"
#include "doltgres/txn/memtable/testing.h"
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

/* txn_state_tests_storage returns a test's storage in pg_default, by its database and relation number. */
static HostStorage
txn_state_tests_storage(uint32 database, uint32 relation)
{
	HostStorage storage = {HOST_PG_DEFAULT, database, relation};

	return storage;
}

/* txn_state_tests_created returns a new repository for a database's number. */
static RepoRepo
txn_state_tests_created(HostIoIo *io, uint32 database)
{
	RepoRepo	repo;
	ChunksErrorError error;

	if (database == 0)
		STD_OPTION_UNWRAP_FAILED(1729, 52);
	if (!repo_Repo_create(io, database, HOST_PG_DEFAULT, &repo, &error))
		STD_RESULT_EXPECT_FAILED(1729, 80, "called `Result::unwrap()` on an `Err` value", &error,
								 chunks_error_Error_Debug_fmt);
	return repo;
}

/* txn_state_tests_put_t is put's gathering's captures: the storage and its base record's encoding. */
typedef struct txn_state_tests_put_t
{
	const HostStorage *storage;
	const StdVec *bytes;
	const StdVec *addresses;
	HostLsn		folded;
} txn_state_tests_put_t;

/* txn_state_tests_put_closure_1 is put's gathering: the storage's base record as the one edit. */
static bool
txn_state_tests_put_closure_1(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
							  ChunksErrorError *error)
{
	txn_state_tests_put_t *c = captures;
	RepoGathered edit = {.tag = REPO_GATHERED_BASE};

	edit.u.base.storage = txn_state_in_repository(c->storage);
	std_slice_to_vec_Checked(&edit.u.base.record.bytes, c->bytes->ptr, c->bytes->len, 1, 1);
	std_slice_to_vec_Checked(&edit.u.base.record.addresses, c->addresses->ptr, c->addresses->len, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	edit.u.base.folded = c->folded;
	std_vec_with_capacity_Checked(result, 1, sizeof(RepoGathered), _Alignof(RepoGathered));
	std_vec_push_Checked(result, &edit, sizeof(edit), _Alignof(RepoGathered));
	return true;
}

/* txn_state_tests_put_closure_2 is put's marks, which mark nothing. */
static bool
txn_state_tests_put_closure_2(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	return true;
}

/* txn_state_tests_put saves a storage's base record into its repository's newest manifest. */
static void
txn_state_tests_put(HostIoIo *io, const RepoRepo *repo, const HostStorage *storage, const TxnBaseBase *base)
{
	HostSharedReader reading;
	HostError	host;
	ChunksErrorError error;
	StdVec		bytes;
	StdVec		addresses;
	txn_state_tests_put_t captures = {storage, &bytes, &addresses, base->folded};

	if (!host_shared_read(io, &reading, &host))
		STD_RESULT_EXPECT_FAILED(1739, 46, "called `Result::unwrap()` on an `Err` value", &host,
								 host_Error_Debug_fmt);
	txn_base_Base_encode(base, &bytes, &addresses);
	if (!repo_Repo_save(repo, io, &reading, false, NULL, txn_state_tests_put_closure_1, &captures,
						txn_state_tests_put_closure_2, NULL, &error))
		STD_RESULT_EXPECT_FAILED(1746, 76, "called `Result::unwrap()` on an `Err` value", &error,
								 chunks_error_Error_Debug_fmt);
	std_vec_free_Checked(&addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_vec_free_Checked(&bytes, 1, 1);
	host_shared_Reader_drop(&reading);
}

/* txn_state_tests_filled asserts a state's ready and saved words, as assert_eq on filled does at its place. */
static void
txn_state_tests_filled(uint32 line, uint32 col, HostSharedRecord record, uint64 ready, uint64 saved)
{
	uint64		left[2] = {txn_state_load(record, TXN_STATE_READY), txn_state_load(record, TXN_STATE_SAVED)};
	uint64		right[2] = {ready, saved};
	StdTupleRef left_tuple = STD_TUPLE(2, STD_ITEM(&left[0], std_u64_Debug_fmt), STD_ITEM(&left[1], std_u64_Debug_fmt));
	StdTupleRef right_tuple = STD_TUPLE(2, STD_ITEM(&right[0], std_u64_Debug_fmt),
										STD_ITEM(&right[1], std_u64_Debug_fmt));

	STD_ASSERT_EQ(line, col, left[0] == right[0] && left[1] == right[1], &left_tuple, std_tuple_Debug_fmt,
				  &right_tuple, std_tuple_Debug_fmt);
}

/* txn_state_tests_keys returns the keys of the published states. */
static void
txn_state_tests_keys(HostIoIo *io, StdVec *result)
{
	StdVec		swept;
	HostError	error;

	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(io), &TXN_STATE_STATE_POOL_OPS, TXN_STATE_KEY, &swept, &error))
		STD_RESULT_EXPECT_FAILED(1773, 33, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_with_capacity_Checked(result, swept.len, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0; k < swept.len; k++)
		std_vec_push_Checked(result, &STD_VEC_AT(&swept, std_TupleU64U64_t, k).f1, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
}

/* txn_state_tests_made returns a storage's state, made or found, as state's unwrap does at its place. */
static HostSharedHeld
txn_state_tests_made(uint32 line, uint32 col, HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
					 const HostStorage *storage)
{
	HostSharedHeld held;
	HostError	error;

	if (!txn_state_state(io, repo, reading, storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return held;
}

/* txn_state_tests_totals asserts a state's totals, as assert_eq on totals does at its place. */
static void
txn_state_tests_totals(uint32 line, uint32 col, const HostSharedHeld *held, TxnBaseSizes expected)
{
	TxnBaseSizes totals = txn_state_totals(held);

	STD_ASSERT_EQ(line, col, txn_base_Sizes_PartialEq_eq(&totals, &expected), &totals, txn_base_Sizes_Debug_fmt,
				  &expected, txn_base_Sizes_Debug_fmt);
}

/* txn_state_tests_based asserts a state's base record, as assert_eq on base_record's unwrap does at their places. */
static void
txn_state_tests_based(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, HostIoIo *io,
					  const HostSharedHeld *held, bool has_expected, const TxnBaseBase *expected)
{
	TxnBaseBase base;
	bool		some;
	HostError	error;
	StdOptionRef left;
	StdOptionRef right = {has_expected, STD_ITEM(expected, txn_base_Base_Debug_fmt)};

	if (!txn_state_base_record(io, held, &some, &base, &error))
		STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = some;
	left.some = STD_ITEM(&base, txn_base_Base_Debug_fmt);
	STD_ASSERT_EQ(line, col, some == has_expected && (!some || txn_base_Base_PartialEq_eq(&base, expected)), &left,
				  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (some)
		txn_base_Base_drop(&base);
}

/*
 * PLAN item 7's make: a state is filled from its base record, ready and
 * clean, its tree's totals the base's sizes, and found again as the same
 * state; a storage with no base is filled empty and clean, its totals zero;
 * the same relation number in another database has a state of its own.
 */
void
test_txn_state_tests_a_state_is_filled_from_its_base_and_found_again(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo = txn_state_tests_created(io, 9201);
	HostStorage based = txn_state_tests_storage(9201, 16384);
	HostStorage bare = txn_state_tests_storage(9201, 16385);
	HostStorage elsewhere = txn_state_tests_storage(9202, 16384);
	TxnBaseBase full = txn_base_tests_full();
	HostSharedReader reading;
	HostError	error;
	HostSharedHeld held;
	HostSharedHeld again;
	HostSharedHeld unbased;
	RepoRepo	other;
	StdVec		keys;
	uint64		key = txn_state_key(&based);
	StdVec		expected = {.ptr = &key, .len = 1, .cap = 1};
	TxnBaseSizes none = txn_base_Sizes_Default_default();
	size_t		count;
	size_t		three = 3;

	txn_state_tests_put(io, &repo, &based, &full);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(1807, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_state_tests_made(1808, 85, io, &repo, &reading, &based);
	txn_state_tests_filled(1809, 9, host_shared_Held_record(&held), 1, TXN_STATE_CLEAN);
	txn_state_tests_based(1810, 9, 1810, 43, io, &held, true, &full);
	txn_state_tests_totals(1811, 9, &held, full.sizes);
	again = txn_state_tests_made(1812, 79, io, &repo, &reading, &based);
	host_shared_Held_drop(&again);
	txn_state_tests_keys(io, &keys);
	STD_ASSERT_EQ(1813, 9, keys.len == 1 && STD_VEC_AT(&keys, uint64, 0) == key, &keys,
				  chunks_trailer_VecU64_Debug_fmt, &expected, chunks_trailer_VecU64_Debug_fmt);
	std_vec_free_Checked(&keys, sizeof(uint64), _Alignof(uint64));
	unbased = txn_state_tests_made(1814, 87, io, &repo, &reading, &bare);
	txn_state_tests_filled(1815, 9, host_shared_Held_record(&unbased), 1, TXN_STATE_CLEAN);
	txn_state_tests_based(1816, 9, 1816, 46, io, &unbased, false, NULL);
	txn_state_tests_totals(1817, 9, &unbased, none);
	other = txn_state_tests_created(io, 9202);
	again = txn_state_tests_made(1819, 84, io, &other, &reading, &elsewhere);
	host_shared_Held_drop(&again);
	txn_state_tests_keys(io, &keys);
	count = keys.len;
	std_vec_free_Checked(&keys, sizeof(uint64), _Alignof(uint64));
	STD_ASSERT_EQ(1820, 9, count == three, &count, std_usize_Debug_fmt, &three, std_usize_Debug_fmt);
	repo_Repo_drop(&other);
	host_shared_Held_drop(&unbased);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

/* txn_state_tests_reopened returns a process's own handle on a database's repository. */
bool
txn_state_tests_reopened(HostIoIo *io, uint32 database, RepoRepo *result, HostError *error)
{
	ChunksErrorError failed;

	if (database == 0)
		STD_OPTION_UNWRAP_FAILED(1734, 50);
	if (!repo_Repo_open(io, database, HOST_PG_DEFAULT, result, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	return true;
}

/* txn_state_tests_endings_Debug_fmt is a Vec<&Ending>'s Debug, of a run's endings without their names. */
static bool
txn_state_tests_endings_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *endings = value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t k = 0; k < endings->len; k++)
		std_DebugList_entry(&b, &STD_VEC_AT(endings, simc_TupleStringEnding_t, k).f1, simc_Ending_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* txn_state_tests_expected_Debug_fmt is a Vec<&Ending>'s Debug, of the endings a test expects. */
static bool
txn_state_tests_expected_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdBytes *expected = value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t k = 0; k < expected->len; k++)
		std_DebugList_entry(&b, &((const SimcEnding *) expected->ptr)[k], simc_Ending_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* txn_state_tests_ended asserts that a run's actors ended as expected, in the order they were added. */
static void
txn_state_tests_ended(const SimcReport *report, const SimcEnding *expected, size_t expected_len)
{
	const StdVec *endings;
	StdBytes	right = {(const uint8 *) expected, expected_len};
	bool		eq;

	if (report->outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report->outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 1778, 67, &arguments);
	}
	endings = &report->outcome.u.ended;
	eq = endings->len == expected_len;
	for (size_t k = 0; k < endings->len && eq; k++)
		eq = simc_Ending_PartialEq_eq(&STD_VEC_AT(endings, simc_TupleStringEnding_t, k).f1, &expected[k]);
	STD_ASSERT_EQ(1780, 9, eq, endings, txn_state_tests_endings_Debug_fmt, &right,
				  txn_state_tests_expected_Debug_fmt);
}

/* txn_state_tests_found returns the published state of a storage, as find's two unwraps do at their places. */
static HostSharedHeld
txn_state_tests_found(uint32 line, uint32 col, uint32 some_line, uint32 some_col, HostIoIo *io,
					  const HostStorage *storage)
{
	HostSharedHeld held;
	bool		some;
	HostError	error;

	if (!txn_state_find(HOST_SHARED_IO_WITHIN(io), storage, &some, &held, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!some)
		STD_OPTION_UNWRAP_FAILED(some_line, some_col);
	return held;
}

/* txn_state_tests_opened_t is an actor's captures that opens a storage's state: its database and storage. */
typedef struct txn_state_tests_opened_t
{
	uint32		database;
	HostStorage storage;
} txn_state_tests_opened_t;

/* txn_state_tests_opened_closure_0 is an actor that reopens a repository and opens a storage's state, then drops it. */
static bool
txn_state_tests_opened_closure_0(void *captures, HostIoIo *io, HostError *error)
{
	txn_state_tests_opened_t *c = captures;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	bool		ok;

	if (!txn_state_tests_reopened(io, c->database, &repo, error))
		return false;
	if (!host_shared_read(io, &reading, error))
	{
		repo_Repo_drop(&repo);
		return false;
	}
	ok = txn_state_state(io, &repo, &reading, &c->storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held,
						 error);
	if (ok)
		host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/*
 * PLAN item 7's case: two backends that open one storage make one state.
 * The first is held after its make, before the makers' lock, while the
 * second makes, publishes, and fills its own; the first then finds that one
 * under the lock and gives its own up unpublished.
 */
void
test_txn_state_tests_two_backends_that_open_one_storage_make_one_state(void)
{
	static const char *const names[] = {"first", "second"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage s = txn_state_tests_storage(9203, 16384);
	RepoRepo	repo = txn_state_tests_created(io, 9203);
	TxnBaseBase full = txn_base_tests_full();
	SimcRun		run;
	SimcStep	steps[2] = {{"second", 6, PG_UINT64_MAX}, {"first", 5, PG_UINT64_MAX}};
	SimcReport	report;
	SimcEnding	expected[2] = {{.tag = SIMC_ENDING_RETURNED}, {.tag = SIMC_ENDING_RETURNED}};
	StdVec		keys;
	uint64		key = txn_state_key(&s);
	StdVec		wanted = {.ptr = &key, .len = 1, .cap = 1};
	HostSharedHeld held;
	txn_state_tests_opened_t captures[2];

	txn_state_tests_put(io, &repo, &s, &full);
	run = simc_Run_new(71);
	for (size_t k = 0; k < lengthof(names); k++)
	{
		SimcActor	actor = {txn_state_tests_opened_closure_0, &captures[k]};

		captures[k].database = 9203;
		captures[k].storage = s;
		simc_Run_actor(&run, names[k], strlen(names[k]), actor);
	}
	simc_Run_until(&run, "first", 5, "doltgres-state-made");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_state_tests_ended(&report, expected, lengthof(expected));
	txn_state_tests_keys(io, &keys);
	STD_ASSERT_EQ(1845, 9, keys.len == 1 && STD_VEC_AT(&keys, uint64, 0) == key, &keys,
				  chunks_trailer_VecU64_Debug_fmt, &wanted, chunks_trailer_VecU64_Debug_fmt);
	std_vec_free_Checked(&keys, sizeof(uint64), _Alignof(uint64));
	held = txn_state_tests_found(1846, 33, 1846, 42, io, &s);
	txn_state_tests_based(1847, 9, 1847, 43, io, &held, true, &full);
	host_shared_Held_drop(&held);
	simc_Report_drop(&report);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

/* txn_state_tests_racing_closure_1 is the second filler: it opens the storage's state, clears saved as a later write would, and checks that a reclaim frees none. */
static bool
txn_state_tests_racing_closure_1(void *captures, HostIoIo *io, HostError *error)
{
	txn_state_tests_opened_t *c = captures;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	bool		freed;
	bool		ok;

	if (!txn_state_tests_reopened(io, c->database, &repo, error))
		return false;
	if (!host_shared_read(io, &reading, error))
	{
		repo_Repo_drop(&repo);
		return false;
	}
	ok = txn_state_state(io, &repo, &reading, &c->storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held,
						 error);
	if (ok)
	{
		txn_state_exchange(host_shared_Held_record(&held), TXN_STATE_SAVED, TXN_STATE_CLEAN, 0);
		host_shared_Held_drop(&held);
		ok = txn_state_State_Pool_reclaim(io, &freed, error);
		if (ok && freed)
		{
			static const char text[] = "a reclaim freed a state the first finder holds";

			*error = txn_state_internal(text, strlen(text));
			ok = false;
		}
	}
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/*
 * PLAN item 7's fill: of two finders, the first is held between its read and
 * its lock while the second fills the state and clears saved, as a later
 * write would, and the first's handle keeps the state from a reclaim; the
 * first then finds ready set under the lock and drops what it read, so saved
 * stays cleared.
 */
void
test_txn_state_tests_of_two_racing_fillers_the_second_to_the_lock_drops_its_read(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage s = txn_state_tests_storage(9204, 16384);
	RepoRepo	repo = txn_state_tests_created(io, 9204);
	TxnBaseBase full = txn_base_tests_full();
	txn_state_tests_opened_t captures = {9204, s};
	SimcActor	first = {txn_state_tests_opened_closure_0, &captures};
	SimcActor	second = {txn_state_tests_racing_closure_1, &captures};
	SimcRun		run;
	SimcStep	steps[2] = {{"second", 6, PG_UINT64_MAX}, {"first", 5, PG_UINT64_MAX}};
	SimcReport	report;
	SimcEnding	expected[2] = {{.tag = SIMC_ENDING_RETURNED}, {.tag = SIMC_ENDING_RETURNED}};
	HostSharedHeld held;

	txn_state_tests_put(io, &repo, &s, &full);
	run = simc_Run_new(73);
	simc_Run_actor(&run, "first", 5, first);
	simc_Run_actor(&run, "second", 6, second);
	simc_Run_until(&run, "first", 5, "doltgres-state-read");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_state_tests_ended(&report, expected, lengthof(expected));
	held = txn_state_tests_found(1881, 33, 1881, 42, io, &s);
	txn_state_tests_filled(1882, 9, host_shared_Held_record(&held), 1, 0);
	txn_state_tests_based(1883, 9, 1883, 43, io, &held, true, &full);
	host_shared_Held_drop(&held);
	simc_Report_drop(&report);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

/* txn_state_tests_terminated_closure_0 is a maker that publishes a storage's state, pauses, and spins until the run terminates it, before its fill. */
static bool
txn_state_tests_terminated_closure_0(void *captures, HostIoIo *io, HostError *error)
{
	const HostStorage *s = captures;
	HostSharedHeld held;

	if (!txn_state_opened(io, s, &held, error))
		return false;
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-state-case-made", error))
	{
		host_shared_Held_drop(&held);
		return false;
	}
	for (;;)
		host_unique();
}

/* PLAN item 7's case: a maker terminated between its publish and ready leaves the state unfilled, and the next finder fills it. */
void
test_txn_state_tests_a_maker_terminated_before_its_fill_leaves_the_fill_to_the_next_finder(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage s = txn_state_tests_storage(9205, 16384);
	RepoRepo	repo = txn_state_tests_created(io, 9205);
	TxnBaseBase full = txn_base_tests_full();
	SimcActor	maker = {txn_state_tests_terminated_closure_0, &s};
	SimcRun		run;
	SimcReport	report;
	SimcEnding	expected[1] = {{.tag = SIMC_ENDING_TERMINATED}};
	HostSharedReader reading;
	HostError	error;
	HostSharedHeld held;

	txn_state_tests_put(io, &repo, &s, &full);
	run = simc_Run_new(79);
	simc_Run_actor(&run, "maker", 5, maker);
	simc_Run_until(&run, "maker", 5, "doltgres-state-case-made");
	simc_Run_terminate_at(&run, "maker", 5, 1000000);
	report = simc_Run_finish(run);
	txn_state_tests_ended(&report, expected, lengthof(expected));
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(1907, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_state_tests_made(1908, 81, io, &repo, &reading, &s);
	txn_state_tests_filled(1909, 9, host_shared_Held_record(&held), 1, TXN_STATE_CLEAN);
	txn_state_tests_based(1910, 9, 1910, 43, io, &held, true, &full);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	simc_Report_drop(&report);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

/* txn_state_tests_helds_drop drops a Vec<Held<State>>. */
static void
txn_state_tests_helds_drop(StdVec *holding)
{
	for (size_t k = 0; k < holding->len; k++)
		host_shared_Held_drop(&STD_VEC_AT(holding, HostSharedHeld, k));
	std_vec_free_Checked(holding, sizeof(HostSharedHeld), _Alignof(HostSharedHeld));
}

/* txn_state_tests_full_pool asserts that an error is the pool's: the internal error that every record is in use. */
static void
txn_state_tests_full_pool(const HostError *error)
{
	static const uint8 internal[5] = {'X', 'X', '0', '0', '0'};
	static const char text[] = "every storage state record is in use";
	StdBytes	sqlstate;
	StdBytes	want_sqlstate;
	StdStr		message;
	StdStr		want_message;
	StdOptionRef left_message;
	StdOptionRef right_message;
	StdTupleRef left;
	StdTupleRef right;
	bool		eq;

	sqlstate.ptr = host_Error_sqlstate(error);
	sqlstate.len = 5;
	want_sqlstate.ptr = internal;
	want_sqlstate.len = 5;
	message = host_Error_message(error);
	want_message.ptr = text;

	want_message.len = sizeof(text) - 1;
	left_message.has = message.ptr != NULL;
	left_message.some = STD_ITEM(&message, std_str_Debug_fmt);
	right_message.has = true;
	right_message.some = STD_ITEM(&want_message, std_str_Debug_fmt);
	left = STD_TUPLE(2, STD_ITEM(&sqlstate, std_bytes_Debug_fmt), STD_ITEM(&left_message, std_option_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&want_sqlstate, std_bytes_Debug_fmt), STD_ITEM(&right_message, std_option_Debug_fmt));
	eq = memcmp(sqlstate.ptr, want_sqlstate.ptr, 5) == 0 && left_message.has && message.len == want_message.len &&
		memcmp(message.ptr, want_message.ptr, message.len) == 0;
	STD_ASSERT_EQ(1926, 21, eq, &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
}

/* txn_state_tests_has_key reports whether a list of keys holds one. */
static bool
txn_state_tests_has_key(const StdVec *keys, uint64 key)
{
	for (size_t k = 0; k < keys->len; k++)
		if (STD_VEC_AT(keys, uint64, k) == key)
			return true;
	return false;
}

/* txn_state_tests_keys_counted asserts that the published states number a count, as assert_eq on keys' len does at its place. */
static void
txn_state_tests_keys_counted(uint32 line, uint32 col, HostIoIo *io, size_t count)
{
	StdVec		keys;
	size_t		len;

	txn_state_tests_keys(io, &keys);
	len = keys.len;
	std_vec_free_Checked(&keys, sizeof(uint64), _Alignof(uint64));
	STD_ASSERT_EQ(line, col, len == count, &len, std_usize_Debug_fmt, &count, std_usize_Debug_fmt);
}

/*
 * PLAN item 7's cases: backends that hold a state for every relation lock
 * Postgres can grant at once get no error, one more is the pool's internal
 * error, and a pool full of idle states makes room for a new one with no
 * error.
 */
void
test_txn_state_tests_the_pool_holds_a_state_for_each_relation_lock_and_reclaims_idle_ones(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo = txn_state_tests_created(io, 9206);
	HostSharedReader reading;
	HostError	error;
	StdVec		holding;
	size_t		count;
	uint32		last;
	StdTryFromIntError failed;
	StdVec		keys;
	HostStorage newest;
	bool		kept;

	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(1920, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_new(&holding, sizeof(HostSharedHeld), _Alignof(HostSharedHeld));
	for (uint32 relation = 1;; relation++)
	{
		HostStorage s = txn_state_tests_storage(9206, relation);
		HostSharedHeld held;

		if (!txn_state_state(io, &repo, &reading, &s, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held, &error))
		{
			txn_state_tests_full_pool(&error);
			host_Error_drop(&error);
			break;
		}
		std_vec_push_Checked(&holding, &held, sizeof(held), _Alignof(HostSharedHeld));
	}
	count = holding.len;
	txn_state_tests_keys_counted(1935, 9, io, count);
	txn_state_tests_helds_drop(&holding);
	if (!std_u32_try_from_u64((uint64) count, &last, &failed))
		STD_RESULT_EXPECT_FAILED(1937, 41, "called `Result::unwrap()` on an `Err` value", &failed,
								 std_TryFromIntError_Debug_fmt);
	last = STD_ADD(u32, last, 1, 1937, 20);
	for (uint32 relation = 1; relation <= last; relation++)
	{
		HostStorage s = txn_state_tests_storage(9206, relation);
		HostSharedHeld held = txn_state_tests_made(1939, 101, io, &repo, &reading, &s);

		host_shared_Held_drop(&held);
	}
	txn_state_tests_keys_counted(1941, 9, io, count);
	newest = txn_state_tests_storage(9206, last);
	txn_state_tests_keys(io, &keys);
	kept = txn_state_tests_has_key(&keys, txn_state_key(&newest));
	std_vec_free_Checked(&keys, sizeof(uint64), _Alignof(uint64));
	STD_ASSERT(1942, 9, kept, "keys(io).contains(&key(&storage(9206, last)))");
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/*
 * PLAN item 7's case: a backend that opens a storage while a reclaim looks
 * at its state, held there, finds the state, and the reclaim's free answers
 * false, so a make in a pool otherwise held fails with the pool's error.
 */
void
test_txn_state_tests_a_state_opened_while_a_reclaim_looks_at_it_is_kept(void)
{
	static const char text[] = "every storage state record is in use";
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo = txn_state_tests_created(io, 9208);
	HostSharedReader reading;
	HostError	error;
	StdVec		holding;
	uint32		len;
	StdTryFromIntError failed;
	HostSharedHeld popped;
	txn_state_tests_opened_t making;
	txn_state_tests_opened_t opening;
	SimcActor	maker = {txn_state_tests_opened_closure_0, &making};
	SimcActor	opener = {txn_state_tests_opened_closure_0, &opening};
	SimcRun		run;
	SimcStep	steps[2] = {{"opener", 6, PG_UINT64_MAX}, {"maker", 5, PG_UINT64_MAX}};
	SimcReport	report;
	SimcEnding	expected[2] = {{.tag = SIMC_ENDING_RETURNED, .is_err = true, .sqlstate = {'X', 'X', '0', '0', '0'}},
	{.tag = SIMC_ENDING_RETURNED}};
	StdVec		keys;
	bool		kept;

	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(1952, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_new(&holding, sizeof(HostSharedHeld), _Alignof(HostSharedHeld));
	for (uint32 relation = 1;; relation++)
	{
		HostStorage s = txn_state_tests_storage(9208, relation);
		HostSharedHeld held;

		if (!txn_state_state(io, &repo, &reading, &s, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held, &error))
		{
			host_Error_drop(&error);
			break;
		}
		std_vec_push_Checked(&holding, &held, sizeof(held), _Alignof(HostSharedHeld));
	}
	if (!std_u32_try_from_u64((uint64) holding.len, &len, &failed))
		STD_RESULT_EXPECT_FAILED(1960, 71, "called `Result::unwrap()` on an `Err` value", &failed,
								 std_TryFromIntError_Debug_fmt);
	opening.database = 9208;
	opening.storage = txn_state_tests_storage(9208, len);
	making.database = 9208;
	making.storage = txn_state_tests_storage(9208, PG_UINT32_MAX);
	if (std_vec_pop(&holding, &popped, sizeof(popped)))
		host_shared_Held_drop(&popped);
	expected[0].message.ptr = (uint8 *) text;

	expected[0].message.len = sizeof(text) - 1;
	expected[0].message.cap = sizeof(text) - 1;
	run = simc_Run_new(83);
	simc_Run_actor(&run, "maker", 5, maker);
	simc_Run_actor(&run, "opener", 6, opener);
	simc_Run_until(&run, "maker", 5, "doltgres-state-reclaim-looked");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_state_tests_ended(&report, expected, lengthof(expected));
	txn_state_tests_keys(io, &keys);
	kept = txn_state_tests_has_key(&keys, txn_state_key(&opening.storage));
	std_vec_free_Checked(&keys, sizeof(uint64), _Alignof(uint64));
	STD_ASSERT(1978, 9, kept, "keys(io).contains(&key(&idle))");
	txn_state_tests_helds_drop(&holding);
	simc_Report_drop(&report);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}

/* txn_state_tests_saved_t is saved's captures: the database, the gathering's Saving while it holds one, and the step run between the manifest's write and saved. */
typedef struct txn_state_tests_saved_t
{
	uint32		database;
	bool		has_saving;
	TxnStateSaving saving;
	void		(*between) (void *captures, HostIoIo *io);
	void	   *between_captures;
} txn_state_tests_saved_t;

/* txn_state_tests_saved_closure_0 is saved's gathering: the states' edits, keeping their Saving for the marks. */
static bool
txn_state_tests_saved_closure_0(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
								ChunksErrorError *error)
{
	txn_state_tests_saved_t *c = captures;
	TxnStateSaving gathered;
	HostError	failed;

	if (!txn_state_gather(io, c->database, read, result, &gathered, &failed))
	{
		error->tag = CHUNKS_ERROR_ERROR_HOST;
		error->u.host = failed;
		return false;
	}
	if (c->has_saving)
		std_vec_free_Checked(&c->saving.locations, sizeof(uint64), _Alignof(uint64));
	c->saving = gathered;
	c->has_saving = true;
	return true;
}

/* txn_state_tests_saved_closure_1 is saved's marks: the step between, then the gathered states marked saved. */
static bool
txn_state_tests_saved_closure_1(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	txn_state_tests_saved_t *c = captures;
	HostError	failed;

	c->between(c->between_captures, io);
	if (!c->has_saving)
		STD_OPTION_UNWRAP_FAILED(1761, 27);
	c->has_saving = false;
	if (!txn_state_Saving_saved(&c->saving, io, &failed))
	{
		error->tag = CHUNKS_ERROR_ERROR_HOST;
		error->u.host = failed;
		return false;
	}
	return true;
}

/* txn_state_tests_saved saves a database's repository through the gathering, running between after the manifest's write landed and before saved. */
void
txn_state_tests_saved(HostIoIo *io, const RepoRepo *repo, uint32 database,
					  void (*between) (void *captures, HostIoIo *io), void *between_captures)
{
	HostSharedReader reading;
	HostError	host;
	ChunksErrorError error;
	txn_state_tests_saved_t captures = {.database = database, .between = between,
	.between_captures = between_captures};

	if (!host_shared_read(io, &reading, &host))
		STD_RESULT_EXPECT_FAILED(1752, 46, "called `Result::unwrap()` on an `Err` value", &host,
								 host_Error_Debug_fmt);
	if (!repo_Repo_save(repo, io, &reading, false, NULL, txn_state_tests_saved_closure_0, &captures,
						txn_state_tests_saved_closure_1, &captures, &error))
		STD_RESULT_EXPECT_FAILED(1763, 57, "called `Result::unwrap()` on an `Err` value", &error,
								 chunks_error_Error_Debug_fmt);
	if (captures.has_saving)
		std_vec_free_Checked(&captures.saving.locations, sizeof(uint64), _Alignof(uint64));
	host_shared_Reader_drop(&reading);
}

/* txn_state_tests_nothing is a step between that does nothing. */
static void
txn_state_tests_nothing(void *captures, HostIoIo *io)
{
}

/* txn_state_tests_written_closure_0 is a write between a gathering and its saved: saved cleared from its token. */
static void
txn_state_tests_written_closure_0(void *captures, HostIoIo *io)
{
	const HostSharedRecord *record = captures;
	uint64		token = txn_state_load(*record, TXN_STATE_SAVED);

	txn_state_exchange(*record, TXN_STATE_SAVED, token, 0);
}

/* txn_state_tests_gathering_closure_0 is a gathering that keeps the states' edits alone, its Saving dropped. */
static bool
txn_state_tests_gathering_closure_0(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
									ChunksErrorError *error)
{
	TxnStateSaving gathered;
	HostError	failed;

	if (!txn_state_gather(io, 9210, read, result, &gathered, &failed))
	{
		error->tag = CHUNKS_ERROR_ERROR_HOST;
		error->u.host = failed;
		return false;
	}
	std_vec_free_Checked(&gathered.locations, sizeof(uint64), _Alignof(uint64));
	return true;
}

/* txn_state_tests_gathering_closure_1 is the marks of a save that gathered nothing, which never run. */
static bool
txn_state_tests_gathering_closure_1(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	STD_PANIC(2031, 54, "a save that gathered nothing wrote");
}

/*
 * PLAN item 7's case: a write between a gathering and its saved leaves the
 * state unsaved, the next save makes it clean, and a save that gathers no
 * state writes nothing.
 */
void
test_txn_state_tests_a_write_between_a_gathering_and_its_saved_leaves_the_state_unsaved(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage s = txn_state_tests_storage(9210, 16384);
	RepoRepo	repo = txn_state_tests_created(io, 9210);
	TxnBaseBase full = txn_base_tests_full();
	HostSharedReader reading;
	HostSharedReader again;
	HostError	error;
	ChunksErrorError failed;
	HostSharedHeld held;
	HostSharedRecord record;

	txn_state_tests_put(io, &repo, &s, &full);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(2018, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_state_tests_made(2019, 81, io, &repo, &reading, &s);
	record = host_shared_Held_record(&held);
	txn_state_exchange(record, TXN_STATE_SAVED, TXN_STATE_CLEAN, 0);
	txn_state_tests_saved(io, &repo, 9210, txn_state_tests_written_closure_0, &record);
	txn_state_tests_filled(2026, 9, record, 1, 0);
	txn_state_tests_saved(io, &repo, 9210, txn_state_tests_nothing, NULL);
	txn_state_tests_filled(2028, 9, record, 1, TXN_STATE_CLEAN);
	if (!host_shared_read(io, &again, &error))
		STD_RESULT_EXPECT_FAILED(2029, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!repo_Repo_save(&repo, io, &again, false, NULL, txn_state_tests_gathering_closure_0, NULL,
						txn_state_tests_gathering_closure_1, NULL, &failed))
		STD_RESULT_EXPECT_FAILED(2031, 100, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	host_shared_Reader_drop(&again);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

/* txn_state_tests_keys_are asserts the keys of the published states, as assert_eq on keys does at its place. */
static void
txn_state_tests_keys_are(uint32 line, uint32 col, HostIoIo *io, const uint64 *expected, size_t expected_len)
{
	StdVec		keys;
	StdVec		wanted = {.ptr = (void *) expected, .len = expected_len, .cap = expected_len};
	bool		eq;

	txn_state_tests_keys(io, &keys);
	eq = keys.len == expected_len;
	for (size_t k = 0; k < keys.len && eq; k++)
		eq = STD_VEC_AT(&keys, uint64, k) == expected[k];
	STD_ASSERT_EQ(line, col, eq, &keys, chunks_trailer_VecU64_Debug_fmt, &wanted, chunks_trailer_VecU64_Debug_fmt);
	std_vec_free_Checked(&keys, sizeof(uint64), _Alignof(uint64));
}

/* txn_state_tests_unlinked frees a storage's state, as unlinked's unwrap does at its place. */
static void
txn_state_tests_unlinked(uint32 line, uint32 col, HostIoIo *io, const HostStorage *storage)
{
	HostError	error;

	if (!txn_state_unlinked(io, storage, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/*
 * PLAN item 7's free: the free at the unlink takes a state a process holds,
 * after which no find reaches it and a find makes a new one, and a storage
 * with no state has nothing to free.
 */
void
test_txn_state_tests_the_free_at_the_unlink_takes_a_held_state(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage s = txn_state_tests_storage(9211, 16384);
	HostStorage bare = txn_state_tests_storage(9211, 16385);
	RepoRepo	repo = txn_state_tests_created(io, 9211);
	TxnBaseBase full = txn_base_tests_full();
	HostSharedReader reading;
	HostError	error;
	HostSharedHeld held;
	HostSharedHeld found;
	HostSharedHeld again;
	bool		some;
	uint64		key = txn_state_key(&s);

	txn_state_tests_put(io, &repo, &s, &full);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(2043, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_state_tests_made(2044, 81, io, &repo, &reading, &s);
	txn_state_tests_unlinked(2045, 26, io, &s);
	txn_state_tests_keys_are(2046, 9, io, NULL, 0);
	if (!txn_state_find(HOST_SHARED_IO_WITHIN(io), &s, &some, &found, &error))
		STD_RESULT_EXPECT_FAILED(2047, 30, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (some)
		host_shared_Held_drop(&found);
	STD_ASSERT(2047, 9, !some, "find(io, &s).unwrap().is_none()");
	txn_state_tests_filled(2048, 9, host_shared_Held_record(&held), 1, TXN_STATE_CLEAN);
	host_shared_Held_drop(&held);
	txn_state_tests_unlinked(2050, 45, io, &bare);
	again = txn_state_tests_made(2051, 82, io, &repo, &reading, &s);
	txn_state_tests_based(2052, 9, 2052, 44, io, &again, true, &full);
	txn_state_tests_totals(2053, 9, &again, full.sizes);
	txn_state_tests_keys_are(2054, 9, io, &key, 1);
	host_shared_Held_drop(&again);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

/*
 * PLAN item 7's save: a state filled from one record and changed to
 * another, as a write would, every field moving, is gathered and saved,
 * clean, into the manifest; reclaimed and made again, it reads every field
 * equal.
 */
void
test_txn_state_tests_a_state_saved_reclaimed_and_made_again_reads_the_same(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage s = txn_state_tests_storage(9209, 16384);
	RepoRepo	repo = txn_state_tests_created(io, 9209);
	TxnBaseBase full = txn_base_tests_full();
	TxnBaseBase empty = txn_base_tests_empty();
	TxnBaseBase decoded;
	HostSharedReader reading;
	HostError	error;
	ChunksErrorError failed;
	HostSharedHeld held;
	HostSharedHeld again;
	HostSharedRecord record;
	HostSharedGuard guard;
	StdVec		blob;
	RepoBaseRecord read;
	bool		some;
	bool		freed;
	RepoStorage name = txn_state_in_repository(&s);

	txn_state_tests_put(io, &repo, &s, &full);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(1992, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_state_tests_made(1993, 81, io, &repo, &reading, &s);
	record = host_shared_Held_record(&held);
	if (!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(io), TXN_STATE_STATE_LOCK_INDEX, &guard, &error))
		STD_RESULT_EXPECT_FAILED(1995, 38, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_state_kept(&empty, &blob);
	if (!host_shared_GuardExclusive_write_blob(&guard, 0, blob.ptr, blob.len, &error))
		STD_RESULT_EXPECT_FAILED(1995, 80, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&blob, 1, 1);
	host_shared_Guard_drop(&guard);
	txn_state_exchange(record, TXN_STATE_SAVED, TXN_STATE_CLEAN, 0);
	txn_state_tests_saved(io, &repo, 9209, txn_state_tests_nothing, NULL);
	txn_state_tests_filled(1998, 9, record, 1, TXN_STATE_CLEAN);
	if (!repo_Repo_base(&repo, io, &reading, &name, &some, &read, &failed))
		STD_RESULT_EXPECT_FAILED(1999, 64, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	if (!some)
		STD_OPTION_UNWRAP_FAILED(1999, 73);
	if (!txn_base_Base_decode(read.bytes.ptr, read.bytes.len, read.addresses.ptr, read.addresses.len, &decoded,
							  &error))
		STD_RESULT_EXPECT_FAILED(2000, 63, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(2000, 9, txn_base_Base_PartialEq_eq(&decoded, &empty), &decoded, txn_base_Base_Debug_fmt,
				  &empty, txn_base_Base_Debug_fmt);
	txn_base_Base_drop(&decoded);
	repo_BaseRecord_drop(&read);
	host_shared_Held_drop(&held);
	if (!txn_state_State_Pool_reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(2002, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2002, 9, freed, "State::reclaim(io).unwrap()");
	txn_state_tests_keys_are(2003, 9, io, NULL, 0);
	again = txn_state_tests_made(2004, 82, io, &repo, &reading, &s);
	txn_state_tests_filled(2005, 9, host_shared_Held_record(&again), 1, TXN_STATE_CLEAN);
	txn_state_tests_based(2006, 9, 2006, 44, io, &again, true, &empty);
	host_shared_Held_drop(&again);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&empty);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

/* txn_state_tests_filled_once is the holds case's call: the state lent is filled once and clean. */
static bool
txn_state_tests_filled_once(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
							HostError *error)
{
	txn_state_tests_filled(2068, 13, host_shared_Held_record(held), 1, TXN_STATE_CLEAN);
	return true;
}

/* txn_state_tests_based_t is the holds case's last call's captures: the base record the state lent reads. */
typedef struct txn_state_tests_based_t
{
	bool		some;
	TxnBaseBase base;
} txn_state_tests_based_t;

/* txn_state_tests_based_closure is the holds case's last call: the base record of the state lent. */
static bool
txn_state_tests_based_closure(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
							  HostError *error)
{
	txn_state_tests_based_t *c = captures;

	return txn_state_base_record(io, held, &c->some, &c->base, error);
}

/* txn_state_tests_keyless returns what a backend's find of a keyless table's state gives held, for the cases that hold states. */
static TxnStateFound
txn_state_tests_keyless(HostIoIo *io, const HostStorage *storage)
{
	TxnStateFind find;
	HostError	error;
	TxnStateFound found = {.tag = TXN_STATE_FOUND_HELD};

	if (!txn_state_holds(io, storage, TXN_STATE_PERSISTENCE_PERMANENT, &find, &error))
		STD_RESULT_EXPECT_FAILED(2093, 58, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (find.tag == TXN_STATE_FIND_HELD)
		found.u.held = find.u.held;
	else
	{
		found.tag = TXN_STATE_FOUND_MADE;
		found.u.made.f0 = find.u.missing;
		found.u.made.f1.key.tag = TXN_BASE_KEY_NONE;
		found.u.made.f1.table = 1;
		found.u.made.f1.has_index = false;
	}
	return found;
}

/* txn_state_tests_held_filled_once lends the holds case's call the state of a relation, as held's unwrap does at its place. */
static void
txn_state_tests_held_filled_once(uint32 line, uint32 col, HostIoIo *io, const RepoRepo *repo,
								 const HostSharedReader *reading, uint32 relation)
{
	HostStorage storage = txn_state_tests_storage(9212, relation);
	TxnStateFound found = txn_state_tests_keyless(io, &storage);
	HostError	error;

	if (!txn_state_held_call(io, repo, reading, &found, txn_state_tests_filled_once, NULL, NULL, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/*
 * PLAN item 7's find: a backend holds each storage state its top-level
 * transaction finds, once a storage, filled, until that transaction's end, so
 * a reclaim frees none of them meanwhile, and a subtransaction's abort keeps
 * the one it found; after the end a reclaim frees one, and the next
 * transaction finds a state again.
 */
void
test_txn_state_tests_a_backend_holds_its_transactions_states_until_its_end(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo = txn_state_tests_created(io, 9212);
	HostStorage first = txn_state_tests_storage(9212, 1);
	TxnBaseBase full = txn_base_tests_full();
	HostSharedReader reading;
	HostError	error;
	TxnStateFound found;
	txn_state_tests_based_t based = {0};
	StdOptionRef left;
	StdOptionRef right;
	bool		freed;
	static const uint32 relations[] = {1, 1, 2};

	txn_state_tests_put(io, &repo, &first, &full);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(2066, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (int r = 0; r < lengthof(relations); r++)
		txn_state_tests_held_filled_once(2073, 59, io, &repo, &reading, relations[r]);
	txn_state_tests_keys_counted(2075, 9, io, 2);
	simc_begin_owner();
	txn_state_tests_held_filled_once(2078, 55, io, &repo, &reading, 3);
	simc_abort_owner();
	if (!txn_state_State_Pool_reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(2080, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2080, 9, !freed, "!State::reclaim(io).unwrap()");
	txn_state_tests_keys_counted(2081, 9, io, 3);
	simc_abort_owner();
	if (!txn_state_State_Pool_reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(2083, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2083, 9, freed, "State::reclaim(io).unwrap()");
	txn_state_tests_keys_counted(2084, 9, io, 2);
	found = txn_state_tests_keyless(io, &first);
	if (!txn_state_held_call(io, &repo, &reading, &found, txn_state_tests_based_closure, &based, NULL, &error))
		STD_RESULT_EXPECT_FAILED(2086, 91, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = based.some;
	left.some = STD_ITEM(&based.base, txn_base_Base_Debug_fmt);
	right.has = true;
	right.some = STD_ITEM(&full, txn_base_Base_Debug_fmt);
	STD_ASSERT_EQ(2087, 9, based.some && txn_base_Base_PartialEq_eq(&based.base, &full), &left,
				  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (based.some)
		txn_base_Base_drop(&based.base);
	simc_abort_owner();
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

/* txn_state_tests_counted returns the base record the manifest holds for a storage, answering false for none. */
static bool
txn_state_tests_counted(HostIoIo *io, const RepoRepo *repo, const HostStorage *storage, TxnBaseBase *result)
{
	HostSharedReader reading;
	HostError	error;
	ChunksErrorError failed;
	RepoBaseRecord read;
	RepoStorage name = txn_state_in_repository(storage);
	bool		some;

	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(2101, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!repo_Repo_base(repo, io, &reading, &name, &some, &read, &failed))
		STD_RESULT_EXPECT_FAILED(2102, 69, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	host_shared_Reader_drop(&reading);
	if (!some)
		return false;
	if (!txn_base_Base_decode(read.bytes.ptr, read.bytes.len, read.addresses.ptr, read.addresses.len, result, &error))
		STD_RESULT_EXPECT_FAILED(2103, 57, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	repo_BaseRecord_drop(&read);
	return true;
}

/* txn_state_tests_counted_is asserts the base record the manifest holds for a storage, as assert_eq on counted does at its place. */
static void
txn_state_tests_counted_is(uint32 line, uint32 col, HostIoIo *io, const RepoRepo *repo, const HostStorage *storage,
						   bool has_expected, const TxnBaseBase *expected)
{
	TxnBaseBase base;
	bool		some = txn_state_tests_counted(io, repo, storage, &base);
	StdOptionRef left = {some, STD_ITEM(&base, txn_base_Base_Debug_fmt)};
	StdOptionRef right = {has_expected, STD_ITEM(expected, txn_base_Base_Debug_fmt)};

	STD_ASSERT_EQ(line, col, some == has_expected && (!some || txn_base_Base_PartialEq_eq(&base, expected)), &left,
				  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (some)
		txn_base_Base_drop(&base);
}

/* txn_state_tests_filled_counter asserts a state's ready and saved words with its counter, as assert_eq on the pair does at its place. */
static void
txn_state_tests_filled_counter(uint32 line, uint32 col, HostSharedRecord record, uint64 ready, uint64 saved,
							   uint64 counter)
{
	uint64		left[3];
	uint64		right[3] = {ready, saved, counter};
	StdTupleRef left_filled = STD_TUPLE(2, STD_ITEM(&left[0], std_u64_Debug_fmt), STD_ITEM(&left[1], std_u64_Debug_fmt));
	StdTupleRef right_filled = STD_TUPLE(2, STD_ITEM(&right[0], std_u64_Debug_fmt),
										 STD_ITEM(&right[1], std_u64_Debug_fmt));
	StdTupleRef left_tuple = STD_TUPLE(2, STD_ITEM(&left_filled, std_tuple_Debug_fmt),
									   STD_ITEM(&left[2], std_u64_Debug_fmt));
	StdTupleRef right_tuple = STD_TUPLE(2, STD_ITEM(&right_filled, std_tuple_Debug_fmt),
										STD_ITEM(&right[2], std_u64_Debug_fmt));

	left[0] = txn_state_load(record, TXN_STATE_READY);
	left[1] = txn_state_load(record, TXN_STATE_SAVED);
	left[2] = txn_state_load(record, TXN_STATE_COUNTER);
	STD_ASSERT_EQ(line, col, left[0] == right[0] && left[1] == right[1] && left[2] == right[2], &left_tuple,
				  std_tuple_Debug_fmt, &right_tuple, std_tuple_Debug_fmt);
}

/* txn_state_tests_moved is the counter case's move: the counter set as a write would, and the state unsaved. */
static void
txn_state_tests_moved(HostSharedRecord record, uint64 counter)
{
	txn_state_exchange(record, TXN_STATE_COUNTER, txn_state_load(record, TXN_STATE_COUNTER), counter);
	txn_state_exchange(record, TXN_STATE_SAVED, TXN_STATE_CLEAN, 0);
}

/*
 * PLAN item 7's tuple id counter: a make starts the counter at the base's
 * fresh end (bugs row 396); a save that finds a changed state's rows'
 * memtable linked keeps the counter and gathers the base as its fold wrote
 * it (bugs rows 483 and 489);
 * one that finds no memtable and no map linked lets the counter fall back to
 * the next tuple id past every row of the blob's record; and a storage with
 * no base, idle, falls back to (0,1) and turns clean with no record.
 */
void
test_txn_state_tests_a_save_keeps_a_linked_states_counter_and_lets_an_idle_ones_fall_back(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage s = txn_state_tests_storage(9214, 16384);
	HostStorage bare = txn_state_tests_storage(9214, 16385);
	RepoRepo	repo = txn_state_tests_created(io, 9214);
	TxnBaseBase put = txn_base_tests_full();
	TxnBaseBase gathered = txn_base_tests_full();
	HostSharedReader reading;
	HostError	error;
	HostSharedHeld held;
	HostSharedHeld unbased;
	HostSharedRecord record;
	uint64		counter;
	uint64		three = 3;

	put.fresh_end = 3;
	txn_state_tests_put(io, &repo, &s, &put);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(2117, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_state_tests_made(2118, 81, io, &repo, &reading, &s);
	record = host_shared_Held_record(&held);
	counter = txn_state_load(record, TXN_STATE_COUNTER);
	STD_ASSERT_EQ(__LINE__, 9, counter == 3, &counter, std_u64_Debug_fmt, &three, std_u64_Debug_fmt);
	txn_state_tests_moved(record, 2 << 16 | 4);
	txn_state_exchange(record, TXN_STATE_MEMTABLE_WORD_INDEX, 0, 1);
	txn_state_tests_saved(io, &repo, 9214, txn_state_tests_nothing, NULL);
	gathered.fresh_end = 3;
	txn_state_tests_filled_counter(2130, 9, record, 1, TXN_STATE_CLEAN, 2 << 16 | 4);
	txn_state_tests_counted_is(2131, 9, io, &repo, &s, true, &gathered);
	txn_state_tests_based(2132, 9, 2132, 43, io, &held, true, &gathered);
	txn_state_exchange(record, TXN_STATE_MEMTABLE_WORD_INDEX, 1, 0);
	txn_state_tests_moved(record, 3 << 16 | 1);
	txn_state_tests_saved(io, &repo, 9214, txn_state_tests_nothing, NULL);
	txn_state_tests_filled_counter(2137, 9, record, 1, TXN_STATE_CLEAN, 3);
	txn_state_tests_counted_is(2138, 9, io, &repo, &s, true, &gathered);
	txn_state_tests_based(2139, 9, 2139, 43, io, &held, true, &gathered);
	unbased = txn_state_tests_made(2140, 87, io, &repo, &reading, &bare);
	record = host_shared_Held_record(&unbased);
	txn_state_exchange(record, TXN_STATE_COUNTER, txn_state_load(record, TXN_STATE_COUNTER), 9);
	txn_state_exchange(record, TXN_STATE_SAVED, TXN_STATE_CLEAN, 0);
	txn_state_tests_saved(io, &repo, 9214, txn_state_tests_nothing, NULL);
	txn_state_tests_filled_counter(2145, 9, record, 1, TXN_STATE_CLEAN, 1);
	txn_state_tests_counted_is(2146, 9, io, &repo, &bare, false, NULL);
	host_shared_Held_drop(&unbased);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&gathered);
	txn_base_Base_drop(&put);
	simc_TestIo_drop(&test_io);
}

/* txn_state_tests_word_of returns the atomic of the fall-back case's word: the free space map's for 0, the tuple id table's otherwise. */
static size_t
txn_state_tests_word_of(size_t word)
{
	return word == 0 ? TXN_STATE_FREE_WORD_INDEX : TXN_STATE_TUPLES_WORD_INDEX;
}

/* txn_state_tests_linked links the fall-back case's word. */
static uint64
txn_state_tests_linked(HostSharedRecord record, size_t word)
{
	return txn_state_exchange(record, txn_state_tests_word_of(word), 0, 1);
}

/* txn_state_tests_cleared clears the fall-back case's word. */
static uint64
txn_state_tests_cleared(HostSharedRecord record, size_t word)
{
	return txn_state_exchange(record, txn_state_tests_word_of(word), 1, 0);
}

/* txn_state_tests_counter_is asserts a state's counter, as assert_eq on its load with the word's message does at its place. */
static void
txn_state_tests_counter_is(uint32 line, uint32 col, HostSharedRecord record, uint64 expected, size_t word)
{
	uint64		counter = txn_state_load(record, TXN_STATE_COUNTER);
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("word "), STD_FMT_ARG(0)};
	StdFmtArg	args[1];
	StdFmtArguments message;

	if (counter == expected)
		return;
	args[0].value = &word;
	args[0].fmt = std_usize_Display_fmt;
	message.pieces = pieces;
	message.pieces_len = lengthof(pieces);
	message.args = args;
	std_assert_failed(RUST_FILE, line, col, "==", &counter, std_u64_Debug_fmt, &expected, std_u64_Debug_fmt,
					  &message);
}

/*
 * PLAN item 7's fall-back words: a state whose free space map's or tuple
 * id table's word is linked with its rows' memtable word clear, as an exit
 * between a prepare and its finish leaves it, keeps its counter through a
 * save, and falls back once the word is clear.
 */
void
test_txn_state_tests_a_linked_free_space_map_or_tuple_id_table_keeps_the_counter_from_falling_back(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage s = txn_state_tests_storage(9215, 16384);
	RepoRepo	repo = txn_state_tests_created(io, 9215);
	TxnBaseBase put = txn_base_tests_full();
	HostSharedReader reading;
	HostError	error;
	HostSharedHeld held;
	HostSharedRecord record;

	put.fresh_end = 3;
	txn_state_tests_put(io, &repo, &s, &put);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(2159, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_state_tests_made(2160, 81, io, &repo, &reading, &s);
	record = host_shared_Held_record(&held);
	for (size_t word = 0; word < 2; word++)
	{
		txn_state_exchange(record, TXN_STATE_COUNTER, txn_state_load(record, TXN_STATE_COUNTER), 2 << 16 | 4);
		txn_state_exchange(record, TXN_STATE_SAVED, TXN_STATE_CLEAN, 0);
		(void) txn_state_tests_linked(record, word);
		txn_state_tests_saved(io, &repo, 9215, txn_state_tests_nothing, NULL);
		txn_state_tests_counter_is(2175, 13, record, 2 << 16 | 4, word);
		(void) txn_state_tests_cleared(record, word);
		txn_state_exchange(record, TXN_STATE_SAVED, TXN_STATE_CLEAN, 0);
		txn_state_tests_saved(io, &repo, 9215, txn_state_tests_nothing, NULL);
		txn_state_tests_counter_is(2179, 13, record, 3, word);
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&put);
	simc_TestIo_drop(&test_io);
}

/*
 * The fall-back words and spills: a state whose rows', free space map's, and
 * tuple id table's words are clear but whose spills are linked, or one of
 * whose spills is pending, keeps its counter through a save, since a spill
 * holds versions of rows and takes of places past its base's; it falls back
 * once both are clear.
 */
void
test_txn_state_tests_a_state_with_spills_keeps_the_counter_from_falling_back(void)
{
	static const size_t words[] = {TXN_STATE_SPILLS_INDEX, TXN_STATE_SPILLS_PENDING};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage s = txn_state_tests_storage(9216, 16384);
	RepoRepo	repo = txn_state_tests_created(io, 9216);
	TxnBaseBase put = txn_base_tests_full();
	HostSharedReader reading;
	HostError	error;
	HostSharedHeld held;
	HostSharedRecord record;

	put.fresh_end = 3;
	txn_state_tests_put(io, &repo, &s, &put);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_state_tests_made(__LINE__, 9, io, &repo, &reading, &s);
	record = host_shared_Held_record(&held);
	for (size_t k = 0; k < lengthof(words); k++)
	{
		txn_state_exchange(record, TXN_STATE_COUNTER, txn_state_load(record, TXN_STATE_COUNTER), 2 << 16 | 4);
		txn_state_exchange(record, TXN_STATE_SAVED, TXN_STATE_CLEAN, 0);
		txn_state_exchange(record, words[k], 0, 1);
		txn_state_tests_saved(io, &repo, 9216, txn_state_tests_nothing, NULL);
		txn_state_tests_counter_is(__LINE__, 9, record, 2 << 16 | 4, k);
		txn_state_exchange(record, words[k], 1, 0);
		txn_state_exchange(record, TXN_STATE_SAVED, TXN_STATE_CLEAN, 0);
		txn_state_tests_saved(io, &repo, 9216, txn_state_tests_nothing, NULL);
		txn_state_tests_counter_is(__LINE__, 9, record, 3, k);
	}
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&put);
	simc_TestIo_drop(&test_io);
}

/* txn_state_tests_column is the key case's key column of an attribute number. */
static TxnBaseKeyColumn
txn_state_tests_column(uint16 attnum)
{
	TxnBaseKeyColumn column = {.attnum = attnum, .class = 1978, .collation = 100, .typid = 23};

	return column;
}

/* txn_state_tests_columns returns the key of one column of an attribute number, as Key::Columns of its vec! does. */
static TxnBaseKey
txn_state_tests_columns(uint16 attnum)
{
	TxnBaseKey	key = {.tag = TXN_BASE_KEY_COLUMNS};

	std_vec_with_capacity_Checked(&key.u.columns, 1, sizeof(TxnBaseKeyColumn), _Alignof(TxnBaseKeyColumn));
	STD_VEC_AT(&key.u.columns, TxnBaseKeyColumn, 0) = txn_state_tests_column(attnum);
	key.u.columns.len = 1;
	return key;
}

/* txn_state_tests_made_keyed makes or finds a storage's state with a make of a key's clone and a table, as state's unwrap does at its place. */
static HostSharedHeld
txn_state_tests_made_keyed(uint32 line, uint32 col, HostIoIo *io, const RepoRepo *repo,
						   const HostSharedReader *reading, const HostStorage *storage, const TxnBaseKey *key,
						   uint32 table)
{
	TxnStateMade made = {.table = table};
	HostSharedHeld held;
	HostError	error;

	txn_base_Key_Clone_clone(key, &made.key);
	if (!txn_state_state(io, repo, reading, storage, TXN_STATE_PERSISTENCE_PERMANENT, true, &made, &held, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return held;
}

/*
 * PLAN's field 11, the storage's key: a backend's make of a storage with no
 * saved base fills its state with an empty base record holding the key and
 * the table it gives, and leaves the state clean, so a save after nothing but
 * that make writes no base; a state redo made with no base takes a backend's
 * key the same way, clean; a saved base whose key is unknown takes a
 * backend's key and is saved again with it; and a known key stays as it is
 * under a make that gives another.
 */
void
test_txn_state_tests_a_backend_records_its_key_in_a_state_and_redo_takes_none(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo = txn_state_tests_created(io, 9230);
	HostStorage first = txn_state_tests_storage(9230, 1);
	HostStorage second = txn_state_tests_storage(9230, 2);
	HostStorage third = txn_state_tests_storage(9230, 3);
	TxnBaseKey	keyed = txn_state_tests_columns(2);
	TxnBaseKey	other = txn_state_tests_columns(1);
	TxnBaseKey	key;
	TxnBaseKey	none = {.tag = TXN_BASE_KEY_NONE};
	TxnBaseBase empty;
	TxnBaseBase unknown_base;
	TxnBaseBase recorded;
	HostSharedReader reading;
	HostError	error;
	HostSharedHeld made;
	HostSharedHeld redone;
	HostSharedHeld unknown;
	HostSharedHeld dropped;
	uint64		saved;
	uint64		clean = TXN_STATE_CLEAN;

	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(2193, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	made = txn_state_tests_made_keyed(2204, 10, io, &repo, &reading, &first, &keyed, 16400);
	txn_base_Key_Clone_clone(&keyed, &key);
	empty = txn_base_Base_empty(&key, 16400, false, 0);
	txn_state_tests_based(2206, 9, 2206, 43, io, &made, true, &empty);
	txn_state_tests_filled(2207, 9, host_shared_Held_record(&made), 1, TXN_STATE_CLEAN);
	txn_state_tests_saved(io, &repo, 9230, txn_state_tests_nothing, NULL);
	txn_state_tests_counted_is(2209, 9, io, &repo, &first, false, NULL);
	redone = txn_state_tests_made(2210, 98, io, &repo, &reading, &second);
	txn_state_tests_based(2211, 9, 2211, 45, io, &redone, false, NULL);
	dropped = txn_state_tests_made_keyed(2221, 14, io, &repo, &reading, &second, &keyed, 16400);
	host_shared_Held_drop(&dropped);
	txn_state_tests_based(2223, 9, 2223, 45, io, &redone, true, &empty);
	txn_state_tests_filled(2224, 9, host_shared_Held_record(&redone), 1, TXN_STATE_CLEAN);
	unknown_base = txn_base_Base_empty(&none, 16401, false, 0);
	unknown_base.key.tag = TXN_BASE_KEY_UNKNOWN;
	txn_state_tests_put(io, &repo, &third, &unknown_base);
	unknown = txn_state_tests_made(2231, 99, io, &repo, &reading, &third);
	dropped = txn_state_tests_made_keyed(2241, 14, io, &repo, &reading, &third, &keyed, 16400);
	host_shared_Held_drop(&dropped);
	txn_base_Key_Clone_clone(&keyed, &key);
	recorded = txn_base_Base_empty(&key, 16401, false, 0);
	txn_state_tests_based(2244, 9, 2244, 46, io, &unknown, true, &recorded);
	saved = txn_state_load(host_shared_Held_record(&unknown), TXN_STATE_SAVED);
	if (saved == TXN_STATE_CLEAN)
		std_assert_failed(RUST_FILE, 2245, 9, "!=", &saved, std_u64_Debug_fmt, &clean, std_u64_Debug_fmt,
						  NULL);
	txn_state_tests_saved(io, &repo, 9230, txn_state_tests_nothing, NULL);
	txn_state_tests_counted_is(2247, 9, io, &repo, &third, true, &recorded);
	dropped = txn_state_tests_made_keyed(2258, 14, io, &repo, &reading, &third, &other, 16401);
	host_shared_Held_drop(&dropped);
	txn_state_tests_based(2260, 9, 2260, 46, io, &unknown, true, &recorded);
	host_shared_Held_drop(&unknown);
	host_shared_Held_drop(&redone);
	host_shared_Held_drop(&made);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&recorded);
	txn_base_Base_drop(&unknown_base);
	txn_base_Base_drop(&empty);
	txn_base_Key_drop(&other);
	txn_base_Key_drop(&keyed);
	simc_TestIo_drop(&test_io);
}

/* txn_state_tests_folded is the install case's base: the full record folded to a point. */
static TxnBaseBase
txn_state_tests_folded(uint64 folded)
{
	TxnBaseBase base = txn_base_tests_full();

	base.folded = folded;
	base.read = folded;
	return base;
}

/* txn_state_tests_newest_is asserts a state's newest base, as assert_eq on newest's unwrap does at their places. */
static void
txn_state_tests_newest_is(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, HostIoIo *io,
						  const HostSharedHeld *held, const TxnBaseBase *expected)
{
	TxnBaseBase base;
	bool		some;
	HostError	error;
	StdOptionRef left;
	StdOptionRef right = {true, STD_ITEM(expected, txn_base_Base_Debug_fmt)};

	if (!txn_state_newest(io, held, &some, &base, &error))
		STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = some;
	left.some = STD_ITEM(&base, txn_base_Base_Debug_fmt);
	STD_ASSERT_EQ(line, col, some && txn_base_Base_PartialEq_eq(&base, expected), &left, std_option_Debug_fmt, &right,
				  std_option_Debug_fmt);
	if (some)
		txn_base_Base_drop(&base);
}

/* txn_state_tests_installs installs a base in a state, as installed's unwrap does at its place, answering whether it did. */
static bool
txn_state_tests_installs(uint32 line, uint32 col, HostIoIo *io, const HostSharedHeld *held, uint64 read,
						 const TxnBaseBase *base)
{
	bool		installed;
	HostError	error;

	if (!txn_state_installed(HOST_IO_IO_TAKES_SHORT(io), held, read, base, NULL, NULL, &installed, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return installed;
}

/*
 * The fold text's 3.3: an install makes the fold's base the newest,
 * dropping the one it replaced, an install whose generation moved since its
 * read installs nothing, and an install marks the state unsaved.
 */
void
test_txn_state_tests_an_install_replaces_the_newest_and_refuses_a_moved_generation(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo = txn_state_tests_created(io, 9401);
	HostStorage s = txn_state_tests_storage(9401, 16384);
	TxnBaseBase full = txn_base_tests_full();
	TxnBaseBase ten = txn_state_tests_folded(10);
	TxnBaseBase twenty = txn_state_tests_folded(20);
	TxnBaseBase thirty = txn_state_tests_folded(30);
	HostSharedReader reading;
	HostError	error;
	HostSharedHeld held;
	uint64		read;

	txn_state_tests_put(io, &repo, &s, &full);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(2272, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_state_tests_made(2273, 81, io, &repo, &reading, &s);
	read = txn_state_generation(host_shared_Held_record(&held));
	STD_ASSERT(2276, 9, txn_state_tests_installs(2276, 57, io, &held, read, &ten),
			   "installed(io, &held, read, &folded(10)).unwrap()");
	txn_state_tests_newest_is(2277, 9, 2277, 38, io, &held, &ten);
	txn_state_tests_filled(2278, 9, host_shared_Held_record(&held), 1, 0);
	STD_ASSERT(2279, 9, !txn_state_tests_installs(2279, 58, io, &held, read, &thirty),
			   "!installed(io, &held, read, &folded(30)).unwrap()");
	txn_state_tests_newest_is(2280, 9, 2280, 38, io, &held, &ten);
	STD_ASSERT(2281, 9, txn_state_tests_installs(2281, 61, io, &held, read + 1, &twenty),
			   "installed(io, &held, read + 1, &folded(20)).unwrap()");
	txn_state_tests_newest_is(2282, 9, 2282, 38, io, &held, &twenty);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&thirty);
	txn_base_Base_drop(&twenty);
	txn_base_Base_drop(&ten);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 80's key race: a backend that records a storage's key after a
 * fold read its base with the key unknown keeps it across the fold's install,
 * which takes the key of the newest base it replaces.
 */
void
test_txn_state_tests_an_install_keeps_a_key_recorded_after_its_fold_read_the_base(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo = txn_state_tests_created(io, 9402);
	HostStorage s = txn_state_tests_storage(9402, 16384);
	TxnBaseKey	none = {.tag = TXN_BASE_KEY_NONE};
	TxnBaseKey	key = {.tag = TXN_BASE_KEY_COLUMNS};
	TxnBaseKeyColumn column = {.attnum = 1, .class = 1978, .typid = 23};
	TxnBaseBase unknown = txn_base_Base_empty(&none, 16401, false, 0);
	TxnBaseBase folding;
	TxnBaseBase based;
	TxnStateMade made = {.table = 16401};
	HostSharedReader reading;
	HostError	error;
	HostSharedHeld held;
	HostSharedHeld dropped;
	StdOptionRef left;
	StdOptionRef right;
	uint64		read;
	bool		some;
	bool		installed;

	unknown.key.tag = TXN_BASE_KEY_UNKNOWN;
	txn_state_tests_put(io, &repo, &s, &unknown);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(2294, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_state_tests_made(2295, 81, io, &repo, &reading, &s);
	read = txn_state_generation(host_shared_Held_record(&held));
	if (!txn_state_newest(io, &held, &some, &folding, &error))
		STD_RESULT_EXPECT_FAILED(2297, 47, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!some)
		STD_PANIC(2297, 63, "the state holds no base");
	std_vec_with_capacity_Checked(&key.u.columns, 1, sizeof(TxnBaseKeyColumn), _Alignof(TxnBaseKeyColumn));
	STD_VEC_AT(&key.u.columns, TxnBaseKeyColumn, 0) = column;
	key.u.columns.len = 1;
	txn_base_Key_Clone_clone(&key, &made.key);
	if (!txn_state_state(io, &repo, &reading, &s, TXN_STATE_PERSISTENCE_PERMANENT, true, &made, &dropped, &error))
		STD_RESULT_EXPECT_FAILED(2306, 81, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	host_shared_Held_drop(&dropped);
	folding.folded = 10;
	if (!txn_state_installed(HOST_IO_IO_TAKES_SHORT(io), &held, read, &folding, NULL, NULL, &installed, &error))
		STD_RESULT_EXPECT_FAILED(2307, 77, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2307, 9, installed, "installed(io, &held, read, &Base { folded: 10, ..folding }).unwrap()");
	txn_base_Base_drop(&folding);
	if (!txn_state_base_record(io, &held, &some, &based, &error))
		STD_RESULT_EXPECT_FAILED(2308, 43, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = some;
	left.some = STD_ITEM(&based.key, txn_base_Key_Debug_fmt);
	right.has = true;
	right.some = STD_ITEM(&key, txn_base_Key_Debug_fmt);
	STD_ASSERT_EQ(2308, 9, some && txn_base_Key_PartialEq_eq(&based.key, &key), &left,
				  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (some)
		txn_base_Base_drop(&based);
	txn_base_Key_drop(&key);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&unknown);
	simc_TestIo_drop(&test_io);
}

/*
 * The fold text's draft 12: a snapshot reads a storage's newest base, and
 * one whose xmin doesn't pass the base's maxFolded, which the storage's
 * horizon keeps from existing, is an internal error.
 */
void
test_txn_state_tests_a_snapshot_reads_the_newest_base_past_its_max_folded(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo = txn_state_tests_created(io, 9419);
	HostStorage s = txn_state_tests_storage(9419, 16384);
	TxnBaseBase folded = txn_base_tests_full();
	TxnBaseBase base;
	HostSharedReader reading;
	HostError	error;
	HostSharedHeld held;
	HostXid		past_xmin = {701};
	HostXid		at_xmin = {700};
	HostXid		xmax = {702};
	HostCommand command = {0};
	SimcTaken	past;
	SimcTaken	at;
	HostSnapshot lent;
	StdOptionRef left;
	StdOptionRef right = {true, STD_ITEM(&folded, txn_base_Base_Debug_fmt)};
	bool		some;
	bool		refused;

	folded.max_folded = 700;
	txn_state_tests_put(io, &repo, &s, &folded);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(2321, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_state_tests_made(2322, 81, io, &repo, &reading, &s);
	past = simc_historic(past_xmin, xmax, NULL, 0, NULL, 0, command);
	lent = simc_Taken_lent(&past);
	if (!txn_state_readable(io, &held, &lent, &some, &base, &error))
		STD_RESULT_EXPECT_FAILED(2324, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = some;
	left.some = STD_ITEM(&base, txn_base_Base_Debug_fmt);
	STD_ASSERT_EQ(2324, 9, some && txn_base_Base_PartialEq_eq(&base, &folded), &left, std_option_Debug_fmt,
				  &right, std_option_Debug_fmt);
	if (some)
		txn_base_Base_drop(&base);
	at = simc_historic(at_xmin, xmax, NULL, 0, NULL, 0, command);
	lent = simc_Taken_lent(&at);
	refused = !txn_state_readable(io, &held, &lent, &some, &base, &error);
	if (refused)
	{
		refused = memcmp(host_Error_sqlstate(&error), "XX000", 5) == 0;
		host_Error_drop(&error);
	}
	else if (some)
		txn_base_Base_drop(&base);
	STD_ASSERT(2326, 9, refused,
			   "readable(io, &held, &at.lent()).is_err_and(|error| error.sqlstate() == b\"XX000\")");
	simc_Taken_drop(&at);
	simc_Taken_drop(&past);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&folded);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 143's reclaim: a clean state whose storage's spills' manifest
 * lists a file is kept by a reclaim and marked due for a fold, since only a
 * fold or an abort takes its spills out, and once it lists none a reclaim
 * frees it.
 */
void
test_txn_state_tests_a_reclaim_keeps_a_state_whose_manifest_lists_a_spill(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage s = txn_state_tests_storage(9213, 16384);
	RepoRepo	repo = txn_state_tests_created(io, 9213);
	TxnBaseBase full = txn_base_tests_full();
	HostSharedReader reading;
	HostError	error;
	HostSharedHeld held;
	uint64		key = txn_state_key(&s);
	uint64		due;
	uint64		one = 1;
	bool		freed;

	txn_state_tests_put(io, &repo, &s, &full);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(2339, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	held = txn_state_tests_made(2340, 81, io, &repo, &reading, &s);
	txn_state_exchange(host_shared_Held_record(&held), TXN_STATE_SPILLS_INDEX, 0, 1);
	host_shared_Held_drop(&held);
	if (!txn_state_State_Pool_reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(2344, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2344, 9, !freed, "!State::reclaim(io).unwrap()");
	txn_state_tests_keys_are(2345, 9, io, &key, 1);
	held = txn_state_tests_made(2346, 81, io, &repo, &reading, &s);
	due = txn_state_load(host_shared_Held_record(&held), TXN_STATE_FOLD_DUE_INDEX);
	STD_ASSERT_EQ(2347, 9, due == 1, &due, std_u64_Debug_fmt, &one, std_u64_Debug_fmt);
	txn_state_exchange(host_shared_Held_record(&held), TXN_STATE_SPILLS_INDEX, 1, 0);
	host_shared_Held_drop(&held);
	if (!txn_state_State_Pool_reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(2350, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2350, 9, freed, "State::reclaim(io).unwrap()");
	txn_state_tests_keys_are(2351, 9, io, NULL, 0);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 340's case (RevB 4's F1): a pool full of idle unsaved states,
 * each holding a running writer's version and no base, which no save turns
 * clean, gives a further make the pool's error: the reclaim's save makes no
 * progress, so the make doesn't look again.
 */
void
test_txn_state_tests_a_make_while_running_writers_hold_every_unsaved_state_gets_the_pools_error(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostError	error;
	HostXid		sub;
	HostXid		top;
	uint32		relation = 1;

	simc_end(false);
	repo = txn_state_tests_created(io, 9211);
	if (!host_shared_read(io, &reading, &error) || !host_current_writer(HOST_IO_IO_IO_DOORS(io), &sub, &top, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (;; relation++)
	{
		HostStorage s = txn_state_tests_storage(9211, relation);
		HostSharedHeld held;
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "row", 3);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(sub.f0), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;

		if (!txn_state_state(io, &repo, &reading, &s, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held, &error))
		{
			txn_memtable_Op_drop(&op);
			txn_state_tests_full_pool(&error);
			host_Error_drop(&error);
			break;
		}
		op.u.put.tuple.f0 = 1;
		op.u.put.heap_bytes = 32;
		if (!txn_memtable_table_push(io, host_shared_Held_record(&held), (const uint8 *) "k", 1, &change, &handle,
									 &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
		txn_state_unsaved(host_shared_Held_record(&held));
		host_shared_Held_drop(&held);
	}
	STD_ASSERT(__LINE__, 0, relation > 1, "the pool held a state");
	simc_end(true);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	simc_TestIo_drop(&test_io);
}


/* txn_state_tests_keying_t is the key case's rechain logging's captures: what a backend's make records the key with. */
typedef struct txn_state_tests_keying_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostStorage *storage;
	TxnStateMade *made;
} txn_state_tests_keying_t;

/* txn_state_tests_keying is the key case's rechain logging: a backend's make records the state's key meanwhile. */
static bool
txn_state_tests_keying(void *captures, HostIoIoDoors io, HostLsn reached, HostLsn *end, HostError *error)
{
	txn_state_tests_keying_t *keying = captures;
	HostSharedHeld made;

	(void) io;
	(void) reached;
	if (!txn_state_state(keying->io, keying->repo, keying->reading, keying->storage, TXN_STATE_PERSISTENCE_PERMANENT,
						 true, keying->made, &made, error))
		return false;
	host_shared_Held_drop(&made);
	*end = 0;
	return true;
}

/*
 * Bugs row 350's window (RevB 4's R1): a backend that records a storage's
 * key while a rechain logs its record, with the state's lock released, keeps
 * it across the rechain's replace, which takes the key of the newest base it
 * replaces.
 */
void
test_txn_state_tests_a_rechain_keeps_a_key_recorded_while_it_logs(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo = txn_state_tests_created(io, 9403);
	HostStorage s = txn_state_tests_storage(9403, 16384);
	TxnBaseKey	none = {.tag = TXN_BASE_KEY_NONE};
	TxnBaseKey	key = {.tag = TXN_BASE_KEY_COLUMNS};
	TxnBaseKeyColumn column = {.attnum = 1, .class = 1978, .typid = 23};
	TxnBaseBase unknown = txn_base_Base_empty(&none, 16401, false, 0);
	TxnBaseBase chaining;
	TxnBaseBase based;
	TxnStateMade made = {.table = 16401};
	txn_state_tests_keying_t keying = {.io = io, .repo = &repo, .storage = &s, .made = &made};
	HostSharedReader reading;
	HostError	error;
	HostSharedHeld held;
	StdOptionRef left;
	StdOptionRef right;
	uint64		read;
	bool		some;
	bool		replaced;

	unknown.key.tag = TXN_BASE_KEY_UNKNOWN;
	txn_state_tests_put(io, &repo, &s, &unknown);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	keying.reading = &reading;
	held = txn_state_tests_made(__LINE__, 0, io, &repo, &reading, &s);
	read = txn_state_generation(host_shared_Held_record(&held));
	if (!txn_state_newest(io, &held, &some, &chaining, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!some)
		STD_PANIC(__LINE__, 0, "the state holds no base");
	std_vec_with_capacity_Checked(&key.u.columns, 1, sizeof(TxnBaseKeyColumn), _Alignof(TxnBaseKeyColumn));
	STD_VEC_AT(&key.u.columns, TxnBaseKeyColumn, 0) = column;
	key.u.columns.len = 1;
	txn_base_Key_Clone_clone(&key, &made.key);
	if (!txn_state_rechained(HOST_IO_IO_TAKES_SHORT(io), &held, read, &chaining, txn_state_tests_keying, &keying,
							 &replaced, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, replaced, "the rechain replaced the newest base");
	txn_base_Base_drop(&chaining);
	if (!txn_state_base_record(io, &held, &some, &based, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = some;
	left.some = STD_ITEM(&based.key, txn_base_Key_Debug_fmt);
	right.has = true;
	right.some = STD_ITEM(&key, txn_base_Key_Debug_fmt);
	STD_ASSERT_EQ(__LINE__, 0, some && txn_base_Key_PartialEq_eq(&based.key, &key), &left,
				  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (some)
		txn_base_Base_drop(&based);
	txn_base_Key_drop(&key);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&unknown);
	simc_TestIo_drop(&test_io);
}

/* TXN_STATE_TESTS_UNWRAP_FAILED is a failed unwrap of a host result at a line of a case of bugs row 405's. */
#define TXN_STATE_TESTS_UNWRAP_FAILED(line, error) \
	STD_RESULT_EXPECT_FAILED((line), 0, "called `Result::unwrap()` on an `Err` value", (error), host_Error_Debug_fmt)

/* txn_state_tests_indexed answers the location the states' pool's index holds for a storage, at a case's line. */
static bool
txn_state_tests_indexed(uint32 line, HostIoIo *io, const HostStorage *storage, uint64 *location)
{
	uint8		key[TXN_STATE_INDEX_KEY];
	HostError	error;
	bool		found;

	txn_state_index_key(storage, key);
	if (!host_shared_index_find(HOST_SHARED_IO_WITHIN(io), &TXN_STATE_STATE_POOL_OPS, key, &found, location, &error))
		TXN_STATE_TESTS_UNWRAP_FAILED(line, &error);
	return found;
}

/* txn_state_tests_found_some answers whether a find without a make reaches a storage's state, at a case's line. */
static bool
txn_state_tests_found_some(uint32 line, HostIoIo *io, const HostStorage *storage)
{
	HostSharedHeld held;
	HostError	error;
	bool		some;

	if (!txn_state_found(io, storage, &some, &held, &error))
		TXN_STATE_TESTS_UNWRAP_FAILED(line, &error);
	if (some)
		host_shared_Held_drop(&held);
	return some;
}

/*
 * Bugs row 405: a find reaches a state through the states' pool's index
 * alone, not a sweep of the pool, so a published state whose entry is taken
 * out isn't found, and is again once its entry is back.
 */
void
test_txn_state_tests_a_find_reaches_a_state_through_the_index_alone(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage s = txn_state_tests_storage(9507, 16384);
	RepoRepo	repo = txn_state_tests_created(io, 9507);
	TxnBaseBase full = txn_base_tests_full();
	HostSharedReader reading;
	HostSharedHeld held;
	HostError	error;
	uint8		key[TXN_STATE_INDEX_KEY];
	uint64		location = 0;

	txn_state_tests_put(io, &repo, &s, &full);
	if (!host_shared_read(io, &reading, &error) ||
		!txn_state_state(io, &repo, &reading, &s, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held, &error))
		TXN_STATE_TESTS_UNWRAP_FAILED(__LINE__, &error);
	STD_ASSERT(__LINE__, 0, txn_state_tests_indexed(__LINE__, io, &s, &location), "the index holds the state made");
	txn_state_index_key(&s, key);
	if (!host_shared_index_remove(HOST_SHARED_IO_WITHIN(io), &TXN_STATE_STATE_POOL_OPS, key, location, &error))
		TXN_STATE_TESTS_UNWRAP_FAILED(__LINE__, &error);
	STD_ASSERT(__LINE__, 0, !txn_state_tests_found_some(__LINE__, io, &s),
			   "a find reaches no published state the index doesn't hold");
	if (!host_shared_index_put(HOST_SHARED_IO_WITHIN(io), &TXN_STATE_STATE_POOL_OPS, key, location, &error))
		TXN_STATE_TESTS_UNWRAP_FAILED(__LINE__, &error);
	STD_ASSERT(__LINE__, 0, txn_state_tests_found_some(__LINE__, io, &s), "a find reaches the state its entry names");
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 405: an entry of the index that names a slot freed and made again
 * for another storage reaches nothing for its own storage, since the
 * location names the slot's old generation, and the other storage's state is
 * found through its own entry.
 */
void
test_txn_state_tests_a_find_passes_over_a_location_whose_slot_was_freed_and_made_again(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage gone = txn_state_tests_storage(9508, 16384);
	HostStorage made = txn_state_tests_storage(9508, 16385);
	RepoRepo	repo = txn_state_tests_created(io, 9508);
	TxnBaseBase full = txn_base_tests_full();
	HostSharedReader reading;
	HostSharedHeld held;
	HostError	error;
	uint8		key[TXN_STATE_INDEX_KEY];
	uint64		location = 0;
	uint64		unused;

	txn_state_tests_put(io, &repo, &gone, &full);
	txn_state_tests_put(io, &repo, &made, &full);
	if (!host_shared_read(io, &reading, &error) ||
		!txn_state_state(io, &repo, &reading, &gone, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held, &error))
		TXN_STATE_TESTS_UNWRAP_FAILED(__LINE__, &error);
	STD_ASSERT(__LINE__, 0, txn_state_tests_indexed(__LINE__, io, &gone, &location), "the index holds the state made");
	host_shared_Held_drop(&held);
	if (!txn_state_unlinked(io, &gone, &error))
		TXN_STATE_TESTS_UNWRAP_FAILED(__LINE__, &error);
	STD_ASSERT(__LINE__, 0, !txn_state_tests_indexed(__LINE__, io, &gone, &unused),
			   "the unlink took the state out of the index");
	txn_state_index_key(&gone, key);
	if (!host_shared_index_put(HOST_SHARED_IO_WITHIN(io), &TXN_STATE_STATE_POOL_OPS, key, location, &error) ||
		!txn_state_state(io, &repo, &reading, &made, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held, &error))
		TXN_STATE_TESTS_UNWRAP_FAILED(__LINE__, &error);
	STD_ASSERT(__LINE__, 0, !txn_state_tests_found_some(__LINE__, io, &gone),
			   "an entry naming a freed slot reaches nothing");
	STD_ASSERT(__LINE__, 0, txn_state_tests_found_some(__LINE__, io, &made), "the state made again is found");
	if (!host_shared_index_remove(HOST_SHARED_IO_WITHIN(io), &TXN_STATE_STATE_POOL_OPS, key, location, &error))
		TXN_STATE_TESTS_UNWRAP_FAILED(__LINE__, &error);
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

/* txn_state_tests_looked_closure_0 is an actor that finds a storage's state without making one, refusing to find none. */
static bool
txn_state_tests_looked_closure_0(void *captures, HostIoIo *io, HostError *error)
{
	const txn_state_tests_opened_t *c = captures;
	HostSharedHeld held;
	bool		some;

	if (!txn_state_found(io, &c->storage, &some, &held, error))
		return false;
	if (!some)
	{
		static const char text[] = "a lookup found no state right after its publish";

		*error = txn_state_internal(text, strlen(text));
		return false;
	}
	host_shared_Held_drop(&held);
	return true;
}

/*
 * Bugs row 405: a lookup that makes nothing, run right after a state's
 * publish while its maker still holds the makers' lock, finds the state,
 * since the index holds its location before the publish.
 */
void
test_txn_state_tests_a_lookup_right_after_a_states_publish_finds_it(void)
{
	static const char *const names[] = {"maker", "looker"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage s = txn_state_tests_storage(9509, 16384);
	RepoRepo	repo = txn_state_tests_created(io, 9509);
	TxnBaseBase full = txn_base_tests_full();
	SimcRun		run;
	SimcStep	steps[2] = {{"looker", 6, PG_UINT64_MAX}, {"maker", 5, PG_UINT64_MAX}};
	SimcReport	report;
	SimcEnding	expected[2] = {{.tag = SIMC_ENDING_RETURNED}, {.tag = SIMC_ENDING_RETURNED}};
	txn_state_tests_opened_t captures = {.database = 9509, .storage = s};
	SimcActor	actors[2] = {{txn_state_tests_opened_closure_0, &captures},
	{txn_state_tests_looked_closure_0, &captures}};

	txn_state_tests_put(io, &repo, &s, &full);
	run = simc_Run_new(72);
	for (size_t k = 0; k < lengthof(names); k++)
		simc_Run_actor(&run, names[k], strlen(names[k]), actors[k]);
	simc_Run_until(&run, "maker", 5, "doltgres-state-published");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_state_tests_ended(&report, expected, lengthof(expected));
	simc_Report_drop(&report);
	repo_Repo_drop(&repo);
	txn_base_Base_drop(&full);
	simc_TestIo_drop(&test_io);
}

#endif
