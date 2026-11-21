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
 * The C port of txn's memtable/tuples.rs: a storage's tuple id table in its
 * delta, an open-addressed table of slots, each a tuple id and the place of
 * its key's bytes on the table's byte pages, which a prepare reserves and its
 * finish stores, grown into a new table of twice the slots once it fills.
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
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/index.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/tuples.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "memtable" STD_SEP "tuples.rs"

/* TXN_MEMTABLE_TUPLES_SLOTS and TXN_MEMTABLE_TUPLES_KEYS are the table's lists: its slot pages and its byte pages. */
#define TXN_MEMTABLE_TUPLES_SLOTS 0
#define TXN_MEMTABLE_TUPLES_KEYS 1

/* The table's lock, TuplesLock, and its atomics, by their INDEX in tuples.rs. */
#define TXN_MEMTABLE_TUPLES_LOCK 0
#define TXN_MEMTABLE_TUPLES_CLOSED 0
#define TXN_MEMTABLE_TUPLES_USED 1
#define TXN_MEMTABLE_TUPLES_RESERVED 2
#define TXN_MEMTABLE_TUPLES_COUNT 3
/* TXN_MEMTABLE_TUPLES_DEAD_COUNT counts a table's slots whose tids a spill dropped, until a growth or a compaction leaves them behind. */
#define TXN_MEMTABLE_TUPLES_DEAD_COUNT 4

/* TXN_MEMTABLE_TUPLES_PER is PER, the slots a slot page holds, two words each. */
#define TXN_MEMTABLE_TUPLES_PER UINT64CONST(512)

/* TXN_MEMTABLE_TUPLES_FIRST_COUNT is FIRST_COUNT, a first table's slots. */
#define TXN_MEMTABLE_TUPLES_FIRST_COUNT UINT64CONST(16)

/* TXN_MEMTABLE_TUPLES_FIRST_BYTES is FIRST_BYTES, a first byte page's body. */
#define TXN_MEMTABLE_TUPLES_FIRST_BYTES (UINT64CONST(1) << 10)

/* TXN_MEMTABLE_TUPLES_BYTES is BYTES, the body byte pages double to, past which a key takes a page of its own. */
#define TXN_MEMTABLE_TUPLES_BYTES (UINT64CONST(64) << 10)

/* TXN_MEMTABLE_TUPLES_ROUND is ROUND, what a key's own page is rounded up to. */
#define TXN_MEMTABLE_TUPLES_ROUND (UINT64CONST(4) << 10)

/* TXN_MEMTABLE_TUPLES_OFFSET and TXN_MEMTABLE_TUPLES_LENGTH are a place's offset's and length's bits. */
#define TXN_MEMTABLE_TUPLES_OFFSET 16
#define TXN_MEMTABLE_TUPLES_LENGTH 17

/* TXN_MEMTABLE_TUPLES_DEAD is DEAD, the tuple id word a dropped slot holds, which a search passes over. */
#define TXN_MEMTABLE_TUPLES_DEAD PG_UINT64_MAX

/*
 * TXN_MEMTABLE_TUPLES_DROPPED is the entry, in a table's list of byte pages,
 * of a byte page a compaction retired, which keeps every other page's index,
 * so no key's place moves; no page's name is zero.
 */
#define TXN_MEMTABLE_TUPLES_DROPPED 0

StaticAssertDecl(HOST_DELTA_PAGE_LARGEST <= UINT64CONST(1) << TXN_MEMTABLE_TUPLES_OFFSET &&
				 HOST_DELTA_PAGE_LARGEST <= UINT64CONST(1) << TXN_MEMTABLE_TUPLES_LENGTH &&
				 HOST_DELTA_PAGE_SLOTS <= UINT64CONST(1) << (64 - TXN_MEMTABLE_TUPLES_OFFSET - TXN_MEMTABLE_TUPLES_LENGTH),
				 "a byte page's offsets and a key's length fit their bits, and a byte page's index fits the rest");

/* txn_memtable_tuples_Tuples_Aged_count is Tuples' count: a table for each storage state, a growth's next table taken while the one it replaces waits for the holds that reach it. */
static size_t
txn_memtable_tuples_Tuples_Aged_count(const HostSharedLimits *limits)
{
	return limits->relation_locks;
}

/* TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS is Tuples' Shape and Aged: a storage's tuple id table, its lock, five atomics, and its slot and byte pages. */
const HostSharedAgedOps TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS = {
	.shape = {.id = UINT64CONST(0x74786e000013), .locks = 1, .atomics = 5, .lists = 2},
	.name = {"tuple id table", 14},
	.count = txn_memtable_tuples_Tuples_Aged_count,
};

/* TXN_MEMTABLE_TUPLES_CLOSES_OPS is Tuples' Closes: TuplesLock and TuplesClosed. */
static const TxnMemtableIndexClosesOps TXN_MEMTABLE_TUPLES_CLOSES_OPS = {
	.shape = &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape,
	.lock = TXN_MEMTABLE_TUPLES_LOCK,
	.closed = TXN_MEMTABLE_TUPLES_CLOSED,
};

