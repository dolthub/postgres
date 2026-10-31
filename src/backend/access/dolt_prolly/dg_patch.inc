/* dolt_patch and dolt_schema_diff: SQL statements that transform one revision
 * into another. See doc/doltgres/schema-tables.md. Included by dg_version.c
 * after dg_diff.inc. */

static char *
quoted_identifier(const char *name)
{
	return psprintf("\"%s\"", name);
}

/* Doltgres quoting: single-quoted strings escape ' and JSON documents escape "
 * with backslashes. Numbers and booleans are bare. */
static char *
patch_value(Oid type, Datum value, bool isnull)
{
	Oid			outfunc;
	bool		varlena;
	char	   *text;
	StringInfoData out;
	const char *p;

	if (isnull)
		return "NULL";
	getTypeOutputInfo(type, &outfunc, &varlena);
	text = OidOutputFunctionCall(outfunc, value);
	switch (type)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			return text;
		case BOOLOID:
			return DatumGetBool(value) ? "TRUE" : "FALSE";
		default:
			break;
	}
	initStringInfo(&out);
	appendStringInfoChar(&out, '\'');
	for (p = text; *p; p++)
	{
		if (*p == '\'' || ((type == JSONOID || type == JSONBOID) && *p == '"'))
			appendStringInfoChar(&out, '\\');
		appendStringInfoChar(&out, *p);
	}
	appendStringInfoChar(&out, '\'');
	return out.data;
}

static char *
column_type_name(const char *schema_json, const char *column)
{
	run("SELECT format_type((SELECT t.oid FROM pg_type t WHERE t.typname=c->>'type' AND t.typnamespace='pg_catalog'::regnamespace),(c->>'typmod')::int) FROM jsonb_array_elements($1::jsonb->'columns') c WHERE c->>'name'=$2", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(schema_json), CStringGetTextDatum(column)), NULL, true);
	return SPI_processed ? strcell(0, 1) : pstrdup("text");
}

/* CREATE TABLE in Doltgres's patch format. */
static char *
create_table_statement(DiffTable *t)
{
	StringInfoData out;
	int			i,
				n;
	char	   *table = strVal(lsecond(stringToQualifiedNameList(t->name, NULL)));

	initStringInfo(&out);
	appendStringInfo(&out, "CREATE TABLE %s (\n", quoted_identifier(table));
	run("SELECT c->>'name',format_type((SELECT t.oid FROM pg_type t WHERE t.typname=c->>'type' AND t.typnamespace='pg_catalog'::regnamespace),(c->>'typmod')::int),(c->>'notnull')::boolean FROM jsonb_array_elements($1::jsonb->'columns') WITH ORDINALITY AS x(c,ord) ORDER BY ord", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(t->schema)), NULL, true);
	n = SPI_processed;
	for (i = 0; i < n; i++)
	{
		char	   *name = strcell(i, 1),
				   *type = strcell(i, 2);
		bool		notnull = DatumGetBool(cell(i, 3));

		appendStringInfo(&out, "  %s %s%s", quoted_identifier(name), type, notnull ? " NOT NULL" : "");
		if (i + 1 < n || t->nkeys)
			appendStringInfoString(&out, ",\n");
		else
			appendStringInfoChar(&out, '\n');
	}
	if (t->nkeys)
	{
		appendStringInfoString(&out, "  PRIMARY KEY (");
		for (i = 0; i < t->nkeys; i++)
			appendStringInfo(&out, "%s%s", i ? ", " : "", quoted_identifier(NameStr(TupleDescAttr(t->desc, t->keys[i])->attname)));
		appendStringInfoString(&out, ")\n");
	}
	run("SELECT c->>1 FROM jsonb_array_elements($1::jsonb->'constraints') c WHERE c->>1 NOT LIKE 'PRIMARY KEY (%' AND c->>1 NOT LIKE 'NOT NULL %' ORDER BY (c->>0) COLLATE \"C\"", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(t->schema)), NULL, true);
	n = SPI_processed;
	for (i = 0; i < n; i++)
	{
		/* Trailing comma on the previous line, then the constraint. */
		if (out.len >= 2 && out.data[out.len - 1] == '\n')
		{
			out.data[out.len - 1] = ',';
			appendStringInfoChar(&out, '\n');
		}
		appendStringInfo(&out, "  %s\n", strcell(i, 1));
	}
	appendStringInfoString(&out, ");");
	return out.data;
}

