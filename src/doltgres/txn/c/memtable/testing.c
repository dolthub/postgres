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
 * The C port of txn's memtable/testing.rs: what the memtable's tests share,
 * a range's stand-in shapes on sim, a push that brings the pages it may
 * need, and a storage's state made in a repository of its own.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#ifdef DOLTGRES_TEST_TXN

#include "doltgres/host/delta.h"
#include "doltgres/host/delta/list.h"
#include "doltgres/host/shared.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/testing.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "memtable" STD_SEP "testing.rs"

/* TXN_MEMTABLE_TESTING_RANGES is Ranges, the atomic of a Holder record that names its range's record. */
#define TXN_MEMTABLE_TESTING_RANGES 0

/* txn_memtable_testing_Holder_count returns the pool's count: four. */
static size_t
txn_memtable_testing_Holder_count(const HostSharedLimits *limits)
{
	return 4;
}

/* txn_memtable_testing_Holder_reclaim reclaims nothing. */
static bool
txn_memtable_testing_Holder_reclaim(HostIoIo *io, bool *result, HostError *error)
{
	*result = false;
	return true;
}

/* TXN_MEMTABLE_TESTING_HOLDER_POOL_OPS is Holder's Shape and Pool: a test's pooled shape, whose atomic names its range's record. */
const HostSharedPoolOps TXN_MEMTABLE_TESTING_HOLDER_POOL_OPS = {
	.shape = {.id = UINT64CONST(0x7e571de0), .atomics = 1},
	.name = {"holder", 6},
	.count = txn_memtable_testing_Holder_count,
	.reclaim = txn_memtable_testing_Holder_reclaim,
};

/* txn_memtable_testing_Keys_count returns the aged pool's count: four. */
static size_t
txn_memtable_testing_Keys_count(const HostSharedLimits *limits)
{
	return 4;
}

/* TXN_MEMTABLE_TESTING_KEYS_AGED_OPS is Keys' Shape and Aged: a test's aged shape whose records index keys and hold their versions. */
const HostSharedAgedOps TXN_MEMTABLE_TESTING_KEYS_AGED_OPS = {
	.shape = {.id = UINT64CONST(0x7e571de1), .locks = 1, .atomics = 5, .lists = 1},
	.name = {"keys", 4},
	.count = txn_memtable_testing_Keys_count,
};

/* TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS is Keys' Keyed: its lock KeysLock, Shut, Head, Counted, Summed, and Newer. */
const TxnMemtableIndexKeyedOps TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS = {
	.closes = {.shape = &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS.shape, .lock = TXN_MEMTABLE_TESTING_KEYS_LOCK, .closed = 1},
	.first = 0,
	.count = 2,
	.bytes = 3,
	.newest = 4,
};

