/*-------------------------------------------------------------------------
 *
 * dg_node_cache.c
 *	  Prolly nodes shared by every backend, read in place.
 *
 * The cache is laid out like the buffer pool: a fixed array of slots, each
 * naming one node, and a partitioned hash table mapping node hashes to slots.
 * A slot's state word holds its pin count, a usage count and its flags. Node
 * bytes vary in size, so they live in a dynamic shared memory area created in
 * place in the main shared memory segment and capped at dolt_node_cache_size;
 * the cache never asks for dynamic segments.
 *
 * Borrowing a node pins its slot under the shared partition lock, so eviction,
 * which takes the exclusive lock and requires a zero pin count, never frees
 * bytes in use. A borrower reads the bytes without any lock until it returns
 * them. Adding a node claims a slot by clock sweep, evicting unpinned slots
 * whose usage has run down, and evicts more when the area lacks room for the
 * bytes. A cache full of pinned nodes refuses additions; callers then keep a
 * private copy.
 *
 * Callers must return every loan: pins are not released at transaction end.
 *
 * src/backend/access/dolt_prolly/dg_node_cache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/doltchunk.h"
#include "access/doltnode.h"
#include "access/htup_details.h"
#include "fmgr.h"
#include "funcapi.h"
#include "port/atomics.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/dsa.h"
#include "utils/hsearch.h"
#include "utils/fmgrprotos.h"
#include "utils/memutils.h"

int			dolt_node_cache_size = 1024 * 1024;	/* kB */

#define NODE_PARTITIONS 128

/* Bytes of cache per slot: nodes average a little more. */
#define NODE_SLOT_BYTES 1024

/* The smallest cache, so that the area's own bookkeeping fits. */
#define NODE_CACHE_MIN ((Size) 1024 * 1024)

/* Larger nodes are not cached: each would evict too many others. */
#define NODE_MAX_BYTES ((Size) 64 * 1024)

/* Evictions to try to make room for one node's bytes before refusing it. */
#define NODE_ROOM_ATTEMPTS 64

/* A slot's state: pins, then usage, then flags. */
#define NODE_PIN_ONE 1U
#define NODE_PIN_MASK 0x0003FFFFU
#define NODE_USAGE_ONE (1U << 18)
#define NODE_USAGE_MASK 0x003C0000U
#define NODE_USAGE_MAX 5
#define NODE_VALID (1U << 30)	/* mapped and readable */
#define NODE_BUSY (1U << 31)	/* claimed by one backend, unmapped */

typedef struct NodeKey
{
	uint8		hash[DOLT_CHUNK_HASH_SIZE];
} NodeKey;

typedef struct NodeMapping
{
	NodeKey		key;
	int32		slot;
} NodeMapping;

typedef struct NodeSlot
{
	NodeKey		key;
	dsa_pointer data;
	uint32		length;
	pg_atomic_uint32 state;
} NodeSlot;

typedef struct NodeCache
{
	int32		nslots;
	Size		area_size;
	pg_atomic_uint64 hand;
	pg_atomic_uint64 added;
	pg_atomic_uint64 evictions;
	pg_atomic_uint64 rejected;
	LWLockPadded locks[NODE_PARTITIONS];
	NodeSlot	slots[FLEXIBLE_ARRAY_MEMBER];
} NodeCache;

static NodeCache *cache;
static HTAB *mapping;
static dsa_area *area;
static uint64 local_hits;
static uint64 local_misses;
static uint64 local_borrowed;

/* The cache's capacity in bytes, or zero if it is disabled. */
static Size
cache_bytes(void)
{
	Size		bytes = (Size) dolt_node_cache_size * 1024;

	return bytes == 0 ? 0 : Max(bytes, NODE_CACHE_MIN);
}

static int32
cache_slots(void)
{
	return (int32) Min(cache_bytes() / NODE_SLOT_BYTES, (Size) PG_INT32_MAX);
}

