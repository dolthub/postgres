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
 * Rows as a table's tree stores them (access' heaptuple.rs, Postgres'
 * heaptuple.c in the layout of the storage design's II.7 and II.8): a
 * table's layout, the deforming of a stored row into its attributes'
 * values, and the placing of a row's values in its tree, their largest
 * moved out of line and its trailing fields into overflow chunks, through
 * blobs its caller writes.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/access/heaptuple.h"
#include "doltgres/chunks/header.h"
#include "doltgres/chunks/lib.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/encoding/tuple.h"
#include "doltgres/pg/word.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "held.h"
#ifdef DOLTGRES_TEST_ACCESS
#include "catalog/pg_type_d.h"
#include "doltgres/chunks/address.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "access" STD_SEP "src" STD_SEP "heaptuple.rs"

/* The element layouts of heaptuple's vectors. */
#define ACCESS_HEAPTUPLE_BYTES 1, 1
#define ACCESS_HEAPTUPLE_BOOL sizeof(bool), _Alignof(bool)
#define ACCESS_HEAPTUPLE_USIZE sizeof(size_t), _Alignof(size_t)
#define ACCESS_HEAPTUPLE_COLUMN sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn)
#define ACCESS_HEAPTUPLE_KEYED sizeof(access_TupleUsizeOrderKeyType_t), _Alignof(access_TupleUsizeOrderKeyType_t)
#define ACCESS_HEAPTUPLE_LENGTH sizeof(StdUsizeOption), _Alignof(StdUsizeOption)
#define ACCESS_HEAPTUPLE_LEVEL sizeof(access_TupleUsizeUsize_t), _Alignof(access_TupleUsizeUsize_t)
#define ACCESS_HEAPTUPLE_FIELD sizeof(access_OptionCowBytes_t), _Alignof(access_OptionCowBytes_t)
#define ACCESS_HEAPTUPLE_LOCATOR sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator)
#define ACCESS_HEAPTUPLE_ADDRESS sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t)

/*
 * Postgres' constants of a heap tuple: MAXIMUM_ALIGNOF, its header, a line
 * pointer, a toast pointer, MAIN's target, and VARHDRSZ.
 */
#define ACCESS_HEAPTUPLE_MAXALIGN 8
#define ACCESS_HEAPTUPLE_HEAP_HEADER_LEN 23
#define ACCESS_HEAPTUPLE_ITEM_ID_LEN 4
#define ACCESS_HEAPTUPLE_TOAST_POINTER_LEN 18
#define ACCESS_HEAPTUPLE_TOAST_TUPLE_TARGET_MAIN 8160
#define ACCESS_HEAPTUPLE_VARHDRSZ 4

/* access_heaptuple_field_bytes returns the bytes an Option<Cow<[u8]>> holds, as_deref's. */
static StdBytes
access_heaptuple_field_bytes(const access_OptionCowBytes_t *field)
{
	StdBytes	bytes = {0};

	if (field->has && field->owned)
	{
		bytes.ptr = field->bytes.ptr;
		bytes.len = field->bytes.len;
	}
	else if (field->has)
		bytes = field->borrowed;
	return bytes;
}

/* access_heaptuple_field_drop frees an owned field's bytes, as a vector's element. */
void
access_heaptuple_field_drop(void *field_)
{
	access_OptionCowBytes_t *field = field_;

	if (field->has && field->owned)
		std_vec_free_Checked(&field->bytes, ACCESS_HEAPTUPLE_BYTES);
}

/* access_heaptuple_Set_PartialEq_eq reports whether two values are the same variant with equal contents. */
bool
access_heaptuple_Set_PartialEq_eq(const AccessHeaptupleSet *self, const AccessHeaptupleSet *other)
{
	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case ACCESS_HEAPTUPLE_SET_NULL:
			return true;
		case ACCESS_HEAPTUPLE_SET_WORD:
			return self->u.word == other->u.word;
		case ACCESS_HEAPTUPLE_SET_FIXED:
		case ACCESS_HEAPTUPLE_SET_VARLENA:
			return self->u.fixed.len == other->u.fixed.len &&
				memcmp(self->u.fixed.ptr, other->u.fixed.ptr, self->u.fixed.len) == 0;
	}
	pg_unreachable();
}

/*
 * access_heaptuple_heap_len_closure_0 is heap_len's null: whether a column is
 * NULL in a row, dropped or its value NULL.
 */
static bool
access_heaptuple_heap_len_closure_0(const AccessHeaptupleColumn *column, const AccessHeaptupleSet *value)
{
	AccessHeaptupleSet null = {.tag = ACCESS_HEAPTUPLE_SET_NULL};

	return column->dropped || access_heaptuple_Set_PartialEq_eq(value, &null);
}

/*
 * access_heaptuple_heap_len returns the length heap_form_tuple gives a row
 * on Postgres' heap pages, before its line pointer and its page's
 * alignment.
 */
static size_t
access_heaptuple_heap_len(const StdVec *columns, const AccessHeaptupleSet *values, size_t values_len,
						  const bool *moved, size_t moved_len)
{
	size_t		pairs = Min(columns->len, values_len);
	size_t		bitmap = 0;
	size_t		len;

	for (size_t i = 0; i < pairs; i++)
	{
		if (access_heaptuple_heap_len_closure_0(&STD_VEC_AT(columns, AccessHeaptupleColumn, i), &values[i]))
		{
			bitmap = (columns->len + 7) / 8;
			break;
		}
	}
	len = std_next_multiple_of_usize(STD_ADD(usize, ACCESS_HEAPTUPLE_HEAP_HEADER_LEN, bitmap, 73, 20),
									 ACCESS_HEAPTUPLE_MAXALIGN);
	for (size_t i = 0; i < Min(pairs, moved_len); i++)
	{
		const AccessHeaptupleColumn *column = &STD_VEC_AT(columns, AccessHeaptupleColumn, i);
		const AccessHeaptupleSet *value = &values[i];
		size_t		align = Max(column->align, 1);

		if (column->dropped || value->tag == ACCESS_HEAPTUPLE_SET_NULL)
			continue;
		if (value->tag == ACCESS_HEAPTUPLE_SET_VARLENA && moved[i])
			len = STD_ADD(usize, len, ACCESS_HEAPTUPLE_TOAST_POINTER_LEN, 78, 46);
		else if (value->tag == ACCESS_HEAPTUPLE_SET_VARLENA && column->storage != ACCESS_HEAPTUPLE_STORAGE_PLAIN &&
				 value->u.varlena.len <= 126)
			len = STD_ADD(usize, STD_ADD(usize, len, 1, 79, 99), value->u.varlena.len,
						  79, 99);
		else if (value->tag == ACCESS_HEAPTUPLE_SET_VARLENA)
			len = STD_ADD(usize, STD_ADD(usize, std_next_multiple_of_usize(len, align), 4, 80, 41),
						  value->u.varlena.len, 80, 41);
		else if (value->tag == ACCESS_HEAPTUPLE_SET_WORD)
			len = STD_ADD(usize, std_next_multiple_of_usize(len, align), (size_t) abs(column->len),
						  81, 34);
		else
			len = STD_ADD(usize, std_next_multiple_of_usize(len, align), value->u.fixed.len,
						  82, 39);
	}
	return len;
}

/* access_heaptuple_Storage_of answers the storage kind of an attstorage byte: 'p', 'e', 'm', or 'x'. */
bool
access_heaptuple_Storage_of(uint8 byte, AccessHeaptupleStorage *result, PgPgError *error)
{
	StdString	str;

	switch (byte)
	{
		case 'p':
			*result = ACCESS_HEAPTUPLE_STORAGE_PLAIN;
			return true;
		case 'e':
			*result = ACCESS_HEAPTUPLE_STORAGE_EXTERNAL;
			return true;
		case 'm':
			*result = ACCESS_HEAPTUPLE_STORAGE_MAIN;
			return true;
		case 'x':
			*result = ACCESS_HEAPTUPLE_STORAGE_EXTENDED;
			return true;
	}
	std_string_from_Checked(&str, "a column's storage kind is unknown", strlen("a column's storage kind is unknown"));
	*error = pg_PgError_fork((const uint8 *) "XX000", &str);
	return false;
}

/* access_heaptuple_Storage_byte returns the storage kind's attstorage byte, which a transform's field names it by. */
uint8
access_heaptuple_Storage_byte(AccessHeaptupleStorage self)
{
	switch (self)
	{
		case ACCESS_HEAPTUPLE_STORAGE_PLAIN:
			return 'p';
		case ACCESS_HEAPTUPLE_STORAGE_EXTERNAL:
			return 'e';
		case ACCESS_HEAPTUPLE_STORAGE_MAIN:
			return 'm';
		case ACCESS_HEAPTUPLE_STORAGE_EXTENDED:
			return 'x';
	}
	pg_unreachable();
}

/* access_heaptuple_reference_len returns the length of the reference to a value of a length kept out of line. */
static size_t
access_heaptuple_reference_len(size_t length)
{
	return STD_ADD(usize, chunks_varint_varint_len((uint64) length), ENCODING_TUPLE_ADDRESS_LEN,
				   332, 5);
}

/* access_heaptuple_is_movable reports whether a value of a type's length and its own length can move out of line. */
static bool
access_heaptuple_is_movable(int16 typlen, size_t length)
{
	return typlen == -1 && length > access_heaptuple_reference_len(length);
}

/* access_heaptuple_externals counts a tuple's fields out of line before a place. */
static size_t
access_heaptuple_externals(const EncodingTupleTuple *tuple, size_t place)
{
	EncodingTupleFields fields = encoding_tuple_Tuple_fields(tuple);
	EncodingTupleField field;
	size_t		count = 0;

	for (size_t taken = 0; taken < place && encoding_tuple_Fields_Iterator_next(&fields, &field); taken++)
		count = std_add_usize(count, (size_t) (field.tag == ENCODING_TUPLE_FIELD_EXTERNAL), STD_ACCUM_FILE, 206, 1);
	return count;
}

/* access_heaptuple_fork returns the fork's XX000 error of a message. */
static PgPgError
access_heaptuple_fork(const char *message)
{
	StdString	str;

	std_string_from_Checked(&str, message, strlen(message));
	return pg_PgError_fork((const uint8 *) "XX000", &str);
}

/* access_heaptuple_corrupt returns the error of a stored row that does not decode. */
static PgPgError
access_heaptuple_corrupt(void)
{
	return access_heaptuple_fork("a stored row is corrupt");
}

/* access_heaptuple_fail sets an error and answers false. */
static bool
access_heaptuple_fail(PgPgError made, PgPgError *error)
{
	*error = made;
	return false;
}

/*
 * access_heaptuple_word_of answers the word of a by-value type's stored
 * field: its bytes, sign-extended as Postgres' fetch_att reads a field of
 * their width.
 */
bool
access_heaptuple_word_of(const uint8 *bytes, size_t bytes_len, uint64 *result, PgPgError *error)
{
	uint64		word = 0;

	switch (bytes_len)
	{
		case 1:
			*result = pg_word_from_char(bytes[0]);
			return true;
		case 2:
			*result = pg_word_from_int16((int16) ((uint16) bytes[0] | (uint16) bytes[1] << 8));
			return true;
		case 4:
			*result = pg_word_from_int32((int32) ((uint32) bytes[0] | (uint32) bytes[1] << 8 |
												  (uint32) bytes[2] << 16 | (uint32) bytes[3] << 24));
			return true;
		case 8:
			for (int i = 7; i >= 0; i--)
				word = word << 8 | bytes[i];
			*result = word;
			return true;
	}
	return access_heaptuple_fail(access_heaptuple_corrupt(), error);
}

/* access_heaptuple_Plan_drop frees a plan's vectors. */
void
access_heaptuple_Plan_drop(AccessHeaptuplePlan *self)
{
	std_vec_free_Checked(&self->lengths, ACCESS_HEAPTUPLE_LENGTH);
	std_vec_free_Checked(&self->external, ACCESS_HEAPTUPLE_BOOL);
	std_vec_free_Checked(&self->moves, ACCESS_HEAPTUPLE_USIZE);
	std_vec_free_Checked(&self->levels, ACCESS_HEAPTUPLE_LEVEL);
}

/*
 * access_heaptuple_Plan_len returns the length of the tuple of the fields from
 * a position on, with an overflow address after the first inline of them when
 * that is fewer.
 */
static size_t
access_heaptuple_Plan_len(const AccessHeaptuplePlan *self, size_t from, size_t inline_)
{
	size_t		end = STD_ADD(usize, from, inline_, 294, 44);
	bool		external = false;
	size_t		total;
	size_t		lengths_end;

	std_range_check(from, end, self->external.len, RUST_FILE, 294, 24);
	for (size_t i = from; i < end && !external; i++)
		external = STD_VEC_AT(&self->external, bool, i);
	total = STD_SUB(usize, self->lengths.len, from, 295, 21);
	lengths_end = STD_ADD(usize, from, inline_, 296, 48);
	std_range_check(from, lengths_end, self->lengths.len, RUST_FILE, 296, 29);
	return encoding_tuple_written_len(&STD_VEC_AT(&self->lengths, StdUsizeOption, from), inline_, external,
									  inline_ < total, total);
}

/* access_heaptuple_Plan_move_out moves a field's value out of line, leaving its reference. */
static void
access_heaptuple_Plan_move_out(AccessHeaptuplePlan *self, size_t i)
{
	StdUsizeOption *length;
	StdUsizeOption moved = {true, 0};

	std_index_check(i, self->lengths.len, RUST_FILE, 301, 46);
	length = &STD_VEC_AT(&self->lengths, StdUsizeOption, i);
	moved.x = access_heaptuple_reference_len(length->has ? length->x : 0);
	std_index_check(i, self->lengths.len, RUST_FILE, 301, 9);
	*length = moved;
	std_index_check(i, self->external.len, RUST_FILE, 302, 9);
	STD_VEC_AT(&self->external, bool, i) = true;
	std_vec_push_Checked(&self->moves, &i, ACCESS_HEAPTUPLE_USIZE);
}

/* access_heaptuple_Plan_push_level appends a tuple's first field and count of fields to the plan's levels. */
static void
access_heaptuple_Plan_push_level(AccessHeaptuplePlan *self, size_t from, size_t count)
{
	access_TupleUsizeUsize_t level = {from, count};

	std_vec_push_Checked(&self->levels, &level, ACCESS_HEAPTUPLE_LEVEL);
}

/*
 * access_heaptuple_Plan_place plans the tuples of the fields, Doltgrust's
 * place: all of them when the tuple fits the room, or else its longest run
 * of first fields that fits with the address of an overflow chunk holding
 * the rest, placed again within MAX_ENTRY.
 */
static void
access_heaptuple_Plan_place(AccessHeaptuplePlan *self, size_t room)
{
	size_t		from = 0;
	bool		chunk = false;

	for (;;)
	{
		size_t		total = STD_SUB(usize, self->lengths.len, from, 313, 25);
		size_t		least;
		size_t		inline_;

		if (access_heaptuple_Plan_len(self, from, total) <= room)
		{
			access_heaptuple_Plan_push_level(self, from, total);
			return;
		}
		if (chunk && access_heaptuple_Plan_len(self, from, 1) > room)
		{
			access_heaptuple_Plan_move_out(self, from);
			continue;
		}
		least = (size_t) chunk;
		inline_ = least;
		for (size_t n = total; n-- > least;)
		{
			if (access_heaptuple_Plan_len(self, from, n) <= room)
			{
				inline_ = n;
				break;
			}
		}
		access_heaptuple_Plan_push_level(self, from, inline_);
		from = STD_ADD(usize, from, inline_, 325, 36);
		room = CHUNKS_MAX_ENTRY;
		chunk = true;
	}
}

/*
 * access_heaptuple_Plan_new plans the placing of fields: the fields already
 * out of line stay so, each of the fields forced names moves out of line
 * first, in order; then, once the tuple passes the threshold, for each kind
 * of storage in turn its largest movable value moves out of line until the
 * tuple is within that kind's target; then the trailing fields go into
 * overflow chunks. It fills a plan its caller made, whose external holds
 * the fields already out of line and whose other vectors are empty, which
 * the caller frees on an unwind.
 */
void
access_heaptuple_Plan_new(const access_OptionCowBytes_t *fields, size_t fields_len, const int16 *typlens,
						  size_t typlens_len, const AccessHeaptupleStorage *storages, size_t storages_len,
						  const size_t *forced, size_t forced_len, size_t threshold,
						  const AccessHeaptupleKind *kinds, size_t kinds_len, size_t room, AccessHeaptuplePlan *result)
{
	size_t		all = fields_len;

	std_vec_reserve_Checked(&result->lengths, fields_len, ACCESS_HEAPTUPLE_LENGTH);
	for (size_t i = 0; i < fields_len; i++)
	{
		StdUsizeOption length = {fields[i].has, access_heaptuple_field_bytes(&fields[i]).len};

		std_vec_push_Checked(&result->lengths, &length, ACCESS_HEAPTUPLE_LENGTH);
	}
	for (size_t f = 0; f < forced_len; f++)
		access_heaptuple_Plan_move_out(result, forced[f]);
	if (access_heaptuple_Plan_len(result, 0, all) > threshold)
	{
		for (size_t k = 0; k < kinds_len; k++)
		{
			while (access_heaptuple_Plan_len(result, 0, all) > kinds[k].target)
			{
				bool		found = false;
				size_t		biggest = 0;
				size_t		biggest_length = 0;

				for (size_t i = 0; i < all; i++)
				{
					bool		movable = false;
					StdUsizeOption length;
					size_t		key;

					std_index_check(i, result->external.len, RUST_FILE, 279, 39);
					if (STD_VEC_AT(&result->external, bool, i))
						continue;
					std_index_check(i, storages_len, RUST_FILE, 279, 77);
					for (size_t m = 0; m < kinds[k].movable_len && !movable; m++)
						movable = kinds[k].movable[m] == storages[i];
					if (!movable)
						continue;
					std_index_check(i, result->lengths.len, RUST_FILE, 280, 38);
					length = STD_VEC_AT(&result->lengths, StdUsizeOption, i);
					if (!length.has)
						continue;
					std_index_check(i, typlens_len, RUST_FILE, 280, 86);
					if (!access_heaptuple_is_movable(typlens[i], length.x))
						continue;
					std_index_check(i, result->lengths.len, RUST_FILE, 281, 43);
					key = length.x;
					if (!found || key > biggest_length)
					{
						found = true;
						biggest = i;
						biggest_length = key;
					}
				}
				if (!found)
					break;
				access_heaptuple_Plan_move_out(result, biggest);
			}
		}
	}
	access_heaptuple_Plan_place(result, room);
}

/* access_heaptuple_RowLayout_drop frees a layout's columns, key, and exact flags. */
void
access_heaptuple_RowLayout_drop(AccessHeaptupleRowLayout *self)
{
	std_vec_free_Checked(&self->columns, ACCESS_HEAPTUPLE_COLUMN);
	std_vec_drop_Checked(&self->key, ACCESS_HEAPTUPLE_KEYED, access_TupleUsizeOrderKeyType_drop);
	std_vec_free_Checked(&self->exact, ACCESS_HEAPTUPLE_BOOL);
}

/* access_heaptuple_key_types returns a key's key types as an iterator over them. */
static AccessKeyKeyTypes
access_heaptuple_key_types(const StdVec *key)
{
	AccessKeyKeyTypes key_types = {key->ptr, key->len, sizeof(access_TupleUsizeOrderKeyType_t),
	offsetof(access_TupleUsizeOrderKeyType_t, f2)};

	return key_types;
}

/* access_heaptuple_RowLayout_new_closure_1 is new's keyed: a column's key column whose key is exact, or NULL. */
static const access_TupleUsizeOrderKeyType_t *
access_heaptuple_RowLayout_new_closure_1(const StdVec *key, size_t i)
{
	for (size_t k = 0; k < key->len; k++)
	{
		const access_TupleUsizeOrderKeyType_t *keyed = &STD_VEC_AT(key, access_TupleUsizeOrderKeyType_t, k);

		if (keyed->f0 == i)
			return access_key_is_exact(&keyed->f2) ? keyed : NULL;
	}
	return NULL;
}

/* access_heaptuple_new_frame_t is new's state across its pad: the layout made so far, while live. */
typedef struct access_heaptuple_new_frame_t
{
	AccessHeaptupleRowLayout *result;
	bool		live;
	bool		exact_live;
} access_heaptuple_new_frame_t;

/* access_heaptuple_RowLayout_new_pad frees the layout made so far. */
static void
access_heaptuple_RowLayout_new_pad(void *frame_)
{
	access_heaptuple_new_frame_t *frame = frame_;

	if (frame->live)
	{
		std_vec_free_Checked(&frame->result->columns, ACCESS_HEAPTUPLE_COLUMN);
		std_vec_drop_Checked(&frame->result->key, ACCESS_HEAPTUPLE_KEYED, access_TupleUsizeOrderKeyType_drop);
	}
	if (frame->exact_live)
		std_vec_free_Checked(&frame->result->exact, ACCESS_HEAPTUPLE_BOOL);
	frame->live = frame->exact_live = false;
}

/* access_heaptuple_RowLayout_new_body works out the layout's bound, exact flags, and fields. */
static void
access_heaptuple_RowLayout_new_body(void *frame_)
{
	access_heaptuple_new_frame_t *frame = frame_;
	AccessHeaptupleRowLayout *layout = frame->result;
	size_t		fields = 0;
	size_t		count = 0;

	layout->bound = access_key_table_bound(access_heaptuple_key_types(&layout->key));
	std_vec_with_capacity_Checked(&layout->exact, layout->columns.len, ACCESS_HEAPTUPLE_BOOL);
	frame->exact_live = true;
	for (size_t i = 0; i < layout->columns.len; i++)
	{
		bool		exact = access_heaptuple_RowLayout_new_closure_1(&layout->key, i) != NULL;

		std_vec_push_Checked(&layout->exact, &exact, ACCESS_HEAPTUPLE_BOOL);
	}
	for (size_t i = 0; i < layout->columns.len; i++)
	{
		AccessHeaptupleColumn *column = &STD_VEC_AT(&layout->columns, AccessHeaptupleColumn, i);
		const access_TupleUsizeOrderKeyType_t *keyed = access_heaptuple_RowLayout_new_closure_1(&layout->key, i);
		bool		in_key_alone = keyed != NULL && !access_key_can_be_cut(&keyed->f2, layout->bound);

		count = std_Enumerate_count_step(count);
		column->has_field = !column->dropped && !in_key_alone;
		column->field = column->has_field ? fields : 0;
		fields = STD_ADD(usize, fields, (size_t) column->has_field, 381, 13);
	}
	frame->live = frame->exact_live = false;
}

/*
 * access_heaptuple_RowLayout_new returns a table's layout from its columns
 * and its primary key's columns, which it takes, and sets each column's
 * field: a column is a field of the tuple unless it was dropped or its key
 * is exact and can't be cut.
 */
void
access_heaptuple_RowLayout_new(StdVec *columns, StdVec *key, AccessHeaptupleRowLayout *result)
{
	access_heaptuple_new_frame_t frame = {.result = result, .live = true};

	result->columns = *columns;
	result->key = *key;
	std_landing(&frame, access_heaptuple_RowLayout_new_body, access_heaptuple_RowLayout_new_pad);
}

/*
 * access_heaptuple_transform_frame_t is transform_to's state across its pad:
 * the old layout's fields and the later's made so far.
 */
typedef struct access_heaptuple_transform_frame_t
{
	access_key_held_t held;
	const AccessHeaptupleRowLayout *self;
	const AccessHeaptupleRowLayout *later;
	AccessHeaptupleMissingFn missing;
	void	   *captures;
	uint32		target;
	uint32		later_target;
	StdVec		fields;
	bool		fields_live;
	bool	   *has_result;
	AccessTransformTransform *result;
	PgPgError  *error;
	bool		ok;
} access_heaptuple_transform_frame_t;

/* access_heaptuple_transform_field_drop is a transform's Field's drop glue as a vector's element. */
static void
access_heaptuple_transform_field_drop(void *field)
{
	access_transform_Field_drop(field);
}

/* access_heaptuple_RowLayout_transform_to_pad frees what transform_to holds. */
static void
access_heaptuple_RowLayout_transform_to_pad(void *frame_)
{
	access_heaptuple_transform_frame_t *frame = frame_;

	access_key_held_pad(&frame->held);
	if (frame->fields_live)
		std_vec_drop_Checked(&frame->fields, sizeof(AccessTransformField), _Alignof(AccessTransformField),
							 access_heaptuple_transform_field_drop);
	frame->fields_live = false;
}

/*
 * access_heaptuple_RowLayout_transform_to_body gives each of the later
 * layout's fields its source and compares the layouts.
 */
