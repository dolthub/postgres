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
 * The C port of txn's written.rs: the keys a backend's top-level transaction
 * wrote to, kept in the backend's memory for the transaction's life, capped,
 * and pruned at its abort or a subtransaction's, after Postgres records the
 * abort, so that the prune removes exactly the versions whose writer the
 * commit log calls aborted. The list's drop at its resource owner's release
 * forgets the backend's handle, so no later transaction inherits it.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/local.h"
#include "doltgres/host/owned.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/num.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/written.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "written.rs"

/* TxnWrittenTouched is Touched: a key a transaction wrote to, its storage, its tree, and the key. */
typedef struct TxnWrittenTouched
{
	HostStorage storage;
	TxnWrittenOf of;
	/* key is a Vec<u8>. */
	StdVec		key;
} TxnWrittenTouched;

/* txn_written_Mark_t is an element of Written's marks: a subtransaction's id and where its keys begin. */
typedef struct txn_written_Mark_t
{
	uint32		f0;
	size_t		f1;
} txn_written_Mark_t;

/* txn_written_Bytes_t is an element of Written's bytes: a storage's tree and the bytes written to it since its last spill. */
typedef struct txn_written_Bytes_t
{
	HostStorage f0;
	TxnWrittenOf f1;
	uint64		f2;
} txn_written_Bytes_t;

/* txn_written_Touched_drop drops a Vec<Touched>'s keys and its buffer. */
static void
txn_written_Touched_drop(StdVec *touched)
{
	for (size_t k = 0; k < touched->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(touched, TxnWrittenTouched, k).key, 1, 1);
	std_vec_free_Checked(touched, sizeof(TxnWrittenTouched), _Alignof(TxnWrittenTouched));
}

/* txn_written_Storages_free frees a Vec<Storage>. */
static void
txn_written_Storages_free(StdVec *storages)
{
	std_vec_free_Checked(storages, sizeof(HostStorage), _Alignof(HostStorage));
}

/*
 * TXN_WRITTEN_CURRENT is CURRENT: the handle of the backend's list, its slot
 * and its generation, with a generation of zero when it has none; its
 * BackendLocal header and the process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	StdAtomicU64 process[2];
}			TXN_WRITTEN_CURRENT = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* TXN_WRITTEN_WRITTEN is WRITTEN: the backend's list, one for its top-level transaction once it writes. */
HostOwnedTable TXN_WRITTEN_WRITTEN = HOST_OWNED_TABLE_NEW;

/* txn_written_Written_Owned_table is Written's Owned table: WRITTEN. */
static HostOwnedTable *
txn_written_Written_Owned_table(void)
{
	return &TXN_WRITTEN_WRITTEN;
}

/* txn_written_Written_drop forgets the backend's handle on the list, so its next transaction makes its own, then drops its lists. */
static void
txn_written_Written_drop(void *record)
{
	TxnWrittenWritten *written = record;
	HostLocalLocal current;

	if (host_local_BackendLocal_existing(&TXN_WRITTEN_CURRENT.local, &current))
	{
		std_AtomicU64_store_relaxed(&((StdAtomicU64 *) host_local_Local_Deref_deref(&current,
																					&TXN_STATE_CURRENT_TYPE))[1], 0);
		host_local_Local_drop(&current, &TXN_STATE_CURRENT_TYPE);
	}
	txn_written_Touched_drop(&written->touched);
	std_vec_free_Checked(&written->marks, sizeof(txn_written_Mark_t), _Alignof(txn_written_Mark_t));
	std_vec_free_Checked(&written->bytes, sizeof(txn_written_Bytes_t), _Alignof(txn_written_Bytes_t));
	txn_written_Storages_free(&written->spills);
}

/* Written's Table, host's owned Table<Written>, with its kind's glue. */
#define HOST_OWNED_SCOPE extern
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX txn_written_Written
#define HOST_OWNED_RS TxnWrittenWritten
#define HOST_OWNED_R TxnWrittenWritten
#define HOST_OWNED_R_DROP txn_written_Written_drop
#define HOST_OWNED_KIND 2
#define HOST_OWNED_TABLE_FN txn_written_Written_Owned_table
#include "doltgres/host/owned_template.h"