static Size
control_size(void)
{
	return MAXALIGN(add_size(offsetof(NodeCache, slots),
							 mul_size(cache_slots(), sizeof(NodeSlot))));
}

/* The node bytes' area, in place after the slots. */
static void *
area_place(void)
{
	return (char *) cache + control_size();
}

/* Node hashes are digests: their first bytes hash them well enough. */
static uint32
key_hash(const void *key, Size keysize)
{
	uint32		h;

	memcpy(&h, key, sizeof(h));
	return h;
}

Size
DoltNodeCacheShmemSize(void)
{
	Size		size;

	if (cache_bytes() == 0)
		return 0;
	size = add_size(control_size(), cache_bytes());
	return add_size(size, hash_estimate_size(cache_slots(), sizeof(NodeMapping)));
}

void
DoltNodeCacheShmemInit(void)
{
	HASHCTL		info = {0};
	bool		found;

	if (cache_bytes() == 0)
		return;
	cache = ShmemInitStruct("Dolt node cache", add_size(control_size(), cache_bytes()), &found);
	if (!found)
	{
		dsa_area   *created;

		cache->nslots = cache_slots();
		cache->area_size = cache_bytes();
		pg_atomic_init_u64(&cache->hand, 0);
		pg_atomic_init_u64(&cache->added, 0);
		pg_atomic_init_u64(&cache->evictions, 0);
		pg_atomic_init_u64(&cache->rejected, 0);
		for (int i = 0; i < NODE_PARTITIONS; i++)
			LWLockInitialize(&cache->locks[i].lock, LWTRANCHE_DOLT_NODE_MAPPING);
		for (int i = 0; i < cache->nslots; i++)
		{
			cache->slots[i].data = InvalidDsaPointer;
			pg_atomic_init_u32(&cache->slots[i].state, 0);
		}

		/* The postmaster cannot use dynamic segments: cap the area at its place. */
		created = dsa_create_in_place(area_place(), cache->area_size, LWTRANCHE_DOLT_NODE_DSA, NULL);
		dsa_pin(created);
		dsa_set_size_limit(created, cache->area_size);
		dsa_detach(created);
	}
	info.keysize = sizeof(NodeKey);
	info.entrysize = sizeof(NodeMapping);
	info.hash = key_hash;
	info.num_partitions = NODE_PARTITIONS;
	mapping = ShmemInitHash("Dolt node cache mapping", cache_slots(), cache_slots(), &info,
							HASH_ELEM | HASH_FUNCTION | HASH_PARTITION | HASH_FIXED_SIZE);
}

/* Whether the cache is available; attaches to its area on first use. */
static bool
cache_attach(void)
{
	if (!cache)
		return false;
	if (!area)
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		area = dsa_attach_in_place(area_place(), NULL);
		dsa_pin_mapping(area);
		MemoryContextSwitchTo(old);
	}
	return true;
}

static LWLock *
partition_lock(uint32 hashcode)
{
	return &cache->locks[hashcode % NODE_PARTITIONS].lock;
}

/* Pin a mapped slot; the caller holds its partition lock. */
static void
pin_slot(NodeSlot *slot)
{
	uint32		state = pg_atomic_read_u32(&slot->state);

	for (;;)
	{
		uint32		next = state + NODE_PIN_ONE;

		Assert(state & NODE_VALID);
		if ((state & NODE_USAGE_MASK) < NODE_USAGE_MAX * NODE_USAGE_ONE)
			next += NODE_USAGE_ONE;
		if (pg_atomic_compare_exchange_u32(&slot->state, &state, next))
			return;
	}
}

static void
lend(int32 index, DoltNodeLoan *loan)
{
	NodeSlot   *slot = &cache->slots[index];

	loan->data = dsa_get_address(area, slot->data);
	loan->length = slot->length;
	loan->token = (uintptr_t) index + 1;
	local_borrowed++;
}

