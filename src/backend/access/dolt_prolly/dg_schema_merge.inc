/*
 * Three-way merge of table definitions, and of rows across them.
 *
 * As in Dolt, a table changed on both sides merges column by column, but
 * columns, constraints and indexes are matched by name, as every schema
 * object is: a rename merges as a drop and an add. Each is taken from the side
 * that changed it; one changed differently on both sides, or dropped on one
 * side and changed on the other, is a schema conflict, recorded for
 * dolt_schema_conflicts. Rows of each side are laid out again in the merged
 * definition before they merge: a column one side added is filled with its
 * default in the other side's new rows, and a column whose type one side
 * changed has the other side's values converted, as ALTER TABLE would.
 */

/* One table definition item, keyed by name: a column or a constraint. */
typedef struct DefinitionItem
{
	char	   *name;
	char	   *json;
} DefinitionItem;

static List *
definition_items(const char *definition, const char *field)
{
	List	   *items = NIL;

	if (!definition)
		return NIL;
	run("SELECT CASE WHEN jsonb_typeof(v)='array' THEN v->>0 ELSE v->>'name' END,v::text FROM jsonb_array_elements(coalesce($1::jsonb->$2,'[]')) WITH ORDINALITY a(v,ord) ORDER BY ord", 2,
		VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(field)), NULL, true);
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		DefinitionItem *item = palloc(sizeof(DefinitionItem));

		item->name = strcell(i, 1);
		item->json = strcell(i, 2);
		items = lappend(items, item);
	}
	return items;
}

static char *
definition_item(List *items, const char *name)
{
	foreach_ptr(DefinitionItem, item, items)
		if (!strcmp(item->name, name))
		return item->json;
	return NULL;
}

static void
merge_describe(StringInfo description, const char *fmt,...) pg_attribute_printf(2, 3);

static void
merge_describe(StringInfo description, const char *fmt,...)
{
	va_list		args;

	if (description->len)
		appendStringInfoChar(description, '\n');
	for (;;)
	{
		int			needed;

		va_start(args, fmt);
		needed = appendStringInfoVA(description, fmt, args);
		va_end(args);
		if (needed == 0)
			break;
		enlargeStringInfo(description, needed);
	}
}

/* A column's type, typmod and collation, which change together. */
static char *
column_type_unit(const char *column)
{
	if (!column)
		return NULL;
	run("SELECT jsonb_build_array($1::jsonb->'type',$1::jsonb->'typmod',$1::jsonb->'collation')::text", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(column)), NULL, true);
	return strcell(0, 1);
}

/*
 * Merge the columns or constraints, |field|, of three definitions: ours in
 * their order, then those theirs added. Returns the merged JSON array, or
 * NULL after describing each conflict.
 */
static char *
merge_definition_items(const char *base, const char *ours, const char *theirs, const char *field,
					   StringInfo description)
{
	List	   *b = definition_items(base, field),
			   *o = definition_items(ours, field),
			   *t = definition_items(theirs, field),
			   *names = NIL;
	bool		columns = !strcmp(field, "columns");
	bool		conflict = false;
	StringInfoData result;

	foreach_ptr(DefinitionItem, item, o)
		names = lappend(names, item->name);
	foreach_ptr(DefinitionItem, item, t)
		if (!definition_item(o, item->name))
		names = lappend(names, item->name);
	initStringInfo(&result);
	appendStringInfoChar(&result, '[');
	foreach_ptr(char, name, names)
	{
		char	   *bv = definition_item(b, name),
				   *ov = definition_item(o, name),
				   *tv = definition_item(t, name),
				   *chosen = NULL;

		if (json_equal(ov, tv) || json_equal(bv, tv))
			chosen = ov;
		else if (json_equal(bv, ov))
			chosen = tv;
		else if (!ov || !tv)
		{
			merge_describe(description, "%s '%s' was deleted in %s but modified in %s",
						   columns ? "column" : "constraint", name, ov ? "theirs" : "ours", ov ? "ours" : "theirs");
			conflict = true;
		}
		else if (columns && bv &&
				 !json_equal(column_type_unit(ov), column_type_unit(tv)) &&
				 !json_equal(column_type_unit(bv), column_type_unit(ov)) &&
				 !json_equal(column_type_unit(bv), column_type_unit(tv)))
		{
			merge_describe(description, "incompatible column types for column '%s'", name);
			conflict = true;
		}
		else if (!columns || !bv || !merge_json_object(bv, ov, tv, &chosen))
		{
			if (columns)
				merge_describe(description, "different column definitions for our column %s and their column %s", name, name);
			else
				merge_describe(description, "two constraints with the name '%s' but different definitions", name);
			conflict = true;
		}
		if (!chosen)
			continue;
		if (result.len > 1)
			appendStringInfoChar(&result, ',');
		appendStringInfoString(&result, chosen);
	}
	appendStringInfoChar(&result, ']');
	return conflict ? NULL : result.data;
}

