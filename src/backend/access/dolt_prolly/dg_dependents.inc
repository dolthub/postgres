/*
 * The dependency record of a database root. Definitions refer to the objects
 * they use by qualified name; a root keeps, for each object referred to, the
 * members that refer to it:
 *
 *   "dependents": {"routine:public.f": [["table","public.t"], ...], ...}
 *
 * so that a rename rewrites exactly the definitions that name the renamed
 * object. References are derived from definitions alone, so the record is
 * independent of history; they are cached by definition hash, as
 * definitions are immutable. Routines are referred to by name without their
 * argument types, which covers every overload; a table's row type is
 * referred to as "type:" and the table's name. Operators outside pg_catalog
 * are referred to as "operator:" and their qualified name.
 */

typedef struct ReferenceCacheEntry
{
	char		key[PL_HASH_SIZE * 2 + 1];
	List	   *references;		/* of "kind:name" strings */
} ReferenceCacheEntry;

static HTAB *reference_cache;

static void
references_add(List **references, const char *kind, const char *name)
{
	char	   *reference = psprintf("%s:%s", kind, name);

	if (!list_member(*references, makeString(reference)))
		*references = lappend(*references, makeString(reference));
}

/* A name list names a versioned object unless it is unqualified or in pg_catalog. */
static char *
references_name(List *names)
{
	char	   *schema,
			   *name;

	DeconstructQualifiedName(names, &schema, &name);
	if (!schema || !strcmp(schema, "pg_catalog"))
		return NULL;
	return dg_qualified_name(schema, name);
}

static bool
references_walker(Node *node, List **references)
{
	char	   *name;

	if (!node)
		return false;
	if (IsA(node, FuncCall) && (name = references_name(((FuncCall *) node)->funcname)))
		references_add(references, "routine", name);
	if (IsA(node, TypeName) && (name = references_name(((TypeName *) node)->names)))
		references_add(references, "type", name);
	/* An operator outside pg_catalog is written OPERATOR(schema.op). */
	if (IsA(node, A_Expr) && (name = references_name(((A_Expr *) node)->name)))
		references_add(references, "operator", name);
	if (IsA(node, TypeCast))
	{
		TypeCast   *cast = (TypeCast *) node;

		if (cast->typeName->names && !strcmp(strVal(llast(cast->typeName->names)), "regclass") && IsA(cast->arg, A_Const))
		{
			A_Const    *value = (A_Const *) cast->arg;

			if (!value->isnull && IsA(&value->val, String))
			{
				List	   *parts = stringToQualifiedNameList(strVal(&value->val), NULL);

				if (list_length(parts) == 2 && (name = references_name(parts)))
					references_add(references, "table", name);
			}
		}
	}
	return raw_expression_tree_walker(node, references_walker, references);
}

static void
references_sql(List **references, const char *sql)
{
	if (sql)
		(void) references_walker(schema_expression_raw(sql), references);
}

/* A type's portable name, as routine_type_name writes it. */
static void
references_type(List **references, const char *type)
{
	char	   *name;
	Size		len;

	if (!type || !strchr(type, '.'))
		return;
	name = pstrdup(type);
	while ((len = strlen(name)) > 2 && !strcmp(name + len - 2, "[]"))
		name[len - 2] = '\0';
	if (strncmp(name, "pg_catalog.", strlen("pg_catalog.")))
		references_add(references, "type", name);
}

static void
references_type_array(List **references, const char *array)
{
	ArrayType  *values;
	Datum	   *names;
	bool	   *missing;
	int			n;

	if (!array)
		return;
	values = DatumGetArrayTypeP(OidInputFunctionCall(F_ARRAY_IN, (char *) array, TEXTOID, -1));
	deconstruct_array(values, TEXTOID, -1, false, TYPALIGN_INT, &names, &missing, &n);
	for (int i = 0; i < n; i++)
		if (!missing[i])
			references_type(references, TextDatumGetCString(names[i]));
}

/* The statement a constraint definition is parsed in. */
static Constraint *
references_constraint(const char *prefix, const char *name, const char *definition)
{
	List	   *parsed = schema_expression_parse(psprintf("%s ADD CONSTRAINT %s %s", prefix, dg_quote_identifier(name), definition));
	Node	   *statement = linitial_node(RawStmt, parsed)->stmt;

	if (IsA(statement, AlterTableStmt))
		return castNode(Constraint, linitial_node(AlterTableCmd, ((AlterTableStmt *) statement)->cmds)->def);
	return castNode(Constraint, ((AlterDomainStmt *) statement)->def);
}