/* txn_memtable_testing_failed returns a test's own error. */
HostError
txn_memtable_testing_failed(const char *what)
{
	StdString	message;

	std_string_from_Checked(&message, what, strlen(what));
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* txn_memtable_testing_keys makes a Keys record, linked into a pooled record, and returns both. */
bool
txn_memtable_testing_keys(HostIoIo *io, HostSharedHeld *result_0, uint64 *result_1, HostError *error)
{
	HostSharedOwned owned;
	uint64		location;
	bool		some;
	bool		made_ok;
	HostDeltaMade made;
	HostDeltaFull full;
	HostSharedNamed ranges;
	HostDeltaNames names = {&HOST_DELTA_NAMED_NAMES_OPS, &ranges};
	HostDeltaWith with;

	if (!host_shared_make(&TXN_MEMTABLE_TESTING_HOLDER_POOL_OPS, io, &owned, error))
		return false;
	location = host_shared_Owned_publish(&owned);
	if (!host_shared_open(HOST_SHARED_IO_WITHIN(io), &TXN_MEMTABLE_TESTING_HOLDER_POOL_OPS, location, &some, result_0,
						  error))
		return false;
	if (!some)
	{
		*error = txn_memtable_testing_failed("a published record didn't open");
		return false;
	}
	if (!host_delta_make(io, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS, &made_ok, &made, &full, error))
	{
		host_shared_Held_drop(result_0);
		return false;
	}
	if (!made_ok)
	{
		host_shared_Held_drop(result_0);
		*error = txn_memtable_testing_failed("an aged pool refused a record it has room for");
		return false;
	}
	ranges = host_shared_Record_names(host_shared_Held_record(result_0), TXN_MEMTABLE_TESTING_RANGES);
	with = host_delta_With_Default_default();
	*result_1 = host_delta_Made_link(&made, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS, names, &with);
	return true;
}

/* txn_memtable_testing_writer returns a top-level transaction's writer at command zero. */
TxnMemtableVersionWriter
txn_memtable_testing_writer(uint64 xid)
{
	TxnMemtableVersionWriter writer = {.xid = xid};

	return writer;
}

/* txn_memtable_testing_put returns the op that puts a value with no addresses. */
TxnMemtableOp
txn_memtable_testing_put(const uint8 *value, size_t value_len)
{
	TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_PUT};

	std_slice_to_vec_Checked(&op.u.put.value, value, value_len, 1, 1);
	std_vec_new(&op.u.put.addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_vec_new(&op.u.put.values, sizeof(uint32), _Alignof(uint32));
	op.u.put.tuple.f0 = 1;
	op.u.put.heap_bytes = 32;
	return op;
}

/* txn_memtable_testing_pushed_frame_0_t is pushed's state across its pad: the holds, the guards, and the pages, while live. */
typedef struct txn_memtable_testing_pushed_frame_0_t
{
	HostIoIo   *io;
	uint64		keys;
	const uint8 *key;
	size_t		key_len;
	const TxnMemtableOp *op;
	HostDeltaPageSize size;
	TxnMemtableVersionPushed *result;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedGuard guard;
	bool		guard_live;
	StdVec		sizes;
	bool		sizes_live;
	StdVec		pages;
	bool		pages_live;
	HostDeltaOwned fresh;
	bool		has_fresh;
	StdVec		buffer;
	bool		buffer_live;
} txn_memtable_testing_pushed_frame_0_t;

/* txn_memtable_testing_pushed_pad_0 is pushed's pad: the guard, the hold, the fresh page, the pages, and the buffers, while live. */
static void
txn_memtable_testing_pushed_pad_0(void *frame_)
{
	txn_memtable_testing_pushed_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	if (frame->buffer_live)
	{
		frame->buffer_live = false;
		std_vec_free_Checked(&frame->buffer, 1, 1);
	}
	if (frame->has_fresh)
	{
		frame->has_fresh = false;
		host_delta_Owned_drop(&frame->fresh);
	}
	if (frame->pages_live)
	{
		frame->pages_live = false;
		for (size_t k = 0, k_len = frame->pages.len; k < k_len; k++)
			host_delta_Owned_drop(&STD_VEC_AT(&frame->pages, HostDeltaOwned, k));
		std_vec_free_Checked(&frame->pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	}
	if (frame->sizes_live)
	{
		frame->sizes_live = false;
		std_vec_free_Checked(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	}
}

/* txn_memtable_testing_locked takes the Keys record's lock under a fresh hold, exclusively or shared. */
static bool
txn_memtable_testing_locked(txn_memtable_testing_pushed_frame_0_t *frame, bool exclusive)
{
	HostSharedRecord record;
	bool		locked;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return false;
	frame->hold_live = true;
	if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS, frame->keys, &record,
								frame->error))
		return false;
	locked = exclusive ?
		host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_TESTING_KEYS_LOCK,
								&frame->guard, frame->error) :
		host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_TESTING_KEYS_LOCK,
									   &frame->guard, frame->error);
	frame->guard_live = locked;
	return locked;
}

/* txn_memtable_testing_pushed_body_0 is pushed's body. */
static void
txn_memtable_testing_pushed_body_0(void *frame_)
{
	txn_memtable_testing_pushed_frame_0_t *frame = frame_;
	HostDeltaListListView list;
	size_t		wants;
	bool		allocated;
	HostDeltaFull full;
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7), .op = frame->op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableIndexKeyedOpen open;
	HostDeltaPageSize list_page = HOST_DELTA_LIST_PAGE;

	if (!txn_memtable_testing_locked(frame, false))
	{
		txn_memtable_testing_pushed_pad_0(frame);
		return;
	}
	list = host_delta_list_Guard_list(&frame->guard, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS.shape,
									  TXN_MEMTABLE_INDEX_PAGES);
	wants = host_delta_list_ListView_wants(&list);
	txn_memtable_testing_pushed_pad_0(frame);
	std_vec_with_capacity_Checked(&frame->sizes, std_wrapping_add_usize(wants, 1), sizeof(HostDeltaPageSize),
								  _Alignof(HostDeltaPageSize));
	frame->sizes_live = true;
	for (size_t k = 0; k < wants; k++)
		std_vec_push_Checked(&frame->sizes, &list_page, sizeof(list_page), _Alignof(HostDeltaPageSize));
	std_vec_push_Checked(&frame->sizes, &frame->size, sizeof(frame->size), _Alignof(HostDeltaPageSize));
	if (!host_delta_alloc(frame->io, frame->sizes.ptr, frame->sizes.len, &allocated, &frame->pages, &full,
						  frame->error))
	{
		txn_memtable_testing_pushed_pad_0(frame);
		return;
	}
	if (!allocated)
	{
		*frame->error = txn_memtable_testing_failed("the delta refused a test's pages");
		txn_memtable_testing_pushed_pad_0(frame);
		return;
	}
	frame->pages_live = true;
	frame->has_fresh = std_vec_pop(&frame->pages, &frame->fresh, sizeof(HostDeltaOwned));
	if (!txn_memtable_testing_locked(frame, true))
	{
		txn_memtable_testing_pushed_pad_0(frame);
		return;
	}
	if (!txn_memtable_index_keyed_open(&TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS, &frame->guard, &open))
	{
		*frame->error = txn_memtable_testing_failed("a test's record is closed");
		txn_memtable_testing_pushed_pad_0(frame);
		return;
	}
	std_vec_new(&frame->buffer, 1, 1);
	frame->buffer_live = true;
	frame->ok = txn_memtable_version_push(&open, &frame->hold, frame->key,
										  frame->key_len, &change, &frame->has_fresh, &frame->fresh, &frame->pages,
										  &frame->buffer, frame->result, frame->error);
	txn_memtable_testing_pushed_pad_0(frame);
}

