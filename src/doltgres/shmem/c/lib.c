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
 * The fork's shared memory (design item 6): one region in Postgres' main
 * shared memory, fixed in size from the server's start, holding the control
 * record with the fork's LWLock and wait events, the processes' epochs, the
 * directory of the pooled records, and every kind's records. Postgres keeps
 * the region's address in pg's C globals, so this crate keeps only offsets
 * and translates them inside a call, and holds the lock only through a
 * guard, which releases it when it drops.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/shmem/lib.h"
#include "doltgres/shmem/records.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"
#include "pg_calls.h"

#define RUST_FILE "shmem" STD_SEP "src" STD_SEP "lib.rs"

/* SHMEM_TRANCHE is the name of the lock's tranche, which wait events show. */
static const char SHMEM_TRANCHE[] = "versioned shared memory";

/* SHMEM_FILE_WAITS are the names of the wait events of the file door's reads, writes, and syncs, in that order. */
static const char *const SHMEM_FILE_WAITS[3] = {"versioned file read", "versioned file write", "versioned file sync"};

/*
 * shmem_request asks Postgres for the region, in the postmaster's request
 * callback: the control record and what the kinds of records take for the
 * server's processes.
 */
bool
shmem_request(const PgMayRaise *raise, const ShmemRecordsKind *kinds, size_t kinds_len, PgPgError *error)
{
	size_t		procs = (size_t) pg_calls_max_procs();
	size_t		size = shmem_records_size(kinds, kinds_len, procs);

	return pg_calls_request_shmem(raise, size, error);
}

/*
 * shmem_initialize sets the region up in the postmaster's initialization
 * callback: the lock, the file door's wait events, the seed of the delta's
 * shapes, and the kinds' records.
 */
bool
shmem_initialize(const PgMayRaise *raise, const ShmemRecordsKind *kinds, size_t kinds_len, PgPgError *error)
{
	ShmemControl *control = (ShmemControl *) pg_calls_shmem_base();
	int32		tranche;
	uint32		file_waits[3] = {0};
	uint8		seed[32] = {0};

	if (!pg_calls_new_tranche(raise, SHMEM_TRANCHE, &tranche, error))
		return false;
	for (size_t i = 0; i < 3; i++)
	{
		if (!pg_calls_wait_event_new(raise, SHMEM_FILE_WAITS[i], &file_waits[i], error))
			return false;
	}
	if (!pg_calls_strong_random(raise, seed, sizeof(seed), error))
		return false;
	pg_calls_lwlock_initialize(&control->lock, tranche);
	memcpy(control->file_waits, file_waits, sizeof(file_waits));
	memcpy(control->seed, seed, sizeof(seed));
#ifdef USE_INJECTION_POINTS
	control->test = 0;
#endif
	return shmem_records_initialize(raise, control, tranche, kinds, kinds_len, error);
}

/* shmem_file_waits returns the wait events of the file door's reads, writes, and syncs, in that order. */
void
shmem_file_waits(uint32 result[3])
{
	ShmemControl *control = (ShmemControl *) pg_calls_shmem_base();

	if (control == NULL)
		STD_OPTION_EXPECT_FAILED(104, 58, "Postgres maps the region before any entry runs");
	memcpy(result, control->file_waits, sizeof(control->file_waits));
}

/*
 * shmem_seed returns the random seed of the delta's shapes in memory, which
 * the postmaster drew at the region's last initialization.
 */
void
shmem_seed(uint8 result[32])
{
	ShmemControl *control = (ShmemControl *) pg_calls_shmem_base();

	if (control == NULL)
		STD_OPTION_EXPECT_FAILED(114, 58, "Postgres maps the region before any entry runs");
	memcpy(result, control->seed, sizeof(control->seed));
}

/* shmem_Region_mapped returns the region as this process maps it. */
ShmemRegion
shmem_Region_mapped(void)
{
	ShmemControl *control = (ShmemControl *) pg_calls_shmem_base();

	if (control == NULL)
		STD_OPTION_EXPECT_FAILED(130, 61, "Postgres maps the region before any entry runs");
	return (ShmemRegion)
	{
		control
	};
}

/* shmem_Region_lock returns the address of the region's lock. */
static LWLock *
shmem_Region_lock(const ShmemRegion *self)
{
	return &self->control->lock;
}

/* shmem_Region_acquire takes the region's lock in a mode. */
static bool
shmem_Region_acquire(const ShmemRegion *self, const PgMayRaise *raise, LWLockMode mode, PgPgError *error)
{
	return pg_calls_lwlock_acquire(raise, shmem_Region_lock(self), mode, error);
}

/* shmem_Region_exclusive takes the region's lock exclusively. */
bool
shmem_Region_exclusive(const ShmemRegion *self, const PgMayRaise *raise, ShmemExclusive *result, PgPgError *error)
{
	if (!shmem_Region_acquire(self, raise, LW_EXCLUSIVE, error))
		return false;
	*result = (ShmemExclusive)
	{
		self
	};
	return true;
}

/* shmem_Region_shared takes the region's lock shared. */
bool
shmem_Region_shared(const ShmemRegion *self, const PgMayRaise *raise, ShmemShared *result, PgPgError *error)
{
	if (!shmem_Region_acquire(self, raise, LW_SHARED, error))
		return false;
	*result = (ShmemShared)
	{
		self
	};
	return true;
}

#ifdef USE_INJECTION_POINTS
/* shmem_Region_test returns the address of the value the fork's tests store. */
static uint64 *
shmem_Region_test(const ShmemRegion *self)
{
	return &self->control->test;
}
#endif

/* shmem_Region_release releases the region's lock, which a guard holds. */
static void
shmem_Region_release(const ShmemRegion *self)
{
	pg_calls_lwlock_release(shmem_Region_lock(self));
}

#ifdef USE_INJECTION_POINTS
/* shmem_Exclusive_store_test writes the value the fork's tests store, in a critical section. */
void
shmem_Exclusive_store_test(ShmemExclusive *self, uint64 value)
{
	pg_calls_start_crit_section();
	*shmem_Region_test(self->region) = value;
	pg_calls_end_crit_section();
}
#endif

/* shmem_Exclusive_drop is Exclusive's drop: the region's lock released. */
void
shmem_Exclusive_drop(ShmemExclusive *self)
{
	shmem_Region_release(self->region);
}

#ifdef USE_INJECTION_POINTS
/* shmem_Shared_load_test reads the value the fork's tests store, which is zero before their first store. */
uint64
shmem_Shared_load_test(const ShmemShared *self)
{
	return *shmem_Region_test(self->region);
}
#endif

/* shmem_Shared_drop is Shared's drop: the region's lock released. */
void
shmem_Shared_drop(ShmemShared *self)
{
	shmem_Region_release(self->region);
}

#ifdef DOLTGRES_TEST_SHMEM

/* test_shmem_tests_the_lock_is_at_the_regions_address: the lock is the first thing in the region. */
void
test_shmem_tests_the_lock_is_at_the_regions_address(void)
{
	size_t		left = offsetof(ShmemControl, lock);
	size_t		right = 0;

	STD_ASSERT_EQ(229, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
}

#endif							/* DOLTGRES_TEST_SHMEM */

#endif							/* FRONTEND */
