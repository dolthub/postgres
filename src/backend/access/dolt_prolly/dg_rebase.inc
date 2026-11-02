/* dolt_rebase (interactive plans over the dolt.rebase table) and dolt_stash.
 * See doc/doltgres/rebase-stash.md. Included by dg_version.c after
 * dg_schemas.inc. */

/* Switch the session to |name| the way dolt_checkout does once its refs
 * exist: bind the branch's working database and schedule the branch change
 * for transaction end. */
static void
switch_session_branch(const char *name)
{
	bytea	   *commit = read_ref(psprintf("heads/%s", name)).target,
			   *target = read_ref(psprintf("working/%s", name)).target;
	VersionRef	ref;

	binding_prepare(name, target ? target : child(commit, 1), true);
	ref = read_ref("views/enabled");
	if (!ref.target)
		move_ref("views/enabled", ref, commit);
	dg_select_branch(name);
}

/* Live working set equals HEAD and nothing is staged. */
static bool
workspace_clean(void)
{
	VersionRef	head = read_ref(branch_ref("heads")),
				staged = read_ref(branch_ref("staged"));
	bytea	   *headdb = child(head.target, 1);

	return database_same(capture_visible().hash, headdb) && (!staged.target || database_same(staged.target, headdb));
}

/* Replace the live tables that |db| defines with their contents in |db|,
 * leaving tables |db| does not know about alone. */
static void
restore_tables(bytea *db)
{
	List	   *keep = state_tables(metadata(db, "database"));
	Capture		current = capture(true);
	bytea	   *result;

	advance_snapshot();
	result = compose_database(current.hash, db, keep);
	replace_working(dg_current_branch(), result, keep);
}

/* ---- dolt_rebase ---- */

static char *
rebase_branch_name(const char *branch)
{
	return psprintf("dolt_rebase_%s", branch);
}

/* The rebase state object of the session branch, or NULL. The session is on
 * the temporary dolt_rebase_<branch> branch while a rebase is in progress. */
static char *
rebase_state(void)
{
	VersionRef	ref = read_ref(branch_ref("rebase"));

	return ref.target ? metadata(ref.target, "rebase") : NULL;
}

bool
dg_rebase_active(void)
{
	int			nest = begin_api();
	bool		active = rebase_state() != NULL;

	end_api(nest);
	return active;
}

static char *
quote_json(const char *text)
{
	StringInfoData out;

	initStringInfo(&out);
	escape_json(&out, text);
	return out.data;
}

/* Commits reachable from |head| but not from |upstream|, oldest first, as
 * the initial pick plan. */
static void
rebase_plan_rows(const char *rebasebranch, bytea *head, bytea *upstream)
{
	int			n,
				i;
	char	  **hashes,
			  **messages;

	run("WITH RECURSIVE a(h,d) AS (SELECT $1::bytea,0 UNION SELECT unnest(o.objchildren[2:]),a.d+1 FROM a JOIN pg_dolt_object_visible o ON o.objhash=a.h), b(h) AS (SELECT $2::bytea UNION SELECT unnest(o.objchildren[2:]) FROM b JOIN pg_dolt_object_visible o ON o.objhash=b.h), x AS (SELECT h,min(d) AS d FROM a WHERE h NOT IN (SELECT h FROM b) GROUP BY h) SELECT pg_catalog.dolt_hash_encode(x.h),convert_from(o.objpayload,'UTF8')::jsonb->>'message' FROM x JOIN pg_dolt_object_visible o ON o.objhash=x.h WHERE o.objkind='commit' ORDER BY x.d DESC,x.h", 2, VC_TYPES(BYTEAOID, BYTEAOID), VC_ARGS(PointerGetDatum(head), PointerGetDatum(upstream)), NULL, true);
	n = SPI_processed;
	if (n == 0)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("didn't identify any commits!")));
	hashes = palloc(sizeof(char *) * n);
	messages = palloc(sizeof(char *) * n);
	for (i = 0; i < n; i++)
	{
		hashes[i] = strcell(i, 1);
		messages[i] = strcell(i, 2);
	}
	for (i = 0; i < n; i++)
		run("INSERT INTO pg_catalog.pg_dolt_rebase(rebasebranch,rebase_order,action,commit_hash,commit_message) VALUES ($1,$2,'pick',$3,$4)", 4, VC_TYPES(TEXTOID, FLOAT8OID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(rebasebranch), Float8GetDatum(i + 1), CStringGetTextDatum(hashes[i]), CStringGetTextDatum(messages[i] ? messages[i] : "")), NULL, false);
}

