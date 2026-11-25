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
 * The collection's and the base backups' meeting point (the collector text's
 * section 3.3): a region of Postgres' main shared memory holding the count of
 * running base backups, the collection's word, and the condition variable a
 * backup's start sleeps on while a collection runs. Each side moves its own
 * word with pg_atomic_fetch_add_u64, a full barrier, and then reads the
 * other's, so one of them always sees the other. It also holds a backend's
 * ask of the collector worker (section 8.2): the ask's word, the worker's
 * process number, the answer's counts, and the condition variable the askers
 * sleep on.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/shmem/collection.h"
#include "doltgres/shmem/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "pg_calls.h"

#define RUST_FILE "shmem" STD_SEP "src" STD_SEP "collection.rs"

/* SHMEM_COLLECTION_DONE is the bit of the ask's word the worker sets once it has written its answer. */
#define SHMEM_COLLECTION_DONE ((uint64) 1 << 32)

/* SHMEM_COLLECTION_HELD is the bit of the ask's word its asker holds while it writes the tablespace the ask empties. */
#define SHMEM_COLLECTION_HELD ((uint64) 1 << 63)

/*
 * SHMEM_COLLECTION_PG_WAIT_LWLOCK is Postgres' wait event class of a
 * lightweight lock's tranche, which the backup's sleep names with the
 * region's tranche, as the key lock's stripes' sleep does.
 */
#define SHMEM_COLLECTION_PG_WAIT_LWLOCK ((uint32) 0x01000000)

/*
 * ShmemCollectionMeeting is the region's layout: the count of running base
 * backups, the collection's word (one while a collection runs), the condition
 * variable a backup's start sleeps on, and the tranche its sleep names; then
 * the ask's word (0, or the asker's process number plus one above DONE, DONE
 * once answered, and the database asked for below it), the tablespace the
 * ask empties, the collector worker's process number plus one (0 before it
 * starts), the answer's
 * counts, the condition variable askers sleep on, and the tranche their
 * sleep names.
 */
typedef struct ShmemCollectionMeeting
{
	pg_atomic_uint64 backups;
	pg_atomic_uint64 collecting;
	ConditionVariable wake;
	uint32		tranche;
	pg_atomic_uint64 asked;
	pg_atomic_uint64 emptied;
	pg_atomic_uint64 worker;
	pg_atomic_uint64 answer[3];
	ConditionVariable answered;
	uint32		asking;
} ShmemCollectionMeeting;

static ShmemCollectionMeeting *shmem_collection_region(void);
static pg_atomic_uint64 *shmem_collection_backups(void);
static pg_atomic_uint64 *shmem_collection_collecting(void);
static ConditionVariable *shmem_collection_wake(void);
static pg_atomic_uint64 *shmem_collection_asked(void);
static ConditionVariable *shmem_collection_answered(void);
static bool shmem_collection_slept(const PgMayRaise *raise, uint32 wait, bool has_mine, uint64 mine,
								   PgPgError *error);

/* shmem_collection_request asks Postgres for the region, in the postmaster's request callback. */
bool
shmem_collection_request(const PgMayRaise *raise, PgPgError *error)
{
	return pg_calls_request_collection(raise, sizeof(ShmemCollectionMeeting), error);
}

/* shmem_collection_initialize sets the region up in the postmaster's initialization callback. */
bool
shmem_collection_initialize(const PgMayRaise *raise, PgPgError *error)
{
	int32		tranche;
	int32		asking;
	ShmemCollectionMeeting *region;

	tranche = shmem_tranche(DOLTGRES_TRANCHE_COLLECTION);
	asking = shmem_tranche(DOLTGRES_TRANCHE_COLLECTION_ASKING);
	pg_calls_atomic_init_u64(shmem_collection_backups(), 0);
	pg_calls_atomic_init_u64(shmem_collection_collecting(), 0);
	pg_calls_cv_init(shmem_collection_wake());
	region = shmem_collection_region();
	STD_CHECK_ALIGNED(region, _Alignof(ShmemCollectionMeeting), 76, 9);
	STD_CHECK_NULL_DEREF(region, 76, 9);
	region->tranche = (uint32) tranche;
	region = shmem_collection_region();
	pg_calls_atomic_init_u64(&region->asked, 0);
	region = shmem_collection_region();
	pg_calls_atomic_init_u64(&region->emptied, 0);
	region = shmem_collection_region();
	pg_calls_atomic_init_u64(&region->worker, 0);
	for (size_t at = 0, at_end = 3; at < at_end; at = std_forward_unchecked_usize_Checked(at, 1))
	{
		region = shmem_collection_region();
		std_index_check(at, 3, RUST_FILE, 80, 42);
		pg_calls_atomic_init_u64(&region->answer[at], 0);
	}
	region = shmem_collection_region();
	pg_calls_cv_init(&region->answered);
	region = shmem_collection_region();
	STD_CHECK_ALIGNED(region, _Alignof(ShmemCollectionMeeting), 83, 9);
	STD_CHECK_NULL_DEREF(region, 83, 9);
	region->asking = (uint32) asking;
	return true;
}

