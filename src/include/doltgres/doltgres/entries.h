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
 * The fork's entries (doltgres' entries.rs): the functions Postgres calls,
 * each the body of the entry of its name in pg's entries.list, which the
 * entry's exported side runs.
 */
#ifndef DOLTGRES_DOLTGRES_ENTRIES_H
#define DOLTGRES_DOLTGRES_ENTRIES_H

#include "doltgres/host/shared.h"

#ifndef FRONTEND
#include "doltgres/pg/lib.h"
#include "doltgres/access/heaptuple.h"
#include "doltgres/doltgres/scan.h"
#include "doltgres/std/c_str.h"
#include "doltgres/trees/lib.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"
#include "doltgres/txn/values.h"

/* doltgres_TupleUsizeAddressU64_t is a (usize, Address, u64): a value's field, its blob's root, and its length. */
typedef struct doltgres_TupleUsizeAddressU64_t
{
	size_t		f0;
	ChunksAddressAddress f1;
	uint64		f2;
} doltgres_TupleUsizeAddressU64_t;

extern void doltgres_entries_prepare(void);
extern bool doltgres_entries_doltgres_shared_preload(const PgCaps *caps, PgPgError *error);
extern bool doltgres_entries_doltgres_dolt_version(const PgCatalogCaps *caps, const PgCall *fcinfo, PgDatum *result,
												   PgPgError *error);
extern bool doltgres_entries_doltgres_shmem_request(const PgCaps *caps, void *arg, PgPgError *error);
extern bool doltgres_entries_doltgres_shmem_init(const PgCaps *caps, void *arg, PgPgError *error);
extern bool doltgres_entries_doltgres_shared_release(const PgCleanup *caps, int32 code, PgDatum arg, PgPgError *error);
extern bool doltgres_entries_doltgres_rmgr_redo(const PgCaps *caps, const PgWalRecord *record, PgPgError *error);
extern bool doltgres_entries_doltgres_rmgr_desc(const PgCaps *caps, PgDesc *buf, const PgWalRecord *record,
												PgPgError *error);
extern bool doltgres_entries_doltgres_rmgr_identify(const PgCaps *caps, uint8 info, const char **result,
													PgPgError *error);
extern bool doltgres_entries_doltgres_shared_exit(const PgCaps *caps, int32 code, PgDatum arg, PgPgError *error);
extern bool doltgres_entries_doltgres_cache_release(const PgCleanup *caps, uint64 res, PgPgError *error);
extern bool doltgres_entries_doltgres_owned_release(const PgCleanup *caps, uint64 res, PgPgError *error);
#ifdef USE_INJECTION_POINTS
/* doltgres_entries_testing_TestRecord_OWNED_OPS is the test records' Owned, which testing.c defines. */
extern const HostOwnedOwnedOps doltgres_entries_testing_TestRecord_OWNED_OPS;
#endif
extern bool doltgres_entries_doltgres_process_start(const PgCaps *caps, PgPgError *error);
extern bool doltgres_entries_doltgres_libc_locale(const PgCatalogCaps *caps, uint32 collid, const PgContext *context,
												  PgLocaleOption *result, PgPgError *error);
extern bool doltgres_entries_doltgres_libc_new_locale(const PgCatalogCaps *caps, uint32 collid,
													  const PgContext *context, PgLibcLocale loc, PgLocaleOption *result, PgPgError *error);
