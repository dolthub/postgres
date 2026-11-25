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
 * The C port of txn's memtable/table.rs: a memtable, its first writer, the
 * hand-off from its owner's word to the range that holds a key, taking one
 * record's lock at a time, the prepared push whose finish inside a
 * checkpoint window allocates nothing, and the retirements of a closed
 * range, of a closed memtable, and of a storage's memtables at its unlink.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/delta.h"
#include "doltgres/host/delta/blocks.h"
#include "doltgres/host/delta/list.h"
#include "doltgres/host/delta/page.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/outcome.h"
#include "doltgres/txn/memtable/index.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/pins.h"
#include "doltgres/txn/memtable/range.h"
#include "doltgres/txn/memtable/split.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/tuples.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/runs.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "memtable" STD_SEP "table.rs"

/* txn_memtable_table_internal returns an internal error of the memtable. */
static HostError
txn_memtable_table_internal(const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* txn_memtable_table_Rows_bare marks a storage due for a fold when its maps are linked with no memtable of its rows. */
static void
txn_memtable_table_Rows_bare(HostSharedRecord state)
{
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE, .u.exchange = {0, 1}};

	if (txn_state_mapped(state))
		host_shared_Record_atomic(state, TXN_STATE_FOLD_DUE_INDEX, op);
}

/* TXN_MEMTABLE_TABLE_STATE_OWNER_OPS is State's Owner: a storage's state, whose grew and shrank count its delta's bytes. */
const TxnMemtableTableOwnerOps TXN_MEMTABLE_TABLE_STATE_OWNER_OPS = {
	.grew = txn_state_grew,
	.shrank = txn_state_shrank,
};

/* TXN_MEMTABLE_TABLE_ROWS_TREE_OPS is Rows' Tree: the memtable of a storage's rows, of its State. */
const TxnMemtableTableTreeOps TXN_MEMTABLE_TABLE_ROWS_TREE_OPS = {
	.of = &TXN_MEMTABLE_TABLE_STATE_OWNER_OPS,
	.lock = TXN_STATE_STATE_LOCK_INDEX,
	.word = TXN_STATE_MEMTABLE_WORD_INDEX,
	.pin = TXN_MEMTABLE_PINS_ROW_PIN,
	.counted = true,
	.bare = txn_memtable_table_Rows_bare,
};

/*
 * txn_memtable_table_first_key lends a range's first key, read through its
 * first page's name, which never changes, on a page the hold keeps, so it
 * takes no lock of the range.
 */
bool
txn_memtable_table_first_key(const HostDeltaHold *hold, uint64 range, HostDeltaLent *result, HostError *error)
{
	HostSharedRecord record;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostDeltaPageRef page;

	return host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, range, &record, error) &&
		host_delta_Hold_page(hold,
							 host_delta_page_Page_named(host_shared_Record_atomic(record,
																				  TXN_MEMTABLE_RANGE_RANGE_START,
																				  load)),
							 &page, error) &&
		txn_memtable_index_begun(page, result, error);
}

/*
 * txn_memtable_table_first_key_cmp compares a range's first key with a key,
 * lent and given back at once, answering false on an error.
 */
static bool
txn_memtable_table_first_key_cmp(const HostDeltaHold *hold, uint64 range, const uint8 *key, size_t key_len,
								 StdOrdering *result, HostError *error)
{
	HostDeltaLent first;
	StdBytes	bytes;

	if (!txn_memtable_table_first_key(hold, range, &first, error))
		return false;
	bytes = host_delta_Lent_Deref_deref(&first);
	*result = std_bytes_Ord_cmp(bytes.ptr, bytes.len, key, key_len);
	host_delta_Lent_drop(&first);
	return true;
}

/*
 * txn_memtable_table_search returns the index and the location of a
 * memtable's entry for a key, the last whose range's first key is at or
 * before it, by a binary search of the list read under the memtable's lock;
 * the first entry's first key is the empty key, so every key has one.
 */
bool
txn_memtable_table_search(const HostDeltaListListView *view, const HostDeltaHold *hold, const uint8 *key,
						  size_t key_len, std_TupleU64U64_t *result, HostError *error)
{
	uint64		low = 0;
	uint64		high = host_delta_list_ListView_len(view);

	while (STD_SUB(u64, high, low, 186, 11) > 1)
	{
		uint64		middle = STD_ADD(u64, low,
									 STD_DIV_FOLDED(u64, STD_SUB(u64, high, low, 187, 29), 2),
									 187, 22);
		uint64		range;
		StdOrdering order;

		if (!host_delta_list_ListView_get(view, hold, middle, &range, error) ||
			!txn_memtable_table_first_key_cmp(hold, range, key, key_len, &order, error))
			return false;
		if (order != STD_ORDERING_GREATER)
			low = middle;
		else
			high = middle;
	}
	result->f0 = low;
	return host_delta_list_ListView_get(view, hold, low, &result->f1, error);
}

/* txn_memtable_table_memtable returns the location of a tree's memtable, from its owner's word one past it, or nothing for none. */
bool
txn_memtable_table_memtable(const TxnMemtableTableTreeOps *tree, HostSharedRecord owner, uint64 *result)
{
	HostSharedPast word = host_shared_Record_names_past(owner, tree->word);

	return host_shared_Past_load(&word, result);
}

/* TxnMemtableTableEntryTag is Entry's variants: what a hand-off read of a memtable under its shared lock. */
typedef enum TxnMemtableTableEntryTag
{
	TXN_MEMTABLE_TABLE_ENTRY_RANGE,
	TXN_MEMTABLE_TABLE_ENTRY_CLOSED,
	TXN_MEMTABLE_TABLE_ENTRY_EMPTY,
} TxnMemtableTableEntryTag;

static bool txn_memtable_table_cleared(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
									   HostSharedRecord owner, uint64 location, HostError *error);
static bool txn_memtable_table_emptied(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
									   HostSharedRecord owner, uint64 location, bool *result, HostError *error);

/*
 * txn_memtable_table_entry reads a memtable's entry for a key under its
 * shared lock: it checks the memtable open and finds the key's range,
 * storing the process's pin of the memtable first when it is given one.
 */
static bool
txn_memtable_table_entry(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
						 uint64 location, const uint8 *key, size_t key_len, const HostSharedMine *pin,
						 TxnMemtableTableEntryTag *tag, uint64 *range, HostError *error)
{
	HostSharedRecord memtable;
	HostSharedGuard guard;
	HostDeltaListListView view;
	bool		ok = true;

	if (!host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, location, &memtable, error) ||
		!host_shared_Record_lock_shared(memtable, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_MEMTABLE_LOCK,
										&guard, error))
		return false;
	view = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape, TXN_MEMTABLE_RANGE_RANGES);
	if (txn_memtable_index_closed(&TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS, &guard))
		*tag = TXN_MEMTABLE_TABLE_ENTRY_CLOSED;
	else if (host_delta_list_ListView_is_empty(&view))
		*tag = TXN_MEMTABLE_TABLE_ENTRY_EMPTY;
	else
	{
		std_TupleU64U64_t found;

		ok = txn_memtable_table_search(&view, hold, key, key_len, &found, error);
		if (ok)
		{
			if (pin != NULL)
				txn_memtable_pins_pin(pin, tree->pin, location);
			*tag = TXN_MEMTABLE_TABLE_ENTRY_RANGE;
			*range = found.f1;
		}
	}
	host_shared_Guard_drop(&guard);
	return ok;
}

/*
 * txn_memtable_table_found finds the range for a key from a tree's word
 * under a hold: under the memtable's shared lock it checks the memtable open
 * and copies the key's entry, storing the process's pin of the memtable
 * first when it is given one. It answers nothing for a tree with no
 * memtable, or with an open memtable whose list a pin left empty, and starts
 * over after clearing the word of a closed memtable or closing an empty one
 * no pin names.
 */
bool
txn_memtable_table_found(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
						 HostSharedRecord owner, const uint8 *key, size_t key_len, const HostSharedMine *pin,
						 bool *some, TxnMemtableTableAt *result, HostError *error)
{
	*some = false;
	for (;;)
	{
		uint64		location;
		TxnMemtableTableEntryTag tag;
		uint64		range;
		bool		again;

		if (!txn_memtable_table_memtable(tree, owner, &location))
		{
			if (tree->bare != NULL)
				tree->bare(owner);
			return true;
		}
		if (!txn_memtable_table_entry(tree, io, hold, location, key, key_len, pin, &tag, &range, error))
			return false;
		switch (tag)
		{
			case TXN_MEMTABLE_TABLE_ENTRY_RANGE:
				result->memtable = location;
				result->range = range;
				*some = true;
				return true;
			case TXN_MEMTABLE_TABLE_ENTRY_CLOSED:
				if (!txn_memtable_table_cleared(tree, io, hold, owner, location, error))
					return false;
				break;
			case TXN_MEMTABLE_TABLE_ENTRY_EMPTY:
				if (!txn_memtable_table_emptied(tree, io, hold, owner, location, &again, error))
					return false;
				if (!again)
					return true;
				break;
		}
	}
}

/*
 * txn_memtable_table_handed hands a prepared push off to its key's range and
 * pins the range's memtable: after the hand-off's store of the pin it takes
 * the range's lock shared and checks it closed, so a closer that read no pin
 * under the range's exclusive lock is seen here. It answers nothing when the
 * prepare goes around: the tree has no memtable, its memtable's list is
 * empty, or the range is closed, which it takes out of the list as any
 * finder does.
 */
bool
txn_memtable_table_handed(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
						  const HostSharedMine *mine, HostSharedRecord owner, const uint8 *key, size_t key_len,
						  bool *some, TxnMemtableTableAt *result, HostError *error)
{
	HostSharedRecord range;
	HostSharedGuard guard;
	bool		closed;

	if (!txn_memtable_table_found(tree, io, hold, owner, key, key_len, mine, some, result, error))
		return false;
	if (!*some)
		return true;
	*some = false;
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-memtable-found", error) ||
		!host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, result->range, &range, error) ||
		!host_shared_Record_lock_shared(range, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
										error))
		return false;
	closed = txn_memtable_index_closed(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS.closes, &guard);
	host_shared_Guard_drop(&guard);
	if (closed)
	{
		txn_memtable_pins_unpin(mine, tree->pin);
		return txn_memtable_table_unlisted(tree, io, hold, owner, *result, error);
	}
	*some = true;
	return true;
}

/* txn_memtable_table_Reserve_drop drops a reserve: its fresh page and its spares, given back unpublished, and its buffers. */
static void
txn_memtable_table_Reserve_drop(TxnMemtableTableReserve *self)
{
	if (self->has_fresh)
	{
		self->has_fresh = false;
		host_delta_Owned_drop(&self->fresh);
	}
	std_vec_clear(&self->spare, sizeof(HostDeltaOwned), host_delta_Owned_drop);
	std_vec_free_Checked(&self->spare, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	std_vec_free_Checked(&self->buffer, 1, 1);
	std_vec_free_Checked(&self->value, 1, 1);
}

/*
 * txn_memtable_table_Reserve_push makes a version its key's newest in an
 * open range with the reserve, which always has room, and keeps what the
 * push saw of the range for the settle. A version but a lock's that the
 * reserve's step counts carries the counted mark.
 */
bool
txn_memtable_table_Reserve_push(TxnMemtableTableReserve *self, TxnMemtableIndexKeyedOpen *open,
								const HostDeltaHold *hold,
								const uint8 *key, size_t key_len, const TxnMemtableVersionChange *change,
								TxnMemtableVersionHandle *result, HostError *error)
{
	TxnMemtableIndexFound found;

	return txn_memtable_index_lookup(open, hold, key, key_len, &found, error) &&
		txn_memtable_table_Reserve_push_found(self, open, hold, found, key, key_len, change, result, error);
}

/*
 * txn_memtable_table_Reserve_push_found is Reserve_push with the key's lookup
 * in the same open already made, its Found taken by value as
 * txn_memtable_version_push_found takes it.
 */
bool
txn_memtable_table_Reserve_push_found(TxnMemtableTableReserve *self, TxnMemtableIndexKeyedOpen *open,
									  const HostDeltaHold *hold, TxnMemtableIndexFound found, const uint8 *key,
									  size_t key_len, const TxnMemtableVersionChange *change,
									  TxnMemtableVersionHandle *result, HostError *error)
{
	TxnMemtableVersionPushed pushed;
	HostSharedRecord record = host_shared_Guard_record(open->open.guard);
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostDeltaListListView pages;
	bool		counted = self->counting && change->op->tag != TXN_MEMTABLE_OP_LOCK &&
		change->op->tag != TXN_MEMTABLE_OP_LOCK_ROW;

	if (txn_memtable_version_encoded(&change->writer, change->op, change->shown, change->has_lsn, change->lsn) >
		self->buffer.cap)
	{
		*error = txn_memtable_table_internal("a prepared version is longer than its prepare's record");
		return false;
	}
	if (!txn_memtable_version_push_found(open, hold, found, key, key_len, change, counted,
										 &self->has_fresh, &self->fresh, &self->spare, &self->buffer, &pushed, error))
		return false;
	if (pushed.tag != TXN_MEMTABLE_VERSION_PUSHED_VERSION)
	{
		*error = txn_memtable_table_internal("a prepared push wanted pages");
		return false;
	}
	self->grown = STD_ADD(u64, self->grown, (uint64) self->buffer.len, 312, 9);
	pages = host_delta_list_Guard_list(open->open.guard, open->open.closes->shape, TXN_MEMTABLE_INDEX_PAGES);
	self->has_seen = true;
	self->seen.keys = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_KEYS, load);
	self->seen.bytes = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_BYTES, load);
	self->seen.pages = host_delta_list_ListView_len(&pages);
	self->seen.longest = (uint64) key_len;
	self->counted = self->counted || counted;
	*result = pushed.u.version;
	return true;
}

/* txn_memtable_table_Target_drop drops a target: its reserve. */
static void
txn_memtable_table_Target_drop(void *self)
{
	txn_memtable_table_Reserve_drop(&((TxnMemtableTableTarget *) self)->reserve);
}

/* txn_memtable_table_Prepared_hold lends the prepare's hold, under which a read inside its window's work keeps every version's place from reuse until the work's push. */
const HostDeltaHold *
txn_memtable_table_PreparedState_hold(const TxnMemtableTablePreparedState *self)
{
	return &self->hold;
}

/* txn_memtable_table_Prepared_rows returns the count of row pushes the prepare readied, its first targets. */
static size_t
txn_memtable_table_Prepared_rows(const TxnMemtableTablePreparedState *self)
{
	size_t		rows = 0;

	while (rows < self->aims_len && self->aims[rows].tree == &TXN_MEMTABLE_TABLE_ROWS_TREE_OPS)
		rows = std_wrapping_add_usize(rows, 1);
	return rows;
}

/* txn_memtable_table_Prepared_target returns the index among the prepare's targets of its take of an index among its takes, past its rows'. */
size_t
txn_memtable_table_Prepared_target(const TxnMemtableTablePreparedState *self, size_t take)
{
	return STD_ADD(usize, take, txn_memtable_table_Prepared_rows(self), 365, 9);
}

/* txn_memtable_table_Prepared_aimed returns the state and the tree a prepared target pushes to. */
bool
txn_memtable_table_Prepared_aimed(const TxnMemtableTablePreparedState *self, size_t target,
								  HostSharedRecord *result_0, const TxnMemtableTableTreeOps **result_1,
								  HostError *error)
{
	if (target >= self->aims_len)
	{
		*error = txn_memtable_table_internal("a take names a target the prepare didn't ready");
		return false;
	}
	*result_0 = self->aims[target].owner;
	*result_1 = self->aims[target].tree;
	return true;
}

/* txn_memtable_table_Prepared_tuple lends a tuple id table's finish the hold and the row's storage, and takes the prepare's reservation. */
bool
txn_memtable_table_Prepared_tuple(TxnMemtableTablePreparedState *self, const HostDeltaHold **result_0,
								  HostSharedRecord *result_1, TxnMemtableTableReservation *result_2,
								  HostError *error)
{
	if (!self->has_tuple)
	{
		*error = txn_memtable_table_internal("a finish of a tid names no reservation");
		return false;
	}
	self->has_tuple = false;
	if (self->aims_len == 0)
	{
		*error = txn_memtable_table_internal("a reservation names a prepare with no row");
		return false;
	}
	*result_0 = &self->hold;
	*result_1 = self->aims[0].owner;
	*result_2 = self->tuple;
	return true;
}

/*
 * txn_memtable_table_passed searches a pinned memtable's list for a key's
 * range past every range closed with nothing in its place, an emptied range
 * its next finder hasn't taken out yet, whose keys belong to the range
 * before it; the pin keeps the memtable open and its first range with it.
 */
static bool
txn_memtable_table_passed(HostIoTakesShort io, const HostDeltaHold *hold, uint64 location, const uint8 *key,
						  size_t key_len, uint64 *result, HostError *error)
{
	HostSharedRecord memtable;
	HostSharedGuard guard;
	HostDeltaListListView view;
	std_TupleU64U64_t found;
	uint64		entry;
	bool		ok = false;

	if (!host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, location, &memtable, error) ||
		!host_shared_Record_lock_shared(memtable, io, TXN_MEMTABLE_RANGE_MEMTABLE_LOCK, &guard, error))
		return false;
	view = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape, TXN_MEMTABLE_RANGE_RANGES);
	if (txn_memtable_index_closed(&TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS, &guard) ||
		host_delta_list_ListView_is_empty(&view))
		*error = txn_memtable_table_internal("a pinned memtable has no range");
	else if (txn_memtable_table_search(&view, hold, key, key_len, &found, error))
	{
		for (entry = found.f0;;)
		{
			HostSharedRecord range;
			HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
			HostSharedPast lower;
			uint64		half;
			bool		gone;

			if (!host_delta_list_ListView_get(&view, hold, entry, result, error) ||
				!host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, *result, &range, error))
				break;
			lower = host_shared_Record_names_past(range, TXN_MEMTABLE_RANGE_RANGE_LOWER);
			gone = host_shared_Record_atomic(range, TXN_MEMTABLE_RANGE_RANGE_CLOSED, load) != 0 &&
				!host_shared_Past_load(&lower, &half);
			if (!gone)
			{
				ok = true;
				break;
			}
			if (entry == 0)
			{
				*error = txn_memtable_table_internal("a pinned memtable's first range is closed");
				break;
			}
			entry = STD_SUB(u64, entry, 1, 430, 26);
		}
	}
	host_shared_Guard_drop(&guard);
	return ok;
}

/*
 * txn_memtable_table_opened takes the exclusive lock of the open range that
 * now holds a key, from the range a hand-off found, answering its guard and
 * its open: a closed range's replacement words name its halves, stored
 * before closed and read with no lock, of which the key's is picked by the
 * upper's first key, and a closed range with none sends it to a search of
 * the memtable's list; each step takes one lock at a time, opens only what
 * the hold reaches, and allocates nothing. A range one of kept already
 * holds is answered as shared, its index there, with no lock taken.
 */
static bool
txn_memtable_table_opened(HostIoTakesShort io, const HostDeltaHold *hold, TxnMemtableTableAt *at, const uint8 *key,
						  size_t key_len, const TxnMemtableTableKept *kept, size_t kept_len, size_t *shared,
						  HostSharedGuard *guard, TxnMemtableIndexKeyedOpen *open, HostError *error)
{
	for (;;)
	{
		HostSharedRecord record;
		HostSharedPast lower;
		HostSharedPast upper;
		uint64		lower_at;
		uint64		upper_at;
		bool		has_lower;
		bool		has_upper;

		for (*shared = 0; *shared < kept_len; (*shared)++)
			if (kept[*shared].owns && kept[*shared].range == at->range)
				return true;
		if (!host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at->range, &record, error) ||
			!host_shared_Record_lock(record, io, TXN_MEMTABLE_RANGE_RANGE_LOCK, guard, error))
			return false;
		if (txn_memtable_index_keyed_open(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, open))
			return true;
		lower = host_shared_Record_names_past(record, TXN_MEMTABLE_RANGE_RANGE_LOWER);
		upper = host_shared_Record_names_past(record, TXN_MEMTABLE_RANGE_RANGE_UPPER);
		has_lower = host_shared_Past_load(&lower, &lower_at);
		has_upper = host_shared_Past_load(&upper, &upper_at);
		host_shared_Guard_drop(guard);
		if (has_lower && has_upper)
		{
			HostDeltaLent first;
			StdBytes	bytes;

			if (!txn_memtable_table_first_key(hold, upper_at, &first, error))
				return false;
			bytes = host_delta_Lent_Deref_deref(&first);
			at->range = std_bytes_Ord_cmp(bytes.ptr, bytes.len, key, key_len) != STD_ORDERING_GREATER ? upper_at :
				lower_at;
			host_delta_Lent_drop(&first);
		}
		else if (!txn_memtable_table_passed(io, hold, at->memtable, key, key_len, &at->range, error))
			return false;
	}
}

/* txn_memtable_table_reached runs a step under the exclusive lock of the open range that now holds a key (opened). */
static bool
txn_memtable_table_reached(HostIoTakesShort io, const HostDeltaHold *hold, TxnMemtableTableAt *at, const uint8 *key,
						   size_t key_len, bool (*step) (void *captures, TxnMemtableIndexKeyedOpen *open,
														 HostError *error),
						   void *captures, HostError *error)
{
	HostSharedGuard guard;
	TxnMemtableIndexKeyedOpen open;
	size_t		shared;
	bool		ok;

	if (!txn_memtable_table_opened(io, hold, at, key, key_len, NULL, 0, &shared, &guard, &open, error))
		return false;
	ok = step(captures, &open, error);
	host_shared_Guard_drop(&guard);
	return ok;
}

static bool txn_memtable_table_deferred(HostIoIo *io, bool *result, HostError *error);

/*
 * A memtable's first range's page, which holds the first node alone, is the
 * smallest range page, a quarter of its body words, and that body is one
 * slot of the smallest class (bugs row 338).
 */
StaticAssertDecl(TXN_MEMTABLE_PAGES_FIRST + TXN_MEMTABLE_INDEX_LEAST_WORDS <= TXN_MEMTABLE_PAGES_SMALLEST / 32 &&
				 TXN_MEMTABLE_PAGES_SMALLEST <= HOST_DELTA_PAGE_SLOT_BYTES,
				 "a memtable's first page is one slot of the smallest class");

/* txn_memtable_table_first_size returns the size of a memtable's first range's page. */
static HostDeltaPageSize
txn_memtable_table_first_size(void)
{
	return txn_memtable_pages_size(false, 0, 0, txn_memtable_index_least(NULL, 0).words);
}

/* txn_memtable_table_prepared_frame_0_t is prepared's state across its pad: the sizes, the hold, the hand-offs, the pages, the targets, and inside's closure, while live. */
typedef struct txn_memtable_table_prepared_frame_0_t
{
	HostIoIo   *io;
	/* all and all_len are the caller's aims, which groups take in turn. */
	const TxnMemtableTableAim *all;
	size_t		all_len;
	const TxnMemtableTableAim *aims;
	size_t		aims_len;
	/* pinned is the count of aims from aims whose trees the try pinned. */
	size_t		pinned;
	/* cap is the most aims a group's next try takes, halved by a refusal. */
	size_t		cap;
	bool		(*inside) (void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared, HostError *error);
	void	   *inside_captures;
	void		(*inside_drop) (void *captures);
	bool	   *has_unstored;
	TxnMemtableTableReservation *unstored;

	/*
	 * refused, for a carry's prepare, answers whether the delta's budget
	 * refused it, with its refusal.
	 */
	TxnMemtableTableRefusal *refused;

	/*
	 * answered is whether a group answered the delta's refusal through
	 * refused.
	 */
	bool		answered;
	HostError  *error;
	bool		ok;
	bool		inside_live;
	HostSharedMine mine;
	StdVec		sizes;
	bool		sizes_live;
	StdVec		ats;
	bool		ats_live;
	StdVec		pages;
	size_t		pages_at;
	bool		pages_live;
	TxnMemtableTablePreparedState prepared;
	bool		hold_live;
	bool		targets_live;
	TxnMemtableTableTarget target;
	bool		target_live;

	/*
	 * missing is a Vec<usize> of the aims whose trees a try makes a memtable
	 * for.
	 */
	StdVec		missing;
	bool		missing_live;

	/*
	 * firsts is a Vec<FirstFrame>: the records and first page of each
	 * memtable a try makes, missing's last first, until it is linked.
	 */
	StdVec		firsts;
	bool		firsts_live;
} txn_memtable_table_prepared_frame_0_t;

static bool txn_memtable_table_prepared_firsts(txn_memtable_table_prepared_frame_0_t *frame, bool *refused,
											   HostDeltaFull *full);
static bool txn_memtable_table_prepared_linked(txn_memtable_table_prepared_frame_0_t *frame, bool *some);
static void txn_memtable_table_prepared_firsts_drop(txn_memtable_table_prepared_frame_0_t *frame);

/* txn_memtable_table_prepared_unpinned clears the process's pin of each pinned aim's tree. */
static void
txn_memtable_table_prepared_unpinned(txn_memtable_table_prepared_frame_0_t *frame)
{
	for (size_t k = 0; k < frame->pinned; k++)
		txn_memtable_pins_unpin(&frame->mine, frame->aims[k].tree->pin);
}

/*
 * An aim's pages, its version page, a list page for each level, and its
 * missing memtable's first page, each at most a block, fit the smallest
 * delta_memory's blocks, 1MB's, so a group always takes its first aim (bugs
 * row 338).
 */
StaticAssertDecl((2 + HOST_DELTA_LIST_DEEPEST) * HOST_DELTA_PAGE_BLOCK <= 1024 * 1024,
				 "an aim's pages fit the smallest delta_memory");

/*
 * TXN_MEMTABLE_TABLE_WHOLE aims are one group at every delta_memory: each
 * aim's version page takes at most a block, the list pages, a list page's
 * words to a slot, pack into whole blocks, and so do the missing memtables'
 * first pages, a slot of the smallest class each.
 */
StaticAssertDecl(TXN_MEMTABLE_TABLE_WHOLE +
				 (TXN_MEMTABLE_TABLE_WHOLE * HOST_DELTA_LIST_DEEPEST * HOST_DELTA_LIST_FANOUT * 8 +
				  HOST_DELTA_PAGE_BLOCK - 1) / HOST_DELTA_PAGE_BLOCK +
				 (TXN_MEMTABLE_TABLE_WHOLE * HOST_DELTA_PAGE_SLOT_BYTES + HOST_DELTA_PAGE_BLOCK - 1) /
				 HOST_DELTA_PAGE_BLOCK <= 1024 * 1024 / HOST_DELTA_PAGE_BLOCK,
				 "a prepare of WHOLE aims is one group at the smallest delta_memory");

/*
 * txn_memtable_table_prepared_least returns the fewest aims a group takes:
 * all of a prepare of at most TXN_MEMTABLE_TABLE_WHOLE aims, which never
 * splits, and one of a larger one, whose inside takes groups.
 */
static size_t
txn_memtable_table_prepared_least(const txn_memtable_table_prepared_frame_0_t *frame)
{
	return frame->all_len <= TXN_MEMTABLE_TABLE_WHOLE ? frame->aims_len : 1;
}

/*
 * txn_memtable_table_prepared_fitting returns how many of the try's aims,
 * from its first, its group takes: its least, and then, to its cap, each one
 * more whose pages, a missing memtable's first page with its aim's, their
 * slots counted by class into whole blocks with the
 * group's, fit the budget's blocks no page takes now, so no allocation of a
 * prepare asks more than an empty budget holds, and a large one asks what the
 * budget has free (bugs row 338).
 */
static size_t
txn_memtable_table_prepared_fitting(const txn_memtable_table_prepared_frame_0_t *frame, size_t each,
									HostDeltaPageSize first)
{
	size_t		missing = 0;
	uint64		budgeted = host_delta_budget() / HOST_DELTA_PAGE_BLOCK;
	uint64		used = host_delta_used() / HOST_DELTA_PAGE_BLOCK;
	uint64		free = budgeted > used ? budgeted - used : 0;
	size_t		least = txn_memtable_table_prepared_least(frame);
	uint64		taken[HOST_DELTA_BLOCKS_CLASSES] = {0};

	for (size_t k = 0; k < frame->aims_len; k++)
	{
		uint64		with[HOST_DELTA_BLOCKS_CLASSES];
		uint64		blocks = 0;

		memcpy(with, taken, sizeof(taken));
		for (size_t d = 0; d < each; d++)
		{
			HostDeltaPageSize size = STD_VEC_AT(&frame->sizes, HostDeltaPageSize, k * each + d);
			uint32		class = host_delta_blocks_class(host_delta_page_Size_body(size));

			with[class] += HOST_DELTA_PAGE_SLOT_BYTES << class;
		}
		if (missing < frame->missing.len && STD_VEC_AT(&frame->missing, size_t, missing) == k)
		{
			uint32		class = host_delta_blocks_class(host_delta_page_Size_body(first));

			with[class] += HOST_DELTA_PAGE_SLOT_BYTES << class;
			missing++;
		}
		for (uint32 c = 0; c < HOST_DELTA_BLOCKS_CLASSES; c++)
			blocks += (with[c] + HOST_DELTA_PAGE_BLOCK - 1) / HOST_DELTA_PAGE_BLOCK;
		if (k >= least && (blocks > free || k >= frame->cap))
			return k;
		memcpy(taken, with, sizeof(taken));
	}
	return frame->aims_len;
}

/* txn_memtable_table_prepared_pages_free drops the pages a try allocated that no target took. */
static void
txn_memtable_table_prepared_pages_free(txn_memtable_table_prepared_frame_0_t *frame)
{
	if (frame->pages_live)
	{
		frame->pages_live = false;
		for (size_t k = frame->pages_at; k < frame->pages.len; k++)
			host_delta_Owned_drop(&STD_VEC_AT(&frame->pages, HostDeltaOwned, k));
		std_vec_free_Checked(&frame->pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	}
}

/* txn_memtable_table_prepared_pad_0 is prepared's pad: the targets, the memtables unlinked, the pages, the hold, the hand-offs, the sizes, and inside's closure, while live. */
static void
txn_memtable_table_prepared_pad_0(void *frame_)
{
	txn_memtable_table_prepared_frame_0_t *frame = frame_;

	if (frame->target_live)
	{
		frame->target_live = false;
		txn_memtable_table_Target_drop(&frame->target);
	}
	if (frame->targets_live)
	{
		frame->targets_live = false;
		std_vec_clear(&frame->prepared.targets, sizeof(TxnMemtableTableTarget), txn_memtable_table_Target_drop);
		std_vec_free_Checked(&frame->prepared.targets, sizeof(TxnMemtableTableTarget),
							 _Alignof(TxnMemtableTableTarget));
	}
	txn_memtable_table_prepared_firsts_drop(frame);
	txn_memtable_table_prepared_pages_free(frame);
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->prepared.hold);
	}
	if (frame->ats_live)
	{
		frame->ats_live = false;
		std_vec_free_Checked(&frame->ats, sizeof(TxnMemtableTableAt), _Alignof(TxnMemtableTableAt));
	}
	if (frame->sizes_live)
	{
		frame->sizes_live = false;
		std_vec_free_Checked(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	}
	if (frame->missing_live)
	{
		frame->missing_live = false;
		std_vec_free_Checked(&frame->missing, sizeof(size_t), _Alignof(size_t));
	}
	if (frame->inside_live)
	{
		frame->inside_live = false;
		if (frame->inside_drop != NULL)
			frame->inside_drop(frame->inside_captures);
	}
}

/*
 * txn_memtable_table_prepared_try is one try of prepared's: it begins the
 * hold, which a readied prepare keeps; hands each push to a tree with a
 * memtable off to its key's range, pinning its memtable; takes the reserves
 * of the pushes that fit the budget, and the first page of each of their
 * memtables a tree lacks, in one allocation; then takes the records of every one of those memtables before
 * it makes any, and then makes each on its page as its first writer and
 * hands its push off. A refused try takes nothing, the refusal of any one
 * memtable's records included, so a refused writer leaves no memtable for a
 * walk to close and free. It answers
 * false on an error, sets ready once the frame holds the targets, and sets
 * refused, with the delta's refusal, when the delta refused the pages of a
 * group's least or a memtable's records, the try's pins given back.
 */
static bool
txn_memtable_table_prepared_try(txn_memtable_table_prepared_frame_0_t *frame, bool *ready, bool *refused,
								HostDeltaFull *full)
{
	size_t		each = 1 + HOST_DELTA_LIST_DEEPEST;
	size_t		sized = std_wrapping_mul_usize(frame->aims_len, each);
	HostDeltaPageSize first = txn_memtable_table_first_size();
	HostDeltaPageSize list_page = HOST_DELTA_LIST_PAGE;
	bool		allocated;
	size_t		kept = 0;

	*ready = false;
	*refused = false;
	frame->pinned = frame->aims_len;
	frame->sizes.len = sized;
	if (frame->missing_live)
		std_vec_free_Checked(&frame->missing, sizeof(size_t), _Alignof(size_t));
	std_vec_new(&frame->missing, sizeof(size_t), _Alignof(size_t));
	frame->missing_live = true;
	if (!host_delta_hold(frame->io, &frame->prepared.hold, frame->error))
		return false;
	frame->hold_live = true;
	std_vec_new(&frame->ats, sizeof(TxnMemtableTableAt), _Alignof(TxnMemtableTableAt));
	frame->ats_live = true;
	if (!std_vec_try_reserve(&frame->ats, frame->aims_len, sizeof(TxnMemtableTableAt), _Alignof(TxnMemtableTableAt)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	for (size_t k = 0; k < frame->aims_len; k++)
	{
		const TxnMemtableTableAim *aim = &frame->aims[k];
		TxnMemtableTableAt at = {0};
		uint64		location;
		uint64		newest = first.words;

		if (!txn_memtable_table_memtable(aim->tree, aim->owner, &location))
		{
			if (!std_vec_try_reserve(&frame->missing, 1, sizeof(size_t), _Alignof(size_t)) ||
				!std_vec_try_reserve(&frame->sizes, 1, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize)))
			{
				*frame->error = host_Error_out_of_memory();
				return false;
			}
			std_vec_push_Checked(&frame->missing, &k, sizeof(size_t), _Alignof(size_t));
			std_vec_push_Checked(&frame->sizes, &first, sizeof(first), _Alignof(HostDeltaPageSize));
		}
		else
		{
			HostSharedRecord range;
			HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
			bool		some;

			if (!txn_memtable_table_handed(aim->tree, frame->io, &frame->prepared.hold, &frame->mine, aim->owner,
										   aim->key.ptr, aim->key.len, &some, &at, frame->error))
				return false;
			if (!some)
			{
				txn_memtable_table_prepared_unpinned(frame);
				return true;
			}
			if (!host_delta_Hold_record(&frame->prepared.hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at.range, &range,
										frame->error))
				return false;
			newest = host_shared_Record_atomic(range, TXN_MEMTABLE_RANGE_RANGE_NEWEST, load);
		}
		std_vec_push_Checked(&frame->ats, &at, sizeof(at), _Alignof(TxnMemtableTableAt));
		STD_VEC_AT(&frame->sizes, HostDeltaPageSize, std_wrapping_mul_usize(k, each)) =
			txn_memtable_version_reserve(newest, aim->key.len, aim->need);
		for (size_t d = 1; d < each; d++)
			STD_VEC_AT(&frame->sizes, HostDeltaPageSize, std_wrapping_mul_usize(k, each) + d) = list_page;
	}
	frame->aims_len = txn_memtable_table_prepared_fitting(frame, each, first);
	for (size_t i = 0; i < frame->missing.len; i++)
		if (STD_VEC_AT(&frame->missing, size_t, i) < frame->aims_len)
			STD_VEC_AT(&frame->missing, size_t, kept++) = STD_VEC_AT(&frame->missing, size_t, i);
	frame->missing.len = kept;
	for (size_t i = 0; i < kept; i++)
		STD_VEC_AT(&frame->sizes, HostDeltaPageSize, std_wrapping_mul_usize(frame->aims_len, each) + i) = first;
	frame->sizes.len = std_wrapping_mul_usize(frame->aims_len, each) + kept;
	frame->ats.len = frame->aims_len;
	if (!host_delta_alloc(frame->io, frame->sizes.ptr, frame->sizes.len, &allocated, &frame->pages, full,
						  frame->error))
		return false;
	if (!allocated)
	{
		size_t		least = txn_memtable_table_prepared_least(frame);

		txn_memtable_table_prepared_unpinned(frame);
		if (frame->aims_len > least)
			frame->cap = Max(frame->aims_len / 2, least);
		else
			*refused = true;
		return true;
	}
	frame->pages_live = true;
	frame->pages_at = 0;
	if (!txn_memtable_table_prepared_firsts(frame, refused, full))
		return false;
	if (*refused)
	{
		txn_memtable_table_prepared_unpinned(frame);
		return true;
	}
	{
		bool		some;

		if (!txn_memtable_table_prepared_linked(frame, &some))
			return false;
		if (!some)
		{
			txn_memtable_table_prepared_unpinned(frame);
			return true;
		}
	}
	frame->missing_live = false;
	std_vec_free_Checked(&frame->missing, sizeof(size_t), _Alignof(size_t));
	std_vec_new(&frame->prepared.targets, sizeof(TxnMemtableTableTarget), _Alignof(TxnMemtableTableTarget));
	frame->targets_live = true;
	if (!std_vec_try_reserve(&frame->prepared.targets, frame->aims_len, sizeof(TxnMemtableTableTarget),
							 _Alignof(TxnMemtableTableTarget)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	for (size_t k = 0; k < frame->aims_len; k++)
	{
		const TxnMemtableTableAim *aim = &frame->aims[k];
		TxnMemtableTableTarget *target = &frame->target;

		*target = (TxnMemtableTableTarget)
		{
			.tree = aim->tree, .at = STD_VEC_AT(&frame->ats, TxnMemtableTableAt, k)
		};
		std_vec_new(&target->reserve.buffer, 1, 1);
		std_vec_new(&target->reserve.value, 1, 1);
		std_vec_new(&target->reserve.spare, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
		frame->target_live = true;
		if (!std_vec_try_reserve_exact(&target->reserve.buffer, aim->need, 1, 1) ||
			!std_vec_try_reserve_exact(&target->reserve.value, aim->most, 1, 1))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		target->reserve.has_fresh = frame->pages_at < frame->pages.len;
		if (target->reserve.has_fresh)
		{
			target->reserve.fresh = STD_VEC_AT(&frame->pages, HostDeltaOwned, frame->pages_at);
			frame->pages_at = std_wrapping_add_usize(frame->pages_at, 1);
		}
		for (size_t s = 0; s < HOST_DELTA_LIST_DEEPEST && frame->pages_at < frame->pages.len; s++)
		{
			std_vec_push_Checked(&target->reserve.spare, &STD_VEC_AT(&frame->pages, HostDeltaOwned, frame->pages_at),
								 sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
			frame->pages_at = std_wrapping_add_usize(frame->pages_at, 1);
		}
		std_vec_push_Checked(&frame->prepared.targets, target, sizeof(*target), _Alignof(TxnMemtableTableTarget));
		frame->target_live = false;
	}
	txn_memtable_table_prepared_pages_free(frame);
	frame->ats_live = false;
	std_vec_free_Checked(&frame->ats, sizeof(TxnMemtableTableAt), _Alignof(TxnMemtableTableAt));
	*ready = true;
	return true;
}

/*
 * txn_memtable_table_prepared_group readies the aims from start that fit an
 * empty budget, as many as its try keeps, runs inside with them, and settles
 * them, answering false on an error, or ends at a refusal it answered.
 */
static bool
txn_memtable_table_prepared_group(txn_memtable_table_prepared_frame_0_t *frame, size_t start)
{
	size_t		each = 1 + HOST_DELTA_LIST_DEEPEST;
	bool		ready = false;
	bool		ok;
	bool		refused;
	HostDeltaFull full;

	frame->aims = frame->all + start;
	frame->aims_len = frame->all_len - start;
	frame->cap = frame->aims_len;
	frame->sizes.len = 0;
	if (!std_vec_try_reserve(&frame->sizes, STD_MUL(usize, frame->aims_len, each, 454, 23),
							 sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	while (!ready)
	{
		frame->aims_len = frame->all_len - start;
		if (!txn_memtable_table_prepared_try(frame, &ready, &refused, &full))
			return false;
		if (!ready)
		{
			bool		answered;

			txn_memtable_table_prepared_pages_free(frame);
			if (frame->ats_live)
			{
				frame->ats_live = false;
				std_vec_free_Checked(&frame->ats, sizeof(TxnMemtableTableAt), _Alignof(TxnMemtableTableAt));
			}
			if (frame->hold_live)
			{
				frame->hold_live = false;
				host_delta_Hold_drop(&frame->prepared.hold);
			}
			if (refused && frame->refused == NULL && frame->prepared.has_tuple)
			{
				if (!txn_memtable_tuples_given(frame->io, frame->all[0].owner, frame->error) ||
					!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-tuple-given", frame->error) ||
					!txn_memtable_table_waited(frame->io, full, false, frame->error) ||
					!txn_memtable_tuples_reserve(frame->io, frame->all[0].owner, frame->all[0].key.ptr,
												 frame->all[0].key.len, &frame->prepared.tuple, NULL, frame->error))
					return false;
				continue;
			}
			if (refused && !txn_memtable_table_refused(frame->io, full, frame->refused, &answered, frame->error))
				return false;
			if (refused && answered)
			{
				frame->answered = true;
				return true;
			}
		}
	}
	frame->prepared.aims = frame->aims;
	frame->prepared.aims_len = frame->aims_len;
	frame->prepared.first = start;
	ok = host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-memtable-prepared", frame->error);
	if (ok)
	{
		frame->inside_live = false;
		ok = frame->inside(frame->inside_captures, frame->io, &frame->prepared, frame->error);
	}
	txn_memtable_table_prepared_unpinned(frame);
	frame->hold_live = false;
	host_delta_Hold_drop(&frame->prepared.hold);
	for (size_t k = 0, k_len = frame->prepared.targets.len; k < k_len; k++)
	{
		TxnMemtableTableReserve *reserve = &STD_VEC_AT(&frame->prepared.targets, TxnMemtableTableTarget, k).reserve;

		if (reserve->has_fresh)
		{
			reserve->has_fresh = false;
			host_delta_Owned_drop(&reserve->fresh);
		}
		std_vec_clear(&reserve->spare, sizeof(HostDeltaOwned), host_delta_Owned_drop);
	}
	if (!ok || !host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-memtable-unpinned", frame->error))
		return false;
	for (size_t k = 0, k_len = frame->prepared.targets.len; k < k_len; k++)
	{
		const TxnMemtableTableTarget *target = &STD_VEC_AT(&frame->prepared.targets, TxnMemtableTableTarget, k);

		if (!txn_memtable_table_settled(target->tree, frame->io, frame->aims[k].owner, target->at,
										target->reserve.has_seen, &target->reserve.seen, frame->error))
			return false;
	}
	frame->targets_live = false;
	std_vec_clear(&frame->prepared.targets, sizeof(TxnMemtableTableTarget), txn_memtable_table_Target_drop);
	std_vec_free_Checked(&frame->prepared.targets, sizeof(TxnMemtableTableTarget), _Alignof(TxnMemtableTableTarget));
	return true;
}

/*
 * txn_memtable_table_prepared_body_0 is prepared's body: its groups in turn,
 * each the aims after the last group that an empty budget can give (bugs row
 * 338).
 */
static void
txn_memtable_table_prepared_body_0(void *frame_)
{
	txn_memtable_table_prepared_frame_0_t *frame = frame_;
	size_t		start = 0;

	if (!host_shared_mine(HOST_IO_IO_TAKES_SHORT(frame->io), &TXN_MEMTABLE_PINS_PINS_PROC_SLOTS_OPS, &frame->mine,
						  frame->error))
	{
		txn_memtable_table_prepared_pad_0(frame);
		return;
	}
	std_vec_new(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	frame->sizes_live = true;
	do
	{
		if (!txn_memtable_table_prepared_group(frame, start))
		{
			if (frame->inside_live && frame->inside_drop != NULL)
			{
				frame->inside_live = false;
				frame->inside_drop(frame->inside_captures);
			}
			txn_memtable_table_prepared_pad_0(frame);
			return;
		}
		if (frame->answered)
		{
			Assert(start == 0);
			*frame->has_unstored = false;
			frame->ok = true;
			txn_memtable_table_prepared_pad_0(frame);
			return;
		}
		start += frame->aims_len;
	} while (start < frame->all_len);
	*frame->has_unstored = frame->prepared.has_tuple;
	if (frame->prepared.has_tuple)
		*frame->unstored = frame->prepared.tuple;
	frame->ok = true;
	txn_memtable_table_prepared_pad_0(frame);
}

/*
 * txn_memtable_table_prepared readies pushes to its owners' trees before a
 * checkpoint window and runs the window's work with them, taking inside's
 * closure, whose captures carry its result and what the caller hands it.
 * Each try hands each push off to its key's range, pinning its memtable, and
 * takes in one allocation the reserves of the pushes from the group's first
 * that an empty budget can give, with the records and first page of each
 * memtable the group's trees lack, which it takes before it links any, going
 * around when a hand-off or the allocation can't and meeting the delta
 * budget's refusal as refused says, a writer giving back the tuple id
 * table's reservation while it waits and reserving again after, so its wait
 * keeps nothing a packing would move, the pause doltgres-tuple-given coming
 * between the give-back and the wait (bugs row 143). It then runs inside with
 * that group, whose finishes allocate nothing, and settles, group after
 * group, so no allocation asks more than the budget holds (bugs row 338); a
 * prepare of at most TXN_MEMTABLE_TABLE_WHOLE pushes is one group, and a
 * refused group of more than its least tries again with half as many. A
 * group's settle clears its pins, gives back what the finishes left of the
 * reserves, and, unless inside failed, splits each range a finish left past
 * SPLIT keys and closes each memtable a prepare made or pinned that no finish
 * reached. It answers the tuple id table's reservation the prepare was given,
 * if no finish stored it.
 */
bool
txn_memtable_table_prepared(HostIoIo *io, const TxnMemtableTableAim *aims, size_t aims_len, bool has_tuple,
							TxnMemtableTableReservation tuple,
							bool (*inside) (void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
											HostError *error),
							void *inside_captures, void (*inside_drop) (void *captures), bool *has_unstored,
							TxnMemtableTableReservation *unstored, TxnMemtableTableRefusal *refused, HostError *error)
{
	txn_memtable_table_prepared_frame_0_t frame = {.io = io, .all = aims, .all_len = aims_len, .inside = inside,
		.inside_captures = inside_captures, .inside_drop = inside_drop, .has_unstored = has_unstored,
		.unstored = unstored, .refused = refused, .error = error, .inside_live = true,
	.prepared = {.has_tuple = has_tuple, .tuple = tuple}};

	if (refused != NULL)
		refused->refused = false;
	std_landing(&frame, txn_memtable_table_prepared_body_0, txn_memtable_table_prepared_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_table_refused meets the delta budget's refusal of a prepare:
 * a writer's, whose refused is NULL, waits for a free and tries again; a
 * rewrite's carry's tries again after a deferred compaction or a spill freed
 * the budget, and otherwise answers the refusal through refused, having
 * taken nothing, so its caller, which holds the catalog, folds the new
 * storage first. It answers whether it answered the refusal.
 */
bool
txn_memtable_table_refused(HostIoIo *io, HostDeltaFull full, TxnMemtableTableRefusal *refused, bool *result,
						   HostError *error)
{
	bool		freed;

	*result = false;
	if (refused == NULL)
		return txn_memtable_table_waited(io, full, false, error);
	if (!txn_memtable_table_deferred(io, &freed, error))
		return false;
	if (!freed && !txn_spill_relieved(io, &freed, error))
		return false;
	refused->refused = !freed;
	refused->full = full;
	*result = !freed;
	return true;
}

/* txn_memtable_table_within_t is within's step's captures: the caller's step and its captures, the hold, the key, and the reserve. */
typedef struct txn_memtable_table_within_t
{
	bool		(*step) (void *captures, TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold, const uint8 *key,
						 size_t key_len, TxnMemtableTableReserve *reserve, HostError *error);
	void	   *captures;
	const HostDeltaHold *hold;
	const TxnMemtableTableAim *aim;
	TxnMemtableTableReserve *reserve;
} txn_memtable_table_within_t;

/* txn_memtable_table_within_step runs the caller's step at the open range with the hold, the key, and the reserve. */
static bool
txn_memtable_table_within_step(void *captures, TxnMemtableIndexKeyedOpen *open, HostError *error)
{
	txn_memtable_table_within_t *within = captures;

	return within->step(within->captures, open, within->hold, within->aim->key.ptr, within->aim->key.len,
						within->reserve, error);
}

/*
 * txn_memtable_table_kept takes and keeps the exclusive locks of the open
 * ranges that hold prepared aims' keys, aims of one map, before its caller's
 * window, so no removal or other take moves their chains until unkept, as
 * heap holds its buffers' locks across its critical section; every finish of
 * those keys runs on them meanwhile (bugs rows 342 and 432). It locks in the
 * keys' order, so a range is locked only after every lower one its keeper
 * holds and two keepers never wait on each other, and two keys in one range
 * share its lock.
 */
bool
txn_memtable_table_kept(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, const size_t *indices,
						size_t len, HostError *error)
{
	size_t		order[TXN_MEMTABLE_TABLE_KEPT_MOST];

	for (size_t i = 0; i < len; i++)
	{
		size_t		at = i;

		for (; at > 0; at--)
		{
			const StdBytes *key = &prepared->aims[indices[i]].key;
			const StdBytes *before = &prepared->aims[order[at - 1]].key;

			if (std_bytes_Ord_cmp(before->ptr, before->len, key->ptr, key->len) != STD_ORDERING_GREATER)
				break;
			order[at] = order[at - 1];
		}
		order[at] = indices[i];
	}
	for (size_t i = 0; i < len; i++)
	{
		TxnMemtableTableTarget *target = &STD_VEC_AT(&prepared->targets, TxnMemtableTableTarget, order[i]);
		TxnMemtableTableKept *kept = &prepared->kept[prepared->kept_len];
		size_t		shared;

		if (!txn_memtable_table_opened(io, &prepared->hold, &target->at, prepared->aims[order[i]].key.ptr,
									   prepared->aims[order[i]].key.len, prepared->kept, prepared->kept_len, &shared,
									   &kept->guard, &kept->open, error))
			return false;
		kept->aim = order[i];
		kept->range = target->at.range;
		kept->owns = shared == prepared->kept_len;
		if (!kept->owns)
			kept->open = prepared->kept[shared].open;
		prepared->kept_len++;
	}
	return true;
}

/* txn_memtable_table_unkept releases the ranges kept keeps, the last locked first. */
void
txn_memtable_table_unkept(TxnMemtableTablePreparedState *prepared)
{
	while (prepared->kept_len > 0)
	{
		TxnMemtableTableKept *kept = &prepared->kept[--prepared->kept_len];

		if (kept->owns)
			host_shared_Guard_drop(&kept->guard);
	}
}

/* txn_memtable_table_keeps returns the kept aim whose key is a prepared aim's, or NULL when kept keeps none. */
TxnMemtableTableKept *
txn_memtable_table_keeps(TxnMemtableTablePreparedState *prepared, size_t index)
{
	const TxnMemtableTableAim *aim = &prepared->aims[index];

	for (size_t k = 0; k < prepared->kept_len; k++)
	{
		const TxnMemtableTableAim *held = &prepared->aims[prepared->kept[k].aim];

		if (held->tree == aim->tree && held->owner.place == aim->owner.place &&
			std_bytes_Ord_cmp(held->key.ptr, held->key.len, aim->key.ptr, aim->key.len) == STD_ORDERING_EQUAL)
			return &prepared->kept[k];
	}
	return NULL;
}

/*
 * txn_memtable_table_within runs a step of a finish inside the window on a
 * prepared target: it follows the target's range to the open range that
 * holds its key now and runs the step there, under that range's exclusive
 * lock, with the prepare's hold, the target's key, and its reserve, then
 * marks a push the target made, which a key lock's view keeps. In a tree
 * that counts, a writer with a room counts its version: one is added at the
 * room's entry before the step and taken back after it, a failed step's
 * included, unless the step pushed a version it marked counted, so the count
 * never falls below the versions that name the entry, and a step that pushed
 * none leaves it as it was.
 */
bool
txn_memtable_table_within(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t index,
						  bool has_roomed, TxnStatusRoomed roomed,
						  bool (*step) (void *captures, TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
										const uint8 *key, size_t key_len, TxnMemtableTableReserve *reserve,
										HostError *error),
						  void *captures, HostError *error)
{
	TxnMemtableTableTarget *target;
	txn_memtable_table_within_t within;
	TxnMemtableTableKept *kept;
	bool		ok;
	uint64		grown;
	bool		counting;
	HostError	given;

	if (index >= prepared->aims_len || index >= prepared->targets.len)
	{
		*error = txn_memtable_table_internal("a finish names no prepared push");
		return false;
	}
	target = &STD_VEC_AT(&prepared->targets, TxnMemtableTableTarget, index);
	within = (txn_memtable_table_within_t)
	{
		step, captures, &prepared->hold, &prepared->aims[index], &target->reserve
	};
	counting = has_roomed && prepared->aims[index].tree->counted;
	target->reserve.counting = counting;
	target->reserve.counted = false;
	if (counting && !txn_status_counted(io, &prepared->hold, roomed, 1, error))
		return false;
	kept = txn_memtable_table_keeps(prepared, index);
	if (kept != NULL)
		ok = txn_memtable_table_within_step(&within, &kept->open, error);
	else
		ok = txn_memtable_table_reached(io, &prepared->hold, &target->at, prepared->aims[index].key.ptr,
										prepared->aims[index].key.len, txn_memtable_table_within_step, &within,
										error);
	if (counting && !target->reserve.counted)
	{
		if (ok)
			ok = txn_status_counted(io, &prepared->hold, roomed, PG_UINT64_MAX, error);
		else if (!txn_status_counted(io, &prepared->hold, roomed, PG_UINT64_MAX, &given))
			host_Error_drop(&given);
	}
	if (target->reserve.has_seen)
		host_pushed(io);
	grown = target->reserve.grown;
	target->reserve.grown = 0;
	if (grown > 0)
		prepared->aims[index].tree->of->grew(prepared->aims[index].owner, grown);
	return ok;
}

/*
 * txn_memtable_table_settled is a target's part of the settle: a split of
 * the range its finish left past SPLIT keys, or, for a target no finish
 * reached, the close of its memtable when it holds nothing.
 */
bool
txn_memtable_table_settled(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
						   TxnMemtableTableAt at, bool has_seen, const TxnMemtableSplitSeen *seen, HostError *error)
{
	bool		abandoned;

	if (has_seen && seen->keys > TXN_MEMTABLE_SPLIT_SPLIT)
		return txn_memtable_split_split(tree, io, owner, at, seen, error);
	if (has_seen)
		return true;
	return txn_memtable_table_abandoned(tree, io, owner, at.memtable, &abandoned, error);
}

/* txn_memtable_table_readied_frame_0_t is readied's state across its pad: the map takes' keys, the aims, and inside's closure until prepared takes it, while live. */
typedef struct txn_memtable_table_readied_frame_0_t
{
	HostIoIo   *io;
	bool		has_rows;
	HostSharedRecord state;
	const txn_TupleRefSliceU8Usize_t *rows;
	size_t		rows_len;
	const TxnMemtableMapTake *takes;
	size_t		takes_len;
	bool		tuple;
	bool		(*inside) (void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared, HostError *error);
	void	   *inside_captures;
	void		(*inside_drop) (void *captures);

	/*
	 * refused, for a carry's prepare, answers whether the delta's budget
	 * refused it, with its refusal.
	 */
	TxnMemtableTableRefusal *refused;
	HostError  *error;
	bool		ok;
	bool		inside_live;
	StdVec		keys;
	bool		keys_live;
	StdVec		aims;
	bool		aims_live;
} txn_memtable_table_readied_frame_0_t;

/* txn_memtable_table_readied_pad_0 is readied's pad: the aims, the keys, and inside's closure, while live. */
static void
txn_memtable_table_readied_pad_0(void *frame_)
{
	txn_memtable_table_readied_frame_0_t *frame = frame_;

	if (frame->aims_live)
	{
		frame->aims_live = false;
		std_vec_free_Checked(&frame->aims, sizeof(TxnMemtableTableAim), _Alignof(TxnMemtableTableAim));
	}
	if (frame->keys_live)
	{
		frame->keys_live = false;
		std_vec_free_Checked(&frame->keys, TXN_MEMTABLE_MAP_KEY, 1);
	}
	if (frame->inside_live)
	{
		frame->inside_live = false;
		if (frame->inside_drop != NULL)
			frame->inside_drop(frame->inside_captures);
	}
}

/* txn_memtable_table_readied_aims gathers the map takes' keys and the aims of the rows and the takes. */
static bool
txn_memtable_table_readied_aims(txn_memtable_table_readied_frame_0_t *frame, size_t row_count)
{
	std_vec_new(&frame->keys, TXN_MEMTABLE_MAP_KEY, 1);
	frame->keys_live = true;
	std_vec_new(&frame->aims, sizeof(TxnMemtableTableAim), _Alignof(TxnMemtableTableAim));
	frame->aims_live = true;
	if (!std_vec_try_reserve(&frame->keys, frame->takes_len, TXN_MEMTABLE_MAP_KEY, 1) ||
		!std_vec_try_reserve(&frame->aims, STD_ADD(usize, row_count, frame->takes_len, 612, 22),
							 sizeof(TxnMemtableTableAim), _Alignof(TxnMemtableTableAim)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	for (size_t k = 0; k < frame->takes_len; k++)
	{
		uint8		key[TXN_MEMTABLE_MAP_KEY];

		std_u32_to_be_bytes(frame->takes[k].key, key);
		std_vec_push_Checked(&frame->keys, key, TXN_MEMTABLE_MAP_KEY, 1);
	}
	for (size_t k = 0; k < row_count; k++)
	{
		TxnMemtableTableAim aim = {.owner = frame->state, .tree = &TXN_MEMTABLE_TABLE_ROWS_TREE_OPS,
		.key = frame->rows[k].f0, .need = frame->rows[k].f1};

		std_vec_push_Checked(&frame->aims, &aim, sizeof(aim), _Alignof(TxnMemtableTableAim));
	}
	for (size_t k = 0; k < frame->takes_len; k++)
	{
		const TxnMemtableMapTake *take = &frame->takes[k];
		TxnMemtableTableAim aim = {.owner = take->state,
			.tree = take->map == TXN_MEMTABLE_MAP_MAP_FREE ? &TXN_MEMTABLE_MAP_FREE_TREE_OPS :
			&TXN_MEMTABLE_MAP_VALUES_TREE_OPS,
			.key = {STD_PTR_AT(frame->keys.ptr, k, TXN_MEMTABLE_MAP_KEY), TXN_MEMTABLE_MAP_KEY},
		.need = txn_memtable_version_largest(take->most), .most = take->most};

		std_vec_push_Checked(&frame->aims, &aim, sizeof(aim), _Alignof(TxnMemtableTableAim));
	}
	return true;
}

/* txn_memtable_table_readied_body_0 is readied's body. */
static void
txn_memtable_table_readied_body_0(void *frame_)
{
	txn_memtable_table_readied_frame_0_t *frame = frame_;
	size_t		row_count = frame->has_rows ? frame->rows_len : 0;
	bool		has_tuple = false;
	TxnMemtableTableReservation reservation = {0};
	bool		has_unstored;
	TxnMemtableTableReservation unstored;

	if (!txn_memtable_table_readied_aims(frame, row_count))
	{
		txn_memtable_table_readied_pad_0(frame);
		return;
	}
	if (frame->tuple && row_count > 0)
	{
		if (!txn_memtable_tuples_reserve(frame->io, frame->state, frame->rows[0].f0.ptr, frame->rows[0].f0.len,
										 &reservation, frame->refused, frame->error))
		{
			txn_memtable_table_readied_pad_0(frame);
			return;
		}
		if (frame->refused != NULL && frame->refused->refused)
		{
			frame->ok = true;
			txn_memtable_table_readied_pad_0(frame);
			return;
		}
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-tuple-reserved", frame->error))
		{
			txn_memtable_table_readied_pad_0(frame);
			return;
		}
		has_tuple = true;
	}
	frame->inside_live = false;
	if (!txn_memtable_table_prepared(frame->io, frame->aims.ptr, frame->aims.len, has_tuple, reservation,
									 frame->inside, frame->inside_captures, frame->inside_drop, &has_unstored,
									 &unstored, frame->refused, frame->error))
	{
		txn_memtable_table_readied_pad_0(frame);
		return;
	}
	txn_memtable_table_readied_pad_0(frame);
	if (frame->refused != NULL && frame->refused->refused)
		has_unstored = has_tuple;
	frame->ok = !has_unstored || row_count == 0 || txn_memtable_tuples_given(frame->io, frame->state, frame->error);
}

/*
 * txn_memtable_table_readied readies a storage's row pushes, none for a
 * prepare of map takes alone, and map takes, each in the storage its take
 * names, as prepare and map's prepare do, with the first row's tuple id slot
 * when tuple is set, taking inside's closure, whose captures carry its
 * result and what the caller hands it, and meeting the delta budget's
 * refusal as refused says (refused): a carry's refused prepare gives back
 * the slot it reserved.
 */
bool
txn_memtable_table_readied(HostIoIo *io, bool has_rows, HostSharedRecord state, const txn_TupleRefSliceU8Usize_t *rows,
						   size_t rows_len, const TxnMemtableMapTake *takes, size_t takes_len, bool tuple,
						   bool (*inside) (void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
										   HostError *error),
						   void *inside_captures, void (*inside_drop) (void *captures),
						   TxnMemtableTableRefusal *refused, HostError *error)
{
	txn_memtable_table_readied_frame_0_t frame = {.io = io, .has_rows = has_rows, .state = state, .rows = rows,
		.rows_len = rows_len, .takes = takes, .takes_len = takes_len, .tuple = tuple, .inside = inside,
		.inside_captures = inside_captures, .inside_drop = inside_drop, .refused = refused, .error = error,
	.inside_live = true};

	std_landing(&frame, txn_memtable_table_readied_body_0, txn_memtable_table_readied_pad_0);
	return frame.ok;
}

/* txn_memtable_table_pushed_t is pushed's step's captures: the change and the handle it gives. */
typedef struct txn_memtable_table_pushed_t
{
	const TxnMemtableVersionChange *change;
	TxnMemtableVersionHandle *result;
	uint32		token;
	TxnMemtableVersionHandle ended;
	uint32		combo;
} txn_memtable_table_pushed_t;

/* txn_memtable_table_pushed_step pushes the change with the reserve. */
static bool
txn_memtable_table_pushed_step(void *captures, TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
							   const uint8 *key, size_t key_len, TxnMemtableTableReserve *reserve, HostError *error)
{
	txn_memtable_table_pushed_t *pushed = captures;

	return txn_memtable_table_Reserve_push(reserve, open, hold, key, key_len, pushed->change, pushed->result, error);
}

/*
 * txn_memtable_table_pushed pushes a prepared target's version inside the
 * window with its reserve, allocating nothing, and counts it at its writer's
 * entry when it names one.
 */
bool
txn_memtable_table_pushed(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t index,
						  const TxnMemtableVersionChange *change, bool has_roomed, TxnStatusRoomed roomed,
						  TxnMemtableVersionHandle *result, HostError *error)
{
	txn_memtable_table_pushed_t pushed = {.change = change, .result = result};

	return txn_memtable_table_within(io, prepared, index, has_roomed, roomed, txn_memtable_table_pushed_step, &pushed,
									 error);
}

/*
 * txn_memtable_table_finish pushes a prepared first row's version inside the
 * window, allocating nothing, and counts it at its writer's entry when it
 * names one; it fails only on corruption.
 */
bool
txn_memtable_table_finish(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared,
						  const TxnMemtableVersionChange *change, bool has_roomed, TxnStatusRoomed roomed,
						  TxnMemtableVersionHandle *result, HostError *error)
{
	return txn_memtable_table_finish_row(io, prepared, 0, change, has_roomed, roomed, result, error);
}

/* txn_memtable_table_speculated_step pushes the change and stores its speculative insert's token under the same lock. */
static bool
txn_memtable_table_speculated_step(void *captures, TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
								   const uint8 *key, size_t key_len, TxnMemtableTableReserve *reserve,
								   HostError *error)
{
	txn_memtable_table_pushed_t *pushed = captures;

	return txn_memtable_table_Reserve_push(reserve, open, hold, key, key_len, pushed->change, pushed->result,
										   error) &&
		txn_memtable_version_speculate(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, open->open.guard, hold, *pushed->result,
									   pushed->token, error);
}

/*
 * txn_memtable_table_finish_speculative pushes a prepared first row's
 * version as finish does, a speculative insert's, and stores its token in
 * the version under the same lock, so a Dirty reader finds it with the
 * version, then counts it at its writer's entry when it names one.
 */
bool
txn_memtable_table_finish_speculative(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared,
									  const TxnMemtableVersionChange *change, uint32 token, bool has_roomed,
									  TxnStatusRoomed roomed, TxnMemtableVersionHandle *result, HostError *error)
{
	txn_memtable_table_pushed_t pushed = {.change = change, .result = result, .token = token};

	return txn_memtable_table_within(io, prepared, 0, has_roomed, roomed, txn_memtable_table_speculated_step, &pushed,
									 error);
}

/* txn_memtable_table_rowed refuses a row index past the prepare's rows with an error of its text. */
static bool
txn_memtable_table_rowed(const TxnMemtableTablePreparedState *prepared, size_t row, const char *text,
						 HostError *error)
{
	if (row < txn_memtable_table_Prepared_rows(prepared))
		return true;
	*error = txn_memtable_table_internal(text);
	return false;
}

/* txn_memtable_table_shown_step pushes the change and stores the combo command id in the row it ends, under the same lock. */
static bool
txn_memtable_table_shown_step(void *captures, TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
							  const uint8 *key, size_t key_len, TxnMemtableTableReserve *reserve, HostError *error)
{
	txn_memtable_table_pushed_t *pushed = captures;

	return txn_memtable_table_Reserve_push(reserve, open, hold, key, key_len, pushed->change, pushed->result,
										   error) &&
		txn_memtable_version_show(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, open->open.guard, hold, pushed->ended,
								  pushed->combo, error);
}

/*
 * txn_memtable_table_finish_showing pushes the version of a prepared row of
 * an index among the prepare's rows as finish_row does, and stores under the
 * same lock a combo command id as the raw command of the row an older
 * version of its key holds, the row the change ends or locks, as heap's
 * change stores it in the old tuple's t_cid, then counts the version at its
 * writer's entry when it names one.
 */
bool
txn_memtable_table_finish_showing(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t row,
								  const TxnMemtableVersionChange *change, TxnMemtableVersionHandle ended,
								  uint32 combo, bool has_roomed, TxnStatusRoomed roomed,
								  TxnMemtableVersionHandle *result, HostError *error)
{
	txn_memtable_table_pushed_t pushed = {.change = change, .result = result, .ended = ended, .combo = combo};

	return txn_memtable_table_rowed(prepared, row, "a row's finish names a row the prepare didn't ready", error) &&
		txn_memtable_table_within(io, prepared, row, has_roomed, roomed, txn_memtable_table_shown_step, &pushed,
								  error);
}

/*
 * txn_memtable_table_finished pushes a prepared target's version inside the
 * window as pushed does, and counts it at its writer's entry when it names
 * one, so a fold finds the writer's outcome.
 */
bool
txn_memtable_table_finished(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t index,
							const TxnMemtableVersionChange *change, bool has_roomed, TxnStatusRoomed roomed,
							TxnMemtableVersionHandle *result, HostError *error)
{
	return txn_memtable_table_pushed(io, prepared, index, change, has_roomed, roomed, result, error);
}

/*
 * txn_memtable_table_finish_row pushes the version of a prepared row of an
 * index among the prepare's rows inside the window, allocating nothing, and
 * counts it at its writer's entry when it names one; it fails only on
 * corruption.
 */
bool
txn_memtable_table_finish_row(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t row,
							  const TxnMemtableVersionChange *change, bool has_roomed, TxnStatusRoomed roomed,
							  TxnMemtableVersionHandle *result, HostError *error)
{
	return txn_memtable_table_rowed(prepared, row, "a row's finish names a row the prepare didn't ready", error) &&
		txn_memtable_table_finished(io, prepared, row, change, has_roomed, roomed, result, error);
}

/* txn_memtable_table_newest_t is newest's step's captures: its answer. */
typedef struct txn_memtable_table_newest_t
{
	bool	   *some;
	TxnMemtableVersionHandle *result;
} txn_memtable_table_newest_t;

/* txn_memtable_table_newest_step finds the key's node and its newest version's handle. */
static bool
txn_memtable_table_newest_step(void *captures, TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
							   const uint8 *key, size_t key_len, TxnMemtableTableReserve *reserve, HostError *error)
{
	txn_memtable_table_newest_t *newest = captures;
	TxnMemtableIndexNode node;

	*newest->some = false;
	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, open->open.guard, hold, key, key_len,
								 newest->some,
								 &node, error))
		return false;
	return !*newest->some ||
		txn_memtable_version_head(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, open->open.guard, hold, node, newest->some,
								  newest->result, error);
}

/*
 * txn_memtable_table_newest returns the handle of the newest version of a
 * prepared row's key inside the window, under its range's lock, or none for
 * a key with none, so that a change under the key's stripe pushes only over
 * the version its read found.
 */
bool
txn_memtable_table_newest(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t row, bool *some,
						  TxnMemtableVersionHandle *result, HostError *error)
{
	txn_memtable_table_newest_t newest = {some, result};
	TxnStatusRoomed unroomed = {0};

	return txn_memtable_table_rowed(prepared, row, "a row's newest version names a row the prepare didn't ready",
									error) &&
		txn_memtable_table_within(io, prepared, row, false, unroomed, txn_memtable_table_newest_step,
								  &newest, error);
}

/*
 * txn_memtable_table_lent runs read's read under a caller's hold, so that
 * what it answers may borrow the hold's pages, as a reader's versions do; it
 * closes an empty memtable under a hold of its own beside the caller's. A
 * closed range is taken out of its memtable's list and the hand-off starts
 * over; reading writes its answer through its captures, and some says
 * whether it ran.
 */
bool
txn_memtable_table_lent(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
						HostSharedRecord owner, const uint8 *key, size_t key_len,
						bool (*reading) (void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
										 const HostSharedGuard *guard, HostError *error),
						void *captures, bool *some, HostError *error)
{
	for (;;)
	{
		TxnMemtableTableAt at;
		HostSharedRecord range;
		HostSharedGuard guard;
		bool		read;
		HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

		if (!txn_memtable_table_found(tree, io, hold, owner, key, key_len, NULL, some, &at, error))
			return false;
		if (!*some)
			return true;
		*some = false;
		if (!host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at.range, &range, error) ||
			!host_shared_Record_lock_shared(range, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
											error))
			return false;
		read = !txn_memtable_index_closed(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS.closes, &guard);
		if (read && !reading(captures, hold, at, &guard, error))
		{
			host_shared_Guard_drop(&guard);
			return false;
		}
		host_shared_Guard_drop(&guard);
		if (!read)
		{
			if (!txn_memtable_table_unlisted(tree, io, hold, owner, at, error))
				return false;
			continue;
		}
		if (host_shared_Record_atomic(range, TXN_MEMTABLE_RANGE_RANGE_KEYS, load) == 1)
		{
			bool		abandoned;

			if (!txn_memtable_table_abandoned(tree, io, owner, at.memtable, &abandoned, error))
				return false;
		}
		*some = true;
		return true;
	}
}

/*
 * txn_memtable_table_read runs a read of a range under its shared lock from
 * a tree's hand-off for a key, outside any window: a closed range is taken
 * out of its memtable's list, as any finder does, and the hand-off starts
 * over. A range that holds only its first key has its memtable closed when
 * nothing else holds or pins it, as any hand-off that finds a memtable empty
 * closes it. It answers nothing for a tree with no memtable.
 */
bool
txn_memtable_table_read(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner, const uint8 *key,
						size_t key_len,
						bool (*reading) (void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
										 const HostSharedGuard *guard, HostError *error),
						void *captures, bool *some, HostError *error)
{
	HostDeltaHold hold;
	bool		ok;

	if (!host_delta_hold(io, &hold, error))
		return false;
	ok = txn_memtable_table_lent(tree, io, &hold, owner, key, key_len, reading, captures, some, error);
	host_delta_Hold_drop(&hold);
	return ok;
}

/* txn_memtable_table_held_t is held's reading's captures: the key, the LSN, and the answer. */
typedef struct txn_memtable_table_held_t
{
	const uint8 *key;
	size_t		key_len;
	uint64		lsn;
	bool		result;
} txn_memtable_table_held_t;

/* txn_memtable_table_held_reading looks for a version of the key carrying the LSN in its range's chain. */
static bool
txn_memtable_table_held_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
								const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_table_held_t *held = captures;
	bool		some;
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;

	held->result = false;
	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, held->key, held->key_len, &some,
								 &node, error))
		return false;
	if (!some)
		return true;
	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error))
		return false;
	for (;;)
	{
		TxnMemtableVersionVersion version;
		TxnMemtableVersionRecord record;
		uint64		lsn;
		bool		found;

		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, error))
			return false;
		if (!some)
			return true;
		record = txn_memtable_version_Version_record(&version);
		found = txn_memtable_version_Record_lsn(&record, &lsn) && lsn == held->lsn;
		txn_memtable_version_Version_drop(&version);
		if (found)
		{
			held->result = true;
			return true;
		}
	}
}

/*
 * txn_memtable_table_held reports whether a tree's delta holds a version of
 * a key carrying the end of a WAL record, I12's test of a logged version the
 * checkpoint's runs brought back, read outside any window.
 */
bool
txn_memtable_table_held(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner, const uint8 *key,
						size_t key_len, uint64 lsn, bool *result, HostError *error)
{
	txn_memtable_table_held_t held = {key, key_len, lsn};
	bool		some;

	if (!txn_memtable_table_read(tree, io, owner, key, key_len, txn_memtable_table_held_reading, &held, &some, error))
		return false;
	*result = some && held.result;
	return true;
}

/*
 * txn_memtable_table_holds reports whether a storage's rows, in its memtable
 * or its spills, hold the version a row change record of an LSN pushed, so
 * that its redo skips the row's push alone.
 */
bool
txn_memtable_table_holds(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len, uint64 lsn,
						 bool *result, HostError *error)
{
	if (!txn_memtable_table_held(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, key, key_len, lsn, result, error))
		return false;
	return *result || txn_spill_holds(&TXN_SPILL_STATE_HOME_OPS, io, state, TXN_RUNS_KIND_SPILLED_ROWS, key, key_len,
									  lsn, result, error);
}

/* txn_memtable_table_alone_t is alone's inside's captures: the change and the handle it gives. */
typedef struct txn_memtable_table_alone_t
{
	const TxnMemtableVersionChange *change;
	bool		has_roomed;
	TxnStatusRoomed roomed;
	TxnMemtableVersionHandle *result;
} txn_memtable_table_alone_t;

/* txn_memtable_table_alone_inside pushes alone's one prepared target. */
static bool
txn_memtable_table_alone_inside(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
								HostError *error)
{
	txn_memtable_table_alone_t *alone = captures;

	return txn_memtable_table_pushed(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, alone->change, alone->has_roomed,
									 alone->roomed, alone->result, error);
}

/*
 * txn_memtable_table_alone makes a version a key's newest in one of an
 * owner's trees with no window, a prepare of that one push, its finish, and
 * its settle in one call, as a row's push and the runs' loader do, counting
 * it at its writer's entry when it names one.
 */
bool
txn_memtable_table_alone(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner, const uint8 *key,
						 size_t key_len, const TxnMemtableVersionChange *change, bool has_roomed,
						 TxnStatusRoomed roomed, TxnMemtableVersionHandle *result, HostError *error)
{
	TxnMemtableTableAim aims[1] = {{.owner = owner, .tree = tree, .key = {key, key_len},
	.need = txn_memtable_version_encoded(&change->writer, change->op, change->shown, change->has_lsn, change->lsn)}};
	txn_memtable_table_alone_t alone = {change, has_roomed, roomed, result};
	TxnMemtableTableReservation none = {0};
	bool		has_unstored;
	TxnMemtableTableReservation unstored;

	return txn_memtable_table_prepared(io, aims, 1, false, none, txn_memtable_table_alone_inside, &alone, NULL,
									   &has_unstored, &unstored, NULL, error);
}

/*
 * txn_memtable_table_alone_State is alone at a storage's state, its rows or
 * one of its maps.
 */
bool
txn_memtable_table_alone_State(HostIoIo *io, HostSharedRecord owner, bool has_which, TxnMemtableMapMap which,
							   const uint8 *key, size_t key_len, const TxnMemtableVersionChange *change,
							   TxnMemtableVersionHandle *result, HostError *error)
{
	TxnStatusRoomed unroomed = {0};

	const TxnMemtableTableTreeOps *tree = !has_which ? &TXN_MEMTABLE_TABLE_ROWS_TREE_OPS :
		which == TXN_MEMTABLE_MAP_MAP_FREE ? &TXN_MEMTABLE_MAP_FREE_TREE_OPS : &TXN_MEMTABLE_MAP_VALUES_TREE_OPS;

	return txn_memtable_table_alone(tree, io, owner, key, key_len, change, false, unroomed, result,
									error);
}

/*
 * txn_memtable_table_push makes a version a key's newest in a storage's
 * memtable, as a prepare, its finish, and its settle in one call, for a
 * writer that holds no checkpoint window.
 */
bool
txn_memtable_table_push(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len,
						const TxnMemtableVersionChange *change, TxnMemtableVersionHandle *result, HostError *error)
{
	TxnStatusRoomed unroomed = {0};

	return txn_memtable_table_alone(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, key, key_len, change, false,
									unroomed, result, error);
}

/* TxnMemtableTableOnePage is an iterator over one owned page, the pages a first range's lock owns at its link. */
typedef struct TxnMemtableTableOnePage
{
	bool		has;
	HostDeltaOwned page;
} TxnMemtableTableOnePage;

/* txn_memtable_table_OnePage_next gives the page once. */
static bool
txn_memtable_table_OnePage_next(void *self_, void *result)
{
	TxnMemtableTableOnePage *self = self_;

	if (!self->has)
		return false;
	self->has = false;
	*(HostDeltaOwned *) result = self->page;
	return true;
}

/* txn_memtable_table_OnePage_size_hint answers how many pages are left: the page, or none. */
static StdSizeHint
txn_memtable_table_OnePage_size_hint(const void *self_)
{
	const TxnMemtableTableOnePage *self = self_;
	StdSizeHint hint = {self->has ? 1 : 0, true, self->has ? 1 : 0};

	return hint;
}

/* txn_memtable_table_OnePage_drop gives back the page the iterator didn't give. */
static void
txn_memtable_table_OnePage_drop(void *self_)
{
	TxnMemtableTableOnePage *self = self_;

	if (self->has)
	{
		self->has = false;
		host_delta_Owned_drop(&self->page);
	}
}

/* TXN_MEMTABLE_TABLE_ONE_PAGE_ITERATOR_OPS is the one-page iterator's Iterator. */
static const StdIteratorOps TXN_MEMTABLE_TABLE_ONE_PAGE_ITERATOR_OPS = {
	txn_memtable_table_OnePage_next, txn_memtable_table_OnePage_size_hint, txn_memtable_table_OnePage_drop,
};

/* txn_memtable_table_first_frame_0_t is first's state across its pad: the pages, the records made, the first page, the link's With, and the owner's guard, while live. */
typedef struct txn_memtable_table_first_frame_0_t
{
	const TxnMemtableTableTreeOps *tree;
	HostIoIo   *io;
	HostSharedRecord owner;
	HostError  *error;
	bool		ok;
	StdVec		pages;
	bool		pages_live;
	HostDeltaMade range;
	bool		range_live;
	HostDeltaMade memtable;
	bool		memtable_live;
	TxnMemtableTableOnePage page;
	HostDeltaWith with;
	bool		with_live;
	HostSharedGuard guard;
	bool		guard_live;

	/*
	 * given is the first page the prepare's allocation took, until the
	 * memtable takes it.
	 */
	HostDeltaOwned given;
	bool		has_given;

	/*
	 * refused is whether the delta refused a record, with its refusal.
	 */
	bool	   *refused;
	HostDeltaFull *full;
} txn_memtable_table_first_frame_0_t;

/* txn_memtable_table_first_pad_0 is first's pad: the guard, the With, the first page, the memtable, the range, and the pages, while live. */
static void
txn_memtable_table_first_pad_0(void *frame_)
{
	txn_memtable_table_first_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	if (frame->with_live)
	{
		frame->with_live = false;
		host_delta_With_drop(&frame->with);
	}
	txn_memtable_table_OnePage_drop(&frame->page);
	if (frame->has_given)
	{
		frame->has_given = false;
		host_delta_Owned_drop(&frame->given);
	}
	if (frame->memtable_live)
	{
		frame->memtable_live = false;
		host_delta_Made_drop(&frame->memtable);
	}
	if (frame->range_live)
	{
		frame->range_live = false;
		host_delta_Made_drop(&frame->range);
	}
	if (frame->pages_live)
	{
		frame->pages_live = false;
		std_vec_clear(&frame->pages, sizeof(HostDeltaOwned), host_delta_Owned_drop);
		std_vec_free_Checked(&frame->pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	}
}

/*
 * txn_memtable_table_first_made takes the first range's page it was given and
 * makes the range and the memtable in one try, answering the delta's refusal
 * of a record, with what it took given back by the pad, rather than waiting,
 * and false on an error.
 */
static bool
txn_memtable_table_first_made(txn_memtable_table_first_frame_0_t *frame)
{
	bool		allocated;

	std_vec_with_capacity_Checked(&frame->pages, 1, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	std_vec_push_Checked(&frame->pages, &frame->given, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	frame->has_given = false;
	frame->pages_live = true;
	if (!host_delta_make(frame->io, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, &allocated, &frame->range, frame->full,
						 frame->error))
		return false;
	if (!allocated)
	{
		*frame->refused = true;
		return true;
	}
	frame->range_live = true;
	if (!host_delta_make(frame->io, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, &allocated, &frame->memtable, frame->full,
						 frame->error))
		return false;
	if (!allocated)
	{
		*frame->refused = true;
		return true;
	}
	frame->memtable_live = true;
	frame->page.has = std_vec_pop(&frame->pages, &frame->page.page, sizeof(HostDeltaOwned));
	if (!frame->page.has)
	{
		*frame->error = txn_memtable_table_internal("an allocation gave no page");
		return false;
	}
	return true;
}

/* txn_memtable_table_filled fills one of a made record's lists with its one entry while it is unlinked. */
bool
txn_memtable_table_filled(const HostDeltaMade *made, const HostSharedShapeOps *shape, size_t list, uint64 entry,
						  HostError *error)
{
	StdVec		spare;
	StdVec		filled;

	std_vec_new(&spare, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	if (!host_delta_list_Made_fill_list(made, shape, list, &entry, 1, &spare, &filled, error))
		return false;
	std_vec_clear(&filled, sizeof(HostDeltaOwned), host_delta_Owned_drop);
	std_vec_free_Checked(&filled, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	return true;
}

/*
 * txn_memtable_table_first_linked makes, once its records are taken, the
 * memtable on its page: the empty key as its range's first and each record's
 * list with its one entry; then, under the owner's lock, one link of the
 * memtable into the tree's word makes them live together if the word still
 * names none. It answers false on an error, the pad giving back what isn't
 * linked.
 */
static bool
txn_memtable_table_first_linked(txn_memtable_table_first_frame_0_t *frame)
{
	uint64		location;
	HostDeltaPageSize size = txn_memtable_table_first_size();
	uint64		name;
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE};
	StdIterator pages;
	HostSharedPast word;

	if (!txn_memtable_index_begin(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &frame->range, &frame->page.page, 0, NULL, 0,
								  frame->error))
		return false;
	name = host_delta_page_Page_value(host_delta_Owned_name(&frame->page.page));
	if (!txn_memtable_table_filled(&frame->range, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES,
								   name, frame->error))
		return false;
	op.u.exchange.f1 = name;
	host_delta_Made_atomic(&frame->range, TXN_MEMTABLE_RANGE_RANGE_START, op);
	op.u.exchange.f1 = (uint64) size.words;
	host_delta_Made_atomic(&frame->range, TXN_MEMTABLE_RANGE_RANGE_NEWEST, op);
	if (!txn_memtable_table_filled(&frame->memtable, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape,
								   TXN_MEMTABLE_RANGE_RANGES, host_delta_Made_location(&frame->range), frame->error))
		return false;
	frame->with = host_delta_With_Default_default();
	frame->with_live = true;
	pages = (StdIterator)
	{
		&TXN_MEMTABLE_TABLE_ONE_PAGE_ITERATOR_OPS, &frame->page
	};
	frame->range_live = false;
	if (!host_delta_With_record(&frame->with, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, TXN_MEMTABLE_RANGE_RANGE_LOCK,
								&frame->range, pages, frame->error) ||
		!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-memtable-first", frame->error) ||
		!host_shared_Record_lock(frame->owner, HOST_IO_IO_TAKES_SHORT(frame->io), frame->tree->lock, &frame->guard,
								 frame->error))
		return false;
	frame->guard_live = true;
	word = host_shared_Record_names_past(frame->owner, frame->tree->word);
	if (!host_shared_Past_load(&word, &location))
	{
		HostDeltaNames names = {&HOST_DELTA_PAST_NAMES_OPS, &word};

		frame->memtable_live = false;
		frame->with_live = false;
		host_delta_Made_link(&frame->memtable, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, names, &frame->with);
	}
	return true;
}

/*
 * txn_memtable_table_prepared_firsts takes, for every memtable the try's
 * trees lack, its range's and memtable's records on the first page the try's
 * allocation took for it, all before any memtable is made, so the refusal of
 * any one's records gives every record and page taken back and leaves no
 * memtable. It answers false on an error.
 */
static bool
txn_memtable_table_prepared_firsts(txn_memtable_table_prepared_frame_0_t *frame, bool *refused, HostDeltaFull *full)
{
	size_t		missing = frame->missing.len;

	std_vec_new(&frame->firsts, sizeof(txn_memtable_table_first_frame_0_t), _Alignof(txn_memtable_table_first_frame_0_t));
	frame->firsts_live = true;
	if (!std_vec_try_reserve_exact(&frame->firsts, missing, sizeof(txn_memtable_table_first_frame_0_t),
								   _Alignof(txn_memtable_table_first_frame_0_t)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	for (size_t i = 0; i < missing; i++)
	{
		const TxnMemtableTableAim *aim = &frame->aims[STD_VEC_AT(&frame->missing, size_t, missing - 1 - i)];
		txn_memtable_table_first_frame_0_t first = {.tree = aim->tree, .io = frame->io, .owner = aim->owner,
			.error = frame->error, .refused = refused, .full = full,
		.given = STD_VEC_AT(&frame->pages, HostDeltaOwned, frame->pages.len - 1), .has_given = true};
		uint64		location;

		frame->pages.len--;
		std_vec_push_Checked(&frame->firsts, &first, sizeof(first), _Alignof(txn_memtable_table_first_frame_0_t));
		if (txn_memtable_table_memtable(aim->tree, aim->owner, &location))
			continue;
		if (!txn_memtable_table_first_made(&STD_VEC_AT(&frame->firsts, txn_memtable_table_first_frame_0_t, i)))
			return false;
		if (*refused)
		{
			txn_memtable_table_prepared_firsts_drop(frame);
			return true;
		}
	}
	return true;
}

/*
 * txn_memtable_table_prepared_linked makes and links each memtable whose
 * records the try took, then hands its aim's push off to its key's range,
 * answering some false when a range gave out and false on an error.
 */
static bool
txn_memtable_table_prepared_linked(txn_memtable_table_prepared_frame_0_t *frame, bool *some)
{
	size_t		missing = frame->missing.len;

	*some = true;
	for (size_t i = 0; i < frame->firsts.len && *some; i++)
	{
		txn_memtable_table_first_frame_0_t *first = &STD_VEC_AT(&frame->firsts, txn_memtable_table_first_frame_0_t, i);
		size_t		k = STD_VEC_AT(&frame->missing, size_t, missing - 1 - i);
		const TxnMemtableTableAim *aim = &frame->aims[k];
		bool		linked = !first->memtable_live || txn_memtable_table_first_linked(first);

		txn_memtable_table_first_pad_0(first);
		if (!linked ||
			!txn_memtable_table_handed(aim->tree, frame->io, &frame->prepared.hold, &frame->mine, aim->owner,
									   aim->key.ptr, aim->key.len, some, &STD_VEC_AT(&frame->ats, TxnMemtableTableAt, k),
									   frame->error))
			return false;
	}
	txn_memtable_table_prepared_firsts_drop(frame);
	frame->missing.len = 0;
	return true;
}

/* txn_memtable_table_prepared_firsts_drop gives back what each unlinked memtable's records and first page hold, while live. */
static void
txn_memtable_table_prepared_firsts_drop(txn_memtable_table_prepared_frame_0_t *frame)
{
	if (!frame->firsts_live)
		return;
	frame->firsts_live = false;
	for (size_t i = 0; i < frame->firsts.len; i++)
		txn_memtable_table_first_pad_0(&STD_VEC_AT(&frame->firsts, txn_memtable_table_first_frame_0_t, i));
	std_vec_free_Checked(&frame->firsts, sizeof(txn_memtable_table_first_frame_0_t),
						 _Alignof(txn_memtable_table_first_frame_0_t));
}

#ifdef DOLTGRES_TEST_TXN
/* txn_memtable_table_first_body_0 is made's body. */
static void
txn_memtable_table_first_body_0(void *frame_)
{
	txn_memtable_table_first_frame_0_t *frame = frame_;
	uint64		location;

	if (txn_memtable_table_memtable(frame->tree, frame->owner, &location))
	{
		frame->ok = true;
		return;
	}
	frame->ok = txn_memtable_table_first_made(frame) && (*frame->refused || txn_memtable_table_first_linked(frame));
	txn_memtable_table_first_pad_0(frame);
}

/*
 * txn_memtable_table_made_on makes a tree's memtable as its first writer in
 * one try, on the first page a prepare's allocation took with its reserves:
 * with no lock it makes the range and the memtable on the page, writes the
 * empty key there as the range's first, and starts each record's list with
 * its one entry; then, under the owner's lock, one link of the memtable into
 * the tree's word makes them live together if the word still names none,
 * and otherwise its drops give everything back. It gives the page back for a
 * tree whose word names a memtable, and answers the delta's refusal of a
 * record, with what it took given back, rather than waiting.
 */
static bool
txn_memtable_table_made_on(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
						   HostDeltaOwned page, bool *refused, HostDeltaFull *full, HostError *error)
{
	txn_memtable_table_first_frame_0_t frame = {.tree = tree, .io = io, .owner = owner, .error = error,
	.refused = refused, .full = full, .given = page, .has_given = true};

	*refused = false;
	std_landing(&frame, txn_memtable_table_first_body_0, txn_memtable_table_first_pad_0);
	if (frame.has_given)
		host_delta_Owned_drop(&frame.given);
	return frame.ok;
}

/* txn_memtable_table_first makes a tree's memtable as its first writer, as made_on does on a page it takes, waiting at the delta's budget when it refuses. */
bool
txn_memtable_table_first(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner, HostError *error)
{
	for (;;)
	{
		HostDeltaPageSize size = txn_memtable_table_first_size();
		bool		allocated;
		bool		refused = true;
		StdVec		pages;
		HostDeltaFull full;

		if (!host_delta_alloc(io, &size, 1, &allocated, &pages, &full, error))
			return false;
		if (allocated)
		{
			HostDeltaOwned page = STD_VEC_AT(&pages, HostDeltaOwned, 0);

			std_vec_free_Checked(&pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
			if (!txn_memtable_table_made_on(tree, io, owner, page, &refused, &full, error))
				return false;
		}
		if (!refused)
			return true;
		if (!txn_memtable_table_waited(io, full, false, error))
			return false;
	}
}
#endif

/* txn_memtable_table_abandoned_frame_0_t is abandoned's state across its pad: the hold and the range's guard, while live. */
typedef struct txn_memtable_table_abandoned_frame_0_t
{
	const TxnMemtableTableTreeOps *tree;
	HostIoIo   *io;
	HostSharedRecord owner;
	uint64		location;
	bool	   *result;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedGuard guard;
	bool		guard_live;
} txn_memtable_table_abandoned_frame_0_t;

/* txn_memtable_table_abandoned_pad_0 is abandoned's pad: the guard, then the hold, while live. */
static void
txn_memtable_table_abandoned_pad_0(void *frame_)
{
	txn_memtable_table_abandoned_frame_0_t *frame = frame_;

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
}

/* txn_memtable_table_names reports whether a tree's owner's word still names a memtable. */
bool
txn_memtable_table_names(const TxnMemtableTableTreeOps *tree, HostSharedRecord owner, uint64 location)
{
	uint64		named;

	return txn_memtable_table_memtable(tree, owner, &named) && named == location;
}

/*
 * txn_memtable_table_only reads, under a memtable's shared lock, its only
 * range when it is open with one, answering false on an error, setting
 * listed when its list holds more than one range, and some for the range.
 */
static bool
txn_memtable_table_only(txn_memtable_table_abandoned_frame_0_t *frame, bool *more, bool *some, uint64 *range)
{
	HostSharedRecord record;
	HostSharedGuard guard;
	HostDeltaListListView view;
	uint64		len;
	bool		ok = true;

	*more = false;
	*some = false;
	if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, frame->location, &record,
								frame->error) ||
		!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_MEMTABLE_LOCK,
										&guard, frame->error))
		return false;
	view = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape, TXN_MEMTABLE_RANGE_RANGES);
	len = host_delta_list_ListView_len(&view);
	if (!txn_memtable_index_closed(&TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS, &guard))
	{
		if (len == 1)
		{
			ok = host_delta_list_ListView_get(&view, &frame->hold, 0, range, frame->error);
			*some = ok;
		}
		else if (len != 0)
			*more = true;
	}
	host_shared_Guard_drop(&guard);
	return ok;
}

/* txn_memtable_table_abandoned_body_0 is abandoned's body. */
static void
txn_memtable_table_abandoned_body_0(void *frame_)
{
	txn_memtable_table_abandoned_frame_0_t *frame = frame_;
	bool		more;
	bool		some;
	uint64		range;
	HostSharedRecord record;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	TxnMemtableIndexNode node;
	TxnMemtableIndexKeyedOpen open;
	bool		empty;
	bool		closed = false;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	*frame->result = true;
	if (!txn_memtable_table_names(frame->tree, frame->owner, frame->location))
	{
		frame->ok = true;
		txn_memtable_table_abandoned_pad_0(frame);
		return;
	}
	if (!txn_memtable_table_only(frame, &more, &some, &range))
	{
		txn_memtable_table_abandoned_pad_0(frame);
		return;
	}
	if (more)
	{
		*frame->result = false;
		frame->ok = true;
		txn_memtable_table_abandoned_pad_0(frame);
		return;
	}
	if (!some)
	{
		frame->ok = txn_memtable_table_emptied(frame->tree, frame->io, &frame->hold, frame->owner, frame->location,
											   frame->result, frame->error);
		txn_memtable_table_abandoned_pad_0(frame);
		return;
	}
	if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, range, &record, frame->error) ||
		!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_RANGE_LOCK,
								 &frame->guard, frame->error))
	{
		txn_memtable_table_abandoned_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	empty = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_KEYS, load) == 1;
	if (empty)
	{
		if (txn_memtable_index_first(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &frame->guard, &node))
		{
			uint64		head;

			if (!txn_memtable_index_head(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &frame->guard, &frame->hold, node,
										 &head, frame->error))
			{
				txn_memtable_table_abandoned_pad_0(frame);
				return;
			}
			empty = head == TXN_MEMTABLE_INDEX_NONE;
		}
		else
			empty = false;
	}
	if (!host_pause(HOST_SHARED_GUARD_PAUSES(&frame->guard), "doltgres-memtable-abandoned", frame->error))
	{
		txn_memtable_table_abandoned_pad_0(frame);
		return;
	}
	if (txn_memtable_index_keyed_open(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &frame->guard, &open) && empty)
	{
		bool		pinned;

		if (!txn_memtable_pins_pinned(frame->tree->pin, frame->location, &pinned, frame->error))
		{
			txn_memtable_table_abandoned_pad_0(frame);
			return;
		}
		if (!pinned)
		{
			txn_memtable_index_Open_close(open.open);
			closed = true;
		}
	}
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	if (closed)
	{
		TxnMemtableTableAt at = {frame->location, range};

		if (!txn_memtable_table_unlisted(frame->tree, frame->io, &frame->hold, frame->owner, at, frame->error))
		{
			txn_memtable_table_abandoned_pad_0(frame);
			return;
		}
	}
	*frame->result = !txn_memtable_table_names(frame->tree, frame->owner, frame->location);
	frame->ok = true;
	txn_memtable_table_abandoned_pad_0(frame);
}

/*
 * txn_memtable_table_abandoned closes a tree's memtable that a prepare made
 * or pinned and no version reached, when its owner's word still names it:
 * open, with its first range its only one, that range open and holding only
 * the empty key with no version, and no process's pin naming the memtable.
 * It reads the list under the memtable's shared lock, and the rest under the
 * range's exclusive lock, which a prepare takes after storing its pin, so
 * either the prepare finds the range closed or this finds its pin; then it
 * takes the range out as any finder of a closed range does, which closes the
 * memtable. A memtable whose list a pin left empty is closed as emptied
 * closes it. It answers whether the word no longer names the memtable.
 */
bool
txn_memtable_table_abandoned(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
							 uint64 location, bool *result, HostError *error)
{
	txn_memtable_table_abandoned_frame_0_t frame = {.tree = tree, .io = io, .owner = owner, .location = location,
	.result = result, .error = error};

	std_landing(&frame, txn_memtable_table_abandoned_body_0, txn_memtable_table_abandoned_pad_0);
	return frame.ok;
}

/* txn_memtable_table_u64_lt is the fn item u64's PartialOrd::lt's FnMut shim, which sort_unstable passes. */
static bool
txn_memtable_table_u64_lt(void *captures, const void *a, const void *b)
{
	(void) captures;
	return *(const uint64 *) a < *(const uint64 *) b;
}

/* The removal's sort of the pages it looked at: sort_unstable at u64. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS U64
#define STD_SORT_T uint64
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT txn_memtable_table_u64_lt
#include "doltgres/std/sort_template.h"

/* txn_memtable_table_u64_eq is u64's PartialEq, which the looked-at pages' dedup takes. */
static bool
txn_memtable_table_u64_eq(const void *self, const void *other)
{
	return *(const uint64 *) self == *(const uint64 *) other;
}

static bool txn_memtable_table_dropped_pages(TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
											 const uint64 *entries, size_t entries_len, StdVec *result,
											 HostError *error);
static bool txn_memtable_table_compactable(const TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
										   uint64 first, const uint64 *entries, size_t entries_len, StdVec *result,
										   HostError *error);
static bool txn_memtable_table_compacted(HostIoIo *io, const HostDeltaHold *hold, uint64 range, StdVec *found,
										 bool *result, HostError *error);
static void txn_memtable_table_Compactable_drop(void *self);

/* txn_memtable_table_bools_drop drops one key's answers, a Vec<bool>. */
static void
txn_memtable_table_bools_drop(void *self)
{
	std_vec_free_Checked((StdVec *) self, sizeof(bool), _Alignof(bool));
}

/* txn_memtable_table_DroppedPage_t is a page a removal dropped from its range's list: its name and its count of lists. */
typedef struct txn_memtable_table_DroppedPage_t
{
	uint64		f0;
	HostDeltaWord f1;
} txn_memtable_table_DroppedPage_t;

/* txn_memtable_table_removed_frame_0_t is removed_all's state across its pad: the answers, the hold, the range's guard, and the lists of a range's take, while live. */
typedef struct txn_memtable_table_removed_frame_0_t
{
	const TxnMemtableTableTreeOps *tree;
	HostIoIo   *io;
	HostSharedRecord owner;
	const TxnMemtableTableRemoving *keys;
	size_t		keys_len;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	StdVec		taken;
	bool		taken_live;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedGuard guard;
	bool		guard_live;
	StdVec		looked;
	bool		looked_live;
	StdVec		touched;
	bool		touched_live;
	StdVec		dropped;
	bool		dropped_live;
	StdVec		found;
	bool		found_live;
	StdVec		entries;
	bool		entries_live;
	StdVec		names;
	bool		names_live;
	StdVec		counted;
	bool		counted_live;
	StdVec	   *given;
} txn_memtable_table_removed_frame_0_t;

/* txn_memtable_table_removed_take_pad drops the lists of one range's take, while live. */
static void
txn_memtable_table_removed_take_pad(txn_memtable_table_removed_frame_0_t *frame)
{
	if (frame->counted_live)
	{
		frame->counted_live = false;
		std_vec_free_Checked(&frame->counted, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->names_live)
	{
		frame->names_live = false;
		std_vec_free_Checked(&frame->names, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->found_live)
	{
		frame->found_live = false;
		std_vec_clear(&frame->found, sizeof(TxnMemtableTableCompactable), txn_memtable_table_Compactable_drop);
		std_vec_free_Checked(&frame->found, sizeof(TxnMemtableTableCompactable),
							 _Alignof(TxnMemtableTableCompactable));
	}
	if (frame->dropped_live)
	{
		frame->dropped_live = false;
		std_vec_free_Checked(&frame->dropped, sizeof(txn_memtable_table_DroppedPage_t),
							 _Alignof(txn_memtable_table_DroppedPage_t));
	}
	if (frame->touched_live)
	{
		frame->touched_live = false;
		std_vec_free_Checked(&frame->touched, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->looked_live)
	{
		frame->looked_live = false;
		std_vec_free_Checked(&frame->looked, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
}

/* txn_memtable_table_removed_pad_0 is removed_all's pad: a take's lists, the hold, and the answers, while live. */
static void
txn_memtable_table_removed_pad_0(void *frame_)
{
	txn_memtable_table_removed_frame_0_t *frame = frame_;

	txn_memtable_table_removed_take_pad(frame);
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	if (frame->taken_live)
	{
		frame->taken_live = false;
		std_vec_clear(&frame->taken, sizeof(StdVec), txn_memtable_table_bools_drop);
		std_vec_free_Checked(&frame->taken, sizeof(StdVec), _Alignof(StdVec));
	}
}

/*
 * txn_memtable_table_removed_keys takes out, under an open range's lock,
 * every key of the run from next that the range holds, as removed_all's
 * inner loop does, answering false on an error and moving next past the
 * run.
 */
static bool
txn_memtable_table_removed_keys(txn_memtable_table_removed_frame_0_t *frame, TxnMemtableIndexKeyedOpen *open,
								TxnMemtableIndexNode first, size_t *next)
{
	size_t		start = *next;

	while (*next < frame->keys_len)
	{
		const TxnMemtableTableRemoving *key = &frame->keys[*next];
		StdVec	   *taken = &STD_VEC_AT(&frame->taken, StdVec, *next);
		bool		some;
		TxnMemtableIndexNode node;
		bool		emptied;
		uint64		bytes;

		if (!txn_memtable_index_find(open->keyed, open->open.guard, &frame->hold, key->key.ptr, key->key.len, &some,
									 &node, frame->error))
			return false;
		if (!some && *next > start)
			break;
		if (some)
		{
			if (!txn_memtable_version_removed(open, &frame->hold, node, key->handles, key->handles_len, taken->ptr,
											  taken->len, frame->given, &emptied, &frame->touched, &bytes,
											  frame->error))
				return false;
			frame->touched_live = true;
			if (bytes > 0)
				frame->tree->of->shrank(frame->owner, bytes);
			if (emptied && node.f0 != first.f0 && !txn_memtable_index_unlink(open, &frame->hold, node, frame->error))
				return false;
			if (!std_vec_try_reserve(&frame->looked, frame->touched.len, sizeof(uint64), _Alignof(uint64)))
			{
				*frame->error = host_Error_out_of_memory();
				return false;
			}
			std_vec_extend_from_slice_Checked(&frame->looked, frame->touched.ptr, frame->touched.len, sizeof(uint64),
											  _Alignof(uint64));
			frame->touched_live = false;
			std_vec_free_Checked(&frame->touched, sizeof(uint64), _Alignof(uint64));
		}
		*next = STD_ADD(usize, *next, 1, 976, 17);
	}
	return true;
}

/*
 * txn_memtable_table_removed_range runs one range's take of removed_all
 * under its exclusive lock, answering false on an error, setting retry when
 * the range was closed and taken out of the list, and leaving the take's
 * results in the frame and closed and floor set.
 */
static bool
txn_memtable_table_removed_range(txn_memtable_table_removed_frame_0_t *frame, TxnMemtableTableAt at,
								 HostSharedRecord record, size_t *next, bool *retry, bool *closed, bool *floor)
{
	TxnMemtableIndexKeyedOpen open;
	TxnMemtableIndexNode first;
	size_t		start = *next;
	HostDeltaListListView view;
	uint64		count;
	uint64		removed = 0;
	uint64		before;
	bool		walked;
	bool		compacting;
	bool		empty;
	uint64		head;
	HostDeltaLent first_key;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};
	HostSharedOp add = {.tag = HOST_SHARED_OP_ADD, .u.add = 1};

	*retry = false;
	if (!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_RANGE_LOCK,
								 &frame->guard, frame->error))
		return false;
	frame->guard_live = true;
	if (!txn_memtable_index_keyed_open(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &frame->guard, &open))
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
		*retry = true;
		return txn_memtable_table_unlisted(frame->tree, frame->io, &frame->hold, frame->owner, at, frame->error);
	}
	if (!txn_memtable_index_first(open.keyed, open.open.guard, &first))
	{
		*frame->error = txn_memtable_table_internal("an open range has no first node");
		return false;
	}
	std_vec_new(&frame->looked, sizeof(uint64), _Alignof(uint64));
	frame->looked_live = true;
	if (!txn_memtable_table_removed_keys(frame, &open, first, next))
		return false;
	view = host_delta_list_Guard_list(open.open.guard, open.open.closes->shape, TXN_MEMTABLE_INDEX_PAGES);
	count = host_delta_list_ListView_len(&view);
	for (size_t k = start; k < *next; k++)
	{
		const StdVec *taken = &STD_VEC_AT(&frame->taken, StdVec, k);

		for (size_t i = 0; i < taken->len; i++)
			if (STD_VEC_AT(taken, bool, i))
				removed = std_wrapping_add_u64(removed, 1);
	}
	if (removed > 0)
		host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_MOVED, add);
	before = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_SWEPT, load);
	walked = STD_ADD(u64, before, removed, 984, 26) >= count;
	exchange.u.exchange.f0 = before;
	exchange.u.exchange.f1 = walked ? 0 : STD_ADD(u64, before, removed, 985, 85);
	host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_SWEPT, exchange);
	if (walked)
	{
		frame->looked.len = 0;
		if (!std_vec_try_reserve(&frame->looked, (size_t) count, sizeof(uint64), _Alignof(uint64)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		for (uint64 entry = 0; entry < count; entry++)
			std_vec_push_Checked(&frame->looked, &entry, sizeof(entry), _Alignof(uint64));
	}
	std_slice_sort_unstable_U64(frame->looked.ptr, frame->looked.len);
	std_vec_dedup(&frame->looked, sizeof(uint64), _Alignof(uint64), txn_memtable_table_u64_eq, NULL);
	before = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_REMOVED, load);
	compacting = STD_MUL(u64, STD_ADD(u64, before, removed, 992, 31), 2,
						 992, 30) >=
		host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_KEYS, load);
	exchange.u.exchange.f0 = before;
	exchange.u.exchange.f1 = compacting ? 0 : STD_ADD(u64, before, removed, 993, 91);
	host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_REMOVED, exchange);
	if (!txn_memtable_table_dropped_pages(&open, &frame->hold, frame->looked.ptr, frame->looked.len, &frame->dropped,
										  frame->error))
		return false;
	frame->dropped_live = true;
	empty = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_KEYS, load) == 1;
	if (empty)
	{
		if (!txn_memtable_index_head(open.keyed, open.open.guard, &frame->hold, first, &head, frame->error))
			return false;
		empty = head == TXN_MEMTABLE_INDEX_NONE;
	}
	if (!txn_memtable_table_first_key(&frame->hold, at.range, &first_key, frame->error))
		return false;
	*floor = host_delta_Lent_Deref_deref(&first_key).len == 0;
	host_delta_Lent_drop(&first_key);
	*closed = empty && !*floor;
	std_vec_new(&frame->found, sizeof(TxnMemtableTableCompactable), _Alignof(TxnMemtableTableCompactable));
	frame->found_live = true;
	if (!*closed && compacting)
	{
		std_vec_with_capacity_Checked(&frame->entries, (size_t) count, sizeof(uint64), _Alignof(uint64));
		frame->entries_live = true;
		for (uint64 entry = 0; entry < count; entry++)
			std_vec_push_Checked(&frame->entries, &entry, sizeof(entry), _Alignof(uint64));
		if (!txn_memtable_table_compactable(&open, &frame->hold, txn_memtable_index_Node_page(first),
											frame->entries.ptr, frame->entries.len, &frame->found, frame->error))
			return false;
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(uint64), _Alignof(uint64));
	}
	if (*closed)
	{
		std_vec_new(&frame->names, sizeof(uint64), _Alignof(uint64));
		frame->names_live = true;
		for (uint64 entry = txn_memtable_index_Node_page(first), len = host_delta_list_ListView_len(&view); entry < len;
			 entry++)
		{
			uint64		name;

			if (!host_delta_list_ListView_get(&view, &frame->hold, entry, &name, frame->error))
				return false;
			std_vec_push_Checked(&frame->names, &name, sizeof(name), _Alignof(uint64));
		}
		for (size_t k = 0; k < frame->names.len; k++)
		{
			uint64		name = STD_VEC_AT(&frame->names, uint64, k);
			HostDeltaPageRef page;

			if (name == TXN_MEMTABLE_PAGES_DROPPED)
				continue;
			if (!host_delta_Hold_page(&frame->hold, host_delta_page_Page_named(name), &page, frame->error) ||
				!host_delta_PageRef_seal(&page, open.open.closes->shape, open.open.closes->lock, open.open.guard,
										 frame->error))
				return false;
		}
		frame->names_live = false;
		std_vec_free_Checked(&frame->names, sizeof(uint64), _Alignof(uint64));
		txn_memtable_index_Open_close(open.open);
	}
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	return true;
}

/* txn_memtable_table_Retiring_next gives the next page's name as an Ok item. */
static bool
txn_memtable_table_Retiring_next(void *self_, void *result)
{
	TxnMemtableTableRetiring *self = self_;
	HostDeltaRetireItem *item = result;

	if (self->at >= self->len)
		return false;
	item->is_ok = true;
	item->ok = host_delta_page_Page_named(self->names[self->at]);
	self->at = std_wrapping_add_usize(self->at, 1);
	return true;
}

/* txn_memtable_table_Retiring_size_hint answers how many names are left. */
static StdSizeHint
txn_memtable_table_Retiring_size_hint(const void *self_)
{
	const TxnMemtableTableRetiring *self = self_;
	size_t		left = std_wrapping_sub_usize(self->len, self->at);
	StdSizeHint hint = {left, true, left};

	return hint;
}

/* TXN_MEMTABLE_TABLE_RETIRING_ITERATOR_OPS is the retiring names' Iterator, whose drop does nothing. */
const StdIteratorOps TXN_MEMTABLE_TABLE_RETIRING_ITERATOR_OPS = {
	txn_memtable_table_Retiring_next, txn_memtable_table_Retiring_size_hint, NULL,
};

/*
 * txn_memtable_table_removed_after lowers the list counts of the pages a
 * range's take dropped, retiring each whose last list it was, compacts what
 * it found worth compacting, and takes a closed range out or closes an
 * abandoned memtable, once the range's lock is released.
 */
static bool
txn_memtable_table_removed_after(txn_memtable_table_removed_frame_0_t *frame, TxnMemtableTableAt at, bool closed,
								 bool floor)
{
	size_t		kept = 0;
	bool		ok = true;

	for (size_t k = 0; k < frame->dropped.len; k++)
	{
		txn_memtable_table_DroppedPage_t *page = &STD_VEC_AT(&frame->dropped, txn_memtable_table_DroppedPage_t, k);

		if (host_delta_Word_fetch_add(&page->f1, PG_UINT64_MAX) == 1)
			STD_VEC_AT(&frame->dropped, txn_memtable_table_DroppedPage_t, kept++).f0 = page->f0;
	}
	if (kept > 0)
	{
		TxnMemtableTableRetiring names;
		StdIterator pages;

		std_vec_with_capacity_Checked(&frame->counted, kept, sizeof(uint64), _Alignof(uint64));
		frame->counted_live = true;
		for (size_t k = 0; k < kept; k++)
			std_vec_push_Checked(&frame->counted, &STD_VEC_AT(&frame->dropped, txn_memtable_table_DroppedPage_t, k).f0,
								 sizeof(uint64), _Alignof(uint64));
		names.names = frame->counted.ptr;
		names.len = frame->counted.len;
		names.at = 0;
		pages.ops = &TXN_MEMTABLE_TABLE_RETIRING_ITERATOR_OPS;
		pages.self = &names;
		ok = host_delta_retire(frame->io, NULL, 0, pages, frame->error);
		frame->counted_live = false;
		std_vec_free_Checked(&frame->counted, sizeof(uint64), _Alignof(uint64));
		if (!ok)
			return false;
	}
	if (frame->found.len > 0)
	{
		bool		replaced;

		frame->found_live = false;
		if (!txn_memtable_table_compacted(frame->io, &frame->hold, at.range, &frame->found, &replaced, frame->error))
			return false;
	}
	if (closed)
		return txn_memtable_table_unlisted(frame->tree, frame->io, &frame->hold, frame->owner, at, frame->error);
	if (floor)
	{
		bool		abandoned;

		return txn_memtable_table_abandoned(frame->tree, frame->io, frame->owner, at.memtable, &abandoned,
											frame->error);
	}
	return true;
}

/* txn_memtable_table_removed_body_0 is removed_all's body. */
static void
txn_memtable_table_removed_body_0(void *frame_)
{
	txn_memtable_table_removed_frame_0_t *frame = frame_;
	size_t		next = 0;

	std_vec_new(&frame->taken, sizeof(StdVec), _Alignof(StdVec));
	frame->taken_live = true;
	if (!std_vec_try_reserve_exact(&frame->taken, frame->keys_len, sizeof(StdVec), _Alignof(StdVec)))
	{
		*frame->error = host_Error_out_of_memory();
		txn_memtable_table_removed_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->keys_len; k++)
	{
		StdVec		each;
		bool		no = false;

		std_vec_new(&each, sizeof(bool), _Alignof(bool));
		if (!std_vec_try_reserve_exact(&each, frame->keys[k].handles_len, sizeof(bool), _Alignof(bool)))
		{
			*frame->error = host_Error_out_of_memory();
			txn_memtable_table_removed_pad_0(frame);
			return;
		}
		for (size_t i = 0; i < frame->keys[k].handles_len; i++)
			std_vec_push_Checked(&each, &no, sizeof(no), _Alignof(bool));
		std_vec_push_Checked(&frame->taken, &each, sizeof(each), _Alignof(StdVec));
	}
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
	{
		txn_memtable_table_removed_pad_0(frame);
		return;
	}
	frame->hold_live = true;
	while (next < frame->keys_len)
	{
		bool		some;
		TxnMemtableTableAt at;
		HostSharedRecord record;
		bool		retry;
		bool		closed;
		bool		floor;

		if (!txn_memtable_table_found(frame->tree, frame->io, &frame->hold, frame->owner, frame->keys[next].key.ptr,
									  frame->keys[next].key.len, NULL, &some, &at, frame->error))
		{
			txn_memtable_table_removed_pad_0(frame);
			return;
		}
		if (!some)
		{
			next = STD_ADD(usize, next, 1, 945, 13);
			continue;
		}
		if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at.range, &record,
									frame->error) ||
			!txn_memtable_table_removed_range(frame, at, record, &next, &retry, &closed, &floor))
		{
			txn_memtable_table_removed_pad_0(frame);
			return;
		}
		if (retry)
			continue;
		if (!txn_memtable_table_removed_after(frame, at, closed, floor))
		{
			txn_memtable_table_removed_pad_0(frame);
			return;
		}
		txn_memtable_table_removed_take_pad(frame);
	}
	frame->taken_live = false;
	*frame->result = frame->taken;
	frame->ok = true;
	txn_memtable_table_removed_pad_0(frame);
}

/*
 * txn_memtable_table_removing is removed_all's walk, appending to given the
 * top-level xid of each version it takes out that its push counted.
 */
static bool
txn_memtable_table_removing(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
							const TxnMemtableTableRemoving *keys, size_t keys_len, StdVec *result, StdVec *given,
							HostError *error)
{
	txn_memtable_table_removed_frame_0_t frame = {.tree = tree, .io = io, .owner = owner, .keys = keys,
	.keys_len = keys_len, .result = result, .given = given, .error = error};

	std_landing(&frame, txn_memtable_table_removed_body_0, txn_memtable_table_removed_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_table_removed_all takes versions out of the chains of keys
 * given in byte order, as removed does for one key, each range taking out
 * every key of the run it holds under one take of its exclusive lock, with
 * one sweep of its dropped pages and one compaction after, as a spill takes
 * a transaction's keys out of its memtable. It answers, for each key at each
 * handle's place, whether this removal took it out. It gives back no count,
 * since a spill moves the versions to its file, whose own count names them.
 */
bool
txn_memtable_table_removed_all(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
							   const TxnMemtableTableRemoving *keys, size_t keys_len, StdVec *result,
							   HostError *error)
{
	StdVec		given;
	bool		ok;

	std_vec_new(&given, sizeof(uint64), _Alignof(uint64));
	ok = txn_memtable_table_removing(tree, io, owner, keys, keys_len, result, &given, error);
	std_vec_free_Checked(&given, sizeof(uint64), _Alignof(uint64));
	return ok;
}

/*
 * txn_memtable_table_removed takes versions out of a key's chain in one of a
 * tree's memtables, through the hand-off a writer takes and under the key's
 * range's exclusive lock: the versions leave the chain, a key left with none
 * leaves the index, and its dropped pages and its compaction are as
 * removed_all's. A key the tree doesn't hold, and a handle its chain
 * doesn't, are passed over. Each version it takes out that its push counted
 * gives its count back at its top-level transaction's entry, after the take.
 * It answers, at each handle's place, whether this removal took it out.
 */
bool
txn_memtable_table_removed(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
						   const uint8 *key, size_t key_len, const TxnMemtableVersionHandle *handles,
						   size_t handles_len, StdVec *result, HostError *error)
{
	TxnMemtableTableRemoving keys[1] = {{{key, key_len}, handles, handles_len}};
	StdVec		taken;
	StdVec		given;
	bool		ok;

	std_vec_new(&given, sizeof(uint64), _Alignof(uint64));
	ok = txn_memtable_table_removing(tree, io, owner, keys, 1, &taken, &given, error);
	for (size_t i = 0; ok && i < given.len;)
	{
		HostXid		top = {STD_VEC_AT(&given, uint64, i)};
		uint64		count = 0;
		bool		has;
		uint64		added;

		for (; i < given.len && STD_VEC_AT(&given, uint64, i) == top.f0; i++)
			count++;
		ok = txn_status_add(io, top, std_wrapping_sub_u64(0, count), &has, &added, error);
	}
	std_vec_free_Checked(&given, sizeof(uint64), _Alignof(uint64));
	if (!ok)
		return false;
	if (!std_vec_pop(&taken, result, sizeof(StdVec)))
		std_vec_new(result, sizeof(bool), _Alignof(bool));
	std_vec_clear(&taken, sizeof(StdVec), txn_memtable_table_bools_drop);
	std_vec_free_Checked(&taken, sizeof(StdVec), _Alignof(StdVec));
	return true;
}

/* txn_memtable_table_Pruning_t is a chain entry pruned reads: a version's handle, its writer, and its hint. */
typedef struct txn_memtable_table_Pruning_t
{
	TxnMemtableVersionHandle handle;
	TxnMemtableVersionWriter writer;
	TxnMemtableVersionHint hint;
} txn_memtable_table_Pruning_t;

/* txn_memtable_table_pruned_t is pruned's reading's captures: the key and the chain read. */
typedef struct txn_memtable_table_pruned_t
{
	const uint8 *key;
	size_t		key_len;
	StdVec		chain;
} txn_memtable_table_pruned_t;

/* txn_memtable_table_pruned_reading reads a key's chain's writers under its range's shared lock. */
static bool
txn_memtable_table_pruned_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
								  const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_table_pruned_t *pruned = captures;
	bool		some;
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;

	pruned->chain.len = 0;
	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, pruned->key, pruned->key_len,
								 &some, &node, error))
		return false;
	if (!some)
		return true;
	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error))
		return false;
	for (;;)
	{
		TxnMemtableVersionVersion version;
		TxnMemtableVersionRecord record;
		txn_memtable_table_Pruning_t entry;

		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, error))
		{
			pruned->chain.len = 0;
			return false;
		}
		if (!some)
			return true;
		record = txn_memtable_version_Version_record(&version);
		entry.handle = version.handle;
		entry.writer = txn_memtable_version_Record_writer(&record);
		entry.hint = txn_memtable_version_Version_hint(&version);
		txn_memtable_version_Version_drop(&version);
		std_vec_push_Checked(&pruned->chain, &entry, sizeof(entry), _Alignof(txn_memtable_table_Pruning_t));
	}
}

/* txn_memtable_table_aborting reports whether a version's writer is one of an abort's xids, or a subtransaction of one. */
bool
txn_memtable_table_aborting(TxnMemtableVersionWriter writer, const uint64 *aborting, size_t aborting_len)
{
	for (size_t k = 0; k < aborting_len; k++)
		if (writer.xid == aborting[k] || (writer.has_top && writer.top == aborting[k]))
			return true;
	return false;
}

/* txn_memtable_table_pruned_frame_0_t is pruned's state across its pad: the chain read, the aborted handles, and the answers, while live. */
typedef struct txn_memtable_table_pruned_frame_0_t
{
	const TxnMemtableTableTreeOps *tree;
	HostIoIo   *io;
	HostSharedRecord owner;
	const uint8 *key;
	size_t		key_len;
	const uint64 *aborting;
	size_t		aborting_len;

	/*
	 * listed takes out only the versions of aborting's xids, which ended
	 * passes as committed writers no fold still needs.
	 */
	bool		listed;
	bool	   *result;
	HostError  *error;
	bool		ok;
	txn_memtable_table_pruned_t pruned;
	bool		chain_live;
	StdVec		handles;
	bool		handles_live;
	StdVec		taken;
	bool		taken_live;
} txn_memtable_table_pruned_frame_0_t;

/* txn_memtable_table_pruned_pad_0 is pruned's pad: the answers, the handles, and the chain, while live. */
static void
txn_memtable_table_pruned_pad_0(void *frame_)
{
	txn_memtable_table_pruned_frame_0_t *frame = frame_;

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
	if (frame->chain_live)
	{
		frame->chain_live = false;
		std_vec_free_Checked(&frame->pruned.chain, sizeof(txn_memtable_table_Pruning_t),
							 _Alignof(txn_memtable_table_Pruning_t));
	}
}

/* txn_memtable_table_pruned_body_0 is pruned's body. */
static void
txn_memtable_table_pruned_body_0(void *frame_)
{
	txn_memtable_table_pruned_frame_0_t *frame = frame_;
	bool		some;
	size_t		kept = 0;

	frame->pruned.key = frame->key;
	frame->pruned.key_len = frame->key_len;
	std_vec_new(&frame->pruned.chain, sizeof(txn_memtable_table_Pruning_t), _Alignof(txn_memtable_table_Pruning_t));
	frame->chain_live = true;
	if (!txn_memtable_table_read(frame->tree, frame->io, frame->owner, frame->key, frame->key_len,
								 txn_memtable_table_pruned_reading, &frame->pruned, &some, frame->error))
	{
		txn_memtable_table_pruned_pad_0(frame);
		return;
	}
	if (!some)
		frame->pruned.chain.len = 0;
	for (size_t k = 0; k < frame->pruned.chain.len; k++)
	{
		txn_memtable_table_Pruning_t *entry = &STD_VEC_AT(&frame->pruned.chain, txn_memtable_table_Pruning_t, k);
		uint64		xid = entry->writer.xid;
		bool		gone;

		if (txn_memtable_table_aborting(entry->writer, frame->aborting, frame->aborting_len))
			gone = true;
		else if (frame->listed)
			gone = false;
		else if (entry->hint == TXN_MEMTABLE_VERSION_HINT_ABORTED)
			gone = true;
		else if (entry->hint == TXN_MEMTABLE_VERSION_HINT_COMMITTED)
			gone = false;
		else
		{
			HostStatus	status;
			HostXid		asked = {xid};

			gone = false;
			if (xid != TXN_MEMTABLE_VERSION_NO_TRANSACTION.xid)
			{
				if (!txn_outcome_stamped(frame->io, asked.f0, &status, frame->error))
				{
					txn_memtable_table_pruned_pad_0(frame);
					return;
				}
				gone = status == HOST_STATUS_ABORTED;
			}
		}
		if (gone)
			STD_VEC_AT(&frame->pruned.chain, txn_memtable_table_Pruning_t, kept++) = *entry;
	}
	frame->pruned.chain.len = kept;
	if (kept == 0)
	{
		frame->ok = true;
		txn_memtable_table_pruned_pad_0(frame);
		return;
	}
	std_vec_with_capacity_Checked(&frame->handles, kept, sizeof(TxnMemtableVersionHandle),
								  _Alignof(TxnMemtableVersionHandle));
	frame->handles_live = true;
	for (size_t k = 0; k < kept; k++)
		std_vec_push_Checked(&frame->handles, &STD_VEC_AT(&frame->pruned.chain, txn_memtable_table_Pruning_t, k).handle,
							 sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	if (!txn_memtable_table_removed(frame->tree, frame->io, frame->owner, frame->key, frame->key_len,
									frame->handles.ptr, frame->handles.len, &frame->taken, frame->error))
	{
		txn_memtable_table_pruned_pad_0(frame);
		return;
	}
	frame->taken_live = true;
	for (size_t k = 0; k < frame->taken.len && frame->result != NULL; k++)
		*frame->result |= STD_VEC_AT(&frame->taken, bool, k);
	frame->ok = true;
	txn_memtable_table_pruned_pad_0(frame);
}

/*
 * txn_memtable_table_pruned takes out of a key's chain in one of a tree's
 * memtables every version whose writer the commit log says aborted: the
 * chain's writers are read under the range's shared lock, each outcome is
 * asked with no lock held, a version hinted aborted taking no question and a
 * frozen one none, and the remover takes out the aborted ones, passing over
 * any a concurrent removal took first. A committed version is the fold's,
 * never pruned here. An abort names its xids as aborting, and every version
 * one of them or one of their subtransactions wrote is aborted with no
 * question (bugs row 332). The remover gives back the count of each counted
 * version it took out, so a rolled-back savepoint's versions leave a
 * committed transaction's count as they found it.
 */
bool
txn_memtable_table_pruned(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
						  const uint8 *key, size_t key_len, const uint64 *aborting, size_t aborting_len,
						  HostError *error)
{
	txn_memtable_table_pruned_frame_0_t frame = {.tree = tree, .io = io, .owner = owner, .key = key,
	.key_len = key_len, .aborting = aborting, .aborting_len = aborting_len, .error = error};

	std_landing(&frame, txn_memtable_table_pruned_body_0, txn_memtable_table_pruned_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_table_ended takes out of a key's chain the versions whose
 * writers, or their top-level transactions, are among ended, committed
 * transactions no fold still needs the versions of, as pruned takes out an
 * abort's, and adds to result whether it took one (bugs row 301).
 */
bool
txn_memtable_table_ended(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner, const uint8 *key,
						 size_t key_len, const uint64 *ended, size_t ended_len, bool *result, HostError *error)
{
	txn_memtable_table_pruned_frame_0_t frame = {.tree = tree, .io = io, .owner = owner, .key = key,
		.key_len = key_len, .aborting = ended, .aborting_len = ended_len, .listed = true, .result = result,
	.error = error};

	std_landing(&frame, txn_memtable_table_pruned_body_0, txn_memtable_table_pruned_pad_0);
	return frame.ok;
}

/* TxnMemtableTableEmptyPage is a page dropped_pages found with no live record: its entry, its name, and its page. */
typedef struct TxnMemtableTableEmptyPage
{
	uint64		entry;
	uint64		name;
	HostDeltaPageRef page;
} TxnMemtableTableEmptyPage;

/* txn_memtable_table_dropped_pages_frame_0_t is dropped_pages' state across its pad: the empty pages found and the pages dropped, while live. */
typedef struct txn_memtable_table_dropped_pages_frame_0_t
{
	TxnMemtableIndexKeyedOpen *open;
	const HostDeltaHold *hold;
	const uint64 *entries;
	size_t		entries_len;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	bool		result_live;
	/* empty is a Vec<EmptyPage>. */
	StdVec		empty;
	bool		empty_live;
} txn_memtable_table_dropped_pages_frame_0_t;

/* txn_memtable_table_dropped_pages_pad_0 is dropped_pages' pad: the empty pages, then the pages dropped, while live. */
static void
txn_memtable_table_dropped_pages_pad_0(void *frame_)
{
	txn_memtable_table_dropped_pages_frame_0_t *frame = frame_;

	if (frame->empty_live)
	{
		frame->empty_live = false;
		std_vec_free_Checked(&frame->empty, sizeof(TxnMemtableTableEmptyPage), _Alignof(TxnMemtableTableEmptyPage));
	}
	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_free_Checked(frame->result, sizeof(txn_memtable_table_DroppedPage_t),
							 _Alignof(txn_memtable_table_DroppedPage_t));
	}
}

/* txn_memtable_table_dropped_pages_empty gathers the pages of the entries but the newest whose count of live records is zero. */
static bool
txn_memtable_table_dropped_pages_empty(txn_memtable_table_dropped_pages_frame_0_t *frame, HostDeltaListListView *view)
{
	uint64		newest = std_saturating_sub_u64(host_delta_list_ListView_len(view), 1);

	for (size_t k = 0; k < frame->entries_len; k++)
	{
		TxnMemtableTableEmptyPage found = {.entry = frame->entries[k]};
		HostDeltaWord live;

		if (found.entry >= newest)
			continue;
		if (!host_delta_list_ListView_get(view, frame->hold, found.entry, &found.name, frame->error))
			return false;
		if (found.name == TXN_MEMTABLE_PAGES_DROPPED)
			continue;
		if (!host_delta_Hold_page(frame->hold, host_delta_page_Page_named(found.name), &found.page, frame->error) ||
			!host_delta_PageRef_word(&found.page, TXN_MEMTABLE_PAGES_LIVE, &live, frame->error))
			return false;
		if (host_delta_Word_load(&live) == 0)
			std_vec_push_Checked(&frame->empty, &found, sizeof(found), _Alignof(TxnMemtableTableEmptyPage));
	}
	return true;
}

/* txn_memtable_table_dropped_pages_body_0 is dropped_pages' body. */
static void
txn_memtable_table_dropped_pages_body_0(void *frame_)
{
	txn_memtable_table_dropped_pages_frame_0_t *frame = frame_;
	TxnMemtableIndexKeyedOpen *open = frame->open;
	HostDeltaListListView view = host_delta_list_Guard_list(open->open.guard, open->open.closes->shape,
															TXN_MEMTABLE_INDEX_PAGES);

	std_vec_new(&frame->empty, sizeof(TxnMemtableTableEmptyPage), _Alignof(TxnMemtableTableEmptyPage));
	frame->empty_live = true;
	std_vec_new(frame->result, sizeof(txn_memtable_table_DroppedPage_t), _Alignof(txn_memtable_table_DroppedPage_t));
	frame->result_live = true;
	if (!txn_memtable_table_dropped_pages_empty(frame, &view))
	{
		txn_memtable_table_dropped_pages_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->empty.len; k++)
	{
		TxnMemtableTableEmptyPage *found = &STD_VEC_AT(&frame->empty, TxnMemtableTableEmptyPage, k);
		HostDeltaListListMut list;
		txn_memtable_table_DroppedPage_t dropped = {found->name};

		if (!host_delta_PageRef_seal(&found->page, open->open.closes->shape, open->open.closes->lock, open->open.guard,
									 frame->error))
		{
			txn_memtable_table_dropped_pages_pad_0(frame);
			return;
		}
		list = host_delta_list_GuardExclusive_list_mut(open->open.guard, open->open.closes->shape,
													   open->open.closes->lock, TXN_MEMTABLE_INDEX_PAGES);
		if (!host_delta_list_ListMut_set(&list, frame->hold, found->entry, TXN_MEMTABLE_PAGES_DROPPED, frame->error) ||
			!host_delta_PageRef_word(&found->page, TXN_MEMTABLE_PAGES_LISTS, &dropped.f1, frame->error))
		{
			txn_memtable_table_dropped_pages_pad_0(frame);
			return;
		}
		std_vec_push_Checked(frame->result, &dropped, sizeof(dropped), _Alignof(txn_memtable_table_DroppedPage_t));
	}
	frame->result_live = false;
	frame->ok = true;
	txn_memtable_table_dropped_pages_pad_0(frame);
}

/*
 * txn_memtable_table_dropped_pages seals and drops from an open range's list
 * each page of the entries looked at but its newest, the one it appends to,
 * whose count of live records is zero, storing no page over its entry, and
 * returns each dropped page's name with its count of lists, for the caller
 * to lower once the lock is released.
 */
static bool
txn_memtable_table_dropped_pages(TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold, const uint64 *entries,
								 size_t entries_len, StdVec *result, HostError *error)
{
	txn_memtable_table_dropped_pages_frame_0_t frame = {.open = open, .hold = hold, .entries = entries,
	.entries_len = entries_len, .result = result, .error = error};

	std_landing(&frame, txn_memtable_table_dropped_pages_body_0, txn_memtable_table_dropped_pages_pad_0);
	return frame.ok;
}

/* txn_memtable_table_Compactable_drop drops a compactable page's entry, which owns nothing. */
static void
txn_memtable_table_Compactable_drop(void *self)
{
	(void) self;
}

/*
 * txn_memtable_table_compactable returns the pages of the entries looked at
 * in an open range worth compacting: each page the range names alone, but
 * its first, its newest, and the dropped, whose dead bytes pass half the
 * bytes it holds, with its copy's size, the words it handed out and the
 * bytes not dead.
 */
static bool
txn_memtable_table_compactable(const TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold, uint64 first,
							   const uint64 *entries, size_t entries_len, StdVec *result, HostError *error)
{
	HostDeltaListListView view = host_delta_list_Guard_list(open->open.guard, open->open.closes->shape,
															TXN_MEMTABLE_INDEX_PAGES);

	for (size_t k = 0; k < entries_len; k++)
	{
		TxnMemtableTableCompactable found = {.entry = entries[k]};
		HostDeltaPageRef page;
		HostDeltaWord dead;
		HostDeltaWord lists;
		HostDeltaWord count;
		uint64		filled;

		if (found.entry >= std_saturating_sub_u64(host_delta_list_ListView_len(&view), 1))
			continue;
		if (!host_delta_list_ListView_get(&view, hold, found.entry, &found.name, error))
			return false;
		if (found.entry == first || found.name == TXN_MEMTABLE_PAGES_DROPPED)
			continue;
		if (!host_delta_Hold_page(hold, host_delta_page_Page_named(found.name), &page, error) ||
			!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_DEAD, &dead, error) ||
			!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_LISTS, &lists, error))
			return false;
		filled = host_delta_PageRef_filled(&page);
		if (host_delta_Word_load(&lists) != 1 ||
			STD_MUL(u64, host_delta_Word_load(&dead), 2, 1156, 45) <= filled)
			continue;
		if (!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_COUNT, &count, error))
			return false;
		found.size.bytes = (uint32) std_saturating_sub_u64(filled, host_delta_Word_load(&dead));
		found.size.words = (uint32) host_delta_Word_load(&count);
		if (!std_vec_try_reserve(result, 1, sizeof(found), _Alignof(TxnMemtableTableCompactable)))
		{
			*error = host_Error_out_of_memory();
			return false;
		}
		std_vec_push_Checked(result, &found, sizeof(found), _Alignof(TxnMemtableTableCompactable));
	}
	return true;
}

/* txn_memtable_table_compacted_frame_0_t is compacted's state across its pad: the sizes, the copies, the range's guard, the heads, and the old pages, while live. */
typedef struct txn_memtable_table_compacted_frame_0_t
{
	HostIoIo   *io;
	const HostDeltaHold *hold;
	uint64		range;
	StdVec	   *found;
	bool	   *result;
	HostError  *error;
	bool		ok;
	StdVec		sizes;
	bool		sizes_live;
	StdVec		copies;
	size_t		copies_at;
	bool		copies_live;
	HostSharedGuard guard;
	bool		guard_live;

	/*
	 * record is the range, whose first page's name a copy of that page takes
	 * over (bugs row 143).
	 */
	HostSharedRecord record;
	StdVec		heads;
	bool		heads_live;
	StdVec		old;
	bool		old_live;
} txn_memtable_table_compacted_frame_0_t;

/* txn_memtable_table_compacted_pad_0 is compacted's pad: the heads, the guard, the old pages, the copies left, the sizes, and the found pages, while live. */
static void
txn_memtable_table_compacted_pad_0(void *frame_)
{
	txn_memtable_table_compacted_frame_0_t *frame = frame_;

	if (frame->heads_live)
	{
		frame->heads_live = false;
		std_vec_free_Checked(&frame->heads, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	if (frame->old_live)
	{
		frame->old_live = false;
		std_vec_free_Checked(&frame->old, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->copies_live)
	{
		frame->copies_live = false;
		for (size_t k = frame->copies_at; k < frame->copies.len; k++)
			host_delta_Owned_drop(&STD_VEC_AT(&frame->copies, HostDeltaOwned, k));
		std_vec_free_Checked(&frame->copies, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	}
	if (frame->sizes_live)
	{
		frame->sizes_live = false;
		std_vec_free_Checked(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	}
	std_vec_free_Checked(frame->found, sizeof(TxnMemtableTableCompactable), _Alignof(TxnMemtableTableCompactable));
}

/*
 * txn_memtable_table_compacted_copies allocates the found pages' copies,
 * refusing at the delta's budget all but the first page's copy, which it
 * takes past the budget, answering false on an error and setting some when
 * it has a copy.
 */
static bool
txn_memtable_table_compacted_copies(txn_memtable_table_compacted_frame_0_t *frame, bool *some)
{
	bool		allocated;
	HostDeltaFull full;
	HostDeltaOwned copy;
	const TxnMemtableTableCompactable *first;

	*some = false;
	std_vec_new(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	frame->sizes_live = true;
	if (!std_vec_try_reserve(&frame->sizes, frame->found->len, sizeof(HostDeltaPageSize),
							 _Alignof(HostDeltaPageSize)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	for (size_t k = 0; k < frame->found->len; k++)
		std_vec_push_Checked(&frame->sizes, &STD_VEC_AT(frame->found, TxnMemtableTableCompactable, k).size,
							 sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	if (!host_delta_alloc(frame->io, frame->sizes.ptr, frame->sizes.len, &allocated, &frame->copies, &full,
						  frame->error))
		return false;
	if (allocated)
	{
		frame->copies_live = true;
		*some = true;
		return true;
	}
	std_index_check(0, frame->found->len, RUST_FILE, 1178, 52);
	first = &STD_VEC_AT(frame->found, TxnMemtableTableCompactable, 0);
	std_vec_with_capacity_Checked(&frame->copies, 1, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	frame->copies_live = true;
	if (!host_delta_alloc_replacing(frame->io, first->size, host_delta_page_Page_named(first->name), &allocated,
									&copy, &full, frame->error))
		return false;
	if (!allocated)
		return true;
	frame->found->len = 1;
	std_vec_push_Checked(&frame->copies, &copy, sizeof(copy), _Alignof(HostDeltaOwned));
	*some = true;
	return true;
}

/* txn_memtable_table_compacted_page copies one found page under the range's lock, linking the copy over its entry, answering false on an error. */
static bool
txn_memtable_table_compacted_page(txn_memtable_table_compacted_frame_0_t *frame, TxnMemtableIndexKeyedOpen *open,
								  const TxnMemtableTableCompactable *found, HostDeltaOwned *copy, bool *linked)
{
	HostDeltaListListView view = host_delta_list_Guard_list(open->open.guard, open->open.closes->shape,
															TXN_MEMTABLE_INDEX_PAGES);
	HostDeltaListListMut list;
	uint64		name;
	HostDeltaPageRef page;
	HostDeltaWord count;
	HostDeltaWord live;
	HostDeltaPagePage set;

	*linked = false;
	if (!host_delta_list_ListView_get(&view, frame->hold, found->entry, &name, frame->error))
		return false;
	if (name != found->name)
		return true;
	if (!host_delta_Hold_page(frame->hold, host_delta_page_Page_named(found->name), &page, frame->error) ||
		!txn_memtable_index_moved(open, frame->hold, found->entry, copy, &frame->heads, frame->error))
		return false;
	frame->heads_live = true;
	if (!txn_memtable_version_carried(open, frame->hold, frame->heads.ptr, frame->heads.len, found->entry, copy,
									  frame->error))
		return false;
	frame->heads_live = false;
	std_vec_free_Checked(&frame->heads, sizeof(uint64), _Alignof(uint64));
	if (!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_COUNT, &count, frame->error) ||
		!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_LIVE, &live, frame->error) ||
		!host_delta_Owned_store(copy, TXN_MEMTABLE_PAGES_COUNT, host_delta_Word_load(&count), frame->error) ||
		!host_delta_Owned_store(copy, TXN_MEMTABLE_PAGES_LIVE, host_delta_Word_load(&live), frame->error) ||
		!host_delta_Owned_store(copy, TXN_MEMTABLE_PAGES_LISTS, 1, frame->error))
		return false;
	list = host_delta_list_GuardExclusive_list_mut(open->open.guard, open->open.closes->shape, open->open.closes->lock,
												   TXN_MEMTABLE_INDEX_PAGES);
	*linked = true;
	if (!host_delta_list_ListMut_set_page(&list, frame->hold, found->entry, copy, &set, frame->error))
		return false;
	{
		HostSharedOp moved = {.tag = HOST_SHARED_OP_EXCHANGE,
		.u.exchange = {found->name, host_delta_page_Page_value(set)}};

		(void) host_shared_Record_atomic(frame->record, TXN_MEMTABLE_RANGE_RANGE_START, moved);
	}
	std_vec_push_Checked(&frame->old, &found->name, sizeof(uint64), _Alignof(uint64));
	return true;
}

/* txn_memtable_table_compacted_body_0 is compacted's body. */
static void
txn_memtable_table_compacted_body_0(void *frame_)
{
	txn_memtable_table_compacted_frame_0_t *frame = frame_;
	bool		some;
	HostSharedRecord record;
	TxnMemtableIndexKeyedOpen open;
	TxnMemtableTableRetiring names;
	StdIterator pages;

	*frame->result = false;
	if (!txn_memtable_table_compacted_copies(frame, &some))
	{
		txn_memtable_table_compacted_pad_0(frame);
		return;
	}
	if (!some)
	{
		frame->ok = true;
		txn_memtable_table_compacted_pad_0(frame);
		return;
	}
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-memtable-compacting", frame->error) ||
		!host_delta_Hold_record(frame->hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, frame->range, &record,
								frame->error) ||
		!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_RANGE_LOCK,
								 &frame->guard, frame->error))
	{
		txn_memtable_table_compacted_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	frame->record = record;
	if (!txn_memtable_index_keyed_open(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &frame->guard, &open))
	{
		frame->ok = true;
		txn_memtable_table_compacted_pad_0(frame);
		return;
	}
	std_vec_new(&frame->old, sizeof(uint64), _Alignof(uint64));
	frame->old_live = true;
	if (!std_vec_try_reserve(&frame->old, frame->found->len, sizeof(uint64), _Alignof(uint64)))
	{
		*frame->error = host_Error_out_of_memory();
		txn_memtable_table_compacted_pad_0(frame);
		return;
	}
	for (size_t k = 0, k_len = std_min_usize(frame->found->len, frame->copies.len); k < k_len; k++)
	{
		bool		linked;
		HostDeltaOwned *copy = &STD_VEC_AT(&frame->copies, HostDeltaOwned, k);

		frame->copies_at = std_wrapping_add_usize(k, 1);
		if (!txn_memtable_table_compacted_page(frame, &open, &STD_VEC_AT(frame->found, TxnMemtableTableCompactable, k),
											   copy, &linked))
		{
			if (!linked)
				host_delta_Owned_drop(copy);
			txn_memtable_table_compacted_pad_0(frame);
			return;
		}
		if (!linked)
			host_delta_Owned_drop(copy);
	}
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	*frame->result = frame->old.len > 0;
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-memtable-moved", frame->error))
	{
		txn_memtable_table_compacted_pad_0(frame);
		return;
	}
	names = (TxnMemtableTableRetiring)
	{
		frame->old.ptr, frame->old.len, 0
	};
	pages = (StdIterator)
	{
		&TXN_MEMTABLE_TABLE_RETIRING_ITERATOR_OPS, &names
	};
	frame->ok = host_delta_retire(frame->io, NULL, 0, pages, frame->error);
	txn_memtable_table_compacted_pad_0(frame);
}

/*
 * txn_memtable_table_compacted compacts the pages the remover found worth
 * it, taking them, after its lock's release: it allocates their copies, a
 * refusal of the delta's budget taking the first page's copy past it while
 * no other replacement is and otherwise leaving the pages as they are, and
 * under the range's exclusive lock again copies each page whose list entry
 * still names it, its live records only, and links the copy over its index;
 * then it retires the old pages. A closed range, or an entry changed
 * meanwhile, gives its copy back. The pause doltgres-memtable-compacting
 * comes between the allocation and the lock, and doltgres-memtable-moved
 * between the lock's release and the old pages' retirement (bugs row 143).
 * It answers whether it replaced a page.
 */
static bool
txn_memtable_table_compacted(HostIoIo *io, const HostDeltaHold *hold, uint64 range, StdVec *found, bool *result,
							 HostError *error)
{
	txn_memtable_table_compacted_frame_0_t frame = {.io = io, .hold = hold, .range = range, .found = found,
	.result = result, .error = error};

	std_landing(&frame, txn_memtable_table_compacted_body_0, txn_memtable_table_compacted_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_table_moved finds a page a packing moves in its range's list
 * under the range's shared lock, and moves it as a compaction copies a page,
 * its first, newest, or only page included, at its own size, so its copy
 * takes the room the packing ordered for it (bugs row 143). A closed range, a
 * page no longer in its list, and one more than one list names stay; it
 * answers whether it moved the page.
 */
static bool
txn_memtable_table_moved(HostIoIo *io, const HostDeltaHold *hold, const HostDeltaDirectoryPacked *packed,
						 bool *result, HostError *error)
{
	HostSharedRecord record;
	HostSharedGuard guard;
	TxnMemtableIndexKeyedOpen open;
	TxnMemtableTableCompactable found = {.name = packed->name, .size = packed->size};
	bool		listed = false;
	bool		ok = true;
	StdVec		moving;

	*result = false;
	if (!host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, packed->location, &record, error) ||
		!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
										error))
		return false;
	if (txn_memtable_index_keyed_open(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &guard, &open))
	{
		HostDeltaListListView view = host_delta_list_Guard_list(open.open.guard, open.open.closes->shape,
																TXN_MEMTABLE_INDEX_PAGES);

		for (uint64 entry = 0, len = host_delta_list_ListView_len(&view); ok && !listed && entry < len; entry++)
		{
			uint64		name;

			ok = host_delta_list_ListView_get(&view, hold, entry, &name, error);
			listed = ok && name == packed->name;
			found.entry = entry;
		}
		if (listed)
		{
			HostDeltaPageRef page;
			HostDeltaWord lists;

			ok = host_delta_Hold_page(hold, host_delta_page_Page_named(packed->name), &page, error) &&
				host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_LISTS, &lists, error);
			listed = ok && host_delta_Word_load(&lists) == 1;
		}
	}
	host_shared_Guard_drop(&guard);
	if (!ok || !listed)
		return ok;
	std_vec_with_capacity_Checked(&moving, 1, sizeof(found), _Alignof(TxnMemtableTableCompactable));
	std_vec_push_Checked(&moving, &found, sizeof(found), _Alignof(TxnMemtableTableCompactable));
	return txn_memtable_table_compacted(io, hold, packed->location, &moving, result, error);
}

/*
 * txn_memtable_table_packed packs the delta's blocks for a writer the budget
 * refused with nothing left to compact or spill (bugs row 143): it moves each
 * live page out of the blocks the packing drains by its owner's move, under
 * its owner's lock, a range's page as a compaction copies it, and answers
 * whether it moved one, so the writer asks again once the moved pages free.
 * Its hold begins before it reads the blocks, so every page it lists, and
 * its owner, stay reachable while it moves them.
 */
bool
txn_memtable_table_packed(HostIoIo *io, bool *result, HostError *error)
{
	uint64		shapes[] = {TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape.id, TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape.id};
	StdVec		packed;
	HostDeltaHold hold;
	bool		packed_ok;
	bool		ok;

	*result = false;
	if (!host_delta_hold(io, &hold, error))
		return false;
	packed_ok = host_delta_packing(shapes, lengthof(shapes), &packed, error);
	ok = packed_ok;
	for (size_t k = 0; ok && k < packed.len; k++)
	{
		const HostDeltaDirectoryPacked *page = &STD_VEC_AT(&packed, HostDeltaDirectoryPacked, k);
		bool		moved = false;

		if (page->shape == TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape.id)
			ok = txn_memtable_table_moved(io, &hold, page, &moved, error);
		else if (page->shape == TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape.id)
			ok = txn_memtable_tuples_relocated(io, page->location, &moved, error);
		*result = *result || moved;
	}
	host_delta_Hold_drop(&hold);
	if (packed_ok)
		std_vec_free_Checked(&packed, sizeof(HostDeltaDirectoryPacked), _Alignof(HostDeltaDirectoryPacked));
	return ok;
}

/* txn_memtable_table_swept_memtables adds the memtables a pool's records name through a tree's word to a list. */
static bool
txn_memtable_table_swept_memtables(HostIoIo *io, const HostSharedPoolOps *pool, size_t word,
								   const TxnMemtableTableTreeOps *const *trees, size_t trees_len, StdVec *memtables,
								   HostError *error)
{
	StdVec		swept;
	bool		ok = true;

	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(io), pool, word, &swept, error))
		return false;
	for (size_t k = 0; k < swept.len && ok; k++)
	{
		bool		some;
		HostSharedHeld held;

		ok = host_shared_open(HOST_SHARED_IO_WITHIN(io), pool, STD_VEC_AT(&swept, std_TupleU64U64_t, k).f0, &some,
							  &held, error);
		if (!ok || !some)
			continue;
		for (size_t t = 0; t < trees_len; t++)
		{
			uint64		location;

			if (txn_memtable_table_memtable(trees[t], host_shared_Held_record(&held), &location))
				std_vec_push_Checked(memtables, &location, sizeof(location), _Alignof(uint64));
		}
		host_shared_Held_drop(&held);
	}
	std_vec_free_Checked(&swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	return ok;
}

/* txn_memtable_table_deferred_range_frame_0_t is deferred_range's state across its pad: the range's guard, its entries, and the pages found, while live. */
typedef struct txn_memtable_table_deferred_range_frame_0_t
{
	HostIoIo   *io;
	const HostDeltaHold *hold;
	uint64		range;
	bool	   *replaced;
	HostError  *error;
	bool		ok;
	HostSharedGuard guard;
	bool		guard_live;
	/* entries is a Vec<u64>. */
	StdVec		entries;
	bool		entries_live;
	/* found is a Vec<Compactable>. */
	StdVec		found;
	bool		found_live;
} txn_memtable_table_deferred_range_frame_0_t;

/* txn_memtable_table_deferred_range_pad_0 is deferred_range's pad: what it holds, while live. */
static void
txn_memtable_table_deferred_range_pad_0(void *frame_)
{
	txn_memtable_table_deferred_range_frame_0_t *frame = frame_;

	if (frame->found_live)
	{
		frame->found_live = false;
		std_vec_free_Checked(&frame->found, sizeof(TxnMemtableTableCompactable), _Alignof(TxnMemtableTableCompactable));
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
}

/* txn_memtable_table_deferred_range_found gathers the range's pages worth compacting, under its guard. */
static bool
txn_memtable_table_deferred_range_found(txn_memtable_table_deferred_range_frame_0_t *frame, HostSharedRecord record,
										bool *open_range)
{
	TxnMemtableIndexKeyedOpen open;
	TxnMemtableIndexNode first;
	HostDeltaListListView view;
	uint64		count;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};

	*open_range = txn_memtable_index_keyed_open(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &frame->guard, &open);
	if (!*open_range)
		return true;
	if (!txn_memtable_index_first(open.keyed, open.open.guard, &first))
	{
		*frame->error = txn_memtable_table_internal("an open range has no first node");
		return false;
	}
	view = host_delta_list_Guard_list(open.open.guard, open.open.closes->shape, TXN_MEMTABLE_INDEX_PAGES);
	count = host_delta_list_ListView_len(&view);
	exchange.u.exchange.f0 = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_REMOVED, load);
	host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_REMOVED, exchange);
	std_vec_with_capacity_Checked(&frame->entries, (size_t) count, sizeof(uint64), _Alignof(uint64));
	frame->entries_live = true;
	for (uint64 entry = 0; entry < count; entry++)
		std_vec_push_Checked(&frame->entries, &entry, sizeof(entry), _Alignof(uint64));
	std_vec_new(&frame->found, sizeof(TxnMemtableTableCompactable), _Alignof(TxnMemtableTableCompactable));
	frame->found_live = true;
	return txn_memtable_table_compactable(&open, frame->hold, txn_memtable_index_Node_page(first),
										  frame->entries.ptr, frame->entries.len, &frame->found, frame->error);
}

/* txn_memtable_table_deferred_range_body_0 is deferred_range's body. */
static void
txn_memtable_table_deferred_range_body_0(void *frame_)
{
	txn_memtable_table_deferred_range_frame_0_t *frame = frame_;
	HostSharedRecord record;
	bool		open_range;
	bool		compacted;

	if (!host_delta_Hold_record(frame->hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, frame->range, &record,
								frame->error) ||
		!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_RANGE_LOCK,
								 &frame->guard, frame->error))
		return;
	frame->guard_live = true;
	if (!txn_memtable_table_deferred_range_found(frame, record, &open_range))
	{
		txn_memtable_table_deferred_range_pad_0(frame);
		return;
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(uint64), _Alignof(uint64));
	}
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	if (!open_range || frame->found.len == 0)
	{
		frame->ok = true;
		txn_memtable_table_deferred_range_pad_0(frame);
		return;
	}
	frame->found_live = false;
	frame->ok = txn_memtable_table_compacted(frame->io, frame->hold, frame->range, &frame->found, &compacted,
											 frame->error);
	*frame->replaced = *frame->replaced || (frame->ok && compacted);
}

/* txn_memtable_table_deferred_range compacts each page worth it of one range, answering false on an error and setting replaced when it replaced one. */
static bool
txn_memtable_table_deferred_range(HostIoIo *io, const HostDeltaHold *hold, uint64 range, bool *replaced,
								  HostError *error)
{
	txn_memtable_table_deferred_range_frame_0_t frame = {.io = io, .hold = hold, .range = range,
	.replaced = replaced, .error = error};

	std_landing(&frame, txn_memtable_table_deferred_range_body_0, txn_memtable_table_deferred_range_pad_0);
	return frame.ok;
}

/* txn_memtable_table_deferred_frame_0_t is deferred's state across its pad: the hold, the memtables, a memtable's guard, and its ranges, while live. */
typedef struct txn_memtable_table_deferred_frame_0_t
{
	HostIoIo   *io;
	bool	   *result;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	/* memtables is a Vec<u64>. */
	StdVec		memtables;
	bool		memtables_live;
	HostSharedGuard guard;
	bool		guard_live;
	/* ranges is a Vec<u64>. */
	StdVec		ranges;
	bool		ranges_live;
} txn_memtable_table_deferred_frame_0_t;

/* txn_memtable_table_deferred_pad_0 is deferred's pad: what it holds, while live. */
static void
txn_memtable_table_deferred_pad_0(void *frame_)
{
	txn_memtable_table_deferred_frame_0_t *frame = frame_;

	if (frame->ranges_live)
	{
		frame->ranges_live = false;
		std_vec_free_Checked(&frame->ranges, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	if (frame->memtables_live)
	{
		frame->memtables_live = false;
		std_vec_free_Checked(&frame->memtables, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
}

/* txn_memtable_table_deferred_memtable compacts each range of one memtable, its ranges read under its shared guard. */
static bool
txn_memtable_table_deferred_memtable(txn_memtable_table_deferred_frame_0_t *frame, uint64 location)
{
	HostSharedRecord record;
	HostDeltaListListView view;

	if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, location, &record,
								frame->error) ||
		!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_MEMTABLE_LOCK,
										&frame->guard, frame->error))
		return false;
	frame->guard_live = true;
	view = host_delta_list_Guard_list(&frame->guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape,
									  TXN_MEMTABLE_RANGE_RANGES);
	std_vec_new(&frame->ranges, sizeof(uint64), _Alignof(uint64));
	frame->ranges_live = true;
	for (uint64 entry = 0, len = host_delta_list_ListView_len(&view); entry < len; entry++)
	{
		uint64		range;

		if (!host_delta_list_ListView_get(&view, &frame->hold, entry, &range, frame->error))
			return false;
		std_vec_push_Checked(&frame->ranges, &range, sizeof(range), _Alignof(uint64));
	}
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	for (size_t r = 0; r < frame->ranges.len; r++)
		if (!txn_memtable_table_deferred_range(frame->io, &frame->hold, STD_VEC_AT(&frame->ranges, uint64, r),
											   frame->result, frame->error))
			return false;
	frame->ranges_live = false;
	std_vec_free_Checked(&frame->ranges, sizeof(uint64), _Alignof(uint64));
	return true;
}

/* txn_memtable_table_deferred_body_0 is deferred's body. */
static void
txn_memtable_table_deferred_body_0(void *frame_)
{
	txn_memtable_table_deferred_frame_0_t *frame = frame_;
	static const TxnMemtableTableTreeOps *const states[] = {&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS,
	&TXN_MEMTABLE_MAP_FREE_TREE_OPS, &TXN_MEMTABLE_MAP_VALUES_TREE_OPS};
	static const TxnMemtableTableTreeOps *const branches[] = {&TXN_MEMTABLE_BRANCH_CHUNKS_TREE_OPS};

	*frame->result = false;
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	std_vec_new(&frame->memtables, sizeof(uint64), _Alignof(uint64));
	frame->memtables_live = true;
	if (!txn_memtable_table_swept_memtables(frame->io, &TXN_STATE_STATE_POOL_OPS, TXN_STATE_MEMTABLE_WORD_INDEX,
											states, lengthof(states), &frame->memtables, frame->error) ||
		!txn_memtable_table_swept_memtables(frame->io, &TXN_MEMTABLE_BRANCH_BRANCH_DELTAS_POOL_OPS,
											TXN_MEMTABLE_BRANCH_CHUNK_WORD_INDEX, branches, lengthof(branches),
											&frame->memtables, frame->error))
	{
		txn_memtable_table_deferred_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->memtables.len; k++)
		if (!txn_memtable_table_deferred_memtable(frame, STD_VEC_AT(&frame->memtables, uint64, k)))
		{
			txn_memtable_table_deferred_pad_0(frame);
			return;
		}
	frame->ok = true;
	txn_memtable_table_deferred_pad_0(frame);
}

/*
 * txn_memtable_table_deferred compacts each page worth it of every range of
 * every storage's and working state's memtables, under a hold taken before
 * it reads their owners' words, so no memtable it names is freed while it
 * reads it, and answers whether it replaced a page.
 */
static bool
txn_memtable_table_deferred(HostIoIo *io, bool *result, HostError *error)
{
	txn_memtable_table_deferred_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, txn_memtable_table_deferred_body_0, txn_memtable_table_deferred_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_table_waited waits at the delta's budget for a writer the
 * budget refused, after giving pages back: it runs the compactions every
 * tree's ranges deferred until half their keys' worth of removals, and then,
 * unless the writer is a spill's own allocation, spills what spill's
 * relieved spills, so a writer whose own or replayed transaction holds the
 * delta's bytes frees them rather than waiting on what only its commit, or
 * in recovery nothing, could free. Either one that gave pages back ends the
 * wait, so the writer asks again, and the writer waits only when neither did.
 */
bool
txn_memtable_table_waited(HostIoIo *io, HostDeltaFull full, bool spilling, HostError *error)
{
	bool		replaced;
	bool		relieved = false;

	if (!txn_memtable_table_deferred(io, &replaced, error))
		return false;
	if (replaced)
		return true;
	if (!spilling && !txn_spill_relieved(io, &relieved, error))
		return false;
	if (relieved)
		return true;
	if (!txn_memtable_table_packed(io, &replaced, error))
		return false;
	if (replaced)
		return true;
	return host_delta_wait(io, &full, error);
}

/*
 * txn_memtable_table_emptied closes an open memtable whose list is empty
 * when no process's pin names it, reading the pins under its exclusive lock,
 * and clears its owner's word of it, as of any closed memtable; it answers
 * whether the memtable is closed.
 */
static bool
txn_memtable_table_emptied(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
						   HostSharedRecord owner, uint64 location, bool *result, HostError *error)
{
	HostSharedRecord memtable;
	HostSharedGuard guard;
	TxnMemtableIndexOpen open;
	bool		closed = true;

	if (!host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, location, &memtable, error) ||
		!host_shared_Record_lock(memtable, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_MEMTABLE_LOCK, &guard,
								 error))
		return false;
	if (txn_memtable_index_open(&TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS, &guard, &open))
	{
		HostDeltaListListView view = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape,
																TXN_MEMTABLE_RANGE_RANGES);
		bool		pinned = false;

		closed = false;
		if (host_delta_list_ListView_is_empty(&view))
		{
			if (!txn_memtable_pins_pinned(tree->pin, location, &pinned, error))
			{
				host_shared_Guard_drop(&guard);
				return false;
			}
			if (!pinned)
			{
				txn_memtable_index_Open_close(open);
				closed = true;
			}
		}
	}
	host_shared_Guard_drop(&guard);
	*result = closed;
	return !closed || txn_memtable_table_cleared(tree, io, hold, owner, location, error);
}

/* txn_memtable_table_Version_drop is Version's drop glue for a list of them. */
static void
txn_memtable_table_Version_drop(void *self)
{
	txn_memtable_version_Version_drop(self);
}

/* txn_memtable_table_chained_reading_t is chain's reading's captures: the key and the versions read. */
typedef struct txn_memtable_table_chained_reading_t
{
	const uint8 *key;
	size_t		key_len;
	StdVec	   *versions;
} txn_memtable_table_chained_reading_t;

/* txn_memtable_table_chain_of collects a node's chain of versions, newest first, onto a list. */
static bool
txn_memtable_table_chain_of(const HostSharedGuard *guard, const HostDeltaHold *hold, TxnMemtableIndexNode node,
							StdVec *versions, HostError *error)
{
	TxnMemtableVersionChain chain;

	if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &chain, error))
		return false;
	for (;;)
	{
		bool		some;
		TxnMemtableVersionVersion version;

		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, error))
			return false;
		if (!some)
			return true;
		std_vec_push_Checked(versions, &version, sizeof(version), _Alignof(TxnMemtableVersionVersion));
	}
}

/* txn_memtable_table_chain_reading reads a key's chain under its range's shared lock. */
static bool
txn_memtable_table_chain_reading(void *captures, const HostDeltaHold *hold, TxnMemtableTableAt at,
								 const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_table_chained_reading_t *reading = captures;
	bool		some;
	TxnMemtableIndexNode node;

	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, reading->key, reading->key_len,
								 &some, &node, error))
		return false;
	return !some || txn_memtable_table_chain_of(guard, hold, node, reading->versions, error);
}

/*
 * txn_memtable_table_chain lends a key's versions in one of a tree's
 * memtables alone, newest first, for a caller's hold, read as versions reads
 * them.
 */
bool
txn_memtable_table_chain(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
						 HostSharedRecord owner, const uint8 *key, size_t key_len, StdVec *result, HostError *error)
{
	txn_memtable_table_chained_reading_t reading = {key, key_len, result};
	bool		some;

	std_vec_new(result, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	if (txn_memtable_table_lent(tree, io, hold, owner, key, key_len, txn_memtable_table_chain_reading, &reading,
								&some, error))
		return true;
	std_vec_clear(result, sizeof(TxnMemtableVersionVersion), txn_memtable_table_Version_drop);
	std_vec_free_Checked(result, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	return false;
}

/*
 * txn_memtable_table_versions_read_t is versions' read's captures: its
 * arguments, the versions it lends, and the key's spilled versions, while
 * live.
 */
typedef struct txn_memtable_table_versions_read_t
{
	HostIoIo   *io;
	const HostDeltaHold *hold;
	HostSharedRecord state;
	const uint8 *key;
	size_t		key_len;
	StdVec	   *result;
	StdVec		spilled;
	bool		spilled_live;
} txn_memtable_table_versions_read_t;

/* txn_memtable_table_versions_dropped drops the versions a read of versions lent. */
static void
txn_memtable_table_versions_dropped(void *captures)
{
	txn_memtable_table_versions_read_t *c = captures;

	if (c->spilled_live)
		txn_memtable_version_versions_drop(&c->spilled, 0);
	c->spilled_live = false;
	std_vec_clear(c->result, sizeof(TxnMemtableVersionVersion), txn_memtable_table_Version_drop);
	std_vec_free_Checked(c->result, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
}

/* txn_memtable_table_versions_read is versions' read: the key's chain in the memtable, then its versions in the spills. */
static bool
txn_memtable_table_versions_read(void *captures, uint64 *listed, HostError *error)
{
	txn_memtable_table_versions_read_t *c = captures;

	if (!txn_memtable_table_chain(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, c->io, c->hold, c->state, c->key, c->key_len,
								  c->result, error))
		return false;
	if (!txn_spill_versions(&TXN_SPILL_STATE_HOME_OPS, c->io, c->hold, c->state, TXN_RUNS_KIND_SPILLED_ROWS, c->key,
							c->key_len, listed, &c->spilled, error))
	{
		txn_memtable_table_versions_dropped(c);
		return false;
	}
	c->spilled_live = true;
	return true;
}

/*
 * txn_memtable_table_versions lends a key's versions in a storage's rows,
 * newest first, the memtable's with its spills' placed among them in commit
 * order (bugs row 397), for a caller's hold: read under the range's shared
 * lock through the hand-off and answered with the lock released, so a reader
 * can ask Postgres each writer's outcome. It answers none when the delta
 * holds no version of the key.
 */
bool
txn_memtable_table_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, const uint8 *key,
							size_t key_len, StdVec *result, HostError *error)
{
	txn_memtable_table_versions_read_t captures = {io, hold, state, key, key_len, result};

	if (!txn_spill_taken(&TXN_SPILL_STATE_HOME_OPS, io, state, txn_memtable_table_versions_read,
						 txn_memtable_table_versions_dropped, &captures, error))
		return false;
	if (!txn_spill_ordered(io, result, &captures.spilled, error))
	{
		txn_memtable_table_versions_dropped(&captures);
		return false;
	}
	std_vec_free_Checked(&captures.spilled, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	return true;
}

/* txn_memtable_table_bytes_drop drops a Vec<u8> in a list of them. */
static void
txn_memtable_table_bytes_drop(void *self)
{
	std_vec_free_Checked((StdVec *) self, 1, 1);
}

/* TxnMemtableTableCopiedKey is one key of a range's copy for a walk: its key, its records newest first, and its handles with their writers' top-level xids. */
typedef struct TxnMemtableTableCopiedKey
{
	StdVec		key;
	StdVec		records;
	StdVec		handles;
} TxnMemtableTableCopiedKey;

/* txn_memtable_table_CopiedKey_drop drops one copied key. */
static void
txn_memtable_table_CopiedKey_drop(void *self_)
{
	TxnMemtableTableCopiedKey *self = self_;

	std_vec_free_Checked(&self->key, 1, 1);
	std_vec_clear(&self->records, sizeof(StdVec), txn_memtable_table_bytes_drop);
	std_vec_free_Checked(&self->records, sizeof(StdVec), _Alignof(StdVec));
	std_vec_free_Checked(&self->handles, sizeof(txn_TupleHandleU64Bool_t), _Alignof(txn_TupleHandleU64Bool_t));
}

/* txn_memtable_table_copied_reading_t is copied's reading's captures: the walk's place, whether it hands out killed versions, and the copy. */
typedef struct txn_memtable_table_copied_reading_t
{
	const StdVec *place;
	bool		killed;
	StdVec		copied;
} txn_memtable_table_copied_reading_t;

/* txn_memtable_table_copied_key copies one node's key, records, and handles onto the copy when it has a version. */
static bool
txn_memtable_table_copied_key(txn_memtable_table_copied_reading_t *reading, const HostSharedGuard *guard,
							  const HostDeltaHold *hold, TxnMemtableIndexNode node, StdBytes key, HostError *error)
{
	TxnMemtableTableCopiedKey copy;
	StdVec		chain;
	bool		ok;

	std_vec_new(&chain, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	ok = txn_memtable_table_chain_of(guard, hold, node, &chain, error);
	if (ok && chain.len > 0)
	{
		std_slice_to_vec_Checked(&copy.key, key.ptr, key.len, 1, 1);
		std_vec_new(&copy.records, sizeof(StdVec), _Alignof(StdVec));
		std_vec_new(&copy.handles, sizeof(txn_TupleHandleU64Bool_t), _Alignof(txn_TupleHandleU64Bool_t));
		for (size_t k = 0; k < chain.len; k++)
		{
			const TxnMemtableVersionVersion *version = &STD_VEC_AT(&chain, TxnMemtableVersionVersion, k);
			TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
			TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);
			txn_TupleHandleU64Bool_t handle = {version->handle, writer.has_top ? writer.top : writer.xid,
			txn_memtable_version_Version_killed(version)};

			std_vec_push_Checked(&copy.handles, &handle, sizeof(handle), _Alignof(txn_TupleHandleU64Bool_t));
			if (reading->killed || !txn_memtable_version_Version_killed(version))
			{
				StdVec		stored;

				txn_memtable_version_Version_stored(version, &stored);
				std_vec_push_Checked(&copy.records, &stored, sizeof(stored), _Alignof(StdVec));
			}
		}
		std_vec_push_Checked(&reading->copied, &copy, sizeof(copy), _Alignof(TxnMemtableTableCopiedKey));
	}
	std_vec_clear(&chain, sizeof(TxnMemtableVersionVersion), txn_memtable_table_Version_drop);
	std_vec_free_Checked(&chain, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	return ok;
}

/*
 * txn_memtable_table_copied_reading copies one range's keys from the walk's
 * place up to the key the step stops at under the range's shared lock.
 */
static bool
txn_memtable_table_copied_reading(void *captures, const HostDeltaHold *hold, const HostSharedGuard *guard,
								  bool has_stop, const uint8 *stop, size_t stop_len, HostError *error)
{
	txn_memtable_table_copied_reading_t *reading = captures;
	bool		some;
	TxnMemtableIndexNode node;

	if (!txn_memtable_index_seek(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, reading->place->ptr,
								 reading->place->len, &some, &node, error))
		return false;
	while (some)
	{
		HostDeltaLent key;
		StdBytes	bytes;
		bool		ok;

		if (!txn_memtable_index_key(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &key, error))
			return false;
		bytes = host_delta_Lent_Deref_deref(&key);
		if (has_stop && std_bytes_Ord_cmp(bytes.ptr, bytes.len, stop, stop_len) != STD_ORDERING_LESS)
		{
			host_delta_Lent_drop(&key);
			break;
		}
		ok = txn_memtable_table_copied_key(reading, guard, hold, node, bytes, error);
		host_delta_Lent_drop(&key);
		if (!ok || !txn_memtable_index_next(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &some, &node,
											error))
			return false;
	}
	return true;
}

/* txn_memtable_table_copied_frame_0_t is copied's state across its pad: the walk's place, the hold, the copy, and the step's stop, while live. */
typedef struct txn_memtable_table_copied_frame_0_t
{
	const TxnMemtableTableTreeOps *tree;
	HostIoIo   *io;
	HostSharedRecord owner;
	bool		killed;
	bool		(*each) (void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *records,
						 size_t records_len, const txn_TupleHandleU64Bool_t *handles, size_t handles_len,
						 HostError *error);
	void	   *captures;
	HostError  *error;
	bool		ok;
	StdVec		place;
	bool		place_live;
	HostDeltaHold hold;
	bool		hold_live;
	txn_memtable_table_copied_reading_t reading;
	bool		copied_live;
	StdVec		stop;
	bool		stop_live;
} txn_memtable_table_copied_frame_0_t;

/* txn_memtable_table_copied_pad_0 is copied's pad: the stop, the copy, the hold, and the place, while live. */
static void
txn_memtable_table_copied_pad_0(void *frame_)
{
	txn_memtable_table_copied_frame_0_t *frame = frame_;

	if (frame->stop_live)
	{
		frame->stop_live = false;
		std_vec_free_Checked(&frame->stop, 1, 1);
	}
	if (frame->copied_live)
	{
		frame->copied_live = false;
		std_vec_clear(&frame->reading.copied, sizeof(TxnMemtableTableCopiedKey), txn_memtable_table_CopiedKey_drop);
		std_vec_free_Checked(&frame->reading.copied, sizeof(TxnMemtableTableCopiedKey),
							 _Alignof(TxnMemtableTableCopiedKey));
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	if (frame->place_live)
	{
		frame->place_live = false;
		std_vec_free_Checked(&frame->place, 1, 1);
	}
}

/* txn_memtable_table_copied_body_0 is copied's body. */
static void
txn_memtable_table_copied_body_0(void *frame_)
{
	txn_memtable_table_copied_frame_0_t *frame = frame_;

	std_vec_new(&frame->place, 1, 1);
	frame->place_live = true;
	frame->reading.place = &frame->place;
	frame->reading.killed = frame->killed;
	for (;;)
	{
		bool		some;
		bool		has_stop;

		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		{
			txn_memtable_table_copied_pad_0(frame);
			return;
		}
		frame->hold_live = true;
		std_vec_new(&frame->reading.copied, sizeof(TxnMemtableTableCopiedKey), _Alignof(TxnMemtableTableCopiedKey));
		frame->copied_live = true;
		if (!txn_memtable_table_stepped(frame->tree, frame->io, &frame->hold, frame->owner, true, frame->place.ptr,
										frame->place.len, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD,
										txn_memtable_table_copied_reading, &frame->reading, &some, &has_stop,
										&frame->stop, frame->error))
		{
			txn_memtable_table_copied_pad_0(frame);
			return;
		}
		frame->stop_live = some && has_stop;
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
		if (!some)
			break;
		for (size_t k = 0; k < frame->reading.copied.len; k++)
		{
			const TxnMemtableTableCopiedKey *copy = &STD_VEC_AT(&frame->reading.copied, TxnMemtableTableCopiedKey,
																k);

			if (!frame->each(frame->captures, frame->io, copy->key.ptr, copy->key.len, copy->records.ptr,
							 copy->records.len, copy->handles.ptr, copy->handles.len, frame->error))
			{
				txn_memtable_table_copied_pad_0(frame);
				return;
			}
		}
		frame->copied_live = false;
		std_vec_clear(&frame->reading.copied, sizeof(TxnMemtableTableCopiedKey), txn_memtable_table_CopiedKey_drop);
		std_vec_free_Checked(&frame->reading.copied, sizeof(TxnMemtableTableCopiedKey),
							 _Alignof(TxnMemtableTableCopiedKey));
		if (!has_stop)
			break;
		std_vec_free_Checked(&frame->place, 1, 1);
		frame->place = frame->stop;
		frame->stop_live = false;
	}
	frame->ok = true;
	txn_memtable_table_copied_pad_0(frame);
}

/*
 * txn_memtable_table_copied hands out every key of a tree's memtable that
 * has a version, in byte order, with its versions' records newest first, as
 * a checkpoint's runs write them, a speculative insert its writer killed
 * among them only when killed says so, and every version's handle, its
 * writer's top-level xid, and whether it is killed, newest first, the killed
 * included. Each step copies one range's keys from the walk's place up to
 * the key the step stops at, the next entry's first key, under the range's
 * shared lock, and hands them out with the lock released, so no lock is held
 * across the caller's writes; it goes on from that key. A range that splits or is emptied and
 * retired between the steps leaves no key skipped or handed out twice.
 * Copying allocates, since a range's records must outlive its lock.
 */
bool
txn_memtable_table_copied(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner, bool killed,
						  bool (*each) (void *captures, HostIoIo *io, const uint8 *key, size_t key_len,
										const StdVec *records, size_t records_len,
										const txn_TupleHandleU64Bool_t *handles, size_t handles_len, HostError *error),
						  void *captures, HostError *error)
{
	txn_memtable_table_copied_frame_0_t frame = {.tree = tree, .io = io, .owner = owner, .killed = killed,
	.each = each, .captures = captures, .error = error};

	std_landing(&frame, txn_memtable_table_copied_body_0, txn_memtable_table_copied_pad_0);
	return frame.ok;
}

/* txn_memtable_table_walk_t is walk's each's captures: the caller's each and its captures. */
typedef struct txn_memtable_table_walk_t
{
	bool		(*each) (void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
						 size_t values_len, HostError *error);
	void	   *captures;
} txn_memtable_table_walk_t;

/* txn_memtable_table_walk_each hands a key with records to the caller's each, passing over one with none. */
static bool
txn_memtable_table_walk_each(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *records,
							 size_t records_len, const txn_TupleHandleU64Bool_t *handles, size_t handles_len,
							 HostError *error)
{
	txn_memtable_table_walk_t *walk = captures;

	return records_len == 0 || walk->each(walk->captures, io, key, key_len, records, records_len, error);
}

/* txn_memtable_table_walk is walked's walk, handing out a killed version among a key's records only when killed says so, taking the caller's each. */
static bool
txn_memtable_table_walk(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner, bool killed,
						bool (*each) (void *captures, HostIoIo *io, const uint8 *key, size_t key_len,
									  const StdVec *values, size_t values_len, HostError *error),
						void *each_captures, void (*each_drop) (void *captures), HostError *error)
{
	txn_memtable_table_walk_t walk = {each, each_captures};
	bool		ok = txn_memtable_table_copied(tree, io, owner, killed, txn_memtable_table_walk_each, &walk, error);

	if (each_drop != NULL)
		each_drop(each_captures);
	return ok;
}

/*
 * txn_memtable_table_walked hands out every key of a tree's memtable that
 * has a version, in byte order, with its versions' records newest first, as
 * a checkpoint's runs write them, leaving out a speculative insert its
 * writer killed, which no reader sees, as copied walks.
 */
bool
txn_memtable_table_walked(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
						  bool (*each) (void *captures, HostIoIo *io, const uint8 *key, size_t key_len,
										const StdVec *values, size_t values_len, HostError *error),
						  void *each_captures, void (*each_drop) (void *captures), HostError *error)
{
	return txn_memtable_table_walk(tree, io, owner, false, each, each_captures, each_drop, error);
}

/*
 * txn_memtable_table_walked_killed hands out every key of a tree's memtable
 * that has a version as walked does, with the versions a speculative
 * insert's writer killed among its records, for the fold's drop, which takes
 * them out.
 */
bool
txn_memtable_table_walked_killed(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
								 bool (*each) (void *captures, HostIoIo *io, const uint8 *key, size_t key_len,
											   const StdVec *values, size_t values_len, HostError *error),
								 void *each_captures, void (*each_drop) (void *captures), HostError *error)
{
	return txn_memtable_table_walk(tree, io, owner, true, each, each_captures, each_drop, error);
}

/* TxnMemtableTableChosenTag is Chosen's variants: what a step read of a memtable under its shared lock. */
typedef enum TxnMemtableTableChosenTag
{
	TXN_MEMTABLE_TABLE_CHOSEN_RANGE,
	TXN_MEMTABLE_TABLE_CHOSEN_CLOSED,
	TXN_MEMTABLE_TABLE_CHOSEN_EMPTY,
	TXN_MEMTABLE_TABLE_CHOSEN_NONE,
} TxnMemtableTableChosenTag;

/* txn_memtable_table_first_key_copy copies a range's first key onto a new vector. */
static bool
txn_memtable_table_first_key_copy(const HostDeltaHold *hold, uint64 range, StdVec *result, HostError *error)
{
	HostDeltaLent first;
	StdBytes	bytes;

	if (!txn_memtable_table_first_key(hold, range, &first, error))
		return false;
	bytes = host_delta_Lent_Deref_deref(&first);
	std_slice_to_vec_Checked(result, bytes.ptr, bytes.len, 1, 1);
	host_delta_Lent_drop(&first);
	return true;
}

/*
 * txn_memtable_table_chosen chooses, under a memtable's shared lock, the
 * range a step's place is in and the key it stops at, answering false on an
 * error.
 */
static bool
txn_memtable_table_chosen(HostIoIo *io, const HostDeltaHold *hold, uint64 location, bool has_place,
						  const uint8 *place, size_t place_len, TxnMemtableTableDirection way,
						  TxnMemtableTableChosenTag *tag, TxnMemtableTableAt *at, bool *has_stop, StdVec *stop,
						  HostError *error)
{
	HostSharedRecord memtable;
	HostSharedGuard guard;
	HostDeltaListListView view;
	bool		ok = true;

	if (!host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, location, &memtable, error) ||
		!host_shared_Record_lock_shared(memtable, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_MEMTABLE_LOCK,
										&guard, error))
		return false;
	view = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape, TXN_MEMTABLE_RANGE_RANGES);
	*has_stop = false;
	if (txn_memtable_index_closed(&TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS, &guard))
		*tag = TXN_MEMTABLE_TABLE_CHOSEN_CLOSED;
	else if (host_delta_list_ListView_is_empty(&view))
		*tag = TXN_MEMTABLE_TABLE_CHOSEN_EMPTY;
	else
	{
		uint64		len = host_delta_list_ListView_len(&view);
		uint64		entry = 0;
		std_TupleU64U64_t found;

		*tag = TXN_MEMTABLE_TABLE_CHOSEN_RANGE;
		if (way == TXN_MEMTABLE_TABLE_DIRECTION_FORWARD)
		{
			ok = txn_memtable_table_search(&view, hold, has_place ? place : NULL, has_place ? place_len : 0, &found,
										   error);
			entry = found.f0;
		}
		else if (!has_place)
			entry = STD_SUB(u64, len, 1, 1500, 56);
		else
		{
			StdOrdering order;

			ok = txn_memtable_table_search(&view, hold, place, place_len, &found, error) &&
				txn_memtable_table_first_key_cmp(hold, found.f1, place, place_len, &order, error);
			if (ok && order == STD_ORDERING_LESS)
				entry = found.f0;
			else if (ok && found.f0 == 0)
				*tag = TXN_MEMTABLE_TABLE_CHOSEN_NONE;
			else if (ok)
				entry = STD_SUB(u64, found.f0, 1, 1506, 47);
		}
		if (ok && *tag == TXN_MEMTABLE_TABLE_CHOSEN_RANGE)
		{
			at->memtable = location;
			ok = host_delta_list_ListView_get(&view, hold, entry, &at->range, error);
			if (ok && way == TXN_MEMTABLE_TABLE_DIRECTION_FORWARD &&
				STD_ADD(u64, entry, 1, 1512, 47) < len)
			{
				uint64		next;

				ok = host_delta_list_ListView_get(&view, hold, STD_ADD(u64, entry, 1, 1513, 65), &next,
												  error) &&
					txn_memtable_table_first_key_copy(hold, next, stop, error);
				*has_stop = ok;
			}
			else if (ok && way == TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD)
			{
				ok = txn_memtable_table_first_key_copy(hold, at->range, stop, error);
				*has_stop = ok;
			}
		}
	}
	host_shared_Guard_drop(&guard);
	return ok;
}

/*
 * txn_memtable_table_step finds the range a walk's step reads from a tree's
 * word under a hold, as found does: the range its place is in, or the last
 * for a backward walk with no place, and the key the step stops at, the next
 * entry's first key going forward, or the range's own first key going
 * backward. It answers nothing for a tree with no memtable, or for a
 * backward walk past the first range.
 */
static bool
txn_memtable_table_step(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
						HostSharedRecord owner, bool has_place, const uint8 *place, size_t place_len,
						TxnMemtableTableDirection way, bool *some, TxnMemtableTableAt *at, bool *has_stop,
						StdVec *stop, HostError *error)
{
	*some = false;
	*has_stop = false;
	for (;;)
	{
		uint64		location;
		TxnMemtableTableChosenTag tag;
		bool		again;

		if (!txn_memtable_table_memtable(tree, owner, &location))
		{
			if (tree->bare != NULL)
				tree->bare(owner);
			return true;
		}
		if (!txn_memtable_table_chosen(io, hold, location, has_place, place, place_len, way, &tag, at, has_stop, stop,
									   error))
			return false;
		switch (tag)
		{
			case TXN_MEMTABLE_TABLE_CHOSEN_RANGE:
				*some = true;
				return true;
			case TXN_MEMTABLE_TABLE_CHOSEN_NONE:
				return true;
			case TXN_MEMTABLE_TABLE_CHOSEN_CLOSED:
				if (!txn_memtable_table_cleared(tree, io, hold, owner, location, error))
					return false;
				break;
			case TXN_MEMTABLE_TABLE_CHOSEN_EMPTY:
				if (!txn_memtable_table_emptied(tree, io, hold, owner, location, &again, error))
					return false;
				if (!again)
					return true;
				break;
		}
	}
}

/*
 * txn_memtable_table_stepped runs a walk's step: the read of one range
 * under its shared lock, from the range the step's place is in, a closed
 * range taken out of its memtable's list and the step started over, a range
 * left holding only its first key having its memtable closed when nothing
 * else holds or pins it. reading writes its answer through its captures; it
 * answers nothing for a tree with no memtable, and the key the step stops
 * at otherwise.
 */
bool
txn_memtable_table_stepped(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
						   HostSharedRecord owner, bool has_place, const uint8 *place, size_t place_len,
						   TxnMemtableTableDirection way,
						   bool (*reading) (void *captures, const HostDeltaHold *hold, const HostSharedGuard *guard,
											bool has_stop, const uint8 *stop, size_t stop_len, HostError *error),
						   void *captures, bool *some, bool *has_stop, StdVec *stop, HostError *error)
{
	for (;;)
	{
		TxnMemtableTableAt at;
		HostSharedRecord range;
		HostSharedGuard guard;
		bool		read;
		HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

		if (!txn_memtable_table_step(tree, io, hold, owner, has_place, place, place_len, way, some, &at, has_stop,
									 stop, error))
			return false;
		if (!*some)
			return true;
		*some = false;
		if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-memtable-step", error) ||
			!host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at.range, &range, error) ||
			!host_shared_Record_lock_shared(range, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
											error))
		{
			if (*has_stop)
				std_vec_free_Checked(stop, 1, 1);
			return false;
		}
		read = !txn_memtable_index_closed(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS.closes, &guard);
		if (read && !reading(captures, hold, &guard, *has_stop, *has_stop ? stop->ptr : NULL,
							 *has_stop ? stop->len : 0, error))
		{
			host_shared_Guard_drop(&guard);
			if (*has_stop)
				std_vec_free_Checked(stop, 1, 1);
			return false;
		}
		host_shared_Guard_drop(&guard);
		if (!read)
		{
			if (*has_stop)
				std_vec_free_Checked(stop, 1, 1);
			if (!txn_memtable_table_unlisted(tree, io, hold, owner, at, error))
				return false;
			continue;
		}
		if (host_shared_Record_atomic(range, TXN_MEMTABLE_RANGE_RANGE_KEYS, load) == 1)
		{
			bool		abandoned;

			if (!txn_memtable_table_abandoned(tree, io, owner, at.memtable, &abandoned, error))
			{
				if (*has_stop)
					std_vec_free_Checked(stop, 1, 1);
				return false;
			}
		}
		*some = true;
		return true;
	}
}

/* txn_memtable_table_range_starts_frame_0_t is range_starts' state across its pad: the hold, the memtable's guard, the keys, and a key copied, while live. */
typedef struct txn_memtable_table_range_starts_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	bool		result_live;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedGuard guard;
	bool		guard_live;
	StdVec		first;
	bool		first_live;
} txn_memtable_table_range_starts_frame_0_t;

/* txn_memtable_table_range_starts_pad_0 is range_starts' pad: the key copied, the guard, the hold, and the keys, while live. */
static void
txn_memtable_table_range_starts_pad_0(void *frame_)
{
	txn_memtable_table_range_starts_frame_0_t *frame = frame_;

	if (frame->first_live)
	{
		frame->first_live = false;
		std_vec_free_Checked(&frame->first, 1, 1);
	}
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
	if (frame->result_live)
	{
		frame->result_live = false;
		std_vec_clear(frame->result, sizeof(StdVec), txn_memtable_table_bytes_drop);
		std_vec_free_Checked(frame->result, sizeof(StdVec), _Alignof(StdVec));
	}
}

/* txn_memtable_table_range_starts_listed copies the first key of each range the memtable's list names into the keys, under its guard. */
static bool
txn_memtable_table_range_starts_listed(txn_memtable_table_range_starts_frame_0_t *frame)
{
	HostDeltaListListView view = host_delta_list_Guard_list(&frame->guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape,
															TXN_MEMTABLE_RANGE_RANGES);

	for (uint64 entry = 0, len = host_delta_list_ListView_len(&view); entry < len; entry++)
	{
		uint64		range;

		if (!host_delta_list_ListView_get(&view, &frame->hold, entry, &range, frame->error) ||
			!txn_memtable_table_first_key_copy(&frame->hold, range, &frame->first, frame->error))
			return false;
		frame->first_live = true;
		std_vec_push_Checked(frame->result, &frame->first, sizeof(frame->first), _Alignof(StdVec));
		frame->first_live = false;
	}
	return true;
}

/* txn_memtable_table_range_starts_body_0 is range_starts' body. */
static void
txn_memtable_table_range_starts_body_0(void *frame_)
{
	txn_memtable_table_range_starts_frame_0_t *frame = frame_;

	for (;;)
	{
		bool		some;
		TxnMemtableTableAt at;
		HostSharedRecord memtable;

		std_vec_new(frame->result, sizeof(StdVec), _Alignof(StdVec));
		frame->result_live = true;
		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		{
			txn_memtable_table_range_starts_pad_0(frame);
			return;
		}
		frame->hold_live = true;
		if (!txn_memtable_table_found(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, &frame->hold, frame->state, NULL,
									  0, NULL, &some, &at, frame->error))
		{
			txn_memtable_table_range_starts_pad_0(frame);
			return;
		}
		if (!some)
		{
			frame->ok = true;
			txn_memtable_table_range_starts_pad_0(frame);
			return;
		}
		if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, at.memtable, &memtable,
									frame->error) ||
			!host_shared_Record_lock_shared(memtable, HOST_IO_IO_TAKES_SHORT(frame->io),
											TXN_MEMTABLE_RANGE_MEMTABLE_LOCK, &frame->guard, frame->error))
		{
			txn_memtable_table_range_starts_pad_0(frame);
			return;
		}
		frame->guard_live = true;
		if (txn_memtable_index_closed(&TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS, &frame->guard))
		{
			frame->guard_live = false;
			host_shared_Guard_drop(&frame->guard);
			frame->hold_live = false;
			host_delta_Hold_drop(&frame->hold);
			frame->result_live = false;
			std_vec_free_Checked(frame->result, sizeof(StdVec), _Alignof(StdVec));
			continue;
		}
		if (!txn_memtable_table_range_starts_listed(frame))
		{
			txn_memtable_table_range_starts_pad_0(frame);
			return;
		}
		frame->result_live = false;
		frame->ok = true;
		txn_memtable_table_range_starts_pad_0(frame);
		return;
	}
}

/*
 * txn_memtable_table_range_starts returns the first key of each range of a
 * storage's rows' memtable, in its list's order, the empty key first, or
 * none for a storage with no memtable.
 */
bool
txn_memtable_table_range_starts(HostIoIo *io, HostSharedRecord state, StdVec *result, HostError *error)
{
	txn_memtable_table_range_starts_frame_0_t frame = {.io = io, .state = state, .result = result, .error = error};

	std_landing(&frame, txn_memtable_table_range_starts_body_0, txn_memtable_table_range_starts_pad_0);
	return frame.ok;
}

/*
 * TxnMemtableTableGathered is Gathered: what a retirement takes back once
 * nothing reaches it, the records leaving, the names of their lists' own
 * pages, and each page a range's list names with its count of lists, which
 * the retirement lowers, retiring a page whose last list it was.
 */
typedef struct TxnMemtableTableGathered
{
	/* records is a Vec<Leaving>. */
	StdVec		records;
	/* names is a Vec<u64> of pages' names. */
	StdVec		names;
	/* counts is a Vec<(u64, Word)>. */
	StdVec		counts;
} TxnMemtableTableGathered;

/* txn_memtable_table_Gathered_new makes an empty gathering. */
static void
txn_memtable_table_Gathered_new(TxnMemtableTableGathered *self)
{
	std_vec_new(&self->records, sizeof(HostDeltaLeaving), _Alignof(HostDeltaLeaving));
	std_vec_new(&self->names, sizeof(uint64), _Alignof(uint64));
	std_vec_new(&self->counts, sizeof(txn_memtable_table_DroppedPage_t), _Alignof(txn_memtable_table_DroppedPage_t));
}

/* txn_memtable_table_Gathered_drop drops a gathering's lists. */
static void
txn_memtable_table_Gathered_drop(TxnMemtableTableGathered *self)
{
	std_vec_free_Checked(&self->records, sizeof(HostDeltaLeaving), _Alignof(HostDeltaLeaving));
	std_vec_free_Checked(&self->names, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&self->counts, sizeof(txn_memtable_table_DroppedPage_t),
						 _Alignof(txn_memtable_table_DroppedPage_t));
}

/* txn_memtable_table_closed_names adds the names of the pages a closed list holds to a list of names. */
bool
txn_memtable_table_closed_names(StdVec *names, const HostDeltaListClosed *closed, const HostDeltaHold *hold,
								HostError *error)
{
	HostDeltaListPages pages = host_delta_list_Closed_pages(closed, hold);

	for (;;)
	{
		HostDeltaRetireItem item;
		uint64		name;

		if (!HOST_DELTA_LIST_PAGES_ITERATOR_OPS.next(&pages, &item))
			return true;
		if (!item.is_ok)
		{
			*error = item.err;
			return false;
		}
		if (!std_vec_try_reserve(names, 1, sizeof(uint64), _Alignof(uint64)))
		{
			*error = host_Error_out_of_memory();
			return false;
		}
		name = host_delta_page_Page_value(item.ok);
		std_vec_push_Checked(names, &name, sizeof(name), _Alignof(uint64));
	}
}

/* txn_memtable_table_shared_frame_0_t is under_shared's state across its pad: the record's shared guard, while live. */
typedef struct txn_memtable_table_shared_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord record;
	size_t		lock;
	bool		(*step) (void *captures, const HostSharedGuard *guard, HostError *error);
	void	   *captures;
	HostError  *error;
	bool		ok;
	HostSharedGuard guard;
	bool		guard_live;
} txn_memtable_table_shared_frame_0_t;

/* txn_memtable_table_shared_pad_0 is under_shared's pad: the guard, while live. */
static void
txn_memtable_table_shared_pad_0(void *frame_)
{
	txn_memtable_table_shared_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
}

/* txn_memtable_table_shared_body_0 is under_shared's body. */
static void
txn_memtable_table_shared_body_0(void *frame_)
{
	txn_memtable_table_shared_frame_0_t *frame = frame_;

	if (!host_shared_Record_lock_shared(frame->record, HOST_IO_IO_TAKES_SHORT(frame->io), frame->lock, &frame->guard,
										frame->error))
		return;
	frame->guard_live = true;
	frame->ok = frame->step(frame->captures, &frame->guard, frame->error);
	txn_memtable_table_shared_pad_0(frame);
}

/* txn_memtable_table_under_shared runs a step under a record's lock held shared, the guard dropped however the step ends. */
static bool
txn_memtable_table_under_shared(HostIoIo *io, HostSharedRecord record, size_t lock,
								bool (*step) (void *captures, const HostSharedGuard *guard, HostError *error),
								void *captures, HostError *error)
{
	txn_memtable_table_shared_frame_0_t frame = {.io = io, .record = record, .lock = lock, .step = step,
	.captures = captures, .error = error};

	std_landing(&frame, txn_memtable_table_shared_body_0, txn_memtable_table_shared_pad_0);
	return frame.ok;
}

/* txn_memtable_table_gathered_t is a gathering's step's captures: the gathering, the hold, and the record's location. */
typedef struct txn_memtable_table_gathered_t
{
	TxnMemtableTableGathered *self;
	const HostDeltaHold *hold;
	uint64		location;
} txn_memtable_table_gathered_t;

/* txn_memtable_table_Gathered_range_closure_0 gathers a closed range under its shared guard. */
static bool
txn_memtable_table_Gathered_range_closure_0(void *captures, const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_table_gathered_t *c = captures;
	TxnMemtableTableGathered *self = c->self;
	HostDeltaListClosed closed;
	HostDeltaListListView view;
	HostDeltaLeaving leaving = host_delta_Leaving_of(&TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, c->location);

	if (!host_delta_list_Guard_closed(guard, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES,
									  &closed))
	{
		*error = txn_memtable_table_internal("a closed range's list is open");
		return false;
	}
	view = host_delta_list_Guard_list(guard, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES);
	if (!std_vec_try_reserve(&self->records, 1, sizeof(HostDeltaLeaving), _Alignof(HostDeltaLeaving)) ||
		!std_vec_try_reserve(&self->counts, (size_t) host_delta_list_ListView_len(&view),
							 sizeof(txn_memtable_table_DroppedPage_t), _Alignof(txn_memtable_table_DroppedPage_t)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	for (uint64 index = 0, len = host_delta_list_ListView_len(&view); index < len; index++)
	{
		txn_memtable_table_DroppedPage_t count;
		HostDeltaPageRef page;

		if (!host_delta_list_ListView_get(&view, c->hold, index, &count.f0, error))
			return false;
		if (count.f0 == TXN_MEMTABLE_PAGES_DROPPED)
			continue;
		if (!host_delta_Hold_page(c->hold, host_delta_page_Page_named(count.f0), &page, error) ||
			!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_LISTS, &count.f1, error))
			return false;
		std_vec_push_Checked(&self->counts, &count, sizeof(count), _Alignof(txn_memtable_table_DroppedPage_t));
	}
	if (!txn_memtable_table_closed_names(&self->names, &closed, c->hold, error))
		return false;
	std_vec_push_Checked(&self->records, &leaving, sizeof(leaving), _Alignof(HostDeltaLeaving));
	return true;
}

/*
 * txn_memtable_table_Gathered_range gathers a closed range: its record, its
 * list's own pages, and each page its list names with its count of lists.
 */
static bool
txn_memtable_table_Gathered_range(TxnMemtableTableGathered *self, HostIoIo *io, const HostDeltaHold *hold,
								  uint64 location, HostError *error)
{
	HostSharedRecord record;
	txn_memtable_table_gathered_t captures = {self, hold, location};

	return host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, location, &record, error) &&
		txn_memtable_table_under_shared(io, record, TXN_MEMTABLE_RANGE_RANGE_LOCK,
										txn_memtable_table_Gathered_range_closure_0, &captures, error);
}

/* txn_memtable_table_Gathered_memtable_closure_0 gathers a closed memtable under its shared guard. */
static bool
txn_memtable_table_Gathered_memtable_closure_0(void *captures, const HostSharedGuard *guard, HostError *error)
{
	txn_memtable_table_gathered_t *c = captures;
	TxnMemtableTableGathered *self = c->self;
	HostDeltaListClosed closed;
	HostDeltaLeaving leaving = host_delta_Leaving_of(&TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, c->location);

	if (!host_delta_list_Guard_closed(guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape, TXN_MEMTABLE_RANGE_RANGES,
									  &closed))
	{
		*error = txn_memtable_table_internal("a closed memtable's list is open");
		return false;
	}
	if (!std_vec_try_reserve(&self->records, 1, sizeof(HostDeltaLeaving), _Alignof(HostDeltaLeaving)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	if (!txn_memtable_table_closed_names(&self->names, &closed, c->hold, error))
		return false;
	std_vec_push_Checked(&self->records, &leaving, sizeof(leaving), _Alignof(HostDeltaLeaving));
	return true;
}

/* txn_memtable_table_Gathered_memtable gathers a closed memtable: its record and its list's own pages. */
static bool
txn_memtable_table_Gathered_memtable(TxnMemtableTableGathered *self, HostIoIo *io, const HostDeltaHold *hold,
									 uint64 location, HostError *error)
{
	HostSharedRecord record;
	txn_memtable_table_gathered_t captures = {self, hold, location};

	return host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, location, &record, error) &&
		txn_memtable_table_under_shared(io, record, TXN_MEMTABLE_RANGE_MEMTABLE_LOCK,
										txn_memtable_table_Gathered_memtable_closure_0, &captures, error);
}

/*
 * txn_memtable_table_GatheredPages is the pages a gathering's retirement
 * gives back, as Rust's chain of its names and a filter_map of its counts:
 * the names, then each page whose count the iteration lowers to zero, each
 * count lowered only when the iteration reaches it.
 */
typedef struct txn_memtable_table_GatheredPages
{
	const uint64 *names;
	size_t		names_len;
	size_t		at;
	txn_memtable_table_DroppedPage_t *counts;
	size_t		counts_len;
	size_t		count_at;
} txn_memtable_table_GatheredPages;

/* txn_memtable_table_GatheredPages_next gives the next page's name as an Ok item: a gathered name, or a counted page whose last list it lowers. */
static bool
txn_memtable_table_GatheredPages_next(void *self_, void *result)
{
	txn_memtable_table_GatheredPages *self = self_;
	HostDeltaRetireItem *item = result;

	if (self->at < self->names_len)
	{
		item->is_ok = true;
		item->ok = host_delta_page_Page_named(self->names[self->at]);
		self->at = std_wrapping_add_usize(self->at, 1);
		return true;
	}
	while (self->count_at < self->counts_len)
	{
		txn_memtable_table_DroppedPage_t *count = &self->counts[self->count_at];

		self->count_at = std_wrapping_add_usize(self->count_at, 1);
		if (host_delta_Word_fetch_add(&count->f1, PG_UINT64_MAX) == 1)
		{
			item->is_ok = true;
			item->ok = host_delta_page_Page_named(count->f0);
			return true;
		}
	}
	return false;
}

/* txn_memtable_table_GatheredPages_size_hint bounds the names left: the gathered ones at least, and every count's at most. */
static StdSizeHint
txn_memtable_table_GatheredPages_size_hint(const void *self_)
{
	const txn_memtable_table_GatheredPages *self = self_;
	size_t		names = std_wrapping_sub_usize(self->names_len, self->at);
	size_t		counts = std_wrapping_sub_usize(self->counts_len, self->count_at);
	StdSizeHint hint = {names, true, names + counts};

	return hint;
}

/* TXN_MEMTABLE_TABLE_GATHERED_PAGES_ITERATOR_OPS is GatheredPages' Iterator. */
static const StdIteratorOps TXN_MEMTABLE_TABLE_GATHERED_PAGES_ITERATOR_OPS = {
	txn_memtable_table_GatheredPages_next, txn_memtable_table_GatheredPages_size_hint, NULL,
};

/* txn_memtable_table_Gathered_retire retires what a gathering holds: its records, its lists' pages, and each page whose last list it lowers. */
static bool
txn_memtable_table_Gathered_retire(TxnMemtableTableGathered *self, HostIoIo *io, HostError *error)
{
	txn_memtable_table_GatheredPages names;
	StdIterator pages;

	names.names = self->names.ptr;
	names.names_len = self->names.len;
	names.at = 0;
	names.counts = self->counts.ptr;
	names.counts_len = self->counts.len;
	names.count_at = 0;
	pages.ops = &TXN_MEMTABLE_TABLE_GATHERED_PAGES_ITERATOR_OPS;
	pages.self = &names;
	return host_delta_retire(io, self->records.ptr, self->records.len, pages, error);
}

/*
 * txn_memtable_table_unlist takes a range's entry out of an open memtable's
 * list and closes the memtable when its list is left empty and no process's
 * pin names it, answering whether it closed it.
 */
bool
txn_memtable_table_unlist(const TxnMemtableTableTreeOps *tree, TxnMemtableIndexOpen open, const HostDeltaHold *hold,
						  uint64 entry, uint64 memtable, bool *result, HostError *error)
{
	HostDeltaListListMut list = host_delta_list_GuardExclusive_list_mut(open.guard, open.closes->shape,
																		open.closes->lock, TXN_MEMTABLE_RANGE_RANGES);
	HostDeltaListListView view;
	bool		pinned = false;

	*result = false;
	if (!host_delta_list_ListMut_remove(&list, hold, entry, error))
		return false;
	view = host_delta_list_ListMut_view(&list);
	if (host_delta_list_ListView_is_empty(&view) && !txn_memtable_pins_pinned(tree->pin, memtable, &pinned, error))
		return false;
	*result = host_delta_list_ListView_is_empty(&view) && !pinned;
	if (*result)
		txn_memtable_index_Open_close(open);
	return true;
}

/* txn_memtable_table_replaced puts a split range's halves over its entry in an open memtable's list, one entry the list kept for them coming back. */
static bool
txn_memtable_table_replaced(TxnMemtableIndexOpen open, const HostDeltaHold *hold, uint64 entry, uint64 lower,
							uint64 upper, HostError *error)
{
	HostDeltaListListMut list = host_delta_list_GuardExclusive_list_mut(open.guard, open.closes->shape,
																		open.closes->lock, TXN_MEMTABLE_RANGE_RANGES);
	uint64		halves[2] = {lower, upper};
	StdVec		spare;
	HostSharedOp op = {.tag = HOST_SHARED_OP_ADD, .u.add = PG_UINT64_MAX};
	bool		ok;

	std_vec_new(&spare, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	ok = host_delta_list_ListMut_replace_with(&list, hold, entry, halves, &spare, error);
	std_vec_clear(&spare, sizeof(HostDeltaOwned), host_delta_Owned_drop);
	std_vec_free_Checked(&spare, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	if (ok)
		host_shared_Record_atomic(host_shared_Guard_record(open.guard), TXN_MEMTABLE_RANGE_MEMTABLE_KEPT, op);
	return ok;
}

/* txn_memtable_table_unlisted_frame_0_t is unlisted's state across its pad: the gathering, the memtable's guard, and the range's first key, while live. */
typedef struct txn_memtable_table_unlisted_frame_0_t
{
	const TxnMemtableTableTreeOps *tree;
	HostIoIo   *io;
	const HostDeltaHold *hold;
	HostSharedRecord owner;
	TxnMemtableTableAt at;
	HostError  *error;
	bool		ok;
	TxnMemtableTableGathered gathered;
	bool		gathered_live;
	HostSharedGuard guard;
	bool		guard_live;
	HostDeltaLent first;
	bool		first_live;
} txn_memtable_table_unlisted_frame_0_t;

/* txn_memtable_table_unlisted_pad_0 is unlisted's pad: the first key, the guard, and the gathering, while live. */
static void
txn_memtable_table_unlisted_pad_0(void *frame_)
{
	txn_memtable_table_unlisted_frame_0_t *frame = frame_;

	if (frame->first_live)
	{
		frame->first_live = false;
		host_delta_Lent_drop(&frame->first);
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	if (frame->gathered_live)
	{
		frame->gathered_live = false;
		txn_memtable_table_Gathered_drop(&frame->gathered);
	}
}

/* txn_memtable_table_unlisted_taken takes the range out of the memtable's list under its exclusive guard, when the list still names it, answering whether it did and whether the take left the memtable empty. */
static bool
txn_memtable_table_unlisted_taken(txn_memtable_table_unlisted_frame_0_t *frame, bool has_lower, uint64 lower_at,
								  bool has_upper, uint64 upper_at, bool *removed, bool *emptied)
{
	TxnMemtableIndexOpen open;
	HostDeltaListListView view;
	std_TupleU64U64_t found;
	StdBytes	bytes;
	bool		ok;

	if (!txn_memtable_index_open(&TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS, &frame->guard, &open))
		return true;
	view = host_delta_list_Guard_list(&frame->guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape,
									  TXN_MEMTABLE_RANGE_RANGES);
	if (host_delta_list_ListView_is_empty(&view))
		return true;
	if (!txn_memtable_table_first_key(frame->hold, frame->at.range, &frame->first, frame->error))
		return false;
	frame->first_live = true;
	bytes = host_delta_Lent_Deref_deref(&frame->first);
	ok = txn_memtable_table_search(&view, frame->hold, bytes.ptr, bytes.len, &found, frame->error);
	frame->first_live = false;
	host_delta_Lent_drop(&frame->first);
	if (!ok || found.f1 != frame->at.range)
		return ok;
	*removed = true;
	return has_lower && has_upper ?
		txn_memtable_table_replaced(open, frame->hold, found.f0, lower_at, upper_at, frame->error) :
		txn_memtable_table_unlist(frame->tree, open, frame->hold, found.f0, frame->at.memtable, emptied,
								  frame->error);
}

/* txn_memtable_table_unlisted_body_0 is unlisted's body. */
static void
txn_memtable_table_unlisted_body_0(void *frame_)
{
	txn_memtable_table_unlisted_frame_0_t *frame = frame_;
	HostSharedRecord record;
	HostSharedPast lower;
	HostSharedPast upper;
	uint64		lower_at;
	uint64		upper_at;
	bool		has_lower;
	bool		has_upper;
	HostSharedRecord memtable;
	bool		removed = false;
	bool		emptied = false;

	txn_memtable_table_Gathered_new(&frame->gathered);
	frame->gathered_live = true;
	if (!txn_memtable_table_Gathered_range(&frame->gathered, frame->io, frame->hold, frame->at.range, frame->error) ||
		!host_delta_Hold_record(frame->hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, frame->at.range, &record,
								frame->error))
	{
		txn_memtable_table_unlisted_pad_0(frame);
		return;
	}
	lower = host_shared_Record_names_past(record, TXN_MEMTABLE_RANGE_RANGE_LOWER);
	upper = host_shared_Record_names_past(record, TXN_MEMTABLE_RANGE_RANGE_UPPER);
	has_lower = host_shared_Past_load(&lower, &lower_at);
	has_upper = host_shared_Past_load(&upper, &upper_at);
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-memtable-unlisting", frame->error) ||
		!host_delta_Hold_record(frame->hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, frame->at.memtable, &memtable,
								frame->error) ||
		!host_shared_Record_lock(memtable, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_MEMTABLE_LOCK,
								 &frame->guard, frame->error))
	{
		txn_memtable_table_unlisted_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	if (!txn_memtable_table_unlisted_taken(frame, has_lower, lower_at, has_upper, upper_at, &removed, &emptied))
	{
		txn_memtable_table_unlisted_pad_0(frame);
		return;
	}
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	if (removed && !txn_memtable_table_Gathered_retire(&frame->gathered, frame->io, frame->error))
	{
		txn_memtable_table_unlisted_pad_0(frame);
		return;
	}
	frame->gathered_live = false;
	txn_memtable_table_Gathered_drop(&frame->gathered);
	frame->ok = !emptied || txn_memtable_table_cleared(frame->tree, frame->io, frame->hold, frame->owner,
													   frame->at.memtable, frame->error);
}

/*
 * txn_memtable_table_unlisted takes a closed range out of its memtable's
 * list, as any finder of a closed range does: it gathers the range's pages
 * first, then, under the memtable's exclusive lock, puts its halves in its
 * place or takes its entry out when the entry still names it, retiring the
 * range once it is out, and clears the owner's word of a memtable the take
 * left empty.
 */
bool
txn_memtable_table_unlisted(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
							HostSharedRecord owner, TxnMemtableTableAt at, HostError *error)
{
	txn_memtable_table_unlisted_frame_0_t frame = {.tree = tree, .io = io, .hold = hold, .owner = owner, .at = at,
	.error = error};

	std_landing(&frame, txn_memtable_table_unlisted_body_0, txn_memtable_table_unlisted_pad_0);
	return frame.ok;
}

/* txn_memtable_table_cleared_frame_0_t is cleared's state across its pad: the gathering and the owner's guard, while live. */
typedef struct txn_memtable_table_cleared_frame_0_t
{
	const TxnMemtableTableTreeOps *tree;
	HostIoIo   *io;
	const HostDeltaHold *hold;
	HostSharedRecord owner;
	uint64		location;
	HostError  *error;
	bool		ok;
	TxnMemtableTableGathered gathered;
	bool		gathered_live;
	HostSharedGuard guard;
	bool		guard_live;
} txn_memtable_table_cleared_frame_0_t;

/* txn_memtable_table_cleared_pad_0 is cleared's pad: the guard, then the gathering, while live. */
static void
txn_memtable_table_cleared_pad_0(void *frame_)
{
	txn_memtable_table_cleared_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	if (frame->gathered_live)
	{
		frame->gathered_live = false;
		txn_memtable_table_Gathered_drop(&frame->gathered);
	}
}

/* txn_memtable_table_cleared_body_0 is cleared's body. */
static void
txn_memtable_table_cleared_body_0(void *frame_)
{
	txn_memtable_table_cleared_frame_0_t *frame = frame_;
	HostSharedPast word;
	bool		cleared;

	txn_memtable_table_Gathered_new(&frame->gathered);
	frame->gathered_live = true;
	if (!txn_memtable_table_Gathered_memtable(&frame->gathered, frame->io, frame->hold, frame->location,
											  frame->error) ||
		!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-memtable-clearing", frame->error) ||
		!host_shared_Record_lock(frame->owner, HOST_IO_IO_TAKES_SHORT(frame->io), frame->tree->lock, &frame->guard,
								 frame->error))
	{
		txn_memtable_table_cleared_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	word = host_shared_Record_names_past(frame->owner, frame->tree->word);
	cleared = host_shared_Past_clear(&word, frame->location);
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	frame->ok = !cleared || txn_memtable_table_Gathered_retire(&frame->gathered, frame->io, frame->error);
	txn_memtable_table_cleared_pad_0(frame);
}

/*
 * txn_memtable_table_cleared clears an owner's word of its closed memtable
 * under the owner's lock, retiring the memtable and its list's pages once the
 * word no longer names it.
 */
static bool
txn_memtable_table_cleared(const TxnMemtableTableTreeOps *tree, HostIoIo *io, const HostDeltaHold *hold,
						   HostSharedRecord owner, uint64 location, HostError *error)
{
	txn_memtable_table_cleared_frame_0_t frame = {.tree = tree, .io = io, .hold = hold, .owner = owner,
	.location = location, .error = error};

	std_landing(&frame, txn_memtable_table_cleared_body_0, txn_memtable_table_cleared_pad_0);
	return frame.ok;
}

/* txn_memtable_table_closed_by closes the record a guard holds exclusively when it is still open. */
static void
txn_memtable_table_closed_by(const TxnMemtableIndexClosesOps *closes, HostSharedGuard *guard)
{
	TxnMemtableIndexOpen open;

	if (txn_memtable_index_open(closes, guard, &open))
		txn_memtable_index_Open_close(open);
}

/* txn_memtable_table_retired_frame_0_t is retired's state across its pad: the hold, a guard, the ranges, and the gathering, while live. */
typedef struct txn_memtable_table_retired_frame_0_t
{
	const TxnMemtableTableTreeOps *tree;
	HostIoIo   *io;
	HostSharedRecord owner;
	uint64		location;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedGuard guard;
	bool		guard_live;
	/* ranges is a Vec<u64>. */
	StdVec		ranges;
	bool		ranges_live;
	TxnMemtableTableGathered gathered;
	bool		gathered_live;
} txn_memtable_table_retired_frame_0_t;

/* txn_memtable_table_retired_pad_0 is retired's pad: what it holds, while live. */
static void
txn_memtable_table_retired_pad_0(void *frame_)
{
	txn_memtable_table_retired_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	if (frame->gathered_live)
	{
		frame->gathered_live = false;
		txn_memtable_table_Gathered_drop(&frame->gathered);
	}
	if (frame->ranges_live)
	{
		frame->ranges_live = false;
		std_vec_free_Checked(&frame->ranges, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
}

/* txn_memtable_table_retired_ranges gathers the memtable's ranges and closes it, under its exclusive guard. */
static bool
txn_memtable_table_retired_ranges(txn_memtable_table_retired_frame_0_t *frame)
{
	HostSharedRecord memtable;
	HostDeltaListListView view;

	if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, frame->location, &memtable,
								frame->error) ||
		!host_shared_Record_lock(memtable, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_MEMTABLE_LOCK,
								 &frame->guard, frame->error))
		return false;
	frame->guard_live = true;
	view = host_delta_list_Guard_list(&frame->guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape,
									  TXN_MEMTABLE_RANGE_RANGES);
	for (uint64 entry = 0, len = host_delta_list_ListView_len(&view); entry < len; entry++)
	{
		uint64		range;

		if (!host_delta_list_ListView_get(&view, &frame->hold, entry, &range, frame->error))
			return false;
		std_vec_push_Checked(&frame->ranges, &range, sizeof(range), _Alignof(uint64));
	}
	txn_memtable_table_closed_by(&TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS, &frame->guard);
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	return true;
}

/* txn_memtable_table_retired_gathered closes each of the memtable's ranges and gathers it, then the memtable. */
static bool
txn_memtable_table_retired_gathered(txn_memtable_table_retired_frame_0_t *frame)
{
	for (size_t k = 0; k < frame->ranges.len; k++)
	{
		uint64		range = STD_VEC_AT(&frame->ranges, uint64, k);
		HostSharedRecord record;

		if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, range, &record, frame->error) ||
			!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_RANGE_LOCK,
									 &frame->guard, frame->error))
			return false;
		frame->guard_live = true;
		txn_memtable_table_closed_by(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS.closes, &frame->guard);
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
		if (!txn_memtable_table_Gathered_range(&frame->gathered, frame->io, &frame->hold, range, frame->error))
			return false;
	}
	return txn_memtable_table_Gathered_memtable(&frame->gathered, frame->io, &frame->hold, frame->location,
												frame->error);
}

/* txn_memtable_table_retired_body_0 is retired's body. */
static void
txn_memtable_table_retired_body_0(void *frame_)
{
	txn_memtable_table_retired_frame_0_t *frame = frame_;
	HostSharedPast word;
	bool		cleared;

	if (!txn_memtable_table_memtable(frame->tree, frame->owner, &frame->location))
	{
		frame->ok = true;
		return;
	}
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	std_vec_new(&frame->ranges, sizeof(uint64), _Alignof(uint64));
	frame->ranges_live = true;
	txn_memtable_table_Gathered_new(&frame->gathered);
	frame->gathered_live = true;
	if (!txn_memtable_table_retired_ranges(frame) || !txn_memtable_table_retired_gathered(frame) ||
		!host_shared_Record_lock(frame->owner, HOST_IO_IO_TAKES_SHORT(frame->io), frame->tree->lock, &frame->guard,
								 frame->error))
	{
		txn_memtable_table_retired_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	word = host_shared_Record_names_past(frame->owner, frame->tree->word);
	cleared = host_shared_Past_clear(&word, frame->location);
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	frame->ok = !cleared || txn_memtable_table_Gathered_retire(&frame->gathered, frame->io, frame->error);
	txn_memtable_table_retired_pad_0(frame);
}

/*
 * txn_memtable_table_retired retires a tree's memtable at its storage's
 * unlink, when no process writes to it: it closes the memtable and each of
 * its ranges, gathers them all, and retires them once the owner's word no
 * longer names the memtable.
 */
bool
txn_memtable_table_retired(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
						   HostError *error)
{
	txn_memtable_table_retired_frame_0_t frame = {.tree = tree, .io = io, .owner = owner, .error = error};

	std_landing(&frame, txn_memtable_table_retired_body_0, txn_memtable_table_retired_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_table_retire_all retires a storage's memtables at its unlink,
 * when no process writes to it: its rows', its free space map's, and its
 * value map's, and then its tuple id table.
 */
bool
txn_memtable_table_retire_all(HostIoIo *io, HostSharedRecord state, HostError *error)
{
	return txn_memtable_table_retired(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, error) &&
		txn_memtable_table_retired(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, state, error) &&
		txn_memtable_table_retired(&TXN_MEMTABLE_MAP_VALUES_TREE_OPS, io, state, error) &&
		txn_memtable_tuples_retired(io, state, error);
}

/* txn_memtable_table_Versions_drop drops a key and its versions. */
static void
txn_memtable_table_Versions_drop(txn_TupleVecU8VecVersion_t *self)
{
	std_vec_free_Checked(&self->f0, 1, 1);
	std_vec_clear(&self->f1, sizeof(TxnMemtableVersionVersion), txn_memtable_table_Version_drop);
	std_vec_free_Checked(&self->f1, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
}

/* txn_memtable_table_chained_frame_0_t is chained's state across its pad: the versions and the node's key, while live. */
typedef struct txn_memtable_table_chained_frame_0_t
{
	const HostSharedGuard *guard;
	const HostDeltaHold *hold;
	TxnMemtableIndexNode node;
	bool	   *some;
	txn_TupleVecU8VecVersion_t *result;
	HostError  *error;
	bool		ok;
	bool		versions_live;
	HostDeltaLent key;
	bool		key_live;
} txn_memtable_table_chained_frame_0_t;

/* txn_memtable_table_chained_pad_0 is chained's pad: the key, then the versions, while live. */
static void
txn_memtable_table_chained_pad_0(void *frame_)
{
	txn_memtable_table_chained_frame_0_t *frame = frame_;

	if (frame->key_live)
	{
		frame->key_live = false;
		host_delta_Lent_drop(&frame->key);
	}
	if (frame->versions_live)
	{
		frame->versions_live = false;
		std_vec_clear(&frame->result->f1, sizeof(TxnMemtableVersionVersion), txn_memtable_table_Version_drop);
		std_vec_free_Checked(&frame->result->f1, sizeof(TxnMemtableVersionVersion),
							 _Alignof(TxnMemtableVersionVersion));
	}
}

/* txn_memtable_table_chained_body_0 is chained's body. */
static void
txn_memtable_table_chained_body_0(void *frame_)
{
	txn_memtable_table_chained_frame_0_t *frame = frame_;
	StdBytes	bytes;

	*frame->some = false;
	std_vec_new(&frame->result->f1, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	frame->versions_live = true;
	if (!txn_memtable_table_chain_of(frame->guard, frame->hold, frame->node, &frame->result->f1, frame->error))
	{
		txn_memtable_table_chained_pad_0(frame);
		return;
	}
	if (frame->result->f1.len == 0)
	{
		frame->ok = true;
		txn_memtable_table_chained_pad_0(frame);
		return;
	}
	if (!txn_memtable_index_key(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, frame->guard, frame->hold, frame->node,
								&frame->key, frame->error))
	{
		txn_memtable_table_chained_pad_0(frame);
		return;
	}
	frame->key_live = true;
	bytes = host_delta_Lent_Deref_deref(&frame->key);
	std_slice_to_vec_Checked(&frame->result->f0, bytes.ptr, bytes.len, 1, 1);
	frame->versions_live = false;
	*frame->some = true;
	frame->ok = true;
	txn_memtable_table_chained_pad_0(frame);
}

/*
 * txn_memtable_table_chained returns a node's key and its versions, newest
 * first, under its range's lock, or nothing for a key whose chain is empty.
 */
static bool
txn_memtable_table_chained(const HostSharedGuard *guard, const HostDeltaHold *hold, TxnMemtableIndexNode node,
						   bool *some, txn_TupleVecU8VecVersion_t *result, HostError *error)
{
	txn_memtable_table_chained_frame_0_t frame = {.guard = guard, .hold = hold, .node = node, .some = some,
	.result = result, .error = error};

	std_landing(&frame, txn_memtable_table_chained_body_0, txn_memtable_table_chained_pad_0);
	return frame.ok;
}

/* txn_memtable_table_nearest_t is forward's and backward's reading's captures: where the walk is, and the nearest key with versions found. */
typedef struct txn_memtable_table_nearest_t
{
	bool		has_place;
	const StdVec *place;
	bool		some;
	txn_TupleVecU8VecVersion_t found;
} txn_memtable_table_nearest_t;

/* txn_memtable_table_forward_reading finds the first key at or past the place with versions, before the step's stop. */
static bool
txn_memtable_table_forward_reading(void *captures, const HostDeltaHold *hold, const HostSharedGuard *guard,
								   bool has_stop, const uint8 *stop, size_t stop_len, HostError *error)
{
	txn_memtable_table_nearest_t *nearest = captures;
	bool		some;
	TxnMemtableIndexNode node;

	nearest->some = false;
	if (!txn_memtable_index_seek(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, nearest->place->ptr,
								 nearest->place->len, &some, &node, error))
		return false;
	while (some)
	{
		HostDeltaLent key;
		StdBytes	bytes;
		bool		past;

		if (!txn_memtable_index_key(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &key, error))
			return false;
		bytes = host_delta_Lent_Deref_deref(&key);
		past = has_stop && std_bytes_Ord_cmp(bytes.ptr, bytes.len, stop, stop_len) != STD_ORDERING_LESS;
		host_delta_Lent_drop(&key);
		if (past)
			return true;
		if (!txn_memtable_table_chained(guard, hold, node, &nearest->some, &nearest->found, error))
			return false;
		if (nearest->some)
			return true;
		if (!txn_memtable_index_next(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &some, &node, error))
			return false;
	}
	return true;
}

/* txn_memtable_table_backward_reading finds the last key below the place with versions, at or past the step's stop. */
static bool
txn_memtable_table_backward_reading(void *captures, const HostDeltaHold *hold, const HostSharedGuard *guard,
									bool has_stop, const uint8 *stop, size_t stop_len, HostError *error)
{
	txn_memtable_table_nearest_t *nearest = captures;
	bool		some;
	TxnMemtableIndexNode node;

	nearest->some = false;
	if (!(nearest->has_place ?
		  txn_memtable_index_before(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, nearest->place->ptr,
									nearest->place->len, &some, &node, error) :
		  txn_memtable_index_last(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, &some, &node, error)))
		return false;
	while (some)
	{
		HostDeltaLent key;
		StdBytes	bytes;
		bool		ok;

		if (!txn_memtable_index_key(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &key, error))
			return false;
		bytes = host_delta_Lent_Deref_deref(&key);
		if (has_stop && std_bytes_Ord_cmp(bytes.ptr, bytes.len, stop, stop_len) == STD_ORDERING_LESS)
		{
			host_delta_Lent_drop(&key);
			return true;
		}
		ok = txn_memtable_table_chained(guard, hold, node, &nearest->some, &nearest->found, error);
		if (ok && !nearest->some)
			ok = txn_memtable_index_before(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, bytes.ptr, bytes.len,
										   &some, &node, error);
		host_delta_Lent_drop(&key);
		if (!ok)
			return false;
		if (nearest->some)
			return true;
	}
	return true;
}

/* txn_memtable_table_nearest_frame_0_t is nearest's state across its pad: the place, a step's stop, and the key found in its captures, while live. */
typedef struct txn_memtable_table_nearest_frame_0_t
{
	HostIoIo   *io;
	const HostDeltaHold *hold;
	HostSharedRecord state;
	bool		has_after;
	const uint8 *after;
	size_t		after_len;
	TxnMemtableTableDirection way;
	bool	   *some;
	txn_TupleVecU8VecVersion_t *result;
	HostError  *error;
	bool		ok;
	StdVec		place;
	bool		place_live;
	StdVec		stop;
	bool		stop_live;
	txn_memtable_table_nearest_t nearest;
} txn_memtable_table_nearest_frame_0_t;

/* txn_memtable_table_nearest_pad_0 is nearest's pad: the key found, the stop, and the place, while live. */
static void
txn_memtable_table_nearest_pad_0(void *frame_)
{
	txn_memtable_table_nearest_frame_0_t *frame = frame_;

	if (frame->nearest.some)
	{
		frame->nearest.some = false;
		txn_memtable_table_Versions_drop(&frame->nearest.found);
	}
	if (frame->stop_live)
	{
		frame->stop_live = false;
		std_vec_free_Checked(&frame->stop, 1, 1);
	}
	if (frame->place_live)
	{
		frame->place_live = false;
		std_vec_free_Checked(&frame->place, 1, 1);
	}
}

/* txn_memtable_table_nearest_body_0 is nearest's body. */
static void
txn_memtable_table_nearest_body_0(void *frame_)
{
	txn_memtable_table_nearest_frame_0_t *frame = frame_;
	bool		forward = frame->way == TXN_MEMTABLE_TABLE_DIRECTION_FORWARD;

	*frame->some = false;
	frame->nearest.has_place = frame->has_after || forward;
	frame->nearest.place = &frame->place;
	if (frame->has_after)
	{
		std_slice_to_vec_Checked(&frame->place, frame->after, frame->after_len, 1, 1);
		frame->place_live = true;
		if (forward)
		{
			uint8		zero = 0;

			std_vec_push_Checked(&frame->place, &zero, 1, 1);
		}
	}
	else
	{
		std_vec_new(&frame->place, 1, 1);
		frame->place_live = true;
	}
	for (;;)
	{
		bool		stepped;
		bool		has_stop;

		if (!txn_memtable_table_stepped(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, frame->hold, frame->state,
										frame->nearest.has_place, frame->place.ptr, frame->place.len, frame->way,
										forward ? txn_memtable_table_forward_reading :
										txn_memtable_table_backward_reading, &frame->nearest, &stepped, &has_stop,
										&frame->stop, frame->error))
		{
			txn_memtable_table_nearest_pad_0(frame);
			return;
		}
		frame->stop_live = has_stop;
		if (!stepped)
			break;
		if (frame->nearest.some)
		{
			frame->nearest.some = false;
			*frame->some = true;
			*frame->result = frame->nearest.found;
			break;
		}
		if (!has_stop || (!forward && frame->stop.len == 0))
			break;
		std_vec_free_Checked(&frame->place, 1, 1);
		frame->place = frame->stop;
		frame->stop_live = false;
		frame->nearest.has_place = true;
	}
	frame->ok = true;
	txn_memtable_table_nearest_pad_0(frame);
}

/*
 * txn_memtable_table_nearest walks a storage's rows' memtable from a place,
 * forward past it or backward below it, step by step, to the nearest key
 * with versions, answering nothing past the last or below the first.
 */
static bool
txn_memtable_table_nearest(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, bool has_after,
						   const uint8 *after, size_t after_len, TxnMemtableTableDirection way, bool *some,
						   txn_TupleVecU8VecVersion_t *result, HostError *error)
{
	txn_memtable_table_nearest_frame_0_t frame = {.io = io, .hold = hold, .state = state, .has_after = has_after,
	.after = after, .after_len = after_len, .way = way, .some = some, .result = result, .error = error};

	std_landing(&frame, txn_memtable_table_nearest_body_0, txn_memtable_table_nearest_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_table_next_read_t is next's read's captures: its arguments,
 * the key it lends with its memtable versions, and the key's spilled
 * versions, while it lends a key.
 */
typedef struct txn_memtable_table_next_read_t
{
	HostIoIo   *io;
	const HostDeltaHold *hold;
	HostSharedRecord state;
	bool		has_after;
	const uint8 *after;
	size_t		after_len;
	TxnMemtableTableDirection way;
	bool	   *some;
	txn_TupleVecU8VecVersion_t *result;
	StdVec		spilled;
} txn_memtable_table_next_read_t;

/* txn_memtable_table_next_dropped drops the key a read of next lent. */
static void
txn_memtable_table_next_dropped(void *captures)
{
	txn_memtable_table_next_read_t *c = captures;

	if (*c->some)
	{
		txn_memtable_table_Versions_drop(c->result);
		txn_memtable_version_versions_drop(&c->spilled, 0);
	}
	*c->some = false;
}

/*
 * txn_memtable_table_next_read is next's read: the memtable's nearest key,
 * then the spills', the nearer lent, its spilled versions apart.
 */
static bool
txn_memtable_table_next_read(void *captures, uint64 *listed, HostError *error)
{
	txn_memtable_table_next_read_t *c = captures;
	HostIoIo   *io = c->io;
	TxnMemtableTableDirection way = c->way;
	bool	   *some = c->some;
	txn_TupleVecU8VecVersion_t *result = c->result;
	bool		has_delta;
	txn_TupleVecU8VecVersion_t delta;
	bool		has_spilled;
	txn_TupleVecU8VecVersion_t spilled;

	*some = false;
	if (!txn_memtable_table_nearest(io, c->hold, c->state, c->has_after, c->after, c->after_len, way, &has_delta,
									&delta, error))
		return false;
	if (!txn_spill_next(&TXN_SPILL_STATE_HOME_OPS, io, c->hold, c->state, TXN_RUNS_KIND_SPILLED_ROWS, c->has_after,
						c->after, c->after_len, way, listed, &has_spilled, &spilled, error))
	{
		if (has_delta)
			txn_memtable_table_Versions_drop(&delta);
		return false;
	}
	if (has_delta && has_spilled)
	{
		StdOrdering order = std_bytes_Ord_cmp(spilled.f0.ptr, spilled.f0.len, delta.f0.ptr, delta.f0.len);

		if (order == STD_ORDERING_EQUAL)
		{
			std_vec_free_Checked(&spilled.f0, 1, 1);
			c->spilled = spilled.f1;
			*result = delta;
		}
		else if (way == TXN_MEMTABLE_TABLE_DIRECTION_FORWARD ? order == STD_ORDERING_LESS :
				 order == STD_ORDERING_GREATER)
		{
			txn_memtable_table_Versions_drop(&delta);
			c->spilled = spilled.f1;
			result->f0 = spilled.f0;
			std_vec_new(&result->f1, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
		}
		else
		{
			txn_memtable_table_Versions_drop(&spilled);
			std_vec_new(&c->spilled, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
			*result = delta;
		}
		*some = true;
	}
	else if (has_delta)
	{
		std_vec_new(&c->spilled, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
		*result = delta;
		*some = true;
	}
	else if (has_spilled)
	{
		c->spilled = spilled.f1;
		result->f0 = spilled.f0;
		std_vec_new(&result->f1, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
		*some = true;
	}
	return true;
}

/*
 * txn_memtable_table_next lends the nearest key past a key, or before it
 * going backward, or the first or the last with none, that a storage's rows
 * hold a version of, in the memtable or its spills, with its versions newest
 * first, the spills' placed among the memtable's in commit order (bugs row
 * 397), for a caller's hold; it answers nothing past the last.
 */
bool
txn_memtable_table_next(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, bool has_after,
						const uint8 *after, size_t after_len, TxnMemtableTableDirection way, bool *some,
						txn_TupleVecU8VecVersion_t *result, HostError *error)
{
	txn_memtable_table_next_read_t captures = {io, hold, state, has_after, after, after_len, way, some, result};

	*some = false;
	if (!txn_spill_taken(&TXN_SPILL_STATE_HOME_OPS, io, state, txn_memtable_table_next_read,
						 txn_memtable_table_next_dropped, &captures, error))
		return false;
	if (!*some)
		return true;
	if (!txn_spill_ordered(io, &result->f1, &captures.spilled, error))
	{
		txn_memtable_table_next_dropped(&captures);
		return false;
	}
	std_vec_free_Checked(&captures.spilled, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	return true;
}

/* The prepare whose closure returns nothing, which every crate that takes it declares and table.c defines. */
#define TXN_MEMTABLE_TABLE_RS Unit
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#define TXN_MEMTABLE_TABLE_DEFINE
#define TXN_MEMTABLE_TABLE_RESULT_FIELDS
#define TXN_MEMTABLE_TABLE_RESULT_INIT
#define TXN_MEMTABLE_TABLE_RESULT_ARGS(c)
#define TXN_MEMTABLE_TABLE_RESULT_ERROR error
#include "doltgres/txn/memtable/table_template.h"

/* The prepare whose closure pushes a version and returns its handle, which table.c defines where Handle lives. */
#define TXN_MEMTABLE_TABLE_RS Handle
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , TxnMemtableVersionHandle *result, HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#define TXN_MEMTABLE_TABLE_DEFINE
#define TXN_MEMTABLE_TABLE_RESULT_FIELDS TxnMemtableVersionHandle *result;
#define TXN_MEMTABLE_TABLE_RESULT_INIT , result
#define TXN_MEMTABLE_TABLE_RESULT_ARGS(c) , (c)->result
#define TXN_MEMTABLE_TABLE_RESULT_ERROR error
#include "doltgres/txn/memtable/table_template.h"

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/chunks/trailer.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/test.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/testing.h"
#include "doltgres/txn/written.h"

/* txn_memtable_table_tests_written_frame_0_t is written's state across its pad: its arguments, and its put, while live. */
typedef struct txn_memtable_table_tests_written_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	StdBytes	key;
	TxnMemtableVersionHandle *result;
	HostError  *error;
	bool		ok;
	TxnMemtableOp op;
	bool		op_live;
} txn_memtable_table_tests_written_frame_0_t;

/* txn_memtable_table_tests_written_pad_0 is written's pad: its put, while live. */
static void
txn_memtable_table_tests_written_pad_0(void *frame_)
{
	txn_memtable_table_tests_written_frame_0_t *frame = frame_;

	if (frame->op_live)
	{
		frame->op_live = false;
		txn_memtable_Op_drop(&frame->op);
	}
}

/* txn_memtable_table_tests_written_body_0 is written's body. */
static void
txn_memtable_table_tests_written_body_0(void *frame_)
{
	txn_memtable_table_tests_written_frame_0_t *frame = frame_;
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7),
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};

	frame->op = txn_memtable_testing_put((const uint8 *) "v", 1);
	frame->op_live = true;
	change.op = &frame->op;
	frame->ok = txn_memtable_table_push(frame->io, frame->state, frame->key.ptr, frame->key.len, &change,
										frame->result, frame->error);
	txn_memtable_table_tests_written_pad_0(frame);
}

/* txn_memtable_table_tests_written pushes a version that puts a value for a key, as a top-level transaction's. */
static bool
txn_memtable_table_tests_written(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len,
								 TxnMemtableVersionHandle *result, HostError *error)
{
	txn_memtable_table_tests_written_frame_0_t frame = {.io = io, .state = state, .result = result, .error = error};

	frame.key.ptr = key;
	frame.key.len = key_len;
	std_landing(&frame, txn_memtable_table_tests_written_body_0, txn_memtable_table_tests_written_pad_0);
	return frame.ok;
}

/* txn_memtable_table_tests_writes pushes a key's version, as written's unwrap does at its place. */
static TxnMemtableVersionHandle
txn_memtable_table_tests_writes(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state, const uint8 *key,
								size_t key_len)
{
	TxnMemtableVersionHandle handle;
	HostError	error;

	if (!txn_memtable_table_tests_written(io, state, key, key_len, &handle, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return handle;
}

/* txn_memtable_table_tests_holder_frame_0_t is holder's state across its pad: the hold and the guard, while live. */
typedef struct txn_memtable_table_tests_holder_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	StdBytes	key;
	TxnMemtableTableAt *at;
	bool	   *holds;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedGuard guard;
	bool		guard_live;
} txn_memtable_table_tests_holder_frame_0_t;

/* txn_memtable_table_tests_holder_pad_0 is holder's pad: the guard, then the hold, while live. */
static void
txn_memtable_table_tests_holder_pad_0(void *frame_)
{
	txn_memtable_table_tests_holder_frame_0_t *frame = frame_;

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
}

/* txn_memtable_table_tests_holder_body_0 is holder's body. */
static void
txn_memtable_table_tests_holder_body_0(void *frame_)
{
	txn_memtable_table_tests_holder_frame_0_t *frame = frame_;
	HostSharedRecord record;
	TxnMemtableIndexNode node;
	bool		some;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_table_found(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, &frame->hold, frame->state,
								  frame->key.ptr, frame->key.len, NULL, &some, frame->at, frame->error))
	{
		txn_memtable_table_tests_holder_pad_0(frame);
		return;
	}
	if (!some)
	{
		*frame->error = txn_memtable_testing_failed("a storage has no memtable");
		txn_memtable_table_tests_holder_pad_0(frame);
		return;
	}
	if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, frame->at->range, &record,
								frame->error) ||
		!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_RANGE_LOCK,
										&frame->guard, frame->error))
	{
		txn_memtable_table_tests_holder_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	frame->ok = txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &frame->guard, &frame->hold,
										frame->key.ptr, frame->key.len, frame->holds, &node, frame->error);
	txn_memtable_table_tests_holder_pad_0(frame);
}

/* txn_memtable_table_tests_holder returns where the hand-off finds a key and whether that range's index holds it. */
static bool
txn_memtable_table_tests_holder(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len,
								TxnMemtableTableAt *at, bool *holds, HostError *error)
{
	txn_memtable_table_tests_holder_frame_0_t frame = {.io = io, .state = state, .at = at, .holds = holds,
	.error = error};

	frame.key.ptr = key;
	frame.key.len = key_len;
	std_landing(&frame, txn_memtable_table_tests_holder_body_0, txn_memtable_table_tests_holder_pad_0);
	return frame.ok;
}

/* txn_memtable_table_tests_held_at returns where the hand-off finds a key and whether its range holds it, as holder's unwrap does at its place. */
static TxnMemtableTableAt
txn_memtable_table_tests_held_at(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state, const uint8 *key,
								 size_t key_len, bool *holds)
{
	TxnMemtableTableAt at;
	HostError	error;

	if (!txn_memtable_table_tests_holder(io, state, key, key_len, &at, holds, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return at;
}

/* txn_memtable_table_tests_firsts_frame_0_t is firsts' state across its pad: the hold, and the firsts read, while live. */
typedef struct txn_memtable_table_tests_firsts_frame_0_t
{
	HostIoIo   *io;
	const uint64 *ranges;
	size_t		ranges_len;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	bool		result_live;
} txn_memtable_table_tests_firsts_frame_0_t;

/* txn_memtable_table_tests_firsts_free frees a Vec<(u64, Vec<u8>)>. */
static void
txn_memtable_table_tests_firsts_free(StdVec *firsts)
{
	for (size_t k = 0; k < firsts->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(firsts, txn_TupleU64VecU8_t, k).f1, 1, 1);
	std_vec_free_Checked(firsts, sizeof(txn_TupleU64VecU8_t), _Alignof(txn_TupleU64VecU8_t));
}

/* txn_memtable_table_tests_firsts_pad_0 is firsts' pad: the firsts read, then the hold, while live. */
static void
txn_memtable_table_tests_firsts_pad_0(void *frame_)
{
	txn_memtable_table_tests_firsts_frame_0_t *frame = frame_;

	if (frame->result_live)
	{
		frame->result_live = false;
		txn_memtable_table_tests_firsts_free(frame->result);
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
}

/* txn_memtable_table_tests_firsts_body_0 is firsts' body. */
static void
txn_memtable_table_tests_firsts_body_0(void *frame_)
{
	txn_memtable_table_tests_firsts_frame_0_t *frame = frame_;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	std_vec_with_capacity_Checked(frame->result, frame->ranges_len, sizeof(txn_TupleU64VecU8_t),
								  _Alignof(txn_TupleU64VecU8_t));
	frame->result_live = true;
	for (size_t k = 0; k < frame->ranges_len; k++)
	{
		HostSharedRecord record;
		HostDeltaLent key;
		StdBytes	bytes;
		txn_TupleU64VecU8_t entry;

		if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, frame->ranges[k], &record,
									frame->error))
		{
			txn_memtable_table_tests_firsts_pad_0(frame);
			return;
		}
		entry.f0 = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_FIRST, load);
		if (!txn_memtable_table_first_key(&frame->hold, frame->ranges[k], &key, frame->error))
		{
			txn_memtable_table_tests_firsts_pad_0(frame);
			return;
		}
		bytes = host_delta_Lent_Deref_deref(&key);
		std_slice_to_vec_Checked(&entry.f1, bytes.ptr, bytes.len, 1, 1);
		host_delta_Lent_drop(&key);
		STD_VEC_AT(frame->result, txn_TupleU64VecU8_t, k) = entry;
		frame->result->len = k + 1;
	}
	frame->result_live = false;
	frame->ok = true;
	txn_memtable_table_tests_firsts_pad_0(frame);
}

/* txn_memtable_table_tests_firsts returns each range's First and first key, a Vec<(u64, Vec<u8>)>. */
static bool
txn_memtable_table_tests_firsts(HostIoIo *io, const uint64 *ranges, size_t ranges_len, StdVec *result,
								HostError *error)
{
	txn_memtable_table_tests_firsts_frame_0_t frame = {.io = io, .ranges = ranges, .ranges_len = ranges_len,
	.result = result, .error = error};

	std_landing(&frame, txn_memtable_table_tests_firsts_body_0, txn_memtable_table_tests_firsts_pad_0);
	return frame.ok;
}

/* txn_memtable_table_tests_first_Debug_fmt is a range's (u64, Vec<u8>)'s Debug. */
static bool
txn_memtable_table_tests_first_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_TupleU64VecU8_t *first = value;
	StdBytes	key = {first->f1.ptr, first->f1.len};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&first->f0, std_u64_Debug_fmt), STD_ITEM(&key, std_bytes_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_memtable_table_tests_firsts_Debug_fmt is a Vec<(u64, Vec<u8>)>'s Debug. */
static bool
txn_memtable_table_tests_firsts_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *firsts = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < firsts->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(firsts, txn_TupleU64VecU8_t, k), txn_memtable_table_tests_first_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_memtable_table_tests_firsts_eq reports whether two Vec<(u64, Vec<u8>)> are equal. */
static bool
txn_memtable_table_tests_firsts_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t k = 0; k < a->len; k++)
	{
		const txn_TupleU64VecU8_t *x = &STD_VEC_AT(a, txn_TupleU64VecU8_t, k);
		const txn_TupleU64VecU8_t *y = &STD_VEC_AT(b, txn_TupleU64VecU8_t, k);

		if (x->f0 != y->f0 || x->f1.len != y->f1.len ||
			(x->f1.len > 0 && memcmp(x->f1.ptr, y->f1.ptr, x->f1.len) != 0))
			return false;
	}
	return true;
}

/* txn_memtable_table_tests_none_next is std::iter::Empty's next: no item. */
static bool
txn_memtable_table_tests_none_next(void *self, void *result)
{
	return false;
}

/* txn_memtable_table_tests_none_size_hint is std::iter::Empty's size_hint: no item left. */
static StdSizeHint
txn_memtable_table_tests_none_size_hint(const void *self)
{
	StdSizeHint hint = {.has_upper = true};

	return hint;
}

/* TXN_MEMTABLE_TABLE_TESTS_NONE_ITERATOR_OPS is std::iter::Empty's Iterator, of the pages a retirement of none takes. */
static const StdIteratorOps TXN_MEMTABLE_TABLE_TESTS_NONE_ITERATOR_OPS = {
	.next = txn_memtable_table_tests_none_next,
	.size_hint = txn_memtable_table_tests_none_size_hint,
};

/* txn_memtable_table_tests_passed runs the delta's free pass, which frees what was retired before every hold now open. */
static void
txn_memtable_table_tests_passed(HostIoIo *io)
{
	StdIterator none = {&TXN_MEMTABLE_TABLE_TESTS_NONE_ITERATOR_OPS};
	HostError	error;

	if (!host_delta_retire(io, NULL, 0, none, &error))
		STD_RESULT_EXPECT_FAILED(1966, 52, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/* txn_memtable_table_tests_returned asserts that a run's actors all returned with no error. */
static void
txn_memtable_table_tests_returned(const SimcReport *report)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	SimcEnding	returned = {.tag = SIMC_ENDING_RETURNED};
	const StdVec *endings;

	if (report->outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtArg	args[1] = {{&report->outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 1971, 67, &message);
	}
	endings = &report->outcome.u.ended;
	for (size_t k = 0; k < endings->len; k++)
	{
		const simc_TupleStringEnding_t *ending = &STD_VEC_AT(endings, simc_TupleStringEnding_t, k);

		if (!simc_Ending_PartialEq_eq(&ending->f1, &returned))
		{
			StdFmtArg	args[1] = {{&ending->f0, std_String_Display_fmt}};
			StdFmtArguments message = {pieces, lengthof(pieces), args};

			std_assert_failed(RUST_FILE, 1973, 13, "==", &ending->f1, simc_Ending_Debug_fmt, &returned,
							  simc_Ending_Debug_fmt, &message);
		}
	}
}

/* txn_memtable_table_tests_Node_Debug_fmt is Node's derived Debug, Node(word), for the tests' asserts. */
static bool
txn_memtable_table_tests_Node_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableIndexNode *node = value;
	StdDebugTuple tuple = std_fmt_debug_tuple(f, "Node", 4);

	std_DebugTuple_field(&tuple, &node->f0, std_u64_Debug_fmt);
	return std_DebugTuple_finish(&tuple);
}

/* txn_memtable_table_tests_At_Debug_fmt is At's derived Debug, At { memtable, range }, for the tests' asserts. */
static bool
txn_memtable_table_tests_At_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableTableAt *at = value;
	StdDebugStruct fields = std_fmt_debug_struct(f, "At", 2);

	std_DebugStruct_field(&fields, "memtable", 8, &at->memtable, std_u64_Debug_fmt);
	std_DebugStruct_field(&fields, "range", 5, &at->range, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&fields);
}

/* txn_memtable_table_tests_has_memtable answers whether a storage's rows have a memtable, as memtable::<Rows> answers its Option. */
static bool
txn_memtable_table_tests_has_memtable(HostSharedRecord state, uint64 *location)
{
	return txn_memtable_table_memtable(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, state, location);
}

/* txn_memtable_table_tests_version_handle reads each version a chain lends to its handle, as the chain's map to each version's unwrap's handle does at its place. */
static void
txn_memtable_table_tests_version_handle(uint32 line, uint32 col, TxnMemtableVersionChain *chain, StdVec *result)
{
	TxnMemtableVersionVersion version;
	HostError	error;
	bool		some;

	std_vec_new(result, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	for (;;)
	{
		if (!txn_memtable_version_Chain_Iterator_next(chain, &some, &version, &error))
			STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!some)
			return;
		std_vec_push_Checked(result, &version.handle, sizeof(version.handle), _Alignof(TxnMemtableVersionHandle));
		txn_memtable_version_Version_drop(&version);
	}
}

/* txn_memtable_table_tests_handles_Debug_fmt is a Vec<Handle>'s Debug. */
static bool
txn_memtable_table_tests_handles_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *handles = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < handles->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(handles, TxnMemtableVersionHandle, k),
							txn_memtable_version_Handle_Debug_fmt);
	return std_DebugList_finish(&list);
}

/*
 * 2d's case: a storage's first writer makes its memtable, whose one range's
 * first key is the empty key on the first page its RangeStart names, the
 * delta's first page held elsewhere, whose node names no version, and the
 * writer's version is its key's newest; the range counts both keys and their
 * one byte.
 */
void
test_txn_memtable_table_tests_a_first_writer_makes_a_memtable_whose_first_key_is_empty(void)
{
	const TxnMemtableIndexKeyedOps *keyed = &TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS;
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9301);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostDeltaPageSize list_page = HOST_DELTA_LIST_PAGE;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	StdOptionRef memtable_none;
	StdOptionRef none = {false};
	StdVec		taken;
	HostDeltaFull full;
	HostError	error;
	TxnMemtableVersionHandle handle;
	TxnMemtableTableAt at;
	HostDeltaHold hold;
	HostSharedRecord memtable;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostDeltaListListView list;
	HostDeltaLent first;
	StdBytes	first_bytes;
	StdBytes	empty_key = {(const uint8 *) "", 0};
	TxnMemtableIndexNode empty;
	TxnMemtableIndexNode node;
	TxnMemtableIndexNode at_first;
	TxnMemtableVersionChain chain;
	TxnMemtableVersionVersion version;
	StdOptionRef left_node;
	StdOptionRef right_node;
	StdVec		newest;
	StdVec		wanted;
	StdTupleRef left_counts;
	StdTupleRef right_counts;
	uint64		location;
	uint64		ranges;
	uint64		one = 1;
	uint64		start;
	uint64		page;
	uint64		counts[2];
	uint64		wanted_counts[2] = {2, 1};
	uint64		head;
	uint64		no_version = TXN_MEMTABLE_INDEX_NONE;
	uint64		versions = 0;
	uint64		zero = 0;
	bool		ok;
	bool		some;
	bool		holds;
	bool		has_first;

	memtable_none.has = txn_memtable_table_tests_has_memtable(state, &location);
	memtable_none.some = STD_ITEM(&location, std_u64_Debug_fmt);
	STD_ASSERT_EQ(1986, 9, !memtable_none.has, &memtable_none, std_option_Debug_fmt, &none,
				  std_option_Debug_fmt);
	if (!host_delta_alloc(io, &list_page, 1, &ok, &taken, &full, &error))
		STD_RESULT_EXPECT_FAILED(1987, 53, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!ok)
	{
		error = txn_memtable_testing_failed("the delta refused a page");
		STD_RESULT_EXPECT_FAILED(1987, 110, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	}
	handle = txn_memtable_table_tests_writes(1988, 55, io, state, (const uint8 *) "k", 1);
	std_vec_drop_Checked(&taken, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
	STD_ASSERT(1990, 9, txn_memtable_table_tests_has_memtable(state, &location),
			   "memtable::<Rows>(held.record()).is_some()");
	at = txn_memtable_table_tests_held_at(1991, 59, io, state, (const uint8 *) "k", 1, &holds);
	STD_ASSERT(1992, 9, holds, "holds");
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(1993, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, at.memtable, &memtable, &error))
		STD_RESULT_EXPECT_FAILED(1994, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(memtable, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_MEMTABLE_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(1995, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	list = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape, TXN_MEMTABLE_RANGE_RANGES);
	ranges = host_delta_list_ListView_len(&list);
	host_shared_Guard_drop(&guard);
	STD_ASSERT_EQ(1995, 9, ranges == 1, &ranges, std_u64_Debug_fmt, &one, std_u64_Debug_fmt);
	if (!txn_memtable_table_first_key(&hold, at.range, &first, &error))
		STD_RESULT_EXPECT_FAILED(1996, 49, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	first_bytes = host_delta_Lent_Deref_deref(&first);
	STD_ASSERT_EQ(1996, 9, first_bytes.len == 0, &first_bytes, std_bytes_Debug_fmt, &empty_key,
				  std_bytes_Debug_fmt);
	host_delta_Lent_drop(&first);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at.range, &record, &error))
		STD_RESULT_EXPECT_FAILED(1997, 53, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(1998, 57, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	start = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_START, load);
	list = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES);
	if (!host_delta_list_ListView_get(&list, &hold, 0, &page, &error))
		STD_RESULT_EXPECT_FAILED(1999, 95, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(1999, 9, start == page, &start, std_u64_Debug_fmt, &page, std_u64_Debug_fmt);
	counts[0] = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_KEYS, load);
	counts[1] = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_BYTES, load);
	left_counts = STD_TUPLE(2, STD_ITEM(&counts[0], std_u64_Debug_fmt), STD_ITEM(&counts[1], std_u64_Debug_fmt));
	right_counts = STD_TUPLE(2, STD_ITEM(&wanted_counts[0], std_u64_Debug_fmt),
							 STD_ITEM(&wanted_counts[1], std_u64_Debug_fmt));
	STD_ASSERT_EQ(2000, 9, counts[0] == 2 && counts[1] == 1, &left_counts, std_tuple_Debug_fmt,
				  &right_counts, std_tuple_Debug_fmt);
	if (!txn_memtable_index_find(keyed, &guard, &hold, (const uint8 *) "", 0, &some, &empty, &error))
		STD_RESULT_EXPECT_FAILED(2001, 53, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!some)
		STD_OPTION_UNWRAP_FAILED(2001, 62);
	has_first = txn_memtable_index_first(keyed, &guard, &at_first);
	left_node.has = true;
	left_node.some = STD_ITEM(&empty, txn_memtable_table_tests_Node_Debug_fmt);
	right_node.has = has_first;
	right_node.some = STD_ITEM(&at_first, txn_memtable_table_tests_Node_Debug_fmt);
	STD_ASSERT_EQ(2002, 9, has_first && at_first.f0 == empty.f0, &left_node, std_option_Debug_fmt,
				  &right_node, std_option_Debug_fmt);
	if (!txn_memtable_index_head(keyed, &guard, &hold, empty, &head, &error))
		STD_RESULT_EXPECT_FAILED(2003, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(2003, 9, head == no_version, &head, std_u64_Debug_fmt, &no_version, std_u64_Debug_fmt);
	if (!txn_memtable_version_chain(keyed, &guard, &hold, empty, &chain, &error))
		STD_RESULT_EXPECT_FAILED(2004, 48, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (;;)
	{
		if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, &error))
		{
			versions++;
			host_Error_drop(&error);
			break;
		}
		if (!some)
			break;
		versions++;
		txn_memtable_version_Version_drop(&version);
	}
	STD_ASSERT_EQ(2004, 9, versions == 0, &versions, std_usize_Debug_fmt, &zero, std_usize_Debug_fmt);
	if (!txn_memtable_index_find(keyed, &guard, &hold, (const uint8 *) "k", 1, &some, &node, &error))
		STD_RESULT_EXPECT_FAILED(2005, 53, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!some)
		STD_OPTION_UNWRAP_FAILED(2005, 62);
	if (!txn_memtable_version_chain(keyed, &guard, &hold, node, &chain, &error))
		STD_RESULT_EXPECT_FAILED(2006, 57, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_version_handle(2006, 88, &chain, &newest);
	wanted.ptr = &handle;
	wanted.len = 1;
	wanted.cap = 1;
	STD_ASSERT_EQ(2007, 9, newest.len == 1 && STD_VEC_AT(&newest, TxnMemtableVersionHandle, 0).f0 == handle.f0,
				  &newest, txn_memtable_table_tests_handles_Debug_fmt, &wanted, txn_memtable_table_tests_handles_Debug_fmt);
	std_vec_free_Checked(&newest, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_first_writer is the two first writers' case's body: its key's push in the storage, the key its captures hold. */
static bool
txn_memtable_table_tests_first_writer(void *captures, HostIoIo *io, HostError *error)
{
	const char *key = captures;
	HostSharedHeld held;
	TxnMemtableVersionHandle handle;
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9302, &held, error))
		return false;
	ok = txn_memtable_table_tests_written(io, host_shared_Held_record(&held), (const uint8 *) key, 1, &handle, error);
	host_shared_Held_drop(&held);
	return ok;
}

/*
 * PLAN's case (b): two first writers held before they take the storage
 * state's lock make one memtable with both versions in it, and the loser's
 * page and records are given back at its drops, so the delta's use returns to
 * its start once the memtable is retired.
 */
void
test_txn_memtable_table_tests_two_first_writers_make_one_memtable(void)
{
	static const char *const names[2] = {"first", "second"};
	static const char *const keys[2] = {"a", "b"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	uint64		used;
	HostSharedHeld held = txn_memtable_testing_created(io, 9302);
	HostStorage storage = txn_memtable_testing_storage(9302);
	SimcRun		run = simc_Run_new(83);
	SimcStep	steps[2] = {{"second", 6, PG_UINT64_MAX}, {"first", 5, PG_UINT64_MAX}};
	SimcReport	report;
	TxnMemtableTableAt a;
	TxnMemtableTableAt b;
	HostError	error;
	bool		holds_a;
	bool		holds_b;

	for (size_t k = 0; k < lengthof(names); k++)
	{
		SimcActor	actor = {txn_memtable_table_tests_first_writer, (void *) keys[k]};

		simc_Run_actor(&run, names[k], strlen(names[k]), actor);
	}
	simc_Run_until(&run, "first", 5, "doltgres-memtable-first");
	simc_Run_until(&run, "second", 6, "doltgres-memtable-first");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	a = txn_memtable_table_tests_held_at(2032, 60, io, host_shared_Held_record(&held), (const uint8 *) "a", 1,
										 &holds_a);
	b = txn_memtable_table_tests_held_at(2033, 60, io, host_shared_Held_record(&held), (const uint8 *) "b", 1,
										 &holds_b);
	STD_ASSERT(2034, 9, holds_a && holds_b, "holds_a && holds_b");
	STD_ASSERT_EQ(2035, 9, a.memtable == b.memtable && a.range == b.range, &a,
				  txn_memtable_table_tests_At_Debug_fmt, &b, txn_memtable_table_tests_At_Debug_fmt);
	host_shared_Held_drop(&held);
	if (!txn_state_unlinked(io, &storage, &error))
		STD_RESULT_EXPECT_FAILED(2037, 38, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_passed(io);
	used = host_delta_used();
	STD_ASSERT_EQ(2039, 9, used == start, &used, std_u64_Debug_fmt, &start, std_u64_Debug_fmt);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_closed_frame_0_t is closed's state across its pad: the hold and the guard, while live. */
typedef struct txn_memtable_table_tests_closed_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	TxnMemtableTableAt *result;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedGuard guard;
	bool		guard_live;
} txn_memtable_table_tests_closed_frame_0_t;

/* txn_memtable_table_tests_closed_pad_0 is closed's pad: the guard, then the hold, while live. */
static void
txn_memtable_table_tests_closed_pad_0(void *frame_)
{
	txn_memtable_table_tests_closed_frame_0_t *frame = frame_;

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
}

/* txn_memtable_table_tests_closed_body_0 is closed's body. */
static void
txn_memtable_table_tests_closed_body_0(void *frame_)
{
	txn_memtable_table_tests_closed_frame_0_t *frame = frame_;
	HostSharedRecord record;
	TxnMemtableIndexOpen open;
	bool		some;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_table_found(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, &frame->hold, frame->state,
								  (const uint8 *) "", 0, NULL, &some, frame->result, frame->error))
	{
		txn_memtable_table_tests_closed_pad_0(frame);
		return;
	}
	if (!some)
	{
		*frame->error = txn_memtable_testing_failed("a storage has no memtable");
		txn_memtable_table_tests_closed_pad_0(frame);
		return;
	}
	if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, frame->result->range, &record,
								frame->error) ||
		!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_RANGE_LOCK,
								 &frame->guard, frame->error))
	{
		txn_memtable_table_tests_closed_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	if (!txn_memtable_index_open(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS.closes, &frame->guard, &open))
	{
		*frame->error = txn_memtable_testing_failed("a new range is closed");
		txn_memtable_table_tests_closed_pad_0(frame);
		return;
	}
	txn_memtable_index_Open_close(open);
	frame->ok = true;
	txn_memtable_table_tests_closed_pad_0(frame);
}

/* txn_memtable_table_tests_closed closes a storage's first range while it holds no version, as an emptied first range that is its memtable's last entry is closed. */
static bool
txn_memtable_table_tests_closed(HostIoIo *io, HostSharedRecord state, TxnMemtableTableAt *result, HostError *error)
{
	txn_memtable_table_tests_closed_frame_0_t frame = {.io = io, .state = state, .result = result, .error = error};

	std_landing(&frame, txn_memtable_table_tests_closed_body_0, txn_memtable_table_tests_closed_pad_0);
	return frame.ok;
}

/* txn_memtable_table_tests_firsts_of returns each range's First and first key, as firsts' unwrap does at its place. */
static void
txn_memtable_table_tests_firsts_of(uint32 line, uint32 col, HostIoIo *io, const StdVec *ranges, StdVec *result)
{
	HostError	error;

	if (!txn_memtable_table_tests_firsts(io, ranges->ptr, ranges->len, result, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/* txn_memtable_table_tests_search_case_t is a search case's key and its range's index among the ranges. */
typedef struct txn_memtable_table_tests_search_case_t
{
	const char *key;
	size_t		key_len;
	size_t		range;
} txn_memtable_table_tests_search_case_t;

/*
 * W1's case: the memtable's search finds the range for keys before, at,
 * between, and after each range's first key, and every range's First and
 * first key stay as they were after each push.
 */
void
test_txn_memtable_table_tests_a_ranges_search_finds_the_range_for_every_key(void)
{
	static const txn_memtable_table_tests_search_case_t cases[10] = {
		{"", 0, 0}, {"a", 1, 0}, {"f\xff", 2, 0}, {"g", 1, 1}, {"g\x00", 2, 1}, {"o", 1, 1}, {"p", 1, 2},
		{"p\x00", 2, 2}, {"pa", 2, 3}, {"z", 1, 3},
	};
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9303);
	HostSharedRecord state = host_shared_Held_record(&held);
	StdBytes	firsts[3] = {{(const uint8 *) "g", 1}, {(const uint8 *) "p", 1}, {(const uint8 *) "pa", 2}};
	StdVec		ranges;
	StdVec		before;
	HostError	error;

	if (!txn_memtable_testing_ranged(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, firsts, 3, &ranges, &error))
		STD_RESULT_EXPECT_FAILED(2050, 78, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_firsts_of(2051, 42, io, &ranges, &before);
	for (size_t k = 0; k < lengthof(cases); k++)
	{
		StdBytes	key = {(const uint8 *) cases[k].key, cases[k].key_len};
		StdFmtArg	args[1] = {{&key, std_bytes_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};
		TxnMemtableTableAt at;
		bool		holds;
		bool		yes = true;
		uint64		range = STD_VEC_AT(&ranges, uint64, cases[k].range);
		StdTupleRef left;
		StdTupleRef right;
		StdVec		after;

		(void) txn_memtable_table_tests_writes(2065, 45, io, state, key.ptr, key.len);
		at = txn_memtable_table_tests_held_at(2066, 62, io, state, key.ptr, key.len, &holds);
		left = STD_TUPLE(2, STD_ITEM(&at.range, std_u64_Debug_fmt), STD_ITEM(&holds, std_bool_Debug_fmt));
		right = STD_TUPLE(2, STD_ITEM(&range, std_u64_Debug_fmt), STD_ITEM(&yes, std_bool_Debug_fmt));
		if (!(at.range == range && holds))
			std_assert_failed(RUST_FILE, 2067, 13, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt,
							  &message);
		txn_memtable_table_tests_firsts_of(2068, 44, io, &ranges, &after);
		if (!txn_memtable_table_tests_firsts_eq(&after, &before))
			std_assert_failed(RUST_FILE, 2068, 13, "==", &after, txn_memtable_table_tests_firsts_Debug_fmt,
							  &before, txn_memtable_table_tests_firsts_Debug_fmt, &message);
		txn_memtable_table_tests_firsts_free(&after);
	}
	txn_memtable_table_tests_firsts_free(&before);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_used_is retires none and asserts the delta's use is back at its start, as passed and assert_eq do at their places. */
static void
txn_memtable_table_tests_used_is(uint32 line, uint32 col, HostIoIo *io, uint64 start)
{
	uint64		used;

	txn_memtable_table_tests_passed(io);
	used = host_delta_used();
	STD_ASSERT_EQ(line, col, used == start, &used, std_u64_Debug_fmt, &start, std_u64_Debug_fmt);
}

/* txn_memtable_table_tests_unlinks unlinks a database's storage, as unlinked's unwrap does at its place. */
static void
txn_memtable_table_tests_unlinks(uint32 line, uint32 col, HostIoIo *io, uint32 database)
{
	HostStorage storage = txn_memtable_testing_storage(database);
	HostError	error;

	if (!txn_state_unlinked(io, &storage, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/* txn_memtable_table_tests_firsted makes a storage's rows' first memtable, as first's unwrap does at its place. */
static void
txn_memtable_table_tests_firsted(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state)
{
	HostError	error;

	if (!txn_memtable_table_first(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/*
 * PLAN's case: a writer that finds its range closed takes it out of the
 * memtable's list, and with its last entry gone the memtable is closed, the
 * storage's word cleared, and both retired; the writer then starts the
 * storage's next memtable as its first writer.
 */
void
test_txn_memtable_table_tests_a_closed_range_found_is_taken_out_and_the_writer_starts_over(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9304);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableTableAt old;
	TxnMemtableTableAt at;
	HostDeltaHold hold;
	HostSharedRecord record;
	HostError	error;
	bool		holds;
	bool		reached;

	txn_memtable_table_tests_firsted(2082, 42, io, state);
	if (!txn_memtable_table_tests_closed(io, state, &old, &error))
		STD_RESULT_EXPECT_FAILED(2083, 45, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	(void) txn_memtable_table_tests_writes(2084, 42, io, state, (const uint8 *) "k", 1);
	at = txn_memtable_table_tests_held_at(2085, 59, io, state, (const uint8 *) "k", 1, &holds);
	STD_ASSERT(2086, 9, holds, "holds");
	if (at.memtable == old.memtable)
		std_assert_failed(RUST_FILE, 2087, 9, "!=", &at.memtable, std_u64_Debug_fmt, &old.memtable,
						  std_u64_Debug_fmt, NULL);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2088, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	reached = host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, old.memtable, &record, &error);
	if (!reached)
		host_Error_drop(&error);
	STD_ASSERT(2089, 9, !reached, "hold.record::<Memtable>(old.memtable).is_err()");
	reached = host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, old.range, &record, &error);
	if (!reached)
		host_Error_drop(&error);
	STD_ASSERT(2090, 9, !reached, "hold.record::<Range>(old.range).is_err()");
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(2092, 38, io, 9304);
	txn_memtable_table_tests_used_is(2094, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_retired_writer is the retired range case's writer: its push of "k". */
static bool
txn_memtable_table_tests_retired_writer(void *captures, HostIoIo *io, HostError *error)
{
	HostSharedHeld held;
	TxnMemtableVersionHandle handle;
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9305, &held, error))
		return false;
	ok = txn_memtable_table_tests_written(io, host_shared_Held_record(&held), (const uint8 *) "k", 1, &handle, error);
	host_shared_Held_drop(&held);
	return ok;
}

/* txn_memtable_table_tests_closer is the retired range case's closer: it closes the first range and takes it out. */
static bool
txn_memtable_table_tests_closer(void *captures, HostIoIo *io, HostError *error)
{
	HostSharedHeld held;
	TxnMemtableTableAt at;
	HostDeltaHold hold;
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9305, &held, error))
		return false;
	ok = txn_memtable_table_tests_closed(io, host_shared_Held_record(&held), &at, error) &&
		host_delta_hold(io, &hold, error);
	if (ok)
	{
		ok = txn_memtable_table_unlisted(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, &hold, host_shared_Held_record(&held),
										 at, error);
		host_delta_Hold_drop(&hold);
	}
	host_shared_Held_drop(&held);
	return ok;
}

/*
 * PLAN's case (a): a writer held between its open of a range and its lock,
 * while the range, emptied, is closed and taken out and the memtable retired
 * with it, puts its version in the storage's next memtable, and the delta's
 * use returns to its start once that one is retired.
 */
void
test_txn_memtable_table_tests_a_writer_that_reaches_a_retired_range_writes_to_the_next_memtable(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9305);
	HostSharedRecord state = host_shared_Held_record(&held);
	SimcRun		run;
	SimcStep	steps[2] = {{"closer", 6, PG_UINT64_MAX}, {"writer", 6, PG_UINT64_MAX}};
	SimcActor	writer = {txn_memtable_table_tests_retired_writer};
	SimcActor	closer = {txn_memtable_table_tests_closer};
	SimcReport	report;
	TxnMemtableTableAt at;
	uint64		old;
	bool		has_old;
	bool		holds;
	StdOptionRef left;
	StdOptionRef right;

	txn_memtable_table_tests_firsted(2107, 42, io, state);
	has_old = txn_memtable_table_tests_has_memtable(state, &old);
	run = simc_Run_new(89);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_actor(&run, "closer", 6, closer);
	simc_Run_until(&run, "writer", 6, "doltgres-memtable-found");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	at = txn_memtable_table_tests_held_at(2124, 59, io, state, (const uint8 *) "k", 1, &holds);
	STD_ASSERT(2125, 9, holds, "holds");
	left.has = true;
	left.some = STD_ITEM(&at.memtable, std_u64_Debug_fmt);
	right.has = has_old;
	right.some = STD_ITEM(&old, std_u64_Debug_fmt);
	if (has_old && at.memtable == old)
		std_assert_failed(RUST_FILE, 2126, 9, "!=", &left, std_option_Debug_fmt, &right, std_option_Debug_fmt,
						  NULL);
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(2128, 38, io, 9305);
	txn_memtable_table_tests_used_is(2130, 9, io, start);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/*
 * The memtable's clause of a state's reclaim: an idle, clean state whose
 * memtable word names a memtable is kept, and found again with its memtable.
 */
void
test_txn_memtable_table_tests_a_reclaim_keeps_a_state_with_a_memtable(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9307);
	HostError	error;
	uint64		location;
	uint64		again;
	bool		has_location;
	bool		has_again;
	bool		freed;
	bool		holds;
	StdOptionRef left;
	StdOptionRef right;

	(void) txn_memtable_table_tests_writes(2189, 42, io, host_shared_Held_record(&held), (const uint8 *) "k", 1);
	has_location = txn_memtable_table_tests_has_memtable(host_shared_Held_record(&held), &location);
	host_shared_Held_drop(&held);
	if (!TXN_STATE_STATE_POOL_OPS.reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(2192, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2192, 9, !freed, "!State::reclaim(io).unwrap()");
	if (!txn_memtable_testing_stated(io, 9307, &held, &error))
		STD_RESULT_EXPECT_FAILED(2193, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	has_again = txn_memtable_table_tests_has_memtable(host_shared_Held_record(&held), &again);
	left.has = has_again;
	left.some = STD_ITEM(&again, std_u64_Debug_fmt);
	right.has = has_location;
	right.some = STD_ITEM(&location, std_u64_Debug_fmt);
	STD_ASSERT_EQ(2194, 9, has_again == has_location && (!has_again || again == location), &left,
				  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	(void) txn_memtable_table_tests_held_at(2195, 49, io, host_shared_Held_record(&held), (const uint8 *) "k", 1,
											&holds);
	STD_ASSERT(2195, 9, holds, "holder(io, held.record(), b\"k\").unwrap().1");
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * The unlink's case: a storage's unlink retires its memtable, its ranges,
 * and their pages, and the delta's use returns to its start.
 */
void
test_txn_memtable_table_tests_an_unlinked_storage_retires_its_memtable(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9308);
	static uint8 c[8192];
	StdBytes	keys[3] = {{(const uint8 *) "a", 1}, {(const uint8 *) "b", 1}, {c, sizeof(c)}};

	memset(c, 'c', sizeof(c));
	for (size_t k = 0; k < lengthof(keys); k++)
		(void) txn_memtable_table_tests_writes(2208, 45, io, host_shared_Held_record(&held), keys[k].ptr,
											   keys[k].len);
	STD_ASSERT(2210, 9, host_delta_used() > start, "delta::used() > start");
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(2212, 38, io, 9308);
	txn_memtable_table_tests_used_is(2214, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/*
 * CommPF 5's point: a key too long for a range's first page puts its version
 * and node on a page that holds them, the first page doubled until it does,
 * 16 KiB for an 8,192-byte key.
 */
void
test_txn_memtable_table_tests_a_long_keys_version_goes_on_a_page_that_holds_it(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9309);
	static uint8 key[8192];
	TxnMemtableTableAt at;
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostDeltaListListView list;
	HostError	error;
	StdVec		sizes;
	uint64		wanted_sizes[2] = {1 << 10, 16 << 10};
	StdVec		wanted = {wanted_sizes, 2, 2};
	bool		holds;

	memset(key, 'k', sizeof(key));
	(void) txn_memtable_table_tests_writes(2226, 42, io, host_shared_Held_record(&held), key, sizeof(key));
	at = txn_memtable_table_tests_held_at(2227, 59, io, host_shared_Held_record(&held), key, sizeof(key), &holds);
	STD_ASSERT(2228, 9, holds, "holds");
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2229, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at.range, &record, &error))
		STD_RESULT_EXPECT_FAILED(2230, 53, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(2231, 57, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	list = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES);
	std_vec_new(&sizes, sizeof(uint64), _Alignof(uint64));
	for (uint64 index = 0; index < host_delta_list_ListView_len(&list); index++)
	{
		HostDeltaPageRef page;
		uint64		name;
		uint64		size;

		if (!host_delta_list_ListView_get(&list, &hold, index, &name, &error))
			STD_RESULT_EXPECT_FAILED(2234, 71, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!host_delta_Hold_page(&hold, host_delta_page_Page_named(name), &page, &error))
			STD_RESULT_EXPECT_FAILED(2234, 82, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		size = STD_MUL(u64, host_delta_PageRef_words(&page), 32, 2234, 26);
		std_vec_push_Checked(&sizes, &size, sizeof(size), _Alignof(uint64));
	}
	STD_ASSERT_EQ(2236, 9, sizes.len == 2 && STD_VEC_AT(&sizes, uint64, 0) == wanted_sizes[0] &&
				  STD_VEC_AT(&sizes, uint64, 1) == wanted_sizes[1], &sizes, chunks_trailer_VecU64_Debug_fmt,
				  &wanted, chunks_trailer_VecU64_Debug_fmt);
	std_vec_free_Checked(&sizes, sizeof(uint64), _Alignof(uint64));
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_closing_frame_0_t is the terminated closer's state across its pad: the state's handle, the hold, the gathering, and the memtable's guard, while live. */
typedef struct txn_memtable_table_tests_closing_frame_0_t
{
	HostIoIo   *io;
	HostError  *error;
	bool		ok;
	HostSharedHeld held;
	bool		held_live;
	HostDeltaHold hold;
	bool		hold_live;
	TxnMemtableTableGathered gathered;
	bool		gathered_live;
	HostSharedGuard guard;
	bool		guard_live;
} txn_memtable_table_tests_closing_frame_0_t;

/* txn_memtable_table_tests_closing_pad_0 is the terminated closer's pad: the guard, the gathering, the hold, then the state's handle, while live. */
static void
txn_memtable_table_tests_closing_pad_0(void *frame_)
{
	txn_memtable_table_tests_closing_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	if (frame->gathered_live)
	{
		frame->gathered_live = false;
		txn_memtable_table_Gathered_drop(&frame->gathered);
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
}

/* txn_memtable_table_tests_closing_unlisted takes the closed range out of its memtable and retires what the gathering holds, under the frame's hold. */
static bool
txn_memtable_table_tests_closing_unlisted(txn_memtable_table_tests_closing_frame_0_t *frame, TxnMemtableTableAt at)
{
	HostSharedRecord memtable;
	TxnMemtableIndexOpen open;
	bool		unlisted;

	if (!txn_memtable_table_Gathered_range(&frame->gathered, frame->io, &frame->hold, at.range, frame->error) ||
		!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, at.memtable, &memtable,
								frame->error) ||
		!host_shared_Record_lock(memtable, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_MEMTABLE_LOCK,
								 &frame->guard, frame->error))
		return false;
	frame->guard_live = true;
	if (!txn_memtable_index_open(&TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS, &frame->guard, &open))
	{
		*frame->error = txn_memtable_testing_failed("a new memtable is closed");
		return false;
	}
	if (!txn_memtable_table_unlist(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, open, &frame->hold, 0, at.memtable, &unlisted,
								   frame->error))
		return false;
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	return txn_memtable_table_Gathered_retire(&frame->gathered, frame->io, frame->error);
}

/* txn_memtable_table_tests_closing_body_0 is the terminated closer's body. */
static void
txn_memtable_table_tests_closing_body_0(void *frame_)
{
	txn_memtable_table_tests_closing_frame_0_t *frame = frame_;
	TxnMemtableTableAt at;

	if (!txn_memtable_testing_stated(frame->io, 9306, &frame->held, frame->error))
		return;
	frame->held_live = true;
	if (!txn_memtable_table_tests_closed(frame->io, host_shared_Held_record(&frame->held), &at, frame->error) ||
		!host_delta_hold(frame->io, &frame->hold, frame->error))
	{
		txn_memtable_table_tests_closing_pad_0(frame);
		return;
	}
	frame->hold_live = true;
	txn_memtable_table_Gathered_new(&frame->gathered);
	frame->gathered_live = true;
	if (!txn_memtable_table_tests_closing_unlisted(frame, at))
	{
		txn_memtable_table_tests_closing_pad_0(frame);
		return;
	}
	frame->gathered_live = false;
	txn_memtable_table_Gathered_drop(&frame->gathered);
	frame->hold_live = false;
	host_delta_Hold_drop(&frame->hold);
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-memtable-case-closed", frame->error))
	{
		txn_memtable_table_tests_closing_pad_0(frame);
		return;
	}
	for (;;)
		(void) host_unique();
}

/* txn_memtable_table_tests_closing is the terminated closer: it closes and takes out the storage's memtable, then spins until its termination. */
static bool
txn_memtable_table_tests_closing(void *captures, HostIoIo *io, HostError *error)
{
	txn_memtable_table_tests_closing_frame_0_t frame = {.io = io, .error = error};

	std_landing(&frame, txn_memtable_table_tests_closing_body_0, txn_memtable_table_tests_closing_pad_0);
	return frame.ok;
}

/*
 * PLAN's case (c): a process terminated between closing a memtable and
 * clearing the storage's word leaves the clear to the next writer, which
 * retires the memtable and writes, and the delta's use returns to its start
 * once the next memtable is retired.
 */
void
test_txn_memtable_table_tests_a_closer_terminated_before_the_clear_leaves_it_to_the_next_writer(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9306);
	HostSharedRecord state = host_shared_Held_record(&held);
	SimcRun		run;
	SimcActor	closer = {txn_memtable_table_tests_closing};
	SimcReport	report;
	SimcEnding	terminated = {.tag = SIMC_ENDING_TERMINATED};
	const SimcEnding *first;
	TxnMemtableTableAt at;
	uint64		old;
	uint64		now;
	bool		has_old;
	bool		has_now;
	bool		holds;
	StdOptionRef left;
	StdOptionRef right;

	txn_memtable_table_tests_firsted(2143, 42, io, state);
	has_old = txn_memtable_table_tests_has_memtable(state, &old);
	run = simc_Run_new(97);
	simc_Run_actor(&run, "closer", 6, closer);
	simc_Run_until(&run, "closer", 6, "doltgres-memtable-case-closed");
	simc_Run_terminate_at(&run, "closer", 6, 5000);
	report = simc_Run_finish(run);
	if (report.outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtArg	args[1] = {{&report.outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 2168, 67, &message);
	}
	std_index_check(0, report.outcome.u.ended.len, RUST_FILE, 2169, 20);
	first = &STD_VEC_AT(&report.outcome.u.ended, simc_TupleStringEnding_t, 0).f1;
	STD_ASSERT_EQ(2169, 9, simc_Ending_PartialEq_eq(first, &terminated), first, simc_Ending_Debug_fmt,
				  &terminated, simc_Ending_Debug_fmt);
	has_now = txn_memtable_table_tests_has_memtable(state, &now);
	left.has = has_now;
	left.some = STD_ITEM(&now, std_u64_Debug_fmt);
	right.has = has_old;
	right.some = STD_ITEM(&old, std_u64_Debug_fmt);
	STD_ASSERT_EQ(2170, 9, has_now == has_old && (!has_now || now == old), &left, std_option_Debug_fmt,
				  &right, std_option_Debug_fmt);
	(void) txn_memtable_table_tests_writes(2171, 42, io, state, (const uint8 *) "k", 1);
	at = txn_memtable_table_tests_held_at(2172, 59, io, state, (const uint8 *) "k", 1, &holds);
	STD_ASSERT(2173, 9, holds, "holds");
	left.has = true;
	left.some = STD_ITEM(&at.memtable, std_u64_Debug_fmt);
	if (has_old && at.memtable == old)
		std_assert_failed(RUST_FILE, 2174, 9, "!=", &left, std_option_Debug_fmt, &right, std_option_Debug_fmt,
						  NULL);
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(2176, 38, io, 9306);
	txn_memtable_table_tests_used_is(2178, 9, io, start);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_keyed_writer_t is a keyed writer's captures: its database and its key. */
typedef struct txn_memtable_table_tests_keyed_writer_t
{
	uint32		database;
	const char *key;
} txn_memtable_table_tests_keyed_writer_t;

/* txn_memtable_table_tests_keyed_writer is a run's writer: its key's push in its database's storage. */
static bool
txn_memtable_table_tests_keyed_writer(void *captures, HostIoIo *io, HostError *error)
{
	const txn_memtable_table_tests_keyed_writer_t *c = captures;
	HostSharedHeld held;
	TxnMemtableVersionHandle handle;
	bool		ok;

	if (!txn_memtable_testing_stated(io, c->database, &held, error))
		return false;
	ok = txn_memtable_table_tests_written(io, host_shared_Held_record(&held), (const uint8 *) c->key, strlen(c->key),
										  &handle, error);
	host_shared_Held_drop(&held);
	return ok;
}

/* txn_memtable_table_tests_ranged_at asserts the range a key's hand-off finds and that it holds the key, as the cases' assert_eq does with the key's message. */
static void
txn_memtable_table_tests_ranged_at(uint32 unwrap_line, uint32 unwrap_col, uint32 line, uint32 col, HostIoIo *io,
								   HostSharedRecord state, const char *key, uint64 range)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdBytes	bytes = {(const uint8 *) key, strlen(key)};
	StdFmtArg	args[1] = {{&bytes, std_bytes_Debug_fmt}};
	StdFmtArguments message = {pieces, lengthof(pieces), args};
	TxnMemtableTableAt at;
	bool		holds;
	bool		yes = true;
	StdTupleRef left;
	StdTupleRef right;

	at = txn_memtable_table_tests_held_at(unwrap_line, unwrap_col, io, state, bytes.ptr, bytes.len, &holds);
	left = STD_TUPLE(2, STD_ITEM(&at.range, std_u64_Debug_fmt), STD_ITEM(&holds, std_bool_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&range, std_u64_Debug_fmt), STD_ITEM(&yes, std_bool_Debug_fmt));
	if (!(at.range == range && holds))
		std_assert_failed(RUST_FILE, line, col, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, &message);
}

/*
 * N2's case: two finders of one closed range, both held between their
 * gathering and the memtable's lock, take it out and retire it once; the
 * second's search by its first key lands on the range before it, which stays
 * listed with every other range, and every key is still found.
 */
void
test_txn_memtable_table_tests_two_finders_of_a_closed_range_take_out_only_that_range(void)
{
	static const char *const names[2] = {"first", "second"};
	static const txn_memtable_table_tests_keyed_writer_t writers[2] = {{9310, "p1"}, {9310, "p2"}};
	static const char *const keys[3] = {"a", "h", "pb"};
	static const char *const found[5] = {"a", "h", "p1", "p2", "pb"};
	static const size_t found_ranges[5] = {0, 1, 1, 1, 3};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9310);
	HostSharedRecord state = host_shared_Held_record(&held);
	StdBytes	firsts[3] = {{(const uint8 *) "g", 1}, {(const uint8 *) "p", 1}, {(const uint8 *) "pa", 2}};
	StdVec		ranges;
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	TxnMemtableIndexOpen open;
	SimcRun		run;
	SimcStep	steps[2] = {{"first", 5, PG_UINT64_MAX}, {"second", 6, PG_UINT64_MAX}};
	SimcReport	report;
	HostDeltaListListView view;
	HostError	error;
	StdVec		listed;
	uint64		wanted_ranges[3];
	StdVec		wanted = {wanted_ranges, 3, 3};
	uint64		location;
	bool		reached;

	if (!txn_memtable_testing_ranged(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, firsts, 3, &ranges, &error))
		STD_RESULT_EXPECT_FAILED(2247, 78, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(keys); k++)
		(void) txn_memtable_table_tests_writes(2249, 45, io, state, (const uint8 *) keys[k], strlen(keys[k]));
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2252, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, STD_VEC_AT(&ranges, uint64, 2), &record,
								&error))
		STD_RESULT_EXPECT_FAILED(2253, 58, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard, &error))
		STD_RESULT_EXPECT_FAILED(2254, 58, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_index_open(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS.closes, &guard, &open))
		STD_OPTION_UNWRAP_FAILED(2255, 37);
	txn_memtable_index_Open_close(open);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	run = simc_Run_new(101);
	for (size_t k = 0; k < lengthof(names); k++)
	{
		SimcActor	actor = {txn_memtable_table_tests_keyed_writer, (void *) &writers[k]};

		simc_Run_actor(&run, names[k], strlen(names[k]), actor);
	}
	simc_Run_until(&run, "first", 5, "doltgres-memtable-unlisting");
	simc_Run_until(&run, "second", 6, "doltgres-memtable-unlisting");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2269, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_table_tests_has_memtable(state, &location))
		STD_OPTION_UNWRAP_FAILED(2270, 56);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, location, &record, &error))
		STD_RESULT_EXPECT_FAILED(2271, 58, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_MEMTABLE_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(2273, 66, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	view = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape, TXN_MEMTABLE_RANGE_RANGES);
	std_vec_new(&listed, sizeof(uint64), _Alignof(uint64));
	for (uint64 entry = 0; entry < host_delta_list_ListView_len(&view); entry++)
	{
		uint64		range;

		if (!host_delta_list_ListView_get(&view, &hold, entry, &range, &error))
			STD_RESULT_EXPECT_FAILED(2275, 64, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		std_vec_push_Checked(&listed, &range, sizeof(range), _Alignof(uint64));
	}
	host_shared_Guard_drop(&guard);
	wanted_ranges[0] = STD_VEC_AT(&ranges, uint64, 0);
	wanted_ranges[1] = STD_VEC_AT(&ranges, uint64, 1);
	wanted_ranges[2] = STD_VEC_AT(&ranges, uint64, 3);
	STD_ASSERT_EQ(2277, 9, listed.len == 3 && memcmp(listed.ptr, wanted_ranges, sizeof(wanted_ranges)) == 0,
				  &listed, chunks_trailer_VecU64_Debug_fmt, &wanted, chunks_trailer_VecU64_Debug_fmt);
	std_vec_free_Checked(&listed, sizeof(uint64), _Alignof(uint64));
	reached = host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, STD_VEC_AT(&ranges, uint64, 2), &record,
									 &error);
	if (!reached)
		host_Error_drop(&error);
	STD_ASSERT(2278, 9, !reached, "hold.record::<Range>(ranges[2]).is_err()");
	host_delta_Hold_drop(&hold);
	for (size_t k = 0; k < lengthof(found); k++)
		txn_memtable_table_tests_ranged_at(2281, 62, 2282, 13, io, state, found[k],
										   STD_VEC_AT(&ranges, uint64, found_ranges[k]));
	simc_Report_drop(&report);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * N1's case: two writers that find one closed memtable, both held between
 * their gathering and the storage state's lock, clear the storage's word
 * once; the one whose exchange cleared it retires the memtable, the other
 * goes on to the next memtable, and the delta's use returns to its start once
 * that one is retired.
 */
void
test_txn_memtable_table_tests_two_writers_that_find_a_closed_memtable_clear_it_once(void)
{
	static const char *const names[2] = {"first", "second"};
	static const txn_memtable_table_tests_keyed_writer_t writers[2] = {{9311, "a"}, {9311, "b"}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9311);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableTableAt at;
	TxnMemtableTableAt a;
	TxnMemtableTableAt b;
	TxnMemtableTableGathered gathered;
	TxnMemtableIndexOpen open;
	HostDeltaHold hold;
	HostSharedRecord memtable;
	HostSharedGuard guard;
	SimcRun		run;
	SimcStep	steps[2] = {{"first", 5, PG_UINT64_MAX}, {"second", 6, PG_UINT64_MAX}};
	SimcReport	report;
	HostError	error;
	StdOptionRef left;
	StdOptionRef right;
	uint64		old;
	bool		has_old;
	bool		unlisted;
	bool		holds_a;
	bool		holds_b;

	txn_memtable_table_tests_firsted(2296, 42, io, state);
	has_old = txn_memtable_table_tests_has_memtable(state, &old);
	if (!txn_memtable_table_tests_closed(io, state, &at, &error))
		STD_RESULT_EXPECT_FAILED(2299, 48, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2300, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_Gathered_new(&gathered);
	if (!txn_memtable_table_Gathered_range(&gathered, io, &hold, at.range, &error))
		STD_RESULT_EXPECT_FAILED(2302, 49, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, at.memtable, &memtable, &error))
		STD_RESULT_EXPECT_FAILED(2303, 65, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock(memtable, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_MEMTABLE_LOCK, &guard, &error))
		STD_RESULT_EXPECT_FAILED(2304, 63, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_index_open(&TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS, &guard, &open))
		STD_OPTION_UNWRAP_FAILED(2305, 52);
	if (!txn_memtable_table_unlist(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, open, &hold, 0, at.memtable, &unlisted, &error))
		STD_RESULT_EXPECT_FAILED(2305, 85, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	host_shared_Guard_drop(&guard);
	if (!txn_memtable_table_Gathered_retire(&gathered, io, &error))
		STD_RESULT_EXPECT_FAILED(2307, 33, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_Gathered_drop(&gathered);
	host_delta_Hold_drop(&hold);
	run = simc_Run_new(103);
	for (size_t k = 0; k < lengthof(names); k++)
	{
		SimcActor	actor = {txn_memtable_table_tests_keyed_writer, (void *) &writers[k]};

		simc_Run_actor(&run, names[k], strlen(names[k]), actor);
	}
	simc_Run_until(&run, "first", 5, "doltgres-memtable-clearing");
	simc_Run_until(&run, "second", 6, "doltgres-memtable-clearing");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	a = txn_memtable_table_tests_held_at(2321, 60, io, state, (const uint8 *) "a", 1, &holds_a);
	b = txn_memtable_table_tests_held_at(2322, 60, io, state, (const uint8 *) "b", 1, &holds_b);
	STD_ASSERT(2323, 9, holds_a && holds_b, "holds_a && holds_b");
	STD_ASSERT_EQ(2324, 9, a.memtable == b.memtable && a.range == b.range, &a,
				  txn_memtable_table_tests_At_Debug_fmt, &b, txn_memtable_table_tests_At_Debug_fmt);
	left.has = true;
	left.some = STD_ITEM(&a.memtable, std_u64_Debug_fmt);
	right.has = has_old;
	right.some = STD_ITEM(&old, std_u64_Debug_fmt);
	if (has_old && a.memtable == old)
		std_assert_failed(RUST_FILE, 2325, 9, "!=", &left, std_option_Debug_fmt, &right, std_option_Debug_fmt,
						  NULL);
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(2327, 38, io, 9311);
	txn_memtable_table_tests_used_is(2329, 9, io, start);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/*
 * N3's case: a memtable emptied of its last range and never written again
 * leaves the storage's word clear, by the process that closed it, so the
 * state is reclaimed and the delta's use returns to its start.
 */
void
test_txn_memtable_table_tests_a_memtable_emptied_and_never_written_again_leaves_its_state_reclaimable(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9312);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableTableAt at;
	HostDeltaHold hold;
	HostError	error;
	StdOptionRef left;
	StdOptionRef none = {false};
	uint64		location;
	bool		freed;

	txn_memtable_table_tests_firsted(2340, 42, io, state);
	if (!txn_memtable_table_tests_closed(io, state, &at, &error))
		STD_RESULT_EXPECT_FAILED(2341, 44, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2343, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_table_unlisted(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, &hold, state, at, &error))
		STD_RESULT_EXPECT_FAILED(2344, 60, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	host_delta_Hold_drop(&hold);
	left.has = txn_memtable_table_tests_has_memtable(state, &location);
	left.some = STD_ITEM(&location, std_u64_Debug_fmt);
	STD_ASSERT_EQ(2346, 9, !left.has, &left, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	host_shared_Held_drop(&held);
	if (!TXN_STATE_STATE_POOL_OPS.reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(2348, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2348, 9, freed, "State::reclaim(io).unwrap()");
	txn_memtable_table_tests_used_is(2350, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_prefilled pushes the numbered keys below SPLIT - 1, which leave a storage's first range at SPLIT keys with its empty key, one short of its split. */
static bool
txn_memtable_table_tests_prefilled(HostIoIo *io, HostSharedRecord state, HostError *error)
{
	for (uint64 number = 0; number < TXN_MEMTABLE_SPLIT_SPLIT - 1; number++)
	{
		StdVec		key;
		TxnMemtableVersionHandle handle;
		bool		ok;

		txn_memtable_testing_numbered(number, &key);
		ok = txn_memtable_table_tests_written(io, state, key.ptr, key.len, &handle, error);
		std_vec_free_Checked(&key, 1, 1);
		if (!ok)
			return false;
	}
	return true;
}

/* txn_memtable_table_tests_prefills prefills a storage, as prefilled's unwrap does at its place. */
static void
txn_memtable_table_tests_prefills(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state)
{
	HostError	error;

	if (!txn_memtable_table_tests_prefilled(io, state, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/* txn_memtable_table_tests_listed_frame_0_t is listed's state across its pad: the hold, the guard, and the ranges, while live. */
typedef struct txn_memtable_table_tests_listed_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	StdVec	   *ranges;
	uint64	   *kept;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedGuard guard;
	bool		guard_live;
	bool		ranges_live;
} txn_memtable_table_tests_listed_frame_0_t;

/* txn_memtable_table_tests_listed_pad_0 is listed's pad: the ranges, the guard, then the hold, while live. */
static void
txn_memtable_table_tests_listed_pad_0(void *frame_)
{
	txn_memtable_table_tests_listed_frame_0_t *frame = frame_;

	if (frame->ranges_live)
	{
		frame->ranges_live = false;
		std_vec_free_Checked(frame->ranges, sizeof(uint64), _Alignof(uint64));
	}
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
}

/* txn_memtable_table_tests_listed_body_0 is listed's body. */
static void
txn_memtable_table_tests_listed_body_0(void *frame_)
{
	txn_memtable_table_tests_listed_frame_0_t *frame = frame_;
	HostSharedRecord memtable;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostDeltaListListView view;
	uint64		location;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_table_tests_has_memtable(frame->state, &location))
	{
		*frame->error = txn_memtable_testing_failed("no memtable");
		txn_memtable_table_tests_listed_pad_0(frame);
		return;
	}
	if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, location, &memtable,
								frame->error) ||
		!host_shared_Record_lock_shared(memtable, HOST_IO_IO_TAKES_SHORT(frame->io),
										TXN_MEMTABLE_RANGE_MEMTABLE_LOCK, &frame->guard, frame->error))
	{
		txn_memtable_table_tests_listed_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	view = host_delta_list_Guard_list(&frame->guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape,
									  TXN_MEMTABLE_RANGE_RANGES);
	std_vec_new(frame->ranges, sizeof(uint64), _Alignof(uint64));
	frame->ranges_live = true;
	for (uint64 entry = 0; entry < host_delta_list_ListView_len(&view); entry++)
	{
		uint64		range;

		if (!host_delta_list_ListView_get(&view, &frame->hold, entry, &range, frame->error))
		{
			txn_memtable_table_tests_listed_pad_0(frame);
			return;
		}
		std_vec_push_Checked(frame->ranges, &range, sizeof(range), _Alignof(uint64));
	}
	*frame->kept = host_shared_Record_atomic(host_shared_Guard_record(&frame->guard), TXN_MEMTABLE_RANGE_MEMTABLE_KEPT,
											 load);
	frame->ranges_live = false;
	frame->ok = true;
	txn_memtable_table_tests_listed_pad_0(frame);
}

/* txn_memtable_table_tests_listed returns a memtable's ranges' locations in its list's order, a Vec<u64>, and its count of kept entries. */
static bool
txn_memtable_table_tests_listed(HostIoIo *io, HostSharedRecord state, StdVec *ranges, uint64 *kept, HostError *error)
{
	txn_memtable_table_tests_listed_frame_0_t frame = {.io = io, .state = state, .ranges = ranges, .kept = kept,
	.error = error};

	std_landing(&frame, txn_memtable_table_tests_listed_body_0, txn_memtable_table_tests_listed_pad_0);
	return frame.ok;
}

/* txn_memtable_table_tests_two_halves reads a memtable's ranges, as listed's unwrap does, and asserts two with none kept, as assert_eq does at their places. */
static void
txn_memtable_table_tests_two_halves(uint32 unwrap_line, uint32 unwrap_col, uint32 line, uint32 col, HostIoIo *io,
									HostSharedRecord state, StdVec *ranges)
{
	HostError	error;
	uint64		kept;
	size_t		len;
	size_t		two = 2;
	uint64		none = 0;
	StdTupleRef left;
	StdTupleRef right;

	if (!txn_memtable_table_tests_listed(io, state, ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	len = ranges->len;
	left = STD_TUPLE(2, STD_ITEM(&len, std_usize_Debug_fmt), STD_ITEM(&kept, std_u64_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&two, std_usize_Debug_fmt), STD_ITEM(&none, std_u64_Debug_fmt));
	STD_ASSERT_EQ(line, col, len == 2 && kept == 0, &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
}

/* txn_memtable_table_tests_numbered_at asserts the range a numbered key's hand-off finds and that it holds the key, as the cases' assert_eq does with the number's message. */
static void
txn_memtable_table_tests_numbered_at(uint32 unwrap_line, uint32 unwrap_col, uint32 line, uint32 col, HostIoIo *io,
									 HostSharedRecord state, uint64 number, uint64 range)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[1] = {{&number, std_u64_Display_fmt}};
	StdFmtArguments message = {pieces, lengthof(pieces), args};
	StdVec		key;
	TxnMemtableTableAt at;
	bool		holds;
	bool		yes = true;
	StdTupleRef left;
	StdTupleRef right;

	txn_memtable_testing_numbered(number, &key);
	at = txn_memtable_table_tests_held_at(unwrap_line, unwrap_col, io, state, key.ptr, key.len, &holds);
	std_vec_free_Checked(&key, 1, 1);
	left = STD_TUPLE(2, STD_ITEM(&at.range, std_u64_Debug_fmt), STD_ITEM(&holds, std_bool_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&range, std_u64_Debug_fmt), STD_ITEM(&yes, std_bool_Debug_fmt));
	if (!(at.range == range && holds))
		std_assert_failed(RUST_FILE, line, col, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, &message);
}

/* txn_memtable_table_tests_pages_of reads a range's list of pages under its guard, each get unwrapped at its place. */
static void
txn_memtable_table_tests_pages_of(uint32 line, uint32 col, const HostSharedGuard *guard, const HostDeltaHold *hold,
								  StdVec *result)
{
	HostDeltaListListView view = host_delta_list_Guard_list(guard, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape,
															TXN_MEMTABLE_INDEX_PAGES);
	HostError	error;

	std_vec_new(result, sizeof(uint64), _Alignof(uint64));
	for (uint64 entry = 0; entry < host_delta_list_ListView_len(&view); entry++)
	{
		uint64		page;

		if (!host_delta_list_ListView_get(&view, hold, entry, &page, &error))
			STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		std_vec_push_Checked(result, &page, sizeof(page), _Alignof(uint64));
	}
}

/* txn_memtable_table_tests_head_of returns the head of a numbered key's node in a range, each unwrap at its place. */
static uint64
txn_memtable_table_tests_head_of(uint32 find_line, uint32 find_col, uint32 some_line, uint32 some_col,
								 uint32 head_line, uint32 head_col, const HostSharedGuard *guard,
								 const HostDeltaHold *hold, uint64 number)
{
	StdVec		key;
	TxnMemtableIndexNode node;
	HostError	error;
	uint64		head;
	bool		some;

	txn_memtable_testing_numbered(number, &key);
	if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, key.ptr, key.len, &some, &node,
								 &error))
		STD_RESULT_EXPECT_FAILED(find_line, find_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&key, 1, 1);
	if (!some)
		STD_OPTION_UNWRAP_FAILED(some_line, some_col);
	if (!txn_memtable_index_head(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, guard, hold, node, &head, &error))
		STD_RESULT_EXPECT_FAILED(head_line, head_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return head;
}

/* txn_memtable_table_tests_half_t is a split's half's (u64, Vec<u8>): its count of keys and its first key. */
typedef struct txn_memtable_table_tests_half_t
{
	uint64		keys;
	StdVec		first;
} txn_memtable_table_tests_half_t;

/* txn_memtable_table_tests_half_Debug_fmt is a half's (u64, Vec<u8>)'s Debug. */
static bool
txn_memtable_table_tests_half_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_table_tests_half_t *half = value;
	StdBytes	first = {half->first.ptr, half->first.len};
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&half->keys, std_u64_Debug_fmt), STD_ITEM(&first, std_bytes_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_memtable_table_tests_halves_Debug_fmt is two halves' Debug, a list of each half's. */
static bool
txn_memtable_table_tests_halves_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_table_tests_half_t *halves = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < 2; k++)
		std_DebugList_entry(&list, &halves[k], txn_memtable_table_tests_half_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_memtable_table_tests_range_at returns ranges[index], its index checked as Rust's is at its place. */
static uint64
txn_memtable_table_tests_range_at(const StdVec *ranges, size_t index, uint32 line, uint32 col)
{
	std_index_check(index, ranges->len, RUST_FILE, line, col);
	return STD_VEC_AT(ranges, uint64, index);
}

/*
 * PLAN S6's split: the push that takes a storage's first range past SPLIT
 * keys replaces it by halves at its middle key, the lower keeping the empty
 * key first with 16,384 keys and the upper the middle key with 16,385, each
 * naming the old range's pages at their indexes, each of them counting the
 * two halves' lists, and then its own, each counting one; every key is found
 * in the half its search gives, a key's head is the version it had, the old
 * range is retired, the kept entry is given back, and the delta's use returns
 * to its start once the memtable is retired.
 */
void
test_txn_memtable_table_tests_a_range_past_split_splits_at_its_middle_key(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9320);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	TxnMemtableTableAt old;
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostError	error;
	StdVec		old_pages;
	StdVec		ranges;
	StdVec		split;
	txn_memtable_table_tests_half_t halves[2];
	txn_memtable_table_tests_half_t wanted_halves[2];
	uint64		head;
	uint64		again;
	bool		holds;
	bool		reached;

	txn_memtable_table_tests_prefills(2384, 38, io, state);
	old = txn_memtable_table_tests_held_at(2385, 55, io, state, (const uint8 *) "", 0, &holds);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2387, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, old.range, &record, &error))
		STD_RESULT_EXPECT_FAILED(2388, 58, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(2389, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_pages_of(2391, 86, &guard, &hold, &old_pages);
	head = txn_memtable_table_tests_head_of(2392, 70, 2392, 79, 2393, 54, &guard, &hold,
											20000);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	{
		StdVec		key;

		txn_memtable_testing_numbered(TXN_MEMTABLE_SPLIT_SPLIT, &key);
		(void) txn_memtable_table_tests_writes(2395, 54, io, state, key.ptr, key.len);
		std_vec_free_Checked(&key, 1, 1);
	}
	txn_memtable_table_tests_two_halves(2396, 56, 2397, 9, io, state, &ranges);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2398, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	reached = host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, old.range, &record, &error);
	if (!reached)
		host_Error_drop(&error);
	STD_ASSERT(2399, 9, !reached, "hold.record::<Range>(old.range).is_err()");
	for (size_t k = 0; k < ranges.len; k++)
	{
		StdVec		pages;
		StdVec		lists;
		StdBytes	left;
		StdBytes	right;
		StdFmtArg	args[1] = {{&lists, chunks_trailer_VecU64_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};
		TxnMemtableIndexNode first;
		HostDeltaLent key;
		StdBytes	key_bytes;
		uint64		old_len = old_pages.len;
		uint64		page;
		bool		all = true;

		if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, STD_VEC_AT(&ranges, uint64, k), &record,
									&error))
			STD_RESULT_EXPECT_FAILED(2402, 54, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
											&error))
			STD_RESULT_EXPECT_FAILED(2403, 61, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_table_tests_pages_of(2405, 86, &guard, &hold, &pages);
		std_range_to_check(old_pages.len, pages.len, RUST_FILE, 2406, 25);
		left.ptr = pages.ptr;
		left.len = old_pages.len * sizeof(uint64);
		right.ptr = old_pages.ptr;
		right.len = old_pages.len * sizeof(uint64);
		if (memcmp(left.ptr, right.ptr, left.len) != 0)
		{
			StdVec		head_pages = {pages.ptr, old_pages.len, old_pages.len};

			std_assert_failed(RUST_FILE, 2406, 13, "==", &head_pages, chunks_trailer_VecU64_Debug_fmt,
							  &old_pages, chunks_trailer_VecU64_Debug_fmt, NULL);
		}
		STD_ASSERT(2407, 13, pages.len > old_pages.len, "pages.len() > old_pages.len()");
		std_vec_with_capacity_Checked(&lists, pages.len, sizeof(uint64), _Alignof(uint64));
		for (size_t p = 0; p < pages.len; p++)
		{
			HostDeltaPageRef ref;
			HostDeltaWord word;

			if (!host_delta_Hold_page(&hold, host_delta_page_Page_named(STD_VEC_AT(&pages, uint64, p)), &ref, &error))
				STD_RESULT_EXPECT_FAILED(2409, 71, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			if (!host_delta_PageRef_word(&ref, TXN_MEMTABLE_PAGES_LISTS, &word, &error))
				STD_RESULT_EXPECT_FAILED(2409, 92, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			STD_VEC_AT(&lists, uint64, p) = host_delta_Word_load(&word);
		}
		lists.len = pages.len;
		std_range_to_check(old_pages.len, lists.len, RUST_FILE, 2410, 21);
		for (size_t p = 0; p < old_pages.len && all; p++)
			all = STD_VEC_AT(&lists, uint64, p) == 2;
		if (!all)
			std_panic_fmt(RUST_FILE, 2410, 13, &message);
		std_range_from_check(old_pages.len, lists.len, RUST_FILE, 2411, 21);
		for (size_t p = old_pages.len; p < lists.len && all; p++)
			all = STD_VEC_AT(&lists, uint64, p) == 1;
		if (!all)
			std_panic_fmt(RUST_FILE, 2411, 13, &message);
		if (!txn_memtable_index_first(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &guard, &first))
			STD_OPTION_UNWRAP_FAILED(2412, 46);
		page = txn_memtable_index_Node_page(first);
		STD_ASSERT_EQ(2413, 13, page == old_len, &page, std_u64_Debug_fmt, &old_len, std_u64_Debug_fmt);
		halves[k].keys = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_KEYS, load);
		if (!txn_memtable_index_key(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &guard, &hold, first, &key, &error))
			STD_RESULT_EXPECT_FAILED(2414, 97, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		key_bytes = host_delta_Lent_Deref_deref(&key);
		std_slice_to_vec_Checked(&halves[k].first, key_bytes.ptr, key_bytes.len, 1, 1);
		host_delta_Lent_drop(&key);
		std_vec_free_Checked(&lists, sizeof(uint64), _Alignof(uint64));
		std_vec_free_Checked(&pages, sizeof(uint64), _Alignof(uint64));
		host_shared_Guard_drop(&guard);
	}
	txn_memtable_testing_numbered(16383, &split);
	wanted_halves[0].keys = 16384;
	std_vec_new(&wanted_halves[0].first, 1, 1);
	wanted_halves[1].keys = 16385;
	wanted_halves[1].first = split;
	STD_ASSERT_EQ(2416, 9, halves[0].keys == 16384 && halves[0].first.len == 0 && halves[1].keys == 16385 &&
				  halves[1].first.len == split.len && memcmp(halves[1].first.ptr, split.ptr, split.len) == 0, halves,
				  txn_memtable_table_tests_halves_Debug_fmt, wanted_halves, txn_memtable_table_tests_halves_Debug_fmt);
	std_vec_free_Checked(&split, 1, 1);
	std_vec_free_Checked(&halves[0].first, 1, 1);
	std_vec_free_Checked(&halves[1].first, 1, 1);
	host_delta_Hold_drop(&hold);
	for (uint64 number = 0; number < TXN_MEMTABLE_SPLIT_SPLIT - 1; number += 97)
		txn_memtable_table_tests_numbered_at(2419, 76, 2420, 13, io, state, number,
											 txn_memtable_table_tests_range_at(&ranges, number >= 16383,
																			   2420, 44));
	{
		static const uint64 tail[3] = {16382, 16383, TXN_MEMTABLE_SPLIT_SPLIT};

		for (size_t k = 0; k < lengthof(tail); k++)
			txn_memtable_table_tests_numbered_at(2419, 76, 2420, 13, io, state, tail[k],
												 txn_memtable_table_tests_range_at(&ranges, tail[k] >= 16383,
																				   2420, 44));
	}
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2423, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS,
								txn_memtable_table_tests_range_at(&ranges, 1, 2424, 47), &record, &error))
		STD_RESULT_EXPECT_FAILED(2424, 58, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(2425, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	again = txn_memtable_table_tests_head_of(2426, 70, 2426, 79, 2427, 57, &guard, &hold,
											 20000);
	STD_ASSERT_EQ(2427, 13, again == head, &again, std_u64_Debug_fmt, &head, std_u64_Debug_fmt);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&old_pages, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(2430, 38, io, 9320);
	txn_memtable_table_tests_used_is(2432, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_numbered_writer_t is a numbered writer's captures: its database and its key's number. */
typedef struct txn_memtable_table_tests_numbered_writer_t
{
	uint32		database;
	uint64		number;
} txn_memtable_table_tests_numbered_writer_t;

/* txn_memtable_table_tests_numbered_writer is a run's writer: its numbered key's push in its database's storage. */
static bool
txn_memtable_table_tests_numbered_writer(void *captures, HostIoIo *io, HostError *error)
{
	const txn_memtable_table_tests_numbered_writer_t *c = captures;
	HostSharedHeld held;
	TxnMemtableVersionHandle handle;
	StdVec		key;
	bool		ok;

	if (!txn_memtable_testing_stated(io, c->database, &held, error))
		return false;
	txn_memtable_testing_numbered(c->number, &key);
	ok = txn_memtable_table_tests_written(io, host_shared_Held_record(&held), key.ptr, key.len, &handle, error);
	std_vec_free_Checked(&key, 1, 1);
	host_shared_Held_drop(&held);
	return ok;
}

/*
 * S2's case and hand-off (b)'s: a splitter held after closing its range and
 * before the list's change, while two writers of the range's keys are held
 * between their gathering and the memtable's lock, then released in turn: the
 * first puts the halves in the list, the second and the splitter find the
 * entry moved, the halves are in the list once, the kept entry is given back
 * once, and the old range is retired once.
 */
void
test_txn_memtable_table_tests_two_finders_of_a_split_range_put_its_halves_in_once(void)
{
	static const char *const names[3] = {"splitter", "first", "second"};
	static const txn_memtable_table_tests_numbered_writer_t writers[3] = {
		{9321, TXN_MEMTABLE_SPLIT_SPLIT}, {9321, 5}, {9321, 30000},
	};
	static const uint64 numbers[3] = {5, 30000, TXN_MEMTABLE_SPLIT_SPLIT};
	static const size_t halves[3] = {0, 1, 1};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9321);
	HostSharedRecord state = host_shared_Held_record(&held);
	SimcRun		run;
	SimcStep	steps[3] = {{"first", 5, PG_UINT64_MAX}, {"second", 6, PG_UINT64_MAX}, {"splitter", 8, PG_UINT64_MAX}};
	SimcReport	report;
	StdVec		ranges;

	txn_memtable_table_tests_prefills(2446, 38, io, state);
	run = simc_Run_new(107);
	for (size_t k = 0; k < lengthof(names); k++)
	{
		SimcActor	actor = {txn_memtable_table_tests_numbered_writer, (void *) &writers[k]};

		simc_Run_actor(&run, names[k], strlen(names[k]), actor);
	}
	simc_Run_until(&run, "splitter", 8, "doltgres-memtable-split");
	simc_Run_until(&run, "first", 5, "doltgres-memtable-unlisting");
	simc_Run_until(&run, "second", 6, "doltgres-memtable-unlisting");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	txn_memtable_table_tests_two_halves(2460, 56, 2461, 9, io, state, &ranges);
	for (size_t k = 0; k < lengthof(numbers); k++)
		txn_memtable_table_tests_numbered_at(2463, 76, 2464, 13, io, state, numbers[k],
											 txn_memtable_table_tests_range_at(&ranges, halves[k], 2464, 44));
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(2467, 38, io, 9321);
	txn_memtable_table_tests_used_is(2469, 9, io, start);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/*
 * Case 24 and hand-off (a): a writer held between its release of the
 * memtable's lock and its take of the range's, while another's push splits
 * the range and puts the halves in the list, finds the range closed, goes
 * around, and writes its key into the half that holds it.
 */
void
test_txn_memtable_table_tests_a_writer_held_across_a_split_writes_into_its_half(void)
{
	static const char *const names[2] = {"writer", "splitter"};
	static const txn_memtable_table_tests_numbered_writer_t writers[2] = {
		{9322, 25000}, {9322, TXN_MEMTABLE_SPLIT_SPLIT},
	};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9322);
	HostSharedRecord state = host_shared_Held_record(&held);
	SimcRun		run;
	SimcStep	steps[2] = {{"splitter", 8, PG_UINT64_MAX}, {"writer", 6, PG_UINT64_MAX}};
	SimcReport	report;
	StdVec		ranges;
	StdVec		key;
	HostError	error;
	TxnMemtableTableAt at;
	uint64		kept;
	uint64		upper;
	size_t		len;
	size_t		two = 2;
	bool		holds;
	bool		yes = true;
	StdTupleRef left;
	StdTupleRef right;

	txn_memtable_table_tests_prefills(2481, 38, io, state);
	run = simc_Run_new(109);
	for (size_t k = 0; k < lengthof(names); k++)
	{
		SimcActor	actor = {txn_memtable_table_tests_numbered_writer, (void *) &writers[k]};

		simc_Run_actor(&run, names[k], strlen(names[k]), actor);
	}
	simc_Run_until(&run, "writer", 6, "doltgres-memtable-found");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(2493, 53, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_testing_numbered(25000, &key);
	at = txn_memtable_table_tests_held_at(2494, 72, io, state, key.ptr, key.len, &holds);
	std_vec_free_Checked(&key, 1, 1);
	len = ranges.len;
	upper = txn_memtable_table_tests_range_at(&ranges, 1, 2495, 57);
	left = STD_TUPLE(3, STD_ITEM(&len, std_usize_Debug_fmt), STD_ITEM(&at.range, std_u64_Debug_fmt),
					 STD_ITEM(&holds, std_bool_Debug_fmt));
	right = STD_TUPLE(3, STD_ITEM(&two, std_usize_Debug_fmt), STD_ITEM(&upper, std_u64_Debug_fmt),
					  STD_ITEM(&yes, std_bool_Debug_fmt));
	STD_ASSERT_EQ(2495, 9, len == 2 && at.range == upper && holds, &left, std_tuple_Debug_fmt, &right,
				  std_tuple_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	simc_Report_drop(&report);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_skewed returns a numbered key, padded to 209 bytes from the middle key on, so that a range's mean key length leaves its upper half short of the bytes a split estimates. */
static void
txn_memtable_table_tests_skewed(uint64 number, StdVec *result)
{
	static const uint8 x = 'x';

	txn_memtable_testing_numbered(number, result);
	if (number >= 16383)
		std_vec_resize(result, 209, &x, 1, 1);
}

/* txn_memtable_table_tests_raw_frame_0_t is raw's state across its pad: the pages taken, the fresh page, the hold, the guard, and the put, while live. */
typedef struct txn_memtable_table_tests_raw_frame_0_t
{
	HostIoIo   *io;
	uint64		range;
	StdBytes	key;
	HostError  *error;
	bool		ok;
	StdVec		spare;
	bool		spare_live;
	HostDeltaOwned fresh;
	bool		has_fresh;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedGuard guard;
	bool		guard_live;
	TxnMemtableOp op;
	bool		op_live;
	StdVec		buffer;
	bool		buffer_live;
} txn_memtable_table_tests_raw_frame_0_t;

/* txn_memtable_table_tests_raw_pad_0 is raw's pad: the buffer, the put, the guard, the hold, the spares, then the fresh page, while live. */
static void
txn_memtable_table_tests_raw_pad_0(void *frame_)
{
	txn_memtable_table_tests_raw_frame_0_t *frame = frame_;

	if (frame->buffer_live)
	{
		frame->buffer_live = false;
		std_vec_free_Checked(&frame->buffer, 1, 1);
	}
	if (frame->op_live)
	{
		frame->op_live = false;
		txn_memtable_Op_drop(&frame->op);
	}
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
	if (frame->spare_live)
	{
		frame->spare_live = false;
		std_vec_drop_Checked(&frame->spare, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
	}
	if (frame->has_fresh)
	{
		frame->has_fresh = false;
		host_delta_Owned_drop(&frame->fresh);
	}
}

/* txn_memtable_table_tests_raw_taken takes the pages a push wants: its fresh page and its spares. */
static bool
txn_memtable_table_tests_raw_taken(txn_memtable_table_tests_raw_frame_0_t *frame, const TxnMemtablePagesWant *want)
{
	StdVec		sizes;
	StdVec		taken;
	HostDeltaFull full;
	HostDeltaPageSize list_page = HOST_DELTA_LIST_PAGE;
	bool		ok;

	std_vec_with_capacity_Checked(&sizes, want->spares + 1, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	std_vec_push_Checked(&sizes, &want->page, sizeof(want->page), _Alignof(HostDeltaPageSize));
	for (size_t k = 0; k < want->spares; k++)
		std_vec_push_Checked(&sizes, &list_page, sizeof(list_page), _Alignof(HostDeltaPageSize));
	if (!host_delta_alloc(frame->io, sizes.ptr, sizes.len, &ok, &taken, &full, frame->error))
	{
		std_vec_free_Checked(&sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
		return false;
	}
	std_vec_free_Checked(&sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	if (!ok)
	{
		*frame->error = txn_memtable_testing_failed("the delta refused pages");
		return false;
	}
	frame->fresh = STD_VEC_AT(&taken, HostDeltaOwned, 0);
	memmove(taken.ptr, &STD_VEC_AT(&taken, HostDeltaOwned, 1), (taken.len - 1) * sizeof(HostDeltaOwned));
	taken.len--;
	frame->spare = taken;
	frame->has_fresh = true;
	frame->spare_live = true;
	return true;
}

/* txn_memtable_table_tests_raw_body_0 is raw's body. */
static void
txn_memtable_table_tests_raw_body_0(void *frame_)
{
	txn_memtable_table_tests_raw_frame_0_t *frame = frame_;
	TxnMemtablePagesWant want;
	bool		wanted = false;

	for (;;)
	{
		HostSharedRecord record;
		TxnMemtableIndexKeyedOpen open;
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7),
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionPushed pushed;

		if (!wanted)
		{
			std_vec_new(&frame->spare, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
			frame->spare_live = true;
		}
		else if (!txn_memtable_table_tests_raw_taken(frame, &want))
		{
			txn_memtable_table_tests_raw_pad_0(frame);
			return;
		}
		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		{
			txn_memtable_table_tests_raw_pad_0(frame);
			return;
		}
		frame->hold_live = true;
		if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, frame->range, &record,
									frame->error) ||
			!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_RANGE_LOCK,
									 &frame->guard, frame->error))
		{
			txn_memtable_table_tests_raw_pad_0(frame);
			return;
		}
		frame->guard_live = true;
		if (!txn_memtable_index_keyed_open(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &frame->guard, &open))
		{
			*frame->error = txn_memtable_testing_failed("the range is closed");
			txn_memtable_table_tests_raw_pad_0(frame);
			return;
		}
		frame->op = txn_memtable_testing_put((const uint8 *) "v", 1);
		frame->op_live = true;
		change.op = &frame->op;
		std_vec_new(&frame->buffer, 1, 1);
		frame->buffer_live = true;
		if (!txn_memtable_version_push(&open, &frame->hold, frame->key.ptr, frame->key.len, &change, false,
									   &frame->has_fresh,
									   &frame->fresh, &frame->spare, &frame->buffer, &pushed, frame->error))
		{
			txn_memtable_table_tests_raw_pad_0(frame);
			return;
		}
		txn_memtable_table_tests_raw_pad_0(frame);
		if (pushed.tag == TXN_MEMTABLE_VERSION_PUSHED_VERSION)
		{
			frame->ok = true;
			return;
		}
		want = pushed.u.wants;
		wanted = true;
	}
}

/* txn_memtable_table_tests_raw pushes a version of a key straight into a range under its lock, bringing the pages it wants, with no split, as a writer between a splitter's takes does to the range's index. */
static bool
txn_memtable_table_tests_raw(HostIoIo *io, uint64 range, const uint8 *key, size_t key_len, HostError *error)
{
	txn_memtable_table_tests_raw_frame_0_t frame = {.io = io, .range = range, .error = error};

	frame.key.ptr = key;
	frame.key.len = key_len;
	std_landing(&frame, txn_memtable_table_tests_raw_body_0, txn_memtable_table_tests_raw_pad_0);
	return frame.ok;
}

/* txn_memtable_table_tests_skewed_writer is the short split's splitter: its push of the skewed key past SPLIT. */
static bool
txn_memtable_table_tests_skewed_writer(void *captures, HostIoIo *io, HostError *error)
{
	HostSharedHeld held;
	TxnMemtableVersionHandle handle;
	StdVec		key;
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9323, &held, error))
		return false;
	txn_memtable_table_tests_skewed(TXN_MEMTABLE_SPLIT_SPLIT, &key);
	ok = txn_memtable_table_tests_written(io, host_shared_Held_record(&held), key.ptr, key.len, &handle, error);
	std_vec_free_Checked(&key, 1, 1);
	host_shared_Held_drop(&held);
	return ok;
}

/* txn_memtable_table_tests_raw_writer_t is a raw writer's captures: the range it writes into and its keys' first number. */
typedef struct txn_memtable_table_tests_raw_writer_t
{
	const uint64 *range;
	uint64		from;
} txn_memtable_table_tests_raw_writer_t;

/* txn_memtable_table_tests_raw_writer is a short split's writer between its takes: 40 long keys pushed straight into the range. */
static bool
txn_memtable_table_tests_raw_writer(void *captures, HostIoIo *io, HostError *error)
{
	static const uint8 y = 'y';
	const txn_memtable_table_tests_raw_writer_t *c = captures;

	for (uint64 number = c->from; number < c->from + 40; number++)
	{
		StdVec		key;
		bool		ok;

		txn_memtable_testing_numbered(number, &key);
		std_vec_resize(&key, 8000, &y, 1, 1);
		ok = txn_memtable_table_tests_raw(io, *c->range, key.ptr, key.len, error);
		std_vec_free_Checked(&key, 1, 1);
		if (!ok)
			return false;
	}
	return true;
}

/* txn_memtable_table_tests_skew_writes pushes a skewed key's version, as written's unwrap does at its place. */
static void
txn_memtable_table_tests_skew_writes(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state, uint64 number)
{
	StdVec		key;

	txn_memtable_table_tests_skewed(number, &key);
	(void) txn_memtable_table_tests_writes(line, col, io, state, key.ptr, key.len);
	std_vec_free_Checked(&key, 1, 1);
}

/*
 * S1's and S3's case, with (i)'s: a split whose first take finds its pages
 * short of the skewed upper half, and whose second and third find the index
 * grown past its margin by writes between the takes, gives up after the
 * third, leaving the range whole and open with every key, its kept stored,
 * and the entry it kept left with the range; the next push past SPLIT splits
 * it with that entry, and the kept count is zero after.
 */
void
test_txn_memtable_table_tests_a_split_short_at_every_take_gives_up_and_leaves_its_entry_for_the_next(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	static const uint64 numbers[3] = {0, 16383, TXN_MEMTABLE_SPLIT_SPLIT};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9323);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	SimcRun		run;
	SimcActor	splitter = {txn_memtable_table_tests_skewed_writer};
	SimcActor	first;
	SimcActor	second;
	txn_memtable_table_tests_raw_writer_t raws[2];
	SimcStep	first_step[1] = {{"first", 5, PG_UINT64_MAX}};
	SimcStep	steps[2] = {{"second", 6, PG_UINT64_MAX}, {"splitter", 8, PG_UINT64_MAX}};
	SimcReport	report;
	TxnMemtableTableAt at;
	HostDeltaHold hold;
	HostSharedRecord record;
	HostError	error;
	StdVec		ranges;
	StdVec		wanted_ranges;
	uint64		range;
	uint64		kept;
	uint64		range_kept;
	uint64		one = 1;
	uint64		wanted_kept = 1;
	bool		holds;
	StdTupleRef left;
	StdTupleRef right;

	for (uint64 number = 0; number < TXN_MEMTABLE_SPLIT_SPLIT - 1; number++)
		txn_memtable_table_tests_skew_writes(2545, 57, io, state, number);
	at = txn_memtable_table_tests_held_at(2547, 52, io, state, (const uint8 *) "", 0, &holds);
	range = at.range;
	raws[0].range = &range;
	raws[0].from = 40000;
	raws[1].range = &range;
	raws[1].from = 50000;
	first.call_once = txn_memtable_table_tests_raw_writer;
	first.captures = &raws[0];
	first.drop = NULL;
	first.size = 0;
	first.align = 0;
	second = first;
	second.captures = &raws[1];
	run = simc_Run_new(113);
	simc_Run_actor(&run, "splitter", 8, splitter);
	simc_Run_actor(&run, "first", 5, first);
	simc_Run_actor(&run, "second", 6, second);
	simc_Run_until(&run, "splitter", 8, "doltgres-memtable-split-short");
	simc_Run_script(&run, first_step, lengthof(first_step));
	simc_Run_until(&run, "splitter", 8, "doltgres-memtable-split-short");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(2569, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	wanted_ranges.ptr = &range;
	wanted_ranges.len = 1;
	wanted_ranges.cap = 1;
	left = STD_TUPLE(2, STD_ITEM(&ranges, chunks_trailer_VecU64_Debug_fmt), STD_ITEM(&kept, std_u64_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&wanted_ranges, chunks_trailer_VecU64_Debug_fmt),
					  STD_ITEM(&wanted_kept, std_u64_Debug_fmt));
	STD_ASSERT_EQ(2569, 9, ranges.len == 1 && STD_VEC_AT(&ranges, uint64, 0) == range && kept == 1, &left,
				  std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2571, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, range, &record, &error))
		STD_RESULT_EXPECT_FAILED(2572, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	range_kept = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_KEPT, load);
	STD_ASSERT_EQ(2573, 13, range_kept == 1, &range_kept, std_u64_Debug_fmt, &one, std_u64_Debug_fmt);
	host_delta_Hold_drop(&hold);
	for (size_t k = 0; k < lengthof(numbers); k++)
	{
		StdVec		key;
		StdFmtArg	args[1] = {{&numbers[k], std_u64_Display_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		txn_memtable_table_tests_skewed(numbers[k], &key);
		(void) txn_memtable_table_tests_held_at(2576, 64, io, state, key.ptr, key.len, &holds);
		std_vec_free_Checked(&key, 1, 1);
		if (!holds)
			std_panic_fmt(RUST_FILE, 2576, 13, &message);
	}
	txn_memtable_table_tests_skew_writes(2578, 56, io, state, TXN_MEMTABLE_SPLIT_SPLIT + 1);
	txn_memtable_table_tests_two_halves(2579, 56, 2580, 9, io, state, &ranges);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	simc_Report_drop(&report);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_numbered_writes pushes a numbered key's version, as written's unwrap does at its place. */
static void
txn_memtable_table_tests_numbered_writes(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state, uint64 number)
{
	StdVec		key;

	txn_memtable_testing_numbered(number, &key);
	(void) txn_memtable_table_tests_writes(line, col, io, state, key.ptr, key.len);
	std_vec_free_Checked(&key, 1, 1);
}

/*
 * (j): a split whose write of a half's nodes fails returns the error with
 * the range open and whole, its keys found, its pages' counts of lists as
 * they were, and the entry it kept left with the range, since its kept is
 * stored; the next push splits it, and the delta's use returns to its start
 * once the memtable is retired, so the failed split's halves and their pages
 * were given back.
 */
void
test_txn_memtable_table_tests_a_split_whose_half_fails_to_build_leaves_the_range_open(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	static const uint64 numbers[3] = {0, 16383, TXN_MEMTABLE_SPLIT_SPLIT};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9324);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableTableAt at;
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostDeltaListListView view;
	HostError	error;
	TxnMemtableVersionHandle handle;
	StdVec		key;
	StdVec		ranges;
	StdVec		wanted_ranges;
	StdBytes	sqlstate;
	StdBytes	internal = {(const uint8 *) "XX000", 5};
	StdTupleRef left;
	StdTupleRef right;
	uint64		range;
	uint64		kept;
	uint64		wanted_kept = 1;
	uint64		one = 1;
	uint64		zero = 0;
	size_t		two = 2;
	size_t		len;
	bool		holds;

	txn_memtable_table_tests_prefills(2594, 38, io, state);
	at = txn_memtable_table_tests_held_at(2595, 52, io, state, (const uint8 *) "", 0, &holds);
	range = at.range;
	simc_refuse_owned_append(1000);
	txn_memtable_testing_numbered(TXN_MEMTABLE_SPLIT_SPLIT, &key);
	if (txn_memtable_table_tests_written(io, state, key.ptr, key.len, &handle, &error))
	{
		char		unit = 0;

		STD_RESULT_EXPECT_FAILED(2597, 78, "called `Result::unwrap_err()` on an `Ok` value", &unit,
								 std_unit_Debug_fmt);
	}
	std_vec_free_Checked(&key, 1, 1);
	sqlstate.ptr = host_Error_sqlstate(&error);
	sqlstate.len = 5;
	STD_ASSERT_EQ(2598, 9, memcmp(sqlstate.ptr, "XX000", 5) == 0, &sqlstate, std_bytes_Debug_fmt, &internal,
				  std_bytes_Debug_fmt);
	host_Error_drop(&error);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(2599, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	wanted_ranges.ptr = &range;
	wanted_ranges.len = 1;
	wanted_ranges.cap = 1;
	left = STD_TUPLE(2, STD_ITEM(&ranges, chunks_trailer_VecU64_Debug_fmt), STD_ITEM(&kept, std_u64_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&wanted_ranges, chunks_trailer_VecU64_Debug_fmt),
					  STD_ITEM(&wanted_kept, std_u64_Debug_fmt));
	STD_ASSERT_EQ(2599, 9, ranges.len == 1 && STD_VEC_AT(&ranges, uint64, 0) == range && kept == 1, &left,
				  std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2601, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, range, &record, &error))
		STD_RESULT_EXPECT_FAILED(2602, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(2603, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2604, 13, !txn_memtable_index_closed(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS.closes, &guard),
			   "!index::closed(&guard)");
	view = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES);
	for (uint64 entry = 0; entry < host_delta_list_ListView_len(&view); entry++)
	{
		HostDeltaPageRef page;
		HostDeltaWord word;
		uint64		name;
		uint64		lists;

		if (!host_delta_list_ListView_get(&view, &hold, entry, &name, &error))
			STD_RESULT_EXPECT_FAILED(2607, 73, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!host_delta_Hold_page(&hold, host_delta_page_Page_named(name), &page, &error))
			STD_RESULT_EXPECT_FAILED(2607, 84, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_LISTS, &word, &error))
			STD_RESULT_EXPECT_FAILED(2608, 45, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		lists = host_delta_Word_load(&word);
		STD_ASSERT_EQ(2608, 17, lists == 1, &lists, std_u64_Debug_fmt, &one, std_u64_Debug_fmt);
	}
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	for (size_t k = 0; k < lengthof(numbers); k++)
	{
		StdFmtArg	args[1] = {{&numbers[k], std_u64_Display_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		txn_memtable_testing_numbered(numbers[k], &key);
		(void) txn_memtable_table_tests_held_at(2612, 66, io, state, key.ptr, key.len, &holds);
		std_vec_free_Checked(&key, 1, 1);
		if (!holds)
			std_panic_fmt(RUST_FILE, 2612, 13, &message);
	}
	txn_memtable_table_tests_numbered_writes(2614, 58, io, state, TXN_MEMTABLE_SPLIT_SPLIT + 1);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(2615, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	len = ranges.len;
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	STD_ASSERT_EQ(2615, 9, len == 2, &len, std_usize_Debug_fmt, &two, std_usize_Debug_fmt);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(2616, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	STD_ASSERT_EQ(2616, 9, kept == 0, &kept, std_u64_Debug_fmt, &zero, std_u64_Debug_fmt);
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(2618, 38, io, 9324);
	txn_memtable_table_tests_used_is(2620, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_refused_writer is a finder whose push past its refused open of a page must fail with XX000. */
static bool
txn_memtable_table_tests_refused_writer(void *captures, HostIoIo *io, HostError *error)
{
	const HostDeltaPagePage *last = captures;
	HostSharedHeld held;
	TxnMemtableVersionHandle handle;
	HostError	written_error;
	StdVec		key;
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9325, &held, error))
		return false;
	simc_refuse_open(*last, 0);
	txn_memtable_testing_numbered(10, &key);
	ok = !txn_memtable_table_tests_written(io, host_shared_Held_record(&held), key.ptr, key.len, &handle,
										   &written_error);
	std_vec_free_Checked(&key, 1, 1);
	if (!ok)
		*error = txn_memtable_testing_failed("a gathering past a refused open went on");
	else if (memcmp(host_Error_sqlstate(&written_error), "XX000", 5) == 0)
		host_Error_drop(&written_error);
	else
	{
		*error = written_error;
		ok = false;
	}
	host_shared_Held_drop(&held);
	return ok;
}

/*
 * (k): a finder of a split range whose gathering can't open one of the
 * range's pages returns the error with the range closed and still in the
 * list; the next finder puts the halves in and retires it, and the splitter,
 * held after closing, finds the entry moved.
 */
void
test_txn_memtable_table_tests_a_retirement_that_cannot_open_a_page_leaves_the_range_for_the_next_finder(void)
{
	static const txn_memtable_table_tests_numbered_writer_t splitter_captures = {9325, TXN_MEMTABLE_SPLIT_SPLIT};
	static const txn_memtable_table_tests_numbered_writer_t next_captures = {9325, 20};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9325);
	HostSharedRecord state = host_shared_Held_record(&held);
	SimcActor	splitter = {txn_memtable_table_tests_numbered_writer, (void *) &splitter_captures};
	SimcActor	refused = {txn_memtable_table_tests_refused_writer};
	SimcActor	next = {txn_memtable_table_tests_numbered_writer, (void *) &next_captures};
	SimcStep	first_step[1] = {{"refused", 7, PG_UINT64_MAX}};
	SimcStep	steps[2] = {{"next", 4, PG_UINT64_MAX}, {"splitter", 8, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;
	HostDeltaPagePage last;
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostDeltaListListView view;
	HostError	error;
	StdVec		ranges;
	StdVec		key;
	StdTupleRef left;
	StdTupleRef right;
	uint64		range;
	uint64		name;
	uint64		kept;
	uint64		zero = 0;
	size_t		len;
	size_t		two = 2;
	bool		holds;
	bool		contains = false;

	txn_memtable_table_tests_prefills(2632, 38, io, state);
	range = txn_memtable_table_tests_held_at(2634, 56, io, state, (const uint8 *) "", 0, &holds).range;
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2635, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, range, &record, &error))
		STD_RESULT_EXPECT_FAILED(2636, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(2637, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	view = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES);
	if (!host_delta_list_ListView_get(&view, &hold, host_delta_list_ListView_len(&view) - 1, &name, &error))
		STD_RESULT_EXPECT_FAILED(2639, 65, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	last = host_delta_page_Page_named(name);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	refused.captures = &last;
	run = simc_Run_new(127);
	simc_Run_actor(&run, "splitter", 8, splitter);
	simc_Run_actor(&run, "refused", 7, refused);
	simc_Run_actor(&run, "next", 4, next);
	simc_Run_until(&run, "splitter", 8, "doltgres-memtable-split");
	simc_Run_script(&run, first_step, lengthof(first_step));
	simc_Run_until(&run, "next", 4, "doltgres-memtable-unlisting");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(2665, 56, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	len = ranges.len;
	left = STD_TUPLE(2, STD_ITEM(&len, std_usize_Debug_fmt), STD_ITEM(&kept, std_u64_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&two, std_usize_Debug_fmt), STD_ITEM(&zero, std_u64_Debug_fmt));
	STD_ASSERT_EQ(2666, 9, len == 2 && kept == 0, &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
	for (size_t k = 0; k < ranges.len; k++)
		contains |= STD_VEC_AT(&ranges, uint64, k) == range;
	STD_ASSERT(2667, 9, !contains, "!ranges.contains(&range)");
	txn_memtable_testing_numbered(20, &key);
	(void) txn_memtable_table_tests_held_at(2668, 58, io, state, key.ptr, key.len, &holds);
	std_vec_free_Checked(&key, 1, 1);
	STD_ASSERT(2668, 9, holds, "holder(io, held.record(), &numbered(20)).unwrap().1");
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	simc_Report_drop(&report);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * txn_memtable_table_tests_oversized fills a storage's rows' memtable with
 * one range a push left past SPLIT keys, splitting nothing, and returns where
 * it is with what the push saw of it, as the settle's split is given them.
 */
static TxnMemtableTableAt
txn_memtable_table_tests_oversized(HostIoIo *io, HostSharedRecord owner, TxnMemtableSplitSeen *seen)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	TxnMemtableTableAt at;
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostDeltaListListView view;
	HostError	error;
	StdVec		key;
	uint64		keys = TXN_MEMTABLE_SPLIT_SPLIT + 1;
	bool		holds;

	if (!txn_memtable_table_tests_prefilled(io, owner, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_table_tests_holder(io, owner, (const uint8 *) "", 0, &at, &holds, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_testing_numbered(TXN_MEMTABLE_SPLIT_SPLIT, &key);
	if (!txn_memtable_table_tests_raw(io, at.range, key.ptr, key.len, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at.range, &record, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	seen->keys = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_KEYS, load);
	seen->bytes = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_BYTES, load);
	view = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES);
	seen->pages = host_delta_list_ListView_len(&view);
	seen->longest = key.len;
	std_vec_free_Checked(&key, 1, 1);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	STD_ASSERT_EQ(__LINE__, 0, seen->keys == keys, &seen->keys, std_u64_Debug_fmt, &keys, std_u64_Debug_fmt);
	return at;
}

/*
 * P2's case: a split whose take of the range's record fails before it stores
 * the range's kept returns the error with the range open, unsplit, and
 * unmarked, and its entry left kept, PLAN's cost, which no split takes, so
 * the next split leaves it kept. The range is past SPLIT by a push that
 * splits nothing, so the split's step 3 makes its one take of the record.
 */
void
test_txn_memtable_table_tests_a_split_that_fails_before_its_kept_leaves_its_entry_kept(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9326);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	TxnMemtableTableAt at;
	TxnMemtableSplitSeen seen;
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostError	error;
	StdVec		key;
	StdVec		ranges;
	StdVec		wanted_ranges;
	StdBytes	sqlstate;
	StdBytes	internal = {(const uint8 *) "XX000", 5};
	StdTupleRef left;
	StdTupleRef right;
	uint64		kept;
	uint64		wanted_kept = 1;
	uint64		zero = 0;
	size_t		len;
	size_t		two = 2;
	bool		holds;

	at = txn_memtable_table_tests_oversized(io, state, &seen);
	simc_refuse_record(TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape.id, at.range, 0);
	if (txn_memtable_split_split(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, at, &seen, &error))
	{
		char		unit = 0;

		STD_RESULT_EXPECT_FAILED(2696, 74, "called `Result::unwrap_err()` on an `Ok` value", &unit,
								 std_unit_Debug_fmt);
	}
	sqlstate.ptr = host_Error_sqlstate(&error);
	sqlstate.len = 5;
	STD_ASSERT_EQ(2697, 9, memcmp(sqlstate.ptr, "XX000", 5) == 0, &sqlstate, std_bytes_Debug_fmt, &internal,
				  std_bytes_Debug_fmt);
	host_Error_drop(&error);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(2698, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	wanted_ranges.ptr = &at.range;
	wanted_ranges.len = 1;
	wanted_ranges.cap = 1;
	left = STD_TUPLE(2, STD_ITEM(&ranges, chunks_trailer_VecU64_Debug_fmt), STD_ITEM(&kept, std_u64_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&wanted_ranges, chunks_trailer_VecU64_Debug_fmt),
					  STD_ITEM(&wanted_kept, std_u64_Debug_fmt));
	STD_ASSERT_EQ(2698, 9, ranges.len == 1 && STD_VEC_AT(&ranges, uint64, 0) == at.range && kept == 1, &left,
				  std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(2700, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at.range, &record, &error))
		STD_RESULT_EXPECT_FAILED(2701, 57, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	kept = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_KEPT, load);
	STD_ASSERT_EQ(2702, 13, kept == 0, &kept, std_u64_Debug_fmt, &zero, std_u64_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(2703, 73, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2703, 13, !txn_memtable_index_closed(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS.closes, &guard),
			   "!index::closed(&record.lock_shared::<RangeLock>(io).unwrap())");
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	if (!txn_memtable_split_split(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, at, &seen, &error))
		STD_RESULT_EXPECT_FAILED(2705, 60, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(2706, 56, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	len = ranges.len;
	left = STD_TUPLE(2, STD_ITEM(&len, std_usize_Debug_fmt), STD_ITEM(&kept, std_u64_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&two, std_usize_Debug_fmt), STD_ITEM(&wanted_kept, std_u64_Debug_fmt));
	STD_ASSERT_EQ(2707, 9, len == 2 && kept == 1, &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
	txn_memtable_testing_numbered(TXN_MEMTABLE_SPLIT_SPLIT, &key);
	(void) txn_memtable_table_tests_held_at(2708, 61, io, state, key.ptr, key.len, &holds);
	std_vec_free_Checked(&key, 1, 1);
	STD_ASSERT(2708, 9, holds, "holder(io, held.record(), &numbered(SPLIT)).unwrap().1");
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_lsn_t is an Option<u64>: a version's record's end, when it has one. */
typedef struct txn_memtable_table_tests_lsn_t
{
	bool		has;
	uint64		lsn;
} txn_memtable_table_tests_lsn_t;

/* txn_memtable_table_tests_lsns_Debug_fmt is a Vec<Option<u64>>'s Debug, of txn_memtable_table_tests_lsn_t items. */
static bool
txn_memtable_table_tests_lsns_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
	{
		const txn_memtable_table_tests_lsn_t *lsn = &STD_VEC_AT(v, txn_memtable_table_tests_lsn_t, k);
		StdOptionRef option = {lsn->has, STD_ITEM(&lsn->lsn, std_u64_Debug_fmt)};

		std_DebugList_entry(&list, &option, std_option_Debug_fmt);
	}
	return std_DebugList_finish(&list);
}

/*
 * The LSN text's case: a row's version pushed with its record's end reads it
 * back through its key's chain, and an unlogged table's, pushed with none,
 * reads back none.
 */
void
test_txn_memtable_table_tests_a_rows_version_reads_back_its_records_lsn(void)
{
	static const char *const keys[2] = {"logged", "unlogged"};
	static const txn_memtable_table_tests_lsn_t lsns[2] = {{true, UINT64CONST(0x100000a28)}, {false}};
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9327);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostError	error;

	for (size_t k = 0; k < lengthof(keys); k++)
	{
		const uint8 *key = (const uint8 *) keys[k];
		size_t		key_len = strlen(keys[k]);
		StdBytes	key_bytes = {key, key_len};
		StdFmtArg	args[1] = {{&key_bytes, std_bytes_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7), .op = &op,
			.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND},
		.has_lsn = lsns[k].has, .lsn = lsns[k].lsn};
		TxnMemtableVersionHandle handle;
		HostDeltaHold hold;
		TxnMemtableTableAt at;
		HostSharedRecord record;
		HostSharedGuard guard;
		TxnMemtableIndexNode node;
		TxnMemtableVersionChain chain;
		StdVec		read;
		StdVec		wanted;
		bool		some;

		if (!txn_memtable_table_push(io, state, key, key_len, &change, &handle, &error))
			STD_RESULT_EXPECT_FAILED(2725, 14, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
		if (!host_delta_hold(io, &hold, &error))
			STD_RESULT_EXPECT_FAILED(2726, 40, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_memtable_table_found(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, &hold, state, key, key_len, NULL, &some,
									  &at, &error))
			STD_RESULT_EXPECT_FAILED(2727, 73, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!some)
			STD_OPTION_UNWRAP_FAILED(2727, 82);
		if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at.range, &record, &error))
			STD_RESULT_EXPECT_FAILED(2728, 57, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
											&error))
			STD_RESULT_EXPECT_FAILED(2729, 61, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_memtable_index_find(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &guard, &hold, key, key_len, &some, &node,
									 &error))
			STD_RESULT_EXPECT_FAILED(2730, 56, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!some)
			STD_OPTION_UNWRAP_FAILED(2730, 65);
		if (!txn_memtable_version_chain(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &guard, &hold, node, &chain, &error))
			STD_RESULT_EXPECT_FAILED(2732, 44, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		std_vec_new(&read, sizeof(txn_memtable_table_tests_lsn_t), _Alignof(txn_memtable_table_tests_lsn_t));
		for (;;)
		{
			TxnMemtableVersionVersion version;
			TxnMemtableVersionRecord version_record;
			txn_memtable_table_tests_lsn_t lsn = {0};

			if (!txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, &error))
				STD_RESULT_EXPECT_FAILED(2732, 75, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			if (!some)
				break;
			version_record = txn_memtable_version_Version_record(&version);
			lsn.has = txn_memtable_version_Record_lsn(&version_record, &lsn.lsn);
			txn_memtable_version_Version_drop(&version);
			std_vec_push_Checked(&read, &lsn, sizeof(lsn), _Alignof(txn_memtable_table_tests_lsn_t));
		}
		wanted.ptr = (void *) &lsns[k];
		wanted.len = 1;
		wanted.cap = 1;
		if (!(read.len == 1 && STD_VEC_AT(&read, txn_memtable_table_tests_lsn_t, 0).has == lsns[k].has &&
			  (!lsns[k].has || STD_VEC_AT(&read, txn_memtable_table_tests_lsn_t, 0).lsn == lsns[k].lsn)))
			std_assert_failed(RUST_FILE, 2733, 13, "==", &read, txn_memtable_table_tests_lsns_Debug_fmt, &wanted,
							  txn_memtable_table_tests_lsns_Debug_fmt, &message);
		std_vec_free_Checked(&read, sizeof(txn_memtable_table_tests_lsn_t), _Alignof(txn_memtable_table_tests_lsn_t));
		host_shared_Guard_drop(&guard);
		host_delta_Hold_drop(&hold);
	}
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_windowed_frame_0_t is windowed's state across its pad: its arguments, and its put, while live. */
typedef struct txn_memtable_table_tests_windowed_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	StdBytes	key;
	TxnMemtableVersionHandle *result;
	HostError  *error;
	bool		ok;
	TxnMemtableOp op;
	bool		op_live;
} txn_memtable_table_tests_windowed_frame_0_t;

/* txn_memtable_table_tests_windowed_pad_0 is windowed's pad: its put, while live. */
static void
txn_memtable_table_tests_windowed_pad_0(void *frame_)
{
	txn_memtable_table_tests_windowed_frame_0_t *frame = frame_;

	if (frame->op_live)
	{
		frame->op_live = false;
		txn_memtable_Op_drop(&frame->op);
	}
}

/* txn_memtable_table_tests_windowed_closure_0 is windowed's window: its finish while every take is refused. */
static bool
txn_memtable_table_tests_windowed_closure_0(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
											TxnMemtableVersionHandle *result, HostError *error)
{
	const TxnMemtableVersionChange *change = captures;
	TxnStatusRoomed roomed = {0};
	bool		finished;

	simc_refuse_any_take(0);
	finished = txn_memtable_table_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, change, false, roomed, result, error);
	simc_refuse_nothing();
	return finished;
}

/* txn_memtable_table_tests_windowed_body_0 is windowed's body. */
static void
txn_memtable_table_tests_windowed_body_0(void *frame_)
{
	txn_memtable_table_tests_windowed_frame_0_t *frame = frame_;
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7),
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	txn_TupleRefSliceU8Usize_t rows = {frame->key};

	frame->op = txn_memtable_testing_put((const uint8 *) "v", 1);
	frame->op_live = true;
	change.op = &frame->op;
	rows.f1 = txn_memtable_version_encoded(&change.writer, change.op, change.shown, false, 0);
	frame->ok = txn_memtable_table_prepare_Handle(frame->io, frame->state, &rows, 1, NULL, 0, false,
												  txn_memtable_table_tests_windowed_closure_0, &change, NULL,
												  frame->result, frame->error);
	txn_memtable_table_tests_windowed_pad_0(frame);
}

/* txn_memtable_table_tests_windowed pushes a version of a key through a prepare whose window refuses every page allocation and record make. */
static bool
txn_memtable_table_tests_windowed(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len,
								  TxnMemtableVersionHandle *result, HostError *error)
{
	txn_memtable_table_tests_windowed_frame_0_t frame = {.io = io, .state = state, .result = result, .error = error};

	frame.key.ptr = key;
	frame.key.len = key_len;
	std_landing(&frame, txn_memtable_table_tests_windowed_body_0, txn_memtable_table_tests_windowed_pad_0);
	return frame.ok;
}

/* txn_memtable_table_tests_windowed_writer is a windowed case's writer: its numbered key's push through windowed. */
static bool
txn_memtable_table_tests_windowed_writer(void *captures, HostIoIo *io, HostError *error)
{
	const txn_memtable_table_tests_numbered_writer_t *c = captures;
	HostSharedHeld held;
	TxnMemtableVersionHandle handle;
	StdVec		key;
	bool		ok;

	if (!txn_memtable_testing_stated(io, c->database, &held, error))
		return false;
	txn_memtable_testing_numbered(c->number, &key);
	ok = txn_memtable_table_tests_windowed(io, host_shared_Held_record(&held), key.ptr, key.len, &handle, error);
	std_vec_free_Checked(&key, 1, 1);
	host_shared_Held_drop(&held);
	return ok;
}

/*
 * CommPF 5's case and the delta maps text's: a writer held between its
 * prepare and its window while another's push splits its range finds the
 * range closed in its finish, follows the replacement to the half that holds
 * its key, and pushes there with the reserve while every page allocation and
 * record make is refused; the splitter, the range's next finder, put the
 * halves in the list once.
 */
void
test_txn_memtable_table_tests_a_finish_after_a_split_pushes_into_its_half_with_nothing_taken(void)
{
	static const txn_memtable_table_tests_numbered_writer_t writer_captures = {9330, 25000};
	static const txn_memtable_table_tests_numbered_writer_t splitter_captures = {9330, TXN_MEMTABLE_SPLIT_SPLIT};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9330);
	HostSharedRecord state = host_shared_Held_record(&held);
	SimcActor	writer = {txn_memtable_table_tests_windowed_writer, (void *) &writer_captures};
	SimcActor	splitter = {txn_memtable_table_tests_numbered_writer, (void *) &splitter_captures};
	SimcStep	steps[2] = {{"splitter", 8, PG_UINT64_MAX}, {"writer", 6, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;
	HostError	error;
	StdVec		ranges;
	StdVec		key;
	StdTupleRef left;
	StdTupleRef right;
	TxnMemtableTableAt at;
	uint64		kept;
	uint64		zero = 0;
	uint64		upper;
	size_t		len;
	size_t		two = 2;
	bool		holds;
	bool		yes = true;

	txn_memtable_table_tests_prefills(2760, 38, io, state);
	run = simc_Run_new(131);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_actor(&run, "splitter", 8, splitter);
	simc_Run_until(&run, "writer", 6, "doltgres-memtable-prepared");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	simc_Report_drop(&report);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(2773, 56, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	len = ranges.len;
	left = STD_TUPLE(2, STD_ITEM(&len, std_usize_Debug_fmt), STD_ITEM(&kept, std_u64_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&two, std_usize_Debug_fmt), STD_ITEM(&zero, std_u64_Debug_fmt));
	STD_ASSERT_EQ(2774, 9, len == 2 && kept == 0, &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
	txn_memtable_testing_numbered(25000, &key);
	at = txn_memtable_table_tests_held_at(2775, 72, io, state, key.ptr, key.len, &holds);
	std_vec_free_Checked(&key, 1, 1);
	upper = txn_memtable_table_tests_range_at(&ranges, 1, 2776, 40);
	left = STD_TUPLE(2, STD_ITEM(&at.range, std_u64_Debug_fmt), STD_ITEM(&holds, std_bool_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&upper, std_u64_Debug_fmt), STD_ITEM(&yes, std_bool_Debug_fmt));
	STD_ASSERT_EQ(2776, 9, at.range == upper && holds, &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_twice_splitter is the second split case's splitter: SPLIT's push, then 16384 keys from 40000. */
static bool
txn_memtable_table_tests_twice_splitter(void *captures, HostIoIo *io, HostError *error)
{
	HostSharedHeld held;
	bool		ok = true;

	if (!txn_memtable_testing_stated(io, 9331, &held, error))
		return false;
	for (uint64 k = 0; ok && k <= 16384; k++)
	{
		TxnMemtableVersionHandle handle;
		StdVec		key;

		txn_memtable_testing_numbered(k == 0 ? TXN_MEMTABLE_SPLIT_SPLIT : 40000 + k - 1, &key);
		ok = txn_memtable_table_tests_written(io, host_shared_Held_record(&held), key.ptr, key.len, &handle, error);
		std_vec_free_Checked(&key, 1, 1);
	}
	host_shared_Held_drop(&held);
	return ok;
}

/*
 * The text's second split case: a writer held between its prepare and its
 * window while its range splits and the half that holds its key splits again
 * follows the chain of replacements in its finish to the open range that
 * holds its key, with nothing taken.
 */
void
test_txn_memtable_table_tests_a_finish_after_two_splits_follows_the_chain_to_an_open_range(void)
{
	static const txn_memtable_table_tests_numbered_writer_t writer_captures = {9331, 90000};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9331);
	HostSharedRecord state = host_shared_Held_record(&held);
	SimcActor	writer = {txn_memtable_table_tests_windowed_writer, (void *) &writer_captures};
	SimcActor	splitter = {txn_memtable_table_tests_twice_splitter};
	SimcStep	steps[2] = {{"splitter", 8, PG_UINT64_MAX}, {"writer", 6, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;
	HostError	error;
	StdVec		ranges;
	StdVec		key;
	StdTupleRef left;
	StdTupleRef right;
	TxnMemtableTableAt at;
	uint64		kept;
	uint64		upper;
	size_t		len;
	size_t		three = 3;
	bool		holds;
	bool		yes = true;

	txn_memtable_table_tests_prefills(2788, 38, io, state);
	run = simc_Run_new(137);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_actor(&run, "splitter", 8, splitter);
	simc_Run_until(&run, "writer", 6, "doltgres-memtable-prepared");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	simc_Report_drop(&report);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(2804, 53, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	len = ranges.len;
	STD_ASSERT_EQ(2805, 9, len == 3, &len, std_usize_Debug_fmt, &three, std_usize_Debug_fmt);
	txn_memtable_testing_numbered(90000, &key);
	at = txn_memtable_table_tests_held_at(2806, 72, io, state, key.ptr, key.len, &holds);
	std_vec_free_Checked(&key, 1, 1);
	upper = txn_memtable_table_tests_range_at(&ranges, 2, 2807, 40);
	left = STD_TUPLE(2, STD_ITEM(&at.range, std_u64_Debug_fmt), STD_ITEM(&holds, std_bool_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&upper, std_u64_Debug_fmt), STD_ITEM(&yes, std_bool_Debug_fmt));
	STD_ASSERT_EQ(2807, 9, at.range == upper && holds, &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_unfinished is the pin case's prepare's closure, which finishes nothing. */
static bool
txn_memtable_table_tests_unfinished(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
									HostError *error)
{
	return true;
}

/*
 * The pin's case: a prepare that makes a storage's memtable and whose window
 * finishes nothing leaves the memtable to its settle, its last unpinner,
 * which closes it, so the state is reclaimed and the delta's use returns to
 * its start.
 */
void
test_txn_memtable_table_tests_a_memtable_no_finish_reached_is_closed_by_its_last_unpinner(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9332);
	txn_TupleRefSliceU8Usize_t rows[1] = {{{(const uint8 *) "k", 1}, 64}};
	StdOptionRef left;
	StdOptionRef none = {false};
	HostError	error;
	uint64		location;
	bool		freed;

	if (!txn_memtable_table_prepare_Unit(io, host_shared_Held_record(&held), rows, 1, NULL, 0, false,
										 txn_memtable_table_tests_unfinished, NULL, NULL, &error))
		STD_RESULT_EXPECT_FAILED(2820, 83, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = txn_memtable_table_tests_has_memtable(host_shared_Held_record(&held), &location);
	left.some = STD_ITEM(&location, std_u64_Debug_fmt);
	STD_ASSERT_EQ(2821, 9, !left.has, &left, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	host_shared_Held_drop(&held);
	if (!TXN_STATE_STATE_POOL_OPS.reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(2823, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2823, 9, freed, "State::reclaim(io).unwrap()");
	txn_memtable_table_tests_used_is(2825, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/* The map's takes that keep a bool, as the first insert case's step keeps whether its key had no entry. */
#define TXN_MEMTABLE_MAP_TS Bool
#define TXN_MEMTABLE_MAP_KEPT_OUT , bool *kept
#define TXN_MEMTABLE_MAP_KEPT_IN , const bool *kept
#define TXN_MEMTABLE_MAP_DECLARE
#define TXN_MEMTABLE_MAP_DEFINE
#define TXN_MEMTABLE_MAP_KEPT_ARG kept
#define TXN_MEMTABLE_MAP_KEPT_CALL(k) , (k)
#define TXN_MEMTABLE_MAP_KEPT_DROP(k)
#include "doltgres/txn/memtable/map_template.h"

/* The prepare whose closure returns the first insert case's ((bool, bool), Taken<bool>). */
#define TXN_MEMTABLE_TABLE_RS MadeTaken
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , bool *result_0, bool *result_1, TxnMemtableMapTaken *result_2, \
	bool *result_3, HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#define TXN_MEMTABLE_TABLE_DEFINE
#define TXN_MEMTABLE_TABLE_RESULT_FIELDS bool *result_0; bool *result_1; TxnMemtableMapTaken *result_2; \
	bool *result_3;
#define TXN_MEMTABLE_TABLE_RESULT_INIT , result_0, result_1, result_2, result_3
#define TXN_MEMTABLE_TABLE_RESULT_ARGS(c) , (c)->result_0, (c)->result_1, (c)->result_2, (c)->result_3
#define TXN_MEMTABLE_TABLE_RESULT_ERROR error
#include "doltgres/txn/memtable/table_template.h"

/* txn_memtable_table_tests_made_t is the first insert case's ((bool, bool), Taken<bool>): its memtables made, and its take. */
typedef struct txn_memtable_table_tests_made_t
{
	bool		rows;
	bool		free;
	TxnMemtableMapTaken taken;
	bool		took;
} txn_memtable_table_tests_made_t;

/* txn_memtable_table_tests_taken_bool_Debug_fmt is a Taken<bool>'s Debug, of a made_t's take. */
static bool
txn_memtable_table_tests_taken_bool_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_table_tests_made_t *made = value;
	StdDebugTuple tuple;

	switch (made->taken)
	{
		case TXN_MEMTABLE_MAP_TAKEN_TOOK:
			tuple = std_fmt_debug_tuple(f, "Took", 4);
			std_DebugTuple_field(&tuple, &made->took, std_bool_Debug_fmt);
			return std_DebugTuple_finish(&tuple);
		case TXN_MEMTABLE_MAP_TAKEN_RETRY:
			return std_fmt_write_str(f, "Retry", 5);
		case TXN_MEMTABLE_MAP_TAKEN_MOVED:
			return std_fmt_write_str(f, "Moved", 5);
		default:
			return std_fmt_write_str(f, "Refused", 7);
	}
}

/* txn_memtable_table_tests_made_Debug_fmt is a ((bool, bool), Taken<bool>)'s Debug, of a made_t. */
static bool
txn_memtable_table_tests_made_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_table_tests_made_t *made = value;
	StdTupleRef memtables = STD_TUPLE(2, STD_ITEM(&made->rows, std_bool_Debug_fmt),
									  STD_ITEM(&made->free, std_bool_Debug_fmt));
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&memtables, std_tuple_Debug_fmt),
								  STD_ITEM(made, txn_memtable_table_tests_taken_bool_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_memtable_table_tests_free_step is the first insert case's step: a put of "free", keeping whether the key had no entry. */
static TxnMemtableMapStep
txn_memtable_table_tests_free_step(void *captures, bool has_current, const uint8 *current, size_t current_len,
								   StdVec *out, bool *kept)
{
	std_vec_extend_from_slice_Checked(out, (const uint8 *) "free", 4, 1, 1);
	*kept = !has_current;
	return TXN_MEMTABLE_MAP_STEP_PUT;
}

/* txn_memtable_table_tests_free_then is the first insert case's then, which does nothing. */
static void
txn_memtable_table_tests_free_then(void *captures, const bool *kept)
{
}

/* txn_memtable_table_tests_first_insert_t is the first insert case's window's captures: its storage and its row's change. */
typedef struct txn_memtable_table_tests_first_insert_t
{
	HostSharedRecord state;
	const TxnMemtableVersionChange *change;
} txn_memtable_table_tests_first_insert_t;

/* txn_memtable_table_tests_first_insert_inside is the first insert case's window: its memtables seen, the map's take, and the row's push. */
static bool
txn_memtable_table_tests_first_insert_inside(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
											 bool *result_0, bool *result_1, TxnMemtableMapTaken *result_2,
											 bool *result_3, HostError *error)
{
	const txn_memtable_table_tests_first_insert_t *c = captures;
	TxnStatusRoomed roomed = {0};
	TxnMemtableVersionHandle handle;
	uint64		location;

	*result_0 = txn_memtable_table_memtable(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, c->state, &location);
	*result_1 = txn_memtable_table_memtable(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, c->state, &location);
	return txn_memtable_map_finish_Bool(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, txn_memtable_testing_writer(7), false,
										roomed, false,
										0, false, NULL, 0, NULL, txn_memtable_table_tests_free_step, NULL, NULL,
										txn_memtable_table_tests_free_then, NULL, NULL, result_2, result_3, error) &&
		txn_memtable_table_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, c->change, false, roomed, &handle, error);
}

/*
 * The combined prepare's ordering case: a first insert into a new table
 * makes its rows' and its free space map's memtables before its hold, and
 * its window takes the map's entry and pushes the row.
 */
void
test_txn_memtable_table_tests_a_first_insert_makes_its_memtables_before_its_hold(void)
{
	static const txn_memtable_table_tests_made_t expected = {true, true, TXN_MEMTABLE_MAP_TAKEN_TOOK, true};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9333);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	txn_TupleRefSliceU8Usize_t rows[1] = {{{(const uint8 *) "k", 1}}};
	TxnMemtableMapTake take = {.state = state, .map = TXN_MEMTABLE_MAP_MAP_FREE, .key = 7, .most = 8};
	txn_memtable_table_tests_first_insert_t inside = {state, &change};
	txn_memtable_table_tests_made_t made = {0};
	TxnMemtableMapEntry free_entry = {.has_value = true, .value = {.ptr = (void *) "free", .len = 4, .cap = 4}};
	HostError	error;
	bool		holds;

	rows[0].f1 = txn_memtable_version_encoded(&change.writer, change.op, change.shown, false, 0);
	if (!txn_memtable_table_prepare_MadeTaken(io, state, rows, 1, &take, 1, false,
											  txn_memtable_table_tests_first_insert_inside, &inside, NULL, &made.rows,
											  &made.free, &made.taken, &made.took, &error))
		STD_RESULT_EXPECT_FAILED(2851, 10, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(2852, 9, made.rows && made.free && made.taken == TXN_MEMTABLE_MAP_TAKEN_TOOK && made.took, &made,
				  txn_memtable_table_tests_made_Debug_fmt, &expected, txn_memtable_table_tests_made_Debug_fmt);
	txn_memtable_map_tests_newest_is(2853, 9, 2853, 57, io, state, TXN_MEMTABLE_MAP_MAP_FREE, 7, true, &free_entry);
	(void) txn_memtable_table_tests_held_at(2854, 41, io, state, (const uint8 *) "k", 1, &holds);
	STD_ASSERT(2854, 9, holds, "holder(io, state, b\"k\").unwrap().1");
	txn_memtable_Op_drop(&op);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_exiter_t is a terminated backend's captures: its database, and whether it takes its free space map's entry 7. */
typedef struct txn_memtable_table_tests_exiter_t
{
	uint32		database;
	bool		take;
} txn_memtable_table_tests_exiter_t;

/* txn_memtable_table_tests_exiter_frame_0_t is the exiter's state across its pad: its arguments, and its storage's state, while held. */
typedef struct txn_memtable_table_tests_exiter_frame_0_t
{
	const txn_memtable_table_tests_exiter_t *captures;
	HostIoIo   *io;
	HostError  *error;
	bool		ok;
	HostSharedHeld held;
	bool		held_live;
} txn_memtable_table_tests_exiter_frame_0_t;

/* txn_memtable_table_tests_exiter_pad_0 is the exiter's pad: its storage's state, while held. */
static void
txn_memtable_table_tests_exiter_pad_0(void *frame_)
{
	txn_memtable_table_tests_exiter_frame_0_t *frame = frame_;

	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
}

/* txn_memtable_table_tests_exiter_window is the exiter's window: it pauses, then spins until its termination. */
static bool
txn_memtable_table_tests_exiter_window(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
									   HostError *error)
{
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-memtable-case-window", error))
		return false;
	for (;;)
		(void) host_unique();
}

/* txn_memtable_table_tests_exiter_body_0 is the exiter's body. */
static void
txn_memtable_table_tests_exiter_body_0(void *frame_)
{
	txn_memtable_table_tests_exiter_frame_0_t *frame = frame_;
	txn_TupleRefSliceU8Usize_t rows[1] = {{{(const uint8 *) "k", 1}, 64}};
	TxnMemtableMapTake take = {.map = TXN_MEMTABLE_MAP_MAP_FREE, .key = 7, .most = 8};

	if (!txn_memtable_testing_stated(frame->io, frame->captures->database, &frame->held, frame->error))
		return;
	frame->held_live = true;
	take.state = host_shared_Held_record(&frame->held);
	frame->ok = txn_memtable_table_prepare_Unit(frame->io, host_shared_Held_record(&frame->held), rows, 1,
												frame->captures->take ? &take : NULL, frame->captures->take ? 1 : 0,
												false, txn_memtable_table_tests_exiter_window, NULL, NULL,
												frame->error);
	txn_memtable_table_tests_exiter_pad_0(frame);
}

/* txn_memtable_table_tests_exiter is a terminated backend: its prepare's window spins until its termination. */
static bool
txn_memtable_table_tests_exiter(void *captures, HostIoIo *io, HostError *error)
{
	txn_memtable_table_tests_exiter_frame_0_t frame = {.captures = captures, .io = io, .error = error};

	std_landing(&frame, txn_memtable_table_tests_exiter_body_0, txn_memtable_table_tests_exiter_pad_0);
	return frame.ok;
}

/*
 * F2's case: a backend that exits between its prepare and its settle leaves
 * its memtable open, empty, and pinned by no slot, since its exit zeroes its
 * words, and the reclaim closes it and then frees the state, so the delta's
 * use returns to its start.
 */
void
test_txn_memtable_table_tests_a_backend_that_exits_inside_its_window_leaves_the_close_to_the_reclaim(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	static const txn_memtable_table_tests_exiter_t exiter_captures = {9334, false};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9334);
	SimcRun		run;
	SimcActor	exiter = {txn_memtable_table_tests_exiter, (void *) &exiter_captures};
	SimcReport	report;
	SimcEnding	terminated = {.tag = SIMC_ENDING_TERMINATED};
	const SimcEnding *first;
	HostError	error;
	uint64		location;
	bool		freed;

	run = simc_Run_new(139);
	simc_Run_actor(&run, "exiter", 6, exiter);
	simc_Run_until(&run, "exiter", 6, "doltgres-memtable-case-window");
	simc_Run_terminate_at(&run, "exiter", 6, 5000);
	report = simc_Run_finish(run);
	if (report.outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtArg	args[1] = {{&report.outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 2880, 67, &message);
	}
	std_index_check(0, report.outcome.u.ended.len, RUST_FILE, 2881, 20);
	first = &STD_VEC_AT(&report.outcome.u.ended, simc_TupleStringEnding_t, 0).f1;
	STD_ASSERT_EQ(2881, 9, simc_Ending_PartialEq_eq(first, &terminated), first, simc_Ending_Debug_fmt,
				  &terminated, simc_Ending_Debug_fmt);
	STD_ASSERT(2882, 9, txn_memtable_table_tests_has_memtable(host_shared_Held_record(&held), &location),
			   "memtable::<Rows>(held.record()).is_some()");
	host_shared_Held_drop(&held);
	for (int k = 0; k < 2; k++)
	{
		if (!TXN_STATE_STATE_POOL_OPS.reclaim(io, &freed, &error))
			STD_RESULT_EXPECT_FAILED(2884 + k, 36, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(2884 + k, 9, freed, "State::reclaim(io).unwrap()");
	}
	txn_memtable_table_tests_used_is(2887, 9, io, start);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_abandoner_t is a pin race's closer's captures: its database and the memtable it looks at. */
typedef struct txn_memtable_table_tests_abandoner_t
{
	uint32		database;
	uint64		location;
} txn_memtable_table_tests_abandoner_t;

/* txn_memtable_table_tests_abandoner is a pin race's closer: its look at its storage's rows' memtable at a location. */
static bool
txn_memtable_table_tests_abandoner(void *captures, HostIoIo *io, HostError *error)
{
	const txn_memtable_table_tests_abandoner_t *c = captures;
	HostSharedHeld held;
	bool		closed;
	bool		ok;

	if (!txn_memtable_testing_stated(io, c->database, &held, error))
		return false;
	ok = txn_memtable_table_abandoned(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, host_shared_Held_record(&held), c->location,
									  &closed, error);
	host_shared_Held_drop(&held);
	return ok;
}

/* txn_memtable_table_tests_first_location makes a storage's rows' first memtable and returns its location, as first's and memtable's unwraps do at their places. */
static uint64
txn_memtable_table_tests_first_location(uint32 line, HostIoIo *io, HostSharedRecord state)
{
	uint64		location;

	txn_memtable_table_tests_firsted(line, 42, io, state);
	if (!txn_memtable_table_tests_has_memtable(state, &location))
		STD_OPTION_UNWRAP_FAILED(line + 1, 56);
	return location;
}

/*
 * The pin race's first case: a writer held between its pin's store and its
 * take of its range's lock, while a closer looks at the memtable it pinned,
 * which holds nothing, leaves the memtable open, since the closer reads the
 * pin, and the writer pushes into it.
 */
void
test_txn_memtable_table_tests_a_closer_that_reads_a_prepares_pin_leaves_its_memtable_open(void)
{
	static const txn_memtable_table_tests_keyed_writer_t writer_captures = {9335, "k"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9335);
	HostSharedRecord state = host_shared_Held_record(&held);
	txn_memtable_table_tests_abandoner_t closer_captures = {9335};
	SimcActor	writer = {txn_memtable_table_tests_keyed_writer, (void *) &writer_captures};
	SimcActor	closer = {txn_memtable_table_tests_abandoner, &closer_captures};
	SimcStep	steps[2] = {{"closer", 6, PG_UINT64_MAX}, {"writer", 6, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;
	TxnMemtableTableAt at;
	StdTupleRef left;
	StdTupleRef right;
	bool		holds;
	bool		yes = true;

	closer_captures.location = txn_memtable_table_tests_first_location(2899, io, state);
	run = simc_Run_new(149);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_actor(&run, "closer", 6, closer);
	simc_Run_until(&run, "writer", 6, "doltgres-memtable-found");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	simc_Report_drop(&report);
	at = txn_memtable_table_tests_held_at(2913, 59, io, state, (const uint8 *) "k", 1, &holds);
	left = STD_TUPLE(2, STD_ITEM(&at.memtable, std_u64_Debug_fmt), STD_ITEM(&holds, std_bool_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&closer_captures.location, std_u64_Debug_fmt), STD_ITEM(&yes, std_bool_Debug_fmt));
	STD_ASSERT_EQ(2914, 9, at.memtable == closer_captures.location && holds, &left, std_tuple_Debug_fmt, &right,
				  std_tuple_Debug_fmt);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * The pin race's second case: a closer that read no pin and closed the
 * memtable's only range, held before it takes the range out, while a writer
 * pins the memtable, finds its range closed after the store, and goes
 * around, writes into the storage's next memtable.
 */
void
test_txn_memtable_table_tests_a_prepare_that_pins_after_a_closers_read_goes_around(void)
{
	static const txn_memtable_table_tests_keyed_writer_t writer_captures = {9336, "k"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9336);
	HostSharedRecord state = host_shared_Held_record(&held);
	txn_memtable_table_tests_abandoner_t closer_captures = {9336};
	SimcActor	closer = {txn_memtable_table_tests_abandoner, &closer_captures};
	SimcActor	writer = {txn_memtable_table_tests_keyed_writer, (void *) &writer_captures};
	SimcStep	steps[2] = {{"writer", 6, PG_UINT64_MAX}, {"closer", 6, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;
	TxnMemtableTableAt at;
	bool		holds;

	closer_captures.location = txn_memtable_table_tests_first_location(2926, io, state);
	run = simc_Run_new(151);
	simc_Run_actor(&run, "closer", 6, closer);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_until(&run, "closer", 6, "doltgres-memtable-unlisting");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	simc_Report_drop(&report);
	at = txn_memtable_table_tests_held_at(2940, 59, io, state, (const uint8 *) "k", 1, &holds);
	STD_ASSERT(2941, 9, holds, "holds");
	if (at.memtable == closer_captures.location)
		std_assert_failed(RUST_FILE, 2942, 9, "!=", &at.memtable, std_u64_Debug_fmt, &closer_captures.location,
						  std_u64_Debug_fmt, NULL);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_dropper is the dropper case's dropper: its prepare of key k, whose window finishes nothing. */
static bool
txn_memtable_table_tests_dropper(void *captures, HostIoIo *io, HostError *error)
{
	txn_TupleRefSliceU8Usize_t rows[1] = {{{(const uint8 *) "k", 1}, 64}};
	HostSharedHeld held;
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9337, &held, error))
		return false;
	ok = txn_memtable_table_prepare_Unit(io, host_shared_Held_record(&held), rows, 1, NULL, 0, false,
										 txn_memtable_table_tests_unfinished, NULL, NULL, error);
	host_shared_Held_drop(&held);
	return ok;
}

/* txn_memtable_table_tests_dropper_closer is the dropper case's closer: its look at the memtable the dropper made. */
static bool
txn_memtable_table_tests_dropper_closer(void *captures, HostIoIo *io, HostError *error)
{
	HostSharedHeld held;
	uint64		location;
	bool		closed;
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9337, &held, error))
		return false;
	if (!txn_memtable_table_tests_has_memtable(host_shared_Held_record(&held), &location))
	{
		*error = txn_memtable_testing_failed("the dropper made no memtable");
		host_shared_Held_drop(&held);
		return false;
	}
	ok = txn_memtable_table_abandoned(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, host_shared_Held_record(&held), location,
									  &closed, error);
	host_shared_Held_drop(&held);
	return ok;
}

/*
 * PRevPF 5's dropper case: a prepare whose window finished nothing, held
 * between its settle's clear of its pin and its check of the memtable, while
 * a closer looks at the memtable, has it closed once, by the closer, which
 * reads no pin, so the state is reclaimed and the delta's use returns to its
 * start.
 */
void
test_txn_memtable_table_tests_a_dropper_held_after_its_clear_and_a_closer_close_the_memtable_once(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9337);
	SimcActor	dropper = {txn_memtable_table_tests_dropper};
	SimcActor	closer = {txn_memtable_table_tests_dropper_closer};
	SimcStep	steps[2] = {{"closer", 6, PG_UINT64_MAX}, {"dropper", 7, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;
	StdOptionRef left;
	StdOptionRef none = {false};
	HostError	error;
	uint64		location;
	bool		freed;

	run = simc_Run_new(157);
	simc_Run_actor(&run, "dropper", 7, dropper);
	simc_Run_actor(&run, "closer", 6, closer);
	simc_Run_until(&run, "dropper", 7, "doltgres-memtable-unpinned");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	simc_Report_drop(&report);
	left.has = txn_memtable_table_tests_has_memtable(host_shared_Held_record(&held), &location);
	left.some = STD_ITEM(&location, std_u64_Debug_fmt);
	STD_ASSERT_EQ(2968, 9, !left.has, &left, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	host_shared_Held_drop(&held);
	if (!TXN_STATE_STATE_POOL_OPS.reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(2970, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(2970, 9, freed, "State::reclaim(io).unwrap()");
	txn_memtable_table_tests_used_is(2972, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_refusing_close is the refused close case's window: a refusal of the memtable's record's next open. */
static bool
txn_memtable_table_tests_refusing_close(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
										HostError *error)
{
	const HostSharedRecord *state = captures;
	uint64		location;

	if (!txn_memtable_table_tests_has_memtable(*state, &location))
	{
		*error = txn_memtable_testing_failed("no memtable");
		return false;
	}
	simc_refuse_record(TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape.id, location, 0);
	return true;
}

/*
 * The pin's refused close: a last unpinner whose close can't open the
 * memtable returns the error and leaves the memtable open and empty, which
 * the reclaim closes, then freeing the state, so the delta's use returns to
 * its start.
 */
void
test_txn_memtable_table_tests_an_unpinners_refused_close_leaves_the_memtable_to_the_reclaim(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9338);
	HostSharedRecord state = host_shared_Held_record(&held);
	txn_TupleRefSliceU8Usize_t rows[1] = {{{(const uint8 *) "k", 1}, 64}};
	StdBytes	sqlstate;
	StdBytes	internal = {(const uint8 *) "XX000", 5};
	HostError	error;
	uint64		location;
	bool		freed;

	if (txn_memtable_table_prepare_Unit(io, state, rows, 1, NULL, 0, false, txn_memtable_table_tests_refusing_close,
										&state, NULL, &error))
	{
		char		unit = 0;

		STD_RESULT_EXPECT_FAILED(2990, 10, "called `Result::unwrap_err()` on an `Ok` value", &unit,
								 std_unit_Debug_fmt);
	}
	sqlstate.ptr = host_Error_sqlstate(&error);
	sqlstate.len = 5;
	STD_ASSERT_EQ(2991, 9, memcmp(sqlstate.ptr, "XX000", 5) == 0, &sqlstate, std_bytes_Debug_fmt, &internal,
				  std_bytes_Debug_fmt);
	host_Error_drop(&error);
	STD_ASSERT(2992, 9, txn_memtable_table_tests_has_memtable(state, &location), "memtable::<Rows>(state).is_some()");
	host_shared_Held_drop(&held);
	for (int k = 0; k < 2; k++)
	{
		if (!TXN_STATE_STATE_POOL_OPS.reclaim(io, &freed, &error))
			STD_RESULT_EXPECT_FAILED(2994 + k, 36, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT(2994 + k, 9, freed, "State::reclaim(io).unwrap()");
	}
	txn_memtable_table_tests_used_is(2997, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_due loads a storage's mark of a fold due. */
static uint64
txn_memtable_table_tests_due(HostSharedRecord state)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

	return host_shared_Record_atomic(state, TXN_STATE_FOLD_DUE_INDEX, load);
}

/*
 * The reclaim's maps case: a backend that exits inside its window after its
 * prepare made a new table's memtables, its rows' and its free space map's,
 * leaves the map linked, so the reclaim keeps the state and marks the
 * storage due for a fold, which folds the map.
 */
void
test_txn_memtable_table_tests_a_reclaim_keeps_a_state_whose_maps_are_linked_and_marks_it_due(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	static const txn_memtable_table_tests_exiter_t exiter_captures = {9339, true};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9339);
	SimcRun		run;
	SimcActor	exiter = {txn_memtable_table_tests_exiter, (void *) &exiter_captures};
	SimcReport	report;
	SimcEnding	terminated = {.tag = SIMC_ENDING_TERMINATED};
	const SimcEnding *first;
	HostSharedPast word;
	HostError	error;
	StdOptionRef was;
	StdOptionRef now;
	StdTupleRef left;
	StdTupleRef right;
	uint64		location;
	uint64		now_location;
	uint64		due;
	uint64		zero = 0;
	uint64		one = 1;
	bool		freed;

	run = simc_Run_new(173);
	simc_Run_actor(&run, "exiter", 6, exiter);
	simc_Run_until(&run, "exiter", 6, "doltgres-memtable-case-window");
	simc_Run_terminate_at(&run, "exiter", 6, 5000);
	report = simc_Run_finish(run);
	if (report.outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtArg	args[1] = {{&report.outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 3028, 67, &message);
	}
	std_index_check(0, report.outcome.u.ended.len, RUST_FILE, 3029, 20);
	first = &STD_VEC_AT(&report.outcome.u.ended, simc_TupleStringEnding_t, 0).f1;
	STD_ASSERT_EQ(3029, 9, simc_Ending_PartialEq_eq(first, &terminated), first, simc_Ending_Debug_fmt,
				  &terminated, simc_Ending_Debug_fmt);
	STD_ASSERT(3030, 9, txn_memtable_table_memtable(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, host_shared_Held_record(&held),
													&location), "memtable::<Free>(held.record()).is_some()");
	due = txn_memtable_table_tests_due(host_shared_Held_record(&held));
	STD_ASSERT_EQ(3031, 9, due == 0, &due, std_u64_Debug_fmt, &zero, std_u64_Debug_fmt);
	word = host_shared_Record_names_past(host_shared_Held_record(&held), TXN_STATE_MEMTABLE_WORD_INDEX);
	was.has = host_shared_Past_load(&word, &location);
	was.some = STD_ITEM(&location, std_u64_Debug_fmt);
	host_shared_Held_drop(&held);
	if (!TXN_STATE_STATE_POOL_OPS.reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(3034, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(3034, 9, !freed, "!State::reclaim(io).unwrap()");
	if (!txn_memtable_testing_stated(io, 9339, &held, &error))
		STD_RESULT_EXPECT_FAILED(3035, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	due = txn_memtable_table_tests_due(host_shared_Held_record(&held));
	word = host_shared_Record_names_past(host_shared_Held_record(&held), TXN_STATE_MEMTABLE_WORD_INDEX);
	now.has = host_shared_Past_load(&word, &now_location);
	now.some = STD_ITEM(&now_location, std_u64_Debug_fmt);
	left = STD_TUPLE(2, STD_ITEM(&due, std_u64_Debug_fmt), STD_ITEM(&now, std_option_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&one, std_u64_Debug_fmt), STD_ITEM(&was, std_option_Debug_fmt));
	STD_ASSERT_EQ(3036, 9, due == 1 && now.has == was.has && (!now.has || now_location == location), &left,
				  std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
	host_shared_Held_drop(&held);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/*
 * The hand-off's maps case: a hand-off of a storage's rows that finds no
 * memtable of them while a map is linked marks the storage due for a fold.
 */
void
test_txn_memtable_table_tests_a_rows_hand_off_that_meets_linked_maps_marks_a_fold_due(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9345);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostDeltaHold hold;
	HostError	error;
	TxnMemtableTableAt at;
	StdOptionRef left;
	StdOptionRef none = {false};
	uint64		due;
	uint64		one = 1;
	bool		some;

	if (!txn_memtable_table_first(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, state, &error))
		STD_RESULT_EXPECT_FAILED(3047, 42, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(3048, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_table_found(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, &hold, state, (const uint8 *) "k", 1, NULL,
								  &some, &at, &error))
		STD_RESULT_EXPECT_FAILED(3049, 72, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = some;
	left.some = STD_ITEM(&at, txn_memtable_table_tests_At_Debug_fmt);
	STD_ASSERT_EQ(3049, 9, !some, &left, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	due = txn_memtable_table_tests_due(state);
	STD_ASSERT_EQ(3050, 9, due == 1, &due, std_u64_Debug_fmt, &one, std_u64_Debug_fmt);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_longer is the finish's guard case's window: its row's finish, longer than its prepare's reserve. */
static bool
txn_memtable_table_tests_longer(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
								TxnMemtableVersionHandle *result, HostError *error)
{
	const TxnMemtableVersionChange *change = captures;
	TxnStatusRoomed roomed = {0};

	return txn_memtable_table_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, change, false, roomed, result, error);
}

/*
 * The finish's guard: a change longer than the record its prepare reserved
 * for is refused with an internal error, and its key is left unwritten.
 */
void
test_txn_memtable_table_tests_a_finish_longer_than_its_prepare_is_refused(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9346);
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	txn_TupleRefSliceU8Usize_t rows[1] = {{{(const uint8 *) "k", 1}}};
	TxnMemtableVersionHandle handle;
	StdBytes	sqlstate;
	StdBytes	internal = {(const uint8 *) "XX000", 5};
	HostError	error;
	bool		holds;

	rows[0].f1 = STD_SUB(usize, txn_memtable_version_encoded(&change.writer, change.op, change.shown, false, 0), 1,
						 3063, 20);
	if (txn_memtable_table_prepare_Handle(io, host_shared_Held_record(&held), rows, 1, NULL, 0, false,
										  txn_memtable_table_tests_longer, &change, NULL, &handle, &error))
		STD_RESULT_EXPECT_FAILED(3067, 10, "called `Result::unwrap_err()` on an `Ok` value", &handle,
								 txn_memtable_version_Handle_Debug_fmt);
	sqlstate.ptr = host_Error_sqlstate(&error);
	sqlstate.len = 5;
	STD_ASSERT_EQ(3068, 9, memcmp(sqlstate.ptr, "XX000", 5) == 0, &sqlstate, std_bytes_Debug_fmt, &internal,
				  std_bytes_Debug_fmt);
	host_Error_drop(&error);
	(void) txn_memtable_table_tests_held_at(3069, 50, io, host_shared_Held_record(&held), (const uint8 *) "k", 1,
											&holds);
	STD_ASSERT(3069, 9, !holds, "!holder(io, held.record(), b\"k\").unwrap().1");
	txn_memtable_Op_drop(&op);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * The close's emptiness: a memtable whose one range holds only the empty
 * key, with a version of it, holds something, so a closer leaves it open
 * with its version.
 */
void
test_txn_memtable_table_tests_a_memtable_whose_empty_key_holds_a_version_stays_open(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9347);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableTableAt at;
	StdTupleRef left;
	StdTupleRef right;
	HostError	error;
	uint64		location;
	bool		closed;
	bool		holds;
	bool		yes = true;

	(void) txn_memtable_table_tests_writes(3080, 41, io, state, (const uint8 *) "", 0);
	if (!txn_memtable_table_tests_has_memtable(state, &location))
		STD_OPTION_UNWRAP_FAILED(3081, 56);
	if (!txn_memtable_table_abandoned(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, location, &closed, &error))
		STD_RESULT_EXPECT_FAILED(3082, 65, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(3082, 9, !closed, "!abandoned::<Rows>(io, held.record(), location).unwrap()");
	at = txn_memtable_table_tests_held_at(3083, 58, io, state, (const uint8 *) "", 0, &holds);
	left = STD_TUPLE(2, STD_ITEM(&at.memtable, std_u64_Debug_fmt), STD_ITEM(&holds, std_bool_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&location, std_u64_Debug_fmt), STD_ITEM(&yes, std_bool_Debug_fmt));
	STD_ASSERT_EQ(3084, 9, at.memtable == location && holds, &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * The reclaim's maps case with no memtable of rows: a state whose free space
 * map alone is linked is kept and marked due for a fold, with its map still
 * linked after.
 */
void
test_txn_memtable_table_tests_a_reclaim_keeps_a_state_whose_map_alone_is_linked(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9359);
	HostError	error;
	StdOptionRef was;
	StdOptionRef now;
	StdTupleRef left;
	StdTupleRef right;
	uint64		location;
	uint64		now_location;
	uint64		due;
	uint64		one = 1;
	bool		freed;

	if (!txn_memtable_table_first(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, io, host_shared_Held_record(&held), &error))
		STD_RESULT_EXPECT_FAILED(3095, 42, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	was.has = txn_memtable_table_memtable(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, host_shared_Held_record(&held), &location);
	was.some = STD_ITEM(&location, std_u64_Debug_fmt);
	host_shared_Held_drop(&held);
	if (!TXN_STATE_STATE_POOL_OPS.reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(3098, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(3098, 9, !freed, "!State::reclaim(io).unwrap()");
	if (!txn_memtable_testing_stated(io, 9359, &held, &error))
		STD_RESULT_EXPECT_FAILED(3099, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	due = txn_memtable_table_tests_due(host_shared_Held_record(&held));
	now.has = txn_memtable_table_memtable(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, host_shared_Held_record(&held),
										  &now_location);
	now.some = STD_ITEM(&now_location, std_u64_Debug_fmt);
	left = STD_TUPLE(2, STD_ITEM(&due, std_u64_Debug_fmt), STD_ITEM(&now, std_option_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&one, std_u64_Debug_fmt), STD_ITEM(&was, std_option_Debug_fmt));
	STD_ASSERT_EQ(3100, 9, due == 1 && now.has == was.has && (!now.has || now_location == location), &left,
				  std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * I12's test: a row change's version pushed with its record's end is held by
 * that LSN, not by another, and a key with no version isn't held.
 */
void
test_txn_memtable_table_tests_a_rows_version_is_held_by_its_records_lsn(void)
{
	static const char *const keys[3] = {"k", "k", "other"};
	static const uint64 lsns[3] = {UINT64CONST(0x100000a28), UINT64CONST(0x100000a29), UINT64CONST(0x100000a28)};
	static const bool expected[3] = {true, false, false};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9361);
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND},
	.has_lsn = true, .lsn = UINT64CONST(0x100000a28)};
	TxnMemtableVersionHandle handle;
	HostError	error;
	bool		found[3];

	if (!txn_memtable_table_push(io, host_shared_Held_record(&held), (const uint8 *) "k", 1, &change, &handle, &error))
		STD_RESULT_EXPECT_FAILED(3119, 48, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(keys); k++)
		if (!txn_memtable_table_holds(io, host_shared_Held_record(&held), (const uint8 *) keys[k], strlen(keys[k]),
									  lsns[k], &found[k], &error))
			STD_RESULT_EXPECT_FAILED(3121, 66, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	STD_ASSERT_EQ(3122, 9, found[0] == expected[0] && found[1] == expected[1] && found[2] == expected[2], found,
				  txn_memtable_branch_tests_held_bools_Debug_fmt, expected,
				  txn_memtable_branch_tests_held_bools_Debug_fmt);
	txn_memtable_Op_drop(&op);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_handed_t is a walk's handed key: its bytes and its count of records, a (Vec<u8>, usize). */
typedef struct txn_memtable_table_tests_handed_t
{
	StdVec		key;
	size_t		records;
} txn_memtable_table_tests_handed_t;

/* txn_memtable_table_tests_handed_Debug_fmt is a Vec<(Vec<u8>, usize)>'s Debug, of handed_t items. */
static bool
txn_memtable_table_tests_handed_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
	{
		const txn_memtable_table_tests_handed_t *handed = &STD_VEC_AT(v, txn_memtable_table_tests_handed_t, k);
		StdBytes	key = {handed->key.ptr, handed->key.len};
		StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&key, std_bytes_Debug_fmt),
									  STD_ITEM(&handed->records, std_usize_Debug_fmt));

		std_DebugList_entry(&list, &tuple, std_tuple_Debug_fmt);
	}
	return std_DebugList_finish(&list);
}

/* txn_memtable_table_tests_handed_eq reports whether two Vec<(Vec<u8>, usize)>s are equal. */
static bool
txn_memtable_table_tests_handed_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t k = 0; k < a->len; k++)
	{
		const txn_memtable_table_tests_handed_t *x = &STD_VEC_AT(a, txn_memtable_table_tests_handed_t, k);
		const txn_memtable_table_tests_handed_t *y = &STD_VEC_AT(b, txn_memtable_table_tests_handed_t, k);

		if (x->records != y->records || x->key.len != y->key.len || memcmp(x->key.ptr, y->key.ptr, x->key.len) != 0)
			return false;
	}
	return true;
}

/* txn_memtable_table_tests_handed_free frees a Vec<(Vec<u8>, usize)> and its keys. */
static void
txn_memtable_table_tests_handed_free(StdVec *v)
{
	for (size_t k = 0; k < v->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(v, txn_memtable_table_tests_handed_t, k).key, 1, 1);
	std_vec_free_Checked(v, sizeof(txn_memtable_table_tests_handed_t), _Alignof(txn_memtable_table_tests_handed_t));
}

/* txn_memtable_table_tests_numbers_handed returns the handed list of the numbered keys below a count, then SPLIT's when asked, each with one record. */
static void
txn_memtable_table_tests_numbers_handed(uint64 count, bool split, StdVec *result)
{
	std_vec_new(result, sizeof(txn_memtable_table_tests_handed_t), _Alignof(txn_memtable_table_tests_handed_t));
	for (uint64 number = 0; number < count + split; number++)
	{
		txn_memtable_table_tests_handed_t handed = {.records = 1};

		txn_memtable_testing_numbered(number < count ? number : TXN_MEMTABLE_SPLIT_SPLIT, &handed.key);
		std_vec_push_Checked(result, &handed, sizeof(handed), _Alignof(txn_memtable_table_tests_handed_t));
	}
}

/* txn_memtable_table_tests_handing_t is a walk's closure's captures: the storage it pushes to, whether it pushes, and the keys handed. */
typedef struct txn_memtable_table_tests_handing_t
{
	HostSharedRecord state;
	bool		pushes;
	StdVec		handed;
} txn_memtable_table_tests_handing_t;

/* txn_memtable_table_tests_handing is a walk's closure: each key handed and its count of records, after SPLIT's push before the first when it pushes. */
static bool
txn_memtable_table_tests_handing(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
								 size_t values_len, HostError *error)
{
	txn_memtable_table_tests_handing_t *c = captures;
	txn_memtable_table_tests_handed_t handed = {.records = values_len};

	if (c->pushes && c->handed.len == 0)
	{
		TxnMemtableVersionHandle handle;
		StdVec		number;
		bool		ok;

		txn_memtable_testing_numbered(TXN_MEMTABLE_SPLIT_SPLIT, &number);
		ok = txn_memtable_table_tests_written(io, c->state, number.ptr, number.len, &handle, error);
		std_vec_free_Checked(&number, 1, 1);
		if (!ok)
			return false;
	}
	std_slice_to_vec_Checked(&handed.key, key, key_len, 1, 1);
	std_vec_push_Checked(&c->handed, &handed, sizeof(handed), _Alignof(txn_memtable_table_tests_handed_t));
	return true;
}

/*
 * The checkpoint's walk: it hands out every key that has a version once, in
 * byte order, passing over the empty key, which has none. A range that
 * splits while its keys are handed out, here by a push its caller makes past
 * them, leaves the walk's keys as the step read them, each once, PLAN's walk
 * reading a range up to the next entry's first key and the last entry to its
 * end (the pushed key, written after the step, is the redo's to bring back);
 * a second walk goes from each half to the next and hands the pushed key out
 * too.
 */
void
test_txn_memtable_table_tests_a_walk_hands_out_each_key_once_across_a_split_and_its_halves(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9364);
	HostSharedRecord state = host_shared_Held_record(&held);
	txn_memtable_table_tests_handing_t handing = {.state = state, .pushes = true};
	txn_memtable_table_tests_handing_t again = {.state = state};
	HostError	error;
	StdVec		ranges;
	StdVec		first;
	StdVec		expected;
	uint64		kept;
	size_t		len;
	size_t		two = 2;

	txn_memtable_table_tests_prefills(3136, 38, io, state);
	std_vec_new(&handing.handed, sizeof(txn_memtable_table_tests_handed_t), _Alignof(txn_memtable_table_tests_handed_t));
	if (!txn_memtable_table_walked(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, txn_memtable_table_tests_handing,
								   &handing, NULL, &error))
		STD_RESULT_EXPECT_FAILED(3145, 10, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(3146, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	len = ranges.len;
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	STD_ASSERT_EQ(3146, 9, len == 2, &len, std_usize_Debug_fmt, &two, std_usize_Debug_fmt);
	txn_memtable_table_tests_numbers_handed(TXN_MEMTABLE_SPLIT_SPLIT - 1, false, &first);
	STD_ASSERT_EQ(3148, 9, txn_memtable_table_tests_handed_eq(&handing.handed, &first), &handing.handed,
				  txn_memtable_table_tests_handed_Debug_fmt, &first, txn_memtable_table_tests_handed_Debug_fmt);
	txn_memtable_table_tests_numbers_handed(TXN_MEMTABLE_SPLIT_SPLIT - 1, true, &expected);
	std_vec_new(&again.handed, sizeof(txn_memtable_table_tests_handed_t), _Alignof(txn_memtable_table_tests_handed_t));
	if (!txn_memtable_table_walked(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, txn_memtable_table_tests_handing,
								   &again, NULL, &error))
		STD_RESULT_EXPECT_FAILED(3156, 10, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(3157, 9, txn_memtable_table_tests_handed_eq(&again.handed, &expected), &again.handed,
				  txn_memtable_table_tests_handed_Debug_fmt, &expected, txn_memtable_table_tests_handed_Debug_fmt);
	txn_memtable_table_tests_handed_free(&again.handed);
	txn_memtable_table_tests_handed_free(&expected);
	txn_memtable_table_tests_handed_free(&first);
	txn_memtable_table_tests_handed_free(&handing.handed);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_lent_free drops each version a read lent and frees their list. */
static void
txn_memtable_table_tests_lent_free(StdVec *lent)
{
	for (size_t k = 0; k < lent->len; k++)
		txn_memtable_version_Version_drop(&STD_VEC_AT(lent, TxnMemtableVersionVersion, k));
	std_vec_free_Checked(lent, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
}

/* txn_memtable_table_tests_lent reads the versions lent of a key for a hold, as versions' unwrap does at its place. */
static void
txn_memtable_table_tests_lent(uint32 line, uint32 col, HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state,
							  const uint8 *key, size_t key_len, StdVec *result)
{
	HostError	error;

	if (!txn_memtable_table_versions(io, hold, state, key, key_len, result, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/*
 * (5)'s first read form: a key's versions are lent newest first for the
 * caller's hold, past the range's lock, and a hint stored on one is read by
 * the next reader; a key the delta lacks, and the range's first key, which
 * has no version, lend none.
 */
void
test_txn_memtable_table_tests_a_keys_versions_are_lent_newest_first_for_the_hold(void)
{
	static const uint64 xids[2] = {7, 8};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9362);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableVersionHandle pushed[2];
	TxnMemtableVersionHandle wanted[2];
	TxnMemtableVersionHint committed = TXN_MEMTABLE_VERSION_HINT_COMMITTED;
	TxnMemtableVersionHint hint;
	HostDeltaHold hold;
	HostError	error;
	StdVec		lent;
	StdVec		again;
	StdVec		none;
	StdVec		handles;
	StdVec		wanted_handles = {wanted, 2, 2};
	bool		eq;

	for (size_t k = 0; k < lengthof(xids); k++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xids[k]), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};

		if (!txn_memtable_table_push(io, state, (const uint8 *) "k", 1, &change, &pushed[k], &error))
			STD_RESULT_EXPECT_FAILED(3177, 52, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
	}
	(void) txn_memtable_table_tests_writes(3179, 46, io, state, (const uint8 *) "other", 5);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(3180, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_lent(3181, 61, io, &hold, state, (const uint8 *) "k", 1, &lent);
	std_vec_with_capacity_Checked(&handles, lent.len, sizeof(TxnMemtableVersionHandle),
								  _Alignof(TxnMemtableVersionHandle));
	for (size_t k = 0; k < lent.len; k++)
		std_vec_push_Checked(&handles, &STD_VEC_AT(&lent, TxnMemtableVersionVersion, k).handle,
							 sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	wanted[0] = pushed[1];
	wanted[1] = pushed[0];
	eq = handles.len == 2;
	for (size_t k = 0; eq && k < 2; k++)
		eq = txn_memtable_version_Handle_PartialEq_eq(&STD_VEC_AT(&handles, TxnMemtableVersionHandle, k), &wanted[k]);
	STD_ASSERT_EQ(3183, 9, eq, &handles, txn_memtable_table_tests_handles_Debug_fmt, &wanted_handles,
				  txn_memtable_table_tests_handles_Debug_fmt);
	std_vec_free_Checked(&handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	std_index_check(0, lent.len, RUST_FILE, 3184, 9);
	txn_memtable_version_Version_set_hint(&STD_VEC_AT(&lent, TxnMemtableVersionVersion, 0), committed);
	txn_memtable_table_tests_lent(3185, 62, io, &hold, state, (const uint8 *) "k", 1, &again);
	std_index_check(0, again.len, RUST_FILE, 3186, 20);
	hint = txn_memtable_version_Version_hint(&STD_VEC_AT(&again, TxnMemtableVersionVersion, 0));
	STD_ASSERT_EQ(3186, 9, hint == committed, &hint, txn_memtable_version_Hint_Debug_fmt, &committed,
				  txn_memtable_version_Hint_Debug_fmt);
	txn_memtable_table_tests_lent(3187, 63, io, &hold, state, (const uint8 *) "absent", 6, &none);
	STD_ASSERT(3187, 9, none.len == 0, "versions(io, &hold, held.record(), b\"absent\").unwrap().is_empty()");
	txn_memtable_table_tests_lent_free(&none);
	txn_memtable_table_tests_lent(3188, 57, io, &hold, state, (const uint8 *) "", 0, &none);
	STD_ASSERT(3188, 9, none.len == 0, "versions(io, &hold, held.record(), b\"\").unwrap().is_empty()");
	txn_memtable_table_tests_lent_free(&none);
	txn_memtable_table_tests_lent_free(&again);
	txn_memtable_table_tests_lent_free(&lent);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_keys_Debug_fmt is a Vec<Vec<u8>>'s Debug. */
bool
txn_memtable_table_tests_keys_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
	{
		const StdVec *key = &STD_VEC_AT(v, StdVec, k);
		StdBytes	bytes = {key->ptr, key->len};

		std_DebugList_entry(&list, &bytes, std_bytes_Debug_fmt);
	}
	return std_DebugList_finish(&list);
}

/* txn_memtable_table_tests_keys_eq reports whether two Vec<Vec<u8>>s are equal. */
bool
txn_memtable_table_tests_keys_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t k = 0; k < a->len; k++)
	{
		const StdVec *x = &STD_VEC_AT(a, StdVec, k);
		const StdVec *y = &STD_VEC_AT(b, StdVec, k);

		if (x->len != y->len || memcmp(x->ptr, y->ptr, x->len) != 0)
			return false;
	}
	return true;
}

/* txn_memtable_table_tests_keys_free frees a Vec<Vec<u8>> and its keys. */
void
txn_memtable_table_tests_keys_free(StdVec *keys)
{
	for (size_t k = 0; k < keys->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(keys, StdVec, k), 1, 1);
	std_vec_free_Checked(keys, sizeof(StdVec), _Alignof(StdVec));
}

/* txn_memtable_table_tests_keys_of returns the scan case's keys as a Vec<Vec<u8>>, as its keys closure does. */
void
txn_memtable_table_tests_keys_of(const char *const *keys, size_t keys_len, StdVec *result)
{
	std_vec_with_capacity_Checked(result, keys_len, sizeof(StdVec), _Alignof(StdVec));
	for (size_t k = 0; k < keys_len; k++)
	{
		StdVec		key;

		std_slice_to_vec_Checked(&key, (const uint8 *) keys[k], strlen(keys[k]), 1, 1);
		std_vec_push_Checked(result, &key, sizeof(StdVec), _Alignof(StdVec));
	}
}

/* txn_memtable_table_tests_walk returns the keys next lends from a place to the end, the way given, each under a hold of its own as a scan's calls take them. */
static void
txn_memtable_table_tests_walk(HostIoIo *io, HostSharedRecord state, bool has_from, const uint8 *from, size_t from_len,
							  TxnMemtableTableDirection way, StdVec *keys)
{
	std_vec_new(keys, sizeof(StdVec), _Alignof(StdVec));
	for (;;)
	{
		HostDeltaHold hold;
		HostError	error;
		txn_TupleVecU8VecVersion_t found;
		const StdVec *last = keys->len > 0 ? &STD_VEC_AT(keys, StdVec, keys->len - 1) : NULL;
		bool		some;

		if (!host_delta_hold(io, &hold, &error))
			STD_RESULT_EXPECT_FAILED(3196, 40, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_memtable_table_next(io, &hold, state, last != NULL || has_from,
									 last != NULL ? last->ptr : from, last != NULL ? last->len : from_len, way, &some,
									 &found, &error))
			STD_RESULT_EXPECT_FAILED(3198, 76, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!some)
		{
			host_delta_Hold_drop(&hold);
			return;
		}
		STD_ASSERT(3199, 13, found.f1.len > 0, "!versions.is_empty()");
		std_vec_push_Checked(keys, &found.f0, sizeof(StdVec), _Alignof(StdVec));
		txn_memtable_table_tests_lent_free(&found.f1);
		host_delta_Hold_drop(&hold);
	}
}

/* txn_memtable_table_tests_walk_is asserts a walk's keys, as the scan cases' assert_eq does at its place. */
static void
txn_memtable_table_tests_walk_is(uint32 line, HostIoIo *io, HostSharedRecord state, const char *from,
								 TxnMemtableTableDirection way, const char *const *keys, size_t keys_len)
{
	StdVec		walked;
	StdVec		expected;

	txn_memtable_table_tests_walk(io, state, from != NULL, (const uint8 *) from, from != NULL ? strlen(from) : 0, way,
								  &walked);
	txn_memtable_table_tests_keys_of(keys, keys_len, &expected);
	STD_ASSERT_EQ(line, 9, txn_memtable_table_tests_keys_eq(&walked, &expected), &walked,
				  txn_memtable_table_tests_keys_Debug_fmt, &expected, txn_memtable_table_tests_keys_Debug_fmt);
	txn_memtable_table_tests_keys_free(&expected);
	txn_memtable_table_tests_keys_free(&walked);
}

/*
 * (5)'s scan form over three ranges: next lends each key with a version
 * once, in key order forward and in reverse backward, from either end or
 * from a place, across the ranges' bounds, skipping the first keys that have
 * none; range_starts names the three ranges' first keys.
 */
void
test_txn_memtable_table_tests_a_scan_lends_each_key_with_a_version_once_either_way(void)
{
	static const char *const written_keys[5] = {"z", "a", "p", "m", "c"};
	static const char *const all[5] = {"a", "c", "m", "p", "z"};
	static const char *const reversed[5] = {"z", "p", "m", "c", "a"};
	static const char *const past_m[2] = {"p", "z"};
	static const char *const before_m[2] = {"c", "a"};
	static const char *const before_t[4] = {"p", "m", "c", "a"};
	static const char *const starts[3] = {"", "m", "t"};
	static const StdBytes firsts[2] = {{(const uint8 *) "m", 1}, {(const uint8 *) "t", 1}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9363);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostError	error;
	StdVec		ranges;
	StdVec		named;
	StdVec		expected;

	if (!txn_memtable_testing_ranged(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, firsts, 2, &ranges, &error))
		STD_RESULT_EXPECT_FAILED(3213, 58, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0; k < lengthof(written_keys); k++)
		(void) txn_memtable_table_tests_writes(3215, 45, io, state, (const uint8 *) written_keys[k],
											   strlen(written_keys[k]));
	txn_memtable_table_tests_walk_is(3219, io, state, NULL, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, all, 5);
	txn_memtable_table_tests_walk_is(3220, io, state, NULL, TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD, reversed, 5);
	txn_memtable_table_tests_walk_is(3221, io, state, "m", TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, past_m, 2);
	txn_memtable_table_tests_walk_is(3222, io, state, "m", TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD, before_m, 2);
	txn_memtable_table_tests_walk_is(3223, io, state, "t", TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD, before_t, 4);
	if (!txn_memtable_table_range_starts(io, state, &named, &error))
		STD_RESULT_EXPECT_FAILED(3224, 44, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_keys_of(starts, 3, &expected);
	STD_ASSERT_EQ(3224, 9, txn_memtable_table_tests_keys_eq(&named, &expected), &named,
				  txn_memtable_table_tests_keys_Debug_fmt, &expected, txn_memtable_table_tests_keys_Debug_fmt);
	txn_memtable_table_tests_keys_free(&expected);
	txn_memtable_table_tests_keys_free(&named);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_one returns the key next lends from a place the way given under a hold of its own, as the split scan case's one closure does. */
static bool
txn_memtable_table_tests_one(HostIoIo *io, HostSharedRecord state, const StdVec *place, TxnMemtableTableDirection way,
							 StdVec *result)
{
	HostDeltaHold hold;
	HostError	error;
	txn_TupleVecU8VecVersion_t found;
	bool		some;

	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(3245, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_table_next(io, &hold, state, true, place->ptr, place->len, way, &some, &found, &error))
		STD_RESULT_EXPECT_FAILED(3246, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (some)
	{
		*result = found.f0;
		txn_memtable_table_tests_lent_free(&found.f1);
	}
	host_delta_Hold_drop(&hold);
	return some;
}

/* txn_memtable_table_tests_one_is asserts the key next lends from a numbered place, or none, as the split scan case's assert_eq does at its place. */
static void
txn_memtable_table_tests_one_is(uint32 line, HostIoIo *io, HostSharedRecord state, const StdVec *place,
								TxnMemtableTableDirection way, bool has_expected, uint64 expected)
{
	StdVec		lent;
	StdVec		wanted;
	StdBytes	lent_bytes;
	StdBytes	wanted_bytes;
	StdOptionRef left;
	StdOptionRef right;
	bool		some = txn_memtable_table_tests_one(io, state, place, way, &lent);

	if (has_expected)
		txn_memtable_testing_numbered(expected, &wanted);
	lent_bytes.ptr = some ? lent.ptr : NULL;
	lent_bytes.len = some ? lent.len : 0;
	wanted_bytes.ptr = has_expected ? wanted.ptr : NULL;
	wanted_bytes.len = has_expected ? wanted.len : 0;
	left.has = some;
	left.some = STD_ITEM(&lent_bytes, std_bytes_Debug_fmt);
	right.has = has_expected;
	right.some = STD_ITEM(&wanted_bytes, std_bytes_Debug_fmt);
	STD_ASSERT_EQ(line, 9, some == has_expected && (!some || (lent_bytes.len == wanted_bytes.len &&
															  memcmp(lent_bytes.ptr, wanted_bytes.ptr,
																	 lent_bytes.len) == 0)),
				  &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (has_expected)
		std_vec_free_Checked(&wanted, 1, 1);
	if (some)
		std_vec_free_Checked(&lent, 1, 1);
}

/*
 * (5)'s scan form across a split: a scan whose place is in a range that
 * splits between its calls goes on from that place in the halves, backward
 * and forward, crossing the halves' bound with no key skipped or lent twice.
 */
void
test_txn_memtable_table_tests_a_scan_goes_on_across_a_split_between_its_calls(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9365);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostDeltaHold hold;
	HostError	error;
	txn_TupleVecU8VecVersion_t found;
	StdVec		last;
	StdVec		wanted;
	StdVec		starts;
	StdVec		expected;
	StdVec		place;
	StdVec		backward;
	StdVec		all;
	StdBytes	last_bytes;
	StdBytes	wanted_bytes;
	bool		some;

	txn_memtable_table_tests_prefills(3236, 30, io, state);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(3238, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_table_next(io, &hold, state, false, NULL, 0, TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD, &some, &found,
								 &error))
		STD_RESULT_EXPECT_FAILED(3239, 63, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!some)
		STD_OPTION_UNWRAP_FAILED(3239, 72);
	last = found.f0;
	txn_memtable_table_tests_lent_free(&found.f1);
	host_delta_Hold_drop(&hold);
	txn_memtable_testing_numbered(TXN_MEMTABLE_SPLIT_SPLIT - 2, &wanted);
	last_bytes.ptr = last.ptr;
	last_bytes.len = last.len;
	wanted_bytes.ptr = wanted.ptr;
	wanted_bytes.len = wanted.len;
	STD_ASSERT_EQ(3241, 9, last.len == wanted.len && memcmp(last.ptr, wanted.ptr, last.len) == 0, &last_bytes,
				  std_bytes_Debug_fmt, &wanted_bytes, std_bytes_Debug_fmt);
	std_vec_free_Checked(&wanted, 1, 1);
	txn_memtable_table_tests_numbered_writes(3242, 46, io, state, TXN_MEMTABLE_SPLIT_SPLIT);
	if (!txn_memtable_table_range_starts(io, state, &starts, &error))
		STD_RESULT_EXPECT_FAILED(3243, 44, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_with_capacity_Checked(&expected, 2, sizeof(StdVec), _Alignof(StdVec));
	std_vec_new(&place, 1, 1);
	std_vec_push_Checked(&expected, &place, sizeof(StdVec), _Alignof(StdVec));
	txn_memtable_testing_numbered(16383, &place);
	std_vec_push_Checked(&expected, &place, sizeof(StdVec), _Alignof(StdVec));
	STD_ASSERT_EQ(3243, 9, txn_memtable_table_tests_keys_eq(&starts, &expected), &starts,
				  txn_memtable_table_tests_keys_Debug_fmt, &expected, txn_memtable_table_tests_keys_Debug_fmt);
	txn_memtable_table_tests_keys_free(&expected);
	txn_memtable_table_tests_keys_free(&starts);
	txn_memtable_table_tests_one_is(3248, io, state, &last, TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD, true,
									TXN_MEMTABLE_SPLIT_SPLIT - 3);
	txn_memtable_table_tests_one_is(3249, io, state, &last, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, true,
									TXN_MEMTABLE_SPLIT_SPLIT);
	txn_memtable_testing_numbered(16383, &place);
	txn_memtable_table_tests_one_is(3250, io, state, &place, TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD, true, 16382);
	std_vec_free_Checked(&place, 1, 1);
	txn_memtable_testing_numbered(16382, &place);
	txn_memtable_table_tests_one_is(3251, io, state, &place, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, true, 16383);
	std_vec_free_Checked(&place, 1, 1);
	txn_memtable_testing_numbered(TXN_MEMTABLE_SPLIT_SPLIT, &place);
	txn_memtable_table_tests_one_is(3252, io, state, &place, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, false, 0);
	std_vec_free_Checked(&place, 1, 1);
	txn_memtable_testing_numbered(0, &place);
	txn_memtable_table_tests_one_is(3253, io, state, &place, TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD, false, 0);
	std_vec_free_Checked(&place, 1, 1);
	txn_memtable_table_tests_walk(io, state, false, NULL, 0, TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD, &backward);
	std_vec_with_capacity_Checked(&all, TXN_MEMTABLE_SPLIT_SPLIT, sizeof(StdVec), _Alignof(StdVec));
	txn_memtable_testing_numbered(TXN_MEMTABLE_SPLIT_SPLIT, &place);
	std_vec_push_Checked(&all, &place, sizeof(StdVec), _Alignof(StdVec));
	for (uint64 number = TXN_MEMTABLE_SPLIT_SPLIT - 1; number > 0; number--)
	{
		txn_memtable_testing_numbered(number - 1, &place);
		std_vec_push_Checked(&all, &place, sizeof(StdVec), _Alignof(StdVec));
	}
	STD_ASSERT_EQ(3256, 9, txn_memtable_table_tests_keys_eq(&backward, &all), &backward,
				  txn_memtable_table_tests_keys_Debug_fmt, &all, txn_memtable_table_tests_keys_Debug_fmt);
	txn_memtable_table_tests_keys_free(&all);
	txn_memtable_table_tests_keys_free(&backward);
	std_vec_free_Checked(&last, 1, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_endings_Debug_fmt is a Vec<(String, Ending)>'s Debug, of a run's endings. */
static bool
txn_memtable_table_tests_endings_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
	{
		const simc_TupleStringEnding_t *ending = &STD_VEC_AT(v, simc_TupleStringEnding_t, k);
		StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&ending->f0, std_String_Debug_fmt),
									  STD_ITEM(&ending->f1, simc_Ending_Debug_fmt));

		std_DebugList_entry(&list, &tuple, std_tuple_Debug_fmt);
	}
	return std_DebugList_finish(&list);
}

/* txn_memtable_table_tests_all_returned reports whether every actor of an ended run returned Ok(()), panicking as the let-else does at its place on a run that didn't end. */
static bool
txn_memtable_table_tests_all_returned(const SimcReport *report, uint32 line, uint32 col)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	SimcEnding	returned = {.tag = SIMC_ENDING_RETURNED};

	if (report->outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtArg	args[1] = {{&report->outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, line, col, &message);
	}
	for (size_t k = 0; k < report->outcome.u.ended.len; k++)
		if (!simc_Ending_PartialEq_eq(&STD_VEC_AT(&report->outcome.u.ended, simc_TupleStringEnding_t, k).f1, &returned))
			return false;
	return true;
}

/* txn_memtable_table_tests_upper_scanner is the backward split scan's scanner: its scan from the end, which must lend SPLIT's key. */
static bool
txn_memtable_table_tests_upper_scanner(void *captures, HostIoIo *io, HostError *error)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("the scan lent "), STD_FMT_ARG(0)};
	HostSharedHeld held;
	HostDeltaHold hold;
	txn_TupleVecU8VecVersion_t found;
	StdVec		split;
	bool		some;
	bool		ok;

	if (!txn_memtable_testing_stated(io, 9367, &held, error))
		return false;
	if (!host_delta_hold(io, &hold, error))
	{
		host_shared_Held_drop(&held);
		return false;
	}
	ok = txn_memtable_table_next(io, &hold, host_shared_Held_record(&held), false, NULL, 0,
								 TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD, &some, &found, error);
	if (ok)
	{
		if (some)
			txn_memtable_table_tests_lent_free(&found.f1);
		txn_memtable_testing_numbered(TXN_MEMTABLE_SPLIT_SPLIT, &split);
		if (!some || found.f0.len != split.len || memcmp(found.f0.ptr, split.ptr, split.len) != 0)
		{
			StdBytes	lent = {some ? found.f0.ptr : NULL, some ? found.f0.len : 0};
			StdOptionRef option = {some, STD_ITEM(&lent, std_bytes_Debug_fmt)};
			StdFmtArg	args[1] = {{&option, std_option_Debug_fmt}};
			StdFmtArguments format = {pieces, lengthof(pieces), args};
			StdString	message;

			std_fmt_format_Checked(&message, &format);
			*error = host_Error_fork((const uint8 *) "XX000", &message);
			ok = false;
		}
		std_vec_free_Checked(&split, 1, 1);
		if (some)
			std_vec_free_Checked(&found.f0, 1, 1);
	}
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	return ok;
}

/*
 * (5)'s backward scan across a split inside one call: a scan that met no
 * version in the last range turns to the range before, which splits after
 * the scan chose it and before the scan reads it, and the scan reads the
 * upper half first and lends the newest key below its bound, never the
 * lower half's last key, which would skip keys that were there before the
 * scan began.
 */
void
test_txn_memtable_table_tests_a_backward_scan_reads_the_upper_half_of_a_range_that_split_under_it(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	static const txn_memtable_table_tests_numbered_writer_t writer_captures = {9367, TXN_MEMTABLE_SPLIT_SPLIT};
	static const StdBytes firsts[1] = {{(const uint8 *) "zz", 2}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcActor	scanner = {txn_memtable_table_tests_upper_scanner};
	SimcActor	writer = {txn_memtable_table_tests_numbered_writer, (void *) &writer_captures};
	SimcStep	steps[2] = {{"writer", 6, PG_UINT64_MAX}, {"scanner", 7, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;
	HostSharedHeld held;
	HostError	error;
	StdVec		ranges;

	held = txn_memtable_testing_created(io, 9367);
	if (!txn_memtable_testing_ranged(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, host_shared_Held_record(&held), firsts, 1,
									 &ranges, &error))
		STD_RESULT_EXPECT_FAILED(3269, 57, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	txn_memtable_table_tests_prefills(3270, 42, io, host_shared_Held_record(&held));
	host_shared_Held_drop(&held);
	run = simc_Run_new(1);
	simc_Run_actor(&run, "scanner", 7, scanner);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_until(&run, "scanner", 7, "doltgres-memtable-step");
	simc_Run_until(&run, "scanner", 7, "doltgres-memtable-step");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	if (!txn_memtable_table_tests_all_returned(&report, 3289, 67))
	{
		StdFmtArg	args[1] = {{&report.outcome.u.ended, txn_memtable_table_tests_endings_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 3290, 9, &message);
	}
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_collecting is the checkpoint walker's closure: each key handed, in a Vec<Vec<u8>>. */
bool
txn_memtable_table_tests_collecting(void *captures, HostIoIo *io, const uint8 *key, size_t key_len,
									const StdVec *values, size_t values_len, HostError *error)
{
	StdVec	   *keys = captures;
	StdVec		copy;

	std_slice_to_vec_Checked(&copy, key, key_len, 1, 1);
	std_vec_push_Checked(keys, &copy, sizeof(StdVec), _Alignof(StdVec));
	return true;
}

/* txn_memtable_table_tests_walker_t is a held walk's walker's captures: its database, the keys it must hand out, whether it walks as the checkpoint does, a scan's way, and its error's lead-in before the keys it handed out, or none for an error that counts them. */
typedef struct txn_memtable_table_tests_walker_t
{
	uint32		database;
	const StdVec *want;
	bool		walked;
	TxnMemtableTableDirection way;
	const char *lent;
} txn_memtable_table_tests_walker_t;

/* txn_memtable_table_tests_next_keys collects the keys a scan lends the way given from its end, each call under a hold of its own. */
static bool
txn_memtable_table_tests_next_keys(HostIoIo *io, HostSharedRecord state, TxnMemtableTableDirection way, StdVec *keys,
								   HostError *error)
{
	for (;;)
	{
		HostDeltaHold hold;
		txn_TupleVecU8VecVersion_t found;
		const StdVec *last = keys->len > 0 ? &STD_VEC_AT(keys, StdVec, keys->len - 1) : NULL;
		bool		some;
		bool		ok;

		if (!host_delta_hold(io, &hold, error))
			return false;
		ok = txn_memtable_table_next(io, &hold, state, last != NULL, last != NULL ? last->ptr : NULL,
									 last != NULL ? last->len : 0, way, &some, &found, error);
		if (ok && some)
		{
			txn_memtable_table_tests_lent_free(&found.f1);
			std_vec_push_Checked(keys, &found.f0, sizeof(StdVec), _Alignof(StdVec));
		}
		host_delta_Hold_drop(&hold);
		if (!ok || !some)
			return ok;
	}
}

/* txn_memtable_table_tests_walker is a held walk's walker: its walk of its storage's keys, which must hand out its wanted keys. */
static bool
txn_memtable_table_tests_walker(void *captures, HostIoIo *io, HostError *error)
{
	static const StdFmtPiece counted[] = {STD_FMT_LIT("the walk handed out "), STD_FMT_ARG(0), STD_FMT_LIT(" keys")};
	static const StdFmtPiece listed[] = {STD_FMT_ARG(0), STD_FMT_ARG(1)};
	const txn_memtable_table_tests_walker_t *c = captures;
	HostSharedHeld held;
	StdVec		keys;
	bool		ok;

	if (!txn_memtable_testing_stated(io, c->database, &held, error))
		return false;
	std_vec_new(&keys, sizeof(StdVec), _Alignof(StdVec));
	if (c->walked)
		ok = txn_memtable_table_walked(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, host_shared_Held_record(&held),
									   txn_memtable_table_tests_collecting, &keys, NULL, error);
	else
		ok = txn_memtable_table_tests_next_keys(io, host_shared_Held_record(&held), c->way, &keys, error);
	if (ok && !txn_memtable_table_tests_keys_eq(&keys, c->want))
	{
		StdStr		lent = {c->lent, c->lent != NULL ? strlen(c->lent) : 0};
		StdFmtArg	counts[1] = {{&keys.len, std_usize_Display_fmt}};
		StdFmtArg	lists[2] = {{&lent, std_str_Display_fmt}, {&keys, txn_memtable_table_tests_keys_Debug_fmt}};
		StdFmtArguments format = {counted, lengthof(counted), counts};
		StdString	message;

		if (c->lent != NULL)
		{
			format.pieces = listed;
			format.pieces_len = lengthof(listed);
			format.args = lists;
		}
		std_fmt_format_Checked(&message, &format);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		ok = false;
	}
	txn_memtable_table_tests_keys_free(&keys);
	host_shared_Held_drop(&held);
	return ok;
}

/*
 * Case 24 (d), the walk's step between two ranges: a forward walk held
 * between the first range and the second (after its read of the first,
 * before it asks the memtable what follows) while a writer's push splits the
 * second, goes on in the halves and hands out each key once, in order, the
 * writer's included; for the checkpoint's walk (walked) and for a scan's
 * (next forward).
 */
void
test_txn_memtable_table_tests_a_walk_held_between_two_ranges_while_the_second_splits_hands_out_each_key_once(void)
{
	static const uint32 databases[2] = {9369, 9370};
	static const char *const walkers[2] = {"walked", "next"};
	static const StdBytes firsts[1] = {{(const uint8 *) "b", 1}};
	static const StdFmtPiece both[] = {STD_FMT_ARG(0), STD_FMT_LIT(": "), STD_FMT_ARG(1)};
	static const StdFmtPiece named[] = {STD_FMT_ARG(0)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	StdVec		expected;
	StdVec		key;

	for (size_t k = 0; k < lengthof(databases); k++)
	{
		HostSharedHeld held = txn_memtable_testing_created(io, databases[k]);
		StdVec		ranges;

		if (!txn_memtable_testing_ranged(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, host_shared_Held_record(&held), firsts,
										 1, &ranges, &error))
			STD_RESULT_EXPECT_FAILED(3304, 56, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
		(void) txn_memtable_table_tests_writes(3305, 46, io, host_shared_Held_record(&held), (const uint8 *) "a", 1);
		txn_memtable_table_tests_prefills(3306, 42, io, host_shared_Held_record(&held));
		host_shared_Held_drop(&held);
	}
	std_vec_with_capacity_Checked(&expected, TXN_MEMTABLE_SPLIT_SPLIT + 1, sizeof(StdVec), _Alignof(StdVec));
	std_slice_to_vec_Checked(&key, (const uint8 *) "a", 1, 1, 1);
	std_vec_push_Checked(&expected, &key, sizeof(StdVec), _Alignof(StdVec));
	for (uint64 number = 0; number < TXN_MEMTABLE_SPLIT_SPLIT; number++)
	{
		txn_memtable_testing_numbered(number < TXN_MEMTABLE_SPLIT_SPLIT - 1 ? number : TXN_MEMTABLE_SPLIT_SPLIT, &key);
		std_vec_push_Checked(&expected, &key, sizeof(StdVec), _Alignof(StdVec));
	}
	for (size_t k = 0; k < lengthof(databases); k++)
	{
		txn_memtable_table_tests_walker_t walker_captures = {databases[k], &expected, k == 0,
		TXN_MEMTABLE_TABLE_DIRECTION_FORWARD};
		txn_memtable_table_tests_numbered_writer_t writer_captures = {databases[k], TXN_MEMTABLE_SPLIT_SPLIT};
		SimcActor	walker = {txn_memtable_table_tests_walker, &walker_captures};
		SimcActor	writer = {txn_memtable_table_tests_numbered_writer, &writer_captures};
		SimcStep	steps[2] = {{"writer", 6, PG_UINT64_MAX}, {walkers[k], strlen(walkers[k]), PG_UINT64_MAX}};
		StdStr		name = {walkers[k], strlen(walkers[k])};
		SimcRun		run = simc_Run_new(1);
		SimcReport	report;
		HostSharedHeld held;
		StdVec		starts;
		size_t		len;
		size_t		three = 3;

		simc_Run_actor(&run, walkers[k], strlen(walkers[k]), walker);
		simc_Run_actor(&run, "writer", 6, writer);
		simc_Run_until(&run, walkers[k], strlen(walkers[k]), "doltgres-memtable-step");
		simc_Run_script(&run, steps, lengthof(steps));
		report = simc_Run_finish(run);
		if (!txn_memtable_table_tests_all_returned(&report, 3341, 71))
		{
			StdFmtArg	args[2] = {{&name, std_str_Display_fmt},
			{&report.outcome.u.ended, txn_memtable_table_tests_endings_Debug_fmt}};
			StdFmtArguments message = {both, lengthof(both), args};

			std_panic_fmt(RUST_FILE, 3342, 13, &message);
		}
		if (!txn_memtable_testing_stated(io, databases[k], &held, &error))
			STD_RESULT_EXPECT_FAILED(3346, 45, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!txn_memtable_table_range_starts(io, host_shared_Held_record(&held), &starts, &error))
			STD_RESULT_EXPECT_FAILED(3347, 56, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		len = starts.len;
		txn_memtable_table_tests_keys_free(&starts);
		if (len != 3)
		{
			StdFmtArg	args[1] = {{&name, std_str_Display_fmt}};
			StdFmtArguments message = {named, lengthof(named), args};

			std_assert_failed(RUST_FILE, 3347, 13, "==", &len, std_usize_Debug_fmt, &three, std_usize_Debug_fmt,
							  &message);
		}
		host_shared_Held_drop(&held);
		simc_Report_drop(&report);
	}
	txn_memtable_table_tests_keys_free(&expected);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_handles_of returns the handles of a key's versions lent for a hold of its own, as the remover cases' handles closures do, the hold's unwrap at its line and their read's on the next. */
static void
txn_memtable_table_tests_handles_of(uint32 line, uint32 hold_col, uint32 col, HostIoIo *io, HostSharedRecord state,
									const uint8 *key, size_t key_len, StdVec *result)
{
	HostDeltaHold hold;
	HostError	error;
	StdVec		lent;

	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(line, hold_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_lent(line + 1, col, io, &hold, state, key, key_len, &lent);
	std_vec_with_capacity_Checked(result, lent.len, sizeof(TxnMemtableVersionHandle),
								  _Alignof(TxnMemtableVersionHandle));
	for (size_t k = 0; k < lent.len; k++)
		std_vec_push_Checked(result, &STD_VEC_AT(&lent, TxnMemtableVersionVersion, k).handle,
							 sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	txn_memtable_table_tests_lent_free(&lent);
	host_delta_Hold_drop(&hold);
}

/* txn_memtable_table_tests_handles_are asserts a key's versions' handles, as the remover cases' assert_eq on their handles does at its place, with handles_of's closure line and column. */
static void
txn_memtable_table_tests_handles_are(uint32 line, uint32 closure_line, uint32 closure_col, HostIoIo *io,
									 HostSharedRecord state, const uint8 *key, size_t key_len,
									 const TxnMemtableVersionHandle *wanted, size_t wanted_len)
{
	StdVec		handles;
	StdVec		expected = {(void *) wanted, wanted_len, wanted_len};
	bool		eq;

	txn_memtable_table_tests_handles_of(closure_line, 40, closure_col, io, state, key, key_len, &handles);
	eq = handles.len == wanted_len;
	for (size_t k = 0; eq && k < wanted_len; k++)
		eq = txn_memtable_version_Handle_PartialEq_eq(&STD_VEC_AT(&handles, TxnMemtableVersionHandle, k), &wanted[k]);
	STD_ASSERT_EQ(line, 9, eq, &handles, txn_memtable_table_tests_handles_Debug_fmt, &expected,
				  txn_memtable_table_tests_handles_Debug_fmt);
	std_vec_free_Checked(&handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
}

/* txn_memtable_table_tests_range_keys returns the count of keys of the range the empty key's hand-off finds, after the storage's range starts are read, as the remover case's keys closure does. */
static bool
txn_memtable_table_tests_range_keys(HostIoIo *io, HostSharedRecord state, uint64 *result, HostError *error)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostDeltaHold hold;
	HostSharedRecord record;
	TxnMemtableTableAt at;
	StdVec		starts;
	bool		some;

	if (!txn_memtable_table_range_starts(io, state, &starts, error))
		return false;
	txn_memtable_table_tests_keys_free(&starts);
	if (!host_delta_hold(io, &hold, error))
		return false;
	if (!txn_memtable_table_found(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, &hold, state, (const uint8 *) "", 0, NULL,
								  &some, &at, error))
	{
		host_delta_Hold_drop(&hold);
		return false;
	}
	if (!some)
		STD_OPTION_UNWRAP_FAILED(3381, 70);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at.range, &record, error))
	{
		host_delta_Hold_drop(&hold);
		return false;
	}
	*result = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_KEYS, load);
	host_delta_Hold_drop(&hold);
	return true;
}

/* txn_memtable_table_tests_range_keys_are asserts the range's count of keys, as the remover case's assert_eq on its keys' unwrap does at its place. */
static void
txn_memtable_table_tests_range_keys_are(uint32 line, HostIoIo *io, HostSharedRecord state, uint64 wanted)
{
	HostError	error;
	uint64		keys;

	if (!txn_memtable_table_tests_range_keys(io, state, &keys, &error))
		STD_RESULT_EXPECT_FAILED(line, 29, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(line, 9, keys == wanted, &keys, std_u64_Debug_fmt, &wanted, std_u64_Debug_fmt);
}

/* txn_memtable_table_tests_removes takes versions out of a key's chain of a storage's rows, as removed's unwrap does at its place. */
static void
txn_memtable_table_tests_removes(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state, const uint8 *key,
								 size_t key_len, const TxnMemtableVersionHandle *handles, size_t handles_len)
{
	HostError	error;
	StdVec		took;

	if (!txn_memtable_table_removed(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, key, key_len, handles, handles_len,
									&took, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&took, sizeof(bool), _Alignof(bool));
}

/* txn_memtable_table_tests_pushed_at returns pushed[index], its index checked as Rust's is at its place. */
static TxnMemtableVersionHandle
txn_memtable_table_tests_pushed_at(const StdVec *pushed, size_t index, uint32 line, uint32 col)
{
	std_index_check(index, pushed->len, RUST_FILE, line, col);
	return STD_VEC_AT(pushed, TxnMemtableVersionHandle, index);
}

/*
 * 2g's remover over a chain: a key's middle, newest, and oldest versions
 * taken out in turn leave the others in order, a handle the chain doesn't
 * hold is passed over, and the last one's removal takes the key out of the
 * index, so a walk either way doesn't meet it and the range counts one key
 * fewer.
 */
void
test_txn_memtable_table_tests_a_removal_takes_versions_out_of_a_chain_and_the_key_out_with_its_last(void)
{
	static const uint64 xids[3] = {7, 8, 9};
	static const char *const z[1] = {"z"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9371);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableVersionHandle handles[2];
	HostError	error;
	StdVec		pushed;
	StdVec		none;

	std_vec_with_capacity_Checked(&pushed, 3, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	for (size_t k = 0; k < lengthof(xids); k++)
	{
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xids[k]), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;

		if (!txn_memtable_table_push(io, state, (const uint8 *) "k", 1, &change, &handle, &error))
			STD_RESULT_EXPECT_FAILED(3370, 48, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_Op_drop(&op);
		std_vec_push_Checked(&pushed, &handle, sizeof(handle), _Alignof(TxnMemtableVersionHandle));
	}
	(void) txn_memtable_table_tests_writes(3373, 34, io, state, (const uint8 *) "z", 1);
	txn_memtable_table_tests_range_keys_are(3385, io, state, 3);
	handles[0] = txn_memtable_table_tests_pushed_at(&pushed, 1, 3386, 44);
	txn_memtable_table_tests_removes(3386, 56, io, state, (const uint8 *) "k", 1, handles, 1);
	handles[0] = txn_memtable_table_tests_pushed_at(&pushed, 2, 3387, 34);
	handles[1] = txn_memtable_table_tests_pushed_at(&pushed, 0, 3387, 45);
	txn_memtable_table_tests_handles_are(3387, 3375, 46, io, state, (const uint8 *) "k", 1, handles, 2);
	handles[0] = txn_memtable_table_tests_pushed_at(&pushed, 2, 3388, 44);
	handles[1] = txn_memtable_table_tests_pushed_at(&pushed, 1, 3388, 55);
	txn_memtable_table_tests_removes(3388, 67, io, state, (const uint8 *) "k", 1, handles, 2);
	handles[0] = txn_memtable_table_tests_pushed_at(&pushed, 0, 3389, 34);
	txn_memtable_table_tests_handles_are(3389, 3375, 46, io, state, (const uint8 *) "k", 1, handles, 1);
	handles[0] = txn_memtable_table_tests_pushed_at(&pushed, 0, 3390, 44);
	txn_memtable_table_tests_removes(3390, 56, io, state, (const uint8 *) "k", 1, handles, 1);
	txn_memtable_table_tests_handles_of(3375, 40, 46, io, state, (const uint8 *) "k", 1, &none);
	STD_ASSERT(3391, 9, none.len == 0, "handles(io).is_empty()");
	std_vec_free_Checked(&none, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	txn_memtable_table_tests_range_keys_are(3392, io, state, 2);
	txn_memtable_table_tests_walk_is(3393, io, state, NULL, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, z, 1);
	txn_memtable_table_tests_walk_is(3394, io, state, NULL, TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD, z, 1);
	handles[0] = txn_memtable_table_tests_pushed_at(&pushed, 0, 3395, 49);
	txn_memtable_table_tests_removes(3395, 61, io, state, (const uint8 *) "absent", 6, handles, 1);
	std_vec_free_Checked(&pushed, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_starts_are asserts a storage's range starts, as assert_eq on range_starts' unwrap does at its place. */
static void
txn_memtable_table_tests_starts_are(uint32 line, HostIoIo *io, HostSharedRecord state, const char *const *keys,
									size_t keys_len)
{
	HostError	error;
	StdVec		starts;
	StdVec		expected;

	if (!txn_memtable_table_range_starts(io, state, &starts, &error))
		STD_RESULT_EXPECT_FAILED(line, 44, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_keys_of(keys, keys_len, &expected);
	STD_ASSERT_EQ(line, 9, txn_memtable_table_tests_keys_eq(&starts, &expected), &starts,
				  txn_memtable_table_tests_keys_Debug_fmt, &expected, txn_memtable_table_tests_keys_Debug_fmt);
	txn_memtable_table_tests_keys_free(&expected);
	txn_memtable_table_tests_keys_free(&starts);
}

/*
 * 2g's remover over a range: the last key of a range past the first taken
 * out leaves the range holding only its first key, so it is closed and
 * taken out of the memtable's list; the floor range emptied too closes the
 * memtable, no pin naming it, and the tree's word names none.
 */
void
test_txn_memtable_table_tests_a_removal_closes_an_emptied_range_and_an_emptied_memtable(void)
{
	static const StdBytes firsts[1] = {{(const uint8 *) "m", 1}};
	static const char *const both[2] = {"", "m"};
	static const char *const floor[1] = {""};
	static const char *const a[1] = {"a"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9372);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableVersionHandle low;
	TxnMemtableVersionHandle high;
	StdOptionRef left;
	StdOptionRef none = {false};
	HostError	error;
	StdVec		ranges;
	uint64		location;

	if (!txn_memtable_testing_ranged(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, firsts, 1, &ranges, &error))
		STD_RESULT_EXPECT_FAILED(3408, 44, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	low = txn_memtable_table_tests_writes(3409, 53, io, state, (const uint8 *) "a", 1);
	high = txn_memtable_table_tests_writes(3409, 88, io, state, (const uint8 *) "n", 1);
	txn_memtable_table_tests_starts_are(3410, io, state, both, 2);
	txn_memtable_table_tests_removes(3411, 51, io, state, (const uint8 *) "n", 1, &high, 1);
	txn_memtable_table_tests_starts_are(3412, io, state, floor, 1);
	txn_memtable_table_tests_walk_is(3413, io, state, NULL, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, a, 1);
	txn_memtable_table_tests_removes(3414, 50, io, state, (const uint8 *) "a", 1, &low, 1);
	left.has = txn_memtable_table_tests_has_memtable(state, &location);
	left.some = STD_ITEM(&location, std_u64_Debug_fmt);
	STD_ASSERT_EQ(3415, 9, !left.has, &left, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_prunes prunes a key's versions of aborted writers in a storage's rows, as pruned's unwrap does at its place. */
static void
txn_memtable_table_tests_prunes(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state, const char *key)
{
	HostError	error;

	if (!txn_memtable_table_pruned(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, (const uint8 *) key, strlen(key), NULL,
								   0, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/* txn_memtable_table_tests_pushes pushes a put of v to a key by a writer, as push's unwrap does at its place. */
static TxnMemtableVersionHandle
txn_memtable_table_tests_pushes(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state, const char *key,
								TxnMemtableVersionWriter writer)
{
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnMemtableVersionChange change = {.writer = writer, .op = &op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
	.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableVersionHandle handle;
	HostError	error;

	if (!txn_memtable_table_push(io, state, (const uint8 *) key, strlen(key), &change, &handle, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_Op_drop(&op);
	return handle;
}

/*
 * 2g's prune: a key's versions whose writers the commit log says aborted, an
 * aborted subtransaction's among them, are taken out, and those of a
 * committed transaction, of one still running, and of no transaction stay; a
 * version hinted aborted goes with no question asked, and a key left with
 * none leaves the index.
 */
void
test_txn_memtable_table_tests_a_prune_takes_out_the_versions_of_aborted_writers_only(void)
{
	static const char *const k[1] = {"k"};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9373);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostXid		committed = simc_begin();
	HostXid		aborted = simc_begin();
	HostXid		running = simc_begin();
	HostXid		sub = simc_begin_under(committed);
	HostXid		lone;
	TxnMemtableVersionWriter writers[5] = {{.xid = committed.f0},
	{.xid = sub.f0, .has_top = true, .top = committed.f0, .command = 1}, {0}, {.xid = aborted.f0},
	{.xid = running.f0}};
	TxnMemtableVersionHandle handles[3];
	HostDeltaHold hold;
	HostError	error;
	StdVec		pushed;
	StdVec		lent;
	StdVec		none;

	writers[2] = TXN_MEMTABLE_VERSION_NO_TRANSACTION;
	std_vec_with_capacity_Checked(&pushed, 5, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	for (size_t w = 0; w < lengthof(writers); w++)
	{
		TxnMemtableVersionHandle handle = txn_memtable_table_tests_pushes(3440, 44, io, state, "k", writers[w]);

		std_vec_push_Checked(&pushed, &handle, sizeof(handle), _Alignof(TxnMemtableVersionHandle));
	}
	simc_abort(sub);
	simc_commit(committed);
	simc_abort(aborted);
	txn_memtable_table_tests_prunes(3450, 47, io, state, "k");
	handles[0] = txn_memtable_table_tests_pushed_at(&pushed, 4, 3451, 40);
	handles[1] = txn_memtable_table_tests_pushed_at(&pushed, 2, 3451, 51);
	handles[2] = txn_memtable_table_tests_pushed_at(&pushed, 0, 3451, 62);
	txn_memtable_table_tests_handles_are(3451, 3447, 45, io, state, (const uint8 *) "k", 1, handles, 3);
	lone = simc_begin();
	(void) txn_memtable_table_tests_pushes(3455, 40, io, state, "q", txn_memtable_testing_writer(lone.f0));
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(3457, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_lent(3458, 46, io, &hold, state, (const uint8 *) "q", 1, &lent);
	std_index_check(0, lent.len, RUST_FILE, 3458, 13);
	txn_memtable_version_Version_set_hint(&STD_VEC_AT(&lent, TxnMemtableVersionVersion, 0),
										  TXN_MEMTABLE_VERSION_HINT_ABORTED);
	txn_memtable_table_tests_lent_free(&lent);
	host_delta_Hold_drop(&hold);
	txn_memtable_table_tests_prunes(3460, 47, io, state, "q");
	txn_memtable_table_tests_handles_of(3447, 40, 45, io, state, (const uint8 *) "q", 1, &none);
	STD_ASSERT(3461, 9, none.len == 0, "handles(io, b\"q\").is_empty()");
	std_vec_free_Checked(&none, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	txn_memtable_table_tests_walk_is(3462, io, state, NULL, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, k, 1);
	std_vec_free_Checked(&pushed, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_wrote records a key in the running transaction's written list and pushes a put to it by a transaction, as the take-back case's wrote closure does. */
static TxnMemtableVersionHandle
txn_memtable_table_tests_wrote(HostIoIo *io, HostSharedRecord state, const HostStorage *at, HostXid xid,
							   const char *key)
{
	HostError	error;
	uint64		touched;

	if (!txn_written_touched(io, 0, at, TXN_WRITTEN_OF_ROWS, (const uint8 *) key, strlen(key), 0, &touched, &error))
		STD_RESULT_EXPECT_FAILED(3478, 83, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return txn_memtable_table_tests_pushes(3486, 43, io, state, key, txn_memtable_testing_writer(xid.f0));
}

/*
 * An abort, a subtransaction's or its top's, leaves the versions its writers
 * pushed beside another transaction's committed ones, for the fold's logged
 * decision to drop (bugs row 432).
 */
void
test_txn_memtable_table_tests_an_abort_leaves_its_transactions_versions_for_a_fold(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9374);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage at = txn_memtable_testing_storage(9374);
	HostXid		top = simc_begin();
	HostXid		other = simc_begin();
	HostXid		sub;
	HostError	error;
	StdVec		handles;
	size_t		len;
	size_t		two = 2;

	(void) txn_memtable_table_tests_wrote(io, state, &at, other, "a");
	simc_commit(other);
	(void) txn_memtable_table_tests_wrote(io, state, &at, top, "a");
	if (!txn_written_sub_started(io, 2, &error))
		STD_RESULT_EXPECT_FAILED(3491, 44, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	sub = simc_begin_under(top);
	(void) txn_memtable_table_tests_wrote(io, state, &at, sub, "b");
	simc_abort(sub);
	if (!txn_written_sub_aborted(io, 2, &error))
		STD_RESULT_EXPECT_FAILED(3495, 44, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_handles_of(3497, 40, 45, io, state, (const uint8 *) "b", 1, &handles);
	STD_ASSERT(3500, 9, handles.len == 1, "the subtransaction's abort leaves its version");
	std_vec_free_Checked(&handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	simc_abort(top);
	if (!txn_written_aborted(io, 0, &error))
		STD_RESULT_EXPECT_FAILED(3503, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_handles_of(3497, 40, 45, io, state, (const uint8 *) "a", 1, &handles);
	len = handles.len;
	std_vec_free_Checked(&handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	STD_ASSERT_EQ(3501, 9, len == 2, &len, std_usize_Debug_fmt, &two, std_usize_Debug_fmt);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_retirer_t is a retired range case's writer's captures: its database, the key and version it takes out, and the key it then writes. */
typedef struct txn_memtable_table_tests_retirer_t
{
	uint32		database;
	const char *gone_key;
	TxnMemtableVersionHandle gone;
	const char *key;
} txn_memtable_table_tests_retirer_t;

/* txn_memtable_table_tests_retirer is a retired range case's writer: it takes the range's one key's version out, so the range is retired, then writes its key. */
static bool
txn_memtable_table_tests_retirer(void *captures, HostIoIo *io, HostError *error)
{
	const txn_memtable_table_tests_retirer_t *c = captures;
	HostSharedHeld held;
	TxnMemtableVersionHandle handle;
	StdVec		took;
	bool		ok;

	if (!txn_memtable_testing_stated(io, c->database, &held, error))
		return false;
	ok = txn_memtable_table_removed(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, host_shared_Held_record(&held),
									(const uint8 *) c->gone_key, strlen(c->gone_key), &c->gone, 1, &took, error);
	if (ok)
	{
		std_vec_free_Checked(&took, sizeof(bool), _Alignof(bool));
		ok = txn_memtable_table_tests_written(io, host_shared_Held_record(&held), (const uint8 *) c->key,
											  strlen(c->key), &handle, error);
	}
	host_shared_Held_drop(&held);
	return ok;
}

/* txn_memtable_table_tests_ranged_rows splits a storage's rows' memtable into ranges at their first keys, as ranged's unwrap does at its place. */
static void
txn_memtable_table_tests_ranged_rows(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state,
									 const StdBytes *firsts, size_t firsts_len)
{
	HostError	error;
	StdVec		ranges;

	if (!txn_memtable_testing_ranged(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, firsts, firsts_len, &ranges, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
}

/*
 * Case 24 (c): a forward walk held after its step chose a range, its stop
 * the next range's first key, while that next range is emptied and retired
 * and a key is then written past its old first key, into the first range:
 * the walk reads that key once, going on from the stop it took, for the
 * checkpoint's walk and for a scan's.
 */
void
test_txn_memtable_table_tests_a_walk_reads_once_a_key_written_where_the_next_range_was_retired(void)
{
	static const uint32 databases[2] = {9375, 9376};
	static const char *const walkers[2] = {"walked", "next"};
	static const char *const wanted[2] = {"a", "p"};
	static const StdBytes firsts[1] = {{(const uint8 *) "m", 1}};
	static const StdFmtPiece both[] = {STD_FMT_ARG(0), STD_FMT_LIT(": "), STD_FMT_ARG(1)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	TxnMemtableVersionHandle gone[2];
	StdVec		want;

	for (size_t k = 0; k < lengthof(databases); k++)
	{
		HostSharedHeld held = txn_memtable_testing_created(io, databases[k]);

		txn_memtable_table_tests_ranged_rows(3518, 56, io, host_shared_Held_record(&held), firsts, 1);
		(void) txn_memtable_table_tests_writes(3519, 46, io, host_shared_Held_record(&held), (const uint8 *) "a", 1);
		gone[k] = txn_memtable_table_tests_writes(3520, 56, io, host_shared_Held_record(&held), (const uint8 *) "n", 1);
		host_shared_Held_drop(&held);
	}
	txn_memtable_table_tests_keys_of(wanted, 2, &want);
	for (size_t k = 0; k < lengthof(databases); k++)
	{
		txn_memtable_table_tests_walker_t walker_captures = {databases[k], &want, k == 0,
		TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, "the walk handed out "};
		txn_memtable_table_tests_retirer_t writer_captures = {databases[k], "n", gone[k], "p"};
		SimcActor	walker = {txn_memtable_table_tests_walker, &walker_captures};
		SimcActor	writer = {txn_memtable_table_tests_retirer, &writer_captures};
		SimcStep	steps[2] = {{"writer", 6, PG_UINT64_MAX}, {walkers[k], strlen(walkers[k]), PG_UINT64_MAX}};
		StdStr		name = {walkers[k], strlen(walkers[k])};
		SimcRun		run = simc_Run_new(1);
		SimcReport	report;

		simc_Run_actor(&run, walkers[k], strlen(walkers[k]), walker);
		simc_Run_actor(&run, "writer", 6, writer);
		simc_Run_until(&run, walkers[k], strlen(walkers[k]), "doltgres-memtable-step");
		simc_Run_script(&run, steps, lengthof(steps));
		report = simc_Run_finish(run);
		if (!txn_memtable_table_tests_all_returned(&report, 3553, 71))
		{
			StdFmtArg	args[2] = {{&name, std_str_Display_fmt},
			{&report.outcome.u.ended, txn_memtable_table_tests_endings_Debug_fmt}};
			StdFmtArguments message = {both, lengthof(both), args};

			std_panic_fmt(RUST_FILE, 3554, 13, &message);
		}
		simc_Report_drop(&report);
	}
	txn_memtable_table_tests_keys_free(&want);
	simc_TestIo_drop(&test_io);
}

/*
 * Case 24 (h), its first half: a backward scan held after its step chose the
 * last range, while the range before it is emptied and retired and a key is
 * written below that range's old first key, into the floor range: the scan
 * lends each key once, in reverse.
 */
void
test_txn_memtable_table_tests_a_backward_scan_lends_each_key_once_across_a_retired_range_before_it(void)
{
	static const char *const wanted[3] = {"n", "i", "a"};
	static const StdBytes firsts[2] = {{(const uint8 *) "g", 1}, {(const uint8 *) "m", 1}};
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9377);
	txn_memtable_table_tests_walker_t scanner_captures = {9377, NULL, false, TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD,
	"the scan lent "};
	txn_memtable_table_tests_retirer_t writer_captures = {9377, "h", {0}, "i"};
	SimcActor	scanner = {txn_memtable_table_tests_walker, &scanner_captures};
	SimcActor	writer = {txn_memtable_table_tests_retirer, &writer_captures};
	SimcStep	steps[2] = {{"writer", 6, PG_UINT64_MAX}, {"scanner", 7, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;
	StdVec		want;

	txn_memtable_table_tests_ranged_rows(3570, 62, io, host_shared_Held_record(&held), firsts, 2);
	(void) txn_memtable_table_tests_writes(3571, 46, io, host_shared_Held_record(&held), (const uint8 *) "a", 1);
	(void) txn_memtable_table_tests_writes(3572, 46, io, host_shared_Held_record(&held), (const uint8 *) "n", 1);
	writer_captures.gone = txn_memtable_table_tests_writes(3573, 46, io, host_shared_Held_record(&held),
														   (const uint8 *) "h", 1);
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_keys_of(wanted, 3, &want);
	scanner_captures.want = &want;
	run = simc_Run_new(1);
	simc_Run_actor(&run, "scanner", 7, scanner);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_until(&run, "scanner", 7, "doltgres-memtable-step");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	if (!txn_memtable_table_tests_all_returned(&report, 3598, 67))
	{
		StdFmtArg	args[1] = {{&report.outcome.u.ended, txn_memtable_table_tests_endings_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 3599, 9, &message);
	}
	simc_Report_drop(&report);
	txn_memtable_table_tests_keys_free(&want);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_keys_are asserts a list of keys, as assert_eq on two Vec<Vec<u8>>s does at its place, and frees both. */
static void
txn_memtable_table_tests_keys_are(uint32 line, StdVec *keys, StdVec *expected)
{
	STD_ASSERT_EQ(line, 9, txn_memtable_table_tests_keys_eq(keys, expected), keys,
				  txn_memtable_table_tests_keys_Debug_fmt, expected, txn_memtable_table_tests_keys_Debug_fmt);
	txn_memtable_table_tests_keys_free(expected);
	txn_memtable_table_tests_keys_free(keys);
}

/* txn_memtable_table_tests_numbered_push pushes a numbered key onto a Vec<Vec<u8>>. */
static void
txn_memtable_table_tests_numbered_push(StdVec *keys, uint64 number)
{
	StdVec		key;

	txn_memtable_testing_numbered(number, &key);
	std_vec_push_Checked(keys, &key, sizeof(StdVec), _Alignof(StdVec));
}

/* txn_memtable_table_tests_removes_all takes every version of a numbered key out, the handles read for a hold of its own at their lines, as the split removal cases' loops do. */
static void
txn_memtable_table_tests_removes_all(uint32 line, uint32 removed_line, HostIoIo *io, HostSharedRecord state,
									 uint64 number)
{
	StdVec		key;
	StdVec		handles;

	txn_memtable_testing_numbered(number, &key);
	txn_memtable_table_tests_handles_of(line, 44, 50, io, state, key.ptr, key.len, &handles);
	txn_memtable_table_tests_removes(removed_line, 56, io, state, key.ptr, key.len, handles.ptr, handles.len);
	std_vec_free_Checked(&handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	std_vec_free_Checked(&key, 1, 1);
}

/*
 * PLAN's S6 (b) and (c): after a split, the upper half's own key, on its own
 * page, taken out while its other keys' versions are on the shared pages
 * leaves the half open with those keys; then every key of the upper half
 * taken out closes and retires it, and the lower half still reads its keys
 * from the shared pages it names.
 */
void
test_txn_memtable_table_tests_a_split_halfs_removals_keep_it_open_until_its_last_key_and_leave_the_shared_pages_to_the_other(void)
{
	static const char *const floor[1] = {""};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9378);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableVersionHandle own;
	HostError	error;
	StdVec		starts;
	StdVec		again;
	StdVec		key;
	StdVec		walked;
	StdVec		upper;
	StdVec		lower;
	size_t		len;
	size_t		two = 2;

	txn_memtable_table_tests_prefills(3613, 30, io, state);
	txn_memtable_table_tests_numbered_writes(3614, 46, io, state, TXN_MEMTABLE_SPLIT_SPLIT);
	if (!txn_memtable_table_range_starts(io, state, &starts, &error))
		STD_RESULT_EXPECT_FAILED(3615, 46, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	len = starts.len;
	STD_ASSERT_EQ(3616, 9, len == 2, &len, std_usize_Debug_fmt, &two, std_usize_Debug_fmt);
	txn_memtable_testing_numbered(TXN_MEMTABLE_SPLIT_SPLIT + 1, &key);
	own = txn_memtable_table_tests_writes(3617, 60, io, state, key.ptr, key.len);
	txn_memtable_table_tests_removes(3618, 66, io, state, key.ptr, key.len, &own, 1);
	std_vec_free_Checked(&key, 1, 1);
	if (!txn_memtable_table_range_starts(io, state, &again, &error))
		STD_RESULT_EXPECT_FAILED(3619, 44, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(3619, 9, txn_memtable_table_tests_keys_eq(&again, &starts), &again,
				  txn_memtable_table_tests_keys_Debug_fmt, &starts, txn_memtable_table_tests_keys_Debug_fmt);
	txn_memtable_table_tests_keys_free(&again);
	std_vec_new(&upper, sizeof(StdVec), _Alignof(StdVec));
	for (uint64 number = 16383; number < TXN_MEMTABLE_SPLIT_SPLIT - 1; number++)
		txn_memtable_table_tests_numbered_push(&upper, number);
	txn_memtable_table_tests_numbered_push(&upper, TXN_MEMTABLE_SPLIT_SPLIT);
	txn_memtable_testing_numbered(16382, &key);
	txn_memtable_table_tests_walk(io, state, true, key.ptr, key.len, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &walked);
	std_vec_free_Checked(&key, 1, 1);
	STD_ASSERT_EQ(3621, 9, txn_memtable_table_tests_keys_eq(&walked, &upper), &walked,
				  txn_memtable_table_tests_keys_Debug_fmt, &upper, txn_memtable_table_tests_keys_Debug_fmt);
	txn_memtable_table_tests_keys_free(&walked);
	txn_memtable_table_tests_keys_free(&upper);
	for (uint64 number = 16383; number < TXN_MEMTABLE_SPLIT_SPLIT - 1; number++)
		txn_memtable_table_tests_removes_all(3628, 3631, io, state, number);
	txn_memtable_table_tests_removes_all(3628, 3631, io, state, TXN_MEMTABLE_SPLIT_SPLIT);
	txn_memtable_table_tests_starts_are(3633, io, state, floor, 1);
	std_vec_with_capacity_Checked(&lower, 16383, sizeof(StdVec), _Alignof(StdVec));
	for (uint64 number = 0; number < 16383; number++)
		txn_memtable_table_tests_numbered_push(&lower, number);
	txn_memtable_table_tests_walk(io, state, false, NULL, 0, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &walked);
	txn_memtable_table_tests_keys_are(3635, &walked, &lower);
	txn_memtable_table_tests_keys_free(&starts);
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(3637, 38, io, 9378);
	txn_memtable_table_tests_used_is(3639, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/*
 * A split of a range whose list holds pages the remover dropped seals and
 * counts only the pages the list still names, so the halves read every key
 * left and, with the storage unlinked, every page goes once.
 */
void
test_txn_memtable_table_tests_a_split_after_the_remover_dropped_pages_counts_only_the_pages_its_list_names(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9379);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostError	error;
	StdVec		starts;
	StdVec		walked;
	StdVec		left;
	size_t		len;
	size_t		two = 2;

	txn_memtable_table_tests_prefills(3652, 30, io, state);
	for (uint64 number = 1; number < 4097; number++)
		txn_memtable_table_tests_removes_all(3657, 3660, io, state, number);
	for (uint64 number = TXN_MEMTABLE_SPLIT_SPLIT - 1; number < TXN_MEMTABLE_SPLIT_SPLIT + 4097; number++)
		txn_memtable_table_tests_numbered_writes(3664, 51, io, state, number);
	if (!txn_memtable_table_range_starts(io, state, &starts, &error))
		STD_RESULT_EXPECT_FAILED(3666, 44, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	len = starts.len;
	txn_memtable_table_tests_keys_free(&starts);
	STD_ASSERT_EQ(3666, 9, len == 2, &len, std_usize_Debug_fmt, &two, std_usize_Debug_fmt);
	std_vec_new(&left, sizeof(StdVec), _Alignof(StdVec));
	for (uint64 number = 0; number < TXN_MEMTABLE_SPLIT_SPLIT + 4097; number++)
		if (!(number >= 1 && number < 4097))
			txn_memtable_table_tests_numbered_push(&left, number);
	txn_memtable_table_tests_walk(io, state, false, NULL, 0, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &walked);
	txn_memtable_table_tests_keys_are(3669, &walked, &left);
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(3671, 38, io, 9379);
	txn_memtable_table_tests_used_is(3673, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_entry returns the name a range's list stores at an index. */
static uint64
txn_memtable_table_tests_entry(HostIoIo *io, uint64 location, uint64 index)
{
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostDeltaListListView view;
	HostError	error;
	uint64		name;

	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(3678, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, location, &record, &error))
		STD_RESULT_EXPECT_FAILED(3679, 53, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(3680, 57, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	view = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES);
	if (!host_delta_list_ListView_get(&view, &hold, index, &name, &error))
		STD_RESULT_EXPECT_FAILED(3681, 49, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	return name;
}

/* txn_memtable_table_tests_listed_pages returns the names a range's list stores, in its order. */
static void
txn_memtable_table_tests_listed_pages(HostIoIo *io, uint64 range, StdVec *result)
{
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostError	error;

	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(4360, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, range, &record, &error))
		STD_RESULT_EXPECT_FAILED(4361, 50, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(4362, 57, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_pages_of(4364, 60, &guard, &hold, result);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
}

/* txn_memtable_table_tests_taken_out takes every version of a key out of a storage's rows. */
static void
txn_memtable_table_tests_taken_out(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len)
{
	StdVec		handles;

	txn_memtable_table_tests_handles_of(3764, 40, 45, io, state, key, key_len, &handles);
	txn_memtable_table_tests_removes(3767, 51, io, state, key, key_len, handles.ptr, handles.len);
	std_vec_free_Checked(&handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
}

/* txn_memtable_table_tests_keys_contains reports whether a Vec<Vec<u8>> holds a key. */
static bool
txn_memtable_table_tests_keys_contains(const StdVec *keys, const StdVec *key)
{
	for (size_t k = 0; k < keys->len; k++)
	{
		const StdVec *other = &STD_VEC_AT(keys, StdVec, k);

		if (other->len == key->len && memcmp(other->ptr, key->ptr, key->len) == 0)
			return true;
	}
	return false;
}

/* txn_memtable_table_tests_lists loads a page's count of lists that name it, as the sweep case's lists closure does. */
static uint64
txn_memtable_table_tests_lists(HostIoIo *io, uint64 name)
{
	HostDeltaHold hold;
	HostDeltaPageRef page;
	HostDeltaWord word;
	HostError	error;
	uint64		lists;

	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(3718, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_page(&hold, host_delta_page_Page_named(name), &page, &error))
		STD_RESULT_EXPECT_FAILED(3719, 42, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_LISTS, &word, &error))
		STD_RESULT_EXPECT_FAILED(3719, 63, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	lists = host_delta_Word_load(&word);
	host_delta_Hold_drop(&hold);
	return lists;
}

/* txn_memtable_table_tests_entry_is asserts the name a range's list stores at index 1, as the sweep case's assert_eq does at its place. */
static void
txn_memtable_table_tests_entry_is(uint32 line, HostIoIo *io, const StdVec *ranges, size_t index, uint64 wanted)
{
	uint64		name = txn_memtable_table_tests_entry(io, txn_memtable_table_tests_range_at(ranges, index, line, 30), 1);

	STD_ASSERT_EQ(line, 9, name == wanted, &name, std_u64_Debug_fmt, &wanted, std_u64_Debug_fmt);
}

/*
 * PLAN's sweep: a sealed page a split's halves share, whose versions are all
 * removed, is dropped from each half's list by that half's next removal that
 * touches it or its next look at every page, within its page count of
 * removals (bugs row 110), the old index's nodes on it no longer counted,
 * and the last drop retires it.
 */
void
test_txn_memtable_table_tests_a_sweep_drops_a_split_ranges_sealed_empty_page_from_both_halves(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9380);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostDeltaHold hold;
	HostDeltaPageRef page;
	HostError	error;
	StdVec		ranges;
	StdVec		keys;
	StdVec		gone;
	StdVec		pages;
	StdVec		left;
	StdVec		walked;
	uint64		kept;
	uint64		name;
	uint64		lists;
	uint64		one = 1;
	size_t		len;
	size_t		two = 2;
	size_t		count;
	size_t		taken = 0;
	bool		opened;

	txn_memtable_table_tests_prefills(3695, 30, io, state);
	txn_memtable_table_tests_numbered_writes(3696, 46, io, state, TXN_MEMTABLE_SPLIT_SPLIT);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(3697, 45, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	len = ranges.len;
	STD_ASSERT_EQ(3698, 9, len == 2, &len, std_usize_Debug_fmt, &two, std_usize_Debug_fmt);
	name = txn_memtable_table_tests_entry(io, txn_memtable_table_tests_range_at(&ranges, 0, 3699, 30), 1);
	txn_memtable_table_tests_entry_is(3700, io, &ranges, 1, name);
	txn_memtable_table_tests_walk(io, state, false, NULL, 0, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &keys);
	std_vec_new(&gone, sizeof(StdVec), _Alignof(StdVec));
	for (size_t k = 0; k < keys.len; k++)
	{
		const StdVec *key = &STD_VEC_AT(&keys, StdVec, k);
		StdVec		handles;
		bool		any = false;
		bool		all = true;

		txn_memtable_table_tests_handles_of(3705, 44, 49, io, state, key->ptr, key->len, &handles);
		for (size_t h = 0; h < handles.len; h++)
		{
			bool		on = txn_memtable_version_Handle_page(STD_VEC_AT(&handles, TxnMemtableVersionHandle, h)) == 1;

			any |= on;
			all &= on;
		}
		if (any)
		{
			StdVec		copy;

			STD_ASSERT(3709, 17, all, "handles.iter().all(|handle| handle.page() == 1)");
			txn_memtable_table_tests_removes(3710, 59, io, state, key->ptr, key->len, handles.ptr, handles.len);
			std_slice_to_vec_Checked(&copy, key->ptr, key->len, 1, 1);
			std_vec_push_Checked(&gone, &copy, sizeof(StdVec), _Alignof(StdVec));
		}
		std_vec_free_Checked(&handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	}
	STD_ASSERT(3714, 9, gone.len > 0, "!gone.is_empty()");
	txn_memtable_table_tests_entry_is(3715, io, &ranges, 0, TXN_MEMTABLE_PAGES_DROPPED);
	txn_memtable_table_tests_entry_is(3716, io, &ranges, 1, name);
	lists = txn_memtable_table_tests_lists(io, name);
	STD_ASSERT_EQ(3721, 9, lists == 1, &lists, std_u64_Debug_fmt, &one, std_u64_Debug_fmt);
	txn_memtable_table_tests_listed_pages(io, txn_memtable_table_tests_range_at(&ranges, 1, 3722, 38), &pages);
	count = pages.len;
	std_vec_free_Checked(&pages, sizeof(uint64), _Alignof(uint64));
	for (size_t k = keys.len; k > 0 && taken < count; k--)
	{
		const StdVec *key = &STD_VEC_AT(&keys, StdVec, k - 1);
		StdVec		copy;

		if (txn_memtable_table_tests_keys_contains(&gone, key))
			continue;
		taken++;
		if (txn_memtable_table_tests_entry(io, txn_memtable_table_tests_range_at(&ranges, 1, 3725, 26), 1) ==
			TXN_MEMTABLE_PAGES_DROPPED)
			break;
		txn_memtable_table_tests_taken_out(io, state, key->ptr, key->len);
		std_slice_to_vec_Checked(&copy, key->ptr, key->len, 1, 1);
		std_vec_push_Checked(&gone, &copy, sizeof(StdVec), _Alignof(StdVec));
	}
	txn_memtable_table_tests_entry_is(3731, io, &ranges, 1, TXN_MEMTABLE_PAGES_DROPPED);
	txn_memtable_table_tests_passed(io);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(3733, 33, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	opened = host_delta_Hold_page(&hold, host_delta_page_Page_named(name), &page, &error);
	if (!opened)
		host_Error_drop(&error);
	host_delta_Hold_drop(&hold);
	STD_ASSERT(3733, 9, !opened, "delta::hold(io).unwrap().page(Page::named(name)).is_err()");
	std_vec_new(&left, sizeof(StdVec), _Alignof(StdVec));
	for (size_t k = 0; k < keys.len; k++)
	{
		StdVec	   *key = &STD_VEC_AT(&keys, StdVec, k);

		if (txn_memtable_table_tests_keys_contains(&gone, key))
			std_vec_free_Checked(key, 1, 1);
		else
			std_vec_push_Checked(&left, key, sizeof(StdVec), _Alignof(StdVec));
	}
	std_vec_free_Checked(&keys, sizeof(StdVec), _Alignof(StdVec));
	txn_memtable_table_tests_walk(io, state, false, NULL, 0, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &walked);
	txn_memtable_table_tests_keys_are(3735, &walked, &left);
	txn_memtable_table_tests_keys_free(&gone);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(3737, 38, io, 9380);
	txn_memtable_table_tests_used_is(3739, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_read_version_t is one of a Read's versions: its handle and its record's bytes. */
typedef struct txn_memtable_table_tests_read_version_t
{
	TxnMemtableVersionHandle handle;
	StdVec		record;
} txn_memtable_table_tests_read_version_t;

/* txn_memtable_table_tests_read_t is Read: a key with its versions' handles and records. */
typedef struct txn_memtable_table_tests_read_t
{
	StdVec		key;
	StdVec		read;
} txn_memtable_table_tests_read_t;

/* txn_memtable_table_tests_read_version_Debug_fmt is a (Handle, Vec<u8>)'s Debug. */
static bool
txn_memtable_table_tests_read_version_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_table_tests_read_version_t *version = value;
	StdBytes	record = {version->record.ptr, version->record.len};
	StdDebugTuple t = std_fmt_debug_tuple(f, "", 0);

	std_DebugTuple_field(&t, &version->handle, txn_memtable_version_Handle_Debug_fmt);
	std_DebugTuple_field(&t, &record, std_bytes_Debug_fmt);
	return std_DebugTuple_finish(&t);
}

/* txn_memtable_table_tests_read_versions_Debug_fmt is a Vec<(Handle, Vec<u8>)>'s Debug. */
static bool
txn_memtable_table_tests_read_versions_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *read = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < read->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(read, txn_memtable_table_tests_read_version_t, k),
							txn_memtable_table_tests_read_version_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_memtable_table_tests_read_Debug_fmt is a Read's Debug. */
static bool
txn_memtable_table_tests_read_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_table_tests_read_t *read = value;
	StdBytes	key = {read->key.ptr, read->key.len};
	StdDebugTuple t = std_fmt_debug_tuple(f, "", 0);

	std_DebugTuple_field(&t, &key, std_bytes_Debug_fmt);
	std_DebugTuple_field(&t, &read->read, txn_memtable_table_tests_read_versions_Debug_fmt);
	return std_DebugTuple_finish(&t);
}

/* txn_memtable_table_tests_reads_Debug_fmt is a Vec<Read>'s Debug. */
static bool
txn_memtable_table_tests_reads_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *reads = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < reads->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(reads, txn_memtable_table_tests_read_t, k),
							txn_memtable_table_tests_read_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_memtable_table_tests_reads_eq reports whether two Vec<Read>s are equal. */
static bool
txn_memtable_table_tests_reads_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t k = 0; k < a->len; k++)
	{
		const txn_memtable_table_tests_read_t *x = &STD_VEC_AT(a, txn_memtable_table_tests_read_t, k);
		const txn_memtable_table_tests_read_t *y = &STD_VEC_AT(b, txn_memtable_table_tests_read_t, k);

		if (x->key.len != y->key.len || memcmp(x->key.ptr, y->key.ptr, x->key.len) != 0 ||
			x->read.len != y->read.len)
			return false;
		for (size_t v = 0; v < x->read.len; v++)
		{
			const txn_memtable_table_tests_read_version_t *p = &STD_VEC_AT(&x->read,
																		   txn_memtable_table_tests_read_version_t,
																		   v);
			const txn_memtable_table_tests_read_version_t *q = &STD_VEC_AT(&y->read,
																		   txn_memtable_table_tests_read_version_t,
																		   v);

			if (!txn_memtable_version_Handle_PartialEq_eq(&p->handle, &q->handle) ||
				p->record.len != q->record.len || memcmp(p->record.ptr, q->record.ptr, p->record.len) != 0)
				return false;
		}
	}
	return true;
}

/* txn_memtable_table_tests_read_free frees a Read's key, records, and list. */
static void
txn_memtable_table_tests_read_free(txn_memtable_table_tests_read_t *read)
{
	std_vec_free_Checked(&read->key, 1, 1);
	for (size_t v = 0; v < read->read.len; v++)
		std_vec_free_Checked(&STD_VEC_AT(&read->read, txn_memtable_table_tests_read_version_t, v).record, 1, 1);
	std_vec_free_Checked(&read->read, sizeof(txn_memtable_table_tests_read_version_t),
						 _Alignof(txn_memtable_table_tests_read_version_t));
}

/* txn_memtable_table_tests_reads_free frees a Vec<Read> and its reads. */
static void
txn_memtable_table_tests_reads_free(StdVec *reads)
{
	for (size_t k = 0; k < reads->len; k++)
		txn_memtable_table_tests_read_free(&STD_VEC_AT(reads, txn_memtable_table_tests_read_t, k));
	std_vec_free_Checked(reads, sizeof(txn_memtable_table_tests_read_t), _Alignof(txn_memtable_table_tests_read_t));
}

/* txn_memtable_table_tests_on_page returns the keys of a storage whose versions are all on a page of its range's list, by the page's index, each with its versions' handles and records. */
static void
txn_memtable_table_tests_on_page(HostIoIo *io, HostSharedRecord state, uint64 entry, StdVec *found)
{
	StdVec		keys;

	std_vec_new(found, sizeof(txn_memtable_table_tests_read_t), _Alignof(txn_memtable_table_tests_read_t));
	txn_memtable_table_tests_walk(io, state, false, NULL, 0, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &keys);
	for (size_t k = 0; k < keys.len; k++)
	{
		txn_memtable_table_tests_read_t read = {.key = STD_VEC_AT(&keys, StdVec, k)};
		HostDeltaHold hold;
		HostError	error;
		StdVec		lent;
		bool		all = true;

		if (!host_delta_hold(io, &hold, &error))
			STD_RESULT_EXPECT_FAILED(3750, 40, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_table_tests_lent(3751, 57, io, &hold, state, read.key.ptr, read.key.len, &lent);
		std_vec_with_capacity_Checked(&read.read, lent.len, sizeof(txn_memtable_table_tests_read_version_t),
									  _Alignof(txn_memtable_table_tests_read_version_t));
		for (size_t v = 0; v < lent.len; v++)
		{
			const TxnMemtableVersionVersion *version = &STD_VEC_AT(&lent, TxnMemtableVersionVersion, v);
			TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
			StdBytes	bytes = txn_memtable_version_Record_bytes(&record);
			txn_memtable_table_tests_read_version_t copy = {.handle = version->handle};

			std_slice_to_vec_Checked(&copy.record, bytes.ptr, bytes.len, 1, 1);
			std_vec_push_Checked(&read.read, &copy, sizeof(txn_memtable_table_tests_read_version_t),
								 _Alignof(txn_memtable_table_tests_read_version_t));
		}
		for (size_t v = 0; all && v < read.read.len; v++)
			all = txn_memtable_version_Handle_page(STD_VEC_AT(&read.read, txn_memtable_table_tests_read_version_t,
															  v).handle) == entry;
		if (all)
			std_vec_push_Checked(found, &read, sizeof(txn_memtable_table_tests_read_t),
								 _Alignof(txn_memtable_table_tests_read_t));
		else
			txn_memtable_table_tests_read_free(&read);
		txn_memtable_table_tests_lent_free(&lent);
		host_delta_Hold_drop(&hold);
	}
	std_vec_free_Checked(&keys, sizeof(StdVec), _Alignof(StdVec));
}

/* txn_memtable_table_tests_counts returns a page's live count, its dead bytes, and the bytes it holds. */
static void
txn_memtable_table_tests_counts(HostIoIo *io, uint64 name, uint64 *live, uint64 *dead, uint64 *filled)
{
	HostDeltaHold hold;
	HostDeltaPageRef page;
	HostDeltaWord word;
	HostError	error;

	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(3772, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_page(&hold, host_delta_page_Page_named(name), &page, &error))
		STD_RESULT_EXPECT_FAILED(3773, 49, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_LIVE, &word, &error))
		STD_RESULT_EXPECT_FAILED(3774, 33, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	*live = host_delta_Word_load(&word);
	if (!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_DEAD, &word, &error))
		STD_RESULT_EXPECT_FAILED(3774, 73, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	*dead = host_delta_Word_load(&word);
	*filled = host_delta_PageRef_filled(&page);
	host_delta_Hold_drop(&hold);
}

/* txn_memtable_table_tests_compacts reports whether a writer's wait compacted, as deferred's unwrap does at its place. */
static bool
txn_memtable_table_tests_compacts(uint32 line, uint32 col, HostIoIo *io)
{
	HostError	error;
	bool		compacted;

	if (!txn_memtable_table_deferred(io, &compacted, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return compacted;
}

/*
 * 2g's compaction: a page of a range's own whose removed records pass half
 * its bytes is copied, once a writer's wait runs the compaction its range
 * deferred (bugs row 110), its live records only, over its index; every key
 * left reads the same handles and records after, the copy counts the same
 * live records and holds fewer bytes, and the old page is retired once the
 * reader that held it before ends its hold.
 */
void
test_txn_memtable_table_tests_a_page_over_half_dead_is_compacted_and_every_live_handle_reads_the_same(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9381);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostDeltaHold reader;
	HostDeltaHold hold;
	HostDeltaPageRef page;
	HostError	error;
	StdVec		ranges;
	StdVec		keys;
	StdVec		kept;
	StdVec		again;
	uint64		listed_kept;
	uint64		old;
	uint64		new;
	uint64		live;
	uint64		dead;
	uint64		filled;
	uint64		after;
	uint64		bytes;
	uint64		wanted;
	bool		opened;

	for (uint64 number = 0; number < 2000; number++)
		txn_memtable_table_tests_numbered_writes(3790, 51, io, state, number);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &listed_kept, &error))
		STD_RESULT_EXPECT_FAILED(3792, 45, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	old = txn_memtable_table_tests_entry(io, txn_memtable_table_tests_range_at(&ranges, 0, 3793, 29), 1);
	txn_memtable_table_tests_on_page(io, state, 1, &keys);
	STD_ASSERT(3795, 9, keys.len > 8, "keys.len() > 8");
	txn_memtable_table_tests_counts(io, old, &live, &dead, &filled);
	if (!host_delta_hold(io, &reader, &error))
		STD_RESULT_EXPECT_FAILED(3797, 38, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);

	std_vec_new(&kept, sizeof(txn_memtable_table_tests_read_t), _Alignof(txn_memtable_table_tests_read_t));
	for (size_t k = 0; k < keys.len; k += 4)
		std_vec_push_Checked(&kept, &STD_VEC_AT(&keys, txn_memtable_table_tests_read_t, k),
							 sizeof(txn_memtable_table_tests_read_t), _Alignof(txn_memtable_table_tests_read_t));
	for (size_t k = 0; k < keys.len; k++)
	{
		const txn_memtable_table_tests_read_t *key = &STD_VEC_AT(&keys, txn_memtable_table_tests_read_t, k);

		if (STD_REM_FOLDED(usize, k, 4) != 0)
			txn_memtable_table_tests_taken_out(io, state, key->key.ptr, key->key.len);
	}
	txn_memtable_table_tests_entry_is(3802, io, &ranges, 0, old);
	STD_ASSERT(3803, 9, txn_memtable_table_tests_compacts(3803, 30, io), "deferred(io).unwrap()");
	new = txn_memtable_table_tests_entry(io, txn_memtable_table_tests_range_at(&ranges, 0, 3804, 29), 1);
	if (new == old)
		std_assert_failed(RUST_FILE, 3805, 9, "!=", &new, std_u64_Debug_fmt, &old, std_u64_Debug_fmt, NULL);
	txn_memtable_table_tests_counts(io, new, &after, &dead, &bytes);
	wanted = STD_SUB(u64, live, STD_MUL(u64, 2, (uint64) STD_SUB(usize, keys.len, kept.len, 3807, 39), 3807, 34),
					 3807, 27);
	STD_ASSERT_EQ(3807, 9, after == wanted, &after, std_u64_Debug_fmt, &wanted, std_u64_Debug_fmt);
	STD_ASSERT(3808, 9, bytes < STD_DIV_FOLDED(u64, filled, 2), "bytes < filled / 2");
	STD_ASSERT(3809, 9, STD_MUL(u64, dead, 2, 3809, 17) <= bytes, "dead * 2 <= bytes");
	txn_memtable_table_tests_on_page(io, state, 1, &again);
	STD_ASSERT_EQ(3810, 9, txn_memtable_table_tests_reads_eq(&again, &kept), &again,
				  txn_memtable_table_tests_reads_Debug_fmt, &kept, txn_memtable_table_tests_reads_Debug_fmt);
	txn_memtable_table_tests_reads_free(&again);
	opened = host_delta_Hold_page(&reader, host_delta_page_Page_named(old), &page, &error);
	if (!opened)
		host_Error_drop(&error);
	STD_ASSERT(3811, 9, opened, "reader.page(Page::named(old)).is_ok()");
	host_delta_Hold_drop(&reader);
	txn_memtable_table_tests_passed(io);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(3814, 33, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	opened = host_delta_Hold_page(&hold, host_delta_page_Page_named(old), &page, &error);
	if (!opened)
		host_Error_drop(&error);
	host_delta_Hold_drop(&hold);
	STD_ASSERT(3814, 9, !opened, "delta::hold(io).unwrap().page(Page::named(old)).is_err()");
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(3816, 38, io, 9381);
	txn_memtable_table_tests_used_is(3818, 9, io, start);
	std_vec_free_Checked(&kept, sizeof(txn_memtable_table_tests_read_t), _Alignof(txn_memtable_table_tests_read_t));
	txn_memtable_table_tests_reads_free(&keys);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	simc_TestIo_drop(&test_io);
}

/* 2g's compaction never copies a range's first page, however much of it is removed. */
void
test_txn_memtable_table_tests_a_ranges_first_page_is_never_compacted(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9382);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostError	error;
	StdVec		ranges;
	StdVec		keys;
	StdVec		again;
	StdVec		head;
	uint64		listed_kept;
	uint64		first;
	uint64		name;
	uint64		live;
	uint64		dead;
	uint64		filled;

	for (uint64 number = 0; number < 2000; number++)
		txn_memtable_table_tests_numbered_writes(3830, 51, io, state, number);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &listed_kept, &error))
		STD_RESULT_EXPECT_FAILED(3832, 45, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	first = txn_memtable_table_tests_entry(io, txn_memtable_table_tests_range_at(&ranges, 0, 3833, 31), 0);
	txn_memtable_table_tests_on_page(io, state, 0, &keys);
	STD_ASSERT(3835, 9, keys.len > 1, "keys.len() > 1");
	std_range_from_check(1, keys.len, RUST_FILE, 3836, 26);
	for (size_t k = 1; k < keys.len; k++)
	{
		const txn_memtable_table_tests_read_t *key = &STD_VEC_AT(&keys, txn_memtable_table_tests_read_t, k);

		txn_memtable_table_tests_taken_out(io, state, key->key.ptr, key->key.len);
	}
	txn_memtable_table_tests_counts(io, first, &live, &dead, &filled);
	STD_ASSERT(3840, 9, STD_MUL(u64, dead, 2, 3840, 17) > filled, "dead * 2 > filled");
	name = txn_memtable_table_tests_entry(io, txn_memtable_table_tests_range_at(&ranges, 0, 3841, 30), 0);
	STD_ASSERT_EQ(3841, 9, name == first, &name, std_u64_Debug_fmt, &first, std_u64_Debug_fmt);
	txn_memtable_table_tests_on_page(io, state, 0, &again);
	std_range_to_check(1, keys.len, RUST_FILE, 3842, 43);
	head.ptr = keys.ptr;
	head.len = 1;
	head.cap = 1;
	STD_ASSERT_EQ(3842, 9, txn_memtable_table_tests_reads_eq(&again, &head), &again,
				  txn_memtable_table_tests_reads_Debug_fmt, &head, txn_memtable_table_tests_reads_Debug_fmt);
	txn_memtable_table_tests_reads_free(&again);
	txn_memtable_table_tests_reads_free(&keys);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_fills takes pages of a size until the delta refuses one, adding them to taken, as the budget cases' loops of alloc do at their place. */
static void
txn_memtable_table_tests_fills(uint32 line, uint32 col, HostIoIo *io, HostDeltaPageSize size, StdVec *taken)
{
	for (;;)
	{
		HostDeltaFull full;
		HostError	error;
		StdVec		pages;
		bool		ok;

		if (!host_delta_alloc(io, &size, 1, &ok, &pages, &full, &error))
			STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!ok)
			return;
		for (size_t k = 0; k < pages.len; k++)
			std_vec_push_Checked(taken, &STD_VEC_AT(&pages, HostDeltaOwned, k), sizeof(HostDeltaOwned),
								 _Alignof(HostDeltaOwned));
		std_vec_free_Checked(&pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	}
}

/* txn_memtable_table_tests_usizes_Debug_fmt is a Vec<usize>'s Debug. */
bool
txn_memtable_table_tests_usizes_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < v->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(v, size_t, k), std_usize_Debug_fmt);
	return std_DebugList_finish(&list);
}

/*
 * Bugs rows 110 and 116's overshoot: with the delta's budget full, a
 * writer's wait runs the compaction its range deferred, whose copy takes the
 * region's one block past the budget, while a second range's compaction
 * waits for the overshoot until the pass frees the page the first replaced,
 * and then takes a free slot of that block; once the pages they replaced are
 * freed, the block those shared goes back, so the delta ends where it
 * started, and the next wait finds nothing left to compact.
 */
void
test_txn_memtable_table_tests_a_compaction_at_the_budget_takes_its_copies_past_it_in_one_block(void)
{
	static const uint32 databases[2] = {9383, 9385};
	static const size_t wanted[2] = {1, 2};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostDeltaPageSize size = {.bytes = 4 << 10, .words = 4};
	HostSharedHeld helds[2];
	HostError	error;
	StdVec		taken;
	size_t		compacted[2];
	StdVec		compacted_vec = {compacted, 2, 2};
	StdVec		wanted_vec = {(void *) wanted, 2, 2};
	StdTupleRef left;
	StdTupleRef right;
	uint64		ranges_of[2];
	uint64		olds[2];
	uint64		full;
	uint64		used;

	for (size_t d = 0; d < lengthof(databases); d++)
	{
		HostSharedRecord state;
		StdVec		ranges;
		uint64		kept;

		helds[d] = txn_memtable_testing_created(io, databases[d]);
		state = host_shared_Held_record(&helds[d]);
		for (uint64 number = 0; number < 2000; number++)
			txn_memtable_table_tests_numbered_writes(3861, 55, io, state, number);
		if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
			STD_RESULT_EXPECT_FAILED(3863, 49, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		ranges_of[d] = txn_memtable_table_tests_range_at(&ranges, 0, 3864, 24);
		olds[d] = txn_memtable_table_tests_entry(io, txn_memtable_table_tests_range_at(&ranges, 0, 3864, 45), 4);
		std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	}
	std_vec_new(&taken, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	txn_memtable_table_tests_fills(3868, 86, io, size, &taken);
	for (size_t d = 0; d < lengthof(helds); d++)
	{
		HostSharedRecord state = host_shared_Held_record(&helds[d]);
		StdVec		keys;
		size_t		end;

		txn_memtable_table_tests_on_page(io, state, 4, &keys);
		STD_ASSERT(3873, 13, keys.len > 8, "keys.len() > 8");
		end = STD_SUB(usize, keys.len, 1, 3874, 38);
		std_range_check(1, end, keys.len, RUST_FILE, 3874, 30);
		for (size_t k = 1; k < end; k++)
		{
			const txn_memtable_table_tests_read_t *key = &STD_VEC_AT(&keys, txn_memtable_table_tests_read_t, k);

			txn_memtable_table_tests_taken_out(io, state, key->key.ptr, key->key.len);
		}
		txn_memtable_table_tests_reads_free(&keys);
	}
	full = host_delta_used();
	for (size_t pass = 0; pass < 2; pass++)
	{
		STD_ASSERT(3881, 13, txn_memtable_table_tests_compacts(3881, 34, io), "deferred(io).unwrap()");
		STD_ASSERT(3882, 13, host_delta_used() <= STD_ADD(u64, full, HOST_DELTA_PAGE_BLOCK, 3882, 38),
				   "delta::used() <= full + delta::page::BLOCK");
		compacted[pass] = 0;
		for (size_t d = 0; d < lengthof(olds); d++)
			if (txn_memtable_table_tests_entry(io, ranges_of[d], 4) != olds[d])
				compacted[pass]++;
		txn_memtable_table_tests_passed(io);
	}
	used = host_delta_used();
	left = STD_TUPLE(2, STD_ITEM(&compacted_vec, txn_memtable_table_tests_usizes_Debug_fmt),
					 STD_ITEM(&used, std_u64_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&wanted_vec, txn_memtable_table_tests_usizes_Debug_fmt),
					  STD_ITEM(&full, std_u64_Debug_fmt));
	STD_ASSERT_EQ(3886, 9, compacted[0] == 1 && compacted[1] == 2 && used == full, &left, std_tuple_Debug_fmt,
				  &right, std_tuple_Debug_fmt);
	STD_ASSERT(3887, 9, !txn_memtable_table_tests_compacts(3887, 31, io), "!deferred(io).unwrap()");
	std_vec_drop_Checked(&taken, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
	for (size_t d = 0; d < lengthof(helds); d++)
		host_shared_Held_drop(&helds[d]);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 235: a split whose memtable was retired, as a fold retires it, in
 * the window from the end of the push's hold to a try's hold, here before its
 * first try, finds that its owner names no memtable once its hold begins, and
 * ends without opening the one it was given, which no hold begun since
 * reaches. A C-only case.
 */
void
test_txn_memtable_table_tests_a_split_whose_memtable_was_retired_before_its_hold_ends_without_opening_it(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9327);
	HostSharedRecord state = host_shared_Held_record(&held);
	TxnMemtableTableAt at;
	TxnMemtableSplitSeen seen;
	HostError	error;
	uint64		named;
	StdOptionRef got = {.some = STD_ITEM(&named, std_u64_Debug_fmt)};
	StdOptionRef none = {.some = STD_ITEM(&named, std_u64_Debug_fmt)};

	at = txn_memtable_table_tests_oversized(io, state, &seen);
	if (!txn_memtable_table_retired(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_split_split(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, at, &seen, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	got.has = txn_memtable_table_memtable(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, state, &named);
	STD_ASSERT_EQ_MSG(__LINE__, 0, !got.has, &got, std_option_Debug_fmt, &none, std_option_Debug_fmt,
					  "the split made its owner a memtable again");
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_compactor is the dropped page case's compactor: a writer's wait's compaction, its answer dropped. */
static bool
txn_memtable_table_tests_compactor(void *captures, HostIoIo *io, HostError *error)
{
	bool		compacted;

	return txn_memtable_table_deferred(io, &compacted, error);
}

/* txn_memtable_table_tests_page_dropper_t is the dropped page case's dropper's captures: the first and last keys left on the page. */
typedef struct txn_memtable_table_tests_page_dropper_t
{
	const StdVec *first;
	const StdVec *last;
} txn_memtable_table_tests_page_dropper_t;

/* txn_memtable_table_tests_page_dropper is the dropped page case's dropper: it takes out the page's last two live keys, which drops the page from its range's list. */
static bool
txn_memtable_table_tests_page_dropper(void *captures, HostIoIo *io, HostError *error)
{
	const txn_memtable_table_tests_page_dropper_t *c = captures;
	HostSharedHeld held;

	if (!txn_memtable_testing_stated(io, 9384, &held, error))
		return false;
	txn_memtable_table_tests_taken_out(io, host_shared_Held_record(&held), c->first->ptr, c->first->len);
	txn_memtable_table_tests_taken_out(io, host_shared_Held_record(&held), c->last->ptr, c->last->len);
	host_shared_Held_drop(&held);
	return true;
}

/*
 * 2g's compaction, run by a writer's wait (bugs row 110), held between its
 * allocation and its second lock while another process's removals take the
 * page's last live keys, which drops the page from the range's list: the
 * compaction finds the entry changed and gives its copy back, and nothing of
 * the delta is left behind.
 */
void
test_txn_memtable_table_tests_a_compaction_whose_page_is_dropped_before_its_second_lock_gives_its_copy_back(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostDeltaPageSize size = {.bytes = 1 << 10, .words = 4};
	HostSharedHeld held = txn_memtable_testing_created(io, 9384);
	HostSharedRecord state = host_shared_Held_record(&held);
	txn_memtable_table_tests_page_dropper_t dropper_captures;
	SimcActor	compactor = {txn_memtable_table_tests_compactor};
	SimcActor	dropper = {txn_memtable_table_tests_page_dropper, &dropper_captures};
	SimcStep	steps[2] = {{"dropper", 7, PG_UINT64_MAX}, {"compactor", 9, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;
	HostError	error;
	StdVec		ranges;
	StdVec		keys;
	StdVec		taken;
	StdVec		first;
	StdVec		last;
	uint64		kept;
	uint64		range;
	uint64		name;
	uint64		dropped = TXN_MEMTABLE_PAGES_DROPPED;
	size_t		end;

	for (uint64 number = 0; number < 2000; number++)
		txn_memtable_table_tests_numbered_writes(3903, 55, io, state, number);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(3905, 49, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_on_page(io, state, 1, &keys);
	STD_ASSERT(3907, 13, keys.len > 8, "keys.len() > 8");
	std_vec_new(&taken, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	txn_memtable_table_tests_fills(3909, 90, io, size, &taken);
	end = STD_SUB(usize, keys.len, 1, 3912, 38);
	std_range_check(1, end, keys.len, RUST_FILE, 3912, 30);
	for (size_t k = 1; k < end; k++)
	{
		const txn_memtable_table_tests_read_t *key = &STD_VEC_AT(&keys, txn_memtable_table_tests_read_t, k);

		txn_memtable_table_tests_taken_out(io, state, key->key.ptr, key->key.len);
	}
	range = txn_memtable_table_tests_range_at(&ranges, 0, 3915, 14);
	std_vec_drop_Checked(&taken, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	std_index_check(0, keys.len, RUST_FILE, 3917, 30);
	std_slice_to_vec_Checked(&first, STD_VEC_AT(&keys, txn_memtable_table_tests_read_t, 0).key.ptr,
							 STD_VEC_AT(&keys, txn_memtable_table_tests_read_t, 0).key.len, 1, 1);
	end = STD_SUB(usize, keys.len, 1, 3917, 54);
	std_index_check(end, keys.len, RUST_FILE, 3917, 49);
	std_slice_to_vec_Checked(&last, STD_VEC_AT(&keys, txn_memtable_table_tests_read_t, end).key.ptr,
							 STD_VEC_AT(&keys, txn_memtable_table_tests_read_t, end).key.len, 1, 1);
	dropper_captures.first = &first;
	dropper_captures.last = &last;
	run = simc_Run_new(1);
	simc_Run_actor(&run, "compactor", 9, compactor);
	simc_Run_actor(&run, "dropper", 7, dropper);
	simc_Run_until(&run, "compactor", 9, "doltgres-memtable-compacting");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	simc_Report_drop(&report);
	name = txn_memtable_table_tests_entry(io, range, 1);
	STD_ASSERT_EQ(3929, 9, name == dropped, &name, std_u64_Debug_fmt, &dropped, std_u64_Debug_fmt);
	txn_memtable_table_tests_unlinks(3930, 38, io, 9384);
	txn_memtable_table_tests_used_is(3932, 9, io, start);
	std_vec_free_Checked(&first, 1, 1);
	std_vec_free_Checked(&last, 1, 1);
	txn_memtable_table_tests_reads_free(&keys);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_touches records a key in the running transaction's written list as a storage's rows', as touched's unwrap does at its place. */
static void
txn_memtable_table_tests_touches(uint32 line, uint32 col, HostIoIo *io, const HostStorage *at, const uint8 *key,
								 size_t key_len)
{
	HostError	error;
	uint64		touched;

	if (!txn_written_touched(io, 0, at, TXN_WRITTEN_OF_ROWS, key, key_len, 0, &touched, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/* txn_memtable_table_tests_written_by records a key in the running transaction's written list and pushes a put to it by a writer, as the rolled back subtransaction case's wrote closure does. */
static TxnMemtableVersionHandle
txn_memtable_table_tests_written_by(HostIoIo *io, HostSharedRecord state, const HostStorage *at,
									TxnMemtableVersionWriter writer, const char *key)
{
	txn_memtable_table_tests_touches(3948, 83, io, at, (const uint8 *) key, strlen(key));
	return txn_memtable_table_tests_pushes(3950, 43, io, state, key, writer);
}

/*
 * A subtransaction's write rolled back while its top-level transaction
 * commits stays after the subtransaction's abort and the top's commit, for the
 * fold's logged decision to drop, and the top-level's write stays (bugs row
 * 432).
 */
void
test_txn_memtable_table_tests_a_subtransactions_write_rolled_back_while_its_top_commits_stays_for_a_fold(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9385);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage at = txn_memtable_testing_storage(9385);
	HostXid		top = simc_begin();
	HostXid		sub;
	TxnMemtableVersionWriter top_writer = {.xid = top.f0};
	TxnMemtableVersionWriter sub_writer = {.has_top = true, .top = top.f0, .command = 1};
	TxnMemtableVersionHandle kept;
	HostError	error;
	StdVec		handles;

	kept = txn_memtable_table_tests_written_by(io, state, &at, top_writer, "a");
	if (!txn_written_sub_started(io, 2, &error))
		STD_RESULT_EXPECT_FAILED(3953, 44, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	sub = simc_begin_under(top);
	sub_writer.xid = sub.f0;
	(void) txn_memtable_table_tests_written_by(io, state, &at, sub_writer, "b");
	simc_abort(sub);
	if (!txn_written_sub_aborted(io, 2, &error))
		STD_RESULT_EXPECT_FAILED(3957, 44, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	simc_commit(top);
	if (!txn_written_ended(io, &error))
		STD_RESULT_EXPECT_FAILED(3959, 35, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_handles_of(3961, 40, 45, io, state, (const uint8 *) "b", 1, &handles);
	STD_ASSERT(3964, 9, handles.len == 1, "the rolled-back write stays");
	std_vec_free_Checked(&handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	txn_memtable_table_tests_handles_are(3965, 3961, 45, io, state, (const uint8 *) "a", 1, &kept, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * 2g's prune of a prepared transaction's versions: they stay while it is
 * prepared, whatever prunes their key, and go at the first prune after
 * `ROLLBACK PREPARED` records its abort; a committed version beside them
 * stays.
 */
void
test_txn_memtable_table_tests_a_prepared_transactions_versions_stay_until_its_rollback_then_go_at_the_next_prune(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9387);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostXid		committed = simc_begin();
	HostXid		prepared = simc_begin();
	TxnMemtableVersionHandle both[2];
	TxnMemtableVersionHandle kept;

	kept = txn_memtable_table_tests_pushes(4022, 44, io, state, "k", txn_memtable_testing_writer(committed.f0));
	simc_commit(committed);
	both[0] = txn_memtable_table_tests_pushes(4022, 44, io, state, "k", txn_memtable_testing_writer(prepared.f0));
	both[1] = kept;
	txn_memtable_table_tests_prunes(4031, 47, io, state, "k");
	txn_memtable_table_tests_handles_are(4032, 4028, 46, io, state, (const uint8 *) "k", 1, both, 2);
	simc_abort(prepared);
	txn_memtable_table_tests_prunes(4034, 47, io, state, "k");
	txn_memtable_table_tests_handles_are(4035, 4028, 46, io, state, (const uint8 *) "k", 1, &kept, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_order_t is Order's step: a budget case's actor, and the pause it runs to, or NULL to run it to its end. */
typedef struct txn_memtable_table_tests_order_t
{
	const char *actor;
	const char *until;
} txn_memtable_table_tests_order_t;

/* txn_memtable_table_tests_budget_writer is the budget case's writer: it holds two readers when reading, as a statement's and an open scan's, and writes keys 1 to 99 of the storage the case filled. */
static bool
txn_memtable_table_tests_budget_writer(void *captures, HostIoIo *io, HostError *error)
{
	const bool *reading = captures;
	HostSharedReader readers[2];
	HostSharedHeld held;
	bool		ok = true;

	if (*reading)
	{
		if (!host_shared_read(io, &readers[0], error))
			return false;
		if (!host_shared_read(io, &readers[1], error))
		{
			host_shared_Reader_drop(&readers[0]);
			return false;
		}
	}
	if (!txn_memtable_testing_stated(io, 9391, &held, error))
		ok = false;
	for (uint64 number = 1; ok && number < 100; number++)
	{
		TxnMemtableVersionHandle handle;
		StdVec		key;

		txn_memtable_testing_numbered(number, &key);
		ok = txn_memtable_table_tests_written(io, host_shared_Held_record(&held), key.ptr, key.len, &handle, error);
		std_vec_free_Checked(&key, 1, 1);
		if (!ok)
			host_shared_Held_drop(&held);
	}
	if (*reading)
	{
		host_shared_Reader_drop(&readers[0]);
		host_shared_Reader_drop(&readers[1]);
	}
	if (ok)
		host_shared_Held_drop(&held);
	return ok;
}

/* txn_memtable_table_tests_remover_t is a budget case's remover's captures: the database of the storage it empties a page of, and that page's keys. */
typedef struct txn_memtable_table_tests_remover_t
{
	uint32		database;
	const StdVec *gone;
} txn_memtable_table_tests_remover_t;

/* txn_memtable_table_tests_budget_remover is a budget case's remover: it takes out every key on a page of the other storage, which retires the page under its last removal's hold. */
static bool
txn_memtable_table_tests_budget_remover(void *captures, HostIoIo *io, HostError *error)
{
	const txn_memtable_table_tests_remover_t *c = captures;
	HostSharedHeld held;

	if (!txn_memtable_testing_stated(io, c->database, &held, error))
		return false;
	for (size_t k = 0; k < c->gone->len; k++)
	{
		const StdVec *key = &STD_VEC_AT(c->gone, StdVec, k);

		txn_memtable_table_tests_taken_out(io, host_shared_Held_record(&held), key->ptr, key->len);
	}
	host_shared_Held_drop(&held);
	return true;
}

/* txn_memtable_table_tests_budget_reader is the budget case's reader: it holds a hold across its pause. */
static bool
txn_memtable_table_tests_budget_reader(void *captures, HostIoIo *io, HostError *error)
{
	HostDeltaHold hold;
	bool		ok;

	if (!host_delta_hold(io, &hold, error))
		return false;
	ok = host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-memtable-case-reading", error);
	host_delta_Hold_drop(&hold);
	return ok;
}

/*
 * txn_memtable_table_tests_filled fills a storage's rows past a page, keeps
 * the keys on its page 3, writes one key of another storage, and takes the
 * delta's pages until it is at its budget, as the budget cases begin, their
 * unwraps at their lines.
 */
static void
txn_memtable_table_tests_filled(uint32 writes_line, uint32 write_line, uint32 fill_line, HostIoIo *io,
								uint32 removed, uint32 written, StdVec *gone, StdVec *taken)
{
	static const HostDeltaPageSize sizes[2] = {{.bytes = 48 << 10, .words = 2 << 10}, {.bytes = 6 << 10, .words = 256}};
	HostSharedHeld held = txn_memtable_testing_created(io, removed);
	StdVec		reads;

	for (uint64 number = 0; number < 2000; number++)
		txn_memtable_table_tests_numbered_writes(writes_line, 55, io, host_shared_Held_record(&held), number);
	txn_memtable_table_tests_on_page(io, host_shared_Held_record(&held), 3, &reads);
	std_vec_with_capacity_Checked(gone, reads.len, sizeof(StdVec), _Alignof(StdVec));
	for (size_t k = 0; k < reads.len; k++)
	{
		txn_memtable_table_tests_read_t *read = &STD_VEC_AT(&reads, txn_memtable_table_tests_read_t, k);

		std_vec_push_Checked(gone, &read->key, sizeof(StdVec), _Alignof(StdVec));
		for (size_t v = 0; v < read->read.len; v++)
			std_vec_free_Checked(&STD_VEC_AT(&read->read, txn_memtable_table_tests_read_version_t, v).record, 1, 1);
		std_vec_free_Checked(&read->read, sizeof(txn_memtable_table_tests_read_version_t),
							 _Alignof(txn_memtable_table_tests_read_version_t));
	}
	std_vec_free_Checked(&reads, sizeof(txn_memtable_table_tests_read_t), _Alignof(txn_memtable_table_tests_read_t));
	host_shared_Held_drop(&held);
	held = txn_memtable_testing_created(io, written);
	txn_memtable_table_tests_numbered_writes(write_line, 54, io, host_shared_Held_record(&held), 0);
	host_shared_Held_drop(&held);
	std_vec_new(taken, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	for (size_t k = 0; k < lengthof(sizes); k++)
		txn_memtable_table_tests_fills(fill_line, 61, io, sizes[k], taken);
}

/*
 * txn_memtable_table_tests_budget runs a writer refused at the budget, a
 * remover that empties a 64 KiB page of another storage, whose last removal
 * retires it under that removal's hold, and a reader that holds a hold across
 * the pause doltgres-memtable-case-reading, in an order, and asserts that
 * every actor returns: the writer's pages are small, so that page's free
 * alone lets it go on. A reading writer holds two readers across its writes,
 * as a statement's and an open scan's.
 */
static void
txn_memtable_table_tests_budget(const txn_memtable_table_tests_order_t *order, size_t order_len, bool reading)
{
	SimcTestIo	test_io;
	HostIoIo   *io;
	SimcActor	writer = {txn_memtable_table_tests_budget_writer, &reading};
	StdVec		gone;
	txn_memtable_table_tests_remover_t remover_captures = {9390, &gone};
	SimcActor	remover = {txn_memtable_table_tests_budget_remover, &remover_captures};
	SimcActor	reader = {txn_memtable_table_tests_budget_reader};
	SimcRun		run;
	SimcReport	report;
	StdVec		taken;

	simc_size_delta(UINT64CONST(8) << 20);
	test_io = simc_io();
	io = &test_io.io;
	txn_memtable_table_tests_filled(4056, 4062, 4066, io, 9390, 9391, &gone, &taken);
	run = simc_Run_new(1);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_actor(&run, "remover", 7, remover);
	simc_Run_actor(&run, "reader", 6, reader);
	for (size_t k = 0; k < order_len; k++)
	{
		SimcStep	step = {order[k].actor, strlen(order[k].actor), PG_UINT64_MAX};

		if (order[k].until != NULL)
			simc_Run_until(&run, order[k].actor, strlen(order[k].actor), order[k].until);
		else
			simc_Run_script(&run, &step, 1);
	}
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	simc_Report_drop(&report);
	std_vec_drop_Checked(&taken, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
	txn_memtable_table_tests_keys_free(&gone);
	simc_TestIo_drop(&test_io);
}

/*
 * PLAN S6 (g) and bugs row 72: with the delta at its budget and a writer
 * asleep there, the last removal of a page retires it under the removal's
 * hold, and that hold's end, seeing the waiter, runs the free pass that frees
 * it, so the writer finishes with no other process running a pass.
 */
void
test_txn_memtable_table_tests_removals_at_the_budget_free_memory_for_a_waiting_writer(void)
{
	static const txn_memtable_table_tests_order_t order[] = {{"reader"}, {"writer"}, {"remover"}, {"writer"}};

	txn_memtable_table_tests_budget(order, lengthof(order), false);
}

/*
 * Bugs row 72 (R72b): a reader holds an older epoch across the removal's
 * end, so the remover's pass frees nothing; the reader retired nothing, yet
 * its hold's end, seeing the waiter and a retirement left unfreed, runs the
 * pass that frees the page, and the writer finishes.
 */
void
test_txn_memtable_table_tests_a_readers_hold_ending_after_a_removal_frees_memory_for_a_waiting_writer(void)
{
	static const txn_memtable_table_tests_order_t order[] = {{"reader", "doltgres-memtable-case-reading"},
	{"writer"}, {"remover"}, {"reader"}, {"writer"}};

	txn_memtable_table_tests_budget(order, lengthof(order), false);
}

/*
 * Bugs row 72's lost wakeup: a reader's hold ends after the writer was
 * refused and before it counts itself among the budget's waiters, so the end
 * reads no waiter and passes nothing; the writer, once counted, runs the
 * pass itself before it sleeps, and finishes.
 */
void
test_txn_memtable_table_tests_a_hold_ending_before_a_refused_writer_counts_itself_leaves_it_the_pass(void)
{
	static const txn_memtable_table_tests_order_t order[] = {{"reader", "doltgres-memtable-case-reading"},
	{"remover"}, {"writer", "doltgres-delta-waiting"}, {"reader"}, {"writer"}};

	txn_memtable_table_tests_budget(order, lengthof(order), false);
}

/*
 * The budget wait text, draft 2: a writer that holds two readers across its
 * writes, a statement's and an open scan's, is refused at the budget with
 * nothing of its own to spill and waits; the remover's later retirement,
 * stamped after both readers began, is freed by the remover's own pass,
 * since the waiter gave up its readers' epochs, and the writer finishes.
 */
void
test_txn_memtable_table_tests_a_writer_holding_readers_waits_without_pinning_another_processs_frees(void)
{
	static const txn_memtable_table_tests_order_t order[] = {{"writer"}, {"remover"}, {"writer"}};

	txn_memtable_table_tests_budget(order, lengthof(order), true);
}

/* txn_memtable_table_tests_wake_writer is the wake case's writer: it writes four keys of the storage the case wrote one key of, numbered from its place among the writers. */
static bool
txn_memtable_table_tests_wake_writer(void *captures, HostIoIo *io, HostError *error)
{
	const uint64 *at = captures;
	HostSharedHeld held;
	bool		ok = true;

	if (!txn_memtable_testing_stated(io, 9482, &held, error))
		return false;
	for (uint64 number = 1; ok && number < 5; number++)
	{
		TxnMemtableVersionHandle handle;
		StdVec		key;

		txn_memtable_testing_numbered(STD_ADD(u64, STD_MUL(u64, 1000, STD_ADD(u64, *at, 1, 4190, 67), 4190, 58),
											  number, 4190, 58), &key);
		ok = txn_memtable_table_tests_written(io, host_shared_Held_record(&held), key.ptr, key.len, &handle, error);
		std_vec_free_Checked(&key, 1, 1);
	}
	host_shared_Held_drop(&held);
	return ok;
}

/* txn_memtable_table_tests_checker is the wake case's checker: it counts the delta's wakes before and after its pause. */
static bool
txn_memtable_table_tests_checker(void *captures, HostIoIo *io, HostError *error)
{
	StdVec	   *counted = captures;
	uint64		wakes = simc_delta_wakes();

	std_vec_push_Checked(counted, &wakes, sizeof(uint64), _Alignof(uint64));
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-memtable-case-checked", error))
		return false;
	wakes = simc_delta_wakes();
	std_vec_push_Checked(counted, &wakes, sizeof(uint64), _Alignof(uint64));
	return true;
}

/* txn_memtable_table_tests_hold_ender is the wake case's holder: it begins and ends fifty holds. */
static bool
txn_memtable_table_tests_hold_ender(void *captures, HostIoIo *io, HostError *error)
{
	for (int k = 0; k < 50; k++)
	{
		HostDeltaHold hold;

		if (!host_delta_hold(io, &hold, error))
			return false;
		host_delta_Hold_drop(&hold);
	}
	return true;
}

/*
 * Bugs row 124: five writers asleep at a full delta, and a retirement a live
 * reader keeps, which no pass can free. Fifty holds that begin after that
 * retirement and end count no wake, since none of them kept it, so no writer
 * wakes to a pass that frees nothing; the reader's end wakes them, and each
 * finishes.
 */
void
test_txn_memtable_table_tests_hold_ends_that_kept_no_retirement_wake_no_writer(void)
{
	static const char *const writers[5] = {"w1", "w2", "w3", "w4", "w5"};
	static const uint64 places[5] = {0, 1, 2, 3, 4};
	SimcTestIo	test_io;
	HostIoIo   *io;
	StdVec		gone;
	StdVec		taken;
	StdVec		counted;
	txn_memtable_table_tests_remover_t remover_captures = {9481, &gone};
	SimcActor	reader = {txn_memtable_table_tests_budget_reader};
	SimcActor	remover = {txn_memtable_table_tests_budget_remover, &remover_captures};
	SimcActor	checker = {txn_memtable_table_tests_checker, &counted};
	SimcActor	holder = {txn_memtable_table_tests_hold_ender};
	SimcStep	last[3] = {{"holder", 6, PG_UINT64_MAX}, {"checker", 7, PG_UINT64_MAX},
	{"reader", 6, PG_UINT64_MAX}};
	SimcStep	step = {"remover", 7, PG_UINT64_MAX};
	SimcRun		run;
	SimcReport	report;

	simc_size_delta(UINT64CONST(8) << 20);
	test_io = simc_io();
	io = &test_io.io;
	txn_memtable_table_tests_filled(4170, 4176, 4180, io, 9481, 9482, &gone, &taken);
	run = simc_Run_new(1);
	for (size_t k = 0; k < lengthof(writers); k++)
	{
		SimcActor	writer = {txn_memtable_table_tests_wake_writer, (void *) &places[k]};

		simc_Run_actor(&run, writers[k], strlen(writers[k]), writer);
	}
	simc_Run_actor(&run, "reader", 6, reader);
	simc_Run_actor(&run, "remover", 7, remover);
	std_vec_new(&counted, sizeof(uint64), _Alignof(uint64));
	simc_Run_actor(&run, "checker", 7, checker);
	simc_Run_actor(&run, "holder", 6, holder);
	simc_Run_until(&run, "reader", 6, "doltgres-memtable-case-reading");
	for (size_t k = 0; k < lengthof(writers); k++)
	{
		SimcStep	writer = {writers[k], strlen(writers[k]), PG_UINT64_MAX};

		simc_Run_script(&run, &writer, 1);
	}
	simc_Run_script(&run, &step, 1);
	simc_Run_until(&run, "checker", 7, "doltgres-memtable-case-checked");
	simc_Run_script(&run, last, lengthof(last));
	for (size_t k = 0; k < lengthof(writers); k++)
	{
		SimcStep	writer = {writers[k], strlen(writers[k]), PG_UINT64_MAX};

		simc_Run_script(&run, &writer, 1);
	}
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	simc_Report_drop(&report);
	std_index_check(0, counted.len, RUST_FILE, 4234, 20);
	std_index_check(1, counted.len, RUST_FILE, 4234, 32);
	STD_ASSERT_EQ_MSG(4234, 9, STD_VEC_AT(&counted, uint64, 0) == STD_VEC_AT(&counted, uint64, 1),
					  &STD_VEC_AT(&counted, uint64, 0), std_u64_Debug_fmt, &STD_VEC_AT(&counted, uint64, 1),
					  std_u64_Debug_fmt, "a hold's end that kept no retirement counted a wake as a free");
	std_vec_free_Checked(&counted, sizeof(uint64), _Alignof(uint64));
	std_vec_drop_Checked(&taken, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
	txn_memtable_table_tests_keys_free(&gone);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 69: a range's pages start at 1 KiB (bugs row 150) and double as
 * the range fills, to 64 KiB, since a prepare sizes the page it takes after
 * the range's newest, so a range filled past each doubling shows pages of 1,
 * 2, 4, 8, 16, 32, and 64 KiB in order, and 64 KiB after.
 */
void
test_txn_memtable_table_tests_a_ranges_pages_double_as_it_fills(void)
{
	static const uint64 wanted[8] = {32, 64, 128, 256, 512, 1024, 2048, 2048};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9392);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostError	error;
	StdVec		ranges;
	StdVec		words;
	StdVec		expected = {(void *) wanted, 8, 8};
	uint64		kept;
	bool		eq;

	for (uint64 number = 0; number < 2000; number++)
		txn_memtable_table_tests_numbered_writes(4249, 51, io, state, number);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(4251, 45, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_with_capacity_Checked(&words, 8, sizeof(uint64), _Alignof(uint64));
	for (uint64 at = 0; at < 8; at++)
	{
		uint64		name = txn_memtable_table_tests_entry(io, txn_memtable_table_tests_range_at(&ranges, 0, 4254, 38),
														  at);
		HostDeltaHold hold;
		HostDeltaPageRef page;
		uint64		count;

		if (!host_delta_hold(io, &hold, &error))
			STD_RESULT_EXPECT_FAILED(4255, 33, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!host_delta_Hold_page(&hold, host_delta_page_Page_named(name), &page, &error))
			STD_RESULT_EXPECT_FAILED(4255, 66, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		count = host_delta_PageRef_words(&page);
		host_delta_Hold_drop(&hold);
		std_vec_push_Checked(&words, &count, sizeof(uint64), _Alignof(uint64));
	}
	eq = words.len == 8 && memcmp(words.ptr, wanted, sizeof(wanted)) == 0;
	STD_ASSERT_EQ(4258, 9, eq, &words, chunks_trailer_VecU64_Debug_fmt, &expected, chunks_trailer_VecU64_Debug_fmt);
	std_vec_free_Checked(&words, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * 2g's dead count: a key's removal counts its version's record and its
 * node's key dead on their page, so a page whose keys are removed one by one
 * counts exactly their records' and keys' bytes dead.
 */
void
test_txn_memtable_table_tests_a_removal_counts_its_records_and_its_keys_bytes_dead(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9393);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostError	error;
	StdVec		ranges;
	StdVec		keys;
	uint64		kept;
	uint64		name;
	uint64		dead = 0;

	for (uint64 number = 0; number < 2000; number++)
		txn_memtable_table_tests_numbered_writes(4271, 51, io, state, number);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(4273, 45, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	name = txn_memtable_table_tests_entry(io, txn_memtable_table_tests_range_at(&ranges, 0, 4274, 30), 2);
	txn_memtable_table_tests_on_page(io, state, 2, &keys);
	std_range_to_check(3, keys.len, RUST_FILE, 4277, 29);
	for (size_t k = 0; k < 3; k++)
	{
		const txn_memtable_table_tests_read_t *read = &STD_VEC_AT(&keys, txn_memtable_table_tests_read_t, k);
		uint64		live;
		uint64		counted;
		uint64		filled;
		size_t		records = 0;

		txn_memtable_table_tests_taken_out(io, state, read->key.ptr, read->key.len);
		for (size_t v = 0; v < read->read.len; v++)
			records = std_add_usize(records,
									STD_VEC_AT(&read->read, txn_memtable_table_tests_read_version_t, v).record.len,
									STD_ACCUM_FILE, 206, 1);
		dead = STD_ADD(u64, dead, (uint64) STD_ADD(usize, read->key.len, records, 4279, 22), 4279, 13);
		txn_memtable_table_tests_counts(io, name, &live, &counted, &filled);
		STD_ASSERT_EQ(4280, 13, counted == dead, &counted, std_u64_Debug_fmt, &dead, std_u64_Debug_fmt);
	}
	txn_memtable_table_tests_reads_free(&keys);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_pin_writer is the pinned prepare case's writer: it writes w to its database's storage. */
static bool
txn_memtable_table_tests_pin_writer(void *captures, HostIoIo *io, HostError *error)
{
	const uint32 *database = captures;
	HostSharedHeld held;
	TxnMemtableVersionHandle handle;
	bool		ok;

	if (!txn_memtable_testing_stated(io, *database, &held, error))
		return false;
	ok = txn_memtable_table_tests_written(io, host_shared_Held_record(&held), (const uint8 *) "w", 1, &handle, error);
	host_shared_Held_drop(&held);
	return ok;
}

/* txn_memtable_table_tests_pin_remover is the pinned prepare case's remover: it takes k out of its database's storage, which empties the memtable's first range. */
static bool
txn_memtable_table_tests_pin_remover(void *captures, HostIoIo *io, HostError *error)
{
	const uint32 *database = captures;
	HostSharedHeld held;

	if (!txn_memtable_testing_stated(io, *database, &held, error))
		return false;
	txn_memtable_table_tests_taken_out(io, host_shared_Held_record(&held), (const uint8 *) "k", 1);
	host_shared_Held_drop(&held);
	return true;
}

/*
 * 2g's pins: a prepare held between its pin's store and its take of the
 * range's lock while the remover empties the memtable's first range leaves
 * the memtable open, since the remover reads the pin, and the prepare pushes
 * into it; with the remover first, it closes the memtable and the prepare
 * goes around to the storage's next one.
 */
void
test_txn_memtable_table_tests_a_prepare_pinned_while_the_remover_empties_the_first_range_pushes_once_either_way(void)
{
	static const uint32 databases[2] = {9394, 9395};
	static const bool pinneds[2] = {true, false};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;

	for (size_t d = 0; d < lengthof(databases); d++)
	{
		SimcActor	writer = {txn_memtable_table_tests_pin_writer, (void *) &databases[d]};
		SimcActor	remover = {txn_memtable_table_tests_pin_remover, (void *) &databases[d]};
		SimcStep	steps[2] = {{"remover", 7, PG_UINT64_MAX}, {"writer", 6, PG_UINT64_MAX}};
		HostSharedHeld held = txn_memtable_testing_created(io, databases[d]);
		HostSharedRecord state;
		HostDeltaHold hold;
		HostError	error;
		SimcRun		run;
		SimcReport	report;
		StdVec		lent;
		uint64		before;
		uint64		after;
		size_t		len;
		size_t		one = 1;
		bool		same;

		(void) txn_memtable_table_tests_writes(4294, 50, io, host_shared_Held_record(&held), (const uint8 *) "k", 1);
		if (!txn_memtable_table_tests_has_memtable(host_shared_Held_record(&held), &before))
			std_option_unwrap_failed(RUST_FILE, 4295, 49);
		host_shared_Held_drop(&held);
		run = simc_Run_new(1);
		simc_Run_actor(&run, "writer", 6, writer);
		simc_Run_actor(&run, "remover", 7, remover);
		if (pinneds[d])
			simc_Run_until(&run, "writer", 6, "doltgres-memtable-found");
		simc_Run_script(&run, steps, lengthof(steps));
		report = simc_Run_finish(run);
		txn_memtable_table_tests_returned(&report);
		simc_Report_drop(&report);
		if (!txn_memtable_testing_stated(io, databases[d], &held, &error))
			STD_RESULT_EXPECT_FAILED(4312, 45, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		state = host_shared_Held_record(&held);
		if (!txn_memtable_table_tests_has_memtable(state, &after))
			std_option_unwrap_failed(RUST_FILE, 4314, 48);
		same = after == before;
		STD_ASSERT_EQ(4314, 13, same == pinneds[d], &same, std_bool_Debug_fmt, &pinneds[d], std_bool_Debug_fmt);
		if (!host_delta_hold(io, &hold, &error))
			STD_RESULT_EXPECT_FAILED(4315, 40, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_table_tests_lent(4316, 57, io, &hold, state, (const uint8 *) "w", 1, &lent);
		len = lent.len;
		txn_memtable_table_tests_lent_free(&lent);
		STD_ASSERT_EQ(4316, 13, len == 1, &len, std_usize_Debug_fmt, &one, std_usize_Debug_fmt);
		txn_memtable_table_tests_lent(4317, 54, io, &hold, state, (const uint8 *) "k", 1, &lent);
		len = lent.len;
		txn_memtable_table_tests_lent_free(&lent);
		STD_ASSERT(4317, 13, len == 0, "versions(io, &hold, state, b\"k\").unwrap().is_empty()");
		host_delta_Hold_drop(&hold);
		host_shared_Held_drop(&held);
	}
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_outnumbered is the split and removal case's check: it fails when the ranges no free pass has freed outnumber the pages no free pass has freed. */
static bool
txn_memtable_table_tests_outnumbered(StdString *message)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" ranges outnumber "), STD_FMT_ARG(1),
	STD_FMT_LIT(" pages")};
	size_t		ranges;
	size_t		pages;

	simc_unfreed(TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape.id, &ranges, &pages);
	if (ranges > pages)
	{
		StdFmtArg	args[2] = {{&ranges, std_usize_Display_fmt}, {&pages, std_usize_Display_fmt}};
		StdFmtArguments text = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(message, &text);
		return true;
	}
	return false;
}

/* txn_memtable_table_tests_splitting_writer is the split and removal case's writer: it writes the thousand keys from the split's count on. */
static bool
txn_memtable_table_tests_splitting_writer(void *captures, HostIoIo *io, HostError *error)
{
	HostSharedHeld held;
	bool		ok = true;

	if (!txn_memtable_testing_stated(io, 9396, &held, error))
		return false;
	for (uint64 number = TXN_MEMTABLE_SPLIT_SPLIT; ok && number < TXN_MEMTABLE_SPLIT_SPLIT + 1000; number++)
	{
		TxnMemtableVersionHandle handle;
		StdVec		key;

		txn_memtable_testing_numbered(number, &key);
		ok = txn_memtable_table_tests_written(io, host_shared_Held_record(&held), key.ptr, key.len, &handle, error);
		std_vec_free_Checked(&key, 1, 1);
	}
	host_shared_Held_drop(&held);
	return ok;
}

/* txn_memtable_table_tests_emptying_remover is the split and removal case's remover: it takes out the first thousand keys. */
static bool
txn_memtable_table_tests_emptying_remover(void *captures, HostIoIo *io, HostError *error)
{
	HostSharedHeld held;

	if (!txn_memtable_testing_stated(io, 9396, &held, error))
		return false;
	for (uint64 number = 0; number < 1000; number++)
	{
		StdVec		key;

		txn_memtable_testing_numbered(number, &key);
		txn_memtable_table_tests_taken_out(io, host_shared_Held_record(&held), key.ptr, key.len);
		std_vec_free_Checked(&key, 1, 1);
	}
	host_shared_Held_drop(&held);
	return true;
}

/*
 * PLAN S6 (d): in sim, through a seeded mix of a writer's pushes that split
 * ranges and a remover's removals that empty pages and ranges, the ranges no
 * free pass has freed never outnumber the pages no free pass has freed,
 * checked at every yield point, since each range names a page of its own for
 * as long as it isn't freed. No fold runs in sim, and only a fold frees live
 * versions, so the delta is sized for the run's live versions with room to
 * spare.
 */
void
test_txn_memtable_table_tests_ranges_never_outnumber_pages_through_splits_and_removals(void)
{
	SimcTestIo	test_io;
	HostIoIo   *io;
	HostSharedHeld held;
	SimcActor	writer = {txn_memtable_table_tests_splitting_writer};
	SimcActor	remover = {txn_memtable_table_tests_emptying_remover};
	SimcRun		run;
	SimcReport	report;

	simc_size_delta(UINT64CONST(16) << 20);
	test_io = simc_io();
	io = &test_io.io;
	held = txn_memtable_testing_created(io, 9396);
	txn_memtable_table_tests_prefills(4334, 42, io, host_shared_Held_record(&held));
	host_shared_Held_drop(&held);
	run = simc_Run_new(1);
	simc_Run_check(&run, txn_memtable_table_tests_outnumbered);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_actor(&run, "remover", 7, remover);
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_swept loads a range's count of removals since its last look at every page, its record's unwrap at its place. */
static uint64
txn_memtable_table_tests_swept(uint32 line, uint32 hold_col, uint32 col, HostIoIo *io, uint64 range)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostDeltaHold hold;
	HostSharedRecord record;
	HostError	error;
	uint64		swept;

	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(line, hold_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, range, &record, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	swept = host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_SWEPT, load);
	host_delta_Hold_drop(&hold);
	return swept;
}

/*
 * Bugs row 110: a removal looks at the pages it took versions from, dropping
 * a page it emptied at once, and looks at every page of its range only once
 * the range's removals since its last such look reach its page count, so
 * removing a range's versions one key at a time looks at every page only once
 * per page count of versions; once every key but one is removed every page
 * but the first, the newest, and that key's is dropped, as a look at every
 * page on each removal leaves it.
 */
void
test_txn_memtable_table_tests_removals_look_at_the_pages_they_touch_and_at_every_page_once_per_page_count(void)
{
	static const char emptied[] = "a page its removal emptied stayed listed";
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9471);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostDeltaHold hold;
	HostError	error;
	StdVec		ranges;
	StdVec		listed;
	StdVec		keys;
	StdVec		lent;
	const StdVec *last;
	uint64		listed_kept;
	uint64		range;
	uint64		count;
	uint64		removed = 0;
	uint64		walks = 0;
	size_t		left;
	size_t		end;
	bool		all = true;

	for (uint64 number = 0; number < 2000; number++)
		txn_memtable_table_tests_numbered_writes(4380, 51, io, state, number);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &listed_kept, &error))
		STD_RESULT_EXPECT_FAILED(4382, 45, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	range = txn_memtable_table_tests_range_at(&ranges, 0, 4383, 21);
	txn_memtable_table_tests_listed_pages(io, range, &listed);
	count = (uint64) listed.len;
	std_vec_free_Checked(&listed, sizeof(uint64), _Alignof(uint64));
	txn_memtable_table_tests_walk(io, state, false, NULL, 0, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &keys);
	if (keys.len == 0)
		std_option_unwrap_failed(RUST_FILE, 4387, 46);
	last = &STD_VEC_AT(&keys, StdVec, keys.len - 1);
	for (size_t k = 0; k + 1 < keys.len; k++)
	{
		const StdVec *key = &STD_VEC_AT(&keys, StdVec, k);
		uint64		entry;
		uint64		versions;
		uint64		before;
		uint64		after;
		uint64		name;

		if (!host_delta_hold(io, &hold, &error))
			STD_RESULT_EXPECT_FAILED(4390, 44, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_table_tests_lent(4391, 60, io, &hold, state, key->ptr, key->len, &lent);
		std_index_check(0, lent.len, RUST_FILE, 4392, 18);
		entry = txn_memtable_version_Handle_page(STD_VEC_AT(&lent, TxnMemtableVersionVersion, 0).handle);
		versions = (uint64) lent.len;
		txn_memtable_table_tests_lent_free(&lent);
		host_delta_Hold_drop(&hold);
		before = txn_memtable_table_tests_swept(4394, 42, 74, io, range);
		txn_memtable_table_tests_taken_out(io, state, key->ptr, key->len);
		removed = STD_ADD(u64, removed, versions, 4396, 13);
		after = txn_memtable_table_tests_swept(4397, 35, 67, io, range);
		if (after == 0)
			walks = STD_ADD(u64, walks, 1, 4398, 22);
		else
		{
			uint64		wanted = STD_ADD(u64, before, versions, 4399, 44);

			STD_ASSERT_EQ(4399, 26, after == wanted, &after, std_u64_Debug_fmt, &wanted, std_u64_Debug_fmt);
		}
		txn_memtable_table_tests_listed_pages(io, range, &listed);
		std_index_check((size_t) entry, listed.len, RUST_FILE, 4402, 24);
		name = STD_VEC_AT(&listed, uint64, entry);
		if (entry != 0 && STD_ADD(u64, entry, 1, 4403, 30) < count && name != TXN_MEMTABLE_PAGES_DROPPED)
		{
			uint64		live;
			uint64		dead;
			uint64		filled;

			txn_memtable_table_tests_counts(io, name, &live, &dead, &filled);
			if (live == 0)
				std_panic(RUST_FILE, 4404, 17, emptied, sizeof(emptied) - 1);
		}
		std_vec_free_Checked(&listed, sizeof(uint64), _Alignof(uint64));
	}
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(4408, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_table_tests_lent(4409, 46, io, &hold, state, last->ptr, last->len, &lent);
	std_index_check(0, lent.len, RUST_FILE, 4409, 13);
	left = (size_t) txn_memtable_version_Handle_page(STD_VEC_AT(&lent, TxnMemtableVersionVersion, 0).handle);
	txn_memtable_table_tests_lent_free(&lent);
	host_delta_Hold_drop(&hold);
	txn_memtable_table_tests_listed_pages(io, range, &listed);
	end = STD_SUB(usize, listed.len, 1, 4412, 21);
	for (size_t entry = 1; all && entry < end; entry++)
	{
		if (entry == left)
			continue;
		std_index_check(entry, listed.len, RUST_FILE, 4412, 82);
		all = STD_VEC_AT(&listed, uint64, entry) == TXN_MEMTABLE_PAGES_DROPPED;
	}
	STD_ASSERT(4412, 9, all,
			   "(1..listed.len() - 1).filter(|&entry| entry != left).all(|entry| listed[entry] == pages::DROPPED)");
	STD_ASSERT(4413, 9, walks >= 1 && walks <= STD_DIV(u64, removed, count, 4413, 40),
			   "walks >= 1 && walks <= removed / count");
	std_vec_free_Checked(&listed, sizeof(uint64), _Alignof(uint64));
	txn_memtable_table_tests_keys_free(&keys);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * Bugs row 110's (3) (orev-18 S4): a removal that retires no page runs no
 * free pass, since a pass walks the whole page directory; removing every
 * other key of a range, which empties no page, runs none.
 */
void
test_txn_memtable_table_tests_a_removal_that_retires_no_page_runs_no_free_pass(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9472);
	HostSharedRecord state = host_shared_Held_record(&held);
	StdVec		keys;
	uint64		before;
	uint64		after;

	for (uint64 number = 0; number < 2000; number++)
		txn_memtable_table_tests_numbered_writes(4426, 51, io, state, number);
	txn_memtable_table_tests_walk(io, state, false, NULL, 0, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &keys);
	before = simc_delta_passes();
	for (size_t k = 0, taken = 0; k < keys.len && taken < 200; k += 2, taken++)
	{
		const StdVec *key = &STD_VEC_AT(&keys, StdVec, k);

		txn_memtable_table_tests_taken_out(io, state, key->ptr, key->len);
	}
	after = simc_delta_passes();
	STD_ASSERT_EQ(4433, 9, after == before, &after, std_u64_Debug_fmt, &before, std_u64_Debug_fmt);
	txn_memtable_table_tests_keys_free(&keys);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * test_txn_memtable_table_tests_a_prepare_with_no_tuple_slot_leaves_its_unstored_slot_as_it_was:
 * a prepare given no first row's tuple id slot answers none for the slot it
 * left unstored and leaves its caller's slot as the caller had it (bugs row
 * 288).
 */
void
test_txn_memtable_table_tests_a_prepare_with_no_tuple_slot_leaves_its_unstored_slot_as_it_was(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9651);
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableVersionHandle handle;
	txn_memtable_table_alone_t alone = {.change = &change, .result = &handle};
	TxnMemtableTableAim aims[1] = {{.owner = host_shared_Held_record(&held), .tree = &TXN_MEMTABLE_TABLE_ROWS_TREE_OPS,
	.key = {(const uint8 *) "k", 1}, .need = txn_memtable_version_encoded(&change.writer, &op, change.shown, false, 0)}};
	TxnMemtableTableReservation tuple = {UINT64CONST(0xA11)};
	TxnMemtableTableReservation unstored = {UINT64CONST(0x5EED)};
	bool		has_unstored = true;
	HostError	error;

	if (!txn_memtable_table_prepared(io, aims, 1, false, tuple, txn_memtable_table_alone_inside, &alone, NULL,
									 &has_unstored, &unstored, NULL, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 5, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 5, !has_unstored && unstored.place == UINT64CONST(0x5EED),
			   "a prepare with no tuple id slot answers none and leaves its unstored slot as it was");
	txn_memtable_Op_drop(&op);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * A refused try takes nothing: with one free block left, a prepare whose
 * tree has no memtable and whose push needs a whole block for its version is
 * refused in one allocation, the memtable's first page with its reserve, and
 * leaves no memtable, range record, or page behind for a walk to close and
 * free.
 */
void
test_txn_memtable_table_tests_a_refused_try_leaves_no_memtable_range_or_page_behind(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	HostSharedRecord state;
	HostDeltaPageSize block = {.bytes = (64 << 10) - 8, .words = 1};
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableVersionHandle handle;
	txn_memtable_table_alone_t alone = {.change = &change, .result = &handle};
	TxnMemtableTableAim aims[1];
	TxnMemtableTableReservation none = {0};
	TxnMemtableTableReservation unstored = {0};
	TxnMemtableTableRefusal refusal = {0};
	StdVec		filler;
	HostError	error;
	size_t		ranges_before;
	size_t		pages_before;
	size_t		ranges_after;
	size_t		pages_after;
	uint64		location;
	bool		has_unstored = false;

	simc_size_delta(UINT64CONST(4) << 16);
	held = txn_memtable_testing_created(io, 9791);
	state = host_shared_Held_record(&held);
	aims[0] = (TxnMemtableTableAim)
	{
		.owner = state, .tree = &TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, .key =
		{
			(const uint8 *) "k", 1
		}, .need = 40000
	};
	std_vec_new(&filler, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	for (;;)
	{
		StdVec		pages;
		HostDeltaFull full;
		bool		allocated;

		if (!host_delta_alloc(io, &block, 1, &allocated, &pages, &full, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!allocated)
			break;
		std_vec_push_Checked(&filler, &STD_VEC_AT(&pages, HostDeltaOwned, 0), sizeof(HostDeltaOwned),
							 _Alignof(HostDeltaOwned));
		std_vec_free_Checked(&pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	}
	STD_ASSERT(__LINE__, 9, filler.len > 0, "!filler.is_empty()");
	filler.len--;
	host_delta_Owned_drop(&STD_VEC_AT(&filler, HostDeltaOwned, filler.len));
	simc_unfreed(TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape.id, &ranges_before, &pages_before);
	if (!txn_memtable_table_prepared(io, aims, 1, false, none, txn_memtable_table_alone_inside, &alone, NULL,
									 &has_unstored, &unstored, &refusal, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, refusal.refused, "refusal.refused");
	STD_ASSERT(__LINE__, 9, !txn_memtable_table_memtable(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, state, &location),
			   "the refused try made no memtable");
	simc_unfreed(TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape.id, &ranges_after, &pages_after);
	STD_ASSERT_EQ_U64(__LINE__, 9, ranges_after, ranges_before);
	STD_ASSERT_EQ_U64(__LINE__, 9, pages_after, pages_before);
	std_vec_clear(&filler, sizeof(HostDeltaOwned), host_delta_Owned_drop);
	std_vec_free_Checked(&filler, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	txn_memtable_Op_drop(&op);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}


/*
 * The refused try of two trees (bugs row 291's review F2): a prepare whose
 * two trees, the rows and the free space map, both lack a memtable takes
 * the delta's pages in one allocation and then every memtable's range and
 * memtable records before it makes either, so a take that fails, the
 * allocation or any of the four records, the second tree's included, leaves
 * neither memtable and no range record or page behind (bugs row 321).
 */
void
test_txn_memtable_table_tests_a_refused_try_of_two_trees_leaves_neither_memtable_behind(void)
{
	for (uint64 after = 0; after < 5; after++)
	{
		SimcTestIo	test_io = simc_io();
		HostIoIo   *io = &test_io.io;
		HostSharedHeld held = txn_memtable_testing_created(io, 9792);
		HostSharedRecord state = host_shared_Held_record(&held);
		TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
		TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7), .op = &op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
		TxnMemtableVersionHandle handle;
		txn_memtable_table_alone_t alone = {.change = &change, .result = &handle};
		static const uint8 block[4] = {0};
		TxnMemtableTableAim aims[2] = {
			{.owner = state, .tree = &TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, .key = {(const uint8 *) "k", 1}, .need = 64},
		{.owner = state, .tree = &TXN_MEMTABLE_MAP_FREE_TREE_OPS, .key = {block, sizeof(block)}, .need = 64}};
		TxnMemtableTableReservation none = {0};
		TxnMemtableTableReservation unstored = {0};
		TxnMemtableTableRefusal refusal = {0};
		HostError	error;
		size_t		ranges_before;
		size_t		pages_before;
		size_t		ranges_after;
		size_t		pages_after;
		uint64		location;
		bool		has_unstored = false;
		bool		prepared;


		simc_unfreed(TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape.id, &ranges_before, &pages_before);
		simc_refuse_any_take(after);
		prepared = txn_memtable_table_prepared(io, aims, 2, false, none, txn_memtable_table_alone_inside, &alone, NULL,
											   &has_unstored, &unstored, &refusal, &error);
		simc_refuse_nothing();
		if (prepared)
			STD_ASSERT(__LINE__, 9, refusal.refused, "a try refused at a take is refused, or fails");
		else
			host_Error_drop(&error);
		STD_ASSERT(__LINE__, 9, !txn_memtable_table_memtable(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, state, &location),
				   "the refused try made no rows memtable");
		STD_ASSERT(__LINE__, 9, !txn_memtable_table_memtable(&TXN_MEMTABLE_MAP_FREE_TREE_OPS, state, &location),
				   "the refused try made no free space map memtable");
		simc_unfreed(TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape.id, &ranges_after, &pages_after);
		STD_ASSERT_EQ_U64(__LINE__, 9, ranges_after, ranges_before);
		STD_ASSERT_EQ_U64(__LINE__, 9, pages_after, pages_before);
		txn_memtable_Op_drop(&op);
		host_shared_Held_drop(&held);
		simc_TestIo_drop(&test_io);
	}
}

/* txn_memtable_table_tests_mover_t is the moved page case's mover's captures: the range and the page a packing moves, and whether it moved it. */
typedef struct txn_memtable_table_tests_mover_t
{
	uint64		range;
	uint64		name;
	bool		moved;
} txn_memtable_table_tests_mover_t;

/* txn_memtable_table_tests_mover is the moved page case's mover: a packing's move of the range's only page, under a hold of its own. */
static bool
txn_memtable_table_tests_mover(void *captures, HostIoIo *io, HostError *error)
{
	txn_memtable_table_tests_mover_t *c = captures;
	HostDeltaDirectoryPacked packed = {c->name, TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape.id, c->range,
	txn_memtable_table_first_size()};
	HostDeltaHold hold;
	bool		ok;

	if (!host_delta_hold(io, &hold, error))
		return false;
	ok = txn_memtable_table_moved(io, &hold, &packed, &c->moved, error);
	host_delta_Hold_drop(&hold);
	return ok;
}

/*
 * txn_memtable_table_tests_mid_move_t is the moved page case's reader's
 * captures: the range and its old page, the old page's filled bytes before
 * the move, and what the reader saw while the move was paused.
 */
typedef struct txn_memtable_table_tests_mid_move_t
{
	uint64		range;
	uint64		old;
	uint64		filled;
	bool		linked;
	bool		kept;
} txn_memtable_table_tests_mid_move_t;

/*
 * txn_memtable_table_tests_mid_move is the moved page case's reader, run
 * while the move is paused between the copy's link and the old page's
 * retirement: it sees the range's list name the copy, reads the old page as
 * it was, and writes a second key, which lands on the copy.
 */
static bool
txn_memtable_table_tests_mid_move(void *captures, HostIoIo *io, HostError *error)
{
	txn_memtable_table_tests_mid_move_t *c = captures;
	HostSharedHeld held;
	HostDeltaHold hold;
	HostDeltaPageRef page;

	c->linked = txn_memtable_table_tests_entry(io, c->range, 0) != c->old;
	if (!host_delta_hold(io, &hold, error))
		return false;
	if (!host_delta_Hold_page(&hold, host_delta_page_Page_named(c->old), &page, error))
	{
		host_delta_Hold_drop(&hold);
		return false;
	}
	c->kept = host_delta_PageRef_filled(&page) == c->filled;
	host_delta_Hold_drop(&hold);
	if (!txn_memtable_testing_stated(io, 9443, &held, error))
		return false;
	txn_memtable_table_tests_numbered_writes(__LINE__, 9, io, host_shared_Held_record(&held), 1);
	host_shared_Held_drop(&held);
	return true;
}

/*
 * A packing's move of a range's only page, its first and its newest (bugs
 * row 143), paused between the copy's link and the old page's retirement:
 * another process then sees the range's list name the copy, reads the old
 * page as it was, and writes a key that lands on the copy; this process's
 * hold, begun before the move, keeps the retired old page unfreed until it
 * ends; and every key reads back.
 */
void
test_txn_memtable_table_tests_a_moved_only_page_stays_for_an_older_hold_and_takes_later_writes(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	HostSharedHeld held = txn_memtable_testing_created(io, 9443);
	HostSharedRecord state = host_shared_Held_record(&held);
	txn_memtable_table_tests_mover_t mover_captures = {0};
	txn_memtable_table_tests_mid_move_t reader_captures = {0};
	SimcActor	mover = {txn_memtable_table_tests_mover, &mover_captures};
	SimcActor	reader = {txn_memtable_table_tests_mid_move, &reader_captures};
	SimcStep	steps[1] = {{"reader", 6, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;
	HostError	error;
	StdVec		ranges;
	StdVec		keys;
	uint64		kept;
	uint64		range;
	uint64		old;
	size_t		ranges_held;
	size_t		pages_held;
	size_t		ranges_after;
	size_t		pages_after;
	HostDeltaHold hold;
	HostDeltaPageRef page;

	txn_memtable_table_tests_numbered_writes(__LINE__, 9, io, state, 0);
	if (!txn_memtable_table_tests_listed(io, state, &ranges, &kept, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	range = txn_memtable_table_tests_range_at(&ranges, 0, __LINE__, 9);
	std_vec_free_Checked(&ranges, sizeof(uint64), _Alignof(uint64));
	old = txn_memtable_table_tests_entry(io, range, 0);
	if (!host_delta_hold(io, &hold, &error) ||
		!host_delta_Hold_page(&hold, host_delta_page_Page_named(old), &page, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	mover_captures.range = reader_captures.range = range;
	mover_captures.name = reader_captures.old = old;
	reader_captures.filled = host_delta_PageRef_filled(&page);
	run = simc_Run_new(1);
	simc_Run_actor(&run, "mover", 5, mover);
	simc_Run_actor(&run, "reader", 6, reader);
	simc_Run_until(&run, "mover", 5, "doltgres-memtable-moved");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_table_tests_returned(&report);
	simc_Report_drop(&report);
	STD_ASSERT(__LINE__, 9, mover_captures.moved, "the packing's move moves the range's only page");
	STD_ASSERT(__LINE__, 9, reader_captures.linked, "the move links its copy over the range's only page");
	STD_ASSERT(__LINE__, 9, reader_captures.kept, "the old page reads as it was while the move is paused");
	txn_memtable_table_tests_passed(io);
	if (!host_delta_Hold_page(&hold, host_delta_page_Page_named(old), &page, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	simc_unfreed(TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape.id, &ranges_held, &pages_held);
	host_delta_Hold_drop(&hold);
	txn_memtable_table_tests_passed(io);
	simc_unfreed(TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape.id, &ranges_after, &pages_after);
	STD_ASSERT(__LINE__, 9, pages_after < pages_held, "the old page frees once the older hold ends");
	txn_memtable_table_tests_on_page(io, state, 0, &keys);
	STD_ASSERT_EQ_U64(__LINE__, 9, (uint64) keys.len, 2);
	txn_memtable_table_tests_reads_free(&keys);
	host_shared_Held_drop(&held);
	txn_memtable_table_tests_unlinks(__LINE__, 9, io, 9443);
	txn_memtable_table_tests_used_is(__LINE__, 9, io, start);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_table_tests_unread is a step's reading that a tree with no memtable never calls. */
static bool
txn_memtable_table_tests_unread(void *captures, const HostDeltaHold *hold, const HostSharedGuard *guard,
								bool has_stop, const uint8 *stop, size_t stop_len, HostError *error)
{
	(void) captures;
	(void) hold;
	(void) guard;
	(void) has_stop;
	(void) stop;
	(void) stop_len;
	(void) error;
	STD_ASSERT(__LINE__, 5, false, "a step over no memtable reads no range");
	return false;
}

/*
 * A step over a tree with no memtable answers no range and no stop, whatever
 * its caller's places held before (bugs row 472).
 */
void
test_txn_memtable_table_tests_a_step_with_no_memtable_answers_no_stop(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 4720);
	HostDeltaHold hold;
	HostError	error;
	StdVec		stop;
	bool		some = true;
	bool		has_stop = true;

	std_vec_new(&stop, 1, 1);
	if (!host_delta_hold(io, &hold, &error) ||
		!txn_memtable_table_stepped(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, &hold, host_shared_Held_record(&held), false,
									NULL, 0, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, txn_memtable_table_tests_unread, NULL,
									&some, &has_stop, &stop, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 5, !some, "a step over no memtable answers no range");
	STD_ASSERT(__LINE__, 5, !has_stop, "a step over no memtable answers no stop");
	host_delta_Hold_drop(&hold);
	std_vec_free_Checked(&stop, 1, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_TXN */
