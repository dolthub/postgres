/* dolt_query_diff, the dolt_help table and dolt_test_run. See
 * doc/doltgres/rebase-stash.md. Included by dg_version.c after dg_rebase.inc. */

/* Run |sql| with the caller's search path inside the API session. */
static void
run_user_query(const char *sql)
{
	run_context(sql, 0, NULL, NULL, NULL, true);
}

/* ---- dolt_query_diff ----
 * Direct projections of a complete primary key can identify modified rows.
 * Other results use multiset comparison. Logical names, not OIDs or column
 * fingerprints, distinguish tables across revisions.
 */
typedef struct QueryIdentity
{
	char	   *source;
	char	   *signature;
	int			nkeys;
	int		   *columns;
} QueryIdentity;

static QueryIdentity
query_identity(const char *sql)
{
	QueryIdentity result = {0};
	List	   *raw = raw_parser(sql, RAW_PARSE_DEFAULT);
	Query	   *query;
	RangeTblEntry *rte;
	Oid			relid,
				pk;
	Relation	rel,
				index;
	StringInfoData signature;
	int			k;

	if (list_length(raw) != 1)
		return result;
	query = parse_analyze_fixedparams(linitial_node(RawStmt, raw), sql, NULL, 0, NULL);
	if (query->commandType != CMD_SELECT || list_length(query->rtable) != 1 ||
		query->hasAggs || query->hasWindowFuncs || query->hasTargetSRFs ||
		query->setOperations || query->groupClause || query->distinctClause)
		return result;
	rte = linitial_node(RangeTblEntry, query->rtable);
	relid = OidIsValid(rte->dolt_rowtype) ? get_typ_typrelid(rte->dolt_rowtype) :
		(rte->rtekind == RTE_RELATION ? rte->relid : InvalidOid);
	if (!OidIsValid(relid))
		return result;
	rel = relation_open(relid, AccessShareLock);
	result.source = rel->rd_catalogview ?
		pstrdup(CatalogViewObjectIdentity(relid, RelationRelationId)) :
		dg_qualified_name(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel));
	pk = RelationGetPrimaryKeyIndex(rel, false);
	/* A parent key does not constrain rows in inherited child tables. */
	if (!OidIsValid(pk) || (rte->inh && has_subclass(relid)))
	{
		relation_close(rel, AccessShareLock);
		return result;
	}

	/* Catalog metadata suffices; historical indexes need no storage binding. */
	index = index_open(pk, AccessShareLock);
	result.nkeys = index->rd_index->indnkeyatts;
	result.columns = palloc(sizeof(int) * result.nkeys);
	initStringInfo(&signature);
	for (k = 0; k < result.nkeys; k++)
	{
		AttrNumber	attnum = index->rd_index->indkey.values[k];
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);
		ListCell   *lc;
		int			column = 0;
		bool		found = false;

		appendStringInfo(&signature, "%zu:%s:%u;", strlen(NameStr(att->attname)),
						 NameStr(att->attname), att->atttypid);
		foreach(lc, query->targetList)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);
			Var		   *var = IsA(tle->expr, Var) ? (Var *) tle->expr : NULL;

			if (tle->resjunk)
				continue;
			if (var && var->varno == 1 && var->varattno == attnum && var->varlevelsup == 0)
			{
				result.columns[k] = column;
				found = true;
				break;
			}
			column++;
		}
		if (!found)
		{
			result.nkeys = 0;
			break;
		}
	}
	if (result.nkeys)
		result.signature = signature.data;
	index_close(index, AccessShareLock);
	relation_close(rel, AccessShareLock);
	return result;
}

typedef struct QueryRow
{
	char	   *key;
	char	   *row;
	Datum	   *values;
	bool	   *nulls;
} QueryRow;

static int
query_row_compare(const void *a, const void *b)
{
	return strcmp(((const QueryRow *) a)->key, ((const QueryRow *) b)->key);
}

