/* Compatibility surface for Dolt features the fork cannot provide natively:
 * statistics procedures over PostgreSQL ANALYZE, backup definitions, and
 * remote/cluster procedures that report the missing transport. See
 * doc/doltgres/compat-procedures.md. Included by dg_version.c after
 * dg_tools.inc. */

/* ---- statistics: PostgreSQL ANALYZE owns the data ---- */

/* ANALYZE every versioned user table so dolt.statistics is populated. The
 * prolly access method may reject ANALYZE; the first error becomes the
 * procedure's message instead of failing the call. */
static char *
analyze_user_tables(void)
{
	int			n,
				i;
	char	  **names;
	char	   *failure = NULL;

	run("SELECT quote_ident(n.nspname) || '.' || quote_ident(c.relname) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace JOIN pg_am a ON a.oid=c.relam WHERE " USER_NAMESPACE " AND c.relkind='r' AND a.amname='dolt_prolly_table' ORDER BY 1", 0, NULL, NULL, NULL, true);
	n = SPI_processed;
	names = palloc(sizeof(char *) * Max(n, 1));
	for (i = 0; i < n; i++)
		names[i] = strcell(i, 1);
	for (i = 0; i < n && !failure; i++)
	{
		MemoryContext old = CurrentMemoryContext;
		ResourceOwner owner = CurrentResourceOwner;

		BeginInternalSubTransaction(NULL);
		MemoryContextSwitchTo(old);
		PG_TRY();
		{
			run(psprintf("ANALYZE %s", names[i]), 0, NULL, NULL, NULL, false);
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
			failure = psprintf("statistics refresh unavailable: %s", edata->message);
		}
		PG_END_TRY();
	}
	CommandCounterIncrement();
	return failure ? failure : "statistics refreshed with ANALYZE";
}

static Datum
stats_message(const char *message)
{
	PG_RETURN_TEXT_P(cstring_to_text(message));
}

Datum
dolt_stats_once(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	char	   *message = analyze_user_tables();

	end_api(nest);
	return stats_message(message);
}

Datum
dolt_stats_wait(PG_FUNCTION_ARGS)
{
	return dolt_stats_once(fcinfo);
}

Datum
dolt_stats_flush(PG_FUNCTION_ARGS)
{
	return dolt_stats_once(fcinfo);
}

Datum
dolt_stats_stop(PG_FUNCTION_ARGS)
{
	return stats_message("statistics are collected by PostgreSQL autovacuum; nothing to stop");
}

Datum
dolt_stats_restart(PG_FUNCTION_ARGS)
{
	return stats_message("statistics are collected by PostgreSQL autovacuum; nothing to restart");
}

Datum
dolt_stats_gc(PG_FUNCTION_ARGS)
{
	return stats_message("statistics live in pg_statistic; nothing to collect");
}

Datum
dolt_stats_purge(PG_FUNCTION_ARGS)
{
	return stats_message("statistics live in pg_statistic; nothing to purge");
}

Datum
dolt_stats_timers(PG_FUNCTION_ARGS)
{
	return stats_message("statistics timers are PostgreSQL autovacuum settings");
}

Datum
dolt_stats_info(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	char	   *info;

	run("SELECT json_build_object('dbCnt',1,'active',true,'storageBucketCnt',count(*),'cachedBucketCnt',count(*),'cachedBoundCnt',count(*),'cachedTemplateCnt',0,'statCnt',count(*),'backing','pg_statistic')::text FROM pg_stats s JOIN pg_namespace n ON n.nspname=s.schemaname WHERE " USER_NAMESPACE, 0, NULL, NULL, NULL, true);
	info = strcell(0, 1);
	end_api(nest);
	return stats_message(info);
}

/* ---- native file backups ---- */

Datum
dolt_backup(PG_FUNCTION_ARGS)
{
	int			n,
				nest;
	char	  **args = arguments(fcinfo, &n);

	if (n == 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("use 'dolt_backups' table to list backups")));
	nest = begin_api();
	if (!strcmp(args[0], "add"))
	{
		if (n != 3)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("dolt_backup add requires a name and a url")));
		valid_name(args[1]);
		run("SELECT 1 FROM pg_catalog.pg_dolt_backup WHERE backupname=$1", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(args[1])), NULL, true);
		if (SPI_processed)
			ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("backup '%s' already exists", args[1])));
		run("INSERT INTO pg_catalog.pg_dolt_backup(backupname,backupurl) VALUES ($1,$2)", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(args[1]), CStringGetTextDatum(args[2])), NULL, false);
	}
	else if (!strcmp(args[0], "remove") || !strcmp(args[0], "rm"))
	{
		if (n != 2)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("dolt_backup remove requires a name")));
		run("DELETE FROM pg_catalog.pg_dolt_backup WHERE backupname=$1", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(args[1])), NULL, false);
		if (!SPI_processed)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("backup '%s' not found", args[1])));
	}
	else if (!strcmp(args[0], "sync") || !strcmp(args[0], "sync-url"))
	{
		char *url;

		standalone(fcinfo, true);
		if (n != 2)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("dolt_backup sync requires a name or URL")));
		url = args[1];
		if (!strcmp(args[0], "sync"))
		{
			run("SELECT backupurl FROM pg_dolt_backup WHERE backupname=$1", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(args[1])), NULL, true);
			if (!SPI_processed)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("backup '%s' not found", args[1])));
			url = strcell(0, 1);
		}
		bundle_write(url);
	}
	else if (!strcmp(args[0], "restore"))
	{
		bool force = n > 1 && !strcmp(args[1], "--force");

		standalone(fcinfo, true);
		if (n != (force ? 4 : 3))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("dolt_backup restore requires a URL and database name")));
		bundle_restore(args[force ? 2 : 1], args[force ? 3 : 2], force);
	}
	else
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("unrecognized dolt_backup parameter '%s'", args[0])));
	end_api(nest);
	PG_RETURN_INT64(0);
}

Datum
dolt_backup_noargs(PG_FUNCTION_ARGS)
{
	return dolt_backup(fcinfo);
}

/* ---- remotes and clusters: no transport ---- */

static Datum
no_transport(const char *procedure)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("%s is not supported: the server has no remote transport for manifest objects", procedure), errhint("Remote definitions are kept in dolt.remotes; use PostgreSQL replication or dumps to move data between servers.")));
	PG_RETURN_INT64(1);
}

Datum
dolt_fetch(PG_FUNCTION_ARGS)
{
	return no_transport("dolt_fetch");
}

Datum
dolt_push(PG_FUNCTION_ARGS)
{
	return no_transport("dolt_push");
}

Datum
dolt_pull(PG_FUNCTION_ARGS)
{
	return no_transport("dolt_pull");
}

Datum
dolt_clone(PG_FUNCTION_ARGS)
{
	return no_transport("dolt_clone");
}

Datum
dolt_assume_cluster_role(PG_FUNCTION_ARGS)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("dolt_assume_cluster_role is not supported: replication is PostgreSQL streaming replication, not a Dolt cluster")));
	PG_RETURN_INT64(1);
}

Datum
dolt_cluster_transition_to_standby(PG_FUNCTION_ARGS)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("dolt_cluster_transition_to_standby is not supported: replication is PostgreSQL streaming replication, not a Dolt cluster")));
	PG_RETURN_INT64(1);
}
