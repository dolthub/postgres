/*-------------------------------------------------------------------------
 *
 * dg_root.c
 *	  Database roots: a database state's members, as chunks read in place.
 *
 * A root is one chunk in the chunk store, named by its hash like any tree
 * node, holding every member of a database state (tables, indexes,
 * namespaces, routines and the rest) sorted by key, the kind and qualified
 * name. Readers find a member by binary search over the chunk's bytes, with
 * nothing to parse. A root too large for a page spills into leaves under
 * index nodes, cut where key hashes say, so that the same members always
 * make the same chunks and one change rewrites one leaf and its path. See
 * DATABASE-ROOT-PLAN.md for the layout.
 *
 * Code that once passed member objects passes member references: an entry
 * encoded on its own, equal for equal members.
 *
 * src/backend/access/dolt_prolly/dg_root.c
 *
 *-------------------------------------------------------------------------
 */
#include "dg.h"
#include "access/doltchunk.h"
#include "access/doltnode.h"
#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "access/xact.h"
#include "storage/proc.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#define ROOT_MAGIC "DGR1"
#define ROOT_HEADER 16
#define LEAF_STRIDE 52
#define INDEX_STRIDE 32
#define ROOT_BUDGET 8000		/* bytes a node may take: one chunk part */
#define ROOT_MINIMUM 2000		/* bytes a spilled node holds before a key may end it */
#define ROOT_PATTERN 32			/* a key whose hash is 0 mod this ends a node */
#define ROOT_MAX_LEVEL 8

#define TAG_ENCODING 1
#define TAG_EXTENSIONS 2		/* extension names, each ending in NUL */

#define FLAG_CONTENT 0x0001

#define REF_MAGIC 'M'
#define REF_VERSION 1
#define REF_HEADER 6			/* magic, version, flags, key length */

/* Reading little-endian integers in place, and writing them. */
static inline uint16
get16(const uint8 *p)
{
	return (uint16) p[0] | ((uint16) p[1] << 8);
}

static inline uint32
get32(const uint8 *p)
{
	return (uint32) p[0] | ((uint32) p[1] << 8) | ((uint32) p[2] << 16) | ((uint32) p[3] << 24);
}

static inline void
put16(uint8 *p, uint16 v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8;
}

static inline void
put32(uint8 *p, uint32 v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = v >> 24;
}

/* ---------------------------------------------------------------------------
 * Keys
 * ---------------------------------------------------------------------------
 */

/* |entry|'s key, kind NUL name, in |out|, which it allocates. */
static int
entry_key(const DGRootEntry *entry, uint8 **out)
{
	size_t		kind = strlen(entry->kind);
	size_t		name = strlen(entry->name);
	size_t		length = kind + 1 + name;

	if (length > PG_UINT16_MAX)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("database member name is too long")));
	*out = palloc(length);
	memcpy(*out, entry->kind, kind);
	(*out)[kind] = '\0';
	memcpy(*out + kind + 1, entry->name, name);
	return (int) length;
}

static int
key_compare(const uint8 *a, int alen, const uint8 *b, int blen)
{
	int			c = memcmp(a, b, Min(alen, blen));

	return c ? c : (alen > blen) - (alen < blen);
}

/* Split |key| into its kind and name. */
static void
key_split(const uint8 *key, int length, char **kind, char **name)
{
	const uint8 *nul = memchr(key, '\0', length);

	if (!nul)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("database root key has no kind")));
	*kind = pnstrdup((const char *) key, nul - key);
	*name = pnstrdup((const char *) nul + 1, length - (nul + 1 - key));
}

/* Whether a node ends after |key|, as its hash says. */
static bool
key_ends_node(const uint8 *key, int length)
{
	PLSlice		bytes = {key, length};
	PLHash		hash;

	if (!pl_hash_bytes(bytes, &hash))
		elog(ERROR, "database root key hash failed");
	return get32(hash.bytes) % ROOT_PATTERN == 0;
}

/* ---------------------------------------------------------------------------
 * Node bytes
 * ---------------------------------------------------------------------------
 */

/* Root nodes read in this backend, by hash: immutable, so kept until the cache fills. */
typedef struct CachedNode
{
	uint8		hash[PL_HASH_SIZE];
	Size		length;
	uint8	   *bytes;
} CachedNode;