/* Length framing distinguishes NULL, empty strings and embedded delimiters. */
static void
query_field(StringInfo key, const char *value)
{
	if (value)
		appendStringInfo(key, "%zu:%s;", strlen(value), value);
	else
		appendStringInfoString(key, "N;");
}

static QueryRow *
query_rows(const char *sql, MemoryContext cxt, int *count, int *natts,
		   QueryIdentity *identity, bool keyed, bool distinct_sources)
{
	QueryRow   *rows;
	int			r,
				c;

	run_user_query(sql);
	*count = SPI_processed;
	*natts = SPI_tuptable->tupdesc->natts;
	rows = MemoryContextAlloc(cxt, sizeof(QueryRow) * Max(*count, 1));
	for (r = 0; r < *count; r++)
	{
		StringInfoData key,
					row;
		MemoryContext old = MemoryContextSwitchTo(cxt);
		char	  **fields = palloc(sizeof(char *) * *natts);

		initStringInfo(&key);
		initStringInfo(&row);
		rows[r].values = palloc(sizeof(Datum) * *natts);
		rows[r].nulls = palloc(sizeof(bool) * *natts);
		for (c = 0; c < *natts; c++)
		{
			Form_pg_attribute att = TupleDescAttr(SPI_tuptable->tupdesc, c);
			Datum		v = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, c + 1, &rows[r].nulls[c]);

			fields[c] = rows[r].nulls[c] ? NULL : SPI_getvalue(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, c + 1);
			rows[r].values[c] = rows[r].nulls[c] ? (Datum) 0 : datumCopy(v, att->attbyval, att->attlen);
			appendStringInfo(&row, "%u/", att->atttypid);
			query_field(&row, fields[c]);
		}
		if (distinct_sources)
			query_field(&key, identity->source);
		if (keyed)
		{
			for (c = 0; c < identity->nkeys; c++)
				query_field(&key, fields[identity->columns[c]]);
		}
		else
			appendStringInfoString(&key, row.data);
		rows[r].key = key.data;
		rows[r].row = row.data;
		MemoryContextSwitchTo(old);
	}
	return rows;
}

Datum
dolt_query_diff(PG_FUNCTION_ARGS)
{
	char	   *first = text_to_cstring(PG_GETARG_TEXT_PP(0)),
			   *second = text_to_cstring(PG_GETARG_TEXT_PP(1));
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext cxt = rsinfo->econtext->ecxt_per_query_memory;
	int			nest,
				na,
				nb,
				wa,
				wb,
				i = 0,
				j = 0,
				total;
	QueryRow   *a,
			   *b;
	Datum	   *values;
	bool	   *nulls;
	QueryIdentity left = query_identity(first),
				right = query_identity(second);
	bool		distinct_sources = left.source && right.source && strcmp(left.source, right.source);
	bool		keyed = !distinct_sources && left.source && right.source &&
		left.signature && right.signature && !strcmp(left.signature, right.signature);

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	nest = begin_api();
	a = query_rows(first, cxt, &na, &wa, &left, keyed, distinct_sources);
	b = query_rows(second, cxt, &nb, &wb, &right, keyed, distinct_sources);
	total = wa + wb + 1;
	if (rsinfo->setDesc->natts != total)
		ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("dolt_query_diff result layout does not match the queries")));
	qsort(a, na, sizeof(QueryRow), query_row_compare);
	qsort(b, nb, sizeof(QueryRow), query_row_compare);
	values = palloc(sizeof(Datum) * total);
	nulls = palloc(sizeof(bool) * total);
	while (i < na || j < nb)
	{
		int			cmp = i == na ? 1 : (j == nb ? -1 : strcmp(a[i].key, b[j].key));
		int			c;

		if (cmp == 0 && !strcmp(a[i].row, b[j].row))
		{
			i++;
			j++;
			continue;
		}
		for (c = 0; c < total; c++)
			nulls[c] = true;
		if (cmp <= 0)
		{
			for (c = 0; c < wa; c++)
			{
				values[c] = a[i].values[c];
				nulls[c] = a[i].nulls[c];
			}
			values[total - 1] = CStringGetTextDatum("deleted");
			i++;
		}
		if (cmp >= 0)
		{
			for (c = 0; c < wb; c++)
			{
				values[wa + c] = b[j].values[c];
				nulls[wa + c] = b[j].nulls[c];
			}
			values[total - 1] = CStringGetTextDatum("added");
			j++;
		}
		if (cmp == 0)
			values[total - 1] = CStringGetTextDatum("modified");
		nulls[total - 1] = false;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	end_api(nest);
	return (Datum) 0;
}

