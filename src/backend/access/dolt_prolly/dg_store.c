/*-------------------------------------------------------------------------
 *
 * dg_store.c
 *	  Prolly relation state: base trees in the chunk store, named by
 *	  pg_dolt_root, with the delta store's changes over them.
 *
 * Every tree node lives in the chunk store (access/doltchunk.h). Each
 * relation storage on each branch is one tree: a base, which one pg_dolt_root
 * row names, and the versions of the delta store (access/doltdelta.h) written
 * since. A snapshot selects a base row with ordinary MVCC, then adds the
 * versions it sees that the base does not hold (dg_view.c); a fold, which is
 * the only writer to replace a base, records in the row which versions its
 * tree holds.
 *
 * Writes go to the delta store. A command's batch of row and index edits is
 * appended there when the command ends, and a state a caller rebuilt whole,
 * as version control and DDL do, is appended as its difference from the
 * content it was loaded with. A tree with no root row yet gets one first:
 * new storage, or a branch's first write, holding the base readers had
 * been using. Commit does nothing more: the versions are already durable in
 * the WAL and visible by the transaction's xid.
 *
 * Within a transaction, nodes are built and read through a working store that
 * loads missing nodes from the chunk store on demand.
 *
 * Handles, which serve as TIDs, are derived from row content, with a
 * per-storage counter in shared memory for the rest (dg_handle.inc).
 *
 * src/backend/access/dolt_prolly/dg_store.c
 *
 *-------------------------------------------------------------------------
 */
#include "dg.h"
#include "access/doltversion.h"
#include "nodes/tidbitmap.h"
#include "access/doltchunk.h"
#include "access/doltdelta.h"
#include "access/doltnode.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/parallel.h"
#include "access/twophase.h"
#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/catalogview.h"
#include "catalog/indexing.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_dolt_root.h"
#include "catalog/index.h"
#include "catalog/pg_dolt_handle.h"
#include "catalog/pg_index.h"
#include "utils/syscache.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "access/doltbranch.h"
#include "access/doltcatalog.h"
#include "access/doltsnapshot.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/dsm_registry.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "common/hashfn.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"

void
dg_unsupported(const char *operation)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("prolly phase 2 does not support %s", operation)));
	pg_unreachable();
}
void
dg_check(PLStatus status)
{
	if (status != PL_OK)
		ereport(ERROR, (errmsg("prolly storage: %s", pl_status_string(status))));
}

/* ---------------------------------------------------------------------------
 * Roots
 * ---------------------------------------------------------------------------
 */

/* Which committed roots: one relation storage on one branch. */
typedef struct RootKey
{
	Oid			space;
	RelFileNumber file;
	char		branch[NAMEDATALEN * 2];
} RootKey;

/*
 * A committed base: its tree's root, and which delta store versions the tree
 * holds. The horizon's xip lives with the Roots' reader.
 */
typedef struct Roots
{
	bool		present;
	PLHash		content;
	DoltDeltaHorizon horizon;
} Roots;

static RootKey
root_key_of(Oid space, RelFileNumber file, const char *branch)
{
	RootKey		key = {0};

	if (strlen(branch) >= sizeof(key.branch))
		ereport(ERROR, (errcode(ERRCODE_NAME_TOO_LONG), errmsg("branch name is too long for prolly storage")));
	key.space = space;
	key.file = file;
	strlcpy(key.branch, branch, sizeof(key.branch));
	return key;
}

static RootKey
root_key(Relation rel, const char *branch)
{
	return root_key_of(rel->rd_locator.spcOid, rel->rd_locator.relNumber, branch);
}

static void
root_scan_keys(const RootKey *key, ScanKeyData *keys)
{
	ScanKeyInit(&keys[0], Anum_pg_dolt_root_rootspace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(key->space));
	ScanKeyInit(&keys[1], Anum_pg_dolt_root_rootfile, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(key->file));
	ScanKeyInit(&keys[2], Anum_pg_dolt_root_rootbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(key->branch));
}

static void
hash_column(HeapTuple tuple, TupleDesc desc, AttrNumber attribute, bool *present, PLHash *hash)
{
	bool		isnull;
	Datum		value = heap_getattr(tuple, attribute, desc, &isnull);
	bytea	   *bytes;

	*present = !isnull;
	if (isnull)
		return;
	bytes = DatumGetByteaPP(value);
	if (VARSIZE_ANY_EXHDR(bytes) != PL_HASH_SIZE)
		elog(ERROR, "invalid prolly root hash");
	memcpy(hash->bytes, VARDATA_ANY(bytes), PL_HASH_SIZE);
}

static Roots
roots_from_tuple(HeapTuple tuple, TupleDesc desc)
{
	Roots		roots = {.present = true};
	Form_pg_dolt_root form = (Form_pg_dolt_root) GETSTRUCT(tuple);
	bool		present;
	bool		isnull;
	Datum		xip;

	hash_column(tuple, desc, Anum_pg_dolt_root_rootcontent, &present, &roots.content);
	if (!present)
		elog(ERROR, "invalid prolly root row");
	roots.horizon.xmin = form->rootxmin;
	roots.horizon.xmax = form->rootxmax;
	xip = heap_getattr(tuple, Anum_pg_dolt_root_rootxip, desc, &isnull);
	if (!isnull)
	{
		ArrayType  *array = DatumGetArrayTypeP(xip);

		roots.horizon.xcnt = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));
		roots.horizon.xip = palloc(Max(roots.horizon.xcnt, 1) * sizeof(TransactionId));
		memcpy(roots.horizon.xip, ARR_DATA_PTR(array), roots.horizon.xcnt * sizeof(TransactionId));
	}
	return roots;
}

/* |roots| with its horizon copied into |memory|. */
static Roots
roots_copy(const Roots *roots, MemoryContext memory)
{
	Roots		copy = *roots;

	copy.horizon.xip = NULL;
	if (roots->horizon.xcnt)
	{
		copy.horizon.xip = MemoryContextAlloc(memory, roots->horizon.xcnt * sizeof(TransactionId));
		memcpy(copy.horizon.xip, roots->horizon.xip, roots->horizon.xcnt * sizeof(TransactionId));
	}
	return copy;
}

/*
 * The latest committed roots of each key this backend has read, with the
 * generation they were read under and the transaction that wrote them. While
 * the generation stands, no roots have been published, so the row is still
 * the latest; a snapshot that sees its writer sees it.
 */
typedef struct RootsCacheEntry
{
	RootKey		key;
	uint64		generation;
	TransactionId xmin;
	Roots		roots;
} RootsCacheEntry;

static HTAB *roots_cache;
static uint64 roots_generation(const RootKey *key);

static RootsCacheEntry *
roots_cache_find(const RootKey *key, bool enter)
{
	if (!roots_cache)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(RootKey);
		ctl.entrysize = sizeof(RootsCacheEntry);
		ctl.hcxt = CacheMemoryContext;
		roots_cache = hash_create("prolly roots", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	{
		bool		found;
		RootsCacheEntry *entry = hash_search(roots_cache, key, enter ? HASH_ENTER : HASH_FIND, &found);

		if (entry && !found)
			memset(((char *) entry) + sizeof(RootKey), 0, sizeof(*entry) - sizeof(RootKey));
		return entry;
	}
}

/* The committed roots |snapshot| selects. */
static Roots
roots_read(const RootKey *key, Snapshot snapshot)
{
	uint64		generation = roots_generation(key);
	RootsCacheEntry *cached = roots_cache_find(key, false);
	Relation	catalog;
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	Roots		roots = {0};
	TransactionId xmin = InvalidTransactionId;
	bool		latest = false;

	if (cached && cached->generation == generation && cached->roots.present &&
		(!IsMVCCSnapshot(snapshot) || !XidInMVCCSnapshot(cached->xmin, snapshot)))
		return roots_copy(&cached->roots, CurrentMemoryContext);
	catalog = table_open(DoltRootRelationId, AccessShareLock);

	/* Callers such as foreign key checks may pass an unregistered snapshot. */
	snapshot = RegisterSnapshot(snapshot);
	root_scan_keys(key, keys);
	scan = systable_beginscan(catalog, DoltRootKeyIndexId, true, snapshot, 3, keys);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		roots = roots_from_tuple(tuple, RelationGetDescr(catalog));
		xmin = HeapTupleHeaderGetXmin(tuple->t_data);

		/* A version another transaction has replaced is not the latest. */
		latest = (tuple->t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask);
	}
	systable_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(catalog, AccessShareLock);

	/*
	 * Remember a committed row that is the latest version. A row of this
	 * transaction's own is visible by command, which the cache cannot tell;
	 * an older snapshot may select a version a later commit replaced, which
	 * its xmax shows.
	 */
	if (roots.present && latest && TransactionIdIsValid(xmin) && !TransactionIdIsCurrentTransactionId(xmin) &&
		IsMVCCSnapshot(snapshot) && !XidInMVCCSnapshot(xmin, snapshot) && generation == roots_generation(key))
	{
		RootsCacheEntry *entry = roots_cache_find(key, true);

		if (entry->roots.present && entry->roots.horizon.xip)
			pfree(entry->roots.horizon.xip);
		entry->generation = generation;
		entry->xmin = xmin;
		entry->roots = roots_copy(&roots, CacheMemoryContext);
	}
	return roots;
}

