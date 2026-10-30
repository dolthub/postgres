static List *table_index_names(const char *schema_json, const char *table);
static bool name_selected(List *names, const char *name);

/* Native ALTER runs against private command-visible catalog tuples. Publication
 * serializes the final definitions once; intermediate roots are not published. */
static bool
native_alter_action(AlterTableType type)
{
	return type == AT_AlterConstraint || type == AT_AddIndex || type == AT_SetStatistics || type == AT_SetOptions || type == AT_ResetOptions ||
		type == AT_SetStorage || type == AT_SetCompression || type == AT_SetNotNull || type == AT_DropNotNull || type == AT_AddColumn || type == AT_DropColumn || type == AT_ColumnDefault || type == AT_AddConstraint || type == AT_DropConstraint || type == AT_ValidateConstraint || type == AT_AlterColumnType ||
		type == AT_AddIdentity || type == AT_SetIdentity || type == AT_DropIdentity;
}

static CatalogView *native_mutation_view;
static DGState *native_mutation_source;
static DGState *native_mutation_destination;
static Oid	native_mutation_relation;
static int	native_mutation_width;
static bool native_mutation_rewritten;


static void
mutation_validate_tuple(Relation catalog, HeapTuple tuple)
{
	int			field;
	bool		isnull;
	Datum		expression;

	if (RelationGetRelid(catalog) == AttrDefaultRelationId)
		field = Anum_pg_attrdef_adbin;
	else if (RelationGetRelid(catalog) == ConstraintRelationId)
		field = Anum_pg_constraint_conbin;
	else
		return;
	expression = heap_getattr(tuple, field, RelationGetDescr(catalog), &isnull);
	if (!isnull)
		(void) schema_expression_check(stringToNode(TextDatumGetCString(expression)), NULL);
}

static char *
mutation_columns(CatalogView *view, Oid relation, const char *definition, int **mapping, int *count)
{
	Relation	catalog = table_open(AttributeRelationId, AccessShareLock);
	Relation	defaults = table_open(AttrDefaultRelationId, AccessShareLock);
	HeapTuple	attributes[MaxHeapAttributeNumber] = {0};
	JsonbValue	document = json_document(definition);
	JsonbValue	oldcolumns;
	int			oldcount;
	JsonbValue *columns = palloc_array(JsonbValue, MaxHeapAttributeNumber);
	List	   *defaultrows = CatalogViewTuplesWith(view, AttrDefaultRelationId, Anum_pg_attrdef_adrelid, relation);

	*mapping = palloc_array(int, MaxHeapAttributeNumber);
	*count = 0;
	oldcount = json_get(&document, "columns", &oldcolumns) ? json_length(&oldcolumns) : 0;
	foreach_ptr(HeapTupleData, tuple, CatalogViewTuplesWith(view, AttributeRelationId, Anum_pg_attribute_attrelid, relation))
	{
		Form_pg_attribute attr = (Form_pg_attribute) GETSTRUCT(tuple);

		if (attr->attnum > 0)
			attributes[attr->attnum - 1] = tuple;
	}

	for (int ordinal = 0; ordinal < MaxHeapAttributeNumber; ordinal++)
	{
		HeapTuple	tuple = attributes[ordinal];
		Form_pg_attribute attr;
		char	   *typename;
		char	   *sql = NULL;
		Jsonb	   *column;
		JsonbValue	old;
		bool		has_old;

		if (!tuple)
			continue;
		attr = (Form_pg_attribute) GETSTRUCT(tuple);
		if (attr->attisdropped)
			continue;
		if (attr->atthasmissing && !native_mutation_destination)
			unsupported("canonical native column expression codec");
		/* A built-in type by its name, a versioned one qualified. */
		{
			HeapTuple	type = SearchSysCache1(TYPEOID, ObjectIdGetDatum(attr->atttypid));

			if (!HeapTupleIsValid(type))
				elog(ERROR, "cache lookup failed for type %u", attr->atttypid);
			if (((Form_pg_type) GETSTRUCT(type))->typnamespace == PG_CATALOG_NAMESPACE)
				typename = pstrdup(NameStr(((Form_pg_type) GETSTRUCT(type))->typname));
			else
				typename = routine_type_name(attr->atttypid);
			ReleaseSysCache(type);
		}

		/*
		 * The column's earlier definition, with its name, type and collation
		 * restated. The comment is kept, or null.
		 */
		has_old = ordinal < oldcount;
		if (has_old)
			old = json_element(&oldcolumns, ordinal);
		{
			static const char *const keys[] = {"name", "comment", "type", "typmod", "notnull", "collation"};
			JsonbValue	values[6];
			JsonbValue	comment;

			values[0] = jv_string(NameStr(attr->attname));
			values[1] = has_old && old.type == jbvBinary && JsonContainerIsObject(old.val.binary.data) &&
				getKeyJsonValueFromContainer(old.val.binary.data, "comment", 7, &comment) ? comment : jv_null();
			values[2] = jv_string(typename);
			values[3] = jv_int(attr->atttypmod);
			values[4] = jv_bool(false);
			values[5] = jv_null();
			if (OidIsValid(attr->attcollation))
			{
				/* Collations are the database's own, as internal SQL reads them. */
				CatalogView *previous = CatalogViewSwitch(NULL);
				HeapTuple	collation = SearchSysCache1(COLLOID, ObjectIdGetDatum(attr->attcollation));

				if (HeapTupleIsValid(collation))
				{
					Form_pg_collation form = (Form_pg_collation) GETSTRUCT(collation);
					HeapTuple	namespace = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(form->collnamespace));

					if (HeapTupleIsValid(namespace))
					{
						JsonbValue	items[8];
						char		provider[2] = {form->collprovider, '\0'};
						const int	fields[] = {Anum_pg_collation_collcollate, Anum_pg_collation_collctype, Anum_pg_collation_colllocale, Anum_pg_collation_collversion};

						items[0] = jv_string(NameStr(((Form_pg_namespace) GETSTRUCT(namespace))->nspname));
						items[1] = jv_string(NameStr(form->collname));
						items[2] = jv_string(provider);
						items[3] = jv_bool(form->collisdeterministic);
						for (int f = 0; f < lengthof(fields); f++)
						{
							bool		isnull;
							Datum		field = SysCacheGetAttr(COLLOID, collation, fields[f], &isnull);

							items[4 + f] = isnull ? jv_null() : jv_string(TextDatumGetCString(field));
						}
						values[5] = jv_jsonb(jb_array(8, items));
						ReleaseSysCache(namespace);
					}
					ReleaseSysCache(collation);
				}
				CatalogViewSwitch(previous);
			}
			column = has_old ? JsonbValueToJsonb(&old) : jb_object(0, NULL, NULL, false);
			column = jb_merge(column, jb_object(6, keys, values, false));
		}

		{
			CatalogView *previous = CatalogViewSwitch(view);

			PG_TRY();
			{
				foreach_ptr(HeapTupleData, def, defaultrows)
				{
					Form_pg_attrdef form = (Form_pg_attrdef) GETSTRUCT(def);
					bool		isnull;
					Datum		expression;

					if (form->adnum != attr->attnum)
						continue;
					expression = heap_getattr(def, Anum_pg_attrdef_adbin, RelationGetDescr(defaults), &isnull);
					if (isnull)
						elog(ERROR, "null native column default expression");
					sql = schema_expression_sql(stringToNode(TextDatumGetCString(expression)), relation);
				}
			}
			PG_FINALLY();
			{
				CatalogViewSwitch(previous);
			}
			PG_END_TRY();
			if (attr->atthasdef != (sql != NULL))
				elog(ERROR, "native attribute/default catalogs disagree");
		}

		/* A generated column's expression is its default's, with a kind. */
		{
			static const char *const keys[] = {"default", "generated"};
			JsonbValue	values[2];
			char		generated[2] = {attr->attgenerated, '\0'};

			values[0] = sql ? jv_string(sql) : jv_null();
			values[1] = attr->attgenerated ? jv_string(generated) : jv_null();
			column = jb_merge(jb_without(jb_without(column, "default"), "generated"),
							  jb_object(2, keys, values, true));
		}

		/* An identity column names its branch sequence, never its OID. */
		if (attr->attidentity)
		{
			static const char *const keys[] = {"identity", "sequence"};
			JsonbValue	values[2];
			char		identity[2] = {attr->attidentity, '\0'};
			CatalogView *outer = CatalogViewSwitch(view);
			Relation	rel = relation_open(relation, NoLock);
			Oid			sequence = getIdentitySequence(rel, attr->attnum, false);

			relation_close(rel, NoLock);
			CatalogViewSwitch(NULL);
			values[0] = jv_string(identity);
			values[1] = jv_string(dg_qualified_name(get_namespace_name(get_rel_namespace(sequence)), get_rel_name(sequence)));
			CatalogViewSwitch(outer);
			column = jb_merge(column, jb_object(2, keys, values, false));
		}
		else
			column = jb_without(jb_without(column, "identity"), "sequence");

		{
			static const char *const keys[] = {"notnull", "statistics", "options", "storage", "compression"};
			JsonbValue	values[5];
			bool		isnull;
			Datum		stats = heap_getattr(tuple, Anum_pg_attribute_attstattarget, RelationGetDescr(catalog), &isnull);
			Datum		options;
			char		storage[2] = {attr->attstorage, '\0'};
			char		compression[2] = {attr->attcompression, '\0'};

			values[0] = jv_bool(attr->attnotnull);
			values[1] = isnull ? jv_null() : jv_int(DatumGetInt16(stats));
			values[2] = jv_null();
			options = heap_getattr(tuple, Anum_pg_attribute_attoptions, RelationGetDescr(catalog), &isnull);
			if (!isnull)
			{
				Datum	   *items;
				int			n;
				JsonbValue *texts;

				/* Sorted as jsonb_agg(v ORDER BY v COLLATE "C") sorts them. */
				deconstruct_array_builtin(DatumGetArrayTypeP(options), TEXTOID, &items, NULL, &n);
				if (n > 0)
				{
					char	  **strings = palloc_array(char *, n);

					texts = palloc_array(JsonbValue, n);
					for (int k = 0; k < n; k++)
						strings[k] = TextDatumGetCString(items[k]);
					qsort(strings, n, sizeof(char *), pg_qsort_strcmp);
					for (int k = 0; k < n; k++)
						texts[k] = jv_string(strings[k]);
					values[2] = jv_jsonb(jb_array(n, texts));
				}
			}
			values[3] = attr->attstorage == get_typstorage(attr->atttypid) ? jv_null() : jv_string(storage);
			values[4] = attr->attcompression == '\0' ? jv_null() : jv_string(compression);
			column = jb_merge(jb_without(jb_without(jb_without(jb_without(column, "statistics"), "options"), "storage"), "compression"),
								 jb_object(5, keys, values, true));
		}
		columns[*count] = jv_jsonb(column);
		(*mapping)[(*count)++] = ordinal;
	}
	table_close(catalog, AccessShareLock);
	table_close(defaults, AccessShareLock);
	return json_set_jsonb(definition, "columns", jb_array(*count, columns));
}


