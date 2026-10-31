/* Per-schema Dolt tables (dolt_ignore, dolt_schemas, dolt_procedures) and
 * small administrative procedures. See doc/doltgres/schema-tables.md.
 * Included by dg_version.c after dg_patch.inc. */

/* ---- dolt_ignore patterns ----
 *
 * Each schema may hold a real dolt_ignore(pattern, ignored) table, created on
 * the first INSERT into it. Patterns use Dolt's glob syntax ('*' and '%'
 * match any sequence, '?' one character) against unqualified table names of
 * that schema. Staging everything (dolt_add -A, dolt_commit -A) leaves
 * ignored untracked tables in the working set. */

typedef struct IgnorePattern
{
	char	   *pattern;
	bool		ignore;
	bool		removed;		/* overridden by a more specific pattern */
}			IgnorePattern;

/* Glob match. In specificity mode the pattern is matched against another
 * pattern: a '?' may stand for any character except a wildcard. */
static bool
pattern_matches(const char *p, const char *s, bool specificity)
{
	for (;;)
	{
		if (*p == '*' || *p == '%')
		{
			while (*p == '*' || *p == '%')
				p++;
			if (!*p)
				return true;
			for (; *s; s++)
				if (pattern_matches(p, s, specificity))
					return true;
			return false;
		}
		if (!*p)
			return !*s;
		if (!*s)
			return false;
		if (*p == '?' ? (!specificity || (*s != '*' && *s != '%')) : *p == *s)
		{
			p++;
			s++;
			continue;
		}
		return false;
	}
}

/* '*' and '%' are the same wildcard; runs collapse. */
static char *
normalize_pattern(const char *pattern)
{
	StringInfoData out;

	initStringInfo(&out);
	for (; *pattern; pattern++)
	{
		if ((*pattern == '*' || *pattern == '%') && out.len && out.data[out.len - 1] == '%')
			continue;
		appendStringInfoChar(&out, *pattern == '*' ? '%' : *pattern);
	}
	return out.data;
}

/* Dolt's IsTableNameIgnored: a table is ignored when it matches a true
 * pattern and every matching false pattern is overridden by a more specific
 * true pattern. Contradictory patterns leave the table tracked. */
static bool
table_ignored(IgnorePattern * patterns, int n, const char *table)
{
	int			i,
				j,
				trues = 0,
				falses = 0,
				removed_trues = 0,
				removed_falses = 0;

	if (!strcmp(table, "dolt_rebase"))
		return true;
	for (i = 0; i < n; i++)
	{
		patterns[i].removed = false;
		if (pattern_matches(patterns[i].pattern, table, false))
			patterns[i].ignore ? trues++ : falses++;
		else
			patterns[i].removed = true;
	}
	if (!trues)
		return false;
	if (!falses)
		return true;
	for (i = 0; i < n; i++)
	{
		if (patterns[i].removed)
			continue;
		for (j = 0; j < n; j++)
		{
			if (patterns[j].removed || patterns[j].ignore == patterns[i].ignore)
				continue;
			if (!strcmp(normalize_pattern(patterns[i].pattern), normalize_pattern(patterns[j].pattern)))
				return false;
			if (pattern_matches(patterns[i].pattern, patterns[j].pattern, true))
			{
				patterns[i].ignore ? removed_trues++ : removed_falses++;
				break;
			}
		}
	}
	if (removed_trues == trues)
		return false;
	return removed_falses == falses;
}

static bool
schema_has_ignore_table(const char *schema)
{
	run_context("SELECT 1 FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname=$1 AND c.relname='dolt_ignore' AND c.relkind='r'", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(schema)), NULL, true);
	return SPI_processed > 0;
}

static IgnorePattern *
schema_ignore_patterns(const char *schema, int *count)
{
	IgnorePattern *result;
	int			i;

	*count = 0;
	if (!schema_has_ignore_table(schema))
		return NULL;
	run_context(psprintf("SELECT pattern,ignored FROM %s.dolt_ignore ORDER BY pattern COLLATE \"C\"", dg_quote_identifier(schema)), 0, NULL, NULL, NULL, true);
	*count = SPI_processed;
	result = palloc0(sizeof(IgnorePattern) * (*count + 1));
	for (i = 0; i < *count; i++)
	{
		result[i].pattern = strcell(i, 1);
		result[i].ignore = DatumGetBool(cell(i, 2));
	}
	return result;
}

/* Subset of |tables| (qualified logical names) that ignore patterns exclude. */
static List *
ignored_tables(List *tables)
{
	List	   *result = NIL;
	ListCell   *lc;
	char	   *cached_schema = NULL;
	IgnorePattern *patterns = NULL;
	int			n = 0;

	foreach(lc, tables)
	{
		char	   *name = (char *) lfirst(lc);
		List	   *parts = stringToQualifiedNameList(name, NULL);
		char	   *schema = strVal(linitial(parts)),
				   *table = strVal(lsecond(parts));

		if (!strcmp(table, "dolt_ignore"))
			continue;
		if (!cached_schema || strcmp(cached_schema, schema))
		{
			cached_schema = schema;
			patterns = schema_ignore_patterns(schema, &n);
		}
		if (table_ignored(patterns, n, table))
			result = lappend(result, name);
	}
	return result;
}