/* ---- dolt_help ---- */

typedef struct HelpEntry
{
	const char *name;
	const char *type;
	const char *synopsis;
	const char *short_description;
	const char *arguments;
}			HelpEntry;

static const HelpEntry help_entries[] = {
	{"dolt_add", "procedure", "dolt_add('.' | '-a' | '-A' | table...)", "Add working set changes to the staged set.", "{\"-A, --all\": \"Stage all supported objects, including empty namespaces\", \"table\": \"Table to stage\"}"},
	{"dolt_branch", "procedure", "dolt_branch([-c | -m | -d | -D | -f] name [start-point])", "Create, copy, move or delete branches.", "{\"-c, --copy\": \"Copy a branch\", \"-m, --move\": \"Rename a branch\", \"-d, --delete\": \"Delete a merged branch\", \"-D\": \"Force delete\", \"-f, --force\": \"Reset an existing branch\"}"},
	{"dolt_checkout", "procedure", "dolt_checkout([-b] branch [start-point] | table...)", "Switch branches or restore working tables.", "{\"-b\": \"Create and switch to a new branch\", \"table\": \"Restore the table from HEAD\"}"},
	{"dolt_cherry_pick", "procedure", "dolt_cherry_pick(commit)", "Apply the changes of an existing commit.", "{\"commit\": \"Commit to apply\", \"--author\": \"Override the commit author\"}"},
	{"dolt_clean", "procedure", "dolt_clean([--dry-run] [table...])", "Delete untracked tables from the working set.", "{\"--dry-run\": \"Report what would be deleted\"}"},
	{"dolt_commit", "procedure", "dolt_commit([-a | -A] [-m message] [--author 'name <email>'] [--allow-empty] [--skip-empty])", "Record staged changes as a new commit.", "{\"-m, --message\": \"Commit message\", \"-a, --all\": \"Stage modified and deleted tracked tables\", \"-A, --ALL\": \"Stage all supported objects, including empty namespaces before committing\", \"--author\": \"Author name and email\", \"--allow-empty\": \"Allow an empty commit\", \"--skip-empty\": \"Do nothing when there are no changes\"}"},
	{"dolt_conflicts_resolve", "procedure", "dolt_conflicts_resolve(--ours | --theirs, table...)", "Resolve merge conflicts of tables.", "{\"--ours\": \"Keep the destination rows\", \"--theirs\": \"Take the merged rows\"}"},
	{"dolt_fetch", "procedure", "dolt_fetch([remote])", "Fetch refs from a remote (unsupported: no remote transport).", "{}"},
	{"dolt_gc", "procedure", "dolt_gc([--shallow | --full])", "Prune reflog history; physical object reclamation is deferred.", "{\"--shallow\": \"Quick collection\", \"--full\": \"Complete collection\"}"},
	{"dolt_merge", "procedure", "dolt_merge([--no-ff] [--squash] [--abort] [-m message] branch)", "Merge a branch into the current branch.", "{\"--no-ff\": \"Always create a merge commit\", \"--squash\": \"Merge without committing\", \"--abort\": \"Abort an unfinished merge\", \"-m, --message\": \"Merge commit message\"}"},
	{"dolt_pull", "procedure", "dolt_pull([remote [branch]])", "Fetch and merge from a remote (unsupported: no remote transport).", "{}"},
	{"dolt_push", "procedure", "dolt_push([remote [branch]])", "Push a branch to a remote (unsupported: no remote transport).", "{}"},
	{"dolt_rebase", "procedure", "dolt_rebase(-i upstream | --continue | --abort)", "Reapply the commits of the current branch on top of another branch using the dolt_rebase plan table.", "{\"-i, --interactive\": \"Start an interactive rebase\", \"--continue\": \"Apply the plan\", \"--abort\": \"Abandon the rebase\"}"},
	{"dolt_remote", "procedure", "dolt_remote(add name url | remove name)", "Manage remote definitions.", "{\"add\": \"Add a remote\", \"remove\": \"Remove a remote\"}"},
	{"dolt_reset", "procedure", "dolt_reset([--hard | --soft] [revision] | table...)", "Reset the staged set or the branch head.", "{\"--hard\": \"Reset working and staged sets\", \"--soft\": \"Move HEAD only\"}"},
	{"dolt_revert", "procedure", "dolt_revert(commit...)", "Create commits that undo earlier commits.", "{\"commit\": \"Commit to revert\", \"--author\": \"Override the commit author\"}"},
	{"dolt_rm", "procedure", "dolt_rm([--cached] table...)", "Remove tables from the working and staged sets.", "{\"--cached\": \"Unstage only\"}"},
	{"dolt_stash", "procedure", "dolt_stash(push | pop | apply | drop | clear, name [index])", "Save and restore working set changes.", "{\"push\": \"Save local changes\", \"pop\": \"Apply and remove a stash\", \"apply\": \"Apply a stash\", \"drop\": \"Remove a stash\", \"clear\": \"Remove all stashes of a name\"}"},
	{"dolt_tag", "procedure", "dolt_tag([-m message] name [revision] | -d name)", "Create or delete tags.", "{\"-m, --message\": \"Tag message\", \"-d\": \"Delete a tag\"}"},
	{"dolt_verify_constraints", "procedure", "dolt_verify_constraints([--all] [--output-only] [table...])", "Verify constraints of the working set.", "{\"--all\": \"Check all rows\", \"--output-only\": \"Do not record violations\"}"},
	{"dolt_count_commits", "procedure", "dolt_count_commits(--from a --to b)", "Count commits between two revisions.", "{\"--from\": \"Start revision\", \"--to\": \"End revision\"}"},
	{"dolt_undrop", "procedure", "dolt_undrop(database)", "Restore a dropped database (unsupported).", "{}"},
	{"dolt_purge_dropped_databases", "procedure", "dolt_purge_dropped_databases()", "Permanently remove dropped databases (no-op).", "{}"},
	{"active_branch", "function", "active_branch()", "Name of the session branch.", "{}"},
	{"dolt_hashof", "function", "dolt_hashof(revision)", "Commit hash of a revision.", "{\"revision\": \"Branch, tag, HEAD or hash\"}"},
	{"dolt_hashof_table", "function", "dolt_hashof_table(table)", "Content hash of a working table.", "{\"table\": \"Table name\"}"},
	{"dolt_hashof_db", "function", "dolt_hashof_db([revision])", "Hash of a database state.", "{\"revision\": \"Branch, WORKING or STAGED\"}"},
	{"dolt_merge_base", "function", "dolt_merge_base(a, b)", "Common ancestor of two revisions.", "{}"},
	{"dolt_version", "function", "dolt_version()", "Version of the server.", "{}"},
	{"dolt_diff", "function", "dolt_diff(from, to, table)", "Row differences of a table between two revisions.", "{}"},
	{"dolt_diff_stat", "function", "dolt_diff_stat(from, to [, table])", "Row and cell counts of changes between two revisions.", "{}"},
	{"dolt_diff_summary", "function", "dolt_diff_summary(from, to [, table])", "Tables changed between two revisions.", "{}"},
	{"dolt_patch", "function", "dolt_patch(from, to [, table])", "SQL statements transforming one revision into another.", "{}"},
	{"dolt_schema_diff", "function", "dolt_schema_diff(from, to [, table])", "Schema differences between two revisions.", "{}"},
	{"dolt_log", "function", "dolt_log([revision] [-n count])", "Commit history.", "{\"-n, --number\": \"Limit the number of commits\"}"},
	{"dolt_reflog", "function", "dolt_reflog([ref])", "History of ref movements.", "{}"},
	{"dolt_query_diff", "function", "dolt_query_diff(query1, query2)", "Rows that differ between two query results.", "{}"},
	{"dolt_test_run", "function", "dolt_test_run([name | group])", "Run the tests stored in dolt_tests.", "{}"},
	{"dolt_preview_merge_conflicts", "function", "dolt_preview_merge_conflicts(a, b, table)", "Conflicts a merge of two branches would produce in a table.", "{}"},
	{"dolt_preview_merge_conflicts_summary", "function", "dolt_preview_merge_conflicts_summary(a, b)", "Tables a merge of two branches would conflict in.", "{}"},
};

