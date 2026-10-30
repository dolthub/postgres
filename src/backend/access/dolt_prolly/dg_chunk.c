/*-------------------------------------------------------------------------
 *
 * dg_chunk.c
 *	  The chunk store layer above every implementation: dispatch, the node
 *	  cache, pins, hash verification and size checks.
 *
 * The node cache maps hashes to copies of chunk bytes. Chunks are immutable, so
 * entries never need invalidation. Unpinned entries sit on an LRU list and are
 * evicted above dolt_chunk_cache_size; pinned entries sit on a separate list and
 * are never evicted. When the cache cannot take a chunk, a get returns a private
 * copy instead. Pins are tracked per store handle and dropped at transaction end,
 * so an error or a forgotten release cannot pin the cache permanently.
 *
 * Existence checks always ask the store: a cached copy proves only that the bytes
 * for a hash are known, not that the store still holds them.
 *
 * src/backend/access/dolt_prolly/dg_chunk.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/doltchunk.h"
#include "access/xact.h"
#include "common/cryptohash.h"
#include "common/sha2.h"
#include "lib/ilist.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

int			dolt_chunk_cache_size = 0;	/* kB; prolly nodes are cached in dg_node_cache.c */
bool		dolt_chunk_verify_hashes = false;

typedef struct CacheEntry
{
	uint8		hash[DOLT_CHUNK_HASH_SIZE];
	uint8	   *data;
	Size		length;
	int			pins;
	dlist_node	node;			/* on cache_lru if unpinned, else cache_pinned */
} CacheEntry;

/* A pin held through a store handle: a cache entry, or a private copy. */
typedef struct Pin
{
	CacheEntry *entry;
	uint8	   *copy;
	int			next_free;		/* free-list link while unused, 1-based */
} Pin;

struct DoltChunkStore
{
	const DoltChunkStoreRoutine *routine;
	void	   *state;
	DoltChunkIntent intent;
	uint64		epoch;			/* transaction its pins belong to */
	Pin		   *pins;
	int			npins;
	int			maxpins;
	int			free_pin;		/* first reusable slot, 1-based; 0 if none */
};

static const DoltChunkStoreRoutine *current_routine;
static int	open_stores;

static MemoryContext cache_context;
static HTAB *cache;
static dlist_head cache_lru = DLIST_STATIC_INIT(cache_lru);
static dlist_head cache_pinned = DLIST_STATIC_INIT(cache_pinned);
static Size cache_bytes;
static DoltChunkCacheStats cache_stats;
static uint64 pin_epoch = 1;
static bool xact_callback_registered;

static const DoltChunkStoreRoutine *
routine(void)
{
	return current_routine ? current_routine : dolt_chunk_heap_routine();
}

/* Bytes an entry is charged against dolt_chunk_cache_size. */
static Size
entry_cost(Size length)
{
	return length + sizeof(CacheEntry) + 64;
}

/* Pins never outlive a transaction; abandoned handles cannot leak them. */
static void
chunk_xact_callback(XactEvent event, void *arg)
{
	dlist_mutable_iter iter;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PREPARE && event != XACT_EVENT_PARALLEL_COMMIT &&
		event != XACT_EVENT_PARALLEL_ABORT)
		return;
	pin_epoch++;
	open_stores = 0;
	dlist_foreach_modify(iter, &cache_pinned)
	{
		CacheEntry *entry = dlist_container(CacheEntry, node, iter.cur);

		entry->pins = 0;
		dlist_delete(&entry->node);
		dlist_push_head(&cache_lru, &entry->node);
	}
}

