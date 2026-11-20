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
 * The node cache (design item 14's S5): decoded tree nodes every backend
 * shares, and a backend's own for its temporary trees, each keyed by its
 * packs' location and its address and read in place through a pin. A pin
 * lends its bytes only within the entry that took or resumed it, and what
 * crosses calls is a Kept, which lends none. The backend's pin table is the
 * one record of each pin, so whichever of its drop and its resource owner's
 * release comes first unpins it, and the other does nothing.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/cache.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/test.h"
#include "doltgres/std/vec.h"

#define RUST_FILE "host" STD_SEP "src" STD_SEP "cache.rs"

/* host_cache_PINS_t is PINS: its BackendLocal header and the process's value, which follows it. */
typedef struct host_cache_PINS_t
{
	HostLocalBackendLocal local;
	StdArcInner *process;
} host_cache_PINS_t;

/* taken at Pins and IoMutex<Pins>, declared here for the take sites below; their definitions follow Pins'. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_TAKEN_TS Pins
#define HOST_LOCK_TAKEN_T HostCachePins
#define HOST_LOCK_TAKEN_TYPE HOST_CACHE_PINS_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Pins
#define HOST_LOCK_T HostCachePins
#define HOST_LOCK_DEFAULT(place) host_cache_Pins_Default_default((place), NULL)
#include "doltgres/host/lock_template.h"

/* The items below that an earlier item reaches, declared ahead so the file keeps cache.rs's order. */
static host_cache_PINS_t HOST_CACHE_PINS;
static const StdDefaultType HOST_CACHE_ARC_TYPE;
static const HostLockType HOST_CACHE_PINS_LOCK_TYPE;
static void host_cache_ArcIoMutexPins_drop(void *value);
static void host_cache_Pins_add(HostCachePins *self, HostCachePlacement placement, uint32 frame, uint64 owner,
								uint32 *out_0, uint32 *out_1, bool *out_2);
static bool host_cache_Pins_remove(HostCachePins *self, uint32 slot, uint32 generation, HostCacheLive *result);
static bool host_cache_Pins_live(const HostCachePins *self, uint32 slot, uint32 generation);
static bool host_cache_Pins_holds(const HostCachePins *self, HostCachePlacement placement, uint32 frame);
static bool host_cache_reserve(HostIoIo *io, StdArcInner **result, HostError *error);
static HostCachePinned host_cache_pin(HostIoIo *io, HostHost host, StdArcInner *table, uint64 owner,
									  HostCachePlacement placement, HostCacheFound *found);
static uint64 host_cache_datum(uint32 slot, uint32 generation);

/* host_cache_Key_bytes returns the key's bytes, for the host that looks it up. */
const uint8 *
host_cache_Key_bytes(const HostCacheKey *self)
{
	return self->f0;
}

/*
 * host_cache_key returns a node's key: its packs' location, its address, and
 * the locator of the stored copy the node is decoded from, since the address
 * names only a chunk's hashed bytes and each stored copy has a trailer of its
 * own.
 */
HostCacheKey
host_cache_key(uint64 location, const uint8 *address, const uint8 *locator)
{
	HostCacheKey key = {{0}};
	void	   *at;
	size_t		at_len;
	void	   *rest;
	size_t		rest_len;
	void	   *of;
	size_t		of_len;
	void	   *stored;
	size_t		stored_len;
	uint8		bytes[sizeof(uint64)];

	std_slice_split_at_mut(key.f0, HOST_CACHE_KEY, sizeof(uint64), 1, 1, &at, &at_len, &rest, &rest_len, RUST_FILE,
						   54, 26);
	std_slice_split_at_mut(rest, rest_len, HOST_CACHE_ADDRESS, 1, 1, &of, &of_len, &stored, &stored_len, RUST_FILE,
						   55, 29);
	std_u64_to_le_bytes(location, bytes);
	std_slice_copy_from_slice_Checked(at, at_len, bytes, sizeof(bytes), 1, 1, RUST_FILE, 56, 8);
	std_slice_copy_from_slice_Checked(of, of_len, address, HOST_CACHE_ADDRESS, 1, 1, RUST_FILE, 57, 8);
	std_slice_copy_from_slice_Checked(stored, stored_len, locator, HOST_CACHE_LOCATOR, 1, 1, RUST_FILE, 58, 12);
	return key;
}

/*
 * HOST_CACHE_CLASSES are a node cache's frame sizes, smallest first, each
 * with its share of the cache's bytes in hundredths: a node takes the
 * smallest frame that holds it, and most nodes are near trees' 8 KiB target.
 * Tuning the shares is a performance-stage item.
 */
const host_cache_CLASSES_t HOST_CACHE_CLASSES[HOST_CACHE_CLASSES_LEN] = {
	{8 << 10, 40}, {16 << 10, 40}, {32 << 10, 12}, {64 << 10, 6}, {128 << 10, 2},
};

/*
 * host_cache_frames returns how many frames of each class a cache of a size
 * holds, by core's [T; N]::map as host's instance runs it: try_map's Drain
 * reads each class, try_from_fn_erased's Guard pushes each result with its
 * get_unchecked_mut and unchecked_add preconditions, and on an unwind the
 * Guard's drop takes its initialized range and the Drain's drop its
 * remaining length, each with its precondition.
 */
void
host_cache_frames(size_t bytes, size_t *result)
{
	volatile size_t drained = 0;
	volatile size_t initialized = 0;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		while (initialized < HOST_CACHE_CLASSES_LEN)
		{
			size_t		size = HOST_CACHE_CLASSES[drained].f0;
			size_t		share = HOST_CACHE_CLASSES[drained].f1;
			size_t		tmp_1;
			size_t		tmp_2;
			size_t		item;

			drained = drained + 1;
			tmp_1 = STD_DIV_FOLDED(usize, bytes, 100);
			tmp_2 = STD_MUL(usize, tmp_1, share, 69, 33);
			item = STD_DIV(usize, tmp_2, size, 69, 33);
			std_slice_get_unchecked_mut_check(initialized, HOST_CACHE_CLASSES_LEN, STD_ARRAY_MOD_FILE, 954, 28);
			result[initialized] = item;
			std_usize_unchecked_add_check(initialized, 1, STD_ARRAY_MOD_FILE, 955, 49);
			initialized = initialized + 1;
		}
		std_unwind_pop(&unwind);
		std_array_Drain_drop(&HOST_CACHE_CLASSES[drained], &HOST_CACHE_CLASSES[HOST_CACHE_CLASSES_LEN],
							 sizeof(HOST_CACHE_CLASSES[0]));
	}
	else
	{
		std_array_Guard_drop(initialized, HOST_CACHE_CLASSES_LEN);
		std_array_Drain_drop(&HOST_CACHE_CLASSES[drained], &HOST_CACHE_CLASSES[HOST_CACHE_CLASSES_LEN],
							 sizeof(HOST_CACHE_CLASSES[0]));
		std_unwind_resume(&unwind);
	}
}

