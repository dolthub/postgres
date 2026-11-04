/*
 * A canonical definition and a storage resource are different objects.
 * Resource records contain locators and access-method properties, never columns,
 * names, types or constraints. The transaction snapshot selects the binding.
 *
 * Native capture registers existing permanent relation files. Canonical CREATE
 * allocates resources without a physical schema row.
 */
#include "dg.h"
#include "access/doltcatalog.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/xact.h"
#include "catalog/catalogview.h"
#include "catalog/catalog.h"
#include "catalog/storage.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_authid_d.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "catalog/indexing.h"
#include "catalog/pg_dolt_binding.h"
#include "catalog/pg_dolt_object.h"
#include "catalog/pg_dolt_storage.h"
#include "catalog/pg_dolt_ref.h"
#include "storage/bufmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/snapmgr.h"

/* This descriptor is private to its consumer, not an entry in native relcache. */
struct DGStorage
{
	RelationData relation;
	FormData_pg_class form;
	Relation	definition;
	char	   *branch;
	Snapshot	snapshot;
	List	   *indexes;
	bytea	   *base;
	bool		writable;
	MemoryContextCallback cleanup;
};

static bool
same_schema(bytea *a, bytea *b)
{
	return VARSIZE_ANY_EXHDR(a) == VARSIZE_ANY_EXHDR(b) &&
		!memcmp(VARDATA_ANY(a), VARDATA_ANY(b), VARSIZE_ANY_EXHDR(a));
}

/* Called while capture holds the source relation lock. */
void
dg_storage_record(Relation source)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	old,
				tuple;
	Datum		values[Natts_pg_dolt_storage];
	bool		nulls[Natts_pg_dolt_storage] = {false};
	bool		changed = true;
	Snapshot	snapshot;
	Oid			id = RelationGetRelid(source);

	if (CatalogViewObjectContext(id) || source->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT ||
		source->rd_locator.dbOid != MyDatabaseId || !RelFileNumberIsValid(source->rd_locator.relNumber))
		elog(ERROR, "invalid physical source for prolly storage registration");
	catalog = table_open(DoltStorageRelationId, RowExclusiveLock);
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	ScanKeyInit(&key, Anum_pg_dolt_storage_storageid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(id));
	scan = systable_beginscan(catalog, DoltStorageKeyIndexId, true, snapshot, 1, &key);
	old = systable_getnext(scan);
	if (HeapTupleIsValid(old))
	{
		Form_pg_dolt_storage form = (Form_pg_dolt_storage) GETSTRUCT(old);

		if (!form->storageimported)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("storage resource identity belongs to an independent allocation")));

		changed = form->storagespc != source->rd_locator.spcOid || form->storagedb != source->rd_locator.dbOid ||
			form->storagefile != source->rd_locator.relNumber || form->storageam != source->rd_rel->relam ||
			form->storagekind != source->rd_rel->relkind;
	}
	if (changed)
	{
		values[Anum_pg_dolt_storage_storageid - 1] = ObjectIdGetDatum(id);
		values[Anum_pg_dolt_storage_storagespc - 1] = ObjectIdGetDatum(source->rd_locator.spcOid);
		values[Anum_pg_dolt_storage_storagedb - 1] = ObjectIdGetDatum(source->rd_locator.dbOid);
		values[Anum_pg_dolt_storage_storagefile - 1] = ObjectIdGetDatum(source->rd_locator.relNumber);
		values[Anum_pg_dolt_storage_storageam - 1] = ObjectIdGetDatum(source->rd_rel->relam);
		values[Anum_pg_dolt_storage_storagekind - 1] = CharGetDatum(source->rd_rel->relkind);
		values[Anum_pg_dolt_storage_storageimported - 1] = BoolGetDatum(true);
		tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
		if (HeapTupleIsValid(old))
			CatalogTupleUpdate(catalog, &old->t_self, tuple);
		else
			CatalogTupleInsert(catalog, tuple);
		heap_freetuple(tuple);
	}
	systable_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(catalog, NoLock);
	if (changed)
		CommandCounterIncrement();
}

