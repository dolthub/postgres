/*-------------------------------------------------------------------------
 *
 * dg_store.c
 *	  Prolly relation state: trees in the chunk store, roots in pg_dolt_root.
 *
 * Every tree node lives in the chunk store (access/doltchunk.h). A relation's
 * committed roots, per branch, live in one pg_dolt_root row, so a snapshot
 * selects them with ordinary MVCC. Relation files hold no prolly data.
 *
 * Within a transaction, nodes are built and read through a working store that
 * loads missing nodes from the chunk store on demand. Saving a state writes
 * nothing: it pushes the new roots onto the transaction's overlay, a stack of
 * (command ID, subtransaction, base roots, roots) per relation storage and
 * branch. Loads consult the overlay first, so a statement sees its earlier
 * statements' writes by command ID, and a subtransaction rollback pops its
 * entries. If an entry's base differs from the committed roots a snapshot
 * selects, the view is the three-way merge of the two.
 *
 * Just before commit or PREPARE, each written (storage, branch) publishes: it
 * locks or creates its root row, merges its changes onto the latest committed
 * roots, writes new nodes to the chunk store and stores the merged roots. The
 * row lock is held until commit, after the transaction becomes visible, so a
 * snapshot that sees one publisher sees every earlier one.
 *
 * Handles, which serve as TIDs, come from a per-storage counter in shared
 * memory and are never reused. After a restart the counter starts above the
 * largest handle in any root row version that did not abort.
 *
 * src/backend/access/dolt_prolly/dg_store.c
 *
 *-------------------------------------------------------------------------
 */
#include "dg.h"
#include "access/doltversion.h"
#include "nodes/tidbitmap.h"
#include "access/doltchunk.h"
#include "access/doltjournal.h"
#include "access/doltnode.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/parallel.h"
#include "access/twophase.h"
#include "access/xlog.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalogview.h"
#include "catalog/indexing.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_dolt_root.h"
#include "catalog/index.h"
#include "catalog/pg_dolt_journal.h"
#include "catalog/pg_dolt_handle.h"
#include "catalog/pg_index.h"
#include "utils/syscache.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "access/doltbranch.h"
#include "access/doltcatalog.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/dsm_registry.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/spin.h"
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

typedef struct Roots
{
	bool		present;
	PLHash		content;
} Roots;

static bool
roots_equal(const Roots *a, const Roots *b)
{
	if (a->present != b->present)
		return false;
	if (!a->present)
		return true;
	return pl_hash_equal(a->content, b->content);
}

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
	bool		present;

	hash_column(tuple, desc, Anum_pg_dolt_root_rootcontent, &present, &roots.content);
	if (!present)
		elog(ERROR, "invalid prolly root row");
	return roots;
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
	return hash_search(roots_cache, key, enter ? HASH_ENTER : HASH_FIND, NULL);
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
		return cached->roots;
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

		entry->generation = generation;
		entry->xmin = xmin;
		entry->roots = roots;
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

/* ---------------------------------------------------------------------------
 * Three-way merge of roots
 * ---------------------------------------------------------------------------
 */

/*
 * Content and index values are an occurrence count followed by the row, so
 * concurrent changes to the same key combine additively.
 */
static PLStatus
merge_counts(void *arg, PLSlice key, PLValue base, PLValue left, PLValue right, PLValue *result)
{
	int64		b = base.present ? (int64) pl_read64(base.bytes.data) : 0;
	int64		l = left.present ? (int64) pl_read64(left.bytes.data) : 0;
	int64		r = right.present ? (int64) pl_read64(right.bytes.data) : 0;
	int64		count = r + l - b;
	PLSlice		row = left.present ? left.bytes : right.present ? right.bytes : base.bytes;
	uint8	   *bytes;

	if (count < 0)
		return PL_CONFLICT;
	if (count == 0)
	{
		result->present = false;
		return PL_OK;
	}
	bytes = palloc(row.len);
	memcpy(bytes, row.data, row.len);
	pl_write64(bytes, (uint64) count);
	result->present = true;
	result->bytes.data = bytes;
	result->bytes.len = row.len;
	return PL_OK;
}

