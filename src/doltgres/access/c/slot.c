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
 * The fork's tuple table slot (access' slot.rs, design item 5), what
 * TTSOpsBufferHeapTuple is for heap: it holds a row as its table's tree
 * stores it, a copy of its key and its tuple with the primary key's columns
 * beside them, and deforms it on demand into values in a buffer the slot
 * owns.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/access/slot.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#ifdef DOLTGRES_TEST_ACCESS
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "doltgres/encoding/tuple.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "access" STD_SEP "src" STD_SEP "slot.rs"

/* The element layouts of slot's vectors. */
#define ACCESS_SLOT_BYTES 1, 1
#define ACCESS_SLOT_COLUMN sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn)
#define ACCESS_SLOT_KEYED sizeof(access_TupleUsizeOrderKeyType_t), _Alignof(access_TupleUsizeOrderKeyType_t)

/* ACCESS_SLOT_PADDING is the most bytes that precede a deformed value in a slot's buffer to align it. */
#define ACCESS_SLOT_PADDING 7

/*
 * ACCESS_SLOT_VARLENA_ROOM is the most a deformed varlena takes in a slot's
 * buffer beyond its bytes: a four-byte header and its padding.
 */
#define ACCESS_SLOT_VARLENA_ROOM (4 + ACCESS_SLOT_PADDING)

/* ACCESS_SLOT_TUPLE_ID_MAX is the largest tuple id, 48 bits. */
#define ACCESS_SLOT_TUPLE_ID_MAX ((UINT64_C(1) << 48) - 1)

/* access_slot_internal returns an internal error with a message. */
static PgPgError
access_slot_internal(const char *message)
{
	StdString	str;

	std_string_from_Checked(&str, message, strlen(message));
	return pg_PgError_fork((const uint8 *) "XX000", &str);
}

/* access_slot_fail sets an error and answers false. */
static bool
access_slot_fail(PgPgError made, PgPgError *error)
{
	*error = made;
	return false;
}

/* access_slot_corrupt returns the fork's XX001 error of a message. */
static PgPgError
access_slot_corrupt(const char *message)
{
	StdString	str;

	std_string_from_Checked(&str, message, strlen(message));
	return pg_PgError_fork((const uint8 *) "XX001", &str);
}

/*
 * access_slot_keyed returns a primary key's columns as the bytes a slot
 * keeps beside a row, which live only in memory: each column's position as
 * two bytes, its order as one, and its key type as KeyType::write writes it.
 */
void
access_slot_keyed(const access_TupleUsizeOrderKeyType_t *key, size_t key_len, StdVec *result)
{
	std_vec_new(result, ACCESS_SLOT_BYTES);
	for (size_t i = 0; i < key_len; i++)
	{
		uint16		column = (uint16) key[i].f0;
		uint8		position[2] = {(uint8) column, (uint8) (column >> 8)};
		uint8		order = (uint8) key[i].f1.descending | (uint8) ((uint8) key[i].f1.nulls_first << 1);

		std_vec_extend_from_slice_Checked(result, position, 2, ACCESS_SLOT_BYTES);
		std_vec_push_Checked(result, &order, ACCESS_SLOT_BYTES);
		access_key_KeyType_write(&key[i].f2, result);
	}
}

/* access_slot_key_columns returns the primary key's columns that keyed wrote (a Vec<(usize, Order, KeyType)>). */
bool
access_slot_key_columns(const uint8 *keyed, size_t keyed_len, StdVec *result, PgPgError *error)
{
	StdBytes	rest = {keyed, keyed_len};

	std_vec_new(result, ACCESS_SLOT_KEYED);
	while (rest.len >= 3)
	{
		access_TupleUsizeOrderKeyType_t column;
		StdBytes	after;

		column.f1.descending = (rest.ptr[2] & 1) != 0;
		column.f1.nulls_first = (rest.ptr[2] & 2) != 0;
		if (!access_key_KeyType_read(rest.ptr + 3, rest.len - 3, &column.f2, &after))
		{
			std_vec_drop_Checked(result, ACCESS_SLOT_KEYED, access_TupleUsizeOrderKeyType_drop);
			return access_slot_fail(access_slot_internal("a slot's key columns are corrupt"), error);
		}
		column.f0 = (size_t) ((uint16) rest.ptr[0] | (uint16) rest.ptr[1] << 8);
		std_vec_push_Checked(result, &column, ACCESS_SLOT_KEYED);
		rest = after;
	}
	if (rest.len == 0)
		return true;
	std_vec_drop_Checked(result, ACCESS_SLOT_KEYED, access_TupleUsizeOrderKeyType_drop);
	return access_slot_fail(access_slot_internal("a slot's key columns are corrupt"), error);
}

