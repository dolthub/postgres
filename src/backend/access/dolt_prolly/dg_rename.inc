/*
 * Renames of versioned objects. Definitions name the objects they use, so a
 * rename runs natively against a provisional copy of the branch's catalogs,
 * where every object still refers to the renamed one by OID, and then
 * encodes again from that copy exactly the renamed object and the members
 * the database's dependency record lists as referring to it. Tables and
 * indexes keep their rows and storage under their new names. COMMENT on a
 * member stored apart from tables changes it in place the same way.
 */

/* A versioned object's definition, encoded again from provisional |view|. */
static char *
rename_encode(CatalogView *view, const char *kind, const char *name, const char *old)
{
	CatalogView *previous = CatalogViewSwitch(view);
	char	   *result = NULL;
	int			nest = NewGUCNestLevel();

	PG_TRY();
	{
		if (!strcmp(kind, "table") || !strcmp(kind, "index"))
		{
			Oid			relid = CatalogViewLookupObject(view, RelationRelationId, name);
			int		   *mapping;
			int			count;

			result = mutation_columns(view, relid, old, &mapping, &count);
			if (!strcmp(kind, "table"))
			{
				result = mutation_constraints(view, relid, result);
				result = mutation_inheritance(view, relid, result);
				result = mutation_index_list(result, mutation_indexes(view, relid));
				if (json_value(result, "matview"))
				{
					run("SELECT jsonb_set($1::jsonb,'{matview}',$2::jsonb)::text", 2, VC_TYPES(TEXTOID, TEXTOID),
						VC_ARGS(CStringGetTextDatum(result), CStringGetTextDatum(matview_encode(relid, json_value(result, "matview")))), NULL, true);
					result = strcell(0, 1);
				}
			}
			else
			{
				char	   *sql;

				schema_expression_gucs();
				sql = TextDatumGetCString(DirectFunctionCall1(pg_get_indexdef, ObjectIdGetDatum(relid)));
				run("SELECT jsonb_set($1::jsonb,'{index_definition}',to_jsonb($2::text))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(result), CStringGetTextDatum(sql)), NULL, true);
				result = strcell(0, 1);
			}
			run("SELECT jsonb_set($1::jsonb,'{name}',to_jsonb($2::text))::text", 2, VC_TYPES(TEXTOID, TEXTOID),
				VC_ARGS(CStringGetTextDatum(result), CStringGetTextDatum(dg_qualified_name(get_namespace_name(get_rel_namespace(relid)), get_rel_name(relid)))), NULL, true);
			result = strcell(0, 1);
		}
		else if (schema_object_class(kind))
		{
			Oid classid = schema_object_class(kind);
			Oid catalogid = classid == ProcedureRelationId ? AggregateRelationId : classid;
			Oid id = CatalogViewLookupObject(view, classid, name);
			Relation catalog = table_open(catalogid, AccessShareLock);
			ScanKeyData key;
			SysScanDesc scan;
			HeapTuple tuple;

			ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(id));
			scan = systable_beginscan(catalog, InvalidOid, false, NULL, 1, &key);
			tuple = systable_getnext(scan);
			if (!HeapTupleIsValid(tuple)) elog(ERROR, "missing schema object %s", name);
			result = schema_object_encode(catalogid, tuple, RelationGetDescr(catalog));
			systable_endscan(scan);
			table_close(catalog, AccessShareLock);
		}
		else if (!strcmp(kind, "sequence"))
		{
			char	   *state = sequence_capture(old);

			result = sequence_encode(CatalogViewLookupObject(view, RelationRelationId, name));
			run("SELECT ($1::jsonb || jsonb_build_object('last_value',$2::jsonb->'last_value','is_called',$2::jsonb->'is_called'))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(result), CStringGetTextDatum(state)), NULL, true);
			result = strcell(0, 1);
		}
		else if (!strcmp(kind, "view"))
			result = view_encode(CatalogViewLookupObject(view, RelationRelationId, name));
		else if (!strcmp(kind, "routine"))
		{
			HeapTuple	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(CatalogViewLookupObject(view, ProcedureRelationId, name)));
			Relation	catalog = table_open(ProcedureRelationId, AccessShareLock);

			result = routine_encode(tuple, RelationGetDescr(catalog));
			table_close(catalog, AccessShareLock);
			ReleaseSysCache(tuple);
		}
		else if (!strcmp(kind, "type"))
		{
			HeapTuple	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(CatalogViewLookupObject(view, TypeRelationId, name)));
			Relation	catalog = table_open(TypeRelationId, AccessShareLock);

			result = type_encode(tuple, RelationGetDescr(catalog));
			table_close(catalog, AccessShareLock);
			ReleaseSysCache(tuple);
		}
		else if (!strcmp(kind, "trigger"))
		{
			Oid			oid = CatalogViewLookupObject(view, TriggerRelationId, name);
			Relation	catalog = table_open(TriggerRelationId, AccessShareLock);
			ScanKeyData key;
			SysScanDesc scan;
			HeapTuple	tuple;

			ScanKeyInit(&key, Anum_pg_trigger_oid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(oid));
			scan = systable_beginscan(catalog, TriggerOidIndexId, true, NULL, 1, &key);
			tuple = systable_getnext(scan);
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "versioned trigger %s is missing", name);
			result = trigger_encode(tuple, RelationGetDescr(catalog));
			systable_endscan(scan);
			table_close(catalog, AccessShareLock);
		}
		else
			elog(ERROR, "cannot encode %s %s again", kind, name);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

