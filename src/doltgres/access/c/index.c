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
 * A btree index's entries (access' index.rs, design item 4): its key
 * columns, each cut at the index's bound (form D), and then the row's key
 * wherever Postgres' uniqueness lets two rows share the columns' keys; and
 * its value, the parts a reader needs beyond the key, placed as a row is.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/access/index.h"
#include "doltgres/chunks/lib.h"
#include "doltgres/encoding/key.h"
#include "doltgres/encoding/tuple.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "held.h"
#ifdef DOLTGRES_TEST_ACCESS
#include "catalog/pg_type_d.h"
#include "doltgres/chunks/address.h"
#include "doltgres/pg/word.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#include "index_tests.h"
#endif

#define RUST_FILE "access" STD_SEP "src" STD_SEP "index.rs"

/* The element layouts of index's vectors. */
#define ACCESS_INDEX_BYTES 1, 1
#define ACCESS_INDEX_BOOL sizeof(bool), _Alignof(bool)
#define ACCESS_INDEX_USIZE sizeof(size_t), _Alignof(size_t)
#define ACCESS_INDEX_I16 sizeof(int16), _Alignof(int16)
#define ACCESS_INDEX_STORAGE sizeof(AccessHeaptupleStorage), _Alignof(AccessHeaptupleStorage)
#define ACCESS_INDEX_COLUMN sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn)
#define ACCESS_INDEX_KEYED sizeof(access_TupleOrderKeyType_t), _Alignof(access_TupleOrderKeyType_t)
#define ACCESS_INDEX_MADE sizeof(access_TupleOrderKeyTypeColumn_t), _Alignof(access_TupleOrderKeyTypeColumn_t)
#define ACCESS_INDEX_FIELD sizeof(access_OptionCowBytes_t), _Alignof(access_OptionCowBytes_t)
#define ACCESS_INDEX_DECODED sizeof(access_OptionDecoded_t), _Alignof(access_OptionDecoded_t)
#define ACCESS_INDEX_LENGTH sizeof(StdUsizeOption), _Alignof(StdUsizeOption)
#define ACCESS_INDEX_LEVEL sizeof(access_TupleUsizeUsize_t), _Alignof(access_TupleUsizeUsize_t)
#define ACCESS_INDEX_ADDRESS sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t)

/* ACCESS_INDEX_MOVABLE is the storage kinds whose values an entry's value moves out of line, in their order. */
static const AccessHeaptupleStorage ACCESS_INDEX_MOVABLE[] = {ACCESS_HEAPTUPLE_STORAGE_EXTENDED,
ACCESS_HEAPTUPLE_STORAGE_EXTERNAL, ACCESS_HEAPTUPLE_STORAGE_MAIN};

/* access_index_fail sets the fork's error of a code and a message and answers false. */
static bool
access_index_fail(const char *code, const char *message, PgPgError *error)
{
	StdString	str;

	std_string_from_Checked(&str, message, strlen(message));
	*error = pg_PgError_fork((const uint8 *) code, &str);
	return false;
}

/* access_index_key_types returns an index's key columns' key types as an iterator over them. */
static AccessKeyKeyTypes
access_index_key_types(const StdVec *columns)
{
	AccessKeyKeyTypes key_types = {columns->ptr, columns->len, sizeof(access_TupleOrderKeyType_t),
	offsetof(access_TupleOrderKeyType_t, f1)};

	return key_types;
}

/* access_index_keyed_drop frees a key column's key type, as a vector's element. */
static void
access_index_keyed_drop(void *keyed)
{
	access_key_KeyType_drop(&((access_TupleOrderKeyType_t *) keyed)->f1);
}

/*
 * access_index_Unique_of returns how an index is keyed from its catalog row's
 * indisunique, indimmediate, and indnullsnotdistinct.
 */
AccessIndexUnique
access_index_Unique_of(bool unique, bool immediate, bool nulls_not_distinct)
{
	if (!(unique && immediate))
		return ACCESS_INDEX_UNIQUE_NO;
	return nulls_not_distinct ? ACCESS_INDEX_UNIQUE_NULLS_NOT_DISTINCT : ACCESS_INDEX_UNIQUE_NULLS_DISTINCT;
}

/*
 * access_index_new_frame_t is new's state across its pad: the parts its
 * bound counts, its key columns and originals, the INCLUDE columns it
 * takes, and the bound.
 */
typedef struct access_index_new_frame_t
{
	StdVec		parts;
	StdVec		columns;
	StdVec		originals;
	StdVec	   *includes;
	bool		live;
	size_t		in_key;
	size_t		prefix;
	size_t		bound;
} access_index_new_frame_t;

/* access_index_IndexKey_new_pad frees what new holds. */
static void
access_index_IndexKey_new_pad(void *frame_)
{
	access_index_new_frame_t *frame = frame_;

	if (frame->live)
	{
		std_vec_free_Checked(&frame->parts, ACCESS_INDEX_USIZE);
		std_vec_drop_Checked(&frame->columns, ACCESS_INDEX_KEYED, access_index_keyed_drop);
		std_vec_free_Checked(&frame->originals, ACCESS_INDEX_COLUMN);
		std_vec_free_Checked(frame->includes, ACCESS_INDEX_COLUMN);
	}
	frame->live = false;
}

/* access_index_IndexKey_new_body finds the index's bound. */
static void
access_index_IndexKey_new_body(void *frame_)
{
	access_index_new_frame_t *frame = frame_;

	frame->bound = access_key_index_bound(access_index_key_types(&frame->columns), frame->in_key, frame->prefix,
										  frame->parts.ptr, frame->parts.len);
}

/*
 * access_index_IndexKey_new returns how an index's entries are made from
 * its key columns (a Vec<(Order, KeyType, Column)>), its INCLUDE columns (a
 * Vec<Column>), both of which it takes, whether it is unique, whether its
 * table is keyless, the most its table's row key takes, and the most each of
 * its value's other parts takes. A unique index's value holds the row key
 * too, so its bound counts both.
 */
void
access_index_IndexKey_new(StdVec *columns, StdVec *includes, AccessIndexUnique unique, bool keyless, size_t row_key,
						  const size_t *parts, size_t parts_len, AccessIndexIndexKey *result)
{
	access_index_new_frame_t frame = {.includes = includes};
	bool		compared = false;

	frame.in_key = unique == ACCESS_INDEX_UNIQUE_NULLS_NOT_DISTINCT ? 0 : row_key;
	std_slice_to_vec_Checked(&frame.parts, parts, parts_len, ACCESS_INDEX_USIZE);
	if (unique != ACCESS_INDEX_UNIQUE_NO)
		std_vec_push_Checked(&frame.parts, &row_key, ACCESS_INDEX_USIZE);
	std_vec_with_capacity_Checked(&frame.columns, columns->len, ACCESS_INDEX_KEYED);
	std_vec_with_capacity_Checked(&frame.originals, columns->len, ACCESS_INDEX_COLUMN);
	for (size_t i = 0; i < columns->len; i++)
	{
		access_TupleOrderKeyTypeColumn_t *made = &STD_VEC_AT(columns, access_TupleOrderKeyTypeColumn_t, i);
		access_TupleOrderKeyType_t keyed = {made->f0, made->f1};

		std_vec_push_Checked(&frame.columns, &keyed, ACCESS_INDEX_KEYED);
		std_vec_push_Checked(&frame.originals, &made->f2, ACCESS_INDEX_COLUMN);
	}
	std_vec_free_Checked(columns, ACCESS_INDEX_MADE);
	for (size_t i = 0; i < frame.columns.len && !compared; i++)
		compared = STD_VEC_AT(&frame.columns, access_TupleOrderKeyType_t, i).f1.inner.tag == ACCESS_KEY_INNER_COMPARED;
	frame.prefix = keyless ? ACCESS_HEAPTUPLE_MAX_COUNT_LEN : 0;
	frame.live = true;
	std_landing(&frame, access_index_IndexKey_new_body, access_index_IndexKey_new_pad);
	result->columns = frame.columns;
	result->bound = frame.bound;
	result->compared = compared;
	result->unique = unique;
	result->originals = frame.originals;
	result->includes = *includes;
	result->keyless = keyless;
	std_vec_free_Checked(&frame.parts, ACCESS_INDEX_USIZE);
}

/* access_index_IndexKey_drop frees what an index's key holds. */
void
access_index_IndexKey_drop(AccessIndexIndexKey *self)
{
	std_vec_drop_Checked(&self->columns, ACCESS_INDEX_KEYED, access_index_keyed_drop);
	std_vec_free_Checked(&self->originals, ACCESS_INDEX_COLUMN);
	std_vec_free_Checked(&self->includes, ACCESS_INDEX_COLUMN);
}

/* access_index_IndexKey_bound returns the bound the index's key columns are cut at. */
size_t
access_index_IndexKey_bound(const AccessIndexIndexKey *self)
{
	return self->bound;
}

/*
 * access_index_IndexKey_key_columns returns each key column's order, key type,
 * and how its original is stored, in the index's order.
 */
AccessIndexKeyColumns
access_index_IndexKey_key_columns(const AccessIndexIndexKey *self)
{
	AccessIndexKeyColumns columns = {self};

	return columns;
}

/* access_index_KeyColumns_Iterator_next gives the next key column, or answers false past the last. */
bool
access_index_KeyColumns_Iterator_next(AccessIndexKeyColumns *self, access_TupleOrderKeyTypeColumnRef_t *result)
{
	const access_TupleOrderKeyType_t *keyed;

	if (self->next >= Min(self->index->columns.len, self->index->originals.len))
		return false;
	keyed = &STD_VEC_AT(&self->index->columns, access_TupleOrderKeyType_t, self->next);
	result->f0 = keyed->f0;
	result->f1 = &keyed->f1;
	result->f2 = &STD_VEC_AT(&self->index->originals, AccessHeaptupleColumn, self->next);
	self->next++;
	return true;
}

/*
 * access_index_encode_frame_t is encode's state across its pad: the key it
 * writes and which columns were cut, in its held slots 0 and 1.
 */
typedef struct access_index_encode_frame_t
{
	access_key_held_t held;
	const AccessIndexIndexKey *self;
	AccessKeySortKeys sorter;
	AccessKeyNames names;
	const access_OptionValue_t *values;
	size_t		values_len;
	StdBytes	row_key;
	PgPgError  *error;
	bool		ok;
} access_index_encode_frame_t;

/*
 * access_index_IndexKey_encode_body writes each key column's key cut at the
 * bound, then the row's key where it may repeat.
 */
static void
access_index_IndexKey_encode_body(void *frame_)
{
	access_index_encode_frame_t *frame = frame_;
	const AccessIndexIndexKey *self = frame->self;
	bool		null = false;
	bool		repeats = false;

	for (size_t i = 0; i < Min(self->columns.len, frame->values_len); i++)
	{
		const access_TupleOrderKeyType_t *keyed = &STD_VEC_AT(&self->columns, access_TupleOrderKeyType_t, i);
		const access_OptionValue_t *value = &frame->values[i];
		bool		cut;

		null |= !value->has;
		if (!access_key_encode_within(&frame->held.vec[0], frame->sorter, frame->names, &keyed->f1, value->has,
									  value->value, keyed->f0, self->bound, self->compared, &cut, frame->error))
			return;
		std_vec_push_Checked(&frame->held.vec[1], &cut, ACCESS_INDEX_BOOL);
	}
	switch (self->unique)
	{
		case ACCESS_INDEX_UNIQUE_NO:
			repeats = true;
			break;
		case ACCESS_INDEX_UNIQUE_NULLS_DISTINCT:
			repeats = null;
			break;
		case ACCESS_INDEX_UNIQUE_NULLS_NOT_DISTINCT:
			repeats = false;
			break;
	}
	if (repeats)
		std_vec_extend_from_slice_Checked(&frame->held.vec[0], frame->row_key.ptr, frame->row_key.len,
										  ACCESS_INDEX_BYTES);
	frame->ok = true;
}

