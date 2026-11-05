/* Heap/MVCC manifest building blocks. See doc/doltgres/manifest.md. */
#include "dg.h"
#include "access/doltchunk.h"
#include "catalog/pg_dolt_object.h"
#include "catalog/pg_dolt_ref.h"
#include "catalog/heap.h"
#include "catalog/pg_dolt_identity.h"
#include "utils/inval.h"
#include "access/relation.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgrprotos.h"
#include "utils/snapmgr.h"
#include "utils/json.h"
#include "mb/pg_wchar.h"
#include "common/hashfn.h"
#include "access/genam.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "utils/jsonb.h"

#define DG_SCHEMA_TREE "DGSCHEMA2"

/*
 * Object kinds, by the id their hashes encode. Kind 0, "chunk", is retired:
 * tree nodes live in the chunk store, under their node hashes, and objects
 * name a tree by its root's (see tree_edge).
 */
static const char *const kinds[] = {"chunk", "schema", "table", "index", "database", "commit", "namespace", "tag", "merge", "routine", "rebase", "stash", "trigger", "type", "view", "sequence", "aggregate", "operator", "cast"};

static void
manifest_begin(void)
{
	MemoryContext caller = CurrentMemoryContext;

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("manifest foundation functions require superuser")));
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "cannot connect manifest SPI");
	MemoryContextSwitchTo(caller);
}

/*
 * Internal SQL is a fixed set of statements, each run many times: prepare
 * each once and keep its plan, as a PL function keeps its statements' plans.
 * The plan cache replans them when the objects they use change.
 */
typedef struct ManifestPlan
{
	uint64		key;			/* hash of the statement text */
	char	   *sql;
	SPIPlanPtr	plan;
} ManifestPlan;
static HTAB *manifest_plans;

SPIPlanPtr
dg_manifest_plan(const char *sql, int nargs, Oid *types)
{
	uint64		key = hash_bytes_extended((const unsigned char *) sql, strlen(sql), nargs);
	ManifestPlan *entry;
	SPIPlanPtr	plan;
	bool		found;

	if (!manifest_plans)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(uint64);
		ctl.entrysize = sizeof(ManifestPlan);
		ctl.hcxt = TopMemoryContext;
		manifest_plans = hash_create("manifest SQL plans", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	entry = hash_search(manifest_plans, &key, HASH_FIND, NULL);
	if (entry)
		return strcmp(entry->sql, sql) == 0 ? entry->plan : NULL;
	/* Table-less statements never prefer a generic plan on cost: force one. */
	plan = SPI_prepare_cursor(sql, nargs, types, CURSOR_OPT_GENERIC_PLAN);
	if (!plan)
		elog(ERROR, "SPI_prepare failed: %s", SPI_result_code_string(SPI_result));
	if (SPI_keepplan(plan))
		elog(ERROR, "SPI_keepplan failed");
	entry = hash_search(manifest_plans, &key, HASH_ENTER, &found);
	entry->sql = MemoryContextStrdup(TopMemoryContext, sql);
	entry->plan = plan;
	return plan;
}

/* SPI restores its procedure context, not an arbitrary caller context. */
static void
manifest_execute(const char *sql, int nargs, Oid *types, Datum *args,
				 const char *nulls, bool readonly, long count)
{
	MemoryContext caller = CurrentMemoryContext;
	SPIPlanPtr	plan = dg_manifest_plan(sql, nargs, types);

	if (plan)
		SPI_execute_plan(plan, args, nulls, readonly, count);
	else
		SPI_execute_with_args(sql, nargs, types, args, nulls, readonly, count);
	MemoryContextSwitchTo(caller);
}

static bytea *
bytes(const void *p, Size n)
{
	bytea	   *v = palloc(VARHDRSZ + n);

	SET_VARSIZE(v, VARHDRSZ + n);
	if (n)
		memcpy(VARDATA(v), p, n);
	return v;
}

static void
check_hash(bytea *hash)
{
	if (VARSIZE_ANY_EXHDR(hash) != PL_HASH_SIZE)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("manifest hashes and revision tokens must be %d bytes", PL_HASH_SIZE)));
}