/* Written's Table's with at (), for the calls that give nothing back. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX txn_written_Written
#define HOST_OWNED_RS TxnWrittenWritten
#define HOST_OWNED_R TxnWrittenWritten
#define HOST_OWNED_R_DROP txn_written_Written_drop
#define HOST_OWNED_KIND 2
#define HOST_OWNED_TABLE_FN txn_written_Written_Owned_table
#define HOST_OWNED_WITH_TS Unit
#define HOST_OWNED_WITH_UNIT
#include "doltgres/host/owned_template.h"

/* Written's Table's with at u64, for touched and spilled. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX txn_written_Written
#define HOST_OWNED_RS TxnWrittenWritten
#define HOST_OWNED_R TxnWrittenWritten
#define HOST_OWNED_R_DROP txn_written_Written_drop
#define HOST_OWNED_KIND 2
#define HOST_OWNED_TABLE_FN txn_written_Written_Owned_table
#define HOST_OWNED_WITH_TS U64
#define HOST_OWNED_WITH_T uint64
#include "doltgres/host/owned_template.h"

/* txn_written_Largest_t is largest's call's result: whether the transaction wrote any bytes, and its largest tree's. */
typedef struct txn_written_Largest_t
{
	bool		has;
	txn_TupleStorageOfU64U64_t largest;
} txn_written_Largest_t;

/* Written's Table's with at Option<(Storage, Of, u64, u64)>, for largest. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX txn_written_Written
#define HOST_OWNED_RS TxnWrittenWritten
#define HOST_OWNED_R TxnWrittenWritten
#define HOST_OWNED_R_DROP txn_written_Written_drop
#define HOST_OWNED_KIND 2
#define HOST_OWNED_TABLE_FN txn_written_Written_Owned_table
#define HOST_OWNED_WITH_TS Largest
#define HOST_OWNED_WITH_T txn_written_Largest_t
#include "doltgres/host/owned_template.h"

/* txn_written_Keys_t is keys' call's result: whether the list knows its keys, and the keys by tree with the top-level xid. */
typedef struct txn_written_Keys_t
{
	bool		has;
	txn_TupleU64VecTupleStorageOfVecVecU8_t keys;
} txn_written_Keys_t;

/* txn_written_Trees_drop drops a Vec<(Storage, Of, Vec<Vec<u8>>)>: each tree's keys, then the buffer. */
static void
txn_written_Trees_drop(StdVec *trees)
{
	for (size_t k = 0; k < trees->len; k++)
	{
		StdVec	   *keys = &STD_VEC_AT(trees, txn_TupleStorageOfVecVecU8_t, k).f2;

		for (size_t j = 0; j < keys->len; j++)
			std_vec_free_Checked(&STD_VEC_AT(keys, StdVec, j), 1, 1);
		std_vec_free_Checked(keys, sizeof(StdVec), _Alignof(StdVec));
	}
	std_vec_free_Checked(trees, sizeof(txn_TupleStorageOfVecVecU8_t), _Alignof(txn_TupleStorageOfVecVecU8_t));
}

/* txn_written_Keys_drop drops keys' call's result. */
static void
txn_written_Keys_drop(txn_written_Keys_t *self)
{
	if (self->has)
		txn_written_Trees_drop(&self->keys.f1);
}

/* Written's Table's with at Option<(u64, Vec<(Storage, Of, Vec<Vec<u8>>)>)>, for keys. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX txn_written_Written
#define HOST_OWNED_RS TxnWrittenWritten
#define HOST_OWNED_R TxnWrittenWritten
#define HOST_OWNED_R_DROP txn_written_Written_drop
#define HOST_OWNED_KIND 2
#define HOST_OWNED_TABLE_FN txn_written_Written_Owned_table
#define HOST_OWNED_WITH_TS Keys
#define HOST_OWNED_WITH_T txn_written_Keys_t
#define HOST_OWNED_WITH_T_DROP(place) txn_written_Keys_drop(place)
#include "doltgres/host/owned_template.h"

/* txn_written_Taken_t is aborted's call's result: the top-level xid, its keys, and the storages it spilled, taken from the list. */
typedef struct txn_written_Taken_t
{
	uint64		top;
	/* touched is a Vec<Touched>. */
	StdVec		touched;
	/* spills is a Vec<Storage>. */
	StdVec		spills;
} txn_written_Taken_t;

/* txn_written_Taken_drop drops aborted's call's result. */
static void
txn_written_Taken_drop(txn_written_Taken_t *self)
{
	txn_written_Touched_drop(&self->touched);
	txn_written_Storages_free(&self->spills);
}