static void
cache_init(void)
{
	HASHCTL		ctl = {0};

	if (!xact_callback_registered)
	{
		RegisterXactCallback(chunk_xact_callback, NULL);
		xact_callback_registered = true;
	}
	if (cache)
		return;
	cache_context = AllocSetContextCreate(TopMemoryContext, "dolt chunk cache", ALLOCSET_DEFAULT_SIZES);
	ctl.keysize = DOLT_CHUNK_HASH_SIZE;
	ctl.entrysize = sizeof(CacheEntry);
	ctl.hcxt = cache_context;
	cache = hash_create("dolt chunk cache", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static void
cache_evict(CacheEntry *entry)
{
	Assert(entry->pins == 0);
	dlist_delete(&entry->node);
	cache_bytes -= entry_cost(entry->length);
	pfree(entry->data);
	hash_search(cache, entry->hash, HASH_REMOVE, NULL);
	cache_stats.evictions++;
}

static void
cache_pin(CacheEntry *entry)
{
	if (entry->pins++ == 0)
	{
		dlist_delete(&entry->node);
		dlist_push_head(&cache_pinned, &entry->node);
	}
}

static void
cache_unpin(CacheEntry *entry)
{
	Assert(entry->pins > 0);
	if (--entry->pins == 0)
	{
		dlist_delete(&entry->node);
		dlist_push_head(&cache_lru, &entry->node);
	}
}

/*
 * Copy a chunk into the cache, evicting least recently used unpinned entries
 * to make room. Returns the entry, or NULL if the chunk cannot fit.
 */
static CacheEntry *
cache_insert(const uint8 *hash, const uint8 *data, Size length)
{
	Size		limit = (Size) dolt_chunk_cache_size * 1024;
	CacheEntry *entry;
	bool		found;

	cache_init();
	entry = hash_search(cache, hash, HASH_FIND, NULL);
	if (entry)
		return entry;
	while (cache_bytes + entry_cost(length) > limit && !dlist_is_empty(&cache_lru))
		cache_evict(dlist_container(CacheEntry, node, dlist_tail_node(&cache_lru)));
	if (cache_bytes + entry_cost(length) > limit)
		return NULL;

	entry = hash_search(cache, hash, HASH_ENTER, &found);
	entry->data = MemoryContextAlloc(cache_context, Max(length, 1));
	memcpy(entry->data, data, length);
	entry->length = length;
	entry->pins = 0;
	dlist_push_head(&cache_lru, &entry->node);
	cache_bytes += entry_cost(length);
	return entry;
}

static void
verify(const uint8 *hash, const uint8 *data, Size length)
{
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);
	uint8		actual[PG_SHA256_DIGEST_LENGTH];

	if (pg_cryptohash_init(ctx) < 0 || pg_cryptohash_update(ctx, data, length) < 0 ||
		pg_cryptohash_final(ctx, actual, sizeof(actual)) < 0)
		elog(ERROR, "could not compute chunk hash: %s", pg_cryptohash_error(ctx));
	pg_cryptohash_free(ctx);

	/* A chunk's hash is its SHA-256 truncated to DOLT_CHUNK_HASH_SIZE bytes. */
	if (memcmp(actual, hash, DOLT_CHUNK_HASH_SIZE) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("dolt chunk content does not match its hash")));
}

DoltChunkStore *
dolt_chunk_open(DoltChunkIntent intent)
{
	DoltChunkStore *store = palloc0(sizeof(DoltChunkStore));

	cache_init();
	store->routine = routine();
	store->intent = intent;
	store->epoch = pin_epoch;
	store->state = store->routine->open(intent);
	open_stores++;
	return store;
}

static void
release_pin(DoltChunkStore *store, int slot)
{
	Pin		   *pin = &store->pins[slot];

	if (pin->entry)
		cache_unpin(pin->entry);
	if (pin->copy)
		pfree(pin->copy);
	pin->entry = NULL;
	pin->copy = NULL;
	pin->next_free = store->free_pin;
	store->free_pin = slot + 1;
}

void
dolt_chunk_close(DoltChunkStore *store)
{
	if (store->epoch == pin_epoch)
	{
		for (int i = 0; i < store->npins; i++)
			release_pin(store, i);
		open_stores--;
	}
	store->routine->close(store->state);
	if (store->pins)
		pfree(store->pins);
	pfree(store);
}

