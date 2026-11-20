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
 * The C port of coretests' num/flt2dec/mod.rs at Rust 1.99.1 (the port
 * text's section 13): the f16 and f32 instances of flt2dec's generics, the
 * checks of a strategy's digits, the strategies' sanity vectors, and the
 * to_*_str vectors, each as core's, with its message and line.
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_flt2dec.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* RUST_FILE is coretests' num/flt2dec/mod.rs, where these tests' panics are. */
#define RUST_FILE STD_RUST_LIBRARY STD_SEP "coretests" STD_SEP "tests" STD_SEP "num" STD_SEP "flt2dec" STD_SEP "mod.rs"

/* core's flt2dec generics at f16, held as its bits. */
#define STD_FLT2DEC_SCOPE extern
#define STD_FLT2DEC_DEFINE
#define STD_FLT2DEC_TS F16
#define STD_FLT2DEC_T uint16
#define STD_FLT2DEC_TO_BITS(v) ((uint64) (v))
#define STD_FLT2DEC_BITS 16
#define STD_FLT2DEC_SIG_BITS 10
#define STD_FLT2DEC_EXP_BIAS 15
#include "doltgres/std/flt2dec_template.h"

/*
 * std_fmt_float_F16_GeneralFormat_already_rounded_value_should_use_exponential
 * is f16's GeneralFormat on its bits: 1e+16 is f16's infinity, 1e-4 rounds to
 * 0x068e, and a positive f16's bits order as its values do.
 */
static bool
std_fmt_float_F16_GeneralFormat_already_rounded_value_should_use_exponential(const uint16 *self)
{
	uint16		max_abs = std_f64_as_f16(1e+16);
	uint16		abs = *self & 0x7fff;

	return (abs != 0 && abs < std_f64_as_f16(1e-4)) || abs == max_abs;
}

/* fmt/float.rs's generics at f16, for f16's Debug. */
#define STD_FLOAT_SCOPE static
#define STD_FLOAT_DECLARE
#define STD_FLOAT_DEFINE
#define STD_FLOAT_TS F16
#define STD_FLOAT_T uint16
#define STD_FLOAT_GENERAL_FORMAT std_fmt_float_F16_GeneralFormat_already_rounded_value_should_use_exponential
#include "doltgres/std/float_template.h"

/* std_f16_Debug_fmt is f16's Debug: float_to_general_debug. */
static bool
std_f16_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_float_float_to_general_debugF16(f, (const uint16 *) value);
}

/* std_flt2dec_decoder_Decoded_Debug_fmt is Decoded's derived Debug. */
bool
std_flt2dec_decoder_Decoded_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdDecoded *self = (const StdDecoded *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Decoded", 7);

	std_DebugStruct_field(&b, "mant", 4, &self->mant, std_u64_Debug_fmt);
	std_DebugStruct_field(&b, "minus", 5, &self->minus, std_u64_Debug_fmt);
	std_DebugStruct_field(&b, "plus", 4, &self->plus, std_u64_Debug_fmt);
	std_DebugStruct_field(&b, "exp", 3, &self->exp, std_i16_Debug_fmt);
	std_DebugStruct_field(&b, "inclusive", 9, &self->inclusive, std_bool_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* std_flt2dec_decoder_FullDecoded_Debug_fmt is FullDecoded's derived Debug. */
static bool
std_flt2dec_decoder_FullDecoded_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdFullDecoded *self = (const StdFullDecoded *) value;

	switch (self->kind)
	{
		case STD_FULL_DECODED_NAN:
			return std_fmt_write_str(f, "Nan", 3);
		case STD_FULL_DECODED_INFINITE:
			return std_fmt_write_str(f, "Infinite", 8);
		case STD_FULL_DECODED_ZERO:
			return std_fmt_write_str(f, "Zero", 4);
		case STD_FULL_DECODED_FINITE:
			break;
	}
	{
		StdDebugTuple b = std_fmt_debug_tuple(f, "Finite", 6);

		std_DebugTuple_field(&b, &self->finite, std_flt2dec_decoder_Decoded_Debug_fmt);
		return std_DebugTuple_finish(&b);
	}
}

/*
 * CORETESTS_DECODE_FINITE defines decode_finite at a float type: the
 * value's Decoded, panicking unless it is finite.
 */
#define CORETESTS_DECODE_FINITE(TS, T, T_DEBUG) \
StdDecoded \
coretests_num_flt2dec_decode_finite##TS(T v) \
{ \
	StdFullDecoded full_decoded; \
	static const StdFmtPiece pieces[] = { \
		STD_FMT_LIT("expected finite, got "), STD_FMT_ARG(0), STD_FMT_LIT(" instead for "), STD_FMT_ARG(1), \
	}; \
	StdFmtArg	args[2]; \
	StdFmtArguments arguments = {pieces, lengthof(pieces), args}; \
\
	std_flt2dec_decoder_decode##TS(v, &full_decoded); \
	if (full_decoded.kind == STD_FULL_DECODED_FINITE) \
		return full_decoded.finite; \
	args[0].value = &full_decoded; \
	args[0].fmt = std_flt2dec_decoder_FullDecoded_Debug_fmt; \
	args[1].value = &v; \
	args[1].fmt = (T_DEBUG); \
	std_panic_fmt(RUST_FILE, 23, 25, &arguments); \
}

/* decode_finite at f16, f32, and f64. */
CORETESTS_DECODE_FINITE(F16, uint16, std_f16_Debug_fmt)
CORETESTS_DECODE_FINITE(F32, float4, std_f32_Debug_fmt)
CORETESTS_DECODE_FINITE(F64, float8, std_f64_Debug_fmt)

/*
 * coretests_num_flt2dec_mismatch panics with a check's message: the
 * caller's pieces and arguments, then ": actual (digits, k), expected
 * (digits, k)", the digits read as UTF-8.
 */
pg_noreturn static void
coretests_num_flt2dec_mismatch(uint32 line, uint32 col, const StdFmtPiece *prefix, size_t prefix_len,
							   const StdFmtArg *prefix_args, size_t prefix_args_len, const uint8 *actual,
							   size_t actual_len, int16 actual_k, const uint8 *expected, size_t expected_len,
							   int16 expected_k)
{
	StdFmtPiece pieces[16];
	StdFmtArg	args[8];
	StdFmtArguments arguments = {pieces, prefix_len + 4, args};
	StdUtf8Error error;
	StdStr		actual_str = {(const char *) actual, actual_len};
	StdStr		expected_str = {(const char *) expected, expected_len};
	StdTupleRef actual_tuple;
	StdTupleRef expected_tuple;
	StdFmtPiece actual_piece = STD_FMT_LIT(": actual ");
	StdFmtPiece expected_piece = STD_FMT_LIT(", expected ");

	if (!std_str_from_utf8(actual, actual_len, &error))
		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, &error,
								 std_Utf8Error_Debug_fmt, RUST_FILE, line, col);
	if (!std_str_from_utf8(expected, expected_len, &error))
		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, &error,
								 std_Utf8Error_Debug_fmt, RUST_FILE, line, col);
	actual_tuple = STD_TUPLE(2, STD_ITEM(&actual_str, std_str_Debug_fmt), STD_ITEM(&actual_k, std_i16_Debug_fmt));
	expected_tuple = STD_TUPLE(2, STD_ITEM(&expected_str, std_str_Debug_fmt),
							   STD_ITEM(&expected_k, std_i16_Debug_fmt));
	memcpy(pieces, prefix, prefix_len * sizeof(StdFmtPiece));
	memcpy(args, prefix_args, prefix_args_len * sizeof(StdFmtArg));
	pieces[prefix_len] = actual_piece;
	pieces[prefix_len + 1] = (StdFmtPiece) STD_FMT_ARG(prefix_args_len);
	pieces[prefix_len + 2] = expected_piece;
	pieces[prefix_len + 3] = (StdFmtPiece) STD_FMT_ARG(prefix_args_len + 1);
	args[prefix_args_len].value = &actual_tuple;
	args[prefix_args_len].fmt = std_tuple_Debug_fmt;
	args[prefix_args_len + 1].value = &expected_tuple;
	args[prefix_args_len + 1].fmt = std_tuple_Debug_fmt;
	std_panic_fmt(RUST_FILE, line, col, &arguments);
}

/*
 * CORETESTS_CHECK_SHORTEST defines check_shortest! at a float type: f's
 * digits of the value's Decoded, which must be expected_buf at expected_exp,
 * panicking at the macro's line and column with v's text.
 */
#define CORETESTS_CHECK_SHORTEST(TS, T) \
static void \
coretests_num_flt2dec_check_shortest##TS(StdFlt2decFormatShortestFn f, void *f_captures, T v, \
										 const char *expected_buf, int16 expected_exp, const char *v_text, \
										 uint32 line, uint32 col) \
{ \
	uint8		buf[STD_FLT2DEC_MAX_SIG_DIGITS]; \
	StdDecoded	decoded = coretests_num_flt2dec_decode_finite##TS(v); \
	int16		k; \
	size_t		len; \
	size_t		expected_len = strlen(expected_buf); \
\
	memset(buf, '_', sizeof(buf)); \
	len = f(f_captures, &decoded, buf, sizeof(buf), &k); \
	if (!(len == expected_len && memcmp(buf, expected_buf, len) == 0 && k == expected_exp)) \
	{ \
		static const StdFmtPiece prefix[] = {STD_FMT_LIT("shortest mismatch for v="), STD_FMT_ARG(0)}; \
		StdStr		v_str = {v_text, strlen(v_text)}; \
		StdFmtArg	prefix_args[1] = {{&v_str, std_str_Display_fmt}}; \
\
		coretests_num_flt2dec_mismatch(line, col, prefix, lengthof(prefix), prefix_args, 1, buf, len, k, \
									   (const uint8 *) expected_buf, expected_len, expected_exp); \
	} \
}

/* check_shortest! at f16, f32, and f64. */
CORETESTS_CHECK_SHORTEST(F16, uint16)
CORETESTS_CHECK_SHORTEST(F32, float4)
CORETESTS_CHECK_SHORTEST(F64, float8)

/*
 * coretests_num_flt2dec_check_shortest_decoded is check_shortest! of a
 * Decoded written out: f's digits of it, which must be expected_buf at
 * expected_exp, panicking with its Debug.
 */
