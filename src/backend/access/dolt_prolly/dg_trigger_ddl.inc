/*
 * Versioned triggers. A trigger is a schema member of its own, apart from its
 * table, so that adding one does not change the table's definition. Its
 * portable form is the CREATE TRIGGER statement PostgreSQL deparses for it,
 * with every name qualified, plus its enabled state. A constraint trigger's
 * statement also defines its constraint.
 */
static char *
trigger_encode(HeapTuple tuple, TupleDesc desc)
{
	Form_pg_trigger trigger = (Form_pg_trigger) GETSTRUCT(tuple);
	char	   *table = dg_qualified_name(get_namespace_name(get_rel_namespace(trigger->tgrelid)),
										  get_rel_name(trigger->tgrelid));
	char	   *sql = NULL;
	bool		isnull;
	Datum		qual = heap_getattr(tuple, Anum_pg_trigger_tgqual, desc, &isnull);
	int			nest = NewGUCNestLevel();
	StringInfoData json;
	char		enabled[2] = {trigger->tgenabled, '\0'};

	/* A foreign key's triggers are derived from it; a constraint trigger is a member. */
	if (trigger->tgisinternal)
		unsupported("versioned internal triggers");
	if (OidIsValid(trigger->tgconstrrelid) && trigger->tgconstrrelid < FirstDurableObjectId)
		unsupported("versioned constraint trigger referring to an unversioned table");
	if (trigger->tgfoid >= FirstNormalObjectId && trigger->tgfoid < FirstDurableObjectId)
		unsupported("versioned trigger calling an unversioned function");
	if (!isnull && expression_calls_native(stringToNode(TextDatumGetCString(qual)), NULL))
		unsupported("versioned trigger condition calling an unversioned function");
	PG_TRY();
	{
		schema_expression_gucs();
		sql = TextDatumGetCString(DirectFunctionCall1(pg_get_triggerdef, ObjectIdGetDatum(trigger->oid)));
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGTRIGGER1\",");
	json_field(&json, "name", psprintf("%s/%s", table, NameStr(trigger->tgname)), false);
	appendStringInfoChar(&json, ',');
	json_field(&json, "table", table, false);
	appendStringInfoChar(&json, ',');
	json_field(&json, "enabled", enabled, false);
	appendStringInfoChar(&json, ',');
	json_field(&json, "definition", sql, false);
	json_comment(&json, trigger->oid, TriggerRelationId);
	appendStringInfoChar(&json, '}');
	return json.data;
}

/* Whether |cmd| enables or disables triggers, as ALTER TABLE ... TRIGGER does. */
static bool
trigger_alter_action(AlterTableCmd *cmd)
{
	switch (cmd->subtype)
	{
		case AT_EnableTrig:
		case AT_EnableAlwaysTrig:
		case AT_EnableReplicaTrig:
		case AT_EnableTrigAll:
		case AT_EnableTrigUser:
		case AT_DisableTrig:
		case AT_DisableTrigAll:
		case AT_DisableTrigUser:
			return true;
		default:
			return false;
	}
}

/* As ATExecCmd, for one ALTER TABLE ... ENABLE or DISABLE TRIGGER action. */
static void
trigger_enable(Relation rel, AlterTableCmd *cmd)
{
	char		fires_when = TRIGGER_FIRES_ON_ORIGIN;
	bool		skip_system = false;
	const char *name = NULL;

	switch (cmd->subtype)
	{
		case AT_EnableTrig:
			name = cmd->name;
			break;
		case AT_EnableAlwaysTrig:
			name = cmd->name;
			fires_when = TRIGGER_FIRES_ALWAYS;
			break;
		case AT_EnableReplicaTrig:
			name = cmd->name;
			fires_when = TRIGGER_FIRES_ON_REPLICA;
			break;
		case AT_DisableTrig:
			name = cmd->name;
			fires_when = TRIGGER_DISABLED;
			break;
		case AT_EnableTrigAll:
			break;
		case AT_DisableTrigAll:
			fires_when = TRIGGER_DISABLED;
			break;
		case AT_EnableTrigUser:
			skip_system = true;
			break;
		case AT_DisableTrigUser:
			fires_when = TRIGGER_DISABLED;
			skip_system = true;
			break;
		default:
			elog(ERROR, "unrecognized trigger action %d", cmd->subtype);
	}

	/* Internal triggers, such as a foreign key's, are derived and keep no state. */
	if (!name && !skip_system)
	{
		Relation	catalog = table_open(TriggerRelationId, AccessShareLock);
		ScanKeyData key;
		SysScanDesc scan;
		HeapTuple	tuple;

		ScanKeyInit(&key, Anum_pg_trigger_tgrelid, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationGetRelid(rel)));
		scan = systable_beginscan(catalog, TriggerRelidNameIndexId, true, NULL, 1, &key);
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			if (((Form_pg_trigger) GETSTRUCT(tuple))->tgisinternal)
				unsupported("enabling or disabling a versioned table's internal triggers");
		systable_endscan(scan);
		table_close(catalog, AccessShareLock);
	}
	EnableDisableTrigger(rel, name, InvalidOid, fires_when, skip_system, false, ShareRowExclusiveLock);
}

