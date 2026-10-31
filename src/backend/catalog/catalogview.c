/*
 * Canonical providers publish immutable catalog tuples through this boundary.
 * A view owns its rows and isolated native catcaches. Physical catalog rows are
 * not consulted when the view provides a catalog, including an empty catalog.
 */
#include "postgres.h"
#include "access/xact.h"
#include "access/transam.h"
#include "access/parallel.h"
#include "utils/syscache.h"
#include "catalog/pg_namespace.h"
#include "fmgr.h"
#include "utils/lsyscache.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/nbtree.h"
#include "catalog/catalogview.h"
#include "access/doltcatalog.h"
#include "executor/tuptable.h"
#include "utils/catcache.h"
#include "utils/rel.h"
#include "utils/hsearch.h"
#include "catalog/pg_type_d.h"
#include "utils/snapshot.h"
#include "utils/sortsupport.h"
#include "catalog/pg_index.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_statistic.h"
#include "utils/inval.h"
#include "utils/fmgroids.h"
#include "utils/typcache.h"

/*
 * Durable OIDs belong to a branch, not to one view: each schema version of
 * the branch is a different view binding the same OIDs. This backend keeps
 * the view it last resolved for each durable OID and asks the resolver again
 * after any relcache invalidation of a durable relation. Relcache forgets the
 * views before it rebuilds open entries, so a rebuild sees the new schema.
 */
typedef struct DurableContext
{
	Oid			oid;
	CatalogView *view;
} DurableContext;
static HTAB *durable_contexts;

typedef struct CatalogViewRows
{
	Oid			catalog;
	List	   *tuples;
} CatalogViewRows;

struct CatalogView
{
	MemoryContext memory;
	MemoryContext rows_memory;
	bool		provisional;
	bool		branch_head;	/* a branch's working schema, not a revision */
	bool		retired;
	List	   *baseline;
	List	   *changes;
	uint64		next_tuple;
	bool		dirty;
	void		(*mutation_validator) (Relation, HeapTuple);
	void		(*storage_handler) (Relation, CatalogViewStorageEffect);
	List	   *catalog_ids;
	uint64		touched;
	TransactionId creator;
	unsigned	scan_references;
	bool		loaded;
	bool		loading;
	void		(*loader) (CatalogView *, void *);
	void	   *loader_arg;
	char	   *identity;
	uint64		serial;
	uint64		references;
	bool		sealed;
	bool		partial;
	List	   *catalogs;
	List	   *caches;
	List	   *handles;
	List	   *durable;		/* CatalogViewObjects bound to durable OIDs */
	List	   *shared;			/* CatalogViewObjects bound to OIDs other views share */
	CatalogViewBaseFilter base_filter;	/* which base rows the view leaves out */
	void	   *base_filter_arg;
	List	   *indexes;		/* CatalogViewIndexes over the rows */
	MemoryContext index_memory;
	CatalogViewBase *base;		/* shared rows each catalog's rows begin with */
	dlist_node	link;
};

/* Rows several views share, which none of them copies. */
struct CatalogViewBase
{
	MemoryContext memory;
	uint64		references;
	List	   *catalogs;		/* CatalogViewRows */
};

/*
 * The rows of one catalog by the value of one of their OID columns, built
 * when first asked for and forgotten whenever the view's rows change.
 */
typedef struct CatalogViewIndex
{
	Oid			catalog;
	AttrNumber	attribute;
	HTAB	   *entries;		/* CatalogViewIndexEntry */
} CatalogViewIndex;

typedef struct CatalogViewIndexEntry
{
	Oid			value;
	List	   *tuples;
} CatalogViewIndexEntry;

static void
forget_indexes(CatalogView *view)
{
	view->indexes = NIL;
	MemoryContextReset(view->index_memory);
}
typedef struct CatalogViewPin
{
	MemoryContextCallback callback;
	CatalogView *view;
} CatalogViewPin;

struct CatalogViewScan
{
	CatalogView *view;
	CatalogViewPin *pin;
	Relation	relation;
	List	   *rows;
	bool		owns_rows;
	HeapTuple  *ordered;
	int			position;
	bool		started;
	int			nkeys;
	ScanKey		keys;
};
typedef struct CatalogViewObject
{
	Oid			handle;
	Oid			objectclass;
	Oid			storage;
	bytea	   *schema;			/* a relation's retained schema object ... */
	bytea	   *content;		/* ... and content root, as of its revision */
	CatalogView *view;
	char	   *identity;
} CatalogViewObject;
static HTAB *objects;
static Oid	next_handle = FirstCatalogViewObjectId;
static CatalogView *routing_context(Oid oid);

static uint64 next_view_serial = 1;
static uint64 view_clock;
int			catalog_view_cache_entries = 16;
static void prune_catalog_views(XactEvent event, void *arg);
static bool prune_registered;
static bool stage_tuple(Relation relation, ItemPointer tid, HeapTuple tuple);
static void refresh_mutation(CatalogView *view);
static CatalogView *current_view;
static dlist_head views = DLIST_STATIC_INIT(views);

CatalogView *
CatalogViewCreate(const char *identity)
{
	MemoryContext memory;
	MemoryContext previous;
	CatalogView *view;

	if (IsParallelWorker())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("revision catalog contexts cannot be constructed in parallel workers")));
	memory = AllocSetContextCreate(TopMemoryContext, "catalog view", ALLOCSET_DEFAULT_SIZES);
	previous = MemoryContextSwitchTo(memory);
	view = palloc0(sizeof(*view));

	view->memory = memory;
	view->rows_memory = AllocSetContextCreate(memory, "catalog view rows", ALLOCSET_DEFAULT_SIZES);
	view->index_memory = AllocSetContextCreate(memory, "catalog view indexes", ALLOCSET_SMALL_SIZES);
	view->touched = ++view_clock;
	if (!prune_registered)
	{
		RegisterXactCallback(prune_catalog_views, NULL);
		prune_registered = true;
	}
	view->identity = pstrdup(identity);
	MemoryContextSetIdentifier(memory, view->identity);
	if (!next_view_serial)
		elog(ERROR, "catalog context identifier space exhausted");
	view->serial = next_view_serial++;
	view->references = 1;
	dlist_push_tail(&views, &view->link);
	MemoryContextSwitchTo(previous);
	return view;
}

