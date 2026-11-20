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
 * The C port's repo unit tests (the port text's section 13), repo's crate
 * built with DOLTGRES_TEST_REPO as cargo builds its unit tests, run in a
 * backend: one row per test, in the Rust's order, with its outcome.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres/repo/lib.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are repo's tests, in the order the Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"tests::a_checkpoints_runs_are_written_and_kept_by_the_next_save", test_repo_tests_a_checkpoints_runs_are_written_and_kept_by_the_next_save},
	{"tests::a_handle_reads_the_manifest_another_saved", test_repo_tests_a_handle_reads_the_manifest_another_saved},
	{"tests::a_manifest_reads_back_what_it_was_written_with",
	test_repo_tests_a_manifest_reads_back_what_it_was_written_with},
	{"tests::a_manifest_that_is_not_one_is_refused", test_repo_tests_a_manifest_that_is_not_one_is_refused},
	{"tests::a_manifest_that_is_not_whole_is_refused", test_repo_tests_a_manifest_that_is_not_whole_is_refused},
	{"tests::a_manifests_runs_read_back_and_unknown_fields_are_refused", test_repo_tests_a_manifests_runs_read_back_and_unknown_fields_are_refused},
	{"tests::a_refused_save_starts_over_with_its_own_runs_or_the_ones_it_reads_again", test_repo_tests_a_refused_save_starts_over_with_its_own_runs_or_the_ones_it_reads_again},
	{"tests::a_repository_keeps_its_manifest_and_chunks", test_repo_tests_a_repository_keeps_its_manifest_and_chunks},
	{"tests::a_save_cut_short_by_a_crash_at_any_step_leaves_one_whole_manifest", test_repo_tests_a_save_cut_short_by_a_crash_at_any_step_leaves_one_whole_manifest},
	{"tests::a_save_refused_by_another_starts_over_and_its_first_pack_is_named_by_no_manifest", test_repo_tests_a_save_refused_by_another_starts_over_and_its_first_pack_is_named_by_no_manifest},
	{"tests::a_saved_base_reads_back_with_its_chunks_and_a_storage_taken_out_is_gone", test_repo_tests_a_saved_base_reads_back_with_its_chunks_and_a_storage_taken_out_is_gone},
	{"tests::a_saves_fold_point_is_the_highest_written_and_its_redo_point_the_last_checkpoints", test_repo_tests_a_saves_fold_point_is_the_highest_written_and_its_redo_point_the_last_checkpoints},
	{"tests::a_saves_pairs_fold_into_the_identity_tree", test_repo_tests_a_saves_pairs_fold_into_the_identity_tree},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_repo);

/* doltgres_cport_test_repo runs repo's tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_repo(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}
