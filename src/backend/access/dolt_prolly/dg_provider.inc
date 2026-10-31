/* Canonical database schemas decoded into native, immutable catalog tuples.
 * This provider never copies a live user relation's definition. Only bootstrap
 * catalog definitions/types and global role identities come from local catalogs.
 */
typedef struct DGProvider
{
	char	   *branch;			/* branch head; NULL for a revision */
	char	   *database;
	CatalogView *view;
	uint64		generation;		/* identity generation a branch view binds */
	uint64		extension_generation;	/* the extensions whose objects it binds */
	uint64		base_generation;	/* the server objects it lists */
	List	   *bindings;		/* the durable OIDs a branch view binds */
	bool		superseded;		/* a later view of the same revision replaced it */
	MemoryContext memory;		/* holds the entry */
	struct DGProvider *next;
} DGProvider;
static DGProvider *providers;


/* A durable OID bound by a branch view, and the storage bound with it. */
typedef struct ProviderBinding
{
	char		kind;
	Oid			oid;
	Oid			storage;
	char		name[FLEXIBLE_ARRAY_MEMBER];
} ProviderBinding;

/* The entry whose view is being built, which records its bindings. */
static DGProvider *provider_current_entry;


/*
 * A branch view binds its objects' durable OIDs. A change to the identity
 * catalog may rebind a name, so a branch view built before one is checked
 * against it before it is used again.
 */
static uint64 identity_generation = 1;
static bool identity_callback;

/*
 * A view binds extensions' objects, which definitions name, to their OIDs
 * here. Creating, altering or dropping an extension replaces those and resets
 * every relcache entry, so no view built before it is used again.
 */
static uint64 extension_generation = 1;

/*
 * Native DDL changes the server objects every view lists, as built-in rows:
 * views built before it are not used again. Ordinary writes and versioned DDL
 * invalidate none of the caches that track them.
 */
static uint64 base_generation = 1;

static void
provider_identities_changed(Datum arg, Oid relid)
{
	if (!OidIsValid(relid) || relid == DoltIdentityRelationId)
		identity_generation++;
	if (!OidIsValid(relid))
		extension_generation++;
}

/* Whether every durable OID |entry|'s view binds is still bound so. */
static bool
provider_bindings_current(DGProvider *entry)
{
	foreach_ptr(ProviderBinding, binding, entry->bindings)
	{
		if (dg_identity_lookup(entry->branch, binding->kind, binding->name) != binding->oid)
			return false;
		if (binding->kind == DOLT_IDENTITY_RELATION &&
			binding_storage(entry->branch, binding->name) != binding->storage)
			return false;
	}
	return true;
}

/* Whether |entry| may serve lookups of |branch|'s views, or revision views. */
static bool
provider_current(DGProvider *entry, const char *branch)
{
	if (entry->extension_generation != extension_generation || entry->base_generation != base_generation)
		return false;
	if (!branch)
		return !entry->branch;
	if (!entry->branch || strcmp(entry->branch, branch))
		return false;
	if (entry->generation != identity_generation)
	{
		if (!provider_bindings_current(entry))
			return false;
		entry->generation = identity_generation;
	}
	return true;
}

/*
 * pg_statistic is not among them: a branch's tables keep their statistics
 * physically, under their durable OIDs, and a revision's tables have none.
 */
static const Oid provider_catalogs[] = {
	NamespaceRelationId, AggregateRelationId, SequenceRelationId, RelationRelationId, AttributeRelationId, ProcedureRelationId, LanguageRelationId,
	TypeRelationId, EnumRelationId, OperatorRelationId, RangeRelationId, DescriptionRelationId, DependRelationId, IndexRelationId, ConstraintRelationId, CastRelationId, StatisticExtRelationId, StatisticExtDataRelationId, PublicationRelRelationId, PublicationNamespaceRelationId, InheritsRelationId, RewriteRelationId, TriggerRelationId, PolicyRelationId, AttrDefaultRelationId, SharedDependRelationId, SecLabelRelationId, InitPrivsRelationId
};

/* Whether branch and revision views supply rows of |catalog|. */
bool
dg_catalog_provides(Oid catalog)
{
	for (int i = 0; i < lengthof(provider_catalogs); i++)
		if (provider_catalogs[i] == catalog)
			return true;
	return false;
}

typedef struct DGProviderRows
{
	Relation	relations[lengthof(provider_catalogs)];
	List	   *rows[lengthof(provider_catalogs)];
	List	   *elements;		/* versioned types with array types */
	List	   *arrays;			/* and those array types */
} DGProviderRows;

/* The view and rows being built, against which column types resolve. */
static CatalogView *provider_current_view;
static DGProviderRows *provider_current_rows;

/* The members being rebuilt, by kind and definition. */
static int	provider_definition_count;
static char **provider_definition_kinds;
static char **provider_definition_list;

/* Its tables, indexes and views by name; relations share one namespace. */
typedef struct ProviderMember
{
	char		name[2 * (2 * NAMEDATALEN + 2) + 2];
	int			index;
} ProviderMember;
static HTAB *provider_members;

/*
 * Decoded columns of its definitions. A table's columns resolve against the
 * types built so far, so an entry is reused only while that count holds.
 */
typedef struct ProviderColumns
{
	const char *definition;
	int			types;
	TupleDesc	desc;
} ProviderColumns;
static HTAB *provider_column_cache;
static Oid routine_type_oid(DGProviderRows *rows, CatalogView *view, const char *name);

static void
provider_index_members(void)
{
	HASHCTL		ctl = {0};

	ctl.keysize = sizeof(((ProviderMember *) NULL)->name);
	ctl.entrysize = sizeof(ProviderMember);
	ctl.hcxt = CurrentMemoryContext;
	provider_members = hash_create("canonical members", Max(provider_definition_count, 16), &ctl,
								   HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
	ctl.keysize = sizeof(const char *);
	ctl.entrysize = sizeof(ProviderColumns);
	provider_column_cache = hash_create("canonical columns", Max(provider_definition_count, 16), &ctl,
										HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (int i = 0; i < provider_definition_count; i++)
	{
		const char *kind = provider_definition_kinds[i];
		char	   *name;
		ProviderMember *member;
		bool		found;

		if (strcmp(kind, "table") && strcmp(kind, "index") && strcmp(kind, "view") && strcmp(kind, "sequence"))
			continue;
		name = json_value(provider_definition_list[i], "name");
		if (strlen(name) >= sizeof(member->name))
			elog(ERROR, "canonical member name is too long");
		member = hash_search(provider_members, name, HASH_ENTER, &found);
		if (!found)
			member->index = i;
	}
}

/*
 * The member of the schema being rebuilt named |name|, or -1: a table, index
 * view or sequence, or only one of |kind| when given.
 */
static int
provider_member(const char *name, const char *kind)
{
	ProviderMember *member;

	if (strlen(name) >= sizeof(member->name))
		return -1;
	member = hash_search(provider_members, name, HASH_FIND, NULL);
	if (!member || (kind && strcmp(provider_definition_kinds[member->index], kind)))
		return -1;
	return member->index;
}

/*
 * The bootstrap catalogs' rows, which every view shares. A change to a
 * built-in object goes through one of these caches; any invalidation of them
 * makes the next view read the catalogs again.
 */
static CatalogViewBase *provider_base;
static bool provider_bootstrap(Oid catalog, HeapTuple tuple, TupleDesc desc);

/*
 * Every physical row other than a user schema's is a built-in object's or a
 * server object's: an extension's member, a heap table, any object not under
 * version control. Every branch lists them, as it lists built-in objects, in
 * their namespaces; namespaces themselves come from each branch. The
 * namespaces server objects live in are noted while the base is built, so
 * a view that leaves one out can leave out its objects too.
 */
static List *provider_server_namespaces;

/* Each server object with a namespace, as the base was built: object OIDs by namespace. */
typedef struct ProviderServerObject
{
	Oid			namespace;
	Oid			object;
} ProviderServerObject;
static ProviderServerObject *provider_server_objects;
static int	provider_server_object_count,
			provider_server_object_capacity;

/* The namespace |tuple| of |catalog| names its object in, or InvalidOid. */
static Oid
provider_row_namespace(Oid catalog, HeapTuple tuple)
{
	switch (catalog)
	{
		case RelationRelationId:
			return ((Form_pg_class) GETSTRUCT(tuple))->relnamespace;
		case TypeRelationId:
			return ((Form_pg_type) GETSTRUCT(tuple))->typnamespace;
		case ProcedureRelationId:
			return ((Form_pg_proc) GETSTRUCT(tuple))->pronamespace;
		case OperatorRelationId:
			return ((Form_pg_operator) GETSTRUCT(tuple))->oprnamespace;
		case ConstraintRelationId:
			return ((Form_pg_constraint) GETSTRUCT(tuple))->connamespace;
		case StatisticExtRelationId:
			return ((Form_pg_statistic_ext) GETSTRUCT(tuple))->stxnamespace;
		default:
			return InvalidOid;
	}
}

/* Whether |namespace| is a user schema, one each branch defines for itself. */
static bool
provider_user_namespace(Oid namespace)
{
	return namespace == PG_PUBLIC_NAMESPACE || namespace >= FirstNormalObjectId;
}

/* The object a native catalog row describes, for rows that describe another's part. */
static Oid
provider_row_owner(Oid catalog, HeapTuple tuple, Oid object)
{
	switch (catalog)
	{
		case RewriteRelationId:
			return ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;
		case ConstraintRelationId:
			{
				Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

				return OidIsValid(con->conrelid) ? con->conrelid : con->contypid;
			}
		case AttrDefaultRelationId:
			return ((Form_pg_attrdef) GETSTRUCT(tuple))->adrelid;
		case TriggerRelationId:
			return ((Form_pg_trigger) GETSTRUCT(tuple))->tgrelid;
		case EnumRelationId:
			return ((Form_pg_enum) GETSTRUCT(tuple))->enumtypid;
		case PolicyRelationId:
			return ((Form_pg_policy) GETSTRUCT(tuple))->polrelid;
		case StatisticExtRelationId:
			return ((Form_pg_statistic_ext) GETSTRUCT(tuple))->stxrelid;
		case DependRelationId:
			return ((Form_pg_depend) GETSTRUCT(tuple))->objid;
		case SharedDependRelationId:
			return ((Form_pg_shdepend) GETSTRUCT(tuple))->objid;
		default:
			return object;
	}
}

/*
 * Release the views no statement can use again: those a later view replaced,
 * and those built before an extension or server object changed. A view still
 * referenced elsewhere, such as by a cached plan, is kept until it is not.
 * Callers hold views without references during a statement, so this waits
 * for the end of the transaction.
 */
static void
provider_xact(XactEvent event, void *arg)
{
	DGProvider **link = &providers;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT && event != XACT_EVENT_PREPARE)
		return;
	while (*link)
	{
		DGProvider *entry = *link;

		if ((entry->superseded || entry->extension_generation != extension_generation ||
			 entry->base_generation != base_generation) &&
			CatalogViewReferences(entry->view) == 1 && CatalogViewCurrent() != entry->view)
		{
			*link = entry->next;
			CatalogViewRelease(entry->view);
			MemoryContextDelete(entry->memory);
			continue;
		}
		link = &entry->next;
	}
}

static void
provider_base_forget(Datum arg, int cacheid, uint32 hashvalue)
{
	base_generation++;
	if (provider_base)
		CatalogViewBaseRelease(provider_base);
	provider_base = NULL;
}

static CatalogViewBase *
provider_shared_base(void)
{
	static bool registered;
	CatalogViewBase *base;
	MemoryContext scratch;
	MemoryContext previous;

	if (!registered)
	{
		const int	caches[] = {RELOID, PROCOID, TYPEOID, NAMESPACEOID, OPEROID, CONSTROID, AGGFNOID, CASTSOURCETARGET};

		for (int i = 0; i < lengthof(caches); i++)
			CacheRegisterSyscacheCallback(caches[i], provider_base_forget, (Datum) 0);
		registered = true;
	}
	if (provider_base)
		return provider_base;
	base = CatalogViewBaseCreate();
	scratch = AllocSetContextCreate(CurrentMemoryContext, "bootstrap rows", ALLOCSET_DEFAULT_SIZES);
	previous = MemoryContextSwitchTo(scratch);
	PG_TRY();
	{
		list_free(provider_server_namespaces);
		provider_server_namespaces = NIL;
		provider_server_object_count = 0;
		for (int i = 0; i < lengthof(provider_catalogs); i++)
		{
			Relation	rel = table_open(provider_catalogs[i], AccessShareLock);
			TableScanDesc scan = table_beginscan_catalog(rel, 0, NULL);
			HeapTuple	tuple;
			List	   *tuples = NIL;

			while ((tuple = heap_getnext(scan, ForwardScanDirection)))
				if (provider_bootstrap(provider_catalogs[i], tuple, RelationGetDescr(rel)))
				{
					Oid			namespace = provider_row_namespace(provider_catalogs[i], tuple);

					tuples = lappend(tuples, heap_copytuple(tuple));
					if (provider_user_namespace(namespace))
					{
						MemoryContext saved = MemoryContextSwitchTo(TopMemoryContext);
						bool		isnull;

						provider_server_namespaces = list_append_unique_oid(provider_server_namespaces, namespace);
						if (provider_server_object_count == provider_server_object_capacity)
						{
							provider_server_object_capacity = Max(1024, provider_server_object_capacity * 2);
							provider_server_objects = provider_server_objects ?
								repalloc_array(provider_server_objects, ProviderServerObject, provider_server_object_capacity) :
								palloc_array(ProviderServerObject, provider_server_object_capacity);
						}
						provider_server_objects[provider_server_object_count].namespace = namespace;
						provider_server_objects[provider_server_object_count++].object =
							DatumGetObjectId(heap_getattr(tuple, 1, RelationGetDescr(rel), &isnull));
						MemoryContextSwitchTo(saved);
					}
				}
			table_endscan(scan);
			table_close(rel, AccessShareLock);
			CatalogViewBaseAddRows(base, provider_catalogs[i], tuples);
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(previous);
		CatalogViewBaseRelease(base);
		PG_RE_THROW();
	}
	PG_END_TRY();
	MemoryContextSwitchTo(previous);
	MemoryContextDelete(scratch);
	if (provider_base)
		CatalogViewBaseRelease(provider_base);
	provider_base = base;
	return base;
}

static int
provider_catalog_index(Oid id)
{
	int			i;

	for (i = 0; i < lengthof(provider_catalogs); i++)
		if (provider_catalogs[i] == id)
			return i;
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("revision-qualified catalog %u is not supported", id)));
	return -1;
}

