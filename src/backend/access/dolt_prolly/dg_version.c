/* Native version-control command slice; see doc/doltgres/version-commands.md. */
#include "dg.h"
#include "access/doltversion.h"
#include "access/dolttransport.h"
#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "libpq/pqsignal.h"
#include "common/file_perm.h"
#include "catalog/pg_database_d.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_relation.h"
#include "utils/ruleutils.h"
#include "tcop/utility.h"
#include "tcop/tcopprot.h"
#include "access/doltcatalog.h"
#include "catalog/namespace.h"
#include "catalog/catalogview.h"
#include "catalog/heap.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_init_privs.h"
#include "catalog/pg_cast.h"
#include "catalog/dependency.h"
#include "catalog/pg_type.h"
#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_dolt_binding.h"
#include "catalog/pg_dolt_storage.h"
#include "catalog/pg_dolt_identity.h"
#include "utils/inval.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "utils/fmgroids.h"
#include "commands/alter.h"
#include "commands/comment.h"
#include "rewrite/rewriteSupport.h"
#include "parser/analyze.h"
#include "commands/view.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "commands/typecmds.h"
#include "commands/dbcommands.h"
#include "utils/acl.h"
#include "utils/syscache.h"
#include "utils/regproc.h"
#include "utils/varlena.h"
#include "parser/parse_clause.h"
#include "parser/parse_expr.h"
#include "parser/parser.h"
#include "executor/functions.h"
#include "commands/defrem.h"
#include "commands/proclang.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "optimizer/optimizer.h"
#include "access/xact.h"
#include "access/doltbranch.h"
#include "access/doltsystem.h"
#include "catalog/pg_dolt_ref.h"
#include "catalog/pg_dolt_object.h"
#include "mb/pg_wchar.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_description.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_aggregate.h"
#include "commands/extension.h"
#include "catalog/pg_range.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_language.h"
#include "parser/parse_type.h"
#include "parser/parse_func.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "commands/sequence.h"
#include "access/sequence.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_dolt_sequence.h"
#include "common/hashfn.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "parser/parse_utilcmd.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_collation.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "libpq/libpq-be.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "common/hashfn.h"
#include "utils/datum.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/portal.h"
#include "tcop/pquery.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#define NAMESPACE_DEFINITIONS_SQL "SELECT jsonb_build_object('format','DGNS1','name',n.nspname,'owner',pg_get_userbyid(n.nspowner),'comment',obj_description(n.oid,'pg_namespace'),'acl',COALESCE((SELECT jsonb_agg(jsonb_build_array(pg_get_userbyid(x.grantor),CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END,x.privilege_type,x.is_grantable) ORDER BY pg_get_userbyid(x.grantor) COLLATE \"C\",CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END COLLATE \"C\",x.privilege_type COLLATE \"C\") FROM aclexplode(COALESCE(n.nspacl,acldefault('n',n.nspowner))) x),'[]'::jsonb))::text FROM pg_catalog.pg_namespace n WHERE " USER_NAMESPACE " ORDER BY n.nspname COLLATE \"C\""
#define USER_NAMESPACE "n.nspname !~ '^pg_' AND n.nspname <> 'information_schema' AND n.nspname <> 'dolt'"

typedef struct VersionRef
{
	bytea	   *target;
	bytea	   *token;
}			VersionRef;
typedef struct Capture
{
	char	   *json;
	bytea	   *hash;
}			Capture;

/* Only set while publishing a freshly created physical relation. */

static const VersionRef empty_ref = {NULL, NULL};

/* Typed argument vectors live for the enclosing block and are consumed synchronously. */
#define VC_TYPES(...) ((Oid[]) {__VA_ARGS__})
#define VC_ARGS(...) ((Datum[]) {__VA_ARGS__})

static char *
branch_ref(const char *prefix)
{
	return psprintf("%s/%s", prefix, dg_current_branch());
}
static bytea *merge_parent(void);
static bool database_same(bytea *a, bytea *b);
static void finish_merge(void);
static bool canonical_drop(Node *statement);
static void stage_tables(List *tables);
static void stage_modified_tables(void);
static void replace_working(const char *branch, bytea *database, List *tables);
static bytea *compose_database(bytea *base, bytea *source, List *tables);
static void merge_databases(bytea *base, bytea *ours, bytea *theirs, StringInfo conflicts, StringInfo violations,
							StringInfo schema_conflicts);
static PLTree remap_rows(Relation rel, PLTree content, const char *definition, List *dropped, bool *lacks);
static void restore_table(bytea *db, const char *name);
static PLSlice fill_row(Relation rel, PLSlice row, const bool *fill);
static PLTree fill_rows(Relation rel, PLTree content, const bool *fill);
static List *merge_dropped_columns(Relation rel, const char *base);
static PLTree check_violations(Relation rel, PLTree content, const char *name, StringInfo violations);
static bytea *staged_capture(Capture * full);
static void catalog_activate(void);
static bool branch_management(char **args, int n);
static void checkout_tables(char **args, int n);
static char *resolve_object_argument(const char *arg, const char *json_a, const char *json_b, bool required);
static char *resolve_table_argument(const char *arg, const char *json_a, const char *json_b, bool required);
static char *staged_state(void);
static char *working_state(void);
static TupleDesc conflict_layout(const char *table, const char *from, const char *to);
static TupleDesc violation_layout(const char *table);
static void binding_record(const char *branch, const char *name, bytea *schema, Oid relid);
static Oid	binding_relation(const char *branch, const char *name, bytea *schema);
static void binding_prepare(const char *branch, bytea *database, bool required);

static void
run(const char *sql, int n, Oid *types, Datum *args, const char *nulls, bool ro)
{
	MemoryContext c = CurrentMemoryContext;
	SPIPlanPtr	plan = dg_manifest_plan(sql, n, types);

	if (plan)
		SPI_execute_plan(plan, args, nulls, ro, 0);
	else
		SPI_execute_with_args(sql, n, types, args, nulls, ro, 0);
	MemoryContextSwitchTo(c);
}
static Datum
cell(int row, int col)
{
	bool		null;
	Datum		d = SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, col, &null);

	if (null)
		return (Datum) 0;
	return datumCopy(d, TupleDescAttr(SPI_tuptable->tupdesc, col - 1)->attbyval, TupleDescAttr(SPI_tuptable->tupdesc, col - 1)->attlen);
}
static char *
strcell(int row, int col)
{
	char	   *s = SPI_getvalue(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, col);

	return s ? pstrdup(s) : NULL;
}

/* Suppress branch interception only for explicitly scoped internal catalog SQL.
 * Track subtransactions so a caught ERROR cannot disable ordinary binding. */
static int	catalog_api_depth;
static SubTransactionId catalog_api_subids[64];
static bool catalog_api_registered;

/*
 * Version-control calls in progress. Unlike catalog_api_depth, nested user SQL
 * does not reset this, and an error that a savepoint catches leaves it raised
 * until the transaction ends. While it is raised, working-store collection
 * waits: these calls hold tree roots that no registered state names.
 */
static int	version_calls;

bool
dg_version_call_active(void)
{
	return version_calls > 0;
}

static void
catalog_api_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_ABORT || event == XACT_EVENT_COMMIT || event == XACT_EVENT_PREPARE)
	{
		catalog_api_depth = 0;
		version_calls = 0;
	}
}
static void
catalog_api_subxact(SubXactEvent event, SubTransactionId id, SubTransactionId parent, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
		while (catalog_api_depth && catalog_api_subids[catalog_api_depth - 1] == id)
			catalog_api_depth--;
	else if (event == SUBXACT_EVENT_COMMIT_SUB)
		for (int i = 0; i < catalog_api_depth; i++)
			if (catalog_api_subids[i] == id)
				catalog_api_subids[i] = parent;
}

/* The caller's search_path, captured before the API scope replaces it. */
static char *api_search_path;