static int
new_pin(DoltChunkStore *store, CacheEntry *entry, uint8 *copy)
{
	int			slot;

	if (store->epoch != pin_epoch)
		elog(ERROR, "dolt chunk store used after the transaction that opened it");
	if (store->free_pin)
	{
		slot = store->free_pin - 1;
		store->free_pin = store->pins[slot].next_free;
	}
	else
	{
		if (store->npins == store->maxpins)
		{
			store->maxpins = store->maxpins ? store->maxpins * 2 : 16;
			store->pins = store->pins ?
				repalloc(store->pins, store->maxpins * sizeof(Pin)) :
				MemoryContextAlloc(GetMemoryChunkContext(store), store->maxpins * sizeof(Pin));
		}
		slot = store->npins++;
	}
	store->pins[slot].entry = entry;
	store->pins[slot].copy = copy;
	if (entry)
		cache_pin(entry);
	return slot + 1;			/* pin numbers start at 1; 0 means none */
}

void
dolt_chunk_release(DoltChunkStore *store, DoltChunk *chunk)
{
	if (chunk->pin && store->epoch == pin_epoch)
		release_pin(store, chunk->pin - 1);
	chunk->pin = 0;
	chunk->data = NULL;
	chunk->length = 0;
}

typedef struct GetContext
{
	DoltChunkStore *store;
	const uint8 *hashes;
	DoltChunk  *chunks;
	int		   *positions;		/* miss index -> caller index */
} GetContext;

static void
store_found(void *arg, int index, const uint8 *data, Size length)
{
	GetContext *ctx = arg;
	int			position = ctx->positions[index];
	const uint8 *hash = ctx->hashes + (Size) position * DOLT_CHUNK_HASH_SIZE;
	DoltChunk  *chunk = &ctx->chunks[position];
	CacheEntry *entry;

	if (dolt_chunk_verify_hashes)
		verify(hash, data, length);
	entry = cache_insert(hash, data, length);
	if (entry)
	{
		chunk->pin = new_pin(ctx->store, entry, NULL);
		chunk->data = entry->data;
	}
	else
	{
		uint8	   *copy = MemoryContextAlloc(GetMemoryChunkContext(ctx->store), Max(length, 1));

		memcpy(copy, data, length);
		chunk->pin = new_pin(ctx->store, NULL, copy);
		chunk->data = copy;
	}
	chunk->length = length;
}

void
dolt_chunk_get_many(DoltChunkStore *store, int n, const uint8 *hashes, DoltChunk *chunks)
{
	uint8	   *missing = NULL;
	int		   *positions = NULL;
	int			nmissing = 0;

	cache_init();
	for (int i = 0; i < n; i++)
	{
		const uint8 *hash = hashes + (Size) i * DOLT_CHUNK_HASH_SIZE;
		CacheEntry *entry = hash_search(cache, hash, HASH_FIND, NULL);

		chunks[i].data = NULL;
		chunks[i].length = 0;
		chunks[i].pin = 0;
		if (entry)
		{
			cache_stats.hits++;
			chunks[i].pin = new_pin(store, entry, NULL);
			chunks[i].data = entry->data;
			chunks[i].length = entry->length;
			continue;
		}
		cache_stats.misses++;
		if (!missing)
		{
			missing = palloc((Size) n * DOLT_CHUNK_HASH_SIZE);
			positions = palloc(n * sizeof(int));
		}
		memcpy(missing + (Size) nmissing * DOLT_CHUNK_HASH_SIZE, hash, DOLT_CHUNK_HASH_SIZE);
		positions[nmissing++] = i;
	}
	if (nmissing)
	{
		GetContext	ctx = {store, hashes, chunks, positions};

		store->routine->get(store->state, nmissing, missing, store_found, &ctx);
		pfree(missing);
		pfree(positions);
	}
}

