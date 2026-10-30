/* Dolt system tables backed by set-returning functions; see doc/doltgres/system-tables.md.
 * Included by dg_version.c to share the manifest command helpers. */
#include "dg_agent_doc.h"

#define DG_DATE_SQL(field) "timestamp '2000-01-01' + (" field ")::bigint * interval '1 microsecond'"

/* Content root of a table without rows, so that an added or dropped empty
 * table is a schema change but not a data change (Dolt's TableDelta rule). */
static const char *
empty_content(void)
{
	static char *cached;

	if (!cached)
	{
		PLStore    *store = pl_store_create();
		PLTree		tree;
		bytea	   *raw = palloc(VARHDRSZ + PL_HASH_SIZE);

		if (!store)
			elog(ERROR, "cannot allocate empty content tree");
		dg_check(pl_build(store, NULL, 0, &tree));
		SET_VARSIZE(raw, VARHDRSZ + PL_HASH_SIZE);
		memcpy(VARDATA(raw), tree.root.bytes, PL_HASH_SIZE);
		cached = MemoryContextStrdup(TopMemoryContext, hash_format(raw));
		pl_store_free(store);
	}
	return cached;
}

/* Database state JSON for the branch head, staged and working sets. A branch
 * without history or staging reads as the empty database. */
static char *
head_state(void)
{
	VersionRef	head = read_ref(branch_ref("heads"));

	return head.target ? metadata(child(head.target, 1), "database") : "{\"members\":[]}";
}
static char *
staged_state(void)
{
	VersionRef	staged = read_ref(branch_ref("staged"));

	return staged.target ? metadata(staged.target, "database") : head_state();
}
static char *
working_state(void)
{
	return capture(false).json;
}

/* The live working contents differ from the persisted head state. */
static bool
state_differs(const char *left, const char *right)
{
	run("SELECT (SELECT coalesce(jsonb_agg(v ORDER BY v->>'kind',coalesce(v->>'name',v->'definition'->>'name')),'[]') FROM jsonb_array_elements($1::jsonb->'members') v) <> (SELECT coalesce(jsonb_agg(v ORDER BY v->>'kind',coalesce(v->>'name',v->'definition'->>'name')),'[]') FROM jsonb_array_elements($2::jsonb->'members') v)", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(left), CStringGetTextDatum(right)), NULL, true);
	return DatumGetBool(cell(0, 1));
}

/* dolt_branches: local branch heads with their latest commit metadata. The
 * dirty flag compares the session branch's live contents with its head; other
 * branches compare their persisted working set, since ordinary DML does not
 * publish working refs yet. Upstream tracking (remote, branch) is empty until
 * remotes are implemented. */
Datum
dolt_branches(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	bool		dirty = false;

	if (read_ref(branch_ref("heads")).target)
		dirty = state_differs(working_state(), head_state());
	run("WITH refs AS (SELECT substr(r.refname,7) AS name,r.reftarget AS h FROM pg_dolt_ref r WHERE r.refname LIKE 'heads/%'),"
		"c AS (SELECT refs.name,refs.h,convert_from(o.objpayload,'UTF8')::jsonb AS m,o.objchildren[1] AS db FROM refs JOIN pg_dolt_object_visible o ON o.objhash=refs.h) "
		"SELECT c.name,pg_catalog.dolt_hash_encode(c.h),m->>'committer',m->>'email'," DG_DATE_SQL("m->>'date_us'") ",m->>'message',''::text,''::text,"
		"CASE WHEN c.name=$1 THEN $2 ELSE coalesce(wo.objpayload<>dbo.objpayload,false) END,"
		"m->>'author',m->>'author_email'," DG_DATE_SQL("m->>'author_date_us'") " "
		"FROM c LEFT JOIN pg_dolt_ref w ON w.refname='working/' || c.name LEFT JOIN pg_dolt_object_visible wo ON wo.objhash=w.reftarget LEFT JOIN pg_dolt_object_visible dbo ON dbo.objhash=c.db ORDER BY c.name COLLATE \"C\"",
		2, VC_TYPES(TEXTOID, BOOLOID), VC_ARGS(CStringGetTextDatum(dg_current_branch()), BoolGetDatum(dirty)), NULL, true);
	return_rows(fcinfo, nest);
	return (Datum) 0;
}

