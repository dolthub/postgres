/* Allocation identity excludes ownership and snapshots, but includes every
 * option affecting the generated values or a bare RESTART. */
static bytea *
sequence_key(const char *definition)
{
	run("SELECT sha256(convert_to(jsonb_build_array($1::jsonb->'type',$1::jsonb->'start',$1::jsonb->'increment',$1::jsonb->'min',$1::jsonb->'max',$1::jsonb->'cache',$1::jsonb->'cycle')::text,'UTF8'))", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	return DatumGetByteaPCopy(cell(0, 1));
}

static void
sequence_lock(const char *definition, LOCKMODE mode)
{
	char	   *name = json_value(definition, "name");
	bytea	   *key = sequence_key(definition);

	LockDatabaseObject(DoltSequenceRelationId,
					   hash_bytes((unsigned char *) name, strlen(name)),
					   hash_bytes((unsigned char *) VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key)), mode);
}

/* Registry replacement is transactional. Old files remain retained; cached
 * native values are invalidated by observing the new physical locator. */
static Oid
sequence_state(const char *definition, bool create, bool replace)
{
	List	   *parts = stringToQualifiedNameList(json_value(definition, "name"), NULL);
	bytea	   *key = sequence_key(definition);
	Oid			result;
	bool		found;

	if (list_length(parts) != 2)
		elog(ERROR, "sequence identity is not qualified");
	if (create && !replace)
	{
		/*
		 * Existing counters need no write lock for a metadata binding. The
		 * allocator itself locks and refreshes the registry before use.
		 */
		result = sequence_state(definition, false, false);
		if (OidIsValid(result))
			return result;
	}
	if (create)
		sequence_lock(definition, ExclusiveLock);
	PushActiveSnapshot(GetLatestSnapshot());
	run("SELECT seqstorage FROM pg_dolt_sequence WHERE seqnamespace=$1::name AND seqname=$2::name AND seqdefinition=$3", 3,
		VC_TYPES(TEXTOID, TEXTOID, BYTEAOID), VC_ARGS(CStringGetTextDatum(strVal(linitial(parts))), CStringGetTextDatum(strVal(lsecond(parts))), PointerGetDatum(key)), NULL, true);
	found = SPI_processed != 0;
	result = found ? DatumGetObjectId(cell(0, 1)) : InvalidOid;
	PopActiveSnapshot();
	if ((found && !replace) || !create)
		return result;
	SequenceCheckState(pg_strtoint64(json_value(definition, "last_value")),
		pg_strtoint64(json_value(definition, "min")), pg_strtoint64(json_value(definition, "max")));
	result = dg_storage_allocate_kind(RELKIND_SEQUENCE, NULL);
	{
		RelationData rel = {0};
		FormData_pg_class form = {0};

		form.relkind = RELKIND_SEQUENCE;
		form.relpersistence = RELPERSISTENCE_PERMANENT;
		rel.rd_rel = &form;
		rel.rd_id = result;
		rel.rd_backend = INVALID_PROC_NUMBER;
		rel.rd_lockInfo.lockRelId.relId = result;
		rel.rd_lockInfo.lockRelId.dbId = MyDatabaseId;
		dg_storage_locator(result, &rel.rd_locator);
		SequenceInitializeResource(&rel, pg_strtoint64(json_value(definition, "last_value")), !strcmp(json_value(definition, "is_called"), "true"));
		RelationCloseSmgr(&rel);
	}
	run(found ? "UPDATE pg_dolt_sequence SET seqstorage=$3 WHERE seqnamespace=$1::name AND seqname=$2::name AND seqdefinition=$4" : "INSERT INTO pg_dolt_sequence VALUES($1::name,$2::name,$3,$4)", 4,
		VC_TYPES(TEXTOID, TEXTOID, OIDOID, BYTEAOID), VC_ARGS(CStringGetTextDatum(strVal(linitial(parts))), CStringGetTextDatum(strVal(lsecond(parts))), ObjectIdGetDatum(result), PointerGetDatum(key)), NULL, false);
	CommandCounterIncrement();
	return result;
}

static Oid
sequence_resource(const char *definition, bool create)
{
	return sequence_state(definition, create, false);
}

