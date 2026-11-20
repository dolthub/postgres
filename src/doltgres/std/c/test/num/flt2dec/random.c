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
 * The C port of coretests' num/flt2dec/random.rs at Rust 1.99.1 (the port
 * text's section 13): Grisu's digits against Dragon's over random finite
 * values of each float type and every finite f16, with rand 0.9.5's Uniform
 * integer sampling over test_rng's XorShiftRng.
 */
#include "postgres.h"

#include "doltgres/std/fmt.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_flt2dec.h"
#include "doltgres/std/unwind.h"

/* RUST_FILE is coretests' num/flt2dec/random.rs. */
#define RUST_FILE STD_RUST_LIBRARY STD_SEP "coretests" STD_SEP "tests" STD_SEP "num" STD_SEP "flt2dec" STD_SEP "random.rs"

/*
 * CoretestsFormatExactOptClosure is the FnMut random.rs's iterate takes: an
 * f, which returns None as false, or a g, which can't.
 */
typedef bool (*CoretestsIterateFn) (void *captures, const StdDecoded *d, uint8 *buf, size_t buf_len, size_t *result,
									int16 *result_1);

/* CoretestsIterateValueFn is iterate's v: the i-th Decoded. */
typedef StdDecoded (*CoretestsIterateValueFn) (void *captures, size_t i);

/*
 * coretests_num_flt2dec_random_iterate is iterate: f against g over n
 * values of v with k digits, printing progress and each mismatch, asserting
 * none, and returning how many passed, with how many f ignored in result_1.
 */
static size_t
coretests_num_flt2dec_random_iterate(const char *func, size_t k, size_t n, CoretestsIterateFn f, void *f_captures,
									 CoretestsIterateFn g, void *g_captures, CoretestsIterateValueFn v,
									 void *v_captures, size_t *result_1)
{
	size_t		npassed = 0;
	size_t		nignored = 0;
	StdStr		func_str = {func, strlen(func)};

	if (!(k <= 1024))
		std_panic(RUST_FILE, 24, 5, "assertion failed: k <= 1024", sizeof("assertion failed: k <= 1024") - 1);
	for (size_t i = 0; i < n; i++)
	{
		StdDecoded	decoded;
		uint8		buf1[1024] = {0};
		size_t		len1;
		int16		e1;

		if ((i & 0xfffff) == 0)
		{
			static const StdFmtPiece pieces[] = {
				STD_FMT_LIT("in progress, "), {NULL, 0, 0, {.fill = ' ', .align = STD_FMT_ALIGN_UNKNOWN}},
				STD_FMT_LIT("/"), {NULL, 0, 1, {.fill = ' ', .align = STD_FMT_ALIGN_UNKNOWN}},
				STD_FMT_LIT(" (ignored="), STD_FMT_ARG(2), STD_FMT_LIT(" passed="), STD_FMT_ARG(3),
				STD_FMT_LIT(" failed="), STD_FMT_ARG(4), STD_FMT_LIT(")\n"),
			};
			size_t		failed = i - nignored - npassed;
			StdFmtArg	args[5] = {
				{&i, std_u64_LowerHex_fmt}, {&n, std_u64_LowerHex_fmt}, {&nignored, std_usize_Display_fmt},
				{&npassed, std_usize_Display_fmt}, {&failed, std_usize_Display_fmt},
			};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_io__print(&arguments);
		}
		decoded = v(v_captures, i);
		if (f(f_captures, &decoded, buf1, k, &len1, &e1))
		{
			uint8		buf2[1024] = {0};
			size_t		len2;
			int16		e2;

			g(g_captures, &decoded, buf2, k, &len2, &e2);
			if (e1 == e2 && len1 == len2 && memcmp(buf1, buf2, len1) == 0)
				npassed += 1;
			else
			{
				static const StdFmtPiece pieces[] = {
					STD_FMT_LIT("equivalence test failed, "), {NULL, 0, 0, {.fill = ' ', .align = STD_FMT_ALIGN_UNKNOWN}},
					STD_FMT_LIT("/"), {NULL, 0, 1, {.fill = ' ', .align = STD_FMT_ALIGN_UNKNOWN}},
					STD_FMT_LIT(": "), STD_FMT_ARG(2), STD_FMT_LIT(" f(i)="), STD_FMT_ARG(3), STD_FMT_LIT("e"),
					STD_FMT_ARG(4), STD_FMT_LIT(" g(i)="), STD_FMT_ARG(5), STD_FMT_LIT("e"), STD_FMT_ARG(6),
					STD_FMT_LIT("\n"),
				};
				StdUtf8Error error;
				StdStr		str1 = {(const char *) buf1, len1};
				StdStr		str2 = {(const char *) buf2, len2};
				StdFmtArg	args[7];
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};

				if (!std_str_from_utf8(buf1, len1, &error))
					std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
											 sizeof("called `Result::unwrap()` on an `Err` value") - 1, &error,
											 std_Utf8Error_Debug_fmt, RUST_FILE, 54, 42);
				if (!std_str_from_utf8(buf2, len2, &error))
					std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
											 sizeof("called `Result::unwrap()` on an `Err` value") - 1, &error,
											 std_Utf8Error_Debug_fmt, RUST_FILE, 56, 42);
				args[0] = (StdFmtArg)
				{
					&i, std_u64_LowerHex_fmt
				};
				args[1] = (StdFmtArg)
				{
					&n, std_u64_LowerHex_fmt
				};
				args[2] = (StdFmtArg)
				{
					&decoded, std_flt2dec_decoder_Decoded_Debug_fmt
				};
				args[3] = (StdFmtArg)
				{
					&str1, std_str_Display_fmt
				};
				args[4] = (StdFmtArg)
				{
					&e1, std_i16_Display_fmt
				};
				args[5] = (StdFmtArg)
				{
					&str2, std_str_Display_fmt
				};
				args[6] = (StdFmtArg)
				{
					&e2, std_i16_Display_fmt
				};
				std_io__print(&arguments);
			}
		}
		else
			nignored += 1;
	}
	{
		static const StdFmtPiece pieces[] = {
			STD_FMT_ARG(0), STD_FMT_LIT("("), STD_FMT_ARG(1), STD_FMT_LIT("): done, ignored="), STD_FMT_ARG(2),
			STD_FMT_LIT(" passed="), STD_FMT_ARG(3), STD_FMT_LIT(" failed="), STD_FMT_ARG(4), STD_FMT_LIT("\n"),
		};
		size_t		failed = n - nignored - npassed;
		StdFmtArg	args[5] = {
			{&func_str, std_str_Display_fmt}, {&k, std_usize_Display_fmt}, {&nignored, std_usize_Display_fmt},
			{&npassed, std_usize_Display_fmt}, {&failed, std_usize_Display_fmt},
		};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_io__print(&arguments);
	}
	if (!(nignored + npassed == n))
	{
		static const StdFmtPiece pieces[] = {
			STD_FMT_ARG(0), STD_FMT_LIT("("), STD_FMT_ARG(1), STD_FMT_LIT("): "), STD_FMT_ARG(2),
			STD_FMT_LIT(" out of "), STD_FMT_ARG(3), STD_FMT_LIT(" values returns an incorrect value!"),
		};
		size_t		failed = n - nignored - npassed;
		StdFmtArg	args[4] = {
			{&func_str, std_str_Display_fmt}, {&k, std_usize_Display_fmt}, {&failed, std_usize_Display_fmt},
			{&n, std_usize_Display_fmt},
		};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 72, 5, &arguments);
	}
	*result_1 = nignored;
	return npassed;
}