static void
coretests_num_flt2dec_check_shortest_decoded(StdFlt2decFormatShortestFn f, void *f_captures, uint64 mant,
											 uint64 minus, uint64 plus, int16 exp, bool inclusive,
											 const char *expected_buf, int16 expected_exp, uint32 line, uint32 col)
{
	uint8		buf[STD_FLT2DEC_MAX_SIG_DIGITS];
	StdDecoded	decoded = {mant, minus, plus, exp, inclusive};
	int16		k;
	size_t		len;
	size_t		expected_len = strlen(expected_buf);

	memset(buf, '_', sizeof(buf));
	len = f(f_captures, &decoded, buf, sizeof(buf), &k);
	if (!(len == expected_len && memcmp(buf, expected_buf, len) == 0 && k == expected_exp))
	{
		static const StdFmtPiece prefix[] = {STD_FMT_LIT("shortest mismatch for "), STD_FMT_ARG(0)};
		StdFmtArg	prefix_args[1] = {{&decoded, std_flt2dec_decoder_Decoded_Debug_fmt}};

		coretests_num_flt2dec_mismatch(line, col, prefix, lengthof(prefix), prefix_args, 1, buf, len, k,
									   (const uint8 *) expected_buf, expected_len, expected_exp);
	}
}

/*
 * coretests_num_flt2dec_try is try_exact! (fixed false) and try_fixed!
 * (fixed true): f's digits of decoded in buf, its first expected_len bytes
 * at no limit for try_exact! and all of it at request for try_fixed!, which
 * must be expected at expectedk, panicking with the caller's prefix.
 */
static void
coretests_num_flt2dec_try(StdFlt2decFormatExactFn f, void *f_captures, const StdDecoded *decoded, uint8 *buf,
						  size_t buf_len, bool fixed, int16 request, const uint8 *expected, size_t expected_len,
						  int16 expectedk, uint32 line, uint32 col, const StdFmtPiece *prefix, size_t prefix_len,
						  const StdFmtArg *prefix_args, size_t prefix_args_len)
{
	int16		k;
	size_t		len;

	if (fixed)
		len = f(f_captures, decoded, buf, buf_len, request, &k);
	else
	{
		std_range_to_check(expected_len, buf_len, RUST_FILE, line, col);
		len = f(f_captures, decoded, buf, expected_len, PG_INT16_MIN, &k);
	}
	if (!(len == expected_len && memcmp(buf, expected, len) == 0 && k == expectedk))
		coretests_num_flt2dec_mismatch(line, col, prefix, prefix_len, prefix_args, prefix_args_len, buf, len, k,
									   expected, expected_len, expectedk);
}

/*
 * coretests_num_flt2dec_check_exact is check_exact's body after the decode:
 * f's exact digits of decoded at every significant length and their fixed
 * forms, the rounding of zero and negative widths, and the infinite zeroes
 * after the expected digits' space, vstr naming the value.
 */
static void
coretests_num_flt2dec_check_exact(StdFlt2decFormatExactFn f, void *f_captures, const StdDecoded *decoded,
								  const char *vstr, const char *expected_text, int16 expectedk)
{
	uint8		buf[1024];
	uint8		expected_[1024];
	const uint8 *expected = (const uint8 *) expected_text;
	size_t		expected_len = strlen(expected_text);
	StdUsizeOption cut = {0};
	StdStr		v = {vstr, strlen(vstr)};
	size_t		end;
	size_t		start;

	memset(buf, '_', sizeof(buf));
	memset(expected_, '_', sizeof(expected_));
	for (size_t j = 0; j < expected_len; j++)
	{
		if (expected[j] == ' ')
		{
			cut.has = true;
			cut.x = j;
			break;
		}
	}
	end = cut.has ? cut.x : expected_len - 1;
	for (size_t i = 1; i < end; i++)
	{
		int16		expectedk_ = expectedk;
		ssize_t		i_value = (ssize_t) i;
		StdFmtArg	prefix_args[2] = {{&v, std_str_Display_fmt}, {&i_value, std_i64_Display_fmt}};

		std_range_to_check(i, lengthof(expected_), RUST_FILE, 96, 18);
		std_range_to_check(i, expected_len, RUST_FILE, 96, 49);
		std_slice_copy_from_slice_Checked(expected_, i, expected, i, 1, 1, RUST_FILE, 96, 24);
		std_index_check(i, expected_len, RUST_FILE, 98, 12);
		if (expected[i] >= '5')
		{
			bool		round_to_even;

			if (i + 1 < expected_len)
			{
				std_index_check(i - 1, expected_len, RUST_FILE, 102, 20);
				round_to_even = (expected[i - 1] & 1) == 0;
				if (round_to_even)
				{
					std_index_check(i, expected_len, RUST_FILE, 103, 20);
					round_to_even = expected[i] == '5';
				}
				if (round_to_even)
				{
					std_index_check(i + 1, expected_len, RUST_FILE, 104, 20);
					round_to_even = expected[i + 1] == ' ';
				}
			}
			else
				round_to_even = false;
			if (!round_to_even)
			{
				uint8		c;

				std_range_to_check(i, lengthof(expected_), RUST_FILE, 110, 57);
				if (std_flt2dec_round_up(expected_, i, &c))
					expectedk_ += 1;
			}
		}
		{
			static const StdFmtPiece prefix[] = {
				STD_FMT_LIT("exact sigdigit mismatch for v="), STD_FMT_ARG(0), STD_FMT_LIT(", i="), STD_FMT_ARG(1),
			};

			std_range_to_check(i, lengthof(expected_), RUST_FILE, 116, 9);
			coretests_num_flt2dec_try(f, f_captures, decoded, buf, sizeof(buf), false, 0, expected_, i,
									  expectedk_, 116, 9, prefix, lengthof(prefix), prefix_args, 2);
		}
		{
			static const StdFmtPiece prefix[] = {
				STD_FMT_LIT("fixed sigdigit mismatch for v="), STD_FMT_ARG(0), STD_FMT_LIT(", i="), STD_FMT_ARG(1),
			};

			std_range_to_check(i, lengthof(expected_), RUST_FILE, 120, 9);
			coretests_num_flt2dec_try(f, f_captures, decoded, buf, sizeof(buf), true,
									  (int16) (expectedk_ - (int16) i), expected_, i, expectedk_, 120, 9, prefix,
									  lengthof(prefix), prefix_args, 2);
		}
	}
	std_index_check(0, expected_len, RUST_FILE, 128, 8);
	if (expected[0] > '5')
	{
		static const StdFmtPiece prefix[] = {STD_FMT_LIT("zero-width rounding-up mismatch for v="), STD_FMT_ARG(0)};
		StdFmtArg	prefix_args[1] = {{&v, std_str_Display_fmt}};

		coretests_num_flt2dec_try(f, f_captures, decoded, buf, sizeof(buf), true, expectedk, (const uint8 *) "1", 1,
								  (int16) (expectedk + 1), 129, 9, prefix, lengthof(prefix), prefix_args, 1);
		start = 1;
	}
	else
		start = 0;
	for (int16 i = (int16) start; i < -10; i++)
	{
		static const StdFmtPiece prefix[] = {
			STD_FMT_LIT("rounding-down mismatch for v="), STD_FMT_ARG(0), STD_FMT_LIT(", i="), STD_FMT_ARG(1),
		};
		int16		minus_i = (int16) -i;
		StdFmtArg	prefix_args[2] = {{&v, std_str_Display_fmt}, {&minus_i, std_i16_Display_fmt}};

		coretests_num_flt2dec_try(f, f_captures, decoded, buf, sizeof(buf), true, (int16) (expectedk - i),
								  (const uint8 *) "", 0, expectedk, 138, 9, prefix, lengthof(prefix), prefix_args,
								  2);
	}
	if (cut.has)
	{
		for (size_t i = cut.x; i < expected_len - 1; i++)
		{
			ssize_t		i_value = (ssize_t) i;
			StdFmtArg	prefix_args[2] = {{&v, std_str_Display_fmt}, {&i_value, std_i64_Display_fmt}};

			std_range_to_check(cut.x, lengthof(expected_), RUST_FILE, 147, 22);
			std_range_to_check(cut.x, expected_len, RUST_FILE, 147, 55);
			std_slice_copy_from_slice_Checked(expected_, cut.x, expected, cut.x, 1, 1, RUST_FILE, 147, 30);
			std_range_check(cut.x, i, lengthof(expected_), RUST_FILE, 148, 36);
			for (size_t j = cut.x; j < i; j++)
				expected_[j] = '0';
			{
				static const StdFmtPiece prefix[] = {
					STD_FMT_LIT("exact infzero mismatch for v="), STD_FMT_ARG(0), STD_FMT_LIT(", i="),
					STD_FMT_ARG(1),
				};

				std_range_to_check(i, lengthof(expected_), RUST_FILE, 152, 13);
				coretests_num_flt2dec_try(f, f_captures, decoded, buf, sizeof(buf), false, 0, expected_, i,
										  expectedk, 152, 13, prefix, lengthof(prefix), prefix_args, 2);
			}
			{
				static const StdFmtPiece prefix[] = {
					STD_FMT_LIT("fixed infzero mismatch for v="), STD_FMT_ARG(0), STD_FMT_LIT(", i="),
					STD_FMT_ARG(1),
				};

				std_range_to_check(i, lengthof(expected_), RUST_FILE, 156, 13);
				coretests_num_flt2dec_try(f, f_captures, decoded, buf, sizeof(buf), true,
										  (int16) (expectedk - (int16) i), expected_, i, expectedk, 156, 13, prefix,
										  lengthof(prefix), prefix_args, 2);
			}
		}
	}
}

/* CORETESTS_CHECK_EXACT defines check_exact at a float type: check_exact's body over the value's Decoded. */
#define CORETESTS_CHECK_EXACT(TS, T) \
static void \
coretests_num_flt2dec_check_exact##TS(StdFlt2decFormatExactFn f, void *f_captures, T v, const char *vstr, \
									  const char *expected, int16 expectedk) \
{ \
	StdDecoded	decoded = coretests_num_flt2dec_decode_finite##TS(v); \
\
	coretests_num_flt2dec_check_exact(f, f_captures, &decoded, vstr, expected, expectedk); \
}