/* txn_memtable_tuples_internal returns an internal error of the tuple id table. */
static HostError
txn_memtable_tuples_internal(const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* txn_memtable_tuples_slot_page returns the size of each slot page of a table of a count of slots: words alone, two a slot. */
static HostDeltaPageSize
txn_memtable_tuples_slot_page(uint64 count)
{
	HostDeltaPageSize size = {0, STD_MUL(u32, 2, (uint32) Min(count, TXN_MEMTABLE_TUPLES_PER),
										 119, 29)};

	return size;
}

/*
 * txn_memtable_tuples_bytes returns the size of a byte page that holds a key,
 * after a newest byte page of a capacity or as a table's first: twice the
 * newest up to 64 KiB, or 1 KiB for the first, and at least the power of two
 * that holds the key, or past 64 KiB the key's length rounded up to 4 KiB.
 */
static HostDeltaPageSize
txn_memtable_tuples_bytes(size_t key_len, bool has_newest, uint64 newest)
{
	uint64		len = (uint64) key_len;
	uint64		body;
	HostDeltaPageSize size = {0, 0};

	if (len > TXN_MEMTABLE_TUPLES_BYTES)
		body = std_next_multiple_of_u64(len, TXN_MEMTABLE_TUPLES_ROUND);
	else
	{
		body = TXN_MEMTABLE_TUPLES_FIRST_BYTES;
		if (has_newest)
			body = Min(STD_MUL(u64, newest, 2, 155, 55), TXN_MEMTABLE_TUPLES_BYTES);
		body = Max(body, std_next_power_of_two_u64(len, RUST_FILE, 155, 83));
	}
	size.bytes = (uint32) body;
	return size;
}

/* txn_memtable_tuples_slot returns a tuple id's first slot in a table of a count of slots, by a multiplicative hash. */
static uint64
txn_memtable_tuples_slot(uint64 tid, uint64 count)
{
	return STD_SHR(u64, std_wrapping_mul_u64(tid, UINT64CONST(0x9e3779b97f4a7c15)),
				   STD_SUB(u32, 64, std_trailing_zeros_u64(count), 163, 49),
				   163, 5);
}

/* txn_memtable_tuples_placed returns a key's place: its byte page's index, its offset there, and its length. */
static uint64
txn_memtable_tuples_placed(uint64 page, uint32 offset, size_t length)
{
	return STD_SHL_FOLDED(u64, page, TXN_MEMTABLE_TUPLES_OFFSET + TXN_MEMTABLE_TUPLES_LENGTH) |
		STD_SHL_FOLDED(u64, (uint64) offset, TXN_MEMTABLE_TUPLES_LENGTH) | (uint64) length;
}

/* txn_memtable_tuples_full reports whether a table of a count of slots passes three quarters with one slot more. */
static bool
txn_memtable_tuples_full(uint64 used, uint64 reserved, uint64 count)
{
	return STD_ADD(u64, STD_ADD(u64, used, reserved, 173, 5), 1, 173, 5) >
		STD_MUL(u64, STD_DIV_FOLDED(u64, count, 4), 3, 173, 27);
}

/* TxnMemtableTuplesWantTag is Want's variants. */
typedef enum TxnMemtableTuplesWantTag
{
	TXN_MEMTABLE_TUPLES_WANT_FIRST,
	TXN_MEMTABLE_TUPLES_WANT_GROW,
	TXN_MEMTABLE_TUPLES_WANT_BYTE,
} TxnMemtableTuplesWantTag;

/*
 * TxnMemtableTuplesWant is Want, what a reservation's try takes before its
 * hold: a first table with its byte page, a growth's table of a count of
 * slots and of byte pages with a byte page for the key, or a byte page for
 * the key with the spares its list wants.
 */
typedef struct TxnMemtableTuplesWant
{
	TxnMemtableTuplesWantTag tag;
	uint64		count;
	uint64		keys;
	HostDeltaPageSize byte;
	size_t		spares;
} TxnMemtableTuplesWant;

/* txn_memtable_tuples_Want_PartialEq_eq is Want's derived PartialEq. */
static bool
txn_memtable_tuples_Want_PartialEq_eq(const TxnMemtableTuplesWant *a, const TxnMemtableTuplesWant *b)
{
	if (a->tag != b->tag || a->byte.bytes != b->byte.bytes || a->byte.words != b->byte.words)
		return false;
	if (a->tag == TXN_MEMTABLE_TUPLES_WANT_GROW)
		return a->count == b->count && a->keys == b->keys;
	if (a->tag == TXN_MEMTABLE_TUPLES_WANT_BYTE)
		return a->spares == b->spares;
	return true;
}

/* txn_memtable_tuples_Want_sizes writes into an empty Vec the pages a want takes: its slot pages, then its byte page, then its lists' pages. */
static bool
txn_memtable_tuples_Want_sizes(TxnMemtableTuplesWant self, StdVec *result, HostError *error)
{
	uint64		count = 0;
	uint64		pages = 0;
	size_t		spares = 0;
	HostDeltaPageSize slot;
	HostDeltaPageSize list_page = HOST_DELTA_LIST_PAGE;

	switch (self.tag)
	{
		case TXN_MEMTABLE_TUPLES_WANT_FIRST:
			count = TXN_MEMTABLE_TUPLES_FIRST_COUNT;
			pages = 1;
			break;
		case TXN_MEMTABLE_TUPLES_WANT_GROW:
			count = self.count;
			pages = std_div_ceil_u64(self.count, TXN_MEMTABLE_TUPLES_PER, RUST_FILE, 196, 29);
			spares = STD_ADD(usize, host_delta_list_pages(pages), host_delta_list_pages(self.keys),
							 197, 38);
			break;
		case TXN_MEMTABLE_TUPLES_WANT_BYTE:
			spares = self.spares;
			break;
	}
	if (!std_vec_try_reserve(result,
							 STD_ADD(usize, STD_ADD(usize, (size_t) pages, 1, 202, 27),
									 spares, 202, 27),
							 sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	slot = txn_memtable_tuples_slot_page(count);
	for (uint64 k = 0; k < pages; k++)
		std_vec_push_Checked(result, &slot, sizeof(slot), _Alignof(HostDeltaPageSize));
	std_vec_push_Checked(result, &self.byte, sizeof(self.byte), _Alignof(HostDeltaPageSize));
	for (size_t k = 0; k < spares; k++)
		std_vec_push_Checked(result, &list_page, sizeof(list_page), _Alignof(HostDeltaPageSize));
	return true;
}

/* TxnMemtableTuplesTaken is Taken: what a try took for its want, its pages in the want's order and its record. */
typedef struct TxnMemtableTuplesTaken
{
	TxnMemtableTuplesWant want;
	/* pages is a Vec<Owned>. */
	StdVec		pages;
	bool		has_made;
	HostDeltaMade made;
} TxnMemtableTuplesTaken;

/* txn_memtable_tuples_Owneds_drop drops a Vec<Owned>'s pages from an index on, and its buffer. */
static void
txn_memtable_tuples_Owneds_drop(StdVec *pages, size_t from)
{
	std_ptr_drop_in_place_slice(STD_VEC_AT_SIZE(pages, from, sizeof(HostDeltaOwned)), pages->len - from,
								sizeof(HostDeltaOwned), host_delta_Owned_drop);
	std_vec_free_Checked(pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
}

/* txn_memtable_tuples_Taken_drop gives back what a try took: its pages and its record. */
static void
txn_memtable_tuples_Taken_drop(TxnMemtableTuplesTaken *self)
{
	txn_memtable_tuples_Owneds_drop(&self->pages, 0);
	if (self->has_made)
	{
		self->has_made = false;
		host_delta_Made_drop(&self->made);
	}
}

/* txn_memtable_tuples_view lends one of a table's lists under its lock. */
static HostDeltaListListView
txn_memtable_tuples_view(const HostSharedGuard *guard, size_t list)
{
	return host_delta_list_Guard_list(guard, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape, list);
}

/* txn_memtable_tuples_word reads a slot's tid word, or its place word when place is set, from the slot page that holds it. */
static bool
txn_memtable_tuples_word(const HostDeltaPageRef *page, uint64 at, bool place, uint32 line, uint32 col,
						 HostDeltaWord *result, HostError *error)
{
	uint64		index = STD_MUL(u64, 2, STD_REM_FOLDED(u64, at, TXN_MEMTABLE_TUPLES_PER), line, col);

	if (place)
		index = STD_ADD(u64, index, 1, line, col);
	return host_delta_PageRef_word(page, index, result, error);
}

/* txn_memtable_tuples_slots_page reads the slot page of a table that holds a slot. */
static bool
txn_memtable_tuples_slots_page(const HostDeltaListListView *slots, const HostDeltaHold *hold, uint64 at,
							   HostDeltaPageRef *result, HostError *error)
{
	uint64		name;

	return host_delta_list_ListView_get(slots, hold, STD_DIV_FOLDED(u64, at, TXN_MEMTABLE_TUPLES_PER), &name, error) &&
		host_delta_Hold_page(hold, host_delta_page_Page_named(name), result, error);
}

/*
 * TxnMemtableTuplesRetiring is Retiring, what a table's retirement takes: its
 * record and the names of its pages.
 */
typedef struct TxnMemtableTuplesRetiring
{
	HostDeltaLeaving record;
	/* names is a Vec<u64> of pages' names. */
	StdVec		names;
} TxnMemtableTuplesRetiring;

/* txn_memtable_tuples_Retiring_retire retires a table's record and pages. */
static bool
txn_memtable_tuples_Retiring_retire(const TxnMemtableTuplesRetiring *self, HostIoIo *io, HostError *error)
{
	TxnMemtableTableRetiring names = {self->names.ptr, self->names.len, 0};
	StdIterator pages = {&TXN_MEMTABLE_TABLE_RETIRING_ITERATOR_OPS, &names};

	return host_delta_retire(io, &self->record, 1, pages, error);
}

/*
 * txn_memtable_tuples_closed closes an open table under its exclusive lock,
 * both lists and then its mark, and gathers its retirement into a live
 * Retiring: the record, its slot pages, its lists' own pages, and, for a
 * table that leaves no next one, its byte pages but those a compaction
 * retired.
 */
static bool
txn_memtable_tuples_closed(HostSharedGuard *guard, const HostDeltaHold *hold, uint64 location, bool bytes,
						   TxnMemtableTuplesRetiring *result, HostError *error)
{
	HostDeltaListListMut keys_mut = host_delta_list_GuardExclusive_list_mut(guard,
																			&TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape,
																			TXN_MEMTABLE_TUPLES_LOCK,
																			TXN_MEMTABLE_TUPLES_KEYS);
	TxnMemtableIndexOpen open;
	HostDeltaListClosed slots;
	HostDeltaListClosed keys;
	HostDeltaListListView slot_pages;
	HostDeltaListListView byte_pages;
	uint64		byte_len;

	host_delta_list_ListMut_close(&keys_mut);
	if (!txn_memtable_index_open(&TXN_MEMTABLE_TUPLES_CLOSES_OPS, guard, &open))
	{
		*error = txn_memtable_tuples_internal("a closing table is closed");
		return false;
	}
	txn_memtable_index_Open_close(open);
	if (!host_delta_list_Guard_closed(guard, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape, TXN_MEMTABLE_TUPLES_SLOTS,
									  &slots) ||
		!host_delta_list_Guard_closed(guard, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape, TXN_MEMTABLE_TUPLES_KEYS,
									  &keys))
	{
		*error = txn_memtable_tuples_internal("a closed table's list is open");
		return false;
	}
	slot_pages = txn_memtable_tuples_view(guard, TXN_MEMTABLE_TUPLES_SLOTS);
	byte_pages = txn_memtable_tuples_view(guard, TXN_MEMTABLE_TUPLES_KEYS);
	byte_len = bytes ? host_delta_list_ListView_len(&byte_pages) : 0;
	if (!std_vec_try_reserve(&result->names,
							 (size_t) STD_ADD(u64, host_delta_list_ListView_len(&slot_pages), byte_len,
											  467, 17),
							 sizeof(uint64), _Alignof(uint64)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	for (uint64 index = 0, len = host_delta_list_ListView_len(&slot_pages); index < len; index++)
	{
		uint64		name;

		if (!host_delta_list_ListView_get(&slot_pages, hold, index, &name, error))
			return false;
		std_vec_push_Checked(&result->names, &name, sizeof(name), _Alignof(uint64));
	}
	for (uint64 index = 0; index < byte_len; index++)
	{
		uint64		name;

		if (!host_delta_list_ListView_get(&byte_pages, hold, index, &name, error))
			return false;
		if (name != TXN_MEMTABLE_TUPLES_DROPPED)
			std_vec_push_Checked(&result->names, &name, sizeof(name), _Alignof(uint64));
	}
	if (!txn_memtable_table_closed_names(&result->names, &slots, hold, error) ||
		!txn_memtable_table_closed_names(&result->names, &keys, hold, error))
		return false;
	result->record = host_delta_Leaving_of(&TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS, location);
	return true;
}

/* txn_memtable_tuples_word_of returns the state's word that names its tuple id table. */
static HostSharedPast
txn_memtable_tuples_word_of(HostSharedRecord state)
{
	return host_shared_Record_names_past(state, TXN_STATE_TUPLES_WORD_INDEX);
}

/* txn_memtable_tuples_first_frame_0_t is first's state across its pad. */
typedef struct txn_memtable_tuples_first_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	TxnMemtableTuplesTaken took;
	HostError  *error;
	bool		ok;
	bool		pages_live;
	bool		made_live;
	HostDeltaOwned slots;
	bool		slots_live;
	HostDeltaOwned byte;
	bool		byte_live;
	HostDeltaWith with;
	bool		with_live;
	HostSharedGuard guard;
	bool		guard_live;
} txn_memtable_tuples_first_frame_0_t;

/* txn_memtable_tuples_first_pad_0 is first's pad: the state's guard, the With, the slot and byte pages, the record, and the try's pages, while live. */
static void
txn_memtable_tuples_first_pad_0(void *frame_)
{
	txn_memtable_tuples_first_frame_0_t *frame = frame_;

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
	if (frame->slots_live)
	{
		frame->slots_live = false;
		host_delta_Owned_drop(&frame->slots);
	}
	if (frame->byte_live)
	{
		frame->byte_live = false;
		host_delta_Owned_drop(&frame->byte);
	}
	if (frame->made_live)
	{
		frame->made_live = false;
		host_delta_Made_drop(&frame->took.made);
	}
	if (frame->pages_live)
	{
		frame->pages_live = false;
		txn_memtable_tuples_Owneds_drop(&frame->took.pages, 0);
	}
}

/* txn_memtable_tuples_first_body_0 is first's body. */
static void
txn_memtable_tuples_first_body_0(void *frame_)
{
	txn_memtable_tuples_first_frame_0_t *frame = frame_;
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE, .u.exchange = {0, TXN_MEMTABLE_TUPLES_FIRST_COUNT}};
	HostSharedPast word;
	uint64		location;

	if (!frame->made_live)
	{
		*frame->error = txn_memtable_tuples_internal("a first table's try made no record");
		txn_memtable_tuples_first_pad_0(frame);
		return;
	}
	frame->byte_live = std_vec_pop(&frame->took.pages, &frame->byte, sizeof(HostDeltaOwned));
	if (!frame->byte_live)
	{
		*frame->error = txn_memtable_tuples_internal("a first table's try took no byte page");
		txn_memtable_tuples_first_pad_0(frame);
		return;
	}
	frame->slots_live = std_vec_pop(&frame->took.pages, &frame->slots, sizeof(HostDeltaOwned));
	if (!frame->slots_live)
	{
		*frame->error = txn_memtable_tuples_internal("a first table's try took no slot page");
		txn_memtable_tuples_first_pad_0(frame);
		return;
	}
	if (!txn_memtable_table_filled(&frame->took.made, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape,
								   TXN_MEMTABLE_TUPLES_SLOTS,
								   host_delta_page_Page_value(host_delta_Owned_name(&frame->slots)), frame->error) ||
		!txn_memtable_table_filled(&frame->took.made, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape,
								   TXN_MEMTABLE_TUPLES_KEYS,
								   host_delta_page_Page_value(host_delta_Owned_name(&frame->byte)), frame->error))
	{
		txn_memtable_tuples_first_pad_0(frame);
		return;
	}
	host_delta_Made_atomic(&frame->took.made, TXN_MEMTABLE_TUPLES_COUNT, op);
	frame->with = host_delta_With_Default_default();
	frame->with_live = true;
	frame->slots_live = false;
	if (!host_delta_With_page(&frame->with, TXN_MEMTABLE_TUPLES_LOCK, &frame->slots, frame->error))
	{
		txn_memtable_tuples_first_pad_0(frame);
		return;
	}
	frame->byte_live = false;
	if (!host_delta_With_page(&frame->with, TXN_MEMTABLE_TUPLES_LOCK, &frame->byte, frame->error) ||
		!host_shared_Record_lock(frame->state, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_STATE_LOCK_INDEX,
								 &frame->guard, frame->error))
	{
		txn_memtable_tuples_first_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	word = txn_memtable_tuples_word_of(frame->state);
	if (!host_shared_Past_load(&word, &location))
	{
		HostDeltaNames names = {&HOST_DELTA_PAST_NAMES_OPS, &word};

		frame->made_live = false;
		frame->with_live = false;
		host_delta_Made_link(&frame->took.made, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS, names, &frame->with);
	}
	frame->ok = true;
	txn_memtable_tuples_first_pad_0(frame);
}

/*
 * txn_memtable_tuples_first makes a storage's first table with its taken slot
 * page and byte page and, under the storage state's lock, links it into the
 * state's word if the word still names none; otherwise its drops give
 * everything back. It takes the try.
 */
static bool
txn_memtable_tuples_first(HostIoIo *io, HostSharedRecord state, TxnMemtableTuplesTaken took, HostError *error)
{
	txn_memtable_tuples_first_frame_0_t frame = {.io = io, .state = state, .took = took, .error = error,
	.pages_live = true, .made_live = took.has_made};

	std_landing(&frame, txn_memtable_tuples_first_body_0, txn_memtable_tuples_first_pad_0);
	return frame.ok;
}

/* txn_memtable_tuples_pushed_frame_0_t is pushed's state across its pad. */
typedef struct txn_memtable_tuples_pushed_frame_0_t
{
	TxnMemtableIndexOpen *open;
	const HostDeltaHold *hold;
	TxnMemtableTuplesTaken took;
	const uint8 *key;
	size_t		key_len;
	TxnMemtableTableReservation *result;
	HostError  *error;
	bool		ok;
	/* took.pages holds the spares once the byte page is taken from it. */
	bool		spare_live;
} txn_memtable_tuples_pushed_frame_0_t;

/* txn_memtable_tuples_pushed_pad_0 is pushed's pad: the spares, while live. */
static void
txn_memtable_tuples_pushed_pad_0(void *frame_)
{
	txn_memtable_tuples_pushed_frame_0_t *frame = frame_;

	if (frame->spare_live)
	{
		frame->spare_live = false;
		txn_memtable_tuples_Owneds_drop(&frame->took.pages, 0);
	}
}

/* txn_memtable_tuples_pushed_body_0 is pushed's body. */
static void
txn_memtable_tuples_pushed_body_0(void *frame_)
{
	txn_memtable_tuples_pushed_frame_0_t *frame = frame_;
	HostDeltaListListMut keys = host_delta_list_GuardExclusive_list_mut(frame->open->guard,
																		&TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape,
																		TXN_MEMTABLE_TUPLES_LOCK,
																		TXN_MEMTABLE_TUPLES_KEYS);
	HostDeltaListListView view;
	HostDeltaOwned byte;
	HostDeltaPagePage name;
	HostDeltaPageRef page;
	uint64		newest;
	uint32		offset;
	HostSharedOp op = {.tag = HOST_SHARED_OP_ADD, .u.add = 1};

	if (frame->took.pages.len == 0)
	{
		*frame->error = txn_memtable_tuples_internal("a byte page's try took no page");
		txn_memtable_tuples_pushed_pad_0(frame);
		return;
	}
	std_vec_remove(&frame->took.pages, 0, &byte, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), RUST_FILE,
				   330, 16);
	if (!host_delta_list_ListMut_push_page(&keys, frame->hold, &byte, &frame->took.pages, &name, frame->error))
	{
		txn_memtable_tuples_pushed_pad_0(frame);
		return;
	}
	view = txn_memtable_tuples_view(frame->open->guard, TXN_MEMTABLE_TUPLES_KEYS);
	newest = STD_SUB(u64, host_delta_list_ListView_len(&view), 1, 333, 18);
	if (!host_delta_Hold_page(frame->hold, name, &page, frame->error) ||
		!host_delta_PageRef_append(&page, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape, TXN_MEMTABLE_TUPLES_LOCK,
								   frame->open->guard, frame->key, frame->key_len, &offset, frame->error))
	{
		txn_memtable_tuples_pushed_pad_0(frame);
		return;
	}
	host_shared_Record_atomic(host_shared_Guard_record(frame->open->guard), TXN_MEMTABLE_TUPLES_RESERVED, op);
	frame->result->place = txn_memtable_tuples_placed(newest, offset, frame->key_len);
	frame->ok = true;
	txn_memtable_tuples_pushed_pad_0(frame);
}

/*
 * txn_memtable_tuples_pushed puts a fresh byte page on an open table's list
 * of byte pages, appends the key's bytes to it, and counts the slot reserved.
 * It takes the try.
 */
static bool
txn_memtable_tuples_pushed(TxnMemtableIndexOpen *open, const HostDeltaHold *hold, TxnMemtableTuplesTaken took,
						   const uint8 *key, size_t key_len, TxnMemtableTableReservation *result, HostError *error)
{
	txn_memtable_tuples_pushed_frame_0_t frame = {.open = open, .hold = hold, .took = took, .key = key,
	.key_len = key_len, .result = result, .error = error, .spare_live = true};

	std_landing(&frame, txn_memtable_tuples_pushed_body_0, txn_memtable_tuples_pushed_pad_0);
	return frame.ok;
}

/* txn_memtable_tuples_grown_frame_0_t is grown's state across its pad. */
typedef struct txn_memtable_tuples_grown_frame_0_t
{
	HostSharedGuard *guard;
	const HostDeltaHold *hold;
	HostSharedRecord state;
	uint64		location;
	TxnMemtableTuplesTaken took;
	TxnMemtableTuplesRetiring *result;
	HostError  *error;
	bool		ok;
	bool		made_live;
	/* took.pages holds the slot pages, of which moved are taken. */
	bool		slots_live;
	size_t		moved;
	/* spare is a Vec<Owned>. */
	StdVec		spare;
	bool		spare_live;
	HostDeltaOwned byte;
	bool		byte_live;
	/* used is a Vec<bool> of the new table's slots. */
	StdVec		used;
	bool		used_live;
	/* names is a Vec<u64> of the byte pages' names. */
	StdVec		names;
	bool		names_live;
	/* slot_names is a Vec<u64> of the new slot pages' names. */
	StdVec		slot_names;
	bool		slot_names_live;
	HostDeltaWith with;
	bool		with_live;

	/*
	 * filled is a Vec<Owned> a list's fill took, of which filled_moved are
	 * taken.
	 */
	StdVec		filled;
	bool		filled_live;
	size_t		filled_moved;
} txn_memtable_tuples_grown_frame_0_t;

/* txn_memtable_tuples_grown_pad_0 is grown's pad: what it holds of its try and its lists, while live. */
static void
txn_memtable_tuples_grown_pad_0(void *frame_)
{
	txn_memtable_tuples_grown_frame_0_t *frame = frame_;

	if (frame->filled_live)
	{
		frame->filled_live = false;
		txn_memtable_tuples_Owneds_drop(&frame->filled, frame->filled_moved);
	}
	if (frame->with_live)
	{
		frame->with_live = false;
		host_delta_With_drop(&frame->with);
	}
	if (frame->slot_names_live)
	{
		frame->slot_names_live = false;
		std_vec_free_Checked(&frame->slot_names, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->names_live)
	{
		frame->names_live = false;
		std_vec_free_Checked(&frame->names, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->used_live)
	{
		frame->used_live = false;
		std_vec_free_Checked(&frame->used, sizeof(bool), _Alignof(bool));
	}
	if (frame->spare_live)
	{
		frame->spare_live = false;
		txn_memtable_tuples_Owneds_drop(&frame->spare, 0);
	}
	if (frame->byte_live)
	{
		frame->byte_live = false;
		host_delta_Owned_drop(&frame->byte);
	}
	if (frame->slots_live)
	{
		frame->slots_live = false;
		txn_memtable_tuples_Owneds_drop(&frame->took.pages, frame->moved);
	}
	if (frame->made_live)
	{
		frame->made_live = false;
		host_delta_Made_drop(&frame->took.made);
	}
}

/* txn_memtable_tuples_grown_with moves the pages of a live list of them into the growth's With. */
static bool
txn_memtable_tuples_grown_with(txn_memtable_tuples_grown_frame_0_t *frame, StdVec *pages, size_t *moved)
{
	while (*moved < pages->len)
	{
		HostDeltaOwned page = STD_VEC_AT(pages, HostDeltaOwned, *moved);

		*moved = std_wrapping_add_usize(*moved, 1);
		if (!host_delta_With_page(&frame->with, TXN_MEMTABLE_TUPLES_LOCK, &page, frame->error))
			return false;
	}
	return true;
}

/* txn_memtable_tuples_grown_filled fills one of the new table's lists and moves the pages its fill took into the With. */
static bool
txn_memtable_tuples_grown_filled(txn_memtable_tuples_grown_frame_0_t *frame, size_t list, const StdVec *entries)
{
	if (!host_delta_list_Made_fill_list(&frame->took.made, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape, list,
										entries->ptr, entries->len, &frame->spare, &frame->filled, frame->error))
		return false;
	frame->filled_live = true;
	frame->filled_moved = 0;
	if (!txn_memtable_tuples_grown_with(frame, &frame->filled, &frame->filled_moved))
		return false;
	frame->filled_live = false;
	std_vec_free_Checked(&frame->filled, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	return true;
}

/* txn_memtable_tuples_grown_copied copies every used slot of the old table but a dropped one to the new slot pages. */
static bool
txn_memtable_tuples_grown_copied(txn_memtable_tuples_grown_frame_0_t *frame, uint64 count, uint64 *kept)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	uint64		old = host_shared_Record_atomic(host_shared_Guard_record(frame->guard), TXN_MEMTABLE_TUPLES_COUNT,
												load);
	HostDeltaListListView old_slots = txn_memtable_tuples_view(frame->guard, TXN_MEMTABLE_TUPLES_SLOTS);
	bool		no = false;

	std_vec_new(&frame->used, sizeof(bool), _Alignof(bool));
	frame->used_live = true;
	if (!std_vec_try_reserve_exact(&frame->used, (size_t) count, sizeof(bool), _Alignof(bool)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_resize(&frame->used, (size_t) count, &no, sizeof(bool), _Alignof(bool));
	*kept = 0;
	for (uint64 index = 0, len = host_delta_list_ListView_len(&old_slots); index < len; index++)
	{
		uint64		name;
		HostDeltaPageRef page;

		if (!host_delta_list_ListView_get(&old_slots, frame->hold, index, &name, frame->error) ||
			!host_delta_Hold_page(frame->hold, host_delta_page_Page_named(name), &page, frame->error))
			return false;
		for (uint64 at = 0, most = Min(old, TXN_MEMTABLE_TUPLES_PER); at < most; at++)
		{
			HostDeltaWord word;
			uint64		tid;
			uint64		to;
			uint64		placed;
			HostDeltaOwned *target;

			if (!host_delta_PageRef_word(&page, STD_MUL(u64, 2, at, 371, 33), &word, frame->error))
				return false;
			tid = host_delta_Word_load(&word);
			if (tid == 0 || tid == TXN_MEMTABLE_TUPLES_DEAD)
				continue;
			to = txn_memtable_tuples_slot(STD_SUB(u64, tid, 1, 375, 31), count);
			for (;;)
			{
				std_index_check((size_t) to, frame->used.len, RUST_FILE, 376, 19);
				if (!STD_VEC_AT(&frame->used, bool, to))
					break;
				to = STD_REM(u64, STD_ADD(u64, to, 1, 377, 23), count, 377, 22);
			}
			std_index_check((size_t) to, frame->used.len, RUST_FILE, 379, 13);
			STD_VEC_AT(&frame->used, bool, to) = true;
			*kept = STD_ADD(u64, *kept, 1, 380, 13);
			std_index_check((size_t) STD_DIV_FOLDED(u64, to, TXN_MEMTABLE_TUPLES_PER), frame->took.pages.len,
							RUST_FILE, 381, 31);
			target = &STD_VEC_AT(&frame->took.pages, HostDeltaOwned, STD_DIV_FOLDED(u64, to, TXN_MEMTABLE_TUPLES_PER));
			if (!host_delta_Owned_store(target,
										STD_MUL(u64, 2, STD_REM_FOLDED(u64, to, TXN_MEMTABLE_TUPLES_PER),
												382, 26),
										tid, frame->error))
				return false;
			placed = STD_ADD(u64, STD_MUL(u64, 2, STD_REM_FOLDED(u64, to, TXN_MEMTABLE_TUPLES_PER),
										  383, 26),
							 1, 383, 26);
			if (!host_delta_PageRef_word(&page, STD_ADD(u64, STD_MUL(u64, 2, at, 383, 56), 1,
														383, 56),
										 &word, frame->error) ||
				!host_delta_Owned_store(target, placed, host_delta_Word_load(&word), frame->error))
				return false;
		}
	}
	frame->used_live = false;
	std_vec_free_Checked(&frame->used, sizeof(bool), _Alignof(bool));
	return true;
}

/* txn_memtable_tuples_grown_named gathers the names of the old table's byte pages, then the fresh one's, and the new slot pages' names. */
static bool
txn_memtable_tuples_grown_named(txn_memtable_tuples_grown_frame_0_t *frame, uint64 *newest)
{
	HostDeltaListListView old_keys = txn_memtable_tuples_view(frame->guard, TXN_MEMTABLE_TUPLES_KEYS);
	uint64		len = host_delta_list_ListView_len(&old_keys);
	uint64		name;

	std_vec_new(&frame->names, sizeof(uint64), _Alignof(uint64));
	frame->names_live = true;
	if (!std_vec_try_reserve_exact(&frame->names, STD_ADD(usize, (size_t) len, 1, 388, 29),
								   sizeof(uint64), _Alignof(uint64)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	for (uint64 index = 0; index < len; index++)
	{
		if (!host_delta_list_ListView_get(&old_keys, frame->hold, index, &name, frame->error))
			return false;
		std_vec_push_Checked(&frame->names, &name, sizeof(name), _Alignof(uint64));
	}
	if (!host_delta_list_ListView_get(&old_keys, frame->hold, STD_SUB(u64, len, 1, 392, 49),
									  newest, frame->error))
		return false;
	name = host_delta_page_Page_value(host_delta_Owned_name(&frame->byte));
	std_vec_push_Checked(&frame->names, &name, sizeof(name), _Alignof(uint64));
	std_vec_new(&frame->slot_names, sizeof(uint64), _Alignof(uint64));
	frame->slot_names_live = true;
	if (!std_vec_try_reserve_exact(&frame->slot_names, frame->took.pages.len, sizeof(uint64), _Alignof(uint64)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	for (size_t k = 0; k < frame->took.pages.len; k++)
	{
		name = host_delta_page_Page_value(host_delta_Owned_name(&STD_VEC_AT(&frame->took.pages, HostDeltaOwned, k)));
		std_vec_push_Checked(&frame->slot_names, &name, sizeof(name), _Alignof(uint64));
	}
	return true;
}

/* txn_memtable_tuples_grown_body_0 is grown's body. */
static void
txn_memtable_tuples_grown_body_0(void *frame_)
{
	txn_memtable_tuples_grown_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Guard_record(frame->guard);
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE};
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostSharedPast word;
	HostDeltaNames names;
	HostDeltaPageRef page;
	uint64		count;
	size_t		pages;
	uint64		kept;
	uint64		newest;

	if (frame->took.want.tag != TXN_MEMTABLE_TUPLES_WANT_GROW)
	{
		*frame->error = txn_memtable_tuples_internal("a growth's try wanted no growth");
		txn_memtable_tuples_grown_pad_0(frame);
		return;
	}
	count = frame->took.want.count;
	if (!frame->made_live)
	{
		*frame->error = txn_memtable_tuples_internal("a growth's try made no record");
		txn_memtable_tuples_grown_pad_0(frame);
		return;
	}
	pages = (size_t) std_div_ceil_u64(count, TXN_MEMTABLE_TUPLES_PER, RUST_FILE, 359, 57);
	if (frame->took.pages.len <= pages)
	{
		*frame->error = txn_memtable_tuples_internal("a growth's try took no byte page");
		txn_memtable_tuples_grown_pad_0(frame);
		return;
	}
	std_vec_split_off(&frame->took.pages, pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), &frame->spare,
					  RUST_FILE, 359, 37);
	frame->spare_live = true;
	std_vec_remove(&frame->spare, 0, &frame->byte, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), RUST_FILE,
				   360, 16);
	frame->byte_live = true;
	if (!txn_memtable_tuples_grown_copied(frame, count, &kept) ||
		!txn_memtable_tuples_grown_named(frame, &newest))
	{
		txn_memtable_tuples_grown_pad_0(frame);
		return;
	}
	frame->with = host_delta_With_Default_default();
	frame->with_live = true;
	if (!txn_memtable_tuples_grown_filled(frame, TXN_MEMTABLE_TUPLES_SLOTS, &frame->slot_names) ||
		!txn_memtable_tuples_grown_filled(frame, TXN_MEMTABLE_TUPLES_KEYS, &frame->names) ||
		!txn_memtable_tuples_grown_with(frame, &frame->took.pages, &frame->moved))
	{
		txn_memtable_tuples_grown_pad_0(frame);
		return;
	}
	frame->byte_live = false;
	if (!host_delta_With_page(&frame->with, TXN_MEMTABLE_TUPLES_LOCK, &frame->byte, frame->error))
	{
		txn_memtable_tuples_grown_pad_0(frame);
		return;
	}
	op.u.exchange.f1 = count;
	host_delta_Made_atomic(&frame->took.made, TXN_MEMTABLE_TUPLES_COUNT, op);
	op.u.exchange.f1 = kept;
	host_delta_Made_atomic(&frame->took.made, TXN_MEMTABLE_TUPLES_USED, op);
	op.u.exchange.f1 = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_RESERVED, load);
	host_delta_Made_atomic(&frame->took.made, TXN_MEMTABLE_TUPLES_RESERVED, op);
	if (!host_delta_Hold_page(frame->hold, host_delta_page_Page_named(newest), &page, frame->error) ||
		!host_delta_PageRef_seal(&page, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape, TXN_MEMTABLE_TUPLES_LOCK,
								 frame->guard, frame->error) ||
		!host_pause(HOST_SHARED_GUARD_PAUSES(frame->guard), "doltgres-tuples-growing", frame->error))
	{
		txn_memtable_tuples_grown_pad_0(frame);
		return;
	}
	word = txn_memtable_tuples_word_of(frame->state);
	names = (HostDeltaNames)
	{
		&HOST_DELTA_PAST_NAMES_OPS, &word
	};
	frame->made_live = false;
	frame->with_live = false;
	host_delta_Made_link(&frame->took.made, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS, names, &frame->with);
	frame->ok = txn_memtable_tuples_closed(frame->guard, frame->hold, frame->location, false, frame->result,
										   frame->error);
	txn_memtable_tuples_grown_pad_0(frame);
}

/*
 * txn_memtable_tuples_grown replaces an open table by one of twice its slots
 * under its exclusive lock: every used slot but a dropped one is copied to
 * the new slot pages, the new list of byte pages names the old one's pages
 * in order and then a fresh one, the old newest byte page is sealed, since
 * the old table's lock owns it, the counts are copied, the used count as the
 * slots copied, the new table is linked into the state's word, and the old
 * one is closed; it gathers the old table's retirement, its slot pages and
 * its lists' own pages, into a live Retiring, to run once the lock is
 * released. It takes the try.
 */
static bool
txn_memtable_tuples_grown(HostSharedGuard *guard, const HostDeltaHold *hold, HostSharedRecord state, uint64 location,
						  TxnMemtableTuplesTaken took, TxnMemtableTuplesRetiring *result, HostError *error)
{
	txn_memtable_tuples_grown_frame_0_t frame = {.guard = guard, .hold = hold, .state = state, .location = location,
	.took = took, .result = result, .error = error, .made_live = took.has_made, .slots_live = true};

	std_landing(&frame, txn_memtable_tuples_grown_body_0, txn_memtable_tuples_grown_pad_0);
	return frame.ok;
}

/* txn_memtable_tuples_reserve_frame_0_t is reserve's state across its pad. */
typedef struct txn_memtable_tuples_reserve_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	const uint8 *key;
	size_t		key_len;
	TxnMemtableTableReservation *result;

	/*
	 * refused, for a carry's reserve, answers whether the delta's budget
	 * refused it, with its refusal.
	 */
	TxnMemtableTableRefusal *refused;
	HostError  *error;
	bool		ok;
	bool		has_want;
	TxnMemtableTuplesWant want;
	/* sizes is a Vec<Size> of a try's pages. */
	StdVec		sizes;
	bool		sizes_live;
	TxnMemtableTuplesTaken took;
	bool		took_live;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedGuard guard;
	bool		guard_live;
	TxnMemtableTuplesRetiring leaving;
	bool		leaving_live;
	/* retired is a Vec<u64> of the byte pages a compaction retired. */
	StdVec		retired;
	bool		retired_live;
} txn_memtable_tuples_reserve_frame_0_t;

/* txn_memtable_tuples_reserve_pad_0 is reserve's pad: the guard, the retirement, the retired byte pages, the hold, the try, and its sizes, while live. */
static void
txn_memtable_tuples_reserve_pad_0(void *frame_)
{
	txn_memtable_tuples_reserve_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	if (frame->leaving_live)
	{
		frame->leaving_live = false;
		std_vec_free_Checked(&frame->leaving.names, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->retired_live)
	{
		frame->retired_live = false;
		std_vec_free_Checked(&frame->retired, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	if (frame->took_live)
	{
		frame->took_live = false;
		txn_memtable_tuples_Taken_drop(&frame->took);
	}
	if (frame->sizes_live)
	{
		frame->sizes_live = false;
		std_vec_free_Checked(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	}
}

/*
 * txn_memtable_tuples_taken takes a want's pages and record into the frame's
 * live try, or answers ok false with the delta's refusal after giving back
 * what it took.
 */
static bool
txn_memtable_tuples_taken(txn_memtable_tuples_reserve_frame_0_t *frame, bool *ok, HostDeltaFull *full)
{
	bool		allocated;

	std_vec_new(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	frame->sizes_live = true;
	if (!txn_memtable_tuples_Want_sizes(frame->want, &frame->sizes, frame->error))
		return false;
	allocated = host_delta_alloc(frame->io, frame->sizes.ptr, frame->sizes.len, ok, &frame->took.pages, full,
								 frame->error);
	frame->sizes_live = false;
	std_vec_free_Checked(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	if (!allocated || !*ok)
		return allocated;
	frame->took.want = frame->want;
	frame->took.has_made = false;
	frame->took_live = true;
	if (frame->want.tag == TXN_MEMTABLE_TUPLES_WANT_BYTE)
		return true;
	if (!host_delta_make(frame->io, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS, ok, &frame->took.made, full, frame->error))
		return false;
	if (!*ok)
	{
		frame->took_live = false;
		txn_memtable_tuples_Taken_drop(&frame->took);
		return true;
	}
	frame->took.has_made = true;
	return true;
}

/*
 * TxnMemtableTuplesLive is a live slot a compaction stores again: its tid,
 * its place, its slot's place word, and the words of the slot it moves to.
 */
typedef struct TxnMemtableTuplesLive
{
	uint64		tid;
	uint64		place;
	HostDeltaWord place_word;
	HostDeltaWord to_tid;
	HostDeltaWord to_place;
} TxnMemtableTuplesLive;

/* txn_memtable_tuples_place_cmp orders live slots by their places, which are distinct. */
static int
txn_memtable_tuples_place_cmp(const void *a, const void *b)
{
	uint64		x = ((const TxnMemtableTuplesLive *) a)->place;
	uint64		y = ((const TxnMemtableTuplesLive *) b)->place;

	return x < y ? -1 : x > y;
}

/*
 * txn_memtable_tuples_moved moves the live keys of a byte page but the open
 * one to the open page when they fit its room, storing each slot's new place
 * as soon as its key is appended, marks the page DROPPED in the list of
 * byte pages, and adds it to the retired pages, which have room for it; live
 * holds the page's slots, from its first to the one before end. A failure
 * part way leaves every slot naming a place that holds its key.
 */
static bool
txn_memtable_tuples_moved(HostSharedGuard *guard, const HostDeltaHold *hold, uint64 index, uint64 newest,
						  const HostDeltaPageRef *open, TxnMemtableTuplesLive *live, size_t len, StdVec *retired,
						  HostError *error)
{
	uint64		mask = (UINT64CONST(1) << TXN_MEMTABLE_TUPLES_LENGTH) - 1;
	uint64		bytes = 0;
	uint64		name;
	HostDeltaPageRef page;
	HostDeltaListListMut keys;

	keys = host_delta_list_GuardExclusive_list_mut(guard, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape,
												   TXN_MEMTABLE_TUPLES_LOCK, TXN_MEMTABLE_TUPLES_KEYS);
	{
		HostDeltaListListView view = host_delta_list_ListMut_view(&keys);

		if (!host_delta_list_ListView_get(&view, hold, index, &name, error))
			return false;
	}
	for (size_t k = 0; k < len; k++)
		bytes += live[k].place & mask;
	if (name == TXN_MEMTABLE_TUPLES_DROPPED || bytes > host_delta_PageRef_room(open))
		return true;
	if (!host_delta_Hold_page(hold, host_delta_page_Page_named(name), &page, error))
		return false;
	for (size_t k = 0; k < len; k++)
	{
		uint64		length = live[k].place & mask;
		uint64		offset = live[k].place >> TXN_MEMTABLE_TUPLES_LENGTH & ((UINT64CONST(1) << TXN_MEMTABLE_TUPLES_OFFSET) - 1);
		HostDeltaLent key;
		StdBytes	held;
		uint32		at;
		bool		ok;

		if (!host_delta_PageRef_bytes(&page, offset, length, &key, error))
			return false;
		held = host_delta_Lent_Deref_deref(&key);
		ok = host_delta_PageRef_append(open, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape, TXN_MEMTABLE_TUPLES_LOCK, guard,
									   held.ptr, held.len, &at, error);
		host_delta_Lent_drop(&key);
		if (!ok)
			return false;
		live[k].place = txn_memtable_tuples_placed(newest, at, (size_t) length);
		host_delta_Word_store(&live[k].place_word, live[k].place);
	}
	if (!host_delta_list_ListMut_set(&keys, hold, index, TXN_MEMTABLE_TUPLES_DROPPED, error))
		return false;
	std_vec_push_Checked(retired, &name, sizeof(name), _Alignof(uint64));
	return true;
}

/*
 * txn_memtable_tuples_compacted_frame_t is compacted's state across its pad:
 * the slot pages, the live slots, and the tid words of every used slot, each
 * while live.
 */
typedef struct txn_memtable_tuples_compacted_frame_t
{
	HostSharedGuard *guard;
	const HostDeltaHold *hold;
	StdVec	   *retired;
	HostError  *error;
	bool		ok;
	StdVec		pages;
	bool		pages_live;
	StdVec		live;
	bool		live_live;
	StdVec		cleared;
	bool		cleared_live;
	StdVec		taken;
	bool		taken_live;
} txn_memtable_tuples_compacted_frame_t;

/* txn_memtable_tuples_compacted_reserved reserves room for more elements in one of compacted's vectors, answering false on running out of memory. */
static bool
txn_memtable_tuples_compacted_reserved(txn_memtable_tuples_compacted_frame_t *frame, StdVec *vec, size_t more,
									   size_t size, size_t align)
{
	if (std_vec_try_reserve(vec, more, size, align))
		return true;
	*frame->error = host_Error_out_of_memory();
	return false;
}

/*
 * txn_memtable_tuples_compacted_body reads every slot page and gathers the
 * live slots and every used slot's tid word, moves the byte pages' keys,
 * plans each live slot's new slot and takes its words, and only then zeroes
 * the used slots and stores the live ones again, so every step that can
 * fail runs before the first slot is zeroed.
 */
static void
txn_memtable_tuples_compacted_body(void *frame_)
{
	txn_memtable_tuples_compacted_frame_t *frame = frame_;
	HostSharedGuard *guard = frame->guard;
	const HostDeltaHold *hold = frame->hold;
	HostSharedRecord record = host_shared_Guard_record(guard);
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	uint64		count = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_COUNT, load);
	uint64		held = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_USED, load) -
		host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_DEAD_COUNT, load);
	uint64		reserved = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_RESERVED, load);
	HostDeltaListListView slots = txn_memtable_tuples_view(guard, TXN_MEMTABLE_TUPLES_SLOTS);
	uint64		len = host_delta_list_ListView_len(&slots);
	HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};
	TxnMemtableTuplesLive *live;

	std_vec_new(&frame->pages, sizeof(HostDeltaPageRef), _Alignof(HostDeltaPageRef));
	frame->pages_live = true;
	std_vec_new(&frame->live, sizeof(TxnMemtableTuplesLive), _Alignof(TxnMemtableTuplesLive));
	frame->live_live = true;
	std_vec_new(&frame->cleared, sizeof(HostDeltaWord), _Alignof(HostDeltaWord));
	frame->cleared_live = true;
	if (!txn_memtable_tuples_compacted_reserved(frame, &frame->pages, (size_t) len, sizeof(HostDeltaPageRef),
												_Alignof(HostDeltaPageRef)) ||
		!txn_memtable_tuples_compacted_reserved(frame, &frame->live, (size_t) held, sizeof(TxnMemtableTuplesLive),
												_Alignof(TxnMemtableTuplesLive)))
		return;
	for (uint64 index = 0; index < len; index++)
	{
		uint64		name;
		HostDeltaPageRef page;

		if (!host_delta_list_ListView_get(&slots, hold, index, &name, frame->error) ||
			!host_delta_Hold_page(hold, host_delta_page_Page_named(name), &page, frame->error))
			return;
		std_vec_push_Checked(&frame->pages, &page, sizeof(page), _Alignof(HostDeltaPageRef));
		for (uint64 at = 0; at < Min(count, TXN_MEMTABLE_TUPLES_PER); at++)
		{
			HostDeltaWord word;
			TxnMemtableTuplesLive slot = {0};

			if (!host_delta_PageRef_word(&page, 2 * at, &word, frame->error))
				return;
			slot.tid = host_delta_Word_load(&word);
			if (slot.tid == 0)
				continue;
			if (!txn_memtable_tuples_compacted_reserved(frame, &frame->cleared, 1, sizeof(HostDeltaWord),
														_Alignof(HostDeltaWord)))
				return;
			std_vec_push_Checked(&frame->cleared, &word, sizeof(word), _Alignof(HostDeltaWord));
			if (slot.tid == TXN_MEMTABLE_TUPLES_DEAD)
				continue;
			if (!host_delta_PageRef_word(&page, 2 * at + 1, &slot.place_word, frame->error) ||
				!txn_memtable_tuples_compacted_reserved(frame, &frame->live, 1, sizeof(TxnMemtableTuplesLive),
														_Alignof(TxnMemtableTuplesLive)))
				return;
			slot.place = host_delta_Word_load(&slot.place_word);
			std_vec_push_Checked(&frame->live, &slot, sizeof(slot), _Alignof(TxnMemtableTuplesLive));
		}
	}
	live = frame->live.ptr;
	if (reserved == 0 && frame->live.len > 0)
	{
		HostDeltaListListView keys = txn_memtable_tuples_view(guard, TXN_MEMTABLE_TUPLES_KEYS);
		uint64		newest = host_delta_list_ListView_len(&keys) - 1;
		size_t		at = 0;
		uint64		name;
		HostDeltaPageRef open;

		qsort(live, frame->live.len, sizeof(TxnMemtableTuplesLive), txn_memtable_tuples_place_cmp);
		if (!host_delta_list_ListView_get(&keys, hold, newest, &name, frame->error) ||
			!host_delta_Hold_page(hold, host_delta_page_Page_named(name), &open, frame->error) ||
			!txn_memtable_tuples_compacted_reserved(frame, frame->retired, (size_t) newest, sizeof(uint64),
													_Alignof(uint64)))
			return;
		for (uint64 index = 0; index < newest; index++)
		{
			size_t		from = at;

			while (at < frame->live.len &&
				   live[at].place >> (TXN_MEMTABLE_TUPLES_OFFSET + TXN_MEMTABLE_TUPLES_LENGTH) == index)
				at++;
			if (!txn_memtable_tuples_moved(guard, hold, index, newest, &open, &live[from], at - from, frame->retired,
										   frame->error))
				return;
		}
	}
	std_vec_new(&frame->taken, sizeof(bool), _Alignof(bool));
	frame->taken_live = true;
	if (!txn_memtable_tuples_compacted_reserved(frame, &frame->taken, (size_t) count, sizeof(bool), _Alignof(bool)))
		return;
	memset(frame->taken.ptr, 0, (size_t) count);
	frame->taken.len = (size_t) count;
	for (size_t k = 0; k < frame->live.len; k++)
	{
		uint64		to = txn_memtable_tuples_slot(live[k].tid - 1, count);
		bool	   *taken = frame->taken.ptr;
		const HostDeltaPageRef *page;

		while (taken[to])
			to = (to + 1) % count;
		taken[to] = true;
		page = &STD_VEC_AT(&frame->pages, HostDeltaPageRef, (size_t) (to / TXN_MEMTABLE_TUPLES_PER));
		if (!txn_memtable_tuples_word(page, to, false, 0, 0, &live[k].to_tid, frame->error) ||
			!txn_memtable_tuples_word(page, to, true, 0, 0, &live[k].to_place, frame->error))
			return;
	}
	for (size_t k = 0; k < frame->cleared.len; k++)
		host_delta_Word_store(&STD_VEC_AT(&frame->cleared, HostDeltaWord, k), 0);
	for (size_t k = 0; k < frame->live.len; k++)
	{
		host_delta_Word_store(&live[k].to_place, live[k].place);
		host_delta_Word_store(&live[k].to_tid, live[k].tid);
	}
	exchange.u.exchange.f0 = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_USED, load);
	exchange.u.exchange.f1 = frame->live.len;
	host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_USED, exchange);
	exchange.u.exchange.f0 = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_DEAD_COUNT, load);
	exchange.u.exchange.f1 = 0;
	host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_DEAD_COUNT, exchange);
	frame->ok = host_pause(HOST_SHARED_GUARD_PAUSES(guard), "doltgres-tuples-compacting", frame->error);
}

/* txn_memtable_tuples_compacted_pad frees what compacted holds live. */
static void
txn_memtable_tuples_compacted_pad(void *frame_)
{
	txn_memtable_tuples_compacted_frame_t *frame = frame_;

	if (frame->taken_live)
		std_vec_free_Checked(&frame->taken, sizeof(bool), _Alignof(bool));
	if (frame->cleared_live)
		std_vec_free_Checked(&frame->cleared, sizeof(HostDeltaWord), _Alignof(HostDeltaWord));
	if (frame->live_live)
		std_vec_free_Checked(&frame->live, sizeof(TxnMemtableTuplesLive), _Alignof(TxnMemtableTuplesLive));
	if (frame->pages_live)
		std_vec_free_Checked(&frame->pages, sizeof(HostDeltaPageRef), _Alignof(HostDeltaPageRef));
	frame->taken_live = frame->cleared_live = frame->live_live = frame->pages_live = false;
}

/*
 * txn_memtable_tuples_compacted compacts an open table in place under its
 * exclusive lock, once its dropped slots leave its live ones at most half
 * its load factor, allocating nothing from the delta: its live slots,
 * gathered with fallible allocation, are stored again in its own slot pages,
 * the dropped ones left behind and its dropped count reset; and, while no
 * reservation holds a place on a byte page, each byte page but the open one
 * whose live keys fit the open page's room, none included, has them moved
 * there with their places and is retired, its entry in the list of byte
 * pages marked DROPPED, so no other page's index moves. It adds the retired
 * pages to retired, to retire once the lock is released. Every step that can
 * fail runs before the first slot is zeroed, and each key's move stores its
 * new place at once, so an error or a panic leaves every tid found.
 */
static bool
txn_memtable_tuples_compacted(HostSharedGuard *guard, const HostDeltaHold *hold, StdVec *retired, HostError *error)
{
	txn_memtable_tuples_compacted_frame_t frame = {.guard = guard, .hold = hold, .retired = retired, .error = error};

	std_landing(&frame, txn_memtable_tuples_compacted_body, txn_memtable_tuples_compacted_pad);
	txn_memtable_tuples_compacted_pad(&frame);
	return frame.ok;
}

/*
 * txn_memtable_tuples_reserve_open is a try under an open table's exclusive
 * lock: it appends the key to the newest byte page when it fits and the table
 * isn't full, answering done, and otherwise grows the table or pushes a byte
 * page with the try that wanted it, answering done after a push, or sets the
 * want for the next try.
 */
static bool
txn_memtable_tuples_reserve_open(txn_memtable_tuples_reserve_frame_0_t *frame, TxnMemtableIndexOpen *open,
								 uint64 location, bool *done)
{
	HostSharedRecord record = host_shared_Guard_record(&frame->guard);
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostSharedOp add = {.tag = HOST_SHARED_OP_ADD, .u.add = 1};
	uint64		count = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_COUNT, load);
	HostDeltaListListView keys = txn_memtable_tuples_view(&frame->guard, TXN_MEMTABLE_TUPLES_KEYS);
	uint64		newest = STD_SUB(u64, host_delta_list_ListView_len(&keys), 1,
								 278, 26);
	uint64		name;
	HostDeltaPageRef page;
	HostDeltaPageSize byte;
	TxnMemtableTuplesWant wanted = {0};
	uint64		used;
	uint64		reserved;
	uint64		dead;

	*done = false;
	if (!host_delta_list_ListView_get(&keys, &frame->hold, newest, &name, frame->error) ||
		!host_delta_Hold_page(&frame->hold, host_delta_page_Page_named(name), &page, frame->error))
		return false;
	byte = txn_memtable_tuples_bytes(frame->key_len, true,
									 STD_ADD(u64, host_delta_PageRef_filled(&page), host_delta_PageRef_room(&page),
											 280, 40));
	used = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_USED, load);
	reserved = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_RESERVED, load);
	dead = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_DEAD_COUNT, load);
	if (txn_memtable_tuples_full(used, reserved, count) && dead > 0 &&
		2 * (used - dead + reserved + 1) <= count / 4 * 3)
	{
		std_vec_new(&frame->retired, sizeof(uint64), _Alignof(uint64));
		frame->retired_live = true;
		return txn_memtable_tuples_compacted(&frame->guard, &frame->hold, &frame->retired, frame->error);
	}
	if (txn_memtable_tuples_full(used, reserved, count))
	{
		wanted.tag = TXN_MEMTABLE_TUPLES_WANT_GROW;
		wanted.count = STD_MUL(u64, 2, count, 282, 37);
		wanted.keys = STD_ADD(u64, newest, 2, 282, 54);
	}
	else
	{
		if ((uint64) frame->key_len <= host_delta_PageRef_room(&page))
		{
			uint32		offset;

			if (!host_delta_PageRef_append(&page, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS.shape, TXN_MEMTABLE_TUPLES_LOCK,
										   open->guard, frame->key, frame->key_len, &offset, frame->error))
				return false;
			host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_RESERVED, add);
			frame->result->place = txn_memtable_tuples_placed(newest, offset, frame->key_len);
			*done = true;
			return true;
		}
		wanted.tag = TXN_MEMTABLE_TUPLES_WANT_BYTE;
		wanted.spares = host_delta_list_ListView_wants(&keys);
	}
	wanted.byte = byte;
	if (!frame->took_live || !txn_memtable_tuples_Want_PartialEq_eq(&frame->took.want, &wanted))
	{
		frame->has_want = true;
		frame->want = wanted;
		return true;
	}
	frame->took_live = false;
	if (wanted.tag == TXN_MEMTABLE_TUPLES_WANT_GROW)
	{
		std_vec_new(&frame->leaving.names, sizeof(uint64), _Alignof(uint64));
		frame->leaving_live = true;
		return txn_memtable_tuples_grown(&frame->guard, &frame->hold, frame->state, location, frame->took,
										 &frame->leaving, frame->error);
	}
	*done = true;
	return txn_memtable_tuples_pushed(open, &frame->hold, frame->took, frame->key, frame->key_len, frame->result,
									  frame->error);
}

