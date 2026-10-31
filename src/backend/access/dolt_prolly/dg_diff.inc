/* Retained property-tree JSON and native JSONB output have different whitespace
 * and property ordering. Compare their values, not their presentation. */
static bool
same_schema_definition(const char *a, const char *b)
{
	return DatumGetBool(DirectFunctionCall2(jsonb_eq,
											DirectFunctionCall1(jsonb_in, CStringGetDatum(a)),
											DirectFunctionCall1(jsonb_in, CStringGetDatum(b))));
}

/* Row-level diff engine and generated per-table system tables.
 * See doc/doltgres/row-diff.md. Included by dg_version.c. */

/* A table's contents and schema at one revision. Logical rows are keyed by
 * packed primary-key values (keyed tables) or by canonical row digest with
 * an occurrence count (keyless tables), so two revisions align by key. */
typedef struct DiffTable
{
	bool		present;
	char	   *name;			/* qualified logical name */
	char	   *schema;			/* canonical definition JSON */
	TupleDesc	desc;
	int			nkeys;
	int		   *keys;			/* 0-based attribute indexes of the key */
	bool		keyed;
	DGRows		rows;			/* sorted logical rows */
	DGState    *live;			/* borrowed live state, released by diff_close */
	PLStore    *store;			/* private store for imported history */
	char	   *object_kind;	/* non-NULL for a schema-object property view */
} DiffTable;

typedef struct DiffRow
{
	const char *type;			/* added, modified, removed */
	PLValue		from;			/* packed row bytes without occurrence prefix */
	PLValue		to;
} DiffRow;

typedef void (*DiffRowFn) (DiffTable *from, DiffTable *to, DiffRow *row, void *arg);
static void split_dots(const char *spec, char **from, char **to);
static DiffTable *diff_table_schema(const char *revision, const char *name);
static char *optional_text_arg(FunctionCallInfo fcinfo, int n);

static char *
strip_identifier(const char *ident)
{
	StringInfoData out;
	const char *p = ident;

	while (*p == ' ')
		p++;
	if (*p != '"')
	{
		char	   *s = pstrdup(p);

		while (strlen(s) && s[strlen(s) - 1] == ' ')
			s[strlen(s) - 1] = '\0';
		return s;
	}
	initStringInfo(&out);
	for (p++; *p; p++)
	{
		if (*p == '"')
		{
			if (p[1] == '"')
				p++;
			else
				break;
		}
		appendStringInfoChar(&out, *p);
	}
	return out.data;
}

/* A retained row must use its revision's type definitions, not the current
 * session's pg_type rows. The catalog provider already reconstructs the full
 * tuple descriptor, including user types and collations. */
static TupleDesc
schema_descriptor(const char *json, CatalogView *view, int *nkeys, int **keys)
{
	Oid			handle = CatalogViewLookupObject(view, RelationRelationId, json_value(json, "name"));
	Relation	relation = relation_open(handle, AccessShareLock);
	TupleDesc	desc = CreateTupleDescCopy(RelationGetDescr(relation));
	int			n = desc->natts;
	int			i;
	char	   *pk;

	relation_close(relation, AccessShareLock);
	run("SELECT regexp_replace(c->>1,'^PRIMARY KEY \\((.*)\\)$','\\1') FROM jsonb_array_elements($1::jsonb->'constraints') c WHERE c->>1 LIKE 'PRIMARY KEY (%'", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json)), NULL, true);
	pk = SPI_processed ? strcell(0, 1) : NULL;
	*nkeys = 0;
	*keys = palloc(sizeof(int) * Max(n, 1));
	if (pk)
	{
		List	   *parts = NIL;
		char	   *copy = pstrdup(pk),
				   *start = copy;
		bool		quoted = false;
		char	   *p;
		ListCell   *lc;

		for (p = copy;; p++)
		{
			if (*p == '"')
				quoted = !quoted;
			if ((*p == ',' && !quoted) || *p == '\0')
			{
				bool		end = *p == '\0';

				*p = '\0';
				parts = lappend(parts, strip_identifier(start));
				if (end)
					break;
				start = p + 1;
			}
		}
		foreach(lc, parts)
		{
			char	   *column = (char *) lfirst(lc);

			for (i = 0; i < n; i++)
				if (!strcmp(NameStr(TupleDescAttr(desc, i)->attname), column))
					break;
			if (i == n)
				elog(ERROR, "primary key column %s missing from retained schema", column);
			(*keys)[(*nkeys)++] = i;
		}
	}
	return desc;
}

/* Logical rows of a content tree, aligned on primary-key values. */
static DGRows
logical_rows_for(DiffTable *t, const PLTree *content)
{
	DGRows		rows = dg_rows(content);
	DGRows		result;
	size_t		i;

	if (!t->keyed)
		return rows;
	result.items = palloc(sizeof(PLEntry) * Max(rows.count, 1));
	result.count = rows.count;
	result.capacity = rows.count;
	for (i = 0; i < rows.count; i++)
	{
		PLValue    *fields = palloc0(sizeof(*fields) * t->nkeys);
		PLSlice		row = {rows.items[i].value.data + 8, rows.items[i].value.len - 8};
		int			j;

		for (j = 0; j < t->nkeys; j++)
			dg_check(pl_tuple_get(row, t->keys[j], &fields[j]));
		result.items[i].key = pack_merge_fields(fields, t->nkeys);
		result.items[i].value = row;
	}
	qsort(result.items, result.count, sizeof(PLEntry), entry_compare);
	return result;
}

static bool
same_key_columns(DiffTable *a, DiffTable *b)
{
	int			i;

	if (a->nkeys != b->nkeys)
		return false;
	for (i = 0; i < a->nkeys; i++)
		if (strcmp(NameStr(TupleDescAttr(a->desc, a->keys[i])->attname), NameStr(TupleDescAttr(b->desc, b->keys[i])->attname)))
			return false;
	return true;
}

