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
 * The functions core Postgres calls in the fork, one per call site that
 * src/doltgres/HOOKS.md lists; each is an entry thunk the binding generates
 * from src/doltgres/pg/entries.list (design item 17). And the variables of
 * the fork's settings, which the binding generates from
 * src/doltgres/pg/settings.list for the GUC table.
 */
#ifndef DOLTGRES_HOOKS_H
#define DOLTGRES_HOOKS_H

#include "access/genam.h"
#include "common/relpath.h"
#include "nodes/parsenodes.h"
#include "postmaster/bgworker.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "utils/guc.h"
#include "utils/pg_locale.h"
#include "utils/relcache.h"

/* TableAmRoutine is a table access method's routine, which access/tableam.h defines. */
struct TableAmRoutine;

/* doltgres_shared_preload is the fork's first entry in every process, inside the preload window. */
extern void doltgres_shared_preload(void);

/* The setting debug_fail_preload_allocation fails the preload entry's first allocation in the postmaster or a child. */
extern PGDLLIMPORT int doltgres_debug_fail_preload_allocation;
extern PGDLLIMPORT const struct config_enum_entry doltgres_debug_fail_preload_allocation_options[];

/*
 * DOLTGRES_LWLOCK_TRANCHES is the number of LWLock tranches the fork registers,
 * which lwlock.c adds to its own limit so extensions keep every one stock gives.
 */
#define DOLTGRES_LWLOCK_TRANCHES 5

/* doltgres_process_start registers the fork's exit callbacks, at the end of every process's BaseInit. */
extern void doltgres_process_start(void);

/*
 * DOLTGRES_EXIT_CALLBACKS is the number of before_shmem_exit callbacks the fork
 * registers, which ipc.c adds to its own limit so extensions keep every slot
 * stock gives.
 */
#define DOLTGRES_EXIT_CALLBACKS 2

/*
 * DoltgresReservedWorkers is the number of background worker slots, with
 * their processes, that the fork reserves for its own workers beside
 * max_worker_processes, so a user's setting leaves extensions and parallel
 * query every worker stock gives; doltgres_shared_preload sets it before
 * shared memory is sized, in every process.
 */
extern PGDLLIMPORT int DoltgresReservedWorkers;

/* The setting collector_first_packs is the number of packs a repository holds before its first full collection. */
extern PGDLLIMPORT int doltgres_collector_first_packs;

/* The setting collector_full_interval is the longest time in seconds between full collections of a repository. */
extern PGDLLIMPORT int doltgres_collector_full_interval;

/* The setting collector_io_limit is the kB a second a collection reads and copies at most while its packs are within their target, 0 for no limit. */
extern PGDLLIMPORT int doltgres_collector_io_limit;

/* The setting collector_min_free_disk is the MB free on the data directory's volume below which collections run whenever packs hold garbage, -1 for the smaller of 10 GB and a tenth of the volume. */
extern PGDLLIMPORT int doltgres_collector_min_free_disk;

/* The setting collector_naptime is the time in milliseconds the collector worker waits between its checks of which collections are due. */
extern PGDLLIMPORT int doltgres_collector_naptime;

/* The setting collector_target_percent is how far a repository's packs, or one pack, may pass their live bytes before a collection, or its copy, runs, as a percent of them. */
extern PGDLLIMPORT int doltgres_collector_target_percent;

/* The setting collector_young_packs is the number of packs written since a repository's last collection that make a young collection due. */
extern PGDLLIMPORT int doltgres_collector_young_packs;

/* The setting collector_young_size is the kB of packs written since a repository's last collection that make a young collection due. */
extern PGDLLIMPORT int doltgres_collector_young_size;

/* The setting delta_memory is the size in kB of the shared memory area for recent changes. */
extern PGDLLIMPORT int doltgres_delta_memory;

/* The setting fold_threshold is the kB of a storage's unfolded changes at which it is folded, -1 for a sixteenth of delta_memory. */
extern PGDLLIMPORT int doltgres_fold_threshold;

/* The setting fold_timeout is the age in seconds of a storage's unfolded changes at which it is folded. */
extern PGDLLIMPORT int doltgres_fold_timeout;

/* The setting fold_workers is the number of fold worker processes. */
extern PGDLLIMPORT int doltgres_fold_workers;

