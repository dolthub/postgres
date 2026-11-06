/*-------------------------------------------------------------------------
 *
 * dg_fold.c
 *	  Background folding of the prolly delta store into new bases.
 *
 * Doltgrust folds a branch's delta into new trees in background workers,
 * never on the commit path. Here a launcher, which connects to no database,
 * watches the delta store (dg_delta.c): a tree is due a fold once it holds
 * dolt_fold_versions versions, once its oldest version has waited
 * dolt_fold_timeout, or, largest first, while the store holds more than half
 * its budget. For each database with trees due, the launcher starts a worker
 * connected to it, up to dolt_fold_workers at once; while the store is past
 * half its budget, one database may have several, which fold different trees.
 * A worker folds each due tree in a transaction of its own (dg_fold_tree),
 * then prunes what the fold holds, and exits once nothing is due.
 *
 * The launcher also prunes: a fold's versions go once every snapshot reads
 * past it, which may be well after the worker has exited.
 *
 * src/backend/access/dolt_prolly/dg_fold.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/doltdelta.h"
#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "dg.h"

int			dolt_fold_workers = 2;
int			dolt_fold_versions = 4096;
int			dolt_fold_batch = 4096;
bool		dolt_unlogged_folds = true;
int			dolt_fold_cache = DOLT_FOLD_CACHE_REPLACE;
int			dolt_fold_timeout = 5000;	/* ms */

/* How often the launcher looks for work. */
#define FOLD_NAP_MS 100

/* How long a worker waits for more work before it exits. */
#define FOLD_IDLE_MS 1000

/* Trees a worker or the launcher considers at once. */
#define FOLD_BATCH 64

/* How often the launcher prunes versions of rolled back writers. */
#define FOLD_SWEEP_MS 10000

void
DoltFoldLauncherRegister(void)
{
	BackgroundWorker bgw = {0};

	if (dolt_fold_workers == 0 || IsBinaryUpgrade)
		return;
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "DoltFoldLauncherMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "dolt fold launcher");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "dolt fold launcher");
	bgw.bgw_restart_time = 5;
	RegisterBackgroundWorker(&bgw);
}

/* Fold |tree| in a transaction of its own; false if it folded nothing or failed. */
static bool
fold_one(const DoltDeltaTree *tree)
{
	MemoryContext caller = CurrentMemoryContext;
	bool		folded = false;

	PG_TRY();
	{
		StartTransactionCommand();
		folded = dg_fold_tree(tree->space, tree->file, tree->branch);
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(caller);
		EmitErrorReport();
		FlushErrorState();
		AbortCurrentTransaction();
		folded = false;
	}
	PG_END_TRY();
	MemoryContextSwitchTo(caller);
	if (folded)
		(void) dolt_delta_prune(tree);
	return folded;
}

void
DoltFoldWorkerMain(Datum argument)
{
	Oid			db = DatumGetObjectId(argument);
	DoltDeltaTree *due = palloc(FOLD_BATCH * sizeof(DoltDeltaTree));
	TimestampTz idle_since = GetCurrentTimestamp();

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(db, BOOTSTRAP_SUPERUSERID, BGWORKER_BYPASS_ALLOWCONN);
	pgstat_report_appname("dolt fold worker");

	for (;;)
	{
		int			n;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		n = dolt_delta_due(db, dolt_fold_versions, dolt_fold_timeout, due, FOLD_BATCH);
		for (int i = 0; i < n; i++)
		{
			CHECK_FOR_INTERRUPTS();
			if (fold_one(&due[i]))
				idle_since = GetCurrentTimestamp();
		}
		if (!n && TimestampDifferenceExceeds(idle_since, GetCurrentTimestamp(), FOLD_IDLE_MS))
			break;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 FOLD_NAP_MS, WAIT_EVENT_PG_SLEEP);
		ResetLatch(MyLatch);
	}
	proc_exit(0);
}

/* A worker the launcher started, by database. */
typedef struct FoldWorker
{
	Oid			db;
	BackgroundWorkerHandle *handle;
} FoldWorker;