/* Find a table member (schema, chunk) of a database state object. */
static bool
database_member(bytea *db, const char *name, bytea **schema, bytea **chunk)
{
	run("SELECT o.objchildren[1],o.objchildren[2] FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible o ON o.objhash=ch WHERE d.objhash=$1 AND o.objkind='table' AND convert_from(o.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(name)), NULL, true);
	if (!SPI_processed)
		return false;
	*schema = DatumGetByteaPCopy(cell(0, 1));
	*chunk = DatumGetByteaPCopy(cell(0, 2));
	return true;
}

static DiffTable *
diff_table_absent(const char *name)
{
	DiffTable  *t = palloc0(sizeof(DiffTable));

	t->name = pstrdup(name);
	return t;
}

/* Load a table from a retained database state object. */
static DiffTable *
diff_table_at(bytea *db, const char *name, bool rows)
{
	DiffTable  *t = diff_table_absent(name);
	bytea	   *schema,
			   *chunk;
	PLTree		content;

	if (!db || !database_member(db, name, &schema, &chunk))
		return t;
	t->present = true;
	t->schema = metadata(schema, "schema");
	t->desc = schema_descriptor(t->schema, provider_view(db), &t->nkeys, &t->keys);
	t->keyed = t->nkeys > 0;
	if (!rows)
		return t;
	t->store = dg_private_store();
	content = dg_history_import(t->store, chunk);
	t->rows = logical_rows_for(t, &content);
	return t;
}

/* Load the live working contents of a table on the session branch. */
static DiffTable *
diff_table_working(const char *name, bool rows)
{
	DiffTable  *t = diff_table_absent(name);
	bytea	   *db = read_ref(branch_ref("working")).target,
			   *schema,
			   *chunk;
	CatalogView *view;
	Oid			handle,
				resource;
	DGStorage  *storage;

	if (!db || !database_member(db, name, &schema, &chunk))
		return t;
	view = provider_view(db);
	handle = CatalogViewLookupObject(view, RelationRelationId, name);
	resource = binding_relation(dg_current_branch(), name, schema);
	if (!OidIsValid(handle) || !OidIsValid(resource))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding unavailable for %s on branch %s", name, dg_current_branch())));
	t->present = true;
	t->schema = metadata(schema, "schema");
	t->desc = schema_descriptor(t->schema, view, &t->nkeys, &t->keys);
	t->keyed = t->nkeys > 0;
	if (!rows)
		return t;
	storage = dg_storage_open(handle, dg_current_branch(), name, schema, resource, GetActiveSnapshot());
	t->live = dg_storage_load(storage);
	dg_storage_close(storage);
	if (t->live->has_root)
		t->rows = logical_rows_for(t, &t->live->content);
	return t;
}

static void
diff_table_close(DiffTable *t)
{
	if (!t)
		return;
	if (t->live)
		dg_release(t->live);
	if (t->store)
		pl_store_free(t->store);
	t->live = NULL;
	t->store = NULL;
}

/* Revision database object for history revisions; NULL means the live
 * working set. STAGED resolves to the staged state object. */
static bytea *
revision_database(const char *revision, bool *working)
{
	*working = false;
	if (!pg_strcasecmp(revision, "WORKING"))
	{
		*working = true;
		return NULL;
	}
	if (!pg_strcasecmp(revision, "STAGED"))
	{
		VersionRef	r = read_ref(branch_ref("staged"));

		if (!r.target)
			r.target = child(read_ref(branch_ref("heads")).target, 1);
		return r.target;
	}
	return child(resolve(revision), 1);
}

static DiffTable *
diff_table_revision(const char *revision, const char *name)
{
	bool		working;
	bytea	   *db = revision_database(revision, &working);

	return working ? diff_table_working(name, true) : diff_table_at(db, name, true);
}
static DiffTable *
diff_table_schema(const char *revision, const char *name)
{
	bool		working;
	bytea	   *db = revision_database(revision, &working);

	return working ? diff_table_working(name, false) : diff_table_at(db, name, false);
}

/*
 * Non-table definitions are already canonical revisioned JSON. Expose their
 * top-level properties as keyed (property text, value jsonb) rows without
 * allocating relations or consulting the live physical catalog. JSON null is
 * a value; an absent property is an absent row. Nested definitions stay intact.
 */
static DiffTable *
diff_object_revision(const char *revision, const char *name, bool rows)
{
	bool		working;
	bytea	   *db = revision_database(revision, &working);
	char	   *kind;
	bytea	   *schema;
	DiffTable  *t;
	int			i;

	if (working)
		db = read_ref(branch_ref("working")).target;
	if (!db)
		return diff_table_absent(name);
	run("SELECT o.objkind,o.objchildren[1] FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible o ON o.objhash=ch WHERE d.objhash=$1 AND o.objkind IN ('table','routine','view','type','trigger','sequence','aggregate','operator','cast') AND convert_from(o.objpayload,'UTF8')::jsonb->>'name'=$2", 2,
		VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(name)), NULL, true);
	if (!SPI_processed)
		return diff_table_absent(name);
	if (SPI_processed != 1)
		ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_ALIAS), errmsg("ambiguous versioned object name: %s", name)));
	kind = strcell(0, 1);
	schema = DatumGetByteaPCopy(cell(0, 2));
	if (!strcmp(kind, "table"))
		return rows ? diff_table_revision(revision, name) : diff_table_schema(revision, name);

	t = diff_table_absent(name);
	t->present = true;
	t->object_kind = kind;
	t->schema = metadata(schema, "schema");
	t->desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(t->desc, 1, "property", TEXTOID, -1, 0);
	TupleDescInitEntry(t->desc, 2, "value", JSONBOID, -1, 0);
	t->nkeys = 1;
	t->keys = palloc(sizeof(int));
	t->keys[0] = 0;
	t->keyed = true;
	if (!rows)
		return t;

	run("SELECT key,value FROM jsonb_each($1::jsonb - 'format' - 'name') ORDER BY key COLLATE \"C\"", 1,
		VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(t->schema)), NULL, true);
	t->rows.count = t->rows.capacity = SPI_processed;
	t->rows.items = palloc(sizeof(PLEntry) * Max(t->rows.count, 1));
	for (i = 0; i < t->rows.count; i++)
	{
		PLValue		fields[2];

		fields[0] = DG_VALUE(true, dg_encode_value(TEXTOID, cell(i, 1)));
		fields[1] = DG_VALUE(true, dg_encode_value(JSONBOID, cell(i, 2)));
		t->rows.items[i].key = pack_merge_fields(fields, 1);
		t->rows.items[i].value = pack_merge_fields(fields, 2);
	}
	qsort(t->rows.items, t->rows.count, sizeof(PLEntry), entry_compare);
	return t;
}

/* Preserve the established missing-table diagnostic for ordinary selectors. */
static char *
resolve_diff_argument(const char *arg, const char *a, const char *b)
{
	char	   *name;

	/*
	 * Preserve table precedence and diagnostics, including ambiguity outside
	 * search_path. Routine signatures and three-part trigger selectors use
	 * the schema-object resolver instead.
	 */
	if (*arg && arg[strlen(arg) - 1] != ')')
	{
		List	   *parts = stringToQualifiedNameList(arg, NULL);

		if (list_length(parts) <= 2)
		{
			name = resolve_table_argument(arg, a, b, false);
			if (name)
				return name;
		}
	}
	name = resolve_object_argument(arg, a, b, false);
	return name ? name : resolve_table_argument(arg, a, b, true);
}

/* Walk two logical row sets by key. Keyless cardinality changes emit one row
 * per added or removed occurrence, like Dolt. */
static void
diff_rows(DiffTable *from, DiffTable *to, DiffRowFn fn, void *arg)
{
	size_t		i = 0,
				j = 0;
	bool		keyed = (from->present && from->keyed) || (to->present && to->keyed);

	if (from->present && to->present &&
		((from->object_kind == NULL) != (to->object_kind == NULL) ||
		 (from->object_kind && strcmp(from->object_kind, to->object_kind))))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot diff %s: object kind changed between revisions", to->name)));
	if (from->present && to->present && (from->keyed != to->keyed || (from->keyed && !same_key_columns(from, to))))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot diff %s: primary key changed between revisions", to->name)));
	for (;;)
	{
		int			cmp;
		DiffRow		row = {0};

		CHECK_FOR_INTERRUPTS();
		if (i >= from->rows.count && j >= to->rows.count)
			break;
		if (i >= from->rows.count)
			cmp = 1;
		else if (j >= to->rows.count)
			cmp = -1;
		else
			cmp = pl_compare(from->rows.items[i].key, to->rows.items[j].key);
		if (keyed)
		{
			if (cmp == 0)
			{
				if (pl_compare(from->rows.items[i].value, to->rows.items[j].value))
				{
					row.type = "modified";
					row.from = DG_VALUE(true, from->rows.items[i].value);
					row.to = DG_VALUE(true, to->rows.items[j].value);
					fn(from, to, &row, arg);
				}
				i++;
				j++;
			}
			else if (cmp < 0)
			{
				row.type = "removed";
				row.from = DG_VALUE(true, from->rows.items[i].value);
				fn(from, to, &row, arg);
				i++;
			}
			else
			{
				row.type = "added";
				row.to = DG_VALUE(true, to->rows.items[j].value);
				fn(from, to, &row, arg);
				j++;
			}
		}
		else
		{
			uint64		before = cmp <= 0 ? pl_read64(from->rows.items[i].value.data) : 0,
						after = cmp >= 0 ? pl_read64(to->rows.items[j].value.data) : 0;
			PLSlice		bytes = cmp <= 0 ? from->rows.items[i].value : to->rows.items[j].value;
			PLSlice		payload = {bytes.data + 8, bytes.len - 8};
			uint64		k;

			for (k = after; k < before; k++)
			{
				row.type = "removed";
				row.from = DG_VALUE(true, payload);
				row.to = DG_VALUE(false, payload);
				fn(from, to, &row, arg);
			}
			for (k = before; k < after; k++)
			{
				row.type = "added";
				row.from = DG_VALUE(false, payload);
				row.to = DG_VALUE(true, payload);
				fn(from, to, &row, arg);
			}
			if (cmp <= 0)
				i++;
			if (cmp >= 0)
				j++;
		}
	}
}