static void
access_heaptuple_RowLayout_transform_to_body(void *frame_)
{
	access_heaptuple_transform_frame_t *frame = frame_;
	StdVec	   *old_fields = access_key_held_new(&frame->held, 0, sizeof(const AccessHeaptupleColumn *),
												 _Alignof(const AccessHeaptupleColumn *));
	bool		alike;
	size_t		count = 0;

	for (size_t i = 0; i < frame->self->columns.len; i++)
	{
		const AccessHeaptupleColumn *column = &STD_VEC_AT(&frame->self->columns, AccessHeaptupleColumn, i);

		if (column->has_field)
			std_vec_push_Checked(old_fields, &column, sizeof(column), _Alignof(const AccessHeaptupleColumn *));
	}
	std_vec_new(&frame->fields, sizeof(AccessTransformField), _Alignof(AccessTransformField));
	frame->fields_live = true;
	for (size_t i = 0; i < frame->later->columns.len; i++)
	{
		const AccessHeaptupleColumn *column = &STD_VEC_AT(&frame->later->columns, AccessHeaptupleColumn, i);
		AccessTransformField field = {.typlen = column->len, .storage = access_heaptuple_Storage_byte(column->storage)};
		const AccessHeaptupleColumn *old = i < frame->self->columns.len ?
			&STD_VEC_AT(&frame->self->columns, AccessHeaptupleColumn, i) : NULL;

		count = std_Enumerate_count_step(count);
		if (!column->has_field)
			continue;
		std_vec_reserve_Checked(&frame->fields, 1, sizeof(AccessTransformField), _Alignof(AccessTransformField));
		if (old != NULL && old->has_field)
		{
			field.source.tag = ACCESS_TRANSFORM_SOURCE_OLD;
			field.source.u.old = old->field;
		}
		else
		{
			field.source.tag = ACCESS_TRANSFORM_SOURCE_MISSING;
			if (!frame->missing(frame->captures, i, &field.source.u.missing.has, &field.source.u.missing.bytes,
								frame->error))
				return;
		}
		std_vec_push_Checked(&frame->fields, &field, sizeof(field), _Alignof(AccessTransformField));
	}
	alike = frame->target == frame->later_target && frame->fields.len == old_fields->len;
	count = 0;
	for (size_t k = 0; alike && k < frame->fields.len; k++)
	{
		const AccessTransformField *field = &STD_VEC_AT(&frame->fields, AccessTransformField, k);
		const AccessHeaptupleColumn *old = STD_VEC_AT(old_fields, const AccessHeaptupleColumn *, k);

		count = std_Enumerate_count_step(count);
		alike = field->source.tag == ACCESS_TRANSFORM_SOURCE_OLD && field->source.u.old == k &&
			field->typlen == old->len && field->storage == access_heaptuple_Storage_byte(old->storage);
	}
	*frame->has_result = !alike;
	if (!alike)
	{
		frame->result->fields = frame->fields;
		frame->result->target = frame->later_target;
		frame->fields_live = false;
	}
	frame->ok = true;
}

/*
 * access_heaptuple_RowLayout_transform_to answers how rows stored under
 * this layout become rows of a later layout of the same table: each of the
 * later layout's fields from this layout's field at its attribute number,
 * or the stored bytes missing gives for its position; none when both
 * layouts store rows alike.
 */
bool
access_heaptuple_RowLayout_transform_to(const AccessHeaptupleRowLayout *self, const AccessHeaptupleRowLayout *later,
										AccessHeaptupleMissingFn missing, void *captures, uint32 target,
										uint32 later_target, bool *has_result, AccessTransformTransform *result,
										PgPgError *error)
{
	access_heaptuple_transform_frame_t frame = {.self = self, .later = later, .missing = missing, .captures = captures,
		.target = target, .later_target = later_target, .has_result = has_result, .result = result,
	.error = error};

	std_landing(&frame, access_heaptuple_RowLayout_transform_to_body, access_heaptuple_RowLayout_transform_to_pad);
	access_heaptuple_RowLayout_transform_to_pad(&frame);
	return frame.ok;
}

/*
 * access_heaptuple_RowLayout_key_starts_closure_0 is key_starts' read: whether
 * a column is in the range and its key exact.
 */
static bool
access_heaptuple_RowLayout_key_starts_closure_0(const AccessHeaptupleRowLayout *self, size_t from, size_t upto,
												size_t i)
{
	if (!(from <= i && i < upto))
		return false;
	std_index_check(i, self->exact.len, RUST_FILE, 417, 60);
	return STD_VEC_AT(&self->exact, bool, i);
}

/*
 * access_heaptuple_RowLayout_key_starts answers where each of the key's
 * columns starts in a row's key (a Vec<&[u8]>), as far as the last one the
 * columns from a position up to another can read from the key, and none
 * when none of them can. Its caller frees the vector.
 */
static bool
access_heaptuple_RowLayout_key_starts(const AccessHeaptupleRowLayout *self, const uint8 *key, size_t key_len,
									  size_t from, size_t upto, StdVec *result, PgPgError *error)
{
	size_t		last = 0;
	bool		found = false;
	StdBytes	rest = {key, key_len};
	size_t		count = 0;

	for (size_t k = self->key.len; k-- > 0;)
	{
		if (access_heaptuple_RowLayout_key_starts_closure_0(self, from, upto,
															STD_VEC_AT(&self->key, access_TupleUsizeOrderKeyType_t, k).f0))
		{
			last = k;
			found = true;
			break;
		}
	}
	if (!found)
		return true;
	std_vec_reserve_Checked(result, STD_ADD(usize, last, 1, 419, 45), sizeof(StdBytes), _Alignof(StdBytes));
	std_range_to_check(STD_ADD(usize, last, 1, 421, 43), self->key.len, RUST_FILE,
					   421, 43);
	for (size_t at = 0; at <= last; at++)
	{
		const access_TupleUsizeOrderKeyType_t *keyed = &STD_VEC_AT(&self->key, access_TupleUsizeOrderKeyType_t, at);
		bool		cut;

		count = std_Enumerate_count_step(count);
		std_vec_push_Checked(result, &rest, sizeof(StdBytes), _Alignof(StdBytes));
		if (at < last && !access_key_skip_within(rest.ptr, rest.len, &keyed->f2, keyed->f1, self->bound, &rest, &cut,
												 error))
			return false;
	}
	return true;
}

/*
 * access_heaptuple_value_frame_t is value's state across its pad: a key
 * column's decoded bytes, while set borrows them.
 */
typedef struct access_heaptuple_value_frame_t
{
	access_key_held_t held;
	AccessHeaptupleSetFn set;
	void	   *captures;
	AccessHeaptupleDeformed deformed;
	PgPgError  *error;
	bool		ok;
} access_heaptuple_value_frame_t;

/* access_heaptuple_value_body gives set the decoded value. */
static void
access_heaptuple_value_body(void *frame_)
{
	access_heaptuple_value_frame_t *frame = frame_;

	frame->ok = frame->set(frame->captures, frame->deformed, frame->error);
}

/* access_heaptuple_set_value gives set a Deformed::Value of a Set. */
static bool
access_heaptuple_set_value(AccessHeaptupleSetFn set, void *captures, AccessHeaptupleSet value, PgPgError *error)
{
	AccessHeaptupleDeformed deformed = {.tag = ACCESS_HEAPTUPLE_DEFORMED_VALUE, .u.value = value};

	return set(captures, deformed, error);
}

/* access_heaptuple_set_null gives set a NULL. */
static bool
access_heaptuple_set_null(AccessHeaptupleSetFn set, void *captures, PgPgError *error)
{
	AccessHeaptupleSet null = {.tag = ACCESS_HEAPTUPLE_SET_NULL};

	return access_heaptuple_set_value(set, captures, null, error);
}

/*
 * access_heaptuple_RowLayout_value gives set a column's value from its
 * field, at its place in a tuple that the row's count of values out of line
 * before it is given with, or from the row's key, whose columns start where
 * key_starts says, for a key column the field holds no value of.
 */
static bool
access_heaptuple_RowLayout_value(const AccessHeaptupleRowLayout *self, size_t i, EncodingTupleField field,
								 size_t place, const EncodingTupleTuple *tuple, size_t before, const StdVec *starts,
								 AccessHeaptupleSetFn set, void *captures, PgPgError *error)
{
	const AccessHeaptupleColumn *column;
	AccessHeaptupleSet value = {0};
	AccessHeaptupleDeformed deformed = {0};

	std_index_check(i, self->columns.len, RUST_FILE, 517, 23);
	column = &STD_VEC_AT(&self->columns, AccessHeaptupleColumn, i);
	std_index_check(i, self->exact.len, RUST_FILE, 518, 12);
	if (STD_VEC_AT(&self->exact, bool, i) && field.tag == ENCODING_TUPLE_FIELD_NULL)
	{
		size_t		at = 0;
		bool		found = false;
		const access_TupleUsizeOrderKeyType_t *keyed;
		bool		has;
		AccessKeyDecoded decoded;
		StdBytes	after;
		access_heaptuple_value_frame_t frame = {.set = set, .captures = captures, .error = error};

		for (; at < self->key.len && !found; at++)
			found = STD_VEC_AT(&self->key, access_TupleUsizeOrderKeyType_t, at).f0 == i;
		if (!found)
			return access_heaptuple_fail(access_heaptuple_corrupt(), error);
		at--;
		std_index_check(at, self->key.len, RUST_FILE, 520, 41);
		keyed = &STD_VEC_AT(&self->key, access_TupleUsizeOrderKeyType_t, at);
		if (at >= starts->len)
			return access_heaptuple_fail(access_heaptuple_corrupt(), error);
		if (!access_key_decode(STD_VEC_AT(starts, StdBytes, at).ptr, STD_VEC_AT(starts, StdBytes, at).len, &keyed->f2,
							   keyed->f1, &has, &decoded, &after, error))
			return false;
		if (!has)
			return access_heaptuple_set_null(set, captures, error);
		if (decoded.tag == ACCESS_KEY_DECODED_WORD)
		{
			value.tag = ACCESS_HEAPTUPLE_SET_WORD;
			value.u.word = decoded.u.word;
			return access_heaptuple_set_value(set, captures, value, error);
		}
		frame.held.vec[0] = decoded.u.bytes;
		frame.held.size[0] = frame.held.align[0] = 1;
		frame.held.live[0] = true;
		value.tag = column->len == -1 ? ACCESS_HEAPTUPLE_SET_VARLENA : ACCESS_HEAPTUPLE_SET_FIXED;
		value.u.fixed.ptr = decoded.u.bytes.ptr;
		value.u.fixed.len = decoded.u.bytes.len;
		frame.deformed.tag = ACCESS_HEAPTUPLE_DEFORMED_VALUE;
		frame.deformed.u.value = value;
		std_landing(&frame, access_heaptuple_value_body, access_key_held_pad);
		access_key_held_pad(&frame);
		return frame.ok;
	}
	switch (field.tag)
	{
		case ENCODING_TUPLE_FIELD_NULL:
			return access_heaptuple_set_null(set, captures, error);
		case ENCODING_TUPLE_FIELD_VALUE:
			if (column->by_value)
			{
				value.tag = ACCESS_HEAPTUPLE_SET_WORD;
				if (!access_heaptuple_word_of(field.u.value.ptr, field.u.value.len, &value.u.word, error))
					return false;
			}
			else
			{
				value.tag = column->len == -1 ? ACCESS_HEAPTUPLE_SET_VARLENA : ACCESS_HEAPTUPLE_SET_FIXED;
				value.u.fixed = field.u.value;
			}
			return access_heaptuple_set_value(set, captures, value, error);
		case ENCODING_TUPLE_FIELD_EXTERNAL:
			deformed.tag = ACCESS_HEAPTUPLE_DEFORMED_OUT_OF_LINE;
			deformed.u.out_of_line.length = field.u.external.length;
			deformed.u.out_of_line.address = field.u.external.address;
			deformed.u.out_of_line.at = field.u.external.at;
			deformed.u.out_of_line.value = STD_ADD(usize, before, access_heaptuple_externals(tuple, place),
												   535, 71);
			return set(captures, deformed, error);
		case ENCODING_TUPLE_FIELD_OVERFLOW:
			deformed.tag = ACCESS_HEAPTUPLE_DEFORMED_OVERFLOWED;
			if (!encoding_tuple_Tuple_overflow(tuple, &deformed.u.overflowed.address, &deformed.u.overflowed.at))
				return access_heaptuple_fail(access_heaptuple_corrupt(), error);
			deformed.u.overflowed.field = field.u.overflow;
			deformed.u.overflowed.before = STD_ADD(usize, before,
												   access_heaptuple_externals(tuple, encoding_tuple_Tuple_len(tuple)),
												   539, 72);
			return set(captures, deformed, error);
	}
	pg_unreachable();
}

/* access_heaptuple_deform_frame_t is deform's and overflowed's state across its pad: where the key's columns start. */
typedef struct access_heaptuple_deform_frame_t
{
	access_key_held_t held;
	const AccessHeaptupleRowLayout *self;
	const uint8 *key;
	size_t		key_len;
	const uint8 *tuple;
	size_t		tuple_len;
	const AccessTransformField *sources;
	size_t		sources_len;
	bool		has_sources;
	size_t		from;
	size_t		upto;
	size_t		field;
	size_t		before;
	AccessHeaptupleSetFn set;
	void	   *captures;
	bool	   *has_result;
	size_t	   *result;
	PgPgError  *error;
	bool		ok;
} access_heaptuple_deform_frame_t;

/* access_heaptuple_RowLayout_deform_body reads each column from the tuple, the key, or the transform. */
static void
access_heaptuple_RowLayout_deform_body(void *frame_)
{
	access_heaptuple_deform_frame_t *frame = frame_;
	const AccessHeaptupleRowLayout *self = frame->self;
	PgPgError  *error = frame->error;
	size_t		upto = Min(frame->upto, self->columns.len);
	EncodingTupleTuple parsed;
	EncodingTupleFields fields;
	StdVec	   *starts;
	StdBytes	overflow;
	size_t		overflow_at;

	if (!encoding_tuple_Tuple_parse(frame->tuple, frame->tuple_len, &parsed))
	{
		*error = access_heaptuple_corrupt();
		return;
	}
	fields = encoding_tuple_Tuple_fields(&parsed);
	starts = access_key_held_new(&frame->held, 0, sizeof(StdBytes), _Alignof(StdBytes));
	if (!access_heaptuple_RowLayout_key_starts(self, frame->key, frame->key_len, frame->from, upto, starts, error))
		return;
	for (size_t i = frame->from; i < upto; i++)
	{
		const AccessHeaptupleColumn *column = &STD_VEC_AT(&self->columns, AccessHeaptupleColumn, i);
		bool		has_stored = column->has_field;
		size_t		stored = column->field;
		EncodingTupleField field = {.tag = ENCODING_TUPLE_FIELD_NULL};

		if (column->dropped)
		{
			if (!access_heaptuple_set_null(frame->set, frame->captures, error))
				return;
			continue;
		}
		if (column->has_field && frame->has_sources)
		{
			const AccessTransformSource *source;

			if (column->field >= frame->sources_len)
			{
				*error = access_heaptuple_corrupt();
				return;
			}
			source = &frame->sources[column->field].source;
			if (source->tag == ACCESS_TRANSFORM_SOURCE_OLD)
				stored = source->u.old;
			else if (!source->u.missing.has)
			{
				if (!access_heaptuple_set_null(frame->set, frame->captures, error))
					return;
				continue;
			}
			else
			{
				EncodingTupleField missing = {.tag = ENCODING_TUPLE_FIELD_VALUE};

				missing.u.value.ptr = source->u.missing.bytes.ptr;
				missing.u.value.len = source->u.missing.bytes.len;
				if (!access_heaptuple_RowLayout_value(self, i, missing, 0, &parsed, 0, starts, frame->set,
													  frame->captures, error))
					return;
				continue;
			}
		}
		if (has_stored && stored >= encoding_tuple_Tuple_len(&parsed))
		{
			AccessHeaptupleDeformed missing = {.tag = ACCESS_HEAPTUPLE_DEFORMED_MISSING};

			if (!frame->set(frame->captures, missing, error))
				return;
			continue;
		}
		if (has_stored)
			field = encoding_tuple_Fields_at(&fields, stored);
		if (!access_heaptuple_RowLayout_value(self, i, field, has_stored ? stored : 0, &parsed, 0, starts, frame->set,
											  frame->captures, error))
			return;
	}
	*frame->has_result = upto == self->columns.len && !encoding_tuple_Tuple_overflow(&parsed, &overflow, &overflow_at);
	if (*frame->has_result)
		*frame->result = access_heaptuple_externals(&parsed, encoding_tuple_Tuple_len(&parsed));
	frame->ok = true;
}

/*
 * access_heaptuple_RowLayout_deform gives set the value of each of a stored
 * row's columns from a position up to another, in attribute order,
 * Postgres' slot_deform_heap_tuple, a row stored at an older epoch read
 * through the composed transform's fields since then (sources, none for
 * none). It answers the row's count of values out of line once it gave the
 * row's last column from a tuple that holds every one of them, and none
 * otherwise.
 */
bool
access_heaptuple_RowLayout_deform(const AccessHeaptupleRowLayout *self, const uint8 *key, size_t key_len,
								  const uint8 *tuple, size_t tuple_len, bool has_sources,
								  const AccessTransformField *sources, size_t sources_len, size_t from, size_t upto,
								  AccessHeaptupleSetFn set, void *captures, bool *has_result, size_t *result,
								  PgPgError *error)
{
	access_heaptuple_deform_frame_t frame = {.self = self, .key = key, .key_len = key_len, .tuple = tuple,
		.tuple_len = tuple_len, .sources = sources, .sources_len = sources_len, .has_sources = has_sources,
		.from = from, .upto = upto, .set = set, .captures = captures, .has_result = has_result, .result = result,
	.error = error};

	std_landing(&frame, access_heaptuple_RowLayout_deform_body, access_key_held_pad);
	access_key_held_pad(&frame);
	return frame.ok;
}

/* access_heaptuple_RowLayout_overflowed_body reads the column's value from the chunk's tuple. */
static void
access_heaptuple_RowLayout_overflowed_body(void *frame_)
{
	access_heaptuple_deform_frame_t *frame = frame_;
	EncodingTupleTuple parsed;
	StdVec	   *starts;
	StdBytes	overflow;
	size_t		overflow_at;

	if (!encoding_tuple_Tuple_parse(frame->tuple, frame->tuple_len, &parsed) ||
		frame->from >= frame->self->columns.len || frame->field >= encoding_tuple_Tuple_len(&parsed))
	{
		*frame->error = access_heaptuple_corrupt();
		return;
	}
	starts = access_key_held_new(&frame->held, 0, sizeof(StdBytes), _Alignof(StdBytes));
	if (!access_heaptuple_RowLayout_key_starts(frame->self, frame->key, frame->key_len, frame->from,
											   STD_ADD(usize, frame->from, 1, 501, 51), starts,
											   frame->error) ||
		!access_heaptuple_RowLayout_value(frame->self, frame->from, encoding_tuple_Tuple_field(&parsed, frame->field),
										  frame->field, &parsed, frame->before, starts, frame->set, frame->captures,
										  frame->error))
		return;
	*frame->has_result = !encoding_tuple_Tuple_overflow(&parsed, &overflow, &overflow_at);
	if (*frame->has_result)
		*frame->result = STD_ADD(usize, frame->before,
								 access_heaptuple_externals(&parsed, encoding_tuple_Tuple_len(&parsed)),
								 503, 48);
	frame->ok = true;
}

/*
 * access_heaptuple_RowLayout_overflowed gives set the value of a column held
 * in an overflow chunk, from the chunk's tuple as the caller read it, as
 * deform gives a value. It answers the row's count of values out of line
 * when the chunk is the row's last, and none otherwise.
 */
bool
access_heaptuple_RowLayout_overflowed(const AccessHeaptupleRowLayout *self, const uint8 *key, size_t key_len,
									  size_t column, const uint8 *chunk, size_t chunk_len, size_t field,
									  size_t before, AccessHeaptupleSetFn set, void *captures, bool *has_result,
									  size_t *result, PgPgError *error)
{
	access_heaptuple_deform_frame_t frame = {.self = self, .key = key, .key_len = key_len, .tuple = chunk,
		.tuple_len = chunk_len, .from = column, .field = field, .before = before, .set = set,
	.captures = captures, .has_result = has_result, .result = result, .error = error};

	std_landing(&frame, access_heaptuple_RowLayout_overflowed_body, access_key_held_pad);
	access_key_held_pad(&frame);
	return frame.ok;
}

/* access_heaptuple_unformed returns the error of a row's values that don't fit its table's columns. */
static PgPgError
access_heaptuple_unformed(void)
{
	return access_heaptuple_fork("a row's values don't fit its table's columns");
}

/*
 * access_heaptuple_field_of answers the bytes a value of a column keeps as a
 * tuple's field: none for NULL, a by-value datum's first bytes of its word,
 * and a fixed-length or variable-length value's own bytes, refusing a value
 * that doesn't fit the column's type.
 */
bool
access_heaptuple_field_of(const AccessHeaptupleColumn *column, const AccessHeaptupleSet *value,
						  access_OptionCowBytes_t *result, PgPgError *error)
{
	memset(result, 0, sizeof(*result));
	switch (value->tag)
	{
		case ACCESS_HEAPTUPLE_SET_NULL:
			return true;
		case ACCESS_HEAPTUPLE_SET_WORD:
			if (column->by_value)
			{
				uint8		le[8];

				if (column->len < 0 || column->len > 8)
					return access_heaptuple_fail(access_heaptuple_unformed(), error);
				for (int i = 0; i < 8; i++)
					le[i] = (uint8) (value->u.word >> (8 * i));
				std_range_to_check((size_t) column->len, sizeof(le), RUST_FILE, 680, 29);
				std_slice_to_vec_Checked(&result->bytes, le, (size_t) column->len, ACCESS_HEAPTUPLE_BYTES);
				result->has = result->owned = true;
				return true;
			}
			break;
		case ACCESS_HEAPTUPLE_SET_FIXED:
			if (!column->by_value && column->len > 0)
			{
				result->has = true;
				result->borrowed = value->u.fixed;
				return true;
			}
			break;
		case ACCESS_HEAPTUPLE_SET_VARLENA:
			if (column->len == -1)
			{
				result->has = true;
				result->borrowed = value->u.varlena;
				return true;
			}
			break;
	}
	return access_heaptuple_fail(access_heaptuple_unformed(), error);
}

/*
 * access_heaptuple_RowLayout_toasted_closure_2 is toasted's biggest: the
 * biggest movable value of some storage kinds a move can take, or none.
 */
static bool
access_heaptuple_RowLayout_toasted_closure_2(const AccessHeaptupleRowLayout *self, const AccessHeaptupleSet *values,
											 size_t values_len, const StdVec *moved, const StdVec *at,
											 bool (*movable) (const void *captures, size_t i), const void *captures,
											 const AccessHeaptupleStorage *kinds, size_t kinds_len, size_t *result)
{
	bool		found = false;
	size_t		biggest_length = 0;

	for (size_t i = 0; i < self->columns.len; i++)
	{
		bool		kind = false;
		size_t		length;

		std_index_check(i, moved->len, RUST_FILE, 648, 31);
		if (STD_VEC_AT(moved, bool, i))
			continue;
		std_index_check(i, at->len, RUST_FILE, 648, 43);
		if (!STD_VEC_AT(at, StdUsizeOption, i).has || !movable(captures, i))
			continue;
		std_index_check(i, self->columns.len, RUST_FILE, 648, 92);
		for (size_t k = 0; k < kinds_len && !kind; k++)
			kind = kinds[k] == STD_VEC_AT(&self->columns, AccessHeaptupleColumn, i).storage;
		if (!kind)
			continue;
		std_index_check(i, values_len, RUST_FILE, 649, 39);
		if (values[i].tag != ACCESS_HEAPTUPLE_SET_VARLENA)
			continue;
		length = values[i].u.varlena.len;
		if (STD_ADD(usize, ACCESS_HEAPTUPLE_VARHDRSZ, length, 650, 44) <=
			std_next_multiple_of_usize(ACCESS_HEAPTUPLE_TOAST_POINTER_LEN, ACCESS_HEAPTUPLE_MAXALIGN) ||
			!access_heaptuple_is_movable(-1, length))
			continue;
		if (!found || length > biggest_length)
		{
			found = true;
			*result = i;
			biggest_length = length;
		}
	}
	return found;
}

/*
 * access_heaptuple_toasted_t is what toasted makes: the fields of its first
 * kind's moves, and each column heap holds out of line.
 */