#define NODE_CACHE_LIMIT (16 * 1024 * 1024)

static MemoryContext node_memory;
static HTAB *node_cache;
static Size node_cache_bytes;

/*
 * Start a reading or building operation: empty the cache if it has filled,
 * never in the middle of one, whose node pointers must stay valid.
 */
static void
node_cache_init(void)
{
	HASHCTL		ctl = {0};

	if (node_cache && node_cache_bytes < NODE_CACHE_LIMIT)
		return;
	if (node_memory)
		MemoryContextDelete(node_memory);
	node_memory = AllocSetContextCreate(TopMemoryContext, "database roots", ALLOCSET_DEFAULT_SIZES);
	node_cache_bytes = 0;
	ctl.keysize = PL_HASH_SIZE;
	ctl.entrysize = sizeof(CachedNode);
	ctl.hcxt = node_memory;
	node_cache = hash_create("database root nodes", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static CachedNode *
node_cache_enter(const uint8 *hash, const uint8 *bytes, Size length)
{
	bool		found;
	CachedNode *node = hash_search(node_cache, hash, HASH_ENTER, &found);

	if (!found)
	{
		node->length = length;
		node->bytes = MemoryContextAlloc(node_memory, Max(length, 1));
		memcpy(node->bytes, bytes, length);
		node_cache_bytes += length + sizeof(CachedNode);
	}
	return node;
}

/* Check that |bytes| hold a node: the header, slots and offsets in bounds. */
static void
node_check(const uint8 *bytes, Size length)
{
	uint16		stride;
	uint32		count;
	uint32		extension;

	if (length < ROOT_HEADER || memcmp(bytes, ROOT_MAGIC, 4) != 0)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("chunk is not a database root")));
	stride = get16(bytes + 6);
	count = get32(bytes + 8);
	extension = get32(bytes + 12);
	if (stride < (bytes[4] ? INDEX_STRIDE : LEAF_STRIDE) || bytes[4] > ROOT_MAX_LEVEL ||
		(Size) ROOT_HEADER + (Size) stride * count > length || extension >= length)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("database root node is malformed")));
	for (uint32 i = 0; i < count; i++)
	{
		const uint8 *slot = bytes + ROOT_HEADER + (Size) stride * i;

		if ((Size) get32(slot) + get16(slot + 4) > length ||
			(!bytes[4] && get32(slot + 48) >= length))
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("database root node is malformed")));
	}
}

/* The node at |hash|, from this backend's cache, the shared node cache, or the chunk store. */
static CachedNode *
node_read(const uint8 *hash)
{
	CachedNode *node;
	const uint8 *shared;
	Size		length;

	node = hash_search(node_cache, hash, HASH_FIND, NULL);
	if (node)
		return node;
	shared = dolt_node_cache_get(hash, &length);
	if (!shared)
	{
		DoltChunkStore *chunks = dolt_chunk_open(DOLT_CHUNK_READ);
		DoltChunk	chunk;

		if (!dolt_chunk_get(chunks, hash, &chunk))
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("database root is missing from the chunk store")));
		node_check(chunk.data, chunk.length);
		dolt_node_cache_add(hash, chunk.data, chunk.length);
		node = node_cache_enter(hash, chunk.data, chunk.length);
		dolt_chunk_release(chunks, &chunk);
		dolt_chunk_close(chunks);
		return node;
	}
	node_check(shared, length);
	return node_cache_enter(hash, shared, length);
}

static const uint8 *
root_hash(bytea *root)
{
	if (VARSIZE_ANY_EXHDR(root) != PL_HASH_SIZE)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid database root address")));
	return (const uint8 *) VARDATA_ANY(root);
}

static inline const uint8 *
slot_at(const CachedNode *node, uint32 i)
{
	return node->bytes + ROOT_HEADER + (Size) get16(node->bytes + 6) * i;
}

static inline uint32
node_count(const CachedNode *node)
{
	return get32(node->bytes + 8);
}