/* host_cache_class returns the class of the smallest frame that holds a node's bytes, or None for bytes no frame holds. */
bool
host_cache_class(size_t len, size_t *result)
{
	for (size_t i = 0; i < HOST_CACHE_CLASSES_LEN; i++)
	{
		size_t		size = HOST_CACHE_CLASSES[i].f0;

		if (len <= size)
		{
			*result = i;
			return true;
		}
	}
	return false;
}

/* host_cache_Bytes_Deref_deref is a cached node's bytes, mapped or lent. */
StdBytes
host_cache_Bytes_Deref_deref(const HostCacheBytes *self)
{
	switch (self->tag)
	{
		case HOST_CACHE_BYTES_MAPPED:
			return (StdBytes)
			{
				self->u.mapped.f0, self->u.mapped.f0_len
			};
		case HOST_CACHE_BYTES_LENT:
			return (StdBytes)
			{
				STD_ARC_VALUE(self->u.lent.f0, 1), self->u.lent.f0_len
			};
	}
	pg_unreachable();
}

/* host_cache_Bytes_drop is Bytes' drop glue: a lent frame's Arc's. */
void
host_cache_Bytes_drop(HostCacheBytes *self)
{
	if (self->tag == HOST_CACHE_BYTES_LENT)
		std_arc_drop(self->u.lent.f0, self->u.lent.f0_len, 1, NULL);
}

/* host_cache_Pinned_Deref_deref is the pinned node's bytes. */
StdBytes
host_cache_Pinned_Deref_deref(const HostCachePinned *self)
{
	return host_cache_Bytes_Deref_deref(&self->bytes);
}

/*
 * host_cache_Pinned_keep gives the pin to state that lives across calls, as
 * a cursor keeps its node's between fetches; it takes the pinned node.
 */
HostCacheKept
host_cache_Pinned_keep(HostCachePinned *self)
{
	return (HostCacheKept)
	{
		self->pin, self->bytes
	};
}

/*
 * host_cache_Pinned_drop is Pinned's drop glue: its pin's, then its bytes',
 * which a panic in the pin's drop still drops.
 */
void
host_cache_Pinned_drop(HostCachePinned *self)
{
	volatile bool bytes_live = true;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		host_cache_Pin_drop(&self->pin);
		bytes_live = false;
		host_cache_Bytes_drop(&self->bytes);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (bytes_live)
			host_cache_Bytes_drop(&self->bytes);
		std_unwind_resume(&unwind);
	}
}

/*
 * host_cache_Kept_resume returns the pinned node in the running entry, or
 * None once the pin's resource owner released it, when its node is read
 * again by its key; it takes the kept pin, which it drops when it returns
 * None.
 */
