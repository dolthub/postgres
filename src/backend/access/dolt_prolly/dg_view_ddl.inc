/*
 * Versioned views. A view is a schema member of its own: its query as SQL with
 * every name qualified, its columns by name and portable type, its options,
 * and the objects its query depends on, by name. The provider rebuilds the
 * view's catalog rows from these; its rewrite rule holds the SQL, which is
 * analyzed when the rule is first read, in the complete catalog view of the
 * view's revision, as SQL routine bodies are.
 */

static int
view_entry_compare(const ListCell *a, const ListCell *b)
{
	return strcmp(strVal(lfirst(a)), strVal(lfirst(b)));
}

/* The rewrite rule of view |relid|, in the current catalog view. */
static HeapTuple
view_rule(Oid relid)
{
	Relation	catalog = table_open(RewriteRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tuple,
				result = NULL;

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		if (((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class == relid &&
			!strcmp(NameStr(((Form_pg_rewrite) GETSTRUCT(tuple))->rulename), ViewSelectRuleName))
			result = heap_copytuple(tuple);
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	if (!result)
		elog(ERROR, "view %u has no rule", relid);
	return result;
}

/* A dependency's object by portable name, or NULL for a built-in one. */
static char *
view_dependency_name(Oid classid, Oid objid)
{
	if (objid < FirstNormalObjectId)
		return NULL;
	if (classid == RelationRelationId)
		return dg_qualified_name(get_namespace_name(get_rel_namespace(objid)), get_rel_name(objid));
	if (classid == ProcedureRelationId)
	{
		if (objid < FirstDurableObjectId)
			unsupported("versioned view calling an unversioned function");
		return pstrdup(CatalogViewObjectIdentity(objid, ProcedureRelationId));
	}
	if (classid == TypeRelationId)
		return routine_type_name(objid);
	if (classid == OperatorRelationId && CatalogViewObjectContext(objid))
		return schema_operator_name(objid);
	unsupported("versioned view depending on this kind of object");
	pg_unreachable();
}

/* The SQL of view or materialized view |relid|'s query, every name qualified. */
static char *
view_query(Oid relid, HeapTuple rule)
{
	char	   *sql = NULL;
	int			nest = NewGUCNestLevel();

	PG_TRY();
	{
		/* As pg_get_viewdef, from the rule itself: SQL cannot read a provisional view. */
		bool		isnull;
		Relation	rules = table_open(RewriteRelationId, AccessShareLock);
		List	   *actions = (List *) stringToNode(TextDatumGetCString(heap_getattr(rule, Anum_pg_rewrite_ev_action,
																						RelationGetDescr(rules), &isnull)));

		table_close(rules, AccessShareLock);
		actions = dg_rule_actions(relid, actions);
		schema_expression_gucs();
		sql = pg_get_querydef(linitial_node(Query, actions), false);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	return sql;
}

/* As rule |ruleid|'s native dependencies, by name: [kind, name, column], as JSON. */
static char *
view_dependencies(Oid relid, Oid ruleid)
{
	Relation	catalog = table_open(DependRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tuple;
	List	   *dependencies = NIL;
	StringInfoData json;

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tuple);
		char	   *name;
		StringInfoData entry;

		if (dep->classid != RewriteRelationId || dep->objid != ruleid || dep->refobjid == relid ||
			!(name = view_dependency_name(dep->refclassid, dep->refobjid)))
			continue;
		initStringInfo(&entry);
		appendStringInfoChar(&entry, '[');
		escape_json(&entry, dep->refclassid == RelationRelationId ? "table" : dep->refclassid == ProcedureRelationId ? "routine" : dep->refclassid == OperatorRelationId ? "operator" : "type");
		appendStringInfoChar(&entry, ',');
		escape_json(&entry, name);
		appendStringInfoChar(&entry, ',');
		if (dep->refclassid == RelationRelationId && dep->refobjsubid > 0)
			escape_json(&entry, get_attname(dep->refobjid, dep->refobjsubid, false));
		else
			appendStringInfoString(&entry, "null");
		appendStringInfoChar(&entry, ']');
		if (!list_member(dependencies, makeString(entry.data)))
			dependencies = lappend(dependencies, makeString(entry.data));
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	list_sort(dependencies, view_entry_compare);
	initStringInfo(&json);
	appendStringInfoChar(&json, '[');
	foreach_node(String, entry, dependencies)
		appendStringInfo(&json, "%s%s", foreach_current_index(entry) ? "," : "", strVal(entry));
	appendStringInfoChar(&json, ']');
	return json.data;
}

static char *
view_encode(Oid relid)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);
	HeapTuple	rule = view_rule(relid);
	StringInfoData json;

	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGVIEW1\",");
	json_field(&json, "name", dg_qualified_name(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel)), false);
	appendStringInfoChar(&json, ',');
	json_field(&json, "owner", GetUserNameFromId(rel->rd_rel->relowner, false), false);
	appendStringInfoChar(&json, ',');
	json_field(&json, "query", view_query(relid, rule), false);
	appendStringInfoString(&json, ",\"options\":[");
	{
		bool		isnull;
		HeapTuple	klass = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		Datum		options = SysCacheGetAttr(RELOID, klass, Anum_pg_class_reloptions, &isnull);

		if (!isnull)
		{
			Datum	   *items;
			int			n;

			deconstruct_array_builtin(DatumGetArrayTypeP(options), TEXTOID, &items, NULL, &n);
			for (int i = 0; i < n; i++)
			{
				if (i)
					appendStringInfoChar(&json, ',');
				escape_json(&json, TextDatumGetCString(items[i]));
			}
		}
		ReleaseSysCache(klass);
	}
	appendStringInfoString(&json, "],\"columns\":[");
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (i)
			appendStringInfoChar(&json, ',');
		appendStringInfoChar(&json, '{');
		json_field(&json, "name", NameStr(attr->attname), false);
		appendStringInfoChar(&json, ',');
		json_field(&json, "type", routine_type_name(attr->atttypid), false);
		appendStringInfo(&json, ",\"typmod\":%d", attr->atttypmod);
		if (attr->attcollation != get_typcollation(attr->atttypid))
		{
			appendStringInfoChar(&json, ',');
			json_field(&json, "collation", type_collation_name(attr->attcollation), false);
		}
		appendStringInfoChar(&json, '}');
	}
	appendStringInfo(&json, "],\"dependencies\":%s", view_dependencies(relid, ((Form_pg_rewrite) GETSTRUCT(rule))->oid));
	json_comment(&json, relid, RelationRelationId);
	appendStringInfoChar(&json, '}');
	relation_close(rel, AccessShareLock);
	return json.data;
}

