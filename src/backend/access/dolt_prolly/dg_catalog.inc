/* Nested SQL inherits an immutable provider selected by its bound arguments. */
static const char *
catalog_current_database(void)
{
	DGProvider *entry;
	CatalogView *view = CatalogViewCurrent();

	if (!view || dg_execution_branch())
		return NULL;
	for (entry = providers; entry; entry = entry->next)
		if (entry->view == view)
			return entry->database;
	return NULL;
}

/* The branch head whose schema the current context view shows, if any. */
static const char *
catalog_current_branch(void)
{
	DGProvider *entry;
	CatalogView *view = CatalogViewCurrent();

	if (!view || dg_execution_branch())
		return NULL;
	for (entry = providers; entry; entry = entry->next)
		if (entry->view == view)
			return entry->branch;
	return NULL;
}

/* Names resolve on |branch|'s head when given, else in revision |db|. */
static CatalogView *
context_view(bytea *db, const char *branch)
{
	return branch ? provider_branch_view(branch, db) : provider_view(db);
}

/* All SQL entry paths use the provider's complete catalog family set. */
static bool
catalog_supported(Oid id)
{
	int			i;

	for (i = 0; i < lengthof(provider_catalogs); i++)
		if (provider_catalogs[i] == id)
			return true;
	return false;
}

/* Versioned metadata lookup. SPI is restricted to fixed internal read queries;
 * permissions on the selected symbolic objects are checked for the caller. */
static bool
catalog_acl_allows(const char *definition, const char *privilege, Oid user)
{
	char	   *owner = json_value(definition, "owner");
	Oid			role;
	int			i,
				n;
	char	  **grantees;

	if (superuser_arg(user))
		return true;

	/* Predefined roles grant data access without ACL entries, as for heap. */
	if ((!strcmp(privilege, "SELECT") || !strcmp(privilege, "USAGE")) &&
		has_privs_of_role(user, ROLE_PG_READ_ALL_DATA))
		return true;
	if ((!strcmp(privilege, "INSERT") || !strcmp(privilege, "UPDATE") ||
		 !strcmp(privilege, "DELETE") || !strcmp(privilege, "USAGE")) &&
		has_privs_of_role(user, ROLE_PG_WRITE_ALL_DATA))
		return true;
	role = owner ? get_role_oid(owner, true) : InvalidOid;
	if (OidIsValid(role) && has_privs_of_role(user, role))
		return true;
	run("SELECT v->>1 FROM jsonb_array_elements($1::jsonb->'acl') v WHERE v->>2=$2", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(privilege)), NULL, true);
	n = SPI_processed;
	grantees = palloc(sizeof(char *) * Max(n, 1));
	for (i = 0; i < n; i++)
		grantees[i] = strcell(i, 1);
	for (i = 0; i < n; i++)
	{
		if (!strcmp(grantees[i], "PUBLIC"))
			return true;
		role = get_role_oid(grantees[i], true);
		if (OidIsValid(role) && has_privs_of_role(user, role))
			return true;
	}
	return false;
}

static void
catalog_acl(const char *definition, const char *privilege, Oid user)
{
	if (!catalog_acl_allows(definition, privilege, user))
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("permission denied for versioned object")));
}

void
dg_catalog_permission(const char *descriptor, Oid user)
{
	Oid			saved;
	int			security,
				nest;
	char	   *schema;

	GetUserIdAndSecContext(&saved, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		nest = begin_api();
		schema = metadata(child(hash_parse(descriptor), 1), "schema");
		catalog_acl(schema, "SELECT", user);
		end_api(nest);
	} PG_FINALLY();
	{
		SetUserIdAndSecContext(saved, security);
	} PG_END_TRY();
}

/* Namespace descriptions are decoded through the same canonical provider. */
/* The definition of namespace |name| in schema |db|, or NULL. */
static char *
catalog_namespace_find(bytea *db, const char *name)
{
	int			n,
				i;
	DGMember   *members = dg_database_members(db, &n);

	for (i = 0; i < n; i++)
	{
		char	   *definition;
		char	   *candidate;

		if (strcmp(members[i].kind, "namespace"))
			continue;
		definition = metadata(members[i].hash, "namespace");
		candidate = json_value(definition, "name");

		if (candidate && !strcmp(candidate, name))
			return definition;
	}
	return NULL;
}

static char *
catalog_namespace(bytea *db, const char *name)
{
	char	   *definition = catalog_namespace_find(db, name);

	if (!definition)
		elog(ERROR, "missing retained namespace");
	return definition;
}

/* Resolve symbolic membership in SQL search-path order. Bootstrap catalogs are
 * candidates in that same order, never a shortcut ahead of an explicit path. */
static char *catalog_resolve_candidates(bytea *db, const char *branch, const RangeVar *rv, Oid user, Oid *native, const char *search_path, bool type_lookup, bool temp_ok);

/*
 * Resolve in revision |db|, on |branch|'s head when given. Its catalog view is
 * built only if a name reaches it: system and native names need no view.
 */