typedef struct access_heaptuple_toasted_t
{
	StdVec		forced;
	StdVec		moved;
} access_heaptuple_toasted_t;

/* ACCESS_HEAPTUPLE_EXTENDED_KINDS and ACCESS_HEAPTUPLE_MAIN_KINDS are the storage kinds of toasting's two passes. */
static const AccessHeaptupleStorage ACCESS_HEAPTUPLE_EXTENDED_KINDS[] = {ACCESS_HEAPTUPLE_STORAGE_EXTENDED,
ACCESS_HEAPTUPLE_STORAGE_EXTERNAL};
static const AccessHeaptupleStorage ACCESS_HEAPTUPLE_MAIN_KINDS[] = {ACCESS_HEAPTUPLE_STORAGE_MAIN};

/*
 * access_heaptuple_RowLayout_toasted makes the moves heap_toast_insert_or_
 * update makes on the row's heap tuple, uncompressed, into vectors its
 * caller made empty and frees on an unwind; at holds each column's field
 * (an Option<usize>), which it fills.
 */
static void
access_heaptuple_RowLayout_toasted(const AccessHeaptupleRowLayout *self, const AccessHeaptupleSet *values,
								   size_t values_len, const AccessHeaptupleKept *kept, size_t kept_len,
								   bool external, bool (*movable) (const void *captures, size_t i),
								   const void *captures, size_t target, StdVec *at, access_heaptuple_toasted_t *result)
{
	uint64		fields_at = 0;
	size_t		i;

	std_vec_reserve_Checked(&result->moved, self->columns.len, ACCESS_HEAPTUPLE_BOOL);
	for (size_t c = 0; c < self->columns.len; c++)
	{
		bool		any = false;

		for (size_t k = 0; k < kept_len && !any; k++)
			any = kept[k].column == c;
		std_vec_push_Checked(&result->moved, &any, ACCESS_HEAPTUPLE_BOOL);
	}
	if (!external && kept_len == 0 &&
		access_heaptuple_heap_len(&self->columns, values, values_len, result->moved.ptr, result->moved.len) <=
		ACCESS_HEAPTUPLE_TOAST_TUPLE_THRESHOLD)
		return;
	std_vec_reserve_Checked(at, self->columns.len, ACCESS_HEAPTUPLE_LENGTH);
	for (size_t c = 0; c < self->columns.len; c++)
	{
		StdUsizeOption field = {STD_VEC_AT(&self->columns, AccessHeaptupleColumn, c).has_field, 0};

		if (field.has)
		{
			field.x = (size_t) fields_at;
			fields_at = std_forward_u64_Checked(fields_at, 1);
		}
		std_vec_push_Checked(at, &field, ACCESS_HEAPTUPLE_LENGTH);
	}
	while (access_heaptuple_heap_len(&self->columns, values, values_len, result->moved.ptr, result->moved.len) > target)
	{
		if (!access_heaptuple_RowLayout_toasted_closure_2(self, values, values_len, &result->moved, at, movable, captures,
														  ACCESS_HEAPTUPLE_EXTENDED_KINDS, lengthof(ACCESS_HEAPTUPLE_EXTENDED_KINDS),
														  &i))
			break;
		std_index_check(i, result->moved.len, RUST_FILE, 660, 13);
		STD_VEC_AT(&result->moved, bool, i) = true;
		std_index_check(i, at->len, RUST_FILE, 661, 27);
		if (STD_VEC_AT(at, StdUsizeOption, i).has)
			std_vec_push_Checked(&result->forced, &STD_VEC_AT(at, StdUsizeOption, i).x, ACCESS_HEAPTUPLE_USIZE);
	}
	while (access_heaptuple_heap_len(&self->columns, values, values_len, result->moved.ptr, result->moved.len) >
		   ACCESS_HEAPTUPLE_TOAST_TUPLE_TARGET_MAIN)
	{
		if (!access_heaptuple_RowLayout_toasted_closure_2(self, values, values_len, &result->moved, at, movable, captures,
														  ACCESS_HEAPTUPLE_MAIN_KINDS, lengthof(ACCESS_HEAPTUPLE_MAIN_KINDS), &i))
			break;
		std_index_check(i, result->moved.len, RUST_FILE, 665, 13);
		STD_VEC_AT(&result->moved, bool, i) = true;
	}
}

/* access_heaptuple_placing_t is placing's state across its pad: everything it makes before the placing takes it. */
typedef struct access_heaptuple_placing_t
{
	const AccessHeaptupleRowLayout *self;
	AccessKeySortKeys sorter;
	AccessKeyNames names;
	const AccessHeaptupleSet *values;
	size_t		values_len;
	const AccessHeaptupleKept *kept;
	size_t		kept_len;
	bool		external;
	size_t		target;
	StdVec		key;
	StdVec		cut;
	StdVec		fields;
	StdVec		outside;
	StdVec		typlens;
	StdVec		storages;
	StdVec		at;
	access_heaptuple_toasted_t toasted;
	AccessHeaptuplePlan plan;
	StdVec		moved;
	bool		live;
	AccessHeaptuplePlacing *result;
	PgPgError  *error;
	bool		ok;
} access_heaptuple_placing_t;

/* access_heaptuple_placing_pad frees everything placing made. */
static void
access_heaptuple_placing_pad(void *frame_)
{
	access_heaptuple_placing_t *frame = frame_;

	if (!frame->live)
		return;
	std_vec_free_Checked(&frame->key, ACCESS_HEAPTUPLE_BYTES);
	std_vec_free_Checked(&frame->cut, ACCESS_HEAPTUPLE_BOOL);
	std_vec_drop_Checked(&frame->fields, ACCESS_HEAPTUPLE_FIELD, access_heaptuple_field_drop);
	std_vec_free_Checked(&frame->outside, ACCESS_HEAPTUPLE_BOOL);
	std_vec_free_Checked(&frame->typlens, sizeof(int16), _Alignof(int16));
	std_vec_free_Checked(&frame->storages, sizeof(AccessHeaptupleStorage), _Alignof(AccessHeaptupleStorage));
	std_vec_free_Checked(&frame->at, ACCESS_HEAPTUPLE_LENGTH);
	std_vec_free_Checked(&frame->toasted.forced, ACCESS_HEAPTUPLE_USIZE);
	std_vec_free_Checked(&frame->toasted.moved, ACCESS_HEAPTUPLE_BOOL);
	access_heaptuple_Plan_drop(&frame->plan);
	std_vec_free_Checked(&frame->moved, ACCESS_HEAPTUPLE_BOOL);
	frame->live = false;
}

/*
 * access_heaptuple_RowLayout_placing_closure_3 is placing's movable: whether a
 * column's value may move out of line, its key not holding it whole.
 */
static bool
access_heaptuple_RowLayout_placing_closure_3(const void *captures, size_t i)
{
	const access_heaptuple_placing_t *frame = captures;

	std_index_check(i, frame->self->exact.len, RUST_FILE, 600, 35);
	if (!STD_VEC_AT(&frame->self->exact, bool, i))
		return true;
	std_index_check(i, frame->cut.len, RUST_FILE, 600, 52);
	return STD_VEC_AT(&frame->cut, bool, i);
}


/* access_heaptuple_RowLayout_placing_body keys the row, makes its fields, plans their moves, and starts its placing. */
static void
access_heaptuple_RowLayout_placing_body(void *frame_)
{
	access_heaptuple_placing_t *frame = frame_;
	const AccessHeaptupleRowLayout *self = frame->self;
	PgPgError  *error = frame->error;
	bool		compared = false;
	bool		no = false;
	size_t		key_len;
	size_t		room;
	uint64		fields_at = 0;
	AccessHeaptupleKind kinds[1];
	StdVec		outside;
	size_t		heap;

	for (size_t k = 0; k < self->key.len && !compared; k++)
		compared = STD_VEC_AT(&self->key, access_TupleUsizeOrderKeyType_t, k).f2.inner.tag == ACCESS_KEY_INNER_COMPARED;
	std_vec_resize(&frame->cut, self->columns.len, &no, ACCESS_HEAPTUPLE_BOOL);
	for (size_t k = 0; k < self->key.len; k++)
	{
		const access_TupleUsizeOrderKeyType_t *keyed = &STD_VEC_AT(&self->key, access_TupleUsizeOrderKeyType_t, k);
		const AccessHeaptupleSet *set;
		AccessKeyValue value = {0};
		bool		cut;

		std_index_check(keyed->f0, frame->values_len, RUST_FILE, 571, 31);
		set = &frame->values[keyed->f0];
		if (set->tag == ACCESS_HEAPTUPLE_SET_WORD)
		{
			value.tag = ACCESS_KEY_VALUE_WORD;
			value.u.word = set->u.word;
		}
		else if (set->tag != ACCESS_HEAPTUPLE_SET_NULL)
		{
			value.tag = ACCESS_KEY_VALUE_BYTES;
			value.u.bytes = set->u.fixed;
		}
		if (!access_key_encode_within(&frame->key, frame->sorter, frame->names, &keyed->f2,
									  set->tag != ACCESS_HEAPTUPLE_SET_NULL, value, keyed->f1, self->bound, compared,
									  &cut, error))
			return;
		std_index_check(keyed->f0, frame->cut.len, RUST_FILE, 576, 13);
		STD_VEC_AT(&frame->cut, bool, keyed->f0) = cut;
	}
	std_vec_reserve_Checked(&frame->fields, frame->values_len, ACCESS_HEAPTUPLE_FIELD);
	std_vec_reserve_Checked(&frame->outside, frame->values_len, ACCESS_HEAPTUPLE_BOOL);
	std_vec_reserve_Checked(&frame->typlens, frame->values_len, sizeof(int16), _Alignof(int16));
	std_vec_reserve_Checked(&frame->storages, frame->values_len, sizeof(AccessHeaptupleStorage),
							_Alignof(AccessHeaptupleStorage));
	for (size_t i = 0; i < Min(self->columns.len, frame->values_len); i++)
	{
		const AccessHeaptupleColumn *column = &STD_VEC_AT(&self->columns, AccessHeaptupleColumn, i);
		const AccessHeaptupleKept *kept = NULL;
		access_OptionCowBytes_t field = {0};
		bool		out_of_line;

		if (!column->has_field)
			continue;
		std_vec_push_Checked(&frame->typlens, &column->len, sizeof(int16), _Alignof(int16));
		std_vec_push_Checked(&frame->storages, &column->storage, sizeof(AccessHeaptupleStorage),
							 _Alignof(AccessHeaptupleStorage));
		for (size_t k = 0; k < frame->kept_len && kept == NULL; k++)
			if (frame->kept[k].column == i)
				kept = &frame->kept[k];
		if (kept != NULL)
		{
			field.has = field.owned = true;
			encoding_tuple_reference(kept->length, kept->address, &field.bytes);
			std_vec_push_Checked(&frame->fields, &field, ACCESS_HEAPTUPLE_FIELD);
			out_of_line = true;
			std_vec_push_Checked(&frame->outside, &out_of_line, ACCESS_HEAPTUPLE_BOOL);
			continue;
		}
		out_of_line = false;
		std_vec_push_Checked(&frame->outside, &out_of_line, ACCESS_HEAPTUPLE_BOOL);
		std_index_check(i, self->exact.len, RUST_FILE, 593, 31);
		if (STD_VEC_AT(&self->exact, bool, i))
			std_index_check(i, frame->cut.len, RUST_FILE, 593, 49);
		if (!(STD_VEC_AT(&self->exact, bool, i) && !STD_VEC_AT(&frame->cut, bool, i)) &&
			!access_heaptuple_field_of(column, &frame->values[i], &field, error))
			return;
		std_vec_push_Checked(&frame->fields, &field, ACCESS_HEAPTUPLE_FIELD);
	}
	key_len = self->key.len == 0 ? ACCESS_HEAPTUPLE_KEYLESS_KEY_LEN + ACCESS_HEAPTUPLE_MAX_COUNT_LEN : frame->key.len;
	room = std_saturating_sub_usize(CHUNKS_MAX_ENTRY, STD_ADD(usize, key_len, ACCESS_HEAPTUPLE_ENTRY_OVERHEAD,
															  599, 45));
	access_heaptuple_RowLayout_toasted(self, frame->values, frame->values_len, frame->kept, frame->kept_len,
									   frame->external, access_heaptuple_RowLayout_placing_closure_3, frame, frame->target,
									   &frame->at, &frame->toasted);
	for (size_t i = 0; i < self->columns.len; i++)
	{
		size_t		at;
		bool		listed = false;

		if (!STD_VEC_AT(&self->columns, AccessHeaptupleColumn, i).has_field)
			continue;
		at = (size_t) fields_at;
		fields_at = std_forward_u64_Checked(fields_at, 1);
		if (!compared)
			continue;
		std_index_check(i, frame->cut.len, RUST_FILE, 605, 28);
		if (!STD_VEC_AT(&frame->cut, bool, i))
			continue;
		for (size_t f = 0; f < frame->toasted.forced.len && !listed; f++)
			listed = STD_VEC_AT(&frame->toasted.forced, size_t, f) == at;
		if (listed)
			continue;
		std_index_check(at, frame->outside.len, RUST_FILE, 605, 64);
		if (!STD_VEC_AT(&frame->outside, bool, at))
			std_vec_push_Checked(&frame->toasted.forced, &at, ACCESS_HEAPTUPLE_USIZE);
	}
	kinds[0].movable = ACCESS_HEAPTUPLE_MAIN_KINDS;
	kinds[0].movable_len = lengthof(ACCESS_HEAPTUPLE_MAIN_KINDS);
	kinds[0].target = room;
	std_vec_clone(&outside, &frame->outside, ACCESS_HEAPTUPLE_BOOL);
	frame->plan.external = outside;
	access_heaptuple_Plan_new(frame->fields.ptr, frame->fields.len, frame->typlens.ptr, frame->typlens.len,
							  frame->storages.ptr, frame->storages.len, frame->toasted.forced.ptr,
							  frame->toasted.forced.len, room, kinds, lengthof(kinds), room, &frame->plan);
	fields_at = 0;
	std_vec_reserve_Checked(&frame->moved, Min(self->columns.len, frame->toasted.moved.len), ACCESS_HEAPTUPLE_BOOL);
	for (size_t i = 0; i < Min(self->columns.len, frame->toasted.moved.len); i++)
	{
		bool		moved = false;

		if (STD_VEC_AT(&self->columns, AccessHeaptupleColumn, i).has_field)
		{
			size_t		at = (size_t) fields_at;

			fields_at = std_forward_u64_Checked(fields_at, 1);
			std_index_check(at, frame->plan.external.len, RUST_FILE, 617, 77);
			moved = STD_VEC_AT(&frame->plan.external, bool, at);
		}
		moved = moved || STD_VEC_AT(&frame->toasted.moved, bool, i);
		std_vec_push_Checked(&frame->moved, &moved, ACCESS_HEAPTUPLE_BOOL);
	}
	heap = STD_ADD(usize, std_next_multiple_of_usize(access_heaptuple_heap_len(&self->columns, frame->values,
																			   frame->values_len, frame->moved.ptr,
																			   frame->moved.len),
													 ACCESS_HEAPTUPLE_MAXALIGN),
				   ACCESS_HEAPTUPLE_ITEM_ID_LEN, 620, 27);
	frame->ok = access_heaptuple_Placing_new(&frame->key, self->key.len == 0, &frame->fields, &frame->outside,
											 &frame->plan, (uint64) heap, frame->result, error);
}

/*
 * access_heaptuple_RowLayout_placing returns the placing of a row of
 * values, one for each column in attribute order, in the table's tree,
 * Postgres' heap_form_tuple and heap_toast_insert_or_update: its tuple
 * holds each column's value the key doesn't hold exactly, and its key is
 * its primary key columns' keys, each cut at the layout's bound, or the
 * start of its tuple's hash for a table without a primary key.
 */
bool
access_heaptuple_RowLayout_placing(const AccessHeaptupleRowLayout *self, AccessKeySortKeys sorter,
								   AccessKeyNames names, const AccessHeaptupleSet *values, size_t values_len,
								   const AccessHeaptupleKept *kept, size_t kept_len, bool external, size_t target,
								   AccessHeaptuplePlacing *result, PgPgError *error)
{
	access_heaptuple_placing_t frame = {.self = self, .sorter = sorter, .names = names, .values = values,
		.values_len = values_len, .kept = kept, .kept_len = kept_len, .external = external, .target = target,
	.result = result, .error = error};

	if (values_len != self->columns.len)
		return access_heaptuple_fail(access_heaptuple_unformed(), error);
	std_vec_new(&frame.key, ACCESS_HEAPTUPLE_BYTES);
	std_vec_new(&frame.cut, ACCESS_HEAPTUPLE_BOOL);
	std_vec_new(&frame.fields, ACCESS_HEAPTUPLE_FIELD);
	std_vec_new(&frame.outside, ACCESS_HEAPTUPLE_BOOL);
	std_vec_new(&frame.typlens, sizeof(int16), _Alignof(int16));
	std_vec_new(&frame.storages, sizeof(AccessHeaptupleStorage), _Alignof(AccessHeaptupleStorage));
	std_vec_new(&frame.at, ACCESS_HEAPTUPLE_LENGTH);
	std_vec_new(&frame.toasted.forced, ACCESS_HEAPTUPLE_USIZE);
	std_vec_new(&frame.toasted.moved, ACCESS_HEAPTUPLE_BOOL);
	std_vec_new(&frame.plan.lengths, ACCESS_HEAPTUPLE_LENGTH);
	std_vec_new(&frame.plan.external, ACCESS_HEAPTUPLE_BOOL);
	std_vec_new(&frame.plan.moves, ACCESS_HEAPTUPLE_USIZE);
	std_vec_new(&frame.plan.levels, ACCESS_HEAPTUPLE_LEVEL);
	std_vec_new(&frame.moved, ACCESS_HEAPTUPLE_BOOL);
	frame.live = true;
	std_landing(&frame, access_heaptuple_RowLayout_placing_body, access_heaptuple_placing_pad);
	access_heaptuple_placing_pad(&frame);
	return frame.ok;
}

/* access_heaptuple_Placing_drop frees what a placing holds. */
void
access_heaptuple_Placing_drop(AccessHeaptuplePlacing *self)
{
	std_vec_free_Checked(&self->key, ACCESS_HEAPTUPLE_BYTES);
	std_vec_free_Checked(&self->kept, ACCESS_HEAPTUPLE_BOOL);
	std_vec_drop_Checked(&self->fields, ACCESS_HEAPTUPLE_FIELD, access_heaptuple_field_drop);
	std_vec_free_Checked(&self->external, ACCESS_HEAPTUPLE_BOOL);
	std_vec_free_Checked(&self->locators, ACCESS_HEAPTUPLE_LOCATOR);
	std_vec_free_Checked(&self->moves, ACCESS_HEAPTUPLE_USIZE);
	std_vec_free_Checked(&self->levels, ACCESS_HEAPTUPLE_LEVEL);
	if (self->has_chunk)
	{
		std_vec_free_Checked(&self->chunk_node, ACCESS_HEAPTUPLE_BYTES);
		std_vec_free_Checked(&self->chunk_locators, ACCESS_HEAPTUPLE_LOCATOR);
	}
	self->has_chunk = false;
}

/* access_heaptuple_Formed_drop frees a formed row's key, tuple, and addresses. */
void
access_heaptuple_Formed_drop(AccessHeaptupleFormed *self)
{
	std_vec_free_Checked(&self->key, ACCESS_HEAPTUPLE_BYTES);
	std_vec_free_Checked(&self->tuple, ACCESS_HEAPTUPLE_BYTES);
	std_vec_free_Checked(&self->addresses, ACCESS_HEAPTUPLE_ADDRESS);
}

/* access_heaptuple_internal returns an internal error of a row's placing. */
static PgPgError
access_heaptuple_internal(const char *message)
{
	return access_heaptuple_fork(message);
}

/* access_heaptuple_placed_t is Placed: a tuple and where it holds addresses (a Vec<(u32, Locator)>). */
typedef struct access_heaptuple_placed_t
{
	StdVec		tuple;
	StdVec		addresses;
} access_heaptuple_placed_t;

/*
 * access_heaptuple_write_frame_t is write's state across its pad: the fields
 * it borrows, the tuple, and its addresses.
 */
typedef struct access_heaptuple_write_frame_t
{
	access_key_held_t held;
	const AccessHeaptuplePlacing *self;
	size_t		level;
	access_heaptuple_placed_t *result;
	PgPgError  *error;
	bool		ok;
} access_heaptuple_write_frame_t;

/*
 * ACCESS_HEAPTUPLE_WRITE_FIELDS, ACCESS_HEAPTUPLE_WRITE_TUPLE, and
 * ACCESS_HEAPTUPLE_WRITE_ADDRESSES are write's slots of what it holds.
 */
#define ACCESS_HEAPTUPLE_WRITE_FIELDS 0
#define ACCESS_HEAPTUPLE_WRITE_TUPLE 1
#define ACCESS_HEAPTUPLE_WRITE_ADDRESSES 2

/* access_heaptuple_Placing_write_closure_3 is write's offset: a tuple's place of an address as a u32. */
static bool
access_heaptuple_Placing_write_closure_3(size_t at, uint32 *result, PgPgError *error)
{
	if (at > PG_UINT32_MAX)
		return access_heaptuple_fail(access_heaptuple_internal("a tuple's address is past its offsets"), error);
	*result = (uint32) at;
	return true;
}