/* CoretestsRandomValue is a random equivalence test's v closure's captures: its rng and its range. */
typedef struct CoretestsRandomValue
{
	uint32		rng[4];
	CoretestsUniformInt range;
} CoretestsRandomValue;

/* coretests_num_flt2dec_random_f16_value is f16_random_equivalence_test's v: a random finite f16's Decoded. */
static StdDecoded
coretests_num_flt2dec_random_f16_value(void *captures, size_t i)
{
	CoretestsRandomValue *c = (CoretestsRandomValue *) captures;
	uint16		x = (uint16) coretests_rand_UniformInt_sample(&c->range, c->rng, 16, 32);

	return coretests_num_flt2dec_decode_finiteF16(x);
}

/* coretests_num_flt2dec_random_f32_value is f32_random_equivalence_test's v: a random finite f32's Decoded. */
static StdDecoded
coretests_num_flt2dec_random_f32_value(void *captures, size_t i)
{
	CoretestsRandomValue *c = (CoretestsRandomValue *) captures;
	uint32		bits = (uint32) coretests_rand_UniformInt_sample(&c->range, c->rng, 32, 32);
	float4		x;

	memcpy(&x, &bits, sizeof(x));
	return coretests_num_flt2dec_decode_finiteF32(x);
}

/* coretests_num_flt2dec_random_f64_value is f64_random_equivalence_test's v: a random finite f64's Decoded. */
static StdDecoded
coretests_num_flt2dec_random_f64_value(void *captures, size_t i)
{
	CoretestsRandomValue *c = (CoretestsRandomValue *) captures;
	uint64		bits = coretests_rand_UniformInt_sample(&c->range, c->rng, 64, 64);
	float8		x;

	memcpy(&x, &bits, sizeof(x));
	return coretests_num_flt2dec_decode_finiteF64(x);
}