void
CatalogViewAddRows(CatalogView *view, Oid catalog, List *tuples)
{
	MemoryContext previous;
	CatalogViewRows *rows;
	ListCell   *lc;
	uint32		ordinal = 0;

	if (view->sealed && !view->loading)
		elog(ERROR, "cannot modify published catalog view");
	forget_indexes(view);
	foreach(lc, view->catalogs)
	{
		if (((CatalogViewRows *) lfirst(lc))->catalog != catalog)
			continue;

		/* A view still being built may replace rows it published early. */
		if (!view->partial)
			elog(ERROR, "duplicate catalog rows in view");
		view->catalogs = foreach_delete_current(view->catalogs, lc);
		foreach_ptr(CatCache, cache, view->caches)
			ResetCatalogCache(cache, false);
		break;
	}
	previous = MemoryContextSwitchTo(view->memory);
	if (!list_member_oid(view->catalog_ids, catalog))
		view->catalog_ids = lappend_oid(view->catalog_ids, catalog);
	MemoryContextSwitchTo(view->rows_memory);
	rows = palloc0(sizeof(*rows));
	rows->catalog = catalog;
	if (view->base)
	{
		foreach_ptr(CatalogViewRows, shared, view->base->catalogs)
		{
			if (shared->catalog != catalog)
				continue;
			if (!view->base_filter)
				rows->tuples = list_copy(shared->tuples);
			else
				foreach_ptr(HeapTupleData, tuple, shared->tuples)
					if (view->base_filter(catalog, tuple, view->base_filter_arg))
						rows->tuples = lappend(rows->tuples, tuple);
		}
	}
	foreach(lc, tuples)
	{
		HeapTuple	tuple = heap_copytuple(lfirst(lc));

		/*
		 * Catcache lists need a tuple identity; it is local to this
		 * catalog/view.
		 */
		ItemPointerSet(&tuple->t_self, CatalogViewFirstBlock + ordinal / MaxOffsetNumber, ordinal % MaxOffsetNumber + 1);
		ordinal++;
		rows->tuples = lappend(rows->tuples, tuple);
	}
	view->catalogs = lappend(view->catalogs, rows);
	MemoryContextSwitchTo(previous);
}

CatalogViewBase *
CatalogViewBaseCreate(void)
{
	MemoryContext memory = AllocSetContextCreate(TopMemoryContext, "catalog view base", ALLOCSET_DEFAULT_SIZES);
	CatalogViewBase *base = MemoryContextAllocZero(memory, sizeof(*base));

	base->memory = memory;
	base->references = 1;
	return base;
}

/* Copy |tuples| into |base| as its rows of |catalog|. */
void
CatalogViewBaseAddRows(CatalogViewBase *base, Oid catalog, List *tuples)
{
	MemoryContext previous = MemoryContextSwitchTo(base->memory);
	CatalogViewRows *rows = palloc0(sizeof(*rows));
	uint32		ordinal = 0;

	rows->catalog = catalog;
	foreach_ptr(HeapTupleData, source, tuples)
	{
		HeapTuple	tuple = heap_copytuple(source);

		ItemPointerSet(&tuple->t_self, CatalogViewBaseBlock + ordinal / MaxOffsetNumber, ordinal % MaxOffsetNumber + 1);
		ordinal++;
		rows->tuples = lappend(rows->tuples, tuple);
	}
	base->catalogs = lappend(base->catalogs, rows);
	MemoryContextSwitchTo(previous);
}

void
CatalogViewBaseRelease(CatalogViewBase *base)
{
	Assert(base->references > 0);
	if (--base->references == 0)
		MemoryContextDelete(base->memory);
}

/* The memory |view| lives in, for what its owner keeps with it. */
MemoryContext
CatalogViewMemory(CatalogView *view)
{
	return view->memory;
}

/*
 * Leave out of |view| the base rows for which |filter| returns false. Set
 * before the view's rows are added.
 */
void
CatalogViewSetBaseFilter(CatalogView *view, CatalogViewBaseFilter filter, void *arg)
{
	view->base_filter = filter;
	view->base_filter_arg = arg;
}

/* Begin each catalog |view| is given with |base|'s rows of it. */
void
CatalogViewSetBase(CatalogView *view, CatalogViewBase *base)
{
	if (view->base)
		CatalogViewBaseRelease(view->base);
	view->base = base;
	base->references++;
}

void
CatalogViewSeal(CatalogView *view)
{
	view->sealed = true;
	view->loaded = true;
	view->partial = false;
}

/*
 * Let a view under construction, or being reloaded, be selected once some of
 * its catalogs are complete, so that later rows can be bound against earlier
 * ones. Lookups in catalogs it does not contain yet use the physical catalogs.
 * Sealing the view ends this state.
 */
void
CatalogViewSetPartial(CatalogView *view, bool partial)
{
	view->partial = partial;
}

bool
CatalogViewIsPartial(CatalogView *view)
{
	return view && view->partial;
}

void
CatalogViewRetain(CatalogView *view)
{
	Assert(view->references > 0);
	view->references++;
}

uint64
CatalogViewReferences(CatalogView *view)
{
	return view->references;
}

void
CatalogViewRelease(CatalogView *view)
{
	ListCell   *lc;

	Assert(view->references > 0);
	if (--view->references)
		return;
	Assert(current_view != view);
	foreach(lc, view->caches)
		DestroyCatCache(lfirst(lc));
	foreach(lc, view->handles)
	{
		Oid			handle = lfirst_oid(lc);

		hash_search(objects, &handle, HASH_REMOVE, NULL);
	}
	dlist_delete(&view->link);
	if (view->base)
		CatalogViewBaseRelease(view->base);
	MemoryContextDelete(view->memory);
}

CatalogView *
CatalogViewCurrent(void)
{
	return current_view;
}

CatalogView *
CatalogViewSwitch(CatalogView *view)
{
	CatalogView *previous = current_view;

	Assert(!view || view->sealed || view->partial);
	if (view)
		CatalogViewEnsureLoaded(view);
	current_view = view;
	return previous;
}

bool
CatalogViewContains(CatalogView *view, Oid catalog)
{
	return view && list_member_oid(view->catalog_ids, catalog);
}

CatCache *
CatalogViewCache(CatalogView *view, CatCache *base)
{
	ListCell   *lc;
	CatCache   *cache;
	MemoryContext previous;

	if (!CatalogViewContains(view, base->cc_reloid))
		return base;
	CatalogViewEnsureLoaded(view);
	foreach(lc, view->caches)
	{
		cache = lfirst(lc);
		if (cache->id == base->id)
			return cache;
	}
	cache = InitCatCache(base->id, base->cc_reloid, base->cc_indexoid,
						 base->cc_nkeys, base->cc_keyno, base->cc_nbuckets);
	cache->cc_catalogview = view;
	previous = MemoryContextSwitchTo(view->memory);
	view->caches = lappend(view->caches, cache);
	MemoryContextSwitchTo(previous);
	return cache;
}

void
CatalogViewInvalidateCache(int cacheid, uint32 hashvalue)
{
	dlist_iter	it;

	dlist_foreach(it, &views)
	{
		CatalogView *view = dlist_container(CatalogView, link, it.cur);
		ListCell   *lc;

		foreach(lc, view->caches)
		{
			CatCache   *cache = lfirst(lc);

			if (cache->id == cacheid)
				CatCacheInvalidate(cache, hashvalue);
		}
	}
}

static void
release_scan_pin(void *arg)
{
	CatalogViewPin *pin = arg;

	if (pin->view)
	{
		CatalogView *view = pin->view;

		pin->view = NULL;
		Assert(view->scan_references > 0);
		view->scan_references--;
		if (view->retired && !view->scan_references)
		{
			view->baseline = view->catalogs = view->changes = NIL;
			MemoryContextReset(view->rows_memory);
			forget_indexes(view);
		}
		CatalogViewRelease(view);
	}
}

static List *mutation_self_rows(CatalogView *view, Oid catalog);

CatalogViewScan *
CatalogViewBeginScan(Relation relation, int nkeys, ScanKey keys)
{
	return CatalogViewBeginScanSnapshot(relation, nkeys, keys, NULL);
}