/* shmem_collection_region returns the region. */
static ShmemCollectionMeeting *
shmem_collection_region(void)
{
	return (ShmemCollectionMeeting *) pg_calls_collection_base();
}

/* shmem_collection_backups returns the count of running base backups. */
static pg_atomic_uint64 *
shmem_collection_backups(void)
{
	return &shmem_collection_region()->backups;
}

/* shmem_collection_collecting returns the collection's word. */
static pg_atomic_uint64 *
shmem_collection_collecting(void)
{
	return &shmem_collection_region()->collecting;
}

/* shmem_collection_wake returns the condition variable a backup's start sleeps on. */
static ConditionVariable *
shmem_collection_wake(void)
{
	return &shmem_collection_region()->wake;
}

/* shmem_collection_added moves a word by an amount with a full barrier and returns its value after. */
static uint64
shmem_collection_added(pg_atomic_uint64 *word, int64 by)
{
	uint64		before = pg_calls_atomic_add_u64(word, by);

	return std_wrapping_add_signed_u64(before, by);
}

/* shmem_collection_read reads a word. */
static uint64
shmem_collection_read(pg_atomic_uint64 *word)
{
	return pg_calls_atomic_read_u64(word);
}

/*
 * shmem_collection_backup_begin counts a base backup in and then sleeps while
 * a collection runs, answering cancel as ConditionVariableSleep does; a
 * canceled wait leaves the backup counted in, which do_pg_abort_backup counts
 * out.
 */
bool
shmem_collection_backup_begin(const PgMayRaise *raise, PgPgError *error)
{
	ShmemCollectionMeeting *region;
	uint32		wait;

	(void) shmem_collection_added(shmem_collection_backups(), 1);
	region = shmem_collection_region();
	STD_CHECK_ALIGNED(region, _Alignof(ShmemCollectionMeeting), 138, 42);
	STD_CHECK_NULL_DEREF(region, 138, 42);
	wait = SHMEM_COLLECTION_PG_WAIT_LWLOCK | region->tranche;
	while (shmem_collection_read(shmem_collection_collecting()) != 0)
	{
		if (!pg_calls_cv_sleep(raise, shmem_collection_wake(), wait, error))
		{
			pg_calls_cv_cancel();
			return false;
		}
	}
	pg_calls_cv_cancel();
	return true;
}

/* shmem_collection_backup_end counts a base backup out. */
void
shmem_collection_backup_end(void)
{
	(void) shmem_collection_added(shmem_collection_backups(), -1);
}

/* shmem_collection_backups_running reports whether a base backup is counted in. */
bool
shmem_collection_backups_running(void)
{
	return shmem_collection_read(shmem_collection_backups()) != 0;
}

/*
 * shmem_collection_collection_begin sets the collection's word and then
 * reads the count of backups, and with a backup counted in clears the word
 * again and wakes the backups asleep on it, answering whether the collection
 * may run.
 */
bool
shmem_collection_collection_begin(void)
{
	(void) shmem_collection_added(shmem_collection_collecting(), 1);
	if (shmem_collection_read(shmem_collection_backups()) == 0)
		return true;
	shmem_collection_collection_end();
	return false;
}

/* shmem_collection_collection_end clears the collection's word and wakes the backups asleep on it. */
void
shmem_collection_collection_end(void)
{
	(void) shmem_collection_added(shmem_collection_collecting(), -1);
	pg_calls_cv_broadcast(shmem_collection_wake());
}

/* shmem_collection_asked returns the ask's word. */
static pg_atomic_uint64 *
shmem_collection_asked(void)
{
	return &shmem_collection_region()->asked;
}

/* shmem_collection_answered returns the condition variable askers sleep on. */
static ConditionVariable *
shmem_collection_answered(void)
{
	return &shmem_collection_region()->answered;
}

/* shmem_collection_swapped writes a word when it holds an expected value, answering whether it did. */
static bool
shmem_collection_swapped(pg_atomic_uint64 *word, uint64 expected, uint64 value)
{
	uint64		seen = expected;

	pg_calls_atomic_exchange_u64(word, &seen, value);
	return seen == expected;
}

