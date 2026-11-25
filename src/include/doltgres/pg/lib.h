/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The binding (pg's lib.rs, design item 17): the only way the fork's C
 * reaches Postgres, through the generated wrappers of calls.list, which catch
 * what Postgres raises, and the only way Postgres reaches the fork's C,
 * through the entries of entries.list, which pg_run turns into outcomes their
 * C thunks raise.
 */
#ifndef DOLTGRES_PG_LIB_H
#define DOLTGRES_PG_LIB_H

#ifndef FRONTEND

#include "calls.h"
#include "doltgres/std/any.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"

/* PgMayRaise lets a function call a wrapper that can raise; only an entry that may raise gets one. */
typedef struct PgMayRaise
{
	char		unused;
} PgMayRaise;

/* PgCatalog lets a function call a wrapper that can reach a catalog; only an entry with catalog access gets one. */
typedef struct PgCatalog
{
	char		unused;
} PgCatalog;

/* PgCaps are the capabilities of an entry that may raise but has no catalog access. */
typedef struct PgCaps
{
	PgMayRaise	raise;
} PgCaps;

/* PgCatalogCaps are the capabilities of an entry that may raise and has catalog access. */
typedef struct PgCatalogCaps
{
	PgMayRaise	raise;
	PgCatalog	catalog;
} PgCatalogCaps;

/*
 * PgCleanup is what an entry Postgres calls while aborting, releasing, or
 * exiting gets: no capability, so it compiles only against wrappers that
 * can't raise or reach a catalog.
 */
typedef struct PgCleanup
{
	char		unused;
} PgCleanup;

/* PgSealedToken is what only this crate can name, so only pg_run grants capabilities. */
typedef struct PgSealedToken
{
	char		unused;
} PgSealedToken;

/*
 * PgGrantOps is the Grant trait's table, implemented by the capabilities
 * pg_run gives an entry, which only this crate can make: ORDINARY is whether
 * the entry is an ordinary one, which retakes the reserve before it runs, and
 * grant makes the capabilities.
 */
typedef struct PgGrantOps
{
	void		(*grant) (PgSealedToken token, void *result);
	bool		ORDINARY;
} PgGrantOps;

/* PgWalRecord is a WAL record Postgres hands a resource manager's entry, read only through the record wrappers. */
typedef struct PgWalRecord
{
	XLogReaderState f0;
} PgWalRecord;

/* PgDesc is the text Postgres builds a WAL record's description in, written only through desc_append. */
typedef struct PgDesc
{
	StringInfoData f0;
} PgDesc;

/*
 * PgContext is a memory context Postgres hands an entry, passed on only to
 * the wrappers that allocate in it.
 */
typedef struct PgContext
{
	char		unused;
} PgContext;

/* PgCall is a function call Postgres hands an SQL function's entry, read only through the argument wrappers. */
typedef struct PgCall
{
	FunctionCallInfoBaseData f0;
} PgCall;

/*
 * PgSlot is a tuple table slot Postgres hands the fork's slot entries, read,
 * stored into, and filled only through the slot wrappers, each of which
 * checks that the slot is of the fork's kind before it reads the fork's
 * fields.
 */
typedef struct PgSlot
{
	char		unused;
} PgSlot;

/*
 * PgIcuLocale is a locale Postgres' ICU constructor made, which only its
 * wrapper makes; whoever holds it owns it until an entry gives it to
 * Postgres.
 */
typedef struct PgIcuLocale
{
	DgIcuLocale f0;
} PgIcuLocale;

/*
 * PgBuiltinLocale is a locale Postgres' builtin constructor made, which only
 * its wrapper makes; whoever holds it owns it until an entry gives it to
 * Postgres.
 */
typedef struct PgBuiltinLocale
{
	DgBuiltinLocale f0;
} PgBuiltinLocale;

/* PgLocale is a locale of any provider an entry gives Postgres, made only from a locale a constructor's wrapper made. */
typedef struct PgLocale
{
	DgLocale	f0;
} PgLocale;

/*
 * PgLibcLocale is the C library's locale Postgres made of a libc collation's
 * names, or none, which only an entry's thunk is handed and the locale it
 * builds keeps; Postgres owns what it points to.
 */
typedef struct PgLibcLocale
{
	DgLibcLocale f0;
} PgLibcLocale;

/*
 * PgIsNull is where a slot's entry says whether the system attribute it
 * returns is NULL, an index fetch's entry whether to call it again and
 * whether every version of its row is dead, or an index entry's flags of its
 * NULL values, written only by Postgres.
 */
typedef struct PgIsNull
{
	char		unused;
} PgIsNull;

/* PgRel is a relation Postgres hands an entry, its relcache entry, read only through the relation wrappers. */
typedef struct PgRel
{
	char		unused;
} PgRel;

/* PgText is a text value Postgres hands an entry, read only through the text wrappers. */
typedef struct PgText
{
	char		unused;
} PgText;

/*
 * PgTableRoutine is the fork's table access method routine, which only
 * table_routine makes and only the entry that chooses a relation's routine
 * returns.
 */
typedef struct PgTableRoutine
{
	const TableAmRoutine *f0;
} PgTableRoutine;

/*
 * PgPgString is a NUL-terminated string a wrapper allocated in one of
 * Postgres' memory contexts, which only wrappers make and an entry gives to
 * Postgres.
 */
typedef struct PgPgString
{
	char	   *f0;
} PgPgString;

/*
 * PG_INDEX_MAX_KEYS is the most columns an index, and so a primary key, has:
 * Postgres' own, the length of the array rel_key_columns fills.
 */
#define PG_INDEX_MAX_KEYS ((size_t) INDEX_MAX_KEYS)

/*
 * PgScan is a table scan's descriptor Postgres hands the fork's scan entries,
 * read and written only through the scan wrappers, each of which checks that
 * the scan is of the fork's table access method before it reads the fork's
 * fields.
 */
typedef struct PgScan
{
	char		unused;
} PgScan;

/* PgScanDesc is a table scan's descriptor scan_new made, which the scan's begin fills in and returns. */
typedef struct PgScanDesc
{
	TableScanDesc f0;
} PgScanDesc;

/*
 * PG_SCAN_DESC_NULL is for the generated default alone, what an entry's
 * thunk gets when the entry raises instead.
 */
#define PG_SCAN_DESC_NULL ((PgScanDesc) {NULL})

/* PgStream is ANALYZE's read stream of the blocks its block sampler picks, which the fork only takes block numbers from. */
typedef struct PgStream
{
	char		unused;
} PgStream;

/* PgSampleState is a sample scan's executor state Postgres hands its entries, read only by its sampling method. */
typedef struct PgSampleState
{
	char		unused;
} PgSampleState;

/* PgCounter is a bitmap scan's count of pages Postgres hands its entry, written only by Postgres. */
typedef struct PgCounter
{
	char		unused;
} PgCounter;

/*
 * PgIndexFetch is an index fetch's descriptor Postgres hands the fork's index
 * fetch entries, read only through the index fetch wrappers.
 */
typedef struct PgIndexFetch
{
	char		unused;
} PgIndexFetch;

/* PgIndexFetchDesc is an index fetch's descriptor fetch_new made, which the fetch's begin returns. */
typedef struct PgIndexFetchDesc
{
	IndexFetchTableData *f0;
} PgIndexFetchDesc;

/* PG_INDEX_FETCH_DESC_NULL is for the generated default alone, what an entry's thunk gets when the entry raises instead. */
#define PG_INDEX_FETCH_DESC_NULL ((PgIndexFetchDesc) {NULL})

/*
 * PgIndexScan is an index scan's descriptor Postgres hands the fork's btree
 * members, read and written only through the index_scan wrappers, which
 * reach the fork's scan state it was made with.
 */
typedef struct PgIndexScan
{
	char		unused;
} PgIndexScan;

/* PgIndexScanDesc is an index scan's descriptor index_scan_make made, which the scan's begin returns. */
typedef struct PgIndexScanDesc
{
	IndexScanDesc f0;
} PgIndexScanDesc;

/* PG_INDEX_SCAN_DESC_NULL is for the generated default alone, what an entry's thunk gets when the entry raises instead. */
#define PG_INDEX_SCAN_DESC_NULL ((PgIndexScanDesc) {NULL})

/* PgPlanner is the planner's state Postgres hands an index's cost estimate, passed on only to index_costs. */
typedef struct PgPlanner
{
	char		unused;
} PgPlanner;

/* PgIndexPath is the index path Postgres hands an index's cost estimate, read only through index_costs and index_path_open. */
typedef struct PgIndexPath
{
	char		unused;
} PgIndexPath;

/* PgBitmap is the TID bitmap a bitmap index scan fills, written only through bitmap_add. */
typedef struct PgBitmap
{
	char		unused;
} PgBitmap;

/* PgTid is a row's ItemPointer Postgres hands the fork's entries, read and written only through the tid wrappers. */
typedef struct PgTid
{
	char		unused;
} PgTid;

/* PgFileLocator is a relation's new file locator Postgres hands the fork's storage swaps, read only by their wrappers. */
typedef struct PgFileLocator
{
	char		unused;
} PgFileLocator;

/* PgFailure is the TM_FailureData a row's delete, update, or lock fills when it fails, written only through set_failure. */
typedef struct PgFailure
{
	char		unused;
} PgFailure;

/* PgLockModeOut is the LockTupleMode an update gives back, the row lock it took, written only through set_lock_mode. */
typedef struct PgLockModeOut
{
	char		unused;
} PgLockModeOut;

/*
 * PgUpdateIndexesOut is the TU_UpdateIndexes an update gives back, which
 * indexes its new row needs entries in, written only through
 * set_update_indexes.
 */
typedef struct PgUpdateIndexesOut
{
	char		unused;
} PgUpdateIndexesOut;

/*
 * PgWidths is the array of a relation's attribute widths Postgres hands a
 * size estimate, which may be null and which the fork only passes back to
 * Postgres.
 */
typedef struct PgWidths
{
	int32	   *f0;
} PgWidths;

/* PgSnapshot is a snapshot Postgres hands a scan's begin, which the fork only passes back to Postgres. */
typedef struct PgSnapshot
{
	char		unused;
} PgSnapshot;

/*
 * PgSnapshotView is a snapshot's fields as the fork's visibility reads them:
 * its type's number, its bounds widened to full xids against the next one,
 * its command, whether it was taken in recovery, whether its subtransactions
 * overflowed, and its running xids and subtransaction xids, lent from
 * Postgres' own arrays for the snapshot's life.
 */
typedef struct PgSnapshotView
{
	int32		kind;
	uint64		xmin;
	uint64		xmax;
	uint32		command;
	bool		recovery;
	bool		overflowed;
	const uint32 *xip;
	size_t		xip_len;
	const uint32 *subxip;
	size_t		subxip_len;
} PgSnapshotView;

/*
 * PgScanKeys is a scan's array of keys Postgres hands a scan's begin and
 * rescan, which the fork only passes back to Postgres, with its count.
 */
typedef struct PgScanKeys
{
	char		unused;
} PgScanKeys;

/*
 * PgBackgroundWorker is a background worker's registration Postgres hands the
 * fork's slot entries, which the fork only passes back to Postgres.
 */
typedef struct PgBackgroundWorker
{
	char		unused;
} PgBackgroundWorker;

/*
 * PgParallelScan is a parallel scan's shared state Postgres hands a scan's
 * begin, which the fork only passes back to Postgres.
 */
typedef struct PgParallelScan
{
	char		unused;
} PgParallelScan;

/* PgAlterCmd is an executed ALTER TABLE subcommand Postgres hands the fork's door, read only through its wrapper. */
typedef struct PgAlterCmd
{
	char		unused;
} PgAlterCmd;

/* PgBulkInsert is a bulk insert's state Postgres hands the fork's inserts, which the fork only receives. */
typedef struct PgBulkInsert
{
	char		unused;
} PgBulkInsert;

/* PgVacuumParams is a VACUUM's options Postgres hands the fork's relation_vacuum, which the fork only passes back. */
typedef struct PgVacuumParams
{
	char		unused;
} PgVacuumParams;

/*
 * PgStrategy is a VACUUM's buffer access strategy Postgres hands the fork's
 * relation_vacuum, which the fork only passes back.
 */
typedef struct PgStrategy
{
	char		unused;
} PgStrategy;

/*
 * PgDeadItems is VACUUM's dead items, a TidStore make_dead_items made for an
 * entry's own use, destroyed with TidStoreDestroy when its owner goes.
 */
typedef struct PgDeadItems
{
	TidStore   *f0;
} PgDeadItems;

/*
 * PgRewrite is a table rewrite's state that rewrite_begin made in an entry's
 * memory context, which the entry ends with end_rewrite and which an error's
 * abort frees with the context and its resource owner.
 */
typedef struct PgRewrite
{
	DgRewrite  *f0;
} PgRewrite;

/*
 * PgIndexStats is an index's bulk delete statistics a wrapper made, palloc'd
 * in the entry's memory context, which VACUUM carries to the index's next
 * bulk delete and its cleanup, each of which takes it; the cleanup frees it,
 * and the memory context any it leaves on an error.
 */
typedef struct PgIndexStats
{
	IndexBulkDeleteResult *f0;
} PgIndexStats;

/* PgIndexStatsOption is an Option<IndexStats>: has, and the statistics when it has them. */
typedef struct PgIndexStatsOption
{
	bool		has;
	PgIndexStats stats;
} PgIndexStatsOption;

/* PgSlotOps is the fork's kind of tuple table slot, which only slot_ops makes and only the table routine's slot_callbacks returns. */
typedef struct PgSlotOps
{
	const TupleTableSlotOps *f0;
} PgSlotOps;

/* PG_SLOT_OPS_NULL is for the generated default alone, what an entry's thunk gets when the entry raises instead. */
#define PG_SLOT_OPS_NULL ((PgSlotOps) {NULL})

/* PgHeapTuple is a heap tuple a slot wrapper formed, which an entry only returns. */
typedef struct PgHeapTuple
{
	HeapTuple	f0;
} PgHeapTuple;

/* PG_HEAP_TUPLE_NULL is for the generated default alone, what an entry's thunk gets when the entry raises instead. */
#define PG_HEAP_TUPLE_NULL ((PgHeapTuple) {NULL})

/* PgIndexDeleteOp is the TM_IndexDeleteOp an index hands its table's index_delete_tuples, read and written only through the index_delete_ wrappers. */
typedef struct PgIndexDeleteOp
{
	char		unused;
} PgIndexDeleteOp;

/* PgDatums is the array of an index entry's column values the executor hands aminsert, read only through index_datum_word and index_datum_bytes. */
typedef struct PgDatums
{
	char		unused;
} PgDatums;

/* PgIndexInfo is the IndexInfo the executor hands an index's insert and its cleanup, read and written only through the insert_record wrappers. */
typedef struct PgIndexInfo
{
	char		unused;
} PgIndexInfo;

/*
 * PgValidateState is the ValidateIndexState a concurrent index build's
 * validation hands the table's validate scan: the index's sorted tids and
 * the scan's counts, read and written only through the validate wrappers.
 */
typedef struct PgValidateState
{
	char		unused;
} PgValidateState;

/*
 * PgOwnedSnapshot is a NonVacuumable snapshot of a relation's horizon that
 * nonvacuumable_snapshot made in the current memory context for an entry's
 * own use, freed with pfree when its owner goes.
 */
typedef struct PgOwnedSnapshot
{
	void	   *f0;
} PgOwnedSnapshot;

/* PgRefRel is a reference to a relation's relcache entry that relation_opened took for an entry's own use, given back with RelationClose when its owner goes. */
typedef struct PgRefRel
{
	void	   *f0;
} PgRefRel;

/*
 * PgOwnedRel is a relation an entry opened for its own use, an index's table
 * that index_table_open opened under AccessShareLock or an index path's index
 * that index_path_open opened, closed with table_close_kept, which keeps its
 * lock, when its owner goes.
 */
typedef struct PgOwnedRel
{
	void	   *f0;
} PgOwnedRel;

/*
 * PgOwnedSlot is a slot of the fork's kind over a relation's columns that
 * make_slot made for an entry's own use, dropped with
 * ExecDropSingleTupleTableSlot when its owner goes.
 */
typedef struct PgOwnedSlot
{
	TupleTableSlot *f0;
} PgOwnedSlot;

/* PgBuild is an index build's state that build_state made, read and written only through the build_ wrappers. */
typedef struct PgBuild
{
	char		unused;
} PgBuild;

/*
 * PgOwnedBuild is an index build's state for an entry's own use, its executor
 * state for the index's expressions and predicate and the values its last
 * row formed, freed with build_free when its owner goes.
 */
typedef struct PgOwnedBuild
{
	DoltgresBuild *f0;
} PgOwnedBuild;

/* PgSort is an index build's sort of its entries' bytes that sort_begin made, read and written only through the sort_ wrappers. */
typedef struct PgSort
{
	char		unused;
} PgSort;

/*
 * PgOwnedSort is an index build's sort for an entry's own use, Postgres'
 * tuplesort over byte strings, which spills past maintenance_work_mem, ended
 * with sort_end when its owner goes.
 */
typedef struct PgOwnedSort
{
	DoltgresSort *f0;
} PgOwnedSort;

/* PgBuildResult is an index build's counts build_result made, which an entry only returns. */
typedef struct PgBuildResult
{
	IndexBuildResult *f0;
} PgBuildResult;

/* PG_BUILD_RESULT_NULL is for the generated default alone, what an entry's thunk gets when the entry raises instead. */
#define PG_BUILD_RESULT_NULL ((PgBuildResult) {NULL})

/* PgVacuumInfo is the IndexVacuumInfo VACUUM hands an index's bulk delete and cleanup, read only through the vacuum_ wrappers. */
typedef struct PgVacuumInfo
{
	char		unused;
} PgVacuumInfo;

/*
 * PgBulkCallback is the callback VACUUM hands an index's bulk delete to ask
 * which of its tids are dead, which the fork's own indexes never ask, since
 * no entry of theirs names a dead row.
 */
typedef struct PgBulkCallback
{
	IndexBulkDeleteCallback f0;
} PgBulkCallback;

/* PgBuildCallback is the callback an index build hands its table's index_build_range_scan, which the fork only passes back to Postgres. */
typedef struct PgBuildCallback
{
	IndexBuildCallback f0;
} PgBuildCallback;

/* PgParallelTarget is the fork's part of a parallel index scan's shared memory, which aminitparallelscan is handed and only index_parallel_init reads. */
typedef struct PgParallelTarget
{
	void	   *f0;
} PgParallelTarget;

/* PgBuildState is the state an index build hands its table's index_build_range_scan for its callback, which the fork only passes back to Postgres. */
typedef struct PgBuildState
{
	void	   *f0;
} PgBuildState;

/* PgMinimalTuple is a minimal tuple a slot wrapper formed, which an entry only returns. */
typedef struct PgMinimalTuple
{
	MinimalTuple f0;
} PgMinimalTuple;

/* PG_MINIMAL_TUPLE_NULL is for the generated default alone, what an entry's thunk gets when the entry raises instead. */
#define PG_MINIMAL_TUPLE_NULL ((PgMinimalTuple) {NULL})

/* PgAtt is an attribute of a slot's descriptor or of a row type, as slot_att and row_att pack it into a word. */
typedef struct PgAtt
{
	uint64		f0;
} PgAtt;

/* PgAttOption is an Option<Att>: has, and the attribute when it has one. */
typedef struct PgAttOption
{
	bool		has;
	PgAtt		att;
} PgAttOption;

/* PgDatum is a Postgres datum passed through the binding without being read. */
typedef struct PgDatum
{
	Datum		f0;
} PgDatum;

/* PG_DATUM_NULL is the datum an entry returns when it raises instead. */
#define PG_DATUM_NULL ((PgDatum) {0})

/*
 * PgPgError is an error the entry raises: one Postgres raised inside a
 * wrapper, which the entry raises again unchanged, or the fork's own, with
 * its SQLSTATE and message, which the entry raises with ereport.
 */
typedef struct PgPgError
{
	/*
	 * raw is Postgres' copy of the error it raised, or only the SQLSTATE of
	 * the fork's own.
	 */
	DgError		raw;

	/*
	 * fork is the message of the fork's own error, when has_fork, and nothing
	 * for one Postgres raised.
	 */
	bool		has_fork;
	StdString	fork;

	/*
	 * hint is the hint of the fork's own error, when it has one, its data
	 * NULL for none.
	 */
	StdStr		hint;

	/* raised is whether the entry took the error to raise. */
	bool		raised;
} PgPgError;

/* PgSlotsResult is the Result<&mut [&mut Slot], PgError> slots gives a multi-row insert's entry: ok, and the slots or the error. */
typedef struct PgSlotsResult
{
	bool		ok;
	union
	{
		struct
		{
			PgSlot	  **ptr;
			size_t		len;
		}			slots;
		PgPgError	error;
	}			u;
} PgSlotsResult;

/* PgToastSliceResult is the Result<&mut [u8], PgError> toast_slice gives a TOAST slice's entry: ok, and the bytes or the error. */
typedef struct PgToastSliceResult
{
	bool		ok;
	union
	{
		struct
		{
			uint8	   *ptr;
			size_t		len;
		}			slice;
		PgPgError	error;
	}			u;
} PgToastSliceResult;

/* PgPgStringOption is an Option<PgString>: has, and the string when it has one. */
typedef struct PgPgStringOption
{
	bool		has;
	PgPgString	string;
} PgPgStringOption;

/* PgLocaleOption is an Option<Locale>: has, and the locale when it has one. */
typedef struct PgLocaleOption
{
	bool		has;
	PgLocale	locale;
} PgLocaleOption;

/* PgTableRoutineOption is an Option<TableRoutine>: has, and the routine when it has one. */
typedef struct PgTableRoutineOption
{
	bool		has;
	PgTableRoutine routine;
} PgTableRoutineOption;

/* PgOutOfMemory is the panic payload of an allocation that failed. */
typedef struct PgOutOfMemory
{
	char		unused;
} PgOutOfMemory;

/* PG_MESSAGE_SIZE is the room the outcome gives a panic's message, its terminating NUL included. */
#define PG_MESSAGE_SIZE ((size_t) 1024)

/* PG_MAY_RAISE_TYPE_ID, PG_CATALOG_CAPS_TYPE_ID, PG_SNAPSHOT_TYPE_ID, and PG_PG_ERROR_TYPE_ID are the identities of the types of pg's that pghost makes into an Any. */
extern const StdTypeId PG_MAY_RAISE_TYPE_ID;
extern const StdTypeId PG_CATALOG_CAPS_TYPE_ID;
extern const StdTypeId PG_SNAPSHOT_TYPE_ID;
extern const StdTypeId PG_PG_ERROR_TYPE_ID;
extern const PgGrantOps PG_CAPS_GRANT_OPS;
extern const PgGrantOps PG_CATALOG_CAPS_GRANT_OPS;
extern const PgGrantOps PG_CLEANUP_GRANT_OPS;
extern pg_atomic_uint64 PG_FAILED_SIZE;

extern const PgMayRaise *pg_Caps_may_raise(const PgCaps *self);
extern const PgMayRaise *pg_CatalogCaps_may_raise(const PgCatalogCaps *self);
extern const PgCatalog *pg_CatalogCaps_catalog(const PgCatalogCaps *self);
extern PgLocale pg_Locale_FromIcuLocale_from(PgIcuLocale *locale);
extern PgLocale pg_Locale_FromBuiltinLocale_from(PgBuiltinLocale *locale);
extern DgLocale pg_Locale_into_raw(PgLocale *self);
extern const TableAmRoutine *pg_TableRoutine_into_raw(PgTableRoutine *self);
extern char *pg_PgString_into_raw(PgPgString *self);
extern PgScan *pg_ScanDesc_scan(PgScanDesc *self);
extern PgIndexFetch *pg_IndexScan_heap_fetch(PgIndexScan *self);
extern const PgRel *pg_IndexScan_index(const PgIndexScan *self);
extern const PgSnapshot *pg_IndexScan_snapshot(const PgIndexScan *self);
extern PgIndexScan *pg_IndexScanDesc_scan(PgIndexScanDesc *self);
extern bool pg_transaction_snapshot_view(const PgMayRaise *raise, PgSnapshotView *result, PgPgError *error);
extern bool pg_snapshot_view(const PgMayRaise *raise, const PgSnapshot *snapshot, PgSnapshotView *result,
							 PgPgError *error);
extern DgScanKeys pg_ScanKeys_with_count(const PgScanKeys *keys, int32 count);
extern void pg_DeadItems_drop(PgDeadItems *self);
extern PgIndexStatsOption pg_IndexStats_from_raw(IndexBulkDeleteResult *stats);
extern IndexBulkDeleteResult *pg_IndexStats_into_raw(PgIndexStats *self);
extern bool pg_slots(TupleTableSlot **slots, int32 count, PgSlot ***result, size_t *result_len, PgPgError *error);
extern bool pg_toast_slice(void *result_, int32 length, uint8 **result, size_t *result_len, PgPgError *error);
extern bool pg_OwnedSnapshot_nonvacuumable(const PgMayRaise *raise, const PgRel *rel, PgOwnedSnapshot *result,
										   PgPgError *error);
extern bool pg_OwnedSnapshot_dirty(const PgMayRaise *raise, PgOwnedSnapshot *result, PgPgError *error);
extern const PgSnapshot *pg_OwnedSnapshot_snapshot(const PgOwnedSnapshot *self);
extern void pg_OwnedSnapshot_drop(PgOwnedSnapshot *self);
extern bool pg_RefRel_opened(const PgMayRaise *raise, const PgCatalog *catalog, uint32 relid, bool *has_result,
							 PgRefRel *result, PgPgError *error);
extern const PgRel *pg_RefRel_rel(const PgRefRel *self);
extern void pg_RefRel_drop(PgRefRel *self);
extern bool pg_OwnedRel_index_table(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *index,
									PgOwnedRel *result, PgPgError *error);
extern bool pg_OwnedRel_path_index(const PgMayRaise *raise, const PgCatalog *catalog, PgIndexPath *path,
								   PgOwnedRel *result, PgPgError *error);
extern const PgRel *pg_OwnedRel_rel(const PgOwnedRel *self);
extern void pg_OwnedRel_drop(PgOwnedRel *self);
extern PgSlot *pg_OwnedSlot_slot(PgOwnedSlot *self);
extern void pg_OwnedSlot_drop(PgOwnedSlot *self);
extern PgBuild *pg_OwnedBuild_build(PgOwnedBuild *self);
extern void pg_OwnedBuild_drop(PgOwnedBuild *self);
extern PgSort *pg_OwnedSort_sort(PgOwnedSort *self);
extern void pg_OwnedSort_drop(PgOwnedSort *self);
extern PgAttOption pg_Att_of(uint64 word);
extern uint32 pg_Att_type_id(PgAtt self);
extern int16 pg_Att_length(PgAtt self);
extern bool pg_Att_by_value(PgAtt self);
extern bool pg_Att_dropped(PgAtt self);
extern size_t pg_Att_align(PgAtt self);
extern const uint8 *pg_borrowed(DgBytes bytes, size_t *result_len);
extern uint64 pg_Datum_word(PgDatum self);
extern PgPgError pg_PgError_new(DgError raw);
extern PgPgError pg_PgError_fork(const uint8 *sqlstate, StdString *message);
extern PgPgError pg_PgError_hinted(const uint8 *sqlstate, StdString *message, StdStr hint);
extern int32 pg_PgError_sqlerrcode(const PgPgError *self);
extern StdStr pg_PgError_message(const PgPgError *self);
extern void pg_PgError_into_raw(PgPgError *self, DgError *out_0, bool *has_out_1, StdString *out_1,
								StdStr *out_2);
extern void pg_PgError_drop(void *self_);
extern bool pg_warn_error(const PgMayRaise *raise, PgPgError *error_, const char *context, PgPgError *error);
extern bool pg_sort_key_of(const PgMayRaise *raise, const PgCatalog *catalog, uint32 collation,
						   const uint8 *text_, size_t text_len, StdVec *result, PgPgError *error);
extern void pg_replayed(uint64 *out_0, uint32 *out_1);
#if defined(USE_INJECTION_POINTS) || defined(DOLTGRES_PG_OUTSIDE_POSTGRES)
extern void pg_fail_allocations(bool fail);
extern void pg_fail_nth_allocation(uint32 nth);
extern uint64 pg_refused_size(void);
#endif
#if defined(DOLTGRES_TEST_PG) || defined(DOLTGRES_PG_OUTSIDE_POSTGRES)
extern size_t pg_heap_in_use(void);
#endif
extern bool pg_retake_reserve(void);
extern void pg_install(void);
extern void pg_written(char room[PG_MESSAGE_SIZE], const uint8 *text_, size_t text_len);
extern void pg_report(DgOutcome *outcome, struct StdCaught *ended);

#ifdef DOLTGRES_TEST_PG
extern void test_pg_tests_dropped_error_panics(void);
extern void test_pg_tests_only_ordinary_entries_retake_the_reserve(void);
extern void test_pg_tests_the_allocator_passes_requests_through(void);
extern void test_pg_tests_a_failed_allocation_frees_the_reserve(void);
extern void test_pg_tests_fork_errors_encode_their_sqlstate(void);
extern void test_pg_tests_borrowed_bytes_are_the_callers(void);
extern void test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free(void);
extern void test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count(void);
extern void test_pg_tests_a_toast_slices_bytes_are_past_its_header(void);
#endif

#endif							/* FRONTEND */

#endif							/* DOLTGRES_PG_LIB_H */
