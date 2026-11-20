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
 * The delta's memory in the server (design item 14's S6): the page
 * directory, a fixed region of a header, one slot of eight words for each 1
 * KiB of delta_memory, three words for each block of the region, and a word of
 * the free hint for each 64 slots below the block past the budget's (bugs row
 * 150); and the region of blocks, a region of its own that holds the pages'
 * bodies and nothing else (bugs row 116). The postmaster makes both. Postgres
 * keeps the regions' addresses in pg's C globals, so this module keeps only
 * numbers and translates them inside a call.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/shmem/delta.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "pg_calls.h"

#define RUST_FILE "shmem" STD_SEP "src" STD_SEP "delta.rs"

/* SHMEM_DELTA_WAIT is the name of the wait event of a wait at the delta's budget. */
static const char SHMEM_DELTA_WAIT[] = "versioned delta budget";

/* SHMEM_DELTA_REFUSAL is the most bytes of a refusal's detail, its ending zero byte included. */
#define SHMEM_DELTA_REFUSAL ((size_t) 384)

/* SHMEM_DELTA_SLOTS is the offset of the slots in the directory: after the header's ten cache lines. */
#define SHMEM_DELTA_SLOTS ((size_t) 640)

/*
 * ShmemDeltaHeader is the directory's fixed part, on its first cache lines:
 * the count of frees, the hand of an allocation's sweep, the allocation lock,
 * the condition variable of the budget's wait with its wait event, the count
 * of slots, the overshoot word, the region's words, lock, and count of
 * blocks, the fold launcher's asks and process, and a standby's root
 * mismatch.
 */
typedef struct ShmemDeltaHeader
{
	pg_atomic_uint64 frees;
	pg_atomic_uint64 hand;

	/*
	 * left counts the retirements since the last free pass that left nothing
	 * retired unfreed.
	 */
	pg_atomic_uint64 left;

	/*
	 * kept is the lowest stamp of a retirement a free pass found and couldn't
	 * free, zero for none (bugs row 124).
	 */
	pg_atomic_uint64 kept;
	/* waiting counts the processes asleep at the budget, or about to sleep. */
	pg_atomic_uint64 waiting;

	/*
	 * overshoot is the slot plus one of the page whose compaction took a page
	 * past the budget, 0 while none has (bugs row 110).
	 */
	pg_atomic_uint64 overshoot;

	/*
	 * spent counts the slots below the block past the budget that are spent,
	 * and spent_over those past it (bugs row 150).
	 */
	pg_atomic_uint64 spent;
	pg_atomic_uint64 spent_over;

	/*
	 * region holds the region's words, which change only under its lock (bugs
	 * row 116).
	 */
	pg_atomic_uint64 region[SHMEM_DELTA_REGION_WORDS];

	/*
	 * asked counts the folds the budget's waiters asked of the fold launcher,
	 * which it compares with the count it last saw.
	 */
	pg_atomic_uint64 asked;

	/*
	 * launcher is the fold launcher's process number plus one, 0 before it
	 * starts, whose latch an ask sets.
	 */
	pg_atomic_uint64 launcher;

	/*
	 * refused is 0 until a standby's fold finds a root other than its
	 * primary's, 1 while that fold writes the refusal's detail, and 2 once it
	 * is written.
	 */
	pg_atomic_uint64 refused;

	/*
	 * refusal is the detail of the startup process's FATAL, ended by a zero
	 * byte, once refused is 2.
	 */
	uint8		refusal[SHMEM_DELTA_REFUSAL];
	LWLock		lock;
	/* region_lock is the region's lock, held only for a block's take or give. */
	LWLock		region_lock;
	ConditionVariable wait;
	uint32		wait_event;
	uint32		slots;
	uint32		blocks;
} ShmemDeltaHeader;

/* ShmemDeltaSlot is a slot of the directory: its generation-and-state word and its other words, which fill its cache line. */
typedef struct ShmemDeltaSlot
{
	pg_atomic_uint64 word;
	pg_atomic_uint64 fields[SHMEM_DELTA_FIELDS];
} ShmemDeltaSlot;

