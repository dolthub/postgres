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
 * The fork's shared memory (shmem's lib.rs, design item 6): one region in
 * Postgres' main shared memory, fixed in size from the server's start,
 * holding the control record with the fork's LWLock and wait events, the
 * processes' epochs, the directory of the pooled records, and every kind's
 * records.
 */
#ifndef DOLTGRES_SHMEM_LIB_H
#define DOLTGRES_SHMEM_LIB_H

#ifndef FRONTEND

#include "doltgres/hooks.h"
#include "doltgres/pg/lib.h"

/* DoltgresTranche numbers the fork's LWLock tranches, as doltgres/tranchelist.h lists them. */
typedef enum DoltgresTranche
{
#define DOLTGRES_TRANCHE(id, name) DOLTGRES_TRANCHE_##id,
#include "doltgres/tranchelist.h"
#undef DOLTGRES_TRANCHE
	DOLTGRES_TRANCHE_COUNT
} DoltgresTranche;

StaticAssertDecl(DOLTGRES_TRANCHE_COUNT == DOLTGRES_LWLOCK_TRANCHES,
				 "hooks.h's DOLTGRES_LWLOCK_TRANCHES must count every tranche in doltgres/tranchelist.h");

struct ShmemRecordsKind;

/*
 * ShmemControl is the region's fixed part. Its lock comes first, at the
 * region's address, where the test module's measurement takes it.
 */
typedef struct ShmemControl
{
	/* lock guards the value the fork's tests store. */
	LWLock		lock;

	/*
	 * file_waits are the wait events of SHMEM_FILE_WAITS, which the
	 * postmaster registers.
	 */
	uint32		file_waits[3];
	/* epoch_lock guards the epochs (host's shared seams). */
	LWLock		epoch_lock;
	/* epoch is the current epoch, under the epoch lock. */
	uint64		epoch;

	/*
	 * procs is the number of processes, each with a slot for its oldest live
	 * reader's epoch.
	 */
	uint32		procs;
	/* tranches are the fork's LWLock tranches' ids, by DoltgresTranche. */
	int32		tranches[DOLTGRES_TRANCHE_COUNT];
	/* flag_wait is the wait event of a wait for another process's flag. */
	uint32		flag_wait;
	/* kinds is the number of kinds of records. */
	uint32		kinds;

	/*
	 * slots is the number of the directory's slots, one for each pooled
	 * record.
	 */
	uint32		slots;
	/* epochs is the offset in the region of the processes' epoch slots. */
	uint64		epochs;
	/* directory is the offset in the region of the directory's words. */
	uint64		directory;

	/*
	 * seed is the random seed of the delta's shapes in memory, drawn at each
	 * initialization of the region.
	 */
	uint8		seed[32];
#ifdef USE_INJECTION_POINTS
	/* test is the value the fork's tests store under the lock. */
	uint64		test;
#endif
} ShmemControl;

/*
 * ShmemRegion is this process's view of the region. It holds a pointer, so
 * it lives only within a call.
 */
typedef struct ShmemRegion
{
	/* control is the region's control record in this process. */
	ShmemControl *control;
} ShmemRegion;

/* ShmemExclusive is the region's lock held exclusively, released when it drops. */
typedef struct ShmemExclusive
{
	/* region is the region whose lock it holds. */
	const ShmemRegion *region;
} ShmemExclusive;

/* ShmemShared is the region's lock held shared, released when it drops. */
typedef struct ShmemShared
{
	/* region is the region whose lock it holds. */
	const ShmemRegion *region;
} ShmemShared;

extern bool shmem_request(const PgMayRaise *raise, const struct ShmemRecordsKind *kinds, size_t kinds_len,
						  PgPgError *error);
extern bool shmem_initialize(const PgMayRaise *raise, const struct ShmemRecordsKind *kinds, size_t kinds_len,
							 PgPgError *error);
extern int32 shmem_tranche(DoltgresTranche which);
extern void shmem_file_waits(uint32 result[3]);
extern void shmem_seed(uint8 result[32]);
extern ShmemRegion shmem_Region_mapped(void);
extern bool shmem_Region_exclusive(const ShmemRegion *self, const PgMayRaise *raise, ShmemExclusive *result,
								   PgPgError *error);
extern bool shmem_Region_shared(const ShmemRegion *self, const PgMayRaise *raise, ShmemShared *result,
								PgPgError *error);
#ifdef USE_INJECTION_POINTS
extern void shmem_Exclusive_store_test(ShmemExclusive *self, uint64 value);
extern uint64 shmem_Shared_load_test(const ShmemShared *self);
#endif
extern void shmem_Exclusive_drop(ShmemExclusive *self);
extern void shmem_Shared_drop(ShmemShared *self);

#ifdef DOLTGRES_TEST_SHMEM
extern void test_shmem_tests_the_lock_is_at_the_regions_address(void);
#endif

#endif							/* FRONTEND */

#endif							/* DOLTGRES_SHMEM_LIB_H */
