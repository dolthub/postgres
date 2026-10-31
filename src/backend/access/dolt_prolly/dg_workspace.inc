/* Workspace commands (reset, clean, rm, table checkout, named add), branch
 * and tag management, and commit-graph helpers. See doc/doltgres/workspace-commands.md.
 * Included by dg_version.c. */

/* ---- Database state composition ----
 *
 * A database state object lists member objects (namespaces, tables, indexes).
 * compose_database builds a new state from |base|, replacing the members of
 * the selected tables (and their indexes) with the versions found in |source|
 * (or removing them when |source| lacks them), and adding namespaces that the
 * selected tables need. */

typedef struct Member
{
	bytea	   *hash;
	char	   *kind;
	char	   *name;			/* qualified table/index name or namespace
								 * name */
	char	   *payload;
}			Member;

static Member *
database_members(bytea *db, int *count)
{
	Member	   *result;
	int			i;

	if (!db)
	{
		*count = 0;
		return NULL;
	}
	{
		DGMember   *listed = sorted_members(db, count);

		result = palloc0(sizeof(Member) * Max(*count, 1));
		for (i = 0; i < *count; i++)
		{
			result[i].hash = listed[i].hash;
			result[i].kind = (char *) listed[i].kind;
			result[i].name = (char *) listed[i].name;
		}
	}
	for (i = 0; i < *count; i++)
	{
		result[i].payload = metadata(result[i].hash, result[i].kind);
		if (!result[i].name)
			result[i].name = json_value(result[i].payload, "name");
	}
	return result;
}

/* Index member names that belong to a table, from its schema definitions. */
static List *
table_index_names(const char *schema_json, const char *table)
{
	List	   *result = NIL;
	char	   *ns = strVal(linitial(stringToQualifiedNameList(table, NULL)));
	int			i;

	run("SELECT d FROM jsonb_array_elements_text($1::jsonb->'indexes') d", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(schema_json)), NULL, true);
	for (i = 0; i < SPI_processed; i++)
	{
		List	   *parsed = raw_parser(strcell(i, 1), RAW_PARSE_DEFAULT);
		Node	   *node = linitial_node(RawStmt, parsed)->stmt;
		IndexStmt  *index;

		if (list_length(parsed) != 1 || !IsA(node, IndexStmt))
			elog(ERROR, "invalid canonical index definition");
		index = (IndexStmt *) node;
		result = lappend(result, dg_qualified_name(ns, index->idxname));
	}
	return result;
}

static bool
name_selected(List *names, const char *name)
{
	ListCell   *lc;

	foreach(lc, names)
		if (!strcmp((char *) lfirst(lc), name))
		return true;
	return false;
}

/* Expand selected tables to their index members in |members|. */
static List *
selected_objects(Member * members, int count, List *tables)
{
	List	   *result = list_copy(tables);
	int			i;

	for (i = 0; i < count; i++)
		if (!strcmp(members[i].kind, "table") && name_selected(tables, members[i].name))
			result = list_concat(result, table_index_names(metadata(child(members[i].hash, 1), "schema"), members[i].name));
	for (i = 0; i < count; i++)
		if (!strcmp(members[i].kind, "trigger") &&
			name_selected(tables, json_value(metadata(child(members[i].hash, 1), "schema"), "table")))
			result = lappend(result, members[i].name);
	for (i = 0; i < count; i++)
		if (!strcmp(members[i].kind, "sequence"))
		{
			char *owned = json_value(metadata(child(members[i].hash, 1), "schema"), "owned_table");

			if (owned && name_selected(tables, owned))
				result = lappend(result, members[i].name);
		}
	return result;
}

static bytea *
compose_database(bytea *base, bytea *source, List *tables)
{
	int			nbase,
				nsource,
				i,
				used = 0;
	Member	   *bm = database_members(base, &nbase),
			   *sm = database_members(source, &nsource);
	List	   *drop = selected_objects(bm, nbase, tables),
			   *take = selected_objects(sm, nsource, tables);
	Datum	   *hashes = palloc(sizeof(Datum) * (nbase + nsource + 1));
	StringInfoData json;
	ListCell   *lc;

	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGDB1\",\"members\":[");
	for (i = 0; i < nbase; i++)
	{
		if (strcmp(bm[i].kind, "namespace") && (name_selected(drop, bm[i].name) || name_selected(take, bm[i].name)))
			continue;
		if (used)
			appendStringInfoChar(&json, ',');
		if (!strcmp(bm[i].kind, "namespace"))
			appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", bm[i].payload);
		else
			appendStringInfoString(&json, bm[i].payload);
		hashes[used++] = PointerGetDatum(bm[i].hash);
	}
	for (i = 0; i < nsource; i++)
	{
		bool		wanted = false;

		if (!strcmp(sm[i].kind, "namespace"))
		{
			/* A selected table's schema must exist in the result. */
			int			j;

			foreach(lc, take)
			{
				char *ns;

				const char *selected = lfirst(lc);
				Member *special = NULL;

				for (int k = 0; k < nsource; k++)
					if ((!strcmp(sm[k].kind, "cast") || !strcmp(sm[k].kind, "operator")) && !strcmp(sm[k].name, selected))
						special = &sm[k];
				if (special && !strcmp(special->kind, "cast"))
					continue; /* Casts do not own a namespace. */
				if (special)
					ns = json_value(json_value(metadata(child(special->hash, 1), "schema"), "row"), "oprnamespace");
				else
				{
					run("SELECT (parse_ident($1,false))[1]", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(selected)), NULL, true);
					ns = strcell(0, 1);
				}

				if (!strcmp(ns, sm[i].name))
					wanted = true;
			}
			for (j = 0; wanted && j < nbase; j++)
				if (!strcmp(bm[j].kind, "namespace") && !strcmp(bm[j].name, sm[i].name))
					wanted = false;
		}
		else
			wanted = name_selected(take, sm[i].name);
		if (!wanted)
			continue;
		if (used)
			appendStringInfoChar(&json, ',');
		if (!strcmp(sm[i].kind, "namespace"))
			appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", sm[i].payload);
		else
			appendStringInfoString(&json, sm[i].payload);
		hashes[used++] = PointerGetDatum(sm[i].hash);
	}
	appendStringInfoString(&json, "]}");
	return canonical_database(json.data, hashes, used);
}

/* ---- Schema merge ----
 *
 * A three-way merge of database states, object by object: a namespace,
 * routine, type, view or trigger is keyed by kind and name, and a table
 * together with its indexes, which its definition lists. An object changed
 * (added, altered or dropped) on one side only takes that side. A table
 * changed on both sides merges its definition column by column (see
 * dg_schema_merge.inc) and is returned in |row_merges|, for its rows to be
 * merged. Anything else changed on both sides, and a table whose definitions
 * do not merge, is a schema conflict: it keeps our side, and is recorded in
 * |schema_conflicts|. */

