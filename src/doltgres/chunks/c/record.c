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
 * Records, the form of every object that is not a tree node: fields in
 * increasing number order, each written as its number, its length, and its
 * bytes.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/record.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "record.rs"

/* CHUNKS_RECORD_ENTRY is the size and alignment of an (Address, Locator), a record's address with its locator. */
#define CHUNKS_RECORD_ENTRY sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t)

/* CHUNKS_RECORD_PLACED is the size and alignment of a (u32, Locator), an address's offset with its locator. */
#define CHUNKS_RECORD_PLACED sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t)

/* chunks_record_RecordWriter_Default_default is RecordWriter's derived Default: an empty record. */
ChunksRecordRecordWriter
chunks_record_RecordWriter_Default_default(void)
{
	ChunksRecordRecordWriter writer = {.out = STD_VEC_NEW, .addresses = STD_VEC_NEW};

	return writer;
}

/* chunks_record_RecordWriter_new returns a writer of an empty record. */
ChunksRecordRecordWriter
chunks_record_RecordWriter_new(void)
{
	return chunks_record_RecordWriter_Default_default();
}

/* chunks_record_RecordWriter_with_capacity returns a writer of an empty record with room for about that many bytes. */
ChunksRecordRecordWriter
chunks_record_RecordWriter_with_capacity(size_t bytes)
{
	StdVec		out;
	ChunksRecordRecordWriter writer;

	std_vec_with_capacity_Checked(&out, bytes, 1, 1);
	writer = chunks_record_RecordWriter_Default_default();
	writer.out = out;
	return writer;
}

static void chunks_record_RecordWriter_head(ChunksRecordRecordWriter *self, uint32 number, size_t len);

/* chunks_record_RecordWriter_field appends a field, which must come after every field written so far. */
ChunksRecordRecordWriter *
chunks_record_RecordWriter_field(ChunksRecordRecordWriter *self, uint32 number, const uint8 *bytes, size_t bytes_len)
{
	chunks_record_RecordWriter_head(self, number, bytes_len);
	std_vec_extend_from_slice_Checked(&self->out, bytes, bytes_len, 1, 1);
	return self;
}

/* chunks_record_RecordWriter_uint appends a field holding an unsigned integer as a varint. */
ChunksRecordRecordWriter *
chunks_record_RecordWriter_uint(ChunksRecordRecordWriter *self, uint32 number, uint64 value)
{
	chunks_record_RecordWriter_head(self, number, chunks_varint_varint_len(value));
	chunks_varint_put_varint(&self->out, value);
	return self;
}

/*
 * chunks_record_RecordWriter_head appends the number and length of a field,
 * which must come after every field written so far.
 */
static void
chunks_record_RecordWriter_head(ChunksRecordRecordWriter *self, uint32 number, size_t len)
{
	static const char text[] = "record fields must be written in increasing order";

	if (!(!self->has_last || self->last < number))
		std_panic(RUST_FILE, 67, 9, text, sizeof(text) - 1);
	self->has_last = true;
	self->last = number;
	chunks_varint_put_varint(&self->out, (uint64) number);
	chunks_varint_put_varint(&self->out, (uint64) len);
}

/* chunks_record_RecordWriter_int appends a field holding a signed integer as a zigzag varint. */
ChunksRecordRecordWriter *
chunks_record_RecordWriter_int(ChunksRecordRecordWriter *self, uint32 number, int64 value)
{
	return chunks_record_RecordWriter_uint(self, number, chunks_record_zigzag(value));
}

/*
 * chunks_record_RecordWriter_addresses_frame_0_t is addresses' state across
 * its pad: its arguments, and the addresses' bytes it holds.
 */
typedef struct chunks_record_RecordWriter_addresses_frame_0_t
{
	ChunksRecordRecordWriter *self;
	uint32		number;
	const chunks_TupleAddressLocator_t *addresses;
	size_t		addresses_len;
	StdVec		bytes;
	bool		bytes_live;
} chunks_record_RecordWriter_addresses_frame_0_t;

/* chunks_record_RecordWriter_addresses_body_0 is addresses' body. */
static void
chunks_record_RecordWriter_addresses_body_0(void *frame_)
{
	chunks_record_RecordWriter_addresses_frame_0_t *frame = frame_;
	StdSizeHint iter = {frame->addresses_len, true, frame->addresses_len};
	StdSizeHint hint = std_FlattenCompat_size_hint(iter, 32);
	size_t		start;
	size_t		i = 0;

	std_vec_with_capacity_trusted(&frame->bytes, hint, 1, 1);
	frame->bytes_live = true;
	std_vec_extend_trusted_reserve(&frame->bytes, hint, 1, 1);
	for (size_t k = 0, k_len = frame->addresses_len; k < k_len; k++)
	{
		const chunks_TupleAddressLocator_t *a = &frame->addresses[k];

		memcpy(STD_PTR_AT(frame->bytes.ptr, frame->bytes.len, 1), a->f0.f0, 32);
		frame->bytes.len = std_wrapping_add_usize(frame->bytes.len, 32);
	}
	chunks_record_RecordWriter_field(frame->self, frame->number, frame->bytes.ptr, frame->bytes.len);
	start = STD_SUB(usize, frame->self->out.len, frame->bytes.len, 83, 21);
	for (size_t k = 0, k_len = frame->addresses_len; k < k_len; k++)
	{
		const chunks_TupleAddressLocator_t *a = &frame->addresses[k];
		chunks_TupleU32Locator_t placed;
		size_t		at = i;
		size_t		offset;

		i = std_Enumerate_count_step(i);
		offset = STD_MUL(usize, 32, at, 85, 43);
		placed.f0 = (uint32) STD_ADD(usize, start, offset, 85, 35);
		placed.f1 = a->f1;
		std_vec_push_Checked(&frame->self->addresses, &placed, CHUNKS_RECORD_PLACED);
	}
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
}

/* chunks_record_RecordWriter_addresses_pad_0 is addresses' pad: the addresses' bytes, while it holds them. */
static void
chunks_record_RecordWriter_addresses_pad_0(void *frame_)
{
	chunks_record_RecordWriter_addresses_frame_0_t *frame = frame_;

	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
}

/*
 * chunks_record_RecordWriter_addresses appends a field holding addresses back
 * to back, which the record's chunk lists in its address table with their
 * locators in its trailer.
 */
ChunksRecordRecordWriter *
chunks_record_RecordWriter_addresses(ChunksRecordRecordWriter *self, uint32 number,
									 const chunks_TupleAddressLocator_t *addresses, size_t addresses_len)
{
	chunks_record_RecordWriter_addresses_frame_0_t frame = {.self = self, .number = number, .addresses = addresses,
	.addresses_len = addresses_len};

	std_landing(&frame, chunks_record_RecordWriter_addresses_body_0, chunks_record_RecordWriter_addresses_pad_0);
	return self;
}

/*
 * chunks_record_RecordWriter_record_frame_0_t is record's state across its
 * pad: its arguments, the inner writer it takes, and the inner record's
 * bytes and addresses it holds.
 */
typedef struct chunks_record_RecordWriter_record_frame_0_t
{
	ChunksRecordRecordWriter *self;
	uint32		number;
	ChunksRecordRecordWriter inner;
	bool		inner_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		addresses;
	bool		addresses_live;
} chunks_record_RecordWriter_record_frame_0_t;

/* chunks_record_RecordWriter_record_body_0 is record's body. */
static void
chunks_record_RecordWriter_record_body_0(void *frame_)
{
	chunks_record_RecordWriter_record_frame_0_t *frame = frame_;
	uint32		start;
	StdSizeHint hint;

	chunks_record_RecordWriter_finish_value(&frame->inner, &frame->bytes, &frame->addresses);
	frame->bytes_live = true;
	frame->addresses_live = true;
	chunks_record_RecordWriter_field(frame->self, frame->number, frame->bytes.ptr, frame->bytes.len);
	start = (uint32) STD_SUB(usize, frame->self->out.len, frame->bytes.len, 94, 22);
	hint.lower = frame->addresses.len;
	hint.has_upper = true;
	hint.upper = frame->addresses.len;
	std_vec_extend_trusted_reserve(&frame->self->addresses, hint, CHUNKS_RECORD_PLACED);
	for (size_t i = 0, i_len = frame->addresses.len; i < i_len; i++)
	{
		const chunks_TupleU32Locator_t *o = &STD_VEC_AT(&frame->addresses, chunks_TupleU32Locator_t, i);
		chunks_TupleU32Locator_t placed;

		placed.f0 = STD_ADD(u32, start, o->f0, 95, 67);
		placed.f1 = o->f1;
		memcpy(STD_PTR_AT(frame->self->addresses.ptr, frame->self->addresses.len, sizeof(placed)), &placed, sizeof(placed));
		frame->self->addresses.len = std_wrapping_add_usize(frame->self->addresses.len, 1);
	}
	frame->addresses_live = false;
	std_vec_free_Checked(&frame->addresses, CHUNKS_RECORD_PLACED);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	frame->inner_live = false;
	chunks_record_RecordWriter_drop(&frame->inner);
}

/*
 * chunks_record_RecordWriter_record_pad_0 is record's pad: the inner
 * record's bytes and addresses, and the inner writer, while it holds them.
 */
static void
chunks_record_RecordWriter_record_pad_0(void *frame_)
{
	chunks_record_RecordWriter_record_frame_0_t *frame = frame_;

	if (frame->addresses_live)
		std_vec_free_Checked(&frame->addresses, CHUNKS_RECORD_PLACED);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->inner_live)
		chunks_record_RecordWriter_drop(&frame->inner);
}

/*
 * chunks_record_RecordWriter_record appends a field holding another record,
 * taking its writer, whose addresses become the outer record's.
 */
ChunksRecordRecordWriter *
chunks_record_RecordWriter_record(ChunksRecordRecordWriter *self, uint32 number, ChunksRecordRecordWriter *inner)
{
	chunks_record_RecordWriter_record_frame_0_t frame = {.self = self, .number = number, .inner = *inner,
	.inner_live = true};

	std_landing(&frame, chunks_record_RecordWriter_record_body_0, chunks_record_RecordWriter_record_pad_0);
	return self;
}

/*
 * chunks_record_RecordWriter_list_frame_0_t is list's state across its pad:
 * its arguments, the items it takes, the field's bytes and addresses, and the
 * item it is writing.
 */
typedef struct chunks_record_RecordWriter_list_frame_0_t
{
	ChunksRecordRecordWriter *self;
	uint32		number;

	/*
	 * items is the Vec<RecordWriter> it takes, of which the items from next
	 * on are still its.
	 */
	StdVec		items;
	bool		items_live;
	size_t		next;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		addresses;
	bool		addresses_live;
	ChunksRecordRecordWriter item;
	bool		item_live;
	StdVec		item_bytes;
	bool		item_bytes_live;
	StdVec		item_addresses;
	bool		item_addresses_live;
} chunks_record_RecordWriter_list_frame_0_t;

