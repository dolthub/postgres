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
 * The C port's trees unit tests (the port text's section 13), trees' crate
 * built with DOLTGRES_TEST_TREES as cargo builds its unit tests, run in a
 * backend: one row per test, in the Rust's order, with its outcome.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres/trees/chunker.h"
#include "doltgres/trees/mutate.h"
#include "doltgres/trees/node.h"
#include "doltgres/trees/store.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are trees' tests, in the order the Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"chunker::tests::internal_nodes_never_end_after_one_entry",
	test_trees_chunker_tests_internal_nodes_never_end_after_one_entry},
	{"chunker::tests::node_sizes_follow_the_target", test_trees_chunker_tests_node_sizes_follow_the_target},
	{"cport::trees::store::tests::an_own_read_of_a_forwarded_copy_leaves_its_place_as_it_was",
	test_trees_store_tests_an_own_read_of_a_forwarded_copy_leaves_its_place_as_it_was},
	{"mutate::tests::a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree",
	test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree},
	{"node::tests::front_coded_leaf_keys_round_trip_at_prefix_boundaries",
	test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries},
	{"node::tests::leaves_and_internal_nodes_round_trip", test_trees_node_tests_leaves_and_internal_nodes_round_trip},
	{"node::tests::refuses_offsets_and_counts_its_accessors_cannot_read",
	test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read},
	{"store::tests::a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded",
	test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded},
	{"store::tests::a_writers_failed_read_of_its_own_copy_returns_the_hosts_error_and_not_the_last_copy",
	test_trees_store_tests_a_writers_failed_read_of_its_own_copy_returns_the_hosts_error_and_not_the_last_copy},
	{"store::tests::a_writers_next_pack_trains_or_shares_a_dictionary_as_its_packs_do",
	test_trees_store_tests_a_writers_next_pack_trains_or_shares_a_dictionary_as_its_packs_do},
	{"store::tests::a_writers_read_by_address_alone_of_a_chunk_it_wrote_with_other_stamps_is_refused_and_of_equal_copies_answers",
	test_trees_store_tests_a_writers_read_by_address_alone_of_a_chunk_it_wrote_with_other_stamps_is_refused_and_of_equal_copies_answers},
	{"store::tests::copies_in_packs_with_wide_numbers_have_their_own_keys",
	test_trees_store_tests_copies_in_packs_with_wide_numbers_have_their_own_keys},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_trees);

/* doltgres_cport_test_trees runs trees' tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_trees(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}
