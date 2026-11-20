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
 * Index keys whose bytes compare as their values do in Postgres' btree order
 * (encoding's key.rs). A key is its columns one after another, each a marker
 * byte and then the value's encoding, inverted for a descending column.
 */
#ifndef DOLTGRES_ENCODING_KEY_H
#define DOLTGRES_ENCODING_KEY_H

#include "doltgres/std/fmt.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"

#define ENCODING_KEY_NULL_FIRST 0x01
#define ENCODING_KEY_VALUE 0x02
#define ENCODING_KEY_NULL_LAST 0x03
#define ENCODING_KEY_BELOW 0x00
#define ENCODING_KEY_PAST 0x04
#define ENCODING_KEY_OUT_OF_LINE 0x05
#define ENCODING_KEY_END 0x01
#define ENCODING_KEY_ELEMENT 0x02
#define ENCODING_KEY_NULL_ELEMENT 0x03

/* EncodingKeyOrder is a key column's order. */
typedef struct EncodingKeyOrder
{
	bool		descending;
	bool		nulls_first;
} EncodingKeyOrder;

/* ENCODING_KEY_ORDER_ASC is ascending with NULLs last, Postgres' default. */
#define ENCODING_KEY_ORDER_ASC ((EncodingKeyOrder) {false, false})

/* ENCODING_KEY_ORDER_DESC is descending with NULLs first, Postgres' default for DESC. */
#define ENCODING_KEY_ORDER_DESC ((EncodingKeyOrder) {true, true})

/* EncodingKeyDigits is a numeric's decimal digits, each below ten. */
typedef struct EncodingKeyDigits
{
	StdBytes	digits;
} EncodingKeyDigits;

/* EncodingKeyNumericTag is Numeric's variants. */
typedef enum EncodingKeyNumericTag
{
	ENCODING_KEY_NUMERIC_NEGATIVE_INFINITY,
	ENCODING_KEY_NUMERIC_FINITE,
	ENCODING_KEY_NUMERIC_INFINITY,
	ENCODING_KEY_NUMERIC_NA_N,
} EncodingKeyNumericTag;

/* EncodingKeyNumeric is a numeric value: its sign, its digits, and the exponent of its first digit. */
typedef struct EncodingKeyNumeric
{
	EncodingKeyNumericTag tag;
	union
	{
		struct
		{
			bool		negative;
			EncodingKeyDigits digits;
			int32		exponent;
		}			finite;
	}			u;
} EncodingKeyNumeric;

/* EncodingKeyTimeWithZone is a time with time zone: microseconds into the day and the zone's offset. */
typedef struct EncodingKeyTimeWithZone
{
	int64		micros;
	int32		zone;
} EncodingKeyTimeWithZone;

/* encoding_key_array_tail_dimensions_t is an array dimension's length and lower bound, (i32, i32). */
typedef struct encoding_key_array_tail_dimensions_t
{
	int32		f0;
	int32		f1;
} encoding_key_array_tail_dimensions_t;

extern void encoding_key_null(StdVec *out, EncodingKeyOrder order);
extern void encoding_key_column(StdVec *out, EncodingKeyOrder order,
								void (*write) (void *, StdVec *),
								void *write_captures, void (*write_drop) (void *));
extern void encoding_key_bool(StdVec *out, bool value);
extern void encoding_key_byte(StdVec *out, uint8 value);
extern void encoding_key_i16(StdVec *out, int16 value);
extern void encoding_key_i32(StdVec *out, int32 value);
extern void encoding_key_i64(StdVec *out, int64 value);
extern void encoding_key_u32(StdVec *out, uint32 value);
extern void encoding_key_u64(StdVec *out, uint64 value);
extern void encoding_key_f32(StdVec *out, float4 value);
extern void encoding_key_f64(StdVec *out, float8 value);
extern void encoding_key_sortable_int(StdVec *out, int64 value);
extern bool encoding_key_Digits_new(const uint8 *digits, size_t digits_len,
									EncodingKeyDigits *result);
extern void encoding_key_numeric(StdVec *out, const EncodingKeyNumeric *value);
extern void encoding_key_text(StdVec *out, const uint8 *value, size_t value_len);
extern void encoding_key_bytes(StdVec *out, const uint8 *value, size_t value_len);
extern void encoding_key_bits(StdVec *out, const uint8 *value, size_t value_len,
							  uint32 len);
extern void encoding_key_raw(StdVec *out, const uint8 *value, size_t value_len);
extern void encoding_key_inet(StdVec *out, bool ipv6, uint8 mask,
							  const uint8 *address, size_t address_len);
extern void encoding_key_interval(StdVec *out, int32 months, int32 days,
								  int64 micros);
extern bool encoding_key_TimeWithZone_new(int64 micros, int32 zone,
										  EncodingKeyTimeWithZone *result);
extern void encoding_key_timetz(StdVec *out, EncodingKeyTimeWithZone value);
extern void encoding_key_array_tail(StdVec *out,
									const encoding_key_array_tail_dimensions_t *dimensions,
									size_t dimensions_len);
extern void encoding_key_range_empty(StdVec *out);
extern void encoding_key_jsonb_null(StdVec *out);
extern void encoding_key_jsonb_string(StdVec *out,
									  void (*write) (void *, StdVec *),
									  void *write_captures, void (*write_drop) (void *));
extern void encoding_key_jsonb_numeric(StdVec *out, const EncodingKeyNumeric *value);
extern void encoding_key_jsonb_bool(StdVec *out, bool value);
extern void encoding_key_jsonb_array(StdVec *out, uint32 len, bool raw_scalar);
extern void encoding_key_jsonb_object(StdVec *out, uint32 len);
extern bool encoding_key_Order_PartialEq_eq(const EncodingKeyOrder *self,
											const EncodingKeyOrder *other);
extern bool encoding_key_Order_Debug_fmt(const void *self, StdFormatter *f);
extern bool encoding_key_Numeric_PartialEq_eq(const EncodingKeyNumeric *self,
											  const EncodingKeyNumeric *other);
extern bool encoding_key_Numeric_Debug_fmt(const void *self, StdFormatter *f);
extern bool encoding_key_Digits_PartialEq_eq(const EncodingKeyDigits *self,
											 const EncodingKeyDigits *other);
extern bool encoding_key_Digits_Debug_fmt(const void *self, StdFormatter *f);
extern bool encoding_key_TimeWithZone_PartialEq_eq(const EncodingKeyTimeWithZone *self,
												   const EncodingKeyTimeWithZone *other);
extern bool encoding_key_TimeWithZone_Debug_fmt(const void *self, StdFormatter *f);

#ifdef DOLTGRES_TEST_ENCODING
extern void test_encoding_key_tests_sortable_ints_order_as_integers(void);
extern void test_encoding_key_tests_descending_columns_invert_and_keep_nulls_placed(void);
extern void test_encoding_key_tests_digits_and_times_with_zones_outside_their_bounds_are_refused(void);
extern void test_encoding_key_tests_numerics_drop_trailing_zeros(void);
#endif

#endif							/* DOLTGRES_ENCODING_KEY_H */
