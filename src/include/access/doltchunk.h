/*-------------------------------------------------------------------------
 *
 * doltchunk.h
 *	  The prolly chunk store interface.
 *
 * Every consumer of prolly chunks goes through this API. Chunks are immutable
 * byte strings addressed by the SHA-256 of their contents, so the bytes for a
 * hash never change.
 *
 * Callers may rely on:
 *	- puts are idempotent;
 *	- a chunk referenced by any root visible to the caller's snapshot can be
 *	  read;
 *	- a chunk put in a transaction is durable once that transaction commits;
 *	- the bytes returned for a hash are the bytes that were put under it.
 *
 * Callers must not rely on whether a put disappears on rollback, whether a put
 * waits for a concurrent inserter, whether other running transactions' puts are
 * visible, or how a store locks or stores anything. Implementations differ in
 * all of these.
 *
 * A store handle and the pins taken through it last at most one transaction.
 *
 * src/include/access/doltchunk.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DOLTCHUNK_H
#define DOLTCHUNK_H

/* The prolly hash size, PL_HASH_SIZE: SHA-256 truncated, as Dolt's addresses are. */
#define DOLT_CHUNK_HASH_SIZE 20

typedef enum DoltChunkIntent
{
	DOLT_CHUNK_READ,
	DOLT_CHUNK_WRITE,
} DoltChunkIntent;

typedef struct DoltChunkStore DoltChunkStore;

/* A chunk returned by a get. data is NULL if the store has no such chunk. */
typedef struct DoltChunk
{
	const uint8 *data;
	Size		length;
	int			pin;			/* private to the store layer */
} DoltChunk;

/* Called once per chunk by a scan; return false to stop the scan. */
typedef bool (*DoltChunkScanFn) (void *arg, const uint8 *hash, Size length);

typedef struct DoltChunkStoreStats
{
	double		chunks;			/* estimate */
	uint64		bytes;			/* storage used, including any index */
} DoltChunkStoreStats;

extern PGDLLIMPORT bool dolt_chunk_verify_hashes;

extern DoltChunkStore *dolt_chunk_open(DoltChunkIntent intent);
extern void dolt_chunk_close(DoltChunkStore *store);

/*
 * Look up n chunks. Each found chunk is pinned: its bytes stay valid until
 * dolt_chunk_release, dolt_chunk_close or transaction end, whichever is first.
 */
extern void dolt_chunk_get_many(DoltChunkStore *store, int n, const uint8 *hashes, DoltChunk *chunks);
extern bool dolt_chunk_get(DoltChunkStore *store, const uint8 *hash, DoltChunk *chunk);
extern void dolt_chunk_release(DoltChunkStore *store, DoltChunk *chunk);

extern void dolt_chunk_exists_many(DoltChunkStore *store, int n, const uint8 *hashes, bool *found);
extern bool dolt_chunk_exists(DoltChunkStore *store, const uint8 *hash);

/*
 * Put n chunks, given hashes and contents; returns how many this call stored.
 * The store may reorder the batch. Requires DOLT_CHUNK_WRITE.
 */
extern int	dolt_chunk_put_many(DoltChunkStore *store, int n, const uint8 *hashes,
								const uint8 *const *data, const Size *lengths);
extern bool dolt_chunk_put(DoltChunkStore *store, const uint8 *hash, const uint8 *data, Size length);

/* Garbage-collection primitives. Deleting requires DOLT_CHUNK_WRITE. */
extern void dolt_chunk_scan(DoltChunkStore *store, DoltChunkScanFn fn, void *arg);
extern int	dolt_chunk_delete_many(DoltChunkStore *store, int n, const uint8 *hashes);

extern Size dolt_chunk_max_size(void);
extern void dolt_chunk_store_stats(DoltChunkStoreStats *stats);

/*
 * Store implementations.
 *
 * The layer above an implementation supplies pins, hash verification and
 * size checks. An implementation only stores and finds bytes.
 * Its get passes each found chunk's bytes to the callback, valid only during
 * that call. Hashes are packed: hash i is at hashes + i * DOLT_CHUNK_HASH_SIZE.
 */
typedef void (*DoltChunkFoundFn) (void *arg, int index, const uint8 *data, Size length);

typedef struct DoltChunkStoreRoutine
{
	const char *name;
	Size		(*max_size) (void);
	void	   *(*open) (DoltChunkIntent intent);
	void		(*close) (void *state);
	void		(*get) (void *state, int n, const uint8 *hashes, DoltChunkFoundFn found, void *arg);
	void		(*exists) (void *state, int n, const uint8 *hashes, bool *found);
	int			(*put) (void *state, int n, const uint8 *hashes, const uint8 *const *data, const Size *lengths);
	void		(*scan) (void *state, DoltChunkScanFn fn, void *arg);
	int			(*remove) (void *state, int n, const uint8 *hashes);
	void		(*stats) (DoltChunkStoreStats *stats);
} DoltChunkStoreRoutine;

/* The built-in implementation: the pg_dolt_chunk heap catalog. */
extern const DoltChunkStoreRoutine *dolt_chunk_heap_routine(void);

/*
 * Install an implementation for this backend, or NULL for the built-in one;
 * returns the previous one. No store may be open.
 */
extern const DoltChunkStoreRoutine *dolt_chunk_set_routine(const DoltChunkStoreRoutine *routine);

#endif							/* DOLTCHUNK_H */