bool
host_cache_Kept_resume(HostCacheKept *self, HostIoIo *io, HostCachePinned *result)
{
	volatile bool self_live = true;
	volatile HostLockLockedRwLockWriteGuard tmp_1;
	volatile bool tmp_1_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostIoHeld *held;
		StdAny		raise;
		const HostCachePins *pins;
		bool		live;

		host_io_Io_Sealed_parts(io, &held, &raise);
		tmp_1 = host_lock_IoMutex_lock((HostLockIoMutex *) STD_ARC_VALUE(self->pin.table, _Alignof(HostLockIoMutexPins)),
									   &HOST_CACHE_PINS_LOCK_TYPE, held);
		tmp_1_live = true;
		pins = host_lock_LockedRwLockWriteGuard_Deref_deref((HostLockLockedRwLockWriteGuard *) &tmp_1);
		live = host_cache_Pins_live(pins, self->pin.slot, self->pin.generation);
		tmp_1_live = false;
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_1);
		if (!live)
		{
			self_live = false;
			host_cache_Kept_drop(self);
			std_unwind_pop(&unwind);
			return false;
		}
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_1_live)
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_1);
		if (self_live)
			host_cache_Kept_drop(self);
		std_unwind_resume(&unwind);
	}
	result->pin = self->pin;
	result->bytes = self->bytes;
	return true;
}

/*
 * host_cache_Kept_drop is Kept's drop glue: its pin's, then its bytes',
 * which a panic in the pin's drop still drops.
 */
void
host_cache_Kept_drop(HostCacheKept *self)
{
	volatile bool bytes_live = true;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		host_cache_Pin_drop(&self->pin);
		bytes_live = false;
		host_cache_Bytes_drop(&self->bytes);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (bytes_live)
			host_cache_Bytes_drop(&self->bytes);
		std_unwind_resume(&unwind);
	}
}

/*
 * host_cache_Pin_drop is Pin's drop glue: its drop, which unpins the frame
 * unless the resource owner's release came first, then its Arc of the table,
 * which a panic in the drop still drops.
 */
void
host_cache_Pin_drop(HostCachePin *self)
{
	HostIoInDrop dropping;
	volatile StdRwLockWriteGuard pins;
	volatile bool pins_live = false;
	volatile bool table_live = true;
	HostCacheLive live;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		dropping = host_io_InDrop_new();
		if (host_lock_IoMutex_lock_in_drop((HostLockIoMutex *) STD_ARC_VALUE(self->table, _Alignof(HostLockIoMutexPins)), &dropping,
										   (StdRwLockWriteGuard *) &pins))
		{
			HostCachePins *value;

			pins_live = true;
			value = (HostCachePins *) pins.data;
			if (host_cache_Pins_remove(value, self->slot, self->generation, &live))
			{
				HostHost	host = self->host;
				uint64		owner = live.owner;
				uint64		datum = host_cache_datum(self->slot, self->generation);

				host.ops->owner_forget(host.self, owner, datum);
				if (!host_cache_Pins_holds(value, live.placement, live.frame))
					self->host.ops->cache_unpin(self->host.self, live.placement, live.frame);
			}
			pins_live = false;
			std_RwLockWriteGuard_drop((StdRwLockWriteGuard *) &pins);
		}
		table_live = false;
		host_cache_ArcIoMutexPins_drop(&self->table);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (pins_live)
			std_RwLockWriteGuard_drop((StdRwLockWriteGuard *) &pins);
		if (table_live)
			host_cache_ArcIoMutexPins_drop(&self->table);
		std_unwind_resume(&unwind);
	}
}

/* host_cache_Read_Deref_deref is the node's bytes, cached or its own. */
StdBytes
host_cache_Read_Deref_deref(const HostCacheRead *self)
{
	switch (self->tag)
	{
		case HOST_CACHE_READ_CACHED:
			return host_cache_Pinned_Deref_deref(&self->u.cached);
		case HOST_CACHE_READ_UNCACHED:
			return (StdBytes)
			{
				self->u.uncached.ptr, self->u.uncached.len
			};
	}
	pg_unreachable();
}

/* host_cache_Read_drop is Read's drop glue: the pinned node's, or the node's own bytes. */
void
host_cache_Read_drop(HostCacheRead *self)
{
	switch (self->tag)
	{
		case HOST_CACHE_READ_CACHED:
			host_cache_Pinned_drop(&self->u.cached);
			break;
		case HOST_CACHE_READ_UNCACHED:
			std_vec_free_Checked(&self->u.uncached, 1, 1);
			break;
	}
}

/* host_cache_get returns the cached node a key names, pinned, or None when no frame holds it whole. */
bool
host_cache_get(HostIoIo *io, HostCachePlacement placement, const HostCacheKey *key,
			   bool *has_result, HostCachePinned *result, HostError *error)
{
	HostHost	host = host_current();
	uint64		owner;
	StdArcInner *table_0;
	StdArcInner *volatile table;
	bool		has_found;
	HostCacheFound found;
	StdUnwind	unwind;

	{
		HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

		if (!host.ops->owner_enlarge(host.self, &raise, &owner, error))
			return false;
	}
	if (!host_cache_reserve(io, &table_0, error))
		return false;
	table = table_0;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));
		StdArcInner *dropped;

		if (!host.ops->cache_get(host.self, &raise, placement, key, &has_found, &found, error))
		{
			dropped = table;
			table = NULL;
			host_cache_ArcIoMutexPins_drop(&dropped);
			std_unwind_pop(&unwind);
			return false;
		}
		if (!has_found)
		{
			*has_result = false;
			dropped = table;
			table = NULL;
			host_cache_ArcIoMutexPins_drop(&dropped);
			std_unwind_pop(&unwind);
			return true;
		}
		std_unwind_pop(&unwind);
	}
	else
	{
		if (table != NULL)
		{
			StdArcInner *dropped = table;

			host_cache_ArcIoMutexPins_drop(&dropped);
		}
		std_unwind_resume(&unwind);
	}
	*result = host_cache_pin(io, host, table, owner, placement, &found);
	*has_result = true;
	return true;
}