/* Written's Table's with at (u64, Vec<Touched>, Vec<Storage>), for aborted. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX txn_written_Written
#define HOST_OWNED_RS TxnWrittenWritten
#define HOST_OWNED_R TxnWrittenWritten
#define HOST_OWNED_R_DROP txn_written_Written_drop
#define HOST_OWNED_KIND 2
#define HOST_OWNED_TABLE_FN txn_written_Written_Owned_table
#define HOST_OWNED_WITH_TS Taken
#define HOST_OWNED_WITH_T txn_written_Taken_t
#define HOST_OWNED_WITH_T_DROP(place) txn_written_Taken_drop(place)
#include "doltgres/host/owned_template.h"

/* Written's Table's with at Vec<Touched>, for sub_aborted. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX txn_written_Written
#define HOST_OWNED_RS TxnWrittenWritten
#define HOST_OWNED_R TxnWrittenWritten
#define HOST_OWNED_R_DROP txn_written_Written_drop
#define HOST_OWNED_KIND 2
#define HOST_OWNED_TABLE_FN txn_written_Written_Owned_table
#define HOST_OWNED_WITH_TS Touched
#define HOST_OWNED_WITH_T StdVec
#define HOST_OWNED_WITH_T_DROP(place) txn_written_Touched_drop(place)
#include "doltgres/host/owned_template.h"

/* txn_written_VecU8_lt is the fn item Vec<u8>'s PartialOrd::lt's FnMut shim, which sort_unstable passes. */
static bool
txn_written_VecU8_lt(void *captures, const void *a, const void *b)
{
	const StdVec *left = a;
	const StdVec *right = b;

	(void) captures;
	return std_bytes_Ord_cmp(left->ptr, left->len, right->ptr, right->len) == STD_ORDERING_LESS;
}

/* The keys' sort of a tree's keys: sort_unstable at Vec<u8>. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS VecU8
#define STD_SORT_T StdVec
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 0
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT txn_written_VecU8_lt
#include "doltgres/std/sort_template.h"

/* txn_written_VecU8_eq is Vec<u8>'s PartialEq, which the keys' dedup takes. */
static bool
txn_written_VecU8_eq(const void *self, const void *other)
{
	const StdVec *left = self;
	const StdVec *right = other;

	return left->len == right->len && memcmp(left->ptr, right->ptr, left->len) == 0;
}

/* txn_written_VecU8_drop is Vec<u8>'s drop, which the keys' dedup runs on each key it removes. */
static void
txn_written_VecU8_drop(void *key)
{
	std_vec_free_Checked(key, 1, 1);
}

/* txn_written_current returns the handle of the backend's list, answering false when its transaction has written nothing. */
static bool
txn_written_current(HostOwnedHandle *handle)
{
	HostLocalLocal current = host_local_BackendLocal_get(&TXN_WRITTEN_CURRENT.local, &TXN_STATE_CURRENT_TYPE);
	StdAtomicU64 *words = (StdAtomicU64 *) host_local_Local_Deref_deref(&current, &TXN_STATE_CURRENT_TYPE);

	handle->generation = std_AtomicU64_load_relaxed(&words[1]);
	handle->slot = (uint32) std_AtomicU64_load_relaxed(&words[0]);
	host_local_Local_drop(&current, &TXN_STATE_CURRENT_TYPE);
	return handle->generation != 0;
}

/* txn_written_listing returns the handle of the backend's list, made for its top-level transaction of an xid when it has none. */
static bool
txn_written_listing(HostIoIo *io, uint64 top, HostOwnedHandle *result, HostError *error)
{
	TxnWrittenWritten empty = {.top = top};
	HostLocalLocal current;
	StdAtomicU64 *words;

	if (txn_written_current(result))
		return true;
	std_vec_new(&empty.touched, sizeof(TxnWrittenTouched), _Alignof(TxnWrittenTouched));
	std_vec_new(&empty.marks, sizeof(txn_written_Mark_t), _Alignof(txn_written_Mark_t));
	std_vec_new(&empty.bytes, sizeof(txn_written_Bytes_t), _Alignof(txn_written_Bytes_t));
	std_vec_new(&empty.spills, sizeof(HostStorage), _Alignof(HostStorage));
	if (!host_owned_TableTxnWrittenWritten_make_in_transaction(&TXN_WRITTEN_WRITTEN, io, empty, result, error))
		return false;
	current = host_local_BackendLocal_get(&TXN_WRITTEN_CURRENT.local, &TXN_STATE_CURRENT_TYPE);
	words = (StdAtomicU64 *) host_local_Local_Deref_deref(&current, &TXN_STATE_CURRENT_TYPE);
	std_AtomicU64_store_relaxed(&words[0], result->slot);
	std_AtomicU64_store_relaxed(&words[1], result->generation);
	host_local_Local_drop(&current, &TXN_STATE_CURRENT_TYPE);
	return true;
}

/* txn_written_touched_t is touched's call's captures: the key and the bytes written with it. */
typedef struct txn_written_touched_t
{
	const HostStorage *storage;
	TxnWrittenOf of;
	const uint8 *key;
	size_t		key_len;
	uint64		bytes;
} txn_written_touched_t;