extern bool doltgres_entries_doltgres_new_collation(const PgCatalogCaps *caps, uint32 collid, const void *from,
													bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_icu_locale(const PgCatalogCaps *caps, uint32 collid, PgPgStringOption *result,
												 PgPgError *error);
extern bool doltgres_entries_doltgres_builtin_locale(const PgCatalogCaps *caps, uint32 collid, PgPgStringOption *result,
													 PgPgError *error);
extern bool doltgres_entries_doltgres_libc_collversion(const PgCatalogCaps *caps, const char *collcollate,
													   PgPgStringOption *result, PgPgError *error);
extern bool doltgres_entries_doltgres_checkpoint(const PgCaps *caps, uint64 redo, PgPgError *error);
extern bool doltgres_entries_doltgres_kept_segment(const PgCaps *caps, int64 segment, int32 segment_size, int64 *result,
												   PgPgError *error);
extern bool doltgres_entries_doltgres_startup_open(const PgCaps *caps, uint64 redo, uint64 label, PgPgError *error);
extern bool doltgres_entries_doltgres_createdb(const PgCatalogCaps *caps, uint32 template_id, uint32 database_id,
											   uint32 tablespace_id, PgPgError *error);
extern bool doltgres_entries_doltgres_dropdb(const PgCatalogCaps *caps, uint32 database_id, PgPgError *error);
extern bool doltgres_entries_doltgres_db_file_conflict(const PgCatalogCaps *caps, uint32 database_id, bool *result,
													   PgPgError *error);
extern bool doltgres_entries_doltgres_movedb_copy(const PgCatalogCaps *caps, uint32 database_id, uint32 from_tablespace,
												  uint32 to_tablespace, PgPgError *error);
extern bool doltgres_entries_doltgres_movedb_finish(const PgCatalogCaps *caps, uint32 database_id,
													uint32 from_tablespace, uint32 to_tablespace, PgPgError *error);
extern bool doltgres_entries_doltgres_slot_init(const PgCaps *caps, PgSlot *slot, PgPgError *error);
extern bool doltgres_entries_doltgres_slot_release(const PgCleanup *caps, PgSlot *slot, PgPgError *error);
extern bool doltgres_entries_doltgres_slot_clear(const PgCleanup *caps, PgSlot *slot, PgPgError *error);
extern bool doltgres_entries_doltgres_slot_getsomeattrs(const PgCaps *caps, PgSlot *slot, int32 natts,
														PgPgError *error);
extern bool doltgres_entries_doltgres_slot_getsysattr(const PgCaps *caps, PgSlot *slot, int32 attnum, PgIsNull *isnull,
													  PgDatum *result, PgPgError *error);
extern bool doltgres_entries_doltgres_slot_is_current_xact_tuple(const PgCaps *caps, PgSlot *slot, bool *result,
																 PgPgError *error);
extern bool doltgres_entries_doltgres_slot_materialize(const PgCaps *caps, PgSlot *slot, PgPgError *error);
extern bool doltgres_entries_doltgres_slot_copyslot(const PgCaps *caps, PgSlot *dstslot, PgSlot *srcslot,
													PgPgError *error);
extern bool doltgres_entries_doltgres_slot_copy_heap_tuple(const PgCaps *caps, PgSlot *slot, PgHeapTuple *result,
														   PgPgError *error);
extern bool doltgres_entries_doltgres_slot_copy_minimal_tuple(const PgCaps *caps, PgSlot *slot, size_t extra,
															  PgMinimalTuple *result, PgPgError *error);
extern bool doltgres_entries_doltgres_table_am_routine(const PgCatalogCaps *caps, const PgRel *relation,
													   PgTableRoutineOption *result, PgPgError *error);
extern bool doltgres_entries_doltgres_relation_creating(const PgCaps *caps, uint32 relid, uint32 relrewrite,
														PgPgError *error);
extern bool doltgres_entries_doltgres_table_slot_callbacks(const PgCaps *caps, const PgRel *rel, PgSlotOps *result,
														   PgPgError *error);
extern bool doltgres_entries_doltgres_scan_begin(const PgCatalogCaps *caps, const PgRel *relation,
												 const PgSnapshot *snapshot, int32 nkeys, const PgScanKeys *key, const PgParallelScan *pscan, uint32 flags,
												 PgScanDesc *result, PgPgError *error);
extern bool doltgres_entries_doltgres_scan_getnextslot(const PgCatalogCaps *caps, PgScan *scan, int32 direction,
													   PgSlot *slot, bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_scan_rescan(const PgCaps *caps, PgScan *scan, const PgScanKeys *key,
												  bool set_params, bool allow_strat, bool allow_sync, bool allow_pagemode, PgPgError *error);
extern bool doltgres_entries_doltgres_scan_set_tidrange(const PgCaps *caps, PgScan *scan, PgTid *mintid, PgTid *maxtid,
														PgPgError *error);
extern bool doltgres_entries_doltgres_scan_getnextslot_tidrange(const PgCatalogCaps *caps, PgScan *scan,
																int32 direction, PgSlot *slot, bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_relation_size(const PgCatalogCaps *caps, const PgRel *rel, int32 forkNumber,
													uint64 *result, PgPgError *error);
extern bool doltgres_entries_doltgres_relation_estimate_size(const PgCatalogCaps *caps, const PgRel *rel,
															 PgWidths attr_widths, uint32 *pages, float8 *tuples, float8 *allvisfrac, PgPgError *error);
extern bool doltgres_entries_doltgres_parallelscan_estimate(const PgCaps *caps, const PgRel *rel, size_t *result,
															PgPgError *error);
extern bool doltgres_entries_doltgres_parallelscan_initialize(const PgCatalogCaps *caps, const PgRel *rel,
															  const PgParallelScan *pscan, size_t *result, PgPgError *error);
extern bool doltgres_entries_doltgres_parallelscan_reinitialize(const PgCatalogCaps *caps, const PgRel *rel,
																const PgParallelScan *pscan, PgPgError *error);
extern bool doltgres_entries_doltgres_scan_end(const PgCaps *caps, PgScan *scan, PgPgError *error);
extern bool doltgres_entries_doltgres_tuple_fetch_row_version(const PgCatalogCaps *caps, const PgRel *relation,
															  PgTid *tid, const PgSnapshot *snapshot, PgSlot *slot, bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_tuple_tid_valid(const PgCaps *caps, PgScan *scan, PgTid *tid, bool *result,
													  PgPgError *error);
extern bool doltgres_entries_doltgres_tuple_get_latest_tid(const PgCatalogCaps *caps, PgScan *scan, PgTid *tid,
														   PgPgError *error);
extern bool doltgres_entries_doltgres_tuple_satisfies_snapshot(const PgCatalogCaps *caps, const PgRel *relation,
															   PgSlot *slot, const PgSnapshot *snapshot, bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_xact_ended(const PgCatalogCaps *caps, int32 event, void *arg, PgPgError *error);
extern bool doltgres_entries_doltgres_tuple_insert(const PgCatalogCaps *caps, const PgRel *rel, PgSlot *slot,
												   uint32 cid, uint32 options, const PgBulkInsert *bistate, PgPgError *error);
extern bool doltgres_entries_doltgres_multi_insert(const PgCatalogCaps *caps, const PgRel *rel, PgSlotsResult *slots,
												   uint32 cid, uint32 options, const PgBulkInsert *bistate, PgPgError *error);
extern bool doltgres_entries_doltgres_unlogged_cleanup(const PgCaps *caps, PgPgError *error);
extern bool doltgres_entries_doltgres_subxact_ended(const PgCaps *caps, int32 event, uint32 mySubid, uint32 parentSubid,
													void *arg, PgPgError *error);
extern bool doltgres_entries_doltgres_index_fetch_begin(const PgCaps *caps, const PgRel *rel, uint32 flags,
														PgIndexFetchDesc *result, PgPgError *error);
extern bool doltgres_entries_doltgres_index_fetch_reset(const PgCaps *caps, PgIndexFetch *fetch, PgPgError *error);
extern bool doltgres_entries_doltgres_index_fetch_end(const PgCaps *caps, PgIndexFetch *fetch, PgPgError *error);
extern bool doltgres_entries_doltgres_index_fetch_tuple(const PgCatalogCaps *caps, PgIndexFetch *fetch, PgTid *tid,
														const PgSnapshot *snapshot, PgSlot *slot, PgIsNull *call_again, PgIsNull *all_dead, bool *result,
														PgPgError *error);
extern bool doltgres_entries_doltgres_relation_fetch_toast_slice(const PgCatalogCaps *caps, const PgRel *toastrel,
																 uint32 valueid, int32 attrsize, int32 sliceoffset, PgToastSliceResult *result, PgPgError *error);
extern bool doltgres_entries_doltgres_scan_bitmap_next_tuple(const PgCatalogCaps *caps, PgScan *scan, PgSlot *slot,
															 PgIsNull *recheck, PgCounter *lossy_pages, PgCounter *exact_pages, bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_scan_sample_next_block(const PgCatalogCaps *caps, PgScan *scan,
															 PgSampleState *scanstate, bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_scan_sample_next_tuple(const PgCatalogCaps *caps, PgScan *scan,
															 PgSampleState *scanstate, PgSlot *slot, bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_scan_analyze_next_block(const PgCaps *caps, PgScan *scan, PgStream *stream,
															  bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_scan_analyze_next_tuple(const PgCatalogCaps *caps, PgScan *scan, float8 *liverows,
															  float8 *deadrows, PgSlot *slot, bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_tuple_delete(const PgCatalogCaps *caps, const PgRel *rel, PgTid *tid, uint32 cid,
												   uint32 options, const PgSnapshot *snapshot, const PgSnapshot *crosscheck, bool wait, PgFailure *tmfd,
												   int32 *result, PgPgError *error);
extern bool doltgres_entries_doltgres_tuple_update(const PgCatalogCaps *caps, const PgRel *rel, PgTid *otid,
												   PgSlot *slot, uint32 cid, uint32 options, const PgSnapshot *snapshot, const PgSnapshot *crosscheck, bool wait,
												   PgFailure *tmfd, PgLockModeOut *lockmode, PgUpdateIndexesOut *update_indexes, int32 *result, PgPgError *error);
extern bool doltgres_entries_doltgres_tuple_lock(const PgCatalogCaps *caps, const PgRel *rel, PgTid *tid,
												 const PgSnapshot *snapshot, PgSlot *slot, uint32 cid, int32 mode, int32 wait_policy, uint8 flags,
												 PgFailure *tmfd, int32 *result, PgPgError *error);
extern bool doltgres_entries_doltgres_tuple_insert_speculative(const PgCatalogCaps *caps, const PgRel *rel,
															   PgSlot *slot, uint32 cid, uint32 options, const PgBulkInsert *bistate, uint32 specToken, PgPgError *error);
extern bool doltgres_entries_doltgres_tuple_complete_speculative(const PgCatalogCaps *caps, const PgRel *rel,
																 PgSlot *slot, uint32 specToken, bool succeeded, PgPgError *error);
extern bool doltgres_entries_doltgres_index_delete_tuples(const PgCatalogCaps *caps, const PgRel *rel,
														  PgIndexDeleteOp *delstate, uint32 *result, PgPgError *error);
extern bool doltgres_entries_doltgres_commit_registered(const PgCleanup *caps, PgPgError *error);
extern bool doltgres_entries_doltgres_redo_committed(const PgCaps *caps, uint32 xid, uint64 end, PgPgError *error);
extern bool doltgres_entries_doltgres_prepared_committing(const PgCaps *caps, uint32 xid, PgPgError *error);
extern bool doltgres_entries_doltgres_prepared_registered(const PgCleanup *caps, PgPgError *error);
extern bool doltgres_entries_doltgres_index_routine(const PgCatalogCaps *caps, const PgRel *relation, PgPgError *error);
extern bool doltgres_entries_doltgres_relation_vacuum(const PgCatalogCaps *caps, const PgRel *rel,
													  const PgVacuumParams *params, const PgStrategy *bstrategy, PgPgError *error);
extern bool doltgres_entries_doltgres_index_insert(const PgCatalogCaps *caps, const PgRel *index,
												   const PgDatums *values, PgIsNull *isnull, PgTid *heap_tid, const PgRel *heap, int32 check_unique,
												   bool index_unchanged, PgIndexInfo *info, bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_index_insertcleanup(const PgCatalogCaps *caps, const PgRel *index,
														  PgIndexInfo *info, PgPgError *error);
extern bool doltgres_entries_doltgres_index_buildempty(const PgCatalogCaps *caps, const PgRel *index, PgPgError *error);
extern bool doltgres_entries_doltgres_old_entries_invalidated(const PgCleanup *caps, PgDatum arg, uint32 relid,
															  PgPgError *error);
extern bool doltgres_entries_doltgres_index_build_range_scan(const PgCatalogCaps *caps, const PgRel *table_rel,
															 const PgRel *index_rel, PgIndexInfo *index_info, bool allow_sync, bool anyvisible, bool progress,
															 uint32 start_blockno, uint32 numblocks, PgBuildCallback callback, PgBuildState callback_state,
															 PgScan *build_scan, float8 *result, PgPgError *error);
extern bool doltgres_entries_doltgres_index_validate_scan(const PgCatalogCaps *caps, const PgRel *table_rel,
														  const PgRel *index_rel, PgIndexInfo *index_info, const PgSnapshot *snapshot, PgValidateState *state,
														  PgPgError *error);
extern bool doltgres_entries_doltgres_index_build(const PgCatalogCaps *caps, const PgRel *heap, const PgRel *index,
												  PgIndexInfo *info, PgBuildResult *result, PgPgError *error);
extern bool doltgres_entries_doltgres_index_build_callback(const PgCatalogCaps *caps, const PgRel *index, PgTid *tid,
														   const PgDatums *values, PgIsNull *isnull, bool alive, PgBuildState state, PgPgError *error);
extern bool doltgres_entries_doltgres_index_bulkdelete(const PgCaps *caps, PgVacuumInfo *info,
													   PgIndexStatsOption *stats, PgBulkCallback callback, void *callback_state, PgIndexStatsOption *result,
													   PgPgError *error);
extern bool doltgres_entries_doltgres_index_vacuumcleanup(const PgCatalogCaps *caps, PgVacuumInfo *info,
														  PgIndexStatsOption *stats, PgIndexStatsOption *result, PgPgError *error);
extern bool doltgres_entries_doltgres_backup_started(const PgCaps *caps, PgPgError *error);
extern bool doltgres_entries_doltgres_backup_checkpointed(const PgCaps *caps, uint64 checkpoint, PgPgError *error);
extern bool doltgres_entries_doltgres_backup_ended(const PgCleanup *caps, PgPgError *error);
extern bool doltgres_entries_doltgres_collect(const PgCatalogCaps *caps, PgDatum main_arg, PgPgError *error);
extern bool doltgres_entries_doltgres_collector_released(const PgCleanup *caps, int32 code, PgDatum arg,
														 PgPgError *error);
extern bool doltgres_entries_doltgres_relation_set_new_filelocator(const PgCatalogCaps *caps, const PgRel *rel,
																   const PgFileLocator *newrlocator, char persistence, uint32 *freezeXid, uint32 *minmulti, PgPgError *error);
extern bool doltgres_entries_doltgres_relation_nontransactional_truncate(const PgCatalogCaps *caps, const PgRel *rel,
																		 PgPgError *error);
extern bool doltgres_entries_doltgres_relation_copy_for_cluster(const PgCatalogCaps *caps, const PgRel *old_table,
																const PgRel *new_table, const PgRel *old_index, bool use_sort, uint32 oldest_xmin, const PgSnapshot *snapshot,
																uint32 *xid_cutoff, uint32 *multi_cutoff, float8 *num_tuples, float8 *tups_vacuumed,
																float8 *tups_recently_dead, PgPgError *error);
extern bool doltgres_entries_doltgres_parallel_based(const PgCatalogCaps *caps, const PgRel *rel,
													 const PgParallelScan *pscan, const PgSnapshot *snapshot, PgPgError *error);
extern bool doltgres_entries_doltgres_redo_done(const PgCaps *caps, PgPgError *error);
extern bool doltgres_entries_doltgres_redo_aborted(const PgCaps *caps, const PgWalRecord *record, PgPgError *error);
extern bool doltgres_entries_doltgres_running_replayed(const PgCaps *caps, const PgWalRecord *record, PgPgError *error);
extern bool doltgres_entries_doltgres_relation_copy_data(const PgCatalogCaps *caps, const PgRel *rel,
														 const PgFileLocator *newrlocator, PgPgError *error);
extern bool doltgres_entries_doltgres_index_copy_data(const PgCaps *caps, const PgRel *rel,
													  const PgFileLocator *newrlocator, PgPgError *error);
extern bool doltgres_entries_doltgres_fold_launch(const PgCatalogCaps *caps, PgDatum main_arg, PgPgError *error);
extern bool doltgres_entries_doltgres_fold_work(const PgCatalogCaps *caps, PgDatum main_arg, PgPgError *error);
extern bool doltgres_entries_doltgres_fold_relation(const PgCatalogCaps *caps, const PgRel *rel, const PgRel *heap,
													bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_fork_worker(const PgCleanup *caps, const PgBackgroundWorker *worker, bool *result,
												  PgPgError *error);
extern bool doltgres_entries_doltgres_worker_refused(const PgCleanup *caps, const PgBackgroundWorker *worker,
													 bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_recovery_refused(const PgCaps *caps, PgPgError *error);
extern bool doltgres_entries_doltgres_restartpoint_held(const PgCaps *caps, uint64 redo, bool *result,
														PgPgError *error);
extern bool doltgres_entries_doltgres_relation_dropped(const PgCatalogCaps *caps, const PgRel *relation,
													   PgPgError *error);
extern bool doltgres_entries_doltgres_altered(const PgCatalogCaps *caps, const PgRel *relation, const PgAlterCmd *cmd,
											  PgPgError *error);
extern bool doltgres_entries_doltgres_object_dropped(const PgCatalogCaps *caps, uint32 class_id, uint32 object_id,
													 PgPgError *error);
extern bool doltgres_entries_doltgres_namespace_created(const PgCatalogCaps *caps, uint32 nspoid, const char *nspname,
														bool istemp, PgPgError *error);
extern bool doltgres_entries_doltgres_index_gettreeheight(const PgCatalogCaps *caps, const PgRel *index, int32 *result,
														  PgPgError *error);
extern bool doltgres_entries_doltgres_index_beginscan(const PgCatalogCaps *caps, const PgRel *index, int32 nkeys,
													  int32 norderbys, PgIndexScanDesc *result, PgPgError *error);
extern bool doltgres_entries_doltgres_index_rescan(const PgCaps *caps, PgIndexScan *scan, const PgScanKeys *keys,
												   int32 nkeys, const PgScanKeys *orderbys, int32 norderbys, PgPgError *error);
extern bool doltgres_entries_doltgres_index_gettuple(const PgCatalogCaps *caps, PgIndexScan *scan, int32 direction,
													 bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_index_endscan(const PgCaps *caps, PgIndexScan *scan, PgPgError *error);
extern bool doltgres_entries_doltgres_index_getbitmap(const PgCatalogCaps *caps, PgIndexScan *scan, PgBitmap *tbm,
													  int64 *result, PgPgError *error);
extern bool doltgres_entries_doltgres_index_markpos(const PgCaps *caps, PgIndexScan *scan, PgPgError *error);
extern bool doltgres_entries_doltgres_index_restrpos(const PgCatalogCaps *caps, PgIndexScan *scan, PgPgError *error);
extern bool doltgres_entries_doltgres_index_costestimate(const PgCatalogCaps *caps, PgPlanner *root, PgIndexPath *path,
														 float8 loop_count, float8 *startup, float8 *total, float8 *selectivity, float8 *correlation, float8 *pages,
														 PgPgError *error);
extern bool doltgres_entries_doltgres_index_estimateparallelscan(const PgCaps *caps, const PgRel *index, int32 nkeys,
																 int32 norderbys, size_t *result, PgPgError *error);
extern bool doltgres_entries_doltgres_index_initparallelscan(const PgCaps *caps, PgParallelTarget *target,
															 PgPgError *error);
extern bool doltgres_entries_doltgres_index_parallelrescan(const PgCaps *caps, PgIndexScan *scan, PgPgError *error);
extern bool doltgres_entries_doltgres_index_only_tid(const PgCatalogCaps *caps, PgIndexScan *scan, PgTid *tid,
													 PgPgError *error);
extern bool doltgres_entries_doltgres_index_holds(const PgCatalogCaps *caps, const PgRel *index, const PgDatums *values,
												  PgIsNull *isnull, PgTid *heap_tid, const PgRel *heap, PgIndexInfo *info, bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_storage_unlinked(const PgCaps *caps, const PgFileLocator *locator,
													   PgPgError *error);
extern bool doltgres_entries_doltgres_relation_needs_toast_table(const PgCaps *caps, const PgRel *rel, bool *result,
																 PgPgError *error);
extern bool doltgres_entries_doltgres_relation_toast_am(const PgCaps *caps, const PgRel *rel, uint32 *result,
														PgPgError *error);
extern bool doltgres_entries_doltgres_index_blocks(const PgCatalogCaps *caps, const PgRel *relation, int32 forkNum,
												   uint32 *blocks, bool *result, PgPgError *error);
extern bool doltgres_entries_doltgres_relation_size_added(const PgCatalogCaps *caps, const PgRel *rel,
														  const PgText *forkName, int64 size, int64 *result, PgPgError *error);
extern bool doltgres_entries_doltgres_database_size(const PgCaps *caps, uint32 dbOid, int64 size, int64 *result,
													PgPgError *error);
extern bool doltgres_entries_doltgres_tablespace_size(const PgCaps *caps, uint32 tblspcOid, int64 size, int64 *result,
													  PgPgError *error);
extern bool doltgres_entries_doltgres_altering(const PgCatalogCaps *caps, const PgRel *relation, const PgAlterCmd *cmd,
											   int32 rewrite, PgPgError *error);
extern bool doltgres_entries_doltgres_column_dropped(const PgCatalogCaps *caps, uint32 relid, int32 attnum,
													 PgPgError *error);
extern bool doltgres_entries_doltgres_toast_created(const PgCatalogCaps *caps, const PgRel *relation, uint32 toast,
													PgPgError *error);
extern bool doltgres_entries_doltgres_key_named(const PgCatalogCaps *caps, const PgRel *table, bool primary,
												PgPgError *error);
extern bool doltgres_entries_doltgres_key_level_opened(const PgCaps *caps, PgPgError *error);
extern bool doltgres_entries_doltgres_key_level_settled(const PgCatalogCaps *caps, PgPgError *error);
extern bool doltgres_entries_doltgres_key_settle(const PgCatalogCaps *caps, uint32 table, PgPgError *error);
extern bool doltgres_entries_doltgres_key_settle_table(const PgCatalogCaps *caps, const PgRel *rel, PgPgError *error);
extern bool doltgres_entries_doltgres_key_settle_commit(const PgCatalogCaps *caps, int32 event, void *arg,
														PgPgError *error);
extern bool doltgres_entries_doltgres_new_heap(const PgCatalogCaps *caps, uint32 new_table, const PgRel *old_table,
											   PgPgError *error);

/* doltgres_TupleUsizeU32_t is a (usize, u32): a value's column or field and its value id. */
typedef struct doltgres_TupleUsizeU32_t
{
	size_t		f0;
	uint32		f1;
} doltgres_TupleUsizeU32_t;

/* doltgres_TupleUsizeU32Entry_t is a (usize, u32, Entry): a kept value's column, its value id, and its map entry. */
typedef struct doltgres_TupleUsizeU32Entry_t
{
	size_t		f0;
	uint32		f1;
	TxnValuesEntry f2;
} doltgres_TupleUsizeU32Entry_t;

/*
 * DoltgresEntriesToasted is Toasted: what a row's out-of-line values give
 * its write, their TOAST relation, its storage's state, each stored value's
 * map entry, their value ids in the row's order, and the places among them
 * of the values an update kept.
 */
typedef struct DoltgresEntriesToasted
{
	TxnRecordToast toast;
	HostSharedHeld state;
	StdVec		entries;
	StdVec		ids;
	StdVec		kept;
} DoltgresEntriesToasted;

/*
 * DoltgresEntriesKeeping is Keeping: what an update keeps out of line from
 * the row it replaces, the TOAST relation and its state, and each kept
 * value's column, value id, and map entry, in column order.
 */
typedef struct DoltgresEntriesKeeping
{
	TxnRecordToast toast;
	HostSharedHeld state;
	StdVec		values;
} DoltgresEntriesKeeping;

/* DoltgresEntriesByTag is By's variants. */
typedef enum DoltgresEntriesByTag
{
	DOLTGRES_ENTRIES_BY_TID,
	DOLTGRES_ENTRIES_BY_KEY,
} DoltgresEntriesByTag;

/* DoltgresEntriesBy is By: what a fetch names its row by, a tuple id, or a row key the fork's index method named. */
typedef struct DoltgresEntriesBy
{
	DoltgresEntriesByTag tag;
	uint64		tuple_id;
	const uint8 *key;
	size_t		key_len;
} DoltgresEntriesBy;

/* DoltgresEntriesReading is Reading: which rows a scan's read gives, all of its base's, or its tid range's. */
typedef enum DoltgresEntriesReading
{
	DOLTGRES_ENTRIES_READING_ALL,
	DOLTGRES_ENTRIES_READING_RANGE,
} DoltgresEntriesReading;

/* The locale entries' helpers the crate's other entries call as well. */
extern PgPgError doltgres_entries_internal(const char *message, size_t message_len);
extern bool doltgres_entries_c_string(StdString *owned, StdCString *result, PgPgError *error);
extern PgLocale doltgres_entries_kept(PgLocale *locale, bool has_loc, PgLibcLocale loc);
extern HostStorage doltgres_entries_storage(const PgRel *relation);
extern bool doltgres_entries_making(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel, TxnStateFound *result,
									PgPgError *error);
extern bool doltgres_entries_recorded(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
									  const HostSharedReader *reading, const PgRel *rel, TxnBaseBase *result,
									  PgPgError *error);
extern bool doltgres_entries_repository(HostIoIo *io, StdArcInner **result, PgPgError *error);
extern PgPgError doltgres_entries_storage_error(ChunksErrorError *error);
extern bool doltgres_entries_blob_bytes(HostIoIo *io, const uint8 address[32], StdVec *result, PgPgError *error);
extern bool doltgres_entries_current(HostIoIo *io, uint32 command, TxnMemtableVersionWriter *result_0,
									 TxnStatusRoomed *result_1, PgPgError *error);
extern TxnInsertPage doltgres_entries_page(void);
extern bool doltgres_entries_index_making(HostIoIo *io, const PgRel *index, TxnStateFound *result, PgPgError *error);
extern bool doltgres_entries_shape(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
								   const HostSharedReader *reading, const PgRel *table, const PgRel *index,
								   TreesKeyOrder *result_0, bool *result_1, PgPgError *error);
extern bool doltgres_entries_stored(const PgMayRaise *raise, PgSlot *slot, const PgRel *relation,
									const DoltgresScanRow *row, PgPgError *error);
extern void doltgres_entries_Toasted_drop(DoltgresEntriesToasted *self);
extern void doltgres_entries_Keeping_drop(DoltgresEntriesKeeping *self);
extern bool doltgres_entries_toast_state(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
										 const HostSharedReader *reading, const PgRel *rel, TxnRecordToast *result_0,
										 HostSharedHeld *result_1, PgPgError *error);
extern bool doltgres_entries_toasted(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
									 const HostSharedReader *reading, const PgRel *rel, AccessHeaptuplePlacing *placing,
									 DoltgresEntriesKeeping *keeping, bool *has_result, DoltgresEntriesToasted *result,
									 PgPgError *error);
extern bool doltgres_entries_keeping(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
									 const HostSharedReader *reading, const PgRel *rel, const PgSlot *old,
									 const PgSlot *new_, const size_t *key_columns, size_t key_columns_len,
									 bool *has_result, DoltgresEntriesKeeping *result, PgPgError *error);
extern TreesStoreTree doltgres_entries_chunk_tree(const uint8 address[32]);
extern bool doltgres_entries_placed(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const PgRel *rel,
									AccessHeaptuplePlacing *placing, StdVec *result, PgPgError *error);
extern bool doltgres_entries_rows(const PgCatalogCaps *caps, const PgRel *relation, const PgSnapshot *snapshot,
								  const HostSnapshot *read, DoltgresEntriesBy by, StdVec *result, PgPgError *error);
extern TxnStatePersistence doltgres_entries_persistence(const PgRel *relation);
extern bool doltgres_entries_counted(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
									 const HostSharedReader *reading, const PgRel *rel, const PgRel *table, bool index,
									 PgPgError *error);
extern HostOwnedHandle doltgres_entries_handle(const PgScan *scan);

/* entries_tail.c's next_unit, root, blocks, and SPECULATED's clear, which this file's entries call. */
extern bool doltgres_entries_next_unit(const PgMayRaise *raise, HostIoIo *io, PgScan *scan, HostOwnedHandle owned,
									   DoltgresEntriesReading reading, bool *result, PgPgError *error);
extern bool doltgres_entries_root(const DgRoot *root, TreesStoreTree *result);
extern uint64 doltgres_entries_blocks(const PgRel *relation, const HostSharedHeld *held);
extern void doltgres_entries_speculated_clear(HostIoIo *io);
extern bool doltgres_entries_serial(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
									const HostSharedReader *reading, const PgScan *scan, DoltgresScanDirection way,
									DoltgresEntriesReading reading_kind, bool parallel, bool *has_result,
									DoltgresScanRow *result, PgPgError *error);
extern void doltgres_entries_found_running(const PgSnapshot *snapshot, const HostSnapshot *read,
										   const DoltgresScanRow *row);
extern bool doltgres_entries_fetched(const PgCatalogCaps *caps, const PgRel *relation, uint64 tuple_id,
									 const PgSnapshot *snapshot, bool *has_result, DoltgresScanRow *result,
									 PgPgError *error);
extern bool doltgres_entries_relocked(const PgCatalogCaps *caps, const PgRel *relation, const PgSnapshot *snapshot,
									  uint64 tuple_id, PgPgError *error);
extern void doltgres_entries_Repo_drop(void *repo);
extern void doltgres_entries_Row_drop(void *row);
extern PgPgError doltgres_entries_unsnapshotted(void);
extern bool doltgres_entries_folded_now(const PgCatalogCaps *caps, const PgRel *target, const PgRel *heap, bool *result,
										PgPgError *error);
#endif

#ifdef USE_INJECTION_POINTS
/* The injection points' tests' kinds of shared records, each its Shape and how the server keeps it. */
extern const HostSharedPoolOps DOLTGRES_ENTRIES_TESTED_POOL_OPS;
extern const HostSharedPoolOps DOLTGRES_ENTRIES_OTHER_POOL_OPS;
extern const HostSharedSingleOps DOLTGRES_ENTRIES_TABLE_SINGLE_OPS;
extern const HostSharedProcSlotsOps DOLTGRES_ENTRIES_WORDS_PROC_SLOTS_OPS;
extern const HostSharedProcSlotsOps DOLTGRES_ENTRIES_STAYS_PROC_SLOTS_OPS;
extern const HostSharedSingleOps DOLTGRES_ENTRIES_SPARE_SINGLE_OPS;
extern const HostSharedAgedOps DOLTGRES_ENTRIES_OUTER_AGED_OPS;
extern const HostSharedAgedOps DOLTGRES_ENTRIES_INNER_AGED_OPS;
#endif

#endif							/* DOLTGRES_DOLTGRES_ENTRIES_H */