/* access_slot_att returns an attribute of a slot's descriptor. */
static bool
access_slot_att(const PgSlot *slot, int32 i, PgAtt *result, PgPgError *error)
{
	PgAttOption att = pg_Att_of(pg_calls_slot_att(slot, i));

	if (!att.has)
		return access_slot_fail(access_slot_internal("a slot's descriptor lost an attribute"), error);
	*result = att.att;
	return true;
}

/* access_slot_room_frame_t is room's state across its pad: the transform it decodes and the key columns it reads. */
typedef struct access_slot_room_frame_t
{
	bool		has_transform;
	AccessTransformTransform transform;
	bool		columns_live;
	StdVec		columns;
	const PgSlot *slot;
	StdBytes	keyed;
	StdBytes	key;
	StdBytes	tuple;
	StdBytes	encoded;
	size_t	   *result;
	PgPgError  *error;
	bool		ok;
} access_slot_room_frame_t;

/* access_slot_room_pad frees room's transform and key columns. */
static void
access_slot_room_pad(void *frame_)
{
	access_slot_room_frame_t *frame = frame_;

	if (frame->has_transform)
		access_transform_Transform_drop(&frame->transform);
	if (frame->columns_live)
		std_vec_drop_Checked(&frame->columns, ACCESS_SLOT_KEYED, access_TupleUsizeOrderKeyType_drop);
	frame->has_transform = frame->columns_live = false;
}

/* access_slot_room_body adds up the bound of a row's deformed values. */
static void
access_slot_room_body(void *frame_)
{
	access_slot_room_frame_t *frame = frame_;
	size_t		room = frame->tuple.len;
	AccessKeyKeyTypes key_types;
	size_t		bound;
	StdBytes	rest = frame->key;

	if (frame->encoded.len != 0)
	{
		if (!access_transform_Transform_decode(frame->encoded.ptr, frame->encoded.len, &frame->transform,
											   frame->error))
			return;
		frame->has_transform = true;
		for (size_t i = 0; i < frame->transform.fields.len; i++)
		{
			const AccessTransformField *field = &STD_VEC_AT(&frame->transform.fields, AccessTransformField, i);

			if (field->source.tag == ACCESS_TRANSFORM_SOURCE_MISSING && field->source.u.missing.has)
				room = STD_ADD(usize, room, field->source.u.missing.bytes.len, 82, 17);
		}
		access_transform_Transform_drop(&frame->transform);
		frame->has_transform = false;
	}
	for (int32 i = 0; i < pg_calls_slot_natts(frame->slot); i++)
	{
		PgAtt		att;
		int16		len;

		if (!access_slot_att(frame->slot, i, &att, frame->error))
			return;
		len = pg_Att_length(att);
		if (pg_Att_by_value(att) || pg_Att_dropped(att))
			continue;
		room = STD_ADD(usize, room, len == -1 ? ACCESS_SLOT_VARLENA_ROOM :
					   STD_ADD(usize, ACCESS_SLOT_PADDING, (size_t) (uint16) (len < 0 ? -(int32) len : len),
							   91, 29),
					   88, 9);
	}
	if (!access_slot_key_columns(frame->keyed.ptr, frame->keyed.len, &frame->columns, frame->error))
		return;
	frame->columns_live = true;
	key_types = (AccessKeyKeyTypes)
	{
		frame->columns.ptr, frame->columns.len, sizeof(access_TupleUsizeOrderKeyType_t),
			offsetof(access_TupleUsizeOrderKeyType_t, f2)
	};
	bound = access_key_table_bound(key_types);
	for (size_t i = 0; i < frame->columns.len; i++)
	{
		const access_TupleUsizeOrderKeyType_t *column = &STD_VEC_AT(&frame->columns, access_TupleUsizeOrderKeyType_t,
																	i);
		StdBytes	after;
		bool		cut;
		size_t		most;

		if (!access_key_skip_within(rest.ptr, rest.len, &column->f2, column->f1, bound, &after, &cut, frame->error))
			return;
		if (!cut)
		{
			if (!access_key_decoded_max(&column->f2, column->f1, rest.ptr, rest.len, &most, frame->error))
				return;
			room = STD_ADD(usize, room, most, 100, 13);
		}
		rest = after;
	}
	*frame->result = room;
	frame->ok = true;
}

/*
 * access_slot_room returns a bound of the bytes the deformed values of a row
 * take in a slot's buffer, whatever the row holds: for each attribute passed
 * by reference its padding and its length, or a varlena's header; the
 * tuple's bytes, and the bytes of the values the row's transform sets; and
 * for each key column the key holds whole what a value read back from the
 * key can take.
 */