static int
begin_api(void)
{
	MemoryContext c = CurrentMemoryContext;
	int			nest = NewGUCNestLevel();

	if (catalog_api_depth == 0)
	{
		const char *path = GetConfigOption("search_path", true, false);

		if (api_search_path)
			pfree(api_search_path);
		api_search_path = MemoryContextStrdup(TopMemoryContext, path ? path : "public");
	}

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("version-control prototype requires superuser")));
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "cannot connect version-control SPI");
	MemoryContextSwitchTo(c);
	if (!catalog_api_registered)
	{
		RegisterXactCallback(catalog_api_xact, NULL);
		RegisterSubXactCallback(catalog_api_subxact, NULL);
		catalog_api_registered = true;
	}
	if (catalog_api_depth == lengthof(catalog_api_subids))
		elog(ERROR, "canonical catalog API nesting limit");
	catalog_api_subids[catalog_api_depth++] = GetCurrentSubTransactionId();
	version_calls++;
	PushCopiedSnapshot(GetActiveSnapshot());
	set_config_option("search_path", "pg_catalog, pg_temp", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("quote_all_identifiers", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	return nest;
}
static void
end_api(int nest)
{
	Assert(catalog_api_depth > 0);
	catalog_api_depth--;
	if (version_calls > 0)
		version_calls--;
	PopActiveSnapshot();
	SPI_finish();
	AtEOXact_GUC(true, nest);
}

/* User SQL must not inherit the internal manifest lookup bypass. Nested
 * provider calls may enter begin_api again, so preserve the caller path. */
static void
run_context(const char *sql, int nargs, Oid *types, Datum *args,
			const char *nulls, bool readonly)
{
	int			depth = catalog_api_depth;
	int			nest = NewGUCNestLevel();
	char	   *path = pstrdup(api_search_path ? api_search_path : "public");
	SubTransactionId subids[64];
	CatalogView *previous = CatalogViewSwitch(NULL);

	memcpy(subids, catalog_api_subids, sizeof(subids));

	PG_TRY();
	{
		catalog_api_depth = 0;
		set_config_option("search_path", path, PGC_USERSET, PGC_S_SESSION,
						  GUC_ACTION_SAVE, true, 0, false);
		run(sql, nargs, types, args, nulls, readonly);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		catalog_api_depth = depth;
		memcpy(catalog_api_subids, subids, sizeof(subids));
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
}

typedef struct StandaloneCall
{
	Oid			funcid;
	FuncExpr   *call;
	int			count;
}			StandaloneCall;

static bool
standalone_walker(Node *node, StandaloneCall * found)
{
	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr) && ((FuncExpr *) node)->funcid == found->funcid)
	{
		found->call = (FuncExpr *) node;
		found->count++;
		return false;
	}
	if (IsA(node, SubPlan) || IsA(node, SubLink) || IsA(node, Var))
	{
		found->count += 2;		/* disqualify */
		return true;
	}
	return expression_tree_walker(node, standalone_walker, found);
}

static void
standalone(FunctionCallInfo fcinfo, bool commits)
{
	PlannedStmt *stmt;
	Plan	   *plan;
	FuncExpr   *call = NULL;

	if (!ActivePortal || list_length(ActivePortal->stmts) != 1)
		goto bad;
	stmt = linitial_node(PlannedStmt, ActivePortal->stmts);
	if (stmt->commandType != CMD_SELECT)
		goto bad;
	plan = stmt->planTree;
	if (plan->qual || plan->initPlan || outerPlan(plan) || innerPlan(plan))
		goto bad;
	if (IsA(plan, Result) && list_length(plan->targetlist) == 1)
	{
		Node	   *expr = (Node *) linitial_node(TargetEntry, plan->targetlist)->expr;
		StandaloneCall found = {fcinfo->flinfo->fn_oid, NULL, 0};

		/* The call may be wrapped in scalar expressions (casts, substring). */
		standalone_walker(expr, &found);
		if (found.count == 1)
			call = found.call;
	}
	else if (IsA(plan, FunctionScan) && list_length(((FunctionScan *) plan)->functions) == 1)
	{
		Node	   *expr = linitial_node(RangeTblFunction, ((FunctionScan *) plan)->functions)->funcexpr;
		ListCell   *lc;

		if (IsA(expr, FuncExpr))
			call = (FuncExpr *) expr;
		foreach(lc, plan->targetlist) if (!IsA(lfirst_node(TargetEntry, lc)->expr, Var))
			goto bad;
	}
	if (!call || call->funcid != fcinfo->flinfo->fn_oid)
		goto bad;
	{
		ListCell   *lc;

		foreach(lc, call->args)
			if (!IsA(lfirst(lc), Const) && !IsA(lfirst(lc), Param) && !IsA(lfirst(lc), ArrayExpr))
			goto bad;
	}
	if (commits)
		PreventInTransactionBlock(true, "version-control command");
	return;
bad:
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("version-control mutation requires a standalone SELECT function call")));
}
static char **
arguments(FunctionCallInfo fcinfo, int *count)
{
	Datum	   *values;
	bool	   *nulls;
	char	  **args;
	int			i;

	if (PG_NARGS() == 0)
	{
		*count = 0;
		return NULL;
	}
	if (PG_ARGISNULL(0))
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("command arguments cannot be null")));
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(0), TEXTOID, -1, false, TYPALIGN_INT, &values, &nulls, count);
	args = palloc(sizeof(char *) * Max(*count, 1));
	for (i = 0; i < *count; i++)
	{
		if (nulls[i])
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("command arguments cannot be null")));
		args[i] = TextDatumGetCString(values[i]);
	}
	return args;
}
static void
unsupported(const char *arg)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unsupported version-control option or operation: %s", arg)));
}
static char *
hash_format(bytea *v)
{
	PLHash		hash;
	char	   *result = palloc(PL_HASH_TEXT_SIZE + 1);

	if (VARSIZE_ANY_EXHDR(v) != PL_HASH_SIZE)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid content hash length")));
	memcpy(hash.bytes, VARDATA_ANY(v), PL_HASH_SIZE);
	pl_hash_format(hash, result);
	return result;
}
static bytea *
hash_parse(const char *s)
{
	PLHash		hash;
	bytea	   *v;

	if (!pl_hash_parse(s, &hash))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid commit hash")));
	v = palloc(VARHDRSZ + PL_HASH_SIZE);
	SET_VARSIZE(v, VARHDRSZ + PL_HASH_SIZE);
	memcpy(VARDATA(v), hash.bytes, PL_HASH_SIZE);
	return v;
}
Datum
dolt_hash_encode(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(hash_format(PG_GETARG_BYTEA_PP(0))));
}
Datum
dolt_hash_decode(PG_FUNCTION_ARGS)
{
	PG_RETURN_BYTEA_P(hash_parse(text_to_cstring(PG_GETARG_TEXT_PP(0))));
}
static bool
same(bytea *a, bytea *b)
{
	return a && b && VARSIZE_ANY_EXHDR(a) == VARSIZE_ANY_EXHDR(b) && !memcmp(VARDATA_ANY(a), VARDATA_ANY(b), VARSIZE_ANY_EXHDR(a));
}

/*
 * Refs and retained objects are read natively, under the snapshot internal
 * SQL would use: the active one. Only the callers that need them pay for
 * payloads, which may be large.
 */
static Snapshot
api_snapshot(void)
{
	return ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
}

static VersionRef
read_ref(const char *name)
{
	VersionRef	r = {0};
	Relation	catalog = table_open(DoltRefRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;

	ScanKeyInit(&key, Anum_pg_dolt_ref_refname, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(name));
	scan = systable_beginscan(catalog, DoltRefKeyIndexId, true, api_snapshot(), 1, &key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		bool		isnull;
		Datum		value = heap_getattr(tuple, Anum_pg_dolt_ref_reftarget, RelationGetDescr(catalog), &isnull);

		if (!isnull)
			r.target = DatumGetByteaPCopy(value);
		value = heap_getattr(tuple, Anum_pg_dolt_ref_reftoken, RelationGetDescr(catalog), &isnull);
		if (!isnull)
			r.token = DatumGetByteaPCopy(value);
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return r;
}

static bool
object_read(bytea *hash, const char *kind, char **actual, bytea **payload, ArrayType **children)
{
	return dg_object_read(hash, kind, actual, payload, children);
}

/*
 * Whether object |hash| of |kind| exists, and the transaction that created
 * it if that is this transaction and no other copy is committed.
 */
static bool
object_creator(bytea *hash, const char *kind, bool *found, TransactionId *creator)
{
	Relation	catalog = table_open(DoltObjectRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;

	*found = false;
	*creator = InvalidTransactionId;
	ScanKeyInit(&key, Anum_pg_dolt_object_objhash, BTEqualStrategyNumber, F_BYTEAEQ, PointerGetDatum(hash));
	scan = systable_beginscan(catalog, DoltObjectKeyIndexId, true, api_snapshot(), 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		bool		isnull;
		char	   *objkind = TextDatumGetCString(heap_getattr(tuple, Anum_pg_dolt_object_objkind, RelationGetDescr(catalog), &isnull));
		TransactionId xid = HeapTupleHeaderGetXmin(tuple->t_data);

		if (strcmp(objkind, kind))
			continue;
		*found = true;
		if (!TransactionIdIsCurrentTransactionId(xid))
		{
			*creator = InvalidTransactionId;
			break;
		}
		*creator = xid;
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return *found;
}

/* The |index|th (from 1) child of object |hash|, or NULL. */
static bytea *
object_child(bytea *hash, int index)
{
	ArrayType  *children;
	Datum	   *values;
	bool	   *nulls;
	int			n;
	bool		known;
	bytea	   *member = dg_member_child(hash, index, &known);

	if (known)
		return member;
	if (!object_read(hash, NULL, NULL, NULL, &children))
		return NULL;
	deconstruct_array(children, BYTEAOID, -1, false, TYPALIGN_INT, &values, &nulls, &n);
	if (index < 1 || index > n || nulls[index - 1])
		return NULL;
	return DatumGetByteaPCopy(values[index - 1]);
}
static void
move_ref(const char *name, VersionRef old, bytea *target)
{
	Oid			types[] = {TEXTOID, BYTEAOID, BYTEAOID};
	Datum		args[] = {CStringGetTextDatum(name), PointerGetDatum(old.token), PointerGetDatum(target)};
	char		nulls[] = "   ";

	if (!old.token)
		nulls[1] = 'n';
	if (!target)
		nulls[2] = 'n';
	run("SELECT pg_catalog.dolt_manifest_cas($1,$2,$3)", 3, types, args, nulls, false);
	if (strncmp(name, "working/", 8) == 0)
		dg_version_mark_branch(name + 8);
}
static ArrayType *
links(Datum *hashes, int count)
{
	return count ? construct_array(hashes, count, BYTEAOID, -1, false, TYPALIGN_INT) : construct_empty_array(BYTEAOID);
}
static bytea *
put(const char *kind, const char *json, Datum *hashes, int count)
{
	bytea	   *result;

	if ((!strcmp(kind, "schema") || !strcmp(kind, "namespace")) && count == 0)
		result = dg_schema_store(kind, json);
	else
	{
		/* The payload is the JSON normalized, in UTF-8. */
		char	   *normalized = DatumGetCString(DirectFunctionCall1(jsonb_out, DirectFunctionCall1(jsonb_in, CStringGetDatum(json))));
		char	   *encoded = pg_server_to_any(normalized, strlen(normalized), PG_UTF8);

		result = dg_manifest_put(kind, (bytea *) cstring_to_text_with_len(encoded, strlen(encoded)), links(hashes, count));
	}
	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
	return result;
}
static char *
metadata(bytea *hash, const char *kind)
{
	bytea	   *payload;
	char	   *text;

	if (!strcmp(kind, "schema") || !strcmp(kind, "namespace"))
		return dg_schema_read(hash, kind);
	if (!object_read(hash, kind, NULL, &payload, NULL))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("revision does not name a visible %s object", kind)));
	text = pnstrdup(VARDATA_ANY(payload), VARSIZE_ANY_EXHDR(payload));

	return pg_any_to_server(text, strlen(text), PG_UTF8);
}
static bytea *
child(bytea *hash, int index)
{
	bytea	   *result = object_child(hash, index);

	if (!result)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("revision has no requested ancestor or state")));
	return result;
}
static void
json_field(StringInfo b, const char *key, const char *value, bool comma)
{
	if (comma)
		appendStringInfoChar(b, ',');
	escape_json(b, key);
	appendStringInfoChar(b, ':');
	escape_json(b, value);
}

