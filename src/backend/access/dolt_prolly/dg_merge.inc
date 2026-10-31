#define DG_SLICE(...) ((PLSlice) {__VA_ARGS__})
#define DG_VALUE(...) ((PLValue) {__VA_ARGS__})
#define DG_ENTRY(...) ((PLEntry) {__VA_ARGS__})
/* Branch and merge commands share the manifest command helpers in dg_version.c. */
static void
advance_snapshot(void)
{
	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
}

static Datum
command_record(FunctionCallInfo fcinfo, Datum *values, bool *nulls)
{
	TupleDesc	desc;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "record result required");
	return HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls));
}

/* Preserve definitions, ignoring only table/index contents. */
static void
same_schema(const char *left, const char *right)
{
	run("SELECT (SELECT coalesce(jsonb_agg(v-'content' ORDER BY v->>'kind',coalesce(v->>'name',v->'definition'->>'name')),'[]') FROM jsonb_array_elements($1::jsonb->'members') v) = (SELECT coalesce(jsonb_agg(v-'content' ORDER BY v->>'kind',coalesce(v->>'name',v->'definition'->>'name')),'[]') FROM jsonb_array_elements($2::jsonb->'members') v)", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(left), CStringGetTextDatum(right)), NULL, true);
	if (!DatumGetBool(cell(0, 1)))
		unsupported("schema checkout/merge: branch views currently require identical definitions");
}

/* Serialize administrative publication, and coordinate with the DDL guard.
 * Ordinary DML continues to use relation writer locks, not this metadata lock. */
static void
lock_workspace(void)
{
	Oid		   *ids;
	int			n,
				i;

	LockDatabaseObject(DoltRefRelationId, 0, 0, ExclusiveLock);
	run("SELECT DISTINCT bindrelid FROM pg_dolt_binding WHERE bindbranch=$1 ORDER BY bindrelid", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(dg_current_branch())), NULL, true);
	n = SPI_processed;
	ids = palloc(sizeof(Oid) * Max(n, 1));
	for (i = 0; i < n; i++)
		ids[i] = DatumGetObjectId(cell(i, 1));
	for (i = 0; i < n; i++)
		dg_storage_lock_branch(ids[i], dg_current_branch(), ExclusiveLock);

	/* Writers lock the branch's relations by durable OID; wait them out. */
	run("SELECT identoid FROM pg_dolt_identity WHERE identbranch=$1 AND identkind='r' ORDER BY identoid", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(dg_current_branch())), NULL, true);
	n = SPI_processed;
	ids = palloc(sizeof(Oid) * Max(n, 1));
	for (i = 0; i < n; i++)
		ids[i] = DatumGetObjectId(cell(i, 1));
	for (i = 0; i < n; i++)
		LockRelationOid(ids[i], ExclusiveLock);
	PopActiveSnapshot();
	PushCopiedSnapshot(GetLatestSnapshot());
}

Datum
active_branch(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(dg_current_branch()));
}

Datum
dolt_checkout(PG_FUNCTION_ARGS)
{
	int			n,
				nest;
	char	  **args = arguments(fcinfo, &n),
			   *name;
	bytea	   *commit;
	VersionRef	ref;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	bool		already = false;

	standalone(fcinfo, true);
	nest = begin_api();
	lock_workspace();
	ensure_repository();
	if (n >= 2 && n <= 3 && !strcmp(args[0], "-b"))
	{
		name = args[1];
		valid_name(name);
		commit = resolve(n == 3 ? args[2] : "HEAD");
		ref = read_ref(psprintf("heads/%s", name));
		if (ref.target)
			ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("branch already exists: %s", name)));
		move_ref(psprintf("heads/%s", name), ref, commit);
		move_ref(psprintf("working/%s", name), empty_ref, child(commit, 1));
		move_ref(psprintf("staged/%s", name), empty_ref, child(commit, 1));
		advance_snapshot();
		replace_working(name, child(commit, 1), NIL);
	}
	else if (n >= 1 && args[0][0] != '-')
	{
		name = args[0];
		ref = read_ref(psprintf("heads/%s", name));
		commit = ref.target;
		if (n == 1 && commit && !strcmp(name, dg_current_branch()))
			already = true;
		if (!commit || n > 1)
		{
			/*
			 * Not a branch: a tag or commit would detach HEAD; otherwise
			 * tables.
			 */
			if (n == 1 && !*name)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("error: cannot checkout empty string")));
			if (n == 1 && (read_ref(psprintf("tags/%s", name)).target || commit_prefix(name) != NULL))
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("dolt does not support a detached head state. To create a branch at this tag, run: dolt checkout %s -b {new_branch_name}", name)));
			checkout_tables(args, n);
			values[0] = Int64GetDatum(0);
			values[1] = CStringGetTextDatum("");
			end_api(nest);
			return command_record(fcinfo, values, nulls);
		}
	}
	else
	{
		unsupported("checkout supports branch, -b branch [start-point], or table names");
		return 0;
	}

	{
		bytea	   *target = read_ref(psprintf("working/%s", name)).target;

		binding_prepare(name, target ? target : child(commit, 1), true);
	}
	dg_select_branch(name);
	values[0] = Int64GetDatum(0);
	values[1] = CStringGetTextDatum(psprintf(already ? "Already on branch '%s'" : "Switched to branch '%s'", name));
	end_api(nest);
	return command_record(fcinfo, values, nulls);
}

