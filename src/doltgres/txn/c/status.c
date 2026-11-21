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
 * The commit status table: each top-level transaction that wrote the fork's
 * storage with where its commit record ends, in an open-addressing table of
 * words of the delta's pages. An entry is three words: its xid, its outcome,
 * and the count of what names it. Its root is a single record of txn's, sized
 * from delta_memory, whose run of words names the segments, each a delta page
 * of 2,048 places; one short lock guards it, shared by a lookup, a take of
 * room, a removal, and a count's add, and exclusive for a growth's copy.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/delta/page.h"
#include "doltgres/host/local.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"
#ifdef DOLTGRES_TEST_TXN
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "status.rs"

/*
 * TXN_STATUS_LSNS is the first outcome word that isn't a commit record's end,
 * and the bit that marks a process's number.
 */
#define TXN_STATUS_LSNS (UINT64CONST(1) << 62)

/* TXN_STATUS_SLOTS_PER_SEGMENT is the count of entries a slot segment holds. */
#define TXN_STATUS_SLOTS_PER_SEGMENT UINT64CONST(2048)

/* TXN_STATUS_SLOT_MASK is the free list head's bits that hold a slot's index plus one; the rest hold its tag. */
#define TXN_STATUS_SLOT_MASK ((UINT64CONST(1) << 37) - 1)

/* TXN_STATUS_SLOT_WORDS is the count of an entry's words in its slot: its xid, its outcome, and its count. */
#define TXN_STATUS_SLOT_WORDS UINT64CONST(3)

/* TXN_STATUS_PLACES is the count of places an index segment holds. */
#define TXN_STATUS_PLACES UINT64CONST(3072)

/* TXN_STATUS_PLACE_WORDS is the count of an index place's words: its xid and its slot's index plus one. */
#define TXN_STATUS_PLACE_WORDS UINT64CONST(2)

/* TXN_STATUS_DELTA_PAGES_PER_SEGMENT is the count of the delta's 1 KiB page slots one segment's 48 KiB takes. */
#define TXN_STATUS_DELTA_PAGES_PER_SEGMENT 48

/*
 * The table's one lock and its four atomics: the index's capacity, its places
 * taken, the slots handed out, the free list's head.
 */
#define TXN_STATUS_TABLE_LOCK 0
#define TXN_STATUS_CAPACITY 0
#define TXN_STATUS_TAKEN 1
#define TXN_STATUS_HIGH 2
#define TXN_STATUS_FREE_HEAD 3

/* txn_status_SEGMENT is a slot segment's and an index segment's size: a page of words alone, 48 KiB. */
static const HostDeltaPageSize txn_status_SEGMENT = {.bytes = 0,
.words = (uint32) (TXN_STATUS_SLOTS_PER_SEGMENT * TXN_STATUS_SLOT_WORDS)};

/*
 * txn_status_Table_Single_words returns the count of the table's run of words: three
 * for each segment of the delta, and two.
 */
static size_t
txn_status_Table_Single_words(const HostSharedLimits *limits)
{
	return 3 * ((limits->delta_pages + TXN_STATUS_DELTA_PAGES_PER_SEGMENT - 1) / TXN_STATUS_DELTA_PAGES_PER_SEGMENT) + 2;
}

/*
 * TXN_STATUS_TABLE_SINGLE_OPS is the commit status table's root, a single
 * record of one lock and four atomics with its run of segments' names.
 */
const HostSharedSingleOps TXN_STATUS_TABLE_SINGLE_OPS = {
	.shape = {.id = UINT64CONST(0x74786e000001), .locks = 1, .atomics = 4},
	.words = txn_status_Table_Single_words,
};

/*
 * TXN_STATUS_COMMITS_PROC_SLOTS_OPS is each process's slot of its last
 * commit's registration: the top-level xid and the end of its commit record,
 * kept at the process's exit for the number's next owner to move.
 */
const HostSharedProcSlotsOps TXN_STATUS_COMMITS_PROC_SLOTS_OPS = {
	.id = UINT64CONST(0x74786e000004),
	.words = 2,
	.kept = true,
};

/* txn_status_internal returns an internal error of the table. */
static HostError
txn_status_internal(const char *message)
{
	StdString text;

	std_string_from_Checked(&text, message, strlen(message));
	return host_Error_fork((const uint8 *) "XX000", &text);
}

/*
 * txn_status_Outcome_word returns the outcome's word, or false for a commit
 * record's end too large for it, which no server reaches.
 */
bool
txn_status_Outcome_word(TxnStatusOutcome self, uint64 *result)
{
	switch (self.tag)
	{
		case TXN_STATUS_OUTCOME_UNOWNED:
			*result = 0;
			return true;
		case TXN_STATUS_OUTCOME_COMMITTED:
			*result = self.u.committed;
			return self.u.committed != 0 && self.u.committed < TXN_STATUS_LSNS;
		case TXN_STATUS_OUTCOME_PENDING_IN:
			*result = TXN_STATUS_LSNS | self.u.pending_in;
			return true;
	}
	pg_unreachable();
}

/* txn_status_Outcome_of returns what an outcome word says: from LSNS on, the process its low 32 bits number. */
TxnStatusOutcome
txn_status_Outcome_of(uint64 word)
{
	TxnStatusOutcome outcome = {.tag = TXN_STATUS_OUTCOME_UNOWNED};

	if (word != 0 && word < TXN_STATUS_LSNS)
	{
		outcome.tag = TXN_STATUS_OUTCOME_COMMITTED;
		outcome.u.committed = word;
	}
	else if (word != 0)
	{
		outcome.tag = TXN_STATUS_OUTCOME_PENDING_IN;
		outcome.u.pending_in = (uint32) (word & PG_UINT32_MAX);
	}
	return outcome;
}

/* txn_status_Outcome_PartialEq_eq reports whether two outcomes are the same variant with the same value. */
bool
txn_status_Outcome_PartialEq_eq(const TxnStatusOutcome *self, const TxnStatusOutcome *other)
{
	if (self->tag != other->tag)
		return false;
	if (self->tag == TXN_STATUS_OUTCOME_COMMITTED)
		return self->u.committed == other->u.committed;
	if (self->tag == TXN_STATUS_OUTCOME_PENDING_IN)
		return self->u.pending_in == other->u.pending_in;
	return true;
}

/* txn_status_Outcome_Debug_fmt is Outcome's derived Debug. */
bool
txn_status_Outcome_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnStatusOutcome *self = value;
	StdDebugTuple t;

	switch (self->tag)
	{
		case TXN_STATUS_OUTCOME_UNOWNED:
			return std_fmt_write_str(f, "Unowned", 7);
		case TXN_STATUS_OUTCOME_COMMITTED:
			t = std_fmt_debug_tuple(f, "Committed", 9);
			std_DebugTuple_field(&t, &self->u.committed, std_u64_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case TXN_STATUS_OUTCOME_PENDING_IN:
			t = std_fmt_debug_tuple(f, "PendingIn", 9);
			std_DebugTuple_field(&t, &self->u.pending_in, std_u32_Debug_fmt);
			return std_DebugTuple_finish(&t);
	}
	pg_unreachable();
}

/* txn_status_Entry_PartialEq_eq reports whether two entries are equal field by field. */
bool
txn_status_Entry_PartialEq_eq(const TxnStatusEntry *self, const TxnStatusEntry *other)
{
	return txn_status_Outcome_PartialEq_eq(&self->outcome, &other->outcome) && self->count == other->count;
}

/* txn_status_Entry_Debug_fmt is Entry's derived Debug. */
bool
txn_status_Entry_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnStatusEntry *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Entry", 5);

	std_DebugStruct_field(&s, "outcome", 7, &self->outcome, txn_status_Outcome_Debug_fmt);
	std_DebugStruct_field(&s, "count", 5, &self->count, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_status_Roomed_PartialEq_eq reports whether two slots are the same. */
bool
txn_status_Roomed_PartialEq_eq(const TxnStatusRoomed *self, const TxnStatusRoomed *other)
{
	return self->f0 == other->f0;
}

/* txn_status_Roomed_Debug_fmt is Roomed's derived Debug. */
bool
txn_status_Roomed_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnStatusRoomed *self = value;
	StdDebugTuple t = std_fmt_debug_tuple(f, "Roomed", 6);

	std_DebugTuple_field(&t, &self->f0, std_u64_Debug_fmt);
	return std_DebugTuple_finish(&t);
}

/* txn_status_pending_here returns the outcome of an entry pending in this process. */
static TxnStatusOutcome
txn_status_pending_here(void)
{
	TxnStatusOutcome pending = {.tag = TXN_STATUS_OUTCOME_PENDING_IN, .u.pending_in = host_shared_proc_number()};

	return pending;
}

/*
 * txn_status_key returns the xid word of an xid's entry, or false for the
 * xids no entry has: those below the first normal one and the last one, a
 * removed place's word.
 */
bool
txn_status_key(HostXid xid, uint64 *result)
{
	if (xid.f0 < TXN_STATUS_FIRST_NORMAL || xid.f0 == TXN_STATUS_REMOVED)
		return false;
	*result = xid.f0;
	return true;
}

/*
 * txn_status_first returns the place a lookup of an xid word starts at in an
 * index of a capacity: the word multiplied by an odd constant that spreads
 * neighbours over the index.
 */
static uint64
txn_status_first(uint64 key, uint64 capacity)
{
	return ((key * UINT64CONST(0x9E3779B97F4A7C15)) >> 16) % capacity;
}

/* txn_status_next returns the place a lookup tries after a place, the one after it, around the index's end. */
static uint64
txn_status_next(uint64 place, uint64 capacity)
{
	return (place + 1) % capacity;
}

/*
 * txn_status_crowded reports whether an index of a capacity with a number of
 * places taken, removed ones counted, is at its load factor, three quarters.
 */
static bool
txn_status_crowded(uint64 taken, uint64 capacity)
{
	return std_saturating_mul_u64(taken, 4) >= std_saturating_mul_u64(capacity, 3);
}

/*
 * txn_status_sized returns the index capacity a growth makes for the entries
 * it keeps: the fewest whole segments of places that hold them under the load
 * factor.
 */
static uint64
txn_status_sized(uint64 kept)
{
	uint64		capacity = TXN_STATUS_PLACES;

	while (txn_status_crowded(kept, capacity))
		capacity = STD_ADD(u64, capacity, TXN_STATUS_PLACES, 121, 9);
	return capacity;
}

/* txn_status_slot_word returns the run's word that names a slot segment. */
static size_t
txn_status_slot_word(uint64 segment)
{
	return (size_t) (3 * segment);
}

/* txn_status_index_word returns the run's word that names an index segment. */
static size_t
txn_status_index_word(uint64 segment)
{
	return (size_t) (3 * (segment / 2) + 1 + segment % 2);
}

/* txn_status_run_word loads a word of the table's run. */
static bool
txn_status_run_word(HostSharedRecord root, size_t index, uint64 *result, HostError *error)
{
	HostSharedRunWord word;

	if (!host_shared_Record_word(root, &TXN_STATUS_TABLE_SINGLE_OPS, index, &word, error))
		return false;
	*result = host_shared_RunWord_load(&word);
	return true;
}

/* txn_status_atomic runs an operation on one of the table's atomics. */
static uint64
txn_status_atomic(HostSharedRecord root, size_t atomic, HostSharedOpTag tag, uint64 a, uint64 b)
{
	HostSharedOp op = {.tag = tag};

	if (tag == HOST_SHARED_OP_ADD)
		op.u.add = a;
	else if (tag == HOST_SHARED_OP_EXCHANGE)
	{
		op.u.exchange.f0 = a;
		op.u.exchange.f1 = b;
	}
	return host_shared_Record_atomic(root, atomic, op);
}

/* txn_status_load loads one of the table's atomics. */
static uint64
txn_status_load(HostSharedRecord root, size_t atomic)
{
	return txn_status_atomic(root, atomic, HOST_SHARED_OP_LOAD, 0, 0);
}

/* txn_status_page opens a segment a run's word names and returns it with the word of one of its places. */
static bool
txn_status_page(const HostDeltaHold *hold, HostSharedRecord root, size_t run, HostDeltaPageRef *page,
				HostError *error)
{
	uint64		name;
	HostDeltaPagePage named;

	if (!txn_status_run_word(root, run, &name, error))
		return false;
	named = host_delta_page_Page_named(name);
	return host_delta_Hold_page(hold, named, page, error);
}

/* txn_status_slotted opens the slot segment that holds a slot and returns the index of the slot's first word. */
static bool
txn_status_slotted(const HostDeltaHold *hold, HostSharedRecord root, uint64 slot, HostDeltaPageRef *page,
				   uint64 *at, HostError *error)
{
	*at = (slot % TXN_STATUS_SLOTS_PER_SEGMENT) * TXN_STATUS_SLOT_WORDS;
	return txn_status_page(hold, root, txn_status_slot_word(slot / TXN_STATUS_SLOTS_PER_SEGMENT), page, error);
}

