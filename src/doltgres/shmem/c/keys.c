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
 * The key lock's stripes ((4b)'s key lock): a region of Postgres' main
 * shared memory of SHMEM_KEYS_STRIPES lightweight locks in a tranche of
 * their own, which a writer of a key takes exclusively from its read through
 * its record and its push, as heap holds a buffer's content lock, and never
 * across a wait.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/shmem/keys.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/vec.h"
#include "pg_calls.h"

#define RUST_FILE "shmem" STD_SEP "src" STD_SEP "keys.rs"

/* SHMEM_KEYS_TRANCHE is the name of the stripes' tranche, which wait events show. */
static const char SHMEM_KEYS_TRANCHE[] = "versioned key";

/*
 * ShmemKeysStripes is the region's layout: its locks; beside each its push
 * count, which every release of it after a push under it moves, so a resolve
 * that read a per-storage stripe's count before the stripe sees under it
 * whether another writer pushed under it since; its hold-back flag, the
 * process number plus one of the resolve that holds the stripe's other
 * writers back, zero for none, and the condition variable they sleep on while
 * it is set; and the stripes' tranche, which their sleep names.
 */
typedef struct ShmemKeysStripes
{
	LWLock		locks[SHMEM_KEYS_STRIPES];
	pg_atomic_uint64 pushes[SHMEM_KEYS_STRIPES];
	pg_atomic_uint32 holders[SHMEM_KEYS_STRIPES];
	ConditionVariable wakes[SHMEM_KEYS_STRIPES];
	uint32		tranche;
} ShmemKeysStripes;

/*
 * SHMEM_KEYS_PG_WAIT_LWLOCK is Postgres' wait event class of a lightweight
 * lock's tranche, which a sleep on a stripe's hold-back flag names with the
 * stripes' tranche, as a wait on the stripe's lock does.
 */
#define SHMEM_KEYS_PG_WAIT_LWLOCK ((uint32) 0x01000000)

static LWLock *shmem_keys_stripe(uint32 at);
static pg_atomic_uint64 *shmem_keys_pushes(uint32 at);
static ShmemKeysStripes *shmem_keys_region(void);
static uint32 shmem_keys_me(void);
static uint32 shmem_keys_holder(uint32 at);

/* shmem_keys_request asks Postgres for the stripes' region, in the postmaster's request callback. */
bool
shmem_keys_request(const PgMayRaise *raise, PgPgError *error)
{
	return pg_calls_request_keys(raise, sizeof(ShmemKeysStripes), error);
}

/* shmem_keys_initialize sets the stripes up in the postmaster's initialization callback, each lock in their tranche. */
bool
shmem_keys_initialize(const PgMayRaise *raise, PgPgError *error)
{
	int32		tranche;
	ShmemKeysStripes *stripes;

	if (!pg_calls_new_tranche(raise, SHMEM_KEYS_TRANCHE, &tranche, error))
		return false;
	stripes = (ShmemKeysStripes *) pg_calls_keys_base();
	STD_CHECK_ALIGNED(stripes, _Alignof(ShmemKeysStripes), 58, 14);
	STD_CHECK_NULL_DEREF(stripes, 58, 14);
	stripes->tranche = (uint32) tranche;
	for (size_t at = 0, at_end = SHMEM_KEYS_STRIPES; at < at_end; at = std_forward_unchecked_usize_Checked(at, 1))
	{
		std_index_check(at, SHMEM_KEYS_STRIPES, RUST_FILE, 63, 44);
		pg_calls_lwlock_initialize(&stripes->locks[at], tranche);
		std_index_check(at, SHMEM_KEYS_STRIPES, RUST_FILE, 64, 42);
		pg_calls_atomic_init_u64(&stripes->pushes[at], 0);
		std_index_check(at, SHMEM_KEYS_STRIPES, RUST_FILE, 65, 42);
		pg_calls_atomic_init_u32(&stripes->holders[at], 0);
		std_index_check(at, SHMEM_KEYS_STRIPES, RUST_FILE, 66, 34);
		pg_calls_cv_init(&stripes->wakes[at]);
	}
	return true;
}

/* shmem_keys_stripe returns the lock of a stripe. */
static LWLock *
shmem_keys_stripe(uint32 at)
{
	ShmemKeysStripes *stripes = (ShmemKeysStripes *) pg_calls_keys_base();

	std_index_check((size_t) at, SHMEM_KEYS_STRIPES, RUST_FILE, 77, 23);
	return &stripes->locks[(size_t) at];
}

/* shmem_keys_lock takes stripes exclusively, in the order given, releasing those it took when one can't be taken. */
bool
shmem_keys_lock(const PgMayRaise *raise, const uint32 *stripes, size_t stripes_len, PgPgError *error)
{
	for (size_t i = 0, i_len = stripes_len, count = 0; i < i_len; i++)
	{
		size_t		taken = count;
		uint32		at = stripes[i];

		count = std_Enumerate_count_step(count);

		if (!pg_calls_lwlock_acquire(raise, shmem_keys_stripe(at), LW_EXCLUSIVE, error))
		{
			std_range_to_check(taken, stripes_len, RUST_FILE, 86, 28);
			shmem_keys_unlock(stripes, taken, false);
			return false;
		}
	}
	return true;
}