/* The latest committed roots, whatever the caller's snapshot. */
static Roots
roots_latest(const RootKey *key)
{
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	Roots		roots = roots_read(key, snapshot);

	UnregisterSnapshot(snapshot);
	return roots;
}

/* ---------------------------------------------------------------------------
 * The transaction's working store and chunk store handle
 * ---------------------------------------------------------------------------
 */

static PLStore *working_store;
StaticAssertDecl(PL_HASH_SIZE == DOLT_CHUNK_HASH_SIZE, "prolly and chunk store hashes differ in size");

static uint64 store_epoch = 1;	/* moves when the working store is recreated */

static DoltChunkStore *working_chunks;
static DoltChunk working_fetched;

/* Nodes this transaction wrote, to share once it commits. */
static PLHash *written;
static int	written_count;
static int	written_capacity;

/* Open the read handle under the top transaction, so subtransaction rollback
 * cannot close it underneath the working store. */
static DoltChunkStore *
working_chunk_store(void)
{
	if (!working_chunks)
	{
		ResourceOwner owner = CurrentResourceOwner;
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

		CurrentResourceOwner = TopTransactionResourceOwner;
		PG_TRY();
		{
			working_chunks = dolt_chunk_open(DOLT_CHUNK_READ);
		}
		PG_FINALLY();
		{
			CurrentResourceOwner = owner;
			MemoryContextSwitchTo(old);
		}
		PG_END_TRY();
	}
	return working_chunks;
}

static PLStatus
fetch_chunk(void *arg, PLHash hash, PLSlice *bytes)
{
	DoltChunkStore *chunks = working_chunk_store();
	ResourceOwner owner = CurrentResourceOwner;
	bool		found;

	/* The store copies each node before the next fetch. */
	CurrentResourceOwner = TopTransactionResourceOwner;
	PG_TRY();
	{
		if (working_fetched.pin)
			dolt_chunk_release(chunks, &working_fetched);
		found = dolt_chunk_get(chunks, hash.bytes, &working_fetched);
	}
	PG_FINALLY();
	{
		CurrentResourceOwner = owner;
	}
	PG_END_TRY();
	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly chunk is missing from the chunk store")));
	bytes->data = working_fetched.data;
	bytes->len = working_fetched.length;
	return PL_OK;
}

static void register_callbacks(void);

/* Read node |hash| through the shared node cache, adding it on a miss. */
static PLStatus
fetch_node(void *arg, PLHash hash, PLSlice *bytes)
{
	Size		length;
	const uint8 *cached = dolt_node_cache_get(hash.bytes, &length);

	if (cached)
	{
		bytes->data = cached;
		bytes->len = length;
		return PL_OK;
	}
	fetch_chunk(arg, hash, bytes);
	dolt_node_cache_add(hash.bytes, bytes->data, bytes->len);
	return PL_OK;
}

PLStore *
dg_working_store(void)
{
	if (!working_store)
	{
		register_callbacks();
		working_store = pl_store_create();
		if (!working_store)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory creating prolly store")));
		pl_store_set_fetch(working_store, fetch_node, NULL);
		store_epoch++;
	}
	return working_store;
}

/* A store of the caller's own that reads nodes as the working store does. */
PLStore *
dg_private_store(void)
{
	PLStore    *store;

	/* Transaction end closes the chunk store that misses read. */
	register_callbacks();
	store = pl_store_create();
	if (!store)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory creating prolly store")));
	pl_store_set_fetch(store, fetch_node, NULL);
	return store;
}

/* Share the nodes this transaction wrote, now that it has committed. */
static void
share_written(void)
{
	for (int i = 0; working_store && i < written_count; i++)
	{
		PLNode	   *node = pl_find_local(working_store, written[i]);

		if (node)
			dolt_node_cache_add(node->hash.bytes, node->data, node->size);
	}
}

static void
working_close(bool commit)
{
	if (working_chunks)
	{
		if (commit)
		{
			ResourceOwner owner = CurrentResourceOwner;

			/* The handle and its pins belong to the top transaction. */
			CurrentResourceOwner = TopTransactionResourceOwner;
			if (working_fetched.pin)
				dolt_chunk_release(working_chunks, &working_fetched);
			dolt_chunk_close(working_chunks);
			CurrentResourceOwner = owner;
		}
		working_chunks = NULL;
	}
	memset(&working_fetched, 0, sizeof(working_fetched));
}

static PLTree
tree_at(PLHash root)
{
	PLTree		tree = {dg_working_store(), root};

	return tree;
}

/*
 * The empty tree, built once per working store and pinned so that dropping a
 * transaction's unwritten nodes keeps it; it is written like any other node
 * the first time a persisted root reaches it.
 */
static PLTree
empty_tree(void)
{
	static PLHash empty_root;
	static bool empty_known;
	PLStore    *store = dg_working_store();
	PLTree		tree;

	if (empty_known && pl_find_local(store, empty_root))
		return (PLTree) {store, empty_root};
	dg_check(pl_build(store, NULL, 0, &tree));
	pl_node_pin(store, pl_find_local(store, tree.root));
	empty_root = tree.root;
	empty_known = true;
	return tree;
}

/*
 * Write every node reachable from |root| that is not yet in the chunk store.
 * A node absent from the working store was never built or imported here, so it
 * came from the chunk store already; a fetched node is marked backed.
 */
typedef struct Pending
{
	PLNode	  **nodes;
	int			count;
	int			capacity;
} Pending;

static void
collect_unbacked(PLStore *store, PLHash root, Pending *pending)
{
	PLNode	   *node = pl_find_local(store, root);

	if (!node || node->backed)
		return;
	check_stack_depth();
	for (uint32 i = 0; node->level && i < node->count; i++)
	{
		PLHash		child;

		child = pl_child_hash(node, i);
		collect_unbacked(store, child, pending);
	}
	if (node->backed)
		return;					/* reached twice through shared subtrees */
	pl_node_set_backed(node);
	if (pending->count == pending->capacity)
	{
		pending->capacity = pending->capacity ? pending->capacity * 2 : 64;
		pending->nodes = pending->nodes ? repalloc(pending->nodes, pending->capacity * sizeof(PLNode *)) :
			palloc(pending->capacity * sizeof(PLNode *));
	}
	pending->nodes[pending->count++] = node;
}

static void
persist_roots(const Roots *roots)
{
	PLStore    *store = dg_working_store();
	Pending		pending = {0};

	if (!roots->present)
		return;
	collect_unbacked(store, roots->content, &pending);
	if (pending.count)
	{
		DoltChunkStore *chunks = dolt_chunk_open(DOLT_CHUNK_WRITE);
		uint8	   *hashes = palloc((Size) pending.count * DOLT_CHUNK_HASH_SIZE);
		const uint8 **data = palloc(pending.count * sizeof(uint8 *));
		Size	   *lengths = palloc(pending.count * sizeof(Size));

		for (int i = 0; i < pending.count; i++)
		{
			memcpy(hashes + (Size) i * DOLT_CHUNK_HASH_SIZE, pending.nodes[i]->hash.bytes, DOLT_CHUNK_HASH_SIZE);
			data[i] = pending.nodes[i]->data;
			lengths[i] = pending.nodes[i]->size;
		}
		dolt_chunk_put_many(chunks, pending.count, hashes, data, lengths);
		dolt_chunk_close(chunks);
		if (written_count + pending.count > written_capacity)
		{
			int			capacity = Max(written_count + pending.count, written_capacity * 2);

			written = written ? repalloc(written, capacity * sizeof(PLHash)) :
				MemoryContextAlloc(TopTransactionContext, capacity * sizeof(PLHash));
			written_capacity = capacity;
		}
		for (int i = 0; i < pending.count; i++)
			written[written_count++] = pending.nodes[i]->hash;
		pfree(hashes);
		pfree(data);
		pfree(lengths);
		pfree(pending.nodes);
	}
}

static bool callbacks_registered;

/* Whether this transaction appended versions to the delta store. */
static bool wrote_versions;

/* Root rows this transaction wrote, whose readers' caches must move at commit. */
typedef struct WrittenRoot
{
	RootKey		key;
	SubTransactionId subxid;
} WrittenRoot;

static List *written_roots;