/* CREATE's storage primitive: no pg_class, pg_attribute or pg_type writes.
 * The caller must publish canonical membership and a branch binding in the same
 * transaction. Native pending-delete machinery owns file cleanup on abort. */
Oid
dg_storage_allocate(Relation definition, const char *branch)
{
	if (!definition->rd_catalogview ||
		!((DG_ROWS_KIND(definition->rd_rel->relkind) && definition->rd_rel->relam == DOLT_PROLLY_TABLE_AM_OID) ||
		  (definition->rd_rel->relkind == RELKIND_INDEX && definition->rd_rel->relam == DOLT_PROLLY_INDEX_AM_OID)))
		elog(ERROR, "storage allocation requires a canonical prolly definition");
	return dg_storage_allocate_kind(definition->rd_rel->relkind, branch);
}

/*
 * Allocate an empty storage resource on |branch| for a prolly table,
 * materialized view or index of |relkind|. Storage needs only the kind of
 * its rows, not the definition, which need not be built yet.
 */
Oid
dg_storage_allocate_kind(char relkind, const char *branch)
{
	Relation	catalog;
	Relation	resource = palloc0(sizeof(RelationData));
	FormData_pg_class form = {0};
	Datum		values[Natts_pg_dolt_storage];
	bool		nulls[Natts_pg_dolt_storage] = {false};
	HeapTuple	tuple;
	Oid			id;

	if (XactReadOnly)
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("cannot allocate storage in a read-only transaction")));
	if (!DG_ROWS_KIND(relkind) && relkind != RELKIND_INDEX && relkind != RELKIND_SEQUENCE)
		elog(ERROR, "storage allocation requires a prolly table or index");
	form.relkind = relkind;
	form.relam = relkind == RELKIND_SEQUENCE ? InvalidOid : relkind == RELKIND_INDEX ? DOLT_PROLLY_INDEX_AM_OID : DOLT_PROLLY_TABLE_AM_OID;
	form.relpersistence = RELPERSISTENCE_PERMANENT;
	catalog = table_open(DoltStorageRelationId, RowExclusiveLock);
	do
	{
		id = GetNewOidWithIndex(catalog, DoltStorageKeyIndexId, Anum_pg_dolt_storage_storageid);
	}
	while (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(id)));
	LockRelationOid(id, AccessExclusiveLock);
	resource->rd_rel = &form;
	resource->rd_smgr = NULL;
	resource->rd_locator.spcOid = MyDatabaseTableSpace;
	resource->rd_locator.dbOid = MyDatabaseId;
	resource->rd_locator.relNumber = GetNewRelFileNumber(MyDatabaseTableSpace, NULL, RELPERSISTENCE_PERMANENT);
	resource->rd_lockInfo.lockRelId.relId = id;
	resource->rd_lockInfo.lockRelId.dbId = MyDatabaseId;
	values[Anum_pg_dolt_storage_storageid - 1] = ObjectIdGetDatum(id);
	values[Anum_pg_dolt_storage_storagespc - 1] = ObjectIdGetDatum(resource->rd_locator.spcOid);
	values[Anum_pg_dolt_storage_storagedb - 1] = ObjectIdGetDatum(resource->rd_locator.dbOid);
	values[Anum_pg_dolt_storage_storagefile - 1] = ObjectIdGetDatum(resource->rd_locator.relNumber);
	values[Anum_pg_dolt_storage_storageam - 1] = ObjectIdGetDatum(form.relam);
	values[Anum_pg_dolt_storage_storagekind - 1] = CharGetDatum(form.relkind);
	values[Anum_pg_dolt_storage_storageimported - 1] = BoolGetDatum(false);
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	CatalogTupleInsert(catalog, tuple);
	heap_freetuple(tuple);
	table_close(catalog, NoLock);
	(void) RelationCreateStorage(resource->rd_locator, RELPERSISTENCE_PERMANENT, true);
	PG_TRY();
	{
		if (relkind != RELKIND_SEQUENCE)
			dg_initialize_resource(resource, branch);
	}
	PG_FINALLY();
	{
		RelationCloseSmgr(resource);
	}
	PG_END_TRY();
	CommandCounterIncrement();
	return id;
}

