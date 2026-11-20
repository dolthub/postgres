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
 * The node caches (shmem's cache.rs, design item 14's S5): the shared one
 * and a backend's own for its temporary trees.
 */
#ifndef DOLTGRES_SHMEM_CACHE_H
#define DOLTGRES_SHMEM_CACHE_H

#ifndef FRONTEND

#include "doltgres/pg/lib.h"
#include "doltgres/std/tuple.h"

/* SHMEM_CACHE_PARTITIONS is the number of the shared table's partitions, each under its own lock. */
#define SHMEM_CACHE_PARTITIONS ((size_t) 128)

/* SHMEM_CACHE_CLASSES is the number of frame sizes. */
#define SHMEM_CACHE_CLASSES ((size_t) 5)

/* SHMEM_CACHE_KEY is the length of a key. */
#define SHMEM_CACHE_KEY ((size_t) 54)

/* ShmemCacheLayout is a cache's frames: each class's count and size. */
typedef struct ShmemCacheLayout
{
	/* counts are the number of frames of each class. */
	size_t		counts[SHMEM_CACHE_CLASSES];
	/* sizes are each class's frame size. */
	size_t		sizes[SHMEM_CACHE_CLASSES];
} ShmemCacheLayout;

/* ShmemCacheCache is a node cache in this process: its base, its hash table, and whether its partitions take locks. */
typedef struct ShmemCacheCache
{
	uint8	   *base;
	HTAB	   *table;
	bool		shared;
} ShmemCacheCache;

extern size_t shmem_cache_Layout_size(const ShmemCacheLayout *self);
extern int64 shmem_cache_Layout_entries(const ShmemCacheLayout *self);
extern bool shmem_cache_request(const PgMayRaise *raise, const ShmemCacheLayout *layout, PgPgError *error);
extern bool shmem_cache_initialize(const PgMayRaise *raise, const ShmemCacheLayout *layout, PgPgError *error);
extern bool shmem_cache_make_local(const PgMayRaise *raise, const ShmemCacheLayout *layout, PgPgError *error);
extern bool shmem_cache_shared(ShmemCacheCache *result);
extern bool shmem_cache_local(ShmemCacheCache *result);
extern void shmem_cache_Cache_unpin(const ShmemCacheCache *self, uint32 frame);
extern bool shmem_cache_Cache_get(const ShmemCacheCache *self, const PgMayRaise *raise, const uint8 *key,
								  bool *has_result, std_TupleU32RefSliceU8_t *result, PgPgError *error);
extern bool shmem_cache_Cache_contains(const ShmemCacheCache *self, const PgMayRaise *raise, const uint8 *key,
									   bool *result, PgPgError *error);
#ifdef USE_INJECTION_POINTS
extern bool shmem_cache_Cache_counts(const ShmemCacheCache *self, const PgMayRaise *raise, const uint8 *key,
									 bool *has_result, uint64 *result_0, uint64 *result_1, PgPgError *error);
#endif
extern bool shmem_cache_Cache_install(const ShmemCacheCache *self, const PgMayRaise *raise, const uint8 *key,
									  size_t len, void (*fill) (void *captures, uint8 *bytes, size_t bytes_len),
									  void *fill_captures, bool *has_result, std_TupleU32RefSliceU8_t *result,
									  PgPgError *error);
extern bool shmem_cache_owner_enlarge(const PgMayRaise *raise, uint64 *result, PgPgError *error);
extern bool shmem_cache_owner_top_enlarge(const PgMayRaise *raise, uint64 *result, PgPgError *error);
extern void shmem_cache_owner_remember(uint64 owner, uint64 pin);
extern void shmem_cache_owner_forget(uint64 owner, uint64 pin);
extern void shmem_cache_owned_remember(uint64 owner, uint64 record);
extern void shmem_cache_owned_forget(uint64 owner, uint64 record);

#ifdef DOLTGRES_TEST_SHMEM
extern void test_shmem_cache_tests_a_layouts_parts_follow_one_another(void);
extern void test_shmem_cache_tests_every_frames_descriptor_and_bytes_lie_in_the_cache_apart(void);
#endif

#endif							/* FRONTEND */

#endif							/* DOLTGRES_SHMEM_CACHE_H */