/* ALTER TABLE statements for column additions, drops and type changes. */
static List *
alter_table_statements(DiffTable *from, DiffTable *to)
{
	List	   *result = NIL;
	int			i,
				j;
	char	   *table = quoted_identifier(strVal(lsecond(stringToQualifiedNameList(to->name, NULL))));

	for (i = 0; i < to->desc->natts; i++)
	{
		const char *name = NameStr(TupleDescAttr(to->desc, i)->attname);

		for (j = 0; j < from->desc->natts; j++)
			if (!strcmp(NameStr(TupleDescAttr(from->desc, j)->attname), name))
				break;
		if (j == from->desc->natts)
			result = lappend(result, psprintf("ALTER TABLE %s ADD COLUMN %s %s%s;", table, quoted_identifier(name), column_type_name(to->schema, name), TupleDescAttr(to->desc, i)->attnotnull ? " NOT NULL" : ""));
		else if (TupleDescAttr(from->desc, j)->atttypid != TupleDescAttr(to->desc, i)->atttypid || TupleDescAttr(from->desc, j)->atttypmod != TupleDescAttr(to->desc, i)->atttypmod)
			result = lappend(result, psprintf("ALTER TABLE %s ALTER COLUMN %s TYPE %s;", table, quoted_identifier(name), column_type_name(to->schema, name)));
	}
	for (j = 0; j < from->desc->natts; j++)
	{
		const char *name = NameStr(TupleDescAttr(from->desc, j)->attname);

		for (i = 0; i < to->desc->natts; i++)
			if (!strcmp(NameStr(TupleDescAttr(to->desc, i)->attname), name))
				break;
		if (i == to->desc->natts)
			result = lappend(result, psprintf("ALTER TABLE %s DROP COLUMN %s;", table, quoted_identifier(name)));
	}
	return result;
}

typedef struct PatchEmit
{
	List	   *statements;
	DiffTable  *from;
	DiffTable  *to;
	char	   *table;			/* quoted unqualified name */
} PatchEmit;

static char *
where_clause(DiffTable *t, Datum *values, bool *nulls)
{
	StringInfoData out;
	int			i;

	initStringInfo(&out);
	if (t->keyed)
		for (i = 0; i < t->nkeys; i++)
			appendStringInfo(&out, "%s%s=%s", i ? " AND " : "", quoted_identifier(NameStr(TupleDescAttr(t->desc, t->keys[i])->attname)), patch_value(TupleDescAttr(t->desc, t->keys[i])->atttypid, values[t->keys[i]], nulls[t->keys[i]]));
	else
		for (i = 0; i < t->desc->natts; i++)
			appendStringInfo(&out, "%s%s%s%s", i ? " AND " : "", quoted_identifier(NameStr(TupleDescAttr(t->desc, i)->attname)), nulls[i] ? " IS " : "=", patch_value(TupleDescAttr(t->desc, i)->atttypid, values[i], nulls[i]));
	return out.data;
}

static void
patch_row(DiffTable *from, DiffTable *to, DiffRow *row, void *arg)
{
	PatchEmit  *e = (PatchEmit *) arg;
	StringInfoData out;
	int			i;

	initStringInfo(&out);
	if (!strcmp(row->type, "added"))
	{
		Datum	   *v = palloc(sizeof(Datum) * to->desc->natts);
		bool	   *n = palloc(sizeof(bool) * to->desc->natts);

		decode_row(to, row->to, v, n);
		appendStringInfo(&out, "INSERT INTO %s (", e->table);
		for (i = 0; i < to->desc->natts; i++)
			appendStringInfo(&out, "%s%s", i ? "," : "", quoted_identifier(NameStr(TupleDescAttr(to->desc, i)->attname)));
		appendStringInfoString(&out, ") VALUES (");
		for (i = 0; i < to->desc->natts; i++)
			appendStringInfo(&out, "%s%s", i ? "," : "", patch_value(TupleDescAttr(to->desc, i)->atttypid, v[i], n[i]));
		appendStringInfoString(&out, ");");
	}
	else if (!strcmp(row->type, "removed"))
	{
		Datum	   *v = palloc(sizeof(Datum) * from->desc->natts);
		bool	   *n = palloc(sizeof(bool) * from->desc->natts);

		decode_row(from, row->from, v, n);
		appendStringInfo(&out, "DELETE FROM %s WHERE %s;", e->table, where_clause(from, v, n));
		if (!from->keyed)
			appendStringInfoString(&out, " -- one occurrence");
	}
	else
	{
		Datum	   *fv = palloc(sizeof(Datum) * from->desc->natts),
				   *tv = palloc(sizeof(Datum) * to->desc->natts);
		bool	   *fn = palloc(sizeof(bool) * from->desc->natts),
				   *tn = palloc(sizeof(bool) * to->desc->natts);
		bool		first = true;

		decode_row(from, row->from, fv, fn);
		decode_row(to, row->to, tv, tn);
		appendStringInfo(&out, "UPDATE %s SET ", e->table);
		for (i = 0; i < to->desc->natts; i++)
		{
			const char *name = NameStr(TupleDescAttr(to->desc, i)->attname);
			int			j;
			PLValue		a,
						b;

			for (j = 0; j < from->desc->natts; j++)
				if (!strcmp(NameStr(TupleDescAttr(from->desc, j)->attname), name))
					break;
			dg_check(pl_tuple_get(row->to.bytes, i, &b));
			if (j < from->desc->natts)
			{
				dg_check(pl_tuple_get(row->from.bytes, j, &a));
				if (equal_value(a, b))
					continue;
			}
			else if (tn[i])
				continue;		/* a new column still at its NULL default */
			appendStringInfo(&out, "%s%s=%s", first ? "" : ",", quoted_identifier(name), patch_value(TupleDescAttr(to->desc, i)->atttypid, tv[i], tn[i]));
			first = false;
		}
		if (first)
			return;				/* only dropped columns differed */
		appendStringInfo(&out, " WHERE %s;", where_clause(from, fv, fn));
	}
	e->statements = lappend(e->statements, out.data);
}