/* Fill |out| from leaf slot |slot| of |node|. */
static void
leaf_entry(const CachedNode *node, const uint8 *slot, DGRootEntry *out)
{
	key_split(node->bytes + get32(slot), get16(slot + 4), &out->kind, &out->name);
	memcpy(out->schema.bytes, slot + 8, PL_HASH_SIZE);
	out->has_content = (get16(slot + 6) & FLAG_CONTENT) != 0;
	memcpy(out->content.bytes, slot + 28, PL_HASH_SIZE);
}

/* ---------------------------------------------------------------------------
 * Reading
 * ---------------------------------------------------------------------------
 */

/*
 * The member of |kind| named |name| in |root|, into |out|: false if there is
 * none. A binary search per level, on the nodes' bytes as stored.
 */
bool
dg_root_find(bytea *root, const char *kind, const char *name, DGRootEntry *out)
{
	DGRootEntry probe = {.kind = (char *) kind, .name = (char *) name};
	uint8	   *key;
	int			length = entry_key(&probe, &key);
	CachedNode *node;

	node_cache_init();
	node = node_read(root_hash(root));

	for (;;)
	{
		uint32		low = 0,
					high = node_count(node);

		/* The first slot whose key is at least |key|. */
		while (low < high)
		{
			uint32		mid = low + (high - low) / 2;
			const uint8 *slot = slot_at(node, mid);

			if (key_compare(node->bytes + get32(slot), get16(slot + 4), key, length) < 0)
				low = mid + 1;
			else
				high = mid;
		}
		if (low == node_count(node))
			return false;
		if (node->bytes[4] == 0)
		{
			const uint8 *slot = slot_at(node, low);

			if (key_compare(node->bytes + get32(slot), get16(slot + 4), key, length) != 0)
				return false;
			if (out)
				leaf_entry(node, slot, out);
			return true;
		}
		node = node_read(slot_at(node, low) + 8);
	}
}

static void
collect(CachedNode *node, DGRootEntry **entries, int *count, int *capacity)
{
	uint32		n = node_count(node);

	check_stack_depth();
	for (uint32 i = 0; i < n; i++)
	{
		const uint8 *slot = slot_at(node, i);

		if (node->bytes[4])
		{
			uint8		child[PL_HASH_SIZE];

			/* The read may move the cache; the parent's bytes stay put. */
			memcpy(child, slot + 8, PL_HASH_SIZE);
			collect(node_read(child), entries, count, capacity);
			continue;
		}
		if (*count == *capacity)
		{
			*capacity *= 2;
			*entries = repalloc(*entries, sizeof(DGRootEntry) * *capacity);
		}
		leaf_entry(node, slot, &(*entries)[(*count)++]);
	}
}

/* Every member of |root|, in key order. */
DGRootEntry *
dg_root_entries(bytea *root, int *count)
{
	int			capacity = 16;
	DGRootEntry *entries = palloc(sizeof(DGRootEntry) * capacity);

	*count = 0;
	node_cache_init();
	collect(node_read(root_hash(root)), &entries, count, &capacity);
	return entries;
}

/* The tagged field |tag| of the top node of |root|, or NULL. */
static char *
root_field(bytea *root, uint8 tag)
{
	CachedNode *node;
	uint32		at;

	node_cache_init();
	node = node_read(root_hash(root));
	at = get32(node->bytes + 12);

	if (!at)
		return NULL;
	while (at + 3 <= node->length && node->bytes[at] != 0)
	{
		uint16		length = get16(node->bytes + at + 1);

		if (at + 3 + length > node->length)
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("database root node is malformed")));
		if (node->bytes[at] == tag)
			return pnstrdup((const char *) node->bytes + at + 3, length);
		at += 3 + length;
	}
	return NULL;
}

/* Check that |root| names a database root the chunk store holds. */
void
dg_root_check(bytea *root)
{
	DoltChunkStore *chunks = dolt_chunk_open(DOLT_CHUNK_READ);
	bool		stored = dolt_chunk_exists(chunks, root_hash(root));

	dolt_chunk_close(chunks);
	if (!stored)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("database root is not in the chunk store")));
	node_cache_init();
	(void) node_read(root_hash(root));
}

/* The server encoding |root| was built under. */
char *
dg_root_encoding(bytea *root)
{
	return root_field(root, TAG_ENCODING);
}