/* The compiler layout of a catalog's non-null fixed fields is its tuple layout.
 * Form a normal full tuple, with the remaining nullable fields initially NULL.
 */
static HeapTuple
provider_fixed(Relation catalog, const void *data, Size size, int nfixed)
{
	TupleDesc	desc = RelationGetDescr(catalog);
	Datum	   *values = palloc0_array(Datum, desc->natts);
	bool	   *nulls = palloc_array(bool, desc->natts);
	Size		offset = 0;
	int			i;
	HeapTuple	result;

	memset(nulls, true, desc->natts * sizeof(bool));
	for (i = 0; i < nfixed; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (attr->attlen <= 0)
			elog(ERROR, "variable field in fixed catalog prefix");
		offset = att_align_nominal(offset, attr->attalign);
		if (offset + attr->attlen > size)
			elog(ERROR, "invalid fixed catalog prefix");
		values[i] = fetch_att((const char *) data + offset, attr->attbyval, attr->attlen);
		nulls[i] = false;
		offset += attr->attlen;
	}
	result = heap_form_tuple(desc, values, nulls);
	pfree(values);
	pfree(nulls);
	return result;
}

static HeapTuple
provider_set(Relation catalog, HeapTuple tuple, int attno, Datum value)
{
	TupleDesc	desc = RelationGetDescr(catalog);
	Datum	   *values = palloc0_array(Datum, desc->natts);
	bool	   *nulls = palloc0_array(bool, desc->natts);
	bool	   *replace = palloc0_array(bool, desc->natts);
	HeapTuple	result;

	values[attno - 1] = value;
	replace[attno - 1] = true;
	result = heap_modify_tuple(tuple, desc, values, nulls, replace);
	heap_freetuple(tuple);
	pfree(values);
	pfree(nulls);
	pfree(replace);
	return result;
}

static void
provider_add(DGProviderRows *rows, Oid catalog, const void *data, Size size, int nfixed)
{
	int			i = provider_catalog_index(catalog);

	rows->rows[i] = lappend(rows->rows[i], provider_fixed(rows->relations[i], data, size, nfixed));
}

static ArrayType *
provider_acl(const char *definition)
{
	int			i,
				n;
	Datum	   *items;
	static const char *names[] = {"SELECT", "INSERT", "UPDATE", "DELETE", "TRUNCATE", "REFERENCES", "TRIGGER", "USAGE", "CREATE", "MAINTAIN"};
	static const AclMode modes[] = {ACL_SELECT, ACL_INSERT, ACL_UPDATE, ACL_DELETE, ACL_TRUNCATE, ACL_REFERENCES, ACL_TRIGGER, ACL_USAGE, ACL_CREATE, ACL_MAINTAIN};

	JsonbValue	document = json_document(definition);
	JsonbValue	acl;

	n = json_get(&document, "acl", &acl) ? json_length(&acl) : 0;
	items = palloc_array(Datum, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		AclItem    *item = palloc0(sizeof(*item));
		JsonbValue	entry = json_element(&acl, i);
		char	   *grantor = json_item_text(&entry, 0),
				   *grantee = json_item_text(&entry, 1),
				   *priv = json_item_text(&entry, 2),
				   *granttext = json_item_text(&entry, 3);
		bool		grant = granttext && DatumGetBool(DirectFunctionCall1(boolin, CStringGetDatum(granttext)));
		AclMode		mode = 0;
		int			j;

		for (j = 0; j < lengthof(names); j++)
			if (!strcmp(priv, names[j]))
				mode = modes[j];
		if (!mode)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unsupported canonical privilege %s", priv)));
		item->ai_grantor = get_role_oid(grantor, false);
		item->ai_grantee = !strcmp(grantee, "PUBLIC") ? ACL_ID_PUBLIC : get_role_oid(grantee, false);
		ACLITEM_SET_PRIVS_GOPTIONS(*item, mode, grant ? mode : 0);
		items[i] = PointerGetDatum(item);
	}
	/* ACLs require a one-dimensional header even when there are no items. */
	if (!n)
		return (ArrayType *) make_empty_acl();
	return construct_array(items, n, ACLITEMOID, sizeof(AclItem), false, TYPALIGN_INT);
}

static void
provider_description(DGProviderRows *rows, Oid object, Oid catalog, int subid, const char *comment)
{
	FormData_pg_description form = {0};
	int			i = provider_catalog_index(DescriptionRelationId);
	HeapTuple	tuple;

	if (!comment)
		return;
	form.objoid = object;
	form.classoid = catalog;
	form.objsubid = subid;
	tuple = provider_fixed(rows->relations[i], &form, sizeof(form), Anum_pg_description_objsubid);
	tuple = provider_set(rows->relations[i], tuple, Anum_pg_description_description, CStringGetTextDatum(comment));
	rows->rows[i] = lappend(rows->rows[i], tuple);
}

static void
provider_dependency(DGProviderRows *rows, Oid objectclass, Oid object, int subid,
					Oid refclass, Oid ref, int refsubid, char kind)
{
	FormData_pg_depend form = {0};

	form.classid = objectclass;
	form.objid = object;
	form.objsubid = subid;
	form.refclassid = refclass;
	form.refobjid = ref;
	form.refobjsubid = refsubid;
	form.deptype = kind;
	provider_add(rows, DependRelationId, &form, sizeof(form), Natts_pg_depend);
}

/* The pg_type row of a type being rebuilt, if it is one. */
static Form_pg_type
provider_row_type(DGProviderRows *rows, Oid type)
{
	if (!rows)
		return NULL;
	foreach_ptr(HeapTupleData, tuple, rows->rows[provider_catalog_index(TypeRelationId)])
		if (((Form_pg_type) GETSTRUCT(tuple))->oid == type)
		return (Form_pg_type) GETSTRUCT(tuple);
	return NULL;
}

/* As TupleDescInitEntry, also for a type whose row the view does not hold yet. */
static void
provider_init_entry(TupleDesc desc, int attno, const char *name, Oid type, int32 typmod)
{
	Form_pg_type form = type >= FirstNormalObjectId ? provider_row_type(provider_current_rows, type) : NULL;
	Form_pg_attribute att;

	if (!form)
	{
		TupleDescInitEntry(desc, attno, name, type, typmod, 0);
		return;
	}
	TupleDescInitEntry(desc, attno, name, INT4OID, typmod, 0);
	att = TupleDescAttr(desc, attno - 1);
	att->atttypid = type;
	att->attlen = form->typlen;
	att->attbyval = form->typbyval;
	att->attalign = form->typalign;
	att->attstorage = form->typstorage;
	att->attcollation = form->typcollation;
	populate_compact_attribute(desc, attno - 1);
}