CatalogViewScan *
CatalogViewBeginScanSnapshot(Relation relation, int nkeys, ScanKey keys, Snapshot snapshot)
{
	CatalogViewScan *scan;
	ListCell   *lc;
	CatalogView *view = current_view;
	bool		has_bound_key = false;
	int			i;

	/* Statistics are physical: a branch's tables keep theirs by durable OID. */
	if (RelationGetRelid(relation) == StatisticRelationId)
		return NULL;

	for (i = 0; i < nkeys; i++)
	{
		int			attr = keys[i].sk_attno;

		if (!(keys[i].sk_flags & (SK_ISNULL | SK_SEARCHARRAY | SK_ROW_HEADER)) && attr > 0 && CatalogViewIsObjectIdType(TupleDescAttr(RelationGetDescr(relation), attr - 1)->atttypid) &&
			(!CatalogViewIsDurable(DatumGetObjectId(keys[i].sk_argument)) || dg_catalog_provides(RelationGetRelid(relation))))
		{
			CatalogView *bound = routing_context(DatumGetObjectId(keys[i].sk_argument));

			if (bound)
			{
				view = bound;
				has_bound_key = true;
				break;
			}
		}
	}
	if (!CatalogViewContains(view, RelationGetRelid(relation)))
	{
		/* A view still being built has only some catalogs as yet. */
		if (has_bound_key && !(view && view->partial))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("catalog scan of %u cannot follow this revision-bound handle", RelationGetRelid(relation))));
		return NULL;
	}
	CatalogViewEnsureLoaded(view);
	scan = palloc0(sizeof(*scan));
	scan->view = view;
	view->scan_references++;
	CatalogViewRetain(scan->view);
	scan->pin = palloc0(sizeof(CatalogViewPin));
	scan->pin->view = scan->view;
	scan->pin->callback.func = release_scan_pin;
	scan->pin->callback.arg = scan->pin;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &scan->pin->callback);
	scan->relation = relation;
	scan->nkeys = nkeys;
	scan->keys = palloc(sizeof(ScanKeyData) * Max(nkeys, 1));
	if (nkeys)
		memcpy(scan->keys, keys, sizeof(ScanKeyData) * nkeys);
	foreach(lc, view->catalogs)
	{
		CatalogViewRows *rows = lfirst(lc);

		if (rows->catalog == RelationGetRelid(relation))
		{
			scan->rows = rows->tuples;
			break;
		}
	}

	/* An OID equality key selects its rows through an index. */
	for (i = 0; i < nkeys && !(CatalogViewIsProvisional(view) && snapshot && snapshot->snapshot_type == SNAPSHOT_SELF); i++)
	{
		int			attr = keys[i].sk_attno;

		if (keys[i].sk_flags == 0 && keys[i].sk_strategy == BTEqualStrategyNumber &&
			keys[i].sk_func.fn_oid == F_OIDEQ && attr > 0 &&
			CatalogViewIsObjectIdType(TupleDescAttr(RelationGetDescr(relation), attr - 1)->atttypid))
		{
			scan->rows = CatalogViewTuplesWith(view, RelationGetRelid(relation), attr, DatumGetObjectId(keys[i].sk_argument));
			scan->owns_rows = true;
			break;
		}
	}
	if (CatalogViewIsProvisional(view) && snapshot && snapshot->snapshot_type == SNAPSHOT_SELF)
	{
		scan->rows = mutation_self_rows(view, RelationGetRelid(relation));
		scan->owns_rows = true;
	}

	return scan;
}

HeapTuple
CatalogViewNext(CatalogViewScan *scan, ScanDirection direction)
{
	while (true)
	{
		HeapTuple	tuple;
		bool		match = true;
		int			i;

		if (!scan->started)
		{
			scan->position = ScanDirectionIsBackward(direction) ? list_length(scan->rows) : -1;
			scan->started = true;
		}
		if (ScanDirectionIsBackward(direction))
		{
			if (scan->position > -1)
				scan->position--;
			if (scan->position < 0)
				return NULL;
		}
		else
		{
			if (scan->position < list_length(scan->rows))
				scan->position++;
			if (scan->position >= list_length(scan->rows))
				return NULL;
		}
		tuple = scan->ordered ? scan->ordered[scan->position] : list_nth(scan->rows, scan->position);
		for (i = 0; i < scan->nkeys; i++)
		{
			ScanKey		key = &scan->keys[i];
			bool		isnull;
			Datum		value = heap_getattr(tuple, key->sk_attno, RelationGetDescr(scan->relation), &isnull);

			if (key->sk_flags & (SK_ROW_HEADER | SK_SEARCHARRAY))
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unsupported versioned catalog scan key")));
			if (key->sk_flags & SK_SEARCHNULL)
				match = isnull;
			else if (key->sk_flags & SK_SEARCHNOTNULL)
				match = !isnull;
			else
				match = !isnull && !(key->sk_flags & SK_ISNULL) &&
					DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation, value, key->sk_argument));
			if (!match)
				break;
		}
		if (match)
			return tuple;
	}
}

void
CatalogViewRescan(CatalogViewScan *scan, ScanKey keys)
{
	scan->position = 0;
	scan->started = false;
	if (keys && scan->nkeys)
		memcpy(scan->keys, keys, sizeof(ScanKeyData) * scan->nkeys);
}

void
CatalogViewEndScan(CatalogViewScan *scan)
{
	release_scan_pin(scan->pin);
	if (scan->owns_rows)
		list_free(scan->rows);
	if (scan->ordered)
		pfree(scan->ordered);
	pfree(scan->keys);
	pfree(scan);
}

Oid
CatalogViewBindObject(CatalogView *view, Oid objectclass, const char *identity, Oid storage)
{
	CatalogViewObject *object;
	MemoryContext previous;
	ListCell   *lc;
	bool		found;

	if (!objects)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(CatalogViewObject);
		ctl.hcxt = TopMemoryContext;
		objects = hash_create("catalog context handles", 128, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	foreach(lc, view->handles)
	{
		Oid			handle = lfirst_oid(lc);

		object = hash_search(objects, &handle, HASH_FIND, NULL);
		if (object->objectclass == objectclass && !strcmp(object->identity, identity))
		{
			if (object->storage != storage)
				elog(ERROR, "catalog handle storage changed within immutable context");
			return handle;
		}
	}
	if (view->sealed && !view->provisional)
		elog(ERROR, "cannot bind a new object in a published catalog context");

	/*
	 * Keep the high-water mark representable so stale handles stay
	 * detectable.
	 */
	if (next_handle == PG_UINT32_MAX)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("backend catalog handle space exhausted")));
	object = hash_search(objects, &next_handle, HASH_ENTER, &found);
	Assert(!found);
	object->view = view;
	object->objectclass = objectclass;
	object->storage = storage;
	previous = MemoryContextSwitchTo(view->memory);
	object->identity = pstrdup(identity);
	view->handles = lappend_oid(view->handles, next_handle);
	MemoryContextSwitchTo(previous);
	return next_handle++;
}

static CatalogViewObject *
view_durable(CatalogView *view, Oid oid)
{
	ListCell   *lc;

	foreach(lc, view->durable)
	{
		CatalogViewObject *object = lfirst(lc);

		if (object->handle == oid)
			return object;
	}
	return NULL;
}

