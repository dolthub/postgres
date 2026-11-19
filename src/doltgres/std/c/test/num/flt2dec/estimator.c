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
 * The C port of coretests' num/flt2dec/estimator.rs at Rust 1.99.1 (the port
 * text's section 13): estimate_scaling_factor at small, large, and extreme
 * values and at every power of two of an f64.
 */
#include "postgres.h"

#include <math.h>

#include "doltgres/std/fmt.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_flt2dec.h"
#include "doltgres/std/unwind.h"

/* RUST_FILE is coretests' num/flt2dec/estimator.rs. */
#define RUST_FILE STD_RUST_LIBRARY STD_SEP "coretests" STD_SEP "tests" STD_SEP "num" STD_SEP "flt2dec" STD_SEP "estimator.rs"

/*
 * coretests_num_flt2dec_estimator_assert_almost_eq is assert_almost_eq!:
 * the two values printed with their text, then expected asserted to be
 * actual or one more, at the macro's line.
 */
static void
coretests_num_flt2dec_estimator_assert_almost_eq(int16 actual, int16 expected, const char *actual_text,
												 const char *expected_text, uint32 line)
{
	static const StdFmtPiece print_pieces[] = {
		STD_FMT_ARG(0), STD_FMT_LIT(" - "), STD_FMT_ARG(1), STD_FMT_LIT(" = "), STD_FMT_ARG(2), STD_FMT_LIT(" - "),
		STD_FMT_ARG(3), STD_FMT_LIT(" = "), STD_FMT_ARG(4), STD_FMT_LIT("\n"),
	};
	StdStr		expected_str = {expected_text, strlen(expected_text)};
	StdStr		actual_str = {actual_text, strlen(actual_text)};
	int16		difference = (int16) (expected - actual);
	StdFmtArg	print_args[5] = {
		{&expected_str, std_str_Display_fmt}, {&actual_str, std_str_Display_fmt}, {&expected, std_i16_Display_fmt},
		{&actual, std_i16_Display_fmt}, {&difference, std_i16_Display_fmt},
	};
	StdFmtArguments print_arguments = {print_pieces, lengthof(print_pieces), print_args};

	std_io__print(&print_arguments);
	if (!(expected == actual || expected == actual + 1))
	{
		static const StdFmtPiece pieces[] = {
			STD_FMT_LIT("expected "), STD_FMT_ARG(0), STD_FMT_LIT(", actual "), STD_FMT_ARG(1),
		};
		StdFmtArg	args[2] = {{&expected, std_i16_Display_fmt}, {&actual, std_i16_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, line, 5, &arguments);
	}
}

/*
 * test_coretests_num_flt2dec_estimator_test_estimate_scaling_factor:
 * estimate_scaling_factor is k or k - 1 for 10^(k-1) < mant * 2^exp <= 10^k.
 */
void
test_coretests_num_flt2dec_estimator_test_estimate_scaling_factor(void)
{
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(1, 0), 0,
													 "estimate_scaling_factor(1, 0)", "0", 28);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(2, 0), 1,
													 "estimate_scaling_factor(2, 0)", "1", 29);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(10, 0), 1,
													 "estimate_scaling_factor(10, 0)", "1", 30);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(11, 0), 2,
													 "estimate_scaling_factor(11, 0)", "2", 31);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(100, 0), 2,
													 "estimate_scaling_factor(100, 0)", "2", 32);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(101, 0), 3,
													 "estimate_scaling_factor(101, 0)", "3", 33);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(UINT64CONST(10000000000000000000), 0),
													 19, "estimate_scaling_factor(10000000000000000000, 0)", "19", 34);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(UINT64CONST(10000000000000000001), 0),
													 20, "estimate_scaling_factor(10000000000000000001, 0)", "20", 35);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(1 * 1048576 / 1000000, -20),
													 -6, "estimate_scaling_factor(1 * 1048576 / 1000000, -20)", "-6",
													 38);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(1 * 1048576 / 1000000 + 1, -20),
													 -5, "estimate_scaling_factor(1 * 1048576 / 1000000 + 1, -20)",
													 "-5", 39);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(10 * 1048576 / 1000000, -20),
													 -5, "estimate_scaling_factor(10 * 1048576 / 1000000, -20)", "-5",
													 40);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(10 * 1048576 / 1000000 + 1, -20),
													 -4, "estimate_scaling_factor(10 * 1048576 / 1000000 + 1, -20)",
													 "-4", 41);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(100 * 1048576 / 1000000, -20),
													 -4, "estimate_scaling_factor(100 * 1048576 / 1000000, -20)", "-4",
													 42);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(100 * 1048576 / 1000000 + 1, -20),
													 -3, "estimate_scaling_factor(100 * 1048576 / 1000000 + 1, -20)",
													 "-3", 43);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(1048575, -20), 0,
													 "estimate_scaling_factor(1048575, -20)", "0", 44);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(1048576, -20), 0,
													 "estimate_scaling_factor(1048576, -20)", "0", 45);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(1048577, -20), 1,
													 "estimate_scaling_factor(1048577, -20)", "1", 46);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(UINT64CONST(10485759999999999999), -20),
													 13, "estimate_scaling_factor(10485759999999999999, -20)", "13",
													 47);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(UINT64CONST(10485760000000000000), -20),
													 13, "estimate_scaling_factor(10485760000000000000, -20)", "13",
													 48);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(UINT64CONST(10485760000000000001), -20),
													 14, "estimate_scaling_factor(10485760000000000001, -20)", "14",
													 49);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(1, -1074), -323,
													 "estimate_scaling_factor(1, -1074)", "-323", 54);
	coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(UINT64CONST(0x1fffffffffffff), 971),
													 309, "estimate_scaling_factor(0x1fffffffffffff, 971)", "309", 55);
	for (int32 i = -1074; i < 972; i++)
	{
		float8		expected = ceil(log10(coretests_num_ldexp_f64(1.0, i)));

		coretests_num_flt2dec_estimator_assert_almost_eq(std_flt2dec_estimator_estimate_scaling_factor(1, (int16) i),
														 (int16) expected, "estimate_scaling_factor(1, i as i16)",
														 "expected as i16", 62);
	}
}