static bool
rename_targeted(List *targets, const char *kind, const char *name)
{
	foreach_ptr(List, target, targets)
		if (!strcmp(linitial(target), kind) && !strcmp(lsecond(target), name))
		return true;
	return false;
}

/*
 * Encode |targets|, (kind, name) pairs naming members of the working schema
 * |current|, again from |view| and publish them in place of the old ones.
 */
static void
rename_publish(CatalogView *view, Capture current, List *targets)
{
	VersionRef	working = read_ref(branch_ref("working"));
	StringInfoData json;
	Datum	   *children;
	bytea	  **members;
	char	  **kinds,
			  **names;
	int			count,
				used = 0;

	run("SELECT t.objhash,t.objkind,CASE WHEN t.objkind<>'namespace' THEN convert_from(t.objpayload,'UTF8')::jsonb->>'name' END FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(current.hash)), NULL, true);
	count = SPI_processed;
	members = palloc_array(bytea *, Max(count, 1));
	kinds = palloc_array(char *, Max(count, 1));
	names = palloc_array(char *, Max(count, 1));
	children = palloc_array(Datum, Max(count, 1));
	for (int i = 0; i < count; i++)
	{
		members[i] = DatumGetByteaPCopy(cell(i, 1));
		kinds[i] = strcell(i, 2);
		names[i] = strcell(i, 3);
	}
	initStringInfo(&json);
	appendStringInfoChar(&json, '[');
	for (int i = 0; i < count; i++)
	{
		char	   *payload = metadata(members[i], kinds[i]);
		bytea	   *member = members[i];

		if (!strcmp(kinds[i], "namespace"))
			names[i] = json_value(payload, "name");

		if (names[i] && !strcmp(kinds[i], "namespace") && rename_targeted(targets, kinds[i], names[i]))
		{
			/* A schema's definition is its member's payload. */
			Oid			namespace = CatalogViewLookupObject(view, NamespaceRelationId, names[i]);
			CatalogView *previous = CatalogViewSwitch(view);
			char	   *name = get_namespace_name(namespace);
			char	   *comment = GetComment(namespace, NamespaceRelationId, 0);

			CatalogViewSwitch(previous);
			run("SELECT jsonb_set(jsonb_set($1::jsonb,'{name}',to_jsonb($2::text)),'{comment}',coalesce(to_jsonb($3::text),'null'))::text", 3,
				VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(payload), CStringGetTextDatum(name), comment ? CStringGetTextDatum(comment) : (Datum) 0),
				comment ? NULL : "  n", true);
			payload = strcell(0, 1);
			member = put("namespace", payload, NULL, 0);
		}
		else if (names[i] && rename_targeted(targets, kinds[i], names[i]))
		{
			bytea	   *oldschema = child(members[i], 1);
			char	   *definition = rename_encode(view, kinds[i], names[i], metadata(oldschema, "schema"));
			char	   *name = json_value(definition, "name");

			if (!strcmp(kinds[i], "table") || !strcmp(kinds[i], "index"))
			{
				bytea	   *schema = put("schema", definition, NULL, 0);
				Oid			resource = binding_relation(dg_current_branch(), names[i], oldschema);

				if (!OidIsValid(resource))
					elog(ERROR, "renamed relation's storage binding is unavailable");
				run("SELECT jsonb_set(jsonb_set($1::jsonb,'{name}',to_jsonb($2::text)),'{schema}',to_jsonb(encode(sha256(convert_to($3,'UTF8')),'hex')))::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID),
					VC_ARGS(CStringGetTextDatum(payload), CStringGetTextDatum(name), CStringGetTextDatum(definition)), NULL, true);
				payload = strcell(0, 1);
				member = put(kinds[i], payload, VC_ARGS(PointerGetDatum(schema), PointerGetDatum(child(members[i], 2))), 2);
				binding_record(dg_current_branch(), name, schema, resource);
			}
			else
			{
				payload = object_payload(kinds[i], definition);
				member = object_member(kinds[i], definition);
				if (!strcmp(kinds[i], "sequence"))
					binding_record(dg_current_branch(), name, child(member, 1), sequence_resource(definition, true));
			}
		}
		if (used)
			appendStringInfoChar(&json, ',');
		if (!strcmp(kinds[i], "namespace"))
			appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", payload);
		else
			appendStringInfoString(&json, payload);
		children[used++] = PointerGetDatum(member);
	}
	appendStringInfoChar(&json, ']');
	run("SELECT jsonb_set($1::jsonb,'{members}',$2::jsonb)::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(json.data)), NULL, true);
	move_ref(branch_ref("working"), working, canonical_database(strcell(0, 1), children, used));
	advance_snapshot();
}

/* Add the members that refer to |kind| object |name| to |targets|. */
static List *
rename_referrers(List *targets, const char *json, const char *kind, const char *name)
{
	foreach_ptr(List, referrer, database_referrers(json, kind, name))
		if (!rename_targeted(targets, linitial(referrer), lsecond(referrer)))
		targets = lappend(targets, referrer);
	return targets;
}

/* The schema a member's name qualifies it with. */
static char *
rename_member_schema(const char *kind, const char *name)
{
	char	   *prefix = pstrdup(name);
	char	   *end = strpbrk(prefix, (!strcmp(kind, "routine") || !strcmp(kind, "aggregate") || !strcmp(kind, "operator")) ? "(" : !strcmp(kind, "trigger") ? "/" : "");

	if (!strcmp(kind, "cast")) return pstrdup("");
	if (!strcmp(kind, "operator"))
	{
		if (end) *end = '\0';
		*strrchr(prefix, '.') = '\0';
		return strVal(linitial(stringToQualifiedNameList(prefix, NULL)));
	}

	if (end)
		*end = '\0';
	return strVal(linitial(stringToQualifiedNameList(prefix, NULL)));
}

/* A routine is referred to by its name without its argument types. */
static char *
rename_reference_name(const char *kind, const char *name)
{
	char	   *result = pstrdup(name);

	if ((!strcmp(kind, "routine") || !strcmp(kind, "operator")) && strchr(result, '('))
		*strchr(result, '(') = '\0';
	return result;
}

/*
 * Add every member in schema |schema| of database root |root|, and every
 * member that refers to one of them, to |targets|.
 */
static List *
rename_schema_members(List *targets, const char *root, const char *schema)
{
	List	   *members = NIL;

	run("SELECT v->>'kind',v->>'name' FROM jsonb_array_elements($1::jsonb->'members') v WHERE v->>'kind'<>'namespace'", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(root)), NULL, true);
	for (uint64 i = 0; i < SPI_processed; i++)
		members = lappend(members, list_make2(strcell(i, 1), strcell(i, 2)));
	foreach_ptr(List, member, members)
	{
		char	   *kind = linitial(member);
		char	   *name = lsecond(member);

		if (strcmp(rename_member_schema(kind, name), schema))
			continue;
		if (!rename_targeted(targets, kind, name))
			targets = lappend(targets, member);
		targets = rename_referrers(targets, root, kind, rename_reference_name(kind, name));
		if (!strcmp(kind, "table"))
			targets = rename_referrers(targets, root, "type", name);
	}
	return targets;
}

