/* Native dependency decisions, followed by one canonical working-set update. */
static bool
deletion_contains(List *objects, Oid classid, Oid id)
{
	ListCell   *lc;

	foreach(lc, objects)
	{
		ObjectAddress *object = lfirst(lc);

		if (object->classId == classid && object->objectId == id && object->objectSubId == 0)
			return true;
	}
	return false;
}

/* Rows of a table whose columns a deletion removed, to be laid out again. */
typedef struct DeletedColumns
{
	char	   *table;
	int			oldcount;
	int			count;
	int		   *mapping;
} DeletedColumns;

/* Whether |objects| deletes column |attnum| of relation |relid|. */
static bool
deletion_contains_column(List *objects, Oid relid, int attnum)
{
	foreach_ptr(ObjectAddress, object, objects)
		if (object->classId == RelationRelationId && object->objectId == relid && object->objectSubId == attnum)
		return true;
	return false;
}

static void
publish_deletions(List *objects, CatalogView *view)
{
	VersionRef	working = read_ref(branch_ref("working"));
	Capture		current = capture(true);
	bytea	  **hashes;
	char	  **kinds,
			  **definitions,
			  **payloads,
			  **names;
	bool	   *removed;
	Datum	   *children;
	StringInfoData json;
	int			n,
				used = 0;
	bytea	   *database;
	List	   *deleted_columns = NIL;

	run("SELECT o.objhash,o.objkind FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) h JOIN pg_dolt_object_visible o ON o.objhash=h WHERE d.objhash=$1", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(current.hash)), NULL, true);
	n = SPI_processed;
	hashes = palloc_array(bytea *, Max(n, 1));
	kinds = palloc_array(char *, Max(n, 1));
	definitions = palloc_array(char *, Max(n, 1));
	payloads = palloc_array(char *, Max(n, 1));
	names = palloc_array(char *, Max(n, 1));
	removed = palloc0_array(bool, Max(n, 1));
	children = palloc_array(Datum, Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		hashes[i] = DatumGetByteaPCopy(cell(i, 1));
		kinds[i] = strcell(i, 2);
	}
	for (int i = 0; i < n; i++)
	{
		Oid			classid,
					id;

		payloads[i] = metadata(hashes[i], kinds[i]);
		definitions[i] = !strcmp(kinds[i], "namespace") ? payloads[i] : metadata(child(hashes[i], 1), "schema");
		names[i] = json_value(definitions[i], "name");
		classid = schema_object_class(kinds[i]) ? schema_object_class(kinds[i]) : !strcmp(kinds[i], "namespace") ? NamespaceRelationId :
			!strcmp(kinds[i], "routine") ? ProcedureRelationId :
			!strcmp(kinds[i], "trigger") ? TriggerRelationId :
			!strcmp(kinds[i], "type") ? TypeRelationId : RelationRelationId;
		id = CatalogViewLookupObject(view, classid, names[i]);
		removed[i] = deletion_contains(objects, classid, id);
	}
	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGDB1\",\"members\":[");
	for (int i = 0; i < n; i++)
	{
		if (removed[i])
			continue;
		if (!strcmp(kinds[i], "table"))
		{
			Oid			tableid = CatalogViewLookupObject(view, RelationRelationId, names[i]);
			char	   *definition = definitions[i];
			bool		changed = false;
			ListCell   *lc;

			for (int j = 0; j < n; j++)
			{
				if (removed[j] && !strcmp(kinds[j], "index"))
				{
					Oid			indexid = CatalogViewLookupObject(view, RelationRelationId, names[j]);

					if (IndexGetRelation(indexid, false) == tableid)
					{
						run("SELECT jsonb_set($1::jsonb,'{indexes}',COALESCE((SELECT jsonb_agg(v) FROM jsonb_array_elements($1::jsonb->'indexes') v WHERE v<>to_jsonb($2::text)),'[]'::jsonb))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(json_value(definitions[j], "index_definition"))), NULL, true);
						definition = strcell(0, 1);
						changed = true;
					}
				}
			}
			foreach(lc, objects)
			{
				ObjectAddress *object = lfirst(lc);
				HeapTuple	tuple;
				Form_pg_constraint con;

				/* A default dropped with a routine it calls, as by CASCADE. */
				if (object->classId == AttrDefaultRelationId)
				{
					ObjectAddress column = GetAttrDefaultColumnAddress(object->objectId);

					if (column.objectId == tableid && !deletion_contains_column(objects, tableid, column.objectSubId))
					{
						if (get_attgenerated(tableid, column.objectSubId))
							unsupported("dropping a generation expression with a routine it calls");
						run("SELECT ($1::jsonb #- ARRAY['columns',($2::int-1)::text,'default'])::text", 2, VC_TYPES(TEXTOID, INT4OID), VC_ARGS(CStringGetTextDatum(definition), Int32GetDatum(column.objectSubId)), NULL, true);
						definition = strcell(0, 1);
						changed = true;
					}
					continue;
				}
				if (object->classId != ConstraintRelationId)
					continue;
				tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(object->objectId));
				con = (Form_pg_constraint) GETSTRUCT(tuple);
				if (con->conrelid == tableid)
				{
					run("SELECT jsonb_set($1::jsonb,'{constraints}',COALESCE((SELECT jsonb_agg(v) FROM jsonb_array_elements($1::jsonb->'constraints') v WHERE v->>0<>$2),'[]'::jsonb))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(NameStr(con->conname))), NULL, true);
					definition = strcell(0, 1);
					if (con->contype == CONSTRAINT_NOTNULL)
					{
						ArrayType  *keys = DatumGetArrayTypeP(SysCacheGetAttrNotNull(CONSTROID, tuple, Anum_pg_constraint_conkey));
						int			att = ((int16 *) ARR_DATA_PTR(keys))[0];

						run("SELECT jsonb_set($1::jsonb,ARRAY['columns',($2::int-1)::text,'notnull'],'false')::text", 2, VC_TYPES(TEXTOID, INT4OID), VC_ARGS(CStringGetTextDatum(definition), Int32GetDatum(att)), NULL, true);
						definition = strcell(0, 1);
					}
					changed = true;
				}
				ReleaseSysCache(tuple);
			}

			/*
			 * Columns go last, as the edits above find columns by position.
			 * Rows hold fields by position too: they are laid out again once
			 * the definition is published.
			 */
			{
				DeletedColumns *columns = palloc0(sizeof(DeletedColumns));
				ArrayBuildState *positions = initArrayResult(INT4OID, CurrentMemoryContext, false);
				JsonbValue	document = json_document(definition);
				JsonbValue	list;

				columns->oldcount = json_get(&document, "columns", &list) ? json_length(&list) : 0;
				columns->mapping = palloc_array(int, Max(columns->oldcount, 1));
				for (int attnum = 1; attnum <= columns->oldcount; attnum++)
				{
					if (deletion_contains_column(objects, tableid, attnum))
						accumArrayResult(positions, Int32GetDatum(attnum), false, INT4OID, CurrentMemoryContext);
					else
						columns->mapping[columns->count++] = attnum - 1;
				}
				if (columns->count < columns->oldcount)
				{
					run("SELECT jsonb_set($1::jsonb,'{columns}',(SELECT coalesce(jsonb_agg(v ORDER BY ord),'[]'::jsonb) FROM jsonb_array_elements($1::jsonb->'columns') WITH ORDINALITY a(v,ord) WHERE ord <> ALL($2::int[])))::text", 2,
						VC_TYPES(TEXTOID, INT4ARRAYOID), VC_ARGS(CStringGetTextDatum(definition), makeArrayResult(positions, CurrentMemoryContext)), NULL, true);
					definition = strcell(0, 1);
					columns->table = names[i];
					deleted_columns = lappend(deleted_columns, columns);
					changed = true;
				}
			}
			if (changed)
			{
				bytea	   *oldschema = child(hashes[i], 1);
				bytea	   *schema = put("schema", definition, NULL, 0);
				bytea	   *content = child(hashes[i], 2);
				Oid			resource = binding_relation(dg_current_branch(), names[i], oldschema);

				run("SELECT jsonb_set($1::jsonb,'{schema}',to_jsonb(encode(sha256(convert_to($2,'UTF8')),'hex')))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(payloads[i]), CStringGetTextDatum(definition)), NULL, true);
				payloads[i] = strcell(0, 1);
				hashes[i] = put("table", payloads[i], VC_ARGS(PointerGetDatum(schema), PointerGetDatum(content)), 2);
				if (!OidIsValid(resource))
					elog(ERROR, "canonical table resource is unavailable during dependency publication");
				binding_record(dg_current_branch(), names[i], schema, resource);
			}
		}
		if (used)
			appendStringInfoChar(&json, ',');
		if (!strcmp(kinds[i], "namespace"))
			appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", payloads[i]);
		else
			appendStringInfoString(&json, payloads[i]);
		children[used++] = PointerGetDatum(hashes[i]);
	}
	appendStringInfoString(&json, "]}");
	run("SELECT jsonb_set($1::jsonb,'{members}',$2::jsonb->'members')::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(json.data)), NULL, true);
	database = canonical_database(strcell(0, 1), children, used);
	(void) provider_view(database);
	move_ref(branch_ref("working"), working, database);
	advance_snapshot();

	/* Installing rows rebuilds the table's surviving indexes. */
	foreach_ptr(DeletedColumns, columns, deleted_columns)
	{
		Relation	rel = dg_catalog_write_table(dg_current_branch(), columns->table, ACL_UPDATE);
		DGState    *state = dg_load(rel, SnapshotSelf);

		dg_history_install(rel, state, alter_rows(state, columns->oldcount, columns->count, columns->mapping, NULL, NULL));
		dg_release(state);
		dg_storage_close_relation(rel);
		UpdateActiveSnapshotCommandId();
	}
	if (deleted_columns)
	{
		move_ref(branch_ref("working"), read_ref(branch_ref("working")), capture(true).hash);
		advance_snapshot();
	}
}

static bool
canonical_drop(Node *statement)
{
	DropStmt   *stmt;
	RangeVar   *constraint_table = NULL;
	bool		pushed = false;
	CatalogView *previous,
			   *view;
	ObjectAddresses *roots;
	List	   *targets;
	ListCell   *lc;
	Oid			user;
	int			security,
				nest;

	/*
	 * Do not acquire a snapshot for unrelated utility commands (notably SET
	 * TRANSACTION, BEGIN and transaction control inside procedures).
	 */
	if (!IsUnderPostmaster || (!IsA(statement, DropStmt) && !IsA(statement, AlterTableStmt)))
		return false;
	if (IsA(statement, AlterTableStmt))
	{
		AlterTableStmt *alter = (AlterTableStmt *) statement;

		AlterTableType subtype;

		if (!alter->cmds)
			return false;
		subtype = linitial_node(AlterTableCmd, alter->cmds)->subtype;
		foreach(lc, alter->cmds)
			if (lfirst_node(AlterTableCmd, lc)->subtype != subtype)
			return false;

		/*
		 * Columns are dropped here only with what depends on them; native
		 * ALTER drops a column alone.
		 */
		if (subtype == AT_DropColumn)
		{
			bool		cascade = false;

			foreach(lc, alter->cmds)
				cascade |= lfirst_node(AlterTableCmd, lc)->behavior == DROP_CASCADE;
			if (!cascade)
				return false;
		}
		else
			/* Native ALTER owns inherited constraint recursion and validation. */
			return false;
		stmt = makeNode(DropStmt);
		stmt->removeType = subtype == AT_DropColumn ? OBJECT_COLUMN : OBJECT_TABCONSTRAINT;
		stmt->objects = alter->cmds;
		stmt->behavior = linitial_node(AlterTableCmd, alter->cmds)->behavior;
		constraint_table = alter->relation;
	}
	else if (IsA(statement, DropStmt))
		stmt = (DropStmt *) statement;
	else
		return false;
	if (stmt->removeType != OBJECT_TABLE && stmt->removeType != OBJECT_INDEX &&
		stmt->removeType != OBJECT_FUNCTION && stmt->removeType != OBJECT_ROUTINE &&
		stmt->removeType != OBJECT_AGGREGATE && stmt->removeType != OBJECT_OPERATOR && stmt->removeType != OBJECT_CAST &&
		stmt->removeType != OBJECT_SCHEMA && stmt->removeType != OBJECT_TYPE &&
		stmt->removeType != OBJECT_TABCONSTRAINT && stmt->removeType != OBJECT_TRIGGER &&
		stmt->removeType != OBJECT_DOMAIN && stmt->removeType != OBJECT_VIEW &&
		stmt->removeType != OBJECT_MATVIEW && stmt->removeType != OBJECT_COLUMN && stmt->removeType != OBJECT_SEQUENCE)
		return false;
	if (!dg_catalog_active())
		return false;

	/* Native objects are unversioned: native DROP removes them. */
	{
		int			versioned = 0,
					native = 0;

		foreach(lc, stmt->objects)
		{
			Oid			id = InvalidOid;
			bool		branch;

			if (constraint_table)
				branch = branch_relation(constraint_table);
			else if (stmt->removeType == OBJECT_TABLE || stmt->removeType == OBJECT_INDEX ||
					 stmt->removeType == OBJECT_VIEW || stmt->removeType == OBJECT_MATVIEW || stmt->removeType == OBJECT_SEQUENCE)
				branch = branch_relation(makeRangeVarFromNameList(lfirst(lc)));
			else if (stmt->removeType == OBJECT_TRIGGER)
				branch = branch_relation(makeRangeVarFromNameList(list_copy_head(lfirst(lc), list_length(lfirst(lc)) - 1)));
			else if (stmt->removeType == OBJECT_SCHEMA)
				branch = true;
			else
			{
				if (stmt->removeType == OBJECT_OPERATOR || stmt->removeType == OBJECT_CAST)
				{
					Relation relation;
					ObjectAddress address = get_object_address(stmt->removeType, lfirst(lc), &relation, AccessExclusiveLock, true);

					id = address.objectId;
					if (relation) relation_close(relation, NoLock);
				}
				else if (stmt->removeType == OBJECT_TYPE || stmt->removeType == OBJECT_DOMAIN)
					id = LookupTypeNameOid(NULL, lfirst_node(TypeName, lc), true);
				else
					id = LookupFuncWithArgs(stmt->removeType, lfirst_node(ObjectWithArgs, lc), true);
				branch = !(id >= FirstNormalObjectId && id < FirstDurableObjectId);
			}
			if (branch)
				versioned++;
			else
				native++;
		}
		if (!versioned)
			return false;
		if (native)
			unsupported("DROP of versioned and unversioned objects together");
	}
	if (constraint_table)
	{
		foreach(lc, stmt->objects)
		{
			if (lfirst_node(AlterTableCmd, lc)->behavior != stmt->behavior)
				unsupported("mixed dependency behaviors in one ALTER TABLE");
		}
	}
	if (stmt->concurrent || CatalogViewCurrent())
		unsupported("concurrent or immutable-context DROP");
	/* SQL functions may own a registered active snapshot. Publication advances
	 * command IDs, so give this utility its own mutable copy. */
	PushCopiedSnapshot(ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot());
	pushed = true;
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	view = dg_catalog_session_view();
	previous = CatalogViewSwitch(view);
	roots = new_object_addresses();
	GetUserIdAndSecContext(&user, &security);
	PG_TRY();
	{
		foreach(lc, stmt->objects)
		{
			ObjectAddress object = {0};
			Oid			namespaceid = InvalidOid,
						owning_table = InvalidOid;

			if (stmt->removeType == OBJECT_COLUMN)
			{
				AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

				if (!dg_catalog_lookup_relation(constraint_table, false, &owning_table))
					unsupported("column deletion on a shared relation");
				namespaceid = get_rel_namespace(owning_table);
				object.classId = RelationRelationId;
				object.objectSubId = get_attnum(owning_table, cmd->name);
				if (object.objectSubId == InvalidAttrNumber)
				{
					if (!cmd->missing_ok)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column \"%s\" of relation \"%s\" does not exist",
										cmd->name, get_rel_name(owning_table))));
					ereport(NOTICE,
							(errmsg("column \"%s\" of relation \"%s\" does not exist, skipping",
									cmd->name, get_rel_name(owning_table))));
					continue;
				}
				object.objectId = owning_table;
			}
			else if (stmt->removeType == OBJECT_TABCONSTRAINT)
			{
				AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

				if (!dg_catalog_lookup_relation(constraint_table, false, &owning_table))
					unsupported("constraint deletion on a shared relation");
				namespaceid = get_rel_namespace(owning_table);
				object.classId = ConstraintRelationId;
				object.objectId = get_relation_constraint_oid(owning_table, cmd->name, cmd->missing_ok);
				if (OidIsValid(object.objectId))
				{
					HeapTuple	constraint = SearchSysCache1(CONSTROID, ObjectIdGetDatum(object.objectId));

					if (((Form_pg_constraint) GETSTRUCT(constraint))->contype == CONSTRAINT_NOTNULL)
					{
						ArrayType  *keys = DatumGetArrayTypeP(SysCacheGetAttrNotNull(CONSTROID, constraint, Anum_pg_constraint_conkey));
						Relation	rel = relation_open(owning_table, NoLock);
						HeapTuple	attribute = CheckDropNotNullConstraint(rel, ((int16 *) ARR_DATA_PTR(keys))[0]);

						heap_freetuple(attribute);
						relation_close(rel, NoLock);
					}
					ReleaseSysCache(constraint);
				}
			}
			else if (stmt->removeType == OBJECT_TABLE || stmt->removeType == OBJECT_INDEX ||
					 stmt->removeType == OBJECT_VIEW || stmt->removeType == OBJECT_MATVIEW || stmt->removeType == OBJECT_SEQUENCE)
			{
				RangeVar   *rv = makeRangeVarFromNameList(lfirst(lc));

				if (!dg_catalog_lookup_relation(rv, stmt->missing_ok, &object.objectId))
					unsupported("DROP of a shared relation in an activated database");
				object.classId = RelationRelationId;
				if (OidIsValid(object.objectId))
				{
					char		kind = get_rel_relkind(object.objectId);

					if (kind != (stmt->removeType == OBJECT_INDEX ? RELKIND_INDEX : stmt->removeType == OBJECT_VIEW ? RELKIND_VIEW :
								 stmt->removeType == OBJECT_MATVIEW ? RELKIND_MATVIEW : stmt->removeType == OBJECT_SEQUENCE ? RELKIND_SEQUENCE : RELKIND_RELATION))
						ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("wrong relation kind for DROP")));
					namespaceid = get_rel_namespace(object.objectId);
				}
			}
			else if (stmt->removeType == OBJECT_TRIGGER)
			{
				List	   *names = lfirst(lc);
				RangeVar   *rv = makeRangeVarFromNameList(list_copy_head(names, list_length(names) - 1));

				if (!dg_catalog_lookup_relation(rv, stmt->missing_ok, &owning_table))
					unsupported("trigger deletion on a shared relation");
				object.classId = TriggerRelationId;
				if (OidIsValid(owning_table))
				{
					object.objectId = get_trigger_oid(owning_table, strVal(llast(names)), stmt->missing_ok);
					namespaceid = get_rel_namespace(owning_table);
				}
			}
			else if (stmt->removeType == OBJECT_OPERATOR || stmt->removeType == OBJECT_CAST)
			{
				Relation relation;

				object = get_object_address(stmt->removeType, lfirst(lc), &relation, AccessExclusiveLock, stmt->missing_ok);
				if (relation) relation_close(relation, NoLock);
			}
			else if (stmt->removeType == OBJECT_SCHEMA)
			{
				object.classId = NamespaceRelationId;
				object.objectId = get_namespace_oid(strVal(lfirst(lc)), stmt->missing_ok);
			}
			else if (stmt->removeType == OBJECT_TYPE || stmt->removeType == OBJECT_DOMAIN)
			{
				object.classId = TypeRelationId;
				object.objectId = LookupTypeNameOid(NULL, lfirst_node(TypeName, lc), stmt->missing_ok);
			}
			else
			{
				object.classId = ProcedureRelationId;
				object.objectId = LookupFuncWithArgs(stmt->removeType, lfirst_node(ObjectWithArgs, lc), stmt->missing_ok);
				if (OidIsValid(object.objectId))
					namespaceid = get_func_namespace(object.objectId);
			}
			if (!OidIsValid(object.objectId))
				continue;
			if (CatalogViewObjectContext(object.objectId) != view)
				unsupported("DROP of a noncanonical object");
			if (!(OidIsValid(owning_table) ? object_ownercheck(RelationRelationId, owning_table, user) : object_ownercheck(object.classId, object.objectId, user)) &&
				!(OidIsValid(namespaceid) && object_ownercheck(NamespaceRelationId, namespaceid, user)))
				aclcheck_error(ACLCHECK_NOT_OWNER, stmt->removeType,
							   CatalogViewObjectIdentity(object.objectId, object.classId));
			add_exact_object_address(&object, roots);
		}
		targets = collect_catalog_deletions(roots, stmt->behavior);
		foreach(lc, targets)
		{
			ObjectAddress *object = lfirst(lc);
			Oid			relation = InvalidOid;

			/* A global sequence survives the branch's table it serves. */
			if (object->classId == RelationRelationId && !CatalogViewObjectContext(object->objectId) &&
				get_rel_relkind(object->objectId) == RELKIND_SEQUENCE)
			{
				targets = foreach_delete_current(targets, lc);
				continue;
			}
			if (object->classId == RelationRelationId)
				relation = object->objectId;
			else if (object->classId == ConstraintRelationId)
			{
				HeapTuple	tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(object->objectId));

				relation = ((Form_pg_constraint) GETSTRUCT(tuple))->conrelid;
				ReleaseSysCache(tuple);
			}
			if (OidIsValid(relation) && AfterTriggerPendingOnRel(relation))
				ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("cannot alter a relation with pending trigger events")));

			/* A column goes with its relation, or alone from a table. */
			if (object->classId == RelationRelationId && object->objectSubId != 0 &&
				CatalogViewObjectContext(object->objectId) == view)
			{
				if (deletion_contains(targets, RelationRelationId, object->objectId))
				{
					targets = foreach_delete_current(targets, lc);
					continue;
				}
				if (get_rel_relkind(object->objectId) == RELKIND_RELATION)
					continue;
			}
			if (CatalogViewObjectContext(object->objectId) != view || object->objectSubId != 0 ||
				(object->classId != RelationRelationId && object->classId != NamespaceRelationId &&
				 object->classId != ProcedureRelationId && object->classId != TypeRelationId &&
				 object->classId != ConstraintRelationId && object->classId != TriggerRelationId &&
				 object->classId != AttrDefaultRelationId && object->classId != RewriteRelationId &&
				 object->classId != CastRelationId && object->classId != OperatorRelationId))
				unsupported("canonical dependency deletion object kind");
		}
		CatalogViewSwitch(NULL);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		run("SELECT 1 FROM pg_event_trigger WHERE evtenabled<>'D'", 0, NULL, NULL, NULL, true);
		if (SPI_processed)
			unsupported("canonical DROP with event triggers");
		if (targets)
			publish_deletions(targets, view);
		end_api(nest);
		UpdateActiveSnapshotCommandId();
		free_object_addresses(roots);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		CatalogViewSwitch(previous);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
	return true;
}