/* coretests_num_flt2dec_random_f16_random_equivalence_test is f16_random_equivalence_test: n random f16s. */
static void
coretests_num_flt2dec_random_f16_random_equivalence_test(CoretestsIterateFn f, void *f_captures, CoretestsIterateFn g,
														 void *g_captures, size_t k, size_t n)
{
	CoretestsRandomValue v;
	size_t		nignored;

	coretests_test_rng(v.rng, RUST_FILE, 89, 19);
	v.range = coretests_rand_UniformInt_new(0x0001, 0x7c00, false, 16, 32, RUST_FILE, 90, 53);
	coretests_num_flt2dec_random_iterate("f16_random_equivalence_test", k, n, f, f_captures, g, g_captures,
										 coretests_num_flt2dec_random_f16_value, &v, &nignored);
}

/* coretests_num_flt2dec_random_f32_random_equivalence_test is f32_random_equivalence_test: n random f32s. */
static void
coretests_num_flt2dec_random_f32_random_equivalence_test(CoretestsIterateFn f, void *f_captures, CoretestsIterateFn g,
														 void *g_captures, size_t k, size_t n)
{
	CoretestsRandomValue v;
	size_t		nignored;

	coretests_test_rng(v.rng, RUST_FILE, 102, 19);
	v.range = coretests_rand_UniformInt_new(0x00000001, 0x7f800000, false, 32, 32, RUST_FILE, 103, 63);
	coretests_num_flt2dec_random_iterate("f32_random_equivalence_test", k, n, f, f_captures, g, g_captures,
										 coretests_num_flt2dec_random_f32_value, &v, &nignored);
}

/* coretests_num_flt2dec_random_f64_random_equivalence_test is f64_random_equivalence_test: n random f64s. */
static void
coretests_num_flt2dec_random_f64_random_equivalence_test(CoretestsIterateFn f, void *f_captures, CoretestsIterateFn g,
														 void *g_captures, size_t k, size_t n)
{
	CoretestsRandomValue v;
	size_t		nignored;

	coretests_test_rng(v.rng, RUST_FILE, 115, 19);
	v.range = coretests_rand_UniformInt_new(UINT64CONST(0x0000000000000001), UINT64CONST(0x7ff0000000000000), false, 64, 64,
											RUST_FILE,
											116, 83);
	coretests_num_flt2dec_random_iterate("f64_random_equivalence_test", k, n, f, f_captures, g, g_captures,
										 coretests_num_flt2dec_random_f64_value, &v, &nignored);
}

/* coretests_num_flt2dec_random_f16_exhaustive_value is f16_exhaustive_equivalence_test's v: the f16 of bits i + 1. */
static StdDecoded
coretests_num_flt2dec_random_f16_exhaustive_value(void *captures, size_t i)
{
	return coretests_num_flt2dec_decode_finiteF16((uint16) ((uint16) i + 1));
}

/* coretests_num_flt2dec_random_f16_exhaustive_equivalence_test is f16_exhaustive_equivalence_test: every finite f16. */
static void
coretests_num_flt2dec_random_f16_exhaustive_equivalence_test(CoretestsIterateFn f, void *f_captures,
															 CoretestsIterateFn g, void *g_captures, size_t k)
{
	size_t		nignored;
	size_t		npassed = coretests_num_flt2dec_random_iterate("f16_exhaustive_equivalence_test", k, 0x7bff, f,
															   f_captures, g, g_captures,
															   coretests_num_flt2dec_random_f16_exhaustive_value, NULL,
															   &nignored);
	size_t		expected_0 = 29735;
	size_t		expected_1 = 2008;

	STD_ASSERT_EQ(138, 5, npassed == expected_0 && nignored == expected_1,
				  &STD_TUPLE(2, STD_ITEM(&npassed, std_usize_Debug_fmt), STD_ITEM(&nignored, std_usize_Debug_fmt)),
				  std_tuple_Debug_fmt,
				  &STD_TUPLE(2, STD_ITEM(&expected_0, std_usize_Debug_fmt), STD_ITEM(&expected_1, std_usize_Debug_fmt)),
				  std_tuple_Debug_fmt);
}

/* coretests_num_flt2dec_random_format_shortest_opt is format_shortest_opt as iterate's f. */
static bool
coretests_num_flt2dec_random_format_shortest_opt(void *captures, const StdDecoded *d, uint8 *buf, size_t buf_len,
												 size_t *result, int16 *result_1)
{
	return std_flt2dec_strategy_grisu_format_shortest_opt(d, buf, buf_len, result, result_1);
}

