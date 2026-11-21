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
 * The bodies of the fork's test entries (doltgres' entries/testing.rs), each
 * the body of the entry of its name in pg's test-entries.list, which a build
 * with injection points runs.
 */
#ifndef DOLTGRES_DOLTGRES_ENTRIES_TESTING_H
#define DOLTGRES_DOLTGRES_ENTRIES_TESTING_H

#include "doltgres/host/owned.h"
#include "doltgres/pg/lib.h"

/* The test records' replay and description, which the resource manager's entries call. */
extern bool doltgres_entries_testing_redo_test(const PgCaps *caps, const uint8 *body, size_t body_len, PgPgError *error);
extern void doltgres_entries_testing_describe_test(const uint8 *body, size_t body_len, StdString *result);

/* The backend's test records, an owned kind whose records the resource owners' release ends. */
extern HostOwnedTable DOLTGRES_ENTRIES_TESTING_TEST_RECORDS;

extern bool doltgres_entries_testing_doltgres_test_shmem_store(const PgCaps *caps, int64 value, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_shmem_load(const PgCaps *caps, int64 *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_lwlock_pairs(const PgCaps *caps, int64 pairs, int32 way,
																PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_escalate(const PgCaps *caps, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_host(const PgCaps *caps, int32 which, int64 *result,
														PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_file(const PgCaps *caps, int32 step, PgDatum *result,
														PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_wal(const PgCaps *caps, int64 value, int64 *result,
													   PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_wal_unknown(const PgCaps *caps, int32 len, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_delay(const PgCaps *caps, int32 step, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_check(const PgCaps *caps, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_lock(const PgCaps *caps, int32 step, int64 *result,
														PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_shared(const PgCaps *caps, int32 step, int64 location, int64 value,
														  int64 *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_zstd(const PgCaps *caps, int32 input, int32 dictionary,
														PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_cache(const PgCaps *caps, int32 step, int64 key, int64 value,
														 int64 *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_exit_callback(const PgCaps *caps, int32 code, PgDatum arg,
																 PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_delta(const PgCaps *caps, int32 step, int64 a, int64 b,
														 int64 *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_status(const PgCaps *caps, int64 xid, int64 *result,
														  PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_in_progress(const PgCaps *caps, int64 xid, int64 *result,
															   PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_writer(const PgCaps *caps, int32 which, int64 *result,
														  PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_command(const PgCaps *caps, int32 used, int64 *result,
														   PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_is_current(const PgCaps *caps, int64 xid, int64 *result,
															  PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_status_table(const PgCaps *caps, int32 step, int64 xid, int64 value,
																int64 *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_kept_wal(const PgCaps *caps, int64 from, int64 to, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_repository(const PgCaps *caps, int32 id, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_registered(const PgCaps *caps, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_session(const PgCaps *caps, bool save, int64 *result,
														   PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_key(const PgCatalogCaps *caps, const PgCall *fcinfo, PgDatum *result,
													   PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_words(const PgCaps *caps, int64 *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_slot(const PgCatalogCaps *caps, PgSlot *slot, PgSlot *source,
														int32 keys, bool descending, const char *version, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_att(const PgCaps *caps, PgSlot *slot, int32 att, int32 field,
													   int64 *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_owned(const PgCatalogCaps *caps, int32 step, int64 value,
														 int64 *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_states(const PgCaps *caps, int64 repository, int64 count,
														  int64 *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_relation_locks(const PgCaps *caps, int64 *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_base_row(const PgCatalogCaps *caps, const PgRel *target,
															PgSlot *source, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_base_save(const PgCatalogCaps *caps, const PgRel *target,
															 int32 extra, int64 *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_index_fetch_key(const PgCatalogCaps *caps, PgIndexFetch *fetch,
																   const PgRel *target, PgSlot *source, uint64 copy, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_fold(const PgCatalogCaps *caps, const PgRel *target, bool *result,
														PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_free_space(const PgCatalogCaps *caps, const PgRel *target,
															  int32 block, PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_base_points(const PgCatalogCaps *caps, const PgRel *target,
															   PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_base_sizes(const PgCatalogCaps *caps, const PgRel *target,
															  PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_index_entries(const PgCatalogCaps *caps, const PgRel *target,
																 PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_held(const PgCatalogCaps *caps, const PgRel *target, uint64 tid,
														bool *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_index_base(const PgCatalogCaps *caps, const PgRel *target,
															  PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_index_fold(const PgCatalogCaps *caps, const PgRel *heap,
															  const PgRel *index, PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_collect(const PgCatalogCaps *caps, PgDatum *result,
														   PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_disk_space(const PgCaps *caps, PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_database_lock(const PgCaps *caps, uint32 database, bool hold,
																 bool *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_changes(const PgCatalogCaps *caps, const PgRel *target,
														   PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_built_nodes(const PgCaps *caps, PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_mapped(const PgCatalogCaps *caps, const PgRel *target, bool *result,
														  PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_identity(const PgCatalogCaps *caps, int32 kind, uint32 number,
															int32 attnum, PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_epochs(const PgCaps *caps, PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_base_levels(const PgCatalogCaps *caps, const PgRel *target,
															   int32 *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_totals(const PgCatalogCaps *caps, const PgRel *target,
														  PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_scan_shape(const PgCatalogCaps *caps, const PgRel *target,
															  PgDatum *result, PgPgError *error);
extern bool doltgres_entries_testing_doltgres_test_count(const PgCatalogCaps *caps, const PgRel *target,
														 PgDatum *result, PgPgError *error);

#endif							/* DOLTGRES_DOLTGRES_ENTRIES_TESTING_H */