/* Whether a native rewrite holds a destination state in the working store. */
bool
dg_native_rewrite_active(void)
{
	return native_mutation_destination != NULL;
}

bool
dg_catalog_begin_rewrite(Relation definition)
{
	MemoryContext old;

	if (!CatalogViewIsProvisional(definition->rd_catalogview))
		return false;
	if (definition->rd_catalogview != native_mutation_view || RelationGetRelid(definition) != native_mutation_relation || native_mutation_destination)
		elog(ERROR, "invalid native catalog rewrite authority");
	old = MemoryContextSwitchTo(GetMemoryChunkContext(native_mutation_source));
	native_mutation_destination = palloc0(sizeof(DGState));
	native_mutation_destination->store = native_mutation_source->store;
	native_mutation_destination->nextid = 1;
	native_mutation_width = RelationGetDescr(definition)->natts;
	dg_check(pl_build(native_mutation_source->store, NULL, 0, &native_mutation_destination->content));
	MemoryContextSwitchTo(old);
	return true;
}

void
dg_catalog_finish_rewrite(Relation definition)
{
	if (definition->rd_catalogview != native_mutation_view || RelationGetRelid(definition) != native_mutation_relation || !native_mutation_destination)
		elog(ERROR, "native rewrite completion has no storage authority");
	native_mutation_rewritten = true;
}

DGState *
dg_catalog_rewrite_state(Relation definition)
{
	if (definition->rd_catalogview != native_mutation_view || RelationGetRelid(definition) != native_mutation_relation || !native_mutation_destination)
		elog(ERROR, "native catalog rewrite has no insertion authority");
	return native_mutation_destination;
}

/* Native validation scans borrow the unchanged working-set data while catalog
 * definitions remain private. No provisional handle is a physical resource. */
