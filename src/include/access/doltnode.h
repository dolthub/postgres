/*-------------------------------------------------------------------------
 *
 * doltnode.h
 *	  The shared prolly node cache.
 *
 * Every backend reads prolly nodes through one cache in shared memory, sized
 * by dolt_node_cache_size. A node's bytes are fixed by its hash, so entries
 * never need invalidation; readers get a copy. Holding a node is no claim
 * that the chunk store holds it: callers add only nodes read from, or
 * committed to, the store.
 *
 * src/include/access/doltnode.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DOLTNODE_H
#define DOLTNODE_H

typedef struct DoltNodeCacheStats
{
	uint64		hits;			/* this backend's */
	uint64		misses;			/* this backend's */
	uint64		added;
	uint64		evictions;
	uint64		rejected;		/* adds refused for want of space */
	uint64		entries;
	uint64		bytes;
	uint64		total_hits;		/* all backends', reported at transaction end */
	uint64		total_misses;
} DoltNodeCacheStats;

extern PGDLLIMPORT int dolt_node_cache_size;
extern PGDLLIMPORT int dolt_session_node_cache;

extern Size DoltNodeCacheShmemSize(void);
extern void DoltNodeCacheShmemInit(void);

extern const uint8 *dolt_node_cache_get(const uint8 *hash, Size *length);
extern void dolt_node_cache_add(const uint8 *hash, const uint8 *data, Size length);
extern void dolt_node_cache_report(void);
extern void dolt_node_cache_stats(DoltNodeCacheStats *stats);

#endif							/* DOLTNODE_H */