/* dolt_status: staged (HEAD vs STAGED) and unstaged (STAGED vs WORKING)
 * table and schema changes, preceded by tables with unresolved conflicts. */
Datum
dolt_status(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	char	   *head,
			   *staged,
			   *working,
			   *merge = "{\"conflicts\":[]}";
	VersionRef	pending;

	head = head_state();
	staged = staged_state();
	working = working_state();
	pending = read_ref(branch_ref("merge"));
	if (pending.target)
		merge = metadata(pending.target, "merge");
	run("WITH h AS (SELECT v FROM jsonb_array_elements($1::jsonb->'members') v),"
		"s AS (SELECT v FROM jsonb_array_elements($2::jsonb->'members') v),"
		"w AS (SELECT v FROM jsonb_array_elements($3::jsonb->'members') v),"
		"conflicts AS (SELECT DISTINCT v->>'table' AS name FROM jsonb_array_elements($4::jsonb->'conflicts') v UNION SELECT DISTINCT v->>'table' FROM jsonb_array_elements(coalesce($4::jsonb->'violations','[]'::jsonb)) v UNION SELECT DISTINCT v->>'table' FROM jsonb_array_elements(coalesce($4::jsonb->'schema_conflicts','[]'::jsonb)) v),"
		"tables AS ("
		" SELECT 1 AS grp,coalesce(a.v->>'name',b.v->>'name') AS name,true AS staged,CASE WHEN a.v IS NULL THEN 'new table' WHEN b.v IS NULL THEN 'deleted' ELSE 'modified' END AS status"
		" FROM (SELECT v FROM h WHERE v->>'kind' IN ('table','routine','view','type','trigger','sequence','aggregate','operator','cast')) a FULL JOIN (SELECT v FROM s WHERE v->>'kind' IN ('table','routine','view','type','trigger','sequence','aggregate','operator','cast')) b ON a.v->>'name'=b.v->>'name' AND a.v->>'kind'=b.v->>'kind'"
		" WHERE a.v IS NULL OR b.v IS NULL OR a.v->>'schema' IS DISTINCT FROM b.v->>'schema' OR a.v->>'content' IS DISTINCT FROM b.v->>'content'"
		" UNION ALL"
		" SELECT 2,coalesce(a.v->>'name',b.v->>'name'),false,CASE WHEN a.v IS NULL THEN 'new table' WHEN b.v IS NULL THEN 'deleted' ELSE 'modified' END"
		" FROM (SELECT v FROM s WHERE v->>'kind' IN ('table','routine','view','type','trigger','sequence','aggregate','operator','cast')) a FULL JOIN (SELECT v FROM w WHERE v->>'kind' IN ('table','routine','view','type','trigger','sequence','aggregate','operator','cast')) b ON a.v->>'name'=b.v->>'name' AND a.v->>'kind'=b.v->>'kind'"
		" WHERE a.v IS NULL OR b.v IS NULL OR a.v->>'schema' IS DISTINCT FROM b.v->>'schema' OR a.v->>'content' IS DISTINCT FROM b.v->>'content'"
		" UNION ALL"
		" SELECT 3,coalesce(a.v->'definition'->>'name',b.v->'definition'->>'name'),true,CASE WHEN a.v IS NULL THEN 'new schema' ELSE 'deleted schema' END"
		" FROM (SELECT v FROM h WHERE v->>'kind'='namespace') a FULL JOIN (SELECT v FROM s WHERE v->>'kind'='namespace') b ON a.v->'definition'->>'name'=b.v->'definition'->>'name'"
		" WHERE a.v IS NULL OR b.v IS NULL"
		" UNION ALL"
		" SELECT 4,coalesce(a.v->'definition'->>'name',b.v->'definition'->>'name'),false,CASE WHEN a.v IS NULL THEN 'new schema' ELSE 'deleted schema' END"
		" FROM (SELECT v FROM s WHERE v->>'kind'='namespace') a FULL JOIN (SELECT v FROM w WHERE v->>'kind'='namespace') b ON a.v->'definition'->>'name'=b.v->'definition'->>'name'"
		" WHERE a.v IS NULL OR b.v IS NULL) "
		"SELECT name,staged,status FROM (SELECT 0 AS grp,name,false AS staged,CASE WHEN name IN (SELECT v->>'table' FROM jsonb_array_elements(coalesce($4::jsonb->'schema_conflicts','[]'::jsonb)) v) THEN 'schema conflict' WHEN name IN (SELECT v->>'table' FROM jsonb_array_elements($4::jsonb->'conflicts') v) THEN 'conflict' ELSE 'constraint violation' END::text AS status FROM conflicts"
		" UNION ALL SELECT grp,name,staged,status FROM tables WHERE name NOT IN (SELECT name FROM conflicts)) x ORDER BY grp,name COLLATE \"C\"",
		4, VC_TYPES(TEXTOID, TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(head), CStringGetTextDatum(staged), CStringGetTextDatum(working), CStringGetTextDatum(merge)), NULL, true);
	return_rows(fcinfo, nest);
	return (Datum) 0;
}

