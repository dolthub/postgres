/*-------------------------------------------------------------------------
 *
 * dg_chunk.c
 *	  The chunk store layer above every implementation: dispatch, pins, hash
 *	  verification and size checks.
 *
 * Nothing is cached here: prolly nodes are cached once for every backend, in
 * dg_node_cache.c. A get returns a copy of each chunk, pinned through the
 * store handle: it stays valid until released, the handle closes or the
 * transaction ends, so an error or a forgotten release cannot leak it.
 *
 * src/backend/access/dolt_prolly/dg_chunk.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/doltchunk.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "common/cryptohash.h"
#include "common/sha2.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

bool		dolt_chunk_verify_hashes = false;

/* Chunks this session looked up in the store, found or not. */
static uint64 session_reads;

/* A copy held through a store handle. */
typedef struct Pin
{
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
static uint64 pin_epoch = 1;
static bool xact_callback_registered;

static const DoltChunkStoreRoutine *
routine(void)
{
	return current_routine ? current_routine : dolt_chunk_journal_routine();
}

/*
 * Unlogged puts become durable before the commit record. Pins never outlive a
 * transaction; abandoned handles cannot leak them.
 */
static void
chunk_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_PARALLEL_PRE_COMMIT ||
		event == XACT_EVENT_PRE_PREPARE)
	{
		dolt_chunk_journal_pre_commit();
		return;
	}
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PREPARE && event != XACT_EVENT_PARALLEL_COMMIT &&
		event != XACT_EVENT_PARALLEL_ABORT)
		return;
	pin_epoch++;
	open_stores = 0;
	dolt_chunk_journal_xact_end();
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

	if (!xact_callback_registered)
	{
		RegisterXactCallback(chunk_xact_callback, NULL);
		xact_callback_registered = true;
	}
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

	if (pin->copy)
		pfree(pin->copy);
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
			if (store->pins[i].copy)
				release_pin(store, i);
		open_stores--;
	}
	store->routine->close(store->state);
	if (store->pins)
		pfree(store->pins);
	pfree(store);
}

static int
new_pin(DoltChunkStore *store, uint8 *copy)
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
	store->pins[slot].copy = copy;
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
} GetContext;

static void
store_found(void *arg, int index, const uint8 *data, Size length)
{
	GetContext *ctx = arg;
	DoltChunk  *chunk = &ctx->chunks[index];
	uint8	   *copy;

	if (dolt_chunk_verify_hashes)
		verify(ctx->hashes + (Size) index * DOLT_CHUNK_HASH_SIZE, data, length);
	copy = MemoryContextAlloc(GetMemoryChunkContext(ctx->store), Max(length, 1));
	memcpy(copy, data, length);
	chunk->pin = new_pin(ctx->store, copy);
	chunk->data = copy;
	chunk->length = length;
}

void
dolt_chunk_get_many(DoltChunkStore *store, int n, const uint8 *hashes, DoltChunk *chunks)
{
	GetContext	ctx = {store, hashes, chunks};

	for (int i = 0; i < n; i++)
	{
		chunks[i].data = NULL;
		chunks[i].length = 0;
		chunks[i].pin = 0;
	}
	session_reads += n;
	if (n > 0)
		store->routine->get(store->state, n, hashes, store_found, &ctx);
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
	if (store->intent == DOLT_CHUNK_READ)
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
	stored = store->routine->put(store->state, n, hashes, data, lengths, store->intent == DOLT_CHUNK_WRITE);
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

const DoltChunkStoreRoutine *
dolt_chunk_set_routine(const DoltChunkStoreRoutine *replacement)
{
	const DoltChunkStoreRoutine *previous = routine();

	if (open_stores > 0)
		elog(ERROR, "cannot change the dolt chunk store while a store is open");
	current_routine = replacement;
	return previous;
}

/* ---- SQL access, for tests and diagnostics ---- */

static void
require_superuser(void)
{
	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("chunk store functions require superuser")));
}

static bytea *
bytes_value(const uint8 *data, Size length)
{
	bytea	   *value = palloc(VARHDRSZ + length);

	SET_VARSIZE(value, VARHDRSZ + length);
	memcpy(VARDATA(value), data, length);
	return value;
}

static bool
list_row(void *arg, const uint8 *hash, Size length)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[2];
	bool		nulls[2] = {false};

	values[0] = PointerGetDatum(bytes_value(hash, DOLT_CHUNK_HASH_SIZE));
	values[1] = Int64GetDatum((int64) length);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	return true;
}

/* dolt_chunk_list(): every chunk the store holds, with its size. */
Datum
dolt_chunk_list(PG_FUNCTION_ARGS)
{
	DoltChunkStore *store;

	require_superuser();
	InitMaterializedSRF(fcinfo, 0);
	store = dolt_chunk_open(DOLT_CHUNK_READ);
	dolt_chunk_scan(store, list_row, fcinfo->resultinfo);
	dolt_chunk_close(store);
	return (Datum) 0;
}

/* dolt_chunk_read(hash): a chunk's bytes, or NULL if the store lacks it. */
Datum
dolt_chunk_read(PG_FUNCTION_ARGS)
{
	bytea	   *hash = PG_GETARG_BYTEA_PP(0);
	DoltChunkStore *store;
	DoltChunk	chunk;
	bytea	   *result = NULL;

	require_superuser();
	if (VARSIZE_ANY_EXHDR(hash) != DOLT_CHUNK_HASH_SIZE)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("a chunk hash has %d bytes", DOLT_CHUNK_HASH_SIZE)));
	store = dolt_chunk_open(DOLT_CHUNK_READ);
	if (dolt_chunk_get(store, (const uint8 *) VARDATA_ANY(hash), &chunk))
		result = bytes_value(chunk.data, chunk.length);
	dolt_chunk_close(store);
	if (!result)
		PG_RETURN_NULL();
	PG_RETURN_BYTEA_P(result);
}

/* dolt_chunk_stats(): the store's chunks and bytes, and this session's lookups. */
Datum
dolt_chunk_stats(PG_FUNCTION_ARGS)
{
	DoltChunkStoreStats stats = {0};
	TupleDesc	desc;
	Datum		values[3];
	bool		nulls[3] = {false};

	require_superuser();
	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	dolt_chunk_store_stats(&stats);
	values[0] = Float8GetDatum(stats.chunks);
	values[1] = Int64GetDatum((int64) stats.bytes);
	values[2] = Int64GetDatum((int64) session_reads);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}