/* txn_written_touched_call adds the bytes to the tree's count and records the key unless it was just recorded or the list is full. */
static bool
txn_written_touched_call(void *captures, HostIoIo *io, TxnWrittenWritten *written, uint64 *result, HostError *error)
{
	txn_written_touched_t *c = captures;
	TxnWrittenTouched touched = {.storage = *c->storage, .of = c->of};
	uint64		count = 0;

	(void) io;
	if (c->of != TXN_WRITTEN_OF_IDENTITY)
	{
		bool		found = false;

		for (size_t k = 0; k < written->bytes.len && !found; k++)
		{
			txn_written_Bytes_t *at = &STD_VEC_AT(&written->bytes, txn_written_Bytes_t, k);

			if (host_Storage_PartialEq_eq(&at->f0, c->storage) && at->f1 == c->of)
			{
				at->f2 = STD_ADD(u64, at->f2, c->bytes, 140, 17);
				count = at->f2;
				found = true;
			}
		}
		if (!found)
		{
			txn_written_Bytes_t entry = {*c->storage, c->of, c->bytes};

			if (!std_vec_try_reserve(&written->bytes, 1, sizeof(entry), _Alignof(txn_written_Bytes_t)))
			{
				*error = host_Error_out_of_memory();
				return false;
			}
			std_vec_push_Checked(&written->bytes, &entry, sizeof(entry), _Alignof(txn_written_Bytes_t));
			count = c->bytes;
		}
	}
	*result = count;
	if (written->touched.len > 0)
	{
		const TxnWrittenTouched *last = &STD_VEC_AT(&written->touched, TxnWrittenTouched, written->touched.len - 1);

		if (host_Storage_PartialEq_eq(&last->storage, c->storage) && last->of == c->of && last->key.len == c->key_len &&
			memcmp(last->key.ptr, c->key, c->key_len) == 0)
			return true;
	}
	if (written->touched.len >= TXN_WRITTEN_CAP)
	{
		written->overflowed = true;
		return true;
	}
	if (!std_vec_try_reserve(&written->touched, 1, sizeof(TxnWrittenTouched), _Alignof(TxnWrittenTouched)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	std_vec_new(&touched.key, 1, 1);
	std_vec_extend_from_slice_Checked(&touched.key, c->key, c->key_len, 1, 1);
	std_vec_push_Checked(&written->touched, &touched, sizeof(touched), _Alignof(TxnWrittenTouched));
	return true;
}

/*
 * txn_written_touched records a key the backend's transaction, of a
 * top-level xid, writes to in a storage's tree, before the write of a
 * version of a count of bytes, making the transaction's list at its first,
 * and adds the bytes to the tree's count with the key's, answering it; a key
 * just recorded isn't recorded twice, and a list at its cap is marked
 * overflowed instead. A key of a database's identity tree adds no bytes,
 * since that tree lives in the working state's delta and never spills.
 */
bool
txn_written_touched(HostIoIo *io, uint64 top, const HostStorage *storage, TxnWrittenOf of, const uint8 *key,
					size_t key_len, size_t version, uint64 *result, HostError *error)
{
	txn_written_touched_t touched = {.storage = storage, .of = of, .key = key, .key_len = key_len,
	.bytes = (uint64) STD_ADD(usize, version, key_len, 114, 17)};
	HostOwnedHandle handle;

	if (!txn_written_listing(io, top, &handle, error) ||
		(of != TXN_WRITTEN_OF_IDENTITY && !txn_spill_wrote(io, top, touched.bytes, error)))
		return false;
	return host_owned_TableTxnWrittenWritten_withU64(&TXN_WRITTEN_WRITTEN, io, handle, txn_written_touched_call,
													 &touched, NULL, result, error);
}

/* txn_written_largest_call finds the tree the transaction wrote the most bytes to, the last of the most as max_by_key does. */
static bool
txn_written_largest_call(void *captures, HostIoIo *io, TxnWrittenWritten *written, txn_written_Largest_t *result,
						 HostError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	result->has = false;
	for (size_t k = 0; k < written->bytes.len; k++)
	{
		const txn_written_Bytes_t *at = &STD_VEC_AT(&written->bytes, txn_written_Bytes_t, k);

		if (!result->has || at->f2 >= result->largest.f2)
		{
			result->has = true;
			result->largest.f0 = at->f0;
			result->largest.f1 = at->f1;
			result->largest.f2 = at->f2;
			result->largest.f3 = written->top;
		}
	}
	return true;
}

/*
 * txn_written_largest returns the storage's tree to which the backend's
 * transaction wrote the most bytes since its last spill, with that count
 * and the transaction's top-level xid, or nothing for a transaction that
 * wrote none.
 */
bool
txn_written_largest(HostIoIo *io, bool *some, txn_TupleStorageOfU64U64_t *result, HostError *error)
{
	HostOwnedHandle handle;
	txn_written_Largest_t largest;

	*some = false;
	if (!txn_written_current(&handle))
		return true;
	if (!host_owned_TableTxnWrittenWritten_withLargest(&TXN_WRITTEN_WRITTEN, io, handle, txn_written_largest_call,
													   NULL, NULL, &largest, error))
		return false;
	*some = largest.has;
	if (largest.has)
		*result = largest.largest;
	return true;
}

/* txn_written_spilled_t is spilled's call's captures: the storage and its tree. */
typedef struct txn_written_spilled_t
{
	const HostStorage *storage;
	TxnWrittenOf of;
} txn_written_spilled_t;

/* txn_written_spilled_call takes the tree's count out of the list, answering it, and records the storage among those spilled. */
static bool
txn_written_spilled_call(void *captures, HostIoIo *io, TxnWrittenWritten *written, uint64 *result, HostError *error)
{
	txn_written_spilled_t *c = captures;
	size_t		kept = 0;
	bool		listed = false;

	(void) io;
	*result = 0;
	for (size_t k = 0; k < written->bytes.len; k++)
	{
		txn_written_Bytes_t *at = &STD_VEC_AT(&written->bytes, txn_written_Bytes_t, k);

		if (host_Storage_PartialEq_eq(&at->f0, c->storage) && at->f1 == c->of)
			*result = std_add_u64(*result, at->f2, STD_ACCUM_FILE, 206, 1);
	}
	for (size_t k = 0; k < written->bytes.len; k++)
	{
		txn_written_Bytes_t at = STD_VEC_AT(&written->bytes, txn_written_Bytes_t, k);

		if (!(host_Storage_PartialEq_eq(&at.f0, c->storage) && at.f1 == c->of))
			STD_VEC_AT(&written->bytes, txn_written_Bytes_t, kept++) = at;
	}
	written->bytes.len = kept;
	for (size_t k = 0; k < written->spills.len && !listed; k++)
		listed = host_Storage_PartialEq_eq(&STD_VEC_AT(&written->spills, HostStorage, k), c->storage);
	if (listed)
		return true;
	if (!std_vec_try_reserve(&written->spills, 1, sizeof(HostStorage), _Alignof(HostStorage)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(&written->spills, c->storage, sizeof(HostStorage), _Alignof(HostStorage));
	return true;
}

/*
 * txn_written_spilled zeroes the count of a storage's tree after a spill
 * took the transaction's versions out of it, and records the storage among
 * those the transaction spilled.
 */
bool
txn_written_spilled(HostIoIo *io, const HostStorage *storage, TxnWrittenOf of, HostError *error)
{
	txn_written_spilled_t spilled = {.storage = storage, .of = of};
	HostOwnedHandle handle;
	uint64		taken;

	if (!txn_written_current(&handle))
		return true;
	return host_owned_TableTxnWrittenWritten_withU64(&TXN_WRITTEN_WRITTEN, io, handle, txn_written_spilled_call,
													 &spilled, NULL, &taken, error) &&
		txn_spill_unwrote(io, true, taken, error);
}

/* txn_written_keys_frame_0_t is keys' state across its pad: the trees its call builds, while it builds them. */
typedef struct txn_written_keys_frame_0_t
{
	HostIoIo   *io;
	bool	   *some;
	txn_TupleU64VecTupleStorageOfVecVecU8_t *result;
	HostError  *error;
	bool		ok;
	txn_written_Keys_t keys;
	bool		building;
} txn_written_keys_frame_0_t;

/* txn_written_keys_pad_0 is keys' pad: the trees its call was building. */
static void
txn_written_keys_pad_0(void *frame_)
{
	txn_written_keys_frame_0_t *frame = frame_;

	if (frame->building)
	{
		frame->building = false;
		txn_written_Trees_drop(&frame->keys.keys.f1);
	}
}

/*
 * txn_written_keys_call gathers the list's keys by tree, each once in key
 * order, with the top-level xid, or none past the cap, building them while
 * keys' frame marks them so its pad drops them.
 */
static bool
txn_written_keys_call(void *captures, HostIoIo *io, TxnWrittenWritten *written, txn_written_Keys_t *result,
					  HostError *error)
{
	txn_written_keys_frame_0_t *frame = captures;
	StdVec	   *trees = &result->keys.f1;

	(void) io;
	result->has = false;
	if (written->overflowed)
		return true;
	std_vec_new(trees, sizeof(txn_TupleStorageOfVecVecU8_t), _Alignof(txn_TupleStorageOfVecVecU8_t));
	frame->building = true;
	result->has = true;
	for (size_t k = 0; k < written->touched.len; k++)
	{
		const TxnWrittenTouched *touched = &STD_VEC_AT(&written->touched, TxnWrittenTouched, k);
		size_t		at = trees->len;
		StdVec	   *keys;
		StdVec		key;

		for (size_t j = 0; j < trees->len && at == trees->len; j++)
		{
			const txn_TupleStorageOfVecVecU8_t *tree = &STD_VEC_AT(trees, txn_TupleStorageOfVecVecU8_t, j);

			if (host_Storage_PartialEq_eq(&tree->f0, &touched->storage) && tree->f1 == touched->of)
				at = j;
		}
		if (at == trees->len)
		{
			txn_TupleStorageOfVecVecU8_t tree = {touched->storage, touched->of};

			if (!std_vec_try_reserve(trees, 1, sizeof(tree), _Alignof(txn_TupleStorageOfVecVecU8_t)))
			{
				*error = host_Error_out_of_memory();
				txn_written_keys_pad_0(frame);
				return false;
			}
			std_vec_new(&tree.f2, sizeof(StdVec), _Alignof(StdVec));
			std_vec_push_Checked(trees, &tree, sizeof(tree), _Alignof(txn_TupleStorageOfVecVecU8_t));
			at = STD_SUB(usize, trees->len, 1, 207, 21);
		}
		std_index_check(at, trees->len, RUST_FILE, 210, 13);
		keys = &STD_VEC_AT(trees, txn_TupleStorageOfVecVecU8_t, at).f2;
		if (!std_vec_try_reserve(keys, 1, sizeof(StdVec), _Alignof(StdVec)))
		{
			*error = host_Error_out_of_memory();
			txn_written_keys_pad_0(frame);
			return false;
		}
		std_index_check(at, trees->len, RUST_FILE, 211, 13);
		std_vec_new(&key, 1, 1);
		std_vec_extend_from_slice_Checked(&key, touched->key.ptr, touched->key.len, 1, 1);
		std_vec_push_Checked(keys, &key, sizeof(key), _Alignof(StdVec));
	}
	for (size_t k = 0; k < trees->len; k++)
	{
		StdVec	   *keys = &STD_VEC_AT(trees, txn_TupleStorageOfVecVecU8_t, k).f2;

		std_slice_sort_unstable_VecU8(keys->ptr, keys->len);
		std_vec_dedup(keys, sizeof(StdVec), _Alignof(StdVec), txn_written_VecU8_eq, txn_written_VecU8_drop);
	}
	result->keys.f0 = written->top;
	frame->building = false;
	return true;
}

/* txn_written_keys_body_0 is keys' body. */
static void
txn_written_keys_body_0(void *frame_)
{
	txn_written_keys_frame_0_t *frame = frame_;
	HostOwnedHandle handle;

	if (!txn_written_current(&handle))
	{
		*frame->some = true;
		frame->result->f0 = 0;
		std_vec_new(&frame->result->f1, sizeof(txn_TupleStorageOfVecVecU8_t), _Alignof(txn_TupleStorageOfVecVecU8_t));
		frame->ok = true;
		return;
	}
	if (!host_owned_TableTxnWrittenWritten_withKeys(&TXN_WRITTEN_WRITTEN, frame->io, handle, txn_written_keys_call,
													frame, NULL, &frame->keys, frame->error))
		return;
	*frame->some = frame->keys.has;
	if (frame->keys.has)
		*frame->result = frame->keys.keys;
	frame->ok = true;
}

/*
 * txn_written_keys returns the keys the backend's top-level transaction
 * wrote to in each storage's trees, each once, with the transaction's
 * top-level xid, or nothing when the list overflowed its cap, whose keys it
 * doesn't know; a transaction that wrote nothing gives an empty list.
 */
bool
txn_written_keys(HostIoIo *io, bool *some, txn_TupleU64VecTupleStorageOfVecVecU8_t *result, HostError *error)
{
	txn_written_keys_frame_0_t frame = {.io = io, .some = some, .result = result, .error = error};

	std_landing(&frame, txn_written_keys_body_0, txn_written_keys_pad_0);
	return frame.ok;
}

/*
 * txn_written_ended ends the backend's list at its top-level transaction's
 * commit or prepare, its versions kept, and zeroes its published count of
 * unspilled bytes, which the next transaction counts anew.
 */
bool
txn_written_ended(HostIoIo *io, HostError *error)
{
	HostOwnedHandle handle;

	if (!txn_spill_unwrote(io, false, 0, error))
		return false;
	if (!txn_written_current(&handle))
		return true;
	return host_owned_TableTxnWrittenWritten_end(&TXN_WRITTEN_WRITTEN, io, handle, error);
}

/* txn_written_pruned_one prunes one touched key in its tree, under the frame's held state or delta when its home is there. */
static bool
txn_written_pruned_one(HostIoIo *io, const TxnWrittenTouched *touched, bool has_aborting, uint64 aborting,
					   HostSharedHeld *held, bool *held_live, HostError *error)
{
	const TxnMemtableTableTreeOps *tree;
	uint32		repository;
	bool		some;

	if (touched->of == TXN_WRITTEN_OF_ROWS || touched->of == TXN_WRITTEN_OF_VALUES)
	{
		tree = touched->of == TXN_WRITTEN_OF_ROWS ? &TXN_MEMTABLE_TABLE_ROWS_TREE_OPS :
			&TXN_MEMTABLE_MAP_VALUES_TREE_OPS;
		if (!txn_state_found(io, &touched->storage, &some, held, error))
			return false;
	}
	else
	{
		tree = touched->of == TXN_WRITTEN_OF_IDENTITY ? &TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS :
			&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS;
		if (touched->of == TXN_WRITTEN_OF_IDENTITY)
		{
			repository = touched->storage.database;
			if (repository == 0)
				return true;
		}
		else if (!txn_spill_chunks_repository(&touched->storage, &repository))
			return true;
		if (!txn_memtable_branch_find(HOST_SHARED_IO_WITHIN(io), repository, &some, held, error))
			return false;
	}
	if (!some)
		return true;
	*held_live = true;
	if (!txn_memtable_table_pruned(tree, io, host_shared_Held_record(held), touched->key.ptr, touched->key.len,
								   has_aborting, aborting, error))
		return false;
	*held_live = false;
	host_shared_Held_drop(held);
	return true;
}

/* txn_written_taken_frame_0_t is the state of aborted and sub_aborted across their pad: the keys and storages taken from the list, and a held home. */
typedef struct txn_written_taken_frame_0_t
{
	HostIoIo   *io;
	uint32		sub;
	HostError  *error;
	bool		ok;
	txn_written_Taken_t taken;
	bool		taken_live;
	HostSharedHeld held;
	bool		held_live;
} txn_written_taken_frame_0_t;

/* txn_written_taken_pad_0 is the pad of aborted and sub_aborted: the held home, then what they took from the list, while live. */
static void
txn_written_taken_pad_0(void *frame_)
{
	txn_written_taken_frame_0_t *frame = frame_;

	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
	if (frame->taken_live)
	{
		frame->taken_live = false;
		txn_written_Taken_drop(&frame->taken);
	}
}

/*
 * txn_written_pruned prunes each touched key the frame took in its storage's
 * tree, in its database's identity tree, or in its repository's chunk tree,
 * a storage whose state is gone, or a database whose working state's delta
 * is, holding nothing to prune, an aborting top-level transaction's own
 * versions taken out by its xid.
 */
static bool
txn_written_pruned(txn_written_taken_frame_0_t *frame, bool has_aborting, uint64 aborting)
{
	for (size_t k = 0; k < frame->taken.touched.len; k++)
		if (!txn_written_pruned_one(frame->io, &STD_VEC_AT(&frame->taken.touched, TxnWrittenTouched, k), has_aborting,
									aborting, &frame->held, &frame->held_live, frame->error))
			return false;
	return true;
}

/* txn_written_aborted_call takes the top-level xid, the keys, and the storages spilled out of the list. */
static bool
txn_written_aborted_call(void *captures, HostIoIo *io, TxnWrittenWritten *written, txn_written_Taken_t *result,
						 HostError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	result->top = written->top;
	result->touched = written->touched;
	std_vec_new(&written->touched, sizeof(TxnWrittenTouched), _Alignof(TxnWrittenTouched));
	result->spills = written->spills;
	std_vec_new(&written->spills, sizeof(HostStorage), _Alignof(HostStorage));
	return true;
}

/* txn_written_aborted_body_0 is aborted's body. */
static void
txn_written_aborted_body_0(void *frame_)
{
	txn_written_taken_frame_0_t *frame = frame_;
	HostOwnedHandle handle;

	if (!txn_written_current(&handle))
	{
		frame->ok = true;
		return;
	}
	if (!host_owned_TableTxnWrittenWritten_withTaken(&TXN_WRITTEN_WRITTEN, frame->io, handle, txn_written_aborted_call,
													 NULL, NULL, &frame->taken, frame->error))
		return;
	frame->taken_live = true;
	frame->ok = txn_spill_aborted(frame->io, frame->taken.top, frame->taken.spills.ptr, frame->taken.spills.len,
								  frame->error) &&
		txn_written_pruned(frame, true, frame->taken.top) && txn_spill_unwrote(frame->io, false, 0, frame->error) &&
		host_owned_TableTxnWrittenWritten_end(&TXN_WRITTEN_WRITTEN, frame->io, handle, frame->error);
	txn_written_taken_pad_0(frame);
}

/*
 * txn_written_aborted takes back the versions of the backend's aborted
 * top-level transaction: it takes its spills' entries out of the storages
 * it spilled and clears its pending spill, whose files the next checkpoint
 * unlinks, prunes every key its list holds, then ends the list.
 */
bool
txn_written_aborted(HostIoIo *io, HostError *error)
{
	txn_written_taken_frame_0_t frame = {.io = io, .error = error};

	std_landing(&frame, txn_written_aborted_body_0, txn_written_taken_pad_0);
	return frame.ok;
}

/* txn_written_sub_started_call marks where a subtransaction's keys begin. */
static bool
txn_written_sub_started_call(void *captures, HostIoIo *io, TxnWrittenWritten *written, HostError *error)
{
	txn_written_Mark_t mark = {*(const uint32 *) captures, written->touched.len};

	(void) io;
	if (!std_vec_try_reserve(&written->marks, 1, sizeof(mark), _Alignof(txn_written_Mark_t)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(&written->marks, &mark, sizeof(mark), _Alignof(txn_written_Mark_t));
	return true;
}

/*
 * txn_written_sub_started marks where a subtransaction's keys begin in the
 * backend's list, when it has one; a subtransaction that starts before the
 * list is made has no mark, and its abort prunes the list from its start,
 * which takes back nothing that isn't aborted.
 */
bool
txn_written_sub_started(HostIoIo *io, uint32 sub, HostError *error)
{
	HostOwnedHandle handle;

	if (!txn_written_current(&handle))
		return true;
	return host_owned_TableTxnWrittenWritten_withUnit(&TXN_WRITTEN_WRITTEN, io, handle, txn_written_sub_started_call,
													  &sub, NULL, error);
}

/* txn_written_marked finds a subtransaction's newest mark, answering whether the list holds one. */
static bool
txn_written_marked(const TxnWrittenWritten *written, uint32 sub, size_t *at)
{
	for (size_t k = written->marks.len; k > 0; k--)
		if (STD_VEC_AT(&written->marks, txn_written_Mark_t, k - 1).f0 == sub)
		{
			*at = k - 1;
			return true;
		}
	return false;
}

/* txn_written_sub_committed_call drops a committed subtransaction's mark and the marks after it. */
static bool
txn_written_sub_committed_call(void *captures, HostIoIo *io, TxnWrittenWritten *written, HostError *error)
{
	size_t		at;

	(void) io;
	(void) error;
	if (txn_written_marked(written, *(const uint32 *) captures, &at))
		std_vec_truncate(&written->marks, at, sizeof(txn_written_Mark_t), NULL);
	return true;
}

/* txn_written_sub_committed drops a committed subtransaction's mark, its keys staying in its parent's part of the list. */
bool
txn_written_sub_committed(HostIoIo *io, uint32 sub, HostError *error)
{
	HostOwnedHandle handle;

	if (!txn_written_current(&handle))
		return true;
	return host_owned_TableTxnWrittenWritten_withUnit(&TXN_WRITTEN_WRITTEN, io, handle, txn_written_sub_committed_call,
													  &sub, NULL, error);
}

/* txn_written_sub_aborted_call takes the keys recorded since the subtransaction's mark, or since the list's start, and the marks after it. */
static bool
txn_written_sub_aborted_call(void *captures, HostIoIo *io, TxnWrittenWritten *written, StdVec *result,
							 HostError *error)
{
	size_t		start = 0;
	size_t		at;

	(void) io;
	(void) error;
	if (txn_written_marked(written, *(const uint32 *) captures, &at))
	{
		std_index_check(at, written->marks.len, RUST_FILE, 275, 29);
		start = STD_VEC_AT(&written->marks, txn_written_Mark_t, at).f1;
		std_vec_truncate(&written->marks, at, sizeof(txn_written_Mark_t), NULL);
	}
	std_vec_split_off(&written->touched, Min(start, written->touched.len), sizeof(TxnWrittenTouched),
					  _Alignof(TxnWrittenTouched), result, RUST_FILE, 281, 12);
	return true;
}

/* txn_written_sub_aborted_body_0 is sub_aborted's body. */
static void
txn_written_sub_aborted_body_0(void *frame_)
{
	txn_written_taken_frame_0_t *frame = frame_;
	HostOwnedHandle handle;

	if (!txn_written_current(&handle))
	{
		frame->ok = true;
		return;
	}
	std_vec_new(&frame->taken.spills, sizeof(HostStorage), _Alignof(HostStorage));
	if (!host_owned_TableTxnWrittenWritten_withTouched(&TXN_WRITTEN_WRITTEN, frame->io, handle,
													   txn_written_sub_aborted_call, &frame->sub, NULL,
													   &frame->taken.touched, frame->error))
		return;
	frame->taken_live = true;
	frame->ok = txn_written_pruned(frame, false, 0);
	txn_written_taken_pad_0(frame);
}

/*
 * txn_written_sub_aborted takes back the versions of an aborted
 * subtransaction and its children: it prunes the keys recorded since its
 * mark, or since the list's start with no mark, and drops them and the
 * marks after it.
 */
bool
txn_written_sub_aborted(HostIoIo *io, uint32 sub, HostError *error)
{
	txn_written_taken_frame_0_t frame = {.io = io, .sub = sub, .error = error};

	std_landing(&frame, txn_written_sub_aborted_body_0, txn_written_taken_pad_0);
	return frame.ok;
}