static TupleDesc provider_decode_columns(const char *definition);

/* A copy of |desc|, keeping its column properties. */
static TupleDesc
provider_copy_columns(TupleDesc desc)
{
	TupleDesc	copy = palloc(TupleDescSize(desc));

	memcpy(copy, desc, TupleDescSize(desc));
	copy->tdrefcount = -1;
	return copy;
}

/* Whether |desc| names only built-in types and collations. */
static bool
provider_builtin_columns(TupleDesc desc)
{
	for (int i = 0; i < desc->natts; i++)
		if (TupleDescAttr(desc, i)->atttypid >= FirstNormalObjectId ||
			TupleDescAttr(desc, i)->attcollation >= FirstNormalObjectId)
			return false;
	return true;
}

/*
 * The columns of |definition|, which the caller may change. Columns of only
 * built-in types are the same in every view, and kept with the schema.
 */
static TupleDesc
provider_columns(const char *definition)
{
	DGSchema   *schema = dg_schema_find(definition);
	ProviderColumns *entry;
	int			types;
	bool		found;
	MemoryContext previous;

	if (schema && (schema->parts & DG_SCHEMA_COLUMNS))
		return provider_copy_columns(schema->columns);
	if (!provider_column_cache)
	{
		TupleDesc	desc = provider_decode_columns(definition);

		if (schema && provider_builtin_columns(desc))
			dg_schema_set_columns(schema, desc);
		return desc;
	}
	types = list_length(provider_current_rows->rows[provider_catalog_index(TypeRelationId)]);
	entry = hash_search(provider_column_cache, &definition, HASH_ENTER, &found);
	if (!found || entry->types != types)
	{
		TupleDesc	desc = provider_decode_columns(definition);

		if (schema && provider_builtin_columns(desc))
		{
			dg_schema_set_columns(schema, desc);
			return desc;
		}
		previous = MemoryContextSwitchTo(GetMemoryChunkContext(provider_column_cache));
		entry->desc = provider_copy_columns(desc);
		MemoryContextSwitchTo(previous);
		entry->types = types;
	}
	return provider_copy_columns(entry->desc);
}

static TupleDesc
provider_decode_columns(const char *definition)
{
	TupleDesc	desc;
	char	  **names,
			  **types,
			  **collations,
			  **storages,
			  **compressions,
			  **generated,
			  **identities;
	int		   *mods;
	bool	   *notnull,
			   *hasdef;
	int			i,
				n;

	JsonbValue	document = json_document(definition);
	JsonbValue	columns;
	JsonbValue	statement;
	bool		index = json_get(&document, "index_definition", &statement);

	n = json_get(&document, "columns", &columns) ? json_length(&columns) : 0;
	names = palloc_array(char *, Max(n, 1));
	types = palloc_array(char *, Max(n, 1));
	collations = palloc_array(char *, Max(n, 1));
	mods = palloc_array(int, Max(n, 1));
	notnull = palloc_array(bool, Max(n, 1));
	storages = palloc_array(char *, Max(n, 1));
	compressions = palloc_array(char *, Max(n, 1));
	hasdef = palloc_array(bool, Max(n, 1));
	generated = palloc_array(char *, Max(n, 1));
	identities = palloc_array(char *, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		JsonbValue	column = json_element(&columns, i);
		JsonbValue	collation;
		char	   *text;

		names[i] = json_field_text(&column, "name");
		types[i] = json_field_text(&column, "type");
		text = json_field_text(&column, "typmod");
		mods[i] = text ? DatumGetInt32(DirectFunctionCall1(int4in, CStringGetDatum(text))) : 0;
		collations[i] = NULL;
		if (json_get(&column, "collation", &collation))
		{
			char	   *schematext = json_item_text(&collation, 0);
			char	   *nametext = json_item_text(&collation, 1);

			if (schematext && nametext)
				collations[i] = psprintf("%s.%s", quote_identifier(schematext), quote_identifier(nametext));
		}
		text = json_field_text(&column, "notnull");
		notnull[i] = text && DatumGetBool(DirectFunctionCall1(boolin, CStringGetDatum(text)));
		storages[i] = json_field_text(&column, "storage");
		compressions[i] = json_field_text(&column, "compression");
		hasdef[i] = json_field_text(&column, "default") != NULL;
		generated[i] = json_field_text(&column, "generated");
		identities[i] = json_field_text(&column, "identity");
	}
	desc = CreateTemplateTupleDesc(n);
	for (i = 0; i < n; i++)
	{
		Oid			type;

		/* A versioned type is qualified; a built-in one is not. */
		if (strchr(types[i], '.') || strstr(types[i], "[]"))
			type = routine_type_oid(provider_current_rows, provider_current_view, types[i]);
		else
			type = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum(types[i]), ObjectIdGetDatum(PG_CATALOG_NAMESPACE));
		if (!OidIsValid(type))
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("unknown canonical built-in type %s", types[i])));
		provider_init_entry(desc, i + 1, names[i], type, mods[i]);
		if (collations[i])
			TupleDescInitEntryCollation(desc, i + 1, get_collation_oid(stringToQualifiedNameList(collations[i], NULL), false));
		else if (index)
		{
			/* An index's included columns have no collation, as natively. */
			TupleDescAttr(desc, i)->attcollation = InvalidOid;
		}
		TupleDescAttr(desc, i)->attnotnull = notnull[i];
		{
			JsonbValue column = json_element(&columns, i);
			char *local = json_field_text(&column, "islocal");
			char *inherited = json_field_text(&column, "inhcount");

			TupleDescAttr(desc, i)->attislocal = !local || !strcmp(local, "true");
			TupleDescAttr(desc, i)->attinhcount = inherited ? pg_strtoint32(inherited) : 0;
		}
		TupleDescAttr(desc, i)->atthasdef = hasdef[i];
		if (generated[i])
			TupleDescAttr(desc, i)->attgenerated = generated[i][0];
		if (identities[i])
			TupleDescAttr(desc, i)->attidentity = identities[i][0];
		if (storages[i])
			TupleDescAttr(desc, i)->attstorage = storages[i][0];
		if (compressions[i])
			TupleDescAttr(desc, i)->attcompression = compressions[i][0];
	}

	/*
	 * Versioned types are validated by their DDL, in the view that defines
	 * them; while this view is built, their rows are not yet visible.
	 */
	if (!provider_current_rows)
		dg_validate_descriptor(desc);
	else
		for (i = 0; i < desc->natts; i++)
		{
			TupleDesc	one;

			if (TupleDescAttr(desc, i)->atttypid >= FirstNormalObjectId)
				continue;
			one = CreateTemplateTupleDesc(1);
			TupleDescCopyEntry(one, 1, desc, i + 1);
			dg_validate_descriptor(one);
			FreeTupleDesc(one);
		}
	return desc;
}

/*
 * Whether a physical row belongs in the shared base: every built-in object's
 * and server object's row, but not a user schema's, which each branch
 * supplies.
 */
static bool
provider_bootstrap(Oid catalog, HeapTuple tuple, TupleDesc desc)
{
	bool		isnull;
	Oid			object = DatumGetObjectId(heap_getattr(tuple, 1, desc, &isnull));

	if (catalog == SharedDependRelationId)
	{
		Oid			database = ((Form_pg_shdepend) GETSTRUCT(tuple))->dbid;

		return database == InvalidOid || database == MyDatabaseId;
	}
	/* Each branch defines its user schemas, and their comments. */
	if (catalog == NamespaceRelationId)
		return !provider_user_namespace(object);
	if (catalog == DescriptionRelationId && ((Form_pg_description) GETSTRUCT(tuple))->classoid == NamespaceRelationId)
		return !provider_user_namespace(object);
	return true;
}

static int
provider_attribute(TupleDesc desc, const char *name)
{
	int			i;

	for (i = 0; i < desc->natts; i++)
		if (!strcmp(NameStr(TupleDescAttr(desc, i)->attname), name))
			return i + 1;
	ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("canonical attribute %s is missing", name)));
	return 0;
}

typedef struct ProviderRoutineReferences
{
	DGProviderRows *rows;
	Oid			classid;
	Oid			objid;
} ProviderRoutineReferences;

/* As recordDependencyOnExpr: an expression depends on the routines it calls. */
static bool
provider_routine_references(Node *node, ProviderRoutineReferences *context)
{
	if (!node)
		return false;
	if (IsA(node, FuncExpr) && ((FuncExpr *) node)->funcid >= FirstDurableObjectId)
		provider_dependency(context->rows, context->classid, context->objid, 0, ProcedureRelationId,
							((FuncExpr *) node)->funcid, 0, DEPENDENCY_NORMAL);
	if ((IsA(node, OpExpr) || IsA(node, DistinctExpr) || IsA(node, NullIfExpr)) && ((OpExpr *) node)->opno >= FirstDurableObjectId)
		provider_dependency(context->rows, context->classid, context->objid, 0, OperatorRelationId, ((OpExpr *) node)->opno, 0, DEPENDENCY_NORMAL);
	if (IsA(node, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) node)->opno >= FirstDurableObjectId)
		provider_dependency(context->rows, context->classid, context->objid, 0, OperatorRelationId, ((ScalarArrayOpExpr *) node)->opno, 0, DEPENDENCY_NORMAL);
	if (IsA(node, Const) && ((Const *) node)->consttype == REGCLASSOID && !((Const *) node)->constisnull &&
		DatumGetObjectId(((Const *) node)->constvalue) >= FirstDurableObjectId)
		provider_dependency(context->rows, context->classid, context->objid, 0, RelationRelationId,
							DatumGetObjectId(((Const *) node)->constvalue), 0, DEPENDENCY_NORMAL);
	return expression_tree_walker(node, provider_routine_references, context);
}

static void
provider_expression_dependencies(DGProviderRows *rows, Oid classid, Oid objid, Node *expression)
{
	ProviderRoutineReferences context = {
		.rows = rows,
		.classid = classid,
		.objid = objid,
	};

	(void) provider_routine_references(expression, &context);
}