/* dolt_merge_status: the pending merge of the session branch. */
Datum
dolt_merge_status(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	VersionRef	pending = read_ref(branch_ref("merge"));
	Datum		values[5] = {0};
	bool		nulls[5] = {false, true, true, true, true};

	values[0] = BoolGetDatum(pending.target != NULL);
	if (pending.target)
	{
		char	   *json = metadata(pending.target, "merge");
		char	   *source = json_value(json, "source");
		char	   *unmerged;

		run("SELECT coalesce(string_agg(name,', ' ORDER BY name COLLATE \"C\"),'') FROM (SELECT DISTINCT (parse_ident(t))[array_length(parse_ident(t),1)] AS name FROM (SELECT v->>'table' AS t FROM jsonb_array_elements($1::jsonb->'conflicts') v UNION SELECT v->>'table' FROM jsonb_array_elements(coalesce($1::jsonb->'violations','[]'::jsonb)) v) y) x", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json)), NULL, true);
		unmerged = strcell(0, 1);
		values[1] = CStringGetTextDatum(source ? source : hash_format(child(pending.target, 2)));
		values[2] = CStringGetTextDatum(hash_format(child(pending.target, 2)));
		values[3] = CStringGetTextDatum(psprintf("refs/heads/%s", dg_current_branch()));
		values[4] = CStringGetTextDatum(unmerged);
		memset(nulls, 0, sizeof(nulls));
	}
	end_api(nest);
	return command_record(fcinfo, values, nulls);
}

/* dolt_diff system table: staged and working table changes of the session
 * branch, then every commit reachable from its head compared with its first
 * parent. Added or dropped tables are data changes only when they had rows. */
