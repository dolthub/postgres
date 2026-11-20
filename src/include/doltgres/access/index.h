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
 * A btree index's entries (access' index.rs): its key columns, each cut at
 * the index's bound, then the row's key wherever Postgres' uniqueness lets
 * two rows share the columns' keys, and its value, the parts a reader needs
 * beyond the key, placed as a row is.
 */
#ifndef DOLTGRES_ACCESS_INDEX_H
#define DOLTGRES_ACCESS_INDEX_H

#ifndef FRONTEND
#include "doltgres/access/heaptuple.h"
#include "doltgres/access/key.h"
#include "doltgres/std/vec.h"

/*
 * AccessIndexUnique is Unique: whether an index's entries are keyed as unique,
 * and if so whether its NULLs are distinct.
 */
typedef enum AccessIndexUnique
{
	ACCESS_INDEX_UNIQUE_NO,
	ACCESS_INDEX_UNIQUE_NULLS_DISTINCT,
	ACCESS_INDEX_UNIQUE_NULLS_NOT_DISTINCT,
} AccessIndexUnique;

/* access_TupleOrderKeyType_t is a key column as an index holds it, an (Order, KeyType). */
typedef struct access_TupleOrderKeyType_t
{
	EncodingKeyOrder f0;
	AccessKeyKeyType f1;
} access_TupleOrderKeyType_t;

/* access_TupleOrderKeyTypeColumn_t is a key column as an index is made from it, an (Order, KeyType, Column). */
typedef struct access_TupleOrderKeyTypeColumn_t
{
	EncodingKeyOrder f0;
	AccessKeyKeyType f1;
	AccessHeaptupleColumn f2;
} access_TupleOrderKeyTypeColumn_t;

/* access_TupleOrderKeyTypeColumnRef_t is a key column as key_columns gives it, an (Order, &KeyType, &Column). */
typedef struct access_TupleOrderKeyTypeColumnRef_t
{
	EncodingKeyOrder f0;
	const AccessKeyKeyType *f1;
	const AccessHeaptupleColumn *f2;
} access_TupleOrderKeyTypeColumnRef_t;

/* access_OptionValue_t is an Option<Value>: a key column's value, or none for NULL. */
typedef struct access_OptionValue_t
{
	bool		has;
	AccessKeyValue value;
} access_OptionValue_t;

/* access_OptionVecU8_t is an Option<Vec<u8>>: a value's field's bytes, or none for NULL. */
typedef struct access_OptionVecU8_t
{
	bool		has;
	StdVec		bytes;
} access_OptionVecU8_t;

/* access_OptionDecoded_t is an Option<Decoded>: a column's decoded value, or none for NULL. */
typedef struct access_OptionDecoded_t
{
	bool		has;
	AccessKeyDecoded value;
} access_OptionDecoded_t;

/*
 * AccessIndexIndexKey is IndexKey: how a btree index's entries are made, its
 * key columns' orders and key types (a Vec<(Order, KeyType)>), the bound a
 * column is cut at, whether it is unique, how each key column's original
 * and each INCLUDE column is stored (Vec<Column>s), and whether its table is
 * keyless.
 */
typedef struct AccessIndexIndexKey
{
	StdVec		columns;
	size_t		bound;

	/*
	 * compared is whether the index's tree is ordered by a comparator, so
	 * every column past the bound is out of line.
	 */
	bool		compared;
	AccessIndexUnique unique;
	StdVec		originals;
	StdVec		includes;
	bool		keyless;
} AccessIndexIndexKey;

/* AccessIndexEntry is Entry: an entry's key, its value, and where the value holds addresses (a Vec<(u32, Locator)>). */
typedef struct AccessIndexEntry
{
	StdVec		key;
	StdVec		value;
	StdVec		addresses;
} AccessIndexEntry;

/* AccessIndexEntryKey is EntryKey: an entry's key and which of its columns were cut (a Vec<bool>). */
typedef struct AccessIndexEntryKey
{
	StdVec		key;
	StdVec		cut;
} AccessIndexEntryKey;

/* AccessIndexKeyColumns is key_columns' iterator: the index's key columns from a position on. */
typedef struct AccessIndexKeyColumns
{
	const AccessIndexIndexKey *index;
	size_t		next;
} AccessIndexKeyColumns;