/*
 * host_cache_install puts a node of len bytes in its cache under its key and
 * returns it pinned there, or pinned where another backend put it meanwhile,
 * or in memory of its own when every frame that could take it is in use.
 * fill writes every one of the node's bytes, which hold anything before it
 * does, once, unless another backend's frame is returned; install takes fill
 * and drops it when it returns.
 */
bool
host_cache_install(HostIoIo *io, HostCachePlacement placement, const HostCacheKey *key,
				   size_t len, void (*fill) (void *, uint8 *, size_t), void *fill_captures,
				   void (*fill_drop) (void *), HostCacheRead *result, HostError *error)
{
	HostHost	host;
	uint64		owner;
	StdArcInner *table_0;
	StdArcInner *volatile table = NULL;
	bool		has_found;
	HostCacheFound found;
	volatile StdVec bytes;
	volatile bool bytes_live = false;
	volatile HostCacheRead read;
	volatile bool read_live = false;
	volatile bool fill_live = true;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostMayRaise raise;
		StdArcInner *dropped;

		host = host_current();
		raise = host_doors(HOST_IO_IO_READ_DOORS(io));
		if (!host.ops->owner_enlarge(host.self, &raise, &owner, error))
		{
			fill_live = false;
			if (fill_drop != NULL)
				fill_drop(fill_captures);
			std_unwind_pop(&unwind);
			return false;
		}
		if (!host_cache_reserve(io, &table_0, error))
		{
			fill_live = false;
			if (fill_drop != NULL)
				fill_drop(fill_captures);
			std_unwind_pop(&unwind);
			return false;
		}
		table = table_0;
		raise = host_doors(HOST_IO_IO_READ_DOORS(io));
		if (!host.ops->cache_install(host.self, &raise, placement, key, len, fill, fill_captures,
									 &has_found, &found, error))
		{
			dropped = table;
			table = NULL;
			host_cache_ArcIoMutexPins_drop(&dropped);
			fill_live = false;
			if (fill_drop != NULL)
				fill_drop(fill_captures);
			std_unwind_pop(&unwind);
			return false;
		}
		if (has_found)
		{
			StdArcInner *moved = table;
			HostCachePinned pinned;

			table = NULL;
			pinned = host_cache_pin(io, host, moved, owner, placement, &found);
			((HostCacheRead *) &read)->tag = HOST_CACHE_READ_CACHED;
			((HostCacheRead *) &read)->u.cached = pinned;
			read_live = true;
		}
		else
		{
			std_vec_from_elem_zeroed((StdVec *) &bytes, len, 1, 1);
			bytes_live = true;
			fill(fill_captures, ((StdVec *) &bytes)->ptr, ((StdVec *) &bytes)->len);
			bytes_live = false;
			((HostCacheRead *) &read)->tag = HOST_CACHE_READ_UNCACHED;
			((HostCacheRead *) &read)->u.uncached = *(StdVec *) &bytes;
			read_live = true;
			dropped = table;
			table = NULL;
			host_cache_ArcIoMutexPins_drop(&dropped);
		}
		fill_live = false;
		if (fill_drop != NULL)
			fill_drop(fill_captures);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (read_live)
			host_cache_Read_drop((HostCacheRead *) &read);
		if (bytes_live)
			std_vec_free_Checked((StdVec *) &bytes, 1, 1);
		if (table != NULL)
		{
			StdArcInner *dropped = table;

			host_cache_ArcIoMutexPins_drop(&dropped);
		}
		if (fill_live && fill_drop != NULL)
			fill_drop(fill_captures);
		std_unwind_resume(&unwind);
	}
	*result = *(HostCacheRead *) &read;
	return true;
}

/*
 * host_cache_contains reports whether a cache's table names a key, whether or
 * not its frame is filled yet, for a reader deciding whether to ask the
 * operating system for a node ahead of reading it. It pins nothing and counts
 * as no use, so a frame only asked about goes to the clock as one never
 * touched.
 */
bool
host_cache_contains(HostIoIo *io, HostCachePlacement placement, const HostCacheKey *key,
					bool *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

	return host.ops->cache_contains(host.self, &raise, placement, key, result, error);
}

/*
 * host_cache_make_local fixes the size of the backend's own cache, as its
 * first write of a temporary tree does, Postgres' InitLocalBuffers; until then
 * the backend caches no temporary node.
 */