static Member *
merge_member(Member * members, int count, const char *kind, const char *name)
{
	for (int i = 0; i < count; i++)
		if (!strcmp(members[i].kind, kind) && !strcmp(members[i].name, name))
			return &members[i];
	return NULL;
}

static bool
merge_same(Member * a, Member * b)
{
	return (!a && !b) || (a && b &&
		(same(a->hash, b->hash) ||
		 (!strcmp(a->kind, "sequence") && !strcmp(b->kind, "sequence") &&
		  json_same(a->payload, b->payload))));
}

/* Carry unrelated staged/working edits across a committed merge. Compare
 * canonical members, including namespaces and table-owned objects, so a schema
 * change cannot silently overwrite a dirty definition or content root. */
static bytea *
carry_workspace_changes(bytea *base, bytea *local, bytea *merged)
{
	int nb, nl, nm, used = 0;
	Member *bm = database_members(base, &nb);
	Member *lm = database_members(local, &nl);
	Member *mm = database_members(merged, &nm);
	Datum *hashes = palloc(sizeof(Datum) * (nb + nl + nm + 1));
	StringInfoData json;

	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGDB1\",\"members\":[");
	for (int i = 0; i < nb + nl + nm; i++)
	{
		Member *key = i < nb ? &bm[i] : i < nb + nl ? &lm[i-nb] : &mm[i-nb-nl];
		Member *b = merge_member(bm, nb, key->kind, key->name);
		Member *l = merge_member(lm, nl, key->kind, key->name);
		Member *m = merge_member(mm, nm, key->kind, key->name);
		Member *chosen;

		if ((i >= nb && b) || (i >= nb + nl && l))
			continue;
		if (!merge_same(b, l) && !merge_same(b, m) && !merge_same(l, m))
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							errmsg("error: local changes would be stomped by merge"),
							errdetail("Local and merged changes overlap at %s %s.", key->kind, key->name)));
		chosen = merge_same(b, l) ? m : l;
		if (!chosen)
			continue;
		if (used)
			appendStringInfoChar(&json, ',');
		if (!strcmp(chosen->kind, "namespace"))
			appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", chosen->payload);
		else
			appendStringInfoString(&json, chosen->payload);
		hashes[used++] = PointerGetDatum(chosen->hash);
	}
	appendStringInfoString(&json, "]}");
	return canonical_database(json.data, hashes, used);
}

static char *
member_definition(Member * member)
{
	if (!member)
		return NULL;
	if (!strcmp(member->kind, "namespace"))
		return member->payload;
	return metadata(child(member->hash, 1), "schema");
}

/*
 * The indexes of table |name| merged by name from each side's index members,
 * appended to |members| and |hashes|, and listed in |definition|, which they
 * are returned in. NULL after describing a conflict.
 */
static char *
merge_table_indexes(const char *name, char *definition, Member * sides[3], int counts[3], Member * table[3],
					StringInfo members, Datum *hashes, int *used, StringInfo description)
{
	List	   *names[3] = {NIL, NIL, NIL},
			   *order = NIL,
			   *chosen = NIL;
	List	   *columns = definition_items(definition, "columns");
	StringInfoData sql;

	for (int side = 0; side < 3; side++)
		if (table[side])
			names[side] = table_index_names(member_definition(table[side]), name);
	foreach_ptr(char, index, names[1])
		order = lappend(order, index);
	foreach_ptr(char, index, names[2])
		if (!name_selected(names[1], index))
		order = lappend(order, index);
	foreach_ptr(char, index, order)
	{
		Member	   *m[3];
		Member	   *pick;

		char	   *d[3];

		/* Definitions, not members: an index member holds its entries too. */
		for (int side = 0; side < 3; side++)
		{
			m[side] = name_selected(names[side], index) ? merge_member(sides[side], counts[side], "index", index) : NULL;
			d[side] = member_definition(m[side]);
		}
		if (json_equal(d[1], d[2]) || json_equal(d[0], d[2]))
			pick = m[1];
		else if (json_equal(d[0], d[1]))
			pick = m[2];
		else
		{
			merge_describe(description, "different index definitions for our index %s and their index %s", index, index);
			return NULL;
		}
		if (!pick)
			continue;

		/* As Dolt, an index over a dropped column is dropped; one over a retyped column conflicts. */
		{
			bool		dropped = false;

			foreach_ptr(DefinitionItem, column, definition_items(member_definition(pick), "columns"))
			{
				char	   *merged = definition_item(columns, column->name);

				if (!merged)
					dropped = true;
				else if (!json_equal(column_type_unit(merged), column_type_unit(column->json)))
				{
					merge_describe(description, "index %s covers column %s, whose type changed on the other side", index, column->name);
					return NULL;
				}
			}
			if (dropped)
				continue;
		}
		chosen = lappend(chosen, pick);
	}
	initStringInfo(&sql);
	appendStringInfoChar(&sql, '[');
	foreach_ptr(Member, pick, chosen)
	{
		if (sql.len > 1)
			appendStringInfoChar(&sql, ',');
		escape_json(&sql, json_value(member_definition(pick), "index_definition"));
		appendStringInfo(members, "%s%s", *used ? "," : "", pick->payload);
		hashes[(*used)++] = PointerGetDatum(pick->hash);
	}
	appendStringInfoChar(&sql, ']');
	run("SELECT jsonb_set($1::jsonb,'{indexes}',(SELECT coalesce(jsonb_agg(v ORDER BY v #>> '{}' COLLATE \"C\"),'[]'::jsonb) FROM jsonb_array_elements($2::jsonb) v))::text", 2,
		VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(sql.data)), NULL, true);
	return strcell(0, 1);
}

/* A schema object's definition as dolt_schema_conflicts shows it. */
static char *
conflict_schema(Member * member, const char *kind, const char *name)
{
	if (!member)
		return pstrdup("<deleted>");
	if (!strcmp(kind, "table"))
		return definition_statement(name, member_definition(member));
	return member_definition(member);
}

/* Sequence snapshots do not make a definition edit. Applying a new source
 * definition joins its global counter; never seed it from the target counter. */
static bool
sequence_definition_equal(const char *a, const char *b)
{
	if (!a || !b)
		return a == b;
	run("SELECT ($1::jsonb - 'last_value' - 'is_called') = ($2::jsonb - 'last_value' - 'is_called')", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(a), CStringGetTextDatum(b)), NULL, true);
	return DatumGetBool(cell(0, 1));
}

