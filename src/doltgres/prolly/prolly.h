/* Standalone prolly tree prototype. See README.md for format and ownership. */
#ifndef DOLTGRES_PROLLY_H
#define DOLTGRES_PROLLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Every content hash: SHA-256 truncated to this many bytes, as Dolt's
 * addresses are. Nodes, rows and version objects are all addressed by it.
 */
#define PL_NODE_FORMAT "DGPTR001"
#define PL_HASH_SIZE 20
#define PL_HASH_TEXT_SIZE 32
#define PL_SHA256_SIZE 32
#define PL_MAX_ITEM (16U * 1024U * 1024U)

typedef enum PLStatus
{
	PL_OK, PL_NOT_FOUND, PL_INVALID, PL_NOMEM, PL_CORRUPT, PL_CONFLICT, PL_STOP, PL_IO, PL_BUSY
} PLStatus;

typedef struct PLSlice
{
	const uint8_t *data;
	size_t		len;
} PLSlice;

typedef struct PLValue
{
	bool		present;		/* false = absent/deleted, not an empty value */
	PLSlice		bytes;
} PLValue;

typedef struct PLHash
{
	uint8_t		bytes[PL_HASH_SIZE];
} PLHash;

void pl_hash_format(PLHash hash, char out[PL_HASH_TEXT_SIZE + 1]);
bool pl_hash_parse(const char *text, PLHash *out);

typedef struct PLStore PLStore;
typedef struct PLTree
{
	PLStore    *store;
	PLHash		root;
} PLTree;

typedef struct PLEntry
{
	PLSlice		key;
	PLSlice		value;
} PLEntry;

typedef struct PLEdit
{
	PLSlice		key;
	PLValue		value;
} PLEdit;

typedef struct PLStats
{
	size_t		chunks;
	size_t		bytes;
	uint64_t	writes;
	uint64_t	reused;
	uint64_t	skipped;
	uint64_t	fetched;
} PLStats;

typedef PLStatus (*PLScanFn) (void *arg, PLSlice key, PLSlice value);
typedef PLStatus (*PLDiffFn) (void *arg, PLSlice key, PLValue before, PLValue after);

/* Resolver output is copied immediately; input values are borrowed from stores. */
typedef PLStatus (*PLResolveFn) (void *arg, PLSlice key, PLValue base,
								 PLValue left, PLValue right, PLValue *result);

/* Called before a new chunk can reach disk. A WAL adapter must insert AND
 * flush its chunk record before returning PL_OK. Never reenter this store.
 * Replay bypasses this callback; duplicate chunks do not invoke it.
 */
typedef PLStatus (*PLBeforeWriteFn) (void *arg, PLHash hash, PLSlice bytes);

/* Called when a node is missing from a store, to load it from a backing store.
 * On PL_OK, *bytes must hold the node's encoding until the callback's next call;
 * the store copies them, trusting the encoding as its own: only imports are
 * checked. Return PL_NOT_FOUND if the backing store lacks it.
 */
typedef PLStatus (*PLFetchFn) (void *arg, PLHash hash, PLSlice *bytes);

/* File open loads all chunks, locks exclusively, and leaves out unchanged on error.
 * repair_tail permits truncating an incomplete final record for subsequent replay.
 * sync is explicit; free does not promise durability. Root refs are external.
 */
PLStatus	pl_store_open_file(const char *path, bool repair_tail,
							   PLBeforeWriteFn before_write, void *arg, PLStore **out);
PLStatus	pl_store_sync(PLStore *store);
PLStatus	pl_store_replay(PLStore *store, PLHash hash, PLSlice bytes);

PLStore    *pl_store_create(void);
void		pl_store_free(PLStore *store);

/* Load missing nodes on demand. Fetched nodes are marked as backed. */
void		pl_store_set_fetch(PLStore *store, PLFetchFn fetch, void *arg);
size_t		pl_store_drop_unbacked(PLStore *store);

/* Free the nodes known to be in the backing store, which finds fetch again. */
size_t		pl_store_drop_backed(PLStore *store);
PLStats		pl_store_stats(const PLStore *store);

/*
 * Free every node not reachable from |roots| through nodes in this store, and
 * return how many were freed. Slices borrowed from freed nodes become invalid,
 * so callers must name every root whose values they still hold. Nodes that a
 * backing store holds can be fetched again later.
 */
size_t		pl_store_retain(PLStore *store, const PLHash *roots, size_t count);

/* Exported bytes are immutable and borrowed until store_free. */
PLStatus	pl_store_export(PLStore *store, PLHash hash, PLSlice *bytes);

/* Import checks hash and local encoding. pl_open also validates the whole DAG. */
PLStatus	pl_store_import(PLStore *store, PLHash hash, PLSlice bytes);
PLStatus	pl_open(PLStore *store, PLHash root, PLTree *out);

/*
 * As pl_open, for a tree imported in part over a backing store: the subtree
 * of a node the backing store holds is trusted once it fits its parent.
 */
PLStatus	pl_open_imported(PLStore *store, PLHash root, PLTree *out);
PLStatus	pl_validate(const PLTree *tree);

/* Bulk input and edits must be strictly ordered by unsigned byte comparison. */
PLStatus	pl_build(PLStore *store, const PLEntry *entries, size_t count, PLTree *out);
PLStatus	pl_apply(const PLTree *tree, const PLEdit *edits, size_t count, PLTree *out);

/* Branch is O(1); branches share store ownership, which remains with the caller. */
PLTree		pl_branch(const PLTree *tree);
PLStatus	pl_get(const PLTree *tree, PLSlice key, PLValue *out);

/* Ordered [lower, upper) scan; NULL bound = unbounded. Values are borrowed. */
PLStatus	pl_scan(const PLTree *tree, const PLSlice *lower, const PLSlice *upper,
					PLScanFn callback, void *arg);
PLStatus	pl_diff(const PLTree *before, const PLTree *after, PLDiffFn callback, void *arg);

/* Without a resolver, divergent changes return PL_CONFLICT. Output unchanged on error. */
PLStatus	pl_merge(const PLTree *base, const PLTree *left, const PLTree *right,
					 PLResolveFn resolver, void *arg, PLTree *out);
uint64_t	pl_count(const PLTree *tree);
int			pl_compare(PLSlice a, PLSlice b);
bool		pl_hash_equal(PLHash a, PLHash b);
const char *pl_status_string(PLStatus status);

/* Schema-driven tuple payload: null bitmap, offsets, concatenated attribute bytes.
 * Fields must already be canonical for their types. No OIDs/XIDs/padding are stored.
 * Pack allocates *buffer with malloc; caller frees it. Get returns a borrowed slice.
 */
PLStatus	pl_tuple_pack(const PLValue *fields, size_t count, uint8_t **buffer, size_t *length);
/* A validated view borrows immutable tuple bytes for its entire lifetime. */
typedef struct PLTupleView
{
	PLSlice bytes;
	size_t count;
	size_t header;
} PLTupleView;
PLStatus pl_tuple_open(PLSlice tuple, PLTupleView *view);
PLStatus pl_tuple_field(const PLTupleView *view, size_t attribute, PLValue *out);
PLStatus	pl_tuple_get(PLSlice tuple, size_t attribute, PLValue *out);
void		pl_key_int64(int64_t value, uint8_t out[8]);

#endif