void
CatalogViewBindDurable(CatalogView *view, Oid objectclass, const char *identity, Oid oid, Oid storage)
{
	CatalogViewObject *object;
	MemoryContext previous;
	ListCell   *lc;

	Assert(CatalogViewIsDurable(oid));
	foreach(lc, view->durable)
	{
		object = lfirst(lc);
		if (object->handle == oid || (object->objectclass == objectclass && !strcmp(object->identity, identity)))
		{
			if (object->handle != oid || object->objectclass != objectclass || strcmp(object->identity, identity) || object->storage != storage)
				elog(ERROR, "durable object %u is bound twice in one catalog context", oid);
			return;
		}
	}
	previous = MemoryContextSwitchTo(view->memory);
	object = palloc0(sizeof(*object));
	object->handle = oid;
	object->objectclass = objectclass;
	object->storage = storage;
	object->view = view;
	object->identity = pstrdup(identity);
	view->durable = lappend(view->durable, object);
	MemoryContextSwitchTo(previous);
}

/*
 * Bind |identity| to |oid| in |view|, an OID other views may bind too: it
 * names one object everywhere, as a namespace's OID does, and no view owns
 * it, so lookups by it use whichever view is current.
 */
void
CatalogViewBindShared(CatalogView *view, Oid objectclass, const char *identity, Oid oid)
{
	CatalogViewObject *object;
	MemoryContext previous;

	foreach_ptr(CatalogViewObject, bound, view->shared)
	{
		if (bound->handle == oid || (bound->objectclass == objectclass && !strcmp(bound->identity, identity)))
		{
			if (bound->handle != oid || bound->objectclass != objectclass || strcmp(bound->identity, identity))
				elog(ERROR, "shared object %u is bound twice in one catalog context", oid);
			return;
		}
	}
	previous = MemoryContextSwitchTo(view->memory);
	object = palloc0(sizeof(*object));
	object->handle = oid;
	object->objectclass = objectclass;
	object->view = view;
	object->identity = pstrdup(identity);
	view->shared = lappend(view->shared, object);
	MemoryContextSwitchTo(previous);
}

/* Whether |view| binds |oid|: shared, durable, or a handle of its own. */
bool
CatalogViewBinds(CatalogView *view, Oid oid)
{
	CatalogView *previous;
	bool		result;

	if (!view || !OidIsValid(oid))
		return false;
	previous = CatalogViewSwitch(view);
	result = CatalogViewObjectContext(oid) == view;
	CatalogViewSwitch(previous);
	return result;
}

void
CatalogViewForgetDurable(void)
{
	HASH_SEQ_STATUS status;
	DurableContext *entry;

	if (!durable_contexts)
		return;
	hash_seq_init(&status, durable_contexts);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		CatalogView *view = entry->view;

		hash_search(durable_contexts, &entry->oid, HASH_REMOVE, NULL);
		CatalogViewRelease(view);
	}
}

/* The current view for a durable OID; NULL if no branch defines it. */
static CatalogView *
durable_context(Oid oid)
{
	DurableContext *entry;
	CatalogView *view;
	bool		found;

	if (!durable_contexts)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(DurableContext);
		ctl.hcxt = TopMemoryContext;
		durable_contexts = hash_create("durable catalog contexts", 128, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	entry = hash_search(durable_contexts, &oid, HASH_FIND, NULL);
	if (entry)
		return entry->view;
	if (IsParallelWorker())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("branch catalog objects cannot be resolved in parallel workers")));
	view = dg_catalog_resolve_durable(oid);
	if (!view || !view->sealed)
		return NULL;			/* unknown, or the view is still being built */
	if (!view_durable(view, oid))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
						errmsg("object %u does not exist in its branch's current schema", oid)));
	entry = hash_search(durable_contexts, &oid, HASH_ENTER, &found);
	if (!found)
	{
		entry->view = view;
		CatalogViewRetain(view);
	}
	return entry->view;
}

static CatalogViewObject *
lookup_object(Oid handle)
{
	CatalogViewObject *object;

	if (CatalogViewIsDurable(handle))
	{
		CatalogView *view = durable_context(handle);

		return view ? view_durable(view, handle) : NULL;
	}

	if (handle >= FirstCatalogViewObjectId && IsParallelWorker())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("runtime catalog handles cannot be resolved in parallel workers")));

	/* An OID views share is the current view's object, if it binds it. */
	if (handle < FirstCatalogViewObjectId)
	{
		CatalogView *view = CatalogViewCurrent();

		if (view)
			foreach_ptr(CatalogViewObject, object, view->shared)
			{
				if (object->handle == handle)
					return object;
			}
		return NULL;
	}
	if (handle >= next_handle)
		return NULL;
	object = objects ? hash_search(objects, &handle, HASH_FIND, NULL) : NULL;
	if (!object)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("catalog object handle %u is no longer bound to a revision", handle)));
	return object;
}

/*
 * The view a key OID sends a lookup to: the one a durable OID or a handle
 * belongs to. An OID views share is also a physical one, so it sends a
 * lookup nowhere in particular.
 */
static CatalogView *
routing_context(Oid oid)
{
	if (oid < FirstCatalogViewObjectId && !CatalogViewIsDurable(oid))
		return NULL;
	return CatalogViewObjectContext(oid);
}

/* Only these by-value SQL types can carry catalog object handles.  Never
 * interpret arbitrary scalar values or composite pointers as identifiers. */
bool
CatalogViewIsObjectIdType(Oid type)
{
	switch (type)
	{
		case OIDOID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGNAMESPACEOID:
			return true;
		default:
			return false;
	}
}

void
CatalogViewMarkBranchHead(CatalogView *view)
{
	view->branch_head = true;
}

CatalogView *
CatalogViewObjectContext(Oid handle)
{
	CatalogViewObject *object = lookup_object(handle);

	return object ? object->view : NULL;
}

/*
 * The view an OID value names, as data rather than as a binding: a value equal
 * to a handle since released is only a number, and names none.
 */
CatalogView *
CatalogViewValueContext(Oid value)
{
	if (value >= FirstCatalogViewObjectId && value < next_handle &&
		!(objects && hash_search(objects, &value, HASH_FIND, NULL)))
		return NULL;
	return CatalogViewObjectContext(value);
}

Oid
CatalogViewObjectStorage(Oid handle)
{
	CatalogViewObject *object = lookup_object(handle);

	return object ? object->storage : handle;
}

/*
 * Record the retained schema object and content root of relation |handle|,
 * bound in |view|, which may still be under construction.
 */
void
CatalogViewObjectSetRetained(CatalogView *view, Oid handle, bytea *schema, bytea *content)
{
	CatalogViewObject *object = CatalogViewIsDurable(handle) ? view_durable(view, handle) :
		(objects ? hash_search(objects, &handle, HASH_FIND, NULL) : NULL);
	MemoryContext previous;

	if (!object || object->view != view)
		elog(ERROR, "catalog object %u is not bound in this context", handle);
	previous = MemoryContextSwitchTo(object->view->memory);
	object->schema = schema ? DatumGetByteaPCopy(PointerGetDatum(schema)) : NULL;
	object->content = content ? DatumGetByteaPCopy(PointerGetDatum(content)) : NULL;
	MemoryContextSwitchTo(previous);
}