static bool
sequence_source_behind(const char *ours, const char *theirs)
{
	bytea	   *okey = sequence_key(ours),
			   *tkey = sequence_key(theirs);
	int			order = memcmp(VARDATA_ANY(okey), VARDATA_ANY(tkey), VARSIZE_ANY_EXHDR(okey));
	int64		ov,
				tv;

	if (!order)
		return false;
	/* Opposite-direction merges acquire counter locks in the same order. */
	sequence_lock(order < 0 ? ours : theirs, ExclusiveLock);
	sequence_lock(order < 0 ? theirs : ours, ExclusiveLock);
	ours = sequence_capture(ours);
	theirs = sequence_capture(theirs);
	ov = sequence_number(ours, "last_value");
	tv = sequence_number(theirs, "last_value");
	if (ov == tv)
		return !strcmp(json_value(ours, "is_called"), "true") && !strcmp(json_value(theirs, "is_called"), "false");
	return sequence_number(ours, "increment") > 0 ? tv < ov : tv > ov;
}

static bytea *
merge_schemas(bytea *base, bytea *ours, bytea *theirs, List **row_merges, StringInfo schema_conflicts)
{
	int			counts[3];
	Member	   *sides[3] = {
		database_members(base, &counts[0]),
		database_members(ours, &counts[1]),
		database_members(theirs, &counts[2]),
	};
	List	   *seen = NIL;
	Datum	   *hashes;
	StringInfoData json;
	int			used = 0;

	*row_merges = NIL;
	hashes = palloc(sizeof(Datum) * (counts[0] + counts[1] + counts[2] + 1));
	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGDB1\",\"members\":[");
	for (int side = 0; side < 3; side++)
		for (int i = 0; i < counts[side]; i++)
		{
			Member	   *key = &sides[side][i];
			char	   *identity = psprintf("%s %s", key->kind, key->name);
			Member	   *b,
					   *o,
					   *t,
					   *chosen;
			Member	   *from;
			int			fromcount;
			bool		table,
						both;

			/* Indexes follow their tables. */
			if (!strcmp(key->kind, "index") || list_member(seen, makeString(identity)))
				continue;
			seen = lappend(seen, makeString(identity));
			b = merge_member(sides[0], counts[0], key->kind, key->name);
			o = merge_member(sides[1], counts[1], key->kind, key->name);
			t = merge_member(sides[2], counts[2], key->kind, key->name);
			table = !strcmp(key->kind, "table");

			/*
			 * A table changed on both sides merges row by row even when the
			 * results agree, as a keyless table's matching changes conflict.
			 */
			both = table && b && o && t && !merge_same(b, o) && !merge_same(b, t);
			if (!strcmp(key->kind, "sequence") && o && t)
			{
				char *od = member_definition(o), *td = member_definition(t);

				if (sequence_definition_equal(od, td) || (b && sequence_definition_equal(member_definition(b), td)))
					chosen = o, from = sides[1], fromcount = counts[1];
				else
				{
					chosen = t, from = sides[2], fromcount = counts[2];
					if (sequence_source_behind(od, td))
						record_schema_conflict(schema_conflicts, "sequence", key->name,
							conflict_schema(b, "sequence", key->name), od, td,
							"source sequence counter is behind target; source definition and counter selected");
				}
			}
			else if (!both && (merge_same(o, t) || merge_same(b, t)))
				chosen = o, from = sides[1], fromcount = counts[1];
			else if (!both && merge_same(b, o))
				chosen = t, from = sides[2], fromcount = counts[2];
			else if (table && o && t)
			{
				/* Changed on both sides: merge the definitions, then the rows. */
				char	   *defs[3] = {member_definition(b), member_definition(o), member_definition(t)};
				RowMerge   *merge = palloc0(sizeof(RowMerge));
				StringInfoData description;
				char	   *definition;

				initStringInfo(&description);
				merge->table = key->name;
				if (b && json_equal(defs[0], defs[1]) && json_equal(defs[1], defs[2]))
				{
					*row_merges = lappend(*row_merges, merge);
					chosen = o, from = sides[1], fromcount = counts[1];
				}
				else if ((definition = merge_table_definition(key->name, defs[0], defs[1], defs[2], &description)) &&
						 (definition = merge_table_indexes(key->name, definition, sides, counts, (Member *[3]) {b, o, t},
														   &json, hashes, &used, &description)))
				{
					bytea	   *member = merged_table_member(key->name, definition);
					char	   *payload = metadata(member, "table");

					merge->remap = true;
					for (int k = 0; k < 3; k++)
						merge->definitions[k] = defs[k];
					*row_merges = lappend(*row_merges, merge);

					appendStringInfo(&json, "%s%s", used ? "," : "", payload);
					hashes[used++] = PointerGetDatum(member);
					continue;
				}
				else
				{
					record_schema_conflict(schema_conflicts, key->kind, key->name, conflict_schema(b, "table", key->name),
										   conflict_schema(o, "table", key->name), conflict_schema(t, "table", key->name), description.data);
					chosen = o, from = sides[1], fromcount = counts[1];
				}
			}
			else
			{
				const char *description = !table ? psprintf("both branches changed %s %s", key->kind, key->name) :
					!b ? "both branches added the table" : "cannot merge a table deletion with data or schema modification";

				record_schema_conflict(schema_conflicts, key->kind, key->name, conflict_schema(b, key->kind, key->name),
									   conflict_schema(o, key->kind, key->name), conflict_schema(t, key->kind, key->name), description);
				chosen = o, from = sides[1], fromcount = counts[1];
			}
			if (!chosen)
				continue;
			if (used)
				appendStringInfoChar(&json, ',');
			if (!strcmp(chosen->kind, "namespace"))
				appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", chosen->payload);
			else
				appendStringInfoString(&json, chosen->payload);
			hashes[used++] = PointerGetDatum(chosen->hash);
			if (!strcmp(chosen->kind, "table"))
				foreach_ptr(char, index, table_index_names(metadata(child(chosen->hash, 1), "schema"), chosen->name))
				{
					Member	   *member = merge_member(from, fromcount, "index", index);

					if (!member)
						elog(ERROR, "canonical index %s of %s is missing", index, chosen->name);
					appendStringInfo(&json, ",%s", member->payload);
					hashes[used++] = PointerGetDatum(member->hash);
				}
		}
	appendStringInfoString(&json, "]}");
	return canonical_database(json.data, hashes, used);
}

/*
 * Merge |theirs| into the working set, whose state is |ours|, from their
 * common |base|: the merged schema is installed, then the rows of tables
 * changed on both sides are merged.
 */
#include "dg_inheritance_merge.inc"

static void
merge_databases(bytea *base, bytea *ours, bytea *theirs, StringInfo conflicts, StringInfo violations,
				StringInfo schema_conflicts)
{
	List	   *row_merges;
	bytea	   *merged = merge_schemas(base, ours, theirs, &row_merges, schema_conflicts);

	merged = merge_inheritance(base, ours, theirs, merged, &row_merges, schema_conflicts);

	replace_working(dg_current_branch(), merged, NIL);
	if (row_merges)
		install_database(base, ours, theirs, row_merges, conflicts, violations);
	else
		validate_working_foreign_keys(dg_current_branch(), violations);
}