static void
clear_rebase(const char *rebasebranch)
{
	VersionRef	ref = read_ref(psprintf("rebase/%s", rebasebranch));

	run("DELETE FROM pg_catalog.pg_dolt_rebase WHERE rebasebranch=$1", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(rebasebranch)), NULL, false);
	if (ref.target)
		move_ref(psprintf("rebase/%s", rebasebranch), ref, NULL);
}

static Datum
rebase_result(FunctionCallInfo fcinfo, int nest, const char *message)
{
	Datum		values[2];
	bool		nulls[2] = {false, false};

	values[0] = Int64GetDatum(0);
	values[1] = CStringGetTextDatum(message);
	end_api(nest);
	return command_record(fcinfo, values, nulls);
}

static Datum
start_rebase(FunctionCallInfo fcinfo, int nest, const char *upstream_spec)
{
	char	   *branch = dg_current_branch(),
			   *rebasebranch = rebase_branch_name(branch);
	VersionRef	head = read_ref(branch_ref("heads"));
	bytea	   *upstream,
			   *state;
	StringInfoData json;

	if (read_ref(branch_ref("merge")).target)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("unable to start rebase while a merge is in progress â63“ abort the current merge before proceeding")));
	if (rebase_state() || read_ref(psprintf("rebase/%s", rebasebranch)).target)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("unable to start rebase while another rebase is in progress â63“ abort the current rebase before proceeding")));
	if (!workspace_clean())
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("cannot start a rebase with uncommitted changes")));
	if (!*upstream_spec)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("no upstream branch specified")));
	upstream = resolve(upstream_spec);
	if (read_ref(psprintf("heads/%s", rebasebranch)).target)
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("branch already exists: %s", rebasebranch)));
	rebase_plan_rows(rebasebranch, head.target, upstream);
	initStringInfo(&json);
	appendStringInfo(&json, "{\"branch\":%s,\"upstream\":\"%s\",\"head\":\"%s\"}", quote_json(branch), hash_format(upstream), hash_format(head.target));
	state = put("rebase", json.data, VC_ARGS(PointerGetDatum(upstream), PointerGetDatum(head.target)), 2);
	create_branch_refs(rebasebranch, upstream, NULL);
	move_ref(psprintf("rebase/%s", rebasebranch), read_ref(psprintf("rebase/%s", rebasebranch)), state);
	advance_snapshot();
	{
		/* Storage may retain roots of an earlier temporary branch. */
		List	   *tables = state_tables(metadata(child(upstream, 1), "database"));
		ListCell   *lc;

		foreach(lc, tables)
			restore_table_on(rebasebranch, child(upstream, 1), (char *) lfirst(lc));
	}
	switch_session_branch(rebasebranch);
	return rebase_result(fcinfo, nest, psprintf("interactive rebase started on branch %s; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')", rebasebranch));
}

/* Leave the temporary branch: bind the original branch (at |newhead| when a
 * continue finished, otherwise unchanged) and drop the temporary refs. */
