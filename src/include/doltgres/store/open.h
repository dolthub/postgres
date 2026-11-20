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
 * The startup process's open of every repository before the redo decision.
 */
#ifndef DOLTGRES_STORE_OPEN_H
#define DOLTGRES_STORE_OPEN_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"

extern bool store_open_startup_open(HostIoIo *io, HostLsn redo, HostLsn label, HostError *error);

#ifdef DOLTGRES_TEST_STORE
extern void test_store_open_tests_a_start_evens_each_repository_whose_manifests_differ(void);
extern void test_store_open_tests_a_start_warns_of_a_repository_it_cant_settle_and_opens_the_others(void);
extern void test_store_open_tests_a_start_keeps_both_generations_of_a_manifest_whose_newest_is_not_whole(void);
extern void test_store_open_tests_a_start_keeps_both_generations_of_a_manifest_whose_newest_root_cant_be_read(void);
extern void test_store_open_tests_a_start_reports_what_each_settle_changed_and_removes_a_dropped_repositorys_directories(void);
extern void test_store_open_tests_a_damaged_list_files_warning_words_its_lost_records_by_their_count(void);
extern void test_store_open_tests_a_start_keeps_both_generations_of_a_manifest_whose_newest_identity_root_cant_be_read(void);
#endif

#endif							/* DOLTGRES_STORE_OPEN_H */