/* Decode a packed row into datums of its descriptor. */
static void
decode_row(DiffTable *t, PLValue row, Datum *values, bool *nulls)
{
	HeapTuple	tuple;
	int			i;

	for (i = 0; i < t->desc->natts; i++)
		nulls[i] = true;
	if (!row.present)
		return;
	tuple = dg_decode_descriptor(t->desc, row.bytes);
	heap_deform_tuple(tuple, t->desc, values, nulls);
}

/* ---- Argument and table-name resolution ---- */

static char *
revision_label(const char *revision)
{
	return pstrdup(revision);
}

/* Resolve a table argument against two states: qualified names match
 * exactly; unqualified names must be unique across schemas, with the search
 * path breaking ties. */
static char *
resolve_table_argument(const char *arg, const char *json_a, const char *json_b, bool required)
{
	List	   *parts;
	char	   *qualified = NULL,
			   *found;
	bool		in_path;

	if (!*arg)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("table name cannot be empty")));
	parts = stringToQualifiedNameList(arg, NULL);
	if (list_length(parts) == 2)
		qualified = dg_qualified_name(strVal(linitial(parts)), strVal(lsecond(parts)));
	else if (list_length(parts) != 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid table name: %s", arg)));
	run("WITH members AS (SELECT v FROM jsonb_array_elements($1::jsonb->'members') v UNION ALL SELECT v FROM jsonb_array_elements($2::jsonb->'members') v), names AS (SELECT DISTINCT v->>'name' AS name FROM members WHERE v->>'kind'='table'), path AS (SELECT s,ord FROM (SELECT CASE WHEN btrim(s)='\"$user\"' THEN current_user::text ELSE btrim(btrim(s),'\"') END AS s,ord FROM unnest(string_to_array($5,',')) WITH ORDINALITY AS p(s,ord)) x WHERE EXISTS (SELECT 1 FROM pg_namespace WHERE nspname=x.s)) SELECT name,path.ord IS NOT NULL FROM names LEFT JOIN path ON (parse_ident(name))[1]=path.s WHERE ($3::text IS NOT NULL AND name=$3) OR ($3::text IS NULL AND (parse_ident(name))[2]=$4) ORDER BY path.ord NULLS LAST,name COLLATE \"C\"", 5, VC_TYPES(TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json_a), CStringGetTextDatum(json_b), qualified ? CStringGetTextDatum(qualified) : (Datum) 0, CStringGetTextDatum(strVal(llast(parts))), CStringGetTextDatum(api_search_path ? api_search_path : "public")), qualified ? NULL : "  n  ", true);
	if (!SPI_processed)
	{
		if (!required)
			return NULL;
		if (!qualified)
		{
			run("SELECT coalesce((SELECT s FROM (SELECT CASE WHEN btrim(s)='\"$user\"' THEN current_user::text ELSE btrim(btrim(s),'\"') END AS s,ord FROM unnest(string_to_array($1,',')) WITH ORDINALITY AS p(s,ord)) x WHERE EXISTS (SELECT 1 FROM pg_namespace WHERE nspname=x.s) ORDER BY ord LIMIT 1),'public')", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(api_search_path ? api_search_path : "public")), NULL, true);
			qualified = dg_qualified_name(strcell(0, 1), strVal(linitial(parts)));
		}
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("table not found: %s", qualified)));
	}
	found = strcell(0, 1);
	in_path = DatumGetBool(cell(0, 2));
	if (SPI_processed > 1 && !in_path)
		ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_ALIAS), errmsg("ambiguous table name: %s", arg)));
	return found;
}

/* ---- Output descriptors ---- */

static void
add_prefixed_columns(TupleDesc out, int *at, const char *prefix, TupleDesc in)
{
	int			i;

	for (i = 0; i < in->natts; i++)
	{
		Form_pg_attribute a = TupleDescAttr(in, i);

		TupleDescInitEntry(out, ++*at, psprintf("%s%s", prefix, NameStr(a->attname)), a->atttypid, a->atttypmod, 0);
		TupleDescInitEntryCollation(out, *at, a->attcollation);
	}
}
static void
add_column(TupleDesc out, int *at, const char *name, Oid type)
{
	TupleDescInitEntry(out, ++*at, name, type, -1, 0);
}

/* Column layout of diff-like outputs. Absent sides borrow the other side's
 * schema, as Dolt's expandFromToSchemas does. */
static TupleDesc
diff_descriptor(const char *kind, DiffTable *from, DiffTable *to)
{
	TupleDesc	fdesc = from->present ? from->desc : to->desc,
				tdesc = to->present ? to->desc : from->desc,
				out;
	int			at = 0,
				i;

	if (!fdesc || !tdesc)
		elog(ERROR, "diff requires a table schema");
	if (!strcmp(kind, "diff") || !strcmp(kind, "commit_diff"))
	{
		out = CreateTemplateTupleDesc(tdesc->natts + fdesc->natts + 5);
		add_prefixed_columns(out, &at, "to_", tdesc);
		add_column(out, &at, "to_commit", TEXTOID);
		add_column(out, &at, "to_commit_date", TIMESTAMPOID);
		add_prefixed_columns(out, &at, "from_", fdesc);
		add_column(out, &at, "from_commit", TEXTOID);
		add_column(out, &at, "from_commit_date", TIMESTAMPOID);
		add_column(out, &at, "diff_type", TEXTOID);
	}
	else if (!strcmp(kind, "history"))
	{
		out = CreateTemplateTupleDesc(tdesc->natts + 3);
		add_prefixed_columns(out, &at, "", tdesc);
		add_column(out, &at, "commit_hash", TEXTOID);
		add_column(out, &at, "committer", TEXTOID);
		add_column(out, &at, "commit_date", TIMESTAMPOID);
	}
	else if (!strcmp(kind, "blame"))
	{
		DiffTable  *t = to->present ? to : from;

		out = CreateTemplateTupleDesc(t->nkeys + 5);
		for (i = 0; i < t->nkeys; i++)
		{
			Form_pg_attribute a = TupleDescAttr(t->desc, t->keys[i]);

			TupleDescInitEntry(out, ++at, NameStr(a->attname), a->atttypid, a->atttypmod, 0);
			TupleDescInitEntryCollation(out, at, a->attcollation);
		}
		add_column(out, &at, "commit", TEXTOID);
		add_column(out, &at, "commit_date", TIMESTAMPOID);
		add_column(out, &at, "committer", TEXTOID);
		add_column(out, &at, "email", TEXTOID);
		add_column(out, &at, "message", TEXTOID);
	}
	else if (!strcmp(kind, "workspace"))
	{
		out = CreateTemplateTupleDesc(tdesc->natts + fdesc->natts + 3);
		add_column(out, &at, "id", INT8OID);
		add_column(out, &at, "staged", BOOLOID);
		add_column(out, &at, "diff_type", TEXTOID);
		add_prefixed_columns(out, &at, "to_", tdesc);
		add_prefixed_columns(out, &at, "from_", fdesc);
	}
	else
		elog(ERROR, "unknown diff output kind: %s", kind);
	return out;
}

/* ---- Commit enumeration ---- */

typedef struct CommitInfo
{
	bytea	   *hash;
	char	   *hash_text;
	char	   *committer;
	char	   *email;
	char	   *message;
	int64		date_us;
	int			nparents;
	bytea	  **parents;
} CommitInfo;

