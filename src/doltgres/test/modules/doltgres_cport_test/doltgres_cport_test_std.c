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
 * The C port's std tests (the port text's section 13): core's own tests of
 * the std forms that port a core algorithm, by their coretests paths, run in
 * a backend one row each; and the runner's cases of a test's captured
 * output.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres/std/c_str.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/io.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_flt2dec.h"
#include "doltgres/std/test_fmt.h"
#include "doltgres/std/test_sort.h"
#include "doltgres/std/test_str.h"
#include "doltgres/std/time.h"
#include "doltgres/std/unwind.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are the std forms' tests, as coretests' harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"c_str::tests::a_cstrs_debug_escapes_as_rusts", test_std_c_str_tests_a_cstrs_debug_escapes_as_rusts},
	{"fmt::builders::debug_list::test_empty", test_coretests_fmt_builders_debug_list_test_empty},
	{"fmt::builders::debug_list::test_multiple", test_coretests_fmt_builders_debug_list_test_multiple},
	{"fmt::builders::debug_list::test_nested", test_coretests_fmt_builders_debug_list_test_nested},
	{"fmt::builders::debug_list::test_single", test_coretests_fmt_builders_debug_list_test_single},
	{"fmt::builders::debug_struct::test_empty", test_coretests_fmt_builders_debug_struct_test_empty},
	{"fmt::builders::debug_struct::test_empty_non_exhaustive", test_coretests_fmt_builders_debug_struct_test_empty_non_exhaustive},
	{"fmt::builders::debug_struct::test_multiple", test_coretests_fmt_builders_debug_struct_test_multiple},
	{"fmt::builders::debug_struct::test_multiple_and_non_exhaustive", test_coretests_fmt_builders_debug_struct_test_multiple_and_non_exhaustive},
	{"fmt::builders::debug_struct::test_nested", test_coretests_fmt_builders_debug_struct_test_nested},
	{"fmt::builders::debug_struct::test_nested_non_exhaustive", test_coretests_fmt_builders_debug_struct_test_nested_non_exhaustive},
	{"fmt::builders::debug_struct::test_single", test_coretests_fmt_builders_debug_struct_test_single},
	{"fmt::builders::debug_tuple::test_empty", test_coretests_fmt_builders_debug_tuple_test_empty},
	{"fmt::builders::debug_tuple::test_multiple", test_coretests_fmt_builders_debug_tuple_test_multiple},
	{"fmt::builders::debug_tuple::test_nested", test_coretests_fmt_builders_debug_tuple_test_nested},
	{"fmt::builders::debug_tuple::test_single", test_coretests_fmt_builders_debug_tuple_test_single},
	{"int128::tests::u128_forms_match_rust", test_std_int128_tests_u128_forms_match_rust},
	{"io::tests::error_forms_match_rust", test_std_io_tests_error_forms_match_rust},
	{"num::flt2dec::estimator::test_estimate_scaling_factor",
	test_coretests_num_flt2dec_estimator_test_estimate_scaling_factor},
	{"num::flt2dec::random::exact_f16_random_equivalence_test",
	test_coretests_num_flt2dec_random_exact_f16_random_equivalence_test},
	{"num::flt2dec::random::exact_f32_random_equivalence_test",
	test_coretests_num_flt2dec_random_exact_f32_random_equivalence_test},
	{"num::flt2dec::random::exact_f64_random_equivalence_test",
	test_coretests_num_flt2dec_random_exact_f64_random_equivalence_test},
	{"num::flt2dec::random::shortest_f16_exhaustive_equivalence_test",
	test_coretests_num_flt2dec_random_shortest_f16_exhaustive_equivalence_test},
	{"num::flt2dec::random::shortest_random_equivalence_test",
	test_coretests_num_flt2dec_random_shortest_random_equivalence_test},
	{"num::flt2dec::strategy::dragon::exact_sanity_test",
	test_coretests_num_flt2dec_strategy_dragon_exact_sanity_test},
	{"num::flt2dec::strategy::dragon::shortest_sanity_test",
	test_coretests_num_flt2dec_strategy_dragon_shortest_sanity_test},
	{"num::flt2dec::strategy::dragon::test_mul_pow10",
	test_coretests_num_flt2dec_strategy_dragon_test_mul_pow10},
	{"num::flt2dec::strategy::dragon::test_to_exact_exp_str",
	test_coretests_num_flt2dec_strategy_dragon_test_to_exact_exp_str},
	{"num::flt2dec::strategy::dragon::test_to_exact_fixed_str",
	test_coretests_num_flt2dec_strategy_dragon_test_to_exact_fixed_str},
	{"num::flt2dec::strategy::dragon::test_to_shortest_exp_str",
	test_coretests_num_flt2dec_strategy_dragon_test_to_shortest_exp_str},
	{"num::flt2dec::strategy::dragon::test_to_shortest_str",
	test_coretests_num_flt2dec_strategy_dragon_test_to_shortest_str},
	{"num::flt2dec::strategy::grisu::exact_sanity_test",
	test_coretests_num_flt2dec_strategy_grisu_exact_sanity_test},
	{"num::flt2dec::strategy::grisu::shortest_sanity_test",
	test_coretests_num_flt2dec_strategy_grisu_shortest_sanity_test},
	{"num::flt2dec::strategy::grisu::test_cached_power",
	test_coretests_num_flt2dec_strategy_grisu_test_cached_power},
	{"num::flt2dec::strategy::grisu::test_max_pow10_no_more_than",
	test_coretests_num_flt2dec_strategy_grisu_test_max_pow10_no_more_than},
	{"num::flt2dec::strategy::grisu::test_to_exact_exp_str",
	test_coretests_num_flt2dec_strategy_grisu_test_to_exact_exp_str},
	{"num::flt2dec::strategy::grisu::test_to_exact_fixed_str",
	test_coretests_num_flt2dec_strategy_grisu_test_to_exact_fixed_str},
	{"num::flt2dec::strategy::grisu::test_to_shortest_exp_str",
	test_coretests_num_flt2dec_strategy_grisu_test_to_shortest_exp_str},
	{"num::flt2dec::strategy::grisu::test_to_shortest_str",
	test_coretests_num_flt2dec_strategy_grisu_test_to_shortest_str},
	{"sort::partial::basic_impl", test_alloctests_sort_partial_basic_impl},
	{"sort::partial::random_patterns", test_alloctests_sort_partial_random_patterns},
	{"sort::tests::stable::basic", test_alloctests_sort_tests_stable_basic},
	{"sort::tests::stable::box_value", test_alloctests_sort_tests_stable_box_value},
	{"sort::tests::stable::correct_1k_ascending", test_alloctests_sort_tests_stable_correct_1k_ascending},
	{"sort::tests::stable::correct_1k_descending", test_alloctests_sort_tests_stable_correct_1k_descending},
	{"sort::tests::stable::correct_1k_random", test_alloctests_sort_tests_stable_correct_1k_random},
	{"sort::tests::stable::correct_1k_random_d2", test_alloctests_sort_tests_stable_correct_1k_random_d2},
	{"sort::tests::stable::correct_1k_random_d20", test_alloctests_sort_tests_stable_correct_1k_random_d20},
	{"sort::tests::stable::correct_1k_random_s95", test_alloctests_sort_tests_stable_correct_1k_random_s95},
	{"sort::tests::stable::correct_1k_random_z1", test_alloctests_sort_tests_stable_correct_1k_random_z1},
	{"sort::tests::stable::correct_1k_saw_mixed", test_alloctests_sort_tests_stable_correct_1k_saw_mixed},
	{"sort::tests::stable::correct_cell_i32_ascending", test_alloctests_sort_tests_stable_correct_cell_i32_ascending},
	{"sort::tests::stable::correct_cell_i32_descending", test_alloctests_sort_tests_stable_correct_cell_i32_descending},
	{"sort::tests::stable::correct_cell_i32_random", test_alloctests_sort_tests_stable_correct_cell_i32_random},
	{"sort::tests::stable::correct_cell_i32_random_d2", test_alloctests_sort_tests_stable_correct_cell_i32_random_d2},
	{"sort::tests::stable::correct_cell_i32_random_d20", test_alloctests_sort_tests_stable_correct_cell_i32_random_d20},
	{"sort::tests::stable::correct_cell_i32_random_s95", test_alloctests_sort_tests_stable_correct_cell_i32_random_s95},
	{"sort::tests::stable::correct_cell_i32_random_z1", test_alloctests_sort_tests_stable_correct_cell_i32_random_z1},
	{"sort::tests::stable::correct_cell_i32_saw_mixed", test_alloctests_sort_tests_stable_correct_cell_i32_saw_mixed},
	{"sort::tests::stable::correct_dyn_val_ascending", test_alloctests_sort_tests_stable_correct_dyn_val_ascending},
	{"sort::tests::stable::correct_dyn_val_descending", test_alloctests_sort_tests_stable_correct_dyn_val_descending},
	{"sort::tests::stable::correct_dyn_val_random", test_alloctests_sort_tests_stable_correct_dyn_val_random},
	{"sort::tests::stable::correct_dyn_val_random_d2", test_alloctests_sort_tests_stable_correct_dyn_val_random_d2},
	{"sort::tests::stable::correct_dyn_val_random_d20", test_alloctests_sort_tests_stable_correct_dyn_val_random_d20},
	{"sort::tests::stable::correct_dyn_val_random_s95", test_alloctests_sort_tests_stable_correct_dyn_val_random_s95},
	{"sort::tests::stable::correct_dyn_val_random_z1", test_alloctests_sort_tests_stable_correct_dyn_val_random_z1},
	{"sort::tests::stable::correct_dyn_val_saw_mixed", test_alloctests_sort_tests_stable_correct_dyn_val_saw_mixed},
	{"sort::tests::stable::correct_f128_ascending", test_alloctests_sort_tests_stable_correct_f128_ascending},
	{"sort::tests::stable::correct_f128_descending", test_alloctests_sort_tests_stable_correct_f128_descending},
	{"sort::tests::stable::correct_f128_random", test_alloctests_sort_tests_stable_correct_f128_random},
	{"sort::tests::stable::correct_f128_random_d2", test_alloctests_sort_tests_stable_correct_f128_random_d2},
	{"sort::tests::stable::correct_f128_random_d20", test_alloctests_sort_tests_stable_correct_f128_random_d20},
	{"sort::tests::stable::correct_f128_random_s95", test_alloctests_sort_tests_stable_correct_f128_random_s95},
	{"sort::tests::stable::correct_f128_random_z1", test_alloctests_sort_tests_stable_correct_f128_random_z1},
	{"sort::tests::stable::correct_f128_saw_mixed", test_alloctests_sort_tests_stable_correct_f128_saw_mixed},
	{"sort::tests::stable::correct_i32_all_equal", test_alloctests_sort_tests_stable_correct_i32_all_equal},
	{"sort::tests::stable::correct_i32_ascending", test_alloctests_sort_tests_stable_correct_i32_ascending},
	{"sort::tests::stable::correct_i32_descending", test_alloctests_sort_tests_stable_correct_i32_descending},
	{"sort::tests::stable::correct_i32_narrow", test_alloctests_sort_tests_stable_correct_i32_narrow},
	{"sort::tests::stable::correct_i32_pipe_organ", test_alloctests_sort_tests_stable_correct_i32_pipe_organ},
	{"sort::tests::stable::correct_i32_random", test_alloctests_sort_tests_stable_correct_i32_random},
	{"sort::tests::stable::correct_i32_random_d1024", test_alloctests_sort_tests_stable_correct_i32_random_d1024},
	{"sort::tests::stable::correct_i32_random_d2", test_alloctests_sort_tests_stable_correct_i32_random_d2},
	{"sort::tests::stable::correct_i32_random_d20", test_alloctests_sort_tests_stable_correct_i32_random_d20},
	{"sort::tests::stable::correct_i32_random_d311", test_alloctests_sort_tests_stable_correct_i32_random_d311},
	{"sort::tests::stable::correct_i32_random_d4", test_alloctests_sort_tests_stable_correct_i32_random_d4},
	{"sort::tests::stable::correct_i32_random_d8", test_alloctests_sort_tests_stable_correct_i32_random_d8},
	{"sort::tests::stable::correct_i32_random_s50", test_alloctests_sort_tests_stable_correct_i32_random_s50},
	{"sort::tests::stable::correct_i32_random_s95", test_alloctests_sort_tests_stable_correct_i32_random_s95},
	{"sort::tests::stable::correct_i32_random_z1", test_alloctests_sort_tests_stable_correct_i32_random_z1},
	{"sort::tests::stable::correct_i32_random_z1_03", test_alloctests_sort_tests_stable_correct_i32_random_z1_03},
	{"sort::tests::stable::correct_i32_random_z2", test_alloctests_sort_tests_stable_correct_i32_random_z2},
	{"sort::tests::stable::correct_i32_saw_mixed", test_alloctests_sort_tests_stable_correct_i32_saw_mixed},
	{"sort::tests::stable::correct_i32_saw_mixed_range", test_alloctests_sort_tests_stable_correct_i32_saw_mixed_range},
	{"sort::tests::stable::correct_string_ascending", test_alloctests_sort_tests_stable_correct_string_ascending},
	{"sort::tests::stable::correct_string_descending", test_alloctests_sort_tests_stable_correct_string_descending},
	{"sort::tests::stable::correct_string_random", test_alloctests_sort_tests_stable_correct_string_random},
	{"sort::tests::stable::correct_string_random_d2", test_alloctests_sort_tests_stable_correct_string_random_d2},
	{"sort::tests::stable::correct_string_random_d20", test_alloctests_sort_tests_stable_correct_string_random_d20},
	{"sort::tests::stable::correct_string_random_s95", test_alloctests_sort_tests_stable_correct_string_random_s95},
	{"sort::tests::stable::correct_string_random_z1", test_alloctests_sort_tests_stable_correct_string_random_z1},
	{"sort::tests::stable::correct_string_saw_mixed", test_alloctests_sort_tests_stable_correct_string_saw_mixed},
	{"sort::tests::stable::correct_u128_ascending", test_alloctests_sort_tests_stable_correct_u128_ascending},
	{"sort::tests::stable::correct_u128_descending", test_alloctests_sort_tests_stable_correct_u128_descending},
	{"sort::tests::stable::correct_u128_random", test_alloctests_sort_tests_stable_correct_u128_random},
	{"sort::tests::stable::correct_u128_random_d2", test_alloctests_sort_tests_stable_correct_u128_random_d2},
	{"sort::tests::stable::correct_u128_random_d20", test_alloctests_sort_tests_stable_correct_u128_random_d20},
	{"sort::tests::stable::correct_u128_random_s95", test_alloctests_sort_tests_stable_correct_u128_random_s95},
	{"sort::tests::stable::correct_u128_random_z1", test_alloctests_sort_tests_stable_correct_u128_random_z1},
	{"sort::tests::stable::correct_u128_saw_mixed", test_alloctests_sort_tests_stable_correct_u128_saw_mixed},
	{"sort::tests::stable::correct_u64_ascending", test_alloctests_sort_tests_stable_correct_u64_ascending},
	{"sort::tests::stable::correct_u64_descending", test_alloctests_sort_tests_stable_correct_u64_descending},
	{"sort::tests::stable::correct_u64_random", test_alloctests_sort_tests_stable_correct_u64_random},
	{"sort::tests::stable::correct_u64_random_d2", test_alloctests_sort_tests_stable_correct_u64_random_d2},
	{"sort::tests::stable::correct_u64_random_d20", test_alloctests_sort_tests_stable_correct_u64_random_d20},
	{"sort::tests::stable::correct_u64_random_s95", test_alloctests_sort_tests_stable_correct_u64_random_s95},
	{"sort::tests::stable::correct_u64_random_z1", test_alloctests_sort_tests_stable_correct_u64_random_z1},
	{"sort::tests::stable::correct_u64_saw_mixed", test_alloctests_sort_tests_stable_correct_u64_saw_mixed},
	{"sort::tests::stable::deterministic_cell_i32_ascending", test_alloctests_sort_tests_stable_deterministic_cell_i32_ascending},
	{"sort::tests::stable::deterministic_cell_i32_descending", test_alloctests_sort_tests_stable_deterministic_cell_i32_descending},
	{"sort::tests::stable::deterministic_cell_i32_random", test_alloctests_sort_tests_stable_deterministic_cell_i32_random},
	{"sort::tests::stable::deterministic_cell_i32_random_d2", test_alloctests_sort_tests_stable_deterministic_cell_i32_random_d2},
	{"sort::tests::stable::deterministic_cell_i32_random_d20", test_alloctests_sort_tests_stable_deterministic_cell_i32_random_d20},
	{"sort::tests::stable::deterministic_cell_i32_random_s95", test_alloctests_sort_tests_stable_deterministic_cell_i32_random_s95},
	{"sort::tests::stable::deterministic_cell_i32_random_z1", test_alloctests_sort_tests_stable_deterministic_cell_i32_random_z1},
	{"sort::tests::stable::deterministic_cell_i32_saw_mixed", test_alloctests_sort_tests_stable_deterministic_cell_i32_saw_mixed},
	{"sort::tests::stable::deterministic_i32_ascending", test_alloctests_sort_tests_stable_deterministic_i32_ascending},
	{"sort::tests::stable::deterministic_i32_descending", test_alloctests_sort_tests_stable_deterministic_i32_descending},
	{"sort::tests::stable::deterministic_i32_random", test_alloctests_sort_tests_stable_deterministic_i32_random},
	{"sort::tests::stable::deterministic_i32_random_d2", test_alloctests_sort_tests_stable_deterministic_i32_random_d2},
	{"sort::tests::stable::deterministic_i32_random_d20", test_alloctests_sort_tests_stable_deterministic_i32_random_d20},
	{"sort::tests::stable::deterministic_i32_random_s95", test_alloctests_sort_tests_stable_deterministic_i32_random_s95},
	{"sort::tests::stable::deterministic_i32_random_z1", test_alloctests_sort_tests_stable_deterministic_i32_random_z1},
	{"sort::tests::stable::deterministic_i32_saw_mixed", test_alloctests_sort_tests_stable_deterministic_i32_saw_mixed},
	{"sort::tests::stable::deterministic_string_ascending", test_alloctests_sort_tests_stable_deterministic_string_ascending},
	{"sort::tests::stable::deterministic_string_descending", test_alloctests_sort_tests_stable_deterministic_string_descending},
	{"sort::tests::stable::deterministic_string_random", test_alloctests_sort_tests_stable_deterministic_string_random},
	{"sort::tests::stable::deterministic_string_random_d2", test_alloctests_sort_tests_stable_deterministic_string_random_d2},
	{"sort::tests::stable::deterministic_string_random_d20", test_alloctests_sort_tests_stable_deterministic_string_random_d20},
	{"sort::tests::stable::deterministic_string_random_s95", test_alloctests_sort_tests_stable_deterministic_string_random_s95},
	{"sort::tests::stable::deterministic_string_random_z1", test_alloctests_sort_tests_stable_deterministic_string_random_z1},
	{"sort::tests::stable::deterministic_string_saw_mixed", test_alloctests_sort_tests_stable_deterministic_string_saw_mixed},
	{"sort::tests::stable::fixed_seed", test_alloctests_sort_tests_stable_fixed_seed},
	{"sort::tests::stable::fixed_seed_rand_vec_prefix", test_alloctests_sort_tests_stable_fixed_seed_rand_vec_prefix},
	{"sort::tests::stable::int_edge", test_alloctests_sort_tests_stable_int_edge},
	{"sort::tests::stable::observable_is_less_ascending", test_alloctests_sort_tests_stable_observable_is_less_ascending},
	{"sort::tests::stable::observable_is_less_descending", test_alloctests_sort_tests_stable_observable_is_less_descending},
	{"sort::tests::stable::observable_is_less_random", test_alloctests_sort_tests_stable_observable_is_less_random},
	{"sort::tests::stable::observable_is_less_random_d2", test_alloctests_sort_tests_stable_observable_is_less_random_d2},
	{"sort::tests::stable::observable_is_less_random_d20", test_alloctests_sort_tests_stable_observable_is_less_random_d20},
	{"sort::tests::stable::observable_is_less_random_s95", test_alloctests_sort_tests_stable_observable_is_less_random_s95},
	{"sort::tests::stable::observable_is_less_random_z1", test_alloctests_sort_tests_stable_observable_is_less_random_z1},
	{"sort::tests::stable::observable_is_less_saw_mixed", test_alloctests_sort_tests_stable_observable_is_less_saw_mixed},
	{"sort::tests::stable::panic_observable_is_less_ascending", test_alloctests_sort_tests_stable_panic_observable_is_less_ascending},
	{"sort::tests::stable::panic_observable_is_less_descending", test_alloctests_sort_tests_stable_panic_observable_is_less_descending},
	{"sort::tests::stable::panic_observable_is_less_random", test_alloctests_sort_tests_stable_panic_observable_is_less_random},
	{"sort::tests::stable::panic_observable_is_less_random_d2", test_alloctests_sort_tests_stable_panic_observable_is_less_random_d2},
	{"sort::tests::stable::panic_observable_is_less_random_d20", test_alloctests_sort_tests_stable_panic_observable_is_less_random_d20},
	{"sort::tests::stable::panic_observable_is_less_random_s95", test_alloctests_sort_tests_stable_panic_observable_is_less_random_s95},
	{"sort::tests::stable::panic_observable_is_less_random_z1", test_alloctests_sort_tests_stable_panic_observable_is_less_random_z1},
	{"sort::tests::stable::panic_observable_is_less_saw_mixed", test_alloctests_sort_tests_stable_panic_observable_is_less_saw_mixed},
	{"sort::tests::stable::panic_retain_orig_set_cell_i32_ascending", test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_ascending},
	{"sort::tests::stable::panic_retain_orig_set_cell_i32_descending", test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_descending},
	{"sort::tests::stable::panic_retain_orig_set_cell_i32_random", test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random},
	{"sort::tests::stable::panic_retain_orig_set_cell_i32_random_d2", test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random_d2},
	{"sort::tests::stable::panic_retain_orig_set_cell_i32_random_d20", test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random_d20},
	{"sort::tests::stable::panic_retain_orig_set_cell_i32_random_s95", test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random_s95},
	{"sort::tests::stable::panic_retain_orig_set_cell_i32_random_z1", test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random_z1},
	{"sort::tests::stable::panic_retain_orig_set_cell_i32_saw_mixed", test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_saw_mixed},
	{"sort::tests::stable::panic_retain_orig_set_i32_ascending", test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_ascending},
	{"sort::tests::stable::panic_retain_orig_set_i32_descending", test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_descending},
	{"sort::tests::stable::panic_retain_orig_set_i32_random", test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random},
	{"sort::tests::stable::panic_retain_orig_set_i32_random_d2", test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random_d2},
	{"sort::tests::stable::panic_retain_orig_set_i32_random_d20", test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random_d20},
	{"sort::tests::stable::panic_retain_orig_set_i32_random_s95", test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random_s95},
	{"sort::tests::stable::panic_retain_orig_set_i32_random_z1", test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random_z1},
	{"sort::tests::stable::panic_retain_orig_set_i32_saw_mixed", test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_saw_mixed},
	{"sort::tests::stable::panic_retain_orig_set_string_ascending", test_alloctests_sort_tests_stable_panic_retain_orig_set_string_ascending},
	{"sort::tests::stable::panic_retain_orig_set_string_descending", test_alloctests_sort_tests_stable_panic_retain_orig_set_string_descending},
	{"sort::tests::stable::panic_retain_orig_set_string_random", test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random},
	{"sort::tests::stable::panic_retain_orig_set_string_random_d2", test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random_d2},
	{"sort::tests::stable::panic_retain_orig_set_string_random_d20", test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random_d20},
	{"sort::tests::stable::panic_retain_orig_set_string_random_s95", test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random_s95},
	{"sort::tests::stable::panic_retain_orig_set_string_random_z1", test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random_z1},
	{"sort::tests::stable::panic_retain_orig_set_string_saw_mixed", test_alloctests_sort_tests_stable_panic_retain_orig_set_string_saw_mixed},
	{"sort::tests::stable::self_cmp_cell_i32_ascending", test_alloctests_sort_tests_stable_self_cmp_cell_i32_ascending},
	{"sort::tests::stable::self_cmp_cell_i32_descending", test_alloctests_sort_tests_stable_self_cmp_cell_i32_descending},
	{"sort::tests::stable::self_cmp_cell_i32_random", test_alloctests_sort_tests_stable_self_cmp_cell_i32_random},
	{"sort::tests::stable::self_cmp_cell_i32_random_d2", test_alloctests_sort_tests_stable_self_cmp_cell_i32_random_d2},
	{"sort::tests::stable::self_cmp_cell_i32_random_d20", test_alloctests_sort_tests_stable_self_cmp_cell_i32_random_d20},
	{"sort::tests::stable::self_cmp_cell_i32_random_s95", test_alloctests_sort_tests_stable_self_cmp_cell_i32_random_s95},
	{"sort::tests::stable::self_cmp_cell_i32_random_z1", test_alloctests_sort_tests_stable_self_cmp_cell_i32_random_z1},
	{"sort::tests::stable::self_cmp_cell_i32_saw_mixed", test_alloctests_sort_tests_stable_self_cmp_cell_i32_saw_mixed},
	{"sort::tests::stable::self_cmp_i32_ascending", test_alloctests_sort_tests_stable_self_cmp_i32_ascending},
	{"sort::tests::stable::self_cmp_i32_descending", test_alloctests_sort_tests_stable_self_cmp_i32_descending},
	{"sort::tests::stable::self_cmp_i32_random", test_alloctests_sort_tests_stable_self_cmp_i32_random},
	{"sort::tests::stable::self_cmp_i32_random_d2", test_alloctests_sort_tests_stable_self_cmp_i32_random_d2},
	{"sort::tests::stable::self_cmp_i32_random_d20", test_alloctests_sort_tests_stable_self_cmp_i32_random_d20},
	{"sort::tests::stable::self_cmp_i32_random_s95", test_alloctests_sort_tests_stable_self_cmp_i32_random_s95},
	{"sort::tests::stable::self_cmp_i32_random_z1", test_alloctests_sort_tests_stable_self_cmp_i32_random_z1},
	{"sort::tests::stable::self_cmp_i32_saw_mixed", test_alloctests_sort_tests_stable_self_cmp_i32_saw_mixed},
	{"sort::tests::stable::self_cmp_string_ascending", test_alloctests_sort_tests_stable_self_cmp_string_ascending},
	{"sort::tests::stable::self_cmp_string_descending", test_alloctests_sort_tests_stable_self_cmp_string_descending},
	{"sort::tests::stable::self_cmp_string_random", test_alloctests_sort_tests_stable_self_cmp_string_random},
	{"sort::tests::stable::self_cmp_string_random_d2", test_alloctests_sort_tests_stable_self_cmp_string_random_d2},
	{"sort::tests::stable::self_cmp_string_random_d20", test_alloctests_sort_tests_stable_self_cmp_string_random_d20},
	{"sort::tests::stable::self_cmp_string_random_s95", test_alloctests_sort_tests_stable_self_cmp_string_random_s95},
	{"sort::tests::stable::self_cmp_string_random_z1", test_alloctests_sort_tests_stable_self_cmp_string_random_z1},
	{"sort::tests::stable::self_cmp_string_saw_mixed", test_alloctests_sort_tests_stable_self_cmp_string_saw_mixed},
	{"sort::tests::stable::sort_vs_sort_by", test_alloctests_sort_tests_stable_sort_vs_sort_by},
	{"sort::tests::stable::stability_cell_i32_ascending", test_alloctests_sort_tests_stable_stability_cell_i32_ascending},
	{"sort::tests::stable::stability_cell_i32_descending", test_alloctests_sort_tests_stable_stability_cell_i32_descending},
	{"sort::tests::stable::stability_cell_i32_random", test_alloctests_sort_tests_stable_stability_cell_i32_random},
	{"sort::tests::stable::stability_cell_i32_random_d2", test_alloctests_sort_tests_stable_stability_cell_i32_random_d2},
	{"sort::tests::stable::stability_cell_i32_random_d20", test_alloctests_sort_tests_stable_stability_cell_i32_random_d20},
	{"sort::tests::stable::stability_cell_i32_random_s95", test_alloctests_sort_tests_stable_stability_cell_i32_random_s95},
	{"sort::tests::stable::stability_cell_i32_random_z1", test_alloctests_sort_tests_stable_stability_cell_i32_random_z1},
	{"sort::tests::stable::stability_cell_i32_saw_mixed", test_alloctests_sort_tests_stable_stability_cell_i32_saw_mixed},
	{"sort::tests::stable::stability_i32_ascending", test_alloctests_sort_tests_stable_stability_i32_ascending},
	{"sort::tests::stable::stability_i32_descending", test_alloctests_sort_tests_stable_stability_i32_descending},
	{"sort::tests::stable::stability_i32_random", test_alloctests_sort_tests_stable_stability_i32_random},
	{"sort::tests::stable::stability_i32_random_d2", test_alloctests_sort_tests_stable_stability_i32_random_d2},
	{"sort::tests::stable::stability_i32_random_d20", test_alloctests_sort_tests_stable_stability_i32_random_d20},
	{"sort::tests::stable::stability_i32_random_s95", test_alloctests_sort_tests_stable_stability_i32_random_s95},
	{"sort::tests::stable::stability_i32_random_z1", test_alloctests_sort_tests_stable_stability_i32_random_z1},
	{"sort::tests::stable::stability_i32_saw_mixed", test_alloctests_sort_tests_stable_stability_i32_saw_mixed},
	{"sort::tests::stable::stability_legacy", test_alloctests_sort_tests_stable_stability_legacy},
	{"sort::tests::stable::stability_string_ascending", test_alloctests_sort_tests_stable_stability_string_ascending},
	{"sort::tests::stable::stability_string_descending", test_alloctests_sort_tests_stable_stability_string_descending},
	{"sort::tests::stable::stability_string_random", test_alloctests_sort_tests_stable_stability_string_random},
	{"sort::tests::stable::stability_string_random_d2", test_alloctests_sort_tests_stable_stability_string_random_d2},
	{"sort::tests::stable::stability_string_random_d20", test_alloctests_sort_tests_stable_stability_string_random_d20},
	{"sort::tests::stable::stability_string_random_s95", test_alloctests_sort_tests_stable_stability_string_random_s95},
	{"sort::tests::stable::stability_string_random_z1", test_alloctests_sort_tests_stable_stability_string_random_z1},
	{"sort::tests::stable::stability_string_saw_mixed", test_alloctests_sort_tests_stable_stability_string_saw_mixed},
	{"sort::tests::stable::violate_ord_retain_orig_set_cell_i32_ascending", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_ascending},
	{"sort::tests::stable::violate_ord_retain_orig_set_cell_i32_descending", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_descending},
	{"sort::tests::stable::violate_ord_retain_orig_set_cell_i32_random", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random},
	{"sort::tests::stable::violate_ord_retain_orig_set_cell_i32_random_d2", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random_d2},
	{"sort::tests::stable::violate_ord_retain_orig_set_cell_i32_random_d20", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random_d20},
	{"sort::tests::stable::violate_ord_retain_orig_set_cell_i32_random_s95", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random_s95},
	{"sort::tests::stable::violate_ord_retain_orig_set_cell_i32_random_z1", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random_z1},
	{"sort::tests::stable::violate_ord_retain_orig_set_cell_i32_saw_mixed", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_saw_mixed},
	{"sort::tests::stable::violate_ord_retain_orig_set_i32_ascending", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_ascending},
	{"sort::tests::stable::violate_ord_retain_orig_set_i32_descending", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_descending},
	{"sort::tests::stable::violate_ord_retain_orig_set_i32_random", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random},
	{"sort::tests::stable::violate_ord_retain_orig_set_i32_random_d2", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random_d2},
	{"sort::tests::stable::violate_ord_retain_orig_set_i32_random_d20", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random_d20},
	{"sort::tests::stable::violate_ord_retain_orig_set_i32_random_s95", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random_s95},
	{"sort::tests::stable::violate_ord_retain_orig_set_i32_random_z1", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random_z1},
	{"sort::tests::stable::violate_ord_retain_orig_set_i32_saw_mixed", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_saw_mixed},
	{"sort::tests::stable::violate_ord_retain_orig_set_string_ascending", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_ascending},
	{"sort::tests::stable::violate_ord_retain_orig_set_string_descending", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_descending},
	{"sort::tests::stable::violate_ord_retain_orig_set_string_random", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random},
	{"sort::tests::stable::violate_ord_retain_orig_set_string_random_d2", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random_d2},
	{"sort::tests::stable::violate_ord_retain_orig_set_string_random_d20", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random_d20},
	{"sort::tests::stable::violate_ord_retain_orig_set_string_random_s95", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random_s95},
	{"sort::tests::stable::violate_ord_retain_orig_set_string_random_z1", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random_z1},
	{"sort::tests::stable::violate_ord_retain_orig_set_string_saw_mixed", test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_saw_mixed},
	{"str::slice_index::boundary::range_1::index_fail", test_alloctests_str_slice_index_boundary_range_1_index_fail, true, "start byte index 4 is not a char boundary; it is inside '\xce\xb1' (bytes 3..5 of string)"},
	{"str::slice_index::boundary::range_2::index_fail", test_alloctests_str_slice_index_boundary_range_2_index_fail, true, "end byte index 6 is not a char boundary; it is inside '\xce\xb2' (bytes 5..7 of string)"},
	{"str::slice_index::boundary::rangefrom::index_fail", test_alloctests_str_slice_index_boundary_rangefrom_index_fail, true, "start byte index 4 is not a char boundary; it is inside '\xce\xb1' (bytes 3..5 of string)"},
	{"str::slice_index::boundary::rangeto::index_fail", test_alloctests_str_slice_index_boundary_rangeto_index_fail, true, "end byte index 6 is not a char boundary; it is inside '\xce\xb2' (bytes 5..7 of string)"},
	{"str::slice_index::range_len_len::index_fail", test_alloctests_str_slice_index_range_len_len_index_fail, true, "out of bounds"},
	{"str::slice_index::range_neg_width::index_fail", test_alloctests_str_slice_index_range_neg_width_index_fail, true, "byte range starts at 4 but ends at 3"},
	{"str::slice_index::rangefrom_len::index_fail", test_alloctests_str_slice_index_rangefrom_len_index_fail, true, "out of bounds"},
	{"str::slice_index::rangeto_len::index_fail", test_alloctests_str_slice_index_rangeto_len_index_fail, true, "out of bounds"},
	{"str::utf8_char_counts", test_alloctests_str_utf8_char_counts},
	{"time::tests::duration_debug_matches_rust", test_std_time_tests_duration_debug_matches_rust},
};