/* The retained schema object a relation was bound with, or NULL. */
bytea *
CatalogViewObjectSchema(Oid handle)
{
	CatalogViewObject *object = lookup_object(handle);

	return object ? object->schema : NULL;
}

/* The content root of a relation as of its revision, or NULL. */
bytea *
CatalogViewObjectContent(Oid handle)
{
	CatalogViewObject *object = lookup_object(handle);

	return object ? object->content : NULL;
}

CatalogView *
CatalogViewSelectKeys(CatalogView *fallback, TupleDesc desc, int nkeys, const int *attributes, const Datum *values)
{
	int			i;

	for (i = 0; i < nkeys; i++)
	{
		if (attributes[i] > 0 && CatalogViewIsObjectIdType(TupleDescAttr(desc, attributes[i] - 1)->atttypid))
		{
			CatalogView *view = routing_context(DatumGetObjectId(values[i]));

			if (view)
				return view;
		}
	}
	return fallback;
}

/* Sort the provider's tuples using the same operator family as the native index.
 * The index supplies ordering metadata only; its physical pages are never read.
 */
typedef struct CatalogViewOrder
{
	TupleDesc	desc;
	int			nkeys;
	AttrNumber *attributes;
	SortSupportData *support;
} CatalogViewOrder;

static int
compare_catalog_tuples(const void *left, const void *right, void *argument)
{
	CatalogViewOrder *order = argument;
	HeapTuple	a = *((HeapTuple const *) left);
	HeapTuple	b = *((HeapTuple const *) right);
	int			i;

	for (i = 0; i < order->nkeys; i++)
	{
		bool		anull,
					bnull;
		Datum		av = heap_getattr(a, order->attributes[i], order->desc, &anull);
		Datum		bv = heap_getattr(b, order->attributes[i], order->desc, &bnull);
		int			result = ApplySortComparator(av, anull, bv, bnull, &order->support[i]);

		if (result)
			return result;
	}
	return 0;
}

void
CatalogViewOrderScan(CatalogViewScan *scan, Relation index)
{
	CatalogViewOrder order;
	MemoryContext memory,
				previous;
	ListCell   *lc;
	int			i = 0;

	Assert(!scan->started && !scan->ordered);
	scan->ordered = palloc_array(HeapTuple, Max(list_length(scan->rows), 1));
	foreach(lc, scan->rows) scan->ordered[i++] = lfirst(lc);
	memory = AllocSetContextCreate(CurrentMemoryContext, "catalog scan ordering", ALLOCSET_SMALL_SIZES);
	previous = MemoryContextSwitchTo(memory);
	order.desc = RelationGetDescr(scan->relation);
	order.nkeys = IndexRelationGetNumberOfKeyAttributes(index);
	order.attributes = index->rd_index->indkey.values;
	order.support = palloc0_array(SortSupportData, order.nkeys);
	for (i = 0; i < order.nkeys; i++)
	{
		SortSupport support = &order.support[i];

		if (order.attributes[i] <= 0)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("expression indexes cannot order a catalog view")));
		support->ssup_cxt = memory;
		support->ssup_attno = i + 1;
		support->ssup_collation = index->rd_indcollation[i];
		support->ssup_nulls_first = (index->rd_indoption[i] & INDOPTION_NULLS_FIRST) != 0;
		support->abbreviate = false;
		PrepareSortSupportFromIndexRel(index, (index->rd_indoption[i] & INDOPTION_DESC) != 0, support);
	}
	qsort_arg(scan->ordered, list_length(scan->rows), sizeof(HeapTuple), compare_catalog_tuples, &order);
	MemoryContextSwitchTo(previous);
	MemoryContextDelete(memory);
}

/* A provider tuple's synthetic TID must never be interpreted as a heap offset. */
void
CatalogViewRejectPhysicalAccess(Relation relation)
{
	if (CatalogViewContains(CatalogViewCurrent(), RelationGetRelid(relation)))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("physical access to revision-bound catalog %s is not supported", RelationGetRelationName(relation))));
}

/*
 * Native DDL writes physical catalogs. It must never change a row it read
 * from a branch's schema, nor make a physical row refer to a branch's object:
 * those objects change only through their branch's canonical schema.
 */
void
CatalogViewRejectBranchWrite(Relation relation, ItemPointer tid, HeapTuple tuple)
{
	TupleDesc	desc = RelationGetDescr(relation);

	if (tid && ItemPointerGetBlockNumberNoCheck(tid) >= CatalogViewFirstBlock)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("this command cannot change a versioned object")));
	if (!tuple || strncmp(RelationGetRelationName(relation), "pg_dolt_", 8) == 0)
		return;
	for (int i = 0; i < desc->natts; i++)
	{
		bool		isnull;
		Datum		value;

		if (!CatalogViewIsObjectIdType(TupleDescAttr(desc, i)->atttypid))
			continue;

		/* A branch's table keeps its statistics under its durable OID. */
		if (RelationGetRelid(relation) == StatisticRelationId && i + 1 == Anum_pg_statistic_starelid)
			continue;

		value = heap_getattr(tuple, i + 1, desc, &isnull);
		if (!isnull && DatumGetObjectId(value) >= FirstDurableObjectId)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("this command cannot make an unversioned object depend on a versioned one")));
	}
}

Oid
CatalogViewFindObject(CatalogView *view, Oid objectclass, const char *identity)
{
	ListCell   *lc;

	foreach_ptr(CatalogViewObject, object, view->shared)
	{
		if (object->objectclass == objectclass && !strcmp(object->identity, identity))
			return object->handle;
	}
	foreach(lc, view->durable)
	{
		CatalogViewObject *object = lfirst(lc);

		if (object->objectclass == objectclass && !strcmp(object->identity, identity))
			return object->handle;
	}

	foreach(lc, view->handles)
	{
		Oid			handle = lfirst_oid(lc);
		CatalogViewObject *object = hash_search(objects, &handle, HASH_FIND, NULL);

		if (object->objectclass == objectclass && !strcmp(object->identity, identity))
			return handle;
	}
	return InvalidOid;
}

Oid
CatalogViewLookupObject(CatalogView *view, Oid objectclass, const char *identity)
{
	Oid			result = CatalogViewFindObject(view, objectclass, identity);

	if (OidIsValid(result))
		return result;
	ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("object %s does not exist in catalog context", identity)));
	return InvalidOid;
}

uint64
CatalogViewCacheId(CatalogView *view)
{
	return view ? view->serial : 0;
}