/* txn_memtable_tuples_reserve_body_0 is reserve's body. */
static void
txn_memtable_tuples_reserve_body_0(void *frame_)
{
	txn_memtable_tuples_reserve_frame_0_t *frame = frame_;

	for (;;)
	{
		HostSharedPast word;
		uint64		location;
		HostSharedRecord record;
		TxnMemtableIndexOpen open;
		bool		done;
		bool		ok;

		if (frame->has_want)
		{
			HostDeltaFull full;

			if (!txn_memtable_tuples_taken(frame, &ok, &full))
			{
				txn_memtable_tuples_reserve_pad_0(frame);
				return;
			}
			if (!ok)
			{
				bool		answered;

				if (!txn_memtable_table_refused(frame->io, full, frame->refused, &answered, frame->error))
				{
					txn_memtable_tuples_reserve_pad_0(frame);
					return;
				}
				if (answered)
				{
					frame->ok = true;
					txn_memtable_tuples_reserve_pad_0(frame);
					return;
				}
				continue;
			}
		}
		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		{
			txn_memtable_tuples_reserve_pad_0(frame);
			return;
		}
		frame->hold_live = true;
		word = txn_memtable_tuples_word_of(frame->state);
		if (!host_shared_Past_load(&word, &location))
		{
			if (frame->took_live && frame->took.want.tag == TXN_MEMTABLE_TUPLES_WANT_FIRST)
			{
				frame->took_live = false;
				if (!txn_memtable_tuples_first(frame->io, frame->state, frame->took, frame->error))
				{
					txn_memtable_tuples_reserve_pad_0(frame);
					return;
				}
			}
			else
			{
				TxnMemtableTuplesWant first = {.tag = TXN_MEMTABLE_TUPLES_WANT_FIRST,
				.byte = txn_memtable_tuples_bytes(frame->key_len, false, 0)};

				frame->has_want = true;
				frame->want = first;
			}
			txn_memtable_tuples_reserve_pad_0(frame);
			continue;
		}
		if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS, location, &record,
									frame->error) ||
			!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_TUPLES_LOCK,
									 &frame->guard, frame->error))
		{
			txn_memtable_tuples_reserve_pad_0(frame);
			return;
		}
		frame->guard_live = true;
		if (!txn_memtable_index_open(&TXN_MEMTABLE_TUPLES_CLOSES_OPS, &frame->guard, &open))
		{
			txn_memtable_tuples_reserve_pad_0(frame);
			continue;
		}
		ok = txn_memtable_tuples_reserve_open(frame, &open, location, &done);
		if (!ok || done)
		{
			frame->ok = ok;
			txn_memtable_tuples_reserve_pad_0(frame);
			return;
		}
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
		if (frame->leaving_live && !txn_memtable_tuples_Retiring_retire(&frame->leaving, frame->io, frame->error))
		{
			txn_memtable_tuples_reserve_pad_0(frame);
			return;
		}
		if (frame->retired_live)
		{
			TxnMemtableTableRetiring names = {frame->retired.ptr, frame->retired.len, 0};
			StdIterator pages = {&TXN_MEMTABLE_TABLE_RETIRING_ITERATOR_OPS, &names};

			if (!host_delta_retire(frame->io, NULL, 0, pages, frame->error))
			{
				txn_memtable_tuples_reserve_pad_0(frame);
				return;
			}
		}
		txn_memtable_tuples_reserve_pad_0(frame);
	}
}