bool
host_cache_make_local(HostIoIoDoors io, size_t bytes, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->cache_make_local(host.self, &raise, bytes, error);
}

/*
 * host_cache_released unpins the pin a resource owner remembered, unless its
 * drop already did; it is the owner's release, which runs inside an abort, so
 * it neither allocates nor raises.
 */
void
host_cache_released(uint64 remembered)
{
	uint32		slot = (uint32) (remembered & (uint64) PG_UINT32_MAX);
	uint32		generation = (uint32) STD_SHR_FOLDED(u64, remembered, 32);
	volatile HostLocalLocal table;
	volatile bool table_live;
	HostIoInDrop dropping;
	volatile StdRwLockWriteGuard pins;
	volatile bool pins_live = false;
	StdUnwind	unwind;

	if (!host_local_BackendLocal_existing(&HOST_CACHE_PINS.local, (HostLocalLocal *) &table))
		return;
	table_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdArcInner *arc;
		HostCachePins *value;
		HostCacheLive live;

		dropping = host_io_InDrop_new();
		arc = *(StdArcInner *const *) host_local_Local_Deref_deref((HostLocalLocal *) &table, &HOST_CACHE_ARC_TYPE);
		if (!host_lock_IoMutex_lock_in_drop((HostLockIoMutex *) STD_ARC_VALUE(arc, _Alignof(HostLockIoMutexPins)), &dropping,
											(StdRwLockWriteGuard *) &pins))
		{
			table_live = false;
			host_local_Local_drop((HostLocalLocal *) &table, &HOST_CACHE_ARC_TYPE);
			std_unwind_pop(&unwind);
			return;
		}
		pins_live = true;
		value = (HostCachePins *) pins.data;
		if (value->has_host && host_cache_Pins_remove(value, slot, generation, &live))
		{
			HostHost	host = value->host;

			if (!host_cache_Pins_holds(value, live.placement, live.frame))
				host.ops->cache_unpin(host.self, live.placement, live.frame);
		}
		pins_live = false;
		std_RwLockWriteGuard_drop((StdRwLockWriteGuard *) &pins);
		table_live = false;
		host_local_Local_drop((HostLocalLocal *) &table, &HOST_CACHE_ARC_TYPE);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (pins_live)
			std_RwLockWriteGuard_drop((StdRwLockWriteGuard *) &pins);
		if (table_live)
			host_local_Local_drop((HostLocalLocal *) &table, &HOST_CACHE_ARC_TYPE);
		std_unwind_resume(&unwind);
	}
}

/*
 * host_cache_release_all unpins every pin the backend still holds, for its
 * exit, which runs after its resource owners' release has unpinned what they
 * remembered.
 */
void
host_cache_release_all(void)
{
	volatile HostLocalLocal table;
	volatile bool table_live;
	HostIoInDrop dropping;
	volatile StdRwLockWriteGuard pins;
	volatile bool pins_live = false;
	StdUnwind	unwind;

	if (!host_local_BackendLocal_existing(&HOST_CACHE_PINS.local, (HostLocalLocal *) &table))
		return;
	table_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdArcInner *arc;
		HostCachePins *value;

		dropping = host_io_InDrop_new();
		arc = *(StdArcInner *const *) host_local_Local_Deref_deref((HostLocalLocal *) &table, &HOST_CACHE_ARC_TYPE);
		if (!host_lock_IoMutex_lock_in_drop((HostLockIoMutex *) STD_ARC_VALUE(arc, _Alignof(HostLockIoMutexPins)), &dropping,
											(StdRwLockWriteGuard *) &pins))
		{
			table_live = false;
			host_local_Local_drop((HostLocalLocal *) &table, &HOST_CACHE_ARC_TYPE);
			std_unwind_pop(&unwind);
			return;
		}
		pins_live = true;
		value = (HostCachePins *) pins.data;
		if (value->has_host)
		{
			HostHost	host = value->host;

			for (size_t slot = 0, slot_len = value->entries.len; slot < slot_len; slot++)
			{
				HostCacheEntry *entry = &STD_VEC_AT(&value->entries, HostCacheEntry, slot);
				HostCacheLive live;

				if (!entry->has_live)
					continue;
				entry->has_live = false;
				live = entry->live;
				if (!host_cache_Pins_holds(value, live.placement, live.frame))
					host.ops->cache_unpin(host.self, live.placement, live.frame);
			}
		}
		pins_live = false;
		std_RwLockWriteGuard_drop((StdRwLockWriteGuard *) &pins);
		table_live = false;
		host_local_Local_drop((HostLocalLocal *) &table, &HOST_CACHE_ARC_TYPE);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (pins_live)
			std_RwLockWriteGuard_drop((StdRwLockWriteGuard *) &pins);
		if (table_live)
			host_local_Local_drop((HostLocalLocal *) &table, &HOST_CACHE_ARC_TYPE);
		std_unwind_resume(&unwind);
	}
}

#ifdef USE_INJECTION_POINTS
/*
 * host_cache_unpin_number has the host unpin a frame by a number no pin gave,
 * for the fork's test that a number past a cache's frames changes nothing.
 */