/*
 * The header fits before the slots, a slot is one cache line, and an atomic
 * is its value alone, so that zeroed memory is slots and words at zero.
 */
StaticAssertDecl(sizeof(ShmemDeltaHeader) <= SHMEM_DELTA_SLOTS && sizeof(ShmemDeltaSlot) == 64 &&
				 sizeof(pg_atomic_uint64) == 8, "the delta's directory is laid out as the Rust's");

/* shmem_delta_header returns the directory's header. */
static ShmemDeltaHeader *
shmem_delta_header(void)
{
	return (ShmemDeltaHeader *) pg_calls_delta_directory();
}

/* shmem_delta_slot returns the address of a slot of the directory. */
static ShmemDeltaSlot *
shmem_delta_slot(uint32 at)
{
	uint8	   *base = (uint8 *) pg_calls_delta_directory();
	size_t		tmp_1 = STD_MUL(usize, (size_t) at, sizeof(ShmemDeltaSlot), 110, 61);
	size_t		tmp_2 = STD_ADD(usize, SHMEM_DELTA_SLOTS, tmp_1, 110, 53);

	return (ShmemDeltaSlot *) STD_PTR_AT(base, tmp_2, 1);
}

/* shmem_delta_field returns the address of one of a slot's other words. */
static pg_atomic_uint64 *
shmem_delta_field(uint32 at, size_t field)
{
	if (!(field < SHMEM_DELTA_FIELDS))
		STD_PANIC(115, 5, "a slot's word is one of its seven");
	return (pg_atomic_uint64 *) STD_PTR_AT(shmem_delta_slot(at)->fields, field, sizeof(pg_atomic_uint64));
}

/* shmem_delta_block_word returns the address of one of a block's words, which follow the directory's slots. */
static pg_atomic_uint64 *
shmem_delta_block_word(uint32 at, size_t which)
{
	size_t		tmp_1;
	size_t		slots;
	size_t		tmp_2;
	size_t		tmp_3;
	size_t		tmp_4;
	size_t		offset;

	if (!(which < SHMEM_DELTA_BLOCK_WORDS))
		STD_PANIC(123, 5, "a block's word is one of its three");
	tmp_1 = STD_MUL(usize, (size_t) shmem_delta_slots(), sizeof(ShmemDeltaSlot), 124, 25);
	slots = STD_ADD(usize, SHMEM_DELTA_SLOTS, tmp_1, 124, 17);
	tmp_2 = STD_MUL(usize, (size_t) at, SHMEM_DELTA_BLOCK_WORDS, 125, 27);
	tmp_3 = STD_ADD(usize, tmp_2, which, 125, 27);
	tmp_4 = STD_MUL(usize, tmp_3, sizeof(pg_atomic_uint64), 125, 26);
	offset = STD_ADD(usize, slots, tmp_4, 125, 18);
	return (pg_atomic_uint64 *) STD_PTR_AT(pg_calls_delta_directory(), offset, 1);
}

/*
 * shmem_delta_hint_word returns the address of one of the free hint's words,
 * which follow the blocks' words: one for each SHMEM_DELTA_HINTED slots below
 * the block past the budget (bugs row 150).
 */
static pg_atomic_uint64 *
shmem_delta_hint_word(uint32 word)
{
	size_t		tmp_1;
	size_t		tmp_2;
	size_t		tmp_3;
	size_t		tmp_4;
	size_t		blocks;
	size_t		tmp_5;

	if (!(word < (uint32) shmem_delta_hints((size_t) shmem_delta_slots())))
		STD_PANIC(132, 5, "a word of the free hint is one of its slots' words");
	tmp_1 = STD_MUL(usize, (size_t) shmem_delta_slots(), sizeof(ShmemDeltaSlot), 133, 26);
	tmp_2 = STD_ADD(usize, SHMEM_DELTA_SLOTS, tmp_1, 133, 18);
	tmp_3 = STD_MUL(usize, (size_t) shmem_delta_blocks(), SHMEM_DELTA_BLOCK_WORDS, 133, 65);
	tmp_4 = STD_MUL(usize, tmp_3, 8, 133, 65);
	blocks = STD_ADD(usize, tmp_2, tmp_4, 133, 18);
	tmp_5 = STD_MUL(usize, (size_t) word, 8, 134, 62);
	return (pg_atomic_uint64 *) STD_PTR_AT(pg_calls_delta_directory(), STD_ADD(usize, blocks, tmp_5, 134, 53), 1);
}

