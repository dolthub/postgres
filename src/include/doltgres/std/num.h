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
 * The C forms of Rust's integer operators (the port text's section 3): each
 * returns its Rust type, panics with Rust's message at the Rust site's
 * location where the Rust's assert builds check overflow, and wraps as
 * release Rust does otherwise; division and remainder check in every build.
 */
#ifndef DOLTGRES_STD_NUM_H
#define DOLTGRES_STD_NUM_H

#include <math.h>

#include "common/int.h"
#include "port/pg_bitutils.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* The port text's assumptions: 64-bit targets, two's complement, arithmetic >>. */
StaticAssertDecl(sizeof(size_t) == 8, "the port assumes 64-bit targets");
StaticAssertDecl((int8) 0xff == -1, "the port assumes two's complement");
StaticAssertDecl((-2 >> 1) == -1, "the port assumes an arithmetic right shift");

/* STD_OVERFLOW panics with one of Rust's overflow messages at a location. */
#define STD_OVERFLOW(file, line, col, literal) \
	std_panic((file), (line), (col), (literal), sizeof(literal) - 1)

/*
 * STD_UNSIGNED defines the operators of an unsigned type: add, subtract,
 * multiply, and the shifts, checked under USE_ASSERT_CHECKING and wrapping
 * otherwise.
 */
#ifdef USE_ASSERT_CHECKING
#define STD_CHECKED(cond, file, line, col, literal) \
	do { if (unlikely(cond)) STD_OVERFLOW(file, line, col, literal); } while (0)
#else
#define STD_CHECKED(cond, file, line, col, literal) ((void) 0)
#endif

#define STD_UNSIGNED(t, T, bits) \
static inline T \
std_add_##t(T a, T b, const char *file, uint32 line, uint32 col) \
{ \
	T r = (T) (a + b); \
	STD_CHECKED(r < a, file, line, col, "attempt to add with overflow"); \
	return r; \
} \
static inline T \
std_sub_##t(T a, T b, const char *file, uint32 line, uint32 col) \
{ \
	STD_CHECKED(b > a, file, line, col, "attempt to subtract with overflow"); \
	return (T) (a - b); \
} \
static inline T \
std_mul_##t(T a, T b, const char *file, uint32 line, uint32 col) \
{ \
	uint64 r = (uint64) a * (uint64) b; \
	STD_CHECKED(bits == 64 ? (a != 0 && r / a != b) : r > (uint64) (T) ~(T) 0, \
				file, line, col, "attempt to multiply with overflow"); \
	return (T) r; \
} \
static inline T \
std_shl_##t(T a, uint64 n, const char *file, uint32 line, uint32 col) \
{ \
	STD_CHECKED(n >= bits, file, line, col, "attempt to shift left with overflow"); \
	return (T) (a << (n & (bits - 1))); \
} \
static inline T \
std_shr_##t(T a, uint64 n, const char *file, uint32 line, uint32 col) \
{ \
	STD_CHECKED(n >= bits, file, line, col, "attempt to shift right with overflow"); \
	return (T) (a >> (n & (bits - 1))); \
} \
static inline T \
std_div_##t(T a, T b, const char *file, uint32 line, uint32 col) \
{ \
	if (b == 0) \
		STD_OVERFLOW(file, line, col, "attempt to divide by zero"); \
	return (T) (a / b); \
} \
static inline T \
std_rem_##t(T a, T b, const char *file, uint32 line, uint32 col) \
{ \
	if (b == 0) \
		STD_OVERFLOW(file, line, col, "attempt to calculate the remainder with a divisor of zero"); \
	return (T) (a % b); \
} \
static inline T \
std_shl_folded_##t(T a, uint64 n) \
{ \
	return (T) (a << n); \
} \
static inline T \
std_shr_folded_##t(T a, uint64 n) \
{ \
	return (T) (a >> n); \
} \
static inline T \
std_div_folded_##t(T a, T b) \
{ \
	return (T) (a / b); \
} \
static inline T \
std_rem_folded_##t(T a, T b) \
{ \
	return (T) (a % b); \
} \
static inline T \
std_wrapping_add_##t(T a, T b) \
{ \
	return (T) (a + b); \
} \
static inline T \
std_wrapping_sub_##t(T a, T b) \
{ \
	return (T) (a - b); \
} \
static inline T \
std_wrapping_mul_##t(T a, T b) \
{ \
	return (T) ((uint64) a * (uint64) b); \
} \
static inline T \
std_saturating_add_##t(T a, T b) \
{ \
	T r = (T) (a + b); \
	return r < a ? (T) ~(T) 0 : r; \
} \
static inline T \
std_saturating_sub_##t(T a, T b) \
{ \
	return b > a ? 0 : (T) (a - b); \
} \
static inline bool \
std_checked_add_##t(T a, T b, T *result) \
{ \
	T			r = (T) (a + b); \
\
	if (r < a) \
		return false; \
	*result = r; \
	return true; \
} \
static inline bool \
std_checked_sub_##t(T a, T b, T *result) \
{ \
	if (b > a) \
		return false; \
	*result = (T) (a - b); \
	return true; \
} \
static inline bool \
std_checked_mul_##t(T a, T b, T *result) \
{ \
	T			r = (T) ((uint64) a * (uint64) b); \
\
	if (a != 0 && r / a != b) \
		return false; \
	*result = r; \
	return true; \
} \
static inline T \
std_saturating_mul_##t(T a, T b) \
{ \
	T			r; \
\
	return std_checked_mul_##t(a, b, &r) ? r : (T) ~(T) 0; \
}

STD_UNSIGNED(u8, uint8, 8)
STD_UNSIGNED(u16, uint16, 16)
STD_UNSIGNED(u32, uint32, 32)
STD_UNSIGNED(u64, uint64, 64)
STD_UNSIGNED(usize, size_t, 64)