/* A schema object's comment, as a "comment" field following others, if it has one. */
static void
json_comment(StringInfo b, Oid object, Oid catalog)
{
	char	   *comment = GetComment(object, catalog, 0);

	if (comment)
		json_field(b, "comment", comment, true);
}

/* The canonical membership of |branch|: its working schema, else its head's. */
static bytea *
catalog_database(const char *branch)
{
	VersionRef	ref;

	ref = read_ref(psprintf("working/%s", branch));
	if (ref.target)
		return ref.target;
	ref = read_ref(psprintf("heads/%s", branch));
	return ref.target ? child(ref.target, 1) : NULL;
}
static bool
catalog_contains(bytea *db, const char *name)
{
	if (!db)
		return true;
	return dg_database_member(db, "table", name) || dg_database_member(db, "index", name) ||
		dg_database_member(db, "view", name) || dg_database_member(db, "sequence", name);
}

static CatalogView * provider_view(bytea *database);

static int
compare_members(const void *a, const void *b)
{
	const		DGMember *left = a;
	const		DGMember *right = b;
	int			kind = strcmp(left->kind, right->kind);
	int			length = Min(VARSIZE_ANY_EXHDR(left->hash), VARSIZE_ANY_EXHDR(right->hash));
	int			bytes;

	if (kind)
		return kind;
	bytes = memcmp(VARDATA_ANY(left->hash), VARDATA_ANY(right->hash), length);
	if (bytes)
		return bytes;
	return (int) VARSIZE_ANY_EXHDR(left->hash) - (int) VARSIZE_ANY_EXHDR(right->hash);
}

/* The members of |database| ordered by kind, then hash. */
static DGMember *
sorted_members(bytea *database, int *count)
{
	DGMember   *members = dg_database_members(database, count);

	qsort(members, *count, sizeof(DGMember), compare_members);
	return members;
}
static CatalogView * provider_branch_view(const char *branch, bytea *database);
static char *json_value(const char *json, const char *key);
static JsonbValue json_document(const char *json);
static bool json_get(JsonbValue *object, const char *key, JsonbValue *result);
static int	json_length(JsonbValue *array);
static JsonbValue json_element(JsonbValue *array, int index);
static char *json_item_text(JsonbValue *array, int index);
static char *json_field_text(JsonbValue *object, const char *key);
static Node **schema_defaults(const char *definition, int *count);
static Constraint **schema_constraints(const char *definition, int *count);
static IndexStmt *schema_index(const char *definition);
static char *json_set_string(const char *json, const char *key, const char *value);

static char *database_dependents(const char *json, Datum *members, int count);
static char *routine_body_encode(HeapTuple tuple, char **dependencies);
static Oid	provider_view_dependency(CatalogView * view, const char *kind, const char *name);

static bytea *
canonical_database(const char *json, Datum *members, int count)
{
	char	   *normalized;
	ArrayType  *array;
	Datum	   *values;
	bool	   *nulls;
	int			n;

	run("SELECT jsonb_set($1::jsonb || jsonb_build_object('encoding',current_setting('server_encoding')),'{members}',COALESCE((SELECT jsonb_agg(v ORDER BY v->>'kind' COLLATE \"C\",COALESCE(v->>'name',v->'definition'->>'name') COLLATE \"C\") FROM jsonb_array_elements($1::jsonb->'members') v),'[]'::jsonb))::text", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json)), NULL, true);
	normalized = database_dependents(strcell(0, 1), members, count);
	run("SELECT coalesce(array_agg(h ORDER BY h),'{}'::bytea[]) FROM unnest($1::bytea[]) h", 1, VC_TYPES(BYTEAARRAYOID), VC_ARGS(PointerGetDatum(links(members, count))), NULL, true);
	array = DatumGetArrayTypeP(cell(0, 1));
	deconstruct_array(array, BYTEAOID, -1, false, TYPALIGN_INT, &values, &nulls, &n);
	return put("database", normalized, values, n);
}

#include "dg_expression_codec.inc"
#include "dg_foreign_codec.inc"
#include "dg_routine_codec.inc"
#include "dg_schema_object_codec.inc"
#include "dg_sequence_codec.inc"
#include "dg_type_codec.inc"
#include "dg_dependents.inc"