static char *
catalog_resolve_name(bytea *db, const char *branch, const RangeVar *rv, Oid user, Oid *native, const char *search_path, bool type_lookup, bool temp_ok)
{
	CatalogView *previous = CatalogViewSwitch(NULL);
	char	   *result;

	/*
	 * Canonical candidates use the explicit database root. Shared system and
	 * temporary candidates use their physical namespace, even in nested
	 * scopes.
	 */
	PG_TRY();
	{
		result = catalog_resolve_candidates(db, branch, rv, user, native, search_path, type_lookup, temp_ok);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

static char *
catalog_resolve_candidates(bytea *db, const char *branch, const RangeVar *rv, Oid user, Oid *native, const char *search_path, bool type_lookup, bool temp_ok)
{
	CatalogView *view = NULL;
	List	   *path = NIL;
	ListCell   *lc;
	bool		explicit = rv->schemaname != NULL;
	Oid			temp,
				toast;

	GetTempNamespaceState(&temp, &toast);

	*native = InvalidOid;
	if (explicit)
		path = list_make1(rv->schemaname);
	else
	{
		char	   *raw = pstrdup(search_path);
		bool		catalog = false;

		if (!SplitIdentifierString(raw, ',', &path))
			elog(ERROR, "invalid search_path");
		foreach(lc, path)
			if (!strcmp((char *) lfirst(lc), "pg_catalog"))
			catalog = true;
		if (!catalog)
			path = lcons("pg_catalog", path);
		if (temp_ok && OidIsValid(temp))
		{
			bool		listed = false;

			foreach(lc, path)
				if (!strcmp((char *) lfirst(lc), "pg_temp") ||
					!strcmp((char *) lfirst(lc), get_namespace_name(temp)))
				listed = true;
			if (!listed)
				path = lcons("pg_temp", path);
		}
	}
	foreach(lc, path)
	{
		char	   *ns = (char *) lfirst(lc);
		char	   *name;
		Oid			namespace;

		if (!strcmp(ns, "$user"))
			ns = GetUserNameFromId(user, false);
		if (dg_system_namespace(ns))
		{
			namespace = get_namespace_oid(ns, true);
			*native = OidIsValid(namespace) ? (type_lookup ? GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum(rv->relname), ObjectIdGetDatum(namespace)) : get_relname_relid(rv->relname, namespace)) : InvalidOid;
			if (OidIsValid(*native))
				return ns;
			continue;
		}
		if (temp_ok && OidIsValid(temp) && (!strcmp(ns, "pg_temp") ||
											!strcmp(ns, get_namespace_name(temp))))
		{
			*native = type_lookup ? GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum(rv->relname), ObjectIdGetDatum(temp)) : get_relname_relid(rv->relname, temp);
			if (OidIsValid(*native))
				return get_namespace_name(temp);
			continue;
		}
		/*
		 * The revision's catalog view holds every member decoded, with its
		 * namespace's ACL: resolve there, as PostgreSQL resolves names in
		 * its catalogs.
		 */
		name = dg_qualified_name(ns, rv->relname);
		if (db && !view)
			view = context_view(db, branch);
		if (view && OidIsValid(CatalogViewFindObject(view, type_lookup ? TypeRelationId : RelationRelationId, name)))
		{
			namespace = CatalogViewFindObject(view, NamespaceRelationId, ns);
			if (!OidIsValid(namespace))
				elog(ERROR, "missing retained namespace");
			if (object_aclcheck(NamespaceRelationId, namespace, user, ACL_USAGE) != ACLCHECK_OK)
			{
				if (explicit)
					aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_SCHEMA, ns);
				continue;
			}
			return ns;
		}

		/* Unversioned native objects live beside the branch's objects. */
		namespace = get_namespace_oid(ns, true);
		if (OidIsValid(namespace))
		{
			Oid			physical = type_lookup ?
				GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum(rv->relname), ObjectIdGetDatum(namespace)) :
				get_relname_relid(rv->relname, namespace);

			if (!OidIsValid(physical))
				continue;
			if (object_aclcheck(NamespaceRelationId, namespace, user, ACL_USAGE) != ACLCHECK_OK)
			{
				if (explicit)
					aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_SCHEMA, ns);
				continue;
			}
			*native = physical;
			return ns;
		}
	}
	return NULL;
}

/*
 * Whether the database has prolly storage. From its first prolly table on,
 * every prolly reference resolves through a branch's canonical schema and
 * routines and schemas are versioned; a database without one is ordinary
 * PostgreSQL. Storage bindings are kept for history, so this never reverts.
 */
bool
dg_catalog_active(void)
{
	static bool active_committed;
	Relation	bindings;
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		active;

	if (!IsUnderPostmaster || catalog_api_depth || rangevar_projection_hook)
		return false;

	/*
	 * An extension's script makes server objects and names only those and
	 * built-in ones: it works on the physical catalogs, which hold the objects
	 * it has just made, as if the database had no branches. Branch views list
	 * its objects once the extension is complete.
	 */
	if (creating_extension)
		return false;
	if (active_committed)
		return true;
	/* A repository may contain routines or types before its first table.
	 * Use a catalog snapshot: parser callers need not have an active snapshot. */
	{
		ScanKeyData key;

		bindings = table_open(DoltRefRelationId, AccessShareLock);
		ScanKeyInit(&key, Anum_pg_dolt_ref_refname, BTEqualStrategyNumber,
					F_TEXTEQ, CStringGetTextDatum("views/enabled"));
		scan = systable_beginscan(bindings, DoltRefKeyIndexId, true, NULL, 1, &key);
		tuple = systable_getnext(scan);
		active = HeapTupleIsValid(tuple);
		if (active && !TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple->t_data)))
			active_committed = true;
		systable_endscan(scan);
		table_close(bindings, AccessShareLock);
		if (active)
			return true;
	}
	bindings = table_open(DoltBindingRelationId, AccessShareLock);
	scan = systable_beginscan(bindings, InvalidOid, false, NULL, 0, NULL);
	tuple = systable_getnext(scan);
	active = HeapTupleIsValid(tuple);

	/* A committed binding is permanent; one of this transaction's may abort. */
	if (active && !TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple->t_data)))
		active_committed = true;
	systable_endscan(scan);
	table_close(bindings, AccessShareLock);
	return active;
}

/* schema.dolt_<table> names the built-in alias unless the schema defines it. */
static const RangeVar *
system_alias(const RangeVar *rv)
{
	RangeVar   *alias;

	if (rv->schemaname && !strcmp(rv->schemaname, "public") &&
		(!strcmp(rv->relname, "dolt_docs") || !strcmp(rv->relname, "dolt_tests") || !strcmp(rv->relname, "dolt_query_catalog")))
		return rv;
	if (!rv->schemaname || !strcmp(rv->schemaname, "pg_catalog") || !OidIsValid(dg_system_relation(rv)))
		return rv;
	if (OidIsValid(get_namespace_oid(rv->schemaname, true)) &&
		OidIsValid(get_relname_relid(rv->relname, get_namespace_oid(rv->schemaname, true))))
		return rv;
	alias = copyObject(rv);
	alias->schemaname = "pg_catalog";
	return alias;
}

/*
 * Bindings of session names on the current branch, by name, search path,
 * user and target use. A binding depends on the branch's working schema,
 * read again on each use and compared, and on physical catalogs, which the
 * invalidation counter tracks. Both unchanged, the binding is the same.
 */
typedef struct BindCacheEntry
{
	uint64		hash;			/* of key_text */
	char	   *key_text;
	bytea	   *schema;			/* the working schema bound against */
	uint64		invalidations;
	char	   *dolt_database,
			   *dolt_ref,
			   *dolt_ref_target,
			   *dolt_branch;
	Oid			dolt_relid,
				dolt_rowtype;
	TupleDesc	desc;
} BindCacheEntry;

static HTAB *bind_cache;

