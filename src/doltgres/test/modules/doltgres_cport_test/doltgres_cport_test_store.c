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
 * The C port's store unit tests (the port text's section 13), store's crate
 * built with DOLTGRES_TEST_STORE as cargo builds its unit tests, run in a
 * backend: one row per test, in the Rust's order, with its outcome.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres/store/collect.h"
#include "doltgres/store/databases.h"
#include "doltgres/store/fsck.h"
#include "doltgres/store/lib.h"
#include "doltgres/store/open.h"
#include "doltgres/store/session.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are store's tests, as its Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"collect::tests::a_cancel_at_any_check_of_a_full_collection_leaves_its_packs_whole",
	test_store_collect_tests_a_cancel_at_any_check_of_a_full_collection_leaves_its_packs_whole},
	{"collect::tests::a_chunk_named_by_address_alone_marks_every_record_of_it",
	test_store_collect_tests_a_chunk_named_by_address_alone_marks_every_record_of_it},
	{"collect::tests::a_collection_retires_a_pack_nothing_names_and_keeps_a_named_one",
	test_store_collect_tests_a_collection_retires_a_pack_nothing_names_and_keeps_a_named_one},
	{"collect::tests::a_collection_runs_nothing_while_a_base_backup_is_counted_in",
	test_store_collect_tests_a_collection_runs_nothing_while_a_base_backup_is_counted_in},
	{"collect::tests::a_paced_collection_naps_for_the_bytes_it_reads",
	test_store_collect_tests_a_paced_collection_naps_for_the_bytes_it_reads},
	{"collect::tests::a_second_collection_with_no_pack_written_since_runs",
	test_store_collect_tests_a_second_collection_with_no_pack_written_since_runs},
	{"collect::tests::a_young_collection_retires_a_new_dead_pack_and_leaves_the_old_ones",
	test_store_collect_tests_a_young_collection_retires_a_new_dead_pack_and_leaves_the_old_ones},
	{"collect::tests::the_check_collects_on_a_low_disk_and_reports_a_directory_no_database_names",
	test_store_collect_tests_the_check_collects_on_a_low_disk_and_reports_a_directory_no_database_names},
	{"collect::tests::the_verify_lists_again_a_retired_pack_a_missed_root_reaches",
	test_store_collect_tests_the_verify_lists_again_a_retired_pack_a_missed_root_reaches},
	{"databases::tests::a_creation_and_a_drop_replay_twice_as_once",
	test_store_databases_tests_a_creation_and_a_drop_replay_twice_as_once},
	{"databases::tests::a_creation_makes_an_empty_repository_and_leaves_its_template",
	test_store_databases_tests_a_creation_makes_an_empty_repository_and_leaves_its_template},
	{"databases::tests::a_creation_replays_into_a_dropped_tablespace_as_nothing",
	test_store_databases_tests_a_creation_replays_into_a_dropped_tablespace_as_nothing},
	{"databases::tests::a_creation_replays_over_a_marked_leftover_and_not_over_its_moved_repository",
	test_store_databases_tests_a_creation_replays_over_a_marked_leftover_and_not_over_its_moved_repository},
	{"databases::tests::a_drop_and_a_move_remove_a_stale_copy",
	test_store_databases_tests_a_drop_and_a_move_remove_a_stale_copy},
	{"databases::tests::a_drop_removes_the_repository_and_checkpoints_after_it",
	test_store_databases_tests_a_drop_removes_the_repository_and_checkpoints_after_it},
	{"databases::tests::a_failed_creation_removes_what_it_made",
	test_store_databases_tests_a_failed_creation_removes_what_it_made},
	{"databases::tests::a_finish_after_another_removed_the_old_directory_ends_without_error",
	test_store_databases_tests_a_finish_after_another_removed_the_old_directory_ends_without_error},
	{"databases::tests::a_move_puts_the_repository_in_its_new_tablespace",
	test_store_databases_tests_a_move_puts_the_repository_in_its_new_tablespace},
	{"databases::tests::a_moves_records_replay_twice_as_once_and_after_it_finished",
	test_store_databases_tests_a_moves_records_replay_twice_as_once_and_after_it_finished},
	{"databases::tests::a_record_made_before_a_moves_finish_is_no_record_of_the_moved_repository",
	test_store_databases_tests_a_record_made_before_a_moves_finish_is_no_record_of_the_moved_repository},
	{"databases::tests::a_record_that_doesnt_read_is_corrupt",
	test_store_databases_tests_a_record_that_doesnt_read_is_corrupt},
	{"databases::tests::a_start_finishes_a_marked_move_and_leaves_an_unmarked_copy",
	test_store_databases_tests_a_start_finishes_a_marked_move_and_leaves_an_unmarked_copy},
	{"databases::tests::an_open_finishes_a_committed_move",
	test_store_databases_tests_an_open_finishes_a_committed_move},
	{"databases::tests::the_first_repository_is_made_once_by_bootstraps_marker",
	test_store_databases_tests_the_first_repository_is_made_once_by_bootstraps_marker},
	{"databases::tests::the_start_after_bootstrap_is_silent",
	test_store_databases_tests_the_start_after_bootstrap_is_silent},
	{"databases::tests::two_finishes_of_one_move_leave_one_pack_record",
	test_store_databases_tests_two_finishes_of_one_move_leave_one_pack_record},
	{"fsck::tests::a_chunk_a_base_record_names_that_no_pack_holds_is_found",
	test_store_fsck_tests_a_chunk_a_base_record_names_that_no_pack_holds_is_found},
	{"fsck::tests::a_dropped_repository_is_settled",
	test_store_fsck_tests_a_dropped_repository_is_settled},
	{"fsck::tests::a_newest_identity_root_that_cant_be_read_is_found_with_its_range",
	test_store_fsck_tests_a_newest_identity_root_that_cant_be_read_is_found_with_its_range},
	{"fsck::tests::a_newest_manifest_not_whole_is_found_with_the_previouss_range",
	test_store_fsck_tests_a_newest_manifest_not_whole_is_found_with_the_previouss_range},
	{"fsck::tests::a_newest_root_that_cant_be_read_is_found_with_its_range",
	test_store_fsck_tests_a_newest_root_that_cant_be_read_is_found_with_its_range},
	{"fsck::tests::a_newest_whose_root_reads_settles_its_ranges_gap_though_a_deeper_chunk_is_missing",
	test_store_fsck_tests_a_newest_whose_root_reads_settles_its_ranges_gap_though_a_deeper_chunk_is_missing},
	{"fsck::tests::a_node_whose_parent_says_another_count_is_found_naming_the_parent",
	test_store_fsck_tests_a_node_whose_parent_says_another_count_is_found_naming_the_parent},
	{"fsck::tests::a_tablespace_whose_location_is_missing_is_noted",
	test_store_fsck_tests_a_tablespace_whose_location_is_missing_is_noted},
	{"fsck::tests::an_evened_repository_finds_nothing",
	test_store_fsck_tests_an_evened_repository_finds_nothing},
	{"fsck::tests::differing_generations_are_settled_with_the_newests_range",
	test_store_fsck_tests_differing_generations_are_settled_with_the_newests_range},
	{"fsck::tests::no_repository_finds_nothing",
	test_store_fsck_tests_no_repository_finds_nothing},
	{"open::tests::a_damaged_list_files_warning_words_its_lost_records_by_their_count",
	test_store_open_tests_a_damaged_list_files_warning_words_its_lost_records_by_their_count},
	{"open::tests::a_start_evens_each_repository_whose_manifests_differ",
	test_store_open_tests_a_start_evens_each_repository_whose_manifests_differ},
	{"open::tests::a_start_keeps_both_generations_of_a_manifest_whose_newest_identity_root_cant_be_read",
	test_store_open_tests_a_start_keeps_both_generations_of_a_manifest_whose_newest_identity_root_cant_be_read},
	{"open::tests::a_start_keeps_both_generations_of_a_manifest_whose_newest_is_not_whole",
	test_store_open_tests_a_start_keeps_both_generations_of_a_manifest_whose_newest_is_not_whole},
	{"open::tests::a_start_keeps_both_generations_of_a_manifest_whose_newest_root_cant_be_read",
	test_store_open_tests_a_start_keeps_both_generations_of_a_manifest_whose_newest_root_cant_be_read},
	{"open::tests::a_start_reports_what_each_settle_changed_and_removes_a_dropped_repositorys_directories",
	test_store_open_tests_a_start_reports_what_each_settle_changed_and_removes_a_dropped_repositorys_directories},
	{"open::tests::a_start_warns_of_a_repository_it_cant_settle_and_opens_the_others",
	test_store_open_tests_a_start_warns_of_a_repository_it_cant_settle_and_opens_the_others},
	{"session::tests::a_backends_exit_appends_its_session_repositorys_deferred_adds",
	test_store_session_tests_a_backends_exit_appends_its_session_repositorys_deferred_adds},
	{"session::tests::a_failed_open_stores_nothing",
	test_store_session_tests_a_failed_open_stores_nothing},
	{"session::tests::a_process_with_no_database_has_no_session_repository",
	test_store_session_tests_a_process_with_no_database_has_no_session_repository},
	{"session::tests::a_refused_repository_raises_its_refusal",
	test_store_session_tests_a_refused_repository_raises_its_refusal},
	{"session::tests::calls_in_one_backend_share_its_repository",
	test_store_session_tests_calls_in_one_backend_share_its_repository},
	{"tests::a_checkpoint_evens_each_marked_manifest_and_stores_the_last_redo_point",
	test_store_tests_a_checkpoint_evens_each_marked_manifest_and_stores_the_last_redo_point},
	{"tests::a_checkpoint_reads_the_wal_it_keeps_only_while_a_manifest_is_marked",
	test_store_tests_a_checkpoint_reads_the_wal_it_keeps_only_while_a_manifest_is_marked},
	{"tests::a_checkpoint_warns_of_a_manifest_it_cant_write_once_more_and_evens_the_others",
	test_store_tests_a_checkpoint_warns_of_a_manifest_it_cant_write_once_more_and_evens_the_others},
	{"tests::a_read_of_the_kept_wal_stands_where_the_servers_wal_does",
	test_store_tests_a_read_of_the_kept_wal_stands_where_the_servers_wal_does},
	{"tests::before_the_first_checkpoint_nothing_is_kept_past_what_postgres_keeps",
	test_store_tests_before_the_first_checkpoint_nothing_is_kept_past_what_postgres_keeps},
	{"tests::the_wal_is_kept_from_the_stored_redo_point_only_while_a_manifest_is_marked",
	test_store_tests_the_wal_is_kept_from_the_stored_redo_point_only_while_a_manifest_is_marked},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_store);

/* doltgres_cport_test_store runs store's tests, one row of each test's name and outcome. */
Datum
doltgres_cport_test_store(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}