/*
 * txn_memtable_tuples_reserve reserves a slot of a storage's table for a key
 * before a window and appends the key's bytes to the table's newest byte
 * page, so that the finish only stores the slot. Each try takes what the
 * last one wanted before its hold, then, under the table's exclusive lock:
 * makes the storage's first table as its first maker when there is none,
 * compacts the table in place when one more slot would pass its load factor
 * while its dropped slots leave its live ones at most half of it, else grows
 * it first, puts a fresh byte page on the table's list when the key doesn't
 * fit the newest, and counts the slot reserved. A take the delta refuses is
 * met as refused says (table's refused).
 */
bool
txn_memtable_tuples_reserve(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len,
							TxnMemtableTableReservation *result, TxnMemtableTableRefusal *refused, HostError *error)
{
	txn_memtable_tuples_reserve_frame_0_t frame = {.io = io, .state = state, .key = key, .key_len = key_len,
	.result = result, .refused = refused, .error = error};

	if (refused != NULL)
		refused->refused = false;
	std_landing(&frame, txn_memtable_tuples_reserve_body_0, txn_memtable_tuples_reserve_pad_0);
	return frame.ok;
}

/* txn_memtable_tuples_held_frame_0_t is the state across the pad of dropped, given, get, and retired_when: a hold and a guard. */
typedef struct txn_memtable_tuples_held_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedGuard guard;
	bool		guard_live;
	const uint64 *tids;
	size_t		tids_len;
	HostRow		tid;
	bool	   *some;
	StdVec	   *key;
	HostDeltaLent lent;
	bool		lent_live;
	bool		key_live;
	bool		emptied;
	TxnMemtableTuplesRetiring retiring;
	bool		retiring_live;
} txn_memtable_tuples_held_frame_0_t;