/* access_heaptuple_Placing_write_body writes the level's tuple and finds where it holds addresses. */
static void
access_heaptuple_Placing_write_body(void *frame_)
{
	access_heaptuple_write_frame_t *frame = frame_;
	const AccessHeaptuplePlacing *self = frame->self;
	const access_TupleUsizeUsize_t *level;
	size_t		from;
	size_t		inline_;
	size_t		end;
	bool		has_below;
	StdVec	   *fields;
	encoding_tuple_write_overflow_t rest = {0};
	StdVec	   *tuple;
	EncodingTupleTuple parsed;
	StdVec	   *addresses;
	StdBytes	overflow;
	size_t		overflow_at;

	std_index_check(frame->level, self->levels.len, RUST_FILE, 787, 30);
	level = &STD_VEC_AT(&self->levels, access_TupleUsizeUsize_t, frame->level);
	from = level->f0;
	inline_ = level->f1;
	has_below = STD_ADD(usize, frame->level, 1, 788, 27) < self->levels.len;
	if (has_below && !self->has_overflow)
	{
		*frame->error = access_heaptuple_internal("an overflow chunk wasn't written");
		return;
	}
	end = STD_ADD(usize, from, inline_, 792, 60);
	std_range_check(from, end, self->fields.len, RUST_FILE, 792, 42);
	fields = access_key_held_new(&frame->held, ACCESS_HEAPTUPLE_WRITE_FIELDS, sizeof(StdBytes), _Alignof(StdBytes));
	std_vec_reserve_Checked(fields, inline_, sizeof(StdBytes), _Alignof(StdBytes));
	for (size_t i = from; i < end; i++)
	{
		const access_OptionCowBytes_t *field = &STD_VEC_AT(&self->fields, access_OptionCowBytes_t, i);
		StdBytes	bytes = access_heaptuple_field_bytes(field);

		if (!field->has)
			bytes.ptr = NULL;
		std_vec_push_Checked(fields, &bytes, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (has_below)
	{
		rest.f0 = self->overflow_address.f0;
		rest.f1 = STD_SUB(usize, self->fields.len, from, 793, 58);
	}
	end = STD_ADD(usize, from, inline_, 794, 78);
	std_range_check(from, end, self->external.len, RUST_FILE, 794, 58);
	tuple = access_key_held_new(&frame->held, ACCESS_HEAPTUPLE_WRITE_TUPLE, ACCESS_HEAPTUPLE_BYTES);
	encoding_tuple_write_with_external(fields->ptr, fields->len, &STD_VEC_AT(&self->external, bool, from), inline_,
									   has_below, rest, tuple);
	if (!encoding_tuple_Tuple_parse(tuple->ptr, tuple->len, &parsed))
	{
		*frame->error = access_heaptuple_internal("a tuple just written doesn't parse");
		return;
	}
	addresses = access_key_held_new(&frame->held, ACCESS_HEAPTUPLE_WRITE_ADDRESSES, ACCESS_HEAPTUPLE_ADDRESS);
	for (size_t i = 0; i < inline_; i++)
	{
		EncodingTupleField field = encoding_tuple_Tuple_field(&parsed, i);
		chunks_TupleU32Locator_t address;
		size_t		at;

		if (field.tag != ENCODING_TUPLE_FIELD_EXTERNAL)
			continue;
		if (!access_heaptuple_Placing_write_closure_3(field.u.external.at, &address.f0, frame->error))
			return;
		at = STD_ADD(usize, from, i, 800, 60);
		std_index_check(at, self->locators.len, RUST_FILE, 800, 46);
		address.f1 = STD_VEC_AT(&self->locators, ChunksTrailerLocator, at);
		std_vec_push_Checked(addresses, &address, ACCESS_HEAPTUPLE_ADDRESS);
	}
	if (encoding_tuple_Tuple_overflow(&parsed, &overflow, &overflow_at) && has_below)
	{
		chunks_TupleU32Locator_t address = {.f1 = self->overflow_locator};

		if (!access_heaptuple_Placing_write_closure_3(overflow_at, &address.f0, frame->error))
			return;
		std_vec_push_Checked(addresses, &address, ACCESS_HEAPTUPLE_ADDRESS);
	}
	frame->result->tuple = access_key_held_take(&frame->held, ACCESS_HEAPTUPLE_WRITE_TUPLE);
	frame->result->addresses = access_key_held_take(&frame->held, ACCESS_HEAPTUPLE_WRITE_ADDRESSES);
	frame->ok = true;
}

/*
 * access_heaptuple_Placing_write answers a level's tuple, the row's or an
 * overflow chunk's, with the address of the chunk below it when it has one,
 * and where it holds addresses, with their locators.
 */
static bool
access_heaptuple_Placing_write(const AccessHeaptuplePlacing *self, size_t level, access_heaptuple_placed_t *result,
							   PgPgError *error)
{
	access_heaptuple_write_frame_t frame = {.self = self, .level = level, .result = result, .error = error};

	std_landing(&frame, access_heaptuple_Placing_write_body, access_key_held_pad);
	access_key_held_pad(&frame);
	return frame.ok;
}

/*
 * access_heaptuple_advance_frame_t is advance's state across its pad: the
 * level's tuple and its addresses' offsets and locators.
 */
typedef struct access_heaptuple_advance_frame_t
{
	access_key_held_t held;
	AccessHeaptuplePlacing *self;
	size_t		level;
	PgPgError  *error;
	bool		ok;
} access_heaptuple_advance_frame_t;

/* access_heaptuple_Placing_advance_body writes the level's tuple as a blob leaf's node. */
static void
access_heaptuple_Placing_advance_body(void *frame_)
{
	access_heaptuple_advance_frame_t *frame = frame_;
	access_heaptuple_placed_t placed;
	ChunksHeaderHeader header = {.kind = CHUNKS_HEADER_BLOB_LEAF};
	StdVec	   *offsets;
	StdVec	   *locators;
	StdVec	   *node;

	if (!access_heaptuple_Placing_write(frame->self, frame->level, &placed, frame->error))
		return;
	frame->held.vec[0] = placed.tuple;
	frame->held.size[0] = frame->held.align[0] = 1;
	frame->held.live[0] = true;
	frame->held.vec[1] = placed.addresses;
	frame->held.size[1] = sizeof(chunks_TupleU32Locator_t);
	frame->held.align[1] = _Alignof(chunks_TupleU32Locator_t);
	frame->held.live[1] = true;
	offsets = access_key_held_new(&frame->held, 2, sizeof(uint32), _Alignof(uint32));
	locators = access_key_held_new(&frame->held, 3, ACCESS_HEAPTUPLE_LOCATOR);
	for (size_t i = 0; i < placed.addresses.len; i++)
	{
		const chunks_TupleU32Locator_t *address = &STD_VEC_AT(&placed.addresses, chunks_TupleU32Locator_t, i);

		std_vec_push_Checked(offsets, &address->f0, sizeof(uint32), _Alignof(uint32));
		std_vec_push_Checked(locators, &address->f1, ACCESS_HEAPTUPLE_LOCATOR);
	}
	std_vec_free_Checked(&frame->held.vec[1], sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	frame->held.live[1] = false;
	header.addresses.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS;
	header.addresses.u.offsets = *offsets;
	node = access_key_held_new(&frame->held, 1, ACCESS_HEAPTUPLE_BYTES);
	chunks_header_Header_write(&header, frame->held.vec[0].ptr, frame->held.vec[0].len, node);
	frame->self->chunk_node = access_key_held_take(&frame->held, 1);
	frame->self->chunk_locators = access_key_held_take(&frame->held, 3);
	frame->self->has_chunk = true;
	frame->ok = true;
}

/*
 * access_heaptuple_Placing_advance readies the chunk asked for next, once
 * the moved values are written: the innermost chunk not yet written, whose
 * own overflow chunk, when it has one, was written last.
 */
static bool
access_heaptuple_Placing_advance(AccessHeaptuplePlacing *self, PgPgError *error)
{
	access_heaptuple_advance_frame_t frame = {.self = self, .error = error};
	size_t		written;
	size_t		levels;

	if (self->has_chunk)
	{
		std_vec_free_Checked(&self->chunk_node, ACCESS_HEAPTUPLE_BYTES);
		std_vec_free_Checked(&self->chunk_locators, ACCESS_HEAPTUPLE_LOCATOR);
	}
	self->has_chunk = false;
	if (!std_checked_sub_usize(self->step, self->moves.len, &written))
		return true;
	levels = STD_SUB(usize, self->levels.len, 1, 774, 28);
	if (!std_checked_sub_usize(levels, written, &frame.level) || frame.level == 0)
		return true;
	std_landing(&frame, access_heaptuple_Placing_advance_body, access_key_held_pad);
	access_key_held_pad(&frame);
	return frame.ok;
}

/*
 * access_heaptuple_Placing_new returns the placing of a tuple's fields by
 * its plan, with the key it is stored under, whether that key is a keyless
 * row's, whether each field is a value an update kept out of line, and the
 * room the row takes on heap pages, and asks for its first blob. It takes
 * the key, fields, kept flags, and plan, leaving them empty.
 */
bool
access_heaptuple_Placing_new(StdVec *key, bool keyless, StdVec *fields, StdVec *kept, AccessHeaptuplePlan *plan,
							 uint64 heap_bytes, AccessHeaptuplePlacing *result, PgPgError *error)
{
	ChunksTrailerLocator none = {0};
	size_t		all = fields->len;

	memset(result, 0, sizeof(*result));
	result->key = *key;
	std_vec_new(key, ACCESS_HEAPTUPLE_BYTES);
	result->keyless = keyless;
	result->kept = *kept;
	std_vec_new(kept, ACCESS_HEAPTUPLE_BOOL);
	result->fields = *fields;
	std_vec_new(fields, ACCESS_HEAPTUPLE_FIELD);
	result->external = plan->external;
	std_vec_new(&plan->external, ACCESS_HEAPTUPLE_BOOL);
	result->moves = plan->moves;
	std_vec_new(&plan->moves, ACCESS_HEAPTUPLE_USIZE);
	result->levels = plan->levels;
	std_vec_new(&plan->levels, ACCESS_HEAPTUPLE_LEVEL);
	std_vec_new(&result->locators, ACCESS_HEAPTUPLE_LOCATOR);
	result->heap_bytes = heap_bytes;
	std_vec_resize(&result->locators, all, &none, ACCESS_HEAPTUPLE_LOCATOR);
	if (!access_heaptuple_Placing_advance(result, error))
	{
		access_heaptuple_Placing_drop(result);
		return false;
	}
	return true;
}

/*
 * access_heaptuple_Placing_field answers the position among the row's fields
 * of the value whose blob is asked for next, or none.
 */
bool
access_heaptuple_Placing_field(const AccessHeaptuplePlacing *self, size_t *result)
{
	if (self->step >= self->moves.len)
		return false;
	*result = STD_VEC_AT(&self->moves, size_t, self->step);
	return true;
}

/*
 * access_heaptuple_Placing_kept answers, for each of the row's values out of
 * line in its fields' order, whether the update kept it (a Vec<bool>).
 */
void
access_heaptuple_Placing_kept(const AccessHeaptuplePlacing *self, StdVec *result)
{
	std_vec_new(result, ACCESS_HEAPTUPLE_BOOL);
	for (size_t i = 0; i < Min(self->external.len, self->kept.len); i++)
		if (STD_VEC_AT(&self->external, bool, i))
			std_vec_push_Checked(result, &STD_VEC_AT(&self->kept, bool, i), ACCESS_HEAPTUPLE_BOOL);
}

/* access_heaptuple_Placing_blob answers the blob the row needs written next, or false once every one is. */
bool
access_heaptuple_Placing_blob(const AccessHeaptuplePlacing *self, AccessHeaptupleBlob *result)
{
	if (self->step < self->moves.len)
	{
		size_t		i = STD_VEC_AT(&self->moves, size_t, self->step);

		std_index_check(i, self->fields.len, RUST_FILE, 736, 42);
		result->tag = ACCESS_HEAPTUPLE_BLOB_VALUE;
		result->u.value = access_heaptuple_field_bytes(&STD_VEC_AT(&self->fields, access_OptionCowBytes_t, i));
		return true;
	}
	if (!self->has_chunk)
		return false;
	result->tag = ACCESS_HEAPTUPLE_BLOB_CHUNK;
	result->u.chunk.node.ptr = self->chunk_node.ptr;
	result->u.chunk.node.len = self->chunk_node.len;
	result->u.chunk.locators = self->chunk_locators.ptr;
	result->u.chunk.locators_len = self->chunk_locators.len;
	return true;
}

/*
 * access_heaptuple_Placing_wrote takes the address and locator of the blob
 * blob asked for, once the caller has written it.
 */
bool
access_heaptuple_Placing_wrote(AccessHeaptuplePlacing *self, ChunksAddressAddress address,
							   ChunksTrailerLocator locator, PgPgError *error)
{
	if (self->step < self->moves.len)
	{
		size_t		i = STD_VEC_AT(&self->moves, size_t, self->step);
		access_OptionCowBytes_t *field;
		access_OptionCowBytes_t reference = {.has = true, .owned = true};

		std_index_check(i, self->fields.len, RUST_FILE, 744, 26);
		field = &STD_VEC_AT(&self->fields, access_OptionCowBytes_t, i);
		encoding_tuple_reference((uint64) access_heaptuple_field_bytes(field).len, address.f0, &reference.bytes);
		std_index_check(i, self->fields.len, RUST_FILE, 745, 13);
		access_heaptuple_field_drop(field);
		*field = reference;
		std_index_check(i, self->locators.len, RUST_FILE, 746, 13);
		STD_VEC_AT(&self->locators, ChunksTrailerLocator, i) = locator;
	}
	else if (self->has_chunk)
	{
		self->has_overflow = true;
		self->overflow_address = address;
		self->overflow_locator = locator;
	}
	else
		return access_heaptuple_fail(access_heaptuple_internal("a row being placed was given a blob it didn't ask for"),
									 error);
	self->step = STD_ADD(usize, self->step, 1, 752, 9);
	return access_heaptuple_Placing_advance(self, error);
}

/*
 * access_heaptuple_formed_frame_t is formed's state across its pad: the
 * placing it takes and the row's tuple and addresses.
 */
typedef struct access_heaptuple_formed_frame_t
{
	AccessHeaptuplePlacing *self;
	bool		self_live;
	access_heaptuple_placed_t placed;
	bool		placed_live;
	AccessHeaptupleFormed *result;
	PgPgError  *error;
	bool		ok;
} access_heaptuple_formed_frame_t;

/* access_heaptuple_Placing_formed_pad frees the placing and the row's tuple and addresses. */
static void
access_heaptuple_Placing_formed_pad(void *frame_)
{
	access_heaptuple_formed_frame_t *frame = frame_;

	if (frame->self_live)
		access_heaptuple_Placing_drop(frame->self);
	if (frame->placed_live)
	{
		std_vec_free_Checked(&frame->placed.tuple, ACCESS_HEAPTUPLE_BYTES);
		std_vec_free_Checked(&frame->placed.addresses, ACCESS_HEAPTUPLE_ADDRESS);
	}
	frame->self_live = frame->placed_live = false;
}

/* access_heaptuple_Placing_formed_body writes the row's tuple and keys a keyless row by its hash. */
static void
access_heaptuple_Placing_formed_body(void *frame_)
{
	access_heaptuple_formed_frame_t *frame = frame_;
	AccessHeaptuplePlacing *self = frame->self;
	AccessHeaptupleBlob blob;

	if (access_heaptuple_Placing_blob(self, &blob))
	{
		*frame->error = access_heaptuple_internal("a row was formed before its blobs were written");
		return;
	}
	if (!access_heaptuple_Placing_write(self, 0, &frame->placed, frame->error))
		return;
	frame->placed_live = true;
	if (self->keyless)
	{
		ChunksAddressAddress hash = chunks_address_Address_of(frame->placed.tuple.ptr, frame->placed.tuple.len);

		std_range_to_check(ACCESS_HEAPTUPLE_KEYLESS_KEY_LEN, sizeof(hash.f0), RUST_FILE,
						   764, 36);
		std_vec_extend_from_slice_Checked(&self->key, hash.f0, ACCESS_HEAPTUPLE_KEYLESS_KEY_LEN, ACCESS_HEAPTUPLE_BYTES);
	}
	frame->result->key = self->key;
	std_vec_new(&self->key, ACCESS_HEAPTUPLE_BYTES);
	frame->result->tuple = frame->placed.tuple;
	frame->result->addresses = frame->placed.addresses;
	frame->result->heap_bytes = self->heap_bytes;
	frame->placed_live = false;
	frame->ok = true;
}

/*
 * access_heaptuple_Placing_formed answers the row as its table's tree stores
 * it, once every blob it asked for is written; it takes the placing.
 */
bool
access_heaptuple_Placing_formed(AccessHeaptuplePlacing *self, AccessHeaptupleFormed *result, PgPgError *error)
{
	access_heaptuple_formed_frame_t frame = {.self = self, .self_live = true, .result = result, .error = error};

	std_landing(&frame, access_heaptuple_Placing_formed_body, access_heaptuple_Placing_formed_pad);
	access_heaptuple_Placing_formed_pad(&frame);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_ACCESS

/*
 * ACCESS_HEAPTUPLE_TESTS_REVERSED and ACCESS_HEAPTUPLE_TESTS_NAMES are key's
 * tests' Reversed as a SortKeys and as a Names.
 */
#define ACCESS_HEAPTUPLE_TESTS_REVERSED ((AccessKeySortKeys) {&ACCESS_KEY_REVERSED_SORT_KEYS_OPS, NULL})
#define ACCESS_HEAPTUPLE_TESTS_NAMES ((AccessKeyNames) {&ACCESS_KEY_REVERSED_NAMES_OPS, NULL})

/* access_heaptuple_tests_message_Debug_fmt is an error's message's Debug, an Option<&str>. */
static bool
access_heaptuple_tests_message_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdStr *message = value;
	StdOptionRef option = {message->ptr != NULL, {message, std_str_Debug_fmt}};

	return std_option_Debug_fmt(&option, f);
}

/* access_heaptuple_tests_ok is tests' ok: the call's result when it succeeded, or a panic with the error's message. */
static void
access_heaptuple_tests_ok(bool ok, PgPgError *error)
{
	if (!ok)
	{
		StdStr		message = pg_PgError_message(error);
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&message, access_heaptuple_tests_message_Debug_fmt}};
		StdFmtArguments arguments = {pieces, 1, args};

		std_panic_fmt(RUST_FILE, 821, 39, &arguments);
	}
}

/* access_heaptuple_tests_GotTag is Got's variants. */
typedef enum access_heaptuple_tests_GotTag
{
	ACCESS_HEAPTUPLE_TESTS_GOT_NULL,
	ACCESS_HEAPTUPLE_TESTS_GOT_WORD,
	ACCESS_HEAPTUPLE_TESTS_GOT_FIXED,
	ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA,
	ACCESS_HEAPTUPLE_TESTS_GOT_MISSING,
	ACCESS_HEAPTUPLE_TESTS_GOT_OUT_OF_LINE,
	ACCESS_HEAPTUPLE_TESTS_GOT_OVERFLOWED,
} access_heaptuple_tests_GotTag;

/* access_heaptuple_tests_Got is Got: a value deforming gave, kept with its bytes. */
typedef struct access_heaptuple_tests_Got
{
	access_heaptuple_tests_GotTag tag;
	uint64		word;
	StdVec		bytes;
	size_t		field;
} access_heaptuple_tests_Got;

/* ACCESS_HEAPTUPLE_GOT is Got's element layout. */
#define ACCESS_HEAPTUPLE_GOT sizeof(access_heaptuple_tests_Got), _Alignof(access_heaptuple_tests_Got)

/* access_heaptuple_tests_Got_has_bytes reports whether a Got holds bytes. */
static bool
access_heaptuple_tests_Got_has_bytes(const access_heaptuple_tests_Got *self)
{
	return self->tag == ACCESS_HEAPTUPLE_TESTS_GOT_FIXED || self->tag == ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA ||
		self->tag == ACCESS_HEAPTUPLE_TESTS_GOT_OUT_OF_LINE;
}

/* access_heaptuple_tests_Got_drop frees a Got's bytes, as a vector's element. */
static void
access_heaptuple_tests_Got_drop(void *got)
{
	access_heaptuple_tests_Got *self = got;

	if (access_heaptuple_tests_Got_has_bytes(self))
		std_vec_free_Checked(&self->bytes, ACCESS_HEAPTUPLE_BYTES);
}

/* access_heaptuple_tests_Got_PartialEq_eq is Got's derived PartialEq. */
static bool
access_heaptuple_tests_Got_PartialEq_eq(const access_heaptuple_tests_Got *self, const access_heaptuple_tests_Got *other)
{
	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case ACCESS_HEAPTUPLE_TESTS_GOT_NULL:
		case ACCESS_HEAPTUPLE_TESTS_GOT_MISSING:
			return true;
		case ACCESS_HEAPTUPLE_TESTS_GOT_WORD:
			return self->word == other->word;
		case ACCESS_HEAPTUPLE_TESTS_GOT_OVERFLOWED:
			return self->field == other->field;
		case ACCESS_HEAPTUPLE_TESTS_GOT_OUT_OF_LINE:
			if (self->word != other->word)
				return false;
			pg_fallthrough;
		case ACCESS_HEAPTUPLE_TESTS_GOT_FIXED:
		case ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA:
			return self->bytes.len == other->bytes.len &&
				memcmp(self->bytes.ptr, other->bytes.ptr, self->bytes.len) == 0;
	}
	pg_unreachable();
}

/* access_heaptuple_tests_gots_eq reports whether two runs of Gots are equal, as a slice's PartialEq is. */
static bool
access_heaptuple_tests_gots_eq(const access_heaptuple_tests_Got *a, size_t a_len, const access_heaptuple_tests_Got *b,
							   size_t b_len)
{
	if (a_len != b_len)
		return false;
	for (size_t i = 0; i < a_len; i++)
		if (!access_heaptuple_tests_Got_PartialEq_eq(&a[i], &b[i]))
			return false;
	return true;
}

/* access_heaptuple_tests_gots_drop frees a vector of Gots. */
static void
access_heaptuple_tests_gots_drop(StdVec *gots)
{
	std_vec_drop_Checked(gots, ACCESS_HEAPTUPLE_GOT, access_heaptuple_tests_Got_drop);
}

/* access_heaptuple_tests_got_of returns a Got of a tag alone, or of a word. */
static access_heaptuple_tests_Got
access_heaptuple_tests_got_of(access_heaptuple_tests_GotTag tag, uint64 word)
{
	access_heaptuple_tests_Got got = {.tag = tag, .word = word};

	return got;
}

/* access_heaptuple_tests_got_bytes returns a Got of a tag and a copy of some bytes. */
static access_heaptuple_tests_Got
access_heaptuple_tests_got_bytes(access_heaptuple_tests_GotTag tag, const void *bytes, size_t len)
{
	access_heaptuple_tests_Got got = {.tag = tag};

	std_slice_to_vec_Checked(&got.bytes, bytes, len, ACCESS_HEAPTUPLE_BYTES);
	return got;
}

/*
 * access_heaptuple_tests_column is tests' column: a column of a type, its
 * length, and whether it is passed by value, of its length's default storage.
 */
static AccessHeaptupleColumn
access_heaptuple_tests_column(uint32 typid, int16 len, bool by_value)
{
	AccessHeaptupleColumn column = {.typid = typid, .len = len, .by_value = by_value};

	column.storage = len == -1 ? ACCESS_HEAPTUPLE_STORAGE_EXTENDED : ACCESS_HEAPTUPLE_STORAGE_PLAIN;
	column.align = len > 0 ? (size_t) len : 4;
	return column;
}

/* access_heaptuple_tests_got is tests' got: a deformed value as a test keeps it. */
static access_heaptuple_tests_Got
access_heaptuple_tests_got(AccessHeaptupleDeformed deformed)
{
	switch (deformed.tag)
	{
		case ACCESS_HEAPTUPLE_DEFORMED_VALUE:
			switch (deformed.u.value.tag)
			{
				case ACCESS_HEAPTUPLE_SET_NULL:
					return access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_NULL, 0);
				case ACCESS_HEAPTUPLE_SET_WORD:
					return access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, deformed.u.value.u.word);
				case ACCESS_HEAPTUPLE_SET_FIXED:
					return access_heaptuple_tests_got_bytes(ACCESS_HEAPTUPLE_TESTS_GOT_FIXED, deformed.u.value.u.fixed.ptr,
															deformed.u.value.u.fixed.len);
				case ACCESS_HEAPTUPLE_SET_VARLENA:
					return access_heaptuple_tests_got_bytes(ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA,
															deformed.u.value.u.varlena.ptr, deformed.u.value.u.varlena.len);
			}
			break;
		case ACCESS_HEAPTUPLE_DEFORMED_MISSING:
			return access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_MISSING, 0);
		case ACCESS_HEAPTUPLE_DEFORMED_OUT_OF_LINE:
			{
				access_heaptuple_tests_Got got = access_heaptuple_tests_got_bytes(ACCESS_HEAPTUPLE_TESTS_GOT_OUT_OF_LINE,
																				  deformed.u.out_of_line.address.ptr,
																				  deformed.u.out_of_line.address.len);

				got.word = deformed.u.out_of_line.length;
				return got;
			}
		case ACCESS_HEAPTUPLE_DEFORMED_OVERFLOWED:
			{
				access_heaptuple_tests_Got got = {.tag = ACCESS_HEAPTUPLE_TESTS_GOT_OVERFLOWED,
				.field = deformed.u.overflowed.field};

				return got;
			}
	}
	pg_unreachable();
}

/* access_heaptuple_tests_deformed_closure_0 is deformed's set: each deformed value pushed as a Got. */
static bool
access_heaptuple_tests_deformed_closure_0(void *captures, AccessHeaptupleDeformed deformed, PgPgError *error)
{
	access_heaptuple_tests_Got got = access_heaptuple_tests_got(deformed);

	std_vec_push_Checked(captures, &got, ACCESS_HEAPTUPLE_GOT);
	return true;
}

/*
 * access_heaptuple_tests_deformed is tests' deformed: what deforming a row's
 * columns from a position up to another gives, a Vec<Got>.
 */
static bool
access_heaptuple_tests_deformed(const AccessHeaptupleRowLayout *layout, StdBytes key, StdBytes tuple, size_t from,
								size_t upto, StdVec *result, PgPgError *error)
{
	bool		has;
	size_t		count;

	std_vec_new(result, ACCESS_HEAPTUPLE_GOT);
	if (!access_heaptuple_RowLayout_deform(layout, key.ptr, key.len, tuple.ptr, tuple.len, false, NULL, 0, from, upto,
										   access_heaptuple_tests_deformed_closure_0, result, &has, &count, error))
	{
		access_heaptuple_tests_gots_drop(result);
		return false;
	}
	return true;
}

/* access_heaptuple_tests_deformed_ok is deformed's answer where the call must succeed. */
static StdVec
access_heaptuple_tests_deformed_ok(const AccessHeaptupleRowLayout *layout, StdBytes key, StdBytes tuple, size_t from,
								   size_t upto)
{
	StdVec		got;
	PgPgError	error;

	access_heaptuple_tests_ok(access_heaptuple_tests_deformed(layout, key, tuple, from, upto, &got, &error), &error);
	return got;
}

/* access_heaptuple_tests_deformed_is_err reports whether deformed fails, dropping what it gave. */
static bool
access_heaptuple_tests_deformed_is_err(const AccessHeaptupleRowLayout *layout, StdBytes key, StdBytes tuple,
									   size_t from, size_t upto)
{
	StdVec		got;
	PgPgError	error;

	if (!access_heaptuple_tests_deformed(layout, key, tuple, from, upto, &got, &error))
	{
		pg_PgError_drop(&error);
		return true;
	}
	access_heaptuple_tests_gots_drop(&got);
	return false;
}