DGState *
dg_catalog_mutation_load(Relation definition, Snapshot snapshot)
{
	CatalogView *view = CatalogViewObjectContext(RelationGetRelid(definition));
	CatalogView *previous;
	Relation	source;
	DGState    *state = NULL;
	const char *name;
	Oid			user;
	int			security;

	if (view != native_mutation_view || !CatalogViewIsProvisional(view))
		elog(ERROR, "native catalog validation has no storage authority");
	if (native_mutation_rewritten && RelationGetRelid(definition) == native_mutation_relation)
	{
		/* A reader's copy: releasing it must not free the shared store. */
		state = palloc0(sizeof(DGState));
		*state = *native_mutation_destination;
		state->shared = true;
		memset(&state->cleanup, 0, sizeof(state->cleanup));
		return state;
	}
	name = CatalogViewObjectIdentity(RelationGetRelid(definition), RelationRelationId);
	previous = CatalogViewSwitch(NULL);
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		source = dg_catalog_write_table(dg_current_branch(), name, ACL_UPDATE);
		state = dg_load(source, snapshot);
		dg_storage_close_relation(source);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		SetUserIdAndSecContext(user, security);
	}
	PG_END_TRY();
	return state;
}

/*
 * Release |state|, from dg_catalog_mutation_load, unless it is the rewrite's
 * destination, which the rewrite owns: it shares the source's store.
 */
void
dg_catalog_mutation_release(DGState *state)
{
	if (state != native_mutation_destination)
		dg_release(state);
}

/* A serialized constraint and its name. */
typedef struct ConstraintEntry
{
	char	   *name;
	JsonbValue	value;
} ConstraintEntry;

static int
compare_constraint_entries(const void *a, const void *b)
{
	const ConstraintEntry *left = a;
	const ConstraintEntry *right = b;

	if (!left->name || !right->name)
		return (left->name == NULL) - (right->name == NULL);
	return strcmp(left->name, right->name);
}