/*
 * A materialized view's "matview" definition, encoded again from its rule in
 * the current catalog view; |old| gives whether it is populated.
 */
static char *
matview_encode(Oid relid, const char *old)
{
	HeapTuple	rule = view_rule(relid);
	StringInfoData json;

	initStringInfo(&json);
	appendStringInfoChar(&json, '{');
	json_field(&json, "query", view_query(relid, rule), false);
	appendStringInfo(&json, ",\"populated\":%s,\"dependencies\":%s}", json_value(old, "populated") ? json_value(old, "populated") : "false",
					 view_dependencies(relid, ((Form_pg_rewrite) GETSTRUCT(rule))->oid));
	return json.data;
}

/*
 * Analyze a versioned view's rule, which holds the view's SQL, in its
 * relation's catalog view. Other rules are returned as they are.
 */
List *
dg_rule_actions(Oid relid, List *actions)
{
	char	   *sql;
	List	   *parsed;
	Query	   *query;
	int			nest;
	CatalogView *previous;

	if (list_length(actions) != 1 || !IsA(linitial(actions), String))
		return actions;

	/*
	 * Names cannot be bound within a version-control API scope, as when the
	 * relation is opened there: the rule's user analyzes it later.
	 */
	if (catalog_api_depth)
		return actions;
	sql = strVal(linitial(actions));
	nest = NewGUCNestLevel();
	previous = CatalogViewSwitch(CatalogViewObjectContext(relid) ? CatalogViewObjectContext(relid) : CatalogViewCurrent());
	/* Names resolve against the latest catalog state, as for any query. */
	PushActiveSnapshot(GetLatestSnapshot());
	PG_TRY();
	{
		schema_expression_gucs();
		parsed = raw_parser(sql, RAW_PARSE_DEFAULT);
		if (list_length(parsed) != 1)
			elog(ERROR, "invalid versioned view query");
		query = parse_analyze_fixedparams(linitial_node(RawStmt, parsed), sql, NULL, 0, NULL);
		AcquireRewriteLocks(query, false, false);
	}
	PG_FINALLY();
	{
		PopActiveSnapshot();
		CatalogViewSwitch(previous);
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	return list_make1(query);
}

/*
 * Store the analyzed rules of the views and materialized views among
 * |targets|, (kind, name) pairs, and the analyzed bodies of their SQL-standard
 * routines, in mutable provisional view |view|, so that native DDL that
 * renames what they refer to changes their queries as it changes any other
 * definition.
 */
static void
materialize_queries(CatalogView *view, List *targets)
{
	CatalogView *previous = CatalogViewSwitch(view);
	Relation	catalog = table_open(RewriteRelationId, RowExclusiveLock);
	Relation	routines = table_open(ProcedureRelationId, RowExclusiveLock);

	foreach_ptr(List, target, targets)
	{
		Oid			relid;
		HeapTuple	rule,
					updated;
		bool		isnull;
		Datum		values[Max(Natts_pg_rewrite, Natts_pg_proc)] = {0};
		bool		nulls[Max(Natts_pg_rewrite, Natts_pg_proc)] = {0};
		bool		replace[Max(Natts_pg_rewrite, Natts_pg_proc)] = {0};
		List	   *actions;

		if (!strcmp(linitial(target), "routine"))
		{
			HeapTuple	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(CatalogViewLookupObject(view, ProcedureRelationId, lsecond(target))));
			Datum		body = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prosqlbody, &isnull);

			if (!isnull)
			{
				values[Anum_pg_proc_prosqlbody - 1] = CStringGetTextDatum(nodeToString(dg_routine_body(tuple, stringToNode(TextDatumGetCString(body)))));
				replace[Anum_pg_proc_prosqlbody - 1] = true;
				updated = heap_modify_tuple(tuple, RelationGetDescr(routines), values, nulls, replace);
				CatalogTupleUpdate(routines, &tuple->t_self, updated);
			}
			ReleaseSysCache(tuple);
			continue;
		}
		if (strcmp(linitial(target), "view") && strcmp(linitial(target), "table"))
			continue;
		relid = CatalogViewLookupObject(view, RelationRelationId, lsecond(target));
		if (!strcmp(linitial(target), "table") && get_rel_relkind(relid) != RELKIND_MATVIEW)
			continue;
		rule = view_rule(relid);
		actions = (List *) stringToNode(TextDatumGetCString(heap_getattr(rule, Anum_pg_rewrite_ev_action, RelationGetDescr(catalog), &isnull)));
		values[Anum_pg_rewrite_ev_action - 1] = CStringGetTextDatum(nodeToString(dg_rule_actions(relid, actions)));
		replace[Anum_pg_rewrite_ev_action - 1] = true;
		updated = heap_modify_tuple(rule, RelationGetDescr(catalog), values, nulls, replace);
		CatalogTupleUpdate(catalog, &rule->t_self, updated);
	}
	table_close(routines, RowExclusiveLock);
	table_close(catalog, RowExclusiveLock);
	CommandCounterIncrement();
	CatalogViewSwitch(previous);
}