/* check_exact at f16, f32, and f64. */
CORETESTS_CHECK_EXACT(F16, uint16)
CORETESTS_CHECK_EXACT(F32, float4)
CORETESTS_CHECK_EXACT(F64, float8)

/* coretests_num_flt2dec_F32_TestableFloat_ldexpi is f32's TestableFloat::ldexpi: f * 2^exp in f32. */
static float4
coretests_num_flt2dec_F32_TestableFloat_ldexpi(int64 f, ssize_t exp)
{
	return (float4) f * exp2f((float4) exp);
}

/* coretests_num_flt2dec_F64_TestableFloat_ldexpi is f64's TestableFloat::ldexpi: f * 2^exp in f64. */
static float8
coretests_num_flt2dec_F64_TestableFloat_ldexpi(int64 f, ssize_t exp)
{
	return (float8) f * exp2((float8) exp);
}

/*
 * CORETESTS_CHECK_EXACT_ONE defines check_exact_one at a float type: f's
 * exact and fixed digits of x * 2^e, which must be expected at expectedk.
 */
#define CORETESTS_CHECK_EXACT_ONE(TS, T) \
static void \
coretests_num_flt2dec_check_exact_one##TS(StdFlt2decFormatExactFn f, void *f_captures, int64 x, ssize_t e, \
										  const char *tstr, const char *expected_text, int16 expectedk) \
{ \
	uint8		buf[1024]; \
	T			v = coretests_num_flt2dec_##TS##_TestableFloat_ldexpi(x, e); \
	StdDecoded	decoded = coretests_num_flt2dec_decode_finite##TS(v); \
	const uint8 *expected = (const uint8 *) expected_text; \
	size_t		expected_len = strlen(expected_text); \
	StdStr		t = {tstr, strlen(tstr)}; \
	int64		e_value = (int64) e; \
	StdFmtArg	prefix_args[3] = {{&x, std_i64_Display_fmt}, {&e_value, std_i64_Display_fmt}, \
								  {&t, std_str_Display_fmt}}; \
\
	memset(buf, '_', sizeof(buf)); \
	{ \
		static const StdFmtPiece prefix[] = { \
			STD_FMT_LIT("exact mismatch for v="), STD_FMT_ARG(0), STD_FMT_LIT("p"), STD_FMT_ARG(1), STD_FMT_ARG(2), \
		}; \
\
		coretests_num_flt2dec_try(f, f_captures, &decoded, buf, sizeof(buf), false, 0, expected, expected_len, \
								  expectedk, 199, 5, prefix, lengthof(prefix), prefix_args, 3); \
	} \
	{ \
		static const StdFmtPiece prefix[] = { \
			STD_FMT_LIT("fixed mismatch for v="), STD_FMT_ARG(0), STD_FMT_LIT("p"), STD_FMT_ARG(1), STD_FMT_ARG(2), \
		}; \
\
		coretests_num_flt2dec_try(f, f_captures, &decoded, buf, sizeof(buf), true, \
								  (int16) (expectedk - (int16) expected_len), expected, expected_len, expectedk, \
								  202, 5, prefix, lengthof(prefix), prefix_args, 3); \
	} \
}

/* check_exact_one at f32 and f64. */
CORETESTS_CHECK_EXACT_ONE(F32, float4)
CORETESTS_CHECK_EXACT_ONE(F64, float8)

/* coretests_num_flt2dec_f16_shortest_sanity_test is f16_shortest_sanity_test: f's shortest digits of f16 values. */
void
coretests_num_flt2dec_f16_shortest_sanity_test(StdFlt2decFormatShortestFn f, void *f_captures, void (*f_drop) (void *))
{
	uint16		minf16;

	coretests_num_flt2dec_check_shortestF16(f, f_captures, std_f64_as_f16(0.1), "1", 0, "0.1f16", 236, 5);
	coretests_num_flt2dec_check_shortestF16(f, f_captures, std_f64_as_f16(1.0 / 3.0), "3333", 0, "1.0f16 / 3.0", 241, 5);
	coretests_num_flt2dec_check_shortestF16(f, f_captures, std_f64_as_f16(3.14), "314", 1, "3.14f16", 246, 5);
	coretests_num_flt2dec_check_shortestF16(f, f_captures, std_f64_as_f16(3.1415e4), "3141", 5, "3.1415e4f16", 251, 5);
	coretests_num_flt2dec_check_shortestF16(f, f_captures, coretests_num_ldexp_f16(std_f64_as_f16(1.0), 5), "32", 2, "crate::num::ldexp_f16(1.0, 5)", 257, 5);
	coretests_num_flt2dec_check_shortestF16(f, f_captures, CORETESTS_F16_MAX, "655", 5, "f16::MAX", 262, 5);
	coretests_num_flt2dec_check_shortestF16(f, f_captures, CORETESTS_F16_MIN_POSITIVE, "6104", -4, "f16::MIN_POSITIVE", 267, 5);
	minf16 = coretests_num_ldexp_f16(std_f64_as_f16(1.0), -24);
	coretests_num_flt2dec_check_shortestF16(f, f_captures, minf16, "6", -7, "minf16", 273, 5);
	if (f_drop != NULL)
		f_drop(f_captures);
}

/* coretests_num_flt2dec_f16_exact_sanity_test is f16_exact_sanity_test: f's exact digits of f16 values. */
void
coretests_num_flt2dec_f16_exact_sanity_test(StdFlt2decFormatExactFn f, void *f_captures, void (*f_drop) (void *))
{
	uint16		minf16;

	minf16 = coretests_num_ldexp_f16(std_f64_as_f16(1.0), -24);
	coretests_num_flt2dec_check_exactF16(f, f_captures, std_f64_as_f16(0.1), "0.1f16", "999755859375     ", -1);
	coretests_num_flt2dec_check_exactF16(f, f_captures, std_f64_as_f16(0.5), "0.5f16", "5                ", 0);
	coretests_num_flt2dec_check_exactF16(f, f_captures, std_f64_as_f16(1.0 / 3.0), "1.0f16 / 3.0", "333251953125     ", 0);
	coretests_num_flt2dec_check_exactF16(f, f_captures, std_f64_as_f16(3.141), "3.141f16", "3140625          ", 1);
	coretests_num_flt2dec_check_exactF16(f, f_captures, std_f64_as_f16(3.141e4), "3.141e4f16", "31408            ", 5);
	coretests_num_flt2dec_check_exactF16(f, f_captures, CORETESTS_F16_MAX, "f16::MAX", "65504            ", 5);
	coretests_num_flt2dec_check_exactF16(f, f_captures, CORETESTS_F16_MIN_POSITIVE, "f16::MIN_POSITIVE", "6103515625       ", -4);
	coretests_num_flt2dec_check_exactF16(f, f_captures, minf16, "minf16", "59604644775390625", -7);
	if (f_drop != NULL)
		f_drop(f_captures);
}

/* coretests_num_flt2dec_f32_shortest_sanity_test is f32_shortest_sanity_test: f's shortest digits of f32 values. */
void
coretests_num_flt2dec_f32_shortest_sanity_test(StdFlt2decFormatShortestFn f, void *f_captures, void (*f_drop) (void *))
{
	float4		minf32;

	coretests_num_flt2dec_check_shortestF32(f, f_captures, 0.1f, "1", 0, "0.1f32", 304, 5);
	coretests_num_flt2dec_check_shortestF32(f, f_captures, 1.0f / 3.0f, "33333334", 0, "1.0f32 / 3.0", 309, 5);
	coretests_num_flt2dec_check_shortestF32(f, f_captures, 3.141592f, "3141592", 1, "3.141592f32", 314, 5);
	coretests_num_flt2dec_check_shortestF32(f, f_captures, 3.141592e17f, "3141592", 18, "3.141592e17f32", 319, 5);
	coretests_num_flt2dec_check_shortestF32(f, f_captures, coretests_num_ldexp_f32(1.0f, 25), "33554432", 8, "ldexp_f32(1.0, 25)", 325, 5);
	coretests_num_flt2dec_check_shortestF32(f, f_captures, FLT_MAX, "34028235", 39, "f32::MAX", 330, 5);
	coretests_num_flt2dec_check_shortestF32(f, f_captures, FLT_MIN, "11754944", -37, "f32::MIN_POSITIVE", 335, 5);
	minf32 = coretests_num_ldexp_f32(1.0f, -149);
	coretests_num_flt2dec_check_shortestF32(f, f_captures, minf32, "1", -44, "minf32", 341, 5);
	if (f_drop != NULL)
		f_drop(f_captures);
}

/* coretests_num_flt2dec_f32_exact_sanity_test is f32_exact_sanity_test: f's exact digits of f32 values. */
void
coretests_num_flt2dec_f32_exact_sanity_test(StdFlt2decFormatExactFn f, void *f_captures, void (*f_drop) (void *))
{
	float4		minf32;

	minf32 = coretests_num_ldexp_f32(1.0f, -149);
	coretests_num_flt2dec_check_exactF32(f, f_captures, 0.1f, "0.1f32", "100000001490116119384765625             ", 0);
	coretests_num_flt2dec_check_exactF32(f, f_captures, 0.5f, "0.5f32", "5                                       ", 0);
	coretests_num_flt2dec_check_exactF32(f, f_captures, 1.0f / 3.0f, "1.0f32 / 3.0", "3333333432674407958984375               ", 0);
	coretests_num_flt2dec_check_exactF32(f, f_captures, 3.141592f, "3.141592f32", "31415920257568359375                    ", 1);
	coretests_num_flt2dec_check_exactF32(f, f_captures, 3.141592e17f, "3.141592e17f32", "314159196796878848                      ", 18);
	coretests_num_flt2dec_check_exactF32(f, f_captures, FLT_MAX, "f32::MAX", "34028234663852885981170418348451692544  ", 39);
	coretests_num_flt2dec_check_exactF32(f, f_captures, FLT_MIN, "f32::MIN_POSITIVE", "1175494350822287507968736537222245677818", -37);
	coretests_num_flt2dec_check_exactF32(f, f_captures, minf32, "minf32", "1401298464324817070923729583289916131280", -44);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(12676506), -102, "f32", "2", -23);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(12676506), -103, "f32", "12", -23);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(15445013), 86, "f32", "119", 34);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(13734123), -138, "f32", "3941", -34);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(12428269), -130, "f32", "91308", -32);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(15334037), -146, "f32", "171900", -36);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(11518287), -41, "f32", "5237910", -5);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(12584953), -145, "f32", "28216440", -36);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(15961084), -125, "f32", "375243281", -30);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(14915817), -146, "f32", "1672120916", -36);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(10845484), -102, "f32", "21388945814", -23);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(16431059), -61, "f32", "712583594561", -11);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(16093626), 69, "f32", "1", 29);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(9983778), 25, "f32", "34", 15);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(12745034), 104, "f32", "259", 39);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(12706553), 72, "f32", "6001", 29);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(11005028), 45, "f32", "38721", 21);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(15059547), 71, "f32", "355584", 29);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(16015691), -99, "f32", "2526831", -22);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(8667859), 56, "f32", "62458507", 24);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(14855922), -82, "f32", "307213267", -17);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(14855922), -83, "f32", "1536066333", -17);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(10144164), -110, "f32", "78147796834", -26);
	coretests_num_flt2dec_check_exact_oneF32(f, f_captures, INT64CONST(13248074), 95, "f32", "524810279937", 36);
	if (f_drop != NULL)
		f_drop(f_captures);
}

