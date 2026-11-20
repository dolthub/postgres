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
 * A heap row's columns and the tuples rows and index entries are stored as
 * (access' heaptuple.rs).
 */
#ifndef DOLTGRES_ACCESS_HEAPTUPLE_H
#define DOLTGRES_ACCESS_HEAPTUPLE_H

#ifndef FRONTEND
#include "doltgres/access/key.h"
#include "doltgres/access/transform.h"
#include "doltgres/chunks/address.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/pg/lib.h"
#include "doltgres/std/str.h"

/* AccessHeaptupleStorage is Storage: how a column's values may be kept, Postgres' attstorage. */
typedef enum AccessHeaptupleStorage
{
	ACCESS_HEAPTUPLE_STORAGE_PLAIN,
	ACCESS_HEAPTUPLE_STORAGE_EXTERNAL,
	ACCESS_HEAPTUPLE_STORAGE_MAIN,
	ACCESS_HEAPTUPLE_STORAGE_EXTENDED,
} AccessHeaptupleStorage;

/*
 * AccessHeaptupleColumn is Column: a row's column, its type, length,
 * by-value flag, alignment, storage, whether it is dropped, and the field it
 * stands for, if any.
 */
typedef struct AccessHeaptupleColumn
{
	uint32		typid;
	int16		len;
	bool		by_value;
	size_t		align;
	AccessHeaptupleStorage storage;
	bool		dropped;
	bool		has_field;
	size_t		field;
} AccessHeaptupleColumn;

/* AccessHeaptupleSetTag is Set's variants. */
typedef enum AccessHeaptupleSetTag
{
	ACCESS_HEAPTUPLE_SET_NULL,
	ACCESS_HEAPTUPLE_SET_WORD,
	ACCESS_HEAPTUPLE_SET_FIXED,
	ACCESS_HEAPTUPLE_SET_VARLENA,
} AccessHeaptupleSetTag;

/* AccessHeaptupleSet is Set: a column's value as a row's forming takes it, borrowed. */
typedef struct AccessHeaptupleSet
{
	AccessHeaptupleSetTag tag;
	union
	{
		uint64		word;
		StdBytes	fixed;
		StdBytes varlena;
	}			u;
} AccessHeaptupleSet;

/*
 * ACCESS_HEAPTUPLE_KEYLESS_KEY_LEN is the length of the key of a row of a
 * table without a primary key, the start of its tuple's hash.
 */
#define ACCESS_HEAPTUPLE_KEYLESS_KEY_LEN 16

/*
 * ACCESS_HEAPTUPLE_TOAST_TUPLE_THRESHOLD is the length of a tuple past which a
 * row's values move out of line, Postgres' own.
 */
#define ACCESS_HEAPTUPLE_TOAST_TUPLE_THRESHOLD 2032

/*
 * ACCESS_HEAPTUPLE_TOAST_TUPLE_TARGET is the length of a tuple its values move
 * out of line until it is under, Postgres' default.
 */
#define ACCESS_HEAPTUPLE_TOAST_TUPLE_TARGET 2032

/*
 * AccessHeaptupleRowLayout is RowLayout: a table's columns in attribute
 * number order (a Vec<Column>), its primary key's columns (a
 * Vec<(usize, Order, KeyType)>), the bound a key column is cut at, and for
 * each column whether a row's key can give its value back (a Vec<bool>).
 */
typedef struct AccessHeaptupleRowLayout
{
	StdVec		columns;
	StdVec		key;
	size_t		bound;
	StdVec		exact;
} AccessHeaptupleRowLayout;

/* AccessHeaptupleKept is Kept: a value an update keeps out of line, its column, its length, and its blob's address. */
typedef struct AccessHeaptupleKept
{
	size_t		column;
	uint64		length;
	const uint8 *address;
} AccessHeaptupleKept;

/* AccessHeaptupleDeformedTag is Deformed's variants. */
typedef enum AccessHeaptupleDeformedTag
{
	ACCESS_HEAPTUPLE_DEFORMED_VALUE,
	ACCESS_HEAPTUPLE_DEFORMED_MISSING,
	ACCESS_HEAPTUPLE_DEFORMED_OUT_OF_LINE,
	ACCESS_HEAPTUPLE_DEFORMED_OVERFLOWED,
} AccessHeaptupleDeformedTag;

/*
 * AccessHeaptupleDeformed is Deformed: a column's value as a stored row
 * gives it back, its missing value, where a value kept out of line is, or
 * the overflow chunk that holds it.
 */
typedef struct AccessHeaptupleDeformed
{
	AccessHeaptupleDeformedTag tag;
	union
	{
		AccessHeaptupleSet value;
		struct
		{
			uint64		length;
			StdBytes	address;
			size_t		at;
			size_t		value;
		}			out_of_line;
		struct
		{
			StdBytes	address;
			size_t		at;
			size_t		field;
			size_t		before;
		}			overflowed;
	}			u;
} AccessHeaptupleDeformed;

/*
 * AccessHeaptupleMissingFn is transform_to's missing: the stored bytes of a
 * column's missing value, or none for NULL.
 */
typedef bool (*AccessHeaptupleMissingFn) (void *captures, size_t i, bool *has_result, StdVec *result,
										  PgPgError *error);

/* AccessHeaptupleSetFn is a deform's set: what it does with each column's value, which may fail. */
typedef bool (*AccessHeaptupleSetFn) (void *captures, AccessHeaptupleDeformed deformed, PgPgError *error);

/*
 * AccessHeaptupleFormed is Formed: a row as its table's tree stores it, its
 * key, its tuple, where the tuple holds addresses (a Vec<(u32, Locator)>),
 * and the room the row takes on heap pages.
 */
typedef struct AccessHeaptupleFormed
{
	StdVec		key;
	StdVec		tuple;
	StdVec		addresses;
	uint64		heap_bytes;
} AccessHeaptupleFormed;

/* AccessHeaptupleBlobTag is Blob's variants. */
typedef enum AccessHeaptupleBlobTag
{
	ACCESS_HEAPTUPLE_BLOB_VALUE,
	ACCESS_HEAPTUPLE_BLOB_CHUNK,
} AccessHeaptupleBlobTag;

/* AccessHeaptupleBlob is Blob: a value to write as a blob, or an overflow chunk's node with its addresses' locators. */
typedef struct AccessHeaptupleBlob
{
	AccessHeaptupleBlobTag tag;
	union
	{
		StdBytes	value;
		struct
		{
			StdBytes	node;
			const ChunksTrailerLocator *locators;
			size_t		locators_len;
		}			chunk;
	}			u;
} AccessHeaptupleBlob;

/* access_OptionCowBytes_t is an Option<Cow<'_, [u8]>>: none, borrowed bytes, or owned ones, a Vec<u8>. */
typedef struct access_OptionCowBytes_t
{
	bool		has;
	bool		owned;
	StdBytes	borrowed;
	StdVec		bytes;
} access_OptionCowBytes_t;

/* access_TupleUsizeUsize_t is a (usize, usize): a tuple's first field and its count of fields. */
typedef struct access_TupleUsizeUsize_t
{
	size_t		f0;
	size_t		f1;
} access_TupleUsizeUsize_t;

/*
 * AccessHeaptuplePlan is Plan: each field's length (an Option<usize>),
 * whether it moves out of line, the fields that move in order, and each
 * tuple's first field and count of fields (a Vec<(usize, usize)>).
 */
typedef struct AccessHeaptuplePlan
{
	StdVec		lengths;
	StdVec		external;
	StdVec		moves;
	StdVec		levels;
} AccessHeaptuplePlan;

/*
 * AccessHeaptuplePlacing is Placing: a row being placed in its table's
 * tree, its key, whether it is keyless, which fields an update kept, its
 * fields (Option<Cow<[u8]>>s), which are out of line, their locators, the
 * moves and levels of its plan, the blobs written, the chunk asked for
 * next, the overflow chunk written last, and its heap bytes.
 */
typedef struct AccessHeaptuplePlacing
{
	StdVec		key;
	bool		keyless;
	StdVec		kept;
	StdVec		fields;
	StdVec		external;
	StdVec		locators;
	StdVec		moves;
	StdVec		levels;
	size_t		step;
	bool		has_chunk;
	StdVec		chunk_node;
	StdVec		chunk_locators;
	bool		has_overflow;
	ChunksAddressAddress overflow_address;
	ChunksTrailerLocator overflow_locator;
	uint64		heap_bytes;
} AccessHeaptuplePlacing;

/* ACCESS_HEAPTUPLE_ENTRY_OVERHEAD is what a tree's leaf entry counts beyond its key and value. */
#define ACCESS_HEAPTUPLE_ENTRY_OVERHEAD 4

/* ACCESS_HEAPTUPLE_MAX_COUNT_LEN is the longest count of copies before a keyless row's tuple. */
#define ACCESS_HEAPTUPLE_MAX_COUNT_LEN 10

/*
 * AccessHeaptupleKind is one of Plan::new's kinds: the storage kinds whose
 * values move, and the length the tuple moves them until it is under.
 */
typedef struct AccessHeaptupleKind
{
	const AccessHeaptupleStorage *movable;
	size_t		movable_len;
	size_t		target;
} AccessHeaptupleKind;

extern bool access_heaptuple_Set_PartialEq_eq(const AccessHeaptupleSet *self, const AccessHeaptupleSet *other);
extern uint8 access_heaptuple_Storage_byte(AccessHeaptupleStorage self);
extern void access_heaptuple_Plan_new(const access_OptionCowBytes_t *fields, size_t fields_len, const int16 *typlens,
									  size_t typlens_len, const AccessHeaptupleStorage *storages,
									  size_t storages_len, const size_t *forced, size_t forced_len,
									  size_t threshold, const AccessHeaptupleKind *kinds, size_t kinds_len,
									  size_t room, AccessHeaptuplePlan *result);
extern void access_heaptuple_RowLayout_new(StdVec *columns, StdVec *key, AccessHeaptupleRowLayout *result);
extern void access_heaptuple_RowLayout_drop(AccessHeaptupleRowLayout *self);
extern bool access_heaptuple_RowLayout_transform_to(const AccessHeaptupleRowLayout *self,
													const AccessHeaptupleRowLayout *later,
													AccessHeaptupleMissingFn missing, void *captures, uint32 target,
													uint32 later_target, bool *has_result,
													AccessTransformTransform *result, PgPgError *error);
extern bool access_heaptuple_RowLayout_deform(const AccessHeaptupleRowLayout *self, const uint8 *key,
											  size_t key_len, const uint8 *tuple, size_t tuple_len,
											  bool has_sources, const AccessTransformField *sources,
											  size_t sources_len, size_t from, size_t upto,
											  AccessHeaptupleSetFn set, void *captures, bool *has_result,
											  size_t *result, PgPgError *error);
extern bool access_heaptuple_RowLayout_overflowed(const AccessHeaptupleRowLayout *self, const uint8 *key,
												  size_t key_len, size_t column, const uint8 *chunk,
												  size_t chunk_len, size_t field, size_t before,
												  AccessHeaptupleSetFn set, void *captures, bool *has_result,
												  size_t *result, PgPgError *error);
extern bool access_heaptuple_RowLayout_placing(const AccessHeaptupleRowLayout *self, AccessKeySortKeys sorter,
											   AccessKeyNames names, const AccessHeaptupleSet *values,
											   size_t values_len, const AccessHeaptupleKept *kept, size_t kept_len,
											   bool external, size_t target, AccessHeaptuplePlacing *result,
											   PgPgError *error);
extern bool access_heaptuple_field_of(const AccessHeaptupleColumn *column, const AccessHeaptupleSet *value,
									  access_OptionCowBytes_t *result, PgPgError *error);
extern bool access_heaptuple_Placing_new(StdVec *key, bool keyless, StdVec *fields, StdVec *kept,
										 AccessHeaptuplePlan *plan, uint64 heap_bytes,
										 AccessHeaptuplePlacing *result, PgPgError *error);
extern void access_heaptuple_Placing_drop(AccessHeaptuplePlacing *self);
extern void access_heaptuple_Formed_drop(AccessHeaptupleFormed *self);
extern void access_heaptuple_field_drop(void *field_);
extern void access_heaptuple_Plan_drop(AccessHeaptuplePlan *self);
extern bool access_heaptuple_Placing_field(const AccessHeaptuplePlacing *self, size_t *result);
extern void access_heaptuple_Placing_kept(const AccessHeaptuplePlacing *self, StdVec *result);
extern bool access_heaptuple_Placing_blob(const AccessHeaptuplePlacing *self, AccessHeaptupleBlob *result);
extern bool access_heaptuple_Placing_wrote(AccessHeaptuplePlacing *self, ChunksAddressAddress address,
										   ChunksTrailerLocator locator, PgPgError *error);
extern bool access_heaptuple_Placing_formed(AccessHeaptuplePlacing *self, AccessHeaptupleFormed *result,
											PgPgError *error);
extern bool access_heaptuple_word_of(const uint8 *bytes, size_t bytes_len, uint64 *result, PgPgError *error);
extern bool access_heaptuple_Storage_of(uint8 byte, AccessHeaptupleStorage *result, PgPgError *error);
#ifdef DOLTGRES_TEST_ACCESS
extern bool access_heaptuple_Formed_PartialEq_eq(const AccessHeaptupleFormed *self, const AccessHeaptupleFormed *other);
extern void test_access_heaptuple_tests_a_chunks_first_field_too_long_for_it_moves_out_of_line(void);
extern void test_access_heaptuple_tests_a_key_column_under_at_and_over_its_bound_forms_and_deforms_to_its_value(void);
extern void test_access_heaptuple_tests_a_keyless_row_is_keyed_by_its_tuples_hash(void);
extern void test_access_heaptuple_tests_a_lossy_key_column_and_short_tuples_read_from_the_tuple(void);
extern void test_access_heaptuple_tests_a_lossy_key_column_forms_into_both(void);
extern void test_access_heaptuple_tests_a_row_deforms_to_its_fields_and_key_columns(void);
extern void test_access_heaptuple_tests_a_row_forms_to_the_key_and_tuple_it_deforms_from(void);
extern void test_access_heaptuple_tests_a_row_holding_a_value_out_of_line_toasts_to_its_target(void);
extern void test_access_heaptuple_tests_a_row_of_main_values_counts_the_heap_tuple_heaps_rules_can_store(void);
extern void test_access_heaptuple_tests_a_row_past_the_threshold_moves_its_biggest_extended_and_external_values_out_first(void);
extern void test_access_heaptuple_tests_a_row_stored_at_an_older_epoch_deforms_through_its_transform(void);
extern void test_access_heaptuple_tests_a_row_that_does_not_decode_is_refused(void);
extern void test_access_heaptuple_tests_a_row_that_does_not_form_is_refused(void);
extern void test_access_heaptuple_tests_a_row_with_a_moved_value_and_two_nested_overflow_chunks_walks_every_request(void);
extern void test_access_heaptuple_tests_a_rows_heap_length_is_the_length_heap_gives_it(void);
extern void test_access_heaptuple_tests_a_value_kept_out_of_line_deforms_to_where_it_is(void);
extern void test_access_heaptuple_tests_a_value_moves_out_of_line_by_its_rows_heap_tuple(void);
extern void test_access_heaptuple_tests_a_whole_deform_answers_the_rows_count_of_values_out_of_line(void);
extern void test_access_heaptuple_tests_deforming_goes_on_from_where_it_stopped(void);
extern void test_access_heaptuple_tests_main_values_move_out_only_while_the_row_is_too_large_for_a_node(void);
extern void test_access_heaptuple_tests_rows_whose_cut_keys_share_their_first_bytes_have_their_own_keys(void);
#endif
#endif

#endif							/* DOLTGRES_ACCESS_HEAPTUPLE_H */
