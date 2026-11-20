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
 * The C forms of core's float formatting at Rust 1.99.1 that aren't generic
 * over the float (core/src/num/imp): diy_float's Fp, num::fmt's parts, the
 * Big32x40 bignum, flt2dec's digit layouts and estimator, and Grisu's and
 * Dragon's shortest and exact strategies, each in its Rust file's order.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/flt2dec.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* IMP_DIR is core's num/imp directory, where these files are. */
#define IMP_DIR STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "num" STD_SEP "imp" STD_SEP

/* DIY_FLOAT_FILE is core's num/imp/diy_float.rs. */
#define DIY_FLOAT_FILE IMP_DIR "diy_float.rs"

/* NUMFMT_FILE is core's num/imp/fmt.rs, num::fmt's. */
#define NUMFMT_FILE IMP_DIR "fmt.rs"

/* BIGNUM_FILE is core's num/imp/bignum.rs, whose checks report its macros' invocations. */
#define BIGNUM_FILE IMP_DIR "bignum.rs"

/* FLT2DEC_FILE is core's num/imp/flt2dec/mod.rs. */
#define FLT2DEC_FILE IMP_DIR "flt2dec" STD_SEP "mod.rs"

/* GRISU_FILE is core's num/imp/flt2dec/strategy/grisu.rs. */
#define GRISU_FILE IMP_DIR "flt2dec" STD_SEP "strategy" STD_SEP "grisu.rs"

/* DRAGON_FILE is core's num/imp/flt2dec/strategy/dragon.rs. */
#define DRAGON_FILE IMP_DIR "flt2dec" STD_SEP "strategy" STD_SEP "dragon.rs"

/* STD_FLT2DEC_PANIC is a panic of core's at a file's line and column, with a literal message. */
#define STD_FLT2DEC_PANIC(file, line, col, text) std_panic((file), (line), (col), (text), sizeof(text) - 1)

/* std_diy_float_Fp_mul is Fp::mul: the correctly rounded product of two numbers. */
StdFp
std_diy_float_Fp_mul(StdFp self, StdFp other)
{
	StdU128		wide = std_mul_u64_wide(self.f, other.f);
	uint64		lo = wide.lo;
	uint64		hi = wide.hi;
	uint64		f = hi + (lo >> 63);
	int16		e = (int16) (self.e + other.e + 64);
	StdFp		result = {f, e};

	return result;
}

/* std_diy_float_Fp_normalize is Fp::normalize: the number with its mantissa at least 2^63. */
StdFp
std_diy_float_Fp_normalize(StdFp self)
{
	uint32		lz = std_leading_zeros_u64(self.f);
	uint64		f = self.f << (lz & 63);
	int16		e = (int16) (self.e - (int16) lz);
	StdFp		result = {f, e};

	return result;
}

/* std_diy_float_Fp_normalize_to is Fp::normalize_to: the number at the exponent e, which is no greater. */
StdFp
std_diy_float_Fp_normalize_to(StdFp self, int16 e)
{
	int16		edelta = (int16) (self.e - e);
	size_t		edelta_2;
	uint64		left;
	StdFp		result;

	if (!(edelta >= 0))
		STD_FLT2DEC_PANIC(DIY_FLOAT_FILE, 44, 9, "assertion failed: edelta >= 0");
	edelta_2 = (size_t) edelta;
	left = self.f << (edelta_2 & 63) >> (edelta_2 & 63);
	if (!(left == self.f))
		std_assert_failed(DIY_FLOAT_FILE, 46, 9, "==", &left, std_u64_Debug_fmt, &self.f, std_u64_Debug_fmt, NULL);
	result.f = self.f << (edelta_2 & 63);
	result.e = e;
	return result;
}

/* std_numfmt_Part_len is Part::len: the bytes the part writes. */
size_t
std_numfmt_Part_len(const StdNumfmtPart *self)
{
	switch (self->kind)
	{
		case STD_NUMFMT_PART_ZERO:
			return self->u.zero;
		case STD_NUMFMT_PART_NUM:
			{
				uint32		ilog10 = 0;

				for (uint16 v = self->u.num; v >= 10; v /= 10)
					ilog10++;
				return (size_t) ilog10 + 1;
			}
		case STD_NUMFMT_PART_COPY:
			break;
	}
	return self->u.copy.len;
}

/* std_numfmt_Part_write is Part::write: the part's bytes written to out, false when out is too short. */
bool
std_numfmt_Part_write(const StdNumfmtPart *self, uint8 *out, size_t out_len, size_t *result)
{
	size_t		len = std_numfmt_Part_len(self);

	if (out_len >= len)
	{
		switch (self->kind)
		{
			case STD_NUMFMT_PART_ZERO:
				std_range_to_check(self->u.zero, out_len, NUMFMT_FILE, 38, 38);
				for (size_t i = 0; i < self->u.zero; i++)
					out[i] = '0';
				break;
			case STD_NUMFMT_PART_NUM:
				{
					uint16		v = self->u.num;

					std_range_to_check(len, out_len, NUMFMT_FILE, 43, 33);
					for (size_t i = len; i > 0; i--)
					{
						out[i - 1] = (uint8) ('0' + (uint8) (v % 10));
						v /= 10;
					}
				}
				break;
			case STD_NUMFMT_PART_COPY:
				std_range_to_check(self->u.copy.len, out_len, NUMFMT_FILE, 49, 24);
				std_slice_copy_from_slice_Unchecked(out, self->u.copy.len, self->u.copy.ptr, self->u.copy.len, 1, 1,
													NUMFMT_FILE, 49, 38);
				break;
		}
		*result = len;
		return true;
	}
	return false;
}

/* std_numfmt_Formatted_len is Formatted::len: the bytes of the sign and the parts, saturating. */
size_t
std_numfmt_Formatted_len(const StdNumfmtFormatted *self)
{
	size_t		len = self->sign.len;

	for (size_t i = 0; i < self->parts_len; i++)
		len = std_saturating_add_usize(len, std_numfmt_Part_len(&self->parts[i]));
	return len;
}

/* std_numfmt_Formatted_write is Formatted::write: the sign and the parts written to out, false when out is too short. */
bool
std_numfmt_Formatted_write(const StdNumfmtFormatted *self, uint8 *out, size_t out_len, size_t *result)
{
	size_t		written;

	if (self->sign.len > out_len)
		return false;
	std_slice_copy_from_slice_Unchecked(out, self->sign.len, self->sign.ptr, self->sign.len, 1, 1, NUMFMT_FILE, 85,
										41);
	written = self->sign.len;
	for (size_t i = 0; i < self->parts_len; i++)
	{
		size_t		len;

		std_range_from_check(written, out_len, NUMFMT_FILE, 89, 42);
		if (!std_numfmt_Part_write(&self->parts[i], out + written, out_len - written, &len))
			return false;
		written += len;
	}
	*result = written;
	return true;
}

/* std_bignum_U32_FullOps_full_mul_add is u32's FullOps::full_mul_add: self * other + other2 + carry as its carry and its digit. */
static uint32
std_bignum_U32_FullOps_full_mul_add(uint32 self, uint32 other, uint32 other2, uint32 carry, uint32 *result_1)
{
	uint64		wide = (uint64) self * other + other2 + carry;
	uint32		lo = (uint32) wide;
	uint32		hi = (uint32) (wide >> 32);

	*result_1 = lo;
	return hi;
}

/* std_bignum_U32_FullOps_full_div_rem is u32's FullOps::full_div_rem: borrow and self over other, as its quotient and remainder. */
static uint32
std_bignum_U32_FullOps_full_div_rem(uint32 self, uint32 other, uint32 borrow, uint32 *result_1)
{
	uint64		lhs = ((uint64) borrow << 32) | (uint64) self;
	uint64		rhs = (uint64) other;
	uint32		quotient = (uint32) std_div_u64(lhs, rhs, BIGNUM_FILE, 57, 1);

	*result_1 = (uint32) std_rem_u64(lhs, rhs, BIGNUM_FILE, 57, 1);
	return quotient;
}

/* std_bignum_Big32x40_from_small is Big32x40::from_small: a one-digit number. */
StdBignumBig32x40
std_bignum_Big32x40_from_small(uint32 v)
{
	StdBignumBig32x40 result = {0};

	result.base[0] = v;
	result.size = 1;
	return result;
}