static bool
access_slot_room(const PgSlot *slot, StdBytes keyed, StdBytes key, StdBytes tuple, StdBytes transform, size_t *result,
				 PgPgError *error)
{
	access_slot_room_frame_t frame = {.slot = slot, .keyed = keyed, .key = key, .tuple = tuple,
	.encoded = transform, .result = result, .error = error};

	std_landing(&frame, access_slot_room_body, access_slot_room_pad);
	access_slot_room_pad(&frame);
	return frame.ok;
}

/*
 * access_slot_columns returns how a slot's descriptor stores each of its
 * attributes (a Vec<Column>), before a layout numbers their fields.
 */
bool
access_slot_columns(const PgSlot *slot, StdVec *result, PgPgError *error)
{
	std_vec_new(result, ACCESS_SLOT_COLUMN);
	for (int32 i = 0; i < pg_calls_slot_natts(slot); i++)
	{
		PgAtt		att;
		AccessHeaptupleColumn column = {0};

		if (!access_slot_att(slot, i, &att, error) ||
			!access_heaptuple_Storage_of(pg_calls_slot_att_storage(slot, i), &column.storage, error))
		{
			std_vec_free_Checked(result, ACCESS_SLOT_COLUMN);
			return false;
		}
		column.typid = pg_Att_type_id(att);
		column.len = pg_Att_length(att);
		column.by_value = pg_Att_by_value(att);
		column.align = pg_Att_align(att);
		column.dropped = pg_Att_dropped(att);
		std_vec_push_Checked(result, &column, ACCESS_SLOT_COLUMN);
	}
	return true;
}

/*
 * access_slot_layout returns the layout of the row a slot holds, from its
 * descriptor's attributes and the key columns kept beside the row.
 */
static bool
access_slot_layout(const PgSlot *slot, AccessHeaptupleRowLayout *result, PgPgError *error)
{
	StdVec		columns;
	StdVec		key;
	size_t		keyed_len;
	const uint8 *keyed;

	if (!access_slot_columns(slot, &columns, error))
		return false;
	keyed = pg_calls_slot_keyed(slot, &keyed_len);
	if (!access_slot_key_columns(keyed, keyed_len, &key, error))
	{
		std_vec_free_Checked(&columns, ACCESS_SLOT_COLUMN);
		return false;
	}
	access_heaptuple_RowLayout_new(&columns, &key, result);
	return true;
}

/* access_slot_identity answers the table's object id and the ctid of the row a slot holds, its block and its offset. */
void
access_slot_identity(const PgSlot *slot, uint32 *result_0, access_TupleU32U16_t *result_1)
{
	uint64		tid = pg_calls_slot_tid(slot);

	*result_0 = pg_calls_slot_table(slot);
	result_1->f0 = (uint32) (tid >> 16);
	result_1->f1 = (uint16) (tid & 0xffff);
}

/*
 * access_slot_ctid returns a tuple id's ctid, its block and its offset: a
 * tuple id is block << 16 | offset, its offset from 1 to a page's most
 * tuples, and any other id is data corruption.
 */