static int
kind_id(const char *kind)
{
	int			i;

	for (i = 0; i < lengthof(kinds); i++)
		if (strcmp(kind, kinds[i]) == 0)
			return i;
	ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("unknown manifest object kind: %s", kind)));
	return -1;
}

/*
 * Read as an internal read-write statement does: under a new statement
 * snapshot that sees this transaction's earlier commands.
 */
static void
statement_snapshot_push(void)
{
	PushActiveSnapshot(GetTransactionSnapshot());
	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
}

/*
 * What child |index| of an object of kind id |id| names: another object; the
 * root of a tree in the chunk store, by its node hash (a schema tree
 * envelope's only child); or a database root (a commit's state, a merge's
 * working and staged states, a stash's states).
 */
typedef enum EdgeKind
{
	EDGE_OBJECT,
	EDGE_TREE,
	EDGE_ROOT,
} EdgeKind;

static EdgeKind
edge_kind(int id, int index)
{
	if ((id == 1 || id == 6) && index == 0)
		return EDGE_TREE;
	if ((id == 5 && index == 0) || (id == 8 && (index == 3 || index == 4)) || (id == 11 && index < 2))
		return EDGE_ROOT;
	return EDGE_OBJECT;
}

/* Kinds a database root's members once were, before roots held them inline. */
static bool
retired_kind(int id)
{
	return id == 0 || id == 2 || id == 3 || id == 4 || id == 9 || (id >= 12 && id <= 18);
}

bool
dg_manifest_tree_edge(const char *kind, int index)
{
	return edge_kind(kind_id(kind), index) == EDGE_TREE;
}

bool
dg_manifest_root_edge(const char *kind, int index)
{
	return edge_kind(kind_id(kind), index) == EDGE_ROOT;
}

/* The kind of visible object |hash|, under the active snapshot. */
static char *
object_kind(bytea *hash)
{
	char	   *kind;

	check_hash(hash);
	if (!dg_object_read(hash, NULL, &kind, NULL, NULL))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("manifest target object is not visible")));
	return kind;
}

/* Whether arrays of hashes |a| and |b| hold the same hashes in order. */
static bool
same_children(ArrayType *a, ArrayType *b)
{
	Datum	   *left,
			   *right;
	bool	   *leftnulls,
			   *rightnulls;
	int			nleft,
				nright;

	deconstruct_array(a, BYTEAOID, -1, false, TYPALIGN_INT, &left, &leftnulls, &nleft);
	deconstruct_array(b, BYTEAOID, -1, false, TYPALIGN_INT, &right, &rightnulls, &nright);
	if (nleft != nright)
		return false;
	for (int i = 0; i < nleft; i++)
	{
		bytea	   *l = DatumGetByteaPP(left[i]);
		bytea	   *r = DatumGetByteaPP(right[i]);

		if (leftnulls[i] || rightnulls[i] || VARSIZE_ANY_EXHDR(l) != VARSIZE_ANY_EXHDR(r) ||
			memcmp(VARDATA_ANY(l), VARDATA_ANY(r), VARSIZE_ANY_EXHDR(l)))
			return false;
	}
	return true;
}

static ArrayType *
children_array(Datum *values, int count)
{
	if (!count)
		return construct_empty_array(BYTEAOID);
	return construct_array(values, count, BYTEAOID, -1, false, TYPALIGN_INT);
}

/*
 * The retained object |hash| of |kind| (any kind if NULL) visible under the
 * active snapshot, else the transaction's. Identical objects may have several
 * rows; any one serves. Returns false if there is none; fills only the parts
 * requested, since payloads may be large.
 */