static BindCacheEntry *
bind_cache_find(const char *key_text, bool enter)
{
	uint64		hash = hash_bytes_extended((const unsigned char *) key_text, strlen(key_text), 0);
	BindCacheEntry *entry;
	bool		found;

	if (!bind_cache)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(uint64);
		ctl.entrysize = sizeof(BindCacheEntry);
		ctl.hcxt = CacheMemoryContext;
		bind_cache = hash_create("prolly bindings", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	entry = hash_search(bind_cache, &hash, enter ? HASH_ENTER : HASH_FIND, &found);
	if (entry && found && strcmp(entry->key_text, key_text) != 0)
	{
		/* A different key with the same hash takes the slot. */
		if (!enter)
			return NULL;
		found = false;
	}
	if (entry && !found)
	{
		MemoryContext old = MemoryContextSwitchTo(CacheMemoryContext);

		memset(((char *) entry) + sizeof(uint64), 0, sizeof(*entry) - sizeof(uint64));
		entry->key_text = pstrdup(key_text);
		MemoryContextSwitchTo(old);
	}
	return entry;
}

static char *
bind_cache_key(const RangeVar *rv, const RangeTblEntry *rte, const char *revision, const char *search_path, Oid user, bool target)
{
	return psprintf("%s\n%s\n%s\n%s\n%s\n%u\n%d", rv->schemaname ? rv->schemaname : "", rv->relname, revision,
					rte->dolt_database ? rte->dolt_database : "", search_path, user, target);
}

static TupleDesc
catalog_bind(ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte, bool target)
{
	BindCacheEntry *cached = NULL;
	bytea	   *bound_schema = NULL;	/* the working schema the binding used */
	bool		plain = false;
	bool		cacheable = false;
	const char *sep = NULL;
	char	   *resolution_path = pstrdup(GetConfigOption("search_path", false, false));
	char	   *revision,
			   *dbname,
			   *logical = NULL;
	Oid			user = GetUserId();
	bytea	   *db;
	CatalogView *view;

	TupleDesc	result = NULL;
	bool		pushed = false;
	bool		working = false;
	Oid			live = InvalidOid;
	/* Stored system-view rules inherit both schema and branch identity. */
	const char *context_branch = rte->dolt_branch;
	Snapshot	outer;
	bool		session_name;

	/*
	 * In a provisional copy of a branch's catalogs, names resolve in the copy,
	 * as ordinary relations: what DDL there analyzes must refer to the copy.
	 */
	if (rangevar_projection_hook || catalog_api_depth)
		return NULL;
	if (CatalogViewIsProvisional(CatalogViewCurrent()))
	{
		/* What a schema object stores names what it uses unqualified by revision. */
		if (rv->dolt_revision || (rv->catalogname && strpbrk(rv->catalogname, "@/")))
			unsupported("revision-qualified references in versioned schema objects");
		return NULL;
	}
	rv = system_alias(rv);
	session_name = !rv->dolt_revision && !rv->catalogname && !rte->dolt_database;
	if (!rte->dolt_database && !rv->dolt_revision &&
		!(rv->catalogname && strpbrk(rv->catalogname, "@/")) && catalog_current_database())
	{
		rte->dolt_database = pstrdup(catalog_current_database());
		context_branch = catalog_current_branch();
	}
	if (!rv->dolt_revision)
	{
		if (!rv->catalogname || !(sep = strpbrk(rv->catalogname, "@/")))
		{
			if (!rte->dolt_database && !dg_catalog_active())
				return NULL;
			if (rv->catalogname && strcmp(rv->catalogname, get_database_name(MyDatabaseId)))
				return NULL;
			revision = pstrdup(context_branch ? context_branch : dg_current_branch());
			plain = true;
			goto revision_selected;
		}
		dbname = pnstrdup(rv->catalogname, sep - rv->catalogname);
		if (strcmp(dbname, get_database_name(MyDatabaseId)))
			return NULL;
		if (strlen(rv->catalogname) == NAMEDATALEN - 1)
			ereport(ERROR, (errcode(ERRCODE_NAME_TOO_LONG), errmsg("revision-qualified identifier may have been truncated"), errhint("Use AS OF 'full hash' or a shorter qualifier.")));
		revision = pstrdup(sep + 1);
	}
	else
	{
		Node	   *expression = transformExpr(pstate, copyObject(rv->dolt_revision), EXPR_KIND_OTHER);

		expression = coerce_to_specific_type(pstate, expression, TEXTOID, "AS OF");
		assign_expr_collations(pstate, expression);
		expression = eval_const_expressions(NULL, expression);
		if (!IsA(expression, Const) || ((Const *) expression)->constisnull)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("AS OF requires a non-null constant revision expression")));
		revision = TextDatumGetCString(((Const *) expression)->constvalue);
		if (rv->catalogname)
		{
			sep = strpbrk(rv->catalogname, "@/");
			dbname = sep ? pnstrdup(rv->catalogname, sep - rv->catalogname) : rv->catalogname;
			if (strcmp(dbname, get_database_name(MyDatabaseId)))
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cross-database versioned queries are not supported")));
			/* HEAD in a qualified relation belongs to that branch, not the session. */
			if (sep && pg_strncasecmp(revision, "HEAD", 4) == 0 &&
				(revision[4] == '\0' || revision[4] == '~' || revision[4] == '^'))
				revision = psprintf("%s%s", sep + 1, revision + 4);
		}
	}