bool
dolt_node_cache_borrow(const uint8 *hash, DoltNodeLoan *loan)
{
	NodeMapping *entry;
	uint32		hashcode;
	LWLock	   *lock;
	int32		index = -1;

	if (!cache_attach())
		return false;
	hashcode = key_hash(hash, DOLT_CHUNK_HASH_SIZE);
	lock = partition_lock(hashcode);
	LWLockAcquire(lock, LW_SHARED);
	entry = hash_search_with_hash_value(mapping, hash, hashcode, HASH_FIND, NULL);
	if (entry)
	{
		index = entry->slot;
		pin_slot(&cache->slots[index]);
	}
	LWLockRelease(lock);
	if (index < 0)
	{
		local_misses++;
		return false;
	}
	local_hits++;
	lend(index, loan);
	return true;
}

void
dolt_node_cache_return(uintptr_t token)
{
	NodeSlot   *slot = &cache->slots[token - 1];

	Assert(token >= 1 && token <= (uintptr_t) cache->nslots);
	Assert(local_borrowed > 0);
	Assert(pg_atomic_read_u32(&slot->state) & NODE_PIN_MASK);
	pg_atomic_fetch_sub_u32(&slot->state, NODE_PIN_ONE);
	local_borrowed--;
}

/*
 * Unmap and free the mapped slot |index|, seen in |state|, unless it was
 * pinned, used or replaced since. The slot is left claimed.
 */
static bool
evict(int32 index, uint32 state)
{
	NodeSlot   *slot = &cache->slots[index];
	NodeKey		key = slot->key;
	uint32		hashcode = key_hash(&key, sizeof(key));
	LWLock	   *lock = partition_lock(hashcode);
	NodeMapping *entry;
	bool		evicted = false;

	/* Pins are taken under the shared lock: none can be taken while we hold it. */
	LWLockAcquire(lock, LW_EXCLUSIVE);
	entry = hash_search_with_hash_value(mapping, &key, hashcode, HASH_FIND, NULL);
	if (entry && entry->slot == index &&
		pg_atomic_compare_exchange_u32(&slot->state, &state, NODE_BUSY))
	{
		hash_search_with_hash_value(mapping, &key, hashcode, HASH_REMOVE, NULL);
		evicted = true;
	}
	LWLockRelease(lock);
	if (!evicted)
		return false;
	dsa_free(area, slot->data);
	slot->data = InvalidDsaPointer;
	pg_atomic_fetch_add_u64(&cache->evictions, 1);
	return true;
}

/* Claim a free slot by clock sweep, evicting one if need be. */
static bool
claim_slot(int32 *out)
{
	uint64		limit = (uint64) cache->nslots * (NODE_USAGE_MAX + 1);

	for (uint64 tries = 0; tries < limit; tries++)
	{
		int32		index = (int32) (pg_atomic_fetch_add_u64(&cache->hand, 1) % cache->nslots);
		NodeSlot   *slot = &cache->slots[index];
		uint32		state = pg_atomic_read_u32(&slot->state);

		if (state & (NODE_BUSY | NODE_PIN_MASK))
			continue;
		if (!(state & NODE_VALID))
		{
			if (pg_atomic_compare_exchange_u32(&slot->state, &state, NODE_BUSY))
			{
				*out = index;
				return true;
			}
			continue;
		}
		if (state & NODE_USAGE_MASK)
		{
			pg_atomic_compare_exchange_u32(&slot->state, &state, state - NODE_USAGE_ONE);
			continue;
		}
		if (evict(index, state))
		{
			*out = index;
			return true;
		}
	}
	return false;
}

/* Room in the area for |length| bytes, evicting nodes to make it. */
static dsa_pointer
allocate_bytes(Size length)
{
	for (int attempt = 0;; attempt++)
	{
		dsa_pointer data = dsa_allocate_extended(area, length, DSA_ALLOC_NO_OOM);
		int32		freed;

		if (DsaPointerIsValid(data) || attempt == NODE_ROOM_ATTEMPTS || !claim_slot(&freed))
			return data;
		pg_atomic_write_u32(&cache->slots[freed].state, 0);
	}
}