bool
dg_object_read(bytea *hash, const char *kind, char **actual, bytea **payload, ArrayType **children)
{
	Relation	catalog = table_open(DoltObjectRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		pushed = false;
	bool		found = false;

	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	ScanKeyInit(&key, Anum_pg_dolt_object_objhash, BTEqualStrategyNumber, F_BYTEAEQ, PointerGetDatum(hash));
	scan = systable_beginscan(catalog, DoltObjectKeyIndexId, true, GetActiveSnapshot(), 1, &key);
	while (!found && HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		bool		isnull;
		char	   *objkind = TextDatumGetCString(heap_getattr(tuple, Anum_pg_dolt_object_objkind, RelationGetDescr(catalog), &isnull));

		if (kind && strcmp(objkind, kind))
			continue;
		found = true;
		if (actual)
			*actual = objkind;
		if (payload)
			*payload = DatumGetByteaPCopy(heap_getattr(tuple, Anum_pg_dolt_object_objpayload, RelationGetDescr(catalog), &isnull));
		if (children)
			*children = DatumGetArrayTypePCopy(heap_getattr(tuple, Anum_pg_dolt_object_objchildren, RelationGetDescr(catalog), &isnull));
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	if (pushed)
		PopActiveSnapshot();
	return found;
}

/* Children are immutable and already visible, so construction cannot form cycles. */
/*
 * The address of an object of kind id |id| with |payload| and children
 * |links|: the hash of its envelope (magic, kind, child count, payload
 * length, child hashes, payload).
 */
static bytea *
object_address(int id, bytea *payload, Datum *links, int n)
{
	Size		length = 20 + (Size) n * PL_HASH_SIZE + VARSIZE_ANY_EXHDR(payload);
	uint8	   *encoding,
			   *p;
	PLSlice		input;
	PLHash		hash;

	if (length > MaxAllocSize)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("manifest object too large")));
	encoding = palloc(length);
	memcpy(encoding, "DGMOBJ01", 8);
	pl_write32(encoding + 8, id);
	pl_write32(encoding + 12, n);
	pl_write32(encoding + 16, VARSIZE_ANY_EXHDR(payload));
	p = encoding + 20;
	for (int i = 0; i < n; i++, p += PL_HASH_SIZE)
		memcpy(p, VARDATA_ANY(DatumGetByteaPP(links[i])), PL_HASH_SIZE);
	memcpy(p, VARDATA_ANY(payload), VARSIZE_ANY_EXHDR(payload));
	input.data = encoding;
	input.len = length;
	if (!pl_hash_bytes(input, &hash))
		elog(ERROR, "manifest SHA-256 failed");
	pfree(encoding);
	return bytes(hash.bytes, PL_HASH_SIZE);
}