/*
 * STD_SIGNED defines the operators of a signed type, computed in the
 * unsigned type of its width so that MSVC, which has no -fwrapv, wraps too.
 */
#define STD_SIGNED(t, T, U, bits, MIN) \
static inline T \
std_add_##t(T a, T b, const char *file, uint32 line, uint32 col) \
{ \
	T r = (T) (U) ((U) a + (U) b); \
	STD_CHECKED((b >= 0) ? (r < a) : (r > a), file, line, col, "attempt to add with overflow"); \
	return r; \
} \
static inline T \
std_sub_##t(T a, T b, const char *file, uint32 line, uint32 col) \
{ \
	T r = (T) (U) ((U) a - (U) b); \
	STD_CHECKED((b >= 0) ? (r > a) : (r < a), file, line, col, "attempt to subtract with overflow"); \
	return r; \
} \
static inline T \
std_mul_##t(T a, T b, const char *file, uint32 line, uint32 col) \
{ \
	T r = (T) (U) ((uint64) (U) a * (uint64) (U) b); \
	STD_CHECKED(a != 0 && ((a == -1 && b == MIN) || (b == -1 && a == MIN) || r / a != b), \
				file, line, col, "attempt to multiply with overflow"); \
	return r; \
} \
static inline T \
std_neg_##t(T a, const char *file, uint32 line, uint32 col) \
{ \
	STD_CHECKED(a == MIN, file, line, col, "attempt to negate with overflow"); \
	return (T) (U) (0 - (U) a); \
} \
static inline T \
std_shl_##t(T a, uint64 n, const char *file, uint32 line, uint32 col) \
{ \
	STD_CHECKED(n >= bits, file, line, col, "attempt to shift left with overflow"); \
	return (T) (U) ((U) a << (n & (bits - 1))); \
} \
static inline T \
std_shr_##t(T a, uint64 n, const char *file, uint32 line, uint32 col) \
{ \
	STD_CHECKED(n >= bits, file, line, col, "attempt to shift right with overflow"); \
	return (T) (a >> (n & (bits - 1))); \
} \
static inline T \
std_div_##t(T a, T b, const char *file, uint32 line, uint32 col) \
{ \
	if (b == 0) \
		STD_OVERFLOW(file, line, col, "attempt to divide by zero"); \
	if (a == MIN && b == -1) \
		STD_OVERFLOW(file, line, col, "attempt to divide with overflow"); \
	return (T) (a / b); \
} \
static inline T \
std_rem_##t(T a, T b, const char *file, uint32 line, uint32 col) \
{ \
	if (b == 0) \
		STD_OVERFLOW(file, line, col, "attempt to calculate the remainder with a divisor of zero"); \
	if (a == MIN && b == -1) \
		STD_OVERFLOW(file, line, col, "attempt to calculate the remainder with overflow"); \
	return (T) (a % b); \
} \
static inline T \
std_shl_folded_##t(T a, uint64 n) \
{ \
	return (T) (U) ((U) a << n); \
} \
static inline T \
std_shr_folded_##t(T a, uint64 n) \
{ \
	return (T) (a >> n); \
} \
static inline T \
std_div_folded_##t(T a, T b) \
{ \
	return (T) (a / b); \
} \
static inline T \
std_rem_folded_##t(T a, T b) \
{ \
	return (T) (a % b); \
} \
static inline T \
std_wrapping_add_##t(T a, T b) \
{ \
	return (T) (U) ((U) a + (U) b); \
} \
static inline T \
std_wrapping_sub_##t(T a, T b) \
{ \
	return (T) (U) ((U) a - (U) b); \
} \
static inline bool \
std_checked_add_##t(T a, T b, T *result) \
{ \
	T			r = (T) (U) ((U) a + (U) b); \
\
	if (((a ^ r) & (b ^ r)) < 0) \
		return false; \
	*result = r; \
	return true; \
} \
static inline bool \
std_checked_sub_##t(T a, T b, T *result) \
{ \
	T			r = (T) (U) ((U) a - (U) b); \
\
	if (((a ^ b) & (a ^ r)) < 0) \
		return false; \
	*result = r; \
	return true; \
} \
static inline bool \
std_checked_mul_##t(T a, T b, T *result) \
{ \
	T			r = (T) (U) ((uint64) (U) a * (uint64) (U) b); \
\
	if (a != 0 && ((a == -1 && b == MIN) || (b == -1 && a == MIN) || r / a != b)) \
		return false; \
	*result = r; \
	return true; \
}

STD_SIGNED(i8, int8, uint8, 8, PG_INT8_MIN)
STD_SIGNED(i16, int16, uint16, 16, PG_INT16_MIN)
STD_SIGNED(i32, int32, uint32, 32, PG_INT32_MIN)
STD_SIGNED(i64, int64, uint64, 64, PG_INT64_MIN)
STD_SIGNED(isize, ssize_t, size_t, 64, PG_INT64_MIN)

/*
 * STD_ORD defines Ord's min and max of an integer type, as core's defaults
 * through min_by and max_by are: min keeps the first of equal values, and max
 * the second.
 */
#define STD_ORD(t, T) \
static inline T \
std_min_##t(T a, T b) \
{ \
	return b < a ? b : a; \
} \
static inline T \
std_max_##t(T a, T b) \
{ \
	return b < a ? a : b; \
}

STD_ORD(u8, uint8)
STD_ORD(u16, uint16)
STD_ORD(u32, uint32)
STD_ORD(u64, uint64)
STD_ORD(usize, size_t)
STD_ORD(i8, int8)
STD_ORD(i16, int16)
STD_ORD(i32, int32)
STD_ORD(i64, int64)
STD_ORD(isize, ssize_t)

