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
 * The node cache (host's cache.rs, design item 14's S5): decoded tree nodes
 * every backend shares, and a backend's own for its temporary trees, each
 * keyed by its packs' location and its address and read in place through a
 * pin.
 */
/* lib.h is the umbrella, whose order of its modules' parts holds whichever header comes first. */
#include "doltgres/host/lib.h"

#ifndef DOLTGRES_HOST_CACHE_H
#define DOLTGRES_HOST_CACHE_H

#include "doltgres/std/sync.h"

/* HOST_CACHE_ADDRESS is the length of a node's address. */
#define HOST_CACHE_ADDRESS 32

/* HOST_CACHE_LOCATOR is the length of a node's locator, where a copy of it is stored, as a chunk's trailer holds one. */
#define HOST_CACHE_LOCATOR 14

/* HOST_CACHE_KEY is the length of a node's key: its packs' location, its address, then its locator. */
#define HOST_CACHE_KEY (sizeof(uint64) + HOST_CACHE_ADDRESS + HOST_CACHE_LOCATOR)

/* HostCacheKey is a node's key in a cache, which only key makes. */
typedef struct HostCacheKey
{
	uint8		f0[HOST_CACHE_KEY];
} HostCacheKey;

/*
 * HostCachePlacement is where a node cache lives: the one every backend
 * shares, or the backend's own for temporary trees.
 */
typedef enum HostCachePlacement
{
	/*
	 * HOST_CACHE_PLACEMENT_SERVER is the cache in shared memory, which every
	 * backend reads.
	 */
	HOST_CACHE_PLACEMENT_SERVER,

	/*
	 * HOST_CACHE_PLACEMENT_BACKEND is the backend's own cache, for its
	 * temporary trees.
	 */
	HOST_CACHE_PLACEMENT_BACKEND,
} HostCachePlacement;

/* HostCacheBytesTag is Bytes' variants. */
typedef enum HostCacheBytesTag
{
	HOST_CACHE_BYTES_MAPPED,
	HOST_CACHE_BYTES_LENT,
} HostCacheBytesTag;

/* HostCacheBytes is a cached node's bytes as the host maps them: in place in its memory, or held while it lends them. */
typedef struct HostCacheBytes
{
	HostCacheBytesTag tag;
	union
	{
		/*
		 * mapped is a frame's bytes in the server's memory, which no one
		 * changes while the frame is pinned.
		 */
		struct
		{
			const uint8 *f0;
			size_t		f0_len;
		}			mapped;

		/*
		 * lent is a frame's bytes the simulation keeps alive for as long as
		 * they are lent, an Arc<[u8]>.
		 */
		struct
		{
			StdArcInner *f0;
			size_t		f0_len;
		}			lent;
	}			u;
} HostCacheBytes;

/* HostCacheFound is a frame a lookup or an install pinned once in shared memory, with its bytes. */
typedef struct HostCacheFound
{
	/* frame is the frame's number in its cache. */
	uint32		frame;
	/* bytes are the frame's bytes. */
	HostCacheBytes bytes;
} HostCacheFound;

/* The module's handles and functions, which take lib.h's types, follow its include. */
#include "doltgres/host/lib.h"

/* HOST_CACHE_CLASSES_LEN is the count of a node cache's frame sizes. */
#define HOST_CACHE_CLASSES_LEN 5

/* host_cache_CLASSES_t is a line of CLASSES: a frame size and its share of the cache's bytes in hundredths. */
typedef struct host_cache_CLASSES_t
{
	size_t		f0;
	size_t		f1;
} host_cache_CLASSES_t;

/*
 * HostCachePin is a pin's record: the host that pinned the frame, the
 * backend's pin table, an Arc<IoMutex<Pins>>, and the pin's slot there with
 * the generation that tells it from a later pin in the same slot. Whichever
 * of its drop and its resource owner's release comes first unpins the frame.
 */
typedef struct HostCachePin
{
	HostHost	host;
	StdArcInner *table;
	uint32		slot;
	uint32		generation;
} HostCachePin;

/*
 * HostCachePinned is a cached node pinned for as long as it lives, whose
 * bytes it lends within the entry that took or resumed it, which its resource
 * owner outlasts; it is neither sent nor copied.
 */
typedef struct HostCachePinned
{
	HostCachePin pin;
	HostCacheBytes bytes;
} HostCachePinned;