static bytea *
put_object(const char *kind, bytea *payload, ArrayType *children)
{
	int			id = kind_id(kind),
				n,
				i;
	Datum	   *links;
	bool	   *nulls;
	bytea	   *address;
	char	   *existing_kind;
	bytea	   *existing_payload;
	ArrayType  *existing_children;

	if (ARR_NDIM(children) > 1 || (ARR_NDIM(children) == 1 && ARR_LBOUND(children)[0] != 1))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("manifest children must be a one-dimensional array with lower bound 1")));
	if (id == 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("tree nodes belong in the chunk store, not the manifest")));
	if (retired_kind(id))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("database states and their members belong in database roots, not the manifest")));
	deconstruct_array(children, BYTEAOID, -1, false, TYPALIGN_INT, &links, &nulls, &n);
	if (((id == 1 || id == 6) && n > 1) || ((id == 2 || id == 3) && n != 2) || (id == 5 && n < 1) || (id == 7 && n != 1) || (id == 8 && n != 3 && n < 5) || ((id == 9 || id >= 15) && n != 1) || (id == 10 && n != 2) || (id == 11 && n != 3))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid manifest child count for %s", kind)));
	if ((id == 1 || id == 6) && n == 1 &&
		(VARSIZE_ANY_EXHDR(payload) != strlen(DG_SCHEMA_TREE) ||
		 memcmp(VARDATA_ANY(payload), DG_SCHEMA_TREE, strlen(DG_SCHEMA_TREE))))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid schema tree envelope")));
	statement_snapshot_push();
	for (i = 0; i < n; i++)
	{
		char	   *child;
		bool		valid;

		if (nulls[i])
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("manifest child cannot be null")));
		if (edge_kind(id, i) == EDGE_ROOT)
		{
			dg_root_check(DatumGetByteaPP(links[i]));
			continue;
		}
		if (edge_kind(id, i) == EDGE_TREE)
		{
			bytea	   *root = DatumGetByteaPP(links[i]);
			DoltChunkStore *chunks;
			bool		stored;

			check_hash(root);
			chunks = dolt_chunk_open(DOLT_CHUNK_READ);
			stored = dolt_chunk_exists(chunks, (const uint8 *) VARDATA_ANY(root));
			dolt_chunk_close(chunks);
			if (!stored)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("manifest %s names a tree the chunk store does not hold", kind)));
			continue;
		}
		child = object_kind(DatumGetByteaPP(links[i]));
		valid = id == 5 ? strcmp(child, "commit") == 0 :
			id == 7 ? strcmp(child, "commit") == 0 :
			/* Merge children 4/5 retain working/staged databases. Optional
			 * later children retain a series HEAD and queued commits. */
			id == 8 ? strcmp(child, "commit") == 0 :
			id == 10 ? strcmp(child, "commit") == 0 :
			id == 11 ? strcmp(child, "commit") == 0 : false;
		if (!valid)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid %s child for manifest %s", child, kind)));
	}
	address = object_address(id, payload, links, n);

	/*
	 * Identical immutable objects can be inserted by independent
	 * transactions. Their snapshot-visible logical representation is
	 * deduplicated by the view.
	 */
	if (dg_object_read(address, NULL, &existing_kind, &existing_payload, &existing_children))
	{
		if (strcmp(existing_kind, kind) ||
			VARSIZE_ANY_EXHDR(existing_payload) != VARSIZE_ANY_EXHDR(payload) ||
			memcmp(VARDATA_ANY(existing_payload), VARDATA_ANY(payload), VARSIZE_ANY_EXHDR(payload)) ||
			!same_children(existing_children, children))
			elog(ERROR, "manifest object hash collision");
	}
	else
	{
		Relation	catalog = table_open(DoltObjectRelationId, RowExclusiveLock);
		Datum		values[Natts_pg_dolt_object];
		bool		valuenulls[Natts_pg_dolt_object] = {false};
		HeapTuple	tuple;

		values[Anum_pg_dolt_object_objhash - 1] = PointerGetDatum(address);
		values[Anum_pg_dolt_object_objkind - 1] = CStringGetTextDatum(kind);
		values[Anum_pg_dolt_object_objpayload - 1] = PointerGetDatum(payload);
		values[Anum_pg_dolt_object_objchildren - 1] = PointerGetDatum(children);
		values[Anum_pg_dolt_object_objid - 1] = DirectFunctionCall1(gen_random_uuid, (Datum) 0);
		tuple = heap_form_tuple(RelationGetDescr(catalog), values, valuenulls);
		CatalogTupleInsert(catalog, tuple);
		heap_freetuple(tuple);
		table_close(catalog, RowExclusiveLock);
	}
	PopActiveSnapshot();
	return address;
}

/*
 * Members of database roots. A member is passed as a reference, its entry
 * encoded on its own (dg_root.c), except a namespace, which is passed as its
 * definition object.
 */
static bytea *
address_value(const PLHash *hash)
{
	return bytes(hash->bytes, PL_HASH_SIZE);
}

static DGMember
member_of(const DGRootEntry *entry)
{
	DGMember	member;

	member.kind = entry->kind;
	member.name = entry->name;
	member.hash = strcmp(entry->kind, "namespace") == 0 ? address_value(&entry->schema) : dg_member_ref(entry);
	return member;
}

/*
 * Child |index| of member |hash|, as the member objects once had them: its
 * schema, then its content. Sets |known| unless |hash| is a reference.
 */
bytea *
dg_member_child(bytea *hash, int index, bool *known)
{
	DGRootEntry entry;

	*known = dg_is_member_ref(hash);
	if (!*known)
		return NULL;
	dg_member_decode(hash, &entry);
	if (index == 1)
		return address_value(&entry.schema);
	if (index == 2 && entry.has_content)
		return address_value(&entry.content);
	return NULL;
}

/* The members of database root |database|, in key order. */
DGMember *
dg_database_members(bytea *database, int *count)
{
	DGRootEntry *entries = dg_root_entries(database, count);
	DGMember   *result = palloc_array(DGMember, Max(*count, 1));

	for (int i = 0; i < *count; i++)
		result[i] = member_of(&entries[i]);
	return result;
}

/* The member of |kind| named |name| in |database|, or NULL. */
bytea *
dg_database_member(bytea *database, const char *kind, const char *name)
{
	DGRootEntry entry;

	if (!dg_root_find(database, kind, name, &entry))
		return NULL;
	return member_of(&entry).hash;
}