/* Whether CREATE VIEW makes a versioned view: a permanent view in a user schema. */
static bool
creates_versioned_view(ViewStmt *stmt)
{
	Oid			namespace;

	if (stmt->view->relpersistence == RELPERSISTENCE_TEMP)
		return false;
	namespace = RangeVarGetCreationNamespace(stmt->view);
	return !isAnyTempNamespace(namespace) && !dg_system_namespace(get_namespace_name(namespace));
}

static void
view_storage(Relation relation, CatalogViewStorageEffect effect)
{
	if (relation->rd_rel->relkind != RELKIND_VIEW)
		unsupported("storage effects of view DDL");
}

static void
view_create(ParseState *pstate, ViewStmt *stmt, int location, int length)
{
	CatalogView *view = CatalogViewCreate("provisional view DDL");
	CatalogView *previous = CatalogViewCurrent();
	CatalogView *saved_mutation = native_mutation_view;
	Oid			caller,
				privileged;
	int			security,
				nest;

	GetUserIdAndSecContext(&caller, &security);
	PG_TRY();
	{
		Capture		current;
		Oid			relid;
		char	   *definition;

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		current = capture(true);
		provider_populate(view, current.hash, NULL);
		end_api(nest);
		CatalogViewBeginMutation(view);
		CatalogViewSetStorageHandler(view, view_storage);
		native_mutation_view = view;
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		{
			/* CREATE OR REPLACE reads the old rule. */
			Oid			existing = RangeVarGetRelid(stmt->view, NoLock, true);

			if (OidIsValid(existing) && get_rel_relkind(existing) == RELKIND_VIEW)
				materialize_queries(view, list_make1(list_make2("view", CatalogViewObjectIdentity(existing, RelationRelationId))));
		}
		relid = DefineView(stmt, pstate->p_sourcetext, location, length).objectId;
		CommandCounterIncrement();
		GetUserIdAndSecContext(&privileged, &security);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		definition = view_encode(relid);
		CatalogViewSwitch(NULL);
		nest = begin_api();
		(void) objects_publish("view", list_make1(json_value(definition, "name")), NULL, false, list_make1(definition));
		end_api(nest);
		UpdateActiveSnapshotCommandId();
	}
	PG_FINALLY();
	{
		native_mutation_view = saved_mutation;
		SetUserIdAndSecContext(caller, security);
		CatalogViewSwitch(previous);
		if (CatalogViewIsProvisional(view))
			CatalogViewRetireMutation(view);
		CatalogViewRelease(view);
	}
	PG_END_TRY();
}