/* Copies of |count| cached trees, which the caller may change. */
static Node **
schema_node_copies(Node **nodes, int count)
{
	Node	  **copies = palloc0_array(Node *, Max(count, 1));

	for (int i = 0; i < count; i++)
		copies[i] = copyObject(nodes[i]);
	return copies;
}

/*
 * The raw default of each column of table |definition|, or NULL, parsed once
 * per schema. The caller may change the trees.
 */
static Node **
schema_defaults(const char *definition, int *count)
{
	DGSchema   *schema = dg_schema_find(definition);
	JsonbValue	document;
	JsonbValue	columns;
	Node	  **defaults;

	if (schema && (schema->parts & DG_SCHEMA_DEFAULTS))
		return schema_node_copies(schema->defaults, *count = schema->ndefaults);
	document = json_document(definition);
	*count = json_get(&document, "columns", &columns) ? json_length(&columns) : 0;
	defaults = palloc0_array(Node *, Max(*count, 1));
	for (int i = 0; i < *count; i++)
	{
		JsonbValue	column = json_element(&columns, i);
		char	   *sql = json_field_text(&column, "default");

		if (sql)
			defaults[i] = schema_expression_raw(sql);
	}
	if (schema)
		dg_schema_set_nodes(schema, DG_SCHEMA_DEFAULTS, *count, defaults);
	return defaults;
}

/*
 * The raw Constraint of each constraint of table |definition|, parsed once
 * per schema. The caller may change the trees.
 */
static Constraint **
schema_constraints(const char *definition, int *count)
{
	DGSchema   *schema = dg_schema_find(definition);
	JsonbValue	document;
	JsonbValue	constraints;
	Node	  **result;

	if (schema && (schema->parts & DG_SCHEMA_CONSTRAINTS))
		return (Constraint **) schema_node_copies(schema->constraints, *count = schema->nconstraints);
	document = json_document(definition);
	*count = json_get(&document, "constraints", &constraints) ? json_length(&constraints) : 0;
	result = palloc0_array(Node *, Max(*count, 1));
	for (int i = 0; i < *count; i++)
	{
		JsonbValue	constraint = json_element(&constraints, i);
		List	   *parsed = schema_expression_parse(psprintf("ALTER TABLE placeholder ADD CONSTRAINT %s %s",
															  dg_quote_identifier(json_item_text(&constraint, 0)),
															  json_item_text(&constraint, 1)));
		Node	   *node = ((RawStmt *) linitial(parsed))->stmt;
		AlterTableStmt *alter;

		if (list_length(parsed) != 1 || !IsA(node, AlterTableStmt))
			elog(ERROR, "invalid canonical constraint");
		alter = (AlterTableStmt *) node;
		if (list_length(alter->cmds) != 1 || !IsA(((AlterTableCmd *) linitial(alter->cmds))->def, Constraint))
			elog(ERROR, "invalid canonical constraint command");
		result[i] = ((AlterTableCmd *) linitial(alter->cmds))->def;
	}
	if (schema)
		dg_schema_set_nodes(schema, DG_SCHEMA_CONSTRAINTS, *count, result);
	return (Constraint **) result;
}

/*
 * The raw IndexStmt of index |definition|, parsed once per schema, or NULL if
 * it has none. The caller may change the tree.
 */
static IndexStmt *
schema_index(const char *definition)
{
	DGSchema   *schema = dg_schema_find(definition);
	char	   *sql;
	List	   *parsed;
	Node	   *node;

	if (schema && (schema->parts & DG_SCHEMA_INDEX))
		return copyObject((IndexStmt *) schema->index);
	sql = json_value(definition, "index_definition");
	if (!sql)
		return NULL;
	parsed = schema_expression_parse(sql);
	node = ((RawStmt *) linitial(parsed))->stmt;
	if (list_length(parsed) != 1 || !IsA(node, IndexStmt))
		elog(ERROR, "invalid canonical index definition");
	if (schema)
		dg_schema_set_nodes(schema, DG_SCHEMA_INDEX, 1, &node);
	return (IndexStmt *) node;
}

static void
provider_defaults(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	char	   *name = json_value(definition, "name");
	Oid			relation = CatalogViewLookupObject(view, RelationRelationId, name);
	TupleDesc	desc = provider_columns(definition);
	JsonbValue	document = json_document(definition);
	JsonbValue	columns;
	int			ndefaults;
	Node	  **defaults = schema_defaults(definition, &ndefaults);

	if (!json_get(&document, "columns", &columns) || json_length(&columns) != desc->natts || ndefaults != desc->natts)
		elog(ERROR, "canonical column count mismatch");
	for (int i = 0; i < desc->natts; i++)
	{
		Node	   *expression;
		FormData_pg_attrdef form = {0};
		HeapTuple	tuple;
		int			slot = provider_catalog_index(AttrDefaultRelationId);

		if (!TupleDescAttr(desc, i)->atthasdef)
			continue;
		expression = schema_expression_bind(view, relation, name, desc, defaults[i], i + 1);
		form.oid = CatalogViewBindObject(view, AttrDefaultRelationId, psprintf("%s/%s", name, NameStr(TupleDescAttr(desc, i)->attname)), InvalidOid);
		form.adrelid = relation;
		form.adnum = i + 1;
		tuple = provider_fixed(rows->relations[slot], &form, sizeof(form), Anum_pg_attrdef_adnum);
		tuple = provider_set(rows->relations[slot], tuple, Anum_pg_attrdef_adbin, CStringGetTextDatum(nodeToString(expression)));
		rows->rows[slot] = lappend(rows->rows[slot], tuple);
		/*
		 * As StoreAttrDefault records: a default belongs to its column, while
		 * a generation expression is part of its column and depends on its
		 * inputs, which then cannot be dropped alone.
		 */
		provider_dependency(rows, AttrDefaultRelationId, form.oid, 0, RelationRelationId, relation, i + 1,
							TupleDescAttr(desc, i)->attgenerated ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);
		provider_expression_dependencies(rows, AttrDefaultRelationId, form.oid, expression);
		if (TupleDescAttr(desc, i)->attgenerated)
		{
			Bitmapset  *inputs = NULL;
			int			input = -1;

			pull_varattnos(expression, 1, &inputs);
			while ((input = bms_next_member(inputs, input)) >= 0)
				if (input + FirstLowInvalidHeapAttributeNumber != i + 1)
					provider_dependency(rows, AttrDefaultRelationId, form.oid, 0, RelationRelationId, relation,
										input + FirstLowInvalidHeapAttributeNumber, DEPENDENCY_NORMAL);
		}
	}
}

#include "dg_foreign_provider.inc"

static void
provider_constraints(DGProviderRows *rows, CatalogView *view, const char *definition, int count, char **definitions, char **kinds)
{
	char	   *name = json_value(definition, "name");
	List	   *parts = stringToQualifiedNameList(name, NULL);
	Oid			relid = CatalogViewLookupObject(view, RelationRelationId, name);
	Oid			ns = CatalogViewLookupObject(view, NamespaceRelationId, strVal(linitial(parts)));
	TupleDesc	desc = provider_columns(definition);
	char	  **names,
			  **metadata;
	int			n,
				i;

	JsonbValue	document = json_document(definition);
	JsonbValue	constraints;
	Constraint **parsed;

	n = json_get(&document, "constraints", &constraints) ? json_length(&constraints) : 0;
	names = palloc_array(char *, Max(n, 1));
	metadata = palloc_array(char *, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		JsonbValue	constraint = json_element(&constraints, i);

		names[i] = json_item_text(&constraint, 0);
		metadata[i] = json_item_json(&constraint, 2);
	}
	parsed = schema_constraints(definition, &i);
	if (i != n)
		elog(ERROR, "canonical constraint count mismatch");
	for (i = 0; i < n; i++)
	{
		Constraint *constraint = parsed[i];
		Node	   *expression = NULL;
		FormData_pg_constraint form = {0};
		ListCell   *lc;
		Datum	   *keys;
		HeapTuple	tuple;
		int			slot = provider_catalog_index(ConstraintRelationId),
					k = 0;

		switch (constraint->contype)
		{
			case CONSTR_PRIMARY:
				form.contype = CONSTRAINT_PRIMARY;
				break;
			case CONSTR_UNIQUE:
				form.contype = CONSTRAINT_UNIQUE;
				break;
			case CONSTR_FOREIGN:
				form.contype = CONSTRAINT_FOREIGN;
				constraint->keys = constraint->fk_attrs;
				break;
			case CONSTR_CHECK:
				form.contype = CONSTRAINT_CHECK;
				expression = schema_expression_bind(view, relid, strVal(lsecond(parts)), desc, constraint->raw_expr, 0);
				break;
			case CONSTR_NOTNULL:
				form.contype = CONSTRAINT_NOTNULL;
				break;
			default:
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unsupported canonical constraint %s", names[i])));
		}
		form.oid = CatalogViewBindObject(view, ConstraintRelationId, psprintf("%s/%s", name, names[i]), InvalidOid);
		namestrcpy(&form.conname, names[i]);
		form.connamespace = ns;
		form.conrelid = relid;
		form.condeferrable = constraint->deferrable;
		form.condeferred = constraint->initdeferred;
		form.conenforced = form.contype == CONSTRAINT_CHECK ? constraint->is_enforced : true;
		form.convalidated = !constraint->skip_validation;
		form.conislocal = true;
		form.connoinherit = form.contype == CONSTRAINT_FOREIGN || constraint->is_no_inherit;
		if (metadata[i])
		{
			char *local = json_value(metadata[i], "islocal");
			char *inherited = json_value(metadata[i], "inhcount");
			char *noinherit = json_value(metadata[i], "noinherit");

			form.conislocal = !local || !strcmp(local, "true");
			form.coninhcount = inherited ? pg_strtoint32(inherited) : 0;
			if (noinherit) form.connoinherit = !strcmp(noinherit, "true");
		}
		form.confupdtype = ' ';
		form.confdeltype = ' ';
		form.confmatchtype = ' ';
		if (form.contype == CONSTRAINT_PRIMARY || form.contype == CONSTRAINT_UNIQUE)
			form.conindid = CatalogViewLookupObject(view, RelationRelationId, dg_qualified_name(strVal(linitial(parts)), names[i]));
		keys = palloc_array(Datum, Max(desc->natts, 1));
		if (expression)
		{
			Bitmapset  *attributes = NULL;
			int			att = -1;

			pull_varattnos(expression, 1, &attributes);
			while ((att = bms_next_member(attributes, att)) >= 0)
			{
				int			number = att + FirstLowInvalidHeapAttributeNumber;

				keys[k++] = Int16GetDatum(number);
				provider_dependency(rows, ConstraintRelationId, form.oid, 0, RelationRelationId, relid, number, DEPENDENCY_AUTO);
			}
			bms_free(attributes);
			/* Constant checks still depend on their owning relation. */
			if (!k)
				provider_dependency(rows, ConstraintRelationId, form.oid, 0, RelationRelationId, relid, 0, DEPENDENCY_AUTO);
			provider_expression_dependencies(rows, ConstraintRelationId, form.oid, expression);
		}
		foreach(lc, constraint->keys)
		{
			int			att = provider_attribute(desc, strVal(lfirst(lc)));

			keys[k++] = Int16GetDatum(att);
			provider_dependency(rows, ConstraintRelationId, form.oid, 0, RelationRelationId, relid, att, DEPENDENCY_AUTO);
		}
		tuple = provider_fixed(rows->relations[slot], &form, sizeof(form), Anum_pg_constraint_conperiod);
		tuple = provider_set(rows->relations[slot], tuple, Anum_pg_constraint_conkey, PointerGetDatum(construct_array(keys, k, INT2OID, 2, true, TYPALIGN_SHORT)));
		if (expression)
			tuple = provider_set(rows->relations[slot], tuple, Anum_pg_constraint_conbin, CStringGetTextDatum(nodeToString(expression)));
		if (form.contype == CONSTRAINT_FOREIGN)
			tuple = provider_foreign(rows, view, tuple, constraint, desc, metadata[i], count, definitions, kinds);
		else if (form.conindid && form.condeferrable)
			provider_unique_trigger(rows, view, (Form_pg_constraint) GETSTRUCT(tuple));
		rows->rows[slot] = lappend(rows->rows[slot], tuple);
		if (form.conindid)
			provider_dependency(rows, RelationRelationId, form.conindid, 0, ConstraintRelationId, form.oid, 0, DEPENDENCY_INTERNAL);
	}
}

