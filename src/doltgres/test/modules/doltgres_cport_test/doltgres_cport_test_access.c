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
 * The C port's access unit tests (the port text's section 13), access'
 * crate built with DOLTGRES_TEST_ACCESS as cargo builds its unit tests, run
 * in a backend: one row per test, in the Rust's order, with its outcome.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres/access/heaptuple.h"
#include "doltgres/access/index.h"
#include "doltgres/access/key.h"
#include "doltgres/access/nbtutils.h"
#include "doltgres/access/relation.h"
#include "doltgres/access/slot.h"
#include "doltgres/access/transform.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are access' tests, in the order the Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"cport::access::key::tests::an_answer_of_none_leaves_its_length_as_it_was", test_access_key_tests_an_answer_of_none_leaves_its_length_as_it_was},
	{"cport::access::nbtutils::tests::a_scan_with_no_ordering_functions_leaves_its_function_as_it_was", test_access_nbtutils_tests_a_scan_with_no_ordering_functions_leaves_its_function_as_it_was},
	{"cport::access::slot::tests::a_deform_that_meets_no_overflow_chunk_leaves_its_chunk_as_it_was", test_access_slot_tests_a_deform_that_meets_no_overflow_chunk_leaves_its_chunk_as_it_was},
	{"heaptuple::tests::a_chunks_first_field_too_long_for_it_moves_out_of_line", test_access_heaptuple_tests_a_chunks_first_field_too_long_for_it_moves_out_of_line},
	{"heaptuple::tests::a_key_column_under_at_and_over_its_bound_forms_and_deforms_to_its_value", test_access_heaptuple_tests_a_key_column_under_at_and_over_its_bound_forms_and_deforms_to_its_value},
	{"heaptuple::tests::a_keyless_row_is_keyed_by_its_tuples_hash", test_access_heaptuple_tests_a_keyless_row_is_keyed_by_its_tuples_hash},
	{"heaptuple::tests::a_lossy_key_column_and_short_tuples_read_from_the_tuple", test_access_heaptuple_tests_a_lossy_key_column_and_short_tuples_read_from_the_tuple},
	{"heaptuple::tests::a_lossy_key_column_forms_into_both", test_access_heaptuple_tests_a_lossy_key_column_forms_into_both},
	{"heaptuple::tests::a_row_deforms_to_its_fields_and_key_columns", test_access_heaptuple_tests_a_row_deforms_to_its_fields_and_key_columns},
	{"heaptuple::tests::a_row_forms_to_the_key_and_tuple_it_deforms_from", test_access_heaptuple_tests_a_row_forms_to_the_key_and_tuple_it_deforms_from},
	{"heaptuple::tests::a_row_holding_a_value_out_of_line_toasts_to_its_target", test_access_heaptuple_tests_a_row_holding_a_value_out_of_line_toasts_to_its_target},
	{"heaptuple::tests::a_row_of_main_values_counts_the_heap_tuple_heaps_rules_can_store", test_access_heaptuple_tests_a_row_of_main_values_counts_the_heap_tuple_heaps_rules_can_store},
	{"heaptuple::tests::a_row_past_the_threshold_moves_its_biggest_extended_and_external_values_out_first", test_access_heaptuple_tests_a_row_past_the_threshold_moves_its_biggest_extended_and_external_values_out_first},
	{"heaptuple::tests::a_row_stored_at_an_older_epoch_deforms_through_its_transform", test_access_heaptuple_tests_a_row_stored_at_an_older_epoch_deforms_through_its_transform},
	{"heaptuple::tests::a_row_that_does_not_decode_is_refused", test_access_heaptuple_tests_a_row_that_does_not_decode_is_refused},
	{"heaptuple::tests::a_row_that_does_not_form_is_refused", test_access_heaptuple_tests_a_row_that_does_not_form_is_refused},
	{"heaptuple::tests::a_row_with_a_moved_value_and_two_nested_overflow_chunks_walks_every_request", test_access_heaptuple_tests_a_row_with_a_moved_value_and_two_nested_overflow_chunks_walks_every_request},
	{"heaptuple::tests::a_rows_heap_length_is_the_length_heap_gives_it", test_access_heaptuple_tests_a_rows_heap_length_is_the_length_heap_gives_it},
	{"heaptuple::tests::a_value_kept_out_of_line_deforms_to_where_it_is", test_access_heaptuple_tests_a_value_kept_out_of_line_deforms_to_where_it_is},
	{"heaptuple::tests::a_value_moves_out_of_line_by_its_rows_heap_tuple", test_access_heaptuple_tests_a_value_moves_out_of_line_by_its_rows_heap_tuple},
	{"heaptuple::tests::a_whole_deform_answers_the_rows_count_of_values_out_of_line", test_access_heaptuple_tests_a_whole_deform_answers_the_rows_count_of_values_out_of_line},
	{"heaptuple::tests::deforming_goes_on_from_where_it_stopped", test_access_heaptuple_tests_deforming_goes_on_from_where_it_stopped},
	{"heaptuple::tests::main_values_move_out_only_while_the_row_is_too_large_for_a_node", test_access_heaptuple_tests_main_values_move_out_only_while_the_row_is_too_large_for_a_node},
	{"heaptuple::tests::rows_whose_cut_keys_share_their_first_bytes_have_their_own_keys", test_access_heaptuple_tests_rows_whose_cut_keys_share_their_first_bytes_have_their_own_keys},
	{"index::tests::a_deferrable_unique_index_is_keyed_as_not_unique", test_access_index_tests_a_deferrable_unique_index_is_keyed_as_not_unique},
	{"index::tests::a_key_column_is_cut_at_the_indexs_bound", test_access_index_tests_a_key_column_is_cut_at_the_indexs_bound},
	{"index::tests::a_non_unique_indexs_entries_are_doltgrusts", test_access_index_tests_a_non_unique_indexs_entries_are_doltgrusts},
	{"index::tests::a_unique_index_keys_equal_values_alike", test_access_index_tests_a_unique_index_keys_equal_values_alike},
	{"index::tests::a_unique_indexs_bound_counts_its_values_row_key", test_access_index_tests_a_unique_indexs_bound_counts_its_values_row_key},
	{"index::tests::a_unique_indexs_entry_wider_than_nbtrees_limit_is_formed", test_access_index_tests_a_unique_indexs_entry_wider_than_nbtrees_limit_is_formed},
	{"index::tests::a_unique_indexs_largest_formed_entry_fits", test_access_index_tests_a_unique_indexs_largest_formed_entry_fits},
	{"index::tests::an_include_column_moves_out_of_line_and_a_keyless_entry_leaves_room_for_its_count", test_access_index_tests_an_include_column_moves_out_of_line_and_a_keyless_entry_leaves_room_for_its_count},
	{"index::tests::each_part_of_an_entrys_value_reads_back", test_access_index_tests_each_part_of_an_entrys_value_reads_back},
	{"index::tests::two_cut_keys_sharing_their_first_bytes_differ", test_access_index_tests_two_cut_keys_sharing_their_first_bytes_differ},
	{"key::tests::a_bpchar_key_ignores_trailing_spaces_and_is_skipped_to_its_end", test_access_key_tests_a_bpchar_key_ignores_trailing_spaces_and_is_skipped_to_its_end},
	{"key::tests::a_key_column_is_cut_one_byte_past_its_bound", test_access_key_tests_a_key_column_is_cut_one_byte_past_its_bound},
	{"key::tests::a_key_column_past_its_bound_is_cut_and_skipped_by_the_bound", test_access_key_tests_a_key_column_past_its_bound_is_cut_and_skipped_by_the_bound},
	{"key::tests::a_stored_value_that_is_not_its_types_form_is_refused", test_access_key_tests_a_stored_value_that_is_not_its_types_form_is_refused},
	{"key::tests::a_tables_bound_follows_its_keys_types", test_access_key_tests_a_tables_bound_follows_its_keys_types},
	{"key::tests::a_tsvector_of_many_short_lexemes_reads_back_from_its_key", test_access_key_tests_a_tsvector_of_many_short_lexemes_reads_back_from_its_key},
	{"key::tests::a_type_without_an_encoding_and_a_key_that_does_not_decode_are_refused", test_access_key_tests_a_type_without_an_encoding_and_a_key_that_does_not_decode_are_refused},
	{"key::tests::a_value_read_from_a_key_is_within_its_bound", test_access_key_tests_a_value_read_from_a_key_is_within_its_bound},
	{"key::tests::an_indexs_bound_fits_its_largest_entry", test_access_key_tests_an_indexs_bound_fits_its_largest_entry},
	{"key::tests::arrays_order_as_postgres_compares_them", test_access_key_tests_arrays_order_as_postgres_compares_them},
	{"key::tests::collated_text_keys_are_their_sort_keys_then_their_bytes", test_access_key_tests_collated_text_keys_are_their_sort_keys_then_their_bytes},
	{"key::tests::compared_and_enum_keys_are_their_bytes_and_sort_orders", test_access_key_tests_compared_and_enum_keys_are_their_bytes_and_sort_orders},
	{"key::tests::exact_keys_round_trip_and_keys_order_as_postgres_does", test_access_key_tests_exact_keys_round_trip_and_keys_order_as_postgres_does},
	{"key::tests::float_keys_order_as_postgres_does_and_are_skipped_by_their_width", test_access_key_tests_float_keys_order_as_postgres_does_and_are_skipped_by_their_width},
	{"key::tests::numeric_and_interval_keys_order_as_postgres_does_and_are_skipped", test_access_key_tests_numeric_and_interval_keys_order_as_postgres_does_and_are_skipped},
	{"key::tests::the_keys_of_stored_forms_order_as_postgres_does", test_access_key_tests_the_keys_of_stored_forms_order_as_postgres_does},
	{"key::tests::the_stored_forms_of_exact_types_round_trip", test_access_key_tests_the_stored_forms_of_exact_types_round_trip},
	{"relation::tests::a_relation_made_to_rewrite_a_catalog_is_not_stored",
	test_access_relation_tests_a_relation_made_to_rewrite_a_catalog_is_not_stored},
	{"relation::tests::a_users_tables_materialized_views_and_toast_tables_are_stored",
	test_access_relation_tests_a_users_tables_materialized_views_and_toast_tables_are_stored},
	{"slot::tests::a_key_columns_bytes_hold_its_position_order_and_type", test_access_slot_tests_a_key_columns_bytes_hold_its_position_order_and_type},
	{"slot::tests::a_tuple_id_splits_into_its_block_and_offset", test_access_slot_tests_a_tuple_id_splits_into_its_block_and_offset},
	{"transform::tests::composed", test_access_transform_tests_composed},
	{"transform::tests::round_trip", test_access_transform_tests_round_trip},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_access);

/* doltgres_cport_test_access runs access' tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_access(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}