/* Commits reachable from a head, newest first. */
static CommitInfo *
reachable_commits(bytea *head, int *count)
{
	CommitInfo *result;
	int			n,
				i;

	run("WITH RECURSIVE walk(h) AS (SELECT $1::bytea UNION SELECT unnest(o.objchildren[2:]) FROM walk w JOIN pg_dolt_object_visible o ON o.objhash=w.h) SELECT o.objhash,pg_catalog.dolt_hash_encode(o.objhash),m->>'committer',m->>'email',m->>'message',(m->>'date_us')::bigint,o.objchildren[2:] FROM walk w JOIN pg_dolt_object_visible o ON o.objhash=w.h, LATERAL (SELECT convert_from(o.objpayload,'UTF8')::jsonb) AS x(m) WHERE o.objkind='commit' ORDER BY (m->>'height')::bigint DESC,(m->>'date_us')::bigint DESC,o.objhash", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(head)), NULL, true);
	n = SPI_processed;
	result = palloc0(sizeof(CommitInfo) * Max(n, 1));
	for (i = 0; i < n; i++)
	{
		ArrayType  *parents = DatumGetArrayTypeP(cell(i, 7));
		Datum	   *values;
		bool	   *nulls;
		int			np,
					j;

		result[i].hash = DatumGetByteaPCopy(cell(i, 1));
		result[i].hash_text = strcell(i, 2);
		result[i].committer = strcell(i, 3);
		result[i].email = strcell(i, 4);
		result[i].message = strcell(i, 5);
		result[i].date_us = DatumGetInt64(cell(i, 6));
		deconstruct_array(parents, BYTEAOID, -1, false, TYPALIGN_INT, &values, &nulls, &np);
		result[i].nparents = np;
		result[i].parents = palloc(sizeof(bytea *) * Max(np, 1));
		for (j = 0; j < np; j++)
			result[i].parents[j] = DatumGetByteaPCopy(values[j]);
	}
	*count = n;
	return result;
}

static CommitInfo *
find_commit(CommitInfo *commits, int count, bytea *hash)
{
	int			i;

	for (i = 0; i < count; i++)
		if (same(commits[i].hash, hash))
			return &commits[i];
	return NULL;
}

/* ---- Row emission into the materialized result ---- */

typedef struct DiffEmit
{
	ReturnSetInfo *rsinfo;
	const char *kind;
	const char *to_label;
	const char *from_label;
	bool		to_date_null;
	bool		from_date_null;
	int64		to_date;
	int64		from_date;
	bool		staged;
	int64		ordinal;
	DiffTable  *schema_from;	/* descriptor sources for the output layout */
	DiffTable  *schema_to;
} DiffEmit;

/* Map a side's decoded values onto the output layout's columns by name. */
static void
place_values(TupleDesc out, int base, TupleDesc layout, DiffTable *side, PLValue row, Datum *values, bool *nulls)
{
	Datum	   *v;
	bool	   *n;
	int			i,
				j;

	if (!row.present || !side->present)
		return;
	v = palloc(sizeof(Datum) * side->desc->natts);
	n = palloc(sizeof(bool) * side->desc->natts);
	decode_row(side, row, v, n);
	for (i = 0; i < layout->natts; i++)
	{
		const char *name = NameStr(TupleDescAttr(layout, i)->attname);

		for (j = 0; j < side->desc->natts; j++)
			if (!strcmp(NameStr(TupleDescAttr(side->desc, j)->attname), name))
			{
				if (!n[j] && TupleDescAttr(side->desc, j)->atttypid == TupleDescAttr(layout, i)->atttypid)
				{
					values[base + i] = v[j];
					nulls[base + i] = false;
				}
				break;
			}
	}
}

/* TODO KNOWN-ISSUES.md: validate/rebind prepared dynamic result layouts. */
static void
emit_diff_row(DiffTable *from, DiffTable *to, DiffRow *row, void *arg)
{
	DiffEmit   *e = (DiffEmit *) arg;
	TupleDesc	out = e->rsinfo->setDesc;
	TupleDesc	tdesc = e->schema_to->present ? e->schema_to->desc : e->schema_from->desc,
				fdesc = e->schema_from->present ? e->schema_from->desc : e->schema_to->desc;
	Datum	   *values = palloc0(sizeof(Datum) * out->natts);
	bool	   *nulls = palloc(sizeof(bool) * out->natts);
	int			at;

	memset(nulls, true, out->natts);
	if (!strcmp(e->kind, "workspace"))
	{
		values[0] = Int64GetDatum(e->ordinal++);
		nulls[0] = false;
		values[1] = BoolGetDatum(e->staged);
		nulls[1] = false;
		values[2] = CStringGetTextDatum(row->type);
		nulls[2] = false;
		place_values(out, 3, tdesc, to, row->to, values, nulls);
		place_values(out, 3 + tdesc->natts, fdesc, from, row->from, values, nulls);
	}
	else
	{
		place_values(out, 0, tdesc, to, row->to, values, nulls);
		at = tdesc->natts;
		values[at] = CStringGetTextDatum(e->to_label);
		nulls[at] = false;
		at++;
		if (!e->to_date_null)
		{
			values[at] = TimestampGetDatum((Timestamp) e->to_date);
			nulls[at] = false;
		}
		at++;
		place_values(out, at, fdesc, from, row->from, values, nulls);
		at += fdesc->natts;
		if (e->from_label)
		{
			values[at] = CStringGetTextDatum(e->from_label);
			nulls[at] = false;
		}
		at++;
		if (!e->from_date_null)
		{
			values[at] = TimestampGetDatum((Timestamp) e->from_date);
			nulls[at] = false;
		}
		at++;
		values[at] = CStringGetTextDatum(row->type);
		nulls[at] = false;
	}
	tuplestore_putvalues(e->rsinfo->setResult, out, values, nulls);
}

/* ---- Parse-time descriptor service ---- */

/* Layout for a generated system table or table function. Used by the parser
 * to build the column definition list of the function RTE. */
TupleDesc
dg_diff_layout(const char *kind, const char *table, const char *from, const char *to)
{
	int			nest = begin_api();
	DiffTable  *a,
			   *b;
	TupleDesc	result;

	if (!strcmp(kind, "conflicts"))
	{
		result = conflict_layout(table, from, to);
		end_api(nest);
		return result;
	}
	if (!strcmp(kind, "violations"))
	{
		result = violation_layout(table);
		end_api(nest);
		return result;
	}
	if (from && !to)
	{
		char	   *f,
				   *t;

		split_dots(from, &f, &t);
		from = f;
		to = t;
	}
	if (from && to)
	{
		char	   *resolved = !strcmp(kind, "diff") ?
			resolve_diff_argument(table, state_json(from), state_json(to)) :
			resolve_table_argument(table, state_json(from), state_json(to), true);

		a = !strcmp(kind, "diff") ? diff_object_revision(from, resolved, false) : diff_table_schema(from, resolved);
		b = !strcmp(kind, "diff") ? diff_object_revision(to, resolved, false) : diff_table_schema(to, resolved);
	}
	else
	{
		a = diff_table_schema("WORKING", table);
		b = a;
	}
	if (!a->present && !b->present)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("table not found: %s", table)));
	result = diff_descriptor(kind, a, b);
	end_api(nest);
	return result;
}

/* ---- dolt_diff(from, to, table) table function ---- */

static void
diff_emit_prepare(FunctionCallInfo fcinfo, DiffEmit *e, const char *kind)
{
	memset(e, 0, sizeof(*e));
	e->rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	e->kind = kind;
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
}

/* Commit date for a revision label, NULL for WORKING/STAGED. */
static bool
revision_date(const char *revision, int64 *date)
{
	bytea	   *commit;
	char	   *json;

	if (!pg_strcasecmp(revision, "WORKING") || !pg_strcasecmp(revision, "STAGED"))
		return false;
	commit = resolve(revision);
	json = metadata(commit, "commit");
	*date = pg_strtoint64(json_value(json, "date_us"));
	return true;
}

static void
split_dots(const char *spec, char **from, char **to)
{
	char	   *three = strstr(spec, "...");

	if (three)
	{
		char	   *left = pnstrdup(spec, three - spec);
		char	   *right = pstrdup(three + 3);
		bytea	   *base;

		if (!*right)
			right = "HEAD";
		base = merge_base(resolve(left), resolve(right));
		*from = hash_format(base);
		*to = right;
	}
	else
	{
		char	   *two = strstr(spec, "..");

		if (!two)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("dolt_diff with two arguments requires a from..to revision range")));
		*from = pnstrdup(spec, two - spec);
		*to = pstrdup(two + 2);
		if (!**to)
			*to = "HEAD";
	}
}