/* Statements for one table: schema first, then data. */
static void
table_patch(const char *from_rev, const char *to_rev, const char *name, List **schema_stmts, List **data_stmts)
{
	DiffTable  *a = diff_table_revision(from_rev, name),
			   *b = diff_table_revision(to_rev, name);
	PatchEmit	e = {NIL, a, b, NULL};

	*schema_stmts = NIL;
	*data_stmts = NIL;
	if (!a->present && !b->present)
		return;
	if (!a->present)
		*schema_stmts = list_make1(create_table_statement(b));
	else if (!b->present)
	{
		*schema_stmts = list_make1(psprintf("DROP TABLE %s;", quoted_identifier(strVal(lsecond(stringToQualifiedNameList(a->name, NULL))))));
		diff_table_close(a);
		diff_table_close(b);
		return;
	}
	else if (!same_schema_definition(a->schema, b->schema))
		*schema_stmts = alter_table_statements(a, b);
	e.table = quoted_identifier(strVal(lsecond(stringToQualifiedNameList(b->name, NULL))));
	if (a->present && b->present && (a->keyed != b->keyed || (a->keyed && !same_key_columns(a, b))))
		e.statements = NIL;		/* key changes have no row-level patch */
	else
		diff_rows(a, b, patch_row, &e);
	*data_stmts = e.statements;
	diff_table_close(a);
	diff_table_close(b);
}

static char *
revision_hash_label(const char *revision)
{
	if (!pg_strcasecmp(revision, "WORKING") || !pg_strcasecmp(revision, "STAGED"))
		return pstrdup(revision);
	return hash_format(resolve(revision));
}

Datum
dolt_patch(PG_FUNCTION_ARGS)
{
	char	   *from = text_to_cstring(PG_GETARG_TEXT_PP(0)),
			   *to = text_to_cstring(PG_GETARG_TEXT_PP(1)),
			   *table = optional_text_arg(fcinfo, 2);
	int			nest = begin_api();
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *fromjson = state_json(from),
			   *tojson = !strcmp(from, to) ? fromjson : state_json(to);
	char	   *fromhash = revision_hash_label(from),
			   *tohash = revision_hash_label(to);
	char	  **names;
	int			n,
				i;
	int64		order = 0;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	if (table)
	{
		names = palloc(sizeof(char *));
		names[0] = resolve_table_argument(table, fromjson, tojson, true);
		n = 1;
	}
	else
		names = changed_tables(fromjson, tojson, &n);
	for (i = 0; i < n; i++)
	{
		List	   *schema_stmts,
				   *data_stmts;
		ListCell   *lc;
		int			k;

		table_patch(from, to, names[i], &schema_stmts, &data_stmts);
		for (k = 0; k < 2; k++)
			foreach(lc, k == 0 ? schema_stmts : data_stmts)
			{
				Datum		values[6];
				bool		nulls[6] = {false, false, false, false, false, false};

				values[0] = DirectFunctionCall1(int8_numeric, Int64GetDatum(++order));
				values[1] = CStringGetTextDatum(fromhash);
				values[2] = CStringGetTextDatum(tohash);
				values[3] = CStringGetTextDatum(names[i]);
				values[4] = CStringGetTextDatum(k == 0 ? "schema" : "data");
				values[5] = CStringGetTextDatum((char *) lfirst(lc));
				tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
			}
	}
	end_api(nest);
	return (Datum) 0;
}

Datum
dolt_patch_table(PG_FUNCTION_ARGS)
{
	return dolt_patch(fcinfo);
}

Datum
dolt_schema_diff(PG_FUNCTION_ARGS)
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
		names[0] = resolve_table_argument(table, fromjson, tojson, true);
		n = 1;
	}
	else
		names = changed_tables(fromjson, tojson, &n);
	for (i = 0; i < n; i++)
	{
		DiffTable  *a = diff_table_schema(from, names[i]),
				   *b = diff_table_schema(to, names[i]);
		Datum		values[4];
		bool		nulls[4] = {false, false, false, false};

		if ((!a->present && !b->present) || (a->present && b->present && same_schema_definition(a->schema, b->schema)))
			continue;
		values[0] = CStringGetTextDatum(a->present ? a->name : "");
		values[1] = CStringGetTextDatum(b->present ? b->name : "");
		values[2] = CStringGetTextDatum(a->present ? create_table_statement(a) : "");
		values[3] = CStringGetTextDatum(b->present ? create_table_statement(b) : "");
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	end_api(nest);
	return (Datum) 0;
}

Datum
dolt_schema_diff_table(PG_FUNCTION_ARGS)
{
	return dolt_schema_diff(fcinfo);
}

Datum
dolt_schema_diff_range(PG_FUNCTION_ARGS)
{
 return dolt_schema_diff(fcinfo);
}
