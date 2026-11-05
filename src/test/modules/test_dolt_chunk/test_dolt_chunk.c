/*--------------------------------------------------------------------------
 *
 * test_dolt_chunk.c
 *		SQL access to the dolt chunk store interface for tests and benchmarks.
 *
 * Everything here goes through access/doltchunk.h, so the same functions test
 * whichever implementation is installed, the journal unless
 * test_chunk_use_store('memory')
 * installs a session-local, non-transactional implementation, which shows that
 * callers depend only on the interface contract.
 *
 * The benchmark functions loop inside one call so their timings measure the
 * store and cache, not SQL call overhead. Synthetic chunk i of a seed has a
 * pseudo-random hash and contents; hashes are not digests of contents, so
 * benchmarks need dolt_chunk_verify_hashes off.
 *
 * src/test/modules/test_dolt_chunk/test_dolt_chunk.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/doltchunk.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_chunk_use_store);
PG_FUNCTION_INFO_V1(test_chunk_put);
PG_FUNCTION_INFO_V1(test_chunk_put_many);
PG_FUNCTION_INFO_V1(test_chunk_get);
PG_FUNCTION_INFO_V1(test_chunk_get_many);
PG_FUNCTION_INFO_V1(test_chunk_exists);
PG_FUNCTION_INFO_V1(test_chunk_delete);
PG_FUNCTION_INFO_V1(test_chunk_scan_count);
PG_FUNCTION_INFO_V1(test_chunk_max_size);
PG_FUNCTION_INFO_V1(test_chunk_store_stats);
PG_FUNCTION_INFO_V1(test_chunk_pin_survives_puts);
PG_FUNCTION_INFO_V1(test_chunk_bench_put);
PG_FUNCTION_INFO_V1(test_chunk_bench_get);

/* ---- An in-memory implementation: session-local and non-transactional ---- */

typedef struct MemoryChunk
{
	uint8		hash[DOLT_CHUNK_HASH_SIZE];
	uint8	   *data;
	Size		length;
} MemoryChunk;

static HTAB *memory_chunks;

static Size
memory_max_size(void)
{
	return 1024 * 1024;
}

static void *
memory_open(DoltChunkIntent intent)
{
	if (!memory_chunks)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = DOLT_CHUNK_HASH_SIZE;
		ctl.entrysize = sizeof(MemoryChunk);
		ctl.hcxt = TopMemoryContext;
		memory_chunks = hash_create("test dolt chunks", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	return memory_chunks;
}

static void
memory_close(void *state)
{
}

static void
memory_get(void *state, int n, const uint8 *hashes, DoltChunkFoundFn found, void *arg)
{
	for (int i = 0; i < n; i++)
	{
		MemoryChunk *chunk = hash_search(memory_chunks, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE,
										 HASH_FIND, NULL);

		if (chunk)
			found(arg, i, chunk->data, chunk->length);
	}
}

static void
memory_exists(void *state, int n, const uint8 *hashes, bool *found)
{
	for (int i = 0; i < n; i++)
		found[i] = hash_search(memory_chunks, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE,
							   HASH_FIND, NULL) != NULL;
}

static int
memory_put(void *state, int n, const uint8 *hashes, const uint8 *const *data, const Size *lengths, bool logged)
{
	int			stored = 0;

	for (int i = 0; i < n; i++)
	{
		bool		present;
		MemoryChunk *chunk = hash_search(memory_chunks, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE,
										 HASH_ENTER, &present);

		if (present)
			continue;
		chunk->data = MemoryContextAlloc(TopMemoryContext, Max(lengths[i], 1));
		memcpy(chunk->data, data[i], lengths[i]);
		chunk->length = lengths[i];
		stored++;
	}
	return stored;
}

static void
memory_scan(void *state, DoltChunkScanFn fn, void *arg)
{
	HASH_SEQ_STATUS status;
	MemoryChunk *chunk;

	hash_seq_init(&status, memory_chunks);
	while ((chunk = hash_seq_search(&status)) != NULL)
	{
		if (!fn(arg, chunk->hash, chunk->length))
		{
			hash_seq_term(&status);
			break;
		}
	}
}

static int
memory_remove(void *state, int n, const uint8 *hashes)
{
	int			removed = 0;

	for (int i = 0; i < n; i++)
	{
		MemoryChunk *chunk = hash_search(memory_chunks, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE,
										 HASH_FIND, NULL);

		if (chunk)
		{
			pfree(chunk->data);
			hash_search(memory_chunks, chunk->hash, HASH_REMOVE, NULL);
			removed++;
		}
	}
	return removed;
}

static void
memory_stats(DoltChunkStoreStats *stats)
{
	HASH_SEQ_STATUS status;
	MemoryChunk *chunk;

	stats->chunks = 0;
	stats->bytes = 0;
	if (!memory_chunks)
		return;
	hash_seq_init(&status, memory_chunks);
	while ((chunk = hash_seq_search(&status)) != NULL)
	{
		stats->chunks++;
		stats->bytes += chunk->length;
	}
}

static const DoltChunkStoreRoutine memory_routine = {
	.name = "memory",
	.max_size = memory_max_size,
	.open = memory_open,
	.close = memory_close,
	.get = memory_get,
	.exists = memory_exists,
	.put = memory_put,
	.scan = memory_scan,
	.remove = memory_remove,
	.stats = memory_stats,
};

/* ---- SQL functions ---- */

Datum
test_chunk_use_store(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const DoltChunkStoreRoutine *previous;

	if (strcmp(name, "journal") == 0)
		previous = dolt_chunk_set_routine(NULL);
	else if (strcmp(name, "memory") == 0)
		previous = dolt_chunk_set_routine(&memory_routine);
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unknown chunk store \"%s\"", name)));
	PG_RETURN_TEXT_P(cstring_to_text(previous->name));
}

