/* Route native signature checks to immutable, branch-local pg_proc values. */
static bool routine_creating;

bool
dg_catalog_creating_routine(void)
{
	return routine_creating;
}

Oid
dg_catalog_routine_namespace(Oid namespaceid)
{
	if (!routine_creating)
		return namespaceid;
	namespaceid = CatalogViewNamespace(CatalogViewCurrent(), namespaceid);
	if (!CatalogViewBinds(CatalogViewCurrent(), namespaceid))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
						errmsg("routine target schema is not present in the selected revision")));
	return namespaceid;
}

/*
 * Whether CREATE FUNCTION makes a versioned routine: SQL or PL/pgSQL in a user
 * schema. Others, such as C functions, stay native and unversioned, as do
 * event trigger functions, which only the database's native event triggers call.
 */
static bool
creates_versioned_routine(CreateFunctionStmt *stmt)
{
	char	   *name;
	Oid			namespace = QualifiedNameGetCreationNamespace(stmt->funcname, &name);
	const char *language = "sql";
	ListCell   *lc;

	if (isAnyTempNamespace(namespace) || dg_system_namespace(get_namespace_name(namespace)))
		return false;
	/* %TYPE is never event_trigger, and looking it up would notice twice. */
	if (stmt->returnType && !stmt->returnType->pct_type &&
		LookupTypeNameOid(NULL, stmt->returnType, true) == EVENT_TRIGGEROID)
		return false;
	foreach(lc, stmt->options)
	{
		DefElem    *option = lfirst_node(DefElem, lc);

		if (!strcmp(option->defname, "language"))
			language = strVal(option->arg);
	}
	return !strcmp(language, "sql") || !strcmp(language, "plpgsql");
}

/*
 * Replace the working schema's |kind| members named in |removed|, named with
 * |prefix|, or all of them, with |definitions|, and return the published
 * database.
 */
static bytea *
objects_publish(const char *kind, List *removed, const char *prefix, bool all, List *definitions)
{
	VersionRef	working = read_ref(branch_ref("working"));
	Capture		current = capture(true);
	StringInfoData names,
				payloads;
	char	   *json;
	Datum	   *members;
	int			n;
	bytea	   *database;
	const char *matches = "v->>'kind'=$5 AND ($4 OR $2::jsonb ? (v->>'name') OR starts_with(v->>'name',$6))";

	initStringInfo(&names);
	appendStringInfoChar(&names, '[');
	foreach_ptr(char, name, removed)
	{
		if (names.len > 1)
			appendStringInfoChar(&names, ',');
		escape_json(&names, name);
	}
	appendStringInfoChar(&names, ']');
	initStringInfo(&payloads);
	appendStringInfoChar(&payloads, '[');
	foreach_ptr(char, definition, definitions)
	{
		if (payloads.len > 1)
			appendStringInfoChar(&payloads, ',');
		appendStringInfoString(&payloads, object_payload(kind, definition));
	}
	appendStringInfoChar(&payloads, ']');
	run(psprintf("SELECT jsonb_set($1::jsonb,'{members}',COALESCE((SELECT jsonb_agg(v) FROM jsonb_array_elements($1::jsonb->'members') v WHERE NOT (%s)),'[]'::jsonb)||$3::jsonb)::text", matches),
		6, VC_TYPES(TEXTOID, TEXTOID, TEXTOID, BOOLOID, TEXTOID, TEXTOID),
		VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(names.data), CStringGetTextDatum(payloads.data),
				BoolGetDatum(all), CStringGetTextDatum(kind), CStringGetTextDatum(prefix ? prefix : "\x01")),
		NULL, true);
	json = strcell(0, 1);
	run(psprintf("SELECT o.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) h JOIN pg_dolt_object_visible o ON o.objhash=h CROSS JOIN LATERAL (SELECT CASE WHEN o.objkind=$5 THEN convert_from(o.objpayload,'UTF8')::jsonb END AS v) p WHERE d.objhash=$1 AND NOT coalesce(%s, false)", matches),
		6, VC_TYPES(BYTEAOID, TEXTOID, TEXTOID, BOOLOID, TEXTOID, TEXTOID),
		VC_ARGS(PointerGetDatum(current.hash), CStringGetTextDatum(names.data), CStringGetTextDatum(payloads.data),
				BoolGetDatum(all), CStringGetTextDatum(kind), CStringGetTextDatum(prefix ? prefix : "\x01")),
		NULL, true);
	n = SPI_processed;
	members = palloc_array(Datum, n + list_length(definitions) + 1);
	for (int i = 0; i < n; i++)
		members[i] = cell(i, 1);
	foreach_ptr(char, definition, definitions)
		members[n++] = PointerGetDatum(object_member(kind, definition));
	database = canonical_database(json, members, n);
	move_ref(branch_ref("working"), working, database);
	advance_snapshot();
	return database;
}

static bytea *
routines_publish(List *removed, bool all, List *definitions)
{
	return objects_publish("routine", removed, NULL, all, definitions);
}