/* The operator macros, which pass the file's RUST_FILE and the Rust site's line and column. */
#define STD_ADD(t, a, b, line, col) std_add_##t((a), (b), RUST_FILE, (line), (col))
#define STD_SUB(t, a, b, line, col) std_sub_##t((a), (b), RUST_FILE, (line), (col))
#define STD_MUL(t, a, b, line, col) std_mul_##t((a), (b), RUST_FILE, (line), (col))
#define STD_NEG(t, a, line, col) std_neg_##t((a), RUST_FILE, (line), (col))
#define STD_SHL(t, a, n, line, col) std_shl_##t((a), (n), RUST_FILE, (line), (col))
#define STD_SHR(t, a, n, line, col) std_shr_##t((a), (n), RUST_FILE, (line), (col))
#define STD_DIV(t, a, b, line, col) std_div_##t((a), (b), RUST_FILE, (line), (col))
#define STD_REM(t, a, b, line, col) std_rem_##t((a), (b), RUST_FILE, (line), (col))

/*
 * The folded operator macros: a shift by a constant below the width, and a
 * division or remainder by a constant other than 0 and -1, whose check rustc
 * folds away, so the C computes in the Rust type with no check.
 */
#define STD_SHL_FOLDED(t, a, n) std_shl_folded_##t((a), (n))
#define STD_SHR_FOLDED(t, a, n) std_shr_folded_##t((a), (n))
#define STD_DIV_FOLDED(t, a, b) std_div_folded_##t((a), (b))
#define STD_REM_FOLDED(t, a, b) std_rem_folded_##t((a), (b))

/* std_div_ceil_usize is usize::div_ceil, whose division panics at its caller. */
static inline size_t
std_div_ceil_usize(size_t a, size_t b, const char *file, uint32 line, uint32 col)
{
	size_t		d = std_div_usize(a, b, file, line, col);
	size_t		r = std_rem_usize(a, b, file, line, col);

	return r > 0 ? d + 1 : d;
}

/* std_div_ceil_u64 is u64::div_ceil, whose division panics at its caller. */
static inline uint64
std_div_ceil_u64(uint64 a, uint64 b, const char *file, uint32 line, uint32 col)
{
	uint64		d = std_div_u64(a, b, file, line, col);
	uint64		r = std_rem_u64(a, b, file, line, col);

	return r > 0 ? d + 1 : d;
}

/* std_rotate_left_u64 is u64::rotate_left: the bits shifted left, those shifted out coming in on the right. */
static inline uint64
std_rotate_left_u64(uint64 x, uint32 n)
{
	n &= 63;
	return n == 0 ? x : (x << n) | (x >> (64 - n));
}

/* std_rotate_right_u64 is u64::rotate_right: the bits shifted right, those shifted out coming in on the left. */
static inline uint64
std_rotate_right_u64(uint64 x, uint32 n)
{
	n &= 63;
	return n == 0 ? x : (x >> n) | (x << (64 - n));
}

/* std_rotate_right_u32 is u32::rotate_right: the bits shifted right, those shifted out coming in on the left. */
static inline uint32
std_rotate_right_u32(uint32 x, uint32 n)
{
	n &= 31;
	return n == 0 ? x : (x >> n) | (x << (32 - n));
}

/* std_as_f64_usize is `x as usize` for an f64 x, saturating: NaN and anything at or below zero give 0, anything past usize's range its maximum. */
static inline size_t
std_as_f64_usize(float8 x)
{
	if (isnan(x) || x <= 0)
		return 0;
	if (x >= 18446744073709551616.0)
		return SIZE_MAX;
	return (size_t) x;
}

/* std_as_f64_i32 is `x as i32`, saturating: NaN gives 0, and anything else is clamped to i32's range and truncated. */
static inline int32
std_as_f64_i32(float8 x)
{
	if (isnan(x))
		return 0;
	if (x <= (float8) PG_INT32_MIN)
		return PG_INT32_MIN;
	if (x >= (float8) PG_INT32_MAX)
		return PG_INT32_MAX;
	return (int32) x;
}

/* std_f64_round is f64::round, the nearest integer, halves away from zero. */
static inline float8
std_f64_round(float8 x)
{
	return round(x);
}

/* std_f64_abs is f64::abs, the magnitude. */
static inline float8
std_f64_abs(float8 x)
{
	return fabs(x);
}

/* std_f64_max is f64::max, maxNum: the larger, or the other operand when one is NaN. */
static inline float8
std_f64_max(float8 a, float8 b)
{
	return fmax(a, b);
}

/* std_f64_min is f64::min, minNum: the smaller, or the other operand when one is NaN. */
static inline float8
std_f64_min(float8 a, float8 b)
{
	return fmin(a, b);
}

/* std_f64_is_normal is f64::is_normal: neither zero, subnormal, infinite, nor NaN. */
static inline bool
std_f64_is_normal(float8 x)
{
	return fpclassify(x) == FP_NORMAL;
}

/*
 * std_checked_sub_unsigned_i64 is i64::checked_sub_unsigned: core's
 * overflowing_sub_unsigned, the unsigned operand cast to i64 and subtracted,
 * its overflow flipped for a negative cast, and nothing on overflow.
 */
static inline bool
std_checked_sub_unsigned_i64(int64 a, uint64 b, int64 *result)
{
	int64		rhs = (int64) b;
	int64		res = (int64) ((uint64) a - (uint64) rhs);
	bool		overflowed = ((a ^ rhs) & (a ^ res)) < 0;

	if (overflowed ^ (rhs < 0))
		return false;
	*result = res;
	return true;
}

/*
 * std_checked_add_unsigned_i64 is i64::checked_add_unsigned: core's
 * overflowing_add_unsigned, the unsigned operand cast to i64 and added, its
 * overflow flipped for a negative cast, and nothing on overflow.
 */
static inline bool
std_checked_add_unsigned_i64(int64 a, uint64 b, int64 *result)
{
	int64		rhs = (int64) b;
	int64		res = (int64) ((uint64) a + (uint64) rhs);
	bool		overflowed = ((a ^ res) & (rhs ^ res)) < 0;

	if (overflowed ^ (rhs < 0))
		return false;
	*result = res;
	return true;
}