/*
 * shmem_delta_hints returns the count of the free hint's words of a directory
 * of a count of slots: one for each SHMEM_DELTA_HINTED of those below the
 * block past the budget, whose slots are a block's last.
 */
size_t
shmem_delta_hints(size_t slots)
{
	size_t		tmp_1 = STD_SUB(usize, slots, (size_t) SHMEM_DELTA_HINTED, 140, 6);

	return STD_DIV_FOLDED(usize, tmp_1, (size_t) SHMEM_DELTA_HINTED);
}

/*
 * shmem_delta_request asks Postgres for the delta's two regions, in the
 * postmaster's request callback: the region of blocks, of its bytes, and the
 * directory, of its header, its slots, its blocks' words, and the free
 * hint's words.
 */
bool
shmem_delta_request(const PgMayRaise *raise, size_t slots, size_t blocks, size_t bytes, PgPgError *error)
{
	size_t		tmp_1 = STD_MUL(usize, blocks, SHMEM_DELTA_BLOCK_WORDS, 146, 18);
	size_t		tmp_2 = STD_ADD(usize, tmp_1, shmem_delta_hints(slots), 146, 18);
	size_t		words = STD_MUL(usize, tmp_2, sizeof(pg_atomic_uint64), 146, 17);
	size_t		tmp_3 = STD_MUL(usize, slots, sizeof(ShmemDeltaSlot), 147, 45);
	size_t		tmp_4 = STD_ADD(usize, SHMEM_DELTA_SLOTS, tmp_3, 147, 37);
	size_t		tmp_5 = STD_ADD(usize, tmp_4, words, 147, 37);

	return pg_calls_request_delta(raise, bytes, tmp_5, error);
}

/*
 * shmem_delta_initialize sets the delta's directory up in the postmaster's
 * initialization callback, after the fork's region: the header, every slot
 * free at its first generation, every block free and never taken, and every
 * bit of the free hint set, since every slot is free. The region's bodies are
 * zeroed as each is taken.
 */
bool
shmem_delta_initialize(const PgMayRaise *raise, size_t slots, size_t blocks, PgPgError *error)
{
	uint32		wait_event;
	int32		tranche;
	ShmemDeltaHeader *header;
	size_t		tmp_1;
	size_t		words;
	size_t		tmp_2;
	size_t		tmp_3;
	size_t		tmp_4;
	size_t		tmp_5;

	if (!pg_calls_wait_event_new(raise, SHMEM_DELTA_WAIT, &wait_event, error))
		return false;
	tranche = ((ShmemControl *) pg_calls_shmem_base())->tranche;
	header = shmem_delta_header();
	pg_calls_atomic_init_u64(&header->frees, 0);
	pg_calls_atomic_init_u64(&header->hand, 0);
	pg_calls_atomic_init_u64(&header->left, 0);
	pg_calls_atomic_init_u64(&header->kept, 0);
	pg_calls_atomic_init_u64(&header->waiting, 0);
	pg_calls_atomic_init_u64(&header->overshoot, 0);
	pg_calls_atomic_init_u64(&header->spent, 0);
	pg_calls_atomic_init_u64(&header->spent_over, 0);
	for (size_t word = 0, word_end = SHMEM_DELTA_REGION_WORDS; word < word_end;
		 word = std_forward_unchecked_usize_Checked(word, 1))
		pg_calls_atomic_init_u64((pg_atomic_uint64 *) STD_PTR_AT(header->region, word, sizeof(pg_atomic_uint64)), 0);
	pg_calls_atomic_init_u64(&header->asked, 0);
	pg_calls_atomic_init_u64(&header->launcher, 0);
	pg_calls_atomic_init_u64(&header->refused, 0);
	pg_calls_lwlock_initialize(&header->lock, tranche);
	pg_calls_lwlock_initialize(&header->region_lock, tranche);
	pg_calls_cv_init(&header->wait);
	header->wait_event = wait_event;
	header->slots = (uint32) slots;
	header->blocks = (uint32) blocks;
	tmp_1 = STD_MUL(usize, blocks, SHMEM_DELTA_BLOCK_WORDS, 183, 21);
	words = STD_MUL(usize, tmp_1, sizeof(pg_atomic_uint64), 183, 21);
	tmp_2 = STD_MUL(usize, slots, sizeof(ShmemDeltaSlot), 184, 45);
	tmp_3 = STD_ADD(usize, tmp_2, words, 184, 45);
	std_ptr_write_bytes(shmem_delta_slot(0), 0, tmp_3, 1, 1);
	tmp_4 = STD_MUL(usize, slots, sizeof(ShmemDeltaSlot), 185, 43);
	tmp_5 = STD_ADD(usize, tmp_4, words, 185, 43);
	std_ptr_write_bytes(STD_PTR_AT(shmem_delta_slot(0), tmp_5, 1), 0xff,
						STD_MUL(usize, shmem_delta_hints(slots), 8, 185, 96), 1, 1);
	return true;
}