static void
provider_index(DGProviderRows *rows, CatalogView *view, const char *definition, int count, char **definitions, char **kinds)
{
	DGSchema   *schema = dg_schema_find(definition);
	IndexStmt  *stmt = schema_index(definition);
	char	   *table;
	TupleDesc	desc = NULL;
	FormData_pg_index form = {0};
	int16	   *keys,
			   *options;
	Oid		   *classes,
			   *types,
			   *collations;
	bool		resolved = false,
				builtin = true;
	ListCell   *lc;
	List	   *elements;
	HeapTuple	tuple;
	int			i,
				k = 0,
				slot = provider_catalog_index(IndexRelationId);

	if (!stmt)
		elog(ERROR, "invalid canonical index definition");
	if (stmt->whereClause || stmt->options || strcmp(stmt->accessMethod, "dolt_prolly_index"))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unsupported canonical index properties")));
	if (!stmt->relation->schemaname)
		elog(ERROR, "unqualified canonical index table");
	table = dg_qualified_name(stmt->relation->schemaname, stmt->relation->relname);
	i = provider_member(table, "table");
	if (i < 0)
		elog(ERROR, "canonical index table is missing");
	desc = provider_columns(definitions[i]);
	form.indexrelid = CatalogViewLookupObject(view, RelationRelationId, json_value(definition, "name"));
	form.indrelid = CatalogViewLookupObject(view, RelationRelationId, table);
	form.indnkeyatts = list_length(stmt->indexParams);
	elements = list_concat_copy(stmt->indexParams, stmt->indexIncludingParams);
	form.indnatts = list_length(elements);
	form.indisunique = stmt->unique;
	form.indnullsnotdistinct = stmt->nulls_not_distinct;
	form.indimmediate = true;
	form.indisvalid = true;
	form.indisready = true;
	form.indislive = true;
	foreach(lc, rows->rows[provider_catalog_index(ConstraintRelationId)])
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT((HeapTuple) lfirst(lc));

		if (con->conindid == form.indexrelid && (con->contype == CONSTRAINT_PRIMARY || con->contype == CONSTRAINT_UNIQUE))
		{
			form.indisprimary = con->contype == CONSTRAINT_PRIMARY;
			form.indimmediate = !con->condeferrable;
		}
	}
	keys = palloc0_array(int16, form.indnatts);
	options = palloc0_array(int16, form.indnkeyatts);
	classes = palloc0_array(Oid, form.indnkeyatts);
	types = palloc0_array(Oid, form.indnkeyatts);
	collations = palloc0_array(Oid, form.indnkeyatts);
	foreach(lc, elements)
	{
		IndexElem  *elem = lfirst_node(IndexElem, lc);
		Form_pg_attribute attr;

		if (!elem->name || elem->expr || elem->opclassopts)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("canonical expression indexes and opclass options are not supported")));
		keys[k] = provider_attribute(desc, elem->name);
		attr = TupleDescAttr(desc, keys[k] - 1);
		if (k < form.indnkeyatts)
		{
			/*
			 * A built-in type's default operator class is kept with the
			 * schema; a named one resolves through the search path.
			 */
			types[k] = attr->atttypid;
			if (schema && (schema->parts & DG_SCHEMA_OPCLASSES) && schema->opclass_types[k] == types[k])
				classes[k] = schema->opclasses[k];
			else
			{
				classes[k] = ResolveOpClass(elem->opclass, attr->atttypid, "dolt_prolly_index", DOLT_PROLLY_INDEX_AM_OID);
				resolved = true;
			}
			if (elem->opclass || types[k] >= FirstNormalObjectId || classes[k] >= FirstNormalObjectId)
				builtin = false;
			collations[k] = elem->collation ? get_collation_oid(elem->collation, false) : attr->attcollation;
			if (elem->ordering == SORTBY_DESC)
				options[k] |= INDOPTION_DESC;
			if (elem->nulls_ordering == SORTBY_NULLS_FIRST || (elem->nulls_ordering == SORTBY_NULLS_DEFAULT && elem->ordering == SORTBY_DESC))
				options[k] |= INDOPTION_NULLS_FIRST;
		}
		provider_dependency(rows, RelationRelationId, form.indexrelid, 0, RelationRelationId, form.indrelid, keys[k], DEPENDENCY_AUTO);
		k++;
	}
	if (schema && resolved && builtin)
		dg_schema_set_opclasses(schema, form.indnkeyatts, types, classes);
	tuple = provider_fixed(rows->relations[slot], &form, sizeof(form), Anum_pg_index_indisreplident);
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_index_indkey, PointerGetDatum(buildint2vector(keys, form.indnatts)));
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_index_indclass, PointerGetDatum(buildoidvector(classes, form.indnkeyatts)));
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_index_indcollation, PointerGetDatum(buildoidvector(collations, form.indnkeyatts)));
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_index_indoption, PointerGetDatum(buildint2vector(options, form.indnkeyatts)));
	rows->rows[slot] = lappend(rows->rows[slot], tuple);
}

#include "dg_routine_provider.inc"
#include "dg_schema_object_provider.inc"
#include "dg_type_provider.inc"
#include "dg_view_provider.inc"
#include "dg_trigger_provider.inc"

/* Shared role references are derived from canonical owners and ACLs. They
 * remain runtime catalog tuples; local role OIDs never enter schema payloads. */
static void
provider_role_dependency(DGProviderRows *rows, Oid catalog, Oid object,
						 Oid role, char kind)
{
	FormData_pg_shdepend dep = {0};

	if (!OidIsValid(role))
		return;					/* PUBLIC is not a role object */
	dep.dbid = MyDatabaseId;
	dep.classid = catalog;
	dep.objid = object;
	dep.refclassid = AuthIdRelationId;
	dep.refobjid = role;
	dep.deptype = kind;
	provider_add(rows, SharedDependRelationId, &dep, sizeof(dep), Natts_pg_shdepend);
}

static void
provider_role_dependencies(DGProviderRows *rows)
{
	const Oid	catalogs[] = {NamespaceRelationId, RelationRelationId, TypeRelationId, ProcedureRelationId, OperatorRelationId};
	const int	owners[] = {Anum_pg_namespace_nspowner, Anum_pg_class_relowner, Anum_pg_type_typowner, Anum_pg_proc_proowner, Anum_pg_operator_oprowner};
	const int	acls[] = {Anum_pg_namespace_nspacl, Anum_pg_class_relacl, Anum_pg_type_typacl, Anum_pg_proc_proacl, 0};

	for (int i = 0; i < lengthof(catalogs); i++)
	{
		int			slot = provider_catalog_index(catalogs[i]);
		TupleDesc	desc = RelationGetDescr(rows->relations[slot]);
		ListCell   *lc;

		foreach(lc, rows->rows[slot])
		{
			HeapTuple	tuple = lfirst(lc);
			bool		isnull;
			Oid			object = DatumGetObjectId(heap_getattr(tuple, 1, desc, &isnull));
			Oid			owner;
			Datum		acl;

			if (object < FirstDurableObjectId)
				continue;		/* bootstrap objects are shared */
			owner = DatumGetObjectId(heap_getattr(tuple, owners[i], desc, &isnull));
			provider_role_dependency(rows, catalogs[i], object, owner, SHARED_DEPENDENCY_OWNER);
			if (!acls[i]) continue;
			acl = heap_getattr(tuple, acls[i], desc, &isnull);
			if (!isnull)
			{
				Oid		   *members;
				int			count = aclmembers(DatumGetAclP(acl), &members);

				for (int j = 0; j < count; j++)
					if (members[j] != owner)
						provider_role_dependency(rows, catalogs[i], object, members[j], SHARED_DEPENDENCY_ACL);
				if (members)
					pfree(members);
			}
		}
	}
}

static bool
provider_has_type(DGProviderRows *rows, Oid namespaceid, const char *name)
{
	ListCell   *lc;

	foreach(lc, rows->rows[provider_catalog_index(TypeRelationId)])
	{
		Form_pg_type type = (Form_pg_type) GETSTRUCT((HeapTuple) lfirst(lc));

		if (type->typnamespace == namespaceid && !strcmp(NameStr(type->typname), name))
			return true;
	}
	return false;
}

/*
 * Bind a table, index or row type. On a branch head it takes the object's
 * durable OID and, for relations, its storage resource; a revision's objects
 * take backend-local handles.
 */
/*
 * Bind namespace |name| to the server's schema of that name, so that it has
 * one OID on every branch, public 2200 among them, and server objects in it
 * are in it on every branch that has it. A name no schema has here yet, as a
 * clone's history may hold, takes a handle of its own.
 */
