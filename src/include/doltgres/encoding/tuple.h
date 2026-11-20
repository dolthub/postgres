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
 * The row tuples that tables' leaves hold (encoding's tuple.rs): a varint of
 * the field count and flags, a bitmap of NULLs, one of external fields when
 * any is, each field's end as two bytes, the fields' data, and the address
 * of the rest of a row too wide for one tuple.
 */
#ifndef DOLTGRES_ENCODING_TUPLE_H
#define DOLTGRES_ENCODING_TUPLE_H

#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"

/* ENCODING_TUPLE_ADDRESS_LEN is the length of the address of a row's overflow. */
#define ENCODING_TUPLE_ADDRESS_LEN 32

/* EncodingTupleFieldTag is Field's variants. */
typedef enum EncodingTupleFieldTag
{
	ENCODING_TUPLE_FIELD_NULL,
	ENCODING_TUPLE_FIELD_VALUE,
	/* A field in the row's overflow, by its index there. */
	ENCODING_TUPLE_FIELD_OVERFLOW,

	/*
	 * A field whose value is a blob stored apart: its length, its address,
	 * and where the address is.
	 */
	ENCODING_TUPLE_FIELD_EXTERNAL,
} EncodingTupleFieldTag;

/* EncodingTupleField is a field of a tuple. */
typedef struct EncodingTupleField
{
	EncodingTupleFieldTag tag;
	union
	{
		StdBytes	value;
		size_t		overflow;
		struct
		{
			uint64		length;
			StdBytes	address;
			size_t		at;
		}			external;
	}			u;
} EncodingTupleField;

/* EncodingTupleTuple is a parsed tuple over its bytes. */
typedef struct EncodingTupleTuple
{
	const uint8 *bytes;
	size_t		bytes_len;
	size_t		total;
	size_t		inline_;
	size_t		nulls_at;

	/*
	 * external_at is where the bitmap of external fields is, when the tuple
	 * has one.
	 */
	bool		has_external_at;
	size_t		external_at;
	size_t		ends_at;
	size_t		data_at;
} EncodingTupleTuple;

/*
 * EncodingTupleFields walks a tuple's fields in order: the next field's
 * index, and where its data starts.
 */
typedef struct EncodingTupleFields
{
	const EncodingTupleTuple *tuple;
	size_t		next;
	size_t		start;
} EncodingTupleFields;

/*
 * encoding_tuple_write_overflow_t is a row's overflow: the address of its
 * blob, and its whole count of fields.
 */
typedef struct encoding_tuple_write_overflow_t
{
	const uint8 *f0;
	size_t		f1;
} encoding_tuple_write_overflow_t;

extern void encoding_tuple_write(const StdBytes *fields, size_t fields_len,
								 bool has_overflow, encoding_tuple_write_overflow_t overflow,
								 StdVec *result);
extern void encoding_tuple_write_with_external(const StdBytes *fields,
											   size_t fields_len,
											   const bool *external,
											   size_t external_len,
											   bool has_overflow,
											   encoding_tuple_write_overflow_t overflow,
											   StdVec *result);
extern size_t encoding_tuple_written_len(const StdUsizeOption *fields,
										 size_t fields_len, bool external,
										 bool has_overflow, size_t overflow);
extern void encoding_tuple_reference(uint64 length, const uint8 *address,
									 StdVec *result);
extern bool encoding_tuple_Tuple_parse(const uint8 *bytes, size_t bytes_len,
									   EncodingTupleTuple *result);
extern size_t encoding_tuple_Tuple_len(const EncodingTupleTuple *self);
extern bool encoding_tuple_Tuple_is_empty(const EncodingTupleTuple *self);
#ifdef DOLTGRES_TEST_ENCODING
extern bool encoding_tuple_Tuple_has_external(const EncodingTupleTuple *self);
#endif
extern EncodingTupleField encoding_tuple_Tuple_field(const EncodingTupleTuple *self,
													 size_t i);
extern EncodingTupleFields encoding_tuple_Tuple_fields(const EncodingTupleTuple *self);
extern bool encoding_tuple_Tuple_overflow(const EncodingTupleTuple *self,
										  StdBytes *result_0,
										  size_t *result_1);
extern EncodingTupleField encoding_tuple_Fields_at(EncodingTupleFields *self,
												   size_t i);
extern bool encoding_tuple_Fields_Iterator_next(EncodingTupleFields *self,
												EncodingTupleField *result);
extern bool encoding_tuple_Field_PartialEq_eq(const EncodingTupleField *self,
											  const EncodingTupleField *other);
extern bool encoding_tuple_Field_Debug_fmt(const void *self, StdFormatter *f);

#ifdef DOLTGRES_TEST_ENCODING
extern void test_encoding_tuple_tests_fields_round_trip(void);
extern void test_encoding_tuple_tests_overflow_names_the_rest(void);
extern void test_encoding_tuple_tests_external_fields_name_their_blobs(void);
extern void test_encoding_tuple_tests_written_lengths_match_the_tuples(void);
extern void test_encoding_tuple_tests_malformed_ends_are_refused(void);
#endif

#endif							/* DOLTGRES_ENCODING_TUPLE_H */