/*
 * The versioned object a COMMENT statement describes, if it is a member
 * stored apart from tables: tables and their columns keep their comments in
 * their definitions, which ALTER TABLE changes.
 */
static bool
comment_subject(CommentStmt *stmt, char **kind, char **name, List **references)
{
	Oid			id;

	*references = NIL;
	switch (stmt->objtype)
	{
		case OBJECT_AGGREGATE:
		case OBJECT_OPERATOR:
		case OBJECT_CAST:
			{
				Relation relation;
				ObjectAddress address = get_object_address(stmt->objtype, stmt->object, &relation, AccessShareLock, false);

				if (relation) relation_close(relation, NoLock);
				if (!CatalogViewObjectContext(address.objectId)) return false;
				*kind = stmt->objtype == OBJECT_AGGREGATE ? "aggregate" : stmt->objtype == OBJECT_OPERATOR ? "operator" : "cast";
				*name = (char *) CatalogViewObjectIdentity(address.objectId, address.classId);
				return true;
			}
		case OBJECT_FUNCTION:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
			id = LookupFuncWithArgs(stmt->objtype, castNode(ObjectWithArgs, stmt->object), true);
			if (!OidIsValid(id) || id < FirstDurableObjectId)
				return false;
			*kind = "routine";
			*name = CatalogViewObjectIdentity(id, ProcedureRelationId);
			return true;
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
			{
				HeapTuple	tuple;
				bool		member;

				id = LookupTypeNameOid(NULL, castNode(TypeName, stmt->object), true);
				if (!OidIsValid(id) || id < FirstDurableObjectId)
					return false;
				tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(id));
				member = type_versioned_row((Form_pg_type) GETSTRUCT(tuple));
				ReleaseSysCache(tuple);
				if (!member)
					unsupported("comments on a versioned table's row type");
				*kind = "type";
				*name = CatalogViewObjectIdentity(id, TypeRelationId);
				return true;
			}
		case OBJECT_VIEW:
			id = RangeVarGetRelid(makeRangeVarFromNameList(castNode(List, stmt->object)), NoLock, true);
			if (!OidIsValid(id) || id < FirstDurableObjectId || get_rel_relkind(id) != RELKIND_VIEW)
				return false;
			*kind = "view";
			*name = CatalogViewObjectIdentity(id, RelationRelationId);
			return true;
		case OBJECT_SCHEMA:
			{
				char	   *schema = strVal(stmt->object);
				int			nest = begin_api();
				bool		versioned = catalog_namespace_find(read_ref(branch_ref("working")).target, schema) != NULL;

				end_api(nest);
				if (!versioned)
					return false;
				*kind = "namespace";
				*name = pstrdup(schema);
				return true;
			}
		case OBJECT_TRIGGER:
			{
				List	   *names = castNode(List, stmt->object);

				id = RangeVarGetRelid(makeRangeVarFromNameList(list_copy_head(names, list_length(names) - 1)), NoLock, true);
				if (!OidIsValid(id) || id < FirstDurableObjectId)
					return false;
				*kind = "trigger";
				*name = psprintf("%s/%s", CatalogViewObjectIdentity(id, RelationRelationId), strVal(llast(names)));
				return true;
			}
		default:
			return false;
	}
}