/* The extensions |root|'s definitions use, by name, in order. */
List *
dg_root_extensions(bytea *root)
{
	CachedNode *node;
	uint32		at;
	List	   *result = NIL;

	node_cache_init();
	node = node_read(root_hash(root));
	at = get32(node->bytes + 12);
	while (at && at + 3 <= node->length && node->bytes[at] != 0)
	{
		uint16		length = get16(node->bytes + at + 1);

		if (at + 3 + length > node->length)
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("database root node is malformed")));
		if (node->bytes[at] == TAG_EXTENSIONS)
		{
			const char *p = (const char *) node->bytes + at + 3;
			const char *end = p + length;

			while (p < end)
			{
				size_t		n = strnlen(p, end - p);

				result = lappend(result, pnstrdup(p, n));
				p += n + 1;
			}
		}
		at += 3 + length;
	}
	return result;
}

static void
nodes_walk(bytea *root, void (*fn) (void *arg, const uint8 *hash, int level), void *arg)
{
	CachedNode *node = node_read(root_hash(root));
	uint8	   *children;
	uint32		n;

	check_stack_depth();
	fn(arg, root_hash(root), node->bytes[4]);
	if (!node->bytes[4])
		return;
	n = node_count(node);
	children = palloc((Size) n * PL_HASH_SIZE);
	for (uint32 i = 0; i < n; i++)
		memcpy(children + (Size) i * PL_HASH_SIZE, slot_at(node, i) + 8, PL_HASH_SIZE);
	for (uint32 i = 0; i < n; i++)
	{
		bytea	   *child = palloc(VARHDRSZ + PL_HASH_SIZE);

		SET_VARSIZE(child, VARHDRSZ + PL_HASH_SIZE);
		memcpy(VARDATA(child), children + (Size) i * PL_HASH_SIZE, PL_HASH_SIZE);
		nodes_walk(child, fn, arg);
		pfree(child);
	}
	pfree(children);
}

/* Call |fn| with the address and level of every node of |root|, parents first. */
void
dg_root_nodes(bytea *root, void (*fn) (void *arg, const uint8 *hash, int level), void *arg)
{
	node_cache_init();
	nodes_walk(root, fn, arg);
}

/* ---------------------------------------------------------------------------
 * Building
 * ---------------------------------------------------------------------------
 */

/* A slot to write: a member, or a child node. */
typedef struct RootItem
{
	uint8	   *key;
	int			length;
	const DGRootEntry *entry;	/* a member, or NULL for a child */
	PLHash		child;
	uint32		count;			/* members under the child */
} RootItem;

static int
item_compare(const void *a, const void *b)
{
	const RootItem *x = a;
	const RootItem *y = b;

	return key_compare(x->key, x->length, y->key, y->length);
}

/* The chunks a build wrote or found, to put together. */
typedef struct Built
{
	uint8	   *hashes;
	uint8	  **data;
	Size	   *lengths;
	int			count;
	int			capacity;
} Built;

static Size
item_cost(int level, const RootItem *item)
{
	return (level ? INDEX_STRIDE : LEAF_STRIDE) + item->length;
}

/* The top node's tagged fields: the encoding, then any extensions. */
typedef struct TopFields
{
	const char *encoding;
	List	   *extensions;
} TopFields;

static Size
top_size(const TopFields *top)
{
	Size		size = 3 + strlen(top->encoding) + 1;

	if (top->extensions)
	{
		size += 3;
		foreach_ptr(char, name, top->extensions)
			size += strlen(name) + 1;
	}
	return size;
}