static bytea *
merge_parent(void)
{
	VersionRef	pending = read_ref(branch_ref("merge"));
	char	   *json;

	if (!pending.target)
		return NULL;
	json = metadata(pending.target, "merge");
	run("SELECT jsonb_array_length($1::jsonb->'conflicts')+coalesce(jsonb_array_length($1::jsonb->'schema_conflicts'),0),coalesce(jsonb_array_length($1::jsonb->'violations'),0)", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json)), NULL, true);
	if (DatumGetInt32(cell(0, 1)))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("unresolved merge conflicts")));
	if (DatumGetInt32(cell(0, 2)))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("unresolved constraint violations"), errhint("Delete the rows of dolt_constraint_violations_<table> after fixing them.")));
	if (!strcmp(json_value(json, "squash") ? json_value(json, "squash") : "false", "true"))
		return NULL;
	return child(pending.target, 2);
}
static void
finish_merge(void)
{
	VersionRef	pending = read_ref(branch_ref("merge"));

	if (pending.target)
		move_ref(branch_ref("merge"), pending, NULL);
}

static bytea *
merge_base(bytea *ours, bytea *theirs)
{
	/*
	 * Common ancestors that are ancestors of another common candidate are not
	 * bases.
	 */
	run("WITH RECURSIVE a(h) AS (SELECT $1::bytea UNION SELECT unnest(o.objchildren[2:]) FROM a JOIN pg_dolt_object_visible o ON o.objhash=a.h), b(h) AS (SELECT $2::bytea UNION SELECT unnest(o.objchildren[2:]) FROM b JOIN pg_dolt_object_visible o ON o.objhash=b.h), common AS (SELECT h FROM a INTERSECT SELECT h FROM b), ancestors(origin,h) AS (SELECT c.h,unnest(o.objchildren[2:]) FROM common c JOIN pg_dolt_object_visible o ON o.objhash=c.h UNION SELECT x.origin,unnest(o.objchildren[2:]) FROM ancestors x JOIN pg_dolt_object_visible o ON o.objhash=x.h) SELECT h FROM common WHERE h NOT IN (SELECT h FROM ancestors)", 2, VC_TYPES(BYTEAOID, BYTEAOID), VC_ARGS(PointerGetDatum(ours), PointerGetDatum(theirs)), NULL, true);
	if (SPI_processed != 1)
		unsupported("merge requires a unique common ancestor (recursive criss-cross merge is not implemented)");
	return (bytea *) DatumGetPointer(cell(0, 1));
}

