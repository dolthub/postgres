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
 * Index keys of Postgres' types (access' key.rs): what a value of a type
 * sorts by, each type's encoding as a key column, and the decoding of the
 * types whose keys are exact.
 */
#ifndef DOLTGRES_ACCESS_KEY_H
#define DOLTGRES_ACCESS_KEY_H

#include "doltgres/encoding/key.h"
#ifndef FRONTEND
#include "doltgres/pg/lib.h"
#endif
#include "doltgres/std/fmt.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"

/* AccessKeyCollationTag is Collation's variants. */
typedef enum AccessKeyCollationTag
{
	/* Bytes is text in byte order. */
	ACCESS_KEY_COLLATION_BYTES,

	/*
	 * Sorted is text by the sort keys of a collation, by its OID, and then by
	 * its bytes when it is deterministic.
	 */
	ACCESS_KEY_COLLATION_SORTED,
} AccessKeyCollationTag;

/*
 * AccessKeyCollation is Collation: how a key type's text sorts, by its bytes,
 * or by a collation's sort keys and then, under a deterministic collation,
 * its bytes.
 */
typedef struct AccessKeyCollation
{
	AccessKeyCollationTag tag;
	/* id is Sorted's collation's OID. */
	uint32		id;
	/* deterministic is whether Sorted's collation is deterministic. */
	bool		deterministic;
} AccessKeyCollation;

typedef struct AccessKeyKeyType AccessKeyKeyType;

/* AccessKeyInnerTag is Inner's variants. */
typedef enum AccessKeyInnerTag
{
	ACCESS_KEY_INNER_NONE,
	ACCESS_KEY_INNER_ARRAY,
	ACCESS_KEY_INNER_RANGE,
	ACCESS_KEY_INNER_MULTIRANGE,
	ACCESS_KEY_INNER_ROW,
	ACCESS_KEY_INNER_ENUM,
	ACCESS_KEY_INNER_COMPARED,
} AccessKeyInnerTag;

/*
 * AccessKeyStored is what reading an array's elements or a range's bounds
 * needs of their type: its key type, boxed, and its stored length, whether
 * it is passed by value, and its alignment (`typlen`, `typbyval`, and
 * `typalign`).
 */
typedef struct AccessKeyStored
{
	AccessKeyKeyType *key_type;
	int16		len;
	bool		by_value;
	uint8		align;
} AccessKeyStored;

/*
 * AccessKeyInner is Inner: what the values of a key type hold whose keys
 * their own key types give.
 */
typedef struct AccessKeyInner
{
	AccessKeyInnerTag tag;
	union
	{
		/* array is Array's elements. */
		AccessKeyStored array;
		/* range is Range's bounds. */
		AccessKeyStored range;
		/* multirange is Multirange's range type's key type, boxed. */
		AccessKeyKeyType *multirange;
		/* row is Row's attributes, a Vec<Attribute> in their order. */
		StdVec		row;

		/*
		 * labels is Enum's labels' sort orders by their OIDs, a Vec<(u32,
		 * f32)> in the OIDs' order.
		 */
		StdVec		labels;
		/* by_value is Compared's: whether the type is passed by value. */
		bool		by_value;
	}			u;
} AccessKeyInner;

/* AccessKeyKeyType is KeyType: what a value of a type sorts by, its type's OID, its text's collation, and what it holds. */
struct AccessKeyKeyType
{
	uint32		typid;
	AccessKeyCollation collation;
	AccessKeyInner inner;
};

/*
 * AccessKeyAttribute is Attribute: an attribute of a row type, its key type,
 * its type's stored length, whether it is passed by value, its alignment in
 * bytes, and whether it was dropped, whose stored value a row's key leaves
 * out.
 */
typedef struct AccessKeyAttribute
{
	AccessKeyKeyType key_type;
	int16		len;
	bool		by_value;
	size_t		align;
	bool		dropped;
} AccessKeyAttribute;

/* access_TupleU32F32_t is the tuple (u32, f32): an enum label's OID and its sort order. */
typedef struct access_TupleU32F32_t
{
	uint32		f0;
	float4		f1;
} access_TupleU32F32_t;

