/*
 * Versioned user-defined types. Type DDL in a versioned database runs natively
 * against a provisional copy of the branch's catalogs; every user type there
 * is then encoded again and published. Unchanged types encode to the same
 * bytes.
 */

/* Whether a type DDL statement creates a type in a versioned schema. */
static bool
creates_versioned_type(Node *statement, Oid *namespaceid)
{
	Oid			namespace;
	char	   *name;

	if (IsA(statement, CreateEnumStmt))
		namespace = QualifiedNameGetCreationNamespace(((CreateEnumStmt *) statement)->typeName, &name);
	else if (IsA(statement, CreateDomainStmt))
		namespace = QualifiedNameGetCreationNamespace(((CreateDomainStmt *) statement)->domainname, &name);
	else if (IsA(statement, CompositeTypeStmt))
		namespace = RangeVarGetCreationNamespace(((CompositeTypeStmt *) statement)->typevar);
	else if (IsA(statement, CreateRangeStmt))
		namespace = QualifiedNameGetCreationNamespace(((CreateRangeStmt *) statement)->typeName, &name);
	else
		return false;
	*namespaceid = namespace;
	return !isAnyTempNamespace(namespace) && !dg_system_namespace(get_namespace_name(namespace));
}

/* The type ALTER TYPE names with |relation|, as ALTER TABLE names a relation. */
static Oid
type_relation_type(RangeVar *relation, bool missing_ok)
{
	List	   *names = relation->schemaname ?
		list_make2(makeString(relation->schemaname), makeString(relation->relname)) :
		list_make1(makeString(relation->relname));

	return LookupTypeNameOid(NULL, makeTypeNameFromNameList(names), missing_ok);
}

/* Whether a type DDL statement changes a versioned type. */
static bool
alters_versioned_type(Node *statement)
{
	Oid			type = InvalidOid;

	if (IsA(statement, AlterTableStmt) && ((AlterTableStmt *) statement)->objtype == OBJECT_TYPE)
		type = type_relation_type(((AlterTableStmt *) statement)->relation, true);
	else if (IsA(statement, AlterEnumStmt))
		type = typenameTypeId(NULL, makeTypeNameFromNameList(((AlterEnumStmt *) statement)->typeName));
	else if (IsA(statement, AlterDomainStmt))
		type = typenameTypeId(NULL, makeTypeNameFromNameList(((AlterDomainStmt *) statement)->typeName));
	return type >= FirstDurableObjectId;
}

/*
 * Whether a pg_type row is a versioned type of its own, not a derived array or
 * row type, nor a server type listed beside the branch's.
 */
static bool
type_versioned_row(Form_pg_type type)
{
	if (type->oid < FirstDurableObjectId)
		return false;
	if (type->typtype == TYPTYPE_ENUM || type->typtype == TYPTYPE_DOMAIN || type->typtype == TYPTYPE_RANGE)
		return true;
	return type->typtype == TYPTYPE_COMPOSITE && get_rel_relkind(type->typrelid) == RELKIND_COMPOSITE_TYPE;
}

/*
 * The tables of database root |root| that can hold a value of type |type|,
 * directly or through domains, arrays and composite types, as its dependency
 * record lists them.
 */
static List *
type_tables(const char *root, const char *type)
{
	List	   *types = list_make1(pstrdup(type));
	List	   *tables = NIL;

	foreach_ptr(char, name, types)
	{
		foreach_ptr(List, referrer, database_referrers(root, "type", name))
		{
			List	  **into = !strcmp(linitial(referrer), "type") ? &types :
				!strcmp(linitial(referrer), "table") ? &tables : NULL;
			bool		listed = false;

			if (!into)
				continue;
			foreach_ptr(char, member, *into)
				listed |= !strcmp(member, lsecond(referrer));
			if (!listed)
				*into = lappend(*into, lsecond(referrer));
		}
	}
	return tables;
}

/*
 * A change to user type |name| that stored values follow: an enum's label
 * |old| renamed to |renamed|, or a composite type's fields laid out again,
 * new field i being old field |mapping|[i], or absent if that is -1.
 */
typedef struct TypeRecode
{
	const char *name;
	const char *old;
	const char *renamed;
	int			count;
	int		   *mapping;
	TupleDesc	desc;			/* of the table being rewritten */
	Oid			type;			/* the type, in its catalogs */
} TypeRecode;

