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
 * The C port of coretests' num/flt2dec/strategy/grisu.rs at Rust 1.99.1 (the
 * port text's section 13): the cached powers' range, max_pow10_no_more_than,
 * and the shared sanity and to_*_str tests through Grisu's strategies.
 */
#include "postgres.h"

#include "doltgres/std/fmt.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_flt2dec.h"
#include "doltgres/std/unwind.h"

/* RUST_FILE is coretests' num/flt2dec/strategy/grisu.rs. */
#define RUST_FILE STD_RUST_LIBRARY STD_SEP "coretests" STD_SEP "tests" STD_SEP "num" STD_SEP "flt2dec" STD_SEP "strategy" STD_SEP "grisu.rs"

/* std_diy_float_Fp_Debug_fmt is Fp's derived Debug. */
static bool
std_diy_float_Fp_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdFp *self = (const StdFp *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Fp", 2);

	std_DebugStruct_field(&b, "f", 1, &self->f, std_u64_Debug_fmt);
	std_DebugStruct_field(&b, "e", 1, &self->e, std_i16_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* test_coretests_num_flt2dec_strategy_grisu_test_cached_power: every f64 exponent's cached power is in its range. */
void
test_coretests_num_flt2dec_strategy_grisu_test_cached_power(void)
{
	int16		first_e = STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_FIRST_E;
	int16		last_e = STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_LAST_E;

	STD_ASSERT_EQ(8, 5, STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10[0].f1 == first_e,
				  &STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10[0].f1, std_i16_Debug_fmt, &first_e, std_i16_Debug_fmt);
	STD_ASSERT_EQ(9, 5, STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10[STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_LEN - 1].f1 ==
				  last_e, &STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10[STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_LEN - 1].f1,
				  std_i16_Debug_fmt, &last_e, std_i16_Debug_fmt);
	for (int16 e = -1137; e < 961; e++)
	{
		int16		low = (int16) (STD_FLT2DEC_STRATEGY_GRISU_ALPHA - e - 64);
		int16		high = (int16) (STD_FLT2DEC_STRATEGY_GRISU_GAMMA - e - 64);
		StdFp		cached;

		std_flt2dec_strategy_grisu_cached_power(low, high, &cached);
		if (!(low <= cached.e && cached.e <= high))
		{
			static const StdFmtPiece pieces[] = {
				STD_FMT_LIT("cached_power("), STD_FMT_ARG(0), STD_FMT_LIT(", "), STD_FMT_ARG(1), STD_FMT_LIT(") = "),
				STD_FMT_ARG(2), STD_FMT_LIT(" is incorrect"),
			};
			StdFmtArg	args[3] = {
				{&low, std_i16_Display_fmt}, {&high, std_i16_Display_fmt}, {&cached, std_diy_float_Fp_Debug_fmt},
			};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 16, 9, &arguments);
		}
	}
}

/*
 * test_coretests_num_flt2dec_strategy_grisu_test_max_pow10_no_more_than:
 * each power of ten is its own largest, and one less is the power below's.
 */
void
test_coretests_num_flt2dec_strategy_grisu_test_max_pow10_no_more_than(void)
{
	uint32		prevtenk = 1;

	for (uint8 k = 1; k < 10; k++)
	{
		uint32		tenk = prevtenk * 10;
		uint32		left_1;
		uint8		left_0 = std_flt2dec_strategy_grisu_max_pow10_no_more_than(tenk - 1, &left_1);
		uint8		right_0 = (uint8) (k - 1);

		STD_ASSERT_EQ(31, 9, left_0 == right_0 && left_1 == prevtenk,
					  &STD_TUPLE(2, STD_ITEM(&left_0, std_u8_Debug_fmt), STD_ITEM(&left_1, std_u32_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&right_0, std_u8_Debug_fmt), STD_ITEM(&prevtenk, std_u32_Debug_fmt)),
					  std_tuple_Debug_fmt);
		left_0 = std_flt2dec_strategy_grisu_max_pow10_no_more_than(tenk, &left_1);
		STD_ASSERT_EQ(32, 9, left_0 == k && left_1 == tenk,
					  &STD_TUPLE(2, STD_ITEM(&left_0, std_u8_Debug_fmt), STD_ITEM(&left_1, std_u32_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&k, std_u8_Debug_fmt), STD_ITEM(&tenk, std_u32_Debug_fmt)),
					  std_tuple_Debug_fmt);
		prevtenk = tenk;
	}
}

/* test_coretests_num_flt2dec_strategy_grisu_shortest_sanity_test: Grisu's shortest digits of the sanity values. */
void
test_coretests_num_flt2dec_strategy_grisu_shortest_sanity_test(void)
{
	coretests_num_flt2dec_f64_shortest_sanity_test(std_flt2dec_strategy_grisu_format_shortest_FnMut_call_mut, NULL,
												   NULL);
	coretests_num_flt2dec_f32_shortest_sanity_test(std_flt2dec_strategy_grisu_format_shortest_FnMut_call_mut, NULL,
												   NULL);
	coretests_num_flt2dec_f16_shortest_sanity_test(std_flt2dec_strategy_grisu_format_shortest_FnMut_call_mut, NULL,
												   NULL);
	coretests_num_flt2dec_more_shortest_sanity_test(std_flt2dec_strategy_grisu_format_shortest_FnMut_call_mut, NULL,
													NULL);
}

/*
 * test_coretests_num_flt2dec_strategy_grisu_exact_sanity_test: Grisu's
 * exact digits of the sanity values, f64's left out under MSVC as cargo
 * leaves them out there.
 */
void
test_coretests_num_flt2dec_strategy_grisu_exact_sanity_test(void)
{
#ifndef _MSC_VER
	coretests_num_flt2dec_f64_exact_sanity_test(std_flt2dec_strategy_grisu_format_exact_FnMut_call_mut, NULL, NULL);
#endif
	coretests_num_flt2dec_f32_exact_sanity_test(std_flt2dec_strategy_grisu_format_exact_FnMut_call_mut, NULL, NULL);
	coretests_num_flt2dec_f16_exact_sanity_test(std_flt2dec_strategy_grisu_format_exact_FnMut_call_mut, NULL, NULL);
}

/* test_coretests_num_flt2dec_strategy_grisu_test_to_shortest_str: to_shortest_str through Grisu. */
void
test_coretests_num_flt2dec_strategy_grisu_test_to_shortest_str(void)
{
	coretests_num_flt2dec_to_shortest_str_test(std_flt2dec_strategy_grisu_format_shortest_FnMut_call_mut, NULL, NULL);
}

/* test_coretests_num_flt2dec_strategy_grisu_test_to_shortest_exp_str: to_shortest_exp_str through Grisu. */
void
test_coretests_num_flt2dec_strategy_grisu_test_to_shortest_exp_str(void)
{
	coretests_num_flt2dec_to_shortest_exp_str_test(std_flt2dec_strategy_grisu_format_shortest_FnMut_call_mut, NULL,
												   NULL);
}

/* test_coretests_num_flt2dec_strategy_grisu_test_to_exact_exp_str: to_exact_exp_str through Grisu. */
void
test_coretests_num_flt2dec_strategy_grisu_test_to_exact_exp_str(void)
{
	coretests_num_flt2dec_to_exact_exp_str_test(std_flt2dec_strategy_grisu_format_exact_FnMut_call_mut, NULL, NULL);
}

/* test_coretests_num_flt2dec_strategy_grisu_test_to_exact_fixed_str: to_exact_fixed_str through Grisu. */
void
test_coretests_num_flt2dec_strategy_grisu_test_to_exact_fixed_str(void)
{
	coretests_num_flt2dec_to_exact_fixed_str_test(std_flt2dec_strategy_grisu_format_exact_FnMut_call_mut, NULL, NULL);
}
