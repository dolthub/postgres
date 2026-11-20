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
#include "doltgres/doltgres/kinds.h"
#include "doltgres/doltgres/layout.h"
#include "doltgres/doltgres/locale.h"
#include "doltgres/doltgres/relation.h"
#include "doltgres/doltgres/scan.h"
#include "doltgres/doltgres/sizes.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are the doltgres crate's tests, in the order the Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"cport::doltgres::scan::tests::a_scan_with_no_sample_or_range_leaves_its_answers_as_they_were",
	test_doltgres_scan_tests_a_scan_with_no_sample_or_range_leaves_its_answers_as_they_were},
	{"deletes::tests::a_bottom_up_deletion_visits_heaps_blocks_in_heaps_order",
	test_doltgres_deletes_tests_a_bottom_up_deletion_visits_heaps_blocks_in_heaps_order},
	{"deletes::tests::a_simple_deletion_marks_the_rows_no_snapshot_keeps",
	test_doltgres_deletes_tests_a_simple_deletion_marks_the_rows_no_snapshot_keeps},
	{"kinds::tests::commits_is_listed",
	test_doltgres_kinds_tests_commits_is_listed},
	{"kinds::tests::the_kinds_ids_are_distinct",
	test_doltgres_kinds_tests_the_kinds_ids_are_distinct},
	{"kinds::tests::the_table_is_ordered",
	test_doltgres_kinds_tests_the_table_is_ordered},
	{"layout::tests::a_comparator_orders_compared_columns_by_their_function",
	test_doltgres_layout_tests_a_comparator_orders_compared_columns_by_their_function},
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
	{"relation::tests::a_creation_speaks_for_its_relation_at_its_level",
	test_doltgres_relation_tests_a_creation_speaks_for_its_relation_at_its_level},
	{"scan::tests::a_compared_scan_merges_the_deltas_keys_in_the_tables_order",
	test_doltgres_scan_tests_a_compared_scan_merges_the_deltas_keys_in_the_tables_order},
	{"scan::tests::a_dirty_scan_gives_what_it_found_running_of_each_row",
	test_doltgres_scan_tests_a_dirty_scan_gives_what_it_found_running_of_each_row},
	{"scan::tests::a_fetch_gives_the_row_its_tuple_id_names",
	test_doltgres_scan_tests_a_fetch_gives_the_row_its_tuple_id_names},
	{"scan::tests::a_fetch_reads_the_deltas_rows_and_the_bases_by_tuple_id",
	test_doltgres_scan_tests_a_fetch_reads_the_deltas_rows_and_the_bases_by_tuple_id},
	{"scan::tests::a_keyed_tables_index_entry_is_one_copy_stamped_zero",
	test_doltgres_scan_tests_a_keyed_tables_index_entry_is_one_copy_stamped_zero},
	{"scan::tests::a_keyless_entry_gives_each_copy_with_its_own_stamp",
	test_doltgres_scan_tests_a_keyless_entry_gives_each_copy_with_its_own_stamp},
	{"scan::tests::a_keyless_keys_rows_are_its_base_copies_the_delta_leaves_then_its_added_copies",
	test_doltgres_scan_tests_a_keyless_keys_rows_are_its_base_copies_the_delta_leaves_then_its_added_copies},
	{"scan::tests::a_leaders_split_keys_are_its_range_starts_spread_within_the_budget",
	test_doltgres_scan_tests_a_leaders_split_keys_are_its_range_starts_spread_within_the_budget},
	{"scan::tests::a_parallel_scan_over_an_empty_base_reads_each_delta_key_once_by_the_leaders_splits",
	test_doltgres_scan_tests_a_parallel_scan_over_an_empty_base_reads_each_delta_key_once_by_the_leaders_splits},
	{"scan::tests::a_parallel_scans_units_give_each_row_once",
	test_doltgres_scan_tests_a_parallel_scans_units_give_each_row_once},
	{"scan::tests::a_parallel_scans_units_merge_the_delta_each_key_once",
	test_doltgres_scan_tests_a_parallel_scans_units_merge_the_delta_each_key_once},
	{"scan::tests::a_parallel_scans_units_partition_the_entries",
	test_doltgres_scan_tests_a_parallel_scans_units_partition_the_entries},
	{"scan::tests::a_parallel_tid_ranges_blocks_are_heaps",
	test_doltgres_scan_tests_a_parallel_tid_ranges_blocks_are_heaps},
	{"scan::tests::a_sample_of_many_blocks_walks_the_delta_once",
	test_doltgres_scan_tests_a_sample_of_many_blocks_walks_the_delta_once},
	{"scan::tests::a_scan_merges_the_deltas_rows_with_the_bases_in_key_order",
	test_doltgres_scan_tests_a_scan_merges_the_deltas_rows_with_the_bases_in_key_order},
	{"scan::tests::a_scan_reads_a_bases_rows_in_order_across_calls",
	test_doltgres_scan_tests_a_scan_reads_a_bases_rows_in_order_across_calls},
	{"scan::tests::a_scan_reads_backward_and_turns_as_heaps_does",
	test_doltgres_scan_tests_a_scan_reads_backward_and_turns_as_heaps_does},
	{"scan::tests::a_seek_reads_on_from_the_first_key_at_or_past_it_that_way",
	test_doltgres_scan_tests_a_seek_reads_on_from_the_first_key_at_or_past_it_that_way},
	{"scan::tests::a_tid_range_pair_the_table_lacks_is_data_corruption",
	test_doltgres_scan_tests_a_tid_range_pair_the_table_lacks_is_data_corruption},
	{"scan::tests::a_tid_range_read_gives_its_rows_in_tuple_id_order",
	test_doltgres_scan_tests_a_tid_range_read_gives_its_rows_in_tuple_id_order},
	{"scan::tests::a_tid_range_read_merges_the_deltas_tuple_ids_with_the_bases",
	test_doltgres_scan_tests_a_tid_range_read_merges_the_deltas_tuple_ids_with_the_bases},
	{"scan::tests::a_tuple_id_is_valid_within_the_storages_blocks",
	test_doltgres_scan_tests_a_tuple_id_is_valid_within_the_storages_blocks},
	{"scan::tests::each_keyless_copy_fetches_by_its_own_tuple_id",
	test_doltgres_scan_tests_each_keyless_copy_fetches_by_its_own_tuple_id},
	{"scan::tests::scans_of_one_base_keep_their_own_places",
	test_doltgres_scan_tests_scans_of_one_base_keep_their_own_places},
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
