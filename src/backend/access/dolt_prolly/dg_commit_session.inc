/* Transaction-local dirty branches; no backend-global flag survives rollback. */
typedef struct CommitDirtyBranch
{
	char	   *branch;
	SubTransactionId subid;
} CommitDirtyBranch;
static List *commit_dirty_branches;
static bool commit_callbacks_registered;
static bool commit_publishing;

static void
commit_session_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT || event == XACT_EVENT_PREPARE)
	{
		commit_dirty_branches = NIL;
		commit_publishing = false;
	}
}

static void
commit_session_subxact(SubXactEvent event, SubTransactionId id, SubTransactionId parent, void *arg)
{
	ListCell   *lc;

	foreach(lc, commit_dirty_branches)
	{
		CommitDirtyBranch *entry = lfirst(lc);

		if (entry->subid != id)
			continue;
		if (event == SUBXACT_EVENT_ABORT_SUB)
			commit_dirty_branches = foreach_delete_current(commit_dirty_branches, lc);
		else if (event == SUBXACT_EVENT_COMMIT_SUB)
			entry->subid = parent;
	}
}

void
dg_version_mark_branch(const char *branch)
{
	ListCell   *lc;
	MemoryContext old;
	CommitDirtyBranch *entry;
	SubTransactionId id;

	if (commit_publishing || !IsUnderPostmaster || IsParallelWorker())
		return;
	id = GetCurrentSubTransactionId();
	foreach(lc, commit_dirty_branches)
	{
		entry = lfirst(lc);
		if (entry->subid == id && strcmp(entry->branch, branch) == 0)
			return;
	}
	if (!commit_callbacks_registered)
	{
		RegisterXactCallback(commit_session_xact, NULL);
		RegisterSubXactCallback(commit_session_subxact, NULL);
		commit_callbacks_registered = true;
	}
	old = MemoryContextSwitchTo(TopTransactionContext);
	entry = palloc(sizeof(*entry));
	entry->branch = pstrdup(branch);
	entry->subid = id;
	commit_dirty_branches = lappend(commit_dirty_branches, entry);
	MemoryContextSwitchTo(old);
}

/* The ref and object writes stay invisible with the prepared SQL transaction. */
void
dg_version_pre_prepare(void)
{
	dg_version_pre_commit();
}

/* Runs before transaction shutdown, so SPI and user type codecs remain usable. */
void
dg_version_pre_commit(void)
{
	const char *branch = NULL;
	const char *previous_branch;
	int			nest;
	ListCell   *lc;

	if (!dolt_transaction_commit || !commit_dirty_branches || commit_publishing)
		return;
	foreach(lc, commit_dirty_branches)
	{
		CommitDirtyBranch *entry = lfirst(lc);

		if (branch && strcmp(branch, entry->branch))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("automatic version-control commit requires a single dirty branch")));
		branch = entry->branch;
	}
	commit_publishing = true;
	PushCopiedSnapshot(GetTransactionSnapshot());
	previous_branch = dg_switch_execution_branch(branch);
	PG_TRY();
	{
		VersionRef	head,
					staged,
					working;
		Capture		full;
		bytea	   *database;
		char	   *message;

		dg_flush_version_batches();
		CommandCounterIncrement();
		/* Publication may have waited for a concurrent working-set writer. */
		PopActiveSnapshot();
		PushCopiedSnapshot(GetLatestSnapshot());
		nest = begin_api();
		UpdateActiveSnapshotCommandId();
		head = read_ref(branch_ref("heads"));

		/*
		 * Preserve the non-aborting merge contract; resolution stays
		 * explicit.
		 */
		if (head.target && !read_ref(branch_ref("merge")).target)
		{
			staged = read_ref(branch_ref("staged"));
			working = read_ref(branch_ref("working"));
			database = staged_capture(&full);
			if (!database_same(database, child(head.target, 1)))
			{
				bytea	   *commit;

				run("SELECT COALESCE(NULLIF(btrim($1),''),'Transaction commit')", 1,
					VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(dolt_transaction_commit_message)), NULL, true);
				message = strcell(0, 1);
				commit = commit_object(database, head.target, NULL, message, NULL, NULL, GetCurrentTimestamp());
				move_ref(branch_ref("heads"), head, commit);
				move_ref(branch_ref("staged"), staged, database);
				move_ref(branch_ref("working"), working, full.hash);
			}
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		dg_switch_execution_branch(previous_branch);
		PopActiveSnapshot();
		commit_publishing = false;
	}
	PG_END_TRY();
}