/* txn_status_located opens the index segment that holds a place and returns the index of the place's first word. */
static bool
txn_status_located(const HostDeltaHold *hold, HostSharedRecord root, uint64 place, HostDeltaPageRef *page,
				   uint64 *at, HostError *error)
{
	*at = (place % TXN_STATUS_PLACES) * TXN_STATUS_PLACE_WORDS;
	return txn_status_page(hold, root, txn_status_index_word(place / TXN_STATUS_PLACES), page, error);
}

/* txn_status_word reads a word of a page. */
static bool
txn_status_word(const HostDeltaPageRef *page, uint64 index, HostDeltaWord *result, HostError *error)
{
	return host_delta_PageRef_word(page, index, result, error);
}

/* txn_status_word_load loads a word of a page. */
static bool
txn_status_word_load(const HostDeltaPageRef *page, uint64 index, uint64 *result, HostError *error)
{
	HostDeltaWord word;

	if (!txn_status_word(page, index, &word, error))
		return false;
	*result = host_delta_Word_load(&word);
	return true;
}

/* txn_status_word_store stores into a word of a page. */
static bool
txn_status_word_store(const HostDeltaPageRef *page, uint64 index, uint64 value, HostError *error)
{
	HostDeltaWord word;

	if (!txn_status_word(page, index, &word, error))
		return false;
	host_delta_Word_store(&word, value);
	return true;
}

/*
 * txn_status_found probes the index under the table's lock for a key from its
 * first place, passing removed places, and returns the key's slot, or false
 * when the probe ends at a free place or finds the key's slot not yet stored.
 */
static bool
txn_status_found(const HostDeltaHold *hold, HostSharedRecord root, uint64 key, uint64 capacity, bool *some,
				 uint64 *slot, HostError *error)
{
	uint64		place = txn_status_first(key, capacity);

	*some = false;
	for (uint64 i = 0; i < capacity; i++)
	{
		HostDeltaPageRef page;
		uint64		at;
		uint64		word;

		if (!txn_status_located(hold, root, place, &page, &at, error) ||
			!txn_status_word_load(&page, at, &word, error))
			return false;
		if (word == TXN_STATUS_FREE)
			return true;
		if (word == key)
		{
			uint64		stored;

			if (!txn_status_word_load(&page, at + 1, &stored, error))
				return false;
			*some = stored != 0;
			if (*some)
				*slot = stored - 1;
			return true;
		}
		place = txn_status_next(place, capacity);
	}
	return true;
}

/*
 * TxnStatusScope is what an operation on the table holds while it runs, each
 * released on every path: its delta hold and its guard of the table's lock,
 * while live, with the table's record.
 */
typedef struct TxnStatusScope
{
	HostIoIo   *io;
	HostDeltaHold hold;
	bool		hold_live;
	HostSharedRecord root;
	HostSharedGuard guard;
	bool		guard_live;
	HostError  *error;
	bool		ok;
} TxnStatusScope;

/* txn_status_release releases what a scope holds, its guard first, then its hold. */
static void
txn_status_release(TxnStatusScope *scope)
{
	if (scope->guard_live)
		host_shared_Guard_drop(&scope->guard);
	scope->guard_live = false;
	if (scope->hold_live)
		host_delta_Hold_drop(&scope->hold);
	scope->hold_live = false;
}

/* txn_status_pad is a scope's pad: what it holds released. Every frame of this file starts with its scope. */
static void
txn_status_pad(void *frame)
{
	txn_status_release(frame);
}

/* txn_status_run runs a body over a frame that starts with its scope, releasing what the scope holds after it. */
static bool
txn_status_run(void *frame, void (*body) (void *frame))
{
	TxnStatusScope *scope = frame;

	std_landing(frame, body, txn_status_pad);
	txn_status_release(scope);
	return scope->ok;
}

/* txn_status_hold takes the scope's delta hold. */
static bool
txn_status_hold(TxnStatusScope *scope)
{
	if (!host_delta_hold(scope->io, &scope->hold, scope->error))
		return false;
	scope->hold_live = true;
	return true;
}

/* txn_status_root finds the table's record. */
static bool
txn_status_root(TxnStatusScope *scope)
{
	return host_shared_single(&TXN_STATUS_TABLE_SINGLE_OPS, &scope->root, scope->error);
}

/* txn_status_lock takes the table's lock, shared or exclusively. */
static bool
txn_status_lock(TxnStatusScope *scope, bool exclusive)
{
	bool		locked = exclusive ?
		host_shared_Record_lock(scope->root, HOST_IO_IO_TAKES_SHORT(scope->io), TXN_STATUS_TABLE_LOCK,
								&scope->guard, scope->error) :
		host_shared_Record_lock_shared(scope->root, HOST_IO_IO_TAKES_SHORT(scope->io), TXN_STATUS_TABLE_LOCK,
									   &scope->guard, scope->error);

	scope->guard_live = locked;
	return locked;
}

/* TxnStatusStep is what an operation over an xid's entry does at its slot. */
typedef enum TxnStatusStep
{
	TXN_STATUS_STEP_LOOKUP,
	TXN_STATUS_STEP_ADD,
	TXN_STATUS_STEP_SETTLED,
} TxnStatusStep;

/* txn_status_entered_frame_t is an operation over an xid's entry: its scope, its xid, its step, and its answer. */
typedef struct txn_status_entered_frame_t
{
	TxnStatusScope scope;
	HostXid		xid;
	TxnStatusStep step;
	uint64		amount;
	uint64		from;
	uint64		to;
	bool		some;
	TxnStatusEntry entry;
	uint64		count;
	bool		settled;
} txn_status_entered_frame_t;

/*
 * txn_status_entered runs a step over an xid's entry under the table's
 * lock shared, with the entry's slot segment and the index of its first word,
 * or answers none for an xid with no entry.
 */
static void
txn_status_entered(void *frame_)
{
	txn_status_entered_frame_t *frame = frame_;
	TxnStatusScope *scope = &frame->scope;
	uint64		key;
	uint64		capacity;
	uint64		slot;
	bool		found;
	HostDeltaPageRef page;
	uint64		at;
	HostDeltaWord word;

	if (!txn_status_key(frame->xid, &key))
	{
		scope->ok = true;
		return;
	}
	if (!txn_status_hold(scope) || !txn_status_root(scope) || !txn_status_lock(scope, false))
		return;
	capacity = txn_status_load(scope->root, TXN_STATUS_CAPACITY);
	if (capacity == 0)
	{
		scope->ok = true;
		return;
	}
	if (!txn_status_found(&scope->hold, scope->root, key, capacity, &found, &slot, scope->error))
		return;
	if (!found)
	{
		scope->ok = true;
		return;
	}
	if (!txn_status_slotted(&scope->hold, scope->root, slot, &page, &at, scope->error))
		return;
	switch (frame->step)
	{
		case TXN_STATUS_STEP_LOOKUP:
			{
				uint64		outcome;

				if (!txn_status_word_load(&page, at + 1, &outcome, scope->error) ||
					!txn_status_word_load(&page, at + 2, &frame->entry.count, scope->error))
					return;
				frame->entry.outcome = txn_status_Outcome_of(outcome);
				break;
			}
		case TXN_STATUS_STEP_ADD:
			if (!txn_status_word(&page, at + 2, &word, scope->error))
				return;
			frame->count = host_delta_Word_fetch_add(&word, frame->amount) + frame->amount;
			break;
		case TXN_STATUS_STEP_SETTLED:
			if (!txn_status_word(&page, at + 1, &word, scope->error))
				return;
			frame->settled = host_delta_Word_compare_exchange(&word, frame->from, frame->to) == frame->from;
			break;
	}
	frame->some = true;
	scope->ok = true;
}

/* txn_status_lookup returns an xid's entry, or false for an xid without one. */
bool
txn_status_lookup(HostIoIo *io, HostXid xid, bool *has_result, TxnStatusEntry *result, HostError *error)
{
	txn_status_entered_frame_t frame = {.scope = {.io = io, .error = error}, .xid = xid,
	.step = TXN_STATUS_STEP_LOOKUP};

	if (!txn_status_run(&frame, txn_status_entered))
		return false;
	*has_result = frame.some;
	if (frame.some)
		*result = frame.entry;
	return true;
}

/*
 * txn_status_add adds to an xid's entry's count, an atomic add under the lock
 * shared, a subtract being the add of its two's complement, and returns the
 * count after it, or false for an xid with no entry, whose add does nothing.
 */
bool
txn_status_add(HostIoIo *io, HostXid xid, uint64 amount, bool *has_result, uint64 *result, HostError *error)
{
	txn_status_entered_frame_t frame = {.scope = {.io = io, .error = error}, .xid = xid,
	.step = TXN_STATUS_STEP_ADD, .amount = amount};

	if (!txn_status_run(&frame, txn_status_entered))
		return false;
	*has_result = frame.some;
	if (frame.some)
		*result = frame.count;
	return true;
}

/*
 * txn_status_settled stores an outcome in an xid's entry in place of the one
 * it holds, a compare-exchange of the outcome word under the lock shared, and
 * reports whether the entry held that one.
 */
bool
txn_status_settled(HostIoIo *io, HostXid xid, TxnStatusOutcome from, TxnStatusOutcome to, bool *result,
				   HostError *error)
{
	txn_status_entered_frame_t frame = {.scope = {.io = io, .error = error}, .xid = xid,
	.step = TXN_STATUS_STEP_SETTLED};

	if (!txn_status_Outcome_word(from, &frame.from) || !txn_status_Outcome_word(to, &frame.to))
	{
		*error = txn_status_internal("a status table outcome is out of range");
		return false;
	}
	if (!txn_status_run(&frame, txn_status_entered))
		return false;
	*result = frame.some && frame.settled;
	return true;
}

/* txn_status_freed pushes a slot onto the free list under the table's lock shared, its xid word cleared first. */
static bool
txn_status_freed(HostSharedRecord root, const HostDeltaPageRef *page, uint64 at, uint64 slot, HostError *error)
{
	if (!txn_status_word_store(page, at, TXN_STATUS_FREE, error))
		return false;
	for (;;)
	{
		uint64		head = txn_status_load(root, TXN_STATUS_FREE_HEAD);
		uint64		pushed = ((head & ~TXN_STATUS_SLOT_MASK) + (TXN_STATUS_SLOT_MASK + 1)) | (slot + 1);

		if (!txn_status_word_store(page, at + 2, head & TXN_STATUS_SLOT_MASK, error))
			return false;
		if (txn_status_atomic(root, TXN_STATUS_FREE_HEAD, HOST_SHARED_OP_EXCHANGE, head, pushed) == head)
			return true;
	}
}

/*
 * txn_status_claimed takes a slot under the table's lock shared, from the
 * free list, or past the slots handed out when the list is empty, or answers
 * false when every linked slot segment is full.
 */
static bool
txn_status_claimed(const HostDeltaHold *hold, HostSharedRecord root, bool *some, uint64 *slot, HostError *error)
{
	*some = false;
	for (;;)
	{
		uint64		head = txn_status_load(root, TXN_STATUS_FREE_HEAD);
		uint64		next;
		HostDeltaPageRef page;
		uint64		at;

		if ((head & TXN_STATUS_SLOT_MASK) == 0)
			break;
		*slot = (head & TXN_STATUS_SLOT_MASK) - 1;
		if (!txn_status_slotted(hold, root, *slot, &page, &at, error) ||
			!txn_status_word_load(&page, at + 2, &next, error))
			return false;
		if (txn_status_atomic(root, TXN_STATUS_FREE_HEAD, HOST_SHARED_OP_EXCHANGE, head,
							  ((head & ~TXN_STATUS_SLOT_MASK) + (TXN_STATUS_SLOT_MASK + 1)) | next) == head)
		{
			*some = true;
			return true;
		}
	}
	for (;;)
	{
		uint64		high = txn_status_load(root, TXN_STATUS_HIGH);
		uint64		name;

		if (!txn_status_run_word(root, txn_status_slot_word(high / TXN_STATUS_SLOTS_PER_SEGMENT), &name, error))
			return false;
		if (name == 0)
			return true;
		if (txn_status_atomic(root, TXN_STATUS_HIGH, HOST_SHARED_OP_EXCHANGE, high, high + 1) == high)
		{
			*some = true;
			*slot = high;
			return true;
		}
	}
}