revision_selected:
	if (target && rv->dolt_revision)
		unsupported("historical write targets");
	if (plain)
	{
		cached = bind_cache_find(bind_cache_key(rv, rte, revision, resolution_path, user, target), false);
		if (cached && cached->desc && cached->invalidations == SharedInvalidMessageCounter)
		{
			/*
			 * A binding against the session's database is keyed by that
			 * database's hash; one against the branch's working schema is
			 * checked against the schema as it stands now.
			 */
			bytea	   *now = cached->schema ? catalog_working_schema(revision) : NULL;

			if (!cached->schema ||
				(now && VARSIZE_ANY_EXHDR(now) == VARSIZE_ANY_EXHDR(cached->schema) &&
				 memcmp(VARDATA_ANY(now), VARDATA_ANY(cached->schema), VARSIZE_ANY_EXHDR(now)) == 0))
			{
				rte->dolt_database = pstrdup(cached->dolt_database);
				rte->dolt_ref = pstrdup(cached->dolt_ref);
				rte->dolt_ref_target = pstrdup(cached->dolt_ref_target);
				rte->dolt_branch = pstrdup(cached->dolt_branch);
				rte->dolt_relid = cached->dolt_relid;
				rte->dolt_rowtype = cached->dolt_rowtype;
				return CreateTupleDescCopy(cached->desc);
			}
		}
	}
	/*
	 * Names on a branch head resolve against its latest schema, as against
	 * catalogs. A SQL query of a catalog itself reads it under the query's
	 * snapshot, and so does a revision reference resolve its ref.
	 */
	outer = ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
	if (!rte->dolt_database && !rv->dolt_revision && !(rv->catalogname && strchr(rv->catalogname, '@')))
	{
		PushActiveSnapshot(GetLatestSnapshot());
		pushed = true;
	}
	else if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	{
		working = (!rte->dolt_database || context_branch) && !rv->dolt_revision &&
			read_ref(psprintf("heads/%s", revision)).target != NULL;
		db = rte->dolt_database ? hash_parse(rte->dolt_database) :
			(working ? (bound_schema = catalog_working_schema(revision)) : NULL);
		if (!db)
		{
			db = child(resolve(revision), 1);
			if (resolved_ref)
			{
				rte->dolt_ref = psprintf("%c:%s", resolved_session ? 's' : 'r', resolved_ref);
				rte->dolt_ref_target = hash_format(resolved_target);
			}
		}
		else if (working)
		{
			rte->dolt_ref = psprintf("%c:working/%s", (context_branch || sep) ? 'c' : 's', revision);
			rte->dolt_ref_target = hash_format(db);
		}
		rte->dolt_database = hash_format(db);
		{
			Oid			native;
			char	   *ns = catalog_resolve_name(db, working ? revision : NULL, rv, user, &native, resolution_path, false, true);
			RangeVar   *resolved = copyObject(rv);

			if (!ns)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg(working ? "relation %s does not exist on branch %s" : "relation %s does not exist in revision %s", rv->relname, revision)));
			if (OidIsValid(native) && isTempNamespace(get_rel_namespace(native)))
			{
				if (rv->dolt_revision || (rv->catalogname && strpbrk(rv->catalogname, "@/")))
					unsupported("revision-qualified temporary relations");
				goto binding_done;
			}

			/* An unversioned native relation exists only on the live database. */
			if (OidIsValid(native) && !dg_system_namespace(ns))
			{
				if (target && sep)
					unsupported("branch-qualified writes to unversioned relations");
				if (!working)
					ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist in revision %s", rv->relname, revision)));
				goto binding_done;
			}
			resolved->schemaname = pstrdup(ns);
			rv = resolved;
		}

		if (rv->schemaname && dg_system_namespace(rv->schemaname))
		{
			Relation	catalog;
			Oid			id = get_relname_relid(rv->relname, get_namespace_oid(rv->schemaname, false));

			/* Built-in system tables are written natively (e.g. dolt_docs). */
			if (target)
				goto binding_done;
			if (!catalog_supported(id) && get_rel_relkind(id) != RELKIND_VIEW)
			{
				if (rv->dolt_revision || (rv->catalogname && strpbrk(rv->catalogname, "@/")))
					provider_catalog_index(id);
				goto binding_done;
			}
			if (get_rel_relkind(id) != RELKIND_VIEW)
				provider_catalog_index(id); /* reject unsupported catalogs
											 * explicitly */
			/* Stored-rule RTEs retain native checkAsUser permission entries. */
			if (pstate && pg_class_aclcheck(id, user, ACL_SELECT) != ACLCHECK_OK)
				aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, rv->relname);
			catalog = table_open(id, AccessShareLock);
			result = CreateTupleDescCopy(RelationGetDescr(catalog));
			rte->dolt_catalog = id;
			if (get_rel_relkind(id) == RELKIND_VIEW)
			{
				if (id >= FirstNormalObjectId)
					unsupported("revision-qualified user views");
				rte->relkind = RELKIND_VIEW;
				rte->relid = id;
			}
			rte->dolt_rowtype = get_rel_type_id(id);

			/* Catalog rows show a branch head's objects by durable OID. */
			if (working)
			{
				rte->dolt_branch = pstrdup(revision);
				if (pushed)
				{
					PushActiveSnapshot(outer);
					rte->dolt_database = hash_format(read_ref(psprintf("working/%s", revision)).target);
					PopActiveSnapshot();
				}
			}
			table_close(catalog, AccessShareLock);
		}
		else
		{
			Oid			handle;
			char		relkind;
			Relation	rel;
			Oid			currentns,
						currentrel;

			/*
			 * The view has every member of the revision decoded and bound to
			 * its OID: no need to read the retained objects again. Permissions
			 * come from its ACL rows, as for physical catalogs.
			 */
			logical = dg_qualified_name(rv->schemaname, rv->relname);
			view = context_view(db, working ? revision : NULL);
			handle = CatalogViewFindObject(view, RelationRelationId, logical);
			if (!OidIsValid(handle))
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg(working ? "relation %s does not exist on branch %s" : "relation %s does not exist in revision %s", rv->relname, revision)));
			relkind = get_rel_relkind(handle);
			if (relkind == RELKIND_SEQUENCE)
			{
				if (target)
					unsupported("direct writes to sequence relations");
				if (pg_class_aclcheck(handle, user, ACL_SELECT) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_SEQUENCE, rv->relname);
				rel = relation_open(handle, AccessShareLock);
				result = CreateTupleDescCopy(RelationGetDescr(rel));
				relation_close(rel, AccessShareLock);
				rte->relkind = RELKIND_SEQUENCE;
				rte->relid = handle;
				if (!working)
					rte->dolt_catalog = handle;
				goto binding_done;
			}
			if (relkind == RELKIND_VIEW)
			{
				/*
				 * A versioned view on the session's branch is an ordinary
				 * relation of its catalog view, which the rewriter expands.
				 */
				if (session_name && working)
				{
					rte->dolt_database = NULL;
					goto binding_done;
				}
				if (target)
					unsupported("writes through a revision's view");

				/*
				 * The caller opens the view: its query is analyzed when it
				 * is opened, which cannot bind names within this scope.
				 */
				rte->relkind = RELKIND_VIEW;
				rte->relid = handle;
				result = CreateTemplateTupleDesc(0);
				goto binding_done;
			}
			if (relkind == RELKIND_INDEX)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg(working ? "relation %s does not exist on branch %s" : "relation %s does not exist in revision %s", rv->relname, revision)));
			if (!target && pg_class_aclcheck(handle, user, ACL_SELECT) != ACLCHECK_OK)
				aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, rv->relname);

			/*
			 * Do not let historical grants bypass current revocations on a
			 * live object.
			 */
			currentns = get_namespace_oid(rv->schemaname, true);
			currentrel = OidIsValid(currentns) ? get_relname_relid(rv->relname, currentns) : InvalidOid;
			if (working)
			{
				live = binding_lookup(revision, logical, CatalogViewObjectSchema(handle), ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot());
				if (!OidIsValid(live))
					ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding unavailable for %s on branch %s", logical, revision)));
			}
			else
			{
				live = currentrel;
				if (OidIsValid(currentns) && object_aclcheck(NamespaceRelationId, currentns, user, ACL_USAGE) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_SCHEMA, rv->schemaname);
				if (!target && OidIsValid(currentrel) && pg_class_aclcheck(currentrel, user, ACL_SELECT) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, rv->relname);
			}
			rel = relation_open(handle, AccessShareLock);
			result = CreateTupleDescCopy(RelationGetDescr(rel));
			rte->dolt_rowtype = rel->rd_rel->reltype;
			relation_close(rel, AccessShareLock);
			if (working)
			{
				rte->dolt_branch = pstrdup(revision);
				rte->dolt_relid = live;
				cacheable = plain && rte->dolt_ref && rte->dolt_ref_target;
			}
		}