/*
 * access_index_IndexKey_encode returns the key a row's values give the
 * index: each key column's key cut at the bound, then the row's key, as
 * _bt_mkscankey and _bt_doinsert key heap's entries, unless the index is
 * unique and no value of its key is a NULL that may repeat. With NULLs not
 * distinct a NULL is equal to a NULL, so the row's key never ends a unique
 * index's key.
 */
bool
access_index_IndexKey_encode(const AccessIndexIndexKey *self, AccessKeySortKeys sorter, AccessKeyNames names,
							 const access_OptionValue_t *values, size_t values_len, const uint8 *row_key,
							 size_t row_key_len, AccessIndexEntryKey *result, PgPgError *error)
{
	access_index_encode_frame_t frame = {.self = self, .sorter = sorter, .names = names, .values = values,
	.values_len = values_len, .row_key = {row_key, row_key_len}, .error = error};

	access_key_held_new(&frame.held, 0, ACCESS_INDEX_BYTES);
	access_key_held_new(&frame.held, 1, ACCESS_INDEX_BOOL);
	std_landing(&frame, access_index_IndexKey_encode_body, access_key_held_pad);
	if (frame.ok)
	{
		result->key = access_key_held_take(&frame.held, 0);
		result->cut = access_key_held_take(&frame.held, 1);
	}
	access_key_held_pad(&frame);
	return frame.ok;
}

/* access_index_EntryKey_drop frees an entry's key and its cut columns. */
void
access_index_EntryKey_drop(AccessIndexEntryKey *self)
{
	std_vec_free_Checked(&self->key, ACCESS_INDEX_BYTES);
	std_vec_free_Checked(&self->cut, ACCESS_INDEX_BOOL);
}

/*
 * access_index_value_frame_t is value's state across its pad: its fields'
 * lengths, storage kinds, forced moves, and which are out of line, in its
 * held slots 0 to 3; its fields; its plan; and its key's copy.
 */
typedef struct access_index_value_frame_t
{
	access_key_held_t held;
	StdVec		fields;
	bool		fields_live;
	AccessHeaptuplePlan plan;
	bool		plan_live;
	StdVec		key_copy;
	bool		key_live;
	const AccessIndexIndexKey *self;
	const AccessIndexEntryKey *key;
	StdBytes	row_key;
	const AccessHeaptupleSet *originals;
	const AccessHeaptupleSet *includes;
	AccessHeaptuplePlacing *result;
	PgPgError  *error;
	bool		ok;
} access_index_value_frame_t;

/* access_index_IndexKey_value_pad frees what value holds. */
static void
access_index_IndexKey_value_pad(void *frame_)
{
	access_index_value_frame_t *frame = frame_;

	access_key_held_pad(frame);
	if (frame->fields_live)
		std_vec_drop_Checked(&frame->fields, ACCESS_INDEX_FIELD, access_heaptuple_field_drop);
	if (frame->plan_live)
		access_heaptuple_Plan_drop(&frame->plan);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key_copy, ACCESS_INDEX_BYTES);
	frame->fields_live = frame->plan_live = frame->key_live = false;
}

/* access_index_IndexKey_value_push adds a field of a type's length and storage kind to value's fields. */
static void
access_index_IndexKey_value_push(access_index_value_frame_t *frame, const access_OptionCowBytes_t *field, int16 typlen,
								 AccessHeaptupleStorage storage)
{
	std_vec_push_Checked(&frame->fields, field, ACCESS_INDEX_FIELD);
	std_vec_push_Checked(&frame->held.vec[0], &typlen, ACCESS_INDEX_I16);
	std_vec_push_Checked(&frame->held.vec[1], &storage, ACCESS_INDEX_STORAGE);
}

/* access_index_IndexKey_value_force names the field value adds next as one that moves out of line first. */
static void
access_index_IndexKey_value_force(access_index_value_frame_t *frame)
{
	std_vec_push_Checked(&frame->held.vec[2], &frame->fields.len, ACCESS_INDEX_USIZE);
}

/* access_index_IndexKey_value_body gathers the value's fields, plans their placing, and makes it. */
static void
access_index_IndexKey_value_body(void *frame_)
{
	access_index_value_frame_t *frame = frame_;
	const AccessIndexIndexKey *self = frame->self;
	const AccessIndexEntryKey *key = frame->key;
	AccessHeaptupleKind kinds[1];
	size_t		count;
	size_t		sum;
	size_t		room;
	bool		no = false;

	if (self->unique != ACCESS_INDEX_UNIQUE_NO)
	{
		access_OptionCowBytes_t field = {.has = true, .borrowed = frame->row_key};

		access_index_IndexKey_value_push(frame, &field, -1, ACCESS_HEAPTUPLE_STORAGE_PLAIN);
	}
	for (size_t i = 0; i < self->includes.len; i++)
	{
		const AccessHeaptupleColumn *column = &STD_VEC_AT(&self->includes, AccessHeaptupleColumn, i);
		access_OptionCowBytes_t field;

		if (!access_heaptuple_field_of(column, &frame->includes[i], &field, frame->error))
			return;
		access_index_IndexKey_value_push(frame, &field, column->len, column->storage);
	}
	for (size_t i = 0; i < Min(self->columns.len, self->originals.len); i++)
	{
		const AccessKeyKeyType *key_type = &STD_VEC_AT(&self->columns, access_TupleOrderKeyType_t, i).f1;
		const AccessHeaptupleColumn *column = &STD_VEC_AT(&self->originals, AccessHeaptupleColumn, i);
		access_OptionCowBytes_t field;

		if (access_key_is_exact(key_type))
			continue;
		std_index_check(i, key->cut.len, RUST_FILE, 192, 16);
		if (STD_VEC_AT(&key->cut, bool, i) && self->compared)
			access_index_IndexKey_value_force(frame);
		std_index_check(i, self->columns.len, RUST_FILE, 195, 54);
		if (!access_heaptuple_field_of(column, &frame->originals[i], &field, frame->error))
			return;
		access_index_IndexKey_value_push(frame, &field, column->len, column->storage);
	}
	for (size_t i = 0; i < Min(self->columns.len, self->originals.len); i++)
	{
		const AccessKeyKeyType *key_type = &STD_VEC_AT(&self->columns, access_TupleOrderKeyType_t, i).f1;
		const AccessHeaptupleColumn *column = &STD_VEC_AT(&self->originals, AccessHeaptupleColumn, i);
		access_OptionCowBytes_t field = {0};

		if (!access_key_is_exact(key_type) || !access_key_can_be_cut(key_type, self->bound))
			continue;
		std_index_check(i, key->cut.len, RUST_FILE, 203, 16);
		if (STD_VEC_AT(&key->cut, bool, i))
			access_index_IndexKey_value_force(frame);
		std_index_check(i, key->cut.len, RUST_FILE, 206, 31);
		if (STD_VEC_AT(&key->cut, bool, i))
		{
			std_index_check(i, self->columns.len, RUST_FILE, 207, 54);
			if (!access_heaptuple_field_of(column, &frame->originals[i], &field, frame->error))
				return;
		}
		access_index_IndexKey_value_push(frame, &field, column->len, ACCESS_HEAPTUPLE_STORAGE_PLAIN);
	}
	count = self->keyless ? ACCESS_HEAPTUPLE_MAX_COUNT_LEN : 0;
	sum = STD_ADD(usize, key->key.len, count, 214, 53);
	sum = STD_ADD(usize, sum, ACCESS_HEAPTUPLE_ENTRY_OVERHEAD, 214, 53);
	room = std_saturating_sub_usize(CHUNKS_MAX_ENTRY, sum);
	kinds[0].movable = ACCESS_INDEX_MOVABLE;
	kinds[0].movable_len = lengthof(ACCESS_INDEX_MOVABLE);
	kinds[0].target = room;
	std_vec_resize(&frame->held.vec[3], frame->fields.len, &no, ACCESS_INDEX_BOOL);
	std_vec_clone(&frame->plan.external, &frame->held.vec[3], ACCESS_INDEX_BOOL);
	std_vec_new(&frame->plan.lengths, ACCESS_INDEX_LENGTH);
	std_vec_new(&frame->plan.moves, ACCESS_INDEX_USIZE);
	std_vec_new(&frame->plan.levels, ACCESS_INDEX_LEVEL);
	frame->plan_live = true;
	access_heaptuple_Plan_new(frame->fields.ptr, frame->fields.len, frame->held.vec[0].ptr, frame->held.vec[0].len,
							  frame->held.vec[1].ptr, frame->held.vec[1].len, frame->held.vec[2].ptr,
							  frame->held.vec[2].len, room, kinds, lengthof(kinds), room, &frame->plan);
	std_vec_clone(&frame->key_copy, &key->key, ACCESS_INDEX_BYTES);
	frame->key_live = true;
	frame->ok = access_heaptuple_Placing_new(&frame->key_copy, false, &frame->fields, &frame->held.vec[3], &frame->plan,
											 0, frame->result, frame->error);
}

/*
 * access_index_IndexKey_value returns the placing of the value of a row's
 * entry, Doltgrust's entry_with_copies in the layout of the btree text's
 * section 3: a tuple of the row's key for a unique index, each INCLUDE
 * column's value, each lossy key column's original, and for each exact key
 * column that can be cut a field that is NULL where the key holds the value
 * whole and otherwise its original, which always moves out of line. The
 * other parts move out of line largest first while the entry passes
 * MAX_ENTRY, then the trailing fields go into overflow chunks.
 */
bool
access_index_IndexKey_value(const AccessIndexIndexKey *self, const AccessIndexEntryKey *key, const uint8 *row_key,
							size_t row_key_len, const AccessHeaptupleSet *originals, size_t originals_len,
							const AccessHeaptupleSet *includes, size_t includes_len, AccessHeaptuplePlacing *result,
							PgPgError *error)
{
	access_index_value_frame_t frame = {.self = self, .key = key, .row_key = {row_key, row_key_len},
	.originals = originals, .includes = includes, .result = result, .error = error};

	if (originals_len != self->columns.len || includes_len != self->includes.len || key->cut.len != originals_len)
		return access_index_fail("XX000", "an index entry's values don't fit its index's columns", error);
	std_vec_new(&frame.fields, ACCESS_INDEX_FIELD);
	frame.fields_live = true;
	access_key_held_new(&frame.held, 0, ACCESS_INDEX_I16);
	access_key_held_new(&frame.held, 1, ACCESS_INDEX_STORAGE);
	access_key_held_new(&frame.held, 2, ACCESS_INDEX_USIZE);
	access_key_held_new(&frame.held, 3, ACCESS_INDEX_BOOL);
	std_landing(&frame, access_index_IndexKey_value_body, access_index_IndexKey_value_pad);
	access_index_IndexKey_value_pad(&frame);
	return frame.ok;
}

/*
 * access_index_IndexKey_row_key returns the row key a unique index's entry
 * value holds as its first field, which the unique check reads to name the
 * row an existing entry is for; a value whose first field is no row key is
 * data corruption.
 */
bool
access_index_IndexKey_row_key(const uint8 *value, size_t value_len, StdBytes *result, PgPgError *error)
{
	EncodingTupleTuple tuple;

	if (encoding_tuple_Tuple_parse(value, value_len, &tuple))
	{
		EncodingTupleField field = encoding_tuple_Tuple_field(&tuple, 0);

		if (field.tag == ENCODING_TUPLE_FIELD_VALUE)
		{
			*result = field.u.value;
			return true;
		}
	}
	return access_index_fail("XX001", "a unique index's entry value holds no row key", error);
}