/* coretests_num_flt2dec_f64_shortest_sanity_test is f64_shortest_sanity_test: f's shortest digits of f64 values. */
void
coretests_num_flt2dec_f64_shortest_sanity_test(StdFlt2decFormatShortestFn f, void *f_captures, void (*f_drop) (void *))
{
	float8		minf64;

	coretests_num_flt2dec_check_shortestF64(f, f_captures, 0.1, "1", 0, "0.1f64", 395, 5);
	coretests_num_flt2dec_check_shortestF64(f, f_captures, 100.0, "1", 3, "100.0f64", 401, 5);
	coretests_num_flt2dec_check_shortestF64(f, f_captures, 1.0 / 3.0, "3333333333333333", 0, "1.0f64 / 3.0", 406, 5);
	coretests_num_flt2dec_check_shortestF64(f, f_captures, 1.00000762939453125, "10000076293945313", 1, "1.00000762939453125f64", 413, 5);
	coretests_num_flt2dec_check_shortestF64(f, f_captures, 3.141592, "3141592", 1, "3.141592f64", 418, 5);
	coretests_num_flt2dec_check_shortestF64(f, f_captures, 3.141592e17, "3141592", 18, "3.141592e17f64", 423, 5);
	coretests_num_flt2dec_check_shortestF64(f, f_captures, coretests_num_ldexp_f64(1.0, 64), "18446744073709552", 20, "ldexp_f64(1.0, 64)", 429, 5);
	coretests_num_flt2dec_check_shortestF64(f, f_captures, 1.0e23, "1", 24, "1.0e23f64", 435, 5);
	coretests_num_flt2dec_check_shortestF64(f, f_captures, DBL_MAX, "17976931348623157", 309, "f64::MAX", 440, 5);
	coretests_num_flt2dec_check_shortestF64(f, f_captures, DBL_MIN, "22250738585072014", -307, "f64::MIN_POSITIVE", 445, 5);
	minf64 = coretests_num_ldexp_f64(1.0, -1074);
	coretests_num_flt2dec_check_shortestF64(f, f_captures, minf64, "5", -323, "minf64", 451, 5);
	if (f_drop != NULL)
		f_drop(f_captures);
}

/* coretests_num_flt2dec_f64_exact_sanity_test is f64_exact_sanity_test: f's exact digits of f64 values. */
void
coretests_num_flt2dec_f64_exact_sanity_test(StdFlt2decFormatExactFn f, void *f_captures, void (*f_drop) (void *))
{
	float8		minf64;

	minf64 = coretests_num_ldexp_f64(1.0, -1074);
	coretests_num_flt2dec_check_exactF64(f, f_captures, 0.1, "0.1f64", "1000000000000000055511151231257827021181", 0);
	coretests_num_flt2dec_check_exactF64(f, f_captures, 0.45, "0.45f64", "4500000000000000111022302462515654042363", 0);
	coretests_num_flt2dec_check_exactF64(f, f_captures, 0.5, "0.5f64", "5                                       ", 0);
	coretests_num_flt2dec_check_exactF64(f, f_captures, 0.95, "0.95f64", "9499999999999999555910790149937383830547", 0);
	coretests_num_flt2dec_check_exactF64(f, f_captures, 100.0, "100.0f64", "1                                       ", 3);
	coretests_num_flt2dec_check_exactF64(f, f_captures, 999.5, "999.5f64", "9995000000000000000000000000000000000000", 3);
	coretests_num_flt2dec_check_exactF64(f, f_captures, 1.0 / 3.0, "1.0f64 / 3.0", "3333333333333333148296162562473909929394", 0);
	coretests_num_flt2dec_check_exactF64(f, f_captures, 3.141592, "3.141592f64", "3141592000000000162174274009885266423225", 1);
	coretests_num_flt2dec_check_exactF64(f, f_captures, 3.141592e17, "3.141592e17f64", "3141592                                 ", 18);
	coretests_num_flt2dec_check_exactF64(f, f_captures, 1.0e23, "1.0e23f64", "99999999999999991611392                 ", 23);
	coretests_num_flt2dec_check_exactF64(f, f_captures, DBL_MAX, "f64::MAX", "1797693134862315708145274237317043567980", 309);
	coretests_num_flt2dec_check_exactF64(f, f_captures, DBL_MIN, "f64::MIN_POSITIVE", "2225073858507201383090232717332404064219", -307);
	coretests_num_flt2dec_check_exactF64(f, f_captures, minf64, "minf64", "4940656458412465441765687928682213723650598026143247644255856825006755072702087518652998363616359923" "7979656469544571773092665671035593979639877479601078187812630071319031140452784581716784898210368871" "8636056998730723050006387409153564984387312473397273169615140031715385398074126238565591171026658556" "6867681870395603106249319452715914924553293054565444011274801297099995419319894090804165633245247571" "4786901472678015935523861155013480352649347201937902681071074917033322268447533357208324319360923828" "9345836806010601150616980975307834227731832924790498252473077637592724787465608477820373446969953364" "7017972677717585125660551199131504891101451037862738167250955837389733598993664809941164205702637090" "279242767544565229087538682506419718265533447265625         ", -323);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8511030020275656), -342, "f64", "9", -87);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(5201988407066741), -824, "f64", "46", -232);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6406892948269899), 237, "f64", "141", 88);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8431154198732492), 72, "f64", "3981", 38);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6475049196144587), 99, "f64", "41040", 46);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8274307542972842), 726, "f64", "292084", 235);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(5381065484265332), -456, "f64", "2891946", -121);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6761728585499734), -1057, "f64", "43787718", -302);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(7976538478610756), 376, "f64", "122770163", 130);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(5982403858958067), 377, "f64", "1841552452", 130);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(5536995190630837), 93, "f64", "54835744350", 44);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(7225450889282194), 710, "f64", "389190181146", 230);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(7225450889282194), 709, "f64", "1945950905732", 230);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8703372741147379), 117, "f64", "14460958381605", 52);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8944262675275217), -1001, "f64", "417367747458531", -285);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(7459803696087692), -707, "f64", "1107950772878888", -196);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6080469016670379), -381, "f64", "12345501366327440", -98);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8385515147034757), 721, "f64", "925031711960365024", 233);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(7514216811389786), -828, "f64", "4198047150284889840", -233);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8397297803260511), -345, "f64", "11716315319786511046", -87);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6733459239310543), 202, "f64", "432810072844612493629", 77);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8091450587292794), -473, "f64", "3317710118160031081518", -126);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6567258882077402), 952, "f64", "3", 303);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6712731423444934), 535, "f64", "76", 177);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6712731423444934), 534, "f64", "378", 177);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(5298405411573037), -957, "f64", "4350", -272);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(5137311167659507), -144, "f64", "23037", -27);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6722280709661868), 363, "f64", "126301", 126);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(5344436398034927), -169, "f64", "7142211", -35);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8369123604277281), -853, "f64", "13934574", -240);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8995822108487663), -780, "f64", "141463449", -218);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8942832835564782), -383, "f64", "4539277920", -99);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8942832835564782), -384, "f64", "22696389598", -99);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8942832835564782), -385, "f64", "113481947988", -99);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6965949469487146), -249, "f64", "7700366561890", -59);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6965949469487146), -250, "f64", "38501832809448", -59);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6965949469487146), -251, "f64", "192509164047238", -59);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(7487252720986826), 548, "f64", "6898586531774201", 181);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(5592117679628511), 164, "f64", "13076622631878654", 66);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8887055249355788), 665, "f64", "136052020756121240", 217);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(6994187472632449), 690, "f64", "3592810217475959676", 224);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8797576579012143), 588, "f64", "89125197712484551899", 193);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(7363326733505337), 272, "f64", "558769757362301140950", 98);
	coretests_num_flt2dec_check_exact_oneF64(f, f_captures, INT64CONST(8549497411294502), -448, "f64", "1176257830728540379990", -118);
	if (f_drop != NULL)
		f_drop(f_captures);
}

/* coretests_num_flt2dec_more_shortest_sanity_test is more_shortest_sanity_test: f's shortest digits of two Decodeds. */
void
coretests_num_flt2dec_more_shortest_sanity_test(StdFlt2decFormatShortestFn f, void *f_captures, void (*f_drop) (void *))
{
	coretests_num_flt2dec_check_shortest_decoded(f, f_captures, UINT64CONST(99999999999999999), 1, 1, 0, true, "1", 18, 545, 5);
	coretests_num_flt2dec_check_shortest_decoded(f, f_captures, UINT64CONST(99999999999999999), 1, 1, 0, false, "99999999999999999", 17, 547, 5);
	if (f_drop != NULL)
		f_drop(f_captures);
}

/* CoretestsToStringWithPartsFn is to_string_with_parts' FnMut: the formatted parts of a value in buf and parts. */
typedef StdNumfmtFormatted (*CoretestsToStringWithPartsFn) (void *captures, uint8 *buf, size_t buf_len,
															StdNumfmtPart *parts, size_t parts_len);

