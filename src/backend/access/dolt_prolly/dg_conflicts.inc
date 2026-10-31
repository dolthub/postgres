/* Conflict and constraint-violation system tables, merge previews and
 * constraint verification. See doc/doltgres/conflicts.md. Included by
 * dg_version.c after dg_diff.inc. */

/* ---- Three-way conflict detection over revision tables ----
 *
 * Mirrors merge_rows: keyed rows conflict when both sides changed the same
 * column differently (JSON objects merge by member); keyless rows conflict
 * when both sides changed the cardinality of the same row value. */

typedef struct ConflictRow
{
	PLValue		base,
				ours,
				theirs;			/* packed rows (keyless: occurrence prefix stripped) */
	uint64		base_count,
				our_count,
				their_count;
	PLSlice		key;
} ConflictRow;

typedef void (*ConflictRowFn) (DiffTable *base, DiffTable *ours, DiffTable *theirs, ConflictRow *row, void *arg);
static bytea *preview_branch(const char *name, bool summary);

static PLValue
row_at(DiffTable *t, size_t i, uint64 *count)
{
	PLSlice		bytes = t->rows.items[i].value;

	*count = 1;
	if (!t->keyed)
	{
		*count = pl_read64(bytes.data);
		bytes.data += 8;
		bytes.len -= 8;
	}
	return DG_VALUE(true, bytes);
}

static bool
columns_conflict(DiffTable *base, DiffTable *ours, DiffTable *theirs, PLValue b, PLValue o, PLValue t)
{
	int			k;

	if (base->desc->natts != ours->desc->natts || ours->desc->natts != theirs->desc->natts)
		return true;
	for (k = 0; k < ours->desc->natts; k++)
	{
		PLValue		f[3];
		PLValue		merged;

		dg_check(pl_tuple_get(b.bytes, k, &f[0]));
		dg_check(pl_tuple_get(o.bytes, k, &f[1]));
		dg_check(pl_tuple_get(t.bytes, k, &f[2]));
		if (equal_value(f[1], f[2]) || equal_value(f[0], f[2]) || equal_value(f[0], f[1]))
			continue;
		if ((TupleDescAttr(ours->desc, k)->atttypid == JSONOID || TupleDescAttr(ours->desc, k)->atttypid == JSONBOID) && merge_json_field(TupleDescAttr(ours->desc, k)->atttypid, f, &merged))
			continue;
		return true;
	}
	return false;
}

static void
conflict_rows(DiffTable *base, DiffTable *ours, DiffTable *theirs, ConflictRowFn fn, void *arg)
{
	size_t		at[3] = {0};
	DiffTable  *t[3] = {base, ours, theirs};
	bool		keyed = (base->present && base->keyed) || (ours->present && ours->keyed) || (theirs->present && theirs->keyed);
	int			j;

	for (j = 0; j < 3; j++)
		if (t[j]->present && t[j]->keyed != keyed)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot merge keyed and keyless versions of %s", t[j]->name)));
	for (;;)
	{
		PLSlice		key = {0};
		ConflictRow row;
		PLValue		v[3] = {{0}};
		uint64		counts[3] = {0};
		bool		conflict = false;

		CHECK_FOR_INTERRUPTS();
		for (j = 0; j < 3; j++)
			if (t[j]->present && at[j] < t[j]->rows.count && (!key.data || pl_compare(t[j]->rows.items[at[j]].key, key) < 0))
				key = t[j]->rows.items[at[j]].key;
		if (!key.data)
			break;
		for (j = 0; j < 3; j++)
			if (t[j]->present && at[j] < t[j]->rows.count && !pl_compare(t[j]->rows.items[at[j]].key, key))
				v[j] = row_at(t[j], at[j]++, &counts[j]);
		if (!keyed)
		{
			bool		ours_changed = counts[0] != counts[1],
						theirs_changed = counts[0] != counts[2];

			conflict = ours_changed && theirs_changed;
		}
		else if (equal_value(v[1], v[2]) || equal_value(v[0], v[1]) || equal_value(v[0], v[2]))
			conflict = false;
		else if (v[0].present && v[1].present && v[2].present)
			conflict = columns_conflict(base, ours, theirs, v[0], v[1], v[2]);
		else
			conflict = true;
		if (!conflict)
			continue;
		row.base = v[0];
		row.ours = v[1];
		row.theirs = v[2];
		row.base_count = counts[0];
		row.our_count = counts[1];
		row.their_count = counts[2];
		row.key = key;
		fn(base, ours, theirs, &row, arg);
	}
}