/*
 * HostCacheKept is a pin kept across calls, under the resource owner that
 * took it. It lends no bytes, since that owner may release the pin between
 * calls: resume gives the pinned node back in a later entry while the pin
 * lasts.
 */
typedef struct HostCacheKept
{
	HostCachePin pin;
	HostCacheBytes bytes;
} HostCacheKept;

/* HostCacheReadTag is Read's variants. */
typedef enum HostCacheReadTag
{
	HOST_CACHE_READ_CACHED,
	HOST_CACHE_READ_UNCACHED,
} HostCacheReadTag;

/* HostCacheRead is a node an install gave back: pinned in its cache, or in memory of its own when no frame was free. */
typedef struct HostCacheRead
{
	HostCacheReadTag tag;
	union
	{
		/* cached is the node pinned in its cache. */
		HostCachePinned cached;

		/*
		 * uncached is the node in memory of its own, which no frame could
		 * take without waiting.
		 */
		StdVec		uncached;
	}			u;
} HostCacheRead;

/* HostCacheLive is a pin in the table: its frame and the resource owner that remembered it. */
typedef struct HostCacheLive
{
	HostCachePlacement placement;
	uint32		frame;
	uint64		owner;
} HostCacheLive;

/* HostCacheEntry is a slot of the pin table: its generation, and its pin while it lives. */
typedef struct HostCacheEntry
{
	uint32		generation;
	bool		has_live;
	HostCacheLive live;
} HostCacheEntry;

/*
 * HostCachePins is a backend's pin table: an entry per pin, in slots a
 * removal clears and a later pin reuses, so a removal never allocates, and
 * the host that pinned them. A frame the backend pins more than once is
 * pinned once in shared memory, which the entries themselves tell. Nothing
 * under its lock panics, so the lock is never poisoned and the table never
 * rebuilt under live pins.
 */
typedef struct HostCachePins
{
	StdVec		entries;
	bool		has_host;
	HostHost	host;
} HostCachePins;

extern const uint8 *host_cache_Key_bytes(const HostCacheKey *self);
extern HostCacheKey host_cache_key(uint64 location, const uint8 *address, const uint8 *locator);
extern const host_cache_CLASSES_t HOST_CACHE_CLASSES[HOST_CACHE_CLASSES_LEN];
extern void host_cache_frames(size_t bytes, size_t *result);
extern bool host_cache_class(size_t len, size_t *result);
extern StdBytes host_cache_Bytes_Deref_deref(const HostCacheBytes *self);
extern void host_cache_Bytes_drop(HostCacheBytes *self);
extern StdBytes host_cache_Pinned_Deref_deref(const HostCachePinned *self);
extern HostCacheKept host_cache_Pinned_keep(HostCachePinned *self);
extern void host_cache_Pinned_drop(HostCachePinned *self);
extern bool host_cache_Kept_resume(HostCacheKept *self, HostIoIo *io, HostCachePinned *result);
extern void host_cache_Kept_drop(HostCacheKept *self);
extern void host_cache_Pin_drop(HostCachePin *self);
extern StdBytes host_cache_Read_Deref_deref(const HostCacheRead *self);
extern void host_cache_Read_drop(HostCacheRead *self);
extern bool host_cache_get(HostIoIo *io, HostCachePlacement placement, const HostCacheKey *key,
						   bool *has_result, HostCachePinned *result, HostError *error);
extern bool host_cache_install(HostIoIo *io, HostCachePlacement placement, const HostCacheKey *key,
							   size_t len, void (*fill) (void *, uint8 *, size_t),
							   void *fill_captures, void (*fill_drop) (void *),
							   HostCacheRead *result, HostError *error);
extern bool host_cache_contains(HostIoIo *io, HostCachePlacement placement,
								const HostCacheKey *key, bool *result, HostError *error);
extern bool host_cache_make_local(HostIoIoDoors io, size_t bytes, HostError *error);
extern void host_cache_released(uint64 remembered);
extern void host_cache_release_all(void);
#ifdef USE_INJECTION_POINTS
extern void host_cache_unpin_number(HostCachePlacement placement, uint32 frame);
#endif

#ifdef DOLTGRES_TEST_HOST
extern void test_host_cache_tests_a_key_holds_its_location_its_whole_address_and_its_whole_locator(void);
#endif

#endif							/* DOLTGRES_HOST_CACHE_H */