/*
 * coretests_num_flt2dec_to_string_with_parts is to_string_with_parts: f's
 * formatted parts written into a String, whose length is the parts'.
 */
static void
coretests_num_flt2dec_to_string_with_parts(CoretestsToStringWithPartsFn f, void *f_captures,
										   void (*f_drop) (void *), StdString *result)
{
	volatile StdVec ret;
	volatile bool ret_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		uint8		buf[1024] = {0};
		StdNumfmtPart parts[16];
		StdNumfmtFormatted formatted;
		StdVec	   *ret_v = (StdVec *) &ret;
		size_t		written;
		bool		has_written;
		size_t		ret_len;
		StdUtf8Error error;

		for (int i = 0; i < 16; i++)
			parts[i] = (StdNumfmtPart)
		{
			.kind = STD_NUMFMT_PART_ZERO
		};
		formatted = f(f_captures, buf, sizeof(buf), parts, lengthof(parts));
		std_vec_u8_from_elem(ret_v, 0, std_numfmt_Formatted_len(&formatted));
		ret_live = true;
		has_written = std_numfmt_Formatted_write(&formatted, ret_v->ptr, ret_v->len, &written);
		ret_len = ret_v->len;
		if (!(has_written && written == ret_len))
		{
			StdOptionRef left = {has_written, STD_ITEM(&written, std_usize_Debug_fmt)};
			StdOptionRef right = {true, STD_ITEM(&ret_len, std_usize_Debug_fmt)};

			std_assert_failed(RUST_FILE, 559, 5, "==", &left, std_option_Debug_fmt, &right, std_option_Debug_fmt, NULL);
		}
		if (!std_str_from_utf8(ret_v->ptr, ret_v->len, &error))
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1, &error,
									 std_Utf8Error_Debug_fmt, RUST_FILE, 560, 28);
		ret_live = false;
		*result = *ret_v;
		if (f_drop != NULL)
			f_drop(f_captures);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (ret_live)
			std_vec_free_Checked((StdVec *) &ret, 1, 1);
		if (f_drop != NULL)
			f_drop(f_captures);
		std_unwind_resume(&unwind);
	}
}

/*
 * coretests_num_flt2dec_assert_eq_str is assert_eq!(actual, expected) of a
 * to_string's String and a str, which drops actual, also when it panics.
 */
static void
coretests_num_flt2dec_assert_eq_str(StdString *actual, const char *expected, uint32 line, uint32 col)
{
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdStr		right = {expected, strlen(expected)};

		if (!(actual->len == right.len && memcmp(actual->ptr, right.ptr, right.len) == 0))
			std_assert_failed(RUST_FILE, line, col, "==", actual, std_String_Debug_fmt, &right, std_str_Debug_fmt, NULL);
		std_vec_free_Checked(actual, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		std_vec_free_Checked(actual, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/*
 * coretests_num_flt2dec_assert_eq_format is assert_eq!(actual,
 * format!("<prefix>{:0>width}<suffix>", "")) of a to_string's String: the
 * format! made, compared, and both dropped, also when it panics.
 */
static void
coretests_num_flt2dec_assert_eq_format(StdString *actual, const char *prefix, uint16 width, const char *suffix,
									   uint32 line, uint32 col)
{
	volatile StdString expected;
	volatile bool expected_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdFmtPiece pieces[3] = {
			{prefix, strlen(prefix)},
			{NULL, 0, 0, {.fill = '0', .align = STD_FMT_ALIGN_RIGHT, .has_width = true, .width = width}},
			{suffix, strlen(suffix)},
		};
		StdStr		empty = {"", 0};
		StdFmtArg	args[1] = {{&empty, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString  *expected_v = (StdString *) &expected;

		std_fmt_format_Checked(expected_v, &arguments);
		expected_live = true;
		if (!(actual->len == expected_v->len && memcmp(actual->ptr, expected_v->ptr, actual->len) == 0))
			std_assert_failed(RUST_FILE, line, col, "==", actual, std_String_Debug_fmt, expected_v,
							  std_String_Debug_fmt, NULL);
		expected_live = false;
		std_vec_free_Checked(expected_v, 1, 1);
		std_vec_free_Checked(actual, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (expected_live)
			std_vec_free_Checked((StdVec *) &expected, 1, 1);
		std_vec_free_Checked(actual, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/*
 * CoretestsToShortestStrClosure0 is to_shortest_str_test's to_string's
 * closure: the borrowed f, and the value's arguments, its float made by the
 * instance that fills it.
 */
typedef struct CoretestsToShortestStrClosure0
{
	StdFlt2decFormatShortestFn f;
	void	   *f_captures;
	union
	{
		uint16		f16;
		float4		f32;
		float8		f64;
	}			v;
	StdFlt2decSign sign;
	size_t		frac_digits;
	int16		dec_bounds_0;
	int16		dec_bounds_1;
	bool		upper;
	size_t		ndigits;
} CoretestsToShortestStrClosure0;

/* coretests_num_flt2dec_closure_0_0_shortest is the inner closure |d, b| f(d, b): the borrowed f, called. */
static size_t
coretests_num_flt2dec_closure_0_0_shortest(void *captures, const StdDecoded *d, uint8 *b, size_t b_len,
										   int16 *result_1)
{
	CoretestsToShortestStrClosure0 *c = (CoretestsToShortestStrClosure0 *) captures;

	return c->f(c->f_captures, d, b, b_len, result_1);
}

/*
 * CORETESTS_TO_STRING defines each to_*_str_test's to_string at a float
 * type: the outer closure |buf, parts| to_*_str(..), and to_string, which
 * runs it through to_string_with_parts.
 */
#define CORETESTS_TO_STRING(TS, T, FIELD) \
static StdNumfmtFormatted \
coretests_num_flt2dec_to_shortest_str_test_to_string_closure_0##TS(void *captures, uint8 *buf, size_t buf_len, \
																   StdNumfmtPart *parts, size_t parts_len) \
{ \
	CoretestsToShortestStrClosure0 *c = (CoretestsToShortestStrClosure0 *) captures; \
\
	return std_flt2dec_to_shortest_str##TS(coretests_num_flt2dec_closure_0_0_shortest, c, NULL, c->v.FIELD, c->sign, \
										   c->frac_digits, buf, buf_len, parts, parts_len); \
} \
static void \
coretests_num_flt2dec_to_shortest_str_test_to_string##TS(StdFlt2decFormatShortestFn f, void *f_captures, T v, \
														 StdFlt2decSign sign, size_t frac_digits, StdString *result) \
{ \
	CoretestsToShortestStrClosure0 captures = {f, f_captures}; \
\
	captures.v.FIELD = v; \
	captures.sign = sign; \
	captures.frac_digits = frac_digits; \
	coretests_num_flt2dec_to_string_with_parts(coretests_num_flt2dec_to_shortest_str_test_to_string_closure_0##TS, \
											   &captures, NULL, result); \
} \
static StdNumfmtFormatted \
coretests_num_flt2dec_to_shortest_exp_str_test_to_string_closure_0##TS(void *captures, uint8 *buf, size_t buf_len, \
																	   StdNumfmtPart *parts, size_t parts_len) \
{ \
	CoretestsToShortestStrClosure0 *c = (CoretestsToShortestStrClosure0 *) captures; \
\
	return std_flt2dec_to_shortest_exp_str##TS(coretests_num_flt2dec_closure_0_0_shortest, c, NULL, c->v.FIELD, \
											   c->sign, c->dec_bounds_0, c->dec_bounds_1, c->upper, buf, buf_len, \
											   parts, parts_len); \
} \
static void \
coretests_num_flt2dec_to_shortest_exp_str_test_to_string##TS(StdFlt2decFormatShortestFn f, void *f_captures, T v, \
															 StdFlt2decSign sign, int16 exp_bounds_0, \
															 int16 exp_bounds_1, bool upper, StdString *result) \
{ \
	CoretestsToShortestStrClosure0 captures = {f, f_captures}; \
\
	captures.v.FIELD = v; \
	captures.sign = sign; \
	captures.dec_bounds_0 = exp_bounds_0; \
	captures.dec_bounds_1 = exp_bounds_1; \
	captures.upper = upper; \
	coretests_num_flt2dec_to_string_with_parts(coretests_num_flt2dec_to_shortest_exp_str_test_to_string_closure_0##TS, \
											   &captures, NULL, result); \
}

/* The shortest tests' to_string at f16, f32, and f64. */
CORETESTS_TO_STRING(F16, uint16, f16)
CORETESTS_TO_STRING(F32, float4, f32)
CORETESTS_TO_STRING(F64, float8, f64)

/*
 * CoretestsToExactStrClosure0 is the exact tests' to_string's closure: the
 * borrowed f, and the value's arguments.
 */
typedef struct CoretestsToExactStrClosure0
{
	StdFlt2decFormatExactFn f;
	void	   *f_captures;
	union
	{
		uint16		f16;
		float4		f32;
		float8		f64;
	}			v;
	StdFlt2decSign sign;
	size_t		ndigits;
	bool		upper;
	size_t		frac_digits;
} CoretestsToExactStrClosure0;

/* coretests_num_flt2dec_closure_0_0_exact is the inner closure |d, b, l| f(d, b, l): the borrowed f, called. */
static size_t
coretests_num_flt2dec_closure_0_0_exact(void *captures, const StdDecoded *d, uint8 *b, size_t b_len, int16 l,
										int16 *result_1)
{
	CoretestsToExactStrClosure0 *c = (CoretestsToExactStrClosure0 *) captures;

	return c->f(c->f_captures, d, b, b_len, l, result_1);
}

/* CORETESTS_TO_EXACT_STRING defines the exact tests' to_string at a float type. */
#define CORETESTS_TO_EXACT_STRING(TS, T, FIELD) \
static StdNumfmtFormatted \
coretests_num_flt2dec_to_exact_exp_str_test_to_string_closure_0##TS(void *captures, uint8 *buf, size_t buf_len, \
																	StdNumfmtPart *parts, size_t parts_len) \
{ \
	CoretestsToExactStrClosure0 *c = (CoretestsToExactStrClosure0 *) captures; \
\
	return std_flt2dec_to_exact_exp_str##TS(coretests_num_flt2dec_closure_0_0_exact, c, NULL, c->v.FIELD, c->sign, \
											std_saturating_sub_usize(c->ndigits, 1), c->upper, buf, buf_len, \
											parts, parts_len); \
} \
static void \
coretests_num_flt2dec_to_exact_exp_str_test_to_string##TS(StdFlt2decFormatExactFn f, void *f_captures, T v, \
														  StdFlt2decSign sign, size_t ndigits, bool upper, \
														  StdString *result) \
{ \
	CoretestsToExactStrClosure0 captures = {f, f_captures}; \
\
	captures.v.FIELD = v; \
	captures.sign = sign; \
	captures.ndigits = ndigits; \
	captures.upper = upper; \
	coretests_num_flt2dec_to_string_with_parts(coretests_num_flt2dec_to_exact_exp_str_test_to_string_closure_0##TS, \
											   &captures, NULL, result); \
} \
static StdNumfmtFormatted \
coretests_num_flt2dec_to_exact_fixed_str_test_to_string_closure_0##TS(void *captures, uint8 *buf, size_t buf_len, \
																	  StdNumfmtPart *parts, size_t parts_len) \
{ \
	CoretestsToExactStrClosure0 *c = (CoretestsToExactStrClosure0 *) captures; \
\
	return std_flt2dec_to_exact_fixed_str##TS(coretests_num_flt2dec_closure_0_0_exact, c, NULL, c->v.FIELD, c->sign, \
											  c->frac_digits, buf, buf_len, parts, parts_len); \
} \
static void \
coretests_num_flt2dec_to_exact_fixed_str_test_to_string##TS(StdFlt2decFormatExactFn f, void *f_captures, T v, \
															StdFlt2decSign sign, size_t frac_digits, \
															StdString *result) \
{ \
	CoretestsToExactStrClosure0 captures = {f, f_captures}; \
\
	captures.v.FIELD = v; \
	captures.sign = sign; \
	captures.frac_digits = frac_digits; \
	coretests_num_flt2dec_to_string_with_parts(coretests_num_flt2dec_to_exact_fixed_str_test_to_string_closure_0##TS, \
											   &captures, NULL, result); \
}