/*
 * Resolve a signature's user types by canonical identity in either compared
 * state. They may have been dropped on the current branch. Native built-in
 * aliases still go through PostgreSQL's type parser. No runtime type handle
 * is needed to match the identity of a retained routine.
 */
static char *
routine_argument_name(TypeName *type, const char *a, const char *b)
{
	char	   *types;
	char	   *name;
	char	   *arg;
	Oid			oid = LookupTypeNameOid(NULL, type, true);

	/* Honor native search_path, including implicit pg_catalog precedence. */
	if (OidIsValid(oid))
		return routine_type_name(oid);

	if (list_length(type->names) == 1 || list_length(type->names) == 2)
	{
		arg = list_length(type->names) == 1 ?
			pstrdup(quote_identifier(strVal(linitial(type->names)))) :
			dg_qualified_name(strVal(linitial(type->names)), strVal(lsecond(type->names)));
		run("SELECT jsonb_build_object('members',coalesce(jsonb_agg(v || jsonb_build_object('kind','table')),'[]'::jsonb))::text FROM (SELECT v FROM jsonb_array_elements($1::jsonb->'members') v UNION SELECT v FROM jsonb_array_elements($2::jsonb->'members') v) m WHERE v->>'kind' IN ('type','table','view')", 2,
			VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(a), CStringGetTextDatum(b)), NULL, true);
		types = strcell(0, 1);
		name = resolve_table_argument(arg, types, types, false);
		if (name)
			return type->arrayBounds ? psprintf("%s[]", name) : name;
	}
	return routine_type_name(typenameTypeId(NULL, type));
}

/* Workspace and diff selectors include independently versioned schema objects. */
static char *
resolve_object_argument(const char *arg, const char *json_a, const char *json_b, bool required)
{
	List *parts;
	char *qualified = NULL;
	char *local;
	char *suffix = "";

	if (!*arg)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("object name cannot be empty")));
	/* Operator and cast identities are not SQL identifiers. Accept the exact
	 * portable identity shown by dolt_status before parsing routine syntax. */
	run("SELECT DISTINCT v->>'name' FROM (SELECT v FROM jsonb_array_elements($1::jsonb->'members') v UNION ALL SELECT v FROM jsonb_array_elements($2::jsonb->'members') v) m WHERE v->>'kind' IN ('aggregate','operator','cast') AND v->>'name'=$3", 3,
		VC_TYPES(TEXTOID,TEXTOID,TEXTOID), VC_ARGS(CStringGetTextDatum(json_a),CStringGetTextDatum(json_b),CStringGetTextDatum(arg)), NULL, true);
	if (SPI_processed) return strcell(0, 1);
	if (arg[strlen(arg) - 1] == ')')
	{
		List *parsed = raw_parser(psprintf("DROP ROUTINE %s", arg), RAW_PARSE_DEFAULT);
		DropStmt *drop;
		ObjectWithArgs *object;
		StringInfoData signature;

		if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, DropStmt))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid routine name: %s", arg)));
		drop = castNode(DropStmt, linitial_node(RawStmt, parsed)->stmt);
		if (drop->removeType != OBJECT_ROUTINE || list_length(drop->objects) != 1)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("expected one routine signature")));
		object = linitial_node(ObjectWithArgs, drop->objects);
		parts = object->objname;
		initStringInfo(&signature);
		appendStringInfoChar(&signature, '(');
		foreach_ptr(TypeName, type, object->objargs)
		{
			if (foreach_current_index(type))
				appendStringInfoChar(&signature, ',');
			appendStringInfoString(&signature, routine_argument_name(type, json_a, json_b));
		}
		appendStringInfoChar(&signature, ')');
		suffix = signature.data;
	}
	else
		parts = stringToQualifiedNameList(arg, NULL);
	if (!*suffix && (list_length(parts) == 2 || list_length(parts) == 3))
	{
		char *table = list_length(parts) == 3 ?
			dg_qualified_name(strVal(linitial(parts)), strVal(lsecond(parts))) :
			pstrdup(quote_identifier(strVal(linitial(parts))));
		char *wanted = psprintf("%s/%s", table, strVal(llast(parts)));

		run("WITH m AS (SELECT v FROM jsonb_array_elements($1::jsonb->'members') v UNION SELECT v FROM jsonb_array_elements($2::jsonb->'members') v) SELECT DISTINCT v->>'name' FROM m WHERE v->>'kind'='trigger' AND (v->>'name'=$3 OR substr(v->>'name',length(quote_ident((parse_ident(v->>'name',false))[1]))+2)=$3)", 3, VC_TYPES(TEXTOID,TEXTOID,TEXTOID), VC_ARGS(CStringGetTextDatum(json_a),CStringGetTextDatum(json_b),CStringGetTextDatum(wanted)), NULL, true);
		if (SPI_processed > 1)
			ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_ALIAS), errmsg("ambiguous trigger name: %s", arg)));
		if (SPI_processed)
			return strcell(0, 1);
	}
	if (list_length(parts) < 1 || list_length(parts) > 2)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid object name: %s", arg)));
	local = psprintf("%s%s", quote_identifier(strVal(llast(parts))), suffix);
	if (list_length(parts) == 2)
		qualified = psprintf("%s%s", dg_qualified_name(strVal(linitial(parts)), strVal(lsecond(parts))), suffix);

	/* parse_ident(...,false) stops at the signature, retaining quoted names.
	 * Compare the full suffix as well, so overloads never select each other. */
	run("WITH members AS (SELECT v FROM jsonb_array_elements($1::jsonb->'members') v UNION ALL SELECT v FROM jsonb_array_elements($2::jsonb->'members') v), names AS (SELECT DISTINCT v->>'name' AS name FROM members WHERE v->>'kind' IN ('table','routine','view','type','trigger','sequence','aggregate')), path AS (SELECT s,ord FROM (SELECT CASE WHEN btrim(s)='\"$user\"' THEN current_user::text ELSE btrim(btrim(s),'\"') END AS s,ord FROM unnest(string_to_array($5,',')) WITH ORDINALITY p(s,ord)) x WHERE EXISTS (SELECT 1 FROM pg_namespace WHERE nspname=x.s)) SELECT name,path.ord IS NOT NULL FROM names LEFT JOIN path ON (parse_ident(name,false))[1]=path.s WHERE ($3::text IS NOT NULL AND name=$3) OR ($3::text IS NULL AND substr(name,length(quote_ident((parse_ident(name,false))[1]))+2)=$4) ORDER BY path.ord NULLS LAST,name COLLATE \"C\"", 5, VC_TYPES(TEXTOID,TEXTOID,TEXTOID,TEXTOID,TEXTOID), VC_ARGS(CStringGetTextDatum(json_a),CStringGetTextDatum(json_b),qualified ? CStringGetTextDatum(qualified) : (Datum) 0,CStringGetTextDatum(local),CStringGetTextDatum(api_search_path ? api_search_path : "public")), qualified ? NULL : "  n  ", true);
	if (!SPI_processed)
	{
		if (!required)
			return NULL;
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("versioned object not found: %s", arg)));
	}
	if (SPI_processed > 1 && !DatumGetBool(cell(0, 2)))
		ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_ALIAS), errmsg("ambiguous versioned object name: %s", arg)));
	return strcell(0, 1);
}

