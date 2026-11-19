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
 * The C port's txn unit tests (the port text's section 13), txn's crate
 * built with DOLTGRES_TEST_TXN as cargo builds its unit tests, run in a
 * backend: one row per test, in the Rust's order, with its outcome.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres/txn/base.h"
#include "doltgres/txn/counted.h"
#include "doltgres/txn/free_space.h"
#include "doltgres/txn/state.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are txn's tests, in the order the Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"base::tests::a_base_record_round_trips_at_its_limits",
	test_txn_base_tests_a_base_record_round_trips_at_its_limits},
	{"base::tests::a_chain_entry_round_trips_and_every_other_form_is_corrupt",
	test_txn_base_tests_a_chain_entry_round_trips_and_every_other_form_is_corrupt},
	{"base::tests::a_cut_short_or_unlocated_record_is_corrupt",
	test_txn_base_tests_a_cut_short_or_unlocated_record_is_corrupt},
	{"base::tests::a_storage_key_round_trips_and_every_other_form_is_corrupt",
	test_txn_base_tests_a_storage_key_round_trips_and_every_other_form_is_corrupt},
	{"base::tests::every_other_form_of_a_base_record_is_corrupt",
	test_txn_base_tests_every_other_form_of_a_base_record_is_corrupt},
	{"base::tests::planned_counts_follow_the_sizes_alone",
	test_txn_base_tests_planned_counts_follow_the_sizes_alone},
	{"base::tests::planned_counts_of_entries_larger_than_a_node_are_those_of_the_tree_the_splitter_makes",
	test_txn_base_tests_planned_counts_of_entries_larger_than_a_node_are_those_of_the_tree_the_splitter_makes},
	{"cport::txn::free_space::tests::an_entry_past_the_bitmap_panics_at_encode",
		test_txn_free_space_tests_an_entry_past_the_bitmap_panics_at_encode, true,
	"range end index 250 out of range for slice of length 147"},
	{"cport::txn::free_space::tests::an_entry_past_the_bitmap_panics_at_is_clear",
		test_txn_free_space_tests_an_entry_past_the_bitmap_panics_at_is_clear, true,
	"range end index 250 out of range for slice of length 147"},
	{"cport::txn::free_space::tests::an_entry_past_the_bitmap_panics_at_lowest",
		test_txn_free_space_tests_an_entry_past_the_bitmap_panics_at_lowest, true,
	"range end index 250 out of range for slice of length 147"},
	{"counted::tests::a_keyless_value_is_its_count_and_its_tuple",
	test_txn_counted_tests_a_keyless_value_is_its_count_and_its_tuple},
	{"free_space::tests::a_free_space_entry_reads_back_and_refuses_every_other_form",
	test_txn_free_space_tests_a_free_space_entry_reads_back_and_refuses_every_other_form},
	{"free_space::tests::a_seek_finds_the_lowest_block_with_room_through_the_summaries",
	test_txn_free_space_tests_a_seek_finds_the_lowest_block_with_room_through_the_summaries},
	{"free_space::tests::an_insert_takes_the_lowest_free_offset_and_a_left_fresh_page_frees_its_counter_offsets",
	test_txn_free_space_tests_an_insert_takes_the_lowest_free_offset_and_a_left_fresh_page_frees_its_counter_offsets},
	{"state::tests::the_largest_base_record_fits_a_states_blob_and_reads_back",
	test_txn_state_tests_the_largest_base_record_fits_a_states_blob_and_reads_back},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_txn);

/* doltgres_cport_test_txn runs txn's tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_txn(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}