bool
access_slot_ctid(uint64 tuple_id, uint16 most_offset, access_TupleU32U16_t *result, PgPgError *error)
{
	uint16		offset = (uint16) (tuple_id & 0xffff);
	StdFmtPiece pieces[] = {STD_FMT_LIT("a stored row's tuple id "), STD_FMT_ARG(0), STD_FMT_LIT(" is out of range")};
	StdFmtArg	args[] = {{&tuple_id, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, 3, args};
	StdString	message;

	if (tuple_id <= ACCESS_SLOT_TUPLE_ID_MAX && offset >= 1 && offset <= most_offset)
	{
		result->f0 = (uint32) (tuple_id >> 16);
		result->f1 = offset;
		return true;
	}
	std_fmt_format_Checked(&message, &arguments);
	*error = pg_PgError_fork((const uint8 *) "XX001", &message);
	return false;
}

/*
 * access_slot_store puts a stored row in a slot, Postgres'
 * ExecStoreBufferHeapTuple: a copy of its primary key's columns, of its key,
 * of its tuple, of its values' ids out of line, and of the encoded transform
 * its read takes it through, with room for the values deforming it will
 * make, and its values' TOAST relation, version, table, and ctid.
 */
bool
access_slot_store(const PgMayRaise *raise, PgSlot *slot, const uint8 *keyed, size_t keyed_len, const uint8 *key,
				  size_t key_len, const uint8 *tuple, size_t tuple_len, AccessSlotOutside outside,
				  const uint8 *transform, size_t transform_len, const AccessSlotVersion *version, PgPgError *error)
{
	StdBytes	keyed_bytes = {keyed, keyed_len};
	StdBytes	key_bytes = {key, key_len};
	StdBytes	tuple_bytes = {tuple, tuple_len};
	StdBytes	transform_bytes = {transform, transform_len};
	DgStored	stored = {.writer = version->writer, .deleter = version->deleter, .command = version->command,
		.deleter_command = version->deleter_command, .table = version->table, .toast = outside.toast,
	.block = version->ctid.f0, .offset = version->ctid.f1};

	if (!access_slot_room(slot, keyed_bytes, key_bytes, tuple_bytes, transform_bytes, &stored.room, error))
		return false;
	return pg_calls_slot_store(raise, slot, keyed, keyed_len, key, key_len, tuple, tuple_len, outside.ids.ptr,
							   outside.ids.len, transform, transform_len, stored, error);
}

/* access_slot_set_t is set_next's captures: the slot and the chunk a deform stopped at, if any. */
typedef struct access_slot_set_t
{
	const PgSlot *slot;
	bool		has_stop;
	AccessSlotChunk stop;
} access_slot_set_t;

/*
 * access_slot_external sets a slot's next attribute from a value its stored
 * row keeps out of line: Postgres' on-disk TOAST pointer of an uncompressed
 * value, formed from its length and the value id its place among the row's
 * values out of line names, and one with no value id is data corruption.
 */
static bool
access_slot_external(const PgSlot *slot, uint64 length, size_t value, PgPgError *error)
{
	size_t		values_len;
	const uint8 *values = pg_calls_slot_values(slot, &values_len);
	uint32		id;
	uint32		raw;

	if (value >= values_len / 4)
		return access_slot_fail(access_slot_corrupt("a stored row's value out of line has no value id"), error);
	id = (uint32) values[4 * value] | (uint32) values[4 * value + 1] << 8 | (uint32) values[4 * value + 2] << 16 |
		(uint32) values[4 * value + 3] << 24;
	if (length > PG_UINT32_MAX || !std_checked_add_u32((uint32) length, 4, &raw))
		return access_slot_fail(access_slot_corrupt("a stored row's value out of line is too long"), error);
	if (!pg_calls_slot_set_external(slot, raw, (uint32) length, id, pg_calls_slot_toast(slot)))
		return access_slot_fail(access_slot_internal("a slot refused a value of the row it holds"), error);
	return true;
}

/*
 * access_slot_set_next sets a slot's next attribute from a deformed value,
 * a value out of line as external sets it, and at a value held in an
 * overflow chunk keeps the chunk in stop and sets no attribute after it.
 */
static bool
access_slot_set_next(access_slot_set_t *set, AccessHeaptupleDeformed deformed, PgPgError *error)
{
	bool		taken = false;

	if (set->has_stop)
		return true;
	switch (deformed.tag)
	{
		case ACCESS_HEAPTUPLE_DEFORMED_VALUE:
			switch (deformed.u.value.tag)
			{
				case ACCESS_HEAPTUPLE_SET_NULL:
					taken = pg_calls_slot_set_null(set->slot);
					break;
				case ACCESS_HEAPTUPLE_SET_WORD:
					taken = pg_calls_slot_set_word(set->slot, deformed.u.value.u.word);
					break;
				case ACCESS_HEAPTUPLE_SET_FIXED:
					taken = pg_calls_slot_set_fixed(set->slot, deformed.u.value.u.fixed.ptr,
													deformed.u.value.u.fixed.len);
					break;
				case ACCESS_HEAPTUPLE_SET_VARLENA:
					taken = pg_calls_slot_set_varlena(set->slot, deformed.u.value.u.varlena.ptr,
													  deformed.u.value.u.varlena.len);
					break;
			}
			break;
		case ACCESS_HEAPTUPLE_DEFORMED_MISSING:
			taken = pg_calls_slot_set_missing(set->slot);
			break;
		case ACCESS_HEAPTUPLE_DEFORMED_OUT_OF_LINE:
			return access_slot_external(set->slot, deformed.u.out_of_line.length, deformed.u.out_of_line.value, error);
		case ACCESS_HEAPTUPLE_DEFORMED_OVERFLOWED:
			if (deformed.u.overflowed.address.len != 32)
				return access_slot_fail(access_slot_internal("an overflow chunk's address isn't 32 bytes"), error);
			memcpy(set->stop.address, deformed.u.overflowed.address.ptr, 32);
			set->stop.field = deformed.u.overflowed.field;
			set->stop.before = deformed.u.overflowed.before;
			set->has_stop = true;
			return true;
	}
	if (!taken)
		return access_slot_fail(access_slot_internal("a slot refused a value of the row it holds"), error);
	return true;
}

/* access_slot_getsomeattrs_closure_1 is getsomeattrs' set: set_next's of each deformed value. */
static bool
access_slot_getsomeattrs_closure_1(void *captures, AccessHeaptupleDeformed deformed, PgPgError *error)
{
	return access_slot_set_next(captures, deformed, error);
}

/* access_slot_overflowed_closure_0 is overflowed's set: set_next's of each deformed value. */
static bool
access_slot_overflowed_closure_0(void *captures, AccessHeaptupleDeformed deformed, PgPgError *error)
{
	return access_slot_set_next(captures, deformed, error);
}

/*
 * access_slot_counted refuses a stored row whose value ids aren't its values
 * out of line, by a count a deform answered once it read the part of the
 * row that holds the last of them, as external refuses a missing id.
 */
static bool
access_slot_counted(const PgSlot *slot, bool has_count, size_t count, PgPgError *error)
{
	size_t		values_len;

	(void) pg_calls_slot_values(slot, &values_len);
	if (has_count && values_len != STD_MUL(usize, 4, count, 244, 55))
		return access_slot_fail(access_slot_corrupt("a stored row's value ids aren't its values out of line"), error);
	return true;
}

/*
 * access_slot_deform_frame_t is getsomeattrs' and overflowed's state across
 * their pads: the transform and the layout they deform through.
 */
typedef struct access_slot_deform_frame_t
{
	bool		has_transform;
	AccessTransformTransform transform;
	bool		layout_live;
	AccessHeaptupleRowLayout layout;
	access_slot_set_t set;
	size_t		from;
	size_t		upto;
	const AccessSlotChunk *chunk;
	StdBytes	bytes;
	bool		has_count;
	size_t		count;
	PgPgError  *error;
	bool		ok;
} access_slot_deform_frame_t;

/* access_slot_deform_pad frees a deform's transform and layout. */
static void
access_slot_deform_pad(void *frame_)
{
	access_slot_deform_frame_t *frame = frame_;

	if (frame->has_transform)
		access_transform_Transform_drop(&frame->transform);
	if (frame->layout_live)
		access_heaptuple_RowLayout_drop(&frame->layout);
	frame->has_transform = frame->layout_live = false;
}

/* access_slot_getsomeattrs_body deforms the row through its transform, if any. */
static void
access_slot_getsomeattrs_body(void *frame_)
{
	access_slot_deform_frame_t *frame = frame_;
	const PgSlot *slot = frame->set.slot;
	size_t		transform_len;
	const uint8 *transform = pg_calls_slot_transform(slot, &transform_len);
	size_t		key_len;
	const uint8 *key;
	size_t		tuple_len;
	const uint8 *tuple = pg_calls_slot_tuple(slot, &tuple_len);

	if (transform_len != 0)
	{
		if (!access_transform_Transform_decode(transform, transform_len, &frame->transform, frame->error))
			return;
		frame->has_transform = true;
	}
	if (!access_slot_layout(slot, &frame->layout, frame->error))
		return;
	frame->layout_live = true;
	key = pg_calls_slot_key(slot, &key_len);
	frame->ok = access_heaptuple_RowLayout_deform(&frame->layout, key, key_len, tuple, tuple_len, frame->has_transform,
												  frame->has_transform ? frame->transform.fields.ptr : NULL,
												  frame->has_transform ? frame->transform.fields.len : 0, frame->from,
												  frame->upto, access_slot_getsomeattrs_closure_1, &frame->set,
												  &frame->has_count, &frame->count, frame->error);
}

/*
 * access_slot_getsomeattrs deforms the stored row a slot holds as far as a
 * number of attributes, the slot's getsomeattrs, through the transform the
 * slot keeps for a row at an older epoch than its reader's, and stops at a
 * value held in an overflow chunk, which it answers for its caller to read
 * and resume with overflowed.
 */
bool
access_slot_getsomeattrs(const PgSlot *slot, int32 natts, bool *has_result, AccessSlotChunk *result,
						 PgPgError *error)
{
	access_slot_deform_frame_t frame = {.set = {slot}, .error = error};
	size_t		tuple_len;

	(void) pg_calls_slot_tuple(slot, &tuple_len);
	if (tuple_len == 0)
		return access_slot_fail(access_slot_internal("a slot that holds no stored row was asked to deform one"), error);
	frame.from = (size_t) std_i32_unsigned_abs(pg_calls_slot_filled(slot));
	frame.upto = (size_t) std_i32_unsigned_abs(natts);
	*has_result = false;
	if (frame.from >= frame.upto)
		return true;
	std_landing(&frame, access_slot_getsomeattrs_body, access_slot_deform_pad);
	access_slot_deform_pad(&frame);
	if (!frame.ok || !access_slot_counted(slot, frame.has_count, frame.count, error))
		return false;
	*has_result = frame.set.has_stop;
	if (frame.set.has_stop)
		*result = frame.set.stop;
	return true;
}

/* access_slot_overflowed_body deforms on from the overflow chunk's bytes. */
static void
access_slot_overflowed_body(void *frame_)
{
	access_slot_deform_frame_t *frame = frame_;
	const PgSlot *slot = frame->set.slot;
	size_t		key_len;
	const uint8 *key;

	if (!access_slot_layout(slot, &frame->layout, frame->error))
		return;
	frame->layout_live = true;
	key = pg_calls_slot_key(slot, &key_len);
	frame->ok = access_heaptuple_RowLayout_overflowed(&frame->layout, key, key_len, frame->from, frame->bytes.ptr,
													  frame->bytes.len, frame->chunk->field, frame->chunk->before,
													  access_slot_overflowed_closure_0, &frame->set, &frame->has_count,
													  &frame->count, frame->error);
}

/*
 * access_slot_overflowed sets a slot's next attribute from the bytes of the
 * overflow chunk a deform stopped at, as getsomeattrs sets a value, and
 * deforms on as far as a number of attributes, stopping at the next chunk
 * as getsomeattrs does.
 */
bool
access_slot_overflowed(const PgSlot *slot, int32 natts, const AccessSlotChunk *chunk, const uint8 *bytes,
					   size_t bytes_len, bool *has_result, AccessSlotChunk *result, PgPgError *error)
{
	access_slot_deform_frame_t frame = {.set = {slot}, .chunk = chunk, .bytes = {bytes, bytes_len}, .error = error};

	frame.from = (size_t) std_i32_unsigned_abs(pg_calls_slot_filled(slot));
	std_landing(&frame, access_slot_overflowed_body, access_slot_deform_pad);
	access_slot_deform_pad(&frame);
	if (!frame.ok || !access_slot_counted(slot, frame.has_count, frame.count, error))
		return false;
	if (frame.set.has_stop)
	{
		*has_result = true;
		*result = frame.set.stop;
		return true;
	}
	return access_slot_getsomeattrs(slot, natts, has_result, result, error);
}

/*
 * access_slot_getsysattr returns a system attribute of the row a slot holds,
 * the slot's getsysattr, from its version or a virtual slot's answer.
 */
bool
access_slot_getsysattr(const PgMayRaise *raise, PgSlot *slot, int32 attnum, PgIsNull *isnull, PgDatum *result,
					   PgPgError *error)
{
	return pg_calls_slot_system(raise, slot, attnum, isnull, result, error);
}

/*
 * access_slot_copy makes a slot hold the row another slot holds, the slot's
 * copyslot: a stored row's bytes and version, or else the source's values.
 */
bool
access_slot_copy(const PgMayRaise *raise, PgSlot *slot, PgSlot *source, PgPgError *error)
{
	size_t		tuple_len;
	const uint8 *tuple = pg_calls_slot_tuple(source, &tuple_len);
	AccessSlotVersion version;
	AccessSlotOutside outside;
	size_t		keyed_len;
	const uint8 *keyed;
	size_t		key_len;
	const uint8 *key;
	size_t		transform_len;
	const uint8 *transform;

	if (tuple_len == 0)
		return pg_calls_slot_copy_values(raise, slot, source, error);
	access_slot_identity(source, &version.table, &version.ctid);
	version.writer = pg_calls_slot_writer(source);
	version.command = pg_calls_slot_command(source);
	version.deleter = pg_calls_slot_deleter(source);
	version.deleter_command = pg_calls_slot_deleter_command(source);
	outside.ids.ptr = pg_calls_slot_values(source, &outside.ids.len);
	outside.toast = pg_calls_slot_toast(source);
	transform = pg_calls_slot_transform(source, &transform_len);
	keyed = pg_calls_slot_keyed(source, &keyed_len);
	key = pg_calls_slot_key(source, &key_len);
	return access_slot_store(raise, slot, keyed, keyed_len, key, key_len, tuple, tuple_len, outside, transform,
							 transform_len, &version, error);
}

#ifdef DOLTGRES_TEST_ACCESS

/* access_slot_tests_assert_bytes is assert_eq! of two byte runs at a place. */
static void
access_slot_tests_assert_bytes(uint32 line, uint32 col, StdBytes left, StdBytes right)
{
	if (left.len != right.len || memcmp(left.ptr, right.ptr, left.len) != 0)
		std_assert_failed(RUST_FILE, line, col, "==", &left, std_bytes_Debug_fmt, &right, std_bytes_Debug_fmt, NULL);
}

/* access_slot_tests_bytes returns a borrowed run of a vector's bytes. */
static StdBytes
access_slot_tests_bytes(const StdVec *vec, size_t len)
{
	StdBytes	bytes = {vec->ptr, len};

	return bytes;
}

/*
 * The fork's own: a key column's bytes hold its position, both parts of its
 * order, and its whole key type, its collation and the key types it holds
 * included, which read back as they were written.
 */
void
test_access_slot_tests_a_key_columns_bytes_hold_its_position_order_and_type(void)
{
	AccessKeyKeyType *element = std_alloc(sizeof(AccessKeyKeyType), _Alignof(AccessKeyKeyType));
	AccessKeyCollation sorted = {ACCESS_KEY_COLLATION_SORTED, 12, false};
	access_TupleUsizeOrderKeyType_t key[2];
	access_TupleUsizeOrderKeyType_t both_key[1];
	static const uint8 expected[18] = {2, 1, 1, 23, 0, 0, 0, 0, 0, 0, 0, 2, 25, 0, 0, 0, 0, 0};
	StdVec		written;
	StdVec		both;
	StdVec		read;
	StdVec		again;
	PgPgError	error;
	const access_TupleUsizeOrderKeyType_t *column;
	size_t		short_len;
	StdBytes	right;

	*element = access_key_KeyType_of(TEXTOID);
	element->collation = sorted;
	both_key[0].f0 = 1;
	both_key[0].f1 = ENCODING_KEY_ORDER_ASC;
	both_key[0].f2 = access_key_KeyType_of(1009);
	access_key_Inner_drop(&both_key[0].f2.inner);
	both_key[0].f2.inner.tag = ACCESS_KEY_INNER_ARRAY;
	both_key[0].f2.inner.u.array = (AccessKeyStored)
	{
		element, -1, false, 'i'
	};
	key[0] = (access_TupleUsizeOrderKeyType_t)
	{
		258,
		{
			true, false
		}, access_key_KeyType_of(INT4OID)
	};
	key[1] = (access_TupleUsizeOrderKeyType_t)
	{
		0,
		{
			false, true
		}, access_key_KeyType_of(TEXTOID)
	};
	access_slot_keyed(key, 2, &written);
	right.ptr = expected;
	right.len = sizeof(expected);
	access_slot_tests_assert_bytes(340, 9, access_slot_tests_bytes(&written, written.len), right);
	std_vec_free_Checked(&written, ACCESS_SLOT_BYTES);
	access_slot_keyed(both_key, 1, &both);
	access_key_KeyType_drop(&both_key[0].f2);
	if (!access_slot_key_columns(both.ptr, both.len, &read, &error))
		std_panic(RUST_FILE, 342, 50, "the columns were refused", sizeof("the columns were refused") - 1);
	access_slot_keyed(read.ptr, read.len, &again);
	access_slot_tests_assert_bytes(343, 9, access_slot_tests_bytes(&again, again.len),
								   access_slot_tests_bytes(&both, both.len));
	std_vec_free_Checked(&again, ACCESS_SLOT_BYTES);
	column = read.len == 1 ? &STD_VEC_AT(&read, access_TupleUsizeOrderKeyType_t, 0) : NULL;
	if (column == NULL || column->f0 != 1 || column->f2.inner.tag != ACCESS_KEY_INNER_ARRAY)
	{
		StdBytes	both_bytes = access_slot_tests_bytes(&both, both.len);
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&both_bytes, std_bytes_Debug_fmt}};
		StdFmtArguments arguments = {pieces, 1, args};

		std_panic_fmt(RUST_FILE, 344, 100, &arguments);
	}
	STD_ASSERT(345, 9,
			   access_key_Collation_PartialEq_eq(&column->f2.inner.u.array.key_type->collation, &sorted),
			   "element.collation == Collation::Sorted { id: 12, deterministic: false }");
	short_len = STD_SUB(usize, both.len, 1, 346, 37);
	std_range_to_check(short_len, both.len, RUST_FILE, 346, 30);
	if (access_slot_key_columns(both.ptr, short_len, &again, &error))
	{
		std_vec_drop_Checked(&again, ACCESS_SLOT_KEYED, access_TupleUsizeOrderKeyType_drop);
		STD_ASSERT(346, 9, false, "key_columns(&both[..both.len() - 1]).is_err()");
	}
	pg_PgError_drop(&error);
	std_vec_drop_Checked(&read, ACCESS_SLOT_KEYED, access_TupleUsizeOrderKeyType_drop);
	std_vec_free_Checked(&both, ACCESS_SLOT_BYTES);
	access_key_KeyType_drop(&key[1].f2);
	access_key_KeyType_drop(&key[0].f2);
}