/*
 * Pending statistics must not name a private descriptor once it is freed. A
 * descriptor copied from its definition shares the definition's entry, which
 * stays linked to the definition.
 */
static void
storage_unlink_statistics(Relation rel)
{
	if (rel->pgstat_info && rel->pgstat_info->relation == rel)
		pgstat_unlink_relation(rel);
	rel->pgstat_info = NULL;
}

static void
storage_cleanup(void *arg)
{
	DGStorage  *storage = arg;

	/*
	 * The resource owner releases definition pins on ERROR. Only this private
	 * descriptor owns the smgr pin, so memory-context cleanup handles it.
	 */
	RelationCloseSmgr(&storage->relation);
	storage_unlink_statistics(&storage->relation);
}

DGStorage *
dg_storage_open(Oid definition, const char *branch, const char *name,
				bytea *schema, Oid resource, Snapshot snapshot)
{
	DGStorage  *storage = palloc0(sizeof(*storage));
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData keys[2];
	HeapTuple	tuple;
	bool		isnull;
	bytea	   *stored;
	FormData_pg_dolt_storage form;
	LockRelId	lock = {resource, MyDatabaseId};

	if (!CatalogViewObjectContext(definition) || !OidIsValid(resource) ||
		resource >= FirstCatalogViewObjectId || !snapshot || !IsMVCCSnapshot(snapshot))
		elog(ERROR, "invalid canonical storage binding");

	/*
	 * Use the stable resource identity BEFORE reading its locator. Native
	 * physical DDL and ordinary AM writers currently use this same lock
	 * identity.
	 */
	LockRelationId(&lock, AccessShareLock);
	catalog = table_open(DoltBindingRelationId, AccessShareLock);
	ScanKeyInit(&keys[0], Anum_pg_dolt_binding_bindbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	ScanKeyInit(&keys[1], Anum_pg_dolt_binding_bindname, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(name));
	scan = systable_beginscan(catalog, DoltBindingKeyIndexId, true, snapshot, 2, keys);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime storage binding unavailable for %s on branch %s", name, branch)));
	stored = DatumGetByteaPP(heap_getattr(tuple, Anum_pg_dolt_binding_bindschema, RelationGetDescr(catalog), &isnull));
	if (((Form_pg_dolt_binding) GETSTRUCT(tuple))->bindrelid != resource || !same_schema(stored, schema))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime storage binding changed for %s on branch %s", name, branch)));
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	catalog = table_open(DoltStorageRelationId, AccessShareLock);
	ScanKeyInit(&keys[0], Anum_pg_dolt_storage_storageid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(resource));
	scan = systable_beginscan(catalog, DoltStorageKeyIndexId, true, snapshot, 1, keys);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("storage resource %u is unavailable", resource)));
	form = *(Form_pg_dolt_storage) GETSTRUCT(tuple);
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	storage->definition = relation_open(definition, CatalogViewIsDurable(definition) ? AccessShareLock : NoLock);
	if (form.storagedb != MyDatabaseId || !OidIsValid(form.storagespc) || !RelFileNumberIsValid(form.storagefile) ||
		form.storageam != storage->definition->rd_rel->relam || form.storagekind != storage->definition->rd_rel->relkind)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("storage resource properties do not match canonical definition")));

	/*
	 * Imported resources retain a physical allocation bridge. Only validate
	 * its locator and AM: canonical ALTER can legitimately change the tuple
	 * layout on one branch without changing that old physical schema.
	 * Independently allocated resources have no physical schema row.
	 */
	if (form.storageimported)
	{
		CatalogView *previous = CatalogViewSwitch(NULL);

		PG_TRY();
		{
			Relation	source = try_relation_open(resource, NoLock);
			RelFileLocator locator = {form.storagespc, form.storagedb, form.storagefile};
			bool		compatible;

			if (!source)
				ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding unavailable for %s on branch %s", name, branch)));
			compatible = RelFileLocatorEquals(source->rd_locator, locator) && source->rd_rel->relam == form.storageam;
			relation_close(source, NoLock);
			if (!compatible)
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("imported resource allocation changed")));
		}
		PG_FINALLY();
		{
			CatalogViewSwitch(previous);
		}
		PG_END_TRY();
	}

	/*
	 * Cache canonical index identities on their owning definition, not on a
	 * disposable shallow copy of that descriptor.
	 */
	if (storage->definition->rd_rel->relhasindex)
		list_free(RelationGetIndexList(storage->definition));
	storage->relation = *storage->definition;
	storage->relation.rd_refparent = storage->definition;
	storage->form = *storage->definition->rd_rel;
	storage->relation.rd_rel = &storage->form;
	storage->relation.rd_smgr = NULL;
	storage->relation.rd_amcache = storage;
	storage->relation.rd_locator.spcOid = form.storagespc;
	storage->relation.rd_locator.dbOid = form.storagedb;
	storage->relation.rd_locator.relNumber = form.storagefile;
	storage->relation.rd_lockInfo.lockRelId = lock;
	storage->branch = pstrdup(branch);
	storage->snapshot = RegisterSnapshot(snapshot);
	storage->cleanup.func = storage_cleanup;
	storage->cleanup.arg = storage;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &storage->cleanup);
	if (!smgrexists(RelationGetSmgr(&storage->relation), MAIN_FORKNUM))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("storage resource file is unavailable")));
	return storage;
}