static bool
constraint_columns_walker(Node *node, List **columns)
{
	if (!node)
		return false;
	if (IsA(node, ColumnRef))
	{
		Node	   *last = llast(((ColumnRef *) node)->fields);

		if (IsA(last, String))
			*columns = lappend(*columns, strVal(last));
		return false;
	}
	return raw_expression_tree_walker(node, constraint_columns_walker, columns);
}

/* The columns of its table that constraint |sql| names. */
static List *
constraint_columns(const char *sql)
{
	List	   *parsed = raw_parser(psprintf("ALTER TABLE pg_catalog.dg_merge ADD CONSTRAINT dg_merge %s", sql), RAW_PARSE_DEFAULT);
	AlterTableCmd *cmd = linitial_node(AlterTableCmd, castNode(AlterTableStmt, linitial_node(RawStmt, parsed)->stmt)->cmds);
	Constraint *constraint = castNode(Constraint, cmd->def);
	List	   *columns = NIL;

	foreach_node(String, key, constraint->keys)
		columns = lappend(columns, strVal(key));
	foreach_node(String, key, constraint->fk_attrs)
		columns = lappend(columns, strVal(key));
	(void) constraint_columns_walker(constraint->raw_expr, &columns);
	return columns;
}

/*
 * The merged definition of table |name| from its definitions |base| (NULL if
 * both sides added it), |ours| and |theirs|, with no indexes listed yet, or
 * NULL after describing each conflict.
 */
static char *
merge_table_definition(const char *name, const char *base, const char *ours, const char *theirs,
					   StringInfo description)
{
	char	   *columns = merge_definition_items(base, ours, theirs, "columns", description),
			   *constraints = merge_definition_items(base, ours, theirs, "constraints", description),
			   *properties[3],
			   *merged;
	const char *definitions[3] = {base, ours, theirs};

	for (int i = 0; i < 3; i++)
	{
		properties[i] = NULL;
		if (definitions[i])
		{
			run("SELECT ($1::jsonb-'columns'-'constraints'-'indexes')::text", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definitions[i])), NULL, true);
			properties[i] = strcell(0, 1);
		}
	}
	if (!merge_json_object(properties[0], properties[1], properties[2], &merged))
	{
		merge_describe(description, "table %s was changed differently on both sides", name);
		return NULL;
	}
	if (!columns || !constraints)
		return NULL;

	/* A constraint may not outlive the columns it names. */
	{
		List	   *kept = definition_items(psprintf("{\"columns\":%s}", columns), "columns");

		run("SELECT v->>0,v->>1 FROM jsonb_array_elements($1::jsonb) v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(constraints)), NULL, true);
		{
			uint64		n = SPI_processed;
			char	  **names = palloc_array(char *, Max(n, 1));
			char	  **sql = palloc_array(char *, Max(n, 1));

			for (uint64 i = 0; i < n; i++)
			{
				names[i] = strcell(i, 1);
				sql[i] = strcell(i, 2);
			}
			for (uint64 i = 0; i < n; i++)
				foreach_ptr(char, column, constraint_columns(sql[i]))
					if (!definition_item(kept, column))
				{
					merge_describe(description, "constraint '%s' references a column that will be deleted after merge", names[i]);
					return NULL;
				}
		}

		/* As Dolt: a new NOT NULL column without a default cannot fill the other side's rows. */
		foreach_ptr(DefinitionItem, column, kept)
		{
			bool		added = !definition_item(definition_items(ours, "columns"), column->name) ||
				!definition_item(definition_items(theirs, "columns"), column->name);

			if (!added)
				continue;
			run("SELECT ($1::jsonb->>'notnull')::boolean AND $1::jsonb->'default' IS NULL AND $1::jsonb->'identity' IS NULL AND $1::jsonb->'generated' IS NULL", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(column->json)), NULL, true);
			if (DatumGetBool(cell(0, 1)))
				ereport(ERROR,
						(errcode(ERRCODE_NOT_NULL_VIOLATION),
						 errmsg("Unable to merge new column %s in table %s because it is not-nullable and has no default value, so existing rows can't be updated with a value for this column",
								column->name, name)));
		}
	}
	run("SELECT ($1::jsonb || jsonb_build_object('columns',$2::jsonb,'constraints',(SELECT coalesce(jsonb_agg(v ORDER BY (v->>0) COLLATE \"C\"),'[]'::jsonb) FROM jsonb_array_elements($3::jsonb) v),'indexes','[]'::jsonb))::text", 3,
		VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(merged ? merged : "{}"), CStringGetTextDatum(columns), CStringGetTextDatum(constraints)), NULL, true);
	return strcell(0, 1);
}