static bytea *
table_chunk(bytea *db, const char *name)
{
	run("SELECT o.objchildren[2] FROM pg_dolt_object_visible db CROSS JOIN LATERAL unnest(db.objchildren) ch JOIN pg_dolt_object_visible o ON o.objhash=ch WHERE db.objhash=$1 AND o.objkind='table' AND convert_from(o.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(name)), NULL, true);
	if (SPI_processed != 1)
		elog(ERROR, "missing retained table: %s", name);
	return (bytea *) DatumGetPointer(cell(0, 1));
}

static int
entry_compare(const void *a, const void *b)
{
	return pl_compare(((const PLEntry *) a)->key, ((const PLEntry *) b)->key);
}

static PLSlice
pack_merge_fields(PLValue * fields, int n)
{
	uint8	   *bytes;
	size_t		len;
	PLSlice		result;

	dg_check(pl_tuple_pack(fields, n, &bytes, &len));
	result.data = palloc(len);
	result.len = len;
	memcpy((void *) result.data, bytes, len);
	free(bytes);
	return result;
}
static bool
equal_value(PLValue a, PLValue b)
{
	return a.present == b.present && (!a.present || !pl_compare(a.bytes, b.bytes));
}

/* Logical PK keys are separate from canonical row hashes. */
static PLTree
logical_rows(Relation rel, const PLTree * content, bool *keyed)
{
	Oid			pk = RelationGetPrimaryKeyIndex(rel, false);
	Relation	index;
	DGRows		rows;
	size_t		i;
	int			j,
				nkeys;
	PLEntry    *items;
	PLTree		result;

	*keyed = OidIsValid(pk);
	if (!*keyed)
		return *content;
	index = rel->rd_catalogview ? dg_storage_index(rel, pk) : index_open(pk, AccessShareLock);
	nkeys = index->rd_index->indnkeyatts;
	rows = dg_rows(content);
	items = palloc(sizeof(*items) * Max(rows.count, 1));
	for (i = 0; i < rows.count; i++)
	{
		PLValue    *fields = palloc0(sizeof(*fields) * nkeys);
		PLSlice		row = {rows.items[i].value.data + 8, rows.items[i].value.len - 8};

		if (pl_read64(rows.items[i].value.data) != 1)
			elog(ERROR, "duplicate primary key occurrence");
		for (j = 0; j < nkeys; j++)
			dg_check(pl_tuple_get(row, index->rd_index->indkey.values[j] - 1, &fields[j]));
		items[i].key = pack_merge_fields(fields, nkeys);
		items[i].value = row;
	}
	qsort(items, rows.count, sizeof(*items), entry_compare);
	dg_check(pl_build(content->store, items, rows.count, &result));
	if (!rel->rd_catalogview)
		index_close(index, AccessShareLock);
	return result;
}
static PLTree
canonical_rows(const PLTree * logical, bool keyed)
{
	DGRows		rows;
	size_t		i;
	PLTree		result;
	PLEntry    *items;

	if (!keyed)
		return *logical;
	rows = dg_rows(logical);
	items = palloc(sizeof(*items) * Max(rows.count, 1));
	for (i = 0; i < rows.count; i++)
	{
		PLHash		hash = dg_rowhash(rows.items[i].value);
		uint8	   *key = palloc(PL_HASH_SIZE),
				   *value = palloc(8 + rows.items[i].value.len);

		memcpy(key, hash.bytes, PL_HASH_SIZE);
		pl_write64(value, 1);
		memcpy(value + 8, rows.items[i].value.data, rows.items[i].value.len);
		items[i].key = DG_SLICE(key, PL_HASH_SIZE);
		items[i].value = DG_SLICE(value, 8 + rows.items[i].value.len);
	}
	qsort(items, rows.count, sizeof(*items), entry_compare);
	dg_check(pl_build(logical->store, items, rows.count, &result));
	return result;
}
static void
json_bytes(StringInfo json, PLValue value)
{
	if (!value.present)
		appendStringInfoString(json, "null");
	else
	{
		bytea	   *b = palloc(VARHDRSZ + value.bytes.len);

		SET_VARSIZE(b, VARHDRSZ + value.bytes.len);
		memcpy(VARDATA(b), value.bytes.data, value.bytes.len);
		escape_json(json, TextDatumGetCString(DirectFunctionCall2(binary_encode, PointerGetDatum(b), CStringGetTextDatum("hex"))));
	}
}
/* Whether conflicting rows are laid out in the merged definition, not their commits'. */
static bool conflict_merged_layout;

static void
record_conflict(StringInfo conflicts, const char *name, PLSlice key, PLValue base, PLValue ours, PLValue theirs)
{
	if (conflicts->len)
		appendStringInfoChar(conflicts, ',');
	appendStringInfoString(conflicts, "{\"table\":");
	escape_json(conflicts, name);
	appendStringInfoString(conflicts, ",\"key\":");
	json_bytes(conflicts, DG_VALUE(true, key));
	appendStringInfoString(conflicts, ",\"base\":");
	json_bytes(conflicts, base);
	appendStringInfoString(conflicts, ",\"ours\":");
	json_bytes(conflicts, ours);
	appendStringInfoString(conflicts, ",\"theirs\":");
	json_bytes(conflicts, theirs);
	if (conflict_merged_layout)
		appendStringInfoString(conflicts, ",\"layout\":\"merged\"");
	appendStringInfoChar(conflicts, '}');
}

/* JSON objects merge recursively by member. Arrays/scalars are atomic values;
 * SQL NULL (absent field) remains distinct from a JSON null document. */
static bool
json_equal(const char *a, const char *b)
{
	return a && b ? strcmp(a, b) == 0 : a == b;
}
static bool
merge_json_object(const char *base, const char *ours, const char *theirs, char **result)
{
	char	  **keys,
			  **b,
			  **o,
			  **t;
	int			n,
				i;
	StringInfoData out;

	check_stack_depth();
	CHECK_FOR_INTERRUPTS();
	if (json_equal(ours, theirs) || json_equal(base, theirs))
	{
		*result = (char *) ours;
		return true;
	}
	if (json_equal(base, ours))
	{
		*result = (char *) theirs;
		return true;
	}
	if (!base || !ours || !theirs || *base != '{' || *ours != '{' || *theirs != '{')
		return false;
	run("WITH keys AS (SELECT jsonb_object_keys($1::jsonb) k UNION SELECT jsonb_object_keys($2::jsonb) UNION SELECT jsonb_object_keys($3::jsonb)) SELECT k,($1::jsonb->k)::text,($2::jsonb->k)::text,($3::jsonb->k)::text FROM keys ORDER BY k COLLATE \"C\"", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(base), CStringGetTextDatum(ours), CStringGetTextDatum(theirs)), NULL, true);
	n = SPI_processed;
	keys = palloc(sizeof(char *) * Max(n, 1));
	b = palloc(sizeof(char *) * Max(n, 1));
	o = palloc(sizeof(char *) * Max(n, 1));
	t = palloc(sizeof(char *) * Max(n, 1));
	for (i = 0; i < n; i++)
	{
		keys[i] = strcell(i, 1);
		b[i] = strcell(i, 2);
		o[i] = strcell(i, 3);
		t[i] = strcell(i, 4);
	}
	initStringInfo(&out);
	appendStringInfoChar(&out, '{');
	for (i = 0; i < n; i++)
	{
		char	   *value;

		if (!merge_json_object(b[i], o[i], t[i], &value))
			return false;
		if (!value)
			continue;
		if (out.len > 1)
			appendStringInfoChar(&out, ',');
		escape_json(&out, keys[i]);
		appendStringInfoChar(&out, ':');
		appendStringInfoString(&out, value);
	}
	appendStringInfoChar(&out, '}');
	*result = out.data;
	return true;
}
static bool
merge_json_field(Oid type, PLValue * f, PLValue * result)
{
	char	   *text[3],
			   *merged;
	int			i;
	Size		prefix = type == JSONBOID ? 5 : 4;
	uint8	   *bytes;

	for (i = 0; i < 3; i++)
	{
		if (!f[i].present)
			return false;
		if (f[i].bytes.len < prefix || memcmp(f[i].bytes.data, "DGB1", 4))
			elog(ERROR, "invalid JSON field encoding");
		text[		i] = pnstrdup((const char *) f[i].bytes.data + prefix, f[i].bytes.len - prefix);
	}
	run("SELECT $1::jsonb::text,$2::jsonb::text,$3::jsonb::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(text[0]), CStringGetTextDatum(text[1]), CStringGetTextDatum(text[2])), NULL, true);
	for (i = 0; i < 3; i++)
		text[		i] = strcell(0, i + 1);

	if (!merge_json_object(text[0], text[1], text[2], &merged))
		return false;
	bytes = palloc(prefix + strlen(merged));
	memcpy(bytes, "DGB1", 4);
	if (type == JSONBOID)
		bytes[4] = 1;
	memcpy(bytes + prefix, merged, strlen(merged));
	*result = DG_VALUE(true, DG_SLICE(bytes, prefix + strlen(merged)));
	return true;
}

/* Walk all keys, including identical keyless changes: those still conflict. */
/*
 * Merge rows |base|, |ours| and |theirs| of |rel|. When the sides' rows were
 * laid out again from other definitions, |lacks|[side][column] says which
 * columns a side's definition lacked: a row only that side has gets their
 * defaults. Rows hold |width| fields: past those of |rel|, the columns the
 * merge drops, which only a conflict could keep.
 */
static PLSlice
strip_row(PLSlice row, int width, int natts)
{
	PLValue    *fields;

	if (width == natts)
		return row;
	fields = palloc_array(PLValue, Max(natts, 1));
	for (int k = 0; k < natts; k++)
		dg_check(pl_tuple_get(row, k, &fields[k]));
	return pack_merge_fields(fields, natts);
}

static PLValue
strip_value(PLValue value, int width, int natts)
{
	if (value.present)
		value.bytes = strip_row(value.bytes, width, natts);
	return value;
}

static PLTree
merge_rows(Relation rel, PLTree base, PLTree ours, PLTree theirs, const char *name, StringInfo conflicts, bool **lacks,
		   int width)
{
	bool		keyed;
	PLTree		logical[3],
				result;
	DGRows		rows[3];
	size_t		at[3] = {0}, used = 0, capacity;
	PLEntry    *out;
	int			j;

	logical[0] = logical_rows(rel, &base, &keyed);
	logical[1] = logical_rows(rel, &ours, &keyed);
	logical[2] = logical_rows(rel, &theirs, &keyed);
	for (j = 0; j < 3; j++)
		rows[j] = dg_rows(&logical[j]);
	capacity = rows[0].count + rows[1].count + rows[2].count;
	out = palloc(sizeof(*out) * Max(capacity, 1));
	for (;;)
	{
		PLSlice		key = {0};
		PLValue		v[3] = {{0}}, value = {0};
		bool		conflict = false;
		int			source = 0;

		CHECK_FOR_INTERRUPTS();
		for (j = 0; j < 3; j++)
			if (at[j] < rows[j].count && (!key.data || pl_compare(rows[j].items[at[j]].key, key) < 0))
				key = rows[j].items[at[j]].key;
		if (!key.data)
			break;
		for (j = 0; j < 3; j++)
			if (at[j] < rows[j].count && !pl_compare(rows[j].items[at[j]].key, key))
			{
				v[j].present = true;
				v[j].bytes = rows[j].items[at[j]++].value;
			}
		if (!keyed)
		{
			/*
			 * Dolt cannot infer identity between indistinguishable
			 * occurrences. Both sides changing cardinality conflicts, even to
			 * equal counts.
			 */
			if (equal_value(v[0], v[1]))
				value = v[2], source = 2;
			else if (equal_value(v[0], v[2]))
				value = v[1], source = 1;
			else
				conflict = true;
		}
		else if (equal_value(v[1], v[2]))
			value = v[1];
		else if (equal_value(v[0], v[1]))
			value = v[2], source = 2;
		else if (equal_value(v[0], v[2]))
			value = v[1], source = 1;
		else if (v[0].present && v[1].present && v[2].present)
		{
			int			n = width,
						k;
			PLValue    *fields = palloc(sizeof(*fields) * n);

			for (k = 0; k < n; k++)
			{
				PLValue		f[3];

				for (j = 0; j < 3; j++)
					dg_check(pl_tuple_get(v[j].bytes, k, &f[j]));
				if (equal_value(f[1], f[2]) || equal_value(f[0], f[2]))
					fields[k] = f[1];
				else if (equal_value(f[0], f[1]))
					fields[k] = f[2];
				else if (k < RelationGetDescr(rel)->natts &&
						 (TupleDescAttr(RelationGetDescr(rel), k)->atttypid == JSONOID || TupleDescAttr(RelationGetDescr(rel), k)->atttypid == JSONBOID) &&
						 merge_json_field(TupleDescAttr(RelationGetDescr(rel), k)->atttypid, f, &fields[k]))
					continue;
				else
				{
					conflict = true;
					break;
				}
			}
			if (!conflict)
				value = DG_VALUE(true, pack_merge_fields(fields, n));
		}
		else
			conflict = true;
		for (j = 0; j < 3; j++)
			v[j] = strip_value(v[j], width, RelationGetDescr(rel)->natts);
		value = strip_value(value, width, RelationGetDescr(rel)->natts);
		if (conflict)
		{
			record_conflict(conflicts, name, key, v[0], v[1], v[2]);
			value = v[1];
		}
		/* A row the other side never had takes defaults for the columns its side lacked. */
		if (lacks && source && value.present && !v[3 - source].present && !v[0].present)
		{
			bool		fill = false;

			for (j = 0; j < RelationGetDescr(rel)->natts; j++)
				fill |= lacks[source][j];
			if (fill)
				value.bytes = fill_row(rel, value.bytes, lacks[source]);
		}
		if (value.present)
			out[used++] = DG_ENTRY(key, value.bytes);
	}
	dg_check(pl_build(ours.store, out, used, &result));
	return canonical_rows(&result, keyed);
}

/* The supported unique opclasses have byte-canonical equality. Validate before
 * publishing so a cross-branch duplicate is a conflict, not SQL ERROR. */
static bool
unique_contents(Relation rel, const PLTree * contents)
{
	List	   *indexes = RelationGetIndexList(rel);
	ListCell   *lc;
	DGRows		rows = dg_rows(contents);

	foreach(lc, indexes)
	{
		Relation	idx = rel->rd_catalogview ? dg_storage_index(rel, lfirst_oid(lc)) : index_open(lfirst_oid(lc), AccessShareLock);
		size_t		i,
					used = 0;
		PLEntry    *keys;

		if (!idx->rd_index->indisunique)
		{
			if (!rel->rd_catalogview)
				index_close(idx, AccessShareLock);
			continue;
		}
		keys = palloc(sizeof(*keys) * Max(rows.count, 1));
		for (i = 0; i < rows.count; i++)
		{
			bool hasnull;
			StringInfoData key;
			PLSlice row = {rows.items[i].value.data + 8, rows.items[i].value.len - 8};

			initStringInfo(&key);
			if (!dg_index_row_key(rel, idx, row, &key, &hasnull) ||
				(hasnull && !idx->rd_index->indnullsnotdistinct))
				continue;
			if (pl_read64(rows.items[i].value.data) > 1)
			{
				if (!rel->rd_catalogview)
					index_close(idx, AccessShareLock);
				return false;
			}
			keys[used++].key = DG_SLICE(key.data, key.len);
		}
		qsort(keys, used, sizeof(*keys), entry_compare);
		for (i = 1; i < used; i++)
			if (!pl_compare(keys[i - 1].key, keys[i].key))
			{
				if (!rel->rd_catalogview)
					index_close(idx, AccessShareLock);
				return false;
			}
		if (!rel->rd_catalogview)
			index_close(idx, AccessShareLock);
	}
	return true;
}

/* Rows of |contents| that share a unique key, recorded as Dolt "unique index"
 * constraint violations: {table, type, row, info:{Columns, Name}}. */
static void
unique_violations(Relation rel, const PLTree * contents, const char *name, StringInfo violations)
{
	List	   *indexes = RelationGetIndexList(rel);
	ListCell   *lc;
	DGRows		rows = dg_rows(contents);

	foreach(lc, indexes)
	{
		Relation	idx = rel->rd_catalogview ? dg_storage_index(rel, lfirst_oid(lc)) : index_open(lfirst_oid(lc), AccessShareLock);
		int			n = idx->rd_index->indnkeyatts;
		size_t		i,
					j;
		PLEntry    *keys;
		StringInfoData info;

		if (!idx->rd_index->indisunique)
		{
			if (!rel->rd_catalogview)
				index_close(idx, AccessShareLock);
			continue;
		}
		initStringInfo(&info);
		appendStringInfoString(&info, "{\"Columns\":[");
		for (j = 0; j < n; j++)
		{
			if (j)
				appendStringInfoChar(&info, ',');
			escape_json(&info, NameStr(TupleDescAttr(RelationGetDescr(idx), j)->attname));
		}
		appendStringInfoString(&info, "],\"Name\":");
		escape_json(&info, RelationGetRelationName(idx));
		appendStringInfoChar(&info, '}');
		keys = palloc0(sizeof(*keys) * Max(rows.count, 1));
		for (i = 0; i < rows.count; i++)
		{
			PLSlice row = {rows.items[i].value.data + 8, rows.items[i].value.len - 8};
			bool hasnull;
			StringInfoData key;

			initStringInfo(&key);
			if (dg_index_row_key(rel, idx, row, &key, &hasnull) &&
				(!hasnull || idx->rd_index->indnullsnotdistinct))
				keys[i].key = DG_SLICE(key.data, key.len);
			keys[i].value = row;
		}
		for (i = 0; i < rows.count; i++)
		{
			bool		duplicate = keys[i].key.data && pl_read64(rows.items[i].value.data) > 1;

			if (keys[i].key.data)
				for (j = 0; j < rows.count && !duplicate; j++)
					if (i != j && keys[j].key.data && !pl_compare(keys[i].key, keys[j].key))
						duplicate = true;
			if (!duplicate)
				continue;
			if (violations->len)
				appendStringInfoChar(violations, ',');
			appendStringInfoString(violations, "{\"table\":");
			escape_json(violations, name);
			appendStringInfoString(violations, ",\"type\":\"unique index\",\"row\":");
			json_bytes(violations, DG_VALUE(true, keys[i].value));
			appendStringInfo(violations, ",\"info\":%s}", info.data);
		}
		if (!rel->rd_catalogview)
			index_close(idx, AccessShareLock);
	}
}

/*
 * Merge the rows of |tables|, whose definitions are the same in all three
 * states, into the working set.
 */
/*
 * A table whose rows merge: its definitions on the base, ours and theirs, if
 * any differs from its merged definition, so that their rows are laid out
 * again first. A table both sides added has no base definition.
 */
typedef struct RowMerge
{
	char	   *table;
	bool		remap;
	char	   *definitions[3];
} RowMerge;

/* Row merges of |tables|, whose definitions are the same on every side. */
static List *
row_merges(List *tables)
{
	List	   *merges = NIL;

	foreach_ptr(char, table, tables)
	{
		RowMerge   *merge = palloc0(sizeof(RowMerge));

		merge->table = table;
		merges = lappend(merges, merge);
	}
	return merges;
}

static void
install_database(bytea *base, bytea *ours, bytea *theirs, List *merges, StringInfo conflicts, StringInfo violations)
{
	foreach_ptr(RowMerge, merge, merges)
	{
		char	   *name = merge->table;
		Relation	rel = dg_catalog_write_table(dg_current_branch(), name, ACL_UPDATE);
		DGState    *state = dg_load(rel, SnapshotSelf);
		PLTree		right = dg_history_import(state->store, table_chunk(theirs, name)),
					result = right;

		if (base)
		{
			PLTree		left = dg_history_import(state->store, table_chunk(ours, name));
			PLTree		ancestor;
			bool	   *lacks[3] = {NULL};
			bool		keyed = OidIsValid(RelationGetPrimaryKeyIndex(rel, false));

			/*
			 * The base's columns the merge drops; a side may have changed
			 * them. A keyless row has no identity to conflict on.
			 */
			List	   *dropped = merge->remap && keyed ? merge_dropped_columns(rel, merge->definitions[0]) : NIL;

			if (merge->remap && !merge->definitions[0])
				dg_check(pl_build(state->store, NULL, 0, &ancestor));
			else
				ancestor = dg_history_import(state->store, table_chunk(base, name));
			if (merge->remap)
			{
				PLTree	   *sides[3] = {&ancestor, &left, &right};

				for (int side = 0; side < 3; side++)
				{
					lacks[side] = palloc0_array(bool, Max(RelationGetDescr(rel)->natts, 1));
					if (merge->definitions[side])
						*sides[side] = remap_rows(rel, *sides[side], merge->definitions[side], dropped, lacks[side]);
					if (!keyed)
						*sides[side] = fill_rows(rel, *sides[side], lacks[side]);
				}
			}
			{
				int			before = conflicts->len;

				/* Set for every merge: an error may leave it set. */
				conflict_merged_layout = merge->remap;
				result = merge_rows(rel, ancestor, left, right, name, conflicts, merge->remap && keyed ? lacks : NULL,
									RelationGetDescr(rel)->natts + list_length(dropped));
				conflict_merged_layout = false;
				if (merge->remap && violations)
					result = check_violations(rel, result, name, violations);
				if (!unique_contents(rel, &result))
				{
					/*
					 * The merged rows violate a unique constraint. Record the
					 * violating rows as Dolt constraint violations and keep
					 * the destination table, since the storage engine cannot
					 * hold duplicate unique keys.
					 */
					conflicts->len = before;
					conflicts->data[before] = '\0';
					if (violations)
						unique_violations(rel, &result, name, violations);
					result = left;
				}
			}
		}
		dg_history_install(rel, state, result);
		dg_release(state);
		dg_storage_close_relation(rel);
		UpdateActiveSnapshotCommandId();
	}
	if (base)
		validate_working_foreign_keys(dg_current_branch(), violations);
}

/* Preserve retained pre-merge workspaces when conflict metadata is edited. */
static bytea *
updated_merge(bytea *previous, const char *json)
{
	ArrayType *array;
	Datum *children;
	bool *nulls;
	int count;

	if (!object_read(previous, "merge", NULL, NULL, &array))
		elog(ERROR, "merge object is not visible");
	deconstruct_array(array, BYTEAOID, -1, false, TYPALIGN_INT, &children, &nulls, &count);
	return put("merge", json, children, count);
}

Datum
dolt_merge(PG_FUNCTION_ARGS)
{
	Capture		live;
	int			n,
				i,
				nest;
	char	  **args = arguments(fcinfo, &n),
			   *revision = NULL,
			   *message = NULL;
	bool		noff = false,
				ffonly = false,
				abort_merge = false,
				squash = false,
				nocommit = false;
	VersionRef	head,
				pending;
	bytea	   *theirs,
			   *base,
			   *oursdb,
			   *their_db,
			   *database,
			   *commit = NULL,
			   *oldstaged,
			   *newworking,
			   *newstaged;
	StringInfoData conflicts,
				violations,
				schema_conflicts,
				json;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};
	int64		ff = 0,
				conflicted = 0;
	const char *result_message = "merge successful";

	standalone(fcinfo, true);
	nest = begin_api();
	lock_workspace();
	ensure_repository();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--no-ff"))
			noff = true;
		else if (!strcmp(args[i], "--ff-only"))
			ffonly = true;
		else if (!strcmp(args[i], "--abort"))
			abort_merge = true;
		else if (!strcmp(args[i], "--squash"))
			squash = true;
		else if (!strcmp(args[i], "--no-commit"))
			nocommit = true;
		else if (!strcmp(args[i], "-m") || !strcmp(args[i], "--message"))
		{
			if (++i == n)
				unsupported("missing merge message");
			message = args[i];
		}
		else if (args[i][0] == '-' || revision)
			unsupported(args[i]);
		else
			revision = args[i];
	}
	if (noff && ffonly)
		unsupported("--no-ff with --ff-only");
	head = read_ref(branch_ref("heads"));
	pending = read_ref(branch_ref("merge"));
	if (abort_merge)
	{
		if (revision || !pending.target)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("no merge to abort")));
		database = object_child(pending.target, 4);
		oldstaged = object_child(pending.target, 5);
		if (!database)
			database = child(child(pending.target, 1), 1);
		if (!oldstaged)
			oldstaged = database;
		replace_working(dg_current_branch(), database, NIL);
		finish_merge();
		move_ref(branch_ref("working"), read_ref(branch_ref("working")), database);
		move_ref(branch_ref("staged"), read_ref(branch_ref("staged")), oldstaged);
		result_message = "merge aborted";
		goto done;
	}
	if (!revision)
		unsupported("merge requires a revision");
	if (pending.target)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("merge already in progress")));
	theirs = resolve(revision);
	base = merge_base(head.target, theirs);
	if (same(base, theirs))
	{
		result_message = "Already up to date.";
		goto done;
	}
	oursdb = child(head.target, 1);
	their_db = child(theirs, 1);
	live = capture(true);
	oldstaged = read_ref(branch_ref("staged")).target;
	if (ffonly && !same(base, head.target))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("not possible to fast-forward")));
	initStringInfo(&conflicts);
	initStringInfo(&violations);
	initStringInfo(&schema_conflicts);
	merge_databases(child(base, 1), oursdb, their_db, &conflicts, &violations, &schema_conflicts);
	database = capture(true).hash;
	newworking = carry_workspace_changes(oursdb, live.hash, database);
	newstaged = carry_workspace_changes(oursdb, oldstaged, database);

	/*
	 * --no-commit cannot stop a fast-forward (there is no new commit to
	 * create). A squash fast-forward reports its source hash without moving
	 * HEAD.
	 */
	if (same(base, head.target) && !noff)
	{
		ff = 1;
		nocommit = false;
		if (squash)
			commit = theirs;
	}
	if (conflicts.len || violations.len || schema_conflicts.len || nocommit)
	{
		initStringInfo(&json);
		appendStringInfo(&json, "{\"format\":\"DGMERGE1\",\"squash\":%s,\"source\":", squash ? "true" : "false");
		escape_json(&json, revision);
		appendStringInfo(&json, ",\"conflicts\":[%s],\"violations\":[%s],\"schema_conflicts\":[%s]}", conflicts.data, violations.data, schema_conflicts.data);
		move_ref(branch_ref("merge"), pending, put("merge", json.data, VC_ARGS(PointerGetDatum(head.target), PointerGetDatum(theirs), PointerGetDatum(base), PointerGetDatum(live.hash), PointerGetDatum(oldstaged)), 5));
		conflicted = (conflicts.len || violations.len || schema_conflicts.len) ? 1 : 0;
		result_message = conflicted ? "conflicts found" : "merge successful";
	}
	else if (!squash)
	{
		if (same(base, head.target) && !noff)
		{
			commit = theirs;
			ff = 1;
		}
		else
			commit = commit_object(database, head.target, theirs, message ? message : psprintf("Merge branch '%s'", revision), NULL, NULL, GetCurrentTimestamp());
		move_ref(branch_ref("heads"), head, commit);
	}
	if (!database_same(newworking, database))
		replace_working(dg_current_branch(), newworking, NIL);
	move_ref(branch_ref("working"), read_ref(branch_ref("working")), newworking);

	/*
	 * A conflict must be explicitly resolved before staging/committing its
	 * rows.
	 */
	if (!conflicted)
		move_ref(branch_ref("staged"), read_ref(branch_ref("staged")), newstaged);