/* doltgres_cport_test_std_print prints a literal line, as println! does. */
static void
doltgres_cport_test_std_print(const char *line, size_t len)
{
	StdFmtPiece pieces[2] = {{line, len}, STD_FMT_LIT("\n")};
	StdFmtArguments arguments = {pieces, lengthof(pieces), NULL};

	std_io__print(&arguments);
}

/* doltgres_cport_test_std_prints_then_fails prints two lines and panics: a failing test's case. */
static void
doltgres_cport_test_std_prints_then_fails(void)
{
	doltgres_cport_test_std_print("line one", 8);
	doltgres_cport_test_std_print("line two", 8);
	std_panic(__FILE__, __LINE__, 0, "failed", 6);
}

/* doltgres_cport_test_std_prints_then_passes prints a line and returns: a passing test's case. */
static void
doltgres_cport_test_std_prints_then_passes(void)
{
	doltgres_cport_test_std_print("unseen", 6);
}

/* doltgres_cport_test_std_panics_boom panics with "boom": a #[should_panic] test's case. */
static void
doltgres_cport_test_std_panics_boom(void)
{
	std_panic(__FILE__, __LINE__, 0, "boom", 4);
}

/* doltgres_cport_test_std_panics_out_of_memory unwinds with a payload that isn't a string: a #[should_panic] test's case. */
static void
doltgres_cport_test_std_panics_out_of_memory(void)
{
	std_resume_unwind(STD_PAYLOAD_OUT_OF_MEMORY, NULL);
}