bool
dg_catalog_view_ddl(ParseState *pstate, Node *statement, int location, int length)
{
	if (!IsA(statement, ViewStmt) || !dg_catalog_active() || !creates_versioned_view((ViewStmt *) statement))
		return false;
	if (CatalogViewCurrent())
		unsupported("view DDL in an immutable revision context");

	/* Publication advances the command ID.  A SQL function's active snapshot
	 * can also be registered by its executor, so never modify that snapshot. */
	PushCopiedSnapshot(ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot());
	PG_TRY();
	{
		dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
		view_create(pstate, (ViewStmt *) statement, location, length);
	}
	PG_FINALLY();
	{
		PopActiveSnapshot();
	}
	PG_END_TRY();
	return true;
}

typedef struct QueryDependencies
{
	List	   *entries;		/* of String: [kind, name, null] */
} QueryDependencies;

static void
query_dependency(QueryDependencies *found, Oid classid, Oid objid)
{
	char	   *name = view_dependency_name(classid, objid);
	StringInfoData entry;

	if (!name)
		return;
	initStringInfo(&entry);
	appendStringInfoChar(&entry, '[');
	escape_json(&entry, classid == RelationRelationId ? "table" : classid == ProcedureRelationId ? "routine" : classid == OperatorRelationId ? "operator" : "type");
	appendStringInfoChar(&entry, ',');
	escape_json(&entry, name);
	appendStringInfoString(&entry, ",null]");
	if (!list_member(found->entries, makeString(entry.data)))
		found->entries = lappend(found->entries, makeString(entry.data));
}