static const uint8 *
hash_argument(bytea *hash)
{
	if (VARSIZE_ANY_EXHDR(hash) != DOLT_CHUNK_HASH_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("chunk hash must be %d bytes", DOLT_CHUNK_HASH_SIZE)));
	return (const uint8 *) VARDATA_ANY(hash);
}

/* Pack a bytea[] of hashes into consecutive DOLT_CHUNK_HASH_SIZE-byte hashes. */
static uint8 *
packed_hashes(ArrayType *array, int *n)
{
	Datum	   *items;
	bool	   *nulls;
	uint8	   *hashes;

	deconstruct_array(array, BYTEAOID, -1, false, TYPALIGN_INT, &items, &nulls, n);
	hashes = palloc(Max(*n, 1) * DOLT_CHUNK_HASH_SIZE);
	for (int i = 0; i < *n; i++)
	{
		if (nulls[i])
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("chunk hash is null")));
		memcpy(hashes + (Size) i * DOLT_CHUNK_HASH_SIZE,
			   hash_argument(DatumGetByteaPP(items[i])), DOLT_CHUNK_HASH_SIZE);
	}
	return hashes;
}

static bytea *
chunk_bytea(const DoltChunk *chunk)
{
	bytea	   *result = palloc(VARHDRSZ + chunk->length);

	SET_VARSIZE(result, VARHDRSZ + chunk->length);
	memcpy(VARDATA(result), chunk->data, chunk->length);
	return result;
}

Datum
test_chunk_put(PG_FUNCTION_ARGS)
{
	bytea	   *hash = PG_GETARG_BYTEA_PP(0);
	bytea	   *data = PG_GETARG_BYTEA_PP(1);
	DoltChunkStore *store = dolt_chunk_open(PG_GETARG_BOOL(2) ? DOLT_CHUNK_WRITE : DOLT_CHUNK_WRITE_UNLOGGED);
	bool		stored;

	stored = dolt_chunk_put(store, hash_argument(hash), (const uint8 *) VARDATA_ANY(data),
							VARSIZE_ANY_EXHDR(data));
	dolt_chunk_close(store);
	PG_RETURN_BOOL(stored);
}

Datum
test_chunk_put_many(PG_FUNCTION_ARGS)
{
	int			n,
				ndata;
	uint8	   *hashes = packed_hashes(PG_GETARG_ARRAYTYPE_P(0), &n);
	Datum	   *items;
	bool	   *nulls;
	const uint8 **data;
	Size	   *lengths;
	DoltChunkStore *store;
	int			stored;

	deconstruct_array(PG_GETARG_ARRAYTYPE_P(1), BYTEAOID, -1, false, TYPALIGN_INT, &items, &nulls, &ndata);
	if (ndata != n)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("hash and data arrays differ in length")));
	data = palloc(Max(n, 1) * sizeof(uint8 *));
	lengths = palloc(Max(n, 1) * sizeof(Size));
	for (int i = 0; i < n; i++)
	{
		bytea	   *value = DatumGetByteaPP(items[i]);

		data[i] = (const uint8 *) VARDATA_ANY(value);
		lengths[i] = VARSIZE_ANY_EXHDR(value);
	}
	store = dolt_chunk_open(DOLT_CHUNK_WRITE);
	stored = dolt_chunk_put_many(store, n, hashes, data, lengths);
	dolt_chunk_close(store);
	PG_RETURN_INT32(stored);
}