/* access_heaptuple_tests_bytes returns a borrowed run of a vector's bytes. */
static StdBytes
access_heaptuple_tests_bytes(const StdVec *vec)
{
	StdBytes	bytes = {vec->ptr, vec->len};

	return bytes;
}

/* access_heaptuple_tests_slice returns a slice of some bytes. */
static StdBytes
access_heaptuple_tests_slice(const void *ptr, size_t len)
{
	StdBytes	slice = {ptr, len};

	return slice;
}

/* access_heaptuple_tests_keyed returns a key column of a layout: its position, order, and key type. */
static access_TupleUsizeOrderKeyType_t
access_heaptuple_tests_keyed(size_t at, EncodingKeyOrder order, uint32 typid)
{
	access_TupleUsizeOrderKeyType_t keyed = {at, order, access_key_KeyType_of(typid)};

	return keyed;
}

/* access_heaptuple_tests_layout returns a layout of some columns and key columns, which it copies into its vectors. */
static AccessHeaptupleRowLayout
access_heaptuple_tests_layout(const AccessHeaptupleColumn *columns, size_t columns_len,
							  const access_TupleUsizeOrderKeyType_t *key, size_t key_len)
{
	StdVec		column_vec;
	StdVec		key_vec;
	AccessHeaptupleRowLayout layout;

	std_slice_to_vec_Checked(&column_vec, columns, columns_len, ACCESS_HEAPTUPLE_COLUMN);
	std_slice_to_vec_Checked(&key_vec, key, key_len, ACCESS_HEAPTUPLE_KEYED);
	access_heaptuple_RowLayout_new(&column_vec, &key_vec, &layout);
	return layout;
}

/* access_heaptuple_tests_encode appends a value's key column where the call must succeed. */
static void
access_heaptuple_tests_encode(StdVec *key, uint32 typid, AccessKeyValue value, EncodingKeyOrder order)
{
	AccessKeyKeyType key_type = access_key_KeyType_of(typid);
	PgPgError	error;

	access_heaptuple_tests_ok(access_key_encode(key, ACCESS_HEAPTUPLE_TESTS_REVERSED, &key_type, true, value, order,
												&error), &error);
}

/* access_heaptuple_tests_write is tuple::write of fields with no overflow, a NULL one's ptr NULL. */
static StdVec
access_heaptuple_tests_write(const StdBytes *fields, size_t len)
{
	StdVec		tuple;
	encoding_tuple_write_overflow_t none = {0};

	encoding_tuple_write(fields, len, false, none, &tuple);
	return tuple;
}

/* access_heaptuple_tests_table_t is tests' table: a layout, a row's key and tuple, and its values. */
typedef struct access_heaptuple_tests_table_t
{
	AccessHeaptupleRowLayout layout;
	StdVec		key;
	StdVec		tuple;
	access_heaptuple_tests_Got values[6];
} access_heaptuple_tests_table_t;

/*
 * access_heaptuple_tests_table is tests' table: a table of a text, an int4,
 * a dropped column, a "char", an int8, and a uuid, whose primary key is the
 * int4 descending and then the text, and a row of it.
 */
static access_heaptuple_tests_table_t
access_heaptuple_tests_table(void)
{
	access_heaptuple_tests_table_t table;
	AccessHeaptupleColumn columns[6] = {
		access_heaptuple_tests_column(TEXTOID, -1, false),
		access_heaptuple_tests_column(INT4OID, 4, true),
		access_heaptuple_tests_column(INT4OID, 4, true),
		access_heaptuple_tests_column(CHAROID, 1, true),
		access_heaptuple_tests_column(INT8OID, 8, true),
		access_heaptuple_tests_column(UUIDOID, 16, false),
	};
	EncodingKeyOrder descending = {true, true};
	access_TupleUsizeOrderKeyType_t key[2] = {access_heaptuple_tests_keyed(1, descending, INT4OID),
	access_heaptuple_tests_keyed(0, ENCODING_KEY_ORDER_ASC, TEXTOID)};
	AccessKeyValue word = {.tag = ACCESS_KEY_VALUE_WORD, .u.word = pg_word_from_int32(-7)};
	AccessKeyValue str = {.tag = ACCESS_KEY_VALUE_BYTES, .u.bytes = {(const uint8 *) "\xc3\xa9" "a", 3}};
	uint8		uuid[16];
	uint8		e9 = 0xE9;
	uint8		min[8] = {0, 0, 0, 0, 0, 0, 0, 0x80};
	StdBytes	fields[4] = {{NULL, 0}, {&e9, 1}, {min, 8}, {uuid, 16}};

	columns[2].dropped = true;
	table.layout = access_heaptuple_tests_layout(columns, 6, key, 2);
	std_vec_new(&table.key, ACCESS_HEAPTUPLE_BYTES);
	access_heaptuple_tests_encode(&table.key, INT4OID, word, descending);
	access_heaptuple_tests_encode(&table.key, TEXTOID, str, ENCODING_KEY_ORDER_ASC);
	for (int i = 0; i < 16; i++)
		uuid[i] = (uint8) (i + 1);
	table.tuple = access_heaptuple_tests_write(fields, 4);
	table.values[0] = access_heaptuple_tests_got_bytes(ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA, "\xc3\xa9" "a", 3);
	table.values[1] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, pg_word_from_int32(-7));
	table.values[2] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_NULL, 0);
	table.values[3] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, pg_word_from_char(0xE9));
	table.values[4] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, (uint64) PG_INT64_MIN);
	table.values[5] = access_heaptuple_tests_got_bytes(ACCESS_HEAPTUPLE_TESTS_GOT_FIXED, uuid, 16);
	return table;
}

/* access_heaptuple_tests_table_drop frees a test table. */
static void
access_heaptuple_tests_table_drop(access_heaptuple_tests_table_t *table)
{
	access_heaptuple_RowLayout_drop(&table->layout);
	std_vec_free_Checked(&table->key, ACCESS_HEAPTUPLE_BYTES);
	std_vec_free_Checked(&table->tuple, ACCESS_HEAPTUPLE_BYTES);
	for (int i = 0; i < 6; i++)
		access_heaptuple_tests_Got_drop(&table->values[i]);
}

/* access_heaptuple_tests_deformed_eq reports whether deforming gives some values, freeing what it gave. */
static bool
access_heaptuple_tests_deformed_eq(const AccessHeaptupleRowLayout *layout, StdBytes key, StdBytes tuple, size_t from,
								   size_t upto, const access_heaptuple_tests_Got *values, size_t len)
{
	StdVec		got = access_heaptuple_tests_deformed_ok(layout, key, tuple, from, upto);
	bool		same = access_heaptuple_tests_gots_eq(got.ptr, got.len, values, len);

	access_heaptuple_tests_gots_drop(&got);
	return same;
}

/*
 * The fork's own: a row's attributes come from its tuple's fields and, for the
 * key columns the tuple leaves out, from its key, and a dropped column is
 * NULL.
 */
void
test_access_heaptuple_tests_a_row_deforms_to_its_fields_and_key_columns(void)
{
	access_heaptuple_tests_table_t table = access_heaptuple_tests_table();
	StdBytes	key = access_heaptuple_tests_bytes(&table.key);
	StdBytes	tuple = access_heaptuple_tests_bytes(&table.tuple);

	STD_ASSERT(904, 9, access_heaptuple_tests_deformed_eq(&table.layout, key, tuple, 0, 6, table.values, 6),
			   "ok(deformed(&layout, &key, &tuple, 0, 6)) == values");
	STD_ASSERT(905, 9, access_heaptuple_tests_deformed_eq(&table.layout, key, tuple, 0, 9, table.values, 6),
			   "ok(deformed(&layout, &key, &tuple, 0, 9)) == values");
	access_heaptuple_tests_table_drop(&table);
}

/* The fork's own: deforming goes on from any attribute, giving only the attributes asked for. */
void
test_access_heaptuple_tests_deforming_goes_on_from_where_it_stopped(void)
{
	access_heaptuple_tests_table_t table = access_heaptuple_tests_table();
	StdBytes	key = access_heaptuple_tests_bytes(&table.key);
	StdBytes	tuple = access_heaptuple_tests_bytes(&table.tuple);

	for (size_t stop = 0; stop <= 6; stop++)
	{
		std_range_to_check(stop, 6, RUST_FILE, 913, 69);
		STD_ASSERT(913, 13,
				   access_heaptuple_tests_deformed_eq(&table.layout, key, tuple, 0, stop, table.values, stop),
				   "ok(deformed(&layout, &key, &tuple, 0, stop)) == values[..stop]");
		std_range_from_check(stop, 6, RUST_FILE, 914, 69);
		STD_ASSERT(914, 13,
				   access_heaptuple_tests_deformed_eq(&table.layout, key, tuple, stop, 6, table.values + stop, 6 - stop),
				   "ok(deformed(&layout, &key, &tuple, stop, 6)) == values[stop..]");
	}
	access_heaptuple_tests_table_drop(&table);
}

/*
 * access_heaptuple_tests_lossy_layout is the lossy tests' layout: a bpchar
 * keyed in its own key, an int4, and an int2.
 */
static AccessHeaptupleRowLayout
access_heaptuple_tests_lossy_layout(void)
{
	AccessHeaptupleColumn columns[3] = {access_heaptuple_tests_column(BPCHAROID, -1, false),
		access_heaptuple_tests_column(INT4OID, 4, true),
	access_heaptuple_tests_column(INT2OID, 2, true)};
	access_TupleUsizeOrderKeyType_t key[1] = {access_heaptuple_tests_keyed(0, ENCODING_KEY_ORDER_ASC, BPCHAROID)};

	return access_heaptuple_tests_layout(columns, 3, key, 1);
}

/*
 * The fork's own: a lossy key column is a field of the tuple, a NULL field and
 * a field past the tuple's count are the attribute's value and its missing
 * value, and a by-value field is sign-extended.
 */
void
test_access_heaptuple_tests_a_lossy_key_column_and_short_tuples_read_from_the_tuple(void)
{
	AccessHeaptupleRowLayout layout = access_heaptuple_tests_lossy_layout();
	StdVec		key;
	uint8		minus_two[2] = {0xfe, 0xff};
	StdBytes	fields[3] = {{(const uint8 *) "ab  ", 4}, {NULL, 0}, {minus_two, 2}};
	StdVec		tuple;
	StdVec		short_tuple;
	access_heaptuple_tests_Got values[3];
	AccessKeyValue ab = {.tag = ACCESS_KEY_VALUE_BYTES, .u.bytes = {(const uint8 *) "ab  ", 4}};

	std_vec_new(&key, ACCESS_HEAPTUPLE_BYTES);
	access_heaptuple_tests_encode(&key, BPCHAROID, ab, ENCODING_KEY_ORDER_ASC);
	tuple = access_heaptuple_tests_write(fields, 3);
	values[0] = access_heaptuple_tests_got_bytes(ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA, "ab  ", 4);
	values[1] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_NULL, 0);
	values[2] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, pg_word_from_int16(-2));
	STD_ASSERT(928, 9,
			   access_heaptuple_tests_deformed_eq(&layout, access_heaptuple_tests_bytes(&key),
												  access_heaptuple_tests_bytes(&tuple), 0, 3, values, 3),
			   "values == [Got::Varlena(b\"ab  \".to_vec()), Got::Null, Got::Word(word::from_int16(-2))]");
	short_tuple = access_heaptuple_tests_write(fields, 1);
	values[1] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_MISSING, 0);
	values[2] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_MISSING, 0);
	STD_ASSERT(931, 9,
			   access_heaptuple_tests_deformed_eq(&layout, access_heaptuple_tests_bytes(&key),
												  access_heaptuple_tests_bytes(&short_tuple), 0, 3, values, 3),
			   "values == [Got::Varlena(b\"ab  \".to_vec()), Got::Missing, Got::Missing]");
	access_heaptuple_tests_Got_drop(&values[0]);
	std_vec_free_Checked(&short_tuple, ACCESS_HEAPTUPLE_BYTES);
	std_vec_free_Checked(&tuple, ACCESS_HEAPTUPLE_BYTES);
	std_vec_free_Checked(&key, ACCESS_HEAPTUPLE_BYTES);
	access_heaptuple_RowLayout_drop(&layout);
}

/*
 * The fork's own: a tuple that doesn't parse, a by-value field of a width no
 * datum has, and a key cut short are errors and never a wrong value.
 */
void
test_access_heaptuple_tests_a_row_that_does_not_decode_is_refused(void)
{
	access_heaptuple_tests_table_t table = access_heaptuple_tests_table();
	StdBytes	key = access_heaptuple_tests_bytes(&table.key);
	StdBytes	tuple = access_heaptuple_tests_bytes(&table.tuple);
	uint8		ff = 0xff;
	uint8		three[3] = {1, 2, 3};
	uint8		eight[8] = {0};
	uint8		sixteen[16] = {0};
	StdBytes	fields[3] = {{three, 3}, {eight, 8}, {sixteen, 16}};
	StdVec		odd;
	StdVec		got;

	STD_ASSERT(939, 9, access_heaptuple_tests_deformed_is_err(&table.layout, key, access_heaptuple_tests_slice(&ff, 1), 0, 6),
			   "deformed(&layout, &key, &[0xff], 0, 6).is_err()");
	std_range_to_check(2, key.len, RUST_FILE, 940, 36);
	STD_ASSERT(940, 9,
			   access_heaptuple_tests_deformed_is_err(&table.layout, access_heaptuple_tests_slice(key.ptr, 2), tuple, 0, 6),
			   "deformed(&layout, &key[..2], &tuple, 0, 6).is_err()");
	std_range_to_check(2, key.len, RUST_FILE, 941, 39);
	got = access_heaptuple_tests_deformed_ok(&table.layout, access_heaptuple_tests_slice(key.ptr, 2), tuple, 2, 6);
	STD_ASSERT(941, 9, got.len == 4, "ok(deformed(&layout, &key[..2], &tuple, 2, 6)).len() == 4");
	access_heaptuple_tests_gots_drop(&got);
	odd = access_heaptuple_tests_write(fields, 3);
	STD_ASSERT(943, 9,
			   access_heaptuple_tests_deformed_is_err(&table.layout, key, access_heaptuple_tests_bytes(&odd), 0, 6),
			   "deformed(&layout, &key, &odd, 0, 6).is_err()");
	std_vec_free_Checked(&odd, ACCESS_HEAPTUPLE_BYTES);
	access_heaptuple_tests_table_drop(&table);
}

/* access_heaptuple_tests_given_one is given's one: a value deforming gave as the value forming takes. */
static AccessHeaptupleSet
access_heaptuple_tests_given_one(const access_heaptuple_tests_Got *value)
{
	AccessHeaptupleSet set = {0};

	switch (value->tag)
	{
		case ACCESS_HEAPTUPLE_TESTS_GOT_NULL:
			set.tag = ACCESS_HEAPTUPLE_SET_NULL;
			return set;
		case ACCESS_HEAPTUPLE_TESTS_GOT_WORD:
			set.tag = ACCESS_HEAPTUPLE_SET_WORD;
			set.u.word = value->word;
			return set;
		case ACCESS_HEAPTUPLE_TESTS_GOT_FIXED:
		case ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA:
			set.tag = value->tag == ACCESS_HEAPTUPLE_TESTS_GOT_FIXED ? ACCESS_HEAPTUPLE_SET_FIXED : ACCESS_HEAPTUPLE_SET_VARLENA;
			set.u.fixed.ptr = value->bytes.ptr;
			set.u.fixed.len = value->bytes.len;
			return set;
		case ACCESS_HEAPTUPLE_TESTS_GOT_MISSING:
		case ACCESS_HEAPTUPLE_TESTS_GOT_OUT_OF_LINE:
		case ACCESS_HEAPTUPLE_TESTS_GOT_OVERFLOWED:
			break;
	}
	std_panic(RUST_FILE, 954, 75, "only a value forms a row", sizeof("only a value forms a row") - 1);
}

/*
 * access_heaptuple_tests_given is tests' given: values deforming gave as the
 * values forming takes, into a caller's array.
 */
static void
access_heaptuple_tests_given(const access_heaptuple_tests_Got *values, size_t len, AccessHeaptupleSet *result)
{
	for (size_t i = 0; i < len; i++)
		result[i] = access_heaptuple_tests_given_one(&values[i]);
}

/*
 * access_heaptuple_tests_formed is tests' formed: a row of values as its
 * table's tree stores it, of a row that moves nothing out of line.
 */
static bool
access_heaptuple_tests_formed(const AccessHeaptupleRowLayout *layout, const AccessHeaptupleSet *values, size_t len,
							  AccessHeaptupleFormed *result, PgPgError *error)
{
	AccessHeaptuplePlacing placing;

	if (!access_heaptuple_RowLayout_placing(layout, ACCESS_HEAPTUPLE_TESTS_REVERSED, ACCESS_HEAPTUPLE_TESTS_NAMES, values,
											len, NULL, 0, false, ACCESS_HEAPTUPLE_TOAST_TUPLE_TARGET, &placing, error))
		return false;
	return access_heaptuple_Placing_formed(&placing, result, error);
}

/* access_heaptuple_tests_formed_ok is formed's answer where the call must succeed. */
static AccessHeaptupleFormed
access_heaptuple_tests_formed_ok(const AccessHeaptupleRowLayout *layout, const AccessHeaptupleSet *values, size_t len)
{
	AccessHeaptupleFormed formed;
	PgPgError	error;

	access_heaptuple_tests_ok(access_heaptuple_tests_formed(layout, values, len, &formed, &error), &error);
	return formed;
}

/* access_heaptuple_tests_formed_is_err reports whether formed fails, dropping what it gave. */
static bool
access_heaptuple_tests_formed_is_err(const AccessHeaptupleRowLayout *layout, const AccessHeaptupleSet *values,
									 size_t len)
{
	AccessHeaptupleFormed formed;
	PgPgError	error;

	if (!access_heaptuple_tests_formed(layout, values, len, &formed, &error))
	{
		pg_PgError_drop(&error);
		return true;
	}
	access_heaptuple_Formed_drop(&formed);
	return false;
}

/* access_heaptuple_tests_vec_eq reports whether a vector holds some bytes. */
static bool
access_heaptuple_tests_vec_eq(const StdVec *vec, const void *bytes, size_t len)
{
	return vec->len == len && memcmp(vec->ptr, bytes, len) == 0;
}

/*
 * The fork's own: forming a row's values gives the key and the tuple that
 * deform to them, an exact key column left out of the tuple and a dropped
 * column's value left out of both.
 */
void
test_access_heaptuple_tests_a_row_forms_to_the_key_and_tuple_it_deforms_from(void)
{
	access_heaptuple_tests_table_t table = access_heaptuple_tests_table();
	AccessHeaptupleSet given[6];
	AccessHeaptupleFormed first;
	AccessHeaptupleFormed formed;

	access_heaptuple_tests_given(table.values, 6, given);
	first = access_heaptuple_tests_formed_ok(&table.layout, given, 6);
	STD_ASSERT(971, 9,
			   access_heaptuple_tests_vec_eq(&first.key, table.key.ptr, table.key.len) &&
			   access_heaptuple_tests_vec_eq(&first.tuple, table.tuple.ptr, table.tuple.len) && first.addresses.len == 0,
			   "(first.key, first.tuple, first.addresses) == (key.clone(), tuple.clone(), Vec::new())");
	access_heaptuple_Formed_drop(&first);
	table.values[2] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, 9);
	access_heaptuple_tests_given(table.values, 6, given);
	formed = access_heaptuple_tests_formed_ok(&table.layout, given, 6);
	STD_ASSERT(974, 9,
			   access_heaptuple_tests_vec_eq(&formed.key, table.key.ptr, table.key.len) &&
			   access_heaptuple_tests_vec_eq(&formed.tuple, table.tuple.ptr, table.tuple.len) && formed.addresses.len == 0,
			   "(&formed.key, &formed.tuple, formed.addresses.is_empty()) == (&key, &tuple, true)");
	table.values[2] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_NULL, 0);
	STD_ASSERT(976, 9,
			   access_heaptuple_tests_deformed_eq(&table.layout, access_heaptuple_tests_bytes(&formed.key),
												  access_heaptuple_tests_bytes(&formed.tuple), 0, 6, table.values, 6),
			   "ok(deformed(&layout, &formed.key, &formed.tuple, 0, 6)) == values");
	access_heaptuple_Formed_drop(&formed);
	access_heaptuple_tests_table_drop(&table);
}

/*
 * The fork's own: a lossy key column is in the key and in the tuple, a NULL is
 * a NULL field, and a by-value field is as wide as its type.
 */
void
test_access_heaptuple_tests_a_lossy_key_column_forms_into_both(void)
{
	AccessHeaptupleRowLayout layout = access_heaptuple_tests_lossy_layout();
	access_heaptuple_tests_Got values[3];
	AccessHeaptupleSet given[3];
	AccessHeaptupleFormed formed;
	StdVec		key;
	AccessKeyValue ab = {.tag = ACCESS_KEY_VALUE_BYTES, .u.bytes = {(const uint8 *) "ab  ", 4}};
	uint8		minus_two[2] = {0xfe, 0xff};
	StdBytes	fields[3] = {{(const uint8 *) "ab  ", 4}, {NULL, 0}, {minus_two, 2}};
	StdVec		tuple;

	values[0] = access_heaptuple_tests_got_bytes(ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA, "ab  ", 4);
	values[1] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_NULL, 0);
	values[2] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, pg_word_from_int16(-2));
	access_heaptuple_tests_given(values, 3, given);
	formed = access_heaptuple_tests_formed_ok(&layout, given, 3);
	std_vec_new(&key, ACCESS_HEAPTUPLE_BYTES);
	access_heaptuple_tests_encode(&key, BPCHAROID, ab, ENCODING_KEY_ORDER_ASC);
	STD_ASSERT(989, 9, access_heaptuple_tests_vec_eq(&formed.key, key.ptr, key.len), "formed.key == key");
	tuple = access_heaptuple_tests_write(fields, 3);
	STD_ASSERT(990, 9, access_heaptuple_tests_vec_eq(&formed.tuple, tuple.ptr, tuple.len),
			   "formed.tuple == tuple::write(&[Some(b\"ab  \"), None, Some(&(-2i16).to_le_bytes())], None)");
	STD_ASSERT(991, 9,
			   access_heaptuple_tests_deformed_eq(&layout, access_heaptuple_tests_bytes(&formed.key),
												  access_heaptuple_tests_bytes(&formed.tuple), 0, 3, values, 3),
			   "ok(deformed(&layout, &formed.key, &formed.tuple, 0, 3)) == values");
	std_vec_free_Checked(&tuple, ACCESS_HEAPTUPLE_BYTES);
	std_vec_free_Checked(&key, ACCESS_HEAPTUPLE_BYTES);
	access_heaptuple_Formed_drop(&formed);
	access_heaptuple_tests_Got_drop(&values[0]);
	access_heaptuple_RowLayout_drop(&layout);
}

/* access_heaptuple_Formed_PartialEq_eq is Formed's derived PartialEq. */
bool
access_heaptuple_Formed_PartialEq_eq(const AccessHeaptupleFormed *self, const AccessHeaptupleFormed *other)
{
	if (!access_heaptuple_tests_vec_eq(&self->key, other->key.ptr, other->key.len) ||
		!access_heaptuple_tests_vec_eq(&self->tuple, other->tuple.ptr, other->tuple.len) ||
		self->addresses.len != other->addresses.len || self->heap_bytes != other->heap_bytes)
		return false;
	for (size_t i = 0; i < self->addresses.len; i++)
	{
		const chunks_TupleU32Locator_t *a = &STD_VEC_AT(&self->addresses, chunks_TupleU32Locator_t, i);
		const chunks_TupleU32Locator_t *b = &STD_VEC_AT(&other->addresses, chunks_TupleU32Locator_t, i);

		if (a->f0 != b->f0 || !chunks_trailer_Locator_PartialEq_eq(&a->f1, &b->f1))
			return false;
	}
	return true;
}

/* access_heaptuple_tests_keyless_row is the keyless test's row: a text and an int4's row formed. */
static AccessHeaptupleFormed
access_heaptuple_tests_keyless_row(const AccessHeaptupleRowLayout *layout, const char *str, int32 number)
{
	AccessHeaptupleSet row[2] = {{.tag = ACCESS_HEAPTUPLE_SET_VARLENA, .u.varlena = {(const uint8 *) str, strlen(str)}},
	{.tag = ACCESS_HEAPTUPLE_SET_WORD, .u.word = pg_word_from_int32(number)}};

	return access_heaptuple_tests_formed_ok(layout, row, 2);
}