/*
 * The versioned object a rename statement renames, in provisional |view|:
 * its kind and old name, and the references by which members name it.
 * Returns false if the statement renames no versioned object.
 */
static bool
rename_subject(Node *statement, char **kind, char **name, List **references)
{
	ObjectType	type;
	Node	   *object = NULL;
	RangeVar   *relation = NULL;

	if (IsA(statement, RenameStmt))
	{
		type = ((RenameStmt *) statement)->renameType;
		object = ((RenameStmt *) statement)->object;
		relation = ((RenameStmt *) statement)->relation;
		/* A column or constraint rename changes its table's or domain's definition. */
		if (type == OBJECT_COLUMN)
			type = ((RenameStmt *) statement)->relationType;
		else if (type == OBJECT_TABCONSTRAINT)
			type = OBJECT_TABLE;
		else if (type == OBJECT_DOMCONSTRAINT)
			type = OBJECT_DOMAIN;
	}
	else if (IsA(statement, AlterObjectSchemaStmt))
	{
		type = ((AlterObjectSchemaStmt *) statement)->objectType;
		object = ((AlterObjectSchemaStmt *) statement)->object;
		relation = ((AlterObjectSchemaStmt *) statement)->relation;
	}
	else if (IsA(statement, CommentStmt))
		return comment_subject((CommentStmt *) statement, kind, name, references);
	else if (IsA(statement, AlterEnumStmt) && ((AlterEnumStmt *) statement)->oldVal)
	{
		Oid			id = typenameTypeId(NULL, makeTypeNameFromNameList(((AlterEnumStmt *) statement)->typeName));

		if (id < FirstDurableObjectId)
			return false;
		*kind = "type";
		*name = CatalogViewObjectIdentity(id, TypeRelationId);
		*references = list_make1(list_make2("type", *name));
		return true;
	}
	else
		return false;
	*references = NIL;
	switch (type)
	{
		case OBJECT_TABLE:
		case OBJECT_INDEX:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
		case OBJECT_SEQUENCE:
			{
				Oid			relid = RangeVarGetRelid(relation, NoLock, true);

				if (!OidIsValid(relid) || relid < FirstDurableObjectId)
					return false;
				*kind = get_rel_relkind(relid) == RELKIND_INDEX ? "index" :
					get_rel_relkind(relid) == RELKIND_VIEW ? "view" :
					get_rel_relkind(relid) == RELKIND_SEQUENCE ? "sequence" : "table";
				*name = CatalogViewObjectIdentity(relid, RelationRelationId);
				if (!strcmp(*kind, "index"))
				{
					/* The table's definition lists its indexes. */
					Oid			table = IndexGetRelation(relid, false);

					*references = list_make1(list_make2("table", CatalogViewObjectIdentity(table, RelationRelationId)));
				}
				else
				{
					*references = list_make2(list_make2("table", *name), list_make2("type", *name));
				}
				return true;
			}
		case OBJECT_SCHEMA:
			{
				/* A schema the branch has; the members in it are found later. */
				char	   *schema = ((RenameStmt *) statement)->subname;
				int			nest = begin_api();
				bool		versioned = catalog_namespace_find(read_ref(branch_ref("working")).target, schema) != NULL;

				end_api(nest);
				if (!versioned)
					return false;
				*kind = "namespace";
				*name = pstrdup(schema);
				return true;
			}
		case OBJECT_TRIGGER:
			{
				Oid			relid = RangeVarGetRelid(relation, NoLock, true);

				if (!OidIsValid(relid) || relid < FirstDurableObjectId)
					return false;
				*kind = "trigger";
				*name = psprintf("%s/%s", CatalogViewObjectIdentity(relid, RelationRelationId),
								 ((RenameStmt *) statement)->subname);
				return true;
			}
		case OBJECT_OPERATOR:
			{
				Relation relation;
				ObjectAddress address = get_object_address(type, object, &relation, AccessShareLock, false);

				if (relation) relation_close(relation, NoLock);
				if (!CatalogViewObjectContext(address.objectId)) return false;
				*kind = "operator";
				*name = (char *) CatalogViewObjectIdentity(address.objectId, address.classId);
				*references = list_make1(list_make2("operator", rename_reference_name("operator", *name)));
				return true;
			}
		case OBJECT_AGGREGATE:
		case OBJECT_FUNCTION:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
			{
				Oid			id = LookupFuncWithArgs(type, castNode(ObjectWithArgs, object), true);

				if (!OidIsValid(id) || id < FirstDurableObjectId)
					return false;
				*kind = type == OBJECT_AGGREGATE ? "aggregate" : "routine";
				*name = CatalogViewObjectIdentity(id, ProcedureRelationId);
				*references = list_make1(list_make2("routine", dg_qualified_name(get_namespace_name(get_func_namespace(id)), get_func_name(id))));
				return true;
			}
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
		case OBJECT_ATTRIBUTE:
			{
				Oid			id;

				if (type == OBJECT_ATTRIBUTE)
					id = typenameTypeId(NULL, makeTypeNameFromNameList(relation->schemaname ?
																	   list_make2(makeString(relation->schemaname), makeString(relation->relname)) :
																	   list_make1(makeString(relation->relname))));
				else
					id = typenameTypeId(NULL, makeTypeNameFromNameList(castNode(List, object)));
				if (id < FirstDurableObjectId)
					return false;
				*kind = "type";
				*name = CatalogViewObjectIdentity(id, TypeRelationId);
				*references = list_make1(list_make2("type", *name));
				return true;
			}
		default:
			return false;
	}
}

