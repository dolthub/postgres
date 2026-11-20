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
 * The C port's encoding unit tests (the port text's section 13), encoding's
 * crate built with DOLTGRES_TEST_ENCODING as cargo builds its unit tests, run
 * in a backend: one row per test, in the Rust's order, with its outcome.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres/encoding/key.h"
#include "doltgres/encoding/summary.h"
#include "doltgres/encoding/tuple.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are encoding's tests, in the order the Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"key::tests::descending_columns_invert_and_keep_nulls_placed",
	test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed},
	{"key::tests::digits_and_times_with_zones_outside_their_bounds_are_refused",
	test_encoding_key_tests_digits_and_times_with_zones_outside_their_bounds_are_refused},
	{"key::tests::numerics_drop_trailing_zeros",
	test_encoding_key_tests_numerics_drop_trailing_zeros},
	{"key::tests::sortable_ints_order_as_integers",
	test_encoding_key_tests_sortable_ints_order_as_integers},
	{"summary::tests::each_columns_union_bytes_stay_as_they_are_stored",
	test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored},
	{"summary::tests::range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items",
	test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items},
	{"summary::tests::text_search_unions_bytes_stay_as_they_are_stored",
	test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored},
	{"tuple::tests::external_fields_name_their_blobs",
	test_encoding_tuple_tests_external_fields_name_their_blobs},
	{"tuple::tests::fields_round_trip",
	test_encoding_tuple_tests_fields_round_trip},
	{"tuple::tests::malformed_ends_are_refused",
	test_encoding_tuple_tests_malformed_ends_are_refused},
	{"tuple::tests::overflow_names_the_rest",
	test_encoding_tuple_tests_overflow_names_the_rest},
	{"tuple::tests::written_lengths_match_the_tuples",
	test_encoding_tuple_tests_written_lengths_match_the_tuples},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_encoding);

/* doltgres_cport_test_encoding runs encoding's tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_encoding(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}