/* Ignore rules apply only to tables absent from both HEAD and the supplied
 * tracked snapshot. Explicitly force-added tables subsequently stay tracked. */
static List *
ignored_untracked_tables(const char *working, const char *tracked)
{
	List *known = list_concat(state_tables(head_state()), state_tables(tracked));
	List *candidates = NIL;
	VersionRef pending = read_ref(branch_ref("merge"));
	ListCell *lc;

	if (pending.target)
		known = list_concat(known, state_tables(metadata(child(child(pending.target, 2), 1), "database")));
	foreach(lc, state_tables(working))
		if (!name_selected(known, lfirst(lc)))
			candidates = lappend(candidates, lfirst(lc));
	return ignored_tables(candidates);
}

/* Capture the working set and drop ignored untracked tables from it: the
 * state that dolt_add -A and dolt_commit -A stage. |full| receives the
 * complete capture for the working ref. */
static bytea *
staged_capture(Capture *full)
{
	VersionRef	staged = read_ref(branch_ref("staged"));
	bytea	   *base = staged.target ? staged.target : child(read_ref(branch_ref("heads")).target, 1);
	List *ignored;

	*full = capture_visible();
	ignored = ignored_untracked_tables(full->json, staged_state());
	if (ignored == NIL)
		return full->hash;
	return compose_database(full->hash, base, ignored);
}

/* Create a schema's dolt_ignore table before the first INSERT into it. */
void
dg_ensure_ignore_table(const char *schema)
{
	int			nest = begin_api();

	bool		previous = dg_internal_system_ddl;

	PG_TRY();
	{
		dg_internal_system_ddl = true;
		ensure_repository();
		if (!schema_has_ignore_table(schema))
		{
			run_context(psprintf("CREATE TABLE %s.dolt_ignore (pattern text PRIMARY KEY, ignored boolean NOT NULL) USING dolt_prolly_table", dg_quote_identifier(schema)), 0, NULL, NULL, NULL, false);
			CommandCounterIncrement();
		}
	}
	PG_FINALLY();
	{
		dg_internal_system_ddl = previous;
	}
	PG_END_TRY();
	end_api(nest);
}

/* An absent dolt_ignore reads as an empty table. */
Datum
dolt_ignore_table(PG_FUNCTION_ARGS)
{
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	return (Datum) 0;
}

/* ---- dolt_schemas: views of a schema ---- */
Datum
dolt_schemas_table(PG_FUNCTION_ARGS)
{
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			nest = begin_api();

	run_context("SELECT 'view'::text,c.relname::text,'CREATE VIEW ' || quote_ident(c.relname) || ' AS ' || pg_get_viewdef(c.oid,true),'{\"CreatedAt\":0}'::json,NULL::text FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname=$1 AND c.relkind='v' ORDER BY c.relname COLLATE \"C\"", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(schema)), NULL, true);
	return_rows(fcinfo, nest);
	return (Datum) 0;
}

/* ---- dolt_procedures: procedures of a schema ---- */
Datum
dolt_procedures_table(PG_FUNCTION_ARGS)
{
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			nest = begin_api();

	run_context("SELECT p.proname::text,pg_get_functiondef(p.oid),NULL::timestamptz,NULL::timestamptz,NULL::text FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace WHERE n.nspname=$1 AND p.prokind='p' ORDER BY p.proname COLLATE \"C\"", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(schema)), NULL, true);
	return_rows(fcinfo, nest);
	return (Datum) 0;
}

/* ---- Administrative procedures ---- */

/* Expire reflog observations as Dolt GC does. Manifest/chunk reclamation
 * remains deferred: live revision handles and retained roots must be accounted
 * for before deleting their objects. PostgreSQL keeps the session usable. */
Datum
dolt_gc(PG_FUNCTION_ARGS)
{
	int			n,
				i;
	char	  **args = arguments(fcinfo, &n);

	for (i = 0; i < n; i++)
		if (strcmp(args[i], "--shallow") && strcmp(args[i], "-s") && strcmp(args[i], "--full") && strcmp(args[i], "--archive-level") && strcmp(args[i], "0") && strcmp(args[i], "1"))
			unsupported(args[i]);
	{
		int nest = begin_api();

		lock_workspace();
		run("DELETE FROM pg_catalog.pg_dolt_reflog", 0, NULL, NULL, NULL, false);
		end_api(nest);
	}
	PG_RETURN_INT64(0);
}