/* txn_memtable_tuples_held_pad_0 is the pad of dropped, given, get, and retired_when: the key, the bytes, the guard, the retirement, and the hold, while live. */
static void
txn_memtable_tuples_held_pad_0(void *frame_)
{
	txn_memtable_tuples_held_frame_0_t *frame = frame_;

	if (frame->key_live)
	{
		frame->key_live = false;
		std_vec_free_Checked(frame->key, 1, 1);
	}
	if (frame->lent_live)
	{
		frame->lent_live = false;
		host_delta_Lent_drop(&frame->lent);
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	if (frame->retiring_live)
	{
		frame->retiring_live = false;
		std_vec_free_Checked(&frame->retiring.names, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
}

/*
 * txn_memtable_tuples_locked takes the exclusive lock of the table the
 * state's word names now, under the frame's hold, answering some false when
 * the word names none, and whether the table is open.
 */
static bool
txn_memtable_tuples_locked(txn_memtable_tuples_held_frame_0_t *frame, bool *some, uint64 *location,
						   TxnMemtableIndexOpen *open, bool *opened)
{
	HostSharedPast word = txn_memtable_tuples_word_of(frame->state);
	HostSharedRecord record;

	*some = host_shared_Past_load(&word, location);
	if (!*some)
		return true;
	if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS, *location, &record,
								frame->error) ||
		!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_TUPLES_LOCK, &frame->guard,
								 frame->error))
		return false;
	frame->guard_live = true;
	*opened = txn_memtable_index_open(&TXN_MEMTABLE_TUPLES_CLOSES_OPS, &frame->guard, open);
	return true;
}

/* txn_memtable_tuples_dropped_one drops one tid's slot from an open table, if it holds it. */
static bool
txn_memtable_tuples_dropped_one(txn_memtable_tuples_held_frame_0_t *frame, const HostDeltaListListView *slots,
								uint64 count, uint64 tid)
{
	uint64		at = txn_memtable_tuples_slot(tid, count);

	for (;;)
	{
		HostDeltaPageRef page;
		HostDeltaWord word;
		uint64		held;

		if (!txn_memtable_tuples_slots_page(slots, &frame->hold, at, &page, frame->error) ||
			!txn_memtable_tuples_word(&page, at, false, 438, 86, &word, frame->error))
			return false;
		held = host_delta_Word_load(&word);
		if (held == 0)
			return true;
		if (held == STD_ADD(u64, tid, 1, 441, 37))
		{
			HostSharedOp add = {.tag = HOST_SHARED_OP_ADD, .u.add = 1};

			host_delta_Word_store(&word, TXN_MEMTABLE_TUPLES_DEAD);
			host_shared_Record_atomic(host_shared_Guard_record(&frame->guard), TXN_MEMTABLE_TUPLES_DEAD_COUNT, add);
			return true;
		}
		at = STD_REM(u64, STD_ADD(u64, at, 1, 445, 32), count, 445, 31);
	}
}

/* txn_memtable_tuples_dropped_body_0 is dropped's body. */
static void
txn_memtable_tuples_dropped_body_0(void *frame_)
{
	txn_memtable_tuples_held_frame_0_t *frame = frame_;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	for (;;)
	{
		bool		some;
		uint64		location;
		TxnMemtableIndexOpen open;
		bool		opened;
		HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
		uint64		count;
		HostDeltaListListView slots;

		if (!txn_memtable_tuples_locked(frame, &some, &location, &open, &opened))
			break;
		if (!some)
		{
			frame->ok = true;
			break;
		}
		if (!opened)
		{
			frame->guard_live = false;
			host_shared_Guard_drop(&frame->guard);
			continue;
		}
		count = host_shared_Record_atomic(host_shared_Guard_record(&frame->guard), TXN_MEMTABLE_TUPLES_COUNT, load);
		slots = txn_memtable_tuples_view(&frame->guard, TXN_MEMTABLE_TUPLES_SLOTS);
		frame->ok = true;
		for (size_t k = 0; k < frame->tids_len && frame->ok; k++)
			frame->ok = txn_memtable_tuples_dropped_one(frame, &slots, count, frame->tids[k]);
		break;
	}
	txn_memtable_tuples_held_pad_0(frame);
}

/*
 * txn_memtable_tuples_dropped drops the slots of the tids given from a
 * storage's table in place, under the exclusive lock of the table the state's
 * word names now, so that a lookup of them goes on to the spilled tuple ids,
 * which name them once a spill wrote them; it allocates nothing, so a spill
 * at a full budget never waits for it. The slots stay counted until a growth
 * leaves them behind, and their keys' bytes stay on their pages until the
 * table's retirement.
 */
bool
txn_memtable_tuples_dropped(HostIoIo *io, HostSharedRecord state, const uint64 *tids, size_t tids_len,
							HostError *error)
{
	txn_memtable_tuples_held_frame_0_t frame = {.io = io, .state = state, .error = error, .tids = tids,
	.tids_len = tids_len};

	std_landing(&frame, txn_memtable_tuples_dropped_body_0, txn_memtable_tuples_held_pad_0);
	return frame.ok;
}

/* txn_memtable_tuples_finish_frame_0_t is finish's state across its pad: the table's guard. */
typedef struct txn_memtable_tuples_finish_frame_0_t
{
	HostIoTakesShort io;
	TxnMemtableTablePreparedState *prepared;
	HostRow		tid;
	HostError  *error;
	bool		ok;
	HostSharedGuard guard;
	bool		guard_live;
} txn_memtable_tuples_finish_frame_0_t;

/* txn_memtable_tuples_finish_pad_0 is finish's pad: the guard, while live. */
static void
txn_memtable_tuples_finish_pad_0(void *frame_)
{
	txn_memtable_tuples_finish_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
}

/* txn_memtable_tuples_finish_stored probes an open table from the tid's slot and stores the reservation in the first free slot or the tid's own. */
static bool
txn_memtable_tuples_finish_stored(txn_memtable_tuples_finish_frame_0_t *frame, const HostDeltaHold *hold,
								  TxnMemtableTableReservation reservation)
{
	HostSharedRecord record = host_shared_Guard_record(&frame->guard);
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	uint64		count = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_COUNT, load);
	HostDeltaListListView slots = txn_memtable_tuples_view(&frame->guard, TXN_MEMTABLE_TUPLES_SLOTS);
	uint64		at = txn_memtable_tuples_slot(frame->tid.f0, count);

	for (;;)
	{
		HostDeltaPageRef page;
		HostDeltaWord word;
		HostDeltaWord place;
		uint64		held;

		if (!txn_memtable_tuples_slots_page(&slots, hold, at, &page, frame->error) ||
			!txn_memtable_tuples_word(&page, at, false, 507, 34, &word, frame->error))
			return false;
		held = host_delta_Word_load(&word);
		if (held == 0 || held == STD_ADD(u64, frame->tid.f0, 1, 509, 37))
		{
			HostSharedOp less = {.tag = HOST_SHARED_OP_ADD, .u.add = PG_UINT64_MAX};
			HostSharedOp more = {.tag = HOST_SHARED_OP_ADD, .u.add = 1};

			if (!txn_memtable_tuples_word(&page, at, true, 510, 27, &place, frame->error))
				return false;
			host_delta_Word_store(&place, reservation.place);
			host_delta_Word_store(&word, STD_ADD(u64, frame->tid.f0, 1, 511, 28));
			host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_RESERVED, less);
			if (held == 0)
				host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_USED, more);
			return true;
		}
		at = STD_REM(u64, STD_ADD(u64, at, 1, 518, 19), count, 518, 18);
	}
}

/* txn_memtable_tuples_finish_body_0 is finish's body. */
static void
txn_memtable_tuples_finish_body_0(void *frame_)
{
	txn_memtable_tuples_finish_frame_0_t *frame = frame_;
	const HostDeltaHold *hold;
	HostSharedRecord state;
	TxnMemtableTableReservation reservation;

	if (!txn_memtable_table_Prepared_tuple(frame->prepared, &hold, &state, &reservation, frame->error))
		return;
	for (;;)
	{
		HostSharedPast word = txn_memtable_tuples_word_of(state);
		uint64		location;
		HostSharedRecord record;
		TxnMemtableIndexOpen open;

		if (!host_shared_Past_load(&word, &location))
		{
			*frame->error = txn_memtable_tuples_internal("a reserved table is gone");
			return;
		}
		if (!host_delta_Hold_record(hold, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS, location, &record, frame->error) ||
			!host_shared_Record_lock(record, frame->io, TXN_MEMTABLE_TUPLES_LOCK, &frame->guard, frame->error))
			return;
		frame->guard_live = true;
		if (txn_memtable_index_open(&TXN_MEMTABLE_TUPLES_CLOSES_OPS, &frame->guard, &open))
		{
			frame->ok = txn_memtable_tuples_finish_stored(frame, hold, reservation);
			txn_memtable_tuples_finish_pad_0(frame);
			return;
		}
		txn_memtable_tuples_finish_pad_0(frame);
	}
}

/*
 * txn_memtable_tuples_finish stores a prepared reservation's slot inside the
 * window, allocating nothing: under the exclusive lock of the storage's
 * table, the one the state's word names now, it probes from the tid's slot to
 * the first free slot or the tid's own, a tid used again, stores the key's
 * place and the tid, and moves the slot from reserved to used. It fails only
 * on corruption.
 */
bool
txn_memtable_tuples_finish(HostIoTakesShort io, TxnMemtableTablePreparedState *prepared, HostRow tid,
						   HostError *error)
{
	txn_memtable_tuples_finish_frame_0_t frame = {.io = io, .prepared = prepared, .tid = tid, .error = error};

	std_landing(&frame, txn_memtable_tuples_finish_body_0, txn_memtable_tuples_finish_pad_0);
	return frame.ok;
}

/* txn_memtable_tuples_given_body_0 is given's body. */
static void
txn_memtable_tuples_given_body_0(void *frame_)
{
	txn_memtable_tuples_held_frame_0_t *frame = frame_;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	for (;;)
	{
		bool		some;
		uint64		location;
		TxnMemtableIndexOpen open;
		bool		opened;

		if (!txn_memtable_tuples_locked(frame, &some, &location, &open, &opened))
			break;
		if (!some)
		{
			*frame->error = txn_memtable_tuples_internal("a reserved table is gone");
			break;
		}
		if (opened)
		{
			HostSharedOp less = {.tag = HOST_SHARED_OP_ADD, .u.add = PG_UINT64_MAX};

			host_shared_Record_atomic(host_shared_Guard_record(&frame->guard), TXN_MEMTABLE_TUPLES_RESERVED, less);
			frame->ok = true;
			break;
		}
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	txn_memtable_tuples_held_pad_0(frame);
}

/*
 * txn_memtable_tuples_given gives back a reservation its finish didn't store,
 * under the exclusive lock of the table the state's word names now; its key's
 * bytes stay on their page until a compaction.
 */
bool
txn_memtable_tuples_given(HostIoIo *io, HostSharedRecord state, HostError *error)
{
	txn_memtable_tuples_held_frame_0_t frame = {.io = io, .state = state, .error = error};

	std_landing(&frame, txn_memtable_tuples_given_body_0, txn_memtable_tuples_held_pad_0);
	return frame.ok;
}

/* txn_memtable_tuples_get_found copies the key at a slot's place on the table's byte pages, under the table's guard. */
static bool
txn_memtable_tuples_get_found(txn_memtable_tuples_held_frame_0_t *frame, uint64 place)
{
	HostDeltaListListView keys = txn_memtable_tuples_view(&frame->guard, TXN_MEMTABLE_TUPLES_KEYS);
	uint64		name;
	HostDeltaPageRef page;
	StdBytes	bytes;

	if (!host_delta_list_ListView_get(&keys, &frame->hold,
									  STD_SHR_FOLDED(u64, place, TXN_MEMTABLE_TUPLES_OFFSET + TXN_MEMTABLE_TUPLES_LENGTH),
									  &name, frame->error) ||
		!host_delta_Hold_page(&frame->hold, host_delta_page_Page_named(name), &page, frame->error) ||
		!host_delta_PageRef_bytes(&page,
								  STD_SHR_FOLDED(u64, place, TXN_MEMTABLE_TUPLES_LENGTH) &
								  ((UINT64CONST(1) << TXN_MEMTABLE_TUPLES_OFFSET) - 1),
								  place & ((UINT64CONST(1) << TXN_MEMTABLE_TUPLES_LENGTH) - 1), &frame->lent,
								  frame->error))
		return false;
	frame->lent_live = true;
	bytes = host_delta_Lent_Deref_deref(&frame->lent);
	std_vec_new(frame->key, 1, 1);
	frame->key_live = true;
	std_vec_extend_from_slice_Checked(frame->key, bytes.ptr, bytes.len, 1, 1);
	frame->key_live = false;
	*frame->some = true;
	frame->lent_live = false;
	host_delta_Lent_drop(&frame->lent);
	return true;
}

/*
 * txn_memtable_tuples_get_open probes an open table under its shared lock
 * from the tid's slot, copying its key when found, and answering whether it
 * found the tid's slot or a free one.
 */
static bool
txn_memtable_tuples_get_open(txn_memtable_tuples_held_frame_0_t *frame, bool *found)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	uint64		count = host_shared_Record_atomic(host_shared_Guard_record(&frame->guard), TXN_MEMTABLE_TUPLES_COUNT,
												  load);
	HostDeltaListListView slots = txn_memtable_tuples_view(&frame->guard, TXN_MEMTABLE_TUPLES_SLOTS);
	uint64		at = txn_memtable_tuples_slot(frame->tid.f0, count);

	for (;;)
	{
		HostDeltaPageRef page;
		HostDeltaWord word;
		uint64		held;

		if (!txn_memtable_tuples_slots_page(&slots, &frame->hold, at, &page, frame->error) ||
			!txn_memtable_tuples_word(&page, at, false, 557, 29, &word, frame->error))
			return false;
		held = host_delta_Word_load(&word);
		if (held == 0)
		{
			*found = false;
			return true;
		}
		if (held == STD_ADD(u64, frame->tid.f0, 1, 562, 33))
		{
			*found = true;
			return txn_memtable_tuples_word(&page, at, true, 563, 43, &word, frame->error) &&
				txn_memtable_tuples_get_found(frame, host_delta_Word_load(&word));
		}
		at = STD_REM(u64, STD_ADD(u64, at, 1, 569, 28), count, 569, 27);
	}
}