/*
 * std_ilog2_usize is usize::ilog2, #[track_caller]: the base 2 logarithm
 * rounded down, BITS - 1 less the leading zeros, and for zero core's
 * panic_for_nonpositive_argument at its caller.
 */
static inline uint32
std_ilog2_usize(size_t self, const char *file, uint32 line, uint32 col)
{
	uint32		leading_zeros = 0;

	if (self == 0)
	{
		static const char text[] = "argument of integer logarithm must be positive";

		std_panic(file, line, col, text, sizeof(text) - 1);
	}
	while ((self & ((size_t) 1 << 63)) == 0)
	{
		self <<= 1;
		leading_zeros++;
	}
	return 64 - 1 - leading_zeros;
}

/* std_leading_zeros_u64 is u64::leading_zeros: the zero bits above the highest one, 64 for zero. */
static inline uint32
std_leading_zeros_u64(uint64 self)
{
	uint32		n = 0;

	while (n < 64 && (self & ((uint64) 1 << (63 - n))) == 0)
		n++;
	return n;
}

/* std_trailing_zeros_u64 is u64::trailing_zeros: the zero bits below the lowest one, 64 for zero. */
static inline uint32
std_trailing_zeros_u64(uint64 self)
{
	uint32		n = 0;

	while (n < 64 && (self & ((uint64) 1 << n)) == 0)
		n++;
	return n;
}

/*
 * std_next_power_of_two_u64 is u64::next_power_of_two: one more than
 * one_less_than_next_power_of_two, the all-ones mask below the highest bit of
 * self - 1, its addition checked as the caller's arithmetic is, since it is
 * #[rustc_inherit_overflow_checks], at uint_impl!'s location for u64.
 */
static inline uint64
std_next_power_of_two_u64(uint64 self, const char *file, uint32 line, uint32 col)
{
	uint64		one_less = self <= 1 ? 0 : PG_UINT64_MAX >> std_leading_zeros_u64(self - 1);

	return std_add_u64(one_less, 1, file, line, col);
}

/* std_leading_zeros_u32 is u32::leading_zeros: the zero bits above the highest one, 32 for zero. */
static inline uint32
std_leading_zeros_u32(uint32 self)
{
	uint32		n = 0;

	while (n < 32 && (self & ((uint32) 1 << (31 - n))) == 0)
		n++;
	return n;
}

/*
 * std_next_power_of_two_u32 is u32::next_power_of_two, as u64's, its
 * addition checked as the caller's arithmetic is, at uint_impl!'s location
 * for u32.
 */
static inline uint32
std_next_power_of_two_u32(uint32 self, const char *file, uint32 line, uint32 col)
{
	uint32		one_less = self <= 1 ? 0 : PG_UINT32_MAX >> std_leading_zeros_u32(self - 1);

	return std_add_u32(one_less, 1, file, line, col);
}

/*
 * STD_NUM_MOD_FILE is core's num/mod.rs, whose uint_impl! invocation for a
 * type is where rustc places an unsigned pow's overflow panic.
 */
#define STD_NUM_MOD_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "num" STD_SEP "mod.rs"

/*
 * std_is_multiple_of_u64 is u64::is_multiple_of: whether a is a multiple of
 * b, zero being one of zero alone, its remainder's zero check kept as the
 * crates' instances keep it.
 */
static inline bool
std_is_multiple_of_u64(uint64 a, uint64 b)
{
	if (b == 0)
		return a == 0;
	return std_rem_u64(a, b, STD_NUM_MOD_FILE, 1322, 5) == 0;
}

/*
 * std_is_multiple_of_usize is usize::is_multiple_of: whether a is a
 * multiple of b, zero being one of zero alone, its remainder's zero check
 * kept as the crates' instances keep it.
 */
static inline bool
std_is_multiple_of_usize(size_t a, size_t b)
{
	if (b == 0)
		return a == 0;
	return std_rem_usize(a, b, STD_NUM_MOD_FILE, 1452, 5) == 0;
}

/*
 * std_next_multiple_of_usize is usize::next_multiple_of, whose operators
 * inherit their caller's overflow checks and report at core's uint_impl!
 * invocation: the value when the remainder is 0, else the value raised by
 * the rest of the divisor.
 */
static inline size_t
std_next_multiple_of_usize(size_t self, size_t rhs)
{
	size_t		r = std_rem_usize(self, rhs, STD_NUM_MOD_FILE, 1452, 5);

	if (r == 0)
		return self;
	return std_add_usize(self, std_sub_usize(rhs, r, STD_NUM_MOD_FILE, 1452, 5), STD_NUM_MOD_FILE, 1452, 5);
}

/*
 * std_checked_next_multiple_of_usize is usize::checked_next_multiple_of:
 * the value when its remainder is 0, else the value raised by the rest of
 * the divisor, or false for a divisor of 0 or a sum past usize.
 */
static inline bool
std_checked_next_multiple_of_usize(size_t self, size_t rhs, size_t *result)
{
	size_t		r;

	if (rhs == 0)
		return false;
	r = self % rhs;
	if (r == 0)
	{
		*result = self;
		return true;
	}
	return std_checked_add_usize(self, rhs - r, result);
}

/* std_abs_diff_usize is usize::abs_diff: the larger less the smaller, which can't overflow. */
static inline size_t
std_abs_diff_usize(size_t self, size_t other)
{
	return self < other ? other - self : self - other;
}

/* std_next_multiple_of_u64 is u64::next_multiple_of, as usize's, at u64's uint_impl! invocation. */
static inline uint64
std_next_multiple_of_u64(uint64 self, uint64 rhs)
{
	uint64		r = std_rem_u64(self, rhs, STD_NUM_MOD_FILE, 1322, 5);

	if (r == 0)
		return self;
	return std_add_u64(self, std_sub_u64(rhs, r, STD_NUM_MOD_FILE, 1322, 5), STD_NUM_MOD_FILE, 1322, 5);
}