/*
 * The fork's own: a row of a table without a primary key is keyed by the start
 * of its tuple's hash, so equal rows have one key and rows that differ have
 * two.
 */
void
test_access_heaptuple_tests_a_keyless_row_is_keyed_by_its_tuples_hash(void)
{
	AccessHeaptupleColumn columns[2] = {access_heaptuple_tests_column(TEXTOID, -1, false),
	access_heaptuple_tests_column(INT4OID, 4, true)};
	AccessHeaptupleRowLayout layout = access_heaptuple_tests_layout(columns, 2, NULL, 0);
	AccessHeaptupleFormed formed = access_heaptuple_tests_keyless_row(&layout, "a", 1);
	ChunksAddressAddress hash = chunks_address_Address_of(formed.tuple.ptr, formed.tuple.len);
	AccessHeaptupleFormed other;
	bool		holds;

	std_range_to_check(ACCESS_HEAPTUPLE_KEYLESS_KEY_LEN, sizeof(hash.f0), RUST_FILE,
					   1001, 31);
	STD_ASSERT(1001, 9, access_heaptuple_tests_vec_eq(&formed.key, hash.f0, ACCESS_HEAPTUPLE_KEYLESS_KEY_LEN),
			   "formed.key == chunks::Address::of(&formed.tuple).0[..KEYLESS_KEY_LEN]");
	holds = formed.key.len == ACCESS_HEAPTUPLE_KEYLESS_KEY_LEN;
	if (holds)
	{
		other = access_heaptuple_tests_keyless_row(&layout, "a", 1);
		holds = access_heaptuple_Formed_PartialEq_eq(&formed, &other);
		access_heaptuple_Formed_drop(&other);
	}
	STD_ASSERT(1002, 9, holds, "formed.key.len() == KEYLESS_KEY_LEN && formed == row(b\"a\", 1)");
	other = access_heaptuple_tests_keyless_row(&layout, "a", 2);
	holds = !access_heaptuple_tests_vec_eq(&formed.key, other.key.ptr, other.key.len);
	access_heaptuple_Formed_drop(&other);
	if (holds)
	{
		other = access_heaptuple_tests_keyless_row(&layout, "b", 1);
		holds = !access_heaptuple_tests_vec_eq(&formed.key, other.key.ptr, other.key.len);
		access_heaptuple_Formed_drop(&other);
	}
	STD_ASSERT(1003, 9, holds, "formed.key != row(b\"a\", 2).key && formed.key != row(b\"b\", 1).key");
	access_heaptuple_Formed_drop(&formed);
	access_heaptuple_RowLayout_drop(&layout);
}

/*
 * The fork's own: values that don't fit the table's columns, by their count or
 * their kind, are refused, and so is forming a row before the blobs it asked
 * for are written.
 */
void
test_access_heaptuple_tests_a_row_that_does_not_form_is_refused(void)
{
	access_heaptuple_tests_table_t table = access_heaptuple_tests_table();
	AccessHeaptupleSet given[6];
	AccessHeaptupleColumn text_column = access_heaptuple_tests_column(TEXTOID, -1, false);
	AccessHeaptupleRowLayout text_layout = access_heaptuple_tests_layout(&text_column, 1, NULL, 0);
	uint8		as[ACCESS_HEAPTUPLE_TOAST_TUPLE_THRESHOLD + 1];
	AccessHeaptupleSet set;
	AccessHeaptuplePlacing long_placing;
	PgPgError	error;
	AccessHeaptupleBlob blob;
	AccessHeaptupleFormed formed;
	bool		holds;

	memset(as, 'a', sizeof(as));
	std_range_to_check(5, 6, RUST_FILE, 1011, 41);
	access_heaptuple_tests_given(table.values, 5, given);
	STD_ASSERT(1011, 9, access_heaptuple_tests_formed_is_err(&table.layout, given, 5),
			   "formed(&layout, &given(&values[..5])).is_err()");
	access_heaptuple_tests_given(table.values, 6, given);
	given[3].tag = ACCESS_HEAPTUPLE_SET_VARLENA;
	given[3].u.varlena = (StdBytes)
	{
		(const uint8 *) "x", 1
	};
	STD_ASSERT(1014, 9, access_heaptuple_tests_formed_is_err(&table.layout, given, 6),
			   "formed(&layout, &wrong).is_err()");
	access_heaptuple_tests_given(table.values, 6, given);
	given[5].tag = ACCESS_HEAPTUPLE_SET_WORD;
	given[5].u.word = 1;
	STD_ASSERT(1017, 9, access_heaptuple_tests_formed_is_err(&table.layout, given, 6),
			   "formed(&layout, &wrong).is_err()");
	set.tag = ACCESS_HEAPTUPLE_SET_VARLENA;
	set.u.varlena.ptr = as;
	set.u.varlena.len = ACCESS_HEAPTUPLE_TOAST_TUPLE_THRESHOLD - 64;
	STD_ASSERT(1019, 9, !access_heaptuple_tests_formed_is_err(&text_layout, &set, 1),
			   "formed(&text, &[Set::Varlena(&[b'a'; TOAST_TUPLE_THRESHOLD - 64])]).is_ok()");
	set.u.varlena.len = ACCESS_HEAPTUPLE_TOAST_TUPLE_THRESHOLD + 1;
	access_heaptuple_tests_ok(access_heaptuple_RowLayout_placing(&text_layout, ACCESS_HEAPTUPLE_TESTS_REVERSED,
																 ACCESS_HEAPTUPLE_TESTS_NAMES, &set, 1, NULL, 0, false,
																 ACCESS_HEAPTUPLE_TOAST_TUPLE_TARGET, &long_placing, &error),
							  &error);
	holds = access_heaptuple_Placing_blob(&long_placing, &blob) && blob.tag == ACCESS_HEAPTUPLE_BLOB_VALUE;
	if (holds)
	{
		holds = !access_heaptuple_Placing_formed(&long_placing, &formed, &error);
		if (holds)
			pg_PgError_drop(&error);
		else
			access_heaptuple_Formed_drop(&formed);
	}
	else
		access_heaptuple_Placing_drop(&long_placing);
	STD_ASSERT(1027, 9, holds, "matches!(long.blob(), Some(Blob::Value(_))) && long.formed().is_err()");
	access_heaptuple_RowLayout_drop(&text_layout);
	access_heaptuple_tests_table_drop(&table);
}

/* access_heaptuple_tests_keyed_by_text_t is keyed_by_text's answer: a layout and a row of it. */
typedef struct access_heaptuple_tests_keyed_by_text_t
{
	AccessHeaptupleRowLayout layout;
	access_heaptuple_tests_Got values[9];
} access_heaptuple_tests_keyed_by_text_t;

/*
 * access_heaptuple_tests_keyed_by_text is tests' keyed_by_text: a table of
 * eight text columns, its primary key, and an int4 column, and a row of it
 * whose first column is a text of a length.
 */
static access_heaptuple_tests_keyed_by_text_t
access_heaptuple_tests_keyed_by_text(size_t len)
{
	access_heaptuple_tests_keyed_by_text_t made;
	AccessHeaptupleColumn columns[9];
	access_TupleUsizeOrderKeyType_t key[8];
	StdVec		first;
	uint8		k = 'k';

	for (int i = 0; i < 8; i++)
	{
		columns[i] = access_heaptuple_tests_column(TEXTOID, -1, false);
		key[i] = access_heaptuple_tests_keyed((size_t) i, ENCODING_KEY_ORDER_ASC, TEXTOID);
	}
	columns[8] = access_heaptuple_tests_column(INT4OID, 4, true);
	made.layout = access_heaptuple_tests_layout(columns, 9, key, 8);
	std_vec_new(&first, ACCESS_HEAPTUPLE_BYTES);
	std_vec_resize(&first, len, &k, ACCESS_HEAPTUPLE_BYTES);
	made.values[0] = (access_heaptuple_tests_Got)
	{
		.tag = ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA, .bytes = first
	};
	for (int i = 1; i < 8; i++)
	{
		uint8		letter = STD_ADD(u8, 'a', (uint8) i, 1037, 56);
		uint8		three[3] = {letter, letter, letter};

		made.values[i] = access_heaptuple_tests_got_bytes(ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA, three, 3);
	}
	made.values[8] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, pg_word_from_int32(5));
	return made;
}

/* access_heaptuple_tests_keyed_by_text_drop frees keyed_by_text's layout and row. */
static void
access_heaptuple_tests_keyed_by_text_drop(access_heaptuple_tests_keyed_by_text_t *made)
{
	access_heaptuple_RowLayout_drop(&made->layout);
	for (int i = 0; i < 9; i++)
		access_heaptuple_tests_Got_drop(&made->values[i]);
}

/* access_heaptuple_tests_parse parses a formed tuple, or panics at a place with a message. */
static EncodingTupleTuple
access_heaptuple_tests_parse(const StdVec *tuple, uint32 line, uint32 col, const char *message)
{
	EncodingTupleTuple parsed;

	if (!encoding_tuple_Tuple_parse(tuple->ptr, tuple->len, &parsed))
		std_panic(RUST_FILE, line, col, message, strlen(message));
	return parsed;
}

/*
 * The fork's own (design item 4's form D): a key column under, at, and one
 * byte over its bound forms whole or cut in the key, and each row deforms to
 * its values.
 */
void
test_access_heaptuple_tests_a_key_column_under_at_and_over_its_bound_forms_and_deforms_to_its_value(void)
{
	size_t		bound = CHUNKS_MAX_ENTRY / 16 - 33;
	size_t		lens[3];
	bool		cuts[3] = {false, false, true};

	lens[0] = STD_SUB(usize, bound, 3, 1048, 29);
	lens[1] = STD_SUB(usize, bound, 2, 1048, 49);
	lens[2] = STD_SUB(usize, bound, 1, 1048, 69);
	for (int c = 0; c < 3; c++)
	{
		size_t		len = lens[c];
		bool		cut = cuts[c];
		access_heaptuple_tests_keyed_by_text_t made = access_heaptuple_tests_keyed_by_text(len);
		AccessHeaptupleSet given[9];
		AccessHeaptupleFormed formed;
		size_t		first;
		EncodingTupleTuple parsed;
		EncodingTupleFields fields;
		EncodingTupleField field;
		bool		holds = true;

		STD_ASSERT(1050, 13, made.layout.bound == bound, "layout.bound == bound");
		access_heaptuple_tests_given(made.values, 9, given);
		formed = access_heaptuple_tests_formed_ok(&made.layout, given, 9);
		first = cut ? STD_ADD(usize, bound, 32, 1052, 34) : STD_ADD(usize, len, 2, 1052, 54);
		STD_ASSERT(1053, 13, formed.key.len == STD_ADD(usize, first, 7 * 5, 1053, 41),
				   "formed.key.len() == first + 7 * 5");
		parsed = access_heaptuple_tests_parse(&formed.tuple, 1054, 72, "a formed tuple doesn't parse");
		fields = encoding_tuple_Tuple_fields(&parsed);
		field = encoding_tuple_Fields_at(&fields, 0);
		if (cut)
		{
			EncodingTupleField expected = {.tag = ENCODING_TUPLE_FIELD_VALUE, .u.value = {made.values[0].bytes.ptr, len}};

			holds = encoding_tuple_Field_PartialEq_eq(&field, &expected);
		}
		else
			holds = field.tag == ENCODING_TUPLE_FIELD_NULL;
		STD_ASSERT(1056, 13, holds,
				   "if cut { field == Field::Value(&vec![b'k'; len]) } else { field == Field::Null }");
		holds = true;
		for (size_t i = 1; i < 8 && holds; i++)
		{
			EncodingTupleFields again = encoding_tuple_Tuple_fields(&parsed);

			holds = encoding_tuple_Fields_at(&again, i).tag == ENCODING_TUPLE_FIELD_NULL;
		}
		STD_ASSERT(1057, 13, holds, "(1..8).all(|i| parsed.fields().at(i) == Field::Null)");
		STD_ASSERT(1058, 13,
				   access_heaptuple_tests_deformed_eq(&made.layout, access_heaptuple_tests_bytes(&formed.key),
													  access_heaptuple_tests_bytes(&formed.tuple), 0, 9, made.values, 9),
				   "ok(deformed(&layout, &formed.key, &formed.tuple, 0, 9)) == values");
		std_range_check(1, 3, 9, RUST_FILE, 1059, 80);
		STD_ASSERT(1059, 13,
				   access_heaptuple_tests_deformed_eq(&made.layout, access_heaptuple_tests_bytes(&formed.key),
													  access_heaptuple_tests_bytes(&formed.tuple), 1, 3, made.values + 1, 2),
				   "ok(deformed(&layout, &formed.key, &formed.tuple, 1, 3)) == values[1..3]");
		access_heaptuple_Formed_drop(&formed);
		access_heaptuple_tests_keyed_by_text_drop(&made);
	}
}

/* access_heaptuple_tests_ending replaces a keyed_by_text row's first value with 1,499 k's and an ending byte. */
static void
access_heaptuple_tests_ending(access_heaptuple_tests_keyed_by_text_t *made, uint8 ending)
{
	uint8		k = 'k';

	access_heaptuple_tests_Got_drop(&made->values[0]);
	std_vec_new(&made->values[0].bytes, ACCESS_HEAPTUPLE_BYTES);
	std_vec_resize(&made->values[0].bytes, 1499, &k, ACCESS_HEAPTUPLE_BYTES);
	std_vec_push_Checked(&made->values[0].bytes, &ending, ACCESS_HEAPTUPLE_BYTES);
	made->values[0].tag = ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA;
}

/*
 * The fork's own: two rows whose cut keys share their first bytes have keys
 * that differ, in their addresses, and each deforms to its own value.
 */
void
test_access_heaptuple_tests_rows_whose_cut_keys_share_their_first_bytes_have_their_own_keys(void)
{
	access_heaptuple_tests_keyed_by_text_t one = access_heaptuple_tests_keyed_by_text(1500);
	access_heaptuple_tests_keyed_by_text_t other = access_heaptuple_tests_keyed_by_text(1500);
	AccessHeaptupleSet given[9];
	AccessHeaptupleFormed first;
	AccessHeaptupleFormed second;
	size_t		bound = one.layout.bound;
	bool		holds;

	access_heaptuple_tests_ending(&one, '1');
	access_heaptuple_tests_ending(&other, '2');
	access_heaptuple_tests_given(one.values, 9, given);
	first = access_heaptuple_tests_formed_ok(&one.layout, given, 9);
	access_heaptuple_tests_given(other.values, 9, given);
	second = access_heaptuple_tests_formed_ok(&one.layout, given, 9);
	std_range_to_check(bound, first.key.len, RUST_FILE, 1072, 17);
	std_range_to_check(bound, second.key.len, RUST_FILE, 1072, 46);
	holds = memcmp(first.key.ptr, second.key.ptr, bound) == 0 &&
		!access_heaptuple_tests_vec_eq(&first.key, second.key.ptr, second.key.len);
	STD_ASSERT(1072, 9, holds, "first.key[..layout.bound] == second.key[..layout.bound] && first.key != second.key");
	STD_ASSERT(1073, 9, first.key.len == second.key.len, "first.key.len() == second.key.len()");
	STD_ASSERT(1074, 9,
			   access_heaptuple_tests_deformed_eq(&one.layout, access_heaptuple_tests_bytes(&first.key),
												  access_heaptuple_tests_bytes(&first.tuple), 0, 9, one.values, 9),
			   "ok(deformed(&layout, &first.key, &first.tuple, 0, 9)) == one");
	STD_ASSERT(1075, 9,
			   access_heaptuple_tests_deformed_eq(&one.layout, access_heaptuple_tests_bytes(&second.key),
												  access_heaptuple_tests_bytes(&second.tuple), 0, 9, other.values, 9),
			   "ok(deformed(&layout, &second.key, &second.tuple, 0, 9)) == other");
	access_heaptuple_Formed_drop(&first);
	access_heaptuple_Formed_drop(&second);
	access_heaptuple_tests_keyed_by_text_drop(&one);
	access_heaptuple_tests_keyed_by_text_drop(&other);
}

/*
 * access_heaptuple_tests_stored_t is a Store's blob: its address, bytes,
 * locator, and an overflow chunk's addresses' locators.
 */
typedef struct access_heaptuple_tests_stored_t
{
	uint8		address[32];
	StdVec		bytes;
	ChunksTrailerLocator locator;
	StdVec		inner;
} access_heaptuple_tests_stored_t;

/*
 * access_heaptuple_tests_Store is Store: the blobs a test's rows were placed
 * through, by address, a map kept as a vector.
 */
typedef struct access_heaptuple_tests_Store
{
	StdVec		blobs;
} access_heaptuple_tests_Store;

/* ACCESS_HEAPTUPLE_STORED is a Store's element layout. */
#define ACCESS_HEAPTUPLE_STORED sizeof(access_heaptuple_tests_stored_t), _Alignof(access_heaptuple_tests_stored_t)

/* access_heaptuple_tests_Store_Default_default is Store's derived Default: a store of no blobs. */
static access_heaptuple_tests_Store
access_heaptuple_tests_Store_Default_default(void)
{
	access_heaptuple_tests_Store store;

	std_vec_new(&store.blobs, ACCESS_HEAPTUPLE_STORED);
	return store;
}

/* access_heaptuple_tests_stored_drop frees a Store's blob, as a vector's element. */
static void
access_heaptuple_tests_stored_drop(void *stored_)
{
	access_heaptuple_tests_stored_t *stored = stored_;

	std_vec_free_Checked(&stored->bytes, ACCESS_HEAPTUPLE_BYTES);
	std_vec_free_Checked(&stored->inner, ACCESS_HEAPTUPLE_LOCATOR);
}

/* access_heaptuple_tests_Store_get returns the blob of an address, or NULL. */
static const access_heaptuple_tests_stored_t *
access_heaptuple_tests_Store_get(const access_heaptuple_tests_Store *self, const uint8 *address)
{
	for (size_t i = 0; i < self->blobs.len; i++)
		if (memcmp(STD_VEC_AT(&self->blobs, access_heaptuple_tests_stored_t, i).address, address, 32) == 0)
			return &STD_VEC_AT(&self->blobs, access_heaptuple_tests_stored_t, i);
	return NULL;
}

/*
 * access_heaptuple_tests_Store_insert puts a blob under its address, as
 * BTreeMap::insert does, dropping the one it replaces.
 */
static void
access_heaptuple_tests_Store_insert(access_heaptuple_tests_Store *self, access_heaptuple_tests_stored_t *stored)
{
	access_heaptuple_tests_stored_t *found = (access_heaptuple_tests_stored_t *)
		access_heaptuple_tests_Store_get(self, stored->address);

	if (found != NULL)
	{
		access_heaptuple_tests_stored_drop(found);
		*found = *stored;
		return;
	}
	std_vec_push_Checked(&self->blobs, stored, ACCESS_HEAPTUPLE_STORED);
}

/* access_heaptuple_tests_asked_t is an Asked's entry: whether a blob is an overflow chunk, and its bytes. */
typedef struct access_heaptuple_tests_asked_t
{
	bool		chunk;
	StdVec		bytes;
} access_heaptuple_tests_asked_t;

/* ACCESS_HEAPTUPLE_ASKED is an Asked's element layout. */
#define ACCESS_HEAPTUPLE_ASKED sizeof(access_heaptuple_tests_asked_t), _Alignof(access_heaptuple_tests_asked_t)

/* access_heaptuple_tests_asked_drop frees an Asked's entry, as a vector's element. */
static void
access_heaptuple_tests_asked_drop(void *asked)
{
	std_vec_free_Checked(&((access_heaptuple_tests_asked_t *) asked)->bytes, ACCESS_HEAPTUPLE_BYTES);
}

/*
 * access_heaptuple_tests_Store_placed is Store::placed: it places a row of
 * values through the store, writing each blob the row asks for, and answers
 * the row and the blobs it asked for in order (a Vec of asked_t).
 */
static bool
access_heaptuple_tests_Store_placed(access_heaptuple_tests_Store *self, const AccessHeaptupleRowLayout *layout,
									const AccessHeaptupleSet *values, size_t len, AccessHeaptupleFormed *formed,
									StdVec *asked, PgPgError *error)
{
	AccessHeaptuplePlacing placing;
	AccessHeaptupleBlob blob;

	if (!access_heaptuple_RowLayout_placing(layout, ACCESS_HEAPTUPLE_TESTS_REVERSED, ACCESS_HEAPTUPLE_TESTS_NAMES, values,
											len, NULL, 0, false, ACCESS_HEAPTUPLE_TOAST_TUPLE_TARGET, &placing, error))
		return false;
	std_vec_new(asked, ACCESS_HEAPTUPLE_ASKED);
	while (access_heaptuple_Placing_blob(&placing, &blob))
	{
		access_heaptuple_tests_stored_t stored = {0};
		access_heaptuple_tests_asked_t entry;
		ChunksAddressAddress address;
		ChunksTrailerLocator locator;

		entry.chunk = blob.tag == ACCESS_HEAPTUPLE_BLOB_CHUNK;
		if (entry.chunk)
		{
			std_slice_to_vec_Checked(&entry.bytes, blob.u.chunk.node.ptr, blob.u.chunk.node.len, ACCESS_HEAPTUPLE_BYTES);
			std_slice_to_vec_Checked(&stored.inner, blob.u.chunk.locators, blob.u.chunk.locators_len,
									 ACCESS_HEAPTUPLE_LOCATOR);
		}
		else
		{
			std_slice_to_vec_Checked(&entry.bytes, blob.u.value.ptr, blob.u.value.len, ACCESS_HEAPTUPLE_BYTES);
			std_vec_new(&stored.inner, ACCESS_HEAPTUPLE_LOCATOR);
		}
		address = chunks_address_Address_of(entry.bytes.ptr, entry.bytes.len);
		locator = chunks_trailer_Locator_at(1, STD_MUL(u64, 64, STD_ADD(u64, (uint64) self->blobs.len, 1,
																		1098, 52),
													   1098, 46),
											64);
		memcpy(stored.address, address.f0, 32);
		std_vec_clone(&stored.bytes, &entry.bytes, ACCESS_HEAPTUPLE_BYTES);
		stored.locator = locator;
		access_heaptuple_tests_Store_insert(self, &stored);
		std_vec_push_Checked(asked, &entry, ACCESS_HEAPTUPLE_ASKED);
		if (!access_heaptuple_Placing_wrote(&placing, address, locator, error))
		{
			access_heaptuple_Placing_drop(&placing);
			std_vec_drop_Checked(asked, ACCESS_HEAPTUPLE_ASKED, access_heaptuple_tests_asked_drop);
			return false;
		}
	}
	if (!access_heaptuple_Placing_formed(&placing, formed, error))
	{
		std_vec_drop_Checked(asked, ACCESS_HEAPTUPLE_ASKED, access_heaptuple_tests_asked_drop);
		return false;
	}
	return true;
}

static bool access_heaptuple_tests_Store_value(const access_heaptuple_tests_Store *self,
											   const AccessHeaptupleRowLayout *layout, StdBytes key, size_t column,
											   const chunks_TupleU32Locator_t *addresses, size_t addresses_len,
											   AccessHeaptupleDeformed deformed, access_heaptuple_tests_Got *result,
											   PgPgError *error);

/* access_heaptuple_tests_read_t is Store::read's and Store::value's set's captures. */
typedef struct access_heaptuple_tests_read_t
{
	const access_heaptuple_tests_Store *self;
	const AccessHeaptupleRowLayout *layout;
	StdBytes	key;
	size_t		column;
	const chunks_TupleU32Locator_t *addresses;
	size_t		addresses_len;
	StdVec	   *values;
	bool		has_value;
	access_heaptuple_tests_Got value;
} access_heaptuple_tests_read_t;

/*
 * access_heaptuple_tests_Store_read_closure_0 is read's set: each column's
 * value, read from the store where it is kept.
 */
static bool
access_heaptuple_tests_Store_read_closure_0(void *captures, AccessHeaptupleDeformed deformed, PgPgError *error)
{
	access_heaptuple_tests_read_t *read = captures;
	access_heaptuple_tests_Got value;

	if (!access_heaptuple_tests_Store_value(read->self, read->layout, read->key, read->values->len, read->addresses,
											read->addresses_len, deformed, &value, error))
		return false;
	std_vec_push_Checked(read->values, &value, ACCESS_HEAPTUPLE_GOT);
	return true;
}

/*
 * access_heaptuple_tests_Store_read is Store::read: what a placed row's
 * columns deform to, read from the store where kept.
 */