/* txn_memtable_tuples_get_body_0 is get's body. */
static void
txn_memtable_tuples_get_body_0(void *frame_)
{
	txn_memtable_tuples_held_frame_0_t *frame = frame_;

	*frame->some = false;
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	for (;;)
	{
		HostSharedPast word = txn_memtable_tuples_word_of(frame->state);
		uint64		location;
		HostSharedRecord record;
		bool		found;

		if (!host_shared_Past_load(&word, &location))
		{
			frame->ok = txn_spill_tuple_key(frame->io, &frame->hold, frame->state, frame->tid, frame->some, frame->key,
											frame->error);
			break;
		}
		if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS, location, &record,
									frame->error) ||
			!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-tuples-reading", frame->error) ||
			!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_TUPLES_LOCK,
											&frame->guard, frame->error))
			break;
		frame->guard_live = true;
		if (txn_memtable_index_closed(&TXN_MEMTABLE_TUPLES_CLOSES_OPS, &frame->guard))
		{
			frame->guard_live = false;
			host_shared_Guard_drop(&frame->guard);
			continue;
		}
		if (!txn_memtable_tuples_get_open(frame, &found))
			break;
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
		frame->ok = found || txn_spill_tuple_key(frame->io, &frame->hold, frame->state, frame->tid, frame->some,
												 frame->key, frame->error);
		break;
	}
	txn_memtable_tuples_held_pad_0(frame);
}

/*
 * txn_memtable_tuples_get returns the key of a tid's row in a storage's
 * table, read under the shared lock of the table the state's word names now,
 * else in its spilled tuple ids, read after the table, or nothing for a tid
 * neither holds.
 */
bool
txn_memtable_tuples_get(HostIoIo *io, HostSharedRecord state, HostRow tid, bool *some, StdVec *result,
						HostError *error)
{
	txn_memtable_tuples_held_frame_0_t frame = {.io = io, .state = state, .error = error, .tid = tid, .some = some,
	.key = result};

	std_landing(&frame, txn_memtable_tuples_get_body_0, txn_memtable_tuples_held_pad_0);
	return frame.ok;
}

/* txn_memtable_tuples_retired_when_body_0 is retired_when's body. */
static void
txn_memtable_tuples_retired_when_body_0(void *frame_)
{
	txn_memtable_tuples_held_frame_0_t *frame = frame_;
	HostSharedPast word = txn_memtable_tuples_word_of(frame->state);
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	uint64		location;
	HostSharedRecord record;
	uint64		rows;
	bool		cleared;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!host_shared_Past_load(&word, &location))
	{
		frame->ok = true;
		txn_memtable_tuples_held_pad_0(frame);
		return;
	}
	if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS, location, &record, frame->error) ||
		!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_TUPLES_LOCK, &frame->guard,
								 frame->error))
	{
		txn_memtable_tuples_held_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	if (!txn_memtable_index_closed(&TXN_MEMTABLE_TUPLES_CLOSES_OPS, &frame->guard) &&
		(!frame->emptied ||
		 (host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_RESERVED, load) == 0 &&
		  !txn_memtable_table_memtable(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->state, &rows))))
	{
		std_vec_new(&frame->retiring.names, sizeof(uint64), _Alignof(uint64));
		frame->retiring_live = true;
		if (!txn_memtable_tuples_closed(&frame->guard, &frame->hold, location, true, &frame->retiring, frame->error))
		{
			txn_memtable_tuples_held_pad_0(frame);
			return;
		}
	}
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	if (!frame->retiring_live)
	{
		frame->ok = true;
		txn_memtable_tuples_held_pad_0(frame);
		return;
	}
	if (!host_shared_Record_lock(frame->state, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_STATE_STATE_LOCK_INDEX,
								 &frame->guard, frame->error))
	{
		txn_memtable_tuples_held_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	cleared = host_shared_Past_clear(&word, location);
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	frame->ok = !cleared || txn_memtable_tuples_Retiring_retire(&frame->retiring, frame->io, frame->error);
	txn_memtable_tuples_held_pad_0(frame);
}

/*
 * txn_memtable_tuples_retired_when retires a storage's table, closing it only
 * when it is open and, for an emptied storage, no reservation of it is
 * outstanding and no rows memtable is linked, read under its exclusive lock:
 * it gathers the table with every page, then, under the storage state's lock,
 * clears the state's word if it still names the table, and the process whose
 * store cleared it retires it.
 */
static bool
txn_memtable_tuples_retired_when(HostIoIo *io, HostSharedRecord state, bool emptied, HostError *error)
{
	txn_memtable_tuples_held_frame_0_t frame = {.io = io, .state = state, .error = error, .emptied = emptied};

	std_landing(&frame, txn_memtable_tuples_retired_when_body_0, txn_memtable_tuples_held_pad_0);
	return frame.ok;
}

/* txn_memtable_tuples_retired retires a storage's table at its unlink, when no process writes to it. */
bool
txn_memtable_tuples_retired(HostIoIo *io, HostSharedRecord state, HostError *error)
{
	return txn_memtable_tuples_retired_when(io, state, false, error);
}

/*
 * txn_memtable_tuples_emptied retires a storage's table as retired does once
 * the fold's drop has left the storage's rows with no version, whose every
 * tuple id the base's tuple id tree then pairs, but only when no reservation
 * of it is outstanding and no rows memtable is linked, both read under the
 * table's exclusive lock: a reservation is counted under that lock, and an
 * insert pushes its row before its finish stores the slot, so no reserved
 * insert ever meets a retired table, and a reserver that finds it closed
 * makes a new one.
 */
bool
txn_memtable_tuples_emptied(HostIoIo *io, HostSharedRecord state, HostError *error)
{
	return txn_memtable_tuples_retired_when(io, state, true, error);
}

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/simc/simc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/test.h"
#include "doltgres/txn/memtable/testing.h"

#define TXN_MEMTABLE_TABLE_RS Unit
#define TXN_MEMTABLE_TABLE_RETURNS bool
#define TXN_MEMTABLE_TABLE_RESULT_PARAMS , HostError *error
#define TXN_MEMTABLE_TABLE_DECLARE
#include "doltgres/txn/memtable/table_template.h"

/* txn_memtable_tuples_tests_key returns the row key a test gives a tid. */
static void
txn_memtable_tuples_tests_key(uint64 tid, StdVec *result)
{
	char		key[32];
	int			len = snprintf(key, sizeof(key), "key" UINT64_FORMAT, tid);

	std_slice_to_vec_Checked(result, (const uint8 *) key, len, 1, 1);
}

/* txn_memtable_tuples_tests_stored_frame_0_t is stored's state across its pad: its arguments, and its put, while live. */
typedef struct txn_memtable_tuples_tests_stored_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	uint64		tid;
	StdBytes	key;
	HostError  *error;
	bool		ok;
	TxnMemtableOp op;
	bool		op_live;
} txn_memtable_tuples_tests_stored_frame_0_t;

/* txn_memtable_tuples_tests_stored_pad_0 is stored's pad: its put, while live. */
static void
txn_memtable_tuples_tests_stored_pad_0(void *frame_)
{
	txn_memtable_tuples_tests_stored_frame_0_t *frame = frame_;

	if (frame->op_live)
	{
		frame->op_live = false;
		txn_memtable_Op_drop(&frame->op);
	}
}

/* txn_memtable_tuples_tests_stored_inside is stored's prepare's closure: the slot's finish, then the row's. */
bool
txn_memtable_tuples_tests_stored_inside(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
										HostError *error)
{
	const txn_memtable_tuples_tests_stored_t *c = captures;
	HostRow		tid = {c->tid};
	TxnMemtableVersionHandle handle;
	TxnStatusRoomed roomed = {0};

	return txn_memtable_tuples_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, tid, error) &&
		txn_memtable_table_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, c->change, false, roomed, &handle, error);
}

/* txn_memtable_tuples_tests_stored_body_0 is stored's body. */
static void
txn_memtable_tuples_tests_stored_body_0(void *frame_)
{
	txn_memtable_tuples_tests_stored_frame_0_t *frame = frame_;
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7),
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	txn_memtable_tuples_tests_stored_t captures = {.tid = frame->tid, .change = &change};
	txn_TupleRefSliceU8Usize_t rows[1];

	frame->op = txn_memtable_testing_put((const uint8 *) "v", 1);
	frame->op_live = true;
	change.op = &frame->op;
	rows[0].f0 = frame->key;
	rows[0].f1 = txn_memtable_version_encoded(&change.writer, change.op, change.shown, false, 0);
	frame->ok = txn_memtable_table_prepare_Unit(frame->io, frame->state, rows, 1, NULL, 0, true,
												txn_memtable_tuples_tests_stored_inside, &captures, NULL,
												frame->error);
	txn_memtable_tuples_tests_stored_pad_0(frame);
}

/* txn_memtable_tuples_tests_stored pushes a row's version of a key through a prepare that reserves its tid's slot, and stores the slot and the row in its window. */
static bool
txn_memtable_tuples_tests_stored(HostIoIo *io, HostSharedRecord state, uint64 tid, const uint8 *key, size_t key_len,
								 HostError *error)
{
	txn_memtable_tuples_tests_stored_frame_0_t frame = {.io = io, .state = state, .tid = tid, .error = error};

	frame.key.ptr = key;
	frame.key.len = key_len;
	std_landing(&frame, txn_memtable_tuples_tests_stored_body_0, txn_memtable_tuples_tests_stored_pad_0);
	return frame.ok;
}

/* txn_memtable_tuples_tests_inserted pushes a row's version through a prepare that reserves its tid's slot, the row's key the test's for the tid. */
static bool
txn_memtable_tuples_tests_inserted(HostIoIo *io, HostSharedRecord state, uint64 tid, HostError *error)
{
	StdVec		key;
	bool		ok;

	txn_memtable_tuples_tests_key(tid, &key);
	ok = txn_memtable_tuples_tests_stored(io, state, tid, key.ptr, key.len, error);
	std_vec_free_Checked(&key, 1, 1);
	return ok;
}

/* txn_memtable_tuples_tests_inserts inserts a tid's row, as inserted's unwrap does at its place. */
static void
txn_memtable_tuples_tests_inserts(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state, uint64 tid)
{
	HostError	error;

	if (!txn_memtable_tuples_tests_inserted(io, state, tid, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/* txn_memtable_tuples_tests_counts_t is counts' (u64, u64, u64, u64): the slots, the used and reserved slots, and the byte pages. */
typedef struct txn_memtable_tuples_tests_counts_t
{
	uint64		count;
	uint64		used;
	uint64		reserved;
	uint64		pages;

	/*
	 * slot_page is the name of the table's last slot page, byte_page of its
	 * first byte page left, and open_page of its newest.
	 */
	uint64		slot_page;
	uint64		byte_page;
	uint64		open_page;
} txn_memtable_tuples_tests_counts_t;

/* txn_memtable_tuples_tests_counts_frame_0_t is counts' state across its pad: the hold and the guard, while live. */
typedef struct txn_memtable_tuples_tests_counts_frame_0_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	txn_memtable_tuples_tests_counts_t *result;
	HostError  *error;
	bool		ok;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedGuard guard;
	bool		guard_live;
} txn_memtable_tuples_tests_counts_frame_0_t;

/* txn_memtable_tuples_tests_counts_pad_0 is counts' pad: the guard, then the hold, while live. */
static void
txn_memtable_tuples_tests_counts_pad_0(void *frame_)
{
	txn_memtable_tuples_tests_counts_frame_0_t *frame = frame_;

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

/* txn_memtable_tuples_tests_counts_body_0 is counts' body. */
static void
txn_memtable_tuples_tests_counts_body_0(void *frame_)
{
	txn_memtable_tuples_tests_counts_frame_0_t *frame = frame_;
	HostSharedPast word = txn_memtable_tuples_word_of(frame->state);
	HostSharedRecord record;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostDeltaListListView keys;
	uint64		location;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!host_shared_Past_load(&word, &location))
	{
		*frame->error = txn_memtable_testing_failed("no table");
		txn_memtable_tuples_tests_counts_pad_0(frame);
		return;
	}
	if (!host_delta_Hold_record(&frame->hold, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS, location, &record,
								frame->error) ||
		!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_TUPLES_LOCK,
										&frame->guard, frame->error))
	{
		txn_memtable_tuples_tests_counts_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	frame->result->count = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_COUNT, load);
	frame->result->used = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_USED, load);
	frame->result->reserved = host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_RESERVED, load);
	keys = txn_memtable_tuples_view(&frame->guard, TXN_MEMTABLE_TUPLES_KEYS);
	frame->result->pages = 0;
	frame->result->byte_page = 0;
	{
		HostDeltaListListView slots = txn_memtable_tuples_view(&frame->guard, TXN_MEMTABLE_TUPLES_SLOTS);

		if (!host_delta_list_ListView_get(&slots, &frame->hold, host_delta_list_ListView_len(&slots) - 1,
										  &frame->result->slot_page, frame->error))
		{
			txn_memtable_tuples_tests_counts_pad_0(frame);
			return;
		}
	}
	for (uint64 index = 0, len = host_delta_list_ListView_len(&keys); index < len; index++)
	{
		uint64		name;

		if (!host_delta_list_ListView_get(&keys, &frame->hold, index, &name, frame->error))
		{
			txn_memtable_tuples_tests_counts_pad_0(frame);
			return;
		}
		if (name != TXN_MEMTABLE_TUPLES_DROPPED && frame->result->pages++ == 0)
			frame->result->byte_page = name;
		frame->result->open_page = name;
	}
	frame->ok = true;
	txn_memtable_tuples_tests_counts_pad_0(frame);
}

/* txn_memtable_tuples_tests_counts returns a storage's table's count of slots, its counts of used and reserved slots, and its count of byte pages but those a compaction retired. */
static bool
txn_memtable_tuples_tests_counts(HostIoIo *io, HostSharedRecord state, txn_memtable_tuples_tests_counts_t *result,
								 HostError *error)
{
	txn_memtable_tuples_tests_counts_frame_0_t frame = {.io = io, .state = state, .result = result, .error = error};

	std_landing(&frame, txn_memtable_tuples_tests_counts_body_0, txn_memtable_tuples_tests_counts_pad_0);
	return frame.ok;
}

