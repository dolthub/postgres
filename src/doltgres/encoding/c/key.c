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
 * Index keys whose bytes compare as their values do in Postgres' btree order.
 * A key is its columns one after another, each a marker byte (NULL first, a
 * value, or NULL last) and then the value's encoding, inverted for a
 * descending column. Every value encoding is self-delimiting, so a shorter
 * value never compares as a prefix of a longer one by accident, and the
 * column after it starts at a known place. Verus proves the Rust's encodings
 * order as their values do; the C's guarantee is tested equivalence.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/encoding/key.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"

#define RUST_FILE "encoding" STD_SEP "src" STD_SEP "key.rs"

/* VSTD_PERVASIVE_FILE is vstd's pervasive.rs as the remapped build names it, where Vec's set indexes. */
#define VSTD_PERVASIVE_FILE "cargo-registry/index.crates.io-1949cf8c6b5b557f" STD_SEP "vstd-0.0.0-2026-11-01-0251" STD_SEP "pervasive.rs"

/* ENCODING_KEY_JSONB_NULL is the token of a jsonb null. */
#define ENCODING_KEY_JSONB_NULL 0x00

/* ENCODING_KEY_JSONB_STRING starts the token of a jsonb string or object key. */
#define ENCODING_KEY_JSONB_STRING 0x01

/* ENCODING_KEY_JSONB_NUMERIC starts the token of a jsonb number. */
#define ENCODING_KEY_JSONB_NUMERIC 0x02

/* ENCODING_KEY_JSONB_BOOL starts the token of a jsonb boolean. */
#define ENCODING_KEY_JSONB_BOOL 0x03

/* ENCODING_KEY_JSONB_ARRAY starts the token of a jsonb array. */
#define ENCODING_KEY_JSONB_ARRAY 0x10

/* ENCODING_KEY_JSONB_OBJECT starts the token of a jsonb object. */
#define ENCODING_KEY_JSONB_OBJECT 0x11

static void encoding_key_invert_from(StdVec *out, size_t start);