/* access_index_decoded_drop frees a decoded value's bytes, as a vector's element. */
void
access_index_decoded_drop(void *value_)
{
	access_OptionDecoded_t *value = value_;

	if (value->has && value->value.tag == ACCESS_KEY_DECODED_BYTES)
		std_vec_free_Checked(&value->value.u.bytes, ACCESS_INDEX_BYTES);
}

/* access_index_decode_frame_t is decode's state across its pad: the values it gives (a Vec<Option<Decoded>>). */
typedef struct access_index_decode_frame_t
{
	StdVec		values;
	bool		live;
	const AccessIndexIndexKey *self;
	StdBytes	key;
	bool		includes;
	const access_OptionVecU8_t *fields;
	size_t		fields_len;
	PgPgError  *error;
	bool		ok;
} access_index_decode_frame_t;

/* access_index_IndexKey_decode_pad frees the values decode gives. */
static void
access_index_IndexKey_decode_pad(void *frame_)
{
	access_index_decode_frame_t *frame = frame_;

	if (frame->live)
		std_vec_drop_Checked(&frame->values, ACCESS_INDEX_DECODED, access_index_decoded_drop);
	frame->live = false;
}

/*
 * access_index_IndexKey_decode_closure_0 is decode's original: the value of a
 * column a field of the entry's value holds, or none.
 */
static bool
access_index_IndexKey_decode_closure_0(const access_index_decode_frame_t *frame, size_t at,
									   const AccessHeaptupleColumn *column, access_OptionDecoded_t *result,
									   PgPgError *error)
{
	const access_OptionVecU8_t *field = at < frame->fields_len ? &frame->fields[at] : NULL;

	result->has = false;
	if (field == NULL || !field->has)
		return true;
	if (column->by_value)
	{
		result->value.tag = ACCESS_KEY_DECODED_WORD;
		if (!access_heaptuple_word_of(field->bytes.ptr, field->bytes.len, &result->value.u.word, error))
			return false;
	}
	else
	{
		result->value.tag = ACCESS_KEY_DECODED_BYTES;
		std_slice_to_vec_Checked(&result->value.u.bytes, field->bytes.ptr, field->bytes.len, ACCESS_INDEX_BYTES);
	}
	result->has = true;
	return true;
}

/* access_index_IndexKey_decode_body decodes each key column, then each INCLUDE column when asked. */
static void
access_index_IndexKey_decode_body(void *frame_)
{
	access_index_decode_frame_t *frame = frame_;
	const AccessIndexIndexKey *self = frame->self;
	size_t		lossy = 0;
	size_t		lossy_at;
	size_t		cut_at;
	StdBytes	rest = frame->key;

	for (size_t i = 0; i < self->columns.len; i++)
		lossy = std_add_usize(lossy, (size_t) !access_key_is_exact(&STD_VEC_AT(&self->columns,
																			   access_TupleOrderKeyType_t, i).f1),
							  STD_ACCUM_FILE, 206, 1);
	lossy_at = STD_ADD(usize, (size_t) (self->unique != ACCESS_INDEX_UNIQUE_NO), self->includes.len,
					   242, 28);
	cut_at = STD_ADD(usize, lossy_at, lossy, 243, 26);
	for (size_t i = 0; i < Min(self->columns.len, self->originals.len); i++)
	{
		const access_TupleOrderKeyType_t *keyed = &STD_VEC_AT(&self->columns, access_TupleOrderKeyType_t, i);
		const AccessHeaptupleColumn *column = &STD_VEC_AT(&self->originals, AccessHeaptupleColumn, i);
		access_OptionDecoded_t value = {0};
		StdBytes	after;
		bool		cut;

		if (!access_key_skip_within(rest.ptr, rest.len, &keyed->f1, keyed->f0, self->bound, &after, &cut,
									frame->error))
			return;
		if (access_key_is_exact(&keyed->f1) && !cut)
		{
			StdBytes	unused;

			if (!access_key_decode(rest.ptr, rest.len, &keyed->f1, keyed->f0, &value.has, &value.value, &unused,
								   frame->error))
				return;
		}
		else if (access_key_is_exact(&keyed->f1))
		{
			if (!access_index_IndexKey_decode_closure_0(frame, cut_at, column, &value, frame->error))
				return;
			if (!value.has)
			{
				access_index_fail("XX001", "an index entry's value keeps no original of its cut key", frame->error);
				return;
			}
		}
		else
		{
			lossy_at = STD_ADD(usize, lossy_at, 1, 262, 21);
			if (!access_index_IndexKey_decode_closure_0(frame, STD_SUB(usize, lossy_at, 1, 263, 30),
														column, &value, frame->error))
				return;
		}
		if (access_key_is_exact(&keyed->f1) && access_key_can_be_cut(&keyed->f1, self->bound))
			cut_at = STD_ADD(usize, cut_at, 1, 267, 17);
		std_vec_push_Checked(&frame->values, &value, ACCESS_INDEX_DECODED);
		rest = after;
	}
	if (frame->includes)
	{
		size_t		at = (size_t) (self->unique != ACCESS_INDEX_UNIQUE_NO);

		for (size_t k = 0;; k++)
		{
			size_t		taken = at;
			access_OptionDecoded_t value;

			at = std_forward_usize_Checked(at, 1);
			if (k >= self->includes.len)
				break;
			if (!access_index_IndexKey_decode_closure_0(frame, taken,
														&STD_VEC_AT(&self->includes, AccessHeaptupleColumn, k),
														&value, frame->error))
				return;
			std_vec_push_Checked(&frame->values, &value, ACCESS_INDEX_DECODED);
		}
	}
	frame->ok = true;
}

/*
 * access_index_IndexKey_decode returns the values of an entry's key columns,
 * and then of its INCLUDE columns when includes asks, as Doltgrust's
 * values_into gives them (a Vec<Option<Decoded>>): an exact key column's
 * from the entry's key where the key holds it whole, and any other column's
 * from its value's fields, or none for NULL. A cut column whose value keeps
 * no original is data corruption.
 */
bool
access_index_IndexKey_decode(const AccessIndexIndexKey *self, const uint8 *key, size_t key_len, bool includes,
							 const access_OptionVecU8_t *fields, size_t fields_len, StdVec *result, PgPgError *error)
{
	access_index_decode_frame_t frame = {.self = self, .key = {key, key_len}, .includes = includes, .fields = fields,
	.fields_len = fields_len, .error = error};

	std_vec_with_capacity_Checked(&frame.values, self->columns.len, ACCESS_INDEX_DECODED);
	frame.live = true;
	std_landing(&frame, access_index_IndexKey_decode_body, access_index_IndexKey_decode_pad);
	if (frame.ok)
	{
		*result = frame.values;
		frame.live = false;
	}
	access_index_IndexKey_decode_pad(&frame);
	return frame.ok;
}

/*
 * access_index_IndexKey_table_bound returns the bound the tree of a table
 * keyed by the index's key columns cuts them at.
 */
size_t
access_index_IndexKey_table_bound(const AccessIndexIndexKey *self)
{
	return access_key_table_bound(access_index_key_types(&self->columns));
}

/*
 * access_index_IndexKey_rest returns a key's bytes after its key columns, cut
 * at the index's bound: the row's key where the key holds it.
 */
bool
access_index_IndexKey_rest(const AccessIndexIndexKey *self, const uint8 *key, size_t key_len, StdBytes *result,
						   PgPgError *error)
{
	StdBytes	rest = {key, key_len};

	for (size_t i = 0; i < self->columns.len; i++)
	{
		const access_TupleOrderKeyType_t *keyed = &STD_VEC_AT(&self->columns, access_TupleOrderKeyType_t, i);
		bool		cut;

		if (!access_key_skip_within(rest.ptr, rest.len, &keyed->f1, keyed->f0, self->bound, &rest, &cut, error))
			return false;
	}
	*result = rest;
	return true;
}

/*
 * access_index_IndexKey_run_prefix returns how much of a key a run of cut
 * keys shares, for a key of a tree that cuts the key columns at a bound: the
 * key through its first cut column's first bytes up to the bound; or none
 * for a key with no cut column, or a comparator tree's, whose cut columns
 * are out of line.
 */
bool
access_index_IndexKey_run_prefix(const AccessIndexIndexKey *self, const uint8 *key, size_t key_len, size_t bound,
								 bool *has_result, size_t *result, PgPgError *error)
{
	StdBytes	rest = {key, key_len};

	*has_result = false;
	for (size_t i = 0; i < self->columns.len; i++)
	{
		const access_TupleOrderKeyType_t *keyed = &STD_VEC_AT(&self->columns, access_TupleOrderKeyType_t, i);
		StdBytes	after;
		bool		cut;

		if (!access_key_skip_within(rest.ptr, rest.len, &keyed->f1, keyed->f0, bound, &after, &cut, error))
			return false;
		if (cut && rest.len > 0 && rest.ptr[0] == ENCODING_KEY_OUT_OF_LINE)
			return true;
		if (cut)
		{
			*has_result = true;
			*result = STD_ADD(usize, STD_SUB(usize, key_len, rest.len, 309, 32), bound,
							  309, 32);
			return true;
		}
		rest = after;
	}
	return true;
}

/* access_index_whole_frame_t is whole's state across its pad: the bytes it writes, in its held slot 0. */
typedef struct access_index_whole_frame_t
{
	access_key_held_t held;
	const AccessIndexIndexKey *self;
	AccessKeySortKeys sorter;
	const access_OptionValue_t *values;
	size_t		values_len;
	StdBytes	key;
	size_t		bound;
	PgPgError  *error;
	bool		ok;
} access_index_whole_frame_t;

/*
 * access_index_IndexKey_whole_body writes each key column's whole encoding,
 * then the rest of the key past its columns.
 */
static void
access_index_IndexKey_whole_body(void *frame_)
{
	access_index_whole_frame_t *frame = frame_;
	StdBytes	rest = frame->key;

	for (size_t i = 0; i < Min(frame->self->columns.len, frame->values_len); i++)
	{
		const access_TupleOrderKeyType_t *keyed = &STD_VEC_AT(&frame->self->columns, access_TupleOrderKeyType_t, i);
		bool		cut;

		if (!access_key_encode(&frame->held.vec[0], frame->sorter, &keyed->f1, frame->values[i].has,
							   frame->values[i].value, keyed->f0, frame->error))
			return;
		if (!access_key_skip_within(rest.ptr, rest.len, &keyed->f1, keyed->f0, frame->bound, &rest, &cut,
									frame->error))
			return;
	}
	std_vec_extend_from_slice_Checked(&frame->held.vec[0], rest.ptr, rest.len, ACCESS_INDEX_BYTES);
	frame->ok = true;
}

/*
 * access_index_IndexKey_whole returns the bytes a key of a tree that cuts
 * the key columns at a bound orders by in its columns' true order: each key
 * column's whole encoding of its value, as no bound cuts it, then the rest
 * of the key past its columns.
 */
bool
access_index_IndexKey_whole(const AccessIndexIndexKey *self, AccessKeySortKeys sorter,
							const access_OptionValue_t *values, size_t values_len, const uint8 *key, size_t key_len,
							size_t bound, StdVec *result, PgPgError *error)
{
	access_index_whole_frame_t frame = {.self = self, .sorter = sorter, .values = values, .values_len = values_len,
	.key = {key, key_len}, .bound = bound, .error = error};

	access_key_held_new(&frame.held, 0, ACCESS_INDEX_BYTES);
	std_landing(&frame, access_index_IndexKey_whole_body, access_key_held_pad);
	if (frame.ok)
		*result = access_key_held_take(&frame.held, 0);
	access_key_held_pad(&frame);
	return frame.ok;
}