void
host_cache_unpin_number(HostCachePlacement placement, uint32 frame)
{
	HostHost	host = host_current();

	host.ops->cache_unpin(host.self, placement, frame);
}
#endif

/* HOST_CACHE_PINS is the backend's pin table, shared with each of its pins. */
static host_cache_PINS_t HOST_CACHE_PINS = {HOST_LOCAL_BACKEND_LOCAL_NEW, NULL};

/* host_cache_Pins_Default_default is Pins' derived Default: no entries and no host. */
static void
host_cache_Pins_Default_default(void *result, const StdLocation *caller)
{
	HostCachePins *pins = (HostCachePins *) result;

	std_vec_new(&pins->entries, sizeof(HostCacheEntry), _Alignof(HostCacheEntry));
	pins->has_host = false;
}

/* host_cache_Pins_drop is Pins' drop glue: its entries. */
static void
host_cache_Pins_drop(void *value)
{
	std_vec_free_Checked(&((HostCachePins *) value)->entries, sizeof(HostCacheEntry), _Alignof(HostCacheEntry));
}

/* taken at Pins, its table HOST_CACHE_PINS_LOCK_TYPE, and IoMutex<Pins> with its new and Default. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Pins
#define HOST_LOCK_TAKEN_T HostCachePins
#define HOST_LOCK_TAKEN_DEFAULT(place) host_cache_Pins_Default_default((place), NULL)
#define HOST_LOCK_TAKEN_DROP(place) host_cache_Pins_drop(place)
#define HOST_LOCK_TAKEN_TYPE HOST_CACHE_PINS_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Pins
#define HOST_LOCK_T HostCachePins
#define HOST_LOCK_DEFAULT(place) host_cache_Pins_Default_default((place), NULL)
#include "doltgres/host/lock_template.h"

/* host_cache_IoMutexPins_drop is IoMutex<Pins>' drop glue: its value's. */
static void
host_cache_IoMutexPins_drop(void *value)
{
	host_cache_Pins_drop(HOST_LOCK_VALUE((HostLockIoMutex *) value));
}

/* HOST_CACHE_MUTEX_TYPE is IoMutex<Pins>' size, Default, and drop. */
static const StdDefaultType HOST_CACHE_MUTEX_TYPE = {
	sizeof(HostLockIoMutexPins), _Alignof(HostLockIoMutexPins), host_lock_IoMutexPins_Default_default, host_cache_IoMutexPins_drop,
	&STD_ARC_DEFAULT_SITE
};

/* host_cache_ArcIoMutexPins_Default_default is Arc<IoMutex<Pins>>' Default. */
static void
host_cache_ArcIoMutexPins_Default_default(void *result, const StdLocation *caller)
{
	*(StdArcInner **) result = std_arc_default(&HOST_CACHE_MUTEX_TYPE);
}

/* host_cache_ArcIoMutexPins_drop is Arc<IoMutex<Pins>>' drop. */
static void
host_cache_ArcIoMutexPins_drop(void *value)
{
	std_arc_drop(*(StdArcInner **) value, HOST_CACHE_MUTEX_TYPE.size, HOST_CACHE_MUTEX_TYPE.align, HOST_CACHE_MUTEX_TYPE.drop);
}

/* HOST_CACHE_ARC_TYPE is Arc<IoMutex<Pins>>' size, Default, and drop. */
static const StdDefaultType HOST_CACHE_ARC_TYPE = {sizeof(StdArcInner *), _Alignof(StdArcInner *), host_cache_ArcIoMutexPins_Default_default,
host_cache_ArcIoMutexPins_drop};

/*
 * host_cache_Pins_add records a pin, in the room reserve made, and returns
 * its slot and generation, and whether the backend already pinned its frame.
 */
static void
host_cache_Pins_add(HostCachePins *self, HostCachePlacement placement, uint32 frame, uint64 owner,
					uint32 *out_0, uint32 *out_1, bool *out_2)
{
	bool		already = host_cache_Pins_holds(self, placement, frame);
	bool		found = false;
	size_t		slot = 0;
	HostCacheEntry *entry;

	for (size_t i = 0, i_len = self->entries.len; i < i_len; i++)
	{
		if (!STD_VEC_AT(&self->entries, HostCacheEntry, i).has_live)
		{
			found = true;
			slot = i;
			break;
		}
	}
	if (!found)
	{
		HostCacheEntry fresh = {0};

		std_vec_push_Checked(&self->entries, &fresh, sizeof(HostCacheEntry), _Alignof(HostCacheEntry));
		slot = STD_SUB(usize, self->entries.len, 1, 315, 17);
	}
	std_index_check(slot, self->entries.len, RUST_FILE, 318, 38);
	entry = &STD_VEC_AT(&self->entries, HostCacheEntry, slot);
	entry->generation = std_wrapping_add_u32(entry->generation, 1);
	entry->has_live = true;
	entry->live = (HostCacheLive)
	{
		placement, frame, owner
	};
	*out_0 = (uint32) slot;
	*out_1 = entry->generation;
	*out_2 = already;
}