/* Store an object natively, as dolt_manifest_put does. */
bytea *
dg_manifest_put(const char *kind, bytea *payload, ArrayType *children)
{
	return put_object(kind, payload, children);
}

Datum
dolt_manifest_put(PG_FUNCTION_ARGS)
{
	bytea	   *result;

	manifest_begin();
	result = put_object(text_to_cstring(PG_GETARG_TEXT_PP(0)), PG_GETARG_BYTEA_PP(1), PG_GETARG_ARRAYTYPE_P(2));
	SPI_finish();
	PG_RETURN_BYTEA_P(result);
}

static Datum
record_result(FunctionCallInfo fcinfo, int columns)
{
	TupleDesc	desc;
	Datum		values[3];
	bool		nulls[3];
	HeapTuple	result;
	int			i;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "manifest function requires composite result");
	for (i = 0; i < columns; i++)
	{
		nulls[i] = true;
		values[i] = (Datum) 0;
		if (SPI_processed)
		{
			values[i] = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, i + 1, &nulls[i]);
			if (!nulls[i])
				values[i] = datumCopy(values[i], false, -1);
		}
	}
	result = heap_form_tuple(BlessTupleDesc(desc), values, nulls);
	SPI_finish();
	return HeapTupleGetDatum(result);
}

Datum
dolt_manifest_get(PG_FUNCTION_ARGS)
{
	Oid			types[] = {BYTEAOID};
	Datum		args[] = {PG_GETARG_DATUM(0)};

	manifest_begin();
	check_hash(PG_GETARG_BYTEA_PP(0));
	manifest_execute("SELECT objkind,objpayload,objchildren FROM pg_catalog.pg_dolt_object_visible WHERE objhash=$1", 1, types, args, NULL, true, 1);
	return record_result(fcinfo, 3);
}

static void
check_name(text *name)
{
	char	   *s = text_to_cstring(name);
	int			prefix = strncmp(s, "merge/", 6) == 0 ? 6 : strncmp(s, "views/", 6) == 0 ? 6 : strncmp(s, "rebase/", 7) == 0 ? 7 : strncmp(s, "stash/", 6) == 0 ? 6 : strncmp(s, "heads/", 6) == 0 ? 6 : strncmp(s, "tags/", 5) == 0 ? 5 : strncmp(s, "working/", 8) == 0 ? 8 : strncmp(s, "staged/", 7) == 0 ? 7 : 0;

	if (!prefix || strlen(s) == prefix || strlen(s) > 512)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("manifest ref requires heads/, tags/, staged/, working/, merge/, rebase/, stash/ or views/ and a nonempty name (maximum 512 bytes)")));
}

Datum
dolt_manifest_read(PG_FUNCTION_ARGS)
{
	Oid			types[] = {TEXTOID};
	Datum		args[] = {PG_GETARG_DATUM(0)};

	manifest_begin();
	check_name(PG_GETARG_TEXT_PP(0));
	manifest_execute("SELECT reftarget,reftoken FROM pg_catalog.pg_dolt_ref WHERE refname=$1", 1, types, args, NULL, true, 1);
	return record_result(fcinfo, 2);
}

static bool
hash_equal(bytea *a, bytea *b)
{
	return VARSIZE_ANY_EXHDR(a) == VARSIZE_ANY_EXHDR(b) && !memcmp(VARDATA_ANY(a), VARDATA_ANY(b), VARSIZE_ANY_EXHDR(a));
}

/* The schema of table or index member |hash|. */
static bytea *
member_schema(bytea *hash)
{
	DGRootEntry entry;
	ArrayType  *children;
	Datum	   *values;
	bool	   *nulls;
	int			n;

	if (dg_is_member_ref(hash))
	{
		dg_member_decode(hash, &entry);
		return address_value(&entry.schema);
	}
	if (!dg_object_read(hash, NULL, NULL, NULL, &children))
		return NULL;
	deconstruct_array(children, BYTEAOID, -1, false, TYPALIGN_INT, &values, &nulls, &n);
	return n < 1 || nulls[0] ? NULL : DatumGetByteaPP(values[0]);
}

/*
 * Statistics describe a relation's columns as its schema defines them: those
 * of |branch|'s tables and indexes whose schema working set |now| changes from
 * |old| are removed. The others' remain current; those of relations it drops
 * go with their identities.
 */
