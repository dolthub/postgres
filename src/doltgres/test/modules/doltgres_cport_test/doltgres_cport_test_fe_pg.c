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
 * pg's frontend test binary (the port text's section 7): the frontend
 * build's binding's tests, pg's fe_tests, and the frontend test-only
 * std::fs's own case, run outside a backend, one row each printed.
 */
#include "postgres_fe.h"

#include "common/logging.h"
#include "doltgres/pg/frontend.h"
#include "doltgres/std/test_fe.h"
#include "doltgres_cport_test_fe.h"

/* CPORT_TESTS are the binary's tests, by their Rust paths in order. */
static const CportTest CPORT_TESTS[] = {
	{"cport::std::fe_fs::tests::a_directory_is_made_written_in_and_removed",
	test_std_fe_fs_tests_a_directory_is_made_written_in_and_removed},
	{"tests::a_directory_lists_its_names_and_asks_for_room",
	test_pg_tests_a_directory_lists_its_names_and_asks_for_room},
	{"tests::a_failure_is_written_cut_to_the_buffer_and_terminated",
	test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated},
	{"tests::a_file_opens_reads_and_closes",
	test_pg_tests_a_file_opens_reads_and_closes},
	{"tests::a_missing_file_fails_with_its_errno_and_text",
	test_pg_tests_a_missing_file_fails_with_its_errno_and_text},
	{"tests::the_version_directory_is_the_major_versions",
	test_pg_tests_the_version_directory_is_the_major_versions},
};

/* main runs the binary's tests and prints one row of each test's name and outcome. */
int
main(int argc, char **argv)
{
	pg_logging_init(argv[0]);
	(void) argc;
	return doltgres_cport_test_fe_run_all(CPORT_TESTS, lengthof(CPORT_TESTS));
}