/* Capture and working-tree diff must use exactly the same portable schema codec. */
static char *
native_schema_definition(Oid relid)
{
	int			nest = NewGUCNestLevel();
	char	   *definition;

	PG_TRY();
	{
		schema_expression_gucs();
		run("SELECT jsonb_build_object('format','DGTABLE1','name',format('%I.%I',n.nspname,c.relname),'owner',pg_get_userbyid(c.relowner),'comment',obj_description(c.oid,'pg_class'),'acl',COALESCE((SELECT jsonb_agg(jsonb_build_array(pg_get_userbyid(x.grantor),CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END,x.privilege_type,x.is_grantable) ORDER BY pg_get_userbyid(x.grantor) COLLATE \"C\",CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END COLLATE \"C\",x.privilege_type COLLATE \"C\") FROM aclexplode(COALESCE(c.relacl,acldefault('r',c.relowner))) x),'[]'::jsonb),'columns',COALESCE((SELECT jsonb_agg(jsonb_build_object('name',a.attname,'comment',col_description(c.oid,a.attnum),'type',t.typname,'typmod',a.atttypmod,'notnull',a.attnotnull,'collation',CASE WHEN a.attcollation=0 THEN NULL ELSE (SELECT jsonb_build_array(cn.nspname,co.collname,co.collprovider,co.collisdeterministic,co.collcollate,co.collctype,co.colllocale,co.collversion) FROM pg_collation co JOIN pg_namespace cn ON cn.oid=co.collnamespace WHERE co.oid=a.attcollation) END) || jsonb_strip_nulls(jsonb_build_object('default',(SELECT pg_get_expr(d.adbin,d.adrelid) FROM pg_attrdef d WHERE d.adrelid=a.attrelid AND d.adnum=a.attnum),'storage',CASE WHEN a.attstorage<>t.typstorage THEN a.attstorage::text END,'compression',nullif(a.attcompression::text,''),'statistics',a.attstattarget,'options',(SELECT jsonb_agg(v ORDER BY v COLLATE \"C\") FROM unnest(a.attoptions) v))) ORDER BY a.attnum) FROM pg_attribute a JOIN pg_type t ON t.oid=a.atttypid WHERE a.attrelid=c.oid AND a.attnum>0),'[]'::jsonb),'indexes',COALESCE((SELECT jsonb_agg(pg_get_indexdef(x.indexrelid) ORDER BY ic.relname COLLATE \"C\") FROM pg_index x JOIN pg_class ic ON ic.oid=x.indexrelid WHERE x.indrelid=c.oid),'[]'::jsonb),'constraints',COALESCE((SELECT jsonb_agg(jsonb_build_array(conname,pg_get_constraintdef(oid)) ORDER BY conname COLLATE \"C\") FROM pg_constraint WHERE conrelid=c.oid),'[]'::jsonb),'index_definition',CASE WHEN c.relkind='i' THEN pg_get_indexdef(c.oid) ELSE NULL END)::text FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE c.oid=$1", 1, VC_TYPES(OIDOID), VC_ARGS(ObjectIdGetDatum(relid)), NULL, true);
		definition = foreign_capture_definition(strcell(0, 1), relid);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	return definition;
}


/* Capture canonical membership and schema, selecting only data from local roots.
 * Physical pg_class enumeration cannot describe branch-only objects. */
static Capture
capture_canonical(bytea *database, bool persist, bool definitions_only)
{
	Capture		result = {0};

	/* The working schema of the session's branch, whose view is current. */
	CatalogView *view = provider_branch_view(dg_current_branch(), database);
	bytea	  **objects;
	char	  **kinds;
	Datum	   *members;
	int			n,
				i;
	StringInfoData json;

	DGMember   *listed = sorted_members(database, &n);

	objects = palloc_array(bytea *, Max(n, 1));
	kinds = palloc_array(char *, Max(n, 1));
	members = palloc_array(Datum, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		objects[i] = listed[i].hash;
		kinds[i] = (char *) listed[i].kind;
	}
	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGDB1\",\"members\":[");
	for (i = 0; i < n; i++)
	{
		char	   *payload = metadata(objects[i], kinds[i]);

		members[i] = PointerGetDatum(objects[i]);
		if (i)
			appendStringInfoChar(&json, ',');
		if (!strcmp(kinds[i], "namespace"))
			appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", payload);
		else if (!strcmp(kinds[i], "sequence"))
		{
			char	   *definition = metadata(child(objects[i], 1), "schema");

			if (!definitions_only)
				definition = sequence_capture(definition);
			appendStringInfoString(&json, object_payload("sequence", definition));
			if (persist)
				members[i] = PointerGetDatum(object_member("sequence", definition));
		}
		else if (schema_object_class(kinds[i]) || !strcmp(kinds[i], "routine") || !strcmp(kinds[i], "trigger") || !strcmp(kinds[i], "type") || !strcmp(kinds[i], "view"))
			appendStringInfoString(&json, payload);
		else
		{
			char	   *name = json_value(payload, "name");
			bytea	   *schema = child(objects[i], 1);
			Oid			resource = binding_relation(dg_current_branch(), name, schema);

			if (!definitions_only)
			{
				PLHash		content;
				bool		load;
				bool		present;
				DGState    *loaded = NULL;

				if (!OidIsValid(resource))
					ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding unavailable for %s on branch %s", name, dg_current_branch())));
				present = dg_storage_resource_root(resource, dg_current_branch(), GetActiveSnapshot(), &content, &load);

				/* Only a full load folds the journal into an index's content. */
				if (load)
				{
					DGStorage  *storage = dg_storage_open(CatalogViewLookupObject(view, RelationRelationId, name), dg_current_branch(), name, schema, resource, GetActiveSnapshot());

					loaded = dg_storage_load(storage);
					dg_storage_close(storage);
					present = loaded->has_root;
					content = loaded->content.root;
				}
				if (present)
				{
					bytea	   *raw = palloc(VARHDRSZ + PL_HASH_SIZE);
					char	   *root;
					char	   *recorded = json_value(payload, "content");

					SET_VARSIZE(raw, VARHDRSZ + PL_HASH_SIZE);
					memcpy(VARDATA(raw), content.bytes, PL_HASH_SIZE);
					root = hash_format(raw);

					/*
					 * A member that already records these contents is the
					 * member this capture would make: its retained tree is
					 * that root's. Only changed contents are loaded to be
					 * retained.
					 */
					if (!recorded || strcmp(recorded, root))
					{
						payload = json_set_string(payload, "content", root);
						if (persist)
						{
							DGState    *state = loaded;

							if (!state)
							{
								DGStorage  *storage = dg_storage_open(CatalogViewLookupObject(view, RelationRelationId, name), dg_current_branch(), name, schema, resource, GetActiveSnapshot());

								state = dg_storage_load(storage);
								dg_storage_close(storage);
							}
							members[i] = PointerGetDatum(put(kinds[i], payload, VC_ARGS(PointerGetDatum(schema), PointerGetDatum(dg_history_export_tree(&state->content))), 2));
							if (state != loaded)
								dg_release(state);
						}
					}
				}
				if (loaded)
					dg_release(loaded);
			}
			appendStringInfoString(&json, payload);
		}
	}
	appendStringInfoString(&json, "]}");
	run("SELECT jsonb_build_object('format','DGDB1','encoding',current_setting('server_encoding'),'members',COALESCE(jsonb_agg(v ORDER BY v->>'kind' COLLATE \"C\",COALESCE(v->>'name',v->'definition'->>'name') COLLATE \"C\"),'[]'::jsonb))::text FROM jsonb_array_elements($1::jsonb->'members') v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json.data)), NULL, true);
	result.json = strcell(0, 1);
	if (persist)
		result.hash = canonical_database(result.json, members, n);
	return result;
}

/* The working schema of the session's branch, with its tables' live content. */
static Capture
capture_inner(bool persist, bool all_catalogs)
{
	return capture_canonical(read_ref(branch_ref("working")).target, persist, all_catalogs);
}
static Capture
capture_mode(bool persist, bool all_catalogs)
{
	Capture		result;
	int			nest = NewGUCNestLevel();

	set_config_option("search_path", "pg_catalog, pg_temp", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("quote_all_identifiers", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	PG_TRY();
	{
		result = capture_inner(persist, all_catalogs);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	return result;
}

static Capture
capture(bool persist)
{
	return capture_mode(persist, false);
}

/*
 * Recently parsed definitions, by text. Decoding a schema reads many fields of
 * each definition, so each is parsed once rather than once per field.
 */
#define PARSED_JSON_ENTRIES 32
typedef struct ParsedJson
{
	char	   *text;
	Size		len;
	uint32		hash;
	Jsonb	   *value;
}			ParsedJson;
static ParsedJson parsed_json[PARSED_JSON_ENTRIES];
static int	parsed_json_next;

static Jsonb *
json_parse(const char *json)
{
	DGSchema   *schema = dg_schema_find(json);
	Size		len;
	uint32		hash;
	ParsedJson *entry;
	Jsonb	   *value;
	MemoryContext previous;

	/* A schema definition was parsed when it was decoded. */
	if (schema)
		return schema->document;
	len = strlen(json);
	hash = hash_bytes((const unsigned char *) json, len);
	for (int i = 0; i < PARSED_JSON_ENTRIES; i++)
	{
		entry = &parsed_json[i];
		if (entry->value && entry->hash == hash && entry->len == len && !memcmp(entry->text, json, len))
			return entry->value;
	}
	value = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(json)));
	entry = &parsed_json[parsed_json_next];
	parsed_json_next = (parsed_json_next + 1) % PARSED_JSON_ENTRIES;
	if (entry->value)
	{
		pfree(entry->text);
		pfree(entry->value);
	}
	previous = MemoryContextSwitchTo(TopMemoryContext);
	entry->text = pnstrdup(json, len);
	entry->value = palloc(VARSIZE(value));
	memcpy(entry->value, value, VARSIZE(value));
	MemoryContextSwitchTo(previous);
	entry->len = len;
	entry->hash = hash;
	return entry->value;
}

/* A JSON value as text, as the ->> operator gives it. */
static char *
json_text(JsonbValue *value)
{
	switch (value->type)
	{
		case jbvNull:
			return NULL;
		case jbvBool:
			return pstrdup(value->val.boolean ? "true" : "false");
		case jbvString:
			return pnstrdup(value->val.string.val, value->val.string.len);
		case jbvNumeric:
			return DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(value->val.numeric)));
		case jbvBinary:
			{
				StringInfoData text;

				initStringInfo(&text);
				(void) JsonbToCString(&text, value->val.binary.data, value->val.binary.len);
				return text.data;
			}
		default:
			elog(ERROR, "unrecognized jsonb type: %d", (int) value->type);
			return NULL;
	}
}

/* Field |key| of JSON object |json| as text, as $1::jsonb->>$2 gives it. */
static char *
json_value(const char *json, const char *key)
{
	Jsonb	   *parsed = json_parse(json);
	JsonbValue	buffer;
	JsonbValue *value;

	if (!JB_ROOT_IS_OBJECT(parsed))
		return NULL;
	value = getKeyJsonValueFromContainer(&parsed->root, key, strlen(key), &buffer);
	return value ? json_text(value) : NULL;
}

/*
 * Definition JSON read natively. A document is a jbvBinary value over a copy
 * of its parsed container, valid for the caller's memory context.
 */
static JsonbValue
json_document(const char *json)
{
	Jsonb	   *parsed = json_parse(json);
	Jsonb	   *copy = palloc(VARSIZE(parsed));
	JsonbValue	result;

	memcpy(copy, parsed, VARSIZE(parsed));
	result.type = jbvBinary;
	result.val.binary.data = &copy->root;
	result.val.binary.len = VARSIZE(copy) - VARHDRSZ;
	return result;
}