/*
 * The content root of storage |resource| on |branch|, read without building its
 * definition: false if it holds none there.
 */
bool
dg_storage_resource_root(Oid resource, const char *branch, Snapshot snapshot, PLHash *content, bool *load)
{
	LockRelId	lock = {resource, MyDatabaseId};
	Relation	catalog;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	FormData_pg_dolt_storage form;
	RelFileLocator locator;

	LockRelationId(&lock, AccessShareLock);
	catalog = table_open(DoltStorageRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_dolt_storage_storageid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(resource));
	scan = systable_beginscan(catalog, DoltStorageKeyIndexId, true, snapshot, 1, &key);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("storage resource %u is unavailable", resource)));
	form = *(Form_pg_dolt_storage) GETSTRUCT(tuple);
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	if (form.storagedb != MyDatabaseId || !OidIsValid(form.storagespc) || !RelFileNumberIsValid(form.storagefile))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("storage resource %u is invalid", resource)));
	locator.spcOid = form.storagespc;
	locator.dbOid = form.storagedb;
	locator.relNumber = form.storagefile;

	/* The roots and this transaction's changes are the whole content. */
	*load = false;
	return dg_storage_content_root(locator, branch, snapshot, content);
}

/* Only this checked entrypoint admits a canonical descriptor to page storage.
 * There is no ambient authority to write a retained/schema-only handle. */
DGState *
dg_storage_load(DGStorage *storage)
{
	return dg_load_bound(&storage->relation, storage->snapshot, storage->branch);
}

void
dg_storage_close(DGStorage *storage)
{
	ListCell   *lc;

	/* Pending edits name this private descriptor: apply them while it lives. */
	if (storage->writable)
		dg_batch_finish(&storage->relation);
	foreach(lc, storage->indexes)
		dg_storage_close_relation((Relation) lfirst(lc));
	RelationCloseSmgr(&storage->relation);
	storage_unlink_statistics(&storage->relation);
	UnregisterSnapshot(storage->snapshot);
	relation_close(storage->definition, NoLock);
	storage->definition = NULL;
	/* Keep the stable resource lock to transaction end, as native scans do. */
}

/* Lock the exact snapshot-visible tuple. Following an updated tuple would give
 * an old definition authority over a new working set. Retry instead. */
static void
lock_catalog_tuple(Relation catalog, HeapTuple tuple)
{
	Buffer		buffer = InvalidBuffer;
	TM_FailureData failure;
	TM_Result	result;

	result = heap_lock_tuple(catalog, tuple, GetCurrentCommandId(false),
							 LockTupleShare, LockWaitBlock, false, &buffer, &failure);
	if (BufferIsValid(buffer))
		ReleaseBuffer(buffer);
	if (result != TM_Ok)
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						errmsg("canonical write binding changed; retry transaction")));
}

/* Pin working membership against concurrent DROP/ref publication. This lock is
 * branch-local and compatible with another data writer's membership pin. */