/* The exact tests' to_string at f16, f32, and f64. */
CORETESTS_TO_EXACT_STRING(F16, uint16, f16)
CORETESTS_TO_EXACT_STRING(F32, float4, f32)
CORETESTS_TO_EXACT_STRING(F64, float8, f64)

/* coretests_num_flt2dec_to_shortest_str_test is to_shortest_str_test: to_shortest_str's output through f. */
void
coretests_num_flt2dec_to_shortest_str_test(StdFlt2decFormatShortestFn f_, void *f__captures, void (*f__drop) (void *))
{
	StdFlt2decFormatShortestFn f = f_;
	void	   *f_captures = f__captures;
	volatile float8 zero = 0.0;
	StdString	actual;
	uint16		minf16;
	float4		minf32;
	float8		minf64;

	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 579, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 580, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+0", 581, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0", 582, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0", 583, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0", 584, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0", 585, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+0.0", 586, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0.00000000", 587, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0.00000000", 588, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "inf", 590, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "inf", 591, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 1.0 / zero, STD_FLT2DEC_SIGN_MINUS_PLUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+inf", 592, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 0.0 / zero, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "NaN", 593, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 0.0 / zero, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "NaN", 594, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 0.0 / zero, STD_FLT2DEC_SIGN_MINUS_PLUS, 64, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "NaN", 595, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-inf", 596, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-inf", 597, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -1.0 / zero, STD_FLT2DEC_SIGN_MINUS_PLUS, 64, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-inf", 598, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.14", 600, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.14", 601, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+3.14", 602, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14", 603, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14", 604, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14", 605, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.14", 606, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 2, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.14", 607, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 4, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+3.1400", 608, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14000000", 609, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14000000", 610, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14000000", 611, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000000000075", 613, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, 3, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000000000075", 614, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, 12, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000000000075", 615, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, 13, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0000000000750", 616, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 1.9971e20, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "199710000000000000000", 618, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 1.9971e20, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "199710000000000000000.0", 619, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 1.9971e20, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "199710000000000000000.00000000", 620, 5);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "65500", 625, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "65500.0", 626, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "65500.00000000", 627, 9);
	minf16 = coretests_num_ldexp_f16(std_f64_as_f16(1.0), -24);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000006", 630, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000006", 631, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 9, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000000060", 632, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "34028235", 31, "", 637, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "34028235", 31, ".0", 638, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "34028235", 31, ".00000000", 639, 9);
	minf32 = coretests_num_ldexp_f32(1.0f, -149);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "0.", 44, "1", 642, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 45, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "0.", 44, "1", 643, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 46, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "0.", 44, "10", 644, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "17976931348623157", 292, "", 649, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "17976931348623157", 292, ".0", 650, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "17976931348623157", 292, ".00000000", 651, 9);
	minf64 = coretests_num_ldexp_f64(1.0, -1074);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "0.", 323, "5", 657, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 324, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "0.", 323, "5", 658, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 325, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "0.", 323, "50", 659, 9);
	coretests_num_flt2dec_to_shortest_str_test_to_stringF64(f, f_captures, 1.1, STD_FLT2DEC_SIGN_MINUS, 50000, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "1.1", 49999, "", 668, 5);
	if (f__drop != NULL)
		f__drop(f__captures);
}

/* coretests_num_flt2dec_to_shortest_exp_str_test is to_shortest_exp_str_test: to_shortest_exp_str's output through f. */
void
coretests_num_flt2dec_to_shortest_exp_str_test(StdFlt2decFormatShortestFn f_, void *f__captures,
											   void (*f__drop) (void *))
{
	StdFlt2decFormatShortestFn f = f_;
	void	   *f_captures = f__captures;
	volatile float8 zero = 0.0;
	StdString	actual;
	uint16		minf16;
	float4		minf32;
	float8		minf64;

	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 687, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 688, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+0", 689, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0", 690, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0", 691, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 0, 0, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0E0", 692, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 0, 0, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0e0", 693, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 5, 9, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+0e0", 694, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS, 0, 0, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0E0", 695, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 5, 9, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0e0", 696, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 1.0 / zero, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "inf", 698, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 1.0 / zero, STD_FLT2DEC_SIGN_MINUS, -4, 16, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "inf", 699, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 1.0 / zero, STD_FLT2DEC_SIGN_MINUS_PLUS, -4, 16, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+inf", 700, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.0 / zero, STD_FLT2DEC_SIGN_MINUS, 0, 0, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "NaN", 701, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.0 / zero, STD_FLT2DEC_SIGN_MINUS, 0, 0, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "NaN", 702, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.0 / zero, STD_FLT2DEC_SIGN_MINUS_PLUS, 5, 9, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "NaN", 703, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 0, 0, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-inf", 704, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 0, 0, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-inf", 705, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -1.0 / zero, STD_FLT2DEC_SIGN_MINUS_PLUS, 5, 9, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-inf", 706, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.14", 708, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+3.14", 709, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14", 710, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14", 711, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 0, 0, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.14E0", 712, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 0, 0, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.14e0", 713, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 5, 9, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+3.14e0", 714, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 0, 0, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14E0", 715, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 0, 0, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14e0", 716, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 5, 9, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14e0", 717, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.1, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.1", 719, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.1, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.1", 720, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.1, STD_FLT2DEC_SIGN_MINUS_PLUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+0.1", 721, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -0.1, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0.1", 722, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -0.1, STD_FLT2DEC_SIGN_MINUS_PLUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0.1", 723, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.1, STD_FLT2DEC_SIGN_MINUS, 0, 0, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1E-1", 724, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.1, STD_FLT2DEC_SIGN_MINUS, 0, 0, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1e-1", 725, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 0.1, STD_FLT2DEC_SIGN_MINUS_PLUS, 5, 9, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+1e-1", 726, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -0.1, STD_FLT2DEC_SIGN_MINUS, 0, 0, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-1E-1", 727, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -0.1, STD_FLT2DEC_SIGN_MINUS, 0, 0, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-1e-1", 728, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, -0.1, STD_FLT2DEC_SIGN_MINUS_PLUS, 5, 9, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-1e-1", 729, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "7.5e-11", 731, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, -11, 10, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000000000075", 732, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, -10, 11, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "7.5e-11", 733, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 1.9971e20, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.9971e20", 735, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 1.9971e20, STD_FLT2DEC_SIGN_MINUS, -20, 21, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "199710000000000000000", 736, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 1.9971e20, STD_FLT2DEC_SIGN_MINUS, -21, 20, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.9971e20", 737, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 1.0e23, STD_FLT2DEC_SIGN_MINUS, 22, 23, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1e23", 740, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 1.0e23, STD_FLT2DEC_SIGN_MINUS, 23, 24, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "100000000000000000000000", 741, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 1.0e23, STD_FLT2DEC_SIGN_MINUS, 24, 25, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1e23", 742, 5);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, -2, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "6.55e4", 747, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, -4, 4, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "6.55e4", 748, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, -5, 5, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "65500", 749, 9);
	minf16 = coretests_num_ldexp_f16(std_f64_as_f16(1.0), -24);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, -2, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "6e-8", 752, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, -7, 7, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "6e-8", 753, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, -8, 8, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000006", 754, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.4028235e38", 759, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, -39, 38, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.4028235e38", 760, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, -38, 39, false, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "34028235", 31, "", 761, 9);
	minf32 = coretests_num_ldexp_f32(1.0f, -149);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1e-45", 764, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, -44, 45, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1e-45", 765, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, -45, 44, false, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "0.", 44, "1", 766, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.7976931348623157e308", 771, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, -308, 309, false, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "17976931348623157", 292, "", 772, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, -309, 308, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.7976931348623157e308", 776, 9);
	minf64 = coretests_num_ldexp_f64(1.0, -1074);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, -4, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "5e-324", 779, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, -324, 323, false, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "0.", 323, "5", 780, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, -323, 324, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "5e-324", 781, 9);
	coretests_num_flt2dec_to_shortest_exp_str_test_to_stringF64(f, f_captures, 1.1, STD_FLT2DEC_SIGN_MINUS, PG_INT16_MIN, PG_INT16_MAX, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.1", 783, 5);
	if (f__drop != NULL)
		f__drop(f__captures);
}