static List *
state_objects(const char *json)
{
	List *result = NIL;

	run("SELECT v->>'name' FROM jsonb_array_elements($1::jsonb->'members') v WHERE v->>'kind' IN ('table','routine','view','type','trigger','sequence','aggregate','operator','cast') ORDER BY v->>'name' COLLATE \"C\"", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json)), NULL, true);
	for (int i = 0; i < SPI_processed; i++)
		result = lappend(result, strcell(i, 1));
	return result;
}

/* Resolve table arguments of a workspace command against the given states. */
static List *
table_arguments(char **args, int n, int first, const char *json_a, const char *json_b)
{
	List	   *result = NIL;
	int			i;

	for (i = first; i < n; i++)
	{
		if (!strcmp(args[i], "."))
			return NIL;
		result = lappend(result, resolve_object_argument(args[i], json_a, json_b, true));
	}
	return result;
}

static List *
state_tables(const char *json)
{
	List	   *result = NIL;
	int			i;

	run("SELECT v->>'name' FROM jsonb_array_elements($1::jsonb->'members') v WHERE v->>'kind'='table' ORDER BY v->>'name' COLLATE \"C\"", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json)), NULL, true);
	for (i = 0; i < SPI_processed; i++)
		result = lappend(result, strcell(i, 1));
	return result;
}

/* ---- Live table installation and removal ---- */

/* Publish the complete canonical definition first, then install matching
 * contents under that definition. All changes share the caller's SQL commit.
 * NIL tables replaces every table, including roots left by a deleted branch. */
static void
replace_working(const char *branch, bytea *database, List *tables)
{
	int			count;
	Member	   *members;
	CatalogView *view;
	List	   *selected;
	VersionRef	working;

	advance_snapshot();
	members = database_members(database, &count);
	view = provider_view(database);
	selected = tables ? selected_objects(members, count, tables) : NIL;
	working = read_ref(psprintf("working/%s", branch));

	binding_prepare(branch, database, false);
	advance_snapshot();
	for (int i = 0; i < count; i++)
	{
		Oid			resource;
		Relation	definition;
		bytea	   *schema;

		if (strcmp(members[i].kind, "table") && strcmp(members[i].kind, "index"))
			continue;
		if (tables && !name_selected(selected, members[i].name))
			continue;
		schema = child(members[i].hash, 1);
		resource = binding_relation(branch, members[i].name, schema);
		if (OidIsValid(resource))
			continue;
		definition = relation_open(CatalogViewLookupObject(view, RelationRelationId, members[i].name), NoLock);
		resource = dg_storage_allocate(definition, branch);
		binding_record(branch, members[i].name, schema, resource);
		relation_close(definition, NoLock);
	}
	move_ref(psprintf("working/%s", branch), working, database);
	advance_snapshot();
	for (int i = 0; i < count; i++)
	{
		Relation	rel;
		DGState    *state;
		PLTree		tree;

		if (strcmp(members[i].kind, "table") ||
			(tables && !name_selected(tables, members[i].name)))
			continue;
		rel = dg_catalog_write_table(branch, members[i].name, ACL_UPDATE);
		state = dg_load(rel, SnapshotSelf);
		tree = dg_history_import(state->store, table_chunk(database, members[i].name));
		dg_history_install(rel, state, tree);
		dg_release(state);
		dg_storage_close_relation(rel);
		UpdateActiveSnapshotCommandId();
	}
	if (tables)
		validate_working_foreign_keys(branch, NULL);
}

static void
restore_table_on(const char *branch, bytea *db, const char *name)
{
	bytea	   *current = !strcmp(branch, dg_current_branch()) ?
		capture(true).hash : read_ref(psprintf("working/%s", branch)).target;
	List	   *tables = list_make1(pstrdup(name));
	bytea	   *result;

	advance_snapshot();
	result = compose_database(current, db, tables);

	replace_working(branch, result, tables);
}

static void
restore_table(bytea *db, const char *name)
{
	restore_table_on(dg_current_branch(), db, name);
}

/* Drop selected objects in dependency order, batching tables so native DROP
 * can remove foreign keys between selected tables without CASCADE. Objects
 * outside the selection remain protected by PostgreSQL dependency checks. */
static void
drop_objects(List *names)
{
	const char *kinds[] = {"view", "table", "routine", "type", "sequence"};
	const char *commands[] = {"VIEW", "TABLE", "ROUTINE", "TYPE", "SEQUENCE"};
	char *json = working_state();
	int count;
	Member *members = database_members(capture(true).hash, &count);

	for (int i = 0; i < count; i++)
		if (!strcmp(members[i].kind, "trigger") && name_selected(names, members[i].name))
		{
			char *definition = metadata(child(members[i].hash, 1), "schema");
			char *table = json_value(definition, "table");
			const char *trigger = members[i].name + strlen(table) + 1;

			run_context(psprintf("DROP TRIGGER %s ON %s", quote_identifier(trigger), table), 0, NULL, NULL, NULL, false);
			CommandCounterIncrement();
			UpdateActiveSnapshotCommandId();
		}

	for (int kind = 0; kind < lengthof(kinds); kind++)
	{
		StringInfoData command;
		bool found = false;

		initStringInfo(&command);
		/* Dropping a table also drops its owned sequences. The selection
		 * was captured before those dependency removals. */
		appendStringInfo(&command, "DROP %s IF EXISTS ", commands[kind]);
		foreach_ptr(char, name, names)
		{
			run("SELECT 1 FROM jsonb_array_elements($1::jsonb->'members') v WHERE v->>'name'=$2 AND v->>'kind'=$3", 3, VC_TYPES(TEXTOID,TEXTOID,TEXTOID), VC_ARGS(CStringGetTextDatum(json),CStringGetTextDatum(name),CStringGetTextDatum(kinds[kind])), NULL, true);
			if (!SPI_processed)
				continue;
			appendStringInfo(&command, "%s%s", found ? "," : "", name);
			found = true;
		}
		if (found)
		{
			run_context(command.data, 0, NULL, NULL, NULL, false);
			CommandCounterIncrement();
			UpdateActiveSnapshotCommandId();
		}
	}
}

/* Persist the live working state and make its objects visible to the API
 * snapshot (capture publishes them under a temporary latest snapshot). */
static Capture
capture_visible(void)
{
	Capture		current = capture(true);

	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
	return current;
}

/* ---- dolt_add with table names ---- */