static char *
mutation_constraints(CatalogView *view, Oid relation, const char *definition)
{
	ListCell   *lc;
	StringInfoData json;
	CatalogView *previous = CatalogViewSwitch(view);
	bool		first = true;

	initStringInfo(&json);
	appendStringInfoChar(&json, '[');
	PG_TRY();
	{
		foreach(lc, CatalogViewTuplesWith(view, ConstraintRelationId, Anum_pg_constraint_conrelid, relation))
		{
			HeapTuple	tuple = lfirst(lc);
			Form_pg_constraint constraint = (Form_pg_constraint) GETSTRUCT(tuple);
			char	   *sql;

			/* A constraint trigger defines its constraint. */
			if (constraint->contype == CONSTRAINT_TRIGGER)
				continue;
			if (constraint->contype != CONSTRAINT_NOTNULL && constraint->contype != CONSTRAINT_PRIMARY && constraint->contype != CONSTRAINT_UNIQUE && constraint->contype != CONSTRAINT_CHECK && constraint->contype != CONSTRAINT_FOREIGN)
				unsupported("canonical codec for native constraint type");
			if (constraint->contype == CONSTRAINT_CHECK)
			{
				bool		isnull;
				Datum		expression = SysCacheGetAttr(CONSTROID, tuple, Anum_pg_constraint_conbin, &isnull);

				if (isnull)
					elog(ERROR, "null native CHECK expression");
				(void) schema_expression_check(stringToNode(TextDatumGetCString(expression)), NULL);
			}
			{
				int			nest = NewGUCNestLevel();

				PG_TRY(deparse);
				{
					schema_expression_gucs();
					sql = TextDatumGetCString(DirectFunctionCall1(pg_get_constraintdef, ObjectIdGetDatum(constraint->oid)));
				}
				PG_FINALLY(deparse);
				{
					AtEOXact_GUC(true, nest);
				}
				PG_END_TRY(deparse);
			}
			if (!first)
				appendStringInfoChar(&json, ',');
			first = false;
			appendStringInfoChar(&json, '[');
			escape_json(&json, NameStr(constraint->conname));
			appendStringInfoChar(&json, ',');
			escape_json(&json, sql);
			if (constraint->contype == CONSTRAINT_FOREIGN)
			{
				appendStringInfoChar(&json, ',');
				appendStringInfoString(&json, foreign_metadata(tuple));
			}
			appendStringInfoChar(&json, ']');
		}
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	appendStringInfoChar(&json, ']');
	{
		/* Ordered by name, as jsonb_agg(v ORDER BY (v->>0) COLLATE "C") orders them. */
		JsonbValue	document = json_document(json.data);
		int			n = json_length(&document);
		ConstraintEntry *entries = palloc_array(ConstraintEntry, Max(n, 1));
		JsonbValue *sorted = palloc_array(JsonbValue, Max(n, 1));

		for (int i = 0; i < n; i++)
		{
			entries[i].value = json_element(&document, i);
			entries[i].name = json_item_text(&entries[i].value, 0);
		}
		qsort(entries, n, sizeof(ConstraintEntry), compare_constraint_entries);
		for (int i = 0; i < n; i++)
			sorted[i] = entries[i].value;
		return json_set_jsonb(definition, "constraints", jb_array(n, sorted));
	}
}

typedef struct MutationIndex
{
	Oid			id;
	Oid			owner;
	char	   *name;
	char	   *sortname;
	char	   *sql;
	bytea	   *schema;
	bytea	   *member;
	Oid			resource;
} MutationIndex;

static bool native_mutation_indexes;

static void
mutation_storage(Relation relation, CatalogViewStorageEffect effect)
{
	if (relation->rd_catalogview == native_mutation_view && relation->rd_rel->relkind == RELKIND_SEQUENCE &&
		(effect == CATALOG_STORAGE_CREATE || effect == CATALOG_STORAGE_DROP))
		return;
	if (relation->rd_catalogview != native_mutation_view ||
		relation->rd_rel->relkind != RELKIND_INDEX ||
		relation->rd_rel->relam != DOLT_PROLLY_INDEX_AM_OID)
		unsupported("native mutation storage effect outside prolly indexes");
	if (effect != CATALOG_STORAGE_CREATE && IndexGetRelation(RelationGetRelid(relation), false) != native_mutation_relation)
		unsupported("native mutation index belongs to another table");
	native_mutation_indexes = true;
}

static int
mutation_index_compare(const ListCell *a, const ListCell *b)
{
	return strcmp(((MutationIndex *) lfirst(a))->sortname, ((MutationIndex *) lfirst(b))->sortname);
}

/* Native pg_index is the authority after ALTER has completed all its passes. */
static List *
mutation_indexes(CatalogView *view, Oid table)
{
	List	   *result = NIL;
	ListCell   *lc;
	CatalogView *previous = CatalogViewSwitch(view);
	int			nest = NewGUCNestLevel();

	PG_TRY();
	{
		schema_expression_gucs();
		foreach(lc, CatalogViewTuples(view, IndexRelationId))
		{
			HeapTuple	tuple = lfirst(lc);
			Form_pg_index index = (Form_pg_index) GETSTRUCT(tuple);
			MutationIndex *item;

			if (index->indrelid != table)
				continue;
			if (index->indisexclusion || !index->indisvalid || !index->indisready)
				unsupported("native index validity or exclusion constraint codec");
			for (int i = 0; i < index->indnatts; i++)
				if (index->indkey.values[i] <= 0)
					unsupported("canonical expression index codec");
			if (!heap_attisnull(tuple, Anum_pg_index_indpred, NULL))
				unsupported("canonical partial index codec");
			item = palloc0(sizeof(*item));
			item->id = index->indexrelid;
			{
				HeapTuple	relation = SearchSysCache1(RELOID, ObjectIdGetDatum(item->id));

				item->owner = ((Form_pg_class) GETSTRUCT(relation))->relowner;
				ReleaseSysCache(relation);
			}
			item->sortname = get_rel_name(item->id);
			item->name = dg_qualified_name(get_namespace_name(get_rel_namespace(item->id)), get_rel_name(item->id));
			item->sql = TextDatumGetCString(DirectFunctionCall1(pg_get_indexdef, ObjectIdGetDatum(item->id)));
			result = lappend(result, item);
		}
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	list_sort(result, mutation_index_compare);
	return result;
}

static char *
mutation_index_list(const char *definition, List *indexes)
{
	StringInfoData json;
	ListCell   *lc;

	initStringInfo(&json);
	appendStringInfoChar(&json, '[');
	foreach(lc, indexes)
	{
		MutationIndex *item = lfirst(lc);

		if (lc != list_head(indexes))
			appendStringInfoChar(&json, ',');
		escape_json(&json, item->sql);
	}
	appendStringInfoChar(&json, ']');
	run("SELECT jsonb_set($1::jsonb,'{indexes}',(SELECT coalesce(jsonb_agg(v ORDER BY v #>> '{}' COLLATE \"C\"),'[]'::jsonb) FROM jsonb_array_elements($2::jsonb) v))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(json.data)), NULL, true);
	return strcell(0, 1);
}

/* Whether |action| changes a column's identity and its sequence dependency. */
static bool
identity_action(AlterTableCmd *action)
{
	return action->subtype == AT_AddIdentity || action->subtype == AT_SetIdentity ||
		action->subtype == AT_DropIdentity;
}

/* Native identity operations in the provisional branch catalogs. Definitions
 * are published with the table; their allocation resources remain shared. */
static void
mutation_identity(Oid relid, AlterTableCmd *action)
{
	Relation	rel = relation_open(relid, NoLock);
	Relation	attrelation = table_open(AttributeRelationId, RowExclusiveLock);
	HeapTuple	tuple = SearchSysCacheCopyAttName(relid, action->name);
	ParseState *pstate = make_parsestate(NULL);
	CatalogView *view = CatalogViewCurrent();
	Form_pg_attribute attr;
	List	   *options = NIL;

	pstate->p_sourcetext = debug_query_string;
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						action->name, RelationGetRelationName(rel))));
	attr = (Form_pg_attribute) GETSTRUCT(tuple);
	if (attr->attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", action->name)));
	if (action->subtype == AT_AddIdentity)
	{
		Constraint *constraint = castNode(Constraint, action->def);
		CreateSeqStmt *create = makeNode(CreateSeqStmt);
		char	   *schema = get_namespace_name(RelationGetNamespace(rel));
		ObjectAddress sequence,
					column;

		if (!attr->attnotnull)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" must be declared NOT NULL before identity can be added",
							action->name, RelationGetRelationName(rel))));
		if (attr->attidentity)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" is already an identity column",
							action->name, RelationGetRelationName(rel))));
		if (attr->atthasdef)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" already has a default value",
							action->name, RelationGetRelationName(rel))));

		/* As generateSerialSequence, in the shared schema of the same name. */
		foreach_node(DefElem, option, constraint->options)
		{
			if (!strcmp(option->defname, "sequence_name"))
				create->sequence = makeRangeVarFromNameList(castNode(List, option->arg));
			else
				options = lappend(options, option);
		}
		PG_TRY();
		{
			if (!create->sequence)
				create->sequence = makeRangeVar(schema, ChooseRelationName(RelationGetRelationName(rel), action->name, "seq",
																			get_namespace_oid(schema, false), false), -1);
			create->options = lappend(options, makeDefElem("as", (Node *) makeTypeNameFromOid(attr->atttypid, -1), -1));
			create->ownerId = rel->rd_rel->relowner;
			create->for_identity = true;
			ObjectAddressSet(sequence, RelationRelationId, DefineSequence(pstate, create).objectId);
			CatalogViewNameObject(sequence.objectId, dg_qualified_name(get_namespace_name(get_rel_namespace(sequence.objectId)), get_rel_name(sequence.objectId)));
		}
		PG_FINALLY();
		{
			CatalogViewSwitch(view);
		}
		PG_END_TRY();
		CommandCounterIncrement();
		attr->attidentity = constraint->generated_when;
		ObjectAddressSubSet(column, RelationRelationId, relid, attr->attnum);
		recordDependencyOn(&sequence, &column, DEPENDENCY_INTERNAL);
	}
	else if (!attr->attidentity)
	{
		if (action->subtype != AT_DropIdentity || !action->missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" is not an identity column",
							action->name, RelationGetRelationName(rel))));
		ereport(NOTICE,
				(errmsg("column \"%s\" of relation \"%s\" is not an identity column, skipping",
						action->name, RelationGetRelationName(rel))));
	}
	else if (action->subtype == AT_SetIdentity)
	{
		foreach_node(DefElem, option, castNode(List, action->def))
		{
			if (!strcmp(option->defname, "generated"))
				attr->attidentity = defGetInt32(option);
			else
				options = lappend(options, option);
		}
		if (options)
		{
			AlterSeqStmt *stmt = makeNode(AlterSeqStmt);

			stmt->options = options;
			stmt->for_identity = true;
			sequence_alter(pstate, getIdentitySequence(rel, attr->attnum, false), stmt);
		}
	}
	else
	{
		ObjectAddress sequence;

		ObjectAddressSet(sequence, RelationRelationId, getIdentitySequence(rel, attr->attnum, false));
		deleteDependencyRecordsForClass(RelationRelationId, sequence.objectId, RelationRelationId, DEPENDENCY_INTERNAL);
		CommandCounterIncrement();
		performDeletion(&sequence, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
		attr->attidentity = '\0';
	}
	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);
	CommandCounterIncrement();
	heap_freetuple(tuple);
	table_close(attrelation, RowExclusiveLock);
	relation_close(rel, NoLock);
}

/*
 * Which members a native ALTER may have changed. A member's definition reads
 * the catalog rows its objects own. It changes when one of those rows changed,
 * or when one names an object whose rows changed, as a foreign key names the
 * table it references. Assertion builds serialize every member and check that
 * the others are unchanged.
 */