Datum
dolt_gc_noargs(PG_FUNCTION_ARGS)
{
	return dolt_gc(fcinfo);
}

/* Dropped databases are removed by PostgreSQL immediately. */
Datum
dolt_purge_dropped_databases(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(0);
}

Datum
dolt_undrop(PG_FUNCTION_ARGS)
{
	int			n;
	char	  **args = arguments(fcinfo, &n);

	if (n != 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("dolt_undrop: database name required")));
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("dolt_undrop: database '%s' cannot be restored; DROP DATABASE is not versioned", args[0])));
	PG_RETURN_INT64(1);
}

Datum
dolt_thread_dump(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(""));
}

/* Lazily allocated ordinary user tables, versioned by normal capture and DDL. */
void
dg_ensure_user_table(const char *name)
{
	int			nest = begin_api();
	bool		previous = dg_internal_system_ddl;
	const char *columns;

	if (!strcmp(name, "dolt_docs"))
		columns = "doc_name text PRIMARY KEY, doc_text text";
	else if (!strcmp(name, "dolt_tests"))
		columns = "test_name text PRIMARY KEY, test_group text, test_query text NOT NULL, assertion_type text NOT NULL, assertion_comparator text NOT NULL, assertion_value text";
	else if (!strcmp(name, "dolt_query_catalog"))
		columns = "id text PRIMARY KEY, display_order int4, name text, query text, description text";
	else
		elog(ERROR, "unknown versioned metadata table");
	PG_TRY();
	{
		dg_internal_system_ddl = true;
		run_context("SELECT EXISTS(SELECT 1 FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='public' AND c.relname=$1)", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(name)), NULL, true);
		if (!DatumGetBool(cell(0, 1)))
		{
			run_context(psprintf("CREATE TABLE public.%s (%s) USING dolt_prolly_table", name, columns), 0, NULL, NULL, NULL, false);
			if (!strcmp(name, "dolt_docs"))
			{
				CommandCounterIncrement();
				UpdateActiveSnapshotCommandId();
				run_context("INSERT INTO public.dolt_docs VALUES ('AGENT.md',pg_catalog.dolt_default_doc('AGENT.md'))", 0, NULL, NULL, NULL, false);
			}
			if (!strcmp(name, "dolt_tests"))
				run_context("ALTER TABLE public.dolt_tests ADD CONSTRAINT assertion_type_check CHECK(assertion_type IN ('expected_rows','expected_columns','expected_single_value')), ADD CONSTRAINT assertion_comparator_check CHECK(assertion_comparator IN ('==','!=','<','>','<=','>='))", 0, NULL, NULL, NULL, false);
			if (!strcmp(name, "dolt_query_catalog"))
				run_context("ALTER TABLE public.dolt_query_catalog ALTER COLUMN display_order SET DEFAULT 0", 0, NULL, NULL, NULL, false);
			CommandCounterIncrement();
			UpdateActiveSnapshotCommandId();
		}
	}
	PG_FINALLY();
	{
		dg_internal_system_ddl = previous;
	}
	PG_END_TRY();
	end_api(nest);
	CommandCounterIncrement();
	if (ActiveSnapshotSet())
		UpdateActiveSnapshotCommandId();
}

Datum
dolt_user_table(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const char *columns;
	const char *empty;
	int			nest = begin_api();
	bool		exists;
	char	   *query;

	if (!strcmp(name, "dolt_docs"))
	{
		columns = "doc_name,doc_text";
		empty = "NULL::text AS doc_name,NULL::text AS doc_text";
	}
	else if (!strcmp(name, "dolt_tests"))
	{
		columns = "test_name,test_group,test_query,assertion_type,assertion_comparator,assertion_value";
		empty = "NULL::text,NULL::text,NULL::text,NULL::text,NULL::text,NULL::text";
	}
	else if (!strcmp(name, "dolt_query_catalog"))
	{
		columns = "id,display_order,name,query,description";
		empty = "NULL::text,NULL::int4,NULL::text,NULL::text,NULL::text";
	}
	else
		elog(ERROR, "unknown versioned metadata table");
	run_context("SELECT EXISTS(SELECT 1 FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='public' AND c.relname=$1)", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(name)), NULL, true);
	exists = DatumGetBool(cell(0, 1));
	query = exists ? psprintf("SELECT %s FROM public.%s", columns, name) : psprintf("SELECT %s WHERE false", empty);
	if (!strcmp(name, "dolt_docs"))
		query = psprintf("%s UNION ALL SELECT 'AGENT.md'::text,pg_catalog.dolt_default_doc('AGENT.md') %s", query, exists ? "WHERE NOT EXISTS (SELECT 1 FROM public.dolt_docs WHERE doc_name='AGENT.md')" : "");
	run_context(query, 0, NULL, NULL, NULL, true);
	return_rows(fcinfo, nest);
	return (Datum) 0;
}