static PLSlice
type_recode_field(int field, PLSlice bytes, void *arg)
{
	TypeRecode *recode = arg;
	Oid			type = TupleDescAttr(recode->desc, field)->atttypid;

	if (recode->mapping)
		return dg_recode_composite(type, bytes, recode->type, recode->count, recode->mapping);
	return dg_recode_label(type, bytes, recode->type, recode->old, recode->renamed);
}

/*
 * Install the rows of every table of database root |root| that can hold a
 * value of type |type| again on the working branch, changed as |recode| says
 * if given. Rows hold enum labels, and index keys label positions, which an
 * added label shifts; installing rows rebuilds their indexes.
 */
static void
type_tables_rewrite(const char *root, const char *type, TypeRecode *recode)
{
	List	   *tables = type_tables(root, type);

	foreach_ptr(char, table, tables)
	{
		Relation	rel = dg_catalog_write_table(dg_current_branch(), table, ACL_UPDATE);
		DGState    *state = dg_load(rel, SnapshotSelf);
		TupleDesc	desc = RelationGetDescr(rel);
		PLTree		tree = state->content;

		if (recode)
		{
			int		   *mapping = palloc_array(int, Max(desc->natts, 1));
			CatalogView *previous = CatalogViewSwitch(rel->rd_catalogview);

			for (int i = 0; i < desc->natts; i++)
				mapping[i] = i;
			recode->desc = desc;
			recode->type = CatalogViewLookupObject(rel->rd_catalogview, TypeRelationId, type);
			PG_TRY();
			{
				tree = alter_rows(state, desc->natts, desc->natts, mapping, type_recode_field, recode);
			}
			PG_FINALLY();
			{
				CatalogViewSwitch(previous);
			}
			PG_END_TRY();
		}
		dg_history_install(rel, state, tree);
		dg_release(state);
		dg_storage_close_relation(rel);
		UpdateActiveSnapshotCommandId();
	}
	if (tables)
		move_ref(branch_ref("working"), read_ref(branch_ref("working")), capture(true).hash);
	advance_snapshot();
}

/*
 * The attribute numbers of composite type relation |relid|'s attributes, in
 * the order its definition lists them: dropped ones are not encoded.
 */
static List *
type_attribute_numbers(Oid relid)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	List	   *numbers = NIL;

	for (int i = 0; i < RelationGetDescr(rel)->natts; i++)
		if (!TupleDescAttr(RelationGetDescr(rel), i)->attisdropped)
			numbers = lappend_int(numbers, TupleDescAttr(RelationGetDescr(rel), i)->attnum);
	relation_close(rel, AccessShareLock);
	return numbers;
}

/* Type DDL makes no storage; a composite type's relation has none. */
static void
type_storage(Relation relation, CatalogViewStorageEffect effect)
{
	if (relation->rd_rel->relkind != RELKIND_COMPOSITE_TYPE)
		unsupported("storage effects of type DDL");
}

