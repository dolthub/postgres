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
 * The C port's shmem unit tests (the port text's section 13), shmem's crate
 * built with DOLTGRES_TEST_SHMEM as cargo builds its unit tests, run in a
 * backend: one row per test, in the Rust's order, with its outcome.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres/shmem/cache.h"
#include "doltgres/shmem/delta.h"
#include "doltgres/shmem/lib.h"
#include "doltgres/shmem/records.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are shmem's tests, in the order the Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"cache::tests::a_layouts_parts_follow_one_another", test_shmem_cache_tests_a_layouts_parts_follow_one_another},
	{"cache::tests::every_frames_descriptor_and_bytes_lie_in_the_cache_apart",
	test_shmem_cache_tests_every_frames_descriptor_and_bytes_lie_in_the_cache_apart},
	{"delta::tests::the_directory_is_a_header_and_slots_of_a_cache_line",
	test_shmem_delta_tests_the_directory_is_a_header_and_slots_of_a_cache_line},
	{"records::tests::a_process_has_a_record_of_its_words", test_shmem_records_tests_a_process_has_a_record_of_its_words},
	{"records::tests::a_records_parts_do_not_overlap", test_shmem_records_tests_a_records_parts_do_not_overlap},
	{"records::tests::the_region_is_the_sum_of_its_kinds", test_shmem_records_tests_the_region_is_the_sum_of_its_kinds},
	{"tests::the_lock_is_at_the_regions_address", test_shmem_tests_the_lock_is_at_the_regions_address},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_shmem);

/* doltgres_cport_test_shmem runs shmem's tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_shmem(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}
