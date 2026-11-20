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
 * The C forms of Rust's i128 and u128 (the port text's section 3): two
 * 64-bit halves on every target, one implementation, the operations the
 * crates use each written on the halves, with Rust's overflow panics under
 * USE_ASSERT_CHECKING.
 */
#ifndef DOLTGRES_STD_INT128_H
#define DOLTGRES_STD_INT128_H

#include "doltgres/std/cmp.h"
#include "doltgres/std/num.h"

/* StdI128 is an i128 as two's complement halves. */
typedef struct StdI128
{
	uint64		hi;
	uint64		lo;
} StdI128;

/* StdU128 is a u128 as halves. */
typedef struct StdU128
{
	uint64		hi;
	uint64		lo;
} StdU128;

/* std_i128_from_i64 is `x as i128`, a sign extension. */
static inline StdI128
std_i128_from_i64(int64 x)
{
	return (StdI128)
	{
		x < 0 ? PG_UINT64_MAX : 0, (uint64) x
	};
}

/* std_i128_is_negative is whether an i128 is below zero. */
static inline bool
std_i128_is_negative(StdI128 x)
{
	return (x.hi >> 63) != 0;
}

/* std_add_i128 is i128's +, with Rust's overflow panic under USE_ASSERT_CHECKING. */
static inline StdI128
std_add_i128(StdI128 a, StdI128 b, const char *file, uint32 line, uint32 col)
{
	StdI128		r;

	r.lo = a.lo + b.lo;
	r.hi = a.hi + b.hi + (r.lo < a.lo ? 1 : 0);
	STD_CHECKED(std_i128_is_negative(a) == std_i128_is_negative(b) &&
				std_i128_is_negative(r) != std_i128_is_negative(a),
				file, line, col, "attempt to add with overflow");
	return r;
}

/* std_mul_u64_wide is the full 128-bit product of two u64s. */
static inline StdU128
std_mul_u64_wide(uint64 a, uint64 b)
{
	uint64		a_lo = a & 0xFFFFFFFF;
	uint64		a_hi = a >> 32;
	uint64		b_lo = b & 0xFFFFFFFF;
	uint64		b_hi = b >> 32;
	uint64		p0 = a_lo * b_lo;
	uint64		p1 = a_lo * b_hi;
	uint64		p2 = a_hi * b_lo;
	uint64		p3 = a_hi * b_hi;
	uint64		mid = (p0 >> 32) + (p1 & 0xFFFFFFFF) + (p2 & 0xFFFFFFFF);

	return (StdU128)
	{
		p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32), (mid << 32) | (p0 & 0xFFFFFFFF)
	};
}

/* std_negate_u128 is the two's complement of a value's halves. */
static inline StdU128
std_negate_u128(uint64 hi, uint64 lo)
{
	return (StdU128)
	{
		~hi + (lo == 0 ? 1 : 0), ~lo + 1
	};
}

/*
 * std_mul_i128 is i128's *, the low 128 bits of the product, with Rust's
 * overflow panic under USE_ASSERT_CHECKING when the true product doesn't fit:
 * the magnitudes multiplied, and the product's magnitude past 2^127, or at it
 * for a positive product.
 */
static inline StdI128
std_mul_i128(StdI128 a, StdI128 b, const char *file, uint32 line, uint32 col)
{
	bool		negative = std_i128_is_negative(a) != std_i128_is_negative(b);
	StdU128		ua = std_i128_is_negative(a) ? std_negate_u128(a.hi, a.lo) : (StdU128) {a.hi, a.lo};
	StdU128		ub = std_i128_is_negative(b) ? std_negate_u128(b.hi, b.lo) : (StdU128) {b.hi, b.lo};
	StdU128		low = std_mul_u64_wide(ua.lo, ub.lo);
	StdU128		cross1 = std_mul_u64_wide(ua.hi, ub.lo);
	StdU128		cross2 = std_mul_u64_wide(ua.lo, ub.hi);
	uint64		mid = low.hi + cross1.lo;
	bool		carry = mid < low.hi;
	uint64		hi = mid + cross2.lo;
	bool		overflow;
	StdU128		r;

	carry = carry || hi < mid;
	overflow = (ua.hi != 0 && ub.hi != 0) || cross1.hi != 0 || cross2.hi != 0 || carry;
	if (!overflow)
	{
		bool		over_half = (hi >> 63) != 0;
		bool		at_half = hi == ((uint64) 1 << 63) && low.lo == 0;

		overflow = negative ? (over_half && !at_half) : over_half;
	}
	STD_CHECKED(overflow, file, line, col, "attempt to multiply with overflow");
	r = negative ? std_negate_u128(hi, low.lo) : (StdU128)
	{
		hi, low.lo
	};
	return (StdI128)
	{
		r.hi, r.lo
	};
}