static void
type_ddl(ParseState *pstate, Node *statement)
{
	CatalogView *view = CatalogViewCreate("provisional type DDL");
	CatalogView *previous = CatalogViewCurrent();
	CatalogView *saved_mutation = native_mutation_view;
	Oid			altered = InvalidOid;
	List	   *before = NIL,
			   *after = NIL;
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
		provider_populate(view, current.hash, NULL);
		end_api(nest);
		CatalogViewBeginMutation(view);
		CatalogViewSetStorageHandler(view, type_storage);

		/* Domain constraints validate against the branch's existing rows. */
		native_mutation_view = view;
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		switch (nodeTag(statement))
		{
			case T_CreateEnumStmt:
				DefineEnum((CreateEnumStmt *) statement);
				break;
			case T_CreateDomainStmt:
				DefineDomain(pstate, (CreateDomainStmt *) statement);
				break;
			case T_CompositeTypeStmt:
				DefineCompositeType(((CompositeTypeStmt *) statement)->typevar,
									((CompositeTypeStmt *) statement)->coldeflist);
				break;
			case T_CreateRangeStmt:
				DefineRange(pstate, (CreateRangeStmt *) statement);
				break;
			case T_AlterEnumStmt:
				altered = typenameTypeId(NULL, makeTypeNameFromNameList(((AlterEnumStmt *) statement)->typeName));
				AlterEnum((AlterEnumStmt *) statement);
				break;
			case T_AlterDomainStmt:
				{
					AlterDomainStmt *stmt = (AlterDomainStmt *) statement;

					switch (stmt->subtype)
					{
						case 'T':
							AlterDomainDefault(stmt->typeName, stmt->def);
							break;
						case 'N':
							AlterDomainNotNull(stmt->typeName, false);
							break;
						case 'O':
							AlterDomainNotNull(stmt->typeName, true);
							break;
						case 'C':
							AlterDomainAddConstraint(stmt->typeName, stmt->def, NULL, false);
							break;
						case 'X':
							AlterDomainDropConstraint(stmt->typeName, stmt->name, stmt->behavior, stmt->missing_ok);
							break;
						case 'V':
							AlterDomainValidateConstraint(stmt->typeName, stmt->name);
							break;
						default:
							elog(ERROR, "unrecognized ALTER DOMAIN subtype %c", stmt->subtype);
					}
					break;
				}
			case T_AlterTableStmt:
				{
					/* ALTER TYPE ... ATTRIBUTE, as ProcessUtilitySlow runs it. */
					AlterTableStmt *stmt = (AlterTableStmt *) statement;
					AlterTableUtilityContext context = {0};
					PlannedStmt *plan = makeNode(PlannedStmt);
					LOCKMODE	lockmode = AlterTableGetLockLevel(stmt->cmds);

					altered = type_relation_type(stmt->relation, false);
					before = type_attribute_numbers(typeidTypeRelid(altered));
					plan->commandType = CMD_UTILITY;
					plan->utilityStmt = statement;
					context.pstmt = plan;
					context.queryString = pstate->p_sourcetext;
					context.relid = AlterTableLookupRelation(stmt, lockmode);
					AlterTable(stmt, lockmode, &context);
					CommandCounterIncrement();
					after = type_attribute_numbers(typeidTypeRelid(altered));
					break;
				}
			default:
				elog(ERROR, "unrecognized versioned type statement");
		}
		CommandCounterIncrement();
		GetUserIdAndSecContext(&privileged, &security);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		catalog = table_open(TypeRelationId, AccessShareLock);
		scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			if (type_versioned_row((Form_pg_type) GETSTRUCT(tuple)))
				definitions = lappend(definitions, type_encode(tuple, RelationGetDescr(catalog)));
		systable_endscan(scan);
		table_close(catalog, AccessShareLock);
		CatalogViewSwitch(NULL);
		{
			bytea	   *database = objects_publish("type", NIL, NULL, true, definitions);

			/* An added label shifts the positions that index keys hold. */
			if (IsA(statement, AlterEnumStmt))
				type_tables_rewrite(metadata(database, "database"),
									CatalogViewObjectIdentity(altered, TypeRelationId), NULL);

			/* Stored composite values hold fields by position. */
			if (IsA(statement, AlterTableStmt) && !equal(before, after))
			{
				TypeRecode	recode = {
					.count = list_length(after),
					.mapping = palloc_array(int, Max(list_length(after), 1)),
				};

				foreach_int(attnum, after)
				{
					recode.mapping[foreach_current_index(attnum)] = -1;
					foreach_int(old, before)
						if (old == attnum)
						recode.mapping[foreach_current_index(attnum)] = foreach_current_index(old);
				}
				type_tables_rewrite(metadata(database, "database"),
									CatalogViewObjectIdentity(altered, TypeRelationId), &recode);
			}
		}
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
dg_catalog_type_ddl(ParseState *pstate, Node *statement)
{
	bool		pushed = false;
	bool		creating;
	Oid			namespaceid = InvalidOid;

	if (!IsA(statement, CreateEnumStmt) && !IsA(statement, CreateDomainStmt) &&
		!IsA(statement, CompositeTypeStmt) && !IsA(statement, CreateRangeStmt) && !IsA(statement, AlterEnumStmt) &&
		!IsA(statement, AlterDomainStmt) && !IsA(statement, AlterTableStmt))
		return false;
	creating = creates_versioned_type(statement, &namespaceid);
	if (!dg_catalog_active())
	{
		if (!creating || !default_table_access_method || strcmp(default_table_access_method, "dolt_prolly_table"))
			return false;
		catalog_activate();
	}
	if (!creating && !alters_versioned_type(statement))
		return false;
	if (creating)
		adopt_schema(namespaceid);
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	if (CatalogViewCurrent())
		unsupported("type DDL in an immutable revision context");
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	type_ddl(pstate, statement);
	if (pushed)
		PopActiveSnapshot();
	return true;
}