Datum
dolt_diff_changes(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	VersionRef	head = read_ref(branch_ref("heads"));
	char	   *empty = (char *) empty_content();

	if (!head.target)
	{
		run("SELECT NULL::text WHERE false", 0, NULL, NULL, NULL, true);
		return_rows(fcinfo, nest);
		return (Datum) 0;
	}
	run("WITH RECURSIVE walk(h) AS (SELECT $1::bytea UNION SELECT unnest(o.objchildren[2:]) FROM walk w JOIN pg_dolt_object_visible o ON o.objhash=w.h),"
		"commits AS (SELECT o.objhash,convert_from(o.objpayload,'UTF8')::jsonb AS m,o.objchildren FROM walk w JOIN pg_dolt_object_visible o ON o.objhash=w.h WHERE o.objkind='commit'),"
		"states AS (SELECT 2 AS grp,pg_catalog.dolt_hash_encode(c.objhash) AS commit_hash,c.m,convert_from(d.objpayload,'UTF8')::jsonb AS db,"
		" (SELECT convert_from(p.objpayload,'UTF8')::jsonb FROM pg_dolt_object_visible pc JOIN pg_dolt_object_visible p ON p.objhash=pc.objchildren[1] WHERE pc.objhash=c.objchildren[2]) AS pdb"
		" FROM commits c JOIN pg_dolt_object_visible d ON d.objhash=c.objchildren[1]"
		" UNION ALL SELECT 0,'STAGED',NULL,$3::jsonb,$2::jsonb WHERE $3::jsonb->'members' <> $2::jsonb->'members'"
		" UNION ALL SELECT 1,'WORKING',NULL,$4::jsonb,$3::jsonb WHERE $4::jsonb->'members' <> $3::jsonb->'members'),"
		"deltas AS (SELECT s.grp,s.commit_hash,s.m,n.name,"
		" (SELECT v FROM jsonb_array_elements(coalesce(s.pdb->'members','[]')) v WHERE v->>'kind'='table' AND v->>'name'=n.name) AS a,"
		" (SELECT v FROM jsonb_array_elements(coalesce(s.db->'members','[]')) v WHERE v->>'kind'='table' AND v->>'name'=n.name) AS b"
		" FROM states s CROSS JOIN LATERAL (SELECT DISTINCT v->>'name' AS name FROM (SELECT v FROM jsonb_array_elements(coalesce(s.pdb->'members','[]')) v UNION ALL SELECT v FROM jsonb_array_elements(coalesce(s.db->'members','[]')) v) x WHERE v->>'kind'='table') n) "
		"SELECT commit_hash,name,m->>'committer',m->>'email'," DG_DATE_SQL("m->>'date_us'") ",m->>'message',"
		"coalesce(a->>'content',$5) IS DISTINCT FROM coalesce(b->>'content',$5),(a->>'schema') IS DISTINCT FROM (b->>'schema'),"
		"m->>'author',m->>'author_email'," DG_DATE_SQL("m->>'author_date_us'") " FROM deltas"
		" WHERE a IS DISTINCT FROM b ORDER BY grp,(m->>'height')::bigint DESC,(m->>'date_us')::bigint DESC,commit_hash,name COLLATE \"C\"",
		5, VC_TYPES(BYTEAOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID), VC_ARGS(PointerGetDatum(head.target), CStringGetTextDatum(head_state()), CStringGetTextDatum(staged_state()), CStringGetTextDatum(working_state()), CStringGetTextDatum(empty)), NULL, true);
	return_rows(fcinfo, nest);
	return (Datum) 0;
}

/* dolt_remote: add or remove a named remote. Remotes are database-local
 * configuration (not versioned), like Dolt's repo state. */
Datum
dolt_remote(PG_FUNCTION_ARGS)
{
	int			n,
				nest;
	char	  **args = arguments(fcinfo, &n);

	standalone(fcinfo, false);
	nest = begin_api();
	if (n == 3 && !strcmp(args[0], "add"))
	{
		valid_name(args[1]);
		if (!*args[2])
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("remote url cannot be empty")));
		run("SELECT 1 FROM pg_catalog.pg_dolt_remote WHERE remotename=$1", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(args[1])), NULL, true);
		if (SPI_processed)
			ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("remote already exists: %s", args[1])));
		run("INSERT INTO pg_catalog.pg_dolt_remote(remotename,remoteurl,remotefetch,remoteparams) VALUES($1,$2,jsonb_build_array('refs/heads/*:refs/remotes/' || $1 || '/*'),'{}'::jsonb)", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(args[1]), CStringGetTextDatum(args[2])), NULL, false);
	}
	else if (n == 2 && (!strcmp(args[0], "remove") || !strcmp(args[0], "rm")))
	{
		run("DELETE FROM pg_catalog.pg_dolt_remote WHERE remotename=$1", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(args[1])), NULL, false);
		if (!SPI_processed)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("unknown remote: %s", args[1])));
	}
	else
		unsupported("dolt_remote supports 'add' name url and 'remove' name");
	end_api(nest);
	PG_RETURN_INT64(0);
}

Datum
dolt_remote_noargs(PG_FUNCTION_ARGS)
{
	return dolt_remote(fcinfo);
}

/* Built-in document defaults for dolt_docs. */
Datum
dolt_default_doc(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (!strcmp(name, "AGENT.md"))
		PG_RETURN_TEXT_P(cstring_to_text(dg_agent_doc));
	PG_RETURN_NULL();
}

/* dolt_hashof / hashof: the commit hash named by a revision (HEAD, branch, tag,
 * hash or ancestor expression). */
Datum
dolt_hashof(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	char	   *answer = hash_format(resolve(text_to_cstring(PG_GETARG_TEXT_PP(0))));

	end_api(nest);
	PG_RETURN_TEXT_P(cstring_to_text(answer));
}