extern AccessIndexUnique access_index_Unique_of(bool unique, bool immediate, bool nulls_not_distinct);
extern void access_index_IndexKey_new(StdVec *columns, StdVec *includes, AccessIndexUnique unique, bool keyless,
									  size_t row_key, const size_t *parts, size_t parts_len,
									  AccessIndexIndexKey *result);
extern void access_index_IndexKey_drop(AccessIndexIndexKey *self);
extern size_t access_index_IndexKey_bound(const AccessIndexIndexKey *self);
extern AccessIndexKeyColumns access_index_IndexKey_key_columns(const AccessIndexIndexKey *self);
extern bool access_index_KeyColumns_Iterator_next(AccessIndexKeyColumns *self,
												  access_TupleOrderKeyTypeColumnRef_t *result);
extern bool access_index_IndexKey_encode(const AccessIndexIndexKey *self, AccessKeySortKeys sorter, AccessKeyNames names,
										 const access_OptionValue_t *values, size_t values_len, const uint8 *row_key,
										 size_t row_key_len, AccessIndexEntryKey *result, PgPgError *error);
extern void access_index_EntryKey_drop(AccessIndexEntryKey *self);
extern bool access_index_IndexKey_value(const AccessIndexIndexKey *self, const AccessIndexEntryKey *key,
										const uint8 *row_key, size_t row_key_len, const AccessHeaptupleSet *originals,
										size_t originals_len, const AccessHeaptupleSet *includes, size_t includes_len,
										AccessHeaptuplePlacing *result, PgPgError *error);
extern bool access_index_IndexKey_row_key(const uint8 *value, size_t value_len, StdBytes *result, PgPgError *error);
extern bool access_index_IndexKey_decode(const AccessIndexIndexKey *self, const uint8 *key, size_t key_len,
										 bool includes, const access_OptionVecU8_t *fields, size_t fields_len,
										 StdVec *result, PgPgError *error);
extern void access_index_decoded_drop(void *value);
extern size_t access_index_IndexKey_table_bound(const AccessIndexIndexKey *self);
extern bool access_index_IndexKey_rest(const AccessIndexIndexKey *self, const uint8 *key, size_t key_len,
									   StdBytes *result, PgPgError *error);
extern bool access_index_IndexKey_run_prefix(const AccessIndexIndexKey *self, const uint8 *key, size_t key_len,
											 size_t bound, bool *has_result, size_t *result, PgPgError *error);
extern bool access_index_IndexKey_whole(const AccessIndexIndexKey *self, AccessKeySortKeys sorter,
										const access_OptionValue_t *values, size_t values_len, const uint8 *key,
										size_t key_len, size_t bound, StdVec *result, PgPgError *error);
extern bool access_index_IndexKey_row_key_bytes(const AccessIndexIndexKey *self, const uint8 *key, size_t key_len,
												const uint8 *value, size_t value_len, StdBytes *result,
												PgPgError *error);
extern AccessIndexEntry access_index_IndexKey_entry(AccessHeaptupleFormed *formed);
extern void access_index_Entry_drop(AccessIndexEntry *self);
#ifdef DOLTGRES_TEST_ACCESS
extern void test_access_index_tests_a_deferrable_unique_index_is_keyed_as_not_unique(void);
extern void test_access_index_tests_a_key_column_is_cut_at_the_indexs_bound(void);
extern void test_access_index_tests_a_non_unique_indexs_entries_are_doltgrusts(void);
extern void test_access_index_tests_a_unique_index_keys_equal_values_alike(void);
extern void test_access_index_tests_a_unique_indexs_bound_counts_its_values_row_key(void);
extern void test_access_index_tests_a_unique_indexs_entry_wider_than_nbtrees_limit_is_formed(void);
extern void test_access_index_tests_a_unique_indexs_largest_formed_entry_fits(void);
extern void test_access_index_tests_an_include_column_moves_out_of_line_and_a_keyless_entry_leaves_room_for_its_count(void);
extern void test_access_index_tests_each_part_of_an_entrys_value_reads_back(void);
extern void test_access_index_tests_two_cut_keys_sharing_their_first_bytes_differ(void);
#endif
#endif

#endif							/* DOLTGRES_ACCESS_INDEX_H */
