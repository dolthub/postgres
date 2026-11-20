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
 * The C form of core's float formatting at 1.99.1 (the port text's section
 * 9): flt2dec's decoder, estimator, digit layouts, and Grisu's and Dragon's
 * shortest and exact strategies, with the Big32x40 bignum, diy_float's Fp,
 * and num::fmt's parts they use.
 */
#ifndef DOLTGRES_STD_FLT2DEC_H
#define DOLTGRES_STD_FLT2DEC_H

#include "doltgres/std/cmp.h"
#include "doltgres/std/str.h"

/* STD_FLT2DEC_MAX_SIG_DIGITS is flt2dec::MAX_SIG_DIGITS: the buffer the shortest mode needs. */
#define STD_FLT2DEC_MAX_SIG_DIGITS 17

/* StdFp is diy_float::Fp: f * 2^e. */
typedef struct StdFp
{
	uint64		f;
	int16		e;
} StdFp;

/*
 * StdDecoded is flt2dec::decoder::Decoded: mant * 2^exp, with every value
 * from (mant - minus) * 2^exp to (mant + plus) * 2^exp rounding to it,
 * inclusive when inclusive is true.
 */
typedef struct StdDecoded
{
	uint64		mant;
	uint64		minus;
	uint64		plus;
	int16		exp;
	bool		inclusive;
} StdDecoded;

/* StdFullDecodedKind is FullDecoded's variant. */
typedef enum StdFullDecodedKind
{
	STD_FULL_DECODED_NAN,
	STD_FULL_DECODED_INFINITE,
	STD_FULL_DECODED_ZERO,
	STD_FULL_DECODED_FINITE,
} StdFullDecodedKind;

/* StdFullDecoded is flt2dec::decoder::FullDecoded: finite holds Finite's fields. */
typedef struct StdFullDecoded
{
	StdFullDecodedKind kind;
	StdDecoded	finite;
} StdFullDecoded;

/* StdFlt2decSign is flt2dec::Sign. */
typedef enum StdFlt2decSign
{
	STD_FLT2DEC_SIGN_MINUS,
	STD_FLT2DEC_SIGN_MINUS_PLUS,
} StdFlt2decSign;

/* StdNumfmtPartKind is num::fmt::Part's variant. */
typedef enum StdNumfmtPartKind
{
	STD_NUMFMT_PART_ZERO,
	STD_NUMFMT_PART_NUM,
	STD_NUMFMT_PART_COPY,
} StdNumfmtPartKind;

/* StdNumfmtPart is num::fmt::Part: zeroes, a number up to five digits, or bytes copied. */
typedef struct StdNumfmtPart
{
	StdNumfmtPartKind kind;
	union
	{
		size_t		zero;
		uint16		num;
		StdBytes	copy;
	}			u;
} StdNumfmtPart;

/* StdNumfmtFormatted is num::fmt::Formatted: a sign and the parts after it. */
typedef struct StdNumfmtFormatted
{
	StdBytes	sign;
	const StdNumfmtPart *parts;
	size_t		parts_len;
} StdNumfmtFormatted;

/* STD_BIGNUM_BIG32X40_N is Big32x40's digit count. */
#define STD_BIGNUM_BIG32X40_N 40

/* StdBignumBig32x40 is bignum::Big32x40: size digits of base, least significant first. */
typedef struct StdBignumBig32x40
{
	size_t		size;
	uint32		base[STD_BIGNUM_BIG32X40_N];
} StdBignumBig32x40;

/*
 * StdFlt2decFormatShortestFn is a format_shortest strategy taken as
 * to_shortest_str's FnMut: it writes the digits to buf, returns their
 * count, and sets the exponent.
 */
typedef size_t (*StdFlt2decFormatShortestFn) (void *captures, const StdDecoded *d, uint8 *buf, size_t buf_len,
											  int16 *result_1);

/* StdFlt2decFormatExactFn is a format_exact strategy taken as to_exact_fixed_str's FnMut. */
typedef size_t (*StdFlt2decFormatExactFn) (void *captures, const StdDecoded *d, uint8 *buf, size_t buf_len,
										   int16 limit, int16 *result_1);

/* StdTupleU64I16I16 is a (u64, i16, i16). */
typedef struct StdTupleU64I16I16
{
	uint64		f0;
	int16		f1;
	int16		f2;
} StdTupleU64I16I16;

/* STD_FLT2DEC_STRATEGY_GRISU_ALPHA is Grisu's ALPHA, the least exponent a cached power gives. */
#define STD_FLT2DEC_STRATEGY_GRISU_ALPHA (-60)

/* STD_FLT2DEC_STRATEGY_GRISU_GAMMA is Grisu's GAMMA, the greatest. */
#define STD_FLT2DEC_STRATEGY_GRISU_GAMMA (-32)

/* STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_LEN is CACHED_POW10's length. */
#define STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_LEN 81

/* STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_FIRST_E is the first cached power's binary exponent. */
#define STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_FIRST_E (-1087)

/* STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_LAST_E is the last cached power's binary exponent. */
#define STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_LAST_E 1039

extern const StdTupleU64I16I16 STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10[STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_LEN];