/*
 * Add the node |hash|, unless the cache cannot make room for it. With |loan|,
 * also borrow it. Adding a node already cached borrows the cached copy.
 */
bool
dolt_node_cache_add(const uint8 *hash, const uint8 *data, Size length, DoltNodeLoan *loan)
{
	NodeMapping *entry;
	NodeSlot   *slot;
	dsa_pointer bytes;
	uint32		hashcode;
	LWLock	   *lock;
	int32		index;
	bool		found;

	if (!cache_attach() || length > NODE_MAX_BYTES)
		return false;
	bytes = allocate_bytes(length);
	if (!DsaPointerIsValid(bytes))
	{
		pg_atomic_fetch_add_u64(&cache->rejected, 1);
		return false;
	}
	if (!claim_slot(&index))
	{
		dsa_free(area, bytes);
		pg_atomic_fetch_add_u64(&cache->rejected, 1);
		return false;
	}
	slot = &cache->slots[index];
	memcpy(dsa_get_address(area, bytes), data, length);
	memcpy(slot->key.hash, hash, DOLT_CHUNK_HASH_SIZE);
	slot->data = bytes;
	slot->length = (uint32) length;

	hashcode = key_hash(hash, DOLT_CHUNK_HASH_SIZE);
	lock = partition_lock(hashcode);
	LWLockAcquire(lock, LW_EXCLUSIVE);
	entry = hash_search_with_hash_value(mapping, hash, hashcode, HASH_ENTER_NULL, &found);
	if (!entry || found)
	{
		/* Another backend added it first, so use its copy; or no mapping is free. */
		int32		existing = entry ? entry->slot : -1;

		if (entry && loan)
			pin_slot(&cache->slots[existing]);
		LWLockRelease(lock);
		slot->data = InvalidDsaPointer;
		pg_atomic_write_u32(&slot->state, 0);
		dsa_free(area, bytes);
		if (!entry)
		{
			pg_atomic_fetch_add_u64(&cache->rejected, 1);
			return false;
		}
		if (loan)
			lend(existing, loan);
		return true;
	}
	entry->slot = index;
	pg_atomic_write_u32(&slot->state, NODE_VALID | NODE_USAGE_ONE | (loan ? NODE_PIN_ONE : 0));
	LWLockRelease(lock);
	pg_atomic_fetch_add_u64(&cache->added, 1);
	if (loan)
		lend(index, loan);
	return true;
}

void
dolt_node_cache_stats(DoltNodeCacheStats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->hits = local_hits;
	stats->misses = local_misses;
	stats->borrowed = local_borrowed;
	if (!cache)
		return;
	stats->added = pg_atomic_read_u64(&cache->added);
	stats->evictions = pg_atomic_read_u64(&cache->evictions);
	stats->rejected = pg_atomic_read_u64(&cache->rejected);
	for (int32 i = 0; i < cache->nslots; i++)
	{
		uint32		state = pg_atomic_read_u32(&cache->slots[i].state);

		if (!(state & NODE_VALID))
			continue;
		stats->entries++;
		stats->bytes += cache->slots[i].length;
		if (state & NODE_PIN_MASK)
			stats->pinned++;
	}
}

/* SQL: dolt_node_cache_stats(); hits, misses and borrowed are this session's. */
Datum
dolt_node_cache_stats_sql(PG_FUNCTION_ARGS)
{
	DoltNodeCacheStats stats;
	TupleDesc	desc;
	Datum		values[9];
	bool		nulls[9] = {false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	dolt_node_cache_stats(&stats);
	values[0] = Int64GetDatum(stats.hits);
	values[1] = Int64GetDatum(stats.misses);
	values[2] = Int64GetDatum(stats.borrowed);
	values[3] = Int64GetDatum(stats.added);
	values[4] = Int64GetDatum(stats.evictions);
	values[5] = Int64GetDatum(stats.rejected);
	values[6] = Int64GetDatum(stats.entries);
	values[7] = Int64GetDatum(stats.bytes);
	values[8] = Int64GetDatum(stats.pinned);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}