binding_done:
		if (cacheable)
		{
			char	   *key = bind_cache_key(rv, rte, revision, resolution_path, user, target);
			MemoryContext old = MemoryContextSwitchTo(CacheMemoryContext);
			BindCacheEntry *entry = bind_cache_find(key, true);

			if (entry->desc)
			{
				FreeTupleDesc(entry->desc);
				if (entry->schema)
					pfree(entry->schema);
				pfree(entry->dolt_database);
				pfree(entry->dolt_ref);
				pfree(entry->dolt_ref_target);
				pfree(entry->dolt_branch);
			}
			entry->schema = bound_schema ? (bytea *) datumCopy(PointerGetDatum(bound_schema), false, -1) : NULL;
			entry->invalidations = SharedInvalidMessageCounter;
			entry->dolt_database = pstrdup(rte->dolt_database);
			entry->dolt_ref = pstrdup(rte->dolt_ref);
			entry->dolt_ref_target = pstrdup(rte->dolt_ref_target);
			entry->dolt_branch = pstrdup(rte->dolt_branch);
			entry->dolt_relid = rte->dolt_relid;
			entry->dolt_rowtype = rte->dolt_rowtype;
			entry->desc = CreateTupleDescCopy(result);
			MemoryContextSwitchTo(old);
		}
	}
	if (pushed)
		PopActiveSnapshot();
	return result;
}

/*
 * Whether the refs and local mappings that a cached query's bindings went
 * through still say the same: |refs| holds (ref, target, relid) triples as
 * recorded on its range table entries. Session names must still be on the
 * same branch; a relation's mapping must still be the one its view bound.
 */
bool
dg_catalog_refs_current(List *refs)
{
	ListCell   *lc;
	int			i = 0;
	char	   *ref = NULL,
			   *target = NULL;
	bool		pushed = false;
	bool		current = true;
	Snapshot	latest;

	/* Refs are read under the query's snapshot; mappings as catalogs are. */
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	latest = RegisterSnapshot(GetLatestSnapshot());
	foreach(lc, refs)
	{
		switch (i++ % 3)
		{
			case 0:
				ref = strVal(lfirst(lc));
				break;
			case 1:
				target = strVal(lfirst(lc));
				break;
			default:
				{
					char		kind = ref[0];
					const char *name = ref + 2;
					const char *branch = strchr(name, '/') + 1;
					Oid			relid = intVal(lfirst(lc));
					bytea	   *now;

					if (kind == 's' && strcmp(branch, dg_current_branch()))
						current = false;
					else
					{
						now = strncmp(name, "working/", 8) == 0 ? catalog_working_schema(branch) : read_ref(name).target;
						current = now && !strcmp(hash_format(now), target);
					}
					if (current && OidIsValid(relid))
					{
						const char *logical = CatalogViewObjectIdentity(relid, RelationRelationId);

						current = logical && OidIsValid(binding_lookup(branch, logical, CatalogViewObjectSchema(relid), latest));
					}
				}
		}
		if (!current)
			break;
	}
	UnregisterSnapshot(latest);
	if (pushed)
		PopActiveSnapshot();
	return current;
}

/*
 * A reference bound through the session's branch, such as a versioned routine
 * or type: add the branch's working schema to |refs| as a dependency, once.
 */
void
dg_catalog_session_dependency(List **refs)
{
	const char *branch;
	bytea	   *root;
	char	   *ref;
	ListCell   *lc;
	int			i = 0;

	if (!dg_catalog_active() || dg_execution_branch())
		return;
	branch = dg_current_branch();
	root = catalog_working_schema(branch);
	if (!root)
		return;
	ref = psprintf("s:working/%s", branch);
	foreach(lc, *refs)
		if (i++ % 3 == 0 && !strcmp(strVal(lfirst(lc)), ref))
			return;
	*refs = lappend(*refs, makeString(ref));
	*refs = lappend(*refs, makeString(hash_format(root)));
	*refs = lappend(*refs, makeInteger(0));
}

/* Internal executor/DDL integration boundary. No SQL preparation API. Resolve
 * authority from canonical membership before constructing a private AM relation. */
static Relation
catalog_open_table(const char *branch, const char *name, AclMode permission)
{
	Oid			user;
	int			security,
				nest;
	Relation	result = NULL;
	List	   *parts = stringToQualifiedNameList(name, NULL);
	const char *privilege;

	if (!ActiveSnapshotSet() || list_length(parts) != 2)
		elog(ERROR, "canonical write requires a snapshot and qualified logical name");
	switch (permission)
	{
		case ACL_SELECT:
			privilege = "SELECT";
			break;
		case ACL_INSERT:
			privilege = "INSERT";
			break;
		case ACL_UPDATE:
			privilege = "UPDATE";
			break;
		case ACL_DELETE:
			privilege = "DELETE";
			break;
		default:
			elog(ERROR, "invalid canonical write permission");
	}
	if (permission != ACL_SELECT)
		dg_storage_lock_working(branch, GetActiveSnapshot());
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bytea	   *db,
				   *descriptor,
				   *schema;
		char	   *definition;
		Oid			resource,
					handle;
		DGStorage  *storage;

		nest = begin_api();
		db = read_ref(psprintf("working/%s", branch)).target;
		if (!db)
			elog(ERROR, "canonical working set disappeared");
		descriptor = dg_database_member(db, "table", name);
		if (!descriptor)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist on branch %s", name, branch)));
		schema = child(descriptor, 1);
		definition = metadata(schema, "schema");
		catalog_acl(catalog_namespace(db, strVal(linitial(parts))), "USAGE", user);
		catalog_acl(definition, privilege, user);
		resource = binding_relation(branch, name, schema);
		if (!OidIsValid(resource))
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding unavailable for %s on branch %s", name, branch)));
		handle = CatalogViewLookupObject(provider_branch_view(branch, db), RelationRelationId, name);
		storage = dg_storage_open(handle, branch, name, schema, resource, GetActiveSnapshot());
		result = permission == ACL_SELECT ? dg_storage_enable_read(storage, child(descriptor, 2)) : dg_storage_enable_write(storage, child(descriptor, 2));
		if (permission != ACL_SELECT)
		{
			List	   *ids = RelationGetIndexList(result);
			List	   *indexes = NIL;
			ListCell   *lc;

			foreach(lc, ids)
			{
				Oid			indexid = lfirst_oid(lc);
				char	   *indexname = dg_qualified_name(get_namespace_name(get_rel_namespace(indexid)), get_rel_name(indexid));
				bytea	   *indexdesc,
						   *indexschema;
				Oid			indexresource;
				DGStorage  *indexstorage;

				indexdesc = dg_database_member(db, "index", indexname);
				if (!indexdesc)
					elog(ERROR, "canonical index membership is unavailable");
				indexschema = child(indexdesc, 1);
				indexresource = binding_relation(branch, indexname, indexschema);
				if (!OidIsValid(indexresource))
					elog(ERROR, "canonical index storage binding is unavailable");
				indexstorage = dg_storage_open(indexid, branch, indexname, indexschema, indexresource, GetActiveSnapshot());
				indexes = lappend(indexes, dg_storage_enable_write(indexstorage, child(indexdesc, 2)));
			}
			dg_storage_set_indexes(result, indexes);
			list_free(ids);
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
	}
	PG_END_TRY();
	return result;
}

