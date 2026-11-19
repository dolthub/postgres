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

/* TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS is Tuples' Shape and Aged: a storage's tuple id table, its lock, four atomics, and its slot and byte pages. */
const HostSharedAgedOps TXN_MEMTABLE_TUPLES_TUPLES_AGED_OPS = {
	.shape = {.id = UINT64CONST(0x74786e000013), .locks = 1, .atomics = 4, .lists = 2},
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
 * table that leaves no next one, its byte pages.
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
} txn_memtable_tuples_reserve_frame_0_t;

/* txn_memtable_tuples_reserve_pad_0 is reserve's pad: the guard, the retirement, the hold, the try, and its sizes, while live. */
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

	*done = false;
	if (!host_delta_list_ListView_get(&keys, &frame->hold, newest, &name, frame->error) ||
		!host_delta_Hold_page(&frame->hold, host_delta_page_Page_named(name), &page, frame->error))
		return false;
	byte = txn_memtable_tuples_bytes(frame->key_len, true,
									 STD_ADD(u64, host_delta_PageRef_filled(&page), host_delta_PageRef_room(&page),
											 280, 40));
	if (txn_memtable_tuples_full(host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_USED, load),
								 host_shared_Record_atomic(record, TXN_MEMTABLE_TUPLES_RESERVED, load), count))
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
				if (!txn_memtable_table_waited(frame->io, full, false, frame->error))
				{
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
		txn_memtable_tuples_reserve_pad_0(frame);
	}
}

/*
 * txn_memtable_tuples_reserve reserves a slot of a storage's table for a key
 * before a window and appends the key's bytes to the table's newest byte
 * page, so that the finish only stores the slot. Each try takes what the
 * last one wanted before its hold, then, under the table's exclusive lock:
 * makes the storage's first table as its first maker when there is none,
 * grows the table first when one more slot would pass its load factor, puts
 * a fresh byte page on the table's list when the key doesn't fit the newest,
 * and counts the slot reserved.
 */
bool
txn_memtable_tuples_reserve(HostIoIo *io, HostSharedRecord state, const uint8 *key, size_t key_len,
							TxnMemtableTableReservation *result, HostError *error)
{
	txn_memtable_tuples_reserve_frame_0_t frame = {.io = io, .state = state, .key = key, .key_len = key_len,
	.result = result, .error = error};

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
			host_delta_Word_store(&word, TXN_MEMTABLE_TUPLES_DEAD);
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