bool
dolt_chunk_get(DoltChunkStore *store, const uint8 *hash, DoltChunk *chunk)
{
	dolt_chunk_get_many(store, 1, hash, chunk);
	return chunk->data != NULL;
}

void
dolt_chunk_exists_many(DoltChunkStore *store, int n, const uint8 *hashes, bool *found)
{
	store->routine->exists(store->state, n, hashes, found);
}

bool
dolt_chunk_exists(DoltChunkStore *store, const uint8 *hash)
{
	bool		found;

	dolt_chunk_exists_many(store, 1, hash, &found);
	return found;
}

static void
require_write(DoltChunkStore *store)
{
	if (store->intent != DOLT_CHUNK_WRITE)
		elog(ERROR, "dolt chunk store opened read-only");
	if (XactReadOnly)
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot write dolt chunks in a read-only transaction")));
}

int
dolt_chunk_put_many(DoltChunkStore *store, int n, const uint8 *hashes,
					const uint8 *const *data, const Size *lengths)
{
	Size		max = store->routine->max_size();
	int			stored;

	require_write(store);
	for (int i = 0; i < n; i++)
	{
		if (lengths[i] > max)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("dolt chunk of %zu bytes exceeds the inline limit of %zu bytes",
							lengths[i], max)));
		if (dolt_chunk_verify_hashes)
			verify(hashes + (Size) i * DOLT_CHUNK_HASH_SIZE, data[i], lengths[i]);
	}
	stored = store->routine->put(store->state, n, hashes, data, lengths);

	/* Newly written chunks are likely to be read soon. */
	for (int i = 0; i < n; i++)
		cache_insert(hashes + (Size) i * DOLT_CHUNK_HASH_SIZE, data[i], lengths[i]);
	return stored;
}

bool
dolt_chunk_put(DoltChunkStore *store, const uint8 *hash, const uint8 *data, Size length)
{
	return dolt_chunk_put_many(store, 1, hash, &data, &length) > 0;
}

void
dolt_chunk_scan(DoltChunkStore *store, DoltChunkScanFn fn, void *arg)
{
	store->routine->scan(store->state, fn, arg);
}

int
dolt_chunk_delete_many(DoltChunkStore *store, int n, const uint8 *hashes)
{
	require_write(store);

	/*
	 * Cached copies stay valid: a hash's bytes never change, and a deleted
	 * chunk is unreachable by definition.
	 */
	return store->routine->remove(store->state, n, hashes);
}

Size
dolt_chunk_max_size(void)
{
	return routine()->max_size();
}

void
dolt_chunk_store_stats(DoltChunkStoreStats *stats)
{
	routine()->stats(stats);
}

void
dolt_chunk_cache_stats(DoltChunkCacheStats *stats)
{
	dlist_iter	iter;

	*stats = cache_stats;
	stats->entries = cache ? hash_get_num_entries(cache) : 0;
	stats->bytes = cache_bytes;
	stats->pinned = 0;
	dlist_foreach(iter, &cache_pinned)
		stats->pinned++;
}

void
dolt_chunk_cache_reset(void)
{
	if (!dlist_is_empty(&cache_pinned))
		elog(ERROR, "cannot reset the dolt chunk cache while chunks are pinned");
	if (cache)
	{
		MemoryContextDelete(cache_context);
		cache = NULL;
		cache_context = NULL;
	}
	dlist_init(&cache_lru);
	dlist_init(&cache_pinned);
	cache_bytes = 0;
	memset(&cache_stats, 0, sizeof(cache_stats));
}

const DoltChunkStoreRoutine *
dolt_chunk_set_routine(const DoltChunkStoreRoutine *replacement)
{
	const DoltChunkStoreRoutine *previous = routine();

	if (open_stores > 0)
		elog(ERROR, "cannot change the dolt chunk store while a store is open");
	dolt_chunk_cache_reset();
	current_routine = replacement;
	return previous;
}