TupleDesc
dg_catalog_bind(ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte)
{
	return catalog_bind(pstate, rv, rte, false);
}

Relation
dg_catalog_target(ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte)
{
	TupleDesc	desc = catalog_bind(pstate, rv, rte, true);

	if (!desc)
		return NULL;
	if (!rte->dolt_branch || rte->dolt_catalog)
		unsupported("non-working canonical write target");

	/*
	 * A branch head's table is an ordinary relation with a durable OID; the
	 * executor opens and locks it like any other.
	 */
	{
		Oid			relid = get_typ_typrelid(rte->dolt_rowtype);

		if (!CatalogViewIsDurable(relid))
			elog(ERROR, "branch table %u has no durable OID", relid);
		rte->dolt_branch = NULL;
		rte->dolt_relid = InvalidOid;
		return table_open(relid, RowExclusiveLock);
	}
}

/* Name-to-OID conversions (regclass and native utility lookup) obey the same
 * selected branch as relation references. The result is a definition handle;
 * resource authority is obtained separately by supported executor paths. */
bool
dg_catalog_lookup_relation(const RangeVar *rv, bool missing_ok, Oid *result)
{
	Oid			user = GetUserId();
	bool		pushed = false;
	bool		native_found = false;
	char	   *search_path;

	if (CatalogViewIsProvisional(CatalogViewCurrent()))
	{
		if ((!rv->schemaname || dg_system_namespace(rv->schemaname)) && OidIsValid(get_relname_relid(rv->relname, PG_CATALOG_NAMESPACE)))
			return false;
		List	   *path = NIL;
		ListCell   *lc;
		char	   *raw = pstrdup(GetConfigOption("search_path", false, false));

		*result = InvalidOid;
		if (rv->schemaname)
			path = list_make1(pstrdup(rv->schemaname));
		else if (!SplitIdentifierString(raw, ',', &path))
			elog(ERROR, "invalid search_path");
		foreach(lc, path)
		{
			char	   *ns = lfirst(lc);

			if (!strcmp(ns, "$user"))
				ns = GetUserNameFromId(GetUserId(), false);
			*result = CatalogViewFindObject(CatalogViewCurrent(), RelationRelationId,
											dg_qualified_name(ns, rv->relname));
			if (OidIsValid(*result))
				break;
		}

		/* Unversioned native relations, such as sequences, are physical. */
		if (!OidIsValid(*result))
		{
			CatalogView *previous = CatalogViewSwitch(NULL);

			foreach(lc, path)
			{
				char	   *ns = lfirst(lc);
				Oid			namespace;

				if (!strcmp(ns, "$user"))
					ns = GetUserNameFromId(GetUserId(), false);
				namespace = get_namespace_oid(ns, true);
				if (OidIsValid(namespace))
					*result = get_relname_relid(rv->relname, namespace);
				if (OidIsValid(*result))
					break;
			}
			CatalogViewSwitch(previous);
		}
		if (!OidIsValid(*result) && !missing_ok)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist in provisional schema", rv->relname)));
		return true;
	}
	if (!dg_catalog_active())
		return false;
	rv = system_alias(rv);
	if (rv->catalogname && strcmp(rv->catalogname, get_database_name(MyDatabaseId)))
		return false;
	if (rv->dolt_revision)
		unsupported("revision-qualified native utility lookup");
	search_path = pstrdup(GetConfigOption("search_path", false, false));

	/*
	 * Names on the session's branch resolve against its latest committed
	 * schema, as PostgreSQL resolves names against its catalogs.
	 */
	if (!catalog_current_database())
	{
		PushActiveSnapshot(GetLatestSnapshot());
		pushed = true;
	}
	else if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	*result = InvalidOid;
	{
		bytea	   *db;
		const char *branch;

		branch = catalog_current_database() ? catalog_current_branch() : dg_current_branch();
		db = catalog_current_database() ? hash_parse(catalog_current_database()) :
			catalog_working_schema(branch);
		{
			Oid			native;
			char	   *ns = catalog_resolve_name(db, branch, rv, user, &native, search_path, false, true);

			if (OidIsValid(native))
			{
				native_found = true;
				*result = native;
			}
			else if (ns)
				*result = CatalogViewLookupObject(context_view(db, branch), RelationRelationId,
												  dg_qualified_name(ns, rv->relname));
		}
	}
	if (pushed)
		PopActiveSnapshot();
	if (!OidIsValid(*result) && !missing_ok)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist on branch %s", rv->relname, dg_current_branch())));
	return !native_found;
}

Relation
dg_catalog_write_table(const char *branch, const char *name, AclMode permission)
{
	if (permission != ACL_INSERT && permission != ACL_UPDATE && permission != ACL_DELETE)
		elog(ERROR, "invalid canonical write permission");
	return catalog_open_table(branch, name, permission);
}