/* host_cache_Pins_remove removes a live pin's entry and returns it, or nothing when the entry is gone or its slot reused. */
static bool
host_cache_Pins_remove(HostCachePins *self, uint32 slot, uint32 generation, HostCacheLive *result)
{
	HostCacheEntry *entry;

	if (!((size_t) slot < self->entries.len))
		return false;
	entry = &STD_VEC_AT(&self->entries, HostCacheEntry, slot);
	if (entry->generation != generation)
		return false;
	if (!entry->has_live)
		return false;
	entry->has_live = false;
	*result = entry->live;
	return true;
}

/* host_cache_Pins_live reports whether a pin's entry is still in the table. */
static bool
host_cache_Pins_live(const HostCachePins *self, uint32 slot, uint32 generation)
{
	const HostCacheEntry *entry;

	if (!((size_t) slot < self->entries.len))
		return false;
	entry = &STD_VEC_AT(&self->entries, HostCacheEntry, slot);
	return entry->generation == generation && entry->has_live;
}

/* host_cache_Pins_holds reports whether any of the backend's pins is of a frame. */
static bool
host_cache_Pins_holds(const HostCachePins *self, HostCachePlacement placement, uint32 frame)
{
	for (size_t i = 0, i_len = self->entries.len; i < i_len; i++)
	{
		const HostCacheEntry *entry = &STD_VEC_AT(&self->entries, HostCacheEntry, i);

		if (entry->has_live && entry->live.placement == placement && entry->live.frame == frame)
			return true;
	}
	return false;
}

/*
 * host_cache_reserve returns the backend's pin table with room for one more
 * pin, before its frame is pinned, so the pin is recorded without failing
 * once it is taken; a failed allocation leaves the table as it was.
 */
static bool
host_cache_reserve(HostIoIo *io, StdArcInner **result, HostError *error)
{
	StdArcInner *volatile table = NULL;
	volatile HostLockLockedRwLockWriteGuard tmp_2;
	volatile bool tmp_2_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostLocalLocal tmp_1;
		HostIoHeld *held;
		StdAny		raise;
		HostCachePins *pins;
		bool		grown;

		tmp_1 = host_local_BackendLocal_get(&HOST_CACHE_PINS.local, &HOST_CACHE_ARC_TYPE);
		table = std_arc_clone(*(StdArcInner *const *) host_local_Local_Deref_deref(&tmp_1, &HOST_CACHE_ARC_TYPE));
		host_local_Local_drop(&tmp_1, &HOST_CACHE_ARC_TYPE);
		host_io_Io_Sealed_parts(io, &held, &raise);
		tmp_2 = host_lock_IoMutex_lock((HostLockIoMutex *) STD_ARC_VALUE(table, _Alignof(HostLockIoMutexPins)), &HOST_CACHE_PINS_LOCK_TYPE,
									   held);
		tmp_2_live = true;
		pins = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut((HostLockLockedRwLockWriteGuard *) &tmp_2);
		grown = std_vec_try_reserve(&pins->entries, 1, sizeof(HostCacheEntry), _Alignof(HostCacheEntry));
		tmp_2_live = false;
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_2);
		if (!grown)
		{
			StdArcInner *dropped = table;

			*error = host_Error_out_of_memory();
			table = NULL;
			host_cache_ArcIoMutexPins_drop(&dropped);
			std_unwind_pop(&unwind);
			return false;
		}
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_2_live)
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_2);
		if (table != NULL)
		{
			StdArcInner *dropped = table;

			host_cache_ArcIoMutexPins_drop(&dropped);
		}
		std_unwind_resume(&unwind);
	}
	*result = table;
	return true;
}

/*
 * host_cache_pin records a frame the host pinned in the backend's table and
 * with the resource owner that has room for it, giving back the shared pin
 * when the backend already held the frame; it takes the table and the found
 * frame.
 */
static HostCachePinned
host_cache_pin(HostIoIo *io, HostHost host, StdArcInner *table, uint64 owner,
			   HostCachePlacement placement, HostCacheFound *found)
{
	uint32		slot;
	uint32		generation;
	bool		already;
	volatile HostLockLockedRwLockWriteGuard pins;
	volatile bool pins_live = false;
	HostCachePinned result;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostIoHeld *held;
		StdAny		raise;
		HostCachePins *target;
		HostCachePins *value;
		uint64		datum;

		host_io_Io_Sealed_parts(io, &held, &raise);
		pins = host_lock_IoMutex_lock((HostLockIoMutex *) STD_ARC_VALUE(table, _Alignof(HostLockIoMutexPins)), &HOST_CACHE_PINS_LOCK_TYPE,
									  held);
		pins_live = true;
		target = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut((HostLockLockedRwLockWriteGuard *) &pins);
		target->has_host = true;
		target->host = host;
		value = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut((HostLockLockedRwLockWriteGuard *) &pins);
		host_cache_Pins_add(value, placement, found->frame, owner, &slot, &generation, &already);
		pins_live = false;
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &pins);
		if (already)
			host.ops->cache_unpin(host.self, placement, found->frame);
		datum = host_cache_datum(slot, generation);
		host.ops->owner_remember(host.self, owner, datum);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (pins_live)
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &pins);
		host_cache_Bytes_drop(&found->bytes);
		host_cache_ArcIoMutexPins_drop(&table);
		std_unwind_resume(&unwind);
	}
	result.pin.host = host;
	result.pin.table = table;
	result.pin.slot = slot;
	result.pin.generation = generation;
	result.bytes = found->bytes;
	return result;
}