/*
 * STD_ENUMERATE_FILE is core's iter/adapters/enumerate.rs, whose
 * Enumerate::next counts with #[rustc_inherit_overflow_checks], so its
 * count += 1 (line 82) is checked as the calling crate's arithmetic is.
 */
#define STD_ENUMERATE_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "iter" STD_SEP "adapters" STD_SEP "enumerate.rs"

/* STD_STEP_BY_FILE is core's iter/adapters/step_by.rs, where StepBy::new asserts its step and sets up a range. */
#define STD_STEP_BY_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "iter" STD_SEP "adapters" STD_SEP "step_by.rs"

/* STD_ITERATOR_FILE is core's iter/traits/iterator.rs, where a default position's count overflows. */
#define STD_ITERATOR_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "iter" STD_SEP "traits" STD_SEP "iterator.rs"

/* std_Iterator_position_step is the default Iterator::position's count past an item that fails, its overflow checked. */
static inline size_t
std_Iterator_position_step(size_t i)
{
	return std_add_usize(i, 1, STD_ITERATOR_FILE, 3145, 21);
}

/* std_Enumerate_count_step is Enumerate::next's count past an item, its overflow checked. */
static inline size_t
std_Enumerate_count_step(size_t count)
{
	return std_add_usize(count, 1, STD_ENUMERATE_FILE, 82, 9);
}

/*
 * STD_IS_VAL_STATICALLY_KNOWN is intrinsics::is_val_statically_known: whether
 * the compiler knows a value, __builtin_constant_p's answer where the compiler
 * has it, and false with MSVC, which has none; either answer gives the same
 * result.
 */
#if defined(__GNUC__) || defined(__clang__)
#define STD_IS_VAL_STATICALLY_KNOWN(x) __builtin_constant_p(x)
#else
#define STD_IS_VAL_STATICALLY_KNOWN(x) false
#endif

/* std_u64_ilog2 is u64::ilog2: checked_ilog2's NonZero ilog2, panicking at its caller for zero. */
static inline uint32
std_u64_ilog2(uint64 self)
{
	if (self == 0)
		std_panic(STD_NUM_MOD_FILE, 1322, 5, "argument of integer logarithm must be positive",
				  sizeof("argument of integer logarithm must be positive") - 1);
	return (uint32) pg_leftmost_one_pos64(self);
}

/* std_u64_checked_shl is u64::checked_shl: the shift when it is below 64, through unchecked_shl's precondition. */
static inline bool
std_u64_checked_shl(uint64 self, uint32 rhs, uint64 *result)
{
	if (!(rhs < 64))
		return false;
#ifdef USE_ASSERT_CHECKING
	if (!(rhs < 64))
		STD_PTR_VIOLATED("u64::unchecked_shl cannot overflow", STD_NUM_MOD_FILE, 1322, 5);
#endif
	*result = self << rhs;
	return true;
}

/*
 * std_u64_checked_pow is u64::checked_pow: for a power of two the compiler
 * knows, a shift by its log times the exponent; else exponentiation by
 * squaring, none on overflow.
 */
static inline bool
std_u64_checked_pow(uint64 base, uint32 exp, uint64 *result)
{
	uint64		acc = 1;

	if (STD_IS_VAL_STATICALLY_KNOWN(base) && base != 0 && (base & (base - 1)) == 0)
	{
		uint32		k = std_u64_ilog2(base);
		uint32		shift;

		if (pg_mul_u32_overflow(k, exp, &shift))
			return false;
		return std_u64_checked_shl(1, shift, result);
	}
	if (exp == 0)
	{
		*result = 1;
		return true;
	}
	if (STD_IS_VAL_STATICALLY_KNOWN(exp))
	{
		while (exp > 1)
		{
			if ((exp & 1) == 1 && pg_mul_u64_overflow(acc, base, &acc))
				return false;
			exp /= 2;
			if (pg_mul_u64_overflow(base, base, &base))
				return false;
		}
		return !pg_mul_u64_overflow(acc, base, result);
	}
	for (;;)
	{
		if ((exp & 1) == 1)
		{
			if (pg_mul_u64_overflow(acc, base, &acc))
				return false;
			if (exp == 1)
			{
				*result = acc;
				return true;
			}
		}
		exp /= 2;
		if (pg_mul_u64_overflow(base, base, &base))
			return false;
	}
}

/*
 * std_u64_wrapping_pow is u64::wrapping_pow, overflowing_pow's result: for a
 * power of two the compiler knows, an unbounded shift by its log times the
 * exponent; else exponentiation by squaring, wrapping.
 */
static inline uint64
std_u64_wrapping_pow(uint64 base, uint32 exp)
{
	uint64		acc = 1;

	if (STD_IS_VAL_STATICALLY_KNOWN(base) && base != 0 && (base & (base - 1)) == 0)
	{
		uint32		k = std_u64_ilog2(base);
		uint32		shift;

		if (pg_mul_u32_overflow(k, exp, &shift))
			return 0;
		return shift < 64 ? (uint64) 1 << shift : 0;
	}
	if (exp == 0)
		return 1;
	while (exp > 1)
	{
		if ((exp & 1) == 1)
			acc = acc * base;
		exp /= 2;
		base = base * base;
	}
	return acc * base;
}

/*
 * std_pow_u64 is u64's pow: strict_pow under overflow checks, panicking at
 * the uint_impl! invocation when checked_pow overflows, and wrapping_pow
 * otherwise.
 */
static inline uint64
std_pow_u64(uint64 base, uint32 exp)
{
#ifdef USE_ASSERT_CHECKING
	uint64		result;

	if (!std_u64_checked_pow(base, exp, &result))
		STD_OVERFLOW(STD_NUM_MOD_FILE, 1322, 5, "attempt to exponentiate with overflow");
	return result;
#else
	return std_u64_wrapping_pow(base, exp);
#endif
}

