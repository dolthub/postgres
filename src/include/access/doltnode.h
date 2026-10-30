/*-------------------------------------------------------------------------
 *
 * doltnode.h
 *	  The shared prolly node cache.
 *
 * Every backend reads prolly nodes through one cache in shared memory, sized
 * by dolt_node_cache_size. A node's bytes are fixed by its hash, so entries
 * never need invalidation; a borrowed node stays in place, unchanged, until
 * its borrower returns it. Holding a node is no claim that the chunk store
 * holds it: callers add only nodes read from, or committed to, the store.
 *
 * src/include/access/doltnode.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DOLTNODE_H
#define DOLTNODE_H

/* A borrowed node: its bytes stay valid until the token is returned. */
typedef struct DoltNodeLoan
{
	const uint8 *data;
	Size		length;
	uintptr_t	token;			/* nonzero */
} DoltNodeLoan;

typedef struct DoltNodeCacheStats
{
	uint64		hits;			/* this backend's */
	uint64		misses;			/* this backend's */
	uint64		borrowed;		/* this backend's loans outstanding */
	uint64		added;
	uint64		evictions;
	uint64		rejected;		/* adds refused for want of space */
	uint64		entries;
	uint64		bytes;
	uint64		pinned;			/* entries borrowed by any backend */
} DoltNodeCacheStats;

extern PGDLLIMPORT int dolt_node_cache_size;

extern Size DoltNodeCacheShmemSize(void);
extern void DoltNodeCacheShmemInit(void);

extern bool dolt_node_cache_borrow(const uint8 *hash, DoltNodeLoan *loan);
extern bool dolt_node_cache_add(const uint8 *hash, const uint8 *data, Size length,
								DoltNodeLoan *loan);
extern void dolt_node_cache_return(uintptr_t token);
extern void dolt_node_cache_stats(DoltNodeCacheStats *stats);

#endif							/* DOLTNODE_H */