/*
 * access_index_IndexKey_row_key_bytes returns the row key an entry names, as
 * Doltgrust's row_key_bytes reads it: the first field of a unique index's
 * value, or what follows its key columns in its key.
 */
bool
access_index_IndexKey_row_key_bytes(const AccessIndexIndexKey *self, const uint8 *key, size_t key_len,
									const uint8 *value, size_t value_len, StdBytes *result, PgPgError *error)
{
	if (self->unique != ACCESS_INDEX_UNIQUE_NO)
		return access_index_IndexKey_row_key(value, value_len, result, error);
	return access_index_IndexKey_rest(self, key, key_len, result, error);
}

/*
 * access_index_IndexKey_entry returns a row's entry from its value's placing
 * once formed, taking its vectors, its value the tuple; a keyless table's
 * caller counts the row's copies before it.
 */
AccessIndexEntry
access_index_IndexKey_entry(AccessHeaptupleFormed *formed)
{
	AccessIndexEntry entry = {formed->key, formed->tuple, formed->addresses};

	return entry;
}

/* access_index_Entry_drop frees an entry's key, value, and addresses. */
void
access_index_Entry_drop(AccessIndexEntry *self)
{
	std_vec_free_Checked(&self->key, ACCESS_INDEX_BYTES);
	std_vec_free_Checked(&self->value, ACCESS_INDEX_BYTES);
	std_vec_free_Checked(&self->addresses, ACCESS_INDEX_ADDRESS);
}

#ifdef DOLTGRES_TEST_ACCESS

/* ACCESS_INDEX_TESTS_REVERSED and ACCESS_INDEX_TESTS_NAMES are key's tests' Reversed as a SortKeys and as a Names. */
#define ACCESS_INDEX_TESTS_REVERSED ((AccessKeySortKeys) {&ACCESS_KEY_REVERSED_SORT_KEYS_OPS, NULL})
#define ACCESS_INDEX_TESTS_NAMES ((AccessKeyNames) {&ACCESS_KEY_REVERSED_NAMES_OPS, NULL})

/* ACCESS_INDEX_BLOB is the element layout of a Vec<Vec<u8>> of blobs. */
#define ACCESS_INDEX_BLOB sizeof(StdVec), _Alignof(StdVec)

/* ACCESS_INDEX_TESTS_LINE declares a message of a corpus line, the "{line}" of an assert. */
#define ACCESS_INDEX_TESTS_LINE(name, line) \
	StdFmtPiece name##_pieces[] = {STD_FMT_ARG(0)}; \
	StdFmtArg	name##_args[] = {{&(line), std_str_Display_fmt}}; \
	StdFmtArguments name = {name##_pieces, 1, name##_args}

/* access_index_tests_message_Debug_fmt is an error's message's Debug, an Option<&str>. */
static bool
access_index_tests_message_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdStr *message = value;
	StdOptionRef option = {message->ptr != NULL, {message, std_str_Debug_fmt}};

	return std_option_Debug_fmt(&option, f);
}

/* access_index_tests_ok is tests' ok: nothing when the call succeeded, or a panic with the error's message. */
static void
access_index_tests_ok(bool ok, PgPgError *error)
{
	if (!ok)
	{
		StdStr		message = pg_PgError_message(error);
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&message, access_index_tests_message_Debug_fmt}};
		StdFmtArguments arguments = {pieces, 1, args};

		std_panic_fmt(RUST_FILE, 372, 39, &arguments);
	}
}

/*
 * access_index_tests_stored is tests' stored: how a column of a type is
 * stored, its length, whether it is passed by value, and its storage kind.
 */
static AccessHeaptupleColumn
access_index_tests_stored(uint32 typid, int16 len, bool by_value, AccessHeaptupleStorage storage)
{
	AccessHeaptupleColumn column = {.typid = typid, .len = len, .by_value = by_value, .storage = storage};

	column.align = len == -1 ? 4 : len >= 0 ? (size_t) len : 1;
	return column;
}

/* access_index_tests_text is tests' text: a text key column in ascending order. */
static access_TupleOrderKeyTypeColumn_t
access_index_tests_text(void)
{
	access_TupleOrderKeyTypeColumn_t text_column = {ENCODING_KEY_ORDER_ASC, access_key_KeyType_of(TEXTOID),
	access_index_tests_stored(TEXTOID, -1, false, ACCESS_HEAPTUPLE_STORAGE_EXTENDED)};

	return text_column;
}

/* access_index_tests_index returns an index's key made from key columns and INCLUDE columns it copies into vectors. */
static AccessIndexIndexKey
access_index_tests_index(const access_TupleOrderKeyTypeColumn_t *columns, size_t columns_len,
						 const AccessHeaptupleColumn *includes, size_t includes_len, AccessIndexUnique unique,
						 bool keyless, size_t row_key, const size_t *parts, size_t parts_len)
{
	StdVec		column_vec;
	StdVec		include_vec;
	AccessIndexIndexKey index;

	std_slice_to_vec_Checked(&column_vec, columns, columns_len, ACCESS_INDEX_MADE);
	std_slice_to_vec_Checked(&include_vec, includes, includes_len, ACCESS_INDEX_COLUMN);
	access_index_IndexKey_new(&column_vec, &include_vec, unique, keyless, row_key, parts, parts_len, &index);
	return index;
}

/* access_index_tests_texts returns n text key columns, as tests make them. */
static StdVec
access_index_tests_texts(size_t n)
{
	StdVec		columns;

	std_vec_with_capacity_Checked(&columns, n, ACCESS_INDEX_MADE);
	for (size_t i = 0; i < n; i++)
	{
		access_TupleOrderKeyTypeColumn_t column = access_index_tests_text();

		std_vec_push_Checked(&columns, &column, ACCESS_INDEX_MADE);
	}
	return columns;
}

/* access_index_tests_bytes returns a value of some bytes. */
static access_OptionValue_t
access_index_tests_bytes(const void *bytes, size_t len)
{
	access_OptionValue_t value = {true, {.tag = ACCESS_KEY_VALUE_BYTES, .u.bytes = {bytes, len}}};

	return value;
}

/* access_index_tests_word returns a value of a word. */
static access_OptionValue_t
access_index_tests_word(uint64 word)
{
	access_OptionValue_t value = {true, {.tag = ACCESS_KEY_VALUE_WORD, .u.word = word}};

	return value;
}

/* access_index_tests_encode is encode under key's tests' Reversed, where the call must succeed. */
static AccessIndexEntryKey
access_index_tests_encode(const AccessIndexIndexKey *index, const access_OptionValue_t *values, size_t len,
						  const uint8 *row_key, size_t row_key_len)
{
	AccessIndexEntryKey key;
	PgPgError	error;

	access_index_tests_ok(access_index_IndexKey_encode(index, ACCESS_INDEX_TESTS_REVERSED, ACCESS_INDEX_TESTS_NAMES,
													   values, len, row_key, row_key_len, &key, &error), &error);
	return key;
}

/* access_index_tests_ends_with reports whether a vector ends with some bytes. */
static bool
access_index_tests_ends_with(const StdVec *vec, const uint8 *bytes, size_t len)
{
	return vec->len >= len && memcmp((const uint8 *) vec->ptr + (vec->len - len), bytes, len) == 0;
}

/* access_index_tests_vec_eq reports whether a vector holds some bytes. */
static bool
access_index_tests_vec_eq(const StdVec *vec, const void *bytes, size_t len)
{
	return vec->len == len && memcmp(vec->ptr, bytes, len) == 0;
}

/* access_index_tests_cut_is reports whether an entry's key cut its one column as given. */
static bool
access_index_tests_cut_is(const AccessIndexEntryKey *key, bool cut)
{
	return key->cut.len == 1 && STD_VEC_AT(&key->cut, bool, 0) == cut;
}

/* access_index_tests_set returns a value of a kind and some bytes as forming takes it. */
static AccessHeaptupleSet
access_index_tests_set(AccessHeaptupleSetTag tag, const void *bytes, size_t len)
{
	AccessHeaptupleSet set = {.tag = tag};

	set.u.varlena.ptr = bytes;
	set.u.varlena.len = len;
	return set;
}

/* access_index_tests_set_word returns a word as forming takes it. */
static AccessHeaptupleSet
access_index_tests_set_word(uint64 word)
{
	AccessHeaptupleSet set = {.tag = ACCESS_HEAPTUPLE_SET_WORD, .u.word = word};

	return set;
}

/* access_index_tests_value is value where the call must succeed. */
static AccessHeaptuplePlacing
access_index_tests_value(const AccessIndexIndexKey *index, const AccessIndexEntryKey *key, const uint8 *row_key,
						 size_t row_key_len, const AccessHeaptupleSet *originals, size_t originals_len,
						 const AccessHeaptupleSet *includes, size_t includes_len)
{
	AccessHeaptuplePlacing placing;
	PgPgError	error;

	access_index_tests_ok(access_index_IndexKey_value(index, key, row_key, row_key_len, originals, originals_len,
													  includes, includes_len, &placing, &error), &error);
	return placing;
}

/*
 * access_index_tests_formed_t is formed's answer: the entry and its blobs'
 * bytes in the order asked for (a Vec<Vec<u8>>).
 */
typedef struct access_index_tests_formed_t
{
	AccessIndexEntry entry;
	StdVec		blobs;
} access_index_tests_formed_t;

/* access_index_tests_blob_drop frees a blob's bytes, as a vector's element. */
static void
access_index_tests_blob_drop(void *blob)
{
	std_vec_free_Checked(blob, ACCESS_INDEX_BYTES);
}

/*
 * access_index_tests_formed is tests' formed: an entry formed from its
 * value's placing, which it takes, writing each blob it asks for under the
 * address of its bytes, with the blobs' bytes in the order they were asked
 * for.
 */
static access_index_tests_formed_t
access_index_tests_formed(AccessHeaptuplePlacing *placing)
{
	access_index_tests_formed_t made;
	AccessHeaptupleBlob blob;
	AccessHeaptupleFormed formed;
	PgPgError	error;

	std_vec_new(&made.blobs, ACCESS_INDEX_BLOB);
	while (access_heaptuple_Placing_blob(placing, &blob))
	{
		StdBytes	bytes = blob.tag == ACCESS_HEAPTUPLE_BLOB_VALUE ? blob.u.value : blob.u.chunk.node;
		StdVec		copy;
		ChunksAddressAddress address;
		ChunksTrailerLocator none = {0};

		std_slice_to_vec_Checked(&copy, bytes.ptr, bytes.len, ACCESS_INDEX_BYTES);
		address = chunks_address_Address_of(copy.ptr, copy.len);
		std_vec_push_Checked(&made.blobs, &copy, ACCESS_INDEX_BLOB);
		access_index_tests_ok(access_heaptuple_Placing_wrote(placing, address, none, &error), &error);
	}
	access_index_tests_ok(access_heaptuple_Placing_formed(placing, &formed, &error), &error);
	made.entry = access_index_IndexKey_entry(&formed);
	return made;
}

/* access_index_tests_formed_drop frees formed's entry and blobs. */
static void
access_index_tests_formed_drop(access_index_tests_formed_t *made)
{
	access_index_Entry_drop(&made->entry);
	std_vec_drop_Checked(&made->blobs, ACCESS_INDEX_BLOB, access_index_tests_blob_drop);
}