static void
finish_rebase(const char *rebasebranch, const char *branch, bytea *newhead)
{
	VersionRef	orig = read_ref(psprintf("heads/%s", branch));

	clear_rebase(rebasebranch);
	if (newhead)
	{
		move_ref(psprintf("heads/%s", branch), orig, newhead);
		move_ref(psprintf("working/%s", branch), read_ref(psprintf("working/%s", branch)), child(newhead, 1));
		move_ref(psprintf("staged/%s", branch), read_ref(psprintf("staged/%s", branch)), child(newhead, 1));
	}
	advance_snapshot();
	switch_session_branch(branch);
	if (newhead)
	{
		/* The original branch's live tables take the rebased contents. */
		List	   *tables = state_tables(metadata(child(newhead, 1), "database"));
		ListCell   *lc;

		foreach(lc, tables)
			restore_table_on(branch, child(newhead, 1), (char *) lfirst(lc));
	}
	delete_branch_refs(rebasebranch);
	advance_snapshot();
}

static Datum
abort_rebase(FunctionCallInfo fcinfo, int nest)
{
	char	   *state = rebase_state(),
			   *rebasebranch = dg_current_branch();

	if (!state)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("no rebase in progress")));
	finish_rebase(rebasebranch, json_value(state, "branch"), NULL);
	return rebase_result(fcinfo, nest, "Interactive rebase aborted");
}

typedef struct RebaseStep
{
	char	   *action;
	char	   *hash;
	char	   *message;
}			RebaseStep;

static RebaseStep *
load_rebase_plan(const char *rebasebranch, int *count)
{
	RebaseStep *steps;
	int			i;
	bool		picked = false;

	run("SELECT action,commit_hash,commit_message FROM pg_catalog.pg_dolt_rebase WHERE rebasebranch=$1 ORDER BY rebase_order", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(rebasebranch)), NULL, true);
	*count = SPI_processed;
	steps = palloc0(sizeof(RebaseStep) * (*count + 1));
	for (i = 0; i < *count; i++)
	{
		steps[i].action = strcell(i, 1);
		steps[i].hash = strcell(i, 2);
		steps[i].message = strcell(i, 3);
		if (!steps[i].message)
			steps[i].message = "";
		if (!strcmp(steps[i].action, "pick") || !strcmp(steps[i].action, "reword"))
			picked = true;
		else if (!strcmp(steps[i].action, "squash") || !strcmp(steps[i].action, "fixup"))
		{
			if (!picked)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid rebase plan: squash and fixup actions must appear after a pick or reword action")));
		}
		else if (!strcmp(steps[i].action, "drop"))
			;
		else
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("rebase action '%s' is not supported", steps[i].action)));
	}
	return steps;
}

/* Apply one commit's changes onto the session branch head, returning the
 * resulting database or NULL when the commit becomes empty. */
static bytea *
apply_rebase_step(RebaseStep * step)
{
	bytea	   *target = resolve(step->hash),
			   *head = read_ref(branch_ref("heads")).target,
			   *headdb = child(head, 1),
			   *targetdb = child(target, 1),
			   *parentdb;
	StringInfoData conflicts,
				violations,
				schema_conflicts;
	bytea	   *database;

	if (parent_count(target) < 1)
		unsupported("rebasing a root commit");
	parentdb = child(child(target, 2), 1);
	initStringInfo(&conflicts);
	initStringInfo(&violations);
	initStringInfo(&schema_conflicts);
	merge_databases(parentdb, headdb, targetdb, &conflicts, &violations, &schema_conflicts);
	if (conflicts.len || violations.len || schema_conflicts.len)
	{
		char	   *meta = metadata(target, "commit");

		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("%s conflict detected while rebasing commit %s (%s)", schema_conflicts.len ? "schema" : "data", step->hash, json_value(meta, "message")), errhint("abort the rebase with dolt_rebase('--abort') and resolve the conflicting changes before rebasing again")));
	}
	database = capture_visible().hash;
	return database_same(database, headdb) ? NULL : database;
}