/* txn_status_pages_drop drops a vector of owned pages, each given back unlinked. */
static void
txn_status_pages_drop(StdVec *pages)
{
	for (size_t i = 0; i < pages->len; i++)
		host_delta_Owned_drop(&STD_VEC_AT(pages, HostDeltaOwned, i));
	std_vec_free_Checked(pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
}

/* txn_status_alloc allocates delta pages of sizes, waiting at the budget until it can. */
static bool
txn_status_alloc(HostIoIo *io, const HostDeltaPageSize *sizes, size_t sizes_len, StdVec *pages, HostError *error)
{
	for (;;)
	{
		bool		ok;
		HostDeltaFull full;

		if (!host_delta_alloc(io, sizes, sizes_len, &ok, pages, &full, error))
			return false;
		if (ok)
			return true;
		if (!txn_memtable_table_waited(io, full, false, error))
			return false;
	}
}

/* txn_status_linked_frame_t is linked's state: its scope and the pages it allocated, while live. */
typedef struct txn_status_linked_frame_t
{
	TxnStatusScope scope;
	StdVec		pages;
	bool		pages_live;
} txn_status_linked_frame_t;

/* txn_status_link links a page into a run's word under the table's lock held exclusively. */
static bool
txn_status_link(TxnStatusScope *scope, HostDeltaOwned *page, size_t run)
{
	HostSharedRunWord word;
	HostDeltaNames names = {&HOST_DELTA_RUN_WORD_NAMES_OPS, &word};

	if (!host_shared_Record_word(scope->root, &TXN_STATUS_TABLE_SINGLE_OPS, run, &word, scope->error))
		return false;
	host_delta_Owned_link(page, &TXN_STATUS_TABLE_SINGLE_OPS.shape, TXN_STATUS_TABLE_LOCK, &scope->guard, names);
	return true;
}

/*
 * txn_status_linked_body links one more slot segment, allocated with no lock
 * and linked under the table's lock exclusively when the slots handed out
 * still fill every linked one; another taker's link meanwhile gives the
 * segment back.
 */
static void
txn_status_linked_body(void *frame_)
{
	txn_status_linked_frame_t *frame = frame_;
	TxnStatusScope *scope = &frame->scope;
	size_t		run;
	uint64		name;

	if (!txn_status_alloc(scope->io, &txn_status_SEGMENT, 1, &frame->pages, scope->error))
		return;
	frame->pages_live = true;
	if (!txn_status_root(scope) || !txn_status_lock(scope, true))
		return;
	run = txn_status_slot_word(txn_status_load(scope->root, TXN_STATUS_HIGH) / TXN_STATUS_SLOTS_PER_SEGMENT);
	if (!txn_status_run_word(scope->root, run, &name, scope->error))
		return;
	if (name == 0 && frame->pages.len != 0)
	{
		HostDeltaOwned page;

		std_vec_pop(&frame->pages, &page, sizeof(HostDeltaOwned));
		if (!txn_status_link(scope, &page, run))
		{
			host_delta_Owned_drop(&page);
			return;
		}
	}
	scope->ok = true;
}

/* txn_status_linked_pad releases the scope, then gives back the pages linked doesn't link. */
static void
txn_status_linked_pad(void *frame_)
{
	txn_status_linked_frame_t *frame = frame_;

	txn_status_release(&frame->scope);
	if (frame->pages_live)
		txn_status_pages_drop(&frame->pages);
	frame->pages_live = false;
}

/* txn_status_linked links one more slot segment when every linked one is full. */
static bool
txn_status_linked(HostIoIo *io, HostError *error)
{
	txn_status_linked_frame_t frame = {.scope = {.io = io, .error = error}};

	std_landing(&frame, txn_status_linked_body, txn_status_linked_pad);
	txn_status_linked_pad(&frame);
	return frame.scope.ok;
}

static bool txn_status_grow(HostIoIo *io, uint64 capacity, HostError *error);

/* TxnStatusTook is a take's answer: whether it took room, retries, or has to grow the index or link a segment first. */
typedef enum TxnStatusTook
{
	TXN_STATUS_TOOK_DONE,
	TXN_STATUS_TOOK_AGAIN,
	TXN_STATUS_TOOK_GROW,
	TXN_STATUS_TOOK_LINK,
} TxnStatusTook;

/* txn_status_take_frame_t is one round of take: its scope, its key and outcome word, and its answer. */
typedef struct txn_status_take_frame_t
{
	TxnStatusScope scope;
	uint64		key;
	uint64		word;
	uint64		capacity;
	TxnStatusTook took;
	TxnStatusRoomed roomed;
	bool		had;
} txn_status_take_frame_t;

/*
 * txn_status_take_body is one round of take: under the table's lock shared,
 * the xid's existing slot, or a slot claimed and stored before a
 * compare-exchange of an index place's xid word claims the place, then the
 * place's slot word.
 */
static void
txn_status_take_body(void *frame_)
{
	txn_status_take_frame_t *frame = frame_;
	TxnStatusScope *scope = &frame->scope;
	uint64		capacity;
	uint64		slot;
	bool		some;
	HostDeltaPageRef entry;
	uint64		entry_at;
	uint64		place;

	if (!txn_status_root(scope))
		return;
	capacity = txn_status_load(scope->root, TXN_STATUS_CAPACITY);
	if (capacity == 0 || txn_status_crowded(txn_status_load(scope->root, TXN_STATUS_TAKEN), capacity))
	{
		frame->capacity = capacity;
		frame->took = TXN_STATUS_TOOK_GROW;
		scope->ok = true;
		return;
	}
	if (!txn_status_hold(scope) || !txn_status_lock(scope, false))
		return;
	scope->ok = true;
	frame->took = TXN_STATUS_TOOK_AGAIN;
	if (txn_status_load(scope->root, TXN_STATUS_CAPACITY) != capacity)
		return;
	scope->ok = false;
	if (!txn_status_found(&scope->hold, scope->root, frame->key, capacity, &some, &slot, scope->error))
		return;
	if (some)
	{
		frame->roomed.f0 = slot;
		frame->had = true;
		frame->took = TXN_STATUS_TOOK_DONE;
		scope->ok = true;
		return;
	}
	if (!txn_status_claimed(&scope->hold, scope->root, &some, &slot, scope->error))
		return;
	if (!some)
	{
		frame->took = TXN_STATUS_TOOK_LINK;
		scope->ok = true;
		return;
	}
	if (!txn_status_slotted(&scope->hold, scope->root, slot, &entry, &entry_at, scope->error) ||
		!txn_status_word_store(&entry, entry_at, frame->key, scope->error) ||
		!txn_status_word_store(&entry, entry_at + 1, frame->word, scope->error) ||
		!txn_status_word_store(&entry, entry_at + 2, 0, scope->error))
		return;
	place = txn_status_first(frame->key, capacity);
	for (uint64 i = 0; i < capacity; i++)
	{
		HostDeltaPageRef page;
		uint64		at;
		HostDeltaWord xid_word;
		uint64		taken;

		if (!txn_status_located(&scope->hold, scope->root, place, &page, &at, scope->error) ||
			!txn_status_word(&page, at, &xid_word, scope->error))
			return;
		taken = host_delta_Word_load(&xid_word);
		if (taken == TXN_STATUS_FREE)
		{
			if (host_delta_Word_compare_exchange(&xid_word, TXN_STATUS_FREE, frame->key) != TXN_STATUS_FREE)
				continue;
			if (!txn_status_word_store(&page, at + 1, slot + 1, scope->error))
				return;
			txn_status_atomic(scope->root, TXN_STATUS_TAKEN, HOST_SHARED_OP_ADD, 1, 0);
			frame->roomed.f0 = slot;
			frame->took = TXN_STATUS_TOOK_DONE;
			scope->ok = true;
			return;
		}
		if (taken == frame->key)
		{
			HostDeltaWord theirs;

			if (!txn_status_freed(scope->root, &entry, entry_at, slot, scope->error) ||
				!txn_status_word(&page, at + 1, &theirs, scope->error))
				return;
			for (;;)
			{
				uint64		stored = host_delta_Word_load(&theirs);

				if (stored != 0)
				{
					frame->roomed.f0 = stored - 1;
					frame->had = true;
					frame->took = TXN_STATUS_TOOK_DONE;
					scope->ok = true;
					return;
				}
				std_hint_spin_loop();
			}
		}
		place = txn_status_next(place, capacity);
	}
	*scope->error = txn_status_internal("the status table has no free place");
}

/*
 * txn_status_take takes room for an xid's entry with an outcome and a count
 * of zero, growing the index first when it is at its load factor and linking
 * a slot segment when every slot is taken, and returns the entry's slot, with
 * whether the xid had an entry already, which it leaves as it is.
 */
bool
txn_status_take(HostIoIo *io, HostXid xid, TxnStatusOutcome outcome, TxnStatusRoomed *result_0, bool *result_1,
				HostError *error)
{
	uint64		key;
	uint64		word;

	if (!txn_status_key(xid, &key))
	{
		*error = txn_status_internal("the status table was asked for room for an xid no entry has");
		return false;
	}
	if (!txn_status_Outcome_word(outcome, &word))
	{
		*error = txn_status_internal("a status table outcome is out of range");
		return false;
	}
	for (;;)
	{
		txn_status_take_frame_t frame = {.scope = {.io = io, .error = error}, .key = key, .word = word};

		if (!txn_status_run(&frame, txn_status_take_body))
			return false;
		switch (frame.took)
		{
			case TXN_STATUS_TOOK_DONE:
				*result_0 = frame.roomed;
				*result_1 = frame.had;
				return true;
			case TXN_STATUS_TOOK_AGAIN:
				break;
			case TXN_STATUS_TOOK_GROW:
				if (!txn_status_grow(io, frame.capacity, error))
					return false;
				break;
			case TXN_STATUS_TOOK_LINK:
				if (!txn_status_linked(io, error))
					return false;
				break;
		}
	}
}

/* txn_status_counted_frame_t is counted's state: its guard of the table's lock, while live, and its arguments. */
typedef struct txn_status_counted_frame_t
{
	HostIoTakesShort io;
	const HostDeltaHold *hold;
	TxnStatusRoomed roomed;
	uint64		amount;
	HostError  *error;
	bool		ok;
	HostSharedGuard guard;
	bool		guard_live;
} txn_status_counted_frame_t;

/* txn_status_counted_body adds the amount to the entry's count under the table's lock shared. */
static void
txn_status_counted_body(void *frame_)
{
	txn_status_counted_frame_t *frame = frame_;
	HostSharedRecord root;
	HostDeltaPageRef page;
	uint64		at;
	HostDeltaWord word;

	if (!host_shared_single(&TXN_STATUS_TABLE_SINGLE_OPS, &root, frame->error) ||
		!host_shared_Record_lock_shared(root, frame->io, TXN_STATUS_TABLE_LOCK, &frame->guard, frame->error))
		return;
	frame->guard_live = true;
	if (!txn_status_slotted(frame->hold, root, frame->roomed.f0, &page, &at, frame->error) ||
		!txn_status_word(&page, at + 2, &word, frame->error))
		return;
	host_delta_Word_fetch_add(&word, frame->amount);
	frame->ok = true;
}

/* txn_status_counted_pad releases counted's guard, while live. */
static void
txn_status_counted_pad(void *frame_)
{
	txn_status_counted_frame_t *frame = frame_;

	if (frame->guard_live)
		host_shared_Guard_drop(&frame->guard);
	frame->guard_live = false;
}

/*
 * txn_status_counted adds an amount to the count of the entry a take's slot
 * names, a subtract being the add of its two's complement, an atomic add
 * under the table's lock shared with no lookup and no check of the slot, for
 * a version's push inside its window under the hold the push holds.
 */
bool
txn_status_counted(HostIoTakesShort io, const HostDeltaHold *hold, TxnStatusRoomed roomed, uint64 amount,
				   HostError *error)
{
	txn_status_counted_frame_t frame = {.io = io, .hold = hold, .roomed = roomed, .amount = amount, .error = error};

	std_landing(&frame, txn_status_counted_body, txn_status_counted_pad);
	txn_status_counted_pad(&frame);
	return frame.ok;
}

/* txn_status_remove_frame_t is remove's state: its scope, its xid, and whether there was an entry. */
typedef struct txn_status_remove_frame_t
{
	TxnStatusScope scope;
	HostXid		xid;
	bool		removed;
} txn_status_remove_frame_t;

/*
 * txn_status_remove_body removes an xid's entry under the lock shared: a
 * store of the removed mark over its index place's xid word, which lookups
 * pass over and a growth leaves behind, then its slot back on the free list.
 */
static void
txn_status_remove_body(void *frame_)
{
	txn_status_remove_frame_t *frame = frame_;
	TxnStatusScope *scope = &frame->scope;
	uint64		key;
	uint64		capacity;
	uint64		place;

	scope->ok = true;
	if (!txn_status_key(frame->xid, &key))
		return;
	scope->ok = false;
	if (!txn_status_hold(scope) || !txn_status_root(scope) || !txn_status_lock(scope, false))
		return;
	scope->ok = true;
	capacity = txn_status_load(scope->root, TXN_STATUS_CAPACITY);
	if (capacity == 0)
		return;
	scope->ok = false;
	place = txn_status_first(key, capacity);
	for (uint64 i = 0; i < capacity; i++)
	{
		HostDeltaPageRef page;
		uint64		at;
		uint64		word;

		if (!txn_status_located(&scope->hold, scope->root, place, &page, &at, scope->error) ||
			!txn_status_word_load(&page, at, &word, scope->error))
			return;
		if (word == TXN_STATUS_FREE)
			break;
		if (word == key)
		{
			uint64		stored;
			HostDeltaPageRef entry;
			uint64		entry_at;

			if (!txn_status_word_load(&page, at + 1, &stored, scope->error))
				return;
			if (stored == 0)
				break;
			if (!txn_status_word_store(&page, at, TXN_STATUS_REMOVED, scope->error) ||
				!txn_status_slotted(&scope->hold, scope->root, stored - 1, &entry, &entry_at, scope->error) ||
				!txn_status_freed(scope->root, &entry, entry_at, stored - 1, scope->error))
				return;
			frame->removed = true;
			break;
		}
		place = txn_status_next(place, capacity);
	}
	scope->ok = true;
}

/* txn_status_remove removes an xid's entry, reporting whether there was one. */
bool
txn_status_remove(HostIoIo *io, HostXid xid, bool *result, HostError *error)
{
	txn_status_remove_frame_t frame = {.scope = {.io = io, .error = error}, .xid = xid};

	if (!txn_status_run(&frame, txn_status_remove_body))
		return false;
	*result = frame.removed;
	return true;
}

/*
 * txn_status_ROOMED is the top-level xid whose room the backend took in this
 * process, zero for none; txn_status_ROOMED_SLOT is its entry's slot; and
 * txn_status_PREPARED is the top-level xid of the prepared transaction whose
 * commit this backend readies. Each is a BackendLocal header and the
 * process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	StdAtomicU64 process;
}			txn_status_ROOMED = {HOST_LOCAL_BACKEND_LOCAL_NEW}, txn_status_ROOMED_SLOT = {HOST_LOCAL_BACKEND_LOCAL_NEW},
			txn_status_PREPARED = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* txn_status_local_load loads a backend's value, made on its first use. */
static uint64
txn_status_local_load(HostLocalBackendLocal *self)
{
	HostLocalLocal local = host_local_BackendLocal_get(self, &STD_ATOMIC_U64_TYPE);
	uint64		value = std_AtomicU64_load_relaxed((StdAtomicU64 *) host_local_Local_Deref_deref(&local,
																								 &STD_ATOMIC_U64_TYPE));

	host_local_Local_drop(&local, &STD_ATOMIC_U64_TYPE);
	return value;
}

/* txn_status_local_store stores a backend's value, made on its first use. */
static void
txn_status_local_store(HostLocalBackendLocal *self, uint64 value)
{
	HostLocalLocal local = host_local_BackendLocal_get(self, &STD_ATOMIC_U64_TYPE);

	std_AtomicU64_store_relaxed((StdAtomicU64 *) host_local_Local_Deref_deref(&local, &STD_ATOMIC_U64_TYPE), value);
	host_local_Local_drop(&local, &STD_ATOMIC_U64_TYPE);
}

/* txn_status_local_existing loads a backend's value only if the backend made it, false otherwise. */
static bool
txn_status_local_existing(HostLocalBackendLocal *self, uint64 *value)
{
	HostLocalLocal local;

	if (!host_local_BackendLocal_existing(self, &local))
		return false;
	*value = std_AtomicU64_load_relaxed((StdAtomicU64 *) host_local_Local_Deref_deref(&local, &STD_ATOMIC_U64_TYPE));
	host_local_Local_drop(&local, &STD_ATOMIC_U64_TYPE);
	return true;
}

/*
 * txn_status_registered adds the backend's moves of its storages' totals,
 * then stores a commit's pair in the process's own slot when this process
 * took its transaction's room, the end first and the xid last: two stores
 * with no lock, no allocation, and no result, for a call inside the commit's
 * critical section.
 */
void
txn_status_registered(const HostSharedListed *listed, HostXid xid, HostLsn end)
{
	uint64		roomed;

	txn_state_committed();
	if (txn_status_local_existing(&txn_status_ROOMED.local, &roomed) && roomed == xid.f0)
	{
		host_shared_store_own(&TXN_STATUS_COMMITS_PROC_SLOTS_OPS, listed, TXN_STATUS_COMMIT_END_INDEX, end);
		host_shared_store_own(&TXN_STATUS_COMMITS_PROC_SLOTS_OPS, listed, TXN_STATUS_COMMIT_XID_INDEX, xid.f0);
	}
}

/*
 * txn_status_prepared_registered is registered for the prepared transaction
 * whose commit prepared_committing readied in this backend.
 */
void
txn_status_prepared_registered(const HostSharedListed *listed, HostLsn end)
{
	uint64		prepared;

	if (txn_status_local_existing(&txn_status_PREPARED.local, &prepared))
	{
		HostXid		xid = {prepared};

		txn_status_registered(listed, xid, end);
	}
}

/*
 * txn_status_moved moves the pair in the process's own slot into its xid's
 * entry: where the entry is still pending in this process, it stores the end
 * as the entry's outcome and drops the entry when nothing names it; then it
 * clears the slot's xid.
 */
static bool
txn_status_moved(HostIoIo *io, const HostSharedMine *mine, HostError *error)
{
	HostXid		xid = {host_shared_Mine_load(mine, TXN_STATUS_COMMIT_XID_INDEX)};
	TxnStatusOutcome pending = txn_status_pending_here();
	TxnStatusOutcome committed = {.tag = TXN_STATUS_OUTCOME_COMMITTED,
	.u.committed = host_shared_Mine_load(mine, TXN_STATUS_COMMIT_END_INDEX)};
	bool		some;
	TxnStatusEntry entry;
	bool		settled;
	bool		removed;

	if (xid.f0 == 0)
		return true;
	if (!txn_status_lookup(io, xid, &some, &entry, error))
		return false;
	if (some && txn_status_Outcome_PartialEq_eq(&entry.outcome, &pending))
	{
		if (!txn_status_settled(io, xid, pending, committed, &settled, error))
			return false;
		if (settled && entry.count == 0 && !txn_status_remove(io, xid, &removed, error))
			return false;
	}
	host_shared_Mine_store(mine, TXN_STATUS_COMMIT_XID_INDEX, 0);
	return true;
}

/*
 * txn_status_roomed takes the room of a top-level xid's entry for a write to
 * the fork's storage, where failing and waiting are allowed, and returns the
 * entry's slot: once for the transaction in this backend, it first moves the
 * pair in the process's slot, then takes the room pending in this process;
 * later writes of the transaction take the slot it kept.
 */
bool
txn_status_roomed(HostIoIo *io, HostXid top, TxnStatusRoomed *result, HostError *error)
{
	HostSharedMine mine;
	bool		had;

	if (txn_status_local_load(&txn_status_ROOMED.local) == top.f0)
	{
		result->f0 = txn_status_local_load(&txn_status_ROOMED_SLOT.local);
		return true;
	}
	if (!host_shared_mine(HOST_IO_IO_TAKES_SHORT(io), &TXN_STATUS_COMMITS_PROC_SLOTS_OPS, &mine, error) ||
		!txn_status_moved(io, &mine, error) ||
		!txn_status_take(io, top, txn_status_pending_here(), result, &had, error))
		return false;
	txn_status_local_store(&txn_status_ROOMED_SLOT.local, result->f0);
	txn_status_local_store(&txn_status_ROOMED.local, top.f0);
	return true;
}

/*
 * txn_status_prepared_committing readies the registration of a prepared
 * transaction's commit, ahead of its critical section: it moves the pair in
 * the process's slot, then points the prepared xid's entry at this process.
 */
bool
txn_status_prepared_committing(HostIoIo *io, HostXid xid, HostError *error)
{
	HostSharedMine mine;
	bool		some;
	TxnStatusEntry entry;
	bool		settled;

	if (!host_shared_mine(HOST_IO_IO_TAKES_SHORT(io), &TXN_STATUS_COMMITS_PROC_SLOTS_OPS, &mine, error) ||
		!txn_status_moved(io, &mine, error))
		return false;
	txn_status_local_store(&txn_status_PREPARED.local, xid.f0);
	if (!txn_status_lookup(io, xid, &some, &entry, error))
		return false;
	if (some)
	{
		if (!txn_status_settled(io, xid, entry.outcome, txn_status_pending_here(), &settled, error))
			return false;
		txn_status_local_store(&txn_status_ROOMED.local, xid.f0);
	}
	return true;
}

/*
 * txn_status_kept_roomed returns the slot of the entry a version's writer's
 * top-level xid has, taking no room for one with none, so a version carried
 * or read back counts where its writer's other versions do.
 */
bool
txn_status_kept_roomed(HostIoIo *io, const TxnMemtableVersionWriter *writer, bool *has_result,
					   TxnStatusRoomed *result, HostError *error)
{
	HostXid		top = {writer->has_top ? writer->top : writer->xid};
	TxnStatusEntry entry;

	if (!txn_status_lookup(io, top, has_result, &entry, error))
		return false;
	return !*has_result || txn_status_redo_roomed(io, writer, has_result, result, error);
}

/*
 * txn_status_redo_roomed takes the room of a replayed row change's top-level
 * xid pending in no process, and returns its slot; a writer below the first
 * normal xid takes none.
 */
bool
txn_status_redo_roomed(HostIoIo *io, const TxnMemtableVersionWriter *writer, bool *has_result,
					   TxnStatusRoomed *result, HostError *error)
{
	HostXid		top = {writer->has_top ? writer->top : writer->xid};
	TxnStatusOutcome unowned = {.tag = TXN_STATUS_OUTCOME_UNOWNED};
	uint64		key;
	bool		had;

	*has_result = txn_status_key(top, &key);
	if (!*has_result)
		return true;
	return txn_status_take(io, top, unowned, result, &had, error);
}

/*
 * txn_status_redo_committed stores a replayed commit record's end as its
 * transaction's outcome, where the transaction took room in the replay,
 * pending in no process.
 */
bool
txn_status_redo_committed(HostIoIo *io, HostXid xid, HostLsn end, HostError *error)
{
	TxnStatusOutcome unowned = {.tag = TXN_STATUS_OUTCOME_UNOWNED};
	TxnStatusOutcome committed = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = end};
	bool		settled;

	return txn_status_settled(io, xid, unowned, committed, &settled, error);
}