/* Encode |items| as a node of |level|, with |top|'s fields if it is the top node. */
static PLHash
node_write(Built *built, int level, const RootItem *items, int n, const TopFields *top)
{
	Size		stride = level ? INDEX_STRIDE : LEAF_STRIDE;
	Size		length = ROOT_HEADER + stride * n;
	Size		extension = 0;
	Size		heap;
	uint8	   *bytes;
	PLSlice		slice;
	PLHash		hash;

	for (int i = 0; i < n; i++)
		length += items[i].length;
	if (top)
	{
		extension = length;
		length += top_size(top);
	}
	bytes = palloc0(length);
	memcpy(bytes, ROOT_MAGIC, 4);
	bytes[4] = level;
	put16(bytes + 6, stride);
	put32(bytes + 8, n);
	put32(bytes + 12, extension);
	heap = ROOT_HEADER + stride * n;
	for (int i = 0; i < n; i++)
	{
		uint8	   *slot = bytes + ROOT_HEADER + stride * i;

		put32(slot, heap);
		put16(slot + 4, items[i].length);
		memcpy(bytes + heap, items[i].key, items[i].length);
		heap += items[i].length;
		if (level)
		{
			memcpy(slot + 8, items[i].child.bytes, PL_HASH_SIZE);
			put32(slot + 28, items[i].count);
		}
		else
		{
			put16(slot + 6, items[i].entry->has_content ? FLAG_CONTENT : 0);
			memcpy(slot + 8, items[i].entry->schema.bytes, PL_HASH_SIZE);
			if (items[i].entry->has_content)
				memcpy(slot + 28, items[i].entry->content.bytes, PL_HASH_SIZE);
		}
	}
	if (top)
	{
		Size		at = extension;

		bytes[at] = TAG_ENCODING;
		put16(bytes + at + 1, strlen(top->encoding));
		memcpy(bytes + at + 3, top->encoding, strlen(top->encoding));
		at += 3 + strlen(top->encoding);
		if (top->extensions)
		{
			Size		start = at + 3;
			Size		p = start;

			bytes[at] = TAG_EXTENSIONS;
			foreach_ptr(char, name, top->extensions)
			{
				memcpy(bytes + p, name, strlen(name) + 1);
				p += strlen(name) + 1;
			}
			if (p - start > PG_UINT16_MAX)
				ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("database root lists too many extensions")));
			put16(bytes + at + 1, p - start);
			at = p;
		}
		bytes[at] = 0;
	}
	slice.data = bytes;
	slice.len = length;
	if (!pl_hash_bytes(slice, &hash))
		elog(ERROR, "database root hash failed");
	if (built->count == built->capacity)
	{
		built->capacity *= 2;
		built->hashes = repalloc(built->hashes, (Size) built->capacity * PL_HASH_SIZE);
		built->data = repalloc(built->data, built->capacity * sizeof(uint8 *));
		built->lengths = repalloc(built->lengths, built->capacity * sizeof(Size));
	}
	memcpy(built->hashes + (Size) built->count * PL_HASH_SIZE, hash.bytes, PL_HASH_SIZE);
	built->data[built->count] = bytes;
	built->lengths[built->count++] = length;
	return hash;
}

/*
 * Cut |items| of |level| into nodes, writing each, and return the items for
 * the level above in |parents|, or the root when they fit one node.
 */
static PLHash
level_write(Built *built, int level, RootItem *items, int n, const TopFields *top)
{
	Size		whole = ROOT_HEADER + top_size(top);
	RootItem	   *parents;
	int			nparents = 0;
	int			start = 0;
	Size		size = ROOT_HEADER;

	for (int i = 0; i < n; i++)
		whole += item_cost(level, &items[i]);
	if (whole <= ROOT_BUDGET || n <= 1)
		return node_write(built, level, items, n, top);
	if (level >= ROOT_MAX_LEVEL)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("database root is too large")));
	parents = palloc(sizeof(RootItem) * n);
	for (int i = 0; i < n; i++)
	{
		Size		cost = item_cost(level, &items[i]);
		bool		end;

		if (cost + ROOT_HEADER > ROOT_BUDGET)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("database member name is too long")));

		/* A node full before this item ends before it. */
		if (i > start && size + cost > ROOT_BUDGET)
		{
			RootItem	   *parent = &parents[nparents++];

			parent->child = node_write(built, level, items + start, i - start, NULL);
			parent->key = items[i - 1].key;
			parent->length = items[i - 1].length;
			parent->entry = NULL;
			parent->count = 0;
			for (int j = start; j < i; j++)
				parent->count += level ? items[j].count : 1;
			start = i;
			size = ROOT_HEADER;
		}
		size += cost;
		end = i == n - 1 || (size >= ROOT_MINIMUM && key_ends_node(items[i].key, items[i].length));
		if (end)
		{
			RootItem	   *parent = &parents[nparents++];

			parent->child = node_write(built, level, items + start, i + 1 - start, NULL);
			parent->key = items[i].key;
			parent->length = items[i].length;
			parent->entry = NULL;
			parent->count = 0;
			for (int j = start; j <= i; j++)
				parent->count += level ? items[j].count : 1;
			start = i + 1;
			size = ROOT_HEADER;
		}
	}
	return level_write(built, level + 1, parents, nparents, top);
}