/* shmem_collection_worker_started records the running process as the collector worker, whose latch an ask sets. */
void
shmem_collection_worker_started(void)
{
	uint64		tmp_1 = (uint64) (uint32) pg_calls_my_proc_number();
	uint64		number = STD_ADD(u64, tmp_1, 1, 207, 18);

	pg_calls_atomic_write_u64(&shmem_collection_region()->worker, number);
}

/* shmem_collection_worker_running reports whether a collector worker has started, which an ask needs to be answered. */
bool
shmem_collection_worker_running(void)
{
	return shmem_collection_read(&shmem_collection_region()->worker) != 0;
}

/* shmem_collection_ask_closure_0 is map's closure: an answer's count at a place. */
static uint64
shmem_collection_ask_closure_0(void *captures, size_t at)
{
	ShmemCollectionMeeting *region = shmem_collection_region();

	(void) captures;
	std_index_check(at, 3, RUST_FILE, 241, 75);
	return pg_calls_atomic_read_u64(&region->answer[at]);
}

/* SHMEM_COLLECTION_ASK_PLACES is the array ask's map maps: the answer's places. */
static const size_t SHMEM_COLLECTION_ASK_PLACES[3] = {0, 1, 2};

/*
 * shmem_collection_ask_frame_0_t is ask's map's state across its pad, as
 * core's [T; N]::map runs it: try_map's Drain over the places and
 * try_from_fn_erased's Guard over the counts it wrote.
 */
typedef struct shmem_collection_ask_frame_0_t
{
	uint64	   *result;
	size_t		drained;
	size_t		initialized;
} shmem_collection_ask_frame_0_t;

/* shmem_collection_ask_body_0 is ask's map: each place's count, with get_unchecked_mut's and unchecked_add's preconditions. */
static void
shmem_collection_ask_body_0(void *frame_)
{
	shmem_collection_ask_frame_0_t *frame = frame_;

	while (frame->initialized < 3)
	{
		size_t		at = SHMEM_COLLECTION_ASK_PLACES[frame->drained];
		uint64		item;

		frame->drained = std_wrapping_add_usize(frame->drained, 1);
		item = shmem_collection_ask_closure_0(NULL, at);
		std_slice_get_unchecked_mut_check(frame->initialized, 3, STD_ARRAY_MOD_FILE, 954, 28);
		frame->result[frame->initialized] = item;
		std_usize_unchecked_add_check(frame->initialized, 1, STD_ARRAY_MOD_FILE, 955, 49);
		frame->initialized = std_wrapping_add_usize(frame->initialized, 1);
	}
	std_array_Drain_drop(&SHMEM_COLLECTION_ASK_PLACES[frame->drained], &SHMEM_COLLECTION_ASK_PLACES[3],
						 sizeof(SHMEM_COLLECTION_ASK_PLACES[0]));
}

/* shmem_collection_ask_pad_0 is ask's map's pad: the Guard's drop, then the Drain's. */
static void
shmem_collection_ask_pad_0(void *frame_)
{
	shmem_collection_ask_frame_0_t *frame = frame_;

	std_array_Guard_drop(frame->initialized, 3);
	std_array_Drain_drop(&SHMEM_COLLECTION_ASK_PLACES[frame->drained], &SHMEM_COLLECTION_ASK_PLACES[3],
						 sizeof(SHMEM_COLLECTION_ASK_PLACES[0]));
}

/*
 * shmem_collection_ask asks the collector worker for a full collection of a
 * database's repository, emptying a tablespace unless that is
 * CHUNKS_PACKS_DEFAULT_SPACE, and sleeps until it answers with the packs it
 * retired, those it copied out, and its live bytes, each
 * SHMEM_COLLECTION_FAILED when it failed; one ask is out at a time, and a
 * later asker sleeps until it may write its own. A cancel ends the sleep,
 * taking back an ask not yet answered, and the collection goes on.
 */