/* Field |key| of object |object|; false when it is absent or JSON null. */
static bool
json_get(JsonbValue *object, const char *key, JsonbValue *result)
{
	if (object->type != jbvBinary || !JsonContainerIsObject(object->val.binary.data))
		return false;
	if (!getKeyJsonValueFromContainer(object->val.binary.data, key, strlen(key), result))
		return false;
	return result->type != jbvNull;
}

/* The number of elements of |array|, 0 if it is not an array. */
static int
json_length(JsonbValue *array)
{
	if (array->type != jbvBinary || !JsonContainerIsArray(array->val.binary.data) ||
		JsonContainerIsScalar(array->val.binary.data))
		return 0;
	return JsonContainerSize(array->val.binary.data);
}

/* Element |index| of |array|. */
static JsonbValue
json_element(JsonbValue *array, int index)
{
	JsonbValue *element = getIthJsonbValueFromContainer(array->val.binary.data, index);

	if (!element)
		elog(ERROR, "JSON array element %d is missing", index);
	return *element;
}

/* Element |index| of |array| as ->> gives it, or NULL. */
static char *
json_item_text(JsonbValue *array, int index)
{
	JsonbValue	element;

	if (index >= json_length(array))
		return NULL;
	element = json_element(array, index);
	return json_text(&element);
}

/* Element |index| of |array| as JSON text, as (->)::text gives it, or NULL. */
static char *
json_item_json(JsonbValue *array, int index)
{
	JsonbValue	element;
	Jsonb	   *encoded;

	if (index >= json_length(array))
		return NULL;
	element = json_element(array, index);
	encoded = JsonbValueToJsonb(&element);
	return JsonbToCString(NULL, &encoded->root, VARSIZE(encoded));
}

/*
 * |json| with string field |key| set to |value|, as
 * jsonb_set($1::jsonb,'{key}',to_jsonb($2::text))::text gives it.
 */
static char *
json_set_string(const char *json, const char *key, const char *value)
{
	StringInfoData encoded;
	Datum		path = CStringGetTextDatum(key);
	Datum		result;

	initStringInfo(&encoded);
	escape_json(&encoded, value);
	result = DirectFunctionCall4(jsonb_set,
								 DirectFunctionCall1(jsonb_in, CStringGetDatum(json)),
								 PointerGetDatum(construct_array_builtin(&path, 1, TEXTOID)),
								 DirectFunctionCall1(jsonb_in, CStringGetDatum(encoded.data)),
								 BoolGetDatum(true));
	return DatumGetCString(DirectFunctionCall1(jsonb_out, result));
}

/*
 * The elements of array field |key| of |json|, as jsonb_array_elements gives
 * them, and their number in |count|: none if the field is absent.
 */
static JsonbValue *
json_elements(const char *json, const char *key, int *count)
{
	JsonbValue	document = json_document(json);
	JsonbValue	array;
	JsonbValue *result;

	*count = json_get(&document, key, &array) ? json_length(&array) : 0;
	result = palloc_array(JsonbValue, Max(*count, 1));
	for (int i = 0; i < *count; i++)
		result[i] = json_element(&array, i);
	return result;
}

/* |text| as ::int reads it; 0 for NULL, as an internal query's cell reads. */
static int
json_int(const char *text)
{
	return text ? DatumGetInt32(DirectFunctionCall1(int4in, CStringGetDatum(text))) : 0;
}

/* |text| as ::bool reads it; false for NULL. */
static bool
json_bool(const char *text)
{
	return text &&DatumGetBool(DirectFunctionCall1(boolin, CStringGetDatum(text)));
}

/*
 * JSON built natively, as the SQL jsonb functions build it: jsonb_build_object
 * keeps null values, jsonb_strip_nulls drops them, and keys come out in jsonb
 * order.
 */
static JsonbValue
jv_string(const char *text)
{
	JsonbValue	value;

	value.type = jbvString;
	value.val.string.val = pstrdup(text);
	value.val.string.len = strlen(text);
	return value;
}

static JsonbValue
jv_null(void)
{
	JsonbValue	value;

	value.type = jbvNull;
	return value;
}

static JsonbValue
jv_int(int number)
{
	JsonbValue	value;

	value.type = jbvNumeric;
	value.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum(number)));
	return value;
}

static JsonbValue
jv_bool(bool flag)
{
	JsonbValue	value;

	value.type = jbvBool;
	value.val.boolean = flag;
	return value;
}

static JsonbValue
jv_jsonb(Jsonb *jsonb)
{
	JsonbValue	value;

	value.type = jbvBinary;
	value.val.binary.data = &jsonb->root;
	value.val.binary.len = VARSIZE(jsonb) - VARHDRSZ;
	return value;
}

/* An object of |count| |keys| and |values|; with |strip|, without nulls. */
static Jsonb *
jb_object(int count, const char *const *keys, JsonbValue *values, bool strip)
{
	JsonbParseState *state = NULL;

	pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
	for (int i = 0; i < count; i++)
	{
		JsonbValue	key = jv_string(keys[i]);

		if (strip && values[i].type == jbvNull)
			continue;
		pushJsonbValue(&state, WJB_KEY, &key);
		pushJsonbValue(&state, WJB_VALUE, &values[i]);
	}
	return JsonbValueToJsonb(pushJsonbValue(&state, WJB_END_OBJECT, NULL));
}

/* An array of |count| |values|. */
static Jsonb *
jb_array(int count, JsonbValue *values)
{
	JsonbParseState *state = NULL;

	pushJsonbValue(&state, WJB_BEGIN_ARRAY, NULL);
	for (int i = 0; i < count; i++)
		pushJsonbValue(&state, WJB_ELEM, &values[i]);
	return JsonbValueToJsonb(pushJsonbValue(&state, WJB_END_ARRAY, NULL));
}

/* |a| || |b|. */
static Jsonb *
jb_merge(Jsonb *a, Jsonb *b)
{
	return DatumGetJsonbP(DirectFunctionCall2(jsonb_concat, JsonbPGetDatum(a), JsonbPGetDatum(b)));
}

/* |object| - |key|. */
static Jsonb *
jb_without(Jsonb *object, const char *key)
{
	return DatumGetJsonbP(DirectFunctionCall2(jsonb_delete, JsonbPGetDatum(object), CStringGetTextDatum(key)));
}

/* |json| with field |key| set to |value|, as jsonb_set(...)::text gives it. */
static char *
json_set_jsonb(const char *json, const char *key, Jsonb *value)
{
	Datum		path = CStringGetTextDatum(key);
	Datum		result = DirectFunctionCall4(jsonb_set,
											 DirectFunctionCall1(jsonb_in, CStringGetDatum(json)),
											 PointerGetDatum(construct_array_builtin(&path, 1, TEXTOID)),
											 JsonbPGetDatum(value),
											 BoolGetDatum(true));

	return DatumGetCString(DirectFunctionCall1(jsonb_out, result));
}

/*
 * Whether definitions |a| and |b| store alike: a schema object holds a
 * definition's fields as jsonb text, so formatting does not matter.
 */
static bool
json_same(const char *a, const char *b)
{
	return !strcmp(a, b) ||
		!strcmp(DatumGetCString(DirectFunctionCall1(jsonb_out, DirectFunctionCall1(jsonb_in, CStringGetDatum(a)))),
				DatumGetCString(DirectFunctionCall1(jsonb_out, DirectFunctionCall1(jsonb_in, CStringGetDatum(b)))));
}

/* Database payloads fingerprint logical members. Sequence children can hold
 * different restore seeds for the same definition; ref CAS still uses same(). */
static bool
database_same(bytea *a, bytea *b)
{
	return same(a, b) || (a && b && json_same(metadata(a, "database"), metadata(b, "database")));
}

/* Field |key| of |object| as ->> gives it, or NULL. */
static char *
json_field_text(JsonbValue *object, const char *key)
{
	JsonbValue	value;

	return json_get(object, key, &value) ? json_text(&value) : NULL;
}

/* Doltgres derives the default committer email from the role and the client
 * address; local (socket or bootstrap) sessions use localhost. */
static char *
default_email(const char *name)
{
	const char *host = "localhost";

	if (MyProcPort && MyProcPort->remote_host && *MyProcPort->remote_host &&
		MyProcPort->raddr.addr.ss_family != AF_UNIX)
		host = MyProcPort->remote_host;
	return psprintf("%s@%s", name, host);
}

/* An explicit --committer overrides the author-derived committer identity. */
bool dolt_transaction_commit = false;
bool dolt_multi_branch_commit = false;
bool dolt_force_transaction_commit = false;
bool dolt_allow_commit_conflicts = false;
char *dolt_transaction_commit_message;
char *dolt_author_name;
char *dolt_author_email;
char *dolt_author_date;
char *dolt_committer_name;
char *dolt_committer_email;
char *dolt_committer_date;
static bool commit_author_date_explicit;

static bool
setting_present(const char *value)
{
	return value && *value;
}

static TimestampTz
commit_date_setting(const char *value)
{
	return DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in,
		CStringGetDatum(value), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));
}

static char *commit_committer_name;
static char *commit_committer_email;