/* txn_memtable_tuples_tests_counts_Debug_fmt is counts' (u64, u64, u64, u64)'s Debug. */
static bool
txn_memtable_tuples_tests_counts_Debug_fmt(const void *value, StdFormatter *f)
{
	const txn_memtable_tuples_tests_counts_t *counts = value;
	StdTupleRef tuple = STD_TUPLE(4, STD_ITEM(&counts->count, std_u64_Debug_fmt),
								  STD_ITEM(&counts->used, std_u64_Debug_fmt),
								  STD_ITEM(&counts->reserved, std_u64_Debug_fmt),
								  STD_ITEM(&counts->pages, std_u64_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_memtable_tuples_tests_counted reads counts, as counts' unwrap does at its place. */
static txn_memtable_tuples_tests_counts_t
txn_memtable_tuples_tests_counted(uint32 line, uint32 col, HostIoIo *io, HostSharedRecord state)
{
	txn_memtable_tuples_tests_counts_t counts;
	HostError	error;

	if (!txn_memtable_tuples_tests_counts(io, state, &counts, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return counts;
}

/* txn_memtable_tuples_tests_counts_are asserts counts, as assert_eq on counts' unwrap does at their places. */
static void
txn_memtable_tuples_tests_counts_are(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, HostIoIo *io,
									 HostSharedRecord state, uint64 count, uint64 used, uint64 reserved, uint64 pages)
{
	txn_memtable_tuples_tests_counts_t counts = txn_memtable_tuples_tests_counted(unwrap_line, unwrap_col, io, state);
	txn_memtable_tuples_tests_counts_t expected = {count, used, reserved, pages};

	STD_ASSERT_EQ(line, col, counts.count == count && counts.used == used && counts.reserved == reserved &&
				  counts.pages == pages, &counts, txn_memtable_tuples_tests_counts_Debug_fmt, &expected,
				  txn_memtable_tuples_tests_counts_Debug_fmt);
}

/* txn_memtable_tuples_tests_count_is asserts the table's count of slots, as assert_eq on counts' unwrap's first does at their places. */
static void
txn_memtable_tuples_tests_count_is(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, HostIoIo *io,
								   HostSharedRecord state, uint64 count)
{
	txn_memtable_tuples_tests_counts_t counts = txn_memtable_tuples_tests_counted(unwrap_line, unwrap_col, io, state);

	STD_ASSERT_EQ(line, col, counts.count == count, &counts.count, std_u64_Debug_fmt, &count, std_u64_Debug_fmt);
}

/* txn_memtable_tuples_tests_key_Debug_fmt is a key's Debug, of a Vec<u8>. */
static bool
txn_memtable_tuples_tests_key_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *key = value;
	StdBytes	bytes = {key->ptr, key->len};

	return std_bytes_Debug_fmt(&bytes, f);
}

/*
 * txn_memtable_tuples_tests_got_is asserts a tid's key, or none, as assert_eq
 * on get's unwrap does at their places, with the tid's message when one is
 * given.
 */
static void
txn_memtable_tuples_tests_got_is(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, HostIoIo *io,
								 HostSharedRecord state, uint64 tid, const StdVec *expected, bool message)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	HostRow		row = {tid};
	HostError	error;
	StdVec		got;
	bool		some;
	StdOptionRef left;
	StdOptionRef right = {expected != NULL, STD_ITEM(expected, txn_memtable_tuples_tests_key_Debug_fmt)};

	if (!txn_memtable_tuples_get(io, state, row, &some, &got, &error))
		STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	left.has = some;
	left.some = STD_ITEM(&got, txn_memtable_tuples_tests_key_Debug_fmt);
	if (some != (expected != NULL) ||
		(some && (got.len != expected->len || (got.len > 0 && memcmp(got.ptr, expected->ptr, got.len) != 0))))
	{
		StdFmtArg	args[1] = {{&tid, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_assert_failed(RUST_FILE, line, col, "==", &left, std_option_Debug_fmt, &right, std_option_Debug_fmt,
						  message ? &arguments : NULL);
	}
	if (some)
		std_vec_free_Checked(&got, 1, 1);
}

/* txn_memtable_tuples_tests_got_key asserts a tid's key is the test's for it, as assert_eq on get's unwrap does with the tid's message. */
static void
txn_memtable_tuples_tests_got_key(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, HostIoIo *io,
								  HostSharedRecord state, uint64 tid)
{
	StdVec		key;

	txn_memtable_tuples_tests_key(tid, &key);
	txn_memtable_tuples_tests_got_is(line, col, unwrap_line, unwrap_col, io, state, tid, &key, true);
	std_vec_free_Checked(&key, 1, 1);
}

/* txn_memtable_tuples_tests_none_next is std::iter::Empty's next: no item. */
static bool
txn_memtable_tuples_tests_none_next(void *self, void *result)
{
	return false;
}

/* txn_memtable_tuples_tests_none_size_hint is std::iter::Empty's size_hint: no item left. */
static StdSizeHint
txn_memtable_tuples_tests_none_size_hint(const void *self)
{
	StdSizeHint hint = {.has_upper = true};

	return hint;
}

/* TXN_MEMTABLE_TUPLES_TESTS_NONE_ITERATOR_OPS is std::iter::Empty's Iterator, of the pages a retirement of none takes. */
static const StdIteratorOps TXN_MEMTABLE_TUPLES_TESTS_NONE_ITERATOR_OPS = {
	.next = txn_memtable_tuples_tests_none_next,
	.size_hint = txn_memtable_tuples_tests_none_size_hint,
};

/*
 * The tid table's case: every tid inserted is found with its row's key,
 * before and after a growth past the load factor that doubles the slots
 * once, a tid never inserted isn't found, and the storage's unlink retires
 * the table with its pages, so the delta's use returns to its start.
 */
void
test_txn_memtable_tuples_tests_a_tables_tids_are_found_after_inserts_and_a_growth(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		start = host_delta_used();
	uint64		used;
	HostSharedHeld held = txn_memtable_testing_created(io, 9342);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostStorage storage = txn_memtable_testing_storage(9342);
	StdIterator none = {&TXN_MEMTABLE_TUPLES_TESTS_NONE_ITERATOR_OPS};
	HostError	error;

	for (uint64 tid = 1; tid <= 300; tid++)
		txn_memtable_tuples_tests_inserts(682, 38, io, state, tid);
	txn_memtable_tuples_tests_count_is(684, 9, 684, 38, io, state, TXN_MEMTABLE_TUPLES_PER);
	for (uint64 tid = 301; tid <= 400; tid++)
		txn_memtable_tuples_tests_inserts(686, 38, io, state, tid);
	txn_memtable_tuples_tests_counts_are(688, 9, 688, 38, io, state, 2 * TXN_MEMTABLE_TUPLES_PER, 400,
										 0, 7);
	for (uint64 tid = 1; tid <= 400; tid++)
		txn_memtable_tuples_tests_got_key(690, 13, 690, 49, io, state, tid);
	txn_memtable_tuples_tests_got_is(692, 9, 692, 45, io, state, 401, NULL, false);
	host_shared_Held_drop(&held);
	if (!txn_state_unlinked(io, &storage, &error))
		STD_RESULT_EXPECT_FAILED(694, 38, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_retire(io, NULL, 0, none, &error))
		STD_RESULT_EXPECT_FAILED(695, 52, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	used = host_delta_used();
	STD_ASSERT_EQ(696, 9, used == start, &used, std_u64_Debug_fmt, &start, std_u64_Debug_fmt);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_tuples_tests_endings_are asserts that each of a run's actors returned, as the endings loop's assert_eq does at its place with the actor's name. */
static void
txn_memtable_tuples_tests_endings_are(uint32 panic_line, uint32 panic_col, uint32 line, uint32 col,
									  const SimcReport *report)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	SimcEnding	returned = {.tag = SIMC_ENDING_RETURNED};
	const StdVec *endings;

	if (report->outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtArg	args[1] = {{&report->outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, panic_line, panic_col, &message);
	}
	endings = &report->outcome.u.ended;
	for (size_t k = 0; k < endings->len; k++)
	{
		const simc_TupleStringEnding_t *ending = &STD_VEC_AT(endings, simc_TupleStringEnding_t, k);

		if (!simc_Ending_PartialEq_eq(&ending->f1, &returned))
		{
			StdFmtArg	args[1] = {{&ending->f0, std_String_Display_fmt}};
			StdFmtArguments message = {pieces, lengthof(pieces), args};

			std_assert_failed(RUST_FILE, line, col, "==", &ending->f1, simc_Ending_Debug_fmt, &returned,
							  simc_Ending_Debug_fmt, &message);
		}
	}
}

/* txn_memtable_tuples_tests_reader is the growth's reader: it reads tid 5's key across the growth. */
static bool
txn_memtable_tuples_tests_reader(void *captures, HostIoIo *io, HostError *error)
{
	HostSharedHeld held;
	HostRow		five = {5};
	StdVec		read;
	StdVec		key;
	bool		some;
	bool		found;

	if (!txn_memtable_testing_stated(io, 9343, &held, error))
		return false;
	if (!txn_memtable_tuples_get(io, host_shared_Held_record(&held), five, &some, &read, error))
	{
		host_shared_Held_drop(&held);
		return false;
	}
	txn_memtable_tuples_tests_key(5, &key);
	found = some && read.len == key.len && memcmp(read.ptr, key.ptr, key.len) == 0;
	std_vec_free_Checked(&key, 1, 1);
	if (some)
		std_vec_free_Checked(&read, 1, 1);
	host_shared_Held_drop(&held);
	if (!found)
	{
		*error = txn_memtable_testing_failed("a reader across a growth missed its tid");
		return false;
	}
	return true;
}

/* txn_memtable_tuples_tests_inserter is the growth cases' inserter: it inserts its tid's row in its database's storage. */
static bool
txn_memtable_tuples_tests_inserter(uint32 database, uint64 tid, HostIoIo *io, HostError *error)
{
	HostSharedHeld held;
	bool		ok;

	if (!txn_memtable_testing_stated(io, database, &held, error))
		return false;
	ok = txn_memtable_tuples_tests_inserted(io, host_shared_Held_record(&held), tid, error);
	host_shared_Held_drop(&held);
	return ok;
}

/* txn_memtable_tuples_tests_grower is the growth's grower: its insert grows the table. */
static bool
txn_memtable_tuples_tests_grower(void *captures, HostIoIo *io, HostError *error)
{
	return txn_memtable_tuples_tests_inserter(9343, 1000, io, error);
}

/*
 * The growth's reader case: a reader held between its open of a table and
 * its take of the table's lock while an insert grows the table, with the old
 * table retired under the reader's hold, finds it closed and reads the tid's
 * key from the next.
 */
void
test_txn_memtable_tuples_tests_a_reader_held_across_a_growth_reads_from_the_next_table(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9343);
	SimcRun		run;
	SimcStep	steps[2] = {{"grower", 6, PG_UINT64_MAX}, {"reader", 6, PG_UINT64_MAX}};
	SimcActor	reader = {txn_memtable_tuples_tests_reader};
	SimcActor	grower = {txn_memtable_tuples_tests_grower};
	SimcReport	report;

	for (uint64 tid = 1; tid <= TXN_MEMTABLE_TUPLES_PER / 4 * 3; tid++)
		txn_memtable_tuples_tests_inserts(709, 46, io, host_shared_Held_record(&held), tid);
	run = simc_Run_new(163);
	simc_Run_actor(&run, "reader", 6, reader);
	simc_Run_actor(&run, "grower", 6, grower);
	simc_Run_until(&run, "reader", 6, "doltgres-tuples-reading");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_tuples_tests_endings_are(726, 67, 728, 13, &report);
	txn_memtable_tuples_tests_count_is(730, 9, 730, 46, io, host_shared_Held_record(&held),
									   2 * TXN_MEMTABLE_TUPLES_PER);
	simc_Report_drop(&report);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_tuples_tests_prepared is the bound case's prepares' body: it inserts its tid's row, the tid its captures hold. */
static bool
txn_memtable_tuples_tests_prepared(void *captures, HostIoIo *io, HostError *error)
{
	const uint64 *tid = captures;

	return txn_memtable_tuples_tests_inserter(9344, *tid, io, error);
}

/*
 * T1's and T2's case: two prepares at the load bound each get a slot and
 * the table grows once: the first reserves its slot and appends its key's
 * bytes to the table's newest byte page, the second's growth seals that page
 * and adds a fresh one, to which it appends its key's bytes, and both
 * finishes' slots read their keys.
 */
void
test_txn_memtable_tuples_tests_two_prepares_at_the_bound_each_get_a_slot_through_one_growth(void)
{
	static const char *const names[2] = {"first", "second"};
	static const uint64 tids[2] = {1000, 2000};
	static const uint64 found[3] = {1, 1000, 2000};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9344);
	SimcRun		run;
	SimcStep	steps[2] = {{"second", 6, PG_UINT64_MAX}, {"first", 5, PG_UINT64_MAX}};
	SimcReport	report;
	HostError	error;
	bool		freed;

	for (uint64 tid = 1; tid < TXN_MEMTABLE_TUPLES_PER / 4 * 3; tid++)
		txn_memtable_tuples_tests_inserts(743, 46, io, host_shared_Held_record(&held), tid);
	run = simc_Run_new(167);
	for (size_t k = 0; k < lengthof(names); k++)
	{
		SimcActor	actor = {txn_memtable_tuples_tests_prepared, (void *) &tids[k]};

		simc_Run_actor(&run, names[k], strlen(names[k]), actor);
	}
	simc_Run_until(&run, "first", 5, "doltgres-memtable-prepared");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_tuples_tests_endings_are(755, 67, 757, 13, &report);
	txn_memtable_tuples_tests_counts_are(759, 9, 759, 46, io, host_shared_Held_record(&held),
										 2 * TXN_MEMTABLE_TUPLES_PER, TXN_MEMTABLE_TUPLES_PER / 4 * 3 + 1, 0, 7);
	for (size_t k = 0; k < lengthof(found); k++)
		txn_memtable_tuples_tests_got_key(761, 13, 761, 57, io, host_shared_Held_record(&held),
										  found[k]);
	if (!TXN_STATE_STATE_POOL_OPS.reclaim(io, &freed, &error))
		STD_RESULT_EXPECT_FAILED(763, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(763, 9, !freed, "!State::reclaim(io).unwrap()");
	simc_Report_drop(&report);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * A tid used again: a second insert of a tid's row with another key stores
 * its slot over the first's, so the tid names its newest key and the table
 * counts one slot.
 */
void
test_txn_memtable_tuples_tests_a_tid_used_again_names_its_newest_key(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9349);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostError	error;
	StdVec		other = {(void *) "other", 5, 5};

	txn_memtable_tuples_tests_inserts(774, 40, io, state, 5);
	if (!txn_memtable_tuples_tests_stored(io, state, 5, (const uint8 *) "other", 5, &error))
		STD_RESULT_EXPECT_FAILED(775, 48, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_tuples_tests_got_is(776, 9, 776, 51, io, state, 5, &other, false);
	txn_memtable_tuples_tests_counts_are(777, 9, 777, 46, io, state, TXN_MEMTABLE_TUPLES_FIRST_COUNT,
										 1, 0, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_tuples_tests_none is the reservation case's prepare's closure, which stores no slot. */
static bool
txn_memtable_tuples_tests_none(void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
							   HostError *error)
{
	return true;
}

/*
 * A reservation no finish stored: a prepare whose window stores no slot has
 * its settle give the slot back, so the table counts none reserved.
 */
void
test_txn_memtable_tuples_tests_a_reservation_no_finish_stored_is_given_back(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9352);
	HostSharedRecord state = host_shared_Held_record(&held);
	txn_TupleRefSliceU8Usize_t rows[1];
	HostError	error;

	rows[0].f0.ptr = (const uint8 *) "k";
	rows[0].f0.len = 1;
	rows[0].f1 = 64;
	if (!txn_memtable_table_prepare_Unit(io, state, rows, 1, NULL, 0, true, txn_memtable_tuples_tests_none, NULL, NULL,
										 &error))
		STD_RESULT_EXPECT_FAILED(788, 89, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_tuples_tests_counts_are(789, 9, 789, 46, io, state, TXN_MEMTABLE_TUPLES_FIRST_COUNT,
										 0, 0, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * A probe past another tid's slot: two tids whose first slot is one,
 * inserted in turn, are each found, the second one slot on.
 */
void
test_txn_memtable_tuples_tests_two_tids_of_one_first_slot_are_each_found(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9360);
	HostSharedRecord state = host_shared_Held_record(&held);
	uint64		other = 2;

	while (txn_memtable_tuples_slot(other, TXN_MEMTABLE_TUPLES_PER) !=
		   txn_memtable_tuples_slot(1, TXN_MEMTABLE_TUPLES_PER))
		other++;
	txn_memtable_tuples_tests_inserts(802, 46, io, state, 1);
	txn_memtable_tuples_tests_inserts(802, 46, io, state, other);
	txn_memtable_tuples_tests_got_key(805, 13, 805, 57, io, state, other);
	txn_memtable_tuples_tests_got_key(805, 13, 805, 57, io, state, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_tuples_tests_page_of opens the first page a table's list names, as the page's two unwraps do at their places. */
static HostDeltaPageRef
txn_memtable_tuples_tests_page_of(uint32 get_line, uint32 get_col, uint32 page_line, uint32 page_col,
								  const HostSharedGuard *guard,
								  const HostDeltaHold *hold, size_t list)
{
	HostDeltaListListView view = txn_memtable_tuples_view(guard, list);
	HostDeltaPageRef page;
	HostError	error;
	uint64		name;

	if (!host_delta_list_ListView_get(&view, hold, 0, &name, &error))
		STD_RESULT_EXPECT_FAILED(get_line, get_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_page(hold, host_delta_page_Page_named(name), &page, &error))
		STD_RESULT_EXPECT_FAILED(page_line, page_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return page;
}

/*
 * Bugs row 150's first pages: a table's first slot page holds its 16 slots
 * in 32 words, and its first byte page is the smallest that holds its first
 * key, 1 KiB for a short key and 2 KiB for one of 1,500 bytes.
 */
void
test_txn_memtable_tuples_tests_a_tables_first_pages_are_the_smallest_that_hold_them(void)
{
	static const uint32 databases[2] = {9522, 9523};
	static const size_t lens[2] = {8, 1500};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint8		key[1500];

	memset(key, 'k', sizeof(key));
	for (size_t k = 0; k < lengthof(databases); k++)
	{
		HostSharedHeld held = txn_memtable_testing_created(io, databases[k]);
		HostSharedPast word = txn_memtable_tuples_word_of(host_shared_Held_record(&held));
		HostDeltaHold hold;
		HostSharedRecord record;
		HostSharedGuard guard;
		HostDeltaPageRef slots;
		HostDeltaPageRef byte;
		HostError	error;
		uint64		location;
		uint64		words;
		uint64		twice = 2 * TXN_MEMTABLE_TUPLES_FIRST_COUNT;
		uint64		held_bytes;
		uint64		size;
		uint32		bytes;
		uint32		smallest;

		if (!txn_memtable_tuples_tests_stored(io, host_shared_Held_record(&held), 1, key, lens[k], &error))
			STD_RESULT_EXPECT_FAILED(817, 48, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!host_delta_hold(io, &hold, &error))
			STD_RESULT_EXPECT_FAILED(818, 40, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!host_shared_Past_load(&word, &location))
			STD_OPTION_UNWRAP_FAILED(819, 76);
		if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS, location, &record, &error))
			STD_RESULT_EXPECT_FAILED(820, 58, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_TUPLES_LOCK, &guard,
											&error))
			STD_RESULT_EXPECT_FAILED(821, 62, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		slots = txn_memtable_tuples_tests_page_of(822, 86, 822, 97, &guard, &hold,
												  TXN_MEMTABLE_TUPLES_SLOTS);
		byte = txn_memtable_tuples_tests_page_of(823, 84, 823, 95, &guard, &hold,
												 TXN_MEMTABLE_TUPLES_KEYS);
		words = host_delta_PageRef_words(&slots);
		STD_ASSERT_EQ(824, 13, words == twice, &words, std_u64_Debug_fmt, &twice, std_u64_Debug_fmt);
		held_bytes = host_delta_PageRef_filled(&byte) + host_delta_PageRef_room(&byte);
		bytes = txn_memtable_tuples_bytes(lens[k], false, 0).bytes;
		size = bytes;
		STD_ASSERT_EQ(825, 13, held_bytes == size, &held_bytes, std_u64_Debug_fmt, &size,
					  std_u64_Debug_fmt);
		smallest = lens[k] < 1024 ? 1 << 10 : 2 << 10;
		STD_ASSERT_EQ(826, 13, bytes == smallest, &bytes, std_u32_Debug_fmt, &smallest, std_u32_Debug_fmt);
		host_shared_Guard_drop(&guard);
		host_delta_Hold_drop(&hold);
		host_shared_Held_drop(&held);
	}
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_tuples_tests_sized returns the bytes of a storage's table's slot pages and byte pages, as sized does with its unwraps. */
static uint64
txn_memtable_tuples_tests_sized(HostIoIo *io, HostSharedRecord state)
{
	HostSharedPast word = txn_memtable_tuples_word_of(state);
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostDeltaListListView lists[2];
	HostError	error;
	uint64		location;
	uint64		bytes = 0;

	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 1, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Past_load(&word, &location))
		STD_OPTION_UNWRAP_FAILED(__LINE__, 1);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS, location, &record, &error) ||
		!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_TUPLES_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 1, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	lists[0] = txn_memtable_tuples_view(&guard, TXN_MEMTABLE_TUPLES_SLOTS);
	lists[1] = txn_memtable_tuples_view(&guard, TXN_MEMTABLE_TUPLES_KEYS);
	for (size_t list = 0; list < lengthof(lists); list++)
	{
		for (uint64 index = 0, len = host_delta_list_ListView_len(&lists[list]); index < len; index++)
		{
			uint64		name;
			HostDeltaPageRef page;

			if (!host_delta_list_ListView_get(&lists[list], &hold, index, &name, &error))
				STD_RESULT_EXPECT_FAILED(__LINE__, 1, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			if (list == 1 && name == TXN_MEMTABLE_TUPLES_DROPPED)
				continue;
			if (!host_delta_Hold_page(&hold, host_delta_page_Page_named(name), &page, &error))
				STD_RESULT_EXPECT_FAILED(__LINE__, 1, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			bytes += list == 0 ? 8 * host_delta_PageRef_words(&page) :
				host_delta_PageRef_filled(&page) + host_delta_PageRef_room(&page);
		}
	}
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	return bytes;
}

/*
 * txn_memtable_tuples_tests_filled inserts the rows of tids 1 to inserted_to,
 * drops the slots of tids 1 to dropped_to as a spill drops them, and inserts
 * the rows of the tids after inserted_to to last one at a time, the last of
 * them past the table's load factor.
 */
static void
txn_memtable_tuples_tests_filled(HostIoIo *io, HostSharedRecord state, uint64 inserted_to, uint64 dropped_to,
								 uint64 last)
{
	uint64		tids[TXN_MEMTABLE_TUPLES_PER];
	HostError	error;

	for (uint64 tid = 1; tid <= inserted_to; tid++)
		txn_memtable_tuples_tests_inserts(__LINE__, 1, io, state, tid);
	for (uint64 tid = 1; tid <= dropped_to; tid++)
		tids[tid - 1] = tid;
	if (!txn_memtable_tuples_dropped(io, state, tids, (size_t) dropped_to, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 1, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (uint64 tid = inserted_to + 1; tid <= last; tid++)
		txn_memtable_tuples_tests_inserts(__LINE__, 1, io, state, tid);
}

/*
 * The compaction's case: a table whose most slots a spill dropped, past its
 * load factor at its next insert, is compacted in place: its live slots
 * stored again in its own slot pages, the dropped ones left behind, and
 * every byte page but the open one retired, the live keys moved into the
 * open page's room, so every live tid is found, every dropped one isn't, and
 * the table holds fewer bytes than it held.
 */
void
test_txn_memtable_tuples_tests_a_table_of_mostly_dropped_tids_is_compacted_at_its_load_factor(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9361);
	HostSharedRecord state = host_shared_Held_record(&held);
	uint64		before;
	uint64		after;

	txn_memtable_tuples_tests_filled(io, state, 300, 290, TXN_MEMTABLE_TUPLES_PER / 4 * 3);
	before = txn_memtable_tuples_tests_sized(io, state);
	txn_memtable_tuples_tests_counts_are(__LINE__, 1, __LINE__, 1, io, state, TXN_MEMTABLE_TUPLES_PER,
										 TXN_MEMTABLE_TUPLES_PER / 4 * 3, 0, 6);
	txn_memtable_tuples_tests_inserts(__LINE__, 1, io, state, 1000);
	txn_memtable_tuples_tests_counts_are(__LINE__, 1, __LINE__, 1, io, state, TXN_MEMTABLE_TUPLES_PER,
										 TXN_MEMTABLE_TUPLES_PER / 4 * 3 - 290 + 1, 0, 1);
	after = txn_memtable_tuples_tests_sized(io, state);
	STD_ASSERT(__LINE__, 1, after < before, "the compaction retired the byte pages of the dropped keys");
	for (uint64 tid = 291; tid <= TXN_MEMTABLE_TUPLES_PER / 4 * 3; tid++)
		txn_memtable_tuples_tests_got_key(__LINE__, 1, __LINE__, 1, io, state, tid);
	txn_memtable_tuples_tests_got_key(__LINE__, 1, __LINE__, 1, io, state, 1000);
	for (uint64 tid = 1; tid <= 290; tid++)
		txn_memtable_tuples_tests_got_is(__LINE__, 1, __LINE__, 1, io, state, tid, NULL, false);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_tuples_tests_compactions is the compaction cases' writers: each inserts the row of the tid its captures hold, after its database. */
static bool
txn_memtable_tuples_tests_compactions(void *captures, HostIoIo *io, HostError *error)
{
	const uint64 *given = captures;

	return txn_memtable_tuples_tests_inserter((uint32) given[0], given[1], io, error);
}

/*
 * The compaction's reservation case: a compaction while another prepare's
 * reservation holds a place on a byte page moves no byte page: it stores the
 * slots again alone, leaving the dropped ones behind and every byte page
 * where it was, so the outstanding reservation's finish stores its key's
 * place as it was.
 */
void
test_txn_memtable_tuples_tests_a_compaction_with_a_reservation_outstanding_moves_no_byte_page(void)
{
	static const char *const names[2] = {"reserving", "compacting"};
	static const uint64 given[2][2] = {{9362, 2000}, {9362, 1000}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9362);
	HostSharedRecord state = host_shared_Held_record(&held);
	SimcRun		run;
	SimcStep	steps[2] = {{"compacting", 10, PG_UINT64_MAX}, {"reserving", 9, PG_UINT64_MAX}};
	SimcReport	report;

	txn_memtable_tuples_tests_filled(io, state, 300, 290, TXN_MEMTABLE_TUPLES_PER / 4 * 3 - 1);
	run = simc_Run_new(171);
	for (size_t k = 0; k < lengthof(names); k++)
	{
		SimcActor	actor = {txn_memtable_tuples_tests_compactions, (void *) given[k]};

		simc_Run_actor(&run, names[k], strlen(names[k]), actor);
	}
	simc_Run_until(&run, "reserving", 9, "doltgres-memtable-prepared");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_tuples_tests_endings_are(__LINE__, 1, __LINE__, 1, &report);
	txn_memtable_tuples_tests_counts_are(__LINE__, 1, __LINE__, 1, io, state, TXN_MEMTABLE_TUPLES_PER,
										 TXN_MEMTABLE_TUPLES_PER / 4 * 3 - 290 + 1, 0, 6);
	for (uint64 tid = 291; tid < TXN_MEMTABLE_TUPLES_PER / 4 * 3; tid++)
		txn_memtable_tuples_tests_got_key(__LINE__, 1, __LINE__, 1, io, state, tid);
	txn_memtable_tuples_tests_got_key(__LINE__, 1, __LINE__, 1, io, state, 1000);
	txn_memtable_tuples_tests_got_key(__LINE__, 1, __LINE__, 1, io, state, 2000);
	simc_Report_drop(&report);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * The compaction's lock case: a writer that reaches the table while a
 * compaction, paused before it releases the table's exclusive lock, holds it
 * waits on the lock and reserves in the compacted table once it ends, so
 * both writers' tids and every live tid before them are found.
 */
void
test_txn_memtable_tuples_tests_a_writer_waits_on_a_compactions_exclusive_lock(void)
{
	static const char *const names[2] = {"compacting", "writer"};
	static const uint64 given[2][2] = {{9363, 1000}, {9363, 2000}};
	static const uint64 found[3] = {1000, 2000, 291};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9363);
	HostSharedRecord state = host_shared_Held_record(&held);
	SimcRun		run;
	SimcStep	steps[2] = {{"writer", 6, PG_UINT64_MAX}, {"compacting", 10, PG_UINT64_MAX}};
	SimcReport	report;
	txn_memtable_tuples_tests_counts_t counts;
	uint64		used = TXN_MEMTABLE_TUPLES_PER / 4 * 3 - 290 + 2;

	txn_memtable_tuples_tests_filled(io, state, 300, 290, TXN_MEMTABLE_TUPLES_PER / 4 * 3);
	run = simc_Run_new(173);
	for (size_t k = 0; k < lengthof(names); k++)
	{
		SimcActor	actor = {txn_memtable_tuples_tests_compactions, (void *) given[k]};

		simc_Run_actor(&run, names[k], strlen(names[k]), actor);
	}
	simc_Run_until(&run, "compacting", 10, "doltgres-tuples-compacting");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	txn_memtable_tuples_tests_endings_are(__LINE__, 1, __LINE__, 1, &report);
	for (size_t k = 0; k < lengthof(found); k++)
		txn_memtable_tuples_tests_got_key(__LINE__, 1, __LINE__, 1, io, state, found[k]);
	counts = txn_memtable_tuples_tests_counted(__LINE__, 1, io, state);
	STD_ASSERT_EQ(__LINE__, 1, counts.used == used, &counts.used, std_u64_Debug_fmt, &used, std_u64_Debug_fmt);
	simc_Report_drop(&report);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}


/* TxnMemtableTuplesTestsRefusal is a compaction case's refusal: an allocation, the last slot page's open, the first byte page's, or the open byte page's. */
typedef enum TxnMemtableTuplesTestsRefusal
{
	TXN_MEMTABLE_TUPLES_TESTS_ALLOCATION,
	TXN_MEMTABLE_TUPLES_TESTS_SLOT_PAGE,
	TXN_MEMTABLE_TUPLES_TESTS_BYTE_PAGE,
	TXN_MEMTABLE_TUPLES_TESTS_OPEN_PAGE,
} TxnMemtableTuplesTestsRefusal;

/* txn_memtable_tuples_tests_insert_t is a refused compaction's insert's captures: the io, the state, and whether it finished. */
typedef struct txn_memtable_tuples_tests_insert_t
{
	HostIoIo   *io;
	HostSharedRecord state;
	bool		inserted;
} txn_memtable_tuples_tests_insert_t;

/* txn_memtable_tuples_tests_insert_body inserts tid 1000's row, an error answering it unfinished. */
static void
txn_memtable_tuples_tests_insert_body(void *captures)
{
	txn_memtable_tuples_tests_insert_t *insert = captures;
	HostError	error;

	insert->inserted = txn_memtable_tuples_tests_inserted(insert->io, insert->state, 1000, &error);
	if (!insert->inserted)
		host_Error_drop(&error);
}

/*
 * txn_memtable_tuples_tests_refused fills a table to its compaction, makes
 * the compacting insert's step of a refusal fail after a count of them, an
 * allocation's failure where it can't be refused panicking, and checks that
 * every live tid's key is still found, the insert's tid's too when it
 * finished; it answers whether the insert failed or panicked.
 */
static bool
txn_memtable_tuples_tests_refused(uint32 database, TxnMemtableTuplesTestsRefusal refusal, uint64 after)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, database);
	HostSharedRecord state = host_shared_Held_record(&held);
	txn_memtable_tuples_tests_counts_t counts;
	txn_memtable_tuples_tests_insert_t insert = {.io = io, .state = state};
	StdCaught	caught;

	txn_memtable_tuples_tests_filled(io, state, 300, 290, TXN_MEMTABLE_TUPLES_PER / 4 * 3);
	counts = txn_memtable_tuples_tests_counted(__LINE__, 1, io, state);
	switch (refusal)
	{
		case TXN_MEMTABLE_TUPLES_TESTS_ALLOCATION:
			simc_fail_allocation(after + 1);
			break;

		case TXN_MEMTABLE_TUPLES_TESTS_SLOT_PAGE:
			simc_refuse_open(host_delta_page_Page_named(counts.slot_page), after);
			break;
		case TXN_MEMTABLE_TUPLES_TESTS_BYTE_PAGE:
			simc_refuse_open(host_delta_page_Page_named(counts.byte_page), after);
			break;
		case TXN_MEMTABLE_TUPLES_TESTS_OPEN_PAGE:
			simc_refuse_open(host_delta_page_Page_named(counts.open_page), after);
			break;
	}
	if (!std_catch_unwind(txn_memtable_tuples_tests_insert_body, &insert, &caught))
		std_Caught_drop_Checked(&caught);
	simc_fail_allocation(0);
	simc_refuse_nothing();
	for (uint64 tid = 291; tid <= TXN_MEMTABLE_TUPLES_PER / 4 * 3; tid++)
		txn_memtable_tuples_tests_got_key(__LINE__, 1, __LINE__, 1, io, state, tid);
	if (insert.inserted)
		txn_memtable_tuples_tests_got_key(__LINE__, 1, __LINE__, 1, io, state, 1000);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
	return !insert.inserted;
}

/* txn_memtable_tuples_tests_refusals runs a refusal's case after each count of its steps up to a bound, and asserts that one of them failed the insert. */
static void
txn_memtable_tuples_tests_refusals(uint32 database, TxnMemtableTuplesTestsRefusal refusal, uint64 bound)
{
	uint64		failed = 0;

	for (uint64 after = 0; after < bound; after++)
		failed += txn_memtable_tuples_tests_refused(database, refusal, after) ? 1 : 0;
	STD_ASSERT_EQ_U64(__LINE__, 1, failed > 0 ? refusal + 1 : 0, refusal + 1);
}

/*
 * The compaction's failure case (bugs row 291's review C1): a compacting
 * insert whose allocation or page open fails, or panics, at any of its steps
 * (the gather's slot pages, the moves' byte pages and the open page, the
 * reinsert's plan) leaves every live tid's key found, since every step that
 * can fail runs before the compaction zeroes its first slot and each moved
 * key's new place is stored at once.
 */
void
test_txn_memtable_tuples_tests_a_compaction_failed_at_any_step_leaves_every_tid_found(void)
{
	txn_memtable_tuples_tests_refusals(9397, TXN_MEMTABLE_TUPLES_TESTS_ALLOCATION, 200);
	txn_memtable_tuples_tests_refusals(9398, TXN_MEMTABLE_TUPLES_TESTS_SLOT_PAGE, 4);
	txn_memtable_tuples_tests_refusals(9399, TXN_MEMTABLE_TUPLES_TESTS_BYTE_PAGE, 4);
	txn_memtable_tuples_tests_refusals(9388, TXN_MEMTABLE_TUPLES_TESTS_OPEN_PAGE, 4);
}

#endif							/* DOLTGRES_TEST_TXN */