/* A table definition as CREATE TABLE, for dolt_schema_conflicts. */
static char *
definition_statement(const char *name, const char *definition)
{
	StringInfoData out;
	uint64		n;

	if (!definition)
		return pstrdup("<deleted>");
	initStringInfo(&out);
	appendStringInfo(&out, "CREATE TABLE %s (", name);
	run("SELECT format('%I %s%s%s',v->>'name',CASE WHEN v->>'type' LIKE '%.%' THEN v->>'type' ELSE format_type((SELECT t.oid FROM pg_type t WHERE t.typname=v->>'type' AND t.typnamespace='pg_catalog'::regnamespace),(v->>'typmod')::int) END,CASE WHEN (v->>'notnull')::boolean THEN ' NOT NULL' ELSE '' END,coalesce(' DEFAULT '||(v->>'default'),'')) FROM jsonb_array_elements($1::jsonb->'columns') WITH ORDINALITY a(v,ord) ORDER BY ord", 1,
		VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	n = SPI_processed;
	for (uint64 i = 0; i < n; i++)
		appendStringInfo(&out, "%s\n  %s", i ? "," : "", strcell(i, 1));
	run("SELECT format('CONSTRAINT %I %s',v->>0,v->>1) FROM jsonb_array_elements($1::jsonb->'constraints') v WHERE v->>1 NOT LIKE 'NOT NULL %' ORDER BY (v->>0) COLLATE \"C\"", 1,
		VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	for (uint64 i = 0; i < SPI_processed; i++)
		appendStringInfo(&out, ",\n  %s", strcell(i, 1));
	appendStringInfoString(&out, "\n);");
	run("SELECT d||';' FROM jsonb_array_elements_text($1::jsonb->'indexes') d ORDER BY d COLLATE \"C\"", 1,
		VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	for (uint64 i = 0; i < SPI_processed; i++)
		appendStringInfo(&out, "\n%s", strcell(i, 1));
	return out.data;
}

/* Record a schema conflict for dolt_schema_conflicts. */
static void
record_schema_conflict(StringInfo conflicts, const char *kind, const char *name,
					   const char *base, const char *ours, const char *theirs, const char *description)
{
	if (conflicts->len)
		appendStringInfoChar(conflicts, ',');
	appendStringInfoString(conflicts, "{\"table\":");
	escape_json(conflicts, name);
	appendStringInfoString(conflicts, ",\"kind\":");
	escape_json(conflicts, kind);
	appendStringInfoString(conflicts, ",\"base_schema\":");
	escape_json(conflicts, base);
	appendStringInfoString(conflicts, ",\"our_schema\":");
	escape_json(conflicts, ours);
	appendStringInfoString(conflicts, ",\"their_schema\":");
	escape_json(conflicts, theirs);
	appendStringInfoString(conflicts, ",\"description\":");
	escape_json(conflicts, description);
	appendStringInfoChar(conflicts, '}');
}

/* A table member with |definition| and no rows yet. */
static bytea *
merged_table_member(const char *name, const char *definition)
{
	DGState    *empty = palloc0(sizeof(*empty));

	empty->store = pl_store_create();
	if (!empty->store)
		ereport(ERROR, (errmsg("out of memory creating prolly store")));
	empty->cleanup.func = canonical_empty_cleanup;
	empty->cleanup.arg = empty;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &empty->cleanup);
	dg_check(pl_build(empty->store, NULL, 0, &empty->tree));
	return canonical_empty_member("table", name, definition, &empty->tree);
}

/* ---- Rows across definitions ---- */

/* The type a definition's column names: built-in by name, versioned qualified. */
static Oid
merge_column_type(const char *name)
{
	Size		len = strlen(name);

	if (len > 2 && !strcmp(name + len - 2, "[]"))
	{
		Oid			array = get_array_type(merge_column_type(pnstrdup(name, len - 2)));

		if (!OidIsValid(array))
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("type %s does not exist", name)));
		return array;
	}
	if (strchr(name, '.'))
		return typenameTypeId(NULL, makeTypeNameFromNameList(stringToQualifiedNameList(name, NULL)));
	return typenameTypeId(NULL, makeTypeNameFromNameList(list_make2(makeString("pg_catalog"), makeString(pstrdup(name)))));
}