/* txn_status_lowered_frame_t is lowered's state: the processes it reads, while live, and its arguments. */
typedef struct txn_status_lowered_frame_t
{
	HostIoIo   *io;
	HostLsn		position;
	HostLsn    *result;
	HostError  *error;
	bool		ok;
	StdVec		processes;
	bool		processes_live;
} txn_status_lowered_frame_t;

/* txn_status_lowered_body lowers the point below each registered commit still in the proc array. */
static void
txn_status_lowered_body(void *frame_)
{
	txn_status_lowered_frame_t *frame = frame_;

	*frame->result = frame->position;
	if (!host_shared_processes(&TXN_STATUS_COMMITS_PROC_SLOTS_OPS, &frame->processes, frame->error))
		return;
	frame->processes_live = true;
	for (size_t i = 0; i < frame->processes.len; i++)
	{
		uint32		process = STD_VEC_AT(&frame->processes, uint32, i);
		uint64		xid;
		uint64		end;

		if (!host_shared_load(&TXN_STATUS_COMMITS_PROC_SLOTS_OPS, TXN_STATUS_COMMIT_XID_INDEX, process, &xid,
							  frame->error) ||
			!host_shared_load(&TXN_STATUS_COMMITS_PROC_SLOTS_OPS, TXN_STATUS_COMMIT_END_INDEX, process, &end,
							  frame->error))
			return;
		if (xid != 0 && end != 0 && end <= frame->position)
		{
			HostXid		asked = {xid};
			bool		running;

			if (!host_in_progress(HOST_IO_IO_IO_DOORS(frame->io), asked, &running, frame->error))
				return;
			if (running)
				*frame->result = Min(*frame->result, end - 1);
		}
	}
	frame->ok = true;
}

/* txn_status_lowered_pad frees the processes lowered read, while live. */
static void
txn_status_lowered_pad(void *frame_)
{
	txn_status_lowered_frame_t *frame = frame_;

	if (frame->processes_live)
		std_vec_free_Checked(&frame->processes, sizeof(uint32), _Alignof(uint32));
	frame->processes_live = false;
}