/*
 * CREATE TRIGGER, and ALTER TABLE ... ENABLE or DISABLE TRIGGER, on a branch
 * table run natively against a provisional copy of the branch's catalogs. The
 * table's triggers there are then encoded and replace its trigger members.
 */
static void
trigger_ddl(ParseState *pstate, Node *statement, RangeVar *relation)
{
	CatalogView *view = CatalogViewCreate("provisional trigger DDL");
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
		ScanKeyData key;
		Oid			relid;
		char	   *table;

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		current = capture(true);
		provider_populate(view, current.hash, NULL);
		end_api(nest);
		CatalogViewBeginMutation(view);
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		if (IsA(statement, CreateTrigStmt))
			(void) CreateTrigger((CreateTrigStmt *) statement, pstate->p_sourcetext, InvalidOid, InvalidOid,
								 InvalidOid, InvalidOid, InvalidOid, InvalidOid, NULL, false, false);
		else
		{
			Relation	rel = table_openrv(relation, ShareRowExclusiveLock);

			if (!object_ownercheck(RelationRelationId, RelationGetRelid(rel), GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(rel->rd_rel->relkind),
							   RelationGetRelationName(rel));
			foreach_node(AlterTableCmd, cmd, ((AlterTableStmt *) statement)->cmds)
			{
				trigger_enable(rel, cmd);
				CommandCounterIncrement();
			}
			table_close(rel, NoLock);
		}
		CommandCounterIncrement();
		relid = RangeVarGetRelid(relation, NoLock, false);
		GetUserIdAndSecContext(&privileged, &security);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		catalog = table_open(TriggerRelationId, AccessShareLock);
		ScanKeyInit(&key, Anum_pg_trigger_tgrelid, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));
		scan = systable_beginscan(catalog, TriggerRelidNameIndexId, true, NULL, 1, &key);
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			if (!((Form_pg_trigger) GETSTRUCT(tuple))->tgisinternal)
				definitions = lappend(definitions, trigger_encode(tuple, RelationGetDescr(catalog)));
		systable_endscan(scan);
		table = dg_qualified_name(get_namespace_name(get_rel_namespace(relid)), get_rel_name(relid));
		table_close(catalog, AccessShareLock);
		CatalogViewSwitch(NULL);
		(void) objects_publish("trigger", NIL, psprintf("%s/", table), false, definitions);
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
dg_catalog_trigger_ddl(ParseState *pstate, Node *statement)
{
	RangeVar   *relation;
	Oid			relid;
	bool		pushed = false;

	if (IsA(statement, CreateTrigStmt))
		relation = ((CreateTrigStmt *) statement)->relation;
	else if (IsA(statement, AlterTableStmt) && ((AlterTableStmt *) statement)->objtype == OBJECT_TABLE &&
			 ((AlterTableStmt *) statement)->cmds &&
			 trigger_alter_action(linitial_node(AlterTableCmd, ((AlterTableStmt *) statement)->cmds)))
	{
		relation = ((AlterTableStmt *) statement)->relation;
		foreach_node(AlterTableCmd, cmd, ((AlterTableStmt *) statement)->cmds)
			if (!trigger_alter_action(cmd))
				unsupported("ALTER TABLE combining trigger and other actions");
	}
	else
		return false;
	if (!dg_catalog_active() || !branch_relation(relation))
		return false;
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	if (CatalogViewCurrent())
		unsupported("trigger DDL in an immutable revision context");

	/* As CreateTrigger and ALTER TABLE, which conflict with concurrent writes to the table. */
	relid = RangeVarGetRelid(relation, ShareRowExclusiveLock, false);

	/* As pg_constraint's unique index: a constraint trigger's constraint shares its table's names. */
	if (IsA(statement, CreateTrigStmt) && ((CreateTrigStmt *) statement)->isconstraint &&
		OidIsValid(get_relation_constraint_oid(relid, ((CreateTrigStmt *) statement)->trigname, true)))
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("duplicate key value violates unique constraint \"%s\"", "pg_constraint_conrelid_contypid_conname_index")));
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	trigger_ddl(pstate, statement, relation);
	if (pushed)
		PopActiveSnapshot();
	return true;
}