/* The columns of |base|, a table definition, that merged |rel| lacks. */
static List *
merge_dropped_columns(Relation rel, const char *base)
{
	List	   *dropped = NIL;

	foreach_ptr(DefinitionItem, column, definition_items(base, "columns"))
	{
		bool		kept = false;

		for (int k = 0; k < RelationGetDescr(rel)->natts; k++)
			kept |= !strcmp(NameStr(TupleDescAttr(RelationGetDescr(rel), k)->attname), column->name);
		if (!kept)
			dropped = lappend(dropped, column->name);
	}
	return dropped;
}

typedef struct RowRemap
{
	DGConversion **conversions;
} RowRemap;

static PLSlice
remap_field(int field, PLSlice bytes, void *arg)
{
	RowRemap   *remap = arg;

	return remap->conversions[field] ? dg_conversion_apply(remap->conversions[field], bytes) : bytes;
}

/*
 * The rows of |content|, stored under |definition|, laid out under the
 * definition of |rel|: its columns by name, converted to its types, followed
 * by the columns named |dropped|, which the merge drops, so that a side's
 * change to one can conflict. Sets |lacks|[i] for the columns of |rel| that
 * |definition| lacks.
 */
static PLTree
remap_rows(Relation rel, PLTree content, const char *definition, List *dropped, bool *lacks)
{
	TupleDesc	desc = RelationGetDescr(rel);
	int			width = desc->natts + list_length(dropped);
	int		   *mapping = palloc_array(int, Max(width, 1));
	RowRemap	remap = {.conversions = palloc0_array(DGConversion *, Max(width, 1))};
	DGState		state = {0};
	char	  **names,
			  **types;
	int		   *typmods;
	int			oldcount;
	CatalogView *previous;
	PLTree		result;

	run("SELECT v->>'name',v->>'type',(v->>'typmod')::int FROM jsonb_array_elements($1::jsonb->'columns') WITH ORDINALITY a(v,ord) ORDER BY ord", 1,
		VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	oldcount = SPI_processed;
	names = palloc_array(char *, Max(oldcount, 1));
	types = palloc_array(char *, Max(oldcount, 1));
	typmods = palloc_array(int, Max(oldcount, 1));
	for (int i = 0; i < oldcount; i++)
	{
		names[i] = strcell(i, 1);
		types[i] = strcell(i, 2);
		typmods[i] = DatumGetInt32(cell(i, 3));
	}
	previous = CatalogViewSwitch(rel->rd_catalogview);
	PG_TRY();
	{
		for (int j = 0; j < desc->natts; j++)
		{
			Form_pg_attribute attr = TupleDescAttr(desc, j);

			mapping[j] = oldcount;
			lacks[j] = true;
			for (int i = 0; i < oldcount; i++)
			{
				Oid			type;

				if (strcmp(names[i], NameStr(attr->attname)))
					continue;
				mapping[j] = i;
				lacks[j] = false;
				type = merge_column_type(types[i]);
				if (type != attr->atttypid || typmods[i] != attr->atttypmod)
					remap.conversions[j] = dg_conversion_begin(type, attr->atttypid, attr->atttypmod);
			}
		}
		foreach_ptr(char, column, dropped)
		{
			int			j = desc->natts + foreach_current_index(column);

			mapping[j] = oldcount;
			for (int i = 0; i < oldcount; i++)
				if (!strcmp(names[i], column))
					mapping[j] = i;
		}
		state.store = content.store;
		state.tree = content;
		result = alter_rows(&state, oldcount, width, mapping, remap_field, &remap);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

/*
 * Row |row| of |rel| with the columns in |fill| set to their defaults, or
 * computed, as INSERT would for a row that does not give them.
 */
static PLSlice
fill_row(Relation rel, PLSlice row, const bool *fill)
{
	TupleDesc	desc = RelationGetDescr(rel);
	CatalogView *previous = CatalogViewSwitch(rel->rd_catalogview);
	PLSlice		result;

	PG_TRY();
	{
		EState	   *estate = CreateExecutorState();
		ExprContext *econtext = GetPerTupleExprContext(estate);
		TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &TTSOpsHeapTuple);
		TupleTableSlot *filled = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);

		ExecStoreHeapTuple(dg_decode_descriptor(desc, row), slot, true);
		slot_getallattrs(slot);
		ExecClearTuple(filled);
		memcpy(filled->tts_values, slot->tts_values, sizeof(Datum) * desc->natts);
		memcpy(filled->tts_isnull, slot->tts_isnull, sizeof(bool) * desc->natts);
		econtext->ecxt_scantuple = slot;

		/* Plain defaults first: a generated column may use them. */
		for (int pass = 0; pass < 2; pass++)
			for (int k = 0; k < desc->natts; k++)
			{
				Form_pg_attribute attr = TupleDescAttr(desc, k);
				Node	   *expr;

				if (!fill[k] || (pass == 0) != (attr->attgenerated == '\0'))
					continue;
				expr = attr->attgenerated ? build_generation_expression(rel, k + 1) : build_column_default(rel, k + 1);
				if (!expr)
				{
					filled->tts_isnull[k] = true;
					continue;
				}
				filled->tts_values[k] = ExecEvalExprSwitchContext(ExecPrepareExpr((Expr *) expr, estate), econtext, &filled->tts_isnull[k]);
				if (pass == 0)
				{
					slot->tts_values[k] = filled->tts_values[k];
					slot->tts_isnull[k] = filled->tts_isnull[k];
				}
			}
		ExecStoreVirtualTuple(filled);
		result = dg_encode(rel, filled);
		ExecDropSingleTupleTableSlot(slot);
		ExecDropSingleTupleTableSlot(filled);
		FreeExecutorState(estate);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

/*
 * Rows |content| of keyless |rel| with the columns in |fill| set to their
 * defaults. A keyless row is its whole value, so every row of a side lacking
 * a column takes the default, as the side that added it filled its rows.
 */
static PLTree
fill_rows(Relation rel, PLTree content, const bool *fill)
{
	DGRows		rows = dg_rows(&content);
	PLEntry    *entries = palloc_array(PLEntry, Max(rows.count, 1));
	size_t		used = 0;
	bool		any = false;
	PLTree		result;

	for (int k = 0; k < RelationGetDescr(rel)->natts; k++)
		any |= fill[k];
	if (!any)
		return content;
	for (size_t i = 0; i < rows.count; i++)
	{
		PLSlice		row = fill_row(rel, DG_SLICE(rows.items[i].value.data + 8, rows.items[i].value.len - 8), fill);
		PLHash		hash = dg_rowhash(row);
		uint8	   *key = palloc(PL_HASH_SIZE),
				   *value = palloc(row.len + 8);

		memcpy(key, hash.bytes, PL_HASH_SIZE);
		memcpy(value, rows.items[i].value.data, 8);
		memcpy(value + 8, row.data, row.len);
		entries[i] = DG_ENTRY(DG_SLICE(key, PL_HASH_SIZE), DG_SLICE(value, row.len + 8));
	}
	qsort(entries, rows.count, sizeof(*entries), entry_compare);
	for (size_t i = 0; i < rows.count; i++)
	{
		if (used && !pl_compare(entries[used - 1].key, entries[i].key))
			pl_write64((uint8 *) entries[used - 1].value.data,
					   pl_read64(entries[used - 1].value.data) + pl_read64(entries[i].value.data));
		else
			entries[used++] = entries[i];
	}
	dg_check(pl_build(content.store, entries, used, &result));
	return result;
}

/*
 * Rows of a merged definition that violate its NOT NULL and CHECK
 * constraints, as a side without them could have written: recorded as Dolt
 * "not null" and "check constraint" violations. A NOT NULL violation's row is
 * left out of the table, as Dolt leaves it out.
 */
static PLTree
check_violations(Relation rel, PLTree content, const char *name, StringInfo violations)
{
	TupleDesc	desc = RelationGetDescr(rel);
	DGRows		rows = dg_rows(&content);
	CatalogView *previous = CatalogViewSwitch(rel->rd_catalogview);
	PLEntry    *kept = palloc_array(PLEntry, Max(rows.count, 1));
	size_t		used = 0;
	PLTree		result = content;

	PG_TRY();
	{
		EState	   *estate = CreateExecutorState();
		ExprContext *econtext = GetPerTupleExprContext(estate);
		TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &TTSOpsHeapTuple);
		TupleConstr *constr = desc->constr;
		ExprState **checks = palloc0_array(ExprState *, Max(constr ? constr->num_check : 0, 1));

		for (int c = 0; constr && c < constr->num_check; c++)
			checks[c] = ExecPrepareExpr((Expr *) stringToNode(constr->check[c].ccbin), estate);
		for (size_t i = 0; i < rows.count; i++)
		{
			PLSlice		row = {rows.items[i].value.data + 8, rows.items[i].value.len - 8};
			bool		keep = true;

			ExecStoreHeapTuple(dg_decode_descriptor(desc, row), slot, true);
			slot_getallattrs(slot);
			econtext->ecxt_scantuple = slot;
			for (int k = 0; k < desc->natts; k++)
			{
				Form_pg_attribute attr = TupleDescAttr(desc, k);

				if (!attr->attnotnull || !slot->tts_isnull[k])
					continue;
				if (violations->len)
					appendStringInfoChar(violations, ',');
				appendStringInfoString(violations, "{\"table\":");
				escape_json(violations, name);
				appendStringInfoString(violations, ",\"type\":\"not null\",\"row\":");
				json_bytes(violations, DG_VALUE(true, row));
				appendStringInfoString(violations, ",\"info\":{\"Columns\":[");
				escape_json(violations, NameStr(attr->attname));
				appendStringInfoString(violations, "],\"Name\":\"\"}}");
				keep = false;
			}
			for (int c = 0; constr && c < constr->num_check; c++)
			{
				if (!constr->check[c].ccenforced || ExecCheck(checks[c], econtext))
					continue;
				if (violations->len)
					appendStringInfoChar(violations, ',');
				appendStringInfoString(violations, "{\"table\":");
				escape_json(violations, name);
				appendStringInfoString(violations, ",\"type\":\"check constraint\",\"row\":");
				json_bytes(violations, DG_VALUE(true, row));
				appendStringInfoString(violations, ",\"info\":{\"Columns\":[],\"Name\":");
				escape_json(violations, constr->check[c].ccname);
				appendStringInfoString(violations, "}}");
			}
			ResetExprContext(econtext);
			if (keep)
				kept[used++] = rows.items[i];
		}
		ExecDropSingleTupleTableSlot(slot);
		FreeExecutorState(estate);
		if (used < rows.count)
			dg_check(pl_build(content.store, kept, used, &result));
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}