/* shmem_delta_blocks returns the region's count of blocks. */
uint32
shmem_delta_blocks(void)
{
	return shmem_delta_header()->blocks;
}

/* shmem_delta_region loads one of the region's words. */
uint64
shmem_delta_region(size_t which)
{
	if (!(which < SHMEM_DELTA_REGION_WORDS))
		STD_PANIC(199, 5, "a region's word is one of its eleven");
	return pg_calls_atomic_read_u64((pg_atomic_uint64 *) STD_PTR_AT(shmem_delta_header()->region, which,
																	sizeof(pg_atomic_uint64)));
}

/* shmem_delta_set_region stores one of the region's words, under the region's lock. */
void
shmem_delta_set_region(size_t which, uint64 value)
{
	if (!(which < SHMEM_DELTA_REGION_WORDS))
		STD_PANIC(207, 5, "a region's word is one of its eleven");
	pg_calls_atomic_write_u64((pg_atomic_uint64 *) STD_PTR_AT(shmem_delta_header()->region, which,
															  sizeof(pg_atomic_uint64)), value);
}

/* shmem_delta_block loads one of a block's words. */
uint64
shmem_delta_block(uint32 at, size_t which)
{
	return pg_calls_atomic_read_u64(shmem_delta_block_word(at, which));
}

/* shmem_delta_set_block stores one of a block's words, under the region's lock. */
void
shmem_delta_set_block(uint32 at, size_t which, uint64 value)
{
	pg_calls_atomic_write_u64(shmem_delta_block_word(at, which), value);
}

/*
 * shmem_delta_lock_region takes the region's lock exclusively, which can't
 * fail; its caller holds it only for a block's take or give.
 */
void
shmem_delta_lock_region(void)
{
	pg_calls_lwlock_acquire_sure(&shmem_delta_header()->region_lock, LW_EXCLUSIVE);
}

/* shmem_delta_unlock_region releases the region's lock, which its caller holds. */
void
shmem_delta_unlock_region(void)
{
	pg_calls_lwlock_release(&shmem_delta_header()->region_lock);
}

/* shmem_delta_slots returns the directory's count of slots. */
uint32
shmem_delta_slots(void)
{
	return shmem_delta_header()->slots;
}

/* shmem_delta_word loads a slot's generation-and-state word. */
uint64
shmem_delta_word(uint32 at)
{
	return pg_calls_atomic_read_u64(&shmem_delta_slot(at)->word);
}

/*
 * shmem_delta_swap stores a slot's generation-and-state word if the slot
 * still holds the word its caller loaded, reporting whether it did.
 */
bool
shmem_delta_swap(uint32 at, uint64 loaded, uint64 word)
{
	uint64		seen = loaded;

	pg_calls_atomic_exchange_u64(&shmem_delta_slot(at)->word, &seen, word);
	return seen == loaded;
}