Datum
test_chunk_get(PG_FUNCTION_ARGS)
{
	bytea	   *hash = PG_GETARG_BYTEA_PP(0);
	DoltChunkStore *store = dolt_chunk_open(DOLT_CHUNK_READ);
	DoltChunk	chunk;
	bytea	   *result = NULL;

	if (dolt_chunk_get(store, hash_argument(hash), &chunk))
	{
		result = chunk_bytea(&chunk);
		dolt_chunk_release(store, &chunk);
	}
	dolt_chunk_close(store);
	if (!result)
		PG_RETURN_NULL();
	PG_RETURN_BYTEA_P(result);
}

Datum
test_chunk_get_many(PG_FUNCTION_ARGS)
{
	int			n;
	uint8	   *hashes = packed_hashes(PG_GETARG_ARRAYTYPE_P(0), &n);
	DoltChunk  *chunks = palloc(Max(n, 1) * sizeof(DoltChunk));
	Datum	   *values = palloc(Max(n, 1) * sizeof(Datum));
	bool	   *nulls = palloc(Max(n, 1) * sizeof(bool));
	int			dims[1] = {n};
	int			lbs[1] = {1};
	DoltChunkStore *store = dolt_chunk_open(DOLT_CHUNK_READ);

	dolt_chunk_get_many(store, n, hashes, chunks);
	for (int i = 0; i < n; i++)
	{
		nulls[i] = chunks[i].data == NULL;
		values[i] = nulls[i] ? (Datum) 0 : PointerGetDatum(chunk_bytea(&chunks[i]));
	}

	/* Closing releases every pin the call took. */
	dolt_chunk_close(store);
	PG_RETURN_ARRAYTYPE_P(construct_md_array(values, nulls, 1, dims, lbs, BYTEAOID, -1, false, TYPALIGN_INT));
}

Datum
test_chunk_exists(PG_FUNCTION_ARGS)
{
	bytea	   *hash = PG_GETARG_BYTEA_PP(0);
	DoltChunkStore *store = dolt_chunk_open(DOLT_CHUNK_READ);
	bool		exists = dolt_chunk_exists(store, hash_argument(hash));

	dolt_chunk_close(store);
	PG_RETURN_BOOL(exists);
}

Datum
test_chunk_delete(PG_FUNCTION_ARGS)
{
	bytea	   *hash = PG_GETARG_BYTEA_PP(0);
	DoltChunkStore *store = dolt_chunk_open(DOLT_CHUNK_WRITE);
	int			removed = dolt_chunk_delete_many(store, 1, hash_argument(hash));

	dolt_chunk_close(store);
	PG_RETURN_BOOL(removed > 0);
}

static bool
count_chunk(void *arg, const uint8 *hash, Size length)
{
	(*(int64 *) arg)++;
	return true;
}

Datum
test_chunk_scan_count(PG_FUNCTION_ARGS)
{
	DoltChunkStore *store = dolt_chunk_open(DOLT_CHUNK_READ);
	int64		count = 0;

	dolt_chunk_scan(store, count_chunk, &count);
	dolt_chunk_close(store);
	PG_RETURN_INT64(count);
}

Datum
test_chunk_max_size(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32((int32) dolt_chunk_max_size());
}