static PLHash
merge_tree(PLHash base, PLHash left, PLHash right, bool counts)
{
	PLTree		b = tree_at(base),
				l = tree_at(left),
				r = tree_at(right),
				out;
	PLStatus	status = pl_merge(&b, &l, &r, counts ? merge_counts : NULL, NULL, &out);

	if (status == PL_CONFLICT)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to concurrent update"),
				 errdetail("Concurrent changes to the same prolly rows could not be combined.")));
	dg_check(status);
	return out.root;
}

/* Combine changes made on |base| (ours) with the committed |theirs|. */
static Roots
roots_merge(const Roots *base, const Roots *ours, const Roots *theirs)
{
	Roots		empty = {0},
				result = *ours;
	Roots		b = *base;

	if (roots_equal(base, theirs))
		return *ours;
	if (roots_equal(base, ours))
		return *theirs;
	if (!ours->present)
		return *ours;
	if (!b.present)
	{
		PLTree		e = empty_tree();

		b.present = true;
		b.content = e.root;
	}
	if (!theirs->present)
	{
		/* Nothing committed yet: only our changes exist. */
		empty = b;
		theirs = &empty;
	}
	result.content = merge_tree(b.content, ours->content, theirs->content, true);
	return result;
}

/* ---------------------------------------------------------------------------
 * The transaction overlay
 * ---------------------------------------------------------------------------
 */

typedef struct OverlayEntry
{
	CommandId	cid;
	SubTransactionId subxid;
	Roots		base;			/* committed roots the entry was built on */
	Roots		roots;
} OverlayEntry;

typedef struct Overlay
{
	RootKey		key;			/* hash key */
	OverlayEntry *entries;
	int			count;
	int			capacity;
} Overlay;

static HTAB *overlays;
static bool callbacks_registered;

/* Pending edits, in dg_batch.inc. */
static bool loading_for_batch;
static Snapshot batch_group_snapshot;	/* the stream view a table and its indexes reload under together */
static HTAB *batches;
static HTAB *journal_marks;		/* (relid, cid, subxid) of batches that were journaled */
static bool journal_batch(DGBatch *b, bool forced);
static bool batch_adds_handle(const RootKey *key, uint64 handle);
static void journal_consume(const RootKey *key, const int64 *seqs, int count, CommandId cid);
static void journal_consumed_abort(SubTransactionId subid);
static DGState *journal_fold_loaded(Relation rel, const char *branch, Snapshot snapshot, DGState *s);
static DGState *load_unfolded(Relation rel, Snapshot snap, bool *foldable);
static void journal_fold_roots(PLHash **roots, int *count, int *capacity);
static uint64 journal_max_handle(Oid space, RelFileNumber file);
static void journal_xact_end(bool committed);
static void journal_pre_commit(void);
static void journal_local_invalidate(void);
static bool journal_handle_committed(const RootKey *key, uint64 handle);
static RootKey journal_index_table(Relation index, const char *branch, Relation *table);
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
static PrefixRows prefix_rows(const PLTree *tree, uint64 handle);
static void batch_flush_key(const RootKey *key);
static void batch_flush_all(void);
static void batch_discard(SubTransactionId subxid);
static void save_roots(const RootKey *key, bool table, DGState *s, CommandId cid, SubTransactionId subxid);
static uint64 roots_generation(const RootKey *key);
static void roots_generation_bump(const RootKey *key, bool claim);
static void handle_table_init(void *ptr);
static bool journal_entry_any(const RootKey *key, uint64 handle, PLHash *digest, PLSlice *row);
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
 * collection frees the nodes no live state or overlay entry can reach. Live
 * states register here, because scans hold slices borrowed from their nodes.
 */
#define DG_COLLECT_MINIMUM ((Size) 64 * 1024 * 1024)

static DGState **live_states;
static int	live_count;
static int	live_capacity;
static Size collect_threshold = DG_COLLECT_MINIMUM;


static void store_xact(XactEvent event, void *arg);
static void store_subxact(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg);

