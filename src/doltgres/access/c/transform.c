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
 * A table's pending transforms (access' transform.rs): how a row stored
 * under one layout of a table's fields becomes a row under the next, named
 * by positions and stored bytes, their encoding, and their composition.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/access/transform.h"
#include "doltgres/std/unwind.h"
#ifdef DOLTGRES_TEST_ACCESS
#include "doltgres/std/slice.h"
#include "doltgres/std/num.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "access" STD_SEP "src" STD_SEP "transform.rs"

/* The element layouts of transform's vectors. */
#define ACCESS_TRANSFORM_BYTES 1, 1
#define ACCESS_TRANSFORM_FIELD sizeof(AccessTransformField), _Alignof(AccessTransformField)

/* SOURCE_OLD, SOURCE_NULL, and SOURCE_BYTES tag a field's source in a transform's encoding. */
#define ACCESS_TRANSFORM_SOURCE_OLD_TAG 0
#define ACCESS_TRANSFORM_SOURCE_NULL_TAG 1
#define ACCESS_TRANSFORM_SOURCE_BYTES_TAG 2

/* access_transform_corrupt returns the error of a transform's bytes that don't decode, data corruption. */
static PgPgError
access_transform_corrupt(void)
{
	static const char message[] = "a table's pending transform is corrupt";
	StdString text;

	std_string_from_Checked(&text, message, sizeof(message) - 1);
	return pg_PgError_fork((const uint8 *) "XX001", &text);
}

/* access_transform_Source_Clone_clone copies a source, a missing value's bytes with it. */
void
access_transform_Source_Clone_clone(const AccessTransformSource *self, AccessTransformSource *result)
{
	*result = *self;
	if (self->tag == ACCESS_TRANSFORM_SOURCE_MISSING && self->u.missing.has)
		std_slice_to_vec_Checked(&result->u.missing.bytes, self->u.missing.bytes.ptr, self->u.missing.bytes.len,
								 ACCESS_TRANSFORM_BYTES);
}

/* access_transform_Source_PartialEq_eq reports whether two sources are the same variant with equal contents. */
bool
access_transform_Source_PartialEq_eq(const AccessTransformSource *self, const AccessTransformSource *other)
{
	if (self->tag != other->tag)
		return false;
	if (self->tag == ACCESS_TRANSFORM_SOURCE_OLD)
		return self->u.old == other->u.old;
	if (self->u.missing.has != other->u.missing.has)
		return false;
	return !self->u.missing.has ||
		(self->u.missing.bytes.len == other->u.missing.bytes.len &&
		 memcmp(self->u.missing.bytes.ptr, other->u.missing.bytes.ptr, self->u.missing.bytes.len) == 0);
}

/* access_transform_vec_u8_Debug_fmt is a Vec<u8>'s Debug, its bytes as a list. */
static bool
access_transform_vec_u8_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *bytes = value;
	StdBytes	slice = {bytes->ptr, bytes->len};

	return std_bytes_Debug_fmt(&slice, f);
}