/* access_index_tests_parse is Tuple::parse(...).unwrap() at a place. */
static EncodingTupleTuple
access_index_tests_parse(const uint8 *bytes, size_t len, uint32 line, uint32 col)
{
	EncodingTupleTuple tuple;

	if (!encoding_tuple_Tuple_parse(bytes, len, &tuple))
		std_option_unwrap_failed(RUST_FILE, line, col);
	return tuple;
}

/*
 * The fork's own (design item 4's form D): an index's text column one byte
 * under, at, and over its bound is whole in the first two keys and cut in the
 * third, and the row's key ends every key of an index that isn't unique.
 */
void
test_access_index_tests_a_key_column_is_cut_at_the_indexs_bound(void)
{
	access_TupleOrderKeyTypeColumn_t columns[1] = {access_index_tests_text()};
	AccessIndexIndexKey index = access_index_tests_index(columns, 1, NULL, 0, ACCESS_INDEX_UNIQUE_NO, false, 5, NULL, 0);
	static const uint8 row_key[5] = {1, 0, 0, 0, 7};
	size_t		lens[3];
	static const bool cuts[3] = {false, false, true};

	STD_ASSERT(393, 9, access_index_IndexKey_bound(&index) == 16300, "index.bound() == 16_300");
	lens[0] = STD_SUB(usize, access_index_IndexKey_bound(&index), 3, 395, 29);
	lens[1] = STD_SUB(usize, access_index_IndexKey_bound(&index), 2, 395, 57);
	lens[2] = STD_SUB(usize, access_index_IndexKey_bound(&index), 1, 395, 85);
	for (int c = 0; c < 3; c++)
	{
		size_t		len = lens[c];
		bool		cut = cuts[c];
		StdVec		value;
		uint8		x = 'x';
		access_OptionValue_t values[1];
		AccessIndexEntryKey entry;
		size_t		column;

		std_vec_new(&value, ACCESS_INDEX_BYTES);
		std_vec_resize(&value, len, &x, ACCESS_INDEX_BYTES);
		values[0] = access_index_tests_bytes(value.ptr, value.len);
		entry = access_index_tests_encode(&index, values, 1, row_key, sizeof(row_key));
		STD_ASSERT(398, 13,
				   access_index_tests_cut_is(&entry, cut) && access_index_tests_ends_with(&entry.key, row_key, 5),
				   "entry.cut == [cut] && entry.key.ends_with(&row_key)");
		column = STD_SUB(usize, entry.key.len, sizeof(row_key), 399, 26);
		if (cut)
			STD_ASSERT(400, 13,
					   column == STD_ADD(usize, access_index_IndexKey_bound(&index), 32, 400, 40),
					   "column == if cut { index.bound() + 32 } else { len + 2 }");
		else
			STD_ASSERT(400, 13, column == STD_ADD(usize, len, 2, 400, 68),
					   "column == if cut { index.bound() + 32 } else { len + 2 }");
		access_index_EntryKey_drop(&entry);
		std_vec_free_Checked(&value, ACCESS_INDEX_BYTES);
	}
	access_index_IndexKey_drop(&index);
}

/*
 * test_access_index_tests_a_unique_index_keys_equal_values_alike_closure_0 is
 * the test's int: an int4 key column in ascending order.
 */
static access_TupleOrderKeyTypeColumn_t
test_access_index_tests_a_unique_index_keys_equal_values_alike_closure_0(void)
{
	access_TupleOrderKeyTypeColumn_t column = {ENCODING_KEY_ORDER_ASC, access_key_KeyType_of(INT4OID),
	access_index_tests_stored(INT4OID, 4, true, ACCESS_HEAPTUPLE_STORAGE_PLAIN)};

	return column;
}

/*
 * access_index_tests_ends_after encodes values where the call must succeed and
 * reports whether the key ends with the row's key.
 */
static bool
access_index_tests_ends_after(const AccessIndexIndexKey *index, const access_OptionValue_t *values, size_t len,
							  const uint8 *row_key, size_t row_key_len)
{
	AccessIndexEntryKey key = access_index_tests_encode(index, values, len, row_key, row_key_len);
	bool		ends = access_index_tests_ends_with(&key.key, row_key, row_key_len);

	access_index_EntryKey_drop(&key);
	return ends;
}

/*
 * The fork's own: a unique index's key leaves the row's key out unless a value
 * is a NULL that may repeat, and with NULLs not distinct it leaves it out
 * always, so equal values share one key.
 */
void
test_access_index_tests_a_unique_index_keys_equal_values_alike(void)
{
	static const uint8 row_key[5] = {9, 9, 9, 9, 9};
	access_TupleOrderKeyTypeColumn_t columns[2];
	AccessIndexIndexKey distinct;
	AccessIndexIndexKey not_distinct;
	access_OptionValue_t values[2];

	columns[0] = test_access_index_tests_a_unique_index_keys_equal_values_alike_closure_0();
	columns[1] = access_index_tests_text();
	distinct = access_index_tests_index(columns, 2, NULL, 0, ACCESS_INDEX_UNIQUE_NULLS_DISTINCT, false, 5, NULL, 0);
	columns[0] = test_access_index_tests_a_unique_index_keys_equal_values_alike_closure_0();
	columns[1] = access_index_tests_text();
	not_distinct = access_index_tests_index(columns, 2, NULL, 0, ACCESS_INDEX_UNIQUE_NULLS_NOT_DISTINCT, false, 5, NULL,
											0);
	values[0] = access_index_tests_word(pg_word_from_int32(1));
	values[1] = access_index_tests_bytes("a", 1);
	STD_ASSERT(413, 9, !access_index_tests_ends_after(&distinct, values, 2, row_key, 5),
			   "!ok(distinct.encode(&Reversed, &Reversed, [one(), Some(Value::Bytes(b\"a\"))], &row_key)).key.ends_with(&row_key)");
	values[1].has = false;
	STD_ASSERT(418, 9, access_index_tests_ends_after(&distinct, values, 2, row_key, 5),
			   "ok(distinct.encode(&Reversed, &Reversed, [one(), None], &row_key)).key.ends_with(&row_key)");
	STD_ASSERT(419, 9, !access_index_tests_ends_after(&not_distinct, values, 2, row_key, 5),
			   "!ok(not_distinct.encode(&Reversed, &Reversed, [one(), None], &row_key)).key.ends_with(&row_key)");
	STD_ASSERT(420, 9, access_index_IndexKey_bound(&not_distinct) > access_index_IndexKey_bound(&distinct),
			   "not_distinct.bound() > distinct.bound()");
	access_index_IndexKey_drop(&not_distinct);
	access_index_IndexKey_drop(&distinct);
}

/* access_index_tests_widest is the tests' widest: the most the key of a table keyed by 32 text columns takes. */
static size_t
access_index_tests_widest(void)
{
	AccessKeyKeyType types[32];
	AccessKeyKeyTypes key_types = {types, 32, sizeof(AccessKeyKeyType), 0};
	size_t		widest;

	for (int i = 0; i < 32; i++)
		types[i] = access_key_KeyType_of(TEXTOID);
	widest = access_key_table_key_most(key_types);
	for (int i = 0; i < 32; i++)
		access_key_KeyType_drop(&types[i]);
	return widest;
}

/* access_index_tests_new_texts returns an index's key of 32 text columns. */
static AccessIndexIndexKey
access_index_tests_new_texts(AccessIndexUnique unique, size_t row_key)
{
	StdVec		columns = access_index_tests_texts(32);
	StdVec		includes;
	AccessIndexIndexKey index;

	std_vec_new(&includes, ACCESS_INDEX_COLUMN);
	access_index_IndexKey_new(&columns, &includes, unique, false, row_key, NULL, 0, &index);
	return index;
}

/*
 * The fork's own (design item 4's form D): a unique index's value holds the
 * row's key too, so its bound leaves room for it, and the unique index's
 * largest entry fits an entry's limit.
 */
void
test_access_index_tests_a_unique_indexs_bound_counts_its_values_row_key(void)
{
	size_t		widest = access_index_tests_widest();
	AccessIndexIndexKey unique = access_index_tests_new_texts(ACCESS_INDEX_UNIQUE_NULLS_DISTINCT, widest);
	AccessIndexIndexKey other = access_index_tests_new_texts(ACCESS_INDEX_UNIQUE_NO, widest);
	StdUsizeOption fields[33];
	size_t		value;
	size_t		sum;
	bool		holds = access_index_IndexKey_bound(&other) == 183 && access_index_IndexKey_bound(&unique) == 182;

	access_index_IndexKey_drop(&other);
	STD_ASSERT(432, 9, holds,
			   "IndexKey::new(texts(), Vec::new(), Unique::No, false, widest, &[]).bound() == 183 && unique.bound() == 182");
	for (int i = 0; i < 33; i++)
		fields[i] = (StdUsizeOption)
	{
		true, 37
	};
	value = encoding_tuple_written_len(fields, 33, true, false, 0);
	sum = STD_ADD(usize, access_index_IndexKey_bound(&unique), 33, 436, 23);
	sum = STD_MUL(usize, 32, sum, 436, 17);
	sum = STD_ADD(usize, sum, widest, 436, 17);
	sum = STD_ADD(usize, sum, value, 436, 17);
	STD_ASSERT(436, 9, sum <= CHUNKS_MAX_ENTRY,
			   "32 * (unique.bound() + 33) + widest + value <= chunks::MAX_ENTRY");
	access_index_IndexKey_drop(&unique);
}

/*
 * The fork's own (the storage design's II.7): a deferrable unique index is
 * keyed as not unique, so two equal rows have two keys until its check, and an
 * immediate one is keyed as unique by its NULLs.
 */
void
test_access_index_tests_a_deferrable_unique_index_is_keyed_as_not_unique(void)
{
	access_TupleOrderKeyTypeColumn_t columns[1] = {access_index_tests_text()};
	AccessIndexIndexKey deferred;
	static const uint8 first[5] = {1, 0, 0, 0, 1};
	static const uint8 second[5] = {1, 0, 0, 0, 2};
	access_OptionValue_t values[1] = {access_index_tests_bytes("a", 1)};
	AccessIndexEntryKey one;
	AccessIndexEntryKey two;

	STD_ASSERT(443, 9,
			   access_index_Unique_of(true, false, false) == ACCESS_INDEX_UNIQUE_NO &&
			   access_index_Unique_of(true, false, true) == ACCESS_INDEX_UNIQUE_NO,
			   "Unique::of(true, false, false) == Unique::No && Unique::of(true, false, true) == Unique::No");
	STD_ASSERT(444, 9, access_index_Unique_of(false, true, false) == ACCESS_INDEX_UNIQUE_NO,
			   "Unique::of(false, true, false) == Unique::No");
	STD_ASSERT(445, 9, access_index_Unique_of(true, true, false) == ACCESS_INDEX_UNIQUE_NULLS_DISTINCT,
			   "Unique::of(true, true, false) == Unique::NullsDistinct");
	STD_ASSERT(446, 9, access_index_Unique_of(true, true, true) == ACCESS_INDEX_UNIQUE_NULLS_NOT_DISTINCT,
			   "Unique::of(true, true, true) == Unique::NullsNotDistinct");
	deferred = access_index_tests_index(columns, 1, NULL, 0, access_index_Unique_of(true, false, false), false, 5,
										NULL, 0);
	one = access_index_tests_encode(&deferred, values, 1, first, 5);
	two = access_index_tests_encode(&deferred, values, 1, second, 5);
	STD_ASSERT(451, 9,
			   !access_index_tests_vec_eq(&one.key, two.key.ptr, two.key.len) &&
			   access_index_tests_ends_with(&one.key, first, 5),
			   "one.key != two.key && one.key.ends_with(&first)");
	access_index_EntryKey_drop(&two);
	access_index_EntryKey_drop(&one);
	access_index_IndexKey_drop(&deferred);
}