static bytea *
commit_object(bytea *database, bytea *parent, bytea *second, const char *message, const char *author, const char *author_email, TimestampTz author_date)
{
	StringInfoData json;
	const char *name = GetUserNameFromId(GetUserId(), false);
	char	   *email = default_email(name);
	int64		height = 1;

	TimestampTz committer_date = GetCurrentTimestamp();
	bool explicit_author = author != NULL;
	bool committer_set = (setting_present(dolt_committer_name) || setting_present(dolt_committer_email)) &&
		(strcmp(setting_present(dolt_committer_name) ? dolt_committer_name : name,
				setting_present(dolt_author_name) ? dolt_author_name : name) ||
		 strcmp(setting_present(dolt_committer_email) ? dolt_committer_email : email,
				setting_present(dolt_author_email) ? dolt_author_email : email));

	if (!author)
	{
		author = setting_present(dolt_author_name) ? dolt_author_name : name;
		author_email = setting_present(dolt_author_email) ? dolt_author_email : email;
	}
	if (setting_present(dolt_committer_name))
		name = dolt_committer_name;
	if (setting_present(dolt_committer_email))
		email = dolt_committer_email;
	/* Explicit author historically supplies the committer unless overridden. */
	if (commit_committer_name)
	{
		name = commit_committer_name;
		email = commit_committer_email;
	}
	else if (explicit_author && !committer_set)
	{
		name = author;
		email = (char *) author_email;
	}
	if (!commit_author_date_explicit && setting_present(dolt_author_date))
		author_date = commit_date_setting(dolt_author_date);
	if (setting_present(dolt_committer_date))
		committer_date = commit_date_setting(dolt_committer_date);
	else if (commit_author_date_explicit)
		committer_date = author_date;

	if (parent)
	{
		char	   *meta = metadata(parent, "commit");
		char	   *h = json_value(meta, "height");

		if (!h || strcmp(json_value(meta, "format"), "DGCOMMIT1"))
			unsupported("commit metadata from an incompatible internal format");
		height = pg_strtoint64(h) + 1;
	}
	if (second)
		height = Max(height, pg_strtoint64(json_value(metadata(second, "commit"), "height")) + 1);
	initStringInfo(&json);
	appendStringInfoChar(&json, '{');
	json_field(&json, "format", "DGCOMMIT1", false);
	json_field(&json, "committer", name, true);
	json_field(&json, "email", email, true);
	json_field(&json, "author", author ? author : name, true);
	json_field(&json, "author_email", author_email ? author_email : email, true);
	json_field(&json, "message", message, true);
	appendStringInfo(&json, ",\"date_us\":%lld,\"author_date_us\":%lld,\"height\":%lld}", (long long) committer_date, (long long) author_date, (long long) height);
	return put("commit", json.data, VC_ARGS(PointerGetDatum(database), PointerGetDatum(parent), PointerGetDatum(second)), second ? 3 : parent ? 2 : 1);
}
static void
ensure_repository(void)
{
	VersionRef	head = read_ref("heads/main");
	bytea	   *db,
			   *commit;

	if (head.target)
		return;

	/*
	 * Initialization is lazy. Only the default public namespace belongs to
	 * the initial baseline: user-created namespaces must remain working-set
	 * changes even when they predate the first version-control command.
	 */
	run("SELECT definition FROM (" NAMESPACE_DEFINITIONS_SQL ") namespaces(definition) WHERE definition::jsonb->>'name'='public'", 0, NULL, NULL, NULL, true);
	{
		int			n = SPI_processed,
					i;
		char	  **definitions = palloc(sizeof(char *) * Max(n, 1));
		Datum	   *members = palloc(sizeof(Datum) * Max(n, 1));
		StringInfoData json;

		for (i = 0; i < n; i++)
			definitions[i] = strcell(i, 1);
		initStringInfo(&json);
		appendStringInfoString(&json, "{\"format\":\"DGDB1\",\"members\":[");
		for (i = 0; i < n; i++)
		{
			appendStringInfo(&json, "%s{\"kind\":\"namespace\",\"definition\":%s}", i ? "," : "", definitions[i]);
			members[i] = PointerGetDatum(put("namespace", definitions[i], NULL, 0));
		}
		appendStringInfoString(&json, "]}");
		run("SELECT jsonb_build_object('format','DGDB1','encoding',current_setting('server_encoding'),'members',COALESCE(jsonb_agg(v ORDER BY v->>'kind' COLLATE \"C\",COALESCE(v->>'name',v->'definition'->>'name') COLLATE \"C\"),'[]'::jsonb))::text FROM jsonb_array_elements($1::jsonb->'members') v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json.data)), NULL, true);
		db = canonical_database(strcell(0, 1), members, n);
	}
	commit = commit_object(db, NULL, NULL, "Initialize data repository", NULL, NULL, GetCurrentTimestamp());
	move_ref("heads/main", head, commit);
	move_ref("staged/main", read_ref("staged/main"), db);
	move_ref("working/main", read_ref("working/main"), db);
	CommandCounterIncrement();
	/* Our command's active snapshot must see repository initialization. */
	UpdateActiveSnapshotCommandId();
}
static void
valid_name(const char *s)
{
	const char *p;

	if (!*s || strlen(s) > 200 || !pg_strcasecmp(s, "HEAD") || !pg_strcasecmp(s, "WORKING") || !pg_strcasecmp(s, "STAGED") || strstr(s, "..") || strstr(s, "@{") || s[0] == '-' || s[0] == '/' || s[strlen(s) - 1] == '/' || s[strlen(s) - 1] == '.' || strstr(s, "//") || (strlen(s) >= 5 && !strcmp(s + strlen(s) - 5, ".lock")))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid branch or tag name: %s", s)));
	for (p = s; *p; p++)
		if ((unsigned char) *p <= 32 || (unsigned char) *p == 127 || strchr("~^:?*[\\", *p))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid branch or tag name: %s", s)));
}
static int
parent_count(bytea *commit)
{
	ArrayType  *children;
	int			n;

	if (!object_read(commit, "commit", NULL, NULL, &children))
		return 0;
	n = ArrayGetNItems(ARR_NDIM(children), ARR_DIMS(children));
	return Max(n, 1) - 1;
}

/* Resolve only retained commits: ordinary names also use base-32 letters. */
static bytea *
commit_prefix(const char *name)
{
	bytea	   *hash = NULL;
	Oid			user;
	int			security,
				nest;

	if (strlen(name) < 8 || strlen(name) > PL_HASH_TEXT_SIZE ||
		strspn(name, "0123456789abcdefghijklmnopqrstuv") != strlen(name))
		return NULL;

	/* An abbreviated hash is searched by SQL, in its own scope. */
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		nest = begin_api();
		run("SELECT objhash FROM pg_dolt_object_visible WHERE objkind='commit' AND pg_catalog.dolt_hash_encode(objhash) LIKE $1 LIMIT 2", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(psprintf("%s%%", name))), NULL, true);
		if (SPI_processed > 1)
			ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_PARAMETER), errmsg("ambiguous commit prefix: %s", name)));
		if (SPI_processed)
			hash = DatumGetByteaPCopy(cell(0, 1));
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
	}
	PG_END_TRY();
	return hash;
}

/* The ref the last resolve() went through, if any, and what it pointed to. */
static char *resolved_ref;
static bytea *resolved_target;
static bool resolved_session;	/* HEAD: relative to the session's branch */

static bytea *
resolve(const char *revision)
{
	char	   *base = pstrdup(revision),
			   *suffix = strpbrk(base, "~^");
	bytea	   *hash = NULL;
	VersionRef	ref;

	resolved_ref = NULL;
	resolved_target = NULL;
	resolved_session = false;
	if (suffix)
		*suffix++ = '\0';
	if (!pg_strcasecmp(base, "HEAD"))
	{
		resolved_ref = branch_ref("heads");
		resolved_session = true;
		hash = resolved_target = read_ref(resolved_ref).target;
	}
	else
	{
		resolved_ref = psprintf("heads/%s", base);
		ref = read_ref(resolved_ref);
		hash = resolved_target = ref.target;
		if (!hash)
		{
			resolved_ref = psprintf("tags/%s", base);
			ref = read_ref(resolved_ref);
			hash = resolved_target = ref.target;
			if (hash)
			{
				char	   *kind;

				if (object_read(hash, NULL, &kind, NULL, NULL) && !strcmp(kind, "tag"))
					hash = child(hash, 1);
			}
		}
		if (!hash)
			resolved_ref = NULL;
		if (!hash && strlen(base) == PL_HASH_TEXT_SIZE)
			hash = hash_parse(base);
		else if (!hash)
			hash = commit_prefix(base);
	}
	if (!hash)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("unknown revision: %s", revision)));
	metadata(hash, "commit");
	if (suffix)
	{
		const char *orig = strpbrk(revision, "~^");
		int			n = *suffix ? pg_strtoint32(suffix) : 1,
					i;

		if (n < 0 || n > 100000)
			unsupported("ancestor count outside supported range");
		if (*orig == '^')
		{
			if (n && n > parent_count(hash))
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid ancestor spec: %s", revision)));
			if (n)
				hash = child(hash, n + 1);
		}
		else
			for (i = 0; i < n; i++)
			{
				CHECK_FOR_INTERRUPTS();
				if (parent_count(hash) < 1)
					ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid ancestor spec: %s", revision)));
				hash = child(hash, 2);
			}
	}
	return hash;
}
static void
author_option(char *value, char **name, char **email)
{
	char	   *lt = strrchr(value, '<'),
			   *gt = strrchr(value, '>');

	if (!lt || !gt || gt[1] || gt < lt + 2 || lt == value)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("author must be Name <email>")));
	*name = pnstrdup(value, lt - value);
	while (**name && (*name)[strlen(*name) - 1] == ' ')
		(*name)[strlen(*name) - 1] = '\0';
	*email = pnstrdup(lt + 1, gt - lt - 1);
}