/* STD_ACCUM_FILE is core's iter/traits/accum.rs, where an integer sum's overflow panics. */
#define STD_ACCUM_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "iter" STD_SEP "traits" STD_SEP "accum.rs"

/*
 * STD_ITER_FILTER_FILE is core's iter/adapters/filter.rs, where a count of a
 * filter over a TrustedLen iterator is assumed within its upper bound.
 */
#define STD_ITER_FILTER_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "iter" STD_SEP "adapters" STD_SEP "filter.rs"

/* StdUsizeOption is an Option<usize> held as a value, as a slice's element. */
typedef struct StdUsizeOption
{
	bool		has;
	size_t		x;
} StdUsizeOption;

/* std_u16_from_le_bytes is u16::from_le_bytes. */
static inline uint16
std_u16_from_le_bytes(const uint8 *b)
{
	return (uint16) (b[0] | (b[1] << 8));
}

/* std_u32_from_le_bytes is u32::from_le_bytes. */
static inline uint32
std_u32_from_le_bytes(const uint8 *b)
{
	return (uint32) b[0] | ((uint32) b[1] << 8) | ((uint32) b[2] << 16) |
		((uint32) b[3] << 24);
}

/* std_u64_from_le_bytes is u64::from_le_bytes. */
static inline uint64
std_u64_from_le_bytes(const uint8 *b)
{
	return (uint64) std_u32_from_le_bytes(b) |
		((uint64) std_u32_from_le_bytes(b + 4) << 32);
}

/* std_u32_from_be_bytes is u32::from_be_bytes. */
static inline uint32
std_u32_from_be_bytes(const uint8 *b)
{
	return ((uint32) b[0] << 24) | ((uint32) b[1] << 16) | ((uint32) b[2] << 8) |
		(uint32) b[3];
}

/* std_u16_from_be_bytes is u16::from_be_bytes. */
static inline uint16
std_u16_from_be_bytes(const uint8 *b)
{
	return (uint16) ((b[0] << 8) | b[1]);
}

/* std_u64_from_be_bytes is u64::from_be_bytes. */
static inline uint64
std_u64_from_be_bytes(const uint8 *b)
{
	return ((uint64) std_u32_from_be_bytes(b) << 32) |
		(uint64) std_u32_from_be_bytes(b + 4);
}

/* std_u16_to_le_bytes is u16::to_le_bytes, written to b. */
static inline void
std_u16_to_le_bytes(uint16 v, uint8 *b)
{
	b[0] = (uint8) v;
	b[1] = (uint8) (v >> 8);
}

/* std_u32_to_le_bytes is u32::to_le_bytes, written to b. */
static inline void
std_u32_to_le_bytes(uint32 v, uint8 *b)
{
	for (int i = 0; i < 4; i++)
		b[i] = (uint8) (v >> (8 * i));
}

/* std_u64_to_le_bytes is u64::to_le_bytes, written to b. */
static inline void
std_u64_to_le_bytes(uint64 v, uint8 *b)
{
	for (int i = 0; i < 8; i++)
		b[i] = (uint8) (v >> (8 * i));
}

/* std_u16_to_be_bytes is u16::to_be_bytes, written to b. */
static inline void
std_u16_to_be_bytes(uint16 v, uint8 *b)
{
	b[0] = (uint8) (v >> 8);
	b[1] = (uint8) v;
}

/* std_u32_to_be_bytes is u32::to_be_bytes, written to b. */
static inline void
std_u32_to_be_bytes(uint32 v, uint8 *b)
{
	for (int i = 0; i < 4; i++)
		b[i] = (uint8) (v >> (8 * (3 - i)));
}

/* std_u64_to_be_bytes is u64::to_be_bytes, written to b. */
static inline void
std_u64_to_be_bytes(uint64 v, uint8 *b)
{
	for (int i = 0; i < 8; i++)
		b[i] = (uint8) (v >> (8 * (7 - i)));
}

/* std_f64_from_le_bytes is f64::from_le_bytes: from_bits of u64::from_le_bytes. */
static inline float8
std_f64_from_le_bytes(const uint8 *b)
{
	uint64		bits = std_u64_from_le_bytes(b);
	float8		v;

	memcpy(&v, &bits, sizeof(v));
	return v;
}

/*
 * STD_ITER_RANGE_FILE is core's iter/range.rs, where Step's unchecked
 * arithmetic reports its preconditions.
 */
#define STD_ITER_RANGE_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "iter" STD_SEP "range.rs"

/* StdIntErrorKind is IntErrorKind: why a string isn't an integer. */
typedef enum StdIntErrorKind
{
	STD_INT_ERROR_KIND_EMPTY,
	STD_INT_ERROR_KIND_INVALID_DIGIT,
	STD_INT_ERROR_KIND_POS_OVERFLOW,
	STD_INT_ERROR_KIND_NEG_OVERFLOW,
	STD_INT_ERROR_KIND_ZERO,
} StdIntErrorKind;

/* StdTryFromIntError is TryFromIntError: the kind of the integer conversion that failed. */
typedef struct StdTryFromIntError
{
	StdIntErrorKind kind;
} StdTryFromIntError;

/* std_wrapping_add_signed_u64 is u64::wrapping_add_signed: the sum of the signed value as a u64, wrapped. */
static inline uint64
std_wrapping_add_signed_u64(uint64 self, int64 rhs)
{
	return self + (uint64) rhs;
}

/* std_i32_try_from_u64 is i32's TryFrom<u64>, try_from_upper_bounded: the value when it is within i32::MAX. */
static inline bool
std_i32_try_from_u64(uint64 u, int32 *result, StdTryFromIntError *error)
{
	if (u > (uint64) PG_INT32_MAX)
	{
		error->kind = STD_INT_ERROR_KIND_POS_OVERFLOW;
		return false;
	}
	*result = (int32) u;
	return true;
}

