/* Canonical DDL publishes definitions and local resources, never schema copies. */
static void native_create_definitions(Capture current, CreateStmt *statement, const char *query, Oid caller,
									  const char *name, char **table, List **index_names,
									  List **index_definitions, List **foreign, List **sequences);
static void
canonical_empty_cleanup(void *arg)
{
	dg_release((DGState *) arg);
}

static char *
canonical_definition(const char *name, Oid owner, Acl *acl, const char *columns,
					 const char *indexes, const char *constraints, const char *indexdef)
{
	run("SELECT jsonb_build_object('format','DGTABLE1','name',$1::text,'owner',pg_get_userbyid($2),'comment',NULL,'acl',COALESCE((SELECT jsonb_agg(jsonb_build_array(pg_get_userbyid(x.grantor),CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END,x.privilege_type,x.is_grantable) ORDER BY pg_get_userbyid(x.grantor) COLLATE \"C\",CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END COLLATE \"C\",x.privilege_type COLLATE \"C\") FROM aclexplode($3::aclitem[]) x),'[]'::jsonb),'columns',$4::jsonb,'indexes',(SELECT coalesce(jsonb_agg(v ORDER BY v #>> '{}' COLLATE \"C\"),'[]'::jsonb) FROM jsonb_array_elements($5::jsonb) v),'constraints',(SELECT coalesce(jsonb_agg(v ORDER BY (v->>0) COLLATE \"C\"),'[]'::jsonb) FROM jsonb_array_elements($6::jsonb) v),'index_definition',NULLIF($7::text,''))::text", 7, VC_TYPES(TEXTOID, OIDOID, ACLITEMARRAYOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(name), ObjectIdGetDatum(owner), PointerGetDatum(acl), CStringGetTextDatum(columns), CStringGetTextDatum(indexes), CStringGetTextDatum(constraints), CStringGetTextDatum(indexdef ? indexdef : "")), NULL, true);
	return strcell(0, 1);
}

static bytea *
canonical_empty_member(const char *kind, const char *name, const char *definition, PLTree * empty)
{
	bytea	   *schema = put("schema", definition, NULL, 0);
	bytea	   *content = dg_history_export_tree(empty);
	bytea	   *raw = palloc(VARHDRSZ + PL_HASH_SIZE);
	char	   *payload;

	SET_VARSIZE(raw, VARHDRSZ + PL_HASH_SIZE);
	memcpy(VARDATA(raw), empty->root.bytes, PL_HASH_SIZE);
	run("SELECT jsonb_build_object('kind',$1::text,'name',$2::text,'schema',encode(sha256(convert_to($3,'UTF8')),'hex'),'content',$4::text)::text", 4, VC_TYPES(TEXTOID, TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(kind), CStringGetTextDatum(name), CStringGetTextDatum(definition), CStringGetTextDatum(hash_format(raw))), NULL, true);
	payload = strcell(0, 1);
	return put(kind, payload, VC_ARGS(PointerGetDatum(schema), PointerGetDatum(content)), 2);
}

/* The canonical definition of physical namespace |name|. */
static char *
namespace_definition(const char *name)
{
	run("SELECT d FROM (" NAMESPACE_DEFINITIONS_SQL ") AS x(d) WHERE d::jsonb->>'name'=$1", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(name)), NULL, true);
	if (SPI_processed != 1)
		elog(ERROR, "schema \"%s\" has no definition", name);
	return strcell(0, 1);
}

static void adopt_schema(Oid schema);

/*
 * A schema created in a repository is versioned on the session's branch, as
 * Doltgres versions schemas. Its physical namespace is shared by all branches.
 */
void
dg_catalog_schema_created(Oid schema)
{
	char	   *name = get_namespace_name(schema);

	if (!IsUnderPostmaster || catalog_api_depth || isAnyTempNamespace(schema) ||
		dg_system_namespace(name) || !dg_catalog_active())
		return;
	adopt_schema(schema);
}

/*
 * Add physical schema |schema| to the session branch's working schema, unless
 * it is already there. A schema created before the database had prolly
 * storage joins the branch with its first versioned table.
 */
static void
adopt_schema(Oid schema)
{
	char	   *name = get_namespace_name(schema);
	Oid			user;
	int			security,
				nest;

	CommandCounterIncrement();
	PushActiveSnapshot(GetLatestSnapshot());
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		VersionRef	working;

		nest = begin_api();
		working = read_ref(branch_ref("working"));
		if (!catalog_namespace_find(working.target, name))
		{
			char	   *definition = namespace_definition(name);
			Capture		current = capture(true);
			Datum	   *members;
			int			count;

			run("SELECT unnest(objchildren) FROM pg_dolt_object_visible WHERE objhash=$1", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(current.hash)), NULL, true);
			count = SPI_processed;
			members = palloc_array(Datum, count + 1);
			for (int i = 0; i < count; i++)
				members[i] = cell(i, 1);
			members[count] = PointerGetDatum(put("namespace", definition, NULL, 0));
			run("SELECT jsonb_set($1::jsonb,'{members}',($1::jsonb->'members')||jsonb_build_array(jsonb_build_object('kind','namespace','definition',$2::jsonb)))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(definition)), NULL, true);
			move_ref(branch_ref("working"), working, canonical_database(strcell(0, 1), members, count + 1));
			advance_snapshot();
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		PopActiveSnapshot();
	}
	PG_END_TRY();
	/* Adoption may replace working membership; refresh the caller too. */
	if (ActiveSnapshotSet())
		UpdateActiveSnapshotCommandId();
}