static Overlay *
overlay_find(const RootKey *key, bool create)
{
	bool		found;
	Overlay    *overlay;

	if (!overlays)
	{
		HASHCTL		ctl = {0};

		if (!create)
			return NULL;
		ctl.keysize = sizeof(RootKey);
		ctl.entrysize = sizeof(Overlay);
		ctl.hcxt = TopTransactionContext;
		overlays = hash_create("prolly transaction overlays", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	overlay = hash_search(overlays, key, create ? HASH_ENTER : HASH_FIND, &found);
	if (overlay && !found)
	{
		overlay->entries = NULL;
		overlay->count = 0;
		overlay->capacity = 0;
	}
	return overlay;
}

/* The entry |snapshot| sees: the latest, or the latest before its command. */
static OverlayEntry *
overlay_entry(Overlay *overlay, Snapshot snapshot)
{
	if (!overlay || !overlay->count)
		return NULL;
	if (!snapshot || !IsMVCCSnapshot(snapshot))
		return &overlay->entries[overlay->count - 1];
	for (int i = overlay->count - 1; i >= 0; i--)
		if (overlay->entries[i].cid < snapshot->curcid)
			return &overlay->entries[i];
	return NULL;
}

static void
overlay_push(const RootKey *key, CommandId cid, SubTransactionId subxid, const Roots *base, const Roots *roots)
{
	Overlay    *overlay = overlay_find(key, true);
	OverlayEntry *top;

	if (overlay->count && overlay->entries[overlay->count - 1].cid == cid &&
		overlay->entries[overlay->count - 1].subxid == subxid)
		top = &overlay->entries[overlay->count - 1];
	else
	{
		if (overlay->count == overlay->capacity)
		{
			overlay->capacity = overlay->capacity ? overlay->capacity * 2 : 4;
			overlay->entries = overlay->entries ?
				repalloc(overlay->entries, overlay->capacity * sizeof(OverlayEntry)) :
				MemoryContextAlloc(TopTransactionContext, overlay->capacity * sizeof(OverlayEntry));
		}
		top = &overlay->entries[overlay->count++];
	}
	top->cid = cid;
	top->subxid = subxid;
	top->base = *base;
	top->roots = *roots;
}

static int
compare_overlays(const void *a, const void *b)
{
	const Overlay *x = *(Overlay *const *) a;
	const Overlay *y = *(Overlay *const *) b;

	if (x->key.space != y->key.space)
		return x->key.space < y->key.space ? -1 : 1;
	if (x->key.file != y->key.file)
		return x->key.file < y->key.file ? -1 : 1;
	return strcmp(x->key.branch, y->key.branch);
}

/*
 * Lock the root row for |key|, waiting out a concurrent publisher, and return
 * the latest committed roots. Creates no row; *tid is invalid if none exists.
 */
static Roots
roots_lock(Relation catalog, const RootKey *key, ItemPointer tid)
{
	for (;;)
	{
		Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
		ScanKeyData keys[3];
		SysScanDesc scan;
		HeapTuple	tuple;
		Roots		roots = {0};
		TM_Result	result = TM_Ok;

		root_scan_keys(key, keys);
		scan = systable_beginscan(catalog, DoltRootKeyIndexId, true, snapshot, 3, keys);
		tuple = systable_getnext(scan);
		ItemPointerSetInvalid(tid);
		if (HeapTupleIsValid(tuple))
		{
			HeapTupleData locked;
			Buffer		buffer = InvalidBuffer;
			TM_FailureData failure;

			roots = roots_from_tuple(tuple, RelationGetDescr(catalog));
			locked.t_self = tuple->t_self;
			result = heap_lock_tuple(catalog, &locked, GetCurrentCommandId(true), LockTupleExclusive,
									 LockWaitBlock, false, &buffer, &failure);
			if (BufferIsValid(buffer))
				ReleaseBuffer(buffer);
			*tid = tuple->t_self;
		}
		systable_endscan(scan);
		UnregisterSnapshot(snapshot);
		if (result == TM_Ok || result == TM_SelfModified)
			return roots;

		/* Another publisher committed first; read its roots and retry. */
	}
}

static void
roots_write(Relation catalog, const RootKey *key, ItemPointer tid, const Roots *roots)
{
	Datum		values[Natts_pg_dolt_root];
	bool		nulls[Natts_pg_dolt_root] = {false};
	bytea	   *hash = palloc(VARHDRSZ + PL_HASH_SIZE);
	HeapTuple	tuple;

	SET_VARSIZE(hash, VARHDRSZ + PL_HASH_SIZE);
	memcpy(VARDATA(hash), roots->content.bytes, PL_HASH_SIZE);
	values[Anum_pg_dolt_root_rootspace - 1] = ObjectIdGetDatum(key->space);
	values[Anum_pg_dolt_root_rootfile - 1] = ObjectIdGetDatum(key->file);
	values[Anum_pg_dolt_root_rootbranch - 1] = CStringGetTextDatum(key->branch);
	values[Anum_pg_dolt_root_rootcontent - 1] = PointerGetDatum(hash);
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	if (ItemPointerIsValid(tid))
		CatalogTupleUpdate(catalog, tid, tuple);
	else
		CatalogTupleInsert(catalog, tuple);
	heap_freetuple(tuple);
	roots_generation_bump(key, true);
}

/* Publish every written relation, in a fixed order to avoid deadlocks. */
static void
publish_all(void)
{
	HASH_SEQ_STATUS status;
	Overlay    *overlay;
	Overlay   **written;
	int			count = 0;
	Relation	catalog;

	if (!overlays)
		return;
	written = palloc(Max(hash_get_num_entries(overlays), 1) * sizeof(Overlay *));
	hash_seq_init(&status, overlays);
	while ((overlay = hash_seq_search(&status)) != NULL)
		if (overlay->count)
			written[count++] = overlay;
	if (!count)
		return;
	if (XactReadOnly)
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("cannot publish prolly roots in a read-only transaction")));
	qsort(written, count, sizeof(Overlay *), compare_overlays);
	catalog = table_open(DoltRootRelationId, RowExclusiveLock);
	for (int i = 0; i < count; i++)
	{
		OverlayEntry *top = &written[i]->entries[written[i]->count - 1];
		ItemPointerData tid;
		Roots		latest = roots_lock(catalog, &written[i]->key, &tid);
		Roots		merged;

		/*
		 * Concurrent first publishers of a (storage, branch) take turns: the
		 * later one waits here until the earlier commits, then finds and
		 * locks its row.
		 */
		if (!ItemPointerIsValid(&tid))
		{
			LockDatabaseObject(DoltRootRelationId, written[i]->key.file,
							   (int32) hash_bytes((const unsigned char *) written[i]->key.branch,
												  strlen(written[i]->key.branch)),
							   ExclusiveLock);
			latest = roots_lock(catalog, &written[i]->key, &tid);
		}
		merged = roots_merge(&top->base, &top->roots, &latest);

		/* A capture may publish a parent's edits inside a savepoint. Keep
		 * its overlay intact: both the catalog publication and this rebase
		 * must disappear if the child aborts. */
		if (top->subxid != GetCurrentSubTransactionId())
		{
			Roots base = top->base;
			Roots roots = top->roots;

			overlay_push(&written[i]->key, GetCurrentCommandId(false),
						 GetCurrentSubTransactionId(), &base, &roots);
			top = &written[i]->entries[written[i]->count - 1];
		}

		/* A pre-commit capture must see the same merged roots we publish.
		 * Rebase the overlay so later pre-commit publication is idempotent. */
		top->base = top->roots = merged;

		if (!merged.present || roots_equal(&merged, &latest))
			continue;
		persist_roots(&merged);
		roots_write(catalog, &written[i]->key, &tid, &merged);
	}
	table_close(catalog, RowExclusiveLock);
	pfree(written);
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
					publish_all();
					working_close(true);
					if (event == XACT_EVENT_PRE_COMMIT)
						journal_pre_commit();
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
			 * Roots this transaction published become visible now: trees
			 * loaded after they were written but before this point are
			 * stale, so the generation moves again.
			 */
			if (event == XACT_EVENT_COMMIT && overlays)
			{
				HASH_SEQ_STATUS status;
				Overlay    *overlay;

				hash_seq_init(&status, overlays);
				while ((overlay = hash_seq_search(&status)) != NULL)
					roots_generation_bump(&overlay->key, false);
			}

			/*
			 * Later pre-commit work, and parallel workers, which publish
			 * nothing, may still have read chunks; close cleanly unless
			 * aborting, when resource owners clean up instead.
			 */
			working_close(event != XACT_EVENT_ABORT && event != XACT_EVENT_PARALLEL_ABORT);
			journal_xact_end(event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT);
			handle_local_invalidate();
			overlays = NULL;	/* in TopTransactionContext */
			batches = NULL;
			journal_marks = NULL;
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
			 * Nodes in the chunk store are read through the shared cache, so
			 * the working store keeps none across transactions: those it
			 * read are dropped, and those it wrote are shared once committed,
			 * then dropped. Unwritten nodes are dropped too, unless pinned,
			 * such as the empty tree and journal folds; they are written only
			 * when a published root reaches them.
			 */
			if (event == XACT_EVENT_COMMIT)
				share_written();
			dolt_node_cache_report();
			if (working_store)
			{
				pl_store_drop_unbacked(working_store);
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
	Overlay    *overlay;

	/*
	 * Pending edits belong to one subtransaction: applied before a nested one
	 * starts or as their own commits, forgotten as it aborts.
	 */
	if (event == SUBXACT_EVENT_PRE_START_SUB || event == SUBXACT_EVENT_PRE_COMMIT_SUB)
		batch_flush_all();
	else if (event == SUBXACT_EVENT_ABORT_SUB)
	{
		batch_discard(mySubid);
		journal_local_invalidate();
		journal_consumed_abort(mySubid);
		handle_local_invalidate();
	}
	if (event != SUBXACT_EVENT_ABORT_SUB || !overlays)
		return;

	/* Entries from this subtransaction and its children have ids >= it. */
	hash_seq_init(&status, overlays);
	while ((overlay = hash_seq_search(&status)) != NULL)
		while (overlay->count && overlay->entries[overlay->count - 1].subxid >= mySubid)
			overlay->count--;
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
	Roots		committed;

	/*
	 * A derived handle is committed when a committed row holds its prefix;
	 * an allocated one when a committed transaction mapped it. A row another
	 * transaction journaled and committed is committed too.
	 */
	if (handle_is_derived(handle))
	{
		committed = roots_latest(&key);
		if (committed.present)
		{
			PLTree		content = tree_at(committed.content);

			if (prefix_rows(&content, handle).count > 0)
				return true;
		}
	}
	else if (handle_delta_committed(&key, handle))
		return true;
	return journal_handle_committed(&key, handle);
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
	pg_atomic_uint64 journal_generation;	/* bumped when a journal write to the storage commits */
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
	HandleSlot *slot = handle_slot_claim(MyDatabaseId, key->space, key->file);

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
	return Max(handle_delta_max(space, file), journal_max_handle(space, file));
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
			pg_atomic_write_u64(&candidate->journal_generation, UINT64CONST(1) << 16);
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
 * own state, when every tree in use belongs to a registered state or the
 * overlay. Version-control calls and native rewrites hold other roots, so
 * collection waits for them.
 */
void
dg_collect(void)
{
	PLHash	   *roots = NULL;
	int			count = 0,
				capacity = 0;
	HASH_SEQ_STATUS status;
	Overlay    *overlay;

	if (!working_store || pl_store_stats(working_store).bytes < collect_threshold ||
		dg_version_call_active() || dg_native_rewrite_active())
		return;
	for (int i = 0; i < live_count; i++)
	{
		DGState    *s = live_states[i];

		add_roots(&roots, &count, &capacity, &s->content.root, 1);
		if (s->based)
			add_roots(&roots, &count, &capacity, &s->base, 1);
	}
	if (overlays)
	{
		hash_seq_init(&status, overlays);
		while ((overlay = hash_seq_search(&status)) != NULL)
			for (int i = 0; i < overlay->count; i++)
			{
				const Roots *both[2] = {&overlay->entries[i].roots, &overlay->entries[i].base};

				for (int j = 0; j < 2; j++)
					if (both[j]->present)
						add_roots(&roots, &count, &capacity, &both[j]->content, 1);
			}
	}
	journal_fold_roots(&roots, &count, &capacity);
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
	s->content = roots->present ? tree_at(roots->content) : empty_tree();
	s->has_root = roots->present;
}

/*
 * The roots |key| shows under |snap|: the committed ones, with this
 * transaction's overlay entry for the snapshot applied. |base| receives what
 * the result was built on.
 */
static Roots
view_roots(const RootKey *key, Snapshot snap, Roots *base)
{
	Overlay    *overlay = overlay_find(key, false);
	OverlayEntry *entry;
	Roots		committed,
				view;

	if (snap && IsMVCCSnapshot(snap))
		committed = roots_read(key, snap);
	else
		committed = roots_latest(key);
	view = *base = committed;
	entry = overlay_entry(overlay, snap);
	if (entry)
	{
		if (roots_equal(&entry->base, &committed))
		{
			view = entry->roots;
			*base = entry->base;
		}
		else
			view = roots_merge(&entry->base, &entry->roots, &committed);
	}
	return view;
}

static DGState *
load_state(Relation rel, Snapshot snap, const uint64 *wanted, const char *branch, bool use_base, bool bound)
{
	DGState    *s;
	RootKey		key;
	Roots		committed,
				view,
				base;
	Overlay    *overlay;
	OverlayEntry *entry;
	bool		table = rel->rd_rel->relkind != RELKIND_INDEX;

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
	overlay = overlay_find(&key, false);
	s->roots_generation = roots_generation(&key);
	s->roots_generation_known = true;
	if (snap && IsMVCCSnapshot(snap))
		committed = roots_read(&key, snap);
	else
		committed = roots_latest(&key);
	view = base = committed;
	entry = overlay_entry(overlay, snap);
	if (entry)
	{
		if (roots_equal(&entry->base, &committed))
		{
			view = entry->roots;
			base = entry->base;
		}
		else
			view = roots_merge(&entry->base, &entry->roots, &committed);
	}
	state_set(s, &view);
	s->based = base.present;
	if (base.present)
		s->base = base.content;
	if (table)
		s->delta = handle_delta_get(&key, snap);
	if (!view.present && use_base)
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
		state->content = dg_history_import(state->store, content);
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

/* The trees of |rel| under |snap| as the roots name them, before the journal. */
static DGState *
load_unfolded(Relation rel, Snapshot snap, bool *foldable)
{
	*foldable = false;
	if (dg_storage_bound(rel))
		return dg_storage_am_load(rel, snap);
	if (CatalogViewIsDurable(RelationGetRelid(rel)))
	{
		*foldable = true;
		return durable_base(rel, load_state(rel, snap, NULL, dg_relation_branch(rel), false, true));
	}
	if (revision_relation(rel))
		return revision_state(rel);
	if (CatalogViewObjectContext(RelationGetRelid(rel)))
		return dg_storage_am_load(rel, snap);
	*foldable = true;
	return load_state(rel, snap, NULL, dg_relation_branch(rel), true, false);
}

/*
 * A load of the latest state reads its roots, its journal and an index's
 * table at different moments, and a fold another transaction persisted in
 * between, which saves roots that include entries and deletes the entries,
 * makes them disagree; the fold then reports it rather than apply anything.
 * Load again, a few times, then fail as a concurrent update would.
 */
#define DG_FOLD_ATTEMPTS 10

static bool journal_fold_moved;

/* Whether the fold that produced |s| found its reads disagree: then |s| is released. */
static bool
fold_again(DGState *s, int attempt)
{
	if (!journal_fold_moved)
		return false;
	dg_release(s);
	if (attempt >= DG_FOLD_ATTEMPTS)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to concurrent update")));
	return true;
}

DGState *
dg_load(Relation rel, Snapshot snap)
{
	for (int attempt = 1;; attempt++)
	{
		bool		foldable;
		DGState    *s = load_unfolded(rel, snap, &foldable);

		if (!foldable)
			return s;
		s = journal_fold_loaded(rel, dg_relation_branch(rel), snap, s);
		if (!fold_again(s, attempt))
			return s;
	}
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
	for (int attempt = 1;; attempt++)
	{
		DGState    *s;

		if (durable)
			s = durable_base(rel, load_state(rel, SnapshotSelf, &id, dg_relation_branch(rel), false, true));
		else
			s = load_state(rel, SnapshotSelf, &id, dg_relation_branch(rel), true, false);
		s = journal_fold_loaded(rel, dg_relation_branch(rel), SnapshotSelf, s);
		if (!fold_again(s, attempt))
			return s;
	}
}

/* Internal page-reader entrypoint; only dg_resource.c supplies this descriptor. */
DGState *
dg_load_bound(Relation rel, Snapshot snap, const char *branch)
{
	Assert(RelFileNumberIsValid(rel->rd_locator.relNumber));
	Assert(rel->rd_lockInfo.lockRelId.relId < FirstCatalogViewObjectId);
	for (int attempt = 1;; attempt++)
	{
		DGState    *s = journal_fold_loaded(rel, branch, snap, load_state(rel, snap, NULL, branch, false, true));

		if (!fold_again(s, attempt))
			return s;
	}
}

DGState *
dg_load_bound_tid(Relation rel, uint64 id, const char *branch)
{
	for (int attempt = 1;; attempt++)
	{
		DGState    *s = journal_fold_loaded(rel, branch, SnapshotSelf, load_state(rel, SnapshotSelf, &id, branch, false, true));

		if (!fold_again(s, attempt))
			return s;
	}
}

static void
save_roots(const RootKey *key, bool table, DGState *s, CommandId cid, SubTransactionId subxid)
{
	Roots		base = {0},
				roots = {.present = true};

	register_callbacks();
	if (s->store != dg_working_store())
		elog(ERROR, "prolly state does not belong to this transaction");
	roots.content = s->content.root;
	if (s->based)
	{
		base.present = true;
		base.content = s->base;
	}
	overlay_push(key, cid, subxid, &base, &roots);

	/* The saved trees hold the entries the state folded: they leave the journal. */
	if (table && s->journal_count)
	{
		journal_consume(key, s->journal_seqs, s->journal_count, cid);
		s->journal_count = 0;
	}
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
 * What a snapshot can see of other transactions' journal rows is fixed by
 * its xmin, xmax and in-progress lists: two snapshots that agree on those see
 * the same committed rows. A load under SnapshotSelf sees what the latest
 * snapshot would. A cached fold is reused only under an identical view, plus
 * the command id for the transaction's own rows; counters that move at
 * commit would lag visibility by the time between a transaction's commit
 * record and its callbacks.
 */
#define JOURNAL_XIP_MAX 16

typedef struct Visibility
{
	TransactionId xmin,
				xmax;
	uint32		xcnt,
				subxcnt;
	TransactionId xip[JOURNAL_XIP_MAX];
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
	if ((uint64) seen->xcnt + seen->subxcnt > JOURNAL_XIP_MAX || seen->suboverflowed)
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

#include "dg_batch.inc"
#include "dg_journal.inc"
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
	Roots		committed,
				view;
	OverlayEntry *entry;

	if (strlen(branch) >= sizeof(key.branch))
		ereport(ERROR, (errcode(ERRCODE_NAME_TOO_LONG), errmsg("branch name is too long for prolly storage")));
	key.space = locator.spcOid;
	key.file = locator.relNumber;
	strlcpy(key.branch, branch, sizeof(key.branch));
	if (!loading_for_batch)
		batch_flush_key(&key);
	committed = roots_read(&key, snapshot);
	view = committed;
	entry = overlay_entry(overlay_find(&key, false), snapshot);
	if (entry)
		view = roots_equal(&entry->base, &committed) ? entry->roots : roots_merge(&entry->base, &entry->roots, &committed);
	/*
	 * Rows journaled to a table's stream are part of its content, though not
	 * yet of its roots: fold them in. An index has no stream of its own;
	 * dg_storage_resource_root sends its callers to a full load instead.
	 */
	{
		JournalEntries entries = journal_entries(&key, snapshot ? snapshot : SnapshotSelf);

		if (entries.count)
		{
			PLTree		tree = view.present ? tree_at(view.content) : empty_tree();

			journal_apply_table(dg_working_store(), &tree, &entries);
			*content = tree.root;
			return true;
		}
	}
	if (!view.present)
		return false;
	*content = view.content;
	return true;
}

/* Whether any journal entry on |branch| is visible to |snapshot|, in any stream. */
bool
dg_journal_pending(const char *branch, Snapshot snapshot)
{
	return journal_branch_pending(branch, snapshot);
}

/* Only the transactional resource allocator calls this, before publishing any
 * branch binding: the new storage starts empty on its branch. A new storage
 * has no journal stream, and its bare descriptor names no definition, so the
 * load does not fold. */
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
	pl_hash_format(s->content.root, result);
	dg_release(s);
	relation_close(rel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/* Make this transaction's batched changes visible to canonical capture. */
void
dg_flush_version_batches(void)
{
	batch_flush_all();
	publish_all();
}