void
dg_storage_lock_working(const char *branch, Snapshot snapshot)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;

	if (XactReadOnly)
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("cannot write canonical storage in a read-only transaction")));
	catalog = table_open(DoltRefRelationId, RowShareLock);
	ScanKeyInit(&key, Anum_pg_dolt_ref_refname, BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(psprintf("working/%s", branch)));
	scan = systable_beginscan(catalog, DoltRefKeyIndexId, true, snapshot, 1, &key);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("branch working set is unavailable: %s", branch)));
	lock_catalog_tuple(catalog, tuple);
	systable_endscan(scan);
	table_close(catalog, NoLock);
}

/*
 * Lock a resource on one branch. Writers take RowExclusiveLock, which they
 * share with each other because rows are locked individually; operations that
 * replace the whole branch state, such as merge, take ExclusiveLock.
 */
void
dg_storage_lock_branch(Oid resource, const char *branch, LOCKMODE mode)
{
	PLHash		hash;
	PLSlice		bytes = {(const uint8 *) branch, strlen(branch)};

	if (!pl_hash_bytes(bytes, &hash))
		elog(ERROR, "branch hash failed");
	/* Hash collisions conservatively serialize unrelated branches. */
	LockDatabaseObject(DoltBindingRelationId, resource, pl_read32(hash.bytes), mode);
}

Relation
dg_storage_enable_write(DGStorage *storage, bytea *base)
{
	Relation	rel = &storage->relation;
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData keys[2];
	HeapTuple	tuple;
	char	   *name;

	if (XactReadOnly)
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("cannot write canonical storage in a read-only transaction")));
	if (!DG_ROWS_KIND(rel->rd_rel->relkind) && rel->rd_rel->relkind != RELKIND_INDEX)
		dg_unsupported("canonical resource write kind");

	/*
	 * Acquire the shared resource lock, never the backend-local definition
	 * OID.
	 */
	LockRelationId(&rel->rd_lockInfo.lockRelId, RowExclusiveLock);
	if (CatalogViewIsDurable(RelationGetRelid(storage->definition)))
		LockRelationOid(RelationGetRelid(storage->definition), RowExclusiveLock);
	dg_storage_lock_branch(rel->rd_lockInfo.lockRelId.relId, storage->branch, RowExclusiveLock);
	name = dg_qualified_name(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel));
	catalog = table_open(DoltBindingRelationId, RowShareLock);
	ScanKeyInit(&keys[0], Anum_pg_dolt_binding_bindbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(storage->branch));
	ScanKeyInit(&keys[1], Anum_pg_dolt_binding_bindname, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(name));
	scan = systable_beginscan(catalog, DoltBindingKeyIndexId, true, storage->snapshot, 2, keys);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "canonical write binding disappeared");
	lock_catalog_tuple(catalog, tuple);
	systable_endscan(scan);
	table_close(catalog, NoLock);
	storage->base = DatumGetByteaPCopy(PointerGetDatum(base));
	storage->writable = true;
	return rel;
}

/* Whether |rel| is a private descriptor over a storage resource. */
bool
dg_storage_bound(Relation rel)
{
	DGStorage  *storage = rel->rd_amcache;

	return rel->rd_catalogview && storage && &storage->relation == rel && storage->definition;
}

static DGStorage *
bound_storage(Relation rel)
{
	DGStorage  *storage = rel->rd_amcache;

	if (!rel->rd_catalogview || !storage || &storage->relation != rel || !storage->definition)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("canonical catalog handles require an explicit storage binding")));
	return storage;
}

const char *
dg_storage_branch(Relation rel)
{
	return bound_storage(rel)->branch;
}

void
dg_storage_require_write(Relation rel)
{
	DGStorage  *storage = bound_storage(rel);

	if (!storage->writable || XactReadOnly)
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("canonical storage binding has no write authority")));
}

static void
storage_base(DGStorage *storage, DGState *state)
{
	if (!state->has_root && storage->base)
	{
		Oid			user;
		int			security;

		GetUserIdAndSecContext(&user, &security);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		PushActiveSnapshot(storage->snapshot);
		PG_TRY();
		{
			dg_set_base(state, dg_history_import(state->store, storage->base));
		}
		PG_FINALLY();
		{
			PopActiveSnapshot();
			SetUserIdAndSecContext(user, security);
		}
		PG_END_TRY();
	}
}