/*
 * Roots this transaction wrote, with the subtransaction that wrote each, for
 * dg_root_creator. In TopTransactionContext, so forgotten with the
 * transaction, which |created_lxid| tells.
 */
typedef struct CreatedRoot
{
	uint8		hash[PL_HASH_SIZE];
	TransactionId xid;
} CreatedRoot;

static HTAB *created_roots;
static LocalTransactionId created_lxid = InvalidLocalTransactionId;

static HTAB *
created(bool make)
{
	if (created_lxid != MyProc->vxid.lxid)
	{
		created_roots = NULL;
		created_lxid = InvalidLocalTransactionId;
	}
	if (!created_roots && make)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = PL_HASH_SIZE;
		ctl.entrysize = sizeof(CreatedRoot);
		ctl.hcxt = TopTransactionContext;
		created_roots = hash_create("created database roots", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		created_lxid = MyProc->vxid.lxid;
	}
	return created_roots;
}

static void
note_created(const uint8 *hash)
{
	bool		found;
	CreatedRoot *entry = hash_search(created(true), hash, HASH_ENTER, &found);

	entry->xid = GetCurrentTransactionId();
}

/*
 * Whether database root |root| can be read, and, as object_creator does for
 * objects, the transaction that wrote it when that is this transaction (its
 * only copy, which rolls back with it), else InvalidTransactionId. Unlike an
 * object, a root needs no visibility check: its address comes only from a
 * visible ref or object, and its bytes are fixed by its hash. So a read
 * through the shared node cache serves, and costs no chunk store probe.
 */
bool
dg_root_creator(bytea *root, TransactionId *creator)
{
	CreatedRoot *entry = created(false) ? hash_search(created_roots, root_hash(root), HASH_FIND, NULL) : NULL;

	*creator = InvalidTransactionId;
	if (entry && TransactionIdIsCurrentTransactionId(entry->xid))
	{
		*creator = entry->xid;
		return true;
	}
	node_cache_init();
	(void) node_read(root_hash(root));
	return true;
}

/*
 * Build the root of |n| members |entries|, under server encoding |encoding|,
 * using |extensions|, sorted names: write the chunks the store lacks, and
 * return its address. Members must have distinct keys.
 */
bytea *
dg_root_build(const DGRootEntry *entries, int n, const char *encoding, List *extensions)
{
	TopFields	top = {encoding, extensions};
	RootItem	   *items = palloc(sizeof(RootItem) * Max(n, 1));
	Built		built = {0};
	PLHash		root;
	bytea	   *result;

	for (int i = 0; i < n; i++)
	{
		items[i].length = entry_key(&entries[i], &items[i].key);
		items[i].entry = &entries[i];
	}
	qsort(items, n, sizeof(RootItem), item_compare);
	for (int i = 1; i < n; i++)
		if (item_compare(&items[i - 1], &items[i]) == 0)
			ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT),
							errmsg("database root lists %s \"%s\" twice", items[i].entry->kind, items[i].entry->name)));
	built.capacity = 8;
	built.hashes = palloc((Size) built.capacity * PL_HASH_SIZE);
	built.data = palloc(built.capacity * sizeof(uint8 *));
	built.lengths = palloc(built.capacity * sizeof(Size));
	root = level_write(&built, 0, items, n, &top);

	/* Write the chunks the store lacks: a put never deduplicates. */
	{
		bool		root_written = false;

		DoltChunkStore *chunks = dolt_chunk_open(DOLT_CHUNK_WRITE);
		bool	   *found = palloc0(built.count * sizeof(bool));
		int			missing = 0;

		dolt_chunk_exists_many(chunks, built.count, built.hashes, found);
		for (int i = 0; i < built.count; i++)
		{
			if (found[i])
				continue;
			if (memcmp(built.hashes + (Size) i * PL_HASH_SIZE, root.bytes, PL_HASH_SIZE) == 0)
				root_written = true;
			memmove(built.hashes + (Size) missing * PL_HASH_SIZE, built.hashes + (Size) i * PL_HASH_SIZE, PL_HASH_SIZE);
			built.data[missing] = built.data[i];
			built.lengths[missing++] = built.lengths[i];
		}
		if (missing)
			dolt_chunk_put_many(chunks, missing, built.hashes, (const uint8 *const *) built.data, built.lengths);
		dolt_chunk_close(chunks);
		if (root_written)
			note_created(root.bytes);
	}
	result = palloc(VARHDRSZ + PL_HASH_SIZE);
	SET_VARSIZE(result, VARHDRSZ + PL_HASH_SIZE);
	memcpy(VARDATA(result), root.bytes, PL_HASH_SIZE);
	return result;
}