/* chunks_record_RecordWriter_list_body_0 is list's body. */
static void
chunks_record_RecordWriter_list_body_0(void *frame_)
{
	chunks_record_RecordWriter_list_frame_0_t *frame = frame_;
	uint32		start;
	StdSizeHint hint;

	frame->bytes = (StdVec) STD_VEC_NEW;
	frame->bytes_live = true;
	frame->addresses = (StdVec) STD_VEC_NEW;
	frame->addresses_live = true;
	while (frame->next < frame->items.len)
	{
		memcpy(&frame->item, STD_PTR_AT(frame->items.ptr, frame->next, sizeof(ChunksRecordRecordWriter)),
			   sizeof(ChunksRecordRecordWriter));
		frame->next = std_wrapping_add_usize(frame->next, 1);
		frame->item_live = true;
		chunks_record_RecordWriter_finish_value(&frame->item, &frame->item_bytes, &frame->item_addresses);
		frame->item_bytes_live = true;
		frame->item_addresses_live = true;
		chunks_varint_put_varint(&frame->bytes, (uint64) frame->item_bytes.len);
		hint.lower = frame->item_addresses.len;
		hint.has_upper = true;
		hint.upper = frame->item_addresses.len;
		std_vec_extend_trusted_reserve(&frame->addresses, hint, CHUNKS_RECORD_PLACED);
		for (size_t i = 0, i_len = frame->item_addresses.len; i < i_len; i++)
		{
			const chunks_TupleU32Locator_t *o = &STD_VEC_AT(&frame->item_addresses, chunks_TupleU32Locator_t, i);
			chunks_TupleU32Locator_t placed;

			placed.f0 = STD_ADD(u32, (uint32) frame->bytes.len, o->f0, 106, 71);
			placed.f1 = o->f1;
			memcpy(STD_PTR_AT(frame->addresses.ptr, frame->addresses.len, sizeof(placed)), &placed, sizeof(placed));
			frame->addresses.len = std_wrapping_add_usize(frame->addresses.len, 1);
		}
		frame->item_addresses_live = false;
		std_vec_free_Checked(&frame->item_addresses, CHUNKS_RECORD_PLACED);
		std_vec_extend_from_slice_Checked(&frame->bytes, frame->item_bytes.ptr, frame->item_bytes.len, 1, 1);
		frame->item_bytes_live = false;
		std_vec_free_Checked(&frame->item_bytes, 1, 1);
		frame->item_live = false;
		chunks_record_RecordWriter_drop(&frame->item);
	}
	frame->items_live = false;
	std_vec_free_Checked(&frame->items, sizeof(ChunksRecordRecordWriter), _Alignof(ChunksRecordRecordWriter));
	chunks_record_RecordWriter_field(frame->self, frame->number, frame->bytes.ptr, frame->bytes.len);
	start = (uint32) STD_SUB(usize, frame->self->out.len, frame->bytes.len, 110, 22);
	hint.lower = frame->addresses.len;
	hint.has_upper = true;
	hint.upper = frame->addresses.len;
	std_vec_extend_trusted_reserve(&frame->self->addresses, hint, CHUNKS_RECORD_PLACED);
	for (size_t i = 0, i_len = frame->addresses.len; i < i_len; i++)
	{
		const chunks_TupleU32Locator_t *o = &STD_VEC_AT(&frame->addresses, chunks_TupleU32Locator_t, i);
		chunks_TupleU32Locator_t placed;

		placed.f0 = STD_ADD(u32, start, o->f0, 111, 67);
		placed.f1 = o->f1;
		memcpy(STD_PTR_AT(frame->self->addresses.ptr, frame->self->addresses.len, sizeof(placed)), &placed, sizeof(placed));
		frame->self->addresses.len = std_wrapping_add_usize(frame->self->addresses.len, 1);
	}
	frame->addresses_live = false;
	std_vec_free_Checked(&frame->addresses, CHUNKS_RECORD_PLACED);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
}

/*
 * chunks_record_RecordWriter_list_pad_0 is list's pad: the item it is
 * writing and its parts, the field's addresses and bytes, and the items it
 * hasn't reached, while it holds them.
 */
static void
chunks_record_RecordWriter_list_pad_0(void *frame_)
{
	chunks_record_RecordWriter_list_frame_0_t *frame = frame_;

	if (frame->item_addresses_live)
		std_vec_free_Checked(&frame->item_addresses, CHUNKS_RECORD_PLACED);
	if (frame->item_bytes_live)
		std_vec_free_Checked(&frame->item_bytes, 1, 1);
	if (frame->item_live)
		chunks_record_RecordWriter_drop(&frame->item);
	if (frame->items_live)
	{
		std_ptr_drop_in_place_slice(STD_PTR_AT(frame->items.ptr, frame->next, sizeof(ChunksRecordRecordWriter)),
									std_wrapping_sub_usize(frame->items.len, frame->next),
									sizeof(ChunksRecordRecordWriter), chunks_record_RecordWriter_drop);
		std_vec_free_Checked(&frame->items, sizeof(ChunksRecordRecordWriter), _Alignof(ChunksRecordRecordWriter));
	}
	if (frame->addresses_live)
		std_vec_free_Checked(&frame->addresses, CHUNKS_RECORD_PLACED);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
}

/*
 * chunks_record_RecordWriter_list appends a field holding records one after
 * another, each preceded by its length, taking their writers.
 */
ChunksRecordRecordWriter *
chunks_record_RecordWriter_list(ChunksRecordRecordWriter *self, uint32 number, StdVec *items)
{
	chunks_record_RecordWriter_list_frame_0_t frame = {.self = self, .number = number, .items = *items,
	.items_live = true};

	std_landing(&frame, chunks_record_RecordWriter_list_body_0, chunks_record_RecordWriter_list_pad_0);
	return self;
}

/* chunks_record_RecordWriter_finish returns the record's bytes, for a record that holds no addresses. */
StdVec
chunks_record_RecordWriter_finish(ChunksRecordRecordWriter *self)
{
	StdVec		out;

	CHUNKS_CONTRACT(117, 9, self->addresses.len == 0, "a record with addresses is a chunk or a tree value");
	out = self->out;
	self->out = (StdVec) STD_VEC_NEW;
	return out;
}

/*
 * chunks_record_RecordWriter_finish_value returns the record as a tree
 * value: its bytes and the offsets of its addresses with their locators.
 */
void
chunks_record_RecordWriter_finish_value(ChunksRecordRecordWriter *self, StdVec *out_0, StdVec *out_1)
{
	self->has_last = false;
	*out_0 = self->out;
	self->out = (StdVec) STD_VEC_NEW;
	*out_1 = self->addresses;
	self->addresses = (StdVec) STD_VEC_NEW;
}

/*
 * chunks_record_RecordWriter_finish_chunk_frame_0_t is finish_chunk's state
 * across its pad: its arguments, the offsets and locators unzip splits its
 * addresses into, and the header that takes the offsets.
 */
typedef struct chunks_record_RecordWriter_finish_chunk_frame_0_t
{
	ChunksRecordRecordWriter *self;
	ChunksHeaderKind kind;
	StdVec	   *out_0;
	StdVec	   *out_1;
	StdVec		offsets;
	bool		offsets_live;
	StdVec		locators;
	bool		locators_live;
	ChunksHeaderHeader header;
	bool		header_live;
} chunks_record_RecordWriter_finish_chunk_frame_0_t;

/* chunks_record_RecordWriter_finish_chunk_body_0 is finish_chunk's body. */
static void
chunks_record_RecordWriter_finish_chunk_body_0(void *frame_)
{
	chunks_record_RecordWriter_finish_chunk_frame_0_t *frame = frame_;
	StdVec		hashed;
	StdVec		moved;
	StdVecIntoIter taken;

	moved = frame->self->addresses;
	frame->self->addresses = (StdVec) STD_VEC_NEW;
	taken = std_Vec_IntoIterator_into_iter(&moved, CHUNKS_RECORD_PLACED, NULL);
	std_Iterator_unzip_VecIntoIter_Checked(&taken, offsetof(chunks_TupleU32Locator_t, f0), sizeof(uint32),
										   _Alignof(uint32), offsetof(chunks_TupleU32Locator_t, f1),
										   sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator),
										   &frame->offsets, &frame->locators);
	frame->offsets_live = true;
	frame->locators_live = true;
	frame->header.kind = frame->kind;
	frame->header.flags = 0;
	frame->header.addresses.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS;
	frame->offsets_live = false;
	frame->header.addresses.u.offsets = frame->offsets;
	frame->header_live = true;
	chunks_header_Header_write(&frame->header, frame->self->out.ptr, frame->self->out.len, &hashed);
	frame->header_live = false;
	chunks_header_Header_drop(&frame->header);
	std_vec_clear(&frame->self->out, 1, NULL);
	*frame->out_0 = hashed;
	frame->locators_live = false;
	*frame->out_1 = frame->locators;
}

/*
 * chunks_record_RecordWriter_finish_chunk_pad_0 is finish_chunk's pad: the
 * header, the locators, and the offsets, while it holds them.
 */
static void
chunks_record_RecordWriter_finish_chunk_pad_0(void *frame_)
{
	chunks_record_RecordWriter_finish_chunk_frame_0_t *frame = frame_;

	if (frame->header_live)
		chunks_header_Header_drop(&frame->header);
	if (frame->locators_live)
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	if (frame->offsets_live)
		std_vec_free_Checked(&frame->offsets, sizeof(uint32), _Alignof(uint32));
}

/*
 * chunks_record_RecordWriter_finish_chunk returns the record as a chunk of
 * the kind: its hashed bytes and the locators of its addresses.
 */
void
chunks_record_RecordWriter_finish_chunk(ChunksRecordRecordWriter *self, ChunksHeaderKind kind, StdVec *out_0,
										StdVec *out_1)
{
	chunks_record_RecordWriter_finish_chunk_frame_0_t frame = {.self = self, .kind = kind, .out_0 = out_0,
	.out_1 = out_1};

	std_landing(&frame, chunks_record_RecordWriter_finish_chunk_body_0,
				chunks_record_RecordWriter_finish_chunk_pad_0);
}

/*
 * chunks_record_fields_frame_0_t is fields' state across its pad: its
 * arguments, its results, and the fields it holds.
 */
typedef struct chunks_record_fields_frame_0_t
{
	const uint8 *bytes;
	size_t		bytes_len;
	StdVec	   *result;
	ChunksErrorError *error;
	StdVec		out;
	bool		out_live;
	bool		ok;
} chunks_record_fields_frame_0_t;

