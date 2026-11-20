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
 * The collector's collection of a repository's packs: its roots captured, a
 * marking through every copy they reach, the live chunks of mostly dead
 * packs copied and forwarded, and the packs with nothing live retired.
 */
#ifndef DOLTGRES_STORE_COLLECT_H
#define DOLTGRES_STORE_COLLECT_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/time.h"
#include "doltgres/std/vec.h"

/*
 * StoreCollectCollected is Collected: what a collection did, the packs it
 * retired, those whose live chunks it copied out, the bytes of the live
 * chunks its marking found and of those it copied, and the old packs the
 * next young collection takes as leaves.
 */
typedef struct StoreCollectCollected
{
	size_t		retired;
	size_t		copied;
	uint64		live;
	uint64		moved;
	/* old is a BTreeSet<u64>, a Vec<u64> in order with each number once. */
	StdVec		old;
} StoreCollectCollected;

/*
 * StoreCollectSettings is Settings: the collector's settings, which the
 * worker reads at each check; min_free_disk is in bytes, or none for the
 * smaller of 10 GB and a tenth of the volume.
 */
typedef struct StoreCollectSettings
{
	uint64		target_percent;
	size_t		first_packs;
	StdDuration full_interval;
	uint64		io_limit;
	bool		has_min_free_disk;
	uint64		min_free_disk;
	uint64		young_size;
	size_t		young_packs;
} StoreCollectSettings;

/*
 * StoreCollectDatabase is Database: a row of pg_database as the collector
 * worker reads it, its id, its tablespace, and whether its repository may be
 * collected.
 */
typedef struct StoreCollectDatabase
{
	uint32		id;
	uint32		tablespace;
	bool		collectable;
} StoreCollectDatabase;

/*
 * StoreCollectHeld is Held: what the collector worker keeps of a repository
 * between its checks, the live bytes its last full collection found, when
 * that ran, those its young ones found since, the bytes collections copied
 * since, and the old packs a young one takes as leaves.
 */
typedef struct StoreCollectHeld
{
	bool		has_live;
	uint64		live;
	bool		has_last;
	StdInstant	last;
	uint64		young_live;
	uint64		moved;
	/* old is a BTreeSet<u64>, a Vec<u64> in order with each number once. */
	StdVec		old;
} StoreCollectHeld;

/*
 * StoreCollectSchedule is Schedule: what the collector worker keeps between
 * its checks, what it keeps of each repository, and the directories it
 * reported that no database names.
 */
typedef struct StoreCollectSchedule
{
	/*
	 * held is a BTreeMap<u32, Held>, a Vec of store_collect_held_t in id
	 * order.
	 */
	StdVec		held;
	/* reported is a BTreeSet<u32>, a Vec<u32> in order with each id once. */
	StdVec		reported;
} StoreCollectSchedule;

/* store_collect_held_t is a repository's entry in the schedule: its id and what the worker keeps of it. */
typedef struct store_collect_held_t
{
	uint32		id;
	StoreCollectHeld held;
} store_collect_held_t;

extern bool store_collect_collected(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
									uint32 database, uint64 limit, uint64 percent, const StdVec *old,
									bool *has_result, StoreCollectCollected *result, HostError *error);
extern void store_collect_Collected_drop(StoreCollectCollected *self);
extern void store_collect_Schedule_Default_default(StoreCollectSchedule *result);
extern void store_collect_Schedule_drop(StoreCollectSchedule *self);
extern bool store_collect_collect_due(HostIoIo *io, StoreCollectSchedule *schedule,
									  const StoreCollectDatabase *databases, size_t databases_len,
									  const StoreCollectSettings *settings, HostError *error);
extern bool store_collect_collect_asked(HostIoIo *io, StoreCollectSchedule *schedule,
										const StoreCollectDatabase *databases, size_t databases_len, uint32 id,
										const StoreCollectSettings *settings, uint64 *result, HostError *error);

#ifdef DOLTGRES_TEST_STORE
extern void test_store_collect_tests_a_collection_retires_a_pack_nothing_names_and_keeps_a_named_one(void);
extern void test_store_collect_tests_the_verify_lists_again_a_retired_pack_a_missed_root_reaches(void);
extern void test_store_collect_tests_a_collection_runs_nothing_while_a_base_backup_is_counted_in(void);
extern void test_store_collect_tests_a_paced_collection_naps_for_the_bytes_it_reads(void);
extern void test_store_collect_tests_the_check_collects_on_a_low_disk_and_reports_a_directory_no_database_names(void);
extern void test_store_collect_tests_a_second_collection_with_no_pack_written_since_runs(void);
extern void test_store_collect_tests_a_young_collection_retires_a_new_dead_pack_and_leaves_the_old_ones(void);
extern void test_store_collect_tests_a_chunk_named_by_address_alone_marks_every_record_of_it(void);
#endif

#endif							/* DOLTGRES_STORE_COLLECT_H */