/* Whether this transaction wrote |key|'s root row, in a subtransaction still alive. */
static bool
roots_written(const RootKey *key)
{
	ListCell   *lc;

	foreach(lc, written_roots)
	{
		WrittenRoot *written = lfirst(lc);

		if (memcmp(&written->key, key, sizeof(RootKey)) == 0)
			return true;
	}
	return false;
}

/* Pending edits, in dg_batch.inc. */
static bool loading_for_batch;
static Snapshot batch_group_snapshot;	/* the stream view a table and its indexes reload under together */
static HTAB *batches;
static bool batch_adds_handle(const RootKey *key, uint64 handle);
static void storage_xact_end(bool committed);
static void storage_pre_commit(void);
static void storage_touch(Oid db, Oid space, RelFileNumber file);
static pg_atomic_uint64 *storage_word(Oid space, RelFileNumber file);
static RootKey index_table_key(Relation index, const char *branch, Relation *table);
struct HandleDelta;
static struct HandleDelta *handle_delta_get(const RootKey *key, Snapshot snapshot);
static void handle_local_invalidate(void);
static void handle_delta_write(const RootKey *key, HTAB *mappings, CommandId cid);
static uint64 handle_delta_max(Oid space, RelFileNumber file);
static bool handle_delta_committed(const RootKey *key, uint64 handle);
static bool handle_records_exist(Oid db, Oid space, RelFileNumber file);
static inline bool handle_is_derived(uint64 handle);
typedef struct PrefixRows
{
	PLHash		digests[8];
	int64		counts[8];
	int			count;
} PrefixRows;
static PrefixRows prefix_rows(DGState *s, uint64 handle);
static void batch_flush_key(const RootKey *key);
static void batch_flush_all(void);
static void batch_discard(SubTransactionId subxid);
static void save_roots(const RootKey *key, bool table, DGState *s, CommandId cid, SubTransactionId subxid);
static uint64 roots_generation(const RootKey *key);
static void roots_generation_bump(const RootKey *key, bool claim);
static void handle_table_init(void *ptr);
static void writer_forget(TransactionId xid, int slot);

/*
 * Rows this transaction updated or deleted, by handle, with the command and
 * subtransaction that did it. A row changed by the current command must be
 * skipped rather than changed again, as PostgreSQL does for heap tuples.
 */
typedef struct ModifiedKey
{
	RootKey		root;
	uint64		handle;
} ModifiedKey;

typedef struct Modified
{
	ModifiedKey key;
	CommandId	cid;
	SubTransactionId subxid;
} Modified;

static HTAB *modified;

/* The tables a serializable transaction wrote, rechecked before it publishes. */
typedef struct SerializableWrite
{
	Oid			db;
	Oid			relid;
} SerializableWrite;

static SerializableWrite *serializable_writes;
static int	serializable_count;
static int	serializable_capacity;

/*
 * Running serializable writers of each table, which readers conflict out to:
 * their private changes are what heap's reader would find in progress. A full
 * registry leaves later writers to the check they make before publishing.
 */
#define DG_WRITER_SLOTS 1024

typedef struct WriterSlot
{
	Oid			db;
	Oid			relid;
	TransactionId xid;
} WriterSlot;

typedef struct WriterTable
{
	slock_t		mutex;
	WriterSlot	slots[DG_WRITER_SLOTS];
} WriterTable;

static WriterTable *writer_table;

/*
 * Every edit builds new nodes and orphans old ones, and the working store keeps
 * all of them until the transaction ends. Once it grows past a threshold,
 * collection frees the nodes no live state can reach. Live
 * states register here, because scans hold slices borrowed from their nodes.
 */
#define DG_COLLECT_MINIMUM ((Size) 64 * 1024 * 1024)

static DGState **live_states;
static int	live_count;
static int	live_capacity;
static Size collect_threshold = DG_COLLECT_MINIMUM;


static void store_xact(XactEvent event, void *arg);
static void store_subxact(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg);

/* The tree of |key| in the delta store. */
static DoltDeltaTree
delta_tree(const RootKey *key)
{
	DoltDeltaTree tree;

	dolt_delta_tree_key(&tree, MyDatabaseId, key->space, key->file, key->branch);
	return tree;
}

/*
 * Write the root row of |key|: a new row naming |content|, with no horizon,
 * as the base of a tree that had none; or, for a fold, a new version of the
 * row at |tid| naming its tree and the versions it holds.
 */
static void
roots_write(Relation catalog, const RootKey *key, ItemPointer tid, PLHash content, const DoltDeltaHorizon *horizon)
{
	Datum		values[Natts_pg_dolt_root];
	bool		nulls[Natts_pg_dolt_root] = {false};
	bytea	   *hash = palloc(VARHDRSZ + PL_HASH_SIZE);
	HeapTuple	tuple;
	WrittenRoot *written;
	MemoryContext old;

	SET_VARSIZE(hash, VARHDRSZ + PL_HASH_SIZE);
	memcpy(VARDATA(hash), content.bytes, PL_HASH_SIZE);
	values[Anum_pg_dolt_root_rootspace - 1] = ObjectIdGetDatum(key->space);
	values[Anum_pg_dolt_root_rootfile - 1] = ObjectIdGetDatum(key->file);
	values[Anum_pg_dolt_root_rootxmin - 1] = TransactionIdGetDatum(horizon ? horizon->xmin : InvalidTransactionId);
	values[Anum_pg_dolt_root_rootxmax - 1] = TransactionIdGetDatum(horizon ? horizon->xmax : InvalidTransactionId);
	values[Anum_pg_dolt_root_rootbranch - 1] = CStringGetTextDatum(key->branch);
	values[Anum_pg_dolt_root_rootcontent - 1] = PointerGetDatum(hash);
	if (horizon && TransactionIdIsValid(horizon->xmax))
	{
		Datum	   *xids = palloc(Max(horizon->xcnt, 1) * sizeof(Datum));

		for (uint32 i = 0; i < horizon->xcnt; i++)
			xids[i] = TransactionIdGetDatum(horizon->xip[i]);
		values[Anum_pg_dolt_root_rootxip - 1] =
			PointerGetDatum(construct_array_builtin(xids, horizon->xcnt, XIDOID));
	}
	else
		nulls[Anum_pg_dolt_root_rootxip - 1] = true;
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	if (tid && ItemPointerIsValid(tid))
		CatalogTupleUpdate(catalog, tid, tuple);
	else
		CatalogTupleInsert(catalog, tuple);
	heap_freetuple(tuple);
	roots_generation_bump(key, true);
	old = MemoryContextSwitchTo(TopTransactionContext);
	written = palloc(sizeof(WrittenRoot));
	written->key = *key;
	written->subxid = GetCurrentSubTransactionId();
	written_roots = lappend(written_roots, written);
	MemoryContextSwitchTo(old);
}

/*
 * Give |key| a root row naming |base|, unless one exists: the tree a state
 * that found none read as its base, which the versions its transaction is
 * about to append must apply to. Without the row, a branch's base would
 * follow its HEAD, which moves when the branch commits. Two transactions
 * creating one tree's row take turns: the lock is held until commit, and the
 * second then finds the first's row. Returns whether a row was written.
 */
static bool
roots_ensure(const RootKey *key, PLHash base)
{
	Roots		latest;
	Relation	catalog;
	Roots		root = {.present = true, .content = base};

	/* A row this transaction wrote in this command is not yet visible to it. */
	if (roots_written(key))
		return false;
	latest = roots_latest(key);
	if (latest.present)
		return false;
	LockDatabaseObject(DoltRootRelationId, key->file,
					   (int32) hash_bytes((const unsigned char *) key->branch, strlen(key->branch)),
					   ExclusiveLock);
	latest = roots_latest(key);
	if (latest.present)
		return false;
	persist_roots(&root);
	catalog = table_open(DoltRootRelationId, RowExclusiveLock);
	roots_write(catalog, key, NULL, base, NULL);
	table_close(catalog, RowExclusiveLock);
	return true;
}