/* std_i128_as_u128 is `x as u128`, the same bits. */
static inline StdU128
std_i128_as_u128(StdI128 x)
{
	return (StdU128)
	{
		x.hi, x.lo
	};
}

/* std_u128_xor is u128's ^. */
static inline StdU128
std_u128_xor(StdU128 a, StdU128 b)
{
	return (StdU128)
	{
		a.hi ^ b.hi, a.lo ^ b.lo
	};
}

/* std_shr_folded_u128 is u128's >> by a constant count below 128, whose check rustc folds away. */
static inline StdU128
std_shr_folded_u128(StdU128 a, uint32 n)
{
	if (n == 0)
		return a;
	if (n >= 64)
		return (StdU128)
	{
		0, a.hi >> (n - 64)
	};
	return (StdU128)
	{
		a.hi >> n, (a.lo >> n) | (a.hi << (64 - n))
	};
}

/* std_shl_u128 is u128's << by a count below 128. */
static inline StdU128
std_shl_u128(StdU128 a, uint32 n, const char *file, uint32 line, uint32 col)
{
	StdU128		r;

	STD_CHECKED(n >= 128, file, line, col, "attempt to shift left with overflow");
	n &= 127;
	if (n == 0)
		return a;
	if (n >= 64)
	{
		r.hi = a.lo << (n - 64);
		r.lo = 0;
		return r;
	}
	r.hi = (a.hi << n) | (a.lo >> (64 - n));
	r.lo = a.lo << n;
	return r;
}

/* std_u128_from_u64 is `x as u128`, a zero extension. */
static inline StdU128
std_u128_from_u64(uint64 x)
{
	StdU128		r = {0, x};

	return r;
}