/* Summary, statistics and schema diffs accept either two revisions, or a
 * two/three-dot range, followed by an optional object name. */
static void
diff_revision_arguments(FunctionCallInfo fcinfo, char **from, char **to, char **table)
{
 char *first = text_to_cstring(PG_GETARG_TEXT_PP(0));

 if (strstr(first, ".."))
 {
  if (PG_NARGS() > 2)
   ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("a diff revision range accepts only an optional table name")));
  split_dots(first, from, to);
  *table = optional_text_arg(fcinfo, 1);
 }
 else
 {
  if (PG_NARGS() < 2)
   ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("a diff requires two revisions or a from..to revision range")));
  *from = first;
  *to = text_to_cstring(PG_GETARG_TEXT_PP(1));
  *table = optional_text_arg(fcinfo, 2);
 }
}

Datum
dolt_diff_rows(PG_FUNCTION_ARGS)
{
	char	   *from,
			   *to,
			   *table;
	int			nest;
	DiffEmit	e;
	DiffTable  *a,
			   *b;
	char	   *resolved;

	if (PG_NARGS() == 2)
	{
		char	   *spec = text_to_cstring(PG_GETARG_TEXT_PP(0));

		table = text_to_cstring(PG_GETARG_TEXT_PP(1));
		if (!strstr(spec, ".."))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("dolt_diff with two arguments requires a from..to revision range")));
		nest = begin_api();
		split_dots(spec, &from, &to);
	}
	else
	{
		from = text_to_cstring(PG_GETARG_TEXT_PP(0));
		to = text_to_cstring(PG_GETARG_TEXT_PP(1));
		table = text_to_cstring(PG_GETARG_TEXT_PP(2));
		nest = begin_api();
	}
	diff_emit_prepare(fcinfo, &e, "diff");
	resolved = resolve_diff_argument(table, state_json(from), state_json(to));
	a = diff_object_revision(from, resolved, true);
	b = diff_object_revision(to, resolved, true);
	e.schema_from = a;
	e.schema_to = b;
	e.from_label = revision_label(from);
	e.to_label = revision_label(to);
	e.from_date_null = !revision_date(from, &e.from_date);
	e.to_date_null = !revision_date(to, &e.to_date);
	diff_rows(a, b, emit_diff_row, &e);
	diff_table_close(a);
	diff_table_close(b);
	end_api(nest);
	return (Datum) 0;
}

Datum
dolt_diff_rows_dots(PG_FUNCTION_ARGS)
{
	return dolt_diff_rows(fcinfo);
}

/* ---- Generated per-table system tables ---- */

/* The partitions of dolt_diff_<table>: the working set against the branch
 * head, then every reachable commit against each of its parents. */
static void
diff_partitions(FunctionCallInfo fcinfo, const char *kind, const char *name, const char *from_hint, const char *to_hint)
{
	int			nest = begin_api();
	DiffEmit	e;
	VersionRef	head = read_ref(branch_ref("heads"));
	CommitInfo *commits;
	int			count,
				i,
				j;
	DiffTable  *layout = diff_table_schema("WORKING", name);

	if (!layout->present)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("table not found: %s", name)));
	diff_emit_prepare(fcinfo, &e, kind);
	e.schema_from = layout;
	e.schema_to = layout;
	commits = reachable_commits(head.target, &count);
	if ((!to_hint || !strcmp(to_hint, "WORKING")) && (!from_hint || !strcmp(from_hint, commits[0].hash_text)))
	{
		DiffTable  *a = diff_table_at(child(head.target, 1), name, true),
				   *b = diff_table_working(name, true);

		e.to_label = "WORKING";
		e.to_date_null = true;
		e.from_label = commits[0].hash_text;
		e.from_date = commits[0].date_us;
		e.from_date_null = false;
		diff_rows(a, b, emit_diff_row, &e);
		diff_table_close(a);
		diff_table_close(b);
	}
	for (i = 0; i < count; i++)
	{
		CommitInfo *c = &commits[i];
		DiffTable  *b;

		if (to_hint && strcmp(to_hint, c->hash_text))
			continue;
		b = diff_table_at(child(c->hash, 1), name, true);
		for (j = 0; j < Max(c->nparents, 1); j++)
		{
			CommitInfo *parent = c->nparents ? find_commit(commits, count, c->parents[j]) : NULL;
			DiffTable  *a;

			if (from_hint && (!parent || strcmp(from_hint, parent->hash_text)))
				continue;
			a = diff_table_at(parent ? child(parent->hash, 1) : NULL, name, true);
			e.to_label = c->hash_text;
			e.to_date = c->date_us;
			e.to_date_null = false;
			e.from_label = parent ? parent->hash_text : NULL;
			e.from_date_null = parent == NULL;
			if (parent)
				e.from_date = parent->date_us;
			diff_rows(a, b, emit_diff_row, &e);
			diff_table_close(a);
		}
		diff_table_close(b);
	}
	end_api(nest);
}

static char *
optional_text_arg(FunctionCallInfo fcinfo, int n)
{
	if (PG_NARGS() <= n || PG_ARGISNULL(n))
		return NULL;
	return text_to_cstring(PG_GETARG_TEXT_PP(n));
}

Datum
dolt_diff_table(PG_FUNCTION_ARGS)
{
	diff_partitions(fcinfo, "diff", text_to_cstring(PG_GETARG_TEXT_PP(0)), optional_text_arg(fcinfo, 1), optional_text_arg(fcinfo, 2));
	return (Datum) 0;
}

/* dolt_commit_diff_<table>: one partition between two explicit revisions. */
Datum
dolt_commit_diff_table(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0)),
			   *from = optional_text_arg(fcinfo, 1),
			   *to = optional_text_arg(fcinfo, 2);
	int			nest;
	DiffEmit	e;
	DiffTable  *a,
			   *b;

	if (!to)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("dolt_commit_diff_* tables must be filtered to a single 'to_commit'")));
	if (!from)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("dolt_commit_diff_* tables must be filtered to a single 'from_commit'")));
	nest = begin_api();
	diff_emit_prepare(fcinfo, &e, "commit_diff");
	a = diff_table_revision(from, name);
	b = diff_table_revision(to, name);
	e.schema_from = a;
	e.schema_to = b;
	e.from_label = from;
	e.to_label = to;
	e.from_date_null = !revision_date(from, &e.from_date);
	e.to_date_null = !revision_date(to, &e.to_date);
	diff_rows(a, b, emit_diff_row, &e);
	diff_table_close(a);
	diff_table_close(b);
	end_api(nest);
	return (Datum) 0;
}

/* dolt_history_<table>: every row of the table at each reachable commit. */
Datum
dolt_history_table(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			nest = begin_api();
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	VersionRef	head = read_ref(branch_ref("heads"));
	CommitInfo *commits;
	int			count,
				i;
	DiffTable  *layout = diff_table_schema("WORKING", name);

	if (!layout->present)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("table not found: %s", name)));
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	commits = reachable_commits(head.target, &count);
	for (i = 0; i < count; i++)
	{
		CommitInfo *c = &commits[i];
		DiffTable  *t = diff_table_at(child(c->hash, 1), name, true);
		size_t		r;
		TupleDesc	out = rsinfo->setDesc;

		if (t->present)
			for (r = 0; r < t->rows.count; r++)
			{
				Datum	   *values = palloc0(sizeof(Datum) * out->natts);
				bool	   *nulls = palloc(sizeof(bool) * out->natts);
				PLSlice		bytes = t->rows.items[r].value;
				uint64		occurrences = 1,
							k;

				memset(nulls, true, out->natts);
				if (!t->keyed)
				{
					occurrences = pl_read64(bytes.data);
					bytes.data += 8;
					bytes.len -= 8;
				}
				place_values(out, 0, layout->desc, t, DG_VALUE(true, bytes), values, nulls);
				values[out->natts - 3] = CStringGetTextDatum(c->hash_text);
				values[out->natts - 2] = CStringGetTextDatum(c->committer);
				values[out->natts - 1] = TimestampGetDatum((Timestamp) c->date_us);
				nulls[out->natts - 3] = nulls[out->natts - 2] = nulls[out->natts - 1] = false;
				for (k = 0; k < occurrences; k++)
					tuplestore_putvalues(rsinfo->setResult, out, values, nulls);
			}
		diff_table_close(t);
	}
	end_api(nest);
	return (Datum) 0;
}

