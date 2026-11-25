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
 * The shared records of a repository's packs (chunks' packs/shared.rs).
 */
#ifndef DOLTGRES_CHUNKS_PACKS_SHARED_H
#define DOLTGRES_CHUNKS_PACKS_SHARED_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/capability.h"

/*
 * CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS is PackState's Pool, its Shape
 * first: the shape of a repository's pack record.
 */
/* The pack record's flags: Rewriting, a whole write of a list file, and Merging, a merge of address-index runs. */
#define CHUNKS_PACKS_SHARED_REWRITING_INDEX 0
#define CHUNKS_PACKS_SHARED_MERGING_INDEX 1

/* The pack record's locks, in the order they are taken: Barrier, ListsLock, StateLock, and EntriesLock. */
#define CHUNKS_PACKS_SHARED_BARRIER_INDEX 0
#define CHUNKS_PACKS_SHARED_LISTS_LOCK_INDEX 1
#define CHUNKS_PACKS_SHARED_STATE_LOCK_INDEX 2
#define CHUNKS_PACKS_SHARED_ENTRIES_LOCK_INDEX 3

/*
 * The pack record's atomics: Generation, Reserved, Rotations, Repository,
 * Marked, Own, and Differs.
 */
#define CHUNKS_PACKS_SHARED_GENERATION_INDEX 0
#define CHUNKS_PACKS_SHARED_RESERVED_INDEX 1
#define CHUNKS_PACKS_SHARED_ROTATIONS_INDEX 2
#define CHUNKS_PACKS_SHARED_REPOSITORY_INDEX 3
#define CHUNKS_PACKS_SHARED_MARKED_INDEX 4
#define CHUNKS_PACKS_SHARED_OWN_INDEX 5
#define CHUNKS_PACKS_SHARED_DIFFERS_INDEX 6

/* CHUNKS_PACKS_SHARED_SNAPSHOT_BLOB is the place among the record's blobs of the snapshot's. */
#define CHUNKS_PACKS_SHARED_SNAPSHOT_BLOB 0

/* CHUNKS_PACKS_SHARED_DEFERRED is how many added packs the list files may leave out before an add appends them all. */
#define CHUNKS_PACKS_SHARED_DEFERRED ((size_t) 64)

/* CHUNKS_PACKS_SHARED_AHEAD is how far past the highest number given the pack table's file sets its bound. */
#define CHUNKS_PACKS_SHARED_AHEAD ((uint64) CHUNKS_PACKS_SHARED_DEFERRED)

/* CHUNKS_PACKS_SHARED_LAST is the highest number a pack is given, the highest a locator's six bytes hold. */
#define CHUNKS_PACKS_SHARED_LAST ((UINT64CONST(1) << 48) - 1)

/* CHUNKS_PACKS_SHARED_LIST_TAIL is the length of a list file's tail in the snapshot. */
#define CHUNKS_PACKS_SHARED_LIST_TAIL (CHUNKS_PACKS_TAIL + 8)

/* CHUNKS_PACKS_SHARED_HEAD is the length of the snapshot's fixed part: the two tails and the count of deferred adds. */
#define CHUNKS_PACKS_SHARED_HEAD (2 * CHUNKS_PACKS_SHARED_LIST_TAIL + 4)

/* CHUNKS_PACKS_SHARED_SNAPSHOT is the snapshot's largest size, with every deferred add it can hold. */
#define CHUNKS_PACKS_SHARED_SNAPSHOT (CHUNKS_PACKS_SHARED_HEAD + (CHUNKS_PACKS_SHARED_DEFERRED + 1) * CHUNKS_PACKS_ENTRY)

/*
 * ChunksPacksSharedListTail is ListTail: where the next record of a list
 * file goes, with the generation of the file's whole write and how many
 * records the file holds past its whole.
 */
typedef struct ChunksPacksSharedListTail
{
	ChunksPacksTail tail;
	uint64		appended;
} ChunksPacksSharedListTail;

/*
 * ChunksPacksSharedSnapshot is Snapshot: what a view is brought up to date
 * from, replaced whole by every change.
 */
typedef struct ChunksPacksSharedSnapshot
{
	ChunksPacksSharedListTail table;
	ChunksPacksSharedListTail forwarding;

	/*
	 * deferred is a Vec<TableEntry> of the packs added that the list files
	 * don't hold yet.
	 */
	StdVec		deferred;
} ChunksPacksSharedSnapshot;

extern const HostSharedPoolOps CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS;

extern uint64 chunks_packs_shared_given(uint64 reserved);
extern uint64 chunks_packs_shared_headroom(uint64 reserved);
extern uint64 chunks_packs_shared_taken(uint64 reserved);
extern uint64 chunks_packs_shared_bound(uint64 reserved);
extern uint64 chunks_packs_shared_reserved(uint64 bound, uint64 given);
extern void chunks_packs_shared_set(HostSharedRecord record, size_t atomic, uint64 value);
extern uint64 chunks_packs_shared_load(HostSharedRecord record, size_t atomic);
extern uint64 chunks_packs_shared_add(HostSharedRecord record, size_t atomic, uint64 value);
extern void chunks_packs_shared_Snapshot_Default_default(ChunksPacksSharedSnapshot *result);
extern void chunks_packs_shared_Snapshot_drop(ChunksPacksSharedSnapshot *self);
extern void chunks_packs_shared_Snapshot_bytes(const ChunksPacksSharedSnapshot *self, StdVec *result);
extern bool chunks_packs_shared_Snapshot_read(const uint8 *bytes, size_t bytes_len, ChunksPacksSharedSnapshot *result,
											  ChunksErrorError *error);
extern bool chunks_packs_shared_ListTail_PartialEq_eq(const ChunksPacksSharedListTail *self,
													  const ChunksPacksSharedListTail *other);
extern bool chunks_packs_shared_ListTail_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_packs_shared_Snapshot_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_packs_shared_Snapshot_PartialEq_eq(const ChunksPacksSharedSnapshot *self,
													  const ChunksPacksSharedSnapshot *other);
extern HostError chunks_packs_shared_raised(ChunksErrorError *error);
extern bool chunks_packs_shared_even_manifests(HostIoIo *io, ChunksErrorError *error);
extern bool chunks_packs_shared_marked(HostIoIo *io, bool *result, ChunksErrorError *error);

/*
 * ChunksPacksBarrierHolding is this process's exclusive hold of a
 * repository's barrier, under which no deletion vector is written and the
 * pack table's vectors don't change (bugs row 499, H14). It is the
 * capability clang's analysis checks the vector writer and the stray pass
 * by; it has no value.
 */
typedef struct ChunksPacksBarrierHolding
{
	int			unused;
}			STD_CAPABILITY("barrier")
ChunksPacksBarrierHolding;

/* CHUNKS_PACKS_BARRIER_HOLDING is this process's exclusive hold of a repository's barrier. */
extern ChunksPacksBarrierHolding CHUNKS_PACKS_BARRIER_HOLDING;

extern void chunks_packs_barrier_held(void) STD_ACQUIRE(CHUNKS_PACKS_BARRIER_HOLDING) STD_NO_THREAD_SAFETY_ANALYSIS;
extern void chunks_packs_barrier_left(void) STD_RELEASE(CHUNKS_PACKS_BARRIER_HOLDING) STD_NO_THREAD_SAFETY_ANALYSIS;

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_shared_tests_a_snapshot_read_back_is_the_snapshot_written_up_to_its_largest_size(void);
extern void test_chunks_packs_shared_tests_bytes_that_are_no_snapshot_are_refused(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_SHARED_H */