/* Select a single invocation scope for SQL and procedural handlers alike. */
CatalogView *
CatalogViewCallContext(FunctionCallInfo fcinfo)
{
	CatalogView *view = NULL;
	int			i;
	Oid		   *declared = NULL;
	int			nargs = 0;

	if (fcinfo->flinfo->fn_expr && IsA(fcinfo->flinfo->fn_expr, FuncExpr) &&
		((FuncExpr *) fcinfo->flinfo->fn_expr)->dolt_working)
		return NULL;
	/*
	 * A routine from a revision runs in that revision's catalogs. One on a
	 * branch head runs like any routine, bound by its arguments.
	 */
	view = CatalogViewObjectContext(fcinfo->flinfo->fn_oid);
	if (view && !view->branch_head)
		return view;
	view = NULL;

	for (i = 0; i < fcinfo->nargs; i++)
	{
		Oid			type = get_fn_expr_argtype(fcinfo->flinfo, i);
		CatalogView *bound;

		/* Internal fmgr callers need not supply an expression tree. */
		if (!OidIsValid(type))
		{
			if (!declared)
				(void) get_func_signature(fcinfo->flinfo->fn_oid, &declared, &nargs);
			if (i < nargs)
				type = declared[i];
		}
		bound = CatalogViewObjectContext(type);
		if (!fcinfo->args[i].isnull &&
			CatalogViewIsObjectIdType(type))
			bound = CatalogViewValueContext(DatumGetObjectId(fcinfo->args[i].value));
		if (bound && view && view != bound)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("SQL function arguments span incompatible catalog contexts")));
		if (bound)
			view = bound;
	}
	if (declared)
		pfree(declared);
	if (!view)
		view = CatalogViewCurrent();
	return view;
}

/* Search-path entries are session-global; translate them at the consumer. */
Oid
CatalogViewNamespace(CatalogView *view, Oid namespaceid)
{
	CatalogView *previous;
	char	   *name;
	Oid			result;

	if (!view || !OidIsValid(namespaceid) || CatalogViewObjectContext(namespaceid))
		return namespaceid;
	previous = CatalogViewSwitch(NULL);
	PG_TRY();
	{
		name = get_namespace_name(namespaceid);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	result = name ? CatalogViewFindObject(view, NamespaceRelationId, name) : InvalidOid;
	return OidIsValid(result) ? result : namespaceid;
}

/* Resolve an already selected runtime function handle, not a name in the
 * body's catalog scope. Shared helpers can execute over historical arguments. */
HeapTuple
CatalogViewFunctionTuple(Oid functionid)
{
	CatalogView *previous = CatalogViewSwitch(CatalogViewObjectContext(functionid));
	HeapTuple	result;

	PG_TRY();
	{
		result = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionid));
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

const char *
CatalogViewObjectIdentity(Oid handle, Oid objectclass)
{
	CatalogViewObject *object = lookup_object(handle);

	if (!object || object->objectclass != objectclass)
		elog(ERROR, "catalog handle %u does not identify class %u", handle, objectclass);
	return object->identity;
}

bool
CatalogViewCaptureUpdate(Relation relation, ItemPointer tid, HeapTuple tuple)
{
	return stage_tuple(relation, tid, tuple);
}

/* Keep handle identities for the backend lifetime, but bound decoded row/cache
 * residency. Plans and type caches may hold those handles after eviction. */
void
CatalogViewSetLoader(CatalogView *view, void (*loader) (CatalogView *, void *), void *arg)
{
	view->loader = loader;
	view->loader_arg = arg;
}

void
CatalogViewEnsureLoaded(CatalogView *view)
{
	view->touched = ++view_clock;
	if (view->retired)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("provisional catalog context has been retired")));
	if (CatalogViewCreatorAborted(view))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("catalog handle refers to an aborted schema revision")));
	if (view->loaded || view->partial)
		return;
	if (!view->loader || view->loading)
		elog(ERROR, "catalog view cannot reload its rows");
	view->loading = true;
	PG_TRY();
	{
		view->loader(view, view->loader_arg);
		if (!view->loaded)
			elog(ERROR, "catalog view loader did not seal its rows");
	}
	PG_CATCH();
	{
		view->loading = false;
		view->loaded = false;
		view->catalogs = NIL;
		MemoryContextReset(view->rows_memory);
		forget_indexes(view);
		PG_RE_THROW();
	}
	PG_END_TRY();
	view->loading = false;
}

static void forget_descriptors(CatalogView *view);

static void
prune_catalog_views(XactEvent event, void *arg)
{
	dlist_mutable_iter miter;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT)
		return;

	/* Descriptors open while DDL retired its view are closed by now. */
	dlist_foreach_modify(miter, &views)
	{
		CatalogView *view = dlist_container(CatalogView, link, miter.cur);

		if (view->retired)
		{
			CatalogViewRetain(view);
			forget_descriptors(view);
			CatalogViewRelease(view);
		}
	}
	for (;;)
	{
		dlist_iter	it;
		CatalogView *oldest = NULL;
		int			count = 0;
		ListCell   *lc;
		List	   *kept = NIL;
		MemoryContext previous;

		dlist_foreach(it, &views)
		{
			CatalogView *view = dlist_container(CatalogView, link, it.cur);

			if (!view->loader || !view->loaded)
				continue;
			count++;
			if (view == current_view || view->scan_references || view->loading)
				continue;
			if (!oldest || view->touched < oldest->touched)
				oldest = view;
		}
		if (count <= catalog_view_cache_entries || !oldest)
			break;
		previous = MemoryContextSwitchTo(oldest->memory);
		foreach(lc, oldest->caches)
		{
			CatCache   *cache = lfirst(lc);

			/*
			 * Referenced cache tuples own their bytes. They remain valid but
			 * dead until their native resource owner releases them.
			 */
			if (!TryDestroyCatCache(cache))
				kept = lappend(kept, cache);
		}
		list_free(oldest->caches);
		oldest->caches = kept;
		oldest->catalogs = NIL;
		oldest->loaded = false;
		MemoryContextReset(oldest->rows_memory);
		forget_indexes(oldest);
		MemoryContextSwitchTo(previous);
	}
}

/* Only provisional roots need this guard. Committed canonical objects are
 * retained; a backend handle must not make an aborted object visible. */
void
CatalogViewSetCreator(CatalogView *view, TransactionId creator)
{
	view->creator = creator;
}

bool
CatalogViewCreatorAborted(CatalogView *view)
{
	return TransactionIdIsValid(view->creator) &&
		!TransactionIdIsCurrentTransactionId(view->creator) &&
		!TransactionIdDidCommit(view->creator);
}

/* Provisional views are private to native DDL. Tuple versions become visible
 * at CCI; scans retain their original list and never observe an in-place edit. */
typedef struct CatalogChange
{
	Oid			catalog;
	ItemPointerData tid;
	HeapTuple	tuple;
	CommandId	command;
	SubTransactionId subid;
	bool		aborted;
} CatalogChange;

static void
mutation_subxact(SubXactEvent event, SubTransactionId id, SubTransactionId parent, void *arg)
{
	dlist_iter	it;

	if (event != SUBXACT_EVENT_ABORT_SUB && event != SUBXACT_EVENT_COMMIT_SUB)
		return;
	dlist_foreach(it, &views)
	{
		CatalogView *view = dlist_container(CatalogView, link, it.cur);
		ListCell   *lc;

		if (!view->provisional || view->retired)
			continue;
		foreach(lc, view->changes)
		{
			CatalogChange *change = lfirst(lc);

			if (change->subid == id)
			{
				if (event == SUBXACT_EVENT_ABORT_SUB)
				{
					change->aborted = true;
					view->dirty = true;
				}
				else
					change->subid = parent;
			}
		}
		if (event == SUBXACT_EVENT_ABORT_SUB && view->dirty)
			refresh_mutation(view);
	}
}