/* access_index_tests_prefix returns a run of a vector's bytes from a place, of a length. */
static StdBytes
access_index_tests_prefix(const StdVec *vec, size_t at, size_t len)
{
	StdBytes	bytes = {(const uint8 *) vec->ptr + at, len};

	return bytes;
}

/* access_index_tests_assert_bytes is assert_eq! of two [u8] slices at a place, with an optional message. */
static void
access_index_tests_assert_bytes(uint32 line, uint32 col, StdBytes left, StdBytes right, const StdFmtArguments *message)
{
	if (left.len != right.len || memcmp(left.ptr, right.ptr, left.len) != 0)
		std_assert_failed(RUST_FILE, line, col, "==", &left, std_bytes_Debug_fmt, &right, std_bytes_Debug_fmt, message);
}

/*
 * test_access_index_tests_two_cut_keys_sharing_their_first_bytes_differ_closure_0
 * is the test's encode of a value under the row's key.
 */
static AccessIndexEntryKey
test_access_index_tests_two_cut_keys_sharing_their_first_bytes_differ_closure_0(const AccessIndexIndexKey *index,
																				const StdVec *value,
																				const uint8 *row_key)
{
	access_OptionValue_t values[1] = {access_index_tests_bytes(value->ptr, value->len)};

	return access_index_tests_encode(index, values, 1, row_key, 5);
}

/*
 * The fork's own (design item 4's form D): two values whose keys share their
 * first bytes past the bound are both cut, their keys share those first bytes
 * and still differ, and each cut column's original is a blob its value
 * references.
 */
void
test_access_index_tests_two_cut_keys_sharing_their_first_bytes_differ(void)
{
	access_TupleOrderKeyTypeColumn_t columns[1] = {access_index_tests_text()};
	AccessIndexIndexKey index = access_index_tests_index(columns, 1, NULL, 0, ACCESS_INDEX_UNIQUE_NO, false, 5, NULL, 0);
	size_t		bound = access_index_IndexKey_bound(&index);
	StdVec		one;
	StdVec		two;
	uint8		x = 'x';
	static const uint8 row_key[5] = {1, 0, 0, 0, 1};
	AccessIndexEntryKey keys[2];
	AccessHeaptupleSet originals[1];
	AccessHeaptuplePlacing placing;
	access_index_tests_formed_t formed;
	EncodingTupleTuple tuple;
	EncodingTupleField field;
	size_t		len;
	StdBytes	left;
	StdBytes	right;

	len = STD_ADD(usize, bound, 100, 476, 46);
	std_vec_new(&one, ACCESS_INDEX_BYTES);
	std_vec_resize(&one, len, &x, ACCESS_INDEX_BYTES);
	len = STD_ADD(usize, bound, 100, 476, 79);
	std_vec_new(&two, ACCESS_INDEX_BYTES);
	std_vec_resize(&two, len, &x, ACCESS_INDEX_BYTES);
	if (one.len == 0)
		std_option_unwrap_failed(RUST_FILE, 477, 26);
	if (two.len == 0)
		std_option_unwrap_failed(RUST_FILE, 477, 52);
	((uint8 *) one.ptr)[one.len - 1] = 'a';
	((uint8 *) two.ptr)[two.len - 1] = 'b';
	keys[0] = test_access_index_tests_two_cut_keys_sharing_their_first_bytes_differ_closure_0(&index, &one, row_key);
	keys[1] = test_access_index_tests_two_cut_keys_sharing_their_first_bytes_differ_closure_0(&index, &two, row_key);
	STD_ASSERT(481, 9,
			   access_index_tests_cut_is(&keys[0], true) && access_index_tests_cut_is(&keys[1], true) &&
			   !access_index_tests_vec_eq(&keys[0].key, keys[1].key.ptr, keys[1].key.len),
			   "keys.iter().all(|key| key.cut == [true]) && keys[0].key != keys[1].key");
	std_range_to_check(bound, keys[0].key.len, RUST_FILE, 482, 20);
	std_range_to_check(bound, keys[1].key.len, RUST_FILE, 482, 50);
	left = access_index_tests_prefix(&keys[0].key, 0, bound);
	right = access_index_tests_prefix(&keys[1].key, 0, bound);
	access_index_tests_assert_bytes(482, 9, left, right, NULL);
	originals[0] = access_index_tests_set(ACCESS_HEAPTUPLE_SET_VARLENA, one.ptr, one.len);
	placing = access_index_tests_value(&index, &keys[0], row_key, 5, originals, 1, NULL, 0);
	formed = access_index_tests_formed(&placing);
	STD_ASSERT(485, 9,
			   formed.blobs.len == 1 &&
			   access_index_tests_vec_eq(&STD_VEC_AT(&formed.blobs, StdVec, 0), one.ptr, one.len),
			   "blobs.len() == 1 && blobs[0] == one");
	tuple = access_index_tests_parse(formed.entry.value.ptr, formed.entry.value.len, 486, 48);
	field = encoding_tuple_Tuple_field(&tuple, 0);
	STD_ASSERT(487, 9,
			   field.tag == ENCODING_TUPLE_FIELD_EXTERNAL && field.u.external.length == (uint64) one.len,
			   "matches!(tuple.field(0), Field::External { length, .. } if length == one.len() as u64)");
	access_index_tests_formed_drop(&formed);
	access_index_EntryKey_drop(&keys[1]);
	access_index_EntryKey_drop(&keys[0]);
	std_vec_free_Checked(&two, ACCESS_INDEX_BYTES);
	std_vec_free_Checked(&one, ACCESS_INDEX_BYTES);
	access_index_IndexKey_drop(&index);
}

/* access_index_tests_fields_t is a run of a tuple's fields to compare, a [Field] slice. */
typedef struct access_index_tests_fields_t
{
	const EncodingTupleField *ptr;
	size_t		len;
} access_index_tests_fields_t;

/* access_index_tests_fields_Debug_fmt is a [Field] slice's Debug. */
static bool
access_index_tests_fields_Debug_fmt(const void *value, StdFormatter *f)
{
	const access_index_tests_fields_t *fields = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t i = 0; i < fields->len; i++)
		std_DebugList_entry(&list, &fields->ptr[i], encoding_tuple_Field_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* access_index_tests_field_value returns a field holding some bytes. */
static EncodingTupleField
access_index_tests_field_value(const void *bytes, size_t len)
{
	EncodingTupleField field = {.tag = ENCODING_TUPLE_FIELD_VALUE, .u.value = {bytes, len}};

	return field;
}

/*
 * The fork's own (the btree text's section 3): each part of a unique index's
 * value reads back from its tuple in order: the row's key, each INCLUDE
 * column's value, a lossy key column's original, and a NULL for an exact key
 * column the key holds whole.
 */
void
test_access_index_tests_each_part_of_an_entrys_value_reads_back(void)
{
	access_TupleOrderKeyTypeColumn_t columns[2] = {{ENCODING_KEY_ORDER_ASC, access_key_KeyType_of(FLOAT8OID),
	access_index_tests_stored(FLOAT8OID, 8, true, ACCESS_HEAPTUPLE_STORAGE_PLAIN)}, access_index_tests_text()};
	AccessHeaptupleColumn includes[2] = {access_index_tests_stored(TEXTOID, -1, false, ACCESS_HEAPTUPLE_STORAGE_EXTENDED),
	access_index_tests_stored(INT4OID, 4, true, ACCESS_HEAPTUPLE_STORAGE_PLAIN)};
	static const size_t parts[3] = {16, 4, 8};
	AccessIndexIndexKey index = access_index_tests_index(columns, 2, includes, 2, ACCESS_INDEX_UNIQUE_NULLS_DISTINCT,
														 false, 5, parts, 3);
	static const uint8 row_key[5] = {3, 0, 0, 0, 9};
	uint64		number;
	float8		two_and_a_half = 2.5;
	AccessHeaptupleSet originals[2];
	access_OptionValue_t values[2];
	AccessHeaptupleSet included[2];
	AccessIndexEntryKey key;
	AccessHeaptuplePlacing placing;
	access_index_tests_formed_t formed;
	EncodingTupleTuple tuple;
	EncodingTupleField fields[5];
	static const uint8 nine[4] = {9, 0, 0, 0};
	uint8		number_bytes[8];
	EncodingTupleField expected[4];
	access_index_tests_fields_t left;
	access_index_tests_fields_t right;
	bool		same = true;
	EncodingTupleField null = {.tag = ENCODING_TUPLE_FIELD_NULL};

	memcpy(&number, &two_and_a_half, sizeof(number));
	originals[0] = access_index_tests_set_word(number);
	originals[1] = access_index_tests_set(ACCESS_HEAPTUPLE_SET_VARLENA, "key", 3);
	values[0] = access_index_tests_word(number);
	values[1] = access_index_tests_bytes("key", 3);
	key = access_index_tests_encode(&index, values, 2, row_key, 5);
	included[0] = access_index_tests_set(ACCESS_HEAPTUPLE_SET_VARLENA, "included", 8);
	included[1] = access_index_tests_set_word(9);
	placing = access_index_tests_value(&index, &key, row_key, 5, originals, 2, included, 2);
	formed = access_index_tests_formed(&placing);
	STD_ASSERT(504, 9,
			   formed.blobs.len == 0 && formed.entry.addresses.len == 0 &&
			   access_index_tests_vec_eq(&formed.entry.key, key.key.ptr, key.key.len),
			   "blobs.is_empty() && entry.addresses.is_empty() && entry.key == key.key");
	tuple = access_index_tests_parse(formed.entry.value.ptr, formed.entry.value.len, 505, 48);
	for (size_t i = 0; i < 5; i++)
		fields[i] = encoding_tuple_Tuple_field(&tuple, i);
	for (int i = 0; i < 8; i++)
		number_bytes[i] = (uint8) (number >> (8 * i));
	expected[0] = access_index_tests_field_value(row_key, 5);
	expected[1] = access_index_tests_field_value("included", 8);
	expected[2] = access_index_tests_field_value(nine, 4);
	expected[3] = access_index_tests_field_value(number_bytes, 8);
	std_range_to_check(4, 5, RUST_FILE, 508, 13);
	for (int i = 0; i < 4 && same; i++)
		same = encoding_tuple_Field_PartialEq_eq(&fields[i], &expected[i]);
	left = (access_index_tests_fields_t)
	{
		fields, 4
	};
	right = (access_index_tests_fields_t)
	{
		expected, 4
	};
	STD_ASSERT_EQ(507, 9, same, &left, access_index_tests_fields_Debug_fmt, &right,
				  access_index_tests_fields_Debug_fmt);
	std_index_check(4, 5, RUST_FILE, 516, 20);
	STD_ASSERT_EQ(516, 9, encoding_tuple_Field_PartialEq_eq(&fields[4], &null), &fields[4],
				  encoding_tuple_Field_Debug_fmt, &null, encoding_tuple_Field_Debug_fmt);
	access_index_tests_formed_drop(&formed);
	access_index_EntryKey_drop(&key);
	access_index_IndexKey_drop(&index);
}

/* access_index_tests_option_Debug_fmt is an Option<usize>'s Debug, a StdUsizeOption. */
static bool
access_index_tests_option_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdUsizeOption *option = value;
	StdOptionRef ref = {option->has, {&option->x, std_usize_Debug_fmt}};

	return std_option_Debug_fmt(&ref, f);
}