/* access_slot_tests_ctid_is reports whether ctid splits a tuple id into a block and an offset. */
static bool
access_slot_tests_ctid_is(uint64 tuple_id, uint16 most, uint32 block, uint16 offset)
{
	access_TupleU32U16_t ctid;
	PgPgError	error;

	if (!access_slot_ctid(tuple_id, most, &ctid, &error))
	{
		pg_PgError_drop(&error);
		return false;
	}
	return ctid.f0 == block && ctid.f1 == offset;
}

/*
 * PLAN's format section, a tuple id: a tuple id splits into its block and its
 * offset, from 1 to a page's most tuples, and an offset of 0 or past the most
 * is data corruption, as is an id past 48 bits.
 */
void
test_access_slot_tests_a_tuple_id_splits_into_its_block_and_offset(void)
{
	StdString	empty;
	PgPgError	made;
	int32		corrupt;
	static const uint16 mosts[2] = {291, 584};

	std_string_from_Checked(&empty, "", 0);
	made = pg_PgError_fork((const uint8 *) "XX001", &empty);
	corrupt = pg_PgError_sqlerrcode(&made);
	pg_PgError_drop(&made);
	for (int m = 0; m < 2; m++)
	{
		uint16		most = mosts[m];
		uint64		last = (uint64) PG_UINT32_MAX << 16 | most;
		uint64		refused[5];

		STD_ASSERT(357, 13, access_slot_tests_ctid_is(last, most, PG_UINT32_MAX, most),
				   "matches!(ctid(last, most), Ok((u32::MAX, offset)) if offset == most)");
		STD_ASSERT(358, 13, access_slot_tests_ctid_is(UINT64_C(1) << 16 | 1, most, 1, 1),
				   "matches!(ctid(1 << 16 | 1, most), Ok((1, 1)))");
		refused[0] = 0;
		refused[1] = UINT64_C(1) << 16;
		refused[2] = STD_ADD(u64, last, 1, 359, 41);
		refused[3] = UINT64_C(5) << 16 | STD_ADD(u16, most, 1, 359, 71);
		refused[4] = UINT64_C(1) << 48 | 1;
		for (int r = 0; r < 5; r++)
		{
			access_TupleU32U16_t ctid;
			PgPgError	error;
			bool		holds = false;

			if (!access_slot_ctid(refused[r], most, &ctid, &error))
			{
				holds = pg_PgError_sqlerrcode(&error) == corrupt;
				pg_PgError_drop(&error);
			}
			if (!holds)
			{
				StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
				StdFmtArg	args[] = {{&refused[r], std_u64_Display_fmt}};
				StdFmtArguments arguments = {pieces, 1, args};

				std_panic_fmt(RUST_FILE, 360, 17, &arguments);
			}
		}
	}
}