static List *
definition_references(const char *kind, const char *definition)
{
	List	   *references = NIL;

	if (!strcmp(kind, "sequence") && json_value(definition, "owned_table"))
		references_add(&references, "table", json_value(definition, "owned_table"));
	/* An index's definition lists its key columns as a table's does. */
	if (!strcmp(kind, "table") || !strcmp(kind, "index"))
	{
		JsonbValue	document = json_document(definition);
		JsonbValue	columns;
		int			n = json_get(&document, "columns", &columns) ? json_length(&columns) : 0;
		int			ndefaults,
					nconstraints;
		Node	  **defaults = schema_defaults(definition, &ndefaults);
		Constraint **constraints = schema_constraints(definition, &nconstraints);

		Assert(ndefaults == n);
		for (int i = 0; i < n; i++)
		{
			JsonbValue	column = json_element(&columns, i);

			references_type(&references, json_field_text(&column, "type"));
			(void) references_walker(defaults[i], &references);
		}
		for (int i = 0; i < nconstraints; i++)
		{
			if (constraints[i]->contype == CONSTR_CHECK)
				(void) references_walker(constraints[i]->raw_expr, &references);
			else if (constraints[i]->contype == CONSTR_FOREIGN && constraints[i]->pktable->schemaname)
				references_add(&references, "table", dg_qualified_name(constraints[i]->pktable->schemaname, constraints[i]->pktable->relname));
		}
	}
	if (!strcmp(kind, "index"))
	{
		IndexStmt  *index = schema_index(definition);

		if (index && index->relation->schemaname)
			references_add(&references, "table", dg_qualified_name(index->relation->schemaname, index->relation->relname));
	}
	if (!strcmp(kind, "view") || (!strcmp(kind, "table") && json_value(definition, "matview")) ||
		(!strcmp(kind, "routine") && json_value(definition, "dependencies")))
	{
		/* A view's query, a materialized view's, or a SQL-standard body lists what it uses. */
		run("SELECT v->>0,v->>1 FROM jsonb_array_elements(COALESCE($1::jsonb->'dependencies',$1::jsonb->'matview'->'dependencies','[]')) v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
		{
			uint64		n = SPI_processed;
			char	  **kinds = palloc_array(char *, Max(n, 1));
			char	  **names = palloc_array(char *, Max(n, 1));

			for (uint64 i = 0; i < n; i++)
			{
				kinds[i] = strcell(i, 1);
				names[i] = strcell(i, 2);
			}
			for (uint64 i = 0; i < n; i++)
			{
				if (!strcmp(kinds[i], "type"))
					references_type(&references, names[i]);
				else if (!strcmp(kinds[i], "routine") && strchr(names[i], '('))
					references_add(&references, "routine", pnstrdup(names[i], strchr(names[i], '(') - names[i]));
				else
					references_add(&references, kinds[i], names[i]);
			}
		}
	}
	if (!strcmp(kind, "routine"))
	{
		references_type(&references, json_value(definition, "prorettype"));
		references_type(&references, json_value(definition, "provariadic"));
		references_type_array(&references, json_value(definition, "proargtypes"));
		references_type_array(&references, json_value(definition, "proallargtypes"));
		{
			char	   *defaults = json_value(definition, "proargdefaults");

			if (defaults)
			{
				ArrayType  *values = DatumGetArrayTypeP(OidInputFunctionCall(F_ARRAY_IN, defaults, TEXTOID, -1));
				Datum	   *sql;
				bool	   *missing;
				int			n;

				deconstruct_array(values, TEXTOID, -1, false, TYPALIGN_INT, &sql, &missing, &n);
				for (int i = 0; i < n; i++)
					references_sql(&references, TextDatumGetCString(sql[i]));
			}
		}
	}
	else if (!strcmp(kind, "trigger"))
	{
		CreateTrigStmt *trigger = castNode(CreateTrigStmt, linitial_node(RawStmt, schema_expression_parse(json_value(definition, "definition")))->stmt);
		char	   *name;

		references_add(&references, "table", json_value(definition, "table"));
		if (trigger->constrrel)
			references_add(&references, "table", dg_qualified_name(trigger->constrrel->schemaname, trigger->constrrel->relname));
		if ((name = references_name(trigger->funcname)))
			references_add(&references, "routine", name);
		(void) references_walker(trigger->whenClause, &references);
	}
	else if (!strcmp(kind, "type"))
	{
		references_type(&references, json_value(definition, "typbasetype"));
		references_type(&references, json_value(definition, "typelem"));
		references_sql(&references, json_value(definition, "typdefaultbin"));
		run("SELECT v->>'type' FROM jsonb_array_elements(COALESCE($1::jsonb->'attributes','[]')) v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
		{
			uint64		n = SPI_processed;
			char	  **types = palloc_array(char *, Max(n, 1));

			for (uint64 i = 0; i < n; i++)
				types[i] = strcell(i, 1);
			for (uint64 i = 0; i < n; i++)
				references_type(&references, types[i]);
		}
		run("SELECT v->>0,v->>1 FROM jsonb_array_elements(COALESCE($1::jsonb->'constraints','[]')) v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
		{
			uint64		n = SPI_processed;
			char	  **names = palloc_array(char *, Max(n, 1));
			char	  **defs = palloc_array(char *, Max(n, 1));

			for (uint64 i = 0; i < n; i++)
			{
				names[i] = strcell(i, 1);
				defs[i] = strcell(i, 2);
			}
			for (uint64 i = 0; i < n; i++)
			{
				Constraint *constraint = references_constraint("ALTER DOMAIN placeholder", names[i], defs[i]);

				if (constraint->contype == CONSTR_CHECK)
					(void) references_walker(constraint->raw_expr, &references);
			}
		}
	}
	return references;
}

/* The references of the definition held in schema object |schema|. */
static List *
schema_references(const char *kind, bytea *schema)
{
	char		key[PL_HASH_SIZE * 2 + 1];
	ReferenceCacheEntry *entry;
	bool		found;

	if (!reference_cache)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(key);
		ctl.entrysize = sizeof(ReferenceCacheEntry);
		ctl.hcxt = TopMemoryContext;
		reference_cache = hash_create("schema references", 256, &ctl, HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
	}
	hex_encode(VARDATA_ANY(schema), Min(VARSIZE_ANY_EXHDR(schema), PL_HASH_SIZE), key);
	key[Min(VARSIZE_ANY_EXHDR(schema), PL_HASH_SIZE) * 2] = '\0';
	entry = hash_search(reference_cache, key, HASH_FIND, NULL);
	if (!entry)
	{
		List	   *references = definition_references(kind, metadata(schema, "schema"));
		MemoryContext previous = MemoryContextSwitchTo(TopMemoryContext);
		List	   *copy = NIL;

		foreach_node(String, reference, references)
			copy = lappend(copy, pstrdup(strVal(reference)));
		MemoryContextSwitchTo(previous);
		entry = hash_search(reference_cache, key, HASH_ENTER, &found);
		entry->references = copy;
	}
	return entry->references;
}

/* Add the dependency record of |members| to a database root's |json|. */
static char *
database_dependents(const char *json, Datum *members, int count)
{
	StringInfoData edges;
	uint64		n;
	bytea	  **schemas;
	char	  **kinds;

	run("SELECT o.objkind,o.objchildren[1] FROM pg_catalog.pg_dolt_object_visible o WHERE o.objhash=ANY($1) AND o.objkind IN ('table','index','routine','trigger','type','view','sequence')", 1, VC_TYPES(BYTEAARRAYOID), VC_ARGS(PointerGetDatum(links(members, count))), NULL, true);
	n = SPI_processed;
	schemas = palloc_array(bytea *, Max(n, 1));
	kinds = palloc_array(char *, Max(n, 1));
	for (uint64 i = 0; i < n; i++)
	{
		kinds[i] = strcell(i, 1);
		schemas[i] = DatumGetByteaPCopy(cell(i, 2));
	}
	initStringInfo(&edges);
	appendStringInfoChar(&edges, '[');
	for (uint64 i = 0; i < n; i++)
	{
		List	   *references = schema_references(kinds[i], schemas[i]);
		char	   *name;

		if (!references)
			continue;
		name = json_value(metadata(schemas[i], "schema"), "name");
		foreach_ptr(char, reference, references)
		{
			if (edges.len > 1)
				appendStringInfoChar(&edges, ',');
			appendStringInfoChar(&edges, '[');
			escape_json(&edges, reference);
			appendStringInfoChar(&edges, ',');
			escape_json(&edges, kinds[i]);
			appendStringInfoChar(&edges, ',');
			escape_json(&edges, name);
			appendStringInfoChar(&edges, ']');
		}
	}
	appendStringInfoChar(&edges, ']');
	run("SELECT jsonb_set($1::jsonb,'{dependents}',COALESCE((SELECT jsonb_object_agg(r,d) FROM (SELECT e->>0 r,jsonb_agg(DISTINCT jsonb_build_array(e->>1,e->>2)) d FROM jsonb_array_elements($2::jsonb) e GROUP BY e->>0) g),'{}'::jsonb))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), CStringGetTextDatum(edges.data)), NULL, true);
	return strcell(0, 1);
}

/* The members of database root |json| that refer to |kind| object |name|. */
static List *
database_referrers(const char *json, const char *kind, const char *name)
{
	List	   *result = NIL;

	run("SELECT e->>0,e->>1 FROM jsonb_array_elements(COALESCE($1::jsonb->'dependents'->($2||':'||$3),'[]')) e", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), CStringGetTextDatum(kind), CStringGetTextDatum(name)), NULL, true);
	for (uint64 i = 0; i < SPI_processed; i++)
		result = lappend(result, list_make2(strcell(i, 1), strcell(i, 2)));
	return result;
}

/*
 * The dependency-record key of server object |object|, such as
 * "routine:public.crypt", or NULL for any other object. Definitions name a
 * server's functions, operators and types, an extension's among them, as
 * they name built-in ones, by qualified name.
 */
static char *
server_object_reference(const ObjectAddress *object)
{
	CatalogView *previous;
	char	   *result = NULL;

	if (object->objectId < FirstNormalObjectId || object->objectId >= FirstDurableObjectId ||
		(object->classId != ProcedureRelationId && object->classId != TypeRelationId &&
		 object->classId != OperatorRelationId))
		return NULL;
	previous = CatalogViewSwitch(NULL);
	PG_TRY();
	{
		if (object->classId == ProcedureRelationId)
			result = psprintf("routine:%s", dg_qualified_name(get_namespace_name(get_func_namespace(object->objectId)),
															  get_func_name(object->objectId)));
		else if (object->classId == TypeRelationId)
		{
			HeapTuple	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(object->objectId));

			if (HeapTupleIsValid(tuple))
			{
				Form_pg_type type = (Form_pg_type) GETSTRUCT(tuple);

				result = psprintf("type:%s", dg_qualified_name(get_namespace_name(type->typnamespace), NameStr(type->typname)));
				ReleaseSysCache(tuple);
			}
		}
		else
		{
			HeapTuple	tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(object->objectId));

			if (HeapTupleIsValid(tuple))
			{
				Form_pg_operator op = (Form_pg_operator) GETSTRUCT(tuple);

				result = psprintf("operator:%s", dg_qualified_name(get_namespace_name(op->oprnamespace), NameStr(op->oprname)));
				ReleaseSysCache(tuple);
			}
		}
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

/* The target of ref |name|, read natively, or NULL. */
static bytea *
native_ref_target(const char *name)
{
	Relation	refs = table_open(DoltRefRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	bytea	   *target = NULL;

	ScanKeyInit(&key, Anum_pg_dolt_ref_refname, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(name));
	scan = systable_beginscan(refs, DoltRefKeyIndexId, true, NULL, 1, &key);
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		bool		isnull;

		target = DatumGetByteaPCopy(heap_getattr(tuple, Anum_pg_dolt_ref_reftarget, RelationGetDescr(refs), &isnull));
	}
	systable_endscan(scan);
	table_close(refs, AccessShareLock);
	return target;
}

/*
 * The first referrer, as "kind name", of any of |references| in database
 * object |database|, skipping referrers in namespace |except|; NULL if there
 * is none.
 */
static char *
database_referrer(bytea *database, char **references, int count, const char *except)
{
	bytea	   *payload;
	Jsonb	   *root;
	JsonbValue	dependents;
	JsonbValue *found;

	if (!database || !dg_object_read(database, "database", NULL, &payload, NULL))
		return NULL;
	root = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(pnstrdup(VARDATA_ANY(payload), VARSIZE_ANY_EXHDR(payload)))));
	found = JB_ROOT_IS_OBJECT(root) ? getKeyJsonValueFromContainer(&root->root, "dependents", strlen("dependents"), &dependents) : NULL;
	if (!found || found->type != jbvBinary)
		return NULL;
	for (int i = 0; i < count; i++)
	{
		JsonbValue	referrers;

		if (!references[i] ||
			!getKeyJsonValueFromContainer(found->val.binary.data, references[i], strlen(references[i]), &referrers) ||
			referrers.type != jbvBinary || !JsonContainerIsArray(referrers.val.binary.data))
			continue;
		for (int j = 0; j < json_length(&referrers); j++)
		{
			JsonbValue	referrer = json_element(&referrers, j);
			char	   *name = json_item_text(&referrer, 1);

			if (except && !strcmp(strVal(linitial(stringToQualifiedNameList(name, NULL))), except))
				continue;
			return psprintf("%s %s", json_item_text(&referrer, 0), name);
		}
	}
	return NULL;
}