static void
stage_tables(List *tables)
{
	VersionRef	staged = read_ref(branch_ref("staged")),
				working = read_ref(branch_ref("working"));
	Capture		current = capture_visible();
	bytea	   *base = staged.target ? staged.target : child(read_ref(branch_ref("heads")).target, 1);
	bytea	   *result = compose_database(base, current.hash, tables);

	move_ref(branch_ref("staged"), staged, result);
	move_ref(branch_ref("working"), working, current.hash);
}

/* -a stages changes to tracked tables; untracked tables remain in working. */
static void
stage_modified_tables(void)
{
	stage_tables(state_objects(staged_state()));
	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
}

/* ---- dolt_reset ---- */

Datum
dolt_reset(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n);
	bool		hard = false,
				soft = false;
	char	   *target = NULL;
	List	   *names = NIL;
	VersionRef	head,
				staged;
	bytea	   *headdb;

	standalone(fcinfo, true);
	nest = begin_api();
	lock_workspace();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--hard"))
			hard = true;
		else if (!strcmp(args[i], "--soft"))
			soft = true;
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else
			names = lappend(names, args[i]);
	}
	if (hard && soft)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("error: --hard and --soft are mutually exclusive options.")));
	head = read_ref(branch_ref("heads"));
	staged = read_ref(branch_ref("staged"));
	headdb = child(head.target, 1);
	if (hard || soft)
	{
		bytea	   *commit;

		if (list_length(names) > 1)
			unsupported("reset accepts at most one revision");
		target = names ? (char *) linitial(names) : "HEAD";
		commit = resolve(target);
		if (hard)
		{
			bytea	   *db = child(commit, 1);

			replace_working(dg_current_branch(), db, NIL);
			move_ref(branch_ref("staged"), staged, db);
			finish_merge();
		}
		if (!same(commit, head.target))
			move_ref(branch_ref("heads"), head, commit);
	}
	else if (names == NIL || (list_length(names) == 1 && !strcmp(linitial(names), ".")))
	{
		/* Unstage everything. */
		move_ref(branch_ref("staged"), staged, headdb);
	}
	else
	{
		char	   *stagedjson = staged_state(),
				   *headjson = head_state();
		bool		is_table = resolve_object_argument((char *) linitial(names), stagedjson, headjson, false) != NULL;

		if (!is_table && list_length(names) == 1)
		{
			/*
			 * Mixed reset to a revision: HEAD and staging move, working
			 * stays.
			 */
			bytea	   *commit = resolve((char *) linitial(names));

			move_ref(branch_ref("heads"), head, commit);
			move_ref(branch_ref("staged"), staged, child(commit, 1));
		}
		else
		{
			List	   *tables = table_arguments(args, n, 0, stagedjson, headjson);

			move_ref(branch_ref("staged"), staged, compose_database(staged.target ? staged.target : headdb, headdb, tables));
		}
	}
	end_api(nest);
	PG_RETURN_INT64(0);
}

Datum
dolt_reset_noargs(PG_FUNCTION_ARGS)
{
	return dolt_reset(fcinfo);
}

/* ---- dolt_clean: drop untracked tables ---- */

Datum
dolt_clean(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n);
	bool		dry = false;
	List	   *names = NIL,
			   *tracked,
			   *live,
			   *untracked = NIL;
	ListCell   *lc;
	char	   *workingjson;

	standalone(fcinfo, true);
	nest = begin_api();
	lock_workspace();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--dry-run"))
			dry = true;
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else
			names = lappend(names, args[i]);
	}
	workingjson = working_state();
	tracked = list_concat(state_objects(head_state()), state_objects(staged_state()));
	live = state_objects(workingjson);
	if (names)
		foreach(lc, names)
	{
		char	   *name = resolve_object_argument((char *) lfirst(lc), workingjson, workingjson, false);

		if (!name)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("table not found: '%s'", (char *) lfirst(lc))));
		if (!name_selected(tracked, name))
			untracked = lappend(untracked, name);
	}
	else
		foreach(lc, live)
		if (!name_selected(tracked, (char *) lfirst(lc)))
		untracked = lappend(untracked, lfirst(lc));
	if (!dry)
		drop_objects(untracked);
	end_api(nest);
	PG_RETURN_INT64(0);
}

Datum
dolt_clean_noargs(PG_FUNCTION_ARGS)
{
	return dolt_clean(fcinfo);
}

/* ---- dolt_rm ---- */

Datum
dolt_rm(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n);
	bool		cached = false;
	List	   *tables = NIL;
	VersionRef	staged;
	char	   *stagedjson;

	standalone(fcinfo, true);
	nest = begin_api();
	lock_workspace();
	stagedjson = staged_state();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--cached"))
			cached = true;
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else
			tables = lappend(tables, resolve_object_argument(args[i], stagedjson, working_state(), true));
	}
	if (!tables)
		unsupported("dolt_rm requires table names");
	staged = read_ref(branch_ref("staged"));
	move_ref(branch_ref("staged"), staged, compose_database(staged.target ? staged.target : child(read_ref(branch_ref("heads")).target, 1), NULL, tables));
	if (!cached)
		drop_objects(tables);
	end_api(nest);
	PG_RETURN_INT64(0);
}

/* ---- Table checkout: restore working tables from HEAD ---- */

static void
checkout_tables(char **args, int n)
{
	char	   *headjson = head_state(),
			   *workingjson = working_state();
	bytea	   *headdb = child(read_ref(branch_ref("heads")).target, 1);
	List	   *headtables = state_objects(headjson);
	int			i;
	StringInfoData missing;

	initStringInfo(&missing);
	for (i = 0; i < n; i++)
	{
		char	   *name = resolve_object_argument(args[i], headjson, workingjson, false);

		if (!name)
		{
			if (missing.len)
				appendStringInfoChar(&missing, '\n');
			appendStringInfo(&missing, "error: tablespec '%s' did not match any table(s) known to dolt", args[i]);
			continue;
		}
		if (name_selected(headtables, name))
			restore_table(headdb, name);
		else
			drop_objects(list_make1(name));
	}
	if (missing.len)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("%s", missing.data)));
}

/* ---- Branch and tag management ---- */

static void
delete_branch_refs(const char *name)
{
	const char *prefixes[] = {"heads", "working", "staged", "merge"};
	int			i;

	for (i = 0; i < lengthof(prefixes); i++)
	{
		VersionRef	r = read_ref(psprintf("%s/%s", prefixes[i], name));

		if (r.target)
			move_ref(psprintf("%s/%s", prefixes[i], name), r, NULL);
	}
	run("DELETE FROM pg_catalog.pg_dolt_binding WHERE bindbranch=$1", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(name)), NULL, false);
	dg_identity_delete_branch(name);
}

static void
create_branch_refs(const char *name, bytea *commit, bytea *working_db)
{
	move_ref(psprintf("heads/%s", name), read_ref(psprintf("heads/%s", name)), commit);
	move_ref(psprintf("working/%s", name), read_ref(psprintf("working/%s", name)), working_db ? working_db : child(commit, 1));
	move_ref(psprintf("staged/%s", name), read_ref(psprintf("staged/%s", name)), child(commit, 1));
	replace_working(name, working_db ? working_db : child(commit, 1), NIL);
}