/*
 * txn_status_lowered returns a fold's point from the WAL's insert position
 * the fold read: below the end of every commit registered at or below it
 * whose transaction is still in the proc array.
 */
bool
txn_status_lowered(HostIoIo *io, HostLsn position, HostLsn *result, HostError *error)
{
	txn_status_lowered_frame_t frame = {.io = io, .position = position, .result = result, .error = error};

	std_landing(&frame, txn_status_lowered_body, txn_status_lowered_pad);
	txn_status_lowered_pad(&frame);
	return frame.ok;
}

/* txn_status_Commit_PartialEq_eq reports whether two commits are the same variant with the same end. */
bool
txn_status_Commit_PartialEq_eq(const TxnStatusCommit *self, const TxnStatusCommit *other)
{
	return self->tag == other->tag && (self->tag != TXN_STATUS_COMMIT_AT || self->u.at == other->u.at);
}

/* txn_status_Commit_Debug_fmt is Commit's derived Debug. */
bool
txn_status_Commit_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnStatusCommit *self = value;
	StdDebugTuple t;

	switch (self->tag)
	{
		case TXN_STATUS_COMMIT_MISSING:
			return std_fmt_write_str(f, "Missing", 7);
		case TXN_STATUS_COMMIT_PENDING:
			return std_fmt_write_str(f, "Pending", 7);
		case TXN_STATUS_COMMIT_AT:
			t = std_fmt_debug_tuple(f, "At", 2);
			std_DebugTuple_field(&t, &self->u.at, std_u64_Debug_fmt);
			return std_DebugTuple_finish(&t);
	}
	pg_unreachable();
}

/*
 * txn_status_committed reads a top-level xid's commit from its entry,
 * following an entry pending in a process to that process's slot: once seam 1
 * says the xid committed, the slot's xid, then its end, then the entry again,
 * the end taken when the slot named the xid and the entry was unchanged.
 */
bool
txn_status_committed(HostIoIo *io, HostXid xid, TxnStatusCommit *result, HostError *error)
{
	for (;;)
	{
		bool		some;
		TxnStatusEntry entry;
		TxnStatusEntry again;
		HostStatus	status;
		uint32		process;
		uint64		named;
		uint64		end;

		if (!txn_status_lookup(io, xid, &some, &entry, error))
			return false;
		if (!some)
		{
			result->tag = TXN_STATUS_COMMIT_MISSING;
			return true;
		}
		if (entry.outcome.tag == TXN_STATUS_OUTCOME_COMMITTED)
		{
			result->tag = TXN_STATUS_COMMIT_AT;
			result->u.at = entry.outcome.u.committed;
			return true;
		}
		if (entry.outcome.tag == TXN_STATUS_OUTCOME_UNOWNED)
		{
			result->tag = TXN_STATUS_COMMIT_PENDING;
			return true;
		}
		process = entry.outcome.u.pending_in;
		if (!host_status(HOST_IO_IO_IO_DOORS(io), xid, &status, error))
			return false;
		if (status != HOST_STATUS_COMMITTED)
		{
			result->tag = TXN_STATUS_COMMIT_PENDING;
			return true;
		}
		if (!host_shared_load(&TXN_STATUS_COMMITS_PROC_SLOTS_OPS, TXN_STATUS_COMMIT_XID_INDEX, process, &named, error) ||
			!host_shared_load(&TXN_STATUS_COMMITS_PROC_SLOTS_OPS, TXN_STATUS_COMMIT_END_INDEX, process, &end, error))
			return false;
		if (named != xid.f0)
			continue;
		if (!txn_status_lookup(io, xid, &some, &again, error))
			return false;
		if (some && txn_status_Entry_PartialEq_eq(&again, &entry))
		{
			result->tag = TXN_STATUS_COMMIT_AT;
			result->u.at = end;
			return true;
		}
	}
}

/* TxnStatusDropped is why a growth's first pass found an entry no longer needed: its xid ended or aborted. */
typedef enum TxnStatusDropped
{
	TXN_STATUS_DROPPED_ENDED,
	TXN_STATUS_DROPPED_ABORTED,
} TxnStatusDropped;

/* TxnStatusMarked is an entry a growth's first pass marked to drop: its xid word and why. */
typedef struct TxnStatusMarked
{
	uint64		key;
	TxnStatusDropped dropped;
} TxnStatusMarked;

/* TxnStatusPlaced is an entry a growth places in its new index: its place, its xid word, and its slot. */
typedef struct TxnStatusPlaced
{
	uint64		to;
	uint64		key;
	uint64		slot;
} TxnStatusPlaced;

/* TxnStatusNames is an iterator of old index segments' names, which a growth retires. */
typedef struct TxnStatusNames
{
	const uint64 *names;
	size_t		at;
	size_t		len;
} TxnStatusNames;

/* txn_status_Names_next yields the next old segment's page, false after the last. */
static bool
txn_status_Names_next(void *self_, void *result_)
{
	TxnStatusNames *self = self_;
	HostDeltaRetireItem *result = result_;

	if (self->at == self->len)
		return false;
	result->is_ok = true;
	result->ok = host_delta_page_Page_named(self->names[self->at++]);
	return true;
}

/* txn_status_Names_size_hint is the iterator's exact count of names left. */
static StdSizeHint
txn_status_Names_size_hint(const void *self_)
{
	const TxnStatusNames *self = self_;
	StdSizeHint hint = {self->len - self->at, true, self->len - self->at};

	return hint;
}

/* TXN_STATUS_NAMES_ITERATOR_OPS is the old segments' iterator's table. */
static const StdIteratorOps TXN_STATUS_NAMES_ITERATOR_OPS = {txn_status_Names_next, txn_status_Names_size_hint};

/* txn_status_marked_order orders marked entries by their xid words. */
static int
txn_status_marked_order(const void *a, const void *b)
{
	uint64		left = ((const TxnStatusMarked *) a)->key;
	uint64		right = ((const TxnStatusMarked *) b)->key;

	return left < right ? -1 : left > right;
}

/*
 * txn_status_grow_frame_t is grow's state across its pad: its scope, the
 * entries its first pass read, the ones it marked, the new segments, and the
 * second pass's vectors, each while live.
 */
typedef struct txn_status_grow_frame_t
{
	TxnStatusScope scope;
	uint64		capacity;
	StdVec		entries;
	bool		entries_live;
	StdVec		marked;
	bool		marked_live;
	StdVec		pages;
	bool		pages_live;
	StdVec		used;
	bool		used_live;
	StdVec		old;
	bool		old_live;
	StdVec		placed;
	bool		placed_live;
	StdVec		dropped;
	bool		dropped_live;
	bool		link;
} txn_status_grow_frame_t;