/*
 * Refuse to drop namespace |name| on the session's branch while the branch's
 * definitions outside it use server objects in it: those stay in it on every
 * branch, and this branch would no longer have it.
 */
static void
namespace_drop_check(const char *name)
{
	CatalogView *previous = CatalogViewSwitch(NULL);
	List	   *objects = NIL;
	char	  **references;
	int			count = 0;
	char	   *referrer;

	PG_TRY();
	{
		Oid			namespace = get_namespace_oid(name, true);

		if (OidIsValid(namespace))
		{
			Relation	depend = table_open(DependRelationId, AccessShareLock);
			ScanKeyData keys[2];
			SysScanDesc scan;
			HeapTuple	tuple;

			ScanKeyInit(&keys[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(NamespaceRelationId));
			ScanKeyInit(&keys[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(namespace));
			scan = systable_beginscan(depend, DependReferenceIndexId, true, NULL, 2, keys);
			while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			{
				ObjectAddress *object = palloc(sizeof(*object));

				ObjectAddressSet(*object, ((Form_pg_depend) GETSTRUCT(tuple))->classid, ((Form_pg_depend) GETSTRUCT(tuple))->objid);
				objects = lappend(objects, object);
			}
			systable_endscan(scan);
			table_close(depend, AccessShareLock);
		}
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	if (!objects)
		return;
	references = palloc0_array(char *, list_length(objects));
	foreach_ptr(ObjectAddress, object, objects)
		references[count++] = server_object_reference(object);
	if ((referrer = database_referrer(native_ref_target(psprintf("working/%s", dg_current_branch())), references, count, name)) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot drop schema %s because other objects depend on it", name),
				 errdetail("%s uses server objects in schema %s, which stay in it on every branch.", referrer, name),
				 errhint("Change or drop the dependent objects on this branch first.")));
}

/*
 * Refuse to drop an extension's function, operator or type while any
 * branch's working schema names it: those definitions would bind nothing.
 * Dropping an extension drops its members, so it is refused too. History may
 * still name a dropped member; reading it then requires the extension, as
 * reading a clone does. Versioned definitions on other branches cannot be
 * dropped with it, so CASCADE does not help.
 */
void
dg_catalog_check_drop(const ObjectAddress *objects, int count, const ObjectAddress *original)
{
	char	  **references = palloc0_array(char *, Max(count, 1));
	int			n = 0;
	Relation	refs;
	SysScanDesc scan;
	HeapTuple	tuple;

	for (int i = 0; i < count; i++)
		n += (references[i] = server_object_reference(&objects[i])) != NULL;
	if (n == 0)
		return;
	refs = table_open(DoltRefRelationId, AccessShareLock);
	scan = systable_beginscan(refs, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		bool		isnull;
		char	   *name = TextDatumGetCString(heap_getattr(tuple, Anum_pg_dolt_ref_refname, RelationGetDescr(refs), &isnull));
		char	   *referrer;

		if (strncmp(name, "working/", strlen("working/")))
			continue;
		for (int i = 0; i < count; i++)
		{
			if (!references[i] ||
				!(referrer = database_referrer(DatumGetByteaPCopy(heap_getattr(tuple, Anum_pg_dolt_ref_reftarget, RelationGetDescr(refs), &isnull)),
											   &references[i], 1, NULL)))
				continue;
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 original ? errmsg("cannot drop %s because other objects depend on it", getObjectDescription(original, false)) :
					 errmsg("cannot drop desired object(s) because other objects depend on them"),
					 errdetail("%s on branch \"%s\" depends on %s.", referrer, name + strlen("working/"),
							   getObjectDescription(&objects[i], false)),
					 errhint("Change or drop the dependent objects on every branch first.")));
		}
	}
	systable_endscan(scan);
	table_close(refs, AccessShareLock);
}
