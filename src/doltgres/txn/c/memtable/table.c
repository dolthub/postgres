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
 * push saw of the range for the settle.
 */
bool
txn_memtable_table_Reserve_push(TxnMemtableTableReserve *self, TxnMemtableIndexKeyedOpen *open,
								const HostDeltaHold *hold,
								const uint8 *key, size_t key_len, const TxnMemtableVersionChange *change,
								TxnMemtableVersionHandle *result, HostError *error)
{
	TxnMemtableVersionPushed pushed;
	HostSharedRecord record = host_shared_Guard_record(open->open.guard);
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostDeltaListListView pages;

	if (txn_memtable_version_encoded(&change->writer, change->op, change->shown, change->has_lsn, change->lsn) >
		self->buffer.cap)
	{
		*error = txn_memtable_table_internal("a prepared version is longer than its prepare's record");
		return false;
	}
	if (!txn_memtable_version_push(open, hold, key, key_len, change,
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
 * txn_memtable_table_reached runs a step under the exclusive lock of the
 * open range that now holds a key, from the range a hand-off found: a closed
 * range's replacement words name its halves, stored before closed and read
 * with no lock, of which the key's is picked by the upper's first key, and a
 * closed range with none sends it to a search of the memtable's list; each
 * step takes one lock at a time, opens only what the hold reaches, and
 * allocates nothing.
 */
static bool
txn_memtable_table_reached(HostIoTakesShort io, const HostDeltaHold *hold, TxnMemtableTableAt *at, const uint8 *key,
						   size_t key_len, bool (*step) (void *captures, TxnMemtableIndexKeyedOpen *open,
														 HostError *error),
						   void *captures, HostError *error)
{
	for (;;)
	{
		HostSharedRecord record;
		HostSharedGuard guard;
		TxnMemtableIndexKeyedOpen open;
		HostSharedPast lower;
		HostSharedPast upper;
		uint64		lower_at;
		uint64		upper_at;
		bool		has_lower;
		bool		has_upper;

		if (!host_delta_Hold_record(hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, at->range, &record, error) ||
			!host_shared_Record_lock(record, io, TXN_MEMTABLE_RANGE_RANGE_LOCK, &guard, error))
			return false;
		if (txn_memtable_index_keyed_open(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &guard, &open))
		{
			bool		ok = step(captures, &open, error);

			host_shared_Guard_drop(&guard);
			return ok;
		}
		lower = host_shared_Record_names_past(record, TXN_MEMTABLE_RANGE_RANGE_LOWER);
		upper = host_shared_Record_names_past(record, TXN_MEMTABLE_RANGE_RANGE_UPPER);
		has_lower = host_shared_Past_load(&lower, &lower_at);
		has_upper = host_shared_Past_load(&upper, &upper_at);
		host_shared_Guard_drop(&guard);
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

/* txn_memtable_table_prepared_frame_0_t is prepared's state across its pad: the sizes, the hold, the hand-offs, the pages, the targets, and inside's closure, while live. */
typedef struct txn_memtable_table_prepared_frame_0_t
{
	HostIoIo   *io;
	const TxnMemtableTableAim *aims;
	size_t		aims_len;
	bool		(*inside) (void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared, HostError *error);
	void	   *inside_captures;
	void		(*inside_drop) (void *captures);
	bool	   *has_unstored;
	TxnMemtableTableReservation *unstored;
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
} txn_memtable_table_prepared_frame_0_t;

/* txn_memtable_table_prepared_unpinned clears the process's pin of each aim's tree. */
static void
txn_memtable_table_prepared_unpinned(txn_memtable_table_prepared_frame_0_t *frame)
{
	for (size_t k = 0; k < frame->aims_len; k++)
		txn_memtable_pins_unpin(&frame->mine, frame->aims[k].tree->pin);
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

/* txn_memtable_table_prepared_pad_0 is prepared's pad: the targets, the pages, the hold, the hand-offs, the sizes, and inside's closure, while live. */
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
	if (frame->inside_live)
	{
		frame->inside_live = false;
		if (frame->inside_drop != NULL)
			frame->inside_drop(frame->inside_captures);
	}
}

/*
 * txn_memtable_table_prepared_try is one try of prepared's: it makes each
 * memtable a tree lacks, begins the hold, hands each push off, and takes
 * every push's reserve, answering false on an error and setting ready once
 * the frame holds the targets.
 */
static bool
txn_memtable_table_prepared_try(txn_memtable_table_prepared_frame_0_t *frame, bool *ready)
{
	size_t		each = 1 + HOST_DELTA_LIST_DEEPEST;
	bool		allocated;
	HostDeltaFull full;

	*ready = false;
	for (size_t k = 0; k < frame->aims_len; k++)
		if (!txn_memtable_table_first(frame->aims[k].tree, frame->io, frame->aims[k].owner, frame->error))
			return false;
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
		bool		some;
		TxnMemtableTableAt at;

		if (!txn_memtable_table_handed(aim->tree, frame->io, &frame->prepared.hold, &frame->mine, aim->owner,
									   aim->key.ptr, aim->key.len, &some, &at, frame->error))
			return false;
		if (!some)
		{
			txn_memtable_table_prepared_unpinned(frame);
			return true;
		}
		std_vec_push_Checked(&frame->ats, &at, sizeof(at), _Alignof(TxnMemtableTableAt));
	}
	for (size_t k = 0; k < frame->aims_len; k++)
	{
		HostSharedRecord range;
		HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

		if (!host_delta_Hold_record(&frame->prepared.hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS,
									STD_VEC_AT(&frame->ats, TxnMemtableTableAt, k).range, &range, frame->error))
			return false;
		STD_VEC_AT(&frame->sizes, HostDeltaPageSize, std_wrapping_mul_usize(k, each)) =
			txn_memtable_version_reserve(host_shared_Record_atomic(range, TXN_MEMTABLE_RANGE_RANGE_NEWEST, load),
										 frame->aims[k].key.len, frame->aims[k].need);
	}
	if (!host_delta_alloc(frame->io, frame->sizes.ptr, frame->sizes.len, &allocated, &frame->pages, &full,
						  frame->error))
		return false;
	if (!allocated)
	{
		txn_memtable_table_prepared_unpinned(frame);
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->prepared.hold);
		return txn_memtable_table_waited(frame->io, full, false, frame->error);
	}
	frame->pages_live = true;
	frame->pages_at = 0;
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

/* txn_memtable_table_prepared_body_0 is prepared's body. */
static void
txn_memtable_table_prepared_body_0(void *frame_)
{
	txn_memtable_table_prepared_frame_0_t *frame = frame_;
	size_t		each = 1 + HOST_DELTA_LIST_DEEPEST;
	bool		ready = false;
	bool		ok;
	HostDeltaPageSize list_page = HOST_DELTA_LIST_PAGE;

	if (!host_shared_mine(HOST_IO_IO_TAKES_SHORT(frame->io), &TXN_MEMTABLE_PINS_PINS_PROC_SLOTS_OPS, &frame->mine,
						  frame->error))
	{
		txn_memtable_table_prepared_pad_0(frame);
		return;
	}
	std_vec_new(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	frame->sizes_live = true;
	if (!std_vec_try_reserve(&frame->sizes, STD_MUL(usize, frame->aims_len, each, 454, 23),
							 sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize)))
	{
		*frame->error = host_Error_out_of_memory();
		txn_memtable_table_prepared_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->aims_len; k++)
	{
		HostDeltaPageSize size = txn_memtable_version_reserve(0, frame->aims[k].key.len, frame->aims[k].need);

		std_vec_push_Checked(&frame->sizes, &size, sizeof(size), _Alignof(HostDeltaPageSize));
		for (size_t d = 0; d < HOST_DELTA_LIST_DEEPEST; d++)
			std_vec_push_Checked(&frame->sizes, &list_page, sizeof(list_page), _Alignof(HostDeltaPageSize));
	}
	while (!ready)
	{
		if (!txn_memtable_table_prepared_try(frame, &ready))
		{
			txn_memtable_table_prepared_pad_0(frame);
			return;
		}
		if (!ready)
		{
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
		}
	}
	frame->prepared.aims = frame->aims;
	frame->prepared.aims_len = frame->aims_len;
	ok = host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-memtable-prepared", frame->error);
	frame->inside_live = false;
	if (ok)
		ok = frame->inside(frame->inside_captures, frame->io, &frame->prepared, frame->error);
	else if (frame->inside_drop != NULL)
		frame->inside_drop(frame->inside_captures);
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
	{
		txn_memtable_table_prepared_pad_0(frame);
		return;
	}
	for (size_t k = 0, k_len = frame->prepared.targets.len; k < k_len; k++)
	{
		const TxnMemtableTableTarget *target = &STD_VEC_AT(&frame->prepared.targets, TxnMemtableTableTarget, k);

		if (!txn_memtable_table_settled(target->tree, frame->io, frame->aims[k].owner, target->at,
										target->reserve.has_seen, &target->reserve.seen, frame->error))
		{
			txn_memtable_table_prepared_pad_0(frame);
			return;
		}
	}
	*frame->has_unstored = frame->prepared.has_tuple;
	*frame->unstored = frame->prepared.tuple;
	frame->ok = true;
	txn_memtable_table_prepared_pad_0(frame);
}

/*
 * txn_memtable_table_prepared readies pushes to its owners' trees before a
 * checkpoint window and runs the window's work with them, taking inside's
 * closure, whose captures carry its result and what the caller hands it.
 * Each try makes the memtable a tree lacks as its first writer, with no
 * hold; begins the hold, which it keeps; hands each push off to its key's
 * range, pinning its memtable; and then takes every push's reserve in one
 * allocation, going around when a hand-off or the allocation can't. It then
 * runs inside, whose finishes allocate nothing, and settles: it clears its
 * pins, gives back what the finishes left of the reserves, and, unless
 * inside failed, splits each range a finish left past SPLIT keys and closes
 * each memtable a prepare made or pinned that no finish reached. It answers
 * the tuple id table's reservation the prepare was given, if no finish
 * stored it.
 */
bool
txn_memtable_table_prepared(HostIoIo *io, const TxnMemtableTableAim *aims, size_t aims_len, bool has_tuple,
							TxnMemtableTableReservation tuple,
							bool (*inside) (void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
											HostError *error),
							void *inside_captures, void (*inside_drop) (void *captures), bool *has_unstored,
							TxnMemtableTableReservation *unstored, HostError *error)
{
	txn_memtable_table_prepared_frame_0_t frame = {.io = io, .aims = aims, .aims_len = aims_len, .inside = inside,
		.inside_captures = inside_captures, .inside_drop = inside_drop, .has_unstored = has_unstored,
	.unstored = unstored, .error = error, .inside_live = true, .prepared = {.has_tuple = has_tuple, .tuple = tuple}};

	std_landing(&frame, txn_memtable_table_prepared_body_0, txn_memtable_table_prepared_pad_0);
	return frame.ok;
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
 * txn_memtable_table_within runs a step of a finish inside the window on a
 * prepared target: it follows the target's range to the open range that
 * holds its key now and runs the step there, under that range's exclusive
 * lock, with the prepare's hold, the target's key, and its reserve, then
 * marks a push the target made, which a key lock's view keeps.
 */
bool
txn_memtable_table_within(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t index,
						  bool (*step) (void *captures, TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
										const uint8 *key, size_t key_len, TxnMemtableTableReserve *reserve,
										HostError *error),
						  void *captures, HostError *error)
{
	TxnMemtableTableTarget *target;
	txn_memtable_table_within_t within;
	bool		ok;
	uint64		grown;

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
	ok = txn_memtable_table_reached(io, &prepared->hold, &target->at, prepared->aims[index].key.ptr,
									prepared->aims[index].key.len, txn_memtable_table_within_step, &within, error);
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
										 &reservation, frame->error) ||
			!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-tuple-reserved", frame->error))
		{
			txn_memtable_table_readied_pad_0(frame);
			return;
		}
		has_tuple = true;
	}
	frame->inside_live = false;
	if (!txn_memtable_table_prepared(frame->io, frame->aims.ptr, frame->aims.len, has_tuple, reservation,
									 frame->inside, frame->inside_captures, frame->inside_drop, &has_unstored,
									 &unstored, frame->error))
	{
		txn_memtable_table_readied_pad_0(frame);
		return;
	}
	txn_memtable_table_readied_pad_0(frame);
	frame->ok = !has_unstored || row_count == 0 || txn_memtable_tuples_given(frame->io, frame->state, frame->error);
}

/*
 * txn_memtable_table_readied readies a storage's row pushes, none for a
 * prepare of map takes alone, and map takes, each in the storage its take
 * names, as prepare and map's prepare do, with the first row's tuple id slot
 * when tuple is set, taking inside's closure, whose captures carry its
 * result and what the caller hands it.
 */
bool
txn_memtable_table_readied(HostIoIo *io, bool has_rows, HostSharedRecord state, const txn_TupleRefSliceU8Usize_t *rows,
						   size_t rows_len, const TxnMemtableMapTake *takes, size_t takes_len, bool tuple,
						   bool (*inside) (void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
										   HostError *error),
						   void *inside_captures, void (*inside_drop) (void *captures), HostError *error)
{
	txn_memtable_table_readied_frame_0_t frame = {.io = io, .has_rows = has_rows, .state = state, .rows = rows,
		.rows_len = rows_len, .takes = takes, .takes_len = takes_len, .tuple = tuple, .inside = inside,
	.inside_captures = inside_captures, .inside_drop = inside_drop, .error = error, .inside_live = true};

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

/* txn_memtable_table_pushed pushes a prepared target's version inside the window with its reserve, allocating nothing. */
bool
txn_memtable_table_pushed(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, size_t index,
						  const TxnMemtableVersionChange *change, TxnMemtableVersionHandle *result, HostError *error)
{
	txn_memtable_table_pushed_t pushed = {.change = change, .result = result};

	return txn_memtable_table_within(io, prepared, index, txn_memtable_table_pushed_step, &pushed, error);
}

/* txn_memtable_table_counted_at counts a pushed version at its writer's entry when it names one. */
static bool
txn_memtable_table_counted_at(HostIoTakesShort io, const TxnMemtableTablePreparedState *prepared, bool has_roomed,
							  TxnStatusRoomed roomed, HostError *error)
{
	return !has_roomed || txn_status_counted(io, &prepared->hold, roomed, error);
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

	return txn_memtable_table_within(io, prepared, 0, txn_memtable_table_speculated_step, &pushed, error) &&
		txn_memtable_table_counted_at(io, prepared, has_roomed, roomed, error);
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
		txn_memtable_table_within(io, prepared, row, txn_memtable_table_shown_step, &pushed, error) &&
		txn_memtable_table_counted_at(io, prepared, has_roomed, roomed, error);
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
	return txn_memtable_table_pushed(io, prepared, index, change, result, error) &&
		txn_memtable_table_counted_at(io, prepared, has_roomed, roomed, error);
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

/*
 * txn_memtable_table_counted counts a map version a transaction pushed
 * inside its window at its writer's entry, as finish_row counts a row's, so
 * the entry stays while the version does, and uncounted takes it back when
 * the fold's drop removes it; a version written with no Roomed, by no
 * transaction, counts nothing.
 */
bool
txn_memtable_table_counted(HostIoTakesShort io, const TxnMemtableTablePreparedState *prepared, bool has_roomed,
						   TxnStatusRoomed roomed, HostError *error)
{
	return txn_memtable_table_counted_at(io, prepared, has_roomed, roomed, error);
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

	return txn_memtable_table_rowed(prepared, row, "a row's newest version names a row the prepare didn't ready",
									error) &&
		txn_memtable_table_within(io, prepared, row, txn_memtable_table_newest_step, &newest, error);
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
	return *result || txn_spill_holds(io, state, TXN_RUNS_KIND_SPILLED_ROWS, key, key_len, lsn, result, error);
}

/* txn_memtable_table_alone_t is alone's inside's captures: the change and the handle it gives. */
typedef struct txn_memtable_table_alone_t
{
	const TxnMemtableVersionChange *change;
	TxnMemtableVersionHandle *result;
} txn_memtable_table_alone_t;

/* txn_memtable_table_alone_inside pushes alone's one prepared target. */
static bool
txn_memtable_table_alone_inside(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
								HostError *error)
{
	txn_memtable_table_alone_t *alone = captures;

	return txn_memtable_table_pushed(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, alone->change, alone->result, error);
}

/*
 * txn_memtable_table_alone makes a version a key's newest in one of an
 * owner's trees with no window, a prepare of that one push, its finish, and
 * its settle in one call, as a row's push and the runs' loader do.
 */
bool
txn_memtable_table_alone(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner, const uint8 *key,
						 size_t key_len, const TxnMemtableVersionChange *change, TxnMemtableVersionHandle *result,
						 HostError *error)
{
	TxnMemtableTableAim aims[1] = {{.owner = owner, .tree = tree, .key = {key, key_len},
	.need = txn_memtable_version_encoded(&change->writer, change->op, change->shown, change->has_lsn, change->lsn)}};
	txn_memtable_table_alone_t alone = {change, result};
	TxnMemtableTableReservation none = {0};
	bool		has_unstored;
	TxnMemtableTableReservation unstored;

	return txn_memtable_table_prepared(io, aims, 1, false, none, txn_memtable_table_alone_inside, &alone, NULL,
									   &has_unstored, &unstored, error);
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
	const TxnMemtableTableTreeOps *tree = !has_which ? &TXN_MEMTABLE_TABLE_ROWS_TREE_OPS :
		which == TXN_MEMTABLE_MAP_MAP_FREE ? &TXN_MEMTABLE_MAP_FREE_TREE_OPS : &TXN_MEMTABLE_MAP_VALUES_TREE_OPS;

	return txn_memtable_table_alone(tree, io, owner, key, key_len, change, result, error);
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
	return txn_memtable_table_alone(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, key, key_len, change, result, error);
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
 * txn_memtable_table_first_made takes the first range's page and makes the
 * range and the memtable, waiting while the delta can't give them, answering
 * false on an error.
 */
static bool
txn_memtable_table_first_made(txn_memtable_table_first_frame_0_t *frame, HostDeltaPageSize size)
{
	for (;;)
	{
		bool		allocated;
		HostDeltaFull full;

		if (!host_delta_alloc(frame->io, &size, 1, &allocated, &frame->pages, &full, frame->error))
			return false;
		if (!allocated)
		{
			if (!txn_memtable_table_waited(frame->io, full, false, frame->error))
				return false;
			continue;
		}
		frame->pages_live = true;
		if (!host_delta_make(frame->io, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, &allocated, &frame->range, &full,
							 frame->error))
			return false;
		if (!allocated)
		{
			txn_memtable_table_first_pad_0(frame);
			if (!txn_memtable_table_waited(frame->io, full, false, frame->error))
				return false;
			continue;
		}
		frame->range_live = true;
		if (!host_delta_make(frame->io, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, &allocated, &frame->memtable, &full,
							 frame->error))
			return false;
		if (!allocated)
		{
			txn_memtable_table_first_pad_0(frame);
			if (!txn_memtable_table_waited(frame->io, full, false, frame->error))
				return false;
			continue;
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

/* txn_memtable_table_first_body_0 is first's body. */
static void
txn_memtable_table_first_body_0(void *frame_)
{
	txn_memtable_table_first_frame_0_t *frame = frame_;
	uint64		location;
	HostDeltaPageSize size;
	uint64		name;
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE};
	StdIterator pages;
	HostSharedPast word;

	if (txn_memtable_table_memtable(frame->tree, frame->owner, &location))
	{
		frame->ok = true;
		return;
	}
	size = txn_memtable_pages_size(false, 0, 0, txn_memtable_index_least(NULL, 0).words);
	if (!txn_memtable_table_first_made(frame, size) ||
		!txn_memtable_index_begin(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &frame->range, &frame->page.page, 0, NULL, 0,
								  frame->error))
	{
		txn_memtable_table_first_pad_0(frame);
		return;
	}
	name = host_delta_page_Page_value(host_delta_Owned_name(&frame->page.page));
	if (!txn_memtable_table_filled(&frame->range, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES,
								   name, frame->error))
	{
		txn_memtable_table_first_pad_0(frame);
		return;
	}
	op.u.exchange.f1 = name;
	host_delta_Made_atomic(&frame->range, TXN_MEMTABLE_RANGE_RANGE_START, op);
	op.u.exchange.f1 = (uint64) size.words;
	host_delta_Made_atomic(&frame->range, TXN_MEMTABLE_RANGE_RANGE_NEWEST, op);
	if (!txn_memtable_table_filled(&frame->memtable, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape,
								   TXN_MEMTABLE_RANGE_RANGES, host_delta_Made_location(&frame->range), frame->error))
	{
		txn_memtable_table_first_pad_0(frame);
		return;
	}
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
	{
		txn_memtable_table_first_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	word = host_shared_Record_names_past(frame->owner, frame->tree->word);
	if (!host_shared_Past_load(&word, &location))
	{
		HostDeltaNames names = {&HOST_DELTA_PAST_NAMES_OPS, &word};

		frame->memtable_live = false;
		frame->with_live = false;
		host_delta_Made_link(&frame->memtable, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, names, &frame->with);
	}
	frame->ok = true;
	txn_memtable_table_first_pad_0(frame);
}

/*
 * txn_memtable_table_first makes a tree's memtable as its first writer: with
 * no hold and no lock it takes the range's first page, makes the range and
 * the memtable, writes the empty key there as the range's first, and starts
 * each record's list with its one entry; then, under the owner's lock, one
 * link of the memtable into the tree's word makes them live together if the
 * word still names none, and otherwise its drops give everything back. It
 * does nothing for a tree whose word names a memtable.
 */
bool
txn_memtable_table_first(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner, HostError *error)
{
	txn_memtable_table_first_frame_0_t frame = {.tree = tree, .io = io, .owner = owner, .error = error};

	std_landing(&frame, txn_memtable_table_first_body_0, txn_memtable_table_first_pad_0);
	return frame.ok;
}

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
static bool
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
											  taken->len, &emptied, &frame->touched, &bytes, frame->error))
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
 * txn_memtable_table_removed_all takes versions out of the chains of keys
 * given in byte order, as removed does for one key, each range taking out
 * every key of the run it holds under one take of its exclusive lock, with
 * one sweep of its dropped pages and one compaction after, as a spill takes
 * a transaction's keys out of its memtable. It answers, for each key at each
 * handle's place, whether this removal took it out.
 */
bool
txn_memtable_table_removed_all(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
							   const TxnMemtableTableRemoving *keys, size_t keys_len, StdVec *result,
							   HostError *error)
{
	txn_memtable_table_removed_frame_0_t frame = {.tree = tree, .io = io, .owner = owner, .keys = keys,
	.keys_len = keys_len, .result = result, .error = error};

	std_landing(&frame, txn_memtable_table_removed_body_0, txn_memtable_table_removed_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_table_removed takes versions out of a key's chain in one of a
 * tree's memtables, through the hand-off a writer takes and under the key's
 * range's exclusive lock: the versions leave the chain, a key left with none
 * leaves the index, and its dropped pages and its compaction are as
 * removed_all's. A key the tree doesn't hold, and a handle its chain
 * doesn't, are passed over. It answers, at each handle's place, whether this
 * removal took it out.
 */
bool
txn_memtable_table_removed(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
						   const uint8 *key, size_t key_len, const TxnMemtableVersionHandle *handles,
						   size_t handles_len, StdVec *result, HostError *error)
{
	TxnMemtableTableRemoving keys[1] = {{{key, key_len}, handles, handles_len}};
	StdVec		taken;

	if (!txn_memtable_table_removed_all(tree, io, owner, keys, 1, &taken, error))
		return false;
	if (!std_vec_pop(&taken, result, sizeof(StdVec)))
		std_vec_new(result, sizeof(bool), _Alignof(bool));
	std_vec_clear(&taken, sizeof(StdVec), txn_memtable_table_bools_drop);
	std_vec_free_Checked(&taken, sizeof(StdVec), _Alignof(StdVec));
	return true;
}

/* txn_memtable_table_Pruning_t is a chain entry pruned reads: a version's handle, its writer, its hint, and whether it is a lock's. */
typedef struct txn_memtable_table_Pruning_t
{
	TxnMemtableVersionHandle handle;
	TxnMemtableVersionWriter writer;
	TxnMemtableVersionHint hint;
	bool		lock;
} txn_memtable_table_Pruning_t;

/* txn_memtable_table_pruned_t is pruned's reading's captures: the key and the chain read. */
typedef struct txn_memtable_table_pruned_t
{
	const uint8 *key;
	size_t		key_len;
	StdVec		chain;
} txn_memtable_table_pruned_t;

/* txn_memtable_table_is_lock reports whether a version's op is a lock's, which counts nothing. */
static bool
txn_memtable_table_is_lock(const TxnMemtableVersionRecord *record)
{
	TxnMemtableVersionOpRefTag op = txn_memtable_version_Record_op(record).tag;

	return op == TXN_MEMTABLE_VERSION_OP_REF_LOCK || op == TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW;
}

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
		entry.lock = txn_memtable_table_is_lock(&record);
		txn_memtable_version_Version_drop(&version);
		std_vec_push_Checked(&pruned->chain, &entry, sizeof(entry), _Alignof(txn_memtable_table_Pruning_t));
	}
}

/* txn_memtable_table_pruned_frame_0_t is pruned's state across its pad: the chain read, the aborted handles, and the answers, while live. */
typedef struct txn_memtable_table_pruned_frame_0_t
{
	const TxnMemtableTableTreeOps *tree;
	HostIoIo   *io;
	HostSharedRecord owner;
	const uint8 *key;
	size_t		key_len;
	bool		has_aborting;
	uint64		aborting;
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

		if (frame->has_aborting && (entry->writer.has_top ? entry->writer.top : xid) == frame->aborting)
			gone = true;
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
				if (!host_status(HOST_IO_IO_IO_DOORS(frame->io), asked, &status, frame->error))
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
	for (size_t k = 0; k < kept && k < frame->taken.len; k++)
	{
		const txn_memtable_table_Pruning_t *entry = &STD_VEC_AT(&frame->pruned.chain,
																txn_memtable_table_Pruning_t, k);
		TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;

		if (!STD_VEC_AT(&frame->taken, bool, k))
			continue;
		if (frame->tree->counted && !entry->lock && !txn_memtable_version_Writer_PartialEq_eq(&entry->writer, &none))
		{
			HostXid		top = {entry->writer.has_top ? entry->writer.top : entry->writer.xid};
			bool		has;
			uint64		added;

			if (!txn_status_add(frame->io, top, PG_UINT64_MAX, &has, &added, frame->error))
			{
				txn_memtable_table_pruned_pad_0(frame);
				return;
			}
		}
	}
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
 * never pruned here. A top-level transaction's abort names its xid as
 * aborting, and every version it or one of its subtransactions wrote is
 * aborted with no question. Each version but a lock's this removal took out
 * gives its count back at its top-level transaction's entry, so a rolled-back
 * savepoint's versions leave a committed transaction's count as they found
 * it.
 */
bool
txn_memtable_table_pruned(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
						  const uint8 *key, size_t key_len, bool has_aborting, uint64 aborting, HostError *error)
{
	txn_memtable_table_pruned_frame_0_t frame = {.tree = tree, .io = io, .owner = owner, .key = key,
	.key_len = key_len, .has_aborting = has_aborting, .aborting = aborting, .error = error};

	std_landing(&frame, txn_memtable_table_pruned_body_0, txn_memtable_table_pruned_pad_0);
	return frame.ok;
}

/*
 * txn_memtable_table_uncounted takes back the add a version's push made at
 * its writer's entry, for a version taken out of the delta: every version
 * but a lock's, as the finishes count them, at its top-level transaction's
 * entry; a writer whose entry is gone takes nothing back.
 */
bool
txn_memtable_table_uncounted(HostIoIo *io, const TxnMemtableVersionVersion *version, HostError *error)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	TxnMemtableVersionWriter writer;
	HostXid		top;
	bool		has;
	uint64		added;

	if (txn_memtable_table_is_lock(&record))
		return true;
	writer = txn_memtable_version_Record_writer(&record);
	top.f0 = writer.has_top ? writer.top : writer.xid;
	return txn_status_add(io, top, PG_UINT64_MAX, &has, &added, error);
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
 * comes between the allocation and the lock. It answers whether it replaced
 * a page.
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
 * txn_memtable_table_versions lends a key's versions in a storage's rows,
 * newest first, the memtable's and then its spills', for a caller's hold:
 * read under the range's shared lock through the hand-off and answered with
 * the lock released, so a reader can ask Postgres each writer's outcome. It
 * answers none when the delta holds no version of the key.
 */
bool
txn_memtable_table_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, const uint8 *key,
							size_t key_len, StdVec *result, HostError *error)
{
	StdVec		spilled;

	if (!txn_memtable_table_chain(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, hold, state, key, key_len, result, error))
		return false;
	if (!txn_spill_versions(&TXN_SPILL_STATE_HOME_OPS, io, hold, state, TXN_RUNS_KIND_SPILLED_ROWS, key, key_len,
							&spilled, error))
	{
		std_vec_clear(result, sizeof(TxnMemtableVersionVersion), txn_memtable_table_Version_drop);
		std_vec_free_Checked(result, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
		return false;
	}
	std_vec_extend_from_slice_Checked(result, spilled.ptr, spilled.len, sizeof(TxnMemtableVersionVersion),
									  _Alignof(TxnMemtableVersionVersion));
	std_vec_free_Checked(&spilled, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
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
	std_vec_free_Checked(&self->handles, sizeof(txn_TupleHandleU64_t), _Alignof(txn_TupleHandleU64_t));
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
		std_vec_new(&copy.handles, sizeof(txn_TupleHandleU64_t), _Alignof(txn_TupleHandleU64_t));
		for (size_t k = 0; k < chain.len; k++)
		{
			const TxnMemtableVersionVersion *version = &STD_VEC_AT(&chain, TxnMemtableVersionVersion, k);
			TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
			TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);
			txn_TupleHandleU64_t handle = {version->handle, writer.has_top ? writer.top : writer.xid};

			std_vec_push_Checked(&copy.handles, &handle, sizeof(handle), _Alignof(txn_TupleHandleU64_t));
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
						 size_t records_len, const txn_TupleHandleU64_t *handles, size_t handles_len,
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
 * among them only when killed says so, and every version's handle and its
 * writer's top-level xid, newest first, the killed included. Each step
 * copies one range's keys from the walk's place up to the key the step stops
 * at, the next entry's first key, under the range's shared lock, and hands
 * them out with the lock released, so no lock is held across the caller's
 * writes; it goes on from that key. A range that splits or is emptied and
 * retired between the steps leaves no key skipped or handed out twice.
 * Copying allocates, since a range's records must outlive its lock.
 */
bool
txn_memtable_table_copied(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner, bool killed,
						  bool (*each) (void *captures, HostIoIo *io, const uint8 *key, size_t key_len,
										const StdVec *records, size_t records_len,
										const txn_TupleHandleU64_t *handles, size_t handles_len, HostError *error),
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
							 size_t records_len, const txn_TupleHandleU64_t *handles, size_t handles_len,
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

/* txn_memtable_table_Gathered_retire retires what a gathering holds: its records, its lists' pages, and each page whose last list it lowers. */
static bool
txn_memtable_table_Gathered_retire(TxnMemtableTableGathered *self, HostIoIo *io, HostError *error)
{
	TxnMemtableTableRetiring names;
	StdIterator pages;

	for (size_t k = 0; k < self->counts.len; k++)
	{
		txn_memtable_table_DroppedPage_t *count = &STD_VEC_AT(&self->counts, txn_memtable_table_DroppedPage_t, k);

		if (host_delta_Word_fetch_add(&count->f1, PG_UINT64_MAX) == 1)
			std_vec_push_Checked(&self->names, &count->f0, sizeof(uint64), _Alignof(uint64));
	}
	names = (TxnMemtableTableRetiring)
	{
		self->names.ptr, self->names.len, 0
	};
	pages = (StdIterator)
	{
		&TXN_MEMTABLE_TABLE_RETIRING_ITERATOR_OPS, &names
	};
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
 * txn_memtable_table_next lends the nearest key past a key, or before it
 * going backward, or the first or the last with none, that a storage's rows
 * hold a version of, in the memtable or its spills, with its versions newest
 * first, the memtable's and then the spills' for a key both hold, for a
 * caller's hold; it answers nothing past the last.
 */
bool
txn_memtable_table_next(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, bool has_after,
						const uint8 *after, size_t after_len, TxnMemtableTableDirection way, bool *some,
						txn_TupleVecU8VecVersion_t *result, HostError *error)
{
	bool		has_delta;
	txn_TupleVecU8VecVersion_t delta;
	bool		has_spilled;
	txn_TupleVecU8VecVersion_t spilled;

	*some = false;
	if (!txn_memtable_table_nearest(io, hold, state, has_after, after, after_len, way, &has_delta, &delta, error))
		return false;
	if (!txn_spill_next(&TXN_SPILL_STATE_HOME_OPS, io, hold, state, TXN_RUNS_KIND_SPILLED_ROWS, has_after, after,
						after_len, way, &has_spilled, &spilled, error))
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
			std_vec_extend_from_slice_Checked(&delta.f1, spilled.f1.ptr, spilled.f1.len,
											  sizeof(TxnMemtableVersionVersion),
											  _Alignof(TxnMemtableVersionVersion));
			std_vec_free_Checked(&spilled.f0, 1, 1);
			std_vec_free_Checked(&spilled.f1, sizeof(TxnMemtableVersionVersion),
								 _Alignof(TxnMemtableVersionVersion));
			*result = delta;
		}
		else if (way == TXN_MEMTABLE_TABLE_DIRECTION_FORWARD ? order == STD_ORDERING_LESS :
				 order == STD_ORDERING_GREATER)
		{
			txn_memtable_table_Versions_drop(&delta);
			*result = spilled;
		}
		else
		{
			txn_memtable_table_Versions_drop(&spilled);
			*result = delta;
		}
		*some = true;
	}
	else if (has_delta || has_spilled)
	{
		*result = has_delta ? delta : spilled;
		*some = true;
	}
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
