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
 * The node caches (design item 14's S5): the shared one, a region of
 * Postgres' main shared memory sized by node_cache_size with a partitioned
 * shared hash table, and a backend's own for its temporary trees, the same
 * layout in its memory with a hash table of its own and no locks. Each class
 * of frame size is a clock of fixed frames, whose descriptors hold a state
 * word as BufferDesc does: the pin count, the usage count, whether the frame
 * holds a key, and whether its bytes are whole. A frame's bytes change only
 * while its one pin is its installer's, with its valid bit clear, so a pinned
 * valid frame's bytes are read in place.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/shmem/cache.h"
#include "doltgres/shmem/lib.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/test.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "pg_calls.h"

#define RUST_FILE "shmem" STD_SEP "src" STD_SEP "cache.rs"

/* SHMEM_CACHE_PINS is the state word's pin count. */
#define SHMEM_CACHE_PINS ((uint64) PG_UINT32_MAX)

/* SHMEM_CACHE_USAGE is the state word's usage count, above the pin count. */
#define SHMEM_CACHE_USAGE ((uint64) 0xf << 32)

/* SHMEM_CACHE_USAGE_ONE is a usage count of one. */
#define SHMEM_CACHE_USAGE_ONE ((uint64) 1 << 32)

/* SHMEM_CACHE_MAX_USAGE is the usage count a pin raises a frame to at most, Postgres' BM_MAX_USAGE_COUNT. */
#define SHMEM_CACHE_MAX_USAGE ((uint64) 5)

/* SHMEM_CACHE_TAGGED is the state word's bit of a frame the table names by its key. */
#define SHMEM_CACHE_TAGGED ((uint64) 1 << 40)

/* SHMEM_CACHE_VALID is the state word's bit of a frame whose bytes are whole. */
#define SHMEM_CACHE_VALID ((uint64) 1 << 41)

/*
 * ShmemCachePlaced is where a class's frames are in a cache: its first
 * frame's number, its count of frames, their size, and where their bytes
 * start.
 */
typedef struct ShmemCachePlaced
{
	uint32		start;
	uint32		count;
	uint64		size;
	uint64		frames_at;
} ShmemCachePlaced;

/* ShmemCacheClass is a frame size's clock: where its frames are, and its hand. */
typedef struct ShmemCacheClass
{
	ShmemCachePlaced placed;
	pg_atomic_uint64 hand;
} ShmemCacheClass;

/* ShmemCacheHeader is a cache's fixed part: its partition locks (the shared cache's only) and its classes. */
typedef struct ShmemCacheHeader
{
	LWLock		partitions[SHMEM_CACHE_PARTITIONS];
	ShmemCacheClass classes[SHMEM_CACHE_CLASSES];
} ShmemCacheHeader;

/* ShmemCacheDesc is a frame's descriptor: its state word, the length of its bytes, and its key while it is tagged. */
typedef struct ShmemCacheDesc
{
	pg_atomic_uint64 state;
	uint64		len;
	uint8		key[SHMEM_CACHE_KEY];
} ShmemCacheDesc;

/* ShmemCacheEntry is the hash table's entry: a key and the frame that holds it. */
typedef struct ShmemCacheEntry
{
	uint8		key[SHMEM_CACHE_KEY];
	uint32		frame;
} ShmemCacheEntry;

/* SHMEM_CACHE_DESCS is the offset of the descriptors in a cache: after the header, on a cache line. */
#define SHMEM_CACHE_DESCS ((size_t) TYPEALIGN(64, sizeof(ShmemCacheHeader)))

/* shmem_cache_Placed_frame_at returns where a frame of the class starts in its cache. */
static size_t
shmem_cache_Placed_frame_at(const ShmemCachePlaced *self, uint32 frame)
{
	uint32		tmp_1 = STD_SUB(u32, frame, self->start, 71, 36);
	size_t		tmp_2 = STD_MUL(usize, (size_t) tmp_1, (size_t) self->size, 71, 35);

	return STD_ADD(usize, (size_t) self->frames_at, tmp_2, 71, 9);
}

/* shmem_cache_desc_at returns where a frame's descriptor starts in its cache. */
static size_t
shmem_cache_desc_at(uint32 frame)
{
	size_t		tmp_1 = STD_MUL(usize, (size_t) frame, sizeof(ShmemCacheDesc), 77, 13);

	return STD_ADD(usize, SHMEM_CACHE_DESCS, tmp_1, 77, 5);
}

/* shmem_cache_Layout_frames returns the number of frames. */
static size_t
shmem_cache_Layout_frames(const ShmemCacheLayout *self)
{
	size_t		sum = 0;

	for (size_t i = 0, i_len = SHMEM_CACHE_CLASSES; i < i_len; i++)
		sum = std_add_usize(sum, self->counts[i], STD_ACCUM_FILE, 206, 1);
	return sum;
}

/* shmem_cache_Layout_bytes_at returns where the frames' bytes start. */
static size_t
shmem_cache_Layout_bytes_at(const ShmemCacheLayout *self)
{
	return std_next_multiple_of_usize(shmem_cache_desc_at((uint32) shmem_cache_Layout_frames(self)), 64);
}

/* ShmemCacheLayoutPlacedClosure0 is placed's from_fn closure's captures: the layout, and the next start and offset. */
typedef struct ShmemCacheLayoutPlacedClosure0
{
	const ShmemCacheLayout *self;
	size_t	   *start;
	size_t	   *at;
} ShmemCacheLayoutPlacedClosure0;

/* shmem_cache_Layout_placed_closure_0 places a class's frames after the class before it. */
static ShmemCachePlaced
shmem_cache_Layout_placed_closure_0(void *captures_, size_t class)
{
	ShmemCacheLayoutPlacedClosure0 *captures = captures_;
	size_t		count;
	size_t		size;
	ShmemCachePlaced placed;
	size_t		tmp_1;

	std_index_check(class, SHMEM_CACHE_CLASSES, RUST_FILE, 136, 34);
	count = captures->self->counts[class];
	std_index_check(class, SHMEM_CACHE_CLASSES, RUST_FILE, 136, 54);
	size = captures->self->sizes[class];
	placed.start = (uint32) *captures->start;
	placed.count = (uint32) count;
	placed.size = (uint64) size;
	placed.frames_at = (uint64) *captures->at;
	*captures->start = STD_ADD(usize, *captures->start, count, 138, 13);
	tmp_1 = STD_MUL(usize, count, size, 139, 19);
	*captures->at = STD_ADD(usize, *captures->at, tmp_1, 139, 13);
	return placed;
}

/* shmem_cache_Layout_placed_frame_0_t is placed's from_fn's state across its pad: its result and the Guard's count. */
typedef struct shmem_cache_Layout_placed_frame_0_t
{
	ShmemCacheLayoutPlacedClosure0 *captures;
	ShmemCachePlaced *result;
	size_t		initialized;
} shmem_cache_Layout_placed_frame_0_t;

/* shmem_cache_Layout_placed_body_0 is core's array::from_fn over the classes, with try_from_fn_erased's preconditions. */
static void
shmem_cache_Layout_placed_body_0(void *frame_)
{
	shmem_cache_Layout_placed_frame_0_t *frame = frame_;

	while (frame->initialized < SHMEM_CACHE_CLASSES)
	{
		ShmemCachePlaced item = shmem_cache_Layout_placed_closure_0(frame->captures, frame->initialized);

		std_slice_get_unchecked_mut_check(frame->initialized, SHMEM_CACHE_CLASSES, STD_ARRAY_MOD_FILE, 954, 28);
		frame->result[frame->initialized] = item;
		std_usize_unchecked_add_check(frame->initialized, 1, STD_ARRAY_MOD_FILE, 955, 49);
		frame->initialized = std_wrapping_add_usize(frame->initialized, 1);
	}
}

/* shmem_cache_Layout_placed_pad_0 is from_fn's pad: the Guard's drop. */
static void
shmem_cache_Layout_placed_pad_0(void *frame_)
{
	shmem_cache_Layout_placed_frame_0_t *frame = frame_;

	std_array_Guard_drop(frame->initialized, SHMEM_CACHE_CLASSES);
}

/* shmem_cache_Layout_placed returns where each class's frames are, one class's after another's. */
static void
shmem_cache_Layout_placed(const ShmemCacheLayout *self, ShmemCachePlaced result[SHMEM_CACHE_CLASSES])
{
	size_t		start = 0;
	size_t		at = shmem_cache_Layout_bytes_at(self);
	ShmemCacheLayoutPlacedClosure0 captures = {self, &start, &at};
	shmem_cache_Layout_placed_frame_0_t frame = {.captures = &captures, .result = result};

	std_landing(&frame, shmem_cache_Layout_placed_body_0, shmem_cache_Layout_placed_pad_0);
}

/* shmem_cache_Layout_size_closure_0 is size's map: a class's frames' bytes. */
static size_t
shmem_cache_Layout_size_closure_0(void *captures, size_t count, size_t size)
{
	(void) captures;
	return STD_MUL(usize, count, size, 146, 82);
}

/* shmem_cache_Layout_size returns the size of a cache of this layout. */
size_t
shmem_cache_Layout_size(const ShmemCacheLayout *self)
{
	size_t		bytes_at = shmem_cache_Layout_bytes_at(self);
	size_t		sum = 0;

	for (size_t i = 0, i_len = SHMEM_CACHE_CLASSES; i < i_len; i++)
	{
		size_t		item = shmem_cache_Layout_size_closure_0(NULL, self->counts[i], self->sizes[i]);

		sum = std_add_usize(sum, item, STD_ACCUM_FILE, 206, 1);
	}
	return STD_ADD(usize, bytes_at, sum, 146, 9);
}

/*
 * shmem_cache_Layout_entries returns the number of entries the hash table
 * needs, so no insert finds it full: one per frame and one per partition, as
 * Postgres sizes its buffer table.
 */
int64
shmem_cache_Layout_entries(const ShmemCacheLayout *self)
{
	return (int64) STD_ADD(usize, shmem_cache_Layout_frames(self), SHMEM_CACHE_PARTITIONS, 152, 10);
}

/* shmem_cache_request asks Postgres for the shared cache's region and hash table, in the postmaster's request callback. */
bool
shmem_cache_request(const PgMayRaise *raise, const ShmemCacheLayout *layout, PgPgError *error)
{
	if (!pg_calls_request_cache(raise, shmem_cache_Layout_size(layout), error))
		return false;
	return pg_calls_request_cache_table(raise, shmem_cache_Layout_entries(layout), SHMEM_CACHE_KEY,
										sizeof(ShmemCacheEntry), error);
}

static void shmem_cache_lay_out(void *base, const ShmemCacheLayout *layout);

/*
 * shmem_cache_initialize sets the shared cache up in the postmaster's
 * initialization callback: its partition locks in their tranche, its classes,
 * and its descriptors, every frame free.
 */
bool
shmem_cache_initialize(const PgMayRaise *raise, const ShmemCacheLayout *layout, PgPgError *error)
{
	int32		tranche;
	void	   *base;
	ShmemCacheHeader *header;

	tranche = shmem_tranche(DOLTGRES_TRANCHE_CACHE);
	base = pg_calls_cache_base();
	header = base;
	for (size_t at = 0, at_end = SHMEM_CACHE_PARTITIONS; at < at_end; at = std_forward_unchecked_usize_Checked(at, 1))
	{
		std_index_check(at, SHMEM_CACHE_PARTITIONS, RUST_FILE, 172, 44);
		pg_calls_lwlock_initialize(&header->partitions[at], tranche);
	}
	shmem_cache_lay_out(base, layout);
	return true;
}

/* shmem_cache_make_local makes the backend's own cache once, in its memory, with its own hash table. */
bool
shmem_cache_make_local(const PgMayRaise *raise, const ShmemCacheLayout *layout, PgPgError *error)
{
	if (!(pg_calls_local_cache_base() == NULL))
		return true;
	if (!pg_calls_make_local_cache(raise, shmem_cache_Layout_size(layout), shmem_cache_Layout_entries(layout),
								   SHMEM_CACHE_KEY, sizeof(ShmemCacheEntry), error))
		return false;
	shmem_cache_lay_out(pg_calls_local_cache_base(), layout);
	return true;
}

/*
 * shmem_cache_lay_out writes a cache's classes and frees its frames. base is
 * shmem_cache_Layout_size's writable bytes, aligned as Postgres aligns what
 * it allocates, that nothing else uses yet.
 */
static void
shmem_cache_lay_out(void *base, const ShmemCacheLayout *layout)
{
	ShmemCacheHeader *header = base;
	ShmemCachePlaced placed[SHMEM_CACHE_CLASSES];
	size_t		frames;

	shmem_cache_Layout_placed(layout, placed);
	for (size_t placed_at = 0, placed_index, count = 0;
		 std_array_IntoIter_next(&placed_at, SHMEM_CACHE_CLASSES, SHMEM_CACHE_CLASSES, &placed_index);)
	{
		size_t		class = count;
		ShmemCacheClass *head;

		count = std_Enumerate_count_step(count);
		std_index_check(class, SHMEM_CACHE_CLASSES, RUST_FILE, 202, 33);
		head = &header->classes[class];
		head->placed = placed[placed_index];
		pg_calls_atomic_init_u64(&head->hand, 0);
	}
	frames = shmem_cache_Layout_frames(layout);
	for (uint32 frame = 0, frame_end = (uint32) frames; frame < frame_end; frame = std_forward_unchecked_u32_Checked(frame, 1))
	{
		ShmemCacheDesc *desc = (ShmemCacheDesc *) STD_PTR_AT(base, shmem_cache_desc_at(frame), 1);

		pg_calls_atomic_init_u64(&desc->state, 0);
		desc->len = 0;
	}
}

/* shmem_cache_shared returns the shared cache, which Postgres maps before any entry runs. */
bool
shmem_cache_shared(ShmemCacheCache *result)
{
	uint8	   *base = pg_calls_cache_base();

	if (base == NULL)
		return false;
	*result = (ShmemCacheCache)
	{
		base, pg_calls_cache_table(), true
	};
	return true;
}

/* shmem_cache_local returns the backend's own cache, or nothing before shmem_cache_make_local. */
bool
shmem_cache_local(ShmemCacheCache *result)
{
	uint8	   *base = pg_calls_local_cache_base();

	if (base == NULL)
		return false;
	*result = (ShmemCacheCache)
	{
		base, pg_calls_local_cache_table(), false
	};
	return true;
}

/* shmem_cache_Cache_header returns the cache's header. */
static ShmemCacheHeader *
shmem_cache_Cache_header(const ShmemCacheCache *self)
{
	return (ShmemCacheHeader *) self->base;
}

/* shmem_cache_Cache_desc returns a frame's descriptor. */
static ShmemCacheDesc *
shmem_cache_Cache_desc(const ShmemCacheCache *self, uint32 frame)
{
	return (ShmemCacheDesc *) STD_PTR_AT(self->base, shmem_cache_desc_at(frame), 1);
}

/* shmem_cache_Cache_class returns a class's clock. */
static ShmemCacheClass *
shmem_cache_Cache_class(const ShmemCacheCache *self, size_t class)
{
	ShmemCacheHeader *header = shmem_cache_Cache_header(self);

	std_index_check(class, SHMEM_CACHE_CLASSES, RUST_FILE, 254, 27);
	return &header->classes[class];
}

/* shmem_cache_Cache_placed returns where a class's frames are. */
static ShmemCachePlaced
shmem_cache_Cache_placed(const ShmemCacheCache *self, size_t class)
{
	return shmem_cache_Cache_class(self, class)->placed;
}

/* shmem_cache_Cache_class_of_closure_0 is find's predicate: whether a class holds the frame. */
static bool
shmem_cache_Cache_class_of_closure_0(const ShmemCacheCache *self, uint32 frame, size_t class)
{
	ShmemCachePlaced placed = shmem_cache_Cache_placed(self, class);

	return frame >= placed.start
		&& frame < STD_ADD(u32, placed.start, placed.count, 287, 50);
}

/* shmem_cache_Cache_class_of returns the class a frame is in. */
static size_t
shmem_cache_Cache_class_of(const ShmemCacheCache *self, uint32 frame)
{
	for (size_t class = 0; class < SHMEM_CACHE_CLASSES; class = std_forward_unchecked_usize_Checked(class, 1))
	{
		if (shmem_cache_Cache_class_of_closure_0(self, frame, class))
			return class;
	}
	STD_OPTION_EXPECT_FAILED(289, 14, "a frame number names a frame of the cache");
}

/*
 * shmem_cache_Cache_bytes returns a frame's bytes, of the length its
 * descriptor holds. The caller pins the frame, which is valid, so no process
 * changes its bytes or its length while the returned bytes are read, and
 * reads them only until it unpins it.
 */
static const uint8 *
shmem_cache_Cache_bytes(const ShmemCacheCache *self, uint32 frame, size_t *result_len)
{
	ShmemCachePlaced placed = shmem_cache_Cache_placed(self, shmem_cache_Cache_class_of(self, frame));
	size_t		at = shmem_cache_Placed_frame_at(&placed, frame);
	size_t		len = (size_t) shmem_cache_Cache_desc(self, frame)->len;
	const uint8 *bytes = STD_PTR_AT(self->base, at, 1);

	std_slice_from_raw_parts_check(bytes, len, 1, 1, RUST_FILE, 278, 13);
	*result_len = len;
	return bytes;
}

/* shmem_cache_Cache_state reads a frame's state word. */
static uint64
shmem_cache_Cache_state(const ShmemCacheCache *self, uint32 frame)
{
	return pg_calls_atomic_read_u64(&shmem_cache_Cache_desc(self, frame)->state);
}

/* shmem_cache_Cache_exchange sets a frame's state word from one value to another, reporting whether it held the first. */
static bool
shmem_cache_Cache_exchange(const ShmemCacheCache *self, uint32 frame, uint64 from, uint64 to)
{
	uint64		seen = from;

	pg_calls_atomic_exchange_u64(&shmem_cache_Cache_desc(self, frame)->state, &seen, to);
	return seen == from;
}

/* shmem_cache_Cache_add adds to a frame's state word. */
static void
shmem_cache_Cache_add(const ShmemCacheCache *self, uint32 frame, int64 add)
{
	(void) pg_calls_atomic_add_u64(&shmem_cache_Cache_desc(self, frame)->state, add);
}

/*
 * shmem_cache_Cache_pin pins a frame whose bytes are whole, raising its usage
 * count as PinBuffer, or changes nothing and returns false for a frame being
 * filled. Under the frame's partition lock a frame only becomes whole, so the
 * check and the pin are one step.
 */
static bool
shmem_cache_Cache_pin(const ShmemCacheCache *self, uint32 frame)
{
	for (;;)
	{
		uint64		state = shmem_cache_Cache_state(self, frame);
		uint64		usage;
		uint64		tmp_1;

		if ((state & SHMEM_CACHE_VALID) == 0)
			return false;
		usage = STD_ADD(u64, std_min_u64(STD_SHR_FOLDED(u64, state & SHMEM_CACHE_USAGE, 32), SHMEM_CACHE_MAX_USAGE - 1), 1,
						324, 25);
		tmp_1 = STD_ADD(u64, (state & ~SHMEM_CACHE_USAGE) | STD_SHL_FOLDED(u64, usage, 32), 1, 325, 44);
		if (shmem_cache_Cache_exchange(self, frame, state, tmp_1))
			return true;
	}
}

/* shmem_cache_Cache_frames returns the number of the cache's frames. */
static uint32
shmem_cache_Cache_frames(const ShmemCacheCache *self)
{
	ShmemCachePlaced last = shmem_cache_Cache_placed(self, SHMEM_CACHE_CLASSES - 1);

	return STD_ADD(u32, last.start, last.count, 334, 9);
}

/* shmem_cache_Cache_release gives up a pin of a frame of the cache. */
static void
shmem_cache_Cache_release(const ShmemCacheCache *self, uint32 frame)
{
	shmem_cache_Cache_add(self, frame, -1);
}

/*
 * shmem_cache_Cache_unpin gives up a pin of a frame, the door of the
 * backend's pin table, which alone knows the pins the process holds; a number
 * past the cache's frames changes nothing. It neither allocates nor raises.
 */
void
shmem_cache_Cache_unpin(const ShmemCacheCache *self, uint32 frame)
{
	if (frame < shmem_cache_Cache_frames(self))
		shmem_cache_Cache_release(self, frame);
}

/* shmem_cache_Cache_partition returns the address of a hash's partition lock. */
static LWLock *
shmem_cache_Cache_partition(const ShmemCacheCache *self, uint32 hash)
{
	ShmemCacheHeader *header = shmem_cache_Cache_header(self);
	size_t		part = STD_REM_FOLDED(usize, (size_t) hash, SHMEM_CACHE_PARTITIONS);

	std_index_check(part, SHMEM_CACHE_PARTITIONS, RUST_FILE, 354, 27);
	return &header->partitions[part];
}

/* ShmemCacheLocked is a cache's partition locks held, in the order taken, released when it drops. */
typedef struct ShmemCacheLocked
{
	const ShmemCacheCache *cache;
	bool		has_held[2];
	size_t		held[2];
} ShmemCacheLocked;

/* shmem_cache_Locked_drop is Locked's drop: the locks it holds released, the last taken first. */
static void
shmem_cache_Locked_drop(ShmemCacheLocked *self)
{
	for (size_t i = 2; i > 0;)
	{
		i--;
		if (self->has_held[i])
			pg_calls_lwlock_release(shmem_cache_Cache_partition(self->cache, (uint32) self->held[i]));
	}
}

/* shmem_cache_Cache_lock_frame_0_t is lock's state across its pad: its arguments, the guard, and the parts to take. */
typedef struct shmem_cache_Cache_lock_frame_0_t
{
	const ShmemCacheCache *self;
	const PgMayRaise *raise;
	LWLockMode	mode;
	bool		has_ordered[2];
	size_t		ordered[2];
	PgPgError  *error;
	bool		done;
	ShmemCacheLocked locked;
	bool		locked_live;
} shmem_cache_Cache_lock_frame_0_t;

/* shmem_cache_Cache_lock_body_0 takes the ordered partitions' locks, recording each in the guard. */
static void
shmem_cache_Cache_lock_body_0(void *frame_)
{
	shmem_cache_Cache_lock_frame_0_t *frame = frame_;

	frame->locked = (ShmemCacheLocked)
	{
		frame->self,
		{
			false, false
		},
		{
			0, 0
		}
	};
	frame->locked_live = true;
	if (frame->self->shared)
	{
		for (size_t ordered_at = 0, ordered_index, count = 0; std_array_IntoIter_next(&ordered_at, 2, 2, &ordered_index);)
		{
			size_t		at;
			size_t		part;

			if (!frame->has_ordered[ordered_index])
				continue;
			part = frame->ordered[ordered_index];
			at = count;
			count = std_Enumerate_count_step(count);
			if (!pg_calls_lwlock_acquire(frame->raise, shmem_cache_Cache_partition(frame->self, (uint32) part),
										 frame->mode, frame->error))
			{
				frame->done = false;
				frame->locked_live = false;
				shmem_cache_Locked_drop(&frame->locked);
				return;
			}
			std_index_check(at, 2, RUST_FILE, 378, 17);
			frame->locked.has_held[at] = true;
			frame->locked.held[at] = part;
		}
	}
	frame->locked_live = false;
	frame->done = true;
}

/* shmem_cache_Cache_lock_pad_0 is lock's pad: the guard, while it is live. */
static void
shmem_cache_Cache_lock_pad_0(void *frame_)
{
	shmem_cache_Cache_lock_frame_0_t *frame = frame_;

	if (frame->locked_live)
		shmem_cache_Locked_drop(&frame->locked);
}

/*
 * shmem_cache_Cache_lock takes the partition lock of a hash, and of another
 * if it is given, in partition order, each once, in a mode, if the cache is
 * shared.
 */
static bool
shmem_cache_Cache_lock(const ShmemCacheCache *self, const PgMayRaise *raise, uint32 hash, bool has_other,
					   uint32 other_hash, LWLockMode mode, ShmemCacheLocked *result, PgPgError *error)
{
	size_t		part = STD_REM_FOLDED(usize, (size_t) hash, SHMEM_CACHE_PARTITIONS);
	bool		has = false;
	size_t		other = 0;
	shmem_cache_Cache_lock_frame_0_t frame = {.self = self, .raise = raise, .mode = mode, .error = error};

	if (has_other)
	{
		other = STD_REM_FOLDED(usize, (size_t) other_hash, SHMEM_CACHE_PARTITIONS);
		has = other != part;
	}
	if (has && other < part)
	{
		frame.has_ordered[0] = true;
		frame.ordered[0] = other;
		frame.has_ordered[1] = true;
		frame.ordered[1] = part;
	}
	else
	{
		frame.has_ordered[0] = true;
		frame.ordered[0] = part;
		frame.has_ordered[1] = has;
		frame.ordered[1] = other;
	}
	std_landing(&frame, shmem_cache_Cache_lock_body_0, shmem_cache_Cache_lock_pad_0);
	if (frame.done)
		*result = frame.locked;
	return frame.done;
}

/* shmem_cache_Cache_hash returns a key's hash. */
static uint32
shmem_cache_Cache_hash(const ShmemCacheCache *self, const uint8 *key)
{
	return pg_calls_hash_value(self->table, key);
}

/* shmem_cache_Cache_find returns the frame the table names for a key, under its partition's lock. */
static bool
shmem_cache_Cache_find(const ShmemCacheCache *self, const uint8 *key, uint32 hash, uint32 *result)
{
	ShmemCacheEntry *entry = pg_calls_hash_find(self->table, key, hash);

	if (entry == NULL)
		return false;
	*result = entry->frame;
	return true;
}

/* shmem_cache_Cache_get_frame_0_t is get's state across its pad: its arguments, its result, and the guard. */
typedef struct shmem_cache_Cache_get_frame_0_t
{
	const ShmemCacheCache *self;
	const uint8 *key;
	uint32		hash;
	bool	   *has_result;
	std_TupleU32RefSliceU8_t *result;
	ShmemCacheLocked locked;
	bool		locked_live;
} shmem_cache_Cache_get_frame_0_t;

/* shmem_cache_Cache_get_body_0 finds the key's frame and pins it under the guard. */
static void
shmem_cache_Cache_get_body_0(void *frame_)
{
	shmem_cache_Cache_get_frame_0_t *frame = frame_;
	uint32		found;

	if (!shmem_cache_Cache_find(frame->self, frame->key, frame->hash, &found)
		|| !shmem_cache_Cache_pin(frame->self, found))
	{
		*frame->has_result = false;
		frame->locked_live = false;
		shmem_cache_Locked_drop(&frame->locked);
		return;
	}
	frame->locked_live = false;
	shmem_cache_Locked_drop(&frame->locked);
	*frame->has_result = true;
	frame->result->f0 = found;
	frame->result->f1 = shmem_cache_Cache_bytes(frame->self, found, &frame->result->f1_len);
}

/* shmem_cache_Cache_get_pad_0 is get's pad: the guard, while it is live. */
static void
shmem_cache_Cache_get_pad_0(void *frame_)
{
	shmem_cache_Cache_get_frame_0_t *frame = frame_;

	if (frame->locked_live)
		shmem_cache_Locked_drop(&frame->locked);
}

/*
 * shmem_cache_Cache_get pins the frame that holds a key's node whole and
 * returns its number and bytes, or returns nothing, having changed nothing,
 * when no frame holds the key or its frame is being filled. The bytes stay
 * as they are until the frame is unpinned, which the caller does once it no
 * longer reads them.
 */
bool
shmem_cache_Cache_get(const ShmemCacheCache *self, const PgMayRaise *raise, const uint8 *key, bool *has_result,
					  std_TupleU32RefSliceU8_t *result, PgPgError *error)
{
	uint32		hash = shmem_cache_Cache_hash(self, key);
	shmem_cache_Cache_get_frame_0_t frame = {.self = self, .key = key, .hash = hash, .has_result = has_result,
	.result = result};

	if (!shmem_cache_Cache_lock(self, raise, hash, false, 0, LW_SHARED, &frame.locked, error))
		return false;
	frame.locked_live = true;
	std_landing(&frame, shmem_cache_Cache_get_body_0, shmem_cache_Cache_get_pad_0);
	return true;
}

/*
 * shmem_cache_Cache_contains reports whether the table names a key, whether
 * or not its frame's bytes are whole, changing nothing.
 */
bool
shmem_cache_Cache_contains(const ShmemCacheCache *self, const PgMayRaise *raise, const uint8 *key, bool *result,
						   PgPgError *error)
{
	uint32		hash = shmem_cache_Cache_hash(self, key);
	ShmemCacheLocked locked;
	uint32		found;

	if (!shmem_cache_Cache_lock(self, raise, hash, false, 0, LW_SHARED, &locked, error))
		return false;
	*result = shmem_cache_Cache_find(self, key, hash, &found);
	shmem_cache_Locked_drop(&locked);
	return true;
}

#ifdef USE_INJECTION_POINTS
/*
 * shmem_cache_Cache_counts returns the pin count and the usage count of the
 * frame that holds a key, or nothing when no frame does, for the tests.
 */
bool
shmem_cache_Cache_counts(const ShmemCacheCache *self, const PgMayRaise *raise, const uint8 *key, bool *has_result,
						 uint64 *result_0, uint64 *result_1, PgPgError *error)
{
	uint32		hash = shmem_cache_Cache_hash(self, key);
	ShmemCacheLocked locked;
	uint32		found;

	if (!shmem_cache_Cache_lock(self, raise, hash, false, 0, LW_SHARED, &locked, error))
		return false;
	*has_result = shmem_cache_Cache_find(self, key, hash, &found);
	if (*has_result)
	{
		*result_0 = shmem_cache_Cache_state(self, found) & SHMEM_CACHE_PINS;
		*result_1 = STD_SHR_FOLDED(u64, shmem_cache_Cache_state(self, found) & SHMEM_CACHE_USAGE, 32);
	}
	shmem_cache_Locked_drop(&locked);
	return true;
}
#endif

/*
 * shmem_cache_Cache_claim sweeps a class's clock for a frame no process pins
 * or used lately, lowering usage counts as it passes, and pins it by one
 * exchange from no pins and no usage, so no other process takes it, giving up
 * after two passes.
 */
static bool
shmem_cache_Cache_claim(const ShmemCacheCache *self, size_t class, uint32 *out_0, uint64 *out_1)
{
	ShmemCacheClass *head = shmem_cache_Cache_class(self, class);
	ShmemCachePlaced placed = shmem_cache_Cache_placed(self, class);
	uint64		passes = STD_MUL(u64, 2, (uint64) placed.count, 440, 21);

	for (uint64 i = 0, i_end = passes; i < i_end; i = std_forward_unchecked_u64_Checked(i, 1))
	{
		uint64		tick = pg_calls_atomic_add_u64(&head->hand, 1);
		uint32		tmp_1 = (uint32) STD_REM(u64, tick, (uint64) placed.count, 443, 34);
		uint32		frame = STD_ADD(u32, placed.start, tmp_1, 443, 25);
		uint64		state = shmem_cache_Cache_state(self, frame);

		if ((state & SHMEM_CACHE_PINS) != 0)
			continue;
		if ((state & SHMEM_CACHE_USAGE) != 0)
		{
			(void) shmem_cache_Cache_exchange(self, frame, state,
											  STD_SUB(u64, state, SHMEM_CACHE_USAGE_ONE, 449, 45));
			continue;
		}
		if (shmem_cache_Cache_exchange(self, frame, state, STD_ADD(u64, state, 1, 452, 44)))
		{
			*out_0 = frame;
			*out_1 = state;
			return true;
		}
	}
	return false;
}

/*
 * ShmemCacheVictim is a frame an install claimed and pins once, which it
 * unpins when it drops unless the install kept it, so no way out of an install
 * leaves its pin behind.
 */
typedef struct ShmemCacheVictim
{
	const ShmemCacheCache *cache;
	uint32		frame;
	bool		kept;
} ShmemCacheVictim;

/* shmem_cache_Victim_drop is Victim's drop: its pin given up unless the install kept it. */
static void
shmem_cache_Victim_drop(ShmemCacheVictim *self)
{
	if (!self->kept)
		shmem_cache_Cache_release(self->cache, self->frame);
}

/* shmem_cache_Cache_install_closure_0 is fits: whether a class has frames large enough for the node. */
static bool
shmem_cache_Cache_install_closure_0(const ShmemCacheCache *self, size_t len, size_t class)
{
	return shmem_cache_Cache_placed(self, class).count > 0
		&& (uint64) len <= shmem_cache_Cache_placed(self, class).size;
}

/* shmem_cache_Cache_install_frame_0_t is install's state across its pad: its arguments, its result, the victim, and the guard. */
typedef struct shmem_cache_Cache_install_frame_0_t
{
	const ShmemCacheCache *self;
	const PgMayRaise *raise;
	const uint8 *key;
	size_t		len;
	void		(*fill) (void *captures, uint8 *bytes, size_t bytes_len);
	void	   *fill_captures;
	bool	   *has_result;
	std_TupleU32RefSliceU8_t *result;
	PgPgError  *error;
	bool		done;
	ShmemCacheVictim victim;
	bool		victim_live;
	ShmemCacheLocked locked;
	bool		locked_live;
} shmem_cache_Cache_install_frame_0_t;

/* shmem_cache_Cache_install_body_0 is install's body. */
static void
shmem_cache_Cache_install_body_0(void *frame_)
{
	shmem_cache_Cache_install_frame_0_t *frame = frame_;
	const ShmemCacheCache *self = frame->self;
	size_t		class = 0;
	bool		found = false;
	ShmemCachePlaced placed;
	uint32		hash;
	uint32		passes;

	for (size_t at = 0; at < SHMEM_CACHE_CLASSES; at = std_forward_unchecked_usize_Checked(at, 1))
	{
		if (shmem_cache_Cache_install_closure_0(self, frame->len, at))
		{
			class = at;
			found = true;
			break;
		}
	}
	frame->done = true;
	*frame->has_result = false;
	if (!found)
		return;
	placed = shmem_cache_Cache_placed(self, class);
	hash = shmem_cache_Cache_hash(self, frame->key);
	passes = STD_MUL(u32, 2, placed.count, 478, 21);
	for (uint32 i = 0, i_end = passes; i < i_end; i = std_forward_unchecked_u32_Checked(i, 1))
	{
		uint32		claimed_frame;
		uint64		claimed;
		uint8		old[SHMEM_CACHE_KEY];
		bool		has_old_hash;
		uint32		old_hash = 0;
		uint64		state;
		uint8		still[SHMEM_CACHE_KEY];
		uint32		named;
		bool		has_named;

		if (!shmem_cache_Cache_claim(self, class, &claimed_frame, &claimed))
			return;
		frame->victim = (ShmemCacheVictim)
		{
			self, claimed_frame, false
		};
		frame->victim_live = true;
#ifdef USE_INJECTION_POINTS
		if (!pg_calls_injection_point(frame->raise, "doltgres-cache-claimed", frame->error))
		{
			frame->done = false;
			frame->victim_live = false;
			shmem_cache_Victim_drop(&frame->victim);
			return;
		}
#endif
		memcpy(old, shmem_cache_Cache_desc(self, claimed_frame)->key, SHMEM_CACHE_KEY);
		has_old_hash = (claimed & SHMEM_CACHE_TAGGED) != 0;
		if (has_old_hash)
			old_hash = shmem_cache_Cache_hash(self, old);
		if (!shmem_cache_Cache_lock(self, frame->raise, hash, has_old_hash, old_hash, LW_EXCLUSIVE, &frame->locked,
									frame->error))
		{
			frame->done = false;
			frame->victim_live = false;
			shmem_cache_Victim_drop(&frame->victim);
			return;
		}
		frame->locked_live = true;
		state = shmem_cache_Cache_state(self, claimed_frame);
		memcpy(still, shmem_cache_Cache_desc(self, claimed_frame)->key, SHMEM_CACHE_KEY);
		if ((state & SHMEM_CACHE_PINS) != 1 || (state & SHMEM_CACHE_TAGGED) != (claimed & SHMEM_CACHE_TAGGED)
			|| ((state & SHMEM_CACHE_TAGGED) != 0 && memcmp(still, old, SHMEM_CACHE_KEY) != 0))
		{
			frame->locked_live = false;
			shmem_cache_Locked_drop(&frame->locked);
			frame->victim_live = false;
			shmem_cache_Victim_drop(&frame->victim);
			continue;
		}
		has_named = shmem_cache_Cache_find(self, frame->key, hash, &named);
		if (has_named && named != claimed_frame)
		{
			if (!shmem_cache_Cache_pin(self, named))
			{
				frame->locked_live = false;
				shmem_cache_Locked_drop(&frame->locked);
				frame->victim_live = false;
				shmem_cache_Victim_drop(&frame->victim);
				return;
			}
			frame->locked_live = false;
			shmem_cache_Locked_drop(&frame->locked);
			frame->victim_live = false;
			shmem_cache_Victim_drop(&frame->victim);
			*frame->has_result = true;
			frame->result->f0 = named;
			frame->result->f1 = shmem_cache_Cache_bytes(self, named, &frame->result->f1_len);
			return;
		}
		if (!has_named)
		{
			ShmemCacheEntry *entry;

			if (!pg_calls_hash_enter(frame->raise, self->table, frame->key, hash, (void **) &entry, frame->error))
			{
				frame->done = false;
				frame->locked_live = false;
				shmem_cache_Locked_drop(&frame->locked);
				frame->victim_live = false;
				shmem_cache_Victim_drop(&frame->victim);
				return;
			}
			if (has_old_hash)
				pg_calls_hash_remove(self->table, old, old_hash);
			entry->frame = claimed_frame;
			memcpy(shmem_cache_Cache_desc(self, claimed_frame)->key, frame->key, SHMEM_CACHE_KEY);
		}
		shmem_cache_Cache_desc(self, claimed_frame)->len = (uint64) frame->len;
		for (;;)
		{
			uint64		now = shmem_cache_Cache_state(self, claimed_frame);

			if (shmem_cache_Cache_exchange(self, claimed_frame, now,
										   (now & ~(SHMEM_CACHE_VALID | SHMEM_CACHE_USAGE)) | SHMEM_CACHE_TAGGED |
										   SHMEM_CACHE_USAGE_ONE))
				break;
		}
		frame->locked_live = false;
		shmem_cache_Locked_drop(&frame->locked);
#ifdef USE_INJECTION_POINTS
		if (!pg_calls_injection_point(frame->raise, "doltgres-cache-retagged", frame->error))
		{
			frame->done = false;
			frame->victim_live = false;
			shmem_cache_Victim_drop(&frame->victim);
			return;
		}
#endif
		{
			uint8	   *bytes = STD_PTR_AT(self->base, shmem_cache_Placed_frame_at(&placed, claimed_frame), 1);

			std_slice_from_raw_parts_mut_check(bytes, frame->len, 1, 1, RUST_FILE,
											   542, 17);
			frame->fill(frame->fill_captures, bytes, frame->len);
		}
		frame->victim.kept = true;
		shmem_cache_Cache_add(self, claimed_frame, (int64) SHMEM_CACHE_VALID);
		*frame->has_result = true;
		frame->result->f0 = claimed_frame;
		frame->result->f1 = shmem_cache_Cache_bytes(self, claimed_frame, &frame->result->f1_len);
		frame->victim_live = false;
		shmem_cache_Victim_drop(&frame->victim);
		return;
	}
}

/* shmem_cache_Cache_install_pad_0 is install's pad: the guard, then the victim, while each is live. */
static void
shmem_cache_Cache_install_pad_0(void *frame_)
{
	shmem_cache_Cache_install_frame_0_t *frame = frame_;

	if (frame->locked_live)
		shmem_cache_Locked_drop(&frame->locked);
	if (frame->victim_live)
		shmem_cache_Victim_drop(&frame->victim);
}

/*
 * shmem_cache_Cache_install claims a frame of the smallest class that holds a
 * node of len bytes, names it by the node's key, has fill write its bytes,
 * and pins it, or pins the frame another process put the key in meanwhile, or
 * returns nothing when no frame of the class is free or the key's frame is
 * still being filled. A claimed victim is rechecked under both keys'
 * partition locks: if another process pinned it or retagged it meanwhile, it
 * is unpinned and another is claimed. A victim the table already names by the
 * key, as a fill that never finished leaves one, is filled where it is. The
 * bytes stay as they are until the frame is unpinned.
 */
bool
shmem_cache_Cache_install(const ShmemCacheCache *self, const PgMayRaise *raise, const uint8 *key, size_t len,
						  void (*fill) (void *captures, uint8 *bytes, size_t bytes_len), void *fill_captures,
						  bool *has_result, std_TupleU32RefSliceU8_t *result, PgPgError *error)
{
	shmem_cache_Cache_install_frame_0_t frame = {.self = self, .raise = raise, .key = key, .len = len, .fill = fill,
	.fill_captures = fill_captures, .has_result = has_result, .result = result, .error = error};

	std_landing(&frame, shmem_cache_Cache_install_body_0, shmem_cache_Cache_install_pad_0);
	return frame.done;
}

/*
 * shmem_cache_owner_enlarge makes room with the current resource owner for
 * one more pin and names the owner by its address, or refuses when there is
 * none.
 */
bool
shmem_cache_owner_enlarge(const PgMayRaise *raise, uint64 *result, PgPgError *error)
{
	ResourceOwner owner;

	if (!pg_calls_owner_enlarge(raise, &owner, error))
		return false;
	*result = (uint64) (uintptr_t) owner;
	return true;
}

/*
 * shmem_cache_owner_top_enlarge makes room with the top-level transaction's
 * resource owner for one more owned record and names the owner by its
 * address, or refuses outside a transaction.
 */
bool
shmem_cache_owner_top_enlarge(const PgMayRaise *raise, uint64 *result, PgPgError *error)
{
	ResourceOwner owner;

	if (!pg_calls_owner_top_enlarge(raise, &owner, error))
		return false;
	*result = (uint64) (uintptr_t) owner;
	return true;
}

/* shmem_cache_owner_remember has the resource owner shmem_cache_owner_enlarge named remember a pin, in the room it made. */
void
shmem_cache_owner_remember(uint64 owner, uint64 pin)
{
	pg_calls_owner_remember((ResourceOwner) (uintptr_t) owner, pin);
}

/* shmem_cache_owner_forget has a resource owner forget a pin it remembers and hasn't released. */
void
shmem_cache_owner_forget(uint64 owner, uint64 pin)
{
	pg_calls_owner_forget((ResourceOwner) (uintptr_t) owner, pin);
}

/*
 * shmem_cache_owned_remember has the resource owner shmem_cache_owner_enlarge
 * or shmem_cache_owner_top_enlarge named remember an owned record, in the
 * room it made.
 */
void
shmem_cache_owned_remember(uint64 owner, uint64 record)
{
	pg_calls_owned_remember((ResourceOwner) (uintptr_t) owner, record);
}

/* shmem_cache_owned_forget has a resource owner forget an owned record it remembers and hasn't released. */
void
shmem_cache_owned_forget(uint64 owner, uint64 record)
{
	pg_calls_owned_forget((ResourceOwner) (uintptr_t) owner, record);
}

#ifdef DOLTGRES_TEST_SHMEM

/*
 * test_shmem_cache_tests_a_layouts_parts_follow_one_another: the descriptors
 * start on a cache line after the header, the frames' bytes after the
 * descriptors, and the table has an entry for every frame and partition.
 */
void
test_shmem_cache_tests_a_layouts_parts_follow_one_another(void)
{
	ShmemCacheLayout layout = {{2, 1, 0, 0, 0}, {8192, 16384, 32768, 65536, 131072}};

	{
		size_t		left = (size_t) (SHMEM_CACHE_DESCS % 64);
		size_t		right = 0;

		STD_ASSERT_EQ(642, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
	}
	STD_ASSERT(643, 9, shmem_cache_Layout_bytes_at(&layout) >= (size_t) (SHMEM_CACHE_DESCS + 3 * sizeof(ShmemCacheDesc)),
			   "layout.bytes_at() >= DESCS + 3 * size_of::<Desc>()");
	{
		size_t		left = shmem_cache_Layout_size(&layout);
		size_t		tmp_1 = STD_ADD(usize, shmem_cache_Layout_bytes_at(&layout), (size_t) (2 * 8192), 644, 35);
		size_t		right = STD_ADD(usize, tmp_1, 16384, 644, 35);

		STD_ASSERT_EQ(644, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
	}
	{
		int64		left = shmem_cache_Layout_entries(&layout);
		int64		right = (int64) (3 + (int64) SHMEM_CACHE_PARTITIONS);

		STD_ASSERT_EQ(645, 9, left == right, &left, std_i64_Debug_fmt, &right, std_i64_Debug_fmt);
	}
}

/*
 * test_shmem_cache_tests_every_frames_descriptor_and_bytes_lie_in_the_cache_apart:
 * in a small cache, one of 1 MB, and one with an empty class between others,
 * every frame's descriptor ends before the frames' bytes begin, and every
 * frame's bytes lie inside the cache, each where the one before it ends, so
 * none overlaps another.
 */
void
test_shmem_cache_tests_every_frames_descriptor_and_bytes_lie_in_the_cache_apart(void)
{
	static const size_t sizes[SHMEM_CACHE_CLASSES] = {8192, 16384, 32768, 65536, 131072};
	static const size_t all[3][SHMEM_CACHE_CLASSES] = {{2, 1, 0, 0, 0}, {51, 25, 3, 0, 0}, {3, 0, 2, 0, 1}};

	for (size_t counts_at = 0, counts_index; std_array_IntoIter_next(&counts_at, 3, 3, &counts_index);)
	{
		ShmemCacheLayout layout;
		size_t		end;
		uint32		frames = 0;
		ShmemCachePlaced placed_all[SHMEM_CACHE_CLASSES];

		memcpy(layout.counts, all[counts_index], sizeof(layout.counts));
		memcpy(layout.sizes, sizes, sizeof(layout.sizes));
		end = shmem_cache_Layout_bytes_at(&layout);
		shmem_cache_Layout_placed(&layout, placed_all);
		for (size_t placed_at = 0, placed_index;
			 std_array_IntoIter_next(&placed_at, SHMEM_CACHE_CLASSES, SHMEM_CACHE_CLASSES, &placed_index);)
		{
			ShmemCachePlaced placed = placed_all[placed_index];
			uint32		last = STD_ADD(u32, placed.start, placed.count, 660, 44);

			STD_ASSERT_EQ(659, 17, placed.start == frames, &placed.start, std_u32_Debug_fmt, &frames,
						  std_u32_Debug_fmt);
			for (uint32 frame = placed.start; frame < last; frame = std_forward_unchecked_u32_Checked(frame, 1))
			{
				bool		held = shmem_cache_desc_at(frame) >= SHMEM_CACHE_DESCS;

				if (held)
					held = STD_ADD(usize, shmem_cache_desc_at(frame), sizeof(ShmemCacheDesc), 661, 56) <=
						shmem_cache_Layout_bytes_at(&layout);
				STD_ASSERT(661, 21, held,
						   "desc_at(frame) >= DESCS && desc_at(frame) + size_of::<Desc>() <= layout.bytes_at()");
				{
					size_t		left = shmem_cache_Placed_frame_at(&placed, frame);

					STD_ASSERT_EQ_MSG(662, 21, left == end, &left, std_usize_Debug_fmt, &end, std_usize_Debug_fmt,
									  "a frame's bytes don't start where the last frame's end");
				}
				end = STD_ADD(usize, end, (size_t) placed.size, 663, 21);
			}
			frames = STD_ADD(u32, frames, placed.count, 665, 17);
		}
		{
			size_t		left = (size_t) frames;
			size_t		right = shmem_cache_Layout_frames(&layout);

			STD_ASSERT_EQ(667, 13, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
		}
		{
			size_t		right = shmem_cache_Layout_size(&layout);

			STD_ASSERT_EQ_MSG(668, 13, end == right, &end, std_usize_Debug_fmt, &right, std_usize_Debug_fmt,
							  "the frames' bytes don't end where the cache does");
		}
	}
}

#endif							/* DOLTGRES_TEST_SHMEM */

#endif							/* FRONTEND */