Datum
dolt_help(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			i;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	for (i = 0; i < lengthof(help_entries); i++)
	{
		const		HelpEntry *h = &help_entries[i];
		Datum		values[6];
		bool		nulls[6] = {false, false, false, false, false, false};

		values[0] = CStringGetTextDatum(h->name);
		values[1] = CStringGetTextDatum(h->type);
		values[2] = CStringGetTextDatum(h->synopsis);
		values[3] = CStringGetTextDatum(h->short_description);
		values[4] = CStringGetTextDatum(h->short_description);
		values[5] = DirectFunctionCall1(json_in, CStringGetDatum(h->arguments));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* ---- dolt_test_run ---- */

static bool
numeric_text(const char *s, double *value)
{
	char	   *end;

	if (!s || !*s)
		return false;
	*value = strtod(s, &end);
	return *end == '\0';
}

/* Dolt's comparison of an actual value against the assertion value. */
static char *
compare_assertion(const char *type, const char *comparator, const char *expected, const char *actual, bool *passed)
{
	double		a,
				b;
	int			cmp;
	const char *wording;

	if (numeric_text(expected, &a) && numeric_text(actual, &b))
		cmp = a < b ? 1 : (a > b ? -1 : 0);
	else
		cmp = strcmp(actual ? actual : "", expected ? expected : "");
	if (!strcmp(comparator, "=="))
	{
		*passed = cmp == 0;
		wording = "equal to";
	}
	else if (!strcmp(comparator, "!="))
	{
		*passed = cmp != 0;
		wording = "not equal to";
	}
	else if (!strcmp(comparator, "<"))
	{
		*passed = cmp < 0;
		wording = "less than";
	}
	else if (!strcmp(comparator, "<="))
	{
		*passed = cmp <= 0;
		wording = "less than or equal to";
	}
	else if (!strcmp(comparator, ">"))
	{
		*passed = cmp > 0;
		wording = "greater than";
	}
	else if (!strcmp(comparator, ">="))
	{
		*passed = cmp >= 0;
		wording = "greater than or equal to";
	}
	else
	{
		*passed = false;
		return psprintf("%s is not a valid comparison type", comparator);
	}
	if (*passed)
		return "";
	return psprintf("Assertion failed: %s %s %s, got %s", type, wording, expected ? expected : "NULL", actual ? actual : "NULL");
}

/* Run one test query in a subtransaction so a failing query reports an
 * error status instead of aborting the whole run. */
static char *
run_test_query(const char *sql, const char *type, const char *comparator, const char *expected, bool *passed)
{
	MemoryContext old = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	char	   *message = NULL;

	*passed = false;
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(old);
	PG_TRY();
	{
		char	   *actual;

		run_user_query(sql);
		if (!strcmp(type, "expected_rows"))
			actual = psprintf(UINT64_FORMAT, (uint64) SPI_processed);
		else if (!strcmp(type, "expected_columns"))
			actual = psprintf("%d", SPI_tuptable ? SPI_tuptable->tupdesc->natts : 0);
		else if (!strcmp(type, "expected_single_value"))
		{
			if (SPI_processed == 0)
				message = "expected_single_value expects exactly one cell. Received 0 rows";
			else if (SPI_tuptable->tupdesc->natts != 1)
				message = "expected_single_value expects exactly one cell. Received multiple columns";
			else if (SPI_processed > 1)
				message = "expected_single_value expects exactly one cell. Received multiple rows";
			actual = message ? NULL : SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
		}
		else
		{
			message = psprintf("%s is not a valid assertion type", type);
			actual = NULL;
		}
		if (!message)
			message = compare_assertion(type, comparator, expected, actual, passed);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(old);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(old);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(old);
		CurrentResourceOwner = owner;
		message = psprintf("Query error: %s", edata->message);
		*passed = false;
	}
	PG_END_TRY();
	return message;
}

Datum
dolt_test_run(PG_FUNCTION_ARGS)
{
	char	   *arg = PG_NARGS() > 0 && !PG_ARGISNULL(0) ? text_to_cstring(PG_GETARG_TEXT_PP(0)) : NULL;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			nest,
				n,
				i;
	char	  **names,
			  **groups,
			  **queries,
			  **types,
			  **comparators,
			  **expecteds;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	nest = begin_api();
	if (arg && (!*arg || !strcmp(arg, "*")))
		arg = NULL;
	run_context("SELECT test_name,test_group,test_query,assertion_type,assertion_comparator,assertion_value FROM dolt.tests WHERE $1::text IS NULL OR test_name=$1 OR test_group=$1 ORDER BY test_group NULLS FIRST,test_name COLLATE \"C\"", 1, VC_TYPES(TEXTOID), VC_ARGS(arg ? CStringGetTextDatum(arg) : (Datum) 0), arg ? NULL : "n", true);
	n = SPI_processed;
	if (n == 0 && arg)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("could not find tests for argument: %s", arg)));
	names = palloc(sizeof(char *) * Max(n, 1));
	groups = palloc(sizeof(char *) * Max(n, 1));
	queries = palloc(sizeof(char *) * Max(n, 1));
	types = palloc(sizeof(char *) * Max(n, 1));
	comparators = palloc(sizeof(char *) * Max(n, 1));
	expecteds = palloc(sizeof(char *) * Max(n, 1));
	for (i = 0; i < n; i++)
	{
		names[i] = strcell(i, 1);
		groups[i] = strcell(i, 2);
		queries[i] = strcell(i, 3);
		types[i] = strcell(i, 4);
		comparators[i] = strcell(i, 5);
		expecteds[i] = strcell(i, 6);
	}
	for (i = 0; i < n; i++)
	{
		bool		passed;
		char	   *message = run_test_query(queries[i], types[i], comparators[i], expecteds[i], &passed);
		Datum		values[5];
		bool		nulls[5] = {false, false, false, false, false};

		values[0] = CStringGetTextDatum(names[i]);
		values[1] = CStringGetTextDatum(groups[i] ? groups[i] : "");
		values[2] = CStringGetTextDatum(queries[i]);
		values[3] = CStringGetTextDatum(passed ? "PASS" : "FAIL");
		values[4] = CStringGetTextDatum(message);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	end_api(nest);
	return (Datum) 0;
}

Datum
dolt_test_run_noargs(PG_FUNCTION_ARGS)
{
	return dolt_test_run(fcinfo);
}

/* Describe this engine's format, not Dolt's incompatible NBS serialization. */
Datum
dolt_storage_format(PG_FUNCTION_ARGS)
{
 PG_RETURN_TEXT_P(cstring_to_text("PROLLY ( " PL_NODE_FORMAT " )"));
}