void
CatalogViewBeginMutation(CatalogView *view)
{
	static bool registered;
	ListCell   *lc;

	if (!view->sealed || view->provisional || view->loader)
		elog(ERROR, "mutation requires a fresh private catalog view");
	view->provisional = true;
	view->baseline = view->catalogs;
	foreach(lc, view->catalogs)
		view->next_tuple = Max(view->next_tuple, list_length(((CatalogViewRows *) lfirst(lc))->tuples));
	if (!registered)
	{
		RegisterSubXactCallback(mutation_subxact, NULL);
		registered = true;
	}
}

/* A portable codec can reject an unsupported proposed value before native
 * DDL evaluates expressions or performs later passes based on that value. */
void
CatalogViewSetMutationValidator(CatalogView *view, void (*validator) (Relation, HeapTuple))
{
	if (!CatalogViewIsProvisional(view) || view->changes)
		elog(ERROR, "mutation validator must be installed before staging");
	view->mutation_validator = validator;
}

/* Validate against pending writes too, matching native unique-index behavior
 * before CCI. Catalogs have ordinary non-expression btree identity indexes. */
static List *
mutation_self_rows(CatalogView *view, Oid catalog)
{
	ListCell   *lc;
	List	   *effective = NIL;

	foreach(lc, view->baseline)
	{
		CatalogViewRows *rows = lfirst(lc);

		if (rows->catalog == catalog)
			effective = list_copy(rows->tuples);
	}
	foreach(lc, view->changes)
	{
		CatalogChange *change = lfirst(lc);
		List	   *next = NIL;
		ListCell   *tc;
		bool		found = false;

		if (change->aborted || change->catalog != catalog)
			continue;
		foreach(tc, effective)
		{
			HeapTuple	tuple = lfirst(tc);

			if (ItemPointerEquals(&tuple->t_self, &change->tid))
			{
				found = true;
				if (change->tuple)
					next = lappend(next, change->tuple);
			}
			else
				next = lappend(next, tuple);
		}
		if (!found && change->tuple)
			next = lappend(next, change->tuple);
		list_free(effective);
		effective = next;
	}
	return effective;
}


static void
check_mutation_tuple(CatalogView *view, Relation relation, ItemPointer target, HeapTuple proposed)
{
	ListCell   *lc,
			   *ic;
	List	   *effective = NIL;
	TupleDesc	desc = RelationGetDescr(relation);
	bool		found_target = target == NULL;

	effective = mutation_self_rows(view, RelationGetRelid(relation));
	foreach(lc, effective)
		if (target && ItemPointerEquals(target, &((HeapTuple) lfirst(lc))->t_self))
		found_target = true;
	if (!found_target)
		elog(ERROR, "provisional catalog mutation target does not exist");
	if (!proposed)
	{
		list_free(effective);
		return;
	}
	for (int i = 0; i < desc->natts; i++)
	{
		bool		isnull;

		(void) heap_getattr(proposed, i + 1, desc, &isnull);
		if (TupleDescAttr(desc, i)->attnotnull && isnull)
			ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("null value in column \"%s\" of catalog \"%s\"", NameStr(TupleDescAttr(desc, i)->attname), RelationGetRelationName(relation))));
	}
	foreach(ic, RelationGetIndexList(relation))
	{
		Relation	index = index_open(lfirst_oid(ic), AccessShareLock);

		if (index->rd_index->indisunique)
		{
			foreach(lc, effective)
			{
				HeapTuple	tuple = lfirst(lc);
				bool		equal = true;

				if (target && ItemPointerEquals(target, &tuple->t_self))
					continue;
				for (int k = 0; k < index->rd_index->indnkeyatts; k++)
				{
					int			att = index->rd_index->indkey.values[k];
					Datum		a,
								b;
					bool		anull,
								bnull;

					if (att <= 0)
						elog(ERROR, "unsupported provisional catalog index expression");
					a = heap_getattr(tuple, att, desc, &anull);
					b = heap_getattr(proposed, att, desc, &bnull);
					if (anull || bnull)
						equal = anull && bnull && index->rd_index->indnullsnotdistinct;
					else
						equal = DatumGetInt32(FunctionCall2Coll(index_getprocinfo(index, k + 1, BTORDER_PROC), index->rd_indcollation[k], a, b)) == 0;
					if (!equal)
						break;
				}
				if (equal)
					ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION), errmsg("duplicate key value violates unique constraint \"%s\"", RelationGetRelationName(index))));
			}
		}
		index_close(index, AccessShareLock);
	}
	list_free(effective);
}

static bool
stage_tuple(Relation relation, ItemPointer tid, HeapTuple tuple)
{
	CatalogView *view = CatalogViewCurrent();
	MemoryContext previous;
	CatalogChange *change;

	if (!view || !view->provisional || !CatalogViewContains(view, RelationGetRelid(relation)))
		return false;
	if (view->retired)
		elog(ERROR, "cannot mutate retired catalog context");
	if (tuple && view->mutation_validator)
		view->mutation_validator(relation, tuple);
	check_mutation_tuple(view, relation, tid, tuple);
	previous = MemoryContextSwitchTo(view->rows_memory);
	change = palloc0(sizeof(*change));
	change->catalog = RelationGetRelid(relation);
	if (tid)
		change->tid = *tid;
	else
	{
		uint64		ordinal = view->next_tuple++;

		ItemPointerSet(&change->tid, CatalogViewFirstBlock + ordinal / MaxOffsetNumber, ordinal % MaxOffsetNumber + 1);
		tuple->t_self = change->tid;
	}
	change->tuple = tuple ? heap_copytuple(tuple) : NULL;
	if (change->tuple)
		change->tuple->t_self = change->tid;
	change->command = GetCurrentCommandId(true);
	change->subid = GetCurrentSubTransactionId();
	view->changes = lappend(view->changes, change);
	view->dirty = true;
	MemoryContextSwitchTo(previous);
	return true;
}

bool
CatalogViewCaptureInsert(Relation relation, HeapTuple tuple)
{
	return stage_tuple(relation, NULL, tuple);
}

bool
CatalogViewCaptureDelete(Relation relation, ItemPointer tid)
{
	return stage_tuple(relation, tid, NULL);
}

static void
refresh_mutation(CatalogView *view)
{
	MemoryContext previous = MemoryContextSwitchTo(view->rows_memory);
	List	   *catalogs = NIL;
	ListCell   *lc;

	foreach(lc, view->baseline)
	{
		CatalogViewRows *base = lfirst(lc);
		CatalogViewRows *rows = palloc(sizeof(*rows));
		ListCell   *change_cell;

		*rows = *base;
		rows->tuples = list_copy(base->tuples);
		foreach(change_cell, view->changes)
		{
			CatalogChange *change = lfirst(change_cell);
			List	   *updated = NIL;
			ListCell   *tuple_cell;
			bool		found = false;

			if (change->aborted || change->catalog != rows->catalog || change->command >= GetCurrentCommandId(false))
				continue;
			foreach(tuple_cell, rows->tuples)
			{
				HeapTuple	tuple = lfirst(tuple_cell);

				if (ItemPointerEquals(&tuple->t_self, &change->tid))
				{
					found = true;
					if (change->tuple)
						updated = lappend(updated, change->tuple);
				}
				else
					updated = lappend(updated, tuple);
			}
			if (!found && change->tuple)
				updated = lappend(updated, change->tuple);
			rows->tuples = updated;
		}
		catalogs = lappend(catalogs, rows);
	}
	view->catalogs = catalogs;
	view->dirty = false;
	forget_indexes(view);
	MemoryContextSwitchTo(previous);
	foreach(lc, view->caches)
		ResetCatalogCache(lfirst(lc), false);
	foreach(lc, view->handles)
	{
		Oid			handle = lfirst_oid(lc);
		CatalogViewObject *object = lookup_object(handle);

		if (object->objectclass == RelationRelationId)
			InvalidateLocalRelation(handle);
	}
}