/* std_u32_try_from_u64 is u32's TryFrom<u64>, try_from_upper_bounded: the value when it is within u32::MAX. */
static inline bool
std_u32_try_from_u64(uint64 u, uint32 *result, StdTryFromIntError *error)
{
	if (u > (uint64) PG_UINT32_MAX)
	{
		error->kind = STD_INT_ERROR_KIND_POS_OVERFLOW;
		return false;
	}
	*result = (uint32) u;
	return true;
}

/* std_u32_try_from_usize is u32's TryFrom<usize>, try_from_upper_bounded: the value when it is within u32::MAX. */
static inline bool
std_u32_try_from_usize(size_t u, uint32 *result, StdTryFromIntError *error)
{
	if (u > (size_t) PG_UINT32_MAX)
	{
		error->kind = STD_INT_ERROR_KIND_POS_OVERFLOW;
		return false;
	}
	*result = (uint32) u;
	return true;
}

/* std_i64_try_from_u64 is i64's TryFrom<u64>, try_from_upper_bounded: the value when it is within i64::MAX. */
static inline bool
std_i64_try_from_u64(uint64 u, int64 *result, StdTryFromIntError *error)
{
	if (u > (uint64) PG_INT64_MAX)
	{
		error->kind = STD_INT_ERROR_KIND_POS_OVERFLOW;
		return false;
	}
	*result = (int64) u;
	return true;
}

/*
 * std_usize_try_from_u64 is usize's TryFrom<u64> on a 64-bit target,
 * impl_try_from_unbounded's: always Ok, the value as usize.
 */
static inline size_t
std_usize_try_from_u64(uint64 u)
{
	return (size_t) u;
}

/* std_i32_unsigned_abs is i32::unsigned_abs: the magnitude as u32, wrapping_abs's bits, which i32::MIN's fits. */
static inline uint32
std_i32_unsigned_abs(int32 v)
{
	return v < 0 ? (uint32) 0 - (uint32) v : (uint32) v;
}

/* std_i64_unsigned_abs is i64::unsigned_abs: the magnitude as u64, wrapping_abs's bits, which i64::MIN's fits. */
static inline uint64
std_i64_unsigned_abs(int64 v)
{
	return v < 0 ? (uint64) 0 - (uint64) v : (uint64) v;
}

/* std_u64_from_ne_bytes is u64::from_ne_bytes: eight bytes in the platform's order. */
static inline uint64
std_u64_from_ne_bytes(const uint8 *b)
{
	uint64		v;

	memcpy(&v, b, sizeof(v));
	return v;
}

/*
 * std_usize_try_from_i32 is usize's TryFrom<i32>, try_from_lower_bounded:
 * the value when it isn't negative, else NegOverflow.
 */
static inline bool
std_usize_try_from_i32(int32 v, size_t *result, StdTryFromIntError *error)
{
	if (v >= 0)
	{
		*result = (size_t) v;
		return true;
	}
	error->kind = STD_INT_ERROR_KIND_NEG_OVERFLOW;
	return false;
}

/*
 * std_forward_unchecked_usize_Checked is usize's Step::forward_unchecked
 * where a crate codegens it: unchecked_add, its precondition checked.
 */
static inline size_t
std_forward_unchecked_usize_Checked(size_t start, size_t n)
{
	std_usize_unchecked_add_check(start, n, STD_ITER_RANGE_FILE, 548, 1);
	return start + n;
}

/* std_forward_unchecked_usize_Unchecked is usize's Step::forward_unchecked in std's own code: unchecked_add. */
static inline size_t
std_forward_unchecked_usize_Unchecked(size_t start, size_t n)
{
	return start + n;
}

/*
 * std_backward_unchecked_usize_Checked is usize's Step::backward_unchecked
 * where a crate codegens it: unchecked_sub, its precondition checked.
 */
static inline size_t
std_backward_unchecked_usize_Checked(size_t start, size_t n)
{
	std_usize_unchecked_sub_check(start, n, STD_ITER_RANGE_FILE, 548, 1);
	return start - n;
}

/* std_backward_unchecked_usize_Unchecked is usize's Step::backward_unchecked in std's own code: unchecked_sub. */
static inline size_t
std_backward_unchecked_usize_Unchecked(size_t start, size_t n)
{
	return start - n;
}

/*
 * std_forward_unchecked_u8_Checked is u8's Step::forward_unchecked where a
 * crate codegens it: unchecked_add of n as u8, its precondition checked.
 */
static inline uint8
std_forward_unchecked_u8_Checked(uint8 start, size_t n)
{
	std_u8_unchecked_add_check(start, (uint8) n, STD_ITER_RANGE_FILE, 548, 1);
	return std_wrapping_add_u8(start, (uint8) n);
}

/*
 * std_forward_unchecked_u32_Checked is u32's Step::forward_unchecked where a
 * crate codegens it: unchecked_add of n as u32, its precondition checked.
 */
static inline uint32
std_forward_unchecked_u32_Checked(uint32 start, size_t n)
{
	std_u32_unchecked_add_check(start, (uint32) n, STD_ITER_RANGE_FILE, 548, 1);
	return (uint32) (start + (uint32) n);
}

/*
 * std_forward_unchecked_u64_Checked is u64's Step::forward_unchecked where a
 * crate codegens it: unchecked_add of n as u64, its precondition checked.
 */
static inline uint64
std_forward_unchecked_u64_Checked(uint64 start, size_t n)
{
	std_u64_unchecked_add_check(start, (uint64) n, STD_ITER_RANGE_FILE, 548, 1);
	return start + (uint64) n;
}

/* std_forward_unchecked_u64_Unchecked is u64's Step::forward_unchecked in std's own code. */
static inline uint64
std_forward_unchecked_u64_Unchecked(uint64 start, size_t n)
{
	return start + (uint64) n;
}