/* ---- Output layout: from_root_ish, base_*, our_*, our_diff_type, their_*,
 * their_diff_type, dolt_conflict_id [, cardinalities] ---- */

static TupleDesc
conflict_descriptor(DiffTable *base, DiffTable *ours, DiffTable *theirs)
{
	DiffTable  *any = ours->present ? ours : theirs->present ? theirs : base;
	TupleDesc	bdesc = base->present ? base->desc : any->desc,
				odesc = ours->present ? ours->desc : any->desc,
				tdesc = theirs->present ? theirs->desc : any->desc,
				out;
	bool		keyless = !any->keyed;
	int			at = 0;

	out = CreateTemplateTupleDesc(bdesc->natts + odesc->natts + tdesc->natts + 4 + (keyless ? 3 : 0));
	add_column(out, &at, "from_root_ish", TEXTOID);
	add_prefixed_columns(out, &at, "base_", bdesc);
	add_prefixed_columns(out, &at, "our_", odesc);
	add_column(out, &at, "our_diff_type", TEXTOID);
	add_prefixed_columns(out, &at, "their_", tdesc);
	add_column(out, &at, "their_diff_type", TEXTOID);
	add_column(out, &at, "dolt_conflict_id", TEXTOID);
	if (keyless)
	{
		add_column(out, &at, "base_cardinality", INT8OID);
		add_column(out, &at, "our_cardinality", INT8OID);
		add_column(out, &at, "their_cardinality", INT8OID);
	}
	return out;
}

static const char *
conflict_diff_type(PLValue base, PLValue other)
{
	if (!base.present)
		return "added";
	if (!other.present)
		return "removed";
	return "modified";
}

static char *
conflict_id(const char *table, PLSlice key)
{
	bytea	   *raw = palloc(VARHDRSZ + key.len);
	char	   *digest;

	SET_VARSIZE(raw, VARHDRSZ + key.len);
	memcpy(VARDATA(raw), key.data, key.len);
	run("SELECT encode(sha256(convert_to($1,'UTF8') || $2),'hex')", 2, VC_TYPES(TEXTOID, BYTEAOID), VC_ARGS(CStringGetTextDatum(table), PointerGetDatum(raw)), NULL, true);
	digest = strcell(0, 1);
	return pnstrdup(digest, 32);
}

typedef struct ConflictEmit
{
	ReturnSetInfo *rsinfo;
	const char *root_ish;
	DiffTable  *base,
			   *ours,
			   *theirs;
} ConflictEmit;

static void
emit_conflict_row(DiffTable *base, DiffTable *ours, DiffTable *theirs, ConflictRow *row, void *arg)
{
	ConflictEmit *e = (ConflictEmit *) arg;
	TupleDesc	out = e->rsinfo->setDesc;
	DiffTable  *any = e->ours->present ? e->ours : e->theirs->present ? e->theirs : e->base;
	TupleDesc	bdesc = e->base->present ? e->base->desc : any->desc,
				odesc = e->ours->present ? e->ours->desc : any->desc,
				tdesc = e->theirs->present ? e->theirs->desc : any->desc;
	Datum	   *values = palloc0(sizeof(Datum) * out->natts);
	bool	   *nulls = palloc(sizeof(bool) * out->natts);
	int			at = 0;

	memset(nulls, true, out->natts);
	values[at] = CStringGetTextDatum(e->root_ish);
	nulls[at++] = false;
	place_values(out, at, bdesc, base, row->base, values, nulls);
	at += bdesc->natts;
	place_values(out, at, odesc, ours, row->ours, values, nulls);
	at += odesc->natts;
	values[at] = CStringGetTextDatum(conflict_diff_type(row->base, row->ours));
	nulls[at++] = false;
	place_values(out, at, tdesc, theirs, row->theirs, values, nulls);
	at += tdesc->natts;
	values[at] = CStringGetTextDatum(conflict_diff_type(row->base, row->theirs));
	nulls[at++] = false;
	values[at] = CStringGetTextDatum(conflict_id(any->name, row->key));
	nulls[at++] = false;
	if (!any->keyed)
	{
		values[at] = Int64GetDatum((int64) row->base_count);
		nulls[at++] = false;
		values[at] = Int64GetDatum((int64) row->our_count);
		nulls[at++] = false;
		values[at] = Int64GetDatum((int64) row->their_count);
		nulls[at++] = false;
	}
	tuplestore_putvalues(e->rsinfo->setResult, out, values, nulls);
}