void
CatalogViewMutationCommandEnd(void)
{
	CatalogView *view = CatalogViewCurrent();

	if (view && view->provisional && view->dirty && !view->retired)
		refresh_mutation(view);
}

List *
CatalogViewTuples(CatalogView *view, Oid catalog)
{
	ListCell   *lc;

	CatalogViewEnsureLoaded(view);
	foreach(lc, view->catalogs)
	{
		CatalogViewRows *rows = lfirst(lc);

		if (rows->catalog == catalog)
			return rows->tuples;
	}
	elog(ERROR, "catalog is not provided by context");
	return NIL;
}

/*
 * Call |callback| for every tuple a provisional view's mutation staged, and
 * for every published tuple one of them replaced or deleted.
 */
/*
 * The rows of |catalog| whose OID column |attribute| holds |value|, in view
 * order, through an index built on first use. The list is the caller's: the
 * index is forgotten when the rows change, as they do between commands of a
 * provisional view.
 */
List *
CatalogViewTuplesWith(CatalogView *view, Oid catalog, AttrNumber attribute, Oid value)
{
	CatalogViewIndex *index = NULL;
	CatalogViewIndexEntry *entry;

	CatalogViewEnsureLoaded(view);
	foreach_ptr(CatalogViewIndex, candidate, view->indexes)
	{
		if (candidate->catalog == catalog && candidate->attribute == attribute)
			index = candidate;
	}
	if (!index)
	{
		MemoryContext previous = MemoryContextSwitchTo(view->index_memory);
		Relation	rel = RelationIdGetRelation(catalog);
		TupleDesc	desc = RelationGetDescr(rel);
		HASHCTL		ctl = {0};

		if (!CatalogViewIsObjectIdType(TupleDescAttr(desc, attribute - 1)->atttypid))
			elog(ERROR, "catalog view index requires an OID column");
		index = palloc(sizeof(*index));
		index->catalog = catalog;
		index->attribute = attribute;
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(CatalogViewIndexEntry);
		ctl.hcxt = view->index_memory;
		index->entries = hash_create("catalog view index", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		foreach_ptr(HeapTupleData, tuple, CatalogViewTuples(view, catalog))
		{
			bool		isnull;
			Datum		key = heap_getattr(tuple, attribute, desc, &isnull);
			Oid			oid = isnull ? InvalidOid : DatumGetObjectId(key);
			bool		found;

			entry = hash_search(index->entries, &oid, HASH_ENTER, &found);
			if (!found)
				entry->tuples = NIL;
			entry->tuples = lappend(entry->tuples, tuple);
		}
		RelationClose(rel);
		view->indexes = lappend(view->indexes, index);
		MemoryContextSwitchTo(previous);
	}
	entry = hash_search(index->entries, &value, HASH_FIND, NULL);
	return entry ? list_copy(entry->tuples) : NIL;
}

void
CatalogViewForEachChange(CatalogView *view, void (*callback) (Oid catalog, HeapTuple tuple, void *arg), void *arg)
{
	ListCell   *lc;

	Assert(CatalogViewIsProvisional(view));
	foreach(lc, view->changes)
	{
		CatalogChange *change = lfirst(lc);

		if (change->aborted)
			continue;
		if (change->tuple)
			callback(change->catalog, change->tuple, arg);
		foreach_ptr(CatalogViewRows, rows, view->baseline)
		{
			if (rows->catalog != change->catalog)
				continue;
			foreach_ptr(HeapTupleData, tuple, rows->tuples)
			{
				if (ItemPointerEquals(&tuple->t_self, &change->tid))
					callback(change->catalog, tuple, arg);
			}
		}
	}
}

/* Drop the unused relation and type descriptors of |view|'s objects. */
static void
forget_descriptors(CatalogView *view)
{
	foreach_oid(handle, view->handles)
	{
		CatalogViewObject *object = hash_search(objects, &handle, HASH_FIND, NULL);

		if (object && object->objectclass == RelationRelationId)
			InvalidateLocalRelation(handle);
	}
	TypeCacheForgetCatalogView(view);
}

void
CatalogViewRetireMutation(CatalogView *view)
{
	ListCell   *lc;

	if (!view->provisional || view == CatalogViewCurrent())
		elog(ERROR, "invalid provisional catalog retirement");
	/*
	 * Unused descriptors of its relations and types go now, so that nothing
	 * keeps the view; one still open is rebuilt while the view can serve it,
	 * and goes at transaction end.
	 */
	forget_descriptors(view);

	/* Existing local handle references fail explicitly after the statement. */
	view->retired = true;
	foreach(lc, view->caches)
		ResetCatalogCache(lfirst(lc), false);
	if (!view->scan_references)
	{
		view->baseline = view->catalogs = view->changes = NIL;
		MemoryContextReset(view->rows_memory);
		forget_indexes(view);
	}
}

bool
CatalogViewIsProvisional(CatalogView *view)
{
	return view && view->provisional && !view->retired;
}

/* Give a provisional object its name, once native DDL has created it. */
void
CatalogViewNameObject(Oid handle, const char *identity)
{
	CatalogViewObject *object = lookup_object(handle);
	MemoryContext previous;

	if (!object || !CatalogViewIsProvisional(object->view))
		elog(ERROR, "only provisional catalog objects can be named");
	previous = MemoryContextSwitchTo(object->view->memory);
	object->identity = pstrdup(identity);
	MemoryContextSwitchTo(previous);
}

Oid
CatalogViewNewObject(Oid catalog)
{
	CatalogView *view = CatalogViewCurrent();

	if (!CatalogViewIsProvisional(view) || !CatalogViewContains(view, catalog))
		elog(ERROR, "invalid provisional object allocation");
	return CatalogViewBindObject(view, catalog, psprintf("$provisional/%u", next_handle), InvalidOid);
}

/* Storage effects need an explicit publisher; immutable contexts never own files. */
void
CatalogViewSetStorageHandler(CatalogView *view, void (*handler) (Relation, CatalogViewStorageEffect))
{
	Assert(CatalogViewIsProvisional(view));
	view->storage_handler = handler;
}

bool
CatalogViewCaptureStorage(Relation relation, CatalogViewStorageEffect effect)
{
	CatalogView *view = CatalogViewObjectContext(RelationGetRelid(relation));

	if (!view)
		return false;
	if (view != CatalogViewCurrent() || !CatalogViewIsProvisional(view) || !view->storage_handler)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("storage effect requires canonical publication")));
	view->storage_handler(relation, effect);
	return true;
}