static void
drop_changed_statistics(const char *branch, bytea *old, bytea *now)
{
	int			count;
	DGMember   *members = dg_database_members(old, &count);

	for (int i = 0; i < count; i++)
	{
		bytea	   *current,
				   *before,
				   *after;
		Oid			oid;

		if (!members[i].name || (strcmp(members[i].kind, "table") && strcmp(members[i].kind, "index")))
			continue;
		current = now ? dg_database_member(now, members[i].kind, members[i].name) : NULL;
		if (!current || hash_equal(current, members[i].hash))
			continue;
		before = member_schema(members[i].hash);
		after = member_schema(current);
		if (before && after && hash_equal(before, after))
			continue;
		oid = dg_identity_lookup(branch, DOLT_IDENTITY_RELATION, members[i].name);
		if (OidIsValid(oid))
			RemoveStatistics(oid, 0);
	}
}

Datum
dolt_manifest_cas(PG_FUNCTION_ARGS)
{
	Oid			types[] = {TEXTOID, BYTEAOID, BYTEAOID, BYTEAOID};
	Datum		args[4];
	char		nulls[] = "    ";
	uint8		random[PL_HASH_SIZE];
	bytea	   *token;
	bytea	   *previous = NULL;
	text	   *name;

	manifest_begin();
	if (PG_ARGISNULL(0) || (PG_ARGISNULL(1) && PG_ARGISNULL(2)))
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("manifest CAS needs a name and expected token or target")));
	name = PG_GETARG_TEXT_PP(0);
	check_name(name);
	args[0] = PointerGetDatum(name);
	args[1] = PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1);
	args[2] = PG_ARGISNULL(2) ? (Datum) 0 : PG_GETARG_DATUM(2);
	if (PG_ARGISNULL(1))
		nulls[1] = 'n';
	else
		check_hash(PG_GETARG_BYTEA_PP(1));
	if (PG_ARGISNULL(2))
		nulls[2] = 'n';
	else
	{
		char	   *kind;
		const char *refname = text_to_cstring(name);
		char	   *expected = strncmp(refname, "merge/", 6) == 0 ? "merge" : strncmp(refname, "rebase/", 7) == 0 ? "rebase" : strncmp(refname, "stash/", 6) == 0 ? "stash" : (strncmp(refname, "working/", 8) == 0 || strncmp(refname, "staged/", 7) == 0) ? "database" : "commit";

		/* A working set names a database root, which is no object. */
		if (strcmp(expected, "database") == 0)
		{
			dg_root_check(PG_GETARG_BYTEA_PP(2));
			kind = expected;
		}
		else
		{
			statement_snapshot_push();
			kind = object_kind(PG_GETARG_BYTEA_PP(2));
			PopActiveSnapshot();
		}

		if (strcmp(kind, expected) != 0 &&
			!(strncmp(text_to_cstring(name), "tags/", 5) == 0 && strcmp(kind, "tag") == 0))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("manifest ref requires %s target", expected)));
	}
	if (!pg_strong_random(random, sizeof(random)))
		elog(ERROR, "cannot generate manifest revision token");
	token = bytes(random, sizeof(random));
	args[3] = PointerGetDatum(token);
	if (PG_ARGISNULL(1))
		manifest_execute("INSERT INTO pg_catalog.pg_dolt_ref VALUES($1,$3,$4)", 4, types, args, nulls, false, 0);
	else if (PG_ARGISNULL(2))
		manifest_execute("DELETE FROM pg_catalog.pg_dolt_ref WHERE refname=$1 AND reftoken=$2", 2, types, args, nulls, false, 0);
	else
		manifest_execute("UPDATE pg_catalog.pg_dolt_ref SET reftarget=$3,reftoken=$4 WHERE refname=$1 AND reftoken=$2 RETURNING old.reftarget", 4, types, args, nulls, false, 0);
	if (SPI_processed != 1)
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("manifest ref changed; reread and retry transaction")));
	if (!PG_ARGISNULL(1) && !PG_ARGISNULL(2))
	{
		bool		isnull;
		Datum		value = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);

		previous = isnull ? NULL : DatumGetByteaPCopy(value);
	}
	/* Branch and tag moves feed dolt_reflog. */
	if (!PG_ARGISNULL(2) && (strncmp(text_to_cstring(name), "heads/", 6) == 0 || strncmp(text_to_cstring(name), "tags/", 5) == 0))
		manifest_execute("INSERT INTO pg_catalog.pg_dolt_reflog(reflogname,reflogtime,reflogtarget) VALUES($1,clock_timestamp(),$3)", 4, types, args, nulls, false, 0);
	/* Named revision bindings must be reanalyzed after ref publication. */
	CacheInvalidateRelcacheByRelid(DoltRefRelationId);

	/*
	 * A working set names the branch's schema: rebuild descriptors of the
	 * branch's objects everywhere, including on standbys.
	 */
	if (strncmp(text_to_cstring(name), "working/", 8) == 0)
	{
		const char *branch = text_to_cstring(name) + 8;

		dg_identity_invalidate_branch(branch);
		if (previous)
			drop_changed_statistics(branch, previous, PG_ARGISNULL(2) ? NULL : PG_GETARG_BYTEA_PP(2));

		/* The branch's OIDs name exactly the objects of its new schema. */
		if (!PG_ARGISNULL(2))
		{
			List	   *members = NIL,
					   *tables = NIL;
			int			count;
			DGMember   *listed = dg_database_members(PG_GETARG_BYTEA_PP(2), &count);

			for (int i = 0; i < count; i++)
			{
				if (!listed[i].name || (strcmp(listed[i].kind, "table") && strcmp(listed[i].kind, "index") && strcmp(listed[i].kind, "sequence")))
					continue;
				members = lappend(members, (char *) listed[i].name);
				if (!strcmp(listed[i].kind, "table"))
					tables = lappend(tables, (char *) listed[i].name);
			}
			dg_identity_sync(branch, members, tables);
		}
	}
	SPI_finish();
	if (PG_ARGISNULL(2))
		PG_RETURN_NULL();
	PG_RETURN_BYTEA_P(token);
}