/* dolt_blame_<table>: the latest commit that added or modified each key. */
typedef struct BlameState
{
	ReturnSetInfo *rsinfo;
	DiffTable  *layout;
	CommitInfo *commit;
	PLEntry    *seen;			/* keys already attributed */
	size_t		nseen;
	size_t		capacity;
} BlameState;

static bool
blame_seen(BlameState *b, PLSlice key)
{
	size_t		i;

	for (i = 0; i < b->nseen; i++)
		if (!pl_compare(b->seen[i].key, key))
			return true;
	return false;
}
static void
blame_row(DiffTable *from, DiffTable *to, DiffRow *row, void *arg)
{
	BlameState *b = (BlameState *) arg;
	PLSlice		key;
	PLValue    *fields;
	int			i;
	TupleDesc	out = b->rsinfo->setDesc;
	Datum	   *values;
	bool	   *nulls;
	Datum	   *v;
	bool	   *n;

	fields = palloc0(sizeof(*fields) * b->layout->nkeys);
	if (row->to.present)
		for (i = 0; i < b->layout->nkeys; i++)
			dg_check(pl_tuple_get(row->to.bytes, to->keys[i], &fields[i]));
	else
		for (i = 0; i < b->layout->nkeys; i++)
			dg_check(pl_tuple_get(row->from.bytes, from->keys[i], &fields[i]));
	key = pack_merge_fields(fields, b->layout->nkeys);
	if (blame_seen(b, key))
		return;
	if (b->nseen == b->capacity)
	{
		b->capacity = Max(16, b->capacity * 2);
		b->seen = b->seen ? repalloc(b->seen, sizeof(PLEntry) * b->capacity) : palloc(sizeof(PLEntry) * b->capacity);
	}
	b->seen[b->nseen++].key = key;
	if (!strcmp(row->type, "removed"))
		return;
	values = palloc0(sizeof(Datum) * out->natts);
	nulls = palloc(sizeof(bool) * out->natts);
	memset(nulls, true, out->natts);
	v = palloc(sizeof(Datum) * to->desc->natts);
	n = palloc(sizeof(bool) * to->desc->natts);
	decode_row(to, row->to, v, n);
	for (i = 0; i < b->layout->nkeys; i++)
	{
		values[i] = v[to->keys[i]];
		nulls[i] = n[to->keys[i]];
	}
	i = b->layout->nkeys;
	values[i] = CStringGetTextDatum(b->commit->hash_text);
	values[i + 1] = TimestampGetDatum((Timestamp) b->commit->date_us);
	values[i + 2] = CStringGetTextDatum(b->commit->committer);
	values[i + 3] = CStringGetTextDatum(b->commit->email);
	values[i + 4] = CStringGetTextDatum(b->commit->message);
	nulls[i] = nulls[i + 1] = nulls[i + 2] = nulls[i + 3] = nulls[i + 4] = false;
	tuplestore_putvalues(b->rsinfo->setResult, out, values, nulls);
}

Datum
dolt_blame_table(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			nest = begin_api();
	VersionRef	head = read_ref(branch_ref("heads"));
	CommitInfo *commits;
	int			count,
				i,
				j;
	BlameState	b = {0};

	b.layout = diff_table_schema("WORKING", name);
	if (!b.layout->present)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("table not found: %s", name)));
	if (!b.layout->keyed)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("dolt_blame requires a primary key on %s", name)));
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	b.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	commits = reachable_commits(head.target, &count);
	for (i = 0; i < count; i++)
	{
		CommitInfo *c = &commits[i];
		DiffTable  *to = diff_table_at(child(c->hash, 1), name, true);

		b.commit = c;
		for (j = 0; j < Max(c->nparents, 1); j++)
		{
			CommitInfo *parent = c->nparents ? find_commit(commits, count, c->parents[j]) : NULL;
			DiffTable  *from = diff_table_at(parent ? child(parent->hash, 1) : NULL, name, true);

			if (to->present && (!from->present || from->keyed))
				diff_rows(from, to, blame_row, &b);
			diff_table_close(from);
		}
		diff_table_close(to);
	}
	/* Dolt orders blame output by the primary key. */
	{
		ReturnSetInfo *rsinfo = b.rsinfo;
		Tuplestorestate *sorted;
		TupleTableSlot *slot = MakeSingleTupleTableSlot(rsinfo->setDesc, &TTSOpsMinimalTuple);
		Tuplesortstate *sort;
		AttrNumber *keys = palloc(sizeof(AttrNumber) * b.layout->nkeys);
		Oid		   *ops = palloc(sizeof(Oid) * b.layout->nkeys),
				   *colls = palloc(sizeof(Oid) * b.layout->nkeys);
		bool	   *nullsfirst = palloc0(sizeof(bool) * b.layout->nkeys);
		int			k;

		for (k = 0; k < b.layout->nkeys; k++)
		{
			Form_pg_attribute a = TupleDescAttr(rsinfo->setDesc, k);
			TypeCacheEntry *tc = lookup_type_cache(a->atttypid, TYPECACHE_LT_OPR);

			keys[k] = k + 1;
			ops[k] = tc->lt_opr;
			colls[k] = a->attcollation;
			if (!OidIsValid(ops[k]))
				elog(ERROR, "blame key type has no ordering operator");
		}
		sort = tuplesort_begin_heap(rsinfo->setDesc, b.layout->nkeys, keys, ops, colls, nullsfirst, work_mem, NULL, TUPLESORT_NONE);
		while (tuplestore_gettupleslot(rsinfo->setResult, true, false, slot))
			tuplesort_puttupleslot(sort, slot);
		tuplesort_performsort(sort);
		{
			MemoryContext old = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

			sorted = tuplestore_begin_heap(true, false, work_mem);
			MemoryContextSwitchTo(old);
		}
		while (tuplesort_gettupleslot(sort, true, false, slot, NULL))
			tuplestore_puttupleslot(sorted, slot);
		tuplesort_end(sort);
		tuplestore_end(rsinfo->setResult);
		rsinfo->setResult = sorted;
		ExecDropSingleTupleTableSlot(slot);
	}
	end_api(nest);
	return (Datum) 0;
}

/* dolt_workspace_<table>: staged then unstaged row changes. */
Datum
dolt_workspace_table(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			nest = begin_api();
	DiffEmit	e;
	DiffTable  *layout = diff_table_schema("WORKING", name),
			   *head,
			   *staged,
			   *working;

	diff_emit_prepare(fcinfo, &e, "workspace");
	if (!layout->present)
	{
		end_api(nest);
		return (Datum) 0;
	}
	e.schema_from = layout;
	e.schema_to = layout;
	head = diff_table_revision("HEAD", name);
	staged = diff_table_revision("STAGED", name);
	working = diff_table_revision("WORKING", name);
	e.staged = true;
	diff_rows(head, staged, emit_diff_row, &e);
	e.staged = false;
	diff_rows(staged, working, emit_diff_row, &e);
	diff_table_close(head);
	diff_table_close(staged);
	diff_table_close(working);
	end_api(nest);
	return (Datum) 0;
}

/* ---- dolt_column_diff system table ---- */

typedef struct ColumnChanges
{
	DiffTable  *from;
	DiffTable  *to;
	bool	   *changed;		/* per to-column: a value differed */
	bool		any;
} ColumnChanges;