static Datum
continue_rebase(FunctionCallInfo fcinfo, int nest)
{
	char	   *state = rebase_state(),
			   *rebasebranch = dg_current_branch(),
			   *branch;
	RebaseStep *steps;
	int			n,
				i;
	bytea	   *newhead;

	if (!state)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("no rebase in progress")));
	branch = json_value(state, "branch");
	if (!workspace_clean())
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("cannot continue a rebase with unstaged changes. If you are trying to add changes from the previous commit, use dolt_add() and dolt_commit() to commit them, then dolt_rebase('--continue')")));
	steps = load_rebase_plan(rebasebranch, &n);
	for (i = 0; i < n; i++)
	{
		RebaseStep *step = &steps[i];
		VersionRef	head;
		bytea	   *database,
				   *commit,
				   *parent;
		char	   *meta,
				   *message;

		if (!strcmp(step->action, "drop"))
			continue;
		database = apply_rebase_step(step);
		head = read_ref(branch_ref("heads"));
		meta = metadata(resolve(step->hash), "commit");
		if (!strcmp(step->action, "pick") || !strcmp(step->action, "reword"))
		{
			if (!database)
				continue;		/* the commit became empty */
			message = !strcmp(step->action, "reword") ? step->message : json_value(meta, "message");
			commit = commit_object(database, head.target, NULL, message, json_value(meta, "committer"), json_value(meta, "email"), GetCurrentTimestamp());
		}
		else
		{
			/* squash/fixup amend the previous commit. */
			char	   *headmeta = metadata(head.target, "commit");

			if (!database)
				database = child(head.target, 1);
			parent = parent_count(head.target) ? child(head.target, 2) : NULL;
			message = !strcmp(step->action, "fixup") ? json_value(headmeta, "message") : psprintf("%s\n\n%s", json_value(headmeta, "message"), step->message);
			commit = commit_object(database, parent, NULL, message, json_value(headmeta, "committer"), json_value(headmeta, "email"), GetCurrentTimestamp());
		}
		move_ref(branch_ref("heads"), head, commit);
		move_ref(branch_ref("staged"), read_ref(branch_ref("staged")), database);
		move_ref(branch_ref("working"), read_ref(branch_ref("working")), database);
		advance_snapshot();
	}
	newhead = read_ref(branch_ref("heads")).target;
	finish_rebase(rebasebranch, branch, newhead);
	return rebase_result(fcinfo, nest, psprintf("Successfully rebased and updated refs/heads/%s", branch));
}

Datum
dolt_rebase(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n);
	bool		interactive = false,
				cont = false,
				abort_flag = false;
	char	   *upstream = NULL;

	standalone(fcinfo, true);
	nest = begin_api();
	lock_workspace();
	ensure_repository();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "-i") || !strcmp(args[i], "--interactive"))
			interactive = true;
		else if (!strcmp(args[i], "--continue"))
			cont = true;
		else if (!strcmp(args[i], "--abort"))
			abort_flag = true;
		else if (!strncmp(args[i], "--empty=", 8))
		{
			if (strcmp(args[i] + 8, "drop") && strcmp(args[i] + 8, "keep"))
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("unsupported option for the empty flag (%s); only 'drop' and 'keep' are supported", args[i] + 8)));
		}
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else if (!upstream)
			upstream = args[i];
		else
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("too many args")));
	}
	if (abort_flag)
		return abort_rebase(fcinfo, nest);
	if (cont)
		return continue_rebase(fcinfo, nest);
	if (!interactive)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("non-interactive rebases not currently supported")));
	if (!upstream)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("not enough args")));
	return start_rebase(fcinfo, nest, upstream);
}

/* ---- dolt_stash ---- */