/* ---------------------------------------------------------------------------
 * Member references
 * ---------------------------------------------------------------------------
 */

/* |entry| encoded on its own: magic, version, flags, key length, key, schema, content. */
bytea *
dg_member_ref(const DGRootEntry *entry)
{
	uint8	   *key;
	int			length = entry_key(entry, &key);
	Size		size = REF_HEADER + length + 2 * PL_HASH_SIZE;
	bytea	   *ref = palloc0(VARHDRSZ + size);
	uint8	   *p = (uint8 *) VARDATA(ref);

	SET_VARSIZE(ref, VARHDRSZ + size);
	p[0] = REF_MAGIC;
	p[1] = REF_VERSION;
	put16(p + 2, entry->has_content ? FLAG_CONTENT : 0);
	put16(p + 4, length);
	memcpy(p + REF_HEADER, key, length);
	memcpy(p + REF_HEADER + length, entry->schema.bytes, PL_HASH_SIZE);
	if (entry->has_content)
		memcpy(p + REF_HEADER + length + PL_HASH_SIZE, entry->content.bytes, PL_HASH_SIZE);
	return ref;
}

/* Whether |value| is a member reference rather than an object's hash. */
bool
dg_is_member_ref(bytea *value)
{
	const uint8 *p = (const uint8 *) VARDATA_ANY(value);

	return VARSIZE_ANY_EXHDR(value) > REF_HEADER + 2 * PL_HASH_SIZE && p[0] == REF_MAGIC && p[1] == REF_VERSION;
}

/* Decode member reference |ref| into |out|. */
void
dg_member_decode(bytea *ref, DGRootEntry *out)
{
	const uint8 *p = (const uint8 *) VARDATA_ANY(ref);
	int			length;

	if (!dg_is_member_ref(ref))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid database member reference")));
	length = get16(p + 4);
	if ((Size) REF_HEADER + length + 2 * PL_HASH_SIZE != VARSIZE_ANY_EXHDR(ref))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid database member reference")));
	key_split(p + REF_HEADER, length, &out->kind, &out->name);
	memcpy(out->schema.bytes, p + REF_HEADER + length, PL_HASH_SIZE);
	out->has_content = (get16(p + 2) & FLAG_CONTENT) != 0;
	memcpy(out->content.bytes, p + REF_HEADER + length + PL_HASH_SIZE, PL_HASH_SIZE);
}

/* ---------------------------------------------------------------------------
 * SQL access, for tests and internal SQL
 * ---------------------------------------------------------------------------
 */

static bytea *
hash_value(const PLHash *hash)
{
	bytea	   *v = palloc(VARHDRSZ + PL_HASH_SIZE);

	SET_VARSIZE(v, VARHDRSZ + PL_HASH_SIZE);
	memcpy(VARDATA(v), hash->bytes, PL_HASH_SIZE);
	return v;
}

static void
hash_from(bytea *v, PLHash *hash)
{
	if (VARSIZE_ANY_EXHDR(v) != PL_HASH_SIZE)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("addresses must be %d bytes", PL_HASH_SIZE)));
	memcpy(hash->bytes, VARDATA_ANY(v), PL_HASH_SIZE);
}