static char *
sequence_encode(Oid id)
{
	HeapTuple	tuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(id));
	HeapTuple	classtuple = SearchSysCache1(RELOID, ObjectIdGetDatum(id));
	bool		aclnull;
	Datum		aclvalue;
	Acl		   *acl;
	Form_pg_sequence seq;
	Relation	rel;
	StringInfoData json;
	char	   *name;

	if (!HeapTupleIsValid(tuple) || !HeapTupleIsValid(classtuple))
		elog(ERROR, "sequence definition is missing");
	aclvalue = SysCacheGetAttr(RELOID, classtuple, Anum_pg_class_relacl, &aclnull);
	acl = aclnull ? acldefault(OBJECT_SEQUENCE, ((Form_pg_class) GETSTRUCT(classtuple))->relowner) : DatumGetAclPCopy(aclvalue);
	seq = (Form_pg_sequence) GETSTRUCT(tuple);
	rel = relation_open(id, NoLock);
	name = dg_qualified_name(get_namespace_name(rel->rd_rel->relnamespace), RelationGetRelationName(rel));
	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGSEQ1\",\"name\":");
	escape_json(&json, name);
	appendStringInfoString(&json, ",\"owner\":");
	escape_json(&json, GetUserNameFromId(rel->rd_rel->relowner, false));
	appendStringInfo(&json, ",\"type\":\"%s\",\"start\":%" PRId64 ",\"increment\":%" PRId64 ",\"min\":%" PRId64 ",\"max\":%" PRId64 ",\"cache\":%" PRId64 ",\"cycle\":%s,\"last_value\":%" PRId64 ",\"is_called\":false,\"acl\":[]}",
					 seq->seqtypid == INT2OID ? "int2" : seq->seqtypid == INT4OID ? "int4" : "int8", seq->seqstart, seq->seqincrement, seq->seqmin, seq->seqmax, seq->seqcache, seq->seqcycle ? "true" : "false", seq->seqstart);
	json.len--;
	json.data[json.len] = '\0';
	{
		Relation	deps = table_open(DependRelationId, AccessShareLock);
		SysScanDesc scan = systable_beginscan(deps, InvalidOid, false, NULL, 0, NULL);
		HeapTuple	dep;

		while (HeapTupleIsValid(dep = systable_getnext(scan)))
		{
			Form_pg_depend edge = (Form_pg_depend) GETSTRUCT(dep);

			if (edge->classid == RelationRelationId && edge->objid == id && edge->refclassid == RelationRelationId && edge->refobjsubid > 0 &&
				(edge->deptype == DEPENDENCY_AUTO || edge->deptype == DEPENDENCY_INTERNAL))
			{
				appendStringInfoString(&json, ",\"owned_table\":");
				escape_json(&json, dg_qualified_name(get_namespace_name(get_rel_namespace(edge->refobjid)), get_rel_name(edge->refobjid)));
				appendStringInfoString(&json, ",\"owned_column\":");
				escape_json(&json, get_attname(edge->refobjid, edge->refobjsubid, false));
				appendStringInfo(&json, ",\"identity\":%s", edge->deptype == DEPENDENCY_INTERNAL ? "true" : "false");
			}
		}
		systable_endscan(scan);
		table_close(deps, AccessShareLock);
	}
	appendStringInfoChar(&json, '}');
	relation_close(rel, NoLock);
	ReleaseSysCache(tuple);
	ReleaseSysCache(classtuple);
	run("SELECT ($1::jsonb || jsonb_build_object('acl',COALESCE((SELECT jsonb_agg(jsonb_build_array(pg_get_userbyid(x.grantor),CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END,x.privilege_type,x.is_grantable) ORDER BY pg_get_userbyid(x.grantor) COLLATE \"C\",CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END COLLATE \"C\",x.privilege_type COLLATE \"C\") FROM aclexplode($2::aclitem[]) x),'[]'::jsonb)))::text", 2, VC_TYPES(TEXTOID, ACLITEMARRAYOID), VC_ARGS(CStringGetTextDatum(json.data), PointerGetDatum(acl)), NULL, true);
	return strcell(0, 1);
}

static int64
sequence_number(const char *definition, const char *field)
{
	return pg_strtoint64(json_value(definition, field));
}

static char *
sequence_capture(const char *definition)
{
	Oid			resource;
	RelationData rel = {0};
	FormData_pg_class form = {0};
	int64		last;
	bool		called;

	/* Replaced files remain retained, so a snapshot can inspect the last
	 * committed registry entry without blocking unrelated branch DDL. */
	resource = sequence_resource(definition, false);
	if (!OidIsValid(resource))
		return (char *) definition;
	form.relkind = RELKIND_SEQUENCE;
	form.relpersistence = RELPERSISTENCE_PERMANENT;
	rel.rd_rel = &form;
	rel.rd_id = resource;
	rel.rd_backend = INVALID_PROC_NUMBER;
	dg_storage_locator(resource, &rel.rd_locator);
	SequenceReadResource(&rel, &last, &called);
	RelationCloseSmgr(&rel);
	run("SELECT ($1::jsonb || jsonb_build_object('last_value',$2::bigint,'is_called',$3::boolean))::text", 3,
		VC_TYPES(TEXTOID, INT8OID, BOOLOID), VC_ARGS(CStringGetTextDatum(definition), Int64GetDatum(last), BoolGetDatum(called)), NULL, true);
	return strcell(0, 1);
}

