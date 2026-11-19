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
 * The fork's single records of chunks' shared state (chunks' packs/single.rs):
 * so far the kind of a file in the removal queue.
 */
#ifndef DOLTGRES_CHUNKS_PACKS_SINGLE_H
#define DOLTGRES_CHUNKS_PACKS_SINGLE_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/fmt.h"

/* CHUNKS_PACKS_SINGLE_MAKING_INDEX is Making's lock, taken to make a repository's packs directory. */
#define CHUNKS_PACKS_SINGLE_MAKING_INDEX 0

/* CHUNKS_PACKS_SINGLE_REMOVING_INDEX is Removing's lock, which guards the removal queue. */
#define CHUNKS_PACKS_SINGLE_REMOVING_INDEX 1

/* CHUNKS_PACKS_SINGLE_SPACES_INDEX is Spaces' long lock. */
#define CHUNKS_PACKS_SINGLE_SPACES_INDEX 2

/* CHUNKS_PACKS_SINGLE_FORGOTTEN_INDEX is Forgotten's atomic. */
#define CHUNKS_PACKS_SINGLE_FORGOTTEN_INDEX 0

/* CHUNKS_PACKS_SINGLE_HITS is HITS, the array of markings' hits. */
#define CHUNKS_PACKS_SINGLE_HITS 0

/* CHUNKS_PACKS_SINGLE_HIT_SLOTS is HIT_SLOTS, the hits array's count of entries. */
#define CHUNKS_PACKS_SINGLE_HIT_SLOTS 16384

/* CHUNKS_PACKS_SINGLE_HIT is HIT, a hit's size: an address, then a locator's pack, offset, and length. */
#define CHUNKS_PACKS_SINGLE_HIT 52

/* CHUNKS_PACKS_SINGLE_QUEUE is QUEUE, the array of the removal queue. */
#define CHUNKS_PACKS_SINGLE_QUEUE 1

/* CHUNKS_PACKS_SINGLE_QUEUED is QUEUED, the removal queue's count of entries. */
#define CHUNKS_PACKS_SINGLE_QUEUED 1024

/* CHUNKS_PACKS_SINGLE_REMOVAL is REMOVAL, a removal queue entry's size. */
#define CHUNKS_PACKS_SINGLE_REMOVAL 48

/* ChunksPacksSingleQueuedTag is Queued's variants. */
typedef enum ChunksPacksSingleQueuedTag
{
	/* A retired pack's file. */
	CHUNKS_PACKS_SINGLE_QUEUED_PACK,

	/*
	 * An address-index run's file: a retired pack's own, or one a merge
	 * replaced.
	 */
	CHUNKS_PACKS_SINGLE_QUEUED_RUN,
	/* A generation of a pack's deletion vector. */
	CHUNKS_PACKS_SINGLE_QUEUED_VECTOR,
} ChunksPacksSingleQueuedTag;

/* ChunksPacksSingleQueued is Queued: the kind of a file in the removal queue, with the generation Vector names. */
typedef struct ChunksPacksSingleQueued
{
	ChunksPacksSingleQueuedTag tag;
	union
	{
		uint64		vector;
	}			u;
} ChunksPacksSingleQueued;

/*
 * ChunksPacksSingleRemoval is Removal: a file that waits in the removal queue
 * for the readers that may hold it, its repository, its tablespace, its kind,
 * the id its name begins with, and the epoch it was queued at.
 */
typedef struct ChunksPacksSingleRemoval
{
	uint32		repository;
	uint32		space;
	ChunksPacksSingleQueued kind;
	ChunksPackPackId id;
	uint64		epoch;
} ChunksPacksSingleRemoval;

extern const HostSharedSingleOps CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS;

extern void chunks_packs_single_hit(const ChunksAddressAddress *address, ChunksTrailerLocator locator, uint8 *result);
extern bool chunks_packs_single_hit_of(const uint8 *bytes, size_t bytes_len, chunks_TupleAddressLocator_t *result,
									   ChunksErrorError *error);
extern bool chunks_packs_single_Queued_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_packs_single_Queued_PartialEq_eq(const ChunksPacksSingleQueued *self,
													const ChunksPacksSingleQueued *other);
extern bool chunks_packs_single_Removal_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_packs_single_Removal_PartialEq_eq(const ChunksPacksSingleRemoval *self,
													 const ChunksPacksSingleRemoval *other);
extern void chunks_packs_single_Removal_bytes(const ChunksPacksSingleRemoval *self, uint8 *result);
extern bool chunks_packs_single_Removal_read(const uint8 *bytes, size_t bytes_len, bool *some,
											 ChunksPacksSingleRemoval *result, ChunksErrorError *error);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_single_tests_a_hit_keeps_a_wide_locator(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_SINGLE_H */