static void
store_xact(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			{
				/*
				 * Commands such as DETACH PARTITION CONCURRENTLY commit while
				 * their portal runs; publish under the transaction's owner.
				 */
				ResourceOwner owner = CurrentResourceOwner;

				CurrentResourceOwner = TopTransactionResourceOwner;
				PG_TRY();
				{
					for (int i = 0; i < serializable_count; i++)
						CheckRelationForSerializableConflictIn(serializable_writes[i].db, serializable_writes[i].relid);
					batch_flush_all();
					working_close(true);
					if (event == XACT_EVENT_PRE_COMMIT)
						storage_pre_commit();
				}
				PG_FINALLY();
				{
					CurrentResourceOwner = owner;
				}
				PG_END_TRY();
			}
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PREPARE:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:

			/*
			 * Root rows this transaction wrote become visible now: roots
			 * read after they were written but before this point are
			 * stale, so the generation moves again.
			 */
			if (event == XACT_EVENT_COMMIT)
			{
				ListCell   *lc;

				foreach(lc, written_roots)
					roots_generation_bump(&((WrittenRoot *) lfirst(lc))->key, false);
			}
			written_roots = NIL;	/* in TopTransactionContext */

			/*
			 * Later pre-commit work, and parallel workers, which publish
			 * nothing, may still have read chunks; close cleanly unless
			 * aborting, when resource owners clean up instead.
			 */
			working_close(event != XACT_EVENT_ABORT && event != XACT_EVENT_PARALLEL_ABORT);
			storage_xact_end(event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT);
			dolt_delta_unpin();
			wrote_versions = false;
			handle_local_invalidate();
			batches = NULL;
			modified = NULL;
			/* A prepared writer stays registered until readers find it finished. */
			if (serializable_count && event != XACT_EVENT_PREPARE)
				writer_forget(GetTopTransactionIdIfAny(), -1);
			serializable_writes = NULL;
			serializable_count = serializable_capacity = 0;
			live_states = NULL;
			live_count = live_capacity = 0;
			collect_threshold = DG_COLLECT_MINIMUM;

			/*
			 * Nodes in the chunk store are read through the shared cache. A
			 * committed transaction's working store keeps the parsed ones,
			 * read or written, up to dolt_session_node_cache, so the next
			 * transaction need not copy and parse its hot paths again; past
			 * that, or after an abort, it drops them. An aborted transaction
			 * marked nodes written that never reached the store. Unwritten
			 * nodes are dropped too, unless pinned, such as the empty tree
			 * and nodes of the empty tree; they are written only when a published root
			 * reaches them.
			 */
			if (event == XACT_EVENT_COMMIT)
				share_written();
			dolt_node_cache_report();
			if (working_store)
			{
				pl_store_drop_unbacked(working_store);
				if (event != XACT_EVENT_COMMIT ||
					pl_store_stats(working_store).bytes > (Size) dolt_session_node_cache * 1024)
					pl_store_drop_backed(working_store);
			}
			written = NULL;		/* in TopTransactionContext */
			written_count = written_capacity = 0;
			break;
		default:
			break;
	}
}

static void
store_subxact(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg)
{
	HASH_SEQ_STATUS status;
	ListCell   *lc;

	/*
	 * Pending edits belong to one subtransaction: applied before a nested one
	 * starts or as their own commits, forgotten as it aborts.
	 */
	if (event == SUBXACT_EVENT_PRE_START_SUB || event == SUBXACT_EVENT_PRE_COMMIT_SUB)
		batch_flush_all();
	else if (event == SUBXACT_EVENT_ABORT_SUB)
	{
		batch_discard(mySubid);
		handle_local_invalidate();
	}
	if (event != SUBXACT_EVENT_ABORT_SUB)
		return;

	/* Root rows from this subtransaction and its children, with ids >= it, are gone. */
	foreach(lc, written_roots)
	{
		WrittenRoot *written = lfirst(lc);

		if (written->subxid >= mySubid)
			written_roots = foreach_delete_current(written_roots, lc);
	}
	if (modified)
	{
		Modified   *row;

		hash_seq_init(&status, modified);
		while ((row = hash_seq_search(&status)) != NULL)
			if (row->subxid >= mySubid)
				hash_search(modified, &row->key, HASH_REMOVE, NULL);
	}
}

static ModifiedKey
modified_key(Relation rel, const char *branch, uint64 handle)
{
	ModifiedKey key;

	memset(&key, 0, sizeof(key));
	key.root = root_key(rel, branch);
	key.handle = handle;
	return key;
}