/* shmem_delta_load loads one of a slot's other words. */
uint64
shmem_delta_load(uint32 at, size_t which)
{
	return pg_calls_atomic_read_u64(shmem_delta_field(at, which));
}

/* shmem_delta_store stores one of a slot's other words. */
void
shmem_delta_store(uint32 at, size_t which, uint64 value)
{
	pg_calls_atomic_write_u64(shmem_delta_field(at, which), value);
}

/* shmem_delta_hint loads a word of the free hint. */
uint64
shmem_delta_hint(uint32 word)
{
	return pg_calls_atomic_read_u64(shmem_delta_hint_word(word));
}

/* shmem_delta_hint_set sets a slot's bit of the free hint. */
void
shmem_delta_hint_set(uint32 slot)
{
	pg_atomic_uint64 *word = shmem_delta_hint_word(STD_DIV_FOLDED(u32, slot, SHMEM_DELTA_HINTED));
	uint64		tmp_1 = STD_SHL(u64, 1, (uint64) STD_REM_FOLDED(u32, slot, SHMEM_DELTA_HINTED), 293, 58);

	(void) pg_calls_atomic_or_u64(word, tmp_1);
}

/* shmem_delta_hint_clear clears a slot's bit of the free hint. */
void
shmem_delta_hint_clear(uint32 slot)
{
	pg_atomic_uint64 *word = shmem_delta_hint_word(STD_DIV_FOLDED(u32, slot, SHMEM_DELTA_HINTED));
	uint64		tmp_1 = STD_SHL(u64, 1, (uint64) STD_REM_FOLDED(u32, slot, SHMEM_DELTA_HINTED), 300, 61);

	(void) pg_calls_atomic_and_u64(word, ~tmp_1);
}

/* shmem_delta_spent loads the count of the spent slots below the block past the budget, or past it. */
uint64
shmem_delta_spent(bool over)
{
	if (!over)
		return pg_calls_atomic_read_u64(&shmem_delta_header()->spent);
	return pg_calls_atomic_read_u64(&shmem_delta_header()->spent_over);
}

/* shmem_delta_spend counts a spent slot below the block past the budget, or past it. */
void
shmem_delta_spend(bool over)
{
	if (!over)
		(void) pg_calls_atomic_add_u64(&shmem_delta_header()->spent, 1);
	else
		(void) pg_calls_atomic_add_u64(&shmem_delta_header()->spent_over, 1);
}

/* shmem_delta_frees returns the count of frees. */
uint64
shmem_delta_frees(void)
{
	return pg_calls_atomic_read_u64(&shmem_delta_header()->frees);
}

/* shmem_delta_freed adds one to the count of frees and wakes the budget's waiters. */
void
shmem_delta_freed(void)
{
	(void) pg_calls_atomic_add_u64(&shmem_delta_header()->frees, 1);
	pg_calls_cv_broadcast(&shmem_delta_header()->wait);
}

/* shmem_delta_left returns the count of retirements since the last free pass that left nothing retired unfreed. */
uint64
shmem_delta_left(void)
{
	return pg_calls_atomic_read_u64(&shmem_delta_header()->left);
}

/* shmem_delta_leave adds one to the count of retirements. */
void
shmem_delta_leave(void)
{
	(void) pg_calls_atomic_add_u64(&shmem_delta_header()->left, 1);
}

/* shmem_delta_settle stores zero over the count of retirements if it still holds the count a free pass loaded. */
void
shmem_delta_settle(uint64 loaded)
{
	uint64		seen = loaded;

	pg_calls_atomic_exchange_u64(&shmem_delta_header()->left, &seen, 0);
}

/* shmem_delta_kept returns the lowest stamp of a retirement a free pass kept, zero for none. */
uint64
shmem_delta_kept(void)
{
	return pg_calls_atomic_read_u64(&shmem_delta_header()->kept);
}

/* shmem_delta_kept_swap stores the kept stamp over the one loaded, reporting whether it was still there. */
bool
shmem_delta_kept_swap(uint64 loaded, uint64 word)
{
	uint64		seen = loaded;

	pg_calls_atomic_exchange_u64(&shmem_delta_header()->kept, &seen, word);
	return seen == loaded;
}

