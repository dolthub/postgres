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
 * The header every chunk's hashed bytes start with: its kind, flags, and a
 * table of where the addresses it refers to are, so that garbage collection,
 * pushes, and integrity checks can follow any chunk's references without
 * knowing its kind.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/header.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/record.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "header.rs"

/* CHUNKS_HEADER_STRIDE is the flag of an address table written as a first offset and a stride. */
#define CHUNKS_HEADER_STRIDE 1

/* chunks_header_VecU32_Debug_fmt is a Vec<u32>'s Debug: its elements as a list. */
bool
chunks_header_VecU32_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = (const StdVec *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
		std_DebugList_entry(&b, &STD_VEC_AT(v, uint32, i), std_u32_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* chunks_header_Addresses_Debug_fmt is Addresses' derived Debug. */
bool
chunks_header_Addresses_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksHeaderAddresses *self = (const ChunksHeaderAddresses *) value;
	StdDebugTuple t;
	StdDebugStruct s;

	switch (self->tag)
	{
		case CHUNKS_HEADER_ADDRESSES_OFFSETS:
			t = std_fmt_debug_tuple(f, "Offsets", 7);
			std_DebugTuple_field(&t, &self->u.offsets, chunks_header_VecU32_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case CHUNKS_HEADER_ADDRESSES_STRIDE:
			s = std_fmt_debug_struct(f, "Stride", 6);
			std_DebugStruct_field(&s, "first", 5, &self->u.stride.first, std_u32_Debug_fmt);
			std_DebugStruct_field(&s, "stride", 6, &self->u.stride.stride, std_u32_Debug_fmt);
			std_DebugStruct_field(&s, "count", 5, &self->u.stride.count, std_u32_Debug_fmt);
			return std_DebugStruct_finish(&s);
	}
	pg_unreachable();
}

/* chunks_header_Addresses_PartialEq_eq is Addresses' derived PartialEq. */
bool
chunks_header_Addresses_PartialEq_eq(const ChunksHeaderAddresses *self, const ChunksHeaderAddresses *other)
{
	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case CHUNKS_HEADER_ADDRESSES_OFFSETS:
			return self->u.offsets.len == other->u.offsets.len &&
				memcmp(self->u.offsets.ptr, other->u.offsets.ptr, self->u.offsets.len * sizeof(uint32)) == 0;
		case CHUNKS_HEADER_ADDRESSES_STRIDE:
			return self->u.stride.first == other->u.stride.first &&
				self->u.stride.stride == other->u.stride.stride && self->u.stride.count == other->u.stride.count;
	}
	pg_unreachable();
}

/* chunks_header_Addresses_drop is Addresses' drop glue: an Offsets' vector's free. */
void
chunks_header_Addresses_drop(ChunksHeaderAddresses *self)
{
	if (self->tag == CHUNKS_HEADER_ADDRESSES_OFFSETS)
		std_vec_free_Checked(&self->u.offsets, sizeof(uint32), _Alignof(uint32));
}

/* chunks_header_Addresses_len returns the number of addresses. */
size_t
chunks_header_Addresses_len(const ChunksHeaderAddresses *self)
{
	switch (self->tag)
	{
		case CHUNKS_HEADER_ADDRESSES_OFFSETS:
			return self->u.offsets.len;
		case CHUNKS_HEADER_ADDRESSES_STRIDE:
			return (size_t) self->u.stride.count;
	}
	pg_unreachable();
}

/* chunks_header_Addresses_is_empty reports whether there are no addresses. */
bool
chunks_header_Addresses_is_empty(const ChunksHeaderAddresses *self)
{
	return chunks_header_Addresses_len(self) == 0;
}

/* chunks_header_Addresses_offset returns the offset in the body of the address at the position. */
size_t
chunks_header_Addresses_offset(const ChunksHeaderAddresses *self, size_t i)
{
	switch (self->tag)
	{
		case CHUNKS_HEADER_ADDRESSES_OFFSETS:
			std_index_check(i, self->u.offsets.len, RUST_FILE, 67, 51);
			return (size_t) STD_VEC_AT(&self->u.offsets, uint32, i);
		case CHUNKS_HEADER_ADDRESSES_STRIDE:
			{
				size_t		tmp_1 = STD_MUL(usize, i, (size_t) self->u.stride.stride, 68, 74);

				return STD_ADD(usize, (size_t) self->u.stride.first, tmp_1, 68, 56);
			}
	}
	pg_unreachable();
}

/* chunks_header_Header_Debug_fmt is Header's derived Debug. */
bool
chunks_header_Header_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksHeaderHeader *self = (const ChunksHeaderHeader *) value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Header", 6);

	std_DebugStruct_field(&s, "kind", 4, &self->kind, std_u8_Debug_fmt);
	std_DebugStruct_field(&s, "flags", 5, &self->flags, std_u8_Debug_fmt);
	std_DebugStruct_field(&s, "addresses", 9, &self->addresses, chunks_header_Addresses_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_header_Header_PartialEq_eq is Header's derived PartialEq. */
bool
chunks_header_Header_PartialEq_eq(const ChunksHeaderHeader *self, const ChunksHeaderHeader *other)
{
	return self->kind == other->kind && self->flags == other->flags &&
		chunks_header_Addresses_PartialEq_eq(&self->addresses, &other->addresses);
}

/* chunks_header_Header_drop is Header's drop glue: its addresses'. */
void
chunks_header_Header_drop(ChunksHeaderHeader *self)
{
	chunks_header_Addresses_drop(&self->addresses);
}

/* chunks_header_Header_write appends the header followed by the body, which is the chunk's hashed bytes. */
void
chunks_header_Header_write(const ChunksHeaderHeader *self, const uint8 *body, size_t body_len, StdVec *result)
{
	volatile StdVec out;
	volatile bool out_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *out_v = (StdVec *) &out;

		std_vec_with_capacity_Checked(out_v, STD_ADD(usize, body_len, 16, 85, 42), 1, 1);
		out_live = true;
		std_vec_u8_push_Checked(out_v, self->kind);
		std_vec_u8_push_Checked(out_v, self->flags);
		switch (self->addresses.tag)
		{
			case CHUNKS_HEADER_ADDRESSES_OFFSETS:
				{
					const StdVec *offsets = &self->addresses.u.offsets;

					std_vec_u8_push_Checked(out_v, 0);
					chunks_varint_put_varint(out_v, (uint64) offsets->len);
					for (size_t i = 0, i_len = offsets->len; i < i_len; i++)
						chunks_varint_put_varint(out_v, (uint64) STD_VEC_AT(offsets, uint32, i));
				}
				break;
			case CHUNKS_HEADER_ADDRESSES_STRIDE:
				std_vec_u8_push_Checked(out_v, CHUNKS_HEADER_STRIDE);
				chunks_varint_put_varint(out_v, (uint64) self->addresses.u.stride.count);
				chunks_varint_put_varint(out_v, (uint64) self->addresses.u.stride.first);
				chunks_varint_put_varint(out_v, (uint64) self->addresses.u.stride.stride);
				break;
		}
		std_vec_extend_from_slice_Checked(out_v, body, body_len, 1, 1);
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

static bool chunks_header_read_u32(const uint8 *bytes, size_t bytes_len, size_t *at, uint32 *result,
								   ChunksErrorError *error);

/*
 * chunks_header_Header_read reads the header of a chunk's hashed bytes and
 * returns it with the offset of the body.
 */
bool
chunks_header_Header_read(const uint8 *bytes, size_t bytes_len, ChunksHeaderHeader *result_0, size_t *result_1,
						  ChunksErrorError *error)
{
	uint8		kind;
	uint8		flags;
	uint8		form;
	size_t		at = 3;
	uint32		count;
	volatile StdVec offsets;
	volatile bool offsets_live = false;
	volatile ChunksHeaderHeader header;
	volatile bool header_live = false;
	StdUnwind	unwind;

	if (bytes_len < 3)
	{
		StdString	message;

		std_string_from_Checked(&message, "chunk is shorter than its header", 32);
		*error = chunks_error_Error_corrupt(&message);
		return false;
	}
	std_index_check(0, bytes_len, RUST_FILE, 112, 36);
	kind = bytes[0];
	std_index_check(1, bytes_len, RUST_FILE, 112, 46);
	flags = bytes[1];
	std_index_check(2, bytes_len, RUST_FILE, 112, 56);
	form = bytes[2];
	if (!chunks_header_read_u32(bytes, bytes_len, &at, &count, error))
		return false;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *offsets_v = (StdVec *) &offsets;
		ChunksHeaderHeader *header_v = (ChunksHeaderHeader *) &header;
		size_t		body;

		header_v->kind = kind;
		header_v->flags = flags;
		switch (form)
		{
			case 0:
				std_vec_with_capacity_Checked(offsets_v, (size_t) std_min_u32(count, 4096), sizeof(uint32),
											  _Alignof(uint32));
				offsets_live = true;
				for (uint32 n = 0, n_end = count; n < n_end; n++)
				{
					uint32		offset;

					if (!chunks_header_read_u32(bytes, bytes_len, &at, &offset, error))
					{
						offsets_live = false;
						std_vec_free_Checked(offsets_v, sizeof(uint32), _Alignof(uint32));
						std_unwind_pop(&unwind);
						return false;
					}
					std_vec_push_Checked(offsets_v, &offset, sizeof(uint32), _Alignof(uint32));
				}
				header_v->addresses.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS;
				header_v->addresses.u.offsets = *offsets_v;
				offsets_live = false;
				break;
			case CHUNKS_HEADER_STRIDE:
				{
					uint32		first;
					uint32		stride;

					if (!chunks_header_read_u32(bytes, bytes_len, &at, &first, error))
					{
						std_unwind_pop(&unwind);
						return false;
					}
					if (!chunks_header_read_u32(bytes, bytes_len, &at, &stride, error))
					{
						std_unwind_pop(&unwind);
						return false;
					}
					header_v->addresses.tag = CHUNKS_HEADER_ADDRESSES_STRIDE;
					header_v->addresses.u.stride.first = first;
					header_v->addresses.u.stride.stride = stride;
					header_v->addresses.u.stride.count = count;
				}
				break;
			default:
				{
					StdString	message;
					StdFmtPiece pieces[] = {STD_FMT_LIT("unknown address table form "), STD_FMT_ARG(0)};
					StdFmtArg	args[] = {{&form, std_u8_Display_fmt}};
					StdFmtArguments arguments = {pieces, 2, args};

					std_fmt_format_Checked(&message, &arguments);
					*error = chunks_error_Error_corrupt(&message);
					std_unwind_pop(&unwind);
					return false;
				}
		}
		header_live = true;
		body = STD_SUB(usize, bytes_len, at, 131, 20);
		if (chunks_header_Addresses_len(&header_v->addresses) > STD_DIV_FOLDED(usize, body, 32))
		{
			StdString	message;

			std_string_from_Checked(&message, "chunk lists more addresses than it has room for", 47);
			*error = chunks_error_Error_corrupt(&message);
			header_live = false;
			chunks_header_Header_drop(header_v);
			std_unwind_pop(&unwind);
			return false;
		}
		for (size_t i = 0, i_end = chunks_header_Addresses_len(&header_v->addresses); i < i_end; i++)
		{
			size_t		end = STD_ADD(usize, chunks_header_Addresses_offset(&header_v->addresses, i), 32,
									  135, 48);

			if (end > body)
			{
				StdString	message;

				std_string_from_Checked(&message, "chunk address lies past the end of the chunk", 44);
				*error = chunks_error_Error_corrupt(&message);
				header_live = false;
				chunks_header_Header_drop(header_v);
				std_unwind_pop(&unwind);
				return false;
			}
		}
		*result_0 = *header_v;
		header_live = false;
		*result_1 = at;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (header_live)
			chunks_header_Header_drop((ChunksHeaderHeader *) &header);
		if (offsets_live)
			std_vec_free_Checked((StdVec *) &offsets, sizeof(uint32), _Alignof(uint32));
		std_unwind_resume(&unwind);
	}
	return true;
}

/* chunks_header_read_u32 reads a varint that must fit 32 bits. */
static bool
chunks_header_read_u32(const uint8 *bytes, size_t bytes_len, size_t *at, uint32 *result, ChunksErrorError *error)
{
	uint64		value;

	if (!chunks_varint_read_varint(bytes, bytes_len, at, &value, error))
		return false;
	if (value > PG_UINT32_MAX)
	{
		StdString	message;

		std_string_from_Checked(&message, "chunk header value is too large", 31);
		*error = chunks_error_Error_corrupt(&message);
		return false;
	}
	*result = (uint32) value;
	return true;
}

/*
 * chunks_header_addresses returns every address a chunk's hashed bytes refer
 * to, in the order of its address table.
 */
bool
chunks_header_addresses(const uint8 *bytes, size_t bytes_len, StdVec *result, ChunksErrorError *error)
{
	volatile ChunksHeaderHeader header;
	volatile bool header_live = false;
	volatile StdVec out;
	volatile bool out_live = false;
	size_t		body;
	StdUnwind	unwind;

	if (!chunks_header_Header_read(bytes, bytes_len, (ChunksHeaderHeader *) &header, &body, error))
		return false;
	header_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		ChunksHeaderHeader *header_v = (ChunksHeaderHeader *) &header;
		StdVec	   *out_v = (StdVec *) &out;
		size_t		len = chunks_header_Addresses_len(&header_v->addresses);
		StdSizeHint hint = {len, true, len};

		std_vec_with_capacity_trusted(out_v, hint, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
		out_live = true;
		std_vec_extend_trusted_reserve(out_v, hint, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
		for (size_t i = 0, i_end = len; i < i_end; i++)
		{
			size_t		at;
			size_t		end;

			at = STD_ADD(usize, body, chunks_header_Addresses_offset(&header_v->addresses, i), 152, 22);
			end = STD_ADD(usize, at, 32, 153, 31);
			std_range_check(at, end, bytes_len, RUST_FILE, 153, 26);
			if (end - at != 32)
				std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
										 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
										 std_TryFromSliceError_Debug_fmt, RUST_FILE, 153, 51);
			memcpy(STD_VEC_AT(out_v, ChunksAddressAddress, out_v->len).f0, bytes + at, 32);
			out_v->len++;
		}
		*result = *out_v;
		out_live = false;
		header_live = false;
		chunks_header_Header_drop(header_v);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (out_live)
			std_vec_free_Checked((StdVec *) &out, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
		if (header_live)
			chunks_header_Header_drop((ChunksHeaderHeader *) &header);
		std_unwind_resume(&unwind);
	}
	return true;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_header_tests_addresses returns the addresses a chunk's header names, unwrapped as the test's unwrap is. */
static StdVec
chunks_header_tests_addresses(const StdVec *bytes, uint32 line)
{
	StdVec		found;
	ChunksErrorError error;

	if (!chunks_header_addresses(bytes->ptr, bytes->len, &found, &error))
		STD_RESULT_EXPECT_FAILED(line, 9, "called `Result::unwrap()` on an `Err` value", &error,
								 chunks_error_Error_Debug_fmt);
	return found;
}

/* chunks_header_tests_offsets returns an Addresses::Offsets of offsets. */
static ChunksHeaderAddresses
chunks_header_tests_offsets(const uint32 *offsets, size_t len)
{
	ChunksHeaderAddresses addresses = {.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS};

	std_slice_to_vec_Checked(&addresses.u.offsets, offsets, len, sizeof(uint32), _Alignof(uint32));
	return addresses;
}

/*
 * test_chunks_header_tests_finds_addresses_of_either_form finds two
 * addresses in a body through a stride header and through an offsets
 * header, which reads back as written, and refuses an offset past the body.
 */
void
test_chunks_header_tests_finds_addresses_of_either_form(void)
{
	static const uint32 offsets_of[] = {37, 5};
	static const uint32 past_of[] = {38};
	ChunksAddressAddress a = chunks_address_Address_of((const uint8 *) "a", 1);
	ChunksAddressAddress b = chunks_address_Address_of((const uint8 *) "b", 1);
	ChunksAddressAddress ab[2] = {a, b};
	ChunksAddressAddress ba[2] = {b, a};
	StdVec		body;
	StdVec		bytes;
	StdVec		found;
	StdVec		want;
	ChunksHeaderHeader stride = {CHUNKS_HEADER_TREE_INTERNAL, 0, {.tag = CHUNKS_HEADER_ADDRESSES_STRIDE}};
	ChunksHeaderHeader offsets = {CHUNKS_HEADER_FIRST_RECORD, 3};
	ChunksHeaderHeader past = {CHUNKS_HEADER_TREE_LEAF, 0};
	ChunksHeaderHeader read;
	size_t		read_len;
	ChunksErrorError error;

	std_vec_u8_from_elem(&body, 9, 5);
	std_vec_extend_from_slice_Checked(&body, a.f0, sizeof(a.f0), 1, 1);
	std_vec_extend_from_slice_Checked(&body, b.f0, sizeof(b.f0), 1, 1);
	stride.addresses.u.stride.first = 5;
	stride.addresses.u.stride.stride = 32;
	stride.addresses.u.stride.count = 2;
	chunks_header_Header_write(&stride, body.ptr, body.len, &bytes);
	found = chunks_header_tests_addresses(&bytes, 170);
	std_slice_to_vec_Checked(&want, ab, 2, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	STD_ASSERT_EQ(170, 9, chunks_record_tests_VecAddress_PartialEq_eq(&found, &want), &found,
				  chunks_record_tests_VecAddress_Debug_fmt, &want, chunks_record_tests_VecAddress_Debug_fmt);
	std_vec_free_Checked(&found, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	std_vec_free_Checked(&want, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	std_vec_free_Checked(&bytes, 1, 1);

	offsets.addresses = chunks_header_tests_offsets(offsets_of, lengthof(offsets_of));
	chunks_header_Header_write(&offsets, body.ptr, body.len, &bytes);
	found = chunks_header_tests_addresses(&bytes, 173);
	std_slice_to_vec_Checked(&want, ba, 2, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	STD_ASSERT_EQ(173, 9, chunks_record_tests_VecAddress_PartialEq_eq(&found, &want), &found,
				  chunks_record_tests_VecAddress_Debug_fmt, &want, chunks_record_tests_VecAddress_Debug_fmt);
	std_vec_free_Checked(&found, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	std_vec_free_Checked(&want, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (!chunks_header_Header_read(bytes.ptr, bytes.len, &read, &read_len, &error))
		STD_RESULT_EXPECT_FAILED(174, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 chunks_error_Error_Debug_fmt);
	STD_ASSERT_EQ(174, 9, chunks_header_Header_PartialEq_eq(&read, &offsets), &read, chunks_header_Header_Debug_fmt,
				  &offsets, chunks_header_Header_Debug_fmt);
	chunks_header_Header_drop(&read);
	chunks_header_Header_drop(&offsets);
	std_vec_free_Checked(&bytes, 1, 1);

	past.addresses = chunks_header_tests_offsets(past_of, lengthof(past_of));
	chunks_header_Header_write(&past, body.ptr, body.len, &bytes);
	if (chunks_header_addresses(bytes.ptr, bytes.len, &found, &error))
		std_panic(RUST_FILE, 176, 9, "assertion failed: addresses(&past.write(&body)).is_err()",
				  sizeof("assertion failed: addresses(&past.write(&body)).is_err()") - 1);
	chunks_error_Error_drop(&error);
	chunks_header_Header_drop(&past);
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&body, 1, 1);
}

/* chunks_header_tests_reads is whether a stride header over a body reads, its read dropped. */
static bool
chunks_header_tests_reads(uint32 stride_of, uint32 count)
{
	static const uint8 body[64] = {0};
	ChunksHeaderHeader stride = {CHUNKS_HEADER_TREE_INTERNAL, 0, {.tag = CHUNKS_HEADER_ADDRESSES_STRIDE}};
	ChunksHeaderHeader read;
	size_t		read_len;
	ChunksErrorError error;
	StdVec		bytes;
	bool		ok;

	stride.addresses.u.stride.stride = stride_of;
	stride.addresses.u.stride.count = count;
	chunks_header_Header_write(&stride, body, sizeof(body), &bytes);
	ok = chunks_header_Header_read(bytes.ptr, bytes.len, &read, &read_len, &error);
	if (ok)
		chunks_header_Header_drop(&read);
	else
		chunks_error_Error_drop(&error);
	std_vec_free_Checked(&bytes, 1, 1);
	return ok;
}

/*
 * test_chunks_header_tests_refuses_more_addresses_than_the_body_holds
 * refuses a stride header naming more addresses than its body holds, and
 * reads one whose addresses fit.
 */
void
test_chunks_header_tests_refuses_more_addresses_than_the_body_holds(void)
{
	STD_ASSERT(187, 9, !chunks_header_tests_reads(0, PG_UINT32_MAX),
			   "Header::read(&stride(0, u32::MAX).write(&body)).is_err()");
	STD_ASSERT(188, 9, chunks_header_tests_reads(32, 2), "Header::read(&stride(32, 2).write(&body)).is_ok()");
}

#endif							/* DOLTGRES_TEST_CHUNKS */