/* Note that the current command updated or deleted row |handle|. */
void
dg_note_modified(Relation rel, const char *branch, uint64 handle, CommandId cid)
{
	ModifiedKey key = modified_key(rel, branch, handle);
	Modified   *row;

	if (!modified)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(ModifiedKey);
		ctl.entrysize = sizeof(Modified);
		ctl.hcxt = TopTransactionContext;
		modified = hash_create("prolly modified rows", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	row = hash_search(modified, &key, HASH_ENTER, NULL);
	row->cid = cid;
	row->subxid = GetCurrentSubTransactionId();
}

/* ---------------------------------------------------------------------------
 * Serializable isolation
 *
 * Predicate locks cover whole tables. A reader locks the table and conflicts
 * out to every writer that published the table's roots after its snapshot,
 * and to every running writer of it: their row versions are those heap's
 * reader would find invisible. A writer checks for readers as it writes, and
 * again before it publishes, in case the registry of running writers had no
 * room for it.
 * ---------------------------------------------------------------------------
 */

static void
writer_table_init(void *ptr)
{
	WriterTable *table = ptr;

	memset(table, 0, sizeof(*table));
	SpinLockInit(&table->mutex);
}

static WriterTable *
shared_writers(void)
{
	bool		found;

	if (!writer_table)
		writer_table = GetNamedDSMSegment("dolt prolly serializable writers", sizeof(WriterTable),
										  writer_table_init, &found);
	return writer_table;
}

static void
writer_register(Oid db, Oid relid, TransactionId xid)
{
	WriterTable *table = shared_writers();

	SpinLockAcquire(&table->mutex);
	for (int i = 0; i < DG_WRITER_SLOTS; i++)
		if (!TransactionIdIsValid(table->slots[i].xid))
		{
			table->slots[i].db = db;
			table->slots[i].relid = relid;
			table->slots[i].xid = xid;
			break;
		}
	SpinLockRelease(&table->mutex);
}

/* Forget this transaction's registrations, or a finished writer's at |slot|. */
static void
writer_forget(TransactionId xid, int slot)
{
	WriterTable *table = shared_writers();

	SpinLockAcquire(&table->mutex);
	for (int i = slot < 0 ? 0 : slot; i < (slot < 0 ? DG_WRITER_SLOTS : slot + 1); i++)
		if (TransactionIdEquals(table->slots[i].xid, xid))
			table->slots[i].xid = InvalidTransactionId;
	SpinLockRelease(&table->mutex);
}

/* Conflict out from a read of |rel| to each running writer of it. */
static void
writers_conflict_out(Relation rel, Snapshot snap)
{
	WriterTable *table = shared_writers();
	TransactionId xids[DG_WRITER_SLOTS];
	int			slots[DG_WRITER_SLOTS];
	int			n = 0;

	SpinLockAcquire(&table->mutex);
	for (int i = 0; i < DG_WRITER_SLOTS; i++)
		if (TransactionIdIsValid(table->slots[i].xid) && table->slots[i].db == rel->rd_locator.dbOid &&
			table->slots[i].relid == RelationGetRelid(rel))
		{
			xids[n] = table->slots[i].xid;
			slots[n++] = i;
		}
	SpinLockRelease(&table->mutex);
	for (int i = 0; i < n; i++)
	{
		if (TransactionIdIsCurrentTransactionId(xids[i]))
			continue;
		if (TransactionIdIsInProgress(xids[i]))
			CheckForSerializableConflictOut(rel, xids[i], snap);
		else
			writer_forget(xids[i], slots[i]);
	}
}

/* Note a serializable transaction's write to |rel|. */
void
dg_serializable_write(Relation rel)
{
	if (!IsolationIsSerializable())
		return;
	CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
	for (int i = 0; i < serializable_count; i++)
		if (serializable_writes[i].db == rel->rd_locator.dbOid && serializable_writes[i].relid == RelationGetRelid(rel))
			return;
	if (serializable_count == serializable_capacity)
	{
		serializable_capacity = serializable_capacity ? serializable_capacity * 2 : 8;
		serializable_writes = serializable_writes ?
			repalloc_array(serializable_writes, SerializableWrite, serializable_capacity) :
			MemoryContextAlloc(TopTransactionContext, serializable_capacity * sizeof(SerializableWrite));
	}
	serializable_writes[serializable_count].db = rel->rd_locator.dbOid;
	serializable_writes[serializable_count++].relid = RelationGetRelid(rel);
	writer_register(rel->rd_locator.dbOid, RelationGetRelid(rel), GetTopTransactionId());
}

/* Lock table |rel| for a serializable read under |snap|, of roots |key|. */
static void
serializable_read(Relation rel, const RootKey *key, Snapshot snap)
{
	Relation	catalog;
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;

	PredicateLockRelation(rel, snap);
	if (!CheckForSerializableConflictOutNeeded(rel, snap))
		return;
	writers_conflict_out(rel, snap);
	catalog = table_open(DoltRootRelationId, AccessShareLock);
	root_scan_keys(key, keys);
	scan = systable_beginscan(catalog, DoltRootKeyIndexId, true, SnapshotAny, 3, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		TransactionId xmin = HeapTupleHeaderGetXmin(tuple->t_data);

		if (TransactionIdIsNormal(xmin) && !TransactionIdIsCurrentTransactionId(xmin) &&
			XidInMVCCSnapshot(xmin, snap) &&
			(TransactionIdIsInProgress(xmin) || TransactionIdDidCommit(xmin)))
			CheckForSerializableConflictOut(rel, xmin, snap);
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
}

/*
 * Whether row |handle| exists in the latest committed state of |rel|'s branch,
 * rather than only among this transaction's changes.
 */
bool
dg_row_committed(Relation rel, uint64 handle)
{
	RootKey		key = root_key(rel, dg_relation_branch(rel));

	/*
	 * A derived handle is committed when a committed row holds its prefix;
	 * an allocated one when a committed transaction mapped it.
	 */
	if (handle_is_derived(handle))
	{
		DGState    *committed = dg_load_committed_latest(rel);
		bool		found = prefix_rows(committed, handle).count > 0;

		dg_release(committed);
		if (found)
			return true;
	}
	else if (handle_delta_committed(&key, handle))
		return true;
	return false;
}

/* If this transaction updated or deleted row |handle|, the command that did. */
bool
dg_row_modified(Relation rel, const char *branch, uint64 handle, CommandId *cid)
{
	ModifiedKey key;
	Modified   *row;

	if (!modified)
		return false;
	key = modified_key(rel, branch, handle);
	row = hash_search(modified, &key, HASH_FIND, NULL);
	if (row)
		*cid = row->cid;
	return row != NULL;
}

static void
register_callbacks(void)
{
	if (callbacks_registered)
		return;
	RegisterXactCallback(store_xact, NULL);
	RegisterSubXactCallback(store_subxact, NULL);
	callbacks_registered = true;
}

/* ---------------------------------------------------------------------------
 * Handles
 * ---------------------------------------------------------------------------
 */

#define DG_HANDLE_SLOTS 65536

typedef struct HandleSlot
{
	Oid			db;
	Oid			space;
	RelFileNumber file;
	pg_atomic_uint32 used;
	pg_atomic_uint64 next;
	pg_atomic_uint64 commit_generation;	/* the storage's word: see storage_word */
	pg_atomic_uint64 roots_generation;	/* bumped by every write of the storage's roots */
	pg_atomic_uint32 records;	/* whether any row of the storage ever had a lock record */
} HandleSlot;

typedef struct HandleTable
{
	slock_t		mutex;
	pg_atomic_uint64 roots_generation;	/* bumped when prepared roots become visible */
	HandleSlot	slots[DG_HANDLE_SLOTS];
} HandleTable;

static HandleTable *handle_table;

static HandleTable *
shared_handles(void)
{
	bool		found;

	if (!handle_table)
		handle_table = GetNamedDSMSegment("dolt prolly handles", sizeof(HandleTable), handle_table_init, &found);
	return handle_table;
}

static HandleSlot *handle_slot_claim(Oid db, Oid space, RelFileNumber file);
static HandleSlot *handle_slot_find(Oid db, Oid space, RelFileNumber file);

/*
 * A counter every write of a storage's roots advances before its commit, and
 * again as the commit becomes visible: while it stands still, no transaction
 * has published the storage's roots, so trees loaded under it are still the
 * latest. A prepared transaction's roots become visible in another backend,
 * which cannot tell which storages they name: that moves every storage's.
 */
static uint64
roots_generation(const RootKey *key)
{
	HandleSlot *slot;

	/*
	 * On a hot standby, roots change only by replay, which advances none of
	 * the counters: while replay stands still, nothing has changed. The high
	 * bit keeps these apart from the counters a promoted server goes on to.
	 */
	if (RecoveryInProgress())
		return (UINT64CONST(1) << 63) | (uint64) GetXLogReplayRecPtr(NULL);
	slot = handle_slot_claim(MyDatabaseId, key->space, key->file);

	return (pg_atomic_read_u64(&shared_handles()->roots_generation) << 32) +
		pg_atomic_read_u64(&slot->roots_generation);
}

/*
 * A write of |key|'s roots, which claims the storage's slot so that its commit
 * finds it; a commit callback, which may not read catalogs, only finds it.
 */
static void
roots_generation_bump(const RootKey *key, bool claim)
{
	HandleSlot *slot;

	shared_handles();
	slot = claim ? handle_slot_claim(MyDatabaseId, key->space, key->file) :
		handle_slot_find(MyDatabaseId, key->space, key->file);
	if (slot)
		pg_atomic_fetch_add_u64(&slot->roots_generation, 1);
}

/* A prepared transaction's roots became visible: COMMIT PREPARED calls this. */
void
dg_roots_published(void)
{
	pg_atomic_fetch_add_u64(&shared_handles()->roots_generation, 1);
}

static void
handle_table_init(void *ptr)
{
	HandleTable *table = ptr;

	SpinLockInit(&table->mutex);
	for (int i = 0; i < DG_HANDLE_SLOTS; i++)
	{
		pg_atomic_init_u32(&table->slots[i].used, 0);
		pg_atomic_init_u64(&table->slots[i].next, 0);
	}
	pg_atomic_init_u64(&table->roots_generation, 1);
}

static uint32
handle_slot_start(Oid db, Oid space, RelFileNumber file)
{
	uint64		h = ((uint64) db * UINT64CONST(0x9E3779B97F4A7C15)) ^
		((uint64) space * UINT64CONST(0xC2B2AE3D27D4EB4F)) ^ ((uint64) file * UINT64CONST(0x165667B19E3779F9));

	return (uint32) ((h ^ (h >> 31)) % DG_HANDLE_SLOTS);
}

static HandleSlot *
handle_slot_find(Oid db, Oid space, RelFileNumber file)
{
	uint32		start = handle_slot_start(db, space, file);

	for (uint32 n = 0; n < DG_HANDLE_SLOTS; n++)
	{
		HandleSlot *slot = &handle_table->slots[(start + n) % DG_HANDLE_SLOTS];

		if (!pg_atomic_read_u32(&slot->used))
			return NULL;
		pg_read_barrier();
		if (slot->db == db && slot->space == space && slot->file == file)
			return slot;
	}
	return NULL;
}

/* The largest handle in any non-aborted version of any branch's root row. */
static uint64
max_handle(Oid space, RelFileNumber file)
{
	return handle_delta_max(space, file);
}

/* The slot of a storage, claimed with the next handle after any in use. */
static HandleSlot *
handle_slot_claim(Oid db, Oid space, RelFileNumber file)
{
	HandleSlot *slot;

	shared_handles();
	slot = handle_slot_find(db, space, file);
	if (!slot)
	{
		uint64		max = max_handle(space, file);
		uint32		start = handle_slot_start(db, space, file);

		SpinLockAcquire(&handle_table->mutex);
		for (uint32 n = 0; n < DG_HANDLE_SLOTS; n++)
		{
			HandleSlot *candidate = &handle_table->slots[(start + n) % DG_HANDLE_SLOTS];

			if (pg_atomic_read_u32(&candidate->used))
			{
				if (candidate->db == db && candidate->space == space && candidate->file == file)
				{
					slot = candidate;
					break;
				}
				continue;
			}
			candidate->db = db;
			candidate->space = space;
			candidate->file = file;
			pg_atomic_write_u64(&candidate->next, Max(max, DG_DERIVED_HANDLES) + 1);
			pg_atomic_write_u64(&candidate->commit_generation, UINT64CONST(1) << 16);
			pg_atomic_write_u64(&candidate->roots_generation, 1);
			pg_atomic_write_u32(&candidate->records, dg_records_exist(space, file) ? 1 : 0);
			pg_write_barrier();
			pg_atomic_write_u32(&candidate->used, 1);
			slot = candidate;
			break;
		}
		SpinLockRelease(&handle_table->mutex);
		if (!slot)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("too many prolly relations for the handle table")));
	}
	return slot;
}

uint64
dg_allocate_handle(Relation rel)
{
	HandleSlot *slot = handle_slot_claim(rel->rd_locator.dbOid, rel->rd_locator.spcOid, rel->rd_locator.relNumber);

	return pg_atomic_fetch_add_u64(&slot->next, 1);
}

/* Whether any row of a storage ever had a lock record, so that a derived handle may be its root. */
static bool
handle_records_exist(Oid db, Oid space, RelFileNumber file)
{
	HandleSlot *slot = handle_slot_claim(db, space, file);

	return pg_atomic_read_u32(&slot->records) != 0;
}

void
dg_handle_records_note(RelFileLocator *locator)
{
	HandleSlot *slot = handle_slot_claim(locator->dbOid, locator->spcOid, locator->relNumber);

	pg_atomic_write_u32(&slot->records, 1);
}

/* ---------------------------------------------------------------------------
 * Loading and saving states
 * ---------------------------------------------------------------------------
 */