/* chunks_record_fields_body_0 is fields' body. */
static void
chunks_record_fields_body_0(void *frame_)
{
	static const char too_large[] = "record field number is too large";
	static const char out_of_order[] = "record fields are out of order";
	static const char past_end[] = "record field runs past the end of the record";
	chunks_record_fields_frame_0_t *frame = frame_;
	size_t		at = 0;
	bool		failed = false;

	frame->out = (StdVec) STD_VEC_NEW;
	frame->out_live = true;
	while (at < frame->bytes_len)
	{
		uint64		key;
		uint64		len64;
		size_t		len;
		uint32		number;
		StdTryFromIntError range;
		size_t		end;
		ChunksRecordField field;
		const ChunksRecordField *last;

		if (!chunks_varint_read_varint(frame->bytes, frame->bytes_len, &at, &key, frame->error))
		{
			failed = true;
			break;
		}
		if (!chunks_varint_read_varint(frame->bytes, frame->bytes_len, &at, &len64, frame->error))
		{
			failed = true;
			break;
		}
		len = (size_t) len64;
		if (!std_u32_try_from_u64(key, &number, &range))
		{
			*frame->error = chunks_error_Error_corrupt_RefStr(too_large, sizeof(too_large) - 1);
			failed = true;
			break;
		}
		last = frame->out.len > 0 ?
			STD_PTR_AT(frame->out.ptr, std_wrapping_sub_usize(frame->out.len, 1), sizeof(ChunksRecordField)) : NULL;
		if (last != NULL && last->number >= number)
		{
			*frame->error = chunks_error_Error_corrupt_RefStr(out_of_order, sizeof(out_of_order) - 1);
			failed = true;
			break;
		}
		if (!std_checked_add_usize(at, len, &end) || !(end <= frame->bytes_len))
		{
			*frame->error = chunks_error_Error_corrupt_RefStr(past_end, sizeof(past_end) - 1);
			failed = true;
			break;
		}
		std_range_check(at, end, frame->bytes_len, RUST_FILE, 150, 47);
		field.number = number;
		field.bytes = STD_PTR_AT(frame->bytes, at, 1);
		field.bytes_len = std_wrapping_sub_usize(end, at);
		field.at = at;
		std_vec_push_Checked(&frame->out, &field, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
		at = end;
	}
	frame->out_live = false;
	if (failed)
	{
		std_vec_free_Checked(&frame->out, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
		frame->ok = false;
		return;
	}
	*frame->result = frame->out;
	frame->ok = true;
}

/* chunks_record_fields_pad_0 is fields' pad: the fields, while it holds them. */
static void
chunks_record_fields_pad_0(void *frame_)
{
	chunks_record_fields_frame_0_t *frame = frame_;

	if (frame->out_live)
		std_vec_free_Checked(&frame->out, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
}

/*
 * chunks_record_fields reads a record's fields, checking that they are in
 * increasing number order with no field repeated.
 */
bool
chunks_record_fields(const uint8 *bytes, size_t bytes_len, StdVec *result, ChunksErrorError *error)
{
	chunks_record_fields_frame_0_t frame = {.bytes = bytes, .bytes_len = bytes_len, .result = result,
	.error = error};

	std_landing(&frame, chunks_record_fields_body_0, chunks_record_fields_pad_0);
	return frame.ok;
}

/* chunks_record_int reads a field holding a signed integer as a zigzag varint. */
bool
chunks_record_int(const ChunksRecordField *field, int64 *result, ChunksErrorError *error)
{
	uint64		value;

	if (!chunks_record_uint(field, &value, error))
		return false;
	*result = chunks_record_unzigzag(value);
	return true;
}

/* chunks_record_zigzag returns a signed integer as the unsigned one a zigzag varint holds. */
uint64
chunks_record_zigzag(int64 value)
{
	return (uint64) (STD_SHL_FOLDED(i64, value, 1) ^ STD_SHR_FOLDED(i64, value, 63));
}

/* chunks_record_unzigzag returns the signed integer a zigzag varint holds. */
int64
chunks_record_unzigzag(uint64 value)
{
	int64		low = (int64) STD_SHR_FOLDED(u64, value, 1);
	int64		sign = STD_NEG(i64, (int64) (value & 1), 168, 27);

	return low ^ sign;
}

/*
 * chunks_record_Record_parse_closure_0 is parse's
 * |hashed| Ok(Header::read(hashed)?.0.addresses.len()): how many addresses
 * the chunk's header lists.
 */
static bool
chunks_record_Record_parse_closure_0(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
									 ChunksErrorError *error)
{
	ChunksHeaderHeader header;
	size_t		body;

	if (!chunks_header_Header_read(hashed, hashed_len, &header, &body, error))
		return false;
	*result = chunks_header_Addresses_len(&header.addresses);
	chunks_header_Header_drop(&header);
	return true;
}

/*
 * chunks_record_Record_parse_frame_0_t is parse's state across its pad: its
 * arguments, its results, and the header, offsets, and locators it holds.
 */
typedef struct chunks_record_Record_parse_frame_0_t
{
	const ChunksChunkChunk *chunk;
	ChunksRecordRecord *result;
	ChunksErrorError *error;
	ChunksHeaderHeader header;
	bool		header_live;
	StdVec		offsets;
	bool		offsets_live;
	StdVec		locators;
	bool		locators_live;
	bool		ok;
} chunks_record_Record_parse_frame_0_t;

/* chunks_record_Record_parse_body_0 is parse's body. */
static void
chunks_record_Record_parse_body_0(void *frame_)
{
	static const char lists_offsets[] = "a record's address table lists offsets";
	static const char out_of_order[] = "a record's addresses are out of order";
	chunks_record_Record_parse_frame_0_t *frame = frame_;
	ChunksTrailerStored split;
	size_t		body;
	const uint32 *v;
	size_t		v_len;
	StdSizeHint hint;
	StdVec		fields;

	if (!chunks_chunk_Chunk_split(frame->chunk, chunks_record_Record_parse_closure_0, NULL, NULL, &split,
								  frame->error))
		return;
	if (!chunks_header_Header_read(split.hashed.ptr, split.hashed.len, &frame->header, &body, frame->error))
		return;
	frame->header_live = true;
	if (frame->header.addresses.tag != CHUNKS_HEADER_ADDRESSES_OFFSETS)
	{
		*frame->error = chunks_error_Error_corrupt_RefStr(lists_offsets, sizeof(lists_offsets) - 1);
		frame->header_live = false;
		chunks_header_Header_drop(&frame->header);
		return;
	}
	frame->header_live = false;
	frame->offsets = frame->header.addresses.u.offsets;
	frame->offsets_live = true;
	v = frame->offsets.ptr;
	v_len = frame->offsets.len;
	while (!(2 > v_len))
	{
		const uint32 *w = v;

		v = v + 1;
		v_len = std_wrapping_sub_usize(v_len, 1);
		std_index_check(0, 2, RUST_FILE, 190, 39);
		std_index_check(1, 2, RUST_FILE, 190, 51);
		if (STD_ADD(u32, w[0], 32, 190, 39) > w[1])
		{
			*frame->error = chunks_error_Error_corrupt_RefStr(out_of_order, sizeof(out_of_order) - 1);
			frame->offsets_live = false;
			std_vec_free_Checked(&frame->offsets, sizeof(uint32), _Alignof(uint32));
			return;
		}
	}
	hint.lower = frame->offsets.len;
	hint.has_upper = true;
	hint.upper = frame->offsets.len;
	std_vec_with_capacity_trusted(&frame->locators, hint, sizeof(ChunksTrailerLocator),
								  _Alignof(ChunksTrailerLocator));
	frame->locators_live = true;
	std_vec_extend_trusted_reserve(&frame->locators, hint, sizeof(ChunksTrailerLocator),
								   _Alignof(ChunksTrailerLocator));
	for (size_t i = 0, i_end = frame->offsets.len; i < i_end; i = std_forward_unchecked_usize_Checked(i, 1))
	{
		ChunksTrailerLocator locator = chunks_trailer_Stored_locator(&split, i);

		memcpy(STD_PTR_AT(frame->locators.ptr, frame->locators.len, sizeof(locator)), &locator, sizeof(locator));
		frame->locators.len = std_wrapping_add_usize(frame->locators.len, 1);
	}
	std_range_from_check(body, split.hashed.len, RUST_FILE, 194, 68);
	if (!chunks_record_fields(STD_PTR_AT(split.hashed.ptr, body, 1), std_wrapping_sub_usize(split.hashed.len, body),
							  &fields, frame->error))
	{
		frame->locators_live = false;
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
		frame->offsets_live = false;
		std_vec_free_Checked(&frame->offsets, sizeof(uint32), _Alignof(uint32));
		return;
	}
	frame->result->kind = frame->header.kind;
	frame->result->fields = fields;
	frame->offsets_live = false;
	frame->result->offsets = frame->offsets;
	frame->locators_live = false;
	frame->result->locators = frame->locators;
	frame->ok = true;
}

/* chunks_record_Record_parse_pad_0 is parse's pad: the locators, the offsets, and the header, while it holds them. */
static void
chunks_record_Record_parse_pad_0(void *frame_)
{
	chunks_record_Record_parse_frame_0_t *frame = frame_;

	if (frame->locators_live)
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	if (frame->offsets_live)
		std_vec_free_Checked(&frame->offsets, sizeof(uint32), _Alignof(uint32));
	if (frame->header_live)
		chunks_header_Header_drop(&frame->header);
}

/* chunks_record_Record_parse reads a record chunk. */
bool
chunks_record_Record_parse(const ChunksChunkChunk *chunk, ChunksRecordRecord *result, ChunksErrorError *error)
{
	chunks_record_Record_parse_frame_0_t frame = {.chunk = chunk, .result = result, .error = error};

	std_landing(&frame, chunks_record_Record_parse_body_0, chunks_record_Record_parse_pad_0);
	return frame.ok;
}

/*
 * chunks_record_Record_value_frame_0_t is value's state across its pad: its
 * arguments, its results, and the offsets and locators it holds.
 */
typedef struct chunks_record_Record_value_frame_0_t
{
	const uint8 *bytes;
	size_t		bytes_len;
	const chunks_TupleU32Locator_t *addresses;
	size_t		addresses_len;
	ChunksRecordRecord *result;
	ChunksErrorError *error;
	StdVec		offsets;
	bool		offsets_live;
	StdVec		locators;
	bool		locators_live;
	bool		ok;
} chunks_record_Record_value_frame_0_t;

/* chunks_record_Record_value_body_0 is value's body. */
static void
chunks_record_Record_value_body_0(void *frame_)
{
	chunks_record_Record_value_frame_0_t *frame = frame_;
	StdVec		fields;

	std_Iterator_unzip_CopiedSliceIter_Checked(frame->addresses, frame->addresses_len, sizeof(chunks_TupleU32Locator_t),
											   offsetof(chunks_TupleU32Locator_t, f0), sizeof(uint32),
											   _Alignof(uint32), offsetof(chunks_TupleU32Locator_t, f1),
											   sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator),
											   &frame->offsets, &frame->locators);
	frame->offsets_live = true;
	frame->locators_live = true;
	if (!chunks_record_fields(frame->bytes, frame->bytes_len, &fields, frame->error))
	{
		frame->locators_live = false;
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
		frame->offsets_live = false;
		std_vec_free_Checked(&frame->offsets, sizeof(uint32), _Alignof(uint32));
		return;
	}
	frame->result->kind = 0;
	frame->result->fields = fields;
	frame->offsets_live = false;
	frame->result->offsets = frame->offsets;
	frame->locators_live = false;
	frame->result->locators = frame->locators;
	frame->ok = true;
}

/* chunks_record_Record_value_pad_0 is value's pad: the locators and the offsets, while it holds them. */
static void
chunks_record_Record_value_pad_0(void *frame_)
{
	chunks_record_Record_value_frame_0_t *frame = frame_;

	if (frame->locators_live)
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	if (frame->offsets_live)
		std_vec_free_Checked(&frame->offsets, sizeof(uint32), _Alignof(uint32));
}

/*
 * chunks_record_Record_value reads a record held as a tree value, with the
 * offsets of its addresses and their locators.
 */
bool
chunks_record_Record_value(const uint8 *bytes, size_t bytes_len, const chunks_TupleU32Locator_t *addresses,
						   size_t addresses_len, ChunksRecordRecord *result, ChunksErrorError *error)
{
	chunks_record_Record_value_frame_0_t frame = {.bytes = bytes, .bytes_len = bytes_len, .addresses = addresses,
	.addresses_len = addresses_len, .result = result, .error = error};

	std_landing(&frame, chunks_record_Record_value_body_0, chunks_record_Record_value_pad_0);
	return frame.ok;
}

static bool chunks_record_Record_inner(const ChunksRecordRecord *self, const uint8 *bytes, size_t bytes_len,
									   size_t at, ChunksRecordRecord *result, ChunksErrorError *error);

/* chunks_record_Record_nested reads a record that a field holds. */
bool
chunks_record_Record_nested(const ChunksRecordRecord *self, const ChunksRecordField *field,
							ChunksRecordRecord *result, ChunksErrorError *error)
{
	return chunks_record_Record_inner(self, field->bytes, field->bytes_len, field->at, result, error);
}

static void chunks_record_Record_within(const ChunksRecordRecord *self, size_t at, size_t len, size_t *start,
										size_t *end);

/*
 * chunks_record_Record_inner_frame_0_t is inner's state across its pad: its
 * arguments, its results, and the fields and offsets it holds.
 */
typedef struct chunks_record_Record_inner_frame_0_t
{
	const ChunksRecordRecord *self;
	const uint8 *bytes;
	size_t		bytes_len;
	size_t		at;
	ChunksRecordRecord *result;
	ChunksErrorError *error;
	StdVec		fields;
	bool		fields_live;
	StdVec		offsets;
	bool		offsets_live;
	bool		ok;
} chunks_record_Record_inner_frame_0_t;

/* chunks_record_Record_inner_body_0 is inner's body. */
static void
chunks_record_Record_inner_body_0(void *frame_)
{
	chunks_record_Record_inner_frame_0_t *frame = frame_;
	size_t		start;
	size_t		end;
	const uint32 *from;
	StdSizeHint hint;
	StdVec		locators;

	chunks_record_Record_within(frame->self, frame->at, frame->bytes_len, &start, &end);
	if (!chunks_record_fields(frame->bytes, frame->bytes_len, &frame->fields, frame->error))
		return;
	frame->fields_live = true;
	std_range_check(start, end, frame->self->offsets.len, RUST_FILE, 214, 34);
	from = STD_PTR_AT(frame->self->offsets.ptr, start, sizeof(uint32));
	hint.lower = std_wrapping_sub_usize(end, start);
	hint.has_upper = true;
	hint.upper = std_wrapping_sub_usize(end, start);
	std_vec_with_capacity_trusted(&frame->offsets, hint, sizeof(uint32), _Alignof(uint32));
	frame->offsets_live = true;
	std_vec_extend_trusted_reserve(&frame->offsets, hint, sizeof(uint32), _Alignof(uint32));
	for (size_t i = 0, i_len = std_wrapping_sub_usize(end, start); i < i_len; i++)
	{
		uint32		moved = STD_SUB(u32, from[i], (uint32) frame->at, 214, 67);

		memcpy(STD_PTR_AT(frame->offsets.ptr, frame->offsets.len, sizeof(uint32)), &moved, sizeof(uint32));
		frame->offsets.len = std_wrapping_add_usize(frame->offsets.len, 1);
	}
	std_range_check(start, end, frame->self->locators.len, RUST_FILE, 215, 36);
	std_slice_to_vec_Checked(&locators, STD_PTR_AT(frame->self->locators.ptr, start, sizeof(ChunksTrailerLocator)),
							 std_wrapping_sub_usize(end, start), sizeof(ChunksTrailerLocator),
							 _Alignof(ChunksTrailerLocator));
	frame->result->kind = 0;
	frame->fields_live = false;
	frame->result->fields = frame->fields;
	frame->offsets_live = false;
	frame->result->offsets = frame->offsets;
	frame->result->locators = locators;
	frame->ok = true;
}

/* chunks_record_Record_inner_pad_0 is inner's pad: the offsets and the fields, while it holds them. */
static void
chunks_record_Record_inner_pad_0(void *frame_)
{
	chunks_record_Record_inner_frame_0_t *frame = frame_;

	if (frame->offsets_live)
		std_vec_free_Checked(&frame->offsets, sizeof(uint32), _Alignof(uint32));
	if (frame->fields_live)
		std_vec_free_Checked(&frame->fields, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
}

/* chunks_record_Record_inner reads a record at an offset of this record. */
static bool
chunks_record_Record_inner(const ChunksRecordRecord *self, const uint8 *bytes, size_t bytes_len, size_t at,
						   ChunksRecordRecord *result, ChunksErrorError *error)
{
	chunks_record_Record_inner_frame_0_t frame = {.self = self, .bytes = bytes, .bytes_len = bytes_len, .at = at,
	.result = result, .error = error};

	std_landing(&frame, chunks_record_Record_inner_body_0, chunks_record_Record_inner_pad_0);
	return frame.ok;
}

/* chunks_record_Record_within_closure_0 is within's |&o| (o as usize) < at. */
static bool
chunks_record_Record_within_closure_0(void *captures, const void *elem)
{
	const size_t *at = captures;
	const uint32 *o = elem;

	return (size_t) *o < *at;
}

/* chunks_record_Record_within_closure_1_t is what within's second closure captures: the offset and the length. */
typedef struct chunks_record_Record_within_closure_1_t
{
	const size_t *at;
	const size_t *len;
} chunks_record_Record_within_closure_1_t;

/* chunks_record_Record_within_closure_1 is within's |&o| (o as usize) < at + len. */
static bool
chunks_record_Record_within_closure_1(void *captures, const void *elem)
{
	const chunks_record_Record_within_closure_1_t *c = captures;
	const uint32 *o = elem;

	return (size_t) *o < STD_ADD(usize, *c->at, *c->len, 222, 65);
}

/*
 * chunks_record_Record_within returns the positions in the address table of
 * the addresses stored in the bytes at an offset, from start up to end.
 */
static void
chunks_record_Record_within(const ChunksRecordRecord *self, size_t at, size_t len, size_t *start, size_t *end)
{
	chunks_record_Record_within_closure_1_t past = {&at, &len};

	*start = std_slice_partition_point(self->offsets.ptr, self->offsets.len, sizeof(uint32),
									   chunks_record_Record_within_closure_0, &at, NULL);
	*end = std_slice_partition_point(self->offsets.ptr, self->offsets.len, sizeof(uint32),
									 chunks_record_Record_within_closure_1, &past, NULL);
}

/*
 * chunks_record_Record_list_frame_0_t is list's state across its pad: its
 * arguments, its results, and the records it holds.
 */
typedef struct chunks_record_Record_list_frame_0_t
{
	const ChunksRecordRecord *self;
	const ChunksRecordField *field;
	StdVec	   *result;
	ChunksErrorError *error;
	StdVec		out;
	bool		out_live;
	bool		ok;
} chunks_record_Record_list_frame_0_t;

/* chunks_record_Record_list_body_0 is list's body. */
static void
chunks_record_Record_list_body_0(void *frame_)
{
	static const char past_field[] = "record list item runs past its field";
	chunks_record_Record_list_frame_0_t *frame = frame_;
	const ChunksRecordField *field = frame->field;
	size_t		at = 0;
	bool		failed = false;

	frame->out = (StdVec) STD_VEC_NEW;
	frame->out_live = true;
	while (at < field->bytes_len)
	{
		uint64		len64;
		size_t		len;
		size_t		end;
		size_t		item_at;
		ChunksRecordRecord item;

		if (!chunks_varint_read_varint(field->bytes, field->bytes_len, &at, &len64, frame->error))
		{
			failed = true;
			break;
		}
		len = (size_t) len64;
		if (!std_checked_add_usize(at, len, &end) || !(end <= field->bytes_len))
		{
			*frame->error = chunks_error_Error_corrupt_RefStr(past_field, sizeof(past_field) - 1);
			failed = true;
			break;
		}
		std_range_check(at, end, field->bytes_len, RUST_FILE, 233, 45);
		item_at = STD_ADD(usize, field->at, at, 233, 56);
		if (!chunks_record_Record_inner(frame->self, STD_PTR_AT(field->bytes, at, 1), std_wrapping_sub_usize(end, at),
										item_at, &item, frame->error))
		{
			failed = true;
			break;
		}
		std_vec_push_Checked(&frame->out, &item, sizeof(ChunksRecordRecord), _Alignof(ChunksRecordRecord));
		at = end;
	}
	frame->out_live = false;
	if (failed)
	{
		std_vec_drop_Checked(&frame->out, sizeof(ChunksRecordRecord), _Alignof(ChunksRecordRecord),
							 chunks_record_Record_drop);
		return;
	}
	*frame->result = frame->out;
	frame->ok = true;
}

/* chunks_record_Record_list_pad_0 is list's pad: the records, while it holds them. */
static void
chunks_record_Record_list_pad_0(void *frame_)
{
	chunks_record_Record_list_frame_0_t *frame = frame_;

	if (frame->out_live)
		std_vec_drop_Checked(&frame->out, sizeof(ChunksRecordRecord), _Alignof(ChunksRecordRecord),
							 chunks_record_Record_drop);
}

/* chunks_record_Record_list reads the records a list field holds. */
bool
chunks_record_Record_list(const ChunksRecordRecord *self, const ChunksRecordField *field, StdVec *result,
						  ChunksErrorError *error)
{
	chunks_record_Record_list_frame_0_t frame = {.self = self, .field = field, .result = result, .error = error};

	std_landing(&frame, chunks_record_Record_list_body_0, chunks_record_Record_list_pad_0);
	return frame.ok;
}

/* chunks_record_Record_bytes returns the bytes of the field with the number, or NULL. */
const uint8 *
chunks_record_Record_bytes(const ChunksRecordRecord *self, uint32 number, size_t *result_len)
{
	const ChunksRecordField *field = chunks_record_Record_field(self, number);

	if (field == NULL)
		return NULL;
	*result_len = field->bytes_len;
	return field->bytes;
}

/* chunks_record_Record_uint returns the unsigned integer of the field with the number, or zero when it is absent. */
bool
chunks_record_Record_uint(const ChunksRecordRecord *self, uint32 number, uint64 *result, ChunksErrorError *error)
{
	const ChunksRecordField *field = chunks_record_Record_field(self, number);

	if (field != NULL)
		return chunks_record_uint(field, result, error);
	*result = 0;
	return true;
}

/* chunks_record_Record_int returns the signed integer of the field with the number, or zero when it is absent. */
bool
chunks_record_Record_int(const ChunksRecordRecord *self, uint32 number, int64 *result, ChunksErrorError *error)
{
	const ChunksRecordField *field = chunks_record_Record_field(self, number);

	if (field != NULL)
		return chunks_record_int(field, result, error);
	*result = 0;
	return true;
}

/*
 * chunks_record_Record_address_frame_0_t is address's state across its pad:
 * the addresses it holds.
 */
typedef struct chunks_record_Record_address_frame_0_t
{
	uint32		number;
	StdVec		addresses;
	bool		addresses_live;
	ChunksErrorError *error;
} chunks_record_Record_address_frame_0_t;

/* chunks_record_Record_address_body_0 is address's error, made while it holds the addresses. */
static void
chunks_record_Record_address_body_0(void *frame_)
{
	chunks_record_Record_address_frame_0_t *frame = frame_;
	StdString	message;
	StdFmtPiece pieces[] = {STD_FMT_LIT("record field "), STD_FMT_ARG(0),
	STD_FMT_LIT(" holds more than one address")};
	StdFmtArg	args[] = {{&frame->number, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, 3, args};

	std_fmt_format_Checked(&message, &arguments);
	*frame->error = chunks_error_Error_corrupt(&message);
	frame->addresses_live = false;
	std_vec_free_Checked(&frame->addresses, CHUNKS_RECORD_ENTRY);
}

/* chunks_record_Record_address_pad_0 is address's pad: the addresses, while it holds them. */
static void
chunks_record_Record_address_pad_0(void *frame_)
{
	chunks_record_Record_address_frame_0_t *frame = frame_;

	if (frame->addresses_live)
		std_vec_free_Checked(&frame->addresses, CHUNKS_RECORD_ENTRY);
}

/*
 * chunks_record_Record_address returns the one address the field with the
 * number holds, or nothing when it is absent.
 */
bool
chunks_record_Record_address(const ChunksRecordRecord *self, uint32 number, bool *has_result,
							 ChunksAddressAddress *result_0, ChunksTrailerLocator *result_1, ChunksErrorError *error)
{
	const ChunksRecordField *field = chunks_record_Record_field(self, number);
	chunks_record_Record_address_frame_0_t frame = {.number = number, .error = error};

	if (field == NULL)
	{
		*has_result = false;
		return true;
	}
	if (!chunks_record_Record_addresses(self, field, &frame.addresses, error))
		return false;
	if (frame.addresses.len == 1)
	{
		const chunks_TupleAddressLocator_t *one = frame.addresses.ptr;

		*has_result = true;
		*result_0 = one->f0;
		*result_1 = one->f1;
		std_vec_free_Checked(&frame.addresses, CHUNKS_RECORD_ENTRY);
		return true;
	}
	frame.addresses_live = true;
	std_landing(&frame, chunks_record_Record_address_body_0, chunks_record_Record_address_pad_0);
	return false;
}

/* chunks_record_Record_field returns the field with the number, or NULL. */
const ChunksRecordField *
chunks_record_Record_field(const ChunksRecordRecord *self, uint32 number)
{
	const ChunksRecordField *fields = self->fields.ptr;

	for (size_t i = 0, i_len = self->fields.len; i < i_len; i++)
	{
		if (fields[i].number == number)
			return &fields[i];
	}
	return NULL;
}

/* chunks_record_Record_addresses_closure_0 is addresses' |&o| (o as usize) < field.at. */
static bool
chunks_record_Record_addresses_closure_0(void *captures, const void *elem)
{
	const ChunksRecordField *field = captures;
	const uint32 *o = elem;

	return (size_t) *o < field->at;
}

/*
 * chunks_record_Record_addresses_frame_0_t is addresses' state across its
 * pad: its arguments, its result, and the addresses it holds.
 */
typedef struct chunks_record_Record_addresses_frame_0_t
{
	const ChunksRecordRecord *self;
	const ChunksRecordField *field;
	StdVec	   *result;
	ChunksErrorError *error;
	StdVec		out;
	bool		out_live;
	bool		ok;
} chunks_record_Record_addresses_frame_0_t;

/* chunks_record_Record_addresses_body_0 is addresses' body. */
static void
chunks_record_Record_addresses_body_0(void *frame_)
{
	chunks_record_Record_addresses_frame_0_t *frame = frame_;
	const ChunksRecordRecord *self = frame->self;
	const ChunksRecordField *field = frame->field;
	size_t		first;
	size_t		count;
	StdSizeHint hint;
	size_t		i = 0;
	const uint8 *v;
	size_t		v_len;

	if (!std_is_multiple_of_usize(field->bytes_len, 32))
	{
		StdString	message;
		StdFmtPiece pieces[] = {STD_FMT_LIT("record field "), STD_FMT_ARG(0), STD_FMT_LIT(" holds no addresses")};
		StdFmtArg	args[] = {{&field->number, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_corrupt(&message);
		return;
	}
	first = std_slice_partition_point(self->offsets.ptr, self->offsets.len, sizeof(uint32),
									  chunks_record_Record_addresses_closure_0, (void *) field, NULL);
	std_slice_chunks_check(32, RUST_FILE, 284, 14);
	count = STD_DIV_FOLDED(usize, field->bytes_len, 32);
	if (STD_REM_FOLDED(usize, field->bytes_len, 32) != 0)
		count = std_wrapping_add_usize(count, 1);
	hint.lower = count;
	hint.has_upper = true;
	hint.upper = count;
	std_vec_with_capacity_trusted(&frame->out, hint, CHUNKS_RECORD_ENTRY);
	frame->out_live = true;
	std_vec_extend_trusted_reserve(&frame->out, hint, CHUNKS_RECORD_ENTRY);
	v = field->bytes;
	v_len = field->bytes_len;
	while (v_len > 0)
	{
		const uint8 *a = v;
		size_t		a_len = v_len < 32 ? v_len : 32;
		size_t		at = i;
		size_t		slot;
		chunks_TupleAddressLocator_t entry = {0};

		v = v + a_len;
		v_len = std_wrapping_sub_usize(v_len, a_len);
		i = std_Enumerate_count_step(i);
		slot = STD_ADD(usize, first, at, 287, 54);
		if (slot < self->offsets.len)
		{
			const uint32 *o = STD_PTR_AT(self->offsets.ptr, slot, sizeof(uint32));
			size_t		offset = STD_MUL(usize, 32, at, 288, 58);

			if ((size_t) *o == STD_ADD(usize, field->at, offset, 288, 47))
			{
				size_t		index = STD_ADD(usize, first, at, 288, 82);

				std_index_check(index, self->locators.len, RUST_FILE, 288, 68);
				entry.f1 = STD_VEC_AT(&self->locators, ChunksTrailerLocator, index);
			}
		}
		if (a_len != 32)
		{
			static const char text[] = "called `Result::unwrap()` on an `Err` value";

			std_result_unwrap_failed(text, sizeof(text) - 1, STD_DANGLING, std_TryFromSliceError_Debug_fmt,
									 RUST_FILE, 291, 39);
		}
		memcpy(entry.f0.f0, a, 32);
		memcpy(STD_PTR_AT(frame->out.ptr, frame->out.len, sizeof(entry)), &entry, sizeof(entry));
		frame->out.len = std_wrapping_add_usize(frame->out.len, 1);
	}
	frame->out_live = false;
	*frame->result = frame->out;
	frame->ok = true;
}

/* chunks_record_Record_addresses_pad_0 is addresses' pad: the addresses, while it holds them. */
static void
chunks_record_Record_addresses_pad_0(void *frame_)
{
	chunks_record_Record_addresses_frame_0_t *frame = frame_;

	if (frame->out_live)
		std_vec_free_Checked(&frame->out, CHUNKS_RECORD_ENTRY);
}

/* chunks_record_Record_addresses returns the addresses a field holds with their locators. */
bool
chunks_record_Record_addresses(const ChunksRecordRecord *self, const ChunksRecordField *field, StdVec *result,
							   ChunksErrorError *error)
{
	chunks_record_Record_addresses_frame_0_t frame = {.self = self, .field = field, .result = result,
	.error = error};

	std_landing(&frame, chunks_record_Record_addresses_body_0, chunks_record_Record_addresses_pad_0);
	return frame.ok;
}

/* chunks_record_uint reads a field holding an unsigned integer as a varint. */
bool
chunks_record_uint(const ChunksRecordField *field, uint64 *result, ChunksErrorError *error)
{
	static const char trailing[] = "record integer field has trailing bytes";
	size_t		at = 0;
	uint64		value;

	if (!chunks_varint_read_varint(field->bytes, field->bytes_len, &at, &value, error))
		return false;
	if (at != field->bytes_len)
	{
		*error = chunks_error_Error_corrupt_RefStr(trailing, sizeof(trailing) - 1);
		return false;
	}
	*result = value;
	return true;
}

/* chunks_record_RecordWriter_drop is RecordWriter's drop glue: its bytes and its addresses. */
void
chunks_record_RecordWriter_drop(void *self_)
{
	ChunksRecordRecordWriter *self = self_;

	std_vec_free_Checked(&self->out, 1, 1);
	std_vec_free_Checked(&self->addresses, CHUNKS_RECORD_PLACED);
}

/* chunks_record_Record_drop is Record's drop glue: its fields, offsets, and locators. */
void
chunks_record_Record_drop(void *self_)
{
	ChunksRecordRecord *self = self_;

	std_vec_free_Checked(&self->fields, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	std_vec_free_Checked(&self->offsets, sizeof(uint32), _Alignof(uint32));
	std_vec_free_Checked(&self->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
}

#ifdef DOLTGRES_TEST_CHUNKS

/*
 * test_chunks_record_tests_round_trips_fields_in_order_frame_0_t is the
 * test's state across its pad: the writer, the record's bytes, and its
 * fields.
 */
typedef struct test_chunks_record_tests_round_trips_fields_in_order_frame_0_t
{
	ChunksRecordRecordWriter writer;
	bool		writer_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		read;
	bool		read_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_record_tests_round_trips_fields_in_order_frame_0_t;

/* test_chunks_record_tests_round_trips_fields_in_order_body_0 is the test's body. */
static void
test_chunks_record_tests_round_trips_fields_in_order_body_0(void *frame_)
{
	test_chunks_record_tests_round_trips_fields_in_order_frame_0_t *frame = frame_;
	static const uint8 index_[] = {'i', 'n', 'd', 'e', 'x'};
	static const uint8 note[] = {'n', 'o', 't', 'e'};
	static const uint8 tail[] = {64, 4};
	ChunksErrorError error;
	ChunksRecordRecordWriter *chained;
	const ChunksRecordField *read;
	size_t		len;
	size_t		three = 3;
	uint64		value;
	uint64		expected = 300;
	size_t		start;
	size_t		end;

	frame->writer = chunks_record_RecordWriter_new();
	frame->writer_live = true;
	chained = chunks_record_RecordWriter_uint(&frame->writer, 1, 300);
	chained = chunks_record_RecordWriter_field(chained, 2, index_, sizeof(index_));
	chained = chunks_record_RecordWriter_field(chained, 64, note, sizeof(note));
	frame->bytes = chunks_record_RecordWriter_finish(chained);
	frame->bytes_live = true;
	frame->writer_live = false;
	chunks_record_RecordWriter_drop(&frame->writer);
	if (!chunks_record_fields(frame->bytes.ptr, frame->bytes.len, &frame->read, &error))
	{
		frame->error = error;
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(314, 35, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->read_live = true;
	read = frame->read.ptr;
	len = frame->read.len;
	STD_ASSERT_EQ(315, 9, len == three, &len, std_usize_Debug_fmt, &three, std_usize_Debug_fmt);
	std_index_check(0, frame->read.len, RUST_FILE, 316, 26);
	if (!chunks_record_uint(&read[0], &value, &error))
	{
		frame->error = error;
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(316, 35, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	STD_ASSERT_EQ(316, 9, value == expected, &value, std_u64_Debug_fmt, &expected, std_u64_Debug_fmt);
	{
		uint32		number;
		StdBytes	bytes;
		uint32		want_number = 2;
		StdBytes	want = {index_, sizeof(index_)};

		std_index_check(1, frame->read.len, RUST_FILE, 317, 21);
		number = read[1].number;
		std_index_check(1, frame->read.len, RUST_FILE, 317, 37);
		bytes.ptr = read[1].bytes;
		bytes.len = read[1].bytes_len;
		STD_ASSERT_EQ(317, 9, number == want_number && bytes.len == want.len &&
					  memcmp(bytes.ptr, want.ptr, want.len) == 0,
					  &STD_TUPLE(2, STD_ITEM(&number, std_u32_Debug_fmt), STD_ITEM(&bytes, std_bytes_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&want_number, std_u32_Debug_fmt), STD_ITEM(&want, std_bytes_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	{
		uint32		number;
		StdBytes	bytes;
		uint32		want_number = 64;
		StdBytes	want = {note, sizeof(note)};

		std_index_check(2, frame->read.len, RUST_FILE, 318, 21);
		number = read[2].number;
		std_index_check(2, frame->read.len, RUST_FILE, 318, 37);
		bytes.ptr = read[2].bytes;
		bytes.len = read[2].bytes_len;
		STD_ASSERT_EQ(318, 9, number == want_number && bytes.len == want.len &&
					  memcmp(bytes.ptr, want.ptr, want.len) == 0,
					  &STD_TUPLE(2, STD_ITEM(&number, std_u32_Debug_fmt), STD_ITEM(&bytes, std_bytes_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&want_number, std_u32_Debug_fmt), STD_ITEM(&want, std_bytes_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	start = STD_SUB(usize, frame->bytes.len, 6, 319, 26);
	end = STD_SUB(usize, frame->bytes.len, 4, 319, 43);
	std_range_check(start, end, frame->bytes.len, RUST_FILE, 319, 25);
	{
		StdBytes	slice = {STD_PTR_AT(frame->bytes.ptr, start, 1), std_wrapping_sub_usize(end, start)};
		StdBytes	want = {tail, sizeof(tail)};

		STD_ASSERT_EQ(319, 9, slice.len == want.len && memcmp(slice.ptr, want.ptr, want.len) == 0, &slice,
					  std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt);
	}
	frame->read_live = false;
	std_vec_free_Checked(&frame->read, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
}

/* test_chunks_record_tests_round_trips_fields_in_order_pad_0 is the test's pad. */
static void
test_chunks_record_tests_round_trips_fields_in_order_pad_0(void *frame_)
{
	test_chunks_record_tests_round_trips_fields_in_order_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->read_live)
		std_vec_free_Checked(&frame->read, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->writer_live)
		chunks_record_RecordWriter_drop(&frame->writer);
}

/*
 * test_chunks_record_tests_round_trips_fields_in_order: fields read back in
 * the order and with the bytes they were written, a field's number and
 * length leading its bytes.
 */
void
test_chunks_record_tests_round_trips_fields_in_order(void)
{
	test_chunks_record_tests_round_trips_fields_in_order_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_record_tests_round_trips_fields_in_order_body_0,
				test_chunks_record_tests_round_trips_fields_in_order_pad_0);
}

/* chunks_record_tests_VecAddress_Debug_fmt is a Vec<Address>'s Debug: a list of the addresses. */
bool
chunks_record_tests_VecAddress_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	const ChunksAddressAddress *items = v->ptr;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t i = 0, i_len = v->len; i < i_len; i++)
		std_DebugList_entry(&list, &items[i], chunks_address_Address_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* chunks_record_tests_VecAddress_PartialEq_eq is Vec<Address>'s PartialEq: the same length, and each address equal. */
bool
chunks_record_tests_VecAddress_PartialEq_eq(const StdVec *self, const StdVec *other)
{
	if (self->len != other->len)
		return false;
	for (size_t i = 0, i_len = self->len; i < i_len; i++)
	{
		if (!chunks_address_Address_PartialEq_eq(STD_PTR_AT(self->ptr, i, sizeof(ChunksAddressAddress)),
												 STD_PTR_AT(other->ptr, i, sizeof(ChunksAddressAddress))))
			return false;
	}
	return true;
}

/* chunks_record_tests_TupleAddressLocator_Debug_fmt is an (Address, Locator)'s Debug. */
bool
chunks_record_tests_TupleAddressLocator_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_TupleAddressLocator_t *t = value;

	return std_tuple_Debug_fmt(&STD_TUPLE(2, STD_ITEM(&t->f0, chunks_address_Address_Debug_fmt),
										  STD_ITEM(&t->f1, chunks_trailer_Locator_Debug_fmt)), f);
}

/* chunks_record_tests_VecTupleAddressLocator_Debug_fmt is a Vec<(Address, Locator)>'s Debug: a list of the pairs. */
bool
chunks_record_tests_VecTupleAddressLocator_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	const chunks_TupleAddressLocator_t *items = v->ptr;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t i = 0, i_len = v->len; i < i_len; i++)
		std_DebugList_entry(&list, &items[i], chunks_record_tests_TupleAddressLocator_Debug_fmt);
	return std_DebugList_finish(&list);
}

/*
 * chunks_record_tests_VecTupleAddressLocator_PartialEq_eq is a
 * Vec<(Address, Locator)>'s PartialEq: the lengths, then each pair in order.
 */
bool
chunks_record_tests_VecTupleAddressLocator_PartialEq_eq(const StdVec *self, const StdVec *other)
{
	const chunks_TupleAddressLocator_t *a = self->ptr;
	const chunks_TupleAddressLocator_t *b = other->ptr;

	if (self->len != other->len)
		return false;
	for (size_t i = 0, i_len = self->len; i < i_len; i++)
	{
		if (!chunks_address_Address_PartialEq_eq(&a[i].f0, &b[i].f0) ||
			!chunks_trailer_Locator_PartialEq_eq(&a[i].f1, &b[i].f1))
			return false;
	}
	return true;
}

/*
 * test_chunks_record_tests_records_hold_addresses_and_signed_integers_frame_0_t
 * is the test's state across its pad: every value it owns, each with whether
 * it is still its to drop.
 */
typedef struct test_chunks_record_tests_records_hold_addresses_and_signed_integers_frame_0_t
{
	ChunksRecordRecordWriter writer;
	bool		writer_live;
	StdVec		hashed;
	bool		hashed_live;
	StdVec		locators;
	bool		locators_live;
	StdVec		left;
	bool		left_live;
	StdVec		right;
	bool		right_live;
	StdVec		left_entries;
	bool		left_entries_live;
	StdVec		right_entries;
	bool		right_entries_live;
	ChunksTrailerStamps stamps;
	bool		stamps_live;
	ChunksChunkChunk stored;
	bool		stored_live;
	ChunksRecordRecord record;
	bool		record_live;
	ChunksRecordRecordWriter inner;
	bool		inner_live;
	ChunksRecordRecordWriter item;
	bool		item_live;
	ChunksRecordRecordWriter outer;
	bool		outer_live;
	StdVec		writers;
	bool		writers_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		addresses;
	bool		addresses_live;
	ChunksRecordRecord value;
	bool		value_live;
	ChunksRecordRecord nested;
	bool		nested_live;
	StdVec		items;
	bool		items_live;
	ChunksRecordRecordWriter each;
	bool		each_live;
	StdVec		each_bytes;
	bool		each_bytes_live;
	StdVec		each_fields;
	bool		each_fields_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_record_tests_records_hold_addresses_and_signed_integers_frame_0_t;

/* test_chunks_record_tests_records_hold_addresses_and_signed_integers_body_0 is the test's body. */
static void
test_chunks_record_tests_records_hold_addresses_and_signed_integers_body_0(void *frame_)
{
	test_chunks_record_tests_records_hold_addresses_and_signed_integers_frame_0_t *frame = frame_;
	static const uint8 a_text[] = {'a'};
	static const uint8 b_text[] = {'b'};
	static const uint8 x[] = {'x'};
	static const uint8 head[] = {'h', 'e', 'a', 'd'};
	static const int64 values[5] = {0, -1, 1, PG_INT64_MIN, PG_INT64_MAX};
	ChunksAddressAddress a = chunks_address_Address_of(a_text, sizeof(a_text));
	ChunksAddressAddress b = chunks_address_Address_of(b_text, sizeof(b_text));
	ChunksTrailerLocator la = chunks_trailer_Locator_at(1, 10, 20);
	ChunksTrailerLocator unknown = {0};
	ChunksErrorError error;
	const ChunksRecordField *field;
	ChunksHeaderKind kind = (ChunksHeaderKind) (CHUNKS_HEADER_FIRST_RECORD + 1);
	int64		number;
	int64		minus_five = -5;
	bool		has;
	ChunksAddressAddress found;
	ChunksTrailerLocator found_locator;
	size_t		len;
	size_t		two = 2;
	uint64		seven;
	uint64		want_seven = 7;

	{
		chunks_TupleAddressLocator_t pairs[2] = {{a, la}, {b, unknown}};
		ChunksRecordRecordWriter *chained;

		frame->writer = chunks_record_RecordWriter_new();
		frame->writer_live = true;
		chained = chunks_record_RecordWriter_int(&frame->writer, 1, -5);
		chained = chunks_record_RecordWriter_addresses(chained, 2, pairs, 2);
		chunks_record_RecordWriter_field(chained, 3, x, sizeof(x));
	}
	chunks_record_RecordWriter_finish_chunk(&frame->writer, kind, &frame->hashed, &frame->locators);
	frame->hashed_live = true;
	frame->locators_live = true;
	if (!chunks_header_addresses(frame->hashed.ptr, frame->hashed.len, &frame->left, &error))
	{
		frame->error = error;
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(329, 54, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->left_live = true;
	{
		ChunksAddressAddress *boxed = std_box_new_uninit_layout_Checked(2 * sizeof(ChunksAddressAddress),
																		_Alignof(ChunksAddressAddress));

		STD_CHECK_ALIGNED(boxed, _Alignof(ChunksAddressAddress), 329, 64);
		STD_CHECK_NULL_DEREF(boxed, 329, 64);
		boxed[0] = a;
		boxed[1] = b;
		std_box_slice_into_vec_Checked(&frame->right, boxed, 2, sizeof(ChunksAddressAddress));
		frame->right_live = true;
	}
	STD_ASSERT_EQ(329, 9, chunks_record_tests_VecAddress_PartialEq_eq(&frame->left, &frame->right), &frame->left,
				  chunks_record_tests_VecAddress_Debug_fmt, &frame->right, chunks_record_tests_VecAddress_Debug_fmt);
	frame->right_live = false;
	std_vec_free_Checked(&frame->right, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	frame->left_live = false;
	std_vec_free_Checked(&frame->left, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	frame->stamps = chunks_trailer_Stamps_Default_default();
	frame->stamps_live = true;
	chunks_chunk_Chunk_stored(frame->hashed.ptr, frame->hashed.len, frame->locators.ptr, frame->locators.len,
							  &frame->stamps, &frame->stored);
	frame->stored_live = true;
	frame->stamps_live = false;
	chunks_trailer_Stamps_drop(&frame->stamps);
	if (!chunks_record_Record_parse(&frame->stored, &frame->record, &error))
	{
		frame->error = error;
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(331, 45, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->record_live = true;
	STD_ASSERT_EQ(332, 9, frame->record.kind == kind, &frame->record.kind, std_u8_Debug_fmt, &kind,
				  std_u8_Debug_fmt);
	field = chunks_record_Record_field(&frame->record, 1);
	if (field == NULL)
		std_option_unwrap_failed(RUST_FILE, 333, 40);
	if (!chunks_record_int(field, &number, &error))
	{
		frame->error = error;
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(333, 50, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	STD_ASSERT_EQ(333, 9, number == minus_five, &number, std_i64_Debug_fmt, &minus_five, std_i64_Debug_fmt);
	field = chunks_record_Record_field(&frame->record, 2);
	if (field == NULL)
		std_option_unwrap_failed(RUST_FILE, 334, 53);
	if (!chunks_record_Record_addresses(&frame->record, field, &frame->left_entries, &error))
	{
		frame->error = error;
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(334, 63, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->left_entries_live = true;
	{
		chunks_TupleAddressLocator_t *boxed =
			std_box_new_uninit_layout_Checked(2 * sizeof(chunks_TupleAddressLocator_t),
											  _Alignof(chunks_TupleAddressLocator_t));

		STD_CHECK_ALIGNED(boxed, _Alignof(chunks_TupleAddressLocator_t), 334, 73);
		STD_CHECK_NULL_DEREF(boxed, 334, 73);
		boxed[0].f0 = a;
		boxed[0].f1 = la;
		boxed[1].f0 = b;
		boxed[1].f1 = unknown;
		std_box_slice_into_vec_Checked(&frame->right_entries, boxed, 2, sizeof(chunks_TupleAddressLocator_t));
		frame->right_entries_live = true;
	}
	STD_ASSERT_EQ(334, 9, chunks_record_tests_VecTupleAddressLocator_PartialEq_eq(&frame->left_entries,
																				  &frame->right_entries),
				  &frame->left_entries, chunks_record_tests_VecTupleAddressLocator_Debug_fmt, &frame->right_entries,
				  chunks_record_tests_VecTupleAddressLocator_Debug_fmt);
	frame->right_entries_live = false;
	std_vec_free_Checked(&frame->right_entries, CHUNKS_RECORD_ENTRY);
	frame->left_entries_live = false;
	std_vec_free_Checked(&frame->left_entries, CHUNKS_RECORD_ENTRY);
	field = chunks_record_Record_field(&frame->record, 3);
	if (field == NULL)
		std_option_unwrap_failed(RUST_FILE, 335, 36);
	{
		StdBytes	got = {field->bytes, field->bytes_len};
		StdBytes	want = {x, sizeof(x)};

		STD_ASSERT_EQ(335, 9, got.len == want.len && memcmp(got.ptr, want.ptr, want.len) == 0, &got,
					  std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt);
	}

	frame->inner = chunks_record_RecordWriter_new();
	frame->inner_live = true;
	{
		chunks_TupleAddressLocator_t pairs[1] = {{b, la}};

		chunks_record_RecordWriter_addresses(&frame->inner, 1, pairs, 1);
	}
	frame->item = chunks_record_RecordWriter_new();
	frame->item_live = true;
	{
		chunks_TupleAddressLocator_t pairs[1] = {{a, la}};

		chunks_record_RecordWriter_addresses(chunks_record_RecordWriter_uint(&frame->item, 1, 7), 2, pairs, 1);
	}
	frame->outer = chunks_record_RecordWriter_new();
	frame->outer_live = true;
	chunks_record_RecordWriter_field(&frame->outer, 1, head, sizeof(head));
	frame->inner_live = false;
	chunks_record_RecordWriter_record(&frame->outer, 2, &frame->inner);
	{
		ChunksRecordRecordWriter *boxed = std_box_new_uninit_layout_Checked(2 * sizeof(ChunksRecordRecordWriter),
																			_Alignof(ChunksRecordRecordWriter));

		STD_CHECK_ALIGNED(boxed, _Alignof(ChunksRecordRecordWriter), 342, 58);
		STD_CHECK_NULL_DEREF(boxed, 342, 58);
		boxed[0] = chunks_record_RecordWriter_new();
		frame->item_live = false;
		boxed[1] = frame->item;
		std_box_slice_into_vec_Checked(&frame->writers, boxed, 2, sizeof(ChunksRecordRecordWriter));
	}
	chunks_record_RecordWriter_list(&frame->outer, 3, &frame->writers);
	chunks_record_RecordWriter_finish_value(&frame->outer, &frame->bytes, &frame->addresses);
	frame->bytes_live = true;
	frame->addresses_live = true;
	if (!chunks_record_Record_value(frame->bytes.ptr, frame->bytes.len, frame->addresses.ptr, frame->addresses.len,
									&frame->value, &error))
	{
		frame->error = error;
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(344, 56, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->value_live = true;
	field = chunks_record_Record_field(&frame->value, 2);
	if (field == NULL)
		std_option_unwrap_failed(RUST_FILE, 345, 50);
	if (!chunks_record_Record_nested(&frame->value, field, &frame->nested, &error))
	{
		frame->error = error;
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(345, 60, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->nested_live = true;
	if (!chunks_record_Record_address(&frame->nested, 1, &has, &found, &found_locator, &error))
	{
		frame->error = error;
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(345, 80, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	{
		chunks_TupleAddressLocator_t got = {found, found_locator};
		chunks_TupleAddressLocator_t want = {b, la};
		StdOptionRef got_ref = {has, STD_ITEM(&got, chunks_record_tests_TupleAddressLocator_Debug_fmt)};
		StdOptionRef want_ref = {true, STD_ITEM(&want, chunks_record_tests_TupleAddressLocator_Debug_fmt)};

		STD_ASSERT_EQ(345, 9, has && chunks_address_Address_PartialEq_eq(&got.f0, &want.f0) &&
					  chunks_trailer_Locator_PartialEq_eq(&got.f1, &want.f1), &got_ref,
					  std_option_Debug_fmt, &want_ref, std_option_Debug_fmt);
	}
	frame->nested_live = false;
	chunks_record_Record_drop(&frame->nested);
	field = chunks_record_Record_field(&frame->value, 3);
	if (field == NULL)
		std_option_unwrap_failed(RUST_FILE, 346, 49);
	if (!chunks_record_Record_list(&frame->value, field, &frame->items, &error))
	{
		frame->error = error;
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(346, 59, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->items_live = true;
	len = frame->items.len;
	STD_ASSERT_EQ(347, 9, len == two, &len, std_usize_Debug_fmt, &two, std_usize_Debug_fmt);
	std_index_check(0, frame->items.len, RUST_FILE, 348, 17);
	STD_ASSERT(348, 9, STD_VEC_AT(&frame->items, ChunksRecordRecord, 0).fields.len == 0,
			   "items[0].fields.is_empty()");
	std_index_check(1, frame->items.len, RUST_FILE, 349, 21);
	if (!chunks_record_Record_uint(&STD_VEC_AT(&frame->items, ChunksRecordRecord, 1), 1, &seven, &error))
	{
		frame->error = error;
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(349, 38, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	std_index_check(1, frame->items.len, RUST_FILE, 349, 48);
	if (!chunks_record_Record_address(&STD_VEC_AT(&frame->items, ChunksRecordRecord, 1), 2, &has, &found,
									  &found_locator, &error))
	{
		frame->error = error;
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(349, 68, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	{
		chunks_TupleAddressLocator_t got = {found, found_locator};
		chunks_TupleAddressLocator_t want = {a, la};
		StdOptionRef got_ref = {has, STD_ITEM(&got, chunks_record_tests_TupleAddressLocator_Debug_fmt)};
		StdOptionRef want_ref = {true, STD_ITEM(&want, chunks_record_tests_TupleAddressLocator_Debug_fmt)};

		STD_ASSERT_EQ(349, 9, seven == want_seven && has && chunks_address_Address_PartialEq_eq(&got.f0, &want.f0) &&
					  chunks_trailer_Locator_PartialEq_eq(&got.f1, &want.f1),
					  &STD_TUPLE(2, STD_ITEM(&seven, std_u64_Debug_fmt), STD_ITEM(&got_ref, std_option_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&want_seven, std_u64_Debug_fmt),
								 STD_ITEM(&want_ref, std_option_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	for (size_t start = 0, n; std_array_IntoIter_next(&start, 5, 5, &n);)
	{
		int64		v = values[n];

		frame->each = chunks_record_RecordWriter_new();
		frame->each_live = true;
		frame->each_bytes = chunks_record_RecordWriter_finish(chunks_record_RecordWriter_int(&frame->each, 1, v));
		frame->each_bytes_live = true;
		frame->each_live = false;
		chunks_record_RecordWriter_drop(&frame->each);
		if (!chunks_record_fields(frame->each_bytes.ptr, frame->each_bytes.len, &frame->each_fields, &error))
		{
			frame->error = error;
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(352, 44, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		frame->each_fields_live = true;
		std_index_check(0, frame->each_fields.len, RUST_FILE, 352, 29);
		if (!chunks_record_int(frame->each_fields.ptr, &number, &error))
		{
			frame->error = error;
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(352, 57, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		frame->each_fields_live = false;
		std_vec_free_Checked(&frame->each_fields, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
		STD_ASSERT_EQ(352, 13, number == v, &number, std_i64_Debug_fmt, &v, std_i64_Debug_fmt);
		frame->each_bytes_live = false;
		std_vec_free_Checked(&frame->each_bytes, 1, 1);
	}
	frame->items_live = false;
	std_vec_drop_Checked(&frame->items, sizeof(ChunksRecordRecord), _Alignof(ChunksRecordRecord),
						 chunks_record_Record_drop);
	frame->value_live = false;
	chunks_record_Record_drop(&frame->value);
	frame->addresses_live = false;
	std_vec_free_Checked(&frame->addresses, CHUNKS_RECORD_PLACED);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	frame->outer_live = false;
	chunks_record_RecordWriter_drop(&frame->outer);
	frame->record_live = false;
	chunks_record_Record_drop(&frame->record);
	frame->stored_live = false;
	chunks_chunk_Chunk_drop(&frame->stored);
	frame->locators_live = false;
	std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	frame->hashed_live = false;
	std_vec_free_Checked(&frame->hashed, 1, 1);
	frame->writer_live = false;
	chunks_record_RecordWriter_drop(&frame->writer);
}

/* test_chunks_record_tests_records_hold_addresses_and_signed_integers_pad_0 is the test's pad. */
static void
test_chunks_record_tests_records_hold_addresses_and_signed_integers_pad_0(void *frame_)
{
	test_chunks_record_tests_records_hold_addresses_and_signed_integers_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->each_fields_live)
		std_vec_free_Checked(&frame->each_fields, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	if (frame->each_bytes_live)
		std_vec_free_Checked(&frame->each_bytes, 1, 1);
	if (frame->each_live)
		chunks_record_RecordWriter_drop(&frame->each);
	if (frame->nested_live)
		chunks_record_Record_drop(&frame->nested);
	if (frame->right_entries_live)
		std_vec_free_Checked(&frame->right_entries, CHUNKS_RECORD_ENTRY);
	if (frame->left_entries_live)
		std_vec_free_Checked(&frame->left_entries, CHUNKS_RECORD_ENTRY);
	if (frame->right_live)
		std_vec_free_Checked(&frame->right, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (frame->left_live)
		std_vec_free_Checked(&frame->left, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (frame->items_live)
		std_vec_drop_Checked(&frame->items, sizeof(ChunksRecordRecord), _Alignof(ChunksRecordRecord),
							 chunks_record_Record_drop);
	if (frame->value_live)
		chunks_record_Record_drop(&frame->value);
	if (frame->addresses_live)
		std_vec_free_Checked(&frame->addresses, CHUNKS_RECORD_PLACED);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->outer_live)
		chunks_record_RecordWriter_drop(&frame->outer);
	if (frame->item_live)
		chunks_record_RecordWriter_drop(&frame->item);
	if (frame->inner_live)
		chunks_record_RecordWriter_drop(&frame->inner);
	if (frame->record_live)
		chunks_record_Record_drop(&frame->record);
	if (frame->stored_live)
		chunks_chunk_Chunk_drop(&frame->stored);
	if (frame->stamps_live)
		chunks_trailer_Stamps_drop(&frame->stamps);
	if (frame->locators_live)
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	if (frame->hashed_live)
		std_vec_free_Checked(&frame->hashed, 1, 1);
	if (frame->writer_live)
		chunks_record_RecordWriter_drop(&frame->writer);
}

/*
 * test_chunks_record_tests_records_hold_addresses_and_signed_integers: a
 * record's chunk lists the addresses its fields hold with their locators, a
 * nested record and a list of records keep theirs, and signed integers read
 * back as written.
 */
void
test_chunks_record_tests_records_hold_addresses_and_signed_integers(void)
{
	test_chunks_record_tests_records_hold_addresses_and_signed_integers_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_record_tests_records_hold_addresses_and_signed_integers_body_0,
				test_chunks_record_tests_records_hold_addresses_and_signed_integers_pad_0);
}

/*
 * test_chunks_record_tests_rejects_bad_records_frame_0_t is the test's state
 * across its pad: the writers, the bytes, and each read's result.
 */
typedef struct test_chunks_record_tests_rejects_bad_records_frame_0_t
{
	ChunksRecordRecordWriter writer;
	bool		writer_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		more;
	bool		more_live;
	bool		read_ok;
	StdVec		read;
	ChunksErrorError error;
	bool		read_live;
} test_chunks_record_tests_rejects_bad_records_frame_0_t;

/* test_chunks_record_tests_rejects_bad_records_body_0 is the test's body. */
static void
test_chunks_record_tests_rejects_bad_records_body_0(void *frame_)
{
	test_chunks_record_tests_rejects_bad_records_frame_0_t *frame = frame_;
	static const uint8 a[] = {'a'};
	static const uint8 b[] = {'b'};
	static const uint8 bad[] = {4, 5, 1};
	bool		ok;

	frame->writer = chunks_record_RecordWriter_new();
	frame->writer_live = true;
	frame->bytes = chunks_record_RecordWriter_finish(chunks_record_RecordWriter_field(&frame->writer, 2, a, 1));
	frame->bytes_live = true;
	frame->writer_live = false;
	chunks_record_RecordWriter_drop(&frame->writer);
	frame->writer = chunks_record_RecordWriter_new();
	frame->writer_live = true;
	frame->more = chunks_record_RecordWriter_finish(chunks_record_RecordWriter_field(&frame->writer, 1, b, 1));
	frame->more_live = true;
	std_vec_reserve_Checked(&frame->bytes, frame->more.len, 1, 1);
	memcpy(STD_PTR_AT(frame->bytes.ptr, frame->bytes.len, 1), frame->more.ptr, frame->more.len);
	frame->bytes.len = std_wrapping_add_usize(frame->bytes.len, frame->more.len);
	frame->more_live = false;
	std_vec_free_Checked(&frame->more, 1, 1);
	frame->writer_live = false;
	chunks_record_RecordWriter_drop(&frame->writer);
	frame->read_ok = chunks_record_fields(frame->bytes.ptr, frame->bytes.len, &frame->read, &frame->error);
	frame->read_live = true;
	ok = !frame->read_ok;
	frame->read_live = false;
	if (frame->read_ok)
		std_vec_free_Checked(&frame->read, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	else
		chunks_error_Error_drop(&frame->error);
	STD_ASSERT(360, 9, ok, "fields(&bytes).is_err()");
	frame->read_ok = chunks_record_fields(bad, sizeof(bad), &frame->read, &frame->error);
	frame->read_live = true;
	ok = !frame->read_ok;
	frame->read_live = false;
	if (frame->read_ok)
		std_vec_free_Checked(&frame->read, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	else
		chunks_error_Error_drop(&frame->error);
	STD_ASSERT(361, 9, ok, "fields(&[4, 5, 1]).is_err()");
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
}

/* test_chunks_record_tests_rejects_bad_records_pad_0 is the test's pad. */
static void
test_chunks_record_tests_rejects_bad_records_pad_0(void *frame_)
{
	test_chunks_record_tests_rejects_bad_records_frame_0_t *frame = frame_;

	if (frame->read_live)
	{
		if (frame->read_ok)
			std_vec_free_Checked(&frame->read, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
		else
			chunks_error_Error_drop(&frame->error);
	}
	if (frame->more_live)
		std_vec_free_Checked(&frame->more, 1, 1);
	if (frame->writer_live)
		chunks_record_RecordWriter_drop(&frame->writer);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
}

/*
 * test_chunks_record_tests_rejects_bad_records: fields out of order and a
 * field that runs past its record's end are refused.
 */
void
test_chunks_record_tests_rejects_bad_records(void)
{
	test_chunks_record_tests_rejects_bad_records_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_record_tests_rejects_bad_records_body_0,
				test_chunks_record_tests_rejects_bad_records_pad_0);
}

/*
 * test_chunks_record_tests_rejects_addresses_out_of_order_closure_0_frame_0_t
 * is the test's parse's state across its pad: the record's body it
 * captures, the offsets it takes, its result, and the header, hashed bytes,
 * stamps, and stored chunk it holds.
 */
typedef struct test_chunks_record_tests_rejects_addresses_out_of_order_closure_0_frame_0_t
{
	const StdVec *body;
	StdVec		offsets;
	bool		offsets_live;
	ChunksErrorError *error;
	ChunksHeaderHeader header;
	bool		header_live;
	StdVec		hashed;
	bool		hashed_live;
	ChunksTrailerStamps stamps;
	bool		stamps_live;
	ChunksChunkChunk stored;
	bool		stored_live;
	bool		ok;
} test_chunks_record_tests_rejects_addresses_out_of_order_closure_0_frame_0_t;

/* test_chunks_record_tests_rejects_addresses_out_of_order_closure_0_body_0 is the parse's body. */
static void
test_chunks_record_tests_rejects_addresses_out_of_order_closure_0_body_0(void *frame_)
{
	test_chunks_record_tests_rejects_addresses_out_of_order_closure_0_frame_0_t *frame = frame_;
	ChunksTrailerLocator locators[2] = {{0}};
	ChunksRecordRecord record;

	frame->header.kind = CHUNKS_HEADER_FIRST_RECORD;
	frame->header.flags = 0;
	frame->header.addresses.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS;
	frame->offsets_live = false;
	frame->header.addresses.u.offsets = frame->offsets;
	frame->header_live = true;
	chunks_header_Header_write(&frame->header, frame->body->ptr, frame->body->len, &frame->hashed);
	frame->hashed_live = true;
	frame->stamps = chunks_trailer_Stamps_Default_default();
	frame->stamps_live = true;
	chunks_chunk_Chunk_stored(frame->hashed.ptr, frame->hashed.len, locators, 2, &frame->stamps, &frame->stored);
	frame->stored_live = true;
	frame->stamps_live = false;
	chunks_trailer_Stamps_drop(&frame->stamps);
	frame->hashed_live = false;
	std_vec_free_Checked(&frame->hashed, 1, 1);
	frame->ok = chunks_record_Record_parse(&frame->stored, &record, frame->error);
	if (frame->ok)
		chunks_record_Record_drop(&record);
	frame->stored_live = false;
	chunks_chunk_Chunk_drop(&frame->stored);
	frame->header_live = false;
	chunks_header_Header_drop(&frame->header);
}

/* test_chunks_record_tests_rejects_addresses_out_of_order_closure_0_pad_0 is the parse's pad. */
static void
test_chunks_record_tests_rejects_addresses_out_of_order_closure_0_pad_0(void *frame_)
{
	test_chunks_record_tests_rejects_addresses_out_of_order_closure_0_frame_0_t *frame = frame_;

	if (frame->stored_live)
		chunks_chunk_Chunk_drop(&frame->stored);
	if (frame->stamps_live)
		chunks_trailer_Stamps_drop(&frame->stamps);
	if (frame->hashed_live)
		std_vec_free_Checked(&frame->hashed, 1, 1);
	if (frame->header_live)
		chunks_header_Header_drop(&frame->header);
	if (frame->offsets_live)
		std_vec_free_Checked(&frame->offsets, sizeof(uint32), _Alignof(uint32));
}

/*
 * test_chunks_record_tests_rejects_addresses_out_of_order_closure_0 is the
 * test's parse, |offsets: Vec<u32>|, which takes the offsets: whether a
 * record whose address table lists them parses, with Record's map(drop).
 */
static bool
test_chunks_record_tests_rejects_addresses_out_of_order_closure_0(void *captures, StdVec *offsets,
																  ChunksErrorError *error)
{
	test_chunks_record_tests_rejects_addresses_out_of_order_closure_0_frame_0_t frame = {.body = captures,
	.offsets = *offsets, .offsets_live = true, .error = error};

	std_landing(&frame, test_chunks_record_tests_rejects_addresses_out_of_order_closure_0_body_0,
				test_chunks_record_tests_rejects_addresses_out_of_order_closure_0_pad_0);
	return frame.ok;
}

/*
 * test_chunks_record_tests_rejects_addresses_out_of_order_frame_0_t is the
 * test's state across its pad: the writer, the record's body, and the error
 * a parse's result holds.
 */
typedef struct test_chunks_record_tests_rejects_addresses_out_of_order_frame_0_t
{
	ChunksRecordRecordWriter writer;
	bool		writer_live;
	StdVec		body;
	bool		body_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_record_tests_rejects_addresses_out_of_order_frame_0_t;

/* test_chunks_record_tests_rejects_addresses_out_of_order_body_0 is the test's body. */
static void
test_chunks_record_tests_rejects_addresses_out_of_order_body_0(void *frame_)
{
	test_chunks_record_tests_rejects_addresses_out_of_order_frame_0_t *frame = frame_;
	static const uint8 zeros[64] = {0};
	StdVec		offsets;
	uint32	   *boxed;
	bool		ok;

	frame->writer = chunks_record_RecordWriter_new();
	frame->writer_live = true;
	frame->body = chunks_record_RecordWriter_finish(chunks_record_RecordWriter_field(&frame->writer, 1, zeros, 64));
	frame->body_live = true;
	frame->writer_live = false;
	chunks_record_RecordWriter_drop(&frame->writer);
	boxed = std_box_new_uninit_layout_Checked(2 * sizeof(uint32), _Alignof(uint32));
	STD_CHECK_ALIGNED(boxed, _Alignof(uint32), 372, 23);
	STD_CHECK_NULL_DEREF(boxed, 372, 23);
	boxed[0] = 2;
	boxed[1] = 34;
	std_box_slice_into_vec_Checked(&offsets, boxed, 2, sizeof(uint32));
	ok = test_chunks_record_tests_rejects_addresses_out_of_order_closure_0(&frame->body, &offsets, &frame->error);
	frame->error_live = !ok;
	if (frame->error_live)
	{
		frame->error_live = false;
		chunks_error_Error_drop(&frame->error);
	}
	STD_ASSERT(372, 9, ok, "parse(vec![2, 34]).is_ok()");
	boxed = std_box_new_uninit_layout_Checked(2 * sizeof(uint32), _Alignof(uint32));
	STD_CHECK_ALIGNED(boxed, _Alignof(uint32), 373, 23);
	STD_CHECK_NULL_DEREF(boxed, 373, 23);
	boxed[0] = 34;
	boxed[1] = 2;
	std_box_slice_into_vec_Checked(&offsets, boxed, 2, sizeof(uint32));
	ok = test_chunks_record_tests_rejects_addresses_out_of_order_closure_0(&frame->body, &offsets, &frame->error);
	frame->error_live = !ok;
	if (frame->error_live)
	{
		frame->error_live = false;
		chunks_error_Error_drop(&frame->error);
	}
	STD_ASSERT(373, 9, !ok, "parse(vec![34, 2]).is_err()");
	boxed = std_box_new_uninit_layout_Checked(2 * sizeof(uint32), _Alignof(uint32));
	STD_CHECK_ALIGNED(boxed, _Alignof(uint32), 374, 23);
	STD_CHECK_NULL_DEREF(boxed, 374, 23);
	boxed[0] = 2;
	boxed[1] = 20;
	std_box_slice_into_vec_Checked(&offsets, boxed, 2, sizeof(uint32));
	ok = test_chunks_record_tests_rejects_addresses_out_of_order_closure_0(&frame->body, &offsets, &frame->error);
	frame->error_live = !ok;
	if (frame->error_live)
	{
		frame->error_live = false;
		chunks_error_Error_drop(&frame->error);
	}
	STD_ASSERT(374, 9, !ok, "parse(vec![2, 20]).is_err()");
	frame->body_live = false;
	std_vec_free_Checked(&frame->body, 1, 1);
}

/* test_chunks_record_tests_rejects_addresses_out_of_order_pad_0 is the test's pad. */
static void
test_chunks_record_tests_rejects_addresses_out_of_order_pad_0(void *frame_)
{
	test_chunks_record_tests_rejects_addresses_out_of_order_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->writer_live)
		chunks_record_RecordWriter_drop(&frame->writer);
	if (frame->body_live)
		std_vec_free_Checked(&frame->body, 1, 1);
}

/*
 * test_chunks_record_tests_rejects_addresses_out_of_order: a record whose
 * address table lists offsets out of order, or addresses that overlap, is
 * refused at its parse.
 */
void
test_chunks_record_tests_rejects_addresses_out_of_order(void)
{
	test_chunks_record_tests_rejects_addresses_out_of_order_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_record_tests_rejects_addresses_out_of_order_body_0,
				test_chunks_record_tests_rejects_addresses_out_of_order_pad_0);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