/* encoding_key_Order_Debug_fmt is Order's derived Debug. */
bool
encoding_key_Order_Debug_fmt(const void *value, StdFormatter *f)
{
	const EncodingKeyOrder *self = (const EncodingKeyOrder *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Order", 5);

	std_DebugStruct_field(&b, "descending", 10, &self->descending, std_bool_Debug_fmt);
	std_DebugStruct_field(&b, "nulls_first", 11, &self->nulls_first, std_bool_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* encoding_key_Order_PartialEq_eq is Order's derived PartialEq. */
bool
encoding_key_Order_PartialEq_eq(const EncodingKeyOrder *self, const EncodingKeyOrder *other)
{
	return self->descending == other->descending && self->nulls_first == other->nulls_first;
}

/* encoding_key_null appends a NULL column. */
void
encoding_key_null(StdVec *out, EncodingKeyOrder order)
{
	std_vec_u8_push_Checked(out, order.nulls_first ? ENCODING_KEY_NULL_FIRST : ENCODING_KEY_NULL_LAST);
}

/* encoding_key_column appends a value column, whose ascending encoding the function writes. */
void
encoding_key_column(StdVec *out, EncodingKeyOrder order, void (*write) (void *, StdVec *), void *write_captures,
					void (*write_drop) (void *))
{
	size_t		start;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_u8_push_Checked(out, ENCODING_KEY_VALUE);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (write_drop != NULL)
			write_drop(write_captures);
		std_unwind_resume(&unwind);
	}
	start = out->len;
	write(write_captures, out);
	if (order.descending)
		encoding_key_invert_from(out, start);
}

/* encoding_key_bool appends a boolean, false first. */
void
encoding_key_bool(StdVec *out, bool value)
{
	std_vec_u8_push_Checked(out, (uint8) value);
}

/* encoding_key_byte appends a single unsigned byte, as the "char" type compares. */
void
encoding_key_byte(StdVec *out, uint8 value)
{
	std_vec_u8_push_Checked(out, value);
}

static void encoding_key_be_bytes_u16(uint16 x, uint8 *bytes);
static void encoding_key_be_bytes_u32(uint32 x, uint8 *bytes);
static void encoding_key_be_bytes_u64(uint64 x, uint8 *bytes);
static void encoding_key_be_bytes_u128(StdU128 x, uint8 *bytes);

/* encoding_key_i16 appends a signed 16-bit integer. */
void
encoding_key_i16(StdVec *out, int16 value)
{
	uint16		x = (uint16) ((uint16) value ^ 0x8000);
	uint8		bytes[2];

	encoding_key_be_bytes_u16(x, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, 2, 1, 1);
}

/* encoding_key_i32 appends a signed 32-bit integer, as int4, date, and other 32-bit types compare. */
void
encoding_key_i32(StdVec *out, int32 value)
{
	uint32		x = (uint32) value ^ 0x80000000;
	uint8		bytes[4];

	encoding_key_be_bytes_u32(x, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, 4, 1, 1);
}

/* encoding_key_i64 appends a signed 64-bit integer, as int8, money, time, and the timestamp types compare. */
void
encoding_key_i64(StdVec *out, int64 value)
{
	uint64		x = (uint64) value ^ ((uint64) 1 << 63);
	uint8		bytes[8];

	encoding_key_be_bytes_u64(x, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, 8, 1, 1);
}

/* encoding_key_u32 appends an unsigned 32-bit integer, as oid compares. */
void
encoding_key_u32(StdVec *out, uint32 value)
{
	uint8		bytes[4];

	encoding_key_be_bytes_u32(value, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, 4, 1, 1);
}

/* encoding_key_u64 appends an unsigned 64-bit integer, as pg_lsn and xid8 compare. */
void
encoding_key_u64(StdVec *out, uint64 value)
{
	uint8		bytes[8];

	encoding_key_be_bytes_u64(value, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, 8, 1, 1);
}

/* encoding_key_be_bytes_u16 returns a number's two bytes, most significant first, as u16::to_be_bytes does. */
static void
encoding_key_be_bytes_u16(uint16 x, uint8 *bytes)
{
	bytes[0] = (uint8) STD_SHR_FOLDED(u16, x, 8);
	bytes[1] = (uint8) x;
}

/* encoding_key_be_bytes_u32 returns a number's four bytes, most significant first, as u32::to_be_bytes does. */
static void
encoding_key_be_bytes_u32(uint32 x, uint8 *bytes)
{
	bytes[0] = (uint8) STD_SHR_FOLDED(u32, x, 24);
	bytes[1] = (uint8) STD_SHR_FOLDED(u32, x, 16);
	bytes[2] = (uint8) STD_SHR_FOLDED(u32, x, 8);
	bytes[3] = (uint8) x;
}

static void encoding_key_float4(StdVec *out, uint32 bits);
static void encoding_key_float8(StdVec *out, uint64 bits);

/*
 * encoding_key_f32 appends a float4 as Postgres' btree orders it: -0 equals
 * 0, and every NaN is one value above infinity.
 */
void
encoding_key_f32(StdVec *out, float4 value)
{
	encoding_key_float4(out, std_f32_to_bits(value));
}

/*
 * encoding_key_f64 appends a float8 as Postgres' btree orders it: -0 equals
 * 0, and every NaN is one value above infinity.
 */
void
encoding_key_f64(StdVec *out, float8 value)
{
	encoding_key_float8(out, std_f64_to_bits(value));
}

/* encoding_key_float4 appends a float4's key from its bits. */
static void
encoding_key_float4(StdVec *out, uint32 bits)
{
	uint32		magnitude = bits & 0x7fffffff;
	uint32		canonical;
	uint32		ordered;
	uint8		bytes[4];

	if (magnitude > 0x7f800000)
		canonical = 0x7fc00000;
	else if (magnitude == 0)
		canonical = 0;
	else
		canonical = bits;
	if (STD_SHR_FOLDED(u32, canonical, 31) == 1)
		ordered = ~canonical;
	else
		ordered = canonical ^ ((uint32) 1 << 31);
	encoding_key_be_bytes_u32(ordered, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, 4, 1, 1);
}

/* encoding_key_float8 appends a float8's key from its bits. */
static void
encoding_key_float8(StdVec *out, uint64 bits)
{
	uint64		magnitude = bits & UINT64CONST(0x7fffffffffffffff);
	uint64		canonical;
	uint64		ordered;
	uint8		bytes[8];

	if (magnitude > UINT64CONST(0x7ff0000000000000))
		canonical = UINT64CONST(0x7ff8000000000000);
	else if (magnitude == 0)
		canonical = 0;
	else
		canonical = bits;
	if (STD_SHR_FOLDED(u64, canonical, 63) == 1)
		ordered = ~canonical;
	else
		ordered = canonical ^ ((uint64) 1 << 63);
	encoding_key_be_bytes_u64(ordered, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, 8, 1, 1);
}

/* encoding_key_byte_len returns how many bytes a number takes, at least one. */
static size_t
encoding_key_byte_len(uint64 v)
{
	size_t		len;

	if (v < ((uint64) 1 << 8))
		len = 1;
	else if (v < ((uint64) 1 << 16))
		len = 2;
	else if (v < ((uint64) 1 << 24))
		len = 3;
	else if (v < ((uint64) 1 << 32))
		len = 4;
	else if (v < ((uint64) 1 << 40))
		len = 5;
	else if (v < ((uint64) 1 << 48))
		len = 6;
	else if (v < ((uint64) 1 << 56))
		len = 7;
	else
		len = 8;
	return len;
}

static void encoding_key_push_tail(StdVec *out, const uint8 *bytes, size_t len);

/*
 * encoding_key_sortable_int appends an integer in a variable-length form
 * whose bytes compare as the integers do: one byte for -64 to 63, and
 * otherwise a byte giving the sign and length before the magnitude's bytes.
 */
void
encoding_key_sortable_int(StdVec *out, int64 value)
{
	if (value >= -64 && value < 64)
		std_vec_u8_push_Checked(out, (uint8) STD_ADD(i64, 0x80, value, 957, 19));
	else if (value >= 64)
	{
		size_t		len = encoding_key_byte_len((uint64) value);
		uint8		bytes[8];

		std_vec_u8_push_Checked(out, STD_ADD(u8, 0xc0, (uint8) len, 960, 18));
		encoding_key_be_bytes_u64((uint64) value, bytes);
		encoding_key_push_tail(out, bytes, len);
	}
	else
	{
		uint64		magnitude = ~(uint64) value;
		size_t		len = encoding_key_byte_len(magnitude);
		uint8		bytes[8];

		std_vec_u8_push_Checked(out, STD_SUB(u8, 0x40, (uint8) len, 965, 18));
		encoding_key_be_bytes_u64(~magnitude, bytes);
		encoding_key_push_tail(out, bytes, len);
	}
}

/* encoding_key_push_tail appends the last bytes of eight. */
static void
encoding_key_push_tail(StdVec *out, const uint8 *bytes, size_t len)
{
	size_t		i = STD_SUB(usize, 8, len, 1020, 17);

	while (i < 8)
	{
		std_index_check(i, 8, RUST_FILE, 1029, 18);
		std_vec_u8_push_Checked(out, bytes[i]);
		i = STD_ADD(usize, i, 1, 1035, 9);
	}
}

/* encoding_key_Numeric_Debug_fmt is Numeric's derived Debug. */
bool
encoding_key_Numeric_Debug_fmt(const void *value, StdFormatter *f)
{
	const EncodingKeyNumeric *self = (const EncodingKeyNumeric *) value;
	StdDebugStruct b;

	switch (self->tag)
	{
		case ENCODING_KEY_NUMERIC_NEGATIVE_INFINITY:
			return std_fmt_write_str(f, "NegativeInfinity", 16);
		case ENCODING_KEY_NUMERIC_FINITE:
			b = std_fmt_debug_struct(f, "Finite", 6);
			std_DebugStruct_field(&b, "negative", 8, &self->u.finite.negative, std_bool_Debug_fmt);
			std_DebugStruct_field(&b, "digits", 6, &self->u.finite.digits, encoding_key_Digits_Debug_fmt);
			std_DebugStruct_field(&b, "exponent", 8, &self->u.finite.exponent, std_i32_Debug_fmt);
			return std_DebugStruct_finish(&b);
		case ENCODING_KEY_NUMERIC_INFINITY:
			return std_fmt_write_str(f, "Infinity", 8);
		case ENCODING_KEY_NUMERIC_NA_N:
			return std_fmt_write_str(f, "NaN", 3);
	}
	pg_unreachable();
}

/* encoding_key_Numeric_PartialEq_eq is Numeric's derived PartialEq. */
bool
encoding_key_Numeric_PartialEq_eq(const EncodingKeyNumeric *self, const EncodingKeyNumeric *other)
{
	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case ENCODING_KEY_NUMERIC_FINITE:
			return self->u.finite.negative == other->u.finite.negative &&
				encoding_key_Digits_PartialEq_eq(&self->u.finite.digits, &other->u.finite.digits) &&
				self->u.finite.exponent == other->u.finite.exponent;
		default:
			return true;
	}
}

/* encoding_key_Digits_Debug_fmt is Digits' derived Debug. */
bool
encoding_key_Digits_Debug_fmt(const void *value, StdFormatter *f)
{
	const EncodingKeyDigits *self = (const EncodingKeyDigits *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Digits", 6);

	std_DebugStruct_field(&b, "digits", 6, &self->digits, std_bytes_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* encoding_key_Digits_PartialEq_eq is Digits' derived PartialEq. */
bool
encoding_key_Digits_PartialEq_eq(const EncodingKeyDigits *self, const EncodingKeyDigits *other)
{
	return self->digits.len == other->digits.len &&
		memcmp(self->digits.ptr, other->digits.ptr, self->digits.len) == 0;
}

/*
 * encoding_key_Digits_new returns decimal digits, or nothing when a digit is
 * above 9 or there are more than i32::MAX of them.
 */
bool
encoding_key_Digits_new(const uint8 *digits, size_t digits_len, EncodingKeyDigits *result)
{
	size_t		i = 0;

	if (digits_len > (size_t) PG_INT32_MAX)
		return false;
	while (i < digits_len)
	{
		std_index_check(i, digits_len, RUST_FILE, 1094, 16);
		if (digits[i] > 9)
			return false;
		i = STD_ADD(usize, i, 1, 1097, 13);
	}
	result->digits.ptr = digits;
	result->digits.len = digits_len;
	return true;
}

static size_t encoding_key_first_nonzero(const uint8 *digits, size_t digits_len);
static size_t encoding_key_last_nonzero(const uint8 *digits, size_t digits_len, size_t first);
static void encoding_key_push_body(StdVec *out, const uint8 *digits, size_t digits_len, size_t first,
								   size_t last, int32 exponent);

/*
 * encoding_key_numeric appends a numeric value: a class byte, then for a
 * value other than zero its base-100 exponent and its base-100 digits with
 * trailing zeros removed (so 1.0 and 1.00 have one key, as Postgres finds
 * them equal), and a terminator. A negative value's bytes after the class
 * are inverted, so larger magnitudes sort first.
 */
void
encoding_key_numeric(StdVec *out, const EncodingKeyNumeric *value)
{
	bool		negative;
	StdBytes	digits;
	int32		exponent;
	size_t		first;
	size_t		last;
	size_t		start;

	switch (value->tag)
	{
		case ENCODING_KEY_NUMERIC_NEGATIVE_INFINITY:
			std_vec_u8_push_Checked(out, 0x01);
			return;
		case ENCODING_KEY_NUMERIC_INFINITY:
			std_vec_u8_push_Checked(out, 0x05);
			return;
		case ENCODING_KEY_NUMERIC_NA_N:
			std_vec_u8_push_Checked(out, 0x06);
			return;
		case ENCODING_KEY_NUMERIC_FINITE:
			negative = value->u.finite.negative;
			digits = value->u.finite.digits.digits;
			exponent = value->u.finite.exponent;
			break;
	}
	first = encoding_key_first_nonzero(digits.ptr, digits.len);
	if (first == digits.len)
	{
		std_vec_u8_push_Checked(out, 0x03);
		return;
	}
	last = encoding_key_last_nonzero(digits.ptr, digits.len, first);
	std_vec_u8_push_Checked(out, negative ? 0x02 : 0x04);
	start = out->len;
	encoding_key_push_body(out, digits.ptr, digits.len, first, last, exponent);
	if (negative)
		encoding_key_invert_from(out, start);
}

/*
 * encoding_key_first_nonzero is the place of a digit sequence's first nonzero
 * digit, or its length when it has none.
 */
static size_t
encoding_key_first_nonzero(const uint8 *digits, size_t digits_len)
{
	size_t		first = 0;

	for (;;)
	{
		if (!(first < digits_len))
			break;
		std_index_check(first, digits_len, RUST_FILE, 1303, 35);
		if (!(digits[first] == 0))
			break;
		first = STD_ADD(usize, first, 1, 1309, 9);
	}
	return first;
}

/*
 * encoding_key_last_nonzero is the place of a digit sequence's last nonzero
 * digit, given a nonzero digit at a place before it.
 */
static size_t
encoding_key_last_nonzero(const uint8 *digits, size_t digits_len, size_t first)
{
	size_t		last = STD_SUB(usize, digits_len, 1, 1326, 20);

	for (;;)
	{
		if (!(last > first))
			break;
		std_index_check(last, digits_len, RUST_FILE, 1327, 27);
		if (!(digits[last] == 0))
			break;
		last = STD_SUB(usize, last, 1, 1334, 9);
	}
	return last;
}

/*
 * encoding_key_base100_exponent is the base-100 exponent of digits
 * 0.d1d2... x 10^exponent whose first `first` digits are zero, and whether the
 * decimal exponent after those zeros is odd, which pads the digits with a
 * leading zero.
 */
static void
encoding_key_base100_exponent(int32 exponent, size_t first, int64 *out_0, bool *out_1)
{
	int64		shifted = STD_SUB(i64, (int64) exponent, (int64) first, 1359, 19);
	bool		odd = STD_REM_FOLDED(i64, shifted, 2) != 0;
	int64		even;

	if (odd)
		even = STD_ADD(i64, shifted, 1, 1362, 9);
	else
		even = shifted;
	*out_0 = STD_DIV_FOLDED(i64, even, 2);
	*out_1 = odd;
}

static void encoding_key_push_pairs(StdVec *out, const uint8 *digits, size_t digits_len, size_t from, size_t last);

/*
 * encoding_key_push_body appends numeric_body for digits with a nonzero
 * digit, whose first nonzero digit is at first and last at last.
 */
static void
encoding_key_push_body(StdVec *out, const uint8 *digits, size_t digits_len, size_t first, size_t last,
					   int32 exponent)
{
	int64		half;
	bool		odd;
	size_t		at;

	encoding_key_base100_exponent(exponent, first, &half, &odd);
	encoding_key_sortable_int(out, half);
	at = first;
	if (odd)
	{
		std_index_check(at, digits_len, RUST_FILE, 1386, 18);
		std_vec_u8_push_Checked(out, STD_ADD(u8, digits[at], 1, 1386, 18));
		at = STD_ADD(usize, at, 1, 1387, 9);
	}
	encoding_key_push_pairs(out, digits, digits_len, at, last);
	std_vec_u8_push_Checked(out, 0);
}

/* encoding_key_push_pairs appends the base-100 digit bytes of digits from one place through another. */
static void
encoding_key_push_pairs(StdVec *out, const uint8 *digits, size_t digits_len, size_t from, size_t last)
{
	size_t		at = from;

	while (at <= last)
	{
		uint8		second;
		uint8		tmp_1;

		if (at < last)
		{
			size_t		next = STD_ADD(usize, at, 1, 1439, 20);

			std_index_check(next, digits_len, RUST_FILE, 1439, 13);
			second = digits[next];
		}
		else
			second = 0;
		std_index_check(at, digits_len, RUST_FILE, 1443, 18);
		tmp_1 = STD_MUL(u8, digits[at], 10, 1443, 18);
		tmp_1 = STD_ADD(u8, tmp_1, second, 1443, 18);
		std_vec_u8_push_Checked(out, STD_ADD(u8, tmp_1, 1, 1443, 18));
		at = STD_ADD(usize, at, 2, 1451, 9);
	}
}

/*
 * encoding_key_invert_from inverts the bits of each byte from a place on,
 * none when the place is past the end.
 */
static void
encoding_key_invert_from(StdVec *out, size_t start)
{
	size_t		at = start;

	while (at < out->len)
	{
		uint8		inverted;

		std_index_check(at, out->len, RUST_FILE, 1483, 28);
		inverted = (uint8) ~STD_VEC_AT(out, uint8, at);
		std_index_check(at, out->len, VSTD_PERVASIVE_FILE, 398, 13);
		STD_VEC_AT(out, uint8, at) = inverted;
		at = STD_ADD(usize, at, 1, 1485, 9);
	}
}

static void encoding_key_append_text(StdVec *out, const uint8 *value, size_t value_len);

/*
 * encoding_key_text appends text compared byte by byte (the C collation),
 * which never holds a NUL byte.
 */
void
encoding_key_text(StdVec *out, const uint8 *value, size_t value_len)
{
#ifdef USE_ASSERT_CHECKING
	if (std_str_slice_contains_u8_Checked(0, value, value_len))
		STD_PANIC(1890, 5, "text holds no NUL byte");
#endif
	encoding_key_append_text(out, value, value_len);
}

/*
 * encoding_key_be_bytes_u64 returns a number's eight bytes, most significant
 * first, as u64::to_be_bytes does.
 */
static void
encoding_key_be_bytes_u64(uint64 x, uint8 *bytes)
{
	bytes[0] = (uint8) STD_SHR_FOLDED(u64, x, 56);
	bytes[1] = (uint8) STD_SHR_FOLDED(u64, x, 48);
	bytes[2] = (uint8) STD_SHR_FOLDED(u64, x, 40);
	bytes[3] = (uint8) STD_SHR_FOLDED(u64, x, 32);
	bytes[4] = (uint8) STD_SHR_FOLDED(u64, x, 24);
	bytes[5] = (uint8) STD_SHR_FOLDED(u64, x, 16);
	bytes[6] = (uint8) STD_SHR_FOLDED(u64, x, 8);
	bytes[7] = (uint8) x;
}

/* encoding_key_append_text appends text's key. */
static void
encoding_key_append_text(StdVec *out, const uint8 *value, size_t value_len)
{
	std_vec_extend_from_slice_Checked(out, value, value_len, 1, 1);
	std_vec_u8_push_Checked(out, 0);
}

/*
 * encoding_key_bytes appends bytes compared byte by byte with a shorter
 * prefix first, with each zero byte escaped, as bytea and a collation's sort
 * keys compare.
 */
void
encoding_key_bytes(StdVec *out, const uint8 *value, size_t value_len)
{
	size_t		i = 0;

	while (i < value_len)
	{
		uint8		b;

		std_index_check(i, value_len, RUST_FILE, 2371, 17);
		b = value[i];
		std_vec_u8_push_Checked(out, b);
		if (b == 0)
			std_vec_u8_push_Checked(out, 0xff);
		i = STD_ADD(usize, i, 1, 2380, 9);
	}
	std_vec_u8_push_Checked(out, 0);
	std_vec_u8_push_Checked(out, 1);
}

/*
 * encoding_key_bits appends a bit string of len bits held in whole bytes,
 * the last padded with zeros, as bit and varbit compare: the bytes, then the
 * length.
 */
void
encoding_key_bits(StdVec *out, const uint8 *value, size_t value_len, uint32 len)
{
	uint8		bytes[4];

	encoding_key_bytes(out, value, value_len);
	std_u32_to_be_bytes(len, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, 4, 1, 1);
}

/* encoding_key_raw appends a fixed-length value compared byte by byte, as uuid and macaddr are. */
void
encoding_key_raw(StdVec *out, const uint8 *value, size_t value_len)
{
	std_vec_extend_from_slice_Checked(out, value, value_len, 1, 1);
}

/*
 * encoding_key_inet appends an inet or cidr value as network_cmp orders
 * them: the family, then the network's bits up to the mask length (a shorter
 * network that is a prefix of a longer one first), then the whole address.
 */
void
encoding_key_inet(StdVec *out, bool ipv6, uint8 mask, const uint8 *address, size_t address_len)
{
	volatile StdVec network;
	volatile bool network_live = false;
	StdUnwind	unwind;

	std_vec_u8_push_Checked(out, ipv6 ? 3 : 2);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *network_v = (StdVec *) &network;
		size_t		count = 0;

		std_string_from_Checked(network_v, (const char *) address, address_len);
		network_live = true;
		for (size_t n = 0, n_len = network_v->len; n < n_len; n++)
		{
			size_t		i = count;
			size_t		tmp_1;
			size_t		keep;

			count = std_Enumerate_count_step(count);
			tmp_1 = STD_MUL(usize, i, 8, 2413, 51);
			keep = std_min_usize(std_saturating_sub_usize((size_t) mask, tmp_1), 8);
			STD_VEC_AT(network_v, uint8, n) &= (uint8) ~STD_SHR(u16, 0xff, (uint32) keep, 2414, 17);
		}
		std_vec_truncate(network_v, std_div_ceil_usize((size_t) mask, 8, RUST_FILE, 2416, 38), 1, NULL);
		encoding_key_bits(out, network_v->ptr, network_v->len, (uint32) mask);
		std_vec_extend_from_slice_Checked(out, address, address_len, 1, 1);
		network_live = false;
		std_vec_free_Checked(network_v, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (network_live)
			std_vec_free_Checked((StdVec *) &network, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/*
 * encoding_key_interval appends an interval as interval_cmp_value orders
 * them: its span in microseconds, counting a month as 30 days and a day as
 * 24 hours.
 */
void
encoding_key_interval(StdVec *out, int32 months, int32 days, int64 micros)
{
	StdI128		span;
	StdU128		x;
	StdU128		sign = {(uint64) 1 << 63, 0};
	uint8		bytes[16];

	span = std_mul_i128(std_i128_from_i64(months), std_i128_from_i64(30), RUST_FILE, 2440, 17);
	span = std_add_i128(span, std_i128_from_i64(days), RUST_FILE, 2440, 17);
	span = std_mul_i128(span, std_i128_from_i64(INT64CONST(86400000000)), RUST_FILE, 2440, 16);
	span = std_add_i128(span, std_i128_from_i64(micros), RUST_FILE, 2440, 16);
	x = std_u128_xor(std_i128_as_u128(span), sign);
	encoding_key_be_bytes_u128(x, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, 16, 1, 1);
}

/*
 * encoding_key_be_bytes_u128 returns a number's sixteen bytes, most
 * significant first, as u128::to_be_bytes does.
 */
static void
encoding_key_be_bytes_u128(StdU128 x, uint8 *bytes)
{
	bytes[0] = std_u128_as_u8(std_shr_folded_u128(x, 120));
	bytes[1] = std_u128_as_u8(std_shr_folded_u128(x, 112));
	bytes[2] = std_u128_as_u8(std_shr_folded_u128(x, 104));
	bytes[3] = std_u128_as_u8(std_shr_folded_u128(x, 96));
	bytes[4] = std_u128_as_u8(std_shr_folded_u128(x, 88));
	bytes[5] = std_u128_as_u8(std_shr_folded_u128(x, 80));
	bytes[6] = std_u128_as_u8(std_shr_folded_u128(x, 72));
	bytes[7] = std_u128_as_u8(std_shr_folded_u128(x, 64));
	bytes[8] = std_u128_as_u8(std_shr_folded_u128(x, 56));
	bytes[9] = std_u128_as_u8(std_shr_folded_u128(x, 48));
	bytes[10] = std_u128_as_u8(std_shr_folded_u128(x, 40));
	bytes[11] = std_u128_as_u8(std_shr_folded_u128(x, 32));
	bytes[12] = std_u128_as_u8(std_shr_folded_u128(x, 24));
	bytes[13] = std_u128_as_u8(std_shr_folded_u128(x, 16));
	bytes[14] = std_u128_as_u8(std_shr_folded_u128(x, 8));
	bytes[15] = std_u128_as_u8(x);
}

/* encoding_key_TimeWithZone_Debug_fmt is TimeWithZone's derived Debug. */
bool
encoding_key_TimeWithZone_Debug_fmt(const void *value, StdFormatter *f)
{
	const EncodingKeyTimeWithZone *self = (const EncodingKeyTimeWithZone *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "TimeWithZone", 12);

	std_DebugStruct_field(&b, "micros", 6, &self->micros, std_i64_Debug_fmt);
	std_DebugStruct_field(&b, "zone", 4, &self->zone, std_i32_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* encoding_key_TimeWithZone_PartialEq_eq is TimeWithZone's derived PartialEq. */
bool
encoding_key_TimeWithZone_PartialEq_eq(const EncodingKeyTimeWithZone *self, const EncodingKeyTimeWithZone *other)
{
	return self->micros == other->micros && self->zone == other->zone;
}

/*
 * encoding_key_TimeWithZone_new returns a time with time zone, or nothing
 * when the time or zone is outside Postgres' bounds.
 */
bool
encoding_key_TimeWithZone_new(int64 micros, int32 zone, EncodingKeyTimeWithZone *result)
{
	if (micros >= 0 && micros <= INT64CONST(86400000000) && zone >= -57599 && zone <= 57599)
	{
		result->micros = micros;
		result->zone = zone;
		return true;
	}
	return false;
}

/*
 * encoding_key_timetz appends a time with time zone as timetz_cmp orders
 * them: the time in UTC, then the zone (in seconds west of UTC).
 */
void
encoding_key_timetz(StdVec *out, EncodingKeyTimeWithZone value)
{
	int64		tmp_1 = STD_MUL(i64, (int64) value.zone, 1000000, 2566, 29);

	encoding_key_i64(out, STD_ADD(i64, value.micros, tmp_1, 2566, 14));
	encoding_key_i32(out, value.zone);
}

/*
 * encoding_key_array_tail appends what follows an array's elements: the end
 * marker, then the number of dimensions and each dimension's length, then
 * each dimension's lower bound, as array_cmp breaks ties.
 */
void
encoding_key_array_tail(StdVec *out, const encoding_key_array_tail_dimensions_t *dimensions, size_t dimensions_len)
{
	std_vec_u8_push_Checked(out, ENCODING_KEY_END);
	std_vec_u8_push_Checked(out, (uint8) dimensions_len);
	for (size_t i = 0, i_len = dimensions_len; i < i_len; i++)
		encoding_key_i32(out, dimensions[i].f0);
	for (size_t i = 0, i_len = dimensions_len; i < i_len; i++)
		encoding_key_i32(out, dimensions[i].f1);
}

/* encoding_key_range_empty appends an empty range, which sorts before every other range. */
void
encoding_key_range_empty(StdVec *out)
{
	std_vec_u8_push_Checked(out, 0x01);
}

/*
 * encoding_key_jsonb_null appends a jsonb null. A jsonb key is the tokens
 * Postgres' compareJsonbContainers walks, in jsonb's storage order (object
 * pairs by key length, then key bytes): each container's start with its
 * size, then its elements, or keys and values, in order. A top-level scalar
 * is a one-element "raw scalar" array, as jsonb stores it.
 */
void
encoding_key_jsonb_null(StdVec *out)
{
	std_vec_u8_push_Checked(out, ENCODING_KEY_JSONB_NULL);
}

/*
 * encoding_key_jsonb_string appends a jsonb string or object key, whose text
 * key (in the database's default collation) the function writes.
 */
void
encoding_key_jsonb_string(StdVec *out, void (*write) (void *, StdVec *), void *write_captures,
						  void (*write_drop) (void *))
{
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_u8_push_Checked(out, ENCODING_KEY_JSONB_STRING);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (write_drop != NULL)
			write_drop(write_captures);
		std_unwind_resume(&unwind);
	}
	write(write_captures, out);
}

/* encoding_key_jsonb_numeric appends a jsonb number. */
void
encoding_key_jsonb_numeric(StdVec *out, const EncodingKeyNumeric *value)
{
	std_vec_u8_push_Checked(out, ENCODING_KEY_JSONB_NUMERIC);
	encoding_key_numeric(out, value);
}

/* encoding_key_jsonb_bool appends a jsonb boolean. */
void
encoding_key_jsonb_bool(StdVec *out, bool value)
{
	std_vec_u8_push_Checked(out, ENCODING_KEY_JSONB_BOOL);
	std_vec_u8_push_Checked(out, (uint8) value);
}

/*
 * encoding_key_jsonb_array appends the start of a jsonb array of len
 * elements. raw_scalar is set for the array that holds a top-level scalar.
 * Arrays order by element count first, and a raw scalar before a one-element
 * array.
 */
void
encoding_key_jsonb_array(StdVec *out, uint32 len, bool raw_scalar)
{
	uint8		bytes[4];

	std_vec_u8_push_Checked(out, ENCODING_KEY_JSONB_ARRAY);
	std_u32_to_be_bytes(len, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, 4, 1, 1);
	std_vec_u8_push_Checked(out, (uint8) !raw_scalar);
}

/* encoding_key_jsonb_object appends the start of a jsonb object of len pairs. */
void
encoding_key_jsonb_object(StdVec *out, uint32 len)
{
	uint8		bytes[4];

	std_vec_u8_push_Checked(out, ENCODING_KEY_JSONB_OBJECT);
	std_u32_to_be_bytes(len, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, 4, 1, 1);
}

#ifdef DOLTGRES_TEST_ENCODING

/* encoding_key_tests_key returns the key a function writes into an empty vector. */
static void
encoding_key_tests_key(void (*f) (void *, StdVec *), void *f_captures, void (*f_drop) (void *), StdVec *result)
{
	volatile StdVec out;
	volatile bool out_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_new((StdVec *) &out, 1, 1);
		out_live = true;
		f(f_captures, (StdVec *) &out);
		*result = *(StdVec *) &out;
		out_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (out_live)
			std_vec_free_Checked((StdVec *) &out, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/* encoding_key_tests_VecU8_Debug_fmt is a Vec<u8>'s Debug: its bytes as a list. */
static bool
encoding_key_tests_VecU8_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = (const StdVec *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
		std_DebugList_entry(&b, &STD_VEC_AT(v, uint8, i), std_u8_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* encoding_key_tests_VecU8_PartialOrd_gt is Vec<u8>'s PartialOrd::gt: the bytes' order, a prefix first. */
static bool
encoding_key_tests_VecU8_PartialOrd_gt(const StdVec *a, const StdVec *b)
{
	return std_bytes_Ord_cmp(a->ptr, a->len, b->ptr, b->len) == STD_ORDERING_GREATER;
}

/* encoding_key_tests_VecU8_PartialOrd_lt is Vec<u8>'s PartialOrd::lt: the bytes' order, a prefix first. */
static bool
encoding_key_tests_VecU8_PartialOrd_lt(const StdVec *a, const StdVec *b)
{
	return std_bytes_Ord_cmp(a->ptr, a->len, b->ptr, b->len) == STD_ORDERING_LESS;
}

/* encoding_key_tests_VecU8_drop is a Vec<u8>'s drop glue, its buffer's free. */
static void
encoding_key_tests_VecU8_drop(void *self)
{
	std_vec_free_Checked((StdVec *) self, 1, 1);
}

/* encoding_key_tests_VecVecU8_Debug_fmt is a Vec<Vec<u8>>'s Debug: its vectors as a list. */
static bool
encoding_key_tests_VecVecU8_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = (const StdVec *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
		std_DebugList_entry(&b, &STD_VEC_AT(v, StdVec, i), encoding_key_tests_VecU8_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* test_encoding_key_tests_sortable_ints_order_as_integers_closure_0_0 is a value's sortable integer key. */
static void
test_encoding_key_tests_sortable_ints_order_as_integers_closure_0_0(void *captures, StdVec *o)
{
	encoding_key_sortable_int(o, *(const int64 *) captures);
}

/*
 * test_encoding_key_tests_sortable_ints_order_as_integers: sortable integer
 * keys order as the integers do, across each change of length.
 */
void
test_encoding_key_tests_sortable_ints_order_as_integers(void)
{
	static const int64 values[] = {PG_INT64_MIN, -((int64) 1 << 40), -65536, -257, -256, -65, -64, -1, 0, 1, 63, 64,
	255, 256, (int64) 1 << 40, PG_INT64_MAX};
	volatile StdVec keys;
	volatile bool keys_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *keys_v = (StdVec *) &keys;
		StdSizeHint hint = {lengthof(values), true, lengthof(values)};
		StdWindows	windows;
		const void *window;
		size_t		window_len;
		bool		all = true;

		std_vec_with_capacity_trusted(keys_v, hint, sizeof(StdVec), _Alignof(StdVec));
		keys_live = true;
		std_vec_extend_trusted_reserve(keys_v, hint, sizeof(StdVec), _Alignof(StdVec));
		for (size_t i = 0, i_len = lengthof(values); i < i_len; i++)
		{
			int64		v = values[i];

			encoding_key_tests_key(test_encoding_key_tests_sortable_ints_order_as_integers_closure_0_0, &v, NULL,
								   &STD_VEC_AT(keys_v, StdVec, keys_v->len));
			keys_v->len++;
		}
		windows = std_slice_windows(keys_v->ptr, keys_v->len, 2, RUST_FILE, 2999, 22);
		while (std_slice_Windows_next(&windows, sizeof(StdVec), &window, &window_len))
		{
			const StdVec *w = window;

			std_index_check(0, window_len, RUST_FILE, 2999, 41);
			std_index_check(1, window_len, RUST_FILE, 2999, 48);
			if (!encoding_key_tests_VecU8_PartialOrd_lt(&w[0], &w[1]))
			{
				all = false;
				break;
			}
		}
		if (!all)
		{
			StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{keys_v, encoding_key_tests_VecVecU8_Debug_fmt}};
			StdFmtArguments arguments = {pieces, 1, args};

			std_panic_fmt(RUST_FILE, 2999, 9, &arguments);
		}
		keys_live = false;
		std_vec_drop_Checked(keys_v, sizeof(StdVec), _Alignof(StdVec), encoding_key_tests_VecU8_drop);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (keys_live)
			std_vec_drop_Checked((StdVec *) &keys, sizeof(StdVec), _Alignof(StdVec), encoding_key_tests_VecU8_drop);
		std_unwind_resume(&unwind);
	}
}

/* test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0_0_0 is asc's text: the captured value's text key. */
static void
test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0_0_0(void *captures, StdVec *o)
{
	const StdBytes *v = captures;

	encoding_key_text(o, v->ptr, v->len);
}

/* test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0_0 is asc's key: the captured value as an ascending column. */
static void
test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0_0(void *captures, StdVec *o)
{
	encoding_key_column(o, ENCODING_KEY_ORDER_ASC,
						test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0_0_0,
						captures, NULL);
}

/* test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0 is asc: a value's key as an ascending text column. */
static void
test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0(void *captures, const uint8 *v,
																				  size_t v_len, StdVec *result)
{
	StdBytes	value = {v, v_len};

	encoding_key_tests_key(test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0_0,
						   &value, NULL, result);
}

/* test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1_0_0 is desc's text: the captured value's text key. */
static void
test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1_0_0(void *captures, StdVec *o)
{
	const StdBytes *v = captures;

	encoding_key_text(o, v->ptr, v->len);
}

/* test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1_0 is desc's key: the captured value as a descending column. */
static void
test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1_0(void *captures, StdVec *o)
{
	encoding_key_column(o, ENCODING_KEY_ORDER_DESC,
						test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1_0_0,
						captures, NULL);
}

/* test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1 is desc: a value's key as a descending text column. */
static void
test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1(void *captures, const uint8 *v,
																				  size_t v_len, StdVec *result)
{
	StdBytes	value = {v, v_len};

	encoding_key_tests_key(test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1_0,
						   &value, NULL, result);
}

/* test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_2 is a descending column's NULL. */
static void
test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_2(void *captures, StdVec *o)
{
	encoding_key_null(o, ENCODING_KEY_ORDER_DESC);
}

/* test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_3 is an ascending column's NULL. */
static void
test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_3(void *captures, StdVec *o)
{
	encoding_key_null(o, ENCODING_KEY_ORDER_ASC);
}

/*
 * test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed: a
 * descending column's values sort in reverse, and its NULLs stay first as
 * an ascending column's stay last.
 */
void
test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed(void)
{
	volatile StdVec tmp[4];
	volatile bool tmp_live[4] = {false, false, false, false};
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 ab[] = {'a', 'b'};
		static const uint8 abc[] = {'a', 'b', 'c'};
		static const uint8 b[] = {'b'};
		static const uint8 z[] = {'z'};
		StdVec	   *t = (StdVec *) tmp;
		bool		holds;

		test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0(NULL, ab, 2, &t[0]);
		tmp_live[0] = true;
		test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0(NULL, abc, 3, &t[1]);
		tmp_live[1] = true;
		holds = encoding_key_tests_VecU8_PartialOrd_lt(&t[0], &t[1]);
		if (holds)
		{
			test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0(NULL, abc, 3, &t[2]);
			tmp_live[2] = true;
			test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0(NULL, b, 1, &t[3]);
			tmp_live[3] = true;
			holds = encoding_key_tests_VecU8_PartialOrd_lt(&t[2], &t[3]);
		}
		for (int k = 3; k >= 0; k--)
		{
			if (tmp_live[k])
			{
				tmp_live[k] = false;
				std_vec_free_Checked(&t[k], 1, 1);
			}
		}
		STD_ASSERT(3006, 9, holds, "asc(b\"ab\") < asc(b\"abc\") && asc(b\"abc\") < asc(b\"b\")");
		test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1(NULL, ab, 2, &t[0]);
		tmp_live[0] = true;
		test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1(NULL, abc, 3, &t[1]);
		tmp_live[1] = true;
		holds = encoding_key_tests_VecU8_PartialOrd_gt(&t[0], &t[1]);
		if (holds)
		{
			test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1(NULL, abc, 3, &t[2]);
			tmp_live[2] = true;
			test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1(NULL, b, 1, &t[3]);
			tmp_live[3] = true;
			holds = encoding_key_tests_VecU8_PartialOrd_gt(&t[2], &t[3]);
		}
		for (int k = 3; k >= 0; k--)
		{
			if (tmp_live[k])
			{
				tmp_live[k] = false;
				std_vec_free_Checked(&t[k], 1, 1);
			}
		}
		STD_ASSERT(3007, 9, holds, "desc(b\"ab\") > desc(b\"abc\") && desc(b\"abc\") > desc(b\"b\")");
		encoding_key_tests_key(test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_2, NULL,
							   NULL, &t[0]);
		tmp_live[0] = true;
		test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_1(NULL, z, 1, &t[1]);
		tmp_live[1] = true;
		holds = encoding_key_tests_VecU8_PartialOrd_lt(&t[0], &t[1]);
		tmp_live[1] = false;
		std_vec_free_Checked(&t[1], 1, 1);
		tmp_live[0] = false;
		std_vec_free_Checked(&t[0], 1, 1);
		STD_ASSERT(3008, 9, holds, "key(|o| null(o, Order::DESC)) < desc(b\"z\")");
		encoding_key_tests_key(test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_3, NULL,
							   NULL, &t[0]);
		tmp_live[0] = true;
		test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed_closure_0(NULL, z, 1, &t[1]);
		tmp_live[1] = true;
		holds = encoding_key_tests_VecU8_PartialOrd_gt(&t[0], &t[1]);
		tmp_live[1] = false;
		std_vec_free_Checked(&t[1], 1, 1);
		tmp_live[0] = false;
		std_vec_free_Checked(&t[0], 1, 1);
		STD_ASSERT(3009, 9, holds, "key(|o| null(o, Order::ASC)) > asc(b\"z\")");
		std_unwind_pop(&unwind);
	}
	else
	{
		for (int k = 3; k >= 0; k--)
		{
			if (tmp_live[k])
				std_vec_free_Checked((StdVec *) &tmp[k], 1, 1);
		}
		std_unwind_resume(&unwind);
	}
}

/*
 * EncodingKeyTestsNumericsDropTrailingZerosClosure00 is n's key's captures:
 * the sign, digits, and exponent, by reference.
 */
typedef struct EncodingKeyTestsNumericsDropTrailingZerosClosure00
{
	const bool *negative;
	const StdBytes *digits;
	const int32 *exponent;
} EncodingKeyTestsNumericsDropTrailingZerosClosure00;

/*
 * test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0_0 is n's
 * key: the finite numeric of the captured sign, digits, and exponent.
 */
static void
test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0_0(void *captures, StdVec *o)
{
	EncodingKeyTestsNumericsDropTrailingZerosClosure00 *c = captures;
	EncodingKeyNumeric value;

	value.tag = ENCODING_KEY_NUMERIC_FINITE;
	value.u.finite.negative = *c->negative;
	if (!encoding_key_Digits_new(c->digits->ptr, c->digits->len, &value.u.finite.digits))
		STD_OPTION_UNWRAP_FAILED(3015, 89);
	value.u.finite.exponent = *c->exponent;
	encoding_key_numeric(o, &value);
}

/* test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0 is n: a finite numeric's key. */
static void
test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(void *captures, bool negative, const uint8 *digits,
															   size_t digits_len, int32 exponent, StdVec *result)
{
	StdBytes	digits_ = {digits, digits_len};
	EncodingKeyTestsNumericsDropTrailingZerosClosure00 key = {&negative, &digits_, &exponent};

	encoding_key_tests_key(test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0_0, &key, NULL, result);
}

/*
 * test_encoding_key_tests_numerics_drop_trailing_zeros: numerics equal but for
 * trailing zeros have one key, zero's key is one key whatever its sign and
 * exponent, and keys order as the numerics do.
 */
void
test_encoding_key_tests_numerics_drop_trailing_zeros(void)
{
	static const uint8 d10[] = {1, 0};
	static const uint8 d1000[] = {1, 0, 0, 0};
	static const uint8 d005[] = {0, 0, 5};
	static const uint8 d5[] = {5};
	static const uint8 d0[] = {0};
	static const uint8 d1[] = {1};
	static const uint8 d123[] = {1, 2, 3};
	static const uint8 d12[] = {1, 2};
	volatile StdVec tmp[2];
	volatile bool tmp_live[2] = {false, false};
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *t = (StdVec *) tmp;
		bool		holds;

		test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(NULL, false, d10, 2, 1, &t[0]);
		tmp_live[0] = true;
		test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(NULL, false, d1000, 4, 1, &t[1]);
		tmp_live[1] = true;
		STD_ASSERT_EQ(3017, 9, t[0].len == t[1].len && memcmp(t[0].ptr, t[1].ptr, t[0].len) == 0, &t[0],
					  encoding_key_tests_VecU8_Debug_fmt, &t[1], encoding_key_tests_VecU8_Debug_fmt);
		tmp_live[1] = false;
		std_vec_free_Checked(&t[1], 1, 1);
		tmp_live[0] = false;
		std_vec_free_Checked(&t[0], 1, 1);
		test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(NULL, false, d005, 3, 3, &t[0]);
		tmp_live[0] = true;
		test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(NULL, false, d5, 1, 1, &t[1]);
		tmp_live[1] = true;
		STD_ASSERT_EQ(3018, 9, t[0].len == t[1].len && memcmp(t[0].ptr, t[1].ptr, t[0].len) == 0, &t[0],
					  encoding_key_tests_VecU8_Debug_fmt, &t[1], encoding_key_tests_VecU8_Debug_fmt);
		tmp_live[1] = false;
		std_vec_free_Checked(&t[1], 1, 1);
		tmp_live[0] = false;
		std_vec_free_Checked(&t[0], 1, 1);
		test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(NULL, true, STD_DANGLING, 0, 0, &t[0]);
		tmp_live[0] = true;
		test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(NULL, false, d0, 1, 4, &t[1]);
		tmp_live[1] = true;
		STD_ASSERT_EQ(3019, 9, t[0].len == t[1].len && memcmp(t[0].ptr, t[1].ptr, t[0].len) == 0, &t[0],
					  encoding_key_tests_VecU8_Debug_fmt, &t[1], encoding_key_tests_VecU8_Debug_fmt);
		tmp_live[1] = false;
		std_vec_free_Checked(&t[1], 1, 1);
		tmp_live[0] = false;
		std_vec_free_Checked(&t[0], 1, 1);
		test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(NULL, false, d5, 1, 1, &t[0]);
		tmp_live[0] = true;
		test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(NULL, false, d1, 1, 2, &t[1]);
		tmp_live[1] = true;
		holds = encoding_key_tests_VecU8_PartialOrd_lt(&t[0], &t[1]);
		tmp_live[1] = false;
		std_vec_free_Checked(&t[1], 1, 1);
		tmp_live[0] = false;
		std_vec_free_Checked(&t[0], 1, 1);
		STD_ASSERT(3020, 9, holds, "n(false, &[5], 1) < n(false, &[1], 2)");
		test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(NULL, true, d1, 1, 2, &t[0]);
		tmp_live[0] = true;
		test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(NULL, true, d5, 1, 1, &t[1]);
		tmp_live[1] = true;
		holds = encoding_key_tests_VecU8_PartialOrd_lt(&t[0], &t[1]);
		tmp_live[1] = false;
		std_vec_free_Checked(&t[1], 1, 1);
		tmp_live[0] = false;
		std_vec_free_Checked(&t[0], 1, 1);
		STD_ASSERT(3021, 9, holds, "n(true, &[1], 2) < n(true, &[5], 1)");
		test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(NULL, true, d123, 3, 0, &t[0]);
		tmp_live[0] = true;
		test_encoding_key_tests_numerics_drop_trailing_zeros_closure_0(NULL, true, d12, 2, 0, &t[1]);
		tmp_live[1] = true;
		holds = encoding_key_tests_VecU8_PartialOrd_lt(&t[0], &t[1]);
		tmp_live[1] = false;
		std_vec_free_Checked(&t[1], 1, 1);
		tmp_live[0] = false;
		std_vec_free_Checked(&t[0], 1, 1);
		STD_ASSERT(3022, 9, holds, "n(true, &[1, 2, 3], 0) < n(true, &[1, 2], 0)");
		std_unwind_pop(&unwind);
	}
	else
	{
		for (int k = 1; k >= 0; k--)
		{
			if (tmp_live[k])
				std_vec_free_Checked((StdVec *) &tmp[k], 1, 1);
		}
		std_unwind_resume(&unwind);
	}
}

/*
 * test_encoding_key_tests_digits_and_times_with_zones_outside_their_bounds_are_refused:
 * a digit above 9, a time outside a day, and a zone of 16 hours or more
 * either way are refused.
 */
void
test_encoding_key_tests_digits_and_times_with_zones_outside_their_bounds_are_refused(void)
{
	static const uint8 ok[] = {0, 9, 3};
	static const uint8 ten[] = {1, 10};
	static const uint8 big[] = {247};
	EncodingKeyDigits digits;
	EncodingKeyTimeWithZone time;

	STD_ASSERT(3027, 9,
			   encoding_key_Digits_new(ok, sizeof(ok), &digits) && !encoding_key_Digits_new(ten, sizeof(ten), &digits) &&
			   !encoding_key_Digits_new(big, sizeof(big), &digits),
			   "Digits::new(&[0, 9, 3]).is_some() && Digits::new(&[1, 10]).is_none() && Digits::new(&[247]).is_none()");
	STD_ASSERT(3028, 9,
			   encoding_key_TimeWithZone_new(0, -57599, &time) &&
			   encoding_key_TimeWithZone_new(INT64CONST(86400000000), 57599, &time),
			   "TimeWithZone::new(0, -57_599).is_some() && TimeWithZone::new(86_400_000_000, 57_599).is_some()");
	STD_ASSERT(3029, 9,
			   !encoding_key_TimeWithZone_new(-1, 0, &time) &&
			   !encoding_key_TimeWithZone_new(INT64CONST(86400000001), 0, &time),
			   "TimeWithZone::new(-1, 0).is_none() && TimeWithZone::new(86_400_000_001, 0).is_none()");
	STD_ASSERT(3030, 9,
			   !encoding_key_TimeWithZone_new(0, 57600, &time) && !encoding_key_TimeWithZone_new(0, -57600, &time),
			   "TimeWithZone::new(0, 57_600).is_none() && TimeWithZone::new(0, -57_600).is_none()");
}

#endif							/* DOLTGRES_TEST_ENCODING */