/* txn_status_grow_free frees what grow's frame holds live, its scope first. */
static void
txn_status_grow_free(void *frame_)
{
	txn_status_grow_frame_t *frame = frame_;

	txn_status_release(&frame->scope);
	if (frame->entries_live)
		std_vec_free_Checked(&frame->entries, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	if (frame->marked_live)
		std_vec_free_Checked(&frame->marked, sizeof(TxnStatusMarked), _Alignof(TxnStatusMarked));
	if (frame->pages_live)
		txn_status_pages_drop(&frame->pages);
	if (frame->used_live)
		std_vec_free_Checked(&frame->used, 1, 1);
	if (frame->old_live)
		std_vec_free_Checked(&frame->old, sizeof(uint64), _Alignof(uint64));
	if (frame->placed_live)
		std_vec_free_Checked(&frame->placed, sizeof(TxnStatusPlaced), _Alignof(TxnStatusPlaced));
	if (frame->dropped_live)
		std_vec_free_Checked(&frame->dropped, sizeof(uint64), _Alignof(uint64));
	frame->entries_live = frame->marked_live = frame->pages_live = frame->used_live = false;
	frame->old_live = frame->placed_live = frame->dropped_live = false;
}

/* txn_status_needed reports whether the index still needs the growth a taker asked for at a capacity it read. */
static bool
txn_status_needed(HostSharedRecord root, uint64 capacity)
{
	return txn_status_load(root, TXN_STATUS_CAPACITY) == capacity &&
		(capacity == 0 || txn_status_crowded(txn_status_load(root, TXN_STATUS_TAKEN), capacity));
}

/* txn_status_grow_read is grow's first pass: every entry's xid word and count, read under the lock shared. */
static bool
txn_status_grow_read(txn_status_grow_frame_t *frame, bool *needed)
{
	TxnStatusScope *scope = &frame->scope;

	std_vec_new(&frame->entries, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->entries_live = true;
	*needed = true;
	if (frame->capacity == 0)
		return true;
	if (!txn_status_hold(scope) || !txn_status_root(scope) || !txn_status_lock(scope, false))
		return false;
	*needed = txn_status_needed(scope->root, frame->capacity);
	for (uint64 place = 0; *needed && place < frame->capacity; place++)
	{
		HostDeltaPageRef page;
		uint64		at;
		std_TupleU64U64_t entry;
		uint64		stored;
		HostDeltaPageRef slots;
		uint64		slot_at;

		if (!txn_status_located(&scope->hold, scope->root, place, &page, &at, scope->error) ||
			!txn_status_word_load(&page, at, &entry.f0, scope->error) ||
			!txn_status_word_load(&page, at + 1, &stored, scope->error))
			return false;
		if (stored == 0 || entry.f0 == TXN_STATUS_FREE || entry.f0 == TXN_STATUS_REMOVED)
			continue;
		if (!txn_status_slotted(&scope->hold, scope->root, stored - 1, &slots, &slot_at, scope->error) ||
			!txn_status_word_load(&slots, slot_at + 2, &entry.f1, scope->error))
			return false;
		std_vec_push_Checked(&frame->entries, &entry, sizeof(entry), _Alignof(std_TupleU64U64_t));
	}
	txn_status_release(scope);
	return true;
}

/*
 * txn_status_grow_marks asks seam 1, with the lock released, whether each
 * entry read is still needed, marking one with a count of zero whose xid is
 * no longer in progress, and one with a count whose xid aborted.
 */
static bool
txn_status_grow_marks(txn_status_grow_frame_t *frame)
{
	TxnStatusScope *scope = &frame->scope;
	bool		answered = host_xids_answered();

	std_vec_new(&frame->marked, sizeof(TxnStatusMarked), _Alignof(TxnStatusMarked));
	frame->marked_live = true;
	for (size_t i = 0; i < frame->entries.len; i++)
	{
		std_TupleU64U64_t entry = STD_VEC_AT(&frame->entries, std_TupleU64U64_t, i);
		HostXid		xid = {entry.f0};
		TxnStatusMarked marked = {.key = entry.f0};
		bool		drop = false;

		if (entry.f1 == 0 && answered)
		{
			bool		running;

			if (!host_in_progress(HOST_IO_IO_IO_DOORS(scope->io), xid, &running, scope->error))
				return false;
			drop = !running;
			marked.dropped = TXN_STATUS_DROPPED_ENDED;
		}
		else if (entry.f1 != 0)
		{
			HostStatus	status;

			if (!host_status(HOST_IO_IO_IO_DOORS(scope->io), xid, &status, scope->error))
				return false;
			drop = status == HOST_STATUS_ABORTED;
			marked.dropped = TXN_STATUS_DROPPED_ABORTED;
		}
		if (drop)
			std_vec_push_Checked(&frame->marked, &marked, sizeof(marked), _Alignof(TxnStatusMarked));
	}
	if (frame->marked.len > 1)
		qsort(frame->marked.ptr, frame->marked.len, sizeof(TxnStatusMarked), txn_status_marked_order);
	return true;
}

/*
 * txn_status_grow_copy is grow's second pass under the lock exclusively:
 * every entry but the marked ones placed in the new segments, the marked
 * ones' slots put back on the free list, the segments linked, the capacity
 * and the places taken stored, and the old segments listed for retirement;
 * it gives the segments back when the index no longer needs them or the copy
 * would reach the load factor.
 */
static bool
txn_status_grow_copy(txn_status_grow_frame_t *frame, uint64 grown, bool *retire)
{
	TxnStatusScope *scope = &frame->scope;
	uint64		taken = 0;
	uint64		name;

	*retire = false;
	if (!txn_status_hold(scope) || !txn_status_root(scope) || !txn_status_lock(scope, true))
		return false;
	if (!txn_status_needed(scope->root, frame->capacity))
		return true;
	if (!txn_status_run_word(scope->root, txn_status_slot_word(0), &name, scope->error))
		return false;
	if (frame->capacity == 0 && name == 0)
	{
		frame->link = true;
		return true;
	}
	std_vec_u8_from_elem(&frame->used, 0, (size_t) grown);
	frame->used_live = true;
	std_vec_new(&frame->old, sizeof(uint64), _Alignof(uint64));
	frame->old_live = true;
	for (uint64 segment = 0; segment < frame->capacity / TXN_STATUS_PLACES; segment++)
	{
		if (!txn_status_run_word(scope->root, txn_status_index_word(segment), &name, scope->error))
			return false;
		std_vec_push_Checked(&frame->old, &name, sizeof(name), _Alignof(uint64));
	}
	std_vec_new(&frame->placed, sizeof(TxnStatusPlaced), _Alignof(TxnStatusPlaced));
	frame->placed_live = true;
	std_vec_new(&frame->dropped, sizeof(uint64), _Alignof(uint64));
	frame->dropped_live = true;
	for (uint64 place = 0; place < frame->capacity; place++)
	{
		HostDeltaPageRef page;
		uint64		at;
		uint64		key;
		uint64		stored;
		HostDeltaPageRef slots;
		uint64		slot_at;
		uint64		count;
		TxnStatusMarked sought;
		const TxnStatusMarked *found;
		TxnStatusPlaced placed;

		if (!txn_status_located(&scope->hold, scope->root, place, &page, &at, scope->error) ||
			!txn_status_word_load(&page, at, &key, scope->error) ||
			!txn_status_word_load(&page, at + 1, &stored, scope->error))
			return false;
		if (stored == 0 || key == TXN_STATUS_FREE || key == TXN_STATUS_REMOVED)
			continue;
		if (!txn_status_slotted(&scope->hold, scope->root, stored - 1, &slots, &slot_at, scope->error) ||
			!txn_status_word_load(&slots, slot_at + 2, &count, scope->error))
			return false;
		sought.key = key;
		found = frame->marked.len == 0 ? NULL :
			bsearch(&sought, frame->marked.ptr, frame->marked.len, sizeof(TxnStatusMarked), txn_status_marked_order);
		if (found != NULL &&
			(found->dropped == TXN_STATUS_DROPPED_ABORTED || (found->dropped == TXN_STATUS_DROPPED_ENDED && count == 0)))
		{
			uint64		slot = stored - 1;

			std_vec_push_Checked(&frame->dropped, &slot, sizeof(slot), _Alignof(uint64));
			continue;
		}
		if (txn_status_crowded(taken + 1, grown))
			return true;
		placed.to = txn_status_first(key, grown);
		while (((uint8 *) frame->used.ptr)[placed.to])
			placed.to = txn_status_next(placed.to, grown);
		((uint8 *) frame->used.ptr)[placed.to] = 1;
		placed.key = key;
		placed.slot = stored - 1;
		std_vec_push_Checked(&frame->placed, &placed, sizeof(placed), _Alignof(TxnStatusPlaced));
		taken++;
	}
	for (size_t i = 0; i < frame->placed.len; i++)
	{
		TxnStatusPlaced placed = STD_VEC_AT(&frame->placed, TxnStatusPlaced, i);
		HostDeltaOwned *page = &STD_VEC_AT(&frame->pages, HostDeltaOwned, placed.to / TXN_STATUS_PLACES);
		uint64		at = (placed.to % TXN_STATUS_PLACES) * TXN_STATUS_PLACE_WORDS;

		if (!host_delta_Owned_store(page, at, placed.key, scope->error) ||
			!host_delta_Owned_store(page, at + 1, placed.slot + 1, scope->error))
			return false;
	}
	for (uint64 segment = 0; frame->pages.len != 0; segment++)
	{
		HostDeltaOwned page;

		std_vec_remove(&frame->pages, 0, &page, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), RUST_FILE, 766, 9);
		if (!txn_status_link(scope, &page, txn_status_index_word(segment)))
		{
			host_delta_Owned_drop(&page);
			return false;
		}
	}
	txn_status_atomic(scope->root, TXN_STATUS_TAKEN, HOST_SHARED_OP_EXCHANGE,
					  txn_status_load(scope->root, TXN_STATUS_TAKEN), taken);
	txn_status_atomic(scope->root, TXN_STATUS_CAPACITY, HOST_SHARED_OP_EXCHANGE, frame->capacity, grown);
	for (size_t i = 0; i < frame->dropped.len; i++)
	{
		uint64		slot = STD_VEC_AT(&frame->dropped, uint64, i);
		HostDeltaPageRef slots;
		uint64		slot_at;

		if (!txn_status_slotted(&scope->hold, scope->root, slot, &slots, &slot_at, scope->error) ||
			!txn_status_freed(scope->root, &slots, slot_at, slot, scope->error))
			return false;
	}
	*retire = true;
	return true;
}

/*
 * txn_status_grow_body rebuilds the index at its load factor from a capacity
 * a taker read, or makes its first segment; it moves no slot. Its first pass
 * reads every entry under the lock shared and, with the lock released, marks
 * the ones no longer needed; the new capacity is sized from the entries it
 * keeps; it allocates the new segments with no lock, waiting at the budget,
 * then copies under the lock exclusively and retires the old segments.
 */
static void
txn_status_grow_body(void *frame_)
{
	txn_status_grow_frame_t *frame = frame_;
	TxnStatusScope *scope = &frame->scope;
	bool		needed;
	uint64		grown;
	size_t		segments;
	StdVec		sizes;
	bool		retire;
	TxnStatusNames names;
	StdIterator pages;

	if (!txn_status_grow_read(frame, &needed))
		return;
	if (!needed)
	{
		scope->ok = true;
		return;
	}
	if (!txn_status_grow_marks(frame) ||
		!host_pause(HOST_SHARED_IO_PAUSES(scope->io), "doltgres-status-grow-decided", scope->error))
		return;
	grown = txn_status_sized((uint64) (frame->entries.len - frame->marked.len));
	segments = (size_t) (grown / TXN_STATUS_PLACES);
	std_vec_with_capacity_Checked(&sizes, segments, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	for (size_t i = 0; i < segments; i++)
		std_vec_push_Checked(&sizes, &txn_status_SEGMENT, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	if (!txn_status_alloc(scope->io, sizes.ptr, sizes.len, &frame->pages, scope->error))
	{
		std_vec_free_Checked(&sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
		return;
	}
	std_vec_free_Checked(&sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	frame->pages_live = true;
	if (!txn_status_grow_copy(frame, grown, &retire))
		return;
	txn_status_release(scope);
	if (frame->link)
	{
		txn_status_pages_drop(&frame->pages);
		frame->pages_live = false;
		scope->ok = txn_status_linked(scope->io, scope->error);
		return;
	}
	if (!retire)
	{
		scope->ok = true;
		return;
	}
	names.names = frame->old.ptr;
	names.at = 0;
	names.len = frame->old.len;
	pages.ops = &TXN_STATUS_NAMES_ITERATOR_OPS;
	pages.self = &names;
	scope->ok = host_delta_retire(scope->io, NULL, 0, pages, scope->error);
}

/* txn_status_grow grows the index from a capacity a taker read. */
static bool
txn_status_grow(HostIoIo *io, uint64 capacity, HostError *error)
{
	txn_status_grow_frame_t frame = {.scope = {.io = io, .error = error}, .capacity = capacity};

	std_landing(&frame, txn_status_grow_body, txn_status_grow_free);
	txn_status_grow_free(&frame);
	return frame.scope.ok;
}

#ifdef DOLTGRES_TEST_TXN

/*
 * test_txn_status_tests_a_key_whose_slot_isnt_stored_yet_leaves_its_slot_as_it_was:
 * a probe that finds a key whose slot an insert hasn't stored yet answers
 * none and leaves its caller's slot as the caller had it (bugs row 288).
 */
void
test_txn_status_tests_a_key_whose_slot_isnt_stored_yet_leaves_its_slot_as_it_was(void)
{
	SimcTestIo	test_io = simc_io();
	TxnStatusOutcome running = {.tag = TXN_STATUS_OUTCOME_UNOWNED};
	TxnStatusScope scope = {.io = &test_io.io};
	TxnStatusRoomed roomed;
	HostXid		xid = simc_begin();
	HostError	error;
	uint64		key;
	uint64		capacity;
	uint64		place;
	uint64		slot = 0x5EED;
	bool		had;
	bool		some = true;

	scope.error = &error;
	if (!txn_status_take(&test_io.io, xid, running, &roomed, &had, &error) || !txn_status_key(xid, &key) ||
		!txn_status_hold(&scope) || !txn_status_root(&scope) || !txn_status_lock(&scope, true))
		STD_RESULT_EXPECT_FAILED(__LINE__, 5, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	capacity = txn_status_load(scope.root, TXN_STATUS_CAPACITY);
	place = txn_status_first(key, capacity);
	for (;;)
	{
		HostDeltaPageRef page;
		uint64		at;
		uint64		word;

		if (!txn_status_located(&scope.hold, scope.root, place, &page, &at, &error) ||
			!txn_status_word_load(&page, at, &word, &error) ||
			(word == key && !txn_status_word_store(&page, at + 1, 0, &error)))
			STD_RESULT_EXPECT_FAILED(__LINE__, 5, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (word == key)
			break;
		place = txn_status_next(place, capacity);
	}
	if (!txn_status_found(&scope.hold, scope.root, key, capacity, &some, &slot, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 5, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 5, !some && slot == 0x5EED,
			   "a key whose slot isn't stored yet answers none and leaves its slot as it was");
	txn_status_release(&scope);
	simc_abort(xid);
	simc_TestIo_drop(&test_io);
}

/*
 * The fork's own: each outcome's word says that outcome again, a zero word is
 * pending in no process, and the kinds' words don't meet.
 */
void
test_txn_status_tests_an_outcomes_word_says_the_outcome(void)
{
	TxnStatusOutcome outcomes[6] = {{.tag = TXN_STATUS_OUTCOME_UNOWNED},
	{.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = 1},
	{.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = UINT64CONST(0x16B3F2A40)},
	{.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = TXN_STATUS_LSNS - 1},
	{.tag = TXN_STATUS_OUTCOME_PENDING_IN, .u.pending_in = 0},
	{.tag = TXN_STATUS_OUTCOME_PENDING_IN, .u.pending_in = PG_UINT32_MAX}};
	TxnStatusOutcome unowned = {.tag = TXN_STATUS_OUTCOME_UNOWNED};
	TxnStatusOutcome zero = txn_status_Outcome_of(0);
	uint64		words[6];

	for (size_t k = 0; k < lengthof(outcomes); k++)
	{
		TxnStatusOutcome again;

		STD_ASSERT(__LINE__, 0, txn_status_Outcome_word(outcomes[k], &words[k]), "the outcome has a word");
		again = txn_status_Outcome_of(words[k]);
		STD_ASSERT_EQ(__LINE__, 0, txn_status_Outcome_PartialEq_eq(&again, &outcomes[k]), &again,
					  txn_status_Outcome_Debug_fmt, &outcomes[k], txn_status_Outcome_Debug_fmt);
		for (size_t j = 0; j < k; j++)
			STD_ASSERT(__LINE__, 0, words[j] != words[k], "!words.contains(&word)");
	}
	STD_ASSERT_EQ(__LINE__, 0, txn_status_Outcome_PartialEq_eq(&zero, &unowned), &zero, txn_status_Outcome_Debug_fmt,
				  &unowned, txn_status_Outcome_Debug_fmt);
}

/* The fork's own: a commit record's end of zero or past the word's room has no word. */
void
test_txn_status_tests_an_outcome_too_large_for_its_word_has_none(void)
{
	TxnStatusOutcome at_zero = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = 0};
	TxnStatusOutcome past = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = TXN_STATUS_LSNS};
	uint64		word;

	STD_ASSERT(__LINE__, 0, !txn_status_Outcome_word(at_zero, &word), "Outcome::Committed(0).word() == None");
	STD_ASSERT(__LINE__, 0, !txn_status_Outcome_word(past, &word), "Outcome::Committed(LSNS).word() == None");
}

/*
 * The fork's own: every normal xid but the last has its own xid word, which
 * is neither a free place's nor a removed one's, and the invalid, bootstrap,
 * and frozen xids have none.
 */
void
test_txn_status_tests_an_xids_word_is_no_empty_places(void)
{
	static const uint64 none[4] = {0, 1, 2, PG_UINT64_MAX};
	static const uint64 some[4] = {3, 747, PG_UINT32_MAX, PG_UINT64_MAX - 1};
	uint64		key;

	for (size_t k = 0; k < lengthof(none); k++)
	{
		HostXid		xid = {none[k]};

		STD_ASSERT(__LINE__, 0, !txn_status_key(xid, &key), "key(Xid(xid)) == None");
	}
	for (size_t k = 0; k < lengthof(some); k++)
	{
		HostXid		xid = {some[k]};

		STD_ASSERT(__LINE__, 0, txn_status_key(xid, &key) && key == some[k], "key(Xid(xid)) == Some(xid)");
	}
}

/*
 * The fork's own: a lookup's places are within the table and reach every one
 * of them, and xids taken in order start at places spread over it.
 */
void
test_txn_status_tests_a_lookup_probes_every_place_and_neighbours_start_apart(void)
{
	static const uint64 capacities[4] = {1, 2, 64, 4096};
	bool		seen[4096];
	size_t		distinct = 0;

	for (size_t c = 0; c < lengthof(capacities); c++)
	{
		uint64		capacity = capacities[c];
		uint64		start = txn_status_first(747, capacity);
		uint64		place = start;

		memset(seen, 0, sizeof(seen));
		for (uint64 k = 0; k < capacity; k++)
		{
			STD_ASSERT(__LINE__, 0, place < capacity && !seen[place], "place < capacity && !seen[place]");
			seen[place] = true;
			place = txn_status_next(place, capacity);
		}
		STD_ASSERT_EQ_U64(__LINE__, 0, place, start);
	}
	memset(seen, 0, sizeof(seen));
	for (uint64 xid = 1000; xid < 2000; xid++)
	{
		uint64		start = txn_status_first(xid, 4096);

		distinct += !seen[start];
		seen[start] = true;
	}
	STD_ASSERT(__LINE__, 0, distinct > 850, "more than 850 of 1000 neighbours start at their own place");
}

/* The fork's own: a table is crowded from three quarters of its places taken. */
void
test_txn_status_tests_a_table_is_crowded_at_three_quarters(void)
{
	STD_ASSERT(__LINE__, 0,
			   !txn_status_crowded(0, 4) && !txn_status_crowded(2, 4) && txn_status_crowded(3, 4) &&
			   txn_status_crowded(4, 4), "crowded at three of four");
	STD_ASSERT(__LINE__, 0, !txn_status_crowded(3071, 4096) && txn_status_crowded(3072, 4096),
			   "crowded at 3072 of 4096");
}

/* txn_status_tests_capacity returns the table's index capacity, read from its root. */
static uint64
txn_status_tests_capacity(HostIoIo *io)
{
	TxnStatusScope scope = {.io = io};
	HostError	error;
	uint64		capacity;

	scope.error = &error;
	if (!txn_status_hold(&scope) || !txn_status_root(&scope))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	capacity = txn_status_load(scope.root, TXN_STATUS_CAPACITY);
	txn_status_release(&scope);
	return capacity;
}

/* txn_status_tests_took takes an xid's entry with an outcome and returns whether the entry was there already. */
static bool
txn_status_tests_took(HostIoIo *io, HostXid xid, TxnStatusOutcome outcome)
{
	TxnStatusRoomed roomed;
	HostError	error;
	bool		had;

	if (!txn_status_take(io, xid, outcome, &roomed, &had, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return had;
}

/* txn_status_tests_added adds an amount to an xid's entry's count and returns whether it has an entry, with the count in a result. */
static bool
txn_status_tests_added(HostIoIo *io, HostXid xid, uint64 amount, uint64 *result)
{
	HostError	error;
	bool		some;

	if (!txn_status_add(io, xid, amount, &some, result, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return some;
}

/* txn_status_tests_looked_up looks up an xid's entry and returns whether it has one, the entry in a result. */
static bool
txn_status_tests_looked_up(HostIoIo *io, HostXid xid, TxnStatusEntry *result)
{
	HostError	error;
	bool		some;

	if (!txn_status_lookup(io, xid, &some, result, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return some;
}

/* txn_status_tests_removed removes an xid's entry and returns whether it had one. */
static bool
txn_status_tests_removed(HostIoIo *io, HostXid xid)
{
	HostError	error;
	bool		removed;

	if (!txn_status_remove(io, xid, &removed, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return removed;
}

/*
 * The fork's own (design item 7): a take of room makes an entry a lookup
 * finds, pending, with a count of zero, a second take for its xid leaves it,
 * its count adds and subtracts, a removal leaves it for no lookup, and the
 * table grows past its load factor keeping every entry; a growth leaves
 * behind an entry of zero count whose xid isn't running.
 */
void
test_txn_status_tests_entries_are_taken_found_counted_removed_and_kept_by_a_growth(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	TxnStatusOutcome unowned = {.tag = TXN_STATUS_OUTCOME_UNOWNED};
	TxnStatusOutcome pending = {.tag = TXN_STATUS_OUTCOME_PENDING_IN, .u.pending_in = 3};
	TxnStatusEntry wanted = {.outcome = pending};
	HostXid		seven = {7};
	HostXid		eight = {8};
	HostXid		ended;
	HostXid		aborted;
	HostXid		running;
	HostXid		committed[TXN_STATUS_PLACES];
	TxnStatusEntry entry;
	uint64		count;

	STD_ASSERT(__LINE__, 0, !txn_status_tests_looked_up(io, seven, &entry), "lookup(io, Xid(7)).unwrap() == None");
	STD_ASSERT(__LINE__, 0, !txn_status_tests_took(io, seven, pending), "!take(io, Xid(7), PendingIn(3)).1");
	STD_ASSERT_EQ(__LINE__, 0, txn_status_tests_looked_up(io, seven, &entry) &&
				  txn_status_Entry_PartialEq_eq(&entry, &wanted), &entry, txn_status_Entry_Debug_fmt, &wanted,
				  txn_status_Entry_Debug_fmt);
	STD_ASSERT(__LINE__, 0, txn_status_tests_took(io, seven, unowned), "take(io, Xid(7), Unowned).1");
	STD_ASSERT(__LINE__, 0, txn_status_tests_added(io, seven, 2, &count) && count == 2,
			   "add(io, Xid(7), 2) == Some(2)");
	STD_ASSERT(__LINE__, 0, txn_status_tests_added(io, seven, PG_UINT64_MAX, &count) && count == 1,
			   "add(io, Xid(7), -1) == Some(1)");
	STD_ASSERT(__LINE__, 0, !txn_status_tests_added(io, eight, 1, &count), "add(io, Xid(8), 1) == None");
	STD_ASSERT(__LINE__, 0, txn_status_tests_removed(io, seven) && !txn_status_tests_removed(io, seven),
			   "remove(io, Xid(7)) && !remove(io, Xid(7))");
	STD_ASSERT(__LINE__, 0, !txn_status_tests_looked_up(io, seven, &entry), "lookup(io, Xid(7)).unwrap() == None");
	ended = simc_begin();
	aborted = simc_begin();
	running = simc_begin();
	simc_commit(ended);
	simc_abort(aborted);
	txn_status_tests_took(io, ended, unowned);
	txn_status_tests_took(io, aborted, unowned);
	txn_status_tests_took(io, running, unowned);
	txn_status_tests_added(io, aborted, 1, &count);
	for (uint64 k = 0; k < TXN_STATUS_PLACES; k++)
		committed[k] = simc_begin();
	for (uint64 k = 0; k < TXN_STATUS_PLACES; k++)
	{
		simc_commit(committed[k]);
		txn_status_tests_took(io, committed[k], unowned);
		txn_status_tests_added(io, committed[k], 1, &count);
	}
	STD_ASSERT(__LINE__, 0, txn_status_tests_capacity(io) >= 2 * TXN_STATUS_PLACES,
			   "root.atomic::<Capacity>(Op::Load) >= 2 * PLACES");
	for (uint64 k = 0; k < TXN_STATUS_PLACES; k++)
		STD_ASSERT(__LINE__, 0, txn_status_tests_looked_up(io, committed[k], &entry) && entry.count == 1,
				   "a committed xid's entry keeps its count of 1");
	STD_ASSERT(__LINE__, 0, !txn_status_tests_looked_up(io, ended, &entry), "lookup(io, ended).unwrap() == None");
	STD_ASSERT(__LINE__, 0, !txn_status_tests_looked_up(io, aborted, &entry), "lookup(io, aborted).unwrap() == None");
	STD_ASSERT(__LINE__, 0, txn_status_tests_looked_up(io, running, &entry) && entry.count == 0,
			   "lookup(io, running).unwrap().map(|e| e.count) == Some(0)");
	simc_TestIo_drop(&test_io);
}

/*
 * The fork's own (PRevPF 5's F1): a growth is sized from the entries it
 * keeps, so churn alone keeps one segment: 40,000 transactions, each taking
 * room, counting, committing, and counting down, every other one removed and
 * the rest left at a count of zero, grow the table many times and leave it at
 * its first capacity.
 */
void
test_txn_status_tests_churn_with_no_live_entries_keeps_one_segment(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	TxnStatusOutcome pending = {.tag = TXN_STATUS_OUTCOME_PENDING_IN, .u.pending_in = 1};
	uint64		count;

	for (uint32 at = 0; at < 40000; at++)
	{
		HostXid		xid = simc_begin();

		txn_status_tests_took(io, xid, pending);
		txn_status_tests_added(io, xid, 1, &count);
		simc_commit(xid);
		txn_status_tests_added(io, xid, PG_UINT64_MAX, &count);
		if (at % 2 == 0)
			txn_status_tests_removed(io, xid);
	}
	STD_ASSERT_EQ_U64(__LINE__, 0, txn_status_tests_capacity(io), TXN_STATUS_PLACES);
	simc_TestIo_drop(&test_io);
}

/*
 * The fork's own: a growth's index capacity is the fewest whole segments of
 * places that hold what it keeps under the load factor.
 */
void
test_txn_status_tests_a_growth_holds_what_it_keeps_in_the_least_room(void)
{
	STD_ASSERT_EQ_U64(__LINE__, 0, txn_status_sized(0), TXN_STATUS_PLACES);
	STD_ASSERT_EQ_U64(__LINE__, 0, txn_status_sized(TXN_STATUS_PLACES * 3 / 4 - 1), TXN_STATUS_PLACES);
	STD_ASSERT_EQ_U64(__LINE__, 0, txn_status_sized(TXN_STATUS_PLACES * 3 / 4), 2 * TXN_STATUS_PLACES);
	STD_ASSERT_EQ_U64(__LINE__, 0, txn_status_sized(2 * TXN_STATUS_PLACES * 3 / 4 - 1), 2 * TXN_STATUS_PLACES);
	STD_ASSERT_EQ_U64(__LINE__, 0, txn_status_sized(2 * TXN_STATUS_PLACES * 3 / 4), 3 * TXN_STATUS_PLACES);
}

/* TXN_STATUS_TESTS_LISTED is LISTED, the commits' slots at their position in the test's table. */
static const HostSharedListed TXN_STATUS_TESTS_LISTED = {0};

/* txn_status_tests_roomed takes a top-level xid's room as a backend's first write does. */
static void
txn_status_tests_roomed(HostIoIo *io, HostXid top)
{
	TxnStatusRoomed roomed;
	HostError	error;

	if (!txn_status_roomed(io, top, &roomed, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/* txn_status_tests_commit returns what the table says of a top-level xid's commit. */
static TxnStatusCommit
txn_status_tests_commit(HostIoIo *io, HostXid xid)
{
	TxnStatusCommit commit;
	HostError	error;

	if (!txn_status_committed(io, xid, &commit, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return commit;
}

/* txn_status_tests_commit_is asserts at its line that the table says a commit of an xid. */
static void
txn_status_tests_commit_is(uint32 line, HostIoIo *io, HostXid xid, TxnStatusCommit wanted)
{
	TxnStatusCommit commit = txn_status_tests_commit(io, xid);

	STD_ASSERT_EQ(line, 0, txn_status_Commit_PartialEq_eq(&commit, &wanted), &commit, txn_status_Commit_Debug_fmt,
				  &wanted, txn_status_Commit_Debug_fmt);
}

/* txn_status_tests_mine returns this process's slot of the commits' words. */
static HostSharedMine
txn_status_tests_mine(HostIoIo *io)
{
	HostSharedMine mine;
	HostError	error;

	if (!host_shared_mine(HOST_IO_IO_TAKES_SHORT(io), &TXN_STATUS_COMMITS_PROC_SLOTS_OPS, &mine, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return mine;
}

/* txn_status_tests_moved moves this process's slot's pair into its entry. */
static void
txn_status_tests_moved(HostIoIo *io, const HostSharedMine *mine)
{
	HostError	error;

	if (!txn_status_moved(io, mine, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/*
 * PLAN item 7's registration, move, and reader: an entry pending in this
 * process reads not committed while its transaction runs; once the
 * transaction commits with its pair registered, a reader takes the end from
 * the slot; the move stores the end in the entry and clears the slot's xid,
 * and a second move does nothing; an entry nothing names is dropped by its
 * move; and an xid with no entry reads missing.
 */
void
test_txn_status_tests_a_commit_registered_in_its_slot_reads_back_and_its_move_settles_the_entry(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedMine mine = txn_status_tests_mine(io);
	HostXid		named = simc_begin();
	HostXid		unnamed = simc_begin();
	HostXid		absent = simc_begin();
	TxnStatusCommit pending = {.tag = TXN_STATUS_COMMIT_PENDING};
	TxnStatusCommit at = {.tag = TXN_STATUS_COMMIT_AT, .u.at = 400};
	TxnStatusCommit missing = {.tag = TXN_STATUS_COMMIT_MISSING};
	TxnStatusEntry settled = {.outcome = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = 400}, .count = 2};
	TxnStatusEntry entry;
	uint64		count;

	txn_status_tests_roomed(io, named);
	txn_status_tests_added(io, named, 2, &count);
	txn_status_tests_commit_is(__LINE__, io, named, pending);
	txn_status_registered(&TXN_STATUS_TESTS_LISTED, named, 400);
	simc_commit(named);
	txn_status_tests_commit_is(__LINE__, io, named, at);
	txn_status_tests_moved(io, &mine);
	STD_ASSERT_EQ_U64(__LINE__, 0, host_shared_Mine_load(&mine, TXN_STATUS_COMMIT_XID_INDEX), 0);
	STD_ASSERT_EQ(__LINE__, 0, txn_status_tests_looked_up(io, named, &entry) &&
				  txn_status_Entry_PartialEq_eq(&entry, &settled), &entry, txn_status_Entry_Debug_fmt, &settled,
				  txn_status_Entry_Debug_fmt);
	txn_status_tests_moved(io, &mine);
	txn_status_tests_commit_is(__LINE__, io, named, at);
	txn_status_tests_roomed(io, unnamed);
	txn_status_registered(&TXN_STATUS_TESTS_LISTED, unnamed, 500);
	simc_commit(unnamed);
	txn_status_tests_moved(io, &mine);
	STD_ASSERT(__LINE__, 0, !txn_status_tests_looked_up(io, unnamed, &entry), "lookup(io, unnamed).unwrap() == None");
	txn_status_tests_commit_is(__LINE__, io, absent, missing);
	simc_TestIo_drop(&test_io);
}

/*
 * The fold text's 3.1 (draft 10): a fold's point lies below the end of a
 * commit registered at or below the insert position whose transaction is
 * still in the proc array, and is the position once it left, or when the
 * commit ends past the position.
 */
void
test_txn_status_tests_a_folds_point_lies_below_a_commit_still_in_the_proc_array(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedMine mine = txn_status_tests_mine(io);
	HostXid		held = simc_begin();
	HostError	error;
	HostLsn		point;
	uint64		count;

	txn_status_tests_roomed(io, held);
	txn_status_tests_added(io, held, 1, &count);
	txn_status_registered(&TXN_STATUS_TESTS_LISTED, held, 400);
	if (!txn_status_lowered(io, 500, &point, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ_U64(__LINE__, 0, point, 399);
	if (!txn_status_lowered(io, 300, &point, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ_U64(__LINE__, 0, point, 300);
	simc_commit(held);
	if (!txn_status_lowered(io, 500, &point, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ_U64(__LINE__, 0, point, 500);
	txn_status_tests_moved(io, &mine);
	simc_TestIo_drop(&test_io);
}

/*
 * PLAN item 7's take of room: a backend's first write in a transaction moves
 * its slot's pair, here a committed earlier transaction's, and takes its
 * top-level xid's room pending in this process; a second write takes
 * nothing.
 */
void
test_txn_status_tests_a_take_of_room_moves_the_slots_pair_first(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	TxnStatusEntry earlier_entry = {.outcome = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = 700},
	.count = 1};
	TxnStatusEntry later_entry = {.outcome = {.tag = TXN_STATUS_OUTCOME_PENDING_IN,
	.u.pending_in = host_shared_proc_number()}};
	HostXid		earlier = simc_begin();
	HostXid		later = simc_begin();
	TxnStatusEntry entry;
	uint64		count;

	txn_status_tests_roomed(io, earlier);
	txn_status_tests_added(io, earlier, 1, &count);
	txn_status_registered(&TXN_STATUS_TESTS_LISTED, earlier, 700);
	simc_commit(earlier);
	txn_status_tests_roomed(io, later);
	STD_ASSERT_EQ(__LINE__, 0, txn_status_tests_looked_up(io, earlier, &entry) &&
				  txn_status_Entry_PartialEq_eq(&entry, &earlier_entry), &entry, txn_status_Entry_Debug_fmt,
				  &earlier_entry, txn_status_Entry_Debug_fmt);
	STD_ASSERT_EQ(__LINE__, 0, txn_status_tests_looked_up(io, later, &entry) &&
				  txn_status_Entry_PartialEq_eq(&entry, &later_entry), &entry, txn_status_Entry_Debug_fmt,
				  &later_entry, txn_status_Entry_Debug_fmt);
	txn_status_tests_added(io, later, 1, &count);
	txn_status_tests_roomed(io, later);
	STD_ASSERT(__LINE__, 0, txn_status_tests_looked_up(io, later, &entry) && entry.count == 1,
			   "lookup(io, later).unwrap().map(|e| e.count) == Some(1)");
	simc_TestIo_drop(&test_io);
}

/*
 * PLAN item 7's redo call site: a replayed commit stores its record's end in
 * the entry its replayed row change took, pending in no process, and a commit
 * of a transaction with no entry stores nothing.
 */
void
test_txn_status_tests_a_replayed_commit_stores_its_end_in_the_entry_its_replay_took(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	TxnStatusOutcome at = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = 900};
	HostXid		wrote = simc_begin();
	HostXid		quiet = simc_begin();
	HostXid		sub = simc_begin_under(wrote);
	TxnMemtableVersionWriter writer = {.xid = sub.f0, .has_top = true, .top = wrote.f0};
	TxnStatusRoomed roomed;
	TxnStatusEntry entry;
	HostError	error;
	bool		some;

	if (!txn_status_redo_roomed(io, &writer, &some, &roomed, &error) ||
		!txn_status_redo_committed(io, wrote, 900, &error) || !txn_status_redo_committed(io, quiet, 950, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(__LINE__, 0, txn_status_tests_looked_up(io, wrote, &entry) &&
				  txn_status_Outcome_PartialEq_eq(&entry.outcome, &at), &entry.outcome,
				  txn_status_Outcome_Debug_fmt, &at, txn_status_Outcome_Debug_fmt);
	STD_ASSERT(__LINE__, 0, !txn_status_tests_looked_up(io, quiet, &entry), "lookup(io, quiet).unwrap() == None");
	simc_TestIo_drop(&test_io);
}

/*
 * PLAN item 7's prepared transaction: its entry, left pending in its
 * preparing backend's number, is pointed at the committing process ahead of
 * the critical section, whose registration then stores its end, which a
 * reader takes from the committer's slot; a prepared transaction with no
 * entry registers nothing.
 */
void
test_txn_status_tests_a_prepared_commit_points_its_entry_at_the_committer_and_registers(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	TxnStatusOutcome preparer = {.tag = TXN_STATUS_OUTCOME_PENDING_IN, .u.pending_in = 77};
	TxnStatusEntry pointed = {.outcome = {.tag = TXN_STATUS_OUTCOME_PENDING_IN,
	.u.pending_in = host_shared_proc_number()}, .count = 1};
	TxnStatusCommit at = {.tag = TXN_STATUS_COMMIT_AT, .u.at = 1200};
	TxnStatusCommit missing = {.tag = TXN_STATUS_COMMIT_MISSING};
	HostXid		prepared = simc_begin();
	HostXid		empty = simc_begin();
	TxnStatusEntry entry;
	HostError	error;
	uint64		count;

	txn_status_tests_took(io, prepared, preparer);
	txn_status_tests_added(io, prepared, 1, &count);
	if (!txn_status_prepared_committing(io, prepared, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(__LINE__, 0, txn_status_tests_looked_up(io, prepared, &entry) &&
				  txn_status_Entry_PartialEq_eq(&entry, &pointed), &entry, txn_status_Entry_Debug_fmt, &pointed,
				  txn_status_Entry_Debug_fmt);
	txn_status_prepared_registered(&TXN_STATUS_TESTS_LISTED, 1200);
	simc_commit(prepared);
	txn_status_tests_commit_is(__LINE__, io, prepared, at);
	if (!txn_status_prepared_committing(io, empty, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_status_prepared_registered(&TXN_STATUS_TESTS_LISTED, 1300);
	simc_commit(empty);
	txn_status_tests_commit_is(__LINE__, io, prepared, at);
	txn_status_tests_commit_is(__LINE__, io, empty, missing);
	simc_TestIo_drop(&test_io);
}

/*
 * The fold text's 3.11: a growth that rebuilds the index between a take of
 * room and a count leaves the entry in its slot, so the count lands in the
 * same entry, and a removed entry's slot is the next take's.
 */
void
test_txn_status_tests_a_slot_stays_its_entrys_across_a_growth_and_a_removed_ones_is_taken_again(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	TxnStatusOutcome unowned = {.tag = TXN_STATUS_OUTCOME_UNOWNED};
	HostXid		counted_one = simc_begin();
	HostXid		gone;
	HostXid		next;
	TxnStatusRoomed roomed;
	TxnStatusRoomed freed_slot;
	TxnStatusRoomed again;
	TxnStatusEntry entry;
	HostDeltaHold hold;
	HostError	error;
	uint64		capacity;
	uint64		count;
	bool		had;

	if (!txn_status_take(io, counted_one, unowned, &roomed, &had, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	capacity = txn_status_tests_capacity(io);
	while (txn_status_tests_capacity(io) == capacity)
	{
		HostXid		xid = simc_begin();

		txn_status_tests_took(io, xid, unowned);
		txn_status_tests_added(io, xid, 1, &count);
	}
	if (!host_delta_hold(io, &hold, &error) ||
		!txn_status_counted(HOST_IO_IO_TAKES_SHORT(io), &hold, roomed, 1, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	host_delta_Hold_drop(&hold);
	STD_ASSERT(__LINE__, 0, txn_status_tests_looked_up(io, counted_one, &entry) && entry.count == 1,
			   "lookup(io, counted_one).unwrap().map(|e| e.count) == Some(1)");
	gone = simc_begin();
	next = simc_begin();
	if (!txn_status_take(io, gone, unowned, &freed_slot, &had, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, txn_status_tests_removed(io, gone), "remove(io, gone).unwrap()");
	if (!txn_status_take(io, next, unowned, &again, &had, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(__LINE__, 0, txn_status_Roomed_PartialEq_eq(&again, &freed_slot) && !had, &again,
				  txn_status_Roomed_Debug_fmt, &freed_slot, txn_status_Roomed_Debug_fmt);
	simc_TestIo_drop(&test_io);
}

/* txn_status_tests_grown takes committed xids' room, each counted once, until the table grows. */
static void
txn_status_tests_grown(HostIoIo *io)
{
	TxnStatusOutcome unowned = {.tag = TXN_STATUS_OUTCOME_UNOWNED};
	uint64		capacity = txn_status_tests_capacity(io);
	uint64		count;

	while (txn_status_tests_capacity(io) == capacity)
	{
		HostXid		xid = simc_begin();

		simc_commit(xid);
		txn_status_tests_took(io, xid, unowned);
		txn_status_tests_added(io, xid, 1, &count);
	}
}

/*
 * Bugs row 90: while the proc array doesn't answer for every running and
 * prepared transaction, as in crash recovery before its end puts the prepared
 * ones in it, a growth keeps an entry of zero count whose xid isn't in
 * progress, which a growth with the proc array answering leaves behind; one
 * with a count whose xid aborted goes either way, since the commit log
 * answers it.
 */
void
test_txn_status_tests_a_growth_while_the_proc_array_cant_answer_keeps_an_ended_entry(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	TxnStatusOutcome unowned = {.tag = TXN_STATUS_OUTCOME_UNOWNED};
	TxnStatusEntry kept = {.outcome = unowned};
	HostXid		ended = simc_begin();
	HostXid		aborted = simc_begin();
	TxnStatusEntry entry;
	uint64		count;

	simc_commit(ended);
	simc_abort(aborted);
	txn_status_tests_took(io, ended, unowned);
	txn_status_tests_took(io, aborted, unowned);
	txn_status_tests_added(io, aborted, 1, &count);
	simc_unanswered(true);
	txn_status_tests_grown(io);
	simc_unanswered(false);
	STD_ASSERT_EQ(__LINE__, 0, txn_status_tests_looked_up(io, ended, &entry) &&
				  txn_status_Entry_PartialEq_eq(&entry, &kept), &entry, txn_status_Entry_Debug_fmt, &kept,
				  txn_status_Entry_Debug_fmt);
	STD_ASSERT(__LINE__, 0, !txn_status_tests_looked_up(io, aborted, &entry), "lookup(io, aborted).unwrap() == None");
	txn_status_tests_grown(io);
	STD_ASSERT(__LINE__, 0, !txn_status_tests_looked_up(io, ended, &entry), "lookup(io, ended).unwrap() == None");
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_TXN */