static StdVec
access_heaptuple_tests_Store_read(const access_heaptuple_tests_Store *self, const AccessHeaptupleRowLayout *layout,
								  const AccessHeaptupleFormed *formed, size_t upto)
{
	StdVec		values;
	access_heaptuple_tests_read_t read = {self, layout, {formed->key.ptr, formed->key.len}, 0, formed->addresses.ptr,
	formed->addresses.len, &values};
	bool		has;
	size_t		count;
	PgPgError	error;

	std_vec_new(&values, ACCESS_HEAPTUPLE_GOT);
	access_heaptuple_tests_ok(access_heaptuple_RowLayout_deform(layout, formed->key.ptr, formed->key.len,
																formed->tuple.ptr, formed->tuple.len, false, NULL, 0, 0,
																upto, access_heaptuple_tests_Store_read_closure_0,
																&read, &has, &count, &error), &error);
	return values;
}

/*
 * access_heaptuple_tests_Store_value_closure_0 is value's stored: a named
 * blob, checked to be written and named by its locator.
 */
static const access_heaptuple_tests_stored_t *
access_heaptuple_tests_Store_value_closure_0(const access_heaptuple_tests_Store *self, StdBytes address, size_t at,
											 const chunks_TupleU32Locator_t *addresses, size_t addresses_len)
{
	const access_heaptuple_tests_stored_t *stored;
	bool		named = false;

	if (address.len != 32)
		std_panic(RUST_FILE, 1129, 79, "an address isn't 32 bytes", sizeof("an address isn't 32 bytes") - 1);
	stored = access_heaptuple_tests_Store_get(self, address.ptr);
	if (stored == NULL)
		std_panic(RUST_FILE, 1130, 69, "a blob the row names wasn't written",
				  sizeof("a blob the row names wasn't written") - 1);
	for (size_t i = 0; i < addresses_len && !named; i++)
		named = addresses[i].f0 == (uint32) at && chunks_trailer_Locator_PartialEq_eq(&addresses[i].f1, &stored->locator);
	STD_ASSERT_MSG(1131, 17, named, "the tuple doesn't name the blob's locator");
	return stored;
}

/*
 * access_heaptuple_tests_Store_value_closure_3 is value's overflowed set: the
 * chunk's field's value, read again through the store.
 */
static bool
access_heaptuple_tests_Store_value_closure_3(void *captures, AccessHeaptupleDeformed deformed, PgPgError *error)
{
	access_heaptuple_tests_read_t *read = captures;

	if (read->has_value)
		access_heaptuple_tests_Got_drop(&read->value);
	read->has_value = false;
	if (!access_heaptuple_tests_Store_value(read->self, read->layout, read->key, read->column, read->addresses,
											read->addresses_len, deformed, &read->value, error))
		return false;
	read->has_value = true;
	return true;
}

/*
 * access_heaptuple_tests_Store_value is Store::value: a column's deformed
 * value, read from the store when it is kept out of line or in an overflow
 * chunk, checking that the tuple that holds its address names its locator.
 */
static bool
access_heaptuple_tests_Store_value(const access_heaptuple_tests_Store *self, const AccessHeaptupleRowLayout *layout,
								   StdBytes key, size_t column, const chunks_TupleU32Locator_t *addresses,
								   size_t addresses_len, AccessHeaptupleDeformed deformed,
								   access_heaptuple_tests_Got *result, PgPgError *error)
{
	if (deformed.tag == ACCESS_HEAPTUPLE_DEFORMED_OUT_OF_LINE)
	{
		const access_heaptuple_tests_stored_t *stored =
			access_heaptuple_tests_Store_value_closure_0(self, deformed.u.out_of_line.address, deformed.u.out_of_line.at,
														 addresses, addresses_len);

		STD_ASSERT(1137, 21, (uint64) stored->bytes.len == deformed.u.out_of_line.length,
				   "bytes.len() as u64 == length");
		*result = access_heaptuple_tests_got_bytes(ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA, stored->bytes.ptr, stored->bytes.len);
		return true;
	}
	if (deformed.tag == ACCESS_HEAPTUPLE_DEFORMED_OVERFLOWED)
	{
		const access_heaptuple_tests_stored_t *stored =
			access_heaptuple_tests_Store_value_closure_0(self, deformed.u.overflowed.address, deformed.u.overflowed.at,
														 addresses, addresses_len);
		ChunksHeaderHeader header;
		size_t		body;
		ChunksErrorError chunk_error;
		StdVec		held;
		access_heaptuple_tests_read_t read = {self, layout, key, column};
		bool		has;
		size_t		count;
		bool		ok;

		if (!chunks_header_Header_read(stored->bytes.ptr, stored->bytes.len, &header, &body, &chunk_error))
			std_panic(RUST_FILE, 1142, 80, "a chunk's header doesn't read",
					  sizeof("a chunk's header doesn't read") - 1);
		STD_ASSERT(1143, 21,
				   header.kind == CHUNKS_HEADER_BLOB_LEAF && chunks_header_Addresses_len(&header.addresses) == stored->inner.len,
				   "header.kind == BLOB_LEAF && header.addresses.len() == inner.len()");
		std_vec_with_capacity_Checked(&held, stored->inner.len, ACCESS_HEAPTUPLE_ADDRESS);
		for (size_t k = 0; k < stored->inner.len; k++)
		{
			chunks_TupleU32Locator_t address;

			address.f0 = (uint32) chunks_header_Addresses_offset(&header.addresses, k);
			std_index_check(k, stored->inner.len, RUST_FILE, 1145, 86);
			address.f1 = STD_VEC_AT(&stored->inner, ChunksTrailerLocator, k);
			std_vec_push_Checked(&held, &address, ACCESS_HEAPTUPLE_ADDRESS);
		}
		chunks_header_Header_drop(&header);
		read.addresses = held.ptr;
		read.addresses_len = held.len;
		std_range_from_check(body, stored->bytes.len, RUST_FILE, 1147, 53);
		ok = access_heaptuple_RowLayout_overflowed(layout, key.ptr, key.len, column,
												   (const uint8 *) stored->bytes.ptr + body, stored->bytes.len - body,
												   deformed.u.overflowed.field, deformed.u.overflowed.before,
												   access_heaptuple_tests_Store_value_closure_3, &read, &has, &count,
												   error);
		std_vec_free_Checked(&held, ACCESS_HEAPTUPLE_ADDRESS);
		if (!ok)
		{
			if (read.has_value)
				access_heaptuple_tests_Got_drop(&read.value);
			return false;
		}
		if (!read.has_value)
			std_panic(RUST_FILE, 1151, 45, "an overflowed field gave no value",
					  sizeof("an overflowed field gave no value") - 1);
		*result = read.value;
		return true;
	}
	*result = access_heaptuple_tests_got(deformed);
	return true;
}

/* access_heaptuple_tests_Store_drop frees a Store's blobs. */
static void
access_heaptuple_tests_Store_drop(access_heaptuple_tests_Store *self)
{
	std_vec_drop_Checked(&self->blobs, ACCESS_HEAPTUPLE_STORED, access_heaptuple_tests_stored_drop);
}

/* access_heaptuple_tests_texts_t is texts' answer: a layout and a row of it. */
typedef struct access_heaptuple_tests_texts_t
{
	AccessHeaptupleRowLayout layout;
	access_heaptuple_tests_Got values[8];
	size_t		len;
} access_heaptuple_tests_texts_t;

/*
 * access_heaptuple_tests_texts is tests' texts: a layout of a table without
 * a primary key of text columns of the storage kinds, then an int4, and a
 * row of it whose texts are the lengths of their own letter, from a on.
 */
static access_heaptuple_tests_texts_t
access_heaptuple_tests_texts(const AccessHeaptupleStorage *storages, const size_t *lengths, size_t len)
{
	access_heaptuple_tests_texts_t made = {.len = len + 1};
	AccessHeaptupleColumn columns[8];
	uint8		i = 0;

	for (size_t c = 0; c < len; c++)
	{
		columns[c] = access_heaptuple_tests_column(TEXTOID, -1, false);
		columns[c].storage = storages[c];
	}
	columns[len] = access_heaptuple_tests_column(INT4OID, 4, true);
	for (size_t c = 0; c < len; c++)
	{
		uint8		letter = STD_ADD(u8, 'a', i, 1164, 89);

		i = std_forward_u8_Checked(i, 1);
		std_vec_new(&made.values[c].bytes, ACCESS_HEAPTUPLE_BYTES);
		std_vec_resize(&made.values[c].bytes, lengths[c], &letter, ACCESS_HEAPTUPLE_BYTES);
		made.values[c].tag = ACCESS_HEAPTUPLE_TESTS_GOT_VARLENA;
	}
	made.values[len] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, pg_word_from_int32(3));
	made.layout = access_heaptuple_tests_layout(columns, len + 1, NULL, 0);
	return made;
}

/* access_heaptuple_tests_texts_drop frees texts' layout and row. */
static void
access_heaptuple_tests_texts_drop(access_heaptuple_tests_texts_t *made)
{
	access_heaptuple_RowLayout_drop(&made->layout);
	for (size_t i = 0; i < made->len; i++)
		access_heaptuple_tests_Got_drop(&made->values[i]);
}

/*
 * access_heaptuple_tests_moved is tests' moved: the blobs a row asked for that
 * are values, each its first byte, a text's letter.
 */
static StdVec
access_heaptuple_tests_moved(const StdVec *asked)
{
	StdVec		moved;

	std_vec_new(&moved, ACCESS_HEAPTUPLE_BYTES);
	for (size_t i = 0; i < asked->len; i++)
	{
		const access_heaptuple_tests_asked_t *entry = &STD_VEC_AT(asked, access_heaptuple_tests_asked_t, i);

		if (entry->chunk)
			continue;
		std_index_check(0, entry->bytes.len, RUST_FILE, 1171, 67);
		std_vec_push_Checked(&moved, entry->bytes.ptr, ACCESS_HEAPTUPLE_BYTES);
	}
	return moved;
}

/* access_heaptuple_tests_moved_eq reports whether moved gives some letters, freeing what it gave. */
static bool
access_heaptuple_tests_moved_eq(const StdVec *asked, const char *letters)
{
	StdVec		moved = access_heaptuple_tests_moved(asked);
	bool		same = access_heaptuple_tests_vec_eq(&moved, letters, strlen(letters));

	std_vec_free_Checked(&moved, ACCESS_HEAPTUPLE_BYTES);
	return same;
}

/* access_heaptuple_tests_placed_t is Store::placed's answer in a test: the row and the blobs it asked for. */
typedef struct access_heaptuple_tests_placed_t
{
	AccessHeaptupleFormed formed;
	StdVec		asked;
} access_heaptuple_tests_placed_t;

/* access_heaptuple_tests_placed_ok places a texts row through a store where the call must succeed. */
static access_heaptuple_tests_placed_t
access_heaptuple_tests_placed_ok(access_heaptuple_tests_Store *store, const access_heaptuple_tests_texts_t *made)
{
	access_heaptuple_tests_placed_t placed;
	AccessHeaptupleSet given[8];
	PgPgError	error;

	access_heaptuple_tests_given(made->values, made->len, given);
	access_heaptuple_tests_ok(access_heaptuple_tests_Store_placed(store, &made->layout, given, made->len, &placed.formed,
																  &placed.asked, &error), &error);
	return placed;
}

/* access_heaptuple_tests_placed_drop frees a placed row and its asked blobs. */
static void
access_heaptuple_tests_placed_drop(access_heaptuple_tests_placed_t *placed)
{
	access_heaptuple_Formed_drop(&placed->formed);
	std_vec_drop_Checked(&placed->asked, ACCESS_HEAPTUPLE_ASKED, access_heaptuple_tests_asked_drop);
}

/*
 * access_heaptuple_tests_read_eq reports whether a placed row reads back from
 * the store to its values, freeing what it read.
 */
static bool
access_heaptuple_tests_read_eq(const access_heaptuple_tests_Store *store, const access_heaptuple_tests_texts_t *made,
							   const AccessHeaptupleFormed *formed, size_t upto)
{
	StdVec		read = access_heaptuple_tests_Store_read(store, &made->layout, formed, upto);
	bool		same = access_heaptuple_tests_gots_eq(read.ptr, read.len, made->values, made->len);

	access_heaptuple_tests_gots_drop(&read);
	return same;
}

/*
 * access_heaptuple_tests_reference_tuple is the tests' tuple of one field out
 * of line: a reference of 70,000 bytes to a blob of sevens.
 */
static StdVec
access_heaptuple_tests_reference_tuple(void)
{
	uint8		sevens[32];
	StdVec		reference;
	StdVec		external;
	StdBytes	field;
	bool		yes = true;
	encoding_tuple_write_overflow_t none = {0};

	memset(sevens, 7, sizeof(sevens));
	encoding_tuple_reference(70000, sevens, &reference);
	field = access_heaptuple_tests_bytes(&reference);
	encoding_tuple_write_with_external(&field, 1, &yes, 1, false, none, &external);
	std_vec_free_Checked(&reference, ACCESS_HEAPTUPLE_BYTES);
	return external;
}

/* The fork's own: a value kept out of line deforms to its length and its blob's address. */
void
test_access_heaptuple_tests_a_value_kept_out_of_line_deforms_to_where_it_is(void)
{
	StdVec		external = access_heaptuple_tests_reference_tuple();
	AccessHeaptupleColumn column = access_heaptuple_tests_column(TEXTOID, -1, false);
	AccessHeaptupleRowLayout text_layout = access_heaptuple_tests_layout(&column, 1, NULL, 0);
	uint8		sevens[32];
	access_heaptuple_tests_Got expected;

	memset(sevens, 7, sizeof(sevens));
	expected = access_heaptuple_tests_got_bytes(ACCESS_HEAPTUPLE_TESTS_GOT_OUT_OF_LINE, sevens, 32);
	expected.word = 70000;
	STD_ASSERT(1180, 9,
			   access_heaptuple_tests_deformed_eq(&text_layout, access_heaptuple_tests_slice(NULL, 0),
												  access_heaptuple_tests_bytes(&external), 0, 1, &expected, 1),
			   "ok(deformed(&text, &[], &external, 0, 1)) == [Got::OutOfLine(70_000, vec![7; 32])]");
	access_heaptuple_tests_Got_drop(&expected);
	access_heaptuple_RowLayout_drop(&text_layout);
	std_vec_free_Checked(&external, ACCESS_HEAPTUPLE_BYTES);
}

/*
 * The fork's own (design item 5): a row past the threshold moves its biggest
 * EXTENDED and EXTERNAL values out of line first, until its tuple is under the
 * target, keeps a MAIN value inline, and deforms to its values.
 */
void
test_access_heaptuple_tests_a_row_past_the_threshold_moves_its_biggest_extended_and_external_values_out_first(void)
{
	static const AccessHeaptupleStorage kinds[4] = {ACCESS_HEAPTUPLE_STORAGE_EXTENDED, ACCESS_HEAPTUPLE_STORAGE_EXTERNAL,
	ACCESS_HEAPTUPLE_STORAGE_MAIN, ACCESS_HEAPTUPLE_STORAGE_EXTERNAL};
	static const size_t lengths[4] = {900, 1000, 1800, 1000};
	access_heaptuple_tests_texts_t made = access_heaptuple_tests_texts(kinds, lengths, 4);
	access_heaptuple_tests_Store store;
	access_heaptuple_tests_placed_t placed;
	EncodingTupleTuple parsed;
	EncodingTupleField field;

	store = access_heaptuple_tests_Store_Default_default();
	placed = access_heaptuple_tests_placed_ok(&store, &made);
	STD_ASSERT(1193, 9, access_heaptuple_tests_moved_eq(&placed.asked, "bda") && placed.asked.len == 3,
			   "moved(&asked) == b\"bda\" && asked.len() == 3");
	STD_ASSERT(1194, 9,
			   placed.formed.addresses.len == 3 && placed.formed.tuple.len < ACCESS_HEAPTUPLE_TOAST_TUPLE_TARGET,
			   "formed.addresses.len() == 3 && formed.tuple.len() < TOAST_TUPLE_TARGET");
	parsed = access_heaptuple_tests_parse(&placed.formed.tuple, 1195, 68, "a placed row doesn't parse");
	field = encoding_tuple_Tuple_field(&parsed, 2);
	STD_ASSERT(1196, 9, field.tag == ENCODING_TUPLE_FIELD_VALUE && field.u.value.len == 1800,
			   "matches!(parsed.field(2), Field::Value(bytes) if bytes.len() == 1800)");
	STD_ASSERT(1197, 9, access_heaptuple_tests_read_eq(&store, &made, &placed.formed, 5),
			   "ok(store.read(&layout, &formed, 5)) == values");
	access_heaptuple_tests_placed_drop(&placed);
	access_heaptuple_tests_Store_drop(&store);
	access_heaptuple_tests_texts_drop(&made);
}

/*
 * The fork's own: a row whose values are all MAIN moves its biggest out only
 * while the row is too large for a tree's node, and keeps a row under a node's
 * room whole.
 */
void
test_access_heaptuple_tests_main_values_move_out_only_while_the_row_is_too_large_for_a_node(void)
{
	static const AccessHeaptupleStorage kinds[2] = {ACCESS_HEAPTUPLE_STORAGE_MAIN, ACCESS_HEAPTUPLE_STORAGE_MAIN};
	static const size_t big[2] = {9000, 9000};
	static const size_t small[2] = {7000, 7000};
	access_heaptuple_tests_texts_t made = access_heaptuple_tests_texts(kinds, big, 2);
	access_heaptuple_tests_Store store;
	access_heaptuple_tests_placed_t placed;

	store = access_heaptuple_tests_Store_Default_default();
	placed = access_heaptuple_tests_placed_ok(&store, &made);
	STD_ASSERT(1208, 9, access_heaptuple_tests_moved_eq(&placed.asked, "a") && placed.asked.len == 1,
			   "moved(&asked) == b\"a\" && asked.len() == 1");
	STD_ASSERT(1209, 9, access_heaptuple_tests_read_eq(&store, &made, &placed.formed, 3),
			   "ok(store.read(&layout, &formed, 3)) == values");
	access_heaptuple_tests_placed_drop(&placed);
	access_heaptuple_tests_texts_drop(&made);
	made = access_heaptuple_tests_texts(kinds, small, 2);
	placed = access_heaptuple_tests_placed_ok(&store, &made);
	STD_ASSERT(1212, 9, placed.asked.len == 0 && placed.formed.addresses.len == 0,
			   "asked.is_empty() && formed.addresses.is_empty()");
	STD_ASSERT(1213, 9, access_heaptuple_tests_read_eq(&store, &made, &placed.formed, 3),
			   "ok(store.read(&layout, &formed, 3)) == values");
	access_heaptuple_tests_placed_drop(&placed);
	access_heaptuple_tests_Store_drop(&store);
	access_heaptuple_tests_texts_drop(&made);
}

/* access_heaptuple_tests_kinds_are reports whether the blobs asked for are chunks in a pattern of 'c' and 'v'. */
static bool
access_heaptuple_tests_kinds_are(const StdVec *asked, const char *kinds)
{
	if (asked->len != strlen(kinds))
		return false;
	for (size_t i = 0; i < asked->len; i++)
		if (STD_VEC_AT(asked, access_heaptuple_tests_asked_t, i).chunk != (kinds[i] == 'c'))
			return false;
	return true;
}

/*
 * The fork's own (CommPF 5's case): a row that needs a value moved out of line
 * and two nested overflow chunks asks for the moved value, then the inner
 * chunk, then the outer one, and deforms to its values.
 */
void
test_access_heaptuple_tests_a_row_with_a_moved_value_and_two_nested_overflow_chunks_walks_every_request(void)
{
	static const AccessHeaptupleStorage kinds[7] = {ACCESS_HEAPTUPLE_STORAGE_EXTENDED, ACCESS_HEAPTUPLE_STORAGE_PLAIN,
		ACCESS_HEAPTUPLE_STORAGE_PLAIN, ACCESS_HEAPTUPLE_STORAGE_PLAIN, ACCESS_HEAPTUPLE_STORAGE_PLAIN,
	ACCESS_HEAPTUPLE_STORAGE_PLAIN, ACCESS_HEAPTUPLE_STORAGE_PLAIN};
	static const size_t lengths[7] = {3000, 6000, 6000, 6000, 6000, 6000, 6000};
	access_heaptuple_tests_texts_t made;
	access_heaptuple_tests_Store store;
	access_heaptuple_tests_placed_t placed;
	ChunksAddressAddress inner;
	const StdVec *outer;
	bool		found = false;
	EncodingTupleTuple parsed;
	StdBytes	overflow;
	size_t		overflow_at;

	made = access_heaptuple_tests_texts(kinds, lengths, 7);
	store = access_heaptuple_tests_Store_Default_default();
	placed = access_heaptuple_tests_placed_ok(&store, &made);
	STD_ASSERT(1227, 9,
			   access_heaptuple_tests_kinds_are(&placed.asked, "vcc") && access_heaptuple_tests_moved_eq(&placed.asked, "a"),
			   "kinds == [false, true, true] && moved(&asked) == b\"a\"");
	std_index_check(1, placed.asked.len, RUST_FILE, 1228, 34);
	inner = chunks_address_Address_of(STD_VEC_AT(&placed.asked, access_heaptuple_tests_asked_t, 1).bytes.ptr,
									  STD_VEC_AT(&placed.asked, access_heaptuple_tests_asked_t, 1).bytes.len);
	std_index_check(2, placed.asked.len, RUST_FILE, 1229, 17);
	outer = &STD_VEC_AT(&placed.asked, access_heaptuple_tests_asked_t, 2).bytes;
	for (size_t i = 0; i + 32 <= outer->len && !found; i++)
		found = memcmp((const uint8 *) outer->ptr + i, inner.f0, 32) == 0;
	STD_ASSERT(1229, 9, found, "asked[2].1.windows(32).any(|window| window == inner.0)");
	parsed = access_heaptuple_tests_parse(&placed.formed.tuple, 1230, 68, "a placed row doesn't parse");
	STD_ASSERT(1231, 9,
			   encoding_tuple_Tuple_overflow(&parsed, &overflow, &overflow_at) && placed.formed.addresses.len == 2,
			   "parsed.overflow().is_some() && formed.addresses.len() == 2");
	STD_ASSERT(1232, 9, access_heaptuple_tests_read_eq(&store, &made, &placed.formed, 8),
			   "ok(store.read(&layout, &formed, 8)) == values");
	access_heaptuple_tests_placed_drop(&placed);
	access_heaptuple_tests_Store_drop(&store);
	access_heaptuple_tests_texts_drop(&made);
}

/*
 * The fork's own (CommPF 5's case): an overflow chunk's first field too long
 * for a chunk by itself moves out of line before the chunk is asked for, and
 * deforms to its value.
 */
void
test_access_heaptuple_tests_a_chunks_first_field_too_long_for_it_moves_out_of_line(void)
{
	static const AccessHeaptupleStorage kinds[2] = {ACCESS_HEAPTUPLE_STORAGE_PLAIN, ACCESS_HEAPTUPLE_STORAGE_PLAIN};
	static const size_t lengths[2] = {100, 20000};
	access_heaptuple_tests_texts_t made = access_heaptuple_tests_texts(kinds, lengths, 2);
	access_heaptuple_tests_Store store;
	access_heaptuple_tests_placed_t placed;
	EncodingTupleTuple parsed;
	EncodingTupleField field;

	store = access_heaptuple_tests_Store_Default_default();
	placed = access_heaptuple_tests_placed_ok(&store, &made);
	STD_ASSERT(1244, 9,
			   access_heaptuple_tests_kinds_are(&placed.asked, "vc") && access_heaptuple_tests_moved_eq(&placed.asked, "b"),
			   "kinds == [false, true] && moved(&asked) == b\"b\"");
	parsed = access_heaptuple_tests_parse(&placed.formed.tuple, 1245, 68, "a placed row doesn't parse");
	field = encoding_tuple_Tuple_field(&parsed, 1);
	STD_ASSERT(1246, 9, field.tag == ENCODING_TUPLE_FIELD_OVERFLOW && field.u.overflow == 0,
			   "matches!(parsed.field(1), Field::Overflow(0))");
	STD_ASSERT(1247, 9, access_heaptuple_tests_read_eq(&store, &made, &placed.formed, 3),
			   "ok(store.read(&layout, &formed, 3)) == values");
	access_heaptuple_tests_placed_drop(&placed);
	access_heaptuple_tests_Store_drop(&store);
	access_heaptuple_tests_texts_drop(&made);
}

/* access_heaptuple_tests_columns returns a vector of some columns, as heap_len takes them. */
static StdVec
access_heaptuple_tests_columns(const AccessHeaptupleColumn *columns, size_t len)
{
	StdVec		vec;

	std_slice_to_vec_Checked(&vec, columns, len, ACCESS_HEAPTUPLE_COLUMN);
	return vec;
}