static bool
query_dependencies_walker(Node *node, QueryDependencies *found)
{
	if (!node)
		return false;
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		/* As find_expr_references_walker. */
		if (rte->rtekind == RTE_NAMEDTUPLESTORE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("transition table \"%s\" cannot be referenced in a persistent object",
							rte->eref->aliasname)));
		if (rte->rtekind == RTE_RELATION)
		{
			/* A relation of another revision is opened in that revision's catalogs. */
			if (CatalogViewObjectContext(rte->relid) && CatalogViewObjectContext(rte->relid) != dg_catalog_session_view())
				unsupported("revision-qualified references in versioned schema objects");
			query_dependency(found, RelationRelationId, rte->relid);
		}
		else if (rte->rtekind == RTE_DOLT && OidIsValid(rte->dolt_rowtype))
		{
			/* What it uses is on its own branch: the query names no revision. */
			if (!rte->dolt_branch || strcmp(rte->dolt_branch, dg_current_branch()))
				unsupported("revision-qualified references in versioned schema objects");
			query_dependency(found, RelationRelationId, get_typ_typrelid(rte->dolt_rowtype));
		}
		return false;
	}
	if (IsA(node, FuncExpr))
		query_dependency(found, ProcedureRelationId, ((FuncExpr *) node)->funcid);
	if (IsA(node, Aggref))
		query_dependency(found, ProcedureRelationId, ((Aggref *) node)->aggfnoid);
	if (IsA(node, WindowFunc))
		query_dependency(found, ProcedureRelationId, ((WindowFunc *) node)->winfnoid);
	if (IsA(node, OpExpr) || IsA(node, DistinctExpr) || IsA(node, NullIfExpr))
		query_dependency(found, OperatorRelationId, ((OpExpr *) node)->opno);
	if (IsA(node, ScalarArrayOpExpr))
		query_dependency(found, OperatorRelationId, ((ScalarArrayOpExpr *) node)->opno);
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, query_dependencies_walker, found, QTW_EXAMINE_RTES_BEFORE);
	return expression_tree_walker(node, query_dependencies_walker, found);
}

/* One statement of a SQL-standard body, analyzed as interpret_AS_clause does. */
static Query *
routine_body_statement(Node *statement, SQLFunctionParseInfoPtr info, const char *sql)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;

	pstate->p_sourcetext = sql;
	sql_fn_parser_setup(pstate, info);
	query = transformStmt(pstate, statement);
	free_parsestate(pstate);
	return query;
}

/*
 * The parse-analyzed SQL-standard body of routine |proctup|, given its stored
 * |body|. A versioned routine's body is stored as its SQL, a String, analyzed
 * here in the catalogs of the routine's revision whenever it is read, as a
 * view's query is.
 */
Node *
dg_routine_body(HeapTuple proctup, Node *body)
{
	Oid			id = ((Form_pg_proc) GETSTRUCT(proctup))->oid;
	CatalogView *previous;
	Node	   *result = NULL;
	int			nest;

	if (!body || !IsA(body, String))
		return body;
	PushActiveSnapshot(GetLatestSnapshot());
	previous = CatalogViewSwitch(CatalogViewObjectContext(id));
	nest = NewGUCNestLevel();
	PG_TRY();
	{
		char	   *sql = psprintf("CREATE FUNCTION pg_catalog.dg_body() RETURNS void LANGUAGE sql %s", strVal(body));
		RawStmt    *raw = linitial_node(RawStmt, raw_parser(sql, RAW_PARSE_DEFAULT));
		Node	   *statements = castNode(CreateFunctionStmt, raw->stmt)->sql_body;
		SQLFunctionParseInfoPtr info = prepare_sql_fn_parse_info(proctup, NULL, InvalidOid);

		schema_expression_gucs();
		if (IsA(statements, List))
		{
			List	   *queries = NIL;

			foreach_ptr(Node, statement, linitial_node(List, castNode(List, statements)))
				queries = lappend(queries, routine_body_statement(statement, info, sql));
			result = (Node *) list_make1(queries);
		}
		else
			result = (Node *) routine_body_statement(statements, info, sql);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
		CatalogViewSwitch(previous);
		PopActiveSnapshot();
	}
	PG_END_TRY();
	return result;
}

/*
 * Routine |tuple|'s SQL-standard body as SQL, every name qualified, and as
 * |dependencies|, what it uses, as JSON.
 */