static Oid
provider_namespace(CatalogView *view, const char *name)
{
	CatalogView *previous = CatalogViewSwitch(NULL);
	Oid			oid = InvalidOid;

	PG_TRY();
	{
		oid = get_namespace_oid(name, true);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	if (!OidIsValid(oid))
		return CatalogViewBindObject(view, NamespaceRelationId, name, InvalidOid);
	CatalogViewBindShared(view, NamespaceRelationId, name, oid);
	return oid;
}

/* The server objects a view leaves out: those in namespaces it does not have. */
typedef struct ProviderHidden
{
	List	   *namespaces;
	HTAB	   *objects;
} ProviderHidden;

static bool
provider_server_row_visible(Oid catalog, HeapTuple tuple, void *arg)
{
	ProviderHidden *hidden = arg;
	Oid			namespace = provider_row_namespace(catalog, tuple);
	Oid			owner;

	if (OidIsValid(namespace))
		return !list_member_oid(hidden->namespaces, namespace);
	/* Every catalog a view holds begins with a fixed OID column. */
	owner = provider_row_owner(catalog, tuple, *(Oid *) GETSTRUCT(tuple));
	return !hash_search(hidden->objects, &owner, HASH_FIND, NULL);
}

/*
 * Leave out of |view| the server objects whose namespace it does not have,
 * and the rows describing them. Only a branch that drops or renames such a
 * namespace needs this; others take the base as it is.
 */
static void
provider_filter_server_objects(CatalogView *view)
{
	ProviderHidden *hidden = NULL;
	MemoryContext previous;
	HASHCTL		ctl = {0};

	foreach_oid(namespace, provider_server_namespaces)
	{
		bool		bound = false;

		foreach_ptr(HeapTupleData, row, provider_current_rows->rows[provider_catalog_index(NamespaceRelationId)])
			bound |= ((Form_pg_namespace) GETSTRUCT(row))->oid == namespace;
		if (bound)
			continue;
		if (!hidden)
		{
			previous = MemoryContextSwitchTo(CatalogViewMemory(view));
			hidden = palloc0(sizeof(*hidden));
			ctl.keysize = sizeof(Oid);
			ctl.entrysize = sizeof(Oid);
			ctl.hcxt = CatalogViewMemory(view);
			hidden->objects = hash_create("hidden server objects", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
			MemoryContextSwitchTo(previous);
		}
		previous = MemoryContextSwitchTo(CatalogViewMemory(view));
		hidden->namespaces = lappend_oid(hidden->namespaces, namespace);
		MemoryContextSwitchTo(previous);
	}
	if (!hidden)
		return;
	for (int i = 0; i < provider_server_object_count; i++)
		if (list_member_oid(hidden->namespaces, provider_server_objects[i].namespace))
			(void) hash_search(hidden->objects, &provider_server_objects[i].object, HASH_ENTER, NULL);
	CatalogViewSetBaseFilter(view, provider_server_row_visible, hidden);
}

static Oid
provider_bind(CatalogView *view, const char *branch, Oid objectclass, char kind,
			  const char *name, const char *identity)
{
	Oid			existing = CatalogViewFindObject(view, objectclass, identity);
	Oid			oid;
	Oid			storage = InvalidOid;

	if (OidIsValid(existing))
		return existing;
	if (branch && OidIsValid(oid = dg_identity_lookup(branch, kind, name)))
	{
		/* A relation without a binding fails when accessed, not here. */
		if (kind == DOLT_IDENTITY_RELATION)
			storage = binding_storage(branch, name);
		CatalogViewBindDurable(view, objectclass, identity, oid, storage);
		if (provider_current_entry)
		{
			MemoryContext previous = MemoryContextSwitchTo(provider_current_entry->memory);
			ProviderBinding *binding = palloc(offsetof(ProviderBinding, name) + strlen(name) + 1);

			binding->kind = kind;
			binding->oid = oid;
			binding->storage = storage;
			strcpy(binding->name, name);
			provider_current_entry->bindings = lappend(provider_current_entry->bindings, binding);
			MemoryContextSwitchTo(previous);
		}
		return oid;
	}
	return CatalogViewBindObject(view, objectclass, identity, InvalidOid);
}

/*
 * Row-type layout is independent of the table's columns. Build it before any
 * composite attributes or table columns: either may name another table's row
 * type, including through an array. Resolving that layout through syscache
 * would recursively load the catalog view still being assembled.
 */
static void
provider_table_type(DGProviderRows *rows, CatalogView *view, bytea *database,
					const char *branch, const char *definition)
{
	char	   *name = json_value(definition, "name");
	List	   *parts = stringToQualifiedNameList(name, NULL);
	FormData_pg_class form = {0};
	FormData_pg_type type = {0};
	char	   *arrayname;
	int			arraypass = 0;
	char	   *arrayidentity;
	FormData_pg_type array = {0};

	form.oid = CatalogViewFindObject(view, RelationRelationId, name);
	form.reltype = CatalogViewFindObject(view, TypeRelationId, name);
	form.relnamespace = CatalogViewLookupObject(view, NamespaceRelationId, strVal(linitial(parts)));
	form.relowner = get_role_oid(json_value(definition, "owner"), false);
	namestrcpy(&form.relname, strVal(lsecond(parts)));
	arrayname = makeObjectName("", NameStr(form.relname), NULL);

	for (;;)
	{
		arrayidentity = dg_qualified_name(strVal(linitial(parts)), arrayname);
		if (provider_member(arrayidentity, NULL) < 0 &&
			!provider_has_type(rows, form.relnamespace, arrayname))
			break;
		pfree(arrayname);
		arrayname = makeObjectName("", NameStr(form.relname), psprintf("%d", ++arraypass));
	}
	array.oid = provider_bind(view, branch, TypeRelationId, DOLT_IDENTITY_ARRAYTYPE, name, arrayidentity);
	type.typarray = array.oid;

	type.oid = form.reltype;
	type.typname = form.relname;
	type.typnamespace = form.relnamespace;
	type.typowner = form.relowner;
	type.typlen = -1;
	type.typtype = TYPTYPE_COMPOSITE;
	type.typcategory = TYPCATEGORY_COMPOSITE;
	type.typisdefined = true;
	type.typdelim = ',';
	type.typrelid = form.oid;
	type.typinput = F_RECORD_IN;
	type.typoutput = F_RECORD_OUT;
	type.typreceive = F_RECORD_RECV;
	type.typsend = F_RECORD_SEND;
	type.typalign = TYPALIGN_DOUBLE;
	type.typstorage = TYPSTORAGE_EXTENDED;
	type.typtypmod = -1;
	provider_add(rows, TypeRelationId, &type, sizeof(type), Anum_pg_type_typcollation);
	provider_dependency(rows, TypeRelationId, type.oid, 0, RelationRelationId, form.oid, 0, DEPENDENCY_INTERNAL);
	namestrcpy(&array.typname, arrayname);
	array.typnamespace = type.typnamespace;
	array.typowner = type.typowner;
	array.typlen = -1;
	array.typtype = TYPTYPE_BASE;
	array.typcategory = TYPCATEGORY_ARRAY;
	array.typisdefined = true;
	array.typdelim = ',';
	array.typelem = type.oid;
	array.typinput = F_ARRAY_IN;
	array.typoutput = F_ARRAY_OUT;
	array.typreceive = F_ARRAY_RECV;
	array.typsend = F_ARRAY_SEND;
	array.typanalyze = F_ARRAY_TYPANALYZE;
	array.typsubscript = F_ARRAY_SUBSCRIPT_HANDLER;
	array.typalign = TYPALIGN_DOUBLE;
	array.typstorage = TYPSTORAGE_EXTENDED;
	array.typtypmod = -1;
	provider_add(rows, TypeRelationId, &array, sizeof(array), Anum_pg_type_typcollation);
	provider_dependency(rows, TypeRelationId, array.oid, 0, TypeRelationId, type.oid, 0, DEPENDENCY_INTERNAL);
}

#include "dg_sequence_provider.inc"

static void
provider_populate(CatalogView *view, bytea *database, const char *branch)
{
	DGProviderRows rows = {0};
	List	   *inherited = NIL;
	DGMember   *listed;
	bytea	  **addresses,
			  **schemas,
			  **contents;
	char	  **kinds,
			  **definitions;
	int			n,
				i,
				j;

	database_check_extensions(database);
	provider_current_view = view;
	provider_current_rows = &rows;
	CatalogViewSetBase(view, provider_shared_base());
	for (i = 0; i < lengthof(provider_catalogs); i++)
		rows.relations[i] = table_open(provider_catalogs[i], AccessShareLock);
	listed = dg_database_members(database, &n);
	addresses = palloc_array(bytea *, Max(n, 1));
	schemas = palloc0_array(bytea *, Max(n, 1));
	contents = palloc0_array(bytea *, Max(n, 1));
	kinds = palloc_array(char *, Max(n, 1));
	definitions = palloc_array(char *, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		addresses[i] = listed[i].hash;
		kinds[i] = (char *) listed[i].kind;
	}
	for (i = 0; i < n; i++)
	{
		if (!strcmp(kinds[i], "namespace"))
			definitions[i] = metadata(addresses[i], "namespace");
		else if (schema_object_class(kinds[i]) || !strcmp(kinds[i], "table") || !strcmp(kinds[i], "index") || !strcmp(kinds[i], "routine") || !strcmp(kinds[i], "trigger") || !strcmp(kinds[i], "type") || !strcmp(kinds[i], "view") || !strcmp(kinds[i], "sequence"))
		{
			schemas[i] = child(addresses[i], 1);
			definitions[i] = metadata(schemas[i], "schema");
			if (!strcmp(kinds[i], "table") || !strcmp(kinds[i], "index"))
				contents[i] = child(addresses[i], 2);
		}
		else
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unsupported canonical catalog member %s", kinds[i])));
	}
	provider_definition_count = n;
	provider_definition_kinds = kinds;
	provider_definition_list = definitions;
	provider_index_members();
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "namespace"))
		{
			FormData_pg_namespace form = {0};
			int			slot = provider_catalog_index(NamespaceRelationId);
			char	   *name = json_value(definitions[i], "name");
			char	   *owner = json_value(definitions[i], "owner");
			HeapTuple	tuple;

			form.oid = provider_namespace(view, name);
			namestrcpy(&form.nspname, name);
			form.nspowner = get_role_oid(owner, false);
			tuple = provider_fixed(rows.relations[slot], &form, sizeof(form), Anum_pg_namespace_nspowner);
			tuple = provider_set(rows.relations[slot], tuple, Anum_pg_namespace_nspacl, PointerGetDatum(provider_acl(definitions[i])));
			rows.rows[slot] = lappend(rows.rows[slot], tuple);
			provider_description(&rows, form.oid, NamespaceRelationId, 0, json_value(definitions[i], "comment"));
		}
	for (i = 0; i < n; i++)
	{
		Oid catalog = schema_object_class(kinds[i]);

		if (!strcmp(kinds[i], "routine")) catalog = ProcedureRelationId;
		if (catalog) CatalogViewBindObject(view, catalog, json_value(definitions[i], "name"), InvalidOid);
	}
	provider_filter_server_objects(view);
	/* Bind all class identities before resolving cross-object edges. */
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "table") || !strcmp(kinds[i], "index") || !strcmp(kinds[i], "sequence"))
			(void) provider_bind(view, branch, RelationRelationId, DOLT_IDENTITY_RELATION,
								 json_value(definitions[i], "name"), json_value(definitions[i], "name"));

	/*
	 * Types, tables and routines name one another's types: bind them all,
	 * then build user-defined types before the tables that use them.
	 */
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "table"))
			(void) provider_bind(view, branch, TypeRelationId, DOLT_IDENTITY_ROWTYPE,
								 json_value(definitions[i], "name"), json_value(definitions[i], "name"));
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "type"))
			provider_type_bind(&rows, view, definitions[i]);
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "view"))
		{
			/* Views may use one another. */
			(void) CatalogViewBindObject(view, RelationRelationId, json_value(definitions[i], "name"), InvalidOid);
			(void) CatalogViewBindObject(view, TypeRelationId, json_value(definitions[i], "name"), InvalidOid);
		}
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "type"))
			provider_type(&rows, view, definitions[i]);
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "table"))
			provider_table_type(&rows, view, database, branch, definitions[i]);
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "type"))
			provider_type_relation(&rows, view, definitions[i]);

	/* The tables some table inherits from, which have subclasses. */
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "table"))
		{
			JsonbValue	document = json_document(definitions[i]),
						parents;

			if (json_get(&document, "inherits", &parents))
				for (int p = 0; p < json_length(&parents); p++)
					inherited = lappend(inherited, json_item_text(&parents, p));
		}
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "table") || !strcmp(kinds[i], "index"))
		{
			const char *definition = definitions[i];
			char	   *name = json_value(definition, "name");
			char	   *owner = json_value(definition, "owner");
			List	   *parts = stringToQualifiedNameList(name, NULL);
			FormData_pg_class form = {0};
			TupleDesc	desc = provider_columns(definition);
			bool		index = !strcmp(kinds[i], "index");
			JsonbValue	document;
			JsonbValue	list;
			char	   *matview;
			HeapTuple	tuple;
			int			slot = provider_catalog_index(RelationRelationId);

			if (list_length(parts) != 2)
				elog(ERROR, "canonical relation name is not qualified");
			form.oid = CatalogViewFindObject(view, RelationRelationId, name);
			CatalogViewObjectSetRetained(view, form.oid, schemas[i], contents[i]);
			form.relnamespace = CatalogViewLookupObject(view, NamespaceRelationId, strVal(linitial(parts)));
			namestrcpy(&form.relname, strVal(lsecond(parts)));
			form.relowner = get_role_oid(owner, false);
			form.reltype = index ? InvalidOid : provider_bind(view, branch, TypeRelationId, DOLT_IDENTITY_ROWTYPE, name, name);
			form.relam = index ? DOLT_PROLLY_INDEX_AM_OID : DOLT_PROLLY_TABLE_AM_OID;
			form.relkind = index ? RELKIND_INDEX : RELKIND_RELATION;
			matview = index ? NULL : json_value(definition, "matview");
			if (matview)
			{
				/* A materialized view is a table with its view's rule. */
				form.relkind = RELKIND_MATVIEW;
				form.relhasrules = true;
			}
			form.relpersistence = RELPERSISTENCE_PERMANENT;
			form.relnatts = desc->natts;
			document = json_document(definition);
			foreach_ptr(char, parent, inherited)
				if (!strcmp(parent, name))
					form.relhassubclass = true;
			form.relchecks = 0;
			if (json_get(&document, "constraints", &list))
				for (j = 0; j < json_length(&list); j++)
				{
					JsonbValue	constraint = json_element(&list, j);
					char	   *text = json_item_text(&constraint, 1);

					if (text && !strncmp(text, "CHECK ", 6))
						form.relchecks++;
				}
			form.reltuples = -1;
			form.relispopulated = !matview || !strcmp(json_value(matview, "populated"), "true");
			form.relreplident = REPLICA_IDENTITY_DEFAULT;
			form.relhasindex = json_get(&document, "indexes", &list) && json_length(&list) > 0;
			tuple = provider_fixed(rows.relations[slot], &form, sizeof(form), Anum_pg_class_relminmxid);
			tuple = provider_set(rows.relations[slot], tuple, Anum_pg_class_relacl, PointerGetDatum(provider_acl(definition)));
			rows.rows[slot] = lappend(rows.rows[slot], tuple);
			provider_description(&rows, form.oid, RelationRelationId, 0, json_value(definition, "comment"));
			provider_dependency(&rows, RelationRelationId, form.oid, 0, NamespaceRelationId, form.relnamespace, 0, DEPENDENCY_NORMAL);
			{
				Datum	   *statistics = palloc0_array(Datum, Max(desc->natts, 1)),
						   *options = palloc0_array(Datum, Max(desc->natts, 1));
				bool	   *nostatistics = palloc_array(bool, Max(desc->natts, 1)),
						   *nooptions = palloc_array(bool, Max(desc->natts, 1));

				/* Per-column statistics targets and options. */
				if (!json_get(&document, "columns", &list) || json_length(&list) != desc->natts)
					elog(ERROR, "canonical column count mismatch");
				for (j = 0; j < desc->natts; j++)
				{
					JsonbValue	column = json_element(&list, j);
					JsonbValue	items;
					char	   *text = json_field_text(&column, "statistics");
					int			count = json_get(&column, "options", &items) ? json_length(&items) : 0;

					nostatistics[j] = text == NULL;
					if (text)
						statistics[j] = DirectFunctionCall1(int4in, CStringGetDatum(text));
					nooptions[j] = count == 0;
					if (count)
					{
						Datum	   *elements = palloc_array(Datum, count);
						bool	   *elementnulls = palloc_array(bool, count);
						int			dims[1] = {count};
						int			lbs[1] = {1};

						for (int k = 0; k < count; k++)
						{
							char	   *option = json_item_text(&items, k);

							elementnulls[k] = option == NULL;
							elements[k] = option ? CStringGetTextDatum(option) : (Datum) 0;
						}
						options[j] = PointerGetDatum(construct_md_array(elements, elementnulls, 1, dims, lbs, TEXTOID, -1, false, TYPALIGN_INT));
					}
				}
				for (j = 0; j < desc->natts; j++)
				{
					Form_pg_attribute attr = TupleDescAttr(desc, j);
					int			aslot = provider_catalog_index(AttributeRelationId);
					HeapTuple	atuple;

					attr->attrelid = form.oid;
					atuple = provider_fixed(rows.relations[aslot], attr, ATTRIBUTE_FIXED_PART_SIZE, Anum_pg_attribute_attcollation);
					if (!nostatistics[j])
						atuple = provider_set(rows.relations[aslot], atuple, Anum_pg_attribute_attstattarget, statistics[j]);
					if (!nooptions[j] && ArrayGetNItems(ARR_NDIM(DatumGetArrayTypeP(options[j])), ARR_DIMS(DatumGetArrayTypeP(options[j]))) > 0)
						atuple = provider_set(rows.relations[aslot], atuple, Anum_pg_attribute_attoptions, options[j]);
					rows.rows[aslot] = lappend(rows.rows[aslot], atuple);
					provider_dependency(&rows, RelationRelationId, form.oid, j + 1, TypeRelationId, attr->atttypid, 0, DEPENDENCY_NORMAL);
				}
			}
			if (!index)
			{
				int			attno;

				for (attno = -1; attno > FirstLowInvalidHeapAttributeNumber; attno--)
				{
					FormData_pg_attribute attr = *SystemAttributeDefinition(attno);

					attr.attrelid = form.oid;
					provider_add(&rows, AttributeRelationId, &attr, sizeof(attr), Anum_pg_attribute_attcollation);
				}
			}
			if (json_get(&document, "columns", &list))
				for (j = 0; j < json_length(&list); j++)
				{
					JsonbValue	column = json_element(&list, j);

					provider_description(&rows, form.oid, RelationRelationId, j + 1, json_field_text(&column, "comment"));
				}
			if (matview)
				provider_view_rule(&rows, view, name, form.oid, matview);
		}
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "view"))
			provider_view_member(&rows, view, definitions[i]);
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "routine") || !strcmp(kinds[i], "aggregate"))
			provider_routine(&rows, view, !strcmp(kinds[i], "aggregate") ? json_value(definitions[i], "routine") : definitions[i]);
	for (i = 0; i < n; i++)
		if (schema_object_class(kinds[i]))
			provider_schema_object(&rows, view, kinds[i], definitions[i]);

	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "sequence"))
		{
			provider_sequence(&rows, view, definitions[i]);
			CatalogViewObjectSetRetained(view, CatalogViewFindObject(view, RelationRelationId, json_value(definitions[i], "name")), schemas[i], NULL);
		}

	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "table"))
		{
			JsonbValue document = json_document(definitions[i]), parents;
			Oid childid = CatalogViewLookupObject(view, RelationRelationId, json_value(definitions[i], "name"));

			if (json_get(&document, "inherits", &parents))
				for (int p = 0; p < json_length(&parents); p++)
				{
					FormData_pg_inherits form = {0};

					form.inhrelid = childid;
					form.inhparent = CatalogViewLookupObject(view, RelationRelationId, json_item_text(&parents, p));
					form.inhseqno = p + 1;
					provider_add(&rows, InheritsRelationId, &form, sizeof(form), Natts_pg_inherits);
					provider_dependency(&rows, RelationRelationId, childid, 0, RelationRelationId, form.inhparent, 0, DEPENDENCY_NORMAL);
				}
		}

	/*
	 * Table expressions may call versioned routines, which they name; the
	 * routines and their schemas are visible while those are bound.
	 */
	CatalogViewAddRows(view, NamespaceRelationId, rows.rows[provider_catalog_index(NamespaceRelationId)]);
	CatalogViewAddRows(view, ProcedureRelationId, rows.rows[provider_catalog_index(ProcedureRelationId)]);
	CatalogViewAddRows(view, AggregateRelationId, rows.rows[provider_catalog_index(AggregateRelationId)]);
	CatalogViewAddRows(view, OperatorRelationId, rows.rows[provider_catalog_index(OperatorRelationId)]);
	CatalogViewAddRows(view, CastRelationId, rows.rows[provider_catalog_index(CastRelationId)]);
	CatalogViewAddRows(view, EnumRelationId, rows.rows[provider_catalog_index(EnumRelationId)]);
	CatalogViewAddRows(view, TypeRelationId, rows.rows[provider_catalog_index(TypeRelationId)]);
	/* Whole-row expressions also need the relation's row type and attributes. */
	CatalogViewAddRows(view, RelationRelationId, rows.rows[provider_catalog_index(RelationRelationId)]);
	CatalogViewAddRows(view, AttributeRelationId, rows.rows[provider_catalog_index(AttributeRelationId)]);
	CatalogViewSetPartial(view, true);
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "type") && !strcmp(json_value(definitions[i], "kind"), "d"))
			provider_domain(&rows, view, definitions[i]);
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "table"))
		{
			provider_defaults(&rows, view, definitions[i]);
			provider_constraints(&rows, view, definitions[i], n, definitions, kinds);
		}
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "index"))
			provider_index(&rows, view, definitions[i], n, definitions, kinds);
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "trigger"))
			provider_trigger(&rows, view, definitions[i], n, definitions, kinds);
	for (i = 0; i < n; i++)
	{
		/* Comments on schema objects kept apart from tables. */
		Oid			catalog = !strcmp(kinds[i], "routine") ? ProcedureRelationId :
			!strcmp(kinds[i], "type") ? TypeRelationId :
			!strcmp(kinds[i], "trigger") ? TriggerRelationId :
			!strcmp(kinds[i], "view") ? RelationRelationId : InvalidOid;
		char	   *comment = OidIsValid(catalog) ? json_value(definitions[i], "comment") : NULL;

		if (comment)
			provider_description(&rows, CatalogViewLookupObject(view, catalog, json_value(definitions[i], "name")),
								 catalog, 0, comment);
	}
	provider_role_dependencies(&rows);
	for (i = 0; i < lengthof(provider_catalogs); i++)
	{
		/* Domain defaults have since been bound into their pg_type rows. */
		if (provider_catalogs[i] != NamespaceRelationId && provider_catalogs[i] != ProcedureRelationId &&
			provider_catalogs[i] != EnumRelationId)
			CatalogViewAddRows(view, provider_catalogs[i], rows.rows[i]);
		table_close(rows.relations[i], AccessShareLock);
	}
	provider_current_view = NULL;
	provider_current_rows = NULL;
	provider_definition_count = 0;
	provider_members = NULL;
	provider_column_cache = NULL;
	CatalogViewSeal(view);
}