/* std_bignum_Big32x40_from_u64 is Big32x40::from_u64: a u64's digits. */
StdBignumBig32x40
std_bignum_Big32x40_from_u64(uint64 v)
{
	StdBignumBig32x40 result = {0};
	size_t		sz = 0;

	while (v > 0)
	{
		std_index_check(sz, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
		result.base[sz] = (uint32) v;
		v >>= 32;
		sz += 1;
	}
	result.size = sz;
	return result;
}

/* std_bignum_Big32x40_is_zero is Big32x40::is_zero: whether every digit in use is zero. */
bool
std_bignum_Big32x40_is_zero(const StdBignumBig32x40 *self)
{
	std_range_to_check(self->size, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
	for (size_t i = 0; i < self->size; i++)
	{
		if (self->base[i] != 0)
			return false;
	}
	return true;
}

/* std_bignum_Big32x40_add is Big32x40::add: other added to self. */
StdBignumBig32x40 *
std_bignum_Big32x40_add(StdBignumBig32x40 *self, const StdBignumBig32x40 *other)
{
	size_t		sz = std_max_usize(self->size, other->size);
	bool		carry = false;

	std_range_to_check(sz, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
	std_range_to_check(sz, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
	for (size_t i = 0; i < sz; i++)
	{
		uint64		sum = (uint64) self->base[i] + other->base[i] + carry;

		self->base[i] = (uint32) sum;
		carry = (sum >> 32) != 0;
	}
	if (carry)
	{
		std_index_check(sz, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
		self->base[sz] = 1;
		sz += 1;
	}
	self->size = sz;
	return self;
}

/* std_bignum_Big32x40_sub is Big32x40::sub: other, no greater, subtracted from self. */
StdBignumBig32x40 *
std_bignum_Big32x40_sub(StdBignumBig32x40 *self, const StdBignumBig32x40 *other)
{
	size_t		sz = std_max_usize(self->size, other->size);
	bool		noborrow = true;

	std_range_to_check(sz, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
	std_range_to_check(sz, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
	for (size_t i = 0; i < sz; i++)
	{
		uint64		sum = (uint64) self->base[i] + (uint32) ~other->base[i] + noborrow;

		self->base[i] = (uint32) sum;
		noborrow = (sum >> 32) != 0;
	}
	if (!noborrow)
		STD_FLT2DEC_PANIC(BIGNUM_FILE, 388, 1, "assertion failed: noborrow");
	self->size = sz;
	return self;
}

/* std_bignum_Big32x40_mul_small is Big32x40::mul_small: self times a digit. */
StdBignumBig32x40 *
std_bignum_Big32x40_mul_small(StdBignumBig32x40 *self, uint32 other)
{
	size_t		sz = self->size;
	uint32		carry = 0;

	std_range_to_check(sz, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
	for (size_t i = 0; i < sz; i++)
	{
		uint64		product = (uint64) self->base[i] * other + carry;

		self->base[i] = (uint32) product;
		carry = (uint32) (product >> 32);
	}
	if (carry > 0)
	{
		std_index_check(sz, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
		self->base[sz] = carry;
		sz += 1;
	}
	self->size = sz;
	return self;
}

/* std_bignum_Big32x40_mul_pow2 is Big32x40::mul_pow2: self times 2^bits. */
StdBignumBig32x40 *
std_bignum_Big32x40_mul_pow2(StdBignumBig32x40 *self, size_t bits)
{
	size_t		digitbits = 32;
	size_t		digits = bits / digitbits;
	size_t		bits_2 = bits % digitbits;
	size_t		sz;

	if (!(digits < STD_BIGNUM_BIG32X40_N))
		STD_FLT2DEC_PANIC(BIGNUM_FILE, 388, 1, "assertion failed: digits < 40");
	for (size_t i = self->size; i > 0; i--)
	{
		uint32		digit;

		std_index_check(i - 1, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
		digit = self->base[i - 1];
		std_index_check(i - 1 + digits, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
		self->base[i - 1 + digits] = digit;
	}
	for (size_t i = 0; i < digits; i++)
	{
		std_index_check(i, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
		self->base[i] = 0;
	}
	sz = self->size + digits;
	if (bits_2 > 0)
	{
		size_t		last = sz;
		uint32		overflow;

		std_index_check(last - 1, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
		overflow = self->base[last - 1] >> (digitbits - bits_2);
		if (overflow > 0)
		{
			std_index_check(last, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
			self->base[last] = overflow;
			sz += 1;
		}
		for (size_t i = last; i > digits + 1; i--)
		{
			uint32		high;
			uint32		low;

			std_index_check(i - 1, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
			high = self->base[i - 1];
			std_index_check(i - 2, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
			low = self->base[i - 2];
			std_index_check(i - 1, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
			self->base[i - 1] = (high << bits_2) | (low >> (digitbits - bits_2));
		}
		std_index_check(digits, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
		self->base[digits] <<= bits_2;
	}
	self->size = sz;
	return self;
}

/* std_bignum_Big32x40_mul_digits_mul_inner is mul_digits' mul_inner: aa times bb into ret, returning its size. */
static size_t
std_bignum_Big32x40_mul_digits_mul_inner(uint32 *ret, const uint32 *aa, size_t aa_len, const uint32 *bb,
										 size_t bb_len)
{
	size_t		retsz = 0;

	for (size_t i = 0; i < aa_len; i++)
	{
		uint32		a = aa[i];
		size_t		sz;
		uint32		carry = 0;

		if (a == 0)
			continue;
		sz = bb_len;
		for (size_t j = 0; j < bb_len; j++)
		{
			uint32		b = bb[j];
			uint32		c;
			uint32		v;

			std_index_check(i + j, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
			c = std_bignum_U32_FullOps_full_mul_add(a, b, ret[i + j], carry, &v);
			std_index_check(i + j, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
			ret[i + j] = v;
			carry = c;
		}
		if (carry > 0)
		{
			std_index_check(i + sz, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
			ret[i + sz] = carry;
			sz += 1;
		}
		if (retsz < i + sz)
			retsz = i + sz;
	}
	return retsz;
}

/* std_bignum_Big32x40_mul_digits is Big32x40::mul_digits: self times the digits of other. */
StdBignumBig32x40 *
std_bignum_Big32x40_mul_digits(StdBignumBig32x40 *self, const uint32 *other, size_t other_len)
{
	uint32		ret[STD_BIGNUM_BIG32X40_N] = {0};
	size_t		retsz;

	if (self->size < other_len)
	{
		std_range_to_check(self->size, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
		retsz = std_bignum_Big32x40_mul_digits_mul_inner(ret, self->base, self->size, other, other_len);
	}
	else
	{
		std_range_to_check(self->size, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
		retsz = std_bignum_Big32x40_mul_digits_mul_inner(ret, other, other_len, self->base, self->size);
	}
	memcpy(self->base, ret, sizeof(ret));
	self->size = retsz;
	return self;
}

/* std_bignum_Big32x40_div_rem_small is Big32x40::div_rem_small: self divided by a digit, returning the remainder in result_1. */
StdBignumBig32x40 *
std_bignum_Big32x40_div_rem_small(StdBignumBig32x40 *self, uint32 other, uint32 *result_1)
{
	size_t		sz;
	uint32		borrow = 0;

	if (!(other > 0))
		STD_FLT2DEC_PANIC(BIGNUM_FILE, 388, 1, "assertion failed: other > 0");
	sz = self->size;
	std_range_to_check(sz, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
	for (size_t i = sz; i > 0; i--)
	{
		uint32		r;
		uint32		q = std_bignum_U32_FullOps_full_div_rem(self->base[i - 1], other, borrow, &r);

		self->base[i - 1] = q;
		borrow = r;
	}
	*result_1 = borrow;
	return self;
}

/* std_bignum_Big32x40_Ord_cmp is Big32x40's Ord::cmp: the digits compared from the most significant. */
StdOrdering
std_bignum_Big32x40_Ord_cmp(const StdBignumBig32x40 *self, const StdBignumBig32x40 *other)
{
	size_t		sz = std_max_usize(self->size, other->size);

	std_range_to_check(sz, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
	std_range_to_check(sz, STD_BIGNUM_BIG32X40_N, BIGNUM_FILE, 388, 1);
	for (size_t i = sz; i > 0; i--)
	{
		if (self->base[i - 1] != other->base[i - 1])
			return self->base[i - 1] < other->base[i - 1] ? STD_ORDERING_LESS : STD_ORDERING_GREATER;
	}
	return STD_ORDERING_EQUAL;
}

/*
 * std_flt2dec_round_up is flt2dec::round_up: the last digit increased with
 * its carry, true with the digit to append in result when the carry runs
 * out of digits.
 */
bool
std_flt2dec_round_up(uint8 *d, size_t d_len, uint8 *result)
{
	StdUsizeOption position = {0};

	for (size_t i = d_len; i > 0; i--)
	{
		if (d[i - 1] != '9')
		{
			position.has = true;
			position.x = i - 1;
			break;
		}
	}
	if (position.has)
	{
		size_t		i = position.x;

		std_index_check(i, d_len, FLT2DEC_FILE, 152, 13);
		d[i] += 1;
		std_range_from_check(i + 1, d_len, FLT2DEC_FILE, 153, 14);
		memset(d + i + 1, '0', d_len - (i + 1));
		return false;
	}
	else if (d_len == 0)
	{
		*result = '1';
		return true;
	}
	else
	{
		std_index_check(0, d_len, FLT2DEC_FILE, 162, 13);
		d[0] = '1';
		std_range_from_check(1, d_len, FLT2DEC_FILE, 163, 14);
		memset(d + 1, '0', d_len - 1);
		*result = '0';
		return true;
	}
}

/* STD_FLT2DEC_COPY is Part::Copy of a literal. */
#define STD_FLT2DEC_COPY(literal) \
	((StdNumfmtPart) {.kind = STD_NUMFMT_PART_COPY, .u.copy = {(const uint8 *) (literal), sizeof(literal) - 1}})

/* STD_FLT2DEC_COPY_BYTES is Part::Copy of a slice. */
#define STD_FLT2DEC_COPY_BYTES(p, n) ((StdNumfmtPart) {.kind = STD_NUMFMT_PART_COPY, .u.copy = {(p), (n)}})

/* STD_FLT2DEC_ZERO is Part::Zero. */
#define STD_FLT2DEC_ZERO(n) ((StdNumfmtPart) {.kind = STD_NUMFMT_PART_ZERO, .u.zero = (n)})

/* STD_FLT2DEC_NUM is Part::Num. */
#define STD_FLT2DEC_NUM(n) ((StdNumfmtPart) {.kind = STD_NUMFMT_PART_NUM, .u.num = (n)})

/*
 * std_flt2dec_digits_to_dec_str is flt2dec::digits_to_dec_str: the parts of
 * 0.<buf> * 10^exp in decimal with at least frac_digits fractional digits,
 * returning how many it wrote.
 */
size_t
std_flt2dec_digits_to_dec_str(const uint8 *buf, size_t buf_len, int16 exp, size_t frac_digits,
							  StdNumfmtPart *parts, size_t parts_len)
{
	if (!(buf_len != 0))
		STD_FLT2DEC_PANIC(FLT2DEC_FILE, 183, 5, "assertion failed: !buf.is_empty()");
	std_index_check(0, buf_len, FLT2DEC_FILE, 184, 13);
	if (!(buf[0] > '0'))
		STD_FLT2DEC_PANIC(FLT2DEC_FILE, 184, 5, "assertion failed: buf[0] > b'0'");
	if (!(parts_len >= 4))
		STD_FLT2DEC_PANIC(FLT2DEC_FILE, 185, 5, "assertion failed: parts.len() >= 4");
	if (exp <= 0)
	{
		size_t		minus_exp = (size_t) (-(int32) exp);

		std_index_check(0, parts_len, FLT2DEC_FILE, 203, 9);
		parts[0] = STD_FLT2DEC_COPY("0.");
		std_index_check(1, parts_len, FLT2DEC_FILE, 204, 9);
		parts[1] = STD_FLT2DEC_ZERO(minus_exp);
		std_index_check(2, parts_len, FLT2DEC_FILE, 205, 9);
		parts[2] = STD_FLT2DEC_COPY_BYTES(buf, buf_len);
		if (frac_digits > buf_len && frac_digits - buf_len > minus_exp)
		{
			std_index_check(3, parts_len, FLT2DEC_FILE, 207, 13);
			parts[3] = STD_FLT2DEC_ZERO((frac_digits - buf_len) - minus_exp);
			std_range_to_check(4, parts_len, FLT2DEC_FILE, 209, 27);
			return 4;
		}
		else
		{
			std_range_to_check(3, parts_len, FLT2DEC_FILE, 212, 27);
			return 3;
		}
	}
	else
	{
		size_t		exp_2 = (size_t) exp;

		if (exp_2 < buf_len)
		{
			std_range_to_check(exp_2, buf_len, FLT2DEC_FILE, 218, 56);
			std_index_check(0, parts_len, FLT2DEC_FILE, 218, 13);
			parts[0] = STD_FLT2DEC_COPY_BYTES(buf, exp_2);
			std_index_check(1, parts_len, FLT2DEC_FILE, 219, 13);
			parts[1] = STD_FLT2DEC_COPY(".");
			std_range_from_check(exp_2, buf_len, FLT2DEC_FILE, 220, 56);
			std_index_check(2, parts_len, FLT2DEC_FILE, 220, 13);
			parts[2] = STD_FLT2DEC_COPY_BYTES(buf + exp_2, buf_len - exp_2);
			if (frac_digits > buf_len - exp_2)
			{
				std_index_check(3, parts_len, FLT2DEC_FILE, 222, 17);
				parts[3] = STD_FLT2DEC_ZERO(frac_digits - (buf_len - exp_2));
				std_range_to_check(4, parts_len, FLT2DEC_FILE, 224, 31);
				return 4;
			}
			else
			{
				std_range_to_check(3, parts_len, FLT2DEC_FILE, 227, 31);
				return 3;
			}
		}
		else
		{
			std_index_check(0, parts_len, FLT2DEC_FILE, 231, 13);
			parts[0] = STD_FLT2DEC_COPY_BYTES(buf, buf_len);
			std_index_check(1, parts_len, FLT2DEC_FILE, 232, 13);
			parts[1] = STD_FLT2DEC_ZERO(exp_2 - buf_len);
			if (frac_digits > 0)
			{
				std_index_check(2, parts_len, FLT2DEC_FILE, 234, 17);
				parts[2] = STD_FLT2DEC_COPY(".");
				std_index_check(3, parts_len, FLT2DEC_FILE, 235, 17);
				parts[3] = STD_FLT2DEC_ZERO(frac_digits);
				std_range_to_check(4, parts_len, FLT2DEC_FILE, 237, 31);
				return 4;
			}
			else
			{
				std_range_to_check(2, parts_len, FLT2DEC_FILE, 240, 31);
				return 2;
			}
		}
	}
}

/*
 * std_flt2dec_digits_to_exp_str is flt2dec::digits_to_exp_str: the parts of
 * 0.<buf> * 10^exp in exponential form with at least frac_digits fractional
 * digits, `E` for the exponent when upper, returning how many it wrote.
 */
size_t
std_flt2dec_digits_to_exp_str(const uint8 *buf, size_t buf_len, int16 exp, size_t frac_digits, bool upper,
							  StdNumfmtPart *parts, size_t parts_len)
{
	size_t		n = 0;
	size_t		actual_frac_digits;
	int32		exp_2;

	if (!(buf_len != 0))
		STD_FLT2DEC_PANIC(FLT2DEC_FILE, 265, 5, "assertion failed: !buf.is_empty()");
	std_index_check(0, buf_len, FLT2DEC_FILE, 266, 13);
	if (!(buf[0] > '0'))
		STD_FLT2DEC_PANIC(FLT2DEC_FILE, 266, 5, "assertion failed: buf[0] > b'0'");
	if (!(parts_len >= 6))
		STD_FLT2DEC_PANIC(FLT2DEC_FILE, 267, 5, "assertion failed: parts.len() >= 6");
	std_range_to_check(1, buf_len, FLT2DEC_FILE, 271, 48);
	std_index_check(n, parts_len, FLT2DEC_FILE, 271, 5);
	parts[n] = STD_FLT2DEC_COPY_BYTES(buf, 1);
	n += 1;
	actual_frac_digits = buf_len - 1;
	if (actual_frac_digits > 0 || frac_digits > 0)
	{
		std_index_check(n, parts_len, FLT2DEC_FILE, 280, 9);
		parts[n] = STD_FLT2DEC_COPY(".");
		n += 1;
		if (actual_frac_digits > 0)
		{
			std_range_from_check(1, buf_len, FLT2DEC_FILE, 283, 56);
			std_index_check(n, parts_len, FLT2DEC_FILE, 283, 13);
			parts[n] = STD_FLT2DEC_COPY_BYTES(buf + 1, buf_len - 1);
			n += 1;
		}
		if (frac_digits > actual_frac_digits)
		{
			std_index_check(n, parts_len, FLT2DEC_FILE, 289, 13);
			parts[n] = STD_FLT2DEC_ZERO(frac_digits - actual_frac_digits);
			n += 1;
		}
	}
	exp_2 = (int32) exp - 1;
	if (exp_2 < 0)
	{
		std_index_check(n, parts_len, FLT2DEC_FILE, 297, 9);
		parts[n] = upper ? STD_FLT2DEC_COPY("E-") : STD_FLT2DEC_COPY("e-");
		std_index_check(n + 1, parts_len, FLT2DEC_FILE, 298, 9);
		parts[n + 1] = STD_FLT2DEC_NUM((uint16) -exp_2);
	}
	else
	{
		std_index_check(n, parts_len, FLT2DEC_FILE, 300, 9);
		parts[n] = upper ? STD_FLT2DEC_COPY("E") : STD_FLT2DEC_COPY("e");
		std_index_check(n + 1, parts_len, FLT2DEC_FILE, 301, 9);
		parts[n + 1] = STD_FLT2DEC_NUM((uint16) exp_2);
	}
	std_range_to_check(n + 2, parts_len, FLT2DEC_FILE, 304, 19);
	return n + 2;
}

/* std_flt2dec_determine_sign is flt2dec::determine_sign: "", "-", or "+" by the sign option. */
StdBytes
std_flt2dec_determine_sign(StdFlt2decSign sign, const StdFullDecoded *decoded, bool negative)
{
	static const uint8 minus[] = "-";
	static const uint8 plus[] = "+";
	StdBytes	none = {minus, 0};
	StdBytes	negative_sign = {minus, 1};
	StdBytes	positive_sign = {plus, 1};

	if (decoded->kind == STD_FULL_DECODED_NAN)
		return none;
	if (sign == STD_FLT2DEC_SIGN_MINUS)
		return negative ? negative_sign : none;
	return negative ? negative_sign : positive_sign;
}

/* std_flt2dec_estimate_max_buf_len is flt2dec::estimate_max_buf_len: an upper bound of the digits format_exact writes. */
size_t
std_flt2dec_estimate_max_buf_len(int16 exp)
{
	return 21 + ((size_t) (int64) ((exp < 0 ? -12 : 5) * (int32) exp) >> 4);
}

/* std_flt2dec_estimator_estimate_scaling_factor is estimator::estimate_scaling_factor: k_0 with 10^(k_0-1) < mant * 2^exp <= 10^(k_0+1). */
int16
std_flt2dec_estimator_estimate_scaling_factor(uint64 mant, int16 exp)
{
	int64		nbits = 64 - (int64) std_leading_zeros_u64(mant - 1);

	return (int16) (((nbits + (int64) exp) * 1292913986) >> 32);
}

/* STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10 is Grisu's CACHED_POW10: each power of ten's mantissa, binary exponent, and decimal exponent. */
const StdTupleU64I16I16 STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10[STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_LEN] = {
	{UINT64CONST(0xe61acf033d1a45df), -1087, -308},
	{UINT64CONST(0xab70fe17c79ac6ca), -1060, -300},
	{UINT64CONST(0xff77b1fcbebcdc4f), -1034, -292},
	{UINT64CONST(0xbe5691ef416bd60c), -1007, -284},
	{UINT64CONST(0x8dd01fad907ffc3c), -980, -276},
	{UINT64CONST(0xd3515c2831559a83), -954, -268},
	{UINT64CONST(0x9d71ac8fada6c9b5), -927, -260},
	{UINT64CONST(0xea9c227723ee8bcb), -901, -252},
	{UINT64CONST(0xaecc49914078536d), -874, -244},
	{UINT64CONST(0x823c12795db6ce57), -847, -236},
	{UINT64CONST(0xc21094364dfb5637), -821, -228},
	{UINT64CONST(0x9096ea6f3848984f), -794, -220},
	{UINT64CONST(0xd77485cb25823ac7), -768, -212},
	{UINT64CONST(0xa086cfcd97bf97f4), -741, -204},
	{UINT64CONST(0xef340a98172aace5), -715, -196},
	{UINT64CONST(0xb23867fb2a35b28e), -688, -188},
	{UINT64CONST(0x84c8d4dfd2c63f3b), -661, -180},
	{UINT64CONST(0xc5dd44271ad3cdba), -635, -172},
	{UINT64CONST(0x936b9fcebb25c996), -608, -164},
	{UINT64CONST(0xdbac6c247d62a584), -582, -156},
	{UINT64CONST(0xa3ab66580d5fdaf6), -555, -148},
	{UINT64CONST(0xf3e2f893dec3f126), -529, -140},
	{UINT64CONST(0xb5b5ada8aaff80b8), -502, -132},
	{UINT64CONST(0x87625f056c7c4a8b), -475, -124},
	{UINT64CONST(0xc9bcff6034c13053), -449, -116},
	{UINT64CONST(0x964e858c91ba2655), -422, -108},
	{UINT64CONST(0xdff9772470297ebd), -396, -100},
	{UINT64CONST(0xa6dfbd9fb8e5b88f), -369, -92},
	{UINT64CONST(0xf8a95fcf88747d94), -343, -84},
	{UINT64CONST(0xb94470938fa89bcf), -316, -76},
	{UINT64CONST(0x8a08f0f8bf0f156b), -289, -68},
	{UINT64CONST(0xcdb02555653131b6), -263, -60},
	{UINT64CONST(0x993fe2c6d07b7fac), -236, -52},
	{UINT64CONST(0xe45c10c42a2b3b06), -210, -44},
	{UINT64CONST(0xaa242499697392d3), -183, -36},
	{UINT64CONST(0xfd87b5f28300ca0e), -157, -28},
	{UINT64CONST(0xbce5086492111aeb), -130, -20},
	{UINT64CONST(0x8cbccc096f5088cc), -103, -12},
	{UINT64CONST(0xd1b71758e219652c), -77, -4},
	{UINT64CONST(0x9c40000000000000), -50, 4},
	{UINT64CONST(0xe8d4a51000000000), -24, 12},
	{UINT64CONST(0xad78ebc5ac620000), 3, 20},
	{UINT64CONST(0x813f3978f8940984), 30, 28},
	{UINT64CONST(0xc097ce7bc90715b3), 56, 36},
	{UINT64CONST(0x8f7e32ce7bea5c70), 83, 44},
	{UINT64CONST(0xd5d238a4abe98068), 109, 52},
	{UINT64CONST(0x9f4f2726179a2245), 136, 60},
	{UINT64CONST(0xed63a231d4c4fb27), 162, 68},
	{UINT64CONST(0xb0de65388cc8ada8), 189, 76},
	{UINT64CONST(0x83c7088e1aab65db), 216, 84},
	{UINT64CONST(0xc45d1df942711d9a), 242, 92},
	{UINT64CONST(0x924d692ca61be758), 269, 100},
	{UINT64CONST(0xda01ee641a708dea), 295, 108},
	{UINT64CONST(0xa26da3999aef774a), 322, 116},
	{UINT64CONST(0xf209787bb47d6b85), 348, 124},
	{UINT64CONST(0xb454e4a179dd1877), 375, 132},
	{UINT64CONST(0x865b86925b9bc5c2), 402, 140},
	{UINT64CONST(0xc83553c5c8965d3d), 428, 148},
	{UINT64CONST(0x952ab45cfa97a0b3), 455, 156},
	{UINT64CONST(0xde469fbd99a05fe3), 481, 164},
	{UINT64CONST(0xa59bc234db398c25), 508, 172},
	{UINT64CONST(0xf6c69a72a3989f5c), 534, 180},
	{UINT64CONST(0xb7dcbf5354e9bece), 561, 188},
	{UINT64CONST(0x88fcf317f22241e2), 588, 196},
	{UINT64CONST(0xcc20ce9bd35c78a5), 614, 204},
	{UINT64CONST(0x98165af37b2153df), 641, 212},
	{UINT64CONST(0xe2a0b5dc971f303a), 667, 220},
	{UINT64CONST(0xa8d9d1535ce3b396), 694, 228},
	{UINT64CONST(0xfb9b7cd9a4a7443c), 720, 236},
	{UINT64CONST(0xbb764c4ca7a44410), 747, 244},
	{UINT64CONST(0x8bab8eefb6409c1a), 774, 252},
	{UINT64CONST(0xd01fef10a657842c), 800, 260},
	{UINT64CONST(0x9b10a4e5e9913129), 827, 268},
	{UINT64CONST(0xe7109bfba19c0c9d), 853, 276},
	{UINT64CONST(0xac2820d9623bf429), 880, 284},
	{UINT64CONST(0x80444b5e7aa7cf85), 907, 292},
	{UINT64CONST(0xbf21e44003acdd2d), 933, 300},
	{UINT64CONST(0x8e679c2f5e44ff8f), 960, 308},
	{UINT64CONST(0xd433179d9c8cb841), 986, 316},
	{UINT64CONST(0x9e19db92b4e31ba9), 1013, 324},
	{UINT64CONST(0xeb96bf6ebadf77d9), 1039, 332},
};

/* std_flt2dec_strategy_grisu_cached_power is grisu::cached_power: the power of ten whose exponent is near gamma, and its decimal exponent. */
int16
std_flt2dec_strategy_grisu_cached_power(int16 alpha, int16 gamma, StdFp *result_1)
{
	int32		offset = STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_FIRST_E;
	int32		range = (int32) lengthof(STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10) - 1;
	int32		domain = STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_LAST_E - STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10_FIRST_E;
	int32		idx = ((int32) gamma - offset) * range / domain;

	std_index_check((size_t) (ssize_t) idx, lengthof(STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10), GRISU_FILE, 127, 21);
	result_1->f = STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10[idx].f0;
	result_1->e = STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10[idx].f1;
	return STD_FLT2DEC_STRATEGY_GRISU_CACHED_POW10[idx].f2;
}

/* std_flt2dec_strategy_grisu_max_pow10_no_more_than is grisu::max_pow10_no_more_than: the largest power of ten no greater than x, with its exponent. */
uint8
std_flt2dec_strategy_grisu_max_pow10_no_more_than(uint32 x, uint32 *result_1)
{
	const uint32 X9 = 1000000000;
	const uint32 X8 = 100000000;
	const uint32 X7 = 10000000;
	const uint32 X6 = 1000000;
	const uint32 X5 = 100000;
	const uint32 X4 = 10000;
	const uint32 X3 = 1000;
	const uint32 X2 = 100;
	const uint32 X1 = 10;

	if (x < X4)
	{
		if (x < X2)
		{
			if (x < X1)
			{
				*result_1 = 1;
				return 0;
			}
			*result_1 = X1;
			return 1;
		}
		if (x < X3)
		{
			*result_1 = X2;
			return 2;
		}
		*result_1 = X3;
		return 3;
	}
	if (x < X6)
	{
		if (x < X5)
		{
			*result_1 = X4;
			return 4;
		}
		*result_1 = X5;
		return 5;
	}
	else if (x < X8)
	{
		if (x < X7)
		{
			*result_1 = X6;
			return 6;
		}
		*result_1 = X7;
		return 7;
	}
	if (x < X9)
	{
		*result_1 = X8;
		return 8;
	}
	*result_1 = X9;
	return 9;
}

/*
 * std_flt2dec_strategy_grisu_format_shortest_opt_round_and_weed is
 * format_shortest_opt's round_and_weed: the last digit moved down toward v
 * while that stays safe, and the digits when the result is provably the
 * shortest and closest, false when Grisu can't tell.
 */
static bool
std_flt2dec_strategy_grisu_format_shortest_opt_round_and_weed(uint8 *buf, size_t buf_len, int16 exp,
															  uint64 remainder, uint64 threshold, uint64 plus1v,
															  uint64 ten_kappa, uint64 ulp, size_t *result,
															  int16 *result_1)
{
	uint64		plus1v_down;
	uint64		plus1v_up;
	uint64		plus1w;

	if (!(buf_len != 0))
		STD_FLT2DEC_PANIC(GRISU_FILE, 368, 9, "assertion failed: !buf.is_empty()");
	plus1v_down = plus1v + ulp;
	plus1v_up = plus1v - ulp;
	plus1w = remainder;
	{
		uint8	   *last;

		if (buf_len == 0)
			std_option_unwrap_failed(GRISU_FILE, 381, 39);
		last = &buf[buf_len - 1];
		while (plus1w < plus1v_up && threshold - plus1w >= ten_kappa &&
			   (plus1w + ten_kappa < plus1v_up || plus1v_up - plus1w >= plus1w + ten_kappa - plus1v_up))
		{
			*last -= 1;
			plus1w += ten_kappa;
		}
	}
	if (plus1w < plus1v_down && threshold - plus1w >= ten_kappa &&
		(plus1w + ten_kappa < plus1v_down || plus1v_down - plus1w >= plus1w + ten_kappa - plus1v_down))
		return false;
	if (2 * ulp <= plus1w && plus1w <= threshold - 4 * ulp)
	{
		*result = buf_len;
		*result_1 = exp;
		return true;
	}
	return false;
}

/*
 * std_flt2dec_strategy_grisu_format_shortest_opt is
 * grisu::format_shortest_opt: Grisu's shortest digits and exponent, false
 * when it can't prove them.
 */
bool
std_flt2dec_strategy_grisu_format_shortest_opt(const StdDecoded *d, uint8 *buf, size_t buf_len, size_t *result,
											   int16 *result_1)
{
	StdFp		plus;
	StdFp		minus;
	StdFp		v;
	StdFp		cached;
	int16		minusk;
	StdFp		plus_2;
	StdFp		minus_2;
	StdFp		v_2;
	uint64		plus1;
	uint64		minus1;
	size_t		e;
	uint32		plus1int;
	uint64		plus1frac;
	uint8		max_kappa;
	uint32		max_ten_kappa;
	size_t		i = 0;
	int16		exp;
	uint64		delta1;
	uint64		delta1frac;
	uint32		ten_kappa;
	uint32		remainder;
	uint64		remainder_2;
	uint64		threshold;
	uint64		ulp;

	if (!(d->mant > 0))
		STD_FLT2DEC_PANIC(GRISU_FILE, 171, 5, "assertion failed: d.mant > 0");
	if (!(d->minus > 0))
		STD_FLT2DEC_PANIC(GRISU_FILE, 172, 5, "assertion failed: d.minus > 0");
	if (!(d->plus > 0))
		STD_FLT2DEC_PANIC(GRISU_FILE, 173, 5, "assertion failed: d.plus > 0");
	if (!(d->mant + d->plus >= d->mant))
		STD_FLT2DEC_PANIC(GRISU_FILE, 174, 5, "assertion failed: d.mant.checked_add(d.plus).is_some()");
	if (!(d->minus <= d->mant))
		STD_FLT2DEC_PANIC(GRISU_FILE, 175, 5, "assertion failed: d.mant.checked_sub(d.minus).is_some()");
	if (!(buf_len >= STD_FLT2DEC_MAX_SIG_DIGITS))
		STD_FLT2DEC_PANIC(GRISU_FILE, 176, 5, "assertion failed: buf.len() >= MAX_SIG_DIGITS");
	if (!(d->mant + d->plus < (UINT64CONST(1) << 61)))
		STD_FLT2DEC_PANIC(GRISU_FILE, 177, 5, "assertion failed: d.mant + d.plus < (1 << 61)");
	plus.f = d->mant + d->plus;
	plus.e = d->exp;
	plus = std_diy_float_Fp_normalize(plus);
	minus.f = d->mant - d->minus;
	minus.e = d->exp;
	minus = std_diy_float_Fp_normalize_to(minus, plus.e);
	v.f = d->mant;
	v.e = d->exp;
	v = std_diy_float_Fp_normalize_to(v, plus.e);
	minusk = std_flt2dec_strategy_grisu_cached_power((int16) (STD_FLT2DEC_STRATEGY_GRISU_ALPHA - plus.e - 64),
													 (int16) (STD_FLT2DEC_STRATEGY_GRISU_GAMMA - plus.e - 64),
													 &cached);
	plus_2 = std_diy_float_Fp_mul(plus, cached);
	minus_2 = std_diy_float_Fp_mul(minus, cached);
	v_2 = std_diy_float_Fp_mul(v, cached);
	plus1 = plus_2.f + 1;
	minus1 = minus_2.f - 1;
	e = (size_t) (int16) -plus_2.e;
	plus1int = (uint32) (plus1 >> e);
	plus1frac = plus1 & ((UINT64CONST(1) << e) - 1);
	max_kappa = std_flt2dec_strategy_grisu_max_pow10_no_more_than(plus1int, &max_ten_kappa);
	exp = (int16) ((int16) max_kappa - minusk + 1);
	delta1 = plus1 - minus1;
	delta1frac = delta1 & ((UINT64CONST(1) << e) - 1);
	ten_kappa = max_ten_kappa;
	remainder = plus1int;
	for (;;)
	{
		uint32		q = std_div_u32(remainder, ten_kappa, GRISU_FILE, 268, 17);
		uint32		r = std_rem_u32(remainder, ten_kappa, GRISU_FILE, 269, 17);
		uint64		plus1rem;

		std_index_check(i, buf_len, GRISU_FILE, 271, 9);
		buf[i] = (uint8) ('0' + (uint8) q);
		i += 1;
		plus1rem = ((uint64) r << e) + plus1frac;
		if (plus1rem < delta1)
		{
			uint64		ten_kappa_2 = (uint64) ten_kappa << e;

			std_range_to_check(i, buf_len, GRISU_FILE, 280, 29);
			return std_flt2dec_strategy_grisu_format_shortest_opt_round_and_weed(buf, i, exp, plus1rem, delta1,
																				 plus1 - v_2.f, ten_kappa_2, 1,
																				 result, result_1);
		}
		if (i > (size_t) max_kappa)
			break;
		ten_kappa /= 10;
		remainder = r;
	}
	remainder_2 = plus1frac;
	threshold = delta1frac;
	ulp = 1;
	for (;;)
	{
		uint64		q;
		uint64		r;

		remainder_2 *= 10;
		threshold *= 10;
		ulp *= 10;
		q = remainder_2 >> e;
		r = remainder_2 & ((UINT64CONST(1) << e) - 1);
		std_index_check(i, buf_len, GRISU_FILE, 322, 9);
		buf[i] = (uint8) ('0' + (uint8) q);
		i += 1;
		if (r < threshold)
		{
			uint64		ten_kappa_3 = UINT64CONST(1) << e;

			std_range_to_check(i, buf_len, GRISU_FILE, 329, 29);
			return std_flt2dec_strategy_grisu_format_shortest_opt_round_and_weed(buf, i, exp, r, threshold,
																				 (plus1 - v_2.f) * ulp, ten_kappa_3,
																				 ulp, result, result_1);
		}
		remainder_2 = r;
	}
}

/* std_flt2dec_strategy_grisu_format_shortest is grisu::format_shortest: Grisu's shortest digits, or Dragon's when Grisu can't prove its own. */
size_t
std_flt2dec_strategy_grisu_format_shortest(const StdDecoded *d, uint8 *buf, size_t buf_len, int16 *result_1)
{
	size_t		len;

	if (std_flt2dec_strategy_grisu_format_shortest_opt(d, buf, buf_len, &len, result_1))
		return len;
	return std_flt2dec_strategy_dragon_format_shortest(d, buf, buf_len, result_1);
}

/*
 * std_flt2dec_strategy_grisu_format_exact_opt_possibly_round is
 * format_exact_opt's possibly_round: the first len digits, rounded up when
 * the remainder says so, false when the error leaves the rounding unknown.
 */
static bool
std_flt2dec_strategy_grisu_format_exact_opt_possibly_round(uint8 *buf, size_t buf_len, size_t len, int16 exp,
														   int16 limit, uint64 remainder, uint64 ten_kappa,
														   uint64 ulp, size_t *result, int16 *result_1)
{
	if (ulp >= ten_kappa)
		return false;
	if (ten_kappa - ulp <= ulp)
		return false;
	if (ten_kappa - remainder > remainder && ten_kappa - 2 * remainder >= 2 * ulp)
	{
		std_range_to_check(len, buf_len, GRISU_FILE, 718, 38);
		*result = len;
		*result_1 = exp;
		return true;
	}
	if (remainder > ulp && ten_kappa - (remainder - ulp) <= remainder - ulp)
	{
		uint8		c;

		std_range_to_check(len, buf_len, GRISU_FILE, 741, 38);
		if (std_flt2dec_round_up(buf, len, &c))
		{
			exp += 1;
			if (exp > limit && len < buf_len)
			{
				std_index_check(len, buf_len, GRISU_FILE, 748, 21);
				buf[len] = c;
				len += 1;
			}
		}
		std_range_to_check(len, buf_len, GRISU_FILE, 753, 38);
		*result = len;
		*result_1 = exp;
		return true;
	}
	return false;
}

/*
 * std_flt2dec_strategy_grisu_format_exact_opt is grisu::format_exact_opt:
 * Grisu's digits filling buf, or up to the limit's place, false when it
 * can't prove them.
 */
bool
std_flt2dec_strategy_grisu_format_exact_opt(const StdDecoded *d, uint8 *buf, size_t buf_len, int16 limit,
											size_t *result, int16 *result_1)
{
	static const uint32 STD_FLT2DEC_STRATEGY_GRISU_FORMAT_EXACT_OPT_POW10_UP_TO_9[10] = {
		1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000,
	};
	StdFp		v;
	StdFp		cached;
	int16		minusk;
	StdFp		v_2;
	size_t		e;
	uint32		vint;
	uint64		vfrac;
	size_t		requested_digits;
	uint64		err;
	uint8		max_kappa;
	uint32		max_ten_kappa;
	size_t		i = 0;
	int16		exp;
	size_t		len;
	uint32		ten_kappa;
	uint32		remainder;
	uint64		remainder_2;
	uint64		maxerr;

	if (!(d->mant > 0))
		STD_FLT2DEC_PANIC(GRISU_FILE, 478, 5, "assertion failed: d.mant > 0");
	if (!(d->mant < (UINT64CONST(1) << 61)))
		STD_FLT2DEC_PANIC(GRISU_FILE, 479, 5, "assertion failed: d.mant < (1 << 61)");
	if (!(buf_len != 0))
		STD_FLT2DEC_PANIC(GRISU_FILE, 480, 5, "assertion failed: !buf.is_empty()");
	v.f = d->mant;
	v.e = d->exp;
	v = std_diy_float_Fp_normalize(v);
	minusk = std_flt2dec_strategy_grisu_cached_power((int16) (STD_FLT2DEC_STRATEGY_GRISU_ALPHA - v.e - 64),
													 (int16) (STD_FLT2DEC_STRATEGY_GRISU_GAMMA - v.e - 64),
													 &cached);
	v_2 = std_diy_float_Fp_mul(v, cached);
	e = (size_t) (int16) -v_2.e;
	vint = (uint32) (v_2.f >> e);
	vfrac = v_2.f & ((UINT64CONST(1) << e) - 1);
	requested_digits = buf_len;
	if (vfrac == 0)
	{
		bool		tmp_1;

		if (requested_digits >= 11)
			tmp_1 = true;
		else
		{
			std_index_check(requested_digits - 1, 10, GRISU_FILE, 504, 61);
			tmp_1 = vint < STD_FLT2DEC_STRATEGY_GRISU_FORMAT_EXACT_OPT_POW10_UP_TO_9[requested_digits - 1];
		}
		if (tmp_1)
			return false;
	}
	err = 1;
	max_kappa = std_flt2dec_strategy_grisu_max_pow10_no_more_than(vint, &max_ten_kappa);
	exp = (int16) ((int16) max_kappa - minusk + 1);
	if (exp <= limit)
		return std_flt2dec_strategy_grisu_format_exact_opt_possibly_round(buf, buf_len, 0, exp, limit, v_2.f / 10,
																		  (uint64) max_ten_kappa << e, err << e,
																		  result, result_1);
	else if ((size_t) ((int32) exp - (int32) limit) < buf_len)
		len = (size_t) (int16) (exp - limit);
	else
		len = buf_len;
	ten_kappa = max_ten_kappa;
	remainder = vint;
	for (;;)
	{
		uint32		q = std_div_u32(remainder, ten_kappa, GRISU_FILE, 565, 17);
		uint32		r = std_rem_u32(remainder, ten_kappa, GRISU_FILE, 566, 17);

		std_index_check(i, buf_len, GRISU_FILE, 568, 9);
		buf[i] = (uint8) ('0' + (uint8) q);
		i += 1;
		if (i == len)
		{
			uint64		vrem = ((uint64) r << e) + vfrac;

			return std_flt2dec_strategy_grisu_format_exact_opt_possibly_round(buf, buf_len, len, exp, limit, vrem,
																			  (uint64) ten_kappa << e, err << e,
																			  result, result_1);
		}
		if (i > (size_t) max_kappa)
			break;
		ten_kappa /= 10;
		remainder = r;
	}
	remainder_2 = vfrac;
	maxerr = UINT64CONST(1) << (e - 1);
	while (err < maxerr)
	{
		uint64		q;
		uint64		r;

		remainder_2 *= 10;
		err *= 10;
		q = remainder_2 >> e;
		r = remainder_2 & ((UINT64CONST(1) << e) - 1);
		std_index_check(i, buf_len, GRISU_FILE, 622, 9);
		buf[i] = (uint8) ('0' + (uint8) q);
		i += 1;
		if (i == len)
			return std_flt2dec_strategy_grisu_format_exact_opt_possibly_round(buf, buf_len, len, exp, limit, r,
																			  UINT64CONST(1) << e, err, result,
																			  result_1);
		remainder_2 = r;
	}
	return false;
}

/* std_flt2dec_strategy_grisu_format_exact is grisu::format_exact: Grisu's exact digits, or Dragon's when Grisu can't prove its own. */
size_t
std_flt2dec_strategy_grisu_format_exact(const StdDecoded *d, uint8 *buf, size_t buf_len, int16 limit, int16 *result_1)
{
	size_t		len;

	if (std_flt2dec_strategy_grisu_format_exact_opt(d, buf, buf_len, limit, &len, result_1))
		return len;
	return std_flt2dec_strategy_dragon_format_exact(d, buf, buf_len, limit, result_1);
}

/* std_flt2dec_strategy_grisu_format_shortest_FnMut_call_mut is grisu::format_shortest called as the FnMut to_shortest_str takes, its shim. */
size_t
std_flt2dec_strategy_grisu_format_shortest_FnMut_call_mut(void *captures, const StdDecoded *d, uint8 *buf,
														  size_t buf_len, int16 *result_1)
{
	return std_flt2dec_strategy_grisu_format_shortest(d, buf, buf_len, result_1);
}

/* std_flt2dec_strategy_grisu_format_exact_FnMut_call_mut is grisu::format_exact called as the FnMut to_exact_fixed_str takes, its shim. */
size_t
std_flt2dec_strategy_grisu_format_exact_FnMut_call_mut(void *captures, const StdDecoded *d, uint8 *buf, size_t buf_len,
													   int16 limit, int16 *result_1)
{
	return std_flt2dec_strategy_grisu_format_exact(d, buf, buf_len, limit, result_1);
}

/* STD_FLT2DEC_STRATEGY_DRAGON_POW10 is Dragon's POW10: the powers of ten a digit holds. */
static const uint32 STD_FLT2DEC_STRATEGY_DRAGON_POW10[10] = {
	1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000,
};

/* STD_FLT2DEC_STRATEGY_DRAGON_POW5TO16 is 5^16's digits. */
static const uint32 STD_FLT2DEC_STRATEGY_DRAGON_POW5TO16[2] = {0x86f26fc1, 0x23};

/* STD_FLT2DEC_STRATEGY_DRAGON_POW5TO32 is 5^32's digits. */
static const uint32 STD_FLT2DEC_STRATEGY_DRAGON_POW5TO32[3] = {0x85acef81, 0x2d6d415b, 0x4ee};

/* STD_FLT2DEC_STRATEGY_DRAGON_POW5TO64 is 5^64's digits. */
static const uint32 STD_FLT2DEC_STRATEGY_DRAGON_POW5TO64[5] = {0xbf6a1f01, 0x6e38ed64, 0xdaa797ed, 0xe93ff9f4,
0x184f03};

/* STD_FLT2DEC_STRATEGY_DRAGON_POW5TO128 is 5^128's digits. */
static const uint32 STD_FLT2DEC_STRATEGY_DRAGON_POW5TO128[10] = {
	0x2e953e01, 0x3df9909, 0xf1538fd, 0x2374e42f, 0xd3cff5ec, 0xc404dc08, 0xbccdb0da, 0xa6337f19,
	0xe91f2603, 0x24e,
};

/* STD_FLT2DEC_STRATEGY_DRAGON_POW5TO256 is 5^256's digits. */
static const uint32 STD_FLT2DEC_STRATEGY_DRAGON_POW5TO256[19] = {
	0x982e7c01, 0xbed3875b, 0xd8d99f72, 0x12152f87, 0x6bde50c6, 0xcf4a6e70, 0xd595d80f, 0x26b2716e,
	0xadc666b0, 0x1d153624, 0x3c42d35a, 0x63ff540e, 0xcc5573c0, 0x65f9ef17, 0x55bc28f2, 0x80dcc7f7,
	0xf46eeddc, 0x5fdcefce, 0x553f7,
};

/* std_flt2dec_strategy_dragon_mul_pow10 is dragon::mul_pow10: x times 10^n. */
StdBignumBig32x40 *
std_flt2dec_strategy_dragon_mul_pow10(StdBignumBig32x40 *x, size_t n)
{
	if (n < 8)
	{
		std_index_check(n & 7, lengthof(STD_FLT2DEC_STRATEGY_DRAGON_POW10), DRAGON_FILE, 36, 28);
		return std_bignum_Big32x40_mul_small(x, STD_FLT2DEC_STRATEGY_DRAGON_POW10[n & 7]);
	}
	if ((n & 7) != 0)
	{
		std_index_check(n & 7, lengthof(STD_FLT2DEC_STRATEGY_DRAGON_POW10), DRAGON_FILE, 41, 21);
		std_bignum_Big32x40_mul_small(x, STD_FLT2DEC_STRATEGY_DRAGON_POW10[n & 7] >> (n & 7));
	}
	if ((n & 8) != 0)
		std_bignum_Big32x40_mul_small(x, STD_FLT2DEC_STRATEGY_DRAGON_POW10[8] >> 8);
	if ((n & 16) != 0)
		std_bignum_Big32x40_mul_digits(x, STD_FLT2DEC_STRATEGY_DRAGON_POW5TO16,
									   lengthof(STD_FLT2DEC_STRATEGY_DRAGON_POW5TO16));
	if ((n & 32) != 0)
		std_bignum_Big32x40_mul_digits(x, STD_FLT2DEC_STRATEGY_DRAGON_POW5TO32,
									   lengthof(STD_FLT2DEC_STRATEGY_DRAGON_POW5TO32));
	if ((n & 64) != 0)
		std_bignum_Big32x40_mul_digits(x, STD_FLT2DEC_STRATEGY_DRAGON_POW5TO64,
									   lengthof(STD_FLT2DEC_STRATEGY_DRAGON_POW5TO64));
	if ((n & 128) != 0)
		std_bignum_Big32x40_mul_digits(x, STD_FLT2DEC_STRATEGY_DRAGON_POW5TO128,
									   lengthof(STD_FLT2DEC_STRATEGY_DRAGON_POW5TO128));
	if ((n & 256) != 0)
		std_bignum_Big32x40_mul_digits(x, STD_FLT2DEC_STRATEGY_DRAGON_POW5TO256,
									   lengthof(STD_FLT2DEC_STRATEGY_DRAGON_POW5TO256));
	return std_bignum_Big32x40_mul_pow2(x, n);
}

/* std_flt2dec_strategy_dragon_div_2pow10 is dragon::div_2pow10: x divided by 2 * 10^n. */
static StdBignumBig32x40 *
std_flt2dec_strategy_dragon_div_2pow10(StdBignumBig32x40 *x, size_t n)
{
	size_t		largest = lengthof(STD_FLT2DEC_STRATEGY_DRAGON_POW10) - 1;
	uint32		tmp_1;

	while (n > largest)
	{
		std_bignum_Big32x40_div_rem_small(x, STD_FLT2DEC_STRATEGY_DRAGON_POW10[largest], &tmp_1);
		n -= largest;
	}
	std_index_check(n, lengthof(STD_FLT2DEC_STRATEGY_DRAGON_POW10), DRAGON_FILE, 70, 21);
	std_bignum_Big32x40_div_rem_small(x, STD_FLT2DEC_STRATEGY_DRAGON_POW10[n] << 1, &tmp_1);
	return x;
}

/* std_flt2dec_strategy_dragon_div_rem_upto_16 is dragon::div_rem_upto_16: the digit of x over scale below 16, x left as the remainder. */
static uint8
std_flt2dec_strategy_dragon_div_rem_upto_16(StdBignumBig32x40 *x, const StdBignumBig32x40 *scale,
											const StdBignumBig32x40 *scale2, const StdBignumBig32x40 *scale4,
											const StdBignumBig32x40 *scale8)
{
	uint8		d = 0;

	if (std_bignum_Big32x40_Ord_cmp(x, scale8) != STD_ORDERING_LESS)
	{
		std_bignum_Big32x40_sub(x, scale8);
		d += 8;
	}
	if (std_bignum_Big32x40_Ord_cmp(x, scale4) != STD_ORDERING_LESS)
	{
		std_bignum_Big32x40_sub(x, scale4);
		d += 4;
	}
	if (std_bignum_Big32x40_Ord_cmp(x, scale2) != STD_ORDERING_LESS)
	{
		std_bignum_Big32x40_sub(x, scale2);
		d += 2;
	}
	if (std_bignum_Big32x40_Ord_cmp(x, scale) != STD_ORDERING_LESS)
	{
		std_bignum_Big32x40_sub(x, scale);
		d += 1;
	}
	return d;
}

/* std_flt2dec_strategy_dragon_format_shortest is dragon::format_shortest: the shortest digits that read back as the value, and the exponent. */
size_t
std_flt2dec_strategy_dragon_format_shortest(const StdDecoded *d, uint8 *buf, size_t buf_len, int16 *result_1)
{
	StdOrdering rounding;
	int16		k;
	StdBignumBig32x40 mant;
	StdBignumBig32x40 minus;
	StdBignumBig32x40 plus;
	StdBignumBig32x40 scale;
	StdBignumBig32x40 tmp_1;
	StdBignumBig32x40 scale2;
	StdBignumBig32x40 scale4;
	StdBignumBig32x40 scale8;
	bool		down;
	bool		up;
	size_t		i = 0;

	if (!(d->mant > 0))
		STD_FLT2DEC_PANIC(DRAGON_FILE, 120, 5, "assertion failed: d.mant > 0");
	if (!(d->minus > 0))
		STD_FLT2DEC_PANIC(DRAGON_FILE, 121, 5, "assertion failed: d.minus > 0");
	if (!(d->plus > 0))
		STD_FLT2DEC_PANIC(DRAGON_FILE, 122, 5, "assertion failed: d.plus > 0");
	if (!(d->mant + d->plus >= d->mant))
		STD_FLT2DEC_PANIC(DRAGON_FILE, 123, 5, "assertion failed: d.mant.checked_add(d.plus).is_some()");
	if (!(d->minus <= d->mant))
		STD_FLT2DEC_PANIC(DRAGON_FILE, 124, 5, "assertion failed: d.mant.checked_sub(d.minus).is_some()");
	if (!(buf_len >= STD_FLT2DEC_MAX_SIG_DIGITS))
		STD_FLT2DEC_PANIC(DRAGON_FILE, 125, 5, "assertion failed: buf.len() >= MAX_SIG_DIGITS");
	rounding = d->inclusive ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL;
	k = std_flt2dec_estimator_estimate_scaling_factor(d->mant + d->plus, d->exp);
	mant = std_bignum_Big32x40_from_u64(d->mant);
	minus = std_bignum_Big32x40_from_u64(d->minus);
	plus = std_bignum_Big32x40_from_u64(d->plus);
	scale = std_bignum_Big32x40_from_small(1);
	if (d->exp < 0)
		std_bignum_Big32x40_mul_pow2(&scale, (size_t) (int16) -d->exp);
	else
	{
		std_bignum_Big32x40_mul_pow2(&mant, (size_t) d->exp);
		std_bignum_Big32x40_mul_pow2(&minus, (size_t) d->exp);
		std_bignum_Big32x40_mul_pow2(&plus, (size_t) d->exp);
	}
	if (k >= 0)
		std_flt2dec_strategy_dragon_mul_pow10(&scale, (size_t) k);
	else
	{
		std_flt2dec_strategy_dragon_mul_pow10(&mant, (size_t) (int16) -k);
		std_flt2dec_strategy_dragon_mul_pow10(&minus, (size_t) (int16) -k);
		std_flt2dec_strategy_dragon_mul_pow10(&plus, (size_t) (int16) -k);
	}
	tmp_1 = mant;
	if (std_bignum_Big32x40_Ord_cmp(&scale, std_bignum_Big32x40_add(&tmp_1, &plus)) < rounding)
		k += 1;
	else
	{
		std_bignum_Big32x40_mul_small(&mant, 10);
		std_bignum_Big32x40_mul_small(&minus, 10);
		std_bignum_Big32x40_mul_small(&plus, 10);
	}
	scale2 = scale;
	std_bignum_Big32x40_mul_pow2(&scale2, 1);
	scale4 = scale;
	std_bignum_Big32x40_mul_pow2(&scale4, 2);
	scale8 = scale;
	std_bignum_Big32x40_mul_pow2(&scale8, 3);
	for (;;)
	{
		uint8		d_2 = std_flt2dec_strategy_dragon_div_rem_upto_16(&mant, &scale, &scale2, &scale4, &scale8);

		std_index_check(i, buf_len, DRAGON_FILE, 196, 9);
		buf[i] = (uint8) ('0' + d_2);
		i += 1;
		down = std_bignum_Big32x40_Ord_cmp(&mant, &minus) < rounding;
		tmp_1 = mant;
		up = std_bignum_Big32x40_Ord_cmp(&scale, std_bignum_Big32x40_add(&tmp_1, &plus)) < rounding;
		if (down || up)
			break;
		std_bignum_Big32x40_mul_small(&mant, 10);
		std_bignum_Big32x40_mul_small(&minus, 10);
		std_bignum_Big32x40_mul_small(&plus, 10);
	}
	if (up && (!down || std_bignum_Big32x40_Ord_cmp(std_bignum_Big32x40_mul_pow2(&mant, 1), &scale) !=
			   STD_ORDERING_LESS))
	{
		uint8		c;

		std_range_to_check(i, buf_len, DRAGON_FILE, 252, 47);
		if (std_flt2dec_round_up(buf, i, &c))
		{
			std_index_check(i, buf_len, DRAGON_FILE, 253, 13);
			buf[i] = c;
			i += 1;
			k += 1;
		}
	}
	std_range_to_check(i, buf_len, DRAGON_FILE, 260, 18);
	*result_1 = k;
	return i;
}

/* std_flt2dec_strategy_dragon_format_exact is dragon::format_exact: the exact digits filling buf, or up to the limit's place, and the exponent. */
size_t
std_flt2dec_strategy_dragon_format_exact(const StdDecoded *d, uint8 *buf, size_t buf_len, int16 limit,
										 int16 *result_1)
{
	int16		k;
	StdBignumBig32x40 mant;
	StdBignumBig32x40 scale;
	StdBignumBig32x40 tmp_1;
	size_t		len;
	StdOrdering order;

	if (!(d->mant > 0))
		STD_FLT2DEC_PANIC(DRAGON_FILE, 269, 5, "assertion failed: d.mant > 0");
	if (!(d->minus > 0))
		STD_FLT2DEC_PANIC(DRAGON_FILE, 270, 5, "assertion failed: d.minus > 0");
	if (!(d->plus > 0))
		STD_FLT2DEC_PANIC(DRAGON_FILE, 271, 5, "assertion failed: d.plus > 0");
	if (!(d->mant + d->plus >= d->mant))
		STD_FLT2DEC_PANIC(DRAGON_FILE, 272, 5, "assertion failed: d.mant.checked_add(d.plus).is_some()");
	if (!(d->minus <= d->mant))
		STD_FLT2DEC_PANIC(DRAGON_FILE, 273, 5, "assertion failed: d.mant.checked_sub(d.minus).is_some()");
	k = std_flt2dec_estimator_estimate_scaling_factor(d->mant, d->exp);
	mant = std_bignum_Big32x40_from_u64(d->mant);
	scale = std_bignum_Big32x40_from_small(1);
	if (d->exp < 0)
		std_bignum_Big32x40_mul_pow2(&scale, (size_t) (int16) -d->exp);
	else
		std_bignum_Big32x40_mul_pow2(&mant, (size_t) d->exp);
	if (k >= 0)
		std_flt2dec_strategy_dragon_mul_pow10(&scale, (size_t) k);
	else
		std_flt2dec_strategy_dragon_mul_pow10(&mant, (size_t) (int16) -k);
	tmp_1 = scale;
	if (std_bignum_Big32x40_Ord_cmp(std_bignum_Big32x40_add(std_flt2dec_strategy_dragon_div_2pow10(&tmp_1, buf_len),
															&mant),
									&scale) != STD_ORDERING_LESS)
		k += 1;
	else
		std_bignum_Big32x40_mul_small(&mant, 10);
	if (k < limit)
		len = 0;
	else if ((size_t) ((int32) k - (int32) limit) < buf_len)
		len = (size_t) (int16) (k - limit);
	else
		len = buf_len;
	if (len > 0)
	{
		StdBignumBig32x40 scale2 = scale;
		StdBignumBig32x40 scale4;
		StdBignumBig32x40 scale8;

		std_bignum_Big32x40_mul_pow2(&scale2, 1);
		scale4 = scale;
		std_bignum_Big32x40_mul_pow2(&scale4, 2);
		scale8 = scale;
		std_bignum_Big32x40_mul_pow2(&scale8, 3);
		for (size_t i = 0; i < len; i++)
		{
			uint8		d_2 = 0;

			if (std_bignum_Big32x40_is_zero(&mant))
			{
				std_range_check(i, len, buf_len, DRAGON_FILE, 334, 34);
				for (size_t j = i; j < len; j++)
					buf[j] = '0';
				std_range_to_check(len, buf_len, DRAGON_FILE, 338, 37);
				*result_1 = k;
				return len;
			}
			if (std_bignum_Big32x40_Ord_cmp(&mant, &scale8) != STD_ORDERING_LESS)
			{
				std_bignum_Big32x40_sub(&mant, &scale8);
				d_2 += 8;
			}
			if (std_bignum_Big32x40_Ord_cmp(&mant, &scale4) != STD_ORDERING_LESS)
			{
				std_bignum_Big32x40_sub(&mant, &scale4);
				d_2 += 4;
			}
			if (std_bignum_Big32x40_Ord_cmp(&mant, &scale2) != STD_ORDERING_LESS)
			{
				std_bignum_Big32x40_sub(&mant, &scale2);
				d_2 += 2;
			}
			if (std_bignum_Big32x40_Ord_cmp(&mant, &scale) != STD_ORDERING_LESS)
			{
				std_bignum_Big32x40_sub(&mant, &scale);
				d_2 += 1;
			}
			std_index_check(i, buf_len, DRAGON_FILE, 360, 13);
			buf[i] = (uint8) ('0' + d_2);
			std_bignum_Big32x40_mul_small(&mant, 10);
		}
	}
	order = std_bignum_Big32x40_Ord_cmp(&mant, std_bignum_Big32x40_mul_small(&scale, 5));
	if (order == STD_ORDERING_GREATER || (order == STD_ORDERING_EQUAL && len > 0 &&
										  (std_index_check(len - 1, buf_len, DRAGON_FILE, 372, 36),
										   (buf[len - 1] & 1) == 1)))
	{
		uint8		c;

		std_range_to_check(len, buf_len, DRAGON_FILE, 377, 47);
		if (std_flt2dec_round_up(buf, len, &c))
		{
			k += 1;
			if (k > limit && len < buf_len)
			{
				std_index_check(len, buf_len, DRAGON_FILE, 383, 17);
				buf[len] = c;
				len += 1;
			}
		}
	}
	std_range_to_check(len, buf_len, DRAGON_FILE, 390, 18);
	*result_1 = k;
	return len;
}

/* core's flt2dec generics at f32, which f32's Debug formats with. */
#define STD_FLT2DEC_SCOPE extern
#define STD_FLT2DEC_DEFINE
#define STD_FLT2DEC_TS F32
#define STD_FLT2DEC_T float4
#define STD_FLT2DEC_TO_BITS(v) ((uint64) std_f32_to_bits(v))
#define STD_FLT2DEC_BITS 32
#define STD_FLT2DEC_SIG_BITS 23
#define STD_FLT2DEC_EXP_BIAS 127
#include "doltgres/std/flt2dec_template.h"

/* core's flt2dec generics at f64, which f64's Debug formats with. */
#define STD_FLT2DEC_SCOPE extern
#define STD_FLT2DEC_DEFINE
#define STD_FLT2DEC_TS F64
#define STD_FLT2DEC_T float8
#define STD_FLT2DEC_TO_BITS(v) std_f64_to_bits(v)
#define STD_FLT2DEC_BITS 64
#define STD_FLT2DEC_SIG_BITS 52
#define STD_FLT2DEC_EXP_BIAS 1023
#include "doltgres/std/flt2dec_template.h"