done:
	nulls[0] = commit == NULL;
	values[0] = commit ? CStringGetTextDatum(hash_format(commit)) : (Datum) 0;
	values[1] = Int64GetDatum(ff);
	values[2] = Int64GetDatum(conflicted);
	values[3] = CStringGetTextDatum(result_message);
	end_api(nest);
	return command_record(fcinfo, values, nulls);
}

Datum
dolt_conflicts_resolve(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i,
				count;
	char	  **args = arguments(fcinfo, &n),
			   *json,
			  **records;
	VersionRef	pending;
	StringInfoData remaining,
				schema_remaining,
				updated;
	Datum		parents[3];
	bool		theirs;

	standalone(fcinfo, false);
	nest = begin_api();
	lock_workspace();
	if (n < 2 || (strcmp(args[0], "--ours") && strcmp(args[0], "--theirs")))
		unsupported("resolve requires --ours or --theirs and table names (or '.')");
	theirs = !strcmp(args[0], "--theirs");
	pending = read_ref(branch_ref("merge"));
	if (!pending.target)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("no merge in progress")));
	json = metadata(pending.target, "merge");
	for (i = 1; i < n; i++)
	{
		int			matches;

		if (!strcmp(args[i], "."))
			continue;
		if (strchr(args[i], '('))
		{
			char *resolved = resolve_object_argument(args[i],
				metadata(child(child(pending.target, 1), 1), "database"),
				metadata(child(child(pending.target, 2), 1), "database"), false);

			if (resolved)
				args[i] = resolved;
		}
		run("SELECT count(DISTINCT v->>'table')::int FROM jsonb_array_elements($1::jsonb->'conflicts' || coalesce($1::jsonb->'schema_conflicts','[]')) v WHERE v->>'table'=$2 OR CASE WHEN strpos($2,'(')>0 OR strpos(v->>'table','(')>0 THEN false ELSE array_length(parse_ident($2),1)=1 AND (parse_ident(v->>'table'))[2]=(parse_ident($2))[1] END", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), CStringGetTextDatum(args[i])), NULL, true);
		matches = DatumGetInt32(cell(0, 1));
		if (matches > 1)
			ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_ALIAS), errmsg("ambiguous conflict table: %s", args[i])));
		if (!matches)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("no conflicts for table: %s", args[i])));
	}
	for (i = 0; i < 3; i++)
		parents[i] = PointerGetDatum(child(pending.target, i + 1));
	run("SELECT v::text FROM jsonb_array_elements($1::jsonb->'conflicts') v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json)), NULL, true);
	count = SPI_processed;
	records = palloc(sizeof(*records) * Max(count, 1));
	for (i = 0; i < count; i++)
		records[i] = strcell(i, 1);
	initStringInfo(&remaining);
	for (i = 0; i < count; i++)
	{
		char	   *name = json_value(records[i], "table");
		bool		selected = false;
		int			j;

		for (j = 1; j < n; j++)
		{
			if (!strcmp(args[j], ".") || !strcmp(args[j], name) || !strcmp(args[j], strVal(llast(stringToQualifiedNameList(name, NULL)))))
				selected = true;
		}
		if (!selected)
		{
			if (remaining.len)
				appendStringInfoChar(&remaining, ',');
			appendStringInfoString(&remaining, records[i]);
			continue;
		}
		{
			Relation	rel = dg_catalog_write_table(dg_current_branch(), name, ACL_UPDATE);
			DGState    *state = dg_load(rel, SnapshotSelf);
			PLTree		result;

			if (json_value(records[i], "whole_table"))
				result = dg_history_import(state->store, table_chunk(child((bytea *) DatumGetPointer(parents[theirs ? 1 : 0]), 1), name));
			else
			{
				bool		keyed;
				PLTree		logical = logical_rows(rel, &state->content, &keyed);
				PLEdit		edit;
				char	   *value = json_value(records[i], theirs ? "theirs" : "ours");
				bytea	   *key = DatumGetByteaP(DirectFunctionCall2(binary_decode, CStringGetTextDatum(json_value(records[i], "key")), CStringGetTextDatum("hex")));

				edit.key = DG_SLICE((uint8 *) VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key));
				edit.value.present = value != NULL;
				if (value)
				{
					bytea	   *bytes = DatumGetByteaP(DirectFunctionCall2(binary_decode, CStringGetTextDatum(value), CStringGetTextDatum("hex")));

					edit.value.bytes = DG_SLICE((uint8 *) VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes));
				}
				dg_check(pl_apply(&logical, &edit, 1, &logical));
				result = canonical_rows(&logical, keyed);
			}
			dg_history_install(rel, state, result);
			dg_release(state);
			if (rel->rd_catalogview)
				dg_storage_close_relation(rel);
			else
				relation_close(rel, NoLock);
			UpdateActiveSnapshotCommandId();
		}
	}
	validate_working_foreign_keys(dg_current_branch(), NULL);

	/*
	 * A schema conflict resolves to one side's whole object: ours is already
	 * in the working set; theirs replaces it, with a table's rows. (Dolt
	 * cannot resolve these yet; ALTER TABLE on one side and merging again
	 * still works.)
	 */
	run("SELECT v::text FROM jsonb_array_elements(coalesce($1::jsonb->'schema_conflicts','[]')) v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json)), NULL, true);
	count = SPI_processed;
	records = palloc(sizeof(*records) * Max(count, 1));
	for (i = 0; i < count; i++)
		records[i] = strcell(i, 1);
	initStringInfo(&schema_remaining);
	for (i = 0; i < count; i++)
	{
		char	   *name = json_value(records[i], "table");
		bool		selected = false;

		for (int j = 1; j < n; j++)
			if (!strcmp(args[j], ".") || !strcmp(args[j], name) ||
				(!strchr(name, '(') && !strcmp(args[j], strVal(llast(stringToQualifiedNameList(name, NULL))))))
				selected = true;
		if (!selected)
		{
			if (schema_remaining.len)
				appendStringInfoChar(&schema_remaining, ',');
			appendStringInfoString(&schema_remaining, records[i]);
		}
		else if (theirs && json_value(records[i], "inheritance_group"))
		{
			JsonbValue document = json_document(records[i]), names;
			List *group = NIL;
			bytea *source = child((bytea *) DatumGetPointer(parents[1]), 1);

			json_get(&document, "inheritance_group", &names);
			for (int j = 0; j < json_length(&names); j++)
				group = lappend(group, json_item_text(&names, j));
			replace_working(dg_current_branch(), compose_database(capture(true).hash, source, group), group);
		}
		else if (theirs || !strcmp(json_value(records[i], "kind"), "sequence"))
			restore_table(child((bytea *) DatumGetPointer(parents[theirs ? 1 : 0]), 1), name);
	}
	initStringInfo(&updated);
	appendStringInfo(&updated, "{\"format\":\"DGMERGE1\",\"squash\":%s,\"source\":", json_value(json, "squash") ? json_value(json, "squash") : "false");
	escape_json(&updated, json_value(json, "source") ? json_value(json, "source") : "");
	run("SELECT coalesce(($1::jsonb->'violations')::text,'[]')", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json)), NULL, true);
	appendStringInfo(&updated, ",\"conflicts\":[%s],\"violations\":%s,\"schema_conflicts\":[%s]}", remaining.data, strcell(0, 1), schema_remaining.data);
	move_ref(branch_ref("merge"), pending, updated_merge(pending.target, updated.data));
	move_ref(branch_ref("working"), read_ref(branch_ref("working")), capture(true).hash);
	end_api(nest);
	PG_RETURN_INT64(0);
}

Datum
dolt_conflict_rows(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	VersionRef	pending = read_ref(branch_ref("merge"));
	char	   *json = pending.target ? metadata(pending.target, "merge") : "{\"conflicts\":[]}";

	run("SELECT v->>'table',decode(v->>'key','hex'),decode(v->>'base','hex'),decode(v->>'ours','hex'),decode(v->>'theirs','hex'),coalesce((v->>'whole_table')::boolean,false) FROM jsonb_array_elements($1::jsonb->'conflicts') v WHERE v->>'table'=$2 OR (parse_ident(v->>'table'))[2]=$2", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), PG_GETARG_DATUM(0)), NULL, true);
	return_rows(fcinfo, nest);
	return 0;
}
