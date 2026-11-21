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
 * The delta's pages' and records' doors over the server's shared memory
 * (design item 14's S6): host's own sequences over the page directory, the
 * aged kinds' records, and the region of blocks, and this process's lists of
 * the pages it owns and the records it made and count of its holds, by which
 * the budget's wait refuses it and its exit gives back what its drops
 * didn't.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/host/delta/blocks.h"
#include "doltgres/pghost/delta.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/pghost/shared.h"
#include "doltgres/shmem/delta.h"
#include "doltgres/shmem/records.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/num.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "pg_calls.h"

#define RUST_FILE "pghost" STD_SEP "src" STD_SEP "delta.rs"

/* PGHOST_DELTA_OWNED_LOCK is OWNED's RwLock, which guards PGHOST_DELTA_OWNED. */
static StdRwLock PGHOST_DELTA_OWNED_LOCK = STD_RW_LOCK_NEW;

/* PGHOST_DELTA_OWNED are the pages this process owns: allocated, and neither linked nor given back yet. */
static StdVec PGHOST_DELTA_OWNED = STD_VEC_NEW;

/* PGHOST_DELTA_MADE_LOCK is MADE's RwLock, which guards PGHOST_DELTA_MADE. */
static StdRwLock PGHOST_DELTA_MADE_LOCK = STD_RW_LOCK_NEW;

/* PGHOST_DELTA_MADE are the records this process made, neither linked nor given back yet. */
static StdVec PGHOST_DELTA_MADE = STD_VEC_NEW;

/* PGHOST_DELTA_HOLDS is the number of this process's live holds. */
static StdAtomicU64 PGHOST_DELTA_HOLDS = STD_ATOMIC_U64_INIT(0);

/* pghost_delta_internal returns the fork's internal error with a message. */
static HostError
pghost_delta_internal(const char *message, size_t message_len)
{
	StdString	owned;

	std_string_from_Checked(&owned, message, message_len);
	return host_Error_fork((const uint8 *) "XX000", &owned);
}

/* pghost_delta_bytes returns the delta's memory in bytes, the setting's. */
size_t
pghost_delta_bytes(void)
{
	return STD_MUL(usize, (size_t) std_i32_unsigned_abs(pg_calls_settings_delta_memory()), 1024, 54, 5);
}

/* PghostDeltaPages is the server's page directory as host's sequences take it. */
typedef struct PghostDeltaPages
{
	char		unused;
} PghostDeltaPages;

/* PGHOST_DELTA_PAGES is the directory's one value, which the doors pass host. */
static PghostDeltaPages PGHOST_DELTA_PAGES;

StaticAssertDecl(SHMEM_DELTA_REGION_WORDS == HOST_DELTA_BLOCKS_WORDS, "the header keeps every word of the region's");

/* pghost_delta_which returns a block's word's place among its words. */
static size_t
pghost_delta_which(HostDeltaBlocksBlock word)
{
	switch (word)
	{
		case HOST_DELTA_BLOCKS_BLOCK_SLOTS:
			return 0;
		case HOST_DELTA_BLOCKS_BLOCK_MAP:
			return 1;
		case HOST_DELTA_BLOCKS_BLOCK_LINKS:
			return 2;
	}
	pg_unreachable();
}

/* pghost_delta_at returns a field's place among a slot's other words. */
static size_t
pghost_delta_at(HostDeltaDirectoryField field)
{
	switch (field)
	{
		case HOST_DELTA_DIRECTORY_FIELD_BODY:
			return 0;
		case HOST_DELTA_DIRECTORY_FIELD_SIZES:
			return 1;
		case HOST_DELTA_DIRECTORY_FIELD_MARK:
			return 2;
		case HOST_DELTA_DIRECTORY_FIELD_OWNER:
			return 3;
		case HOST_DELTA_DIRECTORY_FIELD_LOCK:
			return 4;
		case HOST_DELTA_DIRECTORY_FIELD_STAMP:
			return 5;
		case HOST_DELTA_DIRECTORY_FIELD_SHAPE:
			return 6;
	}
	pg_unreachable();
}

/* pghost_delta_name returns the name of the injection point a point of a sequence stops at. */
static const char *
pghost_delta_name(HostDeltaDirectoryPoint point)
{
	switch (point)
	{
		case HOST_DELTA_DIRECTORY_POINT_OPENING:
			return "doltgres-delta-opening";
		case HOST_DELTA_DIRECTORY_POINT_STAMPED:
			return "doltgres-delta-stamped";
		case HOST_DELTA_DIRECTORY_POINT_CLAIMED:
			return "doltgres-delta-claimed";
		case HOST_DELTA_DIRECTORY_POINT_PASSING:
			return "doltgres-delta-passing";
		case HOST_DELTA_DIRECTORY_POINT_WALKED:
			return "doltgres-delta-walked";
		case HOST_DELTA_DIRECTORY_POINT_STAMPING:
			return "doltgres-delta-stamping";
		case HOST_DELTA_DIRECTORY_POINT_OLDEST:
			return "doltgres-delta-oldest";
		case HOST_DELTA_DIRECTORY_POINT_WAITING:
			return "doltgres-delta-waiting";
		case HOST_DELTA_DIRECTORY_POINT_HINTING:
			return "doltgres-delta-hinting";
	}
	pg_unreachable();
}

/* pghost_delta_aged_kinds_iter is aged_kinds' iterator: the region's kinds from a number, filtered to the aged ones. */
typedef struct pghost_delta_aged_kinds_iter
{
	size_t		start;
	size_t		end;
} pghost_delta_aged_kinds_iter;

/* pghost_delta_aged_kinds returns the region's aged kinds, in the table's order. */
static pghost_delta_aged_kinds_iter
pghost_delta_aged_kinds(void)
{
	return (pghost_delta_aged_kinds_iter)
	{
		0, shmem_records_kinds()
	};
}

/* pghost_delta_aged_kinds_iter_next is the iterator's next: the next kind whose records are aged, false past the last. */
static bool
pghost_delta_aged_kinds_iter_next(pghost_delta_aged_kinds_iter *self, size_t *result)
{
	while (self->start < self->end)
	{
		size_t		kind = self->start;
		std_TupleU32U32_t aged;

		self->start = std_forward_unchecked_usize_Checked(self->start, 1);
		if (shmem_records_aged(kind, &aged))
		{
			*result = kind;
			return true;
		}
	}
	return false;
}

/* pghost_delta_kind_of returns the region's kind at a number among the aged kinds, which the region holds. */
size_t
pghost_delta_kind_of(uint32 aged)
{
	pghost_delta_aged_kinds_iter kinds = pghost_delta_aged_kinds();
	size_t		kind;

	for (size_t n = (size_t) aged;; n--)
	{
		if (!pghost_delta_aged_kinds_iter_next(&kinds, &kind))
			STD_OPTION_EXPECT_FAILED(107, 37, "an aged kind's number is one of the region's aged kinds");
		if (n == 0)
			return kind;
	}
}

/* pghost_delta_slot_of returns the directory's slot of a record at an index among an aged kind's records. */
static uint32
pghost_delta_slot_of(uint32 aged, uint32 index)
{
	std_TupleU32U32_t first;
	uint32		at = 0;

	if (shmem_records_aged(pghost_delta_kind_of(aged), &first))
		at = first.f0;
	return STD_ADD(u32, at, index, 112, 5);
}

/* pghost_delta_stamp_of returns the place among a record's atomics of the epoch of its retirement: the one before its last. */
static size_t
pghost_delta_stamp_of(size_t kind)
{
	return STD_SUB(usize, shmem_records_atomics(kind), 2, 117, 5);
}

/* pghost_delta_link_of returns the place among a record's atomics of its link in its kind's retired list: its last (bugs row 309). */
static size_t
pghost_delta_link_of(size_t kind)
{
	return shmem_records_atomics(kind) - 1;
}

/* pghost_delta_Pages_Directory_slots is the directory's count of slots. */
static uint32
pghost_delta_Pages_Directory_slots(void *self)
{
	(void) self;
	return shmem_delta_slots();
}

/* pghost_delta_Pages_Directory_budget is the delta's budget of bytes. */
static uint64
pghost_delta_Pages_Directory_budget(void *self)
{
	(void) self;
	return (uint64) pghost_delta_bytes();
}

/* pghost_delta_Pages_Directory_word loads a slot's generation-and-state word. */
static uint64
pghost_delta_Pages_Directory_word(void *self, uint32 slot)
{
	(void) self;
	return shmem_delta_word(slot);
}

/* pghost_delta_Pages_Directory_swap swaps a slot's word from the word loaded. */
static bool
pghost_delta_Pages_Directory_swap(void *self, uint32 slot, uint64 loaded, uint64 word)
{
	(void) self;
	return shmem_delta_swap(slot, loaded, word);
}

/* pghost_delta_Pages_Directory_load loads a field of a slot. */
static uint64
pghost_delta_Pages_Directory_load(void *self, uint32 slot, HostDeltaDirectoryField field)
{
	(void) self;
	return shmem_delta_load(slot, pghost_delta_at(field));
}

/* pghost_delta_Pages_Directory_store stores a field of a slot. */
static void
pghost_delta_Pages_Directory_store(void *self, uint32 slot, HostDeltaDirectoryField field, uint64 value)
{
	(void) self;
	shmem_delta_store(slot, pghost_delta_at(field), value);
}

/* pghost_delta_Pages_Directory_blocks is the region's count of blocks. */
static uint32
pghost_delta_Pages_Directory_blocks(void *self)
{
	(void) self;
	return shmem_delta_blocks();
}

/* pghost_delta_Pages_Directory_region loads a word of the region. */
static uint64
pghost_delta_Pages_Directory_region(void *self, HostDeltaBlocksRegion word)
{
	(void) self;
	return shmem_delta_region(host_delta_blocks_Region_index(word));
}

/* pghost_delta_Pages_Directory_set_region stores a word of the region. */
static void
pghost_delta_Pages_Directory_set_region(void *self, HostDeltaBlocksRegion word, uint64 value)
{
	(void) self;
	shmem_delta_set_region(host_delta_blocks_Region_index(word), value);
}

/* pghost_delta_Pages_Directory_block loads a word of a block. */
static uint64
pghost_delta_Pages_Directory_block(void *self, uint32 block, HostDeltaBlocksBlock word)
{
	(void) self;
	return shmem_delta_block(block, pghost_delta_which(word));
}

/* pghost_delta_Pages_Directory_set_block stores a word of a block. */
static void
pghost_delta_Pages_Directory_set_block(void *self, uint32 block, HostDeltaBlocksBlock word, uint64 value)
{
	(void) self;
	shmem_delta_set_block(block, pghost_delta_which(word), value);
}

/* pghost_delta_Pages_Directory_lock_region takes the region's lock. */
static void
pghost_delta_Pages_Directory_lock_region(void *self)
{
	(void) self;
	shmem_delta_lock_region();
}

/* pghost_delta_Pages_Directory_unlock_region releases the region's lock. */
static void
pghost_delta_Pages_Directory_unlock_region(void *self)
{
	(void) self;
	shmem_delta_unlock_region();
}

/* pghost_delta_Pages_Directory_frees is the count of the directory's frees. */
static uint64
pghost_delta_Pages_Directory_frees(void *self)
{
	(void) self;
	return shmem_delta_frees();
}

/* pghost_delta_Pages_Directory_freed counts a free. */
static void
pghost_delta_Pages_Directory_freed(void *self)
{
	(void) self;
	shmem_delta_freed();
}

/* pghost_delta_Pages_Directory_left is the count of the pages left. */
static uint64
pghost_delta_Pages_Directory_left(void *self)
{
	(void) self;
	return shmem_delta_left();
}

/* pghost_delta_Pages_Directory_leave counts a page left. */
static void
pghost_delta_Pages_Directory_leave(void *self)
{
	(void) self;
	shmem_delta_leave();
}

/* pghost_delta_Pages_Directory_settle settles the pages left from the count loaded. */
static void
pghost_delta_Pages_Directory_settle(void *self, uint64 loaded)
{
	(void) self;
	shmem_delta_settle(loaded);
}

/* pghost_delta_Pages_Directory_kept is the count of the bytes kept. */
static uint64
pghost_delta_Pages_Directory_kept(void *self)
{
	(void) self;
	return shmem_delta_kept();
}

/* pghost_delta_Pages_Directory_kept_swap swaps the bytes kept from the count loaded. */
static bool
pghost_delta_Pages_Directory_kept_swap(void *self, uint64 loaded, uint64 word)
{
	(void) self;
	return shmem_delta_kept_swap(loaded, word);
}

/* pghost_delta_Pages_Directory_waiting is the count of the budget's waiters. */
static uint64
pghost_delta_Pages_Directory_waiting(void *self)
{
	(void) self;
	return shmem_delta_waiting();
}

/* pghost_delta_Pages_Directory_count_waiter counts a waiter in or out. */
static void
pghost_delta_Pages_Directory_count_waiter(void *self, bool add)
{
	(void) self;
	shmem_delta_count_waiter(add);
}

/* pghost_delta_Pages_Directory_overshoot is the budget's overshoot. */
static uint64
pghost_delta_Pages_Directory_overshoot(void *self)
{
	(void) self;
	return shmem_delta_overshoot();
}

/* pghost_delta_Pages_Directory_overshoot_swap swaps the overshoot from the value loaded. */
static bool
pghost_delta_Pages_Directory_overshoot_swap(void *self, uint64 loaded, uint64 word)
{
	(void) self;
	return shmem_delta_overshoot_swap(loaded, word);
}

/* pghost_delta_Pages_Directory_hand is the directory's clock hand. */
static uint32
pghost_delta_Pages_Directory_hand(void *self)
{
	(void) self;
	return shmem_delta_hand();
}

/* pghost_delta_Pages_Directory_hint loads a word of the free hint. */
static uint64
pghost_delta_Pages_Directory_hint(void *self, uint32 word)
{
	(void) self;
	return shmem_delta_hint(word);
}

/* pghost_delta_Pages_Directory_hint_set sets a slot's bit of the free hint. */
static void
pghost_delta_Pages_Directory_hint_set(void *self, uint32 slot)
{
	(void) self;
	shmem_delta_hint_set(slot);
}

/* pghost_delta_Pages_Directory_hint_clear clears a slot's bit of the free hint. */
static void
pghost_delta_Pages_Directory_hint_clear(void *self, uint32 slot)
{
	(void) self;
	shmem_delta_hint_clear(slot);
}

/* pghost_delta_Pages_Directory_spent is the bytes spent, over the budget or within it. */
static uint64
pghost_delta_Pages_Directory_spent(void *self, bool over)
{
	(void) self;
	return shmem_delta_spent(over);
}

/* pghost_delta_Pages_Directory_spend counts a spend, over the budget or within it. */
static void
pghost_delta_Pages_Directory_spend(void *self, bool over)
{
	(void) self;
	shmem_delta_spend(over);
}

/* pghost_delta_Pages_Directory_lock takes the directory's allocation lock with pg's capability. */
static bool
pghost_delta_Pages_Directory_lock(void *self, const void *raise, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!shmem_delta_lock(raise, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_delta_Pages_Directory_unlock releases the directory's allocation lock. */
static void
pghost_delta_Pages_Directory_unlock(void *self)
{
	(void) self;
	shmem_delta_unlock();
}

/* pghost_delta_Pages_Directory_zero zeroes a block's body of a size. */
static void
pghost_delta_Pages_Directory_zero(void *self, uint64 body, HostDeltaPageSize size)
{
	(void) self;
	shmem_delta_zero(body, host_delta_page_Size_body(size));
}

/* pghost_delta_Pages_Directory_write writes bytes into a block's body at an offset. */
static void
pghost_delta_Pages_Directory_write(void *self, uint64 body, uint64 at, const uint8 *bytes, size_t bytes_len)
{
	(void) self;
	shmem_delta_write(body, at, bytes, bytes_len);
}

/* pghost_delta_Pages_Directory_set stores a word of a block's body. */
static void
pghost_delta_Pages_Directory_set(void *self, uint64 body, uint64 index, uint64 value)
{
	(void) self;
	shmem_delta_body_store(body, index, value);
}

/* pghost_delta_Pages_Directory_own_frame_0_t is own's state across its pad: the page it lists and the list's guard. */
typedef struct pghost_delta_Pages_Directory_own_frame_0_t
{
	HostDeltaPagePage name;
	StdRwLockWriteGuard owned;
	bool		owned_live;
} pghost_delta_Pages_Directory_own_frame_0_t;

/* pghost_delta_Pages_Directory_own_body_0 is own's body. */
static void
pghost_delta_Pages_Directory_own_body_0(void *frame_)
{
	pghost_delta_Pages_Directory_own_frame_0_t *frame = frame_;

	frame->owned = pghost_shared_own(&PGHOST_DELTA_OWNED_LOCK, &PGHOST_DELTA_OWNED);
	frame->owned_live = true;
	std_vec_push_Checked(frame->owned.data, &frame->name, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	frame->owned_live = false;
	std_RwLockWriteGuard_drop(&frame->owned);
}

/* pghost_delta_Pages_Directory_own_pad_0 is own's pad: the list's guard while it is live. */
static void
pghost_delta_Pages_Directory_own_pad_0(void *frame_)
{
	pghost_delta_Pages_Directory_own_frame_0_t *frame = frame_;

	if (frame->owned_live)
		std_RwLockWriteGuard_drop(&frame->owned);
}

/* pghost_delta_Pages_Directory_own lists a page this process owns. */
static void
pghost_delta_Pages_Directory_own(void *self, HostDeltaPagePage name)
{
	pghost_delta_Pages_Directory_own_frame_0_t frame = {.name = name};

	(void) self;
	std_landing(&frame, pghost_delta_Pages_Directory_own_body_0, pghost_delta_Pages_Directory_own_pad_0);
}

/* pghost_delta_Pages_Directory_disown_closure_0 is disown's retain: whether an owned page is another than the one disowned. */
static bool
pghost_delta_Pages_Directory_disown_closure_0(void *captures, const void *owned)
{
	return ((const HostDeltaPagePage *) owned)->f0 != ((const HostDeltaPagePage *) captures)->f0;
}

/* pghost_delta_Pages_Directory_disown unlists a page this process owned. */
static void
pghost_delta_Pages_Directory_disown(void *self, HostDeltaPagePage name)
{
	StdRwLockWriteGuard owned = pghost_shared_own(&PGHOST_DELTA_OWNED_LOCK, &PGHOST_DELTA_OWNED);

	(void) self;
	std_vec_retain_Checked(owned.data, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage), NULL,
						   pghost_delta_Pages_Directory_disown_closure_0, &name);
	std_RwLockWriteGuard_drop(&owned);
}

/* pghost_delta_Pages_Directory_retiring is the epoch retiring. */
static uint64
pghost_delta_Pages_Directory_retiring(void *self)
{
	(void) self;
	return shmem_records_epochs_retiring();
}

/* pghost_delta_Pages_Directory_retiring_now is the epoch retiring now, if one is. */
static bool
pghost_delta_Pages_Directory_retiring_now(void *self, uint64 *result)
{
	(void) self;
	return shmem_records_epochs_retiring_now(result);
}

/* pghost_delta_Pages_Directory_retired advances the epoch and releases the epoch lock. */
static void
pghost_delta_Pages_Directory_retired(void *self)
{
	(void) self;
	(void) shmem_records_epochs_advance();
	shmem_records_epochs_unlock();
}

/* pghost_delta_Pages_Directory_unretired releases the epoch lock. */
static void
pghost_delta_Pages_Directory_unretired(void *self)
{
	(void) self;
	shmem_records_epochs_unlock();
}

/* pghost_delta_Pages_Directory_epoch is the current epoch. */
static uint64
pghost_delta_Pages_Directory_epoch(void *self)
{
	(void) self;
	return shmem_records_epochs_current();
}

/* pghost_delta_Pages_Directory_oldest is the epoch passing. */
static uint64
pghost_delta_Pages_Directory_oldest(void *self)
{
	(void) self;
	return shmem_records_epochs_passing();
}

/* pghost_delta_Pages_Directory_stop stops at a point of a sequence through its injection point. */
static void
pghost_delta_Pages_Directory_stop(void *self, HostDeltaDirectoryPoint point)
{
	(void) self;
	shmem_delta_stop(pghost_delta_name(point));
}

/* pghost_delta_Pages_Directory_aged is the count of the region's aged kinds. */
static uint32
pghost_delta_Pages_Directory_aged(void *self)
{
	pghost_delta_aged_kinds_iter kinds = pghost_delta_aged_kinds();
	size_t		kind;
	size_t		count = 0;

	(void) self;
	while (pghost_delta_aged_kinds_iter_next(&kinds, &kind))
		count = std_add_usize(count, 1, STD_ACCUM_FILE, 206, 1);
	return (uint32) count;
}

/* pghost_delta_Pages_Directory_aged_kind returns the number among the aged kinds of a shape's kind. */
static bool
pghost_delta_Pages_Directory_aged_kind(void *self, uint64 id, uint32 *result)
{
	size_t		kind;
	pghost_delta_aged_kinds_iter kinds;
	size_t		aged;
	size_t		at = 0;

	(void) self;
	if (!shmem_records_kind(id, &kind))
		return false;
	kinds = pghost_delta_aged_kinds();
	while (pghost_delta_aged_kinds_iter_next(&kinds, &aged))
	{
		if (aged == kind)
		{
			*result = (uint32) at;
			return true;
		}
		at = std_Iterator_position_step(at);
	}
	return false;
}

/* pghost_delta_Pages_Directory_records is the count of an aged kind's records. */
static uint32
pghost_delta_Pages_Directory_records(void *self, uint32 kind)
{
	(void) self;
	return shmem_records_count(pghost_delta_kind_of(kind));
}

/* pghost_delta_Pages_Directory_record_word loads an aged record's directory word. */
static uint64
pghost_delta_Pages_Directory_record_word(void *self, uint32 kind, uint32 index)
{
	(void) self;
	return shmem_records_word(pghost_delta_slot_of(kind, index));
}

/* pghost_delta_Pages_Directory_record_swap swaps an aged record's directory word from the word loaded. */
static bool
pghost_delta_Pages_Directory_record_swap(void *self, uint32 kind, uint32 index, uint64 loaded, uint64 word)
{
	(void) self;
	return shmem_records_swap(pghost_delta_slot_of(kind, index), loaded, word);
}

/* pghost_delta_Pages_Directory_stamp loads the epoch of an aged record's retirement. */
static uint64
pghost_delta_Pages_Directory_stamp(void *self, uint32 kind, uint32 index)
{
	size_t		region_kind = pghost_delta_kind_of(kind);
	ShmemRecordsOp load = {SHMEM_RECORDS_OP_LOAD};

	(void) self;
	return shmem_records_atomic(region_kind, index, pghost_delta_stamp_of(region_kind), load);
}

/* pghost_delta_Pages_Directory_set_stamp stores the epoch of an aged record's retirement by exchanges. */
static void
pghost_delta_Pages_Directory_set_stamp(void *self, uint32 kind, uint32 index, uint64 stamp)
{
	size_t		region_kind = pghost_delta_kind_of(kind);
	ShmemRecordsOp load = {SHMEM_RECORDS_OP_LOAD};
	uint64		held = shmem_records_atomic(region_kind, index, pghost_delta_stamp_of(region_kind), load);

	(void) self;
	for (;;)
	{
		ShmemRecordsOp exchange = {SHMEM_RECORDS_OP_EXCHANGE, {.exchange = {held, stamp}}};
		uint64		before = shmem_records_atomic(region_kind, index, pghost_delta_stamp_of(region_kind), exchange);

		if (before == held)
			return;
		held = before;
	}
}

/* pghost_delta_Pages_Directory_record_hand is an aged kind's hand. */
static uint32
pghost_delta_Pages_Directory_record_hand(void *self, uint32 kind)
{
	(void) self;
	return shmem_records_hand(pghost_delta_kind_of(kind));
}

/* pghost_delta_Pages_Directory_clear clears an aged record's flags. */
static void
pghost_delta_Pages_Directory_clear(void *self, uint32 kind, uint32 index)
{
	(void) self;
	shmem_records_clear(pghost_delta_kind_of(kind), index);
}

/* pghost_delta_Pages_Directory_made_frame_0_t is made's state across its pad: the record it lists and the list's guard. */
typedef struct pghost_delta_Pages_Directory_made_frame_0_t
{
	std_TupleU32U32_t item;
	StdRwLockWriteGuard made;
	bool		made_live;
} pghost_delta_Pages_Directory_made_frame_0_t;

/* pghost_delta_Pages_Directory_made_body_0 is made's body. */
static void
pghost_delta_Pages_Directory_made_body_0(void *frame_)
{
	pghost_delta_Pages_Directory_made_frame_0_t *frame = frame_;

	frame->made = pghost_shared_own(&PGHOST_DELTA_MADE_LOCK, &PGHOST_DELTA_MADE);
	frame->made_live = true;
	std_vec_push_Checked(frame->made.data, &frame->item, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	frame->made_live = false;
	std_RwLockWriteGuard_drop(&frame->made);
}

/* pghost_delta_Pages_Directory_made_pad_0 is made's pad: the list's guard while it is live. */
static void
pghost_delta_Pages_Directory_made_pad_0(void *frame_)
{
	pghost_delta_Pages_Directory_made_frame_0_t *frame = frame_;

	if (frame->made_live)
		std_RwLockWriteGuard_drop(&frame->made);
}

/* pghost_delta_Pages_Directory_made lists a record this process made. */
static void
pghost_delta_Pages_Directory_made(void *self, uint32 kind, uint32 index)
{
	pghost_delta_Pages_Directory_made_frame_0_t frame = {.item = {kind, index}};

	(void) self;
	std_landing(&frame, pghost_delta_Pages_Directory_made_body_0, pghost_delta_Pages_Directory_made_pad_0);
}

/* pghost_delta_Pages_Directory_unmade_closure_0 is unmade's retain: whether a made record is another than the one unmade. */
static bool
pghost_delta_Pages_Directory_unmade_closure_0(void *captures, const void *made)
{
	const std_TupleU32U32_t *a = made;
	const std_TupleU32U32_t *b = captures;

	return !(a->f0 == b->f0 && a->f1 == b->f1);
}

/* pghost_delta_Pages_Directory_unmade unlists a record this process made. */
static void
pghost_delta_Pages_Directory_unmade(void *self, uint32 kind, uint32 index)
{
	StdRwLockWriteGuard made = pghost_shared_own(&PGHOST_DELTA_MADE_LOCK, &PGHOST_DELTA_MADE);
	std_TupleU32U32_t item = {kind, index};

	(void) self;
	std_vec_retain_Checked(made.data, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t), NULL,
						   pghost_delta_Pages_Directory_unmade_closure_0, &item);
	std_RwLockWriteGuard_drop(&made);
}

/* pghost_delta_Pages_Directory_retired_head loads the retired list's head. */
static uint64
pghost_delta_Pages_Directory_retired_head(void *self)
{
	(void) self;
	return shmem_delta_retired();
}

/* pghost_delta_Pages_Directory_retired_swap stores the retired list's head over the one loaded. */
static bool
pghost_delta_Pages_Directory_retired_swap(void *self, uint64 loaded, uint64 word)
{
	(void) self;
	return shmem_delta_retired_swap(loaded, word);
}

/* pghost_delta_Pages_Directory_link loads a slot's link in the retired list. */
static uint32
pghost_delta_Pages_Directory_link(void *self, uint32 slot)
{
	(void) self;
	return shmem_delta_link(slot);
}

/* pghost_delta_Pages_Directory_set_link stores a slot's link in the retired list. */
static void
pghost_delta_Pages_Directory_set_link(void *self, uint32 slot, uint32 value)
{
	(void) self;
	shmem_delta_set_link(slot, value);
}

/* pghost_delta_Pages_Directory_record_retired_head loads an aged kind's retired list's head. */
static uint64
pghost_delta_Pages_Directory_record_retired_head(void *self, uint32 kind)
{
	(void) self;
	return shmem_records_retired(pghost_delta_kind_of(kind));
}

/* pghost_delta_Pages_Directory_record_retired_swap stores an aged kind's retired list's head over the one loaded. */
static bool
pghost_delta_Pages_Directory_record_retired_swap(void *self, uint32 kind, uint64 loaded, uint64 word)
{
	(void) self;
	return shmem_records_retired_swap(pghost_delta_kind_of(kind), loaded, word);
}

/* pghost_delta_Pages_Directory_record_link loads an aged record's link in its kind's retired list. */
static uint64
pghost_delta_Pages_Directory_record_link(void *self, uint32 kind, uint32 index)
{
	size_t		region_kind = pghost_delta_kind_of(kind);
	ShmemRecordsOp load = {SHMEM_RECORDS_OP_LOAD};

	(void) self;
	return shmem_records_atomic(region_kind, index, pghost_delta_link_of(region_kind), load);
}

/* pghost_delta_Pages_Directory_set_record_link stores an aged record's link in its kind's retired list by exchanges. */
static void
pghost_delta_Pages_Directory_set_record_link(void *self, uint32 kind, uint32 index, uint64 value)
{
	size_t		region_kind = pghost_delta_kind_of(kind);
	ShmemRecordsOp load = {SHMEM_RECORDS_OP_LOAD};
	uint64		held = shmem_records_atomic(region_kind, index, pghost_delta_link_of(region_kind), load);

	(void) self;
	for (;;)
	{
		ShmemRecordsOp exchange = {SHMEM_RECORDS_OP_EXCHANGE, {.exchange = {held, value}}};
		uint64		before = shmem_records_atomic(region_kind, index, pghost_delta_link_of(region_kind), exchange);

		if (before == held)
			return;
		held = before;
	}
}

/* pghost_delta_Pages_Directory_lock_pass takes the free pass's lock. */
static void
pghost_delta_Pages_Directory_lock_pass(void *self)
{
	(void) self;
	shmem_delta_lock_pass();
}

/* pghost_delta_Pages_Directory_unlock_pass releases the free pass's lock. */
static void
pghost_delta_Pages_Directory_unlock_pass(void *self)
{
	(void) self;
	shmem_delta_unlock_pass();
}

/* PGHOST_DELTA_PAGES_DIRECTORY_OPS is Pages' Directory table. */
static const HostDeltaDirectoryDirectoryOps PGHOST_DELTA_PAGES_DIRECTORY_OPS = {
	.slots = pghost_delta_Pages_Directory_slots,
	.budget = pghost_delta_Pages_Directory_budget,
	.word = pghost_delta_Pages_Directory_word,
	.swap = pghost_delta_Pages_Directory_swap,
	.load = pghost_delta_Pages_Directory_load,
	.store = pghost_delta_Pages_Directory_store,
	.blocks = pghost_delta_Pages_Directory_blocks,
	.region = pghost_delta_Pages_Directory_region,
	.set_region = pghost_delta_Pages_Directory_set_region,
	.block = pghost_delta_Pages_Directory_block,
	.set_block = pghost_delta_Pages_Directory_set_block,
	.lock_region = pghost_delta_Pages_Directory_lock_region,
	.unlock_region = pghost_delta_Pages_Directory_unlock_region,
	.frees = pghost_delta_Pages_Directory_frees,
	.freed = pghost_delta_Pages_Directory_freed,
	.left = pghost_delta_Pages_Directory_left,
	.leave = pghost_delta_Pages_Directory_leave,
	.settle = pghost_delta_Pages_Directory_settle,
	.kept = pghost_delta_Pages_Directory_kept,
	.kept_swap = pghost_delta_Pages_Directory_kept_swap,
	.waiting = pghost_delta_Pages_Directory_waiting,
	.count_waiter = pghost_delta_Pages_Directory_count_waiter,
	.overshoot = pghost_delta_Pages_Directory_overshoot,
	.overshoot_swap = pghost_delta_Pages_Directory_overshoot_swap,
	.hand = pghost_delta_Pages_Directory_hand,
	.hint = pghost_delta_Pages_Directory_hint,
	.hint_set = pghost_delta_Pages_Directory_hint_set,
	.hint_clear = pghost_delta_Pages_Directory_hint_clear,
	.spent = pghost_delta_Pages_Directory_spent,
	.spend = pghost_delta_Pages_Directory_spend,
	.lock = pghost_delta_Pages_Directory_lock,
	.unlock = pghost_delta_Pages_Directory_unlock,
	.zero = pghost_delta_Pages_Directory_zero,
	.write = pghost_delta_Pages_Directory_write,
	.set = pghost_delta_Pages_Directory_set,
	.own = pghost_delta_Pages_Directory_own,
	.disown = pghost_delta_Pages_Directory_disown,
	.retiring = pghost_delta_Pages_Directory_retiring,
	.retiring_now = pghost_delta_Pages_Directory_retiring_now,
	.retired = pghost_delta_Pages_Directory_retired,
	.unretired = pghost_delta_Pages_Directory_unretired,
	.epoch = pghost_delta_Pages_Directory_epoch,
	.oldest = pghost_delta_Pages_Directory_oldest,
	.stop = pghost_delta_Pages_Directory_stop,
	.aged = pghost_delta_Pages_Directory_aged,
	.aged_kind = pghost_delta_Pages_Directory_aged_kind,
	.records = pghost_delta_Pages_Directory_records,
	.record_word = pghost_delta_Pages_Directory_record_word,
	.record_swap = pghost_delta_Pages_Directory_record_swap,
	.stamp = pghost_delta_Pages_Directory_stamp,
	.set_stamp = pghost_delta_Pages_Directory_set_stamp,
	.record_hand = pghost_delta_Pages_Directory_record_hand,
	.clear = pghost_delta_Pages_Directory_clear,
	.made = pghost_delta_Pages_Directory_made,
	.unmade = pghost_delta_Pages_Directory_unmade,
	.retired_head = pghost_delta_Pages_Directory_retired_head,
	.retired_swap = pghost_delta_Pages_Directory_retired_swap,
	.link = pghost_delta_Pages_Directory_link,
	.set_link = pghost_delta_Pages_Directory_set_link,
	.record_retired_head = pghost_delta_Pages_Directory_record_retired_head,
	.record_retired_swap = pghost_delta_Pages_Directory_record_retired_swap,
	.record_link = pghost_delta_Pages_Directory_record_link,
	.set_record_link = pghost_delta_Pages_Directory_set_record_link,
	.lock_pass = pghost_delta_Pages_Directory_lock_pass,
	.unlock_pass = pghost_delta_Pages_Directory_unlock_pass,
};

/* PGHOST_DELTA_DIRECTORY is &Pages as host's sequences take it. */
#define PGHOST_DELTA_DIRECTORY ((HostDeltaDirectoryDirectory) {&PGHOST_DELTA_PAGES_DIRECTORY_OPS, &PGHOST_DELTA_PAGES})

/* pghost_delta_aged_of returns the number among the aged kinds of a kind of the server's table, refusing one that isn't aged. */
static bool
pghost_delta_aged_of(const HostSharedKind *kind, uint32 *result, HostError *error)
{
	static const char message[] = "a shape freed by the delta's epochs isn't in the server's table";

	if (pghost_delta_Pages_Directory_aged_kind(&PGHOST_DELTA_PAGES, kind->id, result))
		return true;
	*error = pghost_delta_internal(message, sizeof(message) - 1);
	return false;
}

/*
 * pghost_delta_make makes a record of an aged kind, after it makes room for
 * it in this process's list, and returns its place and location, or the
 * location of the record of that name.
 */
bool
pghost_delta_make(const HostSharedKind *kind, const char *name, size_t name_len, bool *result_ok,
				  uint64 *result_0, uint64 *result_1, uint64 *result_err, HostError *error)
{
	uint32		aged;
	StdRwLockWriteGuard made;
	bool		reserved;
	uint32		index;

	if (!pghost_delta_aged_of(kind, &aged, error))
		return false;
	made = pghost_shared_own(&PGHOST_DELTA_MADE_LOCK, &PGHOST_DELTA_MADE);
	reserved = std_vec_try_reserve(made.data, 1, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	std_RwLockWriteGuard_drop(&made);
	if (!reserved)
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	if (!host_delta_directory_make(PGHOST_DELTA_DIRECTORY, aged, name, name_len, result_ok, &index, result_1,
								   result_err, error))
		return false;
	if (*result_ok)
		*result_0 = pghost_shared_placed_aged(aged, index);
	return true;
}

/* pghost_delta_record returns the place of the record of an aged kind at a location, open under an epoch. */
bool
pghost_delta_record(const HostSharedKind *kind, uint64 location, uint64 epoch, uint64 *result, HostError *error)
{
	uint32		aged;
	uint32		index;

	if (!pghost_delta_aged_of(kind, &aged, error))
		return false;
	if (!host_delta_directory_open_record(PGHOST_DELTA_DIRECTORY, aged, location, epoch, &index, error))
		return false;
	*result = pghost_shared_placed_aged(aged, index);
	return true;
}

/* pghost_delta_made_at returns the number among the aged kinds and the index of a record at a place. */
static void
pghost_delta_made_at(uint64 place, uint32 *result_0, uint32 *result_1)
{
	size_t		kind;
	uint32		index;
	pghost_delta_aged_kinds_iter kinds = pghost_delta_aged_kinds();
	size_t		aged;
	size_t		at = SIZE_MAX;
	size_t		count = 0;

	pghost_shared_placed(place, &kind, &index);
	while (pghost_delta_aged_kinds_iter_next(&kinds, &aged))
	{
		if (aged == kind)
		{
			at = count;
			break;
		}
		count = std_Iterator_position_step(count);
	}
	*result_0 = (uint32) at;
	*result_1 = index;
}

/* pghost_delta_publish publishes a record this process made. */
void
pghost_delta_publish(uint64 place)
{
	uint32		aged;
	uint32		index;

	pghost_delta_made_at(place, &aged, &index);
	host_delta_directory_publish(PGHOST_DELTA_DIRECTORY, aged, index);
}

/* pghost_delta_unmake unmakes a record this process made, and does nothing for one it linked or gave back. */
void
pghost_delta_unmake(uint64 place)
{
	std_TupleU32U32_t made_at;
	StdRwLockWriteGuard made;
	bool		contains = false;

	pghost_delta_made_at(place, &made_at.f0, &made_at.f1);
	made = pghost_shared_own(&PGHOST_DELTA_MADE_LOCK, &PGHOST_DELTA_MADE);
	for (size_t i = 0, i_len = ((StdVec *) made.data)->len; i < i_len; i++)
	{
		const std_TupleU32U32_t *item = &STD_VEC_AT((StdVec *) made.data, std_TupleU32U32_t, i);

		if (item->f0 == made_at.f0 && item->f1 == made_at.f1)
		{
			contains = true;
			break;
		}
	}
	std_RwLockWriteGuard_drop(&made);
	if (contains)
		host_delta_directory_unmake(PGHOST_DELTA_DIRECTORY, made_at.f0, made_at.f1);
}

/* pghost_delta_hold holds the delta's pages at the current epoch, counting the hold. */
bool
pghost_delta_hold(const HostMayRaise *raise, uint64 *result, HostError *error)
{
	PgPgError	error_;

	if (!shmem_records_epochs_read(pghost_pg_raise(raise), result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	(void) std_AtomicU64_fetch_add_relaxed(&PGHOST_DELTA_HOLDS, 1);
	return true;
}

/* pghost_delta_release releases a hold at an epoch, uncounting it. */
void
pghost_delta_release(uint64 epoch)
{
	shmem_records_epochs_end(epoch);
	(void) std_AtomicU64_fetch_sub_relaxed(&PGHOST_DELTA_HOLDS, 1);
	host_delta_directory_released(PGHOST_DELTA_DIRECTORY, epoch);
}

/* pghost_delta_ended ends a reader at an epoch that is no hold. */
void
pghost_delta_ended(uint64 epoch)
{
	shmem_records_epochs_end(epoch);
	host_delta_directory_released(PGHOST_DELTA_DIRECTORY, epoch);
}

/* pghost_delta_open opens a page under an epoch. */
bool
pghost_delta_open(HostDeltaPagePage name, uint64 epoch, HostDeltaDirectoryOpened *result, HostError *error)
{
	return host_delta_directory_open(PGHOST_DELTA_DIRECTORY, name, epoch, result, error);
}

/* pghost_delta_bytes_of returns bytes of an opened page, mapped from the region. */
bool
pghost_delta_bytes_of(const HostDeltaDirectoryOpened *opened, uint64 offset, uint64 len, HostCacheBytes *result,
					  HostError *error)
{
	uint64		at;
	size_t		bytes_len;
	const uint8 *bytes;

	if (!host_delta_directory_readable(PGHOST_DELTA_DIRECTORY, opened, offset, len, &at, error))
		return false;
	bytes = shmem_delta_bytes(opened->body, at, len, &bytes_len);
	result->tag = HOST_CACHE_BYTES_MAPPED;
	result->u.mapped.f0 = bytes;
	result->u.mapped.f0_len = bytes_len;
	return true;
}

/* pghost_delta_room returns the room an opened page has. */
uint64
pghost_delta_room(const HostDeltaDirectoryOpened *opened)
{
	return host_delta_directory_room(PGHOST_DELTA_DIRECTORY, opened);
}

/* pghost_delta_filled returns the bytes an opened page holds. */
uint64
pghost_delta_filled(const HostDeltaDirectoryOpened *opened)
{
	return host_delta_directory_filled(PGHOST_DELTA_DIRECTORY, opened);
}

/* pghost_delta_word does an operation on a word of a block's body and returns the value it read, zero for a store. */
uint64
pghost_delta_word(uint64 body, uint64 index, HostDeltaWordOp op)
{
	ShmemRecordsOp records_op = {0};

	switch (op.tag)
	{
		case HOST_DELTA_WORD_OP_LOAD:
			records_op.tag = SHMEM_RECORDS_OP_LOAD;
			return shmem_delta_body_word(body, index, records_op);
		case HOST_DELTA_WORD_OP_STORE:
			shmem_delta_body_store(body, index, op.u.store);
			return 0;
		case HOST_DELTA_WORD_OP_EXCHANGE:
			records_op.tag = SHMEM_RECORDS_OP_EXCHANGE;
			records_op.u.exchange.f0 = op.u.exchange.f0;
			records_op.u.exchange.f1 = op.u.exchange.f1;
			return shmem_delta_body_word(body, index, records_op);
		case HOST_DELTA_WORD_OP_ADD:
			records_op.tag = SHMEM_RECORDS_OP_ADD;
			records_op.u.add = op.u.add;
			return shmem_delta_body_word(body, index, records_op);
	}
	pg_unreachable();
}

/* pghost_delta_alloc allocates pages of sizes, after it makes room for them in this process's list. */
bool
pghost_delta_alloc(const HostMayRaise *raise, const HostDeltaPageSize *sizes, size_t sizes_len, StdVec *names,
				   bool *has_result, uint64 *result, HostError *error)
{
	StdRwLockWriteGuard owned = pghost_shared_own(&PGHOST_DELTA_OWNED_LOCK, &PGHOST_DELTA_OWNED);
	bool		reserved = std_vec_try_reserve(owned.data, sizes_len, sizeof(HostDeltaPagePage),
											   _Alignof(HostDeltaPagePage));

	std_RwLockWriteGuard_drop(&owned);
	if (!reserved)
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	return host_delta_directory_alloc(PGHOST_DELTA_DIRECTORY, pghost_pg_raise(raise), sizes, sizes_len, names,
									  has_result, result, error);
}

/* pghost_delta_alloc_replacing allocates a page in another's place, after it makes room for it in this process's list. */
bool
pghost_delta_alloc_replacing(const HostMayRaise *raise, HostDeltaPageSize size, HostDeltaPagePage replaced,
							 StdVec *names, bool *has_result, uint64 *result, HostError *error)
{
	StdRwLockWriteGuard owned = pghost_shared_own(&PGHOST_DELTA_OWNED_LOCK, &PGHOST_DELTA_OWNED);
	bool		reserved = std_vec_try_reserve(owned.data, 1, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));

	std_RwLockWriteGuard_drop(&owned);
	if (!reserved)
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	return host_delta_directory_alloc_replacing(PGHOST_DELTA_DIRECTORY, pghost_pg_raise(raise), size, replaced,
												names, has_result, result, error);
}

/* pghost_delta_give_back gives back a page this process owns, and does nothing for one it linked. */
void
pghost_delta_give_back(HostDeltaPagePage name)
{
	StdRwLockWriteGuard owned_list = pghost_shared_own(&PGHOST_DELTA_OWNED_LOCK, &PGHOST_DELTA_OWNED);
	bool		owned = false;

	for (size_t i = 0, i_len = ((StdVec *) owned_list.data)->len; i < i_len; i++)
	{
		if (STD_VEC_AT((StdVec *) owned_list.data, HostDeltaPagePage, i).f0 == name.f0)
		{
			owned = true;
			break;
		}
	}
	std_RwLockWriteGuard_drop(&owned_list);
	if (owned)
		host_delta_directory_give_back(PGHOST_DELTA_DIRECTORY, name);
}

/* pghost_delta_owned_append appends a record to a page this process owns. */
bool
pghost_delta_owned_append(HostDeltaPagePage name, const uint8 *record, size_t record_len, uint32 *result,
						  HostError *error)
{
	return host_delta_directory_owned_append(PGHOST_DELTA_DIRECTORY, name, record, record_len, result, error);
}

/* pghost_delta_owned_store stores a word of a page this process owns. */
bool
pghost_delta_owned_store(HostDeltaPagePage name, uint64 index, uint64 value, HostError *error)
{
	return host_delta_directory_owned_store(PGHOST_DELTA_DIRECTORY, name, index, value, error);
}

/* pghost_delta_link links a page to its owner. */
void
pghost_delta_link(HostDeltaPagePage name, HostDeltaDirectoryOwner owner)
{
	host_delta_directory_link(PGHOST_DELTA_DIRECTORY, name, owner);
}

/* pghost_delta_append appends a record to an opened page for its owner. */
bool
pghost_delta_append(const HostDeltaDirectoryOpened *opened, HostDeltaDirectoryOwner owner, const uint8 *record,
					size_t record_len, uint32 *result, HostError *error)
{
	return host_delta_directory_append(PGHOST_DELTA_DIRECTORY, opened, owner, record, record_len, result, error);
}

/* pghost_delta_seal seals an opened page for its owner. */
bool
pghost_delta_seal(const HostDeltaDirectoryOpened *opened, HostDeltaDirectoryOwner owner, HostError *error)
{
	return host_delta_directory_seal(PGHOST_DELTA_DIRECTORY, opened, owner, error);
}

/* pghost_delta_retire retires records and pages the delta no longer holds. */
void
pghost_delta_retire(const HostDeltaLeaving *records, size_t records_len, const HostDeltaPagePage *names,
					size_t names_len)
{
	host_delta_directory_retire(PGHOST_DELTA_DIRECTORY, records, records_len, names, names_len);
}

/* pghost_delta_pass passes the delta's epoch on. */
void
pghost_delta_pass(void)
{
	host_delta_directory_pass(PGHOST_DELTA_DIRECTORY);
}

/* pghost_delta_used returns the delta's bytes in use. */
uint64
pghost_delta_used(void)
{
	return host_delta_blocks_used(PGHOST_DELTA_DIRECTORY);
}

/* PGHOST_DELTA_RECOVERY_FOLD is the OnceLock of the server's fold in recovery. */
static StdOnceLock PGHOST_DELTA_RECOVERY_FOLD;

/* PGHOST_DELTA_RECOVERY_FOLD_VALUE is PGHOST_DELTA_RECOVERY_FOLD's value. */
static PghostDeltaRecoveryFold PGHOST_DELTA_RECOVERY_FOLD_VALUE;

/* pghost_delta_recovery_fold_closure_0 is get_or_init's closure: the fold given. */
static void
pghost_delta_recovery_fold_closure_0(void *captures, void *result)
{
	*(PghostDeltaRecoveryFold *) result = *(const PghostDeltaRecoveryFold *) captures;
}

/* pghost_delta_recovery_fold installs the server's fold in recovery, which a wait at the budget runs in a startup process. */
void
pghost_delta_recovery_fold(PghostDeltaRecoveryFold fold)
{
	std_once_lock_get_or_init(&PGHOST_DELTA_RECOVERY_FOLD, &PGHOST_DELTA_RECOVERY_FOLD_VALUE,
							  pghost_delta_recovery_fold_closure_0, &fold);
}

/*
 * pghost_delta_wait waits at the delta's budget for frees past a count,
 * refusing a backend that holds the delta's pages, owns one, or owns an
 * unlinked record, and folding in a startup process with no launcher.
 */
bool
pghost_delta_wait(const HostMayRaise *raise, uint64 frees, HostError *error)
{
	StdRwLockWriteGuard list;
	bool		empty;
	size_t		paused;
	bool		waited;
	PgPgError	error_;

	if (std_AtomicU64_load_relaxed(&PGHOST_DELTA_HOLDS) != 0)
	{
		static const char message[] = "a backend that has a hold on the delta's pages waited at the delta's budget";

		*error = pghost_delta_internal(message, sizeof(message) - 1);
		return false;
	}
	list = pghost_shared_own(&PGHOST_DELTA_OWNED_LOCK, &PGHOST_DELTA_OWNED);
	empty = ((StdVec *) list.data)->len == 0;
	std_RwLockWriteGuard_drop(&list);
	if (!empty)
	{
		static const char message[] = "a backend that owns a page of the delta waited at the delta's budget";

		*error = pghost_delta_internal(message, sizeof(message) - 1);
		return false;
	}
	list = pghost_shared_own(&PGHOST_DELTA_MADE_LOCK, &PGHOST_DELTA_MADE);
	empty = ((StdVec *) list.data)->len == 0;
	std_RwLockWriteGuard_drop(&list);
	if (!empty)
	{
		static const char message[] =
			"a backend that owns an unlinked record of the delta waited at the delta's budget";

		*error = pghost_delta_internal(message, sizeof(message) - 1);
		return false;
	}
	if (std_once_lock_get(&PGHOST_DELTA_RECOVERY_FOLD) && !shmem_delta_launcher_running())
	{
		HostIoIo	io = pghost_io(pghost_pg_raise(raise));
		bool		folded;

		if (!PGHOST_DELTA_RECOVERY_FOLD_VALUE(&io, &folded, error))
			return false;
		if (folded)
			return true;
	}
	paused = shmem_records_epochs_paused();
	host_delta_directory_pass(PGHOST_DELTA_DIRECTORY);
	host_delta_directory_waiting(PGHOST_DELTA_DIRECTORY);
	waited = shmem_delta_wait(pghost_pg_raise(raise), frees, &error_);
	if (!waited)
		*error = pghost_held(&error_);
	host_delta_directory_woken(PGHOST_DELTA_DIRECTORY);
	shmem_records_epochs_resumed(paused);
	return waited;
}

/* pghost_delta_exit_frame_0_t is exit's state across its pad: the for loop's IntoIter, while it is live. */
typedef struct pghost_delta_exit_frame_0_t
{
	StdVecIntoIter iter;
	bool		iter_live;
} pghost_delta_exit_frame_0_t;

/* pghost_delta_exit_body_0 is exit's body: MADE's taken records unmade, then OWNED's taken pages given back. */
static void
pghost_delta_exit_body_0(void *frame_)
{
	pghost_delta_exit_frame_0_t *frame = frame_;
	StdRwLockWriteGuard list = pghost_shared_own(&PGHOST_DELTA_MADE_LOCK, &PGHOST_DELTA_MADE);
	StdVec		made = *(StdVec *) list.data;
	StdVec		left;
	std_TupleU32U32_t item;
	HostDeltaPagePage name;

	*(StdVec *) list.data = (StdVec) STD_VEC_NEW;
	std_RwLockWriteGuard_drop(&list);
	frame->iter = std_Vec_IntoIterator_into_iter(&made, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t),
												 NULL);
	frame->iter_live = true;
	while (std_VecIntoIter_Iterator_next(&frame->iter, &item))
		host_delta_directory_unmake(PGHOST_DELTA_DIRECTORY, item.f0, item.f1);
	frame->iter_live = false;
	std_VecIntoIter_drop(&frame->iter);
	list = pghost_shared_own(&PGHOST_DELTA_OWNED_LOCK, &PGHOST_DELTA_OWNED);
	left = *(StdVec *) list.data;
	*(StdVec *) list.data = (StdVec) STD_VEC_NEW;
	std_RwLockWriteGuard_drop(&list);
	frame->iter = std_Vec_IntoIterator_into_iter(&left, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage), NULL);
	frame->iter_live = true;
	while (std_VecIntoIter_Iterator_next(&frame->iter, &name))
		host_delta_directory_give_back(PGHOST_DELTA_DIRECTORY, name);
	frame->iter_live = false;
	std_VecIntoIter_drop(&frame->iter);
}

/* pghost_delta_exit_pad_0 is exit's pad: the for loop's IntoIter's drop while it is live, its buffer freed. */
static void
pghost_delta_exit_pad_0(void *frame_)
{
	pghost_delta_exit_frame_0_t *frame = frame_;

	if (frame->iter_live)
		std_VecIntoIter_drop(&frame->iter);
}

/* pghost_delta_exit unmakes the records this process made and gives back the pages it owns, from its exit callback. */
void
pghost_delta_exit(void)
{
	pghost_delta_exit_frame_0_t frame = {0};

	std_landing(&frame, pghost_delta_exit_body_0, pghost_delta_exit_pad_0);
}

#endif							/* FRONTEND */
