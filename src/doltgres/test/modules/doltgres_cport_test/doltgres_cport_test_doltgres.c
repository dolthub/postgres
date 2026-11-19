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
 * The C port's doltgres unit tests (the port text's section 13), the
 * doltgres crate built with DOLTGRES_TEST_DOLTGRES as cargo builds its unit
 * tests, run in a backend: one row per test, in the Rust's order, with its
 * outcome.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres/doltgres/deletes.h"
#include "doltgres/doltgres/locale.h"
#include "doltgres/doltgres/sizes.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are the doltgres crate's tests, in the order the Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"deletes::tests::a_bottom_up_deletion_visits_heaps_blocks_in_heaps_order",
	test_doltgres_deletes_tests_a_bottom_up_deletion_visits_heaps_blocks_in_heaps_order},
	{"deletes::tests::a_simple_deletion_marks_the_rows_no_snapshot_keeps",
	test_doltgres_deletes_tests_a_simple_deletion_marks_the_rows_no_snapshot_keeps},
	{"locale::tests::a_c_utf8_ctype_takes_the_builtin_classes",
	test_doltgres_locale_tests_a_c_utf8_ctype_takes_the_builtin_classes},
	{"locale::tests::a_glibc_alias_is_its_locales_tag",
	test_doltgres_locale_tests_a_glibc_alias_is_its_locales_tag},
	{"locale::tests::a_language_tag_is_itself",
	test_doltgres_locale_tests_a_language_tag_is_itself},
	{"locale::tests::a_modifier_is_the_subtag_it_names",
	test_doltgres_locale_tests_a_modifier_is_the_subtag_it_names},
	{"locale::tests::a_name_of_no_form_is_unknown",
	test_doltgres_locale_tests_a_name_of_no_form_is_unknown},
	{"locale::tests::a_named_half_is_the_collators",
	test_doltgres_locale_tests_a_named_half_is_the_collators},
	{"locale::tests::a_posix_name_is_its_language_and_territory",
	test_doltgres_locale_tests_a_posix_name_is_its_language_and_territory},
	{"locale::tests::a_posix_name_may_state_its_script",
	test_doltgres_locale_tests_a_posix_name_may_state_its_script},
	{"locale::tests::a_sort_name_is_its_rows_tag_or_unknown",
	test_doltgres_locale_tests_a_sort_name_is_its_rows_tag_or_unknown},
	{"locale::tests::a_windows_name_is_its_rows_tag",
	test_doltgres_locale_tests_a_windows_name_is_its_rows_tag},
	{"locale::tests::an_unknown_part_is_a_modifier_or_a_sort_name",
	test_doltgres_locale_tests_an_unknown_part_is_a_modifier_or_a_sort_name},
	{"locale::tests::every_row_is_found_under_its_name",
	test_doltgres_locale_tests_every_row_is_found_under_its_name},
	{"locale::tests::every_sort_is_one_icu_maps",
	test_doltgres_locale_tests_every_sort_is_one_icu_maps},
	{"locale::tests::the_c_family_is_postgres_own",
	test_doltgres_locale_tests_the_c_family_is_postgres_own},
	{"locale::tests::the_c_locales_sort_by_bytes",
	test_doltgres_locale_tests_the_c_locales_sort_by_bytes},
	{"locale::tests::the_names_of_earlier_windows_releases_are_rows",
	test_doltgres_locale_tests_the_names_of_earlier_windows_releases_are_rows},
	{"locale::tests::the_names_postgres_maps_are_rows",
	test_doltgres_locale_tests_the_names_postgres_maps_are_rows},
	{"locale::tests::two_tags_are_two_asks",
	test_doltgres_locale_tests_two_tags_are_two_asks},
	{"sizes::tests::a_tables_blocks_are_its_rows_packed_and_at_least_its_ctid_blocks",
	test_doltgres_sizes_tests_a_tables_blocks_are_its_rows_packed_and_at_least_its_ctid_blocks},
	{"sizes::tests::a_tables_delta_rows_count_their_ctid_blocks",
	test_doltgres_sizes_tests_a_tables_delta_rows_count_their_ctid_blocks},
	{"sizes::tests::a_toast_relations_blocks_are_its_values_packed_whole",
	test_doltgres_sizes_tests_a_toast_relations_blocks_are_its_values_packed_whole},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_doltgres);

/* doltgres_cport_test_doltgres runs the doltgres crate's tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_doltgres(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}