static void
column_change_row(DiffTable *from, DiffTable *to, DiffRow *row, void *arg)
{
	ColumnChanges *cc = (ColumnChanges *) arg;
	int			i;

	cc->any = true;
	if (!row->from.present || !row->to.present)
	{
		for (i = 0; i < to->desc->natts; i++)
			cc->changed[i] = true;
		return;
	}
	for (i = 0; i < to->desc->natts; i++)
	{
		const char *name = NameStr(TupleDescAttr(to->desc, i)->attname);
		int			j;
		PLValue		a,
					b;

		for (j = 0; j < from->desc->natts; j++)
			if (!strcmp(NameStr(TupleDescAttr(from->desc, j)->attname), name))
				break;
		if (j == from->desc->natts)
		{
			cc->changed[i] = true;
			continue;
		}
		dg_check(pl_tuple_get(row->to.bytes, i, &b));
		dg_check(pl_tuple_get(row->from.bytes, j, &a));
		if (!equal_value(a, b))
			cc->changed[i] = true;
	}
}

static void
column_diff_partition(ReturnSetInfo *rsinfo, const char *label, CommitInfo *c, DiffTable *from, DiffTable *to)
{
	TupleDesc	out = rsinfo->setDesc;
	ColumnChanges cc = {from, to, NULL, false};
	int			i;

	if (!from->present && !to->present)
		return;
	cc.changed = palloc0(sizeof(bool) * (to->present ? to->desc->natts : 0));
	if (from->present && to->present)
		diff_rows(from, to, column_change_row, &cc);
	for (i = 0; i < (to->present ? to->desc->natts : 0) + (from->present ? from->desc->natts : 0); i++)
	{
		const char *column,
				   *type = NULL;
		Datum		values[11] = {0};
		bool		nulls[11];
		int			j;

		memset(nulls, true, sizeof(nulls));
		if (to->present && i < to->desc->natts)
		{
			column = NameStr(TupleDescAttr(to->desc, i)->attname);
			if (!from->present)
				type = "added";
			else
			{
				for (j = 0; j < from->desc->natts; j++)
					if (!strcmp(NameStr(TupleDescAttr(from->desc, j)->attname), column))
						break;
				if (j == from->desc->natts)
					type = "added";
				else if (cc.changed[i] || TupleDescAttr(from->desc, j)->atttypid != TupleDescAttr(to->desc, i)->atttypid)
					type = "modified";
			}
		}
		else
		{
			int			k = i - (to->present ? to->desc->natts : 0);

			column = NameStr(TupleDescAttr(from->desc, k)->attname);
			if (to->present)
			{
				for (j = 0; j < to->desc->natts; j++)
					if (!strcmp(NameStr(TupleDescAttr(to->desc, j)->attname), column))
						break;
				if (j < to->desc->natts)
					continue;	/* reported from the to side */
			}
			type = "dropped";
		}
		if (!type)
			continue;
		values[0] = CStringGetTextDatum(label);
		values[1] = CStringGetTextDatum(to->present ? to->name : from->name);
		values[2] = CStringGetTextDatum(column);
		nulls[0] = nulls[1] = nulls[2] = false;
		if (c)
		{
			values[3] = CStringGetTextDatum(c->committer);
			values[4] = CStringGetTextDatum(c->email);
			values[5] = TimestampGetDatum((Timestamp) c->date_us);
			values[6] = CStringGetTextDatum(c->message);
			nulls[3] = nulls[4] = nulls[5] = nulls[6] = false;
		}
		values[7] = CStringGetTextDatum(type);
		nulls[7] = false;
		if (c)
		{
			char	   *meta = metadata(c->hash, "commit");

			values[8] = CStringGetTextDatum(json_value(meta, "author"));
			values[9] = CStringGetTextDatum(json_value(meta, "author_email"));
			values[10] = TimestampGetDatum((Timestamp) pg_strtoint64(json_value(meta, "author_date_us")));
			nulls[8] = nulls[9] = nulls[10] = false;
		}
		tuplestore_putvalues(rsinfo->setResult, out, values, nulls);
	}
}

/* Tables whose membership or contents differ between two states. */
static char **
changed_tables(const char *json_a, const char *json_b, int *count)
{
	char	  **names;
	int			i;

	run("WITH a AS (SELECT v FROM jsonb_array_elements($1::jsonb->'members') v WHERE v->>'kind'='table'),b AS (SELECT v FROM jsonb_array_elements($2::jsonb->'members') v WHERE v->>'kind'='table') SELECT coalesce(a.v->>'name',b.v->>'name') FROM a FULL JOIN b ON a.v->>'name'=b.v->>'name' WHERE a.v IS NULL OR b.v IS NULL OR a.v->>'content' IS DISTINCT FROM b.v->>'content' OR a.v->>'schema' IS DISTINCT FROM b.v->>'schema' ORDER BY coalesce(a.v->>'name',b.v->>'name') COLLATE \"C\"", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json_a), CStringGetTextDatum(json_b)), NULL, true);
	*count = SPI_processed;
	names = palloc(sizeof(char *) * Max(*count, 1));
	for (i = 0; i < *count; i++)
		names[i] = strcell(i, 1);
	return names;
}

Datum
dolt_column_diff(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	VersionRef	head = read_ref(branch_ref("heads"));
	CommitInfo *commits;
	int			count,
				i,
				j,
				n;
	char	  **names;
	char	   *headjson = head_state(),
			   *stagedjson = staged_state(),
			   *workingjson = working_state();

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	names = changed_tables(headjson, stagedjson, &n);
	for (i = 0; i < n; i++)
	{
		DiffTable  *a = diff_table_revision("HEAD", names[i]),
				   *b = diff_table_revision("STAGED", names[i]);

		column_diff_partition(rsinfo, "STAGED", NULL, a, b);
		diff_table_close(a);
		diff_table_close(b);
	}
	names = changed_tables(stagedjson, workingjson, &n);
	for (i = 0; i < n; i++)
	{
		DiffTable  *a = diff_table_revision("STAGED", names[i]),
				   *b = diff_table_revision("WORKING", names[i]);

		column_diff_partition(rsinfo, "WORKING", NULL, a, b);
		diff_table_close(a);
		diff_table_close(b);
	}
	commits = reachable_commits(head.target, &count);
	for (i = 0; i < count; i++)
	{
		CommitInfo *c = &commits[i];
		bytea	   *db = child(c->hash, 1);
		char	   *json = metadata(db, "database");

		for (j = 0; j < Max(c->nparents, 1); j++)
		{
			CommitInfo *parent = c->nparents ? find_commit(commits, count, c->parents[j]) : NULL;
			bytea	   *pdb = parent ? child(parent->hash, 1) : NULL;
			char	   *pjson = pdb ? metadata(pdb, "database") : "{\"members\":[]}";
			int			k;

			names = changed_tables(pjson, json, &n);
			for (k = 0; k < n; k++)
			{
				DiffTable  *a = diff_table_at(pdb, names[k], true),
						   *b = diff_table_at(db, names[k], true);

				column_diff_partition(rsinfo, c->hash_text, c, a, b);
				diff_table_close(a);
				diff_table_close(b);
			}
		}
	}
	end_api(nest);
	return (Datum) 0;
}

/* ---- dolt_diff_stat ---- */

typedef struct DiffStat
{
	int64		rows_unmodified,
				rows_added,
				rows_deleted,
				rows_modified,
				cells_added,
				cells_deleted,
				cells_modified,
				old_rows,
				new_rows,
				old_cells,
				new_cells;
} DiffStat;

static void
stat_row(DiffTable *from, DiffTable *to, DiffRow *row, void *arg)
{
	DiffStat *s = (DiffStat *) arg;
	int i, j;
	bool modified = false;

	if (!strcmp(row->type, "added"))
		s->rows_added++;
	else if (!strcmp(row->type, "removed"))
		s->rows_deleted++;
	else
	{
		/* Missing columns compare as NULL. A schema rewrite alone must not
		 * turn every unchanged row into a data modification. */
		for (i = 0; i < to->desc->natts; i++)
		{
			const char *name = NameStr(TupleDescAttr(to->desc, i)->attname);
			PLValue a = {0}, b;

			for (j = 0; j < from->desc->natts; j++)
				if (!strcmp(NameStr(TupleDescAttr(from->desc, j)->attname), name))
					break;

			dg_check(pl_tuple_get(row->to.bytes, i, &b));
			if (j < from->desc->natts)
				dg_check(pl_tuple_get(row->from.bytes, j, &a));

			if (!equal_value(a, b))
			{
				s->cells_modified++;
				modified = true;
			}
		}

		for (j = 0; j < from->desc->natts; j++)
		{
			const char *name = NameStr(TupleDescAttr(from->desc, j)->attname);
			PLValue a;

			for (i = 0; i < to->desc->natts; i++)
				if (!strcmp(NameStr(TupleDescAttr(to->desc, i)->attname), name))
					break;

			if (i == to->desc->natts)
			{
				dg_check(pl_tuple_get(row->from.bytes, j, &a));
				if (a.present)
					modified = true;
			}
		}

		if (modified)
			s->rows_modified++;
	}
}