/* access_index_tests_assert_len is assert_eq! of two usizes at a place, with an optional message. */
static void
access_index_tests_assert_len(uint32 line, uint32 col, size_t left, size_t right, const StdFmtArguments *message)
{
	if (left != right)
		std_assert_failed(RUST_FILE, line, col, "==", &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt, message);
}

/*
 * The fork's own (the btree text's section 3): an INCLUDE column too long for
 * an entry moves out of line as a row's value does, its blob asked for and its
 * value a reference to it, and a keyless table's entry leaves room for the
 * count of the row's copies.
 */
void
test_access_index_tests_an_include_column_moves_out_of_line_and_a_keyless_entry_leaves_room_for_its_count(void)
{
	access_TupleOrderKeyTypeColumn_t columns[1] = {access_index_tests_text()};
	AccessHeaptupleColumn includes[1] = {access_index_tests_stored(TEXTOID, -1, false, ACCESS_HEAPTUPLE_STORAGE_EXTENDED)};
	static const size_t parts[1] = {CHUNKS_MAX_ENTRY};
	AccessIndexIndexKey index = access_index_tests_index(columns, 1, includes, 1, ACCESS_INDEX_UNIQUE_NO, true, 16,
														 parts, 1);
	uint8		row_key[16];
	StdVec		long_value;
	uint8		i_byte = 'i';
	access_OptionValue_t values[1] = {access_index_tests_bytes("a", 1)};
	AccessHeaptupleSet originals[1];
	AccessHeaptupleSet included[1];
	AccessIndexEntryKey key;
	AccessHeaptuplePlacing placing;
	StdUsizeOption field = {0};
	StdUsizeOption zero = {true, 0};
	access_index_tests_formed_t formed;
	EncodingTupleTuple tuple;
	size_t		sum;
	size_t		at;
	size_t		end;
	ChunksAddressAddress address;
	StdBytes	left;
	StdBytes	right;

	memset(row_key, 5, sizeof(row_key));
	std_vec_new(&long_value, ACCESS_INDEX_BYTES);
	std_vec_resize(&long_value, CHUNKS_MAX_ENTRY, &i_byte, ACCESS_INDEX_BYTES);
	key = access_index_tests_encode(&index, values, 1, row_key, sizeof(row_key));
	originals[0] = access_index_tests_set(ACCESS_HEAPTUPLE_SET_VARLENA, "a", 1);
	included[0] = access_index_tests_set(ACCESS_HEAPTUPLE_SET_VARLENA, long_value.ptr, long_value.len);
	placing = access_index_tests_value(&index, &key, row_key, sizeof(row_key), originals, 1, included, 1);
	field.has = access_heaptuple_Placing_field(&placing, &field.x);
	STD_ASSERT_EQ(529, 9, field.has && field.x == 0, &field, access_index_tests_option_Debug_fmt, &zero,
				  access_index_tests_option_Debug_fmt);
	formed = access_index_tests_formed(&placing);
	STD_ASSERT(531, 9,
			   formed.blobs.len == 1 &&
			   access_index_tests_vec_eq(&STD_VEC_AT(&formed.blobs, StdVec, 0), long_value.ptr, long_value.len),
			   "blobs.len() == 1 && blobs[0] == long");
	tuple = access_index_tests_parse(formed.entry.value.ptr, formed.entry.value.len, 532, 53);
	STD_ASSERT(532, 9, encoding_tuple_Tuple_field(&tuple, 0).tag == ENCODING_TUPLE_FIELD_EXTERNAL,
			   "matches!(Tuple::parse(&entry.value).unwrap().field(0), Field::External { .. })");
	sum = STD_ADD(usize, formed.entry.key.len, ACCESS_HEAPTUPLE_MAX_COUNT_LEN, 533, 17);
	sum = STD_ADD(usize, sum, formed.entry.value.len, 533, 17);
	sum = STD_ADD(usize, sum, ACCESS_HEAPTUPLE_ENTRY_OVERHEAD, 533, 17);
	STD_ASSERT(533, 9, sum <= CHUNKS_MAX_ENTRY,
			   "entry.key.len() + MAX_COUNT_LEN + entry.value.len() + ENTRY_OVERHEAD <= chunks::MAX_ENTRY");
	access_index_tests_assert_len(534, 9, formed.entry.addresses.len, 1, NULL);
	std_index_check(0, formed.entry.addresses.len, RUST_FILE, 535, 18);
	at = (size_t) STD_VEC_AT(&formed.entry.addresses, chunks_TupleU32Locator_t, 0).f0;
	end = STD_ADD(usize, at, 32, 536, 36);
	std_range_check(at, end, formed.entry.value.len, RUST_FILE, 536, 20);
	address = chunks_address_Address_of(long_value.ptr, long_value.len);
	left = access_index_tests_prefix(&formed.entry.value, at, 32);
	right.ptr = address.f0;
	right.len = 32;
	access_index_tests_assert_bytes(536, 9, left, right, NULL);
	access_index_tests_formed_drop(&formed);
	access_index_EntryKey_drop(&key);
	std_vec_free_Checked(&long_value, ACCESS_INDEX_BYTES);
	access_index_IndexKey_drop(&index);
}

/*
 * The fork's own (design item 4's form D): a unique index's largest entry,
 * each of 32 text columns cut on a table whose key is at its largest, fits
 * MAX_ENTRY once its value is placed.
 */
void
test_access_index_tests_a_unique_indexs_largest_formed_entry_fits(void)
{
	size_t		widest = access_index_tests_widest();
	AccessIndexIndexKey index = access_index_tests_new_texts(ACCESS_INDEX_UNIQUE_NULLS_DISTINCT, widest);
	StdVec		long_value;
	StdVec		row_key;
	uint8		z = 'z';
	uint8		seven = 7;
	access_OptionValue_t values[32];
	AccessHeaptupleSet originals[32];
	AccessIndexEntryKey key;
	AccessHeaptuplePlacing placing;
	access_index_tests_formed_t formed;
	size_t		sum;

	std_vec_new(&long_value, ACCESS_INDEX_BYTES);
	std_vec_resize(&long_value, STD_ADD(usize, access_index_IndexKey_bound(&index), 1, 546, 31), &z,
				   ACCESS_INDEX_BYTES);
	std_vec_new(&row_key, ACCESS_INDEX_BYTES);
	std_vec_resize(&row_key, widest, &seven, ACCESS_INDEX_BYTES);
	for (int i = 0; i < 32; i++)
	{
		values[i] = access_index_tests_bytes(long_value.ptr, long_value.len);
		originals[i] = access_index_tests_set(ACCESS_HEAPTUPLE_SET_VARLENA, long_value.ptr, long_value.len);
	}
	key = access_index_tests_encode(&index, values, 32, row_key.ptr, row_key.len);
	placing = access_index_tests_value(&index, &key, row_key.ptr, row_key.len, originals, 32, NULL, 0);
	formed = access_index_tests_formed(&placing);
	access_index_tests_assert_len(551, 9, formed.blobs.len, 32, NULL);
	sum = STD_ADD(usize, formed.entry.key.len, formed.entry.value.len, 552, 17);
	sum = STD_ADD(usize, sum, ACCESS_HEAPTUPLE_ENTRY_OVERHEAD, 552, 17);
	STD_ASSERT(552, 9, sum <= CHUNKS_MAX_ENTRY,
			   "entry.key.len() + entry.value.len() + ENTRY_OVERHEAD <= chunks::MAX_ENTRY");
	access_index_tests_formed_drop(&formed);
	access_index_EntryKey_drop(&key);
	std_vec_free_Checked(&row_key, ACCESS_INDEX_BYTES);
	std_vec_free_Checked(&long_value, ACCESS_INDEX_BYTES);
	access_index_IndexKey_drop(&index);
}

/* access_index_tests_unhex_closure_0 is unhex's map: the byte of the two hexadecimal digits at a place. */
static uint8
access_index_tests_unhex_closure_0(const char *text, size_t text_len, size_t i)
{
	static const char message[] = "called `Result::unwrap()` on an `Err` value";
	size_t		end = STD_ADD(usize, i, 2, 557, 72);
	uint8		byte;
	StdParseIntError error;

	std_str_index_range(text, text_len, i, end, RUST_FILE, 557, 64);
	if (!std_u8_from_str_radix(text +i, 2, 16, &byte, &error))
		std_result_unwrap_failed(message, sizeof(message) - 1, &error, std_ParseIntError_Debug_fmt, RUST_FILE,
								 557, 84);
	return byte;
}

/* access_index_tests_unhex is tests' unhex: the bytes of lowercase hexadecimal. */
static StdVec
access_index_tests_unhex(const char *text, size_t text_len)
{
	StdVec		bytes;

	std_vec_new(&bytes, ACCESS_INDEX_BYTES);
	for (size_t i = 0; i < text_len; i += 2)
	{
		uint8		byte = access_index_tests_unhex_closure_0(text, text_len, i);

		std_vec_push_Checked(&bytes, &byte, ACCESS_INDEX_BYTES);
	}
	return bytes;
}

/*
 * access_index_tests_next_line is str::lines' next: the line before the next
 * '\n', without it or a '\r' before it, or false at the end.
 */
static bool
access_index_tests_next_line(StdStr *rest, StdStr *line)
{
	const char *newline;
	size_t		taken;

	if (rest->len == 0)
		return false;
	newline = memchr(rest->ptr, '\n', rest->len);
	taken = newline != NULL ? (size_t) (newline - rest->ptr) + 1 : rest->len;
	line->ptr = rest->ptr;
	line->len = taken;
	rest->ptr += taken;
	rest->len -= taken;
	if (newline != NULL)
	{
		line->len--;
		if (line->len > 0 && line->ptr[line->len - 1] == '\r')
			line->len--;
	}
	return true;
}

/* access_index_tests_str_is reports whether a str is some text. */
static bool
access_index_tests_str_is(StdStr str, const char *text)
{
	return str.len == strlen(text) && memcmp(str.ptr, text, str.len) == 0;
}

/* access_index_tests_blobs_Debug_fmt is a [Vec<u8>] slice's Debug, a StdVec of StdVecs. */
static bool
access_index_tests_blobs_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *blobs = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t i = 0; i < blobs->len; i++)
	{
		const StdVec *blob = &STD_VEC_AT(blobs, StdVec, i);
		StdBytes	bytes = {blob->ptr, blob->len};

		std_DebugList_entry(&list, &bytes, std_bytes_Debug_fmt);
	}
	return std_DebugList_finish(&list);
}

/*
 * The btree text's section 3, a differential: each entry of a non-unique index
 * with an INCLUDE int4 over the corpus test/btree/generator recorded from
 * Doltgrust's encode_with_type and its tuple has Doltgrust's key, and its
 * value's fields are Doltgrust's, past the fork's own field.
 */
