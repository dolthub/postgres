/* Native DDL mutates only a provisional catalog view. Publication stores
 * complete portable tuples; no physical user catalog row is installed. */
bool
dg_catalog_schema_object_ddl(ParseState *pstate, Node *statement)
{
	AlterOwnerStmt *owner = IsA(statement, AlterOwnerStmt) ? (AlterOwnerStmt *) statement : NULL;
	DefineStmt *define = IsA(statement, DefineStmt) ? (DefineStmt *) statement : NULL;
	DropStmt   *drop = IsA(statement, DropStmt) ? (DropStmt *) statement : NULL;
	CatalogView *previous;
	CatalogView *view;
	Oid caller;
	int security;
	bool pushed = false;
	const Oid catalogs[] = {AggregateRelationId, OperatorRelationId, CastRelationId};
	const char *kinds[] = {"aggregate", "operator", "cast"};
	List *definitions[3] = {NIL, NIL, NIL};

	/*
	 * Dropping an operator clears its commutator's and negator's links to it,
	 * which rewrites their definitions too. A cascade reaches other kinds of
	 * objects and takes the general path.
	 */
	if (drop && (drop->removeType != OBJECT_OPERATOR || drop->behavior == DROP_CASCADE))
		drop = NULL;
	if ((!define || (define->kind != OBJECT_AGGREGATE && define->kind != OBJECT_OPERATOR)) &&
		!IsA(statement, CreateCastStmt) && !IsA(statement, AlterOperatorStmt) && !drop &&
		(!owner || (owner->objectType != OBJECT_AGGREGATE && owner->objectType != OBJECT_OPERATOR)))
		return false;
	if (creating_extension)
		return false;
	if (define)
	{
		char *name;
		Oid namespaceid = QualifiedNameGetCreationNamespace(define->defnames, &name);

		if (isAnyTempNamespace(namespaceid) || dg_system_namespace(get_namespace_name(namespaceid)))
			return false;
	}
	if (!dg_catalog_active())
	{
		if (!default_table_access_method || strcmp(default_table_access_method, "dolt_prolly_table"))
			return false;
		catalog_activate();
	}
	if (define)
	{
		char *name;

		adopt_schema(QualifiedNameGetCreationNamespace(define->defnames, &name));
	}
	if (CatalogViewCurrent())
		unsupported("schema DDL in an immutable revision context");
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	previous = CatalogViewCurrent();
	view = CatalogViewCreate("provisional schema object DDL");
	GetUserIdAndSecContext(&caller, &security);
	PG_TRY();
	{
		Capture current;
		int nest;

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		run("SELECT 1 FROM pg_event_trigger WHERE evtenabled<>'D' AND evtevent<>'login'", 0, NULL, NULL, NULL, true);
		if (SPI_processed)
			unsupported("canonical schema DDL with event triggers");
		current = capture(true);
		provider_populate(view, current.hash, NULL);
		end_api(nest);
		CatalogViewBeginMutation(view);
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		if (define && define->kind == OBJECT_AGGREGATE)
			DefineAggregate(pstate, define->defnames, define->args, define->oldstyle, define->definition, define->replace);
		else if (define)
			DefineOperator(define->defnames, define->definition);
		else if (IsA(statement, CreateCastStmt))
			CreateCast((CreateCastStmt *) statement);
		else if (owner)
			ExecAlterOwnerStmt(owner);
		else if (drop)
			RemoveObjects(drop);
		else
			AlterOperator((AlterOperatorStmt *) statement);
		CommandCounterIncrement();
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		for (int k = 0; k < lengthof(catalogs); k++)
		{
			Relation catalog = table_open(catalogs[k], AccessShareLock);
			SysScanDesc scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
			HeapTuple tuple;

			while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			{
				bool missing;
				Oid id = DatumGetObjectId(heap_getattr(tuple, 1, RelationGetDescr(catalog), &missing));

				if (CatalogViewObjectContext(id) == view)
					definitions[k] = lappend(definitions[k], schema_object_encode(catalogs[k], tuple, RelationGetDescr(catalog)));
			}
			systable_endscan(scan);
			table_close(catalog, AccessShareLock);
		}
		CatalogViewSwitch(NULL);
		/* Publish referenced operators before aggregate sort operators. All
		 * three updates share the statement's transaction and commit decision. */
		for (int k = 2; k >= 0; k--)
			objects_publish(kinds[k], NIL, NULL, true, definitions[k]);
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
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
	return true;
}
