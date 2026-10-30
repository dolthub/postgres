#ifndef DOLTGRES_PROLLY_INTERNAL_H
#define DOLTGRES_PROLLY_INTERNAL_H
#include "prolly.h"
#include <stdlib.h>
#include <string.h>

#define PL_MAX_DEPTH 32
#define PL_MIN_BYTES 256

#define PL_MAX_ENTRIES 128
#ifndef PL_TARGET_BYTES
#define PL_TARGET_BYTES 1024
#endif
#define PL_MAX_BYTES (4 * PL_TARGET_BYTES)
#define PL_HEADER 24

/*
 * A node's directory offsets take two bytes when its keys and values each fit
 * in 64 kB, as all but nodes holding very large values do, else four. Builders
 * charge every entry the narrow width.
 */
#define PL_NARROW_LIMIT 0xFFFF
#define PL_ENTRY_OVERHEAD 4

/* A child reference in an interior node: the child's address, then its row count as a varint. */
#define PL_CHILD_MAX (PL_HASH_SIZE + 10)

typedef struct PLNode
{
	PLHash		hash;
	uint8_t    *data;
	size_t		size;
	uint32_t	level;
	uint32_t	count;
	uint32_t	key_bytes;
	uint32_t	value_bytes;
	uint8_t		offset_width;	/* bytes per directory offset, from the sizes above */
	bool		backed;			/* known to be in the backing store */
	bool		marked;			/* reachable, during pl_store_retain */
	uint32_t	pins;			/* holders keeping it until written, across pl_store_drop_unbacked */
	size_t		unbacked_slot;	/* position in the store's unbacked list, while listed */
	struct PLNode *next;
} PLNode;

struct PLStore
{
	PLNode	  **buckets;
	size_t		capacity;
	PLNode	  **unbacked;		/* nodes added while not backed */
	size_t		unbacked_count;
	size_t		unbacked_capacity;
	PLStats		stats;
	int			fd;
	bool		loading;
	bool		replaying;
	bool		failed;
	PLBeforeWriteFn before_write;
	void	   *write_arg;
	PLFetchFn	fetch;
	void	   *fetch_arg;
};

PLStatus	pl_file_append(PLStore *store, PLHash hash, PLSlice bytes);
uint32_t	pl_read32(const uint8_t *p);
uint64_t	pl_read64(const uint8_t *p);
void		pl_write32(uint8_t *p, uint32_t v);
void		pl_write64(uint8_t *p, uint64_t v);
bool		pl_hash_bytes(PLSlice bytes, PLHash *out);
bool		pl_node_address(PLSlice bytes, PLHash *out);
size_t		pl_child_encode(uint8_t *out, PLHash hash, uint64_t rows);
bool		pl_child_decode(PLSlice value, PLHash *hash, uint64_t *rows);
PLHash		pl_child_hash(const PLNode *node, uint32_t index);
uint64_t	pl_child_rows(const PLNode *node, uint32_t index);
PLNode	   *pl_find(PLStore *store, PLHash hash);
PLNode	   *pl_find_local(PLStore *store, PLHash hash);
void		pl_node_set_backed(PLNode *node);
void		pl_node_pin(PLStore *store, PLNode *node);
void		pl_node_unpin(PLStore *store, PLNode *node);
PLSlice		pl_node_key(const PLNode *node, uint32_t index);
PLSlice		pl_node_value(const PLNode *node, uint32_t index);
uint64_t	pl_node_rows(const PLNode *node);
PLStatus	pl_save(PLStore *store, uint32_t level, const PLEntry *entries,
					uint32_t count, PLNode **out);
bool		pl_boundary(uint32_t level, uint32_t count, size_t bytes, size_t entry_bytes, PLSlice key);
typedef struct PLCursor
{
	PLStore    *store;
	PLNode	   *path[PL_MAX_DEPTH];
	uint32_t	index[PL_MAX_DEPTH];
	uint32_t	depth;
	bool		valid;
} PLCursor;
PLStatus	pl_cursor_init(PLCursor *cursor, const PLTree *tree, const PLSlice *lower);
PLStatus	pl_cursor_next(PLCursor *cursor);
PLStatus	pl_cursor_skip_leaf(PLCursor *cursor);
bool		pl_cursor_at_subtree_start(const PLCursor *cursor, uint32_t depth);
PLStatus	pl_cursor_skip_subtree(PLCursor *cursor, uint32_t depth);
PLEntry		pl_cursor_entry(const PLCursor *cursor);
#endif