void
test_access_index_tests_a_non_unique_indexs_entries_are_doltgrusts(void)
{
	static const uint8 row_key[5] = {1, 0, 0, 0, 7};
	static const char unwrap_message[] = "called `Result::unwrap()` on an `Err` value";
	StdStr		rest = {(const char *) ACCESS_INDEX_TESTS_EXPECTED_BYTES, sizeof(ACCESS_INDEX_TESTS_EXPECTED_BYTES)};
	StdStr		line;
	size_t		checked = 0;

	while (access_index_tests_next_line(&rest, &line))
	{
		StdStr		parts[6];
		size_t		count = 0;
		size_t		start = 0;
		uint32		typid;
		int16		typlen;
		StdParseIntError parse_error;
		EncodingKeyOrder order = {false, true};
		bool		has_bytes;
		StdVec		bytes = STD_VEC_NEW;
		bool		has_word;
		uint64		word = 0;
		access_OptionValue_t given[1] = {{false}};
		AccessHeaptupleSet original[1] = {{.tag = ACCESS_HEAPTUPLE_SET_NULL}};
		access_TupleOrderKeyTypeColumn_t columns[1];
		AccessHeaptupleColumn includes[1] = {access_index_tests_stored(INT4OID, 4, true, ACCESS_HEAPTUPLE_STORAGE_PLAIN)};
		static const size_t value_parts[2] = {4, 16};
		AccessIndexIndexKey index;
		AccessIndexEntryKey entry_key;
		StdVec		theirs_key;
		AccessHeaptupleSet nine[1];
		AccessHeaptuplePlacing placing;
		access_index_tests_formed_t formed;
		EncodingTupleTuple fork;
		StdVec		theirs_bytes;
		EncodingTupleTuple theirs;
		AccessKeyKeyType key_type;
		bool		own;
		size_t		next_blob = 0;

		ACCESS_INDEX_TESTS_LINE(message, line);

		if (line.len > 0 && line.ptr[0] == '#')
			continue;
		for (size_t i = 0; i <= line.len; i++)
		{
			if (i < line.len && line.ptr[i] != '\t')
				continue;
			if (count < 6)
				parts[count] = (StdStr)
			{
				line.ptr + start, i - start
			};
			count++;
			start = i + 1;
		}
		if (count != 6)
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("a corpus line has six fields: "), STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&line, std_str_Display_fmt}};
			StdFmtArguments arguments = {pieces, 2, args};

			std_panic_fmt(RUST_FILE, 571, 17, &arguments);
		}
		if (!std_str_parse_u32(parts[0].ptr, parts[0].len, &typid, &parse_error))
			std_result_unwrap_failed(unwrap_message, sizeof(unwrap_message) - 1, &parse_error,
									 std_ParseIntError_Debug_fmt, RUST_FILE, 573, 62);
		if (!std_str_parse_i16(parts[1].ptr, parts[1].len, &typlen, &parse_error))
			std_result_unwrap_failed(unwrap_message, sizeof(unwrap_message) - 1, &parse_error,
									 std_ParseIntError_Debug_fmt, RUST_FILE, 573, 87);
		if (access_index_tests_str_is(parts[2], "asc"))
			order = ENCODING_KEY_ORDER_ASC;
		else if (access_index_tests_str_is(parts[2], "desc"))
			order = ENCODING_KEY_ORDER_DESC;
		has_bytes = parts[3].len > 0 && parts[3].ptr[0] == 'b';
		if (has_bytes)
			bytes = access_index_tests_unhex(parts[3].ptr + 1, parts[3].len - 1);
		has_word = parts[3].len > 0 && parts[3].ptr[0] == 'w';
		if (has_word && !std_u64_from_str_radix(parts[3].ptr + 1, parts[3].len - 1, 16, &word, &parse_error))
			std_result_unwrap_failed(unwrap_message, sizeof(unwrap_message) - 1, &parse_error,
									 std_ParseIntError_Debug_fmt, RUST_FILE, 580, 89);
		if (has_word)
		{
			given[0] = access_index_tests_word(word);
			original[0] = access_index_tests_set_word(word);
		}
		else if (has_bytes)
		{
			given[0] = access_index_tests_bytes(bytes.ptr, bytes.len);
			original[0] = access_index_tests_set(typlen == -1 ? ACCESS_HEAPTUPLE_SET_VARLENA : ACCESS_HEAPTUPLE_SET_FIXED,
												 bytes.ptr, bytes.len);
		}
		columns[0].f0 = order;
		columns[0].f1 = access_key_KeyType_of(typid);
		columns[0].f2 = access_index_tests_stored(typid, typlen, has_word,
												  typlen == -1 ? ACCESS_HEAPTUPLE_STORAGE_EXTENDED :
												  ACCESS_HEAPTUPLE_STORAGE_PLAIN);
		index = access_index_tests_index(columns, 1, includes, 1, ACCESS_INDEX_UNIQUE_NO, false, 5, value_parts, 2);
		entry_key = access_index_tests_encode(&index, given, 1, row_key, 5);
		theirs_key = access_index_tests_unhex(parts[4].ptr, parts[4].len);
		if (!(access_index_tests_vec_eq(&entry_key.key, theirs_key.ptr, theirs_key.len) &&
			  access_index_tests_cut_is(&entry_key, false)))
			std_panic_fmt(RUST_FILE, 599, 13, &message);
		nine[0] = access_index_tests_set_word(9);
		placing = access_index_tests_value(&index, &entry_key, row_key, 5, original, 1, nine, 1);
		formed = access_index_tests_formed(&placing);
		fork = access_index_tests_parse(formed.entry.value.ptr, formed.entry.value.len, 602, 51);
		theirs_bytes = access_index_tests_unhex(parts[5].ptr, parts[5].len);
		theirs = access_index_tests_parse(theirs_bytes.ptr, theirs_bytes.len, 604, 48);
		key_type = access_key_KeyType_of(typid);
		own = access_key_is_exact(&key_type) && access_key_can_be_cut(&key_type, access_index_IndexKey_bound(&index));
		access_index_tests_assert_len(607, 13, encoding_tuple_Tuple_len(&fork),
									  STD_ADD(usize, encoding_tuple_Tuple_len(&theirs), (size_t) own,
											  607, 36),
									  &message);
		for (size_t i = 0; i < encoding_tuple_Tuple_len(&theirs); i++)
		{
			EncodingTupleField field = encoding_tuple_Tuple_field(&fork, i);
			EncodingTupleField their = encoding_tuple_Tuple_field(&theirs, i);

			if (field.tag == ENCODING_TUPLE_FIELD_EXTERNAL)
			{
				const StdVec *blob;
				ChunksAddressAddress address;
				EncodingTupleField value;

				if (next_blob >= formed.blobs.len)
					std_option_unwrap_failed(RUST_FILE, 612, 49);
				blob = &STD_VEC_AT(&formed.blobs, StdVec, next_blob);
				next_blob++;
				address = chunks_address_Address_of(blob->ptr, blob->len);
				value = access_index_tests_field_value(blob->ptr, blob->len);
				STD_ASSERT(613, 25,
						   field.u.external.address.len == 32 &&
						   memcmp(field.u.external.address.ptr, address.f0, 32) == 0 &&
						   encoding_tuple_Field_PartialEq_eq(&their, &value),
						   "address == chunks::Address::of(blob).0 && theirs.field(i) == Field::Value(blob)");
			}
			else if (!encoding_tuple_Field_PartialEq_eq(&field, &their))
				std_assert_failed(RUST_FILE, 615, 30, "==", &field, encoding_tuple_Field_Debug_fmt, &their,
								  encoding_tuple_Field_Debug_fmt, &message);
		}
		STD_ASSERT(618, 13, next_blob >= formed.blobs.len, "blobs.next().is_none()");
		if (own)
		{
			EncodingTupleField field = encoding_tuple_Tuple_field(&fork, encoding_tuple_Tuple_len(&theirs));
			EncodingTupleField null = {.tag = ENCODING_TUPLE_FIELD_NULL};

			if (!encoding_tuple_Field_PartialEq_eq(&field, &null))
				std_assert_failed(RUST_FILE, 620, 17, "==", &field, encoding_tuple_Field_Debug_fmt, &null,
								  encoding_tuple_Field_Debug_fmt, &message);
		}
		checked = STD_ADD(usize, checked, 1, 622, 13);
		access_key_KeyType_drop(&key_type);
		std_vec_free_Checked(&theirs_bytes, ACCESS_INDEX_BYTES);
		access_index_tests_formed_drop(&formed);
		std_vec_free_Checked(&theirs_key, ACCESS_INDEX_BYTES);
		access_index_EntryKey_drop(&entry_key);
		access_index_IndexKey_drop(&index);
		std_vec_free_Checked(&bytes, ACCESS_INDEX_BYTES);
	}
	STD_ASSERT(624, 9, checked == 480, "checked == 480");
}

/*
 * test_access_index_tests_a_unique_indexs_entry_wider_than_nbtrees_limit_is_formed_closure_0
 * is the test's map: a byte of the wide value at a place, never 0.
 */
static uint8
test_access_index_tests_a_unique_indexs_entry_wider_than_nbtrees_limit_is_formed_closure_0(uint32 at)
{
	return (uint8) ((uint32) (at * UINT32_C(2654435761)) >> 24) | 1;
}

/*
 * Bugs row 109 (design item 4's form D, Differences row 32): a unique index's
 * entry over a 22,400-byte value, which nbtree's index_form_tuple refuses past
 * 8,191 bytes, is formed, its key cut at the index's bound and its original
 * out of line, within MAX_ENTRY.
 */
void
test_access_index_tests_a_unique_indexs_entry_wider_than_nbtrees_limit_is_formed(void)
{
	access_TupleOrderKeyTypeColumn_t columns[1] = {access_index_tests_text()};
	AccessIndexIndexKey index = access_index_tests_index(columns, 1, NULL, 0, ACCESS_INDEX_UNIQUE_NULLS_DISTINCT, false,
														 4, NULL, 0);
	StdVec		wide;
	static const uint8 row_key[4] = {0, 0, 0, 7};
	access_OptionValue_t values[1];
	AccessHeaptupleSet originals[1];
	AccessIndexEntryKey key;
	AccessHeaptuplePlacing placing;
	access_index_tests_formed_t formed;
	size_t		sum;
	StdVec		expected;

	std_vec_with_capacity_Checked(&wide, 22400, ACCESS_INDEX_BYTES);
	for (uint32 at = 0; at < 22400; at++)
	{
		uint8		byte = test_access_index_tests_a_unique_indexs_entry_wider_than_nbtrees_limit_is_formed_closure_0(at);

		std_vec_push_Checked(&wide, &byte, ACCESS_INDEX_BYTES);
	}
	values[0] = access_index_tests_bytes(wide.ptr, wide.len);
	key = access_index_tests_encode(&index, values, 1, row_key, sizeof(row_key));
	originals[0] = access_index_tests_set(ACCESS_HEAPTUPLE_SET_VARLENA, wide.ptr, wide.len);
	placing = access_index_tests_value(&index, &key, row_key, sizeof(row_key), originals, 1, NULL, 0);
	formed = access_index_tests_formed(&placing);
	STD_ASSERT_MSG(637, 9,
				   key.key.len <= STD_ADD(usize, access_index_IndexKey_bound(&index), 33, 637, 34),
				   "the key is cut at the bound");
	sum = STD_ADD(usize, formed.entry.key.len, formed.entry.value.len, 638, 17);
	sum = STD_ADD(usize, sum, ACCESS_HEAPTUPLE_ENTRY_OVERHEAD, 638, 17);
	STD_ASSERT(638, 9, sum <= CHUNKS_MAX_ENTRY,
			   "entry.key.len() + entry.value.len() + ENTRY_OVERHEAD <= chunks::MAX_ENTRY");
	expected = (StdVec)
	{
		&wide, 1, 1
	};
	STD_ASSERT_EQ_MSG(639, 9,
					  formed.blobs.len == 1 &&
					  access_index_tests_vec_eq(&STD_VEC_AT(&formed.blobs, StdVec, 0), wide.ptr, wide.len),
					  &formed.blobs, access_index_tests_blobs_Debug_fmt, &expected, access_index_tests_blobs_Debug_fmt,
					  "the original is one blob");
	access_index_tests_formed_drop(&formed);
	access_index_EntryKey_drop(&key);
	std_vec_free_Checked(&wide, ACCESS_INDEX_BYTES);
	access_index_IndexKey_drop(&index);
}

#endif
#endif