/* Caller has performed native LOCK TABLE ACL checks on the bound definition. */
bool
dg_catalog_lock_relation(Oid id, LOCKMODE mode, bool nowait)
{
	Relation	definition;
	bool		pushed = false;
	const char *branch = dg_current_branch();
	char	   *name;
	bytea	   *db;
	CatalogView *view;
	Oid			handle,
				resource;

	if (!CatalogViewIsLeased(id))
		return false;
	definition = relation_open(id, NoLock);
	if (!definition->rd_catalogview)
	{
		relation_close(definition, NoLock);
		return false;
	}
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	name = dg_qualified_name(get_namespace_name(RelationGetNamespace(definition)), RelationGetRelationName(definition));
	db = read_ref(branch_ref("working")).target;
	view = db ? provider_branch_view(branch, db) : NULL;
	handle = view ? CatalogViewFindObject(view, RelationRelationId, name) : InvalidOid;
	if (!OidIsValid(handle))
		elog(ERROR, "canonical lock target disappeared");
	resource = binding_lookup(branch, name, CatalogViewObjectSchema(handle), GetActiveSnapshot());
	if (!OidIsValid(resource))
		elog(ERROR, "canonical lock resource unavailable");
	if (!nowait)
		LockRelationOid(resource, mode);
	else if (!ConditionalLockRelationOid(resource, mode))
		ereport(ERROR, (errcode(ERRCODE_LOCK_NOT_AVAILABLE), errmsg("could not obtain lock on relation \"%s\"", RelationGetRelationName(definition))));
	if (pushed)
		PopActiveSnapshot();
	relation_close(definition, NoLock);
	return true;
}

/* Bind catalog RTEs loaded from stored rules through the same provider as SQL
 * RangeVars. Preserve aliases and native permission entries from view expansion.
 * Catalog layouts are bootstrap-stable, so existing Vars remain well typed. */
void
dg_catalog_rewrite_relation(RangeTblEntry *rte)
{
	RangeVar   *rv;
	TupleDesc	desc;
	int			i;

	if (rte->rtekind != RTE_RELATION || catalog_api_depth ||
		rangevar_projection_hook || (!rte->dolt_database && !dg_catalog_active()))
		return;
	for (i = 0; i < lengthof(provider_catalogs); i++)
		if (rte->relid == provider_catalogs[i])
			break;
	if (i == lengthof(provider_catalogs))
		return;
	rv = makeRangeVar("pg_catalog", get_rel_name(rte->relid), -1);
	desc = catalog_bind(NULL, rv, rte, false);
	if (!desc)
		elog(ERROR, "could not bind stored catalog reference");
	rte->rtekind = RTE_DOLT;
	rte->inh = false;
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		rte->coltypes = lappend_oid(rte->coltypes, att->atttypid);
		rte->coltypmods = lappend_int(rte->coltypmods, att->atttypmod);
		rte->colcollations = lappend_oid(rte->colcollations, att->attcollation);
	}
	FreeTupleDesc(desc);
}

/* Visibility is a name-resolution question in the object's own schema context,
 * not membership of its synthetic namespace OID in a physical search path. */
bool
dg_catalog_relation_visible(Oid id, const char *name, bool *visible)
{
	CatalogView *view = CatalogViewObjectContext(id);
	DGProvider *provider;
	char	   *database = NULL;
	const char *branch = NULL;
	char	   *path;
	RangeVar   *rv;
	Oid			user = GetUserId();
	bool		pushed = false;

	if (!view)
		view = CatalogViewCurrent();
	if (catalog_api_depth || rangevar_projection_hook || (!view && !dg_catalog_active()))
		return false;
	for (provider = providers; provider; provider = provider->next)
		if (provider->view == view)
		{
			database = provider->database;
			branch = provider->branch;
		}
	if (view && !database)
		return false;			/* non-production synthetic provider */
	if (!view)
		branch = dg_current_branch();
	path = pstrdup(GetConfigOption("search_path", false, false));
	rv = makeRangeVar(NULL, pstrdup(name), -1);
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	{
		bytea	   *db;
		Oid			native;
		char	   *ns;

		db = database ? hash_parse(database) : read_ref(branch_ref("working")).target;
		ns = catalog_resolve_name(db, branch, rv, user, &native, path, false, true);
		*visible = ns && (OidIsValid(native) ? native == id :
						  CatalogViewLookupObject(context_view(db, branch), RelationRelationId,
												  dg_qualified_name(ns, name)) == id);
	}
	if (pushed)
		PopActiveSnapshot();
	return true;
}

/* Named types resolve against the same ordered namespace membership as tables.
 * A handled miss is authoritative: never fall back to a physical user type. */
bool
dg_catalog_lookup_type(const char *schema, const char *name, bool temp_ok, Oid *result)
{
	Oid			user = GetUserId();
	bool		pushed = false;
	RangeVar   *rv = makeRangeVar((char *) schema, (char *) name, -1);
	char	   *search_path;

	/*
	 * In a provisional copy of a branch's catalogs, as for relations, or in a
	 * view still being built: names resolve to its own types, else to native
	 * types, which are physical.
	 */
	if (CatalogViewIsProvisional(CatalogViewCurrent()) || CatalogViewIsPartial(CatalogViewCurrent()))
	{
		List	   *path = NIL;
		CatalogView *previous;

		if ((!schema || dg_system_namespace(schema)) &&
			OidIsValid(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum(name), ObjectIdGetDatum(PG_CATALOG_NAMESPACE))))
			return false;
		*result = InvalidOid;
		if (schema)
			path = list_make1(pstrdup(schema));
		else if (!SplitIdentifierString(pstrdup(GetConfigOption("search_path", false, false)), ',', &path))
			elog(ERROR, "invalid search_path");
		foreach_ptr(char, ns, path)
		{
			if (!strcmp(ns, "$user"))
				ns = GetUserNameFromId(GetUserId(), false);
			*result = CatalogViewFindObject(CatalogViewCurrent(), TypeRelationId, dg_qualified_name(ns, name));
			if (OidIsValid(*result))
				return true;
		}
		previous = CatalogViewSwitch(NULL);
		foreach_ptr(char, ns, path)
		{
			Oid			namespace;

			if (!strcmp(ns, "$user"))
				ns = GetUserNameFromId(GetUserId(), false);
			namespace = get_namespace_oid(ns, true);
			if (OidIsValid(namespace))
				*result = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum(name), ObjectIdGetDatum(namespace));
			if (OidIsValid(*result))
				break;
		}
		CatalogViewSwitch(previous);
		return true;
	}
	if (!dg_catalog_active())
		return false;
	search_path = pstrdup(GetConfigOption("search_path", false, false));

	/*
	 * Names on the session's branch resolve against its latest committed
	 * schema, as PostgreSQL resolves names against its catalogs.
	 */
	if (!catalog_current_database())
	{
		PushActiveSnapshot(GetLatestSnapshot());
		pushed = true;
	}
	else if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	*result = InvalidOid;
	{
		bytea	   *db;
		const char *branch;

		branch = catalog_current_database() ? catalog_current_branch() : dg_current_branch();
		db = catalog_current_database() ? hash_parse(catalog_current_database()) :
			catalog_working_schema(branch);
		{
			Oid			native;
			char	   *ns = catalog_resolve_name(db, branch, rv, user, &native, search_path, true, temp_ok);

			if (OidIsValid(native))
			{
				*result = native;
			}
			else if (ns)
				*result = CatalogViewLookupObject(context_view(db, branch), TypeRelationId,
												  dg_qualified_name(ns, rv->relname));
		}
	}
	if (pushed)
		PopActiveSnapshot();
	return true;
}