void
DoltFoldLauncherMain(Datum argument)
{
	FoldWorker *workers;
	int			nworkers = 0;
	DoltDeltaTree *due;
	TimestampTz swept = GetCurrentTimestamp();

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	workers = MemoryContextAllocZero(TopMemoryContext, Max(dolt_fold_workers, 1) * sizeof(FoldWorker));
	due = MemoryContextAlloc(TopMemoryContext, FOLD_BATCH * sizeof(DoltDeltaTree));

	for (;;)
	{
		int			n;
		int			per_db;
		bool		sweep;

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 FOLD_NAP_MS, WAIT_EVENT_PG_SLEEP);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* Drop the versions folds hold once every snapshot reads past them. */
		sweep = TimestampDifferenceExceeds(swept, GetCurrentTimestamp(), FOLD_SWEEP_MS);
		(void) dolt_delta_prune_all(sweep);
		if (sweep)
			swept = GetCurrentTimestamp();

		/* Forget workers that have exited. */
		for (int i = 0; i < nworkers;)
		{
			pid_t		pid;

			if (GetBackgroundWorkerPid(workers[i].handle, &pid) == BGWH_STOPPED)
			{
				pfree(workers[i].handle);
				workers[i] = workers[--nworkers];
			}
			else
				i++;
		}

		/*
		 * Start a worker for each database with trees due, while there is
		 * room. Under pressure, a database's trees fold in parallel: a
		 * worker folding a tree another holds skips to the next.
		 */
		n = dolt_delta_due(InvalidOid, dolt_fold_versions, dolt_fold_timeout, due, FOLD_BATCH);
		per_db = dolt_delta_pressure() > 0.5 ? dolt_fold_workers : 1;
		for (int i = 0; i < n && nworkers < dolt_fold_workers; i++)
		{
			BackgroundWorker bgw = {0};
			int			running = 0;

			for (int j = 0; j < nworkers; j++)
				running += workers[j].db == due[i].db;
			if (running >= per_db || running >= n)
				continue;
			bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
			bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
			bgw.bgw_restart_time = BGW_NEVER_RESTART;
			bgw.bgw_main_arg = ObjectIdGetDatum(due[i].db);
			snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
			snprintf(bgw.bgw_function_name, BGW_MAXLEN, "DoltFoldWorkerMain");
			snprintf(bgw.bgw_name, BGW_MAXLEN, "dolt fold worker for database %u", due[i].db);
			snprintf(bgw.bgw_type, BGW_MAXLEN, "dolt fold worker");
			if (!RegisterDynamicBackgroundWorker(&bgw, &workers[nworkers].handle))
				break;			/* no slot free now; try again later */
			workers[nworkers++].db = due[i].db;
		}
	}
}

/*
 * dolt_fold(): fold every tree of this database with pending versions now,
 * in the calling transaction, as the background workers would. Returns how
 * many trees it folded. The versions go once every snapshot reads past the
 * folds, which the launcher sees to.
 */
Datum
dolt_fold(PG_FUNCTION_ARGS)
{
	DoltDeltaTree *due = palloc(FOLD_BATCH * sizeof(DoltDeltaTree));
	int			folded = 0;
	int			n;

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to fold prolly trees")));
	n = dolt_delta_due(MyDatabaseId, 0, 0, due, FOLD_BATCH);
	for (int i = 0; i < n; i++)
		if (dg_fold_tree(due[i].space, due[i].file, due[i].branch))
			folded++;
	PG_RETURN_INT32(folded);
}

/*
 * dolt_delta_stats(): the delta store's totals, its budget, the shared memory
 * it has, how long writers have been throttled, and how many waited at the
 * limit.
 */
Datum
dolt_delta_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	Datum		values[8];
	bool		nulls[8] = {false};
	DoltDeltaTotals totals;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	dolt_delta_totals(&totals);
	values[0] = Int64GetDatum((int64) totals.trees);
	values[1] = Int64GetDatum((int64) totals.keys);
	values[2] = Int64GetDatum((int64) totals.versions);
	values[3] = Int64GetDatum((int64) totals.bytes);
	values[4] = Int64GetDatum((int64) totals.budget);
	values[5] = Int64GetDatum((int64) totals.allocated);
	values[6] = Float8GetDatum((double) totals.throttled_us / 1000.0);
	values[7] = Int64GetDatum((int64) totals.limit_waits);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}
