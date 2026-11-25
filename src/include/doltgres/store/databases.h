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
 * A database's repository made, dropped, and moved, with the fork's records
 * of each and their redo, and the first repository the start after
 * bootstrap makes.
 */
#ifndef DOLTGRES_STORE_DATABASES_H
#define DOLTGRES_STORE_DATABASES_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/str.h"
#include "doltgres/txn/insert.h"

extern HostError store_databases_failed(const uint8 *code, const char *said, uint32 database);
extern bool store_databases_warned(HostIoIo *io, HostError *failure, const char *context, uint8 *code,
								   HostError *error);
extern bool store_databases_created(HostIoIo *io, uint32 template, uint32 database, uint32 tablespace,
									HostError *error);
extern bool store_databases_settled(HostIoIo *io, uint32 database, uint32 tablespace, bool committed,
									HostError *error);
extern bool store_databases_creation_ended(HostIoIo *io, bool committed, bool *settled, HostError *error);
extern bool store_databases_started(HostIoIo *io, const TxnInsertPage *page, bool *loaded, HostError *error);
extern bool store_databases_checkpointed(HostIoIo *io, HostError *error);
extern bool store_databases_redo_settled(HostIoIo *io, const TxnInsertPage *page, uint64 xid, bool committed,
										 bool *loaded, HostError *error);
extern bool store_databases_redo_ended(HostIoIo *io, HostError *error);
extern bool store_databases_dropped(HostIoIo *io, uint32 database, HostError *error);
extern bool store_databases_conflicts(HostIoReadDoors io, uint32 database, bool *result, HostError *error);
extern bool store_databases_redo_created(HostIoIo *io, const uint8 *body, size_t body_len, uint64 xid, HostLsn end,
										 HostError *error);
extern bool store_databases_redo_dropped(HostIoIo *io, const uint8 *body, size_t body_len, HostError *error);
extern bool store_databases_moved_copy(HostIoIo *io, uint32 database, uint32 from, uint32 to, HostError *error);
extern bool store_databases_marked_moves(HostIoIo *io, HostError *error);
extern bool store_databases_moved_finish(HostIoIo *io, uint32 database, uint32 from, uint32 to, HostError *error);
extern bool store_databases_committed(HostIoIo *io, uint32 repository, uint32 tablespace, uint32 *result,
									  HostError *error);
extern bool store_databases_unfinished(HostIoReadDoors io, uint32 repository, uint32 tablespace, bool *result,
									   HostError *error);
extern bool store_databases_redo_moved(HostIoIo *io, const uint8 *body, size_t body_len, HostLsn end,
									   HostError *error);
extern bool store_databases_moves_load(HostIoReadDoors io, HostError *error);
extern void store_databases_moves_freed(void);
extern void store_databases_replayed_storage(HostStorage *storage, HostLsn replayed);
extern bool store_databases_describe_moved(const uint8 *body, size_t body_len, StdString *result, HostError *error);
extern bool store_databases_describe_created(const uint8 *body, size_t body_len, StdString *result,
											 HostError *error);
extern bool store_databases_describe_dropped(const uint8 *body, size_t body_len, StdString *result,
											 HostError *error);
extern bool store_databases_bootstrapped(HostIoIo *io, HostError *error);
extern bool store_databases_first(HostIoIo *io, HostError *error);

#ifdef DOLTGRES_TEST_STORE
extern void test_store_databases_tests_a_creation_makes_an_empty_repository_and_leaves_its_template(void);
extern void test_store_databases_tests_a_failed_creation_removes_what_it_made(void);
extern void test_store_databases_tests_a_drop_removes_the_repository_and_checkpoints_after_it(void);
extern void test_store_databases_tests_a_creation_and_a_drop_replay_twice_as_once(void);
extern void test_store_databases_tests_a_creation_replays_over_a_marked_leftover_and_not_over_its_moved_repository(void);
extern void test_store_databases_tests_a_creation_replays_into_a_dropped_tablespace_as_nothing(void);
extern void test_store_databases_tests_a_record_that_doesnt_read_is_corrupt(void);
extern void test_store_databases_tests_the_first_repository_is_made_once_by_bootstraps_marker(void);
extern void test_store_databases_tests_a_move_puts_the_repository_in_its_new_tablespace(void);
extern void test_store_databases_tests_a_moves_records_replay_twice_as_once_and_after_it_finished(void);
extern void test_store_databases_tests_a_start_finishes_a_marked_move_and_leaves_an_unmarked_copy(void);
extern void test_store_databases_tests_an_open_finishes_a_committed_move(void);
extern void test_store_databases_tests_a_drop_and_a_move_remove_a_stale_copy(void);
extern void test_store_databases_tests_two_finishes_of_one_move_leave_one_pack_record(void);
extern void test_store_databases_tests_a_finish_after_another_removed_the_old_directory_ends_without_error(void);
extern void test_store_databases_tests_the_start_after_bootstrap_is_silent(void);
extern void test_store_databases_tests_a_record_made_before_a_moves_finish_is_no_record_of_the_moved_repository(void);
#endif

#endif							/* DOLTGRES_STORE_DATABASES_H */