/* access_transform_Source_Debug_fmt is Source's derived Debug. */
bool
access_transform_Source_Debug_fmt(const void *value, StdFormatter *f)
{
	const AccessTransformSource *self = value;
	StdOptionRef missing;
	StdDebugTuple t;

	switch (self->tag)
	{
		case ACCESS_TRANSFORM_SOURCE_OLD:
			t = std_fmt_debug_tuple(f, "Old", 3);
			std_DebugTuple_field(&t, &self->u.old, std_usize_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case ACCESS_TRANSFORM_SOURCE_MISSING:
			missing.has = self->u.missing.has;
			missing.some.value = &self->u.missing.bytes;
			missing.some.fmt = access_transform_vec_u8_Debug_fmt;
			t = std_fmt_debug_tuple(f, "Missing", 7);
			std_DebugTuple_field(&t, &missing, std_option_Debug_fmt);
			return std_DebugTuple_finish(&t);
	}
	pg_unreachable();
}

/* access_transform_Source_drop frees a missing value's bytes. */
void
access_transform_Source_drop(AccessTransformSource *self)
{
	if (self->tag == ACCESS_TRANSFORM_SOURCE_MISSING && self->u.missing.has)
		std_vec_free_Checked(&self->u.missing.bytes, ACCESS_TRANSFORM_BYTES);
}

/* access_transform_Field_Clone_clone copies a field, its source's bytes with it. */
void
access_transform_Field_Clone_clone(const AccessTransformField *self, AccessTransformField *result)
{
	result->typlen = self->typlen;
	result->storage = self->storage;
	access_transform_Source_Clone_clone(&self->source, &result->source);
}

/* access_transform_Field_PartialEq_eq reports whether two fields have equal lengths, storage kinds, and sources. */
bool
access_transform_Field_PartialEq_eq(const AccessTransformField *self, const AccessTransformField *other)
{
	return self->typlen == other->typlen && self->storage == other->storage &&
		access_transform_Source_PartialEq_eq(&self->source, &other->source);
}

/* access_transform_Field_Debug_fmt is Field's derived Debug. */
bool
access_transform_Field_Debug_fmt(const void *value, StdFormatter *f)
{
	const AccessTransformField *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Field", 5);

	std_DebugStruct_field(&s, "typlen", 6, &self->typlen, std_i16_Debug_fmt);
	std_DebugStruct_field(&s, "storage", 7, &self->storage, std_u8_Debug_fmt);
	std_DebugStruct_field(&s, "source", 6, &self->source, access_transform_Source_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* access_transform_Field_drop frees a field's source's bytes. */
void
access_transform_Field_drop(AccessTransformField *self)
{
	access_transform_Source_drop(&self->source);
}

/* access_transform_field_clone is Field's Clone as a vector's element clone. */
static void
access_transform_field_clone(const void *field, void *result)
{
	access_transform_Field_Clone_clone(field, result);
}

/* access_transform_field_drop is Field's drop glue as a vector's element. */
static void
access_transform_field_drop(void *field)
{
	access_transform_Field_drop(field);
}

/* access_transform_field_eq is Field's PartialEq as a vector's element comparison. */
static bool
access_transform_field_eq(const void *a, const void *b)
{
	return access_transform_Field_PartialEq_eq(a, b);
}

/* access_transform_Transform_Clone_clone copies a transform, each field's with it. */
void
access_transform_Transform_Clone_clone(const AccessTransformTransform *self, AccessTransformTransform *result)
{
	std_slice_to_vec_clone(&result->fields, self->fields.ptr, self->fields.len, ACCESS_TRANSFORM_FIELD,
						   access_transform_field_clone, access_transform_field_drop);
	result->target = self->target;
}

/* access_transform_Transform_PartialEq_eq reports whether two transforms have equal fields and targets. */
bool
access_transform_Transform_PartialEq_eq(const AccessTransformTransform *self, const AccessTransformTransform *other)
{
	return std_vec_PartialEq_eq(&self->fields, &other->fields, sizeof(AccessTransformField),
								access_transform_field_eq) &&
		self->target == other->target;
}

/* access_transform_fields_Debug_fmt is a Vec<Field>'s Debug, a list of its fields. */
static bool
access_transform_fields_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *fields = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t i = 0; i < fields->len; i++)
		std_DebugList_entry(&l, &STD_VEC_AT(fields, AccessTransformField, i), access_transform_Field_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* access_transform_Transform_Debug_fmt is Transform's derived Debug. */
bool
access_transform_Transform_Debug_fmt(const void *value, StdFormatter *f)
{
	const AccessTransformTransform *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Transform", 9);

	std_DebugStruct_field(&s, "fields", 6, &self->fields, access_transform_fields_Debug_fmt);
	std_DebugStruct_field(&s, "target", 6, &self->target, std_u32_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* access_transform_Transform_drop frees a transform's fields. */
void
access_transform_Transform_drop(AccessTransformTransform *self)
{
	std_vec_drop_Checked(&self->fields, ACCESS_TRANSFORM_FIELD, access_transform_field_drop);
}

/*
 * access_transform_fields_frame_t is the state of a making of a transform's
 * fields across its pad: the fields made so far while live, and what they
 * are made from.
 */
typedef struct access_transform_fields_frame_t
{
	const AccessTransformTransform *self;
	const AccessTransformTransform *later;
	const uint8 *bytes;
	size_t		bytes_len;
	StdVec		fields;
	bool		live;
	AccessTransformTransform *result;
	PgPgError  *error;
	bool		ok;
} access_transform_fields_frame_t;

/* access_transform_fields_pad drops the fields made so far. */
static void
access_transform_fields_pad(void *frame_)
{
	access_transform_fields_frame_t *frame = frame_;

	if (frame->live)
		std_vec_drop_Checked(&frame->fields, ACCESS_TRANSFORM_FIELD, access_transform_field_drop);
	frame->live = false;
}

/* access_transform_Transform_then_body gives each of the later transform's fields its source in this one's layout. */
static void
access_transform_Transform_then_body(void *frame_)
{
	access_transform_fields_frame_t *frame = frame_;

	std_vec_with_capacity_Checked(&frame->fields, frame->later->fields.len, ACCESS_TRANSFORM_FIELD);
	frame->live = true;
	for (size_t i = 0; i < frame->later->fields.len; i++)
	{
		const AccessTransformField *field = &STD_VEC_AT(&frame->later->fields, AccessTransformField, i);
		AccessTransformField made = {.typlen = field->typlen, .storage = field->storage};

		if (field->source.tag == ACCESS_TRANSFORM_SOURCE_OLD)
		{
			if (field->source.u.old >= frame->self->fields.len)
			{
				*frame->error = access_transform_corrupt();
				return;
			}
			access_transform_Source_Clone_clone(&STD_VEC_AT(&frame->self->fields, AccessTransformField,
															field->source.u.old).source, &made.source);
		}
		else
			access_transform_Source_Clone_clone(&field->source, &made.source);
		std_vec_push_Checked(&frame->fields, &made, ACCESS_TRANSFORM_FIELD);
	}
	frame->result->fields = frame->fields;
	frame->result->target = frame->later->target;
	frame->live = false;
	frame->ok = true;
}

/*
 * access_transform_Transform_then answers the transform that applies this
 * one and then a later one; it clones each missing value and each field's
 * source this one gives, since the composed transform outlives both.
 */
bool
access_transform_Transform_then(const AccessTransformTransform *self, const AccessTransformTransform *later,
								AccessTransformTransform *result, PgPgError *error)
{
	access_transform_fields_frame_t frame = {.self = self, .later = later, .result = result, .error = error};

	std_landing(&frame, access_transform_Transform_then_body, access_transform_fields_pad);
	access_transform_fields_pad(&frame);
	return frame.ok;
}

/* access_transform_encode_frame_t is encode's state across its pad: the bytes written so far, while live. */
typedef struct access_transform_encode_frame_t
{
	const AccessTransformTransform *self;
	StdVec	   *out;
	bool		live;
} access_transform_encode_frame_t;

/* access_transform_encode_pad frees the bytes written so far. */
static void
access_transform_encode_pad(void *frame_)
{
	access_transform_encode_frame_t *frame = frame_;

	if (frame->live)
		std_vec_free_Checked(frame->out, ACCESS_TRANSFORM_BYTES);
	frame->live = false;
}

/* access_transform_encode_u32 appends a u32's little-endian bytes. */
static void
access_transform_encode_u32(StdVec *out, uint32 value)
{
	uint8		le[4] = {(uint8) value, (uint8) (value >> 8), (uint8) (value >> 16), (uint8) (value >> 24)};

	std_vec_extend_from_slice_Checked(out, le, sizeof(le), ACCESS_TRANSFORM_BYTES);
}

/* access_transform_Transform_encode_body writes the target, the field count, and each field. */
static void
access_transform_Transform_encode_body(void *frame_)
{
	access_transform_encode_frame_t *frame = frame_;
	const StdVec *fields = &frame->self->fields;

	std_vec_new(frame->out, ACCESS_TRANSFORM_BYTES);
	frame->live = true;
	access_transform_encode_u32(frame->out, frame->self->target);
	access_transform_encode_u32(frame->out, (uint32) fields->len);
	for (size_t i = 0; i < fields->len; i++)
	{
		const AccessTransformField *field = &STD_VEC_AT(fields, AccessTransformField, i);
		uint16		typlen = (uint16) field->typlen;
		uint8		le[2] = {(uint8) typlen, (uint8) (typlen >> 8)};
		uint8		tag;

		std_vec_extend_from_slice_Checked(frame->out, le, sizeof(le), ACCESS_TRANSFORM_BYTES);
		std_vec_push_Checked(frame->out, &field->storage, ACCESS_TRANSFORM_BYTES);
		if (field->source.tag == ACCESS_TRANSFORM_SOURCE_OLD)
		{
			tag = ACCESS_TRANSFORM_SOURCE_OLD_TAG;
			std_vec_push_Checked(frame->out, &tag, ACCESS_TRANSFORM_BYTES);
			access_transform_encode_u32(frame->out, (uint32) field->source.u.old);
		}
		else if (!field->source.u.missing.has)
		{
			tag = ACCESS_TRANSFORM_SOURCE_NULL_TAG;
			std_vec_push_Checked(frame->out, &tag, ACCESS_TRANSFORM_BYTES);
		}
		else
		{
			const StdVec *bytes = &field->source.u.missing.bytes;

			tag = ACCESS_TRANSFORM_SOURCE_BYTES_TAG;
			std_vec_push_Checked(frame->out, &tag, ACCESS_TRANSFORM_BYTES);
			access_transform_encode_u32(frame->out, (uint32) bytes->len);
			std_vec_extend_from_slice_Checked(frame->out, bytes->ptr, bytes->len, ACCESS_TRANSFORM_BYTES);
		}
	}
	frame->live = false;
}

/*
 * access_transform_Transform_encode answers the transform's bytes,
 * Doltgrust's: the target and the field count, then each field's length,
 * storage kind, and tagged source, every integer little-endian.
 */
void
access_transform_Transform_encode(const AccessTransformTransform *self, StdVec *result)
{
	access_transform_encode_frame_t frame = {.self = self, .out = result};

	std_landing(&frame, access_transform_Transform_encode_body, access_transform_encode_pad);
}

/* access_transform_Transform_decode_closure_0 is decode's take: the next n bytes, or corruption past the end. */
static bool
access_transform_Transform_decode_closure_0(StdBytes *rest, size_t n, const uint8 **result, PgPgError *error)
{
	if (n > rest->len)
	{
		*error = access_transform_corrupt();
		return false;
	}
	*result = rest->ptr;
	rest->ptr += n;
	rest->len -= n;
	return true;
}

/* access_transform_Transform_decode_closure_1 is decode's u32_of: the u32 of four little-endian bytes. */
static uint32
access_transform_Transform_decode_closure_1(const uint8 *bytes)
{
	return (uint32) bytes[0] | (uint32) bytes[1] << 8 | (uint32) bytes[2] << 16 | (uint32) bytes[3] << 24;
}

/*
 * access_transform_Transform_decode_body reads the target, the field count,
 * and each field, and refuses bytes left over.
 */
static void
access_transform_Transform_decode_body(void *frame_)
{
	access_transform_fields_frame_t *frame = frame_;
	StdBytes	rest = {frame->bytes, frame->bytes_len};
	const uint8 *taken;
	uint32		target;
	uint32		count;

	if (!access_transform_Transform_decode_closure_0(&rest, 4, &taken, frame->error))
		return;
	target = access_transform_Transform_decode_closure_1(taken);
	if (!access_transform_Transform_decode_closure_0(&rest, 4, &taken, frame->error))
		return;
	count = access_transform_Transform_decode_closure_1(taken);
	std_vec_new(&frame->fields, ACCESS_TRANSFORM_FIELD);
	frame->live = true;
	for (uint32 i = 0; i < count; i++)
	{
		AccessTransformField field = {0};
		uint32		len;

		if (!access_transform_Transform_decode_closure_0(&rest, 2, &taken, frame->error))
			return;
		field.typlen = (int16) ((uint16) taken[0] | (uint16) taken[1] << 8);
		if (!access_transform_Transform_decode_closure_0(&rest, 1, &taken, frame->error))
			return;
		field.storage = taken[0];
		if (!access_transform_Transform_decode_closure_0(&rest, 1, &taken, frame->error))
			return;
		switch (taken[0])
		{
			case ACCESS_TRANSFORM_SOURCE_OLD_TAG:
				if (!access_transform_Transform_decode_closure_0(&rest, 4, &taken, frame->error))
					return;
				field.source.tag = ACCESS_TRANSFORM_SOURCE_OLD;
				field.source.u.old = access_transform_Transform_decode_closure_1(taken);
				break;
			case ACCESS_TRANSFORM_SOURCE_NULL_TAG:
				field.source.tag = ACCESS_TRANSFORM_SOURCE_MISSING;
				break;
			case ACCESS_TRANSFORM_SOURCE_BYTES_TAG:
				if (!access_transform_Transform_decode_closure_0(&rest, 4, &taken, frame->error))
					return;
				len = access_transform_Transform_decode_closure_1(taken);
				if (!access_transform_Transform_decode_closure_0(&rest, len, &taken, frame->error))
					return;
				std_vec_reserve_Checked(&frame->fields, 1, ACCESS_TRANSFORM_FIELD);
				field.source.tag = ACCESS_TRANSFORM_SOURCE_MISSING;
				field.source.u.missing.has = true;
				std_slice_to_vec_Checked(&field.source.u.missing.bytes, taken, len, ACCESS_TRANSFORM_BYTES);
				break;
			default:
				*frame->error = access_transform_corrupt();
				return;
		}
		std_vec_push_Checked(&frame->fields, &field, ACCESS_TRANSFORM_FIELD);
	}
	if (rest.len != 0)
	{
		*frame->error = access_transform_corrupt();
		return;
	}
	frame->result->fields = frame->fields;
	frame->result->target = target;
	frame->live = false;
	frame->ok = true;
}

/*
 * access_transform_Transform_decode answers the transform of bytes encode
 * wrote, refusing any other bytes as data corruption.
 */
bool
access_transform_Transform_decode(const uint8 *bytes, size_t bytes_len, AccessTransformTransform *result,
								  PgPgError *error)
{
	access_transform_fields_frame_t frame = {.bytes = bytes, .bytes_len = bytes_len, .result = result,
	.error = error};

	std_landing(&frame, access_transform_Transform_decode_body, access_transform_fields_pad);
	access_transform_fields_pad(&frame);
	return frame.ok;
}

/*
 * access_transform_compose_frame_t is compose's state across its pad: the
 * transform composed so far, while it has one.
 */
typedef struct access_transform_compose_frame_t
{
	const AccessTransformTransform *chain;
	size_t		chain_len;
	bool		has;
	AccessTransformTransform composed;
	PgPgError  *error;
	bool		ok;
} access_transform_compose_frame_t;

/* access_transform_compose_pad drops the transform composed so far. */
static void
access_transform_compose_pad(void *frame_)
{
	access_transform_compose_frame_t *frame = frame_;

	if (frame->has)
		access_transform_Transform_drop(&frame->composed);
	frame->has = false;
}

/* access_transform_compose_body composes each transform of the chain onto those before it. */
static void
access_transform_compose_body(void *frame_)
{
	access_transform_compose_frame_t *frame = frame_;

	for (size_t i = 0; i < frame->chain_len; i++)
	{
		AccessTransformTransform made;

		if (!frame->has)
			access_transform_Transform_Clone_clone(&frame->chain[i], &made);
		else
		{
			if (!access_transform_Transform_then(&frame->composed, &frame->chain[i], &made, frame->error))
				return;
			access_transform_Transform_drop(&frame->composed);
		}
		frame->composed = made;
		frame->has = true;
	}
	frame->ok = true;
}

/*
 * access_transform_compose answers the transform that applies a chain of
 * transforms in order, or none for an empty chain; it clones the chain's
 * first, since the composed transform outlives the chain.
 */
bool
access_transform_compose(const AccessTransformTransform *chain, size_t chain_len, bool *has_result,
						 AccessTransformTransform *result, PgPgError *error)
{
	access_transform_compose_frame_t frame = {.chain = chain, .chain_len = chain_len, .error = error};

	std_landing(&frame, access_transform_compose_body, access_transform_compose_pad);
	if (frame.ok)
	{
		*has_result = frame.has;
		if (frame.has)
			*result = frame.composed;
		frame.has = false;
	}
	access_transform_compose_pad(&frame);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_ACCESS

/* access_transform_tests_field returns a field of an int4's length and plain storage from a source. */
static AccessTransformField
access_transform_tests_field(AccessTransformSource source)
{
	AccessTransformField field = {.typlen = 4, .storage = 'p', .source = source};

	return field;
}

/* access_transform_tests_old returns Source::Old of a position. */
static AccessTransformSource
access_transform_tests_old(size_t at)
{
	AccessTransformSource source = {.tag = ACCESS_TRANSFORM_SOURCE_OLD, .u.old = at};

	return source;
}

/* access_transform_tests_missing returns Source::Missing of n copies of a byte, or of none for n zero and no bytes. */
static AccessTransformSource
access_transform_tests_missing(bool has, uint8 byte, size_t n)
{
	AccessTransformSource source = {.tag = ACCESS_TRANSFORM_SOURCE_MISSING};

	source.u.missing.has = has;
	if (has)
		std_vec_u8_from_elem(&source.u.missing.bytes, byte, n);
	return source;
}

/* access_transform_tests_transform returns a transform of three fields at most, its first count of them taken. */
static AccessTransformTransform
access_transform_tests_transform(AccessTransformField *fields, size_t count, uint32 target)
{
	AccessTransformTransform transform = {.target = target};

	std_vec_with_capacity_Checked(&transform.fields, count, ACCESS_TRANSFORM_FIELD);
	for (size_t i = 0; i < count; i++)
		std_vec_push_Checked(&transform.fields, &fields[i], ACCESS_TRANSFORM_FIELD);
	return transform;
}

/* access_transform_tests_decoded_is_err reports whether bytes fail to decode, dropping what decoding gave. */
static bool
access_transform_tests_decoded_is_err(const uint8 *bytes, size_t len)
{
	AccessTransformTransform decoded;
	PgPgError	error;

	if (access_transform_Transform_decode(bytes, len, &decoded, &error))
	{
		access_transform_Transform_drop(&decoded);
		return false;
	}
	pg_PgError_drop(&error);
	return true;
}

/*
 * A transform's bytes decode to the transform, and bytes cut short, tagged
 * wrongly, or with bytes left over are data corruption.
 */
void
test_access_transform_tests_round_trip(void)
{
	AccessTransformField fields[3] = {
		access_transform_tests_field(access_transform_tests_old(0)),
		access_transform_tests_field(access_transform_tests_missing(false, 0, 0)),
		access_transform_tests_field(access_transform_tests_missing(true, 7, 4)),
	};
	AccessTransformTransform transform = access_transform_tests_transform(fields, 3, 2032);
	StdVec		bytes;
	AccessTransformTransform decoded;
	PgPgError	error;
	StdOptionRef left;
	StdOptionRef right = {true, {&transform, access_transform_Transform_Debug_fmt}};
	StdVec		tagged;
	uint8		zero = 0;

	access_transform_Transform_encode(&transform, &bytes);
	left.has = access_transform_Transform_decode(bytes.ptr, bytes.len, &decoded, &error);
	if (!left.has)
		pg_PgError_drop(&error);
	left.some.value = &decoded;
	left.some.fmt = access_transform_Transform_Debug_fmt;
	STD_ASSERT_EQ(161, 9, left.has && access_transform_Transform_PartialEq_eq(&decoded, &transform), &left,
				  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	if (left.has)
		access_transform_Transform_drop(&decoded);
	access_transform_Transform_drop(&transform);
	std_range_to_check(STD_SUB(usize, bytes.len, 1, 162, 44), bytes.len, RUST_FILE, 162, 36);
	STD_ASSERT(162, 9, access_transform_tests_decoded_is_err(bytes.ptr, bytes.len - 1),
			   "Transform::decode(&bytes[..bytes.len() - 1]).is_err()");
	std_vec_clone(&tagged, &bytes, ACCESS_TRANSFORM_BYTES);
	std_index_check(11, tagged.len, RUST_FILE, 164, 9);
	((uint8 *) tagged.ptr)[11] = 9;
	STD_ASSERT(165, 9, access_transform_tests_decoded_is_err(tagged.ptr, tagged.len),
			   "Transform::decode(&tagged).is_err()");
	std_vec_free_Checked(&tagged, ACCESS_TRANSFORM_BYTES);
	std_vec_push_Checked(&bytes, &zero, ACCESS_TRANSFORM_BYTES);
	STD_ASSERT(168, 9, access_transform_tests_decoded_is_err(bytes.ptr, bytes.len),
			   "Transform::decode(&longer).is_err()");
	std_vec_free_Checked(&bytes, ACCESS_TRANSFORM_BYTES);
}

/* access_transform_tests_sources_t is a list of borrowed sources, a Vec<&Source> or a [&Source; 3]. */
typedef struct access_transform_tests_sources_t
{
	const AccessTransformSource *items[3];
	size_t		len;
} access_transform_tests_sources_t;

/* access_transform_tests_sources_Debug_fmt is a list of borrowed sources' Debug. */
static bool
access_transform_tests_sources_Debug_fmt(const void *value, StdFormatter *f)
{
	const access_transform_tests_sources_t *sources = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t i = 0; i < sources->len; i++)
		std_DebugList_entry(&l, sources->items[i], access_transform_Source_Debug_fmt);
	return std_DebugList_finish(&l);
}

/*
 * Composing a drop of the middle field, then an add with a missing value,
 * gives each new field its source in the first layout, and an empty chain
 * composes to none.
 */
void
test_access_transform_tests_composed(void)
{
	AccessTransformField dropped_fields[2] = {
		access_transform_tests_field(access_transform_tests_old(0)),
		access_transform_tests_field(access_transform_tests_old(2)),
	};
	AccessTransformTransform dropped = access_transform_tests_transform(dropped_fields, 2, 2032);
	AccessTransformField added_fields[3] = {
		access_transform_tests_field(access_transform_tests_old(0)),
		access_transform_tests_field(access_transform_tests_old(1)),
		access_transform_tests_field(access_transform_tests_missing(true, 1, 4)),
	};
	AccessTransformTransform added = access_transform_tests_transform(added_fields, 3, 2032);
	AccessTransformTransform chain[2] = {dropped, added};
	bool		has;
	AccessTransformTransform composed;
	PgPgError	error;
	AccessTransformSource expected[3] = {
		access_transform_tests_old(0), access_transform_tests_old(2), access_transform_tests_missing(true, 1, 4),
	};
	access_transform_tests_sources_t left = {0};
	access_transform_tests_sources_t right = {{&expected[0], &expected[1], &expected[2]}, 3};
	bool		same;
	AccessTransformField past_fields[1] = {access_transform_tests_field(access_transform_tests_old(5))};
	AccessTransformTransform past = access_transform_tests_transform(past_fields, 1, 2032);
	AccessTransformTransform then;

	if (!access_transform_compose(chain, 2, &has, &composed, &error))
	{
		pg_PgError_drop(&error);
		has = false;
	}
	if (!has)
		std_panic(RUST_FILE, 180, 67, "a chain of two composes", sizeof("a chain of two composes") - 1);
	same = composed.fields.len == right.len;
	for (size_t i = 0; i < composed.fields.len && i < lengthof(left.items); i++)
	{
		left.items[i] = &STD_VEC_AT(&composed.fields, AccessTransformField, i).source;
		left.len++;
		same &= access_transform_Source_PartialEq_eq(left.items[i], right.items[i]);
	}
	STD_ASSERT_EQ(182, 9, same, &left, access_transform_tests_sources_Debug_fmt, &right,
				  access_transform_tests_sources_Debug_fmt);
	access_transform_Source_drop(&expected[2]);
	access_transform_Transform_drop(&composed);
	same = access_transform_compose(chain, 0, &has, &composed, &error);
	if (!same)
		pg_PgError_drop(&error);
	else if (has)
		access_transform_Transform_drop(&composed);
	STD_ASSERT(183, 9, same && !has, "matches!(compose([]), Ok(None))");
	same = access_transform_Transform_then(&dropped, &past, &then, &error);
	if (same)
		access_transform_Transform_drop(&then);
	else
		pg_PgError_drop(&error);
	STD_ASSERT(185, 9, !same, "dropped.then(&past).is_err()");
	access_transform_Transform_drop(&past);
	access_transform_Transform_drop(&added);
	access_transform_Transform_drop(&dropped);
}
#endif
#endif