/* The routine an ALTER statement changes, if it is a versioned one. */
static ObjectWithArgs *
altered_routine(Node *statement, ObjectType *type)
{
	ObjectWithArgs *routine = NULL;
	Oid			id;

	if (IsA(statement, AlterFunctionStmt))
	{
		*type = ((AlterFunctionStmt *) statement)->objtype;
		routine = ((AlterFunctionStmt *) statement)->func;
	}
	if (!routine)
		return NULL;
	id = LookupFuncWithArgs(*type, routine, true);
	return OidIsValid(id) && id >= FirstDurableObjectId ? routine : NULL;
}

/*
 * ALTER FUNCTION of a versioned routine runs natively against a
 * provisional copy of the branch's catalogs; every routine there is then
 * encoded again and published. Unchanged routines encode to the same bytes.
 */
static void
routine_alter(ParseState *pstate, Node *statement)
{
	CatalogView *view = CatalogViewCreate("provisional routine DDL");
	CatalogView *previous = CatalogViewCurrent();
	Oid			caller,
				privileged;
	int			security,
				nest;
	List	   *definitions = NIL;

	GetUserIdAndSecContext(&caller, &security);
	PG_TRY();
	{
		Capture		current;
		Relation	catalog;
		SysScanDesc scan;
		HeapTuple	tuple;

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		current = capture(true);
		provider_populate(view, current.hash, dg_current_branch());
		end_api(nest);
		CatalogViewBeginMutation(view);
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		AlterFunction(pstate, (AlterFunctionStmt *) statement);
		CommandCounterIncrement();
		GetUserIdAndSecContext(&privileged, &security);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		catalog = table_open(ProcedureRelationId, AccessShareLock);
		scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			/* Versioned routines only: server functions are listed beside them. */
			if (((Form_pg_proc) GETSTRUCT(tuple))->oid >= FirstDurableObjectId && ((Form_pg_proc) GETSTRUCT(tuple))->prokind != PROKIND_AGGREGATE && !routine_derived(tuple))
				definitions = lappend(definitions, routine_encode(tuple, RelationGetDescr(catalog)));
		systable_endscan(scan);
		table_close(catalog, AccessShareLock);
		CatalogViewSwitch(NULL);
		(void) routines_publish(NIL, true, definitions);
		end_api(nest);
		UpdateActiveSnapshotCommandId();
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(caller, security);
		CatalogViewSwitch(previous);
		if (CatalogViewIsProvisional(view))
			CatalogViewRetireMutation(view);
		CatalogViewRelease(view);
	}
	PG_END_TRY();
}

bool
dg_catalog_routine_ddl(ParseState *pstate, Node *statement)
{
	CatalogView *previous;
	bool		pushed = false;
	ObjectType	type;

	if (IsA(statement, AlterFunctionStmt) &&
		dg_catalog_active() && altered_routine(statement, &type))
	{
		if (!ActiveSnapshotSet())
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			pushed = true;
		}
		if (CatalogViewCurrent())
			unsupported("routine DDL in an immutable revision context");
		dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
		routine_alter(pstate, statement);
		if (pushed)
			PopActiveSnapshot();
		return true;
	}
	if (!IsA(statement, CreateFunctionStmt) ||
		!creates_versioned_routine((CreateFunctionStmt *) statement))
		return false;
	if (!dg_catalog_active())
	{
		if (!default_table_access_method || strcmp(default_table_access_method, "dolt_prolly_table"))
			return false;
		catalog_activate();
	}
	{
		char *name;
		Oid namespaceid = QualifiedNameGetCreationNamespace(((CreateFunctionStmt *) statement)->funcname, &name);

		adopt_schema(namespaceid);
	}
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	if (CatalogViewCurrent())
		unsupported("routine DDL in an immutable revision context");
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	{
		Oid			user;
		int			security,
					nest;

		GetUserIdAndSecContext(&user, &security);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		PG_TRY();
		{
			nest = begin_api();
			run("SELECT 1 FROM pg_event_trigger WHERE evtenabled<>'D' AND evtevent<>'login'", 0, NULL, NULL, NULL, true);
			if (SPI_processed)
				unsupported("canonical routine DDL with event triggers");
			end_api(nest);
		}
		PG_FINALLY();
		{
			SetUserIdAndSecContext(user, security);
		}
		PG_END_TRY();
	}
	previous = CatalogViewSwitch(dg_catalog_session_view());
	routine_creating = IsA(statement, CreateFunctionStmt);
	PG_TRY();
	{
		CreateFunction(pstate, (CreateFunctionStmt *) statement);
	}
	PG_FINALLY();
	{
		routine_creating = false;
		CatalogViewSwitch(previous);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
	return true;
}

Oid
dg_catalog_store_routine(HeapTuple tuple, TupleDesc desc)
{
	Oid			user,
				result = InvalidOid;
	int			security,
				nest;
	CatalogView *previous = CatalogViewCurrent();

	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		char	   *definition,
				   *name;
		bytea	   *database;

		nest = begin_api();
		definition = routine_encode(tuple, desc);
		name = json_value(definition, "name");
		CatalogViewSwitch(NULL);
		database = routines_publish(list_make1(name), false, list_make1(definition));

		/*
		 * The routine belongs to the branch head, whose view its validator
		 * and callers use; a revision view would make its tables read-only.
		 */
		result = CatalogViewLookupObject(provider_branch_view(dg_current_branch(), database),
										 ProcedureRelationId, name);
		end_api(nest);
		UpdateActiveSnapshotCommandId();
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}