extern StdFp std_diy_float_Fp_mul(StdFp self, StdFp other);
extern StdFp std_diy_float_Fp_normalize(StdFp self);
extern StdFp std_diy_float_Fp_normalize_to(StdFp self, int16 e);

extern size_t std_numfmt_Part_len(const StdNumfmtPart *self);
extern bool std_numfmt_Part_write(const StdNumfmtPart *self, uint8 *out, size_t out_len, size_t *result);
extern size_t std_numfmt_Formatted_len(const StdNumfmtFormatted *self);
extern bool std_numfmt_Formatted_write(const StdNumfmtFormatted *self, uint8 *out, size_t out_len, size_t *result);

extern StdBignumBig32x40 std_bignum_Big32x40_from_small(uint32 v);
extern StdBignumBig32x40 std_bignum_Big32x40_from_u64(uint64 v);
extern bool std_bignum_Big32x40_is_zero(const StdBignumBig32x40 *self);
extern StdBignumBig32x40 *std_bignum_Big32x40_add(StdBignumBig32x40 *self, const StdBignumBig32x40 *other);
extern StdBignumBig32x40 *std_bignum_Big32x40_sub(StdBignumBig32x40 *self, const StdBignumBig32x40 *other);
extern StdBignumBig32x40 *std_bignum_Big32x40_mul_small(StdBignumBig32x40 *self, uint32 other);
extern StdBignumBig32x40 *std_bignum_Big32x40_mul_pow2(StdBignumBig32x40 *self, size_t bits);
extern StdBignumBig32x40 *std_bignum_Big32x40_mul_digits(StdBignumBig32x40 *self, const uint32 *other,
														 size_t other_len);
extern StdBignumBig32x40 *std_bignum_Big32x40_div_rem_small(StdBignumBig32x40 *self, uint32 other,
															uint32 *result_1);
extern StdOrdering std_bignum_Big32x40_Ord_cmp(const StdBignumBig32x40 *self, const StdBignumBig32x40 *other);

extern int16 std_flt2dec_estimator_estimate_scaling_factor(uint64 mant, int16 exp);
extern bool std_flt2dec_round_up(uint8 *d, size_t d_len, uint8 *result);
extern size_t std_flt2dec_digits_to_dec_str(const uint8 *buf, size_t buf_len, int16 exp, size_t frac_digits,
											StdNumfmtPart *parts, size_t parts_len);
extern size_t std_flt2dec_digits_to_exp_str(const uint8 *buf, size_t buf_len, int16 exp, size_t frac_digits,
											bool upper, StdNumfmtPart *parts, size_t parts_len);
extern StdBytes std_flt2dec_determine_sign(StdFlt2decSign sign, const StdFullDecoded *decoded, bool negative);
extern size_t std_flt2dec_estimate_max_buf_len(int16 exp);

extern int16 std_flt2dec_strategy_grisu_cached_power(int16 alpha, int16 gamma, StdFp *result_1);
extern uint8 std_flt2dec_strategy_grisu_max_pow10_no_more_than(uint32 x, uint32 *result_1);
extern bool std_flt2dec_strategy_grisu_format_shortest_opt(const StdDecoded *d, uint8 *buf, size_t buf_len,
														   size_t *result, int16 *result_1);
extern size_t std_flt2dec_strategy_grisu_format_shortest(const StdDecoded *d, uint8 *buf, size_t buf_len,
														 int16 *result_1);
extern bool std_flt2dec_strategy_grisu_format_exact_opt(const StdDecoded *d, uint8 *buf, size_t buf_len, int16 limit,
														size_t *result, int16 *result_1);
extern size_t std_flt2dec_strategy_grisu_format_exact(const StdDecoded *d, uint8 *buf, size_t buf_len, int16 limit,
													  int16 *result_1);
extern size_t std_flt2dec_strategy_grisu_format_shortest_FnMut_call_mut(void *captures, const StdDecoded *d,
																		uint8 *buf, size_t buf_len, int16 *result_1);
extern size_t std_flt2dec_strategy_grisu_format_exact_FnMut_call_mut(void *captures, const StdDecoded *d, uint8 *buf,
																	 size_t buf_len, int16 limit, int16 *result_1);

extern StdBignumBig32x40 *std_flt2dec_strategy_dragon_mul_pow10(StdBignumBig32x40 *x, size_t n);
extern size_t std_flt2dec_strategy_dragon_format_shortest(const StdDecoded *d, uint8 *buf, size_t buf_len,
														  int16 *result_1);
extern size_t std_flt2dec_strategy_dragon_format_exact(const StdDecoded *d, uint8 *buf, size_t buf_len, int16 limit,
													   int16 *result_1);

/* core's flt2dec generics at f32, which f32's Debug formats with. */
#define STD_FLT2DEC_SCOPE extern
#define STD_FLT2DEC_DECLARE
#define STD_FLT2DEC_TS F32
#define STD_FLT2DEC_T float4
#include "doltgres/std/flt2dec_template.h"

/* core's flt2dec generics at f64, which f64's Debug formats with. */
#define STD_FLT2DEC_SCOPE extern
#define STD_FLT2DEC_DECLARE
#define STD_FLT2DEC_TS F64
#define STD_FLT2DEC_T float8
#include "doltgres/std/flt2dec_template.h"

#endif							/* DOLTGRES_STD_FLT2DEC_H */