bool
shmem_collection_ask(const PgMayRaise *raise, uint32 database, uint32 emptied, uint64 result[3], PgPgError *error)
{
	uint64		tmp_1 = (uint64) (uint32) pg_calls_my_proc_number();
	uint64		tmp_2 = STD_ADD(u64, tmp_1, 1, 218, 18);
	uint64		mine = STD_SHL_FOLDED(u64, tmp_2, 33) | (uint64) database;
	ShmemCollectionMeeting *region = shmem_collection_region();
	uint32		wait;
	uint64		worker;

	STD_CHECK_ALIGNED(region, _Alignof(ShmemCollectionMeeting), 222, 42);
	STD_CHECK_NULL_DEREF(region, 222, 42);
	wait = SHMEM_COLLECTION_PG_WAIT_LWLOCK | region->asking;
	while (!shmem_collection_swapped(shmem_collection_asked(), 0, mine | SHMEM_COLLECTION_HELD))
	{
		if (!shmem_collection_slept(raise, wait, false, 0, error))
			return false;
	}
	pg_calls_atomic_write_u64(&shmem_collection_region()->emptied, emptied);
	(void) shmem_collection_swapped(shmem_collection_asked(), mine | SHMEM_COLLECTION_HELD, mine);
	worker = shmem_collection_read(&shmem_collection_region()->worker);
	if (worker != 0)
	{
		uint64		tmp_3 = STD_SUB(u64, worker, 1, 233, 42);
		int32		number;
		StdTryFromIntError tried;

		if (!std_i32_try_from_u64(tmp_3, &number, &tried))
			std_result_unwrap_failed("a process number fits an int", sizeof("a process number fits an int") - 1,
									 &tried, std_TryFromIntError_Debug_fmt, RUST_FILE, 233, 54);
		pg_calls_set_proc_latch(number);
	}
	while (shmem_collection_read(shmem_collection_asked()) != (mine | SHMEM_COLLECTION_DONE))
	{
		if (!shmem_collection_slept(raise, wait, true, mine, error))
			return false;
	}
	{
		shmem_collection_ask_frame_0_t frame = {.result = result};

		std_landing(&frame, shmem_collection_ask_body_0, shmem_collection_ask_pad_0);
	}
	pg_calls_atomic_write_u64(shmem_collection_asked(), 0);
	pg_calls_cv_cancel();
	pg_calls_cv_broadcast(shmem_collection_answered());
	return true;
}

/*
 * shmem_collection_slept sleeps once on the askers' condition variable; a
 * cancel takes back the asker's own ask, when it has one, unanswered or
 * answered, and wakes the other askers.
 */
static bool
shmem_collection_slept(const PgMayRaise *raise, uint32 wait, bool has_mine, uint64 mine, PgPgError *error)
{
	if (pg_calls_cv_sleep(raise, shmem_collection_answered(), wait, error))
		return true;
	pg_calls_cv_cancel();
	if (has_mine
		&& (shmem_collection_swapped(shmem_collection_asked(), mine, 0)
			|| shmem_collection_swapped(shmem_collection_asked(), mine | SHMEM_COLLECTION_DONE, 0)))
		pg_calls_cv_broadcast(shmem_collection_answered());
	return false;
}

/* shmem_collection_asked_for returns the unanswered ask's word, the database it asks for, and the tablespace it empties, the collector worker's read. */
bool
shmem_collection_asked_for(std_TupleU64U32_t *result, uint32 *emptied)
{
	uint64		word = shmem_collection_read(shmem_collection_asked());
	uint32		database;
	StdTryFromIntError tried;

	if (!std_u32_try_from_u64(word & (uint64) PG_UINT32_MAX, &database, &tried))
		std_result_unwrap_failed("the word's low half fits a database id",
								 sizeof("the word's low half fits a database id") - 1, &tried,
								 std_TryFromIntError_Debug_fmt, RUST_FILE, 273, 62);
	if (word != 0 && (word & (SHMEM_COLLECTION_DONE | SHMEM_COLLECTION_HELD)) == 0)
	{
		pg_calls_read_barrier();
		result->f0 = word;
		result->f1 = database;
		*emptied = (uint32) shmem_collection_read(&shmem_collection_region()->emptied);
		return true;
	}
	return false;
}

/* shmem_collection_answer writes the worker's answer to an ask and wakes its asker, unless the asker took the ask back. */
void
shmem_collection_answer(uint64 word, const uint64 counts[3])
{
	for (size_t counts_at = 0, counts_index, count = 0; std_array_IntoIter_next(&counts_at, 3, 3, &counts_index);)
	{
		size_t		at = count;
		uint64		value = counts[counts_index];
		ShmemCollectionMeeting *region;

		count = std_Enumerate_count_step(count);
		region = shmem_collection_region();
		std_index_check(at, 3, RUST_FILE, 283, 43);
		pg_calls_atomic_write_u64(&region->answer[at], value);
	}
	if (shmem_collection_swapped(shmem_collection_asked(), word, word | SHMEM_COLLECTION_DONE))
		pg_calls_cv_broadcast(shmem_collection_answered());
}

#endif							/* FRONTEND */
