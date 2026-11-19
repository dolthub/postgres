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
 * The C port's pg unit tests (the port text's section 13), pg's crate built
 * with DOLTGRES_TEST_PG as cargo builds its unit tests, run in a backend: one
 * row per test, in the Rust's order, with its outcome.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres/pg/lib.h"
#include "doltgres/pg/word.h"
#include "doltgres/pg/zstd.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are pg's tests, in the order the Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"tests::a_failed_allocation_frees_the_reserve", test_pg_tests_a_failed_allocation_frees_the_reserve},
	{"tests::a_multi_row_inserts_slots_are_one_slice_of_their_count", test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count},
	{"tests::a_toast_slices_bytes_are_past_its_header", test_pg_tests_a_toast_slices_bytes_are_past_its_header},
	{"tests::borrowed_bytes_are_the_callers", test_pg_tests_borrowed_bytes_are_the_callers},
	{"tests::dropped_error_panics", test_pg_tests_dropped_error_panics, true, "a PgError was dropped instead of raised"},
	{"tests::fork_errors_encode_their_sqlstate", test_pg_tests_fork_errors_encode_their_sqlstate},
	{"tests::only_ordinary_entries_retake_the_reserve", test_pg_tests_only_ordinary_entries_retake_the_reserve},
	{"tests::the_allocator_passes_requests_through", test_pg_tests_the_allocator_passes_requests_through},
	{"tests::the_heap_in_use_follows_a_blocks_allocation_growth_and_free", test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free},
	{"word::tests::words_read_back_as_they_were_written",
	test_pg_word_tests_words_read_back_as_they_were_written},
	{"zstd::tests::a_dictionary_round_trips_what_it_compresses",
	test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses},
	{"zstd::tests::compression_round_trips", test_pg_zstd_tests_compression_round_trips},
	{"zstd::tests::damaged_frames_fail_as_damaged", test_pg_zstd_tests_damaged_frames_fail_as_damaged},
	{"zstd::tests::damaged_frames_record_no_size", test_pg_zstd_tests_damaged_frames_record_no_size},
	{"zstd::tests::too_few_samples_train_nothing", test_pg_zstd_tests_too_few_samples_train_nothing},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_pg);

/* doltgres_cport_test_pg runs pg's tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_pg(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}