/* coretests_num_flt2dec_to_exact_exp_str_test is to_exact_exp_str_test: to_exact_exp_str's output through f. */
void
coretests_num_flt2dec_to_exact_exp_str_test(StdFlt2decFormatExactFn f_, void *f__captures, void (*f__drop) (void *))
{
	StdFlt2decFormatExactFn f = f_;
	void	   *f_captures = f__captures;
	volatile float8 zero = 0.0;
	StdString	actual;
	uint16		minf16;
	float4		minf32;
	float8		minf64;

	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 1, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0E0", 810, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0e0", 811, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+0e0", 812, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS, 1, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0E0", 813, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0e0", 814, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 2, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0E0", 815, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0e0", 816, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+0.0e0", 817, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS, 8, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0.0000000e0", 818, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 8, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0.0000000e0", 819, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "inf", 821, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 1, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "inf", 822, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0 / zero, STD_FLT2DEC_SIGN_MINUS_PLUS, 1, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+inf", 823, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.0 / zero, STD_FLT2DEC_SIGN_MINUS, 8, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "NaN", 824, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.0 / zero, STD_FLT2DEC_SIGN_MINUS, 8, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "NaN", 825, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.0 / zero, STD_FLT2DEC_SIGN_MINUS_PLUS, 8, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "NaN", 826, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 64, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-inf", 827, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 64, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-inf", 828, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -1.0 / zero, STD_FLT2DEC_SIGN_MINUS_PLUS, 64, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-inf", 829, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 1, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3E0", 831, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3e0", 832, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+3e0", 833, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 2, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.1E0", 834, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.1e0", 835, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.1e0", 836, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 3, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.14E0", 837, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 3, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.14e0", 838, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 3, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+3.14e0", 839, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 4, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.140E0", 840, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 4, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.140e0", 841, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 4, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.140e0", 842, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.195, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "2e-1", 844, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.195, STD_FLT2DEC_SIGN_MINUS, 1, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "2E-1", 845, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.195, STD_FLT2DEC_SIGN_MINUS_PLUS, 1, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+2E-1", 846, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -0.195, STD_FLT2DEC_SIGN_MINUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-2.0e-1", 847, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -0.195, STD_FLT2DEC_SIGN_MINUS, 2, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-2.0E-1", 848, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -0.195, STD_FLT2DEC_SIGN_MINUS_PLUS, 2, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-2.0E-1", 849, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.195, STD_FLT2DEC_SIGN_MINUS, 3, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.95e-1", 850, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.195, STD_FLT2DEC_SIGN_MINUS, 3, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.95E-1", 851, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.195, STD_FLT2DEC_SIGN_MINUS_PLUS, 3, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+1.95E-1", 852, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -0.195, STD_FLT2DEC_SIGN_MINUS, 4, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-1.950e-1", 853, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -0.195, STD_FLT2DEC_SIGN_MINUS, 4, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-1.950E-1", 854, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, -0.195, STD_FLT2DEC_SIGN_MINUS_PLUS, 4, true, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-1.950E-1", 855, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 9.5, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1e1", 857, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 9.5, STD_FLT2DEC_SIGN_MINUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "9.5e0", 858, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 9.5, STD_FLT2DEC_SIGN_MINUS, 3, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "9.50e0", 859, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 9.5, STD_FLT2DEC_SIGN_MINUS, 30, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "9.50000000000000000000000000000e0", 860, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1e25", 862, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.0e25", 863, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 15, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.00000000000000e25", 864, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.000000000000000e25", 865, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 17, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.0000000000000001e25", 866, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 18, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.00000000000000009e25", 867, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 19, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.000000000000000091e25", 868, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 20, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.0000000000000000906e25", 869, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 21, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.00000000000000009060e25", 870, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 22, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.000000000000000090597e25", 871, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 23, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.0000000000000000905970e25", 872, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 24, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.00000000000000009059697e25", 873, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 25, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.000000000000000090596966e25", 874, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 26, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.0000000000000000905969664e25", 875, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 27, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.00000000000000009059696640e25", 876, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 30, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.00000000000000009059696640000e25", 877, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1e-6", 879, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.0e-6", 880, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.000000000000000e-6", 881, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 17, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "9.9999999999999995e-7", 882, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 18, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "9.99999999999999955e-7", 883, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 19, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "9.999999999999999547e-7", 884, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 20, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "9.9999999999999995475e-7", 885, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 30, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "9.99999999999999954748111825886e-7", 886, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 40, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "9.999999999999999547481118258862586856139e-7", 887, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 50, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "9.9999999999999995474811182588625868561393872369081e-7", 891, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 60, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "9.99999999999999954748111825886258685613938723690807819366455e-7", 895, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 70, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "9.999999999999999547481118258862586856139387236908078193664550781250000e-7", 899, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "7e4", 906, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "6.6e4", 907, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, 4, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "6.550e4", 908, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, 5, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "6.5504e4", 909, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, 6, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "6.55040e4", 910, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "6.550400000000000e4", 911, 9);
	minf16 = coretests_num_ldexp_f16(std_f64_as_f16(1.0), -24);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "6e-8", 914, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "6.0e-8", 915, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 4, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "5.960e-8", 916, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 8, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "5.9604645e-8", 917, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "5.960464477539062e-8", 918, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 17, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "5.9604644775390625e-8", 919, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 18, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "5.96046447753906250e-8", 920, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 24, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "5.96046447753906250000000e-8", 921, 9);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3e38", 924, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.4e38", 925, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 4, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.403e38", 926, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 8, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.4028235e38", 927, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.402823466385289e38", 928, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 32, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.4028234663852885981170418348452e38", 929, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 64, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.402823466385288598117041834845169254400000000000000000000000000e38", 930, 5);
	minf32 = coretests_num_ldexp_f32(1.0f, -149);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1e-45", 936, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.4e-45", 937, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 4, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.401e-45", 938, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 8, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.4012985e-45", 939, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.401298464324817e-45", 940, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 32, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.4012984643248170709237295832899e-45", 941, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 64, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.401298464324817070923729583289916131280261941876515771757068284e-45", 942, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 128, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.40129846432481707092372958328991613128026194187651577175706828388979108268586060148663818836212158" "20312500000000000000000000000e-45", 946, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "2e308", 957, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.8e308", 958, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 4, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.798e308", 959, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 8, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.7976931e308", 960, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.797693134862316e308", 961, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 32, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.7976931348623157081452742373170e308", 962, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 64, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.797693134862315708145274237317043567980705675258449965989174768e308", 963, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 128, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.79769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878" "17154045895351438246423432133e308", 967, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 256, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.79769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878" "1715404589535143824642343213268894641827684675467035375169860499105765512820762454900903893289440758" "685084551339423045832369032229481658085593321233482747978e308", 972, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 512, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1.79769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878" "1715404589535143824642343213268894641827684675467035375169860499105765512820762454900903893289440758" "6850845513394230458323690322294816580855933212334827479782620414472316873817718091929988125040402618" "4124858368000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" "0000000000000e308", 979, 5);
	minf64 = coretests_num_ldexp_f64(1.0, -1074);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 1, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "5e-324", 993, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 2, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "4.9e-324", 994, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 4, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "4.941e-324", 995, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 8, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "4.9406565e-324", 996, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 16, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "4.940656458412465e-324", 997, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 32, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "4.9406564584124654417656879286822e-324", 998, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 64, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "4.940656458412465441765687928682213723650598026143247644255856825e-324", 999, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 128, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "4.94065645841246544176568792868221372365059802614324764425585682500675507270208751865299836361635992" "37979656469544571773092665671e-324", 1003, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 256, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "4.94065645841246544176568792868221372365059802614324764425585682500675507270208751865299836361635992" "3797965646954457177309266567103559397963987747960107818781263007131903114045278458171678489821036887" "186360569987307230500063874091535649843873124733972731696e-324", 1008, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 512, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "4.94065645841246544176568792868221372365059802614324764425585682500675507270208751865299836361635992" "3797965646954457177309266567103559397963987747960107818781263007131903114045278458171678489821036887" "1863605699873072305000638740915356498438731247339727316961514003171538539807412623856559117102665855" "6686768187039560310624931945271591492455329305456544401127480129709999541931989409080416563324524757" "1478690147267801593552386115501348035264934720193790268107107491703332226844753335720832431936092382" "8934583680601e-324", 1015, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 1024, false, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "4.94065645841246544176568792868221372365059802614324764425585682500675507270208751865299836361635992" "3797965646954457177309266567103559397963987747960107818781263007131903114045278458171678489821036887" "1863605699873072305000638740915356498438731247339727316961514003171538539807412623856559117102665855" "6686768187039560310624931945271591492455329305456544401127480129709999541931989409080416563324524757" "1478690147267801593552386115501348035264934720193790268107107491703332226844753335720832431936092382" "8934583680601060115061698097530783422773183292479049825247307763759272478746560847782037344696995336" "4701797267771758512566055119913150489110145103786273816725095583738973359899366480994116420570263709" "0279242767544565229087538682506419718265533447265625000000000000000000000000000000000000000000000000" "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" "0000000000000000000000000e-324", 1026, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 50000, false, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "0.", 49999, "e0", 1047, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e1, STD_FLT2DEC_SIGN_MINUS, 50000, false, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "1.", 49999, "e1", 1048, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e0, STD_FLT2DEC_SIGN_MINUS, 50000, false, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "1.", 49999, "e0", 1049, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-1, STD_FLT2DEC_SIGN_MINUS, 50000, false, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "1.000000000000000055511151231257827021181583404541015625", 49945, "e-1", 1050, 5);
	coretests_num_flt2dec_to_exact_exp_str_test_to_stringF64(f, f_captures, 1.0e-20, STD_FLT2DEC_SIGN_MINUS, 50000, false, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "9.99999999999999945153271454209571651729503702787392447107715776066783064379706047475337982177734375", 49901, "e-21", 1058, 5);
	if (f__drop != NULL)
		f__drop(f__captures);
}