static void
provider_reload(CatalogView *view, void *arg)
{
	DGProvider *entry = arg;
	DGProvider *saved_entry = provider_current_entry;
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;
	CatalogView *previous = CatalogViewSwitch(NULL);
	MemoryContext caller = CurrentMemoryContext;
	MemoryContext scratch = AllocSetContextCreate(caller, "catalog reload", ALLOCSET_DEFAULT_SIZES);

	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bytea	   *database;

		MemoryContextSwitchTo(scratch);
		if (!ActiveSnapshotSet())
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			pushed = true;
		}
		nest = begin_api();
		database = hash_parse(entry->database);
		if (!object_read(database, "database", NULL, NULL, NULL))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("revision does not name a visible database object")));
		/* The view keeps its durable OIDs, and so the bindings recorded. */
		provider_current_entry = NULL;
		provider_populate(view, database, entry->branch);
		end_api(nest);
	}
	PG_FINALLY();
	{
		provider_current_entry = saved_entry;
		if (pushed)
			PopActiveSnapshot();
		MemoryContextSwitchTo(caller);
		MemoryContextDelete(scratch);
		SetUserIdAndSecContext(user, security);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
}

static TransactionId
provider_creator(bytea *database)
{
	TransactionId creator = InvalidTransactionId;
	bool		found = false;

	if (!object_creator(database, "database", &found, &creator))
		elog(ERROR, "canonical catalog root disappeared");
	return creator;
}