static char *
routine_body_encode(HeapTuple tuple, char **dependencies)
{
	Node	   *body = dg_routine_body(tuple, stringToNode(TextDatumGetCString(SysCacheGetAttrNotNull(PROCOID, tuple, Anum_pg_proc_prosqlbody))));
	QueryDependencies found = {0};
	StringInfoData json;
	char	   *sql = NULL;
	int			nest = NewGUCNestLevel();

	PG_TRY();
	{
		schema_expression_gucs();
		sql = pg_get_function_sqlbody_tuple(tuple);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	(void) query_dependencies_walker(body, &found);
	list_sort(found.entries, view_entry_compare);
	initStringInfo(&json);
	appendStringInfoChar(&json, '[');
	foreach_node(String, entry, found.entries)
		appendStringInfo(&json, "%s%s", foreach_current_index(entry) ? "," : "", strVal(entry));
	appendStringInfoChar(&json, ']');
	*dependencies = json.data;
	return sql;
}

/*
 * Create materialized view |create|, of |query|, as a versioned table whose
 * definition also holds its query and dependencies; REFRESH fills it.
 */
bool
dg_catalog_create_matview(CreateStmt *create, Query *query)
{
	QueryDependencies found = {0};
	StringInfoData json;
	char	   *sql = NULL;
	int			nest = NewGUCNestLevel();
	bool		result;

	if (!IsUnderPostmaster || rangevar_projection_hook || catalog_api_depth || !creates_prolly(create))
		return false;
	(void) query_dependencies_walker((Node *) query, &found);
	PG_TRY();
	{
		schema_expression_gucs();
		sql = pg_get_querydef(query, false);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	list_sort(found.entries, view_entry_compare);
	initStringInfo(&json);
	appendStringInfoChar(&json, '{');
	json_field(&json, "query", sql, false);
	appendStringInfoString(&json, ",\"populated\":false,\"dependencies\":[");
	foreach_node(String, entry, found.entries)
		appendStringInfo(&json, "%s%s", foreach_current_index(entry) ? "," : "", strVal(entry));
	appendStringInfoString(&json, "]}");
	matview_pending = json.data;
	PG_TRY();
	{
		result = dg_catalog_create((Node *) create, NULL);
	}
	PG_FINALLY();
	{
		matview_pending = NULL;
	}
	PG_END_TRY();
	return result;
}

/*
 * Empty versioned materialized view |rel| for REFRESH, and mark it populated
 * or not; its "matview" definition records which.
 */
void
dg_catalog_matview_refresh(Relation rel, bool populated)
{
	Oid			user;
	int			security,
				nest;

	if (rel->rd_rel->relispopulated != populated)
	{
		char	   *name = dg_qualified_name(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel));

		GetUserIdAndSecContext(&user, &security);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);

		/* The view may have been created by this very statement. */
		PushActiveSnapshot(GetLatestSnapshot());
		PG_TRY();
		{
			VersionRef	working;
			Capture		current;
			bytea	   *member = NULL;

			nest = begin_api();
			working = read_ref(branch_ref("working"));
			current = capture(true);
			member = dg_database_member(current.hash, "table", name);
			if (!member)
				elog(ERROR, "versioned materialized view %s is missing", name);
			{
				bytea	   *oldschema = child(member, 1);
				char	   *definition;
				bytea	   *schema;
				char	   *payload = metadata(member, "table");
				Oid			resource = binding_relation(dg_current_branch(), name, oldschema);

				run("SELECT jsonb_set($1::jsonb,'{matview,populated}',to_jsonb($2::bool))::text", 2, VC_TYPES(TEXTOID, BOOLOID), VC_ARGS(CStringGetTextDatum(metadata(oldschema, "schema")), BoolGetDatum(populated)), NULL, true);
				definition = strcell(0, 1);
				schema = put("schema", definition, NULL, 0);
				run("SELECT jsonb_set($1::jsonb,'{schema}',to_jsonb(encode(sha256(convert_to($2,'UTF8')),'hex')))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(payload), CStringGetTextDatum(definition)), NULL, true);
				member = put("table", strcell(0, 1), VC_ARGS(PointerGetDatum(schema), PointerGetDatum(child(member, 2))), 2);
				binding_record(dg_current_branch(), name, schema, resource);
			}
			move_ref(branch_ref("working"), working, alter_database(current, name, member));
			advance_snapshot();
			end_api(nest);
		}
		PG_FINALLY();
		{
			PopActiveSnapshot();
			SetUserIdAndSecContext(user, security);
		}
		PG_END_TRY();
	}
	dg_truncate(rel);
	CommandCounterIncrement();
}