static void
rename_storage(Relation relation, CatalogViewStorageEffect effect)
{
	unsupported("storage effects of a rename");
}

bool
dg_catalog_rename(ParseState *pstate, Node *statement)
{
	CatalogView *view,
			   *previous;
	CatalogView *saved_mutation = native_mutation_view;
	Oid			caller;
	int			security,
				nest;
	bool		pushed = false;
	char	   *kind,
			   *name;
	List	   *references;

	if ((!IsA(statement, RenameStmt) && !IsA(statement, AlterObjectSchemaStmt) && !IsA(statement, AlterEnumStmt) &&
		 !IsA(statement, CommentStmt)) ||
		!dg_catalog_active())
		return false;
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}

	/* The subject is resolved as the statement names it, on the branch head. */
	if (!rename_subject(statement, &kind, &name, &references))
	{
		if (pushed)
			PopActiveSnapshot();
		return false;
	}
	if (CatalogViewCurrent())
		unsupported("rename in an immutable revision context");
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	view = CatalogViewCreate("provisional rename");
	previous = CatalogViewCurrent();
	GetUserIdAndSecContext(&caller, &security);
	PG_TRY();
	{
		Capture		current;
		List	   *targets = NIL;
		char	   *root;
		MemoryContext caller_context = CurrentMemoryContext;

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		current = capture(true);
		CommandCounterIncrement();
		UpdateActiveSnapshotCommandId();
		provider_populate(view, current.hash, NULL);
		root = metadata(current.hash, "database");
		{
			List	   *found = list_make1(list_make2(kind, name));
			MemoryContext api_context;

			if (!strcmp(kind, "namespace"))
				found = rename_schema_members(found, root, name);
			foreach_ptr(List, reference, references)
			{
				if (!strcmp(linitial(reference), "table") && strcmp(kind, "table") && strcmp(kind, "view") && strcmp(kind, "sequence"))
				{
					/*
					 * An index's table lists it; the table itself is
					 * re-encoded.
					 */
					if (!rename_targeted(found, "table", lsecond(reference)))
						found = lappend(found, reference);
				}
				found = rename_referrers(found, root, linitial(reference), lsecond(reference));
			}

			if (!strcmp(kind, "table") && IsA(statement, RenameStmt) &&
				((RenameStmt *) statement)->renameType == OBJECT_COLUMN)
			{
				CatalogView *outer = CatalogViewSwitch(view);
				Oid parent = CatalogViewLookupObject(view, RelationRelationId, name);

				foreach_oid(childid, find_all_inheritors(parent, NoLock, NULL))
				{
					const char *child = CatalogViewObjectIdentity(childid, RelationRelationId);

					if (!rename_targeted(found, "table", child))
						found = lappend(found, list_make2("table", pstrdup(child)));
					found = rename_referrers(found, root, "table", child);
					found = rename_referrers(found, root, "type", child);
				}
				CatalogViewSwitch(outer);
			}

			/* The targets outlive this API call. */
			api_context = MemoryContextSwitchTo(caller_context);
			foreach_ptr(List, target, found)
				targets = lappend(targets, list_make2(pstrdup(linitial(target)), pstrdup(lsecond(target))));
			MemoryContextSwitchTo(api_context);
		}
		end_api(nest);
		CatalogViewBeginMutation(view);
		CatalogViewSetStorageHandler(view, rename_storage);
		native_mutation_view = view;

		/* Stored queries name what they use; bind them before it is renamed. */
		materialize_queries(view, targets);
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		if (IsA(statement, RenameStmt))
			ExecRenameStmt((RenameStmt *) statement);
		else if (IsA(statement, AlterEnumStmt))
			AlterEnum((AlterEnumStmt *) statement);
		else if (IsA(statement, CommentStmt))
			(void) CommentObject((CommentStmt *) statement);
		else
			ExecAlterObjectSchemaStmt((AlterObjectSchemaStmt *) statement, NULL);
		CommandCounterIncrement();
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		CatalogViewSwitch(NULL);

		/*
		 * The working schema is unchanged since the copy was made: native
		 * DDL changed only the copy.
		 */
		nest = begin_api();
		current = capture(true);
		CommandCounterIncrement();
		UpdateActiveSnapshotCommandId();
		root = metadata(current.hash, "database");
		rename_publish(view, current, targets);
		if (IsA(statement, AlterEnumStmt))
		{
			TypeRecode	recode = {
				.old = ((AlterEnumStmt *) statement)->oldVal,
				.renamed = ((AlterEnumStmt *) statement)->newVal,
			};

			type_tables_rewrite(root, name, &recode);
		}
		end_api(nest);

		/*
		 * Physical schemas are shared by every branch and by native objects:
		 * the old one stays, and the new name has one too.
		 */
		if (!strcmp(kind, "namespace") && IsA(statement, RenameStmt) && !OidIsValid(get_namespace_oid(((RenameStmt *) statement)->newname, true)))
		{
			HeapTuple	old = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(get_namespace_oid(name, false)));

			(void) NamespaceCreate(((RenameStmt *) statement)->newname,
								   ((Form_pg_namespace) GETSTRUCT(old))->nspowner, false);
			ReleaseSysCache(old);
			CommandCounterIncrement();
		}
		UpdateActiveSnapshotCommandId();
	}
	PG_FINALLY();
	{
		native_mutation_view = saved_mutation;
		SetUserIdAndSecContext(caller, security);
		CatalogViewSwitch(previous);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
	return true;
}