/* ---------------------------------------------------------------------------
 * Collecting the working store
 * ---------------------------------------------------------------------------
 */

static void
state_register(DGState *s)
{
	if (live_count == live_capacity)
	{
		live_capacity = live_capacity ? live_capacity * 2 : 16;
		live_states = live_states ? repalloc(live_states, live_capacity * sizeof(DGState *)) :
			MemoryContextAlloc(TopTransactionContext, live_capacity * sizeof(DGState *));
	}
	live_states[live_count++] = s;
}

static void
state_unregister(DGState *s)
{
	for (int i = live_count - 1; i >= 0; i--)
		if (live_states[i] == s)
		{
			live_states[i] = live_states[--live_count];
			return;
		}
}

static void
add_roots(PLHash **roots, int *count, int *capacity, const PLHash *hashes, int n)
{
	if (*count + n > *capacity)
	{
		*capacity = Max(*capacity * 2, *count + n + 64);
		*roots = *roots ? repalloc(*roots, *capacity * sizeof(PLHash)) : palloc(*capacity * sizeof(PLHash));
	}
	memcpy(*roots + *count, hashes, n * sizeof(PLHash));
	*count += n;
}

/*
 * Free working-store nodes nothing live can reach, if the store has grown past
 * the threshold. Callers are table AM write callbacks after releasing their
 * own state, when every tree in use belongs to a registered state.
 * Version-control calls and native rewrites hold other roots, so
 * collection waits for them.
 */
void
dg_collect(void)
{
	PLHash	   *roots = NULL;
	int			count = 0,
				capacity = 0;
	if (!working_store || pl_store_stats(working_store).bytes < collect_threshold ||
		dg_version_call_active() || dg_native_rewrite_active())
		return;
	for (int i = 0; i < live_count; i++)
	{
		DGState    *s = live_states[i];

		add_roots(&roots, &count, &capacity, &s->tree.root, 1);
		if (s->loaded_known)
			add_roots(&roots, &count, &capacity, &s->loaded, 1);
	}
	pl_store_retain(working_store, roots, count);
	collect_threshold = Max(DG_COLLECT_MINIMUM, 2 * pl_store_stats(working_store).bytes);
	if (roots)
		pfree(roots);
}

static void
cleanup(void *arg)
{
	DGState    *s = arg;

	if (s->shared && s->store)
		state_unregister(s);
	else if (s->store)
		pl_store_free(s->store);
	s->store = NULL;
}
void
dg_release(DGState *s)
{
	cleanup(s);
}

static DGState *
new_state(Relation rel)
{
	DGState    *s = palloc0(sizeof(*s));

	register_callbacks();
	s->store = dg_working_store();
	s->shared = true;
	state_register(s);
	s->cleanup.func = cleanup;
	s->cleanup.arg = s;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &s->cleanup);
	return s;
}

static void
state_set(DGState *s, const Roots *roots)
{
	s->tree = roots->present ? tree_at(roots->content) : empty_tree();
	s->has_root = roots->present;
	s->loaded = s->tree.root;
	s->loaded_known = true;
}

/*
 * Give |s| the versions of |key|'s delta that |snapshot| sees and |base| does
 * not hold, as a view read on demand (dg_view.c). This transaction's own
 * versions count unless |own| is false.
 */
static void
state_collect(DGState *s, const RootKey *key, Snapshot snapshot, const Roots *base, bool own)
{
	DoltDeltaTree tree = delta_tree(key);

	dg_state_view_delta(s, &tree, snapshot, base->present ? &base->horizon : NULL, own);
}

/*
 * Replace the base of a state that found no root row with |tree|, as a
 * branch's HEAD, a durable member or a bound storage supplies it. Such a
 * state has no changes: versions follow a root row.
 */
void
dg_set_base(DGState *s, PLTree tree)
{
	if (s->changes)
		elog(ERROR, "prolly state with changes has no default base");
	s->tree = tree;
	s->loaded = tree.root;
	s->loaded_known = true;
}

bool		dolt_snapshot_writes = true;

/* Whether writes build on the transaction snapshot now: repeatable read. */
bool
dg_snapshot_writes(void)
{
	return dolt_snapshot_writes && IsolationUsesXactSnapshot() && !IsolationIsSerializable();
}

/*
 * Under repeatable read, a load of a user table's or index's latest state
 * reads the transaction snapshot's roots instead, so the transaction's edits
 * build on its snapshot tree and merge with later commits only when it
 * publishes, and a commit elsewhere never makes its batches stale. Checks
 * that need later commits, as uniqueness does, read them explicitly
 * (dg_load_committed_latest).
 */
static bool
on_snapshot(Relation rel, Snapshot snap)
{
	Oid			namespace = rel->rd_rel->relnamespace;

	/* DDL's private descriptors and provisional relations read the latest state. */
	if ((snap && snap != SnapshotSelf) || !dg_snapshot_writes() ||
		CatalogViewIsProvisional(rel->rd_catalogview) || dg_storage_bound(rel))
		return false;

	/* Catalogs and TOAST tables, whose OIDs may be in the user range, too. */
	return (rel->rd_rel->relkind == RELKIND_RELATION || rel->rd_rel->relkind == RELKIND_INDEX) &&
		RelationGetRelid(rel) >= FirstNormalObjectId &&
		!IsCatalogNamespace(namespace) && !IsToastNamespace(namespace);
}

/* Whether |rel|'s writes build on the transaction snapshot now (on_snapshot). */
bool
dg_writes_on_snapshot(Relation rel)
{
	return on_snapshot(rel, SnapshotSelf);
}

/* The trees of |rel| committed as of |snapshot|, without this transaction's changes. */
DGState *
dg_load_committed_as_of(Relation rel, Snapshot snapshot)
{
	DGState    *s = new_state(rel);
	RootKey		key = root_key(rel, dg_relation_branch(rel));
	Roots		committed = roots_read(&key, snapshot);

	state_set(s, &committed);
	state_collect(s, &key, snapshot, &committed, false);
	dolt_delta_pin(snapshot->xmin);
	if (rel->rd_rel->relkind != RELKIND_INDEX)
		s->delta = handle_delta_get(&key, snapshot);
	return s;
}

/* The latest committed trees of |rel|, without this transaction's changes. */
DGState *
dg_load_committed_latest(Relation rel)
{
	DGState    *s = new_state(rel);
	RootKey		key = root_key(rel, dg_relation_branch(rel));
	Snapshot	latest = RegisterSnapshot(GetLatestSnapshot());
	Roots		committed = roots_read(&key, latest);

	state_set(s, &committed);
	state_collect(s, &key, latest, &committed, false);
	dolt_delta_pin(latest->xmin);
	if (rel->rd_rel->relkind != RELKIND_INDEX)
		s->delta = handle_delta_get(&key, latest);
	UnregisterSnapshot(latest);
	return s;
}

static DGState *
load_state(Relation rel, Snapshot snap, const uint64 *wanted, const char *branch, bool use_base, bool bound)
{
	DGState    *s;
	RootKey		key;
	Roots		committed;
	bool		table = rel->rd_rel->relkind != RELKIND_INDEX;
	Snapshot	read;
	bool		pushed = false,
				registered = false;

	if (!bound)
		dg_catalog_check_relation(rel, branch, snap);
	/* A dirty snapshot reads as SnapshotSelf; uniqueness checks wait on reservations. */
	if (snap && !IsMVCCSnapshot(snap) && snap != SnapshotSelf && snap->snapshot_type != SNAPSHOT_DIRTY)
		dg_unsupported("non-MVCC snapshots");
	key = root_key(rel, branch);
	if (table && snap && IsMVCCSnapshot(snap) && IsolationIsSerializable())
		serializable_read(rel, &key, snap);

	/*
	 * Any other reader of these roots sees the current command's edits
	 * applied. A fetch of one version by handle needs that only if the
	 * command's batch is adding that very version; otherwise the version is
	 * in the trees already, and flushing per fetched row would undo batching.
	 */
	if (!loading_for_batch && (!wanted || batch_adds_handle(&key, *wanted)))
		batch_flush_key(&key);
	s = new_state(rel);
	s->roots_generation = roots_generation(&key);
	s->roots_generation_known = true;

	/*
	 * The snapshot the base row and the versions are read under. The latest
	 * state, which writes and their checks read, is the latest snapshot with
	 * every command of the transaction, as SnapshotSelf shows its rows.
	 */
	if (on_snapshot(rel, snap))
	{
		PushCopiedSnapshot(GetTransactionSnapshot());
		read = GetActiveSnapshot();
		read->curcid = InvalidCommandId;
		pushed = true;
	}
	else if (snap && IsMVCCSnapshot(snap))
		read = snap;
	else
	{
		read = RegisterSnapshot(GetLatestSnapshot());
		read->curcid = InvalidCommandId;
		registered = true;
	}
	committed = roots_read(&key, read);
	state_set(s, &committed);
	state_collect(s, &key, read, &committed, true);
	dolt_delta_pin(read->xmin);
	/* Allocated handles as the roots' snapshot names them. */
	if (table)
		s->delta = handle_delta_get(&key, pushed ? read : snap);
	if (pushed)
		PopActiveSnapshot();
	if (registered)
		UnregisterSnapshot(read);
	if (!committed.present && use_base)
		dg_branch_base(rel, s);
	return s;
}
/* Load committed |content| into |state|, reading chunks as the repository. */
static void
import_content(Relation rel, DGState *state, bytea *content)
{
	Oid			user;
	int			security;

	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		dg_set_base(state, dg_history_import(state->store, content));
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
	}
	PG_END_TRY();
}

