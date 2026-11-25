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
 * The row tuples that tables' leaves hold: a varint of the field count and
 * flags, a bitmap of NULLs, one of external fields when any is, each field's
 * end as two bytes, the fields' data, and the address of the rest of a row
 * too wide for one tuple.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/varint.h"
#include "doltgres/encoding/tuple.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"

#define RUST_FILE "encoding" STD_SEP "src" STD_SEP "tuple.rs"

/*
 * encoding_tuple_write writes a tuple of fields, NULL where a field is none,
 * with the address and field count of the row's overflow when the row
 * continues past these fields.
 */
void
encoding_tuple_write(const StdBytes *fields, size_t fields_len, bool has_overflow,
					 encoding_tuple_write_overflow_t overflow, StdVec *result)
{
	encoding_tuple_write_with_external(fields, fields_len, STD_DANGLING, 0, has_overflow, overflow, result);
}

/* EncodingTupleWriteWithExternalClosure2 is bitmap's captures: the fields, by reference. */
typedef struct EncodingTupleWriteWithExternalClosure2
{
	const StdBytes *fields;
	size_t		fields_len;
} EncodingTupleWriteWithExternalClosure2;

/*
 * encoding_tuple_write_with_external_closure_2 is bitmap: a bit for each
 * field, set where marked holds.
 */