static void
require_clean_branch(const char *name)
{
	VersionRef	head = read_ref(psprintf("heads/%s", name)),
				working = read_ref(psprintf("working/%s", name)),
				staged = read_ref(psprintf("staged/%s", name));
	bytea	   *db = child(head.target, 1);

	if ((working.target && !database_same(working.target, db)) || (staged.target && !database_same(staged.target, db)))
		unsupported("renaming or copying a branch with uncommitted changes");
	if (!strcmp(name, dg_current_branch()) && state_differs(working_state(), head_state()))
		unsupported("renaming or copying the session branch with uncommitted changes");
}

/* Extended dolt_branch: -d/-D delete, -m rename, -c copy, -f force. */
static bool
branch_management(char **args, int n)
{
	bool		del = false,
				rename = false,
				copy = false,
				force = false;
	List	   *names = NIL;
	ListCell   *lc;
	int			i;

	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "-d") || !strcmp(args[i], "--delete"))
			del = true;
		else if (!strcmp(args[i], "-D"))
			del = force = true;
		else if (!strcmp(args[i], "-m") || !strcmp(args[i], "--move"))
			rename = true;
		else if (!strcmp(args[i], "-c") || !strcmp(args[i], "--copy"))
			copy = true;
		else if (!strcmp(args[i], "-f") || !strcmp(args[i], "--force"))
			force = true;
		else if (args[i][0] == '-' && args[i][1] && args[i][1] != '-')
		{
			for (const char *flag = args[i] + 1; *flag; flag++)
				switch (*flag)
				{
					case 'd': del = true; break;
					case 'D': del = force = true; break;
					case 'm': rename = true; break;
					case 'c': copy = true; break;
					case 'f': force = true; break;
					default: unsupported(args[i]);
				}
		}
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else
			names = lappend(names, args[i]);
	}
	if (!del && !rename && !copy)
	{
		if (!force)
			return false;
		if (list_length(names) < 1 || list_length(names) > 2)
			unsupported("-f requires a branch name and optional start point");
		lock_workspace();
		ensure_repository();
		{
			char *name = linitial(names);
			bytea *start;

			valid_name(name);
			start = resolve(list_length(names) == 2 ? lsecond(names) : "HEAD");
			/* Reset both workspace refs and local root bindings. Old roots
			 * tagged with this branch name must not survive its replacement. */
			delete_branch_refs(name);
			advance_snapshot();
			create_branch_refs(name, start, NULL);
		}
		return true;
	}
	if ((del ? 1 : 0) + (rename ? 1 : 0) + (copy ? 1 : 0) > 1)
		unsupported("combining -d, -m and -c");
	lock_workspace();
	ensure_repository();
	if (del)
	{
		if (!names)
			unsupported("-d requires branch names");
		foreach(lc, names)
		{
			char	   *name = (char *) lfirst(lc);

			if (!*name)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("error: cannot delete empty branch name")));
			if (!strcmp(name, dg_current_branch()))
				ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("Cannot delete checked out branch '%s'", name)));
			if (!read_ref(psprintf("heads/%s", name)).target)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("branch not found: %s", name)));
			if (!force)
				dg_branch_require_unused(name);
			delete_branch_refs(name);
		}
		return true;
	}
	if (list_length(names) != 2)
		unsupported(rename ? "-m requires old and new branch names" : "-c requires source and destination branch names");
	{
		char	   *src = (char *) linitial(names),
				   *dst = (char *) lsecond(names);
		VersionRef	srchead = read_ref(psprintf("heads/%s", src));

		valid_name(dst);
		if (!srchead.target)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("branch not found: %s", src)));
		if (read_ref(psprintf("heads/%s", dst)).target)
		{
			if (!force)
				ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("branch already exists: %s", dst)));
			if (!strcmp(dst, dg_current_branch()))
				ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("Cannot overwrite checked out branch '%s'", dst)));
			delete_branch_refs(dst);
			advance_snapshot();
		}
		if (rename && strcmp(src, dg_current_branch()) && !force)
			dg_branch_require_unused(src);
		require_clean_branch(src);
		create_branch_refs(dst, srchead.target, NULL);
		if (rename)
		{
			advance_snapshot();
			delete_branch_refs(src);
			if (!strcmp(src, dg_current_branch()))
				dg_select_branch(dst);
		}
	}
	return true;
}

/* ---- Commit graph helpers ---- */

static int64
ancestor_difference(bytea *of, bytea *minus)
{
	run("WITH RECURSIVE a(h) AS (SELECT $1::bytea UNION SELECT unnest(o.objchildren[2:]) FROM a JOIN pg_dolt_object_visible o ON o.objhash=a.h), b(h) AS (SELECT $2::bytea UNION SELECT unnest(o.objchildren[2:]) FROM b JOIN pg_dolt_object_visible o ON o.objhash=b.h) SELECT count(*) FROM (SELECT h FROM a EXCEPT SELECT h FROM b) x", 2, VC_TYPES(BYTEAOID, BYTEAOID), VC_ARGS(PointerGetDatum(of), PointerGetDatum(minus)), NULL, true);
	return DatumGetInt64(cell(0, 1));
}

Datum
dolt_count_commits(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n),
			   *from = NULL,
			   *to = NULL;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	bytea	   *a,
			   *b;

	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--from") && i + 1 < n)
			from = args[++i];
		else if (!strcmp(args[i], "--to") && i + 1 < n)
			to = args[++i];
		else if (!strncmp(args[i], "--from=", 7))
			from = args[i] + 7;
		else if (!strncmp(args[i], "--to=", 5))
			to = args[i] + 5;
		else
			unsupported(args[i]);
	}
	if (!from || !*from)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("missing from ref")));
	if (!to || !*to)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("missing to ref")));
	nest = begin_api();
	a = resolve(from);
	b = resolve(to);
	values[0] = Int64GetDatum(ancestor_difference(b, a));
	values[1] = Int64GetDatum(ancestor_difference(a, b));
	end_api(nest);
	return command_record(fcinfo, values, nulls);
}

Datum
dolt_branch_status(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bytea	   *base;

	if (n < 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("dolt_branch_status requires a base revision")));
	nest = begin_api();
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	base = resolve(args[0]);
	for (i = 1; i < n; i++)
	{
		bytea	   *branch = resolve(args[i]);
		Datum		values[3];
		bool		nulls[3] = {false, false, false};

		values[0] = CStringGetTextDatum(args[i]);
		values[1] = DirectFunctionCall1(int8_numeric, Int64GetDatum(ancestor_difference(branch, base)));
		values[2] = DirectFunctionCall1(int8_numeric, Int64GetDatum(ancestor_difference(base, branch)));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	end_api(nest);
	return (Datum) 0;
}