/* shmem_delta_waiting returns the count of processes asleep at the budget, or about to sleep. */
uint64
shmem_delta_waiting(void)
{
	return pg_calls_atomic_read_u64(&shmem_delta_header()->waiting);
}

/* shmem_delta_overshoot loads the overshoot word. */
uint64
shmem_delta_overshoot(void)
{
	return pg_calls_atomic_read_u64(&shmem_delta_header()->overshoot);
}

/* shmem_delta_overshoot_swap stores the overshoot word over the one loaded, reporting whether it was still there. */
bool
shmem_delta_overshoot_swap(uint64 loaded, uint64 word)
{
	uint64		seen = loaded;

	pg_calls_atomic_exchange_u64(&shmem_delta_header()->overshoot, &seen, word);
	return seen == loaded;
}

/* shmem_delta_count_waiter adds one to the count of the budget's waiters, or takes one away. */
void
shmem_delta_count_waiter(bool add)
{
	(void) pg_calls_atomic_add_u64(&shmem_delta_header()->waiting, add ? 1 : -1);
}

/* shmem_delta_hand returns the slot an allocation's sweep starts at, and moves the hand on. */
uint32
shmem_delta_hand(void)
{
	uint64		hand = pg_calls_atomic_add_u64(&shmem_delta_header()->hand, 1);
	uint64		tmp_1 = (uint64) std_max_u32(shmem_delta_slots(), 1);

	return (uint32) STD_REM(u64, hand, tmp_1, 417, 6);
}

/* shmem_delta_lock takes the allocation lock exclusively. */
bool
shmem_delta_lock(const PgMayRaise *raise, PgPgError *error)
{
	return pg_calls_lwlock_acquire(raise, &shmem_delta_header()->lock, LW_EXCLUSIVE, error);
}

/* shmem_delta_unlock releases the allocation lock, which its caller holds. */
void
shmem_delta_unlock(void)
{
	pg_calls_lwlock_release(&shmem_delta_header()->lock);
}

/*
 * shmem_delta_wait sleeps on the budget's condition variable while the count
 * of frees is the one given, having first asked the fold launcher for folds
 * that free the delta (shmem_delta_ask_fold): it checks the count again once
 * it is prepared to sleep, so a free counted before the sleep ends it at
 * once. A cancel raises Postgres' error, whose cleanup takes the process off
 * the variable.
 */
bool
shmem_delta_wait(const PgMayRaise *raise, uint64 seen, PgPgError *error)
{
	uint32		event;

	shmem_delta_ask_fold();
	event = shmem_delta_header()->wait_event;
	for (;;)
	{
		if (shmem_delta_frees() != seen)
		{
			pg_calls_cv_cancel();
			return true;
		}
		if (!pg_calls_cv_sleep(raise, &shmem_delta_header()->wait, event, error))
			return false;
	}
}

static uint8 *shmem_delta_address(uint64 pointer);

/* shmem_delta_zero zeroes a body's first bytes, which its take alone reaches. */
void
shmem_delta_zero(uint64 pointer, uint64 len)
{
	std_ptr_write_bytes(shmem_delta_address(pointer), 0, (size_t) len, 1, 1);
}

/*
 * shmem_delta_ask_fold asks the fold launcher for folds of the storages
 * holding the most of the delta, by moving the count of asks and setting the
 * launcher's latch once it has started, as a waiter at the budget and the
 * checkpointer do; a process whose server made no regions for the fork, a
 * bootstrap one, whose checkpoint calls this, asks nothing.
 */
void
shmem_delta_ask_fold(void)
{
	uint64		launcher;

	if (pg_calls_delta_directory() == NULL)
		return;
	(void) pg_calls_atomic_add_u64(&shmem_delta_header()->asked, 1);
	launcher = pg_calls_atomic_read_u64(&shmem_delta_header()->launcher);
	if (launcher != 0)
		pg_calls_set_proc_latch((int32) STD_SUB(u64, launcher, 1, 476, 29));
}