/* The setting node_cache_size is the size in kB of the shared cache of tree nodes. */
extern PGDLLIMPORT int doltgres_node_cache_size;

/*
 * doltgres_libc_locale builds a libc-named locale from the ICU constructor
 * and gives it in *result, for every pair of names but those of the C family.
 */
extern bool doltgres_libc_locale(Oid collid, MemoryContext context, pg_locale_t *result);

/*
 * doltgres_libc_new_locale builds the locale of a new collation CREATE
 * COLLATION checks, once Postgres' C library has made its names' locale, and
 * keeps that locale in it.
 */
extern bool doltgres_libc_new_locale(Oid collid, MemoryContext context, locale_t loc, pg_locale_t *result);

/*
 * doltgres_new_collation loads a new collation's locale for CREATE
 * COLLATION's check, its libc names built by the C library first, unless
 * the collation copies another's: from is its FROM clause, null when it has
 * none.
 */
extern bool doltgres_new_collation(Oid collid, const DefElem *from);

/*
 * doltgres_icu_locale gives the ICU constructor a libc locale's language tag
 * in *iculocstr, in place of the catalog's ICU locale name.
 */
extern bool doltgres_icu_locale(Oid collid, const char **iculocstr);

/*
 * doltgres_builtin_locale gives the builtin provider's constructor the name of
 * the builtin locale a libc locale of the C family, but C and POSIX alone,
 * stands for, in place of the catalog's locale name, which a libc row leaves
 * null; it answers false for a locale of the builtin provider.
 */
extern bool doltgres_builtin_locale(Oid collid, const char **locstr);

/*
 * doltgres_libc_collversion gives a libc collation's version in *collversion
 * as the vendored collator's, for every name but those of the C family.
 */
extern bool doltgres_libc_collversion(const char *collcollate, char **collversion);

/* doltgres_checkpoint is the fork's work at each checkpoint and restartpoint, at the end of CheckPointGuts. */
extern void doltgres_checkpoint(uint64 redo);

/* doltgres_restartpoint_held says whether a restartpoint at this redo point would pass a reached fold point. */
extern bool doltgres_restartpoint_held(uint64 redo);

/* doltgres_kept_segment returns the oldest WAL segment KeepLogSeg keeps, lowered for the fork's manifests. */
extern int64 doltgres_kept_segment(int64 segment, int segment_size);

/* doltgres_startup_open opens every repository before the redo decision, in StartupXLOG. */
extern void doltgres_startup_open(uint64 redo);

/* doltgres_commit_registered stores a commit's end in its process's slot, in RecordTransactionCommit's critical section. */
extern void doltgres_commit_registered(void);

/* doltgres_redo_committed stores a replayed commit's end in its transaction's entry, in xact_redo_commit. */
extern void doltgres_redo_committed(TransactionId xid, uint64 end);

/*
 * doltgres_redo_done drops from the working states' deltas every built node
 * whose base record never came, in StartupXLOG once redo has ended.
 */
extern void doltgres_redo_done(void);

/* XLogReaderState is xlogreader.h's record reader, which the replay entries below take. */
struct XLogReaderState;

/*
 * doltgres_redo_aborted takes out of the working states' deltas the built
 * nodes of every storage a replayed abort drops, at the start of xact_redo.
 */
extern void doltgres_redo_aborted(struct XLogReaderState *record);

/*
 * doltgres_running_replayed drops from the working states' deltas the built
 * nodes of every writer a replayed running transactions record shows ended
 * uncommitted, at the start of standby_redo.
 */
extern void doltgres_running_replayed(struct XLogReaderState *record);

/* doltgres_prepared_committing points a prepared transaction's entry at this process, ahead of its commit. */
extern void doltgres_prepared_committing(TransactionId xid);

/* doltgres_prepared_registered stores a prepared transaction's commit in this process's slot, in its critical section. */
extern void doltgres_prepared_registered(void);

/* doltgres_createdb makes a new database's repository in its default tablespace, in createdb. */
extern void doltgres_createdb(Oid template_id, Oid database_id, Oid tablespace_id);

/* doltgres_dropdb drops a database's repository and checkpoints after its removal, in dropdb. */
extern void doltgres_dropdb(Oid database_id);