/*
 * shmem_keys_unlock releases stripes the process holds, the last taken
 * first, moving each one's push count before its release when the process
 * pushed under them.
 */
void
shmem_keys_unlock(const uint32 *stripes, size_t stripes_len, bool pushed)
{
	for (size_t i = stripes_len; i > 0;)
	{
		uint32		at;

		i--;
		at = stripes[i];
		if (pushed)
			(void) pg_calls_atomic_add_u64(shmem_keys_pushes(at), 1);
		pg_calls_lwlock_release(shmem_keys_stripe(at));
	}
}

/* shmem_keys_pushes returns the push count of a stripe. */
static pg_atomic_uint64 *
shmem_keys_pushes(uint32 at)
{
	ShmemKeysStripes *stripes = (ShmemKeysStripes *) pg_calls_keys_base();

	std_index_check((size_t) at, SHMEM_KEYS_STRIPES, RUST_FILE, 115, 23);
	return &stripes->pushes[(size_t) at];
}

/* shmem_keys_region returns the stripes' region. */
static ShmemKeysStripes *
shmem_keys_region(void)
{
	return (ShmemKeysStripes *) pg_calls_keys_base();
}

/* shmem_keys_me returns the running process's mark in a hold-back flag, its process number plus one. */
static uint32
shmem_keys_me(void)
{
	return std_wrapping_add_u32((uint32) pg_calls_my_proc_number(), 1);
}

/* shmem_keys_holder reads a stripe's hold-back flag. */
static uint32
shmem_keys_holder(uint32 at)
{
	uint32		seen = 0;
	ShmemKeysStripes *region = shmem_keys_region();

	std_index_check((size_t) at, SHMEM_KEYS_STRIPES, RUST_FILE, 133, 47);
	pg_calls_atomic_exchange_u32(&region->holders[(size_t) at], &seen, 0);
	return seen;
}

/*
 * shmem_keys_hold sets a stripe's hold-back flag for the running process
 * unless another process's is set, and reports whether the running process's
 * is.
 */
bool
shmem_keys_hold(uint32 at)
{
	uint32		seen = 0;
	ShmemKeysStripes *region = shmem_keys_region();
	uint32		me;

	std_index_check((size_t) at, SHMEM_KEYS_STRIPES, RUST_FILE, 142, 47);
	me = shmem_keys_me();
	pg_calls_atomic_exchange_u32(&region->holders[(size_t) at], &seen, me);
	return seen == 0 || seen == shmem_keys_me();
}

/* shmem_keys_release clears a stripe's hold-back flag if the running process set it, and wakes the writers asleep on it. */
void
shmem_keys_release(uint32 at)
{
	uint32		seen = shmem_keys_me();
	ShmemKeysStripes *region = shmem_keys_region();

	std_index_check((size_t) at, SHMEM_KEYS_STRIPES, RUST_FILE, 152, 42);
	pg_calls_atomic_exchange_u32(&region->holders[(size_t) at], &seen, 0);
	if (seen == shmem_keys_me())
	{
		region = shmem_keys_region();
		std_index_check((size_t) at, SHMEM_KEYS_STRIPES, RUST_FILE, 154, 39);
		pg_calls_cv_broadcast(&region->wakes[(size_t) at]);
	}
}

/*
 * shmem_keys_held_wait sleeps on a stripe's condition variable while another
 * process's hold-back flag is set, answering cancel and statement_timeout as
 * ConditionVariableSleep does.
 */
bool
shmem_keys_held_wait(const PgMayRaise *raise, uint32 at, PgPgError *error)
{
	ShmemKeysStripes *region = shmem_keys_region();
	uint32		wait;

	STD_CHECK_ALIGNED(region, _Alignof(ShmemKeysStripes), 163, 42);
	STD_CHECK_NULL_DEREF(region, 163, 42);
	wait = SHMEM_KEYS_PG_WAIT_LWLOCK | region->tranche;
	while (!(shmem_keys_holder(at) == 0) && shmem_keys_holder(at) != shmem_keys_me())
	{
		region = shmem_keys_region();
		std_index_check((size_t) at, SHMEM_KEYS_STRIPES, RUST_FILE, 166, 67);
		if (!pg_calls_cv_sleep(raise, &region->wakes[(size_t) at], wait, error))
		{
			pg_calls_cv_cancel();
			return false;
		}
	}
	pg_calls_cv_cancel();
	return true;
}

/*
 * shmem_keys_exit clears every hold-back flag the exiting process set, waking
 * their sleepers, as its last act on the stripes; a process that never mapped
 * the stripes' region, as bootstrap's, set none.
 */
void
shmem_keys_exit(void)
{
	if (pg_calls_keys_base() == NULL)
		return;
	for (uint32 at = 0, at_end = (uint32) SHMEM_KEYS_STRIPES; at < at_end; at = std_forward_unchecked_u32_Checked(at, 1))
		shmem_keys_release(at);
}

/* shmem_keys_count reads a stripe's push count. */
uint64
shmem_keys_count(uint32 at)
{
	return pg_calls_atomic_read_u64(shmem_keys_pushes(at));
}

#endif							/* FRONTEND */