/* shmem_delta_fold_asked returns the count of folds asked of the fold launcher, which it compares with the count it last saw. */
uint64
shmem_delta_fold_asked(void)
{
	return pg_calls_atomic_read_u64(&shmem_delta_header()->asked);
}

/* shmem_delta_launcher_started records the running process as the fold launcher, whose latch an ask sets. */
void
shmem_delta_launcher_started(void)
{
	uint64		tmp_1 = (uint64) (uint32) pg_calls_my_proc_number();
	uint64		number = STD_ADD(u64, tmp_1, 1, 489, 18);

	pg_calls_atomic_write_u64(&shmem_delta_header()->launcher, number);
}

/*
 * shmem_delta_launcher_running says whether the fold launcher has started
 * since the server's shared memory was made, so the startup process leaves
 * the folds of recovery to its workers (bugs row 107).
 */
bool
shmem_delta_launcher_running(void)
{
	if (pg_calls_delta_directory() == NULL)
		return false;
	return pg_calls_atomic_read_u64(&shmem_delta_header()->launcher) != 0;
}

/*
 * shmem_delta_refuse publishes the first root mismatch a standby's fold
 * found, with the detail the startup process's FATAL reports, cut to fit, and
 * wakes the startup process, which raises it.
 */
void
shmem_delta_refuse(const char *detail, size_t detail_len)
{
	ShmemDeltaHeader *header;
	uint64		seen = 0;
	size_t		length;
	uint8	   *refusal;

	if (pg_calls_delta_directory() == NULL)
		return;
	header = shmem_delta_header();
	length = std_min_usize(detail_len, (size_t) (SHMEM_DELTA_REFUSAL - 1));
	pg_calls_atomic_exchange_u64(&header->refused, &seen, 1);
	if (seen != 0)
		return;
	refusal = header->refusal;
	std_ptr_copy_nonoverlapping_Checked(detail, refusal, length, 1, 1);
	*(uint8 *) STD_PTR_AT(refusal, length, 1) = 0;
	(void) pg_calls_atomic_add_u64(&header->refused, 1);
	pg_calls_wakeup_recovery();
}

/*
 * shmem_delta_refusal returns the detail of a published root mismatch,
 * nothing before one, with one read of a word while there is none, since the
 * startup process asks at each record it replays.
 */
bool
shmem_delta_refusal(StdCString *result)
{
	ShmemDeltaHeader *header;
	uint8		refusal[SHMEM_DELTA_REFUSAL];
	size_t		length = SHMEM_DELTA_REFUSAL - 1;
	StdNulError nul;

	if (pg_calls_delta_directory() == NULL)
		return false;
	header = shmem_delta_header();
	if (pg_calls_atomic_read_u64(&header->refused) == 0 || pg_calls_atomic_add_u64(&header->refused, 0) != 2)
		return false;
	memcpy(refusal, header->refusal, sizeof(refusal));
	for (size_t i = 0, i_len = SHMEM_DELTA_REFUSAL; i < i_len; i++)
	{
		if (refusal[i] == 0)
		{
			length = i;
			break;
		}
	}
	std_range_to_check(length, SHMEM_DELTA_REFUSAL, RUST_FILE, 551, 40);
	if (std_CString_new_bytes(refusal, length, result, &nul))
		return true;
	std_NulError_drop(&nul);
	return false;
}

/* shmem_delta_address returns the address of a body in this process. */
static uint8 *
shmem_delta_address(uint64 pointer)
{
	size_t		tmp_1 = STD_SUB(usize, (size_t) pointer, 1, 557, 48);

	return STD_PTR_AT(pg_calls_delta_base(), tmp_1, 1);
}

/*
 * shmem_delta_write copies bytes into a body at an offset. Its caller checked
 * that they lie within the body, above the filled mark of a page whose append
 * it alone may make.
 */
void
shmem_delta_write(uint64 pointer, uint64 at, const uint8 *bytes, size_t bytes_len)
{
	uint8	   *to = STD_PTR_AT(shmem_delta_address(pointer), (size_t) at, 1);

	std_ptr_copy_nonoverlapping_Checked(bytes, to, bytes_len, 1, 1);
}