/* Stash refs are stash/<name>/<seq>; stash@{0} is the highest sequence. */
static char *
stash_ref(const char *name, int idx)
{
	run("SELECT refname FROM pg_catalog.pg_dolt_ref WHERE split_part(refname,'/',1)='stash' AND split_part(refname,'/',2)=$1 ORDER BY split_part(refname,'/',3)::bigint DESC OFFSET $2 LIMIT 1", 2, VC_TYPES(TEXTOID, INT8OID), VC_ARGS(CStringGetTextDatum(name), Int64GetDatum(idx)), NULL, true);
	if (!SPI_processed)
	{
		run("SELECT 1 FROM pg_catalog.pg_dolt_ref WHERE split_part(refname,'/',1)='stash' AND split_part(refname,'/',2)=$1 LIMIT 1", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(name)), NULL, true);
		if (!SPI_processed)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("No stash entries found.")));
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("error: stash@{%d} is not a valid reference in stash %s", idx, name)));
	}
	return strcell(0, 1);
}

static void
stash_push(const char *name, bool untracked, bool all)
{
	const char *branch = dg_current_branch();
	VersionRef	head = read_ref(branch_ref("heads")),
				staged = read_ref(branch_ref("staged"));
	bytea	   *headdb = child(head.target, 1),
			   *stageddb = staged.target ? staged.target : headdb,
			   *workingdb,
			   *state;
	Capture		current = capture_visible();
	List *tracked = list_concat(state_objects(head_state()), state_objects(staged_state()));
	List *selected = list_copy(tracked);
	List *ignored = all ? NIL : ignored_untracked_tables(current.json, staged_state());
	ListCell   *lc;
	StringInfoData json;
	char	   *message,
			   *headmeta = metadata(head.target, "commit");
	int64		seq;

	if (untracked || all)
		foreach(lc, state_objects(current.json))
			if (!name_selected(selected, lfirst(lc)) && !name_selected(ignored, lfirst(lc)))
				selected = lappend(selected, lfirst(lc));
	/* Include deletions by selecting from HEAD as well as the live root. */
	workingdb = compose_database(headdb, current.hash, selected);
	if (database_same(workingdb, headdb) && database_same(stageddb, headdb))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("No local changes to save")));
	run("SELECT coalesce(max(split_part(refname,'/',3)::bigint),0)+1 FROM pg_catalog.pg_dolt_ref WHERE split_part(refname,'/',1)='stash' AND split_part(refname,'/',2)=$1", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(name)), NULL, true);
	seq = DatumGetInt64(cell(0, 1));
	message = json_value(headmeta, "message");
	initStringInfo(&json);
	appendStringInfo(&json, "{\"branch\":%s,\"head\":\"%s\",\"message\":%s}", quote_json(branch), hash_format(head.target), quote_json(message));
	state = put("stash", json.data, VC_ARGS(PointerGetDatum(workingdb), PointerGetDatum(stageddb), PointerGetDatum(head.target)), 3);
	move_ref(psprintf("stash/%s/" INT64_FORMAT, name, seq), empty_ref, state);
	replace_working(dg_current_branch(), compose_database(current.hash, headdb, selected), NIL);
	move_ref(branch_ref("staged"), staged, headdb);
	move_ref(branch_ref("working"), read_ref(branch_ref("working")), capture_visible().hash);
	advance_snapshot();
}