bool
dg_catalog_type_visible(Oid id, const char *name, bool *visible)
{
	CatalogView *view = CatalogViewObjectContext(id);
	CatalogView *previous = CatalogViewSwitch(view ? view : CatalogViewCurrent());
	bool		handled;
	Oid			found;

	PG_TRY();
	{
		handled = dg_catalog_lookup_type(NULL, name, true, &found);
		if (handled)
			*visible = found == id;
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return handled;
}

/*
 * The session view of the current command, which every operator, cast and
 * function lookup asks for: resolved once per command and branch. A ref
 * write invalidates pg_dolt_ref's relcache entry, which drops it too.
 */
static struct
{
	LocalTransactionId lxid;
	CommandId	cid;
	char		branch[NAMEDATALEN * 2];
	CatalogView *view;
	bool		valid;
	bool		callback;
}			session_view;

static void
session_view_invalidate(Datum arg, Oid relid)
{
	if (relid == InvalidOid || relid == DoltRefRelationId)
		session_view.valid = false;
}

/* Compilation scope for argument-free procedural routines. Execution retains
 * its caller's read/write authority; this view only keys cached declarations. */
CatalogView *
dg_catalog_session_view(void)
{
	CatalogView *result = CatalogViewCurrent();
	const char *branch;
	CommandId	cid;

	if (result || !dg_catalog_active())
		return result;

	branch = dg_current_branch();
	cid = GetCurrentCommandId(false);
	if (session_view.valid && session_view.lxid == MyProc->vxid.lxid && session_view.cid == cid &&
		strcmp(session_view.branch, branch) == 0)
		return session_view.view;
	if (!session_view.callback)
	{
		CacheRegisterRelcacheCallback(session_view_invalidate, (Datum) 0);
		session_view.callback = true;
	}

	/* The session's branch schema is read as catalogs are, latest committed. */
	PushActiveSnapshot(GetLatestSnapshot());
	result = provider_branch_view(branch, catalog_working_schema(branch));
	PopActiveSnapshot();
	session_view.valid = strlen(branch) < sizeof(session_view.branch);
	if (session_view.valid)
	{
		strcpy(session_view.branch, branch);
		session_view.lxid = MyProc->vxid.lxid;
		session_view.cid = cid;
		session_view.view = result;
	}
	return result;
}

/*
 * Under a fixed snapshot, a branch lock cannot make the snapshot see a later
 * deletion, so reject roots that changed since the snapshot.
 */
static void
foreign_check_snapshot(Relation rel)
{
	DGState    *visible,
			   *latest;
	bool		same_root;

	if (!IsolationUsesXactSnapshot())
		return;
	visible = dg_load(rel, GetTransactionSnapshot());
	latest = dg_load(rel, GetLatestSnapshot());
	same_root = pl_hash_equal(dg_content(visible)->root, dg_content(latest)->root);
	dg_release(visible);
	dg_release(latest);
	if (!same_root)
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("foreign key relation changed since transaction snapshot")));
}

/*
 * Native prolly tables lock referenced rows (FOR KEY SHARE) like heap.
 * Canonical bindings are read through versioned scans that cannot lock rows
 * yet, so their checks take the branch-level resource lock, which excludes
 * concurrent writers of that branch.
 */
void
dg_catalog_foreign_lock(Relation definition)
{
	CatalogView *previous;
	Oid			user;
	int			security;
	Relation	bound;
	const char *name;

	if (!definition->rd_catalogview || CatalogViewIsDurable(RelationGetRelid(definition)))
		return;
	name = CatalogViewObjectIdentity(RelationGetRelid(definition), RelationRelationId);
	GetUserIdAndSecContext(&user, &security);
	previous = CatalogViewSwitch(NULL);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bound = catalog_open_table(dg_current_branch(), name, ACL_UPDATE);
		dg_storage_lock_branch(bound->rd_lockInfo.lockRelId.relId, dg_storage_branch(bound), ExclusiveLock);
		foreign_check_snapshot(bound);
		dg_storage_close_relation(bound);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
}

/* Reopen a queued event with its original definition and explicit branch.
 * Data roots use the firing snapshot; schema replacement is never substituted. */
Relation
dg_catalog_trigger_relation(Oid id, const char *branch)
{
	CatalogView *view = CatalogViewObjectContext(id);
	CatalogView *previous = CatalogViewSwitch(NULL);
	DGProvider *provider;
	Relation	result = NULL;
	const char *name = CatalogViewObjectIdentity(id, RelationRelationId);

	if (!view || !branch)
		elog(ERROR, "queued trigger has no revision binding");
	for (provider = providers; provider && provider->view != view; provider = provider->next)
	{
	}
	if (!provider)
		elog(ERROR, "queued trigger catalog provider disappeared");
	PG_TRY();
	{
		bytea	   *database,
				   *schema,
				   *content;
		CatalogView *current;
		Oid			handle,
					resource;
		DGStorage  *storage;

		/* The relation as the branch has it now ... */
		database = read_ref(psprintf("working/%s", branch)).target;
		current = database ? provider_branch_view(branch, database) : NULL;
		handle = current ? CatalogViewFindObject(current, RelationRelationId, name) : InvalidOid;
		if (!OidIsValid(handle))
			ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("queued trigger relation disappeared")));
		schema = CatalogViewObjectSchema(handle);
		content = CatalogViewObjectContent(handle);

		/* ... must keep the schema the event's revision had. */
		if (!schema || !same(schema, CatalogViewObjectSchema(id)))
			ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("cannot change a relation with pending versioned trigger events")));
		resource = binding_lookup(branch, name, schema, GetActiveSnapshot());
		storage = dg_storage_open(id, branch, name, schema, resource, GetActiveSnapshot());
		result = dg_storage_enable_write(storage, content);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}