/*
 * The address by which objects name |tree|: its root's node hash, once every
 * node of it is in the chunk store.
 */
bytea *
dg_history_retain(const PLTree * tree)
{
	dg_tree_retain(tree);
	return bytes(tree->root.bytes, PL_HASH_SIZE);
}

Datum
dolt_manifest_export(PG_FUNCTION_ARGS)
{
	Relation	rel;
	DGState    *state;
	bytea	   *result;

	manifest_begin();
	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	if (rel->rd_tableam != dg_table_routine() &&
		!(rel->rd_rel->relkind == RELKIND_INDEX && rel->rd_rel->relam == get_index_am_oid("dolt_prolly_index", false)))
		dg_unsupported("manifest export of non-prolly relations");
	state = dg_load(rel, GetActiveSnapshot());
	result = dg_history_retain(dg_content(state));
	dg_release(state);
	relation_close(rel, AccessShareLock);
	SPI_finish();
	PG_RETURN_BYTEA_P(result);
}

/*
 * The tree whose root an object names at |address|, read through |store|,
 * which fetches its nodes from the chunk store as it needs them.
 */
PLTree
dg_history_open(PLStore *store, bytea *address)
{
	PLTree		tree = {store};

	if (VARSIZE_ANY_EXHDR(address) != PL_HASH_SIZE)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid retained tree address")));
	memcpy(tree.root.bytes, VARDATA_ANY(address), PL_HASH_SIZE);
	if (!pl_find(store, tree.root))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("retained tree is missing from the chunk store")));
	return tree;
}

/* Canonical schema properties use the same node encoding and retained chunk
 * ownership as table contents. Runtime OIDs/transaction metadata never enter
 * this tree. The outer object kind distinguishes table schemas/namespaces. */