/* std_u128_lt is u128's <. */
static inline bool
std_u128_lt(StdU128 a, StdU128 b)
{
	return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

/* std_u128_PartialOrd_lt is u128's PartialOrd::lt, on pointers to the values. */
static inline bool
std_u128_PartialOrd_lt(const StdU128 *self, const StdU128 *other)
{
	return std_u128_lt(*self, *other);
}

/* std_u128_Ord_cmp is u128's Ord, on pointers to the values. */
static inline StdOrdering
std_u128_Ord_cmp(const void *a, const void *b)
{
	StdU128		x = *(const StdU128 *) a;
	StdU128		y = *(const StdU128 *) b;

	return std_u128_lt(x, y) ? STD_ORDERING_LESS : (std_u128_lt(y, x) ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/* std_sub_u128 is u128's -, with Rust's overflow panic under USE_ASSERT_CHECKING. */
static inline StdU128
std_sub_u128(StdU128 a, StdU128 b, const char *file, uint32 line, uint32 col)
{
	StdU128		r;

	STD_CHECKED(std_u128_lt(a, b), file, line, col, "attempt to subtract with overflow");
	r.lo = a.lo - b.lo;
	r.hi = a.hi - b.hi - (a.lo < b.lo ? 1 : 0);
	return r;
}

/*
 * std_mul_u128 is u128's *, the low 128 bits of the product, with Rust's
 * overflow panic under USE_ASSERT_CHECKING when the true product doesn't fit.
 */
static inline StdU128
std_mul_u128(StdU128 a, StdU128 b, const char *file, uint32 line, uint32 col)
{
	StdU128		low = std_mul_u64_wide(a.lo, b.lo);
	StdU128		cross1 = std_mul_u64_wide(a.hi, b.lo);
	StdU128		cross2 = std_mul_u64_wide(a.lo, b.hi);
	uint64		mid = low.hi + cross1.lo;
	bool		carry = mid < low.hi;
	uint64		hi = mid + cross2.lo;
	StdU128		r;

	carry = carry || hi < mid;
	STD_CHECKED((a.hi != 0 && b.hi != 0) || cross1.hi != 0 || cross2.hi != 0 || carry, file, line, col,
				"attempt to multiply with overflow");
	r.hi = hi;
	r.lo = low.lo;
	return r;
}

/* std_shl_folded_u128 is u128's << by a constant count below 128, whose check rustc folds away. */
static inline StdU128
std_shl_folded_u128(StdU128 a, uint32 n)
{
	StdU128		r;

	if (n == 0)
		return a;
	if (n >= 64)
	{
		r.hi = a.lo << (n - 64);
		r.lo = 0;
		return r;
	}
	r.hi = (a.hi << n) | (a.lo >> (64 - n));
	r.lo = a.lo << n;
	return r;
}

/* std_wrapping_sub_u128 is u128's - as core's own code computes it, wrapping. */
static inline StdU128
std_wrapping_sub_u128(StdU128 a, StdU128 b)
{
	StdU128		r;

	r.lo = a.lo - b.lo;
	r.hi = a.hi - b.hi - (a.lo < b.lo ? 1 : 0);
	return r;
}

/*
 * std_div_u128 is u128's /, with Rust's panic at a line and column for a
 * zero divisor: the quotient by long division, a bit at a time.
 */
static inline StdU128
std_div_u128(StdU128 a, StdU128 b, const char *file, uint32 line, uint32 col)
{
	StdU128		quotient = {0, 0};
	StdU128		remainder = {0, 0};

	if (b.hi == 0 && b.lo == 0)
		STD_OVERFLOW(file, line, col, "attempt to divide by zero");
	for (int i = 127; i >= 0; i--)
	{
		uint64		carry = remainder.hi >> 63;
		uint64		bit = i >= 64 ? (a.hi >> (i - 64)) & 1 : (a.lo >> i) & 1;

		remainder.hi = (remainder.hi << 1) | (remainder.lo >> 63);
		remainder.lo = (remainder.lo << 1) | bit;
		if (carry != 0 || !std_u128_lt(remainder, b))
		{
			remainder = std_wrapping_sub_u128(remainder, b);
			if (i >= 64)
				quotient.hi |= (uint64) 1 << (i - 64);
			else
				quotient.lo |= (uint64) 1 << i;
		}
	}
	return quotient;
}

/* std_wrapping_mul_u128 is u128's * as core's own code computes it, the product's low 128 bits. */
static inline StdU128
std_wrapping_mul_u128(StdU128 a, StdU128 b)
{
	StdU128		r = std_mul_u64_wide(a.lo, b.lo);

	r.hi += a.hi * b.lo + a.lo * b.hi;
	return r;
}

/*
 * std_carrying_mul_u128 is u128::carrying_mul: self times rhs plus carry as
 * 256 bits, the low half returned and the high half in result_1.
 */
static inline StdU128
std_carrying_mul_u128(StdU128 self, StdU128 rhs, StdU128 carry, StdU128 *result_1)
{
	StdU128		ll = std_mul_u64_wide(self.lo, rhs.lo);
	StdU128		lh = std_mul_u64_wide(self.lo, rhs.hi);
	StdU128		hl = std_mul_u64_wide(self.hi, rhs.lo);
	StdU128		hh = std_mul_u64_wide(self.hi, rhs.hi);
	uint64		w[4] = {ll.lo, ll.hi, hh.lo, hh.hi};
	const uint64 terms[6][2] = {{lh.lo, 1}, {hl.lo, 1}, {lh.hi, 2}, {hl.hi, 2}, {carry.lo, 0}, {carry.hi, 1}};
	StdU128		lo;

	for (int t = 0; t < 6; t++)
	{
		uint64		add = terms[t][0];

		for (uint64 k = terms[t][1]; k < 4 && add != 0; k++)
		{
			uint64		sum = w[k] + add;

			add = sum < w[k] ? 1 : 0;
			w[k] = sum;
		}
	}
	lo.hi = w[1];
	lo.lo = w[0];
	result_1->hi = w[3];
	result_1->lo = w[2];
	return lo;
}

/* std_u128_or is u128's |. */
static inline StdU128
std_u128_or(StdU128 a, StdU128 b)
{
	StdU128		r = {a.hi | b.hi, a.lo | b.lo};

	return r;
}

/* std_u128_as_u8 is `x as u8`, the low byte. */
static inline uint8
std_u128_as_u8(StdU128 x)
{
	return (uint8) x.lo;
}

/* std_i64_try_from_u128 is i64's TryFrom<u128>, try_from_upper_bounded: the value when it is within i64::MAX. */
static inline bool
std_i64_try_from_u128(StdU128 u, int64 *result, StdTryFromIntError *error)
{
	if (u.hi != 0 || u.lo > (uint64) PG_INT64_MAX)
	{
		error->kind = STD_INT_ERROR_KIND_POS_OVERFLOW;
		return false;
	}
	*result = (int64) u.lo;
	return true;
}

/* std_u128_to_le_bytes is u128::to_le_bytes, written to b: the low half's bytes, then the high half's. */
static inline void
std_u128_to_le_bytes(StdU128 v, uint8 *b)
{
	std_u64_to_le_bytes(v.lo, b);
	std_u64_to_le_bytes(v.hi, STD_PTR_AT(b, 8, 1));
}

/* test_std_int128_tests_u128_forms_match_rust is the u128 forms' edge test, which std's test module runs. */
extern void test_std_int128_tests_u128_forms_match_rust(void);

#endif							/* DOLTGRES_STD_INT128_H */