/*
 * test_access_slot_tests_a_deform_that_meets_no_overflow_chunk_leaves_its_chunk_as_it_was:
 * a deform of a stored row whose values are all in line answers no chunk to
 * read and leaves its caller's chunk as the caller had it (bugs row 288).
 */
void
test_access_slot_tests_a_deform_that_meets_no_overflow_chunk_leaves_its_chunk_as_it_was(void)
{
	PgMayRaise	raise = {0};
	PgCatalog	catalog = {0};
	StdBytes	field = {NULL, 0};
	encoding_tuple_write_overflow_t none = {0};
	AccessSlotOutside outside = {0};
	AccessSlotVersion version = {0};
	AccessSlotChunk chunk = {.field = 0x5EED, .before = 0x5EED};
	ChunksAddressAddress address;
	PgRefRel	rel;
	PgOwnedSlot slot;
	PgPgError	error;
	StdVec		tuple;
	StdVec		keyed;
	bool		opened;
	bool		has = true;

	STD_ASSERT(__LINE__, 5, pg_RefRel_opened(&raise, &catalog, RelationRelationId, &opened, &rel, &error), "pg_class's relcache entry is read");
	STD_ASSERT(__LINE__, 5, opened, "pg_class opens");
	STD_ASSERT(__LINE__, 5, pg_calls_make_slot(&raise, pg_RefRel_rel(&rel), &slot, &error), "a slot of pg_class's columns is made");
	encoding_tuple_write(&field, 1, false, none, &tuple);
	address = chunks_address_Address_of(tuple.ptr, tuple.len);
	access_slot_keyed(NULL, 0, &keyed);
	STD_ASSERT(__LINE__, 5, access_slot_store(&raise, pg_OwnedSlot_slot(&slot), keyed.ptr, keyed.len, address.f0,
											  ACCESS_HEAPTUPLE_KEYLESS_KEY_LEN, tuple.ptr, tuple.len, outside, NULL, 0,
											  &version, &error), "the row is stored in the slot");
	STD_ASSERT(__LINE__, 5, access_slot_getsomeattrs(pg_OwnedSlot_slot(&slot), 1, &has, &chunk, &error), "the row deforms");
	STD_ASSERT(__LINE__, 5, !has && chunk.field == 0x5EED && chunk.before == 0x5EED,
			   "a deform that meets no overflow chunk answers none and leaves its chunk as it was");
	std_vec_free_Checked(&keyed, ACCESS_SLOT_KEYED);
	std_vec_free_Checked(&tuple, 1, 1);
	pg_OwnedSlot_drop(&slot);
	pg_RefRel_drop(&rel);
}

#endif
#endif