/* Initialize the version-control repository of the current database. initdb
 * runs this for template1, so every database starts with a main branch. */
Datum
dolt_init(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();

	ensure_repository();
	end_api(nest);
	PG_RETURN_INT64(0);
}

Datum
dolt_add(PG_FUNCTION_ARGS)
{
	int			n;
	char	  **args = arguments(fcinfo, &n);
	Capture		current;
	VersionRef	staged,
				working;
	int			nest;

	standalone(fcinfo, false);
	nest = begin_api();
	/* Activation advances command visibility on our private API snapshot. */
	if (!read_ref("views/enabled").target)
		catalog_activate();
	ensure_repository();
	if (n == 0)
		unsupported("dolt_add requires '.', '-A' or table names");
	if (n != 1 || (strcmp(args[0], ".") && strcmp(args[0], "-a") && strcmp(args[0], "-A") && strcmp(args[0], "--all")))
	{
		/* Stage only the named tables. */
		List	   *tables = NIL;
		int			i;
		char	   *stagedjson = staged_state(),
				   *workingjson = working_state();

		for (i = 0; i < n; i++)
		{
			if (args[i][0] == '-')
				unsupported(args[i]);
			tables = lappend(tables, resolve_object_argument(args[i], stagedjson, workingjson, true));
		}
		stage_tables(tables);
		end_api(nest);
		PG_RETURN_INT64(0);
	}
	staged = read_ref(branch_ref("staged"));
	working = read_ref(branch_ref("working"));
	move_ref(branch_ref("staged"), staged, staged_capture(&current));
	move_ref(branch_ref("working"), working, current.hash);
	end_api(nest);
	PG_RETURN_INT64(0);
}
Datum
dolt_commit(PG_FUNCTION_ARGS)
{
	int			n,
				i;
	char	  **args = arguments(fcinfo, &n),
			   *message = NULL,
			   *author = NULL,
			   *email = NULL,
			   *committer = NULL,
			   *committer_email = NULL;
	bool		date_explicit = false,
				all = false,
				modified = false,
				allow_empty = false,
				skip_empty = false;
	TimestampTz author_date = GetCurrentTimestamp();
	VersionRef	head,
				staged,
				working;
	Capture		full;
	bytea	   *database,
			   *parentdb,
			   *commit;
	char	   *answer;
	int			nest;

	standalone(fcinfo, true);
	nest = begin_api();
	/* Activation advances command visibility on our private API snapshot. */
	if (!read_ref("views/enabled").target)
		catalog_activate();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "-A") || !strcmp(args[i], "--ALL"))
			all = true;
		else if (!strcmp(args[i], "-a") || !strcmp(args[i], "--all"))
			modified = true;
		else if (!strcmp(args[i], "--allow-empty"))
			allow_empty = true;
		else if (!strcmp(args[i], "--skip-empty"))
			skip_empty = true;
		else if (!strcmp(args[i], "-m") || !strcmp(args[i], "--message") || (!strcmp(args[i], "-Am") || !strcmp(args[i], "-am")))
		{
			if (!strcmp(args[i], "-Am"))
				all = true;
			else if (!strcmp(args[i], "-am"))
				modified = true;
			if (++i == n)
				unsupported("missing commit message");
			message = args[i];
		}
		else if (!strcmp(args[i], "--author"))
		{
			if (++i == n)
				unsupported("missing author");
			author_option(args[i], &author, &email);
		}
		else if (!strcmp(args[i], "--committer"))
		{
			if (++i == n)
				unsupported("missing committer");
			author_option(args[i], &committer, &committer_email);
		}
		else if (!strcmp(args[i], "--date"))
		{
			if (++i == n)
				unsupported("missing date");
			author_date = commit_date_setting(args[i]);
			date_explicit = true;
		}
		else
			unsupported(args[i]);
	}
	if (!message || !*message)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("commit message is required (-m)")));
	if (allow_empty && skip_empty)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("--allow-empty and --skip-empty are incompatible")));
	ensure_repository();
	if (modified && !all)
		stage_modified_tables();
	head = read_ref(branch_ref("heads"));
	staged = read_ref(branch_ref("staged"));
	working = read_ref(branch_ref("working"));
	parentdb = child(head.target, 1);
	database = all ? staged_capture(&full) : staged.target;
	if (!database)
		database = parentdb;
	if (database_same(database, parentdb) && !allow_empty && !merge_parent())
	{
		if (skip_empty)
		{
			end_api(nest);
			PG_RETURN_NULL();
		}
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("nothing to commit")));
	}
	commit_committer_name = committer;
	commit_committer_email = committer_email;
	commit_author_date_explicit = date_explicit;
	PG_TRY();
	{
		commit = commit_object(database, head.target, merge_parent(), message, author, email, author_date);
	}
	PG_FINALLY();
	{
		commit_committer_name = NULL;
		commit_committer_email = NULL;
		commit_author_date_explicit = false;
	}
	PG_END_TRY();
	move_ref(branch_ref("heads"), head, commit);
	move_ref(branch_ref("staged"), staged, database);
	if (all)
		move_ref(branch_ref("working"), working, full.hash);
	finish_merge();
	answer = hash_format(commit);
	end_api(nest);
	PG_RETURN_TEXT_P(cstring_to_text(answer));
}
Datum
dolt_branch(PG_FUNCTION_ARGS)
{
	int			n;
	char	  **args = arguments(fcinfo, &n);
	VersionRef	ref;
	bytea	   *commit;
	int			nest;

	standalone(fcinfo, true);
	nest = begin_api();
	if (branch_management(args, n))
	{
		end_api(nest);
		PG_RETURN_INT64(0);
	}
	if (n < 1 || n > 2 || args[0][0] == '-')
		unsupported("dolt_branch supports creation: name [start-point]");
	valid_name(args[0]);
	ensure_repository();
	commit = resolve(n == 2 ? args[1] : "HEAD");
	ref = read_ref(psprintf("heads/%s", args[0]));
	if (ref.target)
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("branch already exists: %s", args[0])));
	move_ref(psprintf("heads/%s", args[0]), ref, commit);
	move_ref(psprintf("working/%s", args[0]), empty_ref, child(commit, 1));
	move_ref(psprintf("staged/%s", args[0]), empty_ref, child(commit, 1));
	replace_working(args[0], child(commit, 1), NIL);
	end_api(nest);
	PG_RETURN_INT64(0);
}
Datum
dolt_tag(PG_FUNCTION_ARGS)
{
	int			n,
				i,
				pos = 0;
	char	  **args = arguments(fcinfo, &n),
			   *names[2],
			   *message = "",
			   *author = NULL,
			   *email = NULL;
	bytea	   *commit,
			   *tag;
	VersionRef	ref;
	StringInfoData json;
	int			nest;

	standalone(fcinfo, true);
	nest = begin_api();
	if (n == 2 && (!strcmp(args[0], "-d") || !strcmp(args[0], "--delete")))
	{
		VersionRef	existing = read_ref(psprintf("tags/%s", args[1]));

		if (!existing.target)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("tag not found: %s", args[1])));
		move_ref(psprintf("tags/%s", args[1]), existing, NULL);
		end_api(nest);
		PG_RETURN_INT64(0);
	}
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "-m") || !strcmp(args[i], "--message"))
		{
			if (++i == n)
				unsupported("missing tag message");
			message = args[i];
		}
		else if (!strcmp(args[i], "--author"))
		{
			if (++i == n)
				unsupported("missing tag author");
			author_option(args[i], &author, &email);
		}
		else if (args[i][0] == '-' || pos == 2)
			unsupported(args[i]);
		else
			names[pos++] = args[i];
	}
	if (pos == 0)
		unsupported("tag listing: query dolt_tags instead");
	valid_name(names[0]);
	ensure_repository();
	commit = resolve(pos == 2 ? names[1] : "HEAD");
	ref = read_ref(psprintf("tags/%s", names[0]));
	if (ref.target)
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("tag already exists: %s", names[0])));
	if (!author)
	{
		author = GetUserNameFromId(GetUserId(), false);
		email = default_email(author);
	}
	initStringInfo(&json);
	appendStringInfoChar(&json, '{');
	json_field(&json, "format", "DGTAG1", false);
	json_field(&json, "tagger", author, true);
	json_field(&json, "email", email, true);
	json_field(&json, "message", message, true);
	appendStringInfo(&json, ",\"date_us\":%lld}", (long long) GetCurrentTimestamp());
	tag = put("tag", json.data, VC_ARGS(PointerGetDatum(commit)), 1);
	move_ref(psprintf("tags/%s", names[0]), ref, tag);
	end_api(nest);
	PG_RETURN_INT64(0);
}
static void
return_rows(FunctionCallInfo fcinfo, int nest)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	uint64		r;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	for (r = 0; r < SPI_processed; r++)
	{
		Datum		values[16];
		bool		nulls[16];
		int			c;

		for (c = 0; c < SPI_tuptable->tupdesc->natts; c++)
			values[c] = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, c + 1, &nulls[c]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	end_api(nest);
}
Datum
dolt_log(PG_FUNCTION_ARGS)
{
	int			n,
				i,
				limit = -1;
	char	  **args = arguments(fcinfo, &n),
			   *revision = "HEAD";
	bool		parents = false,
				signature = false,
				revset = false;
	bytea	   *hash;
	int			nest = begin_api();

	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--parents"))
			parents = true;
		else if (!strcmp(args[i], "--show-signature"))
			signature = true;
		else if (!strcmp(args[i], "-n") || !strcmp(args[i], "--max-count"))
		{
			if (++i == n)
				unsupported("missing log count");
			limit = pg_strtoint32(args[i]);
			if (limit < 0)
				unsupported("negative log count");
		}
		else if (args[i][0] == '-' || revset)
			unsupported(args[i]);
		else
		{
			revision = args[i];
			revset = true;
		}
	}
	if (!read_ref(branch_ref("heads")).target && !strcmp(revision, "HEAD"))
	{
		run("SELECT NULL::text WHERE false", 0, NULL, NULL, NULL, true);
		return_rows(fcinfo, nest);
		return (Datum) 0;
	}
	hash = resolve(revision);
	run("WITH RECURSIVE walk(h) AS (SELECT $1::bytea UNION SELECT unnest(o.objchildren[2:]) FROM walk w JOIN pg_dolt_object_visible o ON o.objhash=w.h), commits AS (SELECT o.*,convert_from(o.objpayload,'UTF8')::jsonb m FROM walk w JOIN pg_dolt_object_visible o ON o.objhash=w.h WHERE o.objkind='commit'), refs AS (SELECT r.refname,CASE WHEN o.objkind='tag' THEN o.objchildren[1] ELSE r.reftarget END target FROM pg_dolt_ref r JOIN pg_dolt_object_visible o ON o.objhash=r.reftarget WHERE r.refname LIKE 'heads/%' OR r.refname LIKE 'tags/%') SELECT pg_catalog.dolt_hash_encode(c.objhash),m->>'committer',m->>'email',timestamp '2000-01-01' + (m->>'date_us')::bigint * interval '1 microsecond',m->>'message',(m->>'height')::bigint,CASE WHEN $2 THEN COALESCE((SELECT string_agg(pg_catalog.dolt_hash_encode(h),', ' ORDER BY ord) FROM unnest(c.objchildren[2:]) WITH ORDINALITY u(h,ord)),'') ELSE NULL END,COALESCE((SELECT CASE WHEN c.objhash=(SELECT reftarget FROM pg_dolt_ref WHERE refname='heads/' || pg_catalog.active_branch()) THEN 'HEAD -> ' ELSE '' END || string_agg(CASE WHEN r.refname LIKE 'tags/%' THEN 'tag: '||substr(r.refname,6) ELSE substr(r.refname,7) END,', ' ORDER BY r.refname COLLATE \"C\") FROM refs r WHERE r.target=c.objhash),''),CASE WHEN $3 THEN ''::text ELSE NULL END,m->>'author',m->>'author_email',timestamp '2000-01-01' + (m->>'author_date_us')::bigint * interval '1 microsecond' FROM commits c ORDER BY (m->>'height')::bigint DESC,(m->>'date_us')::bigint DESC,c.objhash LIMIT NULLIF($4,-1)", 4, VC_TYPES(BYTEAOID, BOOLOID, BOOLOID, INT4OID), VC_ARGS(PointerGetDatum(hash), BoolGetDatum(parents), BoolGetDatum(signature), Int32GetDatum(limit)), NULL, true);
	return_rows(fcinfo, nest);
	return (Datum) 0;
}
static char *
state_json(const char *revision)
{
	if (!pg_strcasecmp(revision, "WORKING"))
		return capture(false).json;
	if (!pg_strcasecmp(revision, "STAGED"))
	{
		VersionRef	r = read_ref(branch_ref("staged"));

		if (!r.target)
			return "{\"members\":[]}";
		return metadata(r.target, "database");
	}
	return metadata(child(resolve(revision), 1), "database");
}
Datum
dolt_diff_summary(PG_FUNCTION_ARGS)
{
	char	   *from,
			   *to,
			   *table = NULL;
	int			nest = begin_api();

	from = state_json(text_to_cstring(PG_GETARG_TEXT_PP(0)));
	to = !strcmp(text_to_cstring(PG_GETARG_TEXT_PP(0)), text_to_cstring(PG_GETARG_TEXT_PP(1))) ? from : state_json(text_to_cstring(PG_GETARG_TEXT_PP(1)));
	if (PG_NARGS() > 2)
		table = text_to_cstring(PG_GETARG_TEXT_PP(2));
	if (table)
	{
		/* As in Dolt, a name that names nothing has no changes. */
		table = resolve_object_argument(table, from, to, false);
		if (!table)
			table = "";
	}

	run("WITH a AS (SELECT v FROM jsonb_array_elements($1::jsonb->'members') v WHERE v->>'kind' IN ('table','routine','view','type','trigger','sequence','aggregate','operator','cast')),b AS (SELECT v FROM jsonb_array_elements($2::jsonb->'members') v WHERE v->>'kind' IN ('table','routine','view','type','trigger','sequence','aggregate','operator','cast')),d AS (SELECT a.v av,b.v bv FROM a FULL JOIN b ON a.v->>'name'=b.v->>'name' AND a.v->>'kind'=b.v->>'kind') SELECT coalesce(av->>'name',''),coalesce(bv->>'name',''),CASE WHEN av IS NULL THEN 'added' WHEN bv IS NULL THEN 'dropped' ELSE 'modified' END,CASE WHEN coalesce(av->>'kind',bv->>'kind')='table' THEN (av->>'content') IS DISTINCT FROM (bv->>'content') ELSE (av->>'schema') IS DISTINCT FROM (bv->>'schema') END,CASE WHEN coalesce(av->>'kind',bv->>'kind')='table' THEN (av->>'schema') IS DISTINCT FROM (bv->>'schema') ELSE av IS NULL OR bv IS NULL END FROM d WHERE ((av->>'content') IS DISTINCT FROM (bv->>'content') OR (av->>'schema') IS DISTINCT FROM (bv->>'schema')) AND ($3::text IS NULL OR coalesce(av->>'name',bv->>'name')=$3) ORDER BY coalesce(av->>'name',bv->>'name') COLLATE \"C\"", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(from), CStringGetTextDatum(to), table ? CStringGetTextDatum(table) : (Datum) 0), table ? NULL : "  n", true);
	if (table && SPI_processed > 1)
		ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_ALIAS), errmsg("ambiguous table name: %s", table)));
	return_rows(fcinfo, nest);
	return (Datum) 0;
}