/*
 * CPORT_RUNNER_CASES are the runner's cases: a test's captured output, one
 * failing and one passing, and #[should_panic] tests that pass on any panic
 * (a payload that isn't a string included) and on a panic with the expected
 * text, and fail on another text, on a payload that isn't a string when they
 * expect a text, and on no panic.
 */
static const CportTest CPORT_RUNNER_CASES[] = {
	{"prints_then_fails", doltgres_cport_test_std_prints_then_fails},
	{"prints_then_passes", doltgres_cport_test_std_prints_then_passes},
	{"should_panic_any", doltgres_cport_test_std_panics_boom, true},
	{"should_panic_expected", doltgres_cport_test_std_panics_boom, true, "oo"},
	{"should_panic_other", doltgres_cport_test_std_panics_boom, true, "bang"},
	{"should_panic_none", doltgres_cport_test_std_prints_then_passes, true, "boom"},
	{"should_panic_any_payload", doltgres_cport_test_std_panics_out_of_memory, true},
	{"should_panic_expected_payload", doltgres_cport_test_std_panics_out_of_memory, true, "boom"},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_std);

/* doltgres_cport_test_std runs the std forms' tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_std(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}

PG_FUNCTION_INFO_V1(doltgres_cport_test_std_runner);

/* doltgres_cport_test_std_runner runs the runner's cases and returns each one's name and outcome. */
Datum
doltgres_cport_test_std_runner(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_RUNNER_CASES, lengthof(CPORT_RUNNER_CASES));
}