/*
 * access_heaptuple_tests_heap_len is heap_len of some columns, values, and
 * moved flags, the columns copied for the call.
 */
static size_t
access_heaptuple_tests_heap_len(const AccessHeaptupleColumn *columns, size_t columns_len,
								const AccessHeaptupleSet *values, size_t values_len, const bool *moved,
								size_t moved_len)
{
	StdVec		vec = access_heaptuple_tests_columns(columns, columns_len);
	size_t		len = access_heaptuple_heap_len(&vec, values, values_len, moved, moved_len);

	std_vec_free_Checked(&vec, ACCESS_HEAPTUPLE_COLUMN);
	return len;
}

/* access_heaptuple_tests_assert_len is assert_eq! of two usizes at a place. */
static void
access_heaptuple_tests_assert_len(uint32 line, uint32 col, size_t left, size_t right)
{
	STD_ASSERT_EQ(line, col, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
}

/* access_heaptuple_tests_set_bytes returns a Set of a kind and some bytes it borrows. */
static AccessHeaptupleSet
access_heaptuple_tests_set_bytes(AccessHeaptupleSetTag tag, const void *bytes, size_t len)
{
	AccessHeaptupleSet set = {.tag = tag};

	set.u.varlena.ptr = bytes;
	set.u.varlena.len = len;
	return set;
}

/* access_heaptuple_tests_set_word returns a Set of a word. */
static AccessHeaptupleSet
access_heaptuple_tests_set_word(uint64 word)
{
	AccessHeaptupleSet set = {.tag = ACCESS_HEAPTUPLE_SET_WORD, .u.word = word};

	return set;
}

/*
 * PLAN item 5's heap bytes, after heap_form_tuple: a row's length on Postgres'
 * heap pages is the reference's own lp_len for the same row before any
 * rounding, and the heap bytes round the length to MAXALIGN and add the line
 * pointer.
 */
void
test_access_heaptuple_tests_a_rows_heap_length_is_the_length_heap_gives_it(void)
{
	AccessHeaptupleColumn numeric = access_heaptuple_tests_column(1700, -1, false);
	AccessHeaptupleColumn two[2] = {access_heaptuple_tests_column(INT4OID, 4, true),
	access_heaptuple_tests_column(TEXTOID, -1, false)};
	static const size_t cases[4][2] = {{125, 154}, {126, 155}, {127, 159}, {128, 160}};
	static const bool no[9];
	static const bool second[4] = {false, true, false, false};
	static const uint8 zeros[6];
	uint8		xs[1900];
	AccessHeaptupleColumn four[4];
	AccessHeaptupleSet nulls[4] = {{.tag = ACCESS_HEAPTUPLE_SET_WORD, .u.word = 2}, {.tag = ACCESS_HEAPTUPLE_SET_NULL},
	{.tag = ACCESS_HEAPTUPLE_SET_NULL}, {.tag = ACCESS_HEAPTUPLE_SET_NULL}};
	AccessHeaptupleSet row[4];
	AccessHeaptupleColumn nine[9];
	AccessHeaptupleSet words[9];
	AccessHeaptupleColumn plain[2];
	AccessHeaptupleSet pair[2];
	AccessHeaptupleRowLayout layout;
	AccessHeaptupleFormed formed;

	numeric.	storage = ACCESS_HEAPTUPLE_STORAGE_MAIN;

	memset(xs, 'x', sizeof(xs));
	for (int c = 0; c < 4; c++)
	{
		size_t		n = cases[c][0];
		size_t		lp_len = cases[c][1];
		AccessHeaptupleSet values[2] = {access_heaptuple_tests_set_word(1),
		access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, xs, n)};
		size_t		left = access_heaptuple_tests_heap_len(two, 2, values, 2, no, 2);

		if (left != lp_len)
		{
			StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" bytes of text")};
			StdFmtArg	args[] = {{&n, std_usize_Display_fmt}};
			StdFmtArguments message = {pieces, 2, args};

			std_assert_failed(RUST_FILE, 1262, 13, "==", &left, std_usize_Debug_fmt, &lp_len,
							  std_usize_Debug_fmt, &message);
		}
	}
	four[0] = access_heaptuple_tests_column(INT4OID, 4, true);
	four[1] = access_heaptuple_tests_column(TEXTOID, -1, false);
	four[2] = access_heaptuple_tests_column(INT8OID, 8, true);
	four[3] = numeric;

	access_heaptuple_tests_assert_len(1266, 9, access_heaptuple_tests_heap_len(four, 4, nulls, 4, no, 4),
									  28);
	row[0] = access_heaptuple_tests_set_word(1);
	row[1] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, "r1", 2);
	row[2] = access_heaptuple_tests_set_word(7);
	row[3] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, zeros, 6);
	access_heaptuple_tests_assert_len(1268, 9, access_heaptuple_tests_heap_len(four, 4, row, 4, no, 4),
									  47);
	row[0] = access_heaptuple_tests_set_word(5);
	row[1] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, xs, 1900);
	row[2] = access_heaptuple_tests_set_word(1);
	row[3] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, zeros, 2);
	access_heaptuple_tests_assert_len(1271, 9, access_heaptuple_tests_heap_len(four, 4, row, 4, no, 4),
									  1947);
	access_heaptuple_tests_assert_len(1272, 9,
									  access_heaptuple_tests_heap_len(four, 4, row, 4, second, 4), 28 + 18 + 2 + 8 + 3);
	for (int i = 0; i < 9; i++)
	{
		nine[i] = access_heaptuple_tests_column(INT4OID, 4, true);
		words[i] = access_heaptuple_tests_set_word((uint64) i);
	}
	access_heaptuple_tests_assert_len(1275, 9, access_heaptuple_tests_heap_len(nine, 9, words, 9, no, 9),
									  60);
	nine[1].dropped = true;
	access_heaptuple_tests_assert_len(1277, 9, access_heaptuple_tests_heap_len(nine, 9, words, 9, no, 9),
									  64);
	plain[0] = access_heaptuple_tests_column(INT4OID, 4, true);
	plain[1] = access_heaptuple_tests_column(TEXTOID, -1, false);
	plain[1].storage = ACCESS_HEAPTUPLE_STORAGE_PLAIN;
	pair[0] = access_heaptuple_tests_set_word(1);
	pair[1] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, xs, 10);
	access_heaptuple_tests_assert_len(1280, 9, access_heaptuple_tests_heap_len(plain, 2, pair, 2, no, 2),
									  28 + 4 + 10);
	layout = access_heaptuple_tests_layout(two, 2, NULL, 0);
	pair[1] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, xs, 126);
	formed = access_heaptuple_tests_formed_ok(&layout, pair, 2);
	STD_ASSERT_EQ_U64(1283, 9, formed.heap_bytes, 160 + 4);
	access_heaptuple_Formed_drop(&formed);
	access_heaptuple_RowLayout_drop(&layout);
}

/* access_heaptuple_tests_options_Debug_fmt is the Debug of a pair of Option<usize>s, each a StdOptionRef. */
static bool
access_heaptuple_tests_options_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdOptionRef *options = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&options[0], std_option_Debug_fmt),
								  STD_ITEM(&options[1], std_option_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* access_heaptuple_tests_option_triple_Debug_fmt is the Debug of three Option<usize>s, each a StdOptionRef. */
static bool
access_heaptuple_tests_option_triple_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdOptionRef *options = value;
	StdTupleRef tuple = STD_TUPLE(3, STD_ITEM(&options[0], std_option_Debug_fmt),
								  STD_ITEM(&options[1], std_option_Debug_fmt),
								  STD_ITEM(&options[2], std_option_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* access_heaptuple_tests_option returns a StdOptionRef of an Option<usize> held in a caller's place. */
static StdOptionRef
access_heaptuple_tests_option(bool has, const size_t *value)
{
	StdOptionRef option = {has, {value, std_usize_Debug_fmt}};

	return option;
}

/*
 * test_access_heaptuple_tests_a_whole_deform_answers_the_rows_count_of_values_out_of_line_closure_0_closure_0
 * is count's set, which keeps nothing.
 */
static bool
test_access_heaptuple_tests_a_whole_deform_answers_the_rows_count_of_values_out_of_line_closure_0_closure_0(void *captures,
																											AccessHeaptupleDeformed deformed,
																											PgPgError *error)
{
	return true;
}

/*
 * test_access_heaptuple_tests_a_whole_deform_answers_the_rows_count_of_values_out_of_line_closure_0
 * is the test's count: a deform's count of values out of line up to a column.
 */
static bool
test_access_heaptuple_tests_a_whole_deform_answers_the_rows_count_of_values_out_of_line_closure_0(const AccessHeaptupleRowLayout *text_layout,
																								  const StdVec *external,
																								  size_t upto,
																								  size_t *count)
{
	bool		has;
	PgPgError	error;

	access_heaptuple_tests_ok(access_heaptuple_RowLayout_deform(text_layout, NULL, 0, external->ptr, external->len, false, NULL,
																0, 0, upto,
																test_access_heaptuple_tests_a_whole_deform_answers_the_rows_count_of_values_out_of_line_closure_0_closure_0,
																NULL, &has, count, &error), &error);
	return has;
}

/*
 * Bugs row 98: a deform that gives a row's last column from a tuple with no
 * overflow chunk answers the row's count of values out of line, and one that
 * stops short of it answers none.
 */
void
test_access_heaptuple_tests_a_whole_deform_answers_the_rows_count_of_values_out_of_line(void)
{
	StdVec		external = access_heaptuple_tests_reference_tuple();
	AccessHeaptupleColumn column = access_heaptuple_tests_column(TEXTOID, -1, false);
	AccessHeaptupleRowLayout text_layout = access_heaptuple_tests_layout(&column, 1, NULL, 0);
	size_t		counts[2];
	StdOptionRef left[2];
	size_t		one = 1;
	StdOptionRef right[2];

	left[0] = access_heaptuple_tests_option(test_access_heaptuple_tests_a_whole_deform_answers_the_rows_count_of_values_out_of_line_closure_0(&text_layout, &external, 1, &counts[0]),
											&counts[0]);
	left[1] = access_heaptuple_tests_option(test_access_heaptuple_tests_a_whole_deform_answers_the_rows_count_of_values_out_of_line_closure_0(&text_layout, &external, 0, &counts[1]),
											&counts[1]);
	right[0] = access_heaptuple_tests_option(true, &one);
	right[1] = access_heaptuple_tests_option(false, NULL);
	STD_ASSERT_EQ(1294, 9, left[0].has && counts[0] == 1 && !left[1].has, left,
				  access_heaptuple_tests_options_Debug_fmt, right, access_heaptuple_tests_options_Debug_fmt);
	access_heaptuple_RowLayout_drop(&text_layout);
	std_vec_free_Checked(&external, ACCESS_HEAPTUPLE_BYTES);
}

/*
 * test_access_heaptuple_tests_a_value_moves_out_of_line_by_its_rows_heap_tuple_closure_0
 * is the test's moves: the field a row of an int4 and a text of a length moves
 * out of line first.
 */
static bool
test_access_heaptuple_tests_a_value_moves_out_of_line_by_its_rows_heap_tuple_closure_0(const AccessHeaptupleRowLayout *layout,
																					   size_t length, size_t *field)
{
	StdVec		chars;
	uint8		x = 'x';
	AccessHeaptupleSet values[2];
	AccessHeaptuplePlacing placing;
	PgPgError	error;
	bool		has;

	std_vec_new(&chars, ACCESS_HEAPTUPLE_BYTES);
	std_vec_resize(&chars, length, &x, ACCESS_HEAPTUPLE_BYTES);
	values[0] = access_heaptuple_tests_set_word(pg_word_from_int32(1));
	values[1] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, chars.ptr, chars.len);
	access_heaptuple_tests_ok(access_heaptuple_RowLayout_placing(layout, ACCESS_HEAPTUPLE_TESTS_REVERSED,
																 ACCESS_HEAPTUPLE_TESTS_NAMES, values, 2, NULL, 0, false,
																 ACCESS_HEAPTUPLE_TOAST_TUPLE_TARGET, &placing, &error),
							  &error);
	has = access_heaptuple_Placing_field(&placing, field);
	access_heaptuple_Placing_drop(&placing);
	std_vec_free_Checked(&chars, ACCESS_HEAPTUPLE_BYTES);
	return has;
}

/*
 * Bugs row 141: whether a value moves out of line is decided on the row's heap
 * tuple, so beside an int4 primary key a 2,016-byte EXTERNAL text moves and a
 * 2,000-byte one stays.
 */
void
test_access_heaptuple_tests_a_value_moves_out_of_line_by_its_rows_heap_tuple(void)
{
	AccessHeaptupleColumn columns[2] = {access_heaptuple_tests_column(INT4OID, 4, true),
	access_heaptuple_tests_column(TEXTOID, -1, false)};
	access_TupleUsizeOrderKeyType_t key = access_heaptuple_tests_keyed(0, ENCODING_KEY_ORDER_ASC, INT4OID);
	AccessHeaptupleRowLayout layout;
	size_t		fields[2];
	StdOptionRef left[2];
	size_t		zero = 0;
	StdOptionRef right[2];

	columns[1].storage = ACCESS_HEAPTUPLE_STORAGE_EXTERNAL;
	layout = access_heaptuple_tests_layout(columns, 2, &key, 1);
	left[0] = access_heaptuple_tests_option(test_access_heaptuple_tests_a_value_moves_out_of_line_by_its_rows_heap_tuple_closure_0(&layout, 2016, &fields[0]),
											&fields[0]);
	left[1] = access_heaptuple_tests_option(test_access_heaptuple_tests_a_value_moves_out_of_line_by_its_rows_heap_tuple_closure_0(&layout, 2000, &fields[1]),
											&fields[1]);
	right[0] = access_heaptuple_tests_option(true, &zero);
	right[1] = access_heaptuple_tests_option(false, NULL);
	STD_ASSERT_EQ(1310, 9, left[0].has && fields[0] == 0 && !left[1].has, left,
				  access_heaptuple_tests_options_Debug_fmt, right, access_heaptuple_tests_options_Debug_fmt);
	access_heaptuple_RowLayout_drop(&layout);
}

/*
 * test_access_heaptuple_tests_a_row_holding_a_value_out_of_line_toasts_to_its_target_closure_1
 * is the test's moves: the field a row moves out of line first at a target of
 * 128, beside some kept values and with whether the row it replaces held one.
 */
static bool
test_access_heaptuple_tests_a_row_holding_a_value_out_of_line_toasts_to_its_target_closure_1(const AccessHeaptupleRowLayout *layout,
																							 const AccessHeaptupleSet *row,
																							 const AccessHeaptupleKept *kept,
																							 size_t kept_len,
																							 bool external, size_t *field)
{
	AccessHeaptuplePlacing placing;
	PgPgError	error;
	bool		has;

	access_heaptuple_tests_ok(access_heaptuple_RowLayout_placing(layout, ACCESS_HEAPTUPLE_TESTS_REVERSED,
																 ACCESS_HEAPTUPLE_TESTS_NAMES, row, 3, kept, kept_len,
																 external, 128, &placing, &error), &error);
	has = access_heaptuple_Placing_field(&placing, field);
	access_heaptuple_Placing_drop(&placing);
	return has;
}

/*
 * Bugs row 141: a row toasts as heap's does when it or the row it replaces
 * holds a value out of line, so at a target of 128 a 300-byte EXTERNAL value
 * moves beside a kept value or in a row whose old version held one, and stays
 * in a row that holds none.
 */
void
test_access_heaptuple_tests_a_row_holding_a_value_out_of_line_toasts_to_its_target(void)
{
	AccessHeaptupleColumn columns[3] = {access_heaptuple_tests_column(INT4OID, 4, true),
	access_heaptuple_tests_column(TEXTOID, -1, false), access_heaptuple_tests_column(TEXTOID, -1, false)};
	access_TupleUsizeOrderKeyType_t key = access_heaptuple_tests_keyed(0, ENCODING_KEY_ORDER_ASC, INT4OID);
	AccessHeaptupleRowLayout layout;
	uint8		long_[3000];
	uint8		b[300];
	uint8		sevens[32];
	AccessHeaptupleSet beside[3];
	AccessHeaptupleSet alone[3];
	AccessHeaptupleKept kept[1] = {{1, 3000, sevens}};
	size_t		fields[3];
	StdOptionRef left[3];
	size_t		one = 1;
	StdOptionRef right[3];

	columns[1].storage = ACCESS_HEAPTUPLE_STORAGE_EXTERNAL;
	columns[2].storage = ACCESS_HEAPTUPLE_STORAGE_EXTERNAL;
	layout = access_heaptuple_tests_layout(columns, 3, &key, 1);
	memset(long_, 'a', sizeof(long_));
	memset(b, 'z', sizeof(b));
	memset(sevens, 7, sizeof(sevens));
	beside[0] = access_heaptuple_tests_set_word(pg_word_from_int32(1));
	beside[1] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, long_, sizeof(long_));
	beside[2] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, b, sizeof(b));
	alone[0] = access_heaptuple_tests_set_word(pg_word_from_int32(1));
	alone[1] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, "x", 1);
	alone[2] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, b, sizeof(b));
	left[0] = access_heaptuple_tests_option(test_access_heaptuple_tests_a_row_holding_a_value_out_of_line_toasts_to_its_target_closure_1(&layout, beside, kept, 1, false, &fields[0]),
											&fields[0]);
	left[1] = access_heaptuple_tests_option(test_access_heaptuple_tests_a_row_holding_a_value_out_of_line_toasts_to_its_target_closure_1(&layout, alone, NULL, 0, true, &fields[1]),
											&fields[1]);
	left[2] = access_heaptuple_tests_option(test_access_heaptuple_tests_a_row_holding_a_value_out_of_line_toasts_to_its_target_closure_1(&layout, alone, NULL, 0, false, &fields[2]),
											&fields[2]);
	right[0] = access_heaptuple_tests_option(true, &one);
	right[1] = access_heaptuple_tests_option(true, &one);
	right[2] = access_heaptuple_tests_option(false, NULL);
	STD_ASSERT_EQ(1328, 9,
				  left[0].has && fields[0] == 1 && left[1].has && fields[1] == 1 && !left[2].has, left,
				  access_heaptuple_tests_option_triple_Debug_fmt, right, access_heaptuple_tests_option_triple_Debug_fmt);
	access_heaptuple_RowLayout_drop(&layout);
}

/*
 * Bugs row 126: a row of two 4,096-byte MAIN values keeps both inline in the
 * tree and counts its heap bytes with the first out of line, as heap's rules
 * can store it.
 */
void
test_access_heaptuple_tests_a_row_of_main_values_counts_the_heap_tuple_heaps_rules_can_store(void)
{
	AccessHeaptupleColumn columns[2] = {access_heaptuple_tests_column(TEXTOID, -1, false),
	access_heaptuple_tests_column(TEXTOID, -1, false)};
	AccessHeaptupleRowLayout layout;
	uint8		one[4096];
	uint8		two[4096];
	AccessHeaptupleSet values[2];
	AccessHeaptuplePlacing placing;
	PgPgError	error;
	size_t		field;
	bool		has;
	StdOptionRef left;
	StdOptionRef right = access_heaptuple_tests_option(false, NULL);
	AccessHeaptupleFormed formed;

	columns[0].storage = ACCESS_HEAPTUPLE_STORAGE_MAIN;
	columns[1].storage = ACCESS_HEAPTUPLE_STORAGE_MAIN;
	layout = access_heaptuple_tests_layout(columns, 2, NULL, 0);
	for (int i = 0; i < 4096; i += 4)
	{
		memcpy(one + i, "1234", 4);
		memcpy(two + i, "5678", 4);
	}
	values[0] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, one, sizeof(one));
	values[1] = access_heaptuple_tests_set_bytes(ACCESS_HEAPTUPLE_SET_VARLENA, two, sizeof(two));
	access_heaptuple_tests_ok(access_heaptuple_RowLayout_placing(&layout, ACCESS_HEAPTUPLE_TESTS_REVERSED,
																 ACCESS_HEAPTUPLE_TESTS_NAMES, values, 2, NULL, 0, false,
																 ACCESS_HEAPTUPLE_TOAST_TUPLE_TARGET, &placing, &error),
							  &error);
	has = access_heaptuple_Placing_field(&placing, &field);
	left = access_heaptuple_tests_option(has, &field);
	STD_ASSERT_EQ(1349, 9, !has, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	access_heaptuple_tests_ok(access_heaptuple_Placing_formed(&placing, &formed, &error), &error);
	STD_ASSERT_EQ_U64(1350, 9, formed.heap_bytes, 4148);
	access_heaptuple_Formed_drop(&formed);
	access_heaptuple_RowLayout_drop(&layout);
}

/*
 * test_access_heaptuple_tests_a_row_stored_at_an_older_epoch_deforms_through_its_transform_closure_3
 * is the test's set: each deformed value pushed as a Got.
 */
static bool
test_access_heaptuple_tests_a_row_stored_at_an_older_epoch_deforms_through_its_transform_closure_3(void *captures,
																								   AccessHeaptupleDeformed deformed,
																								   PgPgError *error)
{
	access_heaptuple_tests_Got got = access_heaptuple_tests_got(deformed);

	std_vec_push_Checked(captures, &got, ACCESS_HEAPTUPLE_GOT);
	return true;
}

/*
 * The fork's own (STORAGE_DESIGN §II.10): a row stored before a column was
 * dropped and another added is read through the composed transform, each
 * column from the stored field its source names and the added one from its
 * missing value.
 */
void
test_access_heaptuple_tests_a_row_stored_at_an_older_epoch_deforms_through_its_transform(void)
{
	AccessHeaptupleColumn columns[4];
	AccessHeaptupleRowLayout layout;
	static const uint8 ints[3][4] = {{1}, {2}, {3}};
	static const uint8 nine[4] = {9};
	StdBytes	fields[3];
	StdVec		tuple;
	AccessTransformField sources[3] = {
		{.typlen = 4, .storage = 'p', .source = {.tag = ACCESS_TRANSFORM_SOURCE_OLD}},
		{.typlen = 4, .storage = 'p', .source = {.tag = ACCESS_TRANSFORM_SOURCE_OLD, .u.old = 2}},
		{.typlen = 4, .storage = 'p', .source = {.tag = ACCESS_TRANSFORM_SOURCE_MISSING, .u.missing = {.has = true}}},
	};
	StdVec		got;
	bool		has;
	size_t		count;
	PgPgError	error;
	access_heaptuple_tests_Got expected[4];

	for (int i = 0; i < 4; i++)
		columns[i] = access_heaptuple_tests_column(INT4OID, 4, true);
	columns[1].dropped = true;
	layout = access_heaptuple_tests_layout(columns, 4, NULL, 0);
	for (int i = 0; i < 3; i++)
		fields[i] = access_heaptuple_tests_slice(ints[i], 4);
	tuple = access_heaptuple_tests_write(fields, 3);
	std_slice_to_vec_Checked(&sources[2].source.u.missing.bytes, nine, 4, ACCESS_HEAPTUPLE_BYTES);
	std_vec_new(&got, ACCESS_HEAPTUPLE_GOT);
	access_heaptuple_tests_ok(access_heaptuple_RowLayout_deform(&layout, NULL, 0, tuple.ptr, tuple.len, true, sources, 3,
																0, 4,
																test_access_heaptuple_tests_a_row_stored_at_an_older_epoch_deforms_through_its_transform_closure_3,
																&got, &has, &count, &error), &error);
	expected[0] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, pg_word_from_int32(1));
	expected[1] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_NULL, 0);
	expected[2] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, pg_word_from_int32(3));
	expected[3] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, pg_word_from_int32(9));
	STD_ASSERT(1372, 9, access_heaptuple_tests_gots_eq(got.ptr, got.len, expected, 4),
			   "got == [word(1), Got::Null, word(3), word(9)]");
	expected[2] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, pg_word_from_int32(2));
	expected[3] = access_heaptuple_tests_got_of(ACCESS_HEAPTUPLE_TESTS_GOT_WORD, pg_word_from_int32(3));
	STD_ASSERT(1373, 9,
			   access_heaptuple_tests_deformed_eq(&layout, access_heaptuple_tests_slice(NULL, 0),
												  access_heaptuple_tests_bytes(&tuple), 0, 4, expected, 4),
			   "ok(deformed(&layout, &[], &tuple, 0, 4)) == [word(1), Got::Null, word(2), word(3)]");
	access_heaptuple_tests_gots_drop(&got);
	for (int i = 0; i < 3; i++)
		access_transform_Field_drop(&sources[i]);
	std_vec_free_Checked(&tuple, ACCESS_HEAPTUPLE_BYTES);
	access_heaptuple_RowLayout_drop(&layout);
}

#endif
#endif