/* doltgres_index_routine gives an index the fork stores the fork's copy of its routine, in RelationInitIndexAccessInfo. */
extern void doltgres_index_routine(Relation relation);

/*
 * doltgres_index_copy_data moves the storage of an index the fork stores into its new relfilenode, in
 * index_copy_data.
 */
extern void doltgres_index_copy_data(Relation rel, const RelFileLocator *newrlocator);

/* doltgres_table_am_routine gives a relation the fork stores the fork's table routine, in InitTableAmRoutine. */
extern bool doltgres_table_am_routine(Relation relation, const struct TableAmRoutine **routine);

/* doltgres_relation_creating records a relation's OID and what it rewrites, in heap_create_with_catalog. */
extern void doltgres_relation_creating(Oid relid, Oid relrewrite);

/*
 * doltgres_key_named names a table whose primary key the statement creates, marks, or drops, in index_create,
 * index_constraint_create, and index_drop, given whether the index is the table's primary key.
 */
extern void doltgres_key_named(Relation table, bool primary);

/* doltgres_key_level_opened opens a statement's level of the named tables, in ProcessUtility before its dispatch. */
extern void doltgres_key_level_opened(void);

/*
 * doltgres_key_level_settled settles the tables the statement's level named and closes it, in ProcessUtility after
 * its dispatch.
 */
extern void doltgres_key_level_settled(void);

/* doltgres_key_settle settles an altered table that ALTER TABLE doesn't rewrite, in ATRewriteTables. */
extern void doltgres_key_settle(Oid table);

/* doltgres_new_heap gives a transient table's storage its old table's key, in make_new_heap. */
extern void doltgres_new_heap(Oid new_table, Relation old_table);

/*
 * doltgres_storage_unlinked frees the fork's storage of a relfilenode Postgres
 * unlinks, in smgrdounlinkall at commit, at abort, and in redo, reporting a
 * failure at WARNING as smgr_unlink does.
 */
extern void doltgres_storage_unlinked(const RelFileLocator *locator);

/* doltgres_db_file_conflict reports whether a directory of a repository of a database's id stands. */
extern bool doltgres_db_file_conflict(Oid database_id);

/* doltgres_movedb_copy copies a moving database's repository into its new tablespace, in movedb. */
extern void doltgres_movedb_copy(Oid database_id, Oid from_tablespace, Oid to_tablespace);

/* doltgres_movedb_finish makes the copy the repository and removes the old one, in movedb after its commit. */
extern void doltgres_movedb_finish(Oid database_id, Oid from_tablespace, Oid to_tablespace);

/*
 * doltgres_unlogged_cleanup empties every unlogged storage, in StartupXLOG beside
 * ResetUnloggedRelations(UNLOGGED_RELATION_CLEANUP).
 */
extern void doltgres_unlogged_cleanup(void);

/*
 * doltgres_backup_started counts a base backup in and waits out a running collection, in do_pg_backup_start after
 * Postgres counts it.
 */
extern void doltgres_backup_started(void);

/*
 * doltgres_backup_ended counts a base backup out, in do_pg_backup_stop and do_pg_abort_backup after Postgres does.
 */
extern void doltgres_backup_ended(void);

/*
 * doltgres_collect is the collector's background worker: it collects each repository whose collection is due, and
 * removes the files whose readers are gone.
 */
extern void doltgres_collect(Datum main_arg);

/*
 * doltgres_collector_released gives back the session lock the collector worker
 * holds on a database it checks, and cancels a condition variable's sleep it
 * was in, as the worker exits, since no transaction's abort does either for a
 * process outside one.
 */
extern void doltgres_collector_released(int code, Datum arg);

/* doltgres_index_blocks gives a fork index's derived block count, in RelationGetNumberOfBlocksInFork. */
extern bool doltgres_index_blocks(Relation relation, ForkNumber forkNum, BlockNumber *blocks);

/*
 * doltgres_relation_size_added adds a fork relation's size to what its files
 * hold, in pg_relation_size for the fork it names and in dbsize.c's sums over a
 * table's relations for every fork, named by NULL.
 */
extern int64 doltgres_relation_size_added(Relation rel, text *forkName, int64 size);