/*
 * access_TupleUsizeOrderKeyType_t is a key column as a layout holds it, a
 * (usize, Order, KeyType): its column's position, its order, and its key
 * type.
 */
typedef struct access_TupleUsizeOrderKeyType_t
{
	size_t		f0;
	EncodingKeyOrder f1;
	AccessKeyKeyType f2;
} access_TupleUsizeOrderKeyType_t;

/*
 * AccessKeyKeyTypes is an impl Iterator<Item = &KeyType> over a slice: its
 * elements, each holding a key type at an offset.
 */
typedef struct AccessKeyKeyTypes
{
	const void *items;
	size_t		len;
	size_t		stride;
	size_t		offset;
} AccessKeyKeyTypes;

extern bool access_key_Collation_PartialEq_eq(const AccessKeyCollation *self, const AccessKeyCollation *other);
extern AccessKeyKeyType access_key_KeyType_of(uint32 typid);
extern void access_key_Inner_drop(AccessKeyInner *self);
extern void access_key_KeyType_drop(AccessKeyKeyType *self);
extern void access_TupleUsizeOrderKeyType_drop(void *keyed);
extern void access_key_Attribute_Clone_clone(const AccessKeyAttribute *self, AccessKeyAttribute *result);
extern void access_key_Inner_Clone_clone(const AccessKeyInner *self, AccessKeyInner *result);
extern void access_key_KeyType_Clone_clone(const AccessKeyKeyType *self, AccessKeyKeyType *result);
extern bool access_key_KeyType_try_clone(const AccessKeyKeyType *self, AccessKeyKeyType *result);
extern void access_key_KeyType_write(const AccessKeyKeyType *self, StdVec *out);
extern bool access_key_KeyType_read(const uint8 *bytes, size_t bytes_len, AccessKeyKeyType *result_0,
									StdBytes *result_1);

#ifndef FRONTEND
/* AccessKeySortKeysOps is SortKeys: the sort key of a text under a collation, which an encoding asks. */
typedef struct AccessKeySortKeysOps
{
	bool		(*sort_key) (const void *self, uint32 collation, const uint8 *text, size_t text_len, StdVec *result,
							 PgPgError *error);
} AccessKeySortKeysOps;

/* AccessKeySortKeys is a value that implements SortKeys. */
typedef struct AccessKeySortKeys
{
	const AccessKeySortKeysOps *ops;
	const void *self;
} AccessKeySortKeys;

/* AccessKeyNamesOps is Names: the name a value kept out of line past a tree's bound is found by. */
typedef struct AccessKeyNamesOps
{
	void		(*name) (const void *self, const uint8 *stored, size_t stored_len, uint8 result[32]);
} AccessKeyNamesOps;

/* AccessKeyNames is a value that implements Names. */
typedef struct AccessKeyNames
{
	const AccessKeyNamesOps *ops;
	const void *self;
} AccessKeyNames;

/* AccessKeyValueTag is Value's variants. */
typedef enum AccessKeyValueTag
{
	ACCESS_KEY_VALUE_WORD,
	ACCESS_KEY_VALUE_BYTES,
} AccessKeyValueTag;

/* AccessKeyValue is Value: a value to encode, a by-value word or a stored value's bytes. */
typedef struct AccessKeyValue
{
	AccessKeyValueTag tag;
	union
	{
		uint64		word;
		StdBytes	bytes;
	}			u;
} AccessKeyValue;

/* AccessKeyDecodedTag is Decoded's variants. */
typedef enum AccessKeyDecodedTag
{
	ACCESS_KEY_DECODED_WORD,
	ACCESS_KEY_DECODED_BYTES,
} AccessKeyDecodedTag;

/* AccessKeyDecoded is Decoded: a value decoded from a key, a by-value word or a stored value's bytes (a Vec<u8>). */
typedef struct AccessKeyDecoded
{
	AccessKeyDecodedTag tag;
	union
	{
		uint64		word;
		StdVec		bytes;
	}			u;
} AccessKeyDecoded;