/* Pending-merge evidence for a table, turned into revision tables of its
 * three versions. The merge object children are ours, theirs, base commits. */
static bool
pending_merge_commits(bytea **ours, bytea **theirs, bytea **base, char **json)
{
	VersionRef	pending = read_ref(branch_ref("merge"));

	if (!pending.target)
		return false;
	*ours = child(pending.target, 1);
	*theirs = child(pending.target, 2);
	*base = child(pending.target, 3);
	*json = metadata(pending.target, "merge");
	return true;
}

/* Whether |table|'s conflicting rows are laid out in its merged definition. */
static bool
conflicts_merged(const char *json, const char *table)
{
	run("SELECT EXISTS (SELECT 1 FROM jsonb_array_elements($1::jsonb->'conflicts') v WHERE v->>'table'=$2 AND v->>'layout'='merged')", 2,
		VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), CStringGetTextDatum(table)), NULL, true);
	return DatumGetBool(cell(0, 1));
}

/* Layout for dolt_conflicts_<t> and dolt_preview_merge_conflicts(). */
static TupleDesc
conflict_layout(const char *table, const char *from, const char *to)
{
	DiffTable  *base,
			   *ours,
			   *theirs;

	if (from && to)
	{
		bytea	   *oursc = preview_branch(from, false),
				   *theirsc = preview_branch(to, false),
				   *basec = merge_base(oursc, theirsc);
		char	   *resolved = resolve_table_argument(table, metadata(child(oursc, 1), "database"), metadata(child(theirsc, 1), "database"), true);

		base = diff_table_at(child(basec, 1), resolved, false);
		ours = diff_table_at(child(oursc, 1), resolved, false);
		theirs = diff_table_at(child(theirsc, 1), resolved, false);
	}
	else
	{
		bytea	   *oursc,
				   *theirsc,
				   *basec;
		char	   *json;

		ours = diff_table_schema("WORKING", table);
		if (!ours->present)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("table not found: %s", table)));
		if (pending_merge_commits(&oursc, &theirsc, &basec, &json) && !conflicts_merged(json, table))
		{
			base = diff_table_at(child(basec, 1), table, false);
			theirs = diff_table_at(child(theirsc, 1), table, false);
		}
		else
			base = theirs = ours;
	}
	return conflict_descriptor(base, ours, theirs);
}

/* ---- dolt_conflicts_<t> ---- */

Datum
dolt_conflicts_table(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			nest = begin_api();
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bytea	   *oursc,
			   *theirsc,
			   *basec;
	char	   *json;
	DiffTable  *base,
			   *ours,
			   *theirs;
	int			i;
	ConflictEmit e;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	if (!pending_merge_commits(&oursc, &theirsc, &basec, &json))
	{
		end_api(nest);
		return (Datum) 0;
	}
	if (conflicts_merged(json, name))
		base = ours = theirs = diff_table_schema("WORKING", name);
	else
	{
		base = diff_table_at(child(basec, 1), name, false);
		ours = diff_table_at(child(oursc, 1), name, false);
		theirs = diff_table_at(child(theirsc, 1), name, false);
	}
	e.rsinfo = rsinfo;
	e.root_ish = hash_format(theirsc);
	e.base = base;
	e.ours = ours;
	e.theirs = theirs;
	run("SELECT decode(v->>'key','hex'),decode(v->>'base','hex'),decode(v->>'ours','hex'),decode(v->>'theirs','hex') FROM jsonb_array_elements($1::jsonb->'conflicts') v WHERE v->>'table'=$2 AND coalesce((v->>'whole_table')::boolean,false)=false", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), CStringGetTextDatum(name)), NULL, true);
	for (i = 0; i < SPI_processed; i++)
	{
		ConflictRow row = {0};
		bytea	   *values[4];
		int			k;
		SPITupleTable *tuptable = SPI_tuptable;

		for (k = 0; k < 4; k++)
		{
			bool		isnull;
			Datum		d = SPI_getbinval(tuptable->vals[i], tuptable->tupdesc, k + 1, &isnull);

			values[k] = isnull ? NULL : DatumGetByteaPCopy(d);
		}
		row.key = DG_SLICE((uint8 *) VARDATA_ANY(values[0]), VARSIZE_ANY_EXHDR(values[0]));
		for (k = 1; k < 4; k++)
		{
			PLValue    *target = k == 1 ? &row.base : k == 2 ? &row.ours : &row.theirs;
			uint64	   *count = k == 1 ? &row.base_count : k == 2 ? &row.our_count : &row.their_count;

			if (!values[k])
				continue;
			target->present = true;
			target->bytes = DG_SLICE((uint8 *) VARDATA_ANY(values[k]), VARSIZE_ANY_EXHDR(values[k]));
			*count = 1;
			if (!(ours->present ? ours : theirs)->keyed)
			{
				*count = pl_read64(target->bytes.data);
				target->bytes.data += 8;
				target->bytes.len -= 8;
			}
		}
		emit_conflict_row(base, ours, theirs, &row, &e);
		SPI_tuptable = tuptable;
	}
	end_api(nest);
	return (Datum) 0;
}

