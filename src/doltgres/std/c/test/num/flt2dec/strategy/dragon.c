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
 * The C port of coretests' num/flt2dec/strategy/dragon.rs at Rust 1.99.1
 * (the port text's section 13): mul_pow10 against repeated multiplication,
 * and the shared sanity and to_*_str tests through Dragon's strategies.
 */
#include "postgres.h"

#include "doltgres/std/fmt.h"
#include "doltgres/std/test_flt2dec.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* RUST_FILE is coretests' num/flt2dec/strategy/dragon.rs. */
#define RUST_FILE STD_RUST_LIBRARY STD_SEP "coretests" STD_SEP "tests" STD_SEP "num" STD_SEP "flt2dec" STD_SEP "strategy" STD_SEP "dragon.rs"

/* BIGNUM_FILE is core's num/imp/bignum.rs, whose Debug's checks report its macro's invocation. */
#define BIGNUM_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "num" STD_SEP "imp" STD_SEP "bignum.rs"

/* std_bignum_Big32x40_PartialEq_eq is Big32x40's PartialEq: every digit equal, in use or not. */
static bool
std_bignum_Big32x40_PartialEq_eq(const StdBignumBig32x40 *self, const StdBignumBig32x40 *other)
{
	return memcmp(self->base, other->base, sizeof(self->base)) == 0;
}

/* std_bignum_Big32x40_Debug_fmt is Big32x40's Debug: its top digit in {:#x}, then each lower one as _ and 8 hex digits. */
static bool
std_bignum_Big32x40_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdBignumBig32x40 *self = (const StdBignumBig32x40 *) value;
	size_t		sz = self->size < 1 ? 1 : self->size;
	uint16		digitlen = 32 / 4;
	StdFmtPiece top[1] = {{NULL, 0, 0, {.fill = ' ', .align = STD_FMT_ALIGN_UNKNOWN, .alternate = true}}};
	StdFmtArg	top_args[1];
	StdFmtArguments top_arguments = {top, lengthof(top), top_args};

	std_index_check(sz - 1, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
	top_args[0].value = &self->base[sz - 1];
	top_args[0].fmt = std_u32_LowerHex_fmt;
	if (!std_fmt_write_fmt_Checked(f, &top_arguments))
		return false;
	std_range_to_check(sz - 1, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
	for (size_t i = sz - 1; i > 0; i--)
	{
		StdFmtPiece rest[2] = {
			STD_FMT_LIT("_"),
			{NULL, 0, 0, {.fill = ' ', .align = STD_FMT_ALIGN_UNKNOWN, .sign_aware_zero_pad = true,
			.has_width = true, .width = digitlen}},
		};
		StdFmtArg	rest_args[1] = {{&self->base[i - 1], std_u32_LowerHex_fmt}};
		StdFmtArguments rest_arguments = {rest, lengthof(rest), rest_args};

		if (!std_fmt_write_fmt_Checked(f, &rest_arguments))
			return false;
	}
	return true;
}

/* std_flt2dec_strategy_dragon_format_shortest_FnMut_call_mut is dragon::format_shortest called as an FnMut, its shim. */
static size_t
std_flt2dec_strategy_dragon_format_shortest_FnMut_call_mut(void *captures, const StdDecoded *d, uint8 *buf,
														   size_t buf_len, int16 *result_1)
{
	return std_flt2dec_strategy_dragon_format_shortest(d, buf, buf_len, result_1);
}

/* std_flt2dec_strategy_dragon_format_exact_FnMut_call_mut is dragon::format_exact called as an FnMut, its shim. */
static size_t
std_flt2dec_strategy_dragon_format_exact_FnMut_call_mut(void *captures, const StdDecoded *d, uint8 *buf,
														size_t buf_len, int16 limit, int16 *result_1)
{
	return std_flt2dec_strategy_dragon_format_exact(d, buf, buf_len, limit, result_1);
}

/* test_coretests_num_flt2dec_strategy_dragon_test_mul_pow10: mul_pow10 by i is the power before it times ten. */
void
test_coretests_num_flt2dec_strategy_dragon_test_mul_pow10(void)
{
	StdBignumBig32x40 prevpow10 = std_bignum_Big32x40_from_small(1);

	for (size_t i = 1; i < 340; i++)
	{
		StdBignumBig32x40 curpow10 = std_bignum_Big32x40_from_small(1);
		StdBignumBig32x40 tmp_1;

		std_flt2dec_strategy_dragon_mul_pow10(&curpow10, i);
		tmp_1 = prevpow10;
		std_bignum_Big32x40_mul_small(&tmp_1, 10);
		if (!std_bignum_Big32x40_PartialEq_eq(&curpow10, &tmp_1))
			std_assert_failed(RUST_FILE, 12, 9, "==", &curpow10, std_bignum_Big32x40_Debug_fmt, &tmp_1,
							  std_bignum_Big32x40_Debug_fmt, NULL);
		prevpow10 = curpow10;
	}
}

/* test_coretests_num_flt2dec_strategy_dragon_shortest_sanity_test: Dragon's shortest digits of the sanity values. */
void
test_coretests_num_flt2dec_strategy_dragon_shortest_sanity_test(void)
{
	coretests_num_flt2dec_f64_shortest_sanity_test(std_flt2dec_strategy_dragon_format_shortest_FnMut_call_mut, NULL,
												   NULL);
	coretests_num_flt2dec_f32_shortest_sanity_test(std_flt2dec_strategy_dragon_format_shortest_FnMut_call_mut, NULL,
												   NULL);
	coretests_num_flt2dec_f16_shortest_sanity_test(std_flt2dec_strategy_dragon_format_shortest_FnMut_call_mut, NULL,
												   NULL);
	coretests_num_flt2dec_more_shortest_sanity_test(std_flt2dec_strategy_dragon_format_shortest_FnMut_call_mut, NULL,
													NULL);
}

/*
 * test_coretests_num_flt2dec_strategy_dragon_exact_sanity_test: Dragon's
 * exact digits of the sanity values, f64's left out under MSVC as cargo
 * leaves them out there.
 */
void
test_coretests_num_flt2dec_strategy_dragon_exact_sanity_test(void)
{
#ifndef _MSC_VER
	coretests_num_flt2dec_f64_exact_sanity_test(std_flt2dec_strategy_dragon_format_exact_FnMut_call_mut, NULL, NULL);
#endif
	coretests_num_flt2dec_f32_exact_sanity_test(std_flt2dec_strategy_dragon_format_exact_FnMut_call_mut, NULL, NULL);
	coretests_num_flt2dec_f16_exact_sanity_test(std_flt2dec_strategy_dragon_format_exact_FnMut_call_mut, NULL, NULL);
}

/* test_coretests_num_flt2dec_strategy_dragon_test_to_shortest_str: to_shortest_str through Dragon. */
void
test_coretests_num_flt2dec_strategy_dragon_test_to_shortest_str(void)
{
	coretests_num_flt2dec_to_shortest_str_test(std_flt2dec_strategy_dragon_format_shortest_FnMut_call_mut, NULL, NULL);
}

/* test_coretests_num_flt2dec_strategy_dragon_test_to_shortest_exp_str: to_shortest_exp_str through Dragon. */
void
test_coretests_num_flt2dec_strategy_dragon_test_to_shortest_exp_str(void)
{
	coretests_num_flt2dec_to_shortest_exp_str_test(std_flt2dec_strategy_dragon_format_shortest_FnMut_call_mut, NULL,
												   NULL);
}

/* test_coretests_num_flt2dec_strategy_dragon_test_to_exact_exp_str: to_exact_exp_str through Dragon. */
void
test_coretests_num_flt2dec_strategy_dragon_test_to_exact_exp_str(void)
{
	coretests_num_flt2dec_to_exact_exp_str_test(std_flt2dec_strategy_dragon_format_exact_FnMut_call_mut, NULL, NULL);
}

/* test_coretests_num_flt2dec_strategy_dragon_test_to_exact_fixed_str: to_exact_fixed_str through Dragon. */
void
test_coretests_num_flt2dec_strategy_dragon_test_to_exact_fixed_str(void)
{
	coretests_num_flt2dec_to_exact_fixed_str_test(std_flt2dec_strategy_dragon_format_exact_FnMut_call_mut, NULL, NULL);
}