/* host_cache_datum is what a resource owner remembers of a pin: its slot and generation. */
static uint64
host_cache_datum(uint32 slot, uint32 generation)
{
	uint64		tmp_1 = (uint64) slot;
	uint64		tmp_2 = STD_SHL_FOLDED(u64, (uint64) generation, 32);

	return tmp_1 | tmp_2;
}

#ifdef DOLTGRES_TEST_HOST

/* host_cache_tests_ArrayKEYU8_Debug_fmt is the Debug of a key's bytes, an array of KEY bytes. */
static bool
host_cache_tests_ArrayKEYU8_Debug_fmt(const void *value, StdFormatter *f)
{
	StdBytes	bytes = {(const uint8 *) value, HOST_CACHE_KEY};

	return std_bytes_Debug_fmt(&bytes, f);
}

/* TESTS_ASSERT_KEYS_NE is assert_ne! of two keys' bytes at a line and column. */
#define TESTS_ASSERT_KEYS_NE(line, col, a, b) \
	do { \
		HostCacheKey a_ = (a); \
		HostCacheKey b_ = (b); \
		if (memcmp(host_cache_Key_bytes(&a_), host_cache_Key_bytes(&b_), HOST_CACHE_KEY) == 0) \
			std_assert_failed(RUST_FILE, (line), (col), "!=", host_cache_Key_bytes(&a_), host_cache_tests_ArrayKEYU8_Debug_fmt, \
							  host_cache_Key_bytes(&b_), host_cache_tests_ArrayKEYU8_Debug_fmt, NULL); \
	} while (0)

/*
 * test_host_cache_tests_a_key_holds_its_location_its_whole_address_and_its_whole_locator:
 * a key holds its packs' location, its whole address, and its whole locator:
 * two addresses that differ only in their last byte, one address at two
 * locations, and one address stored at two locators that differ only in
 * their last byte, have different keys.
 */
void
test_host_cache_tests_a_key_holds_its_location_its_whole_address_and_its_whole_locator(void)
{
	uint8		one[HOST_CACHE_ADDRESS];
	uint8		other[HOST_CACHE_ADDRESS];
	uint8		stored[HOST_CACHE_LOCATOR];
	uint8		copy[HOST_CACHE_LOCATOR];

	memset(one, 7, sizeof(one));
	memset(other, 7, sizeof(other));
	other[HOST_CACHE_ADDRESS - 1] = 8;
	memset(stored, 3, sizeof(stored));
	memset(copy, 3, sizeof(copy));
	copy[HOST_CACHE_LOCATOR - 1] = 4;
	TESTS_ASSERT_KEYS_NE(399, 9, host_cache_key(1, one, stored), host_cache_key(1, other, stored));
	TESTS_ASSERT_KEYS_NE(400, 9, host_cache_key(1, one, stored), host_cache_key(2, one, stored));
	TESTS_ASSERT_KEYS_NE(401, 9, host_cache_key(1, one, stored), host_cache_key(1, one, copy));
	{
		HostCacheKey key = host_cache_key(1, one, stored);
		StdBytes	left;
		StdBytes	right = {one, HOST_CACHE_ADDRESS};

		std_range_check(8, HOST_CACHE_KEY - HOST_CACHE_LOCATOR, HOST_CACHE_KEY, RUST_FILE, 402, 49);
		left = (StdBytes)
		{
			host_cache_Key_bytes(&key) + 8, HOST_CACHE_KEY - HOST_CACHE_LOCATOR - 8
		};
		STD_ASSERT_EQ(402, 9, left.len == right.len && memcmp(left.ptr, right.ptr, left.len) == 0, &left,
					  std_bytes_Debug_fmt, &right, std_bytes_Debug_fmt);
	}
	{
		HostCacheKey key = host_cache_key(1, one, stored);
		StdBytes	left;
		StdBytes	right = {stored, HOST_CACHE_LOCATOR};

		std_range_from_check(HOST_CACHE_KEY - HOST_CACHE_LOCATOR, HOST_CACHE_KEY, RUST_FILE, 403, 49);
		left = (StdBytes)
		{
			host_cache_Key_bytes(&key) + HOST_CACHE_KEY - HOST_CACHE_LOCATOR, HOST_CACHE_LOCATOR
		};
		STD_ASSERT_EQ(403, 9, left.len == right.len && memcmp(left.ptr, right.ptr, left.len) == 0, &left,
					  std_bytes_Debug_fmt, &right, std_bytes_Debug_fmt);
	}
}

#endif							/* DOLTGRES_TEST_HOST */