DGState *
dg_storage_am_load(Relation rel, Snapshot snapshot)
{
	DGStorage  *storage = bound_storage(rel);
	DGState    *state;

	if (snapshot == SnapshotSelf)
		dg_storage_require_write(rel);
	state = dg_load_bound(rel, snapshot, storage->branch);
	storage_base(storage, state);
	return state;
}

void
dg_storage_close_relation(Relation rel)
{
	dg_storage_close(bound_storage(rel));
}

void
dg_storage_set_indexes(Relation rel, List *indexes)
{
	DGStorage  *storage = bound_storage(rel);

	Assert(storage->indexes == NIL);
	storage->indexes = indexes;
}

List *
dg_storage_indexes(Relation rel)
{
	DGStorage  *storage = bound_storage(rel);

	dg_storage_require_write(rel);
	if (rel->rd_rel->relhasindex && storage->indexes == NIL)
		elog(ERROR, "canonical indexed write has no bound indexes");
	return storage->indexes;
}

Relation
dg_storage_index(Relation rel, Oid id)
{
	ListCell   *lc;

	foreach(lc, dg_storage_indexes(rel))
	{
		Relation	index = lfirst(lc);

		if (RelationGetRelid(index) == id)
			return index;
	}
	elog(ERROR, "canonical index is not bound to target resource");
	return NULL;
}

DGState *
dg_storage_am_load_tid(Relation rel, uint64 id)
{
	DGStorage  *storage = bound_storage(rel);
	DGState    *state;

	dg_storage_require_write(rel);
	state = dg_load_bound_tid(rel, id, storage->branch);
	storage_base(storage, state);
	return state;
}

Relation
dg_storage_enable_read(DGStorage *storage, bytea *base)
{
	storage->base = DatumGetByteaPCopy(PointerGetDatum(base));
	return &storage->relation;
}

/*
 * The locator of the table a bound |index| orders: the table's binding on
 * the index's branch, read under the index's snapshot. The index definition
 * names the table's canonical handle, which has no file of its own.
 */
void
dg_storage_table_locator(Relation index, RelFileLocator *locator)
{
	DGStorage  *storage = bound_storage(index);
	Relation	table = relation_open(storage->definition->rd_index->indrelid, NoLock);
	char	   *name = dg_qualified_name(get_namespace_name(table->rd_rel->relnamespace), RelationGetRelationName(table));
	Relation	catalog = table_open(DoltBindingRelationId, AccessShareLock);
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			resource;

	relation_close(table, NoLock);
	ScanKeyInit(&keys[0], Anum_pg_dolt_binding_bindbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(storage->branch));
	ScanKeyInit(&keys[1], Anum_pg_dolt_binding_bindname, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(name));
	scan = systable_beginscan(catalog, DoltBindingKeyIndexId, true, storage->snapshot, 2, keys);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime storage binding unavailable for %s on branch %s", name, storage->branch)));
	resource = ((Form_pg_dolt_binding) GETSTRUCT(tuple))->bindrelid;
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	dg_storage_locator(resource, locator);
}

/*
 * The file locator of storage resource |resource|, for the relcache entry of
 * a branch head's relation. Read under the catalog snapshot, as relcache
 * builds read pg_class.
 */
void
dg_storage_locator(Oid resource, RelFileLocator *locator)
{
	Relation	catalog = table_open(DoltStorageRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	Form_pg_dolt_storage form;

	if (!OidIsValid(resource))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("runtime catalog binding unavailable")));
	ScanKeyInit(&key, Anum_pg_dolt_storage_storageid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(resource));
	scan = systable_beginscan(catalog, DoltStorageKeyIndexId, true, NULL, 1, &key);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("storage resource %u is unavailable", resource)));
	form = (Form_pg_dolt_storage) GETSTRUCT(tuple);
	locator->spcOid = form->storagespc;
	locator->dbOid = form->storagedb;
	locator->relNumber = form->storagefile;
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
}