/* Three-way application of a stash onto the live working set. */
static void
stash_apply(const char *name, int idx)
{
	char	   *ref = stash_ref(name, idx);
	bytea *state = read_ref(ref).target,
		*stashdb = child(state, 1),
		*stashhead = child(state, 3),
		*stashheaddb = child(stashhead, 1),
		*current = capture_visible().hash;
	StringInfoData conflicts, violations, schema_conflicts;
	List *added = NIL;
	List *base_tables = state_tables(metadata(stashheaddb, "database"));
	List *staged_tables = state_tables(metadata(child(state, 2), "database"));
	ListCell *lc;
	Capture merged;

	initStringInfo(&conflicts);
	initStringInfo(&violations);
	initStringInfo(&schema_conflicts);
	merge_databases(stashheaddb, current, stashdb, &conflicts, &violations, &schema_conflicts);
	if (schema_conflicts.len)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("merge aborted: schema conflict applying stash@{%d}: %s", idx, schema_conflicts.data)));
	if (conflicts.len || violations.len)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("error: Your local changes would be overwritten by applying stash@{%d}; the stash entry is kept", idx)));
	merged = capture_visible();
	/* Dolt restores staging only for newly staged tables, with their full
	 * merged contents. Previously tracked modifications remain unstaged. */
	foreach(lc, staged_tables)
		if (!name_selected(base_tables, lfirst(lc)))
			added = lappend(added, lfirst(lc));
	if (added)
	{
		VersionRef staged = read_ref(branch_ref("staged"));
		bytea *base = staged.target ? staged.target : child(read_ref(branch_ref("heads")).target, 1);

		move_ref(branch_ref("staged"), staged, compose_database(base, merged.hash, added));
	}
	move_ref(branch_ref("working"), read_ref(branch_ref("working")), merged.hash);
	advance_snapshot();
}

static void
stash_drop(const char *name, int idx)
{
	char	   *ref = stash_ref(name, idx);

	move_ref(ref, read_ref(ref), NULL);
}

static void
stash_clear(const char *name)
{
	int			n,
				i;
	char	  **refs;

	run("SELECT refname FROM pg_catalog.pg_dolt_ref WHERE split_part(refname,'/',1)='stash' AND split_part(refname,'/',2)=$1", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(name)), NULL, true);
	n = SPI_processed;
	refs = palloc(sizeof(char *) * Max(n, 1));
	for (i = 0; i < n; i++)
		refs[i] = strcell(i, 1);
	for (i = 0; i < n; i++)
		move_ref(refs[i], read_ref(refs[i]), NULL);
}

Datum
dolt_stash(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				idx = 0;
	char	  **args = arguments(fcinfo, &n),
			   *command,
			   *name;
	bool untracked = false, all = false;

	standalone(fcinfo, true);
	PreventCommandIfReadOnly("dolt_stash");
	if (n < 2)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("error: invalid arguments. Must provide valid subcommand and stash name")));
	command = args[0];
	name = args[1];
	valid_name(name);
	if (strchr(name, '/'))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid stash name: %s", name)));
	if (!strcmp(command, "push"))
	{
		for (int i = 2; i < n; i++)
		{
			if (!strcmp(args[i], "--include-untracked") || !strcmp(args[i], "-u"))
				untracked = true;
			else if (!strcmp(args[i], "--all") || !strcmp(args[i], "-a"))
				all = true;
			else
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("error: invalid arguments. Push takes only subcommand and stash name, with --include-untracked or --all")));
		}
	}
	else if (n > 2)
	{
		char *end, *number = args[2];
		long parsed;

		if (!strcmp(command, "clear"))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("error: invalid arguments. Clear takes only subcommand and stash name")));
		if (!strncmp(number, "stash@{", 7) && number[strlen(number)-1] == '}')
			number = pnstrdup(number + 7, strlen(number) - 8);
		parsed = strtol(number, &end, 10);
		if (!*number || *end || parsed < 0 || parsed > INT_MAX || n > 3)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("error: %s is not a valid reference", args[2])));
		idx = parsed;
	}
	nest = begin_api();
	lock_workspace();
	ensure_repository();
	if (!strcmp(command, "push"))
		stash_push(name, untracked, all);
	else if (!strcmp(command, "pop"))
	{
		stash_apply(name, idx);
		stash_drop(name, idx);
	}
	else if (!strcmp(command, "apply"))
		stash_apply(name, idx);
	else if (!strcmp(command, "drop"))
		stash_drop(name, idx);
	else if (!strcmp(command, "clear"))
		stash_clear(name);
	else
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("unknown stash subcommand %s", command)));
	end_api(nest);
	PG_RETURN_INT64(0);
}