/* coretests_num_flt2dec_random_dragon_format_shortest is Dragon's format_shortest as iterate's g, which always gives digits. */
static bool
coretests_num_flt2dec_random_dragon_format_shortest(void *captures, const StdDecoded *d, uint8 *buf, size_t buf_len,
													size_t *result, int16 *result_1)
{
	*result = std_flt2dec_strategy_dragon_format_shortest(d, buf, buf_len, result_1);
	return true;
}

/* coretests_num_flt2dec_random_exact_closure_0 is |d, buf| format_exact_opt(d, buf, i16::MIN), iterate's f. */
static bool
coretests_num_flt2dec_random_exact_closure_0(void *captures, const StdDecoded *d, uint8 *buf, size_t buf_len,
											 size_t *result, int16 *result_1)
{
	return std_flt2dec_strategy_grisu_format_exact_opt(d, buf, buf_len, PG_INT16_MIN, result, result_1);
}

/* coretests_num_flt2dec_random_exact_closure_1 is |d, buf| fallback(d, buf, i16::MIN), iterate's g. */
static bool
coretests_num_flt2dec_random_exact_closure_1(void *captures, const StdDecoded *d, uint8 *buf, size_t buf_len,
											 size_t *result, int16 *result_1)
{
	*result = std_flt2dec_strategy_dragon_format_exact(d, buf, buf_len, PG_INT16_MIN, result_1);
	return true;
}

/* test_coretests_num_flt2dec_random_shortest_random_equivalence_test: Grisu's shortest digits are Dragon's for random values. */
void
test_coretests_num_flt2dec_random_shortest_random_equivalence_test(void)
{
	size_t		n = 10000;

	coretests_num_flt2dec_random_f64_random_equivalence_test(coretests_num_flt2dec_random_format_shortest_opt, NULL,
															 coretests_num_flt2dec_random_dragon_format_shortest, NULL,
															 STD_FLT2DEC_MAX_SIG_DIGITS, n);
	coretests_num_flt2dec_random_f32_random_equivalence_test(coretests_num_flt2dec_random_format_shortest_opt, NULL,
															 coretests_num_flt2dec_random_dragon_format_shortest, NULL,
															 STD_FLT2DEC_MAX_SIG_DIGITS, n);
	coretests_num_flt2dec_random_f16_random_equivalence_test(coretests_num_flt2dec_random_format_shortest_opt, NULL,
															 coretests_num_flt2dec_random_dragon_format_shortest, NULL,
															 STD_FLT2DEC_MAX_SIG_DIGITS, n);
}

/* test_coretests_num_flt2dec_random_shortest_f16_exhaustive_equivalence_test: Grisu's shortest digits are Dragon's for every f16. */
void
test_coretests_num_flt2dec_random_shortest_f16_exhaustive_equivalence_test(void)
{
	coretests_num_flt2dec_random_f16_exhaustive_equivalence_test(coretests_num_flt2dec_random_format_shortest_opt, NULL,
																 coretests_num_flt2dec_random_dragon_format_shortest,
																 NULL, STD_FLT2DEC_MAX_SIG_DIGITS);
}

/* test_coretests_num_flt2dec_random_exact_f16_random_equivalence_test: Grisu's exact digits are Dragon's for random f16s. */
void
test_coretests_num_flt2dec_random_exact_f16_random_equivalence_test(void)
{
	size_t		n = 1000;

	for (size_t k = 1; k < 21; k++)
		coretests_num_flt2dec_random_f16_random_equivalence_test(coretests_num_flt2dec_random_exact_closure_0, NULL,
																 coretests_num_flt2dec_random_exact_closure_1, NULL, k,
																 n);
}

/* test_coretests_num_flt2dec_random_exact_f32_random_equivalence_test: Grisu's exact digits are Dragon's for random f32s. */
void
test_coretests_num_flt2dec_random_exact_f32_random_equivalence_test(void)
{
	size_t		n = 1000;

	for (size_t k = 1; k < 21; k++)
		coretests_num_flt2dec_random_f32_random_equivalence_test(coretests_num_flt2dec_random_exact_closure_0, NULL,
																 coretests_num_flt2dec_random_exact_closure_1, NULL, k,
																 n);
}

/* test_coretests_num_flt2dec_random_exact_f64_random_equivalence_test: Grisu's exact digits are Dragon's for random f64s. */
void
test_coretests_num_flt2dec_random_exact_f64_random_equivalence_test(void)
{
	size_t		n = 1000;

	for (size_t k = 1; k < 21; k++)
		coretests_num_flt2dec_random_f64_random_equivalence_test(coretests_num_flt2dec_random_exact_closure_0, NULL,
																 coretests_num_flt2dec_random_exact_closure_1, NULL, k,
																 n);
}