/* doltgres_database_size adds a database's repository's files, in calculate_database_size. */
extern int64 doltgres_database_size(Oid dbOid, int64 size);

/* doltgres_tablespace_size adds the files of a tablespace's repositories, in calculate_tablespace_size. */
extern int64 doltgres_tablespace_size(Oid tblspcOid, int64 size);

/*
 * doltgres_fold_launch is the fold launcher's background worker: it starts a
 * fold worker for each database with a storage due a fold.
 */
extern void doltgres_fold_launch(Datum main_arg);

/*
 * doltgres_fold_work is a fold worker: it folds each due storage of the
 * database its argument names.
 */
extern void doltgres_fold_work(Datum main_arg);

/*
 * doltgres_fold_relation folds a relation's storage now, inside a fold
 * worker's transaction as the relation's owner, and says whether it installed
 * a base; heap is the relation itself, or an index's table, whose horizon
 * bounds the fold.
 */
extern bool doltgres_fold_relation(Relation rel, Relation heap);

/*
 * doltgres_fork_worker says whether a background worker is one of the fork's
 * own, whose entry is one of bgworker.c's internal doltgres_ entries.
 */
extern bool doltgres_fork_worker(const BackgroundWorker *worker);

/*
 * doltgres_worker_refused says whether RegisterDynamicBackgroundWorker's slot
 * search refuses a dynamic background worker, with BackgroundWorkerLock held:
 * never one of the fork's own, and any other once max_worker_processes slots
 * hold workers that aren't the fork's, so the slots DoltgresReservedWorkers
 * adds stay the fork's.
 */
extern bool doltgres_worker_refused(const BackgroundWorker *worker);

/*
 * doltgres_worker_slots_in_use counts the in-use background worker slots held
 * by workers that aren't the fork's own, with BackgroundWorkerLock held; it is
 * bgworker.c's, which keeps the slots private.
 */
extern int	doltgres_worker_slots_in_use(void);

/*
 * doltgres_recovery_refused ends the startup process with a FATAL once a
 * standby's fold has found a root other than its primary's.
 */
extern void doltgres_recovery_refused(void);

/*
 * doltgres_relation_dropped removes a dropped relation's identity pairs in the
 * dropping transaction, in heap_drop_with_catalog and index_drop.
 */
extern void doltgres_relation_dropped(Relation relation);

/*
 * doltgres_altering keeps the row layouts of a relation the fork stores and of
 * its inheritors before an ALTER TABLE subcommand runs, at the start of
 * ATExecCmd, with the table's rewrite flags so far, which say whether Postgres
 * rewrites it in phase 3.
 */
extern void doltgres_altering(Relation relation, AlterTableCmd *cmd, int rewrite);

/*
 * doltgres_altered records what an executed ALTER TABLE subcommand changes of
 * a relation the fork stores, at the end of ATExecCmd, the command none when
 * the subcommand did nothing.
 */
extern void doltgres_altered(Relation relation, AlterTableCmd *cmd);

/*
 * doltgres_object_dropped removes a dropped schema's identity pair in the
 * dropping transaction, in doDeletion, for an object of any class.
 */
extern void doltgres_object_dropped(Oid class_id, Oid object_id);

/*
 * doltgres_column_dropped chains the transform of a column's drop from a
 * table the fork stores, in doDeletion right after RemoveAttributeById, and
 * does nothing for a whole relation's drop, attribute number 0.
 */
extern void doltgres_column_dropped(Oid relid, int attnum);

/*
 * doltgres_toast_created records a user table's new TOAST relation's identity
 * pair in the transaction that made it, at the end of create_toast_table.
 */
extern void doltgres_toast_created(Relation relation, Oid toast);

/*
 * doltgres_namespace_created records a new schema's identity pair in its
 * creating transaction, in NamespaceCreate after its row is inserted, and
 * none for a temporary schema.
 */
extern void doltgres_namespace_created(Oid nspoid, const char *nspname, bool istemp);

/*
 * doltgres_index_only_tid gives WHERE CURRENT OF the tid of an index-only
 * scan's current row for an index the fork stores, leaving the tid the scan's
 * entry named for any other, in execCurrent.
 */
extern void doltgres_index_only_tid(IndexScanDesc scan, ItemPointer tid);

#endif							/* DOLTGRES_HOOKS_H */