Datum
dolt_merge_base(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	bytea	   *a = resolve(text_to_cstring(PG_GETARG_TEXT_PP(0))),
			   *b = resolve(text_to_cstring(PG_GETARG_TEXT_PP(1)));
	char	   *answer = hash_format(merge_base(a, b));

	end_api(nest);
	PG_RETURN_TEXT_P(cstring_to_text(answer));
}

Datum
dolt_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(PG_VERSION "-dolt"));
}

Datum
dolt_hashof_table(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	char	   *working = working_state();
	char	   *name = resolve_object_argument(text_to_cstring(PG_GETARG_TEXT_PP(0)), working, working, true);
	char	   *answer;

	run("SELECT v->>'content' FROM jsonb_array_elements($1::jsonb->'members') v WHERE v->>'kind'='table' AND v->>'name'=$2", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(working), CStringGetTextDatum(name)), NULL, true);
	answer = strcell(0, 1);
	end_api(nest);
	PG_RETURN_TEXT_P(cstring_to_text(answer));
}

Datum
dolt_hashof_db(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	char	   *revision = PG_NARGS() ? text_to_cstring(PG_GETARG_TEXT_PP(0)) : "WORKING";
	bytea	   *db;
	char	   *answer;

	if (!pg_strcasecmp(revision, "WORKING"))
		db = capture_visible().hash;
	else
	{
		bool		working;

		db = revision_database(revision, &working);
	}
	answer = hash_format(db);
	end_api(nest);
	PG_RETURN_TEXT_P(cstring_to_text(answer));
}

Datum
dolt_hashof_db_noargs(PG_FUNCTION_ARGS)
{
	return dolt_hashof_db(fcinfo);
}

/* ---- dolt_reflog ---- */

Datum
dolt_reflog(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n),
			   *filter = NULL;
	bool		all = false;

	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--all"))
			all = true;
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else if (filter)
			unsupported(args[i]);
		else
			filter = args[i];
	}
	nest = begin_api();
	run("SELECT CASE WHEN l.reflogname LIKE 'heads/%' THEN 'refs/heads/' || substr(l.reflogname,7) WHEN l.reflogname LIKE 'tags/%' THEN 'refs/tags/' || substr(l.reflogname,6) ELSE 'refs/' || l.reflogname END,l.reflogtime,pg_catalog.dolt_hash_encode(CASE WHEN o.objkind='tag' THEN o.objchildren[1] ELSE l.reflogtarget END),m.meta->>'message' FROM pg_catalog.pg_dolt_reflog l JOIN pg_dolt_object_visible o ON o.objhash=l.reflogtarget LEFT JOIN pg_dolt_commit_visible m ON m.objhash=CASE WHEN o.objkind='tag' THEN o.objchildren[1] ELSE l.reflogtarget END WHERE (l.reflogname LIKE 'heads/%' OR l.reflogname LIKE 'tags/%' OR $3) AND ($1::text IS NULL OR l.reflogname=$1 OR l.reflogname='heads/' || $1 OR l.reflogname='tags/' || $1 OR 'refs/' || l.reflogname=$1 OR $2) ORDER BY l.reflogtime DESC,l.reflogname", 3, VC_TYPES(TEXTOID, BOOLOID, BOOLOID), VC_ARGS(filter ? CStringGetTextDatum(filter) : (Datum) 0, BoolGetDatum(filter == NULL), BoolGetDatum(all)), filter ? NULL : "n  ", true);
	return_rows(fcinfo, nest);
	return (Datum) 0;
}

Datum
dolt_reflog_noargs(PG_FUNCTION_ARGS)
{
	return dolt_reflog(fcinfo);
}

/* ---- dolt_revert and dolt_cherry_pick: three-way application of one
 * commit's changes (or their inverse) onto the branch head ---- */

static Datum
apply_commit(FunctionCallInfo fcinfo, bool revert)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n),
			   *author = NULL,
			   *email = NULL;
	List	   *revisions = NIL;
	ListCell   *lc;
	VersionRef	head;
	bytea	   *commit = NULL;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};

	standalone(fcinfo, true);
	nest = begin_api();
	lock_workspace();
	ensure_repository();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--author") && i + 1 < n)
			author_option(args[++i], &author, &email);
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else
			revisions = lappend(revisions, args[i]);
	}
	if (!revisions)
		unsupported(revert ? "dolt_revert requires a revision" : "dolt_cherry_pick requires a revision");
	if (!revert && list_length(revisions) != 1)
		unsupported("cherry-picking more than one commit");
	head = read_ref(branch_ref("heads"));
	if (!database_same(capture_visible().hash, child(head.target, 1)) || !database_same(read_ref(branch_ref("staged")).target, child(head.target, 1)))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("cannot %s with uncommitted changes", revert ? "revert" : "cherry-pick")));
	foreach(lc, revisions)
	{
		bytea	   *target = resolve((char *) lfirst(lc)),
				   *parent,
				   *targetdb,
				   *parentdb,
				   *database;
		char	   *meta = metadata(target, "commit"),
				   *message;
		StringInfoData conflicts,
					violations,
					schema_conflicts;

		if (parent_count(target) < 1)
			unsupported("reverting or cherry-picking a root commit");
		parent = child(target, 2);
		targetdb = child(target, 1);
		parentdb = child(parent, 1);
		head = read_ref(branch_ref("heads"));
		initStringInfo(&conflicts);
		initStringInfo(&violations);
		initStringInfo(&schema_conflicts);
		if (revert)
			merge_databases(targetdb, child(head.target, 1), parentdb, &conflicts, &violations, &schema_conflicts);
		else
			merge_databases(parentdb, child(head.target, 1), targetdb, &conflicts, &violations, &schema_conflicts);
		if (conflicts.len || violations.len || schema_conflicts.len)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("%s resulted in conflicts; resolve them with dolt_merge", revert ? "revert" : "cherry-pick")));
		database = capture_visible().hash;
		message = revert ? psprintf("Revert \"%s\"", json_value(meta, "message")) : json_value(meta, "message");
		commit = commit_object(database, head.target, NULL, message, author, email, GetCurrentTimestamp());
		move_ref(branch_ref("heads"), head, commit);
		move_ref(branch_ref("staged"), read_ref(branch_ref("staged")), database);
		move_ref(branch_ref("working"), read_ref(branch_ref("working")), database);
		advance_snapshot();
	}
	values[0] = CStringGetTextDatum(hash_format(commit));
	values[1] = Int64GetDatum(0);
	values[2] = Int64GetDatum(0);
	values[3] = Int64GetDatum(0);
	end_api(nest);
	return command_record(fcinfo, values, nulls);
}

Datum
dolt_revert(PG_FUNCTION_ARGS)
{
	return apply_commit(fcinfo, true);
}

Datum
dolt_cherry_pick(PG_FUNCTION_ARGS)
{
	return apply_commit(fcinfo, false);
}