static HTAB *
oid_set(void)
{
	HASHCTL		ctl = {0};

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(Oid);
	ctl.hcxt = CurrentMemoryContext;
	return hash_create("object set", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static bool
oid_set_contains(HTAB *set, Oid oid)
{
	return hash_search(set, &oid, HASH_FIND, NULL) != NULL;
}

static void
oid_set_add(HTAB *set, Oid oid)
{
	if (oid >= FirstNormalObjectId)
		(void) hash_search(set, &oid, HASH_ENTER, NULL);
}

/*
 * The objects whose definitions read row |tuple| of |catalog|, into |owners|;
 * -1 for a catalog whose rows any object they name may read.
 */
static int
row_owners(Oid catalog, HeapTuple tuple, Oid *owners)
{
	switch (catalog)
	{
		case RelationRelationId:
			owners[0] = ((Form_pg_class) GETSTRUCT(tuple))->oid;
			return 1;
		case AttributeRelationId:
			owners[0] = ((Form_pg_attribute) GETSTRUCT(tuple))->attrelid;
			return 1;
		case AttrDefaultRelationId:
			owners[0] = ((Form_pg_attrdef) GETSTRUCT(tuple))->adrelid;
			return 1;
		case ConstraintRelationId:
			owners[0] = ((Form_pg_constraint) GETSTRUCT(tuple))->conrelid;
			owners[1] = ((Form_pg_constraint) GETSTRUCT(tuple))->contypid;
			return 2;
		case IndexRelationId:
			owners[0] = ((Form_pg_index) GETSTRUCT(tuple))->indexrelid;
			owners[1] = ((Form_pg_index) GETSTRUCT(tuple))->indrelid;
			return 2;
		case TypeRelationId:
			owners[0] = ((Form_pg_type) GETSTRUCT(tuple))->oid;
			owners[1] = ((Form_pg_type) GETSTRUCT(tuple))->typrelid;
			return 2;
		case TriggerRelationId:
			owners[0] = ((Form_pg_trigger) GETSTRUCT(tuple))->tgrelid;
			return 1;
		case RewriteRelationId:
			owners[0] = ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;
			return 1;
		case DependRelationId:
			owners[0] = ((Form_pg_depend) GETSTRUCT(tuple))->objid;
			return 1;
		case DescriptionRelationId:
			owners[0] = ((Form_pg_description) GETSTRUCT(tuple))->objoid;
			return 1;
		default:
			return -1;
	}
}

/* Add the objects row |tuple| of |catalog| names to |set|. */
static void
add_named(HTAB *set, Oid catalog, HeapTuple tuple)
{
	Relation	rel = table_open(catalog, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);

	for (int i = 0; i < desc->natts; i++)
	{
		bool		isnull;
		Datum		value;

		if (TupleDescAttr(desc, i)->atttypid != OIDOID)
			continue;
		value = heap_getattr(tuple, i + 1, desc, &isnull);
		if (!isnull)
			oid_set_add(set, DatumGetObjectId(value));
	}
	table_close(rel, AccessShareLock);
}

/* Add the objects that read row |tuple| of |catalog| to |set|. */
static void
add_owners(HTAB *set, Oid catalog, HeapTuple tuple)
{
	Oid			owners[2];
	int			count = row_owners(catalog, tuple, owners);

	if (count < 0)
		add_named(set, catalog, tuple);
	for (int i = 0; i < count; i++)
		oid_set_add(set, owners[i]);
}

static void
note_changed_row(Oid catalog, HeapTuple tuple, void *arg)
{
	add_owners((HTAB *) arg, catalog, tuple);
}

/* The objects whose definitions a native ALTER may have changed. */
static HTAB *
mutation_affected(CatalogView *view)
{
	HTAB	   *changed = oid_set();
	HTAB	   *affected = oid_set();
	HASH_SEQ_STATUS status;
	Oid		   *oid;

	CatalogViewForEachChange(view, note_changed_row, changed);
	for (int i = 0; i < lengthof(provider_catalogs); i++)
	{
		Relation	rel = table_open(provider_catalogs[i], AccessShareLock);
		TupleDesc	desc = RelationGetDescr(rel);

		foreach_ptr(HeapTupleData, tuple, CatalogViewTuples(view, provider_catalogs[i]))
		{
			bool		names_changed = false;

			for (int j = 0; j < desc->natts && !names_changed; j++)
			{
				bool		isnull;
				Datum		value;

				if (TupleDescAttr(desc, j)->atttypid != OIDOID)
					continue;
				value = heap_getattr(tuple, j + 1, desc, &isnull);
				names_changed = !isnull && oid_set_contains(changed, DatumGetObjectId(value));
			}
			if (names_changed)
				add_owners(affected, provider_catalogs[i], tuple);
		}
		table_close(rel, AccessShareLock);
	}
	hash_seq_init(&status, changed);
	while ((oid = hash_seq_search(&status)) != NULL)
		oid_set_add(affected, *oid);
	return affected;
}

static void
native_alter_publish(Capture current, VersionRef working, const char *name, AlterTableStmt *statement, DGState *state, int oldcount, Oid caller)
{
	CatalogView *view = CatalogViewCreate("provisional native ALTER");
	CatalogView *previous = CatalogViewCurrent();
	CatalogView *saved_mutation = native_mutation_view;
	DGState    *saved_source = native_mutation_source;
	DGState    *saved_destination = native_mutation_destination;
	Oid			saved_relation = native_mutation_relation;
	int			saved_width = native_mutation_width;
	bytea	  **members;
	char	  **kinds,
			  **names,
			  **payloads;
	Datum	   *children;
	StringInfoData json;
	int			count;
	int			used = 0;
	List	   *indexes = NIL,
			   *oldindexes = NIL;
	bool		saved_indexes = native_mutation_indexes;
	bool		saved_rewritten = native_mutation_rewritten;
	bool		rewrite = false;
	PLTree		tree = state->content;
	HTAB	   *affected;
	DGMember   *listed;

	PG_TRY();
	{
		Oid			id;
		ListCell   *lc;
		List	   *commands = statement->cmds;

		provider_populate(view, current.hash, NULL);
		CatalogViewBeginMutation(view);
		CatalogViewSetMutationValidator(view, mutation_validate_tuple);
		CatalogViewSetStorageHandler(view, mutation_storage);
		native_mutation_indexes = false;
		native_mutation_view = view;
		native_mutation_source = state;
		native_mutation_destination = NULL;
		native_mutation_rewritten = false;
		id = CatalogViewLookupObject(view, RelationRelationId, name);
		native_mutation_relation = id;
		CatalogViewSwitch(view);

		foreach(lc, commands)
		{
			AlterTableCmd *action = lfirst_node(AlterTableCmd, lc);

			if (identity_action(action))
			{
				mutation_identity(id, action);
				continue;
			}
			if (action->subtype == AT_AddConstraint &&
				castNode(Constraint, action->def)->contype != CONSTR_CHECK && castNode(Constraint, action->def)->contype != CONSTR_NOTNULL && castNode(Constraint, action->def)->contype != CONSTR_UNIQUE && castNode(Constraint, action->def)->contype != CONSTR_PRIMARY && castNode(Constraint, action->def)->contype != CONSTR_FOREIGN)
				unsupported("native constraint storage codec");
			if (action->subtype == AT_AddIndex)
			{
				IndexStmt  *index = castNode(IndexStmt, action->def);
				ListCell   *cc;

				if (index->concurrent || index->whereClause || index->options || index->tableSpace ||
					(index->accessMethod && strcmp(index->accessMethod, "btree") && strcmp(index->accessMethod, "dolt_prolly_index")))
					unsupported("canonical index storage properties");
				index->accessMethod = pstrdup("dolt_prolly_index");
				foreach(cc, index->indexParams)
				{
					IndexElem  *element = lfirst_node(IndexElem, cc);

					/* A user operator class is local to this server. */
					if (element->opclass &&
						get_opclass_oid(DOLT_PROLLY_INDEX_AM_OID, element->opclass, false) >= FirstNormalObjectId)
						unsupported("canonical index with a user-defined operator class");
				}
				action->def = (Node *) transformIndexStmt(id, index, debug_query_string ? debug_query_string : "");
			}
			if (action->subtype == AT_AlterColumnType)
			{
				ColumnDef  *col = castNode(ColumnDef, action->def);
				Relation	rel = relation_open(id, NoLock);

				if (col->raw_default)
					(void) schema_expression_bind(view, id, RelationGetRelationName(rel), RelationGetDescr(rel), col->raw_default, -1);
				relation_close(rel, NoLock);
			}
			if (action->subtype == AT_AddColumn)
			{
				ColumnDef  *col = castNode(ColumnDef, action->def);
				ListCell   *cc;
				const char *typename = strVal(llast(col->typeName->names));

				/*
				 * PostgreSQL ignores an existing IF NOT EXISTS definition
				 * before transforming defaults or subsidiary constraints.
				 */
				if (action->missing_ok && get_attnum(id, col->colname) != InvalidAttrNumber)
					continue;
				if (col->identity || col->generated || !strcmp(typename, "serial") || !strcmp(typename, "serial2") || !strcmp(typename, "serial4") || !strcmp(typename, "serial8") || !strcmp(typename, "smallserial") || !strcmp(typename, "bigserial"))
					unsupported("native identity/sequence/generated column codec");
				foreach(cc, col->constraints)
				{
					Constraint *con = lfirst_node(Constraint, cc);

					if (con->contype != CONSTR_DEFAULT && con->contype != CONSTR_NULL && con->contype != CONSTR_NOTNULL && con->contype != CONSTR_CHECK && con->contype != CONSTR_PRIMARY && con->contype != CONSTR_UNIQUE && con->contype != CONSTR_FOREIGN)
						unsupported("native added-column constraint codec");
				}
			}
		}

		{
			AlterTableUtilityContext context = {0};
			PlannedStmt *plan = makeNode(PlannedStmt);
			AlterTableStmt *stmt = copyObject(statement);
			Oid			privileged;
			int			security;
			int			pathnest = NewGUCNestLevel();

			foreach_node(AlterTableCmd, action, stmt->cmds)
				if (identity_action(action))
				stmt->cmds = foreach_delete_current(stmt->cmds, action);
			plan->commandType = CMD_UTILITY;
			plan->utilityStmt = (Node *) stmt;
			context.pstmt = plan;
			context.queryString = debug_query_string ? debug_query_string : "";
			context.relid = id;
			GetUserIdAndSecContext(&privileged, &security);
			SetUserIdAndSecContext(caller, security);
			PG_TRY(caller);
			{
				set_config_option("search_path", api_search_path, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
				if (stmt->cmds)
					AlterTable(stmt, AlterTableGetLockLevel(stmt->cmds), &context);
			}
			PG_FINALLY(caller);
			{
				AtEOXact_GUC(true, pathnest);
				SetUserIdAndSecContext(privileged, security);
			}
			PG_END_TRY(caller);
		}
		CommandCounterIncrement();
		indexes = mutation_indexes(view, id);
		CatalogViewSwitch(NULL);
		listed = dg_database_members(current.hash, &count);
		members = palloc_array(bytea *, Max(count, 1));
		kinds = palloc_array(char *, Max(count, 1));
		names = palloc_array(char *, Max(count, 1));
		payloads = palloc_array(char *, Max(count, 1));
		children = palloc_array(Datum, Max(count + list_length(indexes) + list_length(CatalogViewTuples(view, SequenceRelationId)), 1));
		for (int i = 0; i < count; i++)
		{
			members[i] = listed[i].hash;
			kinds[i] = (char *) listed[i].kind;
			/* Only tables and indexes are found by name here. */
			names[i] = !strcmp(kinds[i], "table") || !strcmp(kinds[i], "index") ? (char *) listed[i].name : NULL;
		}
		for (int i = 0; i < count; i++)
			if (names[i] && !strcmp(names[i], name))
				oldindexes = table_index_names(metadata(child(members[i], 1), "schema"), name);

		/*
		 * Serialize all surviving indexes, including newly allocated native
		 * handles.
		 */
		foreach(lc, indexes)
		{
			MutationIndex *item = lfirst(lc);
			char	   *old = NULL,
					   *definition;
			int		   *mapping,
						attributes;
			PLTree		empty;

			for (int i = 0; i < count; i++)
				if (!strcmp(kinds[i], "index") && !strcmp(names[i], item->name))
				{
					bytea	   *schema = child(members[i], 1);

					old = metadata(schema, "schema");
					item->resource = binding_relation(dg_current_branch(), item->name, schema);
				}
			if (!old)
				old = canonical_definition(item->name, item->owner, acldefault(OBJECT_TABLE, item->owner), "[]", "[]", "[]", item->sql);
			definition = mutation_columns(view, item->id, old, &mapping, &attributes);
			run("SELECT jsonb_set($1::jsonb,'{index_definition}',to_jsonb($2::text))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(item->sql)), NULL, true);
			definition = strcell(0, 1);
			dg_check(pl_build(state->store, NULL, 0, &empty));
			item->member = canonical_empty_member("index", item->name, definition, &empty);
			item->schema = child(item->member, 1);
		}
		affected = mutation_affected(view);
		initStringInfo(&json);
		appendStringInfoChar(&json, '[');
		for (int i = 0; i < count; i++)
		{
			bool		relation = !strcmp(kinds[i], "table") || !strcmp(kinds[i], "index");
			Oid			object = relation ? CatalogViewLookupObject(view, RelationRelationId, names[i]) : InvalidOid;
			bool		touched = relation && (!strcmp(names[i], name) || oid_set_contains(affected, object));

			payloads[i] = metadata(members[i], kinds[i]);
			if (!strcmp(kinds[i], "sequence"))
				continue;
			if (!strcmp(kinds[i], "index") && name_selected(oldindexes, names[i]))
				continue;
#ifdef USE_ASSERT_CHECKING
			if (relation)
#else
			if (touched)
#endif
			{
				bytea	   *oldschema = child(members[i], 1);
				char	   *old = metadata(oldschema, "schema");
				int		   *mapping;
				int			attributes;
				char	   *definition = mutation_columns(view, object, old, &mapping, &attributes);

				if (!strcmp(names[i], name))
				{
					rewrite = attributes != oldcount;
					for (int j = 0; j < attributes; j++)
						if (mapping[j] != j)
							rewrite = true;
					if (native_mutation_destination)
					{
						rewrite = true;
						tree = alter_rows(native_mutation_destination, native_mutation_width, attributes, mapping, NULL, NULL);
					}
					else if (rewrite)
						tree = alter_rows(state, oldcount, attributes, mapping, NULL, NULL);
				}

				if (!strcmp(kinds[i], "table"))
					definition = mutation_constraints(view, object, definition);
				if (!strcmp(names[i], name))
					definition = mutation_index_list(definition, indexes);
				if (!touched && !json_same(definition, old))
					elog(ERROR, "native ALTER changed %s, whose catalog rows it did not touch", names[i]);

				if (!json_same(definition, old))
				{
					bytea	   *schema = put("schema", definition, NULL, 0);
					bytea	   *content = rewrite && !strcmp(names[i], name) ? dg_history_export_tree(&tree) : child(members[i], 2);
					Oid			resource = binding_relation(dg_current_branch(), names[i], oldschema);

					if (!OidIsValid(resource))
						elog(ERROR, "native mutation storage binding is unavailable");
					run("SELECT jsonb_set($1::jsonb,'{schema}',to_jsonb(encode(sha256(convert_to($2,'UTF8')),'hex')))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(payloads[i]), CStringGetTextDatum(definition)), NULL, true);
					payloads[i] = strcell(0, 1);
					if (rewrite && !strcmp(names[i], name))
					{
						char		root[PL_HASH_TEXT_SIZE + 1];

						pl_hash_format(tree.root, root);
						payloads[i] = json_set_string(payloads[i], "content", root);
					}
					members[i] = put(kinds[i], payloads[i], VC_ARGS(PointerGetDatum(schema), PointerGetDatum(content)), 2);
					binding_record(dg_current_branch(), names[i], schema, resource);
				}
			}
			if (used)
				appendStringInfoChar(&json, ',');
			if (!strcmp(kinds[i], "namespace"))
				appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", payloads[i]);
			else
				appendStringInfoString(&json, payloads[i]);
			children[used++] = PointerGetDatum(members[i]);
		}
		foreach(lc, indexes)
		{
			MutationIndex *item = lfirst(lc);

			if (used)
				appendStringInfoChar(&json, ',');
			appendStringInfoString(&json, metadata(item->member, "index"));
			children[used++] = PointerGetDatum(item->member);
		}
		foreach_ptr(char, seqdef, sequence_definitions(view))
		{
			char	   *captured = sequence_capture(seqdef);
			bytea	   *member = object_member("sequence", captured);
			Oid			resource = sequence_resource(seqdef, true);

			binding_record(dg_current_branch(), json_value(seqdef, "name"), child(member, 1), resource);
			if (used)
				appendStringInfoChar(&json, ',');
			appendStringInfoString(&json, metadata(member, "sequence"));
			children[used++] = PointerGetDatum(member);
		}
		appendStringInfoChar(&json, ']');
		run("SELECT jsonb_set($1::jsonb,'{members}',$2::jsonb)::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(json.data)), NULL, true);
		{
			bytea	   *database = canonical_database(strcell(0, 1), children, used);

			foreach(lc, indexes)
			{
				MutationIndex *item = lfirst(lc);

				if (!OidIsValid(item->resource))
					item->resource = dg_storage_allocate_kind(RELKIND_INDEX, dg_current_branch());
				binding_record(dg_current_branch(), item->name, item->schema, item->resource);
			}
			move_ref(branch_ref("working"), working, database);
			advance_snapshot();
			if (rewrite || indexes != NIL || native_mutation_indexes)
			{
				Relation	target = dg_catalog_write_table(dg_current_branch(), name, ACL_UPDATE);

				dg_history_install(target, state, tree);
				dg_storage_close_relation(target);
				UpdateActiveSnapshotCommandId();
				move_ref(branch_ref("working"), read_ref(branch_ref("working")), capture(true).hash);
			}
		}
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		native_mutation_view = saved_mutation;
		native_mutation_source = saved_source;
		native_mutation_destination = saved_destination;
		native_mutation_relation = saved_relation;
		native_mutation_width = saved_width;
		native_mutation_indexes = saved_indexes;
		native_mutation_rewritten = saved_rewritten;
		if (CatalogViewIsProvisional(view))
			CatalogViewRetireMutation(view);
		CatalogViewRelease(view);
	}
	PG_END_TRY();
}

/* A provisional CREATE may make storage only for the relations it defines. */
static void
create_storage(Relation relation, CatalogViewStorageEffect effect)
{
	if (relation->rd_catalogview != native_mutation_view ||
		(relation->rd_rel->relam != DOLT_PROLLY_TABLE_AM_OID && relation->rd_rel->relam != DOLT_PROLLY_INDEX_AM_OID && relation->rd_rel->relkind != RELKIND_SEQUENCE))
		unsupported("native CREATE storage outside prolly tables and indexes");
}

/* Run one ALTER TABLE of a provisional CREATE against its new relation. */
static void
create_alter(Oid id, AlterTableStmt *stmt, const char *query)
{
	AlterTableUtilityContext context = {0};
	PlannedStmt *plan = makeNode(PlannedStmt);

	plan->commandType = CMD_UTILITY;
	plan->utilityStmt = (Node *) stmt;
	context.pstmt = plan;
	context.queryString = query;
	context.relid = id;
	AlterTable(stmt, AlterTableGetLockLevel(stmt->cmds), &context);
	CommandCounterIncrement();
}

/*
 * Native CREATE TABLE runs against a private copy of the branch's catalogs,
 * as native ALTER does, so PostgreSQL itself interprets the statement. The
 * result is serialized into canonical table and index definitions; nothing
 * is published here. SERIAL and identity sequence definitions and dependencies
 * are captured in the same provisional view. Foreign keys are
 * returned for the caller to add once the table is published.
 */
static void
native_create_definitions(Capture current, CreateStmt *statement, const char *query, Oid caller,
						  const char *name, char **table, List **index_names,
						  List **index_definitions, List **foreign, List **sequences)
{
	CatalogView *view = CatalogViewCreate("provisional native CREATE");
	CatalogView *previous = CatalogViewCurrent();
	CatalogView *saved_mutation = native_mutation_view;
	Oid			saved_relation = native_mutation_relation;
	Oid			privileged;
	int			security;
	int			pathnest = NewGUCNestLevel();

	*index_names = *index_definitions = *foreign = NIL;
	GetUserIdAndSecContext(&privileged, &security);
	PG_TRY();
	{
		List	   *commands;
		List	   *indexes = NIL;
		List	   *created_sequences = NIL;
		List	   *identity_sequences = NIL;
		RangeVar   *rv = NULL;
		Oid			id = InvalidOid;
		Oid			owner;
		Acl		   *acl;
		ListCell   *lc;
		char	   *definition;
		int		   *mapping;
		int			attributes;

		provider_populate(view, current.hash, NULL);
		CatalogViewBeginMutation(view);
		CatalogViewSetMutationValidator(view, mutation_validate_tuple);
		CatalogViewSetStorageHandler(view, create_storage);
		native_mutation_view = view;
		native_mutation_relation = InvalidOid;
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		set_config_option("search_path", api_search_path, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);

		commands = transformCreateStmt(statement, query);
		while (commands != NIL)
		{
			Node	   *command = linitial(commands);

			commands = list_delete_first(commands);
			if (IsA(command, CreateStmt))
			{
				rv = ((CreateStmt *) command)->relation;
				id = DefineRelation((CreateStmt *) command, RELKIND_RELATION, InvalidOid, NULL, query).objectId;
				CatalogViewNameObject(id, name);
				native_mutation_relation = id;
				{
					/* Storage codecs vet column types before any definition. */
					Relation	rel = relation_open(id, NoLock);

					dg_validate_descriptor(RelationGetDescr(rel));
					relation_close(rel, NoLock);
				}
				CommandCounterIncrement();
			}
			else if (IsA(command, TableLikeClause))
				commands = list_concat(expandTableLikeClause(rv, (TableLikeClause *) command), commands);
			else if (IsA(command, CreateSeqStmt))
			{
				CreateSeqStmt *sequence = (CreateSeqStmt *) command;
				ParseState *pstate = make_parsestate(NULL);
				Oid			created;

				pstate->p_sourcetext = query;
				created = DefineSequence(pstate, sequence).objectId;
				CatalogViewNameObject(created, dg_qualified_name(sequence->sequence->schemaname, sequence->sequence->relname));
				created_sequences = lappend_oid(created_sequences, created);
				if (sequence->for_identity)
					identity_sequences = lappend_oid(identity_sequences, created);
				CommandCounterIncrement();
			}
			else if (IsA(command, AlterSeqStmt))
			{
				AlterSeqStmt *alter = (AlterSeqStmt *) command;
				Oid			sequence = RangeVarGetRelid(alter->sequence, NoLock, false);
				Relation	rel = sequence_open(sequence, NoLock);

				foreach_node(DefElem, option, alter->options)
				{
					if (strcmp(option->defname, "owned_by"))
						unsupported("canonical CREATE sequence option");
					SequenceSetOwnedBy(rel, castNode(List, option->arg), list_member_oid(identity_sequences, sequence));
				}
				sequence_close(rel, NoLock);
				CommandCounterIncrement();
			}
			else if (IsA(command, IndexStmt))
			{
				AlterTableCmd *action = makeNode(AlterTableCmd);
				IndexStmt  *index = (IndexStmt *) command;

				if (index->accessMethod && strcmp(index->accessMethod, "btree") && strcmp(index->accessMethod, "dolt_prolly_index"))
					unsupported("canonical index storage properties");
				index->accessMethod = pstrdup("dolt_prolly_index");
				index->tableSpace = NULL;
				index->options = NIL;
				action->subtype = AT_AddIndex;
				action->def = (Node *) transformIndexStmt(id, index, query);
				indexes = lappend(indexes, action);
			}
			else if (IsA(command, AlterTableStmt))
			{
				AlterTableStmt *alter = (AlterTableStmt *) command;
				bool		foreign_only = true;

				foreach(lc, alter->cmds)
				{
					AlterTableCmd *action = lfirst_node(AlterTableCmd, lc);

					if (action->subtype != AT_AddConstraint || !IsA(action->def, Constraint) ||
						castNode(Constraint, action->def)->contype != CONSTR_FOREIGN)
						foreign_only = false;
				}
				if (foreign_only)
					*foreign = lappend(*foreign, alter);
				else
					create_alter(id, alter, query);
			}
			else
				unsupported("canonical CREATE auxiliary command");
		}
		if (indexes)
		{
			AlterTableStmt *alter = makeNode(AlterTableStmt);

			alter->relation = rv;
			alter->objtype = OBJECT_TABLE;
			alter->cmds = indexes;
			create_alter(id, alter, query);
		}
		AtEOXact_GUC(true, pathnest);
		SetUserIdAndSecContext(privileged, security);

		/* Serialize as native ALTER does, from the provisional catalogs. */
		{
			CatalogView *outer = CatalogViewSwitch(view);
			Relation	rel = relation_open(id, NoLock);

			owner = rel->rd_rel->relowner;
			relation_close(rel, NoLock);
			CatalogViewSwitch(outer);
		}
		acl = acldefault(OBJECT_TABLE, owner);
		definition = canonical_definition(name, owner, acl, "[]", "[]", "[]", NULL);
		definition = mutation_columns(view, id, definition, &mapping, &attributes);
		definition = mutation_constraints(view, id, definition);
		indexes = mutation_indexes(view, id);
		foreach(lc, indexes)
		{
			MutationIndex *item = lfirst(lc);
			char	   *index = canonical_definition(item->name, item->owner, acldefault(OBJECT_TABLE, item->owner), "[]", "[]", "[]", item->sql);

			index = mutation_columns(view, item->id, index, &mapping, &attributes);
			*index_names = lappend(*index_names, item->name);
			*index_definitions = lappend(*index_definitions, index);
		}
		*table = mutation_index_list(definition, indexes);
		foreach_oid(sequence, created_sequences)
		{
			CatalogView *outer = CatalogViewSwitch(view);

			*sequences = lappend(*sequences, sequence_encode(sequence));
			CatalogViewSwitch(outer);
		}
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		native_mutation_view = saved_mutation;
		native_mutation_relation = saved_relation;
		SetUserIdAndSecContext(privileged, security);
		if (CatalogViewIsProvisional(view))
			CatalogViewRetireMutation(view);
		CatalogViewRelease(view);
	}
	PG_END_TRY();
}