/*
 * shmem_delta_bytes returns a range of a body's bytes in place. Its caller
 * checked that the range lies below the page's filled mark, and lends it no
 * longer than the hold that opened the page, which the page's free waits for.
 */
const uint8 *
shmem_delta_bytes(uint64 pointer, uint64 at, uint64 len, size_t *result_len)
{
	const uint8 *from = STD_PTR_AT(shmem_delta_address(pointer), (size_t) at, 1);

	std_slice_from_raw_parts_check(from, (size_t) len, 1, 1, RUST_FILE, 575, 14);
	*result_len = (size_t) len;
	return from;
}

/* shmem_delta_word_at returns the address of one of a body's words, which start the body. */
static pg_atomic_uint64 *
shmem_delta_word_at(uint64 pointer, uint64 index)
{
	return (pg_atomic_uint64 *) STD_PTR_AT(shmem_delta_address(pointer), (size_t) index, sizeof(pg_atomic_uint64));
}

/*
 * shmem_delta_body_word operates on one of a body's words, returning its
 * value before. Its caller checked the index against the page's count of
 * words.
 */
uint64
shmem_delta_body_word(uint64 pointer, uint64 index, ShmemRecordsOp op)
{
	pg_atomic_uint64 *word = shmem_delta_word_at(pointer, index);

	switch (op.tag)
	{
		case SHMEM_RECORDS_OP_LOAD:
			return pg_calls_atomic_read_u64(word);
		case SHMEM_RECORDS_OP_ADD:
			return pg_calls_atomic_add_u64(word, (int64) op.u.add);
		case SHMEM_RECORDS_OP_EXCHANGE:
			{
				uint64		seen = op.u.exchange.f0;

				pg_calls_atomic_exchange_u64(word, &seen, op.u.exchange.f1);
				return seen;
			}
	}
	pg_unreachable();
}

/* shmem_delta_body_store stores one of a body's words. Its caller checked the index against the page's count of words. */
void
shmem_delta_body_store(uint64 pointer, uint64 index, uint64 value)
{
	pg_calls_atomic_write_u64(shmem_delta_word_at(pointer, index), value);
}

#ifdef USE_INJECTION_POINTS
/* shmem_delta_stop holds the process at a named point of a sequence of the delta's pages, when a test attached to the point. */
void
shmem_delta_stop(const char *point)
{
	pg_calls_stop_point(point);
}
#else
/* shmem_delta_stop is nothing in a server built without injection points. */
void
shmem_delta_stop(const char *point)
{
	(void) point;
}
#endif

#ifdef DOLTGRES_TEST_SHMEM

/* test_shmem_delta_tests_the_directory_is_a_header_and_slots_of_a_cache_line: the header is on the directory's first cache line and the slots follow it, a cache line each. */
void
test_shmem_delta_tests_the_directory_is_a_header_and_slots_of_a_cache_line(void)
{
	STD_ASSERT(624, 9, sizeof(ShmemDeltaHeader) <= SHMEM_DELTA_SLOTS, "size_of::<Header>() <= SLOTS");
	{
		size_t		left_0 = sizeof(ShmemDeltaSlot);
		size_t		left_1 = SHMEM_DELTA_SLOTS % 64;
		size_t		right_0 = 64;
		size_t		right_1 = 0;
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&left_0, std_usize_Debug_fmt), STD_ITEM(&left_1, std_usize_Debug_fmt));
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&right_0, std_usize_Debug_fmt), STD_ITEM(&right_1, std_usize_Debug_fmt));

		STD_ASSERT_EQ(625, 9, left_0 == right_0 && left_1 == right_1, &left, std_tuple_Debug_fmt, &right,
					  std_tuple_Debug_fmt);
	}
	{
		size_t		left = offsetof(ShmemDeltaSlot, word);
		size_t		right = 0;

		STD_ASSERT_EQ(626, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
	}
	{
		size_t		left = offsetof(ShmemDeltaSlot, fields);
		size_t		right = 8;

		STD_ASSERT_EQ(627, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
	}
}

#endif							/* DOLTGRES_TEST_SHMEM */

#endif							/* FRONTEND */