/*
 * A branch head's relation without a root of its own on the branch holds the
 * committed content of its working schema member, as bound storage does.
 */
static DGState *
durable_base(Relation rel, DGState *state)
{
	bytea	   *content;

	if (state->has_root)
		return state;
	content = dg_catalog_durable_content(RelationGetRelid(rel));
	if (content)
		import_content(rel, state, content);
	return state;
}

/*
 * A revision's relation, named by a leased OID, holds its committed content
 * and never changes, so every snapshot reads the same rows.
 */
static DGState *
revision_state(Relation rel)
{
	DGState    *s = new_state(rel);
	bytea	   *content = dg_catalog_revision_content(RelationGetRelid(rel));

	if (!content)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("revision relation \"%s\" has no content", RelationGetRelationName(rel))));
	import_content(rel, s, content);
	return s;
}

static bool
revision_relation(Relation rel)
{
	return CatalogViewIsLeased(RelationGetRelid(rel)) && !dg_storage_bound(rel) &&
		!CatalogViewIsProvisional(rel->rd_catalogview);
}

DGState *
dg_load(Relation rel, Snapshot snap)
{
	if (dg_storage_bound(rel))
		return dg_storage_am_load(rel, snap);
	if (CatalogViewIsDurable(RelationGetRelid(rel)))
		return durable_base(rel, load_state(rel, snap, NULL, dg_relation_branch(rel), false, true));
	if (revision_relation(rel))
		return revision_state(rel);
	if (CatalogViewObjectContext(RelationGetRelid(rel)))
		return dg_storage_am_load(rel, snap);
	return load_state(rel, snap, NULL, dg_relation_branch(rel), true, false);
}
DGState *
dg_load_branch(Relation rel, Snapshot snap, const char *branch)
{
	if (CatalogViewObjectContext(RelationGetRelid(rel)))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("canonical catalog handles require an explicit storage binding")));

	return load_state(rel, snap, NULL, branch, false, false);
}
DGState *
dg_load_tid(Relation rel, uint64 id)
{
	bool		durable;

	if (dg_storage_bound(rel))
		return dg_storage_am_load_tid(rel, id);
	durable = CatalogViewIsDurable(RelationGetRelid(rel));
	if (!durable && revision_relation(rel))
		return revision_state(rel);
	if (!durable && CatalogViewObjectContext(RelationGetRelid(rel)))
		return dg_storage_am_load_tid(rel, id);
	if (durable)
		return durable_base(rel, load_state(rel, SnapshotSelf, &id, dg_relation_branch(rel), false, true));
	return load_state(rel, SnapshotSelf, &id, dg_relation_branch(rel), true, false);
}

/* Internal page-reader entrypoint; only dg_resource.c supplies this descriptor. */
DGState *
dg_load_bound(Relation rel, Snapshot snap, const char *branch)
{
	Assert(RelFileNumberIsValid(rel->rd_locator.relNumber));
	Assert(rel->rd_lockInfo.lockRelId.relId < FirstCatalogViewObjectId);
	return load_state(rel, snap, NULL, branch, false, true);
}

DGState *
dg_load_bound_tid(Relation rel, uint64 id, const char *branch)
{
	return load_state(rel, SnapshotSelf, &id, branch, false, true);
}

/* A save's difference from the content it was loaded with, as delta edits. */
typedef struct SaveDiff
{
	DoltDeltaEdit *edits;
	int			count,
				capacity;
} SaveDiff;

static PLStatus
save_difference(void *arg, PLSlice key, PLValue before, PLValue after)
{
	SaveDiff   *diff = arg;
	int64		was = before.present ? (int64) pl_read64(before.bytes.data) : 0;
	int64		now = after.present ? (int64) pl_read64(after.bytes.data) : 0;
	PLSlice		row = after.present ? after.bytes : before.bytes;
	uint8	   *bytes;

	/* A key whose row bytes changed is a different row: count both. */
	if (before.present && after.present &&
		(before.bytes.len != after.bytes.len || memcmp(before.bytes.data + 8, after.bytes.data + 8, after.bytes.len - 8) != 0))
		elog(ERROR, "prolly save changes the row a key names");
	if (now == was)
		return PL_OK;
	if (diff->count == diff->capacity)
	{
		diff->capacity = diff->capacity ? diff->capacity * 2 : 64;
		diff->edits = diff->edits ? repalloc(diff->edits, diff->capacity * sizeof(DoltDeltaEdit)) :
			palloc(diff->capacity * sizeof(DoltDeltaEdit));
	}
	bytes = palloc(Max(key.len + row.len - 8, 1));
	memcpy(bytes, key.data, key.len);
	memcpy(bytes + key.len, row.data + 8, row.len - 8);
	diff->edits[diff->count].key = bytes;
	diff->edits[diff->count].keylen = key.len;
	diff->edits[diff->count].row = bytes + key.len;
	diff->edits[diff->count].rowlen = row.len - 8;
	diff->edits[diff->count].delta = now - was;
	diff->count++;
	return PL_OK;
}

/*
 * Append |edits| to |key|'s delta as command |cid|'s, after giving the tree a
 * root row naming |base| if it has none, so that the versions have a base to
 * apply to.
 */
static void
delta_append(const RootKey *key, PLHash base, const DoltDeltaEdit *edits, int count, CommandId cid)
{
	DoltDeltaTree tree;

	if (!count)
		return;
	if (XactReadOnly)
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("cannot write prolly trees in a read-only transaction")));
	(void) roots_ensure(key, base);
	tree = delta_tree(key);
	dolt_delta_append(&tree, edits, count, cid, true);
	wrote_versions = true;

	/* Readers' caches and batches of the storage notice the commit by its word. */
	storage_touch(MyDatabaseId, key->space, key->file);
}

/*
 * Save |s|'s content as |key|'s, for command |cid|. A tree with no root row
 * takes the content as its base. Otherwise the difference from the content
 * the state was loaded with goes to the delta, where it combines with what
 * other transactions changed meanwhile as their counts add.
 */
static void
save_roots(const RootKey *key, bool table, DGState *s, CommandId cid, SubTransactionId subxid)
{
	PLTree		loaded;
	SaveDiff	diff = {0};

	register_callbacks();
	if (s->store != dg_working_store())
		elog(ERROR, "prolly state does not belong to this transaction");

	/* Changes still unapplied mean nobody rebuilt the content. */
	if (s->changes || !s->loaded_known)
		return;
	if (roots_ensure(key, s->tree.root))
	{
		s->loaded = s->tree.root;
		return;
	}
	if (pl_hash_equal(s->loaded, s->tree.root))
		return;
	loaded = tree_at(s->loaded);
	dg_check(pl_diff(&loaded, &s->tree, save_difference, &diff));
	delta_append(key, s->loaded, diff.edits, diff.count, cid);
	s->loaded = s->tree.root;
}

static void
save_state(Relation rel, DGState *s, CommandId cid, const char *branch)
{
	RootKey		key = root_key(rel, branch);

	save_roots(&key, rel->rd_rel->relkind != RELKIND_INDEX, s, cid, GetCurrentSubTransactionId());
}
void
dg_save(Relation rel, DGState *s, CommandId cid)
{
	dg_version_mark_branch(dg_relation_branch(rel));
	if (dg_storage_bound(rel) || CatalogViewIsLeased(RelationGetRelid(rel)))
		dg_storage_require_write(rel);
	save_state(rel, s, cid, dg_relation_branch(rel));
}

/*
 * What a snapshot can see of other transactions' catalog rows is fixed by
 * its xmin, xmax and in-progress lists: two snapshots that agree on those see
 * the same committed rows. A load under SnapshotSelf sees what the latest
 * snapshot would. A cache is reused only under an identical view, plus the
 * command id for the transaction's own rows; counters that move at commit
 * would lag visibility by the time between a transaction's commit record and
 * its callbacks.
 */
#define VISIBILITY_XIP_MAX 16