/*
 * The catalog view of schema |database|: as a branch head's working schema
 * when |branch| is given, else as a read-only revision. The two differ only
 * in object identity, so one schema can be cached under both.
 */
static CatalogView *
provider_lookup(const char *branch, bytea *database)
{
	char	   *key = hash_format(database);
	DGProvider *entry;
	DGProvider *saved_entry;
	CatalogView *view;
	MemoryContext memory;
	Oid			user;
	int			security,
				nest;

	/*
	 * Visibility is required even on a cache hit: an aborted object must not
	 * survive through a backend cache.
	 */
	if (!object_read(database, "database", NULL, NULL, NULL))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("revision does not name a visible database object")));
	if (!identity_callback)
	{
		CacheRegisterRelcacheCallback(provider_identities_changed, (Datum) 0);
		RegisterXactCallback(provider_xact, NULL);
		identity_callback = true;
	}
	for (entry = providers; entry; entry = entry->next)
	{
		if (!strcmp(entry->database, key) && provider_current(entry, branch))
		{
			if (CatalogViewCreatorAborted(entry->view))
				CatalogViewSetCreator(entry->view, provider_creator(database));
			CatalogViewEnsureLoaded(entry->view);
			return entry->view;
		}
	}
	/*
	 * A branch's newer view replaces its views of earlier roots; a revision's
	 * view is only ever replaced by one of the same revision.
	 */
	for (entry = providers; entry; entry = entry->next)
		if (branch ? entry->branch && !strcmp(entry->branch, branch) :
			!entry->branch && !strcmp(entry->database, key))
			entry->superseded = true;
	view = CatalogViewCreate(branch ? psprintf("%s@%s", key, branch) : key);
	if (branch)
		CatalogViewMarkBranchHead(view);

	/*
	 * Register before populating, so lookups made while building the view,
	 * including of its own durable objects, find it rather than recurse.
	 */
	memory = AllocSetContextCreate(TopMemoryContext, "catalog provider", ALLOCSET_SMALL_SIZES);
	entry = MemoryContextAllocZero(memory, sizeof(*entry));
	entry->memory = memory;
	entry->database = MemoryContextStrdup(memory, key);
	entry->branch = branch ? MemoryContextStrdup(memory, branch) : NULL;
	entry->view = view;
	entry->generation = identity_generation;
	entry->extension_generation = extension_generation;
	entry->base_generation = base_generation;
	entry->next = providers;
	providers = entry;
	saved_entry = provider_current_entry;
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		/* Decoding the schema runs internal SQL, in its own scope. */
		nest = begin_api();
		CatalogViewSetCreator(view, provider_creator(database));
		provider_current_entry = entry;
		provider_populate(view, database, branch);
		provider_current_entry = saved_entry;
		end_api(nest);
	}
	PG_CATCH();
	{
		provider_current_entry = saved_entry;
		SetUserIdAndSecContext(user, security);
		providers = entry->next;
		MemoryContextDelete(memory);
		CatalogViewRelease(view);
		PG_RE_THROW();
	}
	PG_END_TRY();
	SetUserIdAndSecContext(user, security);
	CatalogViewSetLoader(view, provider_reload, entry);
	return view;
}

static CatalogView *
provider_view(bytea *database)
{
	return provider_lookup(NULL, database);
}

/* The current schema of |branch|'s head, with its objects' durable OIDs. */
static CatalogView *
provider_branch_view(const char *branch, bytea *database)
{
	return provider_lookup(branch, database);
}

/*
 * The working schema of |branch| under a fresh catalog snapshot: the latest
 * committed schema and this transaction's own changes, as PostgreSQL reads
 * physical catalogs. NULL if the branch has no working set.
 */
static bytea *
catalog_working_schema(const char *branch)
{
	Relation	catalog = table_open(DoltRefRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	bytea	   *result = NULL;

	InvalidateCatalogSnapshot();
	ScanKeyInit(&key, Anum_pg_dolt_ref_refname, BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(psprintf("working/%s", branch)));
	scan = systable_beginscan(catalog, DoltRefKeyIndexId, true, NULL, 1, &key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		bool		isnull;
		Datum		target = heap_getattr(tuple, Anum_pg_dolt_ref_reftarget, RelationGetDescr(catalog), &isnull);

		if (!isnull)
			result = DatumGetByteaPCopy(target);
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return result;
}

/*
 * The committed content of durable relation |oid| in its branch's working
 * schema: what the relation holds until the branch writes a root of its own.
 */
bytea *
dg_catalog_durable_content(Oid oid)
{
	char	   *branch,
			   *name;
	char		kind;

	if (!dg_identity_describe(oid, &branch, &kind, &name) || kind != DOLT_IDENTITY_RELATION)
		return NULL;
	return CatalogViewObjectContent(oid);
}

/*
 * The content of the table or index that leased OID |handle| names in its
 * revision: immutable, so the relation needs no storage root.
 */
bytea *
dg_catalog_revision_content(Oid handle)
{
	return CatalogViewObjectContent(handle);
}

/*
 * The current view for durable OID |oid|: its branch's working schema under
 * the catalog snapshot. NULL if no branch defines the OID.
 */
CatalogView *
dg_catalog_resolve_durable(Oid oid)
{
	char	   *branch,
			   *name;
	char		kind;
	CatalogView *result = NULL;
	CatalogView *previous;

	if (!IsTransactionState() || !dg_identity_describe(oid, &branch, &kind, &name))
		return NULL;
	previous = CatalogViewSwitch(NULL);

	/* The schema is read as catalogs are: latest committed, not as of the
	 * transaction's snapshot. */
	PushActiveSnapshot(GetLatestSnapshot());
	PG_TRY();
	{
		bytea	   *working = catalog_working_schema(branch);

		if (working)
			result = provider_branch_view(branch, working);
	}
	PG_FINALLY();
	{
		PopActiveSnapshot();
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

CatalogView *
dg_catalog_view(const char *database, const char *branch)
{
	Oid			user;
	int			security,
				nest;
	CatalogView *result;
	CatalogView *previous = CatalogViewSwitch(NULL);

	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		nest = begin_api();
		result = provider_lookup(branch, hash_parse(database));
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}