/*
 * std_forward_unchecked_i32_Checked is i32's Step::forward_unchecked where a
 * crate codegens it: checked_add_unsigned of n as u32, unwrap_unchecked, whose
 * precondition is checked.
 */
static inline int32
std_forward_unchecked_i32_Checked(int32 start, size_t n)
{
	int64		sum = (int64) start + (int64) (uint32) n;

	if (sum > PG_INT32_MAX)
	{
#ifdef USE_ASSERT_CHECKING
		std_hint_unreachable_unchecked(STD_ITER_RANGE_FILE, 548, 1);
#else
		pg_unreachable();
#endif
	}
	return (int32) sum;
}

/*
 * std_forward_i32_Checked is i32's Step::forward where a crate codegens it,
 * as RangeFrom's next steps: forward_checked's wrapping sum of n taken as a
 * u32, the inherited overflow check's panic (`Self::MAX + 1`) when it
 * overflows, and the wrapping sum.
 */
static inline int32
std_forward_i32_Checked(int32 start, size_t n)
{
	int32		wrapped = std_wrapping_add_i32(start, (int32) (uint32) n);

	if (n > PG_UINT32_MAX || wrapped < start)
		(void) std_add_i32(PG_INT32_MAX, 1, STD_ITER_RANGE_FILE, 548, 1);
	return wrapped;
}

/*
 * std_forward_u64_Checked is u64's Step::forward where a crate codegens it,
 * a RangeFrom<u64>'s next: forward_checked's sum of n as u64, the inherited
 * overflow check's panic (`Self::MAX + 1`) when it overflows, and the
 * wrapping sum.
 */
static inline uint64
std_forward_u64_Checked(uint64 start, size_t n)
{
	uint64		sum;

	if (!std_checked_add_u64(start, (uint64) n, &sum))
		(void) std_add_u64(PG_UINT64_MAX, 1, STD_ITER_RANGE_FILE, 548, 1);
	return std_wrapping_add_u64(start, (uint64) n);
}

/*
 * std_forward_u8_Checked is u8's Step::forward where a crate codegens it, a
 * RangeFrom<u8>'s next: forward_checked's sum of n as u8, the inherited
 * overflow check's panic (`Self::MAX + 1`) when it overflows, and the
 * wrapping sum.
 */
static inline uint8
std_forward_u8_Checked(uint8 start, size_t n)
{
	uint8		sum;

	if (n > PG_UINT8_MAX || !std_checked_add_u8(start, (uint8) n, &sum))
		(void) std_add_u8(PG_UINT8_MAX, 1, STD_ITER_RANGE_FILE, 548, 1);
	return std_wrapping_add_u8(start, (uint8) n);
}

/*
 * std_forward_usize_Checked is usize's Step::forward where a crate codegens
 * it, a RangeFrom<usize>'s next: forward_checked's sum, the inherited
 * overflow check's panic (`Self::MAX + 1`) when it overflows, and the
 * wrapping sum.
 */
static inline size_t
std_forward_usize_Checked(size_t start, size_t n)
{
	size_t		sum;

	if (!std_checked_add_usize(start, n, &sum))
		(void) std_add_usize(SIZE_MAX, 1, STD_ITER_RANGE_FILE, 548, 1);
	return start + n;
}

/*
 * std_backward_unchecked_i32_Checked is i32's Step::backward_unchecked where
 * a crate codegens it: checked_sub_unsigned of n as u32, unwrap_unchecked,
 * whose precondition is checked.
 */
static inline int32
std_backward_unchecked_i32_Checked(int32 start, size_t n)
{
	int64		difference = (int64) start - (int64) (uint32) n;

	if (difference < PG_INT32_MIN)
	{
#ifdef USE_ASSERT_CHECKING
		std_hint_unreachable_unchecked(STD_ITER_RANGE_FILE, 548, 1);
#else
		pg_unreachable();
#endif
	}
	return (int32) difference;
}

/* std_saturating_abs_i32 is i32::saturating_abs: the magnitude, i32::MAX for i32::MIN. */
static inline int32
std_saturating_abs_i32(int32 self)
{
	if (self == PG_INT32_MIN)
		return PG_INT32_MAX;
	return self < 0 ? -self : self;
}

/* std_forward_unchecked_u32_Unchecked is u32's Step::forward_unchecked in std's own code. */
static inline uint32
std_forward_unchecked_u32_Unchecked(uint32 start, size_t n)
{
	return (uint32) (start + (uint32) n);
}

/* std_f32_from_bits is f32::from_bits, the float of those bits as they are. */
static inline float4
std_f32_from_bits(uint32 v)
{
	float4		f;

	memcpy(&f, &v, sizeof(f));
	return f;
}

/* std_f64_from_bits is f64::from_bits, the float of those bits as they are. */
static inline float8
std_f64_from_bits(uint64 v)
{
	float8		f;

	memcpy(&f, &v, sizeof(f));
	return f;
}

/* std_f32_to_bits is f32::to_bits, the float's bits as they are. */
static inline uint32
std_f32_to_bits(float4 v)
{
	uint32		bits;

	memcpy(&bits, &v, sizeof(bits));
	return bits;
}

/* std_f64_to_bits is f64::to_bits, the float's bits as they are. */
static inline uint64
std_f64_to_bits(float8 v)
{
	uint64		bits;

	memcpy(&bits, &v, sizeof(bits));
	return bits;
}

/* std_f64_to_le_bytes is f64::to_le_bytes, to_bits' u64::to_le_bytes written to b. */
static inline void
std_f64_to_le_bytes(float8 v, uint8 *b)
{
	uint64		bits;

	memcpy(&bits, &v, sizeof(bits));
	std_u64_to_le_bytes(bits, b);
}

/* std_f64_is_nan is f64::is_nan: a value unequal to itself. */
static inline bool
std_f64_is_nan(float8 v)
{
	return v != v;
}

#endif							/* DOLTGRES_STD_NUM_H */