static bytea *
schema_object(const char *kind, const char *json, bool store_it)
{
	PLStore    *volatile store = NULL;
	bytea	   *result = NULL;
	PLTree		tree;
	PLEntry    *entries;
	int			n,
				i;
	Oid			types[] = {TEXTOID};
	Datum		args[] = {CStringGetTextDatum(json)};

	manifest_begin();
	PG_TRY();
	{
		manifest_execute("SELECT key,value::text FROM jsonb_each($1::jsonb) ORDER BY key COLLATE \"C\"", 1, types, args, NULL, true, 0);
		n = SPI_processed;
		entries = palloc(sizeof(*entries) * Max(n, 1));
		for (i = 0; i < n; i++)
		{
			char	   *key = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
			char	   *value = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2);

			key = pg_server_to_any(key, strlen(key), PG_UTF8);
			value = pg_server_to_any(value, strlen(value), PG_UTF8);
			entries[i].key = (PLSlice)
			{
				(uint8 *) key, strlen(key)
			};
			entries[i].value = (PLSlice)
			{
				(uint8 *) value, strlen(value)
			};
		}
		store = dg_private_store();
		dg_check(pl_build(store, entries, n, &tree));
		{
			bytea	   *payload = bytes(DG_SCHEMA_TREE, strlen(DG_SCHEMA_TREE));
			Datum		root;

			if (store_it)
			{
				root = PointerGetDatum(dg_history_retain(&tree));
				result = put_object(kind, payload, children_array(&root, 1));
			}
			else
			{
				root = PointerGetDatum(bytes(tree.root.bytes, PL_HASH_SIZE));
				result = object_address(kind_id(kind), payload, &root, 1);
			}
		}
	}
	PG_FINALLY();
	{
		if (store)
			pl_store_free(store);
	}
	PG_END_TRY();
	SPI_finish();
	return result;
}

/* Canonical schema properties use the same node encoding and retained chunk
 * ownership as table contents. Runtime OIDs/transaction metadata never enter
 * this tree. The outer object kind distinguishes table schemas/namespaces. */
bytea *
dg_schema_store(const char *kind, const char *json)
{
	return schema_object(kind, json, true);
}

/*
 * The address dg_schema_store would give definition |json| of |kind|,
 * without writing anything: members name their schema by it.
 */
bytea *
dg_schema_address(const char *kind, const char *json)
{
	return schema_object(kind, json, false);
}

/* dolt_schema_address(definition): the schema object address a definition has. */
Datum
dolt_schema_address(PG_FUNCTION_ARGS)
{
	PG_RETURN_BYTEA_P(dg_schema_address("schema", text_to_cstring(PG_GETARG_TEXT_PP(0))));
}

/* The definition of schema object |address|, decoded; see dg_schema_cache.c. */
char *
dg_schema_decode(bytea *address, const char *kind)
{
	PLStore    *volatile store = NULL;
	char	   *result = NULL;
	bytea	   *payload;
	ArrayType  *children;
	Datum	   *values;
	bool	   *nulls;
	int			n;

	if (!dg_object_read(address, kind, NULL, &payload, &children))
		elog(ERROR, "missing retained schema object");
	deconstruct_array(children, BYTEAOID, -1, false, TYPALIGN_INT, &values, &nulls, &n);
	if (n == 0)
	{
		/* Existing repositories retain their original JSON schema objects. */
		result = pnstrdup(VARDATA_ANY(payload), VARSIZE_ANY_EXHDR(payload));
	}
	else
	{
		if (n != 1 || nulls[0] || VARSIZE_ANY_EXHDR(payload) != strlen(DG_SCHEMA_TREE) ||
			memcmp(VARDATA_ANY(payload), DG_SCHEMA_TREE, strlen(DG_SCHEMA_TREE)))
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid retained schema tree envelope")));
		PG_TRY();
		{
			PLTree		tree;
			DGRows		rows;
			size_t		i;
			StringInfoData json;

			store = dg_private_store();
			tree = dg_history_open(store, DatumGetByteaP(values[0]));
			rows = dg_rows(&tree);
			initStringInfo(&json);
			appendStringInfoChar(&json, '{');
			for (i = 0; i < rows.count; i++)
			{
				char	   *key = pnstrdup((char *) rows.items[i].key.data, rows.items[i].key.len);

				if (i)
					appendStringInfoChar(&json, ',');
				escape_json(&json, key);
				appendStringInfoChar(&json, ':');
				appendBinaryStringInfo(&json, (char *) rows.items[i].value.data, rows.items[i].value.len);
			}
			appendStringInfoChar(&json, '}');
			result = json.data;
		}
		PG_FINALLY();
		{
			if (store)
				pl_store_free(store);
		}
		PG_END_TRY();
	}
	return pg_any_to_server(result, strlen(result), PG_UTF8);
}