static int64
row_count(DiffTable *t)
{
	size_t		i;
	int64		n = 0;

	if (!t->present)
		return 0;
	if (t->keyed)
		return t->rows.count;
	for (i = 0; i < t->rows.count; i++)
		n += pl_read64(t->rows.items[i].value.data);
	return n;
}

/* A missing/empty table and NULL-only schema rewrites are not data edits.
 * Root hashes alone cannot distinguish those cases in our tuple encoding. */
static bool
diff_table_data_changed(const char *from, const char *to, const char *name,
                        bool explicit_table, bool *key_changed)
{
	DiffTable *a = diff_table_revision(from, name);
	DiffTable *b = diff_table_revision(to, name);
	DiffStat stats = {0};
	bool changed;

	*key_changed = false;
	if (!a->present || !b->present)
		changed = row_count(a) != 0 || row_count(b) != 0;
	else if (a->keyed != b->keyed || (a->keyed && !same_key_columns(a, b)))
	{
		ereport(explicit_table ? ERROR : WARNING,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("failed to compute diff summary for table %s: primary key set changed", name)));
		*key_changed = true;
		changed = false;
	}
	else
	{
		diff_rows(a, b, stat_row, &stats);
		changed = stats.rows_added || stats.rows_deleted || stats.rows_modified;
	}

	diff_table_close(a);
	diff_table_close(b);
	return changed;
}

/* Object statistics follow Dolt's one-row-per-schema-object convention. */
static char **
changed_diff_objects(const char *a, const char *b, int *count)
{
	char	  **names;
	int			i;

	run("WITH a AS (SELECT v FROM jsonb_array_elements($1::jsonb->'members') v WHERE v->>'kind' IN ('table','routine','view','type','trigger','sequence','aggregate','operator','cast')), b AS (SELECT v FROM jsonb_array_elements($2::jsonb->'members') v WHERE v->>'kind' IN ('table','routine','view','type','trigger','sequence','aggregate','operator','cast')) SELECT DISTINCT coalesce(a.v->>'name',b.v->>'name') COLLATE \"C\" FROM a FULL JOIN b ON a.v->>'name'=b.v->>'name' AND a.v->>'kind'=b.v->>'kind' WHERE a.v IS NULL OR b.v IS NULL OR a.v->>'content' IS DISTINCT FROM b.v->>'content' OR a.v->>'schema' IS DISTINCT FROM b.v->>'schema' ORDER BY 1", 2,
		VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(a), CStringGetTextDatum(b)), NULL, true);
	*count = SPI_processed;
	names = palloc(sizeof(char *) * Max(*count, 1));
	for (i = 0; i < *count; i++)
		names[i] = strcell(i, 1);
	return names;
}

Datum
dolt_diff_stat(PG_FUNCTION_ARGS)
{
	char *from, *to, *table;
	int			nest = begin_api();
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char *fromjson, *tojson;
	char	  **names;
	int			n,
				i;

	diff_revision_arguments(fcinfo, &from, &to, &table);
	fromjson = state_json(from);
	tojson = !strcmp(from, to) ? fromjson : state_json(to);
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	if (table)
	{
		names = palloc(sizeof(char *));
		names[0] = resolve_diff_argument(table, fromjson, tojson);
		n = 1;
	}
	else
		names = changed_diff_objects(fromjson, tojson, &n);
	for (i = 0; i < n; i++)
	{
		DiffTable  *a = diff_object_revision(from, names[i], true),
				   *b = diff_object_revision(to, names[i], true);
		DiffStat	s = {0};
		bool key_changed = false;
		Datum		values[12];
		bool		nulls[12];

		if (!a->present && !b->present)
			continue;
		if (a->object_kind || b->object_kind)
		{
			if (a->present && b->present &&
				(!a->object_kind || !b->object_kind || strcmp(a->object_kind, b->object_kind)))
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("cannot diff %s: object kind changed between revisions", names[i])));
			s.rows_added = !a->present;
			s.rows_deleted = !b->present;
			s.rows_modified = a->present && b->present && !same_schema_definition(a->schema, b->schema);

			/*
			 * Match Dolt's root-object DiffStatProgress: only Adds, Removes
			 * or Changes is set. Row/cell totals remain zero. Consequently
			 * rows_unmodified below is -1 for deletion or modification.
			 */
		}
		else
		{
			key_changed = a->present && b->present &&
				(a->keyed != b->keyed || (a->keyed && !same_key_columns(a, b)));
			if (key_changed)
			{
				ereport(table ? ERROR : WARNING,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("failed to compute diff stat for table %s: primary key set changed", names[i])));
				/* The all-table form reports an unavailable zero row and
				 * continues with tables whose keys can be aligned. */
				goto stat_values;
			}

			diff_rows(a, b, stat_row, &s);
			s.old_rows = row_count(a);
			s.new_rows = row_count(b);
			s.old_cells = a->present ? s.old_rows * a->desc->natts : 0;
			s.new_cells = b->present ? s.new_rows * b->desc->natts : 0;
			/* Dolt derives structural cell additions/deletions from the total
			 * sizes, including schema changes on otherwise unchanged rows. */
			{
				int width = b->present ? b->desc->natts : 0;
				int64 delta = s.new_cells - s.old_cells;

				if (delta > 0)
				{
					s.cells_deleted = s.rows_deleted * width;
					s.cells_added = delta + s.cells_deleted;
				}
				else if (delta < 0)
				{
					s.cells_added = s.rows_added * width;
					s.cells_deleted = -delta + s.cells_added;
				}
				else
					s.cells_added = s.cells_deleted = Max(s.rows_added, s.rows_deleted) * width;
			}
		}
stat_values:
		s.rows_unmodified = s.old_rows - s.rows_deleted - s.rows_modified;
		memset(nulls, false, sizeof(nulls));
		values[0] = CStringGetTextDatum(b->present ? b->name : a->name);
		values[1] = Int64GetDatum(s.rows_unmodified);
		values[2] = Int64GetDatum(s.rows_added);
		values[3] = Int64GetDatum(s.rows_deleted);
		values[4] = Int64GetDatum(s.rows_modified);
		values[5] = Int64GetDatum(s.cells_added);
		values[6] = Int64GetDatum(s.cells_deleted);
		values[7] = Int64GetDatum(s.cells_modified);
		values[8] = Int64GetDatum(s.old_rows);
		values[9] = Int64GetDatum(s.new_rows);
		values[10] = Int64GetDatum(s.old_cells);
		values[11] = Int64GetDatum(s.new_cells);
		/* Keyless rows have no stable identity for modified-row statistics.
		 * Match Dolt's public contract: only additions/deletions are known. */
		if (!a->object_kind && !b->object_kind &&
			((a->present && !a->keyed) || (b->present && !b->keyed)))
		{
			int j;

			for (j = 1; j < 12; j++)
				nulls[j] = j != 2 && j != 3;
		}

		/* Empty changes have no row; structural cell changes still do. */
		if (key_changed || s.rows_added || s.rows_deleted || s.rows_modified ||
			s.cells_added || s.cells_deleted)
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		diff_table_close(a);
		diff_table_close(b);
	}
	end_api(nest);
	return (Datum) 0;
}

Datum
dolt_diff_stat_table(PG_FUNCTION_ARGS)
{
	return dolt_diff_stat(fcinfo);
}

Datum
dolt_diff_stat_range(PG_FUNCTION_ARGS)
{
 return dolt_diff_stat(fcinfo);
}