extern const AccessKeySortKeysOps ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS;
extern bool access_key_has_own_key(uint32 ty);
extern bool access_key_encode(StdVec *out, AccessKeySortKeys sorter, const AccessKeyKeyType *key_type, bool has_value,
							  AccessKeyValue value, EncodingKeyOrder order, PgPgError *error);
extern bool access_key_decode(const uint8 *key, size_t key_len, const AccessKeyKeyType *key_type,
							  EncodingKeyOrder order, bool *has_result_0, AccessKeyDecoded *result_0,
							  StdBytes *result_1, PgPgError *error);
extern bool access_key_is_exact(const AccessKeyKeyType *key_type);
extern bool access_key_decoded_max(const AccessKeyKeyType *key_type, EncodingKeyOrder order, const uint8 *key,
								   size_t key_len, size_t *result, PgPgError *error);
extern size_t access_key_table_bound(AccessKeyKeyTypes key);
extern size_t access_key_table_key_most(AccessKeyKeyTypes key);
extern size_t access_key_index_bound(AccessKeyKeyTypes key, size_t row_key, size_t prefix, const size_t *parts,
									 size_t parts_len);
extern bool access_key_can_be_cut(const AccessKeyKeyType *key_type, size_t bound);
extern bool access_key_encode_within(StdVec *out, AccessKeySortKeys sorter, AccessKeyNames names,
									 const AccessKeyKeyType *key_type, bool has_value, AccessKeyValue value,
									 EncodingKeyOrder order, size_t bound, bool outside, bool *result,
									 PgPgError *error);
extern bool access_key_skip_within(const uint8 *key, size_t key_len, const AccessKeyKeyType *key_type,
								   EncodingKeyOrder order, size_t bound, StdBytes *result_0, bool *result_1,
								   PgPgError *error);
#ifdef DOLTGRES_TEST_ACCESS
extern const AccessKeySortKeysOps ACCESS_KEY_REVERSED_SORT_KEYS_OPS;
extern const AccessKeyNamesOps ACCESS_KEY_REVERSED_NAMES_OPS;
extern bool access_key_Decoded_PartialEq_eq(const AccessKeyDecoded *self, const AccessKeyDecoded *other);
extern bool access_key_Decoded_Debug_fmt(const void *value, StdFormatter *f);
extern void test_access_key_tests_a_bpchar_key_ignores_trailing_spaces_and_is_skipped_to_its_end(void);
extern void test_access_key_tests_a_key_column_is_cut_one_byte_past_its_bound(void);
extern void test_access_key_tests_a_key_column_past_its_bound_is_cut_and_skipped_by_the_bound(void);
extern void test_access_key_tests_a_stored_value_that_is_not_its_types_form_is_refused(void);
extern void test_access_key_tests_a_tables_bound_follows_its_keys_types(void);
extern void test_access_key_tests_a_tsvector_of_many_short_lexemes_reads_back_from_its_key(void);
extern void test_access_key_tests_a_type_without_an_encoding_and_a_key_that_does_not_decode_are_refused(void);
extern void test_access_key_tests_a_value_read_from_a_key_is_within_its_bound(void);
extern void test_access_key_tests_an_indexs_bound_fits_its_largest_entry(void);
extern void test_access_key_tests_arrays_order_as_postgres_compares_them(void);
extern void test_access_key_tests_collated_text_keys_are_their_sort_keys_then_their_bytes(void);
extern void test_access_key_tests_compared_and_enum_keys_are_their_bytes_and_sort_orders(void);
extern void test_access_key_tests_exact_keys_round_trip_and_keys_order_as_postgres_does(void);
extern void test_access_key_tests_float_keys_order_as_postgres_does_and_are_skipped_by_their_width(void);
extern void test_access_key_tests_numeric_and_interval_keys_order_as_postgres_does_and_are_skipped(void);
extern void test_access_key_tests_the_keys_of_stored_forms_order_as_postgres_does(void);
extern void test_access_key_tests_the_stored_forms_of_exact_types_round_trip(void);
extern void test_access_key_tests_an_answer_of_none_leaves_its_length_as_it_was(void);
#endif
#endif

#endif							/* DOLTGRES_ACCESS_KEY_H */
