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
 * The server's storage across its repositories: what each checkpoint does
 * for every repository's packs at once, and the WAL it keeps.
 */
#ifndef DOLTGRES_STORE_LIB_H
#define DOLTGRES_STORE_LIB_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"

/*
 * STORE_KEPT_REDO_INDEX is KeptRedo's atomic: the redo point of the
 * checkpoint completed before the one being counted, from which the WAL is
 * kept while a repository's manifest generations may differ, or zero before
 * a run's first checkpoint.
 */
#define STORE_KEPT_REDO_INDEX 0

extern const HostSharedSingleOps STORE_STORE_STATE_SINGLE_OPS;

extern bool store_checkpoint(HostIoIo *io, HostLsn redo, HostError *error);
extern bool store_kept(HostIoIo *io, HostLsn checkpoint, HostError *error);
extern bool store_read_kept_wal(HostIoIo *io, HostLsn from, HostLsn to, HostError *error);
extern bool store_kept_segment(HostIoIo *io, uint64 segment, uint64 segment_size, uint64 *result,
							   HostError *error);

#ifdef DOLTGRES_TEST_STORE
extern void test_store_tests_a_checkpoint_evens_each_marked_manifest_and_stores_the_last_redo_point(void);
extern void test_store_tests_the_wal_is_kept_from_the_stored_redo_point_only_while_a_manifest_is_marked(void);
extern void test_store_tests_before_the_first_checkpoint_nothing_is_kept_past_what_postgres_keeps(void);
extern void test_store_tests_a_checkpoint_reads_the_wal_it_keeps_only_while_a_manifest_is_marked(void);
extern void test_store_tests_a_checkpoint_warns_of_a_manifest_it_cant_write_once_more_and_evens_the_others(void);
extern void test_store_tests_a_read_of_the_kept_wal_stands_where_the_servers_wal_does(void);
#endif

#endif							/* DOLTGRES_STORE_LIB_H */