/* Rewrite the pending merge object without the given table's entries. */
static void
clear_merge_entries(const char *name, const char *array)
{
	VersionRef	pending = read_ref(branch_ref("merge"));
	char	   *json,
			   *updated;

	if (!pending.target)
		return;
	json = metadata(pending.target, "merge");
	run("SELECT jsonb_set($1::jsonb,ARRAY[$2],coalesce((SELECT jsonb_agg(v) FROM jsonb_array_elements($1::jsonb->$2) v WHERE v->>'table'<>$3),'[]'::jsonb))::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), CStringGetTextDatum(array), CStringGetTextDatum(name)), NULL, true);
	updated = strcell(0, 1);
	move_ref(branch_ref("merge"), pending, updated_merge(pending.target, updated));
	advance_snapshot();
}

/* The clearing functions stand in for DELETE and return no rows. */
Datum
dolt_conflicts_clear(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	lock_workspace();
	clear_merge_entries(text_to_cstring(PG_GETARG_TEXT_PP(0)), "conflicts");
	end_api(nest);
	return (Datum) 0;
}

Datum
dolt_constraint_violations_clear(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	lock_workspace();
	clear_merge_entries(text_to_cstring(PG_GETARG_TEXT_PP(0)), "violations");
	end_api(nest);
	return (Datum) 0;
}

/* ---- dolt_constraint_violations_<t> ---- */

static TupleDesc
violation_layout(const char *table)
{
	DiffTable  *t = diff_table_schema("WORKING", table);
	TupleDesc	out;
	int			at = 0;

	if (!t->present)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("table not found: %s", table)));
	out = CreateTemplateTupleDesc(t->desc->natts + 3);
	add_column(out, &at, "from_root_ish", TEXTOID);
	add_column(out, &at, "violation_type", TEXTOID);
	add_prefixed_columns(out, &at, "", t->desc);
	add_column(out, &at, "violation_info", JSONOID);
	return out;
}