static void
encoding_tuple_write_with_external_closure_2(void *captures, bool (*marked) (void *, size_t),
											 void *marked_captures, StdVec *result)
{
	EncodingTupleWriteWithExternalClosure2 *c = captures;
	volatile StdVec bits;
	volatile bool bits_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *bits_v = (StdVec *) &bits;

		std_vec_from_elem_zeroed(bits_v, std_div_ceil_usize(c->fields_len, 8, RUST_FILE, 57, 47), 1, 1);
		bits_live = true;
		for (size_t i = 0, i_end = c->fields_len; i < i_end; i++)
		{
			uint8		tmp_1;
			size_t		tmp_2;

			if (!marked(marked_captures, i))
				continue;
			tmp_1 = STD_SHL(u8, 1, (uint32) STD_REM_FOLDED(usize, i, 8), 59, 28);
			tmp_2 = STD_DIV_FOLDED(usize, i, 8);
			std_index_check(tmp_2, bits_v->len, RUST_FILE, 59, 17);
			STD_VEC_AT(bits_v, uint8, tmp_2) |= tmp_1;
		}
		*result = *bits_v;
		bits_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (bits_live)
			std_vec_free_Checked((StdVec *) &bits, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/* EncodingTupleWriteWithExternalClosure3 is the NULLs' test's captures: the fields, by reference. */
typedef struct EncodingTupleWriteWithExternalClosure3
{
	const StdBytes *fields;
	size_t		fields_len;
} EncodingTupleWriteWithExternalClosure3;

/* encoding_tuple_write_with_external_closure_3 reports whether a field is NULL. */
static bool
encoding_tuple_write_with_external_closure_3(void *captures, size_t i)
{
	EncodingTupleWriteWithExternalClosure3 *c = captures;

	std_index_check(i, c->fields_len, RUST_FILE, 63, 40);
	return c->fields[i].ptr == NULL;
}

/* EncodingTupleWriteWithExternalClosure4 is the external test's captures: the flags, by reference. */
typedef struct EncodingTupleWriteWithExternalClosure4
{
	const bool *external;
	size_t		external_len;
} EncodingTupleWriteWithExternalClosure4;

/* encoding_tuple_write_with_external_closure_4 reports whether a field is held out of line. */
static bool
encoding_tuple_write_with_external_closure_4(void *captures, size_t i)
{
	EncodingTupleWriteWithExternalClosure4 *c = captures;

	return i < c->external_len ? c->external[i] : false;
}

/*
 * encoding_tuple_write_with_external writes a tuple whose fields marked
 * external hold a blob's reference, with a bitmap of them when any is.
 */
void
encoding_tuple_write_with_external(const StdBytes *fields, size_t fields_len, const bool *external,
								   size_t external_len, bool has_overflow,
								   encoding_tuple_write_overflow_t overflow, StdVec *result)
{
	size_t		data = 0;
	bool		any_external = false;
	uint64		flags;
	volatile StdVec out;
	volatile bool out_live = false;
	volatile StdVec tmp_1;
	volatile bool tmp_1_live = false;
	StdUnwind	unwind;

	for (size_t i = 0, i_len = fields_len; i < i_len; i++)
		data = std_add_usize(data, fields[i].ptr == NULL ? 0 : fields[i].len, STD_ACCUM_FILE, 206, 1);
	if (!(data <= PG_UINT16_MAX))
		STD_PANIC(45, 5, "tuple data is larger than 64 KB");
	for (size_t i = 0, i_len = external_len; i < i_len; i++)
	{
		if (external[i])
		{
			any_external = true;
			break;
		}
	}
	flags = STD_SHL_FOLDED(u64, (uint64) any_external, 1) | (uint64) has_overflow;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *out_v = (StdVec *) &out;
		StdVec	   *tmp_1_v = (StdVec *) &tmp_1;
		EncodingTupleWriteWithExternalClosure2 bitmap = {fields, fields_len};
		size_t		end = 0;
		size_t		cap;

		cap = STD_MUL(usize, fields_len, 2, 48, 42);
		cap = STD_ADD(usize, 4, cap, 48, 38);
		cap = STD_ADD(usize, cap, std_div_ceil_usize(fields_len, 4, RUST_FILE, 48, 74), 48, 38);
		cap = STD_ADD(usize, cap, data, 48, 38);
		cap = STD_ADD(usize, cap, ENCODING_TUPLE_ADDRESS_LEN, 48, 38);
		std_vec_with_capacity_Checked(out_v, cap, 1, 1);
		out_live = true;
		if (!has_overflow)
			chunks_varint_put_varint(out_v, STD_SHL_FOLDED(u64, (uint64) fields_len, 2) | flags);
		else
		{
			chunks_varint_put_varint(out_v, STD_SHL_FOLDED(u64, (uint64) overflow.f1, 2) | flags);
			chunks_varint_put_varint(out_v, (uint64) fields_len);
		}
		{
			EncodingTupleWriteWithExternalClosure3 nulls = {fields, fields_len};

			encoding_tuple_write_with_external_closure_2(&bitmap, encoding_tuple_write_with_external_closure_3,
														 &nulls, tmp_1_v);
			tmp_1_live = true;
			std_vec_extend_from_slice_Checked(out_v, tmp_1_v->ptr, tmp_1_v->len, 1, 1);
			tmp_1_live = false;
			std_vec_free_Checked(tmp_1_v, 1, 1);
		}
		if (any_external)
		{
			EncodingTupleWriteWithExternalClosure4 marked = {external, external_len};

			encoding_tuple_write_with_external_closure_2(&bitmap, encoding_tuple_write_with_external_closure_4,
														 &marked, tmp_1_v);
			tmp_1_live = true;
			std_vec_extend_from_slice_Checked(out_v, tmp_1_v->ptr, tmp_1_v->len, 1, 1);
			tmp_1_live = false;
			std_vec_free_Checked(tmp_1_v, 1, 1);
		}
		for (size_t i = 0, i_len = fields_len; i < i_len; i++)
		{
			uint8		le[2];

			end = STD_ADD(usize, end, fields[i].ptr == NULL ? 0 : fields[i].len, 69, 9);
			std_u16_to_le_bytes((uint16) end, le);
			std_vec_extend_from_slice_Checked(out_v, le, 2, 1, 1);
		}
		for (size_t i = 0, i_len = fields_len; i < i_len; i++)
			if (fields[i].ptr != NULL)
				std_vec_extend_from_slice_Checked(out_v, fields[i].ptr, fields[i].len, 1, 1);
		if (has_overflow)
			std_vec_extend_from_slice_Checked(out_v, overflow.f0, ENCODING_TUPLE_ADDRESS_LEN, 1, 1);
		*result = *out_v;
		out_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_1_live)
			std_vec_free_Checked((StdVec *) &tmp_1, 1, 1);
		if (out_live)
			std_vec_free_Checked((StdVec *) &out, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/*
 * encoding_tuple_written_len returns the length write_with_external gives
 * fields of these lengths, None for a NULL, with any held out of line, and
 * with an overflow of a field count.
 */
size_t
encoding_tuple_written_len(const StdUsizeOption *fields, size_t fields_len, bool external, bool has_overflow,
						   size_t overflow)
{
	uint64		flags = STD_SHL_FOLDED(u64, (uint64) external, 1) | (uint64) has_overflow;
	size_t		bitmap = std_div_ceil_usize(fields_len, 8, RUST_FILE, 84, 31);
	size_t		head;
	size_t		data = 0;
	size_t		tmp_1;

	if (!has_overflow)
		head = chunks_varint_varint_len(STD_SHL_FOLDED(u64, (uint64) fields_len, 2) | flags);
	else
	{
		tmp_1 = chunks_varint_varint_len(STD_SHL_FOLDED(u64, (uint64) overflow, 2) | flags);
		tmp_1 = STD_ADD(usize, tmp_1, chunks_varint_varint_len((uint64) fields_len), 87, 24);
		head = STD_ADD(usize, tmp_1, ENCODING_TUPLE_ADDRESS_LEN, 87, 24);
	}
	for (size_t i = 0, i_len = fields_len; i < i_len; i++)
		data = std_add_usize(data, fields[i].has ? fields[i].x : 0, STD_ACCUM_FILE, 206, 1);
	tmp_1 = STD_ADD(usize, 1, (size_t) external, 90, 22);
	tmp_1 = STD_MUL(usize, bitmap, tmp_1, 90, 12);
	tmp_1 = STD_ADD(usize, head, tmp_1, 90, 5);
	tmp_1 = STD_ADD(usize, tmp_1, STD_MUL(usize, 2, fields_len, 90, 47), 90, 5);
	return STD_ADD(usize, tmp_1, data, 90, 5);
}

/* encoding_tuple_reference returns an external field's value: its blob's length and address. */
void
encoding_tuple_reference(uint64 length, const uint8 *address, StdVec *result)
{
	volatile StdVec out;
	volatile bool out_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *out_v = (StdVec *) &out;

		std_vec_with_capacity_Checked(out_v, ENCODING_TUPLE_ADDRESS_LEN + 4, 1, 1);
		out_live = true;
		chunks_varint_put_varint(out_v, length);
		std_vec_extend_from_slice_Checked(out_v, address, ENCODING_TUPLE_ADDRESS_LEN, 1, 1);
		*result = *out_v;
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

/* encoding_tuple_Field_Debug_fmt is Field's derived Debug. */
bool
encoding_tuple_Field_Debug_fmt(const void *value, StdFormatter *f)
{
	const EncodingTupleField *self = (const EncodingTupleField *) value;
	StdDebugTuple t;
	StdDebugStruct s;

	switch (self->tag)
	{
		case ENCODING_TUPLE_FIELD_NULL:
			return std_fmt_write_str(f, "Null", 4);
		case ENCODING_TUPLE_FIELD_VALUE:
			t = std_fmt_debug_tuple(f, "Value", 5);
			std_DebugTuple_field(&t, &self->u.value, std_bytes_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case ENCODING_TUPLE_FIELD_OVERFLOW:
			t = std_fmt_debug_tuple(f, "Overflow", 8);
			std_DebugTuple_field(&t, &self->u.overflow, std_usize_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case ENCODING_TUPLE_FIELD_EXTERNAL:
			s = std_fmt_debug_struct(f, "External", 8);
			std_DebugStruct_field(&s, "length", 6, &self->u.external.length, std_u64_Debug_fmt);
			std_DebugStruct_field(&s, "address", 7, &self->u.external.address, std_bytes_Debug_fmt);
			std_DebugStruct_field(&s, "at", 2, &self->u.external.at, std_usize_Debug_fmt);
			return std_DebugStruct_finish(&s);
	}
	pg_unreachable();
}

/* encoding_tuple_Field_PartialEq_eq is Field's derived PartialEq. */
bool
encoding_tuple_Field_PartialEq_eq(const EncodingTupleField *self, const EncodingTupleField *other)
{
	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case ENCODING_TUPLE_FIELD_NULL:
			return true;
		case ENCODING_TUPLE_FIELD_VALUE:
			return self->u.value.len == other->u.value.len &&
				memcmp(self->u.value.ptr, other->u.value.ptr, self->u.value.len) == 0;
		case ENCODING_TUPLE_FIELD_OVERFLOW:
			return self->u.overflow == other->u.overflow;
		case ENCODING_TUPLE_FIELD_EXTERNAL:
			return self->u.external.length == other->u.external.length &&
				self->u.external.address.len == other->u.external.address.len &&
				memcmp(self->u.external.address.ptr, other->u.external.address.ptr,
					   self->u.external.address.len) == 0 &&
				self->u.external.at == other->u.external.at;
	}
	pg_unreachable();
}

/* encoding_tuple_bitmap_len returns the length of a bitmap of a number of bits. */
static size_t
encoding_tuple_bitmap_len(size_t n)
{
	return STD_ADD(usize, STD_DIV_FOLDED(usize, n, 8), std_is_multiple_of_usize(n, 8) ? 0 : 1, 139, 5);
}

/* encoding_tuple_Tuple_parse parses a tuple, or returns nothing when its bytes are malformed. */
bool
encoding_tuple_Tuple_parse(const uint8 *bytes, size_t bytes_len, EncodingTupleTuple *result)
{
	size_t		at = 0;
	uint64		head;
	size_t		total;
	size_t		inline_;
	size_t		nulls_at;
	size_t		bitmap;
	bool		has_external_at = false;
	size_t		external_at = 0;
	size_t		ends_at;
	size_t		data_at;
	bool		ordered = true;
	size_t		data = 0;
	size_t		i = 0;
	size_t		overflow;
	ChunksVarintMalformed malformed;

	if (!chunks_varint_decode(bytes, bytes_len, &at, &head, &malformed))
		return false;
	total = (size_t) STD_SHR_FOLDED(u64, head, 2);
	if ((head & 1) == 1)
	{
		uint64		count;

		if (!chunks_varint_decode(bytes, bytes_len, &at, &count, &malformed))
			return false;
		inline_ = (size_t) count;
	}
	else
		inline_ = total;
	if (inline_ > total || inline_ > STD_DIV_FOLDED(usize, bytes_len, 2))
		return false;
	nulls_at = at;
	bitmap = encoding_tuple_bitmap_len(inline_);
	if ((head & 2) == 2)
	{
		if (!std_checked_add_usize(nulls_at, bitmap, &external_at))
			return false;
		has_external_at = true;
	}
	if (!std_checked_add_usize(has_external_at ? external_at : nulls_at, bitmap, &ends_at))
		return false;
	if (!std_checked_add_usize(ends_at, STD_MUL(usize, 2, inline_, 208, 43), &data_at))
		return false;
	if (data_at > bytes_len)
		return false;
	while (i < inline_)
	{
		size_t		lo;
		size_t		hi;
		size_t		end;

		lo = STD_MUL(usize, 2, i, 230, 39);
		lo = STD_ADD(usize, ends_at, lo, 230, 29);
		std_index_check(lo, bytes_len, RUST_FILE, 230, 23);
		hi = STD_MUL(usize, 2, i, 230, 79);
		hi = STD_ADD(usize, ends_at, hi, 230, 69);
		hi = STD_ADD(usize, hi, 1, 230, 69);
		std_index_check(hi, bytes_len, RUST_FILE, 230, 63);
		end = STD_MUL(usize, 256, (size_t) bytes[hi], 230, 57);
		end = STD_ADD(usize, (size_t) bytes[lo], end, 230, 23);
		ordered &= end >= data;
		data = end;
		i = STD_ADD(usize, i, 1, 264, 13);
	}
	overflow = inline_ < total ? ENCODING_TUPLE_ADDRESS_LEN : 0;
	if (ordered &&
		STD_SUB(usize, bytes_len, data_at, 271, 23) ==
		STD_ADD(usize, data, overflow, 271, 48))
	{
		result->bytes = bytes;
		result->bytes_len = bytes_len;
		result->total = total;
		result->inline_ = inline_;
		result->nulls_at = nulls_at;
		result->has_external_at = has_external_at;
		result->external_at = external_at;
		result->ends_at = ends_at;
		result->data_at = data_at;
		return true;
	}
	return false;
}

/* encoding_tuple_Tuple_end returns the end offset of the field at the position in the data. */
static size_t
encoding_tuple_Tuple_end(const EncodingTupleTuple *self, size_t i)
{
	size_t		at;
	size_t		tmp_1;
	size_t		tmp_2;

	at = STD_MUL(usize, 2, i, 296, 33);
	at = STD_ADD(usize, self->ends_at, at, 296, 18);
	std_index_check(at, self->bytes_len, RUST_FILE, 297, 9);
	tmp_1 = STD_ADD(usize, at, 1, 297, 52);
	std_index_check(tmp_1, self->bytes_len, RUST_FILE, 297, 41);
	tmp_2 = STD_MUL(usize, 256, (size_t) self->bytes[tmp_1], 297, 35);
	return STD_ADD(usize, (size_t) self->bytes[at], tmp_2, 297, 9);
}

/* encoding_tuple_Tuple_len returns the number of fields the tuple holds, inline or in its overflow blob. */
size_t
encoding_tuple_Tuple_len(const EncodingTupleTuple *self)
{
	return self->total;
}

/* encoding_tuple_Tuple_is_empty reports whether the tuple has no fields. */
bool
encoding_tuple_Tuple_is_empty(const EncodingTupleTuple *self)
{
	return self->total == 0;
}

#ifdef DOLTGRES_TEST_ENCODING
/* encoding_tuple_Tuple_has_external reports whether any of the tuple's fields is out of line. */
bool
encoding_tuple_Tuple_has_external(const EncodingTupleTuple *self)
{
	return self->has_external_at;
}
#endif

static bool encoding_tuple_Tuple_external(const EncodingTupleTuple *self, size_t i);

/*
 * encoding_tuple_Tuple_field returns the field at a position: NULL past the
 * tuple's fields, and the position in the overflow blob for one held there.
 */
EncodingTupleField
encoding_tuple_Tuple_field(const EncodingTupleTuple *self, size_t i)
{
	EncodingTupleField field = {0};
	size_t		at;
	size_t		start;
	size_t		end;
	uint8		tmp_1;

	if (i >= self->total)
	{
		field.tag = ENCODING_TUPLE_FIELD_NULL;
		return field;
	}
	if (i >= self->inline_)
	{
		field.tag = ENCODING_TUPLE_FIELD_OVERFLOW;
		field.u.overflow = STD_SUB(usize, i, self->inline_, 325, 36);
		return field;
	}
	at = STD_ADD(usize, self->nulls_at, STD_DIV_FOLDED(usize, i, 8), 327, 23);
	std_index_check(at, self->bytes_len, RUST_FILE, 327, 12);
	tmp_1 = STD_SHL(u8, 1, (uint32) STD_REM_FOLDED(usize, i, 8), 327, 49);
	if ((self->bytes[at] & tmp_1) != 0)
	{
		field.tag = ENCODING_TUPLE_FIELD_NULL;
		return field;
	}
	if (i == 0)
		start = 0;
	else
		start = encoding_tuple_Tuple_end(self, STD_SUB(usize, i, 1, 333, 22));
	start = STD_ADD(usize, self->data_at, start, 330, 21);
	end = STD_ADD(usize, self->data_at, encoding_tuple_Tuple_end(self, i), 335, 19);
	if (encoding_tuple_Tuple_external(self, i))
	{
		size_t		pos = start;
		uint64		length;
		ChunksVarintMalformed malformed;

		std_range_to_check(end, self->bytes_len, RUST_FILE, 347, 44);
		if (!chunks_varint_decode(self->bytes, end, &pos, &length, &malformed))
			length = 0;
		std_range_check(pos, end, self->bytes_len, RUST_FILE, 348, 66);
		field.tag = ENCODING_TUPLE_FIELD_EXTERNAL;
		field.u.external.length = length;
		field.u.external.address.ptr = self->bytes + pos;
		field.u.external.address.len = end - pos;
		field.u.external.at = pos;
		return field;
	}
	std_range_check(start, end, self->bytes_len, RUST_FILE, 350, 33);
	field.tag = ENCODING_TUPLE_FIELD_VALUE;
	field.u.value.ptr = self->bytes + start;
	field.u.value.len = end - start;
	return field;
}

/* encoding_tuple_Tuple_external reports whether a field is out of line. */
static bool
encoding_tuple_Tuple_external(const EncodingTupleTuple *self, size_t i)
{
	if (self->has_external_at)
	{
		size_t		at = STD_ADD(usize, self->external_at, STD_DIV_FOLDED(usize, i, 8), 362, 45);
		uint8		tmp_1;

		std_index_check(at, self->bytes_len, RUST_FILE, 362, 34);
		tmp_1 = STD_SHL(u8, 1, (uint32) STD_REM_FOLDED(usize, i, 8), 362, 69);
		return (self->bytes[at] & tmp_1) != 0;
	}
	return false;
}

/* encoding_tuple_Tuple_fields returns a walk of the tuple's fields. */
EncodingTupleFields
encoding_tuple_Tuple_fields(const EncodingTupleTuple *self)
{
	EncodingTupleFields fields = {self, 0, 0};

	return fields;
}

/*
 * encoding_tuple_Tuple_overflow returns the address of the tuple's overflow
 * blob and where it is in the tuple, or nothing when it has none.
 */
bool
encoding_tuple_Tuple_overflow(const EncodingTupleTuple *self, StdBytes *result_0, size_t *result_1)
{
	if (self->inline_ < self->total)
	{
		size_t		at = STD_SUB(usize, self->bytes_len, ENCODING_TUPLE_ADDRESS_LEN, 378, 22);

		std_range_from_check(at, self->bytes_len, RUST_FILE, 379, 30);
		result_0->ptr = self->bytes + at;
		result_0->len = self->bytes_len - at;
		*result_1 = at;
		return true;
	}
	return false;
}

static bool encoding_tuple_Fields_step(EncodingTupleFields *self, EncodingTupleField *result);

/*
 * encoding_tuple_Fields_at returns the field at a position and goes on from
 * it, reading the field's start from the ends only when it is not the next.
 */
EncodingTupleField
encoding_tuple_Fields_at(EncodingTupleFields *self, size_t i)
{
	size_t		start;
	EncodingTupleField field = {0};

	if (i != self->next && i < self->tuple->inline_)
	{
		if (i == 0)
			start = 0;
		else
			start = encoding_tuple_Tuple_end(self->tuple, STD_SUB(usize, i, 1, 415, 32));
	}
	else
		start = self->start;
	self->next = i;
	self->start = start;
	if (!encoding_tuple_Fields_step(self, &field))
		field.tag = ENCODING_TUPLE_FIELD_NULL;
	return field;
}

/* encoding_tuple_Fields_step returns the next field and moves past it, or nothing past the last. */
static bool
encoding_tuple_Fields_step(EncodingTupleFields *self, EncodingTupleField *result)
{
	const EncodingTupleTuple *tuple = self->tuple;
	size_t		i = self->next;
	size_t		end;
	size_t		start;
	size_t		at;
	uint8		tmp_1;

	if (i >= tuple->total)
		return false;
	if (i >= tuple->inline_)
	{
		self->next = STD_ADD(usize, i, 1, 436, 43);
		result->tag = ENCODING_TUPLE_FIELD_OVERFLOW;
		result->u.overflow = STD_SUB(usize, i, tuple->inline_, 437, 41);
		return true;
	}
	end = encoding_tuple_Tuple_end(tuple, i);
	start = self->start;
	self->next = STD_ADD(usize, i, 1, 452, 39);
	self->start = end;
	at = STD_ADD(usize, tuple->nulls_at, STD_DIV_FOLDED(usize, i, 8), 453, 24);
	std_index_check(at, tuple->bytes_len, RUST_FILE, 453, 12);
	tmp_1 = STD_SHL(u8, 1, (uint32) STD_REM_FOLDED(usize, i, 8), 453, 51);
	if ((tuple->bytes[at] & tmp_1) != 0)
	{
		result->tag = ENCODING_TUPLE_FIELD_NULL;
		return true;
	}
	if (encoding_tuple_Tuple_external(tuple, i))
	{
		*result = encoding_tuple_Tuple_field(tuple, i);
		return true;
	}
	{
		size_t		from = STD_ADD(usize, tuple->data_at, start, 459, 40);
		size_t		to = STD_ADD(usize, tuple->data_at, end, 459, 63);

		std_range_check(from, to, tuple->bytes_len, RUST_FILE, 459, 39);
		result->tag = ENCODING_TUPLE_FIELD_VALUE;
		result->u.value.ptr = tuple->bytes + from;
		result->u.value.len = to - from;
	}
	return true;
}

/* encoding_tuple_Fields_Iterator_next is Fields' next field. */
bool
encoding_tuple_Fields_Iterator_next(EncodingTupleFields *self, EncodingTupleField *result)
{
	return encoding_tuple_Fields_step(self, result);
}

#ifdef DOLTGRES_TEST_ENCODING

/* encoding_tuple_tests_Array4Field_Debug_fmt is a [Field; 4]'s Debug: its elements as a list. */
static bool
encoding_tuple_tests_Array4Field_Debug_fmt(const void *value, StdFormatter *f)
{
	const EncodingTupleField *a = (const EncodingTupleField *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (int i = 0; i < 4; i++)
		std_DebugList_entry(&b, &a[i], encoding_tuple_Field_Debug_fmt);
	return std_DebugList_finish(&b);
}

/*
 * test_encoding_tuple_tests_fields_round_trip: a tuple's fields read back as
 * they were written, by position, walked, and walked from a position, and a
 * tuple cut short does not parse.
 */
void
test_encoding_tuple_tests_fields_round_trip(void)
{
	volatile StdVec fields;
	volatile bool fields_live = false;
	volatile StdVec bytes;
	volatile bool bytes_live = false;
	volatile StdVec empty;
	volatile bool empty_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 one[] = {'o', 'n', 'e'};
		static const uint8 four[] = {'f', 'o', 'u', 'r'};
		static const size_t positions[] = {3, 4, 8, 20};
		StdVec	   *fields_v = (StdVec *) &fields;
		StdVec	   *bytes_v = (StdVec *) &bytes;
		StdVec	   *empty_v = (StdVec *) &empty;
		StdBytes   *boxed;
		encoding_tuple_write_overflow_t none = {NULL, 0};
		EncodingTupleTuple tuple;
		EncodingTupleTuple other;
		EncodingTupleFields walk;
		EncodingTupleField left;
		EncodingTupleField right = {0};
		EncodingTupleField at_fields[4];
		EncodingTupleField by_field[4];
		size_t		len;
		size_t		expected_len = 9;
		size_t		count = 0;
		size_t		cut;
		StdBytes	unused;
		size_t		unused_at;
		bool		equal = true;

		boxed = std_box_new_uninit_layout_Checked(9 * sizeof(StdBytes), _Alignof(StdBytes));
		STD_CHECK_ALIGNED(boxed, _Alignof(StdBytes), 497, 13);
		STD_CHECK_NULL_DEREF(boxed, 497, 13);
		memset(boxed, 0, 9 * sizeof(StdBytes));
		boxed[0].ptr = one;
		boxed[0].len = sizeof(one);
		boxed[2].ptr = STD_DANGLING;
		boxed[3].ptr = four;
		boxed[3].len = sizeof(four);
		std_box_slice_into_vec_Checked(fields_v, boxed, 9, sizeof(StdBytes));
		fields_live = true;
		encoding_tuple_write(fields_v->ptr, fields_v->len, false, none, bytes_v);
		bytes_live = true;
		if (!encoding_tuple_Tuple_parse(bytes_v->ptr, bytes_v->len, &tuple))
			STD_OPTION_UNWRAP_FAILED(499, 42);
		len = encoding_tuple_Tuple_len(&tuple);
		STD_ASSERT_EQ(500, 9, len == expected_len, &len, std_usize_Debug_fmt, &expected_len, std_usize_Debug_fmt);
		for (size_t n = 0, n_len = fields_v->len; n < n_len; n++)
		{
			size_t		i = count;
			const StdBytes *f = &STD_VEC_AT(fields_v, StdBytes, n);

			count = std_Enumerate_count_step(count);
			left = encoding_tuple_Tuple_field(&tuple, i);
			if (f->ptr == NULL)
				right.tag = ENCODING_TUPLE_FIELD_NULL;
			else
			{
				right.tag = ENCODING_TUPLE_FIELD_VALUE;
				right.u.value = *f;
			}
			STD_ASSERT_EQ(502, 13, encoding_tuple_Field_PartialEq_eq(&left, &right), &left,
						  encoding_tuple_Field_Debug_fmt, &right, encoding_tuple_Field_Debug_fmt);
		}
		left = encoding_tuple_Tuple_field(&tuple, 20);
		right.tag = ENCODING_TUPLE_FIELD_NULL;
		STD_ASSERT_EQ(504, 9, encoding_tuple_Field_PartialEq_eq(&left, &right), &left, encoding_tuple_Field_Debug_fmt,
					  &right, encoding_tuple_Field_Debug_fmt);
		walk = encoding_tuple_Tuple_fields(&tuple);
		for (uint32 i = 0, i_end = 9;; i++)
		{
			EncodingTupleField x;

			if (!encoding_tuple_Fields_Iterator_next(&walk, &x))
			{
				if (i < i_end)
					(void) encoding_tuple_Tuple_field(&tuple, i);
				equal = i == i_end;
				break;
			}
			if (i == i_end)
			{
				equal = false;
				break;
			}
			right = encoding_tuple_Tuple_field(&tuple, i);
			if (!encoding_tuple_Field_PartialEq_eq(&x, &right))
			{
				equal = false;
				break;
			}
		}
		STD_ASSERT(505, 9, equal, "tuple.fields().eq((0..9).map(|i| tuple.field(i)))");
		walk = encoding_tuple_Tuple_fields(&tuple);
		for (int which = 0; which < 2; which++)
		{
			EncodingTupleField *mapped = which == 0 ? at_fields : by_field;
			volatile size_t drained = 0;
			volatile size_t initialized = 0;
			StdUnwind	map_unwind;

			STD_UNWIND_PUSH(&map_unwind);
			if (sigsetjmp(map_unwind.jump, 0) == 0)
			{
				while (initialized < 4)
				{
					size_t		i = positions[drained];
					EncodingTupleField item;

					drained = drained + 1;
					item = which == 0 ? encoding_tuple_Fields_at(&walk, i) : encoding_tuple_Tuple_field(&tuple, i);
					std_slice_get_unchecked_mut_check(initialized, 4, STD_ARRAY_MOD_FILE, 954, 28);
					mapped[initialized] = item;
					std_usize_unchecked_add_check(initialized, 1, STD_ARRAY_MOD_FILE, 955, 49);
					initialized = initialized + 1;
				}
				std_unwind_pop(&map_unwind);
				std_array_Drain_drop(&positions[drained], &positions[4], sizeof(positions[0]));
			}
			else
			{
				std_array_Guard_drop(initialized, 4);
				std_array_Drain_drop(&positions[drained], &positions[4], sizeof(positions[0]));
				std_unwind_resume(&map_unwind);
			}
		}
		equal = true;
		for (int k = 0; k < 4 && equal; k++)
			equal = encoding_tuple_Field_PartialEq_eq(&at_fields[k], &by_field[k]);
		STD_ASSERT_EQ(507, 9, equal, at_fields, encoding_tuple_tests_Array4Field_Debug_fmt, by_field,
					  encoding_tuple_tests_Array4Field_Debug_fmt);
		STD_ASSERT(508, 9, !encoding_tuple_Tuple_overflow(&tuple, &unused, &unused_at), "tuple.overflow().is_none()");
		cut = STD_SUB(usize, bytes_v->len, 1, 509, 39);
		std_range_to_check(cut, bytes_v->len, RUST_FILE, 509, 36);
		STD_ASSERT(509, 9, !encoding_tuple_Tuple_parse(bytes_v->ptr, cut, &other),
				   "Tuple::parse(&bytes[..bytes.len() - 1]).is_none()");
		encoding_tuple_write(STD_DANGLING, 0, false, none, empty_v);
		empty_live = true;
		if (!encoding_tuple_Tuple_parse(empty_v->ptr, empty_v->len, &other))
			STD_OPTION_UNWRAP_FAILED(511, 38);
		STD_ASSERT(511, 9, encoding_tuple_Tuple_is_empty(&other), "Tuple::parse(&empty).unwrap().is_empty()");
		empty_live = false;
		std_vec_free_Checked(empty_v, 1, 1);
		bytes_live = false;
		std_vec_free_Checked(bytes_v, 1, 1);
		fields_live = false;
		std_vec_free_Checked(fields_v, sizeof(StdBytes), _Alignof(StdBytes));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (empty_live)
			std_vec_free_Checked((StdVec *) &empty, 1, 1);
		if (bytes_live)
			std_vec_free_Checked((StdVec *) &bytes, 1, 1);
		if (fields_live)
			std_vec_free_Checked((StdVec *) &fields, sizeof(StdBytes), _Alignof(StdBytes));
		std_unwind_resume(&unwind);
	}
}

/*
 * test_encoding_tuple_tests_overflow_names_the_rest: a tuple with an overflow
 * holds its first fields inline and names the rest's blob, whose fields read
 * as their positions in it.
 */
void
test_encoding_tuple_tests_overflow_names_the_rest(void)
{
	uint8		address[ENCODING_TUPLE_ADDRESS_LEN];
	volatile StdVec bytes;
	volatile bool bytes_live = false;
	StdUnwind	unwind;

	memset(address, 7, sizeof(address));
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 a[] = {'a'};
		StdVec	   *bytes_v = (StdVec *) &bytes;
		StdBytes	fields[2] = {{a, 1}, {NULL, 0}};
		encoding_tuple_write_overflow_t overflow = {address, 1600};
		EncodingTupleTuple tuple;
		EncodingTupleFields walk;
		EncodingTupleField left;
		EncodingTupleField right = {0};
		size_t		len;
		size_t		expected_len = 1600;
		bool		equal = true;
		StdBytes	found;
		size_t		at;
		StdBytes	expected_found = {address, sizeof(address)};
		size_t		expected_at;

		encoding_tuple_write(fields, lengthof(fields), true, overflow, bytes_v);
		bytes_live = true;
		if (!encoding_tuple_Tuple_parse(bytes_v->ptr, bytes_v->len, &tuple))
			STD_OPTION_UNWRAP_FAILED(518, 42);
		len = encoding_tuple_Tuple_len(&tuple);
		STD_ASSERT_EQ(519, 9, len == expected_len, &len, std_usize_Debug_fmt, &expected_len, std_usize_Debug_fmt);
		left = encoding_tuple_Tuple_field(&tuple, 0);
		right.tag = ENCODING_TUPLE_FIELD_VALUE;
		right.u.value.ptr = a;
		right.u.value.len = 1;
		STD_ASSERT_EQ(520, 9, encoding_tuple_Field_PartialEq_eq(&left, &right), &left, encoding_tuple_Field_Debug_fmt,
					  &right, encoding_tuple_Field_Debug_fmt);
		left = encoding_tuple_Tuple_field(&tuple, 1);
		right.tag = ENCODING_TUPLE_FIELD_NULL;
		STD_ASSERT_EQ(521, 9, encoding_tuple_Field_PartialEq_eq(&left, &right), &left, encoding_tuple_Field_Debug_fmt,
					  &right, encoding_tuple_Field_Debug_fmt);
		left = encoding_tuple_Tuple_field(&tuple, 2);
		right.tag = ENCODING_TUPLE_FIELD_OVERFLOW;
		right.u.overflow = 0;
		STD_ASSERT_EQ(522, 9, encoding_tuple_Field_PartialEq_eq(&left, &right), &left, encoding_tuple_Field_Debug_fmt,
					  &right, encoding_tuple_Field_Debug_fmt);
		left = encoding_tuple_Tuple_field(&tuple, 1599);
		right.u.overflow = 1597;
		STD_ASSERT_EQ(523, 9, encoding_tuple_Field_PartialEq_eq(&left, &right), &left, encoding_tuple_Field_Debug_fmt,
					  &right, encoding_tuple_Field_Debug_fmt);
		walk = encoding_tuple_Tuple_fields(&tuple);
		for (uint32 i = 0, i_end = 1600;; i++)
		{
			EncodingTupleField x;

			if (!encoding_tuple_Fields_Iterator_next(&walk, &x))
			{
				if (i < i_end)
					(void) encoding_tuple_Tuple_field(&tuple, i);
				equal = i == i_end;
				break;
			}
			if (i == i_end)
			{
				equal = false;
				break;
			}
			right = encoding_tuple_Tuple_field(&tuple, i);
			if (!encoding_tuple_Field_PartialEq_eq(&x, &right))
			{
				equal = false;
				break;
			}
		}
		STD_ASSERT(524, 9, equal, "tuple.fields().eq((0..1600).map(|i| tuple.field(i)))");
		if (!encoding_tuple_Tuple_overflow(&tuple, &found, &at))
			STD_OPTION_UNWRAP_FAILED(525, 47);
		expected_at = STD_SUB(usize, bytes_v->len, ENCODING_TUPLE_ADDRESS_LEN, 526, 51);
		equal = found.len == expected_found.len && memcmp(found.ptr, expected_found.ptr, found.len) == 0 &&
			at == expected_at;
		STD_ASSERT_EQ(526, 9, equal,
					  &STD_TUPLE(2, STD_ITEM(&found, std_bytes_Debug_fmt), STD_ITEM(&at, std_usize_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&expected_found, std_bytes_Debug_fmt),
								 STD_ITEM(&expected_at, std_usize_Debug_fmt)),
					  std_tuple_Debug_fmt);
		bytes_live = false;
		std_vec_free_Checked(bytes_v, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (bytes_live)
			std_vec_free_Checked((StdVec *) &bytes, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/*
 * test_encoding_tuple_tests_external_fields_name_their_blobs: a field held
 * out of line reads as its blob's length and address, and where the address
 * is in the tuple.
 */
void
test_encoding_tuple_tests_external_fields_name_their_blobs(void)
{
	uint8		address[ENCODING_TUPLE_ADDRESS_LEN];
	volatile StdVec external;
	volatile bool external_live = false;
	volatile StdVec fields;
	volatile bool fields_live = false;
	volatile StdVec bytes;
	volatile bool bytes_live = false;
	volatile StdVec tmp_1;
	volatile bool tmp_1_live = false;
	StdUnwind	unwind;

	memset(address, 9, sizeof(address));
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 a[] = {'a'};
		static const bool marked[] = {false, true, false};
		StdVec	   *external_v = (StdVec *) &external;
		StdVec	   *fields_v = (StdVec *) &fields;
		StdVec	   *bytes_v = (StdVec *) &bytes;
		StdVec	   *tmp_1_v = (StdVec *) &tmp_1;
		StdBytes   *boxed;
		encoding_tuple_write_overflow_t none = {NULL, 0};
		EncodingTupleTuple tuple;
		EncodingTupleTuple other;
		EncodingTupleFields walk;
		EncodingTupleField left;
		EncodingTupleField right = {0};
		uint64		length;
		StdBytes	found;
		size_t		at;
		uint64		expected_length = 70000;
		StdBytes	expected_found = {address, sizeof(address)};
		StdBytes	stored;
		size_t		end;
		bool		equal = true;

		encoding_tuple_reference(70000, address, external_v);
		external_live = true;
		boxed = std_box_new_uninit_layout_Checked(3 * sizeof(StdBytes), _Alignof(StdBytes));
		STD_CHECK_ALIGNED(boxed, _Alignof(StdBytes), 533, 42);
		STD_CHECK_NULL_DEREF(boxed, 533, 42);
		boxed[0].ptr = a;
		boxed[0].len = 1;
		boxed[1].ptr = external_v->ptr;
		boxed[1].len = external_v->len;
		boxed[2].ptr = NULL;
		boxed[2].len = 0;
		std_box_slice_into_vec_Checked(fields_v, boxed, 3, sizeof(StdBytes));
		fields_live = true;
		encoding_tuple_write_with_external(fields_v->ptr, fields_v->len, marked, lengthof(marked), false, none,
										   bytes_v);
		bytes_live = true;
		if (!encoding_tuple_Tuple_parse(bytes_v->ptr, bytes_v->len, &tuple))
			STD_OPTION_UNWRAP_FAILED(535, 42);
		STD_ASSERT(536, 9, encoding_tuple_Tuple_has_external(&tuple), "tuple.has_external()");
		left = encoding_tuple_Tuple_field(&tuple, 0);
		right.tag = ENCODING_TUPLE_FIELD_VALUE;
		right.u.value.ptr = a;
		right.u.value.len = 1;
		STD_ASSERT_EQ(537, 9, encoding_tuple_Field_PartialEq_eq(&left, &right), &left, encoding_tuple_Field_Debug_fmt,
					  &right, encoding_tuple_Field_Debug_fmt);
		left = encoding_tuple_Tuple_field(&tuple, 1);
		if (left.tag != ENCODING_TUPLE_FIELD_EXTERNAL)
			STD_PANIC(538, 84, "not external");
		length = left.u.external.length;
		found = left.u.external.address;
		at = left.u.external.at;
		equal = length == expected_length && found.len == expected_found.len &&
			memcmp(found.ptr, expected_found.ptr, found.len) == 0;
		STD_ASSERT_EQ(539, 9, equal,
					  &STD_TUPLE(2, STD_ITEM(&length, std_u64_Debug_fmt), STD_ITEM(&found, std_bytes_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&expected_length, std_u64_Debug_fmt),
								 STD_ITEM(&expected_found, std_bytes_Debug_fmt)),
					  std_tuple_Debug_fmt);
		end = STD_ADD(usize, at, ENCODING_TUPLE_ADDRESS_LEN, 540, 31);
		std_range_check(at, end, bytes_v->len, RUST_FILE, 540, 26);
		stored.ptr = (const uint8 *) bytes_v->ptr + at;
		stored.len = end - at;
		equal = stored.len == expected_found.len && memcmp(stored.ptr, expected_found.ptr, stored.len) == 0;
		STD_ASSERT_EQ(540, 9, equal, &stored, std_bytes_Debug_fmt, &expected_found, std_bytes_Debug_fmt);
		left = encoding_tuple_Tuple_field(&tuple, 2);
		right.tag = ENCODING_TUPLE_FIELD_NULL;
		STD_ASSERT_EQ(541, 9, encoding_tuple_Field_PartialEq_eq(&left, &right), &left, encoding_tuple_Field_Debug_fmt,
					  &right, encoding_tuple_Field_Debug_fmt);
		walk = encoding_tuple_Tuple_fields(&tuple);
		for (uint32 i = 0, i_end = 3;; i++)
		{
			EncodingTupleField x;

			if (!encoding_tuple_Fields_Iterator_next(&walk, &x))
			{
				if (i < i_end)
					(void) encoding_tuple_Tuple_field(&tuple, i);
				equal = i == i_end;
				break;
			}
			if (i == i_end)
			{
				equal = false;
				break;
			}
			right = encoding_tuple_Tuple_field(&tuple, i);
			if (!encoding_tuple_Field_PartialEq_eq(&x, &right))
			{
				equal = false;
				break;
			}
		}
		STD_ASSERT(542, 9, equal, "tuple.fields().eq((0..3).map(|i| tuple.field(i)))");
		encoding_tuple_write(fields_v->ptr, fields_v->len, false, none, tmp_1_v);
		tmp_1_live = true;
		if (!encoding_tuple_Tuple_parse(tmp_1_v->ptr, tmp_1_v->len, &other))
			STD_OPTION_UNWRAP_FAILED(543, 54);
		STD_ASSERT(543, 9, !encoding_tuple_Tuple_has_external(&other),
				   "!Tuple::parse(&write(&fields, None)).unwrap().has_external()");
		tmp_1_live = false;
		std_vec_free_Checked(tmp_1_v, 1, 1);
		bytes_live = false;
		std_vec_free_Checked(bytes_v, 1, 1);
		fields_live = false;
		std_vec_free_Checked(fields_v, sizeof(StdBytes), _Alignof(StdBytes));
		external_live = false;
		std_vec_free_Checked(external_v, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_1_live)
			std_vec_free_Checked((StdVec *) &tmp_1, 1, 1);
		if (bytes_live)
			std_vec_free_Checked((StdVec *) &bytes, 1, 1);
		if (fields_live)
			std_vec_free_Checked((StdVec *) &fields, sizeof(StdBytes), _Alignof(StdBytes));
		if (external_live)
			std_vec_free_Checked((StdVec *) &external, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/*
 * test_encoding_tuple_tests_written_lengths_match_the_tuples: written_len
 * gives the length write and write_with_external give the same fields.
 */
void
test_encoding_tuple_tests_written_lengths_match_the_tuples(void)
{
	uint8		address[ENCODING_TUPLE_ADDRESS_LEN];
	volatile StdVec long_;
	volatile bool long_live = false;
	volatile StdVec fields;
	volatile bool fields_live = false;
	volatile StdVec lens;
	volatile bool lens_live = false;
	volatile StdVec tmp_1;
	volatile bool tmp_1_live = false;
	StdUnwind	unwind;

	memset(address, 3, sizeof(address));
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 xy[] = {'x', 'y'};
		StdVec	   *long_v = (StdVec *) &long_;
		StdVec	   *fields_v = (StdVec *) &fields;
		StdVec	   *lens_v = (StdVec *) &lens;
		StdVec	   *tmp_1_v = (StdVec *) &tmp_1;
		StdBytes   *boxed;
		StdSizeHint hint = {4, true, 4};
		bool		external[] = {false, false, false, true};
		encoding_tuple_write_overflow_t overflow;
		size_t		left;
		size_t		right;

		std_vec_u8_from_elem(long_v, 5, 300);
		long_live = true;
		boxed = std_box_new_uninit_layout_Checked(4 * sizeof(StdBytes), _Alignof(StdBytes));
		STD_CHECK_ALIGNED(boxed, _Alignof(StdBytes), 550, 42);
		STD_CHECK_NULL_DEREF(boxed, 550, 42);
		boxed[0].ptr = long_v->ptr;
		boxed[0].len = long_v->len;
		boxed[1].ptr = NULL;
		boxed[1].len = 0;
		boxed[2].ptr = xy;
		boxed[2].len = sizeof(xy);
		boxed[3].ptr = address;
		boxed[3].len = sizeof(address);
		std_box_slice_into_vec_Checked(fields_v, boxed, 4, sizeof(StdBytes));
		fields_live = true;
		std_vec_with_capacity_trusted(lens_v, hint, sizeof(StdUsizeOption), _Alignof(StdUsizeOption));
		lens_live = true;
		std_vec_extend_trusted_reserve(lens_v, hint, sizeof(StdUsizeOption), _Alignof(StdUsizeOption));
		for (size_t i = 0, i_len = fields_v->len; i < i_len; i++)
		{
			const StdBytes *f = &STD_VEC_AT(fields_v, StdBytes, i);
			StdUsizeOption len = {f->ptr != NULL, f->ptr != NULL ? f->len : 0};

			STD_VEC_AT(lens_v, StdUsizeOption, lens_v->len) = len;
			lens_v->len++;
		}
		left = encoding_tuple_written_len(lens_v->ptr, lens_v->len, false, false, 0);
		overflow.f0 = NULL;
		overflow.f1 = 0;
		encoding_tuple_write(fields_v->ptr, fields_v->len, false, overflow, tmp_1_v);
		tmp_1_live = true;
		right = tmp_1_v->len;
		STD_ASSERT_EQ(553, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
		tmp_1_live = false;
		std_vec_free_Checked(tmp_1_v, 1, 1);
		left = encoding_tuple_written_len(lens_v->ptr, lens_v->len, true, false, 0);
		encoding_tuple_write_with_external(fields_v->ptr, fields_v->len, external, lengthof(external), false,
										   overflow, tmp_1_v);
		tmp_1_live = true;
		right = tmp_1_v->len;
		STD_ASSERT_EQ(554, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
		tmp_1_live = false;
		std_vec_free_Checked(tmp_1_v, 1, 1);
		overflow.f0 = address;
		overflow.f1 = 40;
		left = encoding_tuple_written_len(lens_v->ptr, lens_v->len, true, true, 40);
		encoding_tuple_write_with_external(fields_v->ptr, fields_v->len, external, lengthof(external), true,
										   overflow, tmp_1_v);
		tmp_1_live = true;
		right = tmp_1_v->len;
		STD_ASSERT_EQ(556, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
		tmp_1_live = false;
		std_vec_free_Checked(tmp_1_v, 1, 1);
		left = encoding_tuple_written_len(STD_DANGLING, 0, false, true, 9);
		overflow.f1 = 9;
		encoding_tuple_write(STD_DANGLING, 0, true, overflow, tmp_1_v);
		tmp_1_live = true;
		right = tmp_1_v->len;
		STD_ASSERT_EQ(557, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
		tmp_1_live = false;
		std_vec_free_Checked(tmp_1_v, 1, 1);
		lens_live = false;
		std_vec_free_Checked(lens_v, sizeof(StdUsizeOption), _Alignof(StdUsizeOption));
		fields_live = false;
		std_vec_free_Checked(fields_v, sizeof(StdBytes), _Alignof(StdBytes));
		long_live = false;
		std_vec_free_Checked(long_v, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_1_live)
			std_vec_free_Checked((StdVec *) &tmp_1, 1, 1);
		if (lens_live)
			std_vec_free_Checked((StdVec *) &lens, sizeof(StdUsizeOption), _Alignof(StdUsizeOption));
		if (fields_live)
			std_vec_free_Checked((StdVec *) &fields, sizeof(StdBytes), _Alignof(StdBytes));
		if (long_live)
			std_vec_free_Checked((StdVec *) &long_, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/*
 * test_encoding_tuple_tests_malformed_ends_are_refused: a tuple whose field
 * ends decrease, or that ends before its bitmap, does not parse.
 */
void
test_encoding_tuple_tests_malformed_ends_are_refused(void)
{
	static const uint8 decreasing[] = {2 << 2, 0, 3, 0, 1, 0, 'x'};
	static const uint8 short_[] = {4 << 2, 0, 0, 0, 0};
	EncodingTupleTuple tuple;

	STD_ASSERT(562, 9, !encoding_tuple_Tuple_parse(decreasing, sizeof(decreasing), &tuple),
			   "Tuple::parse(&[2 << 2, 0, 3, 0, 1, 0, b'x']).is_none()");
	STD_ASSERT(563, 9, !encoding_tuple_Tuple_parse(short_, sizeof(short_), &tuple),
			   "Tuple::parse(&[4 << 2, 0, 0, 0, 0]).is_none()");
}

#endif							/* DOLTGRES_TEST_ENCODING */