/*
 * txn_memtable_testing_pushed pushes a version under the record's lock,
 * bringing a fresh page of a size and the spares the list wants, which it
 * reads under the lock shared first.
 */
bool
txn_memtable_testing_pushed(HostIoIo *io, uint64 keys, const uint8 *key, size_t key_len, const TxnMemtableOp *op,
							HostDeltaPageSize size, TxnMemtableVersionPushed *result, HostError *error)
{
	txn_memtable_testing_pushed_frame_0_t frame = {.io = io, .keys = keys, .key = key, .key_len = key_len, .op = op,
	.size = size, .result = result, .error = error};

	std_landing(&frame, txn_memtable_testing_pushed_body_0, txn_memtable_testing_pushed_pad_0);
	return frame.ok;
}

/* txn_memtable_testing_numbered returns a test's key of a number, which orders as the number does. */
void
txn_memtable_testing_numbered(uint64 number, StdVec *result)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("key "), {NULL, 0, 0, {.sign_aware_zero_pad = true, .fill = ' ',
	.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 5}}};
	StdFmtArg	args[] = {{&number, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, 2, args};

	std_fmt_format_Checked(result, &arguments);
}

/* txn_memtable_testing_storage returns a test's storage in pg_default, by its database. */
HostStorage
txn_memtable_testing_storage(uint32 database)
{
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = database, .relation = 16384};

	return storage;
}

/* txn_memtable_testing_stated returns a handle on a storage's state, filled from its repository, which it opens. */
bool
txn_memtable_testing_stated(HostIoIo *io, uint32 database, HostSharedHeld *result, HostError *error)
{
	HostStorage storage = txn_memtable_testing_storage(database);

	return txn_memtable_testing_held(io, &storage, result, error);
}

/* txn_memtable_testing_failure returns a repository's failure as a test's own error, its text the failure's display. */
static HostError
txn_memtable_testing_failure(ChunksErrorError *failure)
{
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{failure, chunks_error_Error_Display_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	chunks_error_Error_drop(failure);
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/*
 * txn_memtable_testing_held returns a handle on the state of any storage of
 * a test's database, filled from the database's repository, which it opens.
 */
bool
txn_memtable_testing_held(HostIoIo *io, const HostStorage *storage, HostSharedHeld *result, HostError *error)
{
	RepoRepo	repo;
	ChunksErrorError failure;
	HostSharedReader reading;
	bool		ok;

	if (storage->database == 0)
		std_option_unwrap_failed(RUST_FILE, 209, 65);
	if (!repo_Repo_open(io, storage->database, HOST_PG_DEFAULT, &repo, &failure))
	{
		*error = txn_memtable_testing_failure(&failure);
		return false;
	}
	if (!host_shared_read(io, &reading, error))
	{
		repo_Repo_drop(&repo);
		return false;
	}
	ok = txn_state_state(io, &repo, &reading, storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, result, error);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return ok;
}

/* txn_memtable_testing_created makes a database's repository and returns a handle on its storage's state. */
HostSharedHeld
txn_memtable_testing_created(HostIoIo *io, uint32 database)
{
	RepoRepo	repo;
	ChunksErrorError failure;
	HostSharedHeld held;
	HostError	error;

	if (database == 0)
		std_option_unwrap_failed(RUST_FILE, 217, 48);
	if (!repo_Repo_create(io, database, HOST_PG_DEFAULT, &repo, &failure))
		STD_RESULT_EXPECT_FAILED(217, 76, "called `Result::unwrap()` on an `Err` value", &failure,
								 chunks_error_Error_Debug_fmt);
	repo_Repo_drop(&repo);
	if (!txn_memtable_testing_stated(io, database, &held, &error))
		STD_RESULT_EXPECT_FAILED(218, 26, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return held;
}

#endif							/* DOLTGRES_TEST_TXN */