Datum
dolt_constraint_violations_table(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			nest = begin_api();
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bytea	   *oursc,
			   *theirsc,
			   *basec;
	char	   *json;
	DiffTable  *t;
	int			i;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	if (!pending_merge_commits(&oursc, &theirsc, &basec, &json))
	{
		end_api(nest);
		return (Datum) 0;
	}
	t = diff_table_schema("WORKING", name);
	run("SELECT v->>'type',decode(v->>'row','hex'),CASE WHEN v->>'type'='foreign key' THEN v->>'info_text' ELSE format('{\"Columns\":%s,\"Name\":%s}',(v->'info'->'Columns')::text,to_json(v->'info'->>'Name')::text) END FROM jsonb_array_elements($1::jsonb->'violations') v WHERE v->>'table'=$2", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), CStringGetTextDatum(name)), NULL, true);
	for (i = 0; i < SPI_processed; i++)
	{
		SPITupleTable *tuptable = SPI_tuptable;
		TupleDesc	out = rsinfo->setDesc;
		Datum	   *values = palloc0(sizeof(Datum) * out->natts);
		bool	   *nulls = palloc(sizeof(bool) * out->natts);
		bool		isnull;
		bytea	   *rowbytes = DatumGetByteaPCopy(SPI_getbinval(tuptable->vals[i], tuptable->tupdesc, 2, &isnull));
		char	   *type = SPI_getvalue(tuptable->vals[i], tuptable->tupdesc, 1),
				   *info = SPI_getvalue(tuptable->vals[i], tuptable->tupdesc, 3);

		memset(nulls, true, out->natts);
		values[0] = CStringGetTextDatum(hash_format(theirsc));
		values[1] = CStringGetTextDatum(type);
		nulls[0] = nulls[1] = false;
		place_values(out, 2, t->desc, t, DG_VALUE(true, DG_SLICE((uint8 *) VARDATA_ANY(rowbytes), VARSIZE_ANY_EXHDR(rowbytes))), values, nulls);
		values[out->natts - 1] = DirectFunctionCall1(json_in, CStringGetDatum(info));
		nulls[out->natts - 1] = false;
		tuplestore_putvalues(rsinfo->setResult, out, values, nulls);
		SPI_tuptable = tuptable;
	}
	end_api(nest);
	return (Datum) 0;
}

/* ---- dolt_preview_merge_conflicts and its summary ---- */

static char **
preview_arguments(FunctionCallInfo fcinfo, const char *fn, int expected)
{
	Datum	   *values;
	bool	   *nulls;
	int			count,
				i;
	char	  **args;

	if (PG_NARGS() == 0 || PG_ARGISNULL(0))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("function '%s' expected %d arguments, 0 received", fn, expected)));
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(0), TEXTOID, -1, false, TYPALIGN_INT, &values, &nulls, &count);
	if (count != expected)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("function '%s' expected %d arguments, %d received", fn, expected, count)));
	args = palloc(sizeof(char *) * count);
	for (i = 0; i < count; i++)
	{
		if (nulls[i])
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("Invalid argument to %s: NULL", fn)));
		args[i] = TextDatumGetCString(values[i]);
	}
	return args;
}

static bytea *
preview_branch(const char *name, bool summary)
{
	VersionRef	ref;

	if (!*name)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg(summary ? "branch name cannot be empty" : "string is not a valid branch or hash")));
	ref = read_ref(psprintf("heads/%s", name));
	if (ref.target)
		return ref.target;
	if (commit_prefix(name) != NULL)
		return resolve(name);
	ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("branch not found: %s", name)));
	return NULL;
}

/*
 * How table |name|'s definitions merge: 1 if they conflict, 0 if they are the
 * same on all sides or one side's is taken, and -1 if they merge column by
 * column, so the sides' rows are laid out differently.
 */
static int
schema_conflict(bytea *base, bytea *ours, bytea *theirs, const char *name)
{
	int			counts[3];
	Member	   *sides[3] = {
		database_members(base, &counts[0]),
		database_members(ours, &counts[1]),
		database_members(theirs, &counts[2]),
	};
	char	   *defs[3];
	StringInfoData description;

	for (int side = 0; side < 3; side++)
		defs[side] = member_definition(merge_member(sides[side], counts[side], "table", name));
	if (json_equal(defs[1], defs[2]) || json_equal(defs[0], defs[1]) || json_equal(defs[0], defs[2]))
		return 0;
	initStringInfo(&description);
	if (!defs[1] || !defs[2])
		return 1;
	return merge_table_definition(name, defs[0], defs[1], defs[2], &description) ? -1 : 1;
}

static void
count_conflict(DiffTable *base, DiffTable *ours, DiffTable *theirs, ConflictRow *row, void *arg)
{
	(*(int64 *) arg)++;
}