/* Distinct fmgr entries keep overload arity explicit in the built-in catalog. */

Datum
dolt_add_noargs(PG_FUNCTION_ARGS)
{
	return dolt_add(fcinfo);
}

Datum
dolt_commit_noargs(PG_FUNCTION_ARGS)
{
	return dolt_commit(fcinfo);
}

Datum
dolt_branch_noargs(PG_FUNCTION_ARGS)
{
	return dolt_branch(fcinfo);
}

Datum
dolt_tag_noargs(PG_FUNCTION_ARGS)
{
	return dolt_tag(fcinfo);
}

Datum
dolt_log_noargs(PG_FUNCTION_ARGS)
{
	return dolt_log(fcinfo);
}

Datum
dolt_diff_summary_table(PG_FUNCTION_ARGS)
{
	return dolt_diff_summary(fcinfo);
}

#include "dg_binding.inc"

static void validate_working_foreign_keys(const char *branch, StringInfo violations);

#include "dg_merge.inc"



#include "dg_provider.inc"
#include "dg_catalog.inc"
#include "dg_foreign_validate.inc"

#include "dg_catalog_ddl.inc"
#include "dg_canonical_ddl.inc"
#include "dg_routine_ddl.inc"
#include "dg_schema_object_ddl.inc"
#include "dg_sequence_ddl.inc"

#include "dg_alter.inc"
#include "dg_schema_merge.inc"
#include "dg_dependency.inc"
#include "dg_trigger_ddl.inc"
#include "dg_type_ddl.inc"
#include "dg_view_ddl.inc"
#include "dg_rename.inc"
#include "dg_tables.inc"
#include "dg_diff.inc"
#include "dg_patch.inc"
#include "dg_workspace.inc"
#include "dg_conflicts.inc"
#include "dg_schemas.inc"
#include "dg_rebase.inc"
#include "dg_tools.inc"
#include "dg_transport.inc"
#include "dg_compat.inc"

#include "dg_commit_session.inc"