/* Refresh on every open, even with a cached relcache entry and under repeatable
 * read: allocation and RESTART are global, not snapshot-visible table data. */
void
dg_catalog_sequence_refresh(Relation rel)
{
	Oid			caller;
	int			security,
				nest;
	CatalogView *previous;

	if (catalog_api_depth || rel->rd_rel->relkind != RELKIND_SEQUENCE || !CatalogViewIsDurable(RelationGetRelid(rel)))
		return;
	GetUserIdAndSecContext(&caller, &security);
	previous = CatalogViewSwitch(NULL);
	PG_TRY();
	{
		char	   *definition;
		Oid			resource;
		RelFileLocator locator;

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		definition = metadata(CatalogViewObjectSchema(RelationGetRelid(rel)), "schema");

		/*
		 * The lock keeps a RESTART from replacing the allocator under us. On a
		 * hot standby only replay changes it, and recovery allows no stronger
		 * lock on database objects.
		 */
		sequence_lock(definition, RecoveryInProgress() ? AccessShareLock : ShareLock);
		resource = sequence_resource(definition, false);
		if (!OidIsValid(resource))
			elog(ERROR, "sequence allocator is missing");
		PushActiveSnapshot(GetLatestSnapshot());
		dg_storage_locator(resource, &locator);
		PopActiveSnapshot();
		if (!RelFileLocatorEquals(locator, rel->rd_locator))
			RelationCloseSmgr(rel);
		rel->rd_locator = locator;
		rel->rd_rel->relfilenode = locator.relNumber;
		end_api(nest);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		SetUserIdAndSecContext(caller, security);
	}
	PG_END_TRY();
}

void
dg_catalog_sequence_check(Relation rel)
{
	if (rel->rd_catalogview && !CatalogViewIsDurable(RelationGetRelid(rel)))
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("cannot advance a sequence in an immutable revision")));
}

/* Validate with PostgreSQL's native option parser, then publish the definition's
 * counter. New definitions inherit the old allocator state unless RESTART says
 * otherwise. No physical pg_sequence row is changed. */
static void
sequence_alter(ParseState *pstate, Oid id, AlterSeqStmt *stmt)
{
	Oid			caller;
	int			security,
				nest;
	char	   *definition;
	int64		last;
	bool		called;
	CatalogView *view = CatalogViewCurrent();
	bool		restart = false;

	foreach_node(DefElem, option, stmt->options)
		if (!strcmp(option->defname, "restart"))
			restart = true;
	GetUserIdAndSecContext(&caller, &security);
	PG_TRY();
	{
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		definition = sequence_encode(id);
		CatalogViewSwitch(NULL);

		/*
		 * Exclude concurrent allocations until we have taken the initial
		 * value.
		 */
		sequence_lock(definition, ExclusiveLock);
		definition = sequence_capture(definition);
		last = sequence_number(definition, "last_value");
		called = !strcmp(json_value(definition, "is_called"), "true");
		end_api(nest);
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		SequenceAlterDefinition(pstate, id, stmt, &last, &called);
		CommandCounterIncrement();
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		definition = sequence_encode(id);
		CatalogViewSwitch(NULL);
		run("SELECT ($1::jsonb || jsonb_build_object('last_value',$2::bigint,'is_called',$3::boolean))::text", 3, VC_TYPES(TEXTOID, INT8OID, BOOLOID), VC_ARGS(CStringGetTextDatum(definition), Int64GetDatum(last), BoolGetDatum(called)), NULL, true);
		definition = strcell(0, 1);
		sequence_state(definition, true, restart);
		end_api(nest);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
	}
	PG_END_TRY();
}

static List *
sequence_definitions(CatalogView *view)
{
	List	   *definitions = NIL;
	CatalogView *previous = CatalogViewSwitch(view);

	foreach_ptr(HeapTupleData, tuple, CatalogViewTuples(view, SequenceRelationId))
	{
		Oid			id = ((Form_pg_sequence) GETSTRUCT(tuple))->seqrelid;

		if (id >= FirstNormalObjectId)
			definitions = lappend(definitions, sequence_encode(id));
	}
	CatalogViewSwitch(previous);
	return definitions;
}

/* Retained state is data, never a capability to mutate the shared allocator. */
bool
dg_catalog_sequence_retained(Relation rel, int64 *last, bool *called)
{
	Oid			caller;
	int			security,
				nest;

	if (!CatalogViewIsLeased(RelationGetRelid(rel)))
		return false;
	GetUserIdAndSecContext(&caller, &security);
	PG_TRY();
	{
		char	   *definition;

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		definition = metadata(CatalogViewObjectSchema(RelationGetRelid(rel)), "schema");
		*last = sequence_number(definition, "last_value");
		*called = !strcmp(json_value(definition, "is_called"), "true");
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(caller, security);
	}
	PG_END_TRY();
	return true;
}