Datum
dolt_preview_merge_conflicts_summary(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	char	  **args = preview_arguments(fcinfo, "dolt_preview_merge_conflicts_summary", 2);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bytea	   *oursc = preview_branch(args[0], true),
			   *theirsc = preview_branch(args[1], true),
			   *basec = merge_base(oursc, theirsc);
	char	   *oursjson = metadata(child(oursc, 1), "database"),
			   *theirsjson = metadata(child(theirsc, 1), "database");
	char	  **names;
	int			n,
				i;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	/* Every table of either side: identical keyless cardinality changes still conflict. */
	run("SELECT name FROM (SELECT DISTINCT v->>'name' AS name FROM (SELECT v FROM jsonb_array_elements($1::jsonb->'members') v UNION ALL SELECT v FROM jsonb_array_elements($2::jsonb->'members') v) x WHERE v->>'kind'='table') d ORDER BY name COLLATE \"C\"", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(oursjson), CStringGetTextDatum(theirsjson)), NULL, true);
	n = SPI_processed;
	names = palloc(sizeof(char *) * Max(n, 1));
	for (i = 0; i < n; i++)
		names[i] = strcell(i, 1);
	for (i = 0; i < n; i++)
	{
		Datum		values[3];
		bool		nulls[3] = {false, false, false};
		int64		conflicts = 0;

		int			state = schema_conflict(child(basec, 1), child(oursc, 1), child(theirsc, 1), names[i]);

		/* Rows laid out by different definitions are not previewed. */
		if (state < 0)
			continue;
		if (state > 0)
		{
			values[0] = CStringGetTextDatum(names[i]);
			nulls[1] = true;
			values[1] = (Datum) 0;
			values[2] = DirectFunctionCall1(int8_numeric, Int64GetDatum(1));
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
			continue;
		}
		{
			DiffTable  *base = diff_table_at(child(basec, 1), names[i], true),
					   *ours = diff_table_at(child(oursc, 1), names[i], true),
					   *theirs = diff_table_at(child(theirsc, 1), names[i], true);

			if (ours->present && theirs->present)
				conflict_rows(base, ours, theirs, count_conflict, &conflicts);
			diff_table_close(base);
			diff_table_close(ours);
			diff_table_close(theirs);
		}
		if (!conflicts)
			continue;
		values[0] = CStringGetTextDatum(names[i]);
		values[1] = DirectFunctionCall1(int8_numeric, Int64GetDatum(conflicts));
		values[2] = DirectFunctionCall1(int8_numeric, Int64GetDatum(0));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	end_api(nest);
	return (Datum) 0;
}

Datum
dolt_preview_merge_conflicts(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	char	  **args = preview_arguments(fcinfo, "dolt_preview_merge_conflicts", 3);
	bytea	   *oursc = preview_branch(args[0], false),
			   *theirsc = preview_branch(args[1], false),
			   *basec = merge_base(oursc, theirsc);
	char	   *oursjson = metadata(child(oursc, 1), "database"),
			   *theirsjson = metadata(child(theirsc, 1), "database");
	char	   *name = resolve_table_argument(args[2], oursjson, theirsjson, true);
	DiffTable  *base,
			   *ours,
			   *theirs;
	ConflictEmit e;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	if (schema_conflict(child(basec, 1), child(oursc, 1), child(theirsc, 1), name) > 0)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("schema conflicts found: 1")));
	base = diff_table_at(child(basec, 1), name, true);
	ours = diff_table_at(child(oursc, 1), name, true);
	theirs = diff_table_at(child(theirsc, 1), name, true);
	e.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	e.root_ish = hash_format(theirsc);
	e.base = base;
	e.ours = ours;
	e.theirs = theirs;
	if (ours->present && theirs->present)
		conflict_rows(base, ours, theirs, emit_conflict_row, &e);
	diff_table_close(base);
	diff_table_close(ours);
	diff_table_close(theirs);
	end_api(nest);
	return (Datum) 0;
}

/* ---- dolt_verify_constraints ----
 * Unique and primary-key constraints are enforced by the storage engine for
 * every working table; foreign keys and checks are not captured yet. */
Datum
dolt_verify_constraints(PG_FUNCTION_ARGS)
{
	int			n,
				i;
	char	  **args = arguments(fcinfo, &n);

	for (i = 0; i < n; i++)
		if (strcmp(args[i], "--all") && strcmp(args[i], "-a") && strcmp(args[i], "--output-only") && strcmp(args[i], "-o") && args[i][0] == '-')
			unsupported(args[i]);
	PG_RETURN_INT64(0);
}

Datum
dolt_verify_constraints_noargs(PG_FUNCTION_ARGS)
{
	return dolt_verify_constraints(fcinfo);
}