/* Whether CREATE TABLE makes a prolly table rather than a native one. */
static bool
creates_prolly(CreateStmt *stmt)
{
	if (stmt->relation->relpersistence == RELPERSISTENCE_TEMP)
		return false;
	if (stmt->accessMethod)
		return strcmp(stmt->accessMethod, "dolt_prolly_table") == 0;
	return default_table_access_method && strcmp(default_table_access_method, "dolt_prolly_table") == 0;
}

/*
 * Make the database a repository. Its prolly objects resolve through branch
 * schemas from the first one onward; native objects stay unversioned.
 */
static void
catalog_activate(void)
{
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;
	List	   *namespaces = NIL;
	ListCell   *lc;

	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		nest = begin_api();
		LockDatabaseObject(DoltRefRelationId, 0, 0, ExclusiveLock);
		ensure_repository();
		if (!read_ref("views/enabled").target)
		{
			/*
			 * Native schemas created after repository initialization must join
			 * the first versioned working set too, including empty schemas.
			 * Only import at activation: later physical schemas may belong to
			 * another branch and must never leak into this branch.
			 */
			run("SELECT n.oid FROM pg_namespace n WHERE " USER_NAMESPACE,
				0, NULL, NULL, NULL, true);
			for (uint64 i = 0; i < SPI_processed; i++)
				namespaces = lappend_oid(namespaces, DatumGetObjectId(cell(i, 1)));
			move_ref("views/enabled", empty_ref, read_ref("heads/main").target);
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
	CommandCounterIncrement();
	if (ActiveSnapshotSet())
		UpdateActiveSnapshotCommandId();
	foreach(lc, namespaces)
		adopt_schema(lfirst_oid(lc));
}

/* A materialized view being created: its definition's "matview" object. */
static char *matview_pending;

bool
dg_catalog_create(Node *statement, const char *query)
{
	CreateStmt *stmt;
	Oid			user,
				namespaceid,
				existing;
	int			security,
				nest;
	bool		pushed = false,
				skip = false;
	bool		adopt = false;
	char	   *name;
	List	   *foreigncmds = NIL;
	List	   *sequences = NIL;
	ListCell   *lc;

	if (!IsA(statement, CreateStmt) || !IsUnderPostmaster || rangevar_projection_hook || catalog_api_depth ||
		!creates_prolly((CreateStmt *) statement))
		return false;
	if (!dg_catalog_active())
		catalog_activate();
	stmt = copyObject((CreateStmt *) statement);
	if (XactReadOnly)
		PreventCommandIfReadOnly("CREATE TABLE");
	if (stmt->relation->relpersistence != RELPERSISTENCE_PERMANENT)
		unsupported("unlogged versioned tables");
	if (stmt->partbound || stmt->partspec)
		unsupported("partitioned versioned tables");
	if (stmt->ofTypename)
		unsupported("typed versioned tables");
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	GetUserIdAndSecContext(&user, &security);
	namespaceid = RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock, &existing);
	stmt->relation->schemaname = get_namespace_name(namespaceid);
	name = dg_qualified_name(stmt->relation->schemaname, stmt->relation->relname);
	if (OidIsValid(existing) && !CatalogViewIsDurable(existing))
	{
		if (!stmt->if_not_exists)
			ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE), errmsg("relation \"%s\" already exists", stmt->relation->relname)));
		ereport(NOTICE, (errmsg("relation \"%s\" already exists, skipping", stmt->relation->relname)));
		if (pushed)
			PopActiveSnapshot();
		return true;
	}
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		char	   *nsdef;

		nest = begin_api();
		nsdef = catalog_namespace_find(read_ref(branch_ref("working")).target, stmt->relation->schemaname);

		/*
		 * A schema created since the repository was initialized joins the
		 * branch with its first versioned object. The physical schema has
		 * already passed the CREATE privilege check.
		 */
		if (nsdef)
			catalog_acl(nsdef, "CREATE", user);
		else
			adopt = true;
		skip = catalog_contains(read_ref(branch_ref("working")).target, name);
		if (skip && !stmt->if_not_exists)
			ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE), errmsg("relation \"%s\" already exists", stmt->relation->relname)));
		run("SELECT 1 FROM pg_event_trigger WHERE evtenabled <> 'D'", 0, NULL, NULL, NULL, true);
		if (SPI_processed)
			unsupported("branch CREATE with event triggers");
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
	} PG_END_TRY();
	if (skip)
	{
		ereport(NOTICE, (errmsg("relation \"%s\" already exists, skipping", stmt->relation->relname)));
		if (pushed)
			PopActiveSnapshot();
		return true;
	}
	if (adopt)
	{
		adopt_schema(namespaceid);
		UpdateActiveSnapshotCommandId();
	}
	stmt->if_not_exists = false;
	stmt->accessMethod = pstrdup("dolt_prolly_table");

	/* Storage options and tablespaces mean nothing to prolly storage. */
	stmt->options = NIL;
	stmt->tablespacename = NULL;
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		VersionRef	working;
		Capture		current;
		StringInfoData added;
		List	   *index_names,
				   *index_definitions;
		char	   *table;
		bytea	  **newmembers;
		Datum	   *members;
		int			oldcount,
					k,
					total;
		bytea	   *database;
		DGState    *empty = palloc0(sizeof(*empty));

		nest = begin_api();
		working = read_ref(branch_ref("working"));
		current = capture(true);
		native_create_definitions(current, stmt, query, user, name, &table,
								  &index_names, &index_definitions, &foreigncmds, &sequences);
		if (matview_pending)
		{
			run("SELECT jsonb_set($1::jsonb,'{matview}',$2::jsonb)::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(table), CStringGetTextDatum(matview_pending)), NULL, true);
			table = strcell(0, 1);
		}
		k = list_length(index_names);
		newmembers = palloc_array(bytea *, k + 1);
		empty->store = pl_store_create();
		if (!empty->store)
			ereport(ERROR, (errmsg("out of memory creating prolly store")));
		empty->cleanup.func = canonical_empty_cleanup;
		empty->cleanup.arg = empty;
		MemoryContextRegisterResetCallback(CurrentMemoryContext, &empty->cleanup);
		dg_check(pl_build(empty->store, NULL, 0, &empty->content));
		newmembers[0] = canonical_empty_member("table", name, table, &empty->content);
		for (int i = 0; i < k; i++)
			newmembers[i + 1] = canonical_empty_member("index", list_nth(index_names, i), list_nth(index_definitions, i), &empty->content);
		dg_release(empty);
		run("SELECT unnest(objchildren) FROM pg_dolt_object_visible WHERE objhash=$1", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(current.hash)), NULL, true);
		oldcount = SPI_processed;
		total = oldcount + k + 1 + list_length(sequences);
		members = palloc_array(Datum, total);
		for (int i = 0; i < oldcount; i++)
			members[i] = cell(i, 1);
		initStringInfo(&added);
		appendStringInfoChar(&added, '[');
		for (int i = 0; i <= k; i++)
		{
			members[oldcount + i] = PointerGetDatum(newmembers[i]);
			if (i)
				appendStringInfoChar(&added, ',');
			appendStringInfoString(&added, metadata(newmembers[i], i ? "index" : "table"));
		}
		foreach_ptr(char, seqdef, sequences)
		{
			bytea	   *member = object_member("sequence", seqdef);
			Oid			resource = sequence_resource(seqdef, true);

			binding_record(dg_current_branch(), json_value(seqdef, "name"), child(member, 1), resource);
			members[oldcount + k + 1 + foreach_current_index(seqdef)] = PointerGetDatum(member);
			appendStringInfo(&added, ",%s", metadata(member, "sequence"));
		}
		appendStringInfoChar(&added, ']');
		run("SELECT jsonb_set($1::jsonb,'{members}',($1::jsonb->'members')||$2::jsonb)::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(added.data)), NULL, true);
		database = canonical_database(strcell(0, 1), members, total);
		for (int i = 0; i <= k; i++)
		{
			const char *membername = i ? (const char *) list_nth(index_names, i - 1) : name;
			char		relkind = i ? RELKIND_INDEX : matview_pending ? RELKIND_MATVIEW : RELKIND_RELATION;
			Oid			resource = dg_storage_allocate_kind(relkind, dg_current_branch());

			binding_record(dg_current_branch(), membername, child(newmembers[i], 1), resource);
		}
		move_ref(branch_ref("working"), working, database);
		advance_snapshot();
		{
			Relation	rel = dg_catalog_write_table(dg_current_branch(), name, ACL_INSERT);

			dg_rebuild_indexes(rel);
			dg_storage_close_relation(rel);
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
	if (foreigncmds)
	{
		CommandCounterIncrement();
		PushActiveSnapshot(GetLatestSnapshot());
		PG_TRY();
		{
			foreach(lc, foreigncmds)
				if (!dg_catalog_alter(lfirst(lc)))
				elog(ERROR, "canonical foreign key CREATE was not handled");
		}
		PG_FINALLY();
		{
			PopActiveSnapshot();
		}
		PG_END_TRY();
	}
	return true;
}