/* dolt_root_put(kinds, names, schemas, contents): build a root of those members. */
Datum
dolt_root_put(PG_FUNCTION_ARGS)
{
	Datum	   *kinds,
			   *names,
			   *schemas,
			   *contents;
	bool	   *kindnulls,
			   *namenulls,
			   *schemanulls,
			   *contentnulls;
	int			n,
				nnames,
				nschemas,
				ncontents;
	DGRootEntry *entries;

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("database root functions require superuser")));
	deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(0), TEXTOID, &kinds, &kindnulls, &n);
	deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(1), TEXTOID, &names, &namenulls, &nnames);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(2), BYTEAOID, -1, false, TYPALIGN_INT, &schemas, &schemanulls, &nschemas);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(3), BYTEAOID, -1, false, TYPALIGN_INT, &contents, &contentnulls, &ncontents);
	if (nnames != n || nschemas != n || ncontents != n)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("member arrays must have equal lengths")));
	entries = palloc0(sizeof(DGRootEntry) * Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		if (kindnulls[i] || namenulls[i] || schemanulls[i])
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("members need a kind, name and schema")));
		entries[i].kind = TextDatumGetCString(kinds[i]);
		entries[i].name = TextDatumGetCString(names[i]);
		hash_from(DatumGetByteaPP(schemas[i]), &entries[i].schema);
		entries[i].has_content = !contentnulls[i];
		if (!contentnulls[i])
			hash_from(DatumGetByteaPP(contents[i]), &entries[i].content);
	}
	PG_RETURN_BYTEA_P(dg_root_build(entries, n, GetDatabaseEncodingName(), NIL));
}

/* dolt_root_members(root): the members of a root, in key order. */
Datum
dolt_root_members(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	DGRootEntry *entries;
	int			n;

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("database root functions require superuser")));
	InitMaterializedSRF(fcinfo, 0);
	entries = dg_root_entries(PG_GETARG_BYTEA_PP(0), &n);
	for (int i = 0; i < n; i++)
	{
		Datum		values[4];
		bool		nulls[4] = {false};

		values[0] = CStringGetTextDatum(entries[i].kind);
		values[1] = CStringGetTextDatum(entries[i].name);
		values[2] = PointerGetDatum(hash_value(&entries[i].schema));
		if (entries[i].has_content)
			values[3] = PointerGetDatum(hash_value(&entries[i].content));
		else
			nulls[3] = true;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

typedef struct NodeRows
{
	ReturnSetInfo *rsinfo;
} NodeRows;

static void
node_row(void *arg, const uint8 *hash, int level)
{
	NodeRows   *rows = arg;
	CachedNode *node = node_read(hash);
	PLHash		h;
	Datum		values[4];
	bool		nulls[4] = {false};

	memcpy(h.bytes, hash, PL_HASH_SIZE);
	values[0] = PointerGetDatum(hash_value(&h));
	values[1] = Int32GetDatum(level);
	values[2] = Int32GetDatum(node_count(node));
	values[3] = Int32GetDatum((int32) node->length);
	tuplestore_putvalues(rows->rsinfo->setResult, rows->rsinfo->setDesc, values, nulls);
}

/* dolt_root_nodes(root): each node of a root, parents first, with its level, slots and bytes. */
Datum
dolt_root_nodes(PG_FUNCTION_ARGS)
{
	NodeRows	rows;

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("database root functions require superuser")));
	InitMaterializedSRF(fcinfo, 0);
	rows.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	dg_root_nodes(PG_GETARG_BYTEA_PP(0), node_row, &rows);
	return (Datum) 0;
}

/* dolt_root_find(root, kind, name): one member's schema and content, by binary search. */
Datum
dolt_root_find(PG_FUNCTION_ARGS)
{
	DGRootEntry entry;
	TupleDesc	desc;
	Datum		values[2];
	bool		nulls[2] = {false};

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("database root functions require superuser")));
	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (!dg_root_find(PG_GETARG_BYTEA_PP(0), text_to_cstring(PG_GETARG_TEXT_PP(1)), text_to_cstring(PG_GETARG_TEXT_PP(2)), &entry))
		PG_RETURN_NULL();
	values[0] = PointerGetDatum(hash_value(&entry.schema));
	if (entry.has_content)
		values[1] = PointerGetDatum(hash_value(&entry.content));
	else
		nulls[1] = true;
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}