typedef struct Visibility
{
	TransactionId xmin,
				xmax;
	uint32		xcnt,
				subxcnt;
	TransactionId xip[VISIBILITY_XIP_MAX];
	bool		overflow;		/* too many to record: never equal */
	CommandId	cid;
} Visibility;

static void
visibility_capture(Snapshot snapshot, Visibility *v)
{
	Snapshot	seen = snapshot && IsMVCCSnapshot(snapshot) ? snapshot : GetLatestSnapshot();
	uint32		n = 0;

	memset(v, 0, sizeof(*v));
	v->xmin = seen->xmin;
	v->xmax = seen->xmax;
	v->xcnt = seen->xcnt;
	v->subxcnt = seen->subxcnt;
	v->cid = snapshot && IsMVCCSnapshot(snapshot) ? snapshot->curcid : InvalidCommandId;
	if ((uint64) seen->xcnt + seen->subxcnt > VISIBILITY_XIP_MAX || seen->suboverflowed)
	{
		v->overflow = true;
		return;
	}
	for (uint32 i = 0; i < seen->xcnt; i++)
		v->xip[n++] = seen->xip[i];
	for (uint32 i = 0; i < seen->subxcnt; i++)
		v->xip[n++] = seen->subxip[i];
}

static bool
visibility_equal(const Visibility *a, const Visibility *b)
{
	return !a->overflow && !b->overflow && memcmp(a, b, sizeof(*a)) == 0;
}

/* The roots key of index |index|'s table on |branch|, opening the table unless bound. */
static RootKey
index_table_key(Relation index, const char *branch, Relation *table)
{
	RelFileLocator locator;

	if (!dg_storage_bound(index))
	{
		*table = relation_open(index->rd_index->indrelid, NoLock);
		return root_key(*table, branch);
	}
	*table = NULL;
	dg_storage_table_locator(index, &locator);
	return root_key_of(locator.spcOid, locator.relNumber, branch);
}

#include "dg_batch.inc"
#include "dg_handle.inc"

/*
 * The content root of storage file |locator| on |branch| as |snapshot| and this
 * transaction's own changes show it, as a load of its relation would: false
 * if the storage holds no root there.
 */
bool
dg_storage_content_root(RelFileLocator locator, const char *branch, Snapshot snapshot, PLHash *content)
{
	RootKey		key = {0};
	Roots		committed;
	DGState    *s;
	Snapshot	read;
	bool		registered = false;

	if (strlen(branch) >= sizeof(key.branch))
		ereport(ERROR, (errcode(ERRCODE_NAME_TOO_LONG), errmsg("branch name is too long for prolly storage")));
	key.space = locator.spcOid;
	key.file = locator.relNumber;
	strlcpy(key.branch, branch, sizeof(key.branch));
	if (!loading_for_batch)
		batch_flush_key(&key);
	read = snapshot;
	if (!read || !IsMVCCSnapshot(read))
	{
		read = RegisterSnapshot(GetLatestSnapshot());
		read->curcid = InvalidCommandId;
		registered = true;
	}
	committed = roots_read(&key, read);
	if (committed.present)
	{
		s = new_state(NULL);
		state_set(s, &committed);
		state_collect(s, &key, read, &committed, true);
		*content = dg_content(s)->root;
		dg_release(s);
	}
	if (registered)
		UnregisterSnapshot(read);
	return committed.present;
}

/*
 * Fold the delta of storage (|space|, |file|) on |branch| into a new base, in
 * the current transaction, which must have written nothing yet: every
 * version its snapshot sees, which the new root row records as its horizon,
 * so that readers of the new base leave those versions out and pruning can
 * drop them once every snapshot reads it. Folds of one tree take turns; one
 * that finds another running, or the root row locked or gone, folds nothing.
 * Returns whether it wrote a base.
 */
bool
dg_fold_tree(Oid space, RelFileNumber file, const char *branch)
{
	RootKey		key = root_key_of(space, file, branch);
	DoltDeltaTree tree = delta_tree(&key);
	Snapshot	snapshot;
	Relation	catalog;
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	Roots		committed = {0};
	ItemPointerData tid;
	DGState    *s;
	DoltDeltaHorizon horizon;
	bool		folded = false;

	/* Its own versions would count as held once it commits, though it folds none. */
	if (wrote_versions)
		elog(ERROR, "a prolly fold must run in a transaction that wrote no prolly rows");
	if (!ConditionalLockDatabaseObject(DoltRootRelationId, file,
									   (int32) hash_bytes((const unsigned char *) branch, strlen(branch)),
									   ExclusiveLock))
		return false;

	/* Taken after the lock: a fold that held it before is visible. */
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	catalog = table_open(DoltRootRelationId, RowExclusiveLock);
	root_scan_keys(&key, keys);
	scan = systable_beginscan(catalog, DoltRootKeyIndexId, true, snapshot, 3, keys);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		HeapTupleData locked;
		Buffer		buffer = InvalidBuffer;
		TM_FailureData failure;

		committed = roots_from_tuple(tuple, RelationGetDescr(catalog));
		tid = tuple->t_self;
		locked.t_self = tid;

		/* A row another transaction holds is not worth waiting for. */
		if (heap_lock_tuple(catalog, &locked, GetCurrentCommandId(true), LockTupleExclusive,
							LockWaitSkip, false, &buffer, &failure) != TM_Ok)
			committed.present = false;
		if (BufferIsValid(buffer))
			ReleaseBuffer(buffer);
	}
	systable_endscan(scan);
	if (committed.present)
	{
		s = new_state(NULL);
		state_set(s, &committed);
		state_collect(s, &key, snapshot, &committed, false);
		if (s->changes)
		{
			PLTree	   *content = dg_content(s);
			Roots		folded_roots = {.present = true, .content = content->root};

			horizon.xmin = snapshot->xmin;
			horizon.xmax = snapshot->xmax;
			horizon.xcnt = snapshot->xcnt;
			horizon.xip = snapshot->xip;
			persist_roots(&folded_roots);
			roots_write(catalog, &key, &tid, content->root, &horizon);
			dolt_delta_set_fold(&tree, GetTopTransactionId(), &horizon);
			folded = true;
		}
		dg_release(s);
	}
	table_close(catalog, RowExclusiveLock);
	UnregisterSnapshot(snapshot);
	return folded;
}

/* Only the transactional resource allocator calls this, before publishing any
 * branch binding: the new storage starts empty on its branch. */
void
dg_initialize_resource(Relation rel, const char *branch)
{
	DGState    *state;

	state = load_state(rel, SnapshotSelf, NULL, branch, false, true);
	save_state(rel, state, GetCurrentCommandId(true), branch);
	dg_release(state);
}

static PLStatus
collect(void *arg, PLSlice key, PLSlice value)
{
	DGRows	   *r = arg;

	if (r->count == r->capacity)
	{
		r->capacity = r->capacity ? r->capacity * 2 : 64;
		r->items = r->items ? repalloc(r->items, r->capacity * sizeof(PLEntry)) : palloc(r->capacity * sizeof(PLEntry));
	}
	r->items[r->count].key = key;
	r->items[r->count++].value = value;
	return PL_OK;
}

DGRows
dg_rows(const PLTree * tree)
{
	DGRows		rows = {0};

	dg_check(pl_scan(tree, NULL, NULL, collect, &rows));
	return rows;
}

/* Every entry of |s|'s merged content. */
DGRows
dg_state_rows(DGState *s)
{
	DGRows		rows = {0};

	dg_scan_keep(s, NULL, NULL, collect, &rows);
	return rows;
}
/* Logical pages must fit PostgreSQL's exact TID bitmap representation. */
void
dg_tid(uint64 id, ItemPointer tid)
{
	if (id / TBM_MAX_TUPLES_PER_PAGE >= InvalidBlockNumber)
		elog(ERROR, "prolly TID space exhausted");
	ItemPointerSet(tid, id / TBM_MAX_TUPLES_PER_PAGE, (id % TBM_MAX_TUPLES_PER_PAGE) + 1);
}
uint64
dg_id(ItemPointer tid)
{
	return (uint64) ItemPointerGetBlockNumber(tid) * TBM_MAX_TUPLES_PER_PAGE + ItemPointerGetOffsetNumber(tid) - 1;
}
Datum
dolt_prolly_root(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	DGState    *s;
	char		result[PL_HASH_TEXT_SIZE + 1];

	if (rel->rd_tableam != dg_table_routine() &&
		!(rel->rd_rel->relkind == RELKIND_INDEX && !strcmp(get_am_name(rel->rd_rel->relam), "dolt_prolly_index")))
		dg_unsupported("root inspection of other AMs");
	s = dg_load(rel, GetActiveSnapshot());
	pl_hash_format(dg_content(s)->root, result);
	dg_release(s);
	relation_close(rel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/* Make this transaction's batched changes visible to canonical capture. */
void
dg_flush_version_batches(void)
{
	batch_flush_all();
}