/* coretests_num_flt2dec_to_exact_fixed_str_test is to_exact_fixed_str_test: to_exact_fixed_str's output through f. */
void
coretests_num_flt2dec_to_exact_fixed_str_test(StdFlt2decFormatExactFn f_, void *f__captures, void (*f__drop) (void *))
{
	StdFlt2decFormatExactFn f = f_;
	void	   *f_captures = f__captures;
	volatile float8 zero = 0.0;
	StdString	actual;
	uint16		minf16;
	float4		minf32;
	float8		minf64;

	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 1084, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+0", 1085, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0", 1086, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0", 1087, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0", 1088, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+0.0", 1089, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0.00000000", 1090, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -0.0, STD_FLT2DEC_SIGN_MINUS_PLUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0.00000000", 1091, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "inf", 1093, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "inf", 1094, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0 / zero, STD_FLT2DEC_SIGN_MINUS_PLUS, 64, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+inf", 1095, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0 / zero, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "NaN", 1096, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0 / zero, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "NaN", 1097, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0 / zero, STD_FLT2DEC_SIGN_MINUS_PLUS, 64, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "NaN", 1098, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-inf", 1099, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -1.0 / zero, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-inf", 1100, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -1.0 / zero, STD_FLT2DEC_SIGN_MINUS_PLUS, 64, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-inf", 1101, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3", 1103, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3", 1104, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+3", 1105, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3", 1106, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3", 1107, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3", 1108, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.1", 1109, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS, 2, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "3.14", 1110, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 4, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+3.1400", 1111, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14000000", 1112, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14000000", 1113, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -3.14, STD_FLT2DEC_SIGN_MINUS_PLUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-3.14000000", 1114, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.195, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 1116, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.195, STD_FLT2DEC_SIGN_MINUS_PLUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+0", 1117, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -0.195, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0", 1118, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -0.195, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0", 1119, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -0.195, STD_FLT2DEC_SIGN_MINUS_PLUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0", 1120, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.195, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.2", 1121, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.195, STD_FLT2DEC_SIGN_MINUS, 2, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.20", 1122, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.195, STD_FLT2DEC_SIGN_MINUS_PLUS, 4, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "+0.1950", 1123, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -0.195, STD_FLT2DEC_SIGN_MINUS, 5, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0.19500", 1124, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -0.195, STD_FLT2DEC_SIGN_MINUS, 6, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0.195000", 1125, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, -0.195, STD_FLT2DEC_SIGN_MINUS_PLUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "-0.19500000", 1126, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 999.5, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1000", 1128, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 999.5, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "999.5", 1129, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 999.5, STD_FLT2DEC_SIGN_MINUS, 2, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "999.50", 1130, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 999.5, STD_FLT2DEC_SIGN_MINUS, 3, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "999.500", 1131, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 999.5, STD_FLT2DEC_SIGN_MINUS, 30, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "999.500000000000000000000000000000", 1132, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.5, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 1134, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.5, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.5", 1135, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.5, STD_FLT2DEC_SIGN_MINUS, 2, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.50", 1136, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.5, STD_FLT2DEC_SIGN_MINUS, 3, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.500", 1137, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.95, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1", 1139, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.95, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.9", 1140, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.95, STD_FLT2DEC_SIGN_MINUS, 2, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.95", 1141, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.95, STD_FLT2DEC_SIGN_MINUS, 3, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.950", 1142, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.95, STD_FLT2DEC_SIGN_MINUS, 10, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.9500000000", 1143, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.95, STD_FLT2DEC_SIGN_MINUS, 30, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.949999999999999955591079014994", 1144, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.095, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 1146, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.095, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.1", 1147, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.095, STD_FLT2DEC_SIGN_MINUS, 2, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.10", 1148, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.095, STD_FLT2DEC_SIGN_MINUS, 3, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.095", 1149, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.095, STD_FLT2DEC_SIGN_MINUS, 4, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0950", 1150, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.095, STD_FLT2DEC_SIGN_MINUS, 10, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0950000000", 1151, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.095, STD_FLT2DEC_SIGN_MINUS, 30, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.095000000000000001110223024625", 1152, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0095, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 1154, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0095, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0", 1155, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0095, STD_FLT2DEC_SIGN_MINUS, 2, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.01", 1156, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0095, STD_FLT2DEC_SIGN_MINUS, 3, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.009", 1157, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0095, STD_FLT2DEC_SIGN_MINUS, 4, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0095", 1158, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0095, STD_FLT2DEC_SIGN_MINUS, 5, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00950", 1159, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0095, STD_FLT2DEC_SIGN_MINUS, 10, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0095000000", 1160, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0095, STD_FLT2DEC_SIGN_MINUS, 30, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.009499999999999999764077607267", 1161, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 1163, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, 3, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000", 1164, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, 10, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0000000001", 1165, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, 11, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000000007", 1166, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, 12, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000000000075", 1167, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, 13, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0000000000750", 1168, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, 20, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000000007500000000", 1169, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 7.5e-11, STD_FLT2DEC_SIGN_MINUS, 30, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000000000074999999999999999501", 1170, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "10000000000000000905969664", 1172, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "10000000000000000905969664.0", 1173, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e25, STD_FLT2DEC_SIGN_MINUS, 3, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "10000000000000000905969664.000", 1174, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 1176, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 3, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000", 1177, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 6, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000001", 1178, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 9, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000001000", 1179, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 12, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000001000000", 1180, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 22, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0000010000000000000000", 1181, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 23, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000099999999999999995", 1182, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 24, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000000999999999999999955", 1183, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 25, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0000009999999999999999547", 1184, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 35, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000099999999999999995474811182589", 1185, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 45, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000000999999999999999954748111825886258685614", 1186, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 55, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0000009999999999999999547481118258862586856139387236908", 1187, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 65, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000099999999999999995474811182588625868561393872369080781936646", 1191, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-6, STD_FLT2DEC_SIGN_MINUS, 75, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000000999999999999999954748111825886258685613938723690807819366455078125000", 1195, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "65504", 1202, 9);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "65504.0", 1203, 9);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, CORETESTS_F16_MAX, STD_FLT2DEC_SIGN_MINUS, 2, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "65504.00", 1204, 9);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "340282346638528859811704183484516925440", 1207, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "340282346638528859811704183484516925440.0", 1208, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, FLT_MAX, STD_FLT2DEC_SIGN_MINUS, 2, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "340282346638528859811704183484516925440.00", 1209, 5);
	minf16 = coretests_num_ldexp_f16(std_f64_as_f16(1.0), -24);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 1219, 9);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0", 1220, 9);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 2, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00", 1221, 9);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 4, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0000", 1222, 9);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000006", 1223, 9);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 10, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0000000596", 1224, 9);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 15, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000000059604645", 1225, 9);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 20, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000005960464477539", 1226, 9);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 24, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.000000059604644775390625", 1227, 9);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF16(f, f_captures, minf16, STD_FLT2DEC_SIGN_MINUS, 32, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000005960464477539062500000000", 1228, 9);
	minf32 = coretests_num_ldexp_f32(1.0f, -149);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 1232, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0", 1233, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 2, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00", 1234, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 4, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0000", 1235, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 8, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000000", 1236, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 16, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0000000000000000", 1237, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 32, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000000000000000000000000000000", 1238, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 64, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0000000000000000000000000000000000000000000014012984643248170709", 1239, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 128, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000000000000000000000000000000000000000000140129846432481707092372958328991613128026194187651577" "175706828388979108268586060149", 1243, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF32(f, f_captures, minf32, STD_FLT2DEC_SIGN_MINUS, 256, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000000000000000000000000000000000000000000140129846432481707092372958328991613128026194187651577" "1757068283889791082685860601486638188362121582031250000000000000000000000000000000000000000000000000" "0000000000000000000000000000000000000000000000000000000000", 1248, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1797693134862315708145274237317043567980705675258449965989174768031572607800285387605895586327668781" "7154045895351438246423432132688946418276846754670353751698604991057655128207624549009038932894407586" "8508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184" "124858368", 1256, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, DBL_MAX, STD_FLT2DEC_SIGN_MINUS, 10, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "1797693134862315708145274237317043567980705675258449965989174768031572607800285387605895586327668781" "7154045895351438246423432132688946418276846754670353751698604991057655128207624549009038932894407586" "8508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184" "124858368.0000000000", 1264, 5);
	minf64 = coretests_num_ldexp_f64(1.0, -1074);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 0, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0", 1274, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 1, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0", 1275, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 10, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.0000000000", 1276, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 100, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" "00", 1277, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, minf64, STD_FLT2DEC_SIGN_MINUS, 1000, &actual);
	coretests_num_flt2dec_assert_eq_str(&actual, "0.00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" "0000000000000000000000000494065645841246544176568792868221372365059802614324764425585682500675507270" "2087518652998363616359923797965646954457177309266567103559397963987747960107818781263007131903114045" "2784581716784898210368871863605699873072305000638740915356498438731247339727316961514003171538539807" "4126238565591171026658556686768187039560310624931945271591492455329305456544401127480129709999541931" "9894090804165633245247571478690147267801593552386115501348035264934720193790268107107491703332226844" "7533357208324319360923828934583680601060115061698097530783422773183292479049825247307763759272478746" "5608477820373446969953364701797267771758512566055119913150489110145103786273816725095583738973359899" "37", 1282, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 0.0, STD_FLT2DEC_SIGN_MINUS, 50000, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "0.", 50000, "", 1303, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e1, STD_FLT2DEC_SIGN_MINUS, 50000, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "10.", 50000, "", 1304, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e0, STD_FLT2DEC_SIGN_MINUS, 50000, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "1.", 50000, "", 1305, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-1, STD_FLT2DEC_SIGN_MINUS, 50000, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "0.1000000000000000055511151231257827021181583404541015625", 49945, "", 1306, 5);
	coretests_num_flt2dec_to_exact_fixed_str_test_to_stringF64(f, f_captures, 1.0e-20, STD_FLT2DEC_SIGN_MINUS, 50000, &actual);
	coretests_num_flt2dec_assert_eq_format(&actual, "0.00000000000000000000999999999999999945153271454209571651729503702787392447107715776066783064379706" "047475337982177734375", 49881, "", 1310, 5);
	if (f__drop != NULL)
		f__drop(f__captures);
}