Datum
test_chunk_store_stats(PG_FUNCTION_ARGS)
{
	DoltChunkStoreStats stats;
	TupleDesc	desc;
	Datum		values[2];
	bool		nulls[2] = {false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	dolt_chunk_store_stats(&stats);
	values[0] = Float8GetDatum(stats.chunks);
	values[1] = Int64GetDatum(stats.bytes);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}

static uint64
splitmix64(uint64 *state)
{
	uint64		z = (*state += UINT64CONST(0x9E3779B97F4A7C15));

	z = (z ^ (z >> 30)) * UINT64CONST(0xBF58476D1CE4E5B9);
	z = (z ^ (z >> 27)) * UINT64CONST(0x94D049BB133111EB);
	return z ^ (z >> 31);
}

/* Synthetic chunk |i| of |seed|: its hash, and its contents if |data| is set. */
static void
synthetic_chunk(int32 seed, int64 i, uint8 *hash, uint8 *data, Size size)
{
	uint64		state = ((uint64) (uint32) seed << 40) ^ (uint64) i;
	Size		offset;

	for (offset = 0; offset < DOLT_CHUNK_HASH_SIZE; offset += 8)
	{
		uint64		word = splitmix64(&state);

		memcpy(hash + offset, &word, Min(8, DOLT_CHUNK_HASH_SIZE - offset));
	}
	for (offset = 0; data && offset < size; offset += 8)
	{
		uint64		word = splitmix64(&state);

		memcpy(data + offset, &word, Min(8, size - offset));
	}
}

/*
 * Pin a chunk, then put |count| synthetic 4 KB chunks through the same handle.
 * Returns whether the pinned bytes stayed intact throughout, and whether
 * releasing them cleared the chunk.
 */
Datum
test_chunk_pin_survives_puts(PG_FUNCTION_ARGS)
{
	bytea	   *hash = PG_GETARG_BYTEA_PP(0);
	int32		count = PG_GETARG_INT32(1);
	DoltChunkStore *store = dolt_chunk_open(DOLT_CHUNK_WRITE);
	DoltChunk	chunk;
	bytea	   *expected;
	uint8		filler_hash[DOLT_CHUNK_HASH_SIZE];
	uint8		filler[4096];
	bool		intact;

	if (!dolt_chunk_get(store, hash_argument(hash), &chunk))
		elog(ERROR, "chunk to pin is missing");
	expected = chunk_bytea(&chunk);
	for (int i = 0; i < count; i++)
	{
		synthetic_chunk(99, i, filler_hash, filler, sizeof(filler));
		dolt_chunk_put(store, filler_hash, filler, sizeof(filler));
	}
	intact = memcmp(chunk.data, VARDATA(expected), chunk.length) == 0;
	dolt_chunk_release(store, &chunk);
	intact = intact && chunk.data == NULL && chunk.pin == 0;
	dolt_chunk_close(store);
	PG_RETURN_BOOL(intact);
}

/* Size range for mixed-size benchmark chunks, as observed for prolly leaves. */
#define MIXED_MIN 512
#define MIXED_MAX 8000

Datum
test_chunk_bench_put(PG_FUNCTION_ARGS)
{
	int32		seed = PG_GETARG_INT32(0);
	int64		first = PG_GETARG_INT64(1);
	int64		count = PG_GETARG_INT64(2);
	int32		size = PG_GETARG_INT32(3);
	DoltChunkStore *store = dolt_chunk_open(DOLT_CHUNK_WRITE);
	uint8		hash[DOLT_CHUNK_HASH_SIZE];
	uint8	   *data = palloc(size > 0 ? size : MIXED_MAX);
	instr_time	total;

	INSTR_TIME_SET_ZERO(total);
	for (int64 i = first; i < first + count; i++)
	{
		instr_time	start,
					end;
		Size		length = size;

		CHECK_FOR_INTERRUPTS();
		if (size <= 0)
		{
			/* Mixed sizes: uniform over the range observed for prolly leaves. */
			synthetic_chunk(seed, i, hash, NULL, 0);
			length = MIXED_MIN + ((hash[0] << 8) | hash[1]) % (MIXED_MAX - MIXED_MIN + 1);
		}
		synthetic_chunk(seed, i, hash, data, length);
		INSTR_TIME_SET_CURRENT(start);
		dolt_chunk_put(store, hash, data, length);
		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(total, end, start);
	}
	dolt_chunk_close(store);
	PG_RETURN_FLOAT8(INSTR_TIME_GET_DOUBLE(total));
}

Datum
test_chunk_bench_get(PG_FUNCTION_ARGS)
{
	int32		seed = PG_GETARG_INT32(0);
	int64		nchunks = PG_GETARG_INT64(1);
	int64		count = PG_GETARG_INT64(2);
	uint64		rng = (uint64) PG_GETARG_INT32(3);
	DoltChunkStore *store = dolt_chunk_open(DOLT_CHUNK_READ);
	uint8		hash[DOLT_CHUNK_HASH_SIZE];
	instr_time	total;

	INSTR_TIME_SET_ZERO(total);
	for (int64 n = 0; n < count; n++)
	{
		instr_time	start,
					end;
		DoltChunk	chunk;
		bool		found;

		CHECK_FOR_INTERRUPTS();
		synthetic_chunk(seed, (int64) (splitmix64(&rng) % (uint64) nchunks), hash, NULL, 0);
		INSTR_TIME_SET_CURRENT(start);
		found = dolt_chunk_get(store, hash, &chunk);
		dolt_chunk_release(store, &chunk);
		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(total, end, start);
		if (!found)
			elog(ERROR, "benchmark chunk missing");
	}
	dolt_chunk_close(store);
	PG_RETURN_FLOAT8(INSTR_TIME_GET_DOUBLE(total));
}
